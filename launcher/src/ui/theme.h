// Dark theme + palette for the launcher. Colours are in ImGui's (non-linear) space; the swap chain is plain UNORM.
#pragma once

#include <cstdint>

#include "imgui.h"

namespace launcher::ui {

constexpr ImVec4 Rgb(uint32_t rgb, float alpha = 1.0f) {
  return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
}

namespace pal {
inline constexpr ImVec4 kWindow = Rgb(0x0F1115);
inline constexpr ImVec4 kHeader = Rgb(0x14171C);
inline constexpr ImVec4 kSidebar = Rgb(0x13161B);
inline constexpr ImVec4 kPanel = Rgb(0x171A20);
inline constexpr ImVec4 kCard = Rgb(0x1B1F26);
inline constexpr ImVec4 kCardHover = Rgb(0x20252D);
inline constexpr ImVec4 kInset = Rgb(0x0D0F13);
inline constexpr ImVec4 kBorder = Rgb(0x2A303A);
inline constexpr ImVec4 kFrame = Rgb(0x262B34);
inline constexpr ImVec4 kFrameHover = Rgb(0x2F3541);
inline constexpr ImVec4 kFrameActive = Rgb(0x373E4C);
inline constexpr ImVec4 kText = Rgb(0xE8EAEE);
inline constexpr ImVec4 kTextDim = Rgb(0x8F97A6);
inline constexpr ImVec4 kTextDisabled = Rgb(0x5C6370);
inline constexpr ImVec4 kAccent = Rgb(0xF47B20);
inline constexpr ImVec4 kAccentHover = Rgb(0xFF9340);
inline constexpr ImVec4 kAccentActive = Rgb(0xD8650F);
inline constexpr ImVec4 kSuccess = Rgb(0x4CC38A);
inline constexpr ImVec4 kWarn = Rgb(0xE9B949);
inline constexpr ImVec4 kError = Rgb(0xF0616D);
}  // namespace pal

inline ImU32 Col(const ImVec4& c, float alpha_mul = 1.0f) {
  ImVec4 v = c;
  v.w *= alpha_mul;
  return ImGui::ColorConvertFloat4ToU32(v);
}

// Resets the style to the launcher theme and scales every metric by `scale` (DPI scale). Safe to call repeatedly.
void ApplyTheme(float scale);

}  // namespace launcher::ui
