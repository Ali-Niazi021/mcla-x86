// Midnight Club: Los Angeles: the GPU frame governor (LOCK60 A5), glue. The control law and the level-table parser are the pure,
// unit-tested mcla_gov_math.h; this file resolves the SDK's exports, reads the cvars, applies a level's settings and writes the log
// lines. See mcla_gov.h for the contract and mcla_gov_math.h for the law.
//
// Inputs (data symbols of rexgpu-xenos.dll, C linkage, SDK patches 04f0 / 0500; every one may be missing on an older SDK):
//   rexgpu_gpu_frame_us   GPU time of the last frame whose timestamps were read back, microseconds, published 2-4 frames late (0 = none)
//   rexgpu_gpu_frame_seq  bumped after each publish: a new value from a repeated one, and the count of frames
//   rexgpu_gpu_frame_lag  how many frames old the published value is
//   rexgpu_vrs_levels     how many variable-rate-shading levels above 0 exist (0 = none on this GPU / backend)
// Output:
//   rexgpu_vrs_level      WRITTEN here: -1 = not governed (the SDK's own cvar decides), 0 = full rate, 1..N coarser. -1 whenever the
//                         governor is off or the level has no vrs entry, so a user's own VRS cvar keeps working.
// Missing GPU-time symbols: no GPU signal, so the fallback is the missed-frames signal (last_n >= 2) in the control law. Missing VRS
// symbols: `vrs=` entries are ignored (the levels that only had them are skipped as equal to the one before).
//
// The levels. `mcla_gov_levels` is the table of shed levels 1..N and `mcla_gov_rich_levels` the table of the levels -1..-R richer than
// the configured picture (v1: parsed and logged, never entered unless mcla_gov_allow_rich is set; empty by default, so nothing
// happens). A level lists the settings that HOLD there; an actuator it does not mention is at its base value: for a cvar the value it
// had when the governor first set it (restored through ResetToDefault when it was still at its default, so the preset's default and
// its "set by hand" provenance come back, else with SetFlagByName), for VRS -1. A cvar someone else changed meanwhile is left alone on
// the way back (the new value is theirs), and a cvar that does not exist or rejects the value is skipped with one warning.
// While a level above 0 holds a cvar, its source is "runtime" (a settings UI that saves the config would write it; the governor puts
// every knob back when it is turned off).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nfsmw_pch.h"

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_gov.h"
#include "mcla_gov_math.h"

REXCVAR_DEFINE_BOOL(mcla_gov, false, "MCLA",
                    "MCLA GPU frame governor master switch (LOCK60 A5). Off (default) = does nothing, every knob it changed is back, "
                    "variable-rate shading is left to its own cvar and the 60 -> 30 hold of mcla_sim_pacer decides alone. On = closed "
                    "loop on the GPU frame time (SDK export rexgpu_gpu_frame_us; without it, on missed frames): sheds one level of "
                    "mcla_gov_levels when the GPU frame nears the budget, restores slowly, and lets the 60 -> 30 hold start only "
                    "once the last level is reached and the GPU is still over budget. Live");
REXCVAR_DEFINE_DOUBLE(mcla_gov_shed_ms, 15.6, "MCLA",
                      "MCLA governor: shed one level when the GPU frame time (the median of the last 4 samples) is above this, in ms "
                      "(the 60 fps budget is 16.7, minus ~0.7 ms of presenter paint outside the measured frame). The value is the GPU's "
                      "BUSY time of the frame (SDK 04f0: its lists' spans without the idle gaps between them; at a frame cap the GPU "
                      "idles inside the frame while the CPU produces it). A missed frame sheds too when the last sample is within 1 ms "
                      "below it. Live");
REXCVAR_DEFINE_DOUBLE(mcla_gov_restore_ms, 15.0, "MCLA",
                      "MCLA governor: a restore step (one level back) is only considered while the GPU frame time stays below this, "
                      "in ms: its p90 over the whole of mcla_gov_restore_s. Keep it clearly under mcla_gov_shed_ms (the slack between "
                      "them is the hysteresis). Live");
