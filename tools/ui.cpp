// 开发辅助：驱动 lumen 交互，用于回归
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

static void Click(HWND h, int x, int y) {
    LPARAM lp = MAKELPARAM(x, y);
    PostMessageW(h, WM_MOUSEMOVE, 0, lp);
    PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    PostMessageW(h, WM_LBUTTONUP, 0, lp);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    const wchar_t* cmd = argv[1];
    HWND panel = FindWindowW(L"LumenPanelWnd", nullptr);
    HWND msg = FindWindowW(L"LumenMsgWnd", nullptr);

    if (!_wcsicmp(cmd, L"click")) {
        if (!panel) { wprintf(L"no panel\n"); return 1; }
        int x = _wtoi(argv[2]), y = _wtoi(argv[3]);
        Click(panel, x, y);
        wprintf(L"clicked %d,%d\n", x, y);
    } else if (!_wcsicmp(cmd, L"blur")) {
        HWND desk = FindWindowW(L"Progman", nullptr);
        if (desk) SetForegroundWindow(desk);
        wprintf(L"blur requested\n");
    } else if (!_wcsicmp(cmd, L"exists")) {
        wprintf(panel ? L"panel=1\n" : L"panel=0\n");
    } else if (!_wcsicmp(cmd, L"osd")) {
        if (!msg) { wprintf(L"no app\n"); return 1; }
        WPARAM id = (argc > 2 && argv[2][0] == L'd') ? 9002 : 9001;
        PostMessageW(msg, WM_HOTKEY, id, 0);
        wprintf(L"hotkey %llu\n", (unsigned long long)id);
    } else if (!_wcsicmp(cmd, L"esc")) {
        HWND menu = FindWindowW(L"#32768", nullptr);
        if (menu) {
            PostMessageW(menu, WM_KEYDOWN, VK_ESCAPE, 0);
            wprintf(L"esc to menu\n");
        } else {
            wprintf(L"no menu\n");
            return 1;
        }
    } else if (!_wcsicmp(cmd, L"settings")) {
        if (!panel) { wprintf(L"no panel\n"); return 1; }
        Click(panel, 272, 522);
        wprintf(L"settings clicked\n");
    } else if (!_wcsicmp(cmd, L"drag")) {
        if (!panel) { wprintf(L"no panel\n"); return 1; }
        int x1 = _wtoi(argv[2]), y1 = _wtoi(argv[3]);
        int x2 = _wtoi(argv[4]), y2 = _wtoi(argv[5]);
        int steps = (argc > 6) ? _wtoi(argv[6]) : 8;
        if (steps < 1) steps = 1;
        PostMessageW(panel, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x1, y1));
        for (int i = 1; i <= steps; ++i) {
            int x = x1 + (x2 - x1) * i / steps;
            int y = y1 + (y2 - y1) * i / steps;
            PostMessageW(panel, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
        }
        PostMessageW(panel, WM_LBUTTONUP, 0, MAKELPARAM(x2, y2));
        wprintf(L"dragged steps=%d\n", steps);
    } else if (!_wcsicmp(cmd, L"setclick")) {
        HWND set = FindWindowW(L"LumenSetWnd", nullptr);
        if (!set) { wprintf(L"no settings\n"); return 1; }
        int x = _wtoi(argv[2]), y = _wtoi(argv[3]);
        Click(set, x, y);
        wprintf(L"set clicked %d,%d\n", x, y);
    } else if (!_wcsicmp(cmd, L"setex")) {
        HWND set = FindWindowW(L"LumenSetWnd", nullptr);
        wprintf(set ? L"set=1\n" : L"set=0\n");
    }
    return 0;
}
