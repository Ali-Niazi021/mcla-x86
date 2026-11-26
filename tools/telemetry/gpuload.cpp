// gpuload: a foreign GPU load with a duty cycle, for A/B runs inside one game run (tools/run_game.py `<s>:burngpu <pct>`).
//   gpuload.exe <duty_pct 1..100> [parent_pid]
// Every 10 ms period it submits D3D11 compute (the ALU loop of burner.cpp) worth about duty % of the period, waits for it
// to finish (an event query, polled with timed sleeps), and sleeps the rest. Unlike burner.exe it does NOT stop when mcla.exe
// runs: competing with the game is the point. It exits when parent_pid is gone (checked every ~0.5 s), on Ctrl-C / console
// close, or when the device is lost.
//
// How the duty is kept: the work is a number of equal "units" (one dispatch of G thread groups, sized at start to ~0.4 ms of
// GPU time). Each period adds duty * (wall time since the last period) to a credit (ms of GPU time), submits floor(credit /
// unit) units between two GPU timestamps, waits, and subtracts the GPU time those units really took (timestamps; the wall
// time when they are unusable). So the achieved busy time follows the target whatever the GPU clock does (the measured time,
// not the calibrated one, is what is charged), and a duty below one unit per period still gets its work, spread over several
// periods. The unit estimate follows the measurements (EMA) and, once a second, the group count is rescaled when a unit has
// drifted far from its target time (the GPU clock moved). On this driver the disjoint query reads as ready at once and the
// timestamp queries only when the work is done, hence the event query for the wait.
//
// Output (stdout, flushed): one start line, then one line every 5 s with the achieved busy % (sum of measured GPU batch time
// over the wall time of that window):
//   gpuload duty=30% batch_ms=0.398 batches/period=7.54 adapter=AMD Radeon(TM) 880M Graphics
//   gpuload t=5.0 busy=30.0% duty=30% unit_ms=0.400 groups=218 periods=486
// Adapter: DXGI adapter 0 (the default one the game uses), unless that is a software adapter; then the hardware adapter with
// the most dedicated + shared memory. Process and thread priority are normal. Exit code 0 normal, 1 usage, 2 shader, 3 no
// adapter / device, 4 device error.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")

static const char* kCS = R"(
RWStructuredBuffer<float4> buf : register(u0);
[numthreads(256,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
  float4 v = buf[id.x];
  [loop] for (int i = 0; i < 512; ++i) { v = v * 1.0001f + float4(0.3f, 0.7f, 1.1f, 1.3f); v = v * v * 0.5f + v * 0.25f; }
  buf[id.x] = v;
}
)";

static const UINT kBufElems = 1u << 20;       // float4 elements: 4096 groups of 256 threads at most
static const int kMaxGroups = int(kBufElems / 256);
static const int kMinGroups = 4;
static const double kPeriodMs = 10.0;
static const double kUnitTargetMs = 0.4;      // GPU time of one unit (one dispatch) after calibration
static const double kReportS = 5.0;

static std::atomic<bool> g_stop{false};
static HANDLE g_timer = nullptr;              // high-resolution waitable timer (Windows 10 1803+), null = Sleep(1) fallback
static LARGE_INTEGER g_freq;

static BOOL WINAPI CtrlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT: case CTRL_BREAK_EVENT: case CTRL_CLOSE_EVENT: case CTRL_LOGOFF_EVENT: case CTRL_SHUTDOWN_EVENT:
            g_stop = true;
            if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) return TRUE;
            Sleep(1500);  // close / logoff: the process ends when this returns; let the loop finish its batch first
            return TRUE;
        default: return FALSE;
    }
}

static double NowMs() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(g_freq.QuadPart);
}

// Sleep for about `ms` (a timed wait, no spinning).
static void SleepMs(double ms) {
    if (ms <= 0) return;
    if (g_timer) {
        LARGE_INTEGER due; due.QuadPart = -LONGLONG(ms * 10000.0);  // 100 ns units, negative = relative
        if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(g_timer, 1000); return; }
    }
    Sleep(ms < 1.0 ? 1 : DWORD(ms));
}

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

static std::string Utf8(const wchar_t* w) {
    char buf[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, int(sizeof(buf)) - 1, nullptr, nullptr);
    return buf;
}

struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    ID3D11Buffer* buf = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    ID3D11Query *qDisjoint = nullptr, *qTs0 = nullptr, *qTs1 = nullptr, *qEvent = nullptr;
    std::string adapter;
};

struct Batch { double gpu_ms = 0, wall_ms = 0; bool ok = false; };

