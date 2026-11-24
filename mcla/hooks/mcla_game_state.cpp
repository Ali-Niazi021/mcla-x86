// mcla_game_state: the classifier table and the Tracker (see mcla_game_state.h). No engine headers.
#include "mcla_game_state.h"

#include <cstdio>
#include <cstring>

namespace mcla::gamestate {
namespace {

struct RuleDef {
  const char* text;
  bool prefix;  // the name starts with `text` (else: equals it)
  Layer layer;
  Screen screen;
};

// The table. First entry that matches a name wins, the chain is walked leaf to root. Names are the ones
// in docs/mcla/FLOWS.md ([mcla-state] of mac002 r09 and dev001) and the ones the game's code looks up
// (HOOKS_SPEC F: GameState_Loading/Intro/Garage/Hangout, Cruising, Racing, PostRace, PauseMenu,
// PauseTab, ReplayPause, CareerNavSystemScene, GarageShowroom, Career_Loading). Not verified on
// screen: PostRace, ReplayPause, CareerNavSystemScene, Career_Loading, GarageShowroom, Cutscene*,
// PM_Map* (read from code, no logged session reaches them yet): [mcla-mode] says what was seen.
constexpr RuleDef kRules[] = {
    // Roots and containers: "nothing to change", and nothing above them counts. GameState_Interacting
    // holds Cruising and Racing, which say what it means. RaceProposal* and RaceOverMode can come up
    // while the player still steers; the save dialogs (SaveGame*, SG_*) are a separate machine that
    // never reports its end, so they would stick.
    {"GameState", false, Layer::kNone, Screen::kUnknown},
    {"UserInterface", false, Layer::kNone, Screen::kUnknown},
    {"GameState_Interacting", false, Layer::kNone, Screen::kUnknown},
    {"RaceProposal", true, Layer::kNone, Screen::kUnknown},
    {"RaceOverMode", false, Layer::kNone, Screen::kUnknown},
    {"SaveGame", true, Layer::kNone, Screen::kUnknown},
    {"SG_", true, Layer::kNone, Screen::kUnknown},

    // The world's flow.
    {"GameState_Disengaged", false, Layer::kFlow, Screen::kFrontEnd},  // no world: title, loading, intro
    {"GameState_Loading", false, Layer::kFlow, Screen::kLoading},
    {"GameState_Intro", false, Layer::kFlow, Screen::kCutscene},  // vehicle select and the first cutscene
    {"GameState_Garage", false, Layer::kFlow, Screen::kGarage},
    {"GameState_Hangout", false, Layer::kFlow, Screen::kFrontEnd},
    {"Cruising", true, Layer::kFlow, Screen::kFreeRoam},  // Cruising, Cruising_Offline
    {"Racing", true, Layer::kFlow, Screen::kRacing},      // Racing_Local, Racing_Dynamic, RacingOL_*
    {"Garage", true, Layer::kFlow, Screen::kGarage},      // Garage, GarageShowroom
    {"Hangout", true, Layer::kFlow, Screen::kFrontEnd},
    {"EditingRace", true, Layer::kFlow, Screen::kFrontEnd},
    {"LobbyMeeting", true, Layer::kFlow, Screen::kFrontEnd},
    {"PostRace", true, Layer::kFlow, Screen::kFrontEnd},
    {"Cutscene", true, Layer::kFlow, Screen::kCutscene},

    // The user interface on top of it.
    {"Invisible", false, Layer::kOverlay, Screen::kUnknown},  // the HUD only: no overlay
    {"Hud", true, Layer::kOverlay, Screen::kUnknown},         // HudPlayerList
    {"HPL_", true, Layer::kOverlay, Screen::kUnknown},        // HPL_Hidden
    {"Setup", false, Layer::kOverlay, Screen::kFrontEnd},
    {"Startup", true, Layer::kOverlay, Screen::kFrontEnd},  // Startup, Startup_Checks
    {"StartScreen", false, Layer::kOverlay, Screen::kFrontEnd},
    {"Loading", true, Layer::kOverlay, Screen::kLoading},  // Loading, Loading_Fresh, Loading_Old
    {"Career_Loading", true, Layer::kOverlay, Screen::kLoading},
    {"CareerNavSystemScene", true, Layer::kOverlay, Screen::kMap},
    {"PM_Map", true, Layer::kOverlay, Screen::kMap},
    {"PM_", true, Layer::kOverlay, Screen::kPause},  // PM_Continue and the other pause pages
    {"PauseMenu", false, Layer::kOverlay, Screen::kPause},
    {"PauseTab", false, Layer::kOverlay, Screen::kPause},
    {"ReplayPause", false, Layer::kOverlay, Screen::kPause},
    {"MenuScene", false, Layer::kOverlay, Screen::kFrontEnd},  // any menu not named above
};

bool Matches(const RuleDef& rule, const char* name) {
  return rule.prefix ? std::strncmp(name, rule.text, std::strlen(rule.text)) == 0
                     : std::strcmp(name, rule.text) == 0;
}

}  // namespace

const char* ScreenName(Screen screen) {
  switch (screen) {
    case Screen::kUnknown: return "unknown";
    case Screen::kFrontEnd: return "front-end";
    case Screen::kLoading: return "loading";
    case Screen::kCutscene: return "cutscene";
    case Screen::kGarage: return "garage";
    case Screen::kFreeRoam: return "free-roam";
    case Screen::kRacing: return "racing";
    case Screen::kPause: return "pause";
    case Screen::kMap: return "map";
  }
  return "?";
}

Rule Classify(const char* const* chain, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (!chain[i] || !chain[i][0]) continue;
    for (const RuleDef& rule : kRules) {
      if (Matches(rule, chain[i])) return {true, rule.layer, rule.screen};
    }
  }
  return {};
}

