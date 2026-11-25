#include "ui/fonts.h"

#include <windows.h>

#include <cstdint>
#include <string>

#include "ui/icons.h"

namespace launcher::ui {
namespace {

// Glyph ranges must outlive the atlas build, which happens lazily when the renderer backend uploads the texture.
ImVector<ImWchar> g_text_ranges;
ImVector<ImWchar> g_icon_ranges;

void BuildRanges() {
  if (g_text_ranges.Size == 0) {
    ImFontGlyphRangesBuilder b;
    b.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesDefault());  // Basic Latin + Latin-1
    static const ImWchar extra[] = {
        0x0100, 0x024F,  // Latin Extended-A/B (paths with accented user names)
        0x0370, 0x03FF,  // Greek
        0x0400, 0x052F,  // Cyrillic
        0x2010, 0x2027,  // dashes, quotes, ellipsis
        0x2030, 0x203A,  // per mille .. guillemets
        0x2190, 0x2193,  // arrows
        0,
    };
    b.AddRanges(extra);
    b.BuildRanges(&g_text_ranges);
  }
  if (g_icon_ranges.Size == 0) {
    ImFontGlyphRangesBuilder b;
    for (const char32_t cp : icons::kCodepoints) b.AddChar(static_cast<ImWchar>(cp));
    b.BuildRanges(&g_icon_ranges);
  }
}

std::string FontDir() {
  char dir[MAX_PATH] = {};
  const UINT n = GetWindowsDirectoryA(dir, MAX_PATH);
  std::string windows = n ? std::string(dir, n) : std::string("C:\\Windows");
  return windows + "\\Fonts\\";
}

bool FileExists(const std::string& path) {
  const DWORD attr = GetFileAttributesA(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// First existing file among `names` (relative to the Windows fonts dir), or an empty string.
std::string FindFont(std::initializer_list<const char*> names) {
  const std::string dir = FontDir();
  for (const char* n : names) {
    std::string p = dir + n;
    if (FileExists(p)) return p;
  }
  return {};
}

ImFont* Load(ImGuiIO& io, const std::string& path, float px, const ImWchar* ranges, bool icon = false) {
  if (path.empty()) return nullptr;
  ImFontConfig cfg;
  cfg.OversampleH = icon ? 1 : 2;
  cfg.OversampleV = 1;
  cfg.PixelSnapH = icon;
  return io.Fonts->AddFontFromFileTTF(path.c_str(), px, &cfg, ranges);
}

}  // namespace

Fonts BuildFonts(ImGuiIO& io, float scale) {
  io.Fonts->Clear();
  BuildRanges();

  const std::string regular = FindFont({"segoeui.ttf", "tahoma.ttf", "arial.ttf"});
  const std::string semibold = FindFont({"seguisb.ttf", "segoeuib.ttf", "tahomabd.ttf", "arialbd.ttf"});
  const std::string mono = FindFont({"consola.ttf", "cour.ttf"});
  const std::string icon = FindFont({"segmdl2.ttf", "SegoeIcons.ttf"});

  Fonts f;
  f.body = Load(io, regular, 16.0f * scale, g_text_ranges.Data);
  if (!f.body) {  // no system fonts at all: the embedded bitmap font keeps the UI usable
    ImFontConfig cfg;
    cfg.SizePixels = 16.0f * scale;
    f.body = io.Fonts->AddFontDefault(&cfg);
  }
  f.caption = Load(io, regular, 13.5f * scale, g_text_ranges.Data);
  f.heading = Load(io, semibold, 20.0f * scale, g_text_ranges.Data);
  f.title = Load(io, semibold, 26.0f * scale, g_text_ranges.Data);
  f.mono = Load(io, mono, 14.5f * scale, g_text_ranges.Data);
  f.icons = Load(io, icon, 17.0f * scale, g_icon_ranges.Data, true);

  if (!f.caption) f.caption = f.body;
  if (!f.heading) f.heading = f.body;
  if (!f.title) f.title = f.heading;
  if (!f.mono) f.mono = f.body;
  io.FontDefault = f.body;
  return f;
}

}  // namespace launcher::ui
