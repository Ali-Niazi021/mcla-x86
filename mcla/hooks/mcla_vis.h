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

// The scene dev cvars (mcla_dev_tod, mcla_dev_tod_freeze, mcla_dev_weather, mcla_dev_weather_snap, mcla_dev_road_density; DEV / TEST
// ONLY, mcla_vis_scene.cpp): writes the game's time of day, weather and road density while a cvar is set and puts the game's own
// value back when it returns to -1. Called at the end of OnFrame(), so a mcla_vis_ab entry that changes one of them takes effect in
// the same frame. Cheap when every cvar is -1 (five cvar reads). Game thread only.
void SceneOnFrame();

}  // namespace mcla::vis
