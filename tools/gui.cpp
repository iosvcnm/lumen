// 开发辅助：按 PID 读取 GDI / USER 对象数，定位句柄增长来源
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, wchar_t** argv) {
    DWORD pid = (argc > 1) ? (DWORD)_wtoi(argv[1]) : 0;
    if (!pid) {
        HWND h = FindWindowW(L"LumenPanelWnd", nullptr);
        DWORD p = 0;
        if (h) GetWindowThreadProcessId(h, &p);
        pid = p;
    }
    HANDLE ph = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!ph) ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!ph) { wprintf(L"open fail %lu\n", GetLastError()); return 1; }
    DWORD g = GetGuiResources(ph, GR_GDIOBJECTS);
    DWORD u = GetGuiResources(ph, GR_USEROBJECTS);
    wprintf(L"pid=%lu  GDI=%lu  USER=%lu\n", pid, g, u);
    CloseHandle(ph);
    return 0;
}
