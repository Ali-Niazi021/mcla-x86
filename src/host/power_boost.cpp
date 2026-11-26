// mcla-x86 host: CPU boost cap while the game runs. See power_boost.h.
//
// The first half (namespace mcla_power) does not depend on the SDK: it is compiled into the game and, with
// MCLA_POWER_STANDALONE defined, into the helper tools/power_restore (mcla_power_restore.exe). The second half (cvars,
// logging, the live change callback) is the game only.
//
// Test builds define MCLA_POWER_FORBID_WRITES: the powrprof write exports are never even resolved, so a code path that
// would change a power setting only logs a refusal. Nothing in the tests of this file may write a power setting.
#include "power_boost.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace mcla_power {

namespace {

namespace fs = std::filesystem;

// Power setting subgroup and settings (powercfg aliases SUB_PROCESSOR, PERFBOOSTMODE, PROCTHROTTLEMAX).
constexpr const char kSubProcessor[] = "54533251-82be-4824-96c1-47b60b740d00";
constexpr const char kPerfBoostMode[] = "be337238-0d82-4146-a960-4f3749d470c7";
constexpr const char kProcThrottleMax[] = "bc5038f7-23e0-4960-96da-33abaf5935ec";

constexpr wchar_t kMutexName[] = L"Local\\mcla_x86_power_boost";
constexpr wchar_t kDataDirName[] = L"mcla_x86";
// What a MCLA_POWER_FORBID_WRITES build answers when code tries to write a power setting.
constexpr DWORD kWriteForbidden = 100005;
constexpr uint32_t kBoostCapPercent = 99;

// --- Small helpers -----------------------------------------------------------------------------------------------------

uint64_t ToU64(const FILETIME& ft) { return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

std::string GuidToString(const GUID& guid) {
  return std::format("{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", uint32_t(guid.Data1),
                     unsigned(guid.Data2), unsigned(guid.Data3), unsigned(guid.Data4[0]), unsigned(guid.Data4[1]),
                     unsigned(guid.Data4[2]), unsigned(guid.Data4[3]), unsigned(guid.Data4[4]), unsigned(guid.Data4[5]),
                     unsigned(guid.Data4[6]), unsigned(guid.Data4[7]));
}

// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (no braces) to a GUID.
bool ParseGuid(std::string_view text, GUID& guid) {
  uint8_t bytes[16];
  size_t count = 0;
  int pending = -1;
  for (const char c : text) {
    if (c == '-') {
      continue;
    }
    int digit = -1;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      return false;
    }
    if (pending < 0) {
      pending = digit;
    } else {
      if (count >= sizeof(bytes)) {
        return false;
      }
      bytes[count++] = uint8_t(pending * 16 + digit);
      pending = -1;
    }
  }
  if (count != sizeof(bytes) || pending >= 0) {
    return false;
  }
  guid.Data1 = (unsigned long)bytes[0] << 24 | (unsigned long)bytes[1] << 16 | (unsigned long)bytes[2] << 8 | bytes[3];
  guid.Data2 = uint16_t(bytes[4] << 8 | bytes[5]);
  guid.Data3 = uint16_t(bytes[6] << 8 | bytes[7]);
  std::memcpy(guid.Data4, bytes + 8, 8);
  return true;
}

bool SameGuidText(const std::string& a, const char* b) {
  GUID ga{};
  GUID gb{};
  return ParseGuid(a, ga) && ParseGuid(b, gb) && std::memcmp(&ga, &gb, sizeof(GUID)) == 0;
}

void Info(const LogFn& log, const std::string& text) {
  if (log) {
    log(LogLevel::kInfo, "[mcla-power] " + text);
  }
}

void Warn(const LogFn& log, const std::string& text) {
  if (log) {
    log(LogLevel::kWarn, "[mcla-power] WARNING: " + text);
  }
}

// One restore at a time on the machine (the game, its helper and a starting game or launcher can meet).
class MachineLock {
 public:
  MachineLock() {
    mutex_ = CreateMutexW(nullptr, FALSE, kMutexName);
    if (mutex_) {
      const DWORD wait = WaitForSingleObject(mutex_, 15000);
      held_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }
  }
  ~MachineLock() {
    if (mutex_) {
      if (held_) {
        ReleaseMutex(mutex_);
      }
      CloseHandle(mutex_);
    }
  }
  MachineLock(const MachineLock&) = delete;
  MachineLock& operator=(const MachineLock&) = delete;

