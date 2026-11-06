// 生成 lumen.ico：GDI+ 矢量绘制 → PNG 编码 → 组装多尺寸 ICO
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <vector>
#include <stdio.h>

using namespace Gdiplus;

static const double PI = 3.14159265358979323846;

static void DrawSun(Graphics& g, float cx, float cy, float r, Color c) {
    SolidBrush b(c);
    g.FillEllipse(&b, cx - r * 0.34f, cy - r * 0.34f, r * 0.68f, r * 0.68f);
    Pen p(c, r * 0.12f);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    for (int i = 0; i < 8; ++i) {
        double a = i * PI / 4.0;
        g.DrawLine(&p,
            cx + (float)cos(a) * r * 0.56f, cy + (float)sin(a) * r * 0.56f,
            cx + (float)cos(a) * r * 0.92f, cy + (float)sin(a) * r * 0.92f);
    }
}

static bool PngBytes(int size, std::vector<BYTE>& out) {
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    float c = size / 2.f, r = size * 0.46f;
    SolidBrush bg(Color(255, 243, 247, 252));
    g.FillEllipse(&bg, c - r, c - r, r * 2, r * 2);
    DrawSun(g, c, c, size * 0.30f, Color(255, 122, 150, 186));

    CLSID png;
    if (CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png) != S_OK) return false;

    IStream* st = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &st) != S_OK) return false;
    bool ok = bmp.Save(st, &png, nullptr) == Ok;
    if (ok) {
        STATSTG ss{};
        st->Stat(&ss, STATFLAG_NONAME);
        out.resize((size_t)ss.cbSize.QuadPart);
        LARGE_INTEGER z{};
        st->Seek(z, STREAM_SEEK_SET, nullptr);
        ULONG rd = 0;
        ok = SUCCEEDED(st->Read(out.data(), (ULONG)out.size(), &rd)) && rd == out.size();
    }
    st->Release();
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* path = (argc > 1) ? argv[1] : L"lumen.ico";
    GdiplusStartupInput gi;
    ULONG_PTR tok = 0;
    GdiplusStartup(&tok, &gi, nullptr);

    const int sizes[] = { 256, 48, 32, 16 };
    std::vector<std::vector<BYTE>> pngs;
    for (int s : sizes) {
        std::vector<BYTE> b;
        if (!PngBytes(s, b)) { wprintf(L"encode failed at %d\n", s); return 1; }
        pngs.push_back(std::move(b));
    }

    FILE* f = _wfopen(path, L"wb");
    if (!f) return 1;

    WORD zero = 0, type = 1, count = (WORD)pngs.size();
    fwrite(&zero, 2, 1, f);
    fwrite(&type, 2, 1, f);
    fwrite(&count, 2, 1, f);

    DWORD offset = 6 + 16 * (DWORD)pngs.size();
    for (size_t i = 0; i < pngs.size(); ++i) {
        BYTE w = (sizes[i] >= 256) ? 0 : (BYTE)sizes[i];
        BYTE h = w;
        BYTE cc = 0, res = 0;
        WORD planes = 1, bits = 32;
        DWORD len = (DWORD)pngs[i].size();
        fwrite(&w, 1, 1, f); fwrite(&h, 1, 1, f);
        fwrite(&cc, 1, 1, f); fwrite(&res, 1, 1, f);
        fwrite(&planes, 2, 1, f); fwrite(&bits, 2, 1, f);
        fwrite(&len, 4, 1, f); fwrite(&offset, 4, 1, f);
        offset += len;
    }
    for (auto& p : pngs) fwrite(p.data(), 1, p.size(), f);
    fclose(f);

    GdiplusShutdown(tok);
    wprintf(L"%ls written\n", path);
    return 0;
}
