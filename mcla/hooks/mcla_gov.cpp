// Midnight Club: Los Angeles: the GPU frame governor (LOCK60 A5 / F1), glue. The control law (the two-way tier / VRS ladder, the
// enrichment rule, the learned tier costs, the back-off, the pop hiding) is the pure, unit-tested mcla_gov_math.h; this file resolves
// the SDK's exports, reads the cvars, writes the two actuators and the log lines. See mcla_gov.h for the contract and
// mcla_gov_math.h for the law.
//
// Inputs (data symbols of rexgpu-xenos.dll, C linkage, SDK patches 04f0 / 0500; every one may be missing on an older SDK):
//   rexgpu_gpu_frame_us   GPU BUSY time of the last frame whose timestamps were read back, microseconds, published 2-4 frames late (0 = none)
//   rexgpu_gpu_frame_seq  bumped after each publish: a new value from a repeated one, and the count of frames
//   rexgpu_gpu_frame_lag  how many frames old the published value is
//   rexgpu_vrs_levels     how many variable-rate-shading levels above 0 exist (0 = none on this GPU / backend)
// Outputs:
//   rexgpu_vrs_level      WRITTEN here: -1 = not governed (the SDK's own cvar decides), 1..N coarser. -1 whenever the governor is off or
//                         the ladder is at VRS 0, so a user's own VRS cvar keeps working at the base.
//   mcla_gfx_tier         WRITTEN here with SetFlagByName (mcla_gfx_tune.cpp applies it on its next engine frame and logs '[mcla-gfx] tier
//                         N: ...'; CurrentTier() reads back what it applied). Put back to what it was (-1, or the tier the user
//                         configured) when the governor turns off, around a config save (BeforeConfigSave) and at process exit.
// Missing GPU-time symbols: no GPU signal, so the fallback is the missed-frames signal (last_n >= 2) in the control law; the enrichment
// needs the GPU signal and does not run. Missing VRS symbols: the ladder has no VRS rungs (tiers only).
//
// Screen-change grace (mcla_gov_screen_grace_frames): after a change of the driving flag the math layer discards the previous screen's
// samples and, for that many engine frames, records and decides nothing; this file only passes the cvar (and prints it in the summary).
//
// While the governor is on, mcla_gfx_tier is its own: the tier table overrides the knob cvars (mcla_gfx_env_interval, ...), even ones set
// by hand. To choose the tier range, set mcla_gov_tier_floor / mcla_gov_tier_ceiling (or mcla_gfx_tier BEFORE the governor first runs:
// with the floor on auto that value is the floor).
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
#include <cstdlib>
#include <mutex>
#include <set>
#include <cctype>
#include <string>
#include <thread>
#include <vector>

#include "nfsmw_pch.h"

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_gfx_tune.h"
#include "mcla_gov.h"
#include "mcla_gov_math.h"

REXCVAR_DECLARE(int32_t, mcla_gfx_tier);

REXCVAR_DEFINE_BOOL(mcla_gov, false, "MCLA",
                    "MCLA GPU frame governor master switch (LOCK60 A5 / F1). Off (default) = does nothing, mcla_gfx_tier and "
                    "variable-rate shading are back as they were and the 60 -> 30 hold of mcla_sim_pacer decides alone. On = closed "
                    "loop on the GPU frame time (SDK export rexgpu_gpu_frame_us; without it, on missed frames) on one two-way ladder "
                    "of the detail tier (mcla_gfx_tier, which it then owns) and variable-rate shading: it sheds, fast, enriched tiers "
                    "first, then VRS, then the tiers below the floor; restores slowly in the reverse order; enriches above the floor "
                    "(up to mcla_gov_tier_ceiling) while there is headroom; and lets the 60 -> 30 hold start only once the last level "
                    "is reached and the GPU is still over budget. Live");
REXCVAR_DEFINE_DOUBLE(mcla_gov_shed_ms, 15.6, "MCLA",
                      "MCLA governor: shed one rung when the GPU frame time (the median of the last 4 samples) is above this, in ms "
                      "(the 60 fps budget is 16.7, minus ~0.7 ms of presenter paint outside the measured frame), or when "
                      "mcla_gov_shed_over_count of the last 30 samples are. The value is the GPU's BUSY time of the frame (SDK 04f0: its "
                      "lists' spans without the idle gaps between them; at a frame cap the GPU idles inside the frame while the CPU "
                      "produces it). Two missed frames in the last 30 shed too. Live");
REXCVAR_DEFINE_INT32(mcla_gov_shed_over_count, 3, "MCLA",
                     "MCLA governor, the tail rule: shed one rung when at least this many of the last 30 GPU frame times (0.5 s) are above "
                     "mcla_gov_shed_ms, whatever their median. Under CPU or power contention the GPU frame is bimodal (p50 15.2 ms, "
                     "one frame in ten at 25 ms) and the median never sheds. A lone spike in a clean stream is not enough. 0 = off. "
                     "Live");
