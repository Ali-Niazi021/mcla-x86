// Unit test of the swap-release rule (mcla/hooks/mcla_t_swap_target.h): the target vblank the guest's swap callback
// (sub_82419818, D3D's present callback) chooses, the class of a swap the census derives from the callback's registers
// and the timed-swap hand-off between the two hooks of the presenting thread. Hand cases of the rule's three outcomes
// (on time: waits for its target; late with threshold 0: held one vblank; late with threshold 100: released now), the
// clamp of the percentage, the guest's 32-bit wrap-around, and a sweep of the properties that must hold everywhere.
// Standalone: no SDK, no game, no guest memory (the replay of recorded frame times needs the game's logs and is not here).
//
// Built and run by CTest (mcla/tests/CMakeLists.txt, test mcla_swap_target_test). By hand:
//   clang++ -std=c++23 -O1 -Wall -Wextra -I mcla/hooks mcla/tests/test_swap_target.cpp \
//       -o <scratch>/test_swap_target && <scratch>/test_swap_target
#include <cstdint>
#include <cstdio>
#include <thread>

#include "mcla_t_swap_target.h"

namespace m = mcla::sim::math;
using m::SwapClass;
using m::SwapTarget;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_failures;                                                              \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
    }                                                                            \
  } while (0)

#define CHECK_EQ(got, want)                                                                        \
  do {                                                                                             \
    const long long got_ = static_cast<long long>(got), want_ = static_cast<long long>(want);      \
    ++g_checks;                                                                                    \
    if (got_ != want_) {                                                                           \
      ++g_failures;                                                                                \
      std::printf("FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #got, got_, want_); \
    }                                                                                              \
  } while (0)

// ---- SwapTarget: the three outcomes ----------------------------------------------------------------------------

void TestOnTime() {
  // last + interval is still ahead of the vblank counter: the frame waits for exactly that vblank, whatever the
  // threshold says and however far into the period the callback runs.
  CHECK_EQ(SwapTarget(10, 1, 10, 50, 0), 11);
  CHECK_EQ(SwapTarget(10, 2, 11, 50, 0), 12);    // interval 2 (30 fps)
  CHECK_EQ(SwapTarget(10, 2, 11, 50, 100), 12);
  for (uint32_t threshold : {0u, 1u, 50u, 99u, 100u, 127u}) {
    for (uint32_t pct : {1u, 50u, 100u}) {
      CHECK_EQ(SwapTarget(10, 1, 10, pct, threshold), 11);
    }
  }
  // The edge: a target equal to the current vblank is already late.
  CHECK_EQ(SwapTarget(10, 1, 10, 50, 0), 11);  // target 11 > 10: on time
  CHECK_EQ(SwapTarget(10, 1, 11, 50, 0), 12);  // target 11 == 11: late (held to 12 with threshold 0)
}

void TestLateHeldWithThreshold0() {
  // The shipped threshold is 0: every late frame is pushed to the NEXT vblank (33.3 ms at 60 Hz, interval 1).
  CHECK_EQ(SwapTarget(10, 1, 11, 5, 0), 12);
  CHECK_EQ(SwapTarget(10, 1, 11, 1, 0), 12);    // a callback just after the vblank is held too: 1 > 0
  CHECK_EQ(SwapTarget(10, 1, 11, 100, 0), 12);
  CHECK_EQ(SwapTarget(10, 2, 13, 3, 0), 14);    // very late: the current vblank + 1, not the missed one
  CHECK_EQ(SwapTarget(10, 1, 25, 50, 0), 26);
}

void TestLateReleasedWithThreshold100() {
  // A threshold of 100 can never be exceeded: a late frame goes out at the current vblank.
  CHECK_EQ(SwapTarget(10, 1, 11, 100, m::kImmediateThreshold), 11);
  CHECK_EQ(SwapTarget(10, 1, 11, 5, m::kImmediateThreshold), 11);
  CHECK_EQ(SwapTarget(10, 2, 13, 3, m::kImmediateThreshold), 13);
  CHECK_EQ(SwapTarget(10, 1, 25, 50, m::kImmediateThreshold), 25);
  CHECK_EQ(SwapTarget(10, 1, 11, 100, 127), 11);  // the 7-bit maximum is no different
}

void TestThresholdIsStrict() {
  // The frame is pushed only when the percentage is past the threshold; equal is not past it.
  CHECK_EQ(SwapTarget(10, 1, 11, 5, 4), 12);
  CHECK_EQ(SwapTarget(10, 1, 11, 5, 5), 11);
  CHECK_EQ(SwapTarget(10, 1, 11, 5, 6), 11);
  CHECK_EQ(SwapTarget(10, 1, 11, 100, 99), 12);
  CHECK_EQ(SwapTarget(10, 1, 11, 100, 100), 11);
}

void TestIntervalZero() {
  // Interval 0 (no vblank wait): a late frame is never pushed, with any threshold.
  CHECK_EQ(SwapTarget(10, 0, 11, 99, 0), 11);
  CHECK_EQ(SwapTarget(10, 0, 11, 99, m::kImmediateThreshold), 11);
  CHECK_EQ(SwapTarget(10, 0, 10, 50, 0), 10);  // last + 0 is not ahead of the counter: the current vblank
}

void TestPctClamp() {
  // The callback clamps the percentage to 100 (kSwapPctMax): a larger one is no more past the threshold than 100.
  CHECK_EQ(SwapTarget(10, 1, 11, 400, 100), 11);
  CHECK_EQ(SwapTarget(10, 1, 11, 101, 100), 11);
  CHECK_EQ(SwapTarget(10, 1, 11, 0xFFFFFFFFu, 100), 11);
  CHECK_EQ(SwapTarget(10, 1, 11, 400, 99), 12);   // clamped to 100, which is past 99
  CHECK_EQ(SwapTarget(10, 1, 11, 100, 99), 12);   // and the same as 100 itself
  CHECK_EQ(SwapTarget(10, 1, 11, 0xFFFFFFFFu, 0), 12);
}

// ---- the guest's 32-bit counters wrap --------------------------------------------------------------------------

void TestWrapAround() {
  // The compare is unsigned, as the guest's: last + interval wrapping to 0 is NOT ahead of a counter of 5.
  CHECK_EQ(SwapTarget(0xFFFFFFFFu, 1, 5, 50, 0), 6);
  CHECK_EQ(SwapTarget(0xFFFFFFFFu, 1, 5, 50, m::kImmediateThreshold), 5);
  // A target that wrapped to 1 is ahead of a counter that wrapped to 0: on time.
  CHECK_EQ(SwapTarget(0xFFFFFFFEu, 3, 0, 50, 0), 1);
  // The counter at the top: the current vblank + 1 wraps to 0, the current one does not.
  CHECK_EQ(SwapTarget(0xFFFFFFFEu, 1, 0xFFFFFFFFu, 50, 0), 0);
  CHECK_EQ(SwapTarget(0xFFFFFFFEu, 1, 0xFFFFFFFFu, 50, m::kImmediateThreshold), 0xFFFFFFFFu);
  CHECK_EQ(SwapTarget(0xFFFFFFFEu, 1, 0xFFFFFFFDu, 50, 0), 0xFFFFFFFFu);  // on time just below the top
}

// ---- ClassifySwap ----------------------------------------------------------------------------------------------

void TestClassifySwap() {
  CHECK(m::ClassifySwap(10, 1, 10, 11) == SwapClass::kOnTime);
  CHECK(m::ClassifySwap(10, 2, 11, 12) == SwapClass::kOnTime);
  CHECK(m::ClassifySwap(10, 1, 11, 12) == SwapClass::kLateHeld);     // late, pushed to the next vblank
  CHECK(m::ClassifySwap(10, 1, 11, 11) == SwapClass::kLateAtOnce);   // late, released in the callback
  CHECK(m::ClassifySwap(10, 0, 11, 11) == SwapClass::kLateAtOnce);   // interval 0 is late as soon as the counter passes
  CHECK(m::ClassifySwap(10, 1, 11, 14) == SwapClass::kOther);        // a swap callback of the game altered the target
  CHECK(m::ClassifySwap(10, 1, 11, 10) == SwapClass::kOther);        // a target in the past
  // The 32-bit counters wrap here too.
  CHECK(m::ClassifySwap(0xFFFFFFFEu, 1, 0xFFFFFFFFu, 0) == SwapClass::kLateHeld);  // vblank + 1 wraps to 0
  CHECK(m::ClassifySwap(0xFFFFFFFEu, 1, 0xFFFFFFFFu, 0xFFFFFFFFu) == SwapClass::kLateAtOnce);
  CHECK(m::ClassifySwap(0xFFFFFFFEu, 3, 0, 1) == SwapClass::kOnTime);
}

// What must hold for every counter, interval, percentage and threshold (counters kept away from the wrap).
void TestSweep() {
  for (uint32_t last = 0; last < 6; ++last) {
    for (uint32_t interval = 0; interval < 5; ++interval) {
      for (uint32_t current = 0; current < 12; ++current) {
        const bool on_time = last + interval > current;
        for (uint32_t pct = 1; pct <= 100; pct += 7) {
          for (uint32_t threshold : {0u, 1u, 50u, 99u, 100u, 127u}) {
            const uint32_t target = SwapTarget(last, interval, current, pct, threshold);
            const SwapClass cls = m::ClassifySwap(last, interval, current, target);

            CHECK(cls != SwapClass::kOther);  // the rule's own answer is always one of the three outcomes
            CHECK(target >= current);         // never aims at a vblank that has gone
            if (on_time) {
              CHECK(cls == SwapClass::kOnTime);
              CHECK_EQ(target, last + interval);
              continue;
            }
            CHECK(target <= current + 1);     // a late frame waits at most one more vblank
            if (threshold >= m::kImmediateThreshold) {
              CHECK(cls == SwapClass::kLateAtOnce);  // the immediate threshold never holds a frame
            }
            if (interval == 0) {
              CHECK(cls == SwapClass::kLateAtOnce);
            } else if (threshold == 0) {
              CHECK(cls == SwapClass::kLateHeld);    // the shipped game: every late frame is held
            }
          }
        }
      }
    }
  }
}

// ---- the timed-swap hand-off -----------------------------------------------------------------------------------

void TestTimedSwapHandoff() {
  using mcla::sim::NoteSwapTimed;
  using mcla::sim::TakeSwapTimed;

  CHECK(!TakeSwapTimed());  // nothing published yet: a swap that skipped the publisher is never timed
  NoteSwapTimed(true);
  CHECK(TakeSwapTimed());
  CHECK(!TakeSwapTimed());  // consumed by the first take
  NoteSwapTimed(true);
  NoteSwapTimed(false);     // the publisher's latest word wins
  CHECK(!TakeSwapTimed());

  // One flag per thread: another thread neither sees nor consumes this one's.
  NoteSwapTimed(true);
  bool other_saw = true;
  std::thread([&other_saw] {
    other_saw = TakeSwapTimed();
    NoteSwapTimed(true);  // and its own flag does not leak back
  }).join();
  CHECK(!other_saw);
  CHECK(TakeSwapTimed());
  CHECK(!TakeSwapTimed());
}

}  // namespace

int main() {
  TestOnTime();
  TestLateHeldWithThreshold0();
  TestLateReleasedWithThreshold100();
  TestThresholdIsStrict();
  TestIntervalZero();
  TestPctClamp();
  TestWrapAround();
  TestClassifySwap();
  TestSweep();
  TestTimedSwapHandoff();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
