// mcla_input_title_latch (W5.6): a script that polls a pad button "pressed in this update" less often than the game
// updates is told about a press it missed. At 60 fps (mcla_sim60) about half of the presses of Start on the title screen
// and the attract video are lost (run w4.3-b-noaa died of "Start presses did not take").
//
// What the game does (read in our generated code; the finding is Renan's mcla-recomp, KNOWN-ISSUES "Start has to be
// pressed several times on the title screen", launcher/dev/title-start-2026-10-29.md, his own analysis, not LARecomp's; the
// code here is a re-derivation). Once per game update sub_82614D30 reads XInput into a 64-byte pad record, pad n at
// 0x82905388 + 64 n: the word at +8 is the new button mask, the word at +12 the previous update's (LB 4, RB 8, Y 16, B 32,
// A 64, X 128, Back 256, left stick 512, right stick 1024, Start 2048, up 4096, right 8192, down 16384, left 32768; a
// script's button number is the bit number, Start = 11). The script native IS_PAD_BUTTON_DOWN(pad, button), sub_822C8AF8,
// answers (prev ^ new) & (1 << button) & new != 0: "pressed in THIS update", true for one update only:
//     0x822C8B30  and r8,r9,r3          r8 = the press (nonzero = pressed)
//     0x822C8B34  cntlzw r6,r8          <- hook 2, BEFORE this: r8 may be set, r6 = the pad (arg 0), r5 = the button (arg 1)
//     0x822C8B38  rlwinm r5,r6,27,31,31 ; xori r4,r5,1 ; clrlwi r3,r4,24 ; stw r3,0(r7)   (the answer)
// The title screen and the attract video are decided by a script thread (a scrThread run by sub_82554E20 from the game's
// update) that polls the pad this way. Each game update adds a fixed 16 ms to a script thread's clocks and the title's
// loop sleeps about 30 ms, so it runs every 2nd update at 60 fps (every 3rd at 120, every one at 30, the console's
// rate). A press that falls in an update in which the thread does not run is gone at its next run. Renan measured: every
// press whose update the thread ran in was accepted, none of those whose update it did not run in.
//
// The fix (his semantics). Hook 1 sees every press edge of every pad once per update (0x82615048 in sub_82614D30, where the
// branches that set the new mask meet; r31 = the pad record): per (pad, button) a sequence number and the time of the
// last press. Hook 2 is the script's ask. Per (script thread, its program [thread + 8], pad, button) it remembers which
// press the thread has been told about (the real one, or one the latch told), and when it last asked. A thread that asks
// and gets no press is told about the latest one, ONCE, when
//   * it asked this button within 200 ms BEFORE the press (it is a poller; a thread that starts asking after the press,
//     as the next screen's would, is not told, so one press never also answers the next screen),
//   * it has not been told about this press (by the real answer, or earlier),
//   * the press is at most mcla_input_latch_ms (100) old,
// and asks again in the update it was told in answer the same, as they would for the real press. Hook 2 knows the thread
// from the interpreter's "current thread" global BE32[0x828DA3BC] (written by sub_82554E20 at its start and put back at
// its end). The menus do not use this path (their key events are driven by the UI input object, mcla_input_menu_repeat).
// Not covered: the other three natives that ask for an edge this way (IS_BUTTON_DOWN sub_822C7FC8, UILogic_IsPressed,
// Player_FlashingHighbeams): Renan saw no script use them on a screen where a miss showed.
//
// The hooks (mid-asm, hooks_*.toml):
//   void mcla_hook_input_pad_update(PPCRegister& r31)                                  0x82615048
//   void mcla_hook_input_pad_poll(PPCRegister& r8, PPCRegister& r6, PPCRegister& r5)   0x822C8B34
//
// cvar mcla_input_title_latch: -1 auto (default: on while mcla_sim60 is on), 1 on, 0 off. mcla_input_latch_ms: 100.
// Logs `[mcla-input-fix] title start latch active ...` at the first pad update that finds it wanted, one line per first
// latched press, and a counts line every 10 s while something happened. Build switch: MCLA_HOOK_INPUT_TITLE_LATCH=0
// compiles the hooks to nothing.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"
#include "mcla_input_common.h"

