// mcla_cheat: the classic cheats of the mod menu's Cheats tab, done through the game's OWN switches (docs/CHEATS.md has the evidence,
// addresses, grades and the risk to a save of every one). No mid-asm hook, no codegen entry, no MCLA_HOOK_ flag: guest memory
// writes once per engine frame, from the heartbeat hook (mcla_heartbeat.cpp, the first instruction of the engine timer's update,
// game thread), and only while the cheat is on. Always compiled; every cvar is transient (never saved), off at every start, and
// does nothing unless the gate mcla_overlay_cheats is on.
//
//   cvar                    what                                                        how (guest memory, big-endian)
//   mcla_cheat_nitro        infinite nitro                                              the game's cheat byte 0x8288E5B0 = 1
//   mcla_cheat_special      infinite Zone / Agro / Roar / EMP                           the game's cheat byte 0x8288E5B1 = 1
//   mcla_cheat_nodamage     no vehicle damage                                           the game's cheat byte 0x8288E5A7 = 1
//   mcla_cheat_nocops       no police                                                   the game's cheat byte 0x8288E5B4 = 1
//   mcla_cheat_timescale    slow motion, 1 = off (0.05 .. 1)                            the engine timer's scale float 0x827D7554 x factor
//   mcla_cheat_nosave       the game writes no save this session                        the game's `nosave` parameter word 0x8286D878
//   mcla_cheat_unlock       races, parts, performance upgrades, special abilities       the game's `unlock*` parameter words
//   mcla_cheat_money_*      give / take / set the career's money (a button, once)       int [[0x8287484C] + 0x1FC]
//
// The four bytes are the game's built-in cheat block (its pause menu's "Cheats" page toggles the same bytes; sub_8264B2B0), read by
// the game's own code at the points where the nitro bar is spent, the damage is applied, the cops are spawned. The game also treats a
// drive with any of them on as "cheating" (sub_82387A98): it earns no reputation and no money from it (Dlg_Title_NoEarnRep). That is
// the game's rule and is kept. The bytes are held at 1 every frame while a drive is on screen, and the game's own value is written
// back the moment the cheat is switched off or the drive ends (a menu, a load, the garage: nothing is held there).
//
// Log lines, tag [mcla-cheat], one per change of a cheat's state: ON (what is held), waiting (the screen is not a drive), REFUSED (the
// guest memory is not what the cheat expects: nothing was written), released (what was written back). Money: one line per press with
// the old and the new amount.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_cheat.h"
#include "mcla_cheat_math.h"
#include "mcla_game_state.h"
#include "mcla_guest.h"
#include "mcla_hooks.h"

REXCVAR_DECLARE(bool, mcla_overlay_cheats);

REXCVAR_DEFINE_BOOL(mcla_cheat_nitro, false, "MCLA",
                    "Cheat: infinite nitro. Holds the game's own unlimited-nitrous cheat byte (0x8288E5B0) while you drive; the nitro bars are "
                    "never spent. Off at every start, never saved, needs mcla_overlay_cheats. The game earns no reputation or money from a "
                    "drive with a cheat on. Live (mcla_cheat.cpp)")
    .transient();
REXCVAR_DEFINE_BOOL(mcla_cheat_special, false, "MCLA",
                    "Cheat: infinite special abilities (Zone, Agro, Roar, EMP). Holds the game's own unlimited-special cheat byte (0x8288E5B1) "
                    "while you drive. Off at every start, never saved, needs mcla_overlay_cheats. Live (mcla_cheat.cpp)")
    .transient();
REXCVAR_DEFINE_BOOL(mcla_cheat_nodamage, false, "MCLA",
                    "Cheat: no damage. Holds the game's own unlimited-damage cheat byte (0x8288E5A7) while you drive: collisions do not "
                    "damage the vehicle. Off at every start, never saved, needs mcla_overlay_cheats. Live (mcla_cheat.cpp)")
    .transient();
REXCVAR_DEFINE_BOOL(mcla_cheat_nocops, false, "MCLA",
                    "Cheat: no police. Holds the game's own no-cops cheat byte (0x8288E5B4) while you drive: no cop is spawned and the "
                    "pursuit code does not run. Off at every start, never saved, needs mcla_overlay_cheats. Live (mcla_cheat.cpp)")
    .transient();
