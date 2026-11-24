// mcla_heartbeat: one beat per engine frame, for the host's hang watchdog (HOOKS_SPEC A1).
//
// Site 0x821BDA90, first instruction (`mftb r11`) of rage::sysTimeManager::Update(this = r3, ...).
// The engine's one timer object lives at 0x827D7500; its update runs once per iteration of the
// per-frame function sub_822C1FA8 (and from a Reset+Update helper, sub_822611B0). The hook only
// looks at r3: no guest memory is touched.
//
// Each frame:
//   * nfsc::ProgressBeat() (nfsc_progress.h next to this file): the host's hang watchdog reads
//     nfsc::ProgressBeats();
//   * the frame counter (mcla::FrameCount());
//   * nfsc::profile::FrameTick() (mcla/profile/game_profile.h): commits the game's screen, logs [mcla-mode].
//
// A second pass within 1 ms is the same frame (Reset+Update back to back, or a second thread), not
// a new one: it is not counted. At 120 fps a frame is 8 ms.
//
// LIMIT: the timer does not run during the legals, the logos or inside blocking loads (those loops
// never reach the engine timer), so FrameCount() is 0 for the first seconds of every boot and stays
// flat during a long load. The watchdog must not treat "no beats yet" as a freeze: arm it at the
// first beat and give the boot a long timeout (docs/mcla/LEARNINGS.md, 5.0).
//
// 60 fps timing (docs/mcla/TIMING.md, mcla_t_common.h): with --mcla_sim60=true the same counted frame
// also runs mcla::sim::OnFrameStart() (mcla_t_common.cpp), on the game thread, BEFORE the timer reads
// its clock (this hook is the first instruction of Update):
//   * the pacer: sleeps to an even frame deadline (1/60 s, 1/30 s at a 30 cap) so the quantised world
//     step does not flip between n = 1 and 2 with the render thread's jitter (--mcla_sim_pace=false
//     leaves the swap-side cap alone);
//   * the frame-rate object's substep word is put back to the shipped 2, so a frame that takes the
//     timer's reset path (after loads) runs the shipped step; the normal path sets n in T1;
//   * the missed-frames rule (a 60 cap that more than 10 % of the frames cannot hold = hold 30 for
//     30 s) and the '[mcla-sim]' check line every --mcla_sim_log_s seconds (default 5).
// Only the counted frame does this (the 1 ms same-frame rule above), and only while the heartbeat
// itself is on (--mcla_heartbeat=true, the default). With mcla_sim60 off it costs one mutex lock.
//
// Off: --mcla_heartbeat=false, or MCLA_HOOK_HEARTBEAT=0 at build time.
#include <atomic>
#include <chrono>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "game_profile.h"
#include "mcla_gfx_tune.h"
#include "mcla_hooks.h"
#include "mcla_t_common.h"
#include "nfsc_progress.h"

REXCVAR_DEFINE_BOOL(mcla_heartbeat, true, "MCLA",
                    "MCLA: count a frame at every engine-timer update for the hang watchdog and the "
                    "power probe (false: the watchdog sees only thread registers)");

namespace {

mcla::hooks::Registration g_registration("heartbeat", "mcla_heartbeat", MCLA_HOOK_HEARTBEAT != 0,
                                         [] { return REXCVAR_GET(mcla_heartbeat); });

std::atomic<uint64_t> g_frames{0};

}  // namespace

namespace mcla {
uint64_t FrameCount() { return g_frames.load(std::memory_order_relaxed); }
}  // namespace mcla

#if MCLA_HOOK_HEARTBEAT

namespace {

constexpr uint32_t kTimerObject = 0x827D7500;  // rage::sysTimeManager, the one instance
constexpr int64_t kSameFrameNs = 1'000'000;    // 1 ms

std::atomic<int64_t> g_last_beat_ns{0};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

void mcla_hook_heartbeat(PPCRegister& r3) {
  MCLA_HOOK_ENTER();
  if (r3.u32 != kTimerObject || !REXCVAR_GET(mcla_heartbeat)) {
    return;
  }
  const int64_t now = NowNs();
  int64_t last = g_last_beat_ns.load(std::memory_order_relaxed);
  if (last != 0 && now - last < kSameFrameNs) {
    return;
  }
  // Two threads can reach this together; the one that moves the stamp counts the frame.
  if (!g_last_beat_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    return;
  }
  const uint64_t frame = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  nfsc::ProgressBeat();
  nfsc::profile::FrameTick();
  if (frame == 1) {
    REXLOG_INFO("[mcla-heartbeat] first engine frame (the legals, logos and loads before it run "
                "without the timer: the watchdog arms here)");
  }
  // The game's own quality knobs (mcla_gfx_tune.cpp, NATIVE_RENDERER.md section 22): live writes.
  mcla::gfxtune::OnFrame();
  // The 60 fps timing: pacer, substep reset, check line (mcla_t_common.cpp). Last: it may sleep.
  mcla::sim::OnFrameStart(now);
}

#else  // MCLA_HOOK_HEARTBEAT == 0: keeps hooks.toml linking, does nothing.

void mcla_hook_heartbeat(PPCRegister&) {}

#endif
