// Shared state of the 60 fps timing hooks (see mcla_t_common.h and docs/mcla/TIMING.md). Always
// compiled, not a hook: the cvars, the quantised world step, the game-thread pacer, the missed-frames
// rule, the per-movie and per-race records and the `[mcla-sim]` check line live here; the hook files
// only hand registers in and out.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "nfsmw_pch.h"

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"
#include "mcla_t_common.h"

REXCVAR_DEFINE_BOOL(mcla_sim60, false, "MCLA",
                    "MCLA 60 fps timing master switch (docs/mcla/TIMING.md). Off (default) = the game's "
                    "own fixed 1/30 s step. On = the world step is n/60 s (n = 1..4 from the real frame "
                    "time) with n physics substeps, and the camera/chassis smoothing, Flash movie steps, "
                    "countdown and race clock are corrected. Needs a frame cap of 60 to matter; at 30 it "
                    "equals the shipped run");
REXCVAR_DEFINE_INT32(mcla_sim_force_n, 0, "MCLA",
                     "MCLA timing debug: force the world step to n/60 s every frame (n = 1..4), whatever "
                     "the real frame time (a machine at 10-15 fps then shows the 60 fps step in slow "
                     "motion on purpose). 0 = automatic");
REXCVAR_DEFINE_INT32(mcla_physics_substeps, 0, "MCLA",
                     "MCLA timing A/B: physics substeps a frame with mcla_sim60: 0 = automatic (n, so the "
                     "physics step stays the shipped 1/60 s), 1..4 = always that many (2 at n = 1 runs "
                     "physics at 120 Hz like MCLA-4iOS). Never 0 substeps: no wheels");
REXCVAR_DEFINE_INT32(mcla_sim_cap, 0, "MCLA",
                     "MCLA timing: the frame cap the world assumes with mcla_sim60: 30, 60 or 120. 0 = none "
                     "known = 60 (n follows the real frame time). --mcla_sim_cap=30 checks that 30 equals the "
                     "shipped timing");
REXCVAR_DEFINE_BOOL(mcla_sim_pace, true, "MCLA",
                    "MCLA timing: with mcla_sim60 the game thread sleeps to an even frame deadline "
                    "(1/60 s, 1/30 s at a 30 cap) at the start of every engine frame, so n does not "
                    "flip with the render thread's jitter. false = the swap-side cap alone");
REXCVAR_DEFINE_INT32(mcla_sim_hold30_s, 30, "MCLA",
                     "MCLA timing: at a cap of 60, when more than 10% of the frames of a log window needed "
                     "n >= 2 the world holds a 30 fps cap for this many seconds (a steady 30 beats a 40-50 "
                     "judder). 0 = never");
REXCVAR_DEFINE_INT32(mcla_sim_pacer, 1, "MCLA",
                     "MCLA timing: the 60 -> 30 rule. 0 = shipped (more than 10% of a 5 s log window missed -> a "
                     "30 hold of mcla_sim_hold30_s). 1 = adaptive (TIMING.md 13): a short window "
                     "(mcla_sim_miss_window_s) with more than mcla_sim_miss_pct % missed drops to 30; the hold "
                     "starts at mcla_sim_hold30_min_s and doubles up to mcla_sim_hold30_s when the misses return "
                     "within mcla_sim_calm_s of it ending");
REXCVAR_DEFINE_INT32(mcla_sim_hold30_min_s, 6, "MCLA", "MCLA timing, mcla_sim_pacer 1: the first hold at 30, seconds");
REXCVAR_DEFINE_INT32(mcla_sim_miss_window_s, 2, "MCLA", "MCLA timing, mcla_sim_pacer 1: the decision window, seconds");
REXCVAR_DEFINE_INT32(mcla_sim_miss_pct, 10, "MCLA",
                     "MCLA timing, mcla_sim_pacer 1: percent of a window's frames that must be missed to count as high "
                     "(measured: GPU 20 ms = 12-16 %, 52-54 fps; 21.8 ms = 25 %, 48 fps; 10 % is about 55 fps)");
REXCVAR_DEFINE_INT32(mcla_sim_calm_s, 30, "MCLA",
                     "MCLA timing, mcla_sim_pacer 1: seconds at 60 after a hold before the next drop starts short again");
