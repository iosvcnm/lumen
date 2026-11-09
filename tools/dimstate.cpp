// 开发辅助：打印当前 gamma LUT 中点值与遮罩 alpha，验证 10% 两段式调光
#include <windows.h>
#include <stdio.h>
#include <vector>

static BOOL CALLBACK MonEnum(HMONITOR h, HDC, LPRECT, LPARAM p) {
    ((std::vector<HMONITOR>*)p)->push_back(h);
    return TRUE;
}

int wmain() {
    std::vector<HMONITOR> hs;
    EnumDisplayMonitors(nullptr, nullptr, MonEnum, (LPARAM)&hs);
    for (HMONITOR h : hs) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(h, (MONITORINFO*)&mi)) continue;
        HDC dc = CreateDCW(mi.szDevice, mi.szDevice, nullptr, nullptr);
        if (!dc) continue;
        WORD r[3][256];
        BOOL ok = GetDeviceGammaRamp(dc, r);
        wprintf(L"%s ramp[128] ok=%d R=%u (identity=%u)\n", mi.szDevice, ok, ok ? r[0][128] : 0, 128 * 257);
        DeleteDC(dc);
    }
    HWND w = FindWindowW(L"LumenDimWnd", nullptr);
    if (!w) {
        wprintf(L"overlay: none\n");
    } else {
        DWORD flags = 0;
        BYTE alpha = 0;
        BOOL ok = GetLayeredWindowAttributes(w, nullptr, &alpha, &flags);
        wprintf(L"overlay: hwnd=%p alpha=%lu/255 ok=%d\n", (void*)w, alpha, ok);
    }
    return 0;
}
