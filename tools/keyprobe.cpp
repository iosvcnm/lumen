// 开发辅助：验证键盘注入与 WH_KEYBOARD_LL 钩子链路是否正常
// 安装 LL 键盘钩子 → SendInput 发 Alt+X / Ctrl+Y → 打印钩子收到的事件
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static HHOOK g_hook;
static wchar_t g_log[4096];
static int g_logLen = 0;

static LRESULT CALLBACK LowKey(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        g_logLen += swprintf(g_log + g_logLen, 512 - g_logLen,
                             L"wp=%u vk=0x%02X inj=%d\n",
                             (unsigned)wp, (unsigned)k->vkCode, (k->flags & LLKHF_INJECTED) ? 1 : 0);
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static void Pump(int ms) {
    MSG m;
    DWORD deadline = GetTickCount() + ms;
    while (GetTickCount() < deadline) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
    }
}

static void Send(WORD vk, bool up) {
    INPUT i{};
    i.type = INPUT_KEYBOARD;
    i.ki.wVk = vk;
    if (up) i.ki.dwFlags = KEYEVENTF_KEYUP;
    UINT n = SendInput(1, &i, sizeof(INPUT));
    wprintf(L"SendInput vk=0x%02X up=%d -> %u\n", vk, up ? 1 : 0, n);
}

// 组合键：按住 mod → 按/放 vk → 放 mod（模拟真人）；同时用自身 LL 钩子确认事件确实发生
static void Combo(WORD mod, WORD vk) {
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowKey, GetModuleHandleW(nullptr), 0);
    if (!g_hook) wprintf(L"(probe hook install failed)\n");
    INPUT i[4]{};
    for (auto& x : i) x.type = INPUT_KEYBOARD;
    i[0].ki.wVk = mod;
    i[1].ki.wVk = vk;
    i[2].ki.wVk = vk;    i[2].ki.dwFlags = KEYEVENTF_KEYUP;
    i[3].ki.wVk = mod;   i[3].ki.dwFlags = KEYEVENTF_KEYUP;
    UINT n = SendInput(4, i, sizeof(INPUT));
    wprintf(L"combo mod=0x%02X vk=0x%02X -> SendInput returned %u\n", mod, vk, n);
    Pump(300);
    wprintf(L"probe hook saw %d chars:\n%ls", g_logLen, g_log);
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr; g_logLen = 0; g_log[0] = 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && !wcscmp(argv[1], L"combo")) {
        Combo((WORD)wcstoul(argv[2], nullptr, 0), (WORD)wcstoul(argv[3], nullptr, 0));
        Pump(300);
        return 0;
    }
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowKey, GetModuleHandleW(nullptr), 0);
    if (!g_hook) { wprintf(L"hook install failed\n"); return 1; }
    wprintf(L"hook installed\n");

    Send(VK_MENU, false);  Pump(80);   // Alt down
    Send(0x58, false);     Pump(80);   // X down
    Send(0x58, true);      Pump(80);
    Send(VK_MENU, true);   Pump(150);
    Send(VK_CONTROL, false); Pump(80); // Ctrl down
    Send(0x59, false);     Pump(80);   // Y down
    Send(0x59, true);      Pump(80);
    Send(VK_CONTROL, true); Pump(150);

    wprintf(L"--- hook saw ---\n%ls", g_log);
    UnhookWindowsHookEx(g_hook);
    return 0;
}