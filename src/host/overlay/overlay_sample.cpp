// mcla-x86 host overlay: the statistics snapshot. See overlay_sample.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/version.h>

#include "mcla_gfx_tune.h"
#include "mcla_gov.h"
#include "mcla_t_common.h"
#include "overlay_sample.h"
#include "present_stats_abi.h"
#include "system_status.h"
#include "window_fit.h"

#ifndef MCLA_SDK_SERIES
#define MCLA_SDK_SERIES "unknown"
#endif
#ifndef REXGLUE_BUILD_CONFIG
#define REXGLUE_BUILD_CONFIG "unknown"
#endif

namespace mcla_overlay {
namespace {

constexpr int64_t kFastIntervalNs = 250'000'000;      // frames, GPU, governor, world
constexpr int64_t kSystemIntervalNs = 1'000'000'000;  // machine and display
constexpr double kFrameWindowS = 5.0;                  // the window of the fps / percentile numbers
constexpr int64_t kStaleNs = 5'000'000'000;            // a baseline older than this (the overlay was hidden) says nothing about now

// The GPU plugin first (rexgpu-xenos.dll, postfix d / rd in the debug builds), the runtime second: the way mcla_gov.cpp finds
// the SDK's counters.
void* FindSymbol(const char* name) {
  for (const wchar_t* base : {L"rexgpu-xenos", L"rexruntime"}) {
    for (const wchar_t* postfix : {L"", L"d", L"rd"}) {
      const std::wstring file = std::wstring(base) + postfix + L".dll";
      HMODULE handle = GetModuleHandleW(file.c_str());
      if (!handle) continue;
      if (FARPROC symbol = GetProcAddress(handle, name)) {
        return reinterpret_cast<void*>(symbol);
      }
    }
  }
  return nullptr;
}

// CallNtPowerInformation(ProcessorInformation) fills one of these per logical processor (as system_status.cpp).
struct ProcessorPowerInformation {
  ULONG Number;
  ULONG MaxMhz;
  ULONG CurrentMhz;
  ULONG MhzLimit;
  ULONG MaxIdleState;
  ULONG CurrentIdleState;
};

ULONGLONG FileTime100ns(const FILETIME& t) { return (ULONGLONG(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

std::string Utf8FromWide(const wchar_t* wide) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string utf8(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), size, nullptr, nullptr);
  return utf8;
}

}  // namespace

struct Sampler::Impl {
  // Symbols, resolved once (null = this SDK build does not have it).
  bool resolved = false;
  std::atomic<uint32_t>* gpu_seq = nullptr;  // rexgpu_gpu_frame_seq
  std::atomic<uint32_t>* gpu_lag = nullptr;  // rexgpu_gpu_frame_lag
  RexPresentStatsAbi* present = nullptr;     // rex_present_stats (SDK 0755)

  int64_t last_fast_ns = 0;
  int64_t last_system_ns = 0;

  // GPU frame rate.
  uint32_t prev_seq = 0;
  int64_t prev_seq_ns = 0;

  double world_speed = -1.0;  // the game timer against real time (WorldClock)

  // Pipelines created in the last minute.
  struct PipelineSample {
    int64_t ns;
    uint32_t count;
  };
  std::deque<PipelineSample> pipelines;

  // Display side.
  struct PresentPrev {
    uint64_t presents = 0, shown = 0, dropped = 0, over = 0;
    uint64_t held[4] = {};
    uint64_t sync_refreshes = 0;
    int64_t sync_qpc = 0;
    int64_t ns = 0;
    bool valid = false;
  } present_prev;
  PresentView present_view;

  // Machine.
  bool system_init = false;
  std::vector<int> cpu_class;  // efficiency class per logical processor index, -1 unknown
  using CallNtPowerInformationFn = LONG(WINAPI*)(int, PVOID, ULONG, PVOID, ULONG);
  CallNtPowerInformationFn call_power = nullptr;
  ULONGLONG cpu_100ns = 0;
  ULONGLONG wall_100ns = 0;
  DWORD page_faults = 0;
  ULONGLONG read_bytes = 0;

