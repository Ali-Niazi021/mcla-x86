#include "core/game_process.h"

#include <array>
#include <fstream>
#include <map>
#include <mutex>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/text.h"

namespace launcher {

namespace {

constexpr std::array<const char*, 4> kArchives = {"xarchive_audio.rpf", "xarchive_audlo.rpf", "xarchive_cache.rpf",
                                                  "xarchive_music.rpf"};

std::string WithCommas(std::uint64_t n) {
  std::string digits = std::to_string(n);
  std::string out;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i > 0 && (digits.size() - i) % 3 == 0) out.push_back(',');
    out.push_back(digits[i]);
  }
  return out;
}

std::string SystemMessage(DWORD code) {
  wchar_t* buf = nullptr;
  const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
  std::string text;
  if (n > 0 && buf) {
    std::wstring_view w(buf, n);
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ' || w.back() == L'.')) w.remove_suffix(1);
    text = WideToUtf8(w);
  }
  if (buf) LocalFree(buf);
  return text.empty() ? "error " + std::to_string(code) : text + " (error " + std::to_string(code) + ")";
}

}  // namespace

std::filesystem::path LauncherDir() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    if (n < buf.size()) {
      buf.resize(n);
      break;
    }
    buf.resize(buf.size() * 2);  // truncated: long paths
  }
  return std::filesystem::path(buf).parent_path();
}

std::filesystem::path FindGameExeIn(const std::filesystem::path& dir) {
  const std::filesystem::path exe = dir / L"mcla.exe";
  std::error_code ec;
  return std::filesystem::is_regular_file(exe, ec) ? exe : std::filesystem::path();
}

std::filesystem::path FindGameExe() { return FindGameExeIn(LauncherDir()); }

std::filesystem::path ResolvePath(const std::filesystem::path& path, const std::filesystem::path& base_dir) {
  if (path.empty()) return path;
  const std::filesystem::path base = base_dir.empty() ? LauncherDir() : base_dir;
  std::error_code ec;
  std::filesystem::path resolved;
  if (path.is_absolute()) {
    resolved = path;
  } else if (path.has_root_name() || path.has_root_directory() || base.empty()) {
    resolved = std::filesystem::absolute(path, ec);  // C:dir and \dir depend on the current drive / directory
    if (ec) resolved = path;
  } else {
    resolved = base / path;
  }
  resolved = resolved.lexically_normal();
  if (!resolved.has_filename() && resolved != resolved.root_path()) resolved = resolved.parent_path();  // C:\a\b\ -> C:\a\b
  return resolved;
}

bool IsAsciiPath(const std::filesystem::path& path) {
  for (const wchar_t c : path.native())
    if (c > 0x7F) return false;
  return true;
}

bool ManifestRequestsUtf8(std::string_view xml) {
  const auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
  size_t pos = 0;
  while ((pos = xml.find('<', pos)) != std::string_view::npos) {
    if (xml.substr(pos, 4) == "<!--") {  // a comment: skip to its end
      const size_t end = xml.find("-->", pos + 4);
      if (end == std::string_view::npos) return false;
      pos = end + 3;
      continue;
    }
    const size_t close = xml.find('>', pos);
    if (close == std::string_view::npos) return false;
    // Tag name without "<" and without a namespace prefix ("ws2:activeCodePage").
    std::string_view tag = xml.substr(pos + 1, close - pos - 1);
    const size_t name_end = tag.find_first_of(" \t\r\n/");
    std::string_view name = tag.substr(0, name_end);
    if (const size_t colon = name.find(':'); colon != std::string_view::npos) name.remove_prefix(colon + 1);
    const bool self_closing = !tag.empty() && tag.back() == '/';
    pos = close + 1;
    if (self_closing || name.size() != 14) continue;
    std::string lowered(name);
    for (char& c : lowered) c = lower(c);
    if (lowered != "activecodepage") continue;
    // The element's text up to the next tag.
    std::string_view value = xml.substr(pos, xml.find('<', pos) == std::string_view::npos ? std::string_view::npos
                                                                                           : xml.find('<', pos) - pos);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' || value.front() == '\n'))
      value.remove_prefix(1);
    if (value.size() >= 5) {
      std::string head(value.substr(0, 5));
      for (char& c : head) c = lower(c);
      if (head == "utf-8") return true;
    }
  }
  return false;
}

