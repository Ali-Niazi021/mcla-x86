// R6 telemetry sampler: ADLX GPU + system metrics at fixed rate -> CSV.
// Usage: sampler.exe <out.csv> <seconds> [interval_ms=100] [watch_process=mcla.exe]
// Also logs: mcla.exe CPU (cores), system CPU util/perf% (PDH), power scheme + overlay.
#include "SDK/ADLXHelper/Windows/Cpp/ADLXHelper.h"
#include "SDK/Include/IPerformanceMonitoring3.h"
#include "SDK/Include/IPerformanceMonitoring1.h"
#include "SDK/Include/IGPUTuning.h"
#include "SDK/Include/IGPUManualPowerTuning.h"
#include "SDK/Include/IGPUManualGFXTuning.h"
#include "SDK/Include/IPowerTuning.h"
#include "SDK/Include/ISystem1.h"
#include <windows.h>
#include <tlhelp32.h>
#include <powrprof.h>
#include <pdh.h>
#include <cstdio>
#include <string>
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "pdh.lib")

using namespace adlx;
static ADLXHelper g;

typedef DWORD(WINAPI* PFN_Overlay)(GUID*);

static void guidstr(const GUID& gd, char* out) {
    sprintf(out, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", gd.Data1, gd.Data2, gd.Data3,
            gd.Data4[0], gd.Data4[1], gd.Data4[2], gd.Data4[3], gd.Data4[4], gd.Data4[5], gd.Data4[6], gd.Data4[7]);
}

static DWORD findPid(const wchar_t* name) {
    DWORD pid = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{sizeof(e)};
    if (Process32FirstW(s, &e)) do {
        if (_wcsicmp(e.szExeFile, name) == 0) { pid = e.th32ProcessID; break; }
    } while (Process32NextW(s, &e));
    CloseHandle(s);
    return pid;
}

