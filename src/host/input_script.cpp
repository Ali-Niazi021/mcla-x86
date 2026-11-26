// mcla-x86 host: scripted controller input. See input_script.h.
#include "input_script.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>

#include <rex/cvar.h>
#include <rex/input/device.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

namespace mcla_host {

namespace {

// The SDK's X_ERROR_* / X_STATUS_* macros expand to unqualified X_RESULT / X_STATUS.
using rex::X_RESULT;
using rex::X_STATUS;

using Clock = std::chrono::steady_clock;

// Captured while the executable's statics are initialised, i.e. before main/wWinMain: the process start, as far as the
// flows are concerned.
const Clock::time_point g_process_start = Clock::now();

std::string Utf8FromWide(std::wstring_view wide) {
  if (wide.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                       nullptr, nullptr);
  std::string utf8(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size, nullptr,
                      nullptr);
  return utf8;
}

// --- The synthetic gamepad -----------------------------------------------------------------------------------

constexpr auto kScriptPadId = static_cast<rex::input::DeviceId>(0x4D435041);  // "MCPA"

// Thresholds and repeat timing of the SDK's SDL driver (sdl_input_driver.h), so a scripted pad behaves like a real one.
constexpr int kThumbThreshold = 0x4E00;
constexpr uint8_t kTriggerThreshold = 0x1F;
constexpr uint32_t kRepeatDelayMs = 400;
constexpr uint32_t kRepeatRateMs = 100;

int16_t AxisValue(float v) {
  return static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
}

uint8_t TriggerValue(float v) {
  return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

rex::input::X_INPUT_GAMEPAD ToGamepad(const PadState& state) {
  using namespace rex::input;
  struct Map {
    uint32_t script_bit;
    uint16_t xinput_bit;
  };
  static constexpr Map kMap[] = {
      {kA, X_INPUT_GAMEPAD_A},
      {kB, X_INPUT_GAMEPAD_B},
      {kX, X_INPUT_GAMEPAD_X},
      {kY, X_INPUT_GAMEPAD_Y},
      {kBack, X_INPUT_GAMEPAD_BACK},
      {kStart, X_INPUT_GAMEPAD_START},
      {kL3, X_INPUT_GAMEPAD_LEFT_THUMB},
      {kR3, X_INPUT_GAMEPAD_RIGHT_THUMB},
      {kLB, X_INPUT_GAMEPAD_LEFT_SHOULDER},
      {kRB, X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {kUp, X_INPUT_GAMEPAD_DPAD_UP},
      {kDown, X_INPUT_GAMEPAD_DPAD_DOWN},
      {kLeft, X_INPUT_GAMEPAD_DPAD_LEFT},
      {kRight, X_INPUT_GAMEPAD_DPAD_RIGHT},
  };
  uint16_t buttons = 0;
  for (const Map& m : kMap) {
    if (state.buttons & m.script_bit) {
      buttons |= m.xinput_bit;
    }
  }
  X_INPUT_GAMEPAD pad;
  std::memset(&pad, 0, sizeof(pad));
  pad.buttons = buttons;
  pad.left_trigger = TriggerValue(state.lt);
  pad.right_trigger = TriggerValue(state.rt);
  pad.thumb_lx = AxisValue(state.lx);
  pad.thumb_rx = AxisValue(state.rx);
  // The script says up = negative y (SDL); XInput says up = positive y.
  pad.thumb_ly = static_cast<int16_t>(-AxisValue(state.ly));
  pad.thumb_ry = static_cast<int16_t>(-AxisValue(state.ry));
  return pad;
}

// Triggers past their threshold and sticks past theirs count as buttons for the keystroke stream (bits 16..33, the
// layout of the SDK's SDL driver). A stick held diagonally reports the diagonal, not two directions.
uint64_t AnalogKeyfield(const rex::input::X_INPUT_GAMEPAD& pad) {
  uint64_t f = 0;
  f |= static_cast<uint64_t>(pad.left_trigger > kTriggerThreshold) << 16;
  f |= static_cast<uint64_t>(pad.right_trigger > kTriggerThreshold) << 17;
  int x = static_cast<int16_t>(pad.thumb_lx);
  int y = static_cast<int16_t>(pad.thumb_ly);
  for (int shift = 0; shift <= 8; shift += 8) {
    uint64_t u = y > kThumbThreshold;
    uint64_t d = y < -kThumbThreshold - 1;
    uint64_t r = x > kThumbThreshold;
    uint64_t l = x < -kThumbThreshold - 1;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + shift);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + shift);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + shift);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + shift);
    }
    f |= u << (18 + shift);
    f |= d << (19 + shift);
    f |= r << (20 + shift);
    f |= l << (21 + shift);
    x = static_cast<int16_t>(pad.thumb_rx);
    y = static_cast<int16_t>(pad.thumb_ry);
  }
  return f;
}

