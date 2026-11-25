// Segoe MDL2 Assets / Segoe Fluent Icons code points used by the launcher UI (private-use area). The glyphs are
// drawn from the system icon font loaded in fonts.cpp; every call site tolerates a missing icon font.
#pragma once

namespace launcher::ui::icons {

inline constexpr const char* kGear = "\uE713";
inline constexpr const char* kMonitor = "\uE7F4";
inline constexpr const char* kPicture = "\uE91B";
inline constexpr const char* kLightning = "\uE945";
inline constexpr const char* kGamepad = "\uE7FC";
inline constexpr const char* kKeyboard = "\uE765";
inline constexpr const char* kVolume = "\uE767";
inline constexpr const char* kFolder = "\uF12B";
inline constexpr const char* kFolderOpen = "\uED25";
inline constexpr const char* kPlay = "\uE768";
inline constexpr const char* kUndo = "\uE7A7";
inline constexpr const char* kRefresh = "\uE72C";
inline constexpr const char* kSearch = "\uE721";
inline constexpr const char* kCancel = "\uE711";
inline constexpr const char* kCopy = "\uE8C8";
inline constexpr const char* kSave = "\uE74E";
inline constexpr const char* kCheck = "\uE73E";
inline constexpr const char* kError = "\uEA39";
inline constexpr const char* kWarning = "\uE7BA";
inline constexpr const char* kInfo = "\uE946";
inline constexpr const char* kDeveloper = "\uE943";
inline constexpr const char* kSpeedo = "\uEC4A";
inline constexpr const char* kGlobe = "\uE774";
inline constexpr const char* kFlag = "\uE7C1";
inline constexpr const char* kCamera = "\uE722";

// Every glyph above; fonts.cpp bakes exactly these into the atlas (keep in sync when adding one).
inline constexpr char32_t kCodepoints[] = {
    0xE713, 0xE7F4, 0xE91B, 0xE945, 0xE7FC, 0xE765, 0xE767, 0xF12B, 0xED25, 0xE768, 0xE7A7, 0xE72C, 0xE721,
    0xE711, 0xE8C8, 0xE74E, 0xE73E, 0xEA39, 0xE7BA, 0xE946, 0xE943, 0xEC4A, 0xE774, 0xE7C1, 0xE722,
};

}  // namespace launcher::ui::icons
