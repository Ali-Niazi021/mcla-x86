// Midnight Club: Los Angeles: the pure logic of the GPU frame governor (LOCK60 A5, mcla_gov.cpp). No SDK, no guest memory, no
// cvars, no clock: everything here is a function of numbers handed in (std only), so a unit test compiles it standalone
// (mcla/tests/test_gov.cpp). The glue (symbols, cvars, the log lines) is mcla_gov.cpp.
//
// Two parts:
//
//   1. The level table (ParseLevelTable). `mcla_gov_levels` is a string: levels separated by ';', each level a comma-separated
//      list of `name=value` actuator settings that HOLD at that level (absolute, not deltas; an actuator a level does not
//      mention is at its base value). `vrs=K` is the variable-rate-shading level; any other name is a cvar. Level 0 is the
//      configured picture and has no entry: the first entry of the string is level 1. A level whose effective settings equal
//      the previous kept level's (VRS unsupported turns `vrs=1;vrs=2` into two copies of nothing; a level that repeats its
//      predecessor) is skipped, so the level numbers are dense. The harness's command lines use `, ; =` themselves, so the
//      parser also takes '|' for ';', '+' for ',' and ':' for '=' (no exponent notation in values then: "1e+3" would split).
//
//   2. The control law (Controller). One Step per engine frame, fed one GPU sample per new published frame, the missed-frame
//      flag, the driving flag and the clock:
//        SHED (level + 1) when at least shed_frames GPU frames passed since the last level change, the level is below the cap,
//        and one of
//          a. the median of the last 4 GPU samples (at least 3 of them) is above shed_ms        ["gpu median"]
//          b. this engine frame was missed and the last GPU sample is above shed_ms - 1.0       ["missed frame at the budget"]
//          c. there is NO GPU signal (no sample for 1 s) and 2 of the last 8 engine frames were missed
//                                                                                              ["missed frames, no gpu signal"]
//        RESTORE (level - 1) when driving, the level is above the floor, and for the whole of the level's restore delay (a)
//        no frame was missed, (b) the player kept driving, (c) no level change happened, and (d) with a GPU signal the p90 of
//        the delay's GPU samples is below restore_ms (and there are at least 10 samples a second of it); with no GPU signal
//        (a/b/c alone) the quiet time is the whole condition. The delay is restore_s, doubled for every PROBE BOUNCE of that
//        level that has not decayed yet (cap 60 s).
//        A PROBE BOUNCE of level L = a restore from L followed within 2 s by a shed back to L. Each one doubles the delay of
//        the next restore from L (restore_s x 2^k, cap 60 s); the doublings decay by one step per 30 s without a new bounce,
//        counted from the end of the delay they lengthened (so a 32 s wait is not eaten by its own decay and the cap is
//        reachable). This is the guard against DVFS: with slack the GPU clocks down and the frame time rises, so a plain
//        threshold would restore, see the time rise, shed, and repeat.
//        Not driving (a menu, a pause, a load): the level is held (no shed, no restore), and the restore timer is reset on
//        every change of the driving flag and on every missed frame.
//        Pinning (Params::force_level >= 0) replaces the loop; a lowered cap or a closed rich range moves the level at once.
//   Levels below 0 (richer than the configured picture) are the same law mirrored: `Params::min_level` < 0 lets the restore
//   step go on below 0; it is 0 in v1 unless mcla_gov_allow_rich is set (the table of those levels is mcla_gov_rich_levels).
//
// Time is whatever steady clock the caller passes (nanoseconds); the samples carry no timestamps of their own, the call time
// stands in for them (the SDK's value is 2-4 frames old by design, see rexgpu_gpu_frame_lag).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mcla::gov::math {

// ---- the level table -----------------------------------------------------------------------------------------------

struct Setting {
  std::string name;
  std::string value;
};

// What holds at one level. `vrs`: -1 = not governed (the SDK's own cvar decides), 0 = full rate forced, K = level K.
// `cvars`: one entry per cvar name, in the order the table wrote them.
struct LevelSettings {
  int vrs = -1;
  std::vector<Setting> cvars;

