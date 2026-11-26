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
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

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

REXCVAR_DEFINE_STRING(mcla_window_pos, "", "MCLA",
                      "Windowed run: where the game window goes, so that a test run disturbs the machine's owner least. "
                      "Empty = centred on its monitor's work area (the default). bottom-right, bottom-left, top-right, "
                      "top-left = flush with that corner of the monitor's work area (the taskbar stays uncovered); X,Y = "
                      "the outer window's top-left in physical pixels (negative for a monitor left of or above the "
                      "primary). Applied with the pixel-exact fit, or as a plain move when the window is already exact "
                      "or the fit is off or skipped. Ignored in fullscreen. Takes effect at the next start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(mcla_window_behind, false, "MCLA",
                    "Windowed run: after placing it, put the game window in the z-order directly below the window that "
                    "has the focus, without activating it (the window the owner is working in stays on top). Not done "
                    "when the game window itself got the focus, or when the foreground window is the desktop or the "
                    "shell. Ignored in fullscreen. Takes effect at the next start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla_host {

namespace {

constexpr uint32_t kMediumDpi = 96;

bool ExplicitlySet(std::string_view name) {
  return rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault;
}

// --- Placement (mcla_window_pos) and z-order (mcla_window_behind) -------------------------------------------------------

struct Placement {
  enum class Kind { kNone, kCorner, kPoint };
  Kind kind = Kind::kNone;
  bool right = false;   // kCorner
  bool bottom = false;  // kCorner
  int x = 0;            // kPoint
  int y = 0;
  std::string text;  // for the log: "bottom-right" or "100,200"
};

bool ParseInt(std::string_view text, int& value) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  if (text.empty()) {
    return false;
  }
  const char* const end = text.data() + text.size();
  const auto result = std::from_chars(text.data(), end, value);
  return result.ec == std::errc() && result.ptr == end;
}

Placement ReadPlacement() {
  Placement placement;
  std::string text = REXCVAR_GET(mcla_window_pos);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.pop_back();
  }
  size_t first = 0;
  while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) {
    ++first;
  }
  text.erase(0, first);
  if (text.empty()) {
    return placement;
  }
  std::string name;
  for (const char c : text) {
    name.push_back(c == '_' || c == ' ' ? '-' : char(std::tolower(static_cast<unsigned char>(c))));
  }
  if (name == "center" || name == "centre") {
    return placement;  // the default
  }
  if (name == "bottom-right" || name == "bottom-left" || name == "top-right" || name == "top-left") {
    placement.kind = Placement::Kind::kCorner;
    placement.bottom = name.rfind("bottom", 0) == 0;
    placement.right = name.size() >= 5 && name.compare(name.size() - 5, 5, "right") == 0;
    placement.text = name;
    return placement;
  }
  const size_t comma = text.find(',');
  int x = 0;
  int y = 0;
  if (comma != std::string::npos && ParseInt(std::string_view(text).substr(0, comma), x) &&
      ParseInt(std::string_view(text).substr(comma + 1), y)) {
    placement.kind = Placement::Kind::kPoint;
    placement.x = x;
    placement.y = y;
    placement.text = std::to_string(x) + "," + std::to_string(y);
    return placement;
  }
  REXLOG_WARN(
      "[mcla-window] --mcla_window_pos='{}' not understood (bottom-right, bottom-left, top-right, top-left or X,Y): the "
      "window is placed as without it",
      text);
  return placement;
}

