// 开发辅助：托盘菜单「亮度+10」弹出 OSD，拖动 OSD 到目标坐标，验证拖动记忆
// 用法:
//   osdtool menu               右键托盘图标弹菜单，抓菜单区域 BMP
//   osdtool drag <itemX> <itemY> <toX> <toY>
//                              点「亮度+10」弹 OSD → 拖到目标 → 输出前后矩形
//   osdtool recheck <itemX> <itemY>
//                              再弹一次 OSD，输出矩形（应为拖动后的记忆位置）
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static void Click(int x, int y, bool right) {
    SetCursorPos(x, y);
    Sleep(120);
    INPUT in[2]{};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

static void Shot(const wchar_t* path, int x, int y, int w, int h) {
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    DWORD size = (DWORD)w * 4 * h;
    BYTE* bits = (BYTE*)malloc(size);
    GetDIBits(mem, bmp, 0, h, bits, &bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER); fh.bfSize = fh.bfOffBits + size;
    FILE* f = _wfopen(path, L"wb");
    if (f) { fwrite(&fh, sizeof(fh), 1, f); fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f); fwrite(bits, size, 1, f); fclose(f); }
    free(bits);
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
    wprintf(L"%ls %dx%d at %d,%d\n", path, w, h, x, y);
}

// 弹 OSD：托盘右键 → 点「亮度+10」菜单项
static void ShowOsdViaMenu(int itemX, int itemY) {
    Click(2261, 1409, true);       // 托盘图标（TrayX/TrayY 校准坐标）
    Sleep(250);
    Click(itemX, itemY, false);
}

static void DragOsd(int toX, int toY) {
    HWND osd = FindWindowW(L"LumenOsdWnd", nullptr);
    if (!osd) { wprintf(L"OSD not visible\n"); exit(1); }
    RECT r; GetWindowRect(osd, &r);
    wprintf(L"before: %ld,%ld - %ld,%ld\n", r.left, r.top, r.right, r.bottom);
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    SetCursorPos(cx, cy); Sleep(120);
    INPUT d{}; d.type = INPUT_MOUSE; d.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; SendInput(1, &d, sizeof(INPUT));
    Sleep(120);
    for (int i = 1; i <= 20; ++i) {
        SetCursorPos(cx + (toX - cx) * i / 20, cy + (toY - cy) * i / 20);
        Sleep(15);
    }
    Sleep(120);
    d.mi.dwFlags = MOUSEEVENTF_LEFTUP; SendInput(1, &d, sizeof(INPUT));
    Sleep(200);
    GetWindowRect(osd, &r);
    wprintf(L"after:  %ld,%ld - %ld,%ld\n", r.left, r.top, r.right, r.bottom);
}

// 主面板第一个快捷按钮（主题）每次点击内部调 ShowOsdMode → 弹 OSD
// 连点 3 次 = 主题循环回原状态，每次都得到一个可拖动的 OSD
static void ThemeCycle(int btnX, int btnY, int toX, int toY) {
    for (int c = 1; c <= 3; ++c) {
        Click(btnX, btnY, false);
        Sleep(300);
        HWND osd = FindWindowW(L"LumenOsdWnd", nullptr);
        if (!osd) { wprintf(L"cycle %d: OSD not visible\n", c); continue; }
        RECT r; GetWindowRect(osd, &r);
        wprintf(L"cycle %d before: %ld,%ld\n", c, r.left, r.top);
        DragOsd(toX, toY);
        Sleep(2000);           // 等自动隐藏，下一轮重新弹
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: osdtool menu | drag <ix> <iy> <tx> <ty> | recheck <ix> <iy>\n"); return 2; }
    if (!wcscmp(argv[1], L"theme") && argc >= 6) {
        ThemeCycle(_wtoi(argv[2]), _wtoi(argv[3]), _wtoi(argv[4]), _wtoi(argv[5]));
        return 0;
    }
    if (!wcscmp(argv[1], L"menu")) {
        Click(2261, 1409, true);   // 托盘右键 → 菜单保持打开
        Sleep(300);
        Shot(L"C:/Users/Eliauk/AppData/Local/Temp/lumen-ui/traymenu.bmp", 0, 0,
             GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        Click(200, 200, false);    // 点菜单外收起
        return 0;
    }
    int ix = _wtoi(argv[2]), iy = _wtoi(argv[3]);
    if (!wcscmp(argv[1], L"drag")) {
        ShowOsdViaMenu(ix, iy);
        Sleep(300);
        DragOsd(_wtoi(argv[4]), _wtoi(argv[5]));
        Sleep(2000);               // 等自动隐藏
        ShowOsdViaMenu(ix, iy);    // 再弹一次：应在记忆位置
        Sleep(300);
        HWND osd = FindWindowW(L"LumenOsdWnd", nullptr);
        if (!osd) { wprintf(L"OSD not visible (2nd)\n"); return 1; }
        RECT r; GetWindowRect(osd, &r);
        wprintf(L"recall: %ld,%ld - %ld,%ld\n", r.left, r.top, r.right, r.bottom);
        return 0;
    }
    if (!wcscmp(argv[1], L"recheck")) {
        ShowOsdViaMenu(ix, iy);
        Sleep(300);
        HWND osd = FindWindowW(L"LumenOsdWnd", nullptr);
        if (!osd) { wprintf(L"OSD not visible\n"); return 1; }
        RECT r; GetWindowRect(osd, &r);
        wprintf(L"recall: %ld,%ld - %ld,%ld\n", r.left, r.top, r.right, r.bottom);
        return 0;
    }
    wprintf(L"unknown cmd\n");
    return 2;
}