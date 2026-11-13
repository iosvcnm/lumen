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
#include <lowlevelmonitorconfigurationapi.h>
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
#define WM_DIM     (WM_APP + 8)   // wParam = dev string (heap), lParam = 遮罩黑度 0-85
#define WM_WLCAL   (WM_APP + 9)   // wParam = 标定得到的滑条最大白电平（0 = 失败）

#define APP_VERSION L"1.4.0"

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
static const float BTN_R   = 27.f;
static const float BTN_DX  = 60.f;

// ---------------------------------------------------------------- 状态

static HINSTANCE g_inst = nullptr;
static HWND      g_msg = nullptr;
static HWND      g_hwnd = nullptr;
static HWND      g_osd = nullptr;
static POINT g_osdPos = { -1, -1 };   // 记忆的 OSD 位置（屏幕坐标）；负值 = 未拖过，默认右上
static bool  g_osdDrag = false;       // OSD 拖动中：暂停自动隐藏，ShowOsdNow 不重定位
static POINT g_osdAnchor{};           // 拖动锚点（客户区坐标）
static bool      g_tray = false;
static NOTIFYICONDATAW g_nid{};
static int       g_dpi = 96;
static DWORD     g_panelBorn = 0;
static bool      g_dragging = false;
static bool      g_menuOpen = false;
static int       g_menuHover = -1;
static HANDLE    g_icon = nullptr;

struct Mon {
    std::wstring dev;      // 写入分流用的设备名：DDC 屏为 \\.\DISPLAYn，内置屏为伪名 WMI
    std::wstring gdiDev;   // 真实 GDI 设备名；WMI 条目由 RefreshHdrFlags 反查内置面板路径
    std::wstring key;      // 稳定标识：显示器设备接口路径，跨重启不变
    std::wstring name;
    bool controllable = false;
    bool primary = false;
    bool wmi = false;
    bool hdr = false;      // Windows 高级色 HDR 激活（OSD 硬件 HDR 检测不到）
    ULONG sdrwl = 0;       // Windows SDR 内容白电平（HDR 激活时可读，随滑条变化）
    int  pct = 50;
    int  off = 0;          // 单屏偏移（-25..25，仅「全部显示器」同步时叠加）
    DWORD mn = 0, mx = 100;
};

static std::vector<Mon> g_mons;
static int  g_sel = -1;
static std::wstring g_cfgSel;   // 上次选中的显示器键，空 = 全部 / ALL
static int  g_val = 50;
static bool g_dark = false;
static std::atomic<bool> g_fail{false};

static int  g_cfgAuto = 0, g_cfgDelay = 25, g_cfgTheme = 0, g_cfgStart = 0;
// 自动亮度打开时的缓升过渡：从当前亮度 5 秒滑到目标（200ms 一 tick，25 步），手动调节立即取消
static bool g_autoRamp = false;       // 过渡进行中（timer 7，200ms 一 tick）
static bool g_autoEngaged = false;    // 自动亮度处于生效状态（0→1 的那一次 Apply 才启动过渡）
static bool g_rampPost = false;       // RampPost 标记：区分过渡自身写入与用户手动调节
static int  g_autoFrom = 0, g_autoTo = 0, g_autoTick = 0;
static const int AUTO_RAMP_TICKS = 25;
static const UINT AUTO_RAMP_TIMER_MS = 200;
static void AutoRampStop() {          // 取消过渡（手动调节 / 开关关闭 / 熄屏时）
    if (g_autoRamp) {
        g_autoRamp = false;
        if (g_msg) KillTimer(g_msg, 7);
    }
}
static int  g_cfgStartH = 7, g_cfgEndH = 19;
static int  g_dayPct = 80, g_nightPct = 20;   // 到点后的目标亮度：日间 / 夜间
static int  g_page = 0;   // 0 主面板 1 设置页
static int  g_subScroll = 0;   // 子页（设置/校准）内容滚动偏移：逻辑 px，向下为正；页高固定 570，内容超出时滚动
static bool g_wantSettings = false;   // 首次建窗时直接进设置页
static int  g_setFold = 0;            // 设置页分组折叠位：bit0 通用 / bit1 快捷键 / bit2 鼠标与键盘 / bit3 关于（1=展开）
static int  g_setHeaderHover = -1;    // 设置页悬停的分组标题（-1 无）

// 所有子页（设置/校准）固定与主面板同高（570）；内容超出时页内滚动（SubScrollMax/g_subScroll）
static int SettingsPageH();   // 定义在设置页几何段：展开分组的**内容总高**
static int PageH() {
    return PANEL_H;   // 所有页固定与主面板同高；内容超出时页内滚动（g_subScroll）
}
static UINT g_hkUpMod = MOD_CONTROL, g_hkUpVk = VK_PRIOR;
static UINT g_hkDownMod = MOD_CONTROL, g_hkDownVk = VK_NEXT;
static UINT g_hkShowMod = MOD_CONTROL | MOD_ALT, g_hkShowVk = 'B';
static int  g_capSlot = 0, g_hkMsg = 0;  // g_hkMsg: 0 无 / 1 被占用 / 2 需含 Ctrl 或 Alt
static int  g_hover = -1;          // 0 胶囊，1..4 快捷按钮
static bool g_screenOff = false;                // 熄屏状态：true = 已对选中屏幕发送关闭指令
static DWORD g_offAt = 0;                       // 熄屏时刻，3 秒宽限期内忽略输入，防止误触把屏幕点回来
static DWORD g_offInputBase = 0;                // 熄屏那一刻的「最后输入时间」，之后只有变化才算新输入
static bool g_moved = false;                    // 用户拖动过面板：切页与跨开关都保持位置（注册表 PanelX/PanelY）
static POINT g_movedPos{};                      // 拖动后的窗口左上角（屏幕坐标），负值 = 未拖过
// —— 子页双层缓存（滚动/重绘优化）：chrome=卡片+页眉，content=各行控件 ——
// 滚动只是 content 位图的源偏移搬运；chrome 在主题/语言/尺寸变化时重建，content 在内容变化时重建。
// 呈现目标直接是 ULW 的 PARGB DIB（尺寸变化才重建），省掉每帧位图分配与整幅 memcpy。常驻内存 +约 2MB。
static Bitmap* g_chromeBmp = nullptr;
static int     g_chromeW = 0, g_chromeH = 0, g_chromePage = -1;
static Bitmap* g_cntBmp = nullptr;
static int     g_cntW = 0, g_cntH = 0, g_cntPage = -1;
static bool    g_pageCacheDirty = true;
static Bitmap* g_presBmp = nullptr;      // 包装 ULW DIB 的 GDI+ 画布
static HDC     g_presDC = nullptr;
static HBITMAP g_presDIB = nullptr;
static int     g_presW = 0, g_presH = 0;
static bool    g_scrollAnim = false;     // 滚轮缓动进行中（timer 8 @16ms）
static int     g_scrollTo = 0;
static bool    g_scrollDrag = false;     // 滚动条拖动中（点按/拖动 thumb 直接跳转）
static void TouchPageCache() { g_pageCacheDirty = true; }
static std::vector<std::wstring> g_offDevs;     // 已熄屏的外接 DDC 设备（\\.\DISPLAYn）
static std::vector<int> g_offSaved;             // 与 g_offDevs 对应的熄屏前亮度快照（百分比）
static bool g_offAll = false;                   // 是否同时广播 SC_MONITORPOWER（内置屏 / 「全部显示器」兜底）
static std::atomic<bool> g_restoring{false};    // 恢复线程防重入

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

// 用户字体缩放（设置页「字体大小」90–120%）：所有 GDI+ 字号经 FS() 放大，行高不变（余量足够）
static int   g_fontPct = 100;
static float g_fontScale = 1.0f;
static float FS(float px) { return px * g_fontScale; }

// ---------------------------------------------------------------- 多语言（0 跟随系统 / 1 中文 / 2 English）
static int g_lang = 0;
static int g_langPref = 0;   // 注册表原始偏好（0 未解析），g_lang 是 ApplyLang 解析后的实际语言
static int g_step = 5;       // 滚轮/热键步长，可调 1/2/5/10
static int g_wheelTray = 1, g_wheelPanel = 1;   // 鼠标设置：托盘图标 / 打开面板时滚轮调节亮度
static int g_keyBright = 0;                     // 键盘 HID 亮度键（Consumer 0x6F/0x70），默认关
static unsigned long long g_lastKeyTick = 0;    // 亮度键去抖（按住键 auto-repeat ~30/s）
static int g_gammaDim = 0;   // 软件调光：HDR 固件锁 DDC 亮度时的 gamma 回退通路
static std::atomic<float> g_baseL{1.f};   // 仅供提示/诊断：当前白电平 ÷ 标定最大值（1 = 未标定 / 滑条最右）
static DWORD g_dispChangeTick = 0;        // 最近一次显示模式切换（HDR 开关等）
static std::atomic<bool> g_gammaDead{false}; // HDR 下 gamma LUT 不生效（读回校验失败）→ 全区间遮罩
static std::atomic<bool> g_hdrAny{false};    // 任一活动显示路径处于 Windows HDR（屏被 Scan 丢弃时兜底）
static std::atomic<ULONG> g_hdrAnyWl{0};    // 上述路径的 SDR 白电平（提示行 / 基准补偿兜底）
static POINT g_iconPt{};                    // 托盘图标实时位置（悬停时由 WM_MOUSEMOVE 更新）
static bool g_iconHover = false;            // 是否已获得图标位置（获得后滚轮判定收窄到图标邻域）
static bool g_calibrating = false;          // 校准中：下一次托盘左键点击记录图标位置
static int g_wlMax = 0;                   // 标定的滑条最大白电平（0 = 未标定）
enum {
    S_BRIGHTNESS, S_THEME, S_AUTO, S_LIGHT, S_DARK,
    S_ALLMON, S_NOCTRL, S_ALLMONFMT, S_BUILTIN, S_PRIMARY, S_EXTERNAL, S_NOADJ,
    S_BUILTINSCR, S_GENERICFMT,
    S_HINT, S_FAIL, S_BACK, S_SETTINGS, S_AUTOSTART, S_AUTOBR, S_STARTH, S_ENDH, S_DDC, S_STEP,
    S_HOTKEYS, S_UP, S_DOWN, S_SHOWPILL, S_PRESSKEY, S_OCCUPIED, S_NEEDMOD, S_MODIFY, S_CANCEL, S_SIG,
    S_SHOWPANEL, S_BRIGHTUP, S_BRIGHTDN, S_DIAG, S_EXIT, S_LANG, S_GAMMA,
    S_OCCUPFMT, S_UPDATEFMT, S_HDR, S_DAYBR, S_NIGHTBR,
    S_MOUSESET, S_WHEELTRAY, S_WHEELPANEL, S_KEYBRIGHT,
    S_RESTOFF, S_RESTON,
    S_CAL, S_CALDONE, S_CALFAIL, S_CALIB, S_CALIBHINT, S_CALDONE2,
    S_OFFPG, S_OFFHINT, S_OFFRESET,
    S_GENERAL, S_ABOUT, S_HOME, S_HOOKFAIL,
    S_FOLL, S_FONTS,
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
    L"日间亮度", L"夜间亮度",
    L"鼠标与键盘", L"托盘图标上滚轮调节亮度", L"面板打开时滚轮调节亮度", L"键盘亮度键（亮度 +/−）",
    L"屏幕已关闭", L"屏幕已恢复",
    L"标定 HDR 基准亮度", L"标定完成：基准 %lu", L"标定失败：请先拖动「SDR 内容亮度」滑条",
    L"校准滚轮图标位置", L"请点击任务栏上的 lumen 图标", L"已记录滚轮位置 (%d,%d)",
    L"单屏偏移", L"只影响「全部显示器」同步；单屏直接调节不偏移", L"全部归零",
    L"通用", L"关于", L"项目主页", L"键盘钩子安装失败：点击面板后仍可录入",
    L"跟随系统", L"字体大小",
};
static const wchar_t* const STR_EN[S_COUNT] = {
    L"Brightness", L"Theme", L"Auto", L"Light", L"Dark",
    L"All monitors", L"No controllable monitor", L"All monitors (%d)", L" (built-in)", L" (primary)", L" (secondary)", L" · fixed",
    L"Builtin display", L"Generic monitor %d",
    L"Click to switch monitor", L"Write failed: check DDC/CI", L"←  Back", L"Settings", L"Start with Windows", L"Time-based auto",
    L"Start", L"End", L"DDC delay", L"Step",
    L"Global hotkeys", L"Up", L"Down", L"Show", L"Press Ctrl or Alt + key…", L"Combo occupied by another app, keeping the old one",
    L"Needs Ctrl or Alt, e.g. Ctrl+Alt+Up", L"Edit", L"Cancel",
    L"lumen · DDC/CI + WMI hybrid control",
    L"Show panel", L"Brightness +10", L"Brightness -10", L"Copy diagnostics", L"Exit lumen", L"Language",
    L"Software dimming (for HDR monitors)",
    L"%ls %ls: combo occupied", L"lumen %ls is available\nhttps://github.com/iosvcnm/lumen/releases/latest",
    L" · HDR",
    L"Day", L"Night",
    L"Mouse & keyboard", L"Wheel on tray icon adjusts brightness", L"Wheel in panel adjusts brightness",
    L"Keyboard brightness keys",
    L"Screen is off", L"Screen restored",
    L"Calibrate HDR base brightness", L"Calibration done: base %lu", L"Calibration failed: drag the SDR content brightness slider",
    L"Calibrate wheel icon position", L"Click the lumen icon on the taskbar", L"Wheel position recorded (%d,%d)",
    L"Per-screen offset", L"Sync offset applies to All-monitors only",
    L"Reset all",
    L"General", L"About", L"Project page", L"Keyboard hook failed: click the panel first to record",
    L"Follow system", L"Font size",
};
static const wchar_t* tr(int id) { return (g_lang == 2) ? STR_EN[id] : STR_CN[id]; }
static int RegGet(const wchar_t* name, int def);   // 定义在配置区，ApplyLang 先用
static void ApplyLang() {
    TouchPageCache();                        // 子页文字全部随语言变化，内容缓存作废
    int v = RegGet(L"Lang", 0);
    g_langPref = v;                          // 设置页显示用原始偏好（0=跟随系统）
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
    g_dayPct    = std::max(0, std::min(100, RegGet(L"DayPct", 80)));
    g_nightPct  = std::max(0, std::min(100, RegGet(L"NightPct", 20)));
    if (g_cfgEndH <= g_cfgStartH) g_cfgEndH = std::min(23, g_cfgStartH + 1);
    g_hkUpMod  = (UINT)RegGet(L"HotUpMod", MOD_CONTROL);
    g_hkUpVk   = (UINT)RegGet(L"HotUpVk", VK_PRIOR);
    g_hkDownMod = (UINT)RegGet(L"HotDownMod", MOD_CONTROL);
    g_hkDownVk  = (UINT)RegGet(L"HotDownVk", VK_NEXT);
    g_hkShowMod = (UINT)RegGet(L"HotShowMod", MOD_CONTROL | MOD_ALT);
    g_hkShowVk  = (UINT)RegGet(L"HotShowVk", 'B');
    g_cfgSel    = RegGetStr(L"SelMonitor", L"");
    g_gammaDim = RegGet(L"GammaDim", 0);
    g_wheelTray  = RegGet(L"WheelTray", 1) ? 1 : 0;
    g_wheelPanel = RegGet(L"WheelPanel", 1) ? 1 : 0;
    g_keyBright  = RegGet(L"KeyBright", 0) ? 1 : 0;
    g_setFold = RegGet(L"SetFold", 0) & 0xF;   // 分组折叠状态（bit0 通用 / bit1 快捷键 / bit2 鼠标 / bit3 关于）
    g_wlMax = RegGet(L"WlMax", 0);
    { int tx = RegGet(L"TrayX", -1), ty = RegGet(L"TrayY", -1);
      if (tx >= 0 && ty >= 0) { g_iconPt = POINT{tx, ty}; g_iconHover = true; } }
    g_osdPos.x = (int)RegGet(L"OsdX", (DWORD)-1);   // 屏幕坐标可为负（副屏在主屏左侧），-1 = 未拖过
    g_osdPos.y = (int)RegGet(L"OsdY", (DWORD)-1);
    if (g_osdPos.x < 0 || g_osdPos.y < 0) { g_osdPos.x = -1; g_osdPos.y = -1; }
    g_movedPos.x = (int)RegGet(L"PanelX", (DWORD)-1);   // 面板记忆位置：拖动结束持久化，打开时越出工作区则收回
    g_movedPos.y = (int)RegGet(L"PanelY", (DWORD)-1);
    if (g_movedPos.x < 0 || g_movedPos.y < 0) { g_movedPos.x = -1; g_movedPos.y = -1; }
    g_moved = (g_movedPos.x >= 0 && g_movedPos.y >= 0);
    int st = RegGet(L"Step", 5);
    g_step = (st == 1 || st == 2 || st == 5 || st == 10) ? st : 5;
    g_fontPct = RegGet(L"FontPct", 100);
    if (g_fontPct < 90 || g_fontPct > 110 || g_fontPct % 10) g_fontPct = 100;   // 90–110%，120% 会标签压数值
    g_fontScale = g_fontPct / 100.0f;
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
    TouchPageCache();                        // 快捷键 pill 文本变化
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
static bool  g_hookDead = false;   // 最近一次录制钩子安装失败（兜底：面板聚焦时仍可录入）
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
    TouchPageCache();                        // 录制 pill 文本变化
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
    TouchPageCache();
    g_capSlot = slot;
    g_hkMsg = 0;
    g_hookMod = 0;
    // 无条件重装钩子：LL 钩子被系统静默摘除后（回调超时的经典行为，UI 线程忙时发生），
    // 旧句柄还在、回调却永不再触发，"if (!g_kbHook)" 会一直复用死钩子——
    // 表现为录制只剩面板 WM_KEYDOWN 兜底路径，Alt 组合（WM_SYSKEYDOWN）录不进。
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKey, g_inst, 0);
    g_hookDead = (g_kbHook == nullptr);
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
    TouchPageCache();                        // 调色板整体变化
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
            m.gdiDev = mi.szDevice;
            m.key = MonitorKey(mi.szDevice);
            m.off = std::max(-25, std::min(25, RegGet((L"Off_" + m.key).c_str(), 0)));
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
        m.dev = L"WMI";        // 写入走 WmiSetBrightness，不是 GDI 设备名
        m.gdiDev.clear();      // 由 RefreshHdrFlags 反查内置面板的真实 GDI 名
        m.key = L"WMI";
        m.off = std::max(-25, std::min(25, RegGet(L"Off_WMI", 0)));
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

