// Small text helpers of the UI layer (search filtering). UTF-8/UTF-16, path and command-line helpers live in
// core/text.h.
#pragma once

#include <string>
#include <string_view>

namespace launcher::ui {

std::string ToLowerAscii(std::string_view s);

// ASCII case-insensitive substring test (`lower_needle` already lower-case; empty matches everything).
bool ContainsNoCase(std::string_view haystack, std::string_view lower_needle);

}  // namespace launcher::ui
