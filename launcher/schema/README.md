# Launcher settings schema

`settings.json` is the single source of the launcher's settings: the UI is generated from it and so is the game's
command line. The format and the validator's rules are documented at the top of `launcher/src/core/settings_schema.h`;
`LoadSchema` rejects a file that breaks them.

Conventions used by this file:

- A setting's `id` is the cvar it drives (`--<cvar>=<value>`). The only launcher-only setting is `close_on_launch`
  (`cvar: null`, `emit: never`). The launcher hard-codes two ids: `close_on_launch`, and the setting whose cvar is
  `game_data_root` (a `path`, checked before Launch).
- `default` is what the launcher starts with: the "Recommended" preset (1080p, 60 fps, FSR). Where that differs from the
  cvar's own default (or from what `mcla/profile/pc_profile.cpp` applies), the setting uses `emit: always`, so the command
  line fully decides the run. Everything else uses `emit: nondefault` and its default equals the game's effective default
  (the cvar's, or the profile's: `anisotropic_override` 5 and `mnk_mode` true).
- `requires` follows how the game works: FSR needs MSAA off, the single-tile scene and Direct3D 12 (`temporal_aa`), its
  version/quality/sharpness need FSR, the 60 fps timing options need `mcla_fps_target` 60, keyboard bindings need
  `mnk_mode`, the window size needs windowed mode. A hidden setting is never emitted.
- `mcla_profile` values are the presets of `pc_profile.cpp` (`default`, `800p`, `900p16`, `900p`, `1080p`, `1200p`); the FSR
  variants (`1080p-fsr`, `1080p-fsr4p`, ...) are the base size plus `temporal_aa*` cvars, so the upscaler is a separate
  choice. `720p-fsr1` is the shipped size plus `present_effect=fsr`.
- `fullscreen_mode` and `temporal_aa_upscaler` come from SDK patches 0480 and 0470 (`patches/sdk`); both default to the
  patch's default and are only emitted when changed, so a build without those patches ignores them.
- Presets (`presets`) list every setting they own, so applying one is deterministic; a preset that no longer matches the
  current values shows as "Custom".

Adding a setting: confirm the cvar (`REXCVAR_DEFINE_*` in `mcla/`, `src/host/`, the SDK or `patches/sdk`), copy its type,
range and default, and write a one-line `help`. `LoadSchema` (the launcher's unit test) validates the structure; that every
cvar exists with the same type, range and default was checked with a throwaway script (`$WORK/scratch/L0/check_schema.py`).
