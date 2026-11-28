// Unit test of the pure rules behind the `[mcla-sys]` lines (src/host/system_status_rules.h): the refresh multiple, the
// Windows power mode and the GPU power limit judged against a frame-rate target, the Lenovo mode names and the warning
// text. Standalone: the rules need only SystemStatus (system_status.h) and the standard library; no SDK, no Windows, no GPU.
//
// Built and run by CTest (mcla/tests/CMakeLists.txt, test mcla_system_status_test). By hand:
//   clang++ -std=c++23 -O1 -Wall -Wextra -I src/host mcla/tests/test_system_status.cpp -o <scratch>/test_system_status
#include <cstdint>
#include <cstdio>
#include <string>

#include "system_status.h"

using namespace mcla_host;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    ++g_checks;                                                   \
    if (!(cond)) {                                                \
      ++g_failures;                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                             \
  } while (0)

#define CHECK_HAS(text, needle)                                                                            \
  do {                                                                                                     \
    const std::string t_ = (text);                                                                         \
    ++g_checks;                                                                                            \
    if (t_.find(needle) == std::string::npos) {                                                            \
      ++g_failures;                                                                                        \
      std::printf("FAIL %s:%d: \"%s\" does not contain \"%s\"\n", __FILE__, __LINE__, t_.c_str(), needle); \
    }                                                                                                      \
  } while (0)

// The test laptop: AC, High performance plan (the slider says Best performance, which the plan switches off, so the
// effective overlay reads as Balanced), a 120 Hz mode, an RTX 4060 Laptop at 55 W of 100 W in Lenovo's auto mode.
SystemStatus Laptop() {
  SystemStatus s;
  s.power_source = PowerSource::kAc;
  s.has_battery = true;
  s.battery_percent = 96;
  s.overlay_known = true;
  s.overlay_guid = kOverlayBalanced;
  s.overlay_name = "Balanced";
  s.overlay_actual_known = true;
  s.overlay_actual_guid = kOverlayBestPerformance;
  s.overlay_actual_name = "Best performance";
  s.scheme_guid = kPlanHighPerformance;
  s.scheme_name = "High performance";
  s.plan_is_performance = true;
  s.display_known = true;
  s.display_width = 3200;
  s.display_height = 2000;
  s.display_hz = 120;
  s.lenovo_present = true;
  s.lenovo_mode = 0;
  s.lenovo_capability = 10;
  s.gpu_known = true;
  s.gpu_name = "NVIDIA GeForce RTX 4060 Laptop GPU";
  s.gpu_limit_mw = 55000;
  s.gpu_default_mw = 55000;
  s.gpu_min_mw = 5000;
  s.gpu_max_mw = 100000;
  return s;
}

void TestRefresh() {
  CHECK(RefreshIsMultipleOf(120, 60));
  CHECK(RefreshIsMultipleOf(60, 60));
  CHECK(RefreshIsMultipleOf(59, 60));  // 59.94 Hz reports as 59 or 60
  CHECK(!RefreshIsMultipleOf(165, 60));
  CHECK(!RefreshIsMultipleOf(144, 60));
  CHECK(RefreshIsMultipleOf(120, 120));
  CHECK(!RefreshIsMultipleOf(165, 120));
  CHECK(RefreshIsMultipleOf(240, 120));
  CHECK(RefreshIsMultipleOf(0, 60));  // unknown
  CHECK(RefreshIsMultipleOf(60, 0));
}

void TestLenovoNames() {
  CHECK(std::string(LenovoModeName(0)) == "Intelligent Cooling (auto)");
  CHECK(std::string(LenovoModeName(1)) == "Battery Saving");
  CHECK(std::string(LenovoModeName(3)) == "Extreme Performance");
  CHECK(std::string(LenovoModeName(7)) == "an unknown mode");
  CHECK(LenovoModeIsPerformance(3));
  CHECK(!LenovoModeIsPerformance(0));
  CHECK(!LenovoModeIsPerformance(1));
}

void TestGpuCap() {
  SystemStatus s = Laptop();
  CHECK(GpuPowerCapped(s));  // 55 of 100 W
  s.gpu_limit_mw = 100000;
  CHECK(!GpuPowerCapped(s));  // the performance mode
  s.gpu_limit_mw = 70000;
  CHECK(!GpuPowerCapped(s));  // exactly 70 %: not "well below"
  s.gpu_limit_mw = 69900;
  CHECK(GpuPowerCapped(s));
  s.gpu_limit_mw = 0;  // unreadable
  CHECK(!GpuPowerCapped(s));
  s = Laptop();
  s.gpu_max_mw = 0;
  CHECK(!GpuPowerCapped(s));
  s = Laptop();
  s.gpu_known = false;
  CHECK(!GpuPowerCapped(s));
  CHECK(Watts(55000) == "55");
  CHECK(Watts(1400) == "1.4");
  CHECK(Watts(87650) == "87.7");
  CHECK(Watts(100000) == "100");
}

