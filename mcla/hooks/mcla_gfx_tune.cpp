// mcla_gfx_tune: the game's own quality knobs as live mcla_gfx_* cvars (docs/mcla/NATIVE_RENDERER.md
// section 22, ADDRESSES.md "Quality knobs").
//
// RAGE keeps its distances, counts and rates in .data variables and in live heap objects. A knob here
// is one or a few of those words, written once per engine frame by OnFrame() (called from the
// heartbeat hook, on the game thread), so a cvar change takes effect on the next frame and the
// game never sees a half-written state. When a knob goes back to its default the game's original value
// is written back (the original is read at the first write).
//
// mcla_gfx_poke is the generic form, used to find the knobs: 'ADDR/TYPE/VALUE' entries joined by '+'
// (the characters , ; = are taken by the harness's --set-at and the bench variants):
//     827E0DE0/f/150          one float at 0x827E0DE0
//     8288D2B4:258/u32/3      '8288D2B4:258' = the word at 0x8288D2B4 plus 0x258 (a heap object's
//                             field: ':' derefs, each step = u32 at the address so far + the offset;
//                             write '8288D2B4:0:10' for [[0x8288D2B4]+0] + 0x10)
// TYPE f (float32), u32, u16 (h), u8 (b), and32 / or32 (flag words: every frame word & VALUE / word | VALUE; only the
// forced bits go back); VALUE decimal or 0x hex (floats: decimal). A path through a
// heap object is re-resolved every frame, and restored only while it still resolves to the same
// address (a freed object is never written back).
//
// Named knobs (live; 1.0 / 0 = the game as shipped, every default is a no-op):
//   mcla_gfx_city_lod         x the hi-LOD draw distance 0x827E0DE0 (300 m; the game's own script command
//                              City_SetMaxHilodDrawDistance writes it): fewer city objects, impostors and top-down map draws
//   mcla_gfx_shadow_dist      x the shadow section distance 0x827E0DD4 (200 m): sections farther than this get no cascade /
//                              top-down map draws at all
//   mcla_gfx_dl_clear         draw-list bits removed from every object's mask (mid-asm hook at 0x822E0180), see below
//   mcla_gfx_traffic_lod      x the vehicle LOD thresholds (mid-asm hook at 0x827903E0)
//   mcla_gfx_ambient_density  x pedestrian density and parked-car fraction of NEW ambient zones (hook 0x826F5CA0)
//   mcla_gfx_env_interval     N = draw the environment-map phases (0x4 | 0x8: the cars' paint and speedometer reflection, 512x256 4x
//                              and its mips) only every N-th frame (1 / 0 = every frame, as shipped). The phase loop sub_822E6408
//                              reads [renderer+448] & [renderer+364]; the override in mcla_fsr_markers.cpp clears the bits in
//                              [renderer+448] around the loop on the frames that are left out (PhasesToSkip())
//   mcla_gfx_reflection_interval  the same for the reflection view phase (0x2); at 2 it falls on the frames the env map is left out
//   mcla_gfx_far_cascades_interval  N = 2: the far shadow cascade 2 (phase 0x80; cascade 3 is never skipped) on alternate frames, the skipped
//                              cascade's light matrices held (sub_825F61F0 override) and the shadow atlas in memory of its own
//                              (mcla_gfx_shadow_private; sub_825F4EF8 override): mcla_gfx_far_cascades.cpp
//   mcla_gfx_impostor_interval  N >= 2: a tree-impostor type is regenerated at most every N-th frame (mid-asm hook 0x8230DE50)
//   mcla_gfx_shadow_res       0 = shipped 640; 512, 384, 320, 256 or 192 = the edge of the sun-shadow map in guest texels (mid-asm hook
//                              0x825F69A0 in the CShadowMap constructor, mcla_gfx_shadow_res.cpp). Read when the shadow map is created:
//                              a world load. The 2000p profile (draw resolution scale 2) sets 320: host 640, the 1080p look
//   mcla_gfx_env_res          0 = shipped 512x256; 384 or 256 = the width of the environment map (the cars' paint reflection, dual
//                              paraboloid, height = width / 2; mid-asm hook 0x8233AB3C, mcla_gfx_env_res.cpp). Read once, when the
//                              environment map system is created (a world load)
//   mcla_gfx_impostors        false = no tree impostors (the game's own `noimpostors` switch, plus the guard of the refresh loop
//                              the switch alone would hang in: mcla_gfx_impostors.cpp). Applies to impostors created after the change
//   mcla_gfx_preset           "" = as shipped, perf, perf+, lowest, quality, ultra: bundles of the knobs above and below (quality =
//                              the shipped detail plus FXAA, ultra = quality with 1.5x distances), applied once at first use
//                              as app defaults; a knob set by hand wins (see ApplyPreset)
//   mcla_gfx_tier             -1 = off (the knobs above rule); 0..5 = a run-time detail tier for the frame-rate governor: a row of
//                              the table at ApplyTier sets the defaults (SetDefaultValue) of the live knobs and swap_post_effect,
//                              applied on the next engine frame, never over a knob the player set, never saved to the config; back
//                              at -1 the defaults go back to what they were. Tiers() / CurrentTier() in mcla_gfx_tune.h
// mcla_gfx_dump logs words: 'SPEC/COUNT' joined by '+' (same SPEC), once per change of the string
// (add '+0/0' to dump again); each line is the address, then 8 words as hex with the float reading.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "nfsmw_pch.h"

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_gfx_res_math.h"
#include "mcla_gfx_tune.h"
#include "mcla_vis.h"
#include "mcla_guest.h"
#include "mcla_hooks.h"

#ifndef MCLA_HOOK_GFX_CULL_MASK
#define MCLA_HOOK_GFX_CULL_MASK 0
#endif
#ifndef MCLA_HOOK_GFX_SHADOW_MODE
#define MCLA_HOOK_GFX_SHADOW_MODE 0
#endif
#ifndef MCLA_HOOK_GFX_DL_ENABLE
#define MCLA_HOOK_GFX_DL_ENABLE 0
#endif
#ifndef MCLA_HOOK_GFX_TRAFFIC_LOD
#define MCLA_HOOK_GFX_TRAFFIC_LOD 0
#endif
#ifndef MCLA_HOOK_GFX_AMBIENT_ZONE
#define MCLA_HOOK_GFX_AMBIENT_ZONE 0
#endif
#ifndef MCLA_HOOK_GFX_PHASES
#define MCLA_HOOK_GFX_PHASES 0
#endif
#ifndef MCLA_HOOK_GFX_FAR_CASCADES
#define MCLA_HOOK_GFX_FAR_CASCADES 0
#endif
#ifndef MCLA_HOOK_FSR_MARKERS
#define MCLA_HOOK_FSR_MARKERS 0
#endif

REXCVAR_DEFINE_STRING(mcla_gfx_poke, "none", "MCLA",
                      "MCLA quality knobs, generic: guest memory writes applied every engine frame, "
                      "'ADDR/TYPE/VALUE+ADDR/TYPE/VALUE' (TYPE f, u32, u16, u8; ADDR hex, "
                      "'8288D2B4:258' = [0x8288D2B4]+0x258). The old value is written back when an entry "
                      "goes away. Live. Used to find the knobs (docs/mcla/NATIVE_RENDERER.md section 22)");
REXCVAR_DEFINE_DOUBLE(mcla_gfx_city_lod, 1.0, "MCLA",
                      "MCLA quality knob: scale of the city hi-LOD draw distance (the game's 300 m, 0x827E0DE0). "
                      "Below 1 = fewer far objects, impostors and top-down map draws, less CPU and GPU. Live");
REXCVAR_DEFINE_DOUBLE(mcla_gfx_shadow_dist, 1.0, "MCLA",
                      "MCLA quality knob: scale of the shadow distance (the game's 200 m, 0x827E0DD4): city sections "
                      "farther than this draw no shadow cascades / top-down maps. Live");
