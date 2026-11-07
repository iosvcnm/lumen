// lumen — 原生 Windows 亮度控制
// Win32 + GDI+，无第三方运行时。启动只挂托盘；面板按需创建，关闭即销毁。
// 亮度走 DDC/CI（dxva2.dll），每次取句柄用完立即 DestroyPhysicalMonitors。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <highlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <wbemidl.h>
#include <winhttp.h>
#include <ws2tcpip.h>

#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <atomic>
#include <algorithm>
#include <map>

using namespace Gdiplus;

static const double PI = 3.14159265358979323846;
static const wchar_t* kMsgClass   = L"LumenMsgWnd";
static const wchar_t* kBcastClass = L"LumenBcastWnd";   // 顶层隐藏：接收系统广播转发给 g_msg
static const wchar_t* kPanelClass = L"LumenPanelWnd";
static const wchar_t* kOsdClass   = L"LumenOsdWnd";
static const wchar_t* kCfgKey     = L"Software\\lumen";

#define WM_TRAY   (WM_APP + 1)
#define WM_SHOW   (WM_APP + 2)
#define WM_AUTOHIDE (WM_APP + 4)
#define WM_NAMES   (WM_APP + 6)
#define WM_UPDATE  (WM_APP + 7)   // wParam = new tag string (heap, receiver frees)

#define APP_VERSION L"1.2.1"

#define HK_UP     9001
#define HK_DOWN   9002
#define HK_TOGGLE 9005
#define ID_EXIT   4

// ---------------------------------------------------------------- 布局（96 DPI 逻辑坐标）

static const int   PANEL_W = 400;
static const int   PANEL_H = 570;
static const float KNOB_CX = 200.f;
static const float KNOB_CY = 246.f;
static const float KNOB_R  = 110.f;
static const float DOT_R   = 174.f;
static const float TRACK_R = 146.f;
static const float PCT_Y   = 384.f;
static const float PILL_Y  = 444.f;
static const float PILL_W  = 196.f;
static const float PILL_H  = 38.f;
static const float HINT_Y  = 488.f;
static const float BTN_CY  = 528.f;
static const float BTN_R   = 28.f;
static const float BTN_DX  = 74.f;

// ---------------------------------------------------------------- 状态

static HINSTANCE g_inst = nullptr;
static HWND      g_msg = nullptr;
static HWND      g_hwnd = nullptr;
static HWND      g_osd = nullptr;
static bool      g_tray = false;
static NOTIFYICONDATAW g_nid{};
static int       g_dpi = 96;
static DWORD     g_panelBorn = 0;
static bool      g_dragging = false;
static bool      g_menuOpen = false;
static int       g_menuHover = -1;
static HANDLE    g_icon = nullptr;

struct Mon {
    std::wstring dev;
    std::wstring key;      // 稳定标识：显示器设备接口路径，跨重启不变
    std::wstring name;
    bool controllable = false;
    bool primary = false;
    bool wmi = false;
    bool hdr = false;      // Windows 高级色 HDR 激活（OSD 硬件 HDR 检测不到）
    int  pct = 50;
    DWORD mn = 0, mx = 100;
};

static std::vector<Mon> g_mons;
static int  g_sel = -1;
static std::wstring g_cfgSel;   // 上次选中的显示器键，空 = 全部 / ALL
static int  g_val = 50;
static bool g_dark = false;
static std::atomic<bool> g_fail{false};

static int  g_cfgAuto = 0, g_cfgDelay = 25, g_cfgTheme = 0, g_cfgStart = 0;
static int  g_cfgStartH = 7, g_cfgEndH = 19;
static int  g_page = 0;   // 0 主面板 1 设置页
static bool g_wantSettings = false;   // 首次建窗时直接进设置页
static UINT g_hkUpMod = MOD_CONTROL, g_hkUpVk = VK_PRIOR;
static UINT g_hkDownMod = MOD_CONTROL, g_hkDownVk = VK_NEXT;
static UINT g_hkShowMod = MOD_CONTROL | MOD_ALT, g_hkShowVk = 'B';
static int  g_capSlot = 0, g_hkMsg = 0;  // g_hkMsg: 0 无 / 1 被占用 / 2 需含 Ctrl 或 Alt
static int  g_hover = -1;          // 0 胶囊，1..3 快捷按钮

struct Pal {
    Color bg1, bg2, ring, dotOn, dotOff, knob1, knob2, knobEdge,
          icon, text, sub, hint, pill, pillEdge, btn1, btn2, shadow,
          track, trackShade, prog, progDot,
          pillHover, menuBg, menuHover, danger, osdBg, osdText, osdSub, border,
          raisedHi, raisedSh, disc, discSh, iconHi;   // 拟物：左上高光 / 右下落影 / 凹盘 / 浮雕高光
};
static Pal g_pal;

static int Scale(int v) { return MulDiv(v, g_dpi, 96); }

// 面板整体缩到 86%，逻辑坐标仍按 400x570 绘制
static const float UI_SCALE = 0.86f;

// ---------------------------------------------------------------- 多语言（0 跟随系统 / 1 中文 / 2 English）
static int g_lang = 0;
static int g_step = 5;       // 滚轮/热键步长，可调 1/2/5/10
static int g_gammaDim = 0;   // 软件调光：HDR 固件锁 DDC 亮度时的 gamma 回退通路
enum {
    S_BRIGHTNESS, S_THEME, S_AUTO, S_LIGHT, S_DARK,
    S_ALLMON, S_NOCTRL, S_ALLMONFMT, S_BUILTIN, S_PRIMARY, S_EXTERNAL, S_NOADJ,
    S_BUILTINSCR, S_GENERICFMT,
    S_HINT, S_FAIL, S_BACK, S_SETTINGS, S_AUTOSTART, S_AUTOBR, S_STARTH, S_ENDH, S_DDC, S_STEP,
    S_HOTKEYS, S_UP, S_DOWN, S_SHOWPILL, S_PRESSKEY, S_OCCUPIED, S_NEEDMOD, S_MODIFY, S_CANCEL, S_SIG,
    S_SHOWPANEL, S_BRIGHTUP, S_BRIGHTDN, S_DIAG, S_EXIT, S_LANG, S_GAMMA,
    S_OCCUPFMT, S_UPDATEFMT, S_HDR,
    S_COUNT
};
static const wchar_t* const STR_CN[S_COUNT] = {
    L"亮度", L"主题", L"自动", L"浅色", L"深色",
    L"全部显示器", L"未检测到可控显示器", L"全部显示器 (%d 台)", L" (内置)", L" (主显示器)", L" (副屏)", L" · 不可调",
    L"内置显示屏", L"通用外接显示器 %d",
    L"点击切换显示器", L"写入失败：检查 DDC/CI", L"←  返回", L"设置", L"开机自启", L"按时间自动调节",
    L"开始时间", L"结束时间", L"DDC 间隔", L"步长",
    L"全局快捷键", L"上调", L"下调", L"呼出面板", L"按下 Ctrl 或 Alt + 键…", L"该组合已被其他程序占用，保留原快捷键",
    L"需包含 Ctrl 或 Alt，例如 Ctrl+Alt+↑", L"修改", L"取消",
    L"lumen · DDC/CI + WMI 混合控制",
    L"显示面板", L"亮度 +10", L"亮度 -10", L"复制诊断信息", L"退出 lumen", L"语言",
    L"软件调光（HDR 显示器用）",
    L"%ls %ls：组合被占用", L"lumen 有新版本 %ls\nhttps://github.com/iosvcnm/lumen/releases/latest",
    L" · HDR",
};
static const wchar_t* const STR_EN[S_COUNT] = {
    L"Brightness", L"Theme", L"Auto", L"Light", L"Dark",
    L"All monitors", L"No controllable monitor", L"All monitors (%d)", L" (built-in)", L" (primary)", L" (secondary)", L" · fixed",
    L"Builtin display", L"Generic monitor %d",
    L"Click to switch monitor", L"Write failed: check DDC/CI", L"←  Back", L"Settings", L"Start with Windows", L"Time-based auto",
    L"Start time", L"End time", L"DDC delay", L"Step",
    L"Global hotkeys", L"Up", L"Down", L"Show", L"Press Ctrl or Alt + key…", L"Combo occupied by another app, keeping the old one",
    L"Needs Ctrl or Alt, e.g. Ctrl+Alt+Up", L"Edit", L"Cancel",
    L"lumen · DDC/CI + WMI hybrid control",
    L"Show panel", L"Brightness +10", L"Brightness -10", L"Copy diagnostics", L"Exit lumen", L"Language",
    L"Software dimming (for HDR monitors)",
    L"%ls %ls: combo occupied", L"lumen %ls is available\nhttps://github.com/iosvcnm/lumen/releases/latest",
    L" · HDR",
};
static const wchar_t* tr(int id) { return (g_lang == 2) ? STR_EN[id] : STR_CN[id]; }
static int RegGet(const wchar_t* name, int def);   // 定义在配置区，ApplyLang 先用
static void ApplyLang() {
    int v = RegGet(L"Lang", 0);
    if (v == 0) {
        WORD u = GetUserDefaultUILanguage();
        g_lang = (u >= 0x0800 && (u & 0xFF00) == 0x0800 && PRIMARYLANGID(u) == LANG_CHINESE) ? 1 : 2;
    } else {
        g_lang = v;   // 1 中文 / 2 English
    }
}

static void AddTray();

// ---------------------------------------------------------------- 配置

static int RegGet(const wchar_t* name, int def) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kCfgKey, 0, nullptr, 0, KEY_READ, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return def;
    DWORD v = def, sz = sizeof(v), t = 0;
    RegQueryValueExW(k, name, nullptr, &t, (BYTE*)&v, &sz);
    RegCloseKey(k);
    return (int)v;
}

static void RegPut(const wchar_t* name, int val) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kCfgKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    DWORD v = (DWORD)val;
    RegSetValueExW(k, name, 0, REG_DWORD, (BYTE*)&v, sizeof(v));
    RegCloseKey(k);
}

static std::wstring RegGetStr(const wchar_t* name, const wchar_t* def) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kCfgKey, 0, nullptr, 0, KEY_READ, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return def;
    wchar_t buf[512]{};
    DWORD sz = sizeof(buf) - sizeof(wchar_t), t = 0;
    std::wstring out = def;
    if (RegQueryValueExW(k, name, nullptr, &t, (BYTE*)buf, &sz) == ERROR_SUCCESS && t == REG_SZ)
        out = buf;
    RegCloseKey(k);
    return out;
}

