// mcla-x86 host: thread monitor and live thread policy. See thread_policy.h.
#include "thread_policy.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#ifdef MCLA_THREAD_POLICY_STANDALONE
// scratch/threadpol: the SDK pieces this file uses (REXLOG_*, REXCVAR_*) as a few macros, for the console test.
#include "thread_policy_stubs.h"
#else
#include <rex/cvar.h>
#include <rex/logging.h>
#endif

// ---- cvars -------------------------------------------------------------------------------------------------------
// All live: the monitor thread reads them again every tick. Everything defaults to off (no snapshot, no touching).

REXCVAR_DEFINE_INT32(mcla_thread_stats_s, 0, "MCLA",
                     "Thread monitor: every N seconds write '[mcla-threads]' lines with the CPU of the top 10 threads "
                     "over the interval, the context switches and the threads seen waiting for a core. 0 = off. Live")
    .range(0, 3600);
REXCVAR_DEFINE_INT32(mcla_thread_hot_min, 15, "MCLA",
                     "Thread policy: an 'XThread...' guest worker becomes a HOT thread once it averaged at least this "
                     "many percent of a core over the last 5 s (GPU Commands, GPU Submission and Main XThread are "
                     "always HOT). Live")
    .range(0, 1000);
REXCVAR_DEFINE_INT32(mcla_thread_hot_priority, 0, "MCLA",
                     "Thread policy, HOT threads: 0 = leave, 1 = ABOVE_NORMAL, 2 = HIGHEST thread priority. Going back "
                     "to 0 restores the priority they had. Live")
    .range(0, 2);
REXCVAR_DEFINE_INT32(mcla_thread_hot_cpus, 0, "MCLA",
                     "Thread policy, HOT threads: 0 = leave, 1 = CPU sets limited to the logical processors of the "
                     "highest efficiency class (the Zen 5 cores), 2 = 1 plus an ideal processor on distinct physical "
                     "cores of that class. Going back to 0 clears the sets. Live")
    .range(0, 2);
REXCVAR_DEFINE_INT32(mcla_thread_cold_cpus, 0, "MCLA",
                     "Thread policy, COLD threads (pipeline compilers, shader translation, storage writer, timer "
                     "queue): 0 = leave, 1 = CPU sets limited to the lowest efficiency class (the Zen 5c cores). Live")
    .range(0, 1);
REXCVAR_DEFINE_BOOL(mcla_thread_cold_ecoqos, false, "MCLA",
                    "Thread policy, COLD threads: run them in EcoQoS (power throttling, efficient clocks). Live");
REXCVAR_DEFINE_BOOL(mcla_thread_hot_ecoqos, false, "MCLA",
                    "Thread policy, HOT threads: run them in EcoQoS (efficient clocks). An A/B experiment on the shared "
                    "CPU/GPU power budget, expected to cost CPU-side time. Wins over mcla_thread_hot_noecoqos. Live");
REXCVAR_DEFINE_BOOL(mcla_thread_hot_noecoqos, false, "MCLA",
                    "Thread policy, HOT threads: opt them out of EcoQoS explicitly (never throttle). Live");
REXCVAR_DEFINE_INT32(mcla_process_priority, 0, "MCLA",
                     "Thread policy, whole process: 0 = leave, 1 = ABOVE_NORMAL, 2 = HIGH priority class. Going back "
                     "to 0 restores the class found. Note: thread priority HIGHEST inside the HIGH class is base 15. "
                     "Live")
    .range(0, 2);
REXCVAR_DEFINE_BOOL(mcla_process_noecoqos, false, "MCLA",
                    "Thread policy, whole process: opt the process out of EcoQoS, which Windows applies to background "
                    "or occluded windows (the other power-throttling bits, such as the timer-resolution opt-out, are "
                    "kept). Live");

namespace mcla_host {

// input_script.cpp: seconds since the process started (the clock of the logs and of the input schedule).
double SecondsSinceProcessStart();

namespace {

constexpr int kHotWindowTicks = 5;           // the "last 5 s" of the XThread CPU average
constexpr int kStatsTopThreads = 10;
constexpr size_t kMaxRetired = 64;           // threads that ended inside a stats interval, kept for the line
constexpr size_t kMaxSnapshotBytes = 64u << 20;
constexpr int kNameRetryTicks = 5;           // an unnamed thread is asked again for its name on each of its first ticks,
constexpr int kNameSlowRetryTicks = 15;      // and then on every 15th (a thread names itself after it starts)
constexpr int kNoValue = -1000;              // "no failed value remembered"

// ---- NtQuerySystemInformation (ntdll, resolved by GetProcAddress) -------------------------------------------------

constexpr ULONG kSystemProcessInformation = 5;
constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004);
constexpr ULONG kThreadStateReady = 1;

struct NtClientId {
  HANDLE UniqueProcess;
  HANDLE UniqueThread;
};

struct NtSystemThreadInformation {
  LARGE_INTEGER KernelTime;
  LARGE_INTEGER UserTime;
  LARGE_INTEGER CreateTime;
  ULONG WaitTime;
  PVOID StartAddress;
  NtClientId ClientId;
  LONG Priority;
  LONG BasePriority;
  ULONG ContextSwitches;
  ULONG ThreadState;
  ULONG WaitReason;
};
static_assert(sizeof(NtSystemThreadInformation) == 80, "SYSTEM_THREAD_INFORMATION layout (x64)");

// SYSTEM_PROCESS_INFORMATION without the trailing Threads[]: the thread array starts right after it.
struct NtSystemProcessInformation {
  ULONG NextEntryOffset;
  ULONG NumberOfThreads;
  LARGE_INTEGER WorkingSetPrivateSize;
  ULONG HardFaultCount;
  ULONG NumberOfThreadsHighWatermark;
  ULONGLONG CycleTime;
  LARGE_INTEGER CreateTime;
  LARGE_INTEGER UserTime;
  LARGE_INTEGER KernelTime;
  USHORT ImageNameLength;
  USHORT ImageNameMaximumLength;
  PWSTR ImageNameBuffer;
  LONG BasePriority;
  HANDLE UniqueProcessId;
  HANDLE InheritedFromUniqueProcessId;
  ULONG HandleCount;
  ULONG SessionId;
  ULONG_PTR UniqueProcessKey;
  SIZE_T PeakVirtualSize;
  SIZE_T VirtualSize;
  ULONG PageFaultCount;
  SIZE_T PeakWorkingSetSize;
  SIZE_T WorkingSetSize;
  SIZE_T QuotaPeakPagedPoolUsage;
  SIZE_T QuotaPagedPoolUsage;
  SIZE_T QuotaPeakNonPagedPoolUsage;
  SIZE_T QuotaNonPagedPoolUsage;
  SIZE_T PagefileUsage;
  SIZE_T PeakPagefileUsage;
  SIZE_T PrivatePageCount;
  LARGE_INTEGER ReadOperationCount;
  LARGE_INTEGER WriteOperationCount;
  LARGE_INTEGER OtherOperationCount;
  LARGE_INTEGER ReadTransferCount;
  LARGE_INTEGER WriteTransferCount;
  LARGE_INTEGER OtherTransferCount;
};
static_assert(sizeof(NtSystemProcessInformation) == 256, "SYSTEM_PROCESS_INFORMATION layout (x64)");
static_assert(offsetof(NtSystemProcessInformation, UniqueProcessId) == 80, "SYSTEM_PROCESS_INFORMATION layout (x64)");

// What one tick needs to know about one thread of this process.
struct ThreadSample {
  uint32_t tid = 0;
  uint64_t create_time = 0;  // 100 ns, tells a reused thread id from the thread we knew
  uint64_t cpu_100ns = 0;    // kernel + user time (tick-based; the fallback when cycles are not available)
  ULONG context_switches = 0;
  ULONG state = 0;
  ULONG wait_reason = 0;
  LONG priority = 0;
  LONG base_priority = 0;
};

// The threads of this process, with the kernel's own bookkeeping (state, priorities, context switches).
//
// Two ways to get them, same result:
//  * thread walk (preferred): NtGetNextThread over our own process plus NtQueryInformationThread(40 =
//    ThreadSystemThreadInformation) per thread, which returns the same SYSTEM_THREAD_INFORMATION record. About
//    two syscalls per thread: ~0.1 ms for the game's ~70 threads.
//  * NtQuerySystemInformation(SystemProcessInformation), the route everybody knows: it returns EVERY thread of
//    EVERY process (370 processes, 5600 threads on the development machine: ~16 ms per call, 1.6 % of a core at one
//    call a second). Only used when the walk is not available.
class SystemSnapshot {
 public:
  SystemSnapshot() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
      query_system_ = reinterpret_cast<QuerySystemFn>(
          reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQuerySystemInformation")));
      get_next_thread_ = reinterpret_cast<GetNextThreadFn>(
          reinterpret_cast<void*>(GetProcAddress(ntdll, "NtGetNextThread")));
      query_thread_ = reinterpret_cast<QueryThreadFn>(
          reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryInformationThread")));
    }
  }

