// mcla_ws_ui (W6): the Flash UI under widescreen (docs/mcla/WIDESCREEN.md sections 6, 13.4 and 15).
// Probes (counts and logs) plus the UI squeeze, which is OFF by default (mcla_ws_ui_squeeze = 0).
// Run W-1 / ws-1568 showed case S: the Flash stage (1280x720 authored) is stretched over the whole
// back buffer W, so everything Flash is W/1280 too wide (22 % at 1568) and nothing is anchored to
// the screen edges. This file keeps the stage at 16:9 proportions in the middle 1280 px, and puts
// the HUD pieces that belong to a screen edge back at that edge.
//
// W5.1 (back buffers of any height): "the middle 1280 px" below is the middle 16:9 block of the buffer, which is
// H x 16/9 px wide (ws::UiBlockWidth(): 1280 at 720 lines, 1920 at 1080); the stage itself stays 1280 stage px wide
// (stage px per screen px = 1280 / W). A buffer that is not wider than 16:9 (1920x1080, 1600x900, 1440x1080) has
// no side area: nothing is squeezed or moved, whatever the mask says.
//
// Tall buffers (S4: 1280x800, 1440x900, 1920x1200). The stage is stretched to W x H (case S) in both directions, so on a
// buffer taller than 16:9 the HUD would be taller than authored (x 1.5, y 1.667 at 1920x1200 [INFERENCE]). A whole-buffer
// viewport (x = y = 0, the full W and H) of mode C / L / R becomes a 16:9 band: height = W x 9/16 (ws::UiBlockHeight()),
// y = (H - band) / 2 (60 px at 1920x1200, 40 at 1280x800). Mode F is left alone. Bit 0x200 (Flash-space placement) and the
// edge pieces are off there: the buffer has no side room, and the game's own 16:9 layout already puts the pieces at the
// band's sides. Use mask 0x10C (268). Open, as at 1568: 3D-in-movie sub-rects, fades, vertical edge anchoring.
//
// Three mechanisms (section 15 has the evidence and the reasoning):
//
//  1. VIEWPORT SQUEEZE. D3D SetViewport is sub_824162D8(r3 device, f1 x, f2 y, f3 width, f4 height,
//     f5 min z, f6 max z) [read in the image: it clamps to the target, stores x/y/w/h at device
//     +12648..+12660, x scale = w/2 at +10504, x offset = w/2 + x at +10508, and re-applies the
//     scissor intersected with the viewport through sub_82415C80]. A viewport that spans the whole
//     back buffer (x = 0, width = W), set in the UI PHASE (renderer phase 0x20000000), is narrowed
//     to the middle 1280 px before the function runs. The stage then maps 1:1 to pixels (stage
//     x -> screen x + (W - 1280) / 2). What the movie-draw kinds are [read in the image]: kind 0/1 =
//     sub_82720908, the 2D draw (render states, then the swf context's vtable +8): NO viewport is set
//     inside, it draws with the viewport that is current; kind 2/3 = the 3D objects inside a movie
//     (sub_82725B20: grcViewport init + activate = a full-width set, one viewport per 3D object from
//     its rect, perspective from that rect's aspect, the lights pass, then the restore = another
//     full-width set); kind 5 = that lights pass (sub_827250A8, its 220x220 target); kind 6 = the 3D
//     scene path (sub_82724CF0, world icons). So what the 2D HUD / menus inherit is the LAST full-width
//     set before them: the UI phase's own viewport and the restores of kind 2/3. Scope:
//       * outside any movie draw in the UI phase (bit 8 = 0x100): the phase's main viewport and the
//         caller's set / restore around a movie (sub_82227428). This is what kind-0 content inherits;
//       * inside a Flash movie draw (entry of sub_82721258 .. the instruction after each of its eight
//         callers' `bl`): kind n is squeezed when bit n of mcla_ws_ui_squeeze is set and the movie's
//         mode is C / L / R (mode table below). Bits 2 / 3 squeeze the activate / restore sets of the
//         3D-in-movie passes: needed when their restore would otherwise leave a FULL-width viewport for
//         the 2D draw that follows (which of the two is right is the first run's question);
//       * never before the UI phase: in the log's first frame kinds 5 and 3 are drawn before phase
//         0x10000000 begins, kinds 6, 2 and 0 after phase 0x20000000 began.
//     Flash masks are NOT scissor draws (sub_82178D80, the RAGE SetScissor, has one caller, the
//     post-process helper sub_8233AB98), and the viewport's own scissor follows by itself, so no
//     scissor transform is needed (section 15.3).
//  2. MODES. Per movie draw: C centre (x' = (W - 1280) / 2), L left edge (x' = inset), R right edge
//     (x' = W - 1280 - inset), F full width (left stretched). The mode comes from the per-kind
//     string mcla_ws_ui_kind_modes (default CCCCCCFC: kind 6 = the 3D scene path = world icons, F)
//     and is overridden by the movie's NAME (UI manager BE32[0x8286D804]: buckets +60, count
//     +64 >> 16, node +0 name, +8 holder (holder[0] = movie), +12 next) through the built-in table
//     (NAVSYS2DMOVIE, NAVSYSMOVIE, NAVSYSTARGETICONMOVIE, 3DICONSMOVIE0..2 = F: they are placed by
//     the wide camera) and the cvar mcla_ws_ui_movie_modes ("NAME=M,NAME=M", cvar wins). Every
//     visible movie is logged once per (movie, kind) with its name, so a fade or a backdrop that
//     must cover the whole screen is one cvar entry (F) away: an F movie stays stretched, which a
//     fade / dim does not mind. The caller sub_82227428 sets NAVSYS2DMOVIE's own viewport BEFORE
//     the dispatcher call: the hook at 0x82227594 marks it so that set is left full width.
//  3. EDGE PIECES. The gauge movie (hud.swf, not named in the UI manager) holds the left and right
//     HUD groups, so a per-movie viewport cannot anchor them: hook mcla_hook_ws_ui_piece at
//     0x825FB484 in sub_825FB348 (the Flash player's draw of one instance, after it has computed
//     its world matrix: world x at instance +48, parent at +184, name char* at +196, first child
//     +188, next +192, alpha byte +79; prior art LARecomp / Renan, the offsets re-read here) moves
//     the instances of the table below by -(W - 1280)/2 + inset (left) or +(W - 1280)/2 - inset
//     (right) stage pixels: they are children of the clip named "HUD". The minimap's four clips
//     move only while the clip `ring` is not transparent (the expanded map stays centred). Only
//     while the last UI-phase viewport was a centre squeeze (stage px = screen px).
//
//  4. FLASH-SPACE PLACEMENT (bit 9 = 0x200, task hud-edges; WIDESCREEN.md section 16). The edge pieces of
//     3. cannot work with a squeezed viewport: the Flash vertex shader maps stage x in [0, 1280] to NDC
//     [-1, 1], the viewport is the clip volume, so a piece moved out of the 1280 block (x < 0 or > 1280
//     in a 1:1 stage) is cut flat at the block's edge (run U2: minimap at x = 144, speedometer at 1424).
//     With bit 0x200 the ONE UI-phase viewport set the 2D movie draws inherit is left FULL WIDTH: the
//     outside-a-movie set between the last kind 2 / 3 draw and the first kind 0 / 1 draw (traced in all
//     18 frames of the U2 / H2 logs: exactly one a frame, the HUD, hudmap, prompt and pause movies share
//     it, so a per-movie viewport is impossible). With bit 0x100 too (780 = 0x30C) every other outside
//     set stays squeezed: the 3D-in-movie content (title logo, pause panel: kind 6 / 2 passes that
//     never reach sub_825FB348, run H1 / H2 showed them stretched when their sets were left full) keeps
//     the squeeze; 0x200 alone leaves them all full (stretched). The 2D movies are placed in Flash
//     space instead: the hook of 3. also runs on the ROOT instance of every drawn movie (parent +184 == 0) of a
//     movie draw whose mode is C / L / R, and applies x' = x * s + off to its world matrix (row-vector
//     convention read in sub_825FB348: world a +32, b +36, c +40, d +44, tx +48, ty +52; the children
//     compute theirs from the parent's afterwards, so the whole subtree follows): a, c, tx scaled by
//     s = 1280 / W and tx += off (centre (W - 1280) / 2 * s, left inset * s, right (W - 1280 - inset) * s,
//     in stage px = W / 1280 screen px each). One stage px is W / 1280 screen px here, so the edge
//     pieces of 3. shift by ((W - 1280) / 2 - inset) * s. Only while the UI-phase viewport in effect is
//     full width and was left alone (thread flag t_ui_vp_full); with bit 0x100 (a squeezed viewport) the
//     pieces stay in the middle block (the old, clipped edge move is gone).
//
// Probes (all `[[midasm_hook]]` at function entries or call sites of recompiled functions):
//   * mcla_hook_ws_ui_movie_draw(r3 movie, r4 kind)   0x82721258  the Flash movie draw dispatcher
//   * mcla_hook_ws_ui_phase(r4 phase)                 0x822E55D0  sub_822E55D0(renderer, phase)
//   * mcla_hook_ws_ui_movie_done_1..8()               the instruction after each `bl sub_82721258`
//   * mcla_hook_ws_ui_viewport(f1 x, f2 y, f3 w, f4 h) 0x824162D8  D3D set viewport (the squeeze)
//   * mcla_hook_ws_ui_scissor()                       0x82413660  counts only
//   * mcla_hook_ws_ui_piece(r30 instance)             0x825FB484  NEW (needs a re-codegen)
//   * mcla_hook_ws_ui_navsys_viewport()               0x82227594  NEW (needs a re-codegen)
// Every 5 s of widescreen one `[mcla-ws-ui]` line: movie draws visible/total by kind, phases,
// viewport / scissor calls, squeezed counts (in a movie draw by kind, outside one), pieces moved.
//
// Off: widescreen off (the probes then cost one atomic load), --mcla_ws_ui_probe=false with
// mcla_ws_ui_squeeze=0, or MCLA_HOOK_WS_UI=0 at build time.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"
#include "mcla_ws_common.h"