  void Resolve() {
    resolved = true;
    gpu_seq = static_cast<std::atomic<uint32_t>*>(FindSymbol("rexgpu_gpu_frame_seq"));
    gpu_lag = static_cast<std::atomic<uint32_t>*>(FindSymbol("rexgpu_gpu_frame_lag"));
    present = static_cast<RexPresentStatsAbi*>(FindSymbol("rex_present_stats"));
    // A layout older than what this reads, or a different version: leave it out rather than read past the end.
    if (present && (present->version != 1 || present->size < sizeof(RexPresentStatsAbi))) {
      present = nullptr;
    }
  }

  void UpdateFast(int64_t now_ns, Snapshot& s);
  void UpdateWorld(Snapshot& s);
  void UpdatePresent(int64_t now_ns, Snapshot& s);
  void UpdateSystem(Snapshot& s);
};

void Sampler::Impl::UpdateFast(int64_t now_ns, Snapshot& s) {
  // Engine frames.
  double span_s = 0.0;
  const std::vector<float> recent = RecentFrameTimes(now_ns, kFrameWindowS, 1200, &span_s);
  s.frames_fed = !recent.empty();
  s.frames = math::Summarize(recent);
  {
    // The counter's number: the last second, so it follows a change within a second.
    double sum_ms = 0.0;
    const std::vector<float> last = RecentFrameTimes(now_ns, 1.0, 400);
    for (float ms : last) {
      sum_ms += double(ms);
    }
    s.avg_ms_1s = last.empty() ? 0.0 : sum_ms / double(last.size());
    s.fps_1s = s.avg_ms_1s > 0.0 ? 1000.0 / s.avg_ms_1s : 0.0;
  }

  // GPU.
  const mcla::gov::GpuCounters gpu = mcla::gov::ReadGpuCounters();
  s.gpu_known = gpu.gpu && gpu.gpu_us > 0;
  s.gpu_busy_ms = double(gpu.gpu_us) / 1000.0;
  s.compile_known = gpu.compile;
  s.pipelines = gpu.pipelines;
  s.shaders = gpu.shaders;
  s.pending = gpu.pending;
  s.gpu_lag = gpu_lag ? gpu_lag->load(std::memory_order_relaxed) : 0;
  if (gpu_seq) {
    const uint32_t seq = gpu_seq->load(std::memory_order_acquire);
    if (prev_seq_ns != 0 && now_ns - prev_seq_ns >= 1'000'000'000 && now_ns - prev_seq_ns < kStaleNs) {
      s.gpu_fps = math::RatePerSecond(prev_seq, seq, double(now_ns - prev_seq_ns) / 1e9, 32);
      prev_seq = seq;
      prev_seq_ns = now_ns;
    } else if (prev_seq_ns == 0 || now_ns - prev_seq_ns >= kStaleNs) {  // the first read, or the overlay was hidden meanwhile
      prev_seq = seq;
      prev_seq_ns = now_ns;
    }
  }
  if (gpu.compile) {
    if (pipelines.empty() || now_ns - pipelines.back().ns >= 1'000'000'000) {
      pipelines.push_back({now_ns, gpu.pipelines});
    }
    while (pipelines.size() > 1 && now_ns - pipelines.front().ns > 60'000'000'000) pipelines.pop_front();
    if (pipelines.size() > 1) {
      const double minutes = double(now_ns - pipelines.front().ns) / 60e9;
      s.pipelines_per_min = minutes > 0.0 ? double(gpu.pipelines - pipelines.front().count) / minutes : 0.0;
    }
  }

  // The governor.
  s.gov_on = mcla::gov::Enabled();
  s.gov_level = mcla::gov::Level();
  s.gov_tier = mcla::gov::Tier();
  s.gov_vrs = mcla::gov::Vrs();
  s.compile_burst = mcla::gov::CompileBurstActive();
  s.gfx_tier = mcla::gfxtune::CurrentTier();
  s.gfx_tier_name.clear();
  if (s.gfx_tier >= 0) {
    int count = 0;
    const mcla::gfxtune::TierInfo* tiers = mcla::gfxtune::Tiers(&count);
    for (int i = 0; i < count; ++i) {
      if (tiers[i].tier == s.gfx_tier) s.gfx_tier_name = tiers[i].name;
    }
  }

  // The world.
  s.sim_on = mcla::sim::Enabled();
  s.world_cap = mcla::sim::EffectiveCap();
  UpdateWorld(s);
  s.world_speed = world_speed;

  s.hitches = Hitches(now_ns, 60.0);
}

void Sampler::Impl::UpdateWorld(Snapshot& /*s*/) {
  // The feed stores the game timer's total with every frame start (the hook reads it on the game thread), so the two ends of the
  // span are exact frame starts. 3 s: a sliding window that follows a pause or a slow-down within a few seconds.
  double world_s = 0.0, real_s = 0.0;
  world_speed = mcla_overlay::WorldClock(3.0, &world_s, &real_s) ? math::WorldSpeed(world_s, real_s, 2.9) : -1.0;
}

void Sampler::Impl::UpdatePresent(int64_t now_ns, Snapshot& s) {
  PresentView& v = present_view;
  v.available = present != nullptr;
  if (!present) {
    s.present = v;
    return;
  }
  PresentPrev cur;
  cur.presents = present->presents.load(std::memory_order_relaxed);
  cur.shown = present->shown.load(std::memory_order_relaxed);
  cur.dropped = present->dropped.load(std::memory_order_relaxed);
  cur.over = present->over_cadence.load(std::memory_order_relaxed);
  for (int i = 0; i < 4; ++i) cur.held[i] = present->held[i].load(std::memory_order_relaxed);
  cur.sync_refreshes = present->sync_refreshes.load(std::memory_order_relaxed);
  cur.sync_qpc = present->sync_qpc.load(std::memory_order_relaxed);
  cur.ns = now_ns;
  cur.valid = present->valid.load(std::memory_order_relaxed) != 0;
  const int64_t qpf = std::max<int64_t>(1, present->qpf.load(std::memory_order_relaxed));
  v.valid = cur.valid;
  v.dropped_total = cur.dropped;
  v.over_cadence_total = cur.over;
  v.cadence = double(present->cadence_milli.load(std::memory_order_relaxed)) / 1000.0;
  const double seconds = double(now_ns - present_prev.ns) / 1e9;
  if (present_prev.ns != 0 && now_ns - present_prev.ns >= kStaleNs) {
    // The statistics were hidden for a while: the rates would be an average over the gap. Start over; the old numbers go.
    present_prev = cur;
    v.present_fps = v.shown_fps = v.dropped_per_s = v.over_cadence_per_min = v.refresh_hz = 0.0;
    v.held1_pct = v.held2_pct = v.held3p_pct = 0.0;
    v.rates_valid = false;
  } else if (present_prev.ns != 0 && seconds >= 1.0 && cur.presents >= present_prev.presents) {
    v.rates_valid = true;
    v.present_fps = math::RatePerSecond(present_prev.presents, cur.presents, seconds);
    v.shown_fps = math::RatePerSecond(present_prev.shown, cur.shown, seconds);
    v.dropped_per_s = math::RatePerSecond(present_prev.dropped, cur.dropped, seconds);
    v.over_cadence_per_min = math::RatePerSecond(present_prev.over, cur.over, seconds) * 60.0;
    const uint64_t d_refreshes = cur.sync_refreshes - present_prev.sync_refreshes;
    const int64_t d_qpc = cur.sync_qpc - present_prev.sync_qpc;
    v.refresh_hz = (d_refreshes > 0 && d_qpc > 0) ? double(d_refreshes) * double(qpf) / double(d_qpc) : 0.0;
    uint64_t held[4];
    uint64_t held_total = 0;
    for (int i = 0; i < 4; ++i) {
      held[i] = cur.held[i] - present_prev.held[i];
      held_total += held[i];
    }
    if (held_total > 0) {
      v.held1_pct = 100.0 * double(held[0]) / double(held_total);
      v.held2_pct = 100.0 * double(held[1]) / double(held_total);
      v.held3p_pct = 100.0 * double(held[2] + held[3]) / double(held_total);
    }
    present_prev = cur;
  } else if (present_prev.ns == 0 || cur.presents < present_prev.presents) {
    present_prev = cur;  // the first read, or the counters were reset (a new swap chain)
  }
  s.present = v;
}

void Sampler::Impl::UpdateSystem(Snapshot& s) {
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  const DWORD count = si.dwNumberOfProcessors;
  if (!system_init) {
    system_init = true;
    cpu_class.assign(count, -1);
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    if (length) {
      std::vector<uint8_t> buffer(length);
      if (GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), length, &length,
                                     GetCurrentProcess(), 0)) {
        for (size_t offset = 0; offset + 8 <= length;) {
          const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
          if (entry->Size == 0) break;
          if (entry->Type == CpuSetInformation && entry->CpuSet.Group == 0 && entry->CpuSet.LogicalProcessorIndex < count) {
            cpu_class[entry->CpuSet.LogicalProcessorIndex] = entry->CpuSet.EfficiencyClass;
          }
          offset += entry->Size;
        }
      }
    }
    if (HMODULE module = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
      call_power = reinterpret_cast<CallNtPowerInformationFn>(
          reinterpret_cast<void*>(GetProcAddress(module, "CallNtPowerInformation")));
    }
  }

