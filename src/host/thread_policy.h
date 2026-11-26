// mcla-x86 host: thread monitor and live thread policy.
//
// One background thread (named `mcla thread policy`) that ticks once a second and does two independent jobs, both
// controlled by cvars that are re-read every tick (so a schedule thread can flip them with rex::cvar::SetFlagByName):
//
//  1. MONITOR. A snapshot of the threads of this process (state, priorities, context switches: NtGetNextThread +
//     NtQueryInformationThread(ThreadSystemThreadInformation), ~0.1 ms for 70 threads; the fallback,
//     NtQuerySystemInformation(SystemProcessInformation), walks every thread of every process and costs ~16 ms a
//     second on a machine with 370 processes), plus the CPU cycles of each thread (QueryThreadCycleTime, divided by
//     the TSC rate measured once at start) and the thread names (GetThreadDescription, cached; a thread that has no
//     name yet is asked again). With --mcla_thread_stats_s=N it writes two lines every N seconds:
//       [mcla-threads] t=12.3 dt=2.0 total=2.31 | GPU Commands (F8000018)=0.61 | ... | other=0.12
//       [mcla-threads]   csw/s total=12345 ready=GPU Commands (F8000018):3,XThread4D2 (F80004DC):2
//     (top 10 threads by cores of CPU over the interval; unnamed threads as tid<id>; `ready` = how many once-a-second
//     samples found the thread in the Ready state, i.e. runnable and waiting for a core).
//
//  2. POLICY. Every thread gets a role from its name (guest and SDK threads end in ` (HHHHHHHH)`, the guest handle:
//     matched by prefix):
//       NEVER   GPU VSync, GPU Display VBlank, Audio Worker, XMA Decoder, SDLAudio, SDLWASAPI, SDLRawInput, every
//               unnamed thread and this thread itself. Never touched, whatever the cvars say.
//       HOT     GPU Commands, GPU Submission, Main XThread, and every `XThread...` whose CPU averaged at least
//               mcla_thread_hot_min percent of a core over the last 5 s (sticky: once hot, hot).
//       COLD    D3D12 Pipelines, Shader Translation, D3D12 Storage writer, rex::thread::TimerQueue.
//       other   neutral, untouched.
//     The knobs below act on the roles (all off by default: with every knob at 0/false and no stats the thread does no
//     snapshot at all and touches nothing). Going back to 0/false RESTORES what the thread, the process had before
//     the first touch, and Stop() restores everything too.
//
//       mcla_thread_stats_s        0..3600  stats lines every N s (0 = off)
//       mcla_thread_hot_min        0..1000  percent of a core for an XThread to become HOT (default 15)
//       mcla_thread_hot_priority   0..2     HOT: 0 leave, 1 ABOVE_NORMAL, 2 HIGHEST (SetThreadPriority)
//       mcla_thread_hot_cpus       0..2     HOT: 0 leave, 1 CPU sets = every logical processor of the highest efficiency
//                                           class (Zen 5), 2 = 1 + ideal processor spread over distinct physical cores
//                                           of that class (GPU Commands, hot XThreads by CPU, Main, GPU Submission)
//       mcla_thread_cold_cpus      0..1     COLD: 1 = CPU sets = every logical processor of the lowest efficiency class
//       mcla_thread_cold_ecoqos    bool     COLD: EcoQoS (power throttling EXECUTION_SPEED on)
//       mcla_thread_hot_ecoqos     bool     HOT: EcoQoS on (an A/B experiment: efficient clocks for frame-critical threads)
//       mcla_thread_hot_noecoqos   bool     HOT: opted out of EcoQoS ("never throttle"); loses to hot_ecoqos (warns once)
//       mcla_process_priority      0..2     process class: 0 leave, 1 ABOVE_NORMAL, 2 HIGH
//       mcla_process_noecoqos      bool     process opted out of EcoQoS (what Windows applies to hidden windows); the
//                                           other bits of the process throttling state (the timer-resolution opt-out of
//                                           win32_time.cpp) are preserved
//
// Everything is best effort: a failing call is logged once (per thread and knob) and never retried with the same
// value; nothing here can crash or stall the game. Cost: ~0.02 % of a core with stats or knobs on (one thread walk a
// second), ~0.003 % idle (the thread only re-reads the cvars; no snapshot, no handles).
//
// With everything off the thread does not follow the process, so a knob switched on later starts from a fresh baseline:
// roles by name apply at the next tick, an XThread needs 5 s of history to become HOT.
//
// Integration (mcla_app.cpp): a ThreadPolicy member; thread_policy_.Start() in OnPostSetup (after the log exists; it
// does not need the kernel state), thread_policy_.Stop() in OnShutdown / OnGuestThreadExit next to the watchdog.
// No extra libraries: ntdll is resolved with GetProcAddress, everything else is kernel32 (Windows 10 1709+).
#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>

namespace mcla_host {

class ThreadPolicy {
 public:
  ThreadPolicy() = default;
  ~ThreadPolicy();
  ThreadPolicy(const ThreadPolicy&) = delete;
  ThreadPolicy& operator=(const ThreadPolicy&) = delete;

  // Starts the monitor thread (no-op if it already runs).
  void Start();
  // Stops the monitor thread, restores everything the policy changed, and returns once that is done. Idempotent and
  // safe to call concurrently from any threads (exactly one caller joins, the others wait for the thread to be gone).
  // A call from the monitor thread itself detaches it instead of joining.
  void Stop();

 private:
  void Main();

  std::thread thread_;
  std::thread::id worker_id_;  // the monitor thread's id, to recognise a Stop() from inside it
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  bool running_ = false;  // Start() ran and Main() has not returned yet
};

}  // namespace mcla_host