REXCVAR_DEFINE_INT32(mcla_gov_shed_frames, 6, "MCLA",
                     "MCLA governor: the fewest GPU frames between two level changes before the next shed (the SDK's timing value is "
                     "2-4 frames old, so the effect of a step shows after about 5 frames). Live");
REXCVAR_DEFINE_DOUBLE(mcla_gov_restore_s, 4.0, "MCLA",
                      "MCLA governor: seconds below mcla_gov_restore_ms, driving, with no missed frame, before one level is given "
                      "back. A restore that is followed within 2 s by a shed back to that level (a probe bounce: with slack the GPU "
                      "clocks down and the frame time rises) doubles the wait before the next restore from it, up to 60 s; the "
                      "doubling decays 30 s after the wait it lengthened. Live");
REXCVAR_DEFINE_STRING(
    mcla_gov_levels,
    // VRS 1-2 first: pop-free and invisible (run lk-vrs1: -0.4 / -0.6 ms; the SDK has no scene levels: slower and blurred).
    // Then the live knobs. At integration with VISUALS' run-time detail tier (mcla_gfx_tier), the slow steps become
    // `mcla_gfx_tier=N` entries (one tier per level, never below tier 1 here; the 30 hold stays the last resort).
    "vrs=1;vrs=2;vrs=2,mcla_gfx_env_interval=4,mcla_gfx_far_cascades_interval=3;"
    "vrs=2,mcla_gfx_env_interval=4,mcla_gfx_far_cascades_interval=3,mcla_gfx_shadow_dist=0.45,mcla_gfx_city_lod=0.55",
    "MCLA",
    "MCLA governor: the shed levels, ordered by visibility. Levels separated by ';', each a comma-separated list of name=value "
    "settings that HOLD at that level (absolute, not deltas; a setting a level does not mention is at its base value). The first "
    "level of the string is level 1; level 0 is the configured picture. vrs=K is the variable-rate-shading level (ignored when the GPU "
    "or SDK has none, clamped to the levels it has); any other name is a cvar, set while the level holds and put back to the value it "
    "had when the governor first set it. A level whose settings equal the previous level's (VRS unsupported) is skipped. '|' for "
    "';', '+' for ',' and ':' for '=' are accepted too (for command lines that use the first three), so no exponents in values. Live");
REXCVAR_DEFINE_STRING(mcla_gov_rich_levels, "", "MCLA",
                      "MCLA governor: levels richer than the configured picture (levels -1, -2, ... in the order written), same syntax "
                      "as mcla_gov_levels. Only entered with mcla_gov_allow_rich. Empty (default) = none. Reserved for the quality tiers "
                      "above the perf preset");
REXCVAR_DEFINE_BOOL(mcla_gov_allow_rich, false, "MCLA",
                    "MCLA governor: let the restore step go below level 0 into mcla_gov_rich_levels (v1: off; the governor never goes "
                    "richer than the configured picture). Live");
REXCVAR_DEFINE_INT32(mcla_gov_max_level, -1, "MCLA",
                     "MCLA governor: the highest level the closed loop may shed to. -1 = the last level of mcla_gov_levels (after skipping "
                     "equal ones); 0 = never shed (the governor only measures and logs). The 60 -> 30 hold is allowed once this level is "
                     "reached and the GPU is still over budget. Live");
REXCVAR_DEFINE_INT32(mcla_gov_force_level, -1, "MCLA",
                     "MCLA governor: pin this level (0 = the configured picture, up to the last level of the table) and stop the closed loop: "
                     "for A/B tests and screenshots. -1 = closed loop. Live");
REXCVAR_DEFINE_INT32(mcla_gov_log_s, 5, "MCLA",
                     "MCLA governor: seconds between the '[mcla-gov]' summary lines (time at each level, GPU frame time p50 / p95, level "
                     "changes, signal in use). 0 = only the level changes. Every level change is logged either way");