 private:
  HANDLE mutex_ = nullptr;
  bool held_ = false;
};

// --- powrprof.dll, loaded at run time (the game does not link powrprof.lib) --------------------------------------------

template <typename Fn>
Fn ProcAddress(HMODULE module, const char* name) {
  return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

struct PowerApi {
  using GetActiveSchemeFn = DWORD(WINAPI*)(HKEY, GUID**);
  using SetActiveSchemeFn = DWORD(WINAPI*)(HKEY, const GUID*);
  using GetOverlayFn = DWORD(WINAPI*)(GUID*);
  using ReadFn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, PDWORD);
  using WriteFn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, DWORD);

  HMODULE module = nullptr;
  GetActiveSchemeFn get_active = nullptr;
  GetOverlayFn get_overlay = nullptr;
  ReadFn read_ac = nullptr;
  ReadFn read_dc = nullptr;
  SetActiveSchemeFn set_active = nullptr;  // never resolved in a MCLA_POWER_FORBID_WRITES build
  WriteFn write_ac = nullptr;
  WriteFn write_dc = nullptr;

  PowerApi() {
    module = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
      return;
    }
    get_active = ProcAddress<GetActiveSchemeFn>(module, "PowerGetActiveScheme");
    get_overlay = ProcAddress<GetOverlayFn>(module, "PowerGetEffectiveOverlayScheme");
    read_ac = ProcAddress<ReadFn>(module, "PowerReadACValueIndex");
    read_dc = ProcAddress<ReadFn>(module, "PowerReadDCValueIndex");
#ifndef MCLA_POWER_FORBID_WRITES
    set_active = ProcAddress<SetActiveSchemeFn>(module, "PowerSetActiveScheme");
    write_ac = ProcAddress<WriteFn>(module, "PowerWriteACValueIndex");
    write_dc = ProcAddress<WriteFn>(module, "PowerWriteDCValueIndex");
#endif
  }
  bool CanRead() const { return get_active && read_ac && read_dc; }
};

PowerApi& Api() {
  static PowerApi api;  // powrprof stays loaded until the process ends
  return api;
}

bool GetActiveScheme(GUID& scheme) {
  PowerApi& api = Api();
  GUID* ptr = nullptr;
  if (!api.get_active || api.get_active(nullptr, &ptr) != ERROR_SUCCESS || !ptr) {
    return false;
  }
  scheme = *ptr;
  LocalFree(ptr);
  return true;
}

DWORD ReadValue(bool ac, const GUID& scheme, const GUID& subgroup, const GUID& setting, uint32_t& value) {
  PowerApi& api = Api();
  const PowerApi::ReadFn read = ac ? api.read_ac : api.read_dc;
  if (!read) {
    return ERROR_CALL_NOT_IMPLEMENTED;
  }
  DWORD v = 0;
  const DWORD error = read(nullptr, &scheme, &subgroup, &setting, &v);
  if (error == ERROR_SUCCESS) {
    value = v;
  }
  return error;
}

// The only place a power setting is written.
DWORD WriteValue(bool ac, const GUID& scheme, const GUID& subgroup, const GUID& setting, uint32_t value) {
  PowerApi& api = Api();
  const PowerApi::WriteFn write = ac ? api.write_ac : api.write_dc;
  if (!write) {
#ifdef MCLA_POWER_FORBID_WRITES
    return kWriteForbidden;
#else
    return ERROR_CALL_NOT_IMPLEMENTED;
#endif
  }
  return write(nullptr, &scheme, &subgroup, &setting, value);
}

// The only place a plan is (re-)activated.
DWORD ActivateScheme(const GUID& scheme) {
  PowerApi& api = Api();
  if (!api.set_active) {
#ifdef MCLA_POWER_FORBID_WRITES
    return kWriteForbidden;
#else
    return ERROR_CALL_NOT_IMPLEMENTED;
#endif
  }
  return api.set_active(nullptr, &scheme);
}

std::string ErrorText(DWORD error) {
  if (error == kWriteForbidden) {
    return "refused: test build never writes a power setting";
  }
  return std::format("error {}", unsigned(error));
}

// --- Settings the two modes write -------------------------------------------------------------------------------------

const char* SettingGuid(int mode) { return mode == kModeBoostOff ? kPerfBoostMode : kProcThrottleMax; }
const char* SettingName(int mode) { return mode == kModeBoostOff ? "PERFBOOSTMODE" : "PROCTHROTTLEMAX"; }

// The value the cap wants for `old`: boost off = 0 (Disabled); max state = 99 %, never raising a lower one.
uint32_t TargetValue(int mode, uint32_t old_value) {
  return mode == kModeBoostOff ? 0u : std::min(old_value, kBoostCapPercent);
}

// --- Files -----------------------------------------------------------------------------------------------------------

