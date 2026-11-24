// Unit test of the game-state classifier (mcla/hooks/mcla_game_state.{h,cpp}); no engine, no game.
//
// Built and run by CTest (mcla/tests/CMakeLists.txt, test mcla_game_state_test). By hand:
//   clang++ -std=c++20 -O1 -Wall -Wextra -I mcla/hooks mcla/tests/mcla_game_state_test.cpp \
//       mcla/hooks/mcla_game_state.cpp -o /tmp/mcla_game_state_test && /tmp/mcla_game_state_test
//
// The sequences are the [mcla-state] transcripts of $IOS/docs/mcla/FLOWS.md (mac002 r09 and the dev001 device
// log: the registration walk, the title, the fresh-profile chain to free roam, pause and back), written
// in the order and with the gaps the logs show, plus synthetic race / garage / map / unknown-name cases.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mcla_game_state.h"

using mcla::gamestate::Screen;
using mcla::gamestate::Tracker;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_failures;                                                              \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
    }                                                                            \
  } while (0)

#define CHECK_SCREEN(tracker, now, expected)                                                         \
  do {                                                                                               \
    const Tracker::Change c_ = (tracker).Commit(now);                                                \
    ++g_checks;                                                                                      \
    if (c_.screen != (expected)) {                                                                   \
      ++g_failures;                                                                                  \
      std::printf("FAIL %s:%d: at %lld ms screen %s, expected %s\n", __FILE__, __LINE__,             \
                  (long long)(now), mcla::gamestate::ScreenName(c_.screen),                          \
                  mcla::gamestate::ScreenName(expected));                                            \
    }                                                                                                \
  } while (0)

// One activation from a path written root first ("UserInterface/MenuScene/PauseMenu"): the tracker
// wants the chain leaf first.
void Ev(Tracker& t, const std::string& path, int64_t now, bool parent_only = false) {
  std::vector<std::string> names;
  size_t from = 0;
  while (true) {
    const size_t slash = path.find('/', from);
    names.push_back(path.substr(from, slash == std::string::npos ? std::string::npos : slash - from));
    if (slash == std::string::npos) break;
    from = slash + 1;
  }
  std::vector<const char*> chain;
  for (size_t i = names.size(); i-- > 0;) {
    chain.push_back(names[i].c_str());
    if (parent_only && chain.size() == 2) break;
  }
  t.OnActivation(chain.data(), chain.size(), now);
}

// The registration walk of mac002 r09 / dev001: ~48 distinct states, ~620 activations in ~6 ms.
const char* const kRegistration[] = {
    "GameState/GameState_Disengaged", "GameState_Disengaged/GameState_Loading",
    "GameState_Hangout/Hangout_Local", "GameState_Garage/Garage", "EditingRace/EditingRaceNormal",
    "LobbyMeeting/LobbyMeeting_SingleRace", "RaceProposalState/RaceProposalNone",
    "GameState_Interacting/Cruising", "Cruising/Cruising_Offline", "Racing/Racing_Local",
    "Racing_Dynamic/Racing_RedLight", "Racing_Online/RacingOL_FromCruise", "UserInterface/Setup",
    "Startup/StartScreen", "Loading/Loading_Old", "SaveGameScene/SaveGame_DoNotInterruptMe",
    "SaveGame_DoNotInterruptMe/SaveGameIO", "SaveGameIO/SG_SaveFile", "SaveGameDialogs/SG_GeneralError",
    "SG_GeneralError/SG_GeneralError.s0", "SG_OverwriteConfirmation/SG_OverwriteConfirmation.s0",
    "SG_NoGamesFound/SG_NoGamesFound.s0", "SG_NoSignedInPlayer/SG_NSIP_Button1",
};

void RegistrationWalk(Tracker& t, int64_t start_ms, int count = 620) {
  const size_t n = sizeof(kRegistration) / sizeof(kRegistration[0]);
  for (int i = 0; i < count; ++i) {
    Ev(t, kRegistration[size_t(i) % n], start_ms + i / 100, true);  // ~100 activations per ms
  }
}

