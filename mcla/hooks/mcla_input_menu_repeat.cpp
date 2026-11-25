// mcla_input_menu_repeat (W5.6): menu key repeat by time, not by game updates. At 60 fps (mcla_sim60) one tap of a
// direction moves a menu one or two steps; a hold repeats at twice the console's rate.
//
// What the game does (read in our generated code, sub_82223CD8 = the UI input object's update, run once per UI manager
// update; the same code Renan's mcla-recomp found, KNOWN-ISSUES "Menus stepped two or three times for one press above 60
// FPS"). The loop over the 24 menu inputs (r31 = 0..23; r4 = its key code or -1; d-pad, left stick, A, B, bumpers...)
// looks at the input's 12-byte record at 0x8286E4B8 + 12 * r31 (byte 0 the rest value, byte 2 now, byte 3 before; "down" is
// bit 7 differing from the rest value). The press branch (the button is down now and was not before):
//     0x822240C0  stbx r27,r31,r28      threshold[r31] = 7 (r27 = 7)         <- hook 1, BEFORE this
//                 stbx r21,r31,r29      count[r31] = 0
//                 ... key down: sub_82209C38(UI logic, r4 = key, r5)
// The held branch (down now and before), once per update:
//     0x822241BC  lbzx r11,r31,r29      count
//     0x822241C0  lbzx r10,r31,r28      threshold
//     0x822241C4  addi r9,r11,1
//     0x822241C8  clrlwi r8,r9,24       r8 = count + 1
//     0x822241CC  stbx r8,r31,r29       count[r31] = r8                      <- hook 2, BEFORE this
//     0x822241D0  cmplw cr6,r10,r8      equal: the repeat
//     0x822241D4  bne cr6,0x8222421c
//                 lbz r11,803(r26)      [object + 803] = 6, set in sub_8221D560
//                 stbx r21,r31,r29      count = 0 ; stbx r11,r31,r28 : threshold = 6
//                 ... key down again
// So a held button repeats after 7 updates, then every 6: 233 ms then 200 ms at the console's 30 updates a second, 117 ms
// then 100 ms at 60. A tap of 60 to 150 ms meets the first repeat.
//
// The fix (the semantics are Renan's, credited; this is a re-derivation: his hook sits on the compare at 0x822241D0 and
// writes the count back to guest memory, ours sits one store earlier so a register is enough). The press branch records
// the time (hook 1). When the count says a repeat is due (r8 == r10), the repeat goes ahead only if
// mcla_input_repeat_delay_ms (233, after the key down) or mcla_input_repeat_interval_ms (200, after the last repeat)
// have passed, less a quarter of a 30 Hz update (8.3 ms, so a 30 fps run is never held back by jitter); otherwise hook 2
// sets r8 = threshold - 1: the stored count is one short, the compare fails, and the next update asks again. At 30 fps
// nothing changes (the game's count is the slower one). The first repeat lands at the update after 233 ms, then every
// 200 ms: five key events in a 1 s hold at any frame rate.
//
// The hooks (mid-asm, hooks_*.toml; no after_instruction):
//   void mcla_hook_input_key_press(PPCRegister& r31)                                    0x822240C0
//   void mcla_hook_input_key_repeat(PPCRegister& r8, PPCRegister& r10, PPCRegister& r31) 0x822241CC
//
// cvar mcla_input_menu_repeat: -1 auto (default: on while mcla_sim60 is on), 1 on, 0 off. Logs
// `[mcla-input-fix] menu key repeat active ...` at the first hook call that finds it wanted, and a counts line every 10 s
// while something happened. Build switch: MCLA_HOOK_INPUT_MENU_REPEAT=0 compiles the hooks to nothing.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_input_common.h"

REXCVAR_DEFINE_INT32(mcla_input_menu_repeat, -1, "MCLA",
                     "MCLA 60 fps input fix: a held menu button or direction repeats after "
                     "mcla_input_repeat_delay_ms and then every mcla_input_repeat_interval_ms (the console's 7 and 6 "
                     "updates at 30 a second), not after 7 and 6 game updates (117 and 100 ms at 60 fps, which made "
                     "a tap step a menu twice). 1 = on, 0 = off, -1 = auto: on while mcla_sim60 is on (default). Live");
REXCVAR_DEFINE_INT32(mcla_input_repeat_delay_ms, 233, "MCLA",
                     "With mcla_input_menu_repeat: how long a menu button is held before it repeats (the game's 7 "
                     "updates at 30 a second)")
    .range(0, 2000);
REXCVAR_DEFINE_INT32(mcla_input_repeat_interval_ms, 200, "MCLA",
                     "With mcla_input_menu_repeat: the time between repeats of a held menu button (the game's 6 "
                     "updates at 30 a second)")
    .range(0, 2000);