REXCVAR_DEFINE_BOOL(mcla_ws_ui_probe, true, "MCLA",
                    "MCLA widescreen: count the Flash UI draws / phases / viewports, name the movies and log "
                    "them every 5 s (probe only, changes nothing by itself; only with widescreen on)");
REXCVAR_DEFINE_INT32(mcla_ws_ui_squeeze, 0, "MCLA",
                     "MCLA widescreen: the UI squeeze. Mask of what is narrowed to the middle 1280 px (the stage "
                     "keeps 16:9 proportions), only for viewports that span the whole back buffer and only in "
                     "the UI phase: bit 8 (0x100) = UI-phase viewports outside any movie draw (the phase's main "
                     "viewport, the caller's set / restore around a movie: what the 2D draws inherit); bit n "
                     "(0..7) = viewports set inside a Flash movie draw of kind n of sub_82721258 (0/1 2D draw, "
                     "2/3 3D objects in a movie, 5 their lights pass, 6 3D scene). 0 = off (default: the UI is "
                     "stretched with the buffer). First run: 0x100, compare 0x10C (+ kinds 2, 3) and 0x10F "
                     "(= 271, decimal for --set-at). bit 9 (0x200) = Flash-space placement: the one viewport set the 2D "
                     "movies inherit is left full width and each 2D movie is centred / scaled to 16:9 through its "
                     "root instance's matrix, which lets the HUD edge pieces reach the screen edges (task hud-edges; "
                     "780 = 0x30C = this + the other sets squeezed as 268 does; 512 alone stretches the 3D-in-movie "
                     "content). Live: works with --set-at");
REXCVAR_DEFINE_INT32(mcla_ws_ui_trace, 0, "MCLA",
                     "MCLA widescreen UI: trace the next N UI-phase frames: every movie draw (kind, name, mode) and "
                     "every set-viewport call in order, with what the squeeze did. Counts down by itself; set it "
                     "again (--set-at) at each screen of interest. Only with widescreen on");
REXCVAR_DEFINE_STRING(mcla_ws_ui_kind_modes, "CCCCCCFC", "MCLA",
                      "MCLA widescreen UI squeeze: mode per movie-draw kind 0..7, one letter each: C centre, "
                      "L left edge, R right edge, F full width (not squeezed). Default CCCCCCFC (kind 6 = the 3D "
                      "scene path = world icons placed by the wide camera). Shorter strings keep C");
REXCVAR_DEFINE_STRING(mcla_ws_ui_movie_modes, "", "MCLA",
                      "MCLA widescreen UI squeeze: mode by movie NAME, 'NAME=M,NAME=M' (M = C, L, R, F). Wins over "
                      "the kind table and the built-in table (NAVSYS2DMOVIE, NAVSYSMOVIE, NAVSYSTARGETICONMOVIE, "
                      "3DICONSMOVIE0/1/2 = F). The log names every visible movie ('movie 0x... \"NAME\" kind K "
                      "mode M'); a full-screen fade or backdrop that is cut to the middle wants NAME=F");
REXCVAR_DEFINE_BOOL(mcla_ws_hud_aspect, true, "MCLA",
                    "MCLA widescreen: master switch of the UI squeeze (with mcla_ws_ui_squeeze != 0): the UI "
                    "is drawn at 16:9 proportions in the middle of the wide screen (WIDESCREEN.md section 15)");
REXCVAR_DEFINE_BOOL(mcla_ws_hud_edges, true, "MCLA",
                    "MCLA widescreen UI squeeze: the HUD pieces of the left / right screen edge (minimap, lap "
                    "boxes; speedometer, position, player list) move out to the device edges (hook 0x825FB484; "
                    "needs the Flash-space placement, mcla_ws_ui_squeeze bit 0x200; with a squeezed viewport the "
                    "pieces would be cut at the block edge, so they stay). false = the whole HUD stays in the "
                    "middle block");
REXCVAR_DEFINE_INT32(mcla_ws_hud_edge_inset, 0, "MCLA",
                     "MCLA widescreen UI squeeze: pixels at 1x kept free at each side of the screen for the "
                     "edge HUD pieces (the game's own title-safe margin already keeps them ~5 % in; raise it "
                     "if the phone's corner radius clips the minimap)");