  const Setting* Find(std::string_view name) const {
    for (const Setting& s : cvars) {
      if (s.name == name) return &s;
    }
    return nullptr;
  }
  bool Empty() const { return vrs < 0 && cvars.empty(); }
};

namespace detail {

inline std::string_view Trim(std::string_view t) {
  while (!t.empty() && (t.front() == ' ' || t.front() == '\t' || t.front() == '\r' || t.front() == '\n')) t.remove_prefix(1);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r' || t.back() == '\n')) t.remove_suffix(1);
  return t;
}

inline bool ParseInt(std::string_view t, long& out) {
  t = Trim(t);
  if (t.empty()) return false;
  const std::string text(t);
  char* end = nullptr;
  const long v = std::strtol(text.c_str(), &end, 10);
  if (end != text.c_str() + text.size()) return false;
  out = v;
  return true;
}

inline bool ParseDouble(std::string_view t, double& out) {
  t = Trim(t);
  if (t.empty()) return false;
  const std::string text(t);
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end != text.c_str() + text.size() || !std::isfinite(v)) return false;
  out = v;
  return true;
}

inline bool IsNameChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

inline bool EqualNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const char x = (a[i] >= 'A' && a[i] <= 'Z') ? char(a[i] - 'A' + 'a') : a[i];
    const char y = (b[i] >= 'A' && b[i] <= 'Z') ? char(b[i] - 'A' + 'a') : b[i];
    if (x != y) return false;
  }
  return true;
}

}  // namespace detail

// Two cvar values are the same setting: equal as numbers when both are numbers ("0.45" = "0.450000"), else equal text.
inline bool ValuesEqual(const std::string& a, const std::string& b) {
  double x = 0, y = 0;
  if (detail::ParseDouble(a, x) && detail::ParseDouble(b, y)) {
    return std::fabs(x - y) <= 1e-9 * std::max(1.0, std::max(std::fabs(x), std::fabs(y)));
  }
  return detail::EqualNoCase(detail::Trim(a), detail::Trim(b));
}

inline bool SameSettings(const LevelSettings& a, const LevelSettings& b) {
  if (a.vrs != b.vrs || a.cvars.size() != b.cvars.size()) return false;
  for (const Setting& s : a.cvars) {
    const Setting* other = b.Find(s.name);
    if (!other || !ValuesEqual(s.value, other->value)) return false;
  }
  return true;
}

// "vrs=2, mcla_gfx_env_interval=4" (what a log line shows for the level); "(base)" for a level that changes nothing.
inline std::string Describe(const LevelSettings& level) {
  std::string out;
  if (level.vrs >= 0) out = "vrs=" + std::to_string(level.vrs);
  for (const Setting& s : level.cvars) {
    if (!out.empty()) out += ", ";
    out += s.name + "=" + s.value;
  }
  return out.empty() ? std::string("(base)") : out;
}

struct ParsedTable {
  std::vector<LevelSettings> levels;  // the kept levels; levels[0] is level 1 (rich table: levels[0] is level -1)
  std::vector<int> skipped;           // 1-based positions in the string of the levels skipped as equal to the previous kept one
  std::vector<std::string> warnings;  // entries that were not understood (they are ignored)
  int source_levels = 0;              // levels the string had
  int vrs_ignored = 0;                // `vrs=` entries dropped because the GPU has no VRS levels
};

