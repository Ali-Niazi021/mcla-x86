// W5.6: what the two 60 fps input fixes share (mcla_input_title_latch.cpp, mcla_input_menu_repeat.cpp). Not a hook.
//
// Both fixes are for a game whose input code counts game updates, not time: written for the console's 30 updates a
// second, it misbehaves at 60 (mcla_sim60). Their cvars are tri-state, like mcla_present_immediate:
//   1  on        0  off        -1 (default) auto: on while mcla_sim60 is on, so a run as shipped (30 fps) is untouched.
// The decision is read on every call, so `--set-at` / a live change works.
#pragma once

#include <chrono>
#include <cstdint>

#include "mcla_t_common.h"

#ifndef MCLA_HOOK_INPUT_TITLE_LATCH
#define MCLA_HOOK_INPUT_TITLE_LATCH 0
#endif
#ifndef MCLA_HOOK_INPUT_MENU_REPEAT
#define MCLA_HOOK_INPUT_MENU_REPEAT 0
#endif

namespace mcla::input {

inline bool Wanted(int32_t mode) { return mode > 0 || (mode < 0 && mcla::sim::Enabled()); }

inline uint64_t NowUs() {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace mcla::input
