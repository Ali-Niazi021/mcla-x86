// mcla_power_restore.exe: undoes the CPU boost cap of mcla.exe after a crash or a kill.
//
//   mcla_power_restore.exe --wait-pid <game pid> --record <path> [--dry] [--poll-ms <n>] [--log <path>]
//   mcla_power_restore.exe --stale [--record <path>] [--dry] [--log <path>]
//
// mcla.exe (src/host/power_boost.cpp) starts this helper when it applies mcla_cpu_boost_cap, with
// CREATE_BREAKAWAY_FROM_JOB | CREATE_NO_WINDOW | DETACHED_PROCESS so that a kill of the game's job object does not take it
// along. The helper opens the game with SYNCHRONIZE and waits for it to exit; then, if the restore record still exists (the
// game deletes it on a normal restore) it puts the recorded old power plan values back, exactly as the game would
// (mcla_power::RestoreFromRecord: only a value that still equals what the game wrote, the plan re-activated only if it is
// still the active one, the record deleted). While it waits it also checks the record every --poll-ms (2 s): if the record
// is gone (restored by the game, or another game process took over) it exits, so it never lingers.
//
// --stale: no waiting: if the record exists and its process is gone, restore it now and exit (the same stale check the game
// and the launcher run at their start; for a test harness that kills the game's job, run it after the kill). The record
// defaults to %LOCALAPPDATA%\mcla_x86\power_restore.json.
//
// --dry: log what would be restored, write nothing, delete nothing. Log: %LOCALAPPDATA%\mcla_x86\power_restore.log (or
// --log), and stdout when there is a console. Exit code 0 = nothing left to do, 1 = bad arguments or unreadable record,
// 2 = the restore failed (the record is kept).
//
// Build: this file plus src/host/power_boost.cpp, with MCLA_POWER_STANDALONE=1 (the SDK-free half of power_boost.cpp).
#ifndef MCLA_POWER_STANDALONE
#error "mcla_power_restore needs MCLA_POWER_STANDALONE=1 (see the target in CMakeLists.txt)"
#endif

#include "../../src/host/power_boost.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

fs::path g_log_path;

std::string Timestamp() {
  SYSTEMTIME t{};
  GetLocalTime(&t);
  return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                     t.wMilliseconds);
}

void Emit(mcla_power::LogLevel level, const std::string& line) {
  const std::string text = std::format("[{}] [{}] [pid {}] {}\r\n", Timestamp(),
                                       level == mcla_power::LogLevel::kWarn ? "warn" : "info", GetCurrentProcessId(), line);
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);
  if (g_log_path.empty()) {
    return;
  }
  HANDLE file = CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr);
    CloseHandle(file);
  }
}

void Info(const std::string& line) { Emit(mcla_power::LogLevel::kInfo, line); }
void Warn(const std::string& line) { Emit(mcla_power::LogLevel::kWarn, line); }

// The log of an earlier life of the helper: a small file, started over when it has grown.
void PrepareLog() {
  std::error_code ec;
  fs::create_directories(g_log_path.parent_path(), ec);
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExW(g_log_path.c_str(), GetFileExInfoStandard, &data) && data.nFileSizeHigh == 0 &&
      data.nFileSizeLow > (256u << 10)) {
    DeleteFileW(g_log_path.c_str());
  }
}

struct Args {
  uint32_t pid = 0;
  fs::path record;
  fs::path log;
  bool dry = false;
  bool stale = false;
  uint32_t poll_ms = 2000;
};

bool ParseArgs(int argc, wchar_t** argv, Args& args, std::string& error) {
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    auto next = [&](std::wstring& out) {
      if (i + 1 >= argc) {
        return false;
      }
      out = argv[++i];
      return true;
    };
    std::wstring value;
    if (a == L"--dry") {
      args.dry = true;
    } else if (a == L"--stale") {
      args.stale = true;
    } else if (a == L"--wait-pid" && next(value)) {
      args.pid = uint32_t(std::wcstoul(value.c_str(), nullptr, 10));
    } else if (a == L"--record" && next(value)) {
      args.record = value;
    } else if (a == L"--log" && next(value)) {
      args.log = value;
    } else if (a == L"--poll-ms" && next(value)) {
      args.poll_ms = std::max<uint32_t>(50, uint32_t(std::wcstoul(value.c_str(), nullptr, 10)));
    } else {
      error = "unknown or incomplete argument";
      return false;
    }
  }
  if (args.stale) {
    if (args.record.empty()) {
      args.record = mcla_power::DefaultRecordPath();
    }
    return true;
  }
  if (args.pid == 0 || args.record.empty()) {
    error = "--wait-pid and --record are required";
    return false;
  }
  return true;
}