void TestClassifyTable() {
  using mcla::gamestate::Classify;
  using mcla::gamestate::Layer;
  auto rule = [](std::initializer_list<const char*> chain) {
    std::vector<const char*> v(chain);
    return Classify(v.data(), v.size());
  };
  CHECK(!rule({"NoSuchState"}).matched);
  CHECK(!rule({"?"}).matched);
  CHECK(rule({"Cruising_Offline", "Cruising", "GameState_Interacting"}).screen == Screen::kFreeRoam);
  CHECK(rule({"Cruising_Offline"}).layer == Layer::kFlow);
  CHECK(rule({"Racing_RedLight", "Racing_Dynamic"}).screen == Screen::kRacing);
  CHECK(rule({"RacingOL_FromCruise"}).screen == Screen::kRacing);
  CHECK(rule({"PM_Continue", "PauseTab", "PauseMenu", "MenuScene", "UserInterface"}).screen == Screen::kPause);
  CHECK(rule({"PM_Continue"}).layer == Layer::kOverlay);
  CHECK(rule({"PM_MapPage"}).screen == Screen::kMap);
  CHECK(rule({"CareerNavSystemScene"}).screen == Screen::kMap);
  CHECK(rule({"HPL_Hidden"}).layer == Layer::kOverlay && rule({"HPL_Hidden"}).screen == Screen::kUnknown);
  CHECK(rule({"Loading_Fresh", "Loading"}).screen == Screen::kLoading);
  CHECK(rule({"Startup_Checks"}).screen == Screen::kFrontEnd);
  CHECK(rule({"GameState_Intro"}).screen == Screen::kCutscene);
  CHECK(rule({"GameState_Loading"}).screen == Screen::kLoading);
  CHECK(rule({"Garage"}).screen == Screen::kGarage);
  CHECK(rule({"GarageShowroom"}).screen == Screen::kGarage);
  // A name the table does not know falls under its parent...
  CHECK(rule({"SomeNewMenuPage", "MenuScene"}).screen == Screen::kFrontEnd);
  CHECK(rule({"HR_Speedo", "HudRacing", "Invisible"}).layer == Layer::kOverlay);
  // ...but a container stops the walk (nothing above it counts)...
  CHECK(rule({"Weird", "GameState_Interacting", "GameState"}).layer == Layer::kNone);
  CHECK(rule({"Weird", "GameState_Interacting"}).matched);
  // ...and saves and race proposals change nothing.
  CHECK(rule({"SG_NoGamesFound.s0", "SG_NoGamesFound"}).layer == Layer::kNone);
  CHECK(rule({"RaceProposalNone", "RaceProposalState"}).layer == Layer::kNone);
  CHECK(mcla::gamestate::InMenuValue(Screen::kFreeRoam) == 0);
  CHECK(mcla::gamestate::InMenuValue(Screen::kRacing) == 0);
  CHECK(mcla::gamestate::InMenuValue(Screen::kPause) == 1);
  CHECK(mcla::gamestate::InMenuValue(Screen::kCutscene) == 1);
  CHECK(mcla::gamestate::InMenuValue(Screen::kUnknown) == -1);
}

