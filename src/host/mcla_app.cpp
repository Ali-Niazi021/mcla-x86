// mcla-x86 host: the ReXApp of the port. See mcla_app.h.
//
// Patterns taken from Renan Queiroz's mcla-recomp (GPL-3, src/mcla_app.cpp: the shared input assignment, the explicit
// GPU plugin load in OnPreSetup, the timer-resolution opt-out) and from the iOS port's NfsmwApp (common/src/nfsmw_app.h).
#include "mcla_app.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system.h>
#include <rex/system/gpu_plugin.h>
#include <rex/version.h>

#include "pc_profile.h"  // mcla/profile (GAME slice): mcla::ApplyPcProfileLogging(), mcla::ApplyPcProfile()
#include "frame_cap.h"
#include "win32_time.h"

// Fingerprint of the SDK patch series this build was linked against (CMake: md5 of .mcla-x86/series.applied).
#ifndef MCLA_SDK_SERIES
#define MCLA_SDK_SERIES "unknown"
#endif
#ifndef REXGLUE_BUILD_CONFIG
#define REXGLUE_BUILD_CONFIG "unknown"
#endif

// The SDK has no GPU-backend cvar: the plugin takes the backend as an argument of LoadGpuPlugin, so the host owns one.
REXCVAR_DEFINE_STRING(mcla_gpu_backend, "d3d12", "MCLA",
                      "Graphics API of the Xenos GPU plugin: d3d12, vulkan, or any (the plugin's own order: "
                      "Direct3D 12 first). A requested API that fails to load stops the start-up instead of "
                      "falling back. Takes effect at the next start")
    .allowed({"d3d12", "vulkan", "any"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(mcla_watchdog, true, "MCLA",
                    "Hang watchdog: dumps every guest thread to the log when nothing moves any more. It also writes "
                    "the '[mcla-heartbeat] frames=<n>' line every 10 s that tools/run_game.py reads, so leave it on "
                    "for scripted runs");

// The frame cap (frame_cap.h, W3.3). The limiter itself is called from the game layer, at the guest's swap.
REXCVAR_DEFINE_INT32(mcla_fps_cap, 0, "MCLA",
                     "Host frame cap at the guest's swap: 0 = none (shipped: the guest's present interval and vblank "
                     "pace the game), 30, 60 or 120. With --mcla_sim60=true it is also the cap the world assumes "
                     "(unless mcla_sim_cap says otherwise); without it only 30 is meaningful (the shipped step is "
                     "1/30 s). 120 needs --video_mode_refresh_rate=120. Live")
    .range(0, 240);
REXCVAR_DEFINE_BOOL(mcla_fps_cap_spin, false, "MCLA",
                    "Frame cap: wait by spinning on the clock alone (log mode=spin) instead of a high-resolution "
                    "timer plus a short spin. Burns a core; for A/B of the timer's precision. Live");
REXCVAR_DEFINE_INT32(mcla_fps_cap_tail_us, 250, "MCLA",
                     "Frame cap and sim pacer: microseconds at the end of every wait that are spun after the timer "
                     "wakes (the high-resolution timer wakes late by up to ~0.8 ms with a short tail: W3.3 measured p99 "
                     "lateness 0.45-0.85 ms with 300-400 us and 0.1 ms with 1000 us; R7 trimmed the default 1000 -> 250 "
                     "because every wait spun its whole tail, about 0.12 core at the cap, and the 60 fps preset now runs "
                     "no host limiter: the guest vblank gates the swaps). Larger = more even frames, more CPU. Live")
    .range(100, 4000);

namespace {

std::string Utf8FromWide(const wchar_t* wide) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string utf8(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), size, nullptr, nullptr);
  return utf8;
}

// The DXGI adapters, for the log: which GPU a run was on is the first thing to know when reading a performance number.
void LogAdapters() {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    REXLOG_WARN("[mcla] DXGI: could not create a factory, adapters not listed");
    return;
  }
  for (UINT index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) {
      continue;
    }
    REXLOG_INFO("[mcla] adapter {}: {} vendor=0x{:04X} device=0x{:04X} vram={} MiB shared={} MiB{}", index,
                Utf8FromWide(desc.Description), desc.VendorId, desc.DeviceId,
                static_cast<uint64_t>(desc.DedicatedVideoMemory) >> 20,
                static_cast<uint64_t>(desc.SharedSystemMemory) >> 20,
                (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
  }
}

}  // namespace

McLaApp::McLaApp(rex::ui::WindowedAppContext& ctx, const rex::PPCImageInfo& image)
    // "mcla_x86" names the config file (<exe dir>/mcla_x86.toml), the user folder (Documents/mcla_x86), the log files
    // and the window title. Not "mcla": Renan's build uses that one, and saves must never mix.
    : rex::ReXApp(ctx, "mcla_x86", image) {}

McLaApp::~McLaApp() = default;

