# The classic cheats of the mod menu (nitro, special abilities, damage, police, slow motion, money, unlocks, no-save)

Status: **first run done (2026-11-27): 5 rows work, 3 rows are untested, none is known not to work; one defect was found and fixed (money).** The code was
written from the recompiled code and first run on the owner's laptop in three scripted runs (`cht-ck1`..`cht-ck3` in the work folder's `runs/`, each on its own
copy of the `after_intro` fixture; the owner's save was never touched). The per-cheat results are in "Results of the first run" below. Code:
`mcla/hooks/mcla_cheat.cpp` (glue), `mcla_cheat_math.h` (pure logic, unit tested by `mcla/tests/test_cheat.cpp`), the rows are in
`src/host/overlay/overlay_items.h` / `overlay_ui.cpp` (Cheats / mods tab).
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
| Money (`mcla_cheat_money_request` 1 give / 2 take / 3 set, `mcla_cheat_money_amount`) | one 32-bit field, a button, once | `[[0x8287484C] + 0x1FC]` | `GetMoney` `sub_826B9820`, `SpendMoney` `sub_826B98A0`, `CanSpend` `sub_826B98F0`, `AddMoney` `sub_826B93E0` (limits +/- 2,140,000,000); the constructor `sub_826BA578` sets 5000 (seen live: 5000 at the front end, the profile's amount from the load on); the object is checked by its FINAL vtable `0x82039058` (`sub_822BFE30` runs the base constructor, which sets `0x820BBDC4`, then stores the derived class's vtable) | **B** | **yes** |
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

## Results of the first run (2026-11-27)

Method: `python tools/run_game.py --flow <flow> --run-id cht-ckN ...` with the cheats set by schedule steps inside the flow (`<s>:set <cvar>=<value>`), screenshots every
2 s, and a read-only probe next to the game (`ReadProcessMemory` on `mcla.exe`; it finds the 4 GiB guest mapping by testing candidate bases against a known pointer,
`[0x8286D874]` -> "nosave"; base `0x100000000` here) that logs, four times a second, the cheat bytes `0x8288E5A7..BE`, the timer word, the career money, the
`nosave` / `unlock*` words, every nitro object (vtable `0x82046F90`, bars = s16 at `+18`, burning = byte at `+32`) and the run's own save file. The probe and the flows
are throwaway files in `scratch/cheats/tools/` of the work folder. Runs: `cht-ck1` (158 s, windowed), `cht-ck2` (172 s) and `cht-ck3` (168 s) fullscreen; `cht-ck4` was
stopped at T=35 s on the owner's request (no cheat was set in it).

### Works (tested in the running game)

| Row | Evidence |
|---|---|
| **Infinite nitro** | `cht-ck1`: control (T=55, 60) the player's nitro object goes 2 -> 1 -> 0 bars; with the cheat on (T=72..104) six taps at 0 bars each fire it (`burning` = 1, timer and charge running) and the bars stay 0, while the two control taps at 0 bars (T=65, 70) did nothing. `cht-ck2` (cheat first): with 2 bars, four taps (T=55, 60, 65, 70) fire it and the bars stay 2; cheat off at T=73, the next two taps take it to 1 (T=77) and 0 (T=83). Log: `nitro: ON (unlimited-nitrous byte 0x8288E5B0)`, `released: ... back to 0`. The owner watched it fire with an empty bar. |
| **Infinite special abilities** | `cht-ck3` with a control: LB+RB at T=112 with the cheat OFF does nothing (the ability ring on the HUD stays dark in `t112`, `t114`); LB+RB at T=118 with it ON lights the ring and the screen shows the ability's effect, the ring drains over T=120..124 (shots `t118`..`t124`). `cht-ck1`: the same at T=115..121. The owner watched the ability fire with an empty meter. The button is LB+RB (a bumper alone changes the radio). |
| **No police** | `cht-ck2`: cheat off until T=83.4: "POLICE RESPONDING" panel and cop markers on the minimap at T=66..74 and T=82..84 (shots `t68`, `t74`, `t82`, `t84`); `no police: ON (no-cops byte 0x8288E5B4)` at 83.4: the panel is gone by T=86 and no pursuit appears again until the end of the drive (T=172, 86 s of full-throttle driving through traffic). Weak part: one run (a pursuit starts in about one run in two), and `cht-ck1` (cheat off) had none at all in 158 s; the release of a pursuit already running at the moment of switching on is the stronger evidence. |
| **Slow motion** | `cht-ck2`: x0.50 for T=100..112, x0.25 for T=112..124, then 1: the probe reads the timer word 0.5000 / 0.2500 / 1.0000 in every sample; `[mcla-sim] ... guest total` (the game's own clock per 5 s of real time) reads +2.508 s at x0.50, +1.254 s at x0.25 and +5.001 s at 1.0; frames 300 per 5 s all the time, p50 17 ms, p99 18 ms, worst under 19 ms; no `audio underrun` line. **Note: the `world ... (x1.0000)` figure of the same line does NOT show the factor** (it counts the step the sim hook issues, before the timer's scale); read `guest total`. |
| **Money** (give / take / set) | The row first REFUSED (below). After the fix, `cht-ck2`: `money: give 100000: 120952 -> 220952`, `take 25000: 220952 -> 195952`, `set 77777: 195952 -> 77777`, `set 1234: 77777 -> 1234` (experience manager `0xBDD90200`, field `+0x1FC`); the probe reads the field 120952, 220952, 195952, 77777, 1234 at those moments, and the field was 5000 at the front end and 120952 from the profile load on (T=30.6), which is what the career's money field does. The tab shows "The career's money now: $1,234" and the last press ("set the money to $1,234 (77,777 -> 1,234)"). `cht-ck3`: `set 424242: 120952 -> 424242` in a parked free roam. **Not seen:** the game's own screens (the pause menu has no cash line; the Sidekick, the GPS map and the garage were not looked at). |

