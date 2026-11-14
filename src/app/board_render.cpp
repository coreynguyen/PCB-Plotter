// ============================================================================
// board_render.cpp — board renderer.
// ============================================================================
#include "board_render.h"
#include "../ui/theme.h"
#include "../core/package_edit.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace pcbp::app {
using namespace pcbp::ui;

uint32_t point_color(const Doc& d, int point) {
    if (point < 0 || point >= (int)d.prj.model.points.size()) return pal().unassigned;
    return d.prj.model.points[point].color | 0xFF000000u;
}

static D2D1_POINT_2F xfp(const D2D1_MATRIX_3X2_F& m, D2D1_POINT_2F p) { return D2D1::Point2F(p.x * m._11 + p.y * m._21 + m._31, p.x * m._12 + p.y * m._22 + m._32); }

static D2D1_MATRIX_3X2_F to_d2d(const Xform& x) { return D2D1::Matrix3x2F((float)x.a, (float)x.b, (float)x.c, (float)x.d, (float)x.tx, (float)x.ty); }

static void fill_poly(ID2D1RenderTarget* rt, const std::vector<Vec2>& pts, ID2D1Brush* b, bool fill, float w, bool closed = true, ID2D1StrokeStyle* ss = nullptr) {
    if (pts.size() < 2) return;
    Com<ID2D1PathGeometry> g;
    d2d_factory()->CreatePathGeometry(g.put());
    Com<ID2D1GeometrySink> k;
    g->Open(k.put());
    k->BeginFigure(D2D1::Point2F((float)pts[0].x, (float)pts[0].y), fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < pts.size(); ++i) k->AddLine(D2D1::Point2F((float)pts[i].x, (float)pts[i].y));
    k->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    k->Close();
    if (fill) rt->FillGeometry(g.get(), b);
    else rt->DrawGeometry(g.get(), b, w, ss);
}
// R16 (specs/OUTLINES.md): Inferred (unconfirmed) outlines are drawn dashed
// v17: the dash pattern is fixed in screen pixels (10 px on / 7 px off) whatever the stroke width: D2D dash
// lengths are in stroke widths, so the pattern is scaled by 1 / (stroke width in px). The halo under the
// dashes uses the same on/off phase so the line never reads as solid when zoomed out.
static ID2D1StrokeStyle* dash_style(float stroke_px = 3.f) {
    static std::map<int, Com<ID2D1StrokeStyle>> cache;
    const int key = (int)std::lround(std::clamp(stroke_px, 0.25f, 64.f) * 16.f);
    Com<ID2D1StrokeStyle>& s = cache[key];
    if (!s) {
        const float w = key / 16.f;
        const float dashes[2] = {10.f / w, 7.f / w};
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                      D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f);
        d2d_factory()->CreateStrokeStyle(sp, dashes, 2, s.put());
    }
    return s.get();
}

// R22: dashed polyline drawn as explicit segments (world units), so the dashes show on every D2D backend (a custom
// stroke-style dash pattern rendered solid under some drivers / Wine). on / off are world lengths.
static void dashed_poly(ID2D1RenderTarget* rt, const std::vector<Vec2>& pts, ID2D1SolidColorBrush* br, float w, double on, double off, bool closed) {
    if (pts.size() < 2 || !(on > 0) || !(off > 0)) return;
    const size_t n = pts.size();
    const size_t segs = closed && dist(pts.front(), pts.back()) > 1e-9 ? n : n - 1;
    double phase = 0;   // distance into the current on+off period
    for (size_t i = 0; i < segs; ++i) {
        const Vec2 a = pts[i], b = pts[(i + 1) % n];
        const double L = dist(a, b);
        if (L <= 0) continue;
        double t = 0;
        while (t < L) {
            const bool drawing = phase < on;
            const double step = std::min(L - t, drawing ? on - phase : on + off - phase);
            if (drawing) {
                const Vec2 p = a + (b - a) * (t / L), q = a + (b - a) * ((t + step) / L);
                rt->DrawLine(D2D1::Point2F((float)p.x, (float)p.y), D2D1::Point2F((float)q.x, (float)q.y), br, w);
            }
            t += step; phase += step;
            if (phase >= on + off - 1e-12) phase = 0;
        }
    }
}

// R22: diagonal hatch over an inferred outline's extent (lines x - y = const, clipped to the box)
static void hatch_box(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const BBox& bb, float hair, uint32_t argb) {
    if (bb.empty()) return;
    const float step = (float)std::max(std::max(bb.w(), bb.h()) / 36.0, 8.0 * hair);
    br->SetColor(d2c(argb, 0.45f));
    const float x0 = (float)bb.x0, y0 = (float)bb.y0, x1 = (float)bb.x1, y1 = (float)bb.y1;
    for (float k = x0 - (y1 - y0); k < x1; k += step) {
        float ax = k, ay = y0, bx = k + (y1 - y0), by = y1;
        if (ax < x0) { ay += x0 - ax; ax = x0; }
        if (bx > x1) { by -= bx - x1; bx = x1; }
        if (ax < bx) rt->DrawLine(D2D1::Point2F(ax, ay), D2D1::Point2F(bx, by), br, hair * 1.6f);
    }
}

static void draw_gprim(ID2D1RenderTarget* rt, const GPrim& g, ID2D1SolidColorBrush* b, float hair, bool fill_ok) {
    float w = g.width > 0 ? (float)g.width : hair;
    w = std::max(w, hair);
    D2D1_COLOR_F keep = b->GetColor();
    if (g.color) { D2D1_COLOR_F c = d2c(g.color); c.a = keep.a; b->SetColor(c); }
    switch (g.kind) {
    case GKind::Line: if (g.pts.size() >= 2) rt->DrawLine(D2D1::Point2F((float)g.pts[0].x, (float)g.pts[0].y), D2D1::Point2F((float)g.pts[1].x, (float)g.pts[1].y), b, w); break;
    case GKind::Polyline: fill_poly(rt, g.pts, b, false, w, false); break;
    case GKind::Polygon: fill_poly(rt, g.pts, b, g.filled && fill_ok, w, true); break;
    case GKind::Rect:
        if (g.pts.size() >= 2) {
            Vec2 c = (g.pts[0] + g.pts[1]) * 0.5;
            std::vector<Vec2> q = {{g.pts[0].x, g.pts[0].y}, {g.pts[1].x, g.pts[0].y}, {g.pts[1].x, g.pts[1].y}, {g.pts[0].x, g.pts[1].y}};
            for (auto& p : q) p = rotate_about(p, c, g.rot);
            fill_poly(rt, q, b, g.filled && fill_ok, w, true);
        }
        break;
    case GKind::Circle:
        if (!g.pts.empty()) {
            auto e = D2D1::Ellipse(D2D1::Point2F((float)g.pts[0].x, (float)g.pts[0].y), (float)g.radius, (float)g.radius);
            if (g.filled && fill_ok) rt->FillEllipse(e, b); else rt->DrawEllipse(e, b, w);
        }
        break;
    case GKind::Arc:
        if (!g.pts.empty()) {
            std::vector<Vec2> pts;
            int n = std::max(4, (int)(std::fabs(g.a1) / 6));
            for (int i = 0; i <= n; ++i) {
                double a = deg2rad(g.a0 + g.a1 * i / n);
                pts.push_back({g.pts[0].x + g.radius * std::cos(a), g.pts[0].y + g.radius * std::sin(a)});
            }
            fill_poly(rt, pts, b, false, w, false);
        }
        break;
    case GKind::Group: for (auto& c : g.children) draw_gprim(rt, c, b, hair, fill_ok); break;
    case GKind::Text: break;   // package text is not rendered at board scale
    }
    b->SetColor(keep);
}

