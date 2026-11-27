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
#define IDR_DIAL_LIGHT 201
#define IDR_DIAL_DARK  202

static const wchar_t* kMsgClass   = L"LumenMsgWnd";
static const wchar_t* kBcastClass = L"LumenBcastWnd";   // 顶层隐藏：接收系统广播转发给 g_msg
static const wchar_t* kPanelClass = L"LumenPanelWnd";
static const wchar_t* kOsdClass   = L"LumenOsdWnd";
static const wchar_t* kBadgeClass = L"LumenBadgeWnd";       // 任务栏日/夜贴片（无底框图标）
static const wchar_t* kBadgeInfoClass = L"LumenBadgeInfoWnd"; // 悬停读数（名称 + 数值，无底框）
static const wchar_t* kCfgKey     = L"Software\\lumen";

#define WM_TRAY   (WM_APP + 1)
#define WM_SHOW   (WM_APP + 2)
#define WM_AUTOHIDE (WM_APP + 4)
#define WM_NAMES   (WM_APP + 6)
#define WM_UPDATE  (WM_APP + 7)   // wParam = new tag string (heap, receiver frees)
#define WM_DIM     (WM_APP + 8)   // wParam = dev string (heap), lParam = 遮罩黑度 0-85
#define WM_WLCAL   (WM_APP + 9)   // wParam = 标定得到的滑条最大白电平（0 = 失败）
#define WM_BADGEOPEN (WM_APP + 10)  // 双击任务栏贴片：打开主面板（低级鼠标钩子投递）

#define APP_VERSION L"1.5.0"

#define HK_UP     9001
#define HK_DOWN   9002
#define HK_TOGGLE 9005
#define ID_EXIT   4

// ---------------------------------------------------------------- 布局（96 DPI 逻辑坐标）

static const int   PANEL_W = 400;
static const int   PANEL_H = 570;
static const float KNOB_CX = 200.f;
static const float KNOB_CY = 215.f;   // 底图上移：给底部数值/胶囊留出呼吸
// dialRect 整体等比缩小 4.5%（用户指定 4%~5%）：底图与所有沿它定位的动态元素共用此系数
static const float DIAL_SCALE      = 0.955f;
static const float KNOB_R  = 110.f;
static const float DOT_R   = 174.f;
static const float TRACK_R = 106.f * DIAL_SCALE;   // 底图凹槽平底中线，随 dialRect 缩放
static const float PCT_Y   = 390.f;   // 数值下移 6px：为 Phase 2 底座下缘留呼吸
static const float PILL_Y  = 434.f;
static const float PILL_W  = 196.f;
static const float PILL_H  = 38.f;
static const float HINT_Y  = 477.f;
static const float BTN_CY  = 528.f;
static const float BTN_R   = 27.f;
static const float BTN_DX  = 60.f;
// 主旋钮几何（静态层与动态层共用；数值与改造前完全一致，命中测试不受影响）
// 新拟物旋钮几何（Phase 2）：刻度环半径刻意落在 TrackHit 的 146±20 命中带内，
// 拖拽映射与改造前完全一致（TrackHit / DragTo 未改动）。
// 预烘焙底图：画布 358 逻辑px（含外投影），其中可见底座约 332
static const float DIAL_CANVAS     = 358.f * DIAL_SCALE;
static const float NEU_HOUSING_R   = 146.f * DIAL_SCALE;  // 手绘兜底底座半边长
static const float NEU_HOUSING_RAD = 69.f * DIAL_SCALE;
static const float WELL_OUTER      = 140.f * DIAL_SCALE;
static const float WELL_INNER      = 112.f * DIAL_SCALE;
static const float DIAL_DOT_R      = 106.f * DIAL_SCALE;  // 刻度点环：稳落凹槽平底中心
static const float DOT_BASE_R      = 3.6f * DIAL_SCALE;
static const int   DIAL_N          = 30;     // 刻度点数（28~30，避免过密）
static const float KNOB_SKIRT_R    = 100.f * DIAL_SCALE;
static const float KNOB_CAP_R      = 88.f * DIAL_SCALE;
static const float NOTCH_R         = 74.f * DIAL_SCALE;

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
static int  g_oledH = 4;                 // OLED 连续亮屏提醒间隔（小时），0=关闭
static DWORD g_oledSec = 0;              // 连续亮屏秒数（睡眠/熄屏/屏保清零）
static bool g_oledNotified = false;      // 本轮已提醒，避免每帧重复弹气泡
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
static int  g_setFold = 0;            // 设置页分组折叠位：bit0 通用 / bit1 快捷键 / bit2 鼠标与键盘 / bit3 关于 / bit4 亮度与显示
static int  g_setHeaderHover = -1;    // 设置页悬停的分组标题（-1 无）
static bool g_ghHover = false;        // 设置页底部 GitHub 卡片按钮悬停

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
static int  g_pressIdx = -1;       // 按下态：0 胶囊 / 1..4 圆形按钮（仅视觉，动作仍在 mouse-down 触发）
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
// 主面板静态层缓存：背景 / 底座台阶 / 轨道槽 / 旋钮环 / 中心盘与图标。
// 只在窗口尺寸、DPI、浅深主题变化时重建；拖拽与滚轮每帧只画动态层。
static Bitmap* g_baseBmp   = nullptr;
static int     g_baseW     = 0, g_baseH = 0;
static float   g_baseK     = 0.f;
static bool    g_baseDark  = false;
static bool    g_baseDirty = true;
static void InvalidateBaseCache() { g_baseDirty = true; }
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

// —— 新拟物（Soft-UI）专用色板：与 g_pal 完全隔离，设置页 / OSD / 任务栏贴片不受影响 ——
// 光源统一 135°（左上入射、右下投影）。取值见规范 §2.2；Phase 1 只定义，暂不参与绘制。
struct NeuPal {
    Color bgCanvas, bgBase, highlight, shadow,
          trackWell, dotOff, dotOn, dotGlow,
          textPrimary, textMuted;
};
static NeuPal g_neu;

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
static int g_ddcOff = 0;                        // 精确熄屏（DDC 0xD6）开关，默认关——MStar 方案显示器有永久损坏 OSD 的已知固件缺陷（issue #6）
static unsigned long long g_lastKeyTick = 0;    // 亮度键去抖（按住键 auto-repeat ~30/s）
static int g_gammaDim = 0;   // 软件调光：HDR 固件锁 DDC 亮度时的 gamma 回退通路
static std::atomic<float> g_baseL{1.f};   // 仅供提示/诊断：当前白电平 ÷ 标定最大值（1 = 未标定 / 滑条最右）
static DWORD g_dispChangeTick = 0;        // 最近一次显示模式切换（HDR 开关等）
static DWORD g_saverTick = 0;             // 屏保启动时刻（0 = 不在屏保/尚未确认退出）
static DWORD g_saverInputBase = 0;        // 进入屏保时的最后输入时间戳（用于判定退出）
static std::atomic<bool> g_gammaDead{false}; // HDR 下 gamma LUT 不生效（读回校验失败）→ 全区间遮罩
static std::atomic<bool> g_hdrAny{false};    // 任一活动显示路径处于 Windows HDR（屏被 Scan 丢弃时兜底）
static std::atomic<ULONG> g_hdrAnyWl{0};    // 上述路径的 SDR 白电平（提示行 / 基准补偿兜底）
static POINT g_iconPt{};                    // 托盘图标实时位置（悬停时由 WM_MOUSEMOVE 更新）
static bool g_iconHover = false;            // 是否已获得图标位置（获得后滚轮判定收窄到图标邻域）
static bool g_calibrating = false;          // 校准中：下一次托盘左键点击记录图标位置
static int g_wlMax = 0;                   // 标定的滑条最大白电平（0 = 未标定）

// ---- 任务栏数值贴片（可选）：无底框；静止=日/夜小图标，悬停上方=设备名+数值 ----
static int  g_cfgBadge = 0;               // 注册表 TaskbarBadge，默认关
static HWND g_badge = nullptr;            // 图标窗口（ULW 逐像素透明）
static HWND g_badgeInfo = nullptr;        // 悬停读数窗口（同样无底框）
static int  g_badgeDpi = 96;
static bool g_badgeShown = false;
static bool g_badgeHovered = false;       // 指针在图标上：显示任务栏同款选中框
static bool g_badgeInfoShown = false;
static int  g_badgeDrawnPhase = -1;       // 0 夜 / 1 日；-1 未绘制
static bool g_badgeDrawnDark = false;
static std::wstring g_badgeInfoKey;       // 已绘制的悬停文本键（名称+数值+亮暗）
static RECT g_badgeRect{};                // 图标屏幕矩形（悬停读数定位用）
static RECT g_badgeTaskbarRect{};         // 任务栏当前矩形（读数放在任务栏外，避免被任务栏压住）
static int  g_badgeEdge = 3;              // 0 左 / 1 上 / 2 右 / 3 下
static DWORD g_badgeSlowTick = 0;         // 500ms 慢项（主题/设备名/读数文本）上次检查
static DWORD g_badgeHoverSince = 0;       // 指针进入图标的时间（0=不在图标上；悬停靠轮询，不依赖命中）
static DWORD g_badgeLastClick = 0;        // 双击判定：上一次按下的时间/位置
static POINT g_badgeLastClickPt{};
static const int BADGE_WL = 36;           // 96 DPI 逻辑尺寸（悬停选中框 + 图标）
static const int BADGE_HL = 30;