namespace mcla::gov {
namespace {

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

// ---- the state (the game thread only; the atomics below are read from anywhere) ----------------------------------------------

struct Knob {
  std::string base;          // the value (text of GetFlagByName) when the governor took the cvar
  bool base_default = true;  // its source was the compiled-in or app default: ResetToDefault puts it back
  std::string written;       // the value the cvar read back after the governor's write
  bool held = false;
};

struct State {
  bool active = false;  // the governor ran on the last frame (so that turning it off puts everything back)
  bool resolved = false;
  Symbols sym;
  math::Controller ctrl;
  math::ParsedTable table, rich;
  bool tables_valid = false;
  bool start_line = false;  // the next table log is the start line (symbols and thresholds with it)
  std::string levels_text, rich_text;
  int vrs_levels = 0;  // rexgpu_vrs_levels as the tables were parsed with it
  bool seq_valid = false;
  uint32_t last_seq = 0;
  int32_t vrs_written = -1;
  std::map<std::string, Knob> knobs;
  std::set<std::string> warned;
};

State g;
std::atomic<int> g_level{0};
std::atomic<bool> g_hold_allowed{true};

const math::LevelSettings& SettingsOf(const State& s, int level) {
  static const math::LevelSettings kBase;
  if (level > 0 && size_t(level) <= s.table.levels.size()) return s.table.levels[size_t(level) - 1];
  if (level < 0 && size_t(-level) <= s.rich.levels.size()) return s.rich.levels[size_t(-level) - 1];
  return kBase;
}

void WarnOnce(State& s, const std::string& key, const std::string& text) {
  if (s.warned.insert(key).second) REXLOG_WARN("[mcla-gov] {}", text);
}

// ---- the actuators -----------------------------------------------------------------------------------------------------------

void WriteVrs(State& s, int want) {
  if (!s.sym.vrs_level || want == s.vrs_written) return;
  s.sym.vrs_level->store(want, std::memory_order_release);
  s.vrs_written = want;
}

void SetKnob(State& s, const std::string& name, const std::string& value) {
  Knob& knob = s.knobs[name];
  if (!knob.held) {
    if (!rex::cvar::GetFlagInfo(name)) {
      WarnOnce(s, "unknown " + name, "level setting '" + name + "' is not a cvar: ignored");
      return;
    }
    knob.base = rex::cvar::GetFlagByName(name);
    knob.base_default = rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault;
  }
  if (!rex::cvar::SetFlagByName(name, value)) {
    WarnOnce(s, "rejected " + name, "level setting '" + name + "=" + value + "' was rejected by the cvar: ignored");
    return;
  }
  knob.held = true;
  knob.written = rex::cvar::GetFlagByName(name);
}

void RestoreKnob(const std::string& name, Knob& knob) {
  if (!knob.held) return;
  knob.held = false;
  if (rex::cvar::GetFlagByName(name) != knob.written) return;  // changed by hand while held: the new value is theirs
  if (knob.base_default) {
    rex::cvar::ResetToDefault(name);  // the value and the "never set" provenance (the preset's app default comes back with it)
  } else {
    rex::cvar::SetFlagByName(name, knob.base);
  }
}

// What holds at `level`: its vrs, its cvars; every touched cvar it does not mention goes back.
void ApplyLevel(State& s, int level) {
  const math::LevelSettings& settings = SettingsOf(s, level);
  WriteVrs(s, settings.vrs >= 0 ? settings.vrs : -1);
  for (const math::Setting& setting : settings.cvars) SetKnob(s, setting.name, setting.value);
  for (auto& [name, knob] : s.knobs) {
    if (knob.held && !settings.Find(name)) RestoreKnob(name, knob);
  }
}

void RestoreAll(State& s) {
  for (auto& [name, knob] : s.knobs) RestoreKnob(name, knob);
  WriteVrs(s, -1);
}

// ---- the tables ---------------------------------------------------------------------------------------------------------------

std::string DescribeTable(const math::ParsedTable& t, int sign) {
  std::string out;
  for (size_t i = 0; i < t.levels.size(); ++i) {
    if (i) out += " | ";
    out += fmt::format("{}: {}", sign * int(i + 1), math::Describe(t.levels[i]));
  }
  if (out.empty()) out = "none";
  if (!t.skipped.empty()) {
    out += "; skipped as equal to the level before:";
    for (int position : t.skipped) out += fmt::format(" #{}", position);
    if (t.vrs_ignored > 0) out += fmt::format(" (vrs unsupported: {} entries ignored)", t.vrs_ignored);
  } else if (t.vrs_ignored > 0) {
    out += fmt::format("; vrs unsupported: {} entries ignored", t.vrs_ignored);
  }
  return out;
}

// Re-parses when a table string or the VRS level count changed (all live). True when it did.
bool RefreshTables(State& s) {
  const std::string& levels = REXCVAR_GET(mcla_gov_levels);
  const std::string& rich = REXCVAR_GET(mcla_gov_rich_levels);
  const int vrs_levels = s.sym.vrs_levels ? std::max<int32_t>(0, s.sym.vrs_levels->load(std::memory_order_relaxed)) : 0;
  if (s.tables_valid && vrs_levels == s.vrs_levels && levels == s.levels_text && rich == s.rich_text) return false;
  s.table = math::ParseLevelTable(levels, vrs_levels);
  s.rich = math::ParseLevelTable(rich, vrs_levels);
  s.levels_text = levels;
  s.rich_text = rich;
  s.vrs_levels = vrs_levels;
  s.tables_valid = true;
  for (const std::string& w : s.table.warnings) REXLOG_WARN("[mcla-gov] mcla_gov_levels, {}", w);
  for (const std::string& w : s.rich.warnings) REXLOG_WARN("[mcla-gov] mcla_gov_rich_levels, {}", w);
  const std::string rich_text_log =
      s.rich.levels.empty() ? std::string()
                            : fmt::format("; rich levels ({}): {}", REXCVAR_GET(mcla_gov_allow_rich) ? "on" : "off", DescribeTable(s.rich, -1));
  if (s.start_line) {
    s.start_line = false;
    REXLOG_INFO("[mcla-gov] on: shed above {:.1f} ms, restore below {:.1f} ms for {:.1f} s, {} GPU frames between sheds, cap {}, pin {}; "
                "symbols ({}): rexgpu_gpu_frame_us {}, rexgpu_gpu_frame_seq {}, rexgpu_gpu_frame_lag {}, rexgpu_vrs_level {}, "
                "rexgpu_vrs_levels {} = {}{}; levels: {}{}",
                REXCVAR_GET(mcla_gov_shed_ms), REXCVAR_GET(mcla_gov_restore_ms), REXCVAR_GET(mcla_gov_restore_s),
                REXCVAR_GET(mcla_gov_shed_frames), REXCVAR_GET(mcla_gov_max_level), REXCVAR_GET(mcla_gov_force_level), s.sym.module,
                s.sym.gpu_us ? "found" : "MISSING", s.sym.seq ? "found" : "MISSING", s.sym.lag ? "found" : "missing",
                s.sym.vrs_level ? "found" : "MISSING", s.sym.vrs_levels ? "found" : "MISSING", vrs_levels,
                s.sym.GpuTime() ? "" : "; no GPU frame time: the missed-frames signal (last_n >= 2) drives the levels",
                DescribeTable(s.table, 1), rich_text_log);
  } else {
    REXLOG_INFO("[mcla-gov] table changed (rexgpu_vrs_levels {}): {}{}", vrs_levels, DescribeTable(s.table, 1), rich_text_log);
  }
  return true;
}

math::Params MakeParams(const State& s) {
  math::Params p;
  p.shed_ms = std::max(1.0, REXCVAR_GET(mcla_gov_shed_ms));
  p.restore_ms = std::max(0.1, REXCVAR_GET(mcla_gov_restore_ms));
  p.shed_frames = std::clamp<int32_t>(REXCVAR_GET(mcla_gov_shed_frames), 1, 600);
  p.restore_s = std::clamp(REXCVAR_GET(mcla_gov_restore_s), 0.1, 600.0);
  const int shed_levels = int(s.table.levels.size());
  const int cap = REXCVAR_GET(mcla_gov_max_level);
  p.max_level = cap >= 0 ? std::min(cap, shed_levels) : shed_levels;
  p.min_level = REXCVAR_GET(mcla_gov_allow_rich) ? -int(s.rich.levels.size()) : 0;
  const int force = REXCVAR_GET(mcla_gov_force_level);
  p.force_level = force >= 0 ? std::min(force, shed_levels) : -1;
  return p;
}

// ---- the log -------------------------------------------------------------------------------------------------------------------

void LogChange(const State& s, const math::Change& ch, const math::Params& p, int64_t now_ns) {
  std::string detail;
  const std::string stat = ch.stat_name;
  if (stat == "last") {
    detail = fmt::format("gpu {:.1f} ms (last sample)", ch.stat_ms);
  } else if (!stat.empty()) {
    detail = fmt::format("gpu {} {:.1f} ms of the last {}", stat, ch.stat_ms, ch.stat_n);
  } else if (ch.signal_gpu) {
    detail = fmt::format("gpu last {:.1f} ms", s.ctrl.LastSampleMs());
  } else {
    detail = "no gpu signal";
  }
  std::string tail;
  if (ch.bounce) {
    tail = fmt::format(", bounce: a restore from level {} now waits {:.0f} s", ch.to, s.ctrl.RestoreDelayS(ch.to, now_ns, p));
  }
  const math::LevelSettings& settings = SettingsOf(s, ch.to);
  REXLOG_INFO("[mcla-gov] level {} -> {} ({}: {}, missed {}{}): {}", ch.from, ch.to, math::ReasonText(ch.reason), detail, ch.missed, tail,
              ch.to == 0 ? std::string("the configured picture") : math::Describe(settings));
}

void LogWindow(const math::Window& w) {
  std::string levels = w.min_level == w.max_level ? fmt::format("{}", w.min_level) : fmt::format("{}..{}", w.min_level, w.max_level);
  std::string times;
  for (const auto& [level, ns] : w.time_ns) {
    if (!times.empty()) times += ", ";
    times += fmt::format("{}: {:.1f} s", level, double(ns) / 1e9);
  }
  std::string gpu = w.p50_ms >= 0.0 ? fmt::format("gpu p50 {:.1f} p95 {:.1f} ms", w.p50_ms, w.p95_ms) : std::string("gpu p50 - p95 - ms");
  REXLOG_INFO("[mcla-gov] {:.0f} s: level {} ({}), {} ({} samples, lag {}), sheds {} restores {} bounces {}, missed {}, signal {}",
              double(w.length_ns) / 1e9, levels, times, gpu, w.samples, w.lag, w.sheds, w.restores, w.bounces, w.missed,
              w.signal_gpu ? "gpu" : "missed-frames");
}

void Activate(State& s) {
  if (!s.resolved) {
    s.resolved = true;
    s.sym = Resolve();
  }
  s.ctrl = math::Controller();
  s.seq_valid = false;
  s.active = true;
  s.tables_valid = false;  // parsed again by the RefreshTables that follows, which logs the start line
  s.start_line = true;
}

void Deactivate(State& s) {
  RestoreAll(s);
  s.ctrl = math::Controller();
  s.active = false;
  g_level.store(0, std::memory_order_relaxed);
  g_hold_allowed.store(true, std::memory_order_relaxed);
  REXLOG_INFO("[mcla-gov] off: every knob put back, variable-rate shading left to its own cvar");
}

}  // namespace

void OnFrame(int64_t now_ns, int last_n, bool driving) {
  State& s = g;
  if (!REXCVAR_GET(mcla_gov)) {
    if (s.active) Deactivate(s);
    return;
  }
  if (!s.active) Activate(s);
  if (RefreshTables(s)) ApplyLevel(s, s.ctrl.Level());  // the level in force may mean something else now

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

  const math::Params p = MakeParams(s);
  const math::Change change = s.ctrl.Step(in, p);
  if (change.changed) {
    ApplyLevel(s, change.to);
    g_level.store(change.to, std::memory_order_relaxed);
    LogChange(s, change, p, now_ns);
  }
  g_hold_allowed.store(s.ctrl.HoldAllowed(p), std::memory_order_relaxed);

  const int log_s = REXCVAR_GET(mcla_gov_log_s);
  if (log_s > 0 && now_ns - s.ctrl.WindowStartNs() >= int64_t(log_s) * 1'000'000'000) {
    LogWindow(s.ctrl.TakeWindow(now_ns));
  }
}

bool Enabled() { return REXCVAR_GET(mcla_gov); }

int Level() { return g_level.load(std::memory_order_relaxed); }

bool HoldAllowed() { return !Enabled() || g_hold_allowed.load(std::memory_order_relaxed); }

}  // namespace mcla::gov
