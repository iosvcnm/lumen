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

// 参考图纸 v2：黏土白双层徽章 + 大旋钮（刻度点领圈 + 右侧指示孔）+ 凹陷中心井 + 浮雕太阳
static void RRect(GraphicsPath& p, RectF r, float rad) {
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

static bool PngBytes(int size, std::vector<BYTE>& out) {
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    float s = (float)size;
    float inset = s * 0.075f, w = s - inset * 2, rad = w * 0.30f;
    RectF bd(inset, inset, w, w);

    // 徽章：双层软落影 + 黏土白渐变 + 倒角
    GraphicsPath sh1; RRect(sh1, RectF(bd.X + w * 0.04f, bd.Y + w * 0.05f, w, w), rad);
    SolidBrush shA(Color(55, 122, 140, 168));
    g.FillPath(&shA, &sh1);
    GraphicsPath sh2; RRect(sh2, RectF(bd.X + w * 0.018f, bd.Y + w * 0.024f, w, w), rad);
    SolidBrush shB(Color(36, 122, 140, 168));
    g.FillPath(&shB, &sh2);
    GraphicsPath badge; RRect(badge, bd, rad);
    LinearGradientBrush bgl(PointF(0, bd.Y), PointF(0, bd.Y + w),
                            Color(255, 246, 249, 253), Color(255, 222, 229, 240));
    g.FillPath(&bgl, &badge);
    Pen bhi(Color(200, 255, 255, 255), s * 0.012f);
    g.DrawPath(&bhi, &badge);
    // 内层面板接缝：一圈浅色描边呈现第二层黏土
    float ip = s * 0.155f;
    GraphicsPath panel; RRect(panel, RectF(ip, ip, s - ip * 2, s - ip * 2), (s - ip * 2) * 0.26f);
    Pen seam(Color(150, 205, 214, 229), s * 0.011f);
    g.DrawPath(&seam, &panel);

    // 大旋钮：凸起主体 + 45° 双向倒角
    float cx = s * 0.5f, cy = s * 0.475f, kr = s * 0.335f;
    GraphicsPath ksh; ksh.AddEllipse(cx - kr + s * 0.02f, cy - kr + s * 0.03f, kr * 2, kr * 2);
    SolidBrush kshb(Color(80, 116, 134, 162));
    g.FillPath(&kshb, &ksh);
    GraphicsPath knob; knob.AddEllipse(cx - kr, cy - kr, kr * 2, kr * 2);
    PathGradientBrush kg(&knob);
    kg.SetCenterPoint(PointF(cx - kr * 0.28f, cy - kr * 0.34f));
    kg.SetCenterColor(Color(255, 251, 253, 255));
    Color ksur(255, 226, 233, 243);
    int kn = 1;
    kg.SetSurroundColors(&ksur, &kn);
    g.FillPath(&kg, &knob);
    Pen khi(Color(190, 255, 255, 255), s * 0.013f);
    g.DrawArc(&khi, cx - kr, cy - kr, kr * 2, kr * 2, 150, 115);
    Pen kshp(Color(90, 173, 186, 204), s * 0.013f);
    g.DrawArc(&kshp, cx - kr, cy - kr, kr * 2, kr * 2, -40, 115);

    // 刻度点领圈：沿旋钮上缘 260° 弧、大小交替，右侧指示孔更大更深
    if (size >= 32) {
        for (int i = 0; i < 16; ++i) {
            double t = 140.0 + 260.0 * i / 15.0;
            double a = t * PI / 180.0;
            float rr = kr * 0.80f;
            float x = cx + (float)cos(a) * rr, y = cy + (float)sin(a) * rr;
            float dr = (i % 3 == 0) ? s * 0.019f : s * 0.013f;
            SolidBrush d(Color(255, 209, 218, 231));
            g.FillEllipse(&d, x - dr, y - dr, dr * 2, dr * 2);
            SolidBrush dh(Color(120, 255, 255, 255));
            g.FillEllipse(&dh, x - dr + dr * 0.35f, y - dr + dr * 0.55f, dr * 1.4f, dr * 1.4f);
        }
    }
    {
        double a = 0.0 * PI / 180.0;                       // 3 点钟指示孔
        float rr = kr * 0.80f;
        float x = cx + (float)cos(a) * rr, y = cy + (float)sin(a) * rr;
        float dr = s * 0.026f;
        SolidBrush d(Color(255, 196, 207, 222));
        g.FillEllipse(&d, x - dr, y - dr, dr * 2, dr * 2);
        Pen dsh(Color(160, 172, 186, 204), dr * 0.5f);
        g.DrawArc(&dsh, x - dr, y - dr, dr * 2, dr * 2, 150, 115);
    }

    // 中心凹陷井：内阴影 + 底缘受光
    float wr = s * 0.19f;
    SolidBrush wf(Color(255, 230, 236, 244));
    g.FillEllipse(&wf, cx - wr, cy - wr, wr * 2, wr * 2);
    Pen wsh(Color(140, 176, 190, 208), s * 0.020f);
    g.DrawArc(&wsh, cx - wr + s * 0.008f, cy - wr + s * 0.008f, (wr - s * 0.008f) * 2,
              (wr - s * 0.008f) * 2, 150, 115);
    Pen whi(Color(150, 255, 255, 255), s * 0.012f);
    g.DrawArc(&whi, cx - wr + s * 0.010f, cy - wr + s * 0.010f, (wr - s * 0.010f) * 2,
              (wr - s * 0.010f) * 2, -40, 115);

    // 井内浮雕太阳：白色偏移 + 本体（色调一致压印）
    {
        SolidBrush hg(Color(170, 255, 255, 255));
        g.FillEllipse(&hg, cx - s * 0.030f + s * 0.006f, cy - s * 0.030f + s * 0.006f,
                      s * 0.060f, s * 0.060f);
        Pen raysW(Color(170, 255, 255, 255), s * 0.019f);
        Pen raysB(Color(255, 201, 212, 227), s * 0.019f);
        raysW.SetStartCap(LineCapRound); raysW.SetEndCap(LineCapRound);
        raysB.SetStartCap(LineCapRound); raysB.SetEndCap(LineCapRound);
        for (int i = 0; i < 8; ++i) {
            double a = i * PI / 4.0 + PI / 8.0;
            float r1 = s * 0.050f, r2 = s * 0.088f;
            g.DrawLine(&raysW,
                cx + (float)cos(a) * r1 + s * 0.005f, cy + (float)sin(a) * r1 + s * 0.005f,
                cx + (float)cos(a) * r2 + s * 0.005f, cy + (float)sin(a) * r2 + s * 0.005f);
            g.DrawLine(&raysB,
                cx + (float)cos(a) * r1, cy + (float)sin(a) * r1,
                cx + (float)cos(a) * r2, cy + (float)sin(a) * r2);
        }
        SolidBrush ctr(Color(255, 208, 218, 232));
        g.FillEllipse(&ctr, cx - s * 0.030f, cy - s * 0.030f, s * 0.060f, s * 0.060f);
    }

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
