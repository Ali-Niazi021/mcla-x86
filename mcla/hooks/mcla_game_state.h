// mcla_game_state: what the game is doing, as far as the host cares, from the game's state machine
// (HOOKS_SPEC F1 "Then", task N0).
//
// Two files:
//   * mcla_game_state.cpp       the classifier table and the Tracker: pure C++ (no engine headers), so
//                               games/mcla/tests/mcla_game_state_test.cpp compiles and runs it alone;
//   * mcla_game_state_host.cpp  the glue: one process-wide Tracker fed by mcla_state_trace.cpp, the
//                               [mcla-mode] log, and the in_menu()/mode()/frame_tick() providers that
//                               mcla/profile/game_profile.h exposes to the heartbeat hook.
//
// How the state machine is read. Every vhsmState::Activate event is one (state, parent, ...) name chain,
// leaf first. The machine is NOT one tree for our purposes: the world's flow (GameState_*: Disengaged,
// Loading, Intro, Interacting/Cruising/Racing, Garage, Hangout) and the user interface on top of it
// (UserInterface/*: Setup, Startup/StartScreen, Loading, Invisible = the HUD only, MenuScene/PauseMenu)
// move independently, and the game only logs the child that changed (leaving the pause menu activates
// UserInterface/Invisible, never Cruising again). So the tracker keeps two layers:
//   flow     the last flow state seen (Cruising* = free roam, Racing* = racing, ...);
//   overlay  the last UI state seen; "Invisible"/Hud* = none (the HUD is not a menu).
// The screen is the overlay when there is one, else the flow.
//
// Three rules:
//   * a name nobody knows keeps the last known screen (the table is a list of the names seen in
//     docs/mcla/FLOWS.md plus the ones the code looks up, HOOKS_SPEC F; unknown children of a known
//     state fall under it, by walking up the chain);
//   * the state machine's registration walk (~600 activations in ~6 ms at boot: every state is
//     activated once to build the tree, "Cruising_Offline" among them) is not the game's state: a
//     cluster of >= 32 activations with gaps under 50 ms is discarded (both layers reset);
//   * a screen is committed only once its events have settled for 20 ms, so a reader on another
//     thread never sees the middle of a transition.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace mcla::gamestate {

enum class Screen : uint8_t {
  kUnknown = 0,  // nothing classified yet
  kFrontEnd,     // title, startup checks, setup, hangout and lobby, results, any other menu
  kLoading,      // a loading screen (Startup/Loading, GameState_Loading, Career_Loading)
  kCutscene,     // the intro, the vehicle select, a cutscene
  kGarage,       // the garage and the showroom
  kFreeRoam,     // Cruising*: the player steers (driving)
  kRacing,       // Racing*: the player steers in a race (driving)
  kPause,        // the pause menu and its tabs, the replay pause
  kMap,          // the career map (CareerNavSystemScene)
};
inline constexpr int kScreenCount = 9;

const char* ScreenName(Screen screen);
inline bool IsDriving(Screen screen) { return screen == Screen::kFreeRoam || screen == Screen::kRacing; }
// GameProfile::in_menu() convention: -1 unknown, 0 driving, 1 anything else (a menu).
inline int InMenuValue(Screen screen) {
  return screen == Screen::kUnknown ? -1 : IsDriving(screen) ? 0 : 1;
}

enum class Layer : uint8_t { kNone, kFlow, kOverlay };

// What the table says about one activation. `matched` false: no name in the chain is in the table.
// layer kNone + matched: a root or a container, "nothing to change". layer kOverlay + screen kUnknown:
// the overlay is gone (HUD only).
struct Rule {
  bool matched = false;
  Layer layer = Layer::kNone;
  Screen screen = Screen::kUnknown;
};

// chain[0] is the state activated, chain[1] its parent, chain[2] its grandparent, ... (count >= 1).
// The first name, leaf to root, that the table knows decides.
Rule Classify(const char* const* chain, size_t count);

class Tracker {
 public:
  static constexpr int64_t kSettleMs = 20;      // events quiet for this long before a screen is committed
  static constexpr int64_t kBurstGapMs = 50;    // activations closer than this are one cluster
  static constexpr int kBurstEvents = 32;       // a cluster this big is the registration walk
  static constexpr int64_t kLogWindowMs = 10000;
  static constexpr int kLogLinesPerWindow = 20;

  struct Change {
    bool changed = false;
    Screen screen = Screen::kUnknown;    // committed screen after this call
    Screen previous = Screen::kUnknown;  // before it (the same when !changed)
    bool log = false;                    // a [mcla-mode] line is due (rate limit passed)
    uint32_t suppressed = 0;             // changes not logged since the last line
    char cause[96] = "";                 // "parent/state" of the activation that moved the screen
  };

  // One activation (any thread). now_ms: a monotonic clock in milliseconds.
  void OnActivation(const char* const* chain, size_t count, int64_t now_ms);
  // Commits what the events say once they have settled; the screen is the same as before until then.
  // Any thread, cheap (one lock).
  Change Commit(int64_t now_ms);
  // The committed screen (no commit).
  Screen Current() const;

  uint64_t events() const;
  uint64_t bursts() const;     // registration walks discarded
  uint32_t last_burst_size() const;  // activations in the last walk discarded
  uint64_t unmatched() const;        // activations no table entry covered

 private:
  void DiscardBurst();  // mutex_ held

  mutable std::mutex mutex_;
  Screen flow_ = Screen::kUnknown;
  Screen overlay_ = Screen::kUnknown;
  Screen committed_ = Screen::kUnknown;
  int64_t last_event_ms_ = 0;
  int cluster_ = 0;
  bool burst_ = false;
  uint64_t events_ = 0, bursts_ = 0, unmatched_ = 0;
  uint32_t last_burst_size_ = 0;
  char cause_[96] = "";
  int64_t log_window_ms_ = -kLogWindowMs;  // so the first change opens a window
  int log_lines_ = 0;
  uint32_t log_suppressed_ = 0;
};

// ---- host glue (mcla_game_state_host.cpp) --------------------------------------------------------

// From the state hook, for every activation (a guest thread): the chain as in Classify.
void OnActivation(const char* const* chain, size_t count);
// The committed screen (any thread); logs [mcla-mode] when it moved.
Screen CurrentScreen();
// Profile providers (any thread): -1 unknown, 0 driving, 1 menu. -1 while --mcla_mode_follow=false.
int ProfileInMenu();
int ProfileMode();
// Once per engine frame (the heartbeat hook, through game_profile.h): commits the screen so a change is
// logged as [mcla-mode] even when nobody asks. The PC host has no consumer of driving / menu yet.
void FrameTick();

}  // namespace mcla::gamestate
