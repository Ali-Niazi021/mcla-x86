// Pure arithmetic of the 2000p (draw resolution scale 2) cost levers R2 and R3: no SDK, no game, no generated code, so
// mcla/tests/test_gfx_res.cpp compiles it standalone.
//
//   R2: off-screen targets double with the draw resolution scale (a guest 640x640 shadow map is a 1280x1280 host target at
//       scale 2). The game side makes them smaller instead: the sun-shadow map's edge (mcla_gfx_shadow_res, hook 0x825F69A0)
//       and the environment map's size (mcla_gfx_env_res, hook 0x8233AB3C).
//   R3: variable rate shading (SDK patch 0500) as the base level of the 2000p profile: the governor's VRS rungs sit ABOVE that
//       base (mcla_gov.cpp), so a shed never walks the shading rate back to a finer one.
#pragma once

#include <cstdint>

namespace mcla::gfxres {

// ---- the sun-shadow map (rage::CShadowMap, mcla_gfx_shadow_res.cpp) -------------------------------------------------------------

inline constexpr uint32_t kShadowShippedEdge = 640;

// The edges the hook accepts: the shipped 640 and the sizes of the presets (512 perf, 384) and of draw scale 2 (320 = the 1080p
// look, 256 and 192 = what the perf preset's 512 and 384 become at scale 2: ShadowEdgeAtScale). Every one is a multiple of 32
// (the texture tile) and of 16 (the EDRAM tile's rows), and fits the game's own EDRAM check (ShadowFitsEdram).
constexpr bool IsShadowEdge(int32_t edge) {
  return edge == 640 || edge == 512 || edge == 384 || edge == 320 || edge == 256 || edge == 192;
}

// The cvar's value as the edge the map is created with: 0 (the cvar's default) and anything that is not an accepted edge = shipped.
constexpr uint32_t ShadowEdgeOf(int32_t cvar_value) {
  return IsShadowEdge(cvar_value) ? uint32_t(cvar_value) : kShadowShippedEdge;
}

constexpr bool ShadowEdgeIsTiled(uint32_t edge) { return edge != 0 && edge % 32 == 0 && edge % 16 == 0; }

// The game's own check in sub_825F4EF8 (0x825F4F6C..0x825F4F7C): `(w >> 4) * (h >> 3) <= 3600` is stored as the "fits EDRAM"
// flag of the render target (640: 3200, 512: 2048, 384: 1152, 320: 800).
constexpr bool ShadowFitsEdram(uint32_t edge) { return (edge >> 4) * (edge >> 3) <= 3600; }

// A gfx preset's value for mcla_gfx_shadow_res at the draw resolution scale: the presets name the HOST look (perf = 512, quality
// and ultra = 0 = the shipped 640), the guest edge is that divided by the scale, so the host target of a preset at scale 2 is
// the one of the same preset at scale 1. Only the square scale 2 of the 2000p profile is compensated; any other scale (and a
// scale of 1) returns the value unchanged. 0 stands for the shipped 640. A result that is not an accepted edge returns the
// value unchanged.
constexpr int32_t ShadowEdgeAtScale(int32_t preset_value, int32_t draw_scale_x, int32_t draw_scale_y) {
  if (draw_scale_x != 2 || draw_scale_y != 2) {
    return preset_value;
  }
  const int32_t host = preset_value == 0 ? int32_t(kShadowShippedEdge) : preset_value;
  const int32_t guest = host / 2;
  return (host % 2 == 0 && IsShadowEdge(guest)) ? guest : preset_value;
}

// ---- the environment map (the dual-paraboloid car reflection map, mcla_gfx_env_res.cpp) -------------------------------------------

inline constexpr uint32_t kEnvShippedWidth = 512;
inline constexpr uint32_t kEnvShippedHeight = 256;

struct EnvSize {
  uint32_t width;
  uint32_t height;
};

constexpr bool IsEnvWidth(int32_t width) { return width == 512 || width == 384 || width == 256; }

// The cvar's value (the map's width; the height is half of it, as shipped: two square hemispheres side by side) as the size the
// map is created with: 0 and anything that is not an accepted width = shipped 512x256.
constexpr EnvSize EnvSizeOf(int32_t cvar_value) {
  return IsEnvWidth(cvar_value) ? EnvSize{uint32_t(cvar_value), uint32_t(cvar_value) / 2}
                                : EnvSize{kEnvShippedWidth, kEnvShippedHeight};
}

constexpr bool EnvSizeIsTiled(EnvSize size) {
  return size.width % 32 == 0 && size.height % 16 == 0 && size.width == 2 * size.height;
}

// ---- variable rate shading base level (the governor's rungs, mcla_gov.cpp) ----------------------------------------------------------

// The SDK's vrs_level cvar is the BASE level (0 = none); the governor's VRS rungs go from base + 1 to what the SDK's table holds.
// `configured` = mcla_gov_vrs_levels, `sdk_levels` = rexgpu_vrs_levels (0 = no VRS), `base` = the vrs_level cvar. With base 0 this is
// what the governor always used (min(configured, sdk_levels)); with the base at the table's last level there is no rung left.
constexpr int VrsRungs(int configured, int sdk_levels, int base) {
  const int b = base < 0 ? 0 : base;
  const int above = sdk_levels - b;
  const int n = configured < above ? configured : above;
  return n < 0 ? 0 : n;
}

// What the governor writes to rexgpu_vrs_level for its VRS rung: -1 = not governed (the SDK's own cvar, the base, decides),
// else the base plus the rung.
constexpr int VrsWriteLevel(int rung, int base) { return rung >= 1 ? (base < 0 ? 0 : base) + rung : -1; }

}  // namespace mcla::gfxres
