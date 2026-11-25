#include "ui/theme.h"

namespace launcher::ui {

void ApplyTheme(float scale) {
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle();  // fresh metrics every time, so repeated DPI changes do not compound

  style.WindowPadding = ImVec2(12.0f, 10.0f);
  style.FramePadding = ImVec2(10.0f, 7.0f);
  style.ItemSpacing = ImVec2(10.0f, 8.0f);
  style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
  style.CellPadding = ImVec2(8.0f, 6.0f);
  style.IndentSpacing = 18.0f;
  style.ScrollbarSize = 11.0f;
  style.GrabMinSize = 16.0f;
  style.WindowBorderSize = 0.0f;
  style.ChildBorderSize = 0.0f;
  style.PopupBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.WindowRounding = 0.0f;
  style.ChildRounding = 8.0f;
  style.FrameRounding = 6.0f;
  style.PopupRounding = 8.0f;
  style.ScrollbarRounding = 8.0f;
  style.GrabRounding = 8.0f;
  style.TabRounding = 6.0f;
  style.WindowMenuButtonPosition = ImGuiDir_None;
  style.SeparatorTextBorderSize = 1.0f;
  style.DisabledAlpha = 0.45f;
  style.ScaleAllSizes(scale);  // rounding/padding/spacing track the DPI scale together with the fonts

  // Tooltips: appear quickly with the mouse, and with the keyboard/gamepad cursor immediately.
  style.HoverDelayShort = 0.25f;
  style.HoverDelayNormal = 0.5f;
  style.HoverStationaryDelay = 0.2f;
  style.HoverFlagsForTooltipMouse = ImGuiHoveredFlags_Stationary | ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled;
  style.HoverFlagsForTooltipNav = ImGuiHoveredFlags_NoSharedDelay | ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled;

  ImVec4* c = style.Colors;
  c[ImGuiCol_Text] = pal::kText;
  c[ImGuiCol_TextDisabled] = pal::kTextDisabled;
  c[ImGuiCol_WindowBg] = pal::kWindow;
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = Rgb(0x222731, 0.99f);
  c[ImGuiCol_Border] = Rgb(0x3B4250);
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = pal::kFrame;
  c[ImGuiCol_FrameBgHovered] = pal::kFrameHover;
  c[ImGuiCol_FrameBgActive] = pal::kFrameActive;
  c[ImGuiCol_TitleBg] = pal::kHeader;
  c[ImGuiCol_TitleBgActive] = pal::kHeader;
  c[ImGuiCol_TitleBgCollapsed] = pal::kHeader;
  c[ImGuiCol_MenuBarBg] = pal::kHeader;
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = Rgb(0x3A414E);
  c[ImGuiCol_ScrollbarGrabHovered] = Rgb(0x4B5565);
  c[ImGuiCol_ScrollbarGrabActive] = Rgb(0x5E6A7E);
  c[ImGuiCol_CheckMark] = pal::kAccent;
  c[ImGuiCol_SliderGrab] = pal::kAccent;
  c[ImGuiCol_SliderGrabActive] = pal::kAccentHover;
  c[ImGuiCol_Button] = pal::kFrame;
  c[ImGuiCol_ButtonHovered] = pal::kFrameHover;
  c[ImGuiCol_ButtonActive] = pal::kFrameActive;
  c[ImGuiCol_Header] = Rgb(0xF47B20, 0.18f);
  c[ImGuiCol_HeaderHovered] = Rgb(0xFFFFFF, 0.07f);
  c[ImGuiCol_HeaderActive] = Rgb(0xF47B20, 0.28f);
  c[ImGuiCol_Separator] = pal::kBorder;
  c[ImGuiCol_SeparatorHovered] = pal::kAccent;
  c[ImGuiCol_SeparatorActive] = pal::kAccentHover;
  c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ResizeGripHovered] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ResizeGripActive] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TextSelectedBg] = Rgb(0xF47B20, 0.35f);
  c[ImGuiCol_NavCursor] = pal::kAccentHover;
  c[ImGuiCol_NavWindowingHighlight] = Rgb(0xFFFFFF, 0.7f);
  c[ImGuiCol_NavWindowingDimBg] = Rgb(0x000000, 0.55f);
  c[ImGuiCol_ModalWindowDimBg] = Rgb(0x000000, 0.6f);
  c[ImGuiCol_DragDropTarget] = pal::kAccent;
}

}  // namespace launcher::ui