static void RegPutStr(const wchar_t* name, const std::wstring& val) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kCfgKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(),
                   (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

static void LoadConfig() {
    g_cfgAuto  = RegGet(L"AutoBrightness", 0);
    g_cfgDelay = std::max(10, std::min(200, RegGet(L"DdcDelay", 25)));
    g_cfgTheme = RegGet(L"ThemeMode", 0);
    g_cfgStart = RegGet(L"Autostart", 0);
    g_cfgStartH = std::max(0, std::min(23, RegGet(L"AutoStartH", 7)));
    g_cfgEndH   = std::max(0, std::min(23, RegGet(L"AutoEndH", 19)));
    if (g_cfgEndH <= g_cfgStartH) g_cfgEndH = std::min(23, g_cfgStartH + 1);
    g_hkUpMod  = (UINT)RegGet(L"HotUpMod", MOD_CONTROL);
    g_hkUpVk   = (UINT)RegGet(L"HotUpVk", VK_PRIOR);
    g_hkDownMod = (UINT)RegGet(L"HotDownMod", MOD_CONTROL);
    g_hkDownVk  = (UINT)RegGet(L"HotDownVk", VK_NEXT);
    g_hkShowMod = (UINT)RegGet(L"HotShowMod", MOD_CONTROL | MOD_ALT);
    g_hkShowVk  = (UINT)RegGet(L"HotShowVk", 'B');
    g_cfgSel    = RegGetStr(L"SelMonitor", L"");
    g_gammaDim = RegGet(L"GammaDim", 0);
    int st = RegGet(L"Step", 5);
    g_step = (st == 1 || st == 2 || st == 5 || st == 10) ? st : 5;
    ApplyLang();
}

static std::wstring KeyName(UINT vk) {
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG l = (LONG)(sc << 16);
    if (vk == VK_PRIOR || vk == VK_NEXT || vk == VK_UP || vk == VK_DOWN ||
        vk == VK_HOME || vk == VK_END || vk == VK_INSERT || vk == VK_DELETE)
        l |= (1 << 24);
    wchar_t buf[64]{};
    if (GetKeyNameTextW(l, buf, 64) > 0) return buf;
    swprintf(buf, 64, L"VK%u", vk);
    return buf;
}

static std::wstring ModName(UINT m) {
    std::wstring s;
    if (m & MOD_CONTROL) s += L"Ctrl+";
    if (m & MOD_ALT)     s += L"Alt+";
    if (m & MOD_SHIFT)   s += L"Shift+";
    return s;
}

static std::wstring HotkeyText(UINT mod, UINT vk) { return ModName(mod) + KeyName(vk); }

static void ApplyHotkey(int slot, UINT mod, UINT vk) {
    if (!g_msg) return;
    UINT* pMod = (slot == 1) ? &g_hkUpMod : (slot == 2) ? &g_hkDownMod : &g_hkShowMod;
    UINT* pVk  = (slot == 1) ? &g_hkUpVk  : (slot == 2) ? &g_hkDownVk  : &g_hkShowVk;
    int id = (slot == 1) ? HK_UP : (slot == 2) ? HK_DOWN : HK_TOGGLE;
    const wchar_t* base = (slot == 1) ? L"HotUp" : (slot == 2) ? L"HotDown" : L"HotShow";
    UINT oldMod = *pMod, oldVk = *pVk;

    UnregisterHotKey(g_msg, id);
    if (RegisterHotKey(g_msg, id, mod, vk)) {
        *pMod = mod; *pVk = vk;
        RegPut((std::wstring(base) + L"Mod").c_str(), (int)mod);
        RegPut((std::wstring(base) + L"Vk").c_str(), (int)vk);
        g_hkMsg = 0;
    } else {
        RegisterHotKey(g_msg, id, oldMod, oldVk);
        g_hkMsg = 1;
    }
}

static unsigned g_hkFail = 0;   // 位 1/2/4：上调/下调/呼出 注册失败（被占用）

static void RegisterHotkeys() {
    if (!RegisterHotKey(g_msg, HK_UP, g_hkUpMod, g_hkUpVk))         g_hkFail |= 1;
    if (!RegisterHotKey(g_msg, HK_DOWN, g_hkDownMod, g_hkDownVk))   g_hkFail |= 2;
    if (!RegisterHotKey(g_msg, HK_TOGGLE, g_hkShowMod, g_hkShowVk)) g_hkFail |= 4;
}

// 启动时若有热键注册失败，用托盘气泡告知（只提示一次；录制路径已有面板内红字反馈）
static void NotifyHotkeyFailures() {
    if (!g_hkFail || !g_tray) return;
    std::wstring t;
    wchar_t b[192];
    if (g_hkFail & 1) { swprintf(b, 192, tr(S_OCCUPFMT), tr(S_UP), HotkeyText(g_hkUpMod, g_hkUpVk).c_str());   t += b; t += L"\n"; }
    if (g_hkFail & 2) { swprintf(b, 192, tr(S_OCCUPFMT), tr(S_DOWN), HotkeyText(g_hkDownMod, g_hkDownVk).c_str()); t += b; t += L"\n"; }
    if (g_hkFail & 4) { swprintf(b, 192, tr(S_OCCUPFMT), tr(S_SHOWPILL), HotkeyText(g_hkShowMod, g_hkShowVk).c_str()); t += b; }
    while (!t.empty() && t.back() == L'\n') t.pop_back();
    g_nid.uFlags |= NIF_INFO;
    wcsncpy(g_nid.szInfo, t.c_str(), 255);  g_nid.szInfo[255] = 0;
    wcsncpy(g_nid.szInfoTitle, L"lumen", 63); g_nid.szInfoTitle[63] = 0;
    g_nid.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags &= ~NIF_INFO;      // 还原标志：之后的 NIM_MODIFY 不再携带气泡
    g_hkFail = 0;
}

// 热键录制：低级键盘钩子只观察不吞键，焦点不在本程序时也能捕获；
// 面板自身的 WM_KEYDOWN 作为第二条路径，钩子已处理时 g_capSlot 已清零，不会重复生效。
static HHOOK g_kbHook = nullptr;
static UINT  g_hookMod = 0;
static int   g_pendSlot = 0;
static UINT  g_pendMod = 0, g_pendVk = 0;

// 录制完立刻注册时，同一个按键可能还按着，会马上触发一次调节（弹出 OSD）。
// 延后 220 ms 注册，等按键抬起后再生效。
static void QueueHotkey(int slot, UINT mod, UINT vk) {
    g_pendSlot = slot;
    g_pendMod = mod;
    g_pendVk = vk;
    if (g_hwnd) SetTimer(g_hwnd, 10, 220, nullptr);
}

static bool HotkeyValid(UINT mod, UINT vk) {
    if (vk == VK_ESCAPE) return false;
    return (mod & (MOD_CONTROL | MOD_ALT)) != 0;
}

static void StopCapture() {
    g_capSlot = 0;
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK LowLevelKey(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_capSlot) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        UINT vk = k->vkCode;
        bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        bool isCtrl = (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL);
        bool isAlt  = (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU);
        bool isSh   = (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT);
        bool isWin  = (vk == VK_LWIN || vk == VK_RWIN);

        if (isCtrl || isAlt || isSh || isWin) {
            if (down) {
                if (isCtrl) g_hookMod |= MOD_CONTROL;
                if (isAlt)  g_hookMod |= MOD_ALT;
                if (isSh)   g_hookMod |= MOD_SHIFT;
                if (isWin)  g_hookMod |= MOD_WIN;
            } else {
                if (isCtrl) g_hookMod &= ~MOD_CONTROL;
                if (isAlt)  g_hookMod &= ~MOD_ALT;
                if (isSh)   g_hookMod &= ~MOD_SHIFT;
                if (isWin)  g_hookMod &= ~MOD_WIN;
            }
        } else if (down) {
            if (vk == VK_ESCAPE) {
                StopCapture();
            } else if (HotkeyValid(g_hookMod, vk)) {
                int slot = g_capSlot;
                UINT mod = g_hookMod;
                StopCapture();
                QueueHotkey(slot, mod, vk);
                if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
            } else if (g_hwnd) {
                g_hkMsg = 2;
                SetTimer(g_hwnd, 9, 2200, nullptr);
                InvalidateRect(g_hwnd, nullptr, FALSE);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void StartCapture(int slot) {
    g_capSlot = slot;
    g_hkMsg = 0;
    g_hookMod = 0;
    if (!g_kbHook) g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKey, g_inst, 0);
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ApplyAutostart(bool on) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(k, L"lumen", 0, REG_SZ, (BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"lumen");
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------- 主题

static void LoadTheme() {
    int mode = g_cfgTheme;
    if (mode == 0) {
        DWORD v = 1, n = sizeof(v), t = 0;
        if (RegGetValueW(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                L"AppsUseLightTheme", RRF_RT_REG_DWORD, &t, &v, &n) == ERROR_SUCCESS)
            g_dark = (v == 0);
        else
            g_dark = false;
    } else {
        g_dark = (mode == 2);
    }

    if (g_dark) {
        g_pal.bg1       = Color(255, 38, 48, 63);      // 深哑光午夜石板
        g_pal.bg2       = Color(255, 20, 27, 39);
        g_pal.ring      = Color(255, 46, 58, 76);
        g_pal.dotOn     = Color(255, 232, 192, 122);   // 琥珀刻度
        g_pal.dotOff    = Color(255, 58, 70, 90);
        g_pal.knob1     = Color(255, 58, 70, 90);      // 旋钮环受光面
        g_pal.knob2     = Color(255, 28, 37, 52);      // 背光面
        g_pal.knobEdge  = Color(255, 62, 76, 100);
        g_pal.icon      = Color(255, 240, 184, 94);    // 琥珀发光图标
        g_pal.iconHi    = Color(70, 255, 224, 150);
        g_pal.text      = Color(255, 233, 240, 249);
        g_pal.sub       = Color(255, 146, 162, 186);
        g_pal.hint      = Color(255, 92, 108, 134);
        g_pal.pill      = Color(255, 40, 50, 67);
        g_pal.pillEdge  = Color(255, 58, 70, 92);
        g_pal.btn1      = Color(255, 54, 66, 86);
        g_pal.btn2      = Color(255, 30, 39, 54);
        g_pal.shadow    = Color(120, 0, 0, 0);
        g_pal.track     = Color(255, 34, 44, 61);
        g_pal.trackShade= Color(255, 9, 13, 21);
        g_pal.prog      = Color(255, 240, 184, 94);
        g_pal.progDot   = Color(255, 255, 217, 143);
        g_pal.pillHover = Color(255, 48, 60, 79);
        g_pal.menuBg    = Color(252, 30, 42, 58);
        g_pal.menuHover = Color(255, 48, 62, 84);
        g_pal.danger    = Color(255, 226, 130, 130);
        g_pal.osdBg     = Color(240, 24, 34, 48);
        g_pal.osdText   = Color(255, 240, 245, 252);
        g_pal.osdSub    = Color(255, 168, 184, 204);
        g_pal.border    = Color(30, 255, 255, 255);
        g_pal.raisedHi  = Color(36, 255, 255, 255);
        g_pal.raisedSh  = Color(120, 0, 0, 0);
        g_pal.disc      = Color(255, 31, 41, 56);
        g_pal.discSh    = Color(255, 9, 13, 21);
    } else {
        g_pal.bg1       = Color(255, 247, 250, 253);   // 黏土白 / 温润浅灰蓝
        g_pal.bg2       = Color(255, 232, 238, 246);
        g_pal.ring      = Color(255, 246, 249, 253);
        g_pal.dotOn     = Color(255, 143, 163, 190);
        g_pal.dotOff    = Color(255, 214, 223, 235);
        g_pal.knob1     = Color(255, 253, 254, 255);   // 旋钮环受光面
        g_pal.knob2     = Color(255, 219, 228, 240);   // 背光面
        g_pal.knobEdge  = Color(255, 203, 214, 230);
        g_pal.icon      = Color(255, 146, 166, 194);   // 浮雕太阳
        g_pal.iconHi    = Color(230, 255, 255, 255);
        g_pal.text      = Color(255, 52, 68, 90);
        g_pal.sub       = Color(255, 124, 140, 164);
        g_pal.hint      = Color(255, 168, 180, 198);
        g_pal.pill      = Color(255, 246, 250, 254);
        g_pal.pillEdge  = Color(255, 216, 225, 238);
        g_pal.btn1      = Color(255, 251, 253, 255);
        g_pal.btn2      = Color(255, 231, 238, 248);
        g_pal.shadow    = Color(70, 118, 136, 162);
        g_pal.track     = Color(255, 229, 236, 245);
        g_pal.trackShade= Color(255, 203, 214, 229);
        g_pal.prog      = Color(255, 46, 74, 107);     // 深蓝进度弧
        g_pal.progDot   = Color(255, 70, 97, 138);
        g_pal.pillHover = Color(255, 236, 242, 250);
        g_pal.menuBg    = Color(253, 255, 255, 255);
        g_pal.menuHover = Color(255, 233, 241, 250);
        g_pal.danger    = Color(255, 176, 74, 74);
        g_pal.osdBg     = Color(246, 251, 253, 255);
        g_pal.osdText   = Color(255, 58, 76, 98);
        g_pal.osdSub    = Color(255, 126, 146, 170);
        g_pal.border    = Color(26, 120, 150, 185);
        g_pal.raisedHi  = Color(215, 255, 255, 255);
        g_pal.raisedSh  = Color(85, 122, 138, 164);
        g_pal.disc      = Color(255, 238, 243, 249);
        g_pal.discSh    = Color(255, 200, 211, 226);
    }
}

// ---------------------------------------------------------------- 显示器

// ---------------------------------------------------------------- WMI（笔记本内置屏）

struct WmiApi {
    IWbemServices* svc = nullptr;
    std::mutex m;

    bool EnsureLocked() {
        if (svc) return true;
        IWbemLocator* loc = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWbemLocator, (void**)&loc)))
            return false;
        BSTR ns = SysAllocString(L"ROOT\\WMI");
        HRESULT hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
        SysFreeString(ns);
        loc->Release();
        if (FAILED(hr)) { svc = nullptr; return false; }
        CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                          RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        return true;
    }

    IEnumWbemClassObject* Query(const wchar_t* wql) {
        IEnumWbemClassObject* en = nullptr;
        BSTR lang = SysAllocString(L"WQL");
        BSTR q = SysAllocString(wql);
        HRESULT hr = svc->ExecQuery(lang, q,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
        SysFreeString(lang);
        SysFreeString(q);
        return FAILED(hr) ? nullptr : en;
    }

    static std::wstring U16Prop(IWbemClassObject* o, const wchar_t* prop) {
        VARIANT v;
        VariantInit(&v);
        std::wstring out;
        if (SUCCEEDED(o->Get(prop, 0, &v, nullptr, nullptr)) && (v.vt & VT_ARRAY) && v.parray) {
            LONG lb = 0, ub = -1;
            SafeArrayGetLBound(v.parray, 1, &lb);
            SafeArrayGetUBound(v.parray, 1, &ub);
            USHORT* p = nullptr;
            if (SUCCEEDED(SafeArrayAccessData(v.parray, (void**)&p))) {
                for (LONG i = lb; i <= ub; ++i)
                    if (p[i]) out.push_back((wchar_t)p[i]);
                SafeArrayUnaccessData(v.parray);
            }
        }
        VariantClear(&v);
        return out;
    }

    std::vector<std::pair<std::wstring, std::wstring>> Names() {
        std::vector<std::pair<std::wstring, std::wstring>> out;
        std::lock_guard<std::mutex> lk(m);
        if (!EnsureLocked()) return out;

        std::vector<std::pair<std::wstring, std::wstring>> devPnp;
        DISPLAY_DEVICEW ad{};
        ad.cb = sizeof(ad);
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &ad, 0); ++i) {
            if (ad.StateFlags & DISPLAY_DEVICE_ACTIVE) {
                std::wstring dev = ad.DeviceName;
                DISPLAY_DEVICEW mon{};
                mon.cb = sizeof(mon);
                for (DWORD j = 0; EnumDisplayDevicesW(dev.c_str(), j, &mon, 0); ++j) {
                    std::wstring id = mon.DeviceID;
                    size_t p1 = id.find(L'\\');
                    if (p1 != std::wstring::npos) {
                        size_t p2 = id.find(L'\\', p1 + 1);
                        std::wstring pnp = id.substr(p1 + 1,
                            p2 == std::wstring::npos ? std::wstring::npos : p2 - p1 - 1);
                        if (!pnp.empty() && devPnp.size() < 8) devPnp.push_back({ dev, pnp });
                    }
                    memset(&mon, 0, sizeof(mon));
                    mon.cb = sizeof(mon);
                }
            }
            memset(&ad, 0, sizeof(ad));
            ad.cb = sizeof(ad);
        }

        struct Id { std::wstring inst, friendly, manuf; };
        std::vector<Id> ids;
        IEnumWbemClassObject* en =
            Query(L"SELECT InstanceName, UserFriendlyName, ManufacturerName FROM WmiMonitorID");
        if (en) {
            IWbemClassObject* o = nullptr;
            ULONG n = 0;
            while (en->Next(2000, 1, &o, &n) == S_OK && n == 1) {
                Id one;
                VARIANT v;
                VariantInit(&v);
                if (SUCCEEDED(o->Get(L"InstanceName", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR)
                    one.inst = v.bstrVal;
                VariantClear(&v);
                one.friendly = U16Prop(o, L"UserFriendlyName");
                one.manuf = U16Prop(o, L"ManufacturerName");
                ids.push_back(one);
                o->Release();
            }
            en->Release();
        }

        for (auto& dp : devPnp) {
            for (auto& it : ids) {
                if (!it.inst.empty() && it.inst.find(dp.second) != std::wstring::npos) {
                    std::wstring name = it.friendly;
                    if (!name.empty() && !it.manuf.empty() &&
                        name.find(it.manuf) == std::wstring::npos)
                        name = it.manuf + L" " + name;
                    out.push_back({ dp.first, name });
                    break;
                }
            }
        }
        return out;
    }

    int Read() {
        std::lock_guard<std::mutex> lk(m);
        if (!EnsureLocked()) return -1;
        IEnumWbemClassObject* en = Query(L"SELECT CurrentBrightness FROM WmiMonitorBrightness");
        if (!en) return -1;
        int val = -1;
        IWbemClassObject* obj = nullptr;
        ULONG n = 0;
        if (en->Next(1500, 1, &obj, &n) == S_OK && n == 1) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr)))
                val = (int)v.bVal;
            VariantClear(&v);
            obj->Release();
        }
        en->Release();
        return val;
    }

    bool Write(int pct) {
        std::lock_guard<std::mutex> lk(m);
        if (!EnsureLocked()) return false;
        IEnumWbemClassObject* en = Query(L"SELECT * FROM WmiMonitorBrightnessMethods");
        if (!en) return false;

        bool ok = false;
        IWbemClassObject* obj = nullptr;
        ULONG n = 0;
        if (en->Next(1500, 1, &obj, &n) == S_OK && n == 1) {
            VARIANT path;
            VariantInit(&path);
            if (SUCCEEDED(obj->Get(L"__PATH", 0, &path, nullptr, nullptr)) && path.vt == VT_BSTR) {
                IWbemClassObject* in = nullptr;
                BSTR mname = SysAllocString(L"WmiSetBrightness");
                if (SUCCEEDED(obj->GetMethod(mname, 0, &in, nullptr)) && in) {
                    VARIANT a;
                    VariantInit(&a);
                    a.vt = VT_UI4;
                    a.ulVal = 0;
                    in->Put(L"Timeout", 0, &a, 0);
                    VariantClear(&a);
                    a.vt = VT_UI1;
                    a.bVal = (BYTE)pct;
                    in->Put(L"Brightness", 0, &a, 0);
                    VariantClear(&a);
                    IWbemClassObject* outp = nullptr;
                    ok = SUCCEEDED(svc->ExecMethod(path.bstrVal, mname, 0, nullptr, in, &outp, nullptr));
                    if (outp) outp->Release();
                    in->Release();
                }
                SysFreeString(mname);
            }
            VariantClear(&path);
            obj->Release();
        }
        en->Release();
        return ok;
    }
};
static WmiApi g_wmi;