// The fresh-profile chain of FLOWS.md ("[mcla-state] transitions"), at the harness's own timestamps,
// with full ancestor chains (what the hook reads) or parent only (the old log pairs).
void TestFreshProfileChain(bool parent_only) {
  std::printf("-- fresh profile chain (%s)\n", parent_only ? "parent/name pairs" : "full chains");
  Tracker t;
  CHECK_SCREEN(t, 0, Screen::kUnknown);  // nothing yet: unknown, the host keeps its defaults

  // Boot: the registration walk, 620 activations in 6 ms, then quiet. "Cruising_Offline" is in it.
  RegistrationWalk(t, 2600);
  CHECK_SCREEN(t, 2602, Screen::kUnknown);  // unsettled
  CHECK_SCREEN(t, 2640, Screen::kUnknown);  // settled: the walk is discarded, not "free roam"
  CHECK(t.bursts() == 1);
  CHECK(t.last_burst_size() == 620);
  CHECK_SCREEN(t, 2700, Screen::kUnknown);

  // dev001: 1 s after the walk the machine really starts (Disengaged/Loading, UI Setup), then the title.
  Ev(t, "GameState/GameState_Disengaged", 3600, parent_only);
  Ev(t, "GameState_Disengaged/GameState_Loading", 3600, parent_only);
  Ev(t, "UserInterface/Setup", 3600, parent_only);
  CHECK_SCREEN(t, 3605, Screen::kUnknown);  // not settled yet
  CHECK_SCREEN(t, 3650, Screen::kFrontEnd);
  Ev(t, "UserInterface/Startup", 4800, parent_only);
  Ev(t, "UserInterface/Startup/StartScreen", 4800, parent_only);
  CHECK_SCREEN(t, 4850, Screen::kFrontEnd);  // the title

  // Start pressed: the save check (the ALERT), then LOADING.
  Ev(t, "UserInterface/Startup/Startup_Checks", 40000, parent_only);
  CHECK_SCREEN(t, 40100, Screen::kFrontEnd);
  Ev(t, "UserInterface/Startup/Loading", 56000, parent_only);
  Ev(t, "UserInterface/Startup/Loading/Loading_Fresh", 56000, parent_only);
  CHECK_SCREEN(t, 56100, Screen::kLoading);

  // Vehicle select / the intro cutscene: HUD-only UI over GameState_Intro.
  Ev(t, "UserInterface/Invisible", 82000, parent_only);
  Ev(t, "UserInterface/Invisible/HudPlayerList", 82000, parent_only);
  Ev(t, "UserInterface/Invisible/HudPlayerList/HPL_Hidden", 82000, parent_only);
  Ev(t, "GameState/GameState_Disengaged/GameState_Intro", 82030, parent_only);
  CHECK_SCREEN(t, 82100, Screen::kCutscene);

  // FREE ROAM (autosave at 147.1 s).
  Ev(t, "GameState/GameState_Interacting", 147000, parent_only);
  Ev(t, "GameState/GameState_Interacting/Cruising", 147000, parent_only);
  Ev(t, "GameState/GameState_Interacting/Cruising/Cruising_Offline", 147000, parent_only);
  CHECK_SCREEN(t, 147100, Screen::kFreeRoam);
  CHECK(mcla::gamestate::InMenuValue(t.Current()) == 0);

  // PAUSE (Start), then B: back in roam without Cruising being activated again.
  Ev(t, "UserInterface/MenuScene", 200000, parent_only);
  Ev(t, "UserInterface/MenuScene/PauseMenu", 200000, parent_only);
  Ev(t, "UserInterface/MenuScene/PauseMenu/PauseTab", 200000, parent_only);
  Ev(t, "UserInterface/MenuScene/PauseMenu/PauseTab/PM_Continue", 200000, parent_only);
  CHECK_SCREEN(t, 200100, Screen::kPause);
  CHECK(mcla::gamestate::InMenuValue(t.Current()) == 1);
  Ev(t, "UserInterface/Invisible", 208000, parent_only);
  Ev(t, "UserInterface/Invisible/HudPlayerList", 208000, parent_only);
  Ev(t, "UserInterface/Invisible/HudPlayerList/HPL_Hidden", 208000, parent_only);
  CHECK_SCREEN(t, 208100, Screen::kFreeRoam);
  CHECK(mcla::gamestate::InMenuValue(t.Current()) == 0);
  CHECK(t.bursts() == 1);  // the real clusters (<= 5 activations) never look like the walk
}

