// mcla_t_race_clock (T6): the race clock keeps its fraction (docs/mcla/TIMING.md section 4.3). Only
// with --mcla_sim60=true and --mcla_race_clock != off.
//
// Site 0x822562E8 in sub_82256220 (the race clock's update), BEFORE `fctiwz f13,f0`:
//     0x822562E4  fmuls f0,f0,f13        f13 = 1000.0: f0 = the step in ms (timer +0x08 or +0x64)
//     0x822562E8  fctiwz f13,f0          <- hooked BEFORE this: the game truncates f0 to whole ms
//     0x822562F4  lwz r10,3768(r31) ; add ; stw r11,3748(r31)    [race+3748] += that, r31 = the race
// Truncation gives 33 ms per 1/30 s (1 % slow, the console's own clock) and 16 per 1/60 s (4 % slow).
// The hook keeps the fraction per race object and hands the game a whole number: --mcla_race_clock=
// retail (default) is 0.99 of the real step, i.e. 16.5 ms per 1/60 s (the console's 33 per 1/30, so
// lap times match); exact is 16.667; off leaves the game's truncation. At a 30 cap retail returns the
// game's 33 every step.
//
// Off: --mcla_sim60=false (default), --mcla_race_clock=off, or MCLA_HOOK_SIM_RACE_CLOCK=0.
#include <cmath>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_t_common.h"

namespace {

mcla::hooks::Registration g_registration("sim-race-clock", "mcla_race_clock", MCLA_HOOK_SIM_RACE_CLOCK != 0,
                                         [] { return mcla::sim::RaceClockActive(); }, /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_SIM_RACE_CLOCK

void mcla_hook_race_clock(PPCRegister& f0, PPCRegister& r31) {
  MCLA_HOOK_ENTER();
  if (!mcla::sim::RaceClockActive()) {
    return;
  }
  const double dt_ms = f0.f64;
  if (!(dt_ms >= 0.0) || dt_ms > 1000.0) {
    return;  // paused (0 stays 0 through the carry too), NaN or a runaway step: the game's own value
  }
  f0.f64 = double(mcla::sim::RaceClockMs(r31.u32, dt_ms));
}

#else  // MCLA_HOOK_SIM_RACE_CLOCK == 0: keeps hooks_timing.toml linking, does nothing.

void mcla_hook_race_clock(PPCRegister&, PPCRegister&) {}

#endif