struct MonArr {
    std::vector<PHYSICAL_MONITOR> v;
    DWORD n = 0;
    ~MonArr() { if (n) DestroyPhysicalMonitors(n, v.data()); }
};

static BOOL CALLBACK MonEnum(HMONITOR h, HDC, LPRECT, LPARAM lp) {
    ((std::vector<HMONITOR>*)lp)->push_back(h);
    return TRUE;
}

// 显示器设备接口路径（MONITOR\<PnPId>\<实例>），比 \\.\DISPLAYn 稳定
static std::wstring MonitorKey(const wchar_t* dev) {
    DISPLAY_DEVICEW mon{};
    mon.cb = sizeof(mon);
    if (EnumDisplayDevicesW(dev, 0, &mon, 0) && mon.DeviceID[0])
        return mon.DeviceID;
    return dev;
}

static bool RefreshHdrFlags(std::vector<Mon>& mons);   // 定义在 HDR 段，Scan 末尾调用

static std::vector<Mon> Scan() {
    std::vector<HMONITOR> hs;
    EnumDisplayMonitors(nullptr, nullptr, MonEnum, (LPARAM)&hs);
    std::vector<Mon> out;
    for (HMONITOR h : hs) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(h, (MONITORINFO*)&mi)) continue;

        DWORD cnt = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(h, &cnt) || cnt == 0) continue;

        MonArr arr;
        arr.v.resize(cnt);
        arr.n = cnt;
        if (!GetPhysicalMonitorsFromHMONITOR(h, cnt, arr.v.data())) { arr.n = 0; continue; }

        for (DWORD i = 0; i < cnt; ++i) {
            DWORD mn = 0, cur = 0, mx = 0;
            bool ok = GetMonitorBrightness(arr.v[i].hPhysicalMonitor, &mn, &cur, &mx) && mx > mn;
            Mon m;
            m.dev = mi.szDevice;
            m.key = MonitorKey(mi.szDevice);
            m.name = arr.v[i].szPhysicalMonitorDescription;
            if (m.name.empty()) m.name = mi.szDevice;
            m.controllable = ok;
            m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            m.mn = mn; m.mx = mx;
            if (!ok) continue;
            m.pct = (int)((cur - mn) * 100u / (mx - mn));
            if (g_gammaDim) {
                // 软件调光模式：DDC 寄存器被 HDR 固件锁在自身 SDR 值，回读无意义——
                // 初始百分比改用 lumen 自己的记忆（Last_<key>），与 ApplyGammaDim 的写入同源
                m.pct = RegGet((L"Last_" + m.key).c_str(), m.pct);
                if (m.pct < 0 || m.pct > 100) m.pct = 50;
            }
            out.push_back(m);
        }
    }

    int wv = g_wmi.Read();
    if (wv >= 0) {
        Mon m;
        m.dev = L"WMI";
        m.key = L"WMI";
        m.name = tr(S_BUILTINSCR);
        m.controllable = true;
        m.wmi = true;
        m.pct = std::min(100, wv);
        out.push_back(m);
    }
    RefreshHdrFlags(out);
    return out;
}

// COM 直接 ExecMethod 在本机被拒（Timeout 参数不被接受），写入改走系统自带 CIM 调用
static bool WriteWmiCim(int pct) {
    wchar_t cmd[600];
    swprintf(cmd, 600,
        L"powershell -NoProfile -NonInteractive -WindowStyle Hidden -Command "
        L"\"$m=Get-CimInstance -Namespace root/WMI -ClassName WmiMonitorBrightnessMethods -ErrorAction Stop|Select-Object -First 1;"
        L"Invoke-CimMethod -InputObject $m -MethodName WmiSetBrightness -Arguments @{Timeout=[uint32]0;Brightness=[byte]%d}|Out-Null\"",
        std::max(0, std::min(100, pct)));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    else
        TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// ---- 软件调光（gamma ramp）：显示器固件在 HDR 下锁死 DDC/CI 亮度时的回退通路 ----
// 基准恒等 ramp 直接合成（Windows 默认状态），恢复=写恒等；不捕获当前值，
// 避免进程被强杀后把"已调暗的 ramp"当成基准造成永久变暗。

static HDC DcForDev(const std::wstring& dev) {
    std::vector<HMONITOR> hs;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM lp) {
        ((std::vector<HMONITOR>*)lp)->push_back(h); return TRUE;
    }, (LPARAM)&hs);
    for (HMONITOR h : hs) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(h, (MONITORINFO*)&mi)) continue;
        if (dev == mi.szDevice) return CreateDCW(mi.szDevice, mi.szDevice, nullptr, nullptr);
    }
    return nullptr;
}

static void WriteRamp(HDC dc, float scale) {
    WORD r[3][256];
    for (int i = 0; i < 256; ++i) {
        WORD v = (WORD)std::min(65535L, (long)i * 257);
        if (scale < 1.f) v = (WORD)(v * scale);
        r[0][i] = r[1][i] = r[2][i] = v;
    }
    SetDeviceGammaRamp(dc, r);
}

static bool ApplyGammaDim(const std::wstring& dev, int pct) {
    HDC dc = DcForDev(dev);
    if (!dc) return false;
    // Windows 安全限制：ramp 线性缩放低于 ~50% 会被 SetDeviceGammaRamp 拒绝（实测 0.5 过 / 0.4 拒）
    float scale = 0.5f + 0.5f * std::max(0, std::min(100, pct)) / 100.f;
    WriteRamp(dc, scale);
    DeleteDC(dc);
    return true;
}

static void ResetGammaAll() {
    std::vector<HMONITOR> hs;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM lp) {
        ((std::vector<HMONITOR>*)lp)->push_back(h); return TRUE;
    }, (LPARAM)&hs);
    for (HMONITOR h : hs) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(h, (MONITORINFO*)&mi)) continue;
        HDC dc = CreateDCW(mi.szDevice, mi.szDevice, nullptr, nullptr);
        if (!dc) continue;
        WriteRamp(dc, 1.f);      // 恒等 ramp = Windows 默认状态
        DeleteDC(dc);
    }
}

static bool WriteOne(const std::wstring& dev, int pct, unsigned delayMs) {
    if (g_gammaDim && dev != L"WMI") {          // 软件调光：gamma 缩放替代 DDC（HDR 固件锁亮度时用）
        bool ok = ApplyGammaDim(dev, pct);
        if (delayMs) Sleep(delayMs);
        return ok;
    }
    if (dev == L"WMI") {
        bool ok = WriteWmiCim(pct);
        if (delayMs) Sleep(delayMs);
        return ok;
    }
    std::vector<HMONITOR> hs;
    EnumDisplayMonitors(nullptr, nullptr, MonEnum, (LPARAM)&hs);
    for (HMONITOR h : hs) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(h, (MONITORINFO*)&mi)) continue;
        if (dev != mi.szDevice) continue;

        DWORD cnt = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(h, &cnt) || cnt == 0) continue;

        std::vector<PHYSICAL_MONITOR> v(cnt);
        if (!GetPhysicalMonitorsFromHMONITOR(h, cnt, v.data())) continue;

        bool any = false;
        for (DWORD i = 0; i < cnt; ++i) {
            DWORD mn = 0, cur = 0, mx = 0;
            if (!GetMonitorBrightness(v[i].hPhysicalMonitor, &mn, &cur, &mx) || mx <= mn) continue;
            DWORD raw = mn + (DWORD)((pct * (mx - mn) + 50) / 100);
            if (SetMonitorBrightness(v[i].hPhysicalMonitor, raw)) any = true;
        }
        DestroyPhysicalMonitors(cnt, v.data());
        if (delayMs) Sleep(delayMs);
        return any;
    }
    return false;
}

// ---------------------------------------------------------------- HDR 状态（Windows 高级色）

#ifndef DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2
#define DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2 15
#endif

struct DciAci2 {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 value;                 // advancedColorActive = bit1
    UINT32 colorEncoding;
    UINT32 bitsPerColorChannel;
    UINT32 activeColorMode;       // 0 SDR / 1 WCG / 2 HDR
};

// 刷新每台屏的 HDR 激活标志；返回是否有变化（供轮询决定是否重绘）
// 注意：高级色查询必须用路径的 targetInfo（source id 会返回 ERROR_GEN_FAILURE）
static bool RefreshHdrFlags(std::vector<Mon>& mons) {
    UINT32 nPath = 0, nMode = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPath, &nMode) != ERROR_SUCCESS)
        return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPath);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nMode);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPath, paths.data(), &nMode,
                           modes.data(), nullptr) != ERROR_SUCCESS)
        return false;
    std::map<std::wstring, bool> hdrOf;
    for (UINT32 i = 0; i < nPath; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;

        DciAci2 a{};
        a.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
        a.header.size = sizeof(a);
        a.header.adapterId = paths[i].targetInfo.adapterId;
        a.header.id = paths[i].targetInfo.id;
        bool hdr = false;
        if (DisplayConfigGetDeviceInfo(&a.header) == ERROR_SUCCESS) {
            hdr = (a.value & 2) != 0 || a.activeColorMode == 2;
        } else {
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO b{};
            b.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
            b.header.size = sizeof(b);
            b.header.adapterId = paths[i].targetInfo.adapterId;
            b.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&b.header) == ERROR_SUCCESS)
                hdr = (b.value & 2) != 0;   // advancedColorEnabled
        }
        hdrOf[src.viewGdiDeviceName] = hdr;
    }
    bool changed = false;
    for (auto& m : mons) {
        auto it = hdrOf.find(m.dev);
        bool now = (it != hdrOf.end()) && it->second;
        if (now != m.hdr) { m.hdr = now; changed = true; }
    }
    return changed;
}

// ---------------------------------------------------------------- 显示器名称（EDID）

struct NameCache {
    std::mutex m;
    std::map<std::wstring, std::wstring> names;
    std::atomic<bool> busy{false};
};
static NameCache g_nm;

static void ApplyNames(std::vector<Mon>& mons) {
    std::map<std::wstring, std::wstring> cache;
    {
        std::lock_guard<std::mutex> lk(g_nm.m);
        cache = g_nm.names;
    }
    int generic = 0;
    for (auto& m : mons) {
        if (m.wmi) { m.name = tr(S_BUILTINSCR); continue; }
        auto it = cache.find(m.dev);
        if (it != cache.end() && !it->second.empty()) {
            m.name = it->second;
        } else {
            ++generic;
            wchar_t b[64];
            swprintf(b, 64, tr(S_GENERICFMT), generic);
            m.name = b;
        }
    }
    std::map<std::wstring, int> cnt, seen;
    for (auto& m : mons) ++cnt[m.name];
    for (auto& m : mons) {
        if (cnt[m.name] > 1) {
            int k = ++seen[m.name];
            m.name += L" (" + std::to_wstring(k) + L")";
        }
    }
}

// 线程句柄要显式回收：每次 join 上一个，避免反复开面板累积内核句柄
static std::thread g_nameTh;
static bool        g_namesDone = false;

static void RefreshNamesAsync() {
    if (g_nm.busy.exchange(true)) return;
    if (g_nameTh.joinable()) g_nameTh.join();
    g_nameTh = std::thread([] {
        auto v = g_wmi.Names();
        {
            std::lock_guard<std::mutex> lk(g_nm.m);
            g_nm.names.clear();
            for (auto& p : v) if (!p.second.empty()) g_nm.names[p.first] = p.second;
        }
        g_nm.busy = false;
        PostMessageW(g_msg, WM_NAMES, 0, 0);
    });
}

// ---------------------------------------------------------------- 写入线程

struct Writer {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::wstring> devs;
    int pct = 0;
    bool has = false, quit = false;
    std::thread th;

    void Start() { th = std::thread([this] { Run(); }); }

    void Post(const std::vector<std::wstring>& d, int p) {
        { std::lock_guard<std::mutex> lk(m); devs = d; pct = p; has = true; }
        cv.notify_one();
    }

    void Stop() {
        { std::lock_guard<std::mutex> lk(m); quit = true; }
        cv.notify_one();
        if (th.joinable()) th.join();
    }

    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto last = std::chrono::steady_clock::now() - std::chrono::milliseconds(500);
        for (;;) {
            std::vector<std::wstring> d;
            int p = 0;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [this] { return has || quit; });
                if (quit) break;
                d = devs; p = pct; has = false;
            }
            int gapMs = std::max(16, g_cfgDelay);
            auto gap = std::chrono::milliseconds(gapMs) - (std::chrono::steady_clock::now() - last);
            if (gap.count() > 0) std::this_thread::sleep_for(gap);

            bool ok = true;
            for (auto& dev : d) if (!WriteOne(dev, p, 0)) ok = false;
            g_fail = !ok;
            last = std::chrono::steady_clock::now();
        }
        CoUninitialize();
    }
};
static Writer g_writer;

