// Midnight Club: Los Angeles: the classic cheats (mcla_cheat.cpp): what the mod menu's Cheats tab and the game thread share.
//
// Not a hook (no codegen entry, no MCLA_HOOK_ flag): always compiled, called once per engine frame from the heartbeat hook
// (mcla_heartbeat.cpp), on the game thread, before the engine timer reads its clock. The cvars (mcla_cheat_*) are transient and
// every one of them does nothing unless mcla_overlay_cheats is on. Evidence, addresses and the risk to a save: docs/CHEATS.md.
#pragma once

#include <cstdint>
#include <string>

namespace mcla::cheat {

// Once per engine frame, game thread. One cvar read per cheat and nothing else while every cheat is off and none is held.
void OnFrame();

// The cheats the menu shows a state for.
enum class Which : uint8_t { kNitro, kSpecial, kDamage, kCops, kTimeScale, kNoSave, kUnlock, kCount };

// What a cheat is doing, for the menu (any thread; one atomic load).
enum class State : uint8_t {
  kOff,       // the cvar is off (or the gate is)
  kWaiting,   // on, but not acting now: the screen is not one the cheat may act on
  kOn,        // acting: the game's own value is held
  kRefused,   // on, but the guest memory is not what the cheat expects: nothing was written
};
State StateOf(Which which);
// One short phrase for a state ("holding the game's flag", "waiting for a drive (screen: pause)", "refused: ..."), for the menu.
std::string StateText(Which which);

// The last money press, for the menu: "gave $100,000 (5,000 -> 105,000)" or "refused: ...". Empty before the first press.
std::string LastMoneyMessage();
// The career's money as of the last read (the menu shows it next to the buttons). false when it cannot be read now.
bool CurrentMoney(int32_t* money);

}  // namespace mcla::cheat
