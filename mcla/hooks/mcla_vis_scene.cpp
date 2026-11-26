// mcla_vis_scene: the scene dev cvars, to put the game in a chosen time of day, weather and traffic density for a test or an
// A/B (the harness cannot press the game's own menus). DEV / TEST ONLY: every default is the game's own behaviour (nothing is
// touched), every cvar is live (read once per engine frame), and going back to the default writes the game's original value
// back. mcla::vis::SceneOnFrame() is called at the end of mcla::vis::OnFrame() (mcla_vis_ab.cpp), once per engine frame on the
// game thread, so a mcla_vis_ab schedule entry that changes one of these cvars takes effect in the same frame. All writes happen
// there. Always compiled; no MCLA_HOOK_ flag and no codegen entry.
//
// Cvar                    default  what
//   mcla_dev_tod          -1       -1 = the game's clock. 0..24 = hold this hour (24 = 0): the three hour slots, the freeze byte
//                                  (0) and the warp flag (0) are written every frame
//   mcla_dev_tod_freeze   -1       -1 = the game decides. 1 = the clock stops where it is (freeze byte 0), 0 = the clock is forced
//                                  to run (freeze byte 1). Ignored while mcla_dev_tod >= 0
//   mcla_dev_weather      -1       -1 = the game's weather. 0 Nice, 1 Cloudy, 2 Stormy (rain), 3 Foggy = hold it (the weather word
//                                  is written and the auto-change byte cleared every frame)
//   mcla_dev_weather_snap true     on the first frame of a new weather (and when a new world object appears) the four blend weights
//                                  are written one-hot: an instant switch. false = the game's own fade (about 16 s)
//   mcla_dev_road_density -1       -1 = the game's: every road uses its own density. 0..15 = the global road (traffic) density index
//
// Guest memory (all facts verified in the game code; the weather ones also in a live run). Guest memory is big-endian, floats are
// stored as byte-swapped IEEE bits.
//   T = u32 [0x8287E26C], the time-of-day object; null outside the world (then the time-of-day cvars do nothing)
//     T+0x38CC  float  the hour, 0..24, wraps at 24; T+0x38C4 and T+0x38C8 are the per-thread copies: the renderer reads
//               T+0x38C4+4*slot, slot 0..2, so all three are written (one 12-byte store)
//     T+0x38D1  byte   1 = the clock advances (the game's default), 0 = frozen. While it is 0 the weather blend snaps instead of
//               fading (so a frozen clock makes the weather tests quick)
//     T+0x38D8  byte   1 = a smooth warp of the hour is in progress, 0 cancels it
//   0x8287E134  u32    weather: 0 Nice, 1 Cloudy, 2 Stormy, 3 Foggy;  0x8287E138 byte: 1 = the game picks a random weather every 300 s
//   0x827E1B30  4 x float  the weather blend weights (+0, +4, +8, +0xC); the game moves them toward one-hot(weather) at 0.0625 / s
//   0x8282E930  s32    the global road density: -1 = off (each road uses its own), >= 0 = an index into the game's density table
//               (6 is the usual default); the size of the table is not known, so the index is clamped to 0..15
//
// Restore. The first time a field is taken over, its ORIGINAL bytes are remembered (for a T field, also the T they belong to); when
// the cvar goes back to its -1 the bytes are written back once. A T field is written back only while [0x8287E26C] is still the
// same T (a world that was torn down took its object with it; the original is dropped, nothing is written). The hour goes back
// to what it was when control was taken (not to where the held clock has been), the weather blend weights (if they were
// snapped) go back with the weather word, so the release is a clean return to the state the game was in.
//
// Log lines (one per change, from the game thread):
//   [mcla-vis] scene: tod=18.00 (frozen) weather=2 (snap) road_density=6     (only the cvars that are set; tod=game clock (frozen)
//                                                                              / (forced running) for mcla_dev_tod_freeze alone)
//   [mcla-vis] scene: tod applied to the time-of-day object 0x........      (a T appeared or changed while a T field is held)
//   [mcla-vis] scene: tod released (hour back to 14.50, warp byte back to 0, freeze byte back to 1)
//   [mcla-vis] scene: weather released (weather back to 0, auto byte back to 1, blend weights back to the game's)
//   [mcla-vis] scene: road_density released (back to -1)
//
// Cost when everything is -1 (the default): five cvar reads and a few flag tests, no guest access.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_guest.h"
#include "mcla_vis.h"