namespace {

BOOL CALLBACK ScanManifest(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR param) {
  bool* found = reinterpret_cast<bool*>(param);
  if (const HRSRC resource = FindResourceW(module, name, type)) {
    const DWORD size = SizeofResource(module, resource);
    if (const HGLOBAL loaded = LoadResource(module, resource)) {
      if (const void* data = LockResource(loaded)) {
        if (ManifestRequestsUtf8(std::string_view(static_cast<const char*>(data), size))) {
          *found = true;
          return FALSE;  // enough
        }
      }
    }
  }
  return TRUE;
}

struct ManifestAnswer {
  std::uintmax_t size = 0;
  std::filesystem::file_time_type time;
  bool utf8 = false;
};

}  // namespace

bool GameUsesUtf8CodePage(const std::filesystem::path& game_exe) {
  if (game_exe.empty()) return false;
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(game_exe, ec);
  if (ec) return false;
  const auto time = std::filesystem::last_write_time(game_exe, ec);
  if (ec) return false;

  static std::mutex mutex;
  static std::map<std::wstring, ManifestAnswer> cache;
  const std::wstring key = game_exe.native();
  {
    const std::lock_guard<std::mutex> lock(mutex);
    const auto it = cache.find(key);
    if (it != cache.end() && it->second.size == size && it->second.time == time) return it->second.utf8;
  }

  bool found = false;
  if (const HMODULE module = LoadLibraryExW(game_exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
    EnumResourceNamesW(module, RT_MANIFEST, ScanManifest, reinterpret_cast<LONG_PTR>(&found));  // false when it has none
    FreeLibrary(module);
  }
  const std::lock_guard<std::mutex> lock(mutex);
  cache[key] = ManifestAnswer{size, time, found};
  return found;
}

std::string NonAsciiPathProblem(const std::filesystem::path& resolved_path, const std::filesystem::path& game_exe) {
  if (IsAsciiPath(resolved_path)) return {};
  if (GameUsesUtf8CodePage(game_exe.empty() ? FindGameExe() : game_exe)) return {};
  return "path contains non-ASCII characters; the game cannot open it yet: " + PathToUtf8(resolved_path);
}

DataRootCheck ValidateGameDataRoot(const std::filesystem::path& given, const std::filesystem::path& game_exe) {
  DataRootCheck check;
  if (given.empty()) {
    check.issues.push_back("No game data folder is set.");
    return check;
  }
  const std::filesystem::path root = ResolvePath(given);
  const std::string shown = PathToUtf8(root);
  if (const std::string problem = NonAsciiPathProblem(root, game_exe); !problem.empty())
    check.issues.push_back("The game data folder " + problem);
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) {
    check.issues.push_back("The game data folder does not exist: " + shown);
    return check;
  }
  if (!std::filesystem::is_directory(root, ec)) {
    check.issues.push_back("The game data path is not a folder: " + shown);
    return check;
  }

  const std::filesystem::path xex = root / L"default.xex";
  if (!std::filesystem::is_regular_file(xex, ec)) {
    std::string issue = "default.xex is missing from " + shown;
    std::error_code ec2;
    if (std::filesystem::is_regular_file(root / L"game" / L"default.xex", ec2))
      issue += " (it is in the subfolder \"game\": choose that folder)";
    check.issues.push_back(std::move(issue));
  } else {
    const std::uintmax_t size = std::filesystem::file_size(xex, ec);
    if (ec) {
      check.issues.push_back("default.xex cannot be read: " + ec.message());
    } else if (size != kDefaultXexSize) {
      check.issues.push_back("default.xex is " + WithCommas(size) + " bytes; this port needs the " +
                             WithCommas(kDefaultXexSize) + "-byte one of the Complete Edition");
    }
  }

  for (const char* name : kArchives) {
    const std::filesystem::path file = root / Utf8ToWide(name);
    if (!std::filesystem::is_regular_file(file, ec)) {
      check.issues.push_back(std::string(name) + " is missing from " + shown);
    } else if (std::filesystem::file_size(file, ec) == 0) {
      check.issues.push_back(std::string(name) + " is empty");
    }
  }

  check.ok = check.issues.empty();
  return check;
}

DataRootCheck ValidateUserDataRoot(const std::filesystem::path& given, const std::filesystem::path& game_exe) {
  DataRootCheck check;
  if (!given.empty()) {
    const std::filesystem::path root = ResolvePath(given);
    if (const std::string problem = NonAsciiPathProblem(root, game_exe); !problem.empty())
      check.issues.push_back("The user data folder " + problem);
    std::error_code ec;
    if (std::filesystem::exists(root, ec) && !std::filesystem::is_directory(root, ec))
      check.issues.push_back("The user data path is not a folder: " + PathToUtf8(root));
  }
  check.ok = check.issues.empty();
  return check;
}

