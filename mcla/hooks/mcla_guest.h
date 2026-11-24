// Guest memory access for the MCLA hooks.
//
// A hook gets registers, not the memory base, and runs on a guest thread in the middle of a game
// function: a pointer it dereferences is whatever the game had in a register. The game (RAGE) frees
// and rebuilds objects while other threads read them, and our SDK protects guest page 0, so every
// read of an address that is not static image data goes through Read(): the host pages behind the
// address are copied with ReadProcessMemory on the current process (a kernel-side copy), which FAILS
// (false) on an unmapped, decommitted or protected page instead of faulting. Nothing here ever throws
// or crashes on a bad guest address.
//
// Why ReadProcessMemory and not VirtualQuery + memcpy or an SEH __try: a VirtualQuery check races a
// decommit by another guest thread between the check and the copy (the page is gone, the copy
// faults); an __try would first send the fault through the SDK's vectored exception handlers (MMIO
// and write-watch), which must not see a probe. The kernel copy is atomic with respect to the page
// state and raises nothing in user mode. It costs one system call a read, the same as the iOS
// original (vm_read_overwrite): hooks on hot paths read the static image directly and say why.
//
// Guest data is big-endian; U32() swaps. The static image (0x82000000..0x829E0000) is always mapped
// (InImage), but still goes through Read() unless a hook is on a hot path and says why.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mcla::guest {

constexpr uint32_t kImageBase = 0x82000000;
constexpr uint32_t kImageEnd = 0x829E0000;

inline bool InImage(uint32_t address) { return address >= kImageBase && address < kImageEnd; }

// Copies n bytes (1..4096) of guest memory at `address` to `out`. False (and `out` untouched or
// partly written) when the guest memory is not up, the address is below 0x10000, wraps, or any
// host page behind it is unreadable.
bool Read(uint32_t address, void* out, size_t n);

// Big-endian loads. False on failure (`out` unchanged).
bool U32(uint32_t address, uint32_t* out);
bool U8(uint32_t address, uint8_t* out);

// Reads a C string (printable ASCII, \n, \r, \t) into out[0..size-1], always NUL-terminated.
// Stops at the terminator, at the first byte that is not text, or when full. Returns the number
// of characters stored; -1 when the very first byte could not be read. `*complete` (optional) is
// true when the terminator was found within the buffer.
int CString(uint32_t address, char* out, size_t size, bool* complete = nullptr);

// Stores one byte after checking that the byte is readable (so the page is mapped). False if not.
bool WriteU8(uint32_t address, uint8_t value);

// Stores n bytes (1..64) after checking that the whole range is readable (so the pages are mapped).
// Addresses in the image's read-only part (.rdata, below 0x82130000) get their host pages made
// writable first (VirtualProtect): a plain store there faults. False if the range is not mapped or
// cannot be made writable.
bool WriteBytes(uint32_t address, const void* data, size_t n);

}  // namespace mcla::guest