REXCVAR_DEFINE_DOUBLE(mcla_gov_restore_ms, 15.0, "MCLA",
                      "MCLA governor: a restore step (one rung back) is only considered while the GPU frame time stays below this, "
                      "in ms: its p90 over the whole of mcla_gov_restore_s. Also the line an enrichment must stay under (with the "
                      "predicted cost and the margin added). Keep it clearly under mcla_gov_shed_ms (the slack between them is the "
                      "hysteresis). Live");
REXCVAR_DEFINE_INT32(mcla_gov_shed_frames, 6, "MCLA",
                     "MCLA governor: the fewest GPU frames between two steps before the next shed (the SDK's timing value is "
                     "2-4 frames old, so the effect of a step shows after about 5 frames). Live");
REXCVAR_DEFINE_DOUBLE(mcla_gov_restore_s, 4.0, "MCLA",
                      "MCLA governor: seconds below mcla_gov_restore_ms, driving, with no missed frame, before one rung is given "
                      "back. A restore that is followed within 2 s by a shed back to that rung (a probe bounce: with slack the GPU "
                      "clocks down and the frame time rises) doubles the wait before the next restore from it, up to 60 s; the "
                      "doubling decays 30 s after the wait it lengthened. Live");
REXCVAR_DEFINE_INT32(mcla_gov_vrs_levels, 2, "MCLA",
                     "MCLA governor: how many variable-rate-shading levels the ladder uses, 1..N coarser (invisible, ~0.4 / ~0.6 ms for "
                     "1 / 2); 0 = none. At most what the GPU / SDK has (rexgpu_vrs_levels). These are the first rungs below the base. Live");
REXCVAR_DEFINE_INT32(mcla_gov_tier_floor, -1, "MCLA",
                     "MCLA governor: the floor F, the tier of the base rung (1 = perf ... 5). -1 = auto: the tier mcla_gfx_tier was set to "
                     "(>= 1) before the governor first ran, else 1. The tiers below it (F-1 .. 1) are the last, visible rungs of the shed "
                     "order. The governor starts here. Live");
REXCVAR_DEFINE_INT32(mcla_gov_tier_ceiling, 4, "MCLA",
                     "MCLA governor: the ceiling C, the richest tier enrichment may reach (1..5; 5 = 1.5x distances, 'ultra'). At or "
                     "below the floor = no enrichment (the picture never goes above the configured one). Live");
REXCVAR_DEFINE_DOUBLE(mcla_gov_enrich_s, 10.0, "MCLA",
                      "MCLA governor: seconds of driving at the base or above, with no missed frame and no change, over which the p90 of the "
                      "GPU frame time (plus the predicted cost of the next tier and the margin) must fit under mcla_gov_restore_ms "
                      "before the tier is raised one step. Live");
REXCVAR_DEFINE_DOUBLE(mcla_gov_enrich_margin_ms, 1.5, "MCLA",
                      "MCLA governor: headroom in ms an enrichment must keep under mcla_gov_restore_ms after the predicted cost of the "
                      "next tier is added to the p90. Live");
REXCVAR_DEFINE_DOUBLE(mcla_gov_enrich_hold_s, 30.0, "MCLA",
                      "MCLA governor: a tier entered by enrichment and shed again within this many seconds is barred from enrichment for "
                      "30 s, doubling with every repeat up to 600 s and decaying one step per period it stays quiet. Live");
REXCVAR_DEFINE_DOUBLE(mcla_gov_learn_s, 3.0, "MCLA",
                      "MCLA governor: seconds of GPU frame time measured before and after every tier step (the first 0.5 s after it "
                      "skipped) to learn what the tier really costs; the enrichment plans with the larger of this and the table's estimate. "
                      "0 = no learning. Live");
REXCVAR_DEFINE_DOUBLE(mcla_gov_cut_m, 20.0, "MCLA",
                      "MCLA governor: a camera that moves more than this many metres in one engine frame is a cut, a hiding moment for a "
                      "step across a tier that pops (needs the camera feed, mcla::gov::NoteCamera). Live");
REXCVAR_DEFINE_DOUBLE(mcla_gov_pop_speed_kmh, 90.0, "MCLA",
                      "MCLA governor: above this camera speed in km/h a step across a tier that pops is hidden by the motion (needs the "
                      "camera feed, mcla::gov::NoteCamera). Live");
REXCVAR_DEFINE_DOUBLE(mcla_gov_pop_wait_s, 1.5, "MCLA",
                      "MCLA governor: the longest a SHED across a tier that pops waits for a hiding moment (a pause or load, the first "
                      "second after one, a camera cut, speed) before it goes anyway; a missed frame sends it at once. Enrichment and "
                      "restore across such a tier wait without a limit. Live");
REXCVAR_DEFINE_INT32(mcla_gov_screen_grace_frames, 60, "MCLA",
                     "MCLA governor, the screen-change grace: after any change of the driving flag (a load or the pause menu ending or "
                     "starting) every GPU sample and missed-frame record taken before it is thrown away (the median, the tail window, the "
                     "missed history, the restore / enrich windows, the cost learning), and for this many engine frames of the new screen "
                     "nothing is recorded and nothing is shed, restored or enriched: the samples of the previous screen and the hitchy "
                     "first frames of the new one are no judge of its load (field: entering driving shed 0 -> 4 in 0.3 s). 0 = off. Live");
REXCVAR_DEFINE_INT32(mcla_gov_max_level, -1, "MCLA",
                     "MCLA governor: the highest rung the closed loop may shed to (1.. = VRS rungs, then the tiers below the floor). -1 = "
                     "the whole ladder; 0 = never shed (the governor only enriches and logs). The 60 -> 30 hold is allowed once this rung "
                     "is reached and the GPU is still over budget. Live");
REXCVAR_DEFINE_INT32(mcla_gov_force_level, -1, "MCLA",
                     "MCLA governor: pin this shed rung (0 = the base, 1.. = VRS rungs, then the tiers below the floor) and stop the closed "
                     "loop: for A/B tests and screenshots (pin tier T with mcla_gov_tier_floor=T and this = 0). -1 = closed loop. Live");
REXCVAR_DEFINE_INT32(mcla_gov_log_s, 5, "MCLA",
                     "MCLA governor: seconds between the '[mcla-gov]' summary lines (time at each tier / VRS state, GPU frame time p50 / "
                     "p95, steps, signal in use); every 6th also lists the learned tier costs. 0 = only the steps. Every step is logged "
                     "either way");

namespace mcla::gov {
namespace {

constexpr const char* kTierCvar = "mcla_gfx_tier";

std::int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- the SDK's symbols -----------------------------------------------------------------------------------------------------

struct Symbols {
  std::atomic<uint32_t>* gpu_us = nullptr;
  std::atomic<uint32_t>* seq = nullptr;
  std::atomic<uint32_t>* lag = nullptr;
  std::atomic<int32_t>* vrs_level = nullptr;
  std::atomic<int32_t>* vrs_levels = nullptr;
  std::string module = "none";  // where the first one was found
  bool GpuTime() const { return gpu_us && seq; }
};

static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<int32_t>::is_always_lock_free,
              "the SDK's counters are plain words shared across the DLL boundary");

// Like mcla_fence_spin.cpp's FindEventWord: the GPU plugin first (rexgpu-xenos.dll, postfix d / rd in Debug / RelWithDebInfo), the
// runtime second. Resolved once, at the first enabled frame (the plugin is loaded long before the first guest frame).
void* FindSymbol(const char* name, std::string* module) {
  for (const wchar_t* base : {L"rexgpu-xenos", L"rexruntime"}) {
    for (const wchar_t* postfix : {L"", L"d", L"rd"}) {
      const std::wstring file = std::wstring(base) + postfix + L".dll";
      HMODULE handle = GetModuleHandleW(file.c_str());
      if (!handle) continue;
      if (FARPROC symbol = GetProcAddress(handle, name)) {
        if (module && *module == "none") {
          module->assign(file.begin(), file.end());  // ASCII names
        }
        return reinterpret_cast<void*>(symbol);
      }
    }
  }
  return nullptr;
}

Symbols Resolve() {
  Symbols s;
  s.gpu_us = static_cast<std::atomic<uint32_t>*>(FindSymbol("rexgpu_gpu_frame_us", &s.module));
  s.seq = static_cast<std::atomic<uint32_t>*>(FindSymbol("rexgpu_gpu_frame_seq", &s.module));
  s.lag = static_cast<std::atomic<uint32_t>*>(FindSymbol("rexgpu_gpu_frame_lag", &s.module));
  s.vrs_level = static_cast<std::atomic<int32_t>*>(FindSymbol("rexgpu_vrs_level", &s.module));
  s.vrs_levels = static_cast<std::atomic<int32_t>*>(FindSymbol("rexgpu_vrs_levels", &s.module));
  return s;
}

// ---- state shared with other threads (BeforeConfigSave, Shutdown, the accessors) --------------------------------------------

std::atomic<int> g_level{0};
std::atomic<int> g_tier{0};
std::atomic<int> g_vrs{0};
std::atomic<bool> g_hold_allowed{true};
std::atomic<std::atomic<int32_t>*> g_vrs_ptr{nullptr};  // rexgpu_vrs_level, once resolved
std::atomic<std::int64_t> g_release_until_ns{0};        // BeforeConfigSave: the governor leaves mcla_gfx_tier alone until then
std::atomic<bool> g_shutdown{false};
std::atomic<std::int64_t> g_cam_seq{0};
std::atomic<double> g_cam_x{0}, g_cam_y{0}, g_cam_z{0};

// mcla_gfx_tier: what the governor wrote and what to put back. Any thread (the game thread writes it; BeforeConfigSave and Shutdown
// give it back from others): g_tier_mutex guards everything below.
struct TierCvar {
  bool held = false;             // the governor's value is in the cvar
  bool base_default = true;      // the cvar's source was the compiled-in default before the governor wrote it: ResetToDefault puts it back
  std::string base_text = "-1";  // its value before the governor wrote it
  int base_value = -1;
  std::string written_text;      // what the cvar read back after the governor's write
};
std::mutex g_tier_mutex;
TierCvar g_tier_cvar;

void TierCapture() {  // the cvar as the user left it, before the governor's first write
  std::lock_guard lock(g_tier_mutex);
  if (g_tier_cvar.held) return;
  g_tier_cvar.base_text = rex::cvar::GetFlagByName(kTierCvar);
  g_tier_cvar.base_default = rex::cvar::GetFlagSource(kTierCvar) == rex::cvar::Source::kDefault;
  g_tier_cvar.base_value = REXCVAR_GET(mcla_gfx_tier);
}

bool TierWrite(int tier) {
  std::lock_guard lock(g_tier_mutex);
  if (!rex::cvar::SetFlagByName(kTierCvar, std::to_string(tier))) return false;
  g_tier_cvar.held = true;
  g_tier_cvar.written_text = rex::cvar::GetFlagByName(kTierCvar);
  return true;
}

// Gives the cvar back: its default (with the "never set" provenance) or the value it had. A value changed by hand meanwhile is theirs.
// Returns the tier mcla_gfx_tune is to apply (-1 = off).
int TierRelease() {
  std::lock_guard lock(g_tier_mutex);
  if (g_tier_cvar.held) {
    g_tier_cvar.held = false;
    if (rex::cvar::GetFlagByName(kTierCvar) == g_tier_cvar.written_text) {
      if (g_tier_cvar.base_default) {
        rex::cvar::ResetToDefault(kTierCvar);
      } else {
        rex::cvar::SetFlagByName(kTierCvar, g_tier_cvar.base_text);
      }
    }
  }
  return g_tier_cvar.base_default ? -1 : g_tier_cvar.base_value;
}

void PutVrs(int32_t value) {
  if (std::atomic<int32_t>* p = g_vrs_ptr.load(std::memory_order_acquire)) p->store(value, std::memory_order_release);
}

// ---- the state (the game thread only) ------------------------------------------------------------------------------------------

constexpr int kTierUnset = -1;      // nothing written yet
constexpr int kTierReleased = -2;   // given back for a config save

struct State {
  bool active = false;  // the governor ran on the last frame (so that turning it off puts everything back)
  bool resolved = false;
  Symbols sym;
  math::Controller ctrl;
  math::CameraMotion cam;
  std::int64_t cam_seq_seen = 0;
  std::int64_t cam_last_ns = 0;
  double cam_speed = -1.0;
  bool seq_valid = false;
  uint32_t last_seq = 0;
  int32_t vrs_written = -1;
  int tier_state = kTierUnset;  // the tier written to the cvar, kTierReleased, or kTierUnset
  int floor_auto = 1;           // the floor with mcla_gov_tier_floor = -1
  int tier_mismatch_frames = 0;
  bool tier_mismatch_warned = false;
  std::int64_t external_warn_ns = 0;
  bool start_line = false;
  std::int64_t next_learn_log_ns = 0;
  int learn_log_counter = 0;
  std::array<double, math::kTiers> tier_est{};
  std::array<bool, math::kTiers> tier_pop_free{};
  std::set<std::string> warned;
};

State g;

void WarnOnce(State& s, const std::string& key, const std::string& text) {
  if (s.warned.insert(key).second) REXLOG_WARN("[mcla-gov] {}", text);
}

// ---- the actuators -----------------------------------------------------------------------------------------------------------

// The VRS level of the state: 0 = full rate = not governed (-1: the SDK's own cvar decides), K >= 1 = level K.
void WriteVrs(State& s, int vrs) {
  if (!s.sym.vrs_level) return;
  const int32_t want = vrs >= 1 ? vrs : -1;
  if (want == s.vrs_written) return;
  s.sym.vrs_level->store(want, std::memory_order_release);
  s.vrs_written = want;
}

// Brings mcla_gfx_tier to the tier the governor wants (or gives it back during a config save). Every frame: cheap, three compares.
void WriteTier(State& s, int tier, std::int64_t now_ns) {
  const bool released = SteadyNs() < g_release_until_ns.load(std::memory_order_relaxed);
  if (released) {
    if (s.tier_state != kTierReleased) {
      TierRelease();
      s.tier_state = kTierReleased;
      REXLOG_INFO("[mcla-gov] config save: mcla_gfx_tier given back for 2 s (the governor writes tier {} again after)", tier);
    }
    return;
  }
  bool write = s.tier_state != tier;
  if (!write && REXCVAR_GET(mcla_gfx_tier) != tier) {
    // Someone else wrote the cvar (a console, the settings overlay, a script): the governor owns it while it is on.
    if (now_ns - s.external_warn_ns > 10'000'000'000LL) {
      s.external_warn_ns = now_ns;
      REXLOG_WARN("[mcla-gov] mcla_gfx_tier was set to {} by someone else; the governor owns it while mcla_gov is on, put back to {} "
                  "(choose the range with mcla_gov_tier_floor / mcla_gov_tier_ceiling)",
                  REXCVAR_GET(mcla_gfx_tier), tier);
    }
    write = true;
  }
  if (write) {
    if (!TierWrite(tier)) {
      WarnOnce(s, "tier rejected", fmt::format("mcla_gfx_tier = {} was rejected by the cvar: the tier is not applied", tier));
    }
    s.tier_state = tier;
    s.tier_mismatch_frames = 0;
  }
  // The read-back: mcla_gfx_tune applies the tier on its next engine frame.
  if (gfxtune::CurrentTier() == tier) {
    s.tier_mismatch_frames = 0;
  } else if (++s.tier_mismatch_frames == 120 && !s.tier_mismatch_warned) {
    s.tier_mismatch_warned = true;
    REXLOG_WARN("[mcla-gov] mcla_gfx_tune has not applied tier {} after 120 frames (it reports {}): is the heartbeat hook running?", tier,
                gfxtune::CurrentTier());
  }
}

void RestoreAll(State& s) {
  TierRelease();
  s.tier_state = kTierUnset;
  WriteVrs(s, 0);
}

// ---- the parameters ------------------------------------------------------------------------------------------------------------

math::Params MakeParams(const State& s) {
  math::Params p;
  p.shed_ms = std::max(1.0, REXCVAR_GET(mcla_gov_shed_ms));
  p.restore_ms = std::max(0.1, REXCVAR_GET(mcla_gov_restore_ms));
  p.shed_frames = std::clamp<int32_t>(REXCVAR_GET(mcla_gov_shed_frames), 1, 600);
  p.shed_over_count = std::clamp<int32_t>(REXCVAR_GET(mcla_gov_shed_over_count), 0, math::kTailSamples);
  p.restore_s = std::clamp(REXCVAR_GET(mcla_gov_restore_s), 0.1, 600.0);
  const int floor = REXCVAR_GET(mcla_gov_tier_floor);
  p.tier_floor = floor >= math::kMinTier ? std::min(floor, math::kMaxTier) : s.floor_auto;
  p.tier_ceiling = std::clamp<int32_t>(REXCVAR_GET(mcla_gov_tier_ceiling), 0, math::kMaxTier);
  const int sdk_vrs = s.sym.vrs_level && s.sym.vrs_levels ? std::max<int32_t>(0, s.sym.vrs_levels->load(std::memory_order_relaxed)) : 0;
  p.vrs_levels = std::min(std::clamp<int32_t>(REXCVAR_GET(mcla_gov_vrs_levels), 0, 8), sdk_vrs);
  p.max_level = REXCVAR_GET(mcla_gov_max_level);
  p.force_level = REXCVAR_GET(mcla_gov_force_level);
  p.enrich_s = std::clamp(REXCVAR_GET(mcla_gov_enrich_s), 0.1, 600.0);
  p.enrich_margin_ms = REXCVAR_GET(mcla_gov_enrich_margin_ms);
  p.enrich_hold_s = std::max(0.0, REXCVAR_GET(mcla_gov_enrich_hold_s));
  p.learn_s = std::clamp(REXCVAR_GET(mcla_gov_learn_s), 0.0, 30.0);
  p.pop_wait_s = std::max(0.0, REXCVAR_GET(mcla_gov_pop_wait_s));
  p.pop_speed_kmh = REXCVAR_GET(mcla_gov_pop_speed_kmh);
  p.screen_grace_frames = std::clamp<int32_t>(REXCVAR_GET(mcla_gov_screen_grace_frames), 0, 3600);
  p.tier_est_ms = s.tier_est;
  p.tier_pop_free = s.tier_pop_free;
  return p;
}

// ---- the log -------------------------------------------------------------------------------------------------------------------

const char* KindText(math::Kind kind) {
  switch (kind) {
    case math::Kind::kShed: return "shed";
    case math::Kind::kRestore: return "restore";
    case math::Kind::kEnrich: return "enrich";
    default: return "move";
  }
}

std::string LearnedText(const math::Controller& c, int tier) {
  return c.HasLearned(tier) ? fmt::format("+{:.2f} ms ({}x)", c.LearnedMs(tier), c.LearnedCount(tier)) : std::string("none yet");
}

void LogChange(const State& s, const math::Change& ch, const math::Params& p, std::int64_t now_ns) {
  std::string detail;
  const std::string stat = ch.stat_name;
  if (stat == "tail") {
    detail = fmt::format("busy: {} of the last {} samples over {:.1f} ms (largest {:.1f} ms; needs {})", ch.stat_n, ch.stat_window, p.shed_ms,
                         ch.stat_ms, p.shed_over_count);
  } else if (stat == "missed") {
    detail = fmt::format("{} of the last {} engine frames missed", ch.stat_n, ch.stat_window);
    if (s.ctrl.SignalLive()) detail += fmt::format(", busy last {:.1f} ms", s.ctrl.LastSampleMs());
  } else if (stat == "p90" && ch.kind == math::Kind::kEnrich) {
    detail = fmt::format("busy p90 {:.1f} ms over {} samples", ch.stat_ms, ch.stat_n);
  } else if (!stat.empty()) {
    detail = fmt::format("busy {} {:.1f} ms of the last {}", stat, ch.stat_ms, ch.stat_n);
  } else if (ch.signal_gpu) {
    detail = fmt::format("busy last {:.1f} ms", s.ctrl.LastSampleMs());
  } else {
    detail = "no gpu signal";
  }
  // The cost of the tier step: the table's estimate or what was learned, whichever the enrichment plans with.
  if (ch.from_tier != ch.to_tier) {
    const int tier = std::max(ch.from_tier, ch.to_tier);
    const double predicted = ch.kind == math::Kind::kEnrich ? ch.predicted_ms : s.ctrl.PredictedCostMs(tier, p);
    detail += fmt::format(", predicted +{:.1f} ms (tier {}: est +{:.1f}, learned {})", predicted, tier, p.tier_est_ms[size_t(tier)],
                          LearnedText(s.ctrl, tier));
    if (ch.kind == math::Kind::kEnrich) {
      detail += fmt::format(", needs p90 + predicted + {:.1f} ms margin <= {:.1f} ms", p.enrich_margin_ms, p.restore_ms);
    }
  }
  detail += fmt::format(", missed {}", ch.missed);
  if (ch.bounce) {
    detail += fmt::format(", bounce: a restore from tier {} vrs {} now waits {:.0f} s", ch.to_tier, ch.to_vrs,
                          s.ctrl.RestoreDelayS(math::State{ch.to_tier, ch.to_vrs}, now_ns, p));
  }
  if (ch.backoff_s > 0.0) {
    detail += fmt::format(", tier {} was enriched less than {:.0f} s ago: enrichment into it barred for {:.0f} s", ch.from_tier,
                          p.enrich_hold_s, ch.backoff_s);
  }
  if (ch.hide != math::Hide::kNone || ch.waited_s > 0.0) {
    detail += fmt::format(", across a popping tier{}: {}", ch.waited_s > 0.0 ? fmt::format(" after waiting {:.1f} s", ch.waited_s) : std::string(),
                          math::HideText(ch.hide));
  }
  REXLOG_INFO("[mcla-gov] tier {} -> {} / vrs {} -> {} ({}: {}); rung {} -> {}", ch.from_tier, ch.to_tier, ch.from_vrs, ch.to_vrs,
              ch.kind == math::Kind::kOther || ch.kind == math::Kind::kNone ? math::ReasonText(ch.reason) : KindText(ch.kind), detail,
              ch.from, ch.to);
}

void LogWait(const math::Change& ch, const math::Params& p) {
  REXLOG_INFO("[mcla-gov] tier {} -> {} / vrs {} -> {} ({}) waits for a hiding moment (a pause, load or menu, the first second after "
              "one, a camera cut, speed over {:.0f} km/h): {}",
              ch.from_tier, ch.to_tier, ch.from_vrs, ch.to_vrs, KindText(ch.kind), p.pop_speed_kmh,
              ch.kind == math::Kind::kShed ? fmt::format("at most {:.1f} s, a missed frame sends it at once", p.pop_wait_s)
                                           : std::string("no time limit"));
}

void LogLearn(const State& s, const math::Change& ch, const math::Params& p) {
  const int tier = ch.learn_tier;
  REXLOG_INFO("[mcla-gov] learned tier {} over tier {}: +{:.2f} ms (busy p50 {:.2f} -> {:.2f} ms over {} / {} samples); now {} (est +{:.1f})",
              tier, tier - 1, ch.learn_delta_ms, ch.learn_before_ms, ch.learn_after_ms, ch.learn_n_before, ch.learn_n_after,
              LearnedText(s.ctrl, tier), p.tier_est_ms[size_t(tier)]);
}

void LogLearned(const State& s, const math::Params& p, std::int64_t now_ns) {
  std::string out;
  for (int tier = math::kMinTier + 1; tier <= math::kMaxTier; ++tier) {
    if (!out.empty()) out += "; ";
    out += fmt::format("tier {}: est +{:.1f}, learned {}", tier, p.tier_est_ms[size_t(tier)], LearnedText(s.ctrl, tier));
    const double left = s.ctrl.BackoffLeftS(tier, now_ns);
    const double period = s.ctrl.BackoffPeriodS(tier, now_ns);
    if (left > 0.0) {
      out += fmt::format(", barred {:.0f} s more (period {:.0f} s)", left, period);
    } else if (period > 0.0) {
      out += fmt::format(", back-off decaying (period {:.0f} s)", period);
    }
  }
  REXLOG_INFO("[mcla-gov] tier costs: {}", out);
}

void LogWindow(const math::Window& w, const math::Controller& c) {
  std::string times;
  for (const auto& [key, ns] : w.time_ns) {
    if (!times.empty()) times += ", ";
    times += fmt::format("tier {} vrs {}: {:.1f} s", key / 100, key % 100, double(ns) / 1e9);
  }
  const std::string gpu = w.p50_ms >= 0.0 ? fmt::format("busy p50 {:.1f} p95 {:.1f} ms", w.p50_ms, w.p95_ms) : std::string("busy p50 - p95 - ms");
  REXLOG_INFO("[mcla-gov] {:.0f} s: tier {} vrs {} ({}), rung {}..{}, {} ({} samples, lag {}), sheds {} restores {} enriches {} bounces {} waits {}, "
              "missed {}, signal {}, grace {} frames",
              double(w.length_ns) / 1e9, c.Tier(), c.Vrs(), times, w.min_level, w.max_level, gpu, w.samples, w.lag, w.sheds, w.restores,
              w.enriches, w.bounces, w.waits, w.missed, w.signal_gpu ? "gpu" : "missed-frames", w.grace_frames);
}

void LogStart(const State& s, const math::Params& p) {
  const math::Ladder lad = math::Ladder::Of(p);
  REXLOG_INFO("[mcla-gov] on: shed above {:.1f} ms (median of 4; or {} of the last 30 samples; or 2 of the last 30 frames missed), restore below {:.1f} ms for {:.1f} s, {} GPU frames between sheds; ladder: floor tier {} ({}), "
              "ceiling tier {} ({}), vrs levels {} (rexgpu_vrs_levels {}), rungs {}..{} (cap {}, pin {}); enrich over {:.1f} s with a {:.1f} ms "
              "margin, hold {:.0f} s, learn {:.1f} s; pop hiding: cut > {:.0f} m, speed > {:.0f} km/h, shed waits <= {:.1f} s, camera feed {}; screen grace {} frames; "
              "symbols ({}): rexgpu_gpu_frame_us {}, rexgpu_gpu_frame_seq {}, rexgpu_gpu_frame_lag {}, rexgpu_vrs_level {}, rexgpu_vrs_levels {}{}",
              p.shed_ms, p.shed_over_count, p.restore_ms, p.restore_s, p.shed_frames, lad.floor,
              REXCVAR_GET(mcla_gov_tier_floor) >= math::kMinTier ? "mcla_gov_tier_floor" : s.floor_auto > 1 ? "auto: the configured mcla_gfx_tier" : "auto: perf",
              lad.ceiling, lad.enrich > 0 ? "enrichment on" : "no enrichment", lad.vrs,
              s.sym.vrs_levels ? s.sym.vrs_levels->load(std::memory_order_relaxed) : 0, -lad.enrich, lad.shed, p.max_level, p.force_level,
              p.enrich_s, p.enrich_margin_ms, p.enrich_hold_s, p.learn_s, REXCVAR_GET(mcla_gov_cut_m), p.pop_speed_kmh, p.pop_wait_s,
              g_cam_seq.load(std::memory_order_relaxed) != 0 ? "active" : "none yet (only the driving flag hides a pop)", p.screen_grace_frames,
              s.sym.module,
              s.sym.gpu_us ? "found" : "MISSING", s.sym.seq ? "found" : "MISSING", s.sym.lag ? "found" : "missing",
              s.sym.vrs_level ? "found" : "MISSING", s.sym.vrs_levels ? "found" : "MISSING",
              s.sym.GpuTime() ? "" : "; no GPU frame time: the missed-frames signal (last_n >= 2) drives the sheds and nothing enriches");
}

// ---- on / off -------------------------------------------------------------------------------------------------------------------

void AtExit() {
  try {
    Shutdown();
  } catch (...) {
  }
}

void Activate(State& s) {
  if (!s.resolved) {
    s.resolved = true;
    s.sym = Resolve();
    g_vrs_ptr.store(s.sym.vrs_level, std::memory_order_release);
    int n = 0;
    const gfxtune::TierInfo* tiers = gfxtune::Tiers(&n);
    for (int i = 0; i < n; ++i) {
      const int t = tiers[i].tier;
      if (t < 0 || t >= math::kTiers) continue;
      s.tier_est[size_t(t)] = double(tiers[i].est_gpu_ms_over_prev);
      s.tier_pop_free[size_t(t)] = tiers[i].runtime_pop_free;
    }
    std::atexit(AtExit);  // the exit through exit(); the window-close path is std::_Exit and runs none (nothing is saved then either)
  }
  g_shutdown.store(false, std::memory_order_relaxed);
  TierCapture();
  // The floor on auto: the tier the user configured before the governor first wrote the cvar (>= 1), else the tier of the
  // configured preset (mcla_gfx_preset: perf / perf+ / lowest = 1, quality = 4, ultra = 5, none / default = 4: the shipped
  // distances and every-frame env map and far cascade, which tier 4 is, plus FXAA), so the preset stays the floor of normal play.
  {
    std::lock_guard lock(g_tier_mutex);
    if (g_tier_cvar.base_value >= math::kMinTier) {
      s.floor_auto = std::min(g_tier_cvar.base_value, math::kMaxTier);
    } else {
      std::string preset = rex::cvar::GetFlagByName("mcla_gfx_preset");
      for (char& c : preset) c = char(std::tolower(static_cast<unsigned char>(c)));
      s.floor_auto = preset == "ultra" ? 5 : (preset == "quality" || preset.empty() || preset == "default" ||
                                              preset == "none" || preset == "shipped") ? 4 : math::kMinTier;
    }
  }
  s.ctrl = math::Controller();
  s.cam = math::CameraMotion();
  s.cam_seq_seen = 0;
  s.cam_speed = -1.0;
  s.seq_valid = false;
  s.tier_state = kTierUnset;
  s.tier_mismatch_frames = 0;
  s.tier_mismatch_warned = false;
  s.active = true;
  s.start_line = true;
  s.next_learn_log_ns = 0;
  s.learn_log_counter = 0;
}

void Deactivate(State& s) {
  RestoreAll(s);
  s.ctrl = math::Controller();
  s.active = false;
  g_level.store(0, std::memory_order_relaxed);
  g_tier.store(0, std::memory_order_relaxed);
  g_vrs.store(0, std::memory_order_relaxed);
  g_hold_allowed.store(true, std::memory_order_relaxed);
  REXLOG_INFO("[mcla-gov] off: mcla_gfx_tier and variable-rate shading put back");
}

}  // namespace

