// mcla-x86 host: pixel-exact game window. See window_fit.h.
#include "window_fit.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/logging.h>
#include <rex/ui/flags.h>
#include <rex/ui/window.h>

REXCVAR_DEFINE_BOOL(mcla_window_pixel_exact, true, "MCLA",
                    "Windowed run (fullscreen=false): resize the window so its client area is exactly the guest video "
                    "mode in physical pixels, whatever the display's DPI scale (window_width/window_height are "
                    "logical sizes: 1920x1080 on a 150 % desktop would open 2880x1620 and the presenter would scale "
                    "every frame). Skipped when window_width/window_height are set explicitly to a size other than "
                    "the guest video mode. Takes effect at the next start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla_host {

namespace {

constexpr uint32_t kMediumDpi = 96;

bool ExplicitlySet(std::string_view name) {
  return rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault;
}

}  // namespace

void GuestVideoModeSize(int32_t& width, int32_t& height) {
  // The same resolution order as xboxkrnl_video.cpp GetConfiguredVideoModeWidth/Height and window_sdl.cpp
  // GetGuestVideoModeSize: the video mode cvars; if they are still at their defaults, the window cvars (when changed from
  // theirs), then the `resolution` preset.
  width = REXCVAR_GET(video_mode_width);
  height = REXCVAR_GET(video_mode_height);
  int32_t preset_width = 0;
  int32_t preset_height = 0;
  const bool has_preset =
      rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width, preset_height);
  if (!rex::cvar::HasNonDefaultValue("video_mode_width")) {
    if (rex::cvar::HasNonDefaultValue("window_width") && REXCVAR_GET(window_width) > 0) {
      width = REXCVAR_GET(window_width);
    } else if (has_preset) {
      width = preset_width;
    }
  }
  if (!rex::cvar::HasNonDefaultValue("video_mode_height")) {
    if (rex::cvar::HasNonDefaultValue("window_height") && REXCVAR_GET(window_height) > 0) {
      height = REXCVAR_GET(window_height);
    } else if (has_preset) {
      height = preset_height;
    }
  }
  width = std::clamp(width, 640, 0x0FFF);
  height = std::clamp(height, 480, 0x0FFF);
}

void FitWindowToGuestPixels(rex::ui::Window& window) {
  if (!REXCVAR_GET(mcla_window_pixel_exact)) {
    REXLOG_INFO("[mcla-window] pixel-exact window off (--mcla_window_pixel_exact=false)");
    return;
  }
  if (window.IsFullscreen()) {
    REXLOG_INFO("[mcla-window] fullscreen: the window is the display's, nothing to fit");
    return;
  }
  HWND hwnd = static_cast<HWND>(window.GetNativeWindowHandle());
  if (!hwnd) {
    REXLOG_WARN("[mcla-window] the SDK window has no native handle: not fitted");
    return;
  }

  int32_t guest_width = 0;
  int32_t guest_height = 0;
  GuestVideoModeSize(guest_width, guest_height);

  // A window size the user asked for on purpose (a different size than the guest's) is theirs. window_width/height
  // equal to the guest mode, or at 0, only say "the guest's size", which is what this makes exact. Values set by the PC
  // profile (SetDefaultValue) are defaults, not asks.
  if (ExplicitlySet("window_width") || ExplicitlySet("window_height")) {
    const int32_t window_width = REXCVAR_GET(window_width);
    const int32_t window_height = REXCVAR_GET(window_height);
    if ((window_width != 0 && window_width != guest_width) || (window_height != 0 && window_height != guest_height)) {
      REXLOG_INFO("[mcla-window] window {}x{} (logical) set explicitly for a {}x{} guest: kept as asked", window_width,
                  window_height, guest_width, guest_height);
      return;
    }
  }
  if (IsZoomed(hwnd) || IsIconic(hwnd)) {
    REXLOG_INFO("[mcla-window] the window is maximised or minimised: not fitted");
    return;
  }

  UINT dpi = GetDpiForWindow(hwnd);
  if (dpi == 0) {
    dpi = kMediumDpi;
  }
  const double scale = double(dpi) / double(kMediumDpi);
  // What the SDK's logical size would have to be for this physical size (informational; it rounds up).
  const uint32_t points_width = rex::ui::Window::ConvertSizeDpi(uint32_t(guest_width), kMediumDpi, dpi);
  const uint32_t points_height = rex::ui::Window::ConvertSizeDpi(uint32_t(guest_height), kMediumDpi, dpi);

  RECT client{};
  GetClientRect(hwnd, &client);
  const int old_width = client.right - client.left;
  const int old_height = client.bottom - client.top;
  if (old_width == guest_width && old_height == guest_height) {
    REXLOG_INFO(
        "[mcla-window] client {}x{} physical for a {}x{} guest (scale {:.2f}: window {}x{} points): already exact",
        old_width, old_height, guest_width, guest_height, scale, points_width, points_height);
    return;
  }

  // The outer size for that client area at this window's DPI and style, then centred on the monitor's work area (the
  // window stays fully visible when it fits; when it is larger, its top-left corner is the work area's).
  RECT outer{0, 0, guest_width, guest_height};
  const DWORD style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE));
  const DWORD ex_style = DWORD(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
  if (!AdjustWindowRectExForDpi(&outer, style, FALSE, ex_style, dpi)) {
    REXLOG_WARN("[mcla-window] AdjustWindowRectExForDpi failed ({}): not fitted", GetLastError());
    return;
  }
  const int outer_width = outer.right - outer.left;
  const int outer_height = outer.bottom - outer.top;
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  int x = 0;
  int y = 0;
  if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
    const RECT& work = monitor.rcWork;
    x = std::max<int>(work.left, work.left + ((work.right - work.left) - outer_width) / 2);
    y = std::max<int>(work.top, work.top + ((work.bottom - work.top) - outer_height) / 2);
  } else {
    RECT current{};
    GetWindowRect(hwnd, &current);
    x = current.left;
    y = current.top;
  }
  if (!SetWindowPos(hwnd, nullptr, x, y, outer_width, outer_height, SWP_NOZORDER | SWP_NOACTIVATE)) {
    REXLOG_WARN("[mcla-window] SetWindowPos failed ({}): the window keeps its {}x{} client", GetLastError(), old_width,
                old_height);
    return;
  }

  GetClientRect(hwnd, &client);
  const int new_width = client.right - client.left;
  const int new_height = client.bottom - client.top;
  REXLOG_INFO("[mcla-window] client {}x{} physical for a {}x{} guest (scale {:.2f}: window {}x{} points)", new_width,
              new_height, guest_width, guest_height, scale, points_width, points_height);
  REXLOG_INFO("[mcla-window] resized from {}x{}, outer {}x{} at {},{}", old_width, old_height, outer_width, outer_height,
              x, y);
  if (new_width != guest_width || new_height != guest_height) {
    REXLOG_WARN("[mcla-window] the client area is {}x{}, not the {}x{} asked for (the OS limited the window)",
                new_width, new_height, guest_width, guest_height);
  }
}

}  // namespace mcla_host
