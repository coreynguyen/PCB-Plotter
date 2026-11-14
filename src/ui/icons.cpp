// ============================================================================
// icons.cpp — vector icon painter. All icons are authored on a 16x16 grid.
// ============================================================================
#include "icons.h"
#include "d2d.h"
#include "theme.h"
#include <cmath>
#include <map>
#include <algorithm>
#include <cwchar>
#include <cstring>
#include <vector>

namespace pcbp::ui {

namespace {

struct P {
    ID2D1RenderTarget* rt;
    float ox, oy, s;                 // origin + scale (px per grid unit)
    Com<ID2D1SolidColorBrush> fg, ac, tn, nt;   // stroke, accent, accent tint, neutral tint
    float sw;                        // stroke width (px)
    float alpha = 1.f;               // R20: fixed-colour file-type badges
    Com<ID2D1StrokeStyle> round, dash;

    D2D1_POINT_2F pt(float x, float y) const { return D2D1::Point2F(ox + x * s, oy + y * s); }
    D2D1_RECT_F rc(float x0, float y0, float x1, float y1) const { return D2D1::RectF(ox + x0 * s, oy + y0 * s, ox + x1 * s, oy + y1 * s); }
    // brush index: 0 fg, 1 accent, 2 accent tint, 3 neutral tint
    ID2D1Brush* b(int k) const { return k == 1 ? (ID2D1Brush*)ac.get() : k == 2 ? (ID2D1Brush*)tn.get() : k == 3 ? (ID2D1Brush*)nt.get() : (ID2D1Brush*)fg.get(); }