// ---- pin-one / orientation marker (package template data, package coords) ----
static void draw_orient_marker(ID2D1RenderTarget* rt, const Package& pk, const Component& c, const BoardRenderOpts& o,
                               ID2D1SolidColorBrush* br, float hair, float alpha) {
    if (o.lead_labels && o.px_per_mm > 6) {
        // tiny pin-index ticks: dot count is unreadable, so mark pin 1 ring + every pad centre
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f * alpha));
        for (auto& d : pk.pads) rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F((float)d.pos.x, (float)d.pos.y), hair * 1.5f, hair * 1.5f), br);
    }
    // R15: the package's own polarity marker (specs/POLARITY.md), drawn in package coordinates, so the placement
    // transform (rotation, bottom-side mirror, image step) applies. No generic auto pin-1 dot any more.
    (void)c;
    if (o.orient_markers <= 0 || !pkg_has_polarity(pk)) return;
    const bool auto_size = pk.marker_size <= 0;
    double size = auto_size ? pkg_marker_auto(pk).size : pk.marker_size;
    if (o.px_per_mm > 0) {
        const double lo = kMarkerMinPx * o.dip_per_px / o.px_per_mm, hi = kMarkerMaxPx * o.dip_per_px / o.px_per_mm;
        size = auto_size ? std::clamp(size, lo, hi) : std::max(size, lo);
    }
    const uint32_t col = pk.marker_color ? pk.marker_color : 0xFFE03131;
    br->SetColor(D2D1::ColorF(((col >> 16) & 255) / 255.f, ((col >> 8) & 255) / 255.f, (col & 255) / 255.f, alpha));
    const float r = (float)size;
    const Vec2 at = pk.marker_pos;
    switch ((MarkerKind)pk.marker_kind) {
    case MarkerKind::Arrow: {
        const Vec2 dir{std::cos(deg2rad(pk.marker_rot)), std::sin(deg2rad(pk.marker_rot))}, n{-dir.y, dir.x};
        const Vec2 t = at, tail = t - dir * (r * 3.0), b1 = t - dir * (r * 1.4) + n * (r * 0.9), b2 = t - dir * (r * 1.4) - n * (r * 0.9);
        fill_poly(rt, {t, b1, b2}, br, true, hair);
        rt->DrawLine(D2D1::Point2F((float)tail.x, (float)tail.y), D2D1::Point2F((float)(t.x - dir.x * r), (float)(t.y - dir.y * r)), br, std::max(hair * 2, r * 0.35f));
    } break;
    case MarkerKind::Custom:
        if (!pk.marker_art.empty()) { for (auto& a : pk.marker_art) draw_gprim(rt, a, br, hair * 1.5f, true); break; }
        [[fallthrough]];
    default:
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F((float)at.x, (float)at.y), r, r), br);
        break;
    }
}

static void pad_shape(ID2D1RenderTarget* rt, const Pad& p, ID2D1Brush* b) {
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    rt->SetTransform(D2D1::Matrix3x2F::Rotation((float)p.rot) * D2D1::Matrix3x2F::Translation((float)p.pos.x, (float)p.pos.y) * old);
    const float hw = (float)p.w / 2, hh = (float)p.h / 2;
    switch (p.shape) {
    case PadShape::Round: rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), hw, hh), b); break;
    case PadShape::Oblong: { float r = std::min(hw, hh); rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(-hw, -hh, hw, hh), r, r), b); break; }
    default: rt->FillRectangle(D2D1::RectF(-hw, -hh, hw, hh), b); break;
    }
    rt->SetTransform(old);
}

BoardUnderlayHook g_board_underlay = nullptr;