void TestWindowsMode() {
  SystemStatus s = Laptop();
  CHECK(WindowsModeIsPerformance(s));  // the High performance plan: the slider is off, nothing to say
  s.plan_is_performance = false;
  CHECK(!WindowsModeIsPerformance(s));  // Balanced plan, effective Balanced
  s.overlay_guid = kOverlayBestPerformance;
  s.overlay_name = "Best performance";
  CHECK(WindowsModeIsPerformance(s));
  s.overlay_known = false;
  s.plan_is_performance = false;
  CHECK(WindowsModeIsPerformance(s));  // unknown
  CHECK(std::string(OverlayName(kOverlayBestPerformance)) == "Best performance");
  CHECK(OverlayName("12345678-0000-0000-0000-000000000000") == nullptr);
}

void TestWarning() {
  // Below the thresholds: no warning at all.
  SystemStatus s = Laptop();
  CHECK(SystemStatusWarning(s, 30, 2000).empty());
  CHECK(SystemStatusWarning(s, 60, 720).empty());
  CHECK(SystemStatusWarning(s, 120, 991).empty());  // under the 1600x992 guest of the 992p / 1984p profiles

  // The test laptop in Lenovo's auto mode: advice, naming the vendor mode and the watts.
  std::string w = SystemStatusWarning(s, 120, 1000);
  CHECK_HAS(w, "120 fps at 1000p may be held back");
  CHECK_HAS(SystemStatusWarning(s, 120, 992), "120 fps at 992p may be held back");  // 1984p's guest: 3200x1984 on the host
  CHECK_HAS(w, "55 W of its 100 W maximum");
  CHECK_HAS(w, "Intelligent Cooling is on Intelligent Cooling (auto)");
  CHECK_HAS(w, "Extreme Performance");
  CHECK(w.find("unlikely") == std::string::npos);

  // In the performance mode (100 of 100 W): quiet.
  s.gpu_limit_mw = 100000;
  s.lenovo_mode = 3;
  CHECK(SystemStatusWarning(s, 60, 2000).empty());

  // 165 Hz is a hard reason (unchanged rule), the GPU cap still follows as advice.
  s = Laptop();
  s.display_hz = 165;
  w = SystemStatusWarning(s, 60, 2000);
  CHECK_HAS(w, "60 fps at 2000p is unlikely: display refresh 165 Hz is not a multiple of 60");
  CHECK_HAS(w, "may also be held back: the GPU is held to 55 W");

  // A machine that is not a Lenovo: generic advice.
  s = Laptop();
  s.lenovo_present = false;
  CHECK_HAS(SystemStatusWarning(s, 60, 1080), "usually raises it");

  // Lenovo already on Extreme Performance but still capped (the charger or the firmware).
  s = Laptop();
  s.lenovo_mode = 3;
  CHECK_HAS(SystemStatusWarning(s, 60, 1080), "already on Extreme Performance");

  // The Windows power mode: Balanced plan, effective Balanced, AC: advice. Best power efficiency: a hard reason.
  s = Laptop();
  s.gpu_limit_mw = 100000;
  s.plan_is_performance = false;
  s.scheme_guid = "381b4222-f694-41f0-9685-ff5bb260df2e";
  w = SystemStatusWarning(s, 60, 1080);
  CHECK_HAS(w, "may be held back: Windows power mode is Balanced, not Best performance");
  s.overlay_guid = kOverlayBestEfficiency;
  s.overlay_name = "Best power efficiency";
  w = SystemStatusWarning(s, 60, 1080);
  CHECK_HAS(w, "is unlikely: Windows power mode is Best power efficiency");
  CHECK(w.find("not Best performance") == std::string::npos);  // said once, as the hard reason

  // On battery: the old rule, unchanged.
  s = Laptop();
  s.gpu_limit_mw = 100000;
  s.power_source = PowerSource::kBattery;
  s.battery_saver = true;
  w = SystemStatusWarning(s, 60, 1080);
  CHECK_HAS(w, "is unlikely: on battery; battery saver on");
}

}  // namespace

int main() {
  TestRefresh();
  TestLenovoNames();
  TestGpuCap();
  TestWindowsMode();
  TestWarning();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