REXCVAR_DEFINE_INT32(mcla_sim_log_s, 5, "MCLA",
                     "MCLA timing: seconds between the '[mcla-sim]' check lines (world time against real "
                     "time, n histogram, UI and race clock rates) with mcla_sim60");
// P1: 120 fps (TIMING.md section 15) has no cvar of its own: a mcla_sim_cap above 60 puts the world on the
// 1/120 grid, and misses there drop to the 60 tier by the same rule as 60 -> 30.
REXCVAR_DEFINE_BOOL(mcla_smoothing_fix, true, "MCLA",
                    "MCLA timing, with mcla_sim60: chase camera and chassis ground-depth smoothing use "
                    "k(dt) = 1 - (1 - k30)^(30 dt) instead of the game's below-60 / 60+ constants");
REXCVAR_DEFINE_BOOL(mcla_ui_30hz, true, "MCLA",
                    "MCLA timing, with mcla_sim60: Flash movies (HUD, menus) step 30 times a second "
                    "whatever the frame rate, and the race-start countdown gets the time since its last "
                    "step. false = a movie step every frame (double speed at 60)");
REXCVAR_DEFINE_STRING(mcla_race_clock, "retail", "MCLA",
                      "MCLA timing, with mcla_sim60: the race clock's milliseconds a step: 'retail' (0.99 of "
                      "real time with the fraction carried: the console's 33 ms per 1/30 s), 'exact', or "
                      "'off' (the game's int(dt x 1000), 4% slow at 60 fps)");