REXCVAR_DEFINE_DOUBLE(mcla_cheat_timescale, 1.0, "MCLA",
                      "Cheat: slow motion. The game's world runs at this fraction of its speed while you drive (0.05 to 1; 1 = off): the "
                      "engine timer's scale (0x827D7554) is multiplied by it every frame, so the game's own pause (scale 0) still pauses. Off at "
                      "every start, never saved, needs mcla_overlay_cheats. Live (mcla_cheat.cpp)")
    .range(0.05, 1.0)
    .transient();
REXCVAR_DEFINE_BOOL(mcla_cheat_nosave, false, "MCLA",
                    "Safety: the game writes no save while this is on, whatever you do (it is the game's own `nosave` parameter, which makes "
                    "its save manager skip every save and carry on). The game may still SAY it saved. Held on every screen. Off at every start, "
                    "never saved, needs mcla_overlay_cheats. Live (mcla_cheat.cpp)")
    .transient();
REXCVAR_DEFINE_BOOL(mcla_cheat_unlock, false, "MCLA",
                    "Cheat: unlock the races, the parts, the performance upgrades and the special abilities (the game's own `unlockraces`, "
                    "`unlockparts`, `unlockperformance`, `unlockspecialabilities` parameters: the game's 'is it unlocked' checks answer yes; "
                    "its save data is not rewritten). Cars are not covered. Held in a drive, the pause menu, the garage and the map. What you "
                    "do with an unlocked item (buy, fit, race) can end up in your save. Off at every start, never saved, needs "
                    "mcla_overlay_cheats. Live (mcla_cheat.cpp)")
    .transient();
REXCVAR_DEFINE_INT32(mcla_cheat_money_amount, 100000, "MCLA",
                     "Cheat: the amount the money buttons use (0 to 1,000,000,000). Only read when a button is pressed. Never saved. Live "
                     "(mcla_cheat.cpp)")
    .range(0, 1000000000)
    .transient();
REXCVAR_DEFINE_INT32(mcla_cheat_money_request, 0, "MCLA",
                     "Cheat: a money button press, consumed by the game thread on its next frame (the menu sets it, the game resets it to 0): "
                     "1 = give the amount, 2 = take it, 3 = set the money to it. Acts once, only on a career screen (a drive, the pause menu, "
                     "the garage, the map), logs the old and the new amount ([mcla-cheat] money), and the new amount can end up in your save. "
                     "Never saved, needs mcla_overlay_cheats. Live (mcla_cheat.cpp)")
    .range(0, 3)
    .transient();

