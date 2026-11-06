// 开发辅助：真实鼠标点击指定屏幕坐标
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    int x = _wtoi(argv[1]), y = _wtoi(argv[2]);
    SetCursorPos(x, y);
    Sleep(120);
    INPUT in[2]{};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    UINT n = SendInput(2, in, sizeof(INPUT));
    wprintf(L"clicked %d,%d sent=%u\n", x, y, n);
    return 0;
}