uint32_t NowMs() {
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_process_start).count());
}

// A gamepad that exists only in the SDK's input system. Stateless apart from the keystroke edge detector: what it
// reports is the script evaluated at the time of the poll.
class ScriptPadDriver final : public rex::input::InputDriver {
 public:
  explicit ScriptPadDriver(std::shared_ptr<const std::vector<Step>> steps)
      : InputDriver(nullptr, 2), steps_(std::move(steps)) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kScriptPadId;
    info.name = "MCLA script pad";
    info.synthetic = true;  // feeds guest user 0 and never consumes a physical pad's ordinal
    out.push_back(info);
  }

  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out_state) override {
    if (id != kScriptPadId) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!out_state) {
      return X_ERROR_BAD_ARGUMENTS;
    }
    std::memset(out_state, 0, sizeof(*out_state));
    // The merge keeps the highest packet number of all devices, so this one starts above a real pad's and changes with
    // every poll: the game never skips a read of it as "unchanged".
    out_state->packet_number = 0x40000000u + polls_.fetch_add(1, std::memory_order_relaxed);
    // Like the SDK's pads: an inactive input (a dialog overlay has the focus) reads as untouched.
    if (is_active()) {
      out_state->gamepad = ToGamepad(CurrentState());
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t /*flags*/,
                                 rex::input::X_INPUT_CAPABILITIES* out_caps) override {
    if (id != kScriptPadId) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!out_caps) {
      return X_ERROR_BAD_ARGUMENTS;
    }
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xF3FF;  // every button but the guide button
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
    out_caps->vibration.left_motor_speed = 0xFFFFu;
    out_caps->vibration.right_motor_speed = 0xFFFFu;
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION* /*vibration*/) override {
    return id == kScriptPadId ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // The keystroke stream (the title's "Press START" and the menus read XInputGetKeystroke), made from the button edges
  // the way the SDK's SDL driver makes it: key-ups first, one event per call, auto-repeat after 400 ms at 10 Hz.
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t /*flags*/,
                              rex::input::X_INPUT_KEYSTROKE* out) override {
    using rex::ui::VirtualKey;
    if (id != kScriptPadId) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!out) {
      return X_ERROR_BAD_ARGUMENTS;
    }
    // Same order as the SDK's SDL driver (also the order events go out when several buttons change together).
    static constexpr VirtualKey kVk[34] = {
        VirtualKey::kXInputPadDpadUp,       VirtualKey::kXInputPadDpadDown,
        VirtualKey::kXInputPadDpadLeft,     VirtualKey::kXInputPadDpadRight,
        VirtualKey::kXInputPadStart,        VirtualKey::kXInputPadBack,
        VirtualKey::kXInputPadLThumbPress,  VirtualKey::kXInputPadRThumbPress,
        VirtualKey::kXInputPadLShoulder,    VirtualKey::kXInputPadRShoulder,
        VirtualKey::kNone /* guide */,      VirtualKey::kNone /* unknown */,
        VirtualKey::kXInputPadA,            VirtualKey::kXInputPadB,
        VirtualKey::kXInputPadX,            VirtualKey::kXInputPadY,
        VirtualKey::kXInputPadLTrigger,     VirtualKey::kXInputPadRTrigger,
        VirtualKey::kXInputPadLThumbUp,     VirtualKey::kXInputPadLThumbDown,
        VirtualKey::kXInputPadLThumbRight,  VirtualKey::kXInputPadLThumbLeft,
        VirtualKey::kXInputPadLThumbUpLeft, VirtualKey::kXInputPadLThumbUpRight,
        VirtualKey::kXInputPadLThumbDownRight, VirtualKey::kXInputPadLThumbDownLeft,
        VirtualKey::kXInputPadRThumbUp,     VirtualKey::kXInputPadRThumbDown,
        VirtualKey::kXInputPadRThumbRight,  VirtualKey::kXInputPadRThumbLeft,
        VirtualKey::kXInputPadRThumbUpLeft, VirtualKey::kXInputPadRThumbUpRight,
        VirtualKey::kXInputPadRThumbDownRight, VirtualKey::kXInputPadRThumbDownLeft,
    };
    auto fill = [out](VirtualKey vk, uint16_t flags) {
      out->virtual_key = static_cast<uint16_t>(vk);
      out->unicode = 0;
      out->flags = flags;
      out->user_index = 0;  // InputSystem stamps the guest user
      out->hid_code = 0;
    };

    // An inactive input (a dialog overlay) forces everything to "released"; the algorithm then sends the key-ups, and
    // the key-downs again when the input becomes active.
    uint64_t current = 0;
    if (is_active()) {
      const rex::input::X_INPUT_GAMEPAD pad = ToGamepad(CurrentState());
      current = static_cast<uint64_t>(static_cast<uint16_t>(pad.buttons)) | AnalogKeyfield(pad);
    }

    std::lock_guard lock(keys_mutex_);
    const uint32_t now = NowMs();
    if (repeat_ == Repeat::kWaiting && now - repeat_time_ms_ > kRepeatDelayMs) {
      repeat_ = Repeat::kRepeating;
    }
    if (repeat_ == Repeat::kRepeating && now - repeat_time_ms_ > kRepeatRateMs) {
      repeat_time_ms_ = now;
      fill(kVk[repeat_index_],
           rex::input::X_INPUT_KEYSTROKE_KEYDOWN | rex::input::X_INPUT_KEYSTROKE_REPEAT);
      return X_ERROR_SUCCESS;
    }

    const uint64_t changed = current ^ keys_;
    if (!changed) {
      return X_ERROR_EMPTY;
    }
    // Up events first, so a stick moving from up-left to left releases UPLEFT before LEFT goes down (as XInput does).
    for (int pass = 0; pass < 2; ++pass) {
      const bool clear_pass = pass == 0;
      for (uint8_t i = 0; i < 34; ++i) {
        const uint64_t bit = uint64_t(1) << i;
        if (!(changed & bit) || kVk[i] == VirtualKey::kNone) {
          continue;
        }
        const bool pressed = (current & bit) != 0;
        if (clear_pass && !pressed) {
          fill(kVk[i], rex::input::X_INPUT_KEYSTROKE_KEYUP);
          keys_ &= ~bit;
          repeat_ = Repeat::kIdle;
          return X_ERROR_SUCCESS;
        }
        if (!clear_pass && pressed) {
          fill(kVk[i], rex::input::X_INPUT_KEYSTROKE_KEYDOWN);
          keys_ |= bit;
          repeat_ = Repeat::kWaiting;
          repeat_index_ = i;
          repeat_time_ms_ = now;
          return X_ERROR_SUCCESS;
        }
      }
    }
    return X_ERROR_EMPTY;
  }

 private:
  enum class Repeat { kIdle, kWaiting, kRepeating };

  PadState CurrentState() const { return StateAt(*steps_, SecondsSinceProcessStart()); }

  std::shared_ptr<const std::vector<Step>> steps_;
  std::atomic<uint32_t> polls_{0};

  std::mutex keys_mutex_;
  uint64_t keys_ = 0;  // keys already reported as down
  Repeat repeat_ = Repeat::kIdle;
  uint8_t repeat_index_ = 0;
  uint32_t repeat_time_ms_ = 0;
};

}  // namespace

