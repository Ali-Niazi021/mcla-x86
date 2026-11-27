// mcla-x86 host: page-fault probe and guest memory pre-fault.
//
// Why: the first frames of a scene (the front end's first 1080p frame) take 115 to 205 ms and the process takes about
// 73,000 page faults in the same frame (the `[mcla-hitch]` line says `page faults +N`): about 285 MiB of memory touched
// for the first time while the game waits. Which memory, was not known.
//
//   mcla_pf_probe_ms   (default 0 = off) Diagnostic. A thread samples the process' page fault count every N ms. When it
//                      jumps (a burst: at least `mcla_pf_probe_burst` faults), the thread takes a snapshot of which pages
//                      of the whole address space are in the working set after the burst and compares it with one it took
//                      before, and logs the biggest differences by range, naming the arena view and the guest address:
//                          [mcla-pf] burst: +73743 faults in 180 ms ... resident +72101 pages (281.6 MiB):
//                          [mcla-pf]   +65536 pages (256.0 MiB)  phys 0x0A100000..0x1A100000 (arena physical view) ...
//
//   mcla_prefault_guest (code default false, true in the PC profile) Reads one byte of every 4 KiB page of the guest ranges that the first 3D scene
//                      touches (cvar mcla_prefault_ranges says which views), once, on a background thread started when the
//                      runtime is up (default phys,va,ve): the soft faults happen at boot, where nobody sees them, instead of in the scene's
//                      first frame. Reading only: never writes guest memory, so it cannot race the guest. Logs
//                      `[mcla-prefault] touched N MiB (P pages) of <ranges> in X ms`.
//
// Both are host-side and change nothing in the guest. The probe's snapshots take tens of ms of a background thread's
// time and are diagnostics only: never on in a measurement run.
#pragma once

namespace mcla_host {

// Called from McLaApp::OnPostSetup (the kernel state and the guest memory exist) and OnShutdown.
void StartPfProbe();
void StopPfProbe();

}  // namespace mcla_host