// ---- Gamma 备份：进入软件调光前的原始 LUT（含校色曲线），退出时原样写回 ----
struct RampBak { std::wstring dev; WORD r[3][256]; };
static std::vector<RampBak> g_rampBak;

static RampBak* FindRampBak(const std::wstring& dev) {
    for (auto& b : g_rampBak) if (b.dev == dev) return &b;
    return nullptr;
}

static void BackupRamp(const std::wstring& dev, HDC dc) {
    if (FindRampBak(dev)) return;
    RampBak b;
    b.dev = dev;
    if (!GetDeviceGammaRamp(dc, b.r)) return;
    // 上次被强杀可能留下调暗的 LUT（中点明显低于恒等）：先归一，再作为备份
    if (b.r[0][128] < 27900) {
        WORD id[3][256];
        for (int i = 0; i < 256; ++i)
            id[0][i] = id[1][i] = id[2][i] = (WORD)std::min(65535L, (long)i * 257);
        SetDeviceGammaRamp(dc, id);
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i) b.r[c][i] = id[c][i];
    }
    g_rampBak.push_back(b);
}

// 在原始 LUT 上乘系数，保留校色曲线；没有备份时才退回恒等 ramp
// 每屏 DC 只建一次并复用：HDR 模式下 CreateDCW/DeleteDC 可能永久阻塞，
// 反复创建会把写入线程卡死，后续调节全部被跳过。
static std::map<std::wstring, HDC> g_dcCache;

static HDC DcCached(const std::wstring& dev) {
    auto it = g_dcCache.find(dev);
    if (it != g_dcCache.end()) return it->second;
    HDC dc = DcForDev(dev);
    if (dc) g_dcCache[dev] = dc;
    return dc;
}

static void DropDcCache() {          // 显示拓扑变化后重建（旧 DC 不再 Delete，避免卡死）
    g_dcCache.clear();
}

static bool WriteRamp(HDC dc, const std::wstring& dev, float scale) {
    WORD out[3][256];
    const RampBak* b = FindRampBak(dev);
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; ++i) {
            long v = b ? (long)b->r[c][i] : std::min(65535L, (long)i * 257);
            out[c][i] = (WORD)std::min(65535L, (long)(v * scale));
        }
    BOOL w = SetDeviceGammaRamp(dc, out);
    WORD chk[3][256];
    if (!w || !GetDeviceGammaRamp(dc, chk)) return false;
    return std::abs((int)chk[0][128] - (int)out[0][128]) <= 64;   // 读回校验：HDR 下部分驱动直接忽略 LUT
}

// 退出 / 关闭软件调光：把备份的原始 LUT 写回（没有备份的屏退回恒等）
static void RestoreRampsAll() {
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
        RampBak* b = FindRampBak(mi.szDevice);
        if (b) {
            SetDeviceGammaRamp(dc, b->r);
        } else {
            WORD id[3][256];
            for (int i = 0; i < 256; ++i)
                id[0][i] = id[1][i] = id[2][i] = (WORD)std::min(65535L, (long)i * 257);
            SetDeviceGammaRamp(dc, id);
        }
        DeleteDC(dc);
    }
}

// HDR 软件调光的两段式（10% 临界点，两段在 10% 处亮度连续）：
//   pct >= 10%：纯 gamma LUT，遮罩 alpha = 0
//   pct <  10%：gamma 锁死在 10% 的基准值，遮罩 alpha 从 0 线性加深到 0.94（透光 6%）
// 百分比语义：相对「当前 Windows SDR 白电平」的调光比例——100% = 恒等 LUT（当前最亮）。
// WlMax / g_baseL 只用于提示行与诊断，不再参与换算，整条 0–100% 都有响应。
// gamma 在 HDR 下不生效时（部分驱动只对 SDR 叠加层应用 LUT），退化为全区间遮罩。
static void ComputeDim(int pct, float* scaleOut, int* dimOut) {
    float target = std::max(0, std::min(100, pct)) / 100.f;

    if (g_gammaDead.load()) {             // 全区间遮罩：gamma 不动，遮罩直接按总亮度压
        *scaleOut = 1.f;
        float t = target;
        if (t < 0.06f) t = 0.06f;
        *dimOut = (int)lround((1.f - t) * 100.f);
        return;
    }

    const float kKnee = 0.10f;            // 10% 临界点
    float scale = powf(target, 1.f / 2.2f);   // 100% → 1.0，不再按标定基准钳制

    int dim = 0;
    if (target < kKnee) {
        scale = powf(kKnee, 1.f / 2.2f);  // 10% 处锁定的 gamma 系数
        float t = target / kKnee;         // 遮罩透射率：10% → 1，0% → 0
        if (t < 0.06f) t = 0.06f;         // 最深 ≈ 总亮度 6%
        dim = (int)lround((1.f - t) * 100.f);
    }
    *scaleOut = scale;
    *dimOut = dim;
}

static bool ApplyGammaScale(const std::wstring& dev, float scale) {
    // 在 Writer 线程里同步写：HDR 下 GDI 调用可能很慢甚至卡住，
    // 放到子线程会造成写入错序（后一次调节被前一次覆盖），不如直接写、顺序确定。
    HDC dc = DcCached(dev);
    if (!dc) return false;
    BackupRamp(dev, dc);
    bool ok = WriteRamp(dc, dev, scale);
    if (!ok) {
        static int s_bad = 0;
        if (++s_bad >= 2) {
            g_gammaDead.store(true);      // 连续两次读回不符：此环境 LUT 不生效，转全区间遮罩
            const RampBak* b = FindRampBak(dev);
            if (b) SetDeviceGammaRamp(dc, (LPVOID)b->r);
        }
    } else {
        static int s_ok = 0;
        (void)s_ok;
    }
    return ok;
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
        WriteRamp(dc, mi.szDevice, 1.f);   // 1.0 = 写回备份的原始 LUT
        DeleteDC(dc);
    }
}

// ---- 深度调光遮罩：gamma 下限（50%）以下的补充层 ----
// 黑色点击穿透置顶窗盖住整块屏（面板/OSD 会插回其上方）。
// 窗口生命周期只在主线程；写入线程经 WM_DIM 通知。

struct DimWnd { std::wstring dev; HWND wnd; };
static std::vector<DimWnd> g_dims;

static void OverlayRegisterClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSW c{};
    c.lpfnWndProc = DefWindowProcW;
    c.hInstance = g_inst;
    c.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
    c.lpszClassName = L"LumenDimWnd";
    RegisterClassW(&c);
    done = true;
}

static HWND OverlayFind(const std::wstring& dev) {
    for (auto& d : g_dims) if (d.dev == dev) return d.wnd;
    return nullptr;
}

static void OverlayClose(const std::wstring& dev) {
    for (size_t i = 0; i < g_dims.size(); ++i)
        if (g_dims[i].dev == dev) {
            if (g_dims[i].wnd) PostMessageW(g_dims[i].wnd, WM_CLOSE, 0, 0);
            g_dims.erase(g_dims.begin() + i);
            return;
        }
}

static void OverlayCloseAll() {
    for (auto& d : g_dims)
        if (d.wnd) PostMessageW(d.wnd, WM_CLOSE, 0, 0);
    g_dims.clear();
}

static RECT OverlayMonitorRect(const std::wstring& dev) {
    RECT r{};
    std::vector<HMONITOR> hs;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM lp) {
        ((std::vector<HMONITOR>*)lp)->push_back(h); return TRUE;
    }, (LPARAM)&hs);
    for (HMONITOR h : hs) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(h, (MONITORINFO*)&mi)) continue;
        if (dev == mi.szDevice) r = mi.rcMonitor;
    }
    return r;
}

