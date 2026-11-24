// mcla_ws_texclear (W9): the fix for the 1432+ px boot fatal "sysMemSimpleAllocator::Allocate -
// memory node at 0xA8F20010 had guard word trashed" (docs/mcla/WIDESCREEN.md section 14), and the
// probe that shows the memory around that node. Only with widescreen on (mcla_ws_common.h); cvars
// mcla_ws_texclear_clamp (1 default, 2 = also at 1280, 0 = off) and mcla_ws_heap_probe (true).
//
// What goes wrong. The title's XMemAlloc (sub_82130B50) gives the first four physical, 4 KiB
// aligned, allocator-id-128 allocations of the process fixed places in ONE block of 4 x (size of
// the first): k x size, k from the table 0x827D1554 = 1, 3, 2, 0 (block pointer 0x827D1424, counter
// 0x82830B08). They are, in order: front buffer A (0x821784EC), front buffer B (0x82178520), the
// texture of the device colour surface (sub_821840A8 -> sub_82177EB0) and the texture of the device
// depth surface (sub_82183FE0 -> sub_82177EB0), each S = align32(W) x align32(H) x 4 bytes (736 lines
// at 720; mcla_ws_sizing.h TextureBlockUnit), so they sit at block + S, + 3 S, + 2 S and + 0. RAGE's
// texture create (sub_82177EB0) zero-fills a texture it got from D3D with
//     0x82178290  rlwinm r29,r11,12,0,19      r29 = the texture's base address
//     0x82178294  mr r3,r29
//     0x82178298  bl 0x82132600               MmQueryAllocationSize(base)
//     0x8217829C  mr r5,r3                    <- hook 1, BEFORE this: r3 = the size, r29 = the base
//     0x821782A4  mr r3,r29
//     0x821782A8  bl 0x823D9890               memset(base, 0, size)
//     0x821782B4..0x821782CC                  the same for the mip address (r31)   <- hook 2 at 0x821782C0
// For the colour texture the base is INSIDE the block (block + 2 S). Our SDK's
// MmQueryAllocationSize (BaseHeap::QuerySize) answers with the size of the whole allocation for
// any address inside it: 4 S. So the memset runs from block + 2 S for 4 S bytes: 2 S bytes past
// the end of the block. Physical memory is handed out top-down, so what lies after the block is
// what was allocated just before it: D3D's secondary ring buffer (0x800000, 64 KiB pages), its
// primary ring buffer (0x8000) and, 0x810000 bytes after the block's end, the heap of a
// sysMemSimpleAllocator whose first node is at 0xA8F20010. At 1280x720 2 S = 0x730000: the memset stays
// inside the (still empty) secondary ring buffer and nothing is seen. 2 S passes 0x810010 when
// align32(W) x align32(H) exceeds 1.057 million texels (1408 x 736 = 1.036 M boots, 1440 x 736 = 1.060 M
// does not): 1400 boots, 1432 / 1440 / 1560 / 1568 zero the node's guard word, and the next Allocate that
// walks that free list reports it. At 1920x1080 S = 0x7F8000 (8.3 MB), the block is 33 MB and the
// overshoot 2 S = 16.6 MB: twice the reach, always fatal without this clamp (TextureClearReachesHeap).
// The pool sizes play no part. The clamp reads the real allocation, so it needs no knowledge of W and H.
//
// The fix: the size the memset gets is cut to what is left of the allocation from the base (the
// SDK's own region table: allocation base + size - base). That is the answer of a kernel that
// counts pages from the given address to the allocation's end, and it keeps the zero-fill inside
// the block. If the SDK cannot be asked, the game's block word is used: base inside
// (block, block + size) -> size = block + size - base. A size that is already right is left alone,
// so a later SDK fix of MmQueryAllocationSize makes these hooks no-ops.
//
// Probe (mcla_ws_heap_probe): one block of lines at the first texture clear that had to be cut:
// the four fixed places, D3D's ring buffers, the three sysMemSimpleAllocator heaps the image
// creates at boot and which of them the uncut memset would have reached; then, in
// sub_82305230 (the render-target setup, 0x82305430: the game's own "Render Target Pool (%d, '%s')
// has %d bytes free (%d used)" line, a no-op in the shipped build), the use of each slot of the
// shared pool and the state of each heap's first guard word.
//
// Off: --mcla_ws_texclear_clamp=0 / --mcla_ws_heap_probe=false, widescreen off (unless the clamp
// cvar is 2), or MCLA_HOOK_WS_TEXCLEAR=0 at build time.
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"
#include "mcla_ws_common.h"

