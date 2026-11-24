// mcla_state_trace: follow the game's hierarchical state machine (HOOKS_SPEC F1). Read only.
//
// Site 0x8268DDD8 in vhsmState::Activate (0x8268DD70): `stw r10,12(r9)` stores the activated
// state's index as its parent's current child. r31 = the state being activated, r9 = its parent
// (also [state+0x20]). A vhsmState (vtable 0x820B76CC) has: +0x0C current child index, +0x10 flags
// (0x100 = active, 0x10 = disabled), +0x14 name (char*), +0x20 parent, +0x24 next sibling, +0x2C
// first child. The function dereferenced r31 before this point, so it is a valid object; the name
// pointers are read through the guarded reader anyway.
//
// Each activation logs   [mcla-state] <parent name>/<name>
// limited to 30 lines a second and one line per state per second (a state re-activated every
// frame would otherwise flood the log; the suppressed count is added to the next line), and is
// remembered: mcla::state::Current() returns the last state's name and its parent's.
//
// N0: every activation (logged or not) also goes to the classifier (mcla_game_state.h) as the name
// chain state, parent, grandparent, ... (up to kMaxChain, read through [state+0x20]). It maps the
// names to a screen (free roam, racing, pause, loading, ...), which the GameProfile's mode() /
// in_menu() report, the frame tick pushes to the host, and the log shows as `[mcla-mode]`.
//
// Off: --mcla_state_trace=false, or MCLA_HOOK_STATE_TRACE=0 at build time (the profile then answers
// "unknown", the host keeps its defaults).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_game_state.h"
#include "mcla_guest.h"
#include "mcla_hooks.h"

REXCVAR_DEFINE_BOOL(mcla_state_trace, true, "MCLA",
                    "MCLA: log the game's state-machine activations ([mcla-state] parent/name), "
                    "remember the current state and classify it (free roam, pause, ...; mcla_mode_follow)");

namespace {

mcla::hooks::Registration g_registration("state", "mcla_state_trace", MCLA_HOOK_STATE_TRACE != 0,
                                         [] { return REXCVAR_GET(mcla_state_trace); });

constexpr size_t kNameSize = 64;

struct CurrentState {
  std::mutex mutex;
  char name[kNameSize] = "";
  char parent[kNameSize] = "";
  uint64_t activations = 0;
};
CurrentState g_current;

}  // namespace

namespace mcla::state {
uint64_t Current(char* name, size_t name_size, char* parent, size_t parent_size) {
  std::lock_guard<std::mutex> lock(g_current.mutex);
  if (name && name_size) {
    std::snprintf(name, name_size, "%s", g_current.name);  // truncates, always NUL-terminated
  }
  if (parent && parent_size) {
    std::snprintf(parent, parent_size, "%s", g_current.parent);
  }
  return g_current.activations;
}
}  // namespace mcla::state

#if MCLA_HOOK_STATE_TRACE

namespace {

constexpr uint32_t kNameOffset = 0x14;
constexpr uint32_t kParentOffset = 0x20;
constexpr size_t kMaxChain = 6;  // state + 5 ancestors: UserInterface/MenuScene/PauseMenu/PauseTab/PM_x
constexpr int kLinesPerSecond = 30;
constexpr int64_t kSameStateMs = 1000;
constexpr int kRecent = 16;

struct Recent {
  uint32_t state = 0;
  int64_t logged_ms = 0;
};

struct LogLimiter {
  std::mutex mutex;
  Recent recent[kRecent];
  int next = 0;
  int64_t window_ms = 0;
  int lines_in_window = 0;
  uint64_t suppressed = 0;
};
LogLimiter g_limiter;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Reads the name at [state+0x14]; "?" when the object or the string is unreadable.
void ReadName(uint32_t state, char (&out)[kNameSize]) {
  out[0] = '\0';
  uint32_t pointer = 0;
  if (state != 0 && mcla::guest::U32(state + kNameOffset, &pointer) && pointer != 0) {
    mcla::guest::CString(pointer, out, sizeof(out));
  }
  if (out[0] == '\0') {
    std::snprintf(out, sizeof(out), "?");
  }
}

}  // namespace

void mcla_hook_state_trace(PPCRegister& r31) {
  MCLA_HOOK_ENTER();
  if (!REXCVAR_GET(mcla_state_trace)) {
    return;
  }
  const uint32_t state = r31.u32;
  // The chain, leaf first: the state, its parent, its grandparent, ... ([state+0x20] is the parent,
  // 0 or the object itself at the top). Every read is guarded; an unreadable name is "?".
  char chain_names[kMaxChain][kNameSize];
  const char* chain[kMaxChain];
  size_t chain_count = 0;
  uint32_t at = state;
  do {
    ReadName(at, chain_names[chain_count]);  // "?" when at == 0 or unreadable
    chain[chain_count] = chain_names[chain_count];
    ++chain_count;
    uint32_t next = 0;
    if (at == 0 || !mcla::guest::U32(at + kParentOffset, &next) || next == at) {
      break;
    }
    at = next;
  } while (chain_count < kMaxChain && at != 0);
  const char* const name = chain_names[0];
  const char* const parent_name = chain_count > 1 ? chain_names[1] : "";

  mcla::gamestate::OnActivation(chain, chain_count);

  {
    std::lock_guard<std::mutex> lock(g_current.mutex);
    std::snprintf(g_current.name, kNameSize, "%s", name);
    std::snprintf(g_current.parent, kNameSize, "%s", parent_name);
    ++g_current.activations;
  }

  const int64_t now = NowMs();
  uint64_t suppressed_before = 0;
  {
    std::lock_guard<std::mutex> lock(g_limiter.mutex);
    for (const Recent& r : g_limiter.recent) {
      if (r.state == state && now - r.logged_ms < kSameStateMs) {
        ++g_limiter.suppressed;
        return;
      }
    }
    if (now - g_limiter.window_ms >= 1000) {
      g_limiter.window_ms = now;
      g_limiter.lines_in_window = 0;
    }
    if (g_limiter.lines_in_window >= kLinesPerSecond) {
      ++g_limiter.suppressed;
      return;
    }
    ++g_limiter.lines_in_window;
    g_limiter.recent[g_limiter.next] = {state, now};
    g_limiter.next = (g_limiter.next + 1) % kRecent;
    suppressed_before = g_limiter.suppressed;
    g_limiter.suppressed = 0;
  }
  if (suppressed_before != 0) {
    REXLOG_INFO("[mcla-state] {}/{} (0x{:08X}) (+{} activations not logged)", parent_name, name, state,
                suppressed_before);
  } else {
    REXLOG_INFO("[mcla-state] {}/{} (0x{:08X})", parent_name, name, state);
  }
}

#else  // MCLA_HOOK_STATE_TRACE == 0: keeps hooks.toml linking, does nothing.

void mcla_hook_state_trace(PPCRegister&) {}

#endif
