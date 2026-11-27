# The classic cheats of the mod menu (nitro, special abilities, damage, police, slow motion, money, unlocks, no-save)

Status: **written from the recompiled code, not run.** Nothing was built or started while this was made (the owner was playing). The
first run is the test: the recipe is at the end. Code: `mcla/hooks/mcla_cheat.cpp` (glue), `mcla_cheat_math.h` (pure logic, unit
tested by `mcla/tests/test_cheat.cpp`), the rows are in `src/host/overlay/overlay_items.h` / `overlay_ui.cpp` (Cheats / mods tab).
Only addresses, offsets and names are written down here; no game data.

## The idea: the game already has these cheats

Midnight Club LA ships a cheat system of its own, and most of the work was finding it. There are two layers:

* **Command-line parameters** (RAGE `sysParam`): a node in `.data` per parameter, registered by one small thunk each through
  `sub_821C06C8(node, 0, name, 0)`. Node layout: `+0` name pointer, `+4` the value word (0 = not given; the game only tests it for
  non-zero), `+8` 0, `+12` next node, `+16` byte. 540 of them are registered (`noaa`, `nosave`, `unlockraces`, `nodamage`, ...).
* **Runtime cheat bytes**: `sub_82388340` (the init) copies a few parameters into a block of bytes at `0x8288E5A7..0x8288E5BE`, and the
  pause menu's Cheats page (`sub_8264B2B0` toggles, `sub_8264B3A0` builds the page; its six items appear as a value read through the
  object at `[0x828CCF68]` reaches 10, 20, ... 60: not decoded) flips the same bytes. `sub_82387B38` clears them all (called from `sub_822811C8`, `sub_8264B1D0`, `sub_8265D770`, `sub_82643188`).

The cheats here do not patch code. They write the game's own byte or word, so the game's own readers do the work, and they give the
game's value back when the cheat is switched off.

## The cheats

| Row (cvar) | Mechanism | Written | Read by (the evidence) | Grade | In a save? |
|---|---|---|---|---|---|
| Infinite nitro (`mcla_cheat_nitro`) | cheat byte, held at 1 while driving | `0x8288E5B0` | `sub_82345698` (can the nitro fire: bars not required), `sub_82345820` (fire: `bars--` skipped), `sub_82345988` (the empty-bar feedback skipped) | **A** | no |
| Infinite special abilities (`mcla_cheat_special`) | cheat byte | `0x8288E5B1` | `sub_82338E00`: the activation (type `[this+52]` 0..3 = Zone, Agro, Roar, EMP) forces the charge count `[this+8]` to at least 1 before it spends one | **B** | no |
| No damage (`mcla_cheat_nodamage`) | cheat byte | `0x8288E5A7` | `sub_82331970` (the damage amount is not applied), `sub_82342900`, `sub_82343178`, `sub_82343880`, `sub_82356D98` | **A** | no |
| No police (`mcla_cheat_nocops`) | cheat byte | `0x8288E5B4` | `sub_82707208` (the police state machine returns at once), `sub_82704228` (spawn a cop: refused), `sub_82700088`, `sub_82708F30` (puts the police manager into its off state: `sub_8270C408(this, 1)`), `sub_82709AD0` (releases each cop at once: `sub_82706998(.., 1)`) | **A** | no |
| Slow motion (`mcla_cheat_timescale`) | the engine timer's scale float x factor | `0x827D7554` (timer `+0x54`) | `sub_821BDA90` (`rage::sysTimeManager::Update`): dt = step x `[+0x54]`; the game stores its own scale there every frame (`sub_822C1FA8` at `0x822C2124`; a wipe-out's slow motion goes the same way) | **B** | no |
| Money (`mcla_cheat_money_request` 1 give / 2 take / 3 set, `mcla_cheat_money_amount`) | one 32-bit field, a button, once | `[[0x8287484C] + 0x1FC]` | `GetMoney` `sub_826B9820`, `SpendMoney` `sub_826B98A0`, `CanSpend` `sub_826B98F0`, `AddMoney` `sub_826B93E0` (limits +/- 2,140,000,000); the constructor `sub_826BA578` sets 5000; the object is checked by its vtable `0x820BBDC4` | **B** | **yes** |
| Unlock races, parts, upgrades, abilities (`mcla_cheat_unlock`) | four parameter words, held in a career screen | `0x82909518+4` `unlockraces`, `0x829094B4+4` `unlockparts`, `0x829094DC+4` `unlockperformance`, `0x82909504+4` `unlockspecialabilities` | the four "is it unlocked" predicates `sub_826BDA38`, `sub_826BCDD8`, `sub_826BCD90` (called from the garage code `sub_826AB520`, `sub_8269E888`), `sub_826BCE48`: each answers 1 when its word is non-zero, otherwise looks in the career's flag arrays | **B** | **what you do with them** |
| Don't write saves (`mcla_cheat_nosave`) | one parameter word, held on every screen | `0x8286D874+4` (`nosave`) | `sub_82200A30` (the save manager's `SaveData`: with the word set it posts an event and returns without saving), `sub_821FEFA0`, `sub_822016B0` | **B** | protects |

Grades: **A** = the code path is unambiguous and a second source agrees (the game's own pause-menu cheat page and string table, Renan's
HUD reading of the nitro object). **B** = one consistent reading, nothing contradicts it, not yet seen working. Nothing here is C: the C
leads are at the end.

### The nitro object (for the A)

