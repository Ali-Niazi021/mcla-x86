// mcla-x86: the hitch lines (`[mcla-hitch]`). Not a hook, always compiled (the hooks/*.cpp glob picks it up); called
// once per engine frame from mcla::sim::OnFrameStart (mcla_t_common.cpp), on the game thread.
//
// The `[mcla-sim] frame times` line says HOW MANY frames were long in a 5 s window; a hitch line says, for each long one,
// WHAT was going on while it took that long, so a stutter can be put on its cause from the log alone:
//
//   [mcla-hitch] 183.4 ms between engine frames (11.0 periods of 16.7) in menu | game thread on CPU 12.3 ms
//   (waiting 171.1 ms, of which pacer sleep 4.2 ms) | pipelines +148 shaders +0 in flight 3 | GPU frame 6.7 ms |
//   page faults +2340, read +12.4 MiB
//
//   on CPU / waiting   the game thread's own cycles in that frame (QueryThreadCycleTime): mostly on CPU = the game's code
//                      itself was slow (streaming, a long load step); mostly waiting = it was blocked on something (the GPU,
//                      a lock, the disk, a pipeline compile on the GPU command thread, the OS taking the core away)
//   pipelines/shaders  run-time pipeline creations and shader translations in that frame (SDK 0590): a shader-cache miss
//   GPU frame          the GPU busy time of the last frame the GPU timing read back (SDK 04f0/0510), 2-4 frames late
//   page faults / read the process' page faults and file bytes read in that frame: paging or a disk read on the frame
//
// cvars: mcla_hitch_ms (0 = auto: 1.5 frame periods of the cap in force, 25 ms at 60), mcla_hitch_lines (most lines a
// log window, the rest counted in a summary line). The per-frame cost is three cheap syscalls.
#pragma once

#include <cstdint>

namespace mcla::hitch {

// frame_ms: real time since the previous engine frame (0 = none: the first frame, or after a gap over 1 s);
// period_ms: the pacer's frame period; screen_mode: -1 unknown, 0 drive, 1 menu / pause / load (mcla::gamestate);
// slept_ms: what the pacer slept at the start of the previous frame (part of frame_ms).
void OnFrame(int64_t now_ns, double frame_ms, double period_ms, int screen_mode, double slept_ms);

}  // namespace mcla::hitch
