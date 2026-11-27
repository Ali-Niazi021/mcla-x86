// mcla-x86 host: page-fault probe and guest memory pre-fault. See pf_probe.h.
#include "pf_probe.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_INT32(mcla_pf_probe_ms, 0, "MCLA",
                     "Diagnostic, off at 0: sample the process' page fault count every N ms; after a burst of "
                     "mcla_pf_probe_burst faults compare which pages of the address space became resident with a "
                     "snapshot from before it and log the biggest ranges ('[mcla-pf]'). Costs tens of ms of a "
                     "background thread per snapshot: never on in a measurement run")
    .range(0, 1000);
REXCVAR_DEFINE_INT32(mcla_pf_probe_burst, 3000, "MCLA",
                     "mcla_pf_probe_ms: page faults in a burst (a sample of N ms with at least a third of this many "
                     "starts one) that is worth a report")
    .range(100, 10000000);
REXCVAR_DEFINE_BOOL(mcla_prefault_guest, false, "MCLA",
                    "Read one byte of every committed 4 KiB page of the guest views named by mcla_prefault_ranges once "
                    "when the runtime is up (background thread), so the soft page faults of the first 3D scene "
                    "(about 73,000 in one 150 ms frame) happen at boot. Reads only; guest memory is not written");
REXCVAR_DEFINE_STRING(mcla_prefault_ranges, "phys,va,ve", "MCLA",
                      "mcla_prefault_guest: comma separated views of the guest arena: phys (the raw physical view), v0 "
                      "(guest 0x00000000, 4 KiB pages), v4 (0x40000000), v8 (0x80000000 xex), v9 (0x90000000), va "
                      "(0xA0000000, the physical heap in 64 KiB pages), vc (0xC0000000), ve (0xE0000000)");

