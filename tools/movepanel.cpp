// 开发辅助：把面板移到指定位置，并模拟一次拖动结束（触发 WM_EXITSIZEMOVE）
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, wchar_t** argv) {
    HWND h = FindWindowW(L"LumenPanelWnd", nullptr);
    if (!h) { wprintf(L"no panel\n"); return 1; }
    if (argc < 3) { wprintf(L"usage: movepanel x y\n"); return 1; }
    int x = _wtoi(argv[1]), y = _wtoi(argv[2]);
    RECT r{};
    GetWindowRect(h, &r);
    int w = r.right - r.left, ht = r.bottom - r.top;
    SetWindowPos(h, nullptr, x, y, w, ht, SWP_NOZORDER);
    SendMessageW(h, WM_EXITSIZEMOVE, 0, 0);
    GetWindowRect(h, &r);
    wprintf(L"moved to %ld,%ld (%dx%d)\n", r.left, r.top, w, ht);
    return 0;
}