// 主线程调用：blackPct 0-85，0 = 关闭该屏遮罩
static void OverlayApply(const std::wstring& dev, int blackPct) {
    if (!g_gammaDim || blackPct <= 0) { OverlayClose(dev); return; }
    OverlayRegisterClass();
    HWND w = OverlayFind(dev);
    RECT r = OverlayMonitorRect(dev);
    if (!w) {
        w = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                            L"LumenDimWnd", L"", WS_POPUP,
                            r.left, r.top, r.right - r.left, r.bottom - r.top,
                            nullptr, nullptr, g_inst, nullptr);
        if (!w) return;
        SetLayeredWindowAttributes(w, 0, 0, LWA_ALPHA);
        ShowWindow(w, SW_SHOWNOACTIVATE);
        g_dims.push_back({dev, w});
    } else {
        SetWindowPos(w, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
    }
    SetLayeredWindowAttributes(w, 0, (BYTE)(255 * std::min(94, blackPct) / 100), LWA_ALPHA);
    if (g_hwnd) SetWindowPos(w, g_hwnd, 0, 0, 0, 0,   // 遮罩垫到面板之下
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void OverlayPrune() {   // 拓扑变化后：裁掉已消失的屏、重定位余下遮罩（主线程）
    for (size_t i = g_dims.size(); i-- > 0;) {
        bool found = false;
        for (auto& m : g_mons) if (m.dev == g_dims[i].dev) { found = true; break; }
        if (!found) OverlayClose(g_dims[i].dev);
    }
    for (auto& d : g_dims) {
        RECT r = OverlayMonitorRect(d.dev);
        SetWindowPos(d.wnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
    }
}

static std::atomic<long long> g_verifyDeadUntil{0};   // 假成功设备冷却：读回不符且重试仍失败后 5s 内不再重试

static bool WriteOne(const std::wstring& dev, int pct, unsigned delayMs) {
    if (g_gammaDim && dev != L"WMI") {          // 软件调光：gamma + 遮罩替代 DDC（HDR 固件锁亮度时用）
        float scale = 1.f;
        int dim = 0;
        ComputeDim(pct, &scale, &dim);          // 白电平补偿：pct = 总亮度（Windows 基准无关）
            bool ok = true;
            if (g_gammaDead.load()) {
                // gamma LUT 在此环境不生效：不再写 ramp，全部交给遮罩
            } else if (g_dispChangeTick && GetTickCount() - g_dispChangeTick < 2500) {
                g_dispChangeTick = 1;               // 切换静默期：延后，下次调节自动应用
            } else {
                ok = ApplyGammaScale(dev, scale);
            }
        if (g_msg) PostMessageW(g_msg, WM_DIM, (WPARAM)_wcsdup(dev.c_str()), (LPARAM)dim);
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
            // 读回校验：部分显示器（Redmi 238NFS 等）切到非标准显示模式后，固件静默丢弃
            // DDC/CI 写入但 API 仍返回成功（假成功）——旧逻辑此时返回 true，面板永远不提示。
            // 写入后读回比对（±1 容忍舍入），不符且设备未进冷却则重试一次；仍不符视为失败，
            // 让面板红字（S_FAIL）出现，用户可自查显示模式 / DDC/CI 开关。
            bool done = SetMonitorBrightness(v[i].hPhysicalMonitor, raw) != FALSE;
            if (done) {
                DWORD back = 0;
                if (GetMonitorBrightness(v[i].hPhysicalMonitor, &mn, &back, &mx)) {
                    long d = (long)back - (long)raw;
                    if (d < -1 || d > 1) {
                        bool retry = g_verifyDeadUntil.load() <= (long long)GetTickCount64();
                        if (retry) {
                            Sleep(50);
                            done = SetMonitorBrightness(v[i].hPhysicalMonitor, raw) != FALSE;
                            DWORD rb = 0;
                            if (done && GetMonitorBrightness(v[i].hPhysicalMonitor, &mn, &rb, &mx)) {
                                d = (long)rb - (long)raw;
                                if (d < -1 || d > 1) done = false;
                            }
                            if (!done) g_verifyDeadUntil.store((long long)GetTickCount64() + 5000);
                        } else {
                            done = false;   // 冷却期内：不再花重试开销，直接判失败
                        }
                    }
                }
            }
            if (done) any = true;
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

// ---------------------------------------------------------------- 熄屏功能（VCP 0xD6 电源模式）

// 前向声明（HidePanel/ShowOsdMode 定义在下方）
static void HidePanel();
static void ShowOsdMode(const wchar_t* m);

// 通过 DDC/CI VCP 0xD6 设置显示器电源模式：1=On 2=DPM Off 3=Standby 4=Off
// 0xD6 为独立电源寄存器，不受 HDR 固件锁 0x10 亮度影响，HDR 开/关均可生效
static bool SetMonitorPowerDDC(const std::wstring& dev, DWORD mode) {
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
        for (DWORD i = 0; i < cnt; ++i)
            if (SetVCPFeature(v[i].hPhysicalMonitor, 0xD6, mode)) any = true;
        DestroyPhysicalMonitors(cnt, v.data());
        return any;
    }
    return false;
}

// 读取 DDC/CI VCP 0xD6 当前电源模式；读不到（已被关断、DDC 掉线）返回 -1
static int GetMonitorPowerDDC(const std::wstring& dev) {
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
        int val = -1;
        for (DWORD i = 0; i < cnt; ++i) {
            MC_VCP_CODE_TYPE type{};
            DWORD cur = 0, mx = 0;
            if (GetVCPFeatureAndVCPFeatureReply(v[i].hPhysicalMonitor, 0xD6, &type, &cur, &mx)) {
                val = (int)cur;
                break;
            }
        }
        DestroyPhysicalMonitors(cnt, v.data());
        return val;
    }
    return -1;
}

// 关掉一台外接屏的电源：0xD6 的 4(Off soft) / 5(Off hard) 各试一次，写入后读回确认。
// 读不到（DDC 随熄屏断开）或已不是 1(On) 都视为成功；返回 false 表示固件忽略了这个寄存器。
static bool PowerOffDDC(const std::wstring& dev) {
    for (DWORD mode : { 4u, 5u }) {
        if (!SetMonitorPowerDDC(dev, mode)) continue;
        Sleep(200);
        int cur = GetMonitorPowerDDC(dev);
        if (cur < 0 || cur != 1) return true;
    }
    return false;
}

// 关闭选中屏幕：
// - 选中外接 DDC 屏 → 发送 VCP 0xD6=5
// - 选中内置屏（WMI）或「全部显示器」→ DDC 屏逐一 0xD6=5，同时广播 SC_MONITORPOWER=2 兜底
// 记录熄屏前亮度快照，待用户任意输入后恢复
static void TurnOffSelectedScreen() {
    if (g_screenOff || g_restoring) return;
    g_offDevs.clear();
    g_offSaved.clear();
    g_offAll = false;
    if (g_mons.empty()) g_mons = Scan();

    auto snapFor = [&] (const Mon& m) -> int {
        // gammaDim 模式下 DDC 回读无意义，快照取注册表 Last_<key>（UI 值）
        int snap = m.pct;
        if (g_gammaDim) snap = RegGet((L"Last_" + m.key).c_str(), m.pct);
        return (snap < 0 || snap > 100) ? 50 : snap;
    };

    if (g_sel >= 0 && g_sel < (int)g_mons.size()) {
        const Mon& m = g_mons[g_sel];
        if (m.dev != L"WMI") {
            g_offDevs.push_back(m.dev);
            g_offSaved.push_back(snapFor(m));
        } else {
            g_offAll = true;   // 内置屏无 DDC 通道，走系统广播
        }
        RegPut((L"Last_" + m.key).c_str(), m.pct);
    } else {
        for (auto& m : g_mons) {
            if (!m.controllable) continue;
            if (m.dev != L"WMI") {
                g_offDevs.push_back(m.dev);
                g_offSaved.push_back(snapFor(m));
            } else {
                g_offAll = true;
            }
            RegPut((L"Last_" + m.key).c_str(), m.pct);
        }
        g_offAll = true;   // 「全部显示器」一律广播覆盖未被 DDC 覆盖的屏
    }

    if (g_offDevs.empty() && !g_offAll) return;

    bool ddcFail = false;
    for (auto& dev : g_offDevs) if (!PowerOffDDC(dev)) ddcFail = true;
    if (g_offAll || ddcFail)   // DDC 电源寄存器不被支持时，退回系统级关屏
        PostMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, (LPARAM)2);

    g_screenOff = true;
    g_offAt = GetTickCount();
    {   // 记录熄屏时的输入时间戳：点击按钮本身也会更新它，不能拿它当「用户又动了」
        LASTINPUTINFO li{};
        li.cbSize = sizeof(li);
        g_offInputBase = GetLastInputInfo(&li) ? li.dwTime : g_offAt;
    }
    SetTimer(g_msg, 6, 150, nullptr);   // 轮询任意输入，熄屏后点亮并回放亮度
    ShowOsdMode(tr(S_RESTOFF));
    HidePanel();
}

// 用户输入触发恢复：点亮屏幕并回放熄屏前亮度（实现见 Writer 定义之后）
static void RestoreFromScreenOff();

// 刷新每台屏的 HDR 激活标志；返回是否有变化（供轮询决定是否重绘）
// 注意：高级色查询必须用路径的 targetInfo（source id 会返回 ERROR_GEN_FAILURE）
// 内置屏（WMI 通路）的 dev 是伪名 "WMI"，与 DisplayConfig 的 \\.\DISPLAYn 对不上，
// 过去查不到就一直是 hdr=false / sdrwl=0，徽章与白电平读数全灭、面板退回 SDR 显示。
// 这里按内置面板路径（embedded / INTERNAL 输出技术）反查它的真实 GDI 名再取状态；
// 驱动没标内置时再退到「未被 DDC 条目占用的 GDI 名」，最后用任一 HDR 路径兜底。
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
    std::map<std::wstring, ULONG> wlOf;
    std::wstring internalGdi;              // 内置面板对应的 GDI 设备名
    bool anyHdr = false;
    ULONG anyWl = 0;                       // 任一 HDR 路径的白电平（屏被 Scan 丢弃时兜底）
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
            hdr = a.activeColorMode == 2;   // 仅真 HDR；value 的 bit1 是高级色总开关，WCG 也会置位
        } else {
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO b{};
            b.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
            b.header.size = sizeof(b);
            b.header.adapterId = paths[i].targetInfo.adapterId;
            b.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&b.header) == ERROR_SUCCESS)
                hdr = (b.value & 2) != 0;   // advancedColorEnabled
        }
        ULONG wl = 0;
        {
            struct WLQ { DISPLAYCONFIG_DEVICE_INFO_HEADER header; ULONG SDRWhiteLevel; } w{};
            w.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            w.header.size = sizeof(w);
            w.header.adapterId = paths[i].targetInfo.adapterId;
            w.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&w.header) == ERROR_SUCCESS) wl = w.SDRWhiteLevel;
        }
        hdrOf[src.viewGdiDeviceName] = hdr;
        wlOf[src.viewGdiDeviceName] = wl;
        if (hdr) {
            anyHdr = true;
            if (!anyWl && wl) anyWl = wl;
        }
        // 内置面板：DISPLAYPORT_EMBEDDED(11) / UDI_EMBEDDED(13) / LVDS(6) / INTERNAL(0x80000000)
        UINT32 tech = (UINT32)paths[i].targetInfo.outputTechnology;
        bool embedded = (tech == 11 || tech == 13 || tech == 6 || tech == 0x80000000u);
        if (internalGdi.empty() && embedded)
            internalGdi = src.viewGdiDeviceName;
    }

    // 驱动没标内置面板时：取没有被任何 DDC 条目占用的 GDI 名作为内置屏
    if (internalGdi.empty()) {
        for (auto& kv : hdrOf) {
            bool used = false;
            for (auto& m : mons) if (!m.wmi && m.dev == kv.first) { used = true; break; }
            if (!used) { internalGdi = kv.first; break; }
        }
    }

    bool changed = false;
    for (auto& m : mons) {
        std::wstring key = m.dev;
        bool now = false;
        ULONG wl = 0;
        if (m.wmi && !internalGdi.empty()) {
            m.gdiDev = internalGdi;
            key = internalGdi;              // 用真实 GDI 名取 HDR 状态与白电平
        } else if (!m.wmi) {
            m.gdiDev = m.dev;
        }
        if (m.wmi && internalGdi.empty()) {
            now = anyHdr; wl = anyWl;      // 拓扑异常兜底：至少按真实 HDR 状态走，不退回 SDR
        } else {
            auto it = hdrOf.find(key);
            now = (it != hdrOf.end()) && it->second;
            auto iw = wlOf.find(key);
            wl = (iw != wlOf.end()) ? iw->second : 0;
        }
        if (now != m.hdr) { m.hdr = now; changed = true; }
        if (wl != m.sdrwl) { m.sdrwl = wl; changed = true; }
    }

    if (g_hdrAny.load() != anyHdr) { g_hdrAny.store(anyHdr); changed = true; }
    if (g_hdrAnyWl.load() != anyWl) { g_hdrAnyWl.store(anyWl); changed = true; }

    // 诊断用基准系数（不再参与 ComputeDim 换算）：优先取当前选中屏白电平 ÷ 标定最大值
    float baseL = 1.f;
    bool gotBase = false;
    if (g_wlMax >= 1500) {
        if (g_sel >= 0 && g_sel < (int)mons.size() && mons[g_sel].hdr && mons[g_sel].sdrwl >= 500) {
            baseL = (float)mons[g_sel].sdrwl / (float)g_wlMax;
            gotBase = true;
        } else {
            for (auto& m : mons)
                if (m.hdr && m.sdrwl >= 500) {
                    baseL = (float)m.sdrwl / (float)g_wlMax;
                    gotBase = true;
                    break;
                }
        }
        if (!gotBase && anyHdr && anyWl >= 500)
            baseL = (float)anyWl / (float)g_wlMax;   // 屏被 DDC 读失败丢掉时用真实白电平
    }
    if (baseL > 1.f) baseL = 1.f;
    if (baseL < 0.1f) baseL = 1.f;        // 异常值 → 退回 1
    g_baseL.store(baseL);
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
    TouchPageCache();                        // 校准页显示器名称异步到达
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
    std::vector<std::pair<std::wstring, int>> jobs;   // (设备, 目标百分比)，各屏可不同（单屏偏移）
    bool has = false, quit = false;
    std::thread th;
    std::atomic<long long> sickUntil{0};   // DDC 无响应冷却期（GetTickCount64 ms）：HDR 切换等场景
                                           // DDC 会短暂死亡，阻塞式写入 API 无超时，线程可能永久卡死

    void Start() { th = std::thread([this] { Run(); }); }

    void Post(std::vector<std::pair<std::wstring, int>> dp) {
        { std::lock_guard<std::mutex> lk(m); jobs = std::move(dp); has = true; }
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
            std::vector<std::pair<std::wstring, int>> dp;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [this] { return has || quit; });
                if (quit) break;
                dp = std::move(jobs);
                has = false;
            }
            int gapMs = std::max(16, g_cfgDelay);
            auto gap = std::chrono::milliseconds(gapMs) - (std::chrono::steady_clock::now() - last);
            if (gap.count() > 0) std::this_thread::sleep_for(gap);

            if (GetTickCount64() < sickUntil.load()) continue;   // 冷却期：丢弃硬件写入（状态层已记账）

            // 写入放独立线程 + 4s 看门狗：DDC 卡死时放弃该批（子线程自行了结），
            // 主循环继续服务后续调节，冷却 8s 后自动重试，避免一条死写拖垮全部亮度功能
            auto done = std::make_shared<std::atomic<bool>>(false);
            auto okv = std::make_shared<std::atomic<bool>>(true);
            std::thread([dp, done, okv]() {
                bool o = true;
                for (auto& j : dp) if (!WriteOne(j.first, j.second, 0)) o = false;
                okv->store(o);
                done->store(true);
            }).detach();
            for (int i = 0; i < 80 && !done->load(); ++i) Sleep(50);

            bool ok;
            if (done->load()) {
                ok = okv->load();
            } else {
                ok = false;
                sickUntil.store((long long)GetTickCount64() + 8000);
            }
            g_fail = !ok;
            last = std::chrono::steady_clock::now();
        }
        CoUninitialize();
    }
};
static Writer g_writer;

// 用户输入触发恢复：点亮屏幕并回放熄屏前亮度
// 亮度恢复统一走 g_writer.Post，WriteOne 内部按 g_gammaDim 分流：
// HDR/软件调光模式走 gamma ramp，非 HDR 走 DDC 亮度，两种模式下均能正确还原
static void RestoreFromScreenOff() {
    if (!g_screenOff) return;
    g_screenOff = false;
    g_restoring = true;
    ShowOsdMode(tr(S_RESTON));

    std::vector<std::wstring> devs = g_offDevs;
    std::vector<int> saved = g_offSaved;
    g_offDevs.clear();
    g_offSaved.clear();

    std::thread([devs, saved]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (size_t i = 0; i < devs.size(); ++i) {
            SetMonitorPowerDDC(devs[i], 1);   // 1 = On
            Sleep(350);
            SetMonitorPowerDDC(devs[i], 1);   // 部分固件需二次点亮
            g_writer.Post({ { devs[i], saved[i] } });
        }
        CoUninitialize();
        g_restoring = false;
    }).detach();
}

