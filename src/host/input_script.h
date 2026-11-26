// mcla-x86 host: scripted controller input.
//
// Port of the iOS port's macOS harness input ($IOS mac/src/mac_input_script.h + mac_input.cpp). The script language is
// the same, so the flows under tools/flows keep working:
//
//   --input-file=<path>      one step (or several, ';'-separated) per line; a '#' starts a comment
//   --input=<script>         steps separated by ';' (ad-hoc runs)
//   --input-repeat=<spec>    repeating steps, ',' separated (ad-hoc runs)
//
// A line of an --input-file that starts with '#' is a comment: the flows of tools/flows carry `# expect:`, `# seconds:`
// and `# fixture:` headers for run_game.py, which this parser ignores (as does the rest of any line after a '#').
//
//   step    = <seconds>:<button>[+<button>...][/<hold_ms>]
//   repeat  = <button>[+<button>...]@<period_s>[/<hold_ms>]
//   button  = a b x y start back lb rb lt rt l3 r3 up down left right
//             ls_up ls_down ls_left ls_right   (left stick, full deflection)
//             rs_up rs_down rs_left rs_right   (right stick)
//             rt=<0..1> lt=<0..1>              (analog trigger)
//
// <seconds> counts from the start of the process. Default hold: 200 ms (a menu tap); a throttle you want to keep needs
// a long hold, e.g. `30:rt=1/20000`.
//
// SCHEDULE STEPS. The same script (file and --input) can also carry a timed schedule, for A/B experiments inside one
// run. A schedule step shares the <seconds>: prefix; the first word after the colon (case-insensitive) tells it from a
// pad step (no button is called set, burn, burnmem or mark). No /hold and no '+' on these steps.
//
//   <seconds>:set <cvar>=<value>   the host sets a run-time cvar at that time (rex::cvar::SetFlagByName). cvar is
//                                  [A-Za-z0-9_]+; value is non-empty without whitespace, ';' or '#' and is passed on
//                                  verbatim; one pair per step (steps of the same second apply in file order). A cvar
//                                  that does not exist fails the run at once (exit 2).
//   <seconds>:burn <n>             marker: tools/run_game.py starts/stops n CPU (ALU) spinner processes (0..32; 0 stops
//                                  that kind). The host only logs it.
//   <seconds>:burnmem <n>          marker: the same for n memory-bandwidth spinners.
//   <seconds>:mark <label>         marker: a named segment boundary; label is [A-Za-z0-9_.-]{1,40}.
//
// The host logs each one as `[mcla-sched] t=<s> set <name>=<value> ok now=<read back>` (or `FAILED (rejected)`),
// `... burn <n>`, `... burnmem <n>`, `... mark <label>`, then ONE `[mcla-sched] t=<s> segment <k>: <label>` line for
// all the steps that fell due in the same 10 ms tick. The label is the canonical state: every key the schedule touches,
// sorted by name, as key=value joined by spaces (each set cvar with its current value, burn, burnmem and mark when the
// schedule uses them), e.g. `segment 3: burn=4 mcla_gfx_preset=perf`; `segment 0` is the state at the start of the run.
// Those lines are what tools/segments.py cuts its windows at; tools/run_game.py validates the same grammar and runs
// the burn/burnmem spinners.
//
// The flags are read from the raw command line (a plain `--input-file X` with a space would become a positional
// argument of the SDK's cvar parser, and an unknown `--x=y` is only stashed there, not usable as a cvar).
//
// HOW THE PAD REACHES THE GUEST. The iOS harness attached an SDL virtual joystick to the SDK's own SDL input driver.
// That cannot work on Windows: SDL3 is a static library inside rexruntime.dll and its symbols are not exported, so an
// SDL call made from mcla.exe talks to a second copy of SDL with no gamepad subsystem. Instead this host adds a
// synthetic gamepad driver (rex::input::InputDriver, the same mechanism as the SDK's NOP driver) to the SDK's input
// system. It reports a connected pad for guest user 0, answers XInputGetState and XInputGetKeystroke from the script
// and the clock, and needs no window focus. The SDK merges it with any physical pad.
//
// The parser below is standard C++ only.
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace rex::input {
class InputSystem;
}