  // Picks the way to snapshot (the walk must see the calling thread). Returns its name, "none" if nothing works.
  const char* Probe() {
    std::vector<ThreadSample> samples;
    LONG status = 0;
    bool walk_allowed = get_next_thread_ && query_thread_;
#ifdef MCLA_THREAD_POLICY_STANDALONE
    walk_allowed = walk_allowed && GetEnvironmentVariableW(L"MCLA_TP_FORCE_SYSTEM", nullptr, 0) == 0;  // test hook
#endif
    if (walk_allowed && TakeWalk(samples, &status) && Contains(samples, GetCurrentThreadId())) {
      mode_ = Mode::kThreadWalk;
    } else if (query_system_ && TakeSystem(samples, &status) && Contains(samples, GetCurrentThreadId())) {
      mode_ = Mode::kSystemInformation;
    } else {
      mode_ = Mode::kNone;
    }
    return mode_ == Mode::kThreadWalk ? "thread walk (NtGetNextThread)"
           : mode_ == Mode::kSystemInformation ? "NtQuerySystemInformation"
                                               : "none";
  }

  // The threads of this process. False (and *status set) when the snapshot fails.
  bool Take(std::vector<ThreadSample>& out, LONG* status) {
    out.clear();
    if (mode_ == Mode::kUnprobed) {
      Probe();
    }
    switch (mode_) {
      case Mode::kThreadWalk:
        if (TakeWalk(out, status)) {
          walk_failures_ = 0;
          return true;
        }
        // One bad pass is no reason to give up on the walk; three in a row are.
        if (++walk_failures_ >= 3 && query_system_) {
          mode_ = Mode::kSystemInformation;
        }
        return query_system_ && TakeSystem(out, status);
      case Mode::kSystemInformation:
        return TakeSystem(out, status);
      default:
        *status = static_cast<LONG>(0xC0000002);  // STATUS_NOT_IMPLEMENTED
        return false;
    }
  }

 private:
  using QuerySystemFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
  using GetNextThreadFn = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
  using QueryThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  enum class Mode { kUnprobed, kThreadWalk, kSystemInformation, kNone };

  static constexpr ULONG kThreadSystemThreadInformation = 40;
  static constexpr LONG kStatusNoMoreEntries = static_cast<LONG>(0x8000001A);
  static constexpr size_t kMaxWalkThreads = 8192;
  static constexpr ULONG kThreadStateTerminated = 4;

  static bool Contains(const std::vector<ThreadSample>& samples, uint32_t tid) {
    return std::any_of(samples.begin(), samples.end(), [&](const ThreadSample& s) { return s.tid == tid; });
  }

  static ThreadSample ToSample(const NtSystemThreadInformation& thread) {
    ThreadSample sample;
    sample.tid = static_cast<uint32_t>(reinterpret_cast<ULONG_PTR>(thread.ClientId.UniqueThread));
    sample.create_time = static_cast<uint64_t>(thread.CreateTime.QuadPart);
    sample.cpu_100ns =
        static_cast<uint64_t>(thread.KernelTime.QuadPart) + static_cast<uint64_t>(thread.UserTime.QuadPart);
    sample.context_switches = thread.ContextSwitches;
    sample.state = thread.ThreadState;
    sample.wait_reason = thread.WaitReason;
    sample.priority = thread.Priority;
    sample.base_priority = thread.BasePriority;
    return sample;
  }

  bool TakeWalk(std::vector<ThreadSample>& out, LONG* status) {
    out.clear();
    HANDLE current = nullptr;
    for (size_t i = 0; i < kMaxWalkThreads; ++i) {
      HANDLE next = nullptr;
      const LONG result = get_next_thread_(GetCurrentProcess(), current, THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &next);
      if (current) {
        CloseHandle(current);  // the cursor is needed for the call only
      }
      current = nullptr;
      if (result < 0) {
        *status = result == kStatusNoMoreEntries ? 0 : result;
        return result == kStatusNoMoreEntries;
      }
      current = next;
      NtSystemThreadInformation info;
      std::memset(&info, 0, sizeof(info));
      ULONG returned = 0;
      if (query_thread_(next, kThreadSystemThreadInformation, &info, sizeof(info), &returned) >= 0 &&
          returned >= sizeof(info) && info.ThreadState != kThreadStateTerminated) {
        out.push_back(ToSample(info));
      }
    }
    if (current) {
      CloseHandle(current);
    }
    *status = static_cast<LONG>(0xC0000001);  // implausibly many threads: give up on this pass
    return false;
  }

  bool TakeSystem(std::vector<ThreadSample>& out, LONG* status) {
    out.clear();
    if (!query_system_) {
      *status = static_cast<LONG>(0xC0000002);  // STATUS_NOT_IMPLEMENTED
      return false;
    }
    if (buffer_.empty()) {
      buffer_.resize(1u << 20);
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
      ULONG needed = 0;
      const LONG result =
          query_system_(kSystemProcessInformation, buffer_.data(), static_cast<ULONG>(buffer_.size()), &needed);
      if (result == kStatusInfoLengthMismatch) {
        // The process list can grow between the two calls: ask for more than reported.
        const size_t wanted = std::max<size_t>(needed, buffer_.size()) + (128u << 10);
        if (wanted > kMaxSnapshotBytes) {
          *status = result;
          return false;
        }
        buffer_.resize(wanted);
        continue;
      }
      if (result < 0) {
        *status = result;
        return false;
      }
      ParseSystem(needed ? std::min<size_t>(needed, buffer_.size()) : buffer_.size(), out);
      *status = 0;
      return true;
    }
    *status = kStatusInfoLengthMismatch;
    return false;
  }

  void ParseSystem(size_t valid, std::vector<ThreadSample>& out) const {
    const uint8_t* base = buffer_.data();
    const ULONG_PTR self = static_cast<ULONG_PTR>(GetCurrentProcessId());
    size_t offset = 0;
    while (offset + sizeof(NtSystemProcessInformation) <= valid) {
      NtSystemProcessInformation process;
      std::memcpy(&process, base + offset, sizeof(process));
      if (reinterpret_cast<ULONG_PTR>(process.UniqueProcessId) == self) {
        const size_t threads_at = offset + sizeof(NtSystemProcessInformation);
        const size_t count = std::min<size_t>(
            process.NumberOfThreads, (valid > threads_at ? valid - threads_at : 0) / sizeof(NtSystemThreadInformation));
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
          NtSystemThreadInformation thread;
          std::memcpy(&thread, base + threads_at + i * sizeof(NtSystemThreadInformation), sizeof(thread));
          if (thread.ThreadState != kThreadStateTerminated) {
            out.push_back(ToSample(thread));
          }
        }
        return;
      }
      if (process.NextEntryOffset == 0) {
        return;
      }
      offset += process.NextEntryOffset;
    }
  }

  QuerySystemFn query_system_ = nullptr;
  GetNextThreadFn get_next_thread_ = nullptr;
  QueryThreadFn query_thread_ = nullptr;
  Mode mode_ = Mode::kUnprobed;
  int walk_failures_ = 0;
  std::vector<uint8_t> buffer_;  // only the NtQuerySystemInformation way uses it (allocated and grown on demand)
};

// ---- small helpers -----------------------------------------------------------------------------------------------

double QpcSeconds() {
  static const double frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return static_cast<double>(c.QuadPart) / frequency;
}

std::string Utf8FromWide(const wchar_t* wide) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string utf8(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), size, nullptr, nullptr);
  return utf8;
}

