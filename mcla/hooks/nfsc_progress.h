// A heartbeat the game's per-frame hooks bump, for the hang watchdog
// (nfsmw_app.h, VigilanteMain).
//
// The watchdog's signature of "where every thread is" is the guest registers
// lr/r1/r3 sampled once a second. Once the Carbon hooks turned the game's
// Sleep(0) polling and ring-space spinning into blocking waits, every thread of
// a vsync-locked 60 fps game sits at the same wait most of the frame, and the
// 1 s samples (exactly 60 frames apart) all land at the same point of the
// frame: five identical samples in a row = a false "stopped" alarm for 5-11 s
// while the game ran at 60 fps (dev004, Mac logs). A frame counter that moves
// with every completed frame is what the registers cannot show.
//
// Anything that proves the main loop is alive may call ProgressBeat(); a real
// freeze stops it.
#pragma once

#include <atomic>
#include <cstdint>

namespace nfsc {

inline std::atomic<uint64_t> g_progress_beat{0};

inline void ProgressBeat() { g_progress_beat.fetch_add(1, std::memory_order_relaxed); }
inline uint64_t ProgressBeats() { return g_progress_beat.load(std::memory_order_relaxed); }

}  // namespace nfsc
