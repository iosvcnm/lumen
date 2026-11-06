// 开发辅助：发送 Ctrl+Alt+F8，用于验证热键录制
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int wmain() {
    INPUT in[6]{};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = VK_MENU;
    in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = VK_F8;
    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_F8;    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    in[4].type = INPUT_KEYBOARD; in[4].ki.wVk = VK_MENU;  in[4].ki.dwFlags = KEYEVENTF_KEYUP;
    in[5].type = INPUT_KEYBOARD; in[5].ki.wVk = VK_CONTROL; in[5].ki.dwFlags = KEYEVENTF_KEYUP;
    UINT n = SendInput(6, in, sizeof(INPUT));
    wprintf(L"sent %u\n", n);
    return 0;
}