REXCVAR_DEFINE_INT32(mcla_gfx_shadow_mode, 0, "MCLA",
                     "MCLA quality knob: 1 = the game's own FAST shadow path (the one it uses at night): no cascaded "
                     "shadow maps (4 passes of 640x640, ~45 % of the draws), only the fast collector. 0 = as shipped. "
                     "Live (mid-asm hook 0x822F4B9C)");
REXCVAR_DEFINE_INT32(mcla_gfx_dl_disable, 0, "MCLA",
                     "MCLA quality knob: draw-type bits removed from the renderer's draw-type mask [renderer+364] "
                     "(the day path ORs the shadow types 0x75E0 back every frame; hook 0x822F4C48 filters that). "
                     "32/64/128/256 = shadow cascade 0..3, 4096/8192/16384 collectors (DECIMAL: the cvar parser reads 0x.. as 0). 0 = as shipped. Live");
REXCVAR_DEFINE_INT32(mcla_gfx_dl_clear, 0, "MCLA",
                     "MCLA quality knob: draw-list bits removed from every object's culling result (mid-asm hook "
                     "0x822E0180). 32/64/128/256 = shadow cascade 0/1/2/3, 16 particle collision map, "
                     "2/4/8 reflection / env maps, 1024 shadow collector fast (DECIMAL: the cvar parser reads 0x.. as 0). 0 = as shipped. Live");
REXCVAR_DEFINE_DOUBLE(mcla_gfx_traffic_lod, 1.0, "MCLA",
                      "MCLA quality knob: scale of the vehicle LOD distances (mid-asm hook 0x827903E0); below 1 = "
                      "lower LODs sooner. Live");
REXCVAR_DEFINE_DOUBLE(mcla_gfx_ambient_density, 1.0, "MCLA",
                      "MCLA quality knob: scale of pedestrian density and the parked-car fraction of ambient zones "
                      "built after the change (hook 0x826F5CA0: at the next load). 1 = as shipped");
REXCVAR_DEFINE_INT32(mcla_gfx_env_interval, 1, "MCLA",
                     "MCLA quality knob: the environment-map phases of the renderer (0x4 | 0x8: the cars' paint and speedometer "
                     "reflection) are drawn only every N-th frame; the frames in between keep the last picture. 1 (or 0) = every "
                     "frame, as shipped. Live (the phase loop override of mcla_fsr_markers.cpp, build flag MCLA_HOOK_GFX_PHASES)")
    .range(0, 8);
REXCVAR_DEFINE_INT32(mcla_gfx_reflection_interval, 1, "MCLA",
                     "MCLA quality knob: the reflection view phase (0x2) is drawn only every N-th frame (at 2, on the frames the "
                     "environment map is not). 1 (or 0) = every frame, as shipped. Live (same override as "
                     "mcla_gfx_env_interval)")
    .range(0, 8);
REXCVAR_DEFINE_INT32(mcla_gfx_shadow_res, 0, "MCLA",
                     "MCLA quality knob: the edge of the sun-shadow map in GUEST texels (the game's 640; the cascades are drawn into one "
                     "square render target and resolved from it): 0 or 640 = as shipped, 512, 384, 320, 256 or 192. At draw "
                     "resolution scale N the host map is N times that (the 2000p profile sets 320: host 640, the 1080p look; a gfx "
                     "preset's value is halved at scale 2). Read when the shadow map is created (a world load; mid-asm hook "
                     "0x825F69A0, mcla_gfx_shadow_res.cpp)")
    .allowed({"0", "192", "256", "320", "384", "512", "640"});
REXCVAR_DEFINE_INT32(mcla_gfx_env_res, 0, "MCLA",
                     "MCLA quality knob: the width in GUEST texels of the environment map (the cars' paint reflection, a dual-paraboloid "
                     "map of two square hemispheres side by side; the game's 512x256, height = width / 2): 0 or 512 = as shipped, 384 or "
                     "256. At draw resolution scale 2, 256 draws the shipped 512x256 on the host. Read once, when the environment map "
                     "system is created (a world load; mid-asm hook 0x8233AB3C, mcla_gfx_env_res.cpp)")
    .allowed({"0", "256", "384", "512"});
REXCVAR_DEFINE_BOOL(mcla_gfx_impostors, true, "MCLA",
                    "MCLA quality knob: false = no tree impostors (the game's own noimpostors switch; with it the game's refresh "
                    "loop would never end, so mcla_gfx_impostors.cpp guards it). Applies to impostors created after the change. "
                    "true = as shipped");
REXCVAR_DEFINE_INT32(mcla_gfx_far_cascades_interval, 1, "MCLA",
                     "MCLA quality knob: the far sun-shadow cascade 2 (phase 0x80) is drawn only every N-th frame (N = 2: odd "
                     "frames), its light matrices held for the receivers and its slice of the shadow atlas kept, so the static far "
                     "shadows do not slide. Cascade 3 is never skipped (zeroing its phase object breaks the next phases: the car "
                     "body vanished). 1 = every frame, as shipped; "
                     "0 = as shipped plus a one-time log of the four cascade light matrices (index order check). "
                     "Live (phase loop override + the sub_825F61F0 setter override, mcla_gfx_far_cascades.cpp, build flag "
                     "MCLA_HOOK_GFX_FAR_CASCADES). The shadow atlas needs memory of its own for this (mcla_gfx_shadow_private)")
    .range(0, 8);
REXCVAR_DEFINE_INT32(mcla_gfx_shadow_private, -1, "MCLA",
                     "MCLA quality knob: the shadow atlas render target gets its own physical memory instead of a slot of the "
                     "shared render-target pool (which the post-processing targets reuse each frame, so a cascade that is not "
                     "redrawn would be reloaded from memory holding post data). -1 = auto (on while "
                     "mcla_gfx_far_cascades_interval > 1), 0 = off, 1 = on. Read when the shadow map is created (a world load; "
                     "override of sub_825F4EF8)")
    .range(-1, 1);
REXCVAR_DEFINE_INT32(mcla_gfx_impostor_interval, 1, "MCLA",
                     "MCLA quality knob: a tree-impostor type is regenerated at most every N-th frame (the game regenerates the "
                     "untuned types, mask 3, every frame; 128x128 view, clear and resolve each). 1 = as shipped, 2 = every 2nd "
                     "frame, 0 = as shipped plus a statistics line every 300 frames. Live (mid-asm hook 0x8230DE50, "
                     "mcla_gfx_impostors.cpp)")
    .range(0, 8);
REXCVAR_DEFINE_STRING(mcla_gfx_preset, "", "MCLA",
                      "MCLA quality preset, a bundle of the mcla_gfx_* knobs, applied once at first use as app "
                      "defaults (a knob set by hand wins): '' = as shipped; perf = env_interval 2, far_cascades_interval 2, "
                      "shadow_dist 0.6, traffic_lod 0.7, city_lod 0.67, shadow_res 512; perf+ (perfplus) = perf + "
                      "dl_disable 256 (no far cascade) + impostors off; lowest = perf + shadow_mode 1 (no sun shadows); "
                      "quality = the shipped scene detail (intervals 1, distances 1.0, shadow_res 640) plus swap_post_effect "
                      "fxaa; ultra = quality + shadow_dist, city_lod and traffic_lod 1.5 (past the console's distances). "
                      "Logs '[mcla-gfx] preset <name>: ...' once");
REXCVAR_DEFINE_INT32(mcla_gfx_tier, -1, "MCLA",
                     "MCLA run-time detail tier, for the frame-rate governor: -1 = off (the mcla_gfx_* knob cvars rule). 0..5 = "
                     "a row of the tier table (higher = more detail) sets the defaults of the live knobs mcla_gfx_env_interval, "
                     "far_cascades_interval, shadow_dist, city_lod, traffic_lod, shadow_mode and swap_post_effect (not "
                     "mcla_gfx_shadow_res, read at world load): 0 = emergency, no sun shadows; 1 = perf; 2 = perf + FXAA; "
                     "3 = shipped distances; 4 = shipped rates (the quality preset's live part); 5 = 1.5x distances (ultra's). "
                     "A knob you set yourself (command line, launcher, config file, settings overlay) keeps your value in every "
                     "tier. Applied on the next engine frame; back at -1 the knobs' defaults go back to what they were. Logs "
                     "'[mcla-gfx] tier N: ...' once per change. Never saved to the config file (transient). Live (mcla_gfx_tune.cpp, "
                     "ApplyTier)")
    .range(-1, 5)
    .transient();