static void Post(int pct) {
    pct = std::max(0, std::min(100, pct));
    g_val = pct;
    std::vector<std::wstring> devs;
    if (g_sel >= 0 && g_sel < (int)g_mons.size()) devs.push_back(g_mons[g_sel].dev);
    else for (auto& m : g_mons) if (m.controllable) devs.push_back(m.dev);
    g_writer.Post(devs, pct);
    for (auto& d : devs)
        for (auto& m : g_mons)
            if (m.dev == d) RegPut((L"Last_" + m.key).c_str(), pct);   // 记住每台屏的亮度，唤醒/重启后恢复
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void Adjust(int delta) {
    if (g_mons.empty()) g_mons = Scan();
    if (g_sel >= 0 && g_sel < (int)g_mons.size()) {
        Post(g_mons[g_sel].pct + delta);
        g_mons[g_sel].pct = g_val;
        return;
    }
    int sum = 0, n = 0;
    for (auto& m : g_mons) if (m.controllable) { sum += m.pct; ++n; }
    Post((n ? sum / n : g_val) + delta);
    for (auto& m : g_mons) if (m.controllable) m.pct = g_val;
}

// 唤醒/重启后按 PnPId 恢复每台屏上次的亮度（与注册表记录不同才写）
static void RestoreBrightness() {
    for (auto& m : g_mons) {
        if (!m.controllable) continue;
        int last = RegGet((L"Last_" + m.key).c_str(), -1);
        if (last < 0 || last > 100) continue;
        if (g_gammaDim && m.dev != L"WMI") {
            // gamma 状态不跨进程存活：无论 DDC 回读值是否一致，启动都必须重放
            m.pct = last;
            g_writer.Post({ m.dev }, last);
            continue;
        }
        if (last == m.pct) continue;
        m.pct = last;
        g_writer.Post({ m.dev }, last);
    }
    int sum = 0, n = 0;
    for (auto& m : g_mons) if (m.controllable) { sum += m.pct; ++n; }
    if (n) g_val = sum / n;
}

// 一键诊断：显示器清单 + 配置 + 快捷键，复制到剪贴板（托盘菜单 / cmd 6）
static void CopyDiagnostics() {
    if (g_mons.empty()) g_mons = Scan();
    std::wstring t = std::wstring(L"lumen ") + APP_VERSION;
    wchar_t b[320];
    swprintf(b, 320, L"\r\nlang=%d theme=%d step=%d ddc=%d auto=%d(%d-%d) autostart=%d gamma=%d",
             g_lang, g_cfgTheme, g_step, g_cfgDelay, g_cfgAuto, g_cfgStartH, g_cfgEndH,
             g_cfgStart, g_gammaDim);
    t += b;
    swprintf(b, 320, L"\r\nhotkeys: up=%ls down=%ls show=%ls",
             HotkeyText(g_hkUpMod, g_hkUpVk).c_str(),
             HotkeyText(g_hkDownMod, g_hkDownVk).c_str(),
             HotkeyText(g_hkShowMod, g_hkShowVk).c_str());
    t += b;
    for (auto& m : g_mons) {
        swprintf(b, 320, L"\r\nmonitor: %ls | key=%ls | dev=%ls | ctrl=%d pct=%d",
                 m.name.c_str(), m.key.c_str(), m.dev.c_str(), m.controllable ? 1 : 0, m.pct);
        t += b;
    }
    if (!OpenClipboard(g_msg)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    if (h) {
        memcpy(GlobalLock(h), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

// ---------------------------------------------------------------- 绘制基础

static void RoundRect(GraphicsPath& p, RectF r, float rad) {
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

static void DrawSun(Graphics& g, float cx, float cy, float r, Color c) {
    SolidBrush b(c);
    g.FillEllipse(&b, cx - r * 0.22f, cy - r * 0.22f, r * 0.44f, r * 0.44f);
    Pen p(c, r * 0.10f);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    for (int i = 0; i < 8; ++i) {
        double a = i * PI / 4.0;
        g.DrawLine(&p,
            cx + (float)cos(a) * r * 0.58f, cy + (float)sin(a) * r * 0.58f,
            cx + (float)cos(a) * r * 0.95f, cy + (float)sin(a) * r * 0.95f);
    }
}

static void DrawMoon(Graphics& g, float cx, float cy, float r, Color c) {
    GraphicsPath a, b;
    a.AddEllipse(cx - r * 0.74f, cy - r * 0.74f, r * 1.48f, r * 1.48f);
    b.AddEllipse(cx - r * 0.26f, cy - r * 0.98f, r * 1.38f, r * 1.38f);
    Region reg(&a);
    reg.Exclude(&b);
    SolidBrush br(c);
    g.FillRegion(&br, &reg);

    SolidBrush st(c);
    float sx = cx + r * 0.55f, sy = cy - r * 0.52f, s = r * 0.11f;
    PointF pts[8];
    for (int i = 0; i < 8; ++i) {
        double ang = i * PI / 4.0;
        float rr = (i % 2 == 0) ? s * 2.2f : s * 0.7f;
        pts[i] = PointF(sx + (float)cos(ang) * rr, sy + (float)sin(ang) * rr);
    }
    g.FillPolygon(&st, pts, 8);
}

static void DrawDayNight(Graphics& g, float cx, float cy, float r, Color c) {
    // 左半太阳、右半月牙
    GraphicsPath sun;
    sun.AddEllipse(cx - r * 0.92f, cy - r * 0.42f, r * 0.84f, r * 0.84f);
    SolidBrush b(c);
    g.FillPath(&b, &sun);
    Pen p(c, r * 0.10f);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    for (int i = 0; i < 4; ++i) {
        double a = PI + i * PI / 6.0;
        g.DrawLine(&p,
            cx + (float)cos(a) * r * 0.62f, cy + (float)sin(a) * r * 0.62f,
            cx + (float)cos(a) * r * 0.95f, cy + (float)sin(a) * r * 0.95f);
    }
    GraphicsPath m1, m2;
    m1.AddEllipse(cx - r * 0.10f, cy - r * 0.46f, r * 0.92f, r * 0.92f);
    m2.AddEllipse(cx + r * 0.24f, cy - r * 0.62f, r * 0.84f, r * 0.84f);
    Region mr(&m1);
    mr.Exclude(&m2);
    g.FillRegion(&b, &mr);
}

static void DrawAuto(Graphics& g, float cx, float cy, float r, Color c) {
    Pen p(c, r * 0.085f);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    for (int i = 0; i < 8; ++i) {
        double a = i * PI / 4.0 + PI / 8.0;
        g.DrawLine(&p,
            cx + (float)cos(a) * r * 0.86f, cy + (float)sin(a) * r * 0.86f,
            cx + (float)cos(a) * r * 1.0f, cy + (float)sin(a) * r * 1.0f);
    }
    g.DrawEllipse(&p, cx - r * 0.62f, cy - r * 0.62f, r * 1.24f, r * 1.24f);
    Font f(L"Consolas", r * 1.3f, FontStyleBold, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush b(c);
    g.DrawString(L"A", -1, &f, RectF(cx - r, cy - r, r * 2, r * 2), &sf, &b);
}

static void DrawGear(Graphics& g, float cx, float cy, float r, Color c) {
    SolidBrush b(c);
    g.FillEllipse(&b, cx - r * 0.60f, cy - r * 0.60f, r * 1.20f, r * 1.20f);
    for (int i = 0; i < 8; ++i) {
        double a = i * PI / 4.0;
        float tx = cx + (float)cos(a) * r * 0.78f;
        float ty = cy + (float)sin(a) * r * 0.78f;
        g.FillEllipse(&b, tx - r * 0.20f, ty - r * 0.20f, r * 0.40f, r * 0.40f);
    }
    SolidBrush hole(g_dark ? g_pal.bg1 : g_pal.bg2);
    g.FillEllipse(&hole, cx - r * 0.24f, cy - r * 0.24f, r * 0.48f, r * 0.48f);
}

static void DrawTracked(Graphics& g, const wchar_t* s, Font& f, Brush& b,
                        float cx, float y, float track) {
    const int n = (int)wcslen(s);
    std::vector<float> w(n, 0.f);
    float total = track * (n - 1);
    for (int i = 0; i < n; ++i) {
        RectF box;
        g.MeasureString(&s[i], 1, &f, PointF(0, 0), &box);
        w[i] = box.Width;
        total += box.Width;
    }
    float x = cx - total / 2.f;
    for (int i = 0; i < n; ++i) {
        g.DrawString(&s[i], 1, &f, PointF(x, y), &b);
        x += w[i] + track;
    }
}

// ---------------------------------------------------------------- 主面板

static void DrawSettingsPage(Graphics& g, int W, int H);
static void DrawMonitorMenu(Graphics& g, int W, int H);

static void DrawPanel(Graphics& g, int W, int H) {
    if (g_page == 1) { DrawSettingsPage(g, W, H); return; }
    LinearGradientBrush bg(PointF(0, 0), PointF((float)W, (float)H), g_pal.bg1, g_pal.bg2);
    // 窗口 RGN 圆角与卡片抗锯齿圆角曲线不重合，先整体铺底色，避免四角露出未绘制的黑底
    g.FillRectangle(&bg, -2.f, -2.f, (float)W + 4, (float)H + 4);
    GraphicsPath card;
    RoundRect(card, RectF(0, 0, (float)W, (float)H), 34.f);
    g.FillPath(&bg, &card);
    Pen border(g_pal.border, 1.f);
    g.DrawPath(&border, &card);

    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);

    SolidBrush tb(g_pal.text);
    SolidBrush sb(g_pal.sub);

    Font brand(L"Consolas", 19, FontStyleBold, UnitPixel);
    DrawTracked(g, L"lumen", brand, tb, W / 2.f, 24.f, 5.0f);

    // —— 拟物旋钮：三层立体结构，45° 左上光源 ——
    const float PLATEAU_R = 178.f;   // L1 底座台阶
    const float DOT_RING  = 118.f;   // 刻度点环（内移至旋钮环与进度弧之间）
    const float RING_O = 98.f;       // L2 旋钮环外沿
    const float RING_I = 64.f;       // L3 中心凹盘半径

    // L1 底座台阶：凸起圆盘 + 落影 + 左上高光 / 右下投影
    {
        SolidBrush ds(g_pal.raisedSh);
        g.FillEllipse(&ds, KNOB_CX - PLATEAU_R + 3, KNOB_CY - PLATEAU_R + 5.5f,
                      PLATEAU_R * 2, PLATEAU_R * 2);
        LinearGradientBrush pl(PointF(KNOB_CX, KNOB_CY - PLATEAU_R),
                               PointF(KNOB_CX, KNOB_CY + PLATEAU_R), g_pal.ring, g_pal.bg2);
        g.FillEllipse(&pl, KNOB_CX - PLATEAU_R, KNOB_CY - PLATEAU_R, PLATEAU_R * 2, PLATEAU_R * 2);
        Pen hiP(g_pal.raisedHi, 2.6f);
        g.DrawArc(&hiP, KNOB_CX - PLATEAU_R, KNOB_CY - PLATEAU_R, PLATEAU_R * 2, PLATEAU_R * 2, 150, 115);
        Pen shP(Color(60, g_pal.raisedSh.GetR(), g_pal.raisedSh.GetG(), g_pal.raisedSh.GetB()), 2.6f);
        g.DrawArc(&shP, KNOB_CX - PLATEAU_R, KNOB_CY - PLATEAU_R, PLATEAU_R * 2, PLATEAU_R * 2, -40, 115);
    }

    // 刻度点：覆盖段着色；深色主题琥珀微光
    const int N = 36;
    for (int i = 0; i < N; ++i) {
        double t = 210.0 + 300.0 * i / (N - 1);
        double a = (t - 90.0) * PI / 180.0;
        float x = KNOB_CX + DOT_RING * (float)cos(a);
        float y = KNOB_CY + DOT_RING * (float)sin(a);
        bool on = (i * 100.0 / N) <= g_val;
        float r = on ? 3.2f : 2.3f;
        if (on && g_dark) {
            SolidBrush halo(Color(70, g_pal.dotOn.GetR(), g_pal.dotOn.GetG(), g_pal.dotOn.GetB()));
            g.FillEllipse(&halo, x - r * 2.4f, y - r * 2.4f, r * 4.8f, r * 4.8f);
        }
        SolidBrush b(on ? g_pal.dotOn : g_pal.dotOff);
        g.FillEllipse(&b, x - r, y - r, r * 2, r * 2);
    }

    // 进度弧槽（内阴影感） + 亮度进度弧（HDR 激活时切换专属高亮发光样式）
    bool hdrNow = false;
    if (g_sel >= 0 && g_sel < (int)g_mons.size()) hdrNow = g_mons[g_sel].hdr;
    else for (auto& mm : g_mons) if (mm.hdr) hdrNow = true;

    float trk = TRACK_R, tw = 13.f;
    RectF arc(KNOB_CX - trk, KNOB_CY - trk, trk * 2, trk * 2);
    Pen shade(g_pal.trackShade, tw + 3.f);
    g.DrawArc(&shade, arc.X, arc.Y, arc.Width, arc.Height, 120, 300);
    Pen groove(g_pal.track, tw);
    g.DrawArc(&groove, arc.X, arc.Y, arc.Width, arc.Height, 120, 300);
    if (g_val > 0) {
        double sweep = g_val / 100.0 * 300.0;
        if (hdrNow) {
            for (int gp = 3; gp >= 1; --gp) {
                Pen glow(Color(gp * 30, g_pal.prog.GetR(), g_pal.prog.GetG(), g_pal.prog.GetB()),
                         tw + gp * 5.f);
                g.DrawArc(&glow, arc.X, arc.Y, arc.Width, arc.Height, 120, (REAL)sweep);
            }
        }
        Pen prog(g_pal.prog, tw);
        prog.SetStartCap(LineCapRound);
        prog.SetEndCap(LineCapRound);
        g.DrawArc(&prog, arc.X, arc.Y, arc.Width, arc.Height, 120, (REAL)sweep);

        double e = (120.0 + sweep) * PI / 180.0;
        float ex = KNOB_CX + trk * (float)cos(e);
        float ey = KNOB_CY + trk * (float)sin(e);
        if (g_dark || hdrNow) {
            SolidBrush halo(Color(hdrNow ? 90 : 64,
                                  g_pal.prog.GetR(), g_pal.prog.GetG(), g_pal.prog.GetB()));
            g.FillEllipse(&halo, ex - 15, ey - 15, 30.f, 30.f);
        }
        SolidBrush dot(g_pal.progDot);
        g.FillEllipse(&dot, ex - 7, ey - 7, 14.f, 14.f);
    }

    // L2 旋钮环：跨环面线性渐变的斜切厚度 + 外沿倒角 + 内沿倒角
    {
        SolidBrush ds(g_pal.raisedSh);
        g.FillEllipse(&ds, KNOB_CX - RING_O + 2.5f, KNOB_CY - RING_O + 4.5f, RING_O * 2, RING_O * 2);
        GraphicsPath ring;
        ring.AddEllipse(KNOB_CX - RING_O, KNOB_CY - RING_O, RING_O * 2, RING_O * 2);
        ring.AddEllipse(KNOB_CX - RING_I, KNOB_CY - RING_I, RING_I * 2, RING_I * 2);
        ring.SetFillMode(FillModeAlternate);
        LinearGradientBrush rg(PointF(KNOB_CX - RING_O, KNOB_CY - RING_O),
                               PointF(KNOB_CX + RING_O, KNOB_CY + RING_O), g_pal.knob1, g_pal.knob2);
        g.FillPath(&rg, &ring);
        Pen oHi(g_pal.raisedHi, 2.4f);
        g.DrawArc(&oHi, KNOB_CX - RING_O, KNOB_CY - RING_O, RING_O * 2, RING_O * 2, 150, 115);
        Pen oSh(Color(70, g_pal.raisedSh.GetR(), g_pal.raisedSh.GetG(), g_pal.raisedSh.GetB()), 2.4f);
        g.DrawArc(&oSh, KNOB_CX - RING_O, KNOB_CY - RING_O, RING_O * 2, RING_O * 2, -40, 115);
        Pen iSh(Color(90, g_pal.discSh.GetR(), g_pal.discSh.GetG(), g_pal.discSh.GetB()), 3.f);
        g.DrawArc(&iSh, KNOB_CX - RING_I, KNOB_CY - RING_I, RING_I * 2, RING_I * 2, 150, 115);
        Pen iHi(Color(80, 255, 255, 255), 2.f);
        g.DrawArc(&iHi, KNOB_CX - RING_I, KNOB_CY - RING_I, RING_I * 2, RING_I * 2, -40, 115);
    }

    // L3 中心凹盘：内阴影沉降，中央浮雕图标（浅色浮雕太阳 / 深色琥珀弯月星辰）
    {
        SolidBrush db(g_pal.disc);
        g.FillEllipse(&db, KNOB_CX - RING_I, KNOB_CY - RING_I, RING_I * 2, RING_I * 2);
        Pen dsh(g_pal.discSh, 4.5f);
        g.DrawArc(&dsh, KNOB_CX - RING_I + 2, KNOB_CY - RING_I + 2,
                  (RING_I - 2) * 2, (RING_I - 2) * 2, 150, 115);
        Pen dhi(Color(90, 255, 255, 255), 2.f);
        g.DrawArc(&dhi, KNOB_CX - RING_I + 3, KNOB_CY - RING_I + 3,
                  (RING_I - 3) * 2, (RING_I - 3) * 2, -40, 115);
        if (g_dark) {
            SolidBrush gh(Color(40, g_pal.icon.GetR(), g_pal.icon.GetG(), g_pal.icon.GetB()));
            g.FillEllipse(&gh, KNOB_CX - 46.f, KNOB_CY - 46.f, 92.f, 92.f);
            DrawMoon(g, KNOB_CX, KNOB_CY, 30.f, g_pal.icon);
        } else {
            DrawSun(g, KNOB_CX - 1.8f, KNOB_CY - 1.8f, 30.f, g_pal.iconHi);   // 左上高光刻
            DrawSun(g, KNOB_CX + 1.8f, KNOB_CY + 1.8f, 30.f, g_pal.discSh);   // 右下暗刻
            DrawSun(g, KNOB_CX, KNOB_CY, 30.f, g_pal.icon);                   // 浮雕主体
        }
    }

    // 百分比：浅色深蓝 / 深色琥珀（与进度弧同源）
    wchar_t buf[16];
    swprintf(buf, 16, L"%d%%", g_val);
    Font fv(L"Consolas", 46, FontStyleBold, UnitPixel);
    SolidBrush pb3(g_pal.prog);
    g.DrawString(buf, -1, &fv, RectF(0, PCT_Y, (float)W, 52), &sf, &pb3);

    // 胶囊按钮：凸起拟物（落影 + 顶缘高光）
    RectF pill(W / 2.f - PILL_W / 2, PILL_Y, PILL_W, PILL_H);
    GraphicsPath pillPath;
    RoundRect(pillPath, pill, PILL_H / 2);
    GraphicsPath pillSh;
    RoundRect(pillSh, RectF(pill.X + 2, pill.Y + 3.5f, pill.Width, pill.Height), PILL_H / 2);
    SolidBrush pshb(g_pal.raisedSh);
    g.FillPath(&pshb, &pillSh);
    SolidBrush pb(g_hover == 0 ? g_pal.pillHover : g_pal.pill);
    g.FillPath(&pb, &pillPath);
    Pen pe(g_pal.pillEdge, 1.f);
    g.DrawPath(&pe, &pillPath);
    Pen phi(g_pal.raisedHi, 1.2f);
    g.DrawPath(&phi, &pillPath);

    std::wstring full = tr(S_ALLMON);
    if (g_mons.empty()) {
        full = tr(S_NOCTRL);
    } else if (g_sel >= 0 && g_sel < (int)g_mons.size()) {
        full = g_mons[g_sel].name;
    } else {
        wchar_t b[64];
        swprintf(b, 64, tr(S_ALLMONFMT), (int)g_mons.size());
        full = b;
    }
    std::wstring shown = full;
    if (shown.size() > 14) shown = shown.substr(0, 13) + L"…";
    Font fp(L"Microsoft YaHei UI", 14, FontStyleRegular, UnitPixel);
    g.DrawString(shown.c_str(), -1, &fp,
                 RectF(pill.X, pill.Y, pill.Width - 22, pill.Height), &sf, &tb);

    // 悬停显示完整名称
    if (g_hover == 0 && full.size() > 14) {
        Font ft(L"Microsoft YaHei UI", 12, FontStyleRegular, UnitPixel);
        RectF box;
        g.MeasureString(full.c_str(), -1, &ft, PointF(0, 0), &box);
        float tw = box.Width + 28, th = 30;
        float tx = (W - tw) / 2, ty = PILL_Y - th - 8;
        GraphicsPath tp;
        RoundRect(tp, RectF(tx, ty, tw, th), 10.f);
        SolidBrush tbg(g_pal.menuBg);
        g.FillPath(&tbg, &tp);
        Pen te(g_pal.pillEdge, 1.f);
        g.DrawPath(&te, &tp);
        StringFormat tf;
        tf.SetAlignment(StringAlignmentCenter);
        tf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(full.c_str(), -1, &ft, RectF(tx, ty, tw, th), &tf, &tb);
    }

    // 下拉箭头
    float ax = pill.GetRight() - 22, ay = pill.Y + PILL_H / 2;
    Pen ap(g_pal.sub, 1.8f);
    ap.SetStartCap(LineCapRound);
    ap.SetEndCap(LineCapRound);
    g.DrawLine(&ap, ax - 4, ay - 2, ax, ay + 2.5f);
    g.DrawLine(&ap, ax, ay + 2.5f, ax + 4, ay - 2);

    // HDR 徽章：胶囊右侧椭圆小牌，当前选中屏处于 Windows HDR 时出现（钛银蓝 / 琥珀辉光）
    if (hdrNow) {
        RectF chip(pill.GetRight() + 12, PILL_Y + (PILL_H - 26) / 2, 52, 26);
        GraphicsPath csh;
        RoundRect(csh, RectF(chip.X + 1.5f, chip.Y + 2.5f, chip.Width, chip.Height), 13.f);
        SolidBrush cshb(g_pal.raisedSh);
        g.FillPath(&cshb, &csh);
        GraphicsPath cp;
        RoundRect(cp, chip, 13.f);
        LinearGradientBrush clg(PointF(chip.X, chip.Y), PointF(chip.X, chip.Y + chip.Height),
                                g_pal.btn1, g_pal.btn2);
        g.FillPath(&clg, &cp);
        Pen chi(g_pal.raisedHi, 1.3f);
        g.DrawPath(&chi, &cp);
        Pen glow(hdrNow ? Color(g_dark ? 90 : 55,
                                g_pal.prog.GetR(), g_pal.prog.GetG(), g_pal.prog.GetB())
                        : Color(0, 0, 0, 0), 2.5f);
        g.DrawPath(&glow, &cp);
        Font fh2(L"Consolas", 12, FontStyleBold, UnitPixel);
        SolidBrush ctb(g_pal.prog);
        g.DrawString(L"HDR", -1, &fh2, chip, &sf, &ctb);
    }

    // 提示
    SolidBrush hb(g_pal.hint);
    Font fh(L"Microsoft YaHei UI", 10, FontStyleRegular, UnitPixel);
    g.DrawString(tr(S_HINT), -1, &fh, RectF(0, HINT_Y, (float)W, 16), &sf, &hb);

    // 三个圆形快捷按钮：凸起拟物（落影 + 双向倒角），激活项图标琥珀/深蓝
    for (int i = 0; i < 3; ++i) {
        float cx = W / 2.f + (i - 1) * BTN_DX;
        bool hot = (g_hover == i + 1);
        float rr = hot ? BTN_R + 1.5f : BTN_R;

        SolidBrush shadow(g_pal.raisedSh);
        g.FillEllipse(&shadow, cx - rr + 2, BTN_CY - rr + 3.5f, rr * 2, rr * 2);

        LinearGradientBrush lb(PointF(cx, BTN_CY - rr), PointF(cx, BTN_CY + rr),
                               g_pal.btn1, g_pal.btn2);
        g.FillEllipse(&lb, cx - rr, BTN_CY - rr, rr * 2, rr * 2);
        Pen bhi(g_pal.raisedHi, 1.8f);
        g.DrawArc(&bhi, cx - rr, BTN_CY - rr, rr * 2, rr * 2, 150, 115);
        Pen bsh(Color(70, g_pal.raisedSh.GetR(), g_pal.raisedSh.GetG(), g_pal.raisedSh.GetB()), 1.8f);
        g.DrawArc(&bsh, cx - rr, BTN_CY - rr, rr * 2, rr * 2, -40, 115);

        Color ic = (i == 1 && g_cfgAuto) ? g_pal.prog : g_pal.sub;
        if (i == 0) {
            // 按钮图标反映主题模式：自动=昼夜组合，强制时为太阳/月亮
            if (g_cfgTheme == 0) DrawDayNight(g, cx, BTN_CY, 11.f, ic);
            else if (g_dark)     DrawMoon(g, cx, BTN_CY, 11.f, ic);
            else                 DrawSun(g, cx, BTN_CY, 11.f, ic);
        }
        else if (i == 1) DrawAuto(g, cx, BTN_CY, 10.f, ic);
        else DrawGear(g, cx, BTN_CY, 10.5f, ic);
    }

    if (g_fail) {
        SolidBrush rb(g_pal.danger);
        Font fr(L"Microsoft YaHei UI", 9, FontStyleRegular, UnitPixel);
        g.DrawString(tr(S_FAIL), -1, &fr, RectF(0, (float)H - 18, (float)W, 14), &sf, &rb);
    }

    if (g_menuOpen) DrawMonitorMenu(g, W, H);
}

// ---------------------------------------------------------------- 窗口基础

static void BindRound(HWND h, int w, int hgt, int rad, float k = 1.0f) {
    int r = (int)(Scale(rad) * k);
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, hgt + 1, r, r);
    SetWindowRgn(h, rgn, TRUE);
}

static RECT WorkAreaFor(HWND) {
    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) return mi.rcWork;
    RECT r{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    return r;
}

// 任务栏自动隐藏时 rcWork 会退化成全屏，这里按任务栏真实位置再收一次
static RECT SafeWorkArea(HWND h) {
    RECT wa = WorkAreaFor(h);

    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO pm{};
    pm.cbSize = sizeof(pm);
    bool primary = GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &pm)
                   && mon == MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    if (!primary) return wa;

    APPBARDATA pos{};
    pos.cbSize = sizeof(pos);
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &pos)) {
        RECT tb = pos.rc;
        int th = (int)(tb.bottom - tb.top);
        int tw = (int)(tb.right - tb.left);
        if (pos.uEdge == ABE_BOTTOM && th > 0 && th < 200)
            wa.bottom = std::min(wa.bottom, tb.top);
        else if (pos.uEdge == ABE_TOP && th > 0 && th < 200)
            wa.top = std::max(wa.top, tb.bottom);
        else if (pos.uEdge == ABE_LEFT && tw > 0 && tw < 300)
            wa.left = std::max(wa.left, tb.right);
        else if (pos.uEdge == ABE_RIGHT && tw > 0 && tw < 300)
            wa.right = std::min(wa.right, tb.left);
    }
    return wa;
}

// 强制前台：调用方刚点击过本程序时，用 AttachThreadInput 绕过前台锁定
static void ForceForeground(HWND h) {
    if (!h) return;
    DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD me = GetCurrentThreadId();
    if (fg && fg != me) AttachThreadInput(me, fg, TRUE);
    SetForegroundWindow(h);
    SetFocus(h);
    if (fg && fg != me) AttachThreadInput(me, fg, FALSE);
}

static void PlacePanel() {
    if (!g_hwnd) return;
    RECT wa = SafeWorkArea(g_hwnd);
    int w = (int)(Scale(PANEL_W) * UI_SCALE), h = (int)(Scale(PANEL_H) * UI_SCALE), m = Scale(11);
    int x = wa.right - w - m;
    int y = wa.bottom - h - m;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    BindRound(g_hwnd, w, h, 34, UI_SCALE);
}

static void OnPanelPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    Bitmap buf(rc.right, rc.bottom, PixelFormat32bppPARGB);
    Graphics g(&buf);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    float k = g_dpi / 96.0f * UI_SCALE;
    g.ScaleTransform(k, k);
    DrawPanel(g, PANEL_W, PANEL_H);

    Graphics dst(hdc);
    dst.DrawImage(&buf, 0, 0);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- OSD

static wchar_t g_osdMode[12] = L"";   // 非空：OSD 显示主题模式而非亮度百分比

static void OnOsdPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    Bitmap buf(rc.right, rc.bottom, PixelFormat32bppPARGB);
    Graphics g(&buf);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.ScaleTransform(g_dpi / 96.0f, g_dpi / 96.0f);

    // 投影 + 描边：浅色主题下 OSD 落在亮背景上也要有边界
    // 先整幅铺 OSD 底色，避免窗口 RGN 圆角处露出未绘制的黑底
    SolidBrush osdFill(g_pal.osdBg);
    g.FillRectangle(&osdFill, -2.f, -2.f, 194.f, 66.f);
    for (int i = 3; i >= 1; --i) {
        GraphicsPath sp;
        RoundRect(sp, RectF(2.f - i, 3.f - i + 2.f, 186.f + i * 2, 56.f + i * 2), 18.f + i);
        SolidBrush sh(Color(12 + i * 6, 20, 40, 70));
        g.FillPath(&sh, &sp);
    }
    GraphicsPath p;
    RoundRect(p, RectF(0, 0, 190, 62), 18.f);
    SolidBrush bg(g_pal.osdBg);
    g.FillPath(&bg, &p);
    Pen edge(g_pal.border, 1.f);
    g.DrawPath(&edge, &p);

    Font fsub(L"Microsoft YaHei UI", 12, FontStyleRegular, UnitPixel);
    SolidBrush sb(g_pal.osdSub);
    if (g_osdMode[0]) {
        Font fm(L"Microsoft YaHei UI", 20, FontStyleBold, UnitPixel);
        SolidBrush tb(g_pal.osdText);
        StringFormat cen;
        cen.SetAlignment(StringAlignmentCenter);
        cen.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(g_osdMode, -1, &fm, RectF(0, 0, 190, 44), &cen, &tb);
        g.DrawString(tr(S_THEME), -1, &fsub, RectF(0, 40, 190, 22), &cen, &sb);
        Graphics dst(hdc);
        dst.DrawImage(&buf, 0, 0);
        EndPaint(hwnd, &ps);
        return;
    }

    wchar_t buf2[32];
    swprintf(buf2, 32, L"%d%%", g_val);
    Font f(L"Consolas", 26, FontStyleBold, UnitPixel);
    SolidBrush tb(g_pal.osdText);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentNear);
    sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(buf2, -1, &f, RectF(20, 0, 90, 62), &sf, &tb);

    g.DrawString(tr(S_BRIGHTNESS), -1, &fsub, RectF(88, 0, 80, 62), &sf, &sb);

    Graphics dst(hdc);
    dst.DrawImage(&buf, 0, 0);
    EndPaint(hwnd, &ps);
}