namespace mcla::cheat {
namespace {

namespace math = mcla::cheat::math;
using math::Screen;

constexpr int kN = int(Which::kCount);

// ---- addresses (docs/CHEATS.md: the evidence; every one is checked against the guest memory before a write) ---------------------
constexpr uint32_t kFlagNitro = 0x8288E5B0;    // byte: 1 = unlimited nitrous   (read at 0x823456E0, 0x82345848, 0x823459DC)
constexpr uint32_t kFlagSpecial = 0x8288E5B1;  // byte: 1 = unlimited special   (read at 0x82338E14)
constexpr uint32_t kFlagDamage = 0x8288E5A7;   // byte: 1 = unlimited damage    (read at 0x82331C10, 0x82342918, 0x823431AC, 0x82344510, 0x82356DF0)
constexpr uint32_t kFlagCops = 0x8288E5B4;     // byte: 1 = no cops             (read at 0x827000AC, 0x82704310, 0x82707224, 0x8270909C, 0x82709CB8)
constexpr uint32_t kTimerScale = 0x827D7554;   // float: the engine timer's time scale (rage::sysTimeManager + 0x54, stored every frame at 0x822C2124)
constexpr uint32_t kExpManagerPtr = 0x8287484C;   // u32: the career's experience manager (mcExperienceManager singleton)
constexpr uint32_t kExpManagerVtable = 0x820BBDC4;  // its vtable (set by the constructor, sub_826BA578)
constexpr uint32_t kMoneyOffset = 0x1FC;       // s32 in the manager: the career's money (constructor sets 5000)

struct FlagDef {
  Which which;
  const char* name;
  const char* label;       // for the log
  uint32_t address;
  math::Hold hold;
};

struct ParamDef {
  const char* name;  // the parameter's name: node+0 points to this string
  uint32_t node;     // the parameter node; its value word is node+4 (non-zero = given)
  math::Hold32 hold;
  bool verified = false;   // the node's name has been read and is `name`
  uint32_t name_ptr = 0;   // that name pointer (what the cheat stores as the argument)
};

// The unlock parameters: each is read live by one small "is it unlocked" function (sub_826BDA38 races, sub_826BCDD8 parts,
// sub_826BCD90 performance, sub_826BCE48 special abilities).
constexpr int kUnlockCount = 4;

enum class Why : uint8_t { kNone, kGateOff, kNotDriving, kNotCareer, kBadFlag, kBadParam, kBadTimer, kWriteFailed };

struct Cheats {
  FlagDef flags[4] = {
      {Which::kNitro, "nitro", "unlimited-nitrous byte 0x8288E5B0", kFlagNitro, {}},
      {Which::kSpecial, "special", "unlimited-special byte 0x8288E5B1", kFlagSpecial, {}},
      {Which::kDamage, "damage", "unlimited-damage byte 0x8288E5A7", kFlagDamage, {}},
      {Which::kCops, "cops", "no-cops byte 0x8288E5B4", kFlagCops, {}},
  };
  math::ScaleState scale;
  ParamDef nosave{"nosave", 0x8286D874, {}};
  ParamDef unlock[kUnlockCount] = {{"unlockraces", 0x82909518, {}},
                                   {"unlockparts", 0x829094B4, {}},
                                   {"unlockperformance", 0x829094DC, {}},
                                   {"unlockspecialabilities", 0x82909504, {}}};
  State last_state[kN] = {};
  Why last_why[kN] = {};
  Screen last_screen[kN] = {};
  bool idle = false;           // the last frame found nothing on and nothing held (states already published as off)
  double logged_factor = 1.0;  // the time scale factor of the last log line
  int64_t factor_log_ms = 0;   // when it was written (a slider drags through many values: one line a second)
};

Cheats& Cs() {
  static Cheats c;  // the game thread only (OnFrame is called from nowhere else)
  return c;
}

// What the menu reads (any thread).
std::atomic<uint8_t> g_state[kN];
std::atomic<uint8_t> g_why[kN];
std::atomic<uint8_t> g_screen{0};

std::mutex g_msg_mutex;
std::string g_money_message;  // the last money press
std::atomic<int32_t> g_money_seen{0};
std::atomic<bool> g_money_seen_ok{false};

const char* Name(Which w) {
  switch (w) {
    case Which::kNitro: return "nitro";
    case Which::kSpecial: return "special abilities";
    case Which::kDamage: return "no damage";
    case Which::kCops: return "no police";
    case Which::kTimeScale: return "time scale";
    case Which::kNoSave: return "no save";
    case Which::kUnlock: return "unlock";
    default: return "?";
  }
}

const char* NeedText(Which w) {
  return w == Which::kUnlock ? "a drive, the pause menu, the garage or the map" : "a drive";
}

std::string Commas(int64_t v) {
  std::string digits = std::to_string(v < 0 ? -v : v);
  std::string out;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
    out += digits[i];
  }
  return v < 0 ? "-" + out : out;
}

// ---- guest memory --------------------------------------------------------------------------------------------------------------

bool ReadByte(uint32_t address, uint8_t* out) { return mcla::guest::U8(address, out); }

bool WriteWord(uint32_t address, uint32_t value) {
  const uint32_t be = __builtin_bswap32(value);
  return mcla::guest::WriteBytes(address, &be, 4);
}

bool ReadFloat(uint32_t address, float* out) {
  uint32_t bits = 0;
  if (!mcla::guest::U32(address, &bits)) return false;
  std::memcpy(out, &bits, 4);
  return true;
}

// A parameter node is what it says it is: [node] points to its name. False when the memory cannot be read or the name differs
// (then the build is not the one these addresses were found in, and nothing is written).
bool ParamNodeOk(const ParamDef& p, uint32_t* value) {
  uint32_t name_ptr = 0;
  if (!mcla::guest::U32(p.node, &name_ptr) || !mcla::guest::InImage(name_ptr)) return false;
  char text[40];
  bool complete = false;
  if (mcla::guest::CString(name_ptr, text, sizeof(text), &complete) <= 0 || !complete) return false;
  if (std::strcmp(text, p.name) != 0) return false;
  return mcla::guest::U32(p.node + 4, value);
}

// The experience manager and the career's money. False with `why` when it is not there (before a career is loaded) or not what it
// should be.
bool ReadMoney(uint32_t* manager, int32_t* money, const char** why) {
  uint32_t m = 0;
  if (!mcla::guest::U32(kExpManagerPtr, &m) || m == 0) {
    *why = "no career is loaded (no experience manager)";
    return false;
  }
  uint32_t vtable = 0;
  if (!math::PlausibleHeapPointer(m) || !mcla::guest::U32(m, &vtable) || vtable != kExpManagerVtable) {
    *why = "the object is not the experience manager (its vtable is not 0x820BBDC4)";
    return false;
  }
  uint32_t raw = 0;
  if (!mcla::guest::U32(m + kMoneyOffset, &raw)) {
    *why = "the money field cannot be read";
    return false;
  }
  *manager = m;
  *money = int32_t(raw);
  return true;
}

// ---- state and log ---------------------------------------------------------------------------------------------------------------

void Publish(Which w, State s, Why why) {
  g_state[int(w)].store(uint8_t(s), std::memory_order_relaxed);
  g_why[int(w)].store(uint8_t(why), std::memory_order_relaxed);
}

// Sets the state the menu shows and logs the line for a change of state (not for every frame in the same state).
void Report(Cheats& c, Which w, State s, Why why, Screen screen, const char* detail) {
  Publish(w, s, why);
  const int i = int(w);
  const bool screen_matters = s == State::kWaiting;
  if (c.last_state[i] == s && c.last_why[i] == why && (!screen_matters || c.last_screen[i] == screen)) return;
  const State before = c.last_state[i];
  c.last_state[i] = s;
  c.last_why[i] = why;
  c.last_screen[i] = screen;
  switch (s) {
    case State::kOn:
      REXLOG_INFO("[mcla-cheat] {}: ON ({})", Name(w), detail);
      break;
    case State::kWaiting:
      REXLOG_INFO("[mcla-cheat] {}: waiting for {} (the screen is '{}'): the game's own value stays", Name(w), NeedText(w),
                  mcla::gamestate::ScreenName(screen));
      break;
    case State::kRefused:
      REXLOG_WARN("[mcla-cheat] {}: REFUSED: {}", Name(w), detail);
      break;
    case State::kOff:
      if (before != State::kOff) REXLOG_INFO("[mcla-cheat] {}: off", Name(w));
      break;
  }
}

// ---- the four flag bytes -----------------------------------------------------------------------------------------------------------

void RunFlag(Cheats& c, FlagDef& f, bool raw, bool gate, Screen screen) {
  if (!raw && !f.hold.held) {
    Report(c, f.which, State::kOff, Why::kNone, screen, "");
    return;
  }
  const bool driving = math::DrivingOnly(screen);
  const bool want = raw && gate && driving;
  uint8_t current = 0;
  const bool readable = ReadByte(f.address, &current);
  if (!readable || current > 1) {
    // The byte is not a bool where it was found. Do not write; a held byte we can no longer read cannot be given back.
    if (f.hold.held) {
      REXLOG_WARN("[mcla-cheat] {}: the flag byte 0x{:08X} cannot be read as a bool now ({}): dropped the hold without writing", f.name,
                  f.address, readable ? int(current) : -1);
      f.hold = {};
    }
    if (raw) {
      Report(c, f.which, State::kRefused, Why::kBadFlag, screen, "the game's flag byte is not where it was found");
    } else {
      Report(c, f.which, State::kOff, Why::kNone, screen, "");
    }
    return;
  }
  const math::Step step = math::StepFlag(f.hold, want, current);
  if (step.write && !mcla::guest::WriteU8(f.address, step.value)) {
    Report(c, f.which, State::kRefused, Why::kWriteFailed, screen, "the flag byte cannot be written");
    return;
  }
  switch (step.act) {
    case math::Act::kTake:
      if (!step.write) REXLOG_INFO("[mcla-cheat] {}: the game already had its {} on: nothing written", f.name, f.label);
      break;
    case math::Act::kRewrite:
      REXLOG_INFO("[mcla-cheat] {}: the game cleared its {}: put back", f.name, f.label);
      break;
    case math::Act::kRelease:
      REXLOG_INFO("[mcla-cheat] {}: released: {} back to {}", f.name, f.label, int(step.value));
      break;
    case math::Act::kReleaseLeft:
      REXLOG_INFO("[mcla-cheat] {}: released: {} left as the game has it", f.name, f.label);
      break;
    default:
      break;
  }
  if (!raw) {
    Report(c, f.which, State::kOff, Why::kNone, screen, "");
  } else if (!gate) {
    Report(c, f.which, State::kRefused, Why::kGateOff, screen, "locked: \"Allow cheats and mods\" is off");
  } else if (!driving) {
    Report(c, f.which, State::kWaiting, Why::kNotDriving, screen, "");
  } else {
    Report(c, f.which, State::kOn, Why::kNone, screen, f.label);
  }
}

// ---- the time scale ----------------------------------------------------------------------------------------------------------------

void RunTimeScale(Cheats& c, bool raw, bool gate, Screen screen) {
  const Which w = Which::kTimeScale;
  if (!raw && !c.scale.active) {
    Report(c, w, State::kOff, Why::kNone, screen, "");
    return;
  }
  const bool driving = math::DrivingOnly(screen);
  const bool want = raw && gate && driving;
  const double factor = math::ClampFactor(REXCVAR_GET(mcla_cheat_timescale));
  float current = 0.f;
  if (!ReadFloat(kTimerScale, &current)) {
    if (raw) Report(c, w, State::kRefused, Why::kBadTimer, screen, "the engine timer's scale cannot be read");
    return;
  }
  const math::ScaleStep step = math::StepScale(c.scale, want, factor, current);
  if (step.write) {
    uint32_t bits;
    std::memcpy(&bits, &step.value, 4);
    if (!WriteWord(kTimerScale, bits)) {
      Report(c, w, State::kRefused, Why::kWriteFailed, screen, "the engine timer's scale cannot be written");
      return;
    }
  }
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (want && std::fabs(factor - c.logged_factor) > 0.005 && now_ms - c.factor_log_ms >= 1000) {
    c.logged_factor = factor;
    c.factor_log_ms = now_ms;
    REXLOG_INFO("[mcla-cheat] time scale: x{:.2f} of the game's own scale {:.3f} (timer word 0x{:08X} = {:.3f})", factor, double(step.base),
                kTimerScale, double(step.value));
  }
  if (!raw) {
    c.logged_factor = 1.0;
    Report(c, w, State::kOff, Why::kNone, screen, "");
  } else if (!gate) {
    Report(c, w, State::kRefused, Why::kGateOff, screen, "locked: \"Allow cheats and mods\" is off");
  } else if (!driving) {
    Report(c, w, State::kWaiting, Why::kNotDriving, screen, "");
  } else {
    Report(c, w, State::kOn, Why::kNone, screen, "the engine timer's scale is multiplied");
  }
}

// ---- parameter words (no save, unlock) -----------------------------------------------------------------------------------------------

// Returns false when the node is not what it should be (nothing written). A parameter that is neither wanted nor held costs nothing.
bool RunParam(ParamDef& p, bool want) {
  if (!want && !p.hold.held) return true;
  uint32_t current = 0;
  const bool ok = p.verified ? mcla::guest::U32(p.node + 4, &current) : ParamNodeOk(p, &current);
  if (!ok) {
    p.verified = false;
    if (p.hold.held) {
      REXLOG_WARN("[mcla-cheat] {}: the parameter node 0x{:08X} cannot be read now: dropped the hold without writing", p.name, p.node);
      p.hold = {};
    }
    return false;
  }
  if (!p.verified) {
    p.verified = true;  // the name matched once: the node is where it was found
    mcla::guest::U32(p.node, &p.name_ptr);
  }
  const math::Step32 step = math::StepParam(p.hold, want, current, math::ParamArgument(p.name_ptr));
  if (step.write && !WriteWord(p.node + 4, step.value)) return false;
  switch (step.act) {
    case math::Act::kTake:
      if (!step.write) REXLOG_INFO("[mcla-cheat] {}: the game already had the parameter on: nothing written", p.name);
      break;
    case math::Act::kRewrite:
      REXLOG_INFO("[mcla-cheat] {}: the game cleared the parameter: put back", p.name);
      break;
    case math::Act::kRelease:
      REXLOG_INFO("[mcla-cheat] {}: released: the parameter word 0x{:08X} back to {}", p.name, p.node + 4, step.value);
      break;
    case math::Act::kReleaseLeft:
      REXLOG_INFO("[mcla-cheat] {}: released: the parameter left as the game has it", p.name);
      break;
    default:
      break;
  }
  return true;
}

void RunNoSave(Cheats& c, bool raw, bool gate, Screen screen) {
  const Which w = Which::kNoSave;
  if (!raw && !c.nosave.hold.held) {
    Report(c, w, State::kOff, Why::kNone, screen, "");
    return;
  }
  // Held on every screen: the saves happen in menus, between loads and in the garage, which is exactly where it has to hold.
  const bool want = raw && gate;
  const bool ok = RunParam(c.nosave, want);
  if (!raw) {
    Report(c, w, State::kOff, Why::kNone, screen, "");
  } else if (!gate) {
    Report(c, w, State::kRefused, Why::kGateOff, screen, "locked: \"Allow cheats and mods\" is off");
  } else if (!ok) {
    Report(c, w, State::kRefused, Why::kBadParam, screen, "the game's `nosave` parameter is not where it was found");
  } else {
    Report(c, w, State::kOn, Why::kNone, screen, "the game's `nosave` parameter is set: its save manager skips every save");
  }
}

void RunUnlock(Cheats& c, bool raw, bool gate, Screen screen) {
  const Which w = Which::kUnlock;
  bool any_held = false;
  for (const ParamDef& p : c.unlock) any_held |= p.hold.held;
  if (!raw && !any_held) {
    Report(c, w, State::kOff, Why::kNone, screen, "");
    return;
  }
  const bool career = math::CareerScreen(screen);
  const bool want = raw && gate && career;
  bool all_ok = true;
  for (ParamDef& p : c.unlock) all_ok &= RunParam(p, want);
  if (!raw) {
    Report(c, w, State::kOff, Why::kNone, screen, "");
  } else if (!gate) {
    Report(c, w, State::kRefused, Why::kGateOff, screen, "locked: \"Allow cheats and mods\" is off");
  } else if (!all_ok) {
    Report(c, w, State::kRefused, Why::kBadParam, screen, "an unlock parameter is not where it was found");
  } else if (!career) {
    Report(c, w, State::kWaiting, Why::kNotCareer, screen, "");
  } else {
    Report(c, w, State::kOn, Why::kNone, screen, "races, parts, performance upgrades and special abilities answer 'unlocked'");
  }
}

// ---- money ------------------------------------------------------------------------------------------------------------------------

void SetMessage(std::string message) {
  std::lock_guard<std::mutex> lock(g_msg_mutex);
  g_money_message = std::move(message);
}

void RunMoney(int32_t request, bool gate, Screen screen) {
  // Consumed first: whatever happens below, a press acts once and a refused one does not come back on a later screen.
  rex::cvar::SetFlagByName("mcla_cheat_money_request", "0");
  const math::MoneyOp op = math::MoneyOpOf(request);
  auto refuse = [&](const std::string& why) {
    REXLOG_WARN("[mcla-cheat] money: {} REFUSED: {}", math::MoneyOpName(op), why);
    SetMessage("refused: " + why);
  };
  if (op == math::MoneyOp::kNone) return;
  if (!gate) return refuse("the cheats are locked (\"Allow cheats and mods\" is off)");
  if (!math::CareerScreen(screen)) {
    return refuse(std::string("the game is on the '") + mcla::gamestate::ScreenName(screen) +
                  "' screen; use the button in a drive, the pause menu, the garage or the map");
  }
  uint32_t manager = 0;
  int32_t current = 0;
  const char* why = nullptr;
  if (!ReadMoney(&manager, &current, &why)) return refuse(why);
  const int64_t amount = REXCVAR_GET(mcla_cheat_money_amount);
  const math::MoneyResult r = math::MoneyApply(current, op, amount);
  if (!r.ok) return refuse(r.why);
  if (!WriteWord(manager + kMoneyOffset, uint32_t(r.value))) return refuse("the money field cannot be written");
  uint32_t check = 0;
  if (!mcla::guest::U32(manager + kMoneyOffset, &check) || int32_t(check) != r.value) {
    return refuse("the money field did not take the new value");
  }
  g_money_seen.store(r.value, std::memory_order_relaxed);
  g_money_seen_ok.store(true, std::memory_order_relaxed);
  REXLOG_INFO("[mcla-cheat] money: {} {}: {} -> {} (experience manager 0x{:08X}, field +0x1FC). It is saved with the profile the next time "
              "the game saves.",
              math::MoneyOpName(op), amount, current, r.value, manager);
  const char* verb = op == math::MoneyOp::kGive ? "gave $" : op == math::MoneyOp::kTake ? "took $" : "set the money to $";
  SetMessage(std::string(verb) + Commas(amount) + " (" + Commas(current) + " -> " + Commas(r.value) +
             "). The HUD and the garage may show it after the next screen change.");
}

}  // namespace

