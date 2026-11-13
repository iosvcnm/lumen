// 开发辅助：设置页滚动后点击回归 + 通用「点击即截图」驱动
//   sclick seq <shot1.bmp> <shot2.bmp>   进设置页 → 滚 3 档 → 截图 → 点滚动后应命中的开关 → 截图 → 读 WheelTray
//   sclick click <x> <y> [shot.bmp]      面板物理坐标假点击（PostMessage，不抢焦点），可选截图
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static void Click(HWND h, int x, int y) {
    LPARAM lp = MAKELPARAM(x, y);
    PostMessageW(h, WM_MOUSEMOVE, 0, lp);
    PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    PostMessageW(h, WM_LBUTTONUP, 0, lp);
}

static void Wheel(HWND h, int delta) {
    PostMessageW(h, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)delta), 0);
}

static void Shot(HWND h, const wchar_t* out) {
    RECT rc; GetWindowRect(h, &rc);
    int w = rc.right - rc.left, ht = rc.bottom - rc.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, ht);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, ht, screen, rc.left, rc.top, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = ht;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    DWORD size = (DWORD)w * 4 * ht;
    BYTE* bits = (BYTE*)malloc(size);
    GetDIBits(mem, bmp, 0, ht, bits, &bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + size;
    FILE* f = _wfopen(out, L"wb");
    if (f) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
        fwrite(bits, size, 1, f);
        fclose(f);
        wprintf(L"shot %ls %dx%d\n", out, w, ht);
    }
    free(bits);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static int RegRead(const wchar_t* name) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\lumen", 0, KEY_READ, &k) != ERROR_SUCCESS) return -1;
    DWORD v = 0, n = sizeof(DWORD), t = REG_DWORD;
    LONG r = RegQueryValueExW(k, name, nullptr, &t, (BYTE*)&v, &n);
    RegCloseKey(k);
    return r == ERROR_SUCCESS ? (int)v : -1;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    HWND h = FindWindowW(L"LumenPanelWnd", nullptr);
    if (!h) { wprintf(L"NOPANEL\n"); return 3; }

    if (!_wcsicmp(argv[1], L"seq")) {
        if (argc < 4) return 2;
        Click(h, 198, 454);                       // 主面板第 3 个快捷按钮 → 设置页
        Sleep(400);
        Wheel(h, -120); Wheel(h, -120); Wheel(h, -120);
        Sleep(500);                               // 缓动到位
        Shot(h, argv[2]);
        Click(h, 294, 287);                       // 滚 156 逻辑 px 后托盘滚轮开关应在 (294,287) 物理
        Sleep(400);
        Shot(h, argv[3]);
        wprintf(L"WheelTray=%d\n", RegRead(L"WheelTray"));
    } else if (!_wcsicmp(argv[1], L"stepper")) {
        if (argc < 3) return 2;
        Click(h, 198, 454);                       // 设置页
        Sleep(400);
        Click(h, 183, 304);                       // DDC 间隔 +（逻辑 (213,354)）
        Click(h, 161, 216);                       // 开始时间 +（逻辑 (187,251)）
        Click(h, 300, 216);                       // 亮度差 +（逻辑 (349,251)）
        Sleep(400);
        Shot(h, argv[2]);
        wprintf(L"DdcDelay=%d AutoStartH=%d DayPct=%d Step=%d\n",
                RegRead(L"DdcDelay"), RegRead(L"AutoStartH"), RegRead(L"DayPct"), RegRead(L"Step"));
    } else if (!_wcsicmp(argv[1], L"wheeln")) {
        if (argc < 4) return 2;
        Click(h, 198, 454);                       // 重开设置页（滚动归零）
        Sleep(400);
        int n = _wtoi(argv[2]);
        for (int i = 0; i < n; ++i) { Wheel(h, -120); Sleep(350); }   // 每档等缓动完成
        Sleep(300);
        Shot(h, argv[3]);
        wprintf(L"wheels=%d done\n", n);
    } else if (!_wcsicmp(argv[1], L"seq2")) {
        if (argc < 4) return 2;
        Click(h, 198, 454);                       // 设置页
        Sleep(400);
        Click(h, 172, 395);                       // 展开「鼠标与键盘」组（hdr2 位于 (200,460) 逻辑）
        Sleep(400);
        Wheel(h, -120); Wheel(h, -120); Wheel(h, -120);   // 滚 156 逻辑 px
        Sleep(500);
        Shot(h, argv[2]);
        Click(h, 294, 287);                       // 滚动后托盘滚轮开关应在此
        Sleep(400);
        Shot(h, argv[3]);
        wprintf(L"WheelTray=%d\n", RegRead(L"WheelTray"));
    } else if (!_wcsicmp(argv[1], L"wheelv")) {
        if (argc < 3) return 2;
        int d = _wtoi(argv[2]);
        Wheel(h, d);
        wprintf(L"wheelv %d sent\n", d);
        if (argc > 3) { Sleep(600); Shot(h, argv[3]); }
    } else if (!_wcsicmp(argv[1], L"click")) {
        if (argc < 4) return 2;
        int x = _wtoi(argv[2]), y = _wtoi(argv[3]);
        Click(h, x, y);
        wprintf(L"clicked %d,%d\n", x, y);
        if (argc > 4) { Sleep(400); Shot(h, argv[4]); }
    }
    return 0;
}
