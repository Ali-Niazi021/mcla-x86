// Midnight Club: Los Angeles: the pure sizing maths of the widescreen hooks (docs/PLAN.md 3.8, W5.1).
//
// Header-only, <cstdint> and <cmath> only: no SDK, no guest memory, no cvars. Everything is constexpr but VertPlusFov.
// The hooks (mcla_ws_*.cpp) and the
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
//   PoolAreaGrowth()     the default pool policy: the AREA ratio (x2.25 at 1920x1080), Xenia's 0x1626000. The k-max
//   AreaPoolBytes()      PoolGrowth above is the conservative override (x2.667 there, 4.3 MB more).
//   DeviceSurfaceTiles() EDRAM tiles of a D3D-created surface (sub_8240F238 V): ceil(W/80) x align16(H) / 16 at
//                        1x, H doubled at 2x, both at 4x, x2 for 64-bit formats. The two device surfaces
//                        (colour and depth, W x H at 1x whatever `noaa` says) are placed by D3D itself and are
//                        what outgrows the console's 2048 tiles: 1280x720 = 720 each (1440 of 2048),
//                        1920x1080 = 24 x 68 = 1632 each (3264, hence edram_tiles=4096 and the limit hook).
//   GameSceneTiling()    the game's own tile loop (BeginTiledRendering, sub_8217A470 at 0x8217A680..0x8217A700 V,
//                        the mode with flag bit 0 clear): the largest tile whose colour AND depth take 1024 EDRAM
//                        tiles each, and how many tiles that makes of the target. 1280x720: 1x one tile, 2x two,
//                        4x three; 1920x1080: 1x TWO tiles of 672 rows, 2x four, 4x seven (160-row tiles).
//   SingleTile*()        whether the whole target fits one tile in a larger host EDRAM (colour + depth, each
//                        within the console's 2048 tiles as far as a host render target reaches).
//   TextureBlock*()      the title's block of its first four physical allocations (front buffers + the device
//                        textures, 4 x S, S = align32(W) x align32(H) x 4): the SDK's MmQueryAllocationSize
//                        overshoot of the zero-fill is 2 S bytes past the block (mcla_ws_texclear.cpp).
//   RenderTargetMaxHeight() the SDK cvar render_target_max_height must be at least the back buffer's rows.
//   Physical*()          the guest's 512 MiB of physical memory: what a bigger back buffer adds (the front-buffer
//                        block plus the pool) and how much of the game's own 299.5 MiB physical pool to give back
//                        for it (Xenia's 32 MiB at 1920x1080; mcla_ws_phys_pool.cpp).
//
//   IsTall() / UiBlockHeight() / VertPlusFov()
//                        a buffer taller than 16:9 (1920x1200, 1280x800): the camera goes Hor- unless the vertical
//                        angle is widened (Vert+, mcla_ws_tall_fov.cpp), and the Flash UI is drawn in a 16:9 band.
//
// Not game facts but this port's decisions: the limits of the accepted sizes (kLimits: 1280..1920 x 720..1200, bounded
// by what one EDRAM surface can reach, see Limits), and the rounding (W down to a multiple of 8, H down to a multiple
// of 4).
#pragma once

#include <cmath>
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

// ---- the D3D surface ---------------------------------------------------------------------------------------------

// msaa = the renderer's MSAA mode as the game stores it: 1 (none), 2, 4.
constexpr uint32_t MsaaScaleX(uint32_t msaa) { return msaa >= 4 ? 2 : 1; }
constexpr uint32_t MsaaScaleY(uint32_t msaa) { return msaa >= 2 ? 2 : 1; }

// EDRAM tiles (80 x 16 samples, 5120 bytes) of one D3D surface the way the title's create-surface computes it.
constexpr uint32_t DeviceSurfaceTiles(uint32_t width, uint32_t height, uint32_t msaa = 1, bool is_64bpp = false) {
  return CeilDiv(width * MsaaScaleX(msaa), 80) * (AlignUp(height * MsaaScaleY(msaa), 16) / 16) *
         (is_64bpp ? 2u : 1u);
}

