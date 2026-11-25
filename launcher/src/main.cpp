// Midnight Club: Los Angeles - x86 launcher. Win32 window + Direct3D 11 swap chain + Dear ImGui; the UI itself lives in
// ui/app.cpp and is generated from the settings schema (launcher/schema/settings.json).
//
// Command line (mostly for testing):
//   --schema <file>      settings schema to load instead of settings.json next to the executable
//   --settings <file>    settings file to use instead of %LOCALAPPDATA%\mcla_x86\launcher.json
//   --category <id>      open this category first
//   --advanced           start with the advanced settings shown
//   --search <text>      start with this search text

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "ui/app.h"
#include "ui/theme.h"

// Defined in imgui_impl_win32.cpp; the header keeps the declaration under #if 0.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr wchar_t kWindowClass[] = L"MclaX86Launcher";
constexpr wchar_t kWindowTitle[] = L"Midnight Club: Los Angeles \u2014 x86 Launcher";
constexpr float kDefaultWidth = 1220.0f;   // logical pixels (96 dpi)
constexpr float kDefaultHeight = 820.0f;
constexpr float kMinWidth = 1000.0f;
constexpr float kMinHeight = 620.0f;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap_chain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_occluded = false;
bool g_device_lost = false;

HWND g_hwnd = nullptr;
std::unique_ptr<launcher::ui::LauncherApp> g_app;
float g_pending_scale = 0.0f;
bool g_active = true;
bool g_in_size_move = false;
bool g_in_frame = false;

void CreateRenderTarget() {
  ID3D11Texture2D* back_buffer = nullptr;
  if (SUCCEEDED(g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) && back_buffer) {
    g_device->CreateRenderTargetView(back_buffer, nullptr, &g_rtv);
    back_buffer->Release();
  }
}

void CleanupRenderTarget() {
  if (g_rtv) {
    g_rtv->Release();
    g_rtv = nullptr;
  }
}

bool CreateDevice(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC sd = {};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                             &g_swap_chain, &g_device, &got, &g_context);
  if (FAILED(hr)) {  // no usable GPU / driver: the software rasteriser is plenty for a settings window
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap_chain,
                                       &g_device, &got, &g_context);
  }
  if (FAILED(hr)) return false;

  // No Alt+Enter fullscreen toggle.
  IDXGIDevice* dxgi_device = nullptr;
  if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
      IDXGIFactory* factory = nullptr;
      if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        factory->Release();
      }
      adapter->Release();
    }
    dxgi_device->Release();
  }
  CreateRenderTarget();
  return true;
}

void CleanupDevice() {
  CleanupRenderTarget();
  if (g_swap_chain) { g_swap_chain->Release(); g_swap_chain = nullptr; }
  if (g_context) { g_context->Release(); g_context = nullptr; }
  if (g_device) { g_device->Release(); g_device = nullptr; }
}

// The GPU went away (driver reset, TDR, adapter removed): rebuild the device, the swap chain and the renderer objects.
bool RecoverDevice() {
  ImGui_ImplDX11_Shutdown();
  CleanupDevice();
  if (!CreateDevice(g_hwnd)) return false;
  if (!ImGui_ImplDX11_Init(g_device, g_context)) return false;
  g_device_lost = false;
  g_occluded = false;
  return true;
}