`sub_82345540` is its constructor: `+0` vtable, `+12` s16 -1, `+16` s16 100, `+18` **s16 bars**, `+20/+24/+28` floats (burn target, burn
timer, charge), `+32` byte "burning", `+33` byte "unlimited" (copied from the capital-U `UnlimitedNitrous` parameter node `0x8288DCC8`, a
per-object alternative that is not used here). Renan Queiroz's HUD hook (`0x822117C0`, `[[car+664]]+18/+24/+28`) reads the same
fields: that is the second source. `sub_82345698`, `sub_82345820` and `sub_82345988` are the three places that spend or require a bar
and each one tests the global byte first.

### What the game does about a cheating drive

`sub_82387A98` is `IsCheating`: 1 when any of the nitro, special, damage (under a condition), no-cops, no-charges bytes (or `0x8288E5B5`) is set. Its
callers are the reward and statistics paths: `sub_826B96C8` (`CalculateRaceMoney`) returns 0 when it is true, `sub_826BB9E0` (the
experience manager's event handler) ignores every event, and the pause menu (`sub_8264C180`) shows its own "cheat enabled" dialog.
**While a drive-time cheat here is on, that drive pays no reputation and no money. This is the game's own design and is kept.** (Making
the game pay anyway would take an override of `sub_82387A98`; it is not done on purpose.) Money, unlocks and what is done with them are
not blocked by it; they are the rows that can reach a save.

### Which screens, and why

* The five drive cheats act only while `mcla::gamestate::CurrentScreen()` is free roam or a race, once per engine frame from the
  heartbeat hook (the first instruction of the timer's update, game thread). On any other screen the game's own value is written back
  (only if the byte still holds what the cheat wrote: a byte the game, or the game's cheat page, changed itself is left alone).
* Unlocks: a drive, the pause menu, the garage, the map (that is where the predicates are read). Not in a load, the front end, a cutscene.
* Money: the same screens, **once per button press**, never automatic; the press is consumed first (a refused press does not come back
  on a later screen); old and new amounts are in the log.
* No-save: every screen, because the saves happen in menus and between loads. It is one word and acts on nothing by itself.
* Nothing at all unless `mcla_overlay_cheats` (the gate) is on; locking the gate resets every cheat cvar and the next frame writes the
  game's values back. Every cheat cvar is `transient` and `mcla_cheat_` is on the overlay's never-saved list. `--mcla_heartbeat=false`
  also switches the cheats off (they run from that hook).

### The time scale

`sub_822C1FA8` (the per-frame function) stores a scale into the timer (`0x827D7554`) before each update, from a global at `0x827E1C34`
(1.0 normally; the timing notes in `mcla_t_sim_step.cpp` say a pause and a slow motion go through the same scale, and a wipe-out's slow motion is the game's own use of it). The cheat runs from the update's first instruction, multiplies the
word by the factor (0.05 to 1) and remembers what it wrote, so a frame the game did not refresh is not multiplied twice (unit tested)
and a pause (0) stays a pause. The `mcla_sim60` step hooks come before the multiplication, so slow motion works with the 60 fps
timing. Not looked at: the race clock and the audio. A replay recording stores the scaled value.

### The risk to a save

* Nitro, special, damage, police, slow motion: the bytes and the float are never saved. A crash cannot leave them anywhere.
* **Money** is the career's own field and is written to the profile at the next save. Old and new amounts are in the log.
* **Unlocks** only make the game's checks answer yes; the arrays in the save are not rewritten. What you buy, fit or race with an
  unlocked item is saved like any other action.
* **No-save** is the protection: with it on the game's save manager skips every save. The game may still say it saved.
* Where the saves are: `<user data>\<xuid>\545407F8` (the user data folder is `Documents\mcla_x86` unless the launcher says otherwise).
  Back that folder up before using money or unlocks.

## Leads (grade C: needs a look in a running game)

* **Reputation points**: the experience manager (`[0x8287484C]`, class `mcExperienceManager`, vtable `0x820BBDC4`) holds an array of
  experience systems at `+0x30` (count `+0x34`, 28-byte elements built by `sub_826BBE70`); rewards go through a pending list at `+0xEC`
  and are granted at the end-of-race screen. The progress looks like a normalized float per system (`sub_826B9048` maps it through a
  level curve). To find it: read the floats of those elements before and after a race win.
* **Unlock cars**: there is no unlock switch for cars (`unlockvehicles` only sets a byte nobody reads). Showroom availability is
  `RMR_NotForPurchase` / level gated; the money row buys what is on sale.
* **Freeze traffic / opponents**: `0x8282A7C8` is the traffic manager's master byte (`sub_826D5D90` writes it; `sub_826F6920`, the
  traffic update, returns when it is 0). Writing 0 probably freezes the traffic where it stands; the effect on cars already on the road is
  not known. `noautomatictraffic` (`0x828CC980+4`) and `notriggeredtraffic` are the neighbouring switches. `noai`, `notraffic` have no
  reader in this build.
* **Instant escape / end a pursuit**: needs a call into the police code (`sub_827016D8`, `sub_826D5D90` are named in `PORT_STATE.md`);
  there is no guest-call facility. The No police row stops new pursuits.
* **Other cheat bytes of the game's own page**: `0x8288E5B2` top-down race camera (read by `sub_8223D450`, `sub_822B35E8`), `0x8288E5B6`
  "no charges" (`sub_8269EA88`, `sub_82702420`).

## How it was found (for the next one)

A pass of the decrypted image's strings (`cheat_Unlimited_Nitro`, `unlimitednitrous`, `TurnUnlimitedNitrousOn`, `mcExperienceManager_*`
...), a cross-reference index of every `lis`+`addi`/load/store address in the generated code, then reading the consumers. The tools are
throwaway scripts in the work folder (`scratch/cheats/tools`), not in the repo.

## First-run recipe

See the end of `docs/OVERLAY.md` ("The classic cheats") and the log lines to expect: `[mcla-cheat] ...` (tag `cheat`).
