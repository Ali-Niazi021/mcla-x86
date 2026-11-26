// mcla_vis_ab: in-run A/B scheduler. Changes cvars at given seconds of ONE game run, so two settings (swap_post_effect=fxaa
// against none, mcla_gfx_city_lod 1.0 against 1.5, ...) are measured in the same scene, with the same traffic and the same
// camera. The harness has no way to change a cvar mid-run; this is the game-side half of the VISUALS A/B method.
//
// Cvar mcla_vis_ab (string, default "none" = off): entries joined by '+', each
//     SEC:CVARNAME:VALUE
//   SEC       float seconds since PROCESS START (not since the game's first frame): the process creation time
//             (GetProcessTimes) against GetSystemTimePreciseAsFileTime. That is the clock the harness's screenshot
//             names count in, so a switch lines up with the screenshots.
//   CVARNAME  any registered cvar (rex::cvar::SetFlagByName; a name that does not exist is reported REJECTED)
//   VALUE     the text given to SetFlagByName, may be empty (an empty string value) and may contain ':' but not '+'
//   mcla_vis_ab=60:swap_post_effect:fxaa+68:swap_post_effect:none+76:mcla_gfx_city_lod:1.5
// Entries may be in any order (sorted by time; equal times keep their written order). A malformed entry is logged and
// skipped. Whitespace around SEC and CVARNAME is ignored; VALUE is taken as written.
//
// OnFrame() runs once per engine frame on the game thread (mcla_gfx_tune.cpp calls it). The string is parsed on the first call
// and again whenever it changes (a schedule changed from the console restarts: every entry already past is applied at once,
// in order). Each applied entry logs one line:
//     [mcla-vis] ab t=60.012 qpc_ms=1234567.891 swap_post_effect=fxaa (ok) now=fxaa
//   t        seconds since process start at the moment of the switch (the frame boundary at or after the scheduled second)
//   qpc_ms   QueryPerformanceCounter in milliseconds, the unit PresentMon writes with --qpc_time_ms, so the switch can be
//            placed on the PresentMon frame table
//   (ok)     SetFlagByName accepted the value; (REJECTED) it did not (unknown name, value out of range or unparseable, a
//            higher-priority source that cannot be outranked)
//   now=     the value read back with GetFlagByName
// and one line at each parse: '[mcla-vis] ab schedule: N entries', plus a warning per malformed entry.
//
// Always compiled; no MCLA_HOOK_ flag and no codegen entry. Nothing here touches guest memory.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_vis.h"

REXCVAR_DEFINE_STRING(mcla_vis_ab, "none", "MCLA",
                      "MCLA visuals A/B: cvar changes at given seconds of one run, 'SEC:CVARNAME:VALUE' entries joined by '+' "
                      "(SEC = seconds since process start, the harness's screenshot clock; VALUE may be empty or contain ':' "
                      "but not '+'), e.g. 60:swap_post_effect:fxaa+68:swap_post_effect:none. Each switch logs "
                      "'[mcla-vis] ab t=.. qpc_ms=.. name=value (ok|REJECTED)' (qpc_ms = PresentMon's --qpc_time_ms clock). "
                      "none = off. Live (mcla_vis_ab.cpp)");