namespace mcla_host {
namespace {

constexpr uint64_t kPage = 4096;
constexpr uint64_t kChunkPages = 256;  // 1 MiB: the unit of the snapshot's comparison

std::atomic<bool> g_stop{false};
std::thread g_probe_thread;
std::thread g_prefault_thread;

struct Chunk {
  uintptr_t base = 0;
  uint32_t pages = 0;
  uint32_t resident = 0;
  uintptr_t alloc_base = 0;
  DWORD type = 0;
  DWORD protect = 0;
};
using Snapshot = std::vector<Chunk>;

uint8_t* VirtualBase() {
  auto* kernel = rex::system::kernel_state();
  auto* memory = kernel ? kernel->memory() : nullptr;
  return memory ? memory->virtual_membase() : nullptr;
}
uint8_t* PhysicalBase() {
  auto* kernel = rex::system::kernel_state();
  auto* memory = kernel ? kernel->memory() : nullptr;
  return memory ? memory->physical_membase() : nullptr;
}

// Every committed region of the process, summed per cell of an absolute 1 MiB grid (so two snapshots line up whatever the
// regions around a cell did in between), with the number of its pages in the working set. QueryWorkingSetEx never faults.
void TakeSnapshot(Snapshot& out, double* ms_out) {
  const auto t0 = std::chrono::steady_clock::now();
  out.clear();
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  const uintptr_t limit = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
  constexpr uintptr_t kCell = kChunkPages * kPage;
  std::vector<PSAPI_WORKING_SET_EX_INFORMATION> info;
  uintptr_t address = 0;
  MEMORY_BASIC_INFORMATION mbi{};
  while (address < limit && VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi)) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t size = mbi.RegionSize;
    address = base + size;
    // Pages the guest has not allocated are host-committed but PAGE_NOACCESS; they are counted too (QueryWorkingSetEx
    // says whether they are resident, and a guest allocation only changes the protection).
    if (mbi.State != MEM_COMMIT || size == 0) {
      continue;
    }
    for (uintptr_t part = base; part < base + size;) {
      const uintptr_t part_end = std::min<uintptr_t>(base + size, part + (kChunkPages * 64) * kPage);
      const size_t pages = size_t((part_end - part) / kPage);
      info.resize(pages);
      for (size_t i = 0; i < pages; ++i) {
        info[i].VirtualAddress = reinterpret_cast<PVOID>(part + i * kPage);
      }
      if (!K32QueryWorkingSetEx(GetCurrentProcess(), info.data(), DWORD(pages * sizeof(info[0])))) {
        part = part_end;
        continue;
      }
      for (size_t i = 0; i < pages; ++i) {
        const uintptr_t page = part + i * kPage;
        const uintptr_t cell = page & ~(kCell - 1);
        if (out.empty() || out.back().base != cell) {
          Chunk c;
          c.base = cell;
          c.alloc_base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
          c.type = mbi.Type;
          c.protect = mbi.Protect;
          out.push_back(c);
        }
        ++out.back().pages;
        out.back().resident += info[i].VirtualAttributes.Valid ? 1u : 0u;
      }
      part = part_end;
    }
  }
  if (ms_out) {
    *ms_out = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
}

std::string Describe(uintptr_t begin, uintptr_t end, DWORD type, uintptr_t alloc_base) {
  const uint8_t* virt = VirtualBase();
  const uint8_t* phys = PhysicalBase();
  const uintptr_t v = reinterpret_cast<uintptr_t>(virt);
  const uintptr_t p = reinterpret_cast<uintptr_t>(phys);
  if (virt && phys && begin >= v && begin < p + 0x20000000ull) {
    if (begin >= p) {
      return fmt::format("arena physical view, guest physical 0x{:08X}..0x{:08X}", uint32_t(begin - p), uint32_t(end - p));
    }
    const uint64_t guest = begin - v;
    const char* name = guest < 0x40000000ull   ? "v00000000 (4 KiB pages)"
                       : guest < 0x7F000000ull ? "v40000000 (64 KiB pages)"
                       : guest < 0x80000000ull ? "7F000000 writeback"
                       : guest < 0x90000000ull ? "v80000000 (xex, 64 KiB)"
                       : guest < 0xA0000000ull ? "v90000000 (xex, 4 KiB)"
                       : guest < 0xC0000000ull ? "vA0000000 (physical, 64 KiB)"
                       : guest < 0xE0000000ull ? "vC0000000 (physical, 16 MiB)"
                                               : "vE0000000 (physical, 4 KiB)";
    return fmt::format("arena guest virtual 0x{:08X}..0x{:08X} {}", uint32_t(guest), uint32_t(end - v), name);
  }
  const char* kind = type == MEM_IMAGE ? "image" : type == MEM_MAPPED ? "mapped" : "private";
  char module[MAX_PATH] = {};
  if (type == MEM_IMAGE) {
    GetModuleFileNameA(reinterpret_cast<HMODULE>(alloc_base), module, MAX_PATH);
  }
  return fmt::format("{} 0x{:X}..0x{:X} (allocation 0x{:X}){}{}", kind, begin, end, alloc_base, module[0] ? " " : "", module);
}

// The part of the address space a chunk is in, for the totals of a report.
const char* Category(uintptr_t address, DWORD type) {
  const uint8_t* virt = VirtualBase();
  const uint8_t* phys = PhysicalBase();
  const uintptr_t v = reinterpret_cast<uintptr_t>(virt);
  const uintptr_t p = reinterpret_cast<uintptr_t>(phys);
  if (virt && phys && address >= v && address < p + 0x20000000ull) {
    if (address >= p) return "arena physical view (raw)";
    const uint64_t guest = address - v;
    return guest < 0x40000000ull   ? "arena v00000000"
           : guest < 0x80000000ull ? "arena v40000000"
           : guest < 0xA0000000ull ? "arena xex 80000000/90000000"
           : guest < 0xC0000000ull ? "arena vA0000000 (physical, 64 KiB)"
           : guest < 0xE0000000ull ? "arena vC0000000"
                                   : "arena vE0000000 (physical, 4 KiB)";
  }
  return type == MEM_IMAGE ? "image" : type == MEM_MAPPED ? "other mapped" : "private (heap, D3D12, ...)";
}

void Report(const Snapshot& before, const Snapshot& after, uint64_t faults, double burst_ms, double snapshot_ms) {
  struct Run {
    uintptr_t begin = 0, end = 0;
    int64_t delta = 0;
    DWORD type = 0;
    uintptr_t alloc_base = 0;
  };
  struct Total {
    int64_t gained = 0, lost = 0;
    int64_t touched_committed = 0;  // gained pages that were committed but not resident before: a pre-touch could have taken them
    int64_t new_commit = 0;         // gained pages of memory that was committed in the burst (or whose chunk is new)
    int64_t idle_committed_before = 0;  // committed but not resident before the burst (what a pre-touch could take)
  };
  std::vector<Run> runs;
  std::map<std::string, Total> totals;
  size_t i = 0;
  auto lose = [&](const Chunk& b) { totals[Category(b.base, b.type)].lost += b.resident; };
  for (const Chunk& a : after) {
    while (i < before.size() && before[i].base < a.base) {
      lose(before[i]);  // a region that is gone
      ++i;
    }
    int64_t was = 0;
    int64_t was_committed = 0;
    if (i < before.size() && before[i].base == a.base) {
      was = before[i].resident;
      was_committed = before[i].pages;
      ++i;
    }
    Total& total = totals[Category(a.base, a.type)];
    total.idle_committed_before += was_committed - was;
    const int64_t d = int64_t(a.resident) - was;
    if (d > 0) {
      total.gained += d;
      const int64_t prefaultable = std::min<int64_t>(d, was_committed - was);
      total.touched_committed += prefaultable;
      total.new_commit += d - prefaultable;
    } else {
      total.lost -= d;
    }
    if (d == 0) continue;
    if (!runs.empty() && runs.back().end == a.base && runs.back().alloc_base == a.alloc_base && (runs.back().delta > 0) == (d > 0)) {
      runs.back().end = a.base + kChunkPages * kPage;
      runs.back().delta += d;
    } else {
      runs.push_back({a.base, a.base + kChunkPages * kPage, d, a.type, a.alloc_base});
    }
  }
  for (; i < before.size(); ++i) lose(before[i]);
  int64_t gained = 0, lost = 0;
  for (const auto& t : totals) {
    gained += t.second.gained;
    lost += t.second.lost;
  }
  REXLOG_INFO("[mcla-pf] burst: +{} page faults in {:.0f} ms; resident pages +{} -{} ({:+.1f} MiB), snapshot {:.0f} ms, {} runs changed",
              faults, burst_ms, gained, lost, double(gained - lost) * 4.0 / 1024.0, snapshot_ms, runs.size());
  for (const auto& t : totals) {
    if (t.second.gained == 0 && t.second.lost == 0) continue;
    REXLOG_INFO("[mcla-pf]   {:36s} gained {:7} pages ({:6.1f} MiB: {:6.1f} MiB were committed but not resident before, {:6.1f} MiB newly committed), lost {:7}; committed-not-resident before: {:7} pages",
                t.first, t.second.gained, double(t.second.gained) * 4.0 / 1024.0, double(t.second.touched_committed) * 4.0 / 1024.0,
                double(t.second.new_commit) * 4.0 / 1024.0, t.second.lost, t.second.idle_committed_before);
  }
  std::sort(runs.begin(), runs.end(), [](const Run& x, const Run& y) { return std::abs(x.delta) > std::abs(y.delta); });
  for (size_t k = 0; k < runs.size() && k < 8; ++k) {
    const Run& r = runs[k];
    REXLOG_INFO("[mcla-pf]   {:+} pages ({:+.1f} MiB) of {:.1f} MiB: {}", r.delta, double(r.delta) * 4.0 / 1024.0,
                double(r.end - r.begin) / (1024.0 * 1024.0), Describe(r.begin, r.end, r.type, r.alloc_base));
  }
}

void ProbeMain() {
  Snapshot before, after;
  double snapshot_ms = 0.0;
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof(pmc);
  auto faults_now = [&]() -> uint64_t {
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) ? pmc.PageFaultCount : 0;
  };
  uint64_t last = faults_now();
  auto last_snapshot = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  bool in_burst = false;
  uint64_t burst_faults = 0;
  int calm_ticks = 0;
  auto burst_start = std::chrono::steady_clock::now();
  REXLOG_INFO("[mcla-pf] probe on: sampling every {} ms, a burst is {}+ faults", REXCVAR_GET(mcla_pf_probe_ms),
              REXCVAR_GET(mcla_pf_probe_burst));
  while (!g_stop.load(std::memory_order_relaxed)) {
    const int period = std::max(1, REXCVAR_GET(mcla_pf_probe_ms));
    std::this_thread::sleep_for(std::chrono::milliseconds(period));
    const uint64_t now = faults_now();
    const uint64_t delta = now - last;
    last = now;
    const uint64_t threshold = uint64_t(std::max(100, REXCVAR_GET(mcla_pf_probe_burst))) / 3;
    const auto t = std::chrono::steady_clock::now();
    if (!in_burst) {
      if (delta >= threshold) {
        in_burst = true;
        burst_faults = delta;
        calm_ticks = 0;
        burst_start = t;
      } else if (delta < 300 && t - last_snapshot > std::chrono::milliseconds(250)) {
        TakeSnapshot(before, &snapshot_ms);
        last_snapshot = std::chrono::steady_clock::now();
        last = faults_now();
      }
    } else {
      burst_faults += delta;
      calm_ticks = delta < 300 ? calm_ticks + 1 : 0;
      if (calm_ticks >= 4 || t - burst_start > std::chrono::seconds(4)) {
        in_burst = false;
        const double burst_ms = std::chrono::duration<double, std::milli>(t - burst_start).count();
        if (burst_faults >= uint64_t(REXCVAR_GET(mcla_pf_probe_burst)) && !before.empty()) {
          TakeSnapshot(after, &snapshot_ms);
          Report(before, after, burst_faults, burst_ms, snapshot_ms);
          before.swap(after);
        } else {
          before.clear();  // too small a burst to be worth a report, and the snapshot is no longer "before" anything
        }
        last_snapshot = std::chrono::steady_clock::now();
        last = faults_now();
      }
    }
  }
}