namespace mcla::sim {
namespace {

constexpr double kGuestTickGameConstant = 49'875'000.0;  // the game's seconds-per-tick (0x82011110)
constexpr double kMaxRawMs = 125.0;                      // the hitch clamp (A2): 1/8 s
constexpr int kMaxUiRecords = 64;
constexpr int kRaceCarries = 4;

thread_local double t_raw_ms = -1.0;  // A2 -> T1 hand-off, same Update call, same thread

struct UiSlot {
  uint32_t context = 0;
  math::UiRecord record;
  uint64_t calls = 0;  // this window
  uint64_t steps = 0;
};

struct RaceSlot {
  uint32_t race = 0;
  math::RaceCarry carry;
};

struct Window {
  int64_t start_ns = 0;
  uint64_t frames = 0;  // T1 calls (the frames that took the fixed-step path)
  uint64_t n_hist[math::kFineMaxN + 1] = {};  // P1: up to 8 quanta on the 1/120 grid
  math::FrameTimes frame_times;               // P1: real time between engine frames (stutter counters)
  uint64_t substeps = 0;
  uint64_t resyncs = 0;
  uint64_t missed = 0;  // n >= 2 at a 60 cap, hitches excluded
  uint64_t forced = 0;
  double world_s = 0.0;
  uint32_t guest_frames0 = 0;
  float guest_total0 = 0.0f;
  bool guest_ok = false;
  uint64_t sleeps = 0;
  double slept_ms = 0.0;
  uint64_t pace_resyncs = 0;
};

std::mutex g_mutex;  // g_win, g_debt, the pacer, the hold; never held while sleeping
Window g_win;
double g_debt_ms = 0.0;
int g_last_period_q = 0;
bool g_active = false;
bool g_wrote_substeps = false;
int64_t g_deadline_ns = 0;
int64_t g_hold_until_ns = 0;
std::atomic<bool> g_hold30{false};
// The adaptive 60 -> 30 rule (mcla_sim_pacer 1), TIMING.md 13. Counters under g_mutex.
int64_t g_dec_start_ns = 0;
uint64_t g_dec_frames = 0, g_dec_missed = 0;
int g_prev_hold_s = 0;
int64_t g_last_hold_end_ns = 0;
uint64_t g_drops = 0;
std::atomic<uint64_t> g_substep_fail{0};
// P1 begin: the 120 -> 60 tier (a cap above 60 only). g_mutex for the rest; the flag is read by EffectiveCap.
std::atomic<bool> g_tier60{false};
int64_t g_tier_until_ns = 0;
int64_t g_tier_end_ns = 0;
int g_tier_prev_hold_s = 0;
uint64_t g_tier_drops = 0;
int64_t g_last_frame_ns = 0;   // OnFrameStart: the previous engine frame (frame-time counters)
int g_last_grid = 0;           // NextStep: the grid of the previous frame (Hz x 10 + period quanta)
// P1 end

std::mutex g_ui_mutex;
UiSlot g_ui[kMaxUiRecords];
int g_ui_count = 0;
uint64_t g_ui_overflow = 0;
uint64_t g_cd_calls = 0, g_cd_unknown = 0;

std::mutex g_race_mutex;
RaceSlot g_race[kRaceCarries];
int g_race_next = 0;
uint64_t g_race_ms = 0, g_race_calls = 0;

std::atomic<uint64_t> g_smooth_calls[kSmoothKinds];
std::atomic<uint64_t> g_smooth_changed[kSmoothKinds];
std::atomic<uint64_t> g_smooth_dt[kSmoothKinds][3];

rex::memory::Memory* GuestMemory() {
  rex::system::KernelState* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

int SettingCap() { return REXCVAR_GET(mcla_sim_cap); }

double GuestTickFrequency() {
  static const double frequency = double(rex::chrono::Clock::guest_tick_frequency());
  return frequency > 0 ? frequency : 50'000'000.0;
}

// The measured step of this frame in ms when A2 did not hand one over: the timer's own float, which
// uses the game's tick constant (1/49.875 MHz) while the SDK's guest ticks run at their own rate.
double FallbackRawMs() {
  float raw = 0.0f;
  if (!ImageF32(kTimerRaw, &raw) || !(raw > 0.0f)) {
    return math::kQuantumMs * 2;
  }
  return std::min(kMaxRawMs, double(raw) * 1000.0 * (kGuestTickGameConstant / GuestTickFrequency()));
}

void ResetWindow(int64_t now_ns) {
  g_win = Window{};
  g_win.start_ns = now_ns;
  uint32_t frames = 0, total_bits = 0;
  g_win.guest_ok = ImageU32(kTimerFrames, &frames) && ImageU32(kTimerTotal, &total_bits);
  if (g_win.guest_ok) {
    g_win.guest_frames0 = frames;
    std::memcpy(&g_win.guest_total0, &total_bits, sizeof(float));
  }
  std::lock_guard<std::mutex> ui_lock(g_ui_mutex);
  for (int i = 0; i < g_ui_count; ++i) {
    g_ui[i].calls = 0;
    g_ui[i].steps = 0;
  }
  g_cd_calls = g_cd_unknown = 0;
  std::lock_guard<std::mutex> race_lock(g_race_mutex);
  g_race_ms = g_race_calls = 0;
  for (int k = 0; k < kSmoothKinds; ++k) {
    g_smooth_calls[k] = 0;
    g_smooth_changed[k] = 0;
    for (int c = 0; c < 3; ++c) g_smooth_dt[k][c] = 0;
  }
}

// g_mutex held. Writes the check line and applies the missed-frames rule.
void Report(int64_t now_ns) {
  const Window w = g_win;
  const double real_s = double(now_ns - w.start_ns) / 1e9;
  const int cap = EffectiveCap();

  uint32_t frames_now = 0, total_bits = 0;
  float total_now = 0.0f;
  const bool guest_ok = w.guest_ok && ImageU32(kTimerFrames, &frames_now) && ImageU32(kTimerTotal, &total_bits);
  if (guest_ok) std::memcpy(&total_now, &total_bits, sizeof(float));
  const uint32_t guest_frames = guest_ok ? frames_now - w.guest_frames0 : 0;
  const uint64_t reset_frames = guest_frames > w.frames ? guest_frames - w.frames : 0;

  // The busiest movie context of the window = the HUD gauge in a drive; steps a second.
  uint64_t ui_calls = 0, ui_steps = 0;
  uint32_t ui_context = 0;
  int ui_contexts = 0;
  uint64_t cd_calls = 0, cd_unknown = 0, race_ms = 0, race_calls = 0;
  {
    std::lock_guard<std::mutex> lock(g_ui_mutex);
    for (int i = 0; i < g_ui_count; ++i) {
      if (g_ui[i].calls > 0) ++ui_contexts;
      if (g_ui[i].calls > ui_calls) {
        ui_calls = g_ui[i].calls;
        ui_steps = g_ui[i].steps;
        ui_context = g_ui[i].context;
      }
      g_ui[i].calls = 0;
      g_ui[i].steps = 0;
    }
    cd_calls = g_cd_calls;
    cd_unknown = g_cd_unknown;
    g_cd_calls = g_cd_unknown = 0;
  }
  {
    std::lock_guard<std::mutex> lock(g_race_mutex);
    race_ms = g_race_ms;
    race_calls = g_race_calls;
    g_race_ms = g_race_calls = 0;
  }

  REXLOG_INFO("[mcla-sim] {:.2f} s real: world {:.3f} s (x{:.4f}), frames {} (n1 {} n2 {} n3 {} n4 {}), "
              "substeps {}, reset {}, resync {}, guest total {:+.3f} s, ui {}, race {}, cap {}{}",
              real_s, w.world_s, real_s > 0 ? w.world_s / real_s : 0.0, w.frames, w.n_hist[1],
              w.n_hist[2], w.n_hist[3], w.n_hist[4], w.substeps, reset_frames, w.resyncs,
              guest_ok ? double(total_now) - double(w.guest_total0) : 0.0,
              ui_calls > 0 ? fmt::format("steps {} ({:.1f}/s, ctx 0x{:08X} of {})", ui_steps,
                                         real_s > 0 ? double(ui_steps) / real_s : 0.0, ui_context,
                                         ui_contexts)
                           : std::string("steps -"),
              race_calls > 0 ? fmt::format("+{} ms", race_ms) : std::string("-"),
              cap > 0 ? cap : 60, g_hold30.load() ? " (held at 30 after missed frames)" : "");

  uint64_t smooth_dt_total[kSmoothKinds] = {};
  for (int k = 0; k < kSmoothKinds; ++k) {
    smooth_dt_total[k] = g_smooth_dt[k][0] + g_smooth_dt[k][1] + g_smooth_dt[k][2];
  }
  REXLOG_INFO("[mcla-sim]   missed {} forced {} sleeps {} ({:.0f} ms) pace-resync {} substep-write-fail {}; "
              "smoothing calls (dt 1/30 : 1/60 : other, changed) cam-pos {}:{}:{} ({}) look-at {}:{}:{} ({}) "
              "chassis {}:{}:{} ({}); countdown {} (unknown ctx {})",
              w.missed, w.forced, w.sleeps, w.slept_ms, w.pace_resyncs, g_substep_fail.load(),
              uint64_t(g_smooth_dt[0][0]), uint64_t(g_smooth_dt[0][1]), uint64_t(g_smooth_dt[0][2]),
              uint64_t(g_smooth_changed[0]), uint64_t(g_smooth_dt[1][0]), uint64_t(g_smooth_dt[1][1]),
              uint64_t(g_smooth_dt[1][2]), uint64_t(g_smooth_changed[1]), uint64_t(g_smooth_dt[2][0]),
              uint64_t(g_smooth_dt[2][1]), uint64_t(g_smooth_dt[2][2]), uint64_t(g_smooth_changed[2]),
              cd_calls, cd_unknown);
  (void)smooth_dt_total;
  // P1: the stutter counters of the window (game-thread frame to frame; bench.py and read_device_log.py read it).
  if (w.frame_times.count > 0) {
    REXLOG_INFO("[mcla-sim]   frame times: {} frames, p50 {:.0f} p99 {:.0f} worst {:.1f} ms, over 20 / 25 / 34 ms: {} / {} / {}"
                "{}{}",
                w.frame_times.count, w.frame_times.Percentile(0.50), w.frame_times.Percentile(0.99),
                w.frame_times.worst_ms, w.frame_times.Over(20), w.frame_times.Over(25), w.frame_times.Over(34),
                math::FineGrid(SettingCap()) || g_tier60.load() ? fmt::format(", n5-n8 {} {} {} {}", w.n_hist[5],
                                                                              w.n_hist[6], w.n_hist[7], w.n_hist[8])
                                                                : std::string(),
                g_tier60.load() ? " (120 held at 60)" : "");
  }

  // The missed-frames rule: at a 60 cap, more than 10 % of the frames needed two or more quanta.
  const int hold_s = REXCVAR_GET(mcla_sim_hold30_s);
  if (REXCVAR_GET(mcla_sim_pacer) == 0 && hold_s > 0 && !g_hold30.load() && REXCVAR_GET(mcla_sim_force_n) == 0 &&
      math::PeriodQuanta(SettingCap()) == 1 && !math::FineGrid(EffectiveCap()) && w.frames >= 60 &&
      w.missed * 10 > w.frames) {
    g_hold30.store(true);
    g_hold_until_ns = now_ns + int64_t(hold_s) * 1'000'000'000;
    g_debt_ms = 0.0;
    g_deadline_ns = 0;
    REXLOG_INFO("[mcla-sim] cap 60 -> 30 (missed {:.0f} %): the world holds 30 for {} s",
                100.0 * double(w.missed) / double(w.frames), hold_s);
  }
  ResetWindow(now_ns);
}

// g_mutex held. The adaptive rule (mcla_sim_pacer 1): every mcla_sim_miss_window_s of real time, decide from the
// frames of that window whether to drop to 30 (TIMING.md 13). A window with more than mcla_sim_miss_pct % of its
// frames missed is high; a high window drops at once (no resolution governor on the PC to give room first).
void DecideHold(int64_t now_ns) {
  if (REXCVAR_GET(mcla_sim_pacer) == 0) return;
  const int64_t window_ns = int64_t(std::max(1, REXCVAR_GET(mcla_sim_miss_window_s))) * 1'000'000'000;
  if (g_dec_start_ns == 0 || g_hold30.load()) {
    g_dec_start_ns = now_ns;
    g_dec_frames = g_dec_missed = 0;
    return;
  }
  if (now_ns - g_dec_start_ns < window_ns) return;
  const uint64_t frames = g_dec_frames, missed = g_dec_missed;
  const double window_s = double(now_ns - g_dec_start_ns) / 1e9;
  g_dec_start_ns = now_ns;
  g_dec_frames = g_dec_missed = 0;
  if (frames < 30) return;  // a load or a menu: no frames of the 60 step to judge
  const double pct = 100.0 * double(missed) / double(frames);
  const int miss_pct = REXCVAR_GET(mcla_sim_miss_pct);
  if (missed * 100 <= frames * uint64_t(std::max(0, miss_pct))) return;  // not a high window
  const int max_hold = REXCVAR_GET(mcla_sim_hold30_s);
  // P1 begin: at 120 a miss first costs the 120 (60 tier), never straight to 30.
  if (math::FineGrid(EffectiveCap())) {
    if (REXCVAR_GET(mcla_sim_force_n) != 0) return;
    const double since_end = g_tier_end_ns ? double(now_ns - g_tier_end_ns) / 1e9 : 1e9;
    const int tier_s = math::HoldSeconds(g_tier_prev_hold_s, since_end, REXCVAR_GET(mcla_sim_hold30_min_s),
                                         std::max(1, max_hold), REXCVAR_GET(mcla_sim_calm_s));
    g_tier_prev_hold_s = tier_s;
    g_tier60.store(true);
    g_tier_until_ns = now_ns + int64_t(tier_s) * 1'000'000'000;
    g_debt_ms = 0.0;
    g_deadline_ns = 0;
    ++g_tier_drops;
    REXLOG_INFO("[mcla-sim] cap 120 -> 60 (missed {:.0f} % of {:.1f} s): 60 for {} s", pct, window_s, tier_s);
    return;
  }
  // P1 end
  if (max_hold <= 0 || REXCVAR_GET(mcla_sim_force_n) != 0 || math::PeriodQuanta(SettingCap()) != 1) return;
  const double since_end = g_last_hold_end_ns ? double(now_ns - g_last_hold_end_ns) / 1e9 : 1e9;
  const int hold_s = math::HoldSeconds(g_prev_hold_s, since_end, REXCVAR_GET(mcla_sim_hold30_min_s), max_hold,
                                       REXCVAR_GET(mcla_sim_calm_s));
  if (hold_s <= 0) return;
  g_prev_hold_s = hold_s;
  g_hold30.store(true);
  g_hold_until_ns = now_ns + int64_t(hold_s) * 1'000'000'000;
  g_debt_ms = 0.0;
  g_deadline_ns = 0;
  ++g_drops;
  REXLOG_INFO("[mcla-sim] cap 60 -> 30 (missed {:.0f} % of {:.1f} s): the world holds 30 for {} s", pct, window_s,
              hold_s);
}

// ---- the pacer's sleep ---------------------------------------------------------------------------

// The pacer holds the game thread to an even frame grid (16.67 ms at 60, 8.33 ms at 120). A plain Sleep() ends on
// the system timer tick (15.6 ms; 1 ms once the host called timeBeginPeriod), far too coarse for that. A
// high-resolution waitable timer (Windows 10 1803 and later) covers all but the last kSpinTailNs; that tail is
// spun on the clock with a pause per lap, so the thread wakes within a few tens of microseconds of the deadline
// for about 2 % of a core at 60 fps.
constexpr int64_t kSpinTailNs = 300'000;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// One timer per thread that paces (in practice the game thread), closed with the thread.
struct PaceTimer {
  HANDLE handle = nullptr;
  PaceTimer() {
    handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!handle) {
      REXLOG_WARN("[mcla-sim] no high-resolution waitable timer (error {}): the pacer sleeps in whole milliseconds",
                  GetLastError());
    }
  }
  ~PaceTimer() {
    if (handle) CloseHandle(handle);
  }
  PaceTimer(const PaceTimer&) = delete;
  PaceTimer& operator=(const PaceTimer&) = delete;
};

// Sleeps the calling thread until `deadline_ns` on the steady clock (the clock OnFrameStart's `now_ns` is read from).
void SleepUntil(int64_t deadline_ns) {
  thread_local PaceTimer timer;
  const int64_t remaining_ns = deadline_ns - SteadyNs();
  if (remaining_ns > kSpinTailNs) {
    const int64_t wait_ns = remaining_ns - kSpinTailNs;
    if (timer.handle) {
      LARGE_INTEGER due;
      due.QuadPart = -std::max<int64_t>(1, wait_ns / 100);  // relative, in 100 ns units
      if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer.handle, INFINITE);
      }
    } else {
      Sleep(DWORD(wait_ns / 1'000'000));  // whole milliseconds, rounded down: never past the deadline by design
    }
  }
  while (SteadyNs() < deadline_ns) {
    YieldProcessor();
  }
}

}  // namespace