// ---- the frame ----------------------------------------------------------------------------------------------------------------------

void OnFrame() {
  Cheats& c = Cs();
  const bool gate = REXCVAR_GET(mcla_overlay_cheats);
  const bool raw[kN] = {REXCVAR_GET(mcla_cheat_nitro),
                        REXCVAR_GET(mcla_cheat_special),
                        REXCVAR_GET(mcla_cheat_nodamage),
                        REXCVAR_GET(mcla_cheat_nocops),
                        math::FactorIsOn(REXCVAR_GET(mcla_cheat_timescale)),
                        REXCVAR_GET(mcla_cheat_nosave),
                        REXCVAR_GET(mcla_cheat_unlock)};
  const int32_t request = REXCVAR_GET(mcla_cheat_money_request);

  bool busy = request != 0 || c.scale.active || c.nosave.hold.held;
  for (int i = 0; i < kN; ++i) busy |= raw[i];
  for (const FlagDef& f : c.flags) busy |= f.hold.held;
  for (const ParamDef& p : c.unlock) busy |= p.hold.held;
  if (!busy) {  // the usual frame: every cheat off, nothing held
    if (!c.idle) {
      for (int i = 0; i < kN; ++i) Publish(Which(i), State::kOff, Why::kNone);
      c.idle = true;
    }
    return;
  }
  c.idle = false;

  const Screen screen = mcla::gamestate::CurrentScreen();
  g_screen.store(uint8_t(screen), std::memory_order_relaxed);

  for (FlagDef& f : c.flags) RunFlag(c, f, raw[int(f.which)], gate, screen);
  RunTimeScale(c, raw[int(Which::kTimeScale)], gate, screen);
  RunNoSave(c, raw[int(Which::kNoSave)], gate, screen);
  RunUnlock(c, raw[int(Which::kUnlock)], gate, screen);
  if (request != 0) RunMoney(request, gate, screen);
}

