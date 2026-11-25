#include "ui/util.h"

#include <algorithm>
#include <cctype>

namespace launcher::ui {

std::string ToLowerAscii(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool ContainsNoCase(std::string_view haystack, std::string_view lower_needle) {
  if (lower_needle.empty()) return true;
  if (haystack.size() < lower_needle.size()) return false;
  const auto it = std::search(haystack.begin(), haystack.end(), lower_needle.begin(), lower_needle.end(),
                              [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
  return it != haystack.end();
}

}  // namespace launcher::ui