// ---- accepted sizes --------------------------------------------------------------------------------------------

struct Limits {
  uint32_t min_width, max_width, min_height, max_height;
};
// The sizes this port accepts: 1280..1920 wide, 720..1200 high. Exactly 1280x720 is the game's own size (off). The
// upper end is what one surface can reach: a D3D surface is at most 2048 EDRAM tiles (the console's, and as far as
// a host render target reaches from its base), which 1920x1200 (24 x 75 = 1800) is inside and 2048x1280 (2080) or
// 2560x1200 (2400) are not; Resolve rejects those with kEdramSurface whatever `limits` says.
inline constexpr Limits kLimits{kBaseWidth, 1920, kBaseHeight, 1200};

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
  kAutoModeNotLarger, // auto, the mode is smaller than 1280x720 in W or H (no --resolution=WxH)
  kShippedSize,       // the request resolves to exactly 1280x720: the game's own size, nothing to do
  // kRejected
  kWidthNegative,     // a negative width other than -1
  kWidthRange,        // outside min_width..max_width after rounding
  kHeightNegative,
  kHeightRange,       // outside min_height..max_height after rounding (an explicit scene height, or the mode's)
  kTooNarrow,         // narrower than 4:3: the game's own IsWidescreen is false there, the aspect hooks cannot act
  kEdramSurface,      // a device surface of more than 2048 EDRAM tiles: a surface cannot reach further
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
      return "auto: the video mode is smaller than 1280x720 (pass --resolution=WxH)";
    case Reason::kShippedSize:
      return "1280x720 is the game's own size";
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
    case Reason::kEdramSurface:
      return "a device surface would need more than 2048 EDRAM tiles (more than one surface can reach)";
  }
  return "";
}

