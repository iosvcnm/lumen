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

// 参考图纸：圆角方形徽章 + 内圈刻度点 + 旋钮 + 中心太阳（浅色变体）
static bool PngBytes(int size, std::vector<BYTE>& out) {
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    float s = (float)size;
    float inset = s * 0.035f, w = s - inset * 2, rad = s * 0.205f;
    GraphicsPath badge;
    badge.AddArc(inset, inset, rad * 2, rad * 2, 180, 90);
    badge.AddArc(inset + w - rad * 2, inset, rad * 2, rad * 2, 270, 90);
    badge.AddArc(inset + w - rad * 2, inset + w - rad * 2, rad * 2, rad * 2, 0, 90);
    badge.AddArc(inset, inset + w - rad * 2, rad * 2, rad * 2, 90, 90);
    badge.CloseFigure();
    LinearGradientBrush bg(PointF(0, inset), PointF(0, inset + w),
                           Color(255, 252, 253, 255), Color(255, 224, 231, 243));
    g.FillPath(&bg, &badge);
    Pen rim(Color(255, 202, 212, 228), s * 0.016f);
    g.DrawPath(&rim, &badge);
    // 徽章内沿一圈更浅的描边，呈现凸起层次
    GraphicsPath inner;
    float i2 = s * 0.075f;
    inner.AddArc(i2, i2, s * 0.17f, s * 0.17f, 180, 90);
    inner.AddArc(s - i2 - s * 0.17f, i2, s * 0.17f, s * 0.17f, 270, 90);
    inner.AddArc(s - i2 - s * 0.17f, s - i2 - s * 0.17f, s * 0.17f, s * 0.17f, 0, 90);
    inner.AddArc(i2, s - i2 - s * 0.17f, s * 0.17f, s * 0.17f, 90, 90);
    inner.CloseFigure();
    Pen irim(Color(255, 214, 222, 236), s * 0.012f);
    g.DrawPath(&irim, &inner);

    // 刻度点环：14 点、300° 弧，顶部 3 点着色
    float c = s / 2.f;
    if (size >= 24) {
        for (int i = 0; i < 14; ++i) {
            double a = (-240.0 + i * (300.0 / 13.0)) * PI / 180.0;
            float dx = (float)cos(a), dy = (float)sin(a);
            float rr = s * 0.265f, dr = s * 0.021f;
            Color col = (i >= 6 && i <= 8) ? Color(255, 120, 150, 190)
                                           : Color(255, 176, 188, 208);
            SolidBrush db(col);
            g.FillEllipse(&db, c + dx * rr - dr, c + dy * rr - dr, dr * 2, dr * 2);
        }
    }

    // 旋钮：径向渐变凸盘 + 细描边
    float kr = s * 0.185f;
    GraphicsPath knob;
    knob.AddEllipse(c - kr, c - kr, kr * 2, kr * 2);
    PathGradientBrush kg(&knob);
    kg.SetCenterColor(Color(255, 250, 252, 255));
    kg.SetCenterPoint(PointF(c - kr * 0.25f, c - kr * 0.3f));
    Color surr(Color(255, 216, 224, 238));
    INT n = 1;
    kg.SetSurroundColors(&surr, &n);
    g.FillPath(&kg, &knob);
    Pen ke(Color(255, 196, 206, 224), s * 0.014f);
    g.DrawPath(&ke, &knob);

    DrawSun(g, c, c, s * 0.105f, Color(255, 122, 150, 186));

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