static void Post(int pct) {
    if (g_screenOff) return;   // 熄屏期间禁止写亮度：避免污染 Last_<key> 记忆（恢复走 g_writer.Post 绕过此入口）
    if (g_autoRamp && !g_rampPost) AutoRampStop();   // 过渡期间用户手动调节：立即取消过渡，尊重手动值
    pct = std::max(0, std::min(100, pct));
    g_val = pct;
    std::vector<std::pair<std::wstring, int>> dp;   // 单屏偏移只在「全部显示器」同步时叠加
    if (g_sel >= 0 && g_sel < (int)g_mons.size()) {
        dp.push_back({ g_mons[g_sel].dev, pct });
    } else {
        for (auto& m : g_mons) if (m.controllable)
            dp.push_back({ m.dev, std::max(0, std::min(100, pct + m.off)) });
    }
    for (auto& j : dp)
        for (auto& m : g_mons)
            if (m.dev == j.first) {
                m.pct = j.second;                                          // 记录实际落到的值（含偏移）
                RegPut((L"Last_" + m.key).c_str(), j.second);              // 唤醒/重启后按实际值恢复
            }
    g_writer.Post(std::move(dp));   // 注意：dp 已被 move，记账必须放在这之前
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void Adjust(int delta) {
    if (g_mons.empty()) g_mons = Scan();
    if (g_sel >= 0 && g_sel < (int)g_mons.size()) {
        Post(g_mons[g_sel].pct + delta);
        return;
    }
    int sum = 0, n = 0;
    for (auto& m : g_mons) if (m.controllable) { sum += m.pct; ++n; }
    Post((n ? sum / n : g_val) + delta);
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
            g_writer.Post({ { m.dev, last } });
            continue;
        }
        if (last == m.pct) continue;
        m.pct = last;
        g_writer.Post({ { m.dev, last } });
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
    swprintf(b, 320, L"\r\nlang=%d theme=%d step=%d ddc=%d auto=%d(%d-%d) autostart=%d gamma=%d wheel=%d/%d key=%d hookFail=%d",
             g_lang, g_cfgTheme, g_step, g_cfgDelay, g_cfgAuto, g_cfgStartH, g_cfgEndH,
             g_cfgStart, g_gammaDim, g_wheelTray, g_wheelPanel, g_keyBright, g_hookDead ? 1 : 0);
    t += b;
    swprintf(b, 320, L"\r\nhotkeys: up=%ls down=%ls show=%ls",
             HotkeyText(g_hkUpMod, g_hkUpVk).c_str(),
             HotkeyText(g_hkDownMod, g_hkDownVk).c_str(),
             HotkeyText(g_hkShowMod, g_hkShowVk).c_str());
    t += b;
    swprintf(b, 320, L"\r\nhdrAny=%d hdrAnyWl=%lu baseL=%.3f gammaDead=%d wlMax=%d",
             g_hdrAny.load() ? 1 : 0, g_hdrAnyWl.load(), g_baseL.load(),
             g_gammaDead.load() ? 1 : 0, g_wlMax);
    t += b;
    for (auto& m : g_mons) {
        swprintf(b, 320, L"\r\nmonitor: %ls | key=%ls | dev=%ls | gdi=%ls | ctrl=%d pct=%d hdr=%d wl=%lu",
                 m.name.c_str(), m.key.c_str(), m.dev.c_str(), m.gdiDev.c_str(),
                 m.controllable ? 1 : 0, m.pct, m.hdr ? 1 : 0, m.sdrwl);
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

static void DrawPower(Graphics& g, float cx, float cy, float r, Color c) {
    // 电源符号：去掉顶部一段的圆环 + 顶部竖线
    Pen p(c, r * 0.16f);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    g.DrawArc(&p, cx - r * 0.62f, cy - r * 0.62f, r * 1.24f, r * 1.24f, -50, 280);
    g.DrawLine(&p, cx, cy - r * 1.05f, cx, cy - r * 0.25f);
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

static void DrawMonitorMenu(Graphics& g, int W, int H);

// 卡片内投影：沿四边的暗角渐变，增强悬浮层次。分层窗口下卡片外全透明，
// 层次只能画在卡片内；四角两方向渐变交叉形成自然暗角。
static void CardInsetShadow(Graphics& g, int W, int H) {
    GraphicsPath edge;
    RoundRect(edge, RectF(0, 0, (float)W, (float)H), 40.f);
    Region rgn(&edge);
    g.SetClip(&rgn);
    BYTE a = g_dark ? 55 : 40;
    LinearGradientBrush t(PointF(0, 0), PointF(0, 14.f), Color(a, 10, 14, 24), Color(0, 10, 14, 24));
    g.FillRectangle(&t, 0.f, 0.f, (float)W, 14.f);
    LinearGradientBrush b(PointF(0, (float)H), PointF(0, (float)H - 14), Color(a, 10, 14, 24), Color(0, 10, 14, 24));
    g.FillRectangle(&b, 0.f, (float)H - 14.f, (float)W, 14.f);
    LinearGradientBrush l(PointF(0, 0), PointF(14, 0), Color(a, 10, 14, 24), Color(0, 10, 14, 24));
    g.FillRectangle(&l, 0.f, 0.f, 14.f, (float)H);
    LinearGradientBrush r(PointF((float)W, 0), PointF((float)W - 14, 0), Color(a, 10, 14, 24), Color(0, 10, 14, 24));
    g.FillRectangle(&r, (float)W - 14.f, 0.f, 14.f, (float)H);
    g.ResetClip();
}

static void DrawPanel(Graphics& g, int W, int H) {
    LinearGradientBrush bg(PointF(0, 0), PointF((float)W, (float)H), g_pal.bg1, g_pal.bg2);
    GraphicsPath card;
    RoundRect(card, RectF(0, 0, (float)W, (float)H), 40.f);
    g.FillPath(&bg, &card);
    Pen border(g_pal.border, 1.f);
    g.DrawPath(&border, &card);
    CardInsetShadow(g, W, H);

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
    else {
        // 「全部显示器」按真实活动路径判定：HDR 切换瞬间某屏 DDC 读失败被 Scan 丢掉也不误判为 SDR
        hdrNow = g_hdrAny.load();
        for (auto& mm : g_mons) if (mm.hdr) hdrNow = true;
    }

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
    Font fp(L"Microsoft YaHei UI", FS(14.f), FontStyleRegular, UnitPixel);
    g.DrawString(shown.c_str(), -1, &fp,
                 RectF(pill.X, pill.Y, pill.Width - 22, pill.Height), &sf, &tb);

    // 悬停显示完整名称
    if (g_hover == 0 && full.size() > 14) {
        Font ft(L"Microsoft YaHei UI", FS(12.f), FontStyleRegular, UnitPixel);
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
        Font fh2(L"Consolas", FS(12.f), FontStyleBold, UnitPixel);
        SolidBrush ctb(g_pal.prog);
        g.DrawString(L"HDR", -1, &fh2, chip, &sf, &ctb);
    }

    // 提示：HDR 激活时显示 Windows 白电平（滑条镜像）与 lumen 调光比例，两数语义分明
    SolidBrush hb(g_pal.hint);
    Font fh(L"Microsoft YaHei UI", FS(10.f), FontStyleRegular, UnitPixel);
    if (hdrNow) {
        wchar_t hb2[96];
        ULONG wl = 0;
        if (g_sel >= 0 && g_sel < (int)g_mons.size()) wl = g_mons[g_sel].sdrwl;
        else {
            for (auto& mm : g_mons) if (mm.hdr && mm.sdrwl) { wl = mm.sdrwl; break; }
            if (!wl) wl = g_hdrAnyWl.load();   // 屏被丢弃时仍显示真实白电平，不再显示 0
        }
        // 标定值只作参考刻度：显示「当前白电平 / 标定最大白电平」，不再参与亮度换算
        if (g_wlMax >= 1500)
            swprintf(hb2, 96, L"Windows 白电平 %lu/%d · lumen 调光 %d%%", wl, g_wlMax, g_val);
        else
            swprintf(hb2, 96, L"Windows 白电平 %lu · lumen 调光 %d%%", wl, g_val);
        g.DrawString(hb2, -1, &fh, RectF(0, HINT_Y, (float)W, 16), &sf, &hb);
    } else {
        g.DrawString(tr(S_HINT), -1, &fh, RectF(0, HINT_Y, (float)W, 16), &sf, &hb);
    }

    // 四个圆形快捷按钮：凸起拟物（落影 + 双向倒角），激活项图标琥珀/深蓝
    for (int i = 0; i < 4; ++i) {
        float cx = W / 2.f + (i - 1.5f) * BTN_DX;
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
        else if (i == 2) DrawGear(g, cx, BTN_CY, 10.5f, ic);
        else DrawPower(g, cx, BTN_CY, 10.5f, ic);
    }

    if (g_fail) {
        SolidBrush rb(g_pal.danger);
        Font fr(L"Microsoft YaHei UI", FS(9.f), FontStyleRegular, UnitPixel);
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
    int w = (int)(Scale(PANEL_W) * UI_SCALE), h = (int)(Scale(PageH()) * UI_SCALE), m = Scale(11);
    int x, y;
    if (g_moved) {
        // 用户拖过面板：切页时保持左上角，只在越出工作区时收回
        x = g_movedPos.x;
        y = g_movedPos.y;
        if (x + w > wa.right - m) x = wa.right - w - m;
        if (y + h > wa.bottom - m) y = wa.bottom - h - m;
    } else {
        x = wa.right - w - m;
        y = wa.bottom - h - m;
    }
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    // 分层窗口呈现（PresentPanel），无需 region 裁剪；圆角由 alpha 抗锯齿
    if (g_moved) { g_movedPos.x = x; g_movedPos.y = y; }   // 收回后同步，连续切页不漂移
}

static void PresentPanel(HWND hwnd);   // 定义在下方
static void DrawSubPageCard(Graphics& g, int W, int H);
static void DrawSettingsChrome(Graphics& g, int W, int H);
static void DrawSettingsContent(Graphics& g, int W, int H);
static void DrawSettingsStatus(Graphics& g, int W, int H);
static void DrawCalibChrome(Graphics& g, int W, int H);
static void DrawCalibContent(Graphics& g, int W, int H);
static void DrawScrollbar(Graphics& g, int W, int H, int contentEnd);
static int  SubScrollMax();
static void DoCalWL();        // 标定 HDR 基准亮度（托盘/设置页共用）
static void DoCalWheel();     // 校准滚轮图标位置
static void DoGammaToggle();  // 软件调光开关

static void OnPanelPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);
    EndPaint(hwnd, &ps);      // ULW 模式：WM_PAINT 只负责清无效区，内容由 PresentPanel 呈现
    PresentPanel(hwnd);
}

// 面板呈现：GDI+ 绘制进 PARGB 缓冲（premultiplied alpha），经 UpdateLayeredWindow 整窗呈现。
// 圆角边缘走 alpha 渐变，彻底替代 SetWindowRgn 裁剪（region 无抗锯齿，四角有锯齿）。
static void PresentPanel(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    float k = g_dpi / 96.0f * UI_SCALE;
    int Wl = (int)lroundf(w / k), Hl = (int)lroundf(h / k);

    if (!g_presBmp || g_presW != w || g_presH != h) {
        delete g_presBmp; g_presBmp = nullptr;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;              // top-down 与 GDI+ 行序一致
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC hdcScreen = GetDC(hwnd);
        HDC mem = CreateCompatibleDC(hdcScreen);
        HBITMAP dib = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(hwnd, hdcScreen);
        if (!mem || !dib) { if (mem) DeleteDC(mem); if (dib) DeleteObject(dib); return; }
        if (g_presDC) { if (g_presDIB) DeleteObject(g_presDIB); DeleteDC(g_presDC); }
        g_presDC = mem;
        g_presDIB = dib;
        SelectObject(g_presDC, g_presDIB);
        g_presBmp = new Bitmap(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        g_presW = w; g_presH = h;
    }
    Graphics g(g_presBmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.ScaleTransform(k, k);

    if (g_page == 0) {
        DrawPanel(g, Wl, Hl);                    // 主面板：矢量直画（低频重绘）
    } else {
        int sepL = (g_page == 1) ? 96 : 102;
        int cntHl = (g_page == 1) ? SettingsPageH() : 160 + (int)g_mons.size() * 52;
        int cntH = (int)(cntHl * k);

        // 滚动收敛：内容变化（折叠/显示器增减）后越界部分回到可视范围
        int maxS = SubScrollMax();
        if (g_subScroll > maxS) g_subScroll = maxS;
        if (g_subScroll < 0) g_subScroll = 0;

        if (g_pageCacheDirty || g_chromePage != g_page || !g_chromeBmp || g_chromeW != w || g_chromeH != h) {
            delete g_chromeBmp;
            g_chromeBmp = new Bitmap(w, h, PixelFormat32bppPARGB);
            Graphics cg(g_chromeBmp);
            cg.SetSmoothingMode(SmoothingModeAntiAlias);
            cg.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            cg.ScaleTransform(k, k);
            DrawSubPageCard(cg, Wl, Hl);
            if (g_page == 1) DrawSettingsChrome(cg, Wl, Hl); else DrawCalibChrome(cg, Wl, Hl);
            g_chromeW = w; g_chromeH = h; g_chromePage = g_page;
        }
        if (g_pageCacheDirty || g_cntPage != g_page || !g_cntBmp || g_cntW != w || g_cntH != cntH) {
            delete g_cntBmp;
            g_cntBmp = new Bitmap(w, cntH > 0 ? cntH : 1, PixelFormat32bppPARGB);
            Graphics cn(g_cntBmp);
            cn.SetSmoothingMode(SmoothingModeAntiAlias);
            cn.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            cn.ScaleTransform(k, k);
            // 内容按逻辑坐标直画（位图 y = 逻辑 y × k），不预平移——滚动偏移全部由 blit 源坐标承担。
            // 之前在平移前 SetClip 的做法会裁掉位图顶部 82px（clip 固定在设备坐标，不随 translate 移动），
            // 导致前两行控件和组标题消失（issue 反馈的"遮挡/选项缺失"）。
            if (g_page == 1) DrawSettingsContent(cn, Wl, Hl); else DrawCalibContent(cn, Wl, Hl);
            g_cntW = w; g_cntH = cntH; g_cntPage = g_page;
            g_pageCacheDirty = false;    // 重建完成：不清零的话每帧都整页重绘，缓存优化失效
        }

        // DrawImage 的目标矩形在缩放后的逻辑坐标系里（合成目标已 ScaleTransform(k)），
        // 用物理像素尺寸会缩小成 k 倍 → 露出下层残留帧。源矩形保持位图像素（UnitPixel）。
        g.DrawImage(g_chromeBmp, RectF(0.f, 0.f, (float)Wl, (float)Hl));
        // content 位图内坐标 = (逻辑Y − sepL) × k；屏幕可见区从逻辑 98 起、状态行上方 44px 止
        float clipTopL = 98.f;
        float clipHl = (float)Hl - clipTopL - 44.f;
        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
        // 源宽度必须是位图物理宽 w：此前误用 Wl（400 逻辑）→ 源 400px 映到目标 344px，
        // 内容层整体多压一次 k（k²），绘制位置比命中区左偏 1/k，控件看着偏左、点击没反应
        g.DrawImage(g_cntBmp, RectF(0.f, clipTopL, (float)Wl, clipHl),
                    RectF(0.f, (clipTopL + g_subScroll) * k, (float)w, clipHl * k), UnitPixel);
        if (g_page == 1) DrawSettingsStatus(g, Wl, Hl);
        DrawScrollbar(g, Wl, Hl, (g_page == 1) ? SettingsPageH() - 56
                                               : 160 + (int)g_mons.size() * 52);
    }

    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    POINT src{ 0, 0 };
    SIZE size{ w, h };
    HDC hdcScreen = GetDC(hwnd);
    UpdateLayeredWindow(hwnd, hdcScreen, nullptr, &size, g_presDC, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(hwnd, hdcScreen);
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

    Font fsub(L"Microsoft YaHei UI", FS(12.f), FontStyleRegular, UnitPixel);
    SolidBrush sb(g_pal.osdSub);
    if (g_osdMode[0]) {
        Font fm(L"Microsoft YaHei UI", FS(20.f), FontStyleBold, UnitPixel);
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
    int x, y;
    if (g_osdPos.x >= 0 && g_osdPos.y >= 0) {
        // 拖动记忆的位置，clamp 进当前工作区：拔屏/改分辨率后不会留在外面
        x = std::max((int)wa.left,  std::min((int)g_osdPos.x, (int)wa.right - w));
        y = std::max((int)wa.top,   std::min((int)g_osdPos.y, (int)wa.bottom - h));
    } else {
        x = wa.right - w - m;
        y = wa.top + m;
    }
    if (!g_osdDrag) {                     // 拖动中不重定位、不重设定时器（避免拖到一半被隐藏/复位）
        SetWindowPos(g_osd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
        BindRound(g_osd, w, h, 18);
    }
    ShowWindow(g_osd, SW_SHOWNOACTIVATE);
    InvalidateRect(g_osd, nullptr, FALSE);
    if (!g_osdDrag) SetTimer(g_osd, 1, 1400, nullptr);
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

// ---- 设置页几何：折叠分组布局，绘制与命中判定共用同一份计算，避免两处漂移
static const float SET_L = 36.f, SET_R = 374.f;   // 右缘贴近滚动条（391），不再留大空档
static const float SET_HH    = 34.f;   // 分组标题行高
static const float SET_ROW   = 52.f;   // 通用 / 鼠标与键盘 / 关于 行高
static const float SET_HKROW = 44.f;   // 快捷键行高

struct SetLay {
    float hdrY[4];    // 各组标题 Y（组 1..3 的分隔线画在 hdrY-12）
    float bodyY[4];   // 展开时 body 首行 Y
    float total;      // 页面总高
};

static SetLay g_setLay;
static void RefreshSetLayout() {
    static const float bodyH[4] = { 9 * SET_ROW, 3 * SET_HKROW, 4 * SET_ROW, 2 * SET_ROW };
    float y = 104.f;                       // 首组标题（页首分隔线 y=96 下方）
    for (int i = 0; i < 4; ++i) {
        if (i > 0) y += 8.f;               // 组间距（容纳上方分隔线）
        g_setLay.hdrY[i] = y;
        y += SET_HH;
        g_setLay.bodyY[i] = y;
        if (g_setFold & (1 << i)) y += bodyH[i];
    }
    g_setLay.total = y + 56.f;             // body 底 + 状态行(40) + 底边距
}

static int SettingsPageH() { RefreshSetLayout(); return (int)g_setLay.total; }   // 内容总高（含底部留白 56）

static RectF SetHeaderRect(int i) {
    // 高度取 SET_HH（不到 bodyY）：标题带与展开后首行控件之间不留重叠，命中判定优先级是标题在前
    return RectF(SET_L - 8.f, g_setLay.hdrY[i] - 5.f, SET_R - SET_L + 16.f, SET_HH);
}

// 行坐标：全部从当前折叠布局取 Y，绘制与命中共用
static float SetRowY(int i)      { return g_setLay.bodyY[0] + i * SET_ROW; }
static float SetHkY(int i)       { return g_setLay.bodyY[1] + i * SET_HKROW; }
static float SetMouseRowY(int i) { return g_setLay.bodyY[2] + i * SET_ROW; }
static float SetAboutRowY()      { return g_setLay.bodyY[3]; }

static RectF  SetToggleRect(int i)    { return RectF(SET_R - 44.f, SetRowY(i) - 4.f, 44.f, 24.f); }
// 第 2/3 行双列：左列 时间，右列 亮度，各自带加减（值矩形按 120% 字体不折行留足宽度）
static RectF  SetTimeValueRect(int i) { return RectF(96.f,  SetRowY(i) - 6.f, 48.f, 28.f); }
static RectF  SetTimeMinusRect(int i) { return RectF(146.f, SetRowY(i) - 6.f, 30.f, 30.f); }
static RectF  SetTimePlusRect(int i)  { return RectF(178.f, SetRowY(i) - 6.f, 30.f, 30.f); }
static RectF  SetPctValueRect(int i)  { return RectF(268.f, SetRowY(i) - 6.f, 42.f, 28.f); }
static RectF  SetPctMinusRect(int i)  { return RectF(312.f, SetRowY(i) - 6.f, 30.f, 30.f); }
static RectF  SetPctPlusRect(int i)   { return RectF(344.f, SetRowY(i) - 6.f, 30.f, 30.f); }
// 第 4 行左右各半：左 DDC 间隔，右 步长
static RectF  SetDdcValue()           { return RectF(114.f, SetRowY(4) - 6.f, 54.f, 28.f); }
static RectF  SetDdcMinus()           { return RectF(170.f, SetRowY(4) - 6.f, 30.f, 30.f); }
static RectF  SetDdcPlus()            { return RectF(200.f, SetRowY(4) - 6.f, 30.f, 30.f); }
static RectF  SetStepValue()          { return RectF(282.f, SetRowY(4) - 6.f, 26.f, 28.f); }
static RectF  SetStepMinus()          { return RectF(312.f, SetRowY(4) - 6.f, 30.f, 30.f); }
static RectF  SetStepPlus()           { return RectF(344.f, SetRowY(4) - 6.f, 30.f, 30.f); }
static RectF  SetEditRect(int i)      { return RectF(SET_R - 72.f, SetHkY(i), 72.f, 30.f); }
static RectF  SetKeyRect(int i)       { return RectF(96.f, SetHkY(i), 190.f, 30.f); }
static RectF  SetBackRect()           { return RectF(16.f, 8.f, 120.f, 44.f); }
static RectF  SetMouseToggleRect(int i) { return RectF(SET_R - 44.f, SetMouseRowY(i) - 4.f, 44.f, 24.f); }
static RectF  SetHomeRect()           { return RectF(SET_L - 8.f, SetAboutRowY() - 6.f, SET_R - SET_L + 16.f, 34.f); }
// 通用组第 6/7 行（新增）：语言整行点击循环切换；字体大小带加减
static RectF  SetLangRect()           { return RectF(SET_L - 8.f, SetRowY(5) - 6.f, SET_R - SET_L + 16.f, 34.f); }
static RectF  SetLangValueRect()      { return RectF(SET_R - 130.f, SetRowY(5) - 2.f, 130.f, 28.f); }
static RectF  SetFontValueRect()      { return RectF(260.f, SetRowY(6) - 6.f, 50.f, 28.f); }
static RectF  SetFontMinusRect()      { return RectF(312.f, SetRowY(6) - 6.f, 30.f, 30.f); }
static RectF  SetFontPlusRect()       { return RectF(344.f, SetRowY(6) - 6.f, 30.f, 30.f); }
// 自托盘菜单迁入的功能行：软件调光(行7 开关) / 标定 HDR(行8 整行) / 校准滚轮(鼠标组行3) / 复制诊断(关于组行1)
static RectF  SetCalWLRect()          { return RectF(SET_L - 8.f, SetRowY(8) - 6.f, SET_R - SET_L + 16.f, 34.f); }
static RectF  SetCalWheelRect()       { return RectF(SET_L - 8.f, SetMouseRowY(3) - 6.f, SET_R - SET_L + 16.f, 34.f); }
static RectF  SetDiagRect()           { return RectF(SET_L - 8.f, SetAboutRowY() + SET_ROW - 6.f, SET_R - SET_L + 16.f, 34.f); }

// 子页滚动上限：内容底部 + 8px 余量 − 可视区底部（状态行上方留 48px）；页高固定 570
static int SubScrollMax() {
    int end = (g_page == 2) ? (160 + (int)g_mons.size() * 52)
                            : SettingsPageH() - 56;
    int maxS = end + 8 - (PANEL_H - 48);
    return maxS > 0 ? maxS : 0;
}

// 滚轮缓动：目标位置 100ms 内到位（timer 8 @16ms，每帧走剩余的 ~1/4），滚动帧只做位图搬运
static void ScrollTo(int ny) {
    int maxS = SubScrollMax();
    if (ny > maxS) ny = maxS;
    if (ny < 0) ny = 0;
    if (ny == g_subScroll) return;
    g_scrollTo = ny;
    if (!g_scrollAnim && g_msg) { g_scrollAnim = true; SetTimer(g_msg, 8, 16, nullptr); }
}

static void ScrollAnimTick() {
    if (!g_scrollAnim) return;
    int d = g_scrollTo - g_subScroll;
    if (d == 0) { g_scrollAnim = false; KillTimer(g_msg, 8); return; }
    int step = d / 4;
    if (step == 0) step = (d > 0) ? 1 : -1;
    g_subScroll += step;
    if (g_subScroll == g_scrollTo) { g_scrollAnim = false; KillTimer(g_msg, 8); }
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 滚动条几何：绘制与拖动共用（thumb 中心跟随按点，按住可拖）
static void ScrollbarGeom(int contentEnd, float& trackY0, float& viewH, float& thumbH, int& maxS) {
    trackY0 = 102.f;
    float trackY1 = (float)PANEL_H - 46.f;
    viewH = trackY1 - trackY0;
    float contentH = (float)contentEnd + 8.f;
    thumbH = viewH * viewH / contentH;
    if (thumbH < 28.f) thumbH = 28.f;
    maxS = SubScrollMax();
}

// 拖动/点按位置 → 滚动偏移：thumb 中心对齐按点
static int ScrollFromDragY(int y, int contentEnd) {
    float trackY0, viewH, thumbH; int maxS;
    ScrollbarGeom(contentEnd, trackY0, viewH, thumbH, maxS);
    if (viewH - thumbH <= 0 || maxS <= 0) return 0;
    float rel = ((float)y - trackY0 - thumbH / 2.f) / (viewH - thumbH);
    int ny = (int)(rel * maxS + 0.5f);
    if (ny < 0) ny = 0;
    if (ny > maxS) ny = maxS;
    return ny;
}

static void DrawScrollbar(Graphics& g, int W, int H, int contentEnd) {
    float trackY0, viewH, thumbH; int maxS;
    ScrollbarGeom(contentEnd, trackY0, viewH, thumbH, maxS);
    if (maxS <= 0) return;
    float pos = trackY0 + (viewH - thumbH) * (float)g_subScroll / (float)maxS;
    GraphicsPath trk;
    RoundRect(trk, RectF((float)W - 9.f, trackY0, 4.f, viewH), 2.f);
    SolidBrush tb(g_pal.pillEdge);
    g.FillPath(&tb, &trk);
    GraphicsPath th;
    RoundRect(th, RectF((float)W - 9.f, pos, 4.f, thumbH), 2.f);
    SolidBrush hb(g_pal.sub);
    g.FillPath(&hb, &th);
}

static bool InRect(const RectF& r, int x, int y) {
    return x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height;
}

// 分组标题右侧的展开指示：展开=向下 V，收起=向右 >
static void DrawChevron(Graphics& g, float cx, float cy, bool open, Color c) {
    Pen p(c, 2.f);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    if (open) {
        g.DrawLine(&p, cx - 5.f, cy - 2.f, cx, cy + 3.f);
        g.DrawLine(&p, cx, cy + 3.f, cx + 5.f, cy - 2.f);
    } else {
        g.DrawLine(&p, cx - 2.f, cy - 5.f, cx + 3.f, cy);
        g.DrawLine(&p, cx + 3.f, cy, cx - 2.f, cy + 5.f);
    }
}

// 外链箭头 ↗（「项目主页」行尾）
static void DrawExtArrow(Graphics& g, float cx, float cy, Color c) {
    Pen p(c, 2.f);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    g.DrawLine(&p, cx - 5.f, cy + 5.f, cx + 4.f, cy - 4.f);
    g.DrawLine(&p, cx, cy - 5.f, cx + 5.f, cy - 5.f);
    g.DrawLine(&p, cx + 5.f, cy - 5.f, cx + 5.f, cy);
}

// 子页公共卡片：渐变 + 40px 圆角 + 描边 + 内投影（chrome 缓存层，主题/DPI 变化才重建）
static void DrawSubPageCard(Graphics& g, int W, int H) {
    LinearGradientBrush bg(PointF(0, 0), PointF(0, (float)H), g_pal.bg1, g_pal.bg2);
    GraphicsPath card;
    RoundRect(card, RectF(0, 0, (float)W, (float)H), 40.f);
    g.FillPath(&bg, &card);
    Pen border(g_pal.pillEdge, 1.f);
    g.DrawPath(&border, &card);
    CardInsetShadow(g, W, H);
}

// 设置页页眉（固定不滚）：返回 + 标题 + 分隔线
static void DrawSettingsChrome(Graphics& g, int W, int H) {
    SolidBrush tb(g_pal.text);
    SolidBrush sb(g_pal.sub);
    Font fb(L"Microsoft YaHei UI", FS(14.f), FontStyleRegular, UnitPixel);
    Font ft(L"Microsoft YaHei UI", FS(22.f), FontStyleBold, UnitPixel);
    StringFormat cen;
    cen.SetAlignment(StringAlignmentCenter);
    cen.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(tr(S_BACK), -1, &fb, PointF(28, 18), &sb);
    g.DrawString(tr(S_SETTINGS), -1, &ft, RectF(0, 46, (float)W, 32), &cen, &tb);
    Pen sep(g_pal.pillEdge, 1.f);
    g.DrawLine(&sep, SET_L, 96.f, SET_R, 96.f);
}

// 设置页内容层（随滚动平移，绘制进缓存位图）：折叠标题 + 各组 + 关于
static void DrawSettingsContent(Graphics& g, int W, int H) {
    SolidBrush tb(g_pal.text);
    SolidBrush sb(g_pal.sub);
    SolidBrush acc(g_dark ? g_pal.prog : g_pal.progDot);
    Font fl(L"Microsoft YaHei UI", FS(15.f), FontStyleRegular, UnitPixel);
    Font fv(L"Microsoft YaHei UI", FS(15.f), FontStyleBold, UnitPixel);
    Font fs(L"Microsoft YaHei UI", FS(12.f), FontStyleRegular, UnitPixel);
    Font fsec(L"Microsoft YaHei UI", FS(12.f), FontStyleRegular, UnitPixel);
    StringFormat cen;
    cen.SetAlignment(StringAlignmentCenter);
    cen.SetLineAlignment(StringAlignmentCenter);
    StringFormat rgt;
    rgt.SetAlignment(StringAlignmentFar);
    rgt.SetLineAlignment(StringAlignmentCenter);

    RefreshSetLayout();
    Pen sep(g_pal.pillEdge, 1.f);
    // 分组标题：通用 / 全局快捷键 / 鼠标与键盘 / 关于；右侧矢量 chevron 指示展开态
    static const int setTitles[4] = { S_GENERAL, S_HOTKEYS, S_MOUSESET, S_ABOUT };
    for (int gi = 0; gi < 4; ++gi) {
        if (gi > 0) g.DrawLine(&sep, SET_L, g_setLay.hdrY[gi] - 12.f, SET_R, g_setLay.hdrY[gi] - 12.f);
        if (g_setHeaderHover == gi) {
            GraphicsPath hp;
            RoundRect(hp, SetHeaderRect(gi), 11.f);
            SolidBrush hb(g_pal.menuHover);
            g.FillPath(&hb, &hp);
        }
        g.DrawString(tr(setTitles[gi]), -1, &fsec, PointF(SET_L, g_setLay.hdrY[gi]), &tb);
        DrawChevron(g, SET_R - 12.f, g_setLay.hdrY[gi] + 15.f, (g_setFold & (1 << gi)) != 0, g_pal.sub);
    }

    wchar_t buf[32];
    if (g_setFold & 1) {   // 通用
        g.DrawString(tr(S_AUTOSTART), -1, &fl, PointF(SET_L, SetRowY(0)), &tb);
        g.DrawString(tr(S_AUTOBR), -1, &fl, PointF(SET_L, SetRowY(1)), &tb);

        ToggleTrack(g, SetToggleRect(0).X, SetToggleRect(0).Y, g_cfgStart != 0, g_dark);
        ToggleTrack(g, SetToggleRect(1).X, SetToggleRect(1).Y, g_cfgAuto != 0, g_dark);

        Font fv13(L"Microsoft YaHei UI", FS(13.f), FontStyleBold, UnitPixel);
        Font fv12(L"Microsoft YaHei UI", FS(12.f), FontStyleBold, UnitPixel);   // 双列数值：12px 才放得下 "08:00" 不折行
        for (int i = 2; i <= 3; ++i) {
            // 左列：时间；右列：到点后的目标亮度，两列同排
            g.DrawString(i == 2 ? tr(S_STARTH) : tr(S_ENDH), -1, &fl, PointF(SET_L, SetRowY(i)), &sb);
            g.DrawString(i == 2 ? tr(S_DAYBR) : tr(S_NIGHTBR), -1, &fs, PointF(216.f, SetRowY(i) + 2), &sb);

            swprintf(buf, 32, L"%02d:00", i == 2 ? g_cfgStartH : g_cfgEndH);
            g.DrawString(buf, -1, &fv12, SetTimeValueRect(i), &rgt, &tb);
            MiniButton(g, SetTimeMinusRect(i).X, SetTimeMinusRect(i).Y, false);
            MiniButton(g, SetTimePlusRect(i).X, SetTimePlusRect(i).Y, true);

            swprintf(buf, 32, L"%d%%", i == 2 ? g_dayPct : g_nightPct);
            g.DrawString(buf, -1, &fv12, SetPctValueRect(i), &rgt, &tb);
            MiniButton(g, SetPctMinusRect(i).X, SetPctMinusRect(i).Y, false);
            MiniButton(g, SetPctPlusRect(i).X, SetPctPlusRect(i).Y, true);
        }
        // 第 4 行：左 DDC 间隔 / 右 步长
        g.DrawString(tr(S_DDC), -1, &fl, PointF(SET_L, SetRowY(4)), &tb);
        swprintf(buf, 32, L"%d ms", g_cfgDelay);
        g.DrawString(buf, -1, &fv13, SetDdcValue(), &rgt, &tb);
        RectF dm = SetDdcMinus(), dp = SetDdcPlus();
        MiniButton(g, dm.X, dm.Y, false);
        MiniButton(g, dp.X, dp.Y, true);
        g.DrawString(tr(S_STEP), -1, &fl, PointF(250.f, SetRowY(4)), &tb);
        swprintf(buf, 32, L"%d", g_step);
        g.DrawString(buf, -1, &fv13, SetStepValue(), &rgt, &tb);
        RectF sm = SetStepMinus(), sp = SetStepPlus();
        MiniButton(g, sm.X, sm.Y, false);
        MiniButton(g, sp.X, sp.Y, true);

        // 第 6 行：语言（点击整行循环 跟随系统 → 中文 → English）
        g.DrawString(tr(S_LANG), -1, &fl, PointF(SET_L, SetRowY(5)), &tb);
        const wchar_t* lv = (g_langPref == 0) ? tr(S_FOLL) : (g_langPref == 1) ? L"中文" : L"English";
        g.DrawString(lv, -1, &fv, SetLangValueRect(), &rgt, &tb);

        // 第 7 行：字体大小（90–120%，步进 10%）
        g.DrawString(tr(S_FONTS), -1, &fl, PointF(SET_L, SetRowY(6)), &tb);
        swprintf(buf, 32, L"%d%%", g_fontPct);
        g.DrawString(buf, -1, &fv12, SetFontValueRect(), &rgt, &tb);
        RectF fm = SetFontMinusRect(), fp2 = SetFontPlusRect();
        MiniButton(g, fm.X, fm.Y, false);
        MiniButton(g, fp2.X, fp2.Y, true);

        // 第 8 行：软件调光（HDR 显示器用，自托盘迁入）
        g.DrawString(tr(S_GAMMA), -1, &fl, PointF(SET_L, SetRowY(7)), &tb);
        ToggleTrack(g, SetToggleRect(7).X, SetToggleRect(7).Y, g_gammaDim != 0, g_dark);

        // 第 9 行：标定 HDR 基准亮度（整行点击，气泡报结果）
        g.DrawString(tr(S_CAL), -1, &fl, PointF(SET_L, SetRowY(8)), &tb);
    }

    if (g_setFold & 2) {   // 全局快捷键
        for (int i = 0; i < 3; ++i) {
            float y = SetHkY(i);
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
    }

    if (g_setFold & 4) {   // 鼠标与键盘：三个开关（与页首开关同款绘制与命中矩形）+ 校准滚轮图标
        g.DrawString(tr(S_WHEELTRAY), -1, &fl, PointF(SET_L, SetMouseRowY(0)), &tb);
        g.DrawString(tr(S_WHEELPANEL), -1, &fl, PointF(SET_L, SetMouseRowY(1)), &tb);
        ToggleTrack(g, SetMouseToggleRect(0).X, SetMouseToggleRect(0).Y, g_wheelTray != 0, g_dark);
        ToggleTrack(g, SetMouseToggleRect(1).X, SetMouseToggleRect(1).Y, g_wheelPanel != 0, g_dark);
        g.DrawString(tr(S_KEYBRIGHT), -1, &fl, PointF(SET_L, SetMouseRowY(2)), &tb);
        ToggleTrack(g, SetMouseToggleRect(2).X, SetMouseToggleRect(2).Y, g_keyBright != 0, g_dark);
        g.DrawString(tr(S_CALIB), -1, &fl, PointF(SET_L, SetMouseRowY(3)), &tb);   // 整行点击，自托盘迁入
    }

    if (g_setFold & 8) {   // 关于：项目主页（点击打开浏览器）+ 复制诊断信息（自托盘迁入）
        float hy = SetAboutRowY();
        Color lc = g_dark ? g_pal.prog : g_pal.progDot;
        SolidBrush lb(lc);
        g.DrawString(tr(S_HOME), -1, &fl, PointF(SET_L, hy), &lb);
        RectF hm;
        g.MeasureString(tr(S_HOME), -1, &fl, PointF(SET_L, hy), &hm);
        Pen ulink(lc, 1.f);
        g.DrawLine(&ulink, SET_L, hy + 21.f, SET_L + hm.Width, hy + 21.f);
        DrawExtArrow(g, SET_L + hm.Width + 13.f, hy + 15.f, g_pal.sub);
        g.DrawString(tr(S_DIAG), -1, &fl, PointF(SET_L, SetAboutRowY() + SET_ROW), &tb);
    }
}

// 设置页状态行（固定层，每帧轻量矢量）：签名 / 热键错误红字 / 钩子失败提示 + 右下角版本号
static void DrawSettingsStatus(Graphics& g, int W, int H) {
    SolidBrush sb(g_pal.sub);
    Font fs(L"Microsoft YaHei UI", FS(12.f), FontStyleRegular, UnitPixel);
    SolidBrush eb(g_pal.danger);
    SolidBrush* msgBrush = &sb;
    const wchar_t* msg = tr(S_SIG);
    if (g_hkMsg == 1) {
        msg = tr(S_OCCUPIED); msgBrush = &eb;
    } else if (g_hkMsg == 2) {
        msg = tr(S_NEEDMOD); msgBrush = &eb;
    } else if (g_capSlot) {
        msg = tr(S_NEEDMOD);
    } else if (g_hookDead) {
        msg = tr(S_HOOKFAIL); msgBrush = &eb;
    }
    g.DrawString(msg, -1, &fs, PointF(SET_L, H - 40.f), msgBrush);
    StringFormat rgt;
    rgt.SetAlignment(StringAlignmentFar);
    wchar_t vb[32];
    swprintf(vb, 32, L"v%ls", APP_VERSION);
    g.DrawString(vb, -1, &fs, RectF(SET_R - 80.f, H - 40.f, 80.f, 16.f), &rgt, &sb);
}

// ---- 校准页（单屏偏移）：行距同设置页，右对齐到 SET_R；只影响「全部显示器」同步
static float CalRowY(int i)       { return 128.f + i * 52.f; }
static RectF  CalMinusRect(int i) { return RectF(236.f, CalRowY(i) - 4.f, 30.f, 30.f); }
static RectF  CalPlusRect(int i)  { return RectF(344.f, CalRowY(i) - 4.f, 30.f, 30.f); }
static RectF  CalResetRect()      { return RectF(SET_L, CalRowY((int)g_mons.size()) + 2.f, 130.f, 30.f); }

// 校准页页眉（固定不滚）：返回 + 标题 + 提示 + 分隔线
static void DrawCalibChrome(Graphics& g, int W, int H) {
    SolidBrush tb(g_pal.text);
    SolidBrush sb(g_pal.sub);
    Font ft(L"Microsoft YaHei UI", FS(22.f), FontStyleBold, UnitPixel);
    Font fs(L"Microsoft YaHei UI", FS(12.f), FontStyleRegular, UnitPixel);
    StringFormat cen;
    cen.SetAlignment(StringAlignmentCenter);
    cen.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(tr(S_BACK), -1, &fs, PointF(28, 18), &sb);
    g.DrawString(tr(S_OFFPG), -1, &ft, RectF(0, 46, (float)W, 32), &cen, &tb);
    g.DrawString(tr(S_OFFHINT), -1, &fs, RectF(0, 80, (float)W, 16), &cen, &sb);
    Pen sep(g_pal.pillEdge, 1.f);
    g.DrawLine(&sep, SET_L, 102.f, SET_R, 102.f);
}

// 校准页内容层（随滚动平移，绘制进缓存位图）：逐屏偏移行 + 归零按钮
static void DrawCalibContent(Graphics& g, int W, int H) {
    SolidBrush tb(g_pal.text);
    SolidBrush sb(g_pal.sub);
    SolidBrush acc(g_dark ? g_pal.prog : g_pal.progDot);
    Font fb(L"Microsoft YaHei UI", FS(14.f), FontStyleRegular, UnitPixel);
    Font fs(L"Microsoft YaHei UI", FS(12.f), FontStyleRegular, UnitPixel);
    Font fv(L"Microsoft YaHei UI", FS(15.f), FontStyleBold, UnitPixel);

    StringFormat lft;
    lft.SetLineAlignment(StringAlignmentCenter);
    lft.SetFormatFlags(StringFormatFlagsNoWrap);
    lft.SetTrimming(StringTrimmingEllipsisCharacter);
    StringFormat rgt;
    rgt.SetAlignment(StringAlignmentFar);
    rgt.SetLineAlignment(StringAlignmentCenter);
    StringFormat cen;
    cen.SetAlignment(StringAlignmentCenter);
    cen.SetLineAlignment(StringAlignmentCenter);
    Pen border(g_pal.pillEdge, 1.f);

    for (int i = 0; i < (int)g_mons.size(); ++i) {
        float y = CalRowY(i);
        std::wstring name = g_mons[i].name;
        if (name.size() > 14) name = name.substr(0, 13) + L"…";
        RectF nr(SET_L, y - 4.f, 192.f, 30.f);
        g.DrawString(name.c_str(), -1, &fb, nr, &lft, &tb);

        MiniButton(g, CalMinusRect(i).X, CalMinusRect(i).Y, false);
        MiniButton(g, CalPlusRect(i).X, CalPlusRect(i).Y, true);

        wchar_t vb[12];
        int off = g_mons[i].off;
        if (off > 0) swprintf(vb, 12, L"+%d", off);
        else swprintf(vb, 12, L"%d", off);
        SolidBrush& vbr = off ? acc : sb;
        g.DrawString(vb, -1, &fv, RectF(274.f, y - 4.f, 50.f, 30.f), &rgt, &vbr);
    }

    GraphicsPath rp;
    RoundRect(rp, CalResetRect(), 15.f);
    SolidBrush rb(g_pal.pill);
    g.FillPath(&rb, &rp);
    g.DrawPath(&border, &rp);
    g.DrawString(tr(S_OFFRESET), -1, &fs, CalResetRect(), &cen, &tb);
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
    // g_moved 保留：面板位置跨开关记忆（PanelX/PanelY），下次打开仍在原处
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

#define MENU_HIT_CALIB 900   // 菜单页脚「单屏偏移…」行

static RectF MenuBox(int H) {
    float h = MENU_PAD * 2 + (MenuCount() + 1) * MENU_ITEM_H;   // 末行：单屏偏移入口
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

// 返回 -1 表示点在菜单外，0 = 全部显示器，1..n = 第 n 台，MENU_HIT_CALIB = 偏移校准入口
static int MenuHit(int x, int y, int H) {
    if (!g_menuOpen) return -1;
    if (!InRect(MenuBox(H), x, y)) return -1;
    for (int i = 0; i < MenuCount(); ++i) {
        RectF r = MenuItemRect(i, H);
        if (x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height) return i;
    }
    RectF f = MenuItemRect(MenuCount(), H);
    if (x >= f.X && x <= f.X + f.Width && y >= f.Y && y <= f.Y + f.Height) return MENU_HIT_CALIB;
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
    Font fi(L"Microsoft YaHei UI", FS(13.f), FontStyleRegular, UnitPixel);
    Font fb(L"Microsoft YaHei UI", FS(13.f), FontStyleBold, UnitPixel);

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

    // 页脚：单屏偏移校准入口（多屏观感不一致时逐屏补偿）
    RectF fr = MenuItemRect(MenuCount(), H);
    if (g_menuHover == MENU_HIT_CALIB) {
        GraphicsPath hp;
        RoundRect(hp, fr, 11.f);
        SolidBrush hb(g_pal.menuHover);
        g.FillPath(&hb, &hp);
    }
    Pen sepf(g_pal.pillEdge, 1.f);
    g.DrawLine(&sepf, fr.X + 14, fr.Y + 1, fr.X + fr.Width - 14, fr.Y + 1);
    g.DrawString(tr(S_OFFPG), -1, &fi,
                 RectF(fr.X + 32, fr.Y, fr.Width - 44, fr.Height), &lf, &sb);
    // 右侧箭头
    float ax = fr.X + fr.Width - 26, ay = fr.Y + fr.Height / 2;
    Pen ap(g_pal.sub, 1.8f);
    ap.SetStartCap(LineCapRound);
    ap.SetEndCap(LineCapRound);
    g.DrawLine(&ap, ax - 3, ay - 4, ax + 2, ay);
    g.DrawLine(&ap, ax + 2, ay, ax - 3, ay + 4);
}

static int HitButton(int x, int y) {
    for (int i = 0; i < 4; ++i) {
        float cx = PANEL_W / 2.f + (i - 1.5f) * BTN_DX;
        float dx = x - cx, dy = y - BTN_CY;
        if (dx * dx + dy * dy <= (BTN_R + 4) * (BTN_R + 4)) return i + 1;
    }
    if (x >= PANEL_W / 2 - PILL_W / 2 && x <= PANEL_W / 2 + PILL_W / 2 &&
        y >= PILL_Y && y <= PILL_Y + PILL_H) return 0;
    return -1;
}

static void ApplyAutoBrightness();   // 定义在后面；开关打开时要立即应用，不等下一次定时器

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
        TouchPageCache();                        // 设置页里的自动开关状态跟着变
        if (g_cfgAuto) ApplyAutoBrightness();
        else g_autoEngaged = false;             // 关闭不打断进行中的过渡，让它滑完；仅停止后续调度
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
    } else if (idx == 3) {
        g_dragging = false;
        g_menuOpen = false;
        if (GetCapture() == g_hwnd) ReleaseCapture();
        g_page = 1;
        g_subScroll = 0;
        PlacePanel();                        // 页高已固定，仅按记忆位置定位
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
    } else if (idx == 4) {
        TurnOffSelectedScreen();             // 关闭选中屏幕（DDC 0xD6 熄屏，任意输入后恢复）
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
        if (g_scrollDrag && g_page >= 1) {   // 滚动条拖动：thumb 跟随（capture 期间移出窗口也更新）
            int contentEnd = (g_page == 1) ? SettingsPageH() - 56
                                           : 160 + (int)g_mons.size() * 52;
            g_subScroll = g_scrollTo = ScrollFromDragY(y, contentEnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_page == 2) return 0;           // 校准页不响应 hover 与轨道交互
        if (g_page == 1) {                   // 设置页：只做分组标题悬停高亮
            int hh = -1;
            int sy = y + g_subScroll;
            RefreshSetLayout();
            for (int gi = 0; gi < 4; ++gi)
                if (InRect(SetHeaderRect(gi), x, sy)) { hh = gi; break; }
            if (hh != g_setHeaderHover) { g_setHeaderHover = hh; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        }
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
        if (g_setHeaderHover != -1) { g_setHeaderHover = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;

    case WM_LBUTTONDOWN: {
        float s = g_dpi / 96.0f * UI_SCALE;
        int x = (int)(GET_X_LPARAM(lp) / s), y = (int)(GET_Y_LPARAM(lp) / s);

        if (g_menuOpen) {
            int hit = MenuHit(x, y, PANEL_H);
            g_menuOpen = false;
            g_menuHover = -1;
            if (hit == MENU_HIT_CALIB) {
                g_page = 2;                  // 进入单屏偏移校准页
                g_subScroll = 0;
                PlacePanel();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (hit >= 0) ApplyMonitorSel(hit - 1);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (g_page >= 1 && x >= PANEL_W - 14 && SubScrollMax() > 0) {   // 滚动条：点按/拖动 thumb 直接跳转（与控件区不重叠）
            int contentEnd = (g_page == 1) ? SettingsPageH() - 56
                                           : 160 + (int)g_mons.size() * 52;
            int ny = ScrollFromDragY(y, contentEnd);
            SetCapture(hwnd);
            g_scrollDrag = true;
            g_scrollAnim = false;
            if (g_msg) KillTimer(g_msg, 8);
            g_scrollTo = g_subScroll = ny;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (g_page == 2) {
            SetFocus(hwnd);
            TouchPageCache();                    // 校准点击（±/归零）改内容
            if (InRect(SetBackRect(), x, y)) {
                g_page = 0;
                g_subScroll = 0;
                PlacePanel();                // 回主面板
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            for (int i = 0; i < (int)g_mons.size(); ++i) {
                int sy = y + g_subScroll;
                bool minus = InRect(CalMinusRect(i), x, sy);
                bool plus  = InRect(CalPlusRect(i), x, sy);
                if (!minus && !plus) continue;
                g_mons[i].off = std::max(-25, std::min(25, g_mons[i].off + (plus ? 1 : -1)));
                RegPut((L"Off_" + g_mons[i].key).c_str(), g_mons[i].off);
                if (g_sel == -1) Post(g_val);   // 「全部显示器」下立即按新偏移重写，所见即所得
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (InRect(CalResetRect(), x, y + g_subScroll)) {
                for (auto& m : g_mons) { m.off = 0; RegPut((L"Off_" + m.key).c_str(), 0); }
                if (g_sel == -1) Post(g_val);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;
        }

        if (g_page == 1) {
            SetFocus(hwnd);
            TouchPageCache();                    // 任何设置点击都可能改内容（开关/数值/录制态）
            if (InRect(SetBackRect(), x, y)) {
                StopCapture();
                g_menuOpen = false;
                g_page = 0;
                g_subScroll = 0;
                g_setHeaderHover = -1;
                PlacePanel();                // 回主面板
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            RefreshSetLayout();
            int sy = y + g_subScroll;    // 内容坐标：随页内滚动平移（返回按钮/状态行固定，用原 y）
            for (int gi = 0; gi < 4; ++gi) {
                if (!InRect(SetHeaderRect(gi), x, sy)) continue;
                g_setFold ^= (1 << gi);      // 折叠/展开分组，状态记忆（页高固定，无需改窗口）
                RegPut(L"SetFold", g_setFold);
                g_setHeaderHover = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // 折叠组的行控件不可见，命中也必须一并失效：否则折叠后下移的其他组
            // （如快捷键的「修改」）会撞上折叠组残留在原坐标的隐形开关/按钮
            if (g_setFold & 1) {                 // 通用
                if (InRect(SetToggleRect(0), x, sy)) {
                    g_cfgStart = g_cfgStart ? 0 : 1;
                    ApplyAutostart(g_cfgStart != 0);
                    RegPut(L"Autostart", g_cfgStart);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (InRect(SetToggleRect(1), x, sy)) {
                    g_cfgAuto = g_cfgAuto ? 0 : 1;
                    RegPut(L"AutoBrightness", g_cfgAuto);
                    if (g_cfgAuto) ApplyAutoBrightness();       // 与快捷按钮一致：打开立即生效（5 秒过渡）
                    else g_autoEngaged = false;                 // 关闭不打断进行中的过渡
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                for (int i = 2; i <= 3; ++i) {
                bool tminus = InRect(SetTimeMinusRect(i), x, sy);
                bool tplus  = InRect(SetTimePlusRect(i), x, sy);
                bool pminus = InRect(SetPctMinusRect(i), x, sy);
                bool pplus  = InRect(SetPctPlusRect(i), x, sy);
                if (!tminus && !tplus && !pminus && !pplus) continue;
                if (tminus || tplus) {
                    if (i == 2) {
                        g_cfgStartH = (g_cfgStartH + (tplus ? 1 : 23)) % 24;
                        if (g_cfgEndH <= g_cfgStartH) g_cfgEndH = std::min(23, g_cfgStartH + 1);
                        RegPut(L"AutoStartH", g_cfgStartH);
                        RegPut(L"AutoEndH", g_cfgEndH);
                    } else {
                        g_cfgEndH = tplus ? std::min(23, g_cfgEndH + 1)
                                          : std::max(g_cfgStartH + 1, g_cfgEndH - 1);
                        RegPut(L"AutoEndH", g_cfgEndH);
                    }
                } else {
                    int v = (i == 2) ? g_dayPct : g_nightPct;
                    v = pplus ? std::min(100, v + 5) : std::max(0, v - 5);
                    if (i == 2) { g_dayPct = v; RegPut(L"DayPct", v); }
                    else        { g_nightPct = v; RegPut(L"NightPct", v); }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
                if (InRect(SetDdcMinus(), x, sy) || InRect(SetDdcPlus(), x, sy)) {
                    bool plus = InRect(SetDdcPlus(), x, sy);
                    g_cfgDelay = plus ? std::min(200, g_cfgDelay + 5)
                                      : std::max(10, g_cfgDelay - 5);
                    RegPut(L"DdcDelay", g_cfgDelay);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (InRect(SetStepMinus(), x, sy) || InRect(SetStepPlus(), x, sy)) {
                    static const int steps[4] = { 1, 2, 5, 10 };
                    int i = 0;
                    while (i < 3 && steps[i] != g_step) ++i;
                    i = InRect(SetStepPlus(), x, sy) ? std::min(3, i + 1) : std::max(0, i - 1);
                    g_step = steps[i];
                    RegPut(L"Step", g_step);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (InRect(SetLangRect(), x, sy)) {          // 语言整行：循环 跟随系统/中文/English
                    g_langPref = (g_langPref + 1) % 3;
                    RegPut(L"Lang", g_langPref);
                    ApplyLang();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (InRect(SetFontMinusRect(), x, sy) || InRect(SetFontPlusRect(), x, sy)) {
                    bool fplus = InRect(SetFontPlusRect(), x, sy);
                    g_fontPct = fplus ? std::min(110, g_fontPct + 10)   // 120% 时标签会压到数值，上限 110
                                      : std::max(90, g_fontPct - 10);
                    RegPut(L"FontPct", g_fontPct);
                    g_fontScale = g_fontPct / 100.0f;
                    TouchPageCache();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (InRect(SetToggleRect(7), x, sy)) {       // 软件调光（自托盘迁入）
                    DoGammaToggle();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (InRect(SetCalWLRect(), x, sy)) {         // 标定 HDR 基准亮度（自托盘迁入）
                    DoCalWL();
                    return 0;
                }
            }                                    // 通用组折叠时以上全部失效
            if (g_setFold & 4) {                 // 鼠标与键盘
                if (InRect(SetCalWheelRect(), x, sy)) {  // 校准滚轮图标位置（自托盘迁入）
                    DoCalWheel();
                    return 0;
                }
                if (InRect(SetMouseToggleRect(0), x, sy)) {
                    g_wheelTray = g_wheelTray ? 0 : 1;
                    RegPut(L"WheelTray", g_wheelTray);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (InRect(SetMouseToggleRect(1), x, sy)) {
                    g_wheelPanel = g_wheelPanel ? 0 : 1;
                    RegPut(L"WheelPanel", g_wheelPanel);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (InRect(SetMouseToggleRect(2), x, sy)) {
                    g_keyBright = g_keyBright ? 0 : 1;
                    RegPut(L"KeyBright", g_keyBright);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            if (g_setFold & 2) {                 // 全局快捷键
                for (int i = 0; i < 3; ++i) {
                    if (InRect(SetEditRect(i), x, sy)) {
                        if (g_capSlot == i + 1) StopCapture();
                        else StartCapture(i + 1);
                        return 0;
                    }
                }
            }
            if (g_setFold & 8) {                 // 关于
                if (InRect(SetDiagRect(), x, sy)) {      // 复制诊断信息（自托盘迁入）
                    CopyDiagnostics();
                    return 0;
                }
                if (InRect(SetHomeRect(), x, sy)) {
                    ShellExecuteW(nullptr, L"open", L"https://github.com/iosvcnm/lumen", nullptr, nullptr, SW_SHOWNOACTIVATE);
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
        if (g_scrollDrag) { g_scrollDrag = false; ReleaseCapture(); return 0; }
        if (g_dragging) { g_dragging = false; ReleaseCapture(); }
        return 0;

    case WM_CAPTURECHANGED:
        g_scrollDrag = false;                // capture 被系统收回（如切页/失焦）时结束拖动
        return 0;

    case WM_MOUSEWHEEL: {
        if (g_page >= 1) {                   // 子页：滚轮滚动页内内容，每档 ±52 逻辑 px（缓动到位）
            if (!g_scrollDrag) {
                int d = GET_WHEEL_DELTA_WPARAM(wp);
                if (d) {
                    int notches = d / WHEEL_DELTA;
                    if (!notches) notches = d > 0 ? 1 : -1;   // 高精度滚轮（delta < 120）也走一行
                    ScrollTo(g_subScroll - notches * 52);
                }
            }
            return 0;
        }
        if (g_capSlot || g_menuOpen || !g_wheelPanel) return 0;
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        int steps = (d / WHEEL_DELTA) * g_step;
        if (!steps) steps = d > 0 ? 1 : -1;   // 高精度滚轮（delta < 120）
        Post(g_val + steps);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:   // Alt 按住时字母键产生 SYSKEYDOWN：与 KEYDOWN 同路处理，否则 Alt 组合录不进
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
            } else if (g_page >= 1) {
                g_page = 0;                  // 设置页/校准页：Esc 回主面板
                g_subScroll = 0;
                PlacePanel();
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

    case WM_EXITSIZEMOVE: {
        RECT r{};
        if (GetWindowRect(hwnd, &r)) {   // 用户拖动结束：记住位置，切页与下次打开沿用
            g_moved = true;
            g_movedPos = POINT{ r.left, r.top };
            RegPut(L"PanelX", (DWORD)r.left);
            RegPut(L"PanelY", (DWORD)r.top);
        }
        return 0;
    }

    case WM_NCHITTEST: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        if (g_page >= 1 && p.x < (int)(Scale(140) * UI_SCALE) &&
            p.y < (int)(Scale(60) * UI_SCALE)) return HTCLIENT;
        if (p.y < (int)(Scale(52) * UI_SCALE)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        TouchPageCache();
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, FALSE);   // 分层窗口：内容随 PresentPanel 重呈现，无需 region
        return 0;
    }
    case WM_DISPLAYCHANGE:
    case WM_POWERBROADCAST:
        g_dispChangeTick = GetTickCount();   // 模式切换窗口：暂停 gamma 写入避开 GDI 挂死
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
    g_nid.uVersion = NOTIFYICON_VERSION;
    if (g_tray) Shell_NotifyIconW(NIM_SETVERSION, &g_nid);

    // 托盘滚轮走 Raw Input（EarTrumpet 同款）：Win11 不向溢出面板图标转发 WM_MOUSEWHEEL，
    // 但原始输入直达本进程，配合图标矩形命中即可，无需 explorer 参与。幂等注册。
    // 同时注册键盘：熄屏状态下按键也能收到，用于唤醒恢复。
    // Consumer Control（0x0C/0x01）：键盘亮度键 0x006F/0x0070，Win11 不写外接屏 DDC，
    // 由 lumen 接管（GitHub issue #2）；其余 consumer 键（音量等）忽略，RIDEV_INPUTSINK 不吞键。
    RAWINPUTDEVICE rid[3]{};
    rid[0].usUsagePage = 0x01;          // generic desktop
    rid[0].usUsage = 0x02;              // mouse
    rid[0].dwFlags = RIDEV_INPUTSINK;   // 无焦点也接收
    rid[0].hwndTarget = g_msg;
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x06;              // keyboard
    rid[1].dwFlags = RIDEV_INPUTSINK;   // 无焦点也接收，不吞键，热键不受影响
    rid[1].hwndTarget = g_msg;
    rid[2].usUsagePage = 0x0C;
    rid[2].usUsage = 0x01;              // consumer control
    rid[2].dwFlags = RIDEV_INPUTSINK;
    rid[2].hwndTarget = g_msg;
    RegisterRawInputDevices(rid, 3, sizeof(RAWINPUTDEVICE));
}

// 图标当前屏幕矩形：固定在任务栏 = 常有；在溢出面板 = 仅面板展开时可见。
// 不可见（rect 空 / 在虚拟屏外）时返回 false，此时滚轮不响应。
static bool IconRect(RECT* rc) {
    NOTIFYICONIDENTIFIER id{};
    id.cbSize = sizeof(id);
    id.hWnd = g_msg;
    id.uID = 1;
    if (Shell_NotifyIconGetRect(&id, rc) != S_OK) return false;
    if (rc->right <= rc->left || rc->bottom <= rc->top) return false;
    RECT virt{
        GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
    RECT tmp;
    return IntersectRect(&tmp, rc, &virt) != FALSE;
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
        g_subScroll = 0;
        g_wantSettings = false;
        if (g_sel >= 0) {
            g_val = g_mons[g_sel].pct;
        } else {
            int sum = 0, n = 0;
            for (auto& m : g_mons) if (m.controllable) { sum += m.pct; ++n; }
            g_val = n ? sum / n : 50;
        }
        g_dpi = 96;
        g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, kPanelClass, L"lumen",
            WS_POPUP, 0, 0, (int)(Scale(PANEL_W) * UI_SCALE), (int)(Scale(PageH()) * UI_SCALE),
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

static DWORD WINAPI CalWLThread(LPVOID);

// 托盘菜单迁入设置页的功能（设置页行点击与托盘共用同一实现）
static void DoCalWL() {
    CreateThread(nullptr, 0, CalWLThread, nullptr, 0, nullptr);   // 6 秒采样，气泡报结果
}
static void DoCalWheel() {
    g_calibrating = true;
    wchar_t info[96];
    swprintf(info, 96, tr(S_CALIBHINT));
    g_nid.uFlags |= NIF_INFO;
    wcsncpy(g_nid.szInfo, info, 255); g_nid.szInfo[255] = 0;
    wcsncpy(g_nid.szInfoTitle, L"lumen", 63); g_nid.szInfoTitle[63] = 0;
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags &= ~NIF_INFO;
}
static void DoGammaToggle() {
    g_gammaDim = g_gammaDim ? 0 : 1;
    RegPut(L"GammaDim", g_gammaDim);
    if (!g_gammaDim) { ResetGammaAll(); OverlayCloseAll(); }   // 关闭即恢复
    else Post(g_val);                        // 开启立即按当前亮度应用
}

static void ShowMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, tr(S_SHOWPANEL));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 5, tr(S_SETTINGS));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, tr(S_EXIT));
    SetForegroundWindow(g_msg);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_msg, nullptr);
    DestroyMenu(m);

    if (cmd == 1) ShowPanel();
    else if (cmd == 5) {
        ShowPanel();
        g_page = 1;
        g_subScroll = 0;
        PlacePanel();
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

// 标定线程：采样 6 秒 SDR 白电平，取最大值（用户应在此期间把 Windows 滑条拖到最右）
static DWORD WINAPI CalWLThread(LPVOID) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) {
        PostMessageW(g_msg, WM_WLCAL, 0, 0);
        return 0;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm,
                           modes.data(), nullptr) != ERROR_SUCCESS || np == 0) {
        PostMessageW(g_msg, WM_WLCAL, 0, 0);
        return 0;
    }
    LUID a = paths[0].targetInfo.adapterId;
    ULONG id = paths[0].targetInfo.id, mn = 0xFFFFFFFF, mx = 0;
    for (int k = 0; k < 40; ++k) {
        struct WLQ { DISPLAYCONFIG_DEVICE_INFO_HEADER header; ULONG SDRWhiteLevel; } w{};
        w.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        w.header.size = sizeof(w);
        w.header.adapterId = a; w.header.id = id;
        if (DisplayConfigGetDeviceInfo(&w.header) == ERROR_SUCCESS && w.SDRWhiteLevel) {
            if (w.SDRWhiteLevel < mn) mn = w.SDRWhiteLevel;
            if (w.SDRWhiteLevel > mx) mx = w.SDRWhiteLevel;
        }
        Sleep(150);
    }
    PostMessageW(g_msg, WM_WLCAL, (WPARAM)(mx > mn + 300 ? mx : 0), 0);
    return 0;
}

// ---- 托盘滚轮：Win11 不再把滚轮投递给 NOTIFYICON 回调（实测 26300），
// 用低级鼠标钩子接管「托盘区」滚轮直接调亮度；点击行为不受影响（只拦滚轮）。

static HHOOK g_mouseLL = nullptr;

static LRESULT CALLBACK MouseLL(int code, WPARAM wp, LPARAM lp) {
    // 校准模式：点击任务栏托盘区内的 lumen 图标 → 记录光标位置为滚轮热区中心
    if (code == HC_ACTION && g_calibrating && g_msg &&
        (wp == WM_LBUTTONDOWN || wp == WM_LBUTTONUP)) {
        auto* ms = (MOUSEHOOKSTRUCTEX*)lp;
        HWND shell = FindWindowW(L"Shell_TrayWnd", nullptr);
        HWND tn = shell ? FindWindowExW(shell, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
        RECT r{};
        if (tn && GetWindowRect(tn, &r) && PtInRect(&r, ms->pt) && wp == WM_LBUTTONUP) {
            g_iconPt = ms->pt;
            g_iconHover = true;
            g_calibrating = false;
            RegPut(L"TrayX", g_iconPt.x);
            RegPut(L"TrayY", g_iconPt.y);
            wchar_t info[96];
            swprintf(info, 96, tr(S_CALDONE2), g_iconPt.x, g_iconPt.y);
            g_nid.uFlags |= NIF_INFO;
            wcsncpy(g_nid.szInfo, info, 255); g_nid.szInfo[255] = 0;
            wcsncpy(g_nid.szInfoTitle, L"lumen", 63); g_nid.szInfoTitle[63] = 0;
            g_nid.dwInfoFlags = NIIF_INFO;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            g_nid.uFlags &= ~NIF_INFO;
            return 1;   // 吞掉这次点击（不触发面板开关）
        }
    }
    // 滚轮不经过本钩子：统一由主窗口的 WM_INPUT（Raw Input）处理，
    // 避免同一次滚轮在两条通路上各调一次亮度；钩子只负责上面的图标位置校准。
    return CallNextHookEx(g_mouseLL, code, wp, lp);
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

static void RampPost(int pct) {       // 过渡自身的写入：经 Post 记账但不取消过渡
    g_rampPost = true;
    Post(pct);
    g_rampPost = false;
}

static void AutoRampTick() {
    if (!g_autoRamp || g_screenOff) { AutoRampStop(); return; }   // 关闭开关不打断过渡，滑完为止；熄屏仍终止
    ++g_autoTick;
    if (g_autoTick >= AUTO_RAMP_TICKS) { AutoRampStop(); Post(g_autoTo); return; }
    int v = (int)lround(g_autoFrom + (g_autoTo - g_autoFrom) * (double)g_autoTick / AUTO_RAMP_TICKS);
    if (v != g_val) RampPost(v);
}

static void StartAutoRamp(int from, int to) {
    g_autoFrom = from;
    g_autoTo   = to;
    g_autoTick = 0;
    g_autoRamp = true;
    SetTimer(g_msg, 7, AUTO_RAMP_TIMER_MS, nullptr);
}

static void ApplyAutoBrightness() {
    if (!g_cfgAuto || g_screenOff) { g_autoEngaged = false; return; }   // 熄屏期间不写亮度，避免覆盖恢复快照与 Last_ 记忆
    SYSTEMTIME st{};
    GetLocalTime(&st);
    double h = st.wHour + st.wMinute / 60.0;
    double s0 = g_cfgStartH, s1 = g_cfgEndH;
    if (s1 <= s0) s1 = s0 + 1;
    const double ramp = 8.0 / 60.0;          // 到点后 8 分钟渐变（用户要求的 5~10 分钟区间）
    double day = g_dayPct, night = g_nightPct;
    double pct;
    if (h < s0)             pct = night;                                      // 天亮前
    else if (h < s0 + ramp) pct = night + (day - night) * (h - s0) / ramp;    // 到开始时间：升到日间亮度
    else if (h < s1)        pct = day;                                        // 白天保持
    else if (h < s1 + ramp) pct = day + (night - day) * (h - s1) / ramp;      // 到结束时间：降到夜间亮度
    else                    pct = night;                                      // 夜间保持
    int target = (int)lround(pct);
    if (!g_autoEngaged) {                     // 刚打开（0→1）：从当前亮度 5 秒滑到目标，不跳变
        g_autoEngaged = true;
        if (g_autoRamp) return;
        int cur = g_val;
        if (target != cur && abs(target - cur) >= 2) { StartAutoRamp(cur, target); return; }
    } else if (g_autoRamp) {
        return;                               // 过渡进行中：60s 定时器不插手，每 tick 收敛到目标
    }
    Post(target);
}

static LRESULT CALLBACK MsgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY: {
        UINT tmsg = (UINT)LOWORD(lp);
        if (tmsg == WM_LBUTTONUP) TogglePanel();
        else if (tmsg == WM_RBUTTONUP) ShowMenu();
        return 0;
    }

    case WM_INPUT: {                      // 托盘图标上滚轮：Raw Input 直达，不依赖 explorer 转发
        BYTE buf[256];
        UINT sz = sizeof(buf);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            const RAWINPUT* ri = (const RAWINPUT*)buf;
            if (ri->header.dwType == RIM_TYPEMOUSE && (ri->data.mouse.usButtonFlags & RI_MOUSE_WHEEL)) {
                short d = (short)ri->data.mouse.usButtonData;
                if (d && g_wheelTray) {
                    RECT rc{};
                    POINT pt;
                    GetCursorPos(&pt);
                    bool hit = IconRect(&rc) &&
                               pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom;
                    if (!hit && g_iconHover)          // 图标矩形取不到时，退回校准位置 ±20/±26
                        hit = abs(pt.x - g_iconPt.x) <= 20 && abs(pt.y - g_iconPt.y) <= 26;
                    if (hit) {
                        // 按实际格数即时生效：快速滚动时数值跟手，写入频率由 Writer 合并
                        int steps = (d / WHEEL_DELTA) * g_step;
                        if (!steps) steps = d > 0 ? 1 : -1;   // 高精度滚轮（delta < 120）
                        Post(g_val + steps);
                        ShowOsd();
                    }
                }
            } else if (ri->header.dwType == RIM_TYPEHID && g_keyBright) {
                // 键盘亮度键（Consumer 0x006F 亮度+ / 0x0070 亮度−）：报告 = report ID + 16 位小端用法码，
                // 不同键盘 report ID/布局不同，按 16 位窗口扫描匹配，不依赖固定偏移
                unsigned long long now = GetTickCount64();
                if (now - g_lastKeyTick >= 160) {          // 去抖：按住键 auto-repeat ~30/s，否则 OSD/写入被刷爆
                    const BYTE* rep = ri->data.hid.bRawData;
                    UINT len = ri->data.hid.dwSizeHid * ri->data.hid.dwCount;
                    if (len > 64) len = 64;                // consumer 报告极小；缓冲 256 字节，bRawData 偏移 <32，安全
                    for (UINT i = 0; i + 1 < len; ++i) {
                        UINT16 u = (UINT16)rep[i] | ((UINT16)rep[i + 1] << 8);
                        if (u == 0x006F || u == 0x0070) {
                            g_lastKeyTick = now;
                            Adjust(u == 0x006F ? g_step : -g_step);
                            ShowOsd();
                            break;
                        }
                    }
                }
            }
        }
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

    case WM_DIM: {
        wchar_t* dev = (wchar_t*)wp;
        if (dev) {
            OverlayApply(dev, (int)lp);
            free(dev);
        }
        return 0;
    }

    case WM_WLCAL: {
        ULONG mx = (ULONG)wp;
        wchar_t info[128];
        if (mx >= 1500 && mx <= 20000) {
            g_wlMax = (int)mx;
            RegPut(L"WlMax", g_wlMax);
            RefreshHdrFlags(g_mons);
            swprintf(info, 128, tr(S_CALDONE), mx);
            Post(g_val);                      // 标定只刷新提示/诊断刻度，重放一次保持状态同步
        } else {
            swprintf(info, 128, tr(S_CALFAIL));
        }
        g_nid.uFlags |= NIF_INFO;
        wcsncpy(g_nid.szInfo, info, 255); g_nid.szInfo[255] = 0;
        wcsncpy(g_nid.szInfoTitle, L"lumen", 63); g_nid.szInfoTitle[63] = 0;
        g_nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        g_nid.uFlags &= ~NIF_INFO;
        return 0;
    }

    case WM_TIMER:
        if (wp == 6) {                           // 熄屏后等任意输入再点亮
            if (!g_screenOff) { KillTimer(g_msg, 6); return 0; }
            LASTINPUTINFO li{};
            li.cbSize = sizeof(li);
            if (!GetLastInputInfo(&li)) return 0;
            if (GetTickCount() - g_offAt < 3000) {
                g_offInputBase = li.dwTime;      // 3 秒内的误触一律吞掉，不算唤醒输入
                return 0;
            }
            if (li.dwTime != g_offInputBase) {
                KillTimer(g_msg, 6);
                RestoreFromScreenOff();
            }
            return 0;
        }
        if (wp == 2) ApplyAutoBrightness();
        if (wp == 7) AutoRampTick();
        if (wp == 8) ScrollAnimTick();
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

    case NIN_BALLOONUSERCLICK:            // 更新气泡被点击 → 打开下载页（气泡里链接原本只是纯文本）
        ShellExecuteW(nullptr, L"open", L"https://github.com/iosvcnm/lumen/releases/latest", nullptr, nullptr, SW_SHOWNOACTIVATE);
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_POWERBROADCAST:
        g_dispChangeTick = GetTickCount();   // 模式切换窗口：暂停 gamma 写入避开 GDI 挂死
        g_namesDone = false;
        g_mons = Scan();
        ApplyNames(g_mons);
        MatchSavedSelection();
        RestoreBrightness();
        OverlayPrune();
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
        return TRUE;

    case WM_CLOSE:
        if (g_screenOff) {                       // 退出时若仍处于熄屏：先点亮再退，避免屏幕留在黑屏状态
            g_screenOff = false;
            KillTimer(g_msg, 6);
            for (auto& dev : g_offDevs) SetMonitorPowerDDC(dev, 1);
            if (g_offAll)
                PostMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, (LPARAM)-1);
            g_offDevs.clear();
            g_offSaved.clear();
        }
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
        if (m == WM_LBUTTONDOWN) {            // 拖动浮窗：按住即可挪位置，期间暂停 1.4s 自动隐藏
            KillTimer(h, 1);
            g_osdDrag = true;
            g_osdAnchor = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            SetCapture(h);
            return 0;
        }
        if (m == WM_MOUSEMOVE && g_osdDrag) {
            POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ClientToScreen(h, &pt);
            SetWindowPos(h, nullptr, pt.x - g_osdAnchor.x, pt.y - g_osdAnchor.y, 0, 0,
                         SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
            return 0;
        }
        if (m == WM_LBUTTONUP && g_osdDrag) { // 释放：记录位置并恢复自动隐藏
            g_osdDrag = false;
            ReleaseCapture();
            RECT rc;
            GetWindowRect(h, &rc);
            g_osdPos = { rc.left, rc.top };
            RegPut(L"OsdX", (DWORD)rc.left);
            RegPut(L"OsdY", (DWORD)rc.top);
            SetTimer(h, 1, 1400, nullptr);
            return 0;
        }
        if (m == WM_CAPTURECHANGED) { g_osdDrag = false; return 0; }
        return DefWindowProcW(h, m, w, l);
    };
    c.lpszClassName = kOsdClass;
    c.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);   // 可拖动暗示
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
    g_mouseLL = SetWindowsHookExW(WH_MOUSE_LL, MouseLL, g_inst, 0);   // 托盘滚轮
    g_winHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);

    g_writer.Start();
    if (!g_gammaDim) ResetGammaAll();   // 归一遗留状态：上个实例被强杀时可能留下调暗的 ramp
    if (g_gammaDim) {
        // 预热每屏 DC：HDR 下 CreateDCW 可能长时间阻塞，放在主线程启动阶段做一次
        std::vector<HMONITOR> hs;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM lp) {
            ((std::vector<HMONITOR>*)lp)->push_back(h); return TRUE;
        }, (LPARAM)&hs);
        for (HMONITOR h : hs) {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(h, (MONITORINFO*)&mi)) DcCached(mi.szDevice);
        }
    }
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

    // 退出清理限时看门狗：清理段若卡死（驱动/DDC/GDI），3 秒后强制终结。
    // issue #1：v1.3.0 阻塞式 DDC 写入卡死 Writer，退出 join 永久等待 → 托盘已删、
    // 无窗口无界面，只剩「空线程进程」残留。看门狗保证任何卡死路径都不会留下进程。
    auto cleanupDone = std::make_shared<std::atomic<bool>>(false);
    std::thread([cleanupDone]() {
        for (int i = 0; i < 30 && !cleanupDone->load(); ++i) Sleep(100);
        if (!cleanupDone->load()) TerminateProcess(GetCurrentProcess(), 0);
    }).detach();

    g_writer.Stop();
    RestoreRampsAll();       // 退出：把备份的原始 gamma LUT 写回
    OverlayCloseAll();
    UnregisterHotKey(g_msg, HK_UP);
    UnregisterHotKey(g_msg, HK_DOWN);
    if (g_mouseLL) UnhookWindowsHookEx(g_mouseLL);
    UnregisterHotKey(g_msg, HK_TOGGLE);
    RemoveTray();
    if (g_winHook) UnhookWinEvent(g_winHook);
    if (g_icon) DestroyIcon((HICON)g_icon);
    GdiplusShutdown(tok);
    CoUninitialize();
    cleanupDone->store(true);
    return 0;
}