void RenderFrame() {
  if (g_in_frame || !g_app) return;
  g_in_frame = true;

  if (g_device_lost && !RecoverDevice()) {
    g_in_frame = false;
    MessageBoxW(g_hwnd, L"The graphics device was lost and could not be recreated.", kWindowTitle, MB_ICONERROR | MB_OK);
    DestroyWindow(g_hwnd);
    return;
  }
  if (!g_device) {
    g_in_frame = false;
    return;
  }

  if (g_pending_scale > 0.0f) {  // DPI changed: re-rasterise the fonts and re-scale the style between frames
    ImGui_ImplDX11_InvalidateDeviceObjects();
    g_app->SetScale(g_pending_scale);
    g_pending_scale = 0.0f;
  }

  // The gamepad drives the launcher only while its window is the foreground window and no game it started is running:
  // the Win32 backend polls XInput regardless of focus, so the UI additionally ignores the raw gamepad keys then.
  ImGuiIO& io = ImGui::GetIO();
  const bool gamepad_ok = GetForegroundWindow() == g_hwnd && !g_app->game_running();
  if (gamepad_ok) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;

  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  g_app->Frame(io.DisplaySize.x, io.DisplaySize.y);
  ImGui::Render();

  if (g_rtv) {
    const ImVec4 bg = launcher::ui::pal::kWindow;
    const float clear[4] = {bg.x, bg.y, bg.z, 1.0f};
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  }
  const HRESULT hr = g_swap_chain->Present(1, g_occluded ? DXGI_PRESENT_TEST : 0);
  g_occluded = (hr == DXGI_STATUS_OCCLUDED);
  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) g_device_lost = true;  // recovered at the next frame
  g_in_frame = false;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return TRUE;

  switch (msg) {
    case WM_SIZE:
      if (g_device && wparam != SIZE_MINIMIZED) {
        CleanupRenderTarget();
        const HRESULT hr = g_swap_chain->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) g_device_lost = true;
        else CreateRenderTarget();
        if (g_in_size_move) RenderFrame();  // the modal sizing loop starves the main loop: keep drawing while dragging
      }
      return 0;
    case WM_ENTERSIZEMOVE:
      g_in_size_move = true;
      return 0;
    case WM_EXITSIZEMOVE:
      g_in_size_move = false;
      return 0;
    case WM_ACTIVATEAPP:
      g_active = wparam != FALSE;
      return 0;
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      const UINT dpi = GetDpiForWindow(hwnd);
      RECT r = {0, 0, static_cast<LONG>(std::lround(kMinWidth * dpi / 96.0f)), static_cast<LONG>(std::lround(kMinHeight * dpi / 96.0f))};
      AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), dpi);
      info->ptMinTrackSize.x = r.right - r.left;
      info->ptMinTrackSize.y = r.bottom - r.top;
      return 0;
    }
    case WM_DPICHANGED: {
      g_pending_scale = static_cast<float>(HIWORD(wparam)) / 96.0f;
      const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
      SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_SYSCOMMAND:
      if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;  // Alt alone must not open the (absent) system menu
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

launcher::ui::AppOptions ParseCommandLine() {
  launcher::ui::AppOptions options;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return options;
  auto utf8 = [](const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 1 ? static_cast<size_t>(n - 1) : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
  };
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    const bool has_value = i + 1 < argc;
    if (a == L"--schema" && has_value) options.schema_path = argv[++i];
    else if (a == L"--settings" && has_value) options.settings_path = argv[++i];
    else if (a == L"--category" && has_value) options.initial_category = utf8(argv[++i]);
    else if (a == L"--search" && has_value) options.search = utf8(argv[++i]);
    else if (a == L"--advanced") options.show_advanced = true;
  }
  LocalFree(argv);
  return options;
}

void SetDarkTitleBar(HWND hwnd) {
  const BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
  const COLORREF caption = RGB(0x14, 0x17, 0x1C);  // Windows 11 only; ignored elsewhere
  DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
  const COLORREF text = RGB(0xE8, 0xEA, 0xEE);
  DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  // Per-monitor v2 DPI awareness (the manifest asks for it too; this covers a build without resources).
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // the folder picker needs COM

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
  wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
  if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wc.hbrBackground = CreateSolidBrush(RGB(0x0F, 0x11, 0x15));  // no white flash before the first frame
  wc.lpszClassName = kWindowClass;
  RegisterClassExW(&wc);

  // Size the window for the DPI of the monitor under the cursor, centred on its work area.
  POINT cursor = {};
  GetCursorPos(&cursor);
  HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(monitor, &mi);
  const float scale0 = ImGui_ImplWin32_GetDpiScaleForMonitor(monitor);
  const UINT dpi0 = static_cast<UINT>(std::lround(96.0f * scale0));
  const DWORD style = WS_OVERLAPPEDWINDOW;
  RECT frame = {0, 0, static_cast<LONG>(std::lround(kDefaultWidth * scale0)), static_cast<LONG>(std::lround(kDefaultHeight * scale0))};
  AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi0);
  int w = frame.right - frame.left, h = frame.bottom - frame.top;
  const int work_w = mi.rcWork.right - mi.rcWork.left, work_h = mi.rcWork.bottom - mi.rcWork.top;
  w = std::min(w, work_w);
  h = std::min(h, work_h);
  const int x = mi.rcWork.left + (work_w - w) / 2;
  const int y = mi.rcWork.top + (work_h - h) / 2;

  g_hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, style, x, y, w, h, nullptr, nullptr, instance, nullptr);
  if (!g_hwnd) return 1;
  SetDarkTitleBar(g_hwnd);

  if (!CreateDevice(g_hwnd)) {
    MessageBoxW(g_hwnd, L"Direct3D 11 could not be initialised (no hardware or software device).", kWindowTitle, MB_ICONERROR | MB_OK);
    DestroyWindow(g_hwnd);
    return 2;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  ImGui_ImplWin32_Init(g_hwnd);
  ImGui_ImplDX11_Init(g_device, g_context);

  g_app = std::make_unique<launcher::ui::LauncherApp>(g_hwnd, ParseCommandLine());
  g_app->SetScale(ImGui_ImplWin32_GetDpiScaleForHwnd(g_hwnd));

  ShowWindow(g_hwnd, show);
  UpdateWindow(g_hwnd);

  MSG msg = {};
  bool done = false;
  while (!done) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    if (done) break;
    if (g_app->quit_requested()) {
      DestroyWindow(g_hwnd);
      continue;
    }
    if (IsIconic(g_hwnd)) {
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
      continue;
    }
    RenderFrame();
    // While the game runs (or another window has the focus) the launcher idles at ~10 fps so it never competes for the GPU.
    if (g_occluded || !g_active || g_app->game_running()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
  }

  g_app.reset();  // writes pending settings
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  CleanupDevice();
  DestroyWindow(g_hwnd);
  UnregisterClassW(kWindowClass, instance);
  CoUninitialize();
  return static_cast<int>(msg.wParam);
}