State StateOf(Which which) {
  const int i = int(which);
  return i >= 0 && i < kN ? State(g_state[i].load(std::memory_order_relaxed)) : State::kOff;
}

std::string StateText(Which which) {
  const int i = int(which);
  if (i < 0 || i >= kN) return {};
  const State s = State(g_state[i].load(std::memory_order_relaxed));
  const Why why = Why(g_why[i].load(std::memory_order_relaxed));
  const Screen screen = Screen(g_screen.load(std::memory_order_relaxed));
  switch (s) {
    case State::kOff:
      return "off";
    case State::kOn:
      switch (which) {
        case Which::kTimeScale: return "ON: the game's time scale is multiplied";
        case Which::kNoSave: return "ON: the game skips every save";
        case Which::kUnlock: return "ON: the game's own unlock checks say yes";
        default: return "ON: holding the game's own cheat switch";
      }
    case State::kWaiting:
      return std::string("waiting for ") + NeedText(which) + " (the game is on: " + mcla::gamestate::ScreenName(screen) + ")";
    case State::kRefused:
      switch (why) {
        case Why::kGateOff: return "locked: switch on \"Allow cheats and mods\"";
        case Why::kBadFlag: return "refused: the game's flag is not where it was found";
        case Why::kBadParam: return "refused: the game's parameter is not where it was found";
        case Why::kBadTimer: return "refused: the engine timer cannot be read";
        case Why::kWriteFailed: return "refused: the game's memory cannot be written";
        default: return "refused";
      }
  }
  return {};
}

std::string LastMoneyMessage() {
  std::lock_guard<std::mutex> lock(g_msg_mutex);
  return g_money_message;
}

bool CurrentMoney(int32_t* money) {
  uint32_t manager = 0;
  int32_t value = 0;
  const char* why = nullptr;
  if (!ReadMoney(&manager, &value, &why)) return false;
  *money = value;
  return true;
}

}  // namespace mcla::cheat

namespace {

// "[mcla-cheat] armed, off by default (--mcla_overlay_cheats=true switches it on)": written once with the other hooks' lines.
mcla::hooks::Registration g_registration("cheat", "mcla_overlay_cheats", true, [] { return REXCVAR_GET(mcla_overlay_cheats); },
                                         /*default_on=*/false);

}  // namespace
