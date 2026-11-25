// Finding, validating and starting the game.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace launcher {

// The game data the port needs (docs/PLAN.md section 5): default.xex of exactly this size and the four archives.
inline constexpr std::uint64_t kDefaultXexSize = 9252864;

// The directory of the running launcher executable.
std::filesystem::path LauncherDir();

// mcla.exe next to the launcher: its path, or an empty path when it is not there.
std::filesystem::path FindGameExe();
std::filesystem::path FindGameExeIn(const std::filesystem::path& dir);

struct DataRootCheck {
  bool ok = false;
  std::vector<std::string> issues;  // UTF-8, human readable, one per problem; empty when ok
};

// Is `root` the folder with the extracted game files: default.xex (9,252,864 bytes) and xarchive_{audio,audlo,cache,
// music}.rpf present. An empty path is "no folder chosen".
DataRootCheck ValidateGameDataRoot(const std::filesystem::path& root);

// One game process started by the launcher.
class GameProcess {
 public:
  GameProcess() = default;
  ~GameProcess();
  GameProcess(const GameProcess&) = delete;
  GameProcess& operator=(const GameProcess&) = delete;

  // CreateProcessW: `"<exe>" <args...>` (args raw, quoted here), working directory `working_dir` (the game's own folder
  // when empty). False and *error (system message included) when it did not start, or when a game started by this
  // object is still running.
  bool Launch(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
              const std::filesystem::path& working_dir, std::string* error);

  // WaitForSingleObject(process, 0): true while the launched process has not exited. When it has, the exit code is
  // kept and the handle released.
  bool IsRunning();

  std::uint32_t pid() const { return pid_; }                           // 0 before the first Launch
  std::optional<std::uint32_t> exit_code() const { return exit_code_; }  // set once the process has exited

 private:
  void* process_ = nullptr;  // HANDLE
  std::uint32_t pid_ = 0;
  std::optional<std::uint32_t> exit_code_;
};

// Writes a .bat that starts the game with the exact command line and working directory Launch would use (for
// debugging). False and *error when a value cannot be put into a batch file (a double quote or a line break in an
// argument) or the file cannot be written.
bool WriteLaunchBat(const std::filesystem::path& bat, const std::filesystem::path& exe,
                    const std::vector<std::wstring>& args, const std::filesystem::path& working_dir,
                    std::string* error);

}  // namespace launcher