// `vrs_levels` = rexgpu_vrs_levels (0 = no VRS on this GPU or SDK: `vrs=K` entries are dropped; K above it is clamped).
inline ParsedTable ParseLevelTable(std::string_view text, int vrs_levels) {
  ParsedTable out;
  text = detail::Trim(text);
  if (text.empty()) return out;
  std::vector<std::string_view> pieces;
  size_t begin = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == ';' || text[i] == '|') {
      pieces.push_back(text.substr(begin, i - begin));
      begin = i + 1;
    }
  }
  if (!pieces.empty() && detail::Trim(pieces.back()).empty()) pieces.pop_back();  // a trailing ';'
  LevelSettings previous;                                                        // level 0: nothing changed
  int position = 0;
  for (std::string_view piece : pieces) {
    ++position;
    ++out.source_levels;
    LevelSettings level;
    size_t item_begin = 0;
    for (size_t i = 0; i <= piece.size(); ++i) {
      if (i != piece.size() && piece[i] != ',' && piece[i] != '+') continue;
      const std::string_view item = detail::Trim(piece.substr(item_begin, i - item_begin));
      item_begin = i + 1;
      if (item.empty()) continue;
      const size_t eq = item.find_first_of("=:");
      const std::string_view name = eq == std::string_view::npos ? std::string_view() : detail::Trim(item.substr(0, eq));
      const std::string_view value = eq == std::string_view::npos ? std::string_view() : detail::Trim(item.substr(eq + 1));
      bool name_ok = !name.empty();
      for (char c : name) name_ok = name_ok && detail::IsNameChar(c);
      if (!name_ok || value.empty()) {
        out.warnings.push_back("level " + std::to_string(position) + ": '" + std::string(item) + "' is not name=value, ignored");
        continue;
      }
      if (name == "vrs") {
        long k = 0;
        if (!detail::ParseInt(value, k)) {
          out.warnings.push_back("level " + std::to_string(position) + ": '" + std::string(item) + "': vrs needs a whole number, ignored");
        } else if (vrs_levels <= 0) {
          ++out.vrs_ignored;
        } else {
          level.vrs = k < 0 ? -1 : int(std::min<long>(k, vrs_levels));
        }
        continue;
      }
      bool replaced = false;
      for (Setting& s : level.cvars) {
        if (s.name == name) {
          s.value = std::string(value);
          replaced = true;
          out.warnings.push_back("level " + std::to_string(position) + ": '" + std::string(name) + "' set twice, the last value counts");
        }
      }
      if (!replaced) level.cvars.push_back({std::string(name), std::string(value)});
    }
    if (SameSettings(level, previous)) {
      out.skipped.push_back(position);
      continue;
    }
    out.levels.push_back(level);
    previous = out.levels.back();
  }
  return out;
}

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

// ---- the control law -----------------------------------------------------------------------------------------------

constexpr int kMedianSamples = 4;              // the shed statistic: the median of the last 4 GPU samples
constexpr int kMinMedianSamples = 3;           // ... and it needs at least 3 of them
constexpr double kMissedShedMarginMs = 1.0;    // a missed frame sheds when the last sample is above shed_ms minus this
constexpr int kMissedHistoryFrames = 8;        // the no-signal fallback looks at this many engine frames ...
constexpr int kMissedFallbackCount = 2;        // ... and sheds at this many missed ones
constexpr int64_t kSignalLiveNs = 1'000'000'000;       // no new sample for this long = no GPU signal
constexpr int64_t kBounceWindowNs = 2'000'000'000;     // a shed back to the level left by a restore within this = a bounce
constexpr int64_t kBounceDecayNs = 30'000'000'000;     // a doubling of the restore delay decays this long after the delay it lengthened
constexpr double kMaxRestoreDelayS = 60.0;             // the cap of the doubled restore delay (never below restore_s itself)
constexpr int kMaxBounceDoublings = 6;
constexpr double kMinRestoreSamplesPerS = 10.0;        // a restore with a GPU signal needs this many samples a second of its delay

// The live settings (cvars), handed to every Step so a change takes effect on the next frame.
struct Params {
  double shed_ms = 14.8;
  double restore_ms = 12.8;
  int shed_frames = 6;        // GPU frames between two level changes before a shed
  double restore_s = 4.0;
  int min_level = 0;          // the lowest level the loop may restore to (0, or minus the rich table's size)
  int max_level = 0;          // the highest level the loop may shed to (the table's size, capped by mcla_gov_max_level)
  int force_level = -1;       // >= 0 pins that level (already clamped to the table by the caller); < 0 = closed loop
};

struct Input {
  int64_t now_ns = 0;
  bool driving = true;
  bool missed = false;      // the previous engine frame needed n >= 2 (last_n >= 2)
  bool new_sample = false;  // a GPU frame time was published since the previous call
  double gpu_ms = 0.0;      // the latest published GPU frame time (read when new_sample)
  uint32_t seq_delta = 0;   // frames published since the previous call (read when new_sample; >= 1)
  int lag = 0;              // how many frames old the published value is (for the summary only)
};

