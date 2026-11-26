// mcla_hdr: host-side helpers for the HDR work (the SDK's HDR output: cvar `hdr`, patches hdr-output, hdr-ui-layer and
// hdr-scene-mcla). Nothing here touches the guest: it is a background thread and three cvars.
//
// IN-RUN A/B SWITCHER. Measuring what HDR costs (GPU busy time, frame times) needs the settings under test to change
// WITHIN one run: two runs differ in what the game happens to draw, the thermal state, the clocks. With
// `--mcla_hdr_ab=<spec>` a thread walks through the phases of the spec on a clock and sets the cvars of each phase with
// rex::cvar::SetFlagByName while the game runs; tools/hdr/hdr_ab.py then cuts PresentMon's frames by the phases the log names.
//
//   spec    := phase { ';' phase }
//   phase   := [ label ':' ] [ assignment { ',' assignment } ]
//   assignment := cvar '=' value
//   e.g.    --mcla_hdr_ab="sdr:hdr=off;frame:hdr=on,hdr_ui_layer=false,mcla_hdr_scene=false;full:hdr=on,hdr_ui_layer=true,mcla_hdr_scene=true"
//
// A label is letters, digits and `_ . - +` (no label: p1, p2, ... by position; two phases may share a label, the analysis
// pools them). A value has no ',' or ';' (none of the cvars it is for takes one) and may be wrapped in "" or ''. An empty
// phase (`base:`) is the original settings. Empty segments are ignored.
//
// What a phase is. The settings of a phase are the ORIGINAL values (read before the switcher first sets anything) with the
// phase's own assignments laid over them: a cvar that some phase names and the current phase does not is put back to its
// original value. So a phase means the same thing in every cycle, whatever phase ran before it; naming every cvar in every
// phase is not needed. A cvar is only set when its value changes (the SDK calls the change callbacks of a set even when the
// value is the same, and `hdr`'s makes the presenter ask the display again).
//
// Timeline. t = seconds since the process was created, measured on the steady clock (the process creation time from the
// OS at load, so a slow start counts). Nothing happens before t = mcla_hdr_ab_start_s; from then phase
// floor((t - start) / period) mod n is current (period = mcla_hdr_ab_period_s, n phases), checked every 50 ms, so a
// switch lands up to 50 ms after the boundary. The timeline is on the process clock, not "after free roam": give
// start_s long enough for the game to reach free roam (the analysis can also cut the frames before free roam).
//
// Log (one line per switch, INFO; the log's own wall-clock stamp is what the analysis uses, `t` is the process clock):
//   [mcla-hdr] ab: <cvar>=<value> before the switcher            once per cvar named by any phase, before the first phase
//   [mcla-hdr] ab: armed (mcla_hdr_ab="..."): N phase(s), period P s, first phase at S s after the process start
//   [mcla-hdr] ab phase <label> (<i>/<n>) at <t:.3f> s: <assignments>      i is 1-based; the assignments of the phase as
//                                                      given, then ` (restored: cvar=value,...)` for the ones put back
// A cvar that cannot be set (unknown, a value out of range or not allowed) gets one WARN; the run goes on. A spec that cannot be
// parsed gets one ERROR and the switcher stops for good (it never takes the game down). A mcla_hdr_ab changed while running
// (the console) is parsed again and the timeline carries on; emptied, the switcher stands still and leaves the values as they are.
//
// When it starts. The thread is created by a static object here (before main), but touches no cvar until the first
// MCLA_HOOK_ENTER of the run has announced the hooks (mcla_hooks.h g_announced): by then cvar::Init, the environment and
// the TOML config are applied and the log is up, and the guest is running. (cvar::FinalizeInit / IsFinalized is not used:
// nothing in this SDK calls FinalizeInit.) While mcla_hdr_ab is empty each poll reads one size field and goes back to sleep.
//
// Threading of the cvars (read from the SDK source, src/core/cvar.cpp and include/rex/cvar.h): SetFlagByName, GetFlagByName
// and GetFlagInfo take the registry's recursive mutex for the whole call, the setter and the change callbacks included, so
// they are safe from any thread and against each other; a runtime set (Source::kRuntime) outranks the command line, the
// environment and the config. The change callbacks run on the calling thread under that lock (`hdr`'s only stores atomics).
// What is NOT locked is the readers of the stored value: REXCVAR_GET reads the variable itself, so a render thread reading
// hdr_ui_layer / mcla_hdr_scene (bool) sees the old or the new value, never a torn one on x86-64, and the switch takes
// effect at some frame boundary within a frame or two of the set (the analysis's --skip-after-switch absorbs that). The string
// cvar `hdr` is read by the presenter through its change callback's atomic, and by this file only through GetFlagByName.
//
// At exit. The thread is detached. An atexit handler registered after this file's cvars (so it runs before the cvar
// registry is torn down) stops it and waits up to 200 ms for a poll in progress, so a poll cannot meet a destroyed registry.
//
// Build switch: MCLA_HOOK_HDR=0 compiles the switcher out (the cvars stay, so a command line that names them still parses).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_hooks.h"

