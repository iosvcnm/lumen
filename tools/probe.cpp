// 开发辅助：打印 DDC/CI 枚举结果
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <highlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <vector>
#include <stdio.h>

static BOOL CALLBACK En(HMONITOR h, HDC, LPRECT, LPARAM lp) {
    ((std::vector<HMONITOR>*)lp)->push_back(h);
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    std::vector<HMONITOR> hs;
    EnumDisplayMonitors(nullptr, nullptr, En, (LPARAM)&hs);
    wprintf(L"monitors: %d\n", (int)hs.size());

    int want = -1;
    if (argc >= 3 && !_wcsicmp(argv[1], L"set")) want = _wtoi(argv[2]);

    int idx = 0;
    for (HMONITOR h : hs) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(h, (MONITORINFO*)&mi)) continue;
        DWORD cnt = 0;
        GetNumberOfPhysicalMonitorsFromHMONITOR(h, &cnt);
        wprintf(L"[%d] %ls physical=%lu primary=%d\n", idx, mi.szDevice, cnt,
                (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0);
        if (cnt) {
            std::vector<PHYSICAL_MONITOR> v(cnt);
            if (GetPhysicalMonitorsFromHMONITOR(h, cnt, v.data())) {
                for (DWORD i = 0; i < cnt; ++i) {
                    DWORD mn = 0, cur = 0, mx = 0;
                    BOOL ok = GetMonitorBrightness(v[i].hPhysicalMonitor, &mn, &cur, &mx);
                    wprintf(L"    phys %lu  ok=%d  min=%lu cur=%lu max=%lu  name=%ls\n",
                            i, ok, mn, cur, mx, v[i].szPhysicalMonitorDescription);
                    if (want >= 0 && ok && mx > mn) {
                        DWORD raw = mn + (DWORD)((want * (mx - mn) + 50) / 100);
                        BOOL wr = SetMonitorBrightness(v[i].hPhysicalMonitor, raw);
                        wprintf(L"    -> set %lu (%d%%)  ok=%d\n", raw, want, wr);
                    }
                }
                DestroyPhysicalMonitors(cnt, v.data());
            }
        }
        ++idx;
    }
    return 0;
}
