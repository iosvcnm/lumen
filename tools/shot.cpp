// 开发辅助：抓屏或抓指定区域，输出 BMP
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, wchar_t** argv) {
    SetProcessDPIAware();
    const wchar_t* out = (argc > 1) ? argv[1] : L"shot.bmp";
    int x = 0, y = 0;
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    if (argc >= 6) {
        x = _wtoi(argv[2]); y = _wtoi(argv[3]);
        w = _wtoi(argv[4]); h = _wtoi(argv[5]);
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    DWORD size = (DWORD)w * 4 * h;
    BYTE* bits = (BYTE*)malloc(size);
    GetDIBits(mem, bmp, 0, h, bits, &bi, DIB_RGB_COLORS);

    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + size;

    FILE* f = _wfopen(out, L"wb");
    if (!f) return 1;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
    fwrite(bits, size, 1, f);
    fclose(f);

    wprintf(L"%ls  %dx%d at %d,%d\n", out, w, h, x, y);
    free(bits);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return 0;
}