REXCVAR_DEFINE_STRING(mcla_gfx_dump, "none", "MCLA",
                      "MCLA debug: log guest words once per change of this string, 'ADDR/COUNT+...' "
                      "(ADDR as in mcla_gfx_poke). Live");

namespace mcla::gfxtune {
namespace {

struct Entry {
  std::string key;   // identity for restore
  std::string spec;  // 'HEX(:HEX)*'
  uint32_t address;  // resolved
  uint8_t bytes[4];  // big-endian value
  uint8_t size;      // 1, 2 or 4
  bool indirect;     // resolved through a heap pointer
  bool scale_mode;   // float: written value = the game's original x `scale`
  double scale;
  int op;            // 0 = store, 1 = and32 (word & value every frame), 2 = or32 (word | value)
  uint32_t mask;
};

struct Saved {
  int op;
  uint32_t mask;
  std::string spec;
  uint32_t address;
  uint8_t bytes[4];
  uint8_t size;
  bool indirect;
};

// The values in force this frame (the cvars).
struct Effective {
  double city_lod = 1.0, shadow_dist = 1.0, traffic_lod = 1.0;
  int shadow_mode = 0;
  uint32_t dl_disable = 0;
};
Effective g_eff;

std::mutex g_mutex;
std::atomic<uint32_t> g_dl_clear{0};
std::atomic<uint32_t> g_dl_disable{0};
std::atomic<int> g_shadow_mode{0};
std::atomic<float> g_traffic_lod{1.0f};
std::atomic<float> g_ambient_density{1.0f};
std::map<std::string, Saved> g_saved;
std::string g_last_dump = "none";
std::string g_last_poke = "none";

// 'HEX(:HEX)*' -> address. False when a step cannot be read.
bool ResolveSpec(const std::string& spec, uint32_t* out, bool* indirect) {
  uint32_t cur = 0;
  bool first = true;
  *indirect = false;
  size_t pos = 0;
  while (pos <= spec.size()) {
    size_t end = spec.find(':', pos);
    if (end == std::string::npos) {
      end = spec.size();
    }
    const std::string part = spec.substr(pos, end - pos);
    char* tail = nullptr;
    const uint32_t value = uint32_t(std::strtoul(part.c_str(), &tail, 16));
    if (part.empty() || (tail && *tail)) {
      return false;
    }
    if (first) {
      cur = value;
      first = false;
    } else {
      uint32_t deref = 0;
      if (!mcla::guest::U32(cur, &deref) || deref < 0x10000) {
        return false;
      }
      cur = deref + value;
      *indirect = true;
    }
    pos = end + 1;
    if (end == spec.size()) {
      break;
    }
  }
  *out = cur;
  return !first;
}

bool ParseValue(const std::string& type, const std::string& text, Entry* e) {
  char* tail = nullptr;
  if (type == "f") {
    const float v = std::strtof(text.c_str(), &tail);
    if (text.empty() || (tail && *tail)) {
      return false;
    }
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    bits = __builtin_bswap32(bits);
    std::memcpy(e->bytes, &bits, 4);
    e->size = 4;
    return true;
  }
  const unsigned long long v = std::strtoull(text.c_str(), &tail, 0);
  if (text.empty() || (tail && *tail)) {
    return false;
  }
  if (type == "and32" || type == "or32") {
    e->op = type == "and32" ? 1 : 2;
    e->mask = uint32_t(v);
    e->size = 4;
    return true;
  }
  if (type == "u32" || type == "w") {
    const uint32_t b = __builtin_bswap32(uint32_t(v));
    std::memcpy(e->bytes, &b, 4);
    e->size = 4;
  } else if (type == "u16" || type == "h") {
    const uint16_t b = __builtin_bswap16(uint16_t(v));
    std::memcpy(e->bytes, &b, 2);
    e->size = 2;
  } else if (type == "u8" || type == "b") {
    e->bytes[0] = uint8_t(v);
    e->size = 1;
  } else {
    return false;
  }
  return true;
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t end = s.find(sep, pos);
    if (end == std::string::npos) {
      end = s.size();
    }
    out.push_back(s.substr(pos, end - pos));
    pos = end + 1;
  }
  return out;
}

void GatherPokes(std::map<std::string, Entry>* want) {
  const std::string text = REXCVAR_GET(mcla_gfx_poke);
  if (text.empty() || text == "none") {
    return;
  }
  for (const std::string& item : Split(text, '+')) {
    const std::vector<std::string> f = Split(item, '/');
    if (f.size() != 3) {
      continue;
    }
    Entry e{};
    e.key = "poke:" + f[0] + "/" + f[1];
    e.spec = f[0];
    if (!ParseValue(f[1], f[2], &e)) {
      continue;
    }
    if (!ResolveSpec(f[0], &e.address, &e.indirect)) {
      continue;
    }
    (*want)[e.key] = e;
  }
}

void AddScale(std::map<std::string, Entry>* want, const char* name, uint32_t address, double scale) {
  if (!(std::fabs(scale - 1.0) > 1e-6) || !(scale > 0.0)) {
    return;
  }
  Entry e{};
  e.key = std::string("knob:") + name;
  e.address = address;
  e.size = 4;
  e.scale_mode = true;
  e.scale = scale;
  (*want)[e.key] = e;
}

void GatherKnobs(std::map<std::string, Entry>* want) {
  // The renderer's draw-type enable mask (+364 of the object at 0x8287E064): clear the cvar's bits now; the hook at
  // 0x822F4C48 keeps the game from OR-ing them back.
  const uint32_t disable = g_eff.dl_disable;
  if (disable != 0) {
    Entry e{};
    e.key = "knob:dl_disable";
    e.spec = "8287E064:16C";
    e.op = 1;
    e.mask = ~disable;
    e.size = 4;
    if (ResolveSpec(e.spec, &e.address, &e.indirect)) {
      (*want)[e.key] = e;
    }
  }
  AddScale(want, "city_lod", 0x827E0DE0, g_eff.city_lod);
  AddScale(want, "shadow_dist", 0x827E0DD4, g_eff.shadow_dist);
}

void Apply(const std::map<std::string, Entry>& want) {
  // Restore what is no longer wanted.
  for (auto it = g_saved.begin(); it != g_saved.end();) {
    if (want.count(it->first)) {
      ++it;
      continue;
    }
    const Saved& s = it->second;
    bool same = true;
    if (s.indirect) {
      // A heap object: write back only while the path still leads to the address we wrote.
      uint32_t now = 0;
      bool ind = false;
      same = ResolveSpec(s.spec, &now, &ind) && now == s.address;
    }
    if (same && s.op != 0) {
      // A flag word the game also writes: put back only the bits we forced.
      uint32_t cur = 0, orig = (uint32_t(s.bytes[0]) << 24) | (uint32_t(s.bytes[1]) << 16) |
                               (uint32_t(s.bytes[2]) << 8) | s.bytes[3];
      if (mcla::guest::U32(s.address, &cur)) {
        cur = s.op == 1 ? (cur | (orig & ~s.mask)) : ((cur & ~s.mask) | (orig & s.mask));
        const uint32_t be = __builtin_bswap32(cur);
        mcla::guest::WriteBytes(s.address, &be, 4);
      }
      REXLOG_INFO("[mcla-gfx] {}: bits back to the game's value at {:08X}", it->first, s.address);
    } else if (same) {
      mcla::guest::WriteBytes(s.address, s.bytes, s.size);
      REXLOG_INFO("[mcla-gfx] {}: back to the game's value at {:08X}", it->first, s.address);
    } else {
      REXLOG_INFO("[mcla-gfx] {}: removed (heap object, not written back)", it->first);
    }
    it = g_saved.erase(it);
  }
  for (const auto& kv : want) {
    Entry e = kv.second;
    bool first_write = false;
    auto it = g_saved.find(kv.first);
    if (it != g_saved.end() && it->second.address != e.address) {
      g_saved.erase(it);  // the object moved: the old original is gone with it
      it = g_saved.end();
    }
    if (it == g_saved.end()) {
      Saved s{};
      s.op = e.op;
      s.mask = e.mask;
      s.spec = e.spec;
      s.address = e.address;
      s.size = e.size;
      s.indirect = e.indirect;
      if (!mcla::guest::Read(e.address, s.bytes, e.size)) {
        continue;
      }
      g_saved[kv.first] = s;
      first_write = true;
    }
    if (e.op != 0) {
      uint32_t cur = 0;
      if (!mcla::guest::U32(e.address, &cur)) {
        continue;
      }
      cur = e.op == 1 ? (cur & e.mask) : (cur | e.mask);
      const uint32_t be = __builtin_bswap32(cur);
      std::memcpy(e.bytes, &be, 4);
    } else if (e.scale_mode) {
      // The game's own float (read at the first write) x scale.
      const auto sv = g_saved.find(kv.first);
      if (sv == g_saved.end()) {
        continue;
      }
      uint32_t bits = (uint32_t(sv->second.bytes[0]) << 24) | (uint32_t(sv->second.bytes[1]) << 16) |
                      (uint32_t(sv->second.bytes[2]) << 8) | sv->second.bytes[3];
      float orig;
      std::memcpy(&orig, &bits, 4);
      const float value = float(double(orig) * e.scale);
      std::memcpy(&bits, &value, 4);
      bits = __builtin_bswap32(bits);
      std::memcpy(e.bytes, &bits, 4);
    }
    if (first_write) {
      const Saved& sv = g_saved[kv.first];
      REXLOG_INFO("[mcla-gfx] {}: {:08X} <- {:02X}{:02X}{:02X}{:02X} (was {:02X}{:02X}{:02X}{:02X})", kv.first, e.address,
                  e.bytes[0], e.size > 1 ? e.bytes[1] : 0, e.size > 2 ? e.bytes[2] : 0, e.size > 3 ? e.bytes[3] : 0,
                  sv.bytes[0], sv.size > 1 ? sv.bytes[1] : 0, sv.size > 2 ? sv.bytes[2] : 0, sv.size > 3 ? sv.bytes[3] : 0);
    }
    mcla::guest::WriteBytes(e.address, e.bytes, e.size);
  }
}

void Dump() {
  const std::string text = REXCVAR_GET(mcla_gfx_dump);
  if (text == g_last_dump) {
    return;
  }
  g_last_dump = text;
  if (text.empty() || text == "none") {
    return;
  }
  for (const std::string& item : Split(text, '+')) {
    const std::vector<std::string> f = Split(item, '/');
    if (f.size() != 2) {
      continue;
    }
    uint32_t address = 0;
    bool indirect = false;
    const long count = std::strtol(f[1].c_str(), nullptr, 0);
    if (count <= 0) {
      continue;
    }
    if (!ResolveSpec(f[0], &address, &indirect)) {
      REXLOG_INFO("[mcla-gfx] dump {}: not resolvable", f[0]);
      continue;
    }
    REXLOG_INFO("[mcla-gfx] dump {} = {:08X}, {} words", f[0], address, count);
    for (long i = 0; i < count; i += 8) {
      char line[256];
      int n = std::snprintf(line, sizeof(line), "%08X:", uint32_t(address + 4 * i));
      for (long j = i; j < i + 8 && j < count; ++j) {
        uint32_t w = 0;
        if (!mcla::guest::U32(address + 4 * uint32_t(j), &w)) {
          n += std::snprintf(line + n, sizeof(line) - size_t(n), " --------");
          continue;
        }
        float fv;
        std::memcpy(&fv, &w, 4);
        if (w != 0 && std::isfinite(fv) && std::fabs(fv) > 1e-4f && std::fabs(fv) < 1e7f) {
          n += std::snprintf(line + n, sizeof(line) - size_t(n), " %08X(%g)", w, double(fv));
        } else {
          n += std::snprintf(line + n, sizeof(line) - size_t(n), " %08X", w);
        }
      }
      REXLOG_INFO("[mcla-gfx] {}", line);
    }
  }
}

// ---- the environment-map and reflection phases at a lower rate (PhasesToSkip) -------------------------------------
//
// The phase loop sub_822E6408 (one call per frame, from sub_822C0980 at 0x822C0B08, on the render thread) walks r30 = 1, 2, 4
// ... 0x20000000 and runs a phase only when its bit is set in r21 = [renderer+364] & [renderer+448]: `lwz r11,448(r27)` at
// 0x822E6608, `lwz r17,364(r27)` at 0x822E6624, `and r21,r17,r11` at 0x822E6630, r21 stored to [renderer+360] at 0x822E66D8, and
// the 13 draw-list objects at renderer+128.. are called only for a phase in r21 (`and r10,r30,r21; beq` at 0x822E6844). That is
// NOT the whole phase: the per-phase object renderer[k] (k = the bit's index; sub_822E50D8 registers them from 0x823048D0) gets
// its virtual +4 (begin: render target, clear) and +8 (end: resolve) called in every iteration whatever r21 says, so clearing
// the mask bit alone leaves a phase that clears its target and resolves it EMPTY over the texture it should keep (the env map,
// the shadow atlas quarter: the flicker of the first version). mcla_fsr_markers.cpp therefore also zeroes renderer[k] of a
// skipped phase around the call. A phase left out that way costs nothing and keeps the textures it resolved the last time it ran.
//   +364 is the draw-type enable mask the game and mcla_gfx_dl_disable change every frame; +448 is not: it is -1 from the
//   renderer constructor (0x822E4838, sub_822E47E0), cut by dev options in the constructor, and otherwise written only by
//   sub_822E51F8 (no direct caller). It is read by nothing but the phase loop (scan of the generated code: the only function that
//   loads the renderer global 0x8287E064 and reads +448). So +448 is the word to clear, on the render thread, around the call.
//
// The bits. Phase k is the k-th entry of the draw-type name table at 0x827E19E4 (dtRENDERTARGETS, dtREFLECTIONMAP, dtENVMAP0,
// dtENVMAP1, dtPARTICLECOLLISIONMAP, dtSHADOWMAP0..3, dtCULLING, dtSHADOWCOLLECTORFAST, dtSHADOWPREPASS ... dtUI; the table's own
// second column holds 1 << (k + 1), one bit up, so the bit is the entry's index, not that column): 0x1 render targets,
// 0x2 reflection map, 0x4 env map 0, 0x8 env map 1, 0x10 particle collision map, 0x20 / 0x40 / 0x80 / 0x100 shadow cascades 0..3,
// 0x200 culling, 0x400 fast shadow collector, 0x800 shadow prepass = the start of the main view (the FSR scene marker). Checked
// against code: the shadow phase handler sub_823120C8 (r18 = the phase bit) does `addi r11,r18,-32; cmplwi r11,224` + a jump
// table that gives 0x20 / 0x40 / 0x80 / 0x100 the cascade index 0..3, `cmpwi r18,1024` gives 0x400 index 4, and 0x1000 / 0x2000 /
// 0x4000 / 0x800 get 5 / 6 / 7 / 8 (the table at 0x8231223C..0x82312260, the cases 0x823125E4..0x82312640); the fast shadow path of sub_822F3BD8 removes 0x71E0 (the four cascades
// + collectors 0x1000 / 0x2000 / 0x4000) from +364. So entry 5 = 0x20 and the entries before it (render targets, reflection, env 0,
// env 1, particle collision) are 0x1, 0x2, 0x4, 0x8, 0x10. The same bits as RenanQueiroz/mcla-recomp's frame-structure notes
// (cascades 0x20..0x100, env map 0x4 | 0x8, reflection 0x2) and the iOS port's mcla_gfx_dl_clear (2 / 4 / 8).
constexpr uint32_t kPhaseReflection = 0x2;
constexpr uint32_t kPhaseEnvMaps = 0x4 | 0x8;
constexpr uint32_t kMaxInterval = 8;

// The far shadow cascades: 0x80 = cascade 2 and 0x100 = cascade 3 (the phases the shadow handler sub_823120C8 gives the index
// 2 and 3). Skipping one leaves its draws, its clear and its resolve into its quarter of the shadow atlas out of the frame;
// its light matrices ([[CShadowMap+0x18]+4i]+0x10, uploaded for the shadow collector by sub_825F61F0) are held by the setter
// override of mcla_gfx_far_cascades.cpp, which reads FarCascadePhasesSkipped(). Cascades 0 and 1 are never skipped.
constexpr uint32_t kPhaseFarCascade2 = 0x80;
constexpr uint32_t kPhaseFarCascade3 = 0x100;

struct FarState {
  bool was_active = false;  // the phase was in the active mask at the previous phase loop call (render thread only)
};

std::atomic<uint32_t> g_env_interval{1};
std::atomic<uint32_t> g_reflection_interval{1};
std::atomic<uint32_t> g_far_interval{1};
std::atomic<uint32_t> g_far_skipped{0};
std::atomic<uint32_t> g_phase_frame{0};
FarState g_far[2];  // cascade 2, cascade 3; touched by the render thread only

uint32_t ClampInterval(int32_t value) {
  return value <= 1 ? 1u : (uint32_t(value) > kMaxInterval ? kMaxInterval : uint32_t(value));
}

// Game thread, from OnFrame: the cvars into the atomics the render thread reads, with a log line per change.
void PublishPhaseIntervals() {
  const uint32_t env = ClampInterval(REXCVAR_GET(mcla_gfx_env_interval));
  const uint32_t reflection = ClampInterval(REXCVAR_GET(mcla_gfx_reflection_interval));
  const uint32_t far_interval = MCLA_HOOK_GFX_FAR_CASCADES ? ClampInterval(REXCVAR_GET(mcla_gfx_far_cascades_interval)) : 1u;
  if (env == g_env_interval.load(std::memory_order_relaxed) &&
      reflection == g_reflection_interval.load(std::memory_order_relaxed) &&
      far_interval == g_far_interval.load(std::memory_order_relaxed)) {
    return;
  }
  g_env_interval.store(env, std::memory_order_relaxed);
  g_reflection_interval.store(reflection, std::memory_order_relaxed);
  g_far_interval.store(far_interval, std::memory_order_relaxed);
  REXLOG_INFO(
      "[mcla-gfx] phases: environment map (0x{:X}) drawn every {} frame(s), reflection view (0x{:X}) every {} frame(s), "
      "far shadow cascade 2 (0x{:X}, cascade 3 / 0x{:X} is never left out) every {} frame(s), on frames = 1 mod N, "
      "light matrices held (1 = as shipped; the skipped frames keep the last picture)",
      kPhaseEnvMaps, env, kPhaseReflection, reflection, kPhaseFarCascade2, kPhaseFarCascade3, far_interval);
}

// ---- mcla_gfx_preset ----------------------------------------------------------------------------------------------
//
// A bundle of knobs applied with rex::cvar::SetDefaultValue (the way the profile's tables are, mcla/profile/pc_profile.cpp):
// the value becomes the cvar's own default while nothing else sets it, so a knob set by hand (command line, config file,
// environment, console) keeps its value, and nothing is written to the config file. Applied once, at first use: OnFrame, a
// hook registration's announce line, or a hook that reads a knob at its own time (shadow map size, impostors).
struct PresetKnob {
  const char* cvar;
  const char* value;
};

// perf: near-identical picture: the car reflections and shadow cascade 2 at half rate, shorter shadow and LOD distances and
// a smaller shadow map.
//   - The two half-rate knobs flickered once (owner report: the skipped phase still cleared and resolved its empty target;
//     fixed in mcla_fsr_markers.cpp, the phase's own object is taken out too). Checked since: parked, 24 captures in 1 s, 0.3 %
//     of the pixels with a temporal std > 6 with and without them; driving, no frame-to-frame alternation of the car paint
//     (scratch/ORC/flicker/drv, drv_t). They are worth about 5 points of GPU load at 1080p on the 880M (orc-ab-perf 81.5 %,
//     orc-ab-perf_t 76.5 %), which is what keeps the heaviest streets at 60: without them one 40 s stretch of the bench drive
//     alternated between 16.7 and 33 ms (V5m6fs: 3.6 % of the frames over 25 ms).
//   - mcla_gfx_impostor_interval=2 held nothing back (1800 requests per 300 frames, all let through): a no-op, not here.
constexpr PresetKnob kPresetPerf[] = {
    {"mcla_gfx_env_interval", "2"},           // car paint / speedometer reflection at 30 Hz (the console's own rate)
    {"mcla_gfx_far_cascades_interval", "2"},  // cascade 2 on alternate frames, matrices held; cascade 3 every frame
    {"mcla_gfx_shadow_dist", "0.6"},              // sections past 120 m cast no cascade shadows
    {"mcla_gfx_traffic_lod", "0.7"},
    {"mcla_gfx_city_lod", "0.67"},
    {"mcla_gfx_shadow_res", "512"},  // 640 -> 512: ~36 % less cascade fill; read when the shadow map is created
};
// perf+: perf and no far shadow cascade (stale far shadows in motion), no tree impostors.
constexpr PresetKnob kPresetPerfPlus[] = {
    {"mcla_gfx_dl_disable", "256"},
    {"mcla_gfx_impostors", "false"},
};
// lowest: perf and no sun shadows (the game's own night path).
constexpr PresetKnob kPresetLowest[] = {
    {"mcla_gfx_shadow_mode", "1"},
};
// quality: the scene detail as shipped plus FXAA. NOT built on perf: every knob perf touches is set explicitly (a perf default set
// earlier in the same process is undone). swap_post_effect is an SDK cvar; since the SDK patch 0600 the command processor reads
// it on every swap, so a default set here after graphics started still takes effect.
// Measured with the in-run A/B (mcla_vis_ab; run vis3-parked-ab, 1080p, Radeon 880M, the parked scene, GPU busy at 60 fps, noise
// about +-0.3 ms), GPU time per frame:
//   - FXAA: about +0.3 ms (0.15 to 0.6).
//   - the shipped car-reflection and far-cascade rates (env_interval 1, far_cascades_interval 1) over perf's 2 and 2: about +0.8 ms.
//   - the shipped distances (shadow_dist, traffic_lod, city_lod 1.0) over perf's 0.6 / 0.7 / 0.67: about +0.3 to 0.6 ms.
//   - ultra's 1.5x distances over the shipped ones: about +0.3 ms parked and about +1 ms while driving (vis4-drive-tiers, tier 5 against its
//     neighbours), with no visible change at the parked spot; what they buy is
//     objects and shadows appearing farther out while driving (pop-in), which is not measured.
// The same steps, one at a time, are the run-time tiers 2 to 5 (mcla_gfx_tier, ApplyTier).
constexpr PresetKnob kPresetQuality[] = {
    {"mcla_gfx_env_interval", "1"},
    {"mcla_gfx_far_cascades_interval", "1"},
    {"mcla_gfx_shadow_dist", "1.0"},
    {"mcla_gfx_traffic_lod", "1.0"},
    {"mcla_gfx_city_lod", "1.0"},
    {"mcla_gfx_shadow_res", "0"},  // 0 = as shipped, 640
    {"swap_post_effect", "fxaa"},
};
// ultra: quality, with these three replacing quality's values (past the console's distances).
constexpr PresetKnob kPresetUltra[] = {
    {"mcla_gfx_shadow_dist", "1.5"},
    {"mcla_gfx_city_lod", "1.5"},
    {"mcla_gfx_traffic_lod", "1.5"},
};

std::string Lower(const std::string& text) {
  size_t b = 0, e = text.size();
  while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
  std::string out = text.substr(b, e - b);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// "0.600000" -> "0.6", "2" -> "2".
std::string Pretty(std::string value) {
  if (value.find('.') != std::string::npos) {
    while (!value.empty() && value.back() == '0') value.pop_back();
    if (!value.empty() && value.back() == '.') value.pop_back();
  }
  return value;
}

// "mcla_gfx_city_lod" -> "city_lod"; a cvar without the prefix (swap_post_effect) as it is.
const char* ShortName(const char* cvar) {
  constexpr const char kPrefix[] = "mcla_gfx_";
  return std::strncmp(cvar, kPrefix, sizeof(kPrefix) - 1) == 0 ? cvar + (sizeof(kPrefix) - 1) : cvar;
}

void ApplyPreset() {
  const std::string requested = REXCVAR_GET(mcla_gfx_preset);
  std::string name = Lower(requested);
  if (name.empty() || name == "default" || name == "none" || name == "shipped") {
    return;
  }
  if (name == "perfplus" || name == "perf_plus" || name == "perf-plus") {
    name = "perf+";  // '+' in a value is awkward in some shells and bench variants
  }
  std::vector<PresetKnob> knobs;
  if (name == "quality" || name == "ultra") {
    // Its own list, not perf's (see kPresetQuality); ultra = quality with the three distances replaced.
    knobs.assign(std::begin(kPresetQuality), std::end(kPresetQuality));
    if (name == "ultra") {
      for (const PresetKnob& over : kPresetUltra) {
        for (PresetKnob& knob : knobs) {
          if (std::strcmp(knob.cvar, over.cvar) == 0) {
            knob.value = over.value;
          }
        }
      }
    }
  } else if (name == "perf" || name == "perf+" || name == "lowest") {
    knobs.assign(std::begin(kPresetPerf), std::end(kPresetPerf));
    if (name == "perf+") {
      knobs.insert(knobs.end(), std::begin(kPresetPerfPlus), std::end(kPresetPerfPlus));
    } else if (name == "lowest") {
      knobs.insert(knobs.end(), std::begin(kPresetLowest), std::end(kPresetLowest));
    }
  } else {
    REXLOG_WARN("[mcla-gfx] preset '{}': unknown (perf, perf+, lowest, quality, ultra, or empty for as shipped): nothing applied",
                requested);
    return;
  }
  // The draw resolution scale of the run (the 2000p profile draws every render target at 2x2): the presets name the HOST look of the
  // sun-shadow map (perf 512, quality 640), so at scale 2 the guest edge is half of it and the host map is the one of the same
  // preset at scale 1 (mcla_gfx_res_math.h ShadowEdgeAtScale). A cvar the GPU plugin does not define reads 0 = no scale.
  const int32_t scale_x = rex::cvar::Query<int32_t>("draw_resolution_scale_x");
  const int32_t scale_y = rex::cvar::Query<int32_t>("draw_resolution_scale_y");
  std::string line;
  for (const PresetKnob& knob : knobs) {
    // Set by hand (anything but the compiled-in default, an app default of the profile included) = the user's value stays.
    const bool by_hand = rex::cvar::GetFlagSource(knob.cvar) != rex::cvar::Source::kDefault;
    std::string value = knob.value;
    bool at_scale = false;
    if (std::strcmp(knob.cvar, "mcla_gfx_shadow_res") == 0) {
      const int32_t preset_value = int32_t(std::strtol(knob.value, nullptr, 10));
      const int32_t guest = mcla::gfxres::ShadowEdgeAtScale(preset_value, scale_x, scale_y);
      if (guest != preset_value) {
        value = std::to_string(guest);
        at_scale = true;
      }
    }
    const bool ok = rex::cvar::SetDefaultValue(knob.cvar, value);
    line += ' ';
    line += ShortName(knob.cvar);
    line += '=';
    line += Pretty(rex::cvar::GetFlagByName(knob.cvar));
    if (at_scale && !by_hand) {
      line += fmt::format("(draw scale {}x{}: the preset's {} halved)", scale_x, scale_y, knob.value);
    }
    if (by_hand) {
      line += "(yours)";
    }
    if (!ok) {
      line += "(REJECTED)";
    }
  }
  REXLOG_INFO("[mcla-gfx] preset {}:{}", name, line);
}

// ---- mcla_gfx_tier ------------------------------------------------------------------------------------------------
//
// A run-time detail tier for the frame-rate governor (LOCK60; the contract is in mcla_gfx_tune.h). Like a preset a tier is written
// as the knobs' APP DEFAULTS (rex::cvar::SetDefaultValue), but every time mcla_gfx_tier changes, and it is taken back when it goes
// to -1. That keeps each cvar's source at kDefault, so a config save never writes a tier, and it lets the player's own choice win:
// a knob whose source is not kDefault (command line, launcher, config file, settings overlay, console) is skipped, whatever the
// tier says ('(yours)' in the log line, as the preset's). The knobs are the ordinary cvars, so PublishPhaseIntervals, GatherKnobs and
// the hooks pick the change up unchanged, in the same OnFrame (ApplyTier runs before the knobs are read). mcla_gfx_shadow_res is
// not in the table: it is read when the shadow map is created (a world load). Higher = more detail; the governor sheds by stepping
// down.
// The columns are kTierCvars; the rows:
//   0 emergency: no sun shadows (the game's own night path), perf's rates and distances, no AA
//   1 perf (the live part of the perf preset), no AA
//   2 perf + FXAA
//   3 shipped distances (shadow_dist, city_lod, traffic_lod 1.0), perf's rates, FXAA
//   4 shipped rates (env_interval, far_cascades_interval 1) = the live part of the quality preset
//   5 distances at 1.5x = the live part of the ultra preset
constexpr int kTierKnobCount = 7;
constexpr const char* kTierCvars[kTierKnobCount] = {
    "mcla_gfx_env_interval", "mcla_gfx_far_cascades_interval", "mcla_gfx_shadow_dist", "mcla_gfx_city_lod",
    "mcla_gfx_traffic_lod",  "mcla_gfx_shadow_mode",           "swap_post_effect",
};
constexpr const char* kTierValues[kTierMax + 1][kTierKnobCount] = {
    {"2", "2", "0.6", "0.67", "0.7", "1", "none"},
    {"2", "2", "0.6", "0.67", "0.7", "0", "none"},
    {"2", "2", "0.6", "0.67", "0.7", "0", "fxaa"},
    {"2", "2", "1.0", "1.0", "1.0", "0", "fxaa"},
    {"1", "1", "1.0", "1.0", "1.0", "0", "fxaa"},
    {"1", "1", "1.5", "1.5", "1.5", "0", "fxaa"},
};

struct KnobBefore {
  bool captured = false;  // the tier has written this knob's default since the last tier -1
  std::string value;      // the knob's default (and value, source being kDefault) as it stood just before that first write: the
                          // preset's app default or the compiled one
};

int g_tier_last = -1;                // game thread only (OnFrame): the tier written last, -1 = off
std::atomic<int> g_tier_applied{-1};  // the same, for CurrentTier() from any thread
KnobBefore g_tier_before[kTierKnobCount];  // per knob, game thread only

// Game thread, from OnFrame, before the knobs are read into g_eff and the phase atomics.
void ApplyTier() {
  const int tier = std::clamp<int>(REXCVAR_GET(mcla_gfx_tier), -1, kTierMax);
  if (tier == g_tier_last) {
    return;
  }
  std::string line;
  if (tier >= 0) {
    for (int i = 0; i < kTierKnobCount; ++i) {
      const char* cvar = kTierCvars[i];
      line += ' ';
      line += ShortName(cvar);
      line += '=';
      // Set by the player (anything but the default source: command line, launcher, config file, settings overlay, console): the
      // tier leaves it alone. Checked at every tier change, so a knob the player sets while a tier is on is skipped from then on.
      if (rex::cvar::GetFlagSource(cvar) != rex::cvar::Source::kDefault) {
        line += Pretty(rex::cvar::GetFlagByName(cvar));
        line += "(yours)";
        continue;
      }
      KnobBefore& before = g_tier_before[i];
      if (!before.captured) {
        // The first write since the tier was off: the source is kDefault, so the value is the knob's default (a preset's app
        // default included). That is what tier -1 puts back.
        before.value = rex::cvar::GetFlagByName(cvar);
        before.captured = true;
      }
      const bool ok = rex::cvar::SetDefaultValue(cvar, kTierValues[tier][i]);
      line += Pretty(rex::cvar::GetFlagByName(cvar));
      if (!ok) {
        line += "(REJECTED)";
      }
    }
    REXLOG_INFO("[mcla-gfx] tier {}:{}", tier, line);
  } else {
    for (int i = 0; i < kTierKnobCount; ++i) {
      KnobBefore& before = g_tier_before[i];
      if (!before.captured) {
        continue;  // never written (the player's value all along)
      }
      // SetDefaultValue on a cvar whose source is kDefault moves its value with the default; on one the player has set since it only
      // moves the default, the player's value stays.
      rex::cvar::SetDefaultValue(kTierCvars[i], before.value);
      before.captured = false;
    }
    REXLOG_INFO("[mcla-gfx] tier off: knob defaults back to what they were (a knob you set keeps your value)");
  }
  g_tier_last = tier;
  g_tier_applied.store(tier, std::memory_order_relaxed);
}

}  // namespace

void EnsurePreset() {
  static std::once_flag once;
  std::call_once(once, ApplyPreset);
}

const TierInfo* Tiers(int* count) {
  static const TierInfo kTiers[] = {
      {0, "emergency: no sun shadows", -3.0f, false,
       "estimate (S3, vs tier 1), not measured on PC; every sun shadow vanishes at once"},
      {1, "perf", 0.0f, true, "baseline: the live part of the perf preset, no AA"},
      {2, "perf+fxaa", 0.3f, true, "FXAA on; the edges change subtly"},
      {3, "shipped distances", 0.4f, false,
       "objects and shadows at the LOD and shadow distance boundary appear at the switch: a small pop, best switched while moving "
       "fast or behind a camera cut"},
      {4, "shipped rates (quality)", 0.8f, true,
       "car reflections and far shadow cascade at full rate; the phase skip forces a full draw on the first frame (LEARNINGS: no "
       "flicker since the fix)"},
      {5, "ultra distances", 1.0f, false,
       "distances 1.5x: farther objects and shadows appear at the switch; +0.3 ms parked, about +1 ms while driving (vis4-drive-tiers)"},
  };
  if (count) {
    *count = int(sizeof(kTiers) / sizeof(kTiers[0]));
  }
  return kTiers;
}

int CurrentTier() { return g_tier_applied.load(std::memory_order_relaxed); }

uint32_t PhasesToSkip(uint32_t active_phases) {
  const uint32_t env = g_env_interval.load(std::memory_order_relaxed);
  const uint32_t reflection = g_reflection_interval.load(std::memory_order_relaxed);
  const uint32_t far_interval = g_far_interval.load(std::memory_order_relaxed);
  if (env <= 1 && reflection <= 1 && far_interval <= 1) {
    // As shipped: nothing written, nothing counted (the far-cascade state is only dropped when it was in use).
    if (g_far_skipped.load(std::memory_order_relaxed) != 0 || g_far[0].was_active || g_far[1].was_active) {
      g_far_skipped.store(0, std::memory_order_relaxed);
      g_far[0].was_active = g_far[1].was_active = false;
    }
    return 0;
  }
  // The frames are the phase loop's own calls. The first `interval` frames draw everything (the textures exist before they
  // are reused); after that the env map is drawn on frames = 0 mod N and the reflection view on frames = 1 mod N, so at N = 2
  // the two take turns and the frame cost stays even.
  const uint32_t frame = g_phase_frame.fetch_add(1, std::memory_order_relaxed);
  uint32_t skip = 0;
  if (env > 1 && frame >= env && frame % env != 0) {
    skip |= kPhaseEnvMaps;
  }
  if (reflection > 1 && frame >= reflection && frame % reflection != 1) {
    skip |= kPhaseReflection;
  }
#if MCLA_HOOK_GFX_FAR_CASCADES
  // The far shadow cascade 2 (see the block above kPhaseFarCascade2) is drawn on frames = 1 mod N, so at N = 2 on every second
  // frame. A cascade is always drawn on the first frame it is active after being inactive (start of the world, day after night,
  // the preset switched on): its matrices and its slice of the atlas do not exist before that.
  // CASCADE 3 IS NEVER SKIPPED (kFarCascadesSkipped = 1): zeroing its per-phase object (the shadow system, registered for the
  // phases of the mask 0x7DE0) also drops its virtual +8, the end of the last cascade, which sub_823115E8 uses to set up the
  // phases after it (the `cmpwi r30,3` branch: sub_825F5850 at 0x82311A9C, sub_82177B40 at 0x82311AB0, then the setter 0x82311AF4): with it left out on alternate
  // frames the player's car body was not drawn at all on those frames (scratch/ORC flicker run orc-flk-bothfix, b072_b.png).
  // Cascade 2 has no such special case. Verified in the parked scene (24-frame bursts, std of luma over the burst, b072): at 60 fps
  // perf-without-intervals 0.15 / 0.3 % of pixels over 6 / consecutive diff 0.07, with env 2 + cascade 2 on alternate frames
  // 0.11 / 0.3 % / 0.04 (runs orc-flk-base60, orc-flk-both60); before the fix env 2 alone was 0.31 / 1.0 % / 0.28.
  constexpr int kFarCascadesSkipped = 1;
  uint32_t far_skip = 0;
  for (int k = 0; k < kFarCascadesSkipped; ++k) {
    FarState& state = g_far[k];
    const uint32_t bit = k == 0 ? kPhaseFarCascade2 : kPhaseFarCascade3;
    if (far_interval <= 1 || !(active_phases & bit)) {
      state.was_active = false;
      continue;
    }
    const bool forced = !state.was_active;
    state.was_active = true;
    if (!forced && frame % far_interval != (k == 0 ? 1u : 0u) % far_interval) {
      far_skip |= bit;
    }
  }
  g_far_skipped.store(far_skip, std::memory_order_relaxed);
  skip |= far_skip;
#else
  (void)active_phases;
#endif
  return skip;
}

bool PhasesActive() {
  return g_env_interval.load(std::memory_order_relaxed) > 1 || g_reflection_interval.load(std::memory_order_relaxed) > 1 ||
         g_far_interval.load(std::memory_order_relaxed) > 1;
}

uint32_t FarCascadePhasesSkipped() { return g_far_skipped.load(std::memory_order_relaxed); }

uint32_t FarCascadesInterval() {
  EnsurePreset();
  return ClampInterval(REXCVAR_GET(mcla_gfx_far_cascades_interval));
}

bool FarCascadesDump() {
  EnsurePreset();
  return REXCVAR_GET(mcla_gfx_far_cascades_interval) <= 0;
}

bool ShadowAtlasPrivate() {
  EnsurePreset();
  const int32_t mode = REXCVAR_GET(mcla_gfx_shadow_private);
  return mode < 0 ? ClampInterval(REXCVAR_GET(mcla_gfx_far_cascades_interval)) > 1 : mode > 0;
}

uint32_t ImpostorInterval() {
  EnsurePreset();
  const int32_t value = REXCVAR_GET(mcla_gfx_impostor_interval);
  return value <= 0 ? 0u : ClampInterval(value);
}

uint32_t ShadowMapSize() {
  EnsurePreset();
  return mcla::gfxres::ShadowEdgeOf(REXCVAR_GET(mcla_gfx_shadow_res));
}

EnvMapDimensions EnvMapSize() {
  EnsurePreset();
  const mcla::gfxres::EnvSize size = mcla::gfxres::EnvSizeOf(REXCVAR_GET(mcla_gfx_env_res));
  return {size.width, size.height};
}

bool ImpostorsEnabled() {
  EnsurePreset();
  return REXCVAR_GET(mcla_gfx_impostors);
}

void OnFrame() {
  EnsurePreset();
  mcla::vis::OnFrame();  // VISUALS: the mcla_vis_ab schedule (mcla_vis_ab.cpp), before the knobs are read
  ApplyTier();           // mcla_gfx_tier: a tier writes the knob cvars, so it comes before they are read below
  std::lock_guard<std::mutex> lock(g_mutex);
  // The per-call hooks read these relaxed atomics, not the cvars.
  g_dl_clear.store(uint32_t(REXCVAR_GET(mcla_gfx_dl_clear)), std::memory_order_relaxed);
  {
    Effective e;
    e.city_lod = REXCVAR_GET(mcla_gfx_city_lod);
    e.shadow_dist = REXCVAR_GET(mcla_gfx_shadow_dist);
    e.traffic_lod = REXCVAR_GET(mcla_gfx_traffic_lod);
    e.shadow_mode = REXCVAR_GET(mcla_gfx_shadow_mode);
    e.dl_disable = uint32_t(REXCVAR_GET(mcla_gfx_dl_disable));
    g_eff = e;
  }
  g_dl_disable.store(g_eff.dl_disable, std::memory_order_relaxed);
  g_shadow_mode.store(g_eff.shadow_mode, std::memory_order_relaxed);
  g_traffic_lod.store(float(g_eff.traffic_lod), std::memory_order_relaxed);
  g_ambient_density.store(float(REXCVAR_GET(mcla_gfx_ambient_density)), std::memory_order_relaxed);
  PublishPhaseIntervals();
  std::map<std::string, Entry> want;
  GatherPokes(&want);
  GatherKnobs(&want);
  if (!want.empty() || !g_saved.empty()) {
    Apply(want);
  }
  Dump();
}

}  // namespace mcla::gfxtune

