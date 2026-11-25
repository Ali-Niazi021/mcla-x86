// mcla-x86 host: the ReXApp of the port.
//
// The SDK supplies wWinMain (cvar parsing, the SDL app context, COM, the message loop); this class supplies the
// game-specific parts of start-up. What runs where (SDK order, src/ui/rex_app.cpp):
//
//   OnConfigurePaths   timer resolution (1 ms, kept while the window is hidden), log cvars of the PC profile
//                      (the log is created right after this hook), --input-file parsing
//   OnPostInitLogging  start-up banner, the frame cap line ([mcla-cap], frame_cap.h)
//   OnPreSetup         input system (scripted pad, every pad is player 1), the GPU plugin loaded with the backend
//                      of --mcla_gpu_backend, the PC profile of cvar defaults (the GPU cvars must be set before the
//                      plugin's Runtime::Setup reads them: edram_tiles, render_target_max_height)
//   OnPostSetup        hang watchdog, input script clock
//   OnShutdown         stops both, gives the timer resolution back
//
// Host cvars, all in mcla_app.cpp: mcla_gpu_backend (d3d12 | vulkan | any), mcla_watchdog (bool), and the frame cap's
// mcla_fps_cap (0 | 30 | 60 | 120), mcla_fps_cap_spin, mcla_fps_cap_tail_us (frame_cap.h explains the limiter).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <rex/rex_app.h>

#include "input_script.h"
#include "watchdog.h"

class McLaApp : public rex::ReXApp {
 public:
  // Defined in main.cpp, next to REX_DEFINE_APP, because it needs the generated PPCImageConfig.
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

  McLaApp(rex::ui::WindowedAppContext& ctx, const rex::PPCImageInfo& image);
  ~McLaApp() override;

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPostInitLogging() override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  // The window exists and is open, the runtime does not: where the window is made pixel-exact (window_fit.h).
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override;
  void OnPostSetup() override;
  void OnGuestThreadExit(rex::system::XThread* thread) override;
  void OnShutdown() override;

 private:
  // Logs and exits with `exit_code` (after a message box, unless a script drives the run). For start-up failures
  // that the SDK's own flow would swallow or turn into a silent fallback.
  [[noreturn]] void FailStartup(const std::string& message, int exit_code);

  mcla_host::ScriptedInput input_;
  mcla_host::Watchdog watchdog_;

  // Results of OnConfigurePaths, logged by OnPostInitLogging (the log does not exist yet when they are produced).
  std::string input_error_;
  bool timer_resolution_active_ = false;
  bool timer_throttling_opt_out_ = false;
  uint32_t timer_throttling_error_ = 0;
};