// The name a thread gave itself (SetThreadDescription). Empty if it has none or the query fails.
std::string ReadThreadName(HANDLE thread) {
  PWSTR description = nullptr;
  if (FAILED(GetThreadDescription(thread, &description)) || !description) {
    return {};
  }
  std::string name = Utf8FromWide(description);
  LocalFree(description);
  return name;
}

// "GPU Commands (F8000018)" -> "GPU Commands": the guest handle suffix is dropped when it is 1-8 hex digits.
std::string ShortName(const std::string& name) {
  if (name.size() > 4 && name.back() == ')') {
    const size_t open = name.rfind(" (");
    if (open != std::string::npos && open > 0) {
      const size_t digits = name.size() - 1 - (open + 2);
      bool hex = digits >= 1 && digits <= 8;
      for (size_t i = open + 2; hex && i + 1 < name.size(); ++i) {
        const char c = name[i];
        hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
      }
      if (hex) {
        return name.substr(0, open);
      }
    }
  }
  return name;
}

bool StartsWithAny(std::string_view name, std::initializer_list<std::string_view> prefixes) {
  for (std::string_view prefix : prefixes) {
    if (name.starts_with(prefix)) {
      return true;
    }
  }
  return false;
}

// Sorted logical-processor numbers as "0-7" / "0-3,6".
std::string FormatRanges(std::vector<int> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  std::string out;
  for (size_t i = 0; i < values.size();) {
    size_t j = i;
    while (j + 1 < values.size() && values[j + 1] == values[j] + 1) {
      ++j;
    }
    if (!out.empty()) {
      out += ',';
    }
    out += (j == i) ? fmt::format("{}", values[i]) : fmt::format("{}-{}", values[i], values[j]);
    i = j + 1;
  }
  return out;
}

const char* PriorityClassName(DWORD priority_class) {
  switch (priority_class) {
    case IDLE_PRIORITY_CLASS:
      return "IDLE";
    case BELOW_NORMAL_PRIORITY_CLASS:
      return "BELOW_NORMAL";
    case NORMAL_PRIORITY_CLASS:
      return "NORMAL";
    case ABOVE_NORMAL_PRIORITY_CLASS:
      return "ABOVE_NORMAL";
    case HIGH_PRIORITY_CLASS:
      return "HIGH";
    case REALTIME_PRIORITY_CLASS:
      return "REALTIME";
    default:
      return "?";
  }
}

// ---- CPU topology ------------------------------------------------------------------------------------------------

struct CpuTopology {
  struct Processor {
    ULONG id = 0;
    WORD group = 0;
    BYTE index = 0;  // logical processor index inside its group
    BYTE core = 0;   // physical core index inside its group (SMT siblings share it)
    BYTE efficiency_class = 0;
  };

  std::vector<Processor> processors;
  int high_class = 0;
  int low_class = 0;
  bool heterogeneous = false;
  std::vector<ULONG> high_ids;  // CPU set ids of the highest efficiency class
  std::vector<ULONG> low_ids;   // CPU set ids of the lowest efficiency class
  std::vector<PROCESSOR_NUMBER> high_cores;  // first SMT sibling of each physical core of the highest class
  size_t low_core_count = 0;

  void Load() {
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    if (length == 0) {
      return;
    }
    std::vector<uint8_t> buffer(length);
    if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), length, &length,
                                    GetCurrentProcess(), 0)) {
      return;
    }
    for (size_t offset = 0; offset + 8 <= length;) {
      SYSTEM_CPU_SET_INFORMATION entry;
      std::memset(&entry, 0, sizeof(entry));
      std::memcpy(&entry, buffer.data() + offset, std::min<size_t>(sizeof(entry), length - offset));
      if (entry.Size == 0) {
        break;
      }
      if (entry.Type == CpuSetInformation) {
        Processor p;
        p.id = entry.CpuSet.Id;
        p.group = entry.CpuSet.Group;
        p.index = entry.CpuSet.LogicalProcessorIndex;
        p.core = entry.CpuSet.CoreIndex;
        p.efficiency_class = entry.CpuSet.EfficiencyClass;
        processors.push_back(p);
      }
      offset += entry.Size;
    }
    if (processors.empty()) {
      return;
    }
    high_class = low_class = processors.front().efficiency_class;
    for (const Processor& p : processors) {
      high_class = std::max<int>(high_class, p.efficiency_class);
      low_class = std::min<int>(low_class, p.efficiency_class);
    }
    heterogeneous = high_class != low_class;
    // One entry per physical core of the highest class: its first (lowest) logical processor.
    std::map<std::pair<WORD, BYTE>, Processor> first_of_core;
    std::map<std::pair<WORD, BYTE>, bool> low_cores;
    for (const Processor& p : processors) {
      if (p.efficiency_class == high_class) {
        high_ids.push_back(p.id);
        auto [it, inserted] = first_of_core.try_emplace({p.group, p.core}, p);
        if (!inserted && p.index < it->second.index) {
          it->second = p;
        }
      }
      if (p.efficiency_class == low_class) {
        low_ids.push_back(p.id);
        low_cores[{p.group, p.core}] = true;
      }
    }
    low_core_count = low_cores.size();
    for (const auto& [key, p] : first_of_core) {
      PROCESSOR_NUMBER number{};
      number.Group = p.group;
      number.Number = p.index;
      high_cores.push_back(number);
    }
  }

  // "class1=[0-7] class0=[8-19] (physical cores: class1=4 class0=6)", classes from the highest down.
  std::string Describe() const {
    if (processors.empty()) {
      return "unavailable";
    }
    std::map<int, std::vector<int>, std::greater<int>> by_class;
    std::map<int, std::map<std::pair<WORD, BYTE>, bool>, std::greater<int>> cores_by_class;
    for (const Processor& p : processors) {
      by_class[p.efficiency_class].push_back(p.group * 64 + p.index);
      cores_by_class[p.efficiency_class][{p.group, p.core}] = true;
    }
    std::string out;
    for (const auto& [klass, list] : by_class) {
      out += fmt::format("{}class{}=[{}]", out.empty() ? "" : " ", klass, FormatRanges(list));
    }
    out += " (physical cores:";
    for (const auto& [klass, cores] : cores_by_class) {
      out += fmt::format(" class{}={}", klass, cores.size());
    }
    out += ")";
    return out;
  }
};

// ---- roles and per-thread state ----------------------------------------------------------------------------------

enum class Role : uint8_t { kNone, kHot, kCold };  // kNone = never touched (NEVER, unnamed, or neutral)

constexpr std::initializer_list<std::string_view> kNeverPrefixes = {
    "GPU VSync", "GPU Display VBlank", "Audio Worker", "XMA Decoder", "SDLAudio", "SDLWASAPI", "SDLRawInput"};
constexpr std::initializer_list<std::string_view> kHotPrefixes = {"GPU Commands", "GPU Submission", "Main XThread"};
constexpr std::initializer_list<std::string_view> kColdPrefixes = {"D3D12 Pipelines", "Shader Translation",
                                                                   "D3D12 Storage writer",
                                                                   "rex::thread::TimerQueue"};
constexpr std::string_view kWorkerPrefix = "XThread";

// What the policy has changed on one thread, with what it needs to put it back.
struct Applied {
  // thread priority
  bool prio_active = false;
  int prio_orig = 0;
  int prio_set = 0;
  int prio_fail = kNoValue;
  // CPU sets: 0 none, 1 highest efficiency class, 2 lowest
  int cpuset_kind = 0;
  std::vector<ULONG> cpuset_orig;  // empty = the thread had none
  int cpuset_fail = kNoValue;
  // ideal processor (see ApplyPlacement: the original is written down before the first touch of sets or ideal)
  bool ideal_orig_valid = false;
  PROCESSOR_NUMBER ideal_orig{};
  bool ideal_active = false;
  PROCESSOR_NUMBER ideal_set{};
  int ideal_fail = kNoValue;  // slot (core number) that failed
  // power throttling: 0 none, 1 EcoQoS on, 2 opted out ("never throttle")
  int eco_kind = 0;
  THREAD_POWER_THROTTLING_STATE eco_orig{};
  int eco_fail = kNoValue;

  bool Any() const { return prio_active || cpuset_kind != 0 || ideal_active || eco_kind != 0; }
};