// ---- the five mid-asm hooks (games/mcla/codegen/hooks_gfx.toml) ----------------------------------

namespace {
mcla::hooks::Registration g_reg_cull("gfx-cull", "mcla_gfx_dl_clear", MCLA_HOOK_GFX_CULL_MASK != 0,
                                     [] { return REXCVAR_GET(mcla_gfx_dl_clear) != 0; },
                                     /*default_on=*/false);
mcla::hooks::Registration g_reg_shadow_mode("gfx-shadow-mode", "mcla_gfx_shadow_mode",
                                            MCLA_HOOK_GFX_SHADOW_MODE != 0,
                                            [] {
                                              mcla::gfxtune::EnsurePreset();
                                              return REXCVAR_GET(mcla_gfx_shadow_mode) != 0;
                                            },
                                            /*default_on=*/false);
mcla::hooks::Registration g_reg_dl_enable("gfx-dl-disable", "mcla_gfx_dl_disable",
                                          MCLA_HOOK_GFX_DL_ENABLE != 0,
                                          [] {
                                            mcla::gfxtune::EnsurePreset();
                                            return REXCVAR_GET(mcla_gfx_dl_disable) != 0;
                                          },
                                          /*default_on=*/false);
mcla::hooks::Registration g_reg_traffic("gfx-traffic-lod", "mcla_gfx_traffic_lod",
                                        MCLA_HOOK_GFX_TRAFFIC_LOD != 0,
                                        [] {
                                          mcla::gfxtune::EnsurePreset();
                                          return REXCVAR_GET(mcla_gfx_traffic_lod) != 1.0;
                                        },
                                        /*default_on=*/false);
mcla::hooks::Registration g_reg_ambient("gfx-ambient", "mcla_gfx_ambient_density",
                                        MCLA_HOOK_GFX_AMBIENT_ZONE != 0,
                                        [] { return REXCVAR_GET(mcla_gfx_ambient_density) != 1.0; },
                                        /*default_on=*/false);
// The phase skip is an override of sub_822E6408 in mcla_fsr_markers.cpp: it needs both build flags (the far cascades also
// MCLA_HOOK_GFX_FAR_CASCADES, which makes PhasesToSkip() take them).
mcla::hooks::Registration g_reg_phases("gfx-phases", "mcla_gfx_env_interval",
                                       MCLA_HOOK_GFX_PHASES != 0 && MCLA_HOOK_FSR_MARKERS != 0,
                                       [] {
                                         mcla::gfxtune::EnsurePreset();
                                         return REXCVAR_GET(mcla_gfx_env_interval) > 1 ||
                                                REXCVAR_GET(mcla_gfx_reflection_interval) > 1 ||
                                                REXCVAR_GET(mcla_gfx_far_cascades_interval) > 1;
                                       },
                                       /*default_on=*/false);
}  // namespace

