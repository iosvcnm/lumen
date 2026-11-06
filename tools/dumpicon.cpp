// 开发辅助：提取 exe 图标存为 BMP，检查是否嵌入正确
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

static bool SaveBmp(HBITMAP hb, const wchar_t* path) {
    BITMAP bm{};
    GetObjectW(hb, sizeof(bm), &bm);
    int w = bm.bmWidth, h = bm.bmHeight;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    DWORD size = (DWORD)w * 4 * h;
    BYTE* bits = (BYTE*)malloc(size);
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, hb, 0, h, bits, &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (!lines) { free(bits); return false; }

    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + size;
    FILE* f = _wfopen(path, L"wb");
    if (!f) { free(bits); return false; }
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
    fwrite(bits, size, 1, f);
    fclose(f);
    free(bits);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* exe = (argc > 1) ? argv[1] : L"lumen.exe";
    HICON lg = nullptr, sm = nullptr;
    UINT n = ExtractIconExW(exe, 0, &lg, &sm, 1);
    if (n == 0 || !lg) { wprintf(L"no icon\n"); return 1; }

    ICONINFO ii{};
    if (GetIconInfo(lg, &ii)) {
        if (ii.hbmColor && SaveBmp(ii.hbmColor, L"icon_check.bmp")) wprintf(L"ok\n");
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
    }
    DestroyIcon(lg);
    if (sm) DestroyIcon(sm);
    return 0;
}
