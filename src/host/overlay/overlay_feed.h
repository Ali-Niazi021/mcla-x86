// mcla-x86 host overlay: what the game thread tells the overlay, and how the overlay reads it back.
//
// The numbers of the statistics screen that did not exist anywhere but in a log line:
//
//   engine frame times   mcla::sim::OnFrameStart (mcla_t_common.cpp) keeps them in a mutex-guarded window that it resets
//                        and prints every 5 s. NoteEngineFrame() is a second, lock-free record of the same instants: a ring of
//                        the last 4096 frames, written by the game thread and read by the UI thread.
//   hitches              mcla_hitch.cpp logs a line per long frame. NoteHitch() keeps the count and the last cause.
//
// Cost on the game thread, per engine frame: one relaxed load of a flag while nothing is shown (the default), and two
// relaxed stores and one release store into the ring while the FPS counter or the statistics screen is up. No lock, no
// allocation, no system call. NoteHitch() takes a mutex, but a hitch is one frame in many.
//
// Threads: the writers are the game thread (NoteEngineFrame, NoteHitch); the readers are the UI thread (the overlay's draw)
// and, for the hitch list, any thread. The ring is single-writer / multi-reader; a reader that races the writer may see one
// slot a lap old at the very edge of the ring, which the timestamp test below discards.
//
// Clock: std::chrono::steady_clock in nanoseconds, the clock of OnFrameStart's now_ns (NowNs() reads it).
//
// Integration (two one-line calls, docs in integration.diff): mcla_t_common.cpp OnFrameStart, mcla_hitch.cpp OnFrame.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mcla_overlay {

// Nanoseconds on the steady clock (the clock the feed's timestamps use).
int64_t NowNs();

// ---- writers (game thread) ----------------------------------------------------------------------------------------------

// Once per engine frame, before anything else in the frame (OnFrameStart). A gap of a second or more (a load, a suspension, the
// first frame after the feed was off) is not a frame time: it restarts the measurement.
void NoteEngineFrame(int64_t now_ns);

// Right after NoteEngineFrame, with the game timer's total (guest seconds) read at the same instant: the pair is stored with the
// frame, so the world speed (guest seconds over real seconds) is measured between two exact frame starts, not between two UI
// paints (which would be a frame off in 2 s: +-0.8 %). Only call it while Demand().
void NoteWorldTime(int64_t now_ns, float world_total_s);

// A frame the hitch lines found long. `cause` is one short phrase (math::ClassifyHitch), `screen_mode` mcla::hitch's (-1
// unknown, 0 drive, 1 menu / pause / load).
void NoteHitch(int64_t now_ns, double frame_ms, int screen_mode, const char* cause);

// ---- demand (UI thread) -------------------------------------------------------------------------------------------------

// The overlay says whether anything needs the ring. Off, NoteEngineFrame returns after one load.
void SetDemand(bool on);
bool Demand();

// ---- readers ------------------------------------------------------------------------------------------------------------

// The frame times (ms) of the engine frames that ended within the last `window_s` seconds, oldest first, at most `max_frames`.
// `*span_s` (optional) = the time they cover. Empty when the feed has been off or the game has not run a frame.
std::vector<float> RecentFrameTimes(int64_t now_ns, double window_s, size_t max_frames, double* span_s = nullptr);

// The newest `count` frame times regardless of age (for a graph: zeros before the first frame).
std::vector<float> LastFrameTimes(size_t count);

// The world clock between the newest recorded frame and the newest one at least `window_s` older: `*world_s` guest seconds over
// `*real_s` real seconds. False when the feed holds no such pair (nothing noted, or the span is shorter than the window).
bool WorldClock(double window_s, double* world_s, double* real_s);

// Total frames the feed has recorded since the process started (a measure of whether it is fed at all).
uint64_t FramesRecorded();

struct HitchSummary {
  uint64_t total = 0;       // since the process started
  uint32_t recent = 0;      // within `recent_window_s` of now
  double worst_recent_ms = 0.0;
  double last_ms = 0.0;
  int64_t last_ns = 0;      // 0 = none yet
  int last_screen_mode = -1;
  std::string last_cause;
};
HitchSummary Hitches(int64_t now_ns, double recent_window_s);

// For the unit test and a reset on a new world: forgets everything the feed holds.
void ResetForTest();

}  // namespace mcla_overlay