// ---- guest memory --------------------------------------------------------------------------------

bool ImageU32(uint32_t address, uint32_t* out) {
  if (!guest::InImage(address) || (address & 3) != 0) {
    return false;
  }
  rex::memory::Memory* memory = GuestMemory();
  if (!memory) {
    return false;
  }
  uint32_t value = 0;
  std::memcpy(&value, memory->TranslateVirtual<const uint8_t*>(address), sizeof(value));
  *out = __builtin_bswap32(value);
  return true;
}

bool ImageF32(uint32_t address, float* out) {
  uint32_t bits = 0;
  if (!ImageU32(address, &bits)) {
    return false;
  }
  std::memcpy(out, &bits, sizeof(float));
  return true;
}

bool WriteU32(uint32_t address, uint32_t value) {
  uint32_t probe = 0;
  if ((address & 3) != 0 || !guest::U32(address, &probe)) {
    return false;
  }
  rex::memory::Memory* memory = GuestMemory();
  if (!memory) {
    return false;
  }
  const uint32_t big_endian = __builtin_bswap32(value);
  std::memcpy(memory->TranslateVirtual<uint8_t*>(address), &big_endian, sizeof(big_endian));
  std::atomic_thread_fence(std::memory_order_release);
  return true;
}

bool TimerDt(double* dt) {
  float value = 0.0f;
  if (!ImageF32(kTimerDt, &value) || !(value > 0.0f) || !(value <= 0.5f)) {
    return false;
  }
  *dt = double(value);
  return true;
}

