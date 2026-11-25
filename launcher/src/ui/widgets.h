// Custom ImGui widgets used by the settings UI: toggle switch, icon button, badge, spinner, native folder picker.
#pragma once

#include <string>

#include "imgui.h"

namespace launcher::ui {

struct Fonts;

// Win11-style on/off switch. `height` is the track height; returns true when toggled this frame.
bool ToggleSwitch(const char* str_id, bool* value, float height);

// Square-ish button drawn with an icon glyph (icon font, or `fallback_text` when the font is missing).
bool IconButton(const char* str_id, const char* icon, const char* fallback_text, const Fonts& fonts, ImVec2 size);

// Draws one icon glyph centred in `rect`.
void DrawIconCentered(ImDrawList* dl, const Fonts& fonts, const char* icon, ImVec2 min, ImVec2 max, ImU32 color);

// Draws one icon glyph at `pos` (top-left) at the icon font's native size; returns the glyph advance (0 when the icon
// font is missing).
float DrawIconAt(ImDrawList* dl, const Fonts& fonts, const char* icon, ImVec2 pos, ImU32 color);

// Pill-shaped label drawn at the current cursor and vertically centred in a text line of `line_height`; occupies
// layout space like an item (so the next chip can be placed from GetItemRectMax()).
void Badge(const char* text, ImU32 background, ImU32 foreground, float line_height);

// Rotating arc around `center`, drawn into `dl` (used on the Launch button while the game runs).
void DrawSpinner(ImDrawList* dl, ImVec2 center, float radius, float thickness, ImU32 color);

// Tooltip with a wrapped body (max ~ 36 em) and an optional dim footer. Honors the style's tooltip hover flags.
void ItemTooltip(const std::string& body, const std::string& footer = {});

// Native "pick a folder" dialog (IFileOpenDialog / FOS_PICKFOLDERS). `initial` may be empty. Returns false when the
// user cancelled or the dialog could not be created.
bool PickFolder(void* owner_hwnd, const std::wstring& title, const std::wstring& initial, std::wstring* picked);

}  // namespace launcher::ui