#ifndef MCLA_HOOK_HDR
#define MCLA_HOOK_HDR 0
#endif

REXCVAR_DEFINE_STRING(mcla_hdr_ab, "", "MCLA",
                      "MCLA: in-run A/B switcher for HDR measurements. Phases separated by ';', each `[label:]cvar=value"
                      "[,cvar=value...]`, e.g. \"sdr:hdr=off;full:hdr=on,hdr_ui_layer=true\". The phases cycle on a clock "
                      "(mcla_hdr_ab_period_s, mcla_hdr_ab_start_s); every switch is logged as `[mcla-hdr] ab phase <label> "
                      "(i/n) at <t> s: ...` (tools/hdr/hdr_ab.py reads them). A cvar a phase does not name goes back to its "
                      "original value. Empty (default) = off");
REXCVAR_DEFINE_DOUBLE(mcla_hdr_ab_period_s, 4.0, "MCLA",
                      "MCLA: seconds each phase of mcla_hdr_ab lasts (default 4; at least 0.1)");
REXCVAR_DEFINE_DOUBLE(mcla_hdr_ab_start_s, 0.0, "MCLA",
                      "MCLA: seconds since the process was created before the first phase of mcla_hdr_ab is applied (default 0: "
                      "from the first poll after the game starts running; give the time the game needs to reach free roam)");

namespace {

mcla::hooks::Registration g_registration("hdr-ab", "mcla_hdr_ab", MCLA_HOOK_HDR != 0,
                                         [] { return !rex::cvar::GetFlagByName("mcla_hdr_ab").empty(); },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_HDR

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kPollInterval = std::chrono::milliseconds(50);
constexpr double kMinPeriodSeconds = 0.1;
constexpr double kDefaultPeriodSeconds = 4.0;
constexpr int kExitWaitMilliseconds = 200;

// ---- the spec ----------------------------------------------------------------------------------------------------------

struct Assignment {
  std::string name;
  std::string value;
};

struct Phase {
  std::string label;
  std::vector<Assignment> assignments;
};

std::string_view Trim(std::string_view s) {
  constexpr const char* kBlank = " \t\r\n";
  const size_t first = s.find_first_not_of(kBlank);
  if (first == std::string_view::npos) {
    return {};
  }
  return s.substr(first, s.find_last_not_of(kBlank) - first + 1);
}

std::string_view Unquote(std::string_view s) {
  if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

bool IsNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool IsLabelChar(char c) { return IsNameChar(c) || c == '.' || c == '-' || c == '+'; }

bool AllOf(std::string_view s, bool (*ok)(char)) { return !s.empty() && std::all_of(s.begin(), s.end(), ok); }

// Splits `text` at `separator`; the pieces are views into it (a trailing separator gives a last empty piece).
std::vector<std::string_view> Split(std::string_view text, char separator) {
  std::vector<std::string_view> pieces;
  size_t begin = 0;
  while (true) {
    const size_t end = text.find(separator, begin);
    if (end == std::string_view::npos) {
      pieces.push_back(text.substr(begin));
      return pieces;
    }
    pieces.push_back(text.substr(begin, end - begin));
    begin = end + 1;
  }
}

// False with a text in `error` when the spec is not in the format above.
bool ParseSpec(std::string_view spec, std::vector<Phase>& phases, std::string& error) {
  for (std::string_view segment : Split(spec, ';')) {
    segment = Trim(segment);
    if (segment.empty()) {
      continue;
    }
    Phase phase;
    std::string_view body = segment;
    // `label:` is a prefix only when the first ':' comes before the first '=' (a value may hold a ':').
    const size_t colon = segment.find(':');
    const size_t equals = segment.find('=');
    if (colon != std::string_view::npos && (equals == std::string_view::npos || colon < equals)) {
      const std::string_view label = Trim(segment.substr(0, colon));
      if (!AllOf(label, IsLabelChar)) {
        error = "the label '" + std::string(label) + "' of phase " + std::to_string(phases.size() + 1) +
                " is empty or uses characters other than letters, digits and _ . - +";
        return false;
      }
      phase.label = std::string(label);
      body = segment.substr(colon + 1);
    } else {
      phase.label = "p" + std::to_string(phases.size() + 1);
    }
    for (std::string_view token : Split(body, ',')) {
      token = Trim(token);
      if (token.empty()) {
        continue;
      }
      const size_t eq = token.find('=');
      if (eq == std::string_view::npos) {
        error = "'" + std::string(token) + "' in phase " + phase.label + " is not cvar=value";
        return false;
      }
      std::string_view name = Trim(token.substr(0, eq));
      if (name.starts_with("--")) {
        name.remove_prefix(2);
      }
      if (!AllOf(name, IsNameChar)) {
        error = "'" + std::string(name) + "' in phase " + phase.label + " is not a cvar name";
        return false;
      }
      const std::string value(Unquote(Trim(token.substr(eq + 1))));
      const auto same = std::find_if(phase.assignments.begin(), phase.assignments.end(),
                                     [&](const Assignment& a) { return a.name == name; });
      if (same != phase.assignments.end()) {
        same->value = value;  // the last one wins
      } else {
        phase.assignments.push_back({std::string(name), value});
      }
    }
    phases.push_back(std::move(phase));
  }
  if (phases.empty()) {
    error = "no phase in it";
    return false;
  }
  return true;
}

// ---- the clock ---------------------------------------------------------------------------------------------------------

// The steady-clock instant the process was created at: now minus the age the OS gives (GetProcessTimes' creation time against
// the system time). Falls back to now (this file's static initialisation, milliseconds into the process) when that fails or
// is not plausible.
Clock::time_point ProcessStart() {
  static const Clock::time_point start = [] {
    const Clock::time_point now = Clock::now();
    FILETIME created, exited, kernel, user, current;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
      GetSystemTimeAsFileTime(&current);
      const auto to_u64 = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
      const uint64_t from = to_u64(created);
      const uint64_t to = to_u64(current);
      if (to >= from && to - from < 60ull * 10000000ull) {  // 100 ns units; under a minute
        return now - std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds((to - from) * 100));
      }
    }
    return now;
  }();
  return start;
}

double SecondsSinceStart() { return std::chrono::duration<double>(Clock::now() - ProcessStart()).count(); }

// ---- the switcher ------------------------------------------------------------------------------------------------------

// A cvar some phase names.
struct Tracked {
  std::string name;
  std::string original;  // its value before the switcher set anything (GetFlagByName's text)
  std::string applied;   // what the switcher last set it to (the original at first)
  bool known = true;     // a cvar of this build
  bool warned = false;   // the WARN for it was written
};

class AbSwitcher {
 public:
  // One poll. False = stop for good.
  bool Poll() {
    if (!mcla::hooks::g_announced.load(std::memory_order_relaxed)) {
      return true;  // the guest has not started: the cvars may not be parsed yet
    }
    if (REXCVAR_GET(mcla_hdr_ab).empty()) {
      return true;  // the cheap check: one size field, no lock
    }
    const std::string spec = rex::cvar::GetFlagByName("mcla_hdr_ab");
    if (spec.empty()) {
      return true;
    }
    if (spec != spec_ && !Arm(spec)) {
      return false;
    }
    double period = 0.0;
    double start = 0.0;
    ReadTiming(period, start);
    const double t = SecondsSinceStart();
    if (t < start) {
      return true;
    }
    const size_t index = size_t(uint64_t(std::floor((t - start) / period)) % phases_.size());
    if (index != current_) {
      current_ = index;
      Apply(index, t);
    }
    return true;
  }