static void ShowOsdNow() {
    if (!g_osd) {
        g_osd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            kOsdClass, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, g_inst, nullptr);
        if (!g_osd) return;
        SetLayeredWindowAttributes(g_osd, 0, 238, LWA_ALPHA);
    }
    RECT wa = SafeWorkArea(g_msg);
    int w = Scale(190), h = Scale(62), m = Scale(11);
    int x = wa.right - w - m;
    int y = wa.top + m;
    SetWindowPos(g_osd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    BindRound(g_osd, w, h, 18);
    ShowWindow(g_osd, SW_SHOWNOACTIVATE);
    InvalidateRect(g_osd, nullptr, FALSE);
    SetTimer(g_osd, 1, 1400, nullptr);
}

static void ShowOsd() {
    g_osdMode[0] = 0;
    ShowOsdNow();
}

static void ShowOsdMode(const wchar_t* m) {
    wcsncpy(g_osdMode, m, 11);
    g_osdMode[11] = 0;
    ShowOsdNow();
}

// ---------------------------------------------------------------- 设置窗口

static const int SET_W = 360;
static const int SET_H = 420;
static int g_setHover = -1;

static void ToggleTrack(Graphics& g, float x, float y, bool on, bool dark) {
    GraphicsPath p;
    RoundRect(p, RectF(x, y, 44, 24), 12.f);
    SolidBrush bg(on ? (dark ? Color(255, 232, 192, 122) : Color(255, 120, 150, 190))
                     : (dark ? Color(255, 44, 58, 76) : Color(255, 214, 224, 238)));
    g.FillPath(&bg, &p);
    SolidBrush knob(Color(255, 255, 255, 255));
    float kx = on ? x + 23 : x + 3;
    g.FillEllipse(&knob, kx, y + 3, 18.f, 18.f);
}