Defect found and fixed (the only one): **the money row would always have refused.** The code checked the experience manager's vtable against `0x820BBDC4`, which
is the vtable the BASE constructor `sub_826BA578` stores; `sub_822BFE30` then overwrites it with the derived class's `0x82039058` (and `+12` with `0x8203904C`), and
the live object at `[0x8287484C]` has `0x82039058` and the name "ExperienceManager" at `+16`; no object with `0x820BBDC4` exists. `kExpManagerVtable` is now
`0x82039058` (`mcla_cheat.cpp`); the write, the read-back and the log line were then seen working. Cosmetic: the release lines say `damage:` / `special:` / `cops:`
where the state lines say `no damage:` / `special abilities:` / `no police:` (left as is).

### Untested (kept available; the row says "untested")

| Row | What was seen, what was not |
|---|---|
| **No damage** | Seen: the byte `0x8288E5A7` is held (1) while a drive is on screen and returned to 0 (`cht-ck1` T=72.35 -> 108.15, `cht-ck2` T=125.25 -> 166.8). Not seen: a damaging crash that the cheat prevented. The damage meter is the outer red arc round the speedometer (large in the 10 min soak run `rc1-soak1` from T=270, absent at T=120); in this flow it does not show up within 70 s: none in `cht-ck1` at all, even in the control (T=50..72); in `cht-ck2` an arc appeared at T=98..100 with the cheat off and looked the same size at T=126, 140, 166, 170 with it on, but the bike hit little after T=125 and the pad stopped at T=147, so it proves nothing; in `cht-ck3` (cheat on T=60..100, off after) the bike was wedged at 0 mph from T=92, so the comparison is void. To test it: a flow that crashes at speed again and again, with the cheat on first and off as the control. The HUD code reads two floats that could be numbers for it (`[[car+668]+36]` at `0x82211920`, `[[player+16]+204]` at `0x82211820`); not checked. |
| **Unlock races, parts, upgrades, abilities** | Seen: the four parameter words are found (the node names match), set to the node's own name pointer (`0x820BD6C8`, `0x820BD6E8`, `0x820BD6D4`, `0x820BD6F4`), held through the pause menu and put back to 0 (`cht-ck2` T=96 -> 166.3); the game did not misbehave. Not seen: an unlocked race, part or upgrade (the garage and the map were not reached). |
| **Don't write saves this session** | Seen: the `nosave` word `0x8286D878` set to its name pointer `0x8201C710` (`cht-ck2` T=47.1), held on every screen including the pause menu, put back to 0 at T=166.6; no save was written in the run (the probe watched the run's copy). Not seen: a save the cheat prevented, because the run had no save action. What saves in this flow: the `a/200` presses of `quick_drive2` close an arrest / damaged-out summary and the game saves about 0.4 s later (`[mcla-state] HudPlayerList/HPL_Hidden`, then `content: saved game 'mc4.sav' is being rewritten`; runs `rc1-ctl-warm`, `rc1-ctl-warm3`, `h2-g0`); `cht-ck1..3` had no such summary (the A presses did nothing), so there was nothing to prevent. To test it: a run that gets arrested, with and without the cheat. |

### Not working

None found.

### Also checked (all rows)

* **Gate off:** with `mcla_overlay_cheats` off, setting nitro, no damage, no police and slow motion (`cht-ck1` T=41..43) logs `REFUSED: locked: "Allow cheats and mods" is off` for each, the probe's bytes stay 0 and the timer word 1.0.
* **Engages only on a drive screen, gives the game's value back:** pause with three cheats on (`cht-ck1` T=129: all released within one frame, `waiting for a drive (the screen is 'pause')`, held again 0.4 s later when the pause closed; `cht-ck2` T=148.5 -> 154.3); switching off writes the byte back (`released: ... back to 0`); locking the gate with the cheats still set releases them within a frame (`cht-ck1` T=143.2) and opening it again re-holds them (T=147.2). Not tried: leaving the drive by a load.
* **The Cheats / mods tab** renders every row with its state text: `cht-ck1` `shots/t138.png` (top: gate, nitro, special, no damage, no police "ON: holding the game's own cheat switch") and `cht-ck2` `shots/t162.png` (bottom, scrolled with the pad: slow motion, the money amount, "The career's money now: $1,234", Give / Take / Set, the last-press message, the unlock row "ON: the game's own unlock checks say yes", the no-save row "ON: the game skips every save", the save warning with the folder, "Switch every cheat off").
* **Stability:** no crash, `fatal=0`, the heartbeat counted to the end of every run; no new error / warning line against the control run `rc1-ctl-warm3` apart from the expected cheat REFUSED lines and, in `cht-ck2` and `cht-ck3`, one GPU warning (`Texture fetch constant ... has "invalid" type`) that also appears in cheat-free runs (`rc1-soak1`, `rc1-ctl-warm2`, `h2-g0`, `p0-base`); no `audio underrun`; `[mcla-sim] frame times` p50 17 / p99 18 ms, worst under 19 ms, identical with every cheat off (T=44..69), with them on and with the menu open (`cht-ck1`, `cht-ck2`).

## Leads (grade C: needs a look in a running game)

* **Reputation points**: the experience manager (`[0x8287484C]`, class `mcExperienceManager`, final vtable `0x82039058`) holds an array of
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

## Re-running the checks

See the end of `docs/OVERLAY.md` ("The classic cheats") and the log lines to expect: `[mcla-cheat] ...` (tag `cheat`). The scripted flows and the read-only probe of the first run are in the work folder's `scratch/cheats/tools/` (`guest_probe.py`, `flows/ck*.txt`).
