// mcla-x86 host: guest hang watchdog.
//
// Port of the VIGILANTE DE CUELGUES of the iOS port ($IOS common/src/nfsmw_app.h, section 4), minus the Most Wanted
// and Black Edition parts, the semaphore nudge and the SDK's semaphore trace ring (none of them exist for MCLA).
//
// A hang of the whole game leaves nothing in the log: no error, no kernel call, no GPU command. What defines it is that
// no guest thread moves any more, so the watchdog asks the threads. Once a second it samples lr, r1 and r3 of every
// guest thread, folds them into one signature and mixes in the engine's frame heartbeat (nfsc::ProgressBeats(), bumped
// by the game's per-frame hook). A signature that does not change for a while means "stopped, not slow": it dumps
// every guest thread (entry, lr, r1, r13, r3, ctr, last indirect target, what it waits on, a bounded guest stack walk)
// to the log as ERROR lines and flushes the log, then repeats every 15 s for as long as the stall lasts.
//
// Thresholds (MCLA): 90 s of stillness before the first frame beat has ever been seen (the legal screens and the
// blocking loads of the boot have no beat, and the hooked waits park threads), 5 s after it. A DEBUG snapshot of every
// thread goes to the log every 10 s regardless.
//
// Also, every 10 s, one INFO line `[mcla-heartbeat] frames=<n>` (n = nfsc::ProgressBeats()): tools/run_game.py reads it
// as "the heartbeat is advancing". The engine hook itself logs `[mcla-heartbeat]` only once.
//
// Cost when nothing happens: one pass a second reading three registers per thread.
#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>

namespace mcla_host {

class Watchdog {
 public:
  Watchdog() = default;
  ~Watchdog();
  Watchdog(const Watchdog&) = delete;
  Watchdog& operator=(const Watchdog&) = delete;

  // Starts the sampling thread (no-op if it already runs). Call after the kernel state exists (OnPostSetup).
  void Start();
  // Stops and joins the thread (no-op if it does not run). Safe to call from any thread but the watchdog's own.
  void Stop();

 private:
  void Main();

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
};

}  // namespace mcla_host