    void line(float x0, float y0, float x1, float y1, int a = 0, float w = 1.f) const { rt->DrawLine(pt(x0, y0), pt(x1, y1), b(a), sw * w, round.get()); }
    void dline(float x0, float y0, float x1, float y1, int a = 0, float w = 1.f) const { rt->DrawLine(pt(x0, y0), pt(x1, y1), b(a), sw * w, dash.get()); }
    void rect(float x0, float y0, float x1, float y1, int a = 0, float w = 1.f) const { rt->DrawRectangle(rc(x0, y0, x1, y1), b(a), sw * w, round.get()); }
    void drect(float x0, float y0, float x1, float y1, int a = 0, float w = 1.f) const { rt->DrawRectangle(rc(x0, y0, x1, y1), b(a), sw * w, dash.get()); }
    void frect(float x0, float y0, float x1, float y1, int a = 0) const { rt->FillRectangle(rc(x0, y0, x1, y1), b(a)); }
    void rrect(float x0, float y0, float x1, float y1, float r, int a = 0, float w = 1.f) const {
        rt->DrawRoundedRectangle(D2D1::RoundedRect(rc(x0, y0, x1, y1), r * s, r * s), b(a), sw * w, round.get());
    }
    void frrect(float x0, float y0, float x1, float y1, float r, int a = 0) const {
        rt->FillRoundedRectangle(D2D1::RoundedRect(rc(x0, y0, x1, y1), r * s, r * s), b(a));
    }
    // two-tone box: tinted fill + outline
    void box(float x0, float y0, float x1, float y1, float r, int fill = 2, int stroke = 0, float w = 1.f) const { frrect(x0, y0, x1, y1, r, fill); rrect(x0, y0, x1, y1, r, stroke, w); }
    void circle(float cx, float cy, float r, int a = 0, float w = 1.f) const { rt->DrawEllipse(D2D1::Ellipse(pt(cx, cy), r * s, r * s), b(a), sw * w); }
    void fcircle(float cx, float cy, float r, int a = 0) const { rt->FillEllipse(D2D1::Ellipse(pt(cx, cy), r * s, r * s), b(a)); }
    void disc(float cx, float cy, float r, int fill = 2, int stroke = 0, float w = 1.f) const { fcircle(cx, cy, r, fill); circle(cx, cy, r, stroke, w); }
    void poly(std::initializer_list<float> xy, bool closed, bool fill, int a = 0, float w = 1.f) const {
        Com<ID2D1PathGeometry> g;
        d2d_factory()->CreatePathGeometry(g.put());
        Com<ID2D1GeometrySink> k;
        g->Open(k.put());
        auto it = xy.begin();
        float x = *it++, y = *it++;
        k->BeginFigure(pt(x, y), fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        while (it != xy.end()) { x = *it++; y = *it++; k->AddLine(pt(x, y)); }
        k->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        k->Close();
        if (fill) rt->FillGeometry(g.get(), b(a));
        else rt->DrawGeometry(g.get(), b(a), sw * w, round.get());
    }
    // arc from angle a0 to a1 (degrees, 0 = +x, clockwise on screen) radius r
    void arc(float cx, float cy, float r, float a0, float a1, int a = 0, float w = 1.f, bool arrow = false) const {
        Com<ID2D1PathGeometry> g;
        d2d_factory()->CreatePathGeometry(g.put());
        Com<ID2D1GeometrySink> k;
        g->Open(k.put());
        auto P2 = [&](float ang) { float rr = ang * 3.14159265f / 180.f; return pt(cx + r * std::cos(rr), cy + r * std::sin(rr)); };
        k->BeginFigure(P2(a0), D2D1_FIGURE_BEGIN_HOLLOW);
        float sweep = a1 - a0;
        int n = (int)(std::fabs(sweep) / 10.f) + 2;
        for (int i = 1; i <= n; ++i) k->AddLine(P2(a0 + sweep * i / n));
        k->EndFigure(D2D1_FIGURE_END_OPEN);
        k->Close();
        rt->DrawGeometry(g.get(), b(a), sw * w, round.get());
        if (arrow) {
            float rr = a1 * 3.14159265f / 180.f;
            float ex = cx + r * std::cos(rr), ey = cy + r * std::sin(rr);
            float dir = sweep > 0 ? 1.f : -1.f;
            float tx = -std::sin(rr) * dir, ty = std::cos(rr) * dir;        // tangent
            float nx = std::cos(rr), ny = std::sin(rr);
            float L = 2.6f;
            poly({ex + tx * 1.2f, ey + ty * 1.2f, ex - tx * L + nx * L * 0.7f, ey - ty * L + ny * L * 0.7f,
                  ex - tx * L - nx * L * 0.7f, ey - ty * L - ny * L * 0.7f}, true, true, a);
        }
    }
    void text(const wchar_t* t, float x0, float y0, float x1, float y1, float size, int a = 0, bool bold = true) const {
        IDWriteTextFormat* f = shared_text_format(size * s, bold, 1);
        rt->DrawText(t, (UINT32)wcslen(t), f, rc(x0, y0, x1, y1), b(a));
    }
    void chip(float x0, float y0, float x1, float y1, int fill = 2, int stroke = 0) const {   // IC body with pins
        box(x0, y0, x1, y1, 0.8f, fill, stroke);
        for (float y = y0 + 1.5f; y < y1 - 0.5f; y += 2.f) { line(x0 - 1.6f, y, x0, y, stroke); line(x1, y, x1 + 1.6f, y, stroke); }
    }
    void magnifier(float cx, float cy, float r, int lens = 2) const {
        fcircle(cx, cy, r, lens);
        circle(cx, cy, r);
        line(cx + r * 0.72f, cy + r * 0.72f, cx + r * 0.72f + 3.2f, cy + r * 0.72f + 3.2f, 0, 1.7f);
    }
    void doc(float x0, float y0, float x1, float y1, int fill = 3) const {
        poly({x0, y0, x1 - 3, y0, x1, y0 + 3, x1, y1, x0, y1}, true, true, fill);
        poly({x0, y0, x1 - 3, y0, x1, y0 + 3, x1, y1, x0, y1}, true, false);
        poly({x1 - 3, y0, x1 - 3, y0 + 3, x1, y0 + 3}, false, false);
    }
    void cursor(float x, float y, float k, int fill = 1) const {   // arrow pointer, tip at (x,y), scale k
        poly({x, y, x, y + 10.5f * k, x + 2.6f * k, y + 8.1f * k, x + 4.5f * k, y + 12.2f * k, x + 6.2f * k, y + 11.4f * k,
              x + 4.4f * k, y + 7.4f * k, x + 7.8f * k, y + 7.4f * k}, true, true, fill);
        poly({x, y, x, y + 10.5f * k, x + 2.6f * k, y + 8.1f * k, x + 4.5f * k, y + 12.2f * k, x + 6.2f * k, y + 11.4f * k,
              x + 4.4f * k, y + 7.4f * k, x + 7.8f * k, y + 7.4f * k}, true, false, 0, 0.9f);
    }
    void in_arrow() const {   // accent "import" arrow, top-left
        line(1.6f, 1.6f, 6.2f, 6.2f, 1, 1.5f);
        poly({6.6f, 2.6f, 6.6f, 6.6f, 2.6f, 6.6f}, false, false, 1, 1.5f);
    }
    void slab(float y0, float y1) const { box(1.5f, y0, 14.5f, y1, 0.8f, 3, 0); }
    void check(float x, float y, float k, int a = 1, float w = 1.6f) const { poly({x, y, x + 2.2f * k, y + 2.2f * k, x + 6 * k, y - 2.4f * k}, false, false, a, w); }
};

// R20 file-type icons: page with a folded corner and a coloured label band with a 3-letter stroke-font tag.
static const std::vector<std::vector<float>>& glyph(char c) {
    static std::map<char, std::vector<std::vector<float>>> g = {
        {'P', {{0, 1, 0, 0, 1, 0, 1, .5f, 0, .5f}}},
        {'D', {{0, 0, 0, 1, .6f, 1, 1, .7f, 1, .3f, .6f, 0, 0, 0}}},
        {'F', {{1, 0, 0, 0, 0, 1}, {0, .5f, .8f, .5f}}},
        {'O', {{0, 0, 1, 0, 1, 1, 0, 1, 0, 0}}},
        {'C', {{1, 0, 0, 0, 0, 1, 1, 1}}},
        {'X', {{0, 0, 1, 1}, {1, 0, 0, 1}}},
        {'L', {{0, 0, 0, 1, 1, 1}}},
        {'S', {{1, 0, 0, 0, 0, .5f, 1, .5f, 1, 1, 0, 1}}},
        {'V', {{0, 0, .5f, 1, 1, 0}}},
        {'Z', {{0, 0, 1, 0, 0, 1, 1, 1}}},
        {'I', {{.5f, 0, .5f, 1}, {.1f, 0, .9f, 0}, {.1f, 1, .9f, 1}}},
        {'G', {{1, 0, 0, 0, 0, 1, 1, 1, 1, .55f, .5f, .55f}}},
        {'B', {{0, 0, 0, 1, 1, 1, 1, .5f, 0, .5f}, {0, 0, .8f, 0, .8f, .5f}}},
        {'R', {{0, 1, 0, 0, 1, 0, 1, .5f, 0, .5f, 1, 1}}},
        {'M', {{0, 1, 0, 0, .5f, .5f, 1, 0, 1, 1}}},
        {'E', {{1, 0, 0, 0, 0, 1, 1, 1}, {0, .5f, .8f, .5f}}},
        {'A', {{0, 1, .5f, 0, 1, 1}, {.25f, .62f, .75f, .62f}}},
        {'T', {{0, 0, 1, 0}, {.5f, 0, .5f, 1}}},
    };
    static const std::vector<std::vector<float>> none;
    auto it = g.find(c);
    return it == g.end() ? none : it->second;
}
static void file_icon(P& p, uint32_t band, const char* tag) {
    // page (neutral tint fill + outline) with folded corner
    p.poly({3, 1, 10, 1, 13.5f, 4.5f, 13.5f, 15, 3, 15}, true, true, 3);
    p.poly({3, 1, 10, 1, 13.5f, 4.5f, 13.5f, 15, 3, 15}, true, false, 0, 0.9f);
    p.poly({10, 1, 10, 4.5f, 13.5f, 4.5f}, false, false, 0, 0.8f);
    if (!tag) {   // generic: text lines
        for (float y : {6.5f, 8.8f, 11.1f}) p.line(5, y, 11.5f, y, 0, 0.8f);
        return;
    }
    p.line(5, 3.6f, 8.5f, 3.6f, 0, 0.7f);
    Com<ID2D1SolidColorBrush> bb, wb;
    p.rt->CreateSolidColorBrush(d2c(band, p.alpha), bb.put());
    p.rt->CreateSolidColorBrush(d2c(0xFFFFFFFF, p.alpha), wb.put());
    const D2D1_RECT_F br = p.rc(1, 7.2f, 12.5f, 13.2f);
    p.rt->FillRoundedRectangle(D2D1::RoundedRect(br, 1.0f * p.s, 1.0f * p.s), bb.get());
    const int n = (int)std::strlen(tag);
    const float lw = 2.2f, gap = 0.9f, lh = 3.6f;
    const float tw = n * lw + (n - 1) * gap;
    float x = 1 + (11.5f - tw) * 0.5f;
    const float y = 7.2f + (6.0f - lh) * 0.5f;
    const float stroke = std::max(1.0f, p.s * 0.62f);
    for (int i = 0; i < n; ++i, x += lw + gap)
        for (auto& pl : glyph(tag[i]))
            for (size_t k = 2; k + 1 < pl.size(); k += 2)
                p.rt->DrawLine(p.pt(x + pl[k - 2] * lw, y + pl[k - 1] * lh), p.pt(x + pl[k] * lw, y + pl[k + 1] * lh), wb.get(), stroke, p.round.get());
}

void paint(P& p, Icon id) {
    switch (id) {
    // ---- file
    case Icon::New: p.doc(2.5f, 1.5f, 12.5f, 14.5f); p.line(11.8f, 8.6f, 11.8f, 15, 1, 1.6f); p.line(8.6f, 11.8f, 15, 11.8f, 1, 1.6f); break;
    case Icon::Open:
        p.poly({1.5f, 3, 6, 3, 7.5f, 4.5f, 13, 4.5f, 13, 13.5f, 1.5f, 13.5f}, true, true, 3);
        p.poly({1.5f, 3, 6, 3, 7.5f, 4.5f, 13, 4.5f, 13, 7}, false, false);
        p.poly({1.5f, 13.5f, 1.5f, 3}, false, false);
        p.poly({3.5f, 7.5f, 15, 7.5f, 13, 13.5f, 1.5f, 13.5f}, true, true, 1); break;
    case Icon::Save: case Icon::SaveAs:
        p.poly({2, 2, 12, 2, 14, 4, 14, 14, 2, 14}, true, true, 3);
        p.poly({2, 2, 12, 2, 14, 4, 14, 14, 2, 14}, true, false);
        p.frect(5, 2, 11, 6, 2); p.rect(5, 2, 11, 6); p.frect(8.6f, 3, 10, 5, 1);
        p.frrect(4.5f, 9, 11.5f, 14, 0.6f, 1);
        if (id == Icon::SaveAs) { p.line(9, 15, 15, 9, 1, 1.8f); }
        break;
    case Icon::Cut:
        p.disc(4.5f, 12, 2.2f, 2, 0); p.disc(11.5f, 12, 2.2f, 2, 0); p.line(6, 10.3f, 11, 1.8f, 1, 1.3f); p.line(10, 10.3f, 5, 1.8f, 1, 1.3f); break;
    case Icon::Copy: p.rrect(1.8f, 1.8f, 10.5f, 11, 1); p.box(5.5f, 5, 14.2f, 14.5f, 1, 2, 0); p.line(7.8f, 8.5f, 12, 8.5f, 1); p.line(7.8f, 11.2f, 12, 11.2f, 1); break;
    case Icon::Paste: p.box(2.5f, 3, 13.5f, 15, 1, 3, 0); p.frrect(5.5f, 1.5f, 10.5f, 4.8f, 0.8f, 1); p.line(5, 8, 11, 8, 1); p.line(5, 11, 9, 11, 1); break;
    case Icon::Print: case Icon::PrintPreview:
        p.box(1.5f, 5.5f, 14.5f, 11.5f, 1.2f, 3, 0); p.box(4, 1.5f, 12, 5.5f, 0.4f, 0, 0); p.frect(4.6f, 2.1f, 11.4f, 5.4f, 3);
        p.box(4, 9.5f, 12, 14.5f, 0.4f, 1, 1); p.fcircle(12.2f, 7.6f, 0.8f, 1);
        if (id == Icon::PrintPreview) { p.magnifier(10.5f, 10.5f, 2.6f, 2); }
        break;
    case Icon::About: p.disc(8, 8, 6.5f, 2, 1); p.line(8, 7.2f, 8, 11.6f, 1, 1.5f); p.fcircle(8, 4.7f, 1.0f, 1); break;
    case Icon::Undo: p.arc(8.5f, 9, 5, 200, 380, 0, 1.3f); p.poly({1.5f, 4.6f, 3.4f, 9.8f, 7.4f, 6.3f}, true, true, 1); break;
    case Icon::Redo: p.arc(7.5f, 9, 5, 340, 160, 0, 1.3f); p.poly({14.5f, 4.6f, 12.6f, 9.8f, 8.6f, 6.3f}, true, true, 1); break;
    case Icon::Delete:
        p.line(2.5f, 4, 13.5f, 4, 0, 1.2f); p.poly({4, 4, 5, 14.5f, 11, 14.5f, 12, 4}, true, true, 2); p.poly({4, 4, 5, 14.5f, 11, 14.5f, 12, 4}, false, false);
        p.rect(6, 1.5f, 10, 4); p.line(7, 7, 7, 12, 1); p.line(9, 7, 9, 12, 1); break;

    // ---- windows / views
    case Icon::GerberView:
        p.poly({8, 1.5f, 15, 5, 8, 8.5f, 1, 5}, true, true, 2); p.poly({8, 1.5f, 15, 5, 8, 8.5f, 1, 5}, true, false, 1);
        p.poly({1, 8, 8, 11.5f, 15, 8}, false, false); p.poly({1, 11, 8, 14.5f, 15, 11}, false, false); break;
    case Icon::UniversalView:
        p.box(1.5f, 2.5f, 14.5f, 13.5f, 1.2f, 3, 0);
        p.frect(3.5f, 5, 7, 8, 1); p.frect(9, 5, 12.5f, 7, 1); p.frect(9, 9, 12.5f, 11.5f, 2); p.frect(3.5f, 10, 7, 11.5f, 2); break;
    case Icon::FactoryModel:
        p.box(1, 2, 6, 6.5f, 1, 2, 0); p.box(10, 2, 15, 6.5f, 1, 2, 0); p.box(5.5f, 10, 10.5f, 14.5f, 1, 1, 0);
        p.line(6, 4.2f, 10, 4.2f, 1); p.line(12.5f, 6.5f, 8.8f, 10, 1); break;
    case Icon::MachineProg: case Icon::Machine:
        p.box(1.5f, 7, 14.5f, 14.5f, 1, 3, 0); p.line(3.5f, 7, 3.5f, 2); p.line(3.5f, 2, 12.5f, 2, 0, 1.2f); p.line(12.5f, 2, 12.5f, 7);
        p.frect(6.5f, 1.2f, 9.5f, 4.2f, 1); p.line(8, 4, 8, 8.5f, 1, 1.3f); p.fcircle(8, 9.6f, 1.2f, 1); p.frect(4, 11.5f, 12, 13, 2); break;
    case Icon::VisualAid:
        p.box(1.5f, 1.5f, 14.5f, 14.5f, 1, 3, 0); p.frect(3.5f, 3.5f, 9.5f, 9.5f, 2); p.rect(3.5f, 3.5f, 9.5f, 9.5f, 1); p.fcircle(5.5f, 5.5f, 0.9f, 1);
        p.line(11, 4, 13, 4); p.line(11, 6.5f, 13, 6.5f); p.line(3.5f, 12, 12.5f, 12, 1); break;
    case Icon::Notes: p.doc(3, 1.5f, 13, 14.5f); p.line(5.5f, 6, 10.5f, 6); p.line(5.5f, 8.5f, 10.5f, 8.5f); p.line(5.5f, 11, 8.5f, 11, 1, 1.3f); break;
    case Icon::PackageEditor: p.chip(4.5f, 3, 11.5f, 13); p.fcircle(6.5f, 5, 0.9f, 1); break;
    case Icon::Locations:
        p.box(1.5f, 2, 14.5f, 14, 0.8f, 3, 0); p.frect(1.5f, 2, 14.5f, 5.5f, 2); p.line(1.5f, 5.5f, 14.5f, 5.5f); p.line(5.5f, 2, 5.5f, 14);
        p.line(7.5f, 8.5f, 12.5f, 8.5f, 1); p.line(7.5f, 11.5f, 12.5f, 11.5f, 1); break;
    case Icon::Bom:
        p.box(1.5f, 1.5f, 14.5f, 14.5f, 1, 3, 0);
        for (int i = 0; i < 3; ++i) { p.fcircle(4.2f, 4.8f + i * 3.4f, 1.0f, 1); p.line(6.5f, 4.8f + i * 3.4f, 12.5f, 4.8f + i * 3.4f); }
        break;

    // ---- import / export: content glyph + accent "in" arrow
    case Icon::ImportCad: p.chip(7, 5.5f, 13, 14.5f); p.fcircle(8.6f, 7.2f, 0.8f, 1); p.in_arrow(); break;
    case Icon::ImportGerber:
        p.poly({10, 5, 15.2f, 7.6f, 10, 10.2f, 4.8f, 7.6f}, true, true, 2); p.poly({10, 5, 15.2f, 7.6f, 10, 10.2f, 4.8f, 7.6f}, true, false);
        p.poly({4.8f, 10.6f, 10, 13.2f, 15.2f, 10.6f}, false, false); p.in_arrow(); break;
    case Icon::ImportCentroid:
        p.disc(10.5f, 10, 4, 2, 0); p.line(10.5f, 4.6f, 10.5f, 15.4f); p.line(5.1f, 10, 15.9f, 10); p.fcircle(10.5f, 10, 1.4f, 1); p.in_arrow(); break;
    case Icon::ImportBom:
        p.box(6, 4.5f, 15, 15, 0.8f, 3, 0);
        for (int i = 0; i < 3; ++i) { p.fcircle(8.2f, 7.4f + i * 2.6f, 0.75f, 1); p.line(9.8f, 7.4f + i * 2.6f, 13.4f, 7.4f + i * 2.6f); }
        p.in_arrow(); break;
    case Icon::Export:
        p.poly({2, 9, 2, 14.5f, 14, 14.5f, 14, 9}, false, false, 0, 1.1f); p.frect(2.6f, 11.5f, 13.4f, 14, 3);
        p.line(8, 11, 8, 1.8f, 1, 1.5f); p.poly({4.8f, 5, 8, 1.6f, 11.2f, 5}, false, false, 1, 1.5f); break;
    case Icon::Plugin:
        p.box(3, 6, 13, 13, 1.5f, 2, 0); p.line(6, 6, 6, 1.5f, 1, 1.4f); p.line(10, 6, 10, 1.5f, 1, 1.4f); p.line(8, 13, 8, 15.5f, 0, 1.2f); break;
    case Icon::Fusion: p.disc(5.5f, 8, 4.2f, 3, 0); p.disc(10.5f, 8, 4.2f, 2, 1); break;

    // ---- master toolbar
    case Icon::CompInfo: p.chip(3, 4, 9, 14); p.disc(11.8f, 4.6f, 3.4f, 2, 1); p.text(L"i", 8.4f, 1.0f, 15.2f, 8.4f, 5.6f, 1); break;
    case Icon::Select: p.cursor(3.5f, 1.5f, 1.05f, 1); break;
    case Icon::SelectPart: p.cursor(1.5f, 1, 0.78f, 2); p.box(7.5f, 7.5f, 15, 15, 1.2f, 2, 1); p.text(L"#", 7.5f, 7.4f, 15, 15, 6.5f, 1); break;
    case Icon::SelectPackage: p.cursor(1.5f, 1, 0.78f, 2); p.chip(9.5f, 8, 13.5f, 15, 2, 1); break;
    case Icon::SelectTech: p.cursor(1.5f, 1, 0.78f, 2); p.box(7.5f, 7.5f, 15, 15, 1.2f, 2, 1); p.text(L"T", 7.5f, 7.4f, 15, 15, 6.5f, 1); break;
    case Icon::PinOne: p.chip(4, 2.5f, 12, 13.5f); p.fcircle(6.3f, 4.8f, 1.6f, 1); p.text(L"1", 4, 6, 12, 13, 6.5f, 0); break;
    case Icon::VirtualOrigin:
        p.disc(8, 8, 4.5f, 2, 1); p.line(8, 1, 8, 15); p.line(1, 8, 15, 8); p.fcircle(8, 8, 1.6f, 1); break;
    case Icon::MoveLabel:
        p.box(2, 2, 14, 9.5f, 1.2f, 2, 1); p.text(L"R1", 2, 2, 14, 9.5f, 6, 0, true);
        p.line(3, 13, 13, 13, 0, 1.2f); p.poly({5, 11, 3, 13, 5, 15}, false, false, 0, 1.2f); p.poly({11, 11, 13, 13, 11, 15}, false, false, 0, 1.2f); break;
    case Icon::FindRef: p.magnifier(6.5f, 6.5f, 4.8f, 2); p.text(L"R", 1.7f, 1.7f, 11.3f, 11.3f, 6.2f, 1); break;
    case Icon::ShowUnassigned:
        p.chip(3, 2.5f, 9.5f, 12, 3); p.disc(11.5f, 11.5f, 3.5f, 2, 1); p.line(9.4f, 13.6f, 13.6f, 9.4f, 1, 1.2f); break;
    case Icon::Rot90:  p.disc(8, 8.5f, 6.3f, 3, 3); p.arc(8, 8.5f, 5, 180, 270.1f, 1, 1.4f, true); p.text(L"90", 3.5f, 6.5f, 12.5f, 14, 5.6f, 0); break;
    case Icon::Rot180: p.disc(8, 8.5f, 6.3f, 3, 3); p.arc(8, 8.5f, 5, 180, 360, 1, 1.4f, true); p.text(L"180", 2.5f, 7, 13.5f, 14, 5.0f, 0); break;
    case Icon::Rot270: p.disc(8, 8.5f, 6.3f, 3, 3); p.arc(8, 8.5f, 5, 180, 450, 1, 1.4f, true); p.text(L"270", 2.5f, 7, 13.5f, 14, 5.0f, 0); break;

    // ---- zoom
    case Icon::ZoomOut: p.magnifier(6.5f, 6.5f, 5); p.line(4, 6.5f, 9, 6.5f, 1, 1.5f); break;
    case Icon::ZoomIn: p.magnifier(6.5f, 6.5f, 5); p.line(4, 6.5f, 9, 6.5f, 1, 1.5f); p.line(6.5f, 4, 6.5f, 9, 1, 1.5f); break;
    case Icon::ZoomRect: p.frect(1.5f, 1.5f, 10, 10, 2); p.drect(1.5f, 1.5f, 10, 10, 1, 1.1f); p.magnifier(10.5f, 10.5f, 3, 3); break;
    case Icon::Zoom100: p.magnifier(6.5f, 6.5f, 5.2f); p.text(L"1:1", 1.3f, 3.6f, 11.7f, 9.6f, 5.0f, 1); break;
    case Icon::ZoomAll:
        p.poly({1.5f, 5, 1.5f, 1.5f, 5, 1.5f}, false, false, 0, 1.2f); p.poly({11, 1.5f, 14.5f, 1.5f, 14.5f, 5}, false, false, 0, 1.2f);
        p.poly({1.5f, 11, 1.5f, 14.5f, 5, 14.5f}, false, false, 0, 1.2f); p.poly({11, 14.5f, 14.5f, 14.5f, 14.5f, 11}, false, false, 0, 1.2f);
        p.box(4.5f, 4.5f, 11.5f, 11.5f, 1, 2, 1); break;
    case Icon::Pan:
        p.line(8, 2, 8, 14); p.line(2, 8, 14, 8);
        p.poly({5.6f, 4, 8, 1.5f, 10.4f, 4}, true, true, 1); p.poly({5.6f, 12, 8, 14.5f, 10.4f, 12}, true, true, 1);
        p.poly({4, 5.6f, 1.5f, 8, 4, 10.4f}, true, true, 1); p.poly({12, 5.6f, 14.5f, 8, 12, 10.4f}, true, true, 1); break;

    // ---- viewer
    case Icon::ViewTop: p.slab(8, 11); p.frect(3.5f, 5.5f, 7, 8, 1); p.frect(9, 6.5f, 12.5f, 8, 1);
        p.line(8, 0.8f, 8, 4, 0, 1.2f); p.poly({6.3f, 2.6f, 8, 4.3f, 9.7f, 2.6f}, false, false, 0, 1.2f); break;
    case Icon::ViewBottom: p.slab(5, 8); p.frect(3.5f, 8, 7, 10.5f, 1); p.frect(9, 8, 12.5f, 9.5f, 1);
        p.line(8, 15.2f, 8, 12, 0, 1.2f); p.poly({6.3f, 13.4f, 8, 11.7f, 9.7f, 13.4f}, false, false, 0, 1.2f); break;
    case Icon::TopVisible: p.slab(9, 12); p.frect(2.5f, 6.5f, 6, 9, 1); p.frect(10, 7.5f, 13.5f, 9, 1); p.text(L"T", 4, 0.5f, 12, 7.5f, 6.5f, 0); break;
    case Icon::BottomVisible: p.slab(4, 7); p.frect(2.5f, 7, 6, 9.5f, 1); p.frect(10, 7, 13.5f, 8.5f, 1); p.text(L"B", 4, 8.8f, 12, 15.8f, 6.5f, 0); break;
    case Icon::AllImages:
        for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) p.box(1.5f + i * 7, 1.5f + j * 7, 7.5f + i * 7, 7.5f + j * 7, 0.9f, i == 0 && j == 0 ? 1 : 3, 0);
        break;

    // ---- production
    case Icon::Assign: p.chip(2.5f, 4, 7.5f, 12, 3); p.line(9, 8, 12, 8, 1, 1.4f); p.poly({10.6f, 5.8f, 12.8f, 8, 10.6f, 10.2f}, false, false, 1, 1.4f); p.fcircle(14.3f, 8, 1.4f, 1); break;
    case Icon::Unassign: p.chip(2.5f, 4, 7.5f, 12, 3); p.line(10.5f, 5.5f, 14.5f, 10.5f, 1, 1.5f); p.line(14.5f, 5.5f, 10.5f, 10.5f, 1, 1.5f); break;
    case Icon::Digitize:
        p.drect(2, 2, 14, 14, 1, 1.0f);
        p.frect(4, 5, 6.5f, 7.5f, 2); p.frect(9.5f, 5, 12, 7.5f, 2); p.frect(4, 9, 6.5f, 11.5f, 0); p.frect(9.5f, 9, 12, 11.5f, 0); break;
    case Icon::AutoScan:
        p.frect(1.5f, 3, 4, 5.5f, 0); p.frect(6, 3, 8.5f, 5.5f, 0); p.frect(1.5f, 10.5f, 4, 13, 0); p.frect(6, 10.5f, 8.5f, 13, 0);
        p.magnifier(10.5f, 8, 3, 2); p.line(8, 8, 13, 8, 1); break;
    case Icon::Panelize:
        p.rrect(1, 1.5f, 15, 14.5f, 1, 0, 0.9f);
        for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) p.box(3 + i * 5.5f, 3.5f + j * 5.5f, 7.5f + i * 5.5f, 7.5f + j * 5.5f, 0.6f, i == 0 && j == 0 ? 1 : 2, i == 0 && j == 0 ? 1 : 0, 0.8f);
        break;
    case Icon::Generate: p.disc(8, 8, 6.5f, 2, 1); p.poly({6.5f, 4.8f, 11.6f, 8, 6.5f, 11.2f}, true, true, 1); break;
    case Icon::Fiducial: p.disc(8, 8, 5.5f, 2, 0); p.fcircle(8, 8, 2.6f, 1); p.line(8, 0.8f, 8, 3); p.line(8, 13, 8, 15.2f); p.line(0.8f, 8, 3, 8); p.line(13, 8, 15.2f, 8); break;
    case Icon::Layers:
        p.poly({8, 2, 15, 5.5f, 8, 9, 1, 5.5f}, true, true, 1);
        p.poly({1, 8.5f, 8, 12, 15, 8.5f}, false, false); p.poly({1, 11.5f, 8, 15, 15, 11.5f}, false, false); break;
    case Icon::Report:
        p.box(2.5f, 2.5f, 13.5f, 15, 1, 3, 0); p.frrect(5.5f, 1, 10.5f, 4, 0.8f, 0);
        p.check(5, 9.5f, 1.0f, 1, 1.7f); break;
    case Icon::Grid:
        p.frect(2, 2, 14, 14, 3);
        for (int i = 0; i < 4; ++i) { p.line(2 + i * 4, 2, 2 + i * 4, 14, i == 0 || i == 3 ? 0 : 1, 0.9f); p.line(2, 2 + i * 4, 14, 2 + i * 4, i == 0 || i == 3 ? 0 : 1, 0.9f); }
        break;
    case Icon::Snap:
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) p.fcircle(2.5f + i * 3.7f, 2.5f + j * 3.7f, 0.65f);
        p.disc(6.2f, 9.9f, 2.0f, 1, 1); break;
    case Icon::Theme:
        p.circle(8, 8, 5.5f);
        {
            Com<ID2D1PathGeometry> g; d2d_factory()->CreatePathGeometry(g.put());
            Com<ID2D1GeometrySink> k; g->Open(k.put());
            k->BeginFigure(p.pt(8, 2.5f), D2D1_FIGURE_BEGIN_FILLED);
            k->AddArc(D2D1::ArcSegment(p.pt(8, 13.5f), D2D1::SizeF(5.5f * p.s, 5.5f * p.s), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            k->EndFigure(D2D1_FIGURE_END_CLOSED); k->Close();
            p.rt->FillGeometry(g.get(), p.b(1));
        }
        break;
    case Icon::Settings:
        for (int i = 0; i < 8; ++i) {
            float a = i * 3.14159265f / 4.f;
            p.line(8 + std::cos(a) * 4.2f, 8 + std::sin(a) * 4.2f, 8 + std::cos(a) * 6.6f, 8 + std::sin(a) * 6.6f, 0, 1.8f);
        }
        p.disc(8, 8, 4.4f, 3, 0); p.disc(8, 8, 2.0f, 1, 1); break;
    case Icon::Board:
        p.box(1.5f, 2.5f, 14.5f, 13.5f, 1.4f, 2, 0);
        p.poly({4, 5.5f, 7, 5.5f, 9.5f, 8, 12, 8}, false, false, 1, 1.1f); p.line(4, 10.5f, 8, 10.5f, 1, 1.1f);
        p.fcircle(4, 5.5f, 1.1f, 1); p.fcircle(12, 8, 1.1f, 1); p.fcircle(4, 10.5f, 1.1f, 1); p.frect(9.5f, 9.5f, 12.5f, 11.8f, 0); break;
    case Icon::Offset:
        p.drect(1.5f, 6, 9.5f, 14.5f, 0); p.box(6.5f, 1.5f, 14.5f, 9.5f, 0.6f, 2, 1); break;
    case Icon::Text: p.text(L"A", 1, 1, 15, 15, 12, 0, false); p.line(3, 14.5f, 13, 14.5f, 1, 1.2f); break;
    case Icon::Line: p.line(2.5f, 13.5f, 13.5f, 2.5f, 0, 1.2f); p.fcircle(2.5f, 13.5f, 1.5f, 1); p.fcircle(13.5f, 2.5f, 1.5f, 1); break;
    case Icon::Rect: p.box(2, 3.5f, 14, 12.5f, 0.4f, 2, 0); break;
    case Icon::Circle: p.disc(8, 8, 6, 2, 0); break;
    case Icon::Arrow: p.line(2.5f, 13.5f, 13, 3, 0, 1.2f); p.poly({8, 2.6f, 13.4f, 2.6f, 13.4f, 8}, true, true, 1); break;
    case Icon::Picture: p.box(1.5f, 2.5f, 14.5f, 13.5f, 0.8f, 3, 0); p.poly({1.5f, 12.5f, 6, 7, 9, 10, 11, 8, 14.5f, 12.5f}, true, true, 2); p.poly({1.5f, 12.5f, 6, 7, 9, 10, 11, 8, 14.5f, 12.5f}, false, false, 1); p.fcircle(11, 5.5f, 1.3f, 1); break;
    case Icon::BomBlock:
        p.box(1.5f, 2.5f, 14.5f, 13.5f, 0.6f, 3, 0); p.frect(1.5f, 2.5f, 14.5f, 5, 1); p.line(1.5f, 8, 14.5f, 8); p.line(1.5f, 10.8f, 14.5f, 10.8f); p.line(6, 5, 6, 13.5f); break;

    // ---- gerber workspace
    case Icon::Measure:
        p.poly({1.5f, 10.5f, 13, 4, 14.8f, 7.2f, 3.3f, 13.7f}, true, true, 2); p.poly({1.5f, 10.5f, 13, 4, 14.8f, 7.2f, 3.3f, 13.7f}, true, false);
        p.line(4.4f, 8.9f, 5.2f, 10.3f, 1); p.line(7.3f, 7.3f, 8.4f, 9.2f, 1); p.line(10.2f, 5.7f, 11, 7.1f, 1); break;
    case Icon::Clearance:
        p.box(1, 3, 5.5f, 13, 0.6f, 3, 0); p.box(10.5f, 3, 15, 13, 0.6f, 3, 0);
        p.line(6.6f, 8, 9.4f, 8, 1, 1.2f); p.poly({7.8f, 6.4f, 6.3f, 8, 7.8f, 9.6f}, false, false, 1, 1.2f); p.poly({8.2f, 6.4f, 9.7f, 8, 8.2f, 9.6f}, false, false, 1, 1.2f); break;
    case Icon::CropIn: p.drect(1.5f, 1.5f, 14.5f, 14.5f, 0); p.box(5, 5, 11, 11, 0.6f, 1, 1); break;
    case Icon::CropOut: p.frect(1.5f, 1.5f, 14.5f, 14.5f, 2); p.rect(1.5f, 1.5f, 14.5f, 14.5f, 1); p.drect(5, 5, 11, 11, 0); break;
    case Icon::Compare: p.disc(6, 8, 4.5f, 3, 0); p.disc(10, 8, 4.5f, 2, 1); break;
    case Icon::FillSolid: p.frrect(2, 4, 14, 12, 2.5f, 1); break;
    case Icon::FillOutline: p.rrect(2, 4, 14, 12, 2.5f, 1, 1.4f); break;
    case Icon::FillSketch: p.line(2, 8, 14, 8, 1, 1.2f); p.fcircle(2, 8, 1.3f); p.fcircle(14, 8, 1.3f); p.drect(5, 5, 11, 11, 0, 0.8f); break;
    case Icon::Eye:   // R12 §13: ON = bold outline + accent-tinted eye + solid accent iris (reads as "filled")
        p.poly({0.8f, 8, 4.2f, 3.6f, 8, 2.4f, 11.8f, 3.6f, 15.2f, 8, 11.8f, 12.4f, 8, 13.6f, 4.2f, 12.4f}, true, true, 2);
        p.poly({0.8f, 8, 4.2f, 3.6f, 8, 2.4f, 11.8f, 3.6f, 15.2f, 8, 11.8f, 12.4f, 8, 13.6f, 4.2f, 12.4f}, true, false, 0, 1.5f);
        p.fcircle(8, 8, 3.1f, 1); p.fcircle(8, 8, 1.2f, 0); break;
    case Icon::EyeOff:   // R13 (REVIEW_V12 §7): hidden state must read at a glance - solid outline (was dashed / faint),
                         // hollow iris ring, heavy slash
        p.poly({0.8f, 8, 4.2f, 3.6f, 8, 2.4f, 11.8f, 3.6f, 15.2f, 8, 11.8f, 12.4f, 8, 13.6f, 4.2f, 12.4f}, true, false, 0, 1.4f);
        p.circle(8, 8, 2.6f, 0, 1.2f);
        p.line(2.2f, 14.4f, 13.8f, 1.6f, 0, 2.4f); break;
    case Icon::Solo: p.disc(8, 8, 6, 3, 0); p.fcircle(8, 8, 3, 1); break;
    case Icon::Query: p.magnifier(7, 7, 5, 2); p.text(L"?", 2, 1.8f, 12, 12.2f, 7.5f, 1); break;

    // ---- v2 additions
    case Icon::Home:
        p.poly({2.5f, 7.5f, 8, 2.5f, 13.5f, 7.5f, 13.5f, 14, 2.5f, 14}, true, true, 3);
        p.poly({1.2f, 8.6f, 8, 2.2f, 14.8f, 8.6f}, false, false, 0, 1.3f);
        p.poly({2.5f, 7.5f, 2.5f, 14, 13.5f, 14, 13.5f, 7.5f}, false, false);
        p.frrect(6.5f, 9, 9.5f, 14, 0.5f, 1); break;
    case Icon::Move:
        p.box(5.5f, 5.5f, 10.5f, 10.5f, 0.6f, 2, 0);
        p.poly({6.3f, 3, 8, 0.9f, 9.7f, 3}, true, true, 1); p.poly({6.3f, 13, 8, 15.1f, 9.7f, 13}, true, true, 1);
        p.poly({3, 6.3f, 0.9f, 8, 3, 9.7f}, true, true, 1); p.poly({13, 6.3f, 15.1f, 8, 13, 9.7f}, true, true, 1);
        p.line(8, 2.5f, 8, 5.5f, 1); p.line(8, 10.5f, 8, 13.5f, 1); p.line(2.5f, 8, 5.5f, 8, 1); p.line(10.5f, 8, 13.5f, 8, 1); break;
    case Icon::NoRef:
        p.chip(3, 4, 9.5f, 13.5f, 3); p.disc(11.8f, 4.6f, 3.6f, 2, 1); p.text(L"?", 8.2f, 0.9f, 15.4f, 8.3f, 6, 1); break;
    case Icon::SecondSide:
        p.drect(4.5f, 1.5f, 14.5f, 10.5f, 0, 0.9f); p.frect(4.5f, 1.5f, 14.5f, 10.5f, 3);
        p.box(1.5f, 5.5f, 11.5f, 14.5f, 0.8f, 2, 0); p.frect(3.5f, 8, 6.5f, 10.5f, 1); p.frect(7.5f, 10.5f, 9.5f, 12.5f, 1); break;
    case Icon::VerifyAngle:
        p.line(2, 14, 14.5f, 14, 0, 1.2f); p.line(2, 14, 11, 4, 0, 1.2f);
        p.arc(2, 14, 7, 360 - 48, 360, 1, 1.3f); p.check(9.2f, 9.6f, 0.8f, 1, 1.5f); break;
    case Icon::Question: p.disc(8, 8, 6.5f, 2, 1); p.text(L"?", 1.5f, 1.4f, 14.5f, 14.4f, 9.5f, 1); break;
    case Icon::Warning:
        p.poly({8, 1.5f, 15, 14, 1, 14}, true, true, 2); p.poly({8, 1.5f, 15, 14, 1, 14}, true, false, 1, 1.1f);
        p.line(8, 6, 8, 9.8f, 1, 1.5f); p.fcircle(8, 12, 0.95f, 1); break;
    case Icon::Check: p.disc(8, 8, 6.5f, 2, 1); p.check(4.6f, 8.2f, 1.0f, 1, 1.7f); break;
    case Icon::Image: p.box(1.5f, 3, 14.5f, 13, 1, 2, 0); p.fcircle(4.2f, 5.6f, 0.9f, 1); p.fcircle(11.8f, 10.4f, 0.9f, 1); p.frect(6, 6.5f, 10, 9.5f, 0); break;
    case Icon::Link:
        p.rrect(1.5f, 5.5f, 9, 10.5f, 2.5f, 0, 1.3f); p.rrect(7, 5.5f, 14.5f, 10.5f, 2.5f, 1, 1.3f); break;
    case Icon::ErrorX: p.disc(8, 8, 6.5f, 2, 1); p.line(5.4f, 5.4f, 10.6f, 10.6f, 1, 1.7f); p.line(10.6f, 5.4f, 5.4f, 10.6f, 1, 1.7f); break;
    case Icon::CloseDoc:
        p.poly({3, 1.5f, 10, 1.5f, 13, 4.5f, 13, 14.5f, 3, 14.5f}, true, true, 3); p.poly({3, 1.5f, 10, 1.5f, 13, 4.5f, 13, 14.5f, 3, 14.5f}, true, false, 0);
        p.line(6, 7.5f, 10, 11.5f, 1, 1.5f); p.line(10, 7.5f, 6, 11.5f, 1, 1.5f); break;
    case Icon::CloseAll:
        p.rrect(4.5f, 1.5f, 14.5f, 11.5f, 1, 0, 0.9f);
        p.box(1.5f, 4.5f, 11.5f, 14.5f, 1, 3, 0); p.line(4.5f, 7.5f, 8.5f, 11.5f, 1, 1.5f); p.line(8.5f, 7.5f, 4.5f, 11.5f, 1, 1.5f); break;
    case Icon::Exit:
        p.poly({9.5f, 4, 9.5f, 1.5f, 2, 1.5f, 2, 14.5f, 9.5f, 14.5f, 9.5f, 12}, false, false, 0); p.frect(2, 1.5f, 6, 14.5f, 3);
        p.line(6.5f, 8, 14.5f, 8, 1, 1.4f); p.poly({11.5f, 5, 14.5f, 8, 11.5f, 11}, false, false, 1, 1.4f); break;
    case Icon::PrintSetup:
        p.box(1.5f, 5.5f, 14.5f, 11.5f, 1.2f, 3, 0); p.rect(4.5f, 1.5f, 11.5f, 5.5f, 0); p.rect(4.5f, 9.5f, 11.5f, 14.5f, 0);
        p.disc(12, 12, 3, 2, 1); p.fcircle(12, 12, 1, 1); break;
    case Icon::SelectAll:
        p.drect(1.5f, 1.5f, 14.5f, 14.5f, 0); p.box(4, 4, 7.5f, 7.5f, 0.6f, 2, 1); p.box(8.5f, 4, 12, 7.5f, 0.6f, 2, 1);
        p.box(4, 8.5f, 7.5f, 12, 0.6f, 2, 1); p.box(8.5f, 8.5f, 12, 12, 0.6f, 2, 1); break;
    case Icon::Keyboard:
        p.box(1, 4, 15, 12.5f, 1.4f, 3, 0);
        for (int i = 0; i < 5; ++i) p.frect(3 + i * 2.2f, 6, 4.2f + i * 2.2f, 7.2f, 0);
        for (int i = 0; i < 4; ++i) p.frect(4 + i * 2.2f, 8.2f, 5.2f + i * 2.2f, 9.4f, 0);
        p.line(5, 11, 11, 11, 1, 1.3f); break;
    case Icon::StatusBar: p.box(1.5f, 2.5f, 14.5f, 13.5f, 1.2f, 3, 0); p.frect(1.5f, 10.5f, 14.5f, 13.5f, 1); break;
    case Icon::Units:
        p.box(1, 5, 15, 11, 1, 2, 0);
        { for (int i = 0; i < 6; ++i) p.line(3 + i * 2, 5, 3 + i * 2, i % 2 ? 7 : 8.5f, i % 2 ? 0 : 1, 1); } break;
    case Icon::Sun:
        p.disc(8, 8, 3, 2, 1);
        for (int k = 0; k < 8; ++k) { float a = k * 0.7853982f, c = std::cos(a), si = std::sin(a); p.line(8 + c * 5, 8 + si * 5, 8 + c * 6.8f, 8 + si * 6.8f, 0, 1.2f); } break;
    case Icon::Moon:
        p.poly({10.5f, 1.8f, 7, 2.6f, 4, 5, 3, 8.5f, 4.4f, 12, 7.6f, 14.1f, 11.2f, 14, 14.2f, 11.6f, 11, 11.4f, 8.3f, 9.6f, 7.3f, 6.6f, 8.2f, 3.8f}, true, true, 2);
        p.poly({10.5f, 1.8f, 7, 2.6f, 4, 5, 3, 8.5f, 4.4f, 12, 7.6f, 14.1f, 11.2f, 14, 14.2f, 11.6f, 11, 11.4f, 8.3f, 9.6f, 7.3f, 6.6f, 8.2f, 3.8f}, true, false, 1, 1.1f); break;
    case Icon::Tag:
        p.poly({1.5f, 1.5f, 8, 1.5f, 14.5f, 8, 8, 14.5f, 1.5f, 8}, true, true, 2); p.poly({1.5f, 1.5f, 8, 1.5f, 14.5f, 8, 8, 14.5f, 1.5f, 8}, true, false, 0);
        p.disc(5, 5, 1.3f, 3, 1); break;
    case Icon::Eraser:
        p.poly({9, 1.8f, 14.5f, 7.3f, 8, 13.8f, 2.5f, 8.3f}, true, true, 3); p.poly({9, 1.8f, 14.5f, 7.3f, 8, 13.8f, 2.5f, 8.3f}, true, false, 0);
        p.poly({5.5f, 5.3f, 11, 10.8f, 8, 13.8f, 2.5f, 8.3f}, true, true, 1); p.line(8, 14.5f, 15, 14.5f, 0, 1); break;
    case Icon::Palette:
        p.disc(8, 8, 6.5f, 3, 0); p.fcircle(5, 6, 1.2f, 1); p.fcircle(8, 4.2f, 1.2f, 0); p.fcircle(11, 6, 1.2f, 1); p.fcircle(5.2f, 10, 1.2f, 0);
        p.disc(10.5f, 10.5f, 1.6f, 2, 0); break;
    case Icon::ArrowUp: p.line(8, 14, 8, 3, 0, 1.3f); p.poly({3.5f, 7, 8, 2.5f, 12.5f, 7}, false, false, 1, 1.5f); break;
    case Icon::ArrowDown: p.line(8, 2, 8, 13, 0, 1.3f); p.poly({3.5f, 9, 8, 13.5f, 12.5f, 9}, false, false, 1, 1.5f); break;
    case Icon::ArrowLeft: p.line(14, 8, 3, 8, 0, 1.3f); p.poly({7, 3.5f, 2.5f, 8, 7, 12.5f}, false, false, 1, 1.5f); break;
    case Icon::ArrowRight: p.line(2, 8, 13, 8, 0, 1.3f); p.poly({9, 3.5f, 13.5f, 8, 9, 12.5f}, false, false, 1, 1.5f); break;
    case Icon::Maximize:
        p.rect(2, 2, 14, 14, 0, 1.1f);
        p.poly({2.5f, 6, 2.5f, 2.5f, 6, 2.5f}, false, false, 1, 1.6f); p.poly({10, 2.5f, 13.5f, 2.5f, 13.5f, 6}, false, false, 1, 1.6f);
        p.poly({13.5f, 10, 13.5f, 13.5f, 10, 13.5f}, false, false, 1, 1.6f); p.poly({6, 13.5f, 2.5f, 13.5f, 2.5f, 10}, false, false, 1, 1.6f); break;
    case Icon::Polarity:   // R17c: package body with a polarity dot and a direction triangle
        p.chip(3, 3, 13, 13); p.fcircle(5.6f, 5.6f, 1.5f, 1); p.poly({7.5f, 10.5f, 11, 8, 7.5f, 5.5f}, true, true, 1); break;
    case Icon::OtherSide:  // R17c: two offset boards, the far one dashed (other side shown see-through)
        p.dline(5, 2, 14, 2, 1, 1); p.dline(14, 2, 14, 10, 1, 1); p.dline(5, 2, 5, 5, 1, 1); p.dline(11, 10, 14, 10, 1, 1);
        p.rect(2, 6, 11, 14, 0, 1.3f); break;
    case Icon::Plus: p.disc(8, 8, 6.5f, 2, 0); p.line(8, 4.5f, 8, 11.5f, 1, 1.6f); p.line(4.5f, 8, 11.5f, 8, 1, 1.6f); break;
    case Icon::Wand:
        p.line(2, 14, 10, 6, 0, 1.6f); p.line(10, 6, 11.5f, 4.5f, 1, 2.2f);
        p.line(13, 1, 13, 3.5f, 1, 1); p.line(11.8f, 2.2f, 14.2f, 2.2f, 1, 1); p.fcircle(6, 3, 0.8f, 1); p.fcircle(14, 9, 0.8f, 1); break;
    case Icon::Flip:
        p.dline(8, 1, 8, 15, 0, 1); p.poly({6, 3, 6, 13, 1.5f, 13}, true, true, 2); p.poly({6, 3, 6, 13, 1.5f, 13}, true, false, 0);
        p.poly({10, 3, 10, 13, 14.5f, 13}, true, true, 1); break;
    case Icon::Toolbar:
        p.box(1, 3, 15, 8, 1, 3, 0); p.frect(3, 4.8f, 4.5f, 6.3f, 1); p.frect(6, 4.8f, 7.5f, 6.3f, 0); p.frect(9, 4.8f, 10.5f, 6.3f, 0);
        p.rect(1, 10, 15, 13.5f, 0, 0.8f); break;
    case Icon::Windows:
        p.box(4.5f, 1.5f, 14.5f, 9.5f, 1, 3, 0); p.frect(4.5f, 1.5f, 14.5f, 3.5f, 0);
        p.box(1.5f, 6.5f, 11.5f, 14.5f, 1, 2, 0); p.frect(1.5f, 6.5f, 11.5f, 8.5f, 1); break;
    case Icon::Compact:
        p.box(3, 1.5f, 13, 14.5f, 1, 3, 0); p.poly({5.5f, 3.5f, 8, 6, 10.5f, 3.5f}, false, false, 1, 1.3f); p.poly({5.5f, 12.5f, 8, 10, 10.5f, 12.5f}, false, false, 1, 1.3f);
        p.line(5, 8, 11, 8, 0, 1); break;
    case Icon::Recent:
        p.disc(8, 8, 6.5f, 2, 0); p.line(8, 4, 8, 8, 1, 1.4f); p.line(8, 8, 10.8f, 9.8f, 1, 1.4f); break;
    case Icon::Folder:
        p.poly({1.5f, 3, 6, 3, 7.5f, 4.8f, 14.5f, 4.8f, 14.5f, 13.5f, 1.5f, 13.5f}, true, true, 2);
        p.poly({1.5f, 3, 6, 3, 7.5f, 4.8f, 14.5f, 4.8f, 14.5f, 13.5f, 1.5f, 13.5f}, true, false, 0); p.line(1.5f, 7, 14.5f, 7, 1, 1); break;
    // R20 file types (Files tab explorer)
    case Icon::FilePdf: file_icon(p, 0xFFD93025, "PDF"); break;
    case Icon::FileDoc: file_icon(p, 0xFF2B579A, "DOC"); break;
    case Icon::FileSheet: file_icon(p, 0xFF217346, "XLS"); break;
    case Icon::FileCsv: file_icon(p, 0xFF13856B, "CSV"); break;
    case Icon::FileZip: file_icon(p, 0xFFB7860B, "ZIP"); break;
    case Icon::FileGerber: file_icon(p, 0xFFB8652E, "GBR"); break;
    case Icon::FileDrill: file_icon(p, 0xFF546E7A, "DRL"); break;
    case Icon::FileImage: file_icon(p, 0xFF8E44AD, "IMG"); break;
    case Icon::FileEmail: file_icon(p, 0xFFE07B00, "EML"); break;
    case Icon::FileCad: file_icon(p, 0xFF3949AB, "CAD"); break;
    case Icon::FileBom: file_icon(p, 0xFF2E7D32, "BOM"); break;
    case Icon::FileText: file_icon(p, 0xFF607D8B, "TXT"); break;
    case Icon::FileGeneric: file_icon(p, 0, nullptr); break;
    case Icon::ViewDetails:
        for (float y : {3.f, 6.5f, 10.f, 13.5f}) { p.frect(1.5f, y - 1, 3.5f, y + 1, 1); p.line(5.5f, y, 14.5f, y, 0, 1.1f); }
        break;
    case Icon::ViewIcons:
        p.box(1.5f, 1.5f, 7, 7, 1, 2, 0); p.box(9, 1.5f, 14.5f, 7, 1, 3, 0); p.box(1.5f, 9, 7, 14.5f, 1, 3, 0); p.box(9, 9, 14.5f, 14.5f, 1, 2, 0);
        break;
    default: break;
    }
}

} // namespace

void draw_icon(ID2D1RenderTarget* rt, Icon id, const D2D1_RECT_F& r, uint32_t fg, uint32_t accent, float alpha) {
    if (id == Icon::None) return;
    P p{};
    p.rt = rt;
    const float w = r.right - r.left, h = r.bottom - r.top;
    p.s = std::min(w, h) / 16.f;
    p.ox = r.left + (w - 16.f * p.s) * 0.5f;
    p.oy = r.top + (h - 16.f * p.s) * 0.5f;
    p.sw = std::max(1.0f, 1.25f * p.s);
    p.alpha = alpha;
    rt->CreateSolidColorBrush(d2c(fg, alpha), p.fg.put());
    rt->CreateSolidColorBrush(d2c(accent, alpha), p.ac.put());
    rt->CreateSolidColorBrush(d2c(accent, alpha * 0.30f), p.tn.put());
    rt->CreateSolidColorBrush(d2c(fg, alpha * 0.13f), p.nt.put());
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                  D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    d2d_factory()->CreateStrokeStyle(sp, nullptr, 0, p.round.put());
    D2D1_STROKE_STYLE_PROPERTIES dp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                  D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_DASH);
    d2d_factory()->CreateStrokeStyle(dp, nullptr, 0, p.dash.put());
    D2D1_ANTIALIAS_MODE am = rt->GetAntialiasMode();
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    paint(p, id);
    rt->SetAntialiasMode(am);
}