bool Exists(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  Args args;
  std::string error;
  g_log_path = mcla_power::DefaultHelperLogPath();
  if (!ParseArgs(argc, argv, args, error)) {
    Warn("usage: mcla_power_restore.exe --wait-pid <pid> --record <path> [--dry] [--poll-ms <n>] [--log <path>] | --stale "
         "[--record <path>] [--dry] [--log <path>]: " + error);
    return 1;
  }
  if (!args.log.empty()) {
    g_log_path = args.log;
  }
  PrepareLog();
  const mcla_power::LogFn log = Emit;
  if (args.stale) {
    Info(std::format("[mcla-power] helper: stale check of record {}{}", args.record.string(),
                     args.dry ? " (DRY RUN: nothing will be written or deleted)" : ""));
    if (!mcla_power::RestoreStaleRecord(args.record, log, args.dry)) {
      Info("[mcla-power] helper: no stale record restored");
    }
    return 0;
  }
  BOOL in_job = FALSE;
  IsProcessInJob(GetCurrentProcess(), nullptr, &in_job);
  Info(std::format("[mcla-power] helper started: waiting for pid {}, record {}, in a job object: {}{}", args.pid,
                   args.record.string(), in_job ? "yes (a kill of the whole job also kills this helper)" : "no",
                   args.dry ? " (DRY RUN: nothing will be written or deleted)" : ""));

  if (!Exists(args.record)) {
    Info("[mcla-power] helper: no record: nothing to do");
    return 0;
  }
  mcla_power::Record record;
  if (!mcla_power::ReadRecord(args.record, record, error)) {
    Warn("[mcla-power] WARNING: helper: the record is unreadable (" + error + "): not touching anything");
    return 1;
  }
  if (record.pid != args.pid) {
    Info(std::format("[mcla-power] helper: the record belongs to pid {}, not {}: leaving it to that process's helper",
                     record.pid, args.pid));
    return 0;
  }

  // The record is "ours" while the same cap of the same game instance (pid, creation time, when written) is in place: a
  // later cap of the game (the cvar changed from 1 to 2) has its own helper.
  auto still_ours = [&]() {
    mcla_power::Record now;
    std::string ignored;
    return mcla_power::ReadRecord(args.record, now, ignored) && now.pid == record.pid &&
           now.pid_created == record.pid_created && now.written == record.written;
  };

  HANDLE process = nullptr;
  if (mcla_power::ProcessIsAlive(record.pid, record.pid_created)) {
    process = OpenProcess(SYNCHRONIZE, FALSE, record.pid);
  }
  if (!process) {
    Info(std::format("[mcla-power] helper: pid {} is not running (any more): restoring now", record.pid));
  } else {
    for (;;) {
      const DWORD wait = WaitForSingleObject(process, args.poll_ms);
      if (wait == WAIT_OBJECT_0) {
        Info(std::format("[mcla-power] helper: pid {} exited", record.pid));
        break;
      }
      if (wait != WAIT_TIMEOUT) {  // WAIT_FAILED: fall back to looking at the process every poll
        Warn(std::format("[mcla-power] WARNING: helper: waiting on pid {} failed (error {}): polling instead", record.pid,
                         unsigned(GetLastError())));
        Sleep(args.poll_ms);
        if (!mcla_power::ProcessIsAlive(record.pid, record.pid_created)) {
          break;
        }
      }
      if (!still_ours()) {
        Info("[mcla-power] helper: the record is gone or belongs to another process now: nothing to do, exiting");
        CloseHandle(process);
        return 0;
      }
    }
    CloseHandle(process);
  }

  if (!still_ours()) {
    Info("[mcla-power] helper: the record is gone or belongs to another process: nothing to do");
    return 0;
  }
  const bool ok = mcla_power::RestoreFromRecord(args.record, log, args.dry, std::format("helper after pid {} ended", record.pid));
  return ok ? 0 : 2;
}