#if MCLA_HOOK_GFX_CULL_MASK
// sub_822E0130 (the batch visibility loop), after `bl 0x822df960`: r3 = the draw-list bits one box
// belongs to (0x20..0x100 the four shadow cascades, 0x2/0x4/0x8 reflection / env maps ...). Clearing a
// bit keeps the box out of that list: no draws recorded for it by the game, none replayed by us.
void mcla_hook_gfx_cull_mask(PPCRegister& r3) {
  MCLA_HOOK_ENTER();
  const uint32_t clear = mcla::gfxtune::g_dl_clear.load(std::memory_order_relaxed);
  if (clear) {
    r3.u64 = r3.u32 & ~clear;
  }
}
#else
void mcla_hook_gfx_cull_mask(PPCRegister&) {}
#endif

#if MCLA_HOOK_GFX_SHADOW_MODE
// sub_822F3BD8, before `cmplwi cr6,r11,0` at 0x822F4B9C: r11 = the game's night boolean; 1 takes its fast shadow path.
void mcla_hook_gfx_shadow_mode(PPCRegister& r11) {
  MCLA_HOOK_ENTER();
  if (mcla::gfxtune::g_shadow_mode.load(std::memory_order_relaxed) != 0) {
    r11.u64 = 1;
  }
}
#else
void mcla_hook_gfx_shadow_mode(PPCRegister&) {}
#endif

