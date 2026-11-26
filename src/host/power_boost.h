// mcla-x86 host: cap the CPU's boost in the active Windows power plan while the game runs (opt-in, off by default).
//
// Why: on an APU laptop (Ryzen AI 9 365 + Radeon 880M) CPU and GPU share one package power limit (~45 W measured). A
// within-run test showed that foreign CPU load drops the GPU clock from 1913 to 1374 MHz at constant package power: CPU
// boost takes the GPU's watts. Less CPU boost can leave more watts for the GPU.
//
// Cvars:
//   mcla_cpu_boost_cap (live): 0 = leave the power plan alone (and put back what this process changed),
//     1 = PERFBOOSTMODE (processor performance boost mode) = 0 (Disabled), 2 = PROCTHROTTLEMAX (maximum processor state)
//     = 99 % (never raises a lower value), both for the active plan's AC and DC values.
//   mcla_cpu_boost_cap_dry: log exactly what would be written and restored, write nothing.
//
// What is changed: the processor boost mode (or the maximum processor state) of the ACTIVE power plan, for AC and DC,
// through powrprof (PowerWriteACValueIndex / PowerWriteDCValueIndex, then PowerSetActiveScheme to apply). It affects every
// program while the game runs. Nothing else is touched.
//
// How it is undone, in three layers, all from one restore record, %LOCALAPPDATA%\mcla_x86\power_restore.json (plan GUID,
// setting GUIDs, the old and the written values, the game's pid and its creation time):
//   (a) normal: mcla_cpu_boost_cap back to 0, or StopPowerBoost() from OnShutdown, writes the old values back (only a value
//       that still equals what was written: a value the user changed meanwhile is left alone) and deletes the record;
//   (b) crash or kill: the helper mcla_power_restore.exe (tools/power_restore), started when the cap is applied, waits for
//       the game process and then does the same restore from the record if the record still exists. It is started with
//       CREATE_BREAKAWAY_FROM_JOB so that killing the game's job object does not kill it (if breakaway is refused it is
//       started inside the job and (c) covers a job kill);
//   (c) stale record: at the game's start (StartPowerBoost) and at the launcher's start, a record whose process is gone is
//       restored: mcla_power::RestoreStaleRecord. This runs even when mcla_cpu_boost_cap is 0. A launcher or a test harness
//       that has no C++ link to this file can run "mcla_power_restore.exe --stale" for the same check.
//
// The namespace mcla_power does not depend on the SDK: power_boost.cpp compiles it into the game (mcla.exe) and, with
// MCLA_POWER_STANDALONE defined, into mcla_power_restore.exe, and a launcher can do the same for its own stale check.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace mcla_power {

enum class LogLevel { kInfo, kWarn };
// Receives whole lines, already prefixed with "[mcla-power] ".
using LogFn = std::function<void(LogLevel, const std::string&)>;

inline constexpr int kModeBoostOff = 1;  // PERFBOOSTMODE = 0 (Disabled)
inline constexpr int kModeMax99 = 2;     // PROCTHROTTLEMAX = 99 %

// The restore record. Plain text (JSON), one field per line.
struct Record {
  uint32_t version = 1;
  int mode = 0;                // kModeBoostOff / kModeMax99
  uint32_t pid = 0;            // the game process that holds the cap
  uint64_t pid_created = 0;    // its creation time (FILETIME ticks): tells a reused pid from the game
  uint64_t written = 0;        // when the record was written (FILETIME ticks): tells one cap of this process from the next
  std::string scheme;          // the power plan that was changed, "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
  std::string subgroup;        // SUB_PROCESSOR
  std::string setting;         // PERFBOOSTMODE or PROCTHROTTLEMAX
  std::string setting_name;    // "PERFBOOSTMODE" / "PROCTHROTTLEMAX" (for the log)
  uint32_t ac_old = 0, ac_new = 0;  // the value found / the value written (equal = nothing was written)
  uint32_t dc_old = 0, dc_new = 0;
};

// %LOCALAPPDATA%\mcla_x86\power_restore.json and ...\power_restore.log (the helper's log).
std::filesystem::path DefaultRecordPath();
std::filesystem::path DefaultHelperLogPath();

// Record file: ReadRecord fails with a reason when the file is missing or malformed. WriteRecord replaces atomically.
bool ReadRecord(const std::filesystem::path& path, Record& record, std::string& error);
bool WriteRecord(const std::filesystem::path& path, const Record& record);
std::string FormatRecord(const Record& record);

// Creation time of a process as FILETIME ticks (0 = the process cannot be opened, i.e. it is gone).
uint64_t ProcessCreationTime(uint32_t pid);
// True when the process exists, has not exited and (if `created` != 0) has that creation time.
bool ProcessIsAlive(uint32_t pid, uint64_t created);

// Restores from the record file: for AC and DC, a value still equal to what was written is set back to the old value, a
// value already at the old value is left, any other value (the user changed it) is left alone and logged. Re-activates the
// plan only if it is still the active one, then deletes the record (unless a write failed: the record stays so that a later
// start retries). Returns true when a record was found and the restore went through (no failed read or write), false when
// there is no record or it could not be restored. dry: reads and logs, writes and deletes nothing. `reason` ends up in the
// log line.
bool RestoreFromRecord(const std::filesystem::path& path, const LogFn& log, bool dry = false, const std::string& reason = {});

// If a record exists and its process is gone (or the pid was reused): RestoreFromRecord and log
// "[mcla-power] restored a stale boost cap left by pid <n>". A record of a live process is left alone. Returns true when a
// stale record was found. dry: only logs. Call it at every program start that may share the machine (the game, the
// launcher); it needs no cvar.
bool RestoreStaleRecord(const std::filesystem::path& path, const LogFn& log, bool dry);

struct ApplyResult {
  bool applied = false;     // the cap is in place (or, with dry, would be): `record` describes it
  bool dry = false;
  Record record;
  uint32_t helper_pid = 0;  // 0 = no helper
};

// Applies the cap for `mode` (1 or 2) to the active plan: reads the old values, writes the record, starts the helper
// (helper_exe, empty = none), writes the values, activates the plan, reads back and logs
// "[mcla-power] boost cap <mode>: scheme <guid> PERFBOOSTMODE AC <old>-><new> DC <old>-><new> (restore record <path>,
// helper pid <n>)". Nothing is written when both values already are what the cap wants. dry: logs what it would do.
// A refused write (ERROR_ACCESS_DENIED) is logged as a warning and undone: the result has applied == false.
ApplyResult ApplyCap(int mode, bool dry, const std::filesystem::path& record_path, const std::filesystem::path& helper_exe,
                     const LogFn& log);

// dry: logs what restoring `record` would do later (the dry counterpart of RestoreFromRecord for a cap that was only
// simulated).
void LogDryRestore(const Record& record, const LogFn& log, const std::string& reason);

}  // namespace mcla_power

namespace mcla_host {

// Applies mcla_cpu_boost_cap now and on every later change of the cvar; first undoes a stale cap left by a dead process
// (also when the cvar is 0). Call once (OnPostSetup).
void StartPowerBoost();
// Puts the old power plan values back (OnShutdown).
void StopPowerBoost();

}  // namespace mcla_host