enum {
    S_BRIGHTNESS, S_THEME, S_AUTO, S_LIGHT, S_DARK,
    S_ALLMON, S_NOCTRL, S_ALLMONFMT, S_BUILTIN, S_PRIMARY, S_EXTERNAL, S_NOADJ,
    S_BUILTINSCR, S_GENERICFMT,
    S_HINT, S_FAIL, S_BACK, S_SETTINGS, S_AUTOSTART, S_AUTOBR, S_STARTH, S_ENDH, S_DDC, S_STEP,
    S_HOTKEYS, S_UP, S_DOWN, S_SHOWPILL, S_PRESSKEY, S_OCCUPIED, S_NEEDMOD, S_MODIFY, S_CANCEL, S_SIG,
    S_SHOWPANEL, S_BRIGHTUP, S_BRIGHTDN, S_EXIT, S_LANG, S_GAMMA,
    S_OCCUPFMT, S_UPDATEFMT, S_HDR, S_DAYBR, S_NIGHTBR,
    S_MOUSESET, S_WHEELTRAY, S_WHEELPANEL, S_KEYBRIGHT,
    S_RESTOFF, S_RESTON,
    S_CAL, S_CALDONE, S_CALFAIL, S_CALIB, S_CALIBHINT, S_CALDONE2,
    S_OFFPG, S_OFFHINT, S_OFFRESET,
    S_GENERAL, S_ABOUT, S_HOOKFAIL,
    S_FOLL, S_FONTS, S_DDCKILL, S_KILLWARN,
    S_BADGE,
    S_GHTITLE, S_GHDESC, S_GHSTAR,
    S_DISPLAYADV,
    S_UNBOUND, S_BINDHINT, S_OLEDREST, S_OLEDMSG,
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
    L"显示面板", L"亮度 +10", L"亮度 -10", L"退出 lumen", L"语言",
    L"软件调光（HDR 显示器用）",
    L"%ls %ls：组合被占用", L"lumen 有新版本 %ls\nhttps://github.com/iosvcnm/lumen/releases/latest",
    L" · HDR",
    L"日间亮度", L"夜间亮度",
    L"鼠标与键盘", L"托盘图标上滚轮调节亮度", L"面板打开时滚轮调节亮度", L"键盘亮度键（亮度 +/−）",
    L"屏幕已关闭", L"屏幕已恢复",
    L"标定 HDR 基准亮度", L"标定完成：基准 %lu", L"标定失败：请先拖动「SDR 内容亮度」滑条",
    L"校准滚轮图标位置", L"请点击任务栏上的 lumen 图标", L"已记录滚轮位置 (%d,%d)",
    L"单屏偏移", L"只影响「全部显示器」同步；单屏直接调节不偏移", L"全部归零",
    L"通用", L"关于", L"键盘钩子安装失败：点击面板后仍可录入",
    L"跟随系统", L"字体大小",
    L"精确熄屏（DDC 0xD6）",
    L"该显示器为 KTC/MStar 方案，存在「熄屏后 OSD 永久失效」的已知固件缺陷（详见项目 README）。\n\n仍要使用 DDC 熄屏吗？选「否」将改用系统休眠，无此风险。",
    L"任务栏数值贴片",
    L"支持 lumen", L"为项目加星，让更多人发现", L"在 GitHub 加星", L"亮度与显示",
    L"未绑定", L"按 Esc 解除绑定", L"OLED 像素刷新提醒",
    L"已连续使用 %d 小时，建议暂离熄屏让 OLED 自动执行像素补偿。",
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
    L"Show panel", L"Brightness +10", L"Brightness -10", L"Exit lumen", L"Language",
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
    L"General", L"About", L"Keyboard hook failed: click the panel first to record",
    L"Follow system", L"Font size",
    L"Precise screen off (DDC 0xD6)",
    L"This monitor (KTC/MStar scaler) has a known firmware defect: screen-off via DDC can permanently break the OSD (see the project README).\n\nStill use DDC screen-off? Choose No to use the system display sleep instead (no risk).",
    L"Taskbar readout",
    L"Support lumen", L"Star the project to help it grow", L"Star on GitHub", L"Display & Brightness",
    L"Unbound", L"Press Esc to unbind", L"OLED pixel refresh reminder",
    L"Continuous use for %d hours. Step away and let the OLED run its pixel compensation.",
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
static void BadgeInvalidate();      // 贴片重绘（Post/主题变化时调用；窗口未创建时无操作）
static void BadgeApplyEnabled();    // 开关打开/关闭时创建或销毁贴片（设置页调用）

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
    g_ddcOff     = RegGet(L"DdcScreenOff", 0) ? 1 : 0;
    g_cfgBadge   = RegGet(L"TaskbarBadge", 0) ? 1 : 0;
    g_setFold = RegGet(L"SetFold", 0) & 0x1F;  // 分组折叠状态：原有 bit0..3 含义不变，新增 bit4 亮度与显示
    g_wlMax = RegGet(L"WlMax", 0);
    { int oh = (int)RegGet(L"OledRemindH", 4);
      g_oledH = (oh == 0 || (oh >= 1 && oh <= 12)) ? oh : 4; }
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

// 取消绑定：vk=0 表示禁用。只注销热键并清注册表，保留 mod 便于日后重新绑定时恢复默认值
static void UnbindHotkey(int slot) {
    TouchPageCache();
    if (!g_msg) return;
    UINT* pVk = (slot == 1) ? &g_hkUpVk  : (slot == 2) ? &g_hkDownVk  : &g_hkShowVk;
    int id = (slot == 1) ? HK_UP : (slot == 2) ? HK_DOWN : HK_TOGGLE;
    const wchar_t* base = (slot == 1) ? L"HotUp" : (slot == 2) ? L"HotDown" : L"HotShow";
    UnregisterHotKey(g_msg, id);
    *pVk = 0;
    RegPut((std::wstring(base) + L"Vk").c_str(), 0);
    g_hkMsg = 0;
}

static void ApplyHotkey(int slot, UINT mod, UINT vk) {
    TouchPageCache();                        // 快捷键 pill 文本变化
    if (!g_msg) return;
    UINT* pMod = (slot == 1) ? &g_hkUpMod : (slot == 2) ? &g_hkDownMod : &g_hkShowMod;
    UINT* pVk  = (slot == 1) ? &g_hkUpVk  : (slot == 2) ? &g_hkDownVk  : &g_hkShowVk;
    int id = (slot == 1) ? HK_UP : (slot == 2) ? HK_DOWN : HK_TOGGLE;
    const wchar_t* base = (slot == 1) ? L"HotUp" : (slot == 2) ? L"HotDown" : L"HotShow";
    UINT oldMod = *pMod, oldVk = *pVk;

    UnregisterHotKey(g_msg, id);
    if (vk == 0) { *pVk = 0; RegPut((std::wstring(base) + L"Vk").c_str(), 0); g_hkMsg = 0; return; }
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
    // Vk=0 = 用户主动取消绑定，不注册也不报占用
    if (g_hkUpVk   && !RegisterHotKey(g_msg, HK_UP,     g_hkUpMod,   g_hkUpVk))   g_hkFail |= 1;
    if (g_hkDownVk && !RegisterHotKey(g_msg, HK_DOWN,   g_hkDownMod, g_hkDownVk)) g_hkFail |= 2;
    if (g_hkShowVk && !RegisterHotKey(g_msg, HK_TOGGLE, g_hkShowMod, g_hkShowVk)) g_hkFail |= 4;
}

// 启动时若有热键注册失败，用托盘气泡告知（只提示一次；录制路径已有面板内红字反馈）
static void NotifyHotkeyFailures() {
    if (!g_hkFail || !g_tray) return;
    std::wstring t;
    wchar_t b[192];
    if ((g_hkFail & 1) && g_hkUpVk)   { swprintf(b, 192, tr(S_OCCUPFMT), tr(S_UP), HotkeyText(g_hkUpMod, g_hkUpVk).c_str());   t += b; t += L"\n"; }
    if ((g_hkFail & 2) && g_hkDownVk) { swprintf(b, 192, tr(S_OCCUPFMT), tr(S_DOWN), HotkeyText(g_hkDownMod, g_hkDownVk).c_str()); t += b; t += L"\n"; }
    if ((g_hkFail & 4) && g_hkShowVk) { swprintf(b, 192, tr(S_OCCUPFMT), tr(S_SHOWPILL), HotkeyText(g_hkShowMod, g_hkShowVk).c_str()); t += b; }
    while (!t.empty() && t.back() == L'\n') t.pop_back();
    g_nid.uFlags |= NIF_INFO;
    wcsncpy(g_nid.szInfo, t.c_str(), 255);  g_nid.szInfo[255] = 0;
    wcsncpy(g_nid.szInfoTitle, L"lumen", 63); g_nid.szInfoTitle[63] = 0;
    g_nid.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags &= ~NIF_INFO;      // 还原标志：之后的 NIM_MODIFY 不再携带气泡
    g_hkFail = 0;
}

// OLED 连续亮屏提醒（issue #8）：睡眠 / 熄屏 / 屏保时清零；满间隔发一次轻量气泡
static void OledReset() { g_oledSec = 0; g_oledNotified = false; }

static void OledTick() {
    if (!g_oledH || g_screenOff || !g_tray) return;
    if (g_oledNotified) return;
    DWORD limit = (DWORD)g_oledH * 3600u;
    if (g_oledSec < 0xFFFFFFFFu) ++g_oledSec;
    if (g_oledSec < limit) return;
    g_oledNotified = true;
    wchar_t body[192];
    swprintf(body, 192, tr(S_OLEDMSG), g_oledH);
    g_nid.uFlags |= NIF_INFO;
    wcsncpy(g_nid.szInfo, body, 255);  g_nid.szInfo[255] = 0;
    wcsncpy(g_nid.szInfoTitle, tr(S_OLEDREST), 63); g_nid.szInfoTitle[63] = 0;
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags &= ~NIF_INFO;
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
                int slot = g_capSlot;
                StopCapture();
                UnbindHotkey(slot);      // issue #9：Esc 解除绑定，释放系统热键占用
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
    // 新拟物色板（规范 §2.2）：与 g_pal 并行，互不影响
    if (g_dark) {
        g_neu.bgCanvas    = Color(255, 0x13, 0x1B, 0x26);
        g_neu.bgBase      = Color(255, 0x18, 0x22, 0x30);
        g_neu.highlight   = Color(0x88, 0x2B, 0x3A, 0x4E);
        g_neu.shadow      = Color(0xDD, 0x07, 0x0B, 0x11);
        g_neu.trackWell   = Color(255, 0x10, 0x17, 0x20);
        g_neu.dotOff      = Color(255, 0x36, 0x48, 0x5E);   // 深色模式提亮未激活刻度
        g_neu.dotOn       = Color(255, 0xE8, 0xB4, 0x5A);
        g_neu.dotGlow     = Color(0x99, 0xF5, 0xB9, 0x52);   // 规范：0x99 琥珀光晕
        g_neu.textPrimary = Color(255, 0xF2, 0xBE, 0x63);
        g_neu.textMuted   = Color(255, 0x6B, 0x7B, 0x8E);
    } else {
        g_neu.bgCanvas    = Color(255, 0xE8, 0xEE, 0xF5);
        g_neu.bgBase      = Color(255, 0xED, 0xF2, 0xF8);
        g_neu.highlight   = Color(0xCC, 0xFF, 0xFF, 0xFF);
        g_neu.shadow      = Color(0xAA, 0xB8, 0xC6, 0xD8);
        g_neu.trackWell   = Color(255, 0xDC, 0xE4, 0xEE);
        g_neu.dotOff      = Color(255, 0xBC, 0xC7, 0xD6);   // 规范：提对比，让整圈轮廓更清晰
        g_neu.dotOn       = Color(255, 0x4B, 0x64, 0x84);
        g_neu.dotGlow     = Color(0x44, 0x4B, 0x64, 0x84);
        g_neu.textPrimary = Color(255, 0x2E, 0x43, 0x5C);
        g_neu.textMuted   = Color(255, 0x7B, 0x8C, 0xA1);
    }
    InvalidateBaseCache();                   // 主题切换 → 主面板静态层重建

    BadgeInvalidate();                       // 贴片读数颜色跟随任务栏亮暗（任务栏主题独立于应用主题）
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
// - 默认走系统广播 SC_MONITORPOWER=2（Windows 待机同款 DPMS 通路，对显示器无风险）
// - 设置页开启「精确熄屏（DDC 0xD6）」后才对 DDC 屏发 VCP 0xD6=5（可只关单屏；
//   MStar 方案显示器存在 0xD6 永久写坏 OSD 的已知固件缺陷，命中厂商黑名单时先弹系统确认框）
// 记录熄屏前亮度快照，待用户任意输入后恢复
static const wchar_t* const kOsdUnsafeVendors[] = { L"SKG" };   // KTC（issue #6），持续收集于 issue

static bool MonUnsafeOsd(const Mon& m) {
    if (m.key.rfind(L"MONITOR\\", 0) != 0) return false;
    size_t vs = wcslen(L"MONITOR\\");
    for (auto& v : kOsdUnsafeVendors)
        if (_wcsnicmp(m.key.c_str() + vs, v, 3) == 0) return true;
    return false;
}

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
        if (m.dev != L"WMI" && g_ddcOff) {
            g_offDevs.push_back(m.dev);
            g_offSaved.push_back(snapFor(m));
        } else {
            g_offAll = true;   // 内置屏无 DDC 通道 / 未开精确熄屏 → 系统广播
        }
        RegPut((L"Last_" + m.key).c_str(), m.pct);
    } else {
        for (auto& m : g_mons) {
            if (!m.controllable) continue;
            if (m.dev != L"WMI" && g_ddcOff) {
                g_offDevs.push_back(m.dev);
                g_offSaved.push_back(snapFor(m));
            } else {
                g_offAll = true;
            }
            RegPut((L"Last_" + m.key).c_str(), m.pct);
        }
        g_offAll = true;   // 「全部显示器」一律广播覆盖未被 DDC 覆盖的屏
    }

    // DDC 熄屏 + 黑名单厂商（KTC 等 MStar 方案）：系统确认框，默认「否」；取消即整体改走系统广播
    if (!g_offDevs.empty()) {
        bool unsafe = false;
        for (auto& m : g_mons)
            if (m.dev != L"WMI" && MonUnsafeOsd(m)) { unsafe = true; break; }
        if (unsafe) {
            int mb = MessageBoxW(HWND_TOPMOST, tr(S_KILLWARN), L"lumen",
                                 MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND);
            if (mb != IDYES) {
                g_offDevs.clear();
                g_offSaved.clear();
                g_offAll = true;
            }
        }
    }

    if (g_offDevs.empty() && !g_offAll) return;

    bool ddcFail = false;
    for (auto& dev : g_offDevs) if (!PowerOffDDC(dev)) ddcFail = true;
    if (g_offAll || ddcFail)   // DDC 电源寄存器不被支持时，退回系统级关屏
        PostMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, (LPARAM)2);

    g_screenOff = true;
    OledReset();                       // OLED 提醒：熄屏即清零重计
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
    BadgeInvalidate();              // 贴片读数与面板同源，任何亮度变化立即跟上
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