void TestRaceGarageMap() {
  std::printf("-- race, garage, map, results (synthetic)\n");
  Tracker t;
  RegistrationWalk(t, 100);
  CHECK_SCREEN(t, 200, Screen::kUnknown);
  Ev(t, "GameState/GameState_Interacting/Cruising/Cruising_Offline", 1000);
  Ev(t, "UserInterface/Invisible", 1000);
  CHECK_SCREEN(t, 1050, Screen::kFreeRoam);
  // A race proposal in free roam changes nothing.
  Ev(t, "RaceProposalState/RaceProposalPending", 2000);
  CHECK_SCREEN(t, 2050, Screen::kFreeRoam);
  // The race: driving the whole way, red light included.
  Ev(t, "GameState_Interacting/Racing", 3000);
  Ev(t, "Racing/Racing_Local", 3000);
  Ev(t, "Racing_Dynamic/Racing_RedLight", 3000);
  CHECK_SCREEN(t, 3050, Screen::kRacing);
  CHECK(mcla::gamestate::InMenuValue(t.Current()) == 0);
  // Pause during the race, resume.
  Ev(t, "UserInterface/MenuScene/PauseMenu", 20000);
  CHECK_SCREEN(t, 20050, Screen::kPause);
  Ev(t, "UserInterface/Invisible", 22000);
  CHECK_SCREEN(t, 22050, Screen::kRacing);
  // The map from the pause menu and back to the pause menu, then out.
  Ev(t, "UserInterface/MenuScene/PauseMenu", 30000);
  Ev(t, "PauseTab/PM_MapPage", 30500);
  CHECK_SCREEN(t, 30550, Screen::kMap);
  Ev(t, "PauseTab/PM_Continue", 31000);
  CHECK_SCREEN(t, 31050, Screen::kPause);
  Ev(t, "UserInterface/Invisible", 32000);
  CHECK_SCREEN(t, 32050, Screen::kRacing);
  // Results: a flow state, not driving; the next Cruising brings driving back.
  Ev(t, "GameState_Interacting/PostRace", 60000);
  CHECK_SCREEN(t, 60050, Screen::kFrontEnd);
  Ev(t, "GameState_Interacting/Cruising", 70000);
  Ev(t, "Cruising/Cruising_Offline", 70000);
  CHECK_SCREEN(t, 70050, Screen::kFreeRoam);
  // The garage, and out of it.
  Ev(t, "GameState/GameState_Garage", 80000);
  Ev(t, "GameState_Garage/Garage", 80000);
  CHECK_SCREEN(t, 80050, Screen::kGarage);
  Ev(t, "GameState/GameState_Interacting", 90000);
  Ev(t, "GameState_Interacting/Cruising", 90000);
  CHECK_SCREEN(t, 90050, Screen::kFreeRoam);
}

void TestUnknownKeepsLast() {
  std::printf("-- unknown names, settle, save dialogs\n");
  Tracker t;
  Ev(t, "GameState_Interacting/Cruising", 1000);
  CHECK_SCREEN(t, 1050, Screen::kFreeRoam);
  const uint64_t before = t.unmatched();
  Ev(t, "Mystery/Thing", 2000);
  Ev(t, "?", 2000);
  CHECK_SCREEN(t, 2050, Screen::kFreeRoam);  // keeps the last known screen
  CHECK(t.unmatched() == before + 2);
  // Save dialogs (a separate machine, never reports its end) do not move the screen.
  Ev(t, "SaveGameDialogs/SG_SaveGameError", 3000);
  Ev(t, "SG_SaveGameError/SG_SaveGameError.s0", 3000);
  CHECK_SCREEN(t, 3050, Screen::kFreeRoam);
  // Settle: a reader sees the old screen until the events are 20 ms old.
  Ev(t, "UserInterface/MenuScene/PauseMenu", 5000);
  CHECK_SCREEN(t, 5000, Screen::kFreeRoam);
  CHECK_SCREEN(t, 5019, Screen::kFreeRoam);
  CHECK_SCREEN(t, 5020, Screen::kPause);
  CHECK(t.Current() == Screen::kPause);
  // A state that settles back within the window is never seen.
  Ev(t, "UserInterface/Invisible", 6000);
  Ev(t, "UserInterface/MenuScene/PauseMenu", 6005);
  CHECK_SCREEN(t, 6030, Screen::kPause);
  // An unreadable chain ("?") and an empty one are ignored.
  Tracker u;
  Ev(u, "?", 10);
  u.OnActivation(nullptr, 0, 11);
  CHECK_SCREEN(u, 100, Screen::kUnknown);
}