fs::path LocalAppDataDir() {
  wchar_t buffer[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, DWORD(std::size(buffer)));
  if (n == 0 || n >= std::size(buffer)) {
    n = GetTempPathW(DWORD(std::size(buffer)), buffer);
    if (n == 0 || n >= std::size(buffer)) {
      return fs::path(L".");
    }
  }
  return fs::path(buffer);
}

bool ReadWholeFile(const fs::path& path, std::string& text) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= (1 << 16);
  if (ok) {
    text.assign(size_t(size.QuadPart), '\0');
    DWORD read = 0;
    ok = size.QuadPart == 0 || (ReadFile(file, text.data(), DWORD(text.size()), &read, nullptr) && read == text.size());
  }
  CloseHandle(file);
  return ok;
}

bool WriteWholeFile(const fs::path& path, const std::string& text) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  bool ok = WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr) && written == text.size();
  ok = FlushFileBuffers(file) && ok;
  CloseHandle(file);
  return ok;
}

// "key": "text" or "key": 123, searched as the whole quoted key so that "pid" does not find "pid_created".
bool FindToken(const std::string& text, const char* key, std::string& token, bool& quoted) {
  const std::string needle = std::string("\"") + key + "\"";
  const size_t at = text.find(needle);
  if (at == std::string::npos) {
    return false;
  }
  size_t i = at + needle.size();
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
    ++i;
  }
  if (i >= text.size() || text[i] != ':') {
    return false;
  }
  ++i;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
    ++i;
  }
  if (i >= text.size()) {
    return false;
  }
  if (text[i] == '"') {
    const size_t end = text.find('"', i + 1);
    if (end == std::string::npos) {
      return false;
    }
    token = text.substr(i + 1, end - i - 1);
    quoted = true;
    return true;
  }
  size_t end = i;
  while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
    ++end;
  }
  if (end == i) {
    return false;
  }
  token = text.substr(i, end - i);
  quoted = false;
  return true;
}

bool FindString(const std::string& text, const char* key, std::string& value) {
  bool quoted = false;
  return FindToken(text, key, value, quoted) && quoted;
}

bool FindNumber(const std::string& text, const char* key, uint64_t& value) {
  std::string token;
  bool quoted = false;
  if (!FindToken(text, key, token, quoted) || quoted || token.size() > 20) {
    return false;
  }
  uint64_t v = 0;
  for (const char c : token) {
    const uint64_t next = v * 10 + uint64_t(c - '0');
    if (next < v) {
      return false;
    }
    v = next;
  }
  value = v;
  return true;
}

// --- The restore of one value ----------------------------------------------------------------------------------------

struct SideResult {
  std::string text;       // "AC 0->2", "AC 2 (already at the old value)", ...
  bool wrote = false;
  bool failed = false;    // a read or write error that makes the record worth keeping
  bool plan_gone = false; // the plan no longer exists: nothing to restore
};

SideResult RestoreSide(const char* label, bool ac, const GUID& scheme, const GUID& subgroup, const GUID& setting,
                       uint32_t old_value, uint32_t new_value, bool dry) {
  SideResult side;
  uint32_t current = 0;
  const DWORD read_error = ReadValue(ac, scheme, subgroup, setting, current);
  if (read_error == ERROR_FILE_NOT_FOUND || read_error == ERROR_PATH_NOT_FOUND) {
    side.plan_gone = true;
    side.text = std::format("{} unreadable ({}: the plan no longer exists, nothing to restore)", label, ErrorText(read_error));
    return side;
  }
  if (read_error != ERROR_SUCCESS) {
    side.failed = true;
    side.text = std::format("{} unreadable ({})", label, ErrorText(read_error));
    return side;
  }
  if (current == old_value) {
    side.text = std::format("{} {} (already at the old value)", label, current);
  } else if (current != new_value) {
    side.text = std::format("{} {} (not the {} we wrote: changed by someone else since, left alone)", label, current,
                            new_value);
  } else if (dry) {
    side.text = std::format("{} {}->{} (DRY: would write)", label, current, old_value);
  } else {
    const DWORD write_error = WriteValue(ac, scheme, subgroup, setting, old_value);
    if (write_error != ERROR_SUCCESS) {
      side.failed = true;
      side.text = std::format("{} {} (could not write the old value {}: {})", label, current, old_value,
                              ErrorText(write_error));
    } else {
      uint32_t after = 0;
      const bool back = ReadValue(ac, scheme, subgroup, setting, after) == ERROR_SUCCESS;
      side.wrote = true;
      side.text = back ? std::format("{} {}->{}", label, current, after) : std::format("{} {}->{} (written)", label, current, old_value);
    }
  }
  return side;
}