REXCVAR_DEFINE_DOUBLE(mcla_dev_tod, -1.0, "MCLA",
                      "MCLA dev/test: hold the game's time of day at this hour (0..24, 24 = 0): the hour slots are written every "
                      "frame and the clock is frozen. -1 = the game's own clock (the original hour is written back on release). "
                      "Needs a loaded world. Live (mcla_vis_scene.cpp)")
    .range(-1.0, 24.0);
REXCVAR_DEFINE_INT32(mcla_dev_tod_freeze, -1, "MCLA",
                     "MCLA dev/test: the game's clock. -1 = the game decides, 1 = frozen at whatever hour it shows, 0 = forced to "
                     "run. Ignored while mcla_dev_tod >= 0. Live (mcla_vis_scene.cpp)")
    .range(-1, 1);
REXCVAR_DEFINE_INT32(mcla_dev_weather, -1, "MCLA",
                     "MCLA dev/test: hold the weather: 0 Nice, 1 Cloudy, 2 Stormy (rain), 3 Foggy; the weather word is written and "
                     "the game's random weather change (every 300 s) switched off every frame. -1 = the game's weather (the "
                     "original values are written back on release). Live (mcla_vis_scene.cpp)")
    .range(-1, 3);
REXCVAR_DEFINE_BOOL(mcla_dev_weather_snap, true, "MCLA",
                    "MCLA dev/test: with mcla_dev_weather >= 0, true = switch at once (the four blend weights are written one-hot "
                    "on the first frame of a new weather), false = the game's own fade of about 16 s. Live (mcla_vis_scene.cpp)");
REXCVAR_DEFINE_INT32(mcla_dev_road_density, -1, "MCLA",
                     "MCLA dev/test: the global road (traffic) density index, written every frame: -1 = the game's own (every road "
                     "uses its own density), 0..15 = an index into the game's density table (6 is the usual default; the table's "
                     "size is not known, so the index is clamped to 0..15). Live (mcla_vis_scene.cpp)")
    .range(-1, 15);

