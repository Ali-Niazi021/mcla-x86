# Midnight Club: Los Angeles (Xbox 360): the game layer's build contract (PLAN 3.3, 4.1, W1.3).
#
# include()d by the root CMakeLists.txt (W1.2); defines no targets and uses only CMAKE_CURRENT_LIST_DIR, so the
# root can include it from any directory. Sets:
#
#   MCLA_HOOK_SOURCES       absolute paths of every hook and profile translation unit (hooks/*.cpp, profile/*.cpp).
#                           The root compiles them INTO the executable as objects, not through a static archive: the
#                           hook functions are strong definitions that must win over the generated code's weak
#                           defaults, and an archive member that nothing references would never be pulled in.
#   MCLA_HOOK_DEFINITIONS   MCLA_HOOK_<NAME>=1 for every hook switch (target_compile_definitions of the exe).
#   MCLA_HOOK_INCLUDE_DIRS  hooks/ (nfsc_progress.h, frame_clock_policy.h, mcla_*.h) and profile/ (pc_profile.h,
#                           game_profile.h). nfsmw_pch.h comes from the generated directory, which the root adds.
#
# Every hook file keeps its function when its flag is 0 (an empty body, or `return false`), so the
# [[midasm_hook]] declarations of mcla/codegen/hooks*.toml link either way; switching a hook off at build time
# never needs a codegen run. All flags are 1 here. At run time each hook has a cvar, read on every call, with the
# defaults of the iOS port: the fixes, the CPU levers, the skip-intro and the logging hooks are on (mcla_state_trace
# and mcla_heartbeat log by default); the hooks that change what the game does are off, so a run with no cvar given
# plays as shipped: mcla_sim60 (60 fps timing), mcla_widescreen_width, the mcla_gfx_* knobs, and the two A/B render
# switches mcla_noaa and mcla_fast_clear_off.
#
# The hooks, by file (docs/mcla/HOOKS_SPEC.md in the iOS repo, `[mcla-<tag>] active` lines in the log):
#   core      HEARTBEAT 0x821BDA90, RAGE_FATAL 0x821BD618, READ_RETRY_FATAL 0x821C4FF0 + 0x821C50C8,
#             UI_LIGHTS_GUARD 0x82725100, SKIP_INTRO 0x822C2F48 + 0x821315E4, FENCE_SPIN 0x82412FB0,
#             STATE_TRACE 0x8268DDD8, FAST_CLEAR_OFF 0x82418734 (A/B, off), NOAA 0x822E4B80 (A/B, off)
#   widescreen WS_SCENE_SIZE, WS_RT_POOL, WS_RT_GUARD, WS_EDRAM, WS_EDRAM_LIMIT, WS_SINGLE_TILE (A/B, off), WS_PHYS_POOL,
#             WS_TALL_FOV (Vert+ on a buffer taller than 16:9), WS_ASPECT, WS_CUTSCENE_FOV, WS_MAP_ASPECT, WS_UI (probes
#             only), WS_TEXCLEAR; one master cvar, mcla_widescreen_width (0 = off = as shipped), plus mcla_scene_height
#   menus     MENU_MSAA 0x822E6008 + 0x822E5428 (A/B, off): the scene passes the game forces to 4x MSAA (front-end world, pause-menu
#             backdrop) are drawn in the renderer's own MSAA mode instead; cvar mcla_menu_noaa (default off, read on every pass)
#   timing    SIM_FRAME_DELTA, SIM_STEP, SIM_CAMERA_POS, SIM_CAMERA_LOOKAT, SIM_CHASSIS, SIM_UI_ADVANCE,
#             SIM_UI_COUNTDOWN, SIM_RACE_CLOCK, SIM_PRESENT_INTERVAL; one master cvar, mcla_sim60 (off = as shipped)
#             SIM_PRESENT_IMMEDIATE 0x82419AC0 + census 0x82419920 (W4.5): the guest's present-immediate threshold
#             on timed swaps, cvar mcla_present_immediate (0 = as shipped, -1 = auto; mcla_sim60 on)
#   cpu       CPU_POLL_WAIT 0x82148AB0, CPU_POLL_RELEASE 0x82160140, CPU_PUMP 0x821449C8
#   gfx       GFX_CULL_MASK, GFX_SHADOW_MODE, GFX_DL_ENABLE, GFX_TRAFFIC_LOD, GFX_AMBIENT_ZONE (the mcla_gfx_* knobs), plus
#             GFX_PHASES (the phase skip inside the sub_822E6408 override of mcla_fsr_markers.cpp, so it needs FSR_MARKERS
#             too: mcla_gfx_env_interval / mcla_gfx_reflection_interval), GFX_SHADOW_RES (mid-asm hook 0x825F69A0, cvar
#             mcla_gfx_shadow_res; its codegen entry goes in hooks_gfx.toml), GFX_ENV_RES (mid-asm hook 0x8233AB3C, cvar
#             mcla_gfx_env_res: the environment map's size; codegen entry in hooks_gfx.toml) and GFX_IMPOSTORS (function overrides of
#             sub_8230B778 / sub_8230D988, cvar mcla_gfx_impostors, no codegen entry); mcla_gfx_preset (perf, perf+, lowest)
#             is a bundle of these and the older knobs; GFX_FAR_CASCADES (R4: far shadow cascades on alternate frames: the schedule in
#             PhasesToSkip, overrides of sub_825F61F0 / sub_825F4EF8; cvars mcla_gfx_far_cascades_interval / mcla_gfx_shadow_private;
#             needs GFX_PHASES + FSR_MARKERS) and GFX_IMPOSTOR_THROTTLE (mid-asm hook 0x8230DE50, cvar mcla_gfx_impostor_interval; its
#             codegen entry goes in hooks_gfx.toml)
#   fsr       FSR_MARKERS (W5.6): function overrides of sub_822C0980 / sub_822E6408 / sub_822E55D0 that write the guest
#             render markers (PM4 NOP 'REXM') the SDK's FSR 3.1 temporal AA / upscaling needs; no codegen entry (link-time
#             override of the generated weak functions); cvar mcla_render_markers (-1 = auto: on while temporal_aa=fsr, or
#             tonemap_transfer_skip is on, or the SDK's `hdr` is not off: its HDR scene / UI-layer passes need the markers)
#   hdr       HDR (host side only, no guest code touched): the in-run A/B switcher of the HDR measurements, a background
#             thread that cycles groups of cvar assignments on a clock (cvars mcla_hdr_ab, mcla_hdr_ab_period_s,
#             mcla_hdr_ab_start_s; empty mcla_hdr_ab = does nothing; tools/hdr/hdr_ab.py reads its log lines)
#   input     INPUT_TITLE_LATCH 0x82615048 + 0x822C8B34, INPUT_MENU_REPEAT 0x822240C0 + 0x822241CC (W5.6): the 60 fps input
#             fixes (a title Start press a polling script misses; one menu tap stepping twice); cvars
#             mcla_input_title_latch / mcla_input_menu_repeat (-1 = auto: on while mcla_sim60 is on)
#   vis       mcla_vis_ab.cpp (no flag, no codegen entry; the hooks/*.cpp glob below picks it up): the in-run A/B scheduler,
#             cvar mcla_vis_ab = 'SEC:CVARNAME:VALUE+...' changes cvars at given seconds of one run; mcla::vis::OnFrame() is
#             called from mcla::gfxtune::OnFrame; mcla_vis_scene.cpp (no flag, no codegen entry): the mcla_dev_* scene cvars (time of
#             day, weather, road density; dev / test only), called from mcla::vis::OnFrame
#             VIS_MOTION_BLUR (mid-asm hooks 0x822E85F8 + 0x822E88E0, entries in hooks_vis.toml, cvar mcla_vis_motion_blur): the
#             length of the motion blur streaks at 60 fps (1 = as the game does, -1 = the streak of a 30 fps frame, > 0 = a multiplier)
#
# Not hooks, always compiled (no flag, no [[midasm_hook]] entry; the glob below picks them up): the shared state of a hook
# group (mcla_t_common.cpp, mcla_ws_common.cpp, mcla_cpu_common.cpp, mcla_gfx_tune.cpp) and the LOCK60 GPU frame governor
# mcla_gov.cpp (pure logic in mcla_gov_math.h, unit-tested in mcla/tests/test_gov.cpp), called once per engine frame from
# mcla::sim::OnFrameStart; master switch cvar mcla_gov (off = the game as before).
set(MCLA_HOOK_DEFINITIONS
    # core
    MCLA_HOOK_HEARTBEAT=1
    MCLA_HOOK_RAGE_FATAL=1
    MCLA_HOOK_READ_RETRY_FATAL=1
    MCLA_HOOK_UI_LIGHTS_GUARD=1
    MCLA_HOOK_SKIP_INTRO=1
    MCLA_HOOK_FENCE_SPIN=1
    MCLA_HOOK_STATE_TRACE=1
    MCLA_HOOK_FAST_CLEAR_OFF=1
    MCLA_HOOK_NOAA=1
    # widescreen
    MCLA_HOOK_WS_SCENE_SIZE=1
    MCLA_HOOK_WS_RT_POOL=1
    MCLA_HOOK_WS_RT_GUARD=1
    MCLA_HOOK_WS_EDRAM=1
    MCLA_HOOK_WS_EDRAM_LIMIT=1
    MCLA_HOOK_WS_SINGLE_TILE=1
    MCLA_HOOK_WS_PHYS_POOL=1
    MCLA_HOOK_WS_TALL_FOV=1
    MCLA_HOOK_WS_ASPECT=1
    MCLA_HOOK_WS_CUTSCENE_FOV=1
    MCLA_HOOK_WS_MAP_ASPECT=1
    MCLA_HOOK_WS_UI=1
    MCLA_HOOK_WS_TEXCLEAR=1
    # menus
    MCLA_HOOK_MENU_MSAA=1
    # 60 fps timing
    MCLA_HOOK_SIM_FRAME_DELTA=1
    MCLA_HOOK_SIM_STEP=1
    MCLA_HOOK_SIM_CAMERA_POS=1
    MCLA_HOOK_SIM_CAMERA_LOOKAT=1
    MCLA_HOOK_SIM_CHASSIS=1
    MCLA_HOOK_SIM_UI_ADVANCE=1
    MCLA_HOOK_SIM_UI_COUNTDOWN=1
    MCLA_HOOK_SIM_RACE_CLOCK=1
    MCLA_HOOK_SIM_PRESENT_INTERVAL=1
    MCLA_HOOK_SIM_PRESENT_IMMEDIATE=1
    # CPU levers
    MCLA_HOOK_CPU_POLL_WAIT=1
    MCLA_HOOK_CPU_POLL_RELEASE=1
    MCLA_HOOK_CPU_PUMP=1
    # quality knobs
    MCLA_HOOK_GFX_CULL_MASK=1
    MCLA_HOOK_GFX_SHADOW_MODE=1
    MCLA_HOOK_GFX_DL_ENABLE=1
    MCLA_HOOK_GFX_TRAFFIC_LOD=1
    MCLA_HOOK_GFX_AMBIENT_ZONE=1
    MCLA_HOOK_GFX_PHASES=1
    MCLA_HOOK_GFX_SHADOW_RES=1
    MCLA_HOOK_GFX_ENV_RES=1
    MCLA_HOOK_GFX_IMPOSTORS=1
    MCLA_HOOK_GFX_FAR_CASCADES=1
    MCLA_HOOK_GFX_IMPOSTOR_THROTTLE=1
    # visuals
    MCLA_HOOK_VIS_MOTION_BLUR=1
    # FSR render markers, 60 fps input fixes (W5.6)
    MCLA_HOOK_FSR_MARKERS=1
    MCLA_HOOK_INPUT_TITLE_LATCH=1
    MCLA_HOOK_INPUT_MENU_REPEAT=1
    # HDR measurement helpers (the A/B switcher)
    MCLA_HOOK_HDR=1)

set(MCLA_HOOK_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/hooks"
    "${CMAKE_CURRENT_LIST_DIR}/profile")

# CONFIGURE_DEPENDS: a hook file added later (W4.1, W5.x) is picked up by the next build without a manual re-run
# of CMake. Sorted, so the link order does not depend on the file system.
file(GLOB _mcla_hook_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_LIST_DIR}/hooks/*.cpp"
     "${CMAKE_CURRENT_LIST_DIR}/profile/*.cpp")
list(SORT _mcla_hook_sources)
set(MCLA_HOOK_SOURCES ${_mcla_hook_sources})
unset(_mcla_hook_sources)