  // Clocks per efficiency class: the average current clock, and the lowest limit against the maximum. A limit under the
  // maximum is the firmware or Windows capping the clock (thermal, power, a quiet mode).
  s.clocks.clear();
  if (call_power && count > 0) {
    std::vector<ProcessorPowerInformation> info(count);
    constexpr int kProcessorInformation = 11;
    if (call_power(kProcessorInformation, nullptr, 0, info.data(), ULONG(sizeof(ProcessorPowerInformation) * count)) == 0) {
      std::map<int, std::vector<const ProcessorPowerInformation*>, std::greater<int>> by_class;
      for (DWORD i = 0; i < count; ++i) by_class[i < cpu_class.size() ? cpu_class[i] : -1].push_back(&info[i]);
      for (const auto& [cls, list] : by_class) {
        ClockClass c;
        c.efficiency_class = cls;
        c.logical = int(list.size());
        uint64_t sum = 0;
        ULONG max_mhz = 0, min_limit = ~0u;
        for (const ProcessorPowerInformation* p : list) {
          sum += p->CurrentMhz;
          max_mhz = std::max(max_mhz, p->MaxMhz);
          min_limit = std::min(min_limit, p->MhzLimit);
        }
        c.avg_mhz = uint32_t(sum / std::max<size_t>(1, list.size()));
        c.max_mhz = max_mhz;
        c.min_limit_mhz = min_limit == ~0u ? 0 : min_limit;
        c.capped = c.min_limit_mhz > 0 && c.min_limit_mhz < c.max_mhz;
        s.clocks.push_back(c);
      }
    }
  }