// Starts the helper that restores after a crash or a kill. Returns its pid, 0 when it could not be started.
uint32_t StartHelper(const fs::path& helper_exe, const fs::path& record_path, uint32_t pid, const LogFn& log) {
  std::error_code ec;
  if (helper_exe.empty() || !fs::exists(helper_exe, ec)) {
    Warn(log, std::format("restore helper {} not found: a crash or a kill of the game leaves the power plan changed until "
                          "the next start of the game or launcher undoes it",
                          helper_exe.empty() ? std::string("mcla_power_restore.exe") : helper_exe.string()));
    return 0;
  }
  const std::wstring cmd = L"\"" + helper_exe.wstring() + L"\" --wait-pid " + std::to_wstring(pid) + L" --record \"" +
                           record_path.wstring() + L"\"";
  const std::wstring directory = record_path.parent_path().wstring();
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  DWORD flags = CREATE_BREAKAWAY_FROM_JOB | CREATE_NO_WINDOW | DETACHED_PROCESS;
  auto create = [&](DWORD creation_flags) {
    std::wstring command = cmd;  // CreateProcessW may modify the buffer
    return CreateProcessW(helper_exe.c_str(), command.data(), nullptr, nullptr, FALSE, creation_flags, nullptr,
                          directory.empty() ? nullptr : directory.c_str(), &startup, &info);
  };
  BOOL ok = create(flags);
  if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
    flags &= ~DWORD(CREATE_BREAKAWAY_FROM_JOB);
    ok = create(flags);
    if (ok) {
      Warn(log, "the job the game runs in refuses breakaway: the restore helper runs inside it, so killing the whole job "
                "also kills the helper (the next start of the game or launcher then undoes the cap)");
    }
  }
  if (!ok) {
    Warn(log, std::format("could not start the restore helper {} (error {}): a crash or a kill of the game leaves the power "
                          "plan changed until the next start undoes it",
                          helper_exe.string(), unsigned(GetLastError())));
    return 0;
  }
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  return info.dwProcessId;
}

std::string RecordSummary(const Record& r) {
  return std::format("scheme {} {} AC {}->{} DC {}->{}", r.scheme, r.setting_name, r.ac_old, r.ac_new, r.dc_old, r.dc_new);
}

}  // namespace

// --- Public: paths and the record ------------------------------------------------------------------------------------

fs::path DefaultRecordPath() { return LocalAppDataDir() / kDataDirName / L"power_restore.json"; }

fs::path DefaultHelperLogPath() { return LocalAppDataDir() / kDataDirName / L"power_restore.log"; }

std::string FormatRecord(const Record& r) {
  return std::format(
      "{{\n"
      "  \"version\": {},\n"
      "  \"mode\": {},\n"
      "  \"pid\": {},\n"
      "  \"pid_created\": {},\n"
      "  \"written\": {},\n"
      "  \"scheme\": \"{}\",\n"
      "  \"subgroup\": \"{}\",\n"
      "  \"setting\": \"{}\",\n"
      "  \"setting_name\": \"{}\",\n"
      "  \"ac_old\": {},\n"
      "  \"ac_new\": {},\n"
      "  \"dc_old\": {},\n"
      "  \"dc_new\": {}\n"
      "}}\n",
      r.version, r.mode, r.pid, r.pid_created, r.written, r.scheme, r.subgroup, r.setting, r.setting_name, r.ac_old, r.ac_new,
      r.dc_old, r.dc_new);
}

