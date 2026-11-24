// Unit test of the widescreen sizing maths (mcla/hooks/mcla_ws_sizing.h): the back buffer a W x H request resolves
// to (and the requests that are rejected), the EDRAM tiles of the device surfaces and of the game's own tile loop,
// the render-target pool growth, the front-buffer block overshoot of the zero-fill and the SDK cvar height. Pinned
// to the tables of $IOS/docs/mcla/WIDESCREEN.md (sections 3, 13.1, 13.2, 14.1) and to the 1080p numbers of PLAN 3.8.
// Standalone: no SDK, no game, no guest memory.
//
// Built and run by CTest (mcla/tests/CMakeLists.txt, test mcla_ws_sizing_test). By hand:
//   clang++ -std=c++23 -O1 -Wall -Wextra -I mcla/hooks mcla/tests/test_ws_sizing.cpp \
//       -o <scratch>/test_ws_sizing && <scratch>/test_ws_sizing
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "mcla_ws_sizing.h"

namespace sz = mcla::ws::sizing;

// The functions are constexpr: the hooks may use them in constant expressions.
static_assert(sz::Resolve({1920, 1080}, {1920, 1080}).verdict == sz::Verdict::kOn);
static_assert(sz::ScaledPoolBytes(sz::kHdPoolBytes, sz::PoolGrowth(1920, 1080)) == 27525120);

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_failures;                                                              \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
    }                                                                            \
  } while (0)