// Submits `units` dispatches of `groups` groups, waits for them, and returns the GPU time they took (timestamps; the wall time
// from submit to completion when timestamps are unusable).
static Batch RunBatch(Gpu& g, int units, int groups) {
    Batch b;
    g.ctx->Begin(g.qDisjoint);
    g.ctx->End(g.qTs0);
    for (int i = 0; i < units; ++i) g.ctx->Dispatch(UINT(groups), 1, 1);
    g.ctx->End(g.qTs1);
    g.ctx->End(g.qDisjoint);
    g.ctx->End(g.qEvent);
    const double t0 = NowMs();
    g.ctx->Flush();
    // Completion is the EVENT query (this driver reports the disjoint query as ready at once, long before the work is done).
    BOOL done = FALSE;
    for (;;) {
        const HRESULT hr = g.ctx->GetData(g.qEvent, &done, sizeof(done), 0);
        if (hr == S_OK) break;
        if (FAILED(hr) || NowMs() - t0 > 10000.0) return b;      // device removed or hung
        SleepMs(0.3);
    }
    b.wall_ms = NowMs() - t0;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    UINT64 s0 = 0, s1 = 0;
    const bool got = g.ctx->GetData(g.qDisjoint, &dj, sizeof(dj), 0) == S_OK && g.ctx->GetData(g.qTs0, &s0, sizeof(s0), 0) == S_OK &&
                     g.ctx->GetData(g.qTs1, &s1, sizeof(s1), 0) == S_OK;
    // A usable timestamp pair gives the GPU time; otherwise (a disjoint query: the GPU clock changed during the batch) this
    // batch is charged its wall time. A batch cannot have run longer than the wall time it was waited for (plus the poll
    // granularity), and one that was waited for a long time cannot have taken far less: a value outside that band means the
    // timestamps are not trustworthy, and the wall time is used instead.
    b.gpu_ms = b.wall_ms;
    if (got && !dj.Disjoint && dj.Frequency > 0 && s1 > s0) {
        const double ms = double(s1 - s0) * 1000.0 / double(dj.Frequency);
        if (ms <= b.wall_ms * 1.5 + 2.0 && ms >= b.wall_ms * 0.5 - 0.5) b.gpu_ms = ms;
    }
    b.ok = true;
    return b;
}

static bool CreateGpu(Gpu& g) {
    IDXGIFactory1* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&fac)))) return false;
    IDXGIAdapter1* pick = nullptr;
    size_t bestMem = 0;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (fac->EnumAdapters1(i, &a) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        const bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == 0x1414 ||
                              wcsstr(d.Description, L"Microsoft Basic Render") != nullptr;
        if (i == 0 && !software) { pick = a; break; }          // the default adapter, as the game's D3D12 device
        const size_t mem = d.DedicatedVideoMemory + d.SharedSystemMemory;
        if (!software && (!pick || mem > bestMem)) { Rel(pick); pick = a; bestMem = mem; } else { a->Release(); }
    }
    Rel(fac);
    if (!pick) return false;
    DXGI_ADAPTER_DESC1 d{};
    pick->GetDesc1(&d);
    g.adapter = Utf8(d.Description);
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL fl{};
    const HRESULT hr = D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, want, 2, D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx);
    Rel(pick);
    return SUCCEEDED(hr);
}

