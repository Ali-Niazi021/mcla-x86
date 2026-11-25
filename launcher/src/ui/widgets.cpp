#include "ui/widgets.h"

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "imgui_internal.h"
#include "ui/fonts.h"
#include "ui/theme.h"

namespace launcher::ui {

bool ToggleSwitch(const char* str_id, bool* value, float height) {
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems) return false;

  const ImGuiID id = window->GetID(str_id);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float width = std::round(height * 1.85f);
  const ImRect bb(pos, ImVec2(pos.x + width, pos.y + height));
  ImGui::ItemSize(bb);
  if (!ImGui::ItemAdd(bb, id)) return false;

  bool hovered = false, held = false;
  const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
  if (pressed) {
    *value = !*value;
    ImGui::MarkItemEdited(id);
  }

  // Knob animation: ease towards the target; the storage keeps the position per switch.
  float& t = *ImGui::GetStateStorage()->GetFloatRef(id, *value ? 1.0f : 0.0f);
  const float target = *value ? 1.0f : 0.0f;
  t += (target - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 20.0f);
  if (std::fabs(target - t) < 0.002f) t = target;

  const float rounding = height * 0.5f;
  ImU32 track;
  if (*value) {
    track = Col(held ? pal::kAccentActive : hovered ? pal::kAccentHover : pal::kAccent);
  } else {
    track = Col(held ? pal::kFrameActive : hovered ? pal::kFrameHover : pal::kFrame);
  }
  ImDrawList* dl = window->DrawList;
  dl->AddRectFilled(bb.Min, bb.Max, track, rounding);
  if (!*value) dl->AddRect(bb.Min, bb.Max, Col(pal::kTextDisabled, 0.8f), rounding, 0, std::max(1.0f, height * 0.06f));

  const float pad = height * 0.2f;
  const float r = height * 0.5f - pad;
  const float cx = bb.Min.x + height * 0.5f + (width - height) * t;
  const ImU32 knob = *value ? Col(Rgb(0xFFFFFF)) : Col(pal::kTextDim);
  dl->AddCircleFilled(ImVec2(cx, bb.Min.y + height * 0.5f), r + (held ? height * 0.04f : 0.0f), knob, 24);

  ImGui::RenderNavCursor(bb, id);
  return pressed;
}

void DrawIconCentered(ImDrawList* dl, const Fonts& fonts, const char* icon, ImVec2 min, ImVec2 max, ImU32 color) {
  ImFont* f = fonts.icons;
  if (!f || !icon) return;
  const float size = f->FontSize;
  const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, icon);
  const ImVec2 p(std::round((min.x + max.x - ts.x) * 0.5f), std::round((min.y + max.y - ts.y) * 0.5f));
  dl->AddText(f, size, p, color, icon);
}

float DrawIconAt(ImDrawList* dl, const Fonts& fonts, const char* icon, ImVec2 pos, ImU32 color) {
  ImFont* f = fonts.icons;
  if (!f || !icon) return 0.0f;
  const float size = f->FontSize;
  dl->AddText(f, size, pos, color, icon);
  return f->CalcTextSizeA(size, FLT_MAX, 0.0f, icon).x;
}

bool IconButton(const char* str_id, const char* icon, const char* fallback_text, const Fonts& fonts, ImVec2 size) {
  const bool pressed = ImGui::InvisibleButton(str_id, size);
  const ImVec2 mn = ImGui::GetItemRectMin();
  const ImVec2 mx = ImGui::GetItemRectMax();
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hovered || active) {
    dl->AddRectFilled(mn, mx, Col(active ? pal::kFrameActive : pal::kFrameHover), ImGui::GetStyle().FrameRounding);
  }
  const ImU32 fg = Col(hovered ? pal::kText : pal::kTextDim);
  if (fonts.icons) {
    DrawIconCentered(dl, fonts, icon, mn, mx, fg);
  } else if (fallback_text) {
    const ImVec2 ts = ImGui::CalcTextSize(fallback_text);
    dl->AddText(ImVec2(std::round((mn.x + mx.x - ts.x) * 0.5f), std::round((mn.y + mx.y - ts.y) * 0.5f)), fg, fallback_text);
  }
  ImGui::RenderNavCursor(ImRect(mn, mx), ImGui::GetItemID());
  return pressed;
}

void Badge(const char* text, ImU32 background, ImU32 foreground, float line_height) {
  ImFont* font = ImGui::GetFont();
  const float size = ImGui::GetFontSize() * 0.72f;
  const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  const float padx = size * 0.6f;
  const float pady = size * 0.2f;
  const ImVec2 sz(ts.x + padx * 2.0f, ts.y + pady * 2.0f);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float top = pos.y + std::round((line_height - sz.y) * 0.5f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(pos.x, top), ImVec2(pos.x + sz.x, top + sz.y), background, sz.y * 0.5f);
  dl->AddText(font, size, ImVec2(pos.x + padx, top + pady), foreground, text);
  ImGui::Dummy(ImVec2(sz.x, std::max(line_height, sz.y)));
}

void DrawSpinner(ImDrawList* dl, ImVec2 center, float radius, float thickness, ImU32 color) {
  const float t = static_cast<float>(ImGui::GetTime());
  const float a0 = t * 6.0f;
  const float sweep = IM_PI * (0.9f + 0.5f * std::sin(t * 3.0f));
  dl->PathClear();
  dl->PathArcTo(center, radius - thickness * 0.5f, a0, a0 + sweep, 28);
  dl->PathStroke(color, 0, thickness);
}

void ItemTooltip(const std::string& body, const std::string& footer) {
  if (body.empty() && footer.empty()) return;
  if (ImGui::IsItemActive() || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
  if (!ImGui::BeginTooltip()) return;
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
  if (!body.empty()) ImGui::TextUnformatted(body.c_str(), body.c_str() + body.size());
  if (!footer.empty()) {
    if (!body.empty()) ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kTextDim);
    ImGui::TextUnformatted(footer.c_str(), footer.c_str() + footer.size());
    ImGui::PopStyleColor();
  }
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

bool PickFolder(void* owner_hwnd, const std::wstring& title, const std::wstring& initial, std::wstring* picked) {
  using Microsoft::WRL::ComPtr;
  ComPtr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return false;

  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
  if (!title.empty()) dialog->SetTitle(title.c_str());

  if (!initial.empty()) {
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder)))) dialog->SetFolder(folder.Get());
  }

  if (FAILED(dialog->Show(static_cast<HWND>(owner_hwnd)))) return false;  // cancelled (HRESULT_FROM_WIN32(ERROR_CANCELLED)) or failed

  ComPtr<IShellItem> result;
  if (FAILED(dialog->GetResult(&result))) return false;
  PWSTR path = nullptr;
  if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return false;
  *picked = path;
  CoTaskMemFree(path);
  return true;
}

}  // namespace launcher::ui
