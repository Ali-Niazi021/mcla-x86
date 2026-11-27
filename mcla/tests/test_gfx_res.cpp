// Unit test of the 2000p cost-lever arithmetic (mcla/hooks/mcla_gfx_res_math.h, R2 and R3 of docs/native/ROADMAP.md): the sun-shadow
// map's accepted edges (multiples of the texture and EDRAM tiles, the game's own EDRAM-fit check), the compensation of a gfx
// preset's shadow size at draw resolution scale 2, the environment map's sizes, and the governor's VRS rungs above a base level.
// Standalone: no SDK, no game, no guest memory.
//
// Every test is a constexpr function that returns 0, or the line of its first failed check, and is also static_assert-ed: the
// compiler runs it, so `clang++ -std=c++23 -fsyntax-only -I mcla/hooks mcla/tests/test_gfx_res.cpp` IS a test run (what a machine
// without the slot is allowed to do). The program repeats the calls at run time and prints the result.
//
// Built and run by CTest (mcla/tests/CMakeLists.txt, test mcla_gfx_res_test). By hand:
//   clang++ -std=c++23 -O1 -Wall -Wextra -I mcla/hooks mcla/tests/test_gfx_res.cpp -o <scratch>/test_gfx_res && <scratch>/test_gfx_res
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "mcla_gfx_res_math.h"

namespace gr = mcla::gfxres;