  // The process: cores' worth of CPU since the last sample, memory, paging and reads.
  FILETIME create{}, exit{}, kernel{}, user{}, now_ft{};
  GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user);
  GetSystemTimeAsFileTime(&now_ft);
  const ULONGLONG cpu = FileTime100ns(kernel) + FileTime100ns(user);
  const ULONGLONG wall = FileTime100ns(now_ft);
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  IO_COUNTERS io{};
  GetProcessIoCounters(GetCurrentProcess(), &io);
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  GlobalMemoryStatusEx(&mem);
  if (wall_100ns && wall > wall_100ns && wall - wall_100ns < ULONGLONG(kStaleNs / 100)) {
    const double seconds = double(wall - wall_100ns) / 1e7;
    s.cpu_cores = double(cpu - cpu_100ns) / 1e7 / seconds;
    s.page_faults_s = double(pmc.PageFaultCount - page_faults) / seconds;
    s.read_mib_s = double(io.ReadTransferCount - read_bytes) / seconds / (1024.0 * 1024.0);
  }
  cpu_100ns = cpu;
  wall_100ns = wall;
  page_faults = pmc.PageFaultCount;
  read_bytes = io.ReadTransferCount;
  s.working_set_mib = uint32_t(pmc.WorkingSetSize >> 20);
  s.private_mib = uint32_t(pmc.PrivateUsage >> 20);
  s.memory_load_pct = uint32_t(mem.dwMemoryLoad);
  s.system_known = true;
}

