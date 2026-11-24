// Midnight Club: Los Angeles: the pure sizing maths of the widescreen hooks (docs/PLAN.md 3.8, W5.1).
//
// Header-only, constexpr, <cstdint> only: no SDK, no guest memory, no cvars. The hooks (mcla_ws_*.cpp) and the
// unit tests (mcla/tests, a TESTS slice) include exactly this file. Everything is "W x H" (back buffer in
// pixels), never "W x 720": 720p is the special case 1280 x 720 of every formula here.
//
// What is in it, with the game code each part mirrors (V = read in the image, docs/mcla/WIDESCREEN.md):
//
//   Resolve()            the master cvars (mcla_widescreen_width, mcla_scene_height) + the video mode the game
//                        was given -> the back buffer, or off, or a rejected value with a reason.
//   PoolGrowth()         the renderer's shared render-target pool (0x9D8000 bytes at 1280x720, sub_822E47E0 V):
//                        a bump allocator per slot with no bounds check, every texture rounded up to 32 texels a
//                        side, so the pool must grow by the largest growth of any 32-aligned target whose sides
//                        are the screen's divided by 1, 2, 4, 8 or 16 (area, not width).
//   DeviceSurfaceTiles() EDRAM tiles of a D3D-created surface (sub_8240F238 V): ceil(W/80) x align16(H) / 16 at
//                        1x, H doubled at 2x, both at 4x, x2 for 64-bit formats. The two device surfaces
//                        (colour and depth, W x H at 1x whatever `noaa` says) are placed by D3D itself and are
//                        what outgrows the console's 2048 tiles: 1280x720 = 720 each (1440 of 2048),
//                        1920x1080 = 24 x 68 = 1632 each (3264, hence edram_tiles=4096 and the limit hook).
//   GameSceneTiling()    the game's own tile loop (BeginTiledRendering, sub_8217A470 at 0x8217A680..0x8217A700 V,
//                        the mode with flag bit 0 clear): the largest tile whose colour AND depth take 1024 EDRAM
//                        tiles each, and how many tiles that makes of the target. 1280x720: 1x one tile, 2x two,
//                        4x three; 1920x1080: 1x TWO tiles of 672 rows, 2x four, 4x six.
//   SingleTile*()        whether the whole target fits one tile in a larger host EDRAM (colour + depth, each
//                        within the console's 2048 tiles as far as a host render target reaches).
//   TextureBlock*()      the title's block of its first four physical allocations (front buffers + the device
//                        textures, 4 x S, S = align32(W) x align32(H) x 4): the SDK's MmQueryAllocationSize
//                        overshoot of the zero-fill is 2 S bytes past the block (mcla_ws_texclear.cpp).
//   RenderTargetMaxHeight() the SDK cvar render_target_max_height must be at least the back buffer's rows.
//
// Not game facts but this port's decisions: the limits of the accepted sizes (kLimitsM6 now, kLimitsM7 reserved:
// 2560 x 1200 is the optional M7 16:10 / ultrawide step and is NOT accepted until a build selects it), and the
// rounding (W down to a multiple of 8, H down to a multiple of 4).
#pragma once

#include <cstdint>