void Tracker::OnActivation(const char* const* chain, size_t count, int64_t now_ms) {
  if (!chain || count == 0) return;
  const Rule rule = Classify(chain, count);
  std::lock_guard<std::mutex> lock(mutex_);
  if (events_ != 0 && now_ms - last_event_ms_ > kBurstGapMs) {
    // A new cluster: if the one before was the registration walk and nobody committed since, drop it
    // now, or the next commit would drop this cluster with it.
    if (burst_) DiscardBurst();
    cluster_ = 0;
  }
  ++cluster_;
  ++events_;
  last_event_ms_ = now_ms;
  if (cluster_ >= kBurstEvents) burst_ = true;
  if (!rule.matched) {
    ++unmatched_;
    return;
  }
  if (rule.layer == Layer::kNone) return;
  if (rule.layer == Layer::kFlow) {
    flow_ = rule.screen;
  } else {
    overlay_ = rule.screen;
  }
  if (count > 1 && chain[1] && chain[1][0]) {
    std::snprintf(cause_, sizeof(cause_), "%s/%s", chain[1], chain[0]);
  } else {
    std::snprintf(cause_, sizeof(cause_), "%s", chain[0]);
  }
}

// The registration walk is not the game's state: both layers start again from the next real event.
void Tracker::DiscardBurst() {
  burst_ = false;
  last_burst_size_ = cluster_;
  cluster_ = 0;
  ++bursts_;
  flow_ = overlay_ = Screen::kUnknown;
  cause_[0] = '\0';
}

Tracker::Change Tracker::Commit(int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  Change out;
  out.screen = out.previous = committed_;
  if (events_ == 0 || now_ms - last_event_ms_ < kSettleMs) return out;
  if (burst_) {
    DiscardBurst();
    return out;
  }
  const Screen screen = overlay_ != Screen::kUnknown ? overlay_ : flow_;
  if (screen == Screen::kUnknown || screen == committed_) return out;
  out.changed = true;
  out.previous = committed_;
  out.screen = committed_ = screen;
  std::snprintf(out.cause, sizeof(out.cause), "%s", cause_);
  if (now_ms - log_window_ms_ >= kLogWindowMs) {
    log_window_ms_ = now_ms;
    log_lines_ = 0;
  }
  if (log_lines_ < kLogLinesPerWindow) {
    ++log_lines_;
    out.log = true;
    out.suppressed = log_suppressed_;
    log_suppressed_ = 0;
  } else {
    ++log_suppressed_;
  }
  return out;
}

Screen Tracker::Current() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return committed_;
}

uint64_t Tracker::events() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return events_;
}
uint64_t Tracker::bursts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return bursts_;
}
uint32_t Tracker::last_burst_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_burst_size_;
}
uint64_t Tracker::unmatched() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return unmatched_;
}

}  // namespace mcla::gamestate
