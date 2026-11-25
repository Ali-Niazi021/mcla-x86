// Font loading for the launcher: Segoe UI (body / semibold headings), Consolas (command line) and the Segoe icon font,
// all rasterised at `scale` (the DPI scale) so text stays crisp at 150 % and above.
#pragma once

#include "imgui.h"

namespace launcher::ui {

struct Fonts {
  ImFont* body = nullptr;
  ImFont* caption = nullptr;  // small print (named so: <rpcndr.h> #defines "small")
  ImFont* heading = nullptr;
  ImFont* title = nullptr;
  ImFont* mono = nullptr;
  ImFont* icons = nullptr;  // may be null when no Segoe icon font is installed: callers must tolerate that
};

// Clears and rebuilds the atlas. Call outside a frame; the renderer backend must re-create its font texture afterwards
// (ImGui_ImplDX11_InvalidateDeviceObjects() before, NewFrame() re-creates it).
Fonts BuildFonts(ImGuiIO& io, float scale);

}  // namespace launcher::ui