#if MCLA_HOOK_GFX_DL_ENABLE
// sub_822F3BD8, day path, before `bl 0x822e5ae0` at 0x822F4C48: r4 = the shadow draw-type bits the game ORs into
// [renderer+364]; the cvar's bits are taken out (the heartbeat cleared them in the mask once).
void mcla_hook_gfx_dl_enable(PPCRegister& r4) {
  MCLA_HOOK_ENTER();
  const uint32_t clear = mcla::gfxtune::g_dl_disable.load(std::memory_order_relaxed);
  if (clear) {
    r4.u64 = r4.u32 & ~clear;
  }
}
#else
void mcla_hook_gfx_dl_enable(PPCRegister&) {}
#endif

#if MCLA_HOOK_GFX_TRAFFIC_LOD
// sub_827902F0 (vehicle LOD select), after `lfs f0,40(r26)`: f0 = the LOD distance scale of the
// vehicle's tuning. The thresholds are [r26+44] x f0 and [r26+48] x f0 (+ 500 m cull).
void mcla_hook_gfx_traffic_lod(PPCRegister& f0) {
  MCLA_HOOK_ENTER();
  const float scale = mcla::gfxtune::g_traffic_lod.load(std::memory_order_relaxed);
  if (scale != 1.0f) {
    f0.f64 = double(float(f0.f64) * scale);
  }
}
#else
void mcla_hook_gfx_traffic_lod(PPCRegister&) {}
#endif