struct ThreadInfo {
  uint32_t tid = 0;
  uint64_t create_time = 0;
  HANDLE handle = nullptr;
  bool can_set = false;
  bool seen = false;

  std::string name;
  bool named = false;
  int name_checks = 0;
  int ticks_seen = 0;

  Role role = Role::kNone;
  int ideal_slot = -1;  // index into CpuTopology::high_cores, while hot_cpus = 2

  // sampling
  uint64_t last_cycles = 0;
  uint64_t last_cpu_100ns = 0;
  ULONG last_csw = 0;
  ULONG state = 0;
  LONG priority = 0;
  LONG base_priority = 0;
  std::array<double, kHotWindowTicks> hist_cpu{};  // CPU seconds of the last ticks (ring)
  std::array<double, kHotWindowTicks> hist_dt{};
  int hist_count = 0;
  int hist_pos = 0;

  // stats interval
  double acc_cpu = 0;
  uint64_t acc_csw = 0;

  Applied applied;
};

struct RetiredThread {
  std::string label;
  uint32_t tid = 0;
  double cpu = 0;
};

// The knobs, as read once per tick.
struct Knobs {
  int stats_s = 0;
  int hot_min = 15;
  int hot_priority = 0;
  int hot_cpus = 0;
  int cold_cpus = 0;
  bool cold_eco = false;
  bool hot_eco = false;
  bool hot_noeco = false;
  int process_priority = 0;
  bool process_noeco = false;

  // Nothing to monitor and nothing to apply.
  bool Idle() const {
    return stats_s == 0 && hot_priority == 0 && hot_cpus == 0 && cold_cpus == 0 && !cold_eco && !hot_eco &&
           !hot_noeco && process_priority == 0 && !process_noeco;
  }
};

Knobs ReadKnobs() {
  Knobs k;
  k.stats_s = std::clamp<int>(REXCVAR_GET(mcla_thread_stats_s), 0, 3600);
  k.hot_min = std::clamp<int>(REXCVAR_GET(mcla_thread_hot_min), 0, 1000);
  k.hot_priority = std::clamp<int>(REXCVAR_GET(mcla_thread_hot_priority), 0, 2);
  k.hot_cpus = std::clamp<int>(REXCVAR_GET(mcla_thread_hot_cpus), 0, 2);
  k.cold_cpus = std::clamp<int>(REXCVAR_GET(mcla_thread_cold_cpus), 0, 1);
  k.cold_eco = REXCVAR_GET(mcla_thread_cold_ecoqos);
  k.hot_eco = REXCVAR_GET(mcla_thread_hot_ecoqos);
  k.hot_noeco = REXCVAR_GET(mcla_thread_hot_noecoqos);
  k.process_priority = std::clamp<int>(REXCVAR_GET(mcla_process_priority), 0, 2);
  k.process_noeco = REXCVAR_GET(mcla_process_noecoqos);
  return k;
}

// One line per knob and tick: "[mcla-threads] t=12.3 policy hot_priority=2: 5 thread(s) (GPU Commands, ...)".
class PolicyLog {
 public:
  void Add(const std::string& label, bool restored, const ThreadInfo& thread) {
    Entry& entry = entries_[{label, restored}];
    if (std::find(entry.tids.begin(), entry.tids.end(), thread.tid) != entry.tids.end()) {
      return;  // CPU sets and ideal processor of one thread are one line
    }
    entry.tids.push_back(thread.tid);
    entry.names.push_back(ShortName(thread.named ? thread.name : fmt::format("tid{}", thread.tid)));
  }

  void Flush() {
    for (const auto& [key, entry] : entries_) {
      const std::vector<std::string>& names = entry.names;
      // Equal names (a pool) are folded into "name xN"; at most 8 distinct names are listed.
      std::vector<std::pair<std::string, int>> distinct;
      for (const std::string& name : names) {
        auto it = std::find_if(distinct.begin(), distinct.end(), [&](const auto& d) { return d.first == name; });
        if (it == distinct.end()) {
          distinct.emplace_back(name, 1);
        } else {
          ++it->second;
        }
      }
      std::string list;
      for (size_t i = 0; i < distinct.size() && i < 8; ++i) {
        list += fmt::format("{}{}", i ? ", " : "", distinct[i].first);
        if (distinct[i].second > 1) {
          list += fmt::format(" x{}", distinct[i].second);
        }
      }
      if (distinct.size() > 8) {
        list += ", ...";
      }
      REXLOG_INFO("[mcla-threads] t={:.1f} policy {}: {} thread(s){} ({})", SecondsSinceProcessStart(), key.first,
                  names.size(), key.second ? " restored" : "", list);
    }
    entries_.clear();
  }

 private:
  struct Entry {
    std::vector<uint32_t> tids;
    std::vector<std::string> names;
  };
  std::map<std::pair<std::string, bool>, Entry> entries_;
};

// ---- the monitor -------------------------------------------------------------------------------------------------

class Monitor {
 public:
  explicit Monitor(double tsc_hz) : tsc_hz_(tsc_hz), self_tid_(GetCurrentThreadId()) {}
  ~Monitor() { CloseAll(); }
  Monitor(const Monitor&) = delete;
  Monitor& operator=(const Monitor&) = delete;

  void LogStart() {
    topology_.Load();
    REXLOG_INFO("[mcla-threads] cpu sets: {}", topology_.Describe());
    REXLOG_INFO("[mcla-threads] started: thread snapshot via {}, cycle counter {:.3f} GHz; nothing is monitored or "
                "touched until mcla_thread_stats_s or a policy knob is set",
                snapshot_.Probe(), tsc_hz_ / 1e9);
  }

  // One pass (once a second).
  void Tick() {
    const Knobs k = ReadKnobs();
    if (k.Idle() && !AnyApplied()) {
      if (baseline_ || !threads_.empty()) {
        CloseAll();  // nothing to watch: keep no handles, retake the baselines when something is switched on
      }
      return;
    }
    LONG status = 0;
    if (!snapshot_.Take(samples_, &status)) {
      if (!snapshot_failed_) {
        snapshot_failed_ = true;
        REXLOG_WARN("[mcla-threads] thread snapshot failed (NTSTATUS 0x{:08X}): monitor and policy idle",
                    static_cast<uint32_t>(status));
      }
      return;
    }
    snapshot_failed_ = false;

    const double now = QpcSeconds();
    const bool initial = !baseline_;
    const double dt = initial ? 0.0 : std::max(now - last_tick_, 1e-3);
    const bool stats_on = k.stats_s > 0;
    if (stats_on && !stats_active_) {
      stats_active_ = true;
      ClearInterval();
      interval_start_ = initial ? now : last_tick_;
    } else if (!stats_on && stats_active_) {
      stats_active_ = false;
      ClearInterval();
    }

    for (auto& entry : threads_) {
      entry.second.seen = false;
    }
    for (const ThreadSample& sample : samples_) {
      auto it = threads_.find(sample.tid);
      if (it != threads_.end() && it->second.create_time != sample.create_time) {
        Retire(it->second);  // the id was reused by a new thread
        threads_.erase(it);
        it = threads_.end();
      }
      if (it == threads_.end()) {
        it = threads_.emplace(sample.tid, ThreadInfo{}).first;
        Open(it->second, sample, initial);
      }
      Update(it->second, sample, dt, initial);
    }
    for (auto it = threads_.begin(); it != threads_.end();) {
      if (it->second.seen) {
        ++it;
      } else {
        Retire(it->second);
        it = threads_.erase(it);
      }
    }

    EvaluateRoles(k);
    AssignIdeals(k);
    PolicyLog log;
    for (auto& entry : threads_) {
      ApplyThread(entry.second, k, log);
    }
    ApplyProcess(k, log);
    log.Flush();
    any_applied_ = process_.Any();
    for (const auto& entry : threads_) {
      any_applied_ = any_applied_ || entry.second.applied.Any();
    }
    prev_hot_cpus_ = k.hot_cpus;

    if (stats_active_ && !initial) {
      MaybePrintStats(now, k);
    }
    last_tick_ = now;
    baseline_ = true;
    ++tick_count_;
  }

