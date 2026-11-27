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
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include <fmt/format.h>
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
#include "gpu_priority.h"
#include "power_boost.h"
#include "system_status.h"
#include "win32_time.h"
#include "window_fit.h"

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
    // The displays each GPU drives: on a hybrid laptop (NVIDIA Optimus, AMD switchable) the fast GPU often has none,
    // and every frame it renders is copied to the other GPU to be shown.
    std::string outputs;
    for (UINT o = 0;; ++o) {
      Microsoft::WRL::ComPtr<IDXGIOutput> output;
      if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      DXGI_OUTPUT_DESC od{};
      if (SUCCEEDED(output->GetDesc(&od))) {
        outputs += fmt::format("{}{} {}x{}", outputs.empty() ? "" : ", ", Utf8FromWide(od.DeviceName),
                               od.DesktopCoordinates.right - od.DesktopCoordinates.left,
                               od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
      }
    }
    REXLOG_INFO("[mcla] adapter {}: {} vendor=0x{:04X} device=0x{:04X} vram={} MiB shared={} MiB{}; displays: {}",
                index, Utf8FromWide(desc.Description), desc.VendorId, desc.DeviceId,
                static_cast<uint64_t>(desc.DedicatedVideoMemory) >> 20,
                static_cast<uint64_t>(desc.SharedSystemMemory) >> 20,
                (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "", outputs.empty() ? "none" : outputs);
  }
}

// Which GPU drives the monitor the game window is on, against the one that renders (the SDK's d3d12_adapter rule: -1 =
// the first hardware adapter). A different one means every frame crosses to the other GPU before it is shown (a copy
// over PCIe plus a composition by the DWM: no independent flip, more latency, and a frame that misses the copy waits a
// whole refresh). Laptops with a MUX switch can wire the panel to the fast GPU ("dGPU only" / "Ultimate" mode).
void LogDisplayPath(HWND hwnd) {
  HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!monitor || !GetMonitorInfoW(monitor, &mi)) {
    return;
  }
  const std::string monitor_name = Utf8FromWide(mi.szDevice);
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return;
  }
  const int32_t wanted = rex::cvar::Query<int32_t>("d3d12_adapter");
  int render_index = -1, display_index = -1;
  std::string render_name, display_name;
  for (UINT index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) {
      continue;
    }
    const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    if (render_index < 0 && ((wanted >= 0 && int32_t(index) == wanted) || (wanted == -1 && !software))) {
      render_index = int(index);
      render_name = Utf8FromWide(desc.Description);
    }
    for (UINT o = 0; display_index < 0; ++o) {
      Microsoft::WRL::ComPtr<IDXGIOutput> output;
      if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      DXGI_OUTPUT_DESC od{};
      if (SUCCEEDED(output->GetDesc(&od)) && od.Monitor == monitor) {
        display_index = int(index);
        display_name = Utf8FromWide(desc.Description);
      }
    }
  }
  if (display_index < 0 || render_index < 0) {
    REXLOG_INFO("[mcla-display] the game window is on {}; its GPU could not be matched (render adapter {})",
                monitor_name, render_index);
  } else if (display_index == render_index) {
    REXLOG_INFO("[mcla-display] the game window is on {}, driven by the rendering GPU ({}): direct path",
                monitor_name, render_name);
  } else {
    REXLOG_WARN("[mcla-display] HYBRID PATH: the game renders on {} but the window is on {}, driven by {}: every "
                "frame is copied to that GPU and composed by Windows before it is shown (adds latency; a copy that "
                "misses a refresh repeats the previous frame). If the laptop has a MUX switch, the dGPU-only mode "
                "(NVIDIA Control Panel / vendor app) removes the copy; an external monitor on a port wired to the "
                "fast GPU does too",
                render_name, monitor_name, display_name);
  }
}

// DXGI's own answer: can this system present with tearing, which variable refresh needs in a window or borderless?
bool DxgiSupportsTearing() {
  Microsoft::WRL::ComPtr<IDXGIFactory5> factory;
  BOOL allow_tearing = FALSE;
  return SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
         SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
                                                sizeof(allow_tearing))) &&
         allow_tearing != FALSE;
}