AboutInfo CollectAbout(void* native_window) {
  AboutInfo about;
  about.build = fmt::format("{} build {}", REXGLUE_BUILD_CONFIG, REXGLUE_BUILD_TIMESTAMP);
  about.sdk = fmt::format("ReXGlue {}, SDK patch series {}", REXGLUE_VERSION_STRING, MCLA_SDK_SERIES);

  // The adapter the game renders on: the SDK's d3d12_adapter rule (-1 = the first hardware adapter).
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    const int32_t wanted = rex::cvar::Query<int32_t>("d3d12_adapter");
    for (UINT index = 0;; ++index) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc{};
      if (FAILED(adapter->GetDesc1(&desc))) continue;
      const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
      if ((wanted >= 0 && int32_t(index) == wanted) || (wanted < 0 && !software)) {
        about.gpu = fmt::format("{} (vendor 0x{:04X}, {} MiB dedicated, {} MiB shared)", Utf8FromWide(desc.Description),
                                desc.VendorId, static_cast<uint64_t>(desc.DedicatedVideoMemory) >> 20,
                                static_cast<uint64_t>(desc.SharedSystemMemory) >> 20);
        break;
      }
    }
  }
  if (about.gpu.empty()) about.gpu = "unknown";

  const mcla_host::SystemStatus status = mcla_host::QuerySystemStatus(native_window);
  if (status.display_known) {
    about.display = fmt::format("{} {}x{} @ {} Hz, DPI {}", status.display_device, status.display_width,
                                status.display_height, status.display_hz, status.display_dpi);
  } else {
    about.display = "unknown";
  }
  about.machine = mcla_host::SystemStatusOneLine(status);

  int32_t guest_width = 0, guest_height = 0;
  mcla_host::GuestVideoModeSize(guest_width, guest_height);
  const std::string profile = rex::cvar::Query<std::string>("mcla_profile");
  about.guest = fmt::format("guest {}x{}, profile {}, fps target {}", guest_width, guest_height,
                            profile.empty() ? "default" : profile, rex::cvar::Query<int32_t>("mcla_fps_target"));
  return about;
}

Sampler::Sampler() : impl_(new Impl) {}
Sampler::~Sampler() { delete impl_; }

void Sampler::Update(int64_t now_ns, bool with_system) {
  if (!impl_->resolved) impl_->Resolve();
  if (impl_->last_fast_ns == 0 || now_ns - impl_->last_fast_ns >= kFastIntervalNs) {
    impl_->last_fast_ns = now_ns;
    snapshot_.at_ns = now_ns;
    impl_->UpdateFast(now_ns, snapshot_);
  }
  if (with_system && (impl_->last_system_ns == 0 || now_ns - impl_->last_system_ns >= kSystemIntervalNs)) {
    impl_->last_system_ns = now_ns;
    impl_->UpdateSystem(snapshot_);
    impl_->UpdatePresent(now_ns, snapshot_);
  }
}

}  // namespace mcla_overlay
