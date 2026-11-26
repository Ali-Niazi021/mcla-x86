// Midnight Club: Los Angeles: VISUALS-stream tooling that lives in the game process (mcla_vis_ab.cpp).
//
// mcla_vis_ab, the in-run A/B scheduler: changes cvars at given seconds within ONE game run, so two settings can be
// compared in the same scene (the harness has no way to change a cvar mid-run). OnFrame() is called once per engine
// frame on the game thread (from mcla::gfxtune::OnFrame in mcla_gfx_tune.cpp, which the heartbeat hook runs).
#pragma once

namespace mcla::vis {

// Applies the entries of the mcla_vis_ab cvar that have come due (see mcla_vis_ab.cpp for the format). Cheap when the
// cvar is "none" or the schedule is finished: one cvar read and a string compare. Game thread only; no locks.
void OnFrame();

}  // namespace mcla::vis