// 唤醒/重启后按 PnPId 恢复每台屏上次的亮度
// force=true：屏保退出/休眠唤醒后必须重放——显示器常把亮度寄存器复位，
// 但内存里的 m.pct 没变，差值判断会误以为已经一致而跳过写入
static void RestoreBrightness(bool force = false) {
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
        if (!force && last == m.pct) continue;
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
    swprintf(b, 320, L"\r\nlang=%d theme=%d step=%d ddc=%d auto=%d(%d-%d) autostart=%d gamma=%d wheel=%d/%d key=%d badge=%d hookFail=%d",
             g_lang, g_cfgTheme, g_step, g_cfgDelay, g_cfgAuto, g_cfgStartH, g_cfgEndH,
             g_cfgStart, g_gammaDim, g_wheelTray, g_wheelPanel, g_keyBright, g_cfgBadge, g_hookDead ? 1 : 0);
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

// 纯太阳：中心实心圆核（0.42r）+ 8 道圆头短射线，线条重量与月亮图标对称（规范 4.4）
static void DrawCleanSun(Graphics& g, float cx, float cy, float r, Color c) {
    float coreR = r * 0.42f;
    SolidBrush brush(c);
    g.FillEllipse(&brush, cx - coreR, cy - coreR, coreR * 2.0f, coreR * 2.0f);
    Pen pen(c, std::max(1.5f, r * 0.14f));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    float innerDist = r * 0.65f;
    float outerDist = r * 0.95f;
    for (int i = 0; i < 8; ++i) {
        float rad = (float)i * (PI / 4.0f);
        float cf = (float)cos(rad), sf = (float)sin(rad);
        g.DrawLine(&pen, cx + innerDist * cf, cy + innerDist * sf,
                          cx + outerDist * cf, cy + outerDist * sf);
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
    // 裁剪区内缩 1.2px：Region 是像素级（无抗锯齿），贴边裁会把阴影压到卡片 AA 边缘上，
    // 在圆角处形成阶梯脏边。内缩后阴影止于边缘内侧，最外圈交给描边与 alpha 渐变。
    GraphicsPath edge;
    RoundRect(edge, RectF(1.2f, 1.2f, (float)W - 2.4f, (float)H - 2.4f), 39.f);
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

// ---------------------------------------------------------------- 新拟物绘制辅助
// 连续曲率圆角（Squircle 近似）：四角三次贝塞尔，控制点 0.5523·r 保证切线连续。
static void AddSquirclePath(GraphicsPath& p, RectF r, float radius) {
    float x0 = r.X, y0 = r.Y, x1 = r.GetRight(), y1 = r.GetBottom();
    float rad = std::min(radius, std::min(r.Width, r.Height) * 0.5f);
    float c = rad * 0.5523f;
    p.StartFigure();
    p.AddLine(PointF(x0 + rad, y0), PointF(x1 - rad, y0));
    p.AddBezier(PointF(x1 - rad, y0), PointF(x1 - rad + c, y0),
                PointF(x1, y0 + rad - c), PointF(x1, y0 + rad));
    p.AddLine(PointF(x1, y0 + rad), PointF(x1, y1 - rad));
    p.AddBezier(PointF(x1, y1 - rad), PointF(x1, y1 - rad + c),
                PointF(x1 - rad + c, y1), PointF(x1 - rad, y1));
    p.AddLine(PointF(x1 - rad, y1), PointF(x0 + rad, y1));
    p.AddBezier(PointF(x0 + rad, y1), PointF(x0 + rad - c, y1),
                PointF(x0, y1 - rad + c), PointF(x0, y1 - rad));
    p.AddLine(PointF(x0, y1 - rad), PointF(x0, y0 + rad));
    p.AddBezier(PointF(x0, y0 + rad), PointF(x0, y0 + rad - c),
                PointF(x0 + rad - c, y0), PointF(x0 + rad, y0));
    p.CloseFigure();
}

// 柔和投影：N 层渐扩轮廓 + 平方衰减 alpha，模拟 CSS box-shadow 的羽化（规范 §3.2）
static void DrawNeumorphicShadow(Graphics& g, RectF base, float radius,
                                 float dx, float dy, float spread,
                                 Color color, int steps = 10, float core = 0.35f) {
    BYTE baseA = color.GetAlpha();
    for (int i = steps; i >= 1; --i) {
        float t = (float)i / (float)steps;
        float expand = spread * t;
        BYTE a = (BYTE)(baseA * (1.0f - t) * (1.0f - t) * core);
        if (!a) continue;
        RectF r = base;
        r.Offset(dx * t, dy * t);
        r.Inflate(expand, expand);
        GraphicsPath path;
        AddSquirclePath(path, r, radius + expand * 0.5f);
        SolidBrush b(Color(a, color.GetR(), color.GetG(), color.GetB()));
        g.FillPath(&b, &path);
    }
}

// 内阴影：裁剪到路径内，再按偏移方向描边——只有偏移侧一半落入内侧，形成内壁暗影（§3.3）
static void DrawInnerShadowClip(Graphics& g, GraphicsPath& path,
                                float dx, float dy, float width,
                                Color color, int steps = 6) {
    Region rgn(&path);
    GraphicsState st = g.Save();
    g.SetClip(&rgn);
    BYTE baseA = color.GetAlpha();
    for (int i = steps; i >= 1; --i) {
        float t = (float)i / (float)steps;
        BYTE a = (BYTE)(baseA * (1.0f - t) * (1.0f - t));
        if (!a) continue;
        GraphicsState s2 = g.Save();
        g.TranslateTransform(dx * t, dy * t);
        Pen pen(Color(a, color.GetR(), color.GetG(), color.GetB()), width * t);
        g.DrawPath(&pen, &path);
        g.Restore(s2);
    }
    g.Restore(st);
}


// 颜色线性混合（用于按主题微调明暗，避免硬编码两套色）
static Color MixC(Color a, Color b, float t) {
    return Color((BYTE)(a.GetA() + (b.GetA() - a.GetA()) * t),
                 (BYTE)(a.GetR() + (b.GetR() - a.GetR()) * t),
                 (BYTE)(a.GetG() + (b.GetG() - a.GetG()) * t),
                 (BYTE)(a.GetB() + (b.GetB() - a.GetB()) * t));
}

// 刻度光晕精灵：主题变化时烘焙一次，动态层每帧只做 DrawImage（避免每点重建 PathGradientBrush）
static Bitmap* g_glowBmp  = nullptr;
static bool    g_glowDark = false;
static void EnsureGlowSprite() {
    if (g_glowBmp && g_glowDark == g_dark) return;
    delete g_glowBmp;
    const int S = 64;
    g_glowBmp = new Bitmap(S, S, PixelFormat32bppPARGB);
    Graphics gg(g_glowBmp);
    gg.SetSmoothingMode(SmoothingModeAntiAlias);
    GraphicsPath gp;
    gp.AddEllipse(0.f, 0.f, (float)S, (float)S);
    PathGradientBrush pgb(&gp);
    pgb.SetCenterColor(g_neu.dotGlow);
    Color t(0, g_neu.dotGlow.GetR(), g_neu.dotGlow.GetG(), g_neu.dotGlow.GetB());
    int n = 1;
    pgb.SetSurroundColors(&t, &n);
    pgb.SetFocusScales(0.25f, 0.25f);
    gg.FillPath(&pgb, &gp);
    g_glowDark = g_dark;
}

// 外框底座：Squircle + 右下柔和投影 + 135° 边缘光（左上高光 → 右下暗影）
static void DrawSquircleHousing(Graphics& g, RectF r, float radius) {
    // 投影偏移/扩散加大约 18%，让底座与面板背景拉开层级
    DrawNeumorphicShadow(g, r, radius, 7.f, 10.f, 18.f, g_neu.shadow, 12);
    // 唇边倒角：外扩 3.5px 的一圈，左上提亮、右下压暗，形成可见厚度
    RectF lip(r.X - 3.5f, r.Y - 3.5f, r.Width + 7.f, r.Height + 7.f);
    GraphicsPath lp;
    AddSquirclePath(lp, lip, radius + 3.5f);
    LinearGradientBrush lb(PointF(lip.X, lip.Y), PointF(lip.GetRight(), lip.GetBottom()),
                           MixC(g_neu.bgBase, g_neu.highlight, g_dark ? 0.50f : 0.90f),
                           MixC(g_neu.bgBase, g_neu.shadow, g_dark ? 0.55f : 0.40f));
    g.FillPath(&lb, &lp);
    GraphicsPath p;
    AddSquirclePath(p, r, radius);
    LinearGradientBrush body(PointF(r.X, r.Y), PointF(r.GetRight(), r.GetBottom()),
                             MixC(g_neu.bgBase, g_neu.highlight, g_dark ? 0.55f : 0.65f),
                             MixC(g_neu.bgBase, g_neu.shadow, 0.30f));
    g.FillPath(&body, &p);
    Color cs[3] = { g_neu.highlight, Color(0, 0, 0, 0), g_neu.shadow };
    REAL pos[3] = { 0.f, 0.45f, 1.f };
    LinearGradientBrush edge(PointF(r.X, r.Y), PointF(r.GetRight(), r.GetBottom()),
                             g_neu.highlight, g_neu.shadow);
    edge.SetInterpolationColors(cs, pos, 3);
    Pen ep(&edge, 1.6f);
    g.DrawPath(&ep, &p);
}

// 凹陷环形轨道槽：凹底色 + 左上内壁暗影 + 右下内壁反光（阴影反转）
static void DrawRecessedTrackWell(Graphics& g) {
    GraphicsPath ring;
    ring.AddEllipse(KNOB_CX - WELL_OUTER, KNOB_CY - WELL_OUTER, WELL_OUTER * 2, WELL_OUTER * 2);
    ring.AddEllipse(KNOB_CX - WELL_INNER, KNOB_CY - WELL_INNER, WELL_INNER * 2, WELL_INNER * 2);
    ring.SetFillMode(FillModeAlternate);
    SolidBrush w(g_neu.trackWell);
    g.FillPath(&w, &ring);
    DrawInnerShadowClip(g, ring, 1.4f, 1.8f, 8.f, g_neu.shadow, 5);
    DrawInnerShadowClip(g, ring, -1.0f, -1.4f, 5.f, g_neu.highlight, 4);
}

static void DrawPhysicalKnob(Graphics& g);   // 手绘兜底在上面先声明

// 从 PE 资源内存流解码 PNG（CreateStreamOnHGlobal，零新增链接依赖）
static Bitmap* LoadPngFromResource(HINSTANCE hInst, int resId) {
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hRes) return nullptr;
    DWORD size = SizeofResource(hInst, hRes);
    HGLOBAL hMem = LoadResource(hInst, hRes);
    if (!hMem) return nullptr;
    void* pData = LockResource(hMem);
    if (!pData) return nullptr;
    HGLOBAL hCopy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hCopy) return nullptr;
    void* pCopy = GlobalLock(hCopy);
    if (!pCopy) { GlobalFree(hCopy); return nullptr; }
    memcpy(pCopy, pData, size);
    GlobalUnlock(hCopy);
    IStream* pStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hCopy, TRUE, &pStream))) { GlobalFree(hCopy); return nullptr; }
    Bitmap* bmp = Bitmap::FromStream(pStream);
    pStream->Release();
    if (bmp && bmp->GetLastStatus() != Ok) { delete bmp; return nullptr; }
    return bmp;
}

// 底图缓存：仅主题切换时重载（DPI 缩放交给 GDI+ 高质量插值）
static Bitmap* g_dialBmp = nullptr;
static bool    g_dialBmpDark = false;

// 静态 3D 底座（PNG 底图）；资源缺失时退回手绘底座，保证可用
static void DrawDialBase(Graphics& g) {
    if (!g_dialBmp || g_dialBmpDark != g_dark) {
        delete g_dialBmp;
        g_dialBmp = LoadPngFromResource(GetModuleHandleW(nullptr),
                                        g_dark ? IDR_DIAL_DARK : IDR_DIAL_LIGHT);
        g_dialBmpDark = g_dark;
    }
    if (!g_dialBmp) {
        RectF housing(KNOB_CX - NEU_HOUSING_R, KNOB_CY - NEU_HOUSING_R,
                      NEU_HOUSING_R * 2, NEU_HOUSING_R * 2);
        DrawSquircleHousing(g, housing, NEU_HOUSING_RAD);
        DrawRecessedTrackWell(g);
        DrawPhysicalKnob(g);
        return;
    }
    InterpolationMode im = g.GetInterpolationMode();
    PixelOffsetMode  pm = g.GetPixelOffsetMode();
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    // 深色：先在外唇边外侧烘焙 12px 右下深投影（必须在底图之前，否则会盖住底座）
    if (g_dark) {
        float hs = DIAL_CANVAS * 0.41f;          // 实际外唇边半边长 = dialRect 宽高的 82%/2
        float hr = DIAL_CANVAS * 0.82f * 0.22f;  // 圆角半径 ≈ 边长 22%
        RectF lipRect(KNOB_CX - hs, KNOB_CY - hs, hs * 2, hs * 2);
        DrawNeumorphicShadow(g, lipRect, hr, 4.f, 6.f, 12.f,
                             Color(0xAA, 0x04, 0x07, 0x0C), 9, 1.0f);
    }
    g.DrawImage(g_dialBmp, RectF(KNOB_CX - DIAL_CANVAS * 0.5f, KNOB_CY - DIAL_CANVAS * 0.5f,
                                 DIAL_CANVAS, DIAL_CANVAS));
    if (g_dark) {
        // 内侧 2.5px 135° 高光斜坡：左上冷灰蓝 → 右下全透明，贴合底图实际 Squircle 外唇边
        float hs = DIAL_CANVAS * 0.41f;
        float hr = DIAL_CANVAS * 0.82f * 0.22f;
        GraphicsPath lip;
        AddSquirclePath(lip, RectF(KNOB_CX - hs, KNOB_CY - hs, hs * 2, hs * 2), hr);
        LinearGradientBrush lg(PointF(KNOB_CX - hs, KNOB_CY - hs),
                               PointF(KNOB_CX + hs, KNOB_CY + hs),
                               Color(0x55, 0x5E, 0x7A, 0x9E), Color(0, 0, 0, 0));
        Pen lp(&lg, 2.5f);
        g.DrawPath(&lp, &lip);
    }
    g.SetInterpolationMode(im);
    g.SetPixelOffsetMode(pm);
}

// 未激活刻度：凹陷小孔（本体 DotOff + 右下 1px 微高光）
static void DrawDialHoles(Graphics& g) {
    const int N = DIAL_N;
    for (int i = 0; i < N; ++i) {
        double t = 210.0 + 300.0 * i / (N - 1);
        double a = (t - 90.0) * PI / 180.0;
        float x = KNOB_CX + DIAL_DOT_R * (float)cos(a);
        float y = KNOB_CY + DIAL_DOT_R * (float)sin(a);
        const float r = DOT_BASE_R * 0.72f;   // 未激活孔略小于激活点
        SolidBrush hole(g_neu.dotOff);
        g.FillEllipse(&hole, x - r, y - r, r * 2, r * 2);
        // 底缘微高光：低 Alpha 细弧，只交代"凹坑"，不形成白色杂点
        Pen hi(Color((BYTE)(g_dark ? 34 : 62), 255, 255, 255), 0.7f);
        g.DrawArc(&hi, x - r + 0.4f, y - r + 0.4f, (r - 0.4f) * 2, (r - 0.4f) * 2, 25, 130);
    }
}

// 立体旋钮：裙边落影 + 顶盖 135° 渐变 + 顶缘内高光环 + 底缘暗影
static void DrawPhysicalKnob(Graphics& g) {
    RectF skirt(KNOB_CX - KNOB_SKIRT_R, KNOB_CY - KNOB_SKIRT_R,
                KNOB_SKIRT_R * 2, KNOB_SKIRT_R * 2);
    DrawNeumorphicShadow(g, skirt, KNOB_SKIRT_R, 3.f, 5.f, 9.f, g_neu.shadow, 8);

    // 裙边：比顶盖深一档，形成台阶
    SolidBrush sk(MixC(g_neu.bgBase, g_neu.shadow, 0.30f));
    g.FillEllipse(&sk, skirt.X, skirt.Y, skirt.Width, skirt.Height);
    // 裙边高光：整圈均匀低 Alpha 双层柔环，圆周上没有任何起止角
    Pen skHi(Color((BYTE)(g_dark ? 40 : 86), 255, 255, 255), 1.4f);
    g.DrawEllipse(&skHi, skirt.X + 1.0f, skirt.Y + 1.0f, skirt.Width - 2.0f, skirt.Height - 2.0f);
    Pen skHi2(Color((BYTE)(g_dark ? 22 : 44), 255, 255, 255), 1.6f);
    g.DrawEllipse(&skHi2, skirt.X + 2.2f, skirt.Y + 2.2f, skirt.Width - 4.4f, skirt.Height - 4.4f);

    // 顶盖接触阴影：向下偏移的软影，制造"盖在裙边上"的高度
    {
        SolidBrush cs(Color((BYTE)(g_dark ? 90 : 55), g_neu.shadow.GetR(),
                            g_neu.shadow.GetG(), g_neu.shadow.GetB()));
        g.FillEllipse(&cs, KNOB_CX - KNOB_CAP_R - 1.f, KNOB_CY - KNOB_CAP_R + 3.5f,
                      (KNOB_CAP_R + 1.f) * 2, (KNOB_CAP_R + 1.f) * 2);
    }

    RectF cap(KNOB_CX - KNOB_CAP_R, KNOB_CY - KNOB_CAP_R, KNOB_CAP_R * 2, KNOB_CAP_R * 2);
    // 顶盖高光主体由填充渐变承担（连续、无端点）：左上接近纯白，右下压深一档
    LinearGradientBrush cg(PointF(cap.X, cap.Y), PointF(cap.GetRight(), cap.GetBottom()),
                           MixC(g_neu.bgBase, g_neu.highlight, g_dark ? 0.60f : 0.95f),
                           MixC(g_neu.bgBase, g_neu.shadow, g_dark ? 0.34f : 0.26f));
    g.FillEllipse(&cg, cap.X, cap.Y, cap.Width, cap.Height);

    // 顶缘光：整圈均匀低 Alpha 多层柔环（沿圆周没有起止角，物理上不可能出现硬截断），
    // 由内到外逐层递减 ≈ 1.5px 径向羽化。
    {
        struct { float inset; BYTE aL, aD; float w; } rings[] = {
            { 1.2f, 120, 55, 1.0f }, { 2.1f, 78, 36, 1.2f },
            { 3.1f, 46, 22, 1.4f }, { 4.2f, 24, 12, 1.6f },
        };
        for (auto& rg : rings) {
            Pen p(Color((BYTE)(g_dark ? rg.aD : rg.aL), 255, 255, 255), rg.w);
            g.DrawEllipse(&p, cap.X + rg.inset, cap.Y + rg.inset,
                          cap.Width - rg.inset * 2, cap.Height - rg.inset * 2);
        }
        // 右下暗影仍用渐变（起点 Alpha 0，天然不可见）
        RectF rim(cap.X + 0.9f, cap.Y + 0.9f, cap.Width - 1.8f, cap.Height - 1.8f);
        LinearGradientBrush shG(PointF(cap.X, cap.Y), PointF(cap.GetRight(), cap.GetBottom()),
                                Color(0, g_neu.shadow.GetR(), g_neu.shadow.GetG(), g_neu.shadow.GetB()),
                                Color((BYTE)(g_dark ? 165 : 95), g_neu.shadow.GetR(),
                                      g_neu.shadow.GetG(), g_neu.shadow.GetB()));
        Pen shP(&shG, 1.7f);
        g.DrawEllipse(&shP, rim);
    }
}

