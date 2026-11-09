#include <windows.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <stdio.h>
#include <vector>

static BOOL CALLBACK MonEnum(HMONITOR h, HDC, LPRECT, LPARAM p) {
    ((std::vector<HMONITOR>*)p)->push_back(h);
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    BYTE code = 0xD6;
    if (argc > 1) code = (BYTE)wcstoul(argv[1], nullptr, 0);
    DWORD setVal = 0;
    bool doSet = argc > 2;
    if (doSet) setVal = wcstoul(argv[2], nullptr, 0);
    std::vector<HMONITOR> hs;
    EnumDisplayMonitors(nullptr, nullptr, MonEnum, (LPARAM)&hs);
    wprintf(L"VCP 0x%02X\n", code);
    for (HMONITOR h : hs) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(h, (MONITORINFO*)&mi)) continue;
        DWORD cnt = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(h, &cnt) || cnt == 0) continue;
        std::vector<PHYSICAL_MONITOR> v(cnt);
        if (!GetPhysicalMonitorsFromHMONITOR(h, cnt, v.data())) continue;
        for (DWORD i = 0; i < cnt; ++i) {
            MC_VCP_CODE_TYPE type{};
            DWORD cur = 0, mx = 0;
            BOOL ok = GetVCPFeatureAndVCPFeatureReply(v[i].hPhysicalMonitor, code, &type, &cur, &mx);
            if (doSet) {
                BOOL w = SetVCPFeature(v[i].hPhysicalMonitor, code, setVal);
                wprintf(L"%s [%u] write %lu -> %d, before cur=%lu\n", mi.szDevice, i, setVal, w, cur);
                Sleep(500);
                cur = 0; mx = 0;
                ok = GetVCPFeatureAndVCPFeatureReply(v[i].hPhysicalMonitor, code, &type, &cur, &mx);
                wprintf(L"%s [%u] after  ok=%d cur=%lu max=%lu\n", mi.szDevice, i, ok, cur, mx);
            } else {
                wprintf(L"%s [%u] ok=%d cur=%lu max=%lu\n", mi.szDevice, i, ok, cur, mx);
            }
        }
        DestroyPhysicalMonitors(cnt, v.data());
    }
    return 0;
}