// A panel whose refresh rate is not a whole multiple of the frame-rate target (60 fps on 165, 144, 100 or 75 Hz)
// cannot show the frames evenly: each stays a whole number of refreshes (2.75 on average at 60 on 165, so 3, 3, 3, 2),
// a beat that is judder by design: no frame-time counter and no [present-stats] hitch line will point at a fault,
// and no setting of the game fixes it. Only variable refresh (the panel follows the presents: G-Sync / FreeSync, which
// needs tearing allowed; off by default, because on a TV or an ordinary monitor it tears) or a panel mode that is a
// multiple of the target (120 or 60 Hz) does. Said once, plainly, when variable refresh is not in use; with it allowed
// the [present-stats] lines show what the display really did.
void LogRefreshCadence(void* native_window, bool tearing_allowed) {
  const int target_fps = rex::cvar::Query<int32_t>("mcla_fps_target");
  const mcla_host::SystemStatus status = mcla_host::QuerySystemStatus(native_window);
  if (target_fps <= 0 || status.display_hz <= 0 || mcla_host::RefreshIsMultipleOf(status.display_hz, target_fps)) {
    return;
  }
  const bool tearing_supported = DxgiSupportsTearing();
  const int lower = status.display_hz / target_fps;  // frames last `lower` or `lower + 1` refreshes
  const double per_frame = double(status.display_hz) / double(target_fps);
  if (!tearing_allowed || !tearing_supported) {
    const std::string multiples = lower >= 2 ? fmt::format("{} Hz or {} Hz", lower * target_fps, target_fps)
                                             : fmt::format("{} Hz", target_fps);
    REXLOG_WARN("[mcla-display] {} fps cannot be shown evenly on this panel: it refreshes at {} Hz, {:.2f} refreshes a "
                "frame, so the frames stay {} or {} refreshes on screen in a repeating beat (judder by design, not a "
                "slow game), and variable refresh is not in use ({}). What fixes it: a G-Sync / FreeSync panel "
                "with variable refresh on in the GPU driver and \"Allow tearing (G-Sync / FreeSync only)\" on in the "
                "launcher (Display, Advanced; command line --d3d12_allow_variable_refresh_rate_and_tearing=true), or "
                "the panel set to {} in Windows (Settings, System, Display, Advanced display)",
                target_fps, status.display_hz, per_frame, lower, lower + 1,
                !tearing_allowed ? "tearing is off, which is the default: no tearing on a TV or an ordinary monitor"
                                 : "the system does not allow tearing presents (DXGI_FEATURE_PRESENT_ALLOW_TEARING)",
                multiples);
  } else {
    REXLOG_INFO("[mcla-display] {} fps on a {} Hz panel is {:.2f} refreshes a frame: the frames stay {} or {} refreshes "
                "on screen (judder by design) unless the panel runs variable refresh. Tearing is allowed, so G-Sync / "
                "FreeSync can work; it has to be on for this panel in the GPU driver. The '[present-stats]' lines "
                "say which it was ('cadence', 'refresh clock': a clock below the display mode's rate is variable "
                "refresh)",
                target_fps, status.display_hz, per_frame, lower, lower + 1);
  }
}