#define CHECK_EQ(got, want)                                                                        \
  do {                                                                                             \
    const long long got_ = static_cast<long long>(got), want_ = static_cast<long long>(want);      \
    ++g_checks;                                                                                    \
    if (got_ != want_) {                                                                           \
      ++g_failures;                                                                                \
      std::printf("FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #got, got_, want_); \
    }                                                                                              \
  } while (0)

#define CHECK_TILING(t, rows_, count_, tiles_) \
  do {                                         \
    CHECK_EQ((t).tile_rows, rows_);            \
    CHECK_EQ((t).count, count_);               \
    CHECK_EQ((t).surface_tiles, tiles_);       \
  } while (0)

// The three kinds of answer of Resolve(). `notes` is compared exactly (0 = the request is honoured literally).
bool On(const sz::Resolution& r, uint32_t width, uint32_t height, uint32_t notes = 0) {
  return r.verdict == sz::Verdict::kOn && r.reason == sz::Reason::kNone && r.width == width && r.height == height &&
         r.notes == notes;
}
bool Rejected(const sz::Resolution& r, sz::Reason why) {
  return r.verdict == sz::Verdict::kRejected && r.reason == why;
}
bool Off(const sz::Resolution& r, sz::Reason why) { return r.verdict == sz::Verdict::kOff && r.reason == why; }

// ---- EDRAM tiles of the D3D device surfaces (WIDESCREEN.md 13.1) -----------------------------------------------

void TestDeviceSurfaceTiles() {
  CHECK_EQ(sz::DeviceSurfaceTiles(1280, 720), 720);    // 16 x 45
  CHECK_EQ(sz::DeviceSurfaceTiles(1560, 720), 900);    // 20 x 45
  CHECK_EQ(sz::DeviceSurfaceTiles(1920, 1080), 1632);  // 24 x 68
  CHECK_EQ(sz::DeviceSurfaceTiles(1600, 900), 1140);   // 20 x 57
  CHECK_EQ(sz::DeviceSurfaceTiles(1280, 720, 2), 1440);
  CHECK_EQ(sz::DeviceSurfaceTiles(1280, 720, 4), 2880);
  CHECK_EQ(sz::DeviceSurfaceTiles(1280, 720, 1, true), 1440);  // 64 bits per pixel: twice

  // Colour and depth (W x H at 1x whatever MSAA says) against the console's 2048 tiles.
  CHECK_EQ(sz::FreeTilesAfterDeviceSurfaces(2048, 1280, 720), 608);  // what the shipped game has left
  CHECK_EQ(sz::FreeTilesAfterDeviceSurfaces(2048, 1560, 720), 248);  // the log line of 13.1
  CHECK_EQ(sz::FreeTilesAfterDeviceSurfaces(2048, 1920, 1080), -1216);  // does not fit: edram_tiles=4096 + the limit hook
  CHECK_EQ(sz::FreeTilesAfterDeviceSurfaces(4096, 1920, 1080), 832);
  CHECK_EQ(sz::FreeTilesAfterDeviceSurfaces(4096, 1600, 900), 1816);
}

// ---- the game's tile loop (WIDESCREEN.md section 3) ------------------------------------------------------------

void TestGameSceneTiling() {
  // The shipped 1280x720: one tile at 1x, two at 2x, three at 4x, always 1024 tiles a surface.
  auto t = sz::GameSceneTiling(1280, 720, 1);
  CHECK_TILING(t, 1024, 1, 1024);
  CHECK_EQ(t.surface_width, 1280);
  t = sz::GameSceneTiling(1280, 720, 2);
  CHECK_TILING(t, 512, 2, 1024);
  t = sz::GameSceneTiling(1280, 720, 4);
  CHECK_TILING(t, 256, 3, 1024);

  // 1560x720 and 1600x720 share the 1600-wide tile surfaces: one tile at 1x.
  t = sz::GameSceneTiling(1560, 720, 1);
  CHECK_TILING(t, 800, 1, 1000);
  CHECK_EQ(t.surface_width, 1600);
  t = sz::GameSceneTiling(1600, 720, 1);
  CHECK_TILING(t, 800, 1, 1000);
  t = sz::GameSceneTiling(1560, 720, 2);
  CHECK_TILING(t, 384, 2, 960);
  t = sz::GameSceneTiling(1560, 720, 4);
  CHECK_EQ(t.tile_rows, 192);
  CHECK_EQ(t.count, 4);

  // 1080p: TWO tiles of 672 rows at 1x, four at 2x. (4x: 160 rows, ceil(1080 / 160) = 7 tiles.)
  t = sz::GameSceneTiling(1920, 1080, 1);
  CHECK_TILING(t, 672, 2, 24 * 42);
  CHECK_EQ(t.surface_width, 1920);
  t = sz::GameSceneTiling(1920, 1080, 2);
  CHECK_EQ(t.tile_rows, 320);
  CHECK_EQ(t.count, 4);
  t = sz::GameSceneTiling(1920, 1080, 4);
  CHECK_EQ(t.tile_rows, 160);
  CHECK_EQ(t.count, 7);
  t = sz::GameSceneTiling(1600, 900, 1);
  CHECK_EQ(t.tile_rows, 800);
  CHECK_EQ(t.count, 2);

  // An overlap makes more tiles of the same height; one as tall as the tile leaves nothing to draw with.
  t = sz::GameSceneTiling(1920, 1080, 4, 16);
  CHECK_EQ(t.tile_rows, 160);
  CHECK_EQ(t.count, 8);
  t = sz::GameSceneTiling(1920, 1080, 4, 160);
  CHECK_EQ(t.tile_rows, 0);
  CHECK_EQ(t.count, 0);
}

// ---- the whole target as one tile in a larger host EDRAM -------------------------------------------------------

void TestSingleTile() {
  CHECK_EQ(sz::SingleTileSurfaceTiles(1280, 720, 1), 16 * 46);  // 720 rounded up to 736
  CHECK_EQ(sz::SingleTileSurfaceTiles(1280, 720, 2), 16 * 92);  // Renan: 1,472 tiles a surface at 2x
  CHECK_EQ(sz::SingleTileSurfaceTiles(1920, 1080, 1), 24 * 68);
  CHECK_EQ(sz::SingleTileSurfaceTiles(1920, 1080, 2), 24 * 136);

  CHECK(sz::SingleTileFits(1280, 720, 2, 4096));    // 2 x 1472
  CHECK(!sz::SingleTileFits(1280, 720, 2, 2048));
  CHECK(!sz::SingleTileFits(1280, 720, 4, 4096));   // 2 x 2944 > 4096, and 2944 > 2048
  CHECK(sz::SingleTileFits(1920, 1080, 1, 4096));   // 2 x 1632 = 3264
  CHECK(!sz::SingleTileFits(1920, 1080, 1, 2048));
  CHECK(!sz::SingleTileFits(1920, 1080, 2, 4096));  // one surface of 3264 tiles is past the console's 2048
  CHECK(sz::SingleTileFits(1600, 900, 1, 4096));
}

// ---- the shared render-target pool (WIDESCREEN.md 13.2) --------------------------------------------------------

void TestPoolGrowth() {
  // The shipped size: no growth, rounded up to the 64 KiB granule (the hook never scales 1280x720).
  const sz::Ratio hd = sz::PoolGrowth(1280, 720);
  CHECK(hd.num == hd.den);
  CHECK_EQ(sz::ScaledPoolBytes(sz::kHdPoolBytes, hd), 0x9E0000);  // 0x9D8000 up to 64 KiB

  CHECK_EQ(sz::ScaledPoolBytes(sz::kHdPoolBytes, sz::PoolGrowth(1560, 720)), 14483456);  // the 13.2 / 13.1 log line
  CHECK_EQ(sz::ScaledPoolBytes(sz::kHdPoolBytes, sz::PoolGrowth(1360, 720)), 12386304);  // x1.2

  // 1080p: the k = 8 target (256 x 160 against 160 x 96, x2.667) outgrows the screen's area (x2.25).
  const sz::Ratio fhd = sz::PoolGrowth(1920, 1080);
  CHECK(fhd.num * 15360 == fhd.den * 40960);
  CHECK_EQ(sz::ScaledPoolBytes(sz::kHdPoolBytes, fhd), 27525120);

  // 1600x900: k = 8 again, 224 x 128 against 160 x 96 (x1.867).
  const sz::Ratio hdp = sz::PoolGrowth(1600, 900);
  CHECK(hdp.num * 15360 == hdp.den * 28672);
  CHECK_EQ(sz::ScaledPoolBytes(sz::kHdPoolBytes, hdp), 19267584);
}

// ---- the front-buffer block and the zero-fill overrun (WIDESCREEN.md 14.1) -------------------------------------

void TestTextureBlock() {
  CHECK_EQ(sz::TextureBlockUnit(1280, 720), 0x398000);
  CHECK_EQ(sz::TextureBlockUnit(1360, 720), 0x3DD000);
  CHECK_EQ(sz::TextureBlockUnit(1400, 720), 0x3F4000);
  CHECK_EQ(sz::TextureBlockUnit(1440, 720), 0x40B000);
  CHECK_EQ(sz::TextureBlockUnit(1560, 720), 0x467000);
  CHECK_EQ(sz::TextureBlockUnit(1920, 1080), 0x7F8000);
  CHECK_EQ(sz::TextureClearOvershoot(1920, 1080), 0xFF0000);  // 2 S

  // The overshoot reaches the heap's first node from 1432 on (1400 stays short of it).
  CHECK(!sz::TextureClearReachesHeap(1280, 720));
  CHECK(!sz::TextureClearReachesHeap(1360, 720));
  CHECK(!sz::TextureClearReachesHeap(1400, 720));
  CHECK(sz::TextureClearReachesHeap(1432, 720));
  CHECK(sz::TextureClearReachesHeap(1440, 720));
  CHECK(sz::TextureClearReachesHeap(1560, 720));
  CHECK(sz::TextureClearReachesHeap(1920, 1080));
}

// ---- SDK cvar, aspect ------------------------------------------------------------------------------------------

void TestRenderTargetMaxHeight() {
  CHECK_EQ(sz::RenderTargetMaxHeight(720), 720);   // the PC profile's 1024 covers it
  CHECK_EQ(sz::RenderTargetMaxHeight(900), 912);
  CHECK_EQ(sz::RenderTargetMaxHeight(1080), 1088);  // not a multiple of the 16-row tile: rounded up
}

void TestAspect() {
  CHECK(sz::IsSixteenNine(1920, 1080) && sz::IsSixteenNine(1280, 720) && sz::IsSixteenNine(1600, 900) &&
        sz::IsSixteenNine(2560, 1440));
  CHECK(!sz::IsSixteenNine(1560, 720) && !sz::IsSixteenNine(1920, 1200) && !sz::IsSixteenNine(1600, 720));
  CHECK(!sz::IsSixteenNine(1920, 0));
  CHECK(sz::AspectOf(1920, 1080) == double(16.0f / 9.0f));  // the game's own float constant at 0x8201E7EC
  CHECK(sz::AspectOf(1920, 0) == 0.0);
  CHECK(sz::UiBlockWidth(720) == 1280.0);  // the Flash stage's 16:9 block of the full height
  CHECK(sz::UiBlockWidth(1080) == 1920.0);
}

// ---- Resolve: the cvars and the video mode -> the back buffer --------------------------------------------------

void TestResolveWidth720() {
  // The "W x 720" world the shipped profiles use, unchanged.
  CHECK(Off(sz::Resolve({0, 0}, {1920, 1080}), sz::Reason::kWidthZero));
  CHECK(Off(sz::Resolve({0, 1080}, {1920, 1080}), sz::Reason::kWidthZero));  // a height alone does nothing

  CHECK(On(sz::Resolve({1560, 0}, {1560, 720}), 1560, 720));
  CHECK(On(sz::Resolve({1560, 0}, {1280, 720}), 1560, 720));  // the mode is not W: H stays 720
  CHECK(On(sz::Resolve({1600, 0}, {0, 0}), 1600, 720));       // mode unknown
  CHECK(On(sz::Resolve({1561, 0}, {1560, 720}), 1560, 720, sz::kNoteWidthRounded));
  CHECK(On(sz::Resolve({1288, 0}, {1288, 720}), 1288, 720));  // the narrowest: just above 1280

  CHECK(Rejected(sz::Resolve({1287, 0}, {1280, 720}), sz::Reason::kWidthRange));  // 1280 after rounding
  CHECK(Rejected(sz::Resolve({1280, 0}, {1280, 720}), sz::Reason::kWidthRange));  // the shipped size is "off", not "on"
}

void TestResolveWidthAndHeight() {
  // PLAN section 6 M6: all four values explicit.
  CHECK(On(sz::Resolve({1920, 1080}, {1920, 1080}), 1920, 1080));
  CHECK(On(sz::Resolve({1600, 900}, {1600, 900}), 1600, 900));
  CHECK(On(sz::Resolve({1920, 1081}, {1920, 1080}), 1920, 1080, sz::kNoteHeightRounded));

  // mcla_scene_height=0: the height comes from the video mode when its width is W.
  CHECK(On(sz::Resolve({1920, 0}, {1920, 1080}), 1920, 1080, sz::kNoteHeightFromMode));
  CHECK(On(sz::Resolve({1920, 0}, {1600, 900}), 1920, 720));  // another width: 720 (the hook warns about the mode)
  CHECK(On(sz::Resolve({1920, 0}, {1920, 1440}), 1920, 720, sz::kNoteModeHeightIgnored));  // mode height out of range

  // Rejected, not clamped.
  CHECK(Rejected(sz::Resolve({1920, 1085}, {1920, 1080}), sz::Reason::kHeightRange));
  CHECK(Rejected(sz::Resolve({1920, 1084}, {1920, 1080}), sz::Reason::kHeightRange));  // 1084 > 1080
  CHECK(Rejected(sz::Resolve({1920, 719}, {1920, 1080}), sz::Reason::kHeightRange));
  CHECK(Rejected(sz::Resolve({1920, -5}, {1920, 1080}), sz::Reason::kHeightNegative));
  CHECK(Rejected(sz::Resolve({1929, 1080}, {0, 0}), sz::Reason::kWidthRange));  // 1928 after rounding
  CHECK(Rejected(sz::Resolve({-2, 0}, {1920, 1080}), sz::Reason::kWidthNegative));
  CHECK(Rejected(sz::Resolve({2560, 1200}, {2560, 1200}), sz::Reason::kWidthRange));  // M7 is not selected by M6

  // M7 limits, reserved: 2560 x 1200 is accepted only when a build asks for them.
  CHECK(On(sz::Resolve({2560, 1200}, {2560, 1200}, sz::kLimitsM7), 2560, 1200));

  // Aspect: exactly 4:3 is the narrowest; narrower is refused (the game's own widescreen branch is not taken).
  CHECK(On(sz::Resolve({1440, 1080}, {1440, 1080}), 1440, 1080));
  CHECK(Rejected(sz::Resolve({1432, 1080}, {1432, 1080}), sz::Reason::kTooNarrow));
}

void TestResolveAuto() {
  // mcla_widescreen_width=-1: the video mode (--resolution=WxH) is the back buffer.
  CHECK(On(sz::Resolve({-1, 0}, {1920, 1080}), 1920, 1080, sz::kNoteHeightFromMode));
  CHECK(On(sz::Resolve({-1, 0}, {1560, 720}), 1560, 720));
  CHECK(On(sz::Resolve({-1, 1080}, {1920, 720}), 1920, 1080));  // an explicit height replaces the mode's

  // Nothing to do: the game keeps its own size, which is not an error.
  CHECK(Off(sz::Resolve({-1, 0}, {1280, 720}), sz::Reason::kAutoModeNotLarger));
  CHECK(Off(sz::Resolve({-1, 0}, {1280, 1024}), sz::Reason::kAutoModeNotLarger));
  CHECK(Off(sz::Resolve({-1, 0}, {1920, 600}), sz::Reason::kAutoModeNotLarger));  // below 720 lines
  CHECK(Off(sz::Resolve({-1, 0}, {0, 0}), sz::Reason::kAutoModeUnknown));

  // A mode this build cannot honour is an error.
  CHECK(Rejected(sz::Resolve({-1, 0}, {3200, 2000}), sz::Reason::kWidthRange));
  CHECK(Rejected(sz::Resolve({-1, 0}, {1920, 1200}), sz::Reason::kHeightRange));
}

void TestDescribe() {
  CHECK(std::strcmp(sz::Describe(sz::Reason::kNone), "") == 0);
  const int last = static_cast<int>(sz::Reason::kTooNarrow);
  for (int i = 1; i <= last; ++i) {
    const char* text = sz::Describe(static_cast<sz::Reason>(i));
    CHECK(text != nullptr && text[0] != '\0');
    for (int j = i + 1; j <= last; ++j) {
      CHECK(std::strcmp(text, sz::Describe(static_cast<sz::Reason>(j))) != 0);  // a log line says which one
    }
  }
}

}  // namespace

int main() {
  TestDeviceSurfaceTiles();
  TestGameSceneTiling();
  TestSingleTile();
  TestPoolGrowth();
  TestTextureBlock();
  TestRenderTargetMaxHeight();
  TestAspect();
  TestResolveWidth720();
  TestResolveWidthAndHeight();
  TestResolveAuto();
  TestDescribe();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