static std::map<long long, HBITMAP>& bmp_cache() { static std::map<long long, HBITMAP> c; return c; }

HBITMAP icon_bitmap(Icon id, int size) {
    long long key = ((long long)id << 32) | ((long long)size << 1) | (theme_dark() ? 1 : 0);
    auto& c = bmp_cache();
    auto it = c.find(key);
    if (it != c.end()) return it->second;
    const Palette& pl = pal();
    uint32_t fg = colorref_to_argb(pl.text), ac = colorref_to_argb(pl.accent);
    HBITMAP b = render_to_hbitmap(size, size, [&](ID2D1RenderTarget* rt) {
        draw_icon(rt, id, D2D1::RectF(0, 0, (float)size, (float)size), fg, ac);
    });
    c[key] = b;
    return b;
}

void icon_cache_clear() {
    for (auto& kv : bmp_cache()) DeleteObject(kv.second);
    bmp_cache().clear();
}

HICON make_app_icon(int size) {
    HBITMAP color = render_to_hbitmap(size, size, [&](ID2D1RenderTarget* rt) {
        const float s = size / 16.f;
        Com<ID2D1SolidColorBrush> green, gold, dark, white;
        rt->CreateSolidColorBrush(d2c(0xFF1F7A4D), green.put());
        rt->CreateSolidColorBrush(d2c(0xFFE8B931), gold.put());
        rt->CreateSolidColorBrush(d2c(0xFF0D3B24), dark.put());
        rt->CreateSolidColorBrush(d2c(0xFFFFFFFF), white.put());
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f * s, 0.5f * s, 15.5f * s, 15.5f * s), 3 * s, 3 * s), green.get());
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f * s, 0.5f * s, 15.5f * s, 15.5f * s), 3 * s, 3 * s), dark.get(), std::max(1.f, 0.6f * s));
        // traces
        rt->DrawLine(D2D1::Point2F(3.5f * s, 4 * s), D2D1::Point2F(8 * s, 4 * s), gold.get(), 1.0f * s);
        rt->DrawLine(D2D1::Point2F(8 * s, 4 * s), D2D1::Point2F(12 * s, 8 * s), gold.get(), 1.0f * s);
        rt->DrawLine(D2D1::Point2F(3.5f * s, 12 * s), D2D1::Point2F(7 * s, 12 * s), gold.get(), 1.0f * s);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(3.5f * s, 4 * s), 1.6f * s, 1.6f * s), gold.get());
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(3.5f * s, 12 * s), 1.6f * s, 1.6f * s), gold.get());
        // plotter crosshair
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(11 * s, 11 * s), 3 * s, 3 * s), white.get(), std::max(1.f, 0.9f * s));
        rt->DrawLine(D2D1::Point2F(11 * s, 6.8f * s), D2D1::Point2F(11 * s, 15.2f * s), white.get(), std::max(1.f, 0.7f * s));
        rt->DrawLine(D2D1::Point2F(6.8f * s, 11 * s), D2D1::Point2F(15.2f * s, 11 * s), white.get(), std::max(1.f, 0.7f * s));
    });
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return ic;
}

} // namespace pcbp::ui