// The settings that decide whether the frames are locked to the display. vsync=false does NOT change how the frames
// are presented here (the presenter always presents with sync interval 0): it only stops the guest vblank from
// following the display (it free-runs at 1 kHz) and turns the command processor's waits into spins. The 60 fps pacer
// then holds the rate on a timer of its own that drifts against the panel's refresh: every frame lands at an arbitrary,
// slowly moving point of the refresh, and the ones that cross the composition deadline are shown a refresh late (a
// repeat, then a skip: judder the game's own frame-time counters cannot see).
// Whether a frame can tear is the other half, d3d12_allow_variable_refresh_rate_and_tearing (the PC profile turns it
// off): with it off the presenter's sync-interval-0 present is held for the display's next refresh, with it on the frame
// is flipped at once, in the middle of a refresh (clean only on a G-Sync / FreeSync display).
void LogPresentSettings(void* native_window) {
  const bool vsync = rex::cvar::Query<bool>("vsync");
  const std::string tearing = rex::cvar::GetFlagByName("d3d12_allow_variable_refresh_rate_and_tearing");
  const std::string lock = rex::cvar::GetFlagByName("guest_vblank_display_lock");
  REXLOG_INFO("[mcla-display] present settings: vsync={} (guest vblank {}), tearing allowed={}, guest_vblank_display_lock={}",
              vsync ? "true" : "false", vsync ? "on the display's refresh" : "FREE-RUNNING at 1 kHz",
              tearing.empty() ? "?" : tearing, lock.empty() ? "?" : lock);
  // The same pair in player terms, once. Tearing only happens when it is allowed AND the system can present with it.
  const bool tearing_asked = tearing != "false";
  const bool tearing_possible = tearing_asked && DxgiSupportsTearing();
  const bool locked = vsync && lock != "false";
  const char* tearing_text =
      tearing_possible
          ? "tearing allowed: a frame can reach the screen in the middle of a refresh (clean on a G-Sync / FreeSync "
            "display with variable refresh on in the GPU driver, tearing on a TV or an ordinary monitor)"
          : (tearing_asked ? "no tearing (tearing is allowed but this system cannot present with it): each frame "
                             "waits for the display's next refresh"
                           : "no tearing: each frame waits for the display's next refresh (right for a TV or an "
                             "ordinary monitor)");
  if (locked) {
    REXLOG_INFO("[mcla-display] effective: locked to the display, {}", tearing_text);
  } else {
    REXLOG_INFO("[mcla-display] effective: NOT locked to the display (the frames follow a timer of their own and drift "
                "against the refresh), {}",
                tearing_text);
  }
  if (!vsync) {
    REXLOG_WARN("[mcla-display] vsync=false: the game is NOT locked to the display. The frames keep 60 fps on a timer "
                "but drift against the panel's refresh, so some are shown a refresh late (judder), and the GPU command "
                "thread spins instead of sleeping. It lowers no latency here. Remove --vsync=false (the launcher has "
                "no switch for it)");
  }
  LogRefreshCadence(native_window, tearing_asked);
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
  // The exact command line (the launcher builds it from its settings): which options this run had.
  REXLOG_INFO("[mcla] command line: {}", Utf8FromWide(GetCommandLineW()));
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
    REXLOG_INFO("[mcla] scripted input: {} pad step(s), {} schedule step(s), driven from the process clock",
                input_.step_count(), input_.sched_count());
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

std::optional<rex::PathConfig> McLaApp::OnFinalizePaths(const rex::PathConfig& defaults,
                                                        std::function<void(rex::PathConfig)> /*resume*/) {
  // The SDK has just created and opened the window (SetupPresentation) and will construct the runtime right after this
  // returns: the last moment to size the window before the first frame can be presented. Paths pass through unchanged.
  if (window()) {
    mcla_host::FitWindowToGuestPixels(*window());
    // What the machine is doing (power source, Windows power mode, boost, display refresh): the first thing to know
    // when a run is slow. Here because the HWND exists and the process is per-monitor DPI aware by now.
    int32_t guest_width = 0;
    int32_t guest_height = 0;
    mcla_host::GuestVideoModeSize(guest_width, guest_height);
    mcla_host::LogSystemStatus(mcla_host::QuerySystemStatus(window()->GetNativeWindowHandle()),
                               rex::cvar::Query<int32_t>("mcla_fps_target"), guest_height);
    LogDisplayPath(static_cast<HWND>(window()->GetNativeWindowHandle()));
  }
  LogPresentSettings(window() ? window()->GetNativeWindowHandle() : nullptr);
  return defaults;
}

void McLaApp::OnPostSetup() {
  if (REXCVAR_GET(mcla_watchdog)) {
    watchdog_.Start();
  } else {
    REXLOG_INFO("[mcla] hang watchdog off (--mcla_watchdog=false)");
  }
  input_.Start();
  mcla_host::StartGpuPriority();
  mcla_host::StartPowerBoost();  // also undoes a boost cap a crashed run left behind
  thread_policy_.Start();
  if (window()) {
    mcla_host::LogFocusState(*window(), "post-setup");
  }
}

void McLaApp::OnGuestThreadExit(rex::system::XThread* /*thread*/) {
  // The title is over: threads stop moving by design from here on, which must not read as a hang.
  watchdog_.Stop();
}

void McLaApp::OnShutdown() {
  watchdog_.Stop();
  input_.Stop();
  thread_policy_.Stop();
  mcla_host::StopGpuPriority();
  mcla_host::StopPowerBoost();
  if (timer_resolution_active_) {
    mcla_host::EndTimerResolution(1);
    timer_resolution_active_ = false;
  }
}
