// Midnight Club: Los Angeles: the pure logic of the GPU frame governor (LOCK60 A5 / F1, mcla_gov.cpp). No SDK, no guest memory, no
// cvars, no clock: everything here is a function of numbers handed in (std only), so a unit test compiles it standalone
// (mcla/tests/test_gov.cpp). The glue (symbols, cvars, the log lines) is mcla_gov.cpp.
//
// THE LADDER. The state is (tier t, VRS level v). t is the run-time detail tier of mcla_gfx_tune (1 = the perf preset ... 5 = 1.5x
// distances), v the variable-rate-shading level (0 = full rate). Three numbers shape it: the floor F (the tier the picture is
// configured at), the ceiling C (the richest tier the governor may enrich to; C < F = no enrichment) and V (VRS levels used).
// The states form one line, the RUNG r (what Level() returns): r = 0 is the base (F, 0), r < 0 is enriched (F - r, 0), r > 0 is shed:
//
//      enriched tiers      (C,0) ... (F+1,0)      r = -(C-F) ... -1     richer than configured, only while there is headroom
//      the base            (F,0)                  r = 0
//      VRS                 (F,1) ... (F,V)        r = 1 ... V           invisible, ~0.4 / ~0.6 ms
//      tiers below F       (F-1,V) ... (1,V)      r = V+1 ... V+F-1     visible; the last rung (1,V) is the last level: the
//                                                                       60 -> 30 hold may start there (HoldAllowed)
//
// SHED ORDER:   enriched tiers (C..F+1) -> VRS 1..V -> tiers F-1..1 -> hold.      RESTORE / ENRICH: the reverse, one rung at a time.
//
//   SHED (r + 1; fast) when at least shed_frames GPU frames passed since the last change, r is below the cap, and one of
//     a. the median of the last 4 GPU samples (at least 3 of them) is above shed_ms                  ["gpu median"]
//     b. at least shed_over_count (3) of the last 30 GPU samples (0.5 s) are above shed_ms          ["gpu tail"]
//        A tail rule, because under CPU / power contention the GPU frame is bimodal (field run int1-base: busy p50 15.1-15.3 ms, p95
//        24-26 ms, 12-18 missed frames per 5 s, one frame in ~10 far longer) and the median alone never sheds. Samples count while
//        driving and from the 5th published frame after a step (the export is 2-4 frames behind: the first ones are the old state).
//     c. at least 2 of the last 30 engine frames were missed (with or without a GPU signal)         ["missed frames"]
//        Counted while driving, and from the frame after a step. A single missed frame sheds nothing: one 25 ms spike in a clean
//        stream is the stream's, not the rung's.
//   BELOW THE FLOOR (floor_shed_frames, 30; 0 = off: every shed is judged by a, b, c). A shed that goes to a tier under the floor tier F
//     (F -> F-1, and further down to tier 1: the visible rungs, for the perf preset the anti-aliasing) needs SUSTAINED overload, not a
//     snap. Field run lk-w3cold2 (a scene tour: the time of day / weather snaps every ~6 s, one 18-19 ms frame in a 10 ms stream): three
//     snaps inside 0.5 s fired the tail rule and, VRS being at its maximum, took FXAA off, and it came back 4 s later, six times a
//     minute. So for such a shed
//        a. counts only when it HOLDS: the median of the last 4 samples was above shed_ms at a first check and is still above it at a
//           check at least floor_shed_frames GPU frames later, with no check at or below the line in between (a sample whose median is
//           at or below the line ends the run, and so does a screen change, a pause, a lost signal; a step does not: an overload that
//           the rung just taken did not clear goes on down at the usual spacing),
//        b. never counts (the tail rule alone does not take the ladder below the floor; it still sheds the enriched tiers and VRS),
//        c. counts as it is: the player SEES two missed frames in 0.5 s.
//     A restore from a rung below the floor back up is the normal restore; but a shed back to that rung within 30 s of it (kFloorBounceWindowNs:
//     below the floor the probe-bounce window is 30 s, not 2 s) is a probe bounce too: the restore delay out of the rung doubles, so the
//     visible rung cannot flap faster than that back-off.
//   RESTORE (r - 1, r > 0; slow) when driving, and for the whole of the rung's restore delay (a) no frame was missed, (b) the player
//     kept driving, (c) no change happened, and (d) with a GPU signal the p90 of the delay's samples is below restore_ms (and there
//     are at least 10 samples a second of it); with no GPU signal (a/b/c alone) the quiet time is the whole condition. The delay is
//     restore_s, doubled for every PROBE BOUNCE of that rung that has not decayed yet (cap 60 s). A probe bounce = a restore from
//     rung L followed within 2 s by a shed back to L; each doubles the next delay from L; the doublings decay one step per 30 s
//     without a new bounce, counted from the end of the delay they lengthened. (The guard against DVFS: with slack the GPU clocks
//     down and the frame time rises, so a plain threshold would restore, see the time rise, shed, and repeat.)
//   ENRICH (r <= 0, t < C: (t,0) -> (t+1,0); slower still) when driving with a GPU signal, and for the whole of enrich_s (10 s)
//     no frame was missed, the player kept driving, no change happened, and
//         p90(busy over the window) + predicted_cost(t+1) + enrich_margin_ms <= restore_ms,
//     and the back-off of tier t+1 has expired. predicted_cost(k) = max(the table's estimate of tier k over k-1, the cost LEARNED for
//     k): after every tier step between k-1 and k (either way) the difference of the busy p50 over learn_s (3 s) before and after
//     the step (the first 0.5 s after it skipped), over samples taken while driving and without a missed frame, is folded into
//     learned[k] (EMA 0.4, each sample clamped to 0..5 ms; only with at least learn_s x 30 samples on both sides; a pause or a change
//     in between aborts it, except that a tier shed again before the window opened - one that costs far more than planned is over the
//     shed line within a few frames - is measured by the median of the last 4 samples that made the shed (or the samples since the
//     step, from 0.1 s after it, if there are at least 3)). A measurement is DISCARDED (not folded; counted, LearnDiscarded) when any
//     sample of its before or after window is above 1.5x that window's median: a scene snap or a hitch in the window would put its own
//     size into the tier's cost (field: tier 2 drifted to +1.37 ms because the windows straddled scene snaps).
//   BACK-OFF (per tier): a tier k that was entered by ENRICHMENT and is shed (back to k-1) within enrich_hold_s (30 s) of its entry
//     doubles backoff[k] (30 s, cap 600 s: enrichment into k is not tried before it expires). The doubling decays one step (halves,
//     below 30 s = gone) per backoff period spent without a new bounce, counted from the end of the wait it lengthened.
//   Not driving (a menu, a pause, a load): the rung is held (no shed, no restore, no enrich), and the quiet timer is reset on every
//     change of the driving flag and on every missed frame.
//   SCREEN-CHANGE GRACE (screen_grace_frames, 60; 0 = off). A change of the driving flag (either way) changes the scene: the GPU samples
//     and missed frames taken on the previous screen (a 30 ms pause menu, a loading hitch) say nothing about the new one, and the first
//     frames of the new one are hitchy too (field: entering driving from a load or the pause menu shed 0 -> 4 within 0.3-0.4 s on exactly
//     those). So at the change (a) everything recorded so far is DISCARDED: the median ring, the tail window, the missed-frames history,
//     the restore and enrich windows, the learning windows and their samples in progress; and (b) for the change frame and the next
//     screen_grace_frames - 1 engine frames (the first screen_grace_frames frames of the new screen) no sample and no missed frame is
//     recorded anywhere (nor resets the quiet timer), and nothing is shed, restored or enriched. After it the windows fill from empty:
//     the first shed needs 3 fresh samples, the next ones their usual spacing. What is not gated: a pin, the cap, a range change, and a
//     step that waits for a hiding moment, which still goes at the change of the flag to not driving (POP HIDING below; the
//     1 s hiding moment after a change of the flag is unchanged, though with 60 frames of grace it is over about when the grace is).
//     The summary window's own statistics (samples, p50 / p95, missed) and the signal liveness are not part of the decision and still
//     take every sample.
//   POP HIDING. A step between tiers k-1 and k whose tier k has pop_free = false (3 and 5) shows as a pop. Such a step (any kind) is
//     taken only in a HIDING MOMENT: not driving (a pause, a load, a menu), within 1 s after a change of the driving flag, within
//     0.5 s of a camera cut (the camera position jumped more than cut_m in one frame), or while the camera speed is above
//     pop_speed_kmh. Enrichment and restore steps wait for one with no timeout. A SHED waits at most pop_wait_s (1.5 s) and goes
//     at once on a missed frame. A step that is waiting when the player leaves the scene (driving -> not driving) goes at once.
//   COMPILE BURST (compile_excuse_s, 4; 0 = off; Input::compile_burst, from CompileWatch below). Pipelines and shaders are created at run
//     time (the SDK's pipeline cache / translator): a stretch of them stalls the command processor and the GPU samples and missed frames
//     of it are the compiler's, not the picture's (field: a new street shed 0 -> 2 on a hitch no rung helps). While the flag is set,
//     driving and outside a grace, such a frame is EXCUSED: its GPU sample and missed flag are not fed into the shed rules (median, tail,
//     missed history), the restore / enrich windows, the learning; nothing is shed, restored or enriched on it; and it resets the quiet
//     timer (a burst is not calm either, so the first restore / enrichment comes a full delay after it). What was recorded BEFORE the
//     burst stays. Bounded, so a real overload still acts: the excused time is capped at compile_excuse_s within any 20 s (a sliding
//     window, ExcuseBudget; a frame is charged its own length, and a not-driving frame or a frame in a grace is neither excused nor
//     charged); a frame over the cap is judged like any other, and the budget refills as old charges age out of the 20 s.
//   Pinning (Params::force_level >= 0) replaces the loop; a lowered cap, a changed floor / ceiling / VRS count moves the state at once.
//
// THE FLOOR ON AUTO (FloorForPreset / AutoFloor, the glue passes the cvars): the tier mcla_gfx_tier had before the governor first ran if it was
// >= 1, else the tier of the preset mcla_gfx_preset: perf = 2 (perf + FXAA: the AA is part of normal play, so it is never the thing a
// burst takes away: VRS absorbs it, the tier below the floor, tier 1 without FXAA, is only reached on the way to the 30 hold, and an
// enrichment into tier 2 with its back-off, the field's flickering AA, does not exist), perf+ and lowest = 1, quality, default / none = 4,
// ultra = 5. A player who pinned anti-aliasing (swap_post_effect set by hand) makes tiers 1 and 2 the same picture: the 2 -> 1 step then
// buys nothing. It is harmless: the rung is the last one, so no further shed follows it, the learned cost of tier 2 comes out ~0 and the
// enrichment's prediction takes the larger of that and the table's estimate, and a restore from it only costs its delay.
//
// Time is whatever steady clock the caller passes (nanoseconds); the samples carry no timestamps of their own, the call time stands
// in for them (the SDK's value is 2-4 frames old by design, see rexgpu_gpu_frame_lag).
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace mcla::gov::math {

constexpr int kTiers = 6;  // tiers 0..5 (the table of mcla_gfx_tune); the ladder uses 1..5
constexpr int kMinTier = 1;
constexpr int kMaxTier = 5;

// ---- statistics ----------------------------------------------------------------------------------------------------

// Counts of GPU frame times in 0.05 ms buckets (0 .. 80 ms; the last bucket takes everything above): a percentile of a few
// thousand samples costs one pass over 1600 counters and the buffer never grows.
class Histogram {
 public:
  static constexpr int kBuckets = 1600;
  static constexpr double kPerMs = 20.0;

  static int BucketOf(double ms) {
    if (!(ms > 0.0)) return 0;
    const double b = ms * kPerMs;
    return b >= double(kBuckets - 1) ? kBuckets - 1 : int(b);
  }
  static double BucketMs(int bucket) { return (double(bucket) + 0.5) / kPerMs; }  // the middle of the bucket

  void Add(int bucket) {
    ++counts_[size_t(bucket)];
    ++total_;
  }
  void Remove(int bucket) {
    --counts_[size_t(bucket)];
    --total_;
  }
  void Clear() {
    counts_.fill(0);
    total_ = 0;
  }
  uint32_t Count() const { return total_; }
  // Nearest-rank percentile, p in (0, 1]; -1 when empty.
  double Percentile(double p) const {
    if (total_ == 0) return -1.0;
    const uint32_t rank = std::max<uint32_t>(1, uint32_t(std::ceil(p * double(total_) - 1e-9)));
    uint32_t seen = 0;
    for (int b = 0; b < kBuckets; ++b) {
      seen += counts_[size_t(b)];
      if (seen >= rank) return BucketMs(b);
    }
    return BucketMs(kBuckets - 1);
  }

 private:
  std::array<uint32_t, kBuckets> counts_{};
  uint32_t total_ = 0;
};

// The samples of the last `keep` nanoseconds with a histogram over them.
class TimedWindow {
 public:
  void Add(int64_t now, double ms) {
    const int b = Histogram::BucketOf(ms);
    ring_.push_back({now, b});
    hist_.Add(b);
  }
  void Trim(int64_t now, int64_t keep_ns) {
    while (!ring_.empty() && now - ring_.front().ns > keep_ns) {
      hist_.Remove(ring_.front().bucket);
      ring_.pop_front();
    }
  }
  void Reset() {
    ring_.clear();
    hist_.Clear();
  }
  uint32_t Count() const { return hist_.Count(); }
  double Percentile(double p) const { return hist_.Percentile(p); }

 private:
  struct Item {
    int64_t ns;
    int bucket;
  };
  std::deque<Item> ring_;
  Histogram hist_;
};

// ---- the camera: cuts and speed (the pop-hiding sources) ---------------------------------------------------------------

// One call per engine frame with the camera's world position (metres). A jump of more than `cut_m` between two calls is a CUT (a
// teleport, a replay camera change, a respawn); otherwise the speed is the distance over the real time between the calls, smoothed
// (time constant 0.2 s), in km/h. -1 = unknown (the first call, or a gap of more than 0.5 s since the previous one).
class CameraMotion {
 public:
  struct Result {
    bool cut = false;
    double speed_kmh = -1.0;
  };

  Result Update(int64_t now_ns, double x, double y, double z, double cut_m) {
    Result r;
    if (!have_) {
      have_ = true;
      Store(now_ns, x, y, z);
      return r;
    }
    const double dist = std::sqrt((x - x_) * (x - x_) + (y - y_) * (y - y_) + (z - z_) * (z - z_));
    const double dt = double(now_ns - ns_) / 1e9;
    Store(now_ns, x, y, z);
    if (cut_m > 0.0 && dist > cut_m) {
      r.cut = true;
      r.speed_kmh = speed_kmh_;  // the speed before the cut stays: a jump says nothing about it
      return r;
    }
    if (!(dt > 0.0) || dt > kGapS) {
      speed_kmh_ = -1.0;
      return r;
    }
    const double inst = dist / dt * 3.6;
    speed_kmh_ = speed_kmh_ < 0.0 ? inst : speed_kmh_ + (1.0 - std::exp(-dt / kTauS)) * (inst - speed_kmh_);
    r.speed_kmh = speed_kmh_;
    return r;
  }
  double SpeedKmh() const { return speed_kmh_; }

 private:
  static constexpr double kTauS = 0.2;
  static constexpr double kGapS = 0.5;
  void Store(int64_t ns, double x, double y, double z) {
    ns_ = ns;
    x_ = x;
    y_ = y;
    z_ = z;
  }
  bool have_ = false;
  int64_t ns_ = 0;
  double x_ = 0, y_ = 0, z_ = 0;
  double speed_kmh_ = -1.0;
};

// ---- the ladder ----------------------------------------------------------------------------------------------------

constexpr int kMedianSamples = 4;                      // the shed statistic: the median of the last 4 GPU samples
constexpr int kMinMedianSamples = 3;                   // ... and it needs at least 3 of them
constexpr int kTailSamples = 30;                       // the tail rule looks at the last 30 GPU samples (0.5 s at 60 fps) ...
constexpr int kTailLagFrames = 4;                      // ... but not at the first 4 published frames after a step (they are the old state)
constexpr int kMissedHistoryFrames = 30;               // the missed-frames rule looks at this many engine frames ...
constexpr int kMissedShedCount = 2;                    // ... and sheds at this many missed ones
constexpr int64_t kSignalLiveNs = 1'000'000'000;       // no new sample for this long = no GPU signal
constexpr int64_t kBounceWindowNs = 2'000'000'000;     // a shed back to the rung left by a restore within this = a bounce
constexpr int64_t kFloorBounceWindowNs = 30'000'000'000;  // ... below the floor tier: within this (the visible rungs are held to a slower flap)
constexpr int64_t kBounceDecayNs = 30'000'000'000;     // a doubling of the restore delay decays this long after the delay it lengthened
constexpr double kMaxRestoreDelayS = 60.0;             // the cap of the doubled restore delay (never below restore_s itself)
constexpr int kMaxBounceDoublings = 6;
constexpr double kMinRestoreSamplesPerS = 10.0;        // a restore / an enrichment with a GPU signal needs this many samples a second
constexpr int64_t kDrivingResumeHideNs = 1'000'000'000;  // after a change of the driving flag this long is a hiding moment
constexpr int64_t kCutHideNs = 500'000'000;            // after a camera cut this long is a hiding moment
constexpr double kBackoffStartS = 30.0;                // the first back-off of a tier that bounced; below it nothing is left
constexpr double kBackoffCapS = 600.0;
constexpr int64_t kLearnSettleNs = 500'000'000;        // the first stretch after a step is not measured
constexpr int64_t kLearnLagNs = 100'000'000;           // the last stretch before a step is not measured (the export's lag)
constexpr double kLearnMaxMs = 5.0;                    // one learned sample is clamped to 0 .. this
constexpr double kLearnAlpha = 0.4;                    // the EMA weight of a new sample
constexpr double kLearnSpikeRatio = 1.5;               // a learning window with a sample above this x its median is discarded (a snap / hitch)
constexpr double kLearnMinSamplesPerS = 30.0;          // a learning window needs this many samples a second of its length
constexpr int kQuickLearnSamples = 3;                  // a step shed again before its window opened is measured from this many samples
constexpr int64_t kCompileExcuseWindowNs = 20'000'000'000;  // the excused time of compile bursts is capped within any stretch of this

// ---- the floor on auto -----------------------------------------------------------------------------------------------------

// The tier of the base rung for a preset (mcla_gfx_preset, case and surrounding blanks ignored): perf = 2 (perf + FXAA), perf+ / lowest = 1,
// quality and as shipped ('', default, none, shipped) = 4, ultra = 5; an unknown name (mcla_gfx_tune applies nothing for it) = 1, as before.
inline int FloorForPreset(const std::string& preset) {
  size_t b = 0, e = preset.size();
  while (b < e && std::isspace(static_cast<unsigned char>(preset[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(preset[e - 1]))) --e;
  std::string name = preset.substr(b, e - b);
  for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (name == "ultra") return 5;
  if (name.empty() || name == "quality" || name == "default" || name == "none" || name == "shipped") return 4;
  if (name == "perf") return 2;
  return kMinTier;  // perf+ (perfplus, perf_plus, perf-plus), lowest, and what is not a preset
}
// The floor with mcla_gov_tier_floor = -1: the tier mcla_gfx_tier was set to before the governor first ran (>= 1), else the preset's.
inline int AutoFloor(int configured_tier, const std::string& preset) {
  return configured_tier >= kMinTier ? std::min(configured_tier, kMaxTier) : FloorForPreset(preset);
}

// ---- compile bursts ----------------------------------------------------------------------------------------------------------

// The SDK's three words (rexgpu-xenos.dll, patch 0590): cumulative pipelines created, cumulative shaders translated at run time, and
// asynchronous pipeline creations in flight. `valid` = the symbols exist (an older SDK has none: no compile signal, nothing is excused).
struct CompileCounters {
  bool valid = false;
  uint32_t pipelines = 0;
  uint32_t shaders = 0;
  int32_t pending = 0;
};

// The compile-burst state: ACTIVE while a counter grew within the last `quiet` or a creation is in flight. A pure function of the
// numbers handed in once per engine frame. The first call only takes the counters' values (what was compiled before the first look is
// not a burst); the difference of a counter is taken modulo 2^32.
class CompileWatch {
 public:
  struct Result {
    bool active = false;
    bool started = false;                 // a burst began on this call (pipelines / shaders: what it has seen so far)
    bool ended = false;                   // a burst ended on this call: the fields describe the whole of it
    uint32_t pipelines = 0, shaders = 0;  // created / translated during the burst
    int64_t duration_ns = 0;              // from its start to the last call it was active (the quiet tail included)
  };

  Result Update(int64_t now_ns, const CompileCounters& c, int64_t quiet_ns) {
    Result r;
    if (!c.valid) {  // no signal: a burst that was running is over
      have_ = false;
      if (active_) r = Finish();
      r.active = false;
      return r;
    }
    uint32_t dp = 0, ds = 0;
    if (have_) {
      dp = c.pipelines - last_pipelines_;
      ds = c.shaders - last_shaders_;
    }
    have_ = true;
    last_pipelines_ = c.pipelines;
    last_shaders_ = c.shaders;
    if (dp != 0 || ds != 0) {
      have_growth_ = true;
      last_growth_ns_ = now_ns;
    }
    const bool recent = have_growth_ && now_ns - last_growth_ns_ <= std::max<int64_t>(0, quiet_ns);
    if (recent || c.pending > 0) {
      if (!active_) {
        active_ = true;
        start_ns_ = now_ns;
        pipelines_ = shaders_ = 0;
        r.started = true;
      }
      last_active_ns_ = now_ns;
      pipelines_ += dp;
      shaders_ += ds;
      r.pipelines = pipelines_;
      r.shaders = shaders_;
      r.duration_ns = now_ns - start_ns_;
    } else if (active_) {
      r = Finish();
    }
    r.active = active_;
    return r;
  }
  bool Active() const { return active_; }

 private:
  Result Finish() {
    Result r;
    r.ended = true;
    r.pipelines = pipelines_;
    r.shaders = shaders_;
    r.duration_ns = std::max<int64_t>(0, last_active_ns_ - start_ns_);
    active_ = false;
    return r;
  }
  bool have_ = false, have_growth_ = false, active_ = false;
  uint32_t last_pipelines_ = 0, last_shaders_ = 0;
  uint32_t pipelines_ = 0, shaders_ = 0;  // of the burst in progress
  int64_t last_growth_ns_ = 0, start_ns_ = 0, last_active_ns_ = 0;
};

// The excused time of compile bursts, capped within a sliding window: at most `cap` nanoseconds are charged in any `window` before now.
class ExcuseBudget {
 public:
  // The frame that ended at `now_ns` and lasted `frame_ns`: excused if budget is left (the charge is its length, at most what is left,
  // returned in *charged); false (nothing charged) when the window already holds `cap`.
  bool Excuse(int64_t now_ns, int64_t frame_ns, int64_t cap_ns, int64_t window_ns, int64_t* charged = nullptr) {
    Trim(now_ns, window_ns);
    if (charged) *charged = 0;
    const int64_t left = cap_ns - used_ns_;
    if (left <= 0) return false;
    const int64_t charge = std::clamp<int64_t>(frame_ns, 0, left);
    if (charge > 0) {
      charges_.push_back({now_ns, charge});
      used_ns_ += charge;
    }
    if (charged) *charged = charge;
    return true;
  }
  int64_t Used(int64_t now_ns, int64_t window_ns) {
    Trim(now_ns, window_ns);
    return used_ns_;
  }
  void Reset() {
    charges_.clear();
    used_ns_ = 0;
  }

 private:
  struct Charge {
    int64_t end_ns;
    int64_t ns;
  };
  void Trim(int64_t now_ns, int64_t window_ns) {
    while (!charges_.empty() && now_ns - charges_.front().end_ns > window_ns) {
      used_ns_ -= charges_.front().ns;
      charges_.pop_front();
    }
  }
  std::deque<Charge> charges_;
  int64_t used_ns_ = 0;
};

struct State {
  int tier = 1;
  int vrs = 0;
};
inline bool operator==(State a, State b) { return a.tier == b.tier && a.vrs == b.vrs; }
inline bool operator!=(State a, State b) { return !(a == b); }
inline int Key(State s) { return s.tier * 100 + s.vrs; }

// The live settings (cvars), handed to every Step so a change takes effect on the next frame.
struct Params {
  double shed_ms = 14.8;
  double restore_ms = 12.8;
  int shed_frames = 6;        // GPU frames between two changes before a shed
  int shed_over_count = 3;    // the tail rule: this many of the last 30 GPU samples over shed_ms sheds (0 = off)
  int floor_shed_frames = 30; // a shed below the floor tier: the median rule must hold this many GPU frames (see BELOW THE FLOOR); 0 = off
  double restore_s = 4.0;
  // the ladder
  int tier_floor = 1;         // F, 1..5: the configured tier (the base rung)
  int tier_ceiling = 4;       // C, 1..5: the richest tier enrichment may reach (C <= F: none)
  int vrs_levels = 2;         // V: VRS levels the ladder uses (0 = none)
  int max_level = -1;         // the highest rung the loop may shed to; < 0 = the whole ladder; 0 = never shed
  int force_level = -1;       // >= 0 pins that shed rung (clamped to the ladder; 0 = the base); < 0 = closed loop
  // enrichment
  double enrich_s = 10.0;
  double enrich_margin_ms = 1.5;
  double enrich_hold_s = 30.0;
  double learn_s = 3.0;       // 0 = no learning
  // the tier table (mcla_gfx_tune Tiers()): index = tier; the estimate is the ms over the tier below, pop_free = switching it does not show
  std::array<double, kTiers> tier_est_ms{-3.0, 0.0, 0.3, 0.4, 0.8, 1.0};
  std::array<bool, kTiers> tier_pop_free{false, true, true, false, true, false};
  // pop hiding
  double pop_wait_s = 1.5;
  double pop_speed_kmh = 90.0;
  // screen-change grace: engine frames after a change of the driving flag in which nothing is recorded and nothing is shed / restored /
  // enriched (0 = off: the previous screen's samples are not discarded either)
  int screen_grace_frames = 60;
  // compile bursts: seconds of excused time per 20 s while Input::compile_burst is set (0 = off: a burst is judged like any other time)
  double compile_excuse_s = 4.0;
};

// ---- the budget follows the frame-rate target (T6) ---------------------------------------------------------------------------
//
// shed_ms, restore_ms and enrich_margin_ms are written for the 60 fps frame (16.7 ms: shed above 15.6, restore below 15.0, a
// 1.5 ms enrichment margin). On the 1/120 grid (a cap above 60, the 120 fps target) the frame budget is 8.33 ms, and a GPU
// that needs 9.5 ms is over budget by 14 % although it is far under 15.6: left alone the governor would never act at 120 and
// the missed frames alone would send the world to its 60 tier. The thresholds therefore scale with the cap: the frame budget
// is scaled and the presenter's paint outside the measured GPU frame (about 0.7 ms) stays, so at 120: shed above 7.5 ms,
// restore below 7.2 ms, margin 0.75 ms. At a cap of 60 or below (or unknown, 0) nothing changes, bit for bit.
constexpr double kPresenterPaintMs = 0.7;
constexpr int kBudgetBaseFps = 60;
// 1 at a cap of 60 or below (and 0 = unknown); 60 / cap above it (0.5 at 120), never below 0.25.
inline double BudgetScale(int cap) {
  if (cap <= kBudgetBaseFps) return 1.0;
  return std::max(0.25, double(kBudgetBaseFps) / double(cap));
}
// A threshold in GPU frame ms written for the 60 fps frame, for a cap: the frame budget scales, the presenter's paint stays.
inline double ScaleThresholdMs(double ms, int cap) {
  const double scale = BudgetScale(cap);
  if (scale >= 1.0) return ms;
  return std::max(1.0, (ms + kPresenterPaintMs) * scale - kPresenterPaintMs);
}
// A margin written for the 60 fps frame: scales with the budget.
inline double ScaleMarginMs(double ms, int cap) { return ms * BudgetScale(cap); }

// The three thresholds of Params for a cap (the cvars hold the 60 fps values). A no-op at 60 or below.
inline void ScaleParamsForCap(Params& p, int cap) {
  p.shed_ms = ScaleThresholdMs(p.shed_ms, cap);
  p.restore_ms = ScaleThresholdMs(p.restore_ms, cap);
  p.enrich_margin_ms = ScaleMarginMs(p.enrich_margin_ms, cap);
}

// The shape of the ladder for one set of Params.
struct Ladder {
  int floor = 1, ceiling = 1, vrs = 0;
  int enrich = 0;  // rungs above the base
  int shed = 0;    // rungs below the base: V + F - 1 (the last one is the last level)

  static Ladder Of(const Params& p) {
    Ladder l;
    l.floor = std::clamp(p.tier_floor, kMinTier, kMaxTier);
    l.ceiling = std::clamp(p.tier_ceiling, kMinTier, kMaxTier);
    l.vrs = std::max(0, p.vrs_levels);
    l.enrich = std::max(0, l.ceiling - l.floor);
    l.shed = l.vrs + (l.floor - 1);
    return l;
  }
  int TopTier() const { return floor + enrich; }
  // The rung of a state on the ladder (a state off it is read as its nearest rung).
  int Rung(State s) const {
    if (s.tier > floor) return -(s.tier - floor);
    if (s.tier == floor) return std::clamp(s.vrs, 0, vrs);
    return vrs + (floor - s.tier);
  }
  State At(int rung) const {
    rung = std::clamp(rung, -enrich, shed);
    if (rung <= 0) return State{floor - rung, 0};
    if (rung <= vrs) return State{floor, rung};
    return State{floor - (rung - vrs), vrs};
  }
  bool On(State s) const {
    if (s.tier < kMinTier || s.tier > TopTier()) return false;
    if (s.tier > floor) return s.vrs == 0;
    if (s.tier == floor) return s.vrs >= 0 && s.vrs <= vrs;
    return s.vrs == vrs;
  }
  // The nearest state on the ladder: the tier is kept when it can be (a tier below the floor with VRS short of its maximum
  // gets the maximum: shedding more, never enriching, on a settings change).
  State Clamp(State s) const {
    s.tier = std::clamp(s.tier, kMinTier, TopTier());
    s.vrs = std::clamp(s.vrs, 0, vrs);
    if (s.tier > floor) s.vrs = 0;
    if (s.tier < floor) s.vrs = vrs;
    return s;
  }
  int MaxRung(const Params& p) const { return p.max_level >= 0 ? std::min(p.max_level, shed) : shed; }
  // A step between these two states changes the tier across one with pop_free = false.
  static bool Popping(State a, State b, const Params& p) {
    if (a.tier == b.tier) return false;
    const int hi = std::clamp(std::max(a.tier, b.tier), 0, kTiers - 1);
    return !p.tier_pop_free[size_t(hi)];
  }
};

// ---- the control law -----------------------------------------------------------------------------------------------

struct Input {
  int64_t now_ns = 0;
  bool driving = true;
  bool missed = false;      // the previous engine frame needed n >= 2 (last_n >= 2)
  bool new_sample = false;  // a GPU frame time was published since the previous call
  double gpu_ms = 0.0;      // the latest published GPU frame time (read when new_sample)
  uint32_t seq_delta = 0;   // frames published since the previous call (read when new_sample; >= 1)
  int lag = 0;              // how many frames old the published value is (for the summary only)
  bool cut = false;         // the camera jumped more than cut_m this frame (a hiding moment)
  double speed_kmh = -1.0;  // the camera's speed, km/h; < 0 = unknown
  bool compile_burst = false;  // pipelines / shaders are being compiled (CompileWatch::Active): see COMPILE BURST
};

enum class Reason { kNone, kGpuMedian, kGpuTail, kMissedFrames, kRestore, kEnrich, kForced, kCap, kRange };
enum class Kind { kNone, kShed, kRestore, kEnrich, kOther };
// What let a step across a popping tier go.
enum class Hide { kNone, kNotDriving, kDrivingChange, kCut, kSpeed, kMissed, kTimeout };

inline const char* ReasonText(Reason r) {
  switch (r) {
    case Reason::kGpuMedian: return "gpu median over the shed line";
    case Reason::kGpuTail: return "gpu tail over the shed line";
    case Reason::kMissedFrames: return "missed frames";
    case Reason::kRestore: return "restore";
    case Reason::kEnrich: return "enrich";
    case Reason::kForced: return "forced";
    case Reason::kCap: return "level cap";
    case Reason::kRange: return "floor / ceiling / vrs changed";
    case Reason::kNone: break;
  }
  return "";
}
inline const char* HideText(Hide h) {
  switch (h) {
    case Hide::kNotDriving: return "not driving";
    case Hide::kDrivingChange: return "driving flag changed";
    case Hide::kCut: return "camera cut";
    case Hide::kSpeed: return "speed";
    case Hide::kMissed: return "missed frame";
    case Hide::kTimeout: return "wait over";
    case Hide::kNone: break;
  }
  return "";
}

// What a Step did, for the log line:
//   `[mcla-gov] tier a -> b / vrs a -> b (<reason>: busy <stat_name> x.x ms of the last n, predicted +x.x ms, learned ..., missed m)`.
struct Change {
  bool changed = false;       // a step was taken (from_* / to_* are the step)
  bool wait_started = false;  // a step now waits for a hiding moment (from_* / to_* are the planned step; changed is false)
  int from = 0, to = 0;       // rungs
  int from_tier = 0, to_tier = 0, from_vrs = 0, to_vrs = 0;
  Kind kind = Kind::kNone;
  Reason reason = Reason::kNone;
  const char* stat_name = "";  // "p50" (the median of the last samples), "tail" (samples over the line; stat_ms = the largest), "missed", "p90" (restore, enrich), "" = none
  double stat_ms = -1.0;
  int stat_n = 0;              // samples the statistic is over (tail: the number over the line, missed: the missed frames)
  int stat_window = 0;         // ... out of this many (the tail window, the missed-frames window)
  int missed = 0;              // of the last 8 engine frames
  bool bounce = false;         // a shed that is the bounce of a restore probe
  bool below_floor = false;    // a shed to a tier under the floor (judged by the median held floor_shed_frames, or the missed frames)
  int sustained_frames = 0;    // ... the GPU frames the median had been over the line when it fired (the median rule)
  bool signal_gpu = false;
  double est_ms = -1.0;        // enrich: the table's estimate of the tier entered
  double learned_ms = -1.0;    // enrich: the learned cost of the tier entered (-1 = none yet)
  double predicted_ms = -1.0;  // enrich: max of the two
  double backoff_s = 0.0;      // a shed of an enriched tier that bounced: its new back-off
  double waited_s = 0.0;       // a step that waited for a hiding moment: how long
  Hide hide = Hide::kNone;     // what released a step across a popping tier
  // A tier step's learning finished in this call (independent of `changed`).
  bool learn_done = false;
  int learn_tier = 0;          // the tier whose cost over the one below was measured
  double learn_delta_ms = 0.0, learn_before_ms = 0.0, learn_after_ms = 0.0;  // the clamped sample and the two p50s
  int learn_n_before = 0, learn_n_after = 0;
  // A tier step's measurement was thrown away: a sample above 1.5x the window's median (a scene snap, a hitch).
  bool learn_discarded = false;
  int learn_discard_tier = 0;
  bool learn_discard_before = false;  // the window before the step (else the one after it)
  double learn_discard_p50_ms = 0.0, learn_discard_max_ms = 0.0;
};

// One summary window (mcla_gov_log_s seconds).
struct Window {
  int64_t start_ns = 0;
  int64_t length_ns = 0;
  int min_level = 0, max_level = 0;  // rungs
  std::map<int, int64_t> time_ns;    // time at each state (Key(State))
  int sheds = 0, restores = 0, enriches = 0, bounces = 0, waits = 0;
  uint32_t missed = 0;               // engine frames missed
  uint32_t samples = 0;              // GPU samples
  uint32_t grace_frames = 0;         // engine frames spent in a screen-change grace
  uint32_t compile_frames = 0;       // engine frames excused because of a compile burst
  int64_t compile_excused_ns = 0;    // ... and the time charged for them
  uint32_t compile_denied = 0;       // engine frames inside a compile burst that were judged anyway (the budget was spent)
  double p50_ms = -1.0, p95_ms = -1.0;
  int lag = 0;                       // the largest lag seen
  bool signal_gpu = false;           // at the end of the window
};

class Controller {
 public:
  State Current() const { return state_; }
  int Tier() const { return state_.tier; }
  int Vrs() const { return state_.vrs; }
  // The rung of the state as of the last Step (0 = the base, > 0 shed, < 0 enriched).
  int Level() const { return level_; }
  bool SignalLive() const { return signal_live_; }
  double LastSampleMs() const { return have_sample_ ? last_sample_ms_ : 0.0; }
  uint32_t FramesSinceChange() const { return frames_since_change_; }
  int64_t WindowStartNs() const { return win_start_ns_; }
  bool Waiting() const { return plan_.active; }  // a step waits for a hiding moment
  bool InGrace() const { return in_grace_; }     // the last Step was inside a screen-change grace (nothing recorded, nothing decided)
  int GraceFramesLeft() const { return grace_left_; }  // frames of the grace still to come after the last Step
  bool CompileExcused() const { return excused_; }         // the last Step excused its frame (a compile burst with budget left)
  bool CompileDenied() const { return compile_denied_; }   // the last Step was inside a compile burst but judged anyway (the budget is spent)
  // The excused time still charged within the last 20 s as of now_ns (the budget used).
  int64_t CompileExcusedNs(int64_t now_ns) { return excuse_.Used(now_ns, kCompileExcuseWindowNs); }
  // The excused time since the controller was made (monotonic): the difference of two readings is what a stretch of time was charged.
  int64_t CompileExcusedTotalNs() const { return compile_excused_total_ns_; }

  // The 60 -> 30 hold may start: at the last rung (the cap, or tier 1 with VRS at its maximum) and still over budget (or, with no GPU
  // signal, at the last rung).
  bool HoldAllowed(const Params& p) const {
    const Ladder lad = Ladder::Of(p);
    if (lad.Rung(lad.Clamp(state_)) < lad.MaxRung(p)) return false;
    return !signal_live_ || last_sample_ms_ > p.shed_ms;
  }

  // The doublings of the restore delay out of `state` still in force at `now_ns` (0 = none).
  int Doublings(State state, int64_t now_ns) const {
    const auto it = bounce_.find(Key(state));
    if (it == bounce_.end()) return 0;
    const int64_t steps = std::max<int64_t>(0, now_ns - it->second.decay_from_ns) / kBounceDecayNs;
    return int(std::max<int64_t>(0, it->second.doublings - steps));
  }
  // How long the quiet time before a restore out of `state` is.
  double RestoreDelayS(State state, int64_t now_ns, const Params& p) const {
    const double base = std::max(0.1, p.restore_s);
    const double cap = std::max(kMaxRestoreDelayS, base);
    return std::min(cap, base * double(1 << Doublings(state, now_ns)));
  }

  // The cost of tier k over k-1 the enrichment rule plans with: the table's estimate, or what was learned when that is more.
  double PredictedCostMs(int tier, const Params& p) const {
    if (tier < 0 || tier >= kTiers) return 0.0;
    const double est = p.tier_est_ms[size_t(tier)];
    return learned_n_[size_t(tier)] > 0 ? std::max(est, learned_ms_[size_t(tier)]) : est;
  }
  bool HasLearned(int tier) const { return tier >= 0 && tier < kTiers && learned_n_[size_t(tier)] > 0; }
  // Measurements thrown away because of a spike in their window (since the controller was made).
  int LearnDiscarded() const { return learn_discards_; }
  // The GPU frames the median of the last 4 samples has been over the shed line without a break (0 = not over / not counting).
  uint32_t OverloadFrames() const { return over_armed_ ? over_frames_ : 0u; }
  double LearnedMs(int tier) const { return HasLearned(tier) ? learned_ms_[size_t(tier)] : -1.0; }
  int LearnedCount(int tier) const { return tier >= 0 && tier < kTiers ? learned_n_[size_t(tier)] : 0; }

  // The back-off of `tier`: the period still in force (after its decay), 0 = none; and whether enrichment into it is barred now.
  double BackoffPeriodS(int tier, int64_t now_ns) const {
    if (tier < 0 || tier >= kTiers) return 0.0;
    const Backoff& b = backoff_[size_t(tier)];
    double period = b.period_s;
    int64_t t = b.decay_from_ns;
    while (period >= kBackoffStartS) {
      const int64_t step = int64_t(period * 1e9);
      if (now_ns - t < step) break;
      t += step;
      period *= 0.5;
    }
    return period >= kBackoffStartS ? period : 0.0;
  }
  bool BackedOff(int tier, int64_t now_ns) const {
    return tier >= 0 && tier < kTiers && now_ns < backoff_[size_t(tier)].until_ns;
  }
  double BackoffLeftS(int tier, int64_t now_ns) const {
    return BackedOff(tier, now_ns) ? double(backoff_[size_t(tier)].until_ns - now_ns) / 1e9 : 0.0;
  }

  Change Step(const Input& in, const Params& p) {
    const int64_t now = in.now_ns;
    const Ladder lad = Ladder::Of(p);
    int64_t frame_ns = 0;  // the time since the previous call: what an excused frame is charged
    if (!started_) {
      started_ = true;
      win_start_ns_ = last_now_ns_ = quiet_start_ns_ = now;
      driving_ = in.driving;
      state_ = State{lad.floor, 0};
      win_.min_level = win_.max_level = lad.Rung(state_);
    } else {
      frame_ns = std::max<int64_t>(0, now - last_now_ns_);
      win_.time_ns[Key(state_)] += frame_ns;
      last_now_ns_ = now;
    }

    // The screen-change grace: a change of the driving flag throws away what was recorded on the previous screen and opens a stretch of
    // frames in which nothing is recorded (and nothing is decided, see ClosedLoop). The change frame is the first frame of the grace.
    const int grace_len = std::max(0, p.screen_grace_frames);
    if (in.driving != driving_ && grace_len > 0) {
      DiscardRecords();
      grace_left_ = grace_len;
    }
    grace_left_ = std::min(grace_left_, grace_len);  // switched off or shortened live
    in_grace_ = grace_left_ > 0;
    if (in_grace_) {
      --grace_left_;
      ++win_.grace_frames;
    }

    // A compile burst: while driving (outside a grace) and with budget left, this frame is EXCUSED (see COMPILE BURST).
    excused_ = false;
    compile_denied_ = false;
    if (in.compile_burst && p.compile_excuse_s > 0.0 && in.driving && !in_grace_) {
      int64_t charged = 0;
      excused_ = excuse_.Excuse(now, frame_ns, int64_t(p.compile_excuse_s * 1e9), kCompileExcuseWindowNs, &charged);
      compile_denied_ = !excused_;
      if (excused_) {
        ++win_.compile_frames;
        win_.compile_excused_ns += charged;
        compile_excused_total_ns_ += charged;
      } else {
        ++win_.compile_denied;
      }
    }

    // The engine frame: the missed history.
    if (!in.driving || in_grace_) {
      missed_history_ = 0u;  // not driving, grace: nothing counts
    } else if (!excused_) {
      missed_history_ = ((missed_history_ << 1) | (in.missed ? 1u : 0u)) & kMissedMask;  // an excused frame is not recorded: the history stays as it was
    }
    if (in.missed) ++win_.missed;

    // The GPU sample.
    const bool sample = in.new_sample && std::isfinite(in.gpu_ms) && in.gpu_ms > 0.0;
    if (sample && !in_grace_ && !excused_) {
      recent_[size_t(recent_next_)] = in.gpu_ms;
      recent_next_ = (recent_next_ + 1) % kMedianSamples;
      recent_count_ = std::min(recent_count_ + 1, kMedianSamples);
      TrackOverload(in, p);
    }
    if (sample) {
      last_sample_ms_ = in.gpu_ms;
      last_sample_ns_ = now;
      have_sample_ = true;
      win_hist_.Add(Histogram::BucketOf(in.gpu_ms));
      ++win_.samples;
    }
    win_.lag = std::max(win_.lag, in.lag);
    signal_live_ = have_sample_ && now - last_sample_ns_ <= kSignalLiveNs;
    if (!signal_live_) {  // a median of samples that are no longer coming says nothing
      over_armed_ = false;
      over_frames_ = 0;
    }
    // The spacing of changes counts GPU frames while the signal is live (the published count), engine frames without it.
    const uint32_t frames = signal_live_ ? (sample ? std::max<uint32_t>(1, in.seq_delta) : 0u) : 1u;
    frames_since_change_ = frames_since_change_ > UINT32_MAX - frames ? UINT32_MAX : frames_since_change_ + frames;
    // The tail: the samples taken while driving, once the step before them has shown (the export is 2-4 frames behind).
    if (sample && in.driving && !in_grace_ && !excused_ && frames_since_change_ > uint32_t(kTailLagFrames)) {
      tail_[size_t(tail_next_)] = in.gpu_ms;
      tail_next_ = (tail_next_ + 1) % kTailSamples;
      tail_count_ = std::min(tail_count_ + 1, kTailSamples);
    }

    // The camera: a cut and the speed (the hiding moments).
    if (in.cut) cut_until_ns_ = now + kCutHideNs;
    speed_kmh_ = in.speed_kmh;

    // The driving flag. A step that waits for a hiding moment goes when the player leaves the scene.
    Change out;
    if (in.driving != driving_) {
      driving_ = in.driving;
      driving_change_ns_ = now;
      tail_count_ = tail_next_ = 0;  // the frames of a menu or a load are not the drive's
      if (!in.driving && plan_.active && PlanValid(lad, p)) out = ApplyPlan(now, p, lad, Hide::kNotDriving);
      ResetQuiet(now);
    }
    // The quiet timer: a frame that is not driving, a missed frame (outside a grace) and an excused frame (a burst is not calm either)
    // start it over.
    if (!in.driving || (in.missed && !in_grace_) || excused_) ResetQuiet(now);
    const double delay_s = RestoreDelayS(state_, now, p);
    const bool valid = sample && in.driving && !in.missed && !in_grace_ && !excused_;  // a sample the learning may use
    if (sample && in.driving && !in_grace_ && !excused_) {
      restore_win_.Add(now, in.gpu_ms);
      enrich_win_.Add(now, in.gpu_ms);
    }
    restore_win_.Trim(now, int64_t(delay_s * 1e9));
    enrich_win_.Trim(now, int64_t(std::max(0.1, p.enrich_s) * 1e9));
    const Change learn = FeedLearn(now, in, valid, p);

    if (!out.changed) {
      if (!lad.On(state_)) {
        out = Move(lad.Clamp(state_), Reason::kRange, p, lad, now);
      } else if (p.force_level >= 0) {
        const State target = lad.At(std::min(p.force_level, lad.shed));
        plan_ = Plan();
        if (state_ != target) out = Move(target, Reason::kForced, p, lad, now);
      } else if (lad.Rung(state_) > lad.MaxRung(p)) {
        out = Move(lad.At(lad.MaxRung(p)), Reason::kCap, p, lad, now);
      } else {
        out = ClosedLoop(in, p, lad, now, delay_s);
      }
    }
    level_ = lad.Rung(lad.Clamp(state_));
    out.signal_gpu = signal_live_;
    if (learn.learn_discarded && !out.learn_discarded) {
      out.learn_discarded = true;
      out.learn_discard_tier = learn.learn_discard_tier;
      out.learn_discard_before = learn.learn_discard_before;
      out.learn_discard_p50_ms = learn.learn_discard_p50_ms;
      out.learn_discard_max_ms = learn.learn_discard_max_ms;
    }
    if (learn.learn_done) {
      out.learn_done = true;
      out.learn_tier = learn.learn_tier;
      out.learn_delta_ms = learn.learn_delta_ms;
      out.learn_before_ms = learn.learn_before_ms;
      out.learn_after_ms = learn.learn_after_ms;
      out.learn_n_before = learn.learn_n_before;
      out.learn_n_after = learn.learn_n_after;
    }
    return out;
  }

  // The summary of the window since the last call (or the first Step); starts the next one.
  Window TakeWindow(int64_t now_ns) {
    Window w = win_;
    w.start_ns = win_start_ns_;
    w.length_ns = std::max<int64_t>(0, now_ns - win_start_ns_);
    w.p50_ms = win_hist_.Percentile(0.50);
    w.p95_ms = win_hist_.Percentile(0.95);
    w.signal_gpu = signal_live_;
    win_ = Window();
    win_.min_level = win_.max_level = level_;
    win_hist_.Clear();
    win_start_ns_ = now_ns;
    return w;
  }

 private:
  struct Bounce {
    int doublings = 0;
    int64_t decay_from_ns = 0;  // the bounce's time plus the delay it set: the doublings decay from here, one per 30 s
  };
  struct Backoff {
    double period_s = 0.0;      // the wait the last bounce set (before its decay)
    int64_t until_ns = 0;       // enrichment into the tier is barred until here
    int64_t decay_from_ns = 0;  // the period halves one step per period from here (= until_ns)
  };
  struct Plan {  // a step that waits for a hiding moment
    bool active = false;
    Kind kind = Kind::kNone;
    State to;
    int64_t since_ns = 0;
    Change info;  // reason, statistic, prediction of the step
  };
  struct LearnSample {
    int64_t ns;
    int bucket;
    int key;
  };
  struct Learn {  // the measurement of one tier step in progress
    bool active = false;
    int from_tier = 0, to_tier = 0;
    int64_t change_ns = 0;
    double before_ms = 0.0;
    int before_n = 0;
    std::vector<int> after;  // the valid samples from 0.5 s to 3.5 s after the step
    std::vector<int> quick;  // the valid samples from 0.1 s after the step on (for a step that is shed again before the window opens)
  };

  static int PopCount(uint32_t v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
  }

  void ResetQuiet(int64_t now) {
    quiet_start_ns_ = now;
    restore_win_.Reset();
    enrich_win_.Reset();
  }
  // The previous screen's records go (the screen-change grace): every window the shed / restore / enrich / learning rules read.
  void DiscardRecords() {
    recent_count_ = recent_next_ = 0;
    tail_count_ = tail_next_ = 0;
    missed_history_ = 0;
    over_armed_ = false;
    over_frames_ = 0;
    restore_win_.Reset();
    enrich_win_.Reset();
    learn_ring_.clear();
    learn_.active = false;
    learn_.after.clear();
    learn_.quick.clear();
  }
  double Median() const {
    std::array<double, kMedianSamples> v{};
    for (int i = 0; i < recent_count_; ++i) v[size_t(i)] = recent_[size_t(i)];  // the ring holds the last `count` in slots 0..count-1 until full
    std::sort(v.begin(), v.begin() + recent_count_);
    const int n = recent_count_;
    return (n % 2) ? v[size_t(n / 2)] : 0.5 * (v[size_t(n / 2 - 1)] + v[size_t(n / 2)]);
  }
  // The run of the median rule (BELOW THE FLOOR): called with every sample that entered the median ring. The run starts at the first
  // sample whose median (of at least 3) is above the line, counts the GPU frames after it, and ends at the first one that is not.
  void TrackOverload(const Input& in, const Params& p) {
    if (!in.driving || recent_count_ < kMinMedianSamples || !(Median() > p.shed_ms)) {
      over_armed_ = false;
      over_frames_ = 0;
      return;
    }
    if (!over_armed_) {
      over_armed_ = true;
      over_frames_ = 0;
      return;
    }
    const uint32_t frames = std::max<uint32_t>(1, in.seq_delta);
    over_frames_ = over_frames_ > UINT32_MAX - frames ? UINT32_MAX : over_frames_ + frames;
  }
  // The median rule holds for a shed below the floor: over the line for floor_shed_frames GPU frames (0 = the rule is off, so it always does).
  bool OverloadHeld(const Params& p) const {
    return p.floor_shed_frames <= 0 || (over_armed_ && over_frames_ >= uint32_t(p.floor_shed_frames));
  }

  // ---- hiding moments

  Hide HidingNow(int64_t now, const Params& p) const {
    if (!driving_) return Hide::kNotDriving;
    if (now - driving_change_ns_ < kDrivingResumeHideNs) return Hide::kDrivingChange;
    if (now < cut_until_ns_) return Hide::kCut;
    if (speed_kmh_ >= 0.0 && speed_kmh_ > p.pop_speed_kmh) return Hide::kSpeed;
    return Hide::kNone;
  }

  // ---- steps

  Change Fill(Kind kind, State to, const Change& info, const Ladder& lad) const {
    Change c = info;
    c.kind = kind;
    c.from_tier = state_.tier;
    c.from_vrs = state_.vrs;
    c.to_tier = to.tier;
    c.to_vrs = to.vrs;
    c.from = lad.Rung(state_);
    c.to = lad.Rung(to);
    c.missed = PopCount(missed_history_);
    return c;
  }

  // Takes the step: the bookkeeping of enrichment, bounces and learning, then the state.
  Change Apply(Kind kind, State to, const Change& info, const Params& p, const Ladder& lad, int64_t now) {
    const State from = state_;
    Change c = Fill(kind, to, info, lad);
    c.changed = true;
    c.wait_started = false;
    // A tier step: the enrichment record of the tier left, the back-off of an enriched tier shed too soon.
    if (to.tier != from.tier) {
      if (kind == Kind::kShed && to.tier < from.tier) NoteEnrichedShed(from.tier, now, p, &c);
      enriched_[size_t(from.tier)] = false;
      if (kind == Kind::kEnrich && to.tier > from.tier) {
        enriched_[size_t(to.tier)] = true;
        enriched_at_ns_[size_t(to.tier)] = now;
      } else {
        enriched_[size_t(to.tier)] = false;
      }
    }
    // The probe bounce of a restore: a shed back to the state a restore left within 2 s (below the floor: within 30 s).
    if (kind == Kind::kShed) {
      bool bounce = has_restore_ && restore_from_key_ == Key(to) && now - restore_ns_ <= kBounceWindowNs;
      if (to.tier < lad.floor) {  // a rung below the floor: a restore from it within 30 s, whatever came between, is a bounce too
        const auto it = floor_restore_ns_.find(Key(to));
        if (it != floor_restore_ns_.end()) {
          bounce = bounce || now - it->second <= kFloorBounceWindowNs;
          floor_restore_ns_.erase(it);
        }
      }
      if (bounce) {
        const int doublings = std::min(Doublings(to, now) + 1, kMaxBounceDoublings);
        Bounce& b = bounce_[Key(to)];
        b.doublings = doublings;
        b.decay_from_ns = now + int64_t(RestoreDelayS(to, now, p) * 1e9);
        ++win_.bounces;
        c.bounce = true;
      }
      has_restore_ = false;
    } else if (kind == Kind::kRestore) {
      has_restore_ = true;
      restore_from_key_ = Key(from);
      restore_ns_ = now;
      if (from.tier < lad.floor) floor_restore_ns_[Key(from)] = now;
    } else {
      has_restore_ = false;
      floor_restore_ns_.clear();
    }
    // A tier shed again before its measurement window opened (one that costs far more than planned is over the shed line within a few
    // frames): the median of the last 4 samples that made the shed is the measurement, or else the samples taken since the step, from
    // the 0.1 s the export lags, if there are at least 3.
    if (learn_.active && kind == Kind::kShed && from.tier == learn_.to_tier && to.tier == learn_.from_tier) {
      if (info.reason == Reason::kGpuMedian && info.stat_n >= kMinMedianSamples) {
        FoldLearn(info.stat_ms, info.stat_n, &c);
      } else if (int(learn_.quick.size()) >= kQuickLearnSamples) {
        FoldLearn(learn_.quick, &c);
      }
    }
    // The cost of a tier step is measured (a step that is not a tier step, or one taken outside driving, is not).
    learn_.active = false;
    if (to.tier != from.tier && driving_) StartLearn(from, to, now, p, &c);

    state_ = to;
    frames_since_change_ = 0;
    missed_history_ = 0;  // the frames before a step are the old state's: the shed rules start counting again
    tail_count_ = tail_next_ = 0;
    ResetQuiet(now);
    plan_ = Plan();
    win_.min_level = std::min(win_.min_level, c.to);
    win_.max_level = std::max(win_.max_level, c.to);
    switch (kind) {
      case Kind::kShed: ++win_.sheds; break;
      case Kind::kRestore: ++win_.restores; break;
      case Kind::kEnrich: ++win_.enriches; break;
      default: break;
    }
    return c;
  }

  // A forced / capped / out-of-range move (no waiting, no statistics).
  Change Move(State to, Reason reason, const Params& p, const Ladder& lad, int64_t now) {
    Change info;
    info.reason = reason;
    return Apply(Kind::kOther, to, info, p, lad, now);
  }

  bool PlanValid(const Ladder& lad, const Params& p) const {
    if (!lad.On(state_) || !lad.On(plan_.to)) return false;
    const int a = lad.Rung(state_), b = lad.Rung(plan_.to);
    if (std::abs(a - b) != 1) return false;
    if (plan_.kind == Kind::kShed) return b <= lad.MaxRung(p) && p.force_level < 0;
    return p.force_level < 0;
  }

  Change ApplyPlan(int64_t now, const Params& p, const Ladder& lad, Hide hide) {
    const Plan plan = plan_;
    Change c = Apply(plan.kind, plan.to, plan.info, p, lad, now);
    c.waited_s = double(now - plan.since_ns) / 1e9;
    c.hide = hide;
    return c;
  }

  // The step `kind` to `to` is decided. Across a popping tier it waits for a hiding moment unless one is here.
  Change TryStep(Kind kind, State to, const Change& info, const Input& in, const Params& p, const Ladder& lad, int64_t now) {
    const bool waiting_same = plan_.active && plan_.kind == kind && plan_.to == to;
    bool go = true;
    Hide release = Hide::kNone;
    if (Ladder::Popping(state_, to, p)) {
      const Hide hide = HidingNow(now, p);
      go = false;
      if (hide != Hide::kNone) {
        go = true;
        release = hide;
      } else if (kind == Kind::kShed && in.missed) {
        go = true;
        release = Hide::kMissed;
      } else if (kind == Kind::kShed && waiting_same && double(now - plan_.since_ns) >= p.pop_wait_s * 1e9) {
        go = true;
        release = Hide::kTimeout;
      }
    }
    if (go) {
      const double waited = waiting_same ? double(now - plan_.since_ns) / 1e9 : 0.0;
      Change c = Apply(kind, to, info, p, lad, now);
      c.waited_s = waited;
      c.hide = release;
      return c;
    }
    if (waiting_same) {
      plan_.info = info;  // fresh statistics, the same plan
      return Change();
    }
    plan_ = Plan{true, kind, to, now, info};
    ++win_.waits;
    Change w = Fill(kind, to, info, lad);
    w.wait_started = true;
    return w;
  }

  // ---- the three kinds of step

  struct Trigger {
    bool fired = false;
    Reason reason = Reason::kNone;
    const char* name = "";
    double ms = -1.0;
    int n = 0;
    int window = 0;
  };
  // below_floor: the shed goes to a tier under the floor (BELOW THE FLOOR): the median rule must have held, the tail rule is not heard.
  Trigger EvalShed(const Params& p, bool below_floor) const {
    Trigger t;
    if (signal_live_ && recent_count_ >= kMinMedianSamples) {
      const double median = Median();
      if (median > p.shed_ms && (!below_floor || OverloadHeld(p))) {
        return Trigger{true, Reason::kGpuMedian, "p50", median, recent_count_, recent_count_};
      }
    }
    if (!below_floor && signal_live_ && p.shed_over_count > 0) {
      int over = 0;
      double max_ms = 0.0;
      for (int i = 0; i < tail_count_; ++i) {
        if (tail_[size_t(i)] > p.shed_ms) {
          ++over;
          max_ms = std::max(max_ms, tail_[size_t(i)]);
        }
      }
      if (over >= p.shed_over_count) return Trigger{true, Reason::kGpuTail, "tail", max_ms, over, tail_count_};
    }
    if (PopCount(missed_history_) >= kMissedShedCount) {
      return Trigger{true, Reason::kMissedFrames, "missed", -1.0, PopCount(missed_history_), kMissedHistoryFrames};
    }
    return t;
  }

  Change ClosedLoop(const Input& in, const Params& p, const Ladder& lad, int64_t now, double delay_s) {
    // Not driving (a menu, a pause, a load): the rung is held. Loads hitch for reasons no step helps (streaming, pipeline
    // creation: run lk-gov2 shed 0 -> 4 on 25-35 ms loading frames), and the pause menu's cost is its UI.
    if (!in.driving) {
      plan_ = Plan();
      return Change();
    }
    // The first frames of a new screen (the grace) and the excused frames of a compile burst: nothing was recorded for them, so nothing is
    // decided. A step that already waits for a hiding moment is not touched here (it goes at the pause, or is judged again after).
    if (in_grace_ || excused_) return Change();
    const int r = lad.Rung(state_);

    // SHED
    if (r < lad.MaxRung(p) && frames_since_change_ >= uint32_t(std::max(1, p.shed_frames))) {
      const bool below_floor = p.floor_shed_frames > 0 && r + 1 > lad.vrs;  // the next rung is a tier under the floor
      const Trigger t = EvalShed(p, below_floor);
      if (t.fired) {
        Change info;
        info.below_floor = below_floor;
        info.sustained_frames = below_floor && t.reason == Reason::kGpuMedian ? int(over_frames_) : 0;
        info.reason = t.reason;
        info.stat_name = t.name;
        info.stat_ms = t.ms;
        info.stat_n = t.n;
        info.stat_window = t.window;
        return TryStep(Kind::kShed, lad.At(r + 1), info, in, p, lad, now);
      }
    }
    if (plan_.active && plan_.kind == Kind::kShed) plan_ = Plan();  // the trigger went away

    // RESTORE (below the base) and ENRICH (at or above it)
    bool want = false;
    Kind kind = Kind::kNone;
    State to = state_;
    Change info;
    if (!in.missed) {
      if (r > 0) {
        want = RestoreReady(in, p, now, delay_s, &info);
        kind = Kind::kRestore;
        to = lad.At(r - 1);
      } else if (state_.tier < lad.TopTier() && state_.vrs == 0) {
        to = State{state_.tier + 1, 0};
        want = EnrichReady(p, now, to.tier, &info);
        kind = Kind::kEnrich;
      }
    }
    if (!want) {
      if (plan_.active) plan_ = Plan();
      return Change();
    }
    return TryStep(kind, to, info, in, p, lad, now);
  }

  bool RestoreReady(const Input& in, const Params& p, int64_t now, double delay_s, Change* info) const {
    if (!in.driving || in.missed || now - quiet_start_ns_ < int64_t(delay_s * 1e9)) return false;
    info->reason = Reason::kRestore;
    if (!signal_live_) return true;  // no GPU frame time to judge by: the quiet time with no missed frame is the condition
    const uint32_t count = restore_win_.Count();
    const uint32_t need = uint32_t(std::ceil(delay_s * kMinRestoreSamplesPerS));
    const double p90 = restore_win_.Percentile(0.90);
    if (count >= need && p90 >= 0.0 && p90 < p.restore_ms) {
      info->stat_name = "p90";
      info->stat_ms = p90;
      info->stat_n = int(count);
      return true;
    }
    return false;
  }

  bool EnrichReady(const Params& p, int64_t now, int next_tier, Change* info) const {
    const double window_s = std::max(0.1, p.enrich_s);
    if (!signal_live_ || now - quiet_start_ns_ < int64_t(window_s * 1e9)) return false;
    if (BackedOff(next_tier, now)) return false;
    const uint32_t count = enrich_win_.Count();
    if (count < uint32_t(std::ceil(window_s * kMinRestoreSamplesPerS))) return false;
    const double p90 = enrich_win_.Percentile(0.90);
    const double predicted = PredictedCostMs(next_tier, p);
    if (!(p90 >= 0.0) || p90 + predicted + p.enrich_margin_ms > p.restore_ms) return false;
    info->reason = Reason::kEnrich;
    info->stat_name = "p90";
    info->stat_ms = p90;
    info->stat_n = int(count);
    info->est_ms = p.tier_est_ms[size_t(next_tier)];
    info->learned_ms = LearnedMs(next_tier);
    info->predicted_ms = predicted;
    return true;
  }

  // An enriched tier shed within enrich_hold_s of its entry: the back-off of that tier doubles.
  void NoteEnrichedShed(int tier, int64_t now, const Params& p, Change* c) {
    if (!enriched_[size_t(tier)] || double(now - enriched_at_ns_[size_t(tier)]) > p.enrich_hold_s * 1e9) return;
    Backoff& b = backoff_[size_t(tier)];
    const double current = BackoffPeriodS(tier, now);
    const double next = current <= 0.0 ? kBackoffStartS : std::min(kBackoffCapS, current * 2.0);
    b.period_s = next;
    b.until_ns = now + int64_t(next * 1e9);
    b.decay_from_ns = b.until_ns;
    c->backoff_s = next;
  }

  // ---- learning the cost of a tier

  void StartLearn(State from, State to, int64_t now, const Params& p, Change* c) {
    if (p.learn_s <= 0.0) return;
    // The p50 of the valid samples of the state left, from learn_s + the export's lag back to the lag.
    std::vector<int> before;
    const int64_t hi = now - kLearnLagNs, lo = hi - int64_t(p.learn_s * 1e9);
    for (const LearnSample& s : learn_ring_) {
      if (s.key == Key(from) && s.ns >= lo && s.ns <= hi) before.push_back(s.bucket);
    }
    if (int(before.size()) < LearnMinSamples(p)) return;
    double p50 = 0.0, max_ms = 0.0;
    if (Spiky(before, &p50, &max_ms)) {  // a snap or a hitch in the window before: the cost of the step is not measurable from it
      NoteLearnDiscard(c, std::max(from.tier, to.tier), true, p50, max_ms);
      return;
    }
    learn_.active = true;
    learn_.from_tier = from.tier;
    learn_.to_tier = to.tier;
    learn_.change_ns = now;
    learn_.before_ms = p50;
    learn_.before_n = int(before.size());
    learn_.after.clear();
    learn_.quick.clear();
  }
  static int LearnMinSamples(const Params& p) { return std::max(10, int(p.learn_s * kLearnMinSamplesPerS)); }

  Change FeedLearn(int64_t now, const Input& in, bool valid, const Params& p) {
    Change out;
    const int64_t keep = int64_t((std::max(0.0, p.learn_s) + 1.0) * 1e9);
    if (valid && p.learn_s > 0.0) learn_ring_.push_back({now, Histogram::BucketOf(in.gpu_ms), Key(state_)});
    while (!learn_ring_.empty() && now - learn_ring_.front().ns > keep) learn_ring_.pop_front();
    if (!learn_.active) return out;
    if (!in.driving || p.learn_s <= 0.0) {  // a pause changes the scene: the measurement is void
      learn_.active = false;
      return out;
    }
    const int64_t start = learn_.change_ns + kLearnSettleNs;
    const int64_t end = start + int64_t(p.learn_s * 1e9);
    if (valid && now >= learn_.change_ns + kLearnLagNs && now <= end) learn_.quick.push_back(Histogram::BucketOf(in.gpu_ms));
    if (valid && now >= start && now <= end) learn_.after.push_back(Histogram::BucketOf(in.gpu_ms));
    if (now <= end) return out;
    learn_.active = false;
    if (int(learn_.after.size()) >= LearnMinSamples(p)) FoldLearn(learn_.after, &out);
    return out;
  }

  // The measurement of the step in learn_ is `after` (bucket numbers; the median counts): the cost goes into the EMA of the tier.
  void FoldLearn(std::vector<int>& after, Change* out) {
    double p50 = 0.0, max_ms = 0.0;
    if (Spiky(after, &p50, &max_ms)) {  // a snap or a hitch in the window after the step
      NoteLearnDiscard(out, std::max(learn_.from_tier, learn_.to_tier), false, p50, max_ms);
      return;
    }
    FoldLearn(p50, int(after.size()), out);
  }
  // The median (ms) and the largest sample (ms) of a window of bucket numbers; true when the largest is above kLearnSpikeRatio x the median.
  static bool Spiky(std::vector<int>& buckets, double* p50_ms, double* max_ms) {
    std::nth_element(buckets.begin(), buckets.begin() + buckets.size() / 2, buckets.end());
    *p50_ms = Histogram::BucketMs(buckets[buckets.size() / 2]);
    *max_ms = Histogram::BucketMs(*std::max_element(buckets.begin(), buckets.end()));
    return *max_ms > kLearnSpikeRatio * *p50_ms;
  }
  void NoteLearnDiscard(Change* out, int tier, bool before, double p50_ms, double max_ms) {
    ++learn_discards_;
    if (out->learn_discarded) return;
    out->learn_discarded = true;
    out->learn_discard_tier = tier;
    out->learn_discard_before = before;
    out->learn_discard_p50_ms = p50_ms;
    out->learn_discard_max_ms = max_ms;
  }
  void FoldLearn(double after_ms, int n_after, Change* out) {
    const bool up = learn_.to_tier > learn_.from_tier;
    const int tier = std::max(learn_.from_tier, learn_.to_tier);
    const double sample = std::clamp(up ? after_ms - learn_.before_ms : learn_.before_ms - after_ms, 0.0, kLearnMaxMs);
    double& value = learned_ms_[size_t(tier)];
    int& n = learned_n_[size_t(tier)];
    value = n > 0 ? value + kLearnAlpha * (sample - value) : sample;
    ++n;
    out->learn_done = true;
    out->learn_tier = tier;
    out->learn_delta_ms = sample;
    out->learn_before_ms = learn_.before_ms;
    out->learn_after_ms = after_ms;
    out->learn_n_before = learn_.before_n;
    out->learn_n_after = n_after;
  }

  // ---- state

  bool started_ = false;
  State state_;
  int level_ = 0;
  int64_t last_now_ns_ = 0;
  bool driving_ = true;
  int64_t driving_change_ns_ = -(int64_t(1) << 60);  // no change seen yet
  int64_t cut_until_ns_ = -(int64_t(1) << 60);
  double speed_kmh_ = -1.0;
  uint32_t frames_since_change_ = UINT32_MAX;  // the first change is not held back
  int grace_left_ = 0;                         // frames of the screen-change grace still to come after the current one
  bool in_grace_ = false;                      // the current frame is inside it
  bool excused_ = false;                       // the current frame is excused (a compile burst with budget left)
  bool compile_denied_ = false;                // ... or was in a burst with the budget spent
  ExcuseBudget excuse_;
  int64_t compile_excused_total_ns_ = 0;

  static constexpr uint32_t kMissedMask = (1u << kMissedHistoryFrames) - 1u;
  uint32_t missed_history_ = 0;  // bit 0 = the latest engine frame; kMissedHistoryFrames bits; empty while not driving and after a step
  std::array<double, kTailSamples> tail_{};  // the last GPU samples taken while driving, since the last step (a ring: slots 0..count-1 until full)
  int tail_next_ = 0;
  int tail_count_ = 0;

  std::array<double, kMedianSamples> recent_{};
  int recent_next_ = 0;
  int recent_count_ = 0;
  double last_sample_ms_ = 0.0;
  int64_t last_sample_ns_ = 0;
  bool have_sample_ = false;
  bool signal_live_ = false;

  int64_t quiet_start_ns_ = 0;  // the last change, change of the driving flag, or missed frame
  TimedWindow restore_win_;
  TimedWindow enrich_win_;
  bool has_restore_ = false;
  int restore_from_key_ = 0;
  int64_t restore_ns_ = 0;
  std::map<int, Bounce> bounce_;
  std::map<int, int64_t> floor_restore_ns_;  // below the floor: the time of the last restore out of each rung (Key), for the 30 s bounce window
  bool over_armed_ = false;                  // the median of the last 4 samples is over the line, and has been since over_frames_ GPU frames ago
  uint32_t over_frames_ = 0;

  std::array<bool, kTiers> enriched_{};  // the tier was entered by enrichment ...
  std::array<int64_t, kTiers> enriched_at_ns_{};  // ... at this time
  std::array<Backoff, kTiers> backoff_{};
  std::array<double, kTiers> learned_ms_{};
  std::array<int, kTiers> learned_n_{};
  std::deque<LearnSample> learn_ring_;  // valid samples of the last learn_s + 1 s with the state they were taken in
  Learn learn_;
  int learn_discards_ = 0;
  Plan plan_;

  Window win_;
  Histogram win_hist_;
  int64_t win_start_ns_ = 0;
};

}  // namespace mcla::gov::math