 private:
  void ReadTiming(double& period, double& start) {
    period = REXCVAR_GET(mcla_hdr_ab_period_s);
    start = REXCVAR_GET(mcla_hdr_ab_start_s);
    const bool period_ok = std::isfinite(period) && period >= kMinPeriodSeconds;
    const bool start_ok = std::isfinite(start) && start >= 0.0;
    if ((!period_ok || !start_ok) && !warned_timing_) {
      warned_timing_ = true;
      REXLOG_WARN("[mcla-hdr] ab: mcla_hdr_ab_period_s={} / mcla_hdr_ab_start_s={} out of range: using {:.3f} s / {:.3f} s", period,
                  start, period_ok ? period : (std::isfinite(period) ? kMinPeriodSeconds : kDefaultPeriodSeconds),
                  start_ok ? start : 0.0);
    }
    if (!period_ok) {
      period = std::isfinite(period) ? kMinPeriodSeconds : kDefaultPeriodSeconds;
    }
    if (!start_ok) {
      start = 0.0;
    }
  }

  Tracked* FindTracked(std::string_view name) {
    for (Tracked& tracked : tracked_) {
      if (tracked.name == name) {
        return &tracked;
      }
    }
    return nullptr;
  }

  bool Arm(const std::string& spec) {
    std::vector<Phase> parsed;
    std::string error;
    if (!ParseSpec(spec, parsed, error)) {
      REXLOG_ERROR(
          "[mcla-hdr] ab: mcla_hdr_ab=\"{}\" is not usable: {}. The switcher is off for this run (format: "
          "[label:]cvar=value[,cvar=value...][;next phase...])",
          spec, error);
      return false;
    }
    // The originals of every cvar any phase names, before the first set.
    for (const Phase& phase : parsed) {
      for (const Assignment& assignment : phase.assignments) {
        if (FindTracked(assignment.name)) {
          continue;
        }
        Tracked tracked;
        tracked.name = assignment.name;
        tracked.known = rex::cvar::GetFlagInfo(assignment.name) != nullptr;
        if (tracked.known) {
          tracked.original = rex::cvar::GetFlagByName(assignment.name);
          tracked.applied = tracked.original;
          REXLOG_INFO("[mcla-hdr] ab: {}={} before the switcher", tracked.name, tracked.original);
        } else {
          tracked.warned = true;
          REXLOG_WARN("[mcla-hdr] ab: '{}' is not a cvar of this build: its assignments do nothing", tracked.name);
        }
        tracked_.push_back(std::move(tracked));
      }
    }
    spec_ = spec;
    phases_ = std::move(parsed);
    current_ = kNoPhase;
    double period = 0.0;
    double start = 0.0;
    ReadTiming(period, start);
    REXLOG_INFO("[mcla-hdr] ab: armed (mcla_hdr_ab=\"{}\"): {} phase(s), period {:.3f} s, first phase at {:.3f} s after the process start",
                spec_, phases_.size(), period, start);
    return true;
  }