std::string Utf8FromWide(const wchar_t* wide, int length) {
  if (!wide || length <= 0) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string utf8(size_t(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, length, utf8.data(), size, nullptr, nullptr);
  return utf8;
}

// The window's caption, cut to `max_chars` UTF-16 units (a surrogate pair is not split), control characters as spaces.
std::string WindowTitle(HWND hwnd, int max_chars = 40) {
  wchar_t title[256] = {};
  int length = GetWindowTextW(hwnd, title, int(std::size(title)));
  if (length <= 0) {
    return {};
  }
  bool truncated = false;
  if (length > max_chars) {
    length = max_chars;
    if (title[length - 1] >= 0xD800 && title[length - 1] <= 0xDBFF) {
      --length;
    }
    truncated = true;
  }
  for (int i = 0; i < length; ++i) {
    if (title[i] < 0x20) {
      title[i] = L' ';
    }
  }
  return Utf8FromWide(title, length) + (truncated ? "..." : "");
}

// Windows 10/11 draw a 1 px frame inside a window rectangle that is ~7 px (at 100 %) larger on the left, right and bottom:
// the invisible resize border. Flush with a work-area corner means the VISIBLE frame is flush, so those insets are
// measured (DwmGetWindowAttribute DWMWA_EXTENDED_FRAME_BOUNDS, loaded at run time) and compensated; 0 when unknown.
struct FrameInsets {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

FrameInsets InvisibleFrame(HWND hwnd) {
  constexpr DWORD kExtendedFrameBounds = 9;  // DWMWA_EXTENDED_FRAME_BOUNDS
  using GetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
  FrameInsets insets;
  const HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dwm) {
    return insets;
  }
  const auto get_attribute =
      reinterpret_cast<GetWindowAttribute>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmGetWindowAttribute")));
  RECT window{};
  RECT visible{};
  if (get_attribute && GetWindowRect(hwnd, &window) &&
      SUCCEEDED(get_attribute(hwnd, kExtendedFrameBounds, &visible, sizeof(visible))) &&
      // A window that is not shown (yet) can come back empty or elsewhere: only a non-empty visible rectangle inside the
      // window rectangle counts.
      visible.right > visible.left && visible.bottom > visible.top && visible.left >= window.left &&
      visible.top >= window.top && visible.right <= window.right && visible.bottom <= window.bottom) {
    insets.left = visible.left - window.left;
    insets.top = visible.top - window.top;
    insets.right = window.right - visible.right;
    insets.bottom = window.bottom - visible.bottom;
  }
  FreeLibrary(dwm);
  return insets;
}

// The outer top-left for a window of this outer size. False when the monitor cannot be queried (corners only).
bool PlacementOrigin(HWND hwnd, const Placement& placement, int outer_width, int outer_height, int& x, int& y) {
  if (placement.kind == Placement::Kind::kPoint) {
    x = placement.x;
    y = placement.y;
    return true;
  }
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
    return false;
  }
  const RECT& work = monitor.rcWork;
  const FrameInsets frame = InvisibleFrame(hwnd);
  // A window larger than the work area keeps its top-left corner on the work area's, like the centring does.
  const int min_x = work.left - frame.left;
  const int min_y = work.top - frame.top;
  x = std::max<int>(min_x, placement.right ? work.right - outer_width + frame.right : min_x);
  y = std::max<int>(min_y, placement.bottom ? work.bottom - outer_height + frame.bottom : min_y);
  return true;
}

// A plain move (the size is not touched) to the requested placement. Returns true when the window is there.
bool MoveToPlacement(HWND hwnd, const Placement& placement) {
  if (IsZoomed(hwnd) || IsIconic(hwnd)) {
    REXLOG_INFO("[mcla-window] the window is maximised or minimised: not moved to --mcla_window_pos={}", placement.text);
    return false;
  }
  RECT current{};
  if (!GetWindowRect(hwnd, &current)) {
    return false;
  }
  int x = 0;
  int y = 0;
  if (!PlacementOrigin(hwnd, placement, current.right - current.left, current.bottom - current.top, x, y)) {
    REXLOG_WARN("[mcla-window] monitor info unavailable: not moved to --mcla_window_pos={}", placement.text);
    return false;
  }
  if (x == current.left && y == current.top) {
    return true;
  }
  if (!SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
    REXLOG_WARN("[mcla-window] SetWindowPos (move) failed ({}): the window stays at {},{}", GetLastError(), current.left,
                current.top);
    return false;
  }
  return true;
}

// The desktop, the taskbar and the shell's own surfaces: the user is not "working in" them, and going below one of them
// would leave the game above the window the user works in.
bool IsShellSurface(HWND hwnd) {
  if (hwnd == GetShellWindow() || hwnd == GetDesktopWindow()) {
    return true;
  }
  wchar_t class_name[64] = {};
  if (GetClassNameW(hwnd, class_name, int(std::size(class_name))) <= 0) {
    return false;
  }
  const std::wstring_view name(class_name);
  return name == L"Progman" || name == L"WorkerW" || name == L"Shell_TrayWnd" || name == L"Shell_SecondaryTrayWnd" ||
         name == L"Windows.UI.Core.CoreWindow";
}

// mcla_window_behind: directly below the foreground window, without activating. Returns the text for the log.
std::string PutBehindForeground(HWND hwnd) {
  HWND foreground = GetForegroundWindow();
  if (!foreground) {
    return "z-order unchanged: no foreground window";
  }
  if (foreground == hwnd) {
    return "z-order unchanged: the game window took the focus";
  }
  if (IsShellSurface(foreground)) {
    return "z-order unchanged: the foreground window is the desktop or the shell";
  }
  const std::string title = WindowTitle(foreground);
  if (!SetWindowPos(hwnd, foreground, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER)) {
    return "z-order unchanged: SetWindowPos failed (" + std::to_string(GetLastError()) + ")";
  }
  if (GetWindow(hwnd, GW_HWNDPREV) != foreground) {
    return "z-order behind '" + title + "' requested, but the window manager placed the window elsewhere";
  }
  return "z-order behind '" + title + "'";
}

enum class FitOutcome {
  kNotPlaced,  // nothing resized or positioned (already exact, off, skipped, or failed)
  kPlaced,     // resized and positioned (centred, or at the mcla_window_pos placement)
};

}  // namespace

