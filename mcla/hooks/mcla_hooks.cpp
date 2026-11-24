// Shared support for the MCLA hooks: the hook registry (the "[mcla-<tag>] active" lines) and the
// guarded guest memory reads (mcla_guest.h). Not a hook itself: always compiled.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "nfsmw_pch.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"

namespace mcla::hooks {
namespace {

struct Entry {
  const char* tag;
  const char* cvar;
  bool compiled;
  bool (*enabled)();
  bool default_on;
};

// Function-local statics: hook files register from static initializers in any order.
std::vector<Entry>& Registry() {
  static std::vector<Entry> registry;
  return registry;
}
std::mutex& RegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

}  // namespace

Registration::Registration(const char* tag, const char* cvar, bool compiled, bool (*enabled)(),
                           bool default_on) {
  std::lock_guard<std::mutex> lock(RegistryMutex());
  Registry().push_back({tag, cvar, compiled, enabled, default_on});
}

void AnnounceHooks() {
  if (g_announced.exchange(true)) {
    return;
  }
  std::lock_guard<std::mutex> lock(RegistryMutex());
  for (const Entry& e : Registry()) {
    if (!e.compiled) {
      REXLOG_INFO("[mcla-{}] compiled out (MCLA_HOOK flag 0): the game's own code runs", e.tag);
    } else if (!e.enabled()) {
      if (e.default_on) {
        REXLOG_INFO("[mcla-{}] off (--{}=false): the game's own code runs", e.tag, e.cvar);
      } else {
        REXLOG_INFO("[mcla-{}] armed, off by default (--{}=true switches it on): the game's own "
                    "code runs",
                    e.tag, e.cvar);
      }
    } else if (e.default_on) {
      REXLOG_INFO("[mcla-{}] active (--{}=false turns it off)", e.tag, e.cvar);
    } else {
      REXLOG_INFO("[mcla-{}] active (switched on by --{}=true)", e.tag, e.cvar);
    }
  }
}

}  // namespace mcla::hooks

namespace mcla::guest {
namespace {

rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

bool Printable(uint8_t c) { return (c >= 0x20 && c < 0x7F) || c == '\n' || c == '\r' || c == '\t'; }

}  // namespace

bool Read(uint32_t address, void* out, size_t n) {
  if (n == 0 || n > 4096 || address < 0x10000 || uint64_t(address) + n > 0x100000000ull) {
    return false;
  }
  rex::memory::Memory* memory = GuestMemory();
  if (!memory) {
    return false;
  }
  const auto* host = memory->TranslateVirtual<const uint8_t*>(address);
  // A kernel-side copy of our own address space: FALSE (ERROR_PARTIAL_COPY) on a page that is not
  // committed or not readable, never a fault (see mcla_guest.h for why not VirtualQuery / SEH).
  SIZE_T got = 0;
  return ReadProcessMemory(GetCurrentProcess(), host, out, n, &got) && got == n;
}

bool U32(uint32_t address, uint32_t* out) {
  uint32_t value = 0;
  if (!Read(address, &value, sizeof(value))) {
    return false;
  }
  *out = __builtin_bswap32(value);
  return true;
}

bool U8(uint32_t address, uint8_t* out) {
  uint8_t value = 0;
  if (!Read(address, &value, 1)) {
    return false;
  }
  *out = value;
  return true;
}

int CString(uint32_t address, char* out, size_t size, bool* complete) {
  if (complete) {
    *complete = false;
  }
  if (size == 0) {
    return -1;
  }
  out[0] = '\0';
  size_t stored = 0;
  bool first = true;
  while (stored + 1 < size) {
    // Chunks that never cross a 4 KiB boundary: a string may end at the last bytes of a page.
    const uint32_t cursor = address + uint32_t(stored);
    size_t chunk = 0x1000 - (cursor & 0xFFF);
    if (chunk > 32) {
      chunk = 32;
    }
    if (chunk > size - 1 - stored) {
      chunk = size - 1 - stored;
    }
    uint8_t bytes[32];
    if (!Read(cursor, bytes, chunk)) {
      // Fall back to one byte (the chunk may run into an unmapped page); give up on the first miss.
      if (chunk == 1 || !Read(cursor, bytes, 1)) {
        return first ? -1 : int(stored);
      }
      chunk = 1;
    }
    first = false;
    for (size_t i = 0; i < chunk; ++i) {
      if (bytes[i] == 0) {
        out[stored] = '\0';
        if (complete) {
          *complete = true;
        }
        return int(stored);
      }
      if (!Printable(bytes[i])) {
        out[stored] = '\0';
        return int(stored);
      }
      out[stored++] = char(bytes[i]);
    }
    out[stored] = '\0';
  }
  return int(stored);
}

bool WriteU8(uint32_t address, uint8_t value) {
  uint8_t probe = 0;
  if (!U8(address, &probe)) {
    return false;
  }
  rex::memory::Memory* memory = GuestMemory();
  if (!memory) {
    return false;
  }
  *memory->TranslateVirtual<uint8_t*>(address) = value;
  return true;
}

bool WriteBytes(uint32_t address, const void* data, size_t n) {
  if (n == 0 || n > 64) {
    return false;
  }
  uint8_t probe[64];
  if (!Read(address, probe, n)) {
    return false;
  }
  rex::memory::Memory* memory = GuestMemory();
  if (!memory) {
    return false;
  }
  uint8_t* host = memory->TranslateVirtual<uint8_t*>(address);
  if (address >= kImageBase && address < 0x82130000u) {
    // .rdata / .pdata: read-only host pages: make them writable first (VirtualProtect rounds the
    // range to whole pages).
    DWORD previous = 0;
    if (!VirtualProtect(host, n, PAGE_READWRITE, &previous)) {
      return false;
    }
  }
  std::memcpy(host, data, n);
  return true;
}

}  // namespace mcla::guest