bool ReadRecord(const fs::path& path, Record& record, std::string& error) {
  std::string text;
  if (!ReadWholeFile(path, text)) {
    error = "cannot read the file";
    return false;
  }
  Record r;
  uint64_t number = 0;
  auto num = [&](const char* key, uint64_t max, uint64_t& out) {
    if (!FindNumber(text, key, number) || number > max) {
      error = std::string("missing or invalid \"") + key + "\"";
      return false;
    }
    out = number;
    return true;
  };
  uint64_t version = 0, mode = 0, pid = 0, created = 0, written = 0, ac_old = 0, ac_new = 0, dc_old = 0, dc_new = 0;
  if (!num("version", 1, version) || !num("mode", kModeMax99, mode) || !num("pid", 0xFFFFFFFFull, pid) ||
      !num("pid_created", ~0ull, created) || !num("ac_old", 1000, ac_old) || !num("ac_new", 1000, ac_new) ||
      !num("dc_old", 1000, dc_old) || !num("dc_new", 1000, dc_new)) {
    return false;
  }
  if (mode < kModeBoostOff || mode > kModeMax99) {
    error = "unknown mode";
    return false;
  }
  FindNumber(text, "written", written);  // optional
  if (!FindString(text, "scheme", r.scheme) || !FindString(text, "subgroup", r.subgroup) ||
      !FindString(text, "setting", r.setting) || !FindString(text, "setting_name", r.setting_name)) {
    error = "missing a GUID or name field";
    return false;
  }
  GUID guid{};
  if (!ParseGuid(r.scheme, guid) || !ParseGuid(r.subgroup, guid) || !ParseGuid(r.setting, guid)) {
    error = "malformed GUID";
    return false;
  }
  // A record may only ever name the two settings this module writes, in the processor subgroup.
  if (!SameGuidText(r.subgroup, kSubProcessor) || !SameGuidText(r.setting, SettingGuid(int(mode)))) {
    error = "the record names a setting this module does not write";
    return false;
  }
  r.version = uint32_t(version);
  r.mode = int(mode);
  r.pid = uint32_t(pid);
  r.pid_created = created;
  r.written = written;
  r.ac_old = uint32_t(ac_old);
  r.ac_new = uint32_t(ac_new);
  r.dc_old = uint32_t(dc_old);
  r.dc_new = uint32_t(dc_new);
  record = std::move(r);
  return true;
}

bool WriteRecord(const fs::path& path, const Record& record) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path temp = path.wstring() + L".tmp";
  if (!WriteWholeFile(temp, FormatRecord(record))) {
    DeleteFileW(temp.c_str());
    return false;
  }
  if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temp.c_str());
    return false;
  }
  return true;
}

// --- Public: processes ------------------------------------------------------------------------------------------------

uint64_t ProcessCreationTime(uint32_t pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) {
    return 0;
  }
  FILETIME created{}, exited{}, kernel{}, user{};
  const uint64_t time = GetProcessTimes(process, &created, &exited, &kernel, &user) ? ToU64(created) : 0;
  CloseHandle(process);
  return time;
}

bool ProcessIsAlive(uint32_t pid, uint64_t created) {
  if (pid == 0) {
    return false;
  }
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
  if (!process) {
    return false;  // no such process (or one this user cannot even query: not the game)
  }
  bool alive = true;
  FILETIME start{}, exited{}, kernel{}, user{};
  if (created != 0 && GetProcessTimes(process, &start, &exited, &kernel, &user) && ToU64(start) != created) {
    alive = false;  // the pid was reused by another process
  }
  if (alive && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
    alive = false;  // exited, but a handle somewhere keeps the process object around
  }
  CloseHandle(process);
  return alive;
}

// --- Public: restore -------------------------------------------------------------------------------------------------

void LogDryRestore(const Record& r, const LogFn& log, const std::string& reason) {
  Info(log, std::format("DRY RUN: restoring later would put {} AC {}->{} DC {}->{} back in scheme {} (each only if it still "
                        "is the value this process wrote; {}); nothing written",
                        r.setting_name, r.ac_new, r.ac_old, r.dc_new, r.dc_old, r.scheme, reason.empty() ? "later" : reason));
}

bool RestoreFromRecord(const fs::path& path, const LogFn& log, bool dry, const std::string& reason) {
  MachineLock lock;
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return false;
  }
  Record record;
  std::string error;
  if (!ReadRecord(path, record, error)) {
    const fs::path bad = path.wstring() + L".bad";
    if (dry) {
      Warn(log, std::format("DRY RUN: the restore record {} is unreadable ({}): would move it to {}", path.string(), error,
                            bad.string()));
    } else {
      DeleteFileW(bad.c_str());
      const bool moved = MoveFileExW(path.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
      Warn(log, std::format("the restore record {} is unreadable ({}): {}. The power plan was not restored; check the "
                            "processor boost setting in Windows power options",
                            path.string(), error, moved ? "moved to " + bad.string() : std::string("could not move it")));
    }
    return false;
  }
  if (!Api().CanRead()) {
    Warn(log, "powrprof.dll is not available: cannot restore the power plan from " + path.string());
    return false;
  }
  GUID scheme{}, subgroup{}, setting{};
  ParseGuid(record.scheme, scheme);
  ParseGuid(record.subgroup, subgroup);
  ParseGuid(record.setting, setting);

  const SideResult ac = RestoreSide("AC", true, scheme, subgroup, setting, record.ac_old, record.ac_new, dry);
  const SideResult dc = RestoreSide("DC", false, scheme, subgroup, setting, record.dc_old, record.dc_new, dry);
  const bool failed = ac.failed || dc.failed;

  std::string activation;
  if (!dry && (ac.wrote || dc.wrote)) {
    GUID active{};
    if (!GetActiveScheme(active) || std::memcmp(&active, &scheme, sizeof(GUID)) != 0) {
      activation = "; the plan is no longer the active one, so it was not re-activated";
    } else {
      const DWORD activate_error = ActivateScheme(scheme);
      if (activate_error != ERROR_SUCCESS) {
        activation = "; re-activating the plan failed (" + ErrorText(activate_error) + ")";
      }
    }
  }

  const std::string why = reason.empty() ? std::string() : reason + "; ";
  const std::string body = std::format("boost cap {}: scheme {} {} {} {}", record.mode, record.scheme, record.setting_name,
                                       ac.text, dc.text);
  if (dry) {
    Info(log, std::format("DRY RUN: would restore {} ({}record {}); nothing written, record kept", body, why, path.string()));
    return true;
  }
  if (failed) {
    Warn(log, std::format("could not restore {} ({}record {} kept: the next start of the game or launcher retries){}", body,
                          why, path.string(), activation));
    return false;
  }
  Info(log, std::format("restored {} ({}record {} deleted){}", body, why, path.string(), activation));
  DeleteFileW(path.c_str());
  return true;
}

bool RestoreStaleRecord(const fs::path& path, const LogFn& log, bool dry) {
  MachineLock lock;
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return false;
  }
  Record record;
  std::string error;
  if (!ReadRecord(path, record, error)) {
    RestoreFromRecord(path, log, dry, "stale record");  // logs the unreadable record and moves it away
    return false;
  }
  if (ProcessIsAlive(record.pid, record.pid_created)) {
    if (record.pid != GetCurrentProcessId()) {
      Info(log, std::format("boost cap record {} belongs to pid {}, which is still running: left alone", path.string(),
                            record.pid));
    }
    return false;
  }
  if (dry) {
    Info(log, std::format("DRY RUN: found a stale boost cap left by pid {} (no longer running)", record.pid));
    RestoreFromRecord(path, log, true, std::format("stale record of pid {}", record.pid));
    return true;
  }
  if (!RestoreFromRecord(path, log, false, std::format("stale record of pid {}", record.pid))) {
    return false;
  }
  Info(log, std::format("restored a stale boost cap left by pid {}", record.pid));
  return true;
}

// --- Public: apply ---------------------------------------------------------------------------------------------------

ApplyResult ApplyCap(int mode, bool dry, const fs::path& record_path, const fs::path& helper_exe, const LogFn& log) {
  ApplyResult result;
  result.dry = dry;
  if (mode != kModeBoostOff && mode != kModeMax99) {
    Warn(log, std::format("unknown boost cap mode {}: nothing changed", mode));
    return result;
  }
  PowerApi& api = Api();
  if (!api.CanRead()) {
    Warn(log, "powrprof.dll is not available: the CPU boost cap has no effect");
    return result;
  }
  MachineLock lock;

  // A record that is already there: another running game owns it; a dead or our own one is undone first.
  std::error_code ec;
  if (fs::exists(record_path, ec)) {
    Record held;
    std::string error;
    if (ReadRecord(record_path, held, error) && held.pid != GetCurrentProcessId() &&
        ProcessIsAlive(held.pid, held.pid_created)) {
      Info(log, std::format("boost cap {}: pid {} already holds a cap (record {}): not applying a second one", mode, held.pid,
                            record_path.string()));
      return result;
    }
    RestoreFromRecord(record_path, log, dry, "before a new cap");
  }

  GUID scheme{};
  if (!GetActiveScheme(scheme)) {
    Warn(log, "cannot read the active power plan: nothing changed");
    return result;
  }
  Record record;
  record.mode = mode;
  record.pid = GetCurrentProcessId();
  record.pid_created = ProcessCreationTime(record.pid);
  FILETIME written_at{};
  GetSystemTimeAsFileTime(&written_at);
  record.written = ToU64(written_at);
  record.scheme = GuidToString(scheme);
  record.subgroup = kSubProcessor;
  record.setting = SettingGuid(mode);
  record.setting_name = SettingName(mode);
  GUID subgroup{}, setting{};
  ParseGuid(record.subgroup, subgroup);
  ParseGuid(record.setting, setting);
  const DWORD ac_read = ReadValue(true, scheme, subgroup, setting, record.ac_old);
  const DWORD dc_read = ReadValue(false, scheme, subgroup, setting, record.dc_old);
  if (ac_read != ERROR_SUCCESS || dc_read != ERROR_SUCCESS) {
    Warn(log, std::format("cannot read {} of plan {} (AC {}, DC {}): nothing changed", record.setting_name, record.scheme,
                          ErrorText(ac_read), ErrorText(dc_read)));
    return result;
  }
  record.ac_new = TargetValue(mode, record.ac_old);
  record.dc_new = TargetValue(mode, record.dc_old);

  GUID overlay{};
  if (api.get_overlay && api.get_overlay(&overlay) == ERROR_SUCCESS) {
    const GUID zero{};
    if (std::memcmp(&overlay, &zero, sizeof(GUID)) != 0) {
      Info(log, std::format("note: Windows power mode overlay {} is active; an overlay can override the plan's processor "
                            "settings, so check the effect, not only this line",
                            GuidToString(overlay)));
    }
  }

  if (record.ac_new == record.ac_old && record.dc_new == record.dc_old) {
    Info(log, std::format("boost cap {}: {}: nothing to change (already what the cap wants)", mode, RecordSummary(record)));
    return result;
  }

  if (dry) {
    std::string compact;  // the record as one log line
    for (const char c : FormatRecord(record)) {
      if (c != '\n' && !(c == ' ' && (compact.empty() || compact.back() == ' ' || compact.back() == '{'))) {
        compact += c;
      }
    }
    Info(log, std::format("DRY RUN boost cap {}: scheme {} {} AC {}->{} DC {}->{} would be written (restore record {} would "
                          "hold {}; helper {} --wait-pid {} --record \"{}\" would be started); nothing written, no helper "
                          "started",
                          mode, record.scheme, record.setting_name, record.ac_old, record.ac_new, record.dc_old,
                          record.dc_new, record_path.string(), compact,
                          helper_exe.empty() ? std::string("mcla_power_restore.exe") : helper_exe.string(), record.pid,
                          record_path.string()));
    LogDryRestore(record, log, "when the game exits or the cvar goes back to 0");
    result.applied = true;
    result.record = record;
    return result;
  }

  // Real: record first, so that every later step has an undo.
  if (!WriteRecord(record_path, record)) {
    Warn(log, std::format("cannot write the restore record {}: the CPU boost cap was NOT applied", record_path.string()));
    return result;
  }
  result.helper_pid = StartHelper(helper_exe, record_path, record.pid, log);

  auto undo = [&](const std::string& why) {
    RestoreFromRecord(record_path, log, false, why);  // the helper sees the record vanish and exits by itself
    return result;
  };

  const struct {
    bool ac;
    uint32_t old_value, new_value;
    const char* label;
  } sides[] = {{true, record.ac_old, record.ac_new, "AC"}, {false, record.dc_old, record.dc_new, "DC"}};
  for (const auto& side : sides) {
    if (side.new_value == side.old_value) {
      continue;
    }
    const DWORD error = WriteValue(side.ac, scheme, subgroup, setting, side.new_value);
    if (error == ERROR_ACCESS_DENIED) {
      Warn(log, std::format("Windows refused to change {} of plan {} for {} (access denied): the CPU boost cap is NOT "
                            "applied and nothing else was changed",
                            record.setting_name, record.scheme, side.label));
      return undo("write refused");
    }
    if (error != ERROR_SUCCESS) {
      Warn(log, std::format("writing {} of plan {} for {} failed ({}): the CPU boost cap is NOT applied", record.setting_name,
                            record.scheme, side.label, ErrorText(error)));
      return undo("write failed");
    }
  }
  const DWORD activate_error = ActivateScheme(scheme);
  if (activate_error != ERROR_SUCCESS) {
    Warn(log, std::format("the values were written but re-activating the plan failed ({}): the cap may not be in effect",
                          ErrorText(activate_error)));
  }

  uint32_t ac_now = record.ac_new, dc_now = record.dc_new;
  const bool ac_ok = ReadValue(true, scheme, subgroup, setting, ac_now) == ERROR_SUCCESS;
  const bool dc_ok = ReadValue(false, scheme, subgroup, setting, dc_now) == ERROR_SUCCESS;
  Info(log, std::format("boost cap {}: scheme {} {} AC {}->{} DC {}->{} (restore record {}, helper pid {})", mode,
                        record.scheme, record.setting_name, record.ac_old, ac_now, record.dc_old, dc_now,
                        record_path.string(), result.helper_pid ? std::to_string(result.helper_pid) : std::string("none")));
  if (!ac_ok || !dc_ok || ac_now != record.ac_new || dc_now != record.dc_new) {
    Warn(log, std::format("read back AC {} DC {}, expected AC {} DC {}: the plan did not take the values", ac_now, dc_now,
                          record.ac_new, record.dc_new));
  }
  result.applied = true;
  result.record = record;
  return result;
}

}  // namespace mcla_power