REXCVAR_DEFINE_INT32(mcla_ws_texclear_clamp, 1, "MCLA",
                     "MCLA widescreen: RAGE zero-fills a new D3D texture over MmQueryAllocationSize(base) "
                     "bytes; for a texture placed inside the title's block of its first four physical "
                     "allocations our SDK answers with the whole block, and the memset runs 2 x (texture "
                     "size) past the block's end (heap guard word trashed from 1432 px). 1 (default) = cut "
                     "the size to the allocation's end when widescreen is on, 2 = always (also at "
                     "1280x720, where the overrun only zeroes D3D's empty ring buffer), 0 = off");
REXCVAR_DEFINE_BOOL(mcla_ws_heap_probe, true, "MCLA",
                    "MCLA widescreen: log the memory around the texture clear that overran (the four fixed "
                    "places, D3D's ring buffers, the simple-allocator heaps and their first guard word) "
                    "and the use of the shared render-target pool's slots; only with widescreen on");

namespace {

mcla::hooks::Registration g_registration("ws-texclear", "mcla_ws_texclear_clamp", MCLA_HOOK_WS_TEXCLEAR != 0,
                                         [] {
                                           const int32_t mode = REXCVAR_GET(mcla_ws_texclear_clamp);
                                           return mode == 2 || (mode == 1 && mcla::ws::Armed());
                                         },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_WS_TEXCLEAR

namespace {

constexpr uint32_t kBlockWord = 0x827D1424;      // BE32 = the block of the first four (0xFFFFFFFF = none yet)
constexpr uint32_t kBlockCount = 0x82830B08;     // BE32 = how many of the four were handed out
constexpr uint32_t kBlockLastSize = 0x828309B4;  // BE32 = the size of the last one handed out
constexpr uint32_t kDeviceWord = 0x82839254;     // BE32 = the D3D device
constexpr uint32_t kPrimaryRing = 14836;         // device +14836: primary ring buffer (sub_82412A60)
constexpr uint32_t kSecondaryRing = 14840;       // device +14840: secondary ring buffer
constexpr uint32_t kCurrentPool = 0x82839ED4;    // BE32 = the current render-target pool
constexpr uint32_t kProbeLines = 8;

// 1 = only with a wide back buffer (the scene-size hook has run and answered W), 2 = always.
bool ClampWanted() {
  const int32_t mode = REXCVAR_GET(mcla_ws_texclear_clamp);
  return mode == 2 || (mode == 1 && mcla::ws::Width() != 0);
}

// A guest address as a physical address (the three physical views share one memory; the 4 KiB
// view at 0xE0000000 starts one page into it).
uint32_t Physical(uint32_t address) {
  return (address & 0x1FFFFFFFu) + (address >= 0xE0000000u ? 0x1000u : 0u);
}

struct Arena {
  const char* name;
  uint32_t object;  // the sysMemSimpleAllocator (0 = not created yet)
  uint32_t heap = 0, first = 0, bytes = 0;
};

// sysMemSimpleAllocator (vtable 0x82011884, init sub_821C2840): +8 heap, +4 first node =
// align32(heap) + 16, +76 bytes from the first node to the heap's end. Node: +0 guard word = its
// own address, +4 size.
bool ReadArena(Arena* arena) {
  uint32_t vtable = 0;
  return arena->object != 0 && mcla::guest::U32(arena->object, &vtable) && vtable == 0x82011884u &&
         mcla::guest::U32(arena->object + 8, &arena->heap) &&
         mcla::guest::U32(arena->object + 4, &arena->first) &&
         mcla::guest::U32(arena->object + 76, &arena->bytes) && arena->heap != 0;
}

// The simple allocators the image creates at boot: the game heap (sub_82131228, 0x82830CD8), the
// ambient heap (static initialiser sub_827B71A8, 0x82909628, 0xB80000 bytes) and the heap XMemAlloc
// uses for allocator id 130 (sub_821E7770, pointer 0x8286CF50, 0x1580000 bytes).
void Arenas(Arena out[3]) {
  out[0] = Arena{"game heap 0x82830CD8", 0x82830CD8u};
  out[1] = Arena{"ambient heap 0x82909628", 0x82909628u};
  uint32_t audio = 0;
  (void)mcla::guest::U32(0x8286CF50u, &audio);
  out[2] = Arena{"id-130 heap [0x8286CF50]", audio};
}

void LogGuardWords(const char* when) {
  Arena arenas[3];
  Arenas(arenas);
  for (Arena& arena : arenas) {
    if (!ReadArena(&arena)) {
      continue;
    }
    uint32_t guard = 0, size = 0;
    const bool read = mcla::guest::U32(arena.first, &guard) && mcla::guest::U32(arena.first + 4, &size);
    REXLOG_INFO("[mcla-ws-heap] {}: {} = 0x{:08X}..0x{:08X} (physical 0x{:08X}), first node 0x{:08X} guard "
                "word {}",
                when, arena.name, arena.heap, arena.first + arena.bytes, Physical(arena.heap), arena.first,
                !read ? "unreadable" : guard == arena.first ? "intact" : "TRASHED");
    if (read && guard != arena.first) {
      REXLOG_WARN("[mcla-ws-heap] {}: node 0x{:08X} holds guard 0x{:08X}, size 0x{:08X}", when, arena.first,
                  guard, size);
    }
  }
}

// What the uncut memset [base, base + asked) would have covered beyond [base, base + left).
void LogLayout(uint32_t base, uint32_t asked, uint32_t left, uint32_t allocation, uint32_t allocation_size) {
  uint32_t block = 0, count = 0, last = 0, device = 0, primary = 0, secondary = 0;
  (void)mcla::guest::U32(kBlockWord, &block);
  (void)mcla::guest::U32(kBlockCount, &count);
  (void)mcla::guest::U32(kBlockLastSize, &last);
  if (mcla::guest::U32(kDeviceWord, &device) && device != 0) {
    (void)mcla::guest::U32(device + kPrimaryRing, &primary);
    (void)mcla::guest::U32(device + kSecondaryRing, &secondary);
  }
  const uint64_t from = uint64_t(Physical(base)) + left;   // first byte past the allocation
  const uint64_t to = uint64_t(Physical(base)) + asked;    // end of the uncut memset
  REXLOG_INFO("[mcla-ws-heap] the first four fixed places: block 0x{:08X} (physical 0x{:08X}), {} handed "
              "out, last size {} (0x{:X}); the texture is at block + 0x{:X}; allocation 0x{:08X} + 0x{:X}",
              block, Physical(block), count, last, last, base - allocation, allocation, allocation_size);
  REXLOG_INFO("[mcla-ws-heap] the uncut memset would have zeroed physical 0x{:08X}..0x{:08X} after the "
              "allocation ({} bytes)",
              uint32_t(from), uint32_t(to), to - from);
  // The area model (mcla_ws_sizing.h): the block of the first four is 4 S, S = align32(W) x align32(H) x 4, and the
  // device colour texture at block + 2 S is cleared 2 S past the block's end. A mismatch means another texture or a
  // block that is not laid out as modelled.
  const uint32_t wide = mcla::ws::Width();
  const uint32_t high = mcla::ws::Height();
  if (wide != 0 && high != 0) {
    const uint64_t unit = mcla::ws::sizing::TextureBlockUnit(wide, high);
    const uint64_t observed = uint64_t(asked) - left;
    REXLOG_INFO("[mcla-ws-heap] sizing model for {}x{}: S = {} bytes, block 4 S = {}, uncut overshoot 2 S = {}; "
                "observed {}: {}",
                wide, high, unit, 4 * unit, 2 * unit, observed,
                observed == 2 * unit ? "matches the model"
                                     : "differs from the model (another texture, or the block is not 4 x S)");
  }
  REXLOG_INFO("[mcla-ws-heap] D3D ring buffers: secondary 0x{:08X} (physical 0x{:08X}, 0x800000 bytes), "
              "primary 0x{:08X} (physical 0x{:08X}, 0x8000 bytes)",
              secondary, Physical(secondary), primary, Physical(primary));
  Arena arenas[3];
  Arenas(arenas);
  for (Arena& arena : arenas) {
    if (!ReadArena(&arena)) {
      REXLOG_INFO("[mcla-ws-heap] {}: not created yet", arena.name);
      continue;
    }
    const uint64_t start = Physical(arena.heap);
    const uint64_t end = uint64_t(Physical(arena.first)) + arena.bytes;
    const bool hit = from < end && to > start;
    REXLOG_INFO("[mcla-ws-heap] {}: 0x{:08X}..0x{:08X} (physical 0x{:08X}..0x{:08X}), first node 0x{:08X}: "
                "{}",
                arena.name, arena.heap, arena.first + arena.bytes, uint32_t(start), uint32_t(end),
                arena.first,
                hit ? (to > start + 16 ? "the uncut memset reaches its first node (the guard-word fatal)"
                                       : "the uncut memset reaches its first bytes")
                    : "out of reach");
  }
}

// The bytes left from `base` to the end of the allocation that holds it, or 0 when unknown.
uint32_t LeftInAllocation(uint32_t base, uint32_t asked, uint32_t* allocation, uint32_t* allocation_size) {
  rex::system::KernelState* kernel = rex::system::kernel_state();
  rex::memory::Memory* memory = kernel ? kernel->memory() : nullptr;
  if (memory) {
    rex::memory::BaseHeap* heap = memory->LookupHeap(base);
    rex::memory::HeapAllocationInfo info{};
    if (heap && heap->QueryRegionInfo(base, &info) && info.state != 0 && info.allocation_size != 0 &&
        base >= info.allocation_base &&
        uint64_t(base) < uint64_t(info.allocation_base) + info.allocation_size) {
      *allocation = info.allocation_base;
      *allocation_size = info.allocation_size;
      return uint32_t(uint64_t(info.allocation_base) + info.allocation_size - base);
    }
  }
  // The game's own word: a base inside (block, block + asked) was answered with the block's size.
  uint32_t block = 0;
  if (mcla::guest::U32(kBlockWord, &block) && block != 0 && block != 0xFFFFFFFFu && base > block &&
      base - block < asked) {
    *allocation = block;
    *allocation_size = asked;
    return block + asked - base;
  }
  return 0;
}

void Clamp(PPCRegister& size, uint32_t base, const char* what) {
  if (!ClampWanted()) {
    return;
  }
  const uint32_t asked = size.u32;
  if (asked == 0 || base == 0) {
    return;
  }
  uint32_t allocation = 0, allocation_size = 0;
  const uint32_t left = LeftInAllocation(base, asked, &allocation, &allocation_size);
  if (left == 0 || left >= asked) {
    return;  // unknown, or the base is the allocation's start (or the SDK already counts from it)
  }
  size.u64 = left;
  static std::atomic<uint32_t> lines{0};
  const uint32_t n = lines.fetch_add(1, std::memory_order_relaxed);
  if (n >= kProbeLines) {
    return;
  }
  REXLOG_WARN("[mcla-ws-texclear] {} at 0x{:08X} lies 0x{:X} bytes inside its allocation: "
              "MmQueryAllocationSize says {} bytes, {} are left to the allocation's end; the zero-fill is "
              "cut to {} ({} bytes past the end avoided)",
              what, base, base - allocation, asked, left, left, asked - left);
  if (n == 0 && REXCVAR_GET(mcla_ws_heap_probe)) {
    LogLayout(base, asked, left, allocation, allocation_size);
  }
}

}  // namespace

// 0x8217829C in sub_82177EB0, BEFORE `mr r5,r3`: r3 = MmQueryAllocationSize(base), r29 = the base.
void mcla_hook_ws_texclear_base(PPCRegister& r3, PPCRegister& r29) {
  MCLA_HOOK_ENTER();
  Clamp(r3, r29.u32, "a texture's base");
}

// 0x821782C0 in sub_82177EB0, BEFORE `mr r5,r3`: the same for the mip chain, r31 = its address.
void mcla_hook_ws_texclear_mips(PPCRegister& r3, PPCRegister& r31) {
  MCLA_HOOK_ENTER();
  Clamp(r3, r31.u32, "a texture's mip chain");
}

// 0x82305430 in sub_82305230, BEFORE `bl 0x82130000` (the shipped build's empty log function):
// r4 = slot (0..8), r5 = the slot's name (static string), r6 = bytes free, r7 = bytes used.
void mcla_hook_ws_pool_slot(PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r7) {
  MCLA_HOOK_ENTER();
  if (mcla::ws::Width() == 0 || !REXCVAR_GET(mcla_ws_heap_probe)) {
    return;
  }
  static std::atomic<uint32_t> lines{0};
  const uint32_t n = lines.fetch_add(1, std::memory_order_relaxed);
  if (n >= 18) {  // two passes over the nine slots
    return;
  }
  char name[32] = "?";
  if (mcla::guest::InImage(r5.u32)) {
    (void)mcla::guest::CString(r5.u32, name, sizeof(name));
  }
  uint32_t pool = 0, base = 0, size = 0;
  if (mcla::guest::U32(kCurrentPool, &pool) && pool != 0) {
    (void)mcla::guest::U32(pool + 4, &base);
    (void)mcla::guest::U32(pool + 8, &size);
  }
  const int32_t free_bytes = r6.s32;
  REXLOG_INFO("[mcla-ws-heap] render target pool 0x{:08X}..0x{:08X} ({} bytes) slot {} '{}': {} used, {} "
              "free{}",
              base, base + size, size, r4.u32, name, r7.u32, free_bytes,
              free_bytes < 0 ? " PAST THE POOL'S END" : "");
  if (r4.u32 == 8) {
    LogGuardWords("after the render-target setup");
  }
}

#else  // MCLA_HOOK_WS_TEXCLEAR == 0: keeps hooks_widescreen.toml linking, does nothing.

void mcla_hook_ws_texclear_base(PPCRegister&, PPCRegister&) {}
void mcla_hook_ws_texclear_mips(PPCRegister&, PPCRegister&) {}
void mcla_hook_ws_pool_slot(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) {}

#endif
