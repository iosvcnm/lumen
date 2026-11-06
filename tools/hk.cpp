// 开发辅助：发送任意组合，用于验证热键限制
// 用法：hk.exe shift x   /   hk.exe ctrl alt f8
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <vector>

static UINT ModVk(const wchar_t* s) {
    if (!_wcsicmp(s, L"ctrl"))  return VK_CONTROL;
    if (!_wcsicmp(s, L"alt"))   return VK_MENU;
    if (!_wcsicmp(s, L"shift")) return VK_SHIFT;
    if (!_wcsicmp(s, L"win"))   return VK_LWIN;
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 1;
    std::vector<UINT> mods, keys;
    for (int i = 1; i < argc; ++i) {
        UINT m = ModVk(argv[i]);
        if (m) { mods.push_back(m); continue; }
        if (argv[i][0] >= L'a' && argv[i][0] <= L'z' && !argv[i][1])
            keys.push_back((UINT)towupper(argv[i][0]));
        else if (!_wcsicmp(argv[i], L"f8"))  keys.push_back(VK_F8);
        else keys.push_back((UINT)_wtoi(argv[i]));
    }
    std::vector<INPUT> in;
    for (UINT m : mods) {
        INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = (WORD)m; in.push_back(i);
    }
    for (UINT k : keys) {
        INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = (WORD)k; in.push_back(i);
    }
    for (size_t n = keys.size(); n-- > 0;) {
        INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = (WORD)keys[n];
        i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i);
    }
    for (size_t n = mods.size(); n-- > 0;) {
        INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = (WORD)mods[n];
        i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i);
    }
    UINT sent = SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
    wprintf(L"sent %u of %u\n", sent, (UINT)in.size());
    return 0;
}