  // Puts everything back (Stop(): the same code path as switching every knob off).
  void RestoreAll() {
    const Knobs off;
    PolicyLog log;
    for (auto& entry : threads_) {
      ThreadInfo& t = entry.second;
      DWORD exit_code = 0;
      if (t.handle && t.applied.Any() && GetExitCodeThread(t.handle, &exit_code) && exit_code == STILL_ACTIVE) {
        ApplyThread(t, off, log);
      }
    }
    ApplyProcess(off, log);
    log.Flush();
    any_applied_ = false;
  }

 private:
  struct ProcessApplied {
    bool prio_active = false;
    DWORD prio_orig = 0;
    DWORD prio_set = 0;
    int prio_fail = kNoValue;
    bool eco_active = false;
    ULONG eco_orig_control = 0;  // our bit (EXECUTION_SPEED) as we found it
    ULONG eco_orig_state = 0;
    bool eco_fail = false;
    bool Any() const { return prio_active || eco_active; }
  };

  enum class Dim { kPriority, kCpuSet, kIdeal, kEco };

  bool AnyApplied() const { return any_applied_; }

  void CloseAll() {
    for (auto& entry : threads_) {
      if (entry.second.handle) {
        CloseHandle(entry.second.handle);
      }
    }
    threads_.clear();
    baseline_ = false;
    ideal_dirty_ = true;
    ClearInterval();
    stats_active_ = false;
  }

  // ---- sampling

