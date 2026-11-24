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
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
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

// "20:start;24:a;30:rt=1/20000" -> steps (appended). False + err on a bad step.
inline bool ParseScript(const std::string& script, std::vector<Step>& out, std::string& err) {
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
    if (!ParseButtons(Trim(s.substr(colon + 1)), step, err)) {
      err += " (step '" + s + "')";
      return false;
    }
    out.push_back(step);
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

  // True when the script has at least one step.
  bool active() const { return steps_ && !steps_->empty(); }
  size_t step_count() const { return steps_ ? steps_->size() : 0; }

  // Adds the synthetic gamepad driver to `input`. Call from the input factory, before the guest starts polling. Does
  // nothing when inactive.
  void AttachTo(rex::input::InputSystem& input);

  // Starts / stops the logging thread (one line per press, one when a non-repeating script has ended). The pad itself
  // needs no thread: it reads the script and the clock when the guest polls.
  void Start();
  void Stop();

 private:
  void LogMain();

  std::shared_ptr<std::vector<Step>> steps_;
  std::string source_;  // for the log: where the steps came from

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
};

}  // namespace mcla_host