  void Set(Tracked& tracked, const std::string& value) {
    if (!tracked.known || tracked.applied == value) {
      return;
    }
    if (rex::cvar::SetFlagByName(tracked.name, value)) {
      tracked.applied = value;
      return;
    }
    if (!tracked.warned) {
      tracked.warned = true;
      REXLOG_WARN("[mcla-hdr] ab: cvar '{}' was not set to '{}' (the value is out of its range or not allowed, or the cvar cannot be "
                  "changed now); no more warnings for it",
                  tracked.name, value);
    }
  }

  void Apply(size_t index, double t) {
    const Phase& phase = phases_[index];
    std::string own;
    for (const Assignment& assignment : phase.assignments) {
      if (Tracked* tracked = FindTracked(assignment.name)) {
        Set(*tracked, assignment.value);
      }
      own += (own.empty() ? "" : ",") + assignment.name + "=" + assignment.value;
    }
    std::string restored;
    for (Tracked& tracked : tracked_) {
      const bool named = std::any_of(phase.assignments.begin(), phase.assignments.end(),
                                     [&](const Assignment& a) { return a.name == tracked.name; });
      if (named || !tracked.known || tracked.applied == tracked.original) {
        continue;
      }
      Set(tracked, tracked.original);
      restored += (restored.empty() ? "" : ",") + tracked.name + "=" + tracked.original;
    }
    std::string text = own;
    if (!restored.empty()) {
      text += std::string(text.empty() ? "" : " ") + "(restored: " + restored + ")";
    }
    if (text.empty()) {
      text = "(no assignments)";
    }
    REXLOG_INFO("[mcla-hdr] ab phase {} ({}/{}) at {:.3f} s: {}", phase.label, index + 1, phases_.size(), t, text);
  }

  static constexpr size_t kNoPhase = SIZE_MAX;

  std::string spec_;  // the spec phases_ was parsed from
  std::vector<Phase> phases_;
  std::vector<Tracked> tracked_;
  size_t current_ = kNoPhase;
  bool warned_timing_ = false;
};

// ---- the thread --------------------------------------------------------------------------------------------------------

std::atomic<bool> g_stop{false};
std::atomic<bool> g_busy{false};  // a poll is running (it may touch the cvar registry and the log)

void ThreadMain() {
  try {
    AbSwitcher switcher;
    while (!g_stop.load()) {
      std::this_thread::sleep_for(kPollInterval);
      // busy first, then the stop test: either this poll sees the stop, or StopAtExit sees busy and waits for it.
      g_busy.store(true);
      const bool go = !g_stop.load() && switcher.Poll();
      g_busy.store(false);
      if (!go) {
        return;
      }
    }
  } catch (const std::exception& e) {
    g_busy.store(false);
    try {
      REXLOG_ERROR("[mcla-hdr] ab: the switcher stopped on an exception: {}", e.what());
    } catch (...) {
    }
  } catch (...) {
    g_busy.store(false);
  }
}

void StopAtExit() {
  g_stop.store(true);
  for (int i = 0; i < kExitWaitMilliseconds && g_busy.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// Defined after the cvars above: its atexit handler is registered after theirs, so it runs before they are unregistered.
struct Starter {
  Starter() {
    (void)ProcessStart();  // pin the process start now
    std::atexit(&StopAtExit);
    try {
      std::thread(&ThreadMain).detach();
    } catch (...) {
      // no thread: no switcher, the game is not affected
    }
  }
};

Starter g_starter;

}  // namespace

#endif  // MCLA_HOOK_HDR
