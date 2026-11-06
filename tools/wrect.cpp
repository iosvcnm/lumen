// 开发辅助：打印 lumen 各窗口矩形
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static void Show(const wchar_t* cls) {
    HWND h = FindWindowW(cls, nullptr);
    if (!h) { wprintf(L"%ls: none\n", cls); return; }
    RECT r{};
    GetWindowRect(h, &r);
    wprintf(L"%ls: %ld,%ld - %ld,%ld  visible=%d\n", cls,
            r.left, r.top, r.right, r.bottom, IsWindowVisible(h) ? 1 : 0);
}

int wmain() {
    Show(L"LumenPanelWnd");
    Show(L"LumenSetWnd");
    Show(L"LumenOsdWnd");
    return 0;
}