// 中心内雕凹槽图标：暗影左上偏移 + 高光右下偏移 + 主色原位（规范 §3.4），外加细雕环
static void DrawEngravedCenterIcon(Graphics& g) {
    const float R = 46.f * DIAL_SCALE;
    Pen ringSh(Color((BYTE)(g_dark ? 45 : 38), g_neu.shadow.GetR(),
                     g_neu.shadow.GetG(), g_neu.shadow.GetB()), 1.f);
    g.DrawEllipse(&ringSh, KNOB_CX - R, KNOB_CY - R, R * 2, R * 2);
    Pen ringHi(Color((BYTE)(g_dark ? 34 : 120), 255, 255, 255), 1.f);
    g.DrawArc(&ringHi, KNOB_CX - R, KNOB_CY - R, R * 2, R * 2, -20, 115);

    const float IR = 28.f * DIAL_SCALE;
    Color base = g_pal.icon;
    Color dk = MixC(base, g_neu.shadow, 0.75f);
    Color lt = MixC(base, g_neu.highlight, 0.85f);
    if (g_dark) {
        GraphicsPath gp;
        float glR = 48.f * DIAL_SCALE;
        gp.AddEllipse(KNOB_CX - glR, KNOB_CY - glR, glR * 2, glR * 2);
        PathGradientBrush pgb(&gp);
        pgb.SetCenterColor(Color(58, base.GetR(), base.GetG(), base.GetB()));
        Color gt(0, base.GetR(), base.GetG(), base.GetB());
        int gn = 1;
        pgb.SetSurroundColors(&gt, &gn);
        pgb.SetFocusScales(0.18f, 0.18f);
        g.FillPath(&pgb, &gp);
        DrawMoon(g, KNOB_CX - 1.f, KNOB_CY - 1.f, IR, Color(0x66, 0x06, 0x0A, 0x10)); // 暗影底衬：微凹雕刻感
        DrawMoon(g, KNOB_CX - 1.2f, KNOB_CY - 1.2f, IR, dk);
        DrawMoon(g, KNOB_CX + 1.2f, KNOB_CY + 1.2f, IR, MixC(base, g_neu.highlight, 0.55f));
        DrawMoon(g, KNOB_CX, KNOB_CY, IR, base);
    } else {
        DrawSun(g, KNOB_CX - 1.2f, KNOB_CY - 1.2f, IR, dk);
        DrawSun(g, KNOB_CX + 1.2f, KNOB_CY + 1.2f, IR, lt);
        DrawSun(g, KNOB_CX, KNOB_CY, IR, base);
    }
}

// 微凸新拟物块（胶囊 / 徽章通用）：外投影 + 135° 渐变 + 边缘光；pressed 时改为内凹（内阴影反转）
static void DrawNeuConvex(Graphics& g, RectF r, float radius,
                          Color fillA, Color fillB,
                          float dx, float dy, float spread, bool pressed) {
    GraphicsPath p;
    AddSquirclePath(p, r, radius);
    if (!pressed) {
        DrawNeumorphicShadow(g, r, radius, dx, dy, spread, g_neu.shadow, 7);
    }
    LinearGradientBrush b(PointF(r.X, r.Y), PointF(r.GetRight(), r.GetBottom()), fillA, fillB);
    g.FillPath(&b, &p);
    if (pressed) {
        DrawInnerShadowClip(g, p, 1.2f, 1.6f, 6.f, g_neu.shadow, 5);
        DrawInnerShadowClip(g, p, -0.9f, -1.2f, 4.f, g_neu.highlight, 4);
    } else {
        Color hiEdge = g_dark ? MixC(g_neu.highlight, g_neu.bgBase, 0.25f) : g_neu.highlight;
        Color cs[3] = { hiEdge, Color(0, 0, 0, 0), g_neu.shadow };
        REAL pos[3] = { 0.f, 0.5f, 1.f };
        LinearGradientBrush edge(PointF(r.X, r.Y), PointF(r.GetRight(), r.GetBottom()),
                                 hiEdge, g_neu.shadow);
        edge.SetInterpolationColors(cs, pos, 3);
        Pen ep(&edge, 1.2f);
        g.DrawPath(&ep, &p);
        if (g_dark) {
            // 左上微亮描边（渐变自然消失于右下）
            LinearGradientBrush hg(PointF(r.X, r.Y), PointF(r.GetRight(), r.GetBottom()),
                                   Color(0x48, 0x7A, 0x96, 0xB8),
                                   Color(0, 0x7A, 0x96, 0xB8));
            Pen hp(&hg, 1.2f);
            g.DrawPath(&hp, &p);
        }
    }
}

// 圆形功能按钮：默认微凸；hover 高光提亮、投影外扩；pressed 取消外投影并转为内凹
static void DrawNeuCircle(Graphics& g, float cx, float cy, float r, bool hot, bool pressed) {
    RectF rr(cx - r, cy - r, r * 2, r * 2);
    if (!pressed) {
        if (g_dark) {
            // 深色：(+4,+6) 右下 8px 渐扩软投影，核心 0xCC04070C 平方衰减 → 拉出悬浮厚度
            DrawNeumorphicShadow(g, rr, r, 4.f, 6.f, 8.f,
                                 Color(0xCC, 0x04, 0x07, 0x0C), 9, 1.0f);
        } else {
            // 投影整体下压（dy 增大、spread 收敛）：避免向上溢出压到上方提示文字
            DrawNeumorphicShadow(g, rr, r, 1.6f, hot ? 5.2f : 4.6f, hot ? 8.0f : 6.4f,
                                 g_neu.shadow, 7);
        }
    }
    Color top, bot;
    if (g_dark) {
        // 深色：135° 凸面渐变（左上受光 → 右下背光），替代单色平涂
        top = Color(0xFF, 0x27, 0x35, 0x48);
        bot = Color(0xFF, 0x15, 0x1E, 0x2A);
    } else {
        top = MixC(g_neu.bgBase, g_neu.highlight, hot ? 0.90f : 0.66f);
        bot = MixC(g_neu.bgBase, g_neu.shadow, pressed ? 0.32f : 0.16f);
    }
    LinearGradientBrush b(PointF(cx - r, cy - r), PointF(cx + r, cy + r), top, bot);
    g.FillEllipse(&b, rr.X, rr.Y, rr.Width, rr.Height);
    if (pressed) {
        GraphicsPath p;
        p.AddEllipse(rr.X, rr.Y, rr.Width, rr.Height);
        DrawInnerShadowClip(g, p, 1.4f, 1.8f, 7.f, g_neu.shadow, 5);
        DrawInnerShadowClip(g, p, -1.0f, -1.3f, 5.f, g_neu.highlight, 4);
    } else if (!g_dark) {
        // 浅色：保留原有顶缘高光 / 底缘暗影弧
        Color hc = Color(230, 255, 255, 255);
        hc = Color((BYTE)(hot ? 235 : 195), hc.GetR(), hc.GetG(), hc.GetB());
        Pen hi(hc, hot ? 2.4f : 2.0f);
        g.DrawArc(&hi, rr.X + 1.2f, rr.Y + 1.2f, rr.Width - 2.4f, rr.Height - 2.4f, 148, 124);
        Pen sh(Color(90, g_neu.shadow.GetR(), g_neu.shadow.GetG(), g_neu.shadow.GetB()), 1.6f);
        g.DrawArc(&sh, rr.X + 1.2f, rr.Y + 1.2f, rr.Width - 2.4f, rr.Height - 2.4f, -32, 118);
    }
    if (g_dark && !pressed) {
        // 深色：内缘 1.5px 整圈 135° 渐变高光（左上冷灰蓝 → 右下全透明，无端点）
        RectF ir(rr.X + 1.2f, rr.Y + 1.2f, rr.Width - 2.4f, rr.Height - 2.4f);
        LinearGradientBrush rg(PointF(ir.X, ir.Y), PointF(ir.GetRight(), ir.GetBottom()),
                               Color((BYTE)(hot ? 0x88 : 0x66), 0x86, 0xA4, 0xC8),
                               Color(0, 0, 0, 0));
        Pen rp(&rg, 1.5f);
        g.DrawEllipse(&rp, ir);
    }
}

// 主面板静态层：背景 / Squircle 底座 / 凹陷轨道槽 / 刻度孔 / 立体旋钮 / 内雕图标。
// 绘制内容与顺序同改造前逐条对应，只画一次进离屏位图（主题 / DPI / 尺寸变化才重建）。
static void DrawPanelStatic(Graphics& g, int W, int H) {
    GraphicsPath card;
    RoundRect(card, RectF(0, 0, (float)W, (float)H), 40.f);
    if (g_dark) {
        // 深色：垂直渐变（顶部略亮 → 底部更深），衬托 dial_dark.png 底座自身投影
        LinearGradientBrush bg(PointF(0, 0), PointF(0.f, (float)H),
                               Color(0xFF, 0x23, 0x30, 0x40),
                               Color(0xFF, 0x17, 0x20, 0x2B));
        g.FillPath(&bg, &card);
    } else {
        LinearGradientBrush bg(PointF(0, 0), PointF((float)W, (float)H), g_pal.bg1, g_pal.bg2);
        g.FillPath(&bg, &card);
    }
    Pen border(g_pal.border, 1.f);
    g.DrawPath(&border, &card);
    if (g_dark) {
        // 深色下在暗色桌面易丢失窗口边界：叠一条左上亮→右下暗的 1px 渐变轮廓
        LinearGradientBrush rim(PointF(0, 0), PointF((float)W, (float)H),
                                Color(0x70, 0x90, 0xB0, 0xD4),
                                Color(0x30, 0x5A, 0x74, 0x92));
        Pen rp(&rim, 1.f);
        g.DrawPath(&rp, &card);
    }
    CardInsetShadow(g, W, H);

    SolidBrush tb(g_pal.text);
    Font brand(L"Consolas", 19, FontStyleBold, UnitPixel);
    DrawTracked(g, L"lumen", brand, tb, W / 2.f, 24.f, 5.0f);

    // —— 3D 底座（PNG 预烘焙）→ 未激活刻度孔 → 内雕中心图标 ——
    {
        DrawDialBase(g);
        DrawDialHoles(g);
        DrawEngravedCenterIcon(g);
    }
}

