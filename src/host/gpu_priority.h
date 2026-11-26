// mcla-x86 host: the process's GPU scheduling priority class (H2).
//
// The Radeon 880M is shared: another process's GPU work (a browser, a video, an Electron app, a compute job) is
// time-sliced with ours by the Windows GPU scheduler, and run h1run1 / the V5m6t analysis showed the game's GPU time per
// frame growing from ~14 to ~19-26 ms when something else used the GPU. D3DKMTSetProcessSchedulingPriorityClass
// (gdi32, no admin for the classes up to HIGH) raises the share the scheduler gives this process.
//
// Cvar mcla_gpu_priority (live): 0 = leave (restores the class found at start when set back to 0), 1 = above normal,
// 2 = high. Logs `[mcla-gpu] scheduling priority class <old> -> <new> (status 0x...)`.
#pragma once

namespace mcla_host {

// Applies mcla_gpu_priority now and on every later change of the cvar. Call once (OnPostSetup).
void StartGpuPriority();
// Restores the class found at start.
void StopGpuPriority();

}  // namespace mcla_host