void OnFrame(int64_t now_ns, int last_n, bool driving) {
  State& s = g;
  if (g_shutdown.load(std::memory_order_relaxed)) return;
  if (!REXCVAR_GET(mcla_gov)) {
    if (s.active) Deactivate(s);
    return;
  }
  if (!s.active) Activate(s);

  const math::Params p = MakeParams(s);
  if (s.start_line) {
    s.start_line = false;
    LogStart(s, p);
  }

  math::Input in;
  in.now_ns = now_ns;
  in.driving = driving;
  in.missed = last_n >= 2;
  if (s.sym.GpuTime()) {
    const uint32_t seq = s.sym.seq->load(std::memory_order_acquire);
    if (!s.seq_valid || seq != s.last_seq) {
      const uint32_t us = s.sym.gpu_us->load(std::memory_order_acquire);
      in.seq_delta = s.seq_valid ? uint32_t(seq - s.last_seq) : 1u;
      s.last_seq = seq;
      s.seq_valid = true;
      if (us > 0) {
        in.new_sample = true;
        in.gpu_ms = double(us) / 1000.0;
      }
    }
    if (s.sym.lag) in.lag = int(s.sym.lag->load(std::memory_order_relaxed));
  }
  // The camera (TODO: the feed; see NoteCamera): a cut and the speed, when a position arrived this frame.
  if (const std::int64_t cam_seq = g_cam_seq.load(std::memory_order_acquire); cam_seq != 0 && cam_seq != s.cam_seq_seen) {
    s.cam_seq_seen = cam_seq;
    s.cam_last_ns = now_ns;
    const math::CameraMotion::Result r = s.cam.Update(now_ns, g_cam_x.load(std::memory_order_relaxed), g_cam_y.load(std::memory_order_relaxed),
                                                      g_cam_z.load(std::memory_order_relaxed), REXCVAR_GET(mcla_gov_cut_m));
    in.cut = r.cut;
    s.cam_speed = r.speed_kmh;
  }
  in.speed_kmh = now_ns - s.cam_last_ns <= 500'000'000 ? s.cam_speed : -1.0;  // a feed that stopped says nothing

  const math::Change change = s.ctrl.Step(in, p);
  if (change.changed) LogChange(s, change, p, now_ns);
  if (change.wait_started) LogWait(change, p);
  if (change.learn_done) LogLearn(s, change, p);

  WriteVrs(s, s.ctrl.Vrs());
  WriteTier(s, s.ctrl.Tier(), now_ns);
  g_level.store(s.ctrl.Level(), std::memory_order_relaxed);
  g_tier.store(s.ctrl.Tier(), std::memory_order_relaxed);
  g_vrs.store(s.ctrl.Vrs(), std::memory_order_relaxed);
  g_hold_allowed.store(s.ctrl.HoldAllowed(p), std::memory_order_relaxed);

  const int log_s = REXCVAR_GET(mcla_gov_log_s);
  if (log_s > 0 && now_ns - s.ctrl.WindowStartNs() >= int64_t(log_s) * 1'000'000'000) {
    LogWindow(s.ctrl.TakeWindow(now_ns), s.ctrl);
    if (++s.learn_log_counter % 6 == 0) LogLearned(s, p, now_ns);
  }
}