// Device, shader, buffer, queries; 0 or the exit code.
static int SetupGpu(Gpu& g) {
    if (!CreateGpu(g)) { std::printf("gpuload: no hardware D3D11 adapter / device\n"); return 3; }
    ID3DBlob *blob = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kCS, std::strlen(kCS), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, &err))) {
        std::printf("gpuload: shader: %s\n", err ? static_cast<char*>(err->GetBufferPointer()) : "?");
        return 2;
    }
    D3D11_BUFFER_DESC bd{kBufElems * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = kBufElems;
    const D3D11_QUERY_DESC qdj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, qts{D3D11_QUERY_TIMESTAMP, 0}, qev{D3D11_QUERY_EVENT, 0};
    if (FAILED(g.dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g.cs)) ||
        FAILED(g.dev->CreateBuffer(&bd, nullptr, &g.buf)) || FAILED(g.dev->CreateUnorderedAccessView(g.buf, &ud, &g.uav)) ||
        FAILED(g.dev->CreateQuery(&qdj, &g.qDisjoint)) || FAILED(g.dev->CreateQuery(&qts, &g.qTs0)) ||
        FAILED(g.dev->CreateQuery(&qts, &g.qTs1)) || FAILED(g.dev->CreateQuery(&qev, &g.qEvent))) {
        std::printf("gpuload: cannot create the D3D11 resources\n");
        return 3;
    }
    Rel(blob); Rel(err);
    g.ctx->CSSetShader(g.cs, nullptr, 0);
    g.ctx->CSSetUnorderedAccessViews(0, 1, &g.uav, nullptr);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::printf("usage: gpuload.exe <duty_pct 1..100> [parent_pid]\n");
        return 1;
    }
    char* end = nullptr;
    const double duty_pct = std::strtod(argv[1], &end);
    if (*argv[1] == 0 || *end != 0 || !(duty_pct >= 1.0 && duty_pct <= 100.0)) {
        std::printf("gpuload: duty must be 1..100, got '%s'\n", argv[1]);
        return 1;
    }
    HANDLE parent = nullptr;
    if (argc == 3) {
        const long pid = std::strtol(argv[2], &end, 10);
        if (*argv[2] == 0 || *end != 0 || pid <= 0) { std::printf("gpuload: bad parent_pid '%s'\n", argv[2]); return 1; }
        parent = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
        if (!parent) { std::printf("gpuload: parent %ld is not running: nothing to do\n", pid); return 0; }
    }
    const double duty = duty_pct / 100.0;

    QueryPerformanceFrequency(&g_freq);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_timer) timeBeginPeriod(1);

    Gpu g;
    if (const int rc = SetupGpu(g)) return rc;

    // Calibration: the first batches also pay the driver's one-time costs and the GPU clock ramps up, so warm up, then size
    // the unit (the group count) until one dispatch takes about kUnitTargetMs (two rounds in a row within 25 %, at most 16
    // rounds; the per-second rescale in the loop below keeps it there), and take the median of a few batches as the estimate.
    int groups = 128;
    double unit_ms = kUnitTargetMs;
    for (int i = 0, ok_rounds = 0; i < 16 && ok_rounds < 2; ++i) {
        const Batch b = RunBatch(g, 4, groups);
        if (!b.ok) { std::printf("gpuload: device error during calibration\n"); return 4; }
        const double per = b.gpu_ms / 4;
        if (i < 2 || per <= 1e-4) continue;                      // warm-up rounds
        const double ratio = kUnitTargetMs / per;
        ok_rounds = (ratio > 0.8 && ratio < 1.25) ? ok_rounds + 1 : 0;
        groups = std::clamp(int(groups * ratio + 0.5), kMinGroups, kMaxGroups);
    }
    {
        double v[7];
        for (double& x : v) {
            const Batch b = RunBatch(g, 8, groups);
            if (!b.ok) { std::printf("gpuload: device error during calibration\n"); return 4; }
            x = b.gpu_ms / 8;
        }
        std::sort(v, v + 7);
        unit_ms = std::max(v[3], 0.005);
    }
    std::printf("gpuload duty=%g%% batch_ms=%.3f batches/period=%.2f adapter=%s\n", duty_pct, unit_ms,
                duty * kPeriodMs / unit_ms, g.adapter.c_str());
    std::fflush(stdout);

    double credit = 0;                          // ms of GPU time owed to the target, not yet submitted
    const double t_begin = NowMs();
    double last_credit_t = t_begin;
    double next_parent_check = t_begin + 500.0;
    double win_start = t_begin, win_busy = 0;   // the 5 s report window
    double sec_start = t_begin, sec_busy = 0, sec_units = 0;   // the 1 s recalibration window
    long win_periods = 0;
    int exit_code = 0;

    while (!g_stop) {
        const double t_period = NowMs();
        if (t_period >= next_parent_check) {
            next_parent_check = t_period + 500.0;
            if (parent && WaitForSingleObject(parent, 0) != WAIT_TIMEOUT) break;
        }
        credit += duty * (t_period - last_credit_t);
        last_credit_t = t_period;
        credit = std::min(credit, 2.0 * kPeriodMs);
        const int n = std::max(0, int(std::floor(credit / unit_ms)));
        if (n > 0) {
            const Batch b = RunBatch(g, n, groups);
            if (!b.ok) { std::printf("gpuload: device error (removed or hung)\n"); exit_code = 4; break; }
            credit -= b.gpu_ms;
            credit = std::max(credit, -2.0 * kPeriodMs);
            win_busy += b.gpu_ms; sec_busy += b.gpu_ms; sec_units += n;
            unit_ms = std::max(0.005, 0.75 * unit_ms + 0.25 * (b.gpu_ms / n));
        }
        ++win_periods;

        const double now = NowMs();
        if (now - sec_start >= 1000.0) {         // once a second: rescale the unit when the GPU clock moved it far off target
            if (sec_units > 0) {
                const double mean = sec_busy / sec_units;
                if (mean > 2.0 * kUnitTargetMs || mean < 0.5 * kUnitTargetMs) {
                    const int ng = std::clamp(int(groups * kUnitTargetMs / mean + 0.5), kMinGroups, kMaxGroups);
                    if (ng != groups) { unit_ms = std::max(0.005, mean * ng / groups); groups = ng; }
                } else {
                    unit_ms = std::max(0.005, mean);
                }
            }
            sec_start = now; sec_busy = 0; sec_units = 0;
        }
        if (now - win_start >= kReportS * 1000.0) {
            std::printf("gpuload t=%.1f busy=%.1f%% duty=%g%% unit_ms=%.3f groups=%d periods=%ld\n", (now - t_begin) / 1000.0,
                        100.0 * win_busy / (now - win_start), duty_pct, unit_ms, groups, win_periods);
            std::fflush(stdout);
            win_start = now; win_busy = 0; win_periods = 0;
        }
        SleepMs(t_period + kPeriodMs - NowMs());     // the rest of the period (nothing when the batch ran over)
    }

    g.ctx->ClearState();
    g.ctx->Flush();
    Rel(g.qEvent); Rel(g.qTs1); Rel(g.qTs0); Rel(g.qDisjoint); Rel(g.uav); Rel(g.buf); Rel(g.cs); Rel(g.ctx); Rel(g.dev);
    if (parent) CloseHandle(parent);
    std::printf("gpuload: stopped%s\n", exit_code ? " (error)" : "");
    std::fflush(stdout);
    return exit_code;
}
