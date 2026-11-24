// mcla-x86 host: thin wrappers over WaitOnAddress / WakeByAddress* (Synchronization.lib).
//
// The address-wait family is the Windows counterpart of the iOS port's os_sync_wait_on_address: a thread sleeps while a
// 32/64-bit word holds an "undesired" value, and the writer wakes it after changing the word. The hooks under
// mcla/hooks use the raw API (PLAN 4.1); the host pacing code of W3.3 uses these.
//
// Every wait can wake spuriously, and the word can change between the caller's last read and the wait: callers loop
// and re-check their condition.
#pragma once

#include <cstdint>

namespace mcla_host {

inline constexpr uint32_t kWaitInfinite = 0xFFFFFFFFu;  // INFINITE

// Sleeps while *address == undesired. Returns true if it woke (a wake call, a spurious wake, or the word already
// differed), false if timeout_ms elapsed first.
bool WaitWhileEqual32(const volatile void* address, uint32_t undesired, uint32_t timeout_ms = kWaitInfinite);
bool WaitWhileEqual64(const volatile void* address, uint64_t undesired, uint32_t timeout_ms = kWaitInfinite);

// Wake the waiters of `address` (all of them / one of them) after the word changed.
void WakeAll(void* address);
void WakeOne(void* address);

}  // namespace mcla_host