void LogFocusState(rex::ui::Window& window, const char* when) {
  HWND hwnd = static_cast<HWND>(window.GetNativeWindowHandle());
  if (!hwnd) {
    return;
  }
  HWND foreground = GetForegroundWindow();
  DWORD foreground_process = 0;
  if (foreground) {
    GetWindowThreadProcessId(foreground, &foreground_process);
  }
  const bool is_foreground = foreground != nullptr && foreground == hwnd;
  const bool is_active = foreground != nullptr && foreground_process == GetCurrentProcessId();
  std::string detail;
  if (when && *when) {
    detail += std::string(" (") + when + ")";
  }
  if (!is_foreground) {
    detail += foreground ? ", foreground window '" + WindowTitle(foreground) + "'" : std::string(", no foreground window");
  }
  REXLOG_INFO("[mcla-window] focus: foreground={} active={}{}", is_foreground ? "yes" : "no", is_active ? "yes" : "no",
              detail);
}

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

namespace {

// The pixel-exact fit: resizes the window so its client area is the guest video mode, and positions it (centred, or where
// mcla_window_pos says). kPlaced only when it did resize and position; every early return is kNotPlaced.
FitOutcome FitToGuest(rex::ui::Window& window, HWND hwnd, const Placement& placement) {
  if (!REXCVAR_GET(mcla_window_pixel_exact)) {
    REXLOG_INFO("[mcla-window] pixel-exact window off (--mcla_window_pixel_exact=false)");
    return FitOutcome::kNotPlaced;
  }
  if (window.IsFullscreen()) {
    REXLOG_INFO("[mcla-window] fullscreen: the window is the display's, nothing to fit");
    return FitOutcome::kNotPlaced;
  }
  if (!hwnd) {
    REXLOG_WARN("[mcla-window] the SDK window has no native handle: not fitted");
    return FitOutcome::kNotPlaced;
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
      return FitOutcome::kNotPlaced;
    }
  }
  if (IsZoomed(hwnd) || IsIconic(hwnd)) {
    REXLOG_INFO("[mcla-window] the window is maximised or minimised: not fitted");
    return FitOutcome::kNotPlaced;
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
    return FitOutcome::kNotPlaced;
  }

  // The outer size for that client area at this window's DPI and style, then centred on the monitor's work area (the
  // window stays fully visible when it fits; when it is larger, its top-left corner is the work area's), or placed where
  // mcla_window_pos says.
  RECT outer{0, 0, guest_width, guest_height};
  const DWORD style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE));
  const DWORD ex_style = DWORD(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
  if (!AdjustWindowRectExForDpi(&outer, style, FALSE, ex_style, dpi)) {
    REXLOG_WARN("[mcla-window] AdjustWindowRectExForDpi failed ({}): not fitted", GetLastError());
    return FitOutcome::kNotPlaced;
  }
  const int outer_width = outer.right - outer.left;
  const int outer_height = outer.bottom - outer.top;
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  int x = 0;
  int y = 0;
  if (placement.kind != Placement::Kind::kNone && PlacementOrigin(hwnd, placement, outer_width, outer_height, x, y)) {
    // mcla_window_pos: the corner or the point asked for, instead of the centre.
  } else if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
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
    return FitOutcome::kNotPlaced;
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
  return FitOutcome::kPlaced;
}

}  // namespace

void FitWindowToGuestPixels(rex::ui::Window& window) {
  const Placement placement = ReadPlacement();
  const bool behind = REXCVAR_GET(mcla_window_behind);
  HWND hwnd = static_cast<HWND>(window.GetNativeWindowHandle());
  const FitOutcome outcome = FitToGuest(window, hwnd, placement);
  if (!hwnd) {
    return;
  }

  const bool wants_placement = placement.kind != Placement::Kind::kNone;
  if (window.IsFullscreen()) {
    if (wants_placement || behind) {
      REXLOG_INFO("[mcla-window] fullscreen: --mcla_window_pos and --mcla_window_behind ignored");
    }
  } else if (wants_placement || behind) {
    // The fit already positioned the window when it resized it; otherwise (already exact, fit off or skipped) a plain move.
    const bool placed = !wants_placement || outcome == FitOutcome::kPlaced || MoveToPlacement(hwnd, placement);
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    std::string line = "at " + std::to_string(rect.left) + "," + std::to_string(rect.top);
    if (!wants_placement) {
      line = "placed " + line + " (default)";
    } else if (placed) {
      line = "placed " + line + " (" + placement.text + ")";
    } else {
      line += " (--mcla_window_pos=" + placement.text + " not applied)";
    }
    if (behind) {
      line += ", " + PutBehindForeground(hwnd);
    }
    REXLOG_INFO("[mcla-window] {}", line);
  }

  // The H5 check: did the window take the focus from whatever the owner was doing?
  LogFocusState(window, "after placement");
}

}  // namespace mcla_host