GameProcess::~GameProcess() {
  if (process_) CloseHandle(static_cast<HANDLE>(process_));  // the game keeps running
}

bool GameProcess::Launch(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
                         const std::filesystem::path& working_dir, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  if (IsRunning()) return fail("The game is already running.");

  std::error_code ec;
  if (exe.empty() || !std::filesystem::is_regular_file(exe, ec)) return fail("The game executable was not found: " + PathToUtf8(exe));
  const std::filesystem::path abs_exe = std::filesystem::absolute(exe, ec);
  const std::filesystem::path& program = ec ? exe : abs_exe;
  const std::filesystem::path cwd = working_dir.empty() ? program.parent_path() : working_dir;

  std::wstring command_line = FormatCommandLine(program, args);  // CreateProcessW may write into it
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  if (!CreateProcessW(program.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr,
                      cwd.c_str(), &startup, &info))
    return fail("The game did not start: " + SystemMessage(GetLastError()));

  CloseHandle(info.hThread);
  process_ = info.hProcess;
  pid_ = info.dwProcessId;
  exit_code_.reset();
  if (error) error->clear();
  return true;
}

bool GameProcess::IsRunning() {
  if (!process_) return false;
  const HANDLE process = static_cast<HANDLE>(process_);
  const DWORD wait = WaitForSingleObject(process, 0);
  if (wait == WAIT_TIMEOUT) return true;
  if (wait == WAIT_OBJECT_0) {
    DWORD code = 0;
    if (GetExitCodeProcess(process, &code)) exit_code_ = code;
  }
  CloseHandle(process);  // exited (or the handle is unusable): either way it is over
  process_ = nullptr;
  return false;
}

namespace {

bool IsBatSafe(wchar_t c) {
  if (c >= 0x80) return true;
  if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return true;
  switch (c) {
    case L'_': case L'.': case L':': case L'\\': case L'/': case L'+': case L'@': case L'=': case L',': case L'-':
      return true;
    default:
      return false;
  }
}

// One token of a batch file line: quoted unless it is made of characters cmd.exe leaves alone, with % doubled.
std::wstring BatToken(const std::wstring& arg) {
  bool safe = !arg.empty();
  for (const wchar_t c : arg) safe = safe && IsBatSafe(c);
  std::wstring token = QuoteArg(arg, !safe);
  std::wstring out;
  for (const wchar_t c : token) {
    out.push_back(c);
    if (c == L'%') out.push_back(L'%');
  }
  return out;
}

}  // namespace

bool WriteLaunchBat(const std::filesystem::path& bat, const std::filesystem::path& exe,
                    const std::vector<std::wstring>& args, const std::filesystem::path& working_dir,
                    std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  const auto cannot_quote = [](const std::wstring& s) { return s.find_first_of(L"\"\r\n") != std::wstring::npos; };

  std::error_code ec;
  std::filesystem::path program = std::filesystem::absolute(exe, ec);
  if (ec) program = exe;
  const std::filesystem::path cwd = working_dir.empty() ? program.parent_path() : working_dir;

  if (cannot_quote(program.native()) || cannot_quote(cwd.native())) return fail("A path holds a quote or a line break; a .bat cannot carry it.");
  for (const std::wstring& arg : args)
    if (cannot_quote(arg)) return fail("An argument holds a quote or a line break; a .bat cannot carry it: " + WideToUtf8(arg));

  std::wstring text;
  text += L"@echo off\r\n";
  text += L"rem Written by the Midnight Club: Los Angeles x86 launcher: the command line it starts the game with.\r\n";
  text += L"chcp 65001 >nul\r\n";
  text += L"cd /d " + BatToken(cwd.native()) + L"\r\n";
  text += L"start \"\" /wait " + BatToken(program.native());
  for (const std::wstring& arg : args) text += L" " + BatToken(arg);
  text += L"\r\n";
  text += L"echo mcla exited with code %errorlevel%\r\n";
  text += L"pause\r\n";

  if (bat.has_parent_path()) std::filesystem::create_directories(bat.parent_path(), ec);
  std::ofstream out(bat, std::ios::binary | std::ios::trunc);
  if (!out) return fail("Cannot write " + PathToUtf8(bat));
  const std::string bytes = WideToUtf8(text);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.flush();
  if (!out) return fail("Writing " + PathToUtf8(bat) + " failed");
  if (error) error->clear();
  return true;
}

}  // namespace launcher