double SecondsSinceProcessStart() {
  return std::chrono::duration<double>(Clock::now() - g_process_start).count();
}

InputOptions InputOptionsFromCommandLine() {
  InputOptions options;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return options;
  }
  constexpr std::wstring_view kFile = L"--input-file=";
  constexpr std::wstring_view kScript = L"--input=";
  constexpr std::wstring_view kRepeat = L"--input-repeat=";
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i];
    if (arg.starts_with(kFile)) {
      options.file = std::wstring(arg.substr(kFile.size()));
    } else if (arg.starts_with(kScript)) {
      options.script += (options.script.empty() ? "" : ";") + Utf8FromWide(arg.substr(kScript.size()));
    } else if (arg.starts_with(kRepeat)) {
      options.repeat += (options.repeat.empty() ? "" : ",") + Utf8FromWide(arg.substr(kRepeat.size()));
    }
  }
  LocalFree(argv);
  return options;
}

ScriptedInput::~ScriptedInput() {
  Stop();
}

bool ScriptedInput::Configure(const InputOptions& options, std::string& error) {
  steps_ = std::make_shared<std::vector<Step>>();
  sched_.clear();
  source_.clear();

  std::string script = options.script;
  if (!options.file.empty()) {
    const std::string file_utf8 = Utf8FromWide(options.file);
    std::ifstream in(std::filesystem::path(options.file), std::ios::binary);
    if (!in) {
      error = "--input-file: cannot read " + file_utf8;
      return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) {  // a UTF-8 byte-order mark (editors on Windows add one)
      text.erase(0, 3);
    }
    script += (script.empty() ? "" : ";") + ScriptFromFileText(text);
    source_ = file_utf8;
  }
  if (!options.script.empty()) {
    source_ += std::string(source_.empty() ? "" : " + ") + "--input";
  }
  if (!options.repeat.empty()) {
    source_ += std::string(source_.empty() ? "" : " + ") + "--input-repeat";
  }

  std::string parse_error;
  if (!ParseScript(script, *steps_, parse_error, &sched_) || !ParseRepeat(options.repeat, *steps_, parse_error)) {
    error = "input script error: " + parse_error;
    steps_->clear();
    sched_.clear();
    return false;
  }
  return true;
}

void ScriptedInput::AttachTo(rex::input::InputSystem& input) {
  if (!pad_active()) {  // a schedule-only script has no pad to add
    return;
  }
  input.AddDriver(std::make_unique<ScriptPadDriver>(steps_));
  REXLOG_INFO("[mcla-input] script pad added to the input system (synthetic gamepad, guest user 0)");
}

void ScriptedInput::Start() {
  if (!active()) {
    return;
  }
  std::lock_guard lock(mutex_);
  if (thread_.joinable()) {
    return;
  }
  stop_ = false;
  if (pad_active()) {
    REXLOG_INFO("[mcla-input] {} step(s) loaded from {}", steps_->size(), source_);
    for (const Step& s : *steps_) {
      REXLOG_INFO("[mcla-input]   {}{} at {:.2f} s, hold {} ms", s.period_s > 0 ? "repeat " : "", s.text, s.at_s,
                  s.hold_ms);
    }
  }
  StartSchedule();
  thread_ = std::thread([this] { LogMain(); });
}

// Checks the schedule against the cvar registry and logs it with its initial state. Runs from Start (OnPostSetup), when
// every cvar and plugin exists. A typo in a cvar name must fail the run at once, not leave a long unattended
// experiment running without its change.
void ScriptedInput::StartSchedule() {
  sched_next_ = 0;
  sched_segment_ = 0;
  if (sched_.empty()) {
    return;
  }
  std::vector<std::string> checked;
  for (const SchedStep& s : sched_) {
    if (s.kind != SchedStep::kSet || std::find(checked.begin(), checked.end(), s.name) != checked.end()) {
      continue;
    }
    checked.push_back(s.name);
    const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(s.name);
    if (!info) {
      REXLOG_ERROR("[mcla-sched] unknown cvar '{}' in step '{}'", s.name, s.text);
      rex::FlushLogging();
      std::_Exit(2);  // the script-error exit code
    }
    if (info->lifecycle != rex::cvar::Lifecycle::kHotReload) {
      REXLOG_WARN("[mcla-sched] cvar '{}' is {}: a change at run time may not take effect (step '{}')", s.name,
                  info->lifecycle == rex::cvar::Lifecycle::kInitOnly ? "init-only" : "restart-only", s.text);
    }
    if (info->type == rex::cvar::FlagType::String) {
      REXLOG_WARN("[mcla-sched] cvar '{}' is a string: changed live, other threads read it without a lock (racy)",
                  s.name);
    }
  }

  REXLOG_INFO("[mcla-sched] {} step(s) loaded from {}", sched_.size(), source_);
  for (const SchedStep& s : sched_) {
    REXLOG_INFO("[mcla-sched]   {} at {:.2f} s", s.text, s.at_s);
  }
  sched_state_ = SchedState(sched_);
  for (const std::string& name : sched_state_.cvars()) {
    sched_state_.SetCvar(name, rex::cvar::GetFlagByName(name));
  }
  REXLOG_INFO("[mcla-sched] t={:.3f} segment 0: {}", SecondsSinceProcessStart(), sched_state_.Label());
  rex::FlushLogging();
}

