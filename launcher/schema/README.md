# Launcher settings schema

`settings.json` is the single source of the launcher's settings: the UI is generated from it and so is the game's
command line. The format and the validator's rules are documented at the top of `launcher/src/core/settings_schema.h`;
`LoadSchema` rejects a file that breaks them.

Conventions used by this file:

- A setting's `id` is the cvar it drives (`--<cvar>=<value>`). The launcher-only settings (`cvar: null`, `emit: never`) are
  `close_on_launch` and `aa_auto` (see Anti-aliasing below). The launcher hard-codes two ids: `close_on_launch`, and the setting whose cvar is
  `game_data_root` (a `path`, checked before Launch).
- `default` is what the launcher starts with: the "Recommended (1080p)" preset (native 1080p, single tile, MSAA off, 60 fps profile, Performance scene detail). Where that differs from the
  cvar's own default (or from what `mcla/profile/pc_profile.cpp` applies), the setting uses `emit: always`, so the command
  line fully decides the run. Everything else uses `emit: nondefault` and its default equals the game's effective default
  (the cvar's, or the profile's: `anisotropic_override` 5, `mnk_mode` true and `d3d12_allow_variable_refresh_rate_and_tearing`
  false: the SDK's default is on, the profile turns it off so a TV or an ordinary monitor does not tear).
- `requires` follows how the game works: FSR needs MSAA off, the single-tile scene and Direct3D 12 (`temporal_aa`), its
  version/quality/sharpness need FSR, the 60 fps timing options need `mcla_fps_target` 60 or 120 (`"requires": {"mcla_fps_target": [60, 120]}`: a list is "any of these values"), keyboard bindings need
  `mnk_mode`, the window size needs windowed mode. A hidden setting is never emitted.
- `mcla_profile` values are the presets of `pc_profile.cpp` (`default`, `800p`, `900p16`, `900p`, `1000p`, `1080p`, `1200p`, `2000p`); the FSR
  variants (`1080p-fsr`, `1080p-fsr4p`, ...) are the base size plus `temporal_aa*` cvars, so the upscaler is a separate
  choice. The FSR version defaults to `fsr3` (never `auto`) and is always emitted; the FSR 4 preset sets `fsr4`, 2x, sharpness 0.2.
  Presets name `mcla_gfx_preset=default` for "as shipped" (an empty value would be an empty `--name=` argument). `720p-fsr1` is the shipped size plus `present_effect=fsr`.
- `fullscreen_mode` and `temporal_aa_upscaler` come from SDK patches 0480 and 0470 (`patches/sdk`); both default to the
  patch's default and are only emitted when changed, so a build without those patches ignores them.
- Anti-aliasing is Auto / Off / FXAA, made of two entries because a setting can only pass or skip the cvar by its value:
  `aa_auto` (launcher-only, default on) leaves `swap_post_effect` off the command line, so the game decides (the Quality and
  Ultra presets set FXAA as an app default, and the governor's tiers 2 to 5 do); with it off, `swap_post_effect` appears
  (`emit: always`: Off = `none`, `fxaa`, `fxaa_extreme`). A value passed on the command line is the player's: the game's tiers
  and presets (`SetDefaultValue`) never override it.
- Adaptive detail (`mcla_gov`, `emit: always` because the code default is off and the profile switches it on) and its ceiling
  (`mcla_gov_tier_ceiling`: 2 Performance, 4 Shipped look, 5 Beyond console; shown only with Adaptive detail on) sit in the
  Performance category and need the 60 or 120 fps target.
- Presets (`presets`) list every setting they own, so applying one is deterministic; a preset that no longer matches the
  current values shows as "Custom".

Adding a setting: confirm the cvar (`REXCVAR_DEFINE_*` in `mcla/`, `src/host/`, the SDK or `patches/sdk`), copy its type,
range and default, and write a one-line `help`. `LoadSchema` (the launcher's unit test) validates the structure; that every
cvar exists with the same type, range and default was checked with a throwaway script (`$WORK/scratch/L0/check_schema.py`).
