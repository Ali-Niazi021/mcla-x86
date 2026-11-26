// mcla-x86 host: make the game window pixel-exact.
//
// The SDK's window cvars are LOGICAL sizes: window_width=1920 on a 150 % desktop opens a 2880x1620 client area, and the
// presenter then scales the 1920x1080 guest output to it every frame (an extra pass, and a paint that is not 1:1, so
// the fused gamma paint never engages). This resizes the window, once, so that its client area is the guest video mode
// in PHYSICAL pixels, whatever the DPI scale of the monitor it opened on.
//
// Why the window is resized after the SDK created it, and not through window_width / window_height before:
//   * the same two cvars also DEFINE the guest video mode whenever video_mode_width/height are at their defaults
//     (xboxkrnl_video.cpp GetConfiguredVideoModeWidth), so a scaled-down window_width (1920 / 1.5 = 1280) would shrink
//     the guest's video mode with it, and a guest mode that happens to equal the cvar default cannot be pinned apart;
//   * the SDK converts logical to physical with a ceiling at the DPI of the PRIMARY display (SDL_GetDisplayContentScale),
//     while the window may open on another monitor.
// OnFinalizePaths is the hook: the window exists and is open, the runtime and the GPU do not yet, so no frame has been
// presented. The Window class has no resize call, so the HWND the SDK exposes (GetNativeWindowHandle) is resized with
// Win32; SDL's window procedure sees the WM_SIZE like any user resize and the presenter follows on the next message
// pump.
//
// Applies only to a windowed run (fullscreen=false) with mcla_window_pixel_exact on (the default), and not when the user
// asked on purpose for a window of another size than the guest's (window_width/window_height set explicitly and
// different from the guest video mode).
//
// Test-window placement (H5, off by default: the defaults are the behaviour above), so that a test run on the owner's own
// machine disturbs them least:
//   mcla_window_pos     "" (default: centred on the monitor's work area), bottom-right | bottom-left | top-right |
//                       top-left (flush with that corner of the work area, the taskbar stays uncovered; the invisible
//                       resize border of the Windows frame is compensated so the VISIBLE frame is flush), or X,Y (the outer
//                       window's top-left in physical pixels, as GetWindowRect reports it). Applied by the fit, and as a
//                       plain move (SWP_NOSIZE) when the window is already exact or the fit is off or skipped.
//   mcla_window_behind  false (default). true: after the placement, the window goes directly below the foreground
//                       window in the z-order, without activating it. Nothing is changed when the game window itself is
//                       the foreground window (it took the focus) or the foreground window is the desktop or the shell.
// Both are ignored in fullscreen and take effect at the next start. When either is set, FitWindowToGuestPixels logs one
// `[mcla-window] placed at x,y (<pos>), z-order behind '<foreground title>'` line (or the reason nothing changed).
// FitWindowToGuestPixels always ends with one `[mcla-window] focus: ...` line (LogFocusState).
#pragma once

#include <cstdint>

namespace rex::ui {
class Window;
}

namespace mcla_host {

// The guest video mode the way the SDK resolves it (xboxkrnl_video.cpp): video_mode_width/height, or the window cvars or
// the `resolution` preset when the video mode cvars are at their defaults. Clamped like the SDK does.
void GuestVideoModeSize(int32_t& width, int32_t& height);

// Resizes `window` (already open, UI thread) so that its client area is the guest video mode in physical pixels, and
// centres it on its monitor's work area. Logs one `[mcla-window] ...` line saying what it did or why it did nothing.
void FitWindowToGuestPixels(rex::ui::Window& window);

// Logs `[mcla-window] focus: foreground=<yes|no> active=<yes|no>[ (<when>)][, foreground window '<title>']`.
//   foreground  the game window is the foreground window (GetForegroundWindow() == its HWND): it took the focus
//   active      the foreground window belongs to this process (GetActiveWindow is per-thread, so the foreground window's
//               process id is compared with ours)
// `when` is an optional label ("post-setup", "+5 s"). Any thread; does nothing without a native window.
void LogFocusState(rex::ui::Window& window, const char* when = nullptr);

}  // namespace mcla_host