namespace {

mcla::hooks::Registration g_registration(
    "ws-ui", "mcla_ws_ui_probe", MCLA_HOOK_WS_UI != 0,
    [] { return mcla::ws::Armed() && (REXCVAR_GET(mcla_ws_ui_probe) || REXCVAR_GET(mcla_ws_ui_squeeze) != 0); },
    /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_WS_UI

namespace {

constexpr int kKinds = 8;
constexpr int kPhaseSlots = 48;  // run W-1: 33 phase begins a frame, 16 slots lost the rest
constexpr int64_t kReportNs = 5'000'000'000;  // 5 s
constexpr uint32_t kUiPhaseBit = 0x20000000u;
constexpr uint32_t kOutsideBit = 0x100u;  // mask bit: UI-phase viewports outside any movie draw
constexpr uint32_t kFlashSpaceBit = 0x200u;  // mask bit: movies placed in Flash space (viewports left full width)
constexpr uint32_t kUiManagerSlot = 0x8286D804u;

std::atomic<uint64_t> g_kind_calls[kKinds];
std::atomic<uint64_t> g_kind_visible[kKinds];
std::atomic<bool> g_kind_seen[kKinds];
std::atomic<uint64_t> g_phase_total{0};
std::atomic<uint64_t> g_viewport_calls{0};
std::atomic<uint64_t> g_scissor_calls{0};

struct PhaseSlot {
  std::atomic<uint64_t> key{0};  // phase + 1 (0 = free)
  std::atomic<uint64_t> calls{0};
};
PhaseSlot g_phase[kPhaseSlots];

std::atomic<int64_t> g_next_report_ns{0};

// ---- per-thread state (the UI is drawn by the render thread; every hook of this file runs there) ----
//
// The movie draw this thread is inside: a small stack (a movie draw inside a movie draw is not
// expected, the depth is only a guard), pushed by the dispatcher's entry hook, popped by the
// instruction after each of its callers' `bl`.
struct MovieFrame {
  uint32_t kind = 0;
  uint32_t movie = 0;
  char mode = 'F';       // C / L / R squeeze or placement, F leave
  bool hooked = false;   // the instance hook has run once inside this movie draw (its first call = the top)
};
constexpr int kMaxDepth = 4;
thread_local MovieFrame t_stack[kMaxDepth];
thread_local int t_movie_depth = 0;
thread_local uint32_t t_phase = 0;           // the last renderer phase begun on this thread
thread_local bool t_ui_phase = false;        // t_phase has the UI bit
thread_local bool t_ui_squeezed = false;     // the last UI-phase viewport was squeezed in mode C
thread_local bool t_ui_vp_full = false;      // the last UI-phase viewport is full width and was left alone
thread_local bool t_seen_3d = false;        // a kind 2 / 3 movie draw has completed in this UI phase
thread_local bool t_seen_2d = false;        // a kind 0 / 1 movie draw has completed in this UI phase
thread_local bool t_force_full = false;      // the next set-viewport is NAVSYS2DMOVIE's own (left full width)
thread_local bool t_trace = false;           // this UI-phase frame is traced (mcla_ws_ui_trace)

MovieFrame* TopFrame() {
  if (t_movie_depth <= 0) {
    return nullptr;
  }
  return &t_stack[std::min(t_movie_depth, kMaxDepth) - 1];
}

std::atomic<uint64_t> g_viewport_in_movie[kKinds];
std::atomic<uint64_t> g_scissor_in_movie[kKinds];
std::atomic<uint64_t> g_viewport_squeezed[kKinds];
std::atomic<uint64_t> g_viewport_outside_ui{0};          // UI-phase viewports outside a movie draw
std::atomic<uint64_t> g_viewport_outside_squeezed{0};
std::atomic<uint64_t> g_viewport_not_ui_full{0};         // full-width viewports left alone: not the UI phase
std::atomic<uint64_t> g_pieces_moved{0};
std::atomic<uint64_t> g_roots_placed{0};                // movie roots placed in Flash space
std::atomic<uint32_t> g_viewport_lines[2][kKinds];       // [in the UI phase][kind]
std::atomic<uint32_t> g_outside_lines{0};
constexpr uint32_t kViewportLinesPerKind = 6;
constexpr uint32_t kOutsideLines = 12;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Probes and the squeeze run only with widescreen on; the probe cvar or a non-zero mask turns them on.
bool Active() {
  MCLA_HOOK_ENTER();
  return mcla::ws::Width() != 0 && (REXCVAR_GET(mcla_ws_ui_probe) || REXCVAR_GET(mcla_ws_ui_squeeze) != 0);
}

// The squeeze mask in force (0 = nothing is squeezed).
uint32_t SqueezeMask() {
  return REXCVAR_GET(mcla_ws_hud_aspect) ? uint32_t(REXCVAR_GET(mcla_ws_ui_squeeze)) : 0u;
}

bool ValidMode(char c) { return c == 'C' || c == 'L' || c == 'R' || c == 'F'; }

// ---- mode tables ---------------------------------------------------------------------------------

struct NameMode {
  const char* name;
  char mode;
};
// Movies placed by the wide camera (world / GPS icons): the GPS 2D overlay and the 3D icon movies.
constexpr NameMode kBuiltinModes[] = {
    {"NAVSYS2DMOVIE", 'F'},        {"NAVSYSMOVIE", 'F'},     {"NAVSYSTARGETICONMOVIE", 'F'},
    {"3DICONSMOVIE0", 'F'},        {"3DICONSMOVIE1", 'F'},   {"3DICONSMOVIE2", 'F'},
};

std::mutex g_tables_mutex;
std::string g_kinds_src = "\x01";   // never equal to a cvar value: parsed on first use
std::string g_movies_src = "\x01";
char g_kind_mode[kKinds];
constexpr int kMaxUserModes = 32;
struct UserMode {
  char name[48];
  char mode;
};
UserMode g_user_modes[kMaxUserModes];
int g_user_count = 0;

// Parses the two cvar strings when they changed (under g_tables_mutex).
void RefreshTablesLocked() {
  const std::string kinds = REXCVAR_GET(mcla_ws_ui_kind_modes);
  if (kinds != g_kinds_src) {
    g_kinds_src = kinds;
    for (int k = 0; k < kKinds; ++k) {
      char c = 'C';
      if (size_t(k) < kinds.size()) {
        const char u = char(std::toupper(static_cast<unsigned char>(kinds[size_t(k)])));
        if (ValidMode(u)) {
          c = u;
        }
      }
      g_kind_mode[k] = c;
    }
  }
  const std::string movies = REXCVAR_GET(mcla_ws_ui_movie_modes);
  if (movies != g_movies_src) {
    g_movies_src = movies;
    g_user_count = 0;
    size_t pos = 0;
    while (pos < movies.size() && g_user_count < kMaxUserModes) {
      size_t end = movies.find_first_of(",; ", pos);
      if (end == std::string::npos) {
        end = movies.size();
      }
      const std::string token = movies.substr(pos, end - pos);
      pos = end + 1;
      const size_t eq = token.find('=');
      if (eq == std::string::npos || eq == 0 || eq + 1 >= token.size() || eq >= sizeof(UserMode::name)) {
        continue;
      }
      const char m = char(std::toupper(static_cast<unsigned char>(token[eq + 1])));
      if (!ValidMode(m)) {
        continue;
      }
      UserMode& u = g_user_modes[g_user_count++];
      std::memset(u.name, 0, sizeof(u.name));
      std::memcpy(u.name, token.data(), eq);
      u.mode = m;
    }
  }
}

// ---- movie names (the UI manager's map) ------------------------------------------------------------

constexpr int kNameSlots = 64;
constexpr int64_t kNameTtlNs = 2'000'000'000;  // a movie slot can be reused: look again every 2 s
struct NameSlot {
  uint32_t movie = 0;
  int64_t at_ns = 0;
  char name[40] = {};
};
NameSlot g_names[kNameSlots];

inline uint32_t Be32(uint32_t v) { return __builtin_bswap32(v); }

// The name the UI manager has for `movie`, "" when it has none (the gauge movie hud.swf is not in the
// map) or the map cannot be read. Prior art: Renan's aspect_ratio.cpp MovieNameAddress, offsets re-read.
void ResolveMovieName(uint32_t movie, char (&out)[40]) {
  out[0] = '\0';
  uint32_t manager = 0, buckets = 0, count = 0;
  if (!mcla::guest::U32(kUiManagerSlot, &manager) || manager < 0x10000 ||
      !mcla::guest::U32(manager + 60, &buckets) || !mcla::guest::U32(manager + 64, &count)) {
    return;
  }
  count >>= 16;
  if (buckets < 0x10000 || count == 0 || count > 256) {
    return;
  }
  uint32_t table[256];
  if (!mcla::guest::Read(buckets, table, size_t(count) * 4)) {
    return;
  }
  for (uint32_t b = 0; b < count; ++b) {
    uint32_t node = Be32(table[b]);
    for (int guard = 0; guard < 64 && node >= 0x10000; ++guard) {
      uint32_t words[4];  // +0 name, +4 ?, +8 holder, +12 next
      if (!mcla::guest::Read(node, words, sizeof(words))) {
        break;
      }
      const uint32_t holder = Be32(words[2]);
      uint32_t held = 0;
      if (holder >= 0x10000 && mcla::guest::U32(holder, &held) && held == movie) {
        mcla::guest::CString(Be32(words[0]), out, sizeof(out));
        return;
      }
      node = Be32(words[3]);
    }
  }
}

// Name + mode of a movie draw (kind k): the kind table, overridden by the name tables. `name` gets
// the name ("" if none).
char ModeFor(uint32_t movie, uint32_t kind, char (&name)[40]) {
  const int64_t now = NowNs();
  std::lock_guard<std::mutex> lock(g_tables_mutex);
  RefreshTablesLocked();
  NameSlot& slot = g_names[(movie >> 4) % kNameSlots];
  if (slot.movie != movie || now - slot.at_ns > kNameTtlNs) {
    slot.movie = movie;
    slot.at_ns = now;
    ResolveMovieName(movie, slot.name);
  }
  std::memcpy(name, slot.name, sizeof(name));
  char mode = kind < uint32_t(kKinds) ? g_kind_mode[kind] : 'F';
  if (name[0] != '\0') {
    bool found = false;
    for (int i = 0; i < g_user_count; ++i) {
      if (std::strcmp(g_user_modes[i].name, name) == 0) {
        mode = g_user_modes[i].mode;
        found = true;
        break;
      }
    }
    if (!found) {
      for (const NameMode& b : kBuiltinModes) {
        if (std::strcmp(b.name, name) == 0) {
          mode = b.mode;
          break;
        }
      }
    }
  }
  return mode;
}

// ---- HUD pieces ------------------------------------------------------------------------------------

struct Piece {
  const char* name;
  int8_t side;     // -1 left edge, +1 right edge
  bool minimap;    // moves only while the clip `ring` is not transparent
};
// hud.xsf: the script's widescreen branch moves these +-135 on the 1280 stage (+ right, - left), `chat`
// has no offset and sits top left; hudmap.xsf: the minimap's four clips (prior art LARecomp
// ultrawide.cpp kPieces from the movies' data; the names are not re-read from the .xsf here).
constexpr Piece kPieces[] = {
    {"info", +1, false},         {"glows", +1, false},     {"speedometer", +1, false},
    {"posmeter", +1, false},     {"playerList", +1, false}, {"delivery", +1, false},
    {"CTF_score", +1, false},    {"flagTime", -1, false},  {"flagsDelivered", -1, false},
    {"bomb", -1, false},         {"flag", -1, false},      {"lapmeter", -1, false},
    {"laptime", -1, false},      {"totaltime", -1, false}, {"chat", -1, false},
    {"miniBG", -1, true},        {"ring", -1, true},       {"ringNorth", -1, true},
    {"renderMe", -1, true},
};
constexpr int kPieceCount = int(sizeof(kPieces) / sizeof(kPieces[0]));

// Instance (rage swfINSTANCE) offsets: the parent at +184, first child +188, next sibling +192, name char*
// +196 (sub_8260B250 stores it: `stw r5,196(r30)`), alpha byte +79, world x float +48 (sub_825FB348
// stores it at 0x825FB430).
constexpr uint32_t kInstAlpha = 79, kInstWorldA = 32, kInstWorldC = 40, kInstWorldX = 48, kInstParent = 184,
                   kInstFirstChild = 188, kInstNext = 192, kInstName = 196;

struct PieceSlot {
  uint32_t inst = 0;
  int64_t at_ns = 0;
  int8_t piece = -2;       // -2 unknown, -1 not a piece, else the kPieces index
  uint32_t parent = 0;
  uint32_t ring = 0;       // the sibling clip `ring` (minimap pieces), 0 = not found yet
  bool root = false;       // parent +184 == 0: the top of a movie's instance tree
};
constexpr uint32_t kNoRing = 0xFFFFFFFFu;
constexpr int kPieceSlots = 4096;  // every drawn instance of a 2D movie goes through here (menus ~1000)
constexpr int64_t kPieceTtlNs = 500'000'000;
std::mutex g_piece_mutex;
PieceSlot g_piece_cache[kPieceSlots];
std::atomic<uint32_t> g_pieces_logged{0};
std::atomic<uint32_t> g_kept_logged{0};
char g_kept_names[16][40];
std::mutex g_kept_mutex;

bool ReadName(uint32_t instance, char (&out)[40]) {
  out[0] = '\0';
  uint32_t name_ptr = 0;
  if (instance < 0x10000 || !mcla::guest::U32(instance + kInstName, &name_ptr) || name_ptr < 0x10000) {
    return false;
  }
  return mcla::guest::CString(name_ptr, out, sizeof(out)) >= 0;
}

// Classifies the instance (child of a clip named "HUD", name in the table); fills the slot.
void ClassifyPiece(uint32_t instance, PieceSlot& slot) {
  slot.piece = -1;
  slot.parent = 0;
  slot.ring = 0;
  slot.root = false;
  uint32_t parent = 0;
  if (!mcla::guest::U32(instance + kInstParent, &parent)) {
    return;
  }
  slot.root = parent == 0;  // sub_825FB348's own walk up the parents ends at +184 == 0
  if (parent < 0x10000) {
    return;
  }
  char pname[40];
  if (!ReadName(parent, pname) || std::strcmp(pname, "HUD") != 0) {
    return;
  }
  slot.parent = parent;
  char name[40];
  if (!ReadName(instance, name) || name[0] == '\0') {
    return;
  }
  for (int i = 0; i < kPieceCount; ++i) {
    if (std::strcmp(name, kPieces[i].name) == 0) {
      slot.piece = int8_t(i);
      return;
    }
  }
  // Another child of the HUD clip: stays in the middle. The first few are named in the log so that a
  // piece the table missed shows up.
  std::lock_guard<std::mutex> lock(g_kept_mutex);
  const uint32_t n = g_kept_logged.load(std::memory_order_relaxed);
  if (n < 16) {
    for (uint32_t i = 0; i < n; ++i) {
      if (std::strcmp(g_kept_names[i], name) == 0) {
        return;
      }
    }
    std::memcpy(g_kept_names[n], name, sizeof(name));
    g_kept_logged.store(n + 1, std::memory_order_relaxed);
    REXLOG_INFO("[mcla-ws-ui] HUD clip \"{}\" stays in the middle ({} of 16 logged)", name, n + 1);
  }
}

// The sibling `ring` of a minimap piece (its alpha says whether the minimap shows): a bounded walk.
uint32_t FindRing(uint32_t parent) {
  uint32_t child = 0;
  if (!mcla::guest::U32(parent + kInstFirstChild, &child)) {
    return 0;
  }
  for (int guard = 0; guard < 64 && child >= 0x10000; ++guard) {
    char name[40];
    if (ReadName(child, name) && std::strcmp(name, "ring") == 0) {
      return child;
    }
    if (!mcla::guest::U32(child + kInstNext, &child)) {
      break;
    }
  }
  return 0;
}

// One line every 5 s, written by whichever probe gets there first; the counts are the change since
// the previous line.
void MaybeReport() {
  if (!REXCVAR_GET(mcla_ws_ui_probe)) {
    return;
  }
  const int64_t now = NowNs();
  int64_t next = g_next_report_ns.load(std::memory_order_relaxed);
  if (next == 0) {  // the first probe call arms the timer
    g_next_report_ns.compare_exchange_strong(next, now + kReportNs);
    return;
  }
  if (now < next || !g_next_report_ns.compare_exchange_strong(next, now + kReportNs)) {
    return;
  }
  // Only the thread that advanced the timer gets here (once per 5 s): it owns the 'previous' state.
  static uint64_t prev_calls[kKinds], prev_visible[kKinds], prev_viewport, prev_scissor, prev_phase_total;
  static uint64_t prev_phase_calls[kPhaseSlots];
  static uint64_t prev_frames;
  static uint64_t prev_outside, prev_outside_sq, prev_not_ui, prev_pieces, prev_roots;

  char kinds[256];
  size_t used = 0;
  kinds[0] = '\0';
  for (int k = 0; k < kKinds && used + 32 < sizeof(kinds); ++k) {
    const uint64_t calls = g_kind_calls[k].load(std::memory_order_relaxed);
    const uint64_t visible = g_kind_visible[k].load(std::memory_order_relaxed);
    if (calls != prev_calls[k]) {
      used += size_t(std::snprintf(kinds + used, sizeof(kinds) - used, " k%d %llu/%llu", k,
                                   (unsigned long long)(visible - prev_visible[k]),
                                   (unsigned long long)(calls - prev_calls[k])));
    }
    prev_calls[k] = calls;
    prev_visible[k] = visible;
  }
  char phases[768];
  used = 0;
  phases[0] = '\0';
  for (int i = 0; i < kPhaseSlots && used + 32 < sizeof(phases); ++i) {
    const uint64_t key = g_phase[i].key.load(std::memory_order_relaxed);
    const uint64_t calls = g_phase[i].calls.load(std::memory_order_relaxed);
    if (key != 0 && calls != prev_phase_calls[i]) {
      used += size_t(std::snprintf(phases + used, sizeof(phases) - used, " 0x%X:%llu", unsigned(key - 1),
                                   (unsigned long long)(calls - prev_phase_calls[i])));
    }
    prev_phase_calls[i] = calls;
  }
  const uint64_t viewport = g_viewport_calls.load(std::memory_order_relaxed);
  const uint64_t scissor = g_scissor_calls.load(std::memory_order_relaxed);
  const uint64_t phase_total = g_phase_total.load(std::memory_order_relaxed);
  const uint64_t frames = mcla::FrameCount();
  static uint64_t prev_in_movie[kKinds], prev_scissor_in[kKinds], prev_squeezed[kKinds];
  char inside[384];
  used = 0;
  inside[0] = '\0';
  for (int k = 0; k < kKinds && used + 48 < sizeof(inside); ++k) {
    const uint64_t v = g_viewport_in_movie[k].load(std::memory_order_relaxed);
    const uint64_t c = g_scissor_in_movie[k].load(std::memory_order_relaxed);
    const uint64_t q = g_viewport_squeezed[k].load(std::memory_order_relaxed);
    if (v != prev_in_movie[k] || c != prev_scissor_in[k]) {
      used += size_t(std::snprintf(inside + used, sizeof(inside) - used, " k%d vp %llu (squeezed %llu) sc %llu", k,
                                   (unsigned long long)(v - prev_in_movie[k]),
                                   (unsigned long long)(q - prev_squeezed[k]),
                                   (unsigned long long)(c - prev_scissor_in[k])));
    }
    prev_in_movie[k] = v;
    prev_scissor_in[k] = c;
    prev_squeezed[k] = q;
  }
  const uint64_t outside = g_viewport_outside_ui.load(std::memory_order_relaxed);
  const uint64_t outside_sq = g_viewport_outside_squeezed.load(std::memory_order_relaxed);
  const uint64_t not_ui = g_viewport_not_ui_full.load(std::memory_order_relaxed);
  const uint64_t pieces = g_pieces_moved.load(std::memory_order_relaxed);
  const uint64_t roots = g_roots_placed.load(std::memory_order_relaxed);
  REXLOG_INFO("[mcla-ws-ui] 5 s (squeeze mask 0x{:X}): engine frames +{}; movie draws visible/total by "
              "kind:{}; phase begins +{}{}; set viewport +{}, set scissor +{}; inside a movie draw:{}; UI-phase "
              "viewports outside a movie draw +{} (squeezed +{}); full-width viewports left alone outside the UI "
              "phase +{}; HUD pieces moved +{}; movie roots placed in Flash space +{}",
              SqueezeMask(), frames - prev_frames, kinds[0] ? kinds : " none", phase_total - prev_phase_total,
              phases, viewport - prev_viewport, scissor - prev_scissor, inside[0] ? inside : " none",
              outside - prev_outside, outside_sq - prev_outside_sq, not_ui - prev_not_ui, pieces - prev_pieces,
              roots - prev_roots);
  prev_viewport = viewport;
  prev_scissor = scissor;
  prev_phase_total = phase_total;
  prev_frames = frames;
  prev_outside = outside;
  prev_outside_sq = outside_sq;
  prev_not_ui = not_ui;
  prev_pieces = pieces;
  prev_roots = roots;
}

std::atomic<uint32_t> g_movie_lines{0};
constexpr uint32_t kMaxMovieLines = 96;

}  // namespace

void mcla_hook_ws_ui_movie_draw(PPCRegister& r3, PPCRegister& r4) {
  if (!Active()) {
    return;
  }
  const uint32_t movie = r3.u32;
  const uint32_t kind = r4.u32;
  MovieFrame frame;
  frame.kind = kind;
  frame.movie = movie;
  frame.mode = 'F';
  if (kind < uint32_t(kKinds)) {
    g_kind_calls[kind].fetch_add(1, std::memory_order_relaxed);
    uint8_t visible = 0;
    const bool read = mcla::guest::U8(movie + 68, &visible);  // the dispatcher returns when it is 0
    if (read && visible != 0) {
      g_kind_visible[kind].fetch_add(1, std::memory_order_relaxed);
      char name[40];
      frame.mode = ModeFor(movie, kind, name);
      if (!g_kind_seen[kind].exchange(true)) {
        REXLOG_INFO("[mcla-ws-ui] first visible movie draw of kind {}: movie 0x{:08X} \"{}\"", kind, movie, name);
      }
      // Each (movie, kind) once: the name table of a run (which movie is drawn where, in which phase).
      static std::mutex seen_mutex;
      static uint64_t seen[256];
      static size_t seen_count = 0;
      const uint64_t key = (uint64_t(movie) << 8) | kind;
      std::lock_guard<std::mutex> lock(seen_mutex);
      if (g_movie_lines.load(std::memory_order_relaxed) < kMaxMovieLines &&
          std::find(seen, seen + seen_count, key) == seen + seen_count && seen_count < 256) {
        seen[seen_count++] = key;
        g_movie_lines.fetch_add(1, std::memory_order_relaxed);
        REXLOG_INFO("[mcla-ws-ui] movie 0x{:08X} \"{}\" kind {} phase 0x{:08X}{}: mode {}", movie,
                    name[0] ? name : "(unnamed)", kind, t_phase, t_ui_phase ? " (UI phase)" : "", frame.mode);
      }
    }
  }
  if (t_trace) {
    uint8_t vis = 0;
    if (mcla::guest::U8(movie + 68, &vis) && vis != 0) {
      char tname[40];
      const char mode = ModeFor(movie, kind, tname);
      REXLOG_INFO("[mcla-ws-ui] trace: movie draw 0x{:08X} \"{}\" kind {} mode {} (depth {})", movie,
                  tname[0] ? tname : "(unnamed)", kind, mode, t_movie_depth + 1);
    }
  }
  if (t_movie_depth < kMaxDepth) {
    t_stack[t_movie_depth] = frame;
  }
  ++t_movie_depth;
  MaybeReport();
}

void mcla_hook_ws_ui_phase(PPCRegister& r4) {
  if (!Active()) {
    return;
  }
  const uint32_t phase = r4.u32;
  g_phase_total.fetch_add(1, std::memory_order_relaxed);
  // A phase begin is the only place the UI scope can end: reset what a phase may leave open.
  t_phase = phase;
  t_ui_phase = (phase & kUiPhaseBit) != 0;
  t_ui_squeezed = false;
  t_ui_vp_full = false;
  t_seen_3d = false;
  t_seen_2d = false;
  t_force_full = false;
  t_movie_depth = 0;
  t_trace = false;
  if (t_ui_phase) {
    const int32_t left = REXCVAR_GET(mcla_ws_ui_trace);
    if (left > 0) {
      REXCVAR_SET(mcla_ws_ui_trace, left - 1);
      t_trace = true;
      REXLOG_INFO("[mcla-ws-ui] trace: the UI phase begins (engine frame {}, squeeze mask 0x{:X}, {} traces left)",
                  mcla::FrameCount(), SqueezeMask(), left - 1);
    }
  }
  const uint64_t key = uint64_t(phase) + 1;
  for (int i = 0; i < kPhaseSlots; ++i) {
    uint64_t expected = g_phase[i].key.load(std::memory_order_relaxed);
    if (expected == 0 && g_phase[i].key.compare_exchange_strong(expected, key)) {
      REXLOG_INFO("[mcla-ws-ui] first renderer phase begin 0x{:08X} (UI phase bit 0x20000000: {})", phase,
                  (phase & kUiPhaseBit) ? "set" : "clear");
      expected = key;
    }
    if (expected == key) {
      g_phase[i].calls.fetch_add(1, std::memory_order_relaxed);
      break;
    }
  }
  MaybeReport();
}

// The instruction after each `bl sub_82721258` (eight callers, one hook name each): the movie draw
// is over. Not gated: the depth only ever goes down to 0.
namespace {
inline void MovieDone() {
  if (t_trace && t_movie_depth > 0) {
    REXLOG_INFO("[mcla-ws-ui] trace: movie draw of kind {} done (depth {})",
                t_stack[std::min(t_movie_depth, kMaxDepth) - 1].kind, t_movie_depth);
  }
  if (t_movie_depth > 0) {
    const MovieFrame& top = t_stack[std::min(t_movie_depth, kMaxDepth) - 1];
    if (t_ui_phase) {
      if (top.kind == 2 || top.kind == 3) {
        t_seen_3d = true;
      } else if (top.kind == 0 || top.kind == 1) {
        t_seen_2d = true;
      }
    }
    --t_movie_depth;
  }
  t_force_full = false;
}
}  // namespace
void mcla_hook_ws_ui_movie_done_1() { MovieDone(); }  // 0x821FB4F0  sub_821FB1C8
void mcla_hook_ws_ui_movie_done_2() { MovieDone(); }  // 0x821FB518  sub_821FB1C8
void mcla_hook_ws_ui_movie_done_3() { MovieDone(); }  // 0x821FB55C  sub_821FB1C8
void mcla_hook_ws_ui_movie_done_4() { MovieDone(); }  // 0x82225114  sub_822250E0
void mcla_hook_ws_ui_movie_done_5() { MovieDone(); }  // 0x8222522C  sub_82225170
void mcla_hook_ws_ui_movie_done_6() { MovieDone(); }  // 0x822275CC  sub_82227428
void mcla_hook_ws_ui_movie_done_7() { MovieDone(); }  // 0x8223D7E4  sub_8223D780
void mcla_hook_ws_ui_movie_done_8() { MovieDone(); }  // 0x8223D824  sub_8223D780

// sub_82227428's UI-phase branch, NAVSYS2DMOVIE: `li r4,1; addi r3,r25,12700; bl sub_8217C658` sets the
// movie's grcViewport, then FindMovie("NAVSYS2DMOVIE") + the dispatcher (kind 0), then the restore. The
// set comes BEFORE the dispatcher call, outside the bracket: this marks it (cleared by the instruction
// after the dispatcher's `bl`, movie_done_6), so it stays full width like the movie's own mode F.
void mcla_hook_ws_ui_navsys_viewport() {
  if (!Active()) {
    return;
  }
  t_force_full = true;
}

// Entry of sub_824162D8 (D3D set viewport): f1 x, f2 y, f3 width, f4 height, in pixels.
void mcla_hook_ws_ui_viewport(PPCRegister& f1, PPCRegister& f2, PPCRegister& f3, PPCRegister& f4) {
  if (!Active()) {
    return;
  }
  g_viewport_calls.fetch_add(1, std::memory_order_relaxed);
  const double wide = double(mcla::ws::Width());
  const double high = double(mcla::ws::Height());
  const bool tall = mcla::ws::Tall();
  const double x = f1.f64, width = f3.f64;
  // A candidate spans the whole buffer: x = 0 and the full width; on a tall buffer also y = 0 and the full height (the
  // 3D objects inside a movie get rects as wide as the buffer too, and must not become a band).
  bool full_width = x > -0.5 && x < 0.5 && width > wide - 0.5 && width < wide + 0.5;
  if (tall) {
    full_width = full_width && f2.f64 > -0.5 && f2.f64 < 0.5 && f4.f64 > high - 0.5 && f4.f64 < high + 0.5;
  }
  const MovieFrame* frame = TopFrame();
  const bool ui = t_ui_phase;
  // Flash-space placement (bit 0x200) needs side room that a tall buffer does not have: ignored there.
  const uint32_t mask = tall ? (SqueezeMask() & ~kFlashSpaceBit) : SqueezeMask();
  const uint32_t kind = frame ? std::min<uint32_t>(frame->kind, kKinds) : 0;
  const bool in_movie = frame != nullptr && frame->kind < uint32_t(kKinds);
  if (in_movie) {
    g_viewport_in_movie[kind].fetch_add(1, std::memory_order_relaxed);
  } else if (ui) {
    g_viewport_outside_ui.fetch_add(1, std::memory_order_relaxed);
  }

  // The mode this set gets: only a full-width viewport in the UI phase is a candidate.
  char mode = 'F';
  const char* why = "";
  if (full_width) {
    if (!ui) {
      g_viewport_not_ui_full.fetch_add(1, std::memory_order_relaxed);
      why = " (not the UI phase: left)";
    } else if (in_movie) {
      if (((mask >> kind) & 1u) != 0) {
        mode = frame->mode;
        if (mode == 'F') {
          why = " (mode F: left)";
        }
      } else {
        why = " (kind not in the mask)";
      }
    } else if (frame != nullptr) {
      why = " (kind out of range)";
    } else if (t_force_full) {
      why = " (NAVSYS2DMOVIE's own viewport: left)";
    } else if ((mask & kFlashSpaceBit) != 0 && t_seen_3d && !t_seen_2d) {
      // The one set between the 3D-in-movie passes (kinds 2 / 3) and the first 2D movie draw (kind 0 / 1): the
      // 2D draws inherit it. Left full width; those movies are placed in Flash space (the hook of 3.).
      why = " (outside a movie draw, before the 2D movies: left full width, they are placed in Flash space)";
    } else if ((mask & kOutsideBit) != 0) {
      mode = 'C';
    } else if ((mask & kFlashSpaceBit) != 0) {
      why = " (outside a movie draw: left full width, movies are placed in Flash space)";
    } else {
      why = " (outside a movie draw, bit 0x100 off)";
    }
  }
  // A buffer that is not wider than its own 16:9 block (1920x1080, 1600x900) has no side area to squeeze into; a tall
  // one has no side area either but a top and bottom one: its viewport becomes a 16:9 band instead.
  const bool mode_changes = mode == 'C' || mode == 'L' || mode == 'R';
  const bool squeeze_wide = mode_changes && !tall && wide > mcla::ws::UiBlockWidth() + 0.5;
  const bool squeeze_tall = mode_changes && tall;
  const bool squeeze = squeeze_wide || squeeze_tall;

  // Log the first few rectangles: per kind in a movie draw, and outside one in the UI phase.
  bool log_it = t_trace && ui;
  if (log_it) {
    // traced frame: every set in order (the rate limit below is for the untraced first sets)
  } else if (in_movie) {
    std::atomic<uint32_t>& lines = g_viewport_lines[ui ? 1 : 0][kind];
    log_it = lines.load(std::memory_order_relaxed) < kViewportLinesPerKind &&
             lines.fetch_add(1, std::memory_order_relaxed) < kViewportLinesPerKind;
  } else if (ui) {
    log_it = g_outside_lines.load(std::memory_order_relaxed) < kOutsideLines &&
             g_outside_lines.fetch_add(1, std::memory_order_relaxed) < kOutsideLines;
  }
  if (log_it) {
    char where[48];
    if (in_movie) {
      std::snprintf(where, sizeof(where), "inside a movie draw of kind %u", kind);
    } else {
      std::snprintf(where, sizeof(where), "outside a movie draw");
    }
    REXLOG_INFO("[mcla-ws-ui] viewport {}{} (phase 0x{:08X}): x {:.1f} y {:.1f} {:.1f}x{:.1f}{}{}{}", where,
                ui ? " in the UI phase" : "", t_phase, x, f2.f64, width, f4.f64, full_width ? " (full width)" : "",
                squeeze ? " -> squeezed, mode " : "", squeeze ? (mode == 'C' ? "C" : mode == 'L' ? "L" : "R") : why);
  }

  if (ui) {
    t_ui_squeezed = squeeze && mode == 'C';  // the stage is 1:1 in the middle block: nothing may leave it
    t_ui_vp_full = full_width && !squeeze;   // stage = W / 1280 px per stage px: Flash-space placement applies
  }
  if (!squeeze) {
    return;
  }
  if (squeeze_tall) {
    // A tall buffer: the whole-buffer viewport becomes the 16:9 band, centred vertically (60 px at 1920x1200, 40 at
    // 1280x800). x and the width stay: the stage's 1280 stage px still span the width, the band keeps its shape.
    const double band = mcla::ws::UiBlockHeight();
    f4.f64 = double(float(band));
    f2.f64 = double(float((high - band) * 0.5));
    static std::atomic<bool> announced{false};
    if (!announced.exchange(true)) {
      REXLOG_INFO("[mcla-ws-ui] tall buffer {}x{}: whole-buffer UI viewports become a {:.0f}-row band at y {:.0f} (the "
                  "16:9 block of the width, mask 0x{:X})",
                  mcla::ws::Width(), mcla::ws::Height(), band, (high - band) * 0.5, mask);
    }
    if (in_movie) {
      g_viewport_squeezed[kind].fetch_add(1, std::memory_order_relaxed);
    } else {
      g_viewport_outside_squeezed.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  const double base = mcla::ws::UiBlockWidth();  // the middle 16:9 block: 1280 px at 720 lines
  const double inset = std::clamp(double(REXCVAR_GET(mcla_ws_hud_edge_inset)), 0.0, std::max(0.0, (wide - base) * 0.5));
  double new_x = (wide - base) * 0.5;
  if (mode == 'L') {
    new_x = inset;
  } else if (mode == 'R') {
    new_x = std::max(0.0, wide - base - inset);
  }
  f3.f64 = double(float(base));  // width' = W x k with k = base / W
  f1.f64 = double(float(new_x));
  if (in_movie) {
    g_viewport_squeezed[kind].fetch_add(1, std::memory_order_relaxed);
  } else {
    g_viewport_outside_squeezed.fetch_add(1, std::memory_order_relaxed);
  }
}

void mcla_hook_ws_ui_scissor() {
  if (!Active()) {
    return;
  }
  g_scissor_calls.fetch_add(1, std::memory_order_relaxed);
  const MovieFrame* frame = TopFrame();
  if (frame != nullptr && frame->kind < uint32_t(kKinds)) {
    g_scissor_in_movie[frame->kind].fetch_add(1, std::memory_order_relaxed);
  }
}

namespace {

float AsFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}
uint32_t AsBits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  return bits;
}

// x' = x * s + off on the instance's WORLD matrix (row vectors: x' = x * a + y * c + tx, a +32, c +40,
// tx +48 as computed by sub_825FB348): a and c scaled by s, tx scaled and offset. The children compute
// their matrices from this one afterwards, so the whole subtree follows.
bool PlaceWorld(uint32_t instance, double s, double off, float* a_out, float* tx_out) {
  uint32_t ba = 0, bc = 0, btx = 0;
  if (!mcla::guest::U32(instance + kInstWorldA, &ba) || !mcla::guest::U32(instance + kInstWorldC, &bc) ||
      !mcla::guest::U32(instance + kInstWorldX, &btx)) {
    return false;
  }
  const float a = AsFloat(ba), c = AsFloat(bc), tx = AsFloat(btx);
  if (a_out != nullptr) {
    *a_out = a;
  }
  if (tx_out != nullptr) {
    *tx_out = tx;
  }
  return mcla::ws::WriteGuestU32(instance + kInstWorldA, AsBits(float(double(a) * s))) &&
         mcla::ws::WriteGuestU32(instance + kInstWorldC, AsBits(float(double(c) * s))) &&
         mcla::ws::WriteGuestU32(instance + kInstWorldX, AsBits(float(double(tx) * s + off)));
}

std::atomic<uint32_t> g_root_lines{0};
constexpr uint32_t kMaxRootLines = 32;

// What the instance hook sees first in each movie draw, once per (movie, kind, UI phase, viewport state):
// the instance, its parent, its name. Tells which draws traverse instances at all and whether the top has
// parent +184 == 0 (a draw that never shows up here does not go through sub_825FB348).
void LogFirstInstance(const MovieFrame& f, uint32_t instance) {
  static std::mutex mutex;
  static uint64_t seen[128];
  static size_t seen_count = 0;
  const uint64_t key = (uint64_t(f.movie) << 8) | (f.kind << 2) | (t_ui_phase ? 2u : 0u) | (t_ui_vp_full ? 1u : 0u);
  std::lock_guard<std::mutex> lock(mutex);
  if (seen_count >= 128 || std::find(seen, seen + seen_count, key) != seen + seen_count) {
    return;
  }
  seen[seen_count++] = key;
  uint32_t parent = 0;
  const bool have_parent = mcla::guest::U32(instance + kInstParent, &parent);
  char name[40];
  ReadName(instance, name);
  char mname[40];
  ModeFor(f.movie, f.kind, mname);
  REXLOG_INFO("[mcla-ws-ui] first instance of movie 0x{:08X} \"{}\" kind {} (phase 0x{:08X}{}, mode {}, viewport full "
              "and left alone: {}): instance 0x{:08X} \"{}\" parent 0x{:08X}{}",
              f.movie, mname[0] ? mname : "(unnamed)", f.kind, t_phase, t_ui_phase ? " UI" : "", f.mode,
              t_ui_vp_full ? "yes" : "no", instance, name, parent, have_parent ? "" : " (unreadable)");
}

}  // namespace

// sub_825FB348 after the instance's world matrix is computed and before it (and its children) draw:
// r30 = the instance. Two jobs, only in the Flash-space regime (mask bit 0x200, the UI-phase viewport in
// effect is full width, the movie's mode is C / L / R, kind of any 2D or 3D movie draw):
//   * the ROOT instance of a movie (parent +184 == 0): x' = x * s + off, s = 1280 / W, off = the mode's
//     left margin in stage px (centre (W - 1280) / 2 * s): the whole movie is centred at 16:9;
//   * the HUD edge pieces (children of the clip "HUD", mode C): a further +-((W - 1280) / 2 - inset) * s.
// The hot path is the first line: nothing happens unless the last UI-phase viewport was left full width.
void mcla_hook_ws_ui_piece(PPCRegister& r30) {
  MovieFrame* frame = TopFrame();
  if (frame == nullptr || frame->kind >= uint32_t(kKinds)) {
    return;
  }
  const bool first_call = !frame->hooked;  // the first instance a movie draw visits is the top of its tree
  if (!first_call && !t_ui_vp_full) {
    return;  // the common case outside the Flash-space regime: two thread-local reads
  }
  if (!Active()) {
    return;
  }
  frame->hooked = true;
  if (first_call && t_ui_phase) {
    LogFirstInstance(*frame, r30.u32);
  }
  // No side room on a tall buffer: the game's own 16:9 layout already puts the pieces at the band's sides.
  if (!t_ui_vp_full || (SqueezeMask() & kFlashSpaceBit) == 0 || mcla::ws::Tall()) {
    return;
  }
  const char mode = frame->mode;
  if (mode != 'C' && mode != 'L' && mode != 'R') {
    return;
  }
  const double base = mcla::ws::UiBlockWidth();  // the middle 16:9 block in screen px
  const double wide = double(mcla::ws::Width());
  if (wide <= base) {
    return;
  }
  const uint32_t instance = r30.u32;
  const int64_t now = NowNs();
  int piece = -1;
  bool root = false;
  uint32_t ring = 0;
  uint32_t parent = 0;
  {
    std::lock_guard<std::mutex> lock(g_piece_mutex);
    PieceSlot& slot = g_piece_cache[(instance >> 4) % kPieceSlots];
    if (slot.inst != instance || slot.piece == -2 || now - slot.at_ns > kPieceTtlNs) {
      slot.inst = instance;
      slot.at_ns = now;
      ClassifyPiece(instance, slot);
    }
    piece = slot.piece;
    parent = slot.parent;
    root = slot.root || first_call;  // parent +184 == 0, or simply the first instance of this draw
    if (piece >= 0 && kPieces[piece].minimap && slot.ring == 0) {
      slot.ring = FindRing(parent);
      if (slot.ring == 0) {
        slot.ring = kNoRing;  // not found: do not walk again before the slot is refreshed
      }
    }
    ring = slot.ring;
  }
  const double s = double(mcla::ws::sizing::kBaseWidth) / wide;  // stage px per screen px: the 1280-px stage spans the buffer here
  const double inset = std::clamp(double(REXCVAR_GET(mcla_ws_hud_edge_inset)), 0.0, std::max(0.0, (wide - base) * 0.5));
  if (root) {
    double margin = (wide - base) * 0.5;  // screen px left of the middle block
    if (mode == 'L') {
      margin = inset;
    } else if (mode == 'R') {
      margin = std::max(0.0, wide - base - inset);
    }
    float a = 0.0f, tx = 0.0f;
    if (!PlaceWorld(instance, s, margin * s, &a, &tx)) {
      return;
    }
    g_roots_placed.fetch_add(1, std::memory_order_relaxed);
    const bool first_lines = g_root_lines.load(std::memory_order_relaxed) < kMaxRootLines;
    if (first_lines || t_trace) {
      static std::mutex seen_mutex;
      static uint64_t seen[64];
      static size_t seen_count = 0;
      const uint64_t key = (uint64_t(frame->movie) << 8) | frame->kind;
      std::lock_guard<std::mutex> lock(seen_mutex);
      const bool fresh = std::find(seen, seen + seen_count, key) == seen + seen_count && seen_count < 64;
      if (fresh || t_trace) {
        if (fresh) {
          seen[seen_count++] = key;
          g_root_lines.fetch_add(1, std::memory_order_relaxed);
        }
        char name[40];
        ReadName(instance, name);
        REXLOG_INFO("[mcla-ws-ui] {}movie 0x{:08X} kind {} root instance 0x{:08X} \"{}\" placed in Flash space: mode {}, "
                    "x' = x * {:.4f} + {:.2f} stage px (world a {:.4f} -> {:.4f}, tx {:.2f} -> {:.2f})",
                    t_trace ? "trace: " : "", frame->movie, frame->kind, instance, name, mode, s, margin * s,
                    double(a), double(a) * s, double(tx), double(tx) * s + margin * s);
      }
    }
    return;
  }
  if (piece < 0 || mode != 'C' || !REXCVAR_GET(mcla_ws_hud_edges)) {
    return;
  }
  const Piece& p = kPieces[piece];
  if (p.minimap) {
    // The expanded map keeps the minimap's clips in the middle: they are anchored only while `ring` shows.
    uint8_t alpha = 0;
    if (ring == 0 || ring == kNoRing || !mcla::guest::U8(ring + kInstAlpha, &alpha) || alpha == 0) {
      return;
    }
  }
  // Screen px from the middle block's edge to the screen edge, in stage px (the root's scale is in the matrix).
  const double shift_px = std::max(0.0, (wide - base) * 0.5 - inset);
  const float shift = float(shift_px * s);
  if (shift <= 0.0f) {
    return;
  }
  uint32_t bits = 0;
  if (!mcla::guest::U32(instance + kInstWorldX, &bits)) {
    return;
  }
  const float world_x = AsFloat(bits) + float(p.side) * shift;
  if (!mcla::ws::WriteGuestU32(instance + kInstWorldX, AsBits(world_x))) {
    return;
  }
  g_pieces_moved.fetch_add(1, std::memory_order_relaxed);
  const uint32_t bit = 1u << piece;
  if ((g_pieces_logged.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
    REXLOG_INFO("[mcla-ws-ui] HUD piece \"{}\" ({}) -> {} edge by {:.1f} screen px = {:.1f} stage px (inset {})", p.name,
                p.minimap ? "minimap" : "gauge", p.side < 0 ? "left" : "right", shift_px, double(shift), int(inset));
  }
}

#else  // MCLA_HOOK_WS_UI == 0: keeps hooks_widescreen.toml linking, does nothing.

void mcla_hook_ws_ui_movie_draw(PPCRegister&, PPCRegister&) {}
void mcla_hook_ws_ui_phase(PPCRegister&) {}
void mcla_hook_ws_ui_movie_done_1() {}
void mcla_hook_ws_ui_movie_done_2() {}
void mcla_hook_ws_ui_movie_done_3() {}
void mcla_hook_ws_ui_movie_done_4() {}
void mcla_hook_ws_ui_movie_done_5() {}
void mcla_hook_ws_ui_movie_done_6() {}
void mcla_hook_ws_ui_movie_done_7() {}
void mcla_hook_ws_ui_movie_done_8() {}
void mcla_hook_ws_ui_viewport(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) {}
void mcla_hook_ws_ui_scissor() {}
void mcla_hook_ws_ui_piece(PPCRegister&) {}
void mcla_hook_ws_ui_navsys_viewport() {}

#endif
