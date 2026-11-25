// Shared state of the widescreen hooks (see mcla_ws_common.h). Always compiled, not a hook.
#include <atomic>
#include <cstdint>
#include <cstring>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xvideo.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"
#include "mcla_ws_common.h"

REXCVAR_DEFINE_INT32(mcla_widescreen_width, 0, "MCLA",
                     "MCLA widescreen master switch (docs/mcla/WIDESCREEN.md). 0 = off, the game's own "
                     "1280x720 (default). 1288..1920 = a real W x H back buffer with a Hor+ camera (H is "
                     "mcla_scene_height; also pass --resolution=WxH so the presenter does not stretch it). "
                     "-1 = auto: W x H is the video mode's size when --resolution gave a mode larger than "
                     "1280x720. A value that cannot be honoured keeps widescreen off and logs "
                     "'[mcla-wide] error:'. Read once, at device init");
REXCVAR_DEFINE_INT32(mcla_scene_height, 0, "MCLA",
                     "MCLA widescreen: the back buffer's height with mcla_widescreen_width. 0 = derived: the "
                     "video mode's height when the width is auto or equals the mode's width, else 720 (the "
                     "original W x 720). Otherwise 720..1080, rounded down to a multiple of 4. Ignored while "
                     "mcla_widescreen_width is 0. mcla_profile=1080p sets 1920x1080. Read once, at device "
                     "init");
REXCVAR_DEFINE_INT32(mcla_ws_aspect_mask, 31, "MCLA",
                     "MCLA widescreen: which 16:9 loads become W/H: 1 scene camera, 2 rear-view mirror, "
                     "4 sub_82214AC0, 8 sub_8223E230, 16 pause-map box. 31 = all (A/B: 1, 3, 15). Idle "
                     "whatever the mask when the back buffer is exactly 16:9 (the game's constants are right)");
REXCVAR_DEFINE_BOOL(mcla_ws_cutscene_fov, true, "MCLA",
                    "MCLA widescreen: cutscene cameras keep their 16:9 vertical angle (Hor+) instead of "
                    "zooming in; only with widescreen on and a back buffer that is not exactly 16:9");
REXCVAR_DEFINE_BOOL(mcla_ws_rt_pool_scale, true, "MCLA",
                    "MCLA widescreen: grow the renderer's shared render-target pool with the back "
                    "buffer (it is a fixed 10.3 MB sized for 1280x720; default factor = the area ratio, "
                    "Xenia's x2.25 at 1920x1080 = 23,224,320 bytes; see mcla_ws_rt_pool_percent); only with "
                    "widescreen on");
REXCVAR_DEFINE_INT32(mcla_ws_rt_pool_percent, 0, "MCLA",
                     "MCLA widescreen: the render-target pool as a percentage of the shipped 10.3 MB. 0 = auto: "
                     "the area ratio of the back buffer to 1280x720 (225 at 1920x1080, Xenia's value). 267 = the "
                     "conservative x2.667 (the largest growth of a 32-texel-aligned target) that the 1920x1080 boot "
                     "ran out of guest physical memory with. Read at the renderer's creation; the physical-pool "
                     "hook (mcla_ws_phys_pool_reduce_mib) pays for whatever it is");

REXCVAR_DEFINE_BOOL(mcla_ws_rt_pool_guard, true, "MCLA",
                    "MCLA widescreen: a render-target texture that does not fit in what is left of the "
                    "renderer's shared pool gets its own allocation (the game's own no-pool path) instead "
                    "of being placed past the pool's end (the pool's allocator has no bounds check); "
                    "only with widescreen on");
REXCVAR_DEFINE_INT32(mcla_ws_edram_reuse, 1, "MCLA",
                     "MCLA widescreen: the D3D EDRAM tile allocator has no room for the game's small targets "
                     "beside the two wide device surfaces when fewer than 608 tiles (the shipped game's "
                     "1280x720 margin) are left of the limit in force (2048, or edram_tiles with the limit "
                     "hook). 1 (default) = then give the tiles of the device depth surface back to the "
                     "allocator, 2 = the tiles of the device colour surface instead (A/B), 0 = off (RAGE fatal "
                     "'Unable to create ... render target' above about 1500 px at 2048 tiles). Only with "
                     "widescreen on; read at device init");
REXCVAR_DEFINE_BOOL(mcla_ws_edram_log, true, "MCLA",
                    "MCLA widescreen: log every D3D surface the game creates (size, format, MSAA, EDRAM "
                    "tiles, who placed it) for the first 160; only with widescreen on");

