// The PC build's slice of the iOS port's GameProfile seam (core/include/game_profile.h, 313 lines of launcher
// catalog, touch layout, renderer quirks and save summaries that a Windows host has no use for). The hooks
// include "game_profile.h" for two things only, and this header is exactly those two:
//
//   * nfsc::profile::Mode, the values mcla_game_state_host.cpp maps the game's screen to;
//   * nfsc::profile::FrameTick(), the once-per-engine-frame callback the heartbeat hook calls
//     (mcla_heartbeat.cpp), which the game-state glue uses to commit a screen change and log [mcla-mode].
//
// The cvar defaults (the launcher defaults of the iOS profile) are in pc_profile.h.
#pragma once

#include "mcla_game_state.h"

namespace nfsc::profile {

// What the game is doing, as far as the host cares. Games add their own from kModeGame upward.
enum Mode : int {
  kModeUnknown = -1,  // game memory not readable yet
  kModeDriving = 0,   // free roam / race / any state where the player steers
  kModeMenu = 1,      // front end, pause, map, garage: anything but driving
};

// Once per engine frame, on the game thread, from the heartbeat hook.
inline void FrameTick() { mcla::gamestate::FrameTick(); }

}  // namespace nfsc::profile