// ---- the frame -----------------------------------------------------------------------------------

bool SetSubsteps(int count) {
  uint32_t object = 0;
  if (!ImageU32(kFrameRatePtr, &object) || object < 0x10000 || !WriteU32(object + 0x10, uint32_t(count))) {
    g_substep_fail.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

int EffectiveCap() {
  if (g_hold30.load(std::memory_order_relaxed)) return 30;
  const int cap = SettingCap();
  // P1: the 120 -> 60 tier.
  if (cap > 60 && g_tier60.load(std::memory_order_relaxed)) return 60;
  return cap;
}

void NoteRawDelta(double ms) { t_raw_ms = std::clamp(ms, 0.0, kMaxRawMs); }

int NextStep(int* substeps, int* quantum_hz) {
  double elapsed_ms = t_raw_ms;
  t_raw_ms = -1.0;
  if (elapsed_ms < 0.0) {
    elapsed_ms = FallbackRawMs();
  }
  const int cap = EffectiveCap();  // P1: the grid (1/120 above a 60 cap) follows the cap
  const int period_q = math::PeriodQuanta(cap);
  const int grid = math::QuantumHz(cap) * 10 + period_q;
  const int forced = REXCVAR_GET(mcla_sim_force_n);
  const int forced_substeps = REXCVAR_GET(mcla_physics_substeps);

  std::lock_guard<std::mutex> lock(g_mutex);
  if (grid != g_last_grid) {
    g_last_grid = grid;
    g_last_period_q = period_q;
    g_debt_ms = 0.0;  // the cap changed: the debt was in the old period
  }
  int n = 0;
  bool resync = false;
  if (forced >= 1 && forced <= math::MaxN(cap)) {
    n = forced;
    ++g_win.forced;
  } else {
    const math::Step step = math::QuantiseStepFor(g_debt_ms, elapsed_ms, cap);
    n = step.n;
    resync = step.resync;
    if (resync) ++g_win.resyncs;
    if (n >= 2 && period_q == 1 && !resync) ++g_win.missed;
    if (period_q == 1) {
      ++g_dec_frames;
      if (n >= 2 && !resync) ++g_dec_missed;
    }
  }
  const int subs = forced_substeps >= 1 && forced_substeps <= math::kMaxN ? forced_substeps : math::SubstepsFor(n, cap);
  ++g_win.frames;
  ++g_win.n_hist[n];
  g_win.substeps += uint64_t(subs);
  g_win.world_s += double(n) / double(math::QuantumHz(cap));
  if (substeps) *substeps = subs;
  if (quantum_hz) *quantum_hz = math::QuantumHz(cap);  // P1
  return n;
}

void OnFrameStart(int64_t now_ns) {
  const bool on = Enabled();
  int64_t wake_ns = 0;  // the steady-clock time the pacer sleeps until (0 = no sleep this frame)
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!on) {
      if (g_active) {
        g_active = false;
        g_hold30.store(false);
        g_deadline_ns = 0;
        if (g_wrote_substeps) {
          SetSubsteps(2);  // the shipped count, for the frames that follow
          g_wrote_substeps = false;
        }
        REXLOG_INFO("[mcla-sim] off: the game's own timing again");
      }
      return;
    }
    if (!g_active) {
      g_active = true;
      g_debt_ms = 0.0;
      g_last_period_q = 0;
      g_deadline_ns = 0;
      ResetWindow(now_ns);
      REXLOG_INFO("[mcla-sim] on: world step n/60 s (n = 1..4) with n substeps (above a 60 cap: n/120 s, n = 1..8, "
                  "ceil(n/2) substeps); cap {} (mcla_sim_cap {}), "
                  "pacer {}, force_n {}, substeps {}",
                  EffectiveCap() > 0 ? EffectiveCap() : 60, REXCVAR_GET(mcla_sim_cap),
                  REXCVAR_GET(mcla_sim_pace) ? "on" : "off", REXCVAR_GET(mcla_sim_force_n),
                  REXCVAR_GET(mcla_physics_substeps) ? std::to_string(REXCVAR_GET(mcla_physics_substeps))
                                                      : std::string("auto"));
    }
    // A reset-path frame (loads) does not reach T1: it runs the shipped two substeps.
    if (SetSubsteps(2)) {
      g_wrote_substeps = true;
    }
    // P1: the frame-time counters (real time between engine frames; a gap over 1 s is a load or a suspension).
    if (g_last_frame_ns != 0 && now_ns - g_last_frame_ns < 1'000'000'000) {
      g_win.frame_times.Add(double(now_ns - g_last_frame_ns) / 1e6);
    }
    g_last_frame_ns = now_ns;
    // P1: the end of a 120 -> 60 tier: its time passed.
    if (g_tier60.load() && now_ns >= g_tier_until_ns) {
      g_tier60.store(false);
      g_tier_end_ns = now_ns;
      g_debt_ms = 0.0;
      g_deadline_ns = 0;
      g_dec_start_ns = now_ns;
      g_dec_frames = g_dec_missed = 0;
      REXLOG_INFO("[mcla-sim] cap 60 -> 120 (tier over)");
    }
    if (g_hold30.load() && now_ns >= g_hold_until_ns) {
      g_hold30.store(false);
      g_debt_ms = 0.0;
      g_deadline_ns = 0;
      g_last_hold_end_ns = now_ns;
      g_dec_start_ns = now_ns;
      g_dec_frames = g_dec_missed = 0;
      REXLOG_INFO("[mcla-sim] cap 30 -> {} (hold over)", SettingCap() > 0 ? SettingCap() : 60);
    }
    DecideHold(now_ns);
    const int log_s = std::max(1, REXCVAR_GET(mcla_sim_log_s));
    if (now_ns - g_win.start_ns >= int64_t(log_s) * 1'000'000'000) {
      Report(now_ns);
    }
    if (REXCVAR_GET(mcla_sim_pace)) {
      const int64_t period_ns = int64_t(math::PacePeriodMs(EffectiveCap()) * 1e6);  // P1: 8.33 ms at 120
      int64_t deadline = g_deadline_ns + period_ns;
      if (g_deadline_ns == 0 || now_ns - deadline > period_ns / 2 || deadline - now_ns > 2 * period_ns) {
        if (g_deadline_ns != 0) ++g_win.pace_resyncs;
        deadline = now_ns;  // late by more than half a frame, or a clock jump: start a new grid
      } else if (now_ns < deadline) {
        wake_ns = deadline;
        ++g_win.sleeps;
        g_win.slept_ms += double(deadline - now_ns) / 1e6;
      }
      g_deadline_ns = deadline;
    }
  }
  if (wake_ns > 0) {
    SleepUntil(wake_ns);
  }
}

