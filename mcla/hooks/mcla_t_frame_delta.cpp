// mcla_t_frame_delta (A2): the hitch clamp of the 60 fps timing (docs/mcla/TIMING.md section 3,
// HOOKS_SPEC A2). Only with --mcla_sim60=true; off = the game's own code.
//
// Site 0x821BDAB0 in rage::sysTimeManager::Update (sub_821BDA90), AFTER `subf r8,r10,r11`:
//     0x821BDAA0  ld r10,64(r3)       previous timebase
//     0x821BDAA8  std r11,64(r3)      this timebase
//     0x821BDAB0  subf r8,r10,r11     r8 = elapsed guest ticks (signed 64 bit)   <- hooked AFTER this
//     0x821BDAC0..E0  r8 -> seconds x [0x82011110] (1/49,875,000), stored at +0x5C
// r8 is capped at 1/8 s of guest ticks (the SDK's guest tick rate, Clock::guest_tick_frequency()).
// Why: the engine clamps dt to 0.1 s, but the timer's total/game-time accumulators and the audio
// clock (+0x60) take the measured time BEFORE that clamp; a 5 s app suspension (or the first frame,
// whose "previous timebase" is 0) would otherwise reach them as one step. With the shipped fixed
// step this cannot happen; with a world step chosen from the real time it can. The capped time in
// ms is also handed to the T1 hook of the same Update call (the quantiser's input), measured with
// the host's real tick rate: the game's own float uses 49.875 MHz while the SDK's guest clock runs
// at 50 MHz, 0.25 % apart.
//
// Off: --mcla_sim60=false (default), or MCLA_HOOK_SIM_FRAME_DELTA=0 at build time.
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/chrono/clock.h>
#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_t_common.h"

namespace {

mcla::hooks::Registration g_registration("sim-frame-delta", "mcla_sim60", MCLA_HOOK_SIM_FRAME_DELTA != 0,
                                         [] { return mcla::sim::Enabled(); }, /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_SIM_FRAME_DELTA

void mcla_hook_frame_delta(PPCRegister& r8) {
  MCLA_HOOK_ENTER();
  if (!mcla::sim::Enabled()) {
    return;
  }
  static const int64_t frequency = int64_t(rex::chrono::Clock::guest_tick_frequency());
  const int64_t limit = (frequency > 0 ? frequency : 50'000'000) / 8;
  int64_t ticks = r8.s64;
  if (ticks > limit) {
    ticks = limit;
  } else if (ticks < 0) {
    ticks = 0;
  }
  r8.s64 = ticks;
  mcla::sim::NoteRawDelta(double(ticks) * 1000.0 / double(frequency > 0 ? frequency : 50'000'000));
}

#else  // MCLA_HOOK_SIM_FRAME_DELTA == 0: keeps hooks_timing.toml linking, does nothing.

void mcla_hook_frame_delta(PPCRegister&) {}

#endif
