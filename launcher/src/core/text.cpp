#include "core/text.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace launcher {

std::wstring Utf8ToWide(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

std::string WideToUtf8(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::filesystem::path PathFromUtf8(std::string_view utf8) { return std::filesystem::path(Utf8ToWide(utf8)); }

std::string PathToUtf8(const std::filesystem::path& path) { return WideToUtf8(path.native()); }

std::wstring QuoteArg(std::wstring_view arg, bool always) {
  const bool needs_quotes = always || arg.empty() || arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
  if (!needs_quotes) return std::wstring(arg);

  std::wstring out;
  out.reserve(arg.size() + 2);
  out.push_back(L'"');
  size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');  // the backslashes in front of a quote are doubled, the quote escaped
    } else {
      out.append(backslashes, L'\\');
    }
    backslashes = 0;
    out.push_back(c);
  }
  out.append(backslashes * 2, L'\\');  // in front of the closing quote
  out.push_back(L'"');
  return out;
}

std::wstring FormatCommandLine(const std::filesystem::path& exe, const std::vector<std::wstring>& args) {
  std::wstring line;
  // The program is always quoted, even without a space in its path, so a command line reads the same everywhere.
  line.push_back(L'"');
  line += exe.native();
  line.push_back(L'"');
  for (const std::wstring& arg : args) {
    line.push_back(L' ');
    line += QuoteArg(arg);
  }
  return line;
}

}  // namespace launcher