  void Open(ThreadInfo& t, const ThreadSample& s, bool initial) {
    t.tid = s.tid;
    t.create_time = s.create_time;
    // THREAD_QUERY_INFORMATION is for GetThreadIdealProcessorEx / GetThreadInformation (the values to put back).
    t.handle = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION |
                              THREAD_SET_INFORMATION,
                          FALSE, s.tid);
    if (!t.handle) {
      t.handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION | THREAD_SET_INFORMATION,
                            FALSE, s.tid);
    }
    t.can_set = t.handle != nullptr;
    if (!t.handle) {
      t.handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, s.tid);
    }
    if (t.handle) {
      t.name = ReadThreadName(t.handle);
      t.named = !t.name.empty();
      t.name_checks = 1;
    }
    if (initial) {
      // The first pass only takes baselines: what a thread did before this is not part of any interval.
      uint64_t cycles = 0;
      if (t.handle && QueryThreadCycleTime(t.handle, &cycles)) {
        t.last_cycles = cycles;
      }
      t.last_cpu_100ns = s.cpu_100ns;
      t.last_csw = s.context_switches;
    }
    // A thread created after the first pass starts from zero: everything it ran happened inside this interval.
  }

  void Update(ThreadInfo& t, const ThreadSample& s, double dt, bool initial) {
    t.seen = true;
    ++t.ticks_seen;
    t.state = s.state;
    t.priority = s.priority;
    t.base_priority = s.base_priority;

    if (!t.named && t.handle && t.ticks_seen > 1 &&
        (t.name_checks < kNameRetryTicks || t.ticks_seen % kNameSlowRetryTicks == 0)) {
      ++t.name_checks;
      t.name = ReadThreadName(t.handle);
      t.named = !t.name.empty();
    }

    double cpu = 0;
    uint64_t cycles = 0;
    if (tsc_hz_ > 0 && t.handle && QueryThreadCycleTime(t.handle, &cycles)) {
      cpu = cycles >= t.last_cycles ? static_cast<double>(cycles - t.last_cycles) / tsc_hz_ : 0.0;
      t.last_cycles = cycles;
      t.last_cpu_100ns = s.cpu_100ns;
    } else {
      cpu = s.cpu_100ns >= t.last_cpu_100ns ? static_cast<double>(s.cpu_100ns - t.last_cpu_100ns) * 1e-7 : 0.0;
      t.last_cpu_100ns = s.cpu_100ns;
    }
    const uint64_t csw = static_cast<ULONG>(s.context_switches - t.last_csw);
    t.last_csw = s.context_switches;

    if (!initial) {
      t.hist_cpu[t.hist_pos] = cpu;
      t.hist_dt[t.hist_pos] = dt;
      t.hist_pos = (t.hist_pos + 1) % kHotWindowTicks;
      t.hist_count = std::min(t.hist_count + 1, kHotWindowTicks);
    }
    if (stats_active_) {
      t.acc_cpu += cpu;
      t.acc_csw += csw;
      total_cpu_ += cpu;
      total_csw_ += csw;
      if (!initial && s.state == kThreadStateReady) {
        ++ready_[DisplayName(t)];
      }
    }
  }

  // A thread that is gone (or whose id was reused): its last cycles still count in the interval.
  void Retire(ThreadInfo& t) {
    if (t.handle) {
      uint64_t cycles = 0;
      double cpu = 0;
      if (tsc_hz_ > 0 && QueryThreadCycleTime(t.handle, &cycles) && cycles >= t.last_cycles) {
        cpu = static_cast<double>(cycles - t.last_cycles) / tsc_hz_;
      }
      if (stats_active_) {
        t.acc_cpu += cpu;
        total_cpu_ += cpu;
        if (t.acc_cpu > 0 && retired_.size() < kMaxRetired) {
          retired_.push_back({DisplayName(t), t.tid, t.acc_cpu});
        } else {
          retired_other_ += t.acc_cpu;
        }
      }
      CloseHandle(t.handle);
      t.handle = nullptr;
    }
    if (t.role == Role::kHot) {
      ideal_dirty_ = true;
    }
  }

  static std::string DisplayName(const ThreadInfo& t) { return t.named ? t.name : fmt::format("tid{}", t.tid); }

  // Average CPU (fraction of a core) over the last <= 5 ticks; count = samples in the window.
  static double WindowAverage(const ThreadInfo& t, int* count) {
    double cpu = 0;
    double dt = 0;
    for (int i = 0; i < t.hist_count; ++i) {
      cpu += t.hist_cpu[i];
      dt += t.hist_dt[i];
    }
    *count = t.hist_count;
    return dt > 0 ? cpu / dt : 0.0;
  }

  // ---- roles

  void EvaluateRoles(const Knobs& k) {
    for (auto& entry : threads_) {
      ThreadInfo& t = entry.second;
      if (t.role != Role::kNone || !t.named || t.tid == self_tid_) {
        continue;
      }
      std::string_view name = t.name;
      if (StartsWithAny(name, kNeverPrefixes)) {
        continue;
      }
      double avg = 0;
      Role role = Role::kNone;
      if (StartsWithAny(name, kHotPrefixes)) {
        role = Role::kHot;
      } else if (name.starts_with(kWorkerPrefix)) {
        int count = 0;
        avg = WindowAverage(t, &count);
        if (count >= kHotWindowTicks && avg * 100.0 >= k.hot_min) {
          role = Role::kHot;
        }
      } else if (StartsWithAny(name, kColdPrefixes)) {
        role = Role::kCold;
      }
      if (role == Role::kNone) {
        continue;
      }
      t.role = role;
      if (role == Role::kHot) {
        ideal_dirty_ = true;
      }
      std::string extra = avg > 0 ? fmt::format(" ({:.0f}% of a core over {} s)", avg * 100.0, kHotWindowTicks) : "";
      REXLOG_INFO("[mcla-threads] role {}: {} tid {} prio {}/{}{}{}", role == Role::kHot ? "hot" : "cold", t.name,
                  t.tid, t.priority, t.base_priority, extra, t.can_set ? "" : " (cannot be modified: no access)");
    }
  }

  // hot_cpus = 2: GPU Commands first, then the hot XThreads by CPU, then Main, then GPU Submission, each on its own
  // physical core of the highest class (they wrap around when there are more threads than cores). Recomputed only when
  // the hot set changes, so the cores do not shuffle every second.
  void AssignIdeals(const Knobs& k) {
    if (k.hot_cpus < 2 || !topology_.heterogeneous || topology_.high_cores.empty()) {
      return;
    }
    if (!ideal_dirty_ && prev_hot_cpus_ >= 2) {
      return;
    }
    ideal_dirty_ = false;
    struct Candidate {
      int rank;
      double cpu;
      ThreadInfo* thread;
    };
    std::vector<Candidate> hot;
    for (auto& entry : threads_) {
      ThreadInfo& t = entry.second;
      if (t.role != Role::kHot || !t.can_set) {
        continue;
      }
      int rank = 4;
      if (t.name.starts_with("GPU Commands")) {
        rank = 0;
      } else if (t.name.starts_with(kWorkerPrefix)) {
        rank = 1;
      } else if (t.name.starts_with("Main XThread")) {
        rank = 2;
      } else if (t.name.starts_with("GPU Submission")) {
        rank = 3;
      }
      int count = 0;
      hot.push_back({rank, WindowAverage(t, &count), &t});
    }
    std::sort(hot.begin(), hot.end(), [](const Candidate& a, const Candidate& b) {
      if (a.rank != b.rank) {
        return a.rank < b.rank;
      }
      if (a.cpu != b.cpu) {
        return a.cpu > b.cpu;
      }
      return a.thread->tid < b.thread->tid;
    });
    for (size_t i = 0; i < hot.size(); ++i) {
      hot[i].thread->ideal_slot = static_cast<int>(i % topology_.high_cores.size());
    }
  }

  // ---- applying (all of it is "desired state vs applied state": a knob going to 0 restores, a new thread of a role
  // gets what the role asks for)

  static std::string Label(const ThreadInfo& t, Dim dim, const Knobs& k, bool restored, int eco_kind) {
    const bool hot = t.role == Role::kHot;
    switch (dim) {
      case Dim::kPriority:
        return fmt::format("hot_priority={}", restored ? 0 : k.hot_priority);
      case Dim::kCpuSet:
      case Dim::kIdeal:
        return hot ? fmt::format("hot_cpus={}", restored ? 0 : k.hot_cpus)
                   : fmt::format("cold_cpus={}", restored ? 0 : k.cold_cpus);
      case Dim::kEco:
        return fmt::format("{}={}", hot ? (eco_kind == 2 ? "hot_noecoqos" : "hot_ecoqos") : "cold_ecoqos",
                           restored ? 0 : 1);
    }
    return {};
  }

  void Warn(const ThreadInfo& t, const std::string& label, const char* what, DWORD error) {
    REXLOG_WARN("[mcla-threads] t={:.1f} policy {} on {} tid {}: {} failed (error {})", SecondsSinceProcessStart(),
                label, t.name, t.tid, what, error);
  }

  void ApplyThread(ThreadInfo& t, const Knobs& k, PolicyLog& log) {
    if (!t.handle || !t.can_set) {
      return;
    }
    const bool hot = t.role == Role::kHot;
    const bool cold = t.role == Role::kCold;
    if (!hot && !cold) {
      return;
    }
    if (hot && k.hot_eco && k.hot_noeco && !eco_conflict_warned_) {
      eco_conflict_warned_ = true;
      REXLOG_WARN("[mcla-threads] mcla_thread_hot_ecoqos and mcla_thread_hot_noecoqos are both on: hot_ecoqos wins");
    }
    const bool one_class = !topology_.heterogeneous || topology_.high_ids.empty() || topology_.low_ids.empty();

    int priority = 0;  // 0 = leave
    int cpuset = 0;
    int eco = 0;
    const PROCESSOR_NUMBER* ideal = nullptr;
    if (hot) {
      priority = k.hot_priority == 1 ? THREAD_PRIORITY_ABOVE_NORMAL : k.hot_priority == 2 ? THREAD_PRIORITY_HIGHEST : 0;
      cpuset = (k.hot_cpus >= 1 && !one_class) ? 1 : 0;
      if (k.hot_cpus >= 2 && !one_class && t.ideal_slot >= 0 &&
          static_cast<size_t>(t.ideal_slot) < topology_.high_cores.size()) {
        ideal = &topology_.high_cores[static_cast<size_t>(t.ideal_slot)];
      }
      eco = k.hot_eco ? 1 : k.hot_noeco ? 2 : 0;
    } else {
      cpuset = (k.cold_cpus >= 1 && !one_class) ? 2 : 0;
      eco = k.cold_eco ? 1 : 0;
    }
    if (one_class && !one_class_warned_ && ((hot && k.hot_cpus > 0) || (cold && k.cold_cpus > 0))) {
      one_class_warned_ = true;
      REXLOG_WARN("[mcla-threads] this machine has a single CPU efficiency class: hot_cpus / cold_cpus have no effect");
    }

    ApplyPriority(t, priority, k, log);
    ApplyPlacement(t, cpuset, ideal, k, log);
    ApplyEco(t, eco, k, log);
  }

  void ApplyPriority(ThreadInfo& t, int want, const Knobs& k, PolicyLog& log) {
    Applied& a = t.applied;
    if (want == 0) {
      a.prio_fail = kNoValue;
      if (!a.prio_active) {
        return;
      }
      a.prio_active = false;
      // Someone else changed it since: not ours to put back.
      if (GetThreadPriority(t.handle) == a.prio_set) {
        if (SetThreadPriority(t.handle, a.prio_orig)) {
          log.Add(Label(t, Dim::kPriority, k, true, 0), true, t);
        } else {
          Warn(t, Label(t, Dim::kPriority, k, true, 0), "SetThreadPriority", GetLastError());
        }
      }
      return;
    }
    if ((a.prio_active && a.prio_set == want) || a.prio_fail == want) {
      return;
    }
    const int original = a.prio_active ? a.prio_orig : GetThreadPriority(t.handle);
    if (original == THREAD_PRIORITY_ERROR_RETURN) {
      a.prio_fail = want;
      Warn(t, Label(t, Dim::kPriority, k, false, 0), "GetThreadPriority", GetLastError());
      return;
    }
    if (!SetThreadPriority(t.handle, want)) {
      a.prio_fail = want;
      Warn(t, Label(t, Dim::kPriority, k, false, 0), "SetThreadPriority", GetLastError());
      return;
    }
    a.prio_active = true;
    a.prio_orig = original;
    a.prio_set = want;
    a.prio_fail = kNoValue;
    log.Add(Label(t, Dim::kPriority, k, false, 0), false, t);
  }

  static bool ReadCpuSets(HANDLE thread, std::vector<ULONG>* out) {
    out->clear();
    ULONG required = 0;
    if (GetThreadSelectedCpuSets(thread, nullptr, 0, &required)) {
      return true;  // no selected sets
    }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
      return false;
    }
    out->resize(required);
    ULONG filled = 0;
    if (!GetThreadSelectedCpuSets(thread, out->data(), required, &filled)) {
      out->clear();
      return false;
    }
    out->resize(filled);
    return true;
  }

  // The ideal processor the thread had before our first touch. Windows moves it into the new CPU sets by itself, and
  // SetThreadIdealProcessorEx refuses a processor outside the sets, so it has to be written down in advance.
  static void RememberIdeal(ThreadInfo& t) {
    Applied& a = t.applied;
    if (a.ideal_orig_valid) {
      return;
    }
    PROCESSOR_NUMBER ideal{};
    if (GetThreadIdealProcessorEx(t.handle, &ideal)) {
      a.ideal_orig = ideal;
      a.ideal_orig_valid = true;
    }
  }

  // CPU sets and the ideal processor go together: the ideal processor has to lie inside the sets, and putting the old
  // one back needs the sets out of the way (cleared, then the thread's own sets again).
  void ApplyPlacement(ThreadInfo& t, int want_set, const PROCESSOR_NUMBER* want_ideal, const Knobs& k,
                      PolicyLog& log) {
    Applied& a = t.applied;
    const HANDLE h = t.handle;
    const std::vector<ULONG>* ids = want_set == 1 ? &topology_.high_ids : want_set == 2 ? &topology_.low_ids : nullptr;
    if (want_set == 0) {
      a.cpuset_fail = kNoValue;
    }
    if (!want_ideal) {
      a.ideal_fail = kNoValue;
    }

    // 1. the CPU sets
    if (ids && !ids->empty() && a.cpuset_kind != want_set && a.cpuset_fail != want_set) {
      const bool first = a.cpuset_kind == 0;
      if (first) {
        if (!ReadCpuSets(h, &a.cpuset_orig)) {
          a.cpuset_orig.clear();
        }
        RememberIdeal(t);
      }
      if (SetThreadSelectedCpuSets(h, ids->data(), static_cast<ULONG>(ids->size()))) {
        a.cpuset_kind = want_set;
        a.cpuset_fail = kNoValue;
        log.Add(Label(t, Dim::kCpuSet, k, false, 0), false, t);
      } else {
        a.cpuset_fail = want_set;
        Warn(t, Label(t, Dim::kCpuSet, k, false, 0), "SetThreadSelectedCpuSets", GetLastError());
        if (first) {
          a.cpuset_orig.clear();
          if (!a.ideal_active) {
            a.ideal_orig_valid = false;
          }
        }
      }
    }

    // 2. the ideal processor
    if (want_ideal) {
      const int slot = want_ideal->Group * 256 + want_ideal->Number;
      const bool same = a.ideal_active && a.ideal_set.Group == want_ideal->Group && a.ideal_set.Number == want_ideal->Number;
      if (!same && a.ideal_fail != slot) {
        RememberIdeal(t);
        PROCESSOR_NUMBER target = *want_ideal;
        if (SetThreadIdealProcessorEx(h, &target, nullptr)) {
          a.ideal_active = true;
          a.ideal_set = *want_ideal;
          a.ideal_fail = kNoValue;
          log.Add(Label(t, Dim::kIdeal, k, false, 0), false, t);
        } else {
          a.ideal_fail = slot;
          Warn(t, Label(t, Dim::kIdeal, k, false, 0), "SetThreadIdealProcessorEx", GetLastError());
          if (!a.ideal_active && a.cpuset_kind == 0) {
            a.ideal_orig_valid = false;
          }
        }
      }
    } else if (a.ideal_active && !(want_set == 0 && a.cpuset_kind != 0)) {
      // The ideal processor goes away but the sets stay (or there are none): out of the way, back, sets again.
      a.ideal_active = false;
      if (a.cpuset_kind != 0) {
        SetThreadSelectedCpuSets(h, nullptr, 0);
      }
      PROCESSOR_NUMBER original = a.ideal_orig;
      const bool ok = !a.ideal_orig_valid || SetThreadIdealProcessorEx(h, &original, nullptr) != FALSE;
      const DWORD error = ok ? 0 : GetLastError();
      if (a.cpuset_kind != 0) {
        const std::vector<ULONG>& current = a.cpuset_kind == 1 ? topology_.high_ids : topology_.low_ids;
        SetThreadSelectedCpuSets(h, current.data(), static_cast<ULONG>(current.size()));
      } else {
        a.ideal_orig_valid = false;
      }
      if (ok) {
        log.Add(Label(t, Dim::kIdeal, k, true, 0), true, t);
      } else {
        Warn(t, Label(t, Dim::kIdeal, k, true, 0), "SetThreadIdealProcessorEx", error);
      }
    }

    // 3. leaving the CPU sets altogether: no sets, the old ideal processor, the thread's own sets if it had any
    if (want_set == 0 && a.cpuset_kind != 0) {
      bool ok = SetThreadSelectedCpuSets(h, nullptr, 0) != FALSE;
      DWORD error = ok ? 0 : GetLastError();
      if (ok && a.ideal_orig_valid) {
        PROCESSOR_NUMBER original = a.ideal_orig;
        if (!SetThreadIdealProcessorEx(h, &original, nullptr)) {
          ok = false;
          error = GetLastError();
        }
      }
      if (ok && !a.cpuset_orig.empty() &&
          !SetThreadSelectedCpuSets(h, a.cpuset_orig.data(), static_cast<ULONG>(a.cpuset_orig.size()))) {
        ok = false;
        error = GetLastError();
      }
      a.cpuset_kind = 0;
      a.cpuset_orig.clear();
      a.ideal_active = false;
      a.ideal_orig_valid = false;
      if (ok) {
        log.Add(Label(t, Dim::kCpuSet, k, true, 0), true, t);
      } else {
        Warn(t, Label(t, Dim::kCpuSet, k, true, 0), "SetThreadSelectedCpuSets / SetThreadIdealProcessorEx", error);
      }
    }
  }

  void ApplyEco(ThreadInfo& t, int want, const Knobs& k, PolicyLog& log) {
    Applied& a = t.applied;
    if (want == 0) {
      a.eco_fail = kNoValue;
      if (a.eco_kind == 0) {
        return;
      }
      const int was = a.eco_kind;
      a.eco_kind = 0;
      THREAD_POWER_THROTTLING_STATE state = a.eco_orig;  // normally 0/0: let the system decide
      if (SetThreadInformation(t.handle, ThreadPowerThrottling, &state, sizeof(state))) {
        log.Add(Label(t, Dim::kEco, k, true, was), true, t);
      } else {
        Warn(t, Label(t, Dim::kEco, k, true, was), "SetThreadInformation(ThreadPowerThrottling)", GetLastError());
      }
      return;
    }
    if (a.eco_kind == want || a.eco_fail == want) {
      return;
    }
    if (a.eco_kind == 0) {
      THREAD_POWER_THROTTLING_STATE original{};
      original.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
      if (!GetThreadInformation(t.handle, ThreadPowerThrottling, &original, sizeof(original))) {
        original.ControlMask = 0;
        original.StateMask = 0;
      }
      original.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
      a.eco_orig = original;
    }
    THREAD_POWER_THROTTLING_STATE state{};
    state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = want == 1 ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
    if (!SetThreadInformation(t.handle, ThreadPowerThrottling, &state, sizeof(state))) {
      a.eco_fail = want;
      Warn(t, Label(t, Dim::kEco, k, false, want), "SetThreadInformation(ThreadPowerThrottling)", GetLastError());
      return;
    }
    a.eco_kind = want;
    a.eco_fail = kNoValue;
    log.Add(Label(t, Dim::kEco, k, false, want), false, t);
  }

  // ---- the process itself

  void ApplyProcess(const Knobs& k, PolicyLog& /*log*/) {
    const HANDLE self = GetCurrentProcess();
    // priority class
    {
      const DWORD want = k.process_priority == 1 ? ABOVE_NORMAL_PRIORITY_CLASS
                         : k.process_priority == 2 ? HIGH_PRIORITY_CLASS
                                                   : 0;
      ProcessApplied& p = process_;
      if (want == 0) {
        p.prio_fail = kNoValue;
        if (p.prio_active) {
          p.prio_active = false;
          if (GetPriorityClass(self) == p.prio_set) {  // changed by someone else since: not ours to put back
            if (SetPriorityClass(self, p.prio_orig)) {
              REXLOG_INFO("[mcla-threads] t={:.1f} policy process_priority=0: process {} restored",
                          SecondsSinceProcessStart(), PriorityClassName(p.prio_orig));
            } else {
              REXLOG_WARN("[mcla-threads] policy process_priority=0: SetPriorityClass failed (error {})",
                          GetLastError());
            }
          }
        }
      } else if (!(p.prio_active && p.prio_set == want) && p.prio_fail != static_cast<int>(want)) {
        const DWORD original = p.prio_active ? p.prio_orig : GetPriorityClass(self);
        if (original == 0 || !SetPriorityClass(self, want)) {
          p.prio_fail = static_cast<int>(want);
          REXLOG_WARN("[mcla-threads] policy process_priority={}: SetPriorityClass failed (error {})",
                      k.process_priority, GetLastError());
        } else {
          p.prio_active = true;
          p.prio_orig = original;
          p.prio_set = want;
          p.prio_fail = kNoValue;
          REXLOG_INFO("[mcla-threads] t={:.1f} policy process_priority={}: process {} -> {}",
                      SecondsSinceProcessStart(), k.process_priority, PriorityClassName(original),
                      PriorityClassName(want));
        }
      }
    }
    // EcoQoS opt-out: query the state first and keep every bit that is not ours (the timer-resolution opt-out of
    // win32_time.cpp lives in the same state).
    {
      ProcessApplied& p = process_;
      if (!k.process_noeco) {
        if (p.eco_active) {
          p.eco_active = false;
          PROCESS_POWER_THROTTLING_STATE state{};
          state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
          if (GetProcessInformation(self, ProcessPowerThrottling, &state, sizeof(state))) {
            state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            state.ControlMask = (state.ControlMask & ~PROCESS_POWER_THROTTLING_EXECUTION_SPEED) | p.eco_orig_control;
            state.StateMask = (state.StateMask & ~PROCESS_POWER_THROTTLING_EXECUTION_SPEED) | p.eco_orig_state;
            if (SetProcessInformation(self, ProcessPowerThrottling, &state, sizeof(state))) {
              REXLOG_INFO("[mcla-threads] t={:.1f} policy process_noecoqos=0: process power throttling restored",
                          SecondsSinceProcessStart());
            } else {
              REXLOG_WARN("[mcla-threads] policy process_noecoqos=0: SetProcessInformation failed (error {})",
                          GetLastError());
            }
          }
        }
        p.eco_fail = false;
      } else if (!p.eco_active && !p.eco_fail) {
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        if (!GetProcessInformation(self, ProcessPowerThrottling, &state, sizeof(state))) {
          p.eco_fail = true;
          REXLOG_WARN("[mcla-threads] policy process_noecoqos=1: GetProcessInformation failed (error {}), not applied",
                      GetLastError());
        } else {
          const ULONG found_control = state.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
          const ULONG found_state = state.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
          state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
          state.ControlMask |= PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
          state.StateMask &= ~static_cast<ULONG>(PROCESS_POWER_THROTTLING_EXECUTION_SPEED);
          if (SetProcessInformation(self, ProcessPowerThrottling, &state, sizeof(state))) {
            p.eco_active = true;
            p.eco_orig_control = found_control;
            p.eco_orig_state = found_state;
            REXLOG_INFO("[mcla-threads] t={:.1f} policy process_noecoqos=1: process opted out of EcoQoS (kept bits "
                        "control=0x{:X} state=0x{:X})",
                        SecondsSinceProcessStart(), state.ControlMask, state.StateMask);
          } else {
            p.eco_fail = true;
            REXLOG_WARN("[mcla-threads] policy process_noecoqos=1: SetProcessInformation failed (error {})",
                        GetLastError());
          }
        }
      }
    }
  }

  // ---- stats

  void ClearInterval() {
    for (auto& entry : threads_) {
      entry.second.acc_cpu = 0;
      entry.second.acc_csw = 0;
    }
    retired_.clear();
    retired_other_ = 0;
    ready_.clear();
    total_cpu_ = 0;
    total_csw_ = 0;
  }

  void MaybePrintStats(double now, const Knobs& k) {
    const double elapsed = now - interval_start_;
    if (elapsed < 0.5 || elapsed + 0.25 < k.stats_s) {
      return;
    }
    struct Row {
      std::string label;
      uint32_t tid;
      double cpu;
    };
    std::vector<Row> rows;
    for (const auto& entry : threads_) {
      if (entry.second.acc_cpu > 0) {
        rows.push_back({DisplayName(entry.second), entry.second.tid, entry.second.acc_cpu});
      }
    }
    for (const RetiredThread& r : retired_) {
      rows.push_back({r.label, r.tid, r.cpu});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.cpu > b.cpu; });
    // The same name twice among the shown threads (a pool): add the thread id.
    const size_t shown = std::min<size_t>(rows.size(), kStatsTopThreads);
    double shown_cpu = 0;
    std::string line;
    for (size_t i = 0; i < shown; ++i) {
      if (rows[i].cpu / elapsed < 0.005) {
        break;
      }
      std::string label = rows[i].label;
      for (size_t j = 0; j < shown; ++j) {
        if (j != i && rows[j].label == label) {
          label += fmt::format("#{}", rows[i].tid);
          break;
        }
      }
      line += fmt::format(" | {}={:.2f}", label, rows[i].cpu / elapsed);
      shown_cpu += rows[i].cpu;
    }
    const double total = total_cpu_ / elapsed;
    const double other = std::max(0.0, total - shown_cpu / elapsed);
    REXLOG_INFO("[mcla-threads] t={:.1f} dt={:.1f} total={:.2f}{} | other={:.2f}", SecondsSinceProcessStart(), elapsed,
                total, line, other);

    std::vector<std::pair<std::string, int>> ready(ready_.begin(), ready_.end());
    std::stable_sort(ready.begin(), ready.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string ready_text;
    for (size_t i = 0; i < ready.size() && i < 8; ++i) {
      ready_text += fmt::format("{}{}:{}", i ? "," : "", ready[i].first, ready[i].second);
    }
    REXLOG_INFO("[mcla-threads]   csw/s total={:.0f} ready={}", static_cast<double>(total_csw_) / elapsed,
                ready_text.empty() ? "none" : ready_text);

    ClearInterval();
    interval_start_ = now;
  }

  double tsc_hz_ = 0;
  uint32_t self_tid_ = 0;
  SystemSnapshot snapshot_;
  std::vector<ThreadSample> samples_;
  CpuTopology topology_;
  std::map<uint32_t, ThreadInfo> threads_;
  ProcessApplied process_;

  bool baseline_ = false;       // a pass has taken the baselines
  double last_tick_ = 0;
  uint64_t tick_count_ = 0;
  bool any_applied_ = false;
  bool snapshot_failed_ = false;
  bool eco_conflict_warned_ = false;
  bool one_class_warned_ = false;
  bool ideal_dirty_ = true;
  int prev_hot_cpus_ = 0;

  bool stats_active_ = false;
  double interval_start_ = 0;
  double total_cpu_ = 0;
  uint64_t total_csw_ = 0;
  double retired_other_ = 0;
  std::vector<RetiredThread> retired_;
  std::map<std::string, int> ready_;
};