static void MiniButton(Graphics& g, float x, float y, bool plus) {
    SolidBrush bb(g_pal.pill);
    g.FillEllipse(&bb, x, y, 30.f, 30.f);
    Pen pp(g_pal.sub, 1.8f);
    g.DrawLine(&pp, x + 9, y + 15, x + 21, y + 15);
    if (plus) g.DrawLine(&pp, x + 15, y + 9, x + 15, y + 21);
}

// ---- 设置页几何：绘制与命中判定共用同一组矩形，避免两处漂移
static const float SET_L = 36.f, SET_R = 364.f;
static const float SET_HK = 394.f, SET_HK_STEP = 44.f;

static float  SetRowY(int i)          { return 112.f + i * 52.f; }
static RectF  SetToggleRect(int i)    { return RectF(SET_R - 44.f, SetRowY(i) - 4.f, 44.f, 24.f); }
static RectF  SetMinusRect(int i)     { return RectF(236.f, SetRowY(i) - 6.f, 30.f, 30.f); }
static RectF  SetPlusRect(int i)      { return RectF(334.f, SetRowY(i) - 6.f, 30.f, 30.f); }
static RectF  SetValueRect(int i)     { return RectF(270.f, SetRowY(i) - 6.f, 60.f, 28.f); }
// 第 4 行左右各半：左 DDC 间隔，右 步长
static RectF  SetDdcValue()           { return RectF(110.f, SetRowY(4) - 6.f, 54.f, 28.f); }
static RectF  SetDdcMinus()           { return RectF(168.f, SetRowY(4) - 6.f, 30.f, 30.f); }
static RectF  SetDdcPlus()            { return RectF(198.f, SetRowY(4) - 6.f, 30.f, 30.f); }
static RectF  SetStepValue()          { return RectF(272.f, SetRowY(4) - 6.f, 26.f, 28.f); }
static RectF  SetStepMinus()          { return RectF(302.f, SetRowY(4) - 6.f, 30.f, 30.f); }
static RectF  SetStepPlus()           { return RectF(334.f, SetRowY(4) - 6.f, 30.f, 30.f); }
static RectF  SetEditRect(int i)      { return RectF(SET_R - 72.f, SET_HK + i * SET_HK_STEP, 72.f, 30.f); }
static RectF  SetKeyRect(int i)       { return RectF(96.f, SET_HK + i * SET_HK_STEP, 190.f, 30.f); }
static RectF  SetBackRect()           { return RectF(16.f, 8.f, 120.f, 44.f); }

static bool InRect(const RectF& r, int x, int y) {
    return x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height;
}

static void DrawSettingsPage(Graphics& g, int W, int H) {
    // 与主面板完全同源的卡片：同一渐变、同一 34 px 圆角、同一描边
    LinearGradientBrush bg(PointF(0, 0), PointF(0, (float)H), g_pal.bg1, g_pal.bg2);
    g.FillRectangle(&bg, -2.f, -2.f, (float)W + 4, (float)H + 4);   // 铺底防黑角
    GraphicsPath card;
    RoundRect(card, RectF(0, 0, (float)W, (float)H), 34.f);
    g.FillPath(&bg, &card);
    Pen border(g_pal.pillEdge, 1.f);
    g.DrawPath(&border, &card);

    SolidBrush tb(g_pal.text);
    SolidBrush sb(g_pal.sub);
    SolidBrush acc(g_dark ? g_pal.prog : g_pal.progDot);
    Font fb(L"Microsoft YaHei UI", 14, FontStyleRegular, UnitPixel);
    Font ft(L"Microsoft YaHei UI", 22, FontStyleBold, UnitPixel);
    Font fl(L"Microsoft YaHei UI", 15, FontStyleRegular, UnitPixel);
    Font fv(L"Microsoft YaHei UI", 15, FontStyleBold, UnitPixel);
    Font fs(L"Microsoft YaHei UI", 12, FontStyleRegular, UnitPixel);
    Font fsec(L"Microsoft YaHei UI", 12, FontStyleRegular, UnitPixel);

    StringFormat cen;
    cen.SetAlignment(StringAlignmentCenter);
    cen.SetLineAlignment(StringAlignmentCenter);
    StringFormat rgt;
    rgt.SetAlignment(StringAlignmentFar);
    rgt.SetLineAlignment(StringAlignmentCenter);

    g.DrawString(tr(S_BACK), -1, &fb, PointF(28, 18), &sb);
    g.DrawString(tr(S_SETTINGS), -1, &ft, RectF(0, 46, (float)W, 32), &cen, &tb);

    Pen sep(g_pal.pillEdge, 1.f);
    g.DrawLine(&sep, SET_L, 96.f, SET_R, 96.f);

    wchar_t buf[32];
    const wchar_t* label[4] = { tr(S_AUTOSTART), tr(S_AUTOBR), tr(S_STARTH), tr(S_ENDH) };
    for (int i = 0; i < 4; ++i) {
        bool muted = (i == 2 || i == 3);
        g.DrawString(label[i], -1, &fl, PointF(SET_L, SetRowY(i)), muted ? &sb : &tb);
    }

    ToggleTrack(g, SetToggleRect(0).X, SetToggleRect(0).Y, g_cfgStart != 0, g_dark);
    ToggleTrack(g, SetToggleRect(1).X, SetToggleRect(1).Y, g_cfgAuto != 0, g_dark);

    Font fv13(L"Microsoft YaHei UI", 13, FontStyleBold, UnitPixel);
    for (int i = 2; i <= 3; ++i) {
        swprintf(buf, 32, L"%02d:00", i == 2 ? g_cfgStartH : g_cfgEndH);
        RectF vr = SetValueRect(i);
        g.DrawString(buf, -1, &fv, vr, &rgt, &tb);
        RectF mr = SetMinusRect(i), pr = SetPlusRect(i);
        MiniButton(g, mr.X, mr.Y, false);
        MiniButton(g, pr.X, pr.Y, true);
    }
    // 第 4 行：左 DDC 间隔 / 右 步长
    g.DrawString(tr(S_DDC), -1, &fl, PointF(SET_L, SetRowY(4)), &tb);
    swprintf(buf, 32, L"%d ms", g_cfgDelay);
    g.DrawString(buf, -1, &fv13, SetDdcValue(), &rgt, &tb);
    RectF dm = SetDdcMinus(), dp = SetDdcPlus();
    MiniButton(g, dm.X, dm.Y, false);
    MiniButton(g, dp.X, dp.Y, true);
    g.DrawString(tr(S_STEP), -1, &fl, PointF(240.f, SetRowY(4)), &tb);
    swprintf(buf, 32, L"%d", g_step);
    g.DrawString(buf, -1, &fv13, SetStepValue(), &rgt, &tb);
    RectF sm = SetStepMinus(), sp = SetStepPlus();
    MiniButton(g, sm.X, sm.Y, false);
    MiniButton(g, sp.X, sp.Y, true);

    g.DrawLine(&sep, SET_L, 356.f, SET_R, 356.f);
    g.DrawString(tr(S_HOTKEYS), -1, &fsec, PointF(SET_L, 368), &tb);

    for (int i = 0; i < 3; ++i) {
        float y = SET_HK + i * SET_HK_STEP;
        g.DrawString(i == 0 ? tr(S_UP) : i == 1 ? tr(S_DOWN) : tr(S_SHOWPILL), -1, &fl, PointF(SET_L, y + 6), &tb);

        RectF kr = SetKeyRect(i);
        GraphicsPath kp;
        RoundRect(kp, kr, 15.f);
        SolidBrush kb(g_pal.pill);
        g.FillPath(&kb, &kp);
        Pen kpn(g_pal.pillEdge, 1.f);
        g.DrawPath(&kpn, &kp);

        UINT hm = (i == 0) ? g_hkUpMod : (i == 1) ? g_hkDownMod : g_hkShowMod;
        UINT hv = (i == 0) ? g_hkUpVk  : (i == 1) ? g_hkDownVk  : g_hkShowVk;
        std::wstring txt;
        if (g_capSlot == i + 1) txt = tr(S_PRESSKEY);
        else txt = HotkeyText(hm, hv);
        SolidBrush& tc = (g_capSlot == i + 1) ? acc : sb;
        Font& kf = (g_capSlot == i + 1) ? fs : fl;
        g.DrawString(txt.c_str(), -1, &kf, kr, &cen, &tc);

        RectF er = SetEditRect(i);
        GraphicsPath ep;
        RoundRect(ep, er, 15.f);
        SolidBrush eb(g_pal.pill);
        g.FillPath(&eb, &ep);
        Pen epn(g_pal.pillEdge, 1.f);
        g.DrawPath(&epn, &ep);
        g.DrawString(g_capSlot == i + 1 ? tr(S_CANCEL) : tr(S_MODIFY), -1, &fs, er, &cen, &tb);
    }

    SolidBrush eb(g_pal.danger);
    SolidBrush* msgBrush = &sb;
    const wchar_t* msg = tr(S_SIG);
    if (g_hkMsg == 1) {
        msg = tr(S_OCCUPIED); msgBrush = &eb;
    } else if (g_hkMsg == 2) {
        msg = tr(S_NEEDMOD); msgBrush = &eb;
    } else if (g_capSlot) {
        msg = tr(S_NEEDMOD);
    }
    g.DrawString(msg, -1, &fs, PointF(SET_L, H - 40.f), msgBrush);
}

// ---------------------------------------------------------------- 面板窗口过程

static void HidePanel() {
    if (!g_hwnd) return;
    g_menuOpen = false;
    g_menuHover = -1;
    if (g_capSlot) StopCapture();
    if (g_pendSlot) {                    // 面板关掉也要把待注册的键落下去
        KillTimer(g_hwnd, 10);
        int slot = g_pendSlot;
        g_pendSlot = 0;
        ApplyHotkey(slot, g_pendMod, g_pendVk);
    }
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
}

// 按下时的命中判定：环带 ±20 px，底部 60° 为死区
static int TrackHit(int x, int y) {
    double dx = x - KNOB_CX, dy = y - KNOB_CY;
    double d = sqrt(dx * dx + dy * dy);
    if (fabs(d - TRACK_R) > 20.0) return -1;
    double deg = atan2(dy, dx) * 180.0 / PI;
    double t = deg + 90.0;
    if (t < 0) t += 360.0;
    double rel = t - 210.0;
    if (rel < 0) rel += 360.0;
    if (rel > 300.0) return -1;
    return (int)lround(rel / 300.0 * 100.0);
}

// 拖拽：与半径解耦，只取角度；单次事件最多变化 5%
static void DragTo(int x, int y) {
    double dx = x - KNOB_CX, dy = y - KNOB_CY;
    if (dx * dx + dy * dy < 9.0) return;
    double deg = atan2(dy, dx) * 180.0 / PI;
    double t = deg + 90.0;
    if (t < 0) t += 360.0;
    double rel = t - 210.0;
    if (rel < 0) rel += 360.0;
    if (rel > 300.0) return;
    int nv = (int)lround(rel / 300.0 * 100.0);
    int delta = nv - g_val;
    delta = std::max(-5, std::min(5, delta));
    if (delta) Post(g_val + delta);
}

// ---- 显示器下拉：面板内自绘，沿用同一套调色板与圆角
static const float MENU_W = 328.f;
static const float MENU_ITEM_H = 36.f;
static const float MENU_PAD = 12.f;

static int MenuCount() { return 1 + (int)g_mons.size(); }

static RectF MenuBox(int H) {
    float h = MENU_PAD * 2 + MenuCount() * MENU_ITEM_H;
    float bottom = H - 16.f;
    float top = bottom - h;
    if (top < 250.f) top = 250.f;          // 显示器很多时不再往上顶
    return RectF((PANEL_W - MENU_W) / 2.f, top, MENU_W, bottom - top);
}

static RectF MenuItemRect(int i, int H) {
    RectF box = MenuBox(H);
    return RectF(box.X + 8.f, box.Y + MENU_PAD + i * MENU_ITEM_H,
                 box.Width - 16.f, MENU_ITEM_H);
}

// 返回 -1 表示点在菜单外，0 = 全部显示器，1..n = 第 n 台
static int MenuHit(int x, int y, int H) {
    if (!g_menuOpen) return -1;
    if (!InRect(MenuBox(H), x, y)) return -1;
    for (int i = 0; i < MenuCount(); ++i) {
        RectF r = MenuItemRect(i, H);
        if (x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height) return i;
    }
    return -1;
}