namespace mcla::ws {
namespace {

std::atomic<uint32_t> g_width{0};
std::atomic<uint32_t> g_height{0};
std::atomic<bool> g_resolved{false};
std::atomic<uint32_t> g_edram_limit{sizing::kConsoleEdramTiles};

}  // namespace

uint32_t Width() {
  if (!g_resolved.load(std::memory_order_acquire)) {
    return 0;
  }
  return g_width.load(std::memory_order_relaxed);
}

uint32_t Height() {
  if (!g_resolved.load(std::memory_order_acquire)) {
    return 0;
  }
  return g_height.load(std::memory_order_relaxed);
}

bool Armed() { return REXCVAR_GET(mcla_widescreen_width) != 0; }

double Aspect() {
  const uint32_t w = Width();
  const uint32_t h = Height();
  if (w == 0 || h == 0) {
    return 0.0;
  }
  // The game's constants are single-precision floats: round the same way.
  return sizing::AspectOf(w, h);
}

bool SixteenNine() { return sizing::IsSixteenNine(Width(), Height()); }

bool AspectFor(int32_t bit, double* aspect) {
  const double a = Aspect();
  if (a == 0.0 || SixteenNine() || (REXCVAR_GET(mcla_ws_aspect_mask) & bit) == 0) {
    return false;
  }
  *aspect = a;
  return true;
}

double UiBlockWidth() {
  const uint32_t h = Height();
  return h == 0 ? 0.0 : sizing::UiBlockWidth(h);
}

sizing::Resolution ResolveRequest(uint32_t mode_width, uint32_t mode_height) {
  return sizing::Resolve({REXCVAR_GET(mcla_widescreen_width), REXCVAR_GET(mcla_scene_height)},
                         {mode_width, mode_height});
}


sizing::Resolution PlannedResolution() {
  static const sizing::Resolution planned = [] {
    rex::system::X_VIDEO_MODE mode{};
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    return ResolveRequest(uint32_t(mode.display_width), uint32_t(mode.display_height));
  }();
  return planned;
}

uint64_t PoolBytesFor(uint32_t shipped, uint32_t width, uint32_t height) {
  const int32_t percent = REXCVAR_GET(mcla_ws_rt_pool_percent);
  if (percent > 0) {
    return sizing::ScaledPoolBytes(shipped, sizing::Ratio{uint64_t(percent), 100});
  }
  return sizing::AreaPoolBytes(shipped, width, height);
}

bool PhysicalMemory(uint64_t* free_bytes, uint64_t* total_bytes) {
  rex::system::KernelState* kernel = rex::system::kernel_state();
  rex::memory::Memory* memory = kernel ? kernel->memory() : nullptr;
  rex::memory::BaseHeap* heap = memory ? memory->GetPhysicalHeap() : nullptr;
  if (!heap) {
    return false;
  }
  const uint64_t page = heap->page_size();
  *total_bytes = uint64_t(heap->GetTotalPageCount()) * page;
  *free_bytes = uint64_t(heap->GetUnreservedPageCount()) * page;
  return true;
}

void LogPhysicalMemory(const char* where) {
  uint64_t free_bytes = 0, total_bytes = 0;
  if (!PhysicalMemory(&free_bytes, &total_bytes)) {
    return;
  }
  REXLOG_INFO("[mcla-wide] guest physical memory at {}: {:.1f} MiB free of {:.1f} MiB", where,
              double(free_bytes) / 1048576.0, double(total_bytes) / 1048576.0);
}

void Publish(uint32_t width, uint32_t height) {
  const bool on = width != 0 && height != 0;
  g_width.store(on ? width : 0, std::memory_order_relaxed);
  g_height.store(on ? height : 0, std::memory_order_relaxed);
  g_resolved.store(true, std::memory_order_release);
}

uint32_t HostEdramTiles() {
  rex::Runtime* runtime = rex::Runtime::instance();
  rex::system::IGraphicsSystem* graphics = runtime ? runtime->graphics_system() : nullptr;
  return graphics ? graphics->edram_tile_count() : 0;
}

uint32_t EdramLimitInForce() { return g_edram_limit.load(std::memory_order_relaxed); }

void NoteEdramLimit(uint32_t tiles) { g_edram_limit.store(tiles, std::memory_order_relaxed); }

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