static unsigned long long ft(const FILETIME& f) { return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime; }

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: sampler out.csv seconds [interval_ms] [proc]\n"); return 1; }
    const char* out = argv[1];
    double secs = atof(argv[2]);
    int ivl = argc > 3 ? atoi(argv[3]) : 100;
    wchar_t proc[260] = L"mcla.exe";
    if (argc > 4) MultiByteToWideChar(CP_ACP, 0, argv[4], -1, proc, 260);
    timeBeginPeriod(1);

    FILE* f = fopen(out, "w");
    if (!f) return 2;

    // Power scheme + overlay
    GUID* act = nullptr; char gs[64] = "?", os[64] = "?";
    if (PowerGetActiveScheme(NULL, &act) == 0) { guidstr(*act, gs); LocalFree(act); }
    HMODULE pp = LoadLibraryA("powrprof.dll");
    PFN_Overlay ov = (PFN_Overlay)GetProcAddress(pp, "PowerGetEffectiveOverlayScheme");
    GUID og{}; if (ov && ov(&og) == 0) guidstr(og, os);
    SYSTEM_POWER_STATUS sps{}; GetSystemPowerStatus(&sps);
    fprintf(f, "# scheme=%s overlay=%s ac=%d\n", gs, os, sps.ACLineStatus);
    printf("scheme=%s overlay=%s ac=%d\n", gs, os, sps.ACLineStatus);

    if (ADLX_FAILED(g.Initialize())) { printf("ADLX init failed\n"); return 3; }
    IADLXPerformanceMonitoringServicesPtr pms;
    g.GetSystemServices()->GetPerformanceMonitoringServices(&pms);
    IADLXGPUListPtr gpus; g.GetSystemServices()->GetGPUs(&gpus);
    IADLXGPUPtr gpu; gpus->At(gpus->Begin(), &gpu);
    const char* nm = nullptr; gpu->Name(&nm);
    fprintf(f, "# gpu=%s\n", nm); printf("gpu=%s\n", nm);
    pms->SetSamplingInterval(ivl < 100 ? 100 : ivl);
    adlx_int si = 0; pms->GetSamplingInterval(&si);
    fprintf(f, "# adlx_sampling_ms=%d\n", si);

    // Ranges
    IADLXGPUMetricsSupportPtr ms; pms->GetSupportedGPUMetrics(gpu, &ms);
    if (ms) {
        adlx_int a = 0, b = 0;
        if (ADLX_SUCCEEDED(ms->GetGPUClockSpeedRange(&a, &b))) { fprintf(f, "# gfxclk_range=%d..%d\n", a, b); printf("gfxclk_range=%d..%d\n", a, b); }
        if (ADLX_SUCCEEDED(ms->GetGPUVRAMClockSpeedRange(&a, &b))) { fprintf(f, "# memclk_range=%d..%d\n", a, b); printf("memclk_range=%d..%d\n", a, b); }
        if (ADLX_SUCCEEDED(ms->GetGPUPowerRange(&a, &b))) { fprintf(f, "# gpupower_range=%d..%d\n", a, b); printf("gpupower_range=%d..%d\n", a, b); }
        if (ADLX_SUCCEEDED(ms->GetGPUTotalBoardPowerRange(&a, &b))) { fprintf(f, "# tbp_range=%d..%d\n", a, b); printf("tbp_range=%d..%d\n", a, b); }
        if (ADLX_SUCCEEDED(ms->GetGPUTemperatureRange(&a, &b))) { fprintf(f, "# temp_range=%d..%d\n", a, b); }
        adlx_bool s = false;
        ms->IsSupportedGPUClockSpeed(&s); fprintf(f, "# sup_clk=%d", s);
        ms->IsSupportedGPUPower(&s); fprintf(f, " sup_pwr=%d", s);
        ms->IsSupportedGPUTotalBoardPower(&s); fprintf(f, " sup_tbp=%d", s);
        ms->IsSupportedGPUHotspotTemperature(&s); fprintf(f, " sup_hot=%d", s);
        ms->IsSupportedGPUVoltage(&s); fprintf(f, " sup_volt=%d\n", s);
    }
    IADLXSystemMetricsSupportPtr sms; pms->GetSupportedSystemMetrics(&sms);
    IADLXSystemMetricsSupport1Ptr sms1(sms);
    adlx_bool supPD = false, supSS = false;
    if (sms1) sms1->IsSupportedPowerDistribution(&supPD);
    if (sms) sms->IsSupportedSmartShift(&supSS);
    fprintf(f, "# sup_powerdist=%d sup_smartshift=%d\n", supPD, supSS);
    printf("sup_powerdist=%d sup_smartshift=%d\n", supPD, supSS);

    // Tuning info (power limit, max freq) if exposed
    IADLXGPUTuningServicesPtr ts; g.GetSystemServices()->GetGPUTuningServices(&ts);
    if (ts) {
        adlx_bool s = false;
        ts->IsSupportedManualPowerTuning(gpu, &s); fprintf(f, "# sup_manual_power_tuning=%d", s);
        ts->IsSupportedManualGFXTuning(gpu, &s); fprintf(f, " sup_manual_gfx_tuning=%d", s);
        ts->IsSupportedAutoTuning(gpu, &s); fprintf(f, " sup_auto_tuning=%d", s);
        ts->IsSupportedPresetTuning(gpu, &s); fprintf(f, " sup_preset_tuning=%d\n", s);
        IADLXInterfacePtr ifc;
        if (ADLX_SUCCEEDED(ts->GetManualPowerTuning(gpu, &ifc))) {
            IADLXManualPowerTuningPtr mp(ifc); adlx_int v = 0; ADLX_IntRange r{};
            if (mp && ADLX_SUCCEEDED(mp->GetPowerLimit(&v))) { mp->GetPowerLimitRange(&r); fprintf(f, "# powerlimit=%d range=%d..%d\n", v, r.minValue, r.maxValue); }
        }
    }
    IADLXPowerTuningServicesPtr pts;
    { IADLXSystem1Ptr s1; if (ADLX_SUCCEEDED(g.GetSystemServices()->QueryInterface(IADLXSystem1::IID(), reinterpret_cast<void**>(&s1))) && s1) s1->GetPowerTuningServices(&pts); }
    if (pts) {
        IADLXSmartShiftMaxPtr ssm; adlx_bool s = false;
        if (ADLX_SUCCEEDED(pts->GetSmartShiftMax(&ssm)) && ssm) {
            ssm->IsSupported(&s); fprintf(f, "# smartshift_max_supported=%d\n", s);
        }
    }
    fflush(f);

    // PDH: system CPU utility and performance % (effective freq vs base)
    PDH_HQUERY q; PdhOpenQueryW(NULL, 0, &q);
    PDH_HCOUNTER cUtil = 0, cPerf = 0, cFreq = 0;
    PdhAddEnglishCounterW(q, L"\\Processor Information(_Total)\\% Processor Utility", 0, &cUtil);
    PdhAddEnglishCounterW(q, L"\\Processor Information(_Total)\\% Processor Performance", 0, &cPerf);
    PdhAddEnglishCounterW(q, L"\\Processor Information(_Total)\\Processor Frequency", 0, &cFreq);
    PdhCollectQueryData(q);

    fprintf(f, "t_s,gfx_mhz,gpu_util,mem_mhz,gpu_w,tbp_w,temp_c,hot_c,volt_mv,shared_mb,sys_cpu_util_adlx,smartshift,apu_shift,gpu_shift,apu_lim,gpu_lim,tot_lim,proc_cores,pdh_cpu_util,pdh_cpu_perf,pdh_cpu_mhz\n");

    DWORD pid = 0; HANDLE hp = NULL; unsigned long long lastProc = 0;
    LARGE_INTEGER fq, t0, tl; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0); tl = t0;
    int n = 0;
    for (;;) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double t = double(now.QuadPart - t0.QuadPart) / fq.QuadPart;
        if (t > secs) break;
        if (!hp) {
            pid = findPid(proc);
            if (pid) { hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); lastProc = 0; }
        }
        double cores = -1;
        if (hp) {
            FILETIME c, e, k, u;
            DWORD ec = 0;
            if (GetExitCodeProcess(hp, &ec) && ec == STILL_ACTIVE && GetProcessTimes(hp, &c, &e, &k, &u)) {
                unsigned long long tot = ft(k) + ft(u);
                double dt = double(now.QuadPart - tl.QuadPart) / fq.QuadPart;
                if (lastProc && dt > 0) cores = (tot - lastProc) / 1e7 / dt;
                lastProc = tot;
            } else { CloseHandle(hp); hp = NULL; }
        }
        tl = now;

        IADLXGPUMetricsPtr gm; pms->GetCurrentGPUMetrics(gpu, &gm);
        adlx_int clk = -1, mclk = -1, volt = -1, shared = -1;
        adlx_double use = -1, pw = -1, tbp = -1, tc = -1, hot = -1;
        if (gm) {
            gm->GPUClockSpeed(&clk); gm->GPUUsage(&use); gm->GPUVRAMClockSpeed(&mclk);
            gm->GPUPower(&pw); gm->GPUTotalBoardPower(&tbp); gm->GPUTemperature(&tc);
            gm->GPUHotspotTemperature(&hot); gm->GPUVoltage(&volt);
            IADLXGPUMetrics2Ptr gm2(gm); if (gm2) gm2->GPUSharedMemory(&shared);
        }
        IADLXSystemMetricsPtr sm; pms->GetCurrentSystemMetrics(&sm);
        adlx_double scpu = -1; adlx_int ss = -1, a1 = -1, a2 = -1, a3 = -1, a4 = -1, a5 = -1;
        if (sm) {
            sm->CPUUsage(&scpu); if (supSS) sm->SmartShift(&ss);
            IADLXSystemMetrics1Ptr sm1(sm); if (sm1 && supPD) sm1->PowerDistribution(&a1, &a2, &a3, &a4, &a5);
        }
        PdhCollectQueryData(q);
        PDH_FMT_COUNTERVALUE v1{}, v2{}, v3{};
        PdhGetFormattedCounterValue(cUtil, PDH_FMT_DOUBLE, NULL, &v1);
        PdhGetFormattedCounterValue(cPerf, PDH_FMT_DOUBLE, NULL, &v2);
        PdhGetFormattedCounterValue(cFreq, PDH_FMT_DOUBLE, NULL, &v3);
        fprintf(f, "%.3f,%d,%.1f,%d,%.2f,%.2f,%.1f,%.1f,%d,%d,%.1f,%d,%d,%d,%d,%d,%d,%.2f,%.1f,%.1f,%.0f\n", t, clk, use, mclk, pw, tbp, tc, hot, volt, shared,
                scpu, ss, a1, a2, a3, a4, a5, cores, v1.doubleValue, v2.doubleValue, v3.doubleValue);
        if (++n % 10 == 0) fflush(f);
        // sleep to next tick
        double next = (n) * ivl / 1000.0;
        LARGE_INTEGER n2; QueryPerformanceCounter(&n2);
        double el = double(n2.QuadPart - t0.QuadPart) / fq.QuadPart;
        if (next > el) Sleep(DWORD((next - el) * 1000));
    }
    fclose(f);
    PdhCloseQuery(q);
    if (hp) CloseHandle(hp);
    printf("done %d samples\n", n);
    fflush(stdout);
    // x86 port: the original called g.Terminate() here and returned. The ADLX smart pointers of this function (pms, gpu, ...) are
    // still alive then, and their destructors touched a terminated ADLX: the process ended with 0xC0000005 after the CSV was
    // closed. Leave without running them (the OS releases everything).
    ExitProcess(0);
}
