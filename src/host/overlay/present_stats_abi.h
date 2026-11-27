// mcla-x86 host overlay: the layout of the SDK's present statistics export (SDK patch 0755-present-stats-export, merged in the
// series; patches/sdk/0755-present-stats-export.patch declares the same struct).
//
// Patch 0750 samples IDXGISwapChain::GetFrameStatistics after every Present and judges the display's side of the frames
// (shown / dropped, how many refreshes a frame stayed, against the cadence), but only prints it every present_stats_log_s seconds
// from a private class of the presenter. 0755 mirrors the same counters into one exported struct of relaxed atomics:
//
//   extern "C" __declspec(dllexport) RexPresentStatsAbi rex_present_stats;      (rexruntime.dll)
//
// Every counter is CUMULATIVE since the swap chain was made (a reader subtracts two reads); `valid` says whether the display is
// taking presents right now. The overlay finds the symbol with GetProcAddress (like the rexgpu_* counters of mcla_gov.cpp), so a
// build without 0755 simply has no display section, and a newer layout with a larger `size` is read as far as `size` says.
//
// This header is the contract: the SDK patch declares the same struct (without this file; the two are checked by hand when 0755
// changes, and by the static_asserts below on this side).
#pragma once

#include <atomic>
#include <cstdint>

namespace mcla_overlay {

struct RexPresentStatsAbi {
  uint32_t size;     // sizeof(RexPresentStatsAbi) as the SDK built it
  uint32_t version;  // 1
  std::atomic<uint64_t> presents;        // Present calls made
  std::atomic<uint64_t> retired;         // presents the compositor has retired (lags `presents` by 1 or 2)
  std::atomic<uint64_t> shown;           // retired presents that got a refresh of their own
  std::atomic<uint64_t> dropped;         // retired presents that never got one (replaced before their refresh)
  std::atomic<uint64_t> sync_refreshes;  // refreshes the refresh clock counted
  std::atomic<int64_t> sync_qpc;         // QPC ticks those refreshes span (refresh rate = sync_refreshes * qpf / sync_qpc)
  std::atomic<int64_t> qpf;              // QueryPerformanceFrequency
  std::atomic<uint64_t> held[4];         // shown frames that stayed 1, 2, 3, 4 or more refreshes (the attributable ones)
  std::atomic<uint64_t> over_cadence;    // frames that stayed longer than the cadence allows (a '[present-stats] hitch')
  std::atomic<uint32_t> cadence_milli;   // refreshes a frame is expected to stay, x 1000, of the last judgeable 2 s; 0 = none
  std::atomic<uint32_t> valid;           // 1 while GetFrameStatistics works and the window is taking presents
};

static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free,
              "the counters are plain words shared across the DLL boundary");
// 2 x u32, 5 + 2 + 4 + 1 x u64 / i64, 2 x u32: the SDK's struct (patch 0755) asserts the same number.
static_assert(sizeof(RexPresentStatsAbi) == 8 + 8 * 5 + 8 * 2 + 8 * 4 + 8 + 4 + 4, "the layout agrees with the SDK's");

}  // namespace mcla_overlay
