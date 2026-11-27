// mcla-x86 host overlay: the statistics snapshot.
//
// One Sampler (UI thread only) turns the game's counters into the numbers the FPS counter and the statistics screen show. It
// reads what already exists and costs nothing while nobody asks: the overlay calls Update() from its draw, and the draw only
// exists while something is visible.
//
//   engine frames    overlay_feed.h ring: fps, average, p50 / p99 / p99.9, 1 % / 0.1 % low, frames over 20 / 25 / 34 ms, the
//                    graph (updated 4 times a second; the ring is 34 s deep at 120 fps)
//   GPU              mcla::gov::ReadGpuCounters (SDK 04f0 / 0510 busy time, 0590 pipeline counters) and the frame counter
//                    rexgpu_gpu_frame_seq (the GPU's own frame rate)
//   governor         mcla::gov::Enabled / Tier / Vrs / Level / CompileBurstActive, mcla::gfxtune::CurrentTier / Tiers
//   world            mcla::sim::EffectiveCap, and the game timer's total (guest 0x827D7514) against real time = world speed
//   display          the SDK's present statistics when the build has patch 0755 (rex_present_stats, present_stats_abi.h):
//                    presents, frames the display showed, dropped, repeats and the cadence; "not available" without it
//   hitches          overlay_feed.h NoteHitch
//   machine          1 Hz: CPU clock per efficiency class against its maximum and limit (CAPPED = firmware / Windows holds the
//                    clock down), the process' CPU in cores, working set and private memory, the system's memory load, reads
//                    and page faults a second. The same facts as system_status.cpp's `[mcla-sys] sample` line, as numbers.
//
// Everything is best effort: a counter that is not there reads as unknown (a *_known flag false), never as zero.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "overlay_feed.h"
#include "overlay_math.h"

namespace mcla_overlay {

struct ClockClass {
  int efficiency_class = 0;  // Windows' EfficiencyClass: the higher, the faster cores
  int logical = 0;           // logical processors of the class
  uint32_t avg_mhz = 0;
  uint32_t max_mhz = 0;
  uint32_t min_limit_mhz = 0;  // the lowest limit any of them has right now
  bool capped = false;         // min_limit_mhz < max_mhz
};

// The display side of the presents (SDK patch 0755). Rates are over the last second or more.
struct PresentView {
  bool available = false;  // the export exists in this SDK build
  bool valid = false;      // the display is taking presents (window visible) and the statistics work
  bool rates_valid = false;  // the rates below were measured over at least a second (false in the first second after the screen is shown)
  double present_fps = 0.0;
  double shown_fps = 0.0;
  double dropped_per_s = 0.0;
  double refresh_hz = 0.0;  // the refresh clock's rate (below the display mode's: variable refresh), 0 = unknown
  double cadence = 0.0;     // refreshes one frame is expected to stay (refresh / present rate), 0 = unknown
  double held1_pct = 0.0, held2_pct = 0.0, held3p_pct = 0.0;  // share of attributable shown frames
  double over_cadence_per_min = 0.0;                          // frames held longer than the cadence allows
  uint64_t dropped_total = 0;
  uint64_t over_cadence_total = 0;
};

struct Snapshot {
  int64_t at_ns = 0;

  // The engine's frames: the percentiles over a 5 s window, the rate the counter shows over the last second.
  bool frames_fed = false;
  math::FrameTimeStats frames;
  double fps_1s = 0.0;       // frames a second over the last 1 s (0 = none)
  double avg_ms_1s = 0.0;

  // GPU.
  bool gpu_known = false;
  double gpu_busy_ms = 0.0;  // read back 2-4 frames late
  double gpu_fps = 0.0;      // frames the GPU finished a second, 0 = unknown
  uint32_t gpu_lag = 0;
  bool compile_known = false;
  uint32_t pipelines = 0;  // cumulative pipeline creations at run time
  uint32_t shaders = 0;    // cumulative shader translations at run time
  int32_t pending = 0;     // asynchronous pipeline creations in flight now
  double pipelines_per_min = 0.0;

  // The adaptive-detail governor.
  bool gov_on = false;
  int gov_level = 0;   // 0 base, > 0 shed, < 0 enriched
  int gov_tier = 0;
  int gov_vrs = 0;
  bool compile_burst = false;
  int gfx_tier = -1;   // the tier the knobs follow now, -1 = none
  std::string gfx_tier_name;

  // The world.
  bool sim_on = false;      // mcla_sim60: the 60 fps timing
  int world_cap = 0;        // the frame cap the world assumes (30 / 60 / 120), 0 = unknown
  double world_speed = -1.0;  // world seconds a real second, -1 = not known yet; 0 = paused

  PresentView present;
  HitchSummary hitches;

  // The machine (1 Hz).
  bool system_known = false;
  double cpu_cores = 0.0;
  uint32_t working_set_mib = 0;
  uint32_t private_mib = 0;
  uint32_t memory_load_pct = 0;
  double read_mib_s = 0.0;
  double page_faults_s = 0.0;
  std::vector<ClockClass> clocks;
};

// What the About tab prints: facts that do not change while the game runs. Collected once, when the menu is first opened.
struct AboutInfo {
  std::string build;    // "RelWithDebInfo build 20261127_0250"
  std::string sdk;      // "ReXGlue 0.10.0, SDK patch series 03231bbda44785bb/109patches"
  std::string gpu;      // the adapter the game renders on, with its memory
  std::string display;  // the monitor the window is on: device, mode, DPI
  std::string machine;  // power source, Windows power mode, CPU classes (system_status.h one line)
  std::string guest;    // the guest video mode and the frame-rate profile
};
// `native_window` = Window::GetNativeWindowHandle() (null = the primary monitor).
AboutInfo CollectAbout(void* native_window);

class Sampler {
 public:
  Sampler();
  ~Sampler();
  Sampler(const Sampler&) = delete;
  Sampler& operator=(const Sampler&) = delete;

  // UI thread. Cheap when called every frame: it recomputes a part only when that part is due.
  // `with_system` = the statistics screen is up (the 1 Hz machine part and the display part are only read then).
  void Update(int64_t now_ns, bool with_system);

  const Snapshot& Latest() const { return snapshot_; }

  // The newest `count` engine frame times in ms, oldest first (zeros before the first frame): the graph's bars.
  std::vector<float> Graph(size_t count) const { return LastFrameTimes(count); }

 private:
  struct Impl;
  Impl* impl_;
  Snapshot snapshot_;
};

}  // namespace mcla_overlay
