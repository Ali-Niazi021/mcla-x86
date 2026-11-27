// mcla-x86: the hitch lines. See mcla_hitch.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <intrin.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_gov.h"
#include "mcla_hitch.h"

REXCVAR_DEFINE_DOUBLE(mcla_hitch_ms, 0.0, "MCLA",
                      "A frame longer than this (real time between engine frames, ms) gets a '[mcla-hitch]' line that "
                      "says what went on in it: the game thread's CPU time against its waiting, run-time pipeline "
                      "creations and shader translations, the GPU frame time, page faults and disk reads. 0 = auto "
                      "(1.5 frame periods of the cap in force: 25 ms at 60); a negative value turns the lines off. Live")
    .range(-1.0, 10000.0);
REXCVAR_DEFINE_INT32(mcla_hitch_lines, 12, "MCLA",
                     "Most '[mcla-hitch]' lines per 5 s; the rest of the window is counted in one summary line. Live")
    .range(0, 1000);

namespace mcla::hitch {
namespace {

// A snapshot of the counters at the start of a frame. Game thread only.
struct Snapshot {
  bool valid = false;
  uint64_t thread_cycles = 0;
  uint64_t tsc = 0;
  int64_t qpc = 0;
  uint32_t pipelines = 0;
  uint32_t shaders = 0;
  DWORD page_faults = 0;
  uint64_t read_bytes = 0;
};

Snapshot g_prev;
// TSC rate (QueryThreadCycleTime counts in the TSC's units on an invariant-TSC CPU), measured from the first frame on.
uint64_t g_tsc0 = 0;
int64_t g_qpc0 = 0;
int64_t g_qpf = 0;
// Rate limit: a 5 s window of lines.
int64_t g_window_start_ns = 0;
int g_lines = 0;
uint64_t g_unlogged = 0;
double g_unlogged_worst_ms = 0.0;

Snapshot Take() {
  Snapshot s;
  s.valid = true;
  ULONG64 cycles = 0;
  if (QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
    s.thread_cycles = cycles;
  }
  s.tsc = __rdtsc();
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  s.qpc = qpc.QuadPart;
  const mcla::gov::GpuCounters gpu = mcla::gov::ReadGpuCounters();
  s.pipelines = gpu.pipelines;
  s.shaders = gpu.shaders;
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof(pmc);
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    s.page_faults = pmc.PageFaultCount;
  }
  IO_COUNTERS io{};
  if (GetProcessIoCounters(GetCurrentProcess(), &io)) {
    s.read_bytes = io.ReadTransferCount;
  }
  return s;
}

double TscPerMs(const Snapshot& now) {
  if (!g_qpf) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = f.QuadPart > 0 ? f.QuadPart : 1;
  }
  if (!g_tsc0) {
    g_tsc0 = now.tsc;
    g_qpc0 = now.qpc;
    return 0.0;
  }
  const double ms = double(now.qpc - g_qpc0) * 1000.0 / double(g_qpf);
  return ms > 100.0 ? double(now.tsc - g_tsc0) / ms : 0.0;
}

const char* ModeName(int screen_mode) {
  return screen_mode == 0 ? "a drive" : screen_mode == 1 ? "a menu / pause / load" : "an unknown screen";
}

}  // namespace

void OnFrame(int64_t now_ns, double frame_ms, double period_ms, int screen_mode, double slept_ms) {
  const double setting = REXCVAR_GET(mcla_hitch_ms);
  if (setting < 0.0) {
    g_prev.valid = false;
    return;
  }
  const Snapshot now = Take();
  const double tsc_per_ms = TscPerMs(now);
  const Snapshot prev = g_prev;
  g_prev = now;

  if (g_window_start_ns == 0 || now_ns - g_window_start_ns >= 5'000'000'000) {
    if (g_unlogged > 0) {
      REXLOG_INFO("[mcla-hitch] {} more hitch(es) in the last 5 s not logged (worst {:.1f} ms; mcla_hitch_lines {})",
                  g_unlogged, g_unlogged_worst_ms, REXCVAR_GET(mcla_hitch_lines));
    }
    g_window_start_ns = now_ns;
    g_lines = 0;
    g_unlogged = 0;
    g_unlogged_worst_ms = 0.0;
  }

  const double period = period_ms > 0.0 ? period_ms : 1000.0 / 60.0;
  const double threshold = setting > 0.0 ? setting : 1.5 * period;
  if (!prev.valid || frame_ms <= 0.0 || frame_ms < threshold) {
    return;
  }
  if (g_lines >= REXCVAR_GET(mcla_hitch_lines)) {
    ++g_unlogged;
    g_unlogged_worst_ms = std::max(g_unlogged_worst_ms, frame_ms);
    return;
  }
  ++g_lines;

  std::string cpu;
  if (tsc_per_ms > 0.0 && now.thread_cycles >= prev.thread_cycles) {
    const double on_cpu_ms = double(now.thread_cycles - prev.thread_cycles) / tsc_per_ms;
    cpu = fmt::format("game thread on CPU {:.1f} ms (waiting {:.1f} ms, of which pacer sleep {:.1f} ms)", on_cpu_ms,
                      std::max(0.0, frame_ms - on_cpu_ms), slept_ms);
  } else {
    cpu = fmt::format("game thread CPU time not measured yet (pacer sleep {:.1f} ms)", slept_ms);
  }
  const mcla::gov::GpuCounters gpu = mcla::gov::ReadGpuCounters();
  const std::string compile =
      gpu.compile ? fmt::format("pipelines +{} shaders +{} in flight {}", now.pipelines - prev.pipelines,
                                now.shaders - prev.shaders, gpu.pending)
                  : std::string("pipelines ? (no SDK counters)");
  const std::string gpu_time = gpu.gpu && gpu.gpu_us ? fmt::format("GPU frame {:.1f} ms", gpu.gpu_us / 1000.0)
                                                     : std::string("GPU frame ?");
  const uint64_t read = now.read_bytes >= prev.read_bytes ? now.read_bytes - prev.read_bytes : 0;
  REXLOG_INFO("[mcla-hitch] {:.1f} ms between engine frames ({:.1f} periods of {:.1f}) in {} | {} | {} | {} | "
              "page faults +{}, read +{:.1f} MiB",
              frame_ms, frame_ms / period, period,
              ModeName(screen_mode), cpu, compile, gpu_time, now.page_faults - prev.page_faults,
              double(read) / (1024.0 * 1024.0));
}

}  // namespace mcla::hitch