void render_board(ID2D1RenderTarget* rt, const Doc& d, const BoardRenderOpts& o) {
    const Project& p = d.prj;
    const Palette& pl = pal();
    Com<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(d2c(0xFFFFFFFF), br.put());
    const float hair = (float)(1.0 / std::max(o.px_per_mm, 1e-6)) * o.dip_per_px;
    rt->SetTransform(o.world);

    const int nimg = o.all_images ? (int)p.images.size() : 1;
    // board fill + frame per image
    BBox bb = p.board_bbox();
    for (int im = 0; im < nimg; ++im) {
        D2D1_MATRIX_3X2_F m = to_d2d(p.image_xform(im)) * o.world;
        rt->SetTransform(m);
        br->SetColor(d2c(o.print_mode ? 0xFFF4F7F4 : pl.board_fill, p.images[im].x_out ? 0.35f : 1.f));
        // fill the largest closed outline if it covers most of the board, else the extent
        const std::vector<Vec2>* best = nullptr;
        double best_a = 0;
        for (auto& poly : p.board_frame) {
            if (poly.size() < 3 || dist(poly.front(), poly.back()) > 0.01) continue;
            double a = 0;
            for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) a += (poly[j].x * poly[i].y - poly[i].x * poly[j].y);
            a = std::fabs(a) * 0.5;
            if (a > best_a) { best_a = a; best = &poly; }
        }
        if (best && best_a > 0.5 * bb.w() * bb.h()) fill_poly(rt, *best, br.get(), true, hair);
        else rt->FillRectangle(D2D1::RectF((float)bb.x0, (float)bb.y0, (float)bb.x1, (float)bb.y1), br.get());
        if (o.show_frame) {
            // R22 (REVIEW_V21 §2.1): confirmed / imported outline = solid green; inferred (estimated) outline = the
            // same green but thick, dashed and diagonally hatched, so it never reads as measured geometry
            const bool inferred = p.circuit_outline_source() == OutlineSource::Inferred;
            const uint32_t green = p.frame_color ? p.frame_color : 0xFF2F9E44;
            if (inferred && !o.print_mode) hatch_box(rt, br.get(), bb, hair, 0xFF8CE99A);
            const float lw = inferred ? hair * 4.f : hair * 2.f;
            const double dash = hair * 12.0, gap = hair * 8.0;   // 12 px on / 8 px off at any zoom
            const std::vector<std::vector<Vec2>> polys = !p.board_frame.empty() ? p.board_frame
                : std::vector<std::vector<Vec2>>{{{bb.x0, bb.y0}, {bb.x1, bb.y0}, {bb.x1, bb.y1}, {bb.x0, bb.y1}, {bb.x0, bb.y0}}};
            for (int pass = inferred ? 0 : 1; pass < 2; ++pass) {
                if (pass == 0) br->SetColor(d2c(0xFF06140A, 0.55f));
                else br->SetColor(d2c(inferred ? 0xFF40C057 : green));
                for (auto& poly : polys) {
                    const bool cl = poly.size() > 2 && dist(poly.front(), poly.back()) < 0.01;
                    if (inferred && pass == 1) dashed_poly(rt, poly, br.get(), lw, dash, gap, cl);
                    else fill_poly(rt, poly, br.get(), false, pass == 0 ? lw * 1.8f : lw, cl);
                }
            }
        }
        if (p.images[im].x_out) {
            br->SetColor(d2c(pl.error_red));
            rt->DrawLine(D2D1::Point2F((float)bb.x0, (float)bb.y0), D2D1::Point2F((float)bb.x1, (float)bb.y1), br.get(), hair * 3);
            rt->DrawLine(D2D1::Point2F((float)bb.x0, (float)bb.y1), D2D1::Point2F((float)bb.x1, (float)bb.y0), br.get(), hair * 3);
        }
    }
    rt->SetTransform(o.world);
    if (g_board_underlay && !p.board_images.empty()) { g_board_underlay(rt, d, o); rt->SetTransform(o.world); }
    if (o.show_frame && !p.panel_frame.empty()) {   // R22: board outline, same convention (solid green / hatched dashed green)
        const bool inferred = p.panel_outline_src == OutlineSource::Inferred;
        const uint32_t green = p.frame_color ? p.frame_color : 0xFF2F9E44;
        if (inferred && !o.print_mode) hatch_box(rt, br.get(), p.panel_frame_bbox(), hair, 0xFF8CE99A);
        const float lw = inferred ? hair * 4.f : hair * 2.5f;
        for (int pass = inferred ? 0 : 1; pass < 2; ++pass) {
            br->SetColor(pass == 0 ? d2c(0xFF06140A, 0.55f) : d2c(inferred ? 0xFF40C057 : green));
            for (auto& poly : p.panel_frame) {
                const bool cl = poly.size() > 2 && dist(poly.front(), poly.back()) < 0.01;
                if (inferred && pass == 1) dashed_poly(rt, poly, br.get(), lw, hair * 12.0, hair * 8.0, cl);
                else fill_poly(rt, poly, br.get(), false, pass == 0 ? lw * 1.8f : lw, cl);
            }
        }
    }

    // components: draw back side first so the visible side is on top
    for (int pass = 0; pass < 2; ++pass) {
        const Side side = (pass == 0) == !o.mirror ? Side::Bottom : Side::Top;
        const bool far_side = (side == Side::Bottom) != o.mirror;
        const bool shown = side == Side::Top ? o.show_top : o.show_bottom;
        if (!shown && !(far_side && o.ghost_far)) continue;
        if (far_side && !o.second_side && !o.ghost_far) continue;
        for (int im = 0; im < nimg; ++im) {
            for (size_t ci = 0; ci < p.comps.size(); ++ci) {
                const Component& c = p.comps[ci];
                if (c.side != side) continue;
                if (im > 0 && !p.instanced(c)) continue;      // global fiducials: once, panel coords
                const bool focus = o.focus_point < 0 || c.assigned_point == o.focus_point;
                if (o.only_focus && !focus) continue;
                const Package* pk = p.package_of(c);
                rt->SetTransform(to_d2d(p.instance_xform(c, im)) * o.world);
                uint32_t col = c.feature == Feature::Fiducial ? pl.fiducial : point_color(d, c.assigned_point);
                float alpha = far_side ? o.far_alpha : 1.f;
                if (!focus) { col = pl.unassigned; alpha *= 0.35f; }
                if (o.highlight_unassigned && c.feature == Feature::Component && c.assigned_point < 0) { col = pl.error_red; alpha = 1.f; }
                if (o.highlight_noref && c.ref.empty()) { col = pl.error_red; alpha = 1.f; }
                if (o.angle_errors && ci < o.angle_errors->size() && (*o.angle_errors)[ci]) { col = pl.error_red; alpha = 1.f; }
                if (c.dnp) alpha *= 0.4f;
                if (c.feature == Feature::Fiducial) {
                    double r = 0.5;
                    if (pk && !pk->pads.empty()) r = std::max(pk->pads[0].w, pk->pads[0].h) / 2;
                    br->SetColor(d2c(pl.pad_smt, alpha));
                    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), (float)r, (float)r), br.get());
                    br->SetColor(d2c(col, alpha));
                    // R11 P2-03: fiducial = DIAMOND + cross (the board origin is a circle + cross); circuit fiducials get a
                    // second diamond, component fiducials an outer square; the number is drawn beside it (below).
                    const float R = (float)(r * 2.1), X = (float)(r * 3.0);
                    const D2D1_POINT_2F dmd[4] = {{R, 0}, {0, R}, {-R, 0}, {0, -R}};
                    for (int q = 0; q < 4; ++q) rt->DrawLine(dmd[q], dmd[(q + 1) % 4], br.get(), hair * 1.6f);
                    rt->DrawLine(D2D1::Point2F(-X, 0), D2D1::Point2F(X, 0), br.get(), hair * 1.2f);
                    rt->DrawLine(D2D1::Point2F(0, -X), D2D1::Point2F(0, X), br.get(), hair * 1.2f);
                    if (c.fid_scope == FiducialScope::Image) {
                        const float R2 = R * 1.3f;
                        const D2D1_POINT_2F d2[4] = {{R2, 0}, {0, R2}, {-R2, 0}, {0, -R2}};
                        for (int q = 0; q < 4; ++q) rt->DrawLine(d2[q], d2[(q + 1) % 4], br.get(), hair);
                    } else if (c.fid_scope == FiducialScope::Local) rt->DrawRectangle(D2D1::RectF(-R * 1.15f, -R * 1.15f, R * 1.15f, R * 1.15f), br.get(), hair);
                } else {
                    BBox e = pk ? pk->extent() : BBox(-0.4, -0.4, 0.4, 0.4);
                    // body fill (colour-coded)
                    br->SetColor(d2c(col, 0.38f * alpha));
                    if (pk && !pk->body.empty()) {
                        BBox be = gprims_bbox(pk->body);
                        if (be.empty()) be = e;
                        rt->FillRectangle(D2D1::RectF((float)be.x0, (float)be.y0, (float)be.x1, (float)be.y1), br.get());
                    } else rt->FillRectangle(D2D1::RectF((float)e.x0, (float)e.y0, (float)e.x1, (float)e.y1), br.get());
                    if (o.show_pads && pk) {
                        br->SetColor(d2c(pk->smt ? pl.pad_smt : pl.pad_th, alpha));
                        for (auto& pad : pk->pads) pad_shape(rt, pad, br.get());
                    }
                    br->SetColor(d2c(col, alpha));
                    if (pk && !pk->body.empty()) for (auto& g : pk->body) draw_gprim(rt, g, br.get(), hair * 1.2f, false);
                    else rt->DrawRectangle(D2D1::RectF((float)e.x0, (float)e.y0, (float)e.x1, (float)e.y1), br.get(), hair * 1.2f);
                    if (pk) draw_orient_marker(rt, *pk, c, o, br.get(), hair, alpha);
                }
                if (o.show_selection && c.selected) {
                    BBox e = pk ? pk->extent() : BBox(-0.5, -0.5, 0.5, 0.5);
                    if (c.feature == Feature::Fiducial) e = BBox(-1.2, -1.2, 1.2, 1.2);
                    e.inflate(0.15);
                    br->SetColor(d2c(pl.selection));
                    rt->DrawRectangle(D2D1::RectF((float)e.x0, (float)e.y0, (float)e.x1, (float)e.y1), br.get(), hair * 2.2f);
                }
                if (o.verify_angles && c.feature == Feature::Component) {
                    BBox e = pk ? pk->extent() : BBox(-0.5, -0.5, 0.5, 0.5);
                    float L = (float)std::max(e.w(), e.h()) * 0.5f;
                    br->SetColor(d2c(0xFF22B8CF));
                    rt->DrawLine(D2D1::Point2F(0, 0), D2D1::Point2F(L, 0), br.get(), hair * 2);
                    rt->DrawLine(D2D1::Point2F(L, 0), D2D1::Point2F(L * 0.7f, L * 0.2f), br.get(), hair * 2);
                    rt->DrawLine(D2D1::Point2F(L, 0), D2D1::Point2F(L * 0.7f, -L * 0.2f), br.get(), hair * 2);
                }
            }
        }
    }

    // fiducial numbers (screen-aligned, always: they identify the alignment marks)
    if (o.px_per_mm > 1.0) {
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        for (auto& c : p.comps) {
            if (c.feature != Feature::Fiducial || c.fid_number <= 0) continue;
            const bool shown = c.side == Side::Top ? o.show_top : o.show_bottom;
            if (!shown) continue;
            const Package* pk = p.package_of(c);
            double r = 0.5;
            if (pk && !pk->pads.empty()) r = std::max(pk->pads[0].w, pk->pads[0].h) / 2;
            D2D1_POINT_2F sp = xfp(o.world, D2D1::Point2F((float)c.pos.x, (float)c.pos.y));
            const float off = (float)(r * 2.6 * o.px_per_mm) + 3.f, fs = 13.f;
            br->SetColor(d2c(o.print_mode ? 0xFF000000 : pl.fiducial, 1.f));
            std::wstring t = L"F" + std::to_wstring(c.fid_number);
            rt->DrawText(t.c_str(), (UINT32)t.size(), shared_text_format(fs, true, 0), D2D1::RectF(sp.x + off * 0.7f, sp.y - off - fs, sp.x + off * 0.7f + 60, sp.y - off + 4), br.get());
        }
    }
    // labels (screen-aligned text)
    if (o.show_labels && o.px_per_mm > 2.0) {
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        for (int im = 0; im < nimg; ++im) {
            for (auto& c : p.comps) {
                if (c.ref.empty()) continue;
                if (im > 0 && !p.instanced(c)) continue;
                const bool lfar = (c.side == Side::Bottom) != o.mirror;
                const bool lshown = c.side == Side::Top ? o.show_top : o.show_bottom;
                if (!lshown && !(lfar && o.ghost_far)) continue;
                if (o.only_focus && o.focus_point >= 0 && c.assigned_point != o.focus_point) continue;
                const Package* pk = p.package_of(c);
                BBox e = pk ? pk->extent() : BBox(-0.5, -0.5, 0.5, 0.5);
                double size_mm = std::clamp(std::min(e.w(), e.h()) * 0.55, 0.35, 2.0);
                float fs = (float)(size_mm * o.px_per_mm);
                if (fs < 6.f) continue;
                fs = std::min(fs, 28.f);
                Vec2 wp = p.image_xform(im).apply(c.pos + c.label_off);
                D2D1_POINT_2F s = xfp(o.world, D2D1::Point2F((float)wp.x, (float)wp.y));
                const bool far_side = (c.side == Side::Bottom) != o.mirror;
                if (far_side && !o.second_side && !o.ghost_far) continue;
                br->SetColor(d2c(o.print_mode ? 0xFF000000 : pl.label, far_side ? std::min(0.95f, o.far_alpha + 0.1f) : 0.95f));
                std::wstring t = W(c.ref);
                IDWriteTextFormat* f = shared_text_format(fs, true, 1);
                rt->DrawText(t.c_str(), (UINT32)t.size(), f, D2D1::RectF(s.x - 200, s.y - fs, s.x + 200, s.y + fs), br.get());
            }
        }
    }

    // origin
    if (o.show_origin) {
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        D2D1_POINT_2F s = xfp(o.world, D2D1::Point2F((float)p.virtual_origin.x, (float)p.virtual_origin.y));
        br->SetColor(d2c(pl.origin));
        rt->DrawEllipse(D2D1::Ellipse(s, 7, 7), br.get(), 1.5f);
        rt->DrawLine(D2D1::Point2F(s.x - 12, s.y), D2D1::Point2F(s.x + 12, s.y), br.get(), 1.2f);
        rt->DrawLine(D2D1::Point2F(s.x, s.y - 12), D2D1::Point2F(s.x, s.y + 12), br.get(), 1.2f);
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

int hit_component(const Doc& d, Vec2 pt, double tol, bool top_vis, bool bot_vis) {
    int best = -1;
    double best_area = 1e30;
    for (size_t i = 0; i < d.prj.comps.size(); ++i) {
        const Component& c = d.prj.comps[i];
        if (c.side == Side::Top && !top_vis) continue;
        if (c.side == Side::Bottom && !bot_vis) continue;
        const Package* pk = d.prj.package_of(c);
        BBox e = pk ? pk->extent() : BBox(-0.5, -0.5, 0.5, 0.5);
        if (c.feature == Feature::Fiducial) e = BBox(-1, -1, 1, 1);
        e.inflate(tol);
        Vec2 local = d.prj.comp_xform(c, 0).inverse().apply(pt);
        if (e.contains(local)) {
            double a = e.w() * e.h();
            if (a < best_area) { best_area = a; best = (int)i; }
        }
    }
    return best;
}

int hit_pad(const Doc& d, const Component& c, Vec2 pt, double tol) {
    const Package* pk = d.prj.package_of(c);
    if (!pk) return -1;
    Vec2 local = d.prj.comp_xform(c, 0).inverse().apply(pt);
    int best = -1;
    double bd = 1e30;
    for (size_t i = 0; i < pk->pads.size(); ++i) {
        const Pad& p = pk->pads[i];
        Vec2 q = rotate(local - p.pos, -p.rot);
        if (std::fabs(q.x) <= p.w / 2 + tol && std::fabs(q.y) <= p.h / 2 + tol) {
            double dd = dist(local, p.pos);
            if (dd < bd) { bd = dd; best = (int)i; }
        }
    }
    return best;
}

Vec2 pad_world(const Doc& d, const Component& c, int pad) {
    const Package* pk = d.prj.package_of(c);
    if (!pk || pad < 0 || pad >= (int)pk->pads.size()) return c.pos;
    return d.prj.comp_xform(c, 0).apply(pk->pads[pad].pos);
}

} // namespace pcbp::app