namespace mcla_host {

// Bits are SDL gamepad button indices, as in the iOS port (the flows were written against that layout). The driver
// maps them to XInput buttons.
enum Button : uint32_t {
  kA = 1u << 0,
  kB = 1u << 1,
  kX = 1u << 2,
  kY = 1u << 3,
  kBack = 1u << 4,
  kStart = 1u << 6,
  kL3 = 1u << 7,
  kR3 = 1u << 8,
  kLB = 1u << 9,
  kRB = 1u << 10,
  kUp = 1u << 11,
  kDown = 1u << 12,
  kLeft = 1u << 13,
  kRight = 1u << 14,
};

// What a step holds down. Stick axes use SDL's convention: up is negative y.
struct PadState {
  uint32_t buttons = 0;
  float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;

  void Merge(const PadState& o) {
    buttons |= o.buttons;
    auto big = [](float a, float b) { return std::abs(b) > std::abs(a) ? b : a; };
    lx = big(lx, o.lx);
    ly = big(ly, o.ly);
    rx = big(rx, o.rx);
    ry = big(ry, o.ry);
    lt = std::max(lt, o.lt);
    rt = std::max(rt, o.rt);
  }
  bool operator==(const PadState& o) const {
    return buttons == o.buttons && lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry && lt == o.lt &&
           rt == o.rt;
  }
  bool operator!=(const PadState& o) const { return !(*this == o); }
};

struct Step {
  double at_s = 0;      // first press, seconds since process start
  double period_s = 0;  // >0: repeat every period_s (from at_s) for the whole run
  int hold_ms = 200;
  PadState state;
  std::string text;  // as written, for the log
};

// A schedule step (set / burn / burnmem / mark). Kept apart from the pad steps: StateAt and PressesBy never see these.
struct SchedStep {
  enum Kind { kSet, kBurn, kBurnMem, kMark };
  double at_s = 0;    // seconds since process start
  Kind kind = kSet;
  std::string name;   // kSet: the cvar; kMark: the label
  std::string value;  // kSet: the value, verbatim
  int n = 0;          // kBurn / kBurnMem: how many spinners, 0..32
  std::string text;   // as written, for the log
};

inline std::string Trim(const std::string& s) {
  const auto a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

inline std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (true) {
    const size_t e = s.find(sep, pos);
    out.push_back(s.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
    if (e == std::string::npos) break;
    pos = e + 1;
  }
  return out;
}

// One token ("a", "ls_up", "rt=0.6") added to `st`.
inline bool ParseButton(const std::string& tok_in, PadState& st, std::string& err) {
  std::string tok = tok_in;
  std::transform(tok.begin(), tok.end(), tok.begin(), [](unsigned char c) { return std::tolower(c); });
  float analog = 1.0f;
  const size_t eq = tok.find('=');
  if (eq != std::string::npos) {
    char* end = nullptr;
    const std::string v = tok.substr(eq + 1);
    analog = std::strtof(v.c_str(), &end);
    if (v.empty() || *end != 0 || analog < 0.0f || analog > 1.0f) {
      err = "bad analog value in '" + tok_in + "' (0..1)";
      return false;
    }
    tok = tok.substr(0, eq);
    if (tok != "rt" && tok != "lt") {
      err = "only rt and lt take =<0..1>: '" + tok_in + "'";
      return false;
    }
  }
  struct Named {
    const char* name;
    uint32_t bit;
  };
  static const Named buttons[] = {{"a", kA},   {"b", kB},       {"x", kX},       {"y", kY},     {"start", kStart},
                                  {"back", kBack}, {"lb", kLB}, {"rb", kRB},     {"l3", kL3},   {"r3", kR3},
                                  {"up", kUp}, {"down", kDown}, {"left", kLeft}, {"right", kRight}};
  for (const Named& n : buttons) {
    if (tok == n.name) {
      st.buttons |= n.bit;
      return true;
    }
  }
  if (tok == "lt") {
    st.lt = std::max(st.lt, analog);
    return true;
  }
  if (tok == "rt") {
    st.rt = std::max(st.rt, analog);
    return true;
  }
  if (tok == "ls_up") { st.ly = -1; return true; }
  if (tok == "ls_down") { st.ly = 1; return true; }
  if (tok == "ls_left") { st.lx = -1; return true; }
  if (tok == "ls_right") { st.lx = 1; return true; }
  if (tok == "rs_up") { st.ry = -1; return true; }
  if (tok == "rs_down") { st.ry = 1; return true; }
  if (tok == "rs_left") { st.rx = -1; return true; }
  if (tok == "rs_right") { st.rx = 1; return true; }
  err = "unknown button '" + tok_in + "'";
  return false;
}

// "a+b/300" -> state + hold. `hold` stays default when no "/ms".
inline bool ParseButtons(const std::string& text_in, Step& step, std::string& err) {
  std::string text = text_in;
  const size_t slash = text.find('/');
  if (slash != std::string::npos) {
    const std::string h = Trim(text.substr(slash + 1));
    char* end = nullptr;
    const long ms = std::strtol(h.c_str(), &end, 10);
    if (h.empty() || *end != 0 || ms < 20 || ms > 600000) {
      err = "bad hold '" + h + "' (20..600000 ms)";
      return false;
    }
    step.hold_ms = static_cast<int>(ms);
    text = text.substr(0, slash);
  }
  for (const std::string& t : Split(text, '+')) {
    if (!ParseButton(Trim(t), step.state, err)) return false;
  }
  return true;
}

inline std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

inline bool HasSpace(const std::string& s) {
  return std::any_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

// Non-empty, ASCII letters and digits plus the characters of `extra`.
inline bool IsWord(const std::string& s, std::string_view extra) {
  if (s.empty()) return false;
  for (const unsigned char c : s) {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!alnum && extra.find(static_cast<char>(c)) == std::string_view::npos) return false;
  }
  return true;
}

// The cvars of a schedule are keys of the segment label next to these three, so a cvar may not share their names.
inline bool IsReservedSchedKey(const std::string& name) {
  return name == "burn" || name == "burnmem" || name == "mark";
}

// What follows the keyword `kw` (lower case; "set", "burn", "burnmem" or "mark") of a schedule step, `rest` trimmed.
inline bool ParseSchedBody(const std::string& kw, const std::string& rest, SchedStep& step, std::string& err) {
  if (kw == "set") {
    step.kind = SchedStep::kSet;
    const size_t eq = rest.find('=');
    if (eq == std::string::npos) {
      err = "set needs <cvar>=<value>";
      return false;
    }
    if (HasSpace(rest)) {
      err = "set takes exactly one <cvar>=<value>, without spaces ('" + rest + "')";
      return false;
    }
    step.name = rest.substr(0, eq);
    step.value = rest.substr(eq + 1);
    if (!IsWord(step.name, "_")) {
      err = "bad cvar name '" + step.name + "' (letters, digits, _)";
      return false;
    }
    if (IsReservedSchedKey(step.name)) {
      err = "'" + step.name + "' cannot be set from a schedule (it is a segment label key)";
      return false;
    }
    if (step.value.empty()) {
      err = "empty value for cvar '" + step.name + "'";
      return false;
    }
    if (step.value.find_first_of(";#") != std::string::npos) {
      err = "the value '" + step.value + "' of cvar '" + step.name + "' has a ';' or '#'";
      return false;
    }
    return true;
  }
  if (kw == "burn" || kw == "burnmem") {
    step.kind = kw == "burn" ? SchedStep::kBurn : SchedStep::kBurnMem;
    // Digits only (no sign, no space), at most two: 0..32 below.
    const bool digits =
        !rest.empty() && rest.size() <= 2 &&
        std::all_of(rest.begin(), rest.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
    step.n = digits ? std::atoi(rest.c_str()) : -1;
    if (step.n < 0 || step.n > 32) {
      err = kw + " needs a count 0..32, got '" + rest + "'";
      return false;
    }
    return true;
  }
  // mark
  step.kind = SchedStep::kMark;
  if (rest.size() > 40 || !IsWord(rest, "_.-")) {
    err = "bad mark label '" + rest + "' (1..40 of A-Z a-z 0-9 _ . -)";
    return false;
  }
  step.name = rest;
  return true;
}

// "20:start;24:a;30:rt=1/20000" -> steps (appended). False + err on a bad step.
// Schedule steps ("10:set x=1", "20:burn 4", "30:burnmem 2", "40:mark foo") go to `sched` instead (appended, then the
// whole vector is stable-sorted by time: file order stays for equal times); with `sched` null they are an error.
inline bool ParseScript(const std::string& script, std::vector<Step>& out, std::string& err,
                        std::vector<SchedStep>* sched = nullptr) {
  for (const std::string& raw : Split(script, ';')) {
    const std::string s = Trim(raw);
    if (s.empty()) continue;
    const size_t colon = s.find(':');
    if (colon == std::string::npos) {
      err = "step '" + s + "' has no <seconds>: prefix";
      return false;
    }
    Step step;
    step.text = s;
    char* end = nullptr;
    const std::string t = Trim(s.substr(0, colon));
    step.at_s = std::strtod(t.c_str(), &end);
    if (t.empty() || *end != 0 || step.at_s < 0) {
      err = "bad time '" + t + "' in step '" + s + "'";
      return false;
    }
    const std::string body = Trim(s.substr(colon + 1));
    size_t word_end = 0;
    while (word_end < body.size() && !std::isspace(static_cast<unsigned char>(body[word_end]))) ++word_end;
    const std::string kw = ToLower(body.substr(0, word_end));
    if (kw == "set" || kw == "burn" || kw == "burnmem" || kw == "mark") {
      if (!sched) {
        err = "schedule step '" + s + "' is not allowed here";
        return false;
      }
      SchedStep ss;
      ss.at_s = step.at_s;
      ss.text = s;
      if (!ParseSchedBody(kw, Trim(body.substr(word_end)), ss, err)) {
        err += " (step '" + s + "')";
        return false;
      }
      sched->push_back(std::move(ss));
      continue;
    }
    if (!ParseButtons(body, step, err)) {
      err += " (step '" + s + "')";
      return false;
    }
    out.push_back(step);
  }
  if (sched) {
    std::stable_sort(sched->begin(), sched->end(),
                     [](const SchedStep& a, const SchedStep& b) { return a.at_s < b.at_s; });
  }
  return true;
}

// "a@3,start@10/300" -> repeating steps (first press after one period).
inline bool ParseRepeat(const std::string& spec, std::vector<Step>& out, std::string& err) {
  for (const std::string& raw : Split(spec, ',')) {
    const std::string s = Trim(raw);
    if (s.empty()) continue;
    const size_t at = s.find('@');
    if (at == std::string::npos) {
      err = "repeat '" + s + "' needs <button>@<period_s>";
      return false;
    }
    Step step;
    step.text = s;
    std::string period = Trim(s.substr(at + 1));
    const size_t slash = period.find('/');
    std::string hold_part;
    if (slash != std::string::npos) {
      hold_part = period.substr(slash);  // "/ms", parsed with the buttons
      period = Trim(period.substr(0, slash));
    }
    char* end = nullptr;
    step.period_s = std::strtod(period.c_str(), &end);
    if (period.empty() || *end != 0 || step.period_s < 0.3) {
      err = "bad period '" + period + "' in '" + s + "' (>= 0.3 s)";
      return false;
    }
    step.at_s = step.period_s;
    if (!ParseButtons(Trim(s.substr(0, at)) + hold_part, step, err)) {
      err += " (repeat '" + s + "')";
      return false;
    }
    if (step.hold_ms >= step.period_s * 1000) {
      err = "hold is not shorter than the period in '" + s + "'";
      return false;
    }
    out.push_back(step);
  }
  return true;
}

// --input-file: one step (or several, ';'-separated) per line, '#' comments.
inline std::string ScriptFromFileText(const std::string& text) {
  std::string out;
  for (const std::string& line : Split(text, '\n')) {
    const std::string l = Trim(line.substr(0, line.find('#')));
    if (!l.empty()) out += l + ";";
  }
  return out;
}

// The state the whole script asks for at `t` seconds since the process start.
inline PadState StateAt(const std::vector<Step>& steps, double t) {
  PadState st;
  for (const Step& s : steps) {
    const double hold = s.hold_ms / 1000.0;
    bool on = false;
    if (s.period_s > 0) {
      if (t >= s.at_s) {
        const double phase = t - s.at_s - s.period_s * static_cast<long>((t - s.at_s) / s.period_s);
        on = phase < hold;
      }
    } else {
      on = t >= s.at_s && t < s.at_s + hold;
    }
    if (on) st.Merge(s.state);
  }
  return st;
}

// How many presses of `s` have started by `t` (for logging each press once).
inline long PressesBy(const Step& s, double t) {
  if (t < s.at_s) return 0;
  return s.period_s > 0 ? 1 + static_cast<long>((t - s.at_s) / s.period_s) : 1;
}

// The state a schedule has reached, as the label of a segment: every key the schedule touches, sorted by name (plain
// byte order), as key=value joined by single spaces. Keys: each cvar a set step names, `burn` and `burnmem` when the
// schedule has such steps (the counts start at 0) and `mark` when it has mark steps (starts as "start"). The cvars'
// start values come from the cvar registry, which this header does not know: the caller fills them in with SetCvar().
class SchedState {
 public:
  SchedState() = default;
  explicit SchedState(const std::vector<SchedStep>& steps) {
    for (const SchedStep& s : steps) {
      switch (s.kind) {
        case SchedStep::kSet:
          if (kv_.emplace(s.name, std::string()).second) cvars_.push_back(s.name);
          break;
        case SchedStep::kBurn: kv_["burn"] = "0"; break;
        case SchedStep::kBurnMem: kv_["burnmem"] = "0"; break;
        case SchedStep::kMark: kv_["mark"] = "start"; break;
      }
    }
    std::sort(cvars_.begin(), cvars_.end());
  }

  const std::vector<std::string>& cvars() const { return cvars_; }  // the distinct set names, sorted

  void SetCvar(const std::string& name, const std::string& value) { kv_[name] = value; }
  void SetBurn(int n) { kv_["burn"] = std::to_string(n); }
  void SetBurnMem(int n) { kv_["burnmem"] = std::to_string(n); }
  void SetMark(const std::string& label) { kv_["mark"] = label; }

  std::string Label() const {
    std::string out;
    for (const auto& [key, value] : kv_) {
      if (!out.empty()) out += ' ';
      out += key + "=" + value;
    }
    return out;
  }

 private:
  std::map<std::string, std::string> kv_;
  std::vector<std::string> cvars_;
};

// --- Process clock and command line --------------------------------------------------------------------------

// Seconds since the process started (a static captured before main runs). The clock the script's steps count from.
double SecondsSinceProcessStart();

// What the script flags of the raw command line say. Empty strings = flag absent.
struct InputOptions {
  std::wstring file;    // --input-file=<path>
  std::string script;   // --input=<script> (several flags are joined with ';')
  std::string repeat;   // --input-repeat=<spec> (several flags are joined with ',')

  bool any() const { return !file.empty() || !script.empty() || !repeat.empty(); }
};

// Parses GetCommandLineW(). Only the `--flag=value` form is recognised.
InputOptions InputOptionsFromCommandLine();

// --- The scripted pad ----------------------------------------------------------------------------------------

class ScriptedInput {
 public:
  ScriptedInput() = default;
  ~ScriptedInput();
  ScriptedInput(const ScriptedInput&) = delete;
  ScriptedInput& operator=(const ScriptedInput&) = delete;

  // Reads the file (if any) and parses everything. False + a message in `error` on an unreadable file or a bad step;
  // nothing is started in that case. No flags at all is fine: the object stays inactive.
  bool Configure(const InputOptions& options, std::string& error);

  // True when the script has at least one step, pad or schedule (a schedule-only script is valid).
  bool active() const { return pad_active() || !sched_.empty(); }
  size_t step_count() const { return steps_ ? steps_->size() : 0; }  // pad steps
  size_t sched_count() const { return sched_.size(); }               // schedule steps

  // Adds the synthetic gamepad driver to `input`. Call from the input factory, before the guest starts polling. Does
  // nothing when the script has no pad steps.
  void AttachTo(rex::input::InputSystem& input);

  // Starts / stops the script thread: it logs one line per press and one when a non-repeating pad script has ended, and
  // runs the schedule (cvar changes at their time, markers, the segment lines). The pad itself needs no thread: it
  // reads the script and the clock when the guest polls. Start also checks the schedule's cvars (cvars and plugins
  // exist by OnPostSetup): an unknown name is logged and ends the process with exit code 2.
  void Start();
  void Stop();

 private:
  bool pad_active() const { return steps_ && !steps_->empty(); }
  void LogMain();
  void StartSchedule();
  void RunSchedule(double t);

  std::shared_ptr<std::vector<Step>> steps_;
  std::string source_;  // for the log: where the steps came from

  // The schedule, sorted by time. Written by Configure and Start, then only by the script thread.
  std::vector<SchedStep> sched_;
  SchedState sched_state_;
  size_t sched_next_ = 0;  // the first step that has not fired yet
  int sched_segment_ = 0;

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
};

}  // namespace mcla_host
