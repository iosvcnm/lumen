// 开发辅助：向面板发一次滚轮，验证设置页是否隔离调节
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int wmain(int argc, wchar_t** argv) {
    HWND h = FindWindowW(L"LumenPanelWnd", nullptr);
    if (!h) { wprintf(L"no panel\n"); return 1; }
    int delta = (argc > 1 && argv[1][0] == L'-') ? -WHEEL_DELTA : WHEEL_DELTA;
    PostMessageW(h, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)delta), 0);
    wprintf(L"wheel sent %d\n", delta);
    return 0;
}
