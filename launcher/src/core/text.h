// Text helpers of the launcher core: UTF-8 <-> UTF-16, UTF-8 <-> std::filesystem::path, and Windows command-line
// quoting.
//
// All strings of the schema and of the settings file are UTF-8 (JSON). Windows APIs and std::filesystem::path want
// UTF-16, and std::filesystem::path(std::string) would read a std::string as the ANSI code page, so every path that
// comes out of a setting goes through PathFromUtf8.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

std::wstring Utf8ToWide(std::string_view utf8);
std::string WideToUtf8(std::wstring_view wide);

std::filesystem::path PathFromUtf8(std::string_view utf8);
std::string PathToUtf8(const std::filesystem::path& path);

// Quote one command-line argument for CreateProcessW, by the rules CommandLineToArgvW and the MSVC CRT parse: the
// argument is wrapped in double quotes when it is empty or holds a space, tab, newline or double quote (and only
// then, unless `always`), backslashes in front of a double quote (or in front of the closing quote) are doubled, and
// an embedded double quote becomes \".
std::wstring QuoteArg(std::wstring_view arg, bool always = false);

// `"<exe>" arg1 arg2 ...`: the command line CreateProcessW is given. The program is always quoted; `args` are raw
// (unquoted) argv elements, as BuildArgs returns them.
std::wstring FormatCommandLine(const std::filesystem::path& exe, const std::vector<std::wstring>& args);

}  // namespace launcher
