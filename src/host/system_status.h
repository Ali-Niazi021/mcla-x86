// mcla-x86 host: what the machine is doing, logged at start-up (H4).
//
// A run's frame rate depends on things outside the game: a laptop on battery or in Windows' "Best power efficiency"
// mode will not hold 60 fps at 1080p, a 50 Hz panel cannot show 60 fps evenly, and a MyASUS "Whisper" fan profile caps
// the CPU. This reads those settings (read-only, plain Win32, no admin rights, no vendor SDK) and logs them in at most
// five `[mcla-sys] ...` lines, so a log or a recording says under which conditions it was made.
//
//   QuerySystemStatus(hwnd)   fills SystemStatus: power source and battery saver, Windows power mode (overlay) and
//                             plan, processor boost mode and maximum state of the plan, the ASUS fan values, the
//                             refresh rate and DPI of the window's monitor, the logical processors per efficiency class
//   LogSystemStatus(...)      the `[mcla-sys]` lines; the last one is a WARNING when `target_fps` >= 60 at a guest
//                             height >= 1000 is unlikely and says why
//   SystemStatusWarning(...)  the same warning text on its own (empty = nothing to warn about)
//   SystemStatusOneLine(...)  one compact line for a launcher status line
//
// Nothing here changes a setting. powrprof.dll and dwmapi are loaded at run time (no link dependency); the registry is
// read with RegOpenKeyEx and KEY_READ only. The only SDK dependency is in LogSystemStatus (rex/logging.h, and the
// `fullscreen` cvar through rex::cvar::Query).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mcla_host {

enum class PowerSource { kUnknown, kAc, kBattery };

struct SystemStatus {
  // --- GetSystemPowerStatus
  PowerSource power_source = PowerSource::kUnknown;
  bool has_battery = false;
  int battery_percent = -1;  // -1 = unknown or no battery
  bool battery_saver = false;

  // --- Windows power mode (the "overlay": PowerGetEffectiveOverlayScheme) and the active power plan
  bool overlay_known = false;
  std::string overlay_guid;
  std::string overlay_name;  // "Best power efficiency", "Balanced", "Better performance", "Best performance", or the GUID
  std::string scheme_guid;
  std::string scheme_name;  // the plan's friendly name ("Balanced"); empty when it could not be read
  // The plan's own settings (not the overlay's effect on them). PERFBOOSTMODE: 0 Disabled, 1 Enabled, 2 Aggressive,
  // 3 Efficient Enabled, 4 Efficient Aggressive, 5 Aggressive At Guaranteed, 6 Efficient Aggressive At Guaranteed.
  // PROCTHROTTLEMAX: maximum processor state in percent. Empty = the setting could not be read.
  std::optional<uint32_t> boost_mode_ac;
  std::optional<uint32_t> boost_mode_dc;
  std::optional<uint32_t> max_state_ac;
  std::optional<uint32_t> max_state_dc;

  // --- ASUS (MyASUS / ASUS System Control Interface), raw registry values; the mapping to the Standard / Whisper /
  // Performance / Full-speed profiles is NOT verified, so they are logged raw. QuietFan and QuietFanPowerMode live
  // under ...\AsusOptimization\ASUS Keyboard Hotkeys, FanMode under ...\AsusOptimization\ScreenXpert (on the machine
  // this was written on); FanMode is also looked for under the first of those.
  bool asus_present = false;  // HKLM\SOFTWARE\ASUS\ASUS System Control Interface exists
  std::optional<uint32_t> asus_quiet_fan;
  std::optional<uint32_t> asus_quiet_fan_power_mode;
  std::optional<uint32_t> asus_fan_mode;

  // --- The monitor the window is on (the primary one without a window)
  bool display_known = false;
  std::string display_device;  // "\\.\DISPLAY1"
  int display_width = 0;       // current mode in physical pixels
  int display_height = 0;
  int display_hz = 0;       // dmDisplayFrequency; 0 = unknown (the driver says 0 or 1 for "hardware default")
  uint32_t display_dpi = 0;  // of the window's monitor, 0 = unknown
  bool display_primary = false;

  // --- Logical processors per efficiency class (GetSystemCpuSetInformation), highest class first. On a Ryzen AI
  // 9 HX 370 laptop: 8 at class 1 (Zen 5), 12 at class 0 (Zen 5c).
  int cpu_logical = 0;
  std::vector<std::pair<int, int>> cpu_classes;  // {efficiency class, logical processors}
};

// `native_window` is the window's HWND (Window::GetNativeWindowHandle()), or null for the primary monitor. Safe on any
// thread; never throws, never prompts, never changes a setting.
SystemStatus QuerySystemStatus(void* native_window = nullptr);

// Logs the `[mcla-sys]` lines. `target_fps` is the frame rate the run aims at (0 = unknown), `guest_height` the guest
// video mode's height (GuestVideoModeSize in window_fit.h; 0 = unknown). The warning line is only written when
// target_fps >= 60 and guest_height >= 1000 and a reason applies.
void LogSystemStatus(const SystemStatus& status, int target_fps, int guest_height);

// `60 fps at 1080p is unlikely: <reasons>`: on battery; battery saver on; Windows power mode "Best power efficiency";
// the display's refresh rate is not a multiple of target_fps. Empty when target_fps < 60, guest_height < 1000 or no
// reason applies. (The plan's boost mode and maximum state are logged but are not reasons.)
std::string SystemStatusWarning(const SystemStatus& status, int target_fps, int guest_height);

// True when `refresh` is, within 1 Hz (59.94 reports as 59 or 60), a whole multiple of `target` (also when either is
// unknown, <= 0). 60 on 120 or 60 is; 60 on 165, 144, 100 or 75 is not: frames then last 2 or 3 refreshes in a beat.
bool RefreshIsMultipleOf(int refresh, int target);

// One compact line, e.g. `AC power | mode Best performance | 120 Hz 2880x1800 | cpu 20 logical`.
std::string SystemStatusOneLine(const SystemStatus& status);

// One `[mcla-sys] sample:` line (the watchdog writes one every 30 s): power source, the clock of each CPU efficiency class
// against its maximum and its current limit (a limit under the maximum = CAPPED: thermal, power or a quiet mode), the
// process' CPU use in cores, page faults and reads a second, its memory, and the system's memory load. One caller.
void LogSystemSample();

}  // namespace mcla_host