REXCVAR_DEFINE_INT32(mcla_input_title_latch, -1, "MCLA",
                     "MCLA 60 fps input fix: a script that polls a pad button 'pressed in this update' (the title "
                     "screen's and the attract video's Start) less often than the game updates is told about a press "
                     "it missed, once, at its next poll (mcla_input_latch_ms). Without it about half of the presses of "
                     "Start at 60 fps are lost. 1 = on, 0 = off, -1 = auto: on while mcla_sim60 is on (default). Live");
REXCVAR_DEFINE_INT32(mcla_input_latch_ms, 100, "MCLA",
                     "With mcla_input_title_latch: a missed press is told to the script at its next poll only if it "
                     "is at most this many ms old")
    .range(0, 1000);

namespace {

mcla::hooks::Registration g_registration(
    "input-title-latch", "mcla_input_title_latch", MCLA_HOOK_INPUT_TITLE_LATCH != 0,
    [] { return mcla::input::Wanted(REXCVAR_GET(mcla_input_title_latch)); },
    /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_INPUT_TITLE_LATCH

namespace {

constexpr uint32_t kPadBase = 0x82905388;
constexpr uint32_t kPadStride = 64;
constexpr uint32_t kPadCount = 4;
constexpr uint32_t kButtons = 16;  // the mask's low 16 bits: the buttons a script can ask about
constexpr uint32_t kCurrentThread = 0x828DA3BC;
constexpr uint32_t kThreadProgram = 8;  // scrThread + 8: the program it runs
// A script thread polls a button if it asked within this long before the press.
constexpr uint64_t kPollerUs = 200'000;
constexpr uint64_t kReportUs = 10'000'000;
constexpr size_t kSites = 128;

struct Edge {
  uint32_t seq = 0;  // presses seen
  uint64_t us = 0;   // when the last one was seen
};

// What one script thread has done with one button of one pad.
struct Site {
  bool used = false;
  uint32_t thread = 0;
  uint32_t program = 0;  // [thread + 8]: a thread slot that the game reuses for another script is a new site
  uint32_t pad = 0;
  uint32_t button = 0;
  uint64_t last_poll_us = 0;   // its last ask
  uint32_t seen_seq = 0;       // the press it has been told about (the real one or the latched one)
  uint64_t seen_update = 0;    // the game update it was told in
  bool seen_latched = false;   // ... by the latch (later asks in that update answer the same)
};

std::mutex g_mutex;
Edge g_edges[kPadCount][kButtons];
Site g_sites[kSites];
uint64_t g_update = 0;  // pad 0's updates seen

std::atomic<bool> g_announced{false};
std::atomic<uint32_t> g_logged_latched{0};
std::atomic<uint64_t> g_edge_count{0}, g_real_count{0}, g_latched_count{0};
std::atomic<uint64_t> g_next_report_us{0};

void Announce() {
  if (g_announced.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  REXLOG_INFO(
      "[mcla-input-fix] title start latch active (mcla_input_title_latch={}, mcla_sim60={}): a script thread that polls "
      "a pad button 'pressed in this update' (IS_PAD_BUTTON_DOWN, sub_822C8AF8; the title screen and the attract "
      "video poll Start every 2nd update at 60 fps) and misses a press is told about it once, at its next poll, up "
      "to {} ms later; hooks 0x82615048 (press edges) and 0x822C8B34 (the ask)",
      REXCVAR_GET(mcla_input_title_latch), mcla::sim::Enabled(), REXCVAR_GET(mcla_input_latch_ms));
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
  const uint64_t edges = g_edge_count.exchange(0, std::memory_order_relaxed);
  const uint64_t real = g_real_count.exchange(0, std::memory_order_relaxed);
  const uint64_t latched = g_latched_count.exchange(0, std::memory_order_relaxed);
  if (edges || real || latched) {
    REXLOG_INFO("[mcla-input-fix] title start latch: {} pad presses, {} told live to a script, {} told by the latch in "
                "the last {} s",
                edges, real, latched, kReportUs / 1'000'000);
  }
}

// The site of (thread, program, pad, button); a new one replaces the least recently asked. g_mutex held.
Site& FindSite(uint32_t thread, uint32_t program, uint32_t pad, uint32_t button) {
  Site* oldest = &g_sites[0];
  for (Site& site : g_sites) {
    if (site.used && site.thread == thread && site.program == program && site.pad == pad && site.button == button) {
      return site;
    }
    if (!site.used) {
      oldest = &site;
    } else if (oldest->used && site.last_poll_us < oldest->last_poll_us) {
      oldest = &site;
    }
  }
  *oldest = Site{};
  oldest->used = true;
  oldest->thread = thread;
  oldest->program = program;
  oldest->pad = pad;
  oldest->button = button;
  return *oldest;
}

}  // namespace

// 0x82615048 in sub_82614D30: the pad record (r31) has its new button mask (+8) and the previous one (+12); runs once per
// game update for every pad that is connected.
void mcla_hook_input_pad_update(PPCRegister& r31) {
  MCLA_HOOK_ENTER();
  if (!mcla::input::Wanted(REXCVAR_GET(mcla_input_title_latch))) {
    return;
  }
  const uint32_t record = r31.u32;
  if (record < kPadBase || (record - kPadBase) % kPadStride != 0) {
    return;
  }
  const uint32_t pad = (record - kPadBase) / kPadStride;
  uint32_t current = 0, previous = 0;
  if (pad >= kPadCount || !mcla::sim::ImageU32(record + 8, &current) || !mcla::sim::ImageU32(record + 12, &previous)) {
    return;
  }
  Announce();
  const uint64_t now = mcla::input::NowUs();
  const uint32_t pressed = current & ~previous & ((1u << kButtons) - 1);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (pad == 0) {
      ++g_update;
    }
    for (uint32_t button = 0; button < kButtons; ++button) {
      if (pressed & (1u << button)) {
        Edge& edge = g_edges[pad][button];
        ++edge.seq;
        edge.us = now;
        g_edge_count.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  if (pad == 0) {
    MaybeReport(now);
  }
}

// 0x822C8B34 in sub_822C8AF8 (IS_PAD_BUTTON_DOWN(pad, button)), before `cntlzw r6,r8`: r8 = the native's own answer (the
// press: nonzero = pressed; set it to tell the script), r6 = the pad, r5 = the button.
void mcla_hook_input_pad_poll(PPCRegister& r8, PPCRegister& r6, PPCRegister& r5) {
  if (!mcla::input::Wanted(REXCVAR_GET(mcla_input_title_latch))) {
    return;
  }
  const uint32_t pad = r6.u32;
  const uint32_t button = r5.u32;
  uint32_t thread = 0, program = 0;
  if (pad >= kPadCount || button >= kButtons || !mcla::sim::ImageU32(kCurrentThread, &thread) || thread == 0 ||
      !mcla::guest::U32(thread + kThreadProgram, &program)) {
    return;
  }
  const bool real = r8.u32 != 0;
  const uint64_t now = mcla::input::NowUs();
  const uint64_t window_us = uint64_t(std::max(0, REXCVAR_GET(mcla_input_latch_ms))) * 1000;
  bool latched = false;
  uint64_t press_age_us = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const Edge& edge = g_edges[pad][button];
    Site& site = FindSite(thread, program, pad, button);
    if (real) {
      site.seen_seq = edge.seq;
      site.seen_update = g_update;
      site.seen_latched = false;
    } else if (edge.seq != 0) {
      if (site.seen_seq == edge.seq) {
        latched = site.seen_latched && site.seen_update == g_update;
      } else if (window_us != 0 && site.last_poll_us != 0 && site.last_poll_us < edge.us &&
                 edge.us - site.last_poll_us <= kPollerUs && now - edge.us <= window_us) {
        latched = true;
        press_age_us = now - edge.us;
        site.seen_seq = edge.seq;
        site.seen_update = g_update;
        site.seen_latched = true;
        g_latched_count.fetch_add(1, std::memory_order_relaxed);
      }
    }
    site.last_poll_us = now;
  }
  if (real) {
    g_real_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (latched) {
    r8.u64 = uint64_t(1) << button;  // nonzero: pressed
    if (press_age_us != 0 && g_logged_latched.fetch_add(1, std::memory_order_relaxed) < 8) {
      REXLOG_INFO("[mcla-input-fix] title start latch: script thread 0x{:08X} told about pad {} button {} {:.1f} ms "
                  "after the press it missed",
                  thread, pad, button, double(press_age_us) / 1000.0);
    }
  }
}

#else  // MCLA_HOOK_INPUT_TITLE_LATCH == 0: keeps the hook declarations linking, does nothing.

void mcla_hook_input_pad_update(PPCRegister&) {}
void mcla_hook_input_pad_poll(PPCRegister&, PPCRegister&, PPCRegister&) {}

#endif