// --- The game part: cvars, logging, the live change callback ----------------------------------------------------------

#ifndef MCLA_POWER_STANDALONE

#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(mcla_cpu_boost_cap, 0, "MCLA",
                     "CPU boost cap while the game runs (opt-in): 0 = leave the Windows power plan alone, 1 = turn the "
                     "active plan's processor boost mode off, 2 = limit its maximum processor state to 99 % (which also "
                     "disables boost on most AMD systems). AC and DC. On an APU laptop CPU and GPU share one power budget, "
                     "so less CPU boost can leave more watts for the GPU; it affects every program while the game runs. "
                     "The old values are put back when the game exits or the cvar goes back to 0, and after a crash or "
                     "a kill by the helper mcla_power_restore.exe or by the next start. Live")
    .range(0, 2);

REXCVAR_DEFINE_BOOL(mcla_cpu_boost_cap_dry, false, "MCLA",
                    "mcla_cpu_boost_cap dry run: log exactly what would be written and restored, write nothing (also for "
                    "the stale-record check). Read when the cap is applied");

namespace mcla_host {

namespace {

std::mutex g_mutex;
bool g_started = false;
int g_mode = 0;           // the mode this process holds (0 = none)
bool g_held_dry = false;  // the held mode was only simulated
mcla_power::Record g_record;
std::filesystem::path g_record_path;
std::filesystem::path g_helper_path;

void Log(mcla_power::LogLevel level, const std::string& line) {
  if (level == mcla_power::LogLevel::kWarn) {
    REXLOG_WARN("{}", line);
  } else {
    REXLOG_INFO("{}", line);
  }
}

// Next to mcla.exe.
std::filesystem::path HelperPath() {
  wchar_t buffer[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
  if (n == 0 || n >= std::size(buffer)) {
    return {};
  }
  return std::filesystem::path(buffer).parent_path() / L"mcla_power_restore.exe";
}

// With g_mutex held.
void ReleaseLocked(const std::string& reason) {
  if (g_mode == 0) {
    return;
  }
  if (g_held_dry) {
    mcla_power::LogDryRestore(g_record, Log, reason);
  } else if (!mcla_power::RestoreFromRecord(g_record_path, Log, false, reason)) {
    REXLOG_INFO("[mcla-power] nothing restored from {} ({}): the record is gone or the restore failed (see above)",
                g_record_path.string(), reason);
  }
  g_mode = 0;
  g_held_dry = false;
}

// With g_mutex held.
void ApplyLocked(int knob, bool dry) {
  if (!g_started || (knob == g_mode && knob != 0)) {
    return;  // the same cap is already held
  }
  if (g_mode != 0) {
    ReleaseLocked(std::format("mcla_cpu_boost_cap changed to {}", knob));
  }
  if (knob != 0) {
    const mcla_power::ApplyResult result =
        mcla_power::ApplyCap(knob, dry, g_record_path, g_helper_path, Log);
    if (result.applied) {
      g_mode = knob;
      g_held_dry = result.dry;
      g_record = result.record;
    }
  } else {
    REXLOG_INFO("[mcla-power] CPU boost cap off (mcla_cpu_boost_cap=0): the power plan is left alone");
  }
  rex::FlushLogging();
}

}  // namespace

void StartPowerBoost() {
  const int knob = REXCVAR_GET(mcla_cpu_boost_cap);
  const bool dry = REXCVAR_GET(mcla_cpu_boost_cap_dry);
  {
    std::lock_guard lock(g_mutex);
    if (g_started) {
      return;
    }
    g_started = true;
    g_record_path = mcla_power::DefaultRecordPath();
    g_helper_path = HelperPath();
    // A cap left by a process that crashed or was killed is undone first, whatever the cvar says.
    mcla_power::RestoreStaleRecord(g_record_path, Log, dry);
    ApplyLocked(knob, dry);
  }
  rex::cvar::RegisterChangeCallback("mcla_cpu_boost_cap", [](std::string_view, std::string_view) {
    const int changed = REXCVAR_GET(mcla_cpu_boost_cap);
    const bool changed_dry = REXCVAR_GET(mcla_cpu_boost_cap_dry);
    std::lock_guard lock(g_mutex);
    ApplyLocked(changed, changed_dry);
  });
}

void StopPowerBoost() {
  std::lock_guard lock(g_mutex);
  if (!g_started) {
    return;
  }
  ReleaseLocked("game exit");
  g_started = false;  // later callbacks do nothing
  rex::FlushLogging();
}

}  // namespace mcla_host

#endif  // MCLA_POWER_STANDALONE
