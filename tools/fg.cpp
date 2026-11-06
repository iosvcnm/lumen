// 开发辅助：打印当前前台窗口
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int wmain() {
    HWND h = GetForegroundWindow();
    wchar_t cls[128]{}, title[128]{};
    if (h) { GetClassNameW(h, cls, 128); GetWindowTextW(h, title, 128); }
    wprintf(L"fg=%p class=%ls title=%ls\n", (void*)h, cls, title);
    return 0;
}