// 主面板动态层：随亮度 / HDR / hover 变化，每帧直接绘制（不缓存）。
static void DrawPanelDynamic(Graphics& g, int W, int H) {
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);

    SolidBrush tb(g_pal.text);

    // HDR 判定：选中屏优先级最高；「全部显示器」按真实活动路径（HDR 切换瞬间某屏 DDC 读失败也不误判）
    bool hdrNow = false;
    if (g_sel >= 0 && g_sel < (int)g_mons.size()) hdrNow = g_mons[g_sel].hdr;
    else {
        hdrNow = g_hdrAny.load();
        for (auto& mm : g_mons) if (mm.hdr) hdrNow = true;
    }

    // 旋钮暖金环境漫反射（仅深色）：围绕当前角度的大半径低透明光晕，随亮度方向移动。
    // 必须放动态层——静态层缓存不随 g_val 重建，画在那里光晕不会跟随刻度。
    if (g_dark && g_val > 0) {
        double ga = (210.0 + g_val / 100.0 * 300.0 - 90.0) * PI / 180.0;
        float gd = DIAL_DOT_R * 0.68f;
        float gx = KNOB_CX + gd * (float)cos(ga);
        float gy = KNOB_CY + gd * (float)sin(ga);
        float gr = 74.f * DIAL_SCALE;
        GraphicsPath gp;
        gp.AddEllipse(gx - gr, gy - gr, gr * 2, gr * 2);
        PathGradientBrush pg(&gp);
        pg.SetCenterColor(Color(0x1C, 0xFF, 0xE0, 0x9A));
        Color ge(0x00, 0xF5, 0xB9, 0x52);
        int gn = 1;
        pg.SetSurroundColors(&ge, &gn);
        pg.SetFocusScales(0.25f, 0.25f);
        g.FillPath(&pg, &gp);
    }

    // 激活刻度：起点→当前点半径 0.75x→1.15x、Alpha 65%→100% 的渐进强弱；当前点 1.35x 作焦点
    {
        EnsureGlowSprite();
        const int N = DIAL_N;
        int cur = (int)lround(g_val / 100.0 * (N - 1));
        if (cur < 0) cur = 0;
        for (int i = 0; i <= cur; ++i) {
            double t = 210.0 + 300.0 * i / (N - 1);
            double a = (t - 90.0) * PI / 180.0;
            float x = KNOB_CX + DIAL_DOT_R * (float)cos(a);
            float y = KNOB_CY + DIAL_DOT_R * (float)sin(a);
            float tt = (cur > 0) ? (float)i / (float)cur : 1.0f;
            float r = DOT_BASE_R * (0.75f + 0.40f * tt);
            float w = 0.62f + 0.38f * tt;                  // 视觉强度 65% → 100%
            bool isCur = (i == cur);
            if (isCur) r = DOT_BASE_R * 1.35f;

            g.DrawImage(g_glowBmp, RectF(x - r * 2.8f, y - r * 2.8f, r * 5.6f, r * 5.6f));
            if (g_dark) {
                // 暖白金核心 + 琥珀金外层（不透明混色，避免下层凹孔透出成"甜甜圈"）
                Color body = MixC(g_neu.trackWell, g_neu.dotOn, w);
                SolidBrush core(body);
                g.FillEllipse(&core, x - r, y - r, r * 2, r * 2);
                SolidBrush hot(MixC(body, Color(255, 228, 160), 0.72f));
                g.FillEllipse(&hot, x - r * 0.45f, y - r * 0.45f, r * 0.9f, r * 0.9f);
            } else {
                Color body = MixC(g_neu.trackWell, Color(0x4B, 0x64, 0x84), w);
                SolidBrush core(body);
                g.FillEllipse(&core, x - r, y - r, r * 2, r * 2);
                if (isCur) {
                    // 双层环：紧贴点缘，不留浅色缝隙
                    Pen ring(Color(135, 0x4B, 0x64, 0x84), 1.3f);
                    g.DrawEllipse(&ring, x - r - 0.9f, y - r - 0.9f, (r + 0.9f) * 2, (r + 0.9f) * 2);
                }
            }
        }
    }

    // 旋钮缺口指针：随亮度旋转的物理凹窝（替代原滑动圆球，拖拽映射不变）
    {
        double ang = 210.0 + g_val / 100.0 * 300.0;
        double ra = (ang - 90.0) * PI / 180.0;
        float nx = KNOB_CX + NOTCH_R * (float)cos(ra);
        float ny = KNOB_CY + NOTCH_R * (float)sin(ra);
        const float NR = 7.f;
        SolidBrush nb(MixC(g_neu.bgBase, g_neu.shadow, 0.55f));
        g.FillEllipse(&nb, nx - NR, ny - NR, NR * 2, NR * 2);
        Pen nr(g_neu.shadow, 1.4f);
        g.DrawArc(&nr, nx - NR, ny - NR, NR * 2, NR * 2, 145, 125);
        Pen nh(MixC(g_neu.highlight, g_neu.bgBase, 0.15f), 1.1f);
        g.DrawArc(&nh, nx - NR + 1.f, ny - NR + 1.f, (NR - 1.f) * 2, (NR - 1.f) * 2, -35, 120);
        SolidBrush nc(g_neu.dotOn);
        g.FillEllipse(&nc, nx - 2.6f, ny - 2.6f, 5.2f, 5.2f);
    }

    // 百分比：浅色深蓝 / 深色琥珀（与进度弧同源）
    wchar_t buf[16];
    swprintf(buf, 16, L"%d%%", g_val);
    Font fv(L"Consolas", 42, FontStyleBold, UnitPixel);   // 缩小 4px：与底座下缘留出呼吸，等宽字形拖动不抖
    SolidBrush pb3(g_neu.textPrimary);   // 规范 4.5：浅 #2E435C / 深 #F2BE63
    g.DrawString(buf, -1, &fv, RectF(0, PCT_Y, (float)W, 52), &sf, &pb3);

    // 显示器下拉胶囊：微凸新拟物（hover 提亮、展开/按下转内凹）
    RectF pill(W / 2.f - PILL_W / 2, PILL_Y, PILL_W, PILL_H);
    {
        bool pp = (g_menuOpen || g_pressIdx == 0);
        Color pA = MixC(g_neu.bgBase, g_neu.highlight, pp ? 0.30f : (g_hover == 0 ? 0.85f : 0.70f));
        Color pB = MixC(g_neu.bgBase, g_neu.shadow, pp ? 0.28f : 0.14f);
        DrawNeuConvex(g, pill, PILL_H / 2.f, pA, pB, 2.f, 3.f, 8.f, pp);
    }

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
        float tw2 = box.Width + 28, th = 30;
        float tx = (W - tw2) / 2, ty = PILL_Y - th - 8;
        GraphicsPath tp;
        RoundRect(tp, RectF(tx, ty, tw2, th), 10.f);
        SolidBrush tbg(g_pal.menuBg);
        g.FillPath(&tbg, &tp);
        Pen te(g_pal.pillEdge, 1.f);
        g.DrawPath(&te, &tp);
        StringFormat tf;
        tf.SetAlignment(StringAlignmentCenter);
        tf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(full.c_str(), -1, &ft, RectF(tx, ty, tw2, th), &tf, &tb);
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
        // HDR 激活 = 微凹按下态（内阴影反转），文字/细边用主题强调色
        RectF chip(pill.GetRight() + 12, PILL_Y + (PILL_H - 26) / 2, 52, 26);
        Color cA = MixC(g_neu.bgBase, g_neu.shadow, 0.26f);
        Color cB = MixC(g_neu.bgBase, g_neu.highlight, 0.22f);
        DrawNeuConvex(g, chip, 13.f, cA, cB, 1.2f, 1.6f, 5.f, true);
        GraphicsPath cp;
        AddSquirclePath(cp, chip, 13.f);
        Pen glow(Color((BYTE)(g_dark ? 125 : 48), g_neu.dotOn.GetR(),
                        g_neu.dotOn.GetG(), g_neu.dotOn.GetB()), g_dark ? 1.6f : 1.2f);
        g.DrawPath(&glow, &cp);
        Font fh2(L"Consolas", FS(12.f), FontStyleBold, UnitPixel);
        SolidBrush ctb(g_neu.dotOn);
        g.DrawString(L"HDR", -1, &fh2, chip, &sf, &ctb);
    }

    // 提示：HDR 激活时显示 Windows 白电平（滑条镜像）与 lumen 调光比例，两数语义分明
    SolidBrush hb(g_dark ? Color(255, 0x8A, 0x9B, 0xB0) : g_neu.textMuted);   // 深色下进一步提亮
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

        DrawNeuCircle(g, cx, BTN_CY, rr, hot, g_pressIdx == i + 1);

        // 激活态：夜间/深色生效（月亮）或自动亮度开启 → 主题强调色（深色琥珀金 / 浅色深海蓝）
        bool act = (i == 0) ? g_dark : ((i == 1) ? (g_cfgAuto != 0) : false);
        Color ic = act ? g_neu.dotOn : g_pal.sub;
        if (act && g_dark) {
            // 深色下叠一层径向琥珀光晕，与旋钮点亮刻度呼应
            GraphicsPath gp;
            gp.AddEllipse(cx - 15.f, BTN_CY - 15.f, 30.f, 30.f);
            PathGradientBrush pg(&gp);
            pg.SetCenterColor(Color(78, g_neu.dotOn.GetR(), g_neu.dotOn.GetG(), g_neu.dotOn.GetB()));
            Color gt(0, g_neu.dotOn.GetR(), g_neu.dotOn.GetG(), g_neu.dotOn.GetB());
            int gn = 1;
            pg.SetSurroundColors(&gt, &gn);
            pg.SetFocusScales(0.20f, 0.20f);
            g.FillPath(&pg, &gp);
        }
        if (i == 0) {
            // 浅色=纯太阳，深色=月亮（与参考图一致，取消日月重叠图标）
            if (g_dark) DrawMoon(g, cx, BTN_CY, 11.f, ic);
            else        DrawCleanSun(g, cx, BTN_CY, 11.f, ic);
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
    // 每帧先清空为全透明：保证圆角外沿没有上一帧残留，再叠静态层
    g.Clear(Color(0, 0, 0, 0));
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.ScaleTransform(k, k);

    if (g_page == 0) {
        // 静态层离屏预烘焙：背景/底座/轨道槽/旋钮/图标，仅 DPI·主题·尺寸变化时重建
        if (g_baseDirty || !g_baseBmp || g_baseW != w || g_baseH != h ||
            g_baseDark != g_dark || g_baseK != k) {
            delete g_baseBmp;
            // 2× 超采样：重建时放大一倍绘制再降采样一次，得到 2×2 抗锯齿；
            // 每帧仍是 1:1 贴回（若每帧降采样，拖拽 CPU 会翻数倍）。
            Bitmap* hi = new Bitmap(w * 2, h * 2, PixelFormat32bppPARGB);
            {
                Graphics bgr(hi);
                bgr.SetSmoothingMode(SmoothingModeHighQuality);
                bgr.SetPixelOffsetMode(PixelOffsetModeHighQuality);
                bgr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
                bgr.ScaleTransform(k * 2.f, k * 2.f);
                DrawPanelStatic(bgr, Wl, Hl);
            }
            g_baseBmp = new Bitmap(w, h, PixelFormat32bppPARGB);
            {
                Graphics bg(g_baseBmp);
                bg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                bg.SetPixelOffsetMode(PixelOffsetModeHighQuality);
                bg.DrawImage(hi, RectF(0.f, 0.f, (REAL)w, (REAL)h));
            }
            delete hi;
            g_baseW = w; g_baseH = h; g_baseK = k; g_baseDark = g_dark;
            g_baseDirty = false;
        }
        // 设备坐标 1:1 贴回，避免二次重采样导致文字与边缘发虚
        GraphicsState bs = g.Save();
        g.ResetTransform();
        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
        g.DrawImage(g_baseBmp, 0, 0, w, h);
        g.Restore(bs);
        DrawPanelDynamic(g, Wl, Hl);             // 动态层：刻度/进度/数值/胶囊/按钮
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
static const float GH_CARD_H = 104.f;  // 底部 GitHub 卡片高度（含标题/说明/pill 按钮）
static const float GH_BTN_W  = 190.f;  // pill 按钮宽度
static const float GH_BTN_H  = 34.f;   // pill 按钮高度

struct SetLay {
    float hdrY[5];    // 各组标题 Y（组 1..4 的分隔线画在 hdrY-12）
    float bodyY[5];   // 展开时 body 首行 Y
    float ghY;        // GitHub 卡片 Y（位于「关于」分组 body 内，跟随其折叠状态）
    float total;      // 页面总高
};

static SetLay g_setLay;
static void RefreshSetLayout() {
    // 「关于」body 高度 = 卡片上方 8px 呼吸 + GitHub 卡片（两行入口已移除）
    // 视觉顺序：通用(0) / 快捷键(1) / 鼠标与键盘(2) / 亮度与显示(4) / 关于(3)
    // 折叠位语义不变（bit3=关于、bit4=亮度），只改 Y 分配顺序；已保存的 SetFold 全部兼容
    static const float bodyH[5] = { 4 * SET_ROW, 3 * SET_HKROW, 4 * SET_ROW,
                                    8.f + GH_CARD_H, 8 * SET_ROW };
    static const int visOrder[5] = { 0, 1, 2, 4, 3 };   // 视觉位置 → 组 ID
    float y = 104.f;                       // 首组标题（页首分隔线 y=96 下方）
    for (int vp = 0; vp < 5; ++vp) {
        int i = visOrder[vp];
        if (vp > 0) y += 8.f;              // 组间距（容纳上方分隔线）
        g_setLay.hdrY[i] = y;
        y += SET_HH;
        g_setLay.bodyY[i] = y;
        if (g_setFold & (1 << i)) y += bodyH[i];
    }
    // 卡片位于「关于」body 内，标题下方 8px（折叠时不参与绘制）
    g_setLay.ghY = g_setLay.bodyY[3] + 8.f;
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
static float SetAdvRowY(int i)   { return g_setLay.bodyY[4] + i * SET_ROW; }   // 新增「亮度与显示」分组

static RectF  SetToggleRect(int i)    { return RectF(SET_R - 44.f, SetRowY(i) - 4.f, 44.f, 24.f); }
static RectF  SetAdvToggleRect(int i) { return RectF(SET_R - 44.f, SetAdvRowY(i) - 4.f, 44.f, 24.f); }
// 第 2/3 行双列：左列 时间，右列 亮度，各自带加减（值矩形按 120% 字体不折行留足宽度）
// 双列时间/亮度（原通用组第2/3行 → 新分组第1/2行）：i 仍传 2/3，内部减一映射
static RectF  SetTimeValueRect(int i) { return RectF(96.f,  SetAdvRowY(i-1) - 6.f, 48.f, 28.f); }
static RectF  SetTimeMinusRect(int i) { return RectF(146.f, SetAdvRowY(i-1) - 6.f, 30.f, 30.f); }
static RectF  SetTimePlusRect(int i)  { return RectF(178.f, SetAdvRowY(i-1) - 6.f, 30.f, 30.f); }
static RectF  SetPctValueRect(int i)  { return RectF(268.f, SetAdvRowY(i-1) - 6.f, 42.f, 28.f); }
static RectF  SetPctMinusRect(int i)  { return RectF(312.f, SetAdvRowY(i-1) - 6.f, 30.f, 30.f); }
static RectF  SetPctPlusRect(int i)   { return RectF(344.f, SetAdvRowY(i-1) - 6.f, 30.f, 30.f); }
// 原通用组第4行 → 新分组第3行：左 DDC 间隔，右 步长
static RectF  SetDdcValue()           { return RectF(114.f, SetAdvRowY(3) - 6.f, 54.f, 28.f); }
static RectF  SetDdcMinus()           { return RectF(170.f, SetAdvRowY(3) - 6.f, 30.f, 30.f); }
static RectF  SetDdcPlus()            { return RectF(200.f, SetAdvRowY(3) - 6.f, 30.f, 30.f); }
static RectF  SetStepValue()          { return RectF(282.f, SetAdvRowY(3) - 6.f, 26.f, 28.f); }
static RectF  SetStepMinus()          { return RectF(312.f, SetAdvRowY(3) - 6.f, 30.f, 30.f); }
static RectF  SetStepPlus()           { return RectF(344.f, SetAdvRowY(3) - 6.f, 30.f, 30.f); }
// 第 8 行：OLED 像素刷新提醒间隔（0=关闭，1..12 小时）
// 标签较长（OLED 像素刷新提醒），数值与按钮靠右排列避免压字
static RectF  SetOledValue()          { return RectF(232.f, SetAdvRowY(7) - 6.f, 50.f, 28.f); }
static RectF  SetOledMinus()          { return RectF(286.f, SetAdvRowY(7) - 6.f, 30.f, 30.f); }
static RectF  SetOledPlus()           { return RectF(320.f, SetAdvRowY(7) - 6.f, 30.f, 30.f); }
static RectF  SetEditRect(int i)      { return RectF(SET_R - 72.f, SetHkY(i), 72.f, 30.f); }
static RectF  SetKeyRect(int i)       { return RectF(96.f, SetHkY(i), 190.f, 30.f); }
static RectF  SetBackRect()           { return RectF(16.f, 8.f, 120.f, 44.f); }
static RectF  SetMouseToggleRect(int i) { return RectF(SET_R - 44.f, SetMouseRowY(i) - 4.f, 44.f, 24.f); }
// 通用组第 6/7 行（新增）：语言整行点击循环切换；字体大小带加减
static RectF  SetLangRect()           { return RectF(SET_L - 8.f, SetRowY(1) - 6.f, SET_R - SET_L + 16.f, 34.f); }
static RectF  SetLangValueRect()      { return RectF(SET_R - 130.f, SetRowY(1) - 2.f, 130.f, 28.f); }
static RectF  SetFontValueRect()      { return RectF(260.f, SetRowY(2) - 6.f, 50.f, 28.f); }
static RectF  SetFontMinusRect()      { return RectF(312.f, SetRowY(2) - 6.f, 30.f, 30.f); }
static RectF  SetFontPlusRect()       { return RectF(344.f, SetRowY(2) - 6.f, 30.f, 30.f); }
// 自托盘菜单迁入的功能行：软件调光(行7 开关) / 标定 HDR(行8 整行) / 校准滚轮(鼠标组行3)
static RectF  SetCalWLRect()          { return RectF(SET_L - 8.f, SetAdvRowY(5) - 6.f, SET_R - SET_L + 16.f, 34.f); }
static RectF  SetCalWheelRect()       { return RectF(SET_L - 8.f, SetMouseRowY(3) - 6.f, SET_R - SET_L + 16.f, 34.f); }
// 底部 GitHub 卡片：卡片整体 + pill 按钮（绘制与命中共用，随折叠布局移动）
static RectF  GithubCardRect()        { return RectF(SET_L - 8.f, g_setLay.ghY, SET_R - SET_L + 16.f, GH_CARD_H); }
static RectF  GithubBtnRect()         { RectF c = GithubCardRect(); return RectF(c.X + (c.Width - GH_BTN_W) / 2, c.Y + 60.f, GH_BTN_W, GH_BTN_H); }

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

// GitHub 猫头标记（纯矢量近似：圆头 + 双耳 + 尾巴），随主题取色
static void DrawGitHubMark(Graphics& g, float cx, float cy, float r, Color c) {
    SolidBrush b(c);
    g.FillEllipse(&b, cx - r, cy - r * 0.82f, r * 2, r * 1.72f);
    PointF le[3] = { {cx - r*0.88f, cy - r*0.45f}, {cx - r*0.52f, cy - r*1.32f}, {cx - r*0.14f, cy - r*0.72f} };
    PointF re[3] = { {cx + r*0.88f, cy - r*0.45f}, {cx + r*0.52f, cy - r*1.32f}, {cx + r*0.14f, cy - r*0.72f} };
    g.FillPolygon(&b, le, 3);
    g.FillPolygon(&b, re, 3);
    Pen p(c, r * 0.30f);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    g.DrawArc(&p, cx + r*0.15f, cy + r*0.18f, r*1.55f, r*1.15f, 250, 105);
}

// 设置页底部 GitHub 卡片：卡片底 + 标题/说明 + pill 按钮（按钮内图标 + 文案居中）
static void DrawGithubCard(Graphics& g) {
    RectF card = GithubCardRect();
    GraphicsPath cp;
    RoundRect(cp, card, 11.f);
    SolidBrush cbg(g_pal.menuBg);
    g.FillPath(&cbg, &cp);
    Pen ce(g_pal.pillEdge, 1.f);
    g.DrawPath(&ce, &cp);

    SolidBrush tb(g_pal.text), sb(g_pal.sub);
    Font ft(L"Microsoft YaHei UI", FS(15.f), FontStyleBold, UnitPixel);
    Font fd(L"Microsoft YaHei UI", FS(11.f), FontStyleRegular, UnitPixel);
    DrawGitHubMark(g, card.X + 30, card.Y + 30, 13.f, g_pal.text);
    g.DrawString(tr(S_GHTITLE), -1, &ft, PointF(card.X + 54, card.Y + 13), &tb);
    g.DrawString(tr(S_GHDESC), -1, &fd, PointF(card.X + 54, card.Y + 38), &sb);

    RectF btn = GithubBtnRect();
    GraphicsPath bp;
    RoundRect(bp, btn, btn.Height / 2.f);
    SolidBrush bb(g_ghHover ? g_pal.pillHover : g_pal.pill);
    g.FillPath(&bb, &bp);
    Pen be(g_pal.pillEdge, 1.f);
    g.DrawPath(&be, &bp);

    Font fb(L"Microsoft YaHei UI", FS(13.f), FontStyleBold, UnitPixel);
    RectF tm;
    g.MeasureString(tr(S_GHSTAR), -1, &fb, PointF(0, 0), &tm);
    float markX = btn.X + (btn.Width - (18.f + 6.f + tm.Width)) / 2.f + 9.f;
    DrawGitHubMark(g, markX, btn.Y + btn.Height / 2.f, 8.f, g_pal.text);
    StringFormat cen;
    cen.SetAlignment(StringAlignmentNear);
    cen.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(tr(S_GHSTAR), -1, &fb,
                 RectF(markX + 15.f, btn.Y, btn.Width, btn.Height), &cen, &tb);
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
    // 分组标题：折叠时页面只剩这 4 行文字，12px 经 UI_SCALE(0.86) 后仅约 10px、难以阅读。
    // 提到 16px（介于行标签 15px 与页眉 22px 之间）形成清晰层级；标题行高 SET_HH=34px
    // 与命中矩形不变，英文最长 "Mouse & keyboard" 也远窄于 314px 可用宽度，不会截断/换行/溢出。
    Font fsec(L"Microsoft YaHei UI", FS(16.f), FontStyleRegular, UnitPixel);
    StringFormat cen;
    cen.SetAlignment(StringAlignmentCenter);
    cen.SetLineAlignment(StringAlignmentCenter);
    StringFormat rgt;
    rgt.SetAlignment(StringAlignmentFar);
    rgt.SetLineAlignment(StringAlignmentCenter);

    RefreshSetLayout();
    Pen sep(g_pal.pillEdge, 1.f);
    // 分组标题（按视觉位置绘制）：通用 / 全局快捷键 / 鼠标与键盘 / 亮度与显示 / 关于；chevron 指示展开态
    static const int setTitles[5] = { S_GENERAL, S_HOTKEYS, S_MOUSESET, S_ABOUT, S_DISPLAYADV };
    for (int gi = 0; gi < 5; ++gi) {
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
    if (g_setFold & 1) {   // 通用：启动 / 界面
        Font fv12(L"Microsoft YaHei UI", FS(12.f), FontStyleBold, UnitPixel);
        g.DrawString(tr(S_AUTOSTART), -1, &fl, PointF(SET_L, SetRowY(0)), &tb);
        ToggleTrack(g, SetToggleRect(0).X, SetToggleRect(0).Y, g_cfgStart != 0, g_dark);

        // 语言（点击整行循环 跟随系统 → 中文 → English）
        g.DrawString(tr(S_LANG), -1, &fl, PointF(SET_L, SetRowY(1)), &tb);
        const wchar_t* lv = (g_langPref == 0) ? tr(S_FOLL) : (g_langPref == 1) ? L"中文" : L"English";
        g.DrawString(lv, -1, &fv, SetLangValueRect(), &rgt, &tb);

        // 第 7 行：字体大小（90–120%，步进 10%）
        g.DrawString(tr(S_FONTS), -1, &fl, PointF(SET_L, SetRowY(2)), &tb);
        swprintf(buf, 32, L"%d%%", g_fontPct);
        g.DrawString(buf, -1, &fv12, SetFontValueRect(), &rgt, &tb);
        RectF fm = SetFontMinusRect(), fp2 = SetFontPlusRect();
        MiniButton(g, fm.X, fm.Y, false);
        MiniButton(g, fp2.X, fp2.Y, true);

        // 第 11 行：任务栏数值贴片（可选）：无底框日/夜图标（跟随浅/深主题）、悬停名称+数值、滚轮调节、双击开面板
        g.DrawString(tr(S_BADGE), -1, &fl, PointF(SET_L, SetRowY(3)), &tb);
        ToggleTrack(g, SetToggleRect(3).X, SetToggleRect(3).Y, g_cfgBadge != 0, g_dark);
    }

    if (g_setFold & 16) {  // 亮度与显示：自动调度 / 设备高级 / HDR
        g.DrawString(tr(S_AUTOBR), -1, &fl, PointF(SET_L, SetAdvRowY(0)), &tb);
        ToggleTrack(g, SetAdvToggleRect(0).X, SetAdvToggleRect(0).Y, g_cfgAuto != 0, g_dark);

        Font fv13(L"Microsoft YaHei UI", FS(13.f), FontStyleBold, UnitPixel);
        Font fv12(L"Microsoft YaHei UI", FS(12.f), FontStyleBold, UnitPixel);   // 双列数值：12px 才放得下 "08:00" 不折行
        for (int i = 2; i <= 3; ++i) {
            // 左列：时间；右列：到点后的目标亮度，两列同排
            g.DrawString(i == 2 ? tr(S_STARTH) : tr(S_ENDH), -1, &fl, PointF(SET_L, SetAdvRowY(i-1)), &sb);
            g.DrawString(i == 2 ? tr(S_DAYBR) : tr(S_NIGHTBR), -1, &fs, PointF(216.f, SetAdvRowY(i-1) + 2), &sb);

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
        g.DrawString(tr(S_DDC), -1, &fl, PointF(SET_L, SetAdvRowY(3)), &tb);
        swprintf(buf, 32, L"%d ms", g_cfgDelay);
        g.DrawString(buf, -1, &fv13, SetDdcValue(), &rgt, &tb);
        RectF dm = SetDdcMinus(), dp = SetDdcPlus();
        MiniButton(g, dm.X, dm.Y, false);
        MiniButton(g, dp.X, dp.Y, true);
        g.DrawString(tr(S_STEP), -1, &fl, PointF(250.f, SetAdvRowY(3)), &tb);
        swprintf(buf, 32, L"%d", g_step);
        g.DrawString(buf, -1, &fv13, SetStepValue(), &rgt, &tb);
        RectF sm = SetStepMinus(), sp = SetStepPlus();
        MiniButton(g, sm.X, sm.Y, false);
        MiniButton(g, sp.X, sp.Y, true);

        // 第 8 行：软件调光（HDR 显示器用，自托盘迁入）
        g.DrawString(tr(S_GAMMA), -1, &fl, PointF(SET_L, SetAdvRowY(4)), &tb);
        ToggleTrack(g, SetAdvToggleRect(4).X, SetAdvToggleRect(4).Y, g_gammaDim != 0, g_dark);

        // 第 9 行：标定 HDR 基准亮度（整行点击，气泡报结果）
        g.DrawString(tr(S_CAL), -1, &fl, PointF(SET_L, SetAdvRowY(5)), &tb);

        // 第 10 行：精确熄屏（DDC 0xD6），默认关——MStar 方案显示器有永久损坏 OSD 的已知固件缺陷
        g.DrawString(tr(S_DDCKILL), -1, &fl, PointF(SET_L, SetAdvRowY(6)), &tb);
        ToggleTrack(g, SetAdvToggleRect(6).X, SetAdvToggleRect(6).Y, g_ddcOff != 0, g_dark);

        // 第 11 行：OLED 像素刷新提醒间隔
        g.DrawString(tr(S_OLEDREST), -1, &fl, PointF(SET_L, SetAdvRowY(7)), &tb);
        swprintf(buf, 32, g_oledH ? L"%d h" : L"off", g_oledH);
        g.DrawString(buf, -1, &fv13, SetOledValue(), &rgt, &tb);
        MiniButton(g, SetOledMinus().X, SetOledMinus().Y, false);
        MiniButton(g, SetOledPlus().X, SetOledPlus().Y, true);

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
            if (g_capSlot == i + 1) txt = tr(S_BINDHINT);
            else if (hv == 0)       txt = tr(S_UNBOUND);
            else                    txt = HotkeyText(hm, hv);
            SolidBrush unboundBrush(g_pal.sub);
            SolidBrush& tc = (g_capSlot == i + 1) ? acc : (hv == 0 ? unboundBrush : sb);
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

    if (g_setFold & 8) {   // 关于：GitHub 卡片（仅展开时显示，点击按钮打开项目主页）
        DrawGithubCard(g);
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
    BadgeInvalidate();              // 贴片数值/悬停设备名跟随当前选择
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
        if (g_page == 1) {                   // 设置页：分组标题悬停 + 底部 GitHub 按钮悬停
            int hh = -1;
            int sy = y + g_subScroll;
            RefreshSetLayout();
            for (int gi = 0; gi < 5; ++gi)
                if (InRect(SetHeaderRect(gi), x, sy)) { hh = gi; break; }
            if (hh != g_setHeaderHover) { g_setHeaderHover = hh; InvalidateRect(hwnd, nullptr, FALSE); }
            bool gh = (g_setFold & 8) && InRect(GithubBtnRect(), x, sy);
            if (gh != g_ghHover) {
                g_ghHover = gh;
                TouchPageCache();            // hover 高亮画在内容缓存层，需重建
                InvalidateRect(hwnd, nullptr, FALSE);
            }
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
        if (g_pressIdx != -1) { g_pressIdx = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        if (g_setHeaderHover != -1) { g_setHeaderHover = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        if (g_ghHover) { g_ghHover = false; TouchPageCache(); InvalidateRect(hwnd, nullptr, FALSE); }
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
                g_ghHover = false;
                PlacePanel();                // 回主面板
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            RefreshSetLayout();
            int sy = y + g_subScroll;    // 内容坐标：随页内滚动平移（返回按钮/状态行固定，用原 y）
            for (int gi = 0; gi < 5; ++gi) {
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
                    InvalidateBaseCache();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (InRect(SetToggleRect(3), x, sy)) {      // 任务栏数值贴片（可选）
                    g_cfgBadge = g_cfgBadge ? 0 : 1;
                    RegPut(L"TaskbarBadge", g_cfgBadge);
                    BadgeApplyEnabled();
                    InvalidateRect(hwnd, nullptr, FALSE);
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
            if (g_setFold & 16) {                // 亮度与显示
                if (InRect(SetAdvToggleRect(0), x, sy)) {
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
                if (InRect(SetAdvToggleRect(4), x, sy)) {       // 软件调光（自托盘迁入）
                    DoGammaToggle();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (InRect(SetCalWLRect(), x, sy)) {         // 标定 HDR 基准亮度（自托盘迁入）
                    DoCalWL();
                    return 0;
                }
                if (InRect(SetAdvToggleRect(6), x, sy)) {       // 精确熄屏（DDC 0xD6）
                    g_ddcOff = g_ddcOff ? 0 : 1;
                    RegPut(L"DdcScreenOff", g_ddcOff);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (InRect(SetOledMinus(), x, sy) || InRect(SetOledPlus(), x, sy)) {
                    bool plus = InRect(SetOledPlus(), x, sy);
                    int h = g_oledH;
                    if (plus) h = (h >= 12) ? 0 : h + 1;
                    else      h = (h <= 0) ? 12 : h - 1;
                    g_oledH = h;
                    RegPut(L"OledRemindH", g_oledH);
                    OledReset();                   // 改间隔后重新计时
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            if (g_setFold & 2) {                 // 全局快捷键
                for (int i = 0; i < 3; ++i) {
                    if (InRect(SetEditRect(i), x, sy) || InRect(SetKeyRect(i), x, sy)) {
                        if (g_capSlot == i + 1) StopCapture();
                        else StartCapture(i + 1);
                        return 0;
                    }
                }
            }
            // 关于组的 GitHub 卡片按钮：展开时点击打开项目主页
            if ((g_setFold & 8) && InRect(GithubBtnRect(), x, sy)) {
                ShellExecuteW(nullptr, L"open", L"https://github.com/iosvcnm/lumen", nullptr, nullptr, SW_SHOWNOACTIVATE);
                g_ghHover = false;
                return 0;
            }
            return 0;
        }

        int hv = HitButton(x, y);
        g_pressIdx = (hv >= 0) ? hv : -1;   // HitButton: 0=胶囊，1..4=第 i 个按钮
        if (hv == 0) {
            g_menuOpen = true;
            g_menuHover = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hv > 0) { InvalidateRect(hwnd, nullptr, FALSE); OnQuickAction(hv); return 0; }
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
        if (g_pressIdx != -1) { g_pressIdx = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        if (g_scrollDrag) { g_scrollDrag = false; ReleaseCapture(); return 0; }
        if (g_dragging) { g_dragging = false; ReleaseCapture(); }
        return 0;

    case WM_CAPTURECHANGED:
        g_scrollDrag = false;                // capture 被系统收回（如切页/失焦）时结束拖动
        g_pressIdx = -1;
        return 0;

    case WM_MOUSEWHEEL: {
        if (g_page >= 1) {                   // 子页：滚轮滚动页内内容，每档 ±104 逻辑 px（缓动到位）
            if (!g_scrollDrag) {
                int d = GET_WHEEL_DELTA_WPARAM(wp);
                if (d) {
                    int notches = d / WHEEL_DELTA;
                    if (!notches) notches = d > 0 ? 1 : -1;   // 高精度滚轮（delta < 120）也走一行
                    ScrollTo(g_subScroll - notches * 104);   // 每档 104 逻辑 px（原 52，滚一格太慢）
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
            if (wp == VK_ESCAPE) { int slot = g_capSlot; StopCapture(); UnbindHotkey(slot); return 0; } // Esc 解除绑定
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
        InvalidateBaseCache();               // 主面板静态层随 DPI 重建
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
    // 与主面板按钮同源：浅色纯太阳 / 深色月亮
    if (g_dark) DrawMoon(g, c, c, size * 0.26f, Color(255, 232, 192, 122));
    else        DrawCleanSun(g, c, c, size * 0.26f, Color(255, 122, 150, 186));

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

// ---------------------------------------------------------------- 任务栏数值贴片
// 静止时在托盘左侧画一枚日/夜状态小图标（浅色主题=太阳，深色主题=月亮），无底框；
// 悬停时图标出现托盘同款选中框，220ms 后在图标上方显示带圆角框的「设备名 + 当前亮度」。
// 滚轮调节（WM_INPUT，不依赖焦点），双击打开主面板。任务栏自动隐藏时贴片一起消失。

// 任务栏亮暗跟的是「Windows 模式」而不是应用自己的主题覆盖（SystemUsesLightTheme）
static bool TaskbarIsDark() {
    DWORD v = 1, n = sizeof(v), t = 0;
    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"SystemUsesLightTheme", RRF_RT_REG_DWORD, &t, &v, &n) == ERROR_SUCCESS)
        return v == 0;
    return g_dark;
}

static std::wstring BadgeMonitorName() {
    if (g_mons.empty()) return tr(S_NOCTRL);
    if (g_sel >= 0 && g_sel < (int)g_mons.size()) return g_mons[g_sel].name;
    wchar_t b[64];
    swprintf(b, 64, tr(S_ALLMONFMT), (int)g_mons.size());
    return b;
}

// 贴片读数与「选中屏」一致（启动时 g_val 是全部屏均值，面板打开后才对齐选中屏）
static int BadgeValue() {
    if (g_sel >= 0 && g_sel < (int)g_mons.size()) return g_mons[g_sel].pct;
    return g_val;
}

// 图标跟随 lumen 的浅色/深色主题（ThemeMode），颜色再按任务栏亮暗做对比补偿
static Color BadgeAccent() {
    bool tdark = TaskbarIsDark();
    if (g_dark)                                            // 夜间：月亮（琥珀）
        return tdark ? Color(255, 236, 190, 118) : Color(255, 170, 114, 46);
    return tdark ? Color(255, 174, 198, 228) : Color(255, 62, 92, 132);   // 日间：太阳（蓝）
}

static Color BadgeTextColor() {
    return TaskbarIsDark() ? Color(255, 236, 242, 251) : Color(255, 38, 54, 76);
}

// 公共 ULW 呈现：逐像素透明，没有底框；draw 在 96 DPI 逻辑坐标里绘制
template <typename DrawFn>
static bool PresentLayered(HWND hwnd, int w, int h, DrawFn draw) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;             // top-down，与 GDI+ 行序一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!mem || !dib) {
        if (mem) DeleteDC(mem);
        if (dib) DeleteObject(dib);
        ReleaseDC(hwnd, screen);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    {
        Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.ScaleTransform(g_badgeDpi / 96.0f, g_badgeDpi / 96.0f);
        g.Clear(Color(0, 0, 0, 0));
        draw(g);
    }
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    POINT src{ 0, 0 };
    SIZE size{ w, h };
    BOOL ok = UpdateLayeredWindow(hwnd, screen, nullptr, &size, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(hwnd, screen);
    return ok != FALSE;
}

static void BadgePresent(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    bool day = !g_dark;
    bool dark = TaskbarIsDark();
    Color c = BadgeAccent();
    bool hovered = g_badgeHovered;
    bool ok = PresentLayered(hwnd, w, h, [&](Graphics& g) {
        float cx = BADGE_WL / 2.f, cy = BADGE_HL / 2.f;
        if (hovered) {
            // 任务栏托盘图标同款悬停选中框：圆角半透明亮块 + 极淡描边
            const float bw = (float)BADGE_WL - 4.f, bh = (float)BADGE_HL - 4.f;
            GraphicsPath shp;
            RoundRect(shp, RectF(3.f, 4.f, bw, bh), 9.f);
            SolidBrush shb(Color(30, 0, 0, 0));
            g.FillPath(&shb, &shp);
            GraphicsPath box;
            RoundRect(box, RectF(2.f, 2.f, bw, bh), 9.f);
            SolidBrush bg(dark ? Color(40, 255, 255, 255) : Color(70, 255, 255, 255));
            g.FillPath(&bg, &box);
            Pen edge(dark ? Color(40, 255, 255, 255) : Color(55, 120, 140, 165), 1.f);
            g.DrawPath(&edge, &box);
        }
        if (day) DrawSun(g, cx, cy, 9.4f, c);
        else     DrawMoon(g, cx, cy, 9.2f, c);
    });
    if (ok) {
        g_badgeDrawnPhase = day ? 1 : 0;
        g_badgeDrawnDark = dark;
    }
}

// ---- 悬停读数（设备名 + 数值）：文字浮在图标上方，无底框，鼠标穿透 ----
static void BadgeInfoLayout(int* wl, int* hl) {
    // 同一字族/字号：GDI+ 的行居中对齐才能让名称与数值落在同一基线上
    Font fn(L"Microsoft YaHei UI", 13.f, FontStyleRegular, UnitPixel);
    Font fv(L"Microsoft YaHei UI", 13.f, FontStyleBold, UnitPixel);
    Bitmap probe(1, 1, PixelFormat32bppPARGB);
    Graphics mg(&probe);
    std::wstring name = BadgeMonitorName();
    wchar_t val[16];
    swprintf(val, 16, L"%d%%", BadgeValue());
    RectF rn{}, rv{};
    mg.MeasureString(name.c_str(), -1, &fn, PointF(0, 0), &rn);
    mg.MeasureString(val, -1, &fv, PointF(0, 0), &rv);
    float nw = std::min(rn.Width, 320.f);        // 超长显示器名在图内省略
    *wl = (int)ceilf(nw + 9.f + rv.Width) + 20;  // 左右各 10px 内边距（读数框）
    *hl = 29;                                    // 上下各 3px
}

static void BadgeInfoHide() {
    if (g_badgeInfo) ShowWindow(g_badgeInfo, SW_HIDE);
    g_badgeInfoShown = false;
    g_badgeInfoKey.clear();
}

static void BadgeInfoRefresh() {
    if (!g_badgeInfo || !g_badgeShown) return;
    int wl = 0, hl = 0;
    BadgeInfoLayout(&wl, &hl);
    float k = g_badgeDpi / 96.0f;
    int w = (int)ceilf(wl * k), h = (int)ceilf(hl * k);
    int margin = MulDiv(8, g_badgeDpi, 96);

    HMONITOR hm = MonitorFromWindow(g_badge, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) return;
    RECT mon = mi.rcMonitor;
    int x, y;
    int gap = MulDiv(3, g_badgeDpi, 96);
    if (g_badgeEdge == 1) {              // 顶部任务栏：放在任务栏下方（避免被任务栏压住）
        x = g_badgeRect.right - w;
        y = g_badgeTaskbarRect.bottom + gap;
    } else if (g_badgeEdge == 0) {       // 左侧任务栏：任务栏右侧
        x = g_badgeTaskbarRect.right + gap;
        y = g_badgeRect.top + (g_badgeRect.bottom - g_badgeRect.top - h) / 2;
    } else if (g_badgeEdge == 2) {       // 右侧任务栏：任务栏左侧
        x = g_badgeTaskbarRect.left - w - gap;
        y = g_badgeRect.top + (g_badgeRect.bottom - g_badgeRect.top - h) / 2;
    } else {                             // 底部任务栏：任务栏上方
        x = g_badgeRect.right - w;
        y = g_badgeTaskbarRect.top - h - gap;
    }
    if (x < mon.left + margin) x = mon.left + margin;
    if (x + w > mon.right - margin) x = mon.right - margin - w;
    if (y < mon.top + margin) y = mon.top + margin;
    if (y + h > mon.bottom - margin) y = mon.bottom - margin - h;

    RECT cur{};
    GetWindowRect(g_badgeInfo, &cur);
    bool sizeChanged = cur.right - cur.left != w || cur.bottom - cur.top != h;
    if (sizeChanged || cur.left != x || cur.top != y)
        SetWindowPos(g_badgeInfo, HWND_TOPMOST, x, y, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);

    std::wstring key = BadgeMonitorName();
    wchar_t val[16];
    swprintf(val, 16, L"%d%%", BadgeValue());
    key += L'\x1f';
    key += val;
    key += TaskbarIsDark() ? L'T' : L't';
    key += g_dark ? L'D' : L'd';
    if (sizeChanged || key != g_badgeInfoKey) {
        g_badgeInfoKey = key;
        InvalidateRect(g_badgeInfo, nullptr, FALSE);
    }
    ShowWindow(g_badgeInfo, SW_SHOWNOACTIVATE);
    g_badgeInfoShown = true;
}

static bool BadgeInfoCreate() {
    if (g_badgeInfo) return true;
    g_badgeInfo = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        kBadgeInfoClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_inst, nullptr);
    return g_badgeInfo != nullptr;
}

static void BadgeInfoShow() {
    if (!g_badgeShown || !BadgeInfoCreate()) return;
    g_badgeInfoShown = true;             // 先置位：Refresh 里才会触发重绘
    BadgeInfoRefresh();
}

static void BadgeInfoPresent(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    std::wstring name = BadgeMonitorName();
    wchar_t val[16];
    swprintf(val, 16, L"%d%%", BadgeValue());
    Font fn(L"Microsoft YaHei UI", 13.f, FontStyleRegular, UnitPixel);
    Font fv(L"Microsoft YaHei UI", 13.f, FontStyleBold, UnitPixel);
    Bitmap probe(1, 1, PixelFormat32bppPARGB);
    Graphics mg(&probe);
    RectF rn{}, rv{};
    mg.MeasureString(name.c_str(), -1, &fn, PointF(0, 0), &rn);
    mg.MeasureString(val, -1, &fv, PointF(0, 0), &rv);
    float nw = std::min(rn.Width, 320.f);

    PresentLayered(hwnd, w, h, [&](Graphics& g) {
        // 读数选中框：圆角亮/暗块 + 细描边 + 极淡落影，和任务栏托盘悬停框同风格
        float boxW = nw + 9.f + rv.Width + 20.f, boxH = 29.f;
        bool dark = TaskbarIsDark();
        GraphicsPath shp;
        RoundRect(shp, RectF(1.f, 2.f, boxW - 2.f, boxH - 2.f), 10.f);
        SolidBrush shb(Color(35, 0, 0, 0));
        g.FillPath(&shb, &shp);
        GraphicsPath box;
        RoundRect(box, RectF(0.5f, 0.5f, boxW - 1.f, boxH - 1.f), 10.f);
        SolidBrush bg(dark ? Color(160, 34, 46, 62) : Color(155, 250, 252, 255));
        g.FillPath(&bg, &box);
        Pen edge(dark ? Color(70, 100, 120, 150) : Color(75, 150, 168, 190), 1.f);
        g.DrawPath(&edge, &box);

        StringFormat lf;
        lf.SetLineAlignment(StringAlignmentCenter);
        lf.SetFormatFlags(StringFormatFlagsNoWrap);
        lf.SetTrimming(StringTrimmingEllipsisCharacter);
        RectF nr(10.f, 3.f, nw, 23.f);
        RectF vr(10.f + nw + 9.f, 3.f, rv.Width, 23.f);
        SolidBrush nb(BadgeTextColor());
        SolidBrush vb(BadgeAccent());
        g.DrawString(name.c_str(), -1, &fn, nr, &lf, &nb);
        g.DrawString(val, -1, &fv, vr, &lf, &vb);
    });
}

static LRESULT CALLBACK BadgeInfoProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        BadgeInfoPresent(hwnd);
        return 0;
    }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return HTTRANSPARENT;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK BadgeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);      // ULW：WM_PAINT 只清无效区，内容由 BadgePresent 呈现
        BadgePresent(hwnd);
        return 0;
    }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:          // 点击由低级鼠标钩子统一处理（任务栏可能盖在贴片上面）
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 贴片是否还在任务栏上方（任务栏自己会升到 TOPMOST 最上层）
static bool BadgeAbove(HWND a, HWND b) {
    if (!a || !b) return true;
    for (HWND w = GetWindow(a, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV))
        if (w == b) return true;
    return false;
}

static void BadgeInvalidate() {
    if (g_badge) InvalidateRect(g_badge, nullptr, FALSE);
    if (g_badgeInfoShown) BadgeInfoRefresh();
}

static void BadgeDestroy() {
    if (g_badgeInfo) { DestroyWindow(g_badgeInfo); g_badgeInfo = nullptr; }
    if (g_badge) { DestroyWindow(g_badge); g_badge = nullptr; }
    g_badgeShown = false;
    g_badgeHovered = false;
    g_badgeInfoShown = false;
    g_badgeDrawnPhase = -1;
    g_badgeInfoKey.clear();
    g_badgeRect = RECT{};
    g_badgeTaskbarRect = RECT{};
    g_badgeHoverSince = 0;
    g_badgeLastClick = 0;
}

// 任务栏当前矩形 + DPI + 可见比例 + 所在屏幕；完全不可见时返回 false。
// 自动隐藏动画期间任务栏逐帧移动，贴片按当前矩形跟手移动（不再等 60% 才一刀切）。
static bool TaskbarRect(RECT* trOut, int* dpiOut, float* visFrac, RECT* monOut) {
    HWND shell = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!shell || !IsWindowVisible(shell)) return false;
    RECT tr{};
    if (!GetWindowRect(shell, &tr)) return false;
    HMONITOR hm = MonitorFromWindow(shell, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) return false;
    RECT vis{};
    if (!IntersectRect(&vis, &tr, &mi.rcMonitor)) return false;
    int tw = tr.right - tr.left, th = tr.bottom - tr.top;
    if (tw <= 0 || th <= 0) return false;
    bool vertical = tw < th;
    int seen = vertical ? (vis.right - vis.left) : (vis.bottom - vis.top);
    int total = vertical ? tw : th;
    if (seen <= 0) return false;
    *visFrac = (float)seen / (float)total;
    int dpi = 96;
    typedef UINT(WINAPI* PFN)(HWND);
    static PFN fn = (PFN)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn) {
        UINT d = fn(shell);
        if (d >= 96) dpi = (int)d;
    }
    *trOut = tr;
    *dpiOut = dpi;
    *monOut = mi.rcMonitor;
    return true;
}

static bool BadgeCreate() {
    if (g_badge) return true;
    g_badge = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kBadgeClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_inst, nullptr);
    if (!g_badge) return false;
    g_badgeDrawnPhase = -1;
    return true;
}

static void BadgeHideNow() {
    g_badgeHoverSince = 0;
    g_badgeHovered = false;
    g_badgeLastClick = 0;
    BadgeInfoHide();
    if (g_badge && g_badgeShown) {
        ShowWindow(g_badge, SW_HIDE);
        g_badgeShown = false;
    }
}

static void BadgeMaintain() {
    if (!g_cfgBadge) { BadgeDestroy(); return; }

    RECT tr{}, mon{};
    int dpi = 96;
    float visFrac = 0.f;
    if (!TaskbarRect(&tr, &dpi, &visFrac, &mon)) {   // 完全收起：贴片一起消失
        BadgeHideNow();
        return;
    }
    if (!BadgeCreate()) return;

    bool redraw = !g_badgeShown || dpi != g_badgeDpi;
    if (dpi != g_badgeDpi) {
        g_badgeDpi = dpi;
        g_badgeDrawnPhase = -1;
        g_badgeInfoKey.clear();
    }

    int bw = MulDiv(BADGE_WL, dpi, 96);
    int bh = MulDiv(BADGE_HL, dpi, 96);
    int gap = MulDiv(10, dpi, 96);
    int trW = tr.right - tr.left, trH = tr.bottom - tr.top;
    bool vertical = trW < trH;
    HWND shell = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND tn = shell ? FindWindowExW(shell, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    RECT nr{};
    bool hasTray = tn && GetWindowRect(tn, &nr);

    int x, y;
    if (!vertical) {                              // 横向任务栏：托盘区左侧
        int anchor = hasTray ? (int)nr.left : tr.right - MulDiv(220, dpi, 96);
        x = anchor - gap - bw;
        y = tr.top + (trH - bh) / 2;
        if (x < tr.left + gap / 2) x = tr.left + gap / 2;
    } else {                                      // 纵向任务栏：托盘区上方
        int anchor = hasTray ? (int)nr.top : tr.bottom - MulDiv(220, dpi, 96);
        y = anchor - gap - bh;
        x = tr.left + (trW - bw) / 2;
        if (y < tr.top + gap / 2) y = tr.top + gap / 2;
    }

    RECT want{ x, y, x + bw, y + bh };
    RECT isect{};
    if (!IntersectRect(&isect, &want, &mon)) {       // 贴片已跟着任务栏滑出屏幕
        BadgeHideNow();
        return;
    }

    RECT cur{};
    GetWindowRect(g_badge, &cur);
    bool moved = cur.left != x || cur.top != y ||
                 cur.right - cur.left != bw || cur.bottom - cur.top != bh;
    bool wasShown = g_badgeShown;
    if (moved) {
        SetWindowPos(g_badge, HWND_TOPMOST, x, y, bw, bh,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (wasShown) g_badgeInfoKey.clear();
    } else if (!g_badgeShown) {
        SetWindowPos(g_badge, HWND_TOPMOST, x, y, bw, bh,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    g_badgeShown = true;
    g_badgeRect = want;
    g_badgeTaskbarRect = tr;

    // 任务栏在哪条边：决定悬停读数显示在图标的上/下/左/右
    g_badgeEdge = 3;
    if (tr.top <= mon.top + 2)           g_badgeEdge = 1;
    else if (tr.bottom >= mon.bottom - 2) g_badgeEdge = 3;
    else if (tr.left <= mon.left + 2)     g_badgeEdge = 0;
    else                                  g_badgeEdge = 2;

    if (shell && !BadgeAbove(g_badge, shell))     // 任务栏/其他 TOPMOST 升上来后把贴片抬回最上
        SetWindowPos(g_badge, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // 任务栏开始收起时读数立即消失，避免盖在正在下滑的任务栏上
    if (visFrac < 0.9f && g_badgeInfoShown) BadgeInfoHide();
    if (g_badgeInfoShown && moved) BadgeInfoRefresh();

    DWORD now = GetTickCount();

    // 悬停读数用光标轮询：任务栏在前台时会盖住贴片的命中层，收不到 WM_MOUSEMOVE
    POINT pt{};
    GetCursorPos(&pt);
    bool over = visFrac >= 0.9f && PtInRect(&g_badgeRect, pt);
    if (over != g_badgeHovered) {
        g_badgeHovered = over;
        InvalidateRect(g_badge, nullptr, FALSE);   // 选中框立即出现/消失
    }
    if (over) {
        if (!g_badgeHoverSince) g_badgeHoverSince = now ? now : 1;
        if (!g_badgeInfoShown && now - g_badgeHoverSince >= 220) BadgeInfoShow();
    } else {
        g_badgeHoverSince = 0;
        if (g_badgeInfoShown) BadgeInfoHide();
    }

    if (redraw) InvalidateRect(g_badge, nullptr, FALSE);

    // 慢项（主题/设备名/数值兜底）限频 500ms；亮度变化走 BadgeInvalidate 即时刷新
    if (now - g_badgeSlowTick >= 500) {
        g_badgeSlowTick = now;
        bool dark = TaskbarIsDark();
        bool day = !g_dark;
        if (dark != g_badgeDrawnDark || (day ? 1 : 0) != g_badgeDrawnPhase)
            InvalidateRect(g_badge, nullptr, FALSE);
        if (g_badgeInfoShown) BadgeInfoRefresh();
    }

    if (g_badgeInfoShown && shell && !BadgeAbove(g_badgeInfo, shell))
        SetWindowPos(g_badgeInfo, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static bool BadgeHitTest(POINT pt) {
    if (!g_badge || !g_cfgBadge || !g_badgeShown) return false;
    RECT rc{};
    if (!GetWindowRect(g_badge, &rc)) return false;
    return PtInRect(&rc, pt) != FALSE;
}

static void BadgeApplyEnabled() {
    if (!g_msg) return;
    if (!g_cfgBadge) {
        BadgeDestroy();
        KillTimer(g_msg, 5);
        return;
    }
    SetTimer(g_msg, 5, 16, nullptr);    // ~60Hz：任务栏自动隐藏动画每 ~31ms 走一步，贴片按同节奏跟随
    BadgeMaintain();
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
    // 任务栏贴片：点击全部由钩子接管并吞掉。任务栏在前台时会盖住贴片的命中层，
    // 钩子仍能拿到点击；双击打开面板，也避免任务栏抢前台/误触底下的任务按钮。
    if (code == HC_ACTION && g_msg && (wp == WM_LBUTTONDOWN || wp == WM_LBUTTONUP)) {
        auto* ms = (MOUSEHOOKSTRUCTEX*)lp;
        bool inBadge = g_badgeShown && !g_calibrating && PtInRect(&g_badgeRect, ms->pt);
        if (inBadge) {
            if (wp == WM_LBUTTONDOWN) {
                DWORD now = GetTickCount();
                int tol = 8;
                if (g_badgeLastClick &&
                    now - g_badgeLastClick <= (DWORD)GetDoubleClickTime() &&
                    abs(ms->pt.x - g_badgeLastClickPt.x) <= tol &&
                    abs(ms->pt.y - g_badgeLastClickPt.y) <= tol) {
                    g_badgeLastClick = 0;
                    g_badgeHoverSince = 0;
                    PostMessageW(g_msg, WM_BADGEOPEN, 0, 0);
                } else {
                    g_badgeLastClick = now;
                    g_badgeLastClickPt = ms->pt;
                }
            }
            return 1;
        }
        if (wp == WM_LBUTTONDOWN) g_badgeLastClick = 0;   // 在贴片外按下：打断双击序列
    }
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
                if (d) {
                    POINT pt;
                    GetCursorPos(&pt);
                    if (BadgeHitTest(pt)) {
                        // 贴片悬停滚轮：读数就在眼前，不再弹 OSD；按实际格数即时生效
                        int steps = (d / WHEEL_DELTA) * g_step;
                        if (!steps) steps = d > 0 ? 1 : -1;   // 高精度滚轮（delta < 120）
                        Adjust(steps);                       // 按当前选中屏的实时值加减
                        BadgeInfoShow();                     // 滚动时把「名称 + 新数值」亮出来
                    } else if (g_wheelTray) {
                        RECT rc{};
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

    case WM_BADGEOPEN:                    // 双击任务栏贴片
        BadgeInfoHide();
        g_badgeHoverSince = 0;
        ShowPanel();
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
        if (wp == 11) {                          // 屏保轮询：输入时间戳变化即判定屏保退出
            if (!g_saverTick) { KillTimer(g_msg, 11); return 0; }
            LASTINPUTINFO li{};
            li.cbSize = sizeof(li);
            if (GetLastInputInfo(&li) && li.dwTime != g_saverInputBase &&
                GetTickCount() - g_saverTick >= 1000) {
                KillTimer(g_msg, 11);
                g_saverTick = 0;
                SetTimer(g_msg, 13, 700, nullptr);   // 等显示稳定后强制重放
            }
            return 0;
        }
        if (wp == 13) {                          // 屏保退出/唤醒后：恢复记忆亮度
            KillTimer(g_msg, 13);
            g_dispChangeTick = 0;                // 取消 gamma 写入静默期
            g_writer.sickUntil.store(0);         // 清除 DDC 写入冷却期
            DropDcCache();                       // 休眠后 DC 可能失效，重建
            g_mons = Scan();
            ApplyNames(g_mons);
            MatchSavedSelection();
            RestoreBrightness(true);             // 强制重放：不能依赖差值判断
            OverlayPrune();
            if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
            return 0;
        }
        if (wp == 2) ApplyAutoBrightness();
        if (wp == 7) AutoRampTick();
        if (wp == 8) ScrollAnimTick();
        if (wp == 5) {
            BadgeMaintain();                        // 任务栏贴片：位置/显隐/DPI/主题/设备名同步
            OledTick();                             // OLED 连续亮屏计时（静默，满间隔才弹一次气泡）
        }
        if (wp == 4 && RefreshHdrFlags(g_mons) && g_hwnd)
            InvalidateRect(g_hwnd, nullptr, FALSE);   // HDR 开/关 → 徽章自动出现/消失
        return 0;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_SCREENSAVE) {
            OledReset();                   // OLED 提醒：屏保视为熄屏
            LASTINPUTINFO li{};
            li.cbSize = sizeof(li);
            g_saverInputBase = GetLastInputInfo(&li) ? li.dwTime : 0;
            g_saverTick = GetTickCount();
            SetTimer(g_msg, 11, 1000, nullptr);  // 轮询输入以判定屏保退出
            return 0;
        }
        break;

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
        if (msg == WM_POWERBROADCAST &&
            (wp == PBT_APMSUSPEND || wp == PBT_APMRESUMESUSPEND || wp == PBT_APMRESUMEAUTOMATIC)) {
            OledReset();                   // OLED 提醒：睡眠/唤醒都按新的一段亮屏计
            // 休眠唤醒：显示器常把亮度寄存器复位，延迟强制重放一次
            KillTimer(g_msg, 13);
            SetTimer(g_msg, 13, 700, nullptr);
        }
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
        KillTimer(g_msg, 5);
        BadgeDestroy();
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
        if (m == WM_DISPLAYCHANGE || m == WM_POWERBROADCAST || m == WM_SETTINGCHANGE ||
            (m == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_SCREENSAVE)) {
            // message-only 窗口收不到广播：主题/DPI 变化也经这里转给主消息窗口。
            // WM_SETTINGCHANGE 的 lParam 可能是系统地址空间里的字符串，转发时清空避免误读；
            // WM_SYSCOMMAND 的 lParam 同理，屏保广播一律清零。
            bool clearLp = (m == WM_SETTINGCHANGE || m == WM_SYSCOMMAND);
            if (g_msg) PostMessageW(g_msg, m, wp, clearLp ? 0 : lp);
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

    c.lpfnWndProc = BadgeProc;                  // 任务栏贴片：双击靠 CS_DBLCLKS 生成 WM_LBUTTONDBLCLK
    c.lpszClassName = kBadgeClass;
    c.hCursor = LoadCursorW(nullptr, IDC_HAND);
    c.style = CS_DBLCLKS;
    RegisterClassExW(&c);
    c.style = 0;

    c.lpfnWndProc = BadgeInfoProc;              // 悬停读数：无底框文字，鼠标穿透
    c.lpszClassName = kBadgeInfoClass;
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
    BadgeApplyEnabled();          // 任务栏数值贴片（可选，默认关；跟任务栏显隐）
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
