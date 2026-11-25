// R6 per-thread CPU of a process over a window. Usage: threadtop.exe [proc=mcla.exe] [seconds=10]
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <map>
#include <vector>
#include <string>
#include <algorithm>

static unsigned long long ft(const FILETIME& f) { return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static DWORD findPid(const wchar_t* n) {
    DWORD pid = 0; HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); PROCESSENTRY32W e{sizeof(e)};
    if (Process32FirstW(s, &e)) do { if (!_wcsicmp(e.szExeFile, n)) { pid = e.th32ProcessID; break; } } while (Process32NextW(s, &e));
    CloseHandle(s); return pid;
}

struct T { unsigned long long cpu = 0, cyc = 0; std::wstring name; };

static std::map<DWORD, T> snap(DWORD pid) {
    std::map<DWORD, T> m;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 e{sizeof(e)};
    if (Thread32First(s, &e)) do {
        if (e.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, e.th32ThreadID);
        if (!h) continue;
        FILETIME c, x, k, u; T t;
        if (GetThreadTimes(h, &c, &x, &k, &u)) t.cpu = ft(k) + ft(u);
        ULONG64 cy = 0; QueryThreadCycleTime(h, &cy); t.cyc = cy;
        PWSTR d = nullptr; if (SUCCEEDED(GetThreadDescription(h, &d)) && d) { t.name = d; LocalFree(d); }
        m[e.th32ThreadID] = t; CloseHandle(h);
    } while (Thread32Next(s, &e));
    CloseHandle(s); return m;
}

int main(int argc, char** argv) {
    wchar_t n[260] = L"mcla.exe"; if (argc > 1) MultiByteToWideChar(CP_ACP, 0, argv[1], -1, n, 260);
    double secs = argc > 2 ? atof(argv[2]) : 10;
    DWORD pid = findPid(n); if (!pid) { printf("not running\n"); return 1; }
    auto a = snap(pid); Sleep(DWORD(secs * 1000)); auto b = snap(pid);
    std::vector<std::pair<double, DWORD>> v; double tot = 0, totcy = 0;
    for (auto& [id, t] : b) { auto it = a.find(id); if (it == a.end()) continue; double c = (t.cpu - it->second.cpu) / 1e7 / secs; tot += c; totcy += (t.cyc - it->second.cyc) / secs; v.push_back({c, id}); }
    std::sort(v.rbegin(), v.rend());
    printf("pid %lu threads %zu total %.2f cores, %.2f Gcycles/s\n", pid, b.size(), tot, totcy / 1e9);
    for (size_t i = 0; i < v.size() && i < 20; ++i) {
        auto& t = b[v[i].second]; auto& t0 = a[v[i].second];
        printf("%6.1f%%  %5.2f Gcyc/s  tid %6lu  %ls\n", v[i].first * 100, (t.cyc - t0.cyc) / secs / 1e9, v[i].second, t.name.c_str());
    }
    return 0;
}