// The back buffer from the two cvars and the video mode. Pure; never logs.
//   width  0   off.
//   width -1   auto: W x H = the video mode, when it is at least 1280x720 (and not exactly that: larger in W or in H);
//              an explicit scene height replaces the mode's H.
//   width  W   W x H, H = the explicit scene height, else the video mode's height when the mode's width is W
//              (the profile's `video_mode_width/height` = the scene), else the shipped 720.
// W rounds down to a multiple of 8, H to a multiple of 4; a result outside `limits` is rejected, not clamped. A result
// of exactly 1280x720 is kOff (kShippedSize). Narrower than 4:3 is kTooNarrow, and a device surface of more than 2048
// EDRAM tiles is kEdramSurface.
constexpr Resolution Resolve(Request request, VideoMode mode, const Limits& limits = kLimits) {
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
    if (mode.width < limits.min_width || mode.height < limits.min_height ||
        (mode.width == kBaseWidth && mode.height == kBaseHeight)) {
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
  if (rounded_width == kBaseWidth && rounded_height == kBaseHeight) {
    r.reason = Reason::kShippedSize;
    return r;
  }
  if (uint64_t(rounded_width) * 3 < uint64_t(rounded_height) * 4) {
    r.verdict = Verdict::kRejected;
    r.reason = Reason::kTooNarrow;
    r.width = rounded_width;
    r.height = rounded_height;
    return r;
  }
  if (DeviceSurfaceTiles(rounded_width, rounded_height) > kConsoleEdramTiles) {
    r.verdict = Verdict::kRejected;
    r.reason = Reason::kEdramSurface;
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
// 720 lines). The UI squeeze narrows a full-width viewport to this on a buffer wider than 16:9.
constexpr double UiBlockWidth(uint32_t height) { return double(height) * 16.0 / 9.0; }

// A back buffer taller than 16:9 (1920x1200, 1280x800, 1440x900). The game's widescreen branch is still taken
// (the video mode's is-wide test is W x 3 >= H x 4) and its 16:9 constants are hooked to W/H, which with the vertical
// FOV fixed (Perspective: tanH = tanV x aspect) leaves less than the 16:9 horizontal view: Hor-. Vert+ (VertPlusFov)
// and the UI's 16:9 band are the answers.
constexpr bool IsTall(uint32_t width, uint32_t height) { return height != 0 && uint64_t(width) * 9 < uint64_t(height) * 16; }

// The height in screen pixels of a 16:9 block of the screen's full width: the Flash stage's own shape on a tall
// buffer (720 at 1280 wide, 1080 at 1920). The UI band is this tall, centred vertically.
constexpr double UiBlockHeight(uint32_t width) { return double(width) * 9.0 / 16.0; }

// The game's 16/9 constant as the single-precision float it is (0x8201E7EC, 3FE38E39), widened.
inline constexpr double kSixteenNine = double(16.0f / 9.0f);

// sub_8217CB68 turns the FOV (the FULL vertical angle in degrees) into tanV = tan(fov x k), k = 0x820086A8 = 3C0EFA35
// = pi / 360, and Perspective keeps it: tanH = tanV x aspect. Vert+ keeps the 16:9 horizontal view on a taller buffer
// by widening the vertical angle: tanV' = tanV x (16/9) / aspect, so tanH' = tanV x 16/9 whatever the camera's own
// lerp of the aspect (it multiplies both). 60 degrees -> 65.36 at 1.6 (1920x1200), 45 -> 49.43; the identity at
// aspect >= 16/9 (a wider buffer stays Hor+, which is the game's own behaviour). Not constexpr: <cmath>.
inline double VertPlusFov(double fov_degrees, double aspect, double k) {
  if (!(aspect > 0.0) || aspect >= kSixteenNine - 1e-4 || !(k > 0.0)) {
    return fov_degrees;
  }
  return std::atan(std::tan(fov_degrees * k) * (kSixteenNine / aspect)) / k;
}

// temporal_aa=fsr squeezes only a scene surface whose pitch equals the output width (SDK temporal_aa.cpp:349); the
// game's tile surface pitch is the width rounded up to 160, so the output width must be a multiple of 160 (1280, 1440,
// 1600, 1920 are; 1680 is not).
constexpr bool FsrSqueezeWidthOk(uint32_t width) { return width % 160 == 0; }

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

// `shipped` x growth, rounded up to `granularity` bytes: 64 KiB, the kernel's allocation granularity for the pool,
// unless the caller says otherwise (AreaPoolBytes uses the 4 KiB page, which is what Xenia's value is a multiple of).
constexpr uint64_t ScaledPoolBytes(uint32_t shipped, Ratio growth, uint64_t granularity = 0x10000) {
  const uint64_t scaled = (uint64_t(shipped) * growth.num + growth.den - 1) / growth.den;
  return (scaled + granularity - 1) / granularity * granularity;
}

// The pool's growth as the AREA ratio (W x H) / (1280 x 720), never below 1: 2.25 at 1920x1080, 1.5625 at 1600x900.
// This is Xenia's choice for 1920x1080 (the pool constant 0x1626000 = 0x9D8000 x 2.25) and the default policy; the
// conservative max-over-aligned-targets PoolGrowth above is x2.667 there and stays as the explicit override.
constexpr Ratio PoolAreaGrowth(uint32_t width, uint32_t height) {
  const uint64_t num = uint64_t(width) * height;
  const uint64_t den = uint64_t(kBaseWidth) * kBaseHeight;
  return num > den ? Ratio{num, den} : Ratio{1, 1};
}

// The pool for a width x height buffer under the area policy, in whole 4 KiB pages: 23,224,320 at 1920x1080,
// 16,130,048 at 1600x900, `shipped` unchanged at 1280x720.
constexpr uint64_t AreaPoolBytes(uint32_t shipped, uint32_t width, uint32_t height) {
  return ScaledPoolBytes(shipped, PoolAreaGrowth(width, height), 4096);
}

// ---- EDRAM -----------------------------------------------------------------------------------------------------

// (MsaaScaleX / MsaaScaleY / DeviceSurfaceTiles are above, next to Resolve, which uses them.)

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

// ---- the guest's physical memory -------------------------------------------------------------------------------
//
// The console has 512 MiB of physical memory and the title takes it top-down at boot: its own physical pool first
// (sub_82131228: 306,688 KiB = 299.5 MiB, `lis r9,4 ; ori r30,r9,44544` at 0x821312DC), then the game heaps, and what
// is left below them (about 143 MiB) is all D3D's, the front-buffer block, the render-target pool and every texture
// the renderer makes. A bigger back buffer grows the block and the pool, nothing else gives way, and the physical heap
// runs out in the renderer's constructor: `BaseHeap::Alloc failed to find contiguous range`, then RAGE's "Unable to
// create backup texture for rendertarget" (caller 0x82185D68) at 1920x1080 with the pool at x2.667.
//
// Xenia Canary's "1920x1080 Resolution" patch for this title (BSD, xenia-canary/game-patches 545407F8) pays for it
// by taking the same memory out of the game's own pool: 0x821312DC `ori r30,r9,0x2E00` = 273,920 KiB (267.5 MiB), 32 MiB less,
// next to an RT pool of 0x1626000 = 23,224,320 bytes = the shipped pool x the AREA ratio 2.25. Those two numbers are
// exactly what these functions give at 1920x1080.

constexpr uint32_t kPhysicalPoolKiB = 0x4AE00;  // the shipped pool in KiB: 306,688 (299.5 MiB)

// The four-allocation front-buffer block (two front buffers, the device colour and depth textures): 4 S.
constexpr uint64_t FrontBufferBlockBytes(uint32_t width, uint32_t height) { return 4 * TextureBlockUnit(width, height); }

// How much more physical memory a width x height back buffer needs than 1280x720: the block's growth plus the pool's
// (`pool_bytes` = what the pool will be created with; the shipped kHdPoolBytes when it is not scaled).
constexpr uint64_t PhysicalGrowthBytes(uint32_t width, uint32_t height, uint64_t pool_bytes) {
  const uint64_t block = FrontBufferBlockBytes(width, height);
  const uint64_t base_block = FrontBufferBlockBytes(kBaseWidth, kBaseHeight);
  return (block > base_block ? block - base_block : 0) + (pool_bytes > kHdPoolBytes ? pool_bytes - kHdPoolBytes : 0);
}

// How many KiB to take out of the game's physical pool for that growth: whole MiB, rounded up, plus 2 MiB of slack.
// 1920x1080 with the area pool: 31,252,480 bytes = 29.8 MiB -> 30 + 2 = 32 MiB = 32,768 KiB, Xenia's number. 0 when nothing grows.
constexpr uint32_t PhysicalPoolReductionKiB(uint64_t growth_bytes) {
  constexpr uint64_t kMiB = 1024 * 1024;
  return growth_bytes == 0 ? 0u : uint32_t(((growth_bytes + kMiB - 1) / kMiB + 2) * 1024);
}

// ---- SDK cvars -------------------------------------------------------------------------------------------------

// render_target_max_height (rows at 1x, rounded to the 16-row tile by the SDK) must cover the back buffer, or the
// bottom of every full-screen target is cut: the PC profile's 1024 is right for 720 lines, not for 1080.
constexpr uint32_t RenderTargetMaxHeight(uint32_t height) { return AlignUp(height, 16); }

// The rows a host render target of RenderTargetMaxHeight() rows has below a `height`-row picture: the padding to whole
// 16-row EDRAM tiles (times the draw resolution scale on the host). 0 exactly when the height is a multiple of 16, and then
// the host render target the game resolves from is as tall as the texture it resolves into (the 3200x1984 of 1600x992 at
// scale 2): the size condition of render-target renaming (scratch/zerocopy/R5_DESIGN.md 2.1). 1000 rows pad 8 (16 host rows
// at scale 2: a 3200x2016 render target for a 3200x2000 texture).
constexpr uint32_t RenderTargetPaddingRows(uint32_t height) { return RenderTargetMaxHeight(height) - height; }

}  // namespace mcla::ws::sizing
