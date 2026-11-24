// Shared state of the widescreen hooks (see mcla_ws_common.h). Always compiled, not a hook.
#include <atomic>
#include <cstdint>
#include <cstring>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"
#include "mcla_ws_common.h"

REXCVAR_DEFINE_INT32(mcla_widescreen_width, 0, "MCLA",
                     "MCLA widescreen master switch (docs/mcla/WIDESCREEN.md). 0 = off, the game's own "
                     "1280x720 (default). 1288..1600 = a real W x 720 back buffer with a Hor+ camera "
                     "(also pass --resolution=Wx720 so the presenter does not stretch it). -1 = auto: "
                     "W is the video mode's width when --resolution gave a mode wider than 1280 and "
                     "720 high. Read once, at device init");
REXCVAR_DEFINE_INT32(mcla_ws_aspect_mask, 31, "MCLA",
                     "MCLA widescreen: which 16:9 loads become W/H: 1 scene camera, 2 rear-view mirror, "
                     "4 sub_82214AC0, 8 sub_8223E230, 16 pause-map box. 31 = all (A/B: 1, 3, 15)");
REXCVAR_DEFINE_BOOL(mcla_ws_cutscene_fov, true, "MCLA",
                    "MCLA widescreen: cutscene cameras keep their 16:9 vertical angle (Hor+) instead of "
                    "zooming in; only with widescreen on");
REXCVAR_DEFINE_BOOL(mcla_ws_rt_pool_scale, true, "MCLA",
                    "MCLA widescreen: grow the renderer's shared render-target pool with the back "
                    "buffer (it is a fixed 10.3 MB sized for 1280x720; factor = the largest growth of a "
                    "32-texel-aligned target, 1.4 at 1560); only with widescreen on");

REXCVAR_DEFINE_BOOL(mcla_ws_rt_pool_guard, true, "MCLA",
                    "MCLA widescreen: a render-target texture that does not fit in what is left of the "
                    "renderer's shared pool gets its own allocation (the game's own no-pool path) instead "
                    "of being placed past the pool's end (the pool's allocator has no bounds check); "
                    "only with widescreen on");
REXCVAR_DEFINE_INT32(mcla_ws_edram_reuse, 1, "MCLA",
                     "MCLA widescreen: the D3D EDRAM tile allocator (2048 tiles, no overlap) has no room "
                     "for the game's small targets beside the two wide device surfaces. 1 (default) = "
                     "give the tiles of the device depth surface back to the allocator, 2 = the tiles of "
                     "the device colour surface instead (A/B), 0 = off (RAGE fatal 'Unable to create ... "
                     "render target' above about 1500 px). Only with widescreen on; read at device init");
REXCVAR_DEFINE_BOOL(mcla_ws_edram_log, true, "MCLA",
                    "MCLA widescreen: log every D3D surface the game creates (size, format, MSAA, EDRAM "
                    "tiles, who placed it) for the first 160; only with widescreen on");

namespace mcla::ws {
namespace {

std::atomic<uint32_t> g_width{0};
std::atomic<bool> g_resolved{false};

}  // namespace

uint32_t Width() {
  if (!g_resolved.load(std::memory_order_acquire)) {
    return 0;
  }
  return g_width.load(std::memory_order_relaxed);
}

bool Armed() { return REXCVAR_GET(mcla_widescreen_width) != 0; }

double Aspect() {
  const uint32_t w = Width();
  if (w == 0) {
    return 0.0;
  }
  // The game's constants are single-precision floats: round the same way.
  return double(float(w) / float(kSceneHeight));
}

bool AspectFor(int32_t bit, double* aspect) {
  const double a = Aspect();
  if (a == 0.0 || (REXCVAR_GET(mcla_ws_aspect_mask) & bit) == 0) {
    return false;
  }
  *aspect = a;
  return true;
}

uint32_t ResolveAtDeviceInit(uint32_t mode_width, uint32_t mode_height, const char** why) {
  const char* reason = nullptr;
  uint32_t w = 0;
  const int32_t request = REXCVAR_GET(mcla_widescreen_width);
  if (request == 0) {
    reason = "mcla_widescreen_width=0";
  } else if (request == -1) {
    if (mode_width > kBaseWidth && mode_height == kSceneHeight) {
      w = mode_width;
    } else {
      reason = "auto: the video mode is not wider than 1280 and 720 high (pass --resolution=Wx720)";
    }
  } else if (request > 0) {
    w = uint32_t(request);
  } else {
    reason = "negative value other than -1";
  }
  if (w != 0) {
    const uint32_t rounded = w & ~7u;
    if (rounded < kMinWidth || rounded > kMaxWidth) {
      reason = "width outside 1288..1600";
      w = 0;
    } else {
      if (rounded != w) {
        reason = "width rounded down to a multiple of 8";
      }
      w = rounded;
    }
  }
  g_width.store(w, std::memory_order_relaxed);
  g_resolved.store(true, std::memory_order_release);
  if (why) {
    *why = reason;
  }
  return w;
}

bool GuestScreenSize(uint32_t* width, uint32_t* height) {
  uint32_t w = 0, h = 0;
  if (!guest::U32(0x827D4274, &w) || !guest::U32(0x827D4278, &h)) {
    return false;
  }
  *width = w;
  *height = h;
  return true;
}

bool WriteImageU32(uint32_t address, uint32_t value) {
  if (!guest::InImage(address) || (address & 3) != 0) {
    return false;
  }
  uint32_t probe = 0;
  if (!guest::U32(address, &probe)) {
    return false;
  }
  rex::system::KernelState* kernel = rex::system::kernel_state();
  rex::memory::Memory* memory = kernel ? kernel->memory() : nullptr;
  if (!memory) {
    return false;
  }
  const uint32_t big_endian = __builtin_bswap32(value);
  std::memcpy(memory->TranslateVirtual<uint8_t*>(address), &big_endian, sizeof(big_endian));
  std::atomic_thread_fence(std::memory_order_release);
  return true;
}

bool WriteGuestU32(uint32_t address, uint32_t value) {
  if ((address & 3) != 0) {
    return false;
  }
  uint32_t probe = 0;
  if (!guest::U32(address, &probe)) {
    return false;
  }
  rex::system::KernelState* kernel = rex::system::kernel_state();
  rex::memory::Memory* memory = kernel ? kernel->memory() : nullptr;
  if (!memory) {
    return false;
  }
  const uint32_t big_endian = __builtin_bswap32(value);
  std::memcpy(memory->TranslateVirtual<uint8_t*>(address), &big_endian, sizeof(big_endian));
  std::atomic_thread_fence(std::memory_order_release);
  return true;
}

}  // namespace mcla::ws