static void ApplyMonitorSel(int idx) {
    if (idx >= 0 && idx < (int)g_mons.size()) {
        g_sel = idx;
        g_val = g_mons[idx].pct;
        g_cfgSel = g_mons[idx].key;
    } else {
        g_sel = -1;
        int sum = 0, n = 0;
        for (auto& mm : g_mons) if (mm.controllable) { sum += mm.pct; ++n; }
        g_val = n ? sum / n : g_val;
        g_cfgSel = L"ALL";
    }
    RegPutStr(L"SelMonitor", g_cfgSel);
}

// 按上次的键重新对上号；找不到就退回「全部显示器」（不动已存的键，插回后还能恢复）
static void MatchSavedSelection() {
    g_sel = -1;
    if (g_cfgSel.empty() || g_cfgSel == L"ALL") return;
    for (int i = 0; i < (int)g_mons.size(); ++i) {
        if (g_mons[i].key == g_cfgSel) { g_sel = i; return; }
    }
}

static void DrawMonitorMenu(Graphics& g, int W, int H) {
    RectF box = MenuBox(H);

    // 阴影：三层低透明度向外扩散，和面板卡片同风格
    for (int i = 3; i >= 1; --i) {
        GraphicsPath sp;
        RoundRect(sp, RectF(box.X - i, box.Y - i + 3.f, box.Width + i * 2, box.Height + i * 2), 20.f + i);
        SolidBrush sh(Color(14 + i * 6, 0, 0, 0));
        g.FillPath(&sh, &sp);
    }

    GraphicsPath bp;
    RoundRect(bp, box, 22.f);
    SolidBrush bg(g_pal.menuBg);
    g.FillPath(&bg, &bp);
    Pen be(g_pal.pillEdge, 1.f);
    g.DrawPath(&be, &bp);

    SolidBrush tb(g_pal.text);
    SolidBrush sb(g_pal.sub);
    Font fi(L"Microsoft YaHei UI", 13, FontStyleRegular, UnitPixel);
    Font fb(L"Microsoft YaHei UI", 13, FontStyleBold, UnitPixel);

    StringFormat lf;
    lf.SetLineAlignment(StringAlignmentCenter);
    lf.SetFormatFlags(StringFormatFlagsNoWrap);
    lf.SetTrimming(StringTrimmingEllipsisCharacter);

    for (int i = 0; i < MenuCount(); ++i) {
        RectF r = MenuItemRect(i, H);
        bool sel = (i == 0) ? (g_sel < 0) : (g_sel == i - 1);

        if (g_menuHover == i) {
            GraphicsPath hp;
            RoundRect(hp, r, 11.f);
            SolidBrush hb(g_pal.menuHover);
            g.FillPath(&hb, &hp);
        }

        std::wstring label;
        if (i == 0) {
            wchar_t b[64];
            swprintf(b, 64, tr(S_ALLMONFMT), (int)g_mons.size());
            label = b;
        } else {
            const Mon& m = g_mons[i - 1];
            label = m.name;
            if (m.wmi)            label += tr(S_BUILTIN);
            else if (m.primary)   label += tr(S_PRIMARY);
            else                  label += tr(S_EXTERNAL);
            if (m.hdr)            label += tr(S_HDR);
            if (!m.controllable)  label += tr(S_NOADJ);
        }

        // 勾选对号
        if (sel) {
            Pen cp(g_pal.progDot, 2.f);
            cp.SetStartCap(LineCapRound);
            cp.SetEndCap(LineCapRound);
            float cx = r.X + 16, cy = r.Y + r.Height / 2;
            g.DrawLine(&cp, cx - 4, cy, cx - 1, cy + 3);
            g.DrawLine(&cp, cx - 1, cy + 3, cx + 4, cy - 4);
        }

        g.DrawString(label.c_str(), -1, sel ? &fb : &fi,
                     RectF(r.X + 32, r.Y, r.Width - 40, r.Height), &lf, sel ? &tb : &sb);
    }
}

static int HitButton(int x, int y) {
    for (int i = 0; i < 3; ++i) {
        float cx = PANEL_W / 2.f + (i - 1) * BTN_DX;
        float dx = x - cx, dy = y - BTN_CY;
        if (dx * dx + dy * dy <= (BTN_R + 4) * (BTN_R + 4)) return i + 1;
    }
    if (x >= PANEL_W / 2 - PILL_W / 2 && x <= PANEL_W / 2 + PILL_W / 2 &&
        y >= PILL_Y && y <= PILL_Y + PILL_H) return 0;
    return -1;
}

static void OnQuickAction(int idx) {
    if (idx == 1) {
        // 跟随系统时切到当前观感的反色；强制浅/深时一键回「跟随系统」
        g_cfgTheme = (g_cfgTheme == 0) ? (g_dark ? 1 : 2) : 0;
        RegPut(L"ThemeMode", g_cfgTheme);
        LoadTheme();
        AddTray();
        ShowOsdMode(g_cfgTheme == 0 ? tr(S_AUTO) : (g_dark ? tr(S_DARK) : tr(S_LIGHT)));
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
    } else if (idx == 2) {
        g_cfgAuto = g_cfgAuto ? 0 : 1;
        RegPut(L"AutoBrightness", g_cfgAuto);
        if (g_cfgAuto) Adjust(0);
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
    } else {
        g_dragging = false;
        g_menuOpen = false;
        if (GetCapture() == g_hwnd) ReleaseCapture();
        g_page = 1;
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
    }
}

static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        typedef UINT(WINAPI* PFN)(HWND);
        auto f = (PFN)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        if (f) { UINT d = f(hwnd); if (d >= 96) g_dpi = (int)d; }
        return 0;
    }
    case WM_ERASEBKGND: return 1;

    case WM_PAINT:
        OnPanelPaint(hwnd);
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !g_dragging && !g_menuOpen && !g_capSlot)
            PostMessage(hwnd, WM_AUTOHIDE, 0, 0);
        return 0;

    case WM_AUTOHIDE:
        if (GetTickCount() - g_panelBorn < 600) return 0;
        if (!g_dragging && !g_menuOpen && !g_capSlot && GetForegroundWindow() != hwnd)
            HidePanel();
        return 0;

    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        float s = g_dpi / 96.0f * UI_SCALE;
        int x = (int)(GET_X_LPARAM(lp) / s), y = (int)(GET_Y_LPARAM(lp) / s);
        if (g_page == 1) return 0;
        if (g_menuOpen) {
            int hv = MenuHit(x, y, PANEL_H);
            if (hv != g_menuHover) { g_menuHover = hv; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        }
        if (g_dragging) { DragTo(x, y); return 0; }
        int hv = HitButton(x, y);
        if (hv != g_hover) { g_hover = hv; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hover != -1) { g_hover = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;

    case WM_LBUTTONDOWN: {
        float s = g_dpi / 96.0f * UI_SCALE;
        int x = (int)(GET_X_LPARAM(lp) / s), y = (int)(GET_Y_LPARAM(lp) / s);

        if (g_menuOpen) {
            int hit = MenuHit(x, y, PANEL_H);
            g_menuOpen = false;
            g_menuHover = -1;
            if (hit >= 0) ApplyMonitorSel(hit - 1);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (g_page == 1) {
            SetFocus(hwnd);
            if (InRect(SetBackRect(), x, y)) {
                StopCapture();
                g_menuOpen = false;
                g_page = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (InRect(SetToggleRect(0), x, y)) {
                g_cfgStart = g_cfgStart ? 0 : 1;
                ApplyAutostart(g_cfgStart != 0);
                RegPut(L"Autostart", g_cfgStart);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (InRect(SetToggleRect(1), x, y)) {
                g_cfgAuto = g_cfgAuto ? 0 : 1;
                RegPut(L"AutoBrightness", g_cfgAuto);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            for (int i = 2; i <= 3; ++i) {
                bool minus = InRect(SetMinusRect(i), x, y);
                bool plus  = InRect(SetPlusRect(i), x, y);
                if (!minus && !plus) continue;
                if (i == 2) {
                    g_cfgStartH = (g_cfgStartH + (plus ? 1 : 23)) % 24;
                    if (g_cfgEndH <= g_cfgStartH) g_cfgEndH = std::min(23, g_cfgStartH + 1);
                    RegPut(L"AutoStartH", g_cfgStartH);
                    RegPut(L"AutoEndH", g_cfgEndH);
                } else {
                    g_cfgEndH = plus ? std::min(23, g_cfgEndH + 1)
                                     : std::max(g_cfgStartH + 1, g_cfgEndH - 1);
                    RegPut(L"AutoEndH", g_cfgEndH);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (InRect(SetDdcMinus(), x, y) || InRect(SetDdcPlus(), x, y)) {
                bool plus = InRect(SetDdcPlus(), x, y);
                g_cfgDelay = plus ? std::min(200, g_cfgDelay + 5)
                                  : std::max(10, g_cfgDelay - 5);
                RegPut(L"DdcDelay", g_cfgDelay);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (InRect(SetStepMinus(), x, y) || InRect(SetStepPlus(), x, y)) {
                static const int steps[4] = { 1, 2, 5, 10 };
                int i = 0;
                while (i < 3 && steps[i] != g_step) ++i;
                i = InRect(SetStepPlus(), x, y) ? std::min(3, i + 1) : std::max(0, i - 1);
                g_step = steps[i];
                RegPut(L"Step", g_step);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            for (int i = 0; i < 3; ++i) {
                if (InRect(SetEditRect(i), x, y)) {
                    if (g_capSlot == i + 1) StopCapture();
                    else StartCapture(i + 1);
                    return 0;
                }
            }
            return 0;
        }

        int hv = HitButton(x, y);
        if (hv == 0) {
            g_menuOpen = true;
            g_menuHover = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hv > 0) { OnQuickAction(hv); return 0; }
        SetFocus(hwnd);
        int tv = TrackHit(x, y);
        if (tv >= 0) {
            SetCapture(hwnd);
            g_dragging = true;
            Post(tv);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_dragging) { g_dragging = false; ReleaseCapture(); }
        return 0;

    case WM_MOUSEWHEEL:
        if (g_page == 1 || g_capSlot || g_menuOpen) return 0;
        Post(g_val + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? g_step : -g_step));
        return 0;

    case WM_KEYDOWN:
        if (g_capSlot) {
            if (wp == VK_ESCAPE) { StopCapture(); return 0; }
            if (wp == VK_CONTROL || wp == VK_MENU || wp == VK_SHIFT ||
                wp == VK_LWIN || wp == VK_RWIN) return 0;
            UINT mod = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) & 0x8000) mod |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) & 0x8000) mod |= MOD_SHIFT;
            if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mod |= MOD_WIN;
            if (!HotkeyValid(mod, (UINT)wp)) {
                g_hkMsg = 2;
                SetTimer(hwnd, 9, 2200, nullptr);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            int slot = g_capSlot;
            StopCapture();
            QueueHotkey(slot, mod, (UINT)wp);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            if (g_menuOpen) {
                g_menuOpen = false;
                g_menuHover = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (g_page == 1) {
                g_page = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                HidePanel();
            }
        }
        return 0;

    case WM_TIMER:
        if (wp == 9) { KillTimer(hwnd, 9); g_hkMsg = 0; InvalidateRect(hwnd, nullptr, FALSE); }
        if (wp == 10 && g_pendSlot) {
            KillTimer(hwnd, 10);
            int slot = g_pendSlot;
            g_pendSlot = 0;
            ApplyHotkey(slot, g_pendMod, g_pendVk);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_NCHITTEST: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        if (g_page == 1 && p.x < (int)(Scale(140) * UI_SCALE) &&
            p.y < (int)(Scale(60) * UI_SCALE)) return HTCLIENT;
        if (p.y < (int)(Scale(52) * UI_SCALE)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RECT rc; GetClientRect(hwnd, &rc);
        BindRound(hwnd, rc.right, rc.bottom, 34, UI_SCALE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DISPLAYCHANGE:
    case WM_POWERBROADCAST:
        g_namesDone = false;
        g_mons = Scan();
        ApplyNames(g_mons);
        MatchSavedSelection();
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
        return TRUE;

    case WM_CLOSE:
        HidePanel();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- 托盘 / 菜单

static HICON MakeIcon(int size) {
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));
    // 徽章风格（与 exe 图标参考图纸同源）：圆角方形 + 主题感知配色
    float s = (float)size, inset = s * 0.05f, w = s - inset * 2;
    GraphicsPath badge;
    RoundRect(badge, RectF(inset, inset, w, w), w * 0.28f);
    LinearGradientBrush bg(PointF(0, inset), PointF(0, inset + w),
        g_dark ? Color(255, 38, 51, 74) : Color(255, 252, 253, 255),
        g_dark ? Color(255, 19, 29, 44) : Color(255, 224, 231, 243));
    g.FillPath(&bg, &badge);
    Pen rim(g_dark ? Color(255, 58, 74, 102) : Color(255, 202, 212, 228), s * 0.03f);
    g.DrawPath(&rim, &badge);
    float c = size / 2.f;
    if (g_cfgTheme == 0)                       // 跟随系统：昼夜组合图标
        DrawDayNight(g, c, c, size * 0.30f, g_dark ? Color(255, 232, 192, 122)
                                                   : Color(255, 122, 150, 186));
    else if (g_dark) DrawMoon(g, c, c, size * 0.24f, Color(255, 232, 192, 122));
    else             DrawSun(g, c, c, size * 0.24f, Color(255, 122, 150, 186));

    HBITMAP hb = nullptr;
    bmp.GetHBITMAP(Color(0, 0, 0, 0), &hb);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = hb;
    ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(hb);
    DeleteObject(mask);
    return ic;
}

static void AddTray() {
    if (g_tray) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_tray = false; }
    int s = GetSystemMetrics(SM_CXSMICON);
    if (s <= 0) s = 16;
    if (g_icon) DestroyIcon((HICON)g_icon);
    g_icon = MakeIcon(s);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_msg;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)g_icon;
    wcsncpy(g_nid.szTip, L"lumen", 127);
    g_nid.szTip[127] = 0;
    g_tray = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    g_nid.uVersion = NOTIFYICON_VERSION;   // 3：lp=消息，HIWORD(wp)=滚轮增量；点击语义不变
    if (g_tray) Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void RemoveTray() {
    if (g_tray) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_tray = false; }
}

static void ShowPanel() {
    if (!g_hwnd) {
        g_mons = Scan();
        ApplyNames(g_mons);
        if (!g_namesDone) { g_namesDone = true; RefreshNamesAsync(); }
        MatchSavedSelection();
        g_page = g_wantSettings ? 1 : 0;
        g_wantSettings = false;
        if (g_sel >= 0) {
            g_val = g_mons[g_sel].pct;
        } else {
            int sum = 0, n = 0;
            for (auto& m : g_mons) if (m.controllable) { sum += m.pct; ++n; }
            g_val = n ? sum / n : 50;
        }
        g_dpi = 96;
        g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPanelClass, L"lumen",
            WS_POPUP, 0, 0, (int)(Scale(PANEL_W) * UI_SCALE), (int)(Scale(PANEL_H) * UI_SCALE),
            nullptr, nullptr, g_inst, nullptr);
        if (!g_hwnd) return;
        g_panelBorn = GetTickCount();
    }
    PlacePanel();
    ShowWindow(g_hwnd, SW_SHOW);
    ForceForeground(g_hwnd);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void TogglePanel() {
    if (g_hwnd && IsWindowVisible(g_hwnd)) HidePanel();
    else ShowPanel();
}

static void ShowMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, tr(S_SHOWPANEL));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 2, tr(S_BRIGHTUP));
    AppendMenuW(m, MF_STRING, 3, tr(S_BRIGHTDN));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU lm = CreatePopupMenu();
    int cur = RegGet(L"Lang", 0);
    AppendMenuW(lm, MF_STRING | (cur == 0 ? MF_CHECKED : 0), 10, tr(S_AUTO));
    AppendMenuW(lm, MF_STRING | (cur == 1 ? MF_CHECKED : 0), 11, L"中文");
    AppendMenuW(lm, MF_STRING | (cur == 2 ? MF_CHECKED : 0), 12, L"English");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)lm, tr(S_LANG));
    AppendMenuW(m, MF_STRING, 6, tr(S_DIAG));
    AppendMenuW(m, MF_STRING | (g_gammaDim ? MF_CHECKED : 0), 7, tr(S_GAMMA));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 5, tr(S_SETTINGS));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, tr(S_EXIT));
    SetForegroundWindow(g_msg);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_msg, nullptr);
    DestroyMenu(m);

    if (cmd == 1) ShowPanel();
    else if (cmd == 2) { Adjust(10); ShowOsd(); }
    else if (cmd == 3) { Adjust(-10); ShowOsd(); }
    else if (cmd == 6) CopyDiagnostics();
    else if (cmd == 7) {
        g_gammaDim = g_gammaDim ? 0 : 1;
        RegPut(L"GammaDim", g_gammaDim);
        if (!g_gammaDim) ResetGammaAll();        // 关闭即恢复恒等 gamma
        else Post(g_val);                        // 开启立即按当前亮度应用
    }
    else if (cmd == 10 || cmd == 11 || cmd == 12) {
        RegPut(L"Lang", cmd - 10);
        ApplyLang();
    }
    else if (cmd == 5) {
        ShowPanel();
        g_page = 1;
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
    }
    else if (cmd == ID_EXIT) PostMessageW(g_msg, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------- 检查更新（启动后台静默查一次 GitHub Releases）

// Mimosa 约束：仅 https；host 白名单；解析出的 IP 拒绝环回/私有/链路本地等保留地址
static bool HostIsPublic(const wchar_t* host) {
    ADDRINFOW* ai = nullptr;
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (GetAddrInfoW(host, L"443", &hints, &ai) != 0) return false;
    bool ok = ai != nullptr;
    for (ADDRINFOW* p = ai; p && ok; p = p->ai_next) {
        wchar_t ip[128] = L"";
        DWORD len = 128;
        if (WSAAddressToStringW(p->ai_addr, (DWORD)p->ai_addrlen, nullptr, ip, &len) != 0) continue;
        if (wcsncmp(ip, L"127.", 4) == 0 || wcsncmp(ip, L"10.", 3) == 0 ||
            wcsncmp(ip, L"192.168.", 8) == 0 || wcsncmp(ip, L"169.254.", 8) == 0 ||
            wcsncmp(ip, L"0.", 2) == 0)
            ok = false;
        if (ok && wcsncmp(ip, L"172.", 4) == 0) {
            int o2 = _wtoi(ip + 4);
            if (o2 >= 16 && o2 <= 31) ok = false;
        }
        if (ok && p->ai_family == AF_INET6) {
            if (wcsncmp(ip, L"::1", 3) == 0 || wcsncmp(ip, L"fc", 2) == 0 ||
                wcsncmp(ip, L"fd", 2) == 0 || wcsncmp(ip, L"fe80", 4) == 0)
                ok = false;
        }
    }
    FreeAddrInfoW(ai);
    return ok;
}

static bool TagNewer(const wchar_t* tag) {
    int a1 = 0, a2 = 0, a3 = 0, b1 = 0, b2 = 0, b3 = 0;
    swscanf(APP_VERSION, L"%d.%d.%d", &a1, &a2, &a3);
    if (swscanf(tag, L"v%d.%d.%d", &b1, &b2, &b3) != 3 &&
        swscanf(tag, L"%d.%d.%d", &b1, &b2, &b3) != 3) return false;
    if (b1 != a1) return b1 > a1;
    if (b2 != a2) return b2 > a2;
    return b3 > a3;
}

static DWORD WINAPI CheckUpdateTh(LPVOID) {
    Sleep(8000);
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[64] = L"", path[256] = L"";
    uc.lpszHostName = host;  uc.dwHostNameLength = 63;
    uc.lpszUrlPath = path;   uc.dwUrlPathLength = 255;
    if (!WinHttpCrackUrl(L"https://api.github.com/repos/iosvcnm/lumen/releases/latest", 0, 0, &uc))
        return 0;
    if (wcscmp(host, L"api.github.com") != 0 || !HostIsPublic(host)) return 0;

    HINTERNET ses = WinHttpOpen(L"lumen-update-check", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 5000, 5000, 5000, 5000);
    HINTERNET con = WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE) : nullptr;
    std::string body;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            for (;;) {
                DWORD rd = 0;
                char buf[4096];
                if (!WinHttpReadData(req, buf, sizeof(buf), &rd) || rd == 0) break;
                body.append(buf, rd);
                if (body.size() > 1 << 20) break;
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);

    size_t p = body.find("\"tag_name\":\"v");
    if (p == std::string::npos) p = body.find("\"tag_name\":\"");
    if (p == std::string::npos) return 0;
    p += strlen("\"tag_name\":\"");
    size_t e = body.find('"', p);
    if (e == std::string::npos || e - p > 31) return 0;
    std::string tag = body.substr(p, e - p);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, tag.c_str(), -1, nullptr, 0);
    if (wlen <= 0 || wlen > 32) return 0;
    wchar_t* wtag = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (!wtag) return 0;
    MultiByteToWideChar(CP_UTF8, 0, tag.c_str(), -1, wtag, wlen);
    if (TagNewer(wtag)) PostMessageW(g_msg, WM_UPDATE, (WPARAM)wtag, 0);
    else free(wtag);
    return 0;
}

