// mcla-x86 host: the pure rules behind the `[mcla-sys]` lines (H4), separated from the Win32 and NVML calls so that
// mcla/tests can check them with the standard library alone: the refresh-multiple test, the Windows power mode and the
// GPU power limit judged against a frame-rate target, the Lenovo mode names, and the warning text.
//
// Included at the end of system_status.h; needs only SystemStatus.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "system_status.h"

namespace mcla_host {

// Windows power-mode overlays (the slider of Settings > System > Power) and power plans, by GUID.
inline constexpr const char* kOverlayBestEfficiency = "961cc777-2547-4f9d-8174-7d86181b8a7a";
inline constexpr const char* kOverlayBalanced = "00000000-0000-0000-0000-000000000000";
inline constexpr const char* kOverlayBetterPerformance = "3af9b8d9-7c97-431d-ad78-34a8bfea439f";
inline constexpr const char* kOverlayBestPerformance = "ded574b5-45a0-4f42-8737-46345c09c238";
inline constexpr const char* kPlanHighPerformance = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";
inline constexpr const char* kPlanUltimatePerformance = "e9a42b02-d5df-448d-aa00-03f14749eb61";

inline const char* OverlayName(const std::string& guid) {
  if (guid == kOverlayBestEfficiency) return "Best power efficiency";
  if (guid == kOverlayBalanced) return "Balanced";
  if (guid == kOverlayBetterPerformance) return "Better performance";
  if (guid == kOverlayBestPerformance) return "Best performance";
  return nullptr;
}

// True when `refresh` is, within 1 Hz (59.94 reports as 59 or 60), a whole multiple of `target` (also when either is
// unknown, <= 0). 60 on 120 or 60 is; 60 on 165, 144, 100 or 75 is not: frames then last 2 or 3 refreshes in a beat.
inline bool RefreshIsMultipleOf(int refresh, int target) {
  if (refresh <= 0 || target <= 0) {
    return true;
  }
  const int multiples = std::max(1, (refresh + target / 2) / target);
  return std::abs(refresh - multiples * target) <= 1;
}

// A power plan that already lets the processor boost freely: the High performance and Ultimate performance plans. With
// one of them active the Windows power-mode slider is switched off, and the effective overlay reads as "Balanced / none".
inline bool WindowsModeIsPerformance(const SystemStatus& status) {
  if (status.plan_is_performance) return true;
  if (status.overlay_known) return status.overlay_guid == kOverlayBestPerformance;
  return true;  // unknown: nothing to say
}

// The Lenovo Intelligent Cooling modes as the registry value MMC\CurrentSetting holds them, read back after setting each one with
// the vendor's own API on an IdeaPad Slim Pro 9 16IRP8: 0 (with AutomaticModeSetting 2) = ItsAuto, 1 = MmcCool, 3 = MmcPerformance.
// The enforced GPU power limit of that machine in each: 55 W, 55 W, 100 W. Other values are not seen yet.
inline const char* LenovoModeName(uint32_t setting) {
  switch (setting) {
    case 0:
      return "Intelligent Cooling (auto)";
    case 1:
      return "Battery Saving";
    case 3:
      return "Extreme Performance";
    default:
      return "an unknown mode";
  }
}
inline bool LenovoModeIsPerformance(uint32_t setting) { return setting == 3; }

// The GPU's enforced power limit is under 70 % of its maximum: on the test laptop (RTX 4060 Laptop) 55 of 100 W in the vendor's
// default mode, 100 of 100 W in its performance mode. Unreadable values are not a reason.
inline bool GpuPowerCapped(const SystemStatus& status) {
  return status.gpu_known && status.gpu_limit_mw > 0 && status.gpu_max_mw > 0 &&
         uint64_t(status.gpu_limit_mw) * 10 < uint64_t(status.gpu_max_mw) * 7;
}

inline std::string Watts(uint32_t milliwatts) {
  const uint32_t tenths = (milliwatts + 50) / 100;
  return tenths % 10 == 0 ? std::to_string(tenths / 10) : std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

// `N fps at Hp is unlikely: <reasons>`, with `; may also be held back: <advice>` or on its own `N fps at Hp may be held back:
// <advice>`. Hard reasons: on battery, battery saver, Windows power mode "Best power efficiency", a display refresh that is not
// a multiple of the target. Advice (a setting that is usually free to change): the Windows power mode is not Best performance
// on a plan that does not boost freely, and the GPU is held to under 70 % of its maximum power limit (the Lenovo mode is named
// when the machine has one). Empty when target_fps < 60, guest_height < 1000 or nothing applies. (The plan's boost mode and
// maximum state are logged but are not reasons.)
inline std::string SystemStatusWarning(const SystemStatus& status, int target_fps, int guest_height) {
  if (target_fps < 60 || guest_height < 1000) {
    return {};
  }
  std::string hard, advice;
  auto add = [](std::string& list, const std::string& text) {
    if (!list.empty()) {
      list += "; ";
    }
    list += text;
  };
  if (status.power_source == PowerSource::kBattery) {
    add(hard, "on battery");
  }
  if (status.battery_saver) {
    add(hard, "battery saver on");
  }
  if (status.overlay_known && status.overlay_guid == kOverlayBestEfficiency) {
    add(hard, "Windows power mode is Best power efficiency");
  }
  if (status.display_hz > 0 && !RefreshIsMultipleOf(status.display_hz, target_fps)) {
    add(hard, "display refresh " + std::to_string(status.display_hz) + " Hz is not a multiple of " + std::to_string(target_fps));
  }
  if (!WindowsModeIsPerformance(status) && !(status.overlay_known && status.overlay_guid == kOverlayBestEfficiency)) {
    const std::string name = status.overlay_name.empty() ? std::string("not Best performance") : status.overlay_name;
    add(advice, "Windows power mode is " + name + ", not Best performance (Settings > System > Power & battery)");
  }
  if (GpuPowerCapped(status)) {
    std::string text = "the GPU is held to " + Watts(status.gpu_limit_mw) + " W of its " + Watts(status.gpu_max_mw) + " W maximum";
    if (status.lenovo_present && status.lenovo_mode && !LenovoModeIsPerformance(*status.lenovo_mode)) {
      text += " (Lenovo Intelligent Cooling is on " + std::string(LenovoModeName(*status.lenovo_mode)) +
              ": Extreme Performance, Fn+Q or Lenovo Vantage, raises it)";
    } else if (status.lenovo_present) {
      text += " (Lenovo Intelligent Cooling is already on Extreme Performance: the limit comes from the firmware or the charger)";
    } else {
      text += " (the laptop's performance mode, a vendor app or a Fn key, usually raises it)";
    }
    add(advice, text);
  }
  if (hard.empty() && advice.empty()) {
    return {};
  }
  const std::string head = std::to_string(target_fps) + " fps at " + std::to_string(guest_height) + "p ";
  if (hard.empty()) {
    return head + "may be held back: " + advice;
  }
  return head + "is unlikely: " + hard + (advice.empty() ? std::string() : "; may also be held back: " + advice);
}

}  // namespace mcla_host