void McLaApp::FailStartup(const std::string& message, int exit_code) {
  REXLOG_ERROR("[mcla] {}", message);
  rex::FlushLogging();
  // A script drives the run unattended (run_game.py): a message box would sit there until the timeout kills the game.
  if (!input_.active()) {
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, message);
  }
  std::_Exit(exit_code);
}

void McLaApp::OnConfigurePaths(rex::PathConfig& /*paths*/) {
  // The earliest hook, before any guest thread and before the log exists. Windows 11 stops honouring a timer-resolution
  // request while no window of the process is visible (minimised, covered, display asleep); opt out of that first.
  timer_throttling_opt_out_ = mcla_host::OptOutOfTimerResolutionThrottling(&timer_throttling_error_);
  timer_resolution_active_ = mcla_host::BeginTimerResolution(1);

  // The log is created right after this hook and reads log_max_file_size_mb / log_flush_interval there, so the PC
  // profile's log defaults have to be in by now (every other cvar of the profile goes in OnPreSetup).
  mcla::ApplyPcProfileLogging();

  // Scripted input: parsed before anything starts, so a typo fails at once (OnPostInitLogging reports it).
  input_.Configure(mcla_host::InputOptionsFromCommandLine(), input_error_);
}

void McLaApp::OnPostInitLogging() {
  REXLOG_INFO("[mcla] {} {} built {} {} with clang {}", GetName(), REXGLUE_BUILD_CONFIG, __DATE__, __TIME__,
              __clang_version__);
  REXLOG_INFO("[mcla] SDK rexglue {} (patch series {})", REXGLUE_VERSION_STRING, MCLA_SDK_SERIES);
  if (timer_resolution_active_) {
    REXLOG_INFO("[mcla] timer resolution 1 ms{}",
                timer_throttling_opt_out_ ? ", kept while the window is hidden"
                                          : ", NOT kept while the window is hidden");
  } else {
    REXLOG_WARN("[mcla] timeBeginPeriod(1) failed: timers stay on the system grid");
  }
  if (!timer_throttling_opt_out_) {
    REXLOG_WARN("[mcla] could not opt out of timer-resolution throttling (error {})", timer_throttling_error_);
  }
  mcla_host::LogFrameCapStartup();

  if (!input_error_.empty()) {
    // OnConfigurePaths could not read or parse the script: nothing has started yet, fail at once (exit 2, the code the
    // iOS harness used for a bad script).
    REXLOG_ERROR("[mcla-input] {}", input_error_);
    rex::FlushLogging();
    std::_Exit(2);
  }
  if (input_.active()) {
    REXLOG_INFO("[mcla] scripted input: {} step(s), driven from the process clock", input_.step_count());
  }
}

void McLaApp::OnPreSetup(rex::RuntimeConfig& config) {
  // Every controller is player one (a game for one console: the SDK's default gives each pad its own player in
  // connection order and a second pad, Steam Input for example, becomes player two, which has no profile). The script
  // pad, when there is one, is added to the same system as a synthetic device for user 0.
  config.input_factory = [this](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto input = rex::input::CreateDefaultInputSystem(tool_mode);
    if (input && !tool_mode) {
      input->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
      input_.AttachTo(*input);
    }
    return input;
  };

  LogAdapters();

  // The GPU plugin is loaded here, not left to the SDK: the SDK loads it after this hook, but both the plugin's cvars
  // (they exist only once it is loaded) and their consumers are needed first. Runtime::Setup, which comes later, starts
  // the command processor and the render-target cache, and they read edram_tiles and render_target_max_height at that
  // moment. A requested backend that fails to load is an error, never a silent fallback to the other API.
  config.gpu_plugin = "xenos";
  const std::string backend = REXCVAR_GET(mcla_gpu_backend);
  REXLOG_INFO("[mcla] GPU backend: {}", backend);
  config.graphics = rex::system::LoadGpuPlugin("xenos", backend);
  if (!config.graphics) {
    FailStartup("Could not load the xenos GPU plugin with backend '" + backend +
                    "'. See the log for the reason (rexgpu-xenos.dll next to the executable? a GPU driver for "
                    "that API?).",
                3);
  }

  // The PC profile: cvar defaults that need the plugin's cvars to exist and must come before Runtime::Setup.
  mcla::ApplyPcProfile();
}

void McLaApp::OnPostSetup() {
  if (REXCVAR_GET(mcla_watchdog)) {
    watchdog_.Start();
  } else {
    REXLOG_INFO("[mcla] hang watchdog off (--mcla_watchdog=false)");
  }
  input_.Start();
}

void McLaApp::OnGuestThreadExit(rex::system::XThread* /*thread*/) {
  // The title is over: threads stop moving by design from here on, which must not read as a hang.
  watchdog_.Stop();
}

void McLaApp::OnShutdown() {
  watchdog_.Stop();
  input_.Stop();
  if (timer_resolution_active_) {
    mcla_host::EndTimerResolution(1);
    timer_resolution_active_ = false;
  }
}
