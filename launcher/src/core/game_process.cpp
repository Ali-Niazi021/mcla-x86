#include "core/game_process.h"

#include <array>
#include <fstream>
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

DataRootCheck ValidateGameDataRoot(const std::filesystem::path& root) {
  DataRootCheck check;
  const std::string shown = PathToUtf8(root);
  if (root.empty()) {
    check.issues.push_back("No game data folder is set.");
    return check;
  }
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