namespace mcla::ws::sizing {

// ---- the shipped game ------------------------------------------------------------------------------------------

constexpr uint32_t kBaseWidth = 1280;             // the shipped HD back buffer
constexpr uint32_t kBaseHeight = 720;
constexpr uint32_t kHdPoolBytes = 0x9D8000;       // 10,321,920: the renderer's shared target pool in HD
constexpr uint32_t kConsoleEdramTiles = 2048;     // 10 MiB / 5120: the Xenos EDRAM and the game's own check
constexpr uint32_t kShippedFreeTiles = 608;       // 2048 - 2 x 720: what D3D has left for small targets at 1280x720
constexpr uint32_t kTileRowsPerSurface = 1024;    // the game sizes a tile so colour and depth take 1024 tiles each
constexpr uint32_t kRingAndHeapReach = 0x810010;  // bytes past the front-buffer block before a heap's first node
                                                  // (D3D's two ring buffers, then the 0xA8F20010 node, WIDESCREEN.md 14)

constexpr uint32_t CeilDiv(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
constexpr uint32_t AlignUp(uint32_t value, uint32_t alignment) { return CeilDiv(value, alignment) * alignment; }

// ---- accepted sizes --------------------------------------------------------------------------------------------

struct Limits {
  uint32_t min_width, max_width, min_height, max_height;
};
// M6: up to 1920 x 1080 (one 1x scene on the 880M). M7 (reserved, not selected by any cvar): 2560 x 1200 for
// 16:10 and ultrawide. W must exceed the shipped 1280, H is at least the shipped 720.
inline constexpr Limits kLimitsM6{kBaseWidth + 8, 1920, kBaseHeight, 1080};
inline constexpr Limits kLimitsM7{kBaseWidth + 8, 2560, kBaseHeight, 1200};

constexpr uint32_t kWidthStep = 8;   // widths are rounded down to a multiple of 8
constexpr uint32_t kHeightStep = 4;  // heights down to a multiple of 4 (900 and 1080 are, neither is a multiple of 8 / 16)

struct Request {
  int32_t width = 0;   // mcla_widescreen_width: 0 off, -1 auto (the video mode), else W
  int32_t height = 0;  // mcla_scene_height: 0 derived (see Resolve), else H
};

struct VideoMode {
  uint32_t width = 0;  // what XGetVideoMode gave the game (the SDK's --resolution / video_mode_*); 0 = unknown
  uint32_t height = 0;
};

enum class Verdict : uint8_t {
  kOff,       // not asked for, or auto with nothing to do: the game keeps its own 1280x720 (not an error)
  kOn,        // width x height is the back buffer
  kRejected,  // a value that cannot be honoured: the game keeps its own 1280x720 and the log says why (error)
};

enum class Reason : uint8_t {
  kNone,
  // kOff
  kWidthZero,         // mcla_widescreen_width=0
  kAutoModeUnknown,   // auto, but the guest's video mode could not be read
  kAutoModeNotLarger, // auto, the mode is not larger than 1280x720 (no --resolution=WxH)
  // kRejected
  kWidthNegative,     // a negative width other than -1
  kWidthRange,        // outside min_width..max_width after rounding
  kHeightNegative,
  kHeightRange,       // outside min_height..max_height after rounding (an explicit scene height, or the mode's)
  kTooNarrow,         // narrower than 4:3: the game's own IsWidescreen is false there, the aspect hooks cannot act
};

// Bits of Resolution::notes (a successful answer that differs from the literal request).
enum Note : uint32_t {
  kNoteWidthRounded = 1,        // the width was rounded down to a multiple of 8
  kNoteHeightRounded = 2,       // the height was rounded down to a multiple of 4
  kNoteHeightFromMode = 4,      // mcla_scene_height=0: the height is the video mode's
  kNoteModeHeightIgnored = 8,   // mcla_scene_height=0, the mode matched the width but its height is outside the limits: 720
};

struct Resolution {
  Verdict verdict = Verdict::kOff;
  Reason reason = Reason::kNone;
  uint32_t width = 0;  // valid with kOn
  uint32_t height = 0;
  uint32_t notes = 0;  // Note bits, with kOn
};

constexpr const char* Describe(Reason reason) {
  switch (reason) {
    case Reason::kNone:
      return "";
    case Reason::kWidthZero:
      return "mcla_widescreen_width=0";
    case Reason::kAutoModeUnknown:
      return "auto: the guest's video mode could not be read";
    case Reason::kAutoModeNotLarger:
      return "auto: the video mode is not larger than 1280x720 (pass --resolution=WxH)";
    case Reason::kWidthNegative:
      return "mcla_widescreen_width is negative but not -1 (auto)";
    case Reason::kWidthRange:
      return "width outside the accepted range";
    case Reason::kHeightNegative:
      return "mcla_scene_height is negative";
    case Reason::kHeightRange:
      return "height outside the accepted range";
    case Reason::kTooNarrow:
      return "narrower than 4:3 (the game's widescreen branch is not taken)";
  }
  return "";
}

// The back buffer from the two cvars and the video mode. Pure; never logs.
//   width  0   off.
//   width -1   auto: W x H = the video mode, when it is larger than 1280x720 in both W (>= min_width) and H
//              (>= min_height); an explicit scene height replaces the mode's H.
//   width  W   W x H, H = the explicit scene height, else the video mode's height when the mode's width is W
//              (the profile's `video_mode_width/height` = the scene), else the shipped 720.
// W rounds down to a multiple of 8, H to a multiple of 4; a result outside `limits` is rejected, not clamped.
constexpr Resolution Resolve(Request request, VideoMode mode, const Limits& limits = kLimitsM6) {
  Resolution r{};
  const bool mode_known = mode.width != 0 && mode.height != 0;
  bool automatic = false;
  uint32_t width = 0;
  if (request.width == 0) {
    r.reason = Reason::kWidthZero;
    return r;
  }
  if (request.width == -1) {
    automatic = true;
    if (!mode_known) {
      r.reason = Reason::kAutoModeUnknown;
      return r;
    }
    if (mode.width < limits.min_width || mode.height < limits.min_height) {
      r.reason = Reason::kAutoModeNotLarger;
      return r;
    }
    width = mode.width;
  } else if (request.width < 0) {
    r.verdict = Verdict::kRejected;
    r.reason = Reason::kWidthNegative;
    return r;
  } else {
    width = uint32_t(request.width);
  }

  const uint32_t rounded_width = width & ~(kWidthStep - 1);
  if (rounded_width < limits.min_width || rounded_width > limits.max_width) {
    r.verdict = Verdict::kRejected;
    r.reason = Reason::kWidthRange;
    r.width = width;
    return r;
  }

  if (request.height < 0) {
    r.verdict = Verdict::kRejected;
    r.reason = Reason::kHeightNegative;
    return r;
  }
  uint32_t height = kBaseHeight;
  uint32_t notes = 0;
  if (request.height > 0) {
    height = uint32_t(request.height);
  } else if (automatic) {
    height = mode.height;
    if (height != kBaseHeight) {
      notes |= kNoteHeightFromMode;
    }
  } else if (mode_known && mode.width == rounded_width) {
    const uint32_t mode_height = mode.height & ~(kHeightStep - 1);
    if (mode_height >= limits.min_height && mode_height <= limits.max_height) {
      height = mode.height;
      if (height != kBaseHeight) {
        notes |= kNoteHeightFromMode;
      }
    } else {
      notes |= kNoteModeHeightIgnored;
    }
  }
  const uint32_t rounded_height = height & ~(kHeightStep - 1);
  if (rounded_height < limits.min_height || rounded_height > limits.max_height) {
    r.verdict = Verdict::kRejected;
    r.reason = Reason::kHeightRange;
    r.width = rounded_width;
    r.height = height;
    return r;
  }
  if (uint64_t(rounded_width) * 3 < uint64_t(rounded_height) * 4) {
    r.verdict = Verdict::kRejected;
    r.reason = Reason::kTooNarrow;
    r.width = rounded_width;
    r.height = rounded_height;
    return r;
  }
  if (rounded_width != width) {
    notes |= kNoteWidthRounded;
  }
  if (rounded_height != height) {
    notes |= kNoteHeightRounded;
  }
  r.verdict = Verdict::kOn;
  r.width = rounded_width;
  r.height = rounded_height;
  r.notes = notes;
  return r;
}

// ---- aspect ----------------------------------------------------------------------------------------------------

// Exactly 16:9: the game's own constants (0x8201E7EC, the cutscene and map boxes) are already right, so the
// aspect hooks have nothing to replace. 1920x1080 and 1600x900 are.
constexpr bool IsSixteenNine(uint32_t width, uint32_t height) {
  return height != 0 && uint64_t(width) * 9 == uint64_t(height) * 16;
}

// W / H rounded to single precision like the game's float constants (0 for H = 0).
constexpr double AspectOf(uint32_t width, uint32_t height) {
  return height == 0 ? 0.0 : double(float(width) / float(height));
}

// The width in screen pixels of a 16:9 block of the screen's full height: the Flash stage's own shape (1280 at
// 720 lines). The UI squeeze narrows a full-width viewport to this.
constexpr double UiBlockWidth(uint32_t height) { return double(height) * 16.0 / 9.0; }

// ---- render-target pool ----------------------------------------------------------------------------------------

struct Ratio {
  uint64_t num = 1, den = 1;
  constexpr double value() const { return double(num) / double(den); }
};

// The largest growth, over k = 1, 2, 4, 8, 16, of a 32-texel-aligned (W/k) x (H/k) target against the same at
// 1280 x 720 (never below 1). A slot's cursor is a sum of such sizes, so no cursor grows by more than this.
// 1560x720 -> 1.4; 1920x1080 -> 2.667 (k = 8: 256 x 160 against 160 x 96); 1600x900 -> 2.0.
constexpr Ratio PoolGrowth(uint32_t width, uint32_t height) {
  Ratio best{1, 1};
  for (uint32_t k = 1; k <= 16; k *= 2) {
    const uint64_t num = uint64_t(AlignUp(CeilDiv(width, k), 32)) * AlignUp(CeilDiv(height, k), 32);
    const uint64_t den = uint64_t(AlignUp(CeilDiv(kBaseWidth, k), 32)) * AlignUp(CeilDiv(kBaseHeight, k), 32);
    if (num * best.den > best.num * den) {
      best = Ratio{num, den};
    }
  }
  return best;
}

// `shipped` x growth, rounded up to 64 KiB (the kernel's allocation granularity for the pool).
constexpr uint64_t ScaledPoolBytes(uint32_t shipped, Ratio growth) {
  constexpr uint64_t kPage = 0x10000;
  const uint64_t scaled = (uint64_t(shipped) * growth.num + growth.den - 1) / growth.den;
  return (scaled + kPage - 1) / kPage * kPage;
}

// ---- EDRAM -----------------------------------------------------------------------------------------------------

// msaa = the renderer's MSAA mode as the game stores it: 1 (none), 2, 4.
constexpr uint32_t MsaaScaleX(uint32_t msaa) { return msaa >= 4 ? 2 : 1; }
constexpr uint32_t MsaaScaleY(uint32_t msaa) { return msaa >= 2 ? 2 : 1; }

// EDRAM tiles (80 x 16 samples, 5120 bytes) of one D3D surface the way the title's create-surface computes it.
constexpr uint32_t DeviceSurfaceTiles(uint32_t width, uint32_t height, uint32_t msaa = 1, bool is_64bpp = false) {
  return CeilDiv(width * MsaaScaleX(msaa), 80) * (AlignUp(height * MsaaScaleY(msaa), 16) / 16) *
         (is_64bpp ? 2u : 1u);
}

// Tiles left over for the targets D3D places itself, after both device surfaces (negative: they do not fit).
constexpr int32_t FreeTilesAfterDeviceSurfaces(uint32_t edram_tiles, uint32_t width, uint32_t height) {
  return int32_t(edram_tiles) - 2 * int32_t(DeviceSurfaceTiles(width, height));
}

struct SceneTiling {
  uint32_t tile_rows = 0;       // rows of the largest tile (0: the arithmetic cannot make one)
  uint32_t count = 0;           // tiles the game draws the scene in
  uint32_t surface_width = 0;   // the tile surfaces: W rounded up to 160 ...
  uint32_t surface_tiles = 0;   // EDRAM tiles of one of them (colour or depth), at most 1024
};

// The tiling BeginTiledRendering chooses for a width x height target at 1x / 2x / 4x (flag bit 0 clear, overlap 0).
constexpr SceneTiling GameSceneTiling(uint32_t width, uint32_t height, uint32_t msaa, uint32_t overlap = 0) {
  SceneTiling t{};
  const uint32_t unit_width = msaa > 2 ? 40 : 80;
  const uint32_t unit_height = msaa > 1 ? 8 : 16;
  const uint32_t surface_width = CeilDiv(width, 160) * 160;
  const uint32_t across = surface_width / unit_width;
  if (across == 0) {
    return t;
  }
  const uint32_t rows = ((unit_height * kTileRowsPerSurface) / across) & ~31u;
  if (rows <= overlap) {
    return t;
  }
  t.tile_rows = rows;
  t.count = (rows - 2 * overlap + height - 1) / (rows - overlap);
  t.surface_width = surface_width;
  t.surface_tiles = across * (rows / unit_height);
  return t;
}

// EDRAM tiles of ONE surface (colour or depth) when the whole target is a single tile: the width rounded up to 160
// and the height up to 32, in 80x16 / 80x8 / 40x8 tiles.
constexpr uint32_t SingleTileSurfaceTiles(uint32_t width, uint32_t height, uint32_t msaa) {
  const uint32_t unit_width = msaa > 2 ? 40 : 80;
  const uint32_t unit_height = msaa > 1 ? 8 : 16;
  return (CeilDiv(width, 160) * 160 / unit_width) * (AlignUp(height, 32) / unit_height);
}

// Colour and depth, one after the other, fit the host's EDRAM, and each one stays within the console's 2048 tiles
// (a host render target reaches no further from its base).
constexpr bool SingleTileFits(uint32_t width, uint32_t height, uint32_t msaa, uint32_t host_edram_tiles) {
  const uint32_t surface = SingleTileSurfaceTiles(width, height, msaa);
  return surface <= kConsoleEdramTiles && 2 * uint64_t(surface) <= host_edram_tiles;
}

// ---- the front-buffer block and the zero-fill overrun ----------------------------------------------------------

// S: one W x H x 32 bit texture the way D3D sizes it (32-texel aligned sides). The title's first four physical
// allocations (two front buffers, the device colour and depth textures) share ONE block of 4 S.
constexpr uint64_t TextureBlockUnit(uint32_t width, uint32_t height) {
  return uint64_t(AlignUp(width, 32)) * AlignUp(height, 32) * 4;
}

// How far the SDK's MmQueryAllocationSize answer (the whole 4 S block from an address inside it) makes the device
// colour texture's zero-fill (at block + 2 S) run past the block's end: 2 S.
constexpr uint64_t TextureClearOvershoot(uint32_t width, uint32_t height) { return 2 * TextureBlockUnit(width, height); }

// True when that overshoot reaches the first node of the heap behind the block (guard word fatal) without the
// mcla_ws_texclear clamp or an SDK that counts from the address. 1280x720: no (0x730000); 1440: yes.
constexpr bool TextureClearReachesHeap(uint32_t width, uint32_t height) {
  return TextureClearOvershoot(width, height) > kRingAndHeapReach;
}

// ---- SDK cvars -------------------------------------------------------------------------------------------------

// render_target_max_height (rows at 1x, rounded to the 16-row tile by the SDK) must cover the back buffer, or the
// bottom of every full-screen target is cut: the PC profile's 1024 is right for 720 lines, not for 1080.
constexpr uint32_t RenderTargetMaxHeight(uint32_t height) { return AlignUp(height, 16); }

}  // namespace mcla::ws::sizing