namespace {

mcla::hooks::Registration g_registration(
    "input-menu-repeat", "mcla_input_menu_repeat", MCLA_HOOK_INPUT_MENU_REPEAT != 0,
    [] { return mcla::input::Wanted(REXCVAR_GET(mcla_input_menu_repeat)); },
    /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_INPUT_MENU_REPEAT

namespace {

constexpr uint32_t kInputs = 24;
// A quarter of a 30 Hz update: a repeat may come this much before its time.
constexpr int64_t kSlackUs = 1'000'000 / 30 / 4;
constexpr uint64_t kReportUs = 10'000'000;

struct KeyTime {
  uint64_t last_us = 0;  // the key down, or the last repeat; 0 = not seen
  uint64_t need_us = 0;  // how long after it the next repeat is due
};

std::mutex g_mutex;
KeyTime g_key[kInputs];

std::atomic<bool> g_announced{false};
std::atomic<uint64_t> g_downs{0}, g_allowed{0}, g_deferred{0};
std::atomic<uint64_t> g_next_report_us{0};

uint64_t DelayUs() { return uint64_t(std::max(0, REXCVAR_GET(mcla_input_repeat_delay_ms))) * 1000; }
uint64_t IntervalUs() { return uint64_t(std::max(0, REXCVAR_GET(mcla_input_repeat_interval_ms))) * 1000; }

void Announce() {
  if (g_announced.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  REXLOG_INFO(
      "[mcla-input-fix] menu key repeat active (mcla_input_menu_repeat={}, mcla_sim60={}): a held menu button repeats "
      "after {} ms, then every {} ms, as at the console's 30 updates a second (the game counts 7, then 6 updates: "
      "117 / 100 ms at 60 fps); hooks 0x822240C0 (key down) and 0x822241CC (count)",
      REXCVAR_GET(mcla_input_menu_repeat), mcla::sim::Enabled(), REXCVAR_GET(mcla_input_repeat_delay_ms),
      REXCVAR_GET(mcla_input_repeat_interval_ms));
}

void MaybeReport(uint64_t now) {
  uint64_t due = g_next_report_us.load(std::memory_order_relaxed);
  if (due == 0) {
    g_next_report_us.store(now + kReportUs, std::memory_order_relaxed);
    return;
  }
  if (now < due || !g_next_report_us.compare_exchange_strong(due, now + kReportUs, std::memory_order_relaxed)) {
    return;
  }
  const uint64_t downs = g_downs.exchange(0, std::memory_order_relaxed);
  const uint64_t allowed = g_allowed.exchange(0, std::memory_order_relaxed);
  const uint64_t deferred = g_deferred.exchange(0, std::memory_order_relaxed);
  if (downs || allowed || deferred) {
    REXLOG_INFO("[mcla-input-fix] menu key repeat: {} key downs, {} repeats allowed, {} updates held back in the last "
                "{} s",
                downs, allowed, deferred, kReportUs / 1'000'000);
  }
}

}  // namespace

// 0x822240C0, before `stbx r27,r31,r28`: a button is first seen pressed (r31 = the input, 0..23).
void mcla_hook_input_key_press(PPCRegister& r31) {
  MCLA_HOOK_ENTER();
  if (!mcla::input::Wanted(REXCVAR_GET(mcla_input_menu_repeat))) {
    return;
  }
  const uint32_t input = r31.u32;
  if (input >= kInputs) {
    return;
  }
  Announce();
  const uint64_t now = mcla::input::NowUs();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_key[input] = {now, DelayUs()};
  }
  g_downs.fetch_add(1, std::memory_order_relaxed);
  MaybeReport(now);
}

// 0x822241CC, before `stbx r8,r31,r29` (r8 = the count after this update, r10 = the threshold, r31 = the input): the
// compare that follows sends the repeat when they are equal.
void mcla_hook_input_key_repeat(PPCRegister& r8, PPCRegister& r10, PPCRegister& r31) {
  const uint32_t count = r8.u32;
  const uint32_t threshold = r10.u32;
  if (count != threshold) {
    return;  // no repeat on this update, the usual case: costs two compares
  }
  MCLA_HOOK_ENTER();
  const uint32_t input = r31.u32;
  if (threshold == 0 || input >= kInputs || !mcla::input::Wanted(REXCVAR_GET(mcla_input_menu_repeat))) {
    return;
  }
  Announce();
  const uint64_t now = mcla::input::NowUs();
  bool hold_back = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    KeyTime& key = g_key[input];
    if (key.last_us == 0) {
      // Down before the hooks watched (a button held when the menu came up, or the fix switched on while held): from now.
      key = {now, DelayUs()};
    }
    const int64_t late = int64_t(now) - int64_t(key.last_us + key.need_us);
    if (late < -kSlackUs) {
      hold_back = true;
    } else {
      // On time: the next one is due from when this one was due, unless it came a lot later.
      key.last_us = late < int64_t(key.need_us / 2) ? key.last_us + key.need_us : now;
      key.need_us = IntervalUs();
    }
  }
  if (hold_back) {
    // Too early: the stored count one short, so the compare fails and the next update asks again.
    r8.u64 = (threshold - 1) & 0xFF;
    g_deferred.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_allowed.fetch_add(1, std::memory_order_relaxed);
  }
  MaybeReport(now);
}

#else  // MCLA_HOOK_INPUT_MENU_REPEAT == 0: keeps the hook declarations linking, does nothing.

void mcla_hook_input_key_press(PPCRegister&) {}
void mcla_hook_input_key_repeat(PPCRegister&, PPCRegister&, PPCRegister&) {}

#endif
