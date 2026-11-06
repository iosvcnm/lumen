// 开发辅助：从 BMP 截图中裁剪区域另存 BMP，用于 UI 回归查看（纯 GDI，无依赖）
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, wchar_t** argv) {
    if (argc < 7) {
        fwprintf(stderr, L"usage: crop.exe in.bmp out.bmp x y w h\n");
        return 2;
    }
    HANDLE hf = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (hf == INVALID_HANDLE_VALUE) { fwprintf(stderr, L"open in failed\n"); return 3; }
    DWORD size = GetFileSize(hf, nullptr);
    char* buf = (char*)malloc(size);
    DWORD rd = 0;
    if (!ReadFile(hf, buf, size, &rd, nullptr) || rd != size) {
        fwprintf(stderr, L"read failed\n"); free(buf); CloseHandle(hf); return 4;
    }
    CloseHandle(hf);
    BITMAPFILEHEADER* fh = (BITMAPFILEHEADER*)buf;
    BITMAPINFO* bi = (BITMAPINFO*)(buf + sizeof(BITMAPFILEHEADER));
    INT sx = _wtoi(argv[3]), sy = _wtoi(argv[4]);
    INT w = _wtoi(argv[5]), h = _wtoi(argv[6]);
    INT iw = bi->bmiHeader.biWidth, ih = bi->bmiHeader.biHeight;
    if (bi->bmiHeader.biBitCount != 24 && bi->bmiHeader.biBitCount != 32) {
        fwprintf(stderr, L"unsupported bpp %d\n", bi->bmiHeader.biBitCount);
        free(buf); return 5;
    }
    if (sx < 0 || sy < 0 || w <= 0 || h <= 0 || sx + w > iw || sy + h > ih) {
        fwprintf(stderr, L"rect out of range (img %dx%d)\n", iw, ih);
        free(buf); return 6;
    }
    // 源 DC（内存位图，含整幅）
    HDC sdc = GetDC(nullptr);
    HBITMAP hb = CreateCompatibleBitmap(sdc, iw, ih);
    char* bits = buf + fh->bfOffBits;
    HBITMAP oldS = (HBITMAP)SelectObject(sdc, hb);
    SetDIBits(sdc, hb, 0, ih, bits, bi, DIB_RGB_COLORS);
    // 目标 DC
    HDC ddc = CreateCompatibleDC(sdc);
    HBITMAP db = CreateCompatibleBitmap(sdc, w, h);
    HBITMAP oldD = (HBITMAP)SelectObject(ddc, db);
    BitBlt(ddc, 0, 0, w, h, sdc, sx, sy, SRCCOPY);
    SelectObject(ddc, oldD);
    SelectObject(sdc, oldS);
    // 写出 BMP（注意 biHeight 为负 = top-down）
    DWORD hdr = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    DWORD row = ((DWORD)w * bi->bmiHeader.biBitCount / 8 + 3) & ~3u;
    DWORD dataSize = row * h;
    BITMAPFILEHEADER of = {0x4D42, hdr + dataSize, 0, 0, hdr};
    BITMAPINFOHEADER oh = bi->bmiHeader;
    oh.biWidth = w; oh.biHeight = h; oh.biSizeImage = dataSize;
    HANDLE out = CreateFileW(argv[2], GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE) { fwprintf(stderr, L"open out failed\n"); return 7; }
    DWORD wr = 0;
    char* data = (char*)malloc(dataSize);
    GetDIBits(ddc, db, 0, h, data, (BITMAPINFO*)&oh, DIB_RGB_COLORS);
    WriteFile(out, &of, sizeof(of), &wr, nullptr);
    WriteFile(out, &oh, sizeof(oh), &wr, nullptr);
    if (bi->bmiHeader.biBitCount == 32) {
        // GDI 32bpp 输出为 BI_RGB（无 alpha）
        memset((char*)&oh + sizeof(oh), 0, 0);
        WriteFile(out, data, dataSize, &wr, nullptr);
    } else {
        WriteFile(out, data, dataSize, &wr, nullptr);
    }
    CloseHandle(out);
    free(data); free(buf);
    DeleteObject(hb); DeleteObject(db);
    DeleteDC(ddc); ReleaseDC(nullptr, sdc);
    wprintf(L"saved %s %dx%d\n", argv[2], w, h);
    return 0;
}
