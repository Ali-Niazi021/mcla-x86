// mcla-x86 host overlay: the engine frame ring and the hitch record. See overlay_feed.h.
#include "overlay_feed.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>

namespace mcla_overlay {
namespace {

constexpr size_t kRing = 4096;  // a power of two: 34 s at 120 fps
static_assert((kRing & (kRing - 1)) == 0, "the ring index is masked");
constexpr int64_t kGapNs = 1'000'000'000;  // a longer gap between frames is a load or a suspension, not a frame time

constexpr uint32_t kNoWorld = 0x7FC00000u;  // a quiet NaN: no world time was noted for the frame

struct Slot {
  std::atomic<uint32_t> us{0};      // the frame's time in microseconds
  std::atomic<int64_t> end_ns{0};   // when the frame ended (the next frame's start), steady clock
  std::atomic<uint32_t> world_bits{kNoWorld};  // the game timer's total (float bits) at that instant, kNoWorld = not noted
};

Slot g_ring[kRing];
std::atomic<uint64_t> g_head{0};       // frames written; slot i is g_ring[i & (kRing - 1)]
std::atomic<bool> g_demand{false};
std::atomic<FrameTap> g_tap{nullptr};
std::atomic<int64_t> g_last_ns{0};     // the previous frame's start; written by the game thread only

struct HitchEvent {
  int64_t ns;
  double ms;
};
std::mutex g_hitch_mutex;
std::deque<HitchEvent> g_hitches;  // the last 256
uint64_t g_hitch_total = 0;
double g_hitch_last_ms = 0.0;
int64_t g_hitch_last_ns = 0;
int g_hitch_last_screen = -1;
std::string g_hitch_last_cause;

}  // namespace

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void SetFrameTap(FrameTap tap) { g_tap.store(tap, std::memory_order_release); }

void NoteEngineFrame(int64_t now_ns) {
  if (const FrameTap tap = g_tap.load(std::memory_order_acquire)) tap(now_ns);
  if (!g_demand.load(std::memory_order_relaxed)) {
    // Nobody is looking: the next frame after the overlay is shown must not measure from a stale instant.
    g_last_ns.store(0, std::memory_order_relaxed);
    return;
  }
  const int64_t last = g_last_ns.exchange(now_ns, std::memory_order_relaxed);
  if (last == 0 || now_ns <= last || now_ns - last >= kGapNs) {
    return;
  }
  const uint64_t us = uint64_t(now_ns - last) / 1000;
  const uint64_t index = g_head.load(std::memory_order_relaxed);
  Slot& slot = g_ring[index & (kRing - 1)];
  slot.us.store(uint32_t(std::min<uint64_t>(us, 0xFFFFFFFFu)), std::memory_order_relaxed);
  slot.end_ns.store(now_ns, std::memory_order_relaxed);
  slot.world_bits.store(kNoWorld, std::memory_order_relaxed);
  g_head.store(index + 1, std::memory_order_release);
}

void NoteWorldTime(int64_t now_ns, float world_total_s) {
  const uint64_t head = g_head.load(std::memory_order_relaxed);
  if (head == 0) return;
  Slot& slot = g_ring[(head - 1) & (kRing - 1)];
  if (slot.end_ns.load(std::memory_order_relaxed) != now_ns) return;  // no frame was recorded at this instant (the first one)
  uint32_t bits;
  std::memcpy(&bits, &world_total_s, sizeof(bits));
  slot.world_bits.store(bits, std::memory_order_relaxed);
}

bool WorldClock(double window_s, double* world_s, double* real_s) {
  const uint64_t head = g_head.load(std::memory_order_acquire);
  const uint64_t available = std::min<uint64_t>(head, kRing - 16);
  auto world_of = [](const Slot& slot, float* out) {
    const uint32_t bits = slot.world_bits.load(std::memory_order_relaxed);
    if (bits == kNoWorld) return false;
    std::memcpy(out, &bits, sizeof(*out));
    return true;
  };
  // The newest frame with a world time (the very newest may not have had it stored yet).
  uint64_t newest = 0;
  float world_new = 0.0f;
  bool have_new = false;
  for (uint64_t k = 1; k <= std::min<uint64_t>(available, 4); ++k) {
    const Slot& slot = g_ring[(head - k) & (kRing - 1)];
    if (world_of(slot, &world_new)) {
      newest = k;
      have_new = true;
      break;
    }
  }
  if (!have_new) return false;
  const int64_t t_new = g_ring[(head - newest) & (kRing - 1)].end_ns.load(std::memory_order_relaxed);
  const int64_t want_ns = t_new - int64_t(window_s * 1e9);
  for (uint64_t k = newest + 1; k <= available; ++k) {
    const Slot& slot = g_ring[(head - k) & (kRing - 1)];
    const int64_t t = slot.end_ns.load(std::memory_order_relaxed);
    if (t > want_ns) continue;
    float world_old = 0.0f;
    if (!world_of(slot, &world_old)) return false;
    *world_s = double(world_new) - double(world_old);
    *real_s = double(t_new - t) / 1e9;
    return true;
  }
  return false;
}

void NoteHitch(int64_t now_ns, double frame_ms, int screen_mode, const char* cause) {
  std::lock_guard<std::mutex> lock(g_hitch_mutex);
  ++g_hitch_total;
  g_hitches.push_back({now_ns, frame_ms});
  while (g_hitches.size() > 256) g_hitches.pop_front();
  g_hitch_last_ms = frame_ms;
  g_hitch_last_ns = now_ns;
  g_hitch_last_screen = screen_mode;
  g_hitch_last_cause = cause ? cause : "";
}

void SetDemand(bool on) { g_demand.store(on, std::memory_order_relaxed); }
bool Demand() { return g_demand.load(std::memory_order_relaxed); }

std::vector<float> RecentFrameTimes(int64_t now_ns, double window_s, size_t max_frames, double* span_s) {
  std::vector<float> out;
  const uint64_t head = g_head.load(std::memory_order_acquire);
  const uint64_t available = std::min<uint64_t>(head, kRing);
  const int64_t cutoff = now_ns - int64_t(window_s * 1e9);
  const size_t limit = std::min<size_t>(max_frames, kRing - 16);  // never read the slot the writer is about to reuse
  double sum_ms = 0.0;
  for (uint64_t k = 0; k < available && out.size() < limit; ++k) {
    const Slot& slot = g_ring[(head - 1 - k) & (kRing - 1)];
    if (slot.end_ns.load(std::memory_order_relaxed) < cutoff) break;
    const float ms = float(slot.us.load(std::memory_order_relaxed)) / 1000.0f;
    out.push_back(ms);
    sum_ms += double(ms);
  }
  std::reverse(out.begin(), out.end());
  if (span_s) *span_s = sum_ms / 1000.0;
  return out;
}

std::vector<float> LastFrameTimes(size_t count) {
  std::vector<float> out(count, 0.0f);
  const uint64_t head = g_head.load(std::memory_order_acquire);
  const size_t have = size_t(std::min<uint64_t>({head, kRing - 16, uint64_t(count)}));
  for (size_t k = 0; k < have; ++k) {
    const Slot& slot = g_ring[(head - 1 - k) & (kRing - 1)];
    out[count - 1 - k] = float(slot.us.load(std::memory_order_relaxed)) / 1000.0f;
  }
  return out;
}

uint64_t FramesRecorded() { return g_head.load(std::memory_order_relaxed); }

HitchSummary Hitches(int64_t now_ns, double recent_window_s) {
  HitchSummary s;
  std::lock_guard<std::mutex> lock(g_hitch_mutex);
  s.total = g_hitch_total;
  const int64_t cutoff = now_ns - int64_t(recent_window_s * 1e9);
  for (auto it = g_hitches.rbegin(); it != g_hitches.rend(); ++it) {
    if (it->ns < cutoff) break;
    ++s.recent;
    s.worst_recent_ms = std::max(s.worst_recent_ms, it->ms);
  }
  s.last_ms = g_hitch_last_ms;
  s.last_ns = g_hitch_last_ns;
  s.last_screen_mode = g_hitch_last_screen;
  s.last_cause = g_hitch_last_cause;
  return s;
}

void ResetForTest() {
  g_head.store(0, std::memory_order_relaxed);
  g_last_ns.store(0, std::memory_order_relaxed);
  for (Slot& slot : g_ring) {
    slot.us.store(0, std::memory_order_relaxed);
    slot.end_ns.store(0, std::memory_order_relaxed);
  }
  std::lock_guard<std::mutex> lock(g_hitch_mutex);
  g_hitches.clear();
  g_hitch_total = 0;
  g_hitch_last_ms = 0.0;
  g_hitch_last_ns = 0;
  g_hitch_last_screen = -1;
  g_hitch_last_cause.clear();
}

}  // namespace mcla_overlay
