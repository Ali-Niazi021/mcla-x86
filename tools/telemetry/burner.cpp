// R6 GPU burner: D3D11 compute ALU load for N seconds. Usage: burner.exe [seconds=30] [cpu_threads=0]
// cpu_threads>0 additionally spins that many CPU threads (to test CPU-load -> GPU-clock coupling).
// Aborts at once if mcla.exe appears (never compete with a Verifier run).
#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

static const char* kCS = R"(
RWStructuredBuffer<float4> buf : register(u0);
[numthreads(256,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
  float4 v = buf[id.x];
  [loop] for (int i = 0; i < 512; ++i) { v = v * 1.0001f + float4(0.3f, 0.7f, 1.1f, 1.3f); v = v * v * 0.5f + v * 0.25f; }
  buf[id.x] = v;
}
)";

static std::atomic<bool> g_stop{false};

static bool gameRunning() {
    bool r = false; HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); PROCESSENTRY32W e{sizeof(e)};
    if (Process32FirstW(s, &e)) do { if (!_wcsicmp(e.szExeFile, L"mcla.exe")) { r = true; break; } } while (Process32NextW(s, &e));
    CloseHandle(s); return r;
}

int main(int argc, char** argv) {
    double secs = argc > 1 ? atof(argv[1]) : 30;
    int cpu = argc > 2 ? atoi(argv[2]) : 0;
    if (gameRunning()) { printf("ABORTED(mcla.exe) before start\n"); return 3; }
    ID3D11Device* dev; ID3D11DeviceContext* ctx; D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx))) return 1;
    ID3DBlob *b = nullptr, *e = nullptr;
    if (FAILED(D3DCompile(kCS, strlen(kCS), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &b, &e))) { printf("%s\n", (char*)e->GetBufferPointer()); return 2; }
    ID3D11ComputeShader* cs; dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &cs);
    const UINT N = 1 << 20;
    D3D11_BUFFER_DESC bd{N * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
    ID3D11Buffer* buf; dev->CreateBuffer(&bd, nullptr, &buf);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = N;
    ID3D11UnorderedAccessView* uav; dev->CreateUnorderedAccessView(buf, &ud, &uav);
    ctx->CSSetShader(cs, nullptr, 0); ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0}; ID3D11Query* qy; dev->CreateQuery(&qd, &qy);

    std::vector<std::thread> th;
    for (int i = 0; i < cpu; ++i) th.emplace_back([] { volatile double x = 1; while (!g_stop) x = x * 1.0000001 + 1e-9; });

    LARGE_INTEGER fq, t0, t; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    long long batches = 0; double lastChk = 0; bool aborted = false;
    for (;;) {
        QueryPerformanceCounter(&t);
        double el0 = double(t.QuadPart - t0.QuadPart) / fq.QuadPart;
        if (el0 > secs) break;
        if (el0 - lastChk > 0.3) { lastChk = el0; if (gameRunning()) { aborted = true; break; } }
        ctx->Dispatch(N / 256, 1, 1);
        ctx->Dispatch(N / 256, 1, 1);
        ctx->End(qy);
        BOOL done = FALSE; while (ctx->GetData(qy, &done, sizeof(done), 0) != S_OK) Sleep(0);
        ++batches;
    }
    g_stop = true; for (auto& x : th) x.join();
    double el = double(t.QuadPart - t0.QuadPart) / fq.QuadPart;
    printf("%sbatches=%lld ms/batch=%.3f secs=%.2f cpu_threads=%d\n", aborted ? "ABORTED(mcla.exe) " : "", batches, batches ? el * 1000 / batches : 0, el, cpu);
    return 0;
}