void TestBurstThreshold() {
  std::printf("-- burst threshold\n");
  // 31 quick activations are a real (large) cluster; the 32nd makes it the registration walk.
  Tracker a;
  for (int i = 0; i < 31; ++i) Ev(a, i % 2 ? "GameState_Interacting/Cruising" : "UserInterface/Invisible", 1000 + i);
  CHECK_SCREEN(a, 1100, Screen::kFreeRoam);
  CHECK(a.bursts() == 0);
  Tracker b;
  for (int i = 0; i < 32; ++i) Ev(b, i % 2 ? "GameState_Interacting/Cruising" : "UserInterface/Invisible", 1000 + i);
  CHECK_SCREEN(b, 1100, Screen::kUnknown);
  CHECK(b.bursts() == 1);
  // 40 activations spread by >= 50 ms are not a burst.
  Tracker c;
  for (int i = 0; i < 40; ++i) Ev(c, i % 2 ? "GameState_Interacting/Cruising" : "UserInterface/Invisible", 1000 + i * 60);
  CHECK_SCREEN(c, 5000, Screen::kFreeRoam);
  CHECK(c.bursts() == 0);
  // After a discarded walk the next real state is taken at once (the walk's layers were cleared).
  Ev(b, "UserInterface/MenuScene/PauseMenu", 9000);
  CHECK_SCREEN(b, 9050, Screen::kPause);
}

void TestWalkThenClusterWithoutCommit() {
  std::printf("-- walk, then the first real cluster with no commit in between\n");
  // No tick ran between the registration walk and the title's first states (the heartbeat only starts
  // with the first engine frame): the walk must still be dropped and the cluster kept.
  Tracker t;
  RegistrationWalk(t, 2600);
  Ev(t, "GameState_Disengaged/GameState_Loading", 3600);
  Ev(t, "UserInterface/Setup", 3600);
  CHECK_SCREEN(t, 3650, Screen::kFrontEnd);
  CHECK(t.bursts() == 1);
}

void TestLogRateLimit() {
  std::printf("-- [mcla-mode] rate limit\n");
  Tracker t;
  int logged = 0;
  uint32_t suppressed_reported = 0;
  int64_t now = 1000;
  for (int i = 0; i < 60; ++i) {  // 60 changes in 6 s: driving <-> pause
    Ev(t, i % 2 ? "UserInterface/Invisible" : "UserInterface/MenuScene/PauseMenu", now);
    if (i == 0) Ev(t, "GameState_Interacting/Cruising", now);
    const Tracker::Change c = t.Commit(now + 30);
    if (c.log) {
      ++logged;
      suppressed_reported += c.suppressed;
    }
    now += 100;
  }
  CHECK(logged == Tracker::kLogLinesPerWindow);
  // The next window (after 10 s) logs again and reports what was held back.
  now += 11000;
  Ev(t, "UserInterface/MenuScene/PauseMenu", now);
  Tracker::Change c = t.Commit(now + 30);
  CHECK(c.changed);
  CHECK(c.log);
  CHECK(c.suppressed == 60 - Tracker::kLogLinesPerWindow);  // the 40 changes held back, said once
  suppressed_reported += c.suppressed;
  std::printf("   logged %d of 60 changes in the first window, %u reported as suppressed later\n", logged,
              suppressed_reported);
}

}  // namespace

int main() {
  TestClassifyTable();
  TestFreshProfileChain(false);
  TestFreshProfileChain(true);
  TestRaceGarageMap();
  TestUnknownKeepsLast();
  TestBurstThreshold();
  TestWalkThenClusterWithoutCommit();
  TestLogRateLimit();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
