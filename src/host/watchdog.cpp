// mcla-x86 host: guest hang watchdog. See watchdog.h.
//
// Port of the VIGILANTE DE CUELGUES of the iOS port ($IOS common/src/nfsmw_app.h, section 4): same signature, same
// dump. Written for MCLA only: no Most Wanted job-queue lines, no semaphore nudge, no semaphore trace ring.
#include "watchdog.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>

// mcla/hooks/nfsc_progress.h (GAME slice): the frame heartbeat the game's per-frame hook bumps.
#include "nfsc_progress.h"
#include "system_status.h"

namespace mcla_host {

namespace {

// Seconds in a row with nothing moving before the alarm: before the first frame beat of the process (legal screens and
// blocking loads have none, and the hooked waits park threads) and after it. The MCLA branch of the iOS watchdog.
constexpr int kSecondsStillBeforeFirstBeat = 90;
constexpr int kSecondsStillAfterFirstBeat = 5;
constexpr int kSecondsBetweenDumps = 15;
constexpr int kSecondsBetweenSnapshots = 10;
// One INFO line of the frame heartbeat this often: run_game.py reads it as "heartbeat advancing" (the engine hook logs
// "[mcla-heartbeat] first engine frame" only once). The format is a contract: `[mcla-heartbeat] frames=<n>`.
constexpr int kSecondsBetweenHeartbeatLines = 10;

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

// Copies guest-visible memory that may be unmapped or decommitted. ReadProcessMemory on the current process is a
// kernel-side copy: it FAILS (false) on a bad page instead of faulting, so it is atomic with respect to another thread
// decommitting the page and never reaches the SDK's vectored exception handlers (MMIO, write watch), which must not see
// a probe. A __try would do neither (and in a TU built without -fasync-exceptions may not catch a fault in an inlined
// load). Same approach as mcla/hooks/mcla_guest.h.
bool SafeCopy(void* destination, const void* source, size_t size) {
  SIZE_T copied = 0;
  return ReadProcessMemory(GetCurrentProcess(), source, destination, size, &copied) != FALSE && copied == size;
}

// Big-endian 32-bit word of guest memory at `address`. False if the host page is not readable.
bool ReadGuestU32(rex::memory::Memory* memory, uint32_t address, uint32_t* out) {
  const auto* host = memory->TranslateVirtual<const uint8_t*>(address);
  uint32_t raw = 0;
  if (!host || !SafeCopy(&raw, host, sizeof(raw))) {
    return false;
  }
  *out = __builtin_bswap32(raw);
  return true;
}

const char* TypeName(rex::system::XObject::Type type) {
  using Type = rex::system::XObject::Type;
  switch (type) {
    case Type::Event:
      return "event";
    case Type::Semaphore:
      return "semaphore";
    case Type::Mutant:
      return "mutex";
    case Type::Timer:
      return "timer";
    case Type::Thread:
      return "thread";
    case Type::File:
      return "file";
    case Type::IOCompletion:
      return "io completion";
    case Type::NotifyListener:
      return "notify listener";
    default:
      return "object";
  }
}

// What is behind r3 of a stopped thread. In the kernel's wait functions r3 is either a handle (NtWaitForSingleObjectEx)
// or the guest pointer of the object (KeWaitForSingleObject), so both are tried. Whether a thread waits on an event, a
// semaphore or a mutex tells "the game waits for the disc" from "nobody is going to wake anybody".
std::string DescribeWait(uint32_t value) {
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !value) {
    return {};
  }
  using rex::system::XObject;
  auto describe = [&](const XObject& object, const char* how) {
    const std::string& name = object.name();
    return fmt::format(" waits on {} {}{}{} ({})", TypeName(object.type()), name.empty() ? "" : "'", name,
                       name.empty() ? "" : "'", how);
  };
  auto* table = kernel->object_table();
  if (auto object = table->LookupObject<XObject>(value)) {
    return describe(*object, "handle");
  }
  // By guest pointer: there is no reverse index, but this runs only when something is already stopped.
  for (XObject::Type type : {XObject::Type::Event, XObject::Type::Semaphore, XObject::Type::Mutant,
                             XObject::Type::Timer, XObject::Type::Thread}) {
    for (auto& object : table->GetObjectsByType<XObject>(type)) {
      if (object->guest_object() == value) {
        return describe(*object, "pointer");
      }
    }
  }
  return {};
}

// The guest stack of a stopped thread: return addresses from the current function upwards. Xbox 360 convention: the
// stack pointer points at the back chain (0(r1) = the caller's r1) and each function saves its LR at -8 from the
// caller's frame. Only inside THIS thread's stack, between its end and its base (PCR +0x74 and +0x70): the first
// version of the iOS watchdog walked up to the base of an unused, unmapped stack and got stuck on the fault. At most
// 16 frames; every read is fault-guarded.
std::string DescribeGuestStack(uint32_t pcr, uint32_t sp) {
  auto* kernel = rex::system::kernel_state();
  auto* memory = kernel ? kernel->memory() : nullptr;
  if (!memory || !pcr) {
    return {};
  }
  uint32_t stack_base = 0;
  uint32_t stack_end = 0;
  if (!ReadGuestU32(memory, pcr + 0x70, &stack_base) || !ReadGuestU32(memory, pcr + 0x74, &stack_end)) {
    return {};
  }
  if (stack_end >= stack_base || stack_base - stack_end > 0x1000000) {
    return {};
  }
  // `a` and `a - 8` must lie inside the stack: between its end and its base, aligned.
  auto in_stack = [&](uint32_t a) { return a >= stack_end + 8 && a + 4 <= stack_base && (a & 3) == 0; };
  std::string out;
  for (int frame = 0; frame < 16 && in_stack(sp); ++frame) {
    uint32_t caller_sp = 0;
    if (!ReadGuestU32(memory, sp, &caller_sp)) {
      break;
    }
    if (!in_stack(caller_sp) || caller_sp <= sp || caller_sp - sp > 0x40000) {
      break;
    }
    uint32_t saved_lr = 0;
    if (!ReadGuestU32(memory, caller_sp - 8, &saved_lr)) {
      break;
    }
    out += fmt::format(" <- 0x{:08X}", saved_lr);
    sp = caller_sp;
  }
  return out.empty() ? out : " | stack" + out;
}

// Dump of the guest thread table. `alarm` decides between ERROR lines (a real alarm: includes what each thread waits
// on and its stack) and DEBUG lines (the routine snapshots).
template <typename ThreadList>
void DumpThreads(const ThreadList& threads, bool alarm) {
  for (auto& thread : threads) {
    const auto* params = thread->creation_params();
    auto* state = thread->thread_state();
    if (!state || !state->context()) {
      REXLOG_DEBUG("[mcla-watchdog]   thread id=0x{:X} entry=0x{:08X} has no context", thread->thread_id(),
                   params->start_address);
      continue;
    }
    const auto& c = *state->context();
    if (alarm) {
      REXLOG_ERROR(
          "[mcla-watchdog]   thread id=0x{:X} entry=0x{:08X} main={} running={} | lr=0x{:08X} r1=0x{:08X} "
          "r13=0x{:08X} r3=0x{:08X} ctr=0x{:08X} last_indirect=0x{:08X}{}{}",
          thread->thread_id(), params->start_address, thread->main_thread(), thread->is_running(),
          static_cast<uint32_t>(c.lr), c.r1.u32, c.r13.u32, c.r3.u32, c.ctr.u32, c.last_indirect_target,
          DescribeWait(c.r3.u32), DescribeGuestStack(thread->pcr_ptr(), c.r1.u32));
    } else {
      REXLOG_DEBUG(
          "[mcla-watchdog]   thread id=0x{:X} entry=0x{:08X} main={} running={} | lr=0x{:08X} r1=0x{:08X} "
          "r13=0x{:08X} r3=0x{:08X} ctr=0x{:08X} last_indirect=0x{:08X}",
          thread->thread_id(), params->start_address, thread->main_thread(), thread->is_running(),
          static_cast<uint32_t>(c.lr), c.r1.u32, c.r13.u32, c.r3.u32, c.ctr.u32, c.last_indirect_target);
    }
  }
}

// Everything the sampling loop remembers between two seconds.
struct SampleState {
  uint64_t previous_signature = 0;
  uint64_t previous_register_signature = 0;
  uint64_t beats_when_registers_stalled = 0;
  int registers_still = 0;  // seconds the registers alone have been identical
  int still = 0;            // seconds the whole signature (registers + heartbeat) has been identical
  int since_last_dump = 0;
  int since_snapshot = 0;
  int since_heartbeat_line = 0;
  int since_system_sample = 0;
  bool alarmed = false;
};

// One pass of the loop (once a second).
void Sample(SampleState& s) {
  // The heartbeat line comes first and needs no kernel state: it is also written while the guest threads do not exist
  // yet (boot) or no longer do (shutdown), which tells a run that never started apart from one that stopped.
  if (++s.since_heartbeat_line >= kSecondsBetweenHeartbeatLines) {
    s.since_heartbeat_line = 0;
    REXLOG_INFO("[mcla-heartbeat] frames={}", nfsc::ProgressBeats());
  }
  if (++s.since_system_sample >= 30) {
    s.since_system_sample = 0;
    LogSystemSample();  // system_status.h: power, CPU clocks and limits, the process' CPU and memory
  }

  auto* kernel = rex::system::kernel_state();
  if (!kernel) {
    return;
  }
  auto threads = kernel->object_table()->GetObjectsByType<rex::system::XThread>();
  if (threads.empty()) {
    return;
  }

  // A signature of "where everybody is". It does not have to be a good hash, only change when any register changes.
  // The threads go through a map by thread id first: GetObjectsByType does not promise an order (the iOS dumps came out
  // shuffled from one pass to the next, and one id even twice), and a signature that changes by itself never raises
  // the alarm in a real hang. The map orders them and overwrites a repeated id instead of counting it twice.
  std::map<uint32_t, uint64_t> per_thread;
  for (auto& thread : threads) {
    auto* state = thread->thread_state();
    if (!state || !state->context()) {
      continue;
    }
    const auto& c = *state->context();
    per_thread[thread->thread_id()] = static_cast<uint64_t>(c.lr) ^ (static_cast<uint64_t>(c.r1.u32) << 20) ^
                                      (static_cast<uint64_t>(c.r3.u32) << 40);
  }
  uint64_t signature = kFnvOffset;
  for (const auto& [thread_id, fingerprint] : per_thread) {
    signature = (signature ^ thread_id) * kFnvPrime;
    signature = (signature ^ fingerprint) * kFnvPrime;
  }

  // The frame heartbeat (nfsc_progress.h). The registers alone are not enough: with the threads asleep in real waits
  // (the CPU hooks) and a game locked to a fixed rate, the once-a-second sample always lands on the same point of the
  // frame, and five identical samples are a false alarm while the game runs. A real hang stops the heartbeat too.
  const uint64_t beats = nfsc::ProgressBeats();
  // How long the registers alone have been identical although the game advanced: counts the false alarms the heartbeat
  // saves us (noted at info level when the stretch ends).
  if (signature == s.previous_register_signature) {
    ++s.registers_still;
  } else {
    if (s.registers_still >= kSecondsStillAfterFirstBeat && beats != s.beats_when_registers_stalled) {
      REXLOG_INFO(
          "[mcla-watchdog] false alarm avoided: the registers of the {} guest threads were identical for {} s (the "
          "once-a-second sample lands on the same point of the frame) but the game advanced {} frames.",
          threads.size(), s.registers_still, beats - s.beats_when_registers_stalled);
    }
    s.previous_register_signature = signature;
    s.registers_still = 0;
    s.beats_when_registers_stalled = beats;
  }
  signature = (signature ^ beats) * kFnvPrime;

  // A snapshot every ten seconds whatever happens, at debug level: when the game freezes in a way that keeps the
  // registers changing (a tight loop waiting for something that never comes looks stopped from outside and still
  // escapes the alarm), the last snapshots show which addresses lr cycles between.
  if (++s.since_snapshot >= kSecondsBetweenSnapshots) {
    s.since_snapshot = 0;
    REXLOG_DEBUG("[mcla-watchdog] snapshot: {} guest threads", threads.size());
    DumpThreads(threads, false);
  }

  if (signature != s.previous_signature) {
    if (s.alarmed) {
      REXLOG_WARN("[mcla-watchdog] the game moved again after {} s stopped.", s.still);
      s.alarmed = false;
    }
    s.previous_signature = signature;
    s.still = 0;
    s.since_last_dump = 0;
    return;
  }

  ++s.still;
  ++s.since_last_dump;
  // No frame beat until the engine timer's first frame: before it allow a long boot.
  if (s.still < (beats == 0 ? kSecondsStillBeforeFirstBeat : kSecondsStillAfterFirstBeat)) {
    return;
  }
  if (s.alarmed && s.since_last_dump < kSecondsBetweenDumps) {
    return;
  }
  s.since_last_dump = 0;

  REXLOG_ERROR(
      "[mcla-watchdog] {} s without a single register moving in any of the {} guest threads, and no frame heartbeat "
      "({}). This is not slowness: the game is stopped.",
      s.still, threads.size(), beats);
  DumpThreads(threads, true);
  rex::FlushLogging();
  s.alarmed = true;
}

}  // namespace