enum class Reason { kNone, kGpuMedian, kMissedAtBudget, kMissedFrames, kRestore, kForced, kCap, kRichOff };

inline const char* ReasonText(Reason r) {
  switch (r) {
    case Reason::kGpuMedian: return "gpu median over the shed line";
    case Reason::kMissedAtBudget: return "missed frame at the budget";
    case Reason::kMissedFrames: return "missed frames, no gpu signal";
    case Reason::kRestore: return "restore";
    case Reason::kForced: return "forced";
    case Reason::kCap: return "level cap";
    case Reason::kRichOff: return "rich levels off";
    case Reason::kNone: break;
  }
  return "";
}

// What a Step did, for the log line: `[mcla-gov] level a -> b (<reason>: gpu <stat_name> x.x ms of the last n, missed m)`.
struct Change {
  bool changed = false;
  int from = 0, to = 0;
  Reason reason = Reason::kNone;
  const char* stat_name = "";  // "p50" (shed: the median of the last samples), "last" (a missed frame), "p90" (restore), "" = none
  double stat_ms = -1.0;
  int stat_n = 0;              // samples the statistic is over
  int missed = 0;              // of the last 8 engine frames
  bool bounce = false;         // a shed that is the bounce of a probe
  bool signal_gpu = false;
};

// One summary window (mcla_gov_log_s seconds).
struct Window {
  int64_t start_ns = 0;
  int64_t length_ns = 0;
  int min_level = 0, max_level = 0;
  std::map<int, int64_t> time_ns;  // time at each level
  int sheds = 0, restores = 0, bounces = 0;
  uint32_t missed = 0;             // engine frames missed
  uint32_t samples = 0;            // GPU samples
  double p50_ms = -1.0, p95_ms = -1.0;
  int lag = 0;                     // the largest lag seen
  bool signal_gpu = false;         // at the end of the window
};

class Controller {
 public:
  int Level() const { return level_; }
  bool SignalLive() const { return signal_live_; }
  double LastSampleMs() const { return have_sample_ ? last_sample_ms_ : 0.0; }
  uint32_t FramesSinceChange() const { return frames_since_change_; }
  int64_t WindowStartNs() const { return win_start_ns_; }

  // The 60 -> 30 hold may start: at the last level and still over budget (or, with no GPU signal, at the last level).
  bool HoldAllowed(const Params& p) const {
    if (level_ < p.max_level) return false;
    return !signal_live_ || last_sample_ms_ > p.shed_ms;
  }

  // The doublings of the restore delay of `level` still in force at `now_ns` (0 = none).
  int Doublings(int level, int64_t now_ns) const {
    const auto it = bounce_.find(level);
    if (it == bounce_.end()) return 0;
    const int64_t steps = std::max<int64_t>(0, now_ns - it->second.decay_from_ns) / kBounceDecayNs;
    return int(std::max<int64_t>(0, it->second.doublings - steps));
  }
  // How long the quiet time before a restore from `level` is.
  double RestoreDelayS(int level, int64_t now_ns, const Params& p) const {
    const double base = std::max(0.1, p.restore_s);
    const double cap = std::max(kMaxRestoreDelayS, base);
    return std::min(cap, base * double(1 << Doublings(level, now_ns)));
  }

