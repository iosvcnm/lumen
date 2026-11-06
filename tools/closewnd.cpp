// 开发辅助：关闭 lumen 面板窗口，用于验证句柄回收
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int wmain() {
    HWND h = FindWindowW(L"LumenPanelWnd", nullptr);
    if (!h) { wprintf(L"panel not found\n"); return 1; }
    PostMessageW(h, WM_CLOSE, 0, 0);
    wprintf(L"closed %p\n", (void*)h);
    return 0;
}