// ---------------------------------------------------------------- 主消息窗口

// 全局前台变化：点击面板/设置窗口以外任何地方都收起（不依赖本窗口能否收到 WM_ACTIVATE）
static HWINEVENTHOOK g_winHook = nullptr;

static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (ev != EVENT_SYSTEM_FOREGROUND) return;
    if (hwnd) {
        if (hwnd == g_hwnd || hwnd == g_osd) return;
        wchar_t cls[96]{};
        GetClassNameW(hwnd, cls, 96);
        if (!wcscmp(cls, kPanelClass) || !wcscmp(cls, kOsdClass)) return;
        if (!wcscmp(cls, L"#32768")) return;                                  // 系统菜单
        if (!wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"TrayNotifyWnd") ||
            !wcscmp(cls, L"NotifyIconOverflowWindow") ||
            !wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland")) return;    // 任务栏/托盘
    }
    DWORD now = GetTickCount();
    if (g_hwnd && IsWindowVisible(g_hwnd) && now - g_panelBorn > 600 && !g_menuOpen && !g_dragging)
        HidePanel();
}

static void ApplyAutoBrightness() {
    if (!g_cfgAuto) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    double h = st.wHour + st.wMinute / 60.0;
    double s0 = g_cfgStartH, s1 = g_cfgEndH;
    if (s1 <= s0) s1 = s0 + 1;
    double t = 1.0;
    if (h >= s1) t = 0.0;
    else if (h > s0) t = 1.0 - (h - s0) / (s1 - s0);
    Post((int)lround(25 + t * 75));
}

static LRESULT CALLBACK MsgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY: {
        UINT tmsg = (UINT)LOWORD(lp);
        if (tmsg == WM_MOUSEWHEEL) {          // 托盘图标上滚轮：直接调亮度
            short d = (short)HIWORD(wp);
            if (d) { Post(g_val + (d > 0 ? g_step : -g_step)); ShowOsd(); }
            return 0;
        }
        if (tmsg == WM_LBUTTONUP) TogglePanel();
        else if (tmsg == WM_RBUTTONUP) ShowMenu();
        return 0;
    }

    case WM_HOTKEY:
        if (wp == HK_UP) { Adjust(g_step); ShowOsd(); }
        else if (wp == HK_DOWN) { Adjust(-g_step); ShowOsd(); }
        else if (wp == HK_TOGGLE) TogglePanel();
        return 0;

    case WM_SHOW:
        ShowPanel();
        return 0;

    case WM_NAMES:
        ApplyNames(g_mons);
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
        return 0;

    case WM_TIMER:
        if (wp == 2) ApplyAutoBrightness();
        if (wp == 4 && RefreshHdrFlags(g_mons) && g_hwnd)
            InvalidateRect(g_hwnd, nullptr, FALSE);   // HDR 开/关 → 徽章自动出现/消失
        return 0;

    case WM_SETTINGCHANGE:
        LoadTheme();
        AddTray();
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
        return 0;

    case WM_UPDATE: {
        wchar_t* tag = (wchar_t*)wp;
        if (g_tray && tag) {
            wchar_t info[256];
            swprintf(info, 256, tr(S_UPDATEFMT), tag);
            g_nid.uFlags |= NIF_INFO;
            wcsncpy(g_nid.szInfo, info, 255); g_nid.szInfo[255] = 0;
            wcsncpy(g_nid.szInfoTitle, L"lumen", 63); g_nid.szInfoTitle[63] = 0;
            g_nid.dwInfoFlags = NIIF_INFO;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            g_nid.uFlags &= ~NIF_INFO;
        }
        free(tag);
        return 0;
    }

    case WM_DISPLAYCHANGE:
    case WM_POWERBROADCAST:
        g_namesDone = false;
        g_mons = Scan();
        ApplyNames(g_mons);
        MatchSavedSelection();
        RestoreBrightness();
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
        return TRUE;

    case WM_CLOSE:
        if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void RegisterClasses() {
    WNDCLASSEXW c{};
    c.cbSize = sizeof(c);
    c.hInstance = g_inst;
    c.lpfnWndProc = MsgProc;
    c.lpszClassName = kMsgClass;
    RegisterClassExW(&c);

    // 广播转发窗口：message-only 收不到系统广播，用顶层隐藏窗口转发给 g_msg
    WNDCLASSW bc{};
    bc.lpfnWndProc = [](HWND h, UINT m, WPARAM wp, LPARAM lp) -> LRESULT {
        if (m == WM_DISPLAYCHANGE || m == WM_POWERBROADCAST) {
            if (g_msg) PostMessageW(g_msg, m, wp, lp);
            return 0;
        }
        return DefWindowProcW(h, m, wp, lp);
    };
    bc.hInstance = g_inst;
    bc.lpszClassName = kBcastClass;
    RegisterClassW(&bc);

    c.lpfnWndProc = PanelProc;
    c.lpszClassName = kPanelClass;
    c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&c);

    c.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        if (m == WM_PAINT) { OnOsdPaint(h); return 0; }
        if (m == WM_ERASEBKGND) return 1;
        if (m == WM_TIMER) { ShowWindow(h, SW_HIDE); KillTimer(h, 1); return 0; }
        return DefWindowProcW(h, m, w, l);
    };
    c.lpszClassName = kOsdClass;
    c.hCursor = nullptr;
    RegisterClassExW(&c);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    g_inst = hInst;

    typedef BOOL(WINAPI* PFN_CTX)(HANDLE);
    auto setctx = (PFN_CTX)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                          "SetProcessDpiAwarenessContext");
    if (setctx) setctx((HANDLE)-4);

    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"lumen.single.instance");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已有实例在跑：唤起它的面板后退出（再次双击 exe = 弹面板）
        if (HWND prev = FindWindowW(kMsgClass, nullptr))
            PostMessageW(prev, WM_SHOW, 0, 0);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                         RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);

    GdiplusStartupInput gi;
    ULONG_PTR tok = 0;
    GdiplusStartup(&tok, &gi, nullptr);

    LoadConfig();
    LoadTheme();
    RegisterClasses();

    g_msg = CreateWindowExW(0, kMsgClass, L"", 0, 0, 0, 0, 0,
                            HWND_MESSAGE, nullptr, hInst, nullptr);   // message-only：热键/托盘宿主
    if (!g_msg) return 1;
    CreateWindowExW(WS_EX_TOOLWINDOW, kBcastClass, L"", WS_POPUP, 0, 0, 0, 0,
                    nullptr, nullptr, hInst, nullptr);   // 隐藏顶层：接收 WM_POWERBROADCAST / WM_DISPLAYCHANGE

    AddTray();
    RegisterHotkeys();
    NotifyHotkeyFailures();
    SetTimer(g_msg, 2, 60000, nullptr);
    SetTimer(g_msg, 4, 3000, nullptr);   // HDR 状态轮询
    g_winHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);

    g_writer.Start();
    if (!g_gammaDim) ResetGammaAll();   // 归一遗留状态：上个实例被强杀时可能留下调暗的 ramp
    g_mons = Scan();
    ApplyNames(g_mons);
    RestoreBrightness();          // 重启后恢复每台屏的上次亮度
    ApplyAutoBrightness();
    CloseHandle(CreateThread(nullptr, 0, CheckUpdateTh, nullptr, 0, nullptr));

    // --show：启动后直接展开面板；--settings：直接进设置页；--diag：复制诊断信息后退出
    if (wcsstr(GetCommandLineW(), L"--show")) PostMessageW(g_msg, WM_SHOW, 0, 0);
    if (wcsstr(GetCommandLineW(), L"--settings")) {
        g_wantSettings = true;
        PostMessageW(g_msg, WM_SHOW, 0, 0);
    }
    if (wcsstr(GetCommandLineW(), L"--diag")) {
        g_mons = Scan();
        ApplyNames(g_mons);
        CopyDiagnostics();
        PostQuitMessage(0);
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    g_writer.Stop();
    ResetGammaAll();
    UnregisterHotKey(g_msg, HK_UP);
    UnregisterHotKey(g_msg, HK_DOWN);
    UnregisterHotKey(g_msg, HK_TOGGLE);
    RemoveTray();
    if (g_winHook) UnhookWinEvent(g_winHook);
    if (g_icon) DestroyIcon((HICON)g_icon);
    GdiplusShutdown(tok);
    CoUninitialize();
    return 0;
}