  Change Step(const Input& in, const Params& p) {
    const int64_t now = in.now_ns;
    if (!started_) {
      started_ = true;
      win_start_ns_ = last_now_ns_ = restore_start_ns_ = now;
      driving_ = in.driving;
      win_.min_level = win_.max_level = level_;
    } else {
      win_.time_ns[level_] += std::max<int64_t>(0, now - last_now_ns_);
      last_now_ns_ = now;
    }

    // The engine frame: the missed history.
    missed_history_ = uint8_t((missed_history_ << 1) | (in.missed ? 1u : 0u));
    if (in.missed) ++win_.missed;

    // The GPU sample.
    const bool sample = in.new_sample && std::isfinite(in.gpu_ms) && in.gpu_ms > 0.0;
    if (sample) {
      recent_[size_t(recent_next_)] = in.gpu_ms;
      recent_next_ = (recent_next_ + 1) % kMedianSamples;
      recent_count_ = std::min(recent_count_ + 1, kMedianSamples);
      last_sample_ms_ = in.gpu_ms;
      last_sample_ns_ = now;
      have_sample_ = true;
      win_hist_.Add(Histogram::BucketOf(in.gpu_ms));
      ++win_.samples;
    }
    win_.lag = std::max(win_.lag, in.lag);
    signal_live_ = have_sample_ && now - last_sample_ns_ <= kSignalLiveNs;
    // The spacing of level changes counts GPU frames while the signal is live (the published count), engine frames without it.
    const uint32_t frames = signal_live_ ? (sample ? std::max<uint32_t>(1, in.seq_delta) : 0u) : 1u;
    frames_since_change_ = frames_since_change_ > UINT32_MAX - frames ? UINT32_MAX : frames_since_change_ + frames;

    // The restore timer: a change of the driving flag, a frame that is not driving and a missed frame all start it over.
    if (in.driving != driving_) {
      driving_ = in.driving;
      ResetRestore(now);
    }
    if (!in.driving || in.missed) ResetRestore(now);
    const double delay_s = RestoreDelayS(level_, now, p);
    if (sample && in.driving) AddRestoreSample(now, in.gpu_ms);
    TrimRestore(now, delay_s);

    // Pinned, capped, or the closed loop.
    Change out;
    if (p.force_level >= 0) {
      if (level_ != p.force_level) out = ChangeTo(p.force_level, Reason::kForced, now);
    } else if (level_ > p.max_level) {
      out = ChangeTo(std::max(p.max_level, p.min_level), Reason::kCap, now);
    } else if (level_ < p.min_level) {
      out = ChangeTo(p.min_level, Reason::kRichOff, now);
    } else {
      out = ClosedLoop(in, p, now, delay_s);
    }
    out.signal_gpu = signal_live_;
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
  struct TimedBucket {
    int64_t ns;
    int bucket;
  };

  static int PopCount(uint8_t v) {
    int n = 0;
    for (; v; v = uint8_t(v & (v - 1))) ++n;
    return n;
  }

  void ResetRestore(int64_t now) {
    restore_start_ns_ = now;
    restore_ring_.clear();
    restore_hist_.Clear();
  }
  void AddRestoreSample(int64_t now, double ms) {
    const int b = Histogram::BucketOf(ms);
    restore_ring_.push_back({now, b});
    restore_hist_.Add(b);
  }
  void TrimRestore(int64_t now, double delay_s) {
    const int64_t keep = int64_t(delay_s * 1e9);
    while (!restore_ring_.empty() && now - restore_ring_.front().ns > keep) {
      restore_hist_.Remove(restore_ring_.front().bucket);
      restore_ring_.pop_front();
    }
  }
  double Median() const {
    std::array<double, kMedianSamples> v{};
    for (int i = 0; i < recent_count_; ++i) v[size_t(i)] = recent_[size_t(i)];  // the ring holds the last `count` in slots 0..count-1 until full
    std::sort(v.begin(), v.begin() + recent_count_);
    const int n = recent_count_;
    return (n % 2) ? v[size_t(n / 2)] : 0.5 * (v[size_t(n / 2 - 1)] + v[size_t(n / 2)]);
  }

  Change ChangeTo(int to, Reason reason, int64_t now) {
    Change c;
    c.changed = true;
    c.from = level_;
    c.to = to;
    c.reason = reason;
    c.missed = PopCount(missed_history_);
    level_ = to;
    frames_since_change_ = 0;
    ResetRestore(now);
    win_.min_level = std::min(win_.min_level, to);
    win_.max_level = std::max(win_.max_level, to);
    return c;
  }

  Change Shed(Reason reason, const char* stat_name, double stat_ms, int stat_n, const Params& p, int64_t now) {
    const int to = level_ + 1;
    Change c = ChangeTo(to, reason, now);
    c.stat_name = stat_name;
    c.stat_ms = stat_ms;
    c.stat_n = stat_n;
    ++win_.sheds;
    if (has_restore_ && restore_from_ == to && now - restore_ns_ <= kBounceWindowNs) {
      const int doublings = std::min(Doublings(to, now) + 1, kMaxBounceDoublings);
      Bounce& b = bounce_[to];
      b.doublings = doublings;
      b.decay_from_ns = now;
      b.decay_from_ns = now + int64_t(RestoreDelayS(to, now, p) * 1e9);
      ++win_.bounces;
      c.bounce = true;
    }
    has_restore_ = false;
    return c;
  }

  Change ClosedLoop(const Input& in, const Params& p, int64_t now, double delay_s) {
    // Not driving (a menu, a pause, a load): the level is held. Loads hitch for reasons no level helps (streaming, pipeline
    // creation: run lk-gov2 shed 0 -> 4 on 25-35 ms loading frames), and the pause menu's cost is its UI.
    if (!in.driving) return Change();
    const int shed_frames = std::max(1, p.shed_frames);
    if (level_ < p.max_level && frames_since_change_ >= uint32_t(shed_frames)) {
      if (signal_live_ && recent_count_ >= kMinMedianSamples) {
        const double median = Median();
        if (median > p.shed_ms) return Shed(Reason::kGpuMedian, "p50", median, recent_count_, p, now);
      }
      if (signal_live_ && in.missed && last_sample_ms_ > p.shed_ms - kMissedShedMarginMs) {
        return Shed(Reason::kMissedAtBudget, "last", last_sample_ms_, 1, p, now);
      }
      if (!signal_live_ && PopCount(missed_history_) >= kMissedFallbackCount) {
        return Shed(Reason::kMissedFrames, "", -1.0, 0, p, now);
      }
    }
    if (in.driving && !in.missed && level_ > p.min_level && now - restore_start_ns_ >= int64_t(delay_s * 1e9)) {
      if (!signal_live_) {
        return Restore("", -1.0, 0, now);  // no GPU frame time to judge by: the quiet time with no missed frame is the condition
      }
      const uint32_t count = restore_hist_.Count();
      const uint32_t need = uint32_t(std::ceil(delay_s * kMinRestoreSamplesPerS));
      const double p90 = restore_hist_.Percentile(0.90);
      if (count >= need && p90 >= 0.0 && p90 < p.restore_ms) return Restore("p90", p90, int(count), now);
    }
    return Change();
  }

  Change Restore(const char* stat_name, double stat_ms, int stat_n, int64_t now) {
    Change c = ChangeTo(level_ - 1, Reason::kRestore, now);
    c.stat_name = stat_name;
    c.stat_ms = stat_ms;
    c.stat_n = stat_n;
    ++win_.restores;
    has_restore_ = true;
    restore_from_ = c.from;
    restore_ns_ = now;
    return c;
  }

  bool started_ = false;
  int level_ = 0;
  int64_t last_now_ns_ = 0;
  bool driving_ = true;
  uint32_t frames_since_change_ = UINT32_MAX;  // the first change is not held back

  uint8_t missed_history_ = 0;  // bit 0 = the latest engine frame; kMissedHistoryFrames bits
  static_assert(kMissedHistoryFrames == 8, "missed_history_ is one byte");

  std::array<double, kMedianSamples> recent_{};
  int recent_next_ = 0;
  int recent_count_ = 0;
  double last_sample_ms_ = 0.0;
  int64_t last_sample_ns_ = 0;
  bool have_sample_ = false;
  bool signal_live_ = false;

  int64_t restore_start_ns_ = 0;
  std::deque<TimedBucket> restore_ring_;
  Histogram restore_hist_;
  bool has_restore_ = false;
  int restore_from_ = 0;
  int64_t restore_ns_ = 0;
  std::map<int, Bounce> bounce_;

  Window win_;
  Histogram win_hist_;
  int64_t win_start_ns_ = 0;
};

}  // namespace mcla::gov::math
