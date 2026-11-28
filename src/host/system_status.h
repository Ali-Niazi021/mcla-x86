// mcla-x86 host: what the machine is doing, logged at start-up (H4).
//
// A run's frame rate depends on things outside the game: a laptop on battery or in Windows' "Best power efficiency"
// mode will not hold 60 fps at 1080p, a 50 Hz panel cannot show 60 fps evenly, and a MyASUS "Whisper" fan profile caps
// the CPU. This reads those settings (read-only, plain Win32, no admin rights, no vendor SDK) and logs them in a few
// `[mcla-sys] ...` lines, so a log or a recording says under which conditions it was made. A laptop GPU's power limit is
// the largest of them: the RTX 4060 Laptop of the test machine ran at 55 W of its 100 W maximum in the vendor's default
// mode (Lenovo Intelligent Cooling "auto"), 85 fps at 2000p; in the vendor's Extreme Performance mode 100 W, 106 fps.
//
//   QuerySystemStatus(hwnd)   fills SystemStatus: power source and battery saver, Windows power mode (overlay) and
//                             plan, processor boost mode and maximum state of the plan, the ASUS fan values, the Lenovo
//                             Intelligent Cooling mode, an NVIDIA GPU's power limit / clocks / temperature / throttle
//                             reasons (NVML, loaded at run time), the refresh rate and DPI of the window's monitor, the
//                             logical processors per efficiency class
//   LogSystemStatus(...)      the `[mcla-sys]` lines; the last one is a WARNING when `target_fps` >= 60 at a guest
//                             height >= 992 (the 992p profiles, 1000p, 2000p) is unlikely and says why
//   SystemStatusWarning(...)  the same warning text on its own (empty = nothing to warn about)
//   SystemStatusOneLine(...)  one compact line for a launcher status line
//
// Nothing here changes a setting. powrprof.dll, nvml.dll and dwmapi are loaded at run time (no link dependency); the
// registry is read with RegOpenKeyEx and KEY_READ only; NVML is only asked questions (nvmlDeviceGet*). The only SDK dependency is in LogSystemStatus (rex/logging.h, and the
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
  // The power mode the Windows slider holds for the current power source (PowerGetActualOverlayScheme). It differs from the
  // effective one above when the active plan is not Balanced: the High performance plan switches the slider off (the
  // effective overlay then reads as "Balanced / none").
  bool overlay_actual_known = false;
  std::string overlay_actual_guid;
  std::string overlay_actual_name;
  bool plan_is_performance = false;  // the active plan is "High performance" or "Ultimate performance"

  // --- ASUS (MyASUS / ASUS System Control Interface), raw registry values; the mapping to the Standard / Whisper /
  // Performance / Full-speed profiles is NOT verified, so they are logged raw. QuietFan and QuietFanPowerMode live
  // under ...\AsusOptimization\ASUS Keyboard Hotkeys, FanMode under ...\AsusOptimization\ScreenXpert (on the machine
  // this was written on); FanMode is also looked for under the first of those.
  bool asus_present = false;  // HKLM\SOFTWARE\ASUS\ASUS System Control Interface exists
  std::optional<uint32_t> asus_quiet_fan;
  std::optional<uint32_t> asus_quiet_fan_power_mode;
  std::optional<uint32_t> asus_fan_mode;

  // --- Lenovo Intelligent Cooling (the ITS service, "Lenovo Notebook ITS Service"): the vendor's thermal / performance mode,
  // HKLM\SYSTEM\CurrentControlSet\Services\LITSSVC\LNBITS\IC\MMC. CurrentSetting 0 = Intelligent Cooling (auto), 1 = Battery
  // Saving (cool and quiet), 3 = Extreme Performance (see LenovoModeName; Capability is a bit mask of the modes the
  // machine has: 2 cool, 8 performance). Verified on an IdeaPad Slim Pro 9 16IRP8 (docs/LEARNINGS NATIVE-O).
  bool lenovo_present = false;
  std::optional<uint32_t> lenovo_mode;
  std::optional<uint32_t> lenovo_capability;

  // --- An NVIDIA GPU through NVML (nvml.dll from the driver, loaded at run time; absent = unknown). The first device only.
  // The enforced power limit is what the GPU is held to now: on a laptop the vendor's thermal mode sets it, and a limit well
  // under the maximum is the commonest reason for a missed frame rate.
  bool gpu_known = false;
  std::string gpu_name;
  uint32_t gpu_limit_mw = 0;    // enforced power limit; 0 = not readable
  uint32_t gpu_default_mw = 0;  // the board's default limit
  uint32_t gpu_min_mw = 0;
  uint32_t gpu_max_mw = 0;
  uint32_t gpu_draw_mw = 0;     // power draw now
  uint32_t gpu_clock_mhz = 0;   // graphics clock now
  uint32_t gpu_clock_max_mhz = 0;
  int gpu_temp_c = -1;
  bool gpu_reasons_known = false;
  uint64_t gpu_reasons = 0;     // NVML clocks throttle reasons bit mask (0x1 idle, 0x4 software power cap, 0x20 software thermal ...)

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
// target_fps >= 60 and guest_height >= 992 and a reason applies.
void LogSystemStatus(const SystemStatus& status, int target_fps, int guest_height);

// The pure rules (the warning text, the GPU-capped test, the Lenovo mode names, the refresh multiple) live in
// system_status_rules.h, which only needs this header and the standard library: mcla/tests checks them.

// One compact line, e.g. `AC power | mode Best performance | 120 Hz 2880x1800 | cpu 20 logical`.
std::string SystemStatusOneLine(const SystemStatus& status);

// One `[mcla-sys] sample:` line (the watchdog writes one every 30 s): power source, the clock of each CPU efficiency class
// against its maximum and its current limit (a limit under the maximum = CAPPED: thermal, power or a quiet mode), the
// process' CPU use in cores, page faults and reads a second, its memory, and the system's memory load. One caller.
void LogSystemSample();

}  // namespace mcla_host

#include "system_status_rules.h"
