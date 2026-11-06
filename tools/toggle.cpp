// 开发辅助：驱动 lumen 面板与亮度调节，用于回归
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#define WM_SHOW (WM_APP + 2)

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    wchar_t cmd = argv[1][0];
    HWND msg = FindWindowW(L"LumenMsgWnd", nullptr);
    HWND panel = FindWindowW(L"LumenPanelWnd", nullptr);
    if (!msg) { wprintf(L"app not running\n"); return 1; }

    if (cmd == L's' || cmd == L'o') PostMessageW(msg, WM_SHOW, 0, 0);
    else if (cmd == L'c') { if (panel) PostMessageW(panel, WM_CLOSE, 0, 0); }
    else if (cmd == L'u') PostMessageW(msg, WM_HOTKEY, 1, 0);   // HK_UP
    else if (cmd == L'd') PostMessageW(msg, WM_HOTKEY, 2, 0);   // HK_DOWN
    return 0;
}