namespace mcla::vis {
namespace {

struct Entry {
  double sec = 0.0;
  std::string name;
  std::string value;
};

// Game thread only (OnFrame is called from nowhere else), so plain statics.
struct State {
  bool parsed = false;
  std::string text;          // the cvar string the schedule was parsed from
  std::vector<Entry> sched;  // sorted by sec
  size_t next = 0;           // first entry not yet applied
};

State& St() {
  static State s;
  return s;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
    s.remove_suffix(1);
  }
  return s;
}

bool ParseSeconds(std::string_view text, double* out) {
  if (text.empty()) {
    return false;
  }
  double v = 0.0;
  const char* const first = text.data();
  const char* const last = first + text.size();
  const auto r = std::from_chars(first, last, v);
  if (r.ec != std::errc() || r.ptr != last || !std::isfinite(v) || v < 0.0) {
    return false;
  }
  *out = v;
  return true;
}

// One 'SEC:NAME:VALUE' item; false (with *why) when malformed.
bool ParseEntry(std::string_view item, Entry* out, const char** why) {
  const size_t c1 = item.find(':');
  if (c1 == std::string_view::npos) {
    *why = "no ':' (want SEC:CVARNAME:VALUE)";
    return false;
  }
  const size_t c2 = item.find(':', c1 + 1);
  if (c2 == std::string_view::npos) {
    *why = "only one ':' (want SEC:CVARNAME:VALUE; an empty VALUE still needs the second ':')";
    return false;
  }
  double sec = 0.0;
  if (!ParseSeconds(Trim(item.substr(0, c1)), &sec)) {
    *why = "SEC is not a number >= 0";
    return false;
  }
  const std::string_view name = Trim(item.substr(c1 + 1, c2 - c1 - 1));
  if (name.empty() || name.find_first_of(" \t\r\n") != std::string_view::npos) {
    *why = "CVARNAME is empty or contains whitespace";
    return false;
  }
  out->sec = sec;
  out->name.assign(name);
  out->value.assign(item.substr(c2 + 1));
  return true;
}

void Parse(State& s, const std::string& text) {
  s.parsed = true;
  s.text = text;
  s.sched.clear();
  s.next = 0;
  if (text.empty() || text == "none") {
    return;
  }
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find('+', pos);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string_view item(text.data() + pos, end - pos);
    pos = end + 1;
    if (Trim(item).empty()) {
      continue;  // '++' or a trailing '+'
    }
    Entry e;
    const char* why = "";
    if (ParseEntry(item, &e, &why)) {
      s.sched.push_back(std::move(e));
    } else {
      REXLOG_WARN("[mcla-vis] ab: malformed entry '{}' skipped: {}", std::string(item), why);
    }
  }
  std::stable_sort(s.sched.begin(), s.sched.end(), [](const Entry& a, const Entry& b) { return a.sec < b.sec; });
  REXLOG_INFO("[mcla-vis] ab schedule: {} entries", s.sched.size());
}

uint64_t FileTimeU64(const FILETIME& ft) {
  return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// Process creation time as 100 ns FILETIME ticks; 0 if it cannot be read.
uint64_t ProcessStartTicks() {
  static const uint64_t start = [] {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
      return uint64_t{0};
    }
    return FileTimeU64(created);
  }();
  return start;
}

// Seconds since process start; the same FILETIME clock the process creation time is on.
double SecondsSinceProcessStart() {
  const uint64_t start = ProcessStartTicks();
  FILETIME now{};
  GetSystemTimePreciseAsFileTime(&now);
  const uint64_t t = FileTimeU64(now);
  if (start == 0 || t < start) {
    return 0.0;
  }
  return static_cast<double>(t - start) * 1e-7;
}

double QpcMs() {
  static const double freq = [] {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
  LARGE_INTEGER c{};
  QueryPerformanceCounter(&c);
  return freq > 0.0 ? static_cast<double>(c.QuadPart) / freq * 1000.0 : 0.0;
}

}  // namespace

static void AbOnFrame() {
  State& s = St();
  const std::string& text = REXCVAR_GET(mcla_vis_ab);
  if (!s.parsed || text != s.text) {
    Parse(s, text);
  }
  if (s.next >= s.sched.size()) {
    return;
  }
  const double now = SecondsSinceProcessStart();
  while (s.next < s.sched.size() && s.sched[s.next].sec <= now) {
    const Entry& e = s.sched[s.next];
    ++s.next;
    const double t = SecondsSinceProcessStart();
    const double qpc_ms = QpcMs();
    const bool ok = rex::cvar::SetFlagByName(e.name, e.value);
    const std::string back = rex::cvar::GetFlagByName(e.name);
    REXLOG_INFO("[mcla-vis] ab t={:.3f} qpc_ms={:.3f} {}={} ({}) now={}", t, qpc_ms, e.name, e.value,
                ok ? "ok" : "REJECTED", back);
  }
}

void OnFrame() {
  AbOnFrame();
  SceneOnFrame();  // the mcla_dev_* scene cvars (mcla_vis_scene.cpp), after the schedule so a switch it makes applies this frame
}

}  // namespace mcla::vis