bool Enabled() { return REXCVAR_GET(mcla_gov); }

int Level() { return g_level.load(std::memory_order_relaxed); }
int Tier() { return g_tier.load(std::memory_order_relaxed); }
int Vrs() { return g_vrs.load(std::memory_order_relaxed); }

bool HoldAllowed() { return !Enabled() || g_hold_allowed.load(std::memory_order_relaxed); }

void NoteCamera(double x, double y, double z) {
  g_cam_x.store(x, std::memory_order_relaxed);
  g_cam_y.store(y, std::memory_order_relaxed);
  g_cam_z.store(z, std::memory_order_relaxed);
  g_cam_seq.fetch_add(1, std::memory_order_release);
}

bool CameraFeedActive() { return g_cam_seq.load(std::memory_order_relaxed) != 0; }

void BeforeConfigSave() {
  try {
    if (!REXCVAR_GET(mcla_gov) || !g.active) return;
    g_release_until_ns.store(SteadyNs() + 2'000'000'000LL, std::memory_order_relaxed);
    const int wanted = TierRelease();  // the tier mcla_gfx_tune is to be at (-1 = off)
    // The game thread applies the cvar (and so the knobs) on its next engine frame.
    const std::int64_t deadline = SteadyNs() + 250'000'000LL;
    while (gfxtune::CurrentTier() != wanted && SteadyNs() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } catch (...) {
  }
}

void Shutdown() {
  g_shutdown.store(true, std::memory_order_relaxed);
  if (!g.resolved) return;
  TierRelease();
  PutVrs(-1);
}

}  // namespace mcla::gov