namespace {

#define CHECK(cond)    \
  do {                 \
    if (!(cond)) {     \
      return __LINE__; \
    }                  \
  } while (0)

#define CHECK_EQ(got, want)                                   \
  do {                                                        \
    if (static_cast<long long>(got) != static_cast<long long>(want)) { \
      return __LINE__;                                        \
    }                                                         \
  } while (0)

constexpr int TestShadowEdges() {
  // The cvar's values: 0 = shipped, the accepted edges as they are, everything else = shipped (never a made-up size).
  CHECK_EQ(gr::ShadowEdgeOf(0), 640);
  CHECK_EQ(gr::ShadowEdgeOf(640), 640);
  CHECK_EQ(gr::ShadowEdgeOf(512), 512);
  CHECK_EQ(gr::ShadowEdgeOf(384), 384);
  CHECK_EQ(gr::ShadowEdgeOf(320), 320);
  CHECK_EQ(gr::ShadowEdgeOf(256), 256);
  CHECK_EQ(gr::ShadowEdgeOf(192), 192);
  for (int32_t bad : {-1, 1, 100, 128, 300, 321, 448, 600, 641, 1024, 1280}) {
    CHECK_EQ(gr::ShadowEdgeOf(bad), 640);
    CHECK(!gr::IsShadowEdge(bad));
  }
  // Every accepted edge is tile-aligned (texture tile 32, EDRAM tile rows 16) and fits the game's own EDRAM check.
  for (int32_t edge : {640, 512, 384, 320, 256, 192}) {
    CHECK(gr::IsShadowEdge(edge));
    CHECK(gr::ShadowEdgeIsTiled(uint32_t(edge)));
    CHECK(gr::ShadowFitsEdram(uint32_t(edge)));
  }
  CHECK(!gr::ShadowEdgeIsTiled(0));
  CHECK(!gr::ShadowEdgeIsTiled(300));
  // The check's numbers, as in the header of mcla_gfx_shadow_res.cpp.
  CHECK_EQ((640u >> 4) * (640u >> 3), 3200);
  CHECK_EQ((512u >> 4) * (512u >> 3), 2048);
  CHECK_EQ((384u >> 4) * (384u >> 3), 1152);
  CHECK_EQ((320u >> 4) * (320u >> 3), 800);
  CHECK(gr::ShadowFitsEdram(640));
  CHECK(!gr::ShadowFitsEdram(1280));  // what 640 would be at the host scale: the check is on the GUEST size
  CHECK(!gr::ShadowFitsEdram(768));
  // The 2000p profile's value: 320 at scale 2 is the host 640 of the shipped map.
  CHECK_EQ(gr::ShadowEdgeOf(320) * 2, gr::kShadowShippedEdge);
  return 0;
}

constexpr int TestShadowAtScale() {
  // Scale 1 (and any scale but the square 2): untouched, bit-identical to before.
  for (int32_t v : {0, 640, 512, 384, 320}) {
    CHECK_EQ(gr::ShadowEdgeAtScale(v, 1, 1), v);
    CHECK_EQ(gr::ShadowEdgeAtScale(v, 0, 0), v);
    CHECK_EQ(gr::ShadowEdgeAtScale(v, 3, 3), v);
    CHECK_EQ(gr::ShadowEdgeAtScale(v, 2, 1), v);
    CHECK_EQ(gr::ShadowEdgeAtScale(v, 1, 2), v);
  }
  // Scale 2x2: the preset values name the host look, the guest edge is half of it.
  CHECK_EQ(gr::ShadowEdgeAtScale(0, 2, 2), 320);    // quality / ultra: the shipped 640 host
  CHECK_EQ(gr::ShadowEdgeAtScale(640, 2, 2), 320);
  CHECK_EQ(gr::ShadowEdgeAtScale(512, 2, 2), 256);  // perf: 512 host
  CHECK_EQ(gr::ShadowEdgeAtScale(384, 2, 2), 192);
  // A value that would not be an accepted edge stays as it is (320 would become 160).
  CHECK_EQ(gr::ShadowEdgeAtScale(320, 2, 2), 320);
  CHECK_EQ(gr::ShadowEdgeAtScale(999, 2, 2), 999);
  // The host target is the same as the same preset at scale 1.
  for (int32_t v : {0, 640, 512, 384}) {
    CHECK_EQ(2 * gr::ShadowEdgeOf(gr::ShadowEdgeAtScale(v, 2, 2)), gr::ShadowEdgeOf(v));
  }
  return 0;
}

constexpr int TestEnv() {
  // As shipped for 0 and for anything unknown.
  for (int32_t v : {0, 512, -1, 1, 128, 300, 1024}) {
    const gr::EnvSize s = gr::EnvSizeOf(v);
    CHECK_EQ(s.width, 512);
    CHECK_EQ(s.height, 256);
  }
  const gr::EnvSize half = gr::EnvSizeOf(256);
  CHECK_EQ(half.width, 256);
  CHECK_EQ(half.height, 128);
  const gr::EnvSize mid = gr::EnvSizeOf(384);
  CHECK_EQ(mid.width, 384);
  CHECK_EQ(mid.height, 192);
  for (int32_t v : {512, 384, 256}) {
    CHECK(gr::IsEnvWidth(v));
    CHECK(gr::EnvSizeIsTiled(gr::EnvSizeOf(v)));  // 2:1, width a multiple of 32, height of 16
  }
  CHECK(!gr::IsEnvWidth(0));
  CHECK(!gr::EnvSizeIsTiled(gr::EnvSize{500, 250}));
  // Half the width at draw scale 2 is the shipped host size.
  CHECK_EQ(2 * gr::EnvSizeOf(256).width, gr::kEnvShippedWidth);
  CHECK_EQ(2 * gr::EnvSizeOf(256).height, gr::kEnvShippedHeight);
  return 0;
}

constexpr int TestVrsRungs() {
  // Base 0: what the governor always did (min of the cvar and what the SDK has).
  CHECK_EQ(gr::VrsRungs(2, 2, 0), 2);
  CHECK_EQ(gr::VrsRungs(1, 2, 0), 1);
  CHECK_EQ(gr::VrsRungs(0, 2, 0), 0);
  CHECK_EQ(gr::VrsRungs(2, 0, 0), 0);  // no VRS on this GPU
  CHECK_EQ(gr::VrsRungs(8, 2, 0), 2);
  CHECK_EQ(gr::VrsRungs(2, 3, 0), 2);
  // A base level uses up the table: the 2000p profile's base 2 of the SDK's two levels leaves no rung.
  CHECK_EQ(gr::VrsRungs(2, 2, 2), 0);
  CHECK_EQ(gr::VrsRungs(2, 2, 1), 1);
  CHECK_EQ(gr::VrsRungs(1, 3, 1), 1);
  CHECK_EQ(gr::VrsRungs(2, 3, 1), 2);
  CHECK_EQ(gr::VrsRungs(2, 1, 2), 0);  // a base beyond the table (the SDK clamps it): nothing above
  CHECK_EQ(gr::VrsRungs(2, 0, 2), 0);
  CHECK_EQ(gr::VrsRungs(2, 2, -1), 2);  // a negative base is 0
  // The level written for a rung: -1 = the SDK's cvar (the base) decides; else base + rung, never below the base.
  CHECK_EQ(gr::VrsWriteLevel(0, 0), -1);
  CHECK_EQ(gr::VrsWriteLevel(0, 2), -1);
  CHECK_EQ(gr::VrsWriteLevel(1, 0), 1);
  CHECK_EQ(gr::VrsWriteLevel(2, 0), 2);
  CHECK_EQ(gr::VrsWriteLevel(1, 1), 2);
  CHECK_EQ(gr::VrsWriteLevel(1, 2), 3);
  CHECK_EQ(gr::VrsWriteLevel(1, -3), 1);
  // The written level of every rung is within the table, and never finer than the base.
  for (int sdk = 0; sdk <= 4; ++sdk) {
    for (int base = 0; base <= 5; ++base) {
      for (int configured = 0; configured <= 4; ++configured) {
        const int rungs = gr::VrsRungs(configured, sdk, base);
        CHECK(rungs >= 0 && rungs <= configured);
        for (int rung = 1; rung <= rungs; ++rung) {
          const int level = gr::VrsWriteLevel(rung, base);
          CHECK(level > base);
          CHECK(level <= sdk);
        }
      }
    }
  }
  return 0;
}

// The compiler runs the tests (see the header of this file).
static_assert(TestShadowEdges() == 0, "shadow edges: the line of the failed check is TestShadowEdges()'s value");
static_assert(TestShadowAtScale() == 0, "shadow edge at draw scale 2");
static_assert(TestEnv() == 0, "environment map size");
static_assert(TestVrsRungs() == 0, "governor VRS rungs above a base level");

int Report(const char* name, int line) {
  if (line != 0) {
    std::printf("FAIL %s: check at line %d\n", name, line);
    return 1;
  }
  return 0;
}

}  // namespace

int main() {
  int failures = 0;
  failures += Report("shadow edges", TestShadowEdges());
  failures += Report("shadow edge at draw scale 2", TestShadowAtScale());
  failures += Report("environment map size", TestEnv());
  failures += Report("governor VRS rungs", TestVrsRungs());
  std::printf("4 tests, %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