// ---- smoothing counters --------------------------------------------------------------------------

void NoteSmooth(SmoothKind kind, double dt, bool changed) {
  const int cls = std::fabs(30.0 * dt - 1.0) < 1e-3 ? 0 : std::fabs(60.0 * dt - 1.0) < 1e-3 ? 1 : 2;
  g_smooth_calls[kind].fetch_add(1, std::memory_order_relaxed);
  g_smooth_dt[kind][cls].fetch_add(1, std::memory_order_relaxed);
  if (changed) g_smooth_changed[kind].fetch_add(1, std::memory_order_relaxed);
}

// ---- Flash movies --------------------------------------------------------------------------------

namespace {

// g_ui_mutex held.
UiSlot* FindUi(uint32_t context, bool create) {
  for (int i = 0; i < g_ui_count; ++i) {
    if (g_ui[i].context == context) return &g_ui[i];
  }
  if (!create) return nullptr;
  if (g_ui_count >= kMaxUiRecords) {
    ++g_ui_overflow;
    return nullptr;
  }
  g_ui[g_ui_count] = UiSlot{};
  g_ui[g_ui_count].context = context;
  return &g_ui[g_ui_count++];
}

}  // namespace

bool UiMovieHold(uint32_t context) {
  uint32_t serial = 0;
  float step = 0.0f;
  if (!ImageU32(kTimerFrames, &serial) || !ImageF32(kTimerUnscaled, &step) || !(step > 0.0f) ||
      !(step <= 0.5f)) {
    return false;  // cannot tell: the game's own step
  }
  std::lock_guard<std::mutex> lock(g_ui_mutex);
  UiSlot* slot = FindUi(context, true);
  if (!slot) return false;
  const bool hold = math::UiHold(slot->record, serial, double(step));
  ++slot->calls;
  if (!hold) ++slot->steps;
  return hold;
}

