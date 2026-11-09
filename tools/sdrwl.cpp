// 开发辅助：读取 / 写入 Windows SDR White Level（HDR 下的 SDR 内容亮度）
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#ifndef DISPLAYCONFIG_DEVICE_INFO_SET_SDR_WHITE_LEVEL
#define DISPLAYCONFIG_DEVICE_INFO_SET_SDR_WHITE_LEVEL 12
#endif

struct WlInfo {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 SDRWhiteLevel;
};

int wmain(int argc, wchar_t** argv) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) {
        wprintf(L"buffer sizes failed\n");
        return 1;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) {
        wprintf(L"query failed\n");
        return 1;
    }
    UINT32 setVal = (argc > 1) ? (UINT32)_wtoi(argv[1]) : 0;
    UINT32 setType = (argc > 2) ? (UINT32)_wtoi(argv[2]) : 12;
    for (UINT32 i = 0; i < np; ++i) {
        WlInfo g{};
        g.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        g.header.size = sizeof(g);
        g.header.adapterId = paths[i].targetInfo.adapterId;
        g.header.id = paths[i].targetInfo.id;
        LONG r = DisplayConfigGetDeviceInfo(&g.header);
        wprintf(L"path %u  get=%ld  wl=%u\n", i, r, g.SDRWhiteLevel);
        if (setVal) {
            WlInfo s{};
            s.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)setType;
            s.header.size = sizeof(s);
            s.header.adapterId = paths[i].targetInfo.adapterId;
            s.header.id = paths[i].targetInfo.id;
            s.SDRWhiteLevel = setVal;
            LONG rr = DisplayConfigSetDeviceInfo(&s.header);
            wprintf(L"  set %u -> %ld\n", setVal, rr);
            Sleep(300);
            g.SDRWhiteLevel = 0;
            DisplayConfigGetDeviceInfo(&g.header);
            wprintf(L"  readback wl=%u\n", g.SDRWhiteLevel);
        }
    }
    return 0;
}