// Cycles per second of the counter QueryThreadCycleTime counts in (the TSC), measured against the QPC. 0 if it fails.
double TscHz(uint64_t tsc0, double qpc0, uint64_t tsc1, double qpc1) {
  const double seconds = qpc1 - qpc0;
  if (seconds <= 0.01 || tsc1 <= tsc0) {
    return 0;
  }
  const double hz = static_cast<double>(tsc1 - tsc0) / seconds;
  return (hz > 1e8 && hz < 2e10) ? hz : 0;
}

}  // namespace

ThreadPolicy::~ThreadPolicy() {
  Stop();
}

void ThreadPolicy::Start() {
  std::lock_guard lock(mutex_);
  if (running_) {
    return;
  }
  stop_ = false;
  running_ = true;
  thread_ = std::thread([this] { Main(); });
  worker_id_ = thread_.get_id();
}

void ThreadPolicy::Stop() {
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
    if (worker.joinable()) {
      worker.detach();
    }
    return;
  }
  if (worker.joinable()) {
    worker.join();
  }
  std::unique_lock lock(mutex_);
  wake_.wait(lock, [this] { return !running_; });
}

void ThreadPolicy::Main() {
  SetThreadDescription(GetCurrentThread(), L"mcla thread policy");
  try {
    // The cycle counter's rate, once: ~100 ms of TSC against the QPC (waiting on the stop flag, so Stop() is immediate).
    const uint64_t tsc0 = __rdtsc();
    const double qpc0 = QpcSeconds();
    std::unique_lock lock(mutex_);
    if (!wake_.wait_for(lock, std::chrono::milliseconds(100), [this] { return stop_; })) {
      const uint64_t tsc1 = __rdtsc();
      const double qpc1 = QpcSeconds();
      lock.unlock();

      Monitor monitor(TscHz(tsc0, qpc0, tsc1, qpc1));
      monitor.LogStart();
      auto next = std::chrono::steady_clock::now();
      while (true) {
        try {
          monitor.Tick();
        } catch (const std::exception& e) {
          REXLOG_WARN("[mcla-threads] tick failed: {}", e.what());
        } catch (...) {
          REXLOG_WARN("[mcla-threads] tick failed");
        }
        next += std::chrono::seconds(1);
        const auto current = std::chrono::steady_clock::now();
        if (next < current) {
          next = current + std::chrono::seconds(1);  // we fell behind: do not burst to catch up
        }
        lock.lock();
        const bool stopping = wake_.wait_until(lock, next, [this] { return stop_; });
        lock.unlock();
        if (stopping) {
          break;
        }
      }
      try {
        monitor.RestoreAll();
      } catch (...) {
      }
    } else {
      lock.unlock();
    }
  } catch (const std::exception& e) {
    REXLOG_WARN("[mcla-threads] monitor stopped by an error: {}", e.what());
  } catch (...) {
    REXLOG_WARN("[mcla-threads] monitor stopped by an error");
  }
  {
    std::lock_guard lock(mutex_);
    running_ = false;
  }
  wake_.notify_all();
}

}  // namespace mcla_host
