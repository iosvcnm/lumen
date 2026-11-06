// 开发辅助：统计 BMP 中"文字像素"的平均色，用于核对前景色是否被错误分支覆盖
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: px file.bmp\n"); return 1; }
    HANDLE f = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { wprintf(L"open fail\n"); return 1; }
    DWORD sz = GetFileSize(f, nullptr);
    BYTE* buf = (BYTE*)malloc(sz);
    DWORD got = 0;
    ReadFile(f, buf, sz, &got, nullptr);
    CloseHandle(f);
    BITMAPFILEHEADER* fh = (BITMAPFILEHEADER*)buf;
    BITMAPINFOHEADER* ih = (BITMAPINFOHEADER*)(buf + sizeof(BITMAPFILEHEADER));
    BYTE* px = buf + fh->bfOffBits;
    int bpp = ih->biBitCount / 8;
    long long r = 0, g = 0, b = 0, n = 0, bright = 0;
    for (int y = 0; y < ih->biHeight; ++y) {
        for (int x = 0; x < ih->biWidth; ++x) {
            BYTE* p = px + ((size_t)y * ih->biWidth + x) * bpp;
            int B = p[0], G = p[1], R = p[2];
            int lum = (R * 30 + G * 59 + B * 11) / 100;
            if (lum > 110) { r += R; g += G; b += B; ++n; }
            else if (lum > 40) ++bright;
        }
    }
    if (n == 0) { wprintf(L"no bright pixels\n"); return 0; }
    wprintf(L"text pixels=%lld  avg R=%lld G=%lld B=%lld   R-G=%lld\n",
            n, r / n, g / n, b / n, (r - g) / n);
    return 0;
}
