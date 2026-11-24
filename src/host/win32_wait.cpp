// mcla-x86 host: WaitOnAddress wrappers. See win32_wait.h.
#include "win32_wait.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <synchapi.h>

namespace mcla_host {

namespace {

bool WaitWhileEqualN(const volatile void* address, void* undesired, size_t size, uint32_t timeout_ms) {
  // WaitOnAddress returns TRUE when the word differs from `undesired` at once, or when it was woken.
  return WaitOnAddress(const_cast<volatile VOID*>(address), undesired, size, timeout_ms) != FALSE;
}

}  // namespace

bool WaitWhileEqual32(const volatile void* address, uint32_t undesired, uint32_t timeout_ms) {
  return WaitWhileEqualN(address, &undesired, sizeof(undesired), timeout_ms);
}

bool WaitWhileEqual64(const volatile void* address, uint64_t undesired, uint32_t timeout_ms) {
  return WaitWhileEqualN(address, &undesired, sizeof(undesired), timeout_ms);
}

void WakeAll(void* address) {
  WakeByAddressAll(address);
}

void WakeOne(void* address) {
  WakeByAddressSingle(address);
}

}  // namespace mcla_host
