// mcla_game_state_host: the glue between the state hook and the rest of the process (see
// mcla_game_state.h). Not a hook itself: always compiled, does nothing until mcla_state_trace.cpp
// feeds it.
//
// On iOS the committed screen drove the host: touch layout, the in-game options sheet, 30 fps in menus,
// the adaptive QoS. The PC has none of those consumers, so nothing is pushed anywhere; what stays is
// what the logs and the tools read:
//   * the process-wide Tracker the state hook feeds;
//   * the [mcla-mode] lines when the screen changes ("... -> free-roam": tools/run_game.py's `reached`),
//     committed once per engine frame from the heartbeat hook (FrameTick), so a change is logged even
//     when no thread asks for it;
//   * ProfileInMenu() / ProfileMode(): -1 / unknown until the first state of the title is seen, then
//     0 / driving in free roam and races, 1 / menu everywhere else. A later overlay or host policy
//     reads them.
//
// Off: --mcla_mode_follow=false (the providers report unknown, no [mcla-mode] lines).
#include <atomic>
#include <chrono>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "game_profile.h"
#include "mcla_game_state.h"
#include "mcla_guest.h"

REXCVAR_DEFINE_BOOL(mcla_mode_follow, true, "MCLA",
                    "MCLA: follow the game's state machine (profile mode / in_menu) and write the "
                    "[mcla-mode] log lines");

namespace mcla::gamestate {
namespace {

// The top-level mcGameState (HOOKS_SPEC F): object = BE32[0x828CCFBC], state = BE32[object + 4]
// (1 Boot, 2 Game, 3 Replay, 4 Movie, 5 Garage, 6 RaceEditor, 7 MC3Frontend, 8 Quit). Read only for the
// log, to check the table against the game's own coarse state in the next run. -1: not readable.
int McGameState() {
  uint32_t object = 0, state = 0;
  if (!mcla::guest::U32(0x828CCFBC, &object) || object == 0 || !mcla::guest::U32(object + 4, &state)) {
    return -1;
  }
  return state < 64 ? int(state) : -1;
}

Tracker& GameTracker() {
  static Tracker tracker;
  return tracker;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

Screen CommitNow() {
  Tracker& tracker = GameTracker();
  const Tracker::Change change = tracker.Commit(NowMs());
  // A discarded registration walk is said once (a second one would be news: a level re-registering).
  static std::atomic<uint64_t> reported_bursts{0};
  const uint64_t bursts = tracker.bursts();
  if (bursts != 0 && reported_bursts.exchange(bursts) != bursts) {
    REXLOG_INFO("[mcla-mode] state machine registration walk #{} ignored ({}+ activations in one burst)",
                bursts, tracker.last_burst_size());
  }
  if (change.changed && change.log) {
    if (change.suppressed != 0) {
      REXLOG_INFO("[mcla-mode] {} -> {} ({}) via {} (mcGameState {}) (+{} changes not logged)",
                  ScreenName(change.previous), ScreenName(change.screen),
                  IsDriving(change.screen) ? "driving" : "menu", change.cause, McGameState(),
                  change.suppressed);
    } else {
      REXLOG_INFO("[mcla-mode] {} -> {} ({}) via {} (mcGameState {})", ScreenName(change.previous),
                  ScreenName(change.screen), IsDriving(change.screen) ? "driving" : "menu",
                  change.cause, McGameState());
    }
  }
  return change.screen;
}

}  // namespace

void OnActivation(const char* const* chain, size_t count) {
  GameTracker().OnActivation(chain, count, NowMs());
}

Screen CurrentScreen() { return CommitNow(); }

int ProfileInMenu() {
  if (!REXCVAR_GET(mcla_mode_follow)) return -1;
  return InMenuValue(CommitNow());
}

int ProfileMode() {
  const int in_menu = ProfileInMenu();
  return in_menu < 0 ? nfsc::profile::kModeUnknown
                     : in_menu == 0 ? nfsc::profile::kModeDriving : nfsc::profile::kModeMenu;
}

void FrameTick() {
  if (REXCVAR_GET(mcla_mode_follow)) {
    CommitNow();
  }
}

}  // namespace mcla::gamestate
