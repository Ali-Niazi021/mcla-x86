// Finding, validating and starting the game.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

// The game data the port needs (docs/PLAN.md section 5): default.xex of exactly this size and the four archives.
inline constexpr std::uint64_t kDefaultXexSize = 9252864;

// The directory of the running launcher executable.
std::filesystem::path LauncherDir();

// mcla.exe next to the launcher: its path, or an empty path when it is not there.
std::filesystem::path FindGameExe();
std::filesystem::path FindGameExeIn(const std::filesystem::path& dir);

// A path as the game will get it: a relative one is taken from the launcher's folder (`base_dir`, empty = LauncherDir();
// the game starts in that folder too), the result is absolute and lexically normalised (preferred separators, no "." or
// ".." parts, no trailing separator except on a root such as C:\). An empty path stays empty.
std::filesystem::path ResolvePath(const std::filesystem::path& path, const std::filesystem::path& base_dir = {});

// Every character of the path is ASCII (the only paths the game can open unless it asks for the UTF-8 code page).
bool IsAsciiPath(const std::filesystem::path& path);

// Does an application manifest (XML text) ask for the UTF-8 ANSI code page: <activeCodePage>UTF-8</activeCodePage>.
bool ManifestRequestsUtf8(std::string_view manifest_xml);

// Does the game executable carry such a manifest (its RT_MANIFEST resources are read; an empty, missing or manifest-less
// file is false). Only then can the game open a path with non-ASCII characters: the SDK turns the UTF-8 cvar value into
// a path through the ANSI code page (rex_app.cpp). The answer is cached per file, size and modification time.
bool GameUsesUtf8CodePage(const std::filesystem::path& game_exe);

// "" when the game can open `resolved_path` (all ASCII, or the game asks for the UTF-8 code page: `game_exe`, empty =
// FindGameExe()); otherwise "path contains non-ASCII characters; the game cannot open it yet: <path>" (callers put the
// subject in front: "The game data folder " + ...).
std::string NonAsciiPathProblem(const std::filesystem::path& resolved_path, const std::filesystem::path& game_exe = {});

struct DataRootCheck {
  bool ok = false;
  std::vector<std::string> issues;  // UTF-8, human readable, one per problem; empty when ok
};

// Is `root` the folder with the extracted game files: default.xex (9,252,864 bytes) and xarchive_{audio,audlo,cache,
// music}.rpf present. An empty path is "no folder chosen". A relative `root` is taken from the launcher's folder
// (ResolvePath), as BuildArgs passes it. A path with non-ASCII characters is an issue ("path contains non-ASCII
// characters; the game cannot open it yet") unless the game executable (`game_exe`, empty = FindGameExe()) asks for the
// UTF-8 code page.
DataRootCheck ValidateGameDataRoot(const std::filesystem::path& root, const std::filesystem::path& game_exe = {});

// The user data folder (cvar user_data_root): empty is fine (the game picks its own); otherwise the same non-ASCII rule,
// and an existing path must be a folder.
DataRootCheck ValidateUserDataRoot(const std::filesystem::path& root, const std::filesystem::path& game_exe = {});

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