bool UiCountdown(uint32_t context, double game_dt, double* out) {
  std::lock_guard<std::mutex> lock(g_ui_mutex);
  ++g_cd_calls;
  UiSlot* slot = FindUi(context, false);
  if (!slot) {
    ++g_cd_unknown;
    return false;
  }
  *out = double(float(math::UiCountdown(slot->record, game_dt)));
  return true;
}

// ---- the race clock ------------------------------------------------------------------------------

int RaceClockMs(uint32_t race, double dt_ms) {
  const std::string& setting = REXCVAR_GET(mcla_race_clock);
  const math::RaceMode mode = setting == "off"     ? math::RaceMode::kOff
                              : setting == "exact" ? math::RaceMode::kExact
                                                   : math::RaceMode::kRetail;
  std::lock_guard<std::mutex> lock(g_race_mutex);
  RaceSlot* slot = nullptr;
  for (RaceSlot& s : g_race) {
    if (s.race == race) {
      slot = &s;
      break;
    }
  }
  if (!slot) {
    slot = &g_race[g_race_next];
    g_race_next = (g_race_next + 1) % kRaceCarries;
    *slot = RaceSlot{};
    slot->race = race;
  }
  const int ms = math::RaceMs(slot->carry, dt_ms, mode);
  ++g_race_calls;
  g_race_ms += uint64_t(std::max(ms, 0));
  return ms;
}

}  // namespace mcla::sim