// Fires every schedule step that is due at `t`, in order and once each, then writes ONE segment line for the batch:
// tools/segments.py cuts its windows at those lines, so steps of the same instant must not split the run.
void ScriptedInput::RunSchedule(double t) {
  bool ran = false;
  while (sched_next_ < sched_.size() && sched_[sched_next_].at_s <= t) {
    const SchedStep& s = sched_[sched_next_++];
    ran = true;
    switch (s.kind) {
      case SchedStep::kSet: {
        const bool ok = rex::cvar::SetFlagByName(s.name, s.value);
        // What the cvar holds now is the state, whatever was asked for: a rejected value leaves the old one, and a bool
        // set as "1" reads back as "true", so equal states always get the same label.
        const std::string now = rex::cvar::GetFlagByName(s.name);
        sched_state_.SetCvar(s.name, now);
        if (ok) {
          REXLOG_INFO("[mcla-sched] t={:.3f} set {}={} ok now={}", t, s.name, s.value, now);
        } else {
          REXLOG_WARN("[mcla-sched] t={:.3f} set {}={} FAILED (rejected) now={}", t, s.name, s.value, now);
        }
        break;
      }
      case SchedStep::kBurn:
        sched_state_.SetBurn(s.n);
        REXLOG_INFO("[mcla-sched] t={:.3f} burn {}", t, s.n);
        break;
      case SchedStep::kBurnMem:
        sched_state_.SetBurnMem(s.n);
        REXLOG_INFO("[mcla-sched] t={:.3f} burnmem {}", t, s.n);
        break;
      case SchedStep::kBurnGpu:
        sched_state_.SetBurnGpu(s.n);
        REXLOG_INFO("[mcla-sched] t={:.3f} burngpu {}", t, s.n);
        break;
      case SchedStep::kMark:
        sched_state_.SetMark(s.name);
        REXLOG_INFO("[mcla-sched] t={:.3f} mark {}", t, s.name);
        break;
    }
  }
  if (ran) {
    REXLOG_INFO("[mcla-sched] t={:.3f} segment {}: {}", t, ++sched_segment_, sched_state_.Label());
    rex::FlushLogging();
  }
}

void ScriptedInput::Stop() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

// Writes one log line per press (the pad itself needs no thread), and one when a script without repeating steps ends;
// runs the schedule. run_game.py and the humans reading the log line the presses up against the game's own lines.
void ScriptedInput::LogMain() {
  std::vector<long> presses(steps_->size(), 0);
  bool endless = false;
  double end_s = 0;
  for (const Step& s : *steps_) {
    if (s.period_s > 0) {
      endless = true;
    } else {
      end_s = std::max(end_s, s.at_s + s.hold_ms / 1000.0);
    }
  }
  bool finished_logged = false;

  std::unique_lock lock(mutex_);
  while (!stop_) {
    wake_.wait_for(lock, std::chrono::milliseconds(10), [this] { return stop_; });
    if (stop_) {
      break;
    }
    const double t = SecondsSinceProcessStart();
    if (sched_next_ < sched_.size() && sched_[sched_next_].at_s <= t) {
      // A cvar change runs other modules' change callbacks: not under our lock (Stop needs it to end this thread).
      lock.unlock();
      RunSchedule(t);
      lock.lock();
    }
    bool logged = false;
    for (size_t i = 0; i < steps_->size(); ++i) {
      const Step& s = (*steps_)[i];
      const long n = PressesBy(s, t);
      if (n > presses[i]) {
        presses[i] = n;
        REXLOG_INFO("[mcla-input] t={:.2f} {}{} (hold {} ms)", t, s.period_s > 0 ? "repeat " : "", s.text,
                    s.hold_ms);
        logged = true;
      }
    }
    // Only the pad steps end: a schedule keeps the thread busy until Stop.
    if (pad_active() && !endless && !finished_logged && t >= end_s) {
      finished_logged = true;
      REXLOG_INFO("[mcla-input] t={:.2f} script finished", t);
      logged = true;
    }
    if (logged) {
      rex::FlushLogging();
    }
  }
}

}  // namespace mcla_host