Watchdog::~Watchdog() {
  Stop();
}

void Watchdog::Start() {
  std::lock_guard lock(mutex_);
  if (running_) {
    return;
  }
  stop_ = false;
  running_ = true;
  thread_ = std::thread([this] { Main(); });
  worker_id_ = thread_.get_id();
}

void Watchdog::Stop() {
  std::thread worker;
  bool from_worker = false;
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    worker = std::move(thread_);  // whoever gets here first owns the join: two join() calls would terminate
    from_worker = std::this_thread::get_id() == worker_id_;
  }
  wake_.notify_all();
  if (from_worker) {
    // Called from the sampling thread itself (nothing does today): it cannot join itself.
    if (worker.joinable()) {
      worker.detach();
    }
    return;
  }
  if (worker.joinable()) {
    worker.join();
  }
  // A concurrent Stop() may own the thread and still be joining it: wait for Main to return as well, so that no caller
  // leaves (and goes on to tear the runtime down) while the watchdog is still sampling.
  std::unique_lock lock(mutex_);
  wake_.wait(lock, [this] { return !running_; });
}

void Watchdog::Main() {
  REXLOG_INFO("[mcla-watchdog] started: alarm after {} s without movement before the first frame beat, {} s after",
              kSecondsStillBeforeFirstBeat, kSecondsStillAfterFirstBeat);
  SampleState state;
  std::unique_lock lock(mutex_);
  while (!stop_) {
    if (wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_; })) {
      break;
    }
    lock.unlock();
    Sample(state);
    lock.lock();
  }
  lock.unlock();
  REXLOG_INFO("[mcla-watchdog] stopped");
  lock.lock();
  running_ = false;
  lock.unlock();
  wake_.notify_all();
}

}  // namespace mcla_host
