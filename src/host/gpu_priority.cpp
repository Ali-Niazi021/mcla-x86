// mcla-x86 host: the process's GPU scheduling priority class. See gpu_priority.h.
#include "gpu_priority.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(mcla_gpu_priority, 0, "MCLA",
                     "GPU scheduling priority class of the game process (D3DKMTSetProcessSchedulingPriorityClass): "
                     "0 = leave it to Windows (normal), 1 = above normal, 2 = high. A higher class gets a larger "
                     "share of the GPU when another program uses it at the same time. No admin rights needed. Live")
    .range(0, 2);

namespace mcla_host {

namespace {

// D3DKMT_SCHEDULINGPRIORITYCLASS (d3dkmthk.h): IDLE 0, BELOW_NORMAL 1, NORMAL 2, ABOVE_NORMAL 3, HIGH 4, REALTIME 5.
using SetClassFn = LONG(APIENTRY*)(HANDLE, int);
using GetClassFn = LONG(APIENTRY*)(HANDLE, int*);

std::mutex g_mutex;
SetClassFn g_set = nullptr;
GetClassFn g_get = nullptr;
int g_original = -1;  // the class found at start (-1 = unknown)
bool g_started = false;

const char* ClassName(int c) {
  switch (c) {
    case 0: return "idle";
    case 1: return "below normal";
    case 2: return "normal";
    case 3: return "above normal";
    case 4: return "high";
    case 5: return "realtime";
    default: return "unknown";
  }
}

int CurrentClass() {
  int c = -1;
  if (g_get && g_get(GetCurrentProcess(), &c) != 0) {
    c = -1;
  }
  return c;
}

void Apply(int knob) {
  std::lock_guard lock(g_mutex);
  if (!g_set) {
    return;
  }
  const int want = knob == 1 ? 3 : knob == 2 ? 4 : (g_original >= 0 ? g_original : 2);
  const int before = CurrentClass();
  if (before == want) {
    REXLOG_INFO("[mcla-gpu] scheduling priority class {} (mcla_gpu_priority={}): unchanged", ClassName(want), knob);
    return;
  }
  const LONG status = g_set(GetCurrentProcess(), want);
  const int after = CurrentClass();
  if (status != 0) {
    REXLOG_WARN("[mcla-gpu] scheduling priority class {} -> {} FAILED (status 0x{:08X}); now {}", ClassName(before),
                ClassName(want), static_cast<uint32_t>(status), ClassName(after));
  } else {
    REXLOG_INFO("[mcla-gpu] scheduling priority class {} -> {} (mcla_gpu_priority={}); now {}", ClassName(before),
                ClassName(want), knob, ClassName(after));
  }
  rex::FlushLogging();
}

}  // namespace

void StartGpuPriority() {
  {
    std::lock_guard lock(g_mutex);
    if (g_started) {
      return;
    }
    g_started = true;
    if (HMODULE gdi = LoadLibraryW(L"gdi32.dll")) {
      g_set = reinterpret_cast<SetClassFn>(GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass"));
      g_get = reinterpret_cast<GetClassFn>(GetProcAddress(gdi, "D3DKMTGetProcessSchedulingPriorityClass"));
    }
    if (!g_set) {
      REXLOG_WARN("[mcla-gpu] D3DKMTSetProcessSchedulingPriorityClass not found: mcla_gpu_priority has no effect");
      return;
    }
    g_original = CurrentClass();
  }
  const int knob = REXCVAR_GET(mcla_gpu_priority);
  if (knob != 0) {
    Apply(knob);
  } else {
    REXLOG_INFO("[mcla-gpu] scheduling priority class {} (left to Windows; mcla_gpu_priority=0)",
                ClassName(g_original));
  }
  rex::cvar::RegisterChangeCallback("mcla_gpu_priority", [](std::string_view, std::string_view) {
    Apply(REXCVAR_GET(mcla_gpu_priority));
  });
}

void StopGpuPriority() {
  std::lock_guard lock(g_mutex);
  if (g_set && g_original >= 0 && CurrentClass() != g_original) {
    g_set(GetCurrentProcess(), g_original);
  }
}

}  // namespace mcla_host