#if MCLA_HOOK_GFX_AMBIENT_ZONE
// sub_826F5B18 (ambient zone constructor), before its last `lwz r12,-8(r1)`: r3 = the zone (168 bytes).
// +0x60 pedestrian density (15), +0x98 parked-car fraction (0.25), scaled by mcla_gfx_ambient_density.
void mcla_hook_gfx_ambient_zone(PPCRegister& r3) {
  MCLA_HOOK_ENTER();
  const float scale = mcla::gfxtune::g_ambient_density.load(std::memory_order_relaxed);
  if (scale == 1.0f || r3.u32 < 0x10000) {
    return;
  }
  for (const uint32_t offset : {0x60u, 0x98u}) {
    uint32_t raw = 0;
    if (!mcla::guest::U32(r3.u32 + offset, &raw)) {
      continue;
    }
    float value;
    std::memcpy(&value, &raw, 4);
    value *= scale;
    std::memcpy(&raw, &value, 4);
    raw = __builtin_bswap32(raw);
    mcla::guest::WriteBytes(r3.u32 + offset, &raw, 4);
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 3) {
    REXLOG_INFO("[mcla-gfx] ambient zone {:08X}: pedestrian density and parked fraction x{}", r3.u32,
                double(scale));
  }
}
#else
void mcla_hook_gfx_ambient_zone(PPCRegister&) {}
#endif