// --- Pre-fault ---------------------------------------------------------------------------------------------------

struct View {
  const char* name;
  bool physical;
  uint64_t begin;
  uint64_t end;
};
constexpr View kViews[] = {
    {"phys", true, 0x00000000ull, 0x1FD00000ull}, {"v0", false, 0x00000000ull, 0x40000000ull},
    {"v4", false, 0x40000000ull, 0x7F000000ull},  {"v8", false, 0x80000000ull, 0x90000000ull},
    {"v9", false, 0x90000000ull, 0xA0000000ull},  {"va", false, 0xA0000000ull, 0xC0000000ull},
    {"vc", false, 0xC0000000ull, 0xE0000000ull},  {"ve", false, 0xE0000000ull, 0xFFD00000ull},
};

void PrefaultMain() {
  const std::string wanted = REXCVAR_GET(mcla_prefault_ranges);
  const uint8_t* virt = VirtualBase();
  const uint8_t* phys = PhysicalBase();
  if (!virt || !phys) {
    REXLOG_WARN("[mcla-prefault] the guest memory is not up: nothing touched");
    return;
  }
  const HANDLE self = GetCurrentProcess();
  const auto t0 = std::chrono::steady_clock::now();
  uint64_t pages = 0;
  uint64_t sink = 0;
  std::string done;
  size_t pos = 0;
  while (pos <= wanted.size() && !g_stop.load(std::memory_order_relaxed)) {
    size_t comma = wanted.find(',', pos);
    if (comma == std::string::npos) comma = wanted.size();
    const std::string name = wanted.substr(pos, comma - pos);
    pos = comma + 1;
    const View* view = nullptr;
    for (const View& v : kViews) {
      if (name == v.name) view = &v;
    }
    if (!view) {
      if (!name.empty()) REXLOG_WARN("[mcla-prefault] unknown view '{}' in mcla_prefault_ranges", name);
      continue;
    }
    const uint8_t* base = (view->physical ? phys : virt) + view->begin;
    const uint8_t* end = (view->physical ? phys : virt) + view->end;
    uint64_t view_pages = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (const uint8_t* p = base; p < end && !g_stop.load(std::memory_order_relaxed);) {
      if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
      const uint8_t* region_end = std::min(end, static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize);
      // Committed and readable now. The guest may change the protection or decommit at any moment, so the byte is read
      // by the kernel (ReadProcessMemory on this process fails instead of faulting, and never reaches the SDK's
      // exception handlers); a page that went away meanwhile is simply not counted.
      const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
      if (readable) {
        for (const uint8_t* q = p; q < region_end; q += kPage) {
          if ((view_pages & 0x3FFF) == 0 && g_stop.load(std::memory_order_relaxed)) {
            break;  // the process is shutting down: do not hold it up
          }
          uint8_t byte = 0;
          SIZE_T copied = 0;
          if (ReadProcessMemory(self, q, &byte, 1, &copied) && copied == 1) {
            sink += byte;
            ++view_pages;
          }
        }
      }
      p = region_end;
    }
    pages += view_pages;
    done += (done.empty() ? "" : ", ") + fmt::format("{} {:.1f} MiB", view->name, double(view_pages) * 4.0 / 1024.0);
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REXLOG_INFO("[mcla-prefault] touched {:.1f} MiB ({} pages; {}) in {:.0f} ms (sum {})", double(pages) * 4.0 / 1024.0,
              pages, done, ms, sink & 1);
}

}  // namespace

void StartPfProbe() {
  g_stop.store(false);
  if (REXCVAR_GET(mcla_pf_probe_ms) > 0) {
    g_probe_thread = std::thread(ProbeMain);
  }
  if (REXCVAR_GET(mcla_prefault_guest)) {
    g_prefault_thread = std::thread(PrefaultMain);
  }
}

void StopPfProbe() {
  g_stop.store(true);
  if (g_probe_thread.joinable()) g_probe_thread.join();
  if (g_prefault_thread.joinable()) g_prefault_thread.join();
}

}  // namespace mcla_host