namespace mcla::vis {
namespace {

constexpr uint32_t kTodPtr = 0x8287E26C;       // u32: the time-of-day object T (null outside the world)
constexpr uint32_t kTodHourSlots = 0x38C4;     // T+: three floats, the renderer's per-thread hour copies (0x38CC is the hour)
constexpr uint32_t kTodFreeze = 0x38D1;        // T+: byte, 1 = the clock advances, 0 = frozen
constexpr uint32_t kTodWarp = 0x38D8;          // T+: byte, 1 = a smooth warp in progress
constexpr uint32_t kWeather = 0x8287E134;      // u32: 0 Nice, 1 Cloudy, 2 Stormy, 3 Foggy
constexpr uint32_t kWeatherAuto = 0x8287E138;  // byte: 1 = a random weather change every 300 s
constexpr uint32_t kWeatherWeights = 0x827E1B30;  // 4 floats: the blend weights
constexpr uint32_t kRoadDensity = 0x8282E930;  // s32: -1 = each road its own, >= 0 = an index into the density table
constexpr int kRoadDensityMax = 15;

// One guest field the cvars have taken over: where it is, how big, the ORIGINAL bytes, and (for a field of T) which T.
struct Held {
  bool held = false;
  uint32_t base = 0;  // the time-of-day object this field belongs to; 0 = a static field
  uint32_t address = 0;
  uint8_t size = 0;
  uint8_t orig[16] = {};
};

enum class Release { kNone, kWritten, kDropped };

struct Key {  // the cvars, to see a change
  double tod;
  int32_t freeze;
  int32_t weather;
  bool snap;
  int32_t road;
  bool operator==(const Key&) const = default;
};

struct Scene {
  Held hour, warp, freeze;           // fields of T
  Held weather, weather_auto, weights, road;  // static fields
  Key last{-1.0, -1, -1, true, -1};  // the cvars at the last change (the defaults: nothing to log)
  uint32_t last_t = 0;               // the last non-null T seen while active
  int applied_weather = -1;          // the weather the blend weights were last handled for
  bool snap_pending = false;         // write the weights one-hot this frame
};

Scene& Sc() {
  static Scene s;  // the game thread only (SceneOnFrame is called from nowhere else)
  return s;
}

uint32_t BE32(uint32_t v) { return __builtin_bswap32(v); }

uint32_t FloatBE(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  return BE32(bits);
}

uint32_t WordOf(const uint8_t* be) {
  uint32_t v;
  std::memcpy(&v, be, 4);
  return BE32(v);
}

float FloatOf(const uint8_t* be) {
  const uint32_t bits = WordOf(be);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// Takes the field over: remembers the original the first time (or when the object it belonged to has changed), then stores.
// False when the memory cannot be read or written (the page is not mapped).
bool Put(Held& h, uint32_t base, uint32_t address, const void* data, size_t n) {
  if (h.held && (h.base != base || h.address != address)) {
    h.held = false;  // the object moved: the old original went with it
  }
  if (!h.held) {
    if (n > sizeof(h.orig) || !mcla::guest::Read(address, h.orig, n)) {
      return false;
    }
    h.held = true;
    h.base = base;
    h.address = address;
    h.size = static_cast<uint8_t>(n);
  }
  return mcla::guest::WriteBytes(address, data, n);
}

// Gives the field back: the original is written once; a field of T only while T is still the same. h.orig stays readable.
Release Restore(Held& h, uint32_t current_t) {
  if (!h.held) {
    return Release::kNone;
  }
  h.held = false;
  if (h.base != 0 && h.base != current_t) {
    return Release::kDropped;
  }
  return mcla::guest::WriteBytes(h.address, h.orig, h.size) ? Release::kWritten : Release::kDropped;
}


// The log text of a field that was given back: 'WHAT back to VALUE' when the original was written, 'WHAT not written back (...)' when
// it was dropped, nothing for a field that was not held. `value` is only called when there is something to say.
template <class ValueFn>
void Note(std::vector<std::string>* out, Release r, const char* what, ValueFn value) {
  if (r == Release::kWritten) {
    out->push_back(std::string(what) + " back to " + value());
  } else if (r == Release::kDropped) {
    out->push_back(std::string(what) + " not written back (the world object is gone)");
  }
}

std::string Join(const std::vector<std::string>& parts, const char* sep) {
  std::string s;
  for (const std::string& p : parts) {
    if (!s.empty()) {
      s += sep;
    }
    s += p;
  }
  return s;
}

std::string Fixed2(double v) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.2f", v);
  return buf;
}

bool AnyHeld(const Scene& s) {
  return s.hour.held || s.warp.held || s.freeze.held || s.weather.held || s.weather_auto.held || s.weights.held || s.road.held;
}

}  // namespace

void SceneOnFrame() {
  Scene& s = Sc();
  const double tod_cvar = REXCVAR_GET(mcla_dev_tod);
  const int32_t freeze_cvar = REXCVAR_GET(mcla_dev_tod_freeze);
  const int32_t weather_cvar = REXCVAR_GET(mcla_dev_weather);
  const bool snap = REXCVAR_GET(mcla_dev_weather_snap);
  const int32_t road_cvar = REXCVAR_GET(mcla_dev_road_density);

  const bool tod_on = tod_cvar >= 0.0;  // false for NaN
  const int freeze = tod_on ? -1 : (freeze_cvar < 0 ? -1 : (freeze_cvar > 0 ? 1 : 0));  // mcla_dev_tod wins over the freeze cvar
  const int weather = weather_cvar < 0 ? -1 : (weather_cvar > 3 ? 3 : weather_cvar);
  const int road = road_cvar < 0 ? -1 : (road_cvar > kRoadDensityMax ? kRoadDensityMax : road_cvar);

  if (!tod_on && freeze < 0 && weather < 0 && road < 0 && !AnyHeld(s)) {
    s.last = Key{-1.0, -1, -1, snap, -1};  // nothing on, nothing held: the game's own (a set-and-reset must log the next set)
    return;
  }

  // The time-of-day object: the fields of T, and the weather's snap on a new world object.
  uint32_t t = 0;
  if (!mcla::guest::U32(kTodPtr, &t) || t < 0x10000) {
    t = 0;  // outside the world
  }

  std::vector<std::string> released;  // one entry per group whose fields were given back this frame

  // ---- time of day ----------------------------------------------------------------------------------------------------------
  float hour = 0.0f;
  if (tod_on) {
    hour = static_cast<float>(tod_cvar);
    if (!(hour < 24.0f)) {
      hour = 0.0f;  // 24 is 0
    }
    if (t != 0) {
      const bool fresh = !(s.hour.held && s.hour.base == t);
      const uint32_t bits = FloatBE(hour);
      const uint32_t slots[3] = {bits, bits, bits};
      const uint8_t zero = 0;
      Put(s.hour, t, t + kTodHourSlots, slots, sizeof(slots));
      Put(s.warp, t, t + kTodWarp, &zero, 1);
      Put(s.freeze, t, t + kTodFreeze, &zero, 1);
      if (fresh && s.hour.held) {
        REXLOG_INFO("[mcla-vis] scene: tod applied to the time-of-day object 0x{:08X}", t);
      }
    }
  }
  std::vector<std::string> tod_back;
  if (!tod_on) {
    Note(&tod_back, Restore(s.hour, t), "hour", [&] { return Fixed2(FloatOf(s.hour.orig + 8)); });  // slot 2 = T+0x38CC, the hour proper
    Note(&tod_back, Restore(s.warp, t), "warp byte", [&] { return std::to_string(s.warp.orig[0]); });
  }
  if (freeze >= 0 && t != 0) {
    const uint8_t byte = freeze == 1 ? 0 : 1;  // cvar 1 = frozen = the byte 0; cvar 0 = forced to run = the byte 1
    const bool fresh = !(s.freeze.held && s.freeze.base == t);
    Put(s.freeze, t, t + kTodFreeze, &byte, 1);
    if (fresh && s.freeze.held) {
      REXLOG_INFO("[mcla-vis] scene: tod applied to the time-of-day object 0x{:08X}", t);
    }
  } else if (!tod_on && freeze < 0) {
    Note(&tod_back, Restore(s.freeze, t), "freeze byte", [&] { return std::to_string(s.freeze.orig[0]); });
  }
  if (!tod_back.empty()) {
    released.push_back("tod released (" + Join(tod_back, ", ") + ")");
  }

  // ---- weather --------------------------------------------------------------------------------------------------------------
  if (weather >= 0) {
    const uint32_t word = BE32(static_cast<uint32_t>(weather));
    const uint8_t zero = 0;
    Put(s.weather, 0, kWeather, &word, 4);
    Put(s.weather_auto, 0, kWeatherAuto, &zero, 1);
    if (weather != s.applied_weather) {
      s.applied_weather = weather;
      s.snap_pending = snap;  // the first frame of a new value
    }
    if (t != 0 && t != s.last_t && snap) {
      s.snap_pending = true;  // a new world object: the game may have reset the blend
    }
    if (s.snap_pending) {
      uint32_t weights[4];
      for (int i = 0; i < 4; ++i) {
        weights[i] = FloatBE(i == weather ? 1.0f : 0.0f);
      }
      Put(s.weights, 0, kWeatherWeights, weights, sizeof(weights));
      s.snap_pending = false;
    }
  } else {
    std::vector<std::string> back;
    Note(&back, Restore(s.weather, 0), "weather", [&] { return std::to_string(WordOf(s.weather.orig)); });
    Note(&back, Restore(s.weather_auto, 0), "auto byte", [&] { return std::to_string(s.weather_auto.orig[0]); });
    Note(&back, Restore(s.weights, 0), "blend weights", [] { return std::string("the game's"); });
    if (!back.empty()) {
      released.push_back("weather released (" + Join(back, ", ") + ")");
    }
    s.applied_weather = -1;
    s.snap_pending = false;
  }
  if (t != 0) {
    s.last_t = t;
  }

  // ---- road density ---------------------------------------------------------------------------------------------------------
  if (road >= 0) {
    const uint32_t word = BE32(static_cast<uint32_t>(road));
    Put(s.road, 0, kRoadDensity, &word, 4);
  } else if (Restore(s.road, 0) == Release::kWritten) {
    released.push_back("road_density released (back to " + std::to_string(static_cast<int32_t>(WordOf(s.road.orig))) + ")");
  }

  // ---- the log: the lines for what was given back, then one line with what is set, once per change of the cvars ---------
  const Key key{tod_cvar, freeze_cvar, weather_cvar, snap, road_cvar};
  if (key == s.last) {
    return;
  }
  s.last = key;
  for (const std::string& line : released) {
    REXLOG_INFO("[mcla-vis] scene: {}", line);
  }
  std::vector<std::string> active;
  if (tod_on) {
    active.push_back("tod=" + Fixed2(hour) + " (frozen)");
  } else if (freeze == 1) {
    active.push_back("tod=game clock (frozen)");
  } else if (freeze == 0) {
    active.push_back("tod=game clock (forced running)");
  }
  if (weather >= 0) {
    active.push_back("weather=" + std::to_string(weather) + (snap ? " (snap)" : " (fade)"));
  }
  if (road >= 0) {
    active.push_back("road_density=" + std::to_string(road));
  }
  if (!active.empty()) {
    REXLOG_INFO("[mcla-vis] scene: {}", Join(active, " "));
  }
}

}  // namespace mcla::vis
