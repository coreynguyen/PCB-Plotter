// ============================================================================
// package_library.cpp — Packages view: package library browser + package
// editor + template wizard (CircuitCAM "Package Library" / Package Type Editor).
//
//   [ library browser ] [ header: name + chips ............ grid/snap  ]
//   [ search / sort    ] [                                              ]
//   [ filter chips     ] [   GCanvas editor (pads, pins, body, rulers,  ]
//   [ thumbnails/list  ] [   dimensions, measure, HUD)                  ]
//   [ footer actions   ] [                                              ]
//                                              [ inspector | pads | templates ]
//
// Logic ported from legacy/app/package_editor.cpp (render body, renumber,
// rotate with compensation, centre origin). All edits go through
// doc->checkpoint() / doc->changed(CH_GEOMETRY).
// ============================================================================
#include "../io/cad_common.h"
#include "gapp.h"
#include "gcanvas.h"
#include "gfx.h"
#include "../core/project_io.h"
#include "../core/pkg_library.h"
#include "../core/package_edit.h"
#include "../core/package_resolve.h"   // R16b
#include "pkg_wizard.h"
#include "../io/digitize.h"
#include "../ui/d2d.h"
#include "../ui/form.h"
#include "../ui/theme.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <shlobj.h>

namespace pcbp::gui { std::vector<ui::Form*>& modal_stack(); }   // gform.cpp
namespace pcbp::gui { void draw_machine_data(const app::DocPtr& d, const std::vector<std::string>& names, const std::string& cur, const std::function<void(const std::string&)>& pick); }   // R24b machine_data_ui.cpp
namespace pcbp::gui {
HWND main_hwnd();

namespace {
using app::Doc;
using app::DocPtr;
using namespace pcbp::app;

// ===========================================================================
// small helpers
// ===========================================================================
ImU32 im_argb(uint32_t c, float a = 1.f) {
    return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, (int)(((c >> 24) & 255) * a));
}
ImU32 with_alpha(ImU32 c, int a) { return (c & 0x00FFFFFF) | ((ImU32)a << 24); }
D2D1_COLOR_F d2_im(ImU32 c) {
    return D2D1::ColorF((c & 255) / 255.f, ((c >> 8) & 255) / 255.f, ((c >> 16) & 255) / 255.f, ((c >> 24) & 255) / 255.f);
}
Units disp_units() { return (Units)app::settings().units; }
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
std::string mm(double v) { return digitize::fmt_mm(v, 2); }

const char* kShapeNames[] = {"Round", "Square", "Rect", "Oblong", "Octagon"};

// R18c: process = the package's SMT / through-hole flag. Importers set it from drilled leads / holes; the user can
// change it in the header bar ("Detect from drilled leads" re-derives it).
bool is_th(const Package& p) { return !p.smt; }

BBox pad_bbox(const Pad& p, Vec2 off = {}) {
    BBox b;
    const double hw = p.w * 0.5, hh = p.h * 0.5;
    Vec2 c[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    for (auto& q : c) b.add(rotate(q, p.rot) + p.pos + off);
    return b;
}
bool pad_hit(const Pad& p, Vec2 w, double tol) {
    Vec2 l = rotate(w - p.pos, -p.rot);
    return std::fabs(l.x) <= p.w * 0.5 + tol && std::fabs(l.y) <= p.h * 0.5 + tol;
}
int pin1_index(const Package& pk) {
    for (size_t i = 0; i < pk.pads.size(); ++i) if (pk.pads[i].pin == 1) return (int)i;
    // R13 (P1-05): no fallback to the first pad. After pin 1 is deleted, the first remaining lead
    // used to be drawn as pin 1 (pink lead + marker in an unexpected place) although its number
    // never changed. Now: no pin 1 means no pin-1 decoration, and the editor says so.
    return -1;
}
int max_pin(const Package& pk) { int m = 0; for (auto& p : pk.pads) m = std::max(m, p.pin); return m; }

// ---- geometry edits on primitives ------------------------------------------
void rotate_prim(GPrim& g, double deg) {
    switch (g.kind) {
    case GKind::Rect:
        if (g.pts.size() >= 2) {
            Vec2 c = (g.pts[0] + g.pts[1]) * 0.5, h = (g.pts[1] - g.pts[0]) * 0.5;
            Vec2 c2 = rotate(c, deg);
            g.pts[0] = c2 - h; g.pts[1] = c2 + h;
            g.rot = norm_deg(g.rot + deg);
        }
        break;
    case GKind::Circle: case GKind::Arc:
        if (!g.pts.empty()) g.pts[0] = rotate(g.pts[0], deg);
        g.a0 = norm_deg(g.a0 + deg);
        break;
    case GKind::Text:
        if (!g.pts.empty()) g.pts[0] = rotate(g.pts[0], deg);
        g.rot = norm_deg(g.rot + deg);
        break;
    case GKind::Group: for (auto& c : g.children) rotate_prim(c, deg); break;
    default: for (auto& p : g.pts) p = rotate(p, deg); break;
    }
}
void offset_prim(GPrim& g, Vec2 d) {
    for (auto& p : g.pts) p += d;
    for (auto& c : g.children) offset_prim(c, d);
}
void mirror_prim(GPrim& g) {   // about the Y axis (x -> -x)
    for (auto& p : g.pts) p.x = -p.x;
    switch (g.kind) {
    case GKind::Rect: if (g.pts.size() >= 2) std::swap(g.pts[0].x, g.pts[1].x); g.rot = norm_deg(-g.rot); break;
    case GKind::Arc: g.a0 = norm_deg(180 - (g.a0 + g.a1)); break;
    case GKind::Text: g.rot = norm_deg(-g.rot); break;
    case GKind::Group: for (auto& c : g.children) { for (auto& p : c.pts) p.x = -p.x; } break;
    default: break;
    }
}

// Auto-render artwork (manual Package Library: SMT, Notched, J-Lead, Radial).
// R15: a derived body is a SUGGESTION. It may only fill an empty body or replace an earlier derived one;
// a CAD outline / CAD extent / user-drawn body is never replaced (returns false, nothing changed).
bool render_body(Package& pk) {
    if (!pkg_body_replaceable(pk)) return false;
    if (pk.pads.empty()) { pk.auto_body(); return true; }
    BBox pb, cb;
    for (auto& p : pk.pads) { pb.add(pad_bbox(p)); cb.add(p.pos); }
    const Vec2 c = pb.center();
    const int p1 = pin1_index(pk);
    const Vec2 p1pos = p1 >= 0 ? pk.pads[p1].pos : c;
    double minpad = 1e9;
    for (auto& p : pk.pads) minpad = std::min(minpad, std::min(p.w, p.h));
    const double lw = 0.12;
    std::vector<GPrim> body;
    if (pk.radial) {
        double r = 0;
        for (auto& p : pk.pads) r = std::max(r, dist(c, p.pos) + std::max(p.w, p.h) * 0.5);
        r += 0.3;
        GPrim g; g.kind = GKind::Circle; g.pts = {c}; g.radius = r; g.width = lw;
        body.push_back(g);
        if (pkg_has_polarity(pk) && p1 >= 0) {
            Vec2 dir = p1pos - c;
            double l = dir.len();
            if (l > 1e-6) {
                dir = dir * (1.0 / l);
                Vec2 n{-dir.y, dir.x};
                Vec2 m = c + dir * (r * 0.7);
                double hl = std::sqrt(std::max(0.0, r * r - (r * 0.7) * (r * 0.7)));
                GPrim ln; ln.kind = GKind::Line; ln.pts = {m - n * hl, m + n * hl}; ln.width = lw;
                body.push_back(ln);
            }
        }
        pk.body = body;
        pk.body_source = "pads";     // rendered from the leads: approximate
        return true;
    }
    BBox b;
    if (pk.j_lead) { b = pb; b.inflate(0.1); }
    else if (pk.smt) {
        if (pk.pads.size() <= 3) { pk.auto_body(); b = gprims_bbox(pk.body); }
        else { b = cb; b.inflate(std::max(0.05, minpad * 0.25)); }
    } else { b = pb; b.inflate(0.4); }
    if (b.empty()) b = pb;
    GPrim r; r.kind = GKind::Rect; r.pts = {{b.x0, b.y0}, {b.x1, b.y1}}; r.width = lw;
    body.push_back(r);
    const double sm = std::min(b.w(), b.h());
    if (pk.notched) {
        GPrim a; a.kind = GKind::Arc; a.width = lw; a.a1 = 180;
        a.radius = std::clamp(sm * 0.15, 0.2, 1.5);
        if (b.w() <= b.h()) {
            if (p1pos.y >= c.y) { a.pts = {{c.x, b.y1}}; a.a0 = 180; }
            else { a.pts = {{c.x, b.y0}}; a.a0 = 0; }
        } else {
            if (p1pos.x <= c.x) { a.pts = {{b.x0, c.y}}; a.a0 = -90; }
            else { a.pts = {{b.x1, c.y}}; a.a0 = 90; }
        }
        body.push_back(a);
    }
    if (false && pk.pin_marked && p1 >= 0) {   // R15: orientation is shown by the polarity marker, not body art
        // style: filled triangle in the body corner nearest pin 1
        const bool left = p1pos.x <= c.x, top = p1pos.y >= c.y;
        const Vec2 corner{left ? b.x0 : b.x1, top ? b.y1 : b.y0};
        const double sx = left ? 1 : -1, sy = top ? -1 : 1;
        const double in = std::clamp(sm * 0.05, 0.05, 0.4), leg = std::clamp(sm * 0.25, 0.25, 2.0);
        GPrim g; g.kind = GKind::Polygon; g.filled = true; g.width = lw;
        g.pts = {corner + Vec2{sx * in, sy * in}, corner + Vec2{sx * (in + leg), sy * in}, corner + Vec2{sx * in, sy * (in + leg)}};
        body.push_back(g);
    }
    if (pkg_has_polarity(pk) && pk.pads.size() == 2 && p1 >= 0) {
        GPrim ln; ln.kind = GKind::Line; ln.width = lw * 2;
        if (b.w() >= b.h()) { double x = p1pos.x < c.x ? b.x0 + b.w() * 0.18 : b.x1 - b.w() * 0.18; ln.pts = {{x, b.y0}, {x, b.y1}}; }
        else { double y = p1pos.y < c.y ? b.y0 + b.h() * 0.18 : b.y1 - b.h() * 0.18; ln.pts = {{b.x0, y}, {b.x1, y}}; }
        body.push_back(ln);
    }
    pk.body = body;
    pk.body_source = "pads";         // rendered from the leads: approximate
    return true;
}

// renumber modes
void renumber_ccw(Package& pk) {
    if (pk.pads.empty()) return;
    BBox cb;
    for (auto& pd : pk.pads) cb.add(pd.pos);
    const Vec2 c = cb.center();
    int first = -1;
    for (size_t i = 0; i < pk.pads.size(); ++i) if (pk.pads[i].pin == 1) { first = (int)i; break; }
    if (first < 0) {
        double best = 1e300;
        for (size_t i = 0; i < pk.pads.size(); ++i) {
            double k = pk.pads[i].pos.x - pk.pads[i].pos.y * 1.0001;
            if (k < best) { best = k; first = (int)i; }
        }
    }
    auto ang = [&](const Pad& pd) { return std::atan2(pd.pos.y - c.y, pd.pos.x - c.x); };
    const double a1 = ang(pk.pads[first]);
    std::vector<int> order(pk.pads.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    auto key = [&](int i) {
        if (i == first) return -1.0;
        double a = ang(pk.pads[i]) - a1;
        while (a < -1e-9) a += 2 * kPi;
        while (a >= 2 * kPi - 1e-9) a -= 2 * kPi;
        return a;
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        double ka = key(a), kb = key(b);
        if (std::fabs(ka - kb) > 1e-6) return ka < kb;
        return dist(pk.pads[a].pos, c) > dist(pk.pads[b].pos, c);
    });
    for (size_t k = 0; k < order.size(); ++k) pk.pads[order[k]].pin = (int)k + 1;
}
void renumber_rows(Package& pk, bool by_columns) {
    if (pk.pads.empty()) return;
    double tol = 1e9;
    for (auto& p : pk.pads) tol = std::min(tol, std::min(p.w, p.h) * 0.5);
    tol = std::max(tol, 0.01);
    std::vector<int> order(pk.pads.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const Vec2 pa = pk.pads[a].pos, pb = pk.pads[b].pos;
        if (by_columns) { if (std::fabs(pa.x - pb.x) > tol) return pa.x < pb.x; return pa.y > pb.y; }
        if (std::fabs(pa.y - pb.y) > tol) return pa.y > pb.y;
        return pa.x < pb.x;
    });
    for (size_t k = 0; k < order.size(); ++k) pk.pads[order[k]].pin = (int)k + 1;
}

// most common nearest-neighbour centre distance
double pad_pitch(const Package& pk) {
    const size_t n = std::min<size_t>(pk.pads.size(), 600);
    if (n < 2) return 0;
    std::map<long, int> hist;
    for (size_t i = 0; i < n; ++i) {
        double best = 1e300;
        for (size_t j = 0; j < n; ++j) if (i != j) best = std::min(best, dist(pk.pads[i].pos, pk.pads[j].pos));
        if (best < 1e299) hist[std::lround(best * 100)]++;
    }
    long bk = 0; int bc = 0;
    for (auto& kv : hist) if (kv.second > bc) { bc = kv.second; bk = kv.first; }
    return bk / 100.0;
}

uint64_t pkg_hash(const Package& p) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](double v) { int64_t q = std::llround(v * 10000); h ^= (uint64_t)q; h *= 1099511628211ull; };
    for (auto& d : p.pads) { mix(d.pos.x); mix(d.pos.y); mix(d.w); mix(d.h); mix(d.rot); mix((int)d.shape); mix(d.drill); mix(d.pin); }
    std::function<void(const GPrim&)> g = [&](const GPrim& q) {
        mix((int)q.kind); mix(q.radius); mix(q.a0); mix(q.a1); mix(q.rot); mix(q.filled);
        for (auto& v : q.pts) { mix(v.x); mix(v.y); }
        for (auto& c : q.children) g(c);
    };
    for (auto& q : p.body) g(q);
    mix(p.smt);
    return h;
}

// ---- body graphics hit testing ---------------------------------------------
double seg_dist(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 d = b - a;
    double l2 = d.x * d.x + d.y * d.y;
    double t = l2 > 1e-18 ? std::clamp(((p.x - a.x) * d.x + (p.y - a.y) * d.y) / l2, 0.0, 1.0) : 0.0;
    return dist(p, a + d * t);
}
std::vector<Vec2> rect_corners(const GPrim& g) {
    if (g.pts.size() < 2) return {};
    Vec2 c = (g.pts[0] + g.pts[1]) * 0.5;
    std::vector<Vec2> q = {{g.pts[0].x, g.pts[0].y}, {g.pts[1].x, g.pts[0].y}, {g.pts[1].x, g.pts[1].y}, {g.pts[0].x, g.pts[1].y}};
    for (auto& v : q) v = rotate_about(v, c, g.rot);
    return q;
}
// distance from p to the primitive's stroke (filled shapes: 0 inside)
double prim_dist(const GPrim& g, Vec2 p) {
    double best = 1e300;
    auto poly = [&](const std::vector<Vec2>& v, bool closed) {
        for (size_t i = 1; i < v.size(); ++i) best = std::min(best, seg_dist(p, v[i - 1], v[i]));
        if (closed && v.size() > 2) best = std::min(best, seg_dist(p, v.back(), v[0]));
        if (closed && g.filled && v.size() > 2) {
            bool in = false;
            for (size_t i = 0, j = v.size() - 1; i < v.size(); j = i++)
                if (((v[i].y > p.y) != (v[j].y > p.y)) && p.x < (v[j].x - v[i].x) * (p.y - v[i].y) / (v[j].y - v[i].y) + v[i].x) in = !in;
            if (in) best = 0;
        }
    };
    switch (g.kind) {
    case GKind::Line: case GKind::Polyline: poly(g.pts, false); break;
    case GKind::Polygon: poly(g.pts, true); break;
    case GKind::Rect: poly(rect_corners(g), true); break;
    case GKind::Circle: if (!g.pts.empty()) { double d = dist(p, g.pts[0]); best = g.filled && d <= g.radius ? 0 : std::fabs(d - g.radius); } break;
    case GKind::Arc: if (!g.pts.empty()) {
        std::vector<Vec2> v;
        int n = std::max(8, (int)(std::fabs(g.a1) / 4));
        for (int i = 0; i <= n; ++i) { double a = deg2rad(g.a0 + g.a1 * i / n); v.push_back({g.pts[0].x + g.radius * std::cos(a), g.pts[0].y + g.radius * std::sin(a)}); }
        poly(v, false);
    } break;
    case GKind::Text: if (!g.pts.empty()) {
        double h = g.text_h > 0 ? g.text_h : 1.0;
        BBox b(g.pts[0].x, g.pts[0].y, g.pts[0].x + h * 0.6 * std::max<size_t>(1, g.text.size()), g.pts[0].y + h);
        best = b.contains(p) ? 0 : dist(p, b.center()) - h * 0.5;
    } break;
    case GKind::Group: for (auto& c : g.children) best = std::min(best, prim_dist(c, p)); break;
    }
    return best;
}
const char* prim_kind_name(GKind k) {
    switch (k) {
    case GKind::Line: return "Line"; case GKind::Polyline: return "Polyline"; case GKind::Polygon: return "Polygon";
    case GKind::Rect: return "Rectangle"; case GKind::Circle: return "Circle"; case GKind::Arc: return "Arc";
    case GKind::Text: return "Text"; case GKind::Group: return "Group";
    }
    return "?";
}

// ===========================================================================
// Master package library (shared by every open project)
// ===========================================================================
std::wstring appdata_dir() {
    wchar_t app[MAX_PATH] = L"";
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, app))) {
        std::wstring d = std::wstring(app) + L"\\PCBPlotter";
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }
    return L".";
}
std::wstring lib_ini() { return appdata_dir() + L"\\library.ini"; }
std::wstring master_default_path() { return appdata_dir() + L"\\master_library.pcbplib"; }
std::wstring master_path_setting() {
    wchar_t b[2048] = L"";
    GetPrivateProfileStringW(L"Library", L"MasterPath", L"", b, 2048, lib_ini().c_str());
    return b[0] ? std::wstring(b) : master_default_path();
}
void set_master_path_setting(const std::wstring& p) { WritePrivateProfileStringW(L"Library", L"MasterPath", p.c_str(), lib_ini().c_str()); }

struct LibState {
    PackageLibrary master;
    PackageLibrary source;          // optional read-only CircuitCAM source
    bool loaded = false;
    std::string err;
};
LibState& LIB() {
    static LibState s;
    if (!s.loaded) {
        s.loaded = true;
        if (!s.master.load(ui::U(master_path_setting()), s.err)) s.master.packages.clear();
        if (s.master.name.empty()) s.master.name = "Master library";
    }
    return s;
}
bool save_master(std::string* err_out = nullptr) {
    std::string err;
    LIB().master.touch();
    bool ok = false;
    pcbp::gui::run_with_progress_wait("Saving master library", [&](pcbp::gui::Progress& p) { p.set(-1.f, "Writing packages..."); ok = LIB().master.save(err); },
                                      pcbp::gui::ProgressOpts{true, false, 0.25f});
    if (ok) return true;
    if (err_out) *err_out = err;
    pcbp::gui::toast("Master library not saved: " + err, pcbp::gui::ToastKind::Error, 8.f);
    return false;
}
std::string file_base(const std::string& p) {
    size_t a = p.find_last_of("\\/");
    return a == std::string::npos ? p : p.substr(a + 1);
}

// ===========================================================================
// Direct2D package painter (canvas + thumbnails)
// ===========================================================================
void path_poly(ID2D1RenderTarget* rt, const std::vector<Vec2>& pts, ID2D1Brush* b, bool fill, float w, bool closed, ID2D1StrokeStyle* ss = nullptr) {
    if (pts.size() < 2) return;
    Com<ID2D1PathGeometry> g;
    if (FAILED(ui::d2d_factory()->CreatePathGeometry(g.put()))) return;
    Com<ID2D1GeometrySink> k;
    if (FAILED(g->Open(k.put()))) return;
    k->BeginFigure(D2D1::Point2F((float)pts[0].x, (float)pts[0].y), fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < pts.size(); ++i) k->AddLine(D2D1::Point2F((float)pts[i].x, (float)pts[i].y));
    k->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    k->Close();
    if (fill) rt->FillGeometry(g.get(), b);
    else rt->DrawGeometry(g.get(), b, w, ss);
}

void draw_prim(ID2D1RenderTarget* rt, const GPrim& g, ID2D1SolidColorBrush* b, float hair) {
    const float w = std::max((float)g.width, hair);
    switch (g.kind) {
    case GKind::Line:
        if (g.pts.size() >= 2) rt->DrawLine(D2D1::Point2F((float)g.pts[0].x, (float)g.pts[0].y), D2D1::Point2F((float)g.pts[1].x, (float)g.pts[1].y), b, w);
        break;
    case GKind::Polyline: path_poly(rt, g.pts, b, false, w, false); break;
    case GKind::Polygon: path_poly(rt, g.pts, b, g.filled, w, true); break;
    case GKind::Rect:
        if (g.pts.size() >= 2) {
            Vec2 c = (g.pts[0] + g.pts[1]) * 0.5;
            std::vector<Vec2> q = {{g.pts[0].x, g.pts[0].y}, {g.pts[1].x, g.pts[0].y}, {g.pts[1].x, g.pts[1].y}, {g.pts[0].x, g.pts[1].y}};
            for (auto& p : q) p = rotate_about(p, c, g.rot);
            path_poly(rt, q, b, g.filled, w, true);
        }
        break;
    case GKind::Circle:
        if (!g.pts.empty()) {
            auto e = D2D1::Ellipse(D2D1::Point2F((float)g.pts[0].x, (float)g.pts[0].y), (float)g.radius, (float)g.radius);
            if (g.filled) rt->FillEllipse(e, b); else rt->DrawEllipse(e, b, w);
        }
        break;
    case GKind::Arc:
        if (!g.pts.empty()) {
            std::vector<Vec2> pts;
            int n = std::max(6, (int)(std::fabs(g.a1) / 5));
            for (int i = 0; i <= n; ++i) {
                double a = deg2rad(g.a0 + g.a1 * i / n);
                pts.push_back({g.pts[0].x + g.radius * std::cos(a), g.pts[0].y + g.radius * std::sin(a)});
            }
            path_poly(rt, pts, b, false, w, false);
        }
        break;
    case GKind::Group: for (auto& c : g.children) draw_prim(rt, c, b, hair); break;
    case GKind::Text: break;
    }
}

void draw_pad(ID2D1RenderTarget* rt, const Pad& p, Vec2 off, ID2D1SolidColorBrush* fill, ID2D1SolidColorBrush* line, float lw,
              ID2D1SolidColorBrush* hole) {
    D2D1_MATRIX_3X2_F world;
    rt->GetTransform(&world);
    D2D1::Matrix3x2F local = D2D1::Matrix3x2F::Rotation((float)p.rot) *
                             D2D1::Matrix3x2F::Translation((float)(p.pos.x + off.x), (float)(p.pos.y + off.y));
    rt->SetTransform(local * *D2D1::Matrix3x2F::ReinterpretBaseType(&world));
    const float hw = (float)p.w * 0.5f, hh = (float)p.h * 0.5f;
    auto rect = D2D1::RectF(-hw, -hh, hw, hh);
    switch (p.shape) {
    case PadShape::Round: {
        auto e = D2D1::Ellipse(D2D1::Point2F(0, 0), hw, hh);
        if (fill) rt->FillEllipse(e, fill);
        if (line) rt->DrawEllipse(e, line, lw);
    } break;
    case PadShape::Oblong: {
        float r = std::min(hw, hh);
        auto rr = D2D1::RoundedRect(rect, r, r);
        if (fill) rt->FillRoundedRectangle(rr, fill);
        if (line) rt->DrawRoundedRectangle(rr, line, lw);
    } break;
    case PadShape::Octagon: {
        double c = std::min(p.w, p.h) * 0.29;
        double x = hw, y = hh;
        std::vector<Vec2> q = {{-x + c, -y}, {x - c, -y}, {x, -y + c}, {x, y - c}, {x - c, y}, {-x + c, y}, {-x, y - c}, {-x, -y + c}};
        if (fill) path_poly(rt, q, fill, true, lw, true);
        if (line) path_poly(rt, q, line, false, lw, true);
    } break;
    default:
        if (fill) rt->FillRectangle(rect, fill);
        if (line) rt->DrawRectangle(rect, line, lw);
        break;
    }
    if (hole && p.drill > 0) {
        float r = (float)p.drill * 0.5f;
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), r, r), hole);
    }
    rt->SetTransform(world);
}

struct PaintOpts {
    float hair = 0.02f;                  // 1 px in mm
    const std::set<int>* sel = nullptr;
    Vec2 drag{};                         // offset applied to selected pads
    bool courtyard = false, centroid = false, pin1 = true;
    bool body = true, pads = true;       // R10 §9: independently toggleable in the editor
    ImU32 bg = 0;                        // hole colour (ImGui ABGR)
};

void paint_package(ID2D1RenderTarget* rt, const Package& pk, const PaintOpts& o) {
    const auto& P = ui::pal();
    Com<ID2D1SolidColorBrush> br, br2, hole;
    rt->CreateSolidColorBrush(ui::d2c(P.comp_body), br.put());
    rt->CreateSolidColorBrush(ui::d2c(P.selection), br2.put());
    rt->CreateSolidColorBrush(d2_im(o.bg | 0xFF000000), hole.put());
    if (!br || !br2 || !hole) return;
    // courtyard (dashed)
    if (o.courtyard) {
        BBox e = pk.extent();
        e.inflate(0.25);
        static Com<ID2D1StrokeStyle> dash;
        if (!dash) {
            float d[] = {4.f, 3.f};
            ui::d2d_factory()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                             D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
                                                 d, 2, dash.put());
        }
        br->SetColor(ui::d2c(P.grid_major));
        br->SetOpacity(0.9f);
        std::vector<Vec2> q = {{e.x0, e.y0}, {e.x1, e.y0}, {e.x1, e.y1}, {e.x0, e.y1}};
        path_poly(rt, q, br.get(), false, o.hair * 1.2f, true, dash.get());
        br->SetOpacity(1.f);
    }
    // body
    br->SetColor(ui::d2c(P.comp_body));
    if (!o.body) {}
    else if (pk.body.empty()) {
        Package t = pk;
        t.auto_body();
        br->SetOpacity(0.45f);
        for (auto& g : t.body) draw_prim(rt, g, br.get(), o.hair * 1.5f);
        br->SetOpacity(1.f);
    } else {
        for (auto& g : pk.body) draw_prim(rt, g, br.get(), o.hair * 1.5f);
    }
    // pads
    const bool thp = is_th(pk);
    Com<ID2D1SolidColorBrush> pf;
    rt->CreateSolidColorBrush(ui::d2c(thp ? P.pad_th : P.pad_smt), pf.put());
    const int p1 = o.pin1 && o.pads ? pin1_index(pk) : -1;
    for (size_t i = 0; o.pads && i < pk.pads.size(); ++i) {
        const Pad& p = pk.pads[i];
        const bool s = o.sel && o.sel->count((int)i);
        pf->SetColor(ui::d2c(p.drill > 0 ? P.pad_th : P.pad_smt));
        pf->SetOpacity(s ? 1.f : 0.92f);
        if (s && (o.drag.x != 0 || o.drag.y != 0)) {
            pf->SetOpacity(0.25f);
            draw_pad(rt, p, {}, pf.get(), nullptr, o.hair, nullptr);
            pf->SetOpacity(0.9f);
            draw_pad(rt, p, o.drag, pf.get(), br2.get(), o.hair * 2.f, hole.get());
        } else {
            draw_pad(rt, p, {}, pf.get(), s ? br2.get() : nullptr, o.hair * 2.5f, hole.get());
        }
    }
    // P2-02: pin 1 and origin differ from the (yellow) pads by hue AND shape, legible in both themes:
    // pin 1 = magenta outline + solid corner triangle; origin = cyan crosshair in a diamond.
    const bool dark = th().dark;
    const ImU32 pin1_col = dark ? IM_COL32(244, 114, 182, 255) : IM_COL32(190, 24, 93, 255);
    const ImU32 org_col = dark ? IM_COL32(34, 211, 238, 255) : IM_COL32(3, 105, 161, 255);
    if (p1 >= 0 && p1 < (int)pk.pads.size()) {
        Com<ID2D1SolidColorBrush> w;
        rt->CreateSolidColorBrush(d2_im(pin1_col), w.put());
        if (w) {
            const Pad& p = pk.pads[p1];
            const Vec2 dd = (o.sel && o.sel->count(p1)) ? o.drag : Vec2{};
            Pad q = p; q.w += o.hair * 6; q.h += o.hair * 6;
            draw_pad(rt, q, dd, nullptr, w.get(), o.hair * 2.2f, nullptr);
            // solid triangle on the pad's outer corner (top-left of its bbox)
            const BBox b = pad_bbox(q);
            const float k = (float)std::min(std::max(std::min(b.w(), b.h()) * 0.28, (double)o.hair * 6), (double)o.hair * 14);
            const float x0 = (float)(b.x0 + dd.x), y1 = (float)(b.y1 + dd.y);
            Com<ID2D1PathGeometry> g;
            if (SUCCEEDED(ui::d2d_factory()->CreatePathGeometry(g.put()))) {
                Com<ID2D1GeometrySink> sk;
                if (SUCCEEDED(g->Open(sk.put()))) {
                    sk->BeginFigure(D2D1::Point2F(x0 - o.hair * 2, y1 + o.hair * 2), D2D1_FIGURE_BEGIN_FILLED);
                    sk->AddLine(D2D1::Point2F(x0 + k, y1 + o.hair * 2));
                    sk->AddLine(D2D1::Point2F(x0 - o.hair * 2, y1 - k));
                    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sk->Close();
                    rt->FillGeometry(g.get(), w.get());
                }
            }
        }
    }
    if (o.centroid) {
        Com<ID2D1SolidColorBrush> c;
        rt->CreateSolidColorBrush(d2_im(org_col), c.put());
        if (c) {
            float r = o.hair * 9;
            rt->DrawLine(D2D1::Point2F(-r * 2.f, 0), D2D1::Point2F(r * 2.f, 0), c.get(), o.hair * 1.6f);
            rt->DrawLine(D2D1::Point2F(0, -r * 2.f), D2D1::Point2F(0, r * 2.f), c.get(), o.hair * 1.6f);
            D2D1_POINT_2F dm[4] = {{r, 0}, {0, r}, {-r, 0}, {0, -r}};
            for (int i = 0; i < 4; ++i) rt->DrawLine(dm[i], dm[(i + 1) % 4], c.get(), o.hair * 1.6f);
        }
    }
}

// ImDrawList package painter (template tiles, wizard preview)
// R18a: pin1_fill != 0 fills lead 1 (or draws the user-placed polarity marker when there is no lead 1) in that colour;
// pin1_at receives its screen position (unchanged when the package has neither).
void draw_pkg_dl(ImDrawList* dl, const Package& pk, ImVec2 a, ImVec2 b, bool pins, ImU32 pin1_fill = 0, ImVec2* pin1_at = nullptr) {
    BBox e = pk.extent();
    if (e.empty()) return;
    const float pad = 6;
    float sx = (b.x - a.x - 2 * pad) / (float)std::max(e.w(), 0.1), sy = (b.y - a.y - 2 * pad) / (float)std::max(e.h(), 0.1);
    float s = std::min(sx, sy);
    ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    Vec2 ec = e.center();
    auto X = [&](Vec2 w) { return ImVec2(c.x + (float)(w.x - ec.x) * s, c.y - (float)(w.y - ec.y) * s); };
    const ImU32 body = im_argb(ui::pal().comp_body);
    std::function<void(const GPrim&)> prim = [&](const GPrim& g) {
        switch (g.kind) {
        case GKind::Line: if (g.pts.size() >= 2) dl->AddLine(X(g.pts[0]), X(g.pts[1]), body, 1.2f); break;
        case GKind::Polyline: case GKind::Polygon:
            for (size_t i = 1; i < g.pts.size(); ++i) dl->AddLine(X(g.pts[i - 1]), X(g.pts[i]), body, 1.2f);
            if (g.kind == GKind::Polygon && g.pts.size() > 2) dl->AddLine(X(g.pts.back()), X(g.pts[0]), body, 1.2f);
            break;
        case GKind::Rect: if (g.pts.size() >= 2) {
            Vec2 cc = (g.pts[0] + g.pts[1]) * 0.5;
            Vec2 q[4] = {{g.pts[0].x, g.pts[0].y}, {g.pts[1].x, g.pts[0].y}, {g.pts[1].x, g.pts[1].y}, {g.pts[0].x, g.pts[1].y}};
            ImVec2 s4[4];
            for (int i = 0; i < 4; ++i) s4[i] = X(rotate_about(q[i], cc, g.rot));
            dl->AddPolyline(s4, 4, body, ImDrawFlags_Closed, 1.2f);
        } break;
        case GKind::Circle: if (!g.pts.empty()) { if (g.filled) dl->AddCircleFilled(X(g.pts[0]), (float)g.radius * s, body); else dl->AddCircle(X(g.pts[0]), (float)g.radius * s, body, 0, 1.2f); } break;
        case GKind::Arc: if (!g.pts.empty()) {
            int n = std::max(6, (int)(std::fabs(g.a1) / 8));
            for (int i = 0; i < n; ++i) {
                double a0 = deg2rad(g.a0 + g.a1 * i / n), a1 = deg2rad(g.a0 + g.a1 * (i + 1) / n);
                dl->AddLine(X({g.pts[0].x + g.radius * std::cos(a0), g.pts[0].y + g.radius * std::sin(a0)}),
                            X({g.pts[0].x + g.radius * std::cos(a1), g.pts[0].y + g.radius * std::sin(a1)}), body, 1.2f);
            }
        } break;
        case GKind::Group: for (auto& q : g.children) prim(q); break;
        default: break;
        }
    };
    for (auto& g : pk.body) prim(g);
    const int p1 = pin1_index(pk);
    for (size_t i = 0; i < pk.pads.size(); ++i) {
        const Pad& p = pk.pads[i];
        ImU32 col = im_argb(p.drill > 0 ? ui::pal().pad_th : ui::pal().pad_smt);
        if ((int)i == p1 && pin1_fill) { col = pin1_fill; if (pin1_at) *pin1_at = X(p.pos); }
        if (p.shape == PadShape::Round) dl->AddEllipseFilled(X(p.pos), ImVec2((float)p.w * 0.5f * s, (float)p.h * 0.5f * s), col, (float)-deg2rad(p.rot));
        else {
            const double hw = p.w * 0.5, hh = p.h * 0.5;
            Vec2 q[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
            ImVec2 s4[4];
            for (int k = 0; k < 4; ++k) s4[k] = X(rotate(q[k], p.rot) + p.pos);
            dl->AddQuadFilled(s4[0], s4[1], s4[2], s4[3], col);
        }
        if (p.drill > 0) dl->AddCircleFilled(X(p.pos), (float)p.drill * 0.5f * s, th().panel2);
        if ((int)i == p1 && pins) {
            BBox bb = pad_bbox(p);
            dl->AddRect(X({bb.x0, bb.y1}) - ImVec2(2, 2), X({bb.x1, bb.y0}) + ImVec2(2, 2), th().warn, 2, 0, 1.5f);
        }
    }
    if (pin1_fill && p1 < 0 && pk.marker_kind != 0 && pk.marker_user_pos) {   // polarity marker stands in for lead 1
        const float r = std::max(3.f, (float)(pk.marker_size > 0 ? pk.marker_size * 0.5 : 0.25) * s);
        dl->AddCircleFilled(X(pk.marker_pos), r, pin1_fill);
        if (pin1_at) *pin1_at = X(pk.marker_pos);
    }
}

// Results of user actions -> toast (Info for hints, Success for commits, Warning for refusals)
void say(const std::string& s, pcbp::gui::ToastKind k = pcbp::gui::ToastKind::Success) { pcbp::gui::toast(s, k); }
void hint(const std::string& s) { pcbp::gui::status(s, pcbp::gui::ToastKind::Info); }

// ===========================================================================
// Custom glyphs (drawn with ImDrawList, 16-unit design box, theme tinted)
// ===========================================================================
enum class G {
    AddPad, Array, Measure, Mirror, Renumber, PinOrder, AutoBody, Centre, ListView, GridView, Template, Merge, Users,
    Rename, Duplicate, Import, Export, NewPkg, Sort, Filter, RotPad,
    Arc, Poly, ToLib, FromLib, Library, Project, OriginPick, Width
};

void glyph(ImDrawList* dl, G g, ImVec2 p, float sz, float alpha = 1.f) {
    const float k = sz / 16.f;
    const ImU32 fg = with_alpha(th().text, (int)(230 * alpha)), ac = with_alpha(th().accent, (int)(255 * alpha));
    const ImU32 acf = with_alpha(th().accent, (int)(90 * alpha));
    const float t = std::max(1.f, k * 1.3f);
    auto P = [&](float x, float y) { return ImVec2(p.x + x * k, p.y + y * k); };
    auto L = [&](float x0, float y0, float x1, float y1, ImU32 c) { dl->AddLine(P(x0, y0), P(x1, y1), c, t); };
    auto R = [&](float x0, float y0, float x1, float y1, ImU32 c, bool f, float r = 1.2f) {
        if (f) dl->AddRectFilled(P(x0, y0), P(x1, y1), c, r * k); else dl->AddRect(P(x0, y0), P(x1, y1), c, r * k, 0, t);
    };
    auto Tx = [&](const char* s, float x, float y, float h, ImU32 c) { dl->AddText(g_font_bold, h * k, P(x, y), c, s); };
    auto Arrow = [&](float x0, float y0, float x1, float y1, ImU32 c) {
        L(x0, y0, x1, y1, c);
        float dx = x1 - x0, dy = y1 - y0, l = std::sqrt(dx * dx + dy * dy);
        if (l < 1e-3f) return;
        dx /= l; dy /= l;
        dl->AddTriangleFilled(P(x1 + dx * 1.2f, y1 + dy * 1.2f), P(x1 - dx * 2.6f - dy * 2.f, y1 - dy * 2.6f + dx * 2.f),
                              P(x1 - dx * 2.6f + dy * 2.f, y1 - dy * 2.6f - dx * 2.f), c);
    };
    switch (g) {
    case G::AddPad: R(2, 5, 10, 13, acf, true); R(2, 5, 10, 13, ac, false); L(12.5f, 1.5f, 12.5f, 7.5f, fg); L(9.5f, 4.5f, 15.5f, 4.5f, fg); break;
    case G::Array: for (int i = 0; i < 3; ++i) { R(1.5f + i * 5, 3, 4.5f + i * 5, 9, acf, true); R(1.5f + i * 5, 3, 4.5f + i * 5, 9, ac, false); } Arrow(2, 13, 13.5f, 13, fg); break;
    case G::Measure:
        R(1.5f, 5.5f, 14.5f, 10.5f, fg, false, 0.8f);
        for (int i = 0; i < 5; ++i) L(3.5f + i * 2.4f, 5.5f, 3.5f + i * 2.4f, i % 2 ? 7.5f : 8.5f, fg);
        Arrow(5, 13.5f, 1.5f, 13.5f, ac); Arrow(11, 13.5f, 14.5f, 13.5f, ac); L(5, 13.5f, 11, 13.5f, ac);
        break;
    case G::Mirror:
        for (int i = 0; i < 4; ++i) L(8, 1.5f + i * 3.6f, 8, 3.3f + i * 3.6f, fg);
        dl->AddTriangleFilled(P(6, 3), P(6, 13), P(1.5f, 13), ac);
        dl->AddTriangle(P(10, 3), P(10, 13), P(14.5f, 13), fg, t);
        break;
    case G::Renumber: Tx("12", 1, 1.5f, 9, fg); Tx("3", 9.5f, 6.5f, 9, ac); break;
    case G::PinOrder:
        R(1.5f, 1.5f, 9, 9, acf, true); R(1.5f, 1.5f, 9, 9, ac, false); Tx("1", 3.6f, 1.6f, 8, fg);
        dl->AddTriangleFilled(P(9, 8), P(15, 12.5f), P(11.5f, 13), fg); L(11.5f, 12.5f, 13.5f, 15, fg);
        break;
    case G::AutoBody:
        R(4, 2.5f, 12, 13.5f, ac, false, 0.8f);
        for (int i = 0; i < 3; ++i) { R(1, 3.5f + i * 3.5f, 4, 5.5f + i * 3.5f, fg, true, 0.3f); R(12, 3.5f + i * 3.5f, 15, 5.5f + i * 3.5f, fg, true, 0.3f); }
        dl->AddCircleFilled(P(6.3f, 5), 1.0f * k, ac);
        break;
    case G::Centre:
        R(2, 2, 14, 14, fg, false, 1.5f);
        L(8, 4, 8, 12, ac); L(4, 8, 12, 8, ac); dl->AddCircle(P(8, 8), 2.4f * k, ac, 0, t);
        break;
    case G::ListView: for (int i = 0; i < 3; ++i) { R(1.5f, 2.5f + i * 4.5f, 4.5f, 5.5f + i * 4.5f, ac, true, 0.6f); L(6.5f, 4 + i * 4.5f, 14.5f, 4 + i * 4.5f, fg); } break;
    case G::GridView: R(2, 2, 7.2f, 7.2f, fg, false); R(8.8f, 2, 14, 7.2f, fg, false); R(2, 8.8f, 7.2f, 14, fg, false); R(8.8f, 8.8f, 14, 14, ac, true); break;
    case G::Template:
        L(2, 14, 10.5f, 5.5f, fg); L(2.6f, 14.6f, 11.1f, 6.1f, fg);
        dl->AddCircleFilled(P(12, 4), 1.3f * k, ac);
        L(12, 0.8f, 12, 2.2f, ac); L(12, 5.8f, 12, 7.2f, ac); L(8.8f, 4, 10.2f, 4, ac); L(13.8f, 4, 15.2f, 4, ac);
        dl->AddCircleFilled(P(5, 3.5f), 0.9f * k, ac); dl->AddCircleFilled(P(14, 10), 0.9f * k, ac);
        break;
    case G::Merge: L(2, 3, 8, 8, fg); L(2, 13, 8, 8, fg); Arrow(8, 8, 14, 8, ac); break;
    case G::Users:
        dl->AddCircle(P(8, 8), 5.5f * k, fg, 0, t); dl->AddCircleFilled(P(8, 8), 2.0f * k, ac);
        L(8, 0.8f, 8, 3, fg); L(8, 13, 8, 15.2f, fg); L(0.8f, 8, 3, 8, fg); L(13, 8, 15.2f, 8, fg);
        break;
    case G::Rename: L(3, 13, 12, 4, fg); L(4.2f, 14.2f, 13.2f, 5.2f, fg); L(1.5f, 14.8f, 3.4f, 13.4f, ac); dl->AddTriangleFilled(P(12, 2.8f), P(14.5f, 5.3f), P(13.2f, 6.4f), ac); break;
    case G::Duplicate: R(1.5f, 1.5f, 10, 10, fg, false); R(6, 6, 14.5f, 14.5f, acf, true); R(6, 6, 14.5f, 14.5f, ac, false); break;
    case G::Import: L(2, 10, 2, 14, fg); L(2, 14, 14, 14, fg); L(14, 14, 14, 10, fg); Arrow(8, 1.5f, 8, 10, ac); break;
    case G::Export: L(2, 10, 2, 14, fg); L(2, 14, 14, 14, fg); L(14, 14, 14, 10, fg); Arrow(8, 10.5f, 8, 2.5f, ac); break;
    case G::NewPkg:
        R(3, 4, 11, 13, fg, false, 1);
        for (int i = 0; i < 3; ++i) { L(1, 6 + i * 2.6f, 3, 6 + i * 2.6f, fg); L(11, 6 + i * 2.6f, 12.5f, 6 + i * 2.6f, fg); }
        dl->AddCircleFilled(P(12.5f, 3.5f), 3.2f * k, ac);
        dl->AddLine(P(12.5f, 1.8f), P(12.5f, 5.2f), th().accent_text, t); dl->AddLine(P(10.8f, 3.5f), P(14.2f, 3.5f), th().accent_text, t);
        break;
    case G::Sort: L(2, 3.5f, 9, 3.5f, fg); L(2, 8, 7, 8, fg); L(2, 12.5f, 5, 12.5f, fg); Arrow(12.5f, 2, 12.5f, 13, ac); break;
    case G::Filter: dl->AddTriangleFilled(P(1.5f, 2.5f), P(14.5f, 2.5f), P(8, 9.5f), acf); dl->AddTriangle(P(1.5f, 2.5f), P(14.5f, 2.5f), P(8, 9.5f), ac, t); L(8, 9.5f, 8, 14, ac); break;
    case G::RotPad: R(4, 1.5f, 9, 10, acf, true); R(4, 1.5f, 9, 10, ac, false); Arrow(3, 13.5f, 13.5f, 13.5f, fg); L(13.5f, 13.5f, 13.5f, 7, fg); break;
    case G::Arc:
        dl->PathArcTo(P(8, 12), 6.5f * k, 3.14159f, 2 * 3.14159f, 16); dl->PathStroke(fg, 0, t);
        dl->AddCircleFilled(P(1.5f, 12), 1.4f * k, ac); dl->AddCircleFilled(P(14.5f, 12), 1.4f * k, ac); dl->AddCircle(P(8, 12), 1.2f * k, ac, 0, t);
        break;
    case G::Poly:
        L(2, 13, 5, 4, fg); L(5, 4, 11, 10, fg); L(11, 10, 14, 3, fg);
        for (auto q : {ImVec2(2, 13), ImVec2(5, 4), ImVec2(11, 10), ImVec2(14, 3)}) dl->AddRectFilled(P(q.x - 1.3f, q.y - 1.3f), P(q.x + 1.3f, q.y + 1.3f), ac);
        break;
    case G::Library:
        for (int i = 0; i < 3; ++i) { dl->AddEllipse(P(8, 3.5f + i * 4.5f), ImVec2(6 * k, 2 * k), i == 0 ? ac : fg, 0, 0, t); }
        L(2, 3.5f, 2, 12.5f, fg); L(14, 3.5f, 14, 12.5f, fg);
        break;
    case G::Project: R(2, 2, 14, 14, fg, false, 1.5f); R(4.5f, 5, 7.5f, 7, ac, true, 0.3f); R(8.5f, 9, 11.5f, 11, ac, true, 0.3f); break;
    case G::ToLib: R(1, 3, 6, 13, acf, true); R(1, 3, 6, 13, ac, false); Arrow(7, 8, 14.5f, 8, fg); break;
    case G::FromLib: R(10, 3, 15, 13, acf, true); R(10, 3, 15, 13, ac, false); Arrow(9, 8, 1.5f, 8, fg); break;
    case G::OriginPick:
        L(8, 1, 8, 15, fg); L(1, 8, 15, 8, fg); dl->AddCircle(P(8, 8), 3.5f * k, ac, 0, t); dl->AddCircleFilled(P(8, 8), 1.3f * k, ac);
        break;
    case G::Width: L(2, 3, 14, 3, fg); dl->AddLine(P(2, 7.5f), P(14, 7.5f), fg, t * 1.8f); dl->AddLine(P(2, 12.5f), P(14, 12.5f), ac, t * 3.f); break;
    }
}

bool gbtn(const char* id, G g, const char* tip, bool checked = false, bool enabled = true, float size = 0) {
    if (size <= 0) size = S(30);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton("##gb", ImVec2(size, size));
    bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const GTheme& t = th();
    if (checked) {
        dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), with_alpha(t.accent, 0x38), S(6));
        dl->AddRect(p, ImVec2(p.x + size, p.y + size), with_alpha(t.accent, 0xB0), S(6), 0, 1.2f);
    } else if (hov && enabled) {
        dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), act ? t.border : t.raised, S(6));
    }
    float is = std::round(size * 0.62f);
    glyph(dl, g, ImVec2(p.x + (size - is) * 0.5f, p.y + (size - is) * 0.5f), is, enabled ? 1.f : 0.35f);
    tooltip(tip);
    ImGui::PopID();
    return clicked && enabled;
}

void tb_sep() {
    ImGui::SameLine(0, S(6));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + S(6)), ImVec2(p.x, p.y + S(24)), th().border, 1.f);
    ImGui::Dummy(ImVec2(S(1), S(30)));
    ImGui::SameLine(0, S(6));
}

// pill-shaped toggle chip; returns true when clicked
bool chip(const char* label, bool on, const char* tip = nullptr, ImU32 dot = 0) {
    const GTheme& t = th();
    ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    float dw = dot ? S(12) : 0;
    ImVec2 sz(ts.x + S(18) + dw, ImGui::GetFrameHeight() - S(2));
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool c = ImGui::InvisibleButton(label, sz);
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = on ? with_alpha(t.accent, 0x40) : hov ? t.raised : t.panel2;
    dl->AddRectFilled(p, p + sz, bg, sz.y * 0.5f);
    dl->AddRect(p, p + sz, on ? with_alpha(t.accent, 0xC0) : t.border, sz.y * 0.5f, 0, 1.f);
    if (dot) dl->AddCircleFilled(ImVec2(p.x + S(11), p.y + sz.y * 0.5f), S(3.5f), dot);
    const char* end = ImGui::FindRenderedTextEnd(label);
    dl->AddText(ImVec2(p.x + S(9) + dw, p.y + (sz.y - ts.y) * 0.5f), on ? t.text : t.text_dim, label, end);
    if (tip) tooltip(tip);
    return c;
}

void badge(ImDrawList* dl, ImVec2 right_top, const char* txt, ImU32 bg, ImU32 fg) {
    ImVec2 ts = ImGui::CalcTextSize(txt);
    float h = ts.y + S(2), w = std::max(h, ts.x + S(10));
    ImVec2 a(right_top.x - w, right_top.y), b(right_top.x, right_top.y + h);
    dl->AddRectFilled(a, b, bg, h * 0.5f);
    dl->AddText(ImVec2(a.x + (w - ts.x) * 0.5f, a.y + S(1)), fg, txt);
}

void text_clip(ImDrawList* dl, ImVec2 p, float maxw, const std::string& s, ImU32 col, ImFont* f = nullptr) {
    if (f) ImGui::PushFont(f);
    ImVec2 ts = ImGui::CalcTextSize(s.c_str());
    if (ts.x <= maxw) dl->AddText(p, col, s.c_str());
    else {
        std::string t = s;
        while (!t.empty() && ImGui::CalcTextSize((t + "...").c_str()).x > maxw) t.pop_back();
        t += "...";
        dl->AddText(p, col, t.c_str());
    }
    if (f) ImGui::PopFont();
}

// labelled property row: label column then widget fills
void prop_label(const char* l) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, th().text_dim);
    ImGui::TextUnformatted(l);
    ImGui::PopStyleColor();
    ImGui::SameLine(S(92));
    ImGui::SetNextItemWidth(-1);
}

// ===========================================================================
// Template generators
// ===========================================================================
enum class Tpl { Chip, Melf, Sod, Sot23, Sot223, Sot89, Soic, Qfp, Qfn, Bga, Plcc, Dip, Header, Radial, Axial, Xtal4, Count };
// R16b: template ids used by core/package_resolve suggestions (same order as Tpl)
const char* const kTplIds[] = {"chip", "melf", "sod", "sot23", "sot223", "sot89", "soic", "qfp", "qfn", "bga", "plcc", "dip", "header", "radial", "axial", "xtal4"};
int tpl_from_id(const std::string& id) { for (int i = 0; i < (int)Tpl::Count; ++i) if (id == kTplIds[i]) return i; return -1; }

struct TParam { const char* key; const char* label; double def; int dec; };
struct TPreset { const char* label; std::vector<std::pair<const char*, double>> v; };
struct TSpec {
    Tpl k;
    const char* title;
    const char* blurb;
    std::vector<TParam> params;
    std::vector<TPreset> presets;
};

const std::vector<TSpec>& tspecs() {
    static std::vector<TSpec> v = {
        {Tpl::Chip, "Chip (R/C/L)", "Two-terminal rectangular chip, imperial sizes with metric names",
         {{"L", "Pad length (X)", 0.8, 3}, {"W", "Pad width (Y)", 0.95, 3}, {"C", "Pad centre distance", 1.6, 3}, {"bl", "Body length", 1.6, 2}, {"bw", "Body width", 0.8, 2}},
         {{"0603 / 1608M", {{"L", .8}, {"W", .95}, {"C", 1.6}, {"bl", 1.6}, {"bw", .8}}},
          {"01005 / 0402M", {{"L", .2}, {"W", .22}, {"C", .4}, {"bl", .4}, {"bw", .2}}},
          {"0201 / 0603M", {{"L", .3}, {"W", .3}, {"C", .55}, {"bl", .6}, {"bw", .3}}},
          {"0402 / 1005M", {{"L", .55}, {"W", .6}, {"C", 1.0}, {"bl", 1.0}, {"bw", .5}}},
          {"0805 / 2012M", {{"L", 1.0}, {"W", 1.45}, {"C", 1.9}, {"bl", 2.0}, {"bw", 1.25}}},
          {"1206 / 3216M", {{"L", 1.15}, {"W", 1.8}, {"C", 3.0}, {"bl", 3.2}, {"bw", 1.6}}},
          {"1210 / 3225M", {{"L", 1.15}, {"W", 2.7}, {"C", 3.0}, {"bl", 3.2}, {"bw", 2.5}}},
          {"1812 / 4532M", {{"L", 1.3}, {"W", 3.4}, {"C", 4.2}, {"bl", 4.5}, {"bw", 3.2}}},
          {"2010 / 5025M", {{"L", 1.2}, {"W", 2.7}, {"C", 4.6}, {"bl", 5.0}, {"bw", 2.5}}},
          {"2512 / 6332M", {{"L", 1.4}, {"W", 3.4}, {"C", 5.8}, {"bl", 6.3}, {"bw", 3.2}}}}},
        {Tpl::Melf, "MELF", "Cylindrical MELF diode / resistor (polarized)",
         {{"L", "Pad length (X)", 1.2, 3}, {"W", "Pad width (Y)", 1.8, 3}, {"C", "Pad centre distance", 3.3, 3}, {"bl", "Body length", 3.6, 2}, {"bw", "Body diameter", 1.6, 2}},
         {{"MiniMELF (SOD-80)", {{"L", 1.2}, {"W", 1.8}, {"C", 3.3}, {"bl", 3.6}, {"bw", 1.6}}},
          {"MicroMELF", {{"L", .8}, {"W", 1.4}, {"C", 1.9}, {"bl", 2.0}, {"bw", 1.25}}},
          {"MELF (MMB0207)", {{"L", 1.5}, {"W", 2.6}, {"C", 5.4}, {"bl", 5.9}, {"bw", 2.2}}}}},
        {Tpl::Sod, "SOD", "Small-outline diode, cathode = pin 1",
         {{"L", "Pad length (X)", .9, 3}, {"W", "Pad width (Y)", 1.2, 3}, {"C", "Pad centre distance", 3.6, 3}, {"bl", "Body length", 2.7, 2}, {"bw", "Body width", 1.6, 2}},
         {{"SOD-123", {{"L", .9}, {"W", 1.2}, {"C", 3.6}, {"bl", 2.7}, {"bw", 1.6}}},
          {"SOD-323", {{"L", .7}, {"W", .65}, {"C", 2.5}, {"bl", 1.7}, {"bw", 1.25}}},
          {"SOD-523", {{"L", .5}, {"W", .6}, {"C", 1.4}, {"bl", 1.2}, {"bw", .8}}}}},
        {Tpl::Sot23, "SOT-23", "SOT-23-3 / -5 / -6 small outline transistor",
         {{"n", "Pins (3, 5, 6)", 3, 0}, {"p", "Pitch", .95, 3}, {"s", "Row span (centres)", 2.2, 3}, {"L", "Pad length", 1.0, 3}, {"W", "Pad width", .6, 3}, {"bl", "Body length", 2.9, 2}, {"bw", "Body width", 1.3, 2}},
         {{"SOT-23-3", {{"n", 3}}}, {"SOT-23-5", {{"n", 5}}}, {"SOT-23-6", {{"n", 6}}}, {"SC-70 (SOT-323)", {{"n", 3}, {"p", .65}, {"s", 1.9}, {"L", .8}, {"W", .4}, {"bl", 2.0}, {"bw", 1.25}}}}},
        {Tpl::Sot223, "SOT-223", "3 leads + tab (pin 4)",
         {{"p", "Pitch", 2.3, 3}, {"s", "Row span (centres)", 6.3, 3}, {"L", "Pad length", 2.0, 3}, {"W", "Pad width", 1.0, 3}, {"tw", "Tab width", 3.3, 3}, {"bl", "Body length", 6.5, 2}, {"bw", "Body width", 3.5, 2}},
         {}},
        {Tpl::Sot89, "SOT-89", "3 leads, centre tab connected to pin 2",
         {{"p", "Pitch", 1.5, 3}, {"L", "Pad length", 1.2, 3}, {"W", "Pad width", .6, 3}, {"y", "Lead row Y", -1.75, 3}, {"tw", "Tab width", 1.8, 3}, {"th", "Tab height", 2.4, 3}, {"bl", "Body length", 4.5, 2}, {"bw", "Body width", 2.5, 2}},
         {}},
        {Tpl::Soic, "SOIC / SSOP / TSSOP", "Dual-row gull-wing, pin 1 bottom-left, CCW",
         {{"n", "Pins (even)", 8, 0}, {"p", "Pitch", 1.27, 3}, {"s", "Row span (centres)", 5.4, 3}, {"L", "Pad length", 1.55, 3}, {"W", "Pad width", .6, 3}, {"bw", "Body width", 3.9, 2}},
         {{"SOIC", {{"p", 1.27}, {"s", 5.4}, {"L", 1.55}, {"W", .6}, {"bw", 3.9}}},
          {"SOIC wide", {{"n", 16}, {"p", 1.27}, {"s", 9.4}, {"L", 2.0}, {"W", .6}, {"bw", 7.5}}},
          {"SSOP", {{"n", 20}, {"p", .65}, {"s", 7.2}, {"L", 1.75}, {"W", .4}, {"bw", 5.3}}},
          {"TSSOP", {{"n", 14}, {"p", .65}, {"s", 5.8}, {"L", 1.5}, {"W", .4}, {"bw", 4.4}}},
          {"MSOP", {{"n", 8}, {"p", .65}, {"s", 4.4}, {"L", 1.45}, {"W", .4}, {"bw", 3.0}}}}},
        {Tpl::Qfp, "QFP", "Quad flat pack, pin 1 top of left side, CCW",
         {{"n", "Pins per side", 11, 0}, {"p", "Pitch", .8, 3}, {"s", "Row span (centres)", 11.4, 3}, {"L", "Pad length", 1.5, 3}, {"W", "Pad width", .5, 3}, {"b", "Body size", 10, 2}, {"ep", "Exposed pad (0 = none)", 0, 2}},
         {{"QFP-44 0.8", {{"n", 11}, {"p", .8}, {"s", 11.4}, {"L", 1.5}, {"W", .5}, {"b", 10}}},
          {"LQFP-48 0.5", {{"n", 12}, {"p", .5}, {"s", 8.4}, {"L", 1.5}, {"W", .3}, {"b", 7}}},
          {"LQFP-64 0.5", {{"n", 16}, {"p", .5}, {"s", 11.4}, {"L", 1.5}, {"W", .3}, {"b", 10}}},
          {"LQFP-100 0.5", {{"n", 25}, {"p", .5}, {"s", 15.4}, {"L", 1.5}, {"W", .3}, {"b", 14}}},
          {"TQFP-32 0.8", {{"n", 8}, {"p", .8}, {"s", 8.4}, {"L", 1.5}, {"W", .5}, {"b", 7}}}}},
        {Tpl::Qfn, "QFN / DFN", "No-lead quad with optional exposed pad",
         {{"n", "Pins per side", 4, 0}, {"p", "Pitch", .5, 3}, {"s", "Row span (centres)", 2.9, 3}, {"L", "Pad length", .8, 3}, {"W", "Pad width", .3, 3}, {"b", "Body size", 3, 2}, {"ep", "Exposed pad (0 = none)", 1.7, 2}},
         {{"QFN-16 3x3", {{"n", 4}, {"p", .5}, {"s", 2.9}, {"L", .8}, {"W", .3}, {"b", 3}, {"ep", 1.7}}},
          {"QFN-20 4x4", {{"n", 5}, {"p", .5}, {"s", 3.9}, {"L", .8}, {"W", .3}, {"b", 4}, {"ep", 2.6}}},
          {"QFN-32 5x5", {{"n", 8}, {"p", .5}, {"s", 4.9}, {"L", .8}, {"W", .3}, {"b", 5}, {"ep", 3.4}}},
          {"QFN-48 7x7", {{"n", 12}, {"p", .5}, {"s", 6.9}, {"L", .8}, {"W", .3}, {"b", 7}, {"ep", 5.3}}}}},
        {Tpl::Bga, "BGA", "Ball grid array, pins row-major from top-left",
         {{"r", "Rows", 8, 0}, {"c", "Columns", 8, 0}, {"p", "Pitch", .8, 3}, {"d", "Pad diameter", .4, 3}, {"dep", "Depopulated centre (rows)", 0, 0}, {"b", "Body size", 7, 2}},
         {{"8x8 0.8", {{"r", 8}, {"c", 8}, {"p", .8}, {"d", .4}, {"b", 7}}},
          {"10x10 1.0", {{"r", 10}, {"c", 10}, {"p", 1.0}, {"d", .5}, {"b", 11}}},
          {"16x16 1.0 (4 depop)", {{"r", 16}, {"c", 16}, {"p", 1.0}, {"d", .5}, {"dep", 4}, {"b", 17}}},
          {"12x12 0.5", {{"r", 12}, {"c", 12}, {"p", .5}, {"d", .25}, {"b", 7}}}}},
        {Tpl::Plcc, "PLCC", "J-lead chip carrier, pin 1 centre of top side",
         {{"n", "Pins per side (odd)", 11, 0}, {"p", "Pitch", 1.27, 3}, {"s", "Row span (centres)", 16.0, 3}, {"L", "Pad length", 2.0, 3}, {"W", "Pad width", .6, 3}},
         {{"PLCC-44", {{"n", 11}, {"s", 16.0}}}, {"PLCC-20", {{"n", 5}, {"s", 8.4}}}, {"PLCC-28", {{"n", 7}, {"s", 10.9}}},
          {"PLCC-68", {{"n", 17}, {"s", 23.6}}}, {"PLCC-84", {{"n", 21}, {"s", 28.7}}}}},
        {Tpl::Dip, "DIP", "Through-hole dual in-line, square pin 1",
         {{"n", "Pins (even)", 16, 0}, {"p", "Pitch", 2.54, 3}, {"s", "Row spacing", 7.62, 3}, {"d", "Pad diameter", 1.6, 3}, {"h", "Drill", .8, 3}},
         {{"DIP-16 300mil", {{"n", 16}, {"s", 7.62}}}, {"DIP-8 300mil", {{"n", 8}, {"s", 7.62}}}, {"DIP-28 600mil", {{"n", 28}, {"s", 15.24}}},
          {"DIP-40 600mil", {{"n", 40}, {"s", 15.24}}}}},
        {Tpl::Header, "SIP / Header", "Pin header, 1 or 2 rows (odd/even numbering)",
         {{"n", "Pins per row", 5, 0}, {"rows", "Rows (1 or 2)", 1, 0}, {"p", "Pitch", 2.54, 3}, {"d", "Pad diameter", 1.7, 3}, {"h", "Drill", 1.0, 3}},
         {{"1 row 2.54", {{"rows", 1}, {"p", 2.54}, {"d", 1.7}, {"h", 1.0}}}, {"2 rows 2.54", {{"rows", 2}, {"p", 2.54}, {"d", 1.7}, {"h", 1.0}}},
          {"1 row 2.00", {{"rows", 1}, {"p", 2.0}, {"d", 1.35}, {"h", .8}}}, {"2 rows 1.27", {{"rows", 2}, {"p", 1.27}, {"d", 1.0}, {"h", .65}}}}},
        {Tpl::Radial, "Radial TH", "Radial capacitor / LED, polarized",
         {{"p", "Lead pitch", 2.5, 3}, {"d", "Pad diameter", 1.6, 3}, {"h", "Drill", .8, 3}, {"D", "Body diameter", 6.3, 2}},
         {{"D5 p2.0", {{"p", 2.0}, {"D", 5}}}, {"D6.3 p2.5", {{"p", 2.5}, {"D", 6.3}}}, {"D8 p3.5", {{"p", 3.5}, {"D", 8}}}, {"D10 p5.0", {{"p", 5}, {"D", 10}}}}},
        {Tpl::Axial, "Axial TH", "Axial resistor / diode",
         {{"p", "Lead pitch", 10.16, 3}, {"d", "Pad diameter", 1.8, 3}, {"h", "Drill", .9, 3}, {"bl", "Body length", 6.3, 2}, {"bw", "Body diameter", 2.5, 2}},
         {{"0207 p10.16", {{"p", 10.16}, {"bl", 6.3}, {"bw", 2.5}}}, {"0204 p7.62", {{"p", 7.62}, {"bl", 3.6}, {"bw", 1.6}}}, {"0411 p15.24", {{"p", 15.24}, {"bl", 9.9}, {"bw", 3.6}}}}},
        // R16b (REVIEW_V15 §6): 4-pad SMD crystal / oscillator, pins CCW from bottom-left (1, 3 = crystal; 2, 4 = case / GND)
        {Tpl::Xtal4, "Crystal 4-pad", "SMD crystal / oscillator, 4 pads, pin 1 bottom-left, CCW",
         {{"px", "Pad centre distance X", 2.2, 3}, {"py", "Pad centre distance Y", 1.6, 3}, {"L", "Pad length (X)", 1.4, 3}, {"W", "Pad width (Y)", 1.15, 3}, {"bl", "Body length", 3.2, 2}, {"bw", "Body width", 2.5, 2}},
         {{"3225 (3.2x2.5)", {{"px", 2.2}, {"py", 1.6}, {"L", 1.4}, {"W", 1.15}, {"bl", 3.2}, {"bw", 2.5}}},
          {"2520 (2.5x2.0)", {{"px", 1.7}, {"py", 1.3}, {"L", 1.1}, {"W", 1.0}, {"bl", 2.5}, {"bw", 2.0}}},
          {"2016 (2.0x1.6)", {{"px", 1.45}, {"py", 1.1}, {"L", .9}, {"W", .8}, {"bl", 2.0}, {"bw", 1.6}}},
          {"1612 (1.6x1.2)", {{"px", 1.1}, {"py", .8}, {"L", .7}, {"W", .6}, {"bl", 1.6}, {"bw", 1.2}}},
          {"5032 (5.0x3.2)", {{"px", 3.7}, {"py", 2.4}, {"L", 2.0}, {"W", 1.6}, {"bl", 5.0}, {"bw", 3.2}}},
          {"7050 (7.0x5.0)", {{"px", 5.08}, {"py", 4.2}, {"L", 2.0}, {"W", 2.2}, {"bl", 7.0}, {"bw", 5.0}}}}},
    };
    return v;
}

const TSpec& tspec(Tpl k) { return tspecs()[(int)k]; }

using PMap = std::map<std::string, double>;
PMap tdefaults(Tpl k) {
    PMap m;
    for (auto& p : tspec(k).params) m[p.key] = p.def;
    if (!tspec(k).presets.empty()) for (auto& kv : tspec(k).presets[0].v) m[kv.first] = kv.second;
    return m;
}

Package make_template(Tpl k, const PMap& m, std::string* name_out) {
    auto V = [&](const char* key) { auto it = m.find(key); return it == m.end() ? 0.0 : it->second; };
    Package pk;
    auto add = [&](double x, double y, double w, double h, int pin, PadShape sh = PadShape::Rect, double drill = 0) {
        Pad p; p.pos = {x, y}; p.w = std::max(0.01, w); p.h = std::max(0.01, h); p.pin = pin; p.shape = sh; p.drill = drill;
        pk.pads.push_back(p);
    };
    auto body_rect = [&](double bl, double bw) {
        GPrim r; r.kind = GKind::Rect; r.pts = {{-bl / 2, -bw / 2}, {bl / 2, bw / 2}}; r.width = 0.12;
        pk.body.push_back(r);
    };
    std::string name;
    const std::string P = mm(V("p"));
    switch (k) {
    case Tpl::Chip: case Tpl::Melf: case Tpl::Sod: {
        double C = V("C");
        add(-C / 2, 0, V("L"), V("W"), 1);
        add(C / 2, 0, V("L"), V("W"), 2);
        body_rect(V("bl"), V("bw"));
        if (k == Tpl::Chip) name = "CHIP_" + mm(V("bl")) + "x" + mm(V("bw"));
        else {
            pk.polarized = true;
            GPrim ln; ln.kind = GKind::Line; ln.width = 0.24;
            double x = -V("bl") / 2 + V("bl") * 0.18;
            ln.pts = {{x, -V("bw") / 2}, {x, V("bw") / 2}};
            pk.body.push_back(ln);
            name = (k == Tpl::Melf ? "MELF_" : "SOD_") + mm(V("bl")) + "x" + mm(V("bw"));
        }
    } break;
    case Tpl::Sot23: {
        int n = (int)V("n");
        double p = V("p"), s = V("s") / 2, L = V("L"), W = V("W");
        if (n == 3) { add(-p, -s, W, L, 1); add(p, -s, W, L, 2); add(0, s, W, L, 3); }
        else if (n == 5) { add(-p, -s, W, L, 1); add(0, -s, W, L, 2); add(p, -s, W, L, 3); add(p, s, W, L, 4); add(-p, s, W, L, 5); }
        else { n = 6; add(-p, -s, W, L, 1); add(0, -s, W, L, 2); add(p, -s, W, L, 3); add(p, s, W, L, 4); add(0, s, W, L, 5); add(-p, s, W, L, 6); }
        body_rect(V("bl"), V("bw"));
        name = "SOT23-" + std::to_string(n) + "_" + P;
    } break;
    case Tpl::Sot223: {
        double p = V("p"), s = V("s") / 2, L = V("L"), W = V("W");
        add(-p, -s, W, L, 1); add(0, -s, W, L, 2); add(p, -s, W, L, 3); add(0, s, V("tw"), L, 4);
        body_rect(V("bl"), V("bw"));
        name = "SOT223_" + P;
    } break;
    case Tpl::Sot89: {
        double p = V("p"), y = V("y"), L = V("L"), W = V("W");
        add(-p, y, W, L, 1); add(0, y, W, L, 2); add(p, y, W, L, 3);
        add(0, y + L / 2 + V("th") / 2 - 0.2, V("tw"), V("th"), 4);
        body_rect(V("bl"), V("bw"));
        name = "SOT89_" + P;
    } break;
    case Tpl::Soic: {
        int n = std::max(2, (int)V("n") / 2 * 2), h = n / 2;
        double p = V("p"), s = V("s") / 2;
        for (int i = 0; i < h; ++i) add((i - (h - 1) / 2.0) * p, -s, V("W"), V("L"), i + 1);
        for (int i = 0; i < h; ++i) add(((h - 1) / 2.0 - i) * p, s, V("W"), V("L"), h + i + 1);
        double bl = (h - 1) * p + std::max(1.0, p * 1.2);
        pk.notched = true;
        pk.pin_marked = true;
        render_body(pk);
        pk.body.erase(pk.body.begin());
        GPrim r; r.kind = GKind::Rect; r.pts = {{-bl / 2, -V("bw") / 2}, {bl / 2, V("bw") / 2}}; r.width = 0.12;
        pk.body.insert(pk.body.begin(), r);
        name = std::string(p < 1.0 ? "SSOP" : "SOIC") + std::to_string(n) + (V("bw") >= 7 ? "W" : "") + "_" + P;
    } break;
    case Tpl::Qfp: case Tpl::Qfn: {
        int n = std::max(1, (int)V("n"));
        double p = V("p"), s = V("s") / 2, L = V("L"), W = V("W");
        int pin = 1;
        for (int i = 0; i < n; ++i) add(-s, ((n - 1) / 2.0 - i) * p, L, W, pin++);          // left, top->bottom
        for (int i = 0; i < n; ++i) add((i - (n - 1) / 2.0) * p, -s, W, L, pin++);          // bottom, left->right
        for (int i = 0; i < n; ++i) add(s, (i - (n - 1) / 2.0) * p, L, W, pin++);           // right, bottom->top
        for (int i = 0; i < n; ++i) add(((n - 1) / 2.0 - i) * p, s, W, L, pin++);           // top, right->left
        if (V("ep") > 0) add(0, 0, V("ep"), V("ep"), pin++);
        double b = V("b");
        body_rect(b, b);
        GPrim m; m.kind = GKind::Circle; m.filled = true; m.radius = std::clamp(b * 0.04, 0.15, 0.5);
        m.pts = {{-b / 2 + m.radius * 2.5, b / 2 - m.radius * 2.5}};
        pk.body.push_back(m);
        pk.pin_marked = true;
        name = std::string(k == Tpl::Qfp ? "QFP" : "QFN") + std::to_string(4 * n) + "_" + P;
    } break;
    case Tpl::Bga: {
        int R = std::max(1, (int)V("r")), Cn = std::max(1, (int)V("c")), dep = std::max(0, (int)V("dep"));
        double p = V("p"), d = V("d");
        int pin = 1;
        for (int r = 0; r < R; ++r)
            for (int c = 0; c < Cn; ++c) {
                int rr = r - (R - dep) / 2, cc = c - (Cn - dep) / 2;
                if (dep > 0 && rr >= 0 && rr < dep && cc >= 0 && cc < dep) continue;
                add((c - (Cn - 1) / 2.0) * p, ((R - 1) / 2.0 - r) * p, d, d, pin++, PadShape::Round);
            }
        double b = std::max(V("b"), std::max(R, Cn) * p);
        body_rect(b, b);
        GPrim ch; ch.kind = GKind::Polyline; ch.width = 0.12;
        ch.pts = {{-b / 2, b / 2 - b * 0.08}, {-b / 2 + b * 0.08, b / 2}};
        pk.body.push_back(ch);
        name = "BGA" + std::to_string(pin - 1) + "_" + P;
    } break;
    case Tpl::Plcc: {
        int n = std::max(1, (int)V("n") | 1);
        double p = V("p"), s = V("s") / 2, L = V("L"), W = V("W");
        int m = n / 2, pin = 1;
        std::vector<std::pair<Vec2, bool>> ring;   // pos, horizontal-side
        for (int i = 0; i < n; ++i) ring.push_back({{((n - 1) / 2.0 - i) * p, s}, true});      // top right->left
        for (int i = 0; i < n; ++i) ring.push_back({{-s, ((n - 1) / 2.0 - i) * p}, false});    // left top->bottom
        for (int i = 0; i < n; ++i) ring.push_back({{(i - (n - 1) / 2.0) * p, -s}, true});     // bottom
        for (int i = 0; i < n; ++i) ring.push_back({{s, (i - (n - 1) / 2.0) * p}, false});     // right
        // pin 1 = centre of top side (index m in the top run)
        for (size_t i = 0; i < ring.size(); ++i) {
            auto& e = ring[(i + m) % ring.size()];
            add(e.first.x, e.first.y, e.second ? W : L, e.second ? L : W, pin++);
        }
        pk.j_lead = true;
        pk.pin_marked = true;
        render_body(pk);
        name = "PLCC" + std::to_string(4 * n);
    } break;
    case Tpl::Dip: {
        int n = std::max(2, (int)V("n") / 2 * 2), h = n / 2;
        double p = V("p"), s = V("s") / 2;
        for (int i = 0; i < h; ++i) add((i - (h - 1) / 2.0) * p, -s, V("d"), V("d"), i + 1, i == 0 ? PadShape::Square : PadShape::Round, V("h"));
        for (int i = 0; i < h; ++i) add(((h - 1) / 2.0 - i) * p, s, V("d"), V("d"), h + i + 1, PadShape::Round, V("h"));
        pk.smt = false;
        pk.notched = true;
        double bl = (h - 1) * p + p, bw = std::max(0.5, V("s") - V("d") - 0.6);
        body_rect(bl, bw);
        GPrim a; a.kind = GKind::Arc; a.width = 0.12; a.a0 = -90; a.a1 = 180; a.radius = std::clamp(bw * 0.15, 0.3, 1.2); a.pts = {{-bl / 2, 0}};
        pk.body.push_back(a);
        name = "DIP" + std::to_string(n) + (V("s") >= 12 ? "W" : "") + "_" + P;
    } break;
    case Tpl::Header: {
        int n = std::max(1, (int)V("n")), rows = V("rows") >= 2 ? 2 : 1;
        double p = V("p");
        int pin = 1;
        for (int i = 0; i < n; ++i)
            for (int r = 0; r < rows; ++r) {
                double y = rows == 2 ? (r == 0 ? -p / 2 : p / 2) : 0;
                add((i - (n - 1) / 2.0) * p, y, V("d"), V("d"), pin, pin == 1 ? PadShape::Square : PadShape::Round, V("h"));
                ++pin;
            }
        pk.smt = false;
        body_rect(n * p, rows * p);
        name = (rows == 2 ? "HDR2x" + std::to_string(n) : "SIP" + std::to_string(n)) + "_" + P;
    } break;
    case Tpl::Radial: {
        double p = V("p");
        add(-p / 2, 0, V("d"), V("d"), 1, PadShape::Square, V("h"));
        add(p / 2, 0, V("d"), V("d"), 2, PadShape::Round, V("h"));
        pk.smt = false; pk.radial = true; pk.polarized = true;
        GPrim c; c.kind = GKind::Circle; c.pts = {{0, 0}}; c.radius = V("D") / 2; c.width = 0.12;
        pk.body.push_back(c);
        GPrim ln; ln.kind = GKind::Line; ln.width = 0.12;
        double x = -V("D") / 2 * 0.7, hl = std::sqrt(std::max(0.0, V("D") * V("D") / 4 - x * x));
        ln.pts = {{x, -hl}, {x, hl}};
        pk.body.push_back(ln);
        name = "RAD_" + P + "_D" + mm(V("D"));
    } break;
    case Tpl::Xtal4: {
        const double x = V("px") / 2, y = V("py") / 2, L = V("L"), W = V("W");
        add(-x, -y, L, W, 1); add(x, -y, L, W, 2); add(x, y, L, W, 3); add(-x, y, L, W, 4);
        body_rect(V("bl"), V("bw"));
        GPrim m; m.kind = GKind::Circle; m.filled = true; m.radius = std::clamp(V("bw") * 0.05, 0.08, 0.3);
        m.pts = {{-V("bl") / 2 - m.radius * 2, -V("bw") / 2}};
        pk.body.push_back(m);
        name = "XTAL4_" + mm(V("bl")) + "x" + mm(V("bw"));
    } break;
    case Tpl::Axial: {
        double p = V("p");
        add(-p / 2, 0, V("d"), V("d"), 1, PadShape::Square, V("h"));
        add(p / 2, 0, V("d"), V("d"), 2, PadShape::Round, V("h"));
        pk.smt = false;
        body_rect(V("bl"), V("bw"));
        GPrim l1; l1.kind = GKind::Line; l1.width = 0.12; l1.pts = {{-p / 2, 0}, {-V("bl") / 2, 0}};
        GPrim l2 = l1; l2.pts = {{V("bl") / 2, 0}, {p / 2, 0}};
        pk.body.push_back(l1); pk.body.push_back(l2);
        name = "AXIAL_" + P;
    } break;
    default: break;
    }
    pk.leads = (int)pk.pads.size();
    pk.shape_confirmed = true;
    pk.description = std::string(tspec(k).title) + " (template)";
    if (name_out) *name_out = name;
    return pk;
}

// ===========================================================================
// View + canvas
// ===========================================================================
class PackagesView;

class PkgCanvas : public GCanvas {
public:
    PackagesView* v = nullptr;
    Vec2 drag_d{};
    int drag_primary = -1;          // pad index, or -2 when dragging graphics only
    Vec2 drag_anchor{};
    bool measuring = false, has_measure = false;
    Vec2 m0{}, m1{};
    bool ctx_pending = false;
    // drawing tools
    bool drawing = false;
    Vec2 d0{}, d1{};
    std::vector<Vec2> poly;         // polyline in progress
    double grid() const { return grid_mm_; }
    bool snapping() const { return snap_; }
    bool grid_on() const { return grid_; }
    void cancel_draw() { drawing = false; poly.clear(); }

protected:
    BBox world_extent() const override;
    void paint_world(ID2D1DeviceContext* rt) override;
    void paint_overlay(ImDrawList* dl) override;
    void on_click(Vec2 w, int mods) override;
    void on_dblclick(Vec2 w, int mods) override;
    void on_rect(const BBox& w, int mods) override;
    bool wants_rubber_band() const override;
    bool hit_drag_target(Vec2 w, int mods) override;
    void on_drag(Vec2 from, Vec2 to, int mods, bool done) override;
    void on_context(Vec2 w) override { (void)w; ctx_pending = true; ctx_world = w; }
    void on_mouse_move(Vec2 w) override;
    bool on_key(ImGuiKey k, int mods) override;
    ImGuiMouseCursor cursor() const override;

public:
    Vec2 ctx_world{};
};

enum class Tool { Select, AddPad, Measure, SetPin1, PinOrder, SetOrigin, DrawLine, DrawRect, DrawCircle, DrawArc, DrawPoly, DrawText };
bool is_draw_tool(Tool t) { return t >= Tool::DrawLine; }

struct Thumb {
    RenderTexture tex;
    uint64_t hash = 0;
    bool dark = false;
    int px = 0;
    int last = 0;
};

struct PadStats {
    uint64_t gen = 0;
    const void* pkg = nullptr;
    double pitch = 0;
    BBox field, body;
};

enum class OriginMode { Click, PadCentre, LeadCentroid, Pin1, BodyCentre };

class PackagesView : public View {
public:
    PackagesView() { canvas_.v = this; canvas_.set_grid(true, 0.05); canvas_.set_snap(true); }
    ViewKind kind() const override { return ViewKind::Packages; }
    const char* title() const override { return "Packages"; }
    ui::Icon icon() const override { return ui::Icon::PackageEditor; }
    void draw(ImVec2 size) override;
    void draw_toolbar() override;
    bool on_command(int id) override;
    int cmd_state(int id) override;
    std::string status() const override;
    std::string status_right() const override;
    void on_activate() override { canvas_.invalidate(); }

    // ---- state used by the canvas
    // editable project package (nullptr in Library mode)
    Package* pkg() {
        if (!doc || cur_.empty() || mode_ != 0) return nullptr;
        auto it = doc->prj.packages.find(cur_);
        return it == doc->prj.packages.end() ? nullptr : &it->second;
    }
    // package shown in the canvas (project package, or read-only library preview)
    const Package* shown() {
        if (mode_ == 0) return pkg();
        auto& m = curlib().packages;
        auto it = m.find(lcur_);
        return it == m.end() ? nullptr : &it->second;
    }
    PackageLibrary& curlib() { return (lsrc_ == 1 && !LIB().source.path.empty()) ? LIB().source : LIB().master; }
    int hit_pad(Vec2 w, double tol_px = 3) {
        Package* p = pkg();
        if (!p) return -1;
        double tol = tol_px / std::max(1e-6, canvas_.px_per_mm());
        for (int i = (int)p->pads.size() - 1; i >= 0; --i) if (pad_hit(p->pads[i], w, tol)) return i;
        return -1;
    }
    int hit_prim(Vec2 w, double tol_px = 5) {
        Package* p = pkg();
        if (!p) return -1;
        double tol = tol_px / std::max(1e-6, canvas_.px_per_mm());
        int best = -1; double bd = 1e300;
        for (int i = (int)p->body.size() - 1; i >= 0; --i) {
            double d = prim_dist(p->body[i], w) - p->body[i].width * 0.5;
            if (d <= tol && d < bd) { bd = d; best = i; }
        }
        return best;
    }
    std::set<int> psel_;            // selected pads
    std::set<int> gsel_;            // selected body graphics
    int hover_ = -1, ghover_ = -1;
    Tool tool_ = Tool::Select;
    int pin_seq_ = 1;
    PadStats stats_;
    const PadStats& stats();
    double stroke_ = 0.12;          // mm, new graphics
    bool fill_ = false;
    double arc_sweep_ = 180;
    double text_h_ = 1.0;

    // ---- edits
    void edited() { doc->changed(ui::CH_GEOMETRY); canvas_.invalidate(); }
    void add_pad(Vec2 at);
    void delete_pads();
    void delete_selection();
    void move_pads(Vec2 d, const char* label = "Move pads");
    void rotate_pads();
    void set_pin1(int i);
    void pin_order_click(int i);
    void rotate_package();
    void mirror_package();
    void renumber(int mode);
    void auto_body();
    void set_origin(OriginMode m, Vec2 at = {});
    void pad_array();
    void copy_pads();
    void paste_pads();
    void add_prim(GPrim g, const char* label);
    void add_text_at(Vec2 at);
    void toggle_flag(bool Package::*m, const char* label);
    void select_package(const std::string& name);
    void set_tool(Tool t) {
        if (tool_ != t) canvas_.cancel_draw();
        if (tool_ == Tool::PinOrder && t != Tool::PinOrder && pord_.active) pin_order_cancel();
        tool_ = t;
        if (t == Tool::PinOrder) { if (Package* p = pkg()) pord_.begin(*p); pin_seq_ = 1; }
    }
    // ---- template edits gated by package_edit (shared-package confirmation + compensation)
    PinOrderSession pord_;          // transactional: nothing written until commit
    void pin_order_commit();
    void pin_order_cancel();
    void gated(const std::string& label, std::function<PkgEditResult(Project&, const PkgEditOpts&)> fn);
    void rotate_by(double deg, bool compensate);
    double rot_dlg_ = -30;
    int rot_mode_ = 0;              // 0 compensate placements, 1 keep placements (board changes)
    double off_dx_ = 0, off_dy_ = 0;
    void draw_rotate_popup();
    void draw_orientation_props(Package* p);
    // ---- R16b: unresolved packages (core/package_resolve.h) + tape orientation
    void show_templates() { right_tab_ = 3; if (!show_right_) { show_right_ = true; app::set_ui_pref("Pkg.ShowProps", 1); } maximized_ = false; }
    void draw_resolve_props(Package* p);
    void draw_tape_props(Package* p);
    void load_from_library(const std::string& target, const std::string& lib_name);
    int resolve_exact_library(bool dry_run);
    std::vector<PkgSuggestion> sugg_;
    std::string sugg_key_;
    char lib_pick_[96] = {};

private:
    friend class PkgCanvas;
    void sync();
    void rebuild_rows();
    void rebuild_lib_rows();
    void refresh_sync();
    void draw_library(float w, float h);
    void draw_mode_switch();
    void draw_items(const std::vector<std::string>& rows, bool lib, float footer);
    void draw_lib_context();
    void draw_mlib_context();
    void draw_editor(float w, float h);
    void draw_edit_toolbar(float w);
    void draw_flags_row();
    void draw_canvas_context();
    void draw_inspector(float w, float h);
    void draw_properties();
    void draw_lib_preview_props();
    void draw_pad_table();
    void draw_graphics();
    void draw_templates(float w);
    void* thumb(const std::string& key, const Package& p, int px, int& budget);
    void lib_click(const std::string& name);
    void mlib_click(const std::string& name);
    std::vector<std::string> lib_targets() const;   // multi-selection or current
    std::vector<std::string> mlib_targets() const;
    // package-level actions (deferred: they may open dialogs)
    void act_new();
    void act_duplicate();
    void act_rename();
    void act_delete_unused();
    void act_merge();
    void act_export(bool selection_only);
    void act_import();
    void act_show_users();
    void open_template(Tpl k, const std::string& assign_to = "", int preset = 0, const std::map<std::string, double>& params = {});
    // master library
    void act_transfer(bool to_lib);
    void act_lib_delete();
    void act_lib_open_source();
    void act_lib_choose_master(bool create_new);
    std::string unique_name(const std::string& base) const { return unique_package_name(doc->prj.packages, base); }
    std::string fmt_n(double mm_v) const;          // display units at the editor precision
    bool len_input(const char* id, double& v, double minv = -1e300);   // returns true when committed (value changed)

    PkgCanvas canvas_;
    std::string cur_;
    std::set<std::string> lsel_;
    std::string anchor_;
    std::vector<std::string> rows_;
    std::map<std::string, int> use_comps_, use_parts_;
    uint64_t seen_gen_ = 0, rows_gen_ = 0;
    std::string rows_key_;
    char search_[128] = {};
    int sort_ = 0;                  // 0 name, 1 usage, 2 pads
    bool sort_desc_ = false;
    unsigned filt_ = 0;             // bit: 0 used 1 unused 2 smt 3 th 4 reconstructed 5 no pads 6 local-only 7 differs
    bool grid_view_ = false;          // == (view_mode_ == 0), kept for existing code paths
    int view_mode_ = 1;
    bool vis_body_ = true, vis_pads_ = true, vis_pins_ = true, vis_origin_ = true;   // R10 §9 editor layers
    // R11 P2-01 / P2-02 / P2-05: measurement overlays + stats card off by default; collapsible / resizable side panes;
    // the central library lives in the Package Library Manager window (mgr_open_)
    bool vis_dims_ = false, vis_stats_ = false;
    bool show_left_ = true, show_right_ = true, panes_loaded_ = false;
    float lw_ = 0, rw_ = 0;
    bool mgr_open_ = false;
    Tool last_draw_ = Tool::DrawLine;
    void draw_vtoolbar(float h);
    void draw_view_controls();
    void draw_manager();
    void list_keys(const std::vector<std::string>& rows, bool lib);
    // R12 §7: production hotkeys shared by the focused list and the canvas (R / Shift+R / P)
    bool pkg_hotkeys(bool from_canvas);
    void step_package(int d);           // Up / Down: previous / next package in the visible list
    void draw_lead_inspector(Package* p);
    // R15 polarity marker
    void toggle_polarity();       // P: add red dot near lead 1 / remove
    void cycle_marker_type();     // Shift+P: Dot <-> Arrow
    void move_marker(Vec2 d, const char* label = "Move polarity marker");
    void marker_edit(const char* label, std::function<void(Package&)> f);
    // R18c multi-select (Ctrl / Shift-click, Ctrl+A in the list): an edit applies ONLY its own field to every
    // selected package, as one undo step with one aggregated shared-package confirmation.
    std::vector<std::string> targets();
    void multi_edit(const std::string& label, std::function<void(Package&)> f);
    void draw_header_bar(float w);      // compact bar above the drawing: name, process, polarity, height
    void draw_body_style();             // Graphics tab: body style + suggest-from-leads (was the flags row)
    void rename_current(const std::string& n);
    double marker_size_mm(const Package& p) const;
    bool hit_marker(Vec2 w, double scale);
    bool msel_ = false;           // polarity marker selected in the editor
    bool maximized_ = false, max_left_ = true, max_right_ = false;   // Ctrl+Space (Blender-like maximize area)
    // R12 §8: "Merge similar packages..." (tolerance-aware grouping)
    bool msim_open_ = false;
    std::vector<SimilarGroup> msim_;
    std::vector<int> msim_choice_;      // 1 merge, 0 keep separate (remembered), -1 leave
    float msim_abs_um_ = 50, msim_rel_pct_ = 5;
    bool msim_orient_ = true;
    void open_merge_similar();
    void draw_merge_similar();
    std::map<std::string, double> rot_acc_;   // rotation applied per package since it was opened (for "0 deg")               // R10 §9: 0 thumbnails, 1 compact preview, 2 plain text list
    bool compensate_ = true;
    bool auto_render_ = true;
    bool lib_focus_ = false;
    bool scroll_to_cur_ = false;
    std::map<std::string, std::unique_ptr<Thumb>> thumbs_;
    int frame_ = 0;
    std::vector<Pad> clip_;
    std::string rename_target_;
    int right_tab_ = 0;             // 0 properties 1 leads 2 graphics 3 templates
    std::vector<Package> tpl_preview_;
    int prec_ = 3;                  // decimals for numeric fields
    // master library pane
    int mode_ = 0;                  // 0 project packages, 1 master library
    int lsrc_ = 0;                  // 0 master, 1 read-only source
    std::string lcur_, lanchor_;
    std::set<std::string> llsel_;
    std::vector<std::string> lrows_;
    std::string lrows_key_;
    char lsearch_[128] = {};
    unsigned lfilt_ = 0;            // bit 0 not in project, 1 differs, 2 in project
    ConflictAction policy_ = ConflictAction::Ask;
    std::map<std::string, SyncState> psync_, lsync_;
    uint64_t sync_doc_gen_ = 0, sync_lib_gen_ = 0;
    const void* sync_lib_ptr_ = nullptr;
};

// ---------------------------------------------------------------------------
// canvas implementation
// ---------------------------------------------------------------------------
BBox PkgCanvas::world_extent() const {
    const Package* p = v->shown();
    if (!p) return BBox(-5, -5, 5, 5);
    BBox e = p->extent();
    e.add({0, 0});
    double m = std::max(0.6, std::max(e.w(), e.h()) * 0.18);
    e.inflate(m);
    return e;
}

void PkgCanvas::paint_world(ID2D1DeviceContext* rt) {
    const Package* p = v->shown();
    if (!p) return;
    PaintOpts o;
    o.hair = (float)(1.0 / std::max(1e-6, scale_));
    o.sel = v->mode_ == 0 ? &v->psel_ : nullptr;
    o.drag = drag_d;
    o.courtyard = true;
    o.centroid = v->vis_origin_;
    o.body = v->vis_body_;
    o.pads = v->vis_pads_;
    o.bg = im_argb(background());
    paint_package(rt, *p, o);
    // selected / dragged graphics
    if (v->mode_ == 0 && !v->gsel_.empty()) {
        Com<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(ui::d2c(ui::pal().selection), b.put());
        if (b) for (int i : v->gsel_) if (i < (int)p->body.size()) {
            GPrim g = p->body[i];
            offset_prim(g, drag_d);
            g.width = std::max(g.width, (double)o.hair * 2.5);
            draw_prim(rt, g, b.get(), o.hair * 2.5f);
        }
    }
}

bool PkgCanvas::wants_rubber_band() const { return v->tool_ == Tool::Select; }

void PkgCanvas::on_click(Vec2 w, int mods) {
    v->lib_focus_ = false;
    Package* p = v->pkg();
    if (!p) return;
    const int i = v->hit_pad(w);
    switch (v->tool_) {
    case Tool::Select: {
        const bool add = (mods & (KM_SHIFT | KM_CTRL)) != 0;
        if (v->mode_ == 0 && v->hit_marker(w, scale_)) {      // R15: polarity marker is pickable
            v->msel_ = true; v->psel_.clear(); v->gsel_.clear();
            if (v->right_tab_ != 0) v->right_tab_ = 0;
            dirty_ = true;
            break;
        }
        v->msel_ = false;
        if (i >= 0) {
            if (!add) v->gsel_.clear();
            if (mods & KM_CTRL) { if (!v->psel_.erase(i)) v->psel_.insert(i); }
            else if (mods & KM_SHIFT) v->psel_.insert(i);
            else { v->psel_.clear(); v->psel_.insert(i); }
        } else if (int g = v->hit_prim(w); g >= 0) {
            if (!add) v->psel_.clear();
            if (mods & KM_CTRL) { if (!v->gsel_.erase(g)) v->gsel_.insert(g); }
            else if (mods & KM_SHIFT) v->gsel_.insert(g);
            else { v->gsel_.clear(); v->gsel_.insert(g); }
            if (v->right_tab_ == 0 && !add) v->right_tab_ = 2;
        } else if (!add) { v->psel_.clear(); v->gsel_.clear(); }
        dirty_ = true;
    } break;
    case Tool::AddPad: v->add_pad(snap_pt(w)); break;
    case Tool::SetPin1: if (i >= 0) v->set_pin1(i); break;
    case Tool::PinOrder: if (i >= 0) v->pin_order_click(i); break;
    case Tool::Measure: has_measure = false; break;
    case Tool::SetOrigin: {
        int k = v->hit_pad(w, 6);
        v->set_origin(OriginMode::Click, k >= 0 ? p->pads[k].pos : snap_pt(w));
    } break;
    case Tool::DrawPoly: {
        Vec2 q = snap_pt(w);
        if (!poly.empty() && dist(q, poly.front()) < 6 / std::max(1e-6, scale_) && poly.size() >= 3) {
            GPrim g; g.kind = GKind::Polygon; g.pts = poly; g.width = v->stroke_; g.filled = v->fill_;
            poly.clear();
            v->add_prim(g, "Draw polygon");
        } else poly.push_back(q);
    } break;
    case Tool::DrawText: v->add_text_at(snap_pt(w)); break;
    default: break;
    }
}

void PkgCanvas::on_dblclick(Vec2 w, int mods) {
    (void)mods;
    if (v->tool_ == Tool::DrawPoly) {
        // the double-click's first click already added the point; finish
        if (poly.size() >= 2) {
            if (poly.size() >= 3 && dist(poly[poly.size() - 1], poly[poly.size() - 2]) < 1e-9) poly.pop_back();
            GPrim g; g.kind = GKind::Polyline; g.pts = poly; g.width = v->stroke_;
            poly.clear();
            if (g.pts.size() >= 2) v->add_prim(g, "Draw polyline");
        }
        return;
    }
    if (v->tool_ != Tool::Select) return;
    int i = v->hit_pad(w);
    if (i >= 0) { v->psel_ = {i}; v->gsel_.clear(); v->right_tab_ = 1; dirty_ = true; return; }
    int g = v->hit_prim(w);
    if (g >= 0) { v->gsel_ = {g}; v->psel_.clear(); v->right_tab_ = 2; dirty_ = true; }
}

void PkgCanvas::on_rect(const BBox& b, int mods) {
    Package* p = v->pkg();
    if (!p || v->tool_ != Tool::Select) return;
    if (!(mods & (KM_SHIFT | KM_CTRL))) { v->psel_.clear(); v->gsel_.clear(); }
    for (size_t i = 0; i < p->pads.size(); ++i) if (b.contains(p->pads[i].pos)) v->psel_.insert((int)i);
    for (size_t i = 0; i < p->body.size(); ++i) {
        BBox gb = gprim_bbox(p->body[i]);
        if (!gb.empty() && b.contains({gb.x0, gb.y0}) && b.contains({gb.x1, gb.y1})) v->gsel_.insert((int)i);
    }
    dirty_ = true;
}

bool PkgCanvas::hit_drag_target(Vec2 w, int mods) {
    v->lib_focus_ = false;
    Package* p = v->pkg();
    if (!p) return false;
    if (v->tool_ == Tool::Measure) {
        int i = v->hit_pad(w, 10);
        m0 = i >= 0 ? p->pads[i].pos : snap_pt(w);
        m1 = m0;
        measuring = true;
        has_measure = true;
        return true;
    }
    if (is_draw_tool(v->tool_) && v->tool_ != Tool::DrawPoly && v->tool_ != Tool::DrawText) {
        drawing = true;
        d0 = d1 = snap_pt(w);
        return true;
    }
    if (v->tool_ != Tool::Select) return false;
    if (v->mode_ == 0 && v->hit_marker(w, scale_)) {          // R15: drag the polarity marker
        v->msel_ = true; v->psel_.clear(); v->gsel_.clear();
        drag_primary = -3;
        drag_anchor = p->marker_pos;
        drag_d = {};
        return true;
    }
    v->msel_ = false;
    int i = v->hit_pad(w);
    if (i >= 0) {
        if (!v->psel_.count(i)) {
            if (!(mods & (KM_SHIFT | KM_CTRL))) { v->psel_.clear(); v->gsel_.clear(); }
            v->psel_.insert(i);
        }
        drag_primary = i;
        drag_anchor = p->pads[i].pos;
        drag_d = {};
        return true;
    }
    int g = v->hit_prim(w);
    if (g < 0) return false;
    if (!v->gsel_.count(g)) {
        if (!(mods & (KM_SHIFT | KM_CTRL))) { v->psel_.clear(); v->gsel_.clear(); }
        v->gsel_.insert(g);
    }
    drag_primary = -2;
    drag_anchor = snap_pt(w);
    drag_d = {};
    return true;
}

void PkgCanvas::on_drag(Vec2 from, Vec2 to, int mods, bool done) {
    (void)mods;
    Package* p = v->pkg();
    if (v->tool_ == Tool::Measure) {
        int i = v->hit_pad(to, 10);
        m1 = i >= 0 && p ? p->pads[i].pos : snap_pt(to);
        if (done) measuring = false;
        return;
    }
    if (drawing) {
        d1 = snap_pt(to);
        if (done) {
            drawing = false;
            Vec2 a = d0, b = d1;
            const double mn = 2 / std::max(1e-6, scale_);
            if (dist(a, b) < mn || !p) return;
            GPrim g; g.width = v->stroke_;
            switch (v->tool_) {
            case Tool::DrawLine: g.kind = GKind::Line; g.pts = {a, b}; break;
            case Tool::DrawRect:
                if (std::fabs(a.x - b.x) < mn || std::fabs(a.y - b.y) < mn) return;
                g.kind = GKind::Rect; g.pts = {{std::min(a.x, b.x), std::min(a.y, b.y)}, {std::max(a.x, b.x), std::max(a.y, b.y)}}; g.filled = v->fill_; break;
            case Tool::DrawCircle: g.kind = GKind::Circle; g.pts = {a}; g.radius = dist(a, b); g.filled = v->fill_; break;
            case Tool::DrawArc: g.kind = GKind::Arc; g.pts = {a}; g.radius = dist(a, b); g.a0 = rad2deg(std::atan2(b.y - a.y, b.x - a.x)); g.a1 = v->arc_sweep_; break;
            default: return;
            }
            v->add_prim(g, "Draw graphics");
        }
        return;
    }
    if (!p) return;
    if (drag_primary == -1) return;
    Vec2 base = drag_anchor;
    Vec2 d = snap_ ? snap_pt(base + (to - from)) - base : to - from;
    drag_d = d;
    if (done) {
        drag_d = {};
        if (std::fabs(d.x) > 1e-9 || std::fabs(d.y) > 1e-9) { if (drag_primary == -3) v->move_marker(d); else v->move_pads(d, "Move"); }
        drag_primary = -1;
    }
    dirty_ = true;
}

void PkgCanvas::on_mouse_move(Vec2 w) {
    int h = v->hit_pad(w);
    int g = h < 0 && v->tool_ == Tool::Select ? v->hit_prim(w) : -1;
    if (h != v->hover_ || g != v->ghover_) { v->hover_ = h; v->ghover_ = g; request_redraw(2); }
    if (is_draw_tool(v->tool_)) request_redraw(1);
}

bool PkgCanvas::on_key(ImGuiKey k, int mods) {
    Package* p = v->pkg();
    if (!p) return false;
    const bool sel = !v->psel_.empty() || !v->gsel_.empty();
    double step = snap_ ? grid_mm_ : 0.01;
    if (mods & KM_SHIFT) step *= 10;
    if (v->msel_ && pkg_has_polarity(*p) && !sel) {           // R15: nudge / delete the selected polarity marker
        switch (k) {
        case ImGuiKey_LeftArrow: v->move_marker({-step, 0}, "Nudge marker"); return true;
        case ImGuiKey_RightArrow: v->move_marker({step, 0}, "Nudge marker"); return true;
        case ImGuiKey_UpArrow: v->move_marker({0, step}, "Nudge marker"); return true;
        case ImGuiKey_DownArrow: v->move_marker({0, -step}, "Nudge marker"); return true;
        case ImGuiKey_Delete: case ImGuiKey_Backspace: v->msel_ = false; v->marker_edit("Remove polarity marker", [](Package& q) { pkg_remove_polarity_marker(q); }); return true;
        case ImGuiKey_Escape: v->msel_ = false; dirty_ = true; return true;
        default: break;
        }
    }
    switch (k) {
    case ImGuiKey_LeftArrow: if (sel) { v->move_pads({-step, 0}, "Nudge"); return true; } return false;
    case ImGuiKey_RightArrow: if (sel) { v->move_pads({step, 0}, "Nudge"); return true; } return false;
    // R12 §7: with leads / graphics selected the arrows nudge them; with nothing selected Up / Down step through
    // the package list (so Select package -> R -> Down -> R ... works with the mouse over the drawing too)
    case ImGuiKey_UpArrow: if (sel) { v->move_pads({0, step}, "Nudge"); return true; } if (v->mode_ == 0 && !(mods & (KM_CTRL | KM_ALT))) { v->step_package(-1); return true; } return false;
    case ImGuiKey_DownArrow: if (sel) { v->move_pads({0, -step}, "Nudge"); return true; } if (v->mode_ == 0 && !(mods & (KM_CTRL | KM_ALT))) { v->step_package(1); return true; } return false;
    case ImGuiKey_P: if (mods & (KM_CTRL | KM_ALT)) return false; if (v->mode_ == 0) { if (mods & KM_SHIFT) v->cycle_marker_type(); else v->toggle_polarity(); return true; } return false;
    case ImGuiKey_Delete: if (sel) { v->delete_selection(); return true; } return false;
    case ImGuiKey_Enter:
        if (v->tool_ == Tool::PinOrder && v->pord_.active) { v->pin_order_commit(); return true; }
        if (v->tool_ == Tool::DrawPoly && poly.size() >= 2) {
            GPrim g; g.kind = GKind::Polyline; g.pts = poly; g.width = v->stroke_;
            poly.clear();
            v->add_prim(g, "Draw polyline");
            return true;
        }
        return false;
    case ImGuiKey_Escape:
        if (drawing || !poly.empty()) { cancel_draw(); dirty_ = true; return true; }
        if (v->tool_ == Tool::PinOrder && v->pord_.active) { v->pin_order_cancel(); dirty_ = true; return true; }
        v->set_tool(Tool::Select); v->psel_.clear(); v->gsel_.clear(); has_measure = false; dirty_ = true; return true;
    case ImGuiKey_Tab: if (!p->pads.empty()) {
        int cur = v->psel_.empty() ? -1 : *v->psel_.begin();
        cur = (cur + ((mods & KM_SHIFT) ? (int)p->pads.size() - 1 : 1)) % (int)p->pads.size();
        v->psel_ = {cur}; v->gsel_.clear(); dirty_ = true; return true;
    } return false;
    case ImGuiKey_R:
        if (mods & (KM_CTRL | KM_ALT)) return false;
        if (!v->psel_.empty()) v->rotate_pads();
        else if (v->pkg()) v->rotate_by((mods & KM_SHIFT) ? -90 : 90, v->compensate_);
        return true;
    case ImGuiKey_A: if (mods & KM_CTRL) return false; v->set_tool(Tool::AddPad); return true;
    case ImGuiKey_M: v->set_tool(Tool::Measure); return true;
    case ImGuiKey_B: v->auto_body(); return true;
    case ImGuiKey_T: v->set_tool(Tool::DrawText); return true;
    default: return false;
    }
}

ImGuiMouseCursor PkgCanvas::cursor() const {
    switch (v->tool_) {
    case Tool::Select:
        if (v->hover_ >= 0) return v->psel_.count(v->hover_) ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand;
        if (v->ghover_ >= 0) return v->gsel_.count(v->ghover_) ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand;
        return ImGuiMouseCursor_Arrow;
    case Tool::SetPin1: case Tool::PinOrder: return v->hover_ >= 0 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_NotAllowed;
    case Tool::DrawText: return ImGuiMouseCursor_TextInput;
    default: return ImGuiMouseCursor_Arrow;
    }
}

// nice step (1-2-5) in display units, returns display-unit step
double nice_step(double min_units) {
    double e = std::pow(10.0, std::floor(std::log10(std::max(min_units, 1e-9))));
    for (double m : {1.0, 2.0, 5.0, 10.0}) if (m * e >= min_units) return m * e;
    return 10 * e;
}
std::string fmt_tick(double v, double step) {
    char b[32];
    int dec = step >= 1 ? 0 : (int)std::ceil(-std::log10(step) - 1e-9);
    snprintf(b, sizeof b, "%.*f", std::clamp(dec, 0, 4), std::fabs(v) < step * 1e-6 ? 0.0 : v);
    return b;
}

void PkgCanvas::paint_overlay(ImDrawList* dl) {
    const Package* p = v->shown();
    const GTheme& t = th();
    const ImVec2 o = origin(), sz = size();
    if (!p) return;
    const double s = scale_;
    const Units u = disp_units();
    // ---- R15 polarity marker (package data; drawn by the user / placed by default near lead 1)
    if (pkg_has_polarity(*p)) {
        const Vec2 at = p->marker_pos + (drag_primary == -3 ? drag_d : Vec2{});
        const uint32_t c = p->marker_color ? p->marker_color : 0xFFE03131;
        const ImU32 col = IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, 255);
        const float r = std::max(S(4), (float)(v->marker_size_mm(*p) * s));
        const ImVec2 cc = w2s(at);
        if (p->marker_kind == (uint8_t)MarkerKind::Arrow) {
            const double a = deg2rad(p->marker_rot);
            const ImVec2 d((float)std::cos(a), -(float)std::sin(a)), n(-d.y, d.x);
            const ImVec2 tip = cc, b0 = cc - d * (r * 1.4f), tail = cc - d * (r * 3.0f);
            dl->AddTriangleFilled(tip, b0 + n * (r * 0.9f), b0 - n * (r * 0.9f), col);
            dl->AddLine(tail, cc - d * r, col, std::max(S(2), r * 0.35f));
        } else dl->AddCircleFilled(cc, r, col, 24);
        if (v->msel_) dl->AddCircle(cc, r * (p->marker_kind == (uint8_t)MarkerKind::Arrow ? 3.2f : 1.0f) + S(4), t.accent, 32, S(2));
    }
    // ---- pin numbers
    const int p1 = pin1_index(*p);
    for (size_t i = 0; v->vis_pins_ && i < p->pads.size(); ++i) {
        const Pad& pd = p->pads[i];
        Vec2 pos = pd.pos + (v->psel_.count((int)i) ? drag_d : Vec2{});
        BBox bb = pad_bbox(pd);
        float pw = (float)(bb.w() * s), ph = (float)(bb.h() * s);
        float m = std::min(pw, ph);
        const int shown_pin = v->pord_.active ? (v->pord_.done[i] ? v->pord_.work[i] : 0) : pd.pin;
        if (m < S(9) || shown_pin <= 0) continue;
        char b[16];
        snprintf(b, sizeof b, "%d", shown_pin);
        float fs = std::clamp(m * 0.55f, S(9), S(18));
        ImVec2 ts = g_font_bold->CalcTextSizeA(fs, FLT_MAX, 0, b);
        if (ts.x > pw * 0.95f && ts.x > ph * 0.95f) fs *= std::max(0.6f, std::max(pw, ph) * 0.9f / ts.x), ts = g_font_bold->CalcTextSizeA(fs, FLT_MAX, 0, b);
        ImVec2 c = w2s(pos);
        uint32_t pc = pd.drill > 0 ? ui::pal().pad_th : ui::pal().pad_smt;
        float lum = 0.299f * ((pc >> 16) & 255) + 0.587f * ((pc >> 8) & 255) + 0.114f * (pc & 255);
        ImU32 tc = lum > 140 ? IM_COL32(20, 20, 24, 235) : IM_COL32(255, 255, 255, 240);
        if (pd.drill > 0 && pd.drill * s > ts.x) tc = t.text;
        dl->AddText(g_font_bold, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), tc, b);
    }
    // ---- pin 1 badge (outside the pad, away from the centroid)
    if (p1 >= 0) {
        const Pad& pd = p->pads[p1];
        Vec2 pos = pd.pos + (v->psel_.count(p1) ? drag_d : Vec2{});
        Vec2 dir = pos.len() > 1e-6 ? pos * (1.0 / pos.len()) : Vec2{-1, 1} * (1 / std::sqrt(2.0));
        BBox bb = pad_bbox(pd);
        double r = std::max(bb.w(), bb.h()) * 0.5;
        ImVec2 c = w2s(pos + dir * r);
        ImVec2 off((float)dir.x * S(12), -(float)dir.y * S(12));
        ImVec2 bc = c + off;
        dl->AddCircleFilled(bc, S(8), t.warn);
        ImVec2 ts = g_font_bold->CalcTextSizeA(S(11), FLT_MAX, 0, "1");
        dl->AddText(g_font_bold, S(11), ImVec2(bc.x - ts.x * 0.5f, bc.y - ts.y * 0.5f), IM_COL32(20, 20, 20, 255), "1");
    }
    // ---- hover outline
    if (v->hover_ >= 0 && v->hover_ < (int)p->pads.size()) {
        const Pad& pd = p->pads[v->hover_];
        const double hw = pd.w * 0.5, hh = pd.h * 0.5;
        Vec2 q[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
        ImVec2 s4[4];
        for (int k = 0; k < 4; ++k) s4[k] = w2s(rotate(q[k], pd.rot) + pd.pos);
        dl->AddPolyline(s4, 4, t.accent, ImDrawFlags_Closed, S(2));
        // tooltip-ish chip
        char b[96];
        snprintf(b, sizeof b, "Pin %d  %s x %s", pd.pin, fmt_len_u8(pd.w, false).c_str(), fmt_len_u8(pd.h, false).c_str());
        ImVec2 mp = ImGui::GetIO().MousePos + ImVec2(S(14), S(14));
        ImVec2 ts = ImGui::CalcTextSize(b);
        dl->AddRectFilled(mp, mp + ts + ImVec2(S(12), S(6)), with_alpha(t.panel, 0xEE), S(5));
        dl->AddRect(mp, mp + ts + ImVec2(S(12), S(6)), t.border, S(5));
        dl->AddText(mp + ImVec2(S(6), S(3)), t.text, b);
    }
    // ---- dimension lines for the pad field
    const PadStats& st = v->stats();
    if (v->vis_dims_ && !st.field.empty() && p->pads.size() >= 2) {   // P2-02: off by default (viewport controls)
        auto dim = [&](ImVec2 a, ImVec2 b, const std::string& label, bool vertical) {
            ImU32 c = with_alpha(t.text_dim, 0xD0);
            dl->AddLine(a, b, c, 1.f);
            float k = S(5);
            if (!vertical) {
                dl->AddLine(a + ImVec2(0, -k), a + ImVec2(0, k), c); dl->AddLine(b + ImVec2(0, -k), b + ImVec2(0, k), c);
                dl->AddTriangleFilled(a, a + ImVec2(k * 1.4f, -k * 0.6f), a + ImVec2(k * 1.4f, k * 0.6f), c);
                dl->AddTriangleFilled(b, b + ImVec2(-k * 1.4f, -k * 0.6f), b + ImVec2(-k * 1.4f, k * 0.6f), c);
            } else {
                dl->AddLine(a + ImVec2(-k, 0), a + ImVec2(k, 0), c); dl->AddLine(b + ImVec2(-k, 0), b + ImVec2(k, 0), c);
                dl->AddTriangleFilled(a, a + ImVec2(-k * 0.6f, k * 1.4f), a + ImVec2(k * 0.6f, k * 1.4f), c);
                dl->AddTriangleFilled(b, b + ImVec2(-k * 0.6f, -k * 1.4f), b + ImVec2(k * 0.6f, -k * 1.4f), c);
            }
            ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            ImVec2 m = (a + b) * 0.5f;
            ImVec2 tp = vertical ? ImVec2(m.x + S(6), m.y - ts.y * 0.5f) : ImVec2(m.x - ts.x * 0.5f, m.y + S(4));
            dl->AddRectFilled(tp - ImVec2(S(4), S(1)), tp + ts + ImVec2(S(4), S(1)), with_alpha(t.panel, 0xD8), S(4));
            dl->AddText(tp, t.text_dim, label.c_str());
        };
        ImVec2 a = w2s({st.field.x0, st.field.y0}), b = w2s({st.field.x1, st.field.y0});
        float y = a.y + S(24);
        if (b.x - a.x > S(40)) dim(ImVec2(a.x, y), ImVec2(b.x, y), fmt_len_u8(st.field.w()), false);
        ImVec2 c = w2s({st.field.x1, st.field.y1}), d = w2s({st.field.x1, st.field.y0});
        float x = c.x + S(24);
        if (d.y - c.y > S(40)) dim(ImVec2(x, c.y), ImVec2(x, d.y), fmt_len_u8(st.field.h()), true);
    }
    // ---- add-pad ghost
    if (v->tool_ == Tool::AddPad && hovered()) {
        Pad g;
        if (!v->psel_.empty() && *v->psel_.begin() < (int)p->pads.size()) g = p->pads[*v->psel_.begin()];
        else if (!p->pads.empty()) g = p->pads.back();
        g.pos = snap_pt(mouse_world());
        BBox bb = pad_bbox(g);
        ImVec2 a = w2s({bb.x0, bb.y1}), b = w2s({bb.x1, bb.y0});
        dl->AddRectFilled(a, b, with_alpha(t.accent, 0x50), g.shape == PadShape::Round ? (b.x - a.x) * 0.5f : 2.f);
        dl->AddRect(a, b, t.accent, g.shape == PadShape::Round ? (b.x - a.x) * 0.5f : 2.f, 0, 1.5f);
        char lb[24]; snprintf(lb, sizeof lb, "+%d", max_pin(*p) + 1);
        dl->AddText(b + ImVec2(S(4), 0), t.accent, lb);
    }
    if (v->tool_ == Tool::PinOrder && hovered()) {
        char lb[64]; snprintf(lb, sizeof lb, "pin %d  (Enter = apply, Esc = cancel)", v->pord_.next);
        ImVec2 mp = ImGui::GetIO().MousePos + ImVec2(S(14), S(-6));
        ImVec2 ts = ImGui::CalcTextSize(lb);
        dl->AddRectFilled(mp - ImVec2(S(5), S(2)), mp + ts + ImVec2(S(5), S(2)), t.accent, S(8));
        dl->AddText(mp, t.accent_text, lb);
    }
    // ---- measure
    if (has_measure) {
        ImVec2 a = w2s(m0), b = w2s(m1);
        dl->AddLine(a, b, t.accent, S(2));
        dl->AddCircleFilled(a, S(3.5f), t.accent);
        dl->AddCircleFilled(b, S(3.5f), t.accent);
        Vec2 d = m1 - m0;
        std::string lb = "d " + fmt_len_u8(d.len()) + "   dx " + fmt_len_u8(std::fabs(d.x), false) + "   dy " + fmt_len_u8(std::fabs(d.y), false);
        ImVec2 ts = ImGui::CalcTextSize(lb.c_str());
        ImVec2 m = (a + b) * 0.5f + ImVec2(S(10), -ts.y - S(10));
        dl->AddRectFilled(m - ImVec2(S(6), S(3)), m + ts + ImVec2(S(6), S(3)), t.accent, S(6));
        dl->AddText(m, t.accent_text, lb.c_str());
    }
    // ---- rulers (top / left), display units relative to the package origin
    {
        const float rh = S(20);
        const double upmm = to_display(1.0, u);              // display units per mm
        const double pxu = s / upmm;                           // px per display unit
        const double step = nice_step(S(70) / pxu);
        const ImU32 rb = with_alpha(t.panel, 0xEC), tc = t.text_dim;
        dl->AddRectFilled(o, ImVec2(o.x + sz.x, o.y + rh), rb);
        dl->AddRectFilled(ImVec2(o.x, o.y + rh), ImVec2(o.x + rh, o.y + sz.y), rb);
        dl->AddLine(ImVec2(o.x, o.y + rh), ImVec2(o.x + sz.x, o.y + rh), t.border);
        dl->AddLine(ImVec2(o.x + rh, o.y + rh), ImVec2(o.x + rh, o.y + sz.y), t.border);
        Vec2 wa = s2w(o), wb = s2w(o + sz);
        double ua = to_display(std::min(wa.x, wb.x), u), ub = to_display(std::max(wa.x, wb.x), u);
        for (double x = std::floor(ua / step) * step; x <= ub; x += step / 5) {
            bool major = std::fabs(std::remainder(x, step)) < step * 1e-3;
            float sx = w2s({from_display(x, u), 0}).x;
            if (sx < o.x + rh) continue;
            dl->AddLine(ImVec2(sx, o.y + rh - (major ? S(8) : S(4))), ImVec2(sx, o.y + rh), tc);
            if (major) dl->AddText(g_font, S(11), ImVec2(sx + S(3), o.y + S(2)), tc, fmt_tick(x, step).c_str());
        }
        double va = to_display(std::min(wa.y, wb.y), u), vb = to_display(std::max(wa.y, wb.y), u);
        for (double y = std::floor(va / step) * step; y <= vb; y += step / 5) {
            bool major = std::fabs(std::remainder(y, step)) < step * 1e-3;
            float sy = w2s({0, from_display(y, u)}).y;
            if (sy < o.y + rh) continue;
            dl->AddLine(ImVec2(o.x + rh - (major ? S(8) : S(4)), sy), ImVec2(o.x + rh, sy), tc);
            if (major) {
                std::string tx = fmt_tick(y, step);
                ImVec2 ts = g_font->CalcTextSizeA(S(10), FLT_MAX, 0, tx.c_str());
                if (ts.x < rh - S(2)) dl->AddText(g_font, S(10), ImVec2(o.x + S(2), sy + S(1)), tc, tx.c_str());
            }
        }
        if (hovered()) {
            ImVec2 mp = ImGui::GetIO().MousePos;
            dl->AddLine(ImVec2(mp.x, o.y), ImVec2(mp.x, o.y + rh), t.accent, 1.5f);
            dl->AddLine(ImVec2(o.x, mp.y), ImVec2(o.x + rh, mp.y), t.accent, 1.5f);
        }
        dl->AddRectFilled(o, ImVec2(o.x + rh, o.y + rh), t.panel2);
        const char* us = u == Units::MM ? "mm" : u == Units::Mil ? "mil" : "in";
        ImVec2 uts = g_font->CalcTextSizeA(S(10), FLT_MAX, 0, us);
        dl->AddText(g_font, S(10), ImVec2(o.x + (rh - uts.x) * 0.5f, o.y + (rh - uts.y) * 0.5f), tc, us);
    }
    // ---- body text primitives (D2D painter skips text)
    for (size_t i = 0; i < p->body.size(); ++i) {
        const GPrim& g = p->body[i];
        if (g.kind != GKind::Text || g.pts.empty() || g.text.empty()) continue;
        const bool sel = v->mode_ == 0 && v->gsel_.count((int)i);
        float fs = (float)((g.text_h > 0 ? g.text_h : 1.0) * s * 1.35);
        if (fs < 3) continue;
        ImVec2 a = w2s(g.pts[0] + (sel ? drag_d : Vec2{}));
        ImVec2 ts = g_font->CalcTextSizeA(fs, FLT_MAX, 0, g.text.c_str());
        dl->AddText(g_font, fs, ImVec2(a.x, a.y - ts.y), sel ? im_argb(ui::pal().selection) : im_argb(ui::pal().comp_body), g.text.c_str());
    }
    // ---- hovered graphic
    if (v->ghover_ >= 0 && v->ghover_ < (int)p->body.size() && v->tool_ == Tool::Select && v->hover_ < 0) {
        BBox gb = gprim_bbox(p->body[v->ghover_]);
        if (!gb.empty()) dl->AddRect(w2s({gb.x0, gb.y1}) - ImVec2(S(3), S(3)), w2s({gb.x1, gb.y0}) + ImVec2(S(3), S(3)), with_alpha(t.accent, 0xA0), S(3), 0, 1.f);
    }
    // ---- drawing previews
    if (v->mode_ == 0 && (drawing || !poly.empty() || (is_draw_tool(v->tool_) && hovered()))) {
        const ImU32 c = t.accent;
        const float lw = std::max(1.5f, (float)(v->stroke_ * s));
        Vec2 mw = snap_pt(mouse_world());
        if (drawing) {
            switch (v->tool_) {
            case Tool::DrawLine: dl->AddLine(w2s(d0), w2s(d1), c, lw); break;
            case Tool::DrawRect: dl->AddRect(w2s({std::min(d0.x, d1.x), std::max(d0.y, d1.y)}), w2s({std::max(d0.x, d1.x), std::min(d0.y, d1.y)}), c, 0, 0, lw); break;
            case Tool::DrawCircle: dl->AddCircle(w2s(d0), (float)(dist(d0, d1) * s), c, 0, lw); break;
            case Tool::DrawArc: {
                double a0 = std::atan2(d1.y - d0.y, d1.x - d0.x);
                dl->PathArcTo(w2s(d0), (float)(dist(d0, d1) * s), (float)-a0, (float)-(a0 + deg2rad(v->arc_sweep_)), 48);
                dl->PathStroke(c, 0, lw);
                dl->AddCircle(w2s(d0), S(3), c);
            } break;
            default: break;
            }
            Vec2 d = d1 - d0;
            std::string lb = v->tool_ == Tool::DrawCircle || v->tool_ == Tool::DrawArc ? "r " + fmt_len_u8(d.len()) : fmt_len_u8(std::fabs(d.x), false) + " x " + fmt_len_u8(std::fabs(d.y));
            dl->AddText(w2s(d1) + ImVec2(S(10), S(6)), c, lb.c_str());
        }
        if (!poly.empty()) {
            for (size_t i = 1; i < poly.size(); ++i) dl->AddLine(w2s(poly[i - 1]), w2s(poly[i]), c, lw);
            dl->AddLine(w2s(poly.back()), w2s(mw), with_alpha(c, 0x90), lw);
            for (auto& q : poly) dl->AddRectFilled(w2s(q) - ImVec2(S(3), S(3)), w2s(q) + ImVec2(S(3), S(3)), c);
            if (poly.size() >= 3) dl->AddCircle(w2s(poly.front()), S(7), c, 0, 1.5f);
        }
        if (hovered() && !drawing) {
            ImVec2 m = w2s(mw);
            dl->AddLine(m - ImVec2(S(7), 0), m + ImVec2(S(7), 0), c, 1.2f);
            dl->AddLine(m - ImVec2(0, S(7)), m + ImVec2(0, S(7)), c, 1.2f);
        }
    }
    if (v->tool_ == Tool::SetOrigin && hovered()) {
        int k = v->hit_pad(mouse_world(), 6);
        Vec2 at = k >= 0 ? p->pads[k].pos : snap_pt(mouse_world());
        ImVec2 m = w2s(at);
        dl->AddLine(m - ImVec2(S(14), 0), m + ImVec2(S(14), 0), t.warn, 1.5f);
        dl->AddLine(m - ImVec2(0, S(14)), m + ImVec2(0, S(14)), t.warn, 1.5f);
        dl->AddCircle(m, S(6), t.warn, 0, 1.5f);
        std::string lb = "new origin " + fmt_len_u8(at.x, false) + ", " + fmt_len_u8(at.y);
        dl->AddText(m + ImVec2(S(10), S(6)), t.warn, lb.c_str());
    }
    // ---- HUD card (bottom-left): hidden by default (P2-01), toggled from the viewport controls
    if (v->vis_stats_) {
        std::vector<std::pair<std::string, std::string>> rows;
        rows.push_back({"Pads", std::to_string(p->pads.size())});
        if (st.pitch > 0) rows.push_back({"Pitch", fmt_len_u8(st.pitch)});
        if (!st.field.empty()) rows.push_back({"Pad field", fmt_len_u8(st.field.w(), false) + " x " + fmt_len_u8(st.field.h())});
        if (!st.body.empty()) rows.push_back({"Body", fmt_len_u8(st.body.w(), false) + " x " + fmt_len_u8(st.body.h())});
        if (!v->psel_.empty()) rows.push_back({"Selected", std::to_string(v->psel_.size()) + (drag_d.len() > 0 ? "  moving " + fmt_pos_u8(*v->doc, drag_d + v->doc->prj.virtual_origin) : "")});
        float lw = 0, vw = 0, lh = ImGui::GetTextLineHeight();
        for (auto& r : rows) { lw = std::max(lw, ImGui::CalcTextSize(r.first.c_str()).x); vw = std::max(vw, ImGui::CalcTextSize(r.second.c_str()).x); }
        ImVec2 sz2(lw + vw + S(34), rows.size() * (lh + S(3)) + S(14));
        ImVec2 a(o.x + S(32), o.y + sz.y - sz2.y - S(12));
        dl->AddRectFilled(a, a + sz2, with_alpha(t.panel, 0xE6), S(8));
        dl->AddRect(a, a + sz2, t.border, S(8));
        float y = a.y + S(7);
        for (auto& r : rows) {
            dl->AddText(ImVec2(a.x + S(10), y), t.text_dim, r.first.c_str());
            dl->AddText(ImVec2(a.x + S(22) + lw, y), t.text, r.second.c_str());
            y += lh + S(3);
        }
    }
}

// ---------------------------------------------------------------------------
// view: data sync
// ---------------------------------------------------------------------------
const PadStats& PackagesView::stats() {
    const Package* p = shown();
    if (stats_.gen != doc->gen || stats_.pkg != (const void*)p) {
        stats_ = {};
        stats_.gen = doc->gen;
        stats_.pkg = p;
        if (p) {
            stats_.pitch = pad_pitch(*p);
            for (auto& d : p->pads) stats_.field.add(pad_bbox(d));
            stats_.body = gprims_bbox(p->body);
        }
    }
    return stats_;
}

void PackagesView::sync() {
    if (!g_focus_package.empty()) {
        std::string f = g_focus_package;
        g_focus_package.clear();
        if (doc->prj.packages.count(f)) { mode_ = 0; select_package(f); scroll_to_cur_ = true; }
    }
    if (seen_gen_ != doc->gen) {
        // R16b (REVIEW_V15 §1): every package name used by a placement is listed, even without graphics
        if (ensure_package_entries(doc->prj)) { doc->prj.dirty = true; doc->changed(ui::CH_GEOMETRY); }
        seen_gen_ = doc->gen;
        use_comps_.clear();
        use_parts_.clear();
        for (auto& c : doc->prj.comps) if (!c.package.empty()) use_comps_[c.package]++;
        for (auto& kv : doc->prj.parts) if (!kv.second.package.empty()) use_parts_[kv.second.package]++;
        if (!cur_.empty() && !doc->prj.packages.count(cur_)) cur_.clear();
        for (auto it = lsel_.begin(); it != lsel_.end();) it = doc->prj.packages.count(*it) ? std::next(it) : lsel_.erase(it);
        auto cp = doc->prj.packages.find(cur_);
        if (cp != doc->prj.packages.end()) {
            for (auto it = psel_.begin(); it != psel_.end();) it = *it < (int)cp->second.pads.size() ? std::next(it) : psel_.erase(it);
            for (auto it = gsel_.begin(); it != gsel_.end();) it = *it < (int)cp->second.body.size() ? std::next(it) : gsel_.erase(it);
        } else { psel_.clear(); gsel_.clear(); }
        canvas_.invalidate();
    }
    if (cur_.empty() && !doc->prj.packages.empty()) {
        rebuild_rows();
        select_package(rows_.empty() ? doc->prj.packages.begin()->first : rows_.front());
    }
}

void PackagesView::select_package(const std::string& name) {
    if (name == cur_ && lsel_.count(name)) return;
    bool changed = name != cur_;
    cur_ = name;
    lsel_ = {name};
    anchor_ = name;
    if (changed) {
        psel_.clear();
        gsel_.clear();
        canvas_.cancel_draw();
        hover_ = ghover_ = -1;
        canvas_.has_measure = false;
        pin_seq_ = 1;
        if (tool_ == Tool::PinOrder) tool_ = Tool::Select;
        canvas_.zoom_all();
    }
    canvas_.invalidate();
}

void PackagesView::rebuild_rows() {
    std::string key = std::string(search_) + "|" + std::to_string(sort_) + std::to_string(sort_desc_) + "|" + std::to_string(filt_);
    if (rows_gen_ == doc->gen && key == rows_key_) return;
    rows_gen_ = doc->gen;
    rows_key_ = key;
    rows_.clear();
    const std::string q = lower(trim(search_));
    for (auto& kv : doc->prj.packages) {
        const Package& p = kv.second;
        if (!q.empty() && lower(kv.first).find(q) == std::string::npos && lower(p.description).find(q) == std::string::npos) continue;
        auto it = use_comps_.find(kv.first);
        const int used = it == use_comps_.end() ? 0 : it->second;
        const bool th_ = is_th(p);
        if ((filt_ & 1) && !used) continue;
        if ((filt_ & 2) && used) continue;
        if ((filt_ & 4) && th_) continue;
        if ((filt_ & 8) && !th_) continue;
        if ((filt_ & 16) && lower(p.description).find("reconstruct") == std::string::npos) continue;
        if ((filt_ & 32) && !p.pads.empty()) continue;
        if ((filt_ & 256) && (package_resolved(p) || !used)) continue;   // R16b Unresolved
        if (filt_ & (64 | 128)) {
            auto ps = psync_.find(kv.first);
            SyncState ss = ps == psync_.end() ? SyncState::LocalOnly : ps->second;
            if ((filt_ & 64) && ss != SyncState::LocalOnly) continue;
            if ((filt_ & 128) && ss != SyncState::Differs) continue;
        }
        rows_.push_back(kv.first);
    }
    auto usage = [&](const std::string& n) { auto it = use_comps_.find(n); return it == use_comps_.end() ? 0 : it->second; };
    auto npads = [&](const std::string& n) { return (int)doc->prj.packages[n].pads.size(); };
    std::stable_sort(rows_.begin(), rows_.end(), [&](const std::string& a, const std::string& b) {
        int c = 0;
        if (sort_ == 1) c = usage(b) - usage(a);
        else if (sort_ == 2) c = npads(a) - npads(b);
        if (c == 0) return sort_desc_ ? ref_less(b, a) : ref_less(a, b);
        return sort_desc_ ? c > 0 : c < 0;
    });
}

std::vector<std::string> PackagesView::lib_targets() const {
    std::vector<std::string> v(lsel_.begin(), lsel_.end());
    if (v.empty() && !cur_.empty()) v.push_back(cur_);
    return v;
}

// ---------------------------------------------------------------------------
// thumbnails (D2D into cached RenderTextures)
// ---------------------------------------------------------------------------
void* PackagesView::thumb(const std::string& key, const Package& pkref, int px, int& budget) {
    auto& slot = thumbs_[key];
    if (!slot) slot = std::make_unique<Thumb>();
    Thumb& t = *slot;
    t.last = frame_;
    const bool dark = th().dark;
    // hash only when the document moved on (cheap otherwise)
    const bool stale = !t.tex.valid() || t.dark != dark || t.px != px || t.hash != pkg_hash(pkref);
    if (stale && budget > 0) {
        --budget;
        t.tex.ensure(px, px);
        if (ID2D1DeviceContext* rt = t.tex.begin(1.0f)) {
            const ImU32 bg = th().panel2;
            rt->Clear(d2_im(bg | 0xFF000000));
            const Package& p = pkref;
            BBox e = p.extent();
            const float m = px * 0.12f;
            double sc = std::min((px - 2 * m) / std::max(e.w(), 0.2), (px - 2 * m) / std::max(e.h(), 0.2));
            Vec2 c = e.center();
            rt->SetTransform(D2D1::Matrix3x2F((float)sc, 0, 0, (float)-sc, (float)(px * 0.5 - c.x * sc), (float)(px * 0.5 + c.y * sc)));
            PaintOpts o;
            o.hair = (float)(1.0 / sc);
            o.bg = bg;
            o.pin1 = p.pads.size() > 1;
            paint_package(rt, p, o);
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
            t.tex.end();
        }
        t.hash = pkg_hash(pkref);
        t.dark = dark;
        t.px = px;
    } else if (stale) {
        request_redraw(2);
    }
    return t.tex.valid() ? t.tex.imgui_id() : nullptr;
}

// ---------------------------------------------------------------------------
// edits
// ---------------------------------------------------------------------------
void PackagesView::add_pad(Vec2 at) {
    Package* p = pkg();
    if (!p) return;
    Pad pd;
    if (!psel_.empty() && *psel_.begin() < (int)p->pads.size()) pd = p->pads[*psel_.begin()];
    else if (!p->pads.empty()) pd = p->pads.back();
    else if (!p->smt) { pd.shape = PadShape::Round; pd.w = pd.h = 1.6; pd.drill = 0.9; }
    pd.pos = at;
    pd.pin = max_pin(*p) + 1;
    doc->checkpoint("Add pad");
    p->pads.push_back(pd);
    p->leads = (int)p->pads.size();
    psel_ = {(int)p->pads.size() - 1};
    edited();
}

void PackagesView::delete_pads() {
    Package* p = pkg();
    if (!p || psel_.empty()) return;
    doc->checkpoint("Delete pads");
    // Pin numbers are stored per lead (Pad::pin) and are NEVER renumbered by a delete: the
    // remaining leads keep their numbers (gaps are allowed). Only an explicit Renumber renumbers.
    bool had_pin1 = false;
    for (int i : psel_) if (i < (int)p->pads.size() && p->pads[i].pin == 1) had_pin1 = true;
    for (auto it = psel_.rbegin(); it != psel_.rend(); ++it) if (*it < (int)p->pads.size()) p->pads.erase(p->pads.begin() + *it);
    p->leads = (int)p->pads.size();
    if (had_pin1 && !p->pads.empty())
        say(std::to_string(psel_.size()) + " lead(s) deleted, including pin 1. Other leads keep their numbers; pick a lead and press Make Pin 1, or Renumber.", ToastKind::Warning);
    else
        say(std::to_string(psel_.size()) + " lead(s) deleted; the other leads keep their pin numbers (Renumber to close gaps)");
    psel_.clear();
    hover_ = -1;
    edited();
}

void PackagesView::move_pads(Vec2 d, const char* label) {
    Package* p = pkg();
    if (!p || (psel_.empty() && gsel_.empty())) return;
    doc->checkpoint(label);
    for (int i : psel_) if (i < (int)p->pads.size()) p->pads[i].pos += d;
    for (int i : gsel_) if (i < (int)p->body.size()) offset_prim(p->body[i], d);
    edited();
}

void PackagesView::delete_selection() {
    Package* p = pkg();
    if (!p) return;
    if (!gsel_.empty()) {
        doc->checkpoint("Delete graphics");
        for (auto it = gsel_.rbegin(); it != gsel_.rend(); ++it) if (*it < (int)p->body.size()) p->body.erase(p->body.begin() + *it);
        p->body_source.clear();      // user edited

        say(std::to_string(gsel_.size()) + " graphic(s) deleted");
        gsel_.clear();
        ghover_ = -1;
        edited();
    }
    if (!psel_.empty()) delete_pads();
}

void PackagesView::add_prim(GPrim g, const char* label) {
    Package* p = pkg();
    if (!p) return;
    doc->checkpoint(label);
    p->body.push_back(std::move(g));
    p->body_source.clear();          // user drawn
    p->shape_confirmed = true;
    gsel_ = {(int)p->body.size() - 1};
    psel_.clear();
    if (auto_render_) { auto_render_ = false; say("Graphics added; auto-render turned off so flag changes keep your artwork", ToastKind::Info); }
    edited();
}

void PackagesView::add_text_at(Vec2 at) {
    if (!pkg()) return;
    DocPtr d = doc;
    std::string name = cur_;
    defer([this, d, name, at]() {
        std::wstring txt = L"";
        if (!ui::prompt_text(main_hwnd(), L"Add Text", L"Text to place on the package artwork", txt) || txt.empty()) return;
        if (cur_ != name || !pkg()) return;
        GPrim g; g.kind = GKind::Text; g.pts = {at}; g.text = ui::U(txt); g.text_h = text_h_; g.width = stroke_;
        add_prim(g, "Add text");
    });
}

void PackagesView::toggle_flag(bool Package::*m, const char* label) {
    Package* p = pkg();
    if (!p) return;
    doc->checkpoint(label);
    p->*m = !(p->*m);
    if (auto_render_ && !render_body(*p)) say("Body outline kept (" + std::string(io::body_source_label(p->body_source)) + "): flags never replace a real outline", ToastKind::Info);
    edited();
}

void PackagesView::rotate_pads() {
    Package* p = pkg();
    if (!p || psel_.empty()) return;
    doc->checkpoint("Rotate pads");
    for (int i : psel_) if (i < (int)p->pads.size()) p->pads[i].rot = norm_deg(p->pads[i].rot + 90);
    edited();
}

// ---- gated template edits (core/package_edit) --------------------------------
// Every transform / numbering edit of a project package goes through here: when the
// package is used by placements the user confirms "This changes N placements of PKG
// (shared)" first; the edit itself is done by package_edit (compensation, counts).
// R13: confirmed shared packages (asked once per package per session); shared by gated() and multi_edit()
static std::set<std::string>& shared_confirmed() { static std::set<std::string> s; return s; }

void PackagesView::gated(const std::string& label, std::function<PkgEditResult(Project&, const PkgEditOpts&)> fn) {
    if (!pkg()) return;
    DocPtr d = doc;
    const std::string name = cur_;
    auto run = [this, d, label, fn]() {
        d->checkpoint(label);
        PkgEditOpts o; o.confirmed = true;
        PkgEditResult r = fn(d->prj, o);
        if (!r.applied) { say(r.message.empty() ? std::string("Nothing changed") : r.message, ToastKind::Info); return; }
        d->changed(ui::CH_GEOMETRY);
        canvas_.invalidate();
        say(r.message);
    };
    const std::string w = shared_warning(d->prj, name);
    if (w.empty()) { run(); return; }
    // R13 (§7): ask once per package per session (and never again after "Don't show this message
    // again", which persists in the [UI] settings section as Confirm.pkg.shared_edit)
    auto& confirmed = shared_confirmed();
    if (confirmed.count(name)) { run(); return; }
    defer([run, w, label, name, &confirmed]() {
        if (!confirm_info("pkg.shared_edit", label.c_str(), w + ".\n\nThe package is a shared template: every placement of it is affected (undoable).\nPlacement-only changes (rotate one part, Assign Pin One) belong in the Plotter.\n\nYou will not be asked again for this package until PCB Plotter restarts.", "Change package"))
            return;
        confirmed.insert(name);
        run();
    });
}

void PackagesView::set_pin1(int i) {
    Package* p = pkg();
    if (!p || i < 0 || i >= (int)p->pads.size()) return;
    if (p->pads[i].pin == 1) { say("That lead is already pin 1", ToastKind::Info); return; }
    // cyclic shift of the existing order: the CCW sequence is kept (2-lead parts: swap)
    const std::vector<int> pins = pins_cyclic_from(*p, i);
    const std::string name = cur_;
    gated("Set pin 1", [name, pins](Project& prj, const PkgEditOpts& o) { return pkg_set_pins(prj, name, pins, o); });
}

void PackagesView::pin_order_click(int i) {
    if (!pkg()) return;
    if (!pord_.active) pord_.begin(*pkg());
    psel_ = {i};
    pin_seq_ = pord_.next;
    if (pord_.click(i)) pin_order_commit();
    canvas_.invalidate();
}
void PackagesView::pin_order_commit() {
    if (!pord_.active || !pkg()) { pord_.cancel(); return; }
    const std::vector<int> pins = pord_.result();
    const bool all = pord_.complete();
    pord_.cancel();
    tool_ = Tool::Select;
    if (pins == pord_.original) { say("Pin order unchanged", ToastKind::Info); return; }
    const std::string name = cur_;
    gated("Pin ordering", [name, pins](Project& prj, const PkgEditOpts& o) { return pkg_set_pins(prj, name, pins, o); });
    if (!all) hint("Unclicked leads were numbered after the clicked ones in their previous order");
}
void PackagesView::pin_order_cancel() {
    pord_.cancel();         // nothing was written: the original numbering stands
    tool_ = Tool::Select;
    canvas_.invalidate();
    say("Pin ordering cancelled; original numbering kept", ToastKind::Info);
}

void PackagesView::rotate_by(double deg, bool comp) {
    // The "0 deg" accumulator only advances when the rotation is actually applied: a cancelled
    // shared-package confirmation (or a no-op) must not drift it.
    const std::string name = cur_;
    gated(comp ? "Rotate package (placements compensated)" : "Rotate package (placements kept)",
          [this, name, deg, comp](Project& prj, const PkgEditOpts& o0) {
              PkgEditOpts o = o0; o.compensate = comp;
              PkgEditResult r = pkg_rotate(prj, name, deg, o);
              if (r.applied && !name.empty()) { double& a = rot_acc_[name]; a = std::fmod(a + deg + 720.0, 360.0); }
              return r; });
    canvas_.zoom_all();
}

void PackagesView::rotate_package() {
    if (!pkg()) return;
    rotate_by(90, compensate_);
}

bool PackagesView::pkg_hotkeys(bool from_canvas) {
    (void)from_canvas;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.KeyCtrl || io.KeyAlt || mode_ != 0) return false;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) { if (pkg()) rotate_by(io.KeyShift ? -90 : 90, compensate_); return true; }
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) { if (io.KeyShift) cycle_marker_type(); else toggle_polarity(); return true; }
    return false;
}

void PackagesView::step_package(int d) {
    if (rows_.empty()) return;
    const int n = (int)rows_.size();
    const int i = (int)(std::find(rows_.begin(), rows_.end(), cur_) - rows_.begin());
    const int j = std::clamp(i >= n ? (d > 0 ? 0 : n - 1) : i + d, 0, n - 1);
    if (j == i) return;
    lib_click(rows_[j]);
    scroll_to_cur_ = true;
}

// ---- R15 polarity marker (specs/POLARITY.md) ----------------------------------------------------
// Every marker edit is one undo step through the shared-package gate. Nothing here touches the body.
void PackagesView::marker_edit(const char* label, std::function<void(Package&)> f) {
    if (!pkg()) return;
    multi_edit(label, [f](Package& q) { f(q); q.polarity_set = true; });
}

std::vector<std::string> PackagesView::targets() {
    std::vector<std::string> v;
    if (!doc || mode_ != 0 || !pkg()) return v;
    if (lsel_.size() > 1 && lsel_.count(cur_)) {
        for (auto& n : rows_) if (lsel_.count(n) && doc->prj.packages.count(n)) v.push_back(n);
        for (auto& n : lsel_) if (doc->prj.packages.count(n) && std::find(v.begin(), v.end(), n) == v.end()) v.push_back(n);
    } else v.push_back(cur_);
    return v;
}

void PackagesView::multi_edit(const std::string& label, std::function<void(Package&)> f) {
    std::vector<std::string> names = targets();
    if (names.empty()) return;
    {   // dry run: only packages the edit really changes are edited / asked about; none -> no undo step
        Project probe;
        for (auto& n : names) probe.packages[n] = doc->prj.packages[n];
        names = pkg_edit_many(probe, names, f);
        if (names.empty()) return;
    }
    DocPtr d = doc;
    auto run = [this, d, label, names, f]() {
        d->checkpoint(label);
        const auto ch = pkg_edit_many(d->prj, names, f);
        d->changed(ui::CH_GEOMETRY);
        canvas_.invalidate();
        if (targets().size() > 1) say(label + ": " + std::to_string(ch.size()) + " package(s) changed");
    };
    std::vector<std::string> ask;
    for (auto& n : names) if (!shared_confirmed().count(n) && !shared_warning(d->prj, n).empty()) ask.push_back(n);
    if (ask.empty()) { run(); return; }
    const std::string w = shared_warning_many(d->prj, names);
    defer([run, w, label, ask]() {
        if (!confirm_info("pkg.shared_edit", label.c_str(), w + ".\n\nThe packages are shared templates: every placement of them is affected (one undo step).\nPlacement-only changes (rotate one part, Assign Pin One) belong in the Plotter.\n\nYou will not be asked again for these packages until PCB Plotter restarts.", "Change"))
            return;
        for (auto& n : ask) shared_confirmed().insert(n);
        run();
    });
}
// P: add a red dot near lead 1 (first lead when none is numbered 1) / remove the marker
void PackagesView::toggle_polarity() {
    if (!pkg()) return;
    // R18c: all selected. All have a marker -> remove all; otherwise add one to each package without (keeps existing)
    bool all = true;
    for (auto& n : targets()) all = all && pkg_has_polarity(doc->prj.packages[n]);
    if (all) { msel_ = false; marker_edit("Remove polarity marker", [](Package& q) { pkg_remove_polarity_marker(q); }); }
    else { marker_edit("Add polarity marker", [](Package& q) { if (!pkg_has_polarity(q)) pkg_add_polarity_marker(q); }); msel_ = targets().size() == 1; }
}
// Shift+P: Dot <-> Arrow (adds a dot when there is no marker)
void PackagesView::cycle_marker_type() {
    Package* p = pkg();
    if (!p) return;
    if (!pkg_has_polarity(*p)) { toggle_polarity(); return; }
    const bool to_arrow = p->marker_kind != (uint8_t)MarkerKind::Arrow;
    marker_edit(to_arrow ? "Marker type: Arrow" : "Marker type: Dot", [to_arrow](Package& q) { if (pkg_has_polarity(q)) q.marker_kind = (uint8_t)(to_arrow ? MarkerKind::Arrow : MarkerKind::Dot); });
}
void PackagesView::move_marker(Vec2 d, const char* label) {
    Package* p = pkg();
    if (!p || !pkg_has_polarity(*p)) return;
    marker_edit(label, [d](Package& q) { q.marker_user_pos = true; q.marker_pos += d; });
}
double PackagesView::marker_size_mm(const Package& p) const {
    return p.marker_size > 0 ? p.marker_size : pkg_marker_auto(p).size;
}
bool PackagesView::hit_marker(Vec2 w, double scale) {
    const Package* p = shown();
    if (!p || !pkg_has_polarity(*p)) return false;
    const double r = std::max(marker_size_mm(*p) * (p->marker_kind == (uint8_t)MarkerKind::Arrow ? 3.0 : 1.3), 7.0 / std::max(1e-6, scale));
    return dist(w, p->marker_pos) <= r;
}

void PackagesView::open_merge_similar() {
    msim_open_ = true;
    msim_abs_um_ = (float)app::ui_pref("Pkg.TolAbsUm", 50);
    msim_rel_pct_ = (float)app::ui_pref("Pkg.TolRelPct", 5);
    PkgMatchTol t; t.abs_mm = msim_abs_um_ / 1000.0; t.rel = msim_rel_pct_ / 100.0; t.orientation_aware = msim_orient_;
    msim_ = pkg_similar_groups(doc->prj, t);
    msim_choice_.assign(msim_.size(), 1);
}

void PackagesView::draw_merge_similar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(S(560), S(460)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(vp->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Merge Similar Packages###pkgmsim", &msim_open_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) { ImGui::End(); return; }
    ImGui::TextWrapped("Packages with the same pad count, pad shapes and pitch whose pads agree within the tolerance "
                       "(per dimension: the larger of the absolute and relative value). Orientation-aware: packages are only "
                       "grouped in their own 0\xc2\xb0 orientation.");
    bool re = false;
    ImGui::SetNextItemWidth(S(110)); re |= ImGui::DragFloat("Absolute (\xc2\xb5m)", &msim_abs_um_, 1, 0, 500, "%.0f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(S(90)); re |= ImGui::DragFloat("Relative (%)", &msim_rel_pct_, 0.5f, 0, 30, "%.1f");
    ImGui::SameLine(); re |= ImGui::Checkbox("Orientation-aware", &msim_orient_);
    if (re) {
        app::set_ui_pref("Pkg.TolAbsUm", (int)std::lround(msim_abs_um_)); app::set_ui_pref("Pkg.TolRelPct", (int)std::lround(msim_rel_pct_));
        PkgMatchTol t; t.abs_mm = msim_abs_um_ / 1000.0; t.rel = msim_rel_pct_ / 100.0; t.orientation_aware = msim_orient_;
        msim_ = pkg_similar_groups(doc->prj, t);
        msim_choice_.assign(msim_.size(), 1);
    }
    ImGui::Separator();
    if (msim_.empty()) ImGui::TextDisabled("No similar packages at this tolerance.");
    ImGui::BeginChild("##msl", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - S(6)));
    for (size_t i = 0; i < msim_.size(); ++i) {
        ImGui::PushID((int)i);
        const auto& g = msim_[i];
        std::string m;
        for (auto& n : g.merge) m += (m.empty() ? "" : ", ") + n;
        ImGui::SetNextItemWidth(S(150));
        const char* ch[] = {"Leave as is", "Keep separate", "Merge"};
        int c = msim_choice_[i] + 1;
        if (ImGui::Combo("##c", &c, ch, 3)) msim_choice_[i] = c - 1;
        ImGui::SameLine();
        ImGui::TextWrapped("%s  \xe2\x86\x90  %s   (max dev %.3f mm)", g.keep.c_str(), m.c_str(), g.worst_dev);
        ImGui::PopID();
    }
    ImGui::EndChild();
    int nm = 0; for (int c : msim_choice_) if (c >= 0) ++nm;
    if (button("Apply", ui::Icon::Check, true, ImVec2(S(120), 0), nm > 0)) {
        doc->checkpoint("Merge similar packages");
        int moved = 0, merged = 0, kept = 0;
        for (size_t i = 0; i < msim_.size(); ++i) {
            if (msim_choice_[i] == 1) { moved += pkg_merge(doc->prj, msim_[i].keep, msim_[i].merge); merged += (int)msim_[i].merge.size(); }
            else if (msim_choice_[i] == 0) { for (auto& n : msim_[i].merge) pkg_keep_separate(doc->prj, msim_[i].keep, n); ++kept; }
        }
        if (!doc->prj.packages.count(cur_)) cur_.clear();
        doc->changed(ui::CH_GEOMETRY);
        canvas_.invalidate();
        say(std::to_string(merged) + " package(s) merged (" + std::to_string(moved) + " placements re-pointed), " + std::to_string(kept) + " group(s) kept separate");
        msim_open_ = false;
    }
    ImGui::SameLine();
    if (button("Close", ui::Icon::None, false, ImVec2(S(100), 0))) msim_open_ = false;
    ImGui::End();
}

void PackagesView::mirror_package() {
    Package* p = pkg();
    if (!p) return;
    doc->checkpoint("Mirror package");
    for (auto& pd : p->pads) { pd.pos.x = -pd.pos.x; pd.rot = norm_deg(-pd.rot); }
    for (auto& g : p->body) if (g.kind == GKind::Text && !g.pts.empty()) g.pts[0].x -= (g.text_h > 0 ? g.text_h : 1.0) * 0.6 * g.text.size();
    for (auto& g : p->body) mirror_prim(g);
    if (p->marker_kind) { p->marker_pos.x = -p->marker_pos.x; p->marker_rot = norm_deg(180 - p->marker_rot); }   // R15
    edited();
    say("Package mirrored left-right");
}

void PackagesView::renumber(int mode) {
    Package* p = pkg();
    if (!p || p->pads.empty()) return;
    if (mode == 3) { set_tool(Tool::PinOrder); hint("Click the leads in pin order  \xc2\xb7  Enter applies  \xc2\xb7  Esc cancels (original kept)"); return; }
    Package tmp = *p;
    if (mode == 0) renumber_ccw(tmp);
    else renumber_rows(tmp, mode == 2);
    std::vector<int> pins;
    for (auto& d : tmp.pads) pins.push_back(d.pin);
    const std::string name = cur_;
    gated(mode == 0 ? "Renumber leads CCW" : "Renumber leads", [name, pins](Project& prj, const PkgEditOpts& o) { return pkg_set_pins(prj, name, pins, o); });
}

void PackagesView::auto_body() {
    Package* p = pkg();
    if (!p) return;
    if (!pkg_body_replaceable(*p)) {
        // R15: a real outline is replaced only on an explicit, confirmed request (undoable)
        DocPtr d = doc;
        const std::string name = cur_;
        const std::string src = io::body_source_label(p->body_source);
        defer([this, d, name, src]() {
            if (!ui::message(main_hwnd(), L"Render Body", ui::W("This package has a real body outline (" + src + ").\n\nReplace it with a rectangle derived from the pads (approximate)?\nUndo restores the outline."), true)) return;
            if (cur_ != name || !pkg()) return;
            Package* q = pkg();
            d->checkpoint("Render body (replace outline)");
            q->body.clear(); q->body_source = "pads";
            render_body(*q);
            edited();
            say("Body replaced by a derived suggestion (approximate). Ctrl+Z restores the outline.");
        });
        return;
    }
    doc->checkpoint("Render body");
    render_body(*p);
    p->shape_confirmed = true;
    edited();
    say("Body suggested from the pads (approximate)");
}

// Move the package origin to `at` (package coordinates). With compensation,
// every placement using the package keeps its pads where they are on the board.
void PackagesView::set_origin(OriginMode m, Vec2 at) {
    Package* p = pkg();
    if (!p) return;
    const char* what = "clicked point";
    switch (m) {
    case OriginMode::Click: break;
    case OriginMode::PadCentre: { if (p->pads.empty()) return; BBox b; for (auto& pd : p->pads) b.add(pad_bbox(pd)); at = b.center(); what = "pad extent centre"; } break;
    case OriginMode::LeadCentroid: {
        if (p->pads.empty()) return;
        Vec2 s{}; for (auto& pd : p->pads) s += pd.pos;
        at = s * (1.0 / p->pads.size()); what = "lead centroid";
    } break;
    case OriginMode::Pin1: { int i = pin1_index(*p); if (i < 0) return; at = p->pads[i].pos; what = "pin 1"; } break;
    case OriginMode::BodyCentre: {
        BBox b = gprims_bbox(p->body);
        if (b.empty()) { say("Package has no body graphics", ToastKind::Warning); return; }
        at = b.center(); what = "body centre";
    } break;
    }
    const Vec2 d = at;
    if (std::fabs(d.x) < 1e-7 && std::fabs(d.y) < 1e-7) { say(std::string("Origin is already at the ") + what, ToastKind::Info); return; }
    const std::string name = cur_;
    const bool comp = compensate_;
    gated(std::string("Set package origin (") + what + ")",
          [name, d, comp](Project& prj, const PkgEditOpts& o0) { PkgEditOpts o = o0; o.compensate = comp; return pkg_set_origin(prj, name, d, o); });
    canvas_.zoom_all();
    if (tool_ == Tool::SetOrigin) set_tool(Tool::Select);
}

std::string PackagesView::fmt_n(double v) const {
    char b[48];
    snprintf(b, sizeof b, "%.*f", prec_, to_display(v, disp_units()));
    return b;
}

bool PackagesView::len_input(const char* id, double& v, double minv) {
    char b[48];
    snprintf(b, sizeof b, "%s", fmt_n(v).c_str());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText(id, b, sizeof b);
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    double nv = app::parse_len(ui::W(b), v);
    nv = std::max(minv, nv);
    if (std::fabs(nv - v) < 1e-12) return false;
    v = nv;
    return true;
}

void PackagesView::copy_pads() {
    Package* p = pkg();
    if (!p || psel_.empty()) return;
    clip_.clear();
    for (int i : psel_) if (i < (int)p->pads.size()) clip_.push_back(p->pads[i]);
    say(std::to_string(clip_.size()) + " pad(s) copied");
}

void PackagesView::paste_pads() {
    Package* p = pkg();
    if (!p || clip_.empty()) return;
    doc->checkpoint("Paste pads");
    double off = canvas_.grid() * 4;
    int pin = max_pin(*p);
    psel_.clear();
    for (Pad q : clip_) { q.pos += Vec2{off, -off}; q.pin = ++pin; p->pads.push_back(q); psel_.insert((int)p->pads.size() - 1); }
    p->leads = (int)p->pads.size();
    edited();
}

void PackagesView::pad_array() {
    Package* p = pkg();
    if (!p) return;
    Pad base;
    if (!psel_.empty() && *psel_.begin() < (int)p->pads.size()) base = p->pads[*psel_.rbegin()];
    else if (!p->pads.empty()) base = p->pads.back();
    const Units u = disp_units();
    const std::wstring us = L" (" + app::units_label() + L")";
    auto D = [&](double v) { return to_display(v, u); };
    double pitch = stats().pitch > 0 ? stats().pitch : 1.27;
    DocPtr d = doc;
    std::string name = cur_;
    std::set<int> sel = psel_;
    defer([this, d, name, base, pitch, us, u, D, sel]() {
        ui::Form f(L"Pad Array", 520);
        f.header(L"Generate a row (or two mirrored rows) of pads").icon((int)ui::Icon::PackageEditor).two_columns();
        f.number("n", L"Pad count", 8, 0).number("pitch", L"Pitch" + us, D(pitch), 4);
        f.combo("dir", L"Direction", {L"Right (+X)", L"Left (-X)", L"Up (+Y)", L"Down (-Y)"}, L"Right (+X)");
        bool has = !sel.empty();
        f.number("x", L"Start X" + us, D(has ? base.pos.x + pitch : 0), 4).number("y", L"Start Y" + us, D(has ? base.pos.y : 0), 4);
        f.number("w", L"Pad width" + us, D(base.w), 4).number("h", L"Pad height" + us, D(base.h), 4);
        std::vector<std::wstring> shapes;
        for (auto* s : kShapeNames) shapes.push_back(ui::W(s));
        f.combo("shape", L"Shape", shapes, ui::W(kShapeNames[(int)base.shape]));
        f.number("drill", L"Drill (0 = SMT)" + us, D(base.drill), 4);
        f.number("pin", L"First pin number", 0, 0).tip(L"0 = continue after the highest pin");
        f.check("dual", L"Add mirrored second row", false);
        f.number("span", L"Row spacing" + us, D(5.4), 4).tip(L"Second row is offset perpendicular to the direction; numbered counter-clockwise");
        f.ok_text(L"Create pads");
        if (!f.run(main_hwnd())) return;
        auto it = d->prj.packages.find(name);
        if (it == d->prj.packages.end()) return;
        Package& pk = it->second;
        int n = std::clamp((int)f.num("n"), 1, 2000);
        double pt = from_display(f.num("pitch"), u);
        int dir = std::max(0, f.index("dir"));
        Vec2 step = dir == 0 ? Vec2{pt, 0} : dir == 1 ? Vec2{-pt, 0} : dir == 2 ? Vec2{0, pt} : Vec2{0, -pt};
        Vec2 perp = dir < 2 ? Vec2{0, 1} : Vec2{-1, 0};
        Pad q = base;
        q.w = std::max(0.01, from_display(f.num("w"), u));
        q.h = std::max(0.01, from_display(f.num("h"), u));
        q.shape = (PadShape)std::clamp(f.index("shape"), 0, 4);
        q.drill = std::max(0.0, from_display(f.num("drill"), u));
        int pin = (int)f.num("pin") > 0 ? (int)f.num("pin") : max_pin(pk) + 1;
        Vec2 s0{from_display(f.num("x"), u), from_display(f.num("y"), u)};
        d->checkpoint("Pad array");
        psel_.clear();
        for (int i = 0; i < n; ++i) { q.pos = s0 + step * i; q.pin = pin++; pk.pads.push_back(q); psel_.insert((int)pk.pads.size() - 1); }
        if (f.checked("dual")) {
            Vec2 off = perp * from_display(f.num("span"), u);
            for (int i = n - 1; i >= 0; --i) { q.pos = s0 + step * i + off; q.pin = pin++; pk.pads.push_back(q); psel_.insert((int)pk.pads.size() - 1); }
        }
        pk.leads = (int)pk.pads.size();
        d->changed(ui::CH_GEOMETRY);
        canvas_.zoom_all();
        say(std::to_string(psel_.size()) + " pads created");
    });
}

// ---------------------------------------------------------------------------
// package-level (library) actions
// ---------------------------------------------------------------------------
void PackagesView::act_new() {
    DocPtr d = doc;
    defer([this, d]() {
        std::wstring name = ui::W(unique_name("NEW_PACKAGE"));
        if (!ui::prompt_text(main_hwnd(), L"New Package", L"Package name", name)) return;
        std::string n = trim(ui::U(name));
        if (n.empty()) return;
        if (d->prj.packages.count(n)) { ui::message(main_hwnd(), L"New Package", L"Package '" + name + L"' already exists."); return; }
        d->checkpoint("New package");
        Package pk;
        pk.name = n;
        Pad a, b;
        a.pos = {-0.75, 0}; b.pos = {0.75, 0};
        a.w = b.w = 0.8; a.h = b.h = 0.9;
        a.pin = 1; b.pin = 2;
        pk.pads = {a, b};
        pk.leads = 2;
        pk.auto_body();
        d->prj.packages[n] = pk;
        d->changed(ui::CH_GEOMETRY);
        select_package(n);
        scroll_to_cur_ = true;
    });
}

void PackagesView::act_duplicate() {
    auto names = lib_targets();
    if (names.empty()) return;
    doc->checkpoint("Duplicate packages");
    std::string last;
    for (auto& n : names) {
        auto it = doc->prj.packages.find(n);
        if (it == doc->prj.packages.end()) continue;
        Package cp = it->second;
        cp.name = unique_name(n + "_COPY");
        last = cp.name;
        doc->prj.packages[cp.name] = cp;
    }
    doc->changed(ui::CH_GEOMETRY);
    if (!last.empty()) { select_package(last); scroll_to_cur_ = true; }
    say(std::to_string(names.size()) + " package(s) duplicated");
}

void PackagesView::act_rename() {
    if (cur_.empty()) return;
    DocPtr d = doc;
    std::string old = cur_;
    defer([this, d, old]() {
        std::wstring name = ui::W(old);
        if (!ui::prompt_text(main_hwnd(), L"Rename Package", L"New package name (references are updated)", name)) return;
        std::string n = trim(ui::U(name));
        if (n.empty() || n == old || !d->prj.packages.count(old)) return;
        if (d->prj.packages.count(n)) { ui::message(main_hwnd(), L"Rename Package", L"Package '" + name + L"' already exists."); return; }
        d->checkpoint("Rename package");
        Package pk = d->prj.packages[old];
        pk.name = n;
        d->prj.packages.erase(old);
        d->prj.packages[n] = pk;
        int nc = 0, np = 0;
        for (auto& c : d->prj.comps) if (c.package == old) { c.package = n; ++nc; }
        for (auto& kv : d->prj.parts) if (kv.second.package == old) { kv.second.package = n; ++np; }
        for (auto& mp : d->prj.model.points) for (auto& f : mp.filter_packages) if (f == old) f = n;
        d->changed(ui::CH_GEOMETRY);
        thumbs_.erase(old);
        select_package(n);
        scroll_to_cur_ = true;
        say("Renamed; " + std::to_string(nc) + " locations and " + std::to_string(np) + " part numbers updated");
    });
}

void PackagesView::act_delete_unused() {
    auto names = lib_targets();
    std::vector<std::string> del;
    int used = 0;
    for (auto& n : names) { if (use_comps_.count(n)) ++used; else del.push_back(n); }
    if (del.empty()) { say("Selected packages are in use; merge them into another package instead", ToastKind::Warning); return; }
    DocPtr d = doc;
    defer([this, d, del, used]() {
        std::wstring msg = L"Delete " + std::to_wstring(del.size()) + L" unused package(s)?";
        if (del.size() == 1) msg = L"Delete package '" + ui::W(del[0]) + L"'?";
        if (used) msg += L"\n" + std::to_wstring(used) + L" selected package(s) are placed on the board and will be kept.";
        if (!ui::message(main_hwnd(), L"Delete Packages", msg, true)) return;
        d->checkpoint("Delete packages");
        // neighbour selection
        std::string next;
        for (size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i] == del.back()) { for (size_t j = i + 1; j < rows_.size(); ++j) if (std::find(del.begin(), del.end(), rows_[j]) == del.end()) { next = rows_[j]; break; } break; }
        for (auto& n : del) { d->prj.packages.erase(n); thumbs_.erase(n); }
        d->changed(ui::CH_GEOMETRY);
        cur_.clear();
        lsel_.clear();
        if (!next.empty()) select_package(next);
        say(std::to_string(del.size()) + " package(s) deleted");
    });
}

void PackagesView::act_merge() {
    auto src = lib_targets();
    if (src.empty()) return;
    std::vector<std::wstring> choices;
    std::vector<std::string> names;
    for (auto& kv : doc->prj.packages)
        if (std::find(src.begin(), src.end(), kv.first) == src.end()) { names.push_back(kv.first); choices.push_back(ui::W(kv.first)); }
    if (names.empty()) return;
    DocPtr d = doc;
    defer([this, d, src, names, choices]() {
        int k = ui::prompt_choice(main_hwnd(), L"Merge Packages",
                                  L"Replace every reference to the " + std::to_wstring(src.size()) + L" selected package(s) with:", choices, 0);
        if (k < 0 || k >= (int)names.size()) return;
        const std::string tgt = names[k];
        d->checkpoint("Merge packages");
        int nc = 0, np = 0;
        for (auto& s : src) {
            for (auto& c : d->prj.comps) if (c.package == s) { c.package = tgt; ++nc; }
            for (auto& kv : d->prj.parts) if (kv.second.package == s) { kv.second.package = tgt; ++np; }
            for (auto& mp : d->prj.model.points) for (auto& f : mp.filter_packages) if (f == s) f = tgt;
            d->prj.packages.erase(s);
            thumbs_.erase(s);
        }
        d->changed(ui::CH_GEOMETRY);
        select_package(tgt);
        scroll_to_cur_ = true;
        say("Merged into " + tgt + ": " + std::to_string(nc) + " locations, " + std::to_string(np) + " part numbers updated");
    });
}

void PackagesView::act_export(bool selection_only) {
    std::vector<std::string> names;
    if (selection_only) names = lib_targets();
    else for (auto& kv : doc->prj.packages) names.push_back(kv.first);
    if (names.empty()) return;
    DocPtr d = doc;
    defer([d, names]() {
        std::wstring path = L"packages.pcbp";
        if (!ui::save_file_dialog(main_hwnd(), L"Export Package Library", {{L"PCB Plotter library (*.pcbp)", L"*.pcbp"}}, L"pcbp", path)) return;
        Project lib;
        lib.info.name = "Package library";
        lib.info.description = "Exported from " + ui::U(d->display_name());
        lib.display_units = d->prj.display_units;
        for (auto& n : names) { auto it = d->prj.packages.find(n); if (it != d->prj.packages.end()) lib.packages[n] = it->second; }
        std::string err;
        if (!save_project(lib, ui::U(path), err)) { ui::message(main_hwnd(), L"Export Package Library", ui::W(err)); return; }
        say(std::to_string(lib.packages.size()) + " package(s) exported to " + ui::U(path));
    });
}

void PackagesView::act_import() {
    DocPtr d = doc;
    defer([this, d]() {
        std::wstring path = app::settings().library_path;
        if (!ui::open_file_dialog(main_hwnd(), L"Import Packages",
                                  {{L"Libraries and projects", L"*.pcbp;*.cpf;*.mdb"}, {L"PCB Plotter (*.pcbp)", L"*.pcbp"},
                                   {L"Legacy project / library", L"*.cpf;*.mdb"}, {L"All files", L"*.*"}}, path)) return;
        Project tmp;
        std::string err;
        std::wstring ext = path.size() > 5 ? path.substr(path.size() - 5) : L"";
        for (auto& c : ext) c = (wchar_t)towlower(c);
        if (ext == L".pcbp") {
            if (!load_project(ui::U(path), tmp, err)) { ui::message(main_hwnd(), L"Import Packages", ui::W(err)); return; }
        } else {
            Importer* im = PluginRegistry::get().importer("circuitcam.cpf");
            if (!im) { ui::message(main_hwnd(), L"Import Packages", L"Legacy library importer not available."); return; }
            ImportReport rep;
            Options o = default_options(im->info());
            o["library_only"] = "1";
            if (!im->run(ui::U(path), o, tmp, rep, err)) { ui::message(main_hwnd(), L"Import Packages", ui::W(err)); return; }
        }
        if (tmp.packages.empty()) { ui::message(main_hwnd(), L"Import Packages", L"The file contains no packages."); return; }
        int clash = 0;
        for (auto& kv : tmp.packages) if (d->prj.packages.count(kv.first)) ++clash;
        int mode = 0;
        if (clash) {
            mode = ui::prompt_choice(main_hwnd(), L"Import Packages",
                                     std::to_wstring(tmp.packages.size()) + L" packages found; " + std::to_wstring(clash) + L" already exist in this project.",
                                     {L"Skip existing packages", L"Overwrite existing packages", L"Import duplicates with a new name"}, 0);
            if (mode < 0) return;
        }
        d->checkpoint("Import packages");
        int added = 0, replaced = 0;
        for (auto& kv : tmp.packages) {
            if (!d->prj.packages.count(kv.first)) { d->prj.packages[kv.first] = kv.second; ++added; }
            else if (mode == 1) { d->prj.packages[kv.first] = kv.second; thumbs_.erase(kv.first); ++replaced; }
            else if (mode == 2) { Package p = kv.second; p.name = unique_name(kv.first + "_LIB"); d->prj.packages[p.name] = p; ++added; }
        }
        d->changed(ui::CH_GEOMETRY);
        say(std::to_string(added) + " package(s) added, " + std::to_string(replaced) + " replaced");
    });
}

void PackagesView::act_show_users() {
    auto names = lib_targets();
    std::set<std::string> ns(names.begin(), names.end());
    std::vector<int> idx;
    for (size_t i = 0; i < doc->prj.comps.size(); ++i) if (ns.count(doc->prj.comps[i].package)) idx.push_back((int)i);
    if (idx.empty()) { say("No placements use the selected package(s)", ToastKind::Warning); return; }
    DocPtr d = doc;
    defer([d, idx]() {
        activate_view(d, ViewKind::Plotter);
        focus_components(d, idx, true);
        say(std::to_string(idx.size()) + " placement(s) selected");
    });
}

// ---------------------------------------------------------------------------
// master library: send / pull with duplicate handling
// ---------------------------------------------------------------------------
const char* kPolicyNames[] = {"Ask", "Overwrite", "Skip", "Keep both"};

// Conflict prompt (between frames). Returns Ask when the user cancels the transfer.
ConflictAction ask_conflict(const std::string& name, const Package& incoming, const Package& existing, bool to_lib, int remaining, bool& apply_all) {
    ui::Form f(L"Package Already Exists", 640);
    f.header(ui::W("'" + name + "' already exists in the " + (to_lib ? "master library" : "project") + " with different content"))
        .icon((int)ui::Icon::Warning);
    Package a = incoming, b = existing;
    f.custom("cmp", [a, b, to_lib](ui::Form&, float w) {
        const float h = S(200), gap = S(10), cw = (w - gap) * 0.5f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 o = ImGui::GetCursorScreenPos();
        const Package* pk[2] = {&a, &b};
        const char* lbl[2] = {to_lib ? "Incoming (project)" : "Incoming (library)", to_lib ? "Existing (library)" : "Existing (project)"};
        for (int i = 0; i < 2; ++i) {
            ImVec2 p0 = o + ImVec2((cw + gap) * i, 0), p1 = p0 + ImVec2(cw, h);
            dl->AddRectFilled(p0, p1, th().panel2, S(8));
            dl->AddRect(p0, p1, i == 0 ? with_alpha(th().accent, 0xC0) : th().border, S(8));
            dl->AddText(p0 + ImVec2(S(10), S(6)), th().text_dim, lbl[i]);
            draw_pkg_dl(dl, *pk[i], p0 + ImVec2(S(8), S(26)), p1 - ImVec2(S(8), S(26)), true);
            char info[96];
            snprintf(info, sizeof info, "%d pads  \xc2\xb7  %d graphics  \xc2\xb7  %s", (int)pk[i]->pads.size(), (int)pk[i]->body.size(), is_th(*pk[i]) ? "TH" : "SMT");
            dl->AddText(ImVec2(p0.x + S(10), p1.y - S(22)), th().text_dim, info);
        }
        ImGui::Dummy(ImVec2(w, h));
    }, 205);
    f.combo("act", L"Action", {L"Overwrite existing", L"Skip (keep existing)", L"Keep both (incoming gets a new name)"}, L"Overwrite existing");
    f.check("all", L"Apply to all remaining conflicts" + (remaining > 0 ? L" (" + std::to_wstring(remaining) + L" more)" : std::wstring()), false);
    f.ok_text(L"Apply").cancel_text(L"Stop transfer");
    if (!f.run(main_hwnd())) return ConflictAction::Ask;
    apply_all = f.checked("all");
    int k = f.index("act");
    return k == 1 ? ConflictAction::Skip : k == 2 ? ConflictAction::KeepBoth : ConflictAction::Overwrite;
}

void PackagesView::act_transfer(bool to_lib) {
    std::vector<std::string> names = to_lib ? lib_targets() : mlib_targets();
    if (names.empty()) { say("Select packages to transfer", ToastKind::Warning); return; }
    if (to_lib && LIB().master.read_only) { say("Master library is read-only", ToastKind::Warning); return; }
    DocPtr d = doc;
    const ConflictAction pol = policy_;
    const bool from_source = !to_lib && lsrc_ == 1 && !LIB().source.path.empty();
    defer([this, d, names, to_lib, pol, from_source]() {
        auto& lib = from_source ? LIB().source : LIB().master;
        auto& src = to_lib ? d->prj.packages : lib.packages;
        auto& dst = to_lib ? LIB().master.packages : d->prj.packages;
        // count real conflicts for the prompt
        int conflicts = 0;
        for (auto& n : names) {
            auto a = src.find(n), b = dst.find(n);
            if (a != src.end() && b != dst.end() && !package_same(a->second, b->second)) ++conflicts;
        }
        ConflictAction sticky = pol;
        int seen = 0;
        auto resolve = [&](const std::string& n) -> ConflictAction {
            ++seen;
            if (sticky != ConflictAction::Ask) return sticky;
            bool all = false;
            ConflictAction a = ask_conflict(n, src.at(n), dst.at(n), to_lib, conflicts - seen, all);
            if (all && a != ConflictAction::Ask) sticky = a;
            return a;
        };
        // work on a copy so "Stop transfer" leaves everything untouched
        std::map<std::string, Package> out = dst;
        TransferStats st;
        std::vector<std::string> renamed;
        if (!transfer_packages(src, out, names, resolve, st, &renamed)) { say("Transfer cancelled; nothing changed", ToastKind::Warning); return; }
        const bool changed = st.added + st.replaced + st.renamed > 0;
        if (to_lib) {
            if (changed) {
                LIB().master.packages = std::move(out);
                std::string err;
                if (!save_master(&err)) { ui::message(main_hwnd(), L"Master Library", L"The library could not be saved:\n" + ui::W(err)); }
                for (auto& n : names) thumbs_.erase("L:" + n);
            }
        } else if (changed) {
            d->checkpoint("Pull packages from library");
            d->prj.packages = std::move(out);
            for (auto& n : names) { thumbs_.erase(n); if (d->prj.packages.count(n)) d->prj.packages[n].name = n; }
            d->changed(ui::CH_GEOMETRY);
        }
        sync_doc_gen_ = 0;
        std::string msg = std::string(to_lib ? "Sent to library: " : "Pulled into project: ") + std::to_string(st.added) + " added";
        if (st.replaced) msg += ", " + std::to_string(st.replaced) + " overwritten";
        if (st.renamed) msg += ", " + std::to_string(st.renamed) + " kept as copies";
        if (st.skipped) msg += ", " + std::to_string(st.skipped) + " skipped";
        if (st.identical) msg += ", " + std::to_string(st.identical) + " already identical";
        say(msg, st.skipped ? ToastKind::Warning : ToastKind::Success);
        if (!to_lib && names.size() == 1 && d->prj.packages.count(names[0]) && renamed.empty()) { mode_ = 0; select_package(names[0]); scroll_to_cur_ = true; }
    });
}

void PackagesView::act_lib_delete() {
    if (lsrc_ == 1 && !LIB().source.path.empty()) { say("Source libraries are read-only", ToastKind::Warning); return; }
    auto names = mlib_targets();
    if (names.empty()) return;
    defer([this, names]() {
        std::wstring msg = names.size() == 1 ? L"Delete '" + ui::W(names[0]) + L"' from the master library?" : L"Delete " + std::to_wstring(names.size()) + L" packages from the master library?";
        msg += L"\nProjects keep their local copies.";
        if (!ui::message(main_hwnd(), L"Master Library", msg, true)) return;
        for (auto& n : names) { LIB().master.packages.erase(n); thumbs_.erase("L:" + n); }
        save_master();
        lcur_.clear();
        llsel_.clear();
        sync_doc_gen_ = 0;
        say(std::to_string(names.size()) + " package(s) removed from the master library");
    });
}

void PackagesView::act_lib_open_source() {
    defer([this]() {
        std::wstring path = app::settings().library_path;
        if (!ui::open_file_dialog(main_hwnd(), L"Open Source Library (read-only)",
                                  {{L"Libraries and projects", L"*.mdb;*.cpf;*.pcbplib;*.pcbp"}, {L"Legacy library / project", L"*.mdb;*.cpf"},
                                   {L"PCB Plotter library / project", L"*.pcbplib;*.pcbp"}, {L"All files", L"*.*"}}, path)) return;
        PackageLibrary lib;
        std::string err, u = ui::U(path), lo = lower(u);
        auto ends = [&](const char* e) { size_t n = strlen(e); return lo.size() >= n && lo.compare(lo.size() - n, n, e) == 0; };
        if (ends(".pcbplib")) {
            if (!lib.load(u, err)) { ui::message(main_hwnd(), L"Open Source Library", ui::W(err)); return; }
        } else if (ends(".pcbp")) {
            Project tmp;
            bool lok = false;
            if (!run_with_progress_wait("Opening source library", [&](Progress& pr) { pr.set(-1.f, u.c_str()); lok = load_project(u, tmp, err); }, ProgressOpts{true, false, 0.2f})) return;
            if (!lok) { toast("Open source library: " + err, ToastKind::Error, 8.f); return; }
            lib.packages = tmp.packages;
        } else {
            Importer* im = PluginRegistry::get().importer("circuitcam.cpf");
            if (!im) { ui::message(main_hwnd(), L"Open Source Library", L"Legacy library importer not available."); return; }
            Project tmp;
            ImportReport rep;
            Options o = default_options(im->info());
            o["library_only"] = "1";
            bool lok = false;
            if (!run_with_progress_wait("Reading legacy library", [&](Progress& pr) { pr.set(-1.f, u.c_str()); lok = im->run(u, o, tmp, rep, err); }, ProgressOpts{true, false, 0.2f})) return;
            if (!lok) { toast("Open source library: " + err, ToastKind::Error, 8.f); return; }
            lib.packages = tmp.packages;
        }
        lib.path = u;
        lib.name = file_base(u);
        lib.read_only = true;
        lib.touch();
        LIB().source = std::move(lib);
        for (auto it = thumbs_.begin(); it != thumbs_.end();) it = it->first.rfind("L:", 0) == 0 ? thumbs_.erase(it) : std::next(it);
        lsrc_ = 1;
        mode_ = 1;
        lcur_.clear(); llsel_.clear();
        sync_doc_gen_ = 0;
        say(std::to_string(LIB().source.packages.size()) + " packages in " + LIB().source.name + " (read-only)");
    });
}

void PackagesView::act_lib_choose_master(bool create_new) {
    defer([this, create_new]() {
        std::wstring path = ui::W(LIB().master.path);
        bool ok = create_new ? ui::save_file_dialog(main_hwnd(), L"New Master Library", {{L"PCB Plotter package library (*.pcbplib)", L"*.pcbplib"}}, L"pcbplib", path)
                             : ui::open_file_dialog(main_hwnd(), L"Open Master Library", {{L"PCB Plotter package library (*.pcbplib)", L"*.pcbplib"}, {L"All files", L"*.*"}}, path);
        if (!ok) return;
        PackageLibrary lib;
        std::string err;
        bool lok = false;
        std::string up = ui::U(path);
        if (!run_with_progress_wait("Loading master library", [&](Progress& pr) { pr.set(-1.f, up.c_str()); lok = lib.load(up, err); }, ProgressOpts{true, false, 0.2f})) return;
        if (!lok) { toast("Master library: " + err, ToastKind::Error, 8.f); return; }
        if (create_new) { lib.packages.clear(); lib.name = "Master library"; if (!lib.save(err)) { ui::message(main_hwnd(), L"Master Library", ui::W(err)); return; } }
        if (lib.name.empty()) lib.name = "Master library";
        LIB().master = std::move(lib);
        LIB().master.touch();
        set_master_path_setting(path);
        for (auto it = thumbs_.begin(); it != thumbs_.end();) it = it->first.rfind("L:", 0) == 0 ? thumbs_.erase(it) : std::next(it);
        lsrc_ = 0;
        lcur_.clear(); llsel_.clear();
        sync_doc_gen_ = 0;
        say("Master library: " + LIB().master.path + " (" + std::to_string(LIB().master.packages.size()) + " packages)");
    });
}

void PackagesView::refresh_sync() {
    PackageLibrary& L = curlib();
    if (sync_doc_gen_ == doc->gen && sync_lib_gen_ == L.gen && sync_lib_ptr_ == &L) return;
    sync_doc_gen_ = doc->gen;
    sync_lib_gen_ = L.gen;
    sync_lib_ptr_ = &L;
    psync_.clear();
    lsync_.clear();
    // project badges are always against the master library
    for (auto& kv : doc->prj.packages) psync_[kv.first] = sync_state(doc->prj.packages, LIB().master.packages, kv.first, true);
    for (auto& kv : L.packages) lsync_[kv.first] = sync_state(doc->prj.packages, L.packages, kv.first, false);
    rows_gen_ = 0;
    lrows_key_.clear();
}

void PackagesView::rebuild_lib_rows() {
    PackageLibrary& L = curlib();
    std::string key = std::string(lsearch_) + "|" + std::to_string(lfilt_) + "|" + std::to_string(L.gen) + "|" + std::to_string((uintptr_t)&L) + "|" + std::to_string(doc->gen);
    if (key == lrows_key_) return;
    lrows_key_ = key;
    lrows_.clear();
    const std::string q = lower(trim(lsearch_));
    for (auto& kv : L.packages) {
        if (!q.empty() && lower(kv.first).find(q) == std::string::npos && lower(kv.second.description).find(q) == std::string::npos) continue;
        auto it = lsync_.find(kv.first);
        SyncState ss = it == lsync_.end() ? SyncState::LibraryOnly : it->second;
        if ((lfilt_ & 1) && ss != SyncState::LibraryOnly) continue;
        if ((lfilt_ & 2) && ss != SyncState::Differs) continue;
        if ((lfilt_ & 4) && ss == SyncState::LibraryOnly) continue;
        lrows_.push_back(kv.first);
    }
    std::stable_sort(lrows_.begin(), lrows_.end(), [](const std::string& a, const std::string& b) { return ref_less(a, b); });
    if (!lcur_.empty() && !L.packages.count(lcur_)) lcur_.clear();
    for (auto it = llsel_.begin(); it != llsel_.end();) it = L.packages.count(*it) ? std::next(it) : llsel_.erase(it);
}

std::vector<std::string> PackagesView::mlib_targets() const {
    std::vector<std::string> v(llsel_.begin(), llsel_.end());
    if (v.empty() && !lcur_.empty()) v.push_back(lcur_);
    return v;
}

void PackagesView::mlib_click(const std::string& name) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl) { if (!llsel_.erase(name)) llsel_.insert(name); lanchor_ = name; if (llsel_.count(name)) lcur_ = name; }
    else if (io.KeyShift && !lanchor_.empty()) {
        auto a = std::find(lrows_.begin(), lrows_.end(), lanchor_), b = std::find(lrows_.begin(), lrows_.end(), name);
        if (a != lrows_.end() && b != lrows_.end()) { if (a > b) std::swap(a, b); llsel_.clear(); for (auto it = a; it <= b; ++it) llsel_.insert(*it); lcur_ = name; }
    } else { llsel_ = {name}; lanchor_ = name; if (lcur_ != name) { lcur_ = name; canvas_.zoom_all(); } }
    canvas_.invalidate();
}

void PackagesView::open_template(Tpl k, const std::string& assign_to, int preset, const std::map<std::string, double>& params) {
    DocPtr d = doc;
    defer([this, d, k, assign_to, preset, params]() {
        const TSpec& sp = tspec(k);
        PMap def = tdefaults(k);
        const int pi0 = preset >= 0 && preset < (int)sp.presets.size() ? preset : 0;
        if (!sp.presets.empty()) for (auto& kv : sp.presets[pi0].v) def[kv.first] = kv.second;
        for (auto& kv : params) def[kv.first] = kv.second;
        const bool assign = !assign_to.empty();   // R16b: fill an unresolved package instead of creating a new one
        ui::Form f(assign ? L"Assign Template to Package" : L"New Package from Template", 640);
        std::string hdr = std::string(sp.title) + "  \xe2\x80\x94  " + sp.blurb;
        if (assign) hdr = "Graphics for " + assign_to + " (" + std::to_string(placements_using(d->prj, assign_to)) + " placement(s))  \xe2\x80\x94  " + sp.title +
                          ". Check the pads against the part / Gerber before using it.";
        f.header(ui::W(hdr)).icon((int)ui::Icon::PackageEditor).two_columns();
        std::string nm;
        make_template(k, def, &nm);
        if (!assign) f.text("name", L"Package name", ui::W(unique_name(nm))).tip(L"Leave as generated or type your own");
        if (!sp.presets.empty()) {
            std::vector<std::wstring> ch;
            for (auto& p : sp.presets) ch.push_back(ui::W(p.label));
            f.combo("preset", L"Preset", ch, ch[pi0]);
            f.on_change([k, this](ui::Form& ff, const std::string&) {
                int i = ff.index("preset");
                if (i < 0) return;
                for (auto& kv : tspec(k).presets[i].v) {
                    int dec = 3;
                    for (auto& p : tspec(k).params) if (std::string(p.key) == kv.first) dec = p.dec;
                    char b[32]; snprintf(b, sizeof b, "%.*f", dec, kv.second);
                    ff.set(kv.first, ui::W(b));
                }
                PMap m;
                for (auto& p : tspec(k).params) m[p.key] = ff.num(p.key, p.def);
                std::string n2;
                make_template(k, m, &n2);
                if (ff.field("name")) ff.set("name", ui::W(unique_name(n2)));
            });
        }
        for (auto& p : sp.params) f.number(p.key, ui::W(std::string(p.label) + (p.dec > 0 ? " (mm)" : "")), def[p.key], p.dec);
        f.custom("preview", [k](ui::Form& ff, float w) {
            PMap m;
            for (auto& p : tspec(k).params) m[p.key] = ff.num(p.key, p.def);
            Package pk = make_template(k, m, nullptr);
            ImVec2 a = ImGui::GetCursorScreenPos();
            float h = S(240);
            ImVec2 b = a + ImVec2(w, h);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(a, b, th().panel2, S(8));
            dl->AddRect(a, b, th().border, S(8));
            draw_pkg_dl(dl, pk, a + ImVec2(S(10), S(10)), b - ImVec2(S(10), S(28)), true);
            char info[160];
            BBox e;
            for (auto& q : pk.pads) e.add(pad_bbox(q));
            snprintf(info, sizeof info, "%d pads   pad field %s x %s   %s", (int)pk.pads.size(), mm(e.w()).c_str(), mm(e.h()).c_str(), is_th(pk) ? "through-hole" : "SMT");
            dl->AddText(ImVec2(a.x + S(12), b.y - S(22)), th().text_dim, info);
            ImGui::Dummy(ImVec2(w, h));
        }, 250);
        f.validate([d, assign](ui::Form& ff, std::wstring& e) {
            if (assign) return true;
            std::string n = trim(ff.u8("name"));
            if (n.empty()) { e = L"Enter a package name."; return false; }
            if (d->prj.packages.count(n)) { e = L"A package with this name already exists."; return false; }
            return true;
        });
        f.ok_text(assign ? ui::W("Use for all " + std::to_string(placements_using(d->prj, assign_to)) + " placements of " + assign_to) : L"Create package");   // R17a: precise wording
        if (!f.run(main_hwnd())) return;
        PMap m;
        for (auto& p : sp.params) m[p.key] = f.num(p.key, p.def);
        Package pk = make_template(k, m, nullptr);
        if (assign) {
            d->checkpoint("Assign template to " + assign_to);
            pk.polarity_set = false;
            const int n = assign_package_geometry(d->prj, assign_to, pk, std::string("template:") + kTplIds[(int)k]);
            thumbs_.erase(assign_to);
            d->changed(ui::CH_GEOMETRY);
            select_package(assign_to);
            right_tab_ = 0;
            say(assign_to + ": " + sp.title + " graphics applied to " + std::to_string(n) + " placement(s)");
            return;
        }
        pk.name = trim(f.u8("name"));
        d->checkpoint("New package from template");
        d->prj.packages[pk.name] = pk;
        d->changed(ui::CH_GEOMETRY);
        select_package(pk.name);
        scroll_to_cur_ = true;
        right_tab_ = 0;
        say("Created " + pk.name + " (" + std::to_string(pk.pads.size()) + " pads)");
    });
}

// ---------------------------------------------------------------------------
// drawing: library browser
// ---------------------------------------------------------------------------
void PackagesView::lib_click(const std::string& name) {
    ImGuiIO& io = ImGui::GetIO();
    lib_focus_ = true;
    if (io.KeyCtrl) {
        if (!lsel_.erase(name)) lsel_.insert(name);
        anchor_ = name;
        if (lsel_.count(name) && name != cur_) { std::set<std::string> keep = lsel_; select_package(name); lsel_ = keep; }
        else if (!lsel_.count(name) && name == cur_ && !lsel_.empty()) { std::set<std::string> keep = lsel_; select_package(*keep.begin()); lsel_ = keep; }
    } else if (io.KeyShift && !anchor_.empty()) {
        auto a = std::find(rows_.begin(), rows_.end(), anchor_), b = std::find(rows_.begin(), rows_.end(), name);
        if (a != rows_.end() && b != rows_.end()) {
            if (a > b) std::swap(a, b);
            std::string keep_anchor = anchor_;
            select_package(name);
            lsel_.clear();
            for (auto it = a; it <= b; ++it) lsel_.insert(*it);
            anchor_ = keep_anchor;
        }
    } else select_package(name);
}

// sync badge text/colours
void sync_badge(ImDrawList* dl, ImVec2 right_top, SyncState s, bool lib_side) {
    const GTheme& t = th();
    const char* txt = nullptr;
    ImU32 bg = 0, fg = t.text;
    switch (s) {
    case SyncState::Same: txt = lib_side ? "in project" : "in library"; bg = with_alpha(t.ok, 0x40); fg = t.text; break;
    case SyncState::Differs: txt = "differs"; bg = with_alpha(t.warn, 0x60); fg = t.text; break;
    case SyncState::LocalOnly: txt = "local only"; bg = with_alpha(t.text_faint, 0x40); fg = t.text_dim; break;
    case SyncState::LibraryOnly: txt = "not in project"; bg = with_alpha(t.text_faint, 0x30); fg = t.text_dim; break;
    }
    if (!txt) return;
    ImVec2 ts = g_font->CalcTextSizeA(S(11), FLT_MAX, 0, txt);
    ImVec2 a(right_top.x - ts.x - S(10), right_top.y), b(right_top.x, right_top.y + ts.y + S(3));
    dl->AddRectFilled(a, b, bg, (b.y - a.y) * 0.5f);
    dl->AddText(g_font, S(11), ImVec2(a.x + S(5), a.y + S(1.5f)), fg, txt);
}

void PackagesView::draw_mode_switch() {
    const GTheme& t = th();
    const float avail = ImGui::GetContentRegionAvail().x, bh = S(34), bw = avail * 0.5f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + ImVec2(avail, bh), t.panel2, S(9));
    struct M { const char* l; G g; int n; const char* tip; } ms[2] = {
        {"Project", G::Project, (int)doc->prj.packages.size(), "Local packages stored in this project (editable)"},
        {"Library", G::Library, (int)curlib().packages.size(), "Master package library shared by all projects\n(send local packages to it, pull packages from it)"}};
    for (int i = 0; i < 2; ++i) {
        ImGui::SetCursorScreenPos(p + ImVec2(bw * i, 0));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##mode", ImVec2(bw, bh)) && mode_ != i) { mode_ = i; hover_ = ghover_ = -1; canvas_.cancel_draw(); canvas_.zoom_all(); canvas_.invalidate(); }
        bool hov = ImGui::IsItemHovered();
        tooltip(ms[i].tip);
        ImGui::PopID();
        ImVec2 a = p + ImVec2(bw * i + S(3), S(3)), b = p + ImVec2(bw * (i + 1) - S(3), bh - S(3));
        if (mode_ == i) { dl->AddRectFilled(a, b, t.raised, S(7)); dl->AddRect(a, b, with_alpha(t.accent, 0x90), S(7)); }
        char lb[64];
        snprintf(lb, sizeof lb, "%s  %d", ms[i].l, ms[i].n);
        ImVec2 ts = ImGui::CalcTextSize(lb);
        float iw = S(16), x = (a.x + b.x - ts.x - iw - S(6)) * 0.5f;
        glyph(dl, ms[i].g, ImVec2(x, (a.y + b.y - iw) * 0.5f), iw, mode_ == i ? 1.f : 0.6f);
        dl->AddText(ImVec2(x + iw + S(6), (a.y + b.y - ts.y) * 0.5f), mode_ == i || hov ? t.text : t.text_dim, lb);
    }
    ImGui::SetCursorScreenPos(p + ImVec2(0, bh + S(8)));
}

// spinner placeholder while a thumbnail waits for its render budget
static void thumb_placeholder(ImVec2 a, float px) {
    const GTheme& t = th();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, a + ImVec2(px, px), col_alpha(t.raised, 0.35f + 0.25f * pulse(1.4f)), S(6));
    // draw-list only spinner (no ImGui item: keeps SameLine grid layout intact)
    const float r = std::min(S(9), px * 0.18f);
    const ImVec2 c = a + ImVec2(px * 0.5f, px * 0.5f);
    const float a0 = (float)ImGui::GetTime() * 6.f;
    dl->PathArcTo(c, r, a0, a0 + 4.2f, 24);
    dl->PathStroke(t.accent, 0, std::max(1.5f, S(2)));
    request_redraw(2);
}

void PackagesView::draw_items(const std::vector<std::string>& rows, bool lib, float footer) {
    const GTheme& t = th();
    auto& pk_map = lib ? curlib().packages : doc->prj.packages;
    auto& sel_set = lib ? llsel_ : lsel_;
    const std::string& cur = lib ? lcur_ : cur_;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
    ImGui::BeginChild(lib ? "##mlibitems" : "##libitems", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs);
    list_keys(rows, lib);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int budget = 16;
    const float avail = ImGui::GetContentRegionAvail().x;
    auto usage_of = [&](const std::string& n) { auto it = use_comps_.find(n); return it == use_comps_.end() ? 0 : it->second; };
    auto sync_of = [&](const std::string& n) {
        auto& m = lib ? lsync_ : psync_;
        auto it = m.find(n);
        return it == m.end() ? (lib ? SyncState::LibraryOnly : SyncState::LocalOnly) : it->second;
    };
    bool open_ctx = false;
    auto click = [&](const std::string& n) { if (lib) mlib_click(n); else lib_click(n); };
    if (rows.empty()) {
        if (!pk_map.empty()) {
            if (empty_state(ui::Icon::FindRef, "No packages match", "Nothing matches the search text / filters.", "Clear search", ui::Icon::Eraser)) {
                (lib ? lsearch_ : search_)[0] = 0;
            }
        } else if (lib) {
            if (empty_state(ui::Icon::PackageEditor, "Master library is empty", "Switch to Project, select packages and send them to the library.",
                            "Show project packages", ui::Icon::ArrowUp))
                mode_ = 0;
        } else if (empty_state(ui::Icon::PackageEditor, "No packages yet", "Pull packages from the master library, import a library file, or create one from a template.",
                               "Browse master library", ui::Icon::Folder)) {
            mgr_open_ = true; mode_ = 1;
        }
    } else if (grid_view_) {
        const float gap = S(6);
        int cols = std::max(1, (int)((avail + gap) / (S(96) + gap)));
        float tw = std::floor((avail - gap * (cols - 1)) / cols);
        float th_ = tw + S(22);
        int nrows = ((int)rows.size() + cols - 1) / cols;
        if (scroll_to_cur_ && !lib) {
            auto it = std::find(rows.begin(), rows.end(), cur);
            if (it != rows.end()) ImGui::SetScrollY(std::max(0.f, (float)((it - rows.begin()) / cols) * (th_ + gap) - S(60)));
            scroll_to_cur_ = false;
        }
        ImGuiListClipper clip;
        clip.Begin(nrows, th_ + gap);
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                for (int c = 0; c < cols; ++c) {
                    size_t i = (size_t)r * cols + c;
                    if (i >= rows.size()) break;
                    const std::string& n = rows[i];
                    auto pit = pk_map.find(n);
                    if (pit == pk_map.end()) continue;
                    if (c) ImGui::SameLine(0, gap);
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::PushID((int)i);
                    bool clicked = ImGui::InvisibleButton("##t", ImVec2(tw, th_));
                    bool hov = ImGui::IsItemHovered();
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { if (!sel_set.count(n)) click(n); open_ctx = true; }
                    if (clicked) click(n);
                    const bool sel = sel_set.count(n) > 0, curr = n == cur;
                    const float ha = anim(ImGui::GetID("h"), hov ? 1.f : 0.f, 16.f);
                    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    dl->AddRectFilled(p, p + ImVec2(tw, th_), sel ? with_alpha(t.accent, 0x30) : col_lerp(t.panel2, t.raised, ha), S(8));
                    if (sel || ha > 0.01f) dl->AddRect(p, p + ImVec2(tw, th_), sel ? with_alpha(t.accent, curr ? 0xFF : 0x90) : col_alpha(t.border, ha), S(8), 0, curr ? 2.f : 1.f);
                    int px = (int)(tw - S(12));
                    if (void* tex = thumb((lib ? "L:" : "") + n, pit->second, px, budget)) {
                        float fa = anim(ImGui::GetID("ta"), 1.f, 10.f);   // fade in once rendered
                        dl->AddImageRounded((ImTextureID)tex, p + ImVec2(S(6), S(6)), p + ImVec2(S(6) + px, S(6) + px), ImVec2(0, 0), ImVec2(1, 1), col_alpha(IM_COL32_WHITE, fa), S(6));
                    } else {
                        thumb_placeholder(p + ImVec2(S(6), S(6)), (float)px);
                        anim(ImGui::GetID("ta"), 0.f, 1000.f);
                    }
                    text_clip(dl, ImVec2(p.x + S(7), p.y + tw - S(3)), tw - S(14), n, !lib && !package_resolved(pit->second) ? t.warn : sel ? t.text : t.text_dim);
                    if (!lib && !package_resolved(pit->second)) text_clip(dl, ImVec2(p.x + S(8), p.y + tw * 0.42f), tw - S(14), "Unresolved", t.warn);
                    SyncState ss = sync_of(n);
                    ImU32 dot = ss == SyncState::Same ? t.ok : ss == SyncState::Differs ? t.warn : with_alpha(t.text_faint, 0x80);
                    dl->AddCircleFilled(ImVec2(p.x + S(11), p.y + S(11)), S(4), dot);
                    if (!lib) {
                        int u = usage_of(n);
                        char b[16];
                        snprintf(b, sizeof b, "%d", u);
                        badge(dl, ImVec2(p.x + tw - S(5), p.y + S(5)), b, u ? t.accent : with_alpha(t.text_faint, 0x50), u ? t.accent_text : t.text_dim);
                    }
                    if (hov) {
                        const Package& pk = pit->second;
                        std::string tip = n + "\n" + std::to_string(pk.pads.size()) + " pads, " + (is_th(pk) ? "through-hole" : "SMT");
                        if (!lib) tip += "\nUsed by " + std::to_string(usage_of(n)) + " placement(s)";
                        tip += std::string("\n") + (ss == SyncState::Same ? (lib ? "Identical copy in this project" : "Identical in the master library")
                                                   : ss == SyncState::Differs ? (lib ? "Project copy differs" : "Master library copy differs")
                                                   : lib ? "Not in this project" : "Not in the master library");
                        if (!pk.description.empty()) tip += "\n" + pk.description;
                        tip += std::string("\nBody outline: ") + io::body_source_label(pk.body_source);   // REVIEW_V9 §6A provenance
                        if (AxisInfo ax = pkg_axis_info(pk); ax.off_axis) { char ob[96]; snprintf(ob, sizeof ob, "\nArtwork rotated %+.2f\xc2\xb0 off-axis (%s)", ax.off90, ax.why.c_str()); tip += ob; }
                        tooltip(tip.c_str());
                    }
                    ImGui::PopID();
                }
                ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
            }
    } else if (view_mode_ == 2) {
        // plain text list: names only (no thumbnails / tags), fastest way to find a package
        const float rh = ImGui::GetTextLineHeight() + S(2);   // R12 §15: tighter rows
        if (scroll_to_cur_ && !lib) {
            auto it = std::find(rows.begin(), rows.end(), cur);
            if (it != rows.end()) ImGui::SetScrollY(std::max(0.f, (float)(it - rows.begin()) * rh - S(60)));
            scroll_to_cur_ = false;
        }
        ImGuiListClipper clip;
        clip.Begin((int)rows.size(), rh);
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const std::string& n = rows[i];
                ImGui::PushID(i);
                ImVec2 p = ImGui::GetCursorScreenPos();
                bool clicked = ImGui::InvisibleButton("##p", ImVec2(avail, rh));
                bool hov = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { if (!sel_set.count(n)) click(n); open_ctx = true; }
                if (clicked) click(n);
                const bool sel = sel_set.count(n) > 0, curr = n == cur;
                if (sel) dl->AddRectFilled(p, p + ImVec2(avail, rh), with_alpha(t.accent, 0x30), S(4));
                else if (hov) dl->AddRectFilled(p, p + ImVec2(avail, rh), t.raised, S(4));
                if (curr) dl->AddRectFilled(p + ImVec2(0, S(2)), p + ImVec2(S(3), rh - S(2)), t.accent, S(2));
                text_clip(dl, ImVec2(p.x + S(8), p.y + S(1)), avail - S(16), n, curr ? t.text : t.text, curr ? g_font_bold : g_font);
                ImGui::PopID();
                ImGui::Dummy(ImVec2(0, -ImGui::GetStyle().ItemSpacing.y));
            }
    } else {
        const float rh = S(46);
        if (scroll_to_cur_ && !lib) {
            auto it = std::find(rows.begin(), rows.end(), cur);
            if (it != rows.end()) ImGui::SetScrollY(std::max(0.f, (float)(it - rows.begin()) * (rh + S(2)) - S(60)));
            scroll_to_cur_ = false;
        }
        ImGuiListClipper clip;
        clip.Begin((int)rows.size(), rh + S(2));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const std::string& n = rows[i];
                auto pit = pk_map.find(n);
                if (pit == pk_map.end()) { ImGui::Dummy(ImVec2(avail, rh)); continue; }
                const Package& pk = pit->second;
                ImGui::PushID(i);
                ImVec2 p = ImGui::GetCursorScreenPos();
                bool clicked = ImGui::InvisibleButton("##r", ImVec2(avail, rh));
                bool hov = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { if (!sel_set.count(n)) click(n); open_ctx = true; }
                if (clicked) click(n);
                const bool sel = sel_set.count(n) > 0, curr = n == cur;
                const float ha = anim(ImGui::GetID("h"), hov ? 1.f : 0.f, 16.f);
                if (sel) dl->AddRectFilled(p, p + ImVec2(avail, rh), with_alpha(t.accent, 0x30), S(7));
                else if (ha > 0.01f) dl->AddRectFilled(p, p + ImVec2(avail, rh), col_alpha(t.raised, ha), S(7));
                if (curr) dl->AddRectFilled(p + ImVec2(0, S(8)), p + ImVec2(S(3), rh - S(8)), t.accent, S(2));
                int px = (int)(rh - S(8));
                if (void* tex = thumb((lib ? "L:" : "") + n, pk, (int)S(84), budget))
                    dl->AddImageRounded((ImTextureID)tex, p + ImVec2(S(8), S(4)), p + ImVec2(S(8) + px, S(4) + px), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, S(5));
                else thumb_placeholder(p + ImVec2(S(8), S(4)), (float)px);
                float tx = p.x + S(16) + px;
                SyncState ss = sync_of(n);
                text_clip(dl, ImVec2(tx, p.y + S(5)), avail - (tx - p.x) - S(96), n, t.text, g_font_bold);
                std::string sub = std::to_string(pk.pads.size()) + " pads  \xc2\xb7  " + (is_th(pk) ? "TH" : "SMT");
                const bool unres = !lib && !package_resolved(pk);   // R16b
                if (unres) sub = "UNRESOLVED / NO GRAPHICS";
                if (!lib) sub += "  \xc2\xb7  used " + std::to_string(usage_of(n));
                if (!pk.description.empty()) sub += "  \xc2\xb7  " + pk.description;
                const AxisInfo ax = pkg_axis_info(pk);
                if (ax.off_axis) { char ob[48]; snprintf(ob, sizeof ob, "OFF-AXIS %+.1f\xc2\xb0  \xc2\xb7  ", ax.off90); sub = ob + sub; }
                text_clip(dl, ImVec2(tx, p.y + S(25)), avail - (tx - p.x) - S(12), sub, (ax.off_axis || unres) ? t.warn : t.text_dim);
                sync_badge(dl, ImVec2(p.x + avail - S(8), p.y + S(6)), ss, lib);
                ImGui::PopID();
                ImGui::Dummy(ImVec2(0, S(2) - ImGui::GetStyle().ItemSpacing.y));
            }
    }
    if (open_ctx) ImGui::OpenPopup(lib ? "##mlibctx" : "##libctx");
    if (lib) draw_mlib_context(); else draw_lib_context();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void PackagesView::draw_library(float w, float h) {
    const GTheme& t = th();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    ImGui::BeginChild("##lib", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    refresh_sync();
    const bool lib = mode_ == 1;
    {   // header (P2-05: no Project / Library switch; the library has its own manager window)
        section_header(lib ? "LIBRARY" : "PACKAGES");
        if (!lib) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(30) - ImGui::GetFrameHeight());
            if (gbtn("libmgr", G::Library, "Package Library Manager...\nBrowse the master library, pull packages into this project, send packages to the library")) {
                mgr_open_ = true; mode_ = 1; hover_ = ghover_ = -1; canvas_.cancel_draw(); canvas_.zoom_all(); canvas_.invalidate();
            }
            ImGui::SameLine(0, S(2));
            if (icon_button("lcollapse", ui::Icon::ArrowLeft, "Hide the package list (N or Ctrl+[)", false, true, ImGui::GetFrameHeight())) { show_left_ = false; app::set_ui_pref("Pkg.ShowList", 0); }
        }
    }
    if (!lib) rebuild_rows(); else rebuild_lib_rows();
    // search + view / sort
    {
        float bs = ImGui::GetFrameHeight();
        search_box(lib ? "##lsearch" : "##pkgsearch", lib ? lsearch_ : search_, sizeof search_, lib ? "Search library" : "Search project packages",
                   ImGui::GetContentRegionAvail().x - (bs + S(2)) * (lib ? 1 : 2) - S(2));
        if (!lib) {
            ImGui::SameLine(0, S(4));
            if (gbtn("sort", G::Sort, "Sort", false, true, bs)) ImGui::OpenPopup("##sortpop");
            if (ImGui::BeginPopup("##sortpop")) {
                const char* sn[] = {"Name", "Usage (most used first)", "Pad count"};
                for (int i = 0; i < 3; ++i) if (ImGui::MenuItem(sn[i], nullptr, sort_ == i)) sort_ = i;
                ImGui::Separator();
                ImGui::MenuItem("Reverse order", nullptr, &sort_desc_);
                ImGui::Separator();
                if (ImGui::MenuItem("Merge similar packages...")) open_merge_similar();
                ImGui::EndPopup();
            }
        }
        ImGui::SameLine(0, S(2));
        if (gbtn("viewmode", view_mode_ == 0 ? G::GridView : G::ListView, "Display: thumbnails / compact preview / plain text list", false, true, bs)) ImGui::OpenPopup("##vmpop");
        if (ImGui::BeginPopup("##vmpop")) {
            const char* vm[] = {"Thumbnails", "Compact preview", "Plain text list"};
            for (int i = 0; i < 3; ++i) if (ImGui::MenuItem(vm[i], nullptr, view_mode_ == i)) view_mode_ = i;
            ImGui::EndPopup();
        }
        grid_view_ = view_mode_ == 0;
    }
    // library source line
    if (lib) {
        PackageLibrary& L = curlib();
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(L.read_only ? "Source" : "Library");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, S(6));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - S(4));
        std::string cur = L.read_only ? L.name + "  (read-only)" : "Master  \xc2\xb7  " + file_base(L.path);
        if (ImGui::BeginCombo("##libsrc", cur.c_str())) {
            if (ImGui::Selectable(("Master  \xc2\xb7  " + file_base(LIB().master.path)).c_str(), lsrc_ == 0)) { lsrc_ = 0; lcur_.clear(); llsel_.clear(); canvas_.invalidate(); }
            if (!LIB().source.path.empty() && ImGui::Selectable((LIB().source.name + "  (read-only)").c_str(), lsrc_ == 1)) { lsrc_ = 1; lcur_.clear(); llsel_.clear(); canvas_.invalidate(); }
            ImGui::Separator();
            if (ImGui::Selectable("Open source library (legacy .mdb / .cpf, .pcbplib)...")) act_lib_open_source();
            if (ImGui::Selectable("Open master library file...")) act_lib_choose_master(false);
            if (ImGui::Selectable("New master library file...")) act_lib_choose_master(true);
            ImGui::EndCombo();
        }
        tooltip(L.path.c_str());
        ImGui::SameLine(0, S(4));
        if (gbtn("libopen", G::Import, "Open a legacy library (.mdb / .cpf) as a read-only source", false, true, ImGui::GetFrameHeight())) act_lib_open_source();
    }
    // filter chips
    {
        struct F { const char* l; unsigned bit; unsigned excl; const char* tip; };
        std::vector<F> fs;
        if (!lib) fs = {{"Used", 1, 2, "Placed on the board"}, {"Unused", 2, 1, "Not referenced by any placement"},
                        {"SMT", 4, 8, "Surface mount"}, {"TH", 8, 4, "Through-hole (drilled pads)"},
                        {"Local only", 64, 128, "Not in the master library"}, {"Differs", 128, 64, "Different from the master library copy"},
                        {"Reconstructed", 16, 0, "Rebuilt from imported machine programs"},
                        {"Unresolved", 256, 2, "Used by placements but no graphics yet (centroid imports): assign a template, a library package or build from the Gerbers"}};
        else fs = {{"Not in project", 1, 6, "Library packages this project does not have"}, {"Differs", 2, 5, "Project has a different copy"},
                   {"In project", 4, 3, "Project has a copy"}};
        unsigned& fl = lib ? lfilt_ : filt_;
        // R10 §9: one compact Filter dropdown instead of a wall of chips
        int nact = 0; std::string act;
        for (auto& f : fs) if (fl & f.bit) { ++nact; act += (act.empty() ? "" : ", ") + std::string(f.l); }
        std::string fl_lbl = nact ? "Filter: " + act : std::string("Filter: all packages");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (nact ? ImGui::GetFrameHeight() + S(4) : 0));
        if (ImGui::BeginCombo("##pkfilter", fl_lbl.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (auto& f : fs) {
                bool on = (fl & f.bit) != 0;
                if (ImGui::Checkbox(f.l, &on)) { fl ^= f.bit; if (fl & f.bit) fl &= ~f.excl; }
                tooltip(f.tip);
            }
            ImGui::EndCombo();
        }
        if (nact) { ImGui::SameLine(0, S(4)); if (icon_button("fclr", ui::Icon::Eraser, "Clear all filters", false, true, ImGui::GetFrameHeight())) fl = 0; }
    }
    // counts line
    {
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_faint);
        if (!lib) {
            int used = 0, inlib = 0;
            for (auto& kv : doc->prj.packages) { if (use_comps_.count(kv.first)) ++used; auto it = psync_.find(kv.first); if (it != psync_.end() && it->second == SyncState::Same) ++inlib; }
            ImGui::Text("%d shown  \xc2\xb7  %d used  \xc2\xb7  %d synced%s", (int)rows_.size(), used, inlib, lsel_.size() > 1 ? ("  \xc2\xb7  " + std::to_string(lsel_.size()) + " selected").c_str() : "");
            const int nun = (int)unresolved_packages(doc->prj).size();   // R16b
            if (nun) {
                ImGui::PushStyleColor(ImGuiCol_Text, t.warn);
                ImGui::Text("%d unresolved (no graphics)", nun);
                ImGui::PopStyleColor();
                if (ImGui::IsItemClicked()) filt_ = (filt_ & ~2u) | 256u;
                tooltip("Click to show only the unresolved packages");
            }
        } else {
            int inproj = 0;
            for (auto& kv : lsync_) if (kv.second != SyncState::LibraryOnly) ++inproj;
            ImGui::Text("%d shown  \xc2\xb7  %d in project%s", (int)lrows_.size(), inproj, llsel_.size() > 1 ? ("  \xc2\xb7  " + std::to_string(llsel_.size()) + " selected").c_str() : "");
        }
        ImGui::PopStyleColor();
    }
    const float footer = lib ? S(92) : S(46);
    draw_items(lib ? lrows_ : rows_, lib, footer);
    // footer: transfer + actions (transfer / conflict policy only in the Library Manager)
    ImGui::Separator();
    if (lib) {
        const int nsel = (int)(lib ? mlib_targets().size() : lib_targets().size());
        const bool ro_master = LIB().master.read_only;
        float bw = ImGui::GetContentRegionAvail().x;
        std::string lbl;
        if (!lib) lbl = nsel > 1 ? "Send " + std::to_string(nsel) + " to Library  \xe2\x96\xb6" : "Send to Library  \xe2\x96\xb6";
        else lbl = nsel > 1 ? "\xe2\x97\x80  Pull " + std::to_string(nsel) + " into Project" : "\xe2\x97\x80  Pull into Project";
        if (button(lbl.c_str(), ui::Icon::None, true, ImVec2(bw * 0.66f, 0), nsel > 0 && !(!lib && ro_master))) act_transfer(!lib);
        (void)ro_master;
        tooltip(lib ? "Copy the selected library packages into this project" : "Copy the selected project packages into the master library");
        ImGui::SameLine(0, S(4));
        ImGui::SetNextItemWidth(-1);
        int pi = (int)policy_;
        if (ImGui::Combo("##policy", &pi, kPolicyNames, 4)) policy_ = (ConflictAction)pi;
        tooltip("On conflict - when a package with the same name but different content already exists:\n"
                "Ask -  - show both and choose (with 'apply to all')\nOverwrite - replace the existing copy\n"
                "Skip - keep the existing copy\nKeep both - add the incoming one under a new name");
    }
    ImGui::Dummy(ImVec2(0, S(1)));
    if (!lib) {
        if (gbtn("new", G::NewPkg, "New blank package")) act_new();
        ImGui::SameLine(0, S(2));
        if (gbtn("tpl", G::Template, "Create from template (opens the Templates tab)")) show_templates();
        ImGui::SameLine(0, S(2));
        if (gbtn("dup", G::Duplicate, "Duplicate selected", false, !lsel_.empty())) act_duplicate();
        ImGui::SameLine(0, S(2));
        if (icon_button("del", ui::Icon::Delete, "Delete unused selected packages", false, !lsel_.empty())) act_delete_unused();
        ImGui::SameLine(0, S(8));
        if (gbtn("imp", G::Import, "Import packages from a file...\n(.pcbp, legacy .cpf / .mdb)")) act_import();
        ImGui::SameLine(0, S(2));
        if (gbtn("exp", G::Export, "Export packages to a .pcbp file...")) act_export(false);
    } else {
        const bool ro = curlib().read_only;
        if (icon_button("ldel", ui::Icon::Delete, "Delete selected packages from the master library", false, !ro && !mlib_targets().empty())) act_lib_delete();
        ImGui::SameLine(0, S(2));
        if (gbtn("lsrc", G::Import, "Open a read-only source library (legacy .mdb / .cpf, .pcbplib)")) act_lib_open_source();
        ImGui::SameLine(0, S(2));
        if (gbtn("lmaster", G::Library, "Open another master library file...")) act_lib_choose_master(false);
        ImGui::SameLine(0, S(8));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", ro ? "read-only source" : "auto-saved");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    // trim thumbnail cache
    if (thumbs_.size() > 700)
        for (auto it = thumbs_.begin(); it != thumbs_.end();) it = (frame_ - it->second->last > 240) ? thumbs_.erase(it) : std::next(it);
}

void PackagesView::draw_lib_context() {
    if (!ImGui::BeginPopup("##libctx")) return;
    auto item = [&](const char* label, G g, bool en = true) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool r = ImGui::MenuItem((std::string("      ") + label).c_str(), nullptr, false, en);
        glyph(ImGui::GetWindowDrawList(), g, ImVec2(p.x, p.y + 1), ImGui::GetFontSize(), en ? 1.f : 0.4f);
        return r;
    };
    const int n = (int)lib_targets().size();
    int used = 0;
    for (auto& s : lib_targets()) if (use_comps_.count(s)) ++used;
    ImGui::PushStyleColor(ImGuiCol_Text, th().text_dim);
    if (n == 1) ImGui::TextUnformatted(cur_.c_str()); else ImGui::Text("%d packages", n);
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (item("Rename...", G::Rename, n == 1)) act_rename();
    if (item("Duplicate", G::Duplicate)) act_duplicate();
    if (item("Merge into...", G::Merge)) act_merge();
    if (item("Merge similar packages...", G::Merge)) open_merge_similar();
    if (item(used == n ? "Delete unused (all in use)" : "Delete unused", G::NewPkg, used < n)) act_delete_unused();
    ImGui::Separator();
    if (item("Show users in plotter", G::Users, used > 0)) act_show_users();
    ImGui::Separator();
    if (item("Send to master library", G::ToLib, !LIB().master.read_only)) act_transfer(true);
    if (item("Export selection to file...", G::Export)) act_export(true);
    ImGui::EndPopup();
}

void PackagesView::draw_mlib_context() {
    if (!ImGui::BeginPopup("##mlibctx")) return;
    auto item = [&](const char* label, G g, bool en = true) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool r = ImGui::MenuItem((std::string("      ") + label).c_str(), nullptr, false, en);
        glyph(ImGui::GetWindowDrawList(), g, ImVec2(p.x, p.y + 1), ImGui::GetFontSize(), en ? 1.f : 0.4f);
        return r;
    };
    auto names = mlib_targets();
    ImGui::PushStyleColor(ImGuiCol_Text, th().text_dim);
    if (names.size() == 1) ImGui::TextUnformatted(names[0].c_str()); else ImGui::Text("%d packages", (int)names.size());
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (item("Pull into project", G::FromLib)) act_transfer(false);
    bool one_in_proj = names.size() == 1 && doc->prj.packages.count(names[0]);
    if (item("Show project copy", G::Project, one_in_proj)) { mode_ = 0; select_package(names[0]); scroll_to_cur_ = true; }
    ImGui::Separator();
    if (item("Delete from library", G::NewPkg, !curlib().read_only)) act_lib_delete();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// drawing: editor (centre)
// ---------------------------------------------------------------------------
void PackagesView::draw_flags_row() {}   // R18c: replaced by draw_header_bar() + Graphics > BODY STYLE

void PackagesView::rename_current(const std::string& n) {
    Package* p = pkg();
    if (!p || n.empty() || n == cur_) return;
    if (doc->prj.packages.count(n)) { say("A package named " + n + " already exists", ToastKind::Warning); return; }
    const std::string old = cur_;
    doc->checkpoint("Rename package");
    Package pk = *p;
    pk.name = n;
    doc->prj.packages.erase(old);
    doc->prj.packages[n] = pk;
    for (auto& c : doc->prj.comps) if (c.package == old) c.package = n;
    for (auto& kv : doc->prj.parts) if (kv.second.package == old) kv.second.package = n;
    for (auto& mp : doc->prj.model.points) for (auto& f : mp.filter_packages) if (f == old) f = n;
    thumbs_.erase(old);
    cur_ = n;
    lsel_ = {n};
    doc->changed(ui::CH_GEOMETRY);
    scroll_to_cur_ = true;
}

static const char* kMixed = "\xe2\x80\x94";   // em dash: selected packages differ

void PackagesView::draw_header_bar(float w) {
    Package* p = pkg();
    if (!p) return;
    const std::vector<std::string> names = targets();
    const bool many = names.size() > 1;
    auto& P = doc->prj.packages;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(6), S(4)));
    if (many) {
        ImGui::PushFont(g_font_bold);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d packages", (int)names.size());
        ImGui::PopFont();
        tooltip("Multi-selection (Ctrl / Shift-click in the list, Ctrl+A for all shown).\nEach edit applies only that property to every selected package, as one undo step.\nMixed values show \xe2\x80\x94.");
    } else {
        char buf[256];
        snprintf(buf, sizeof buf, "%s", cur_.c_str());
        ImGui::PushFont(g_font_bold);
        ImGui::SetNextItemWidth(std::min(S(240), w * 0.28f));
        ImGui::InputText("##hname", buf, sizeof buf, ImGuiInputTextFlags_AutoSelectAll);
        const bool done = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::PopFont();
        auto it = use_comps_.find(cur_);
        tooltip(("Package name (edit + Enter renames; placements and part numbers follow)\n" + std::to_string(p->pads.size()) + " leads, used by " +
                 std::to_string(it == use_comps_.end() ? 0 : it->second) + " placement(s)").c_str());
        if (done) { rename_current(trim(buf)); p = pkg(); if (!p) { ImGui::PopStyleVar(); return; } }
    }
    // process: SMT / through-hole (auto-detected from drilled leads on import, editable here)
    int nth = 0, nsmt = 0, npol = 0, ndet = 0;
    bool hmixed = false;
    for (auto& n : names) {
        const Package& q = P[n];
        (is_th(q) ? nth : nsmt)++;
        npol += pkg_has_polarity(q);
        ndet += pkg_detect_th(q) != is_th(q);
        hmixed = hmixed || std::fabs(q.height - p->height) > 1e-9;
    }
    ImGui::SameLine(0, S(16));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Process");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(122));
    if (ImGui::BeginCombo("##hproc", nth && nsmt ? kMixed : nth ? "Through-hole" : "SMT")) {
        if (ImGui::Selectable("SMT", nsmt && !nth)) multi_edit("Process: SMT", [](Package& q) { q.smt = true; });
        if (ImGui::Selectable("Through-hole", nth && !nsmt)) multi_edit("Process: through-hole", [](Package& q) { q.smt = false; });
        ImGui::Separator();
        if (ImGui::Selectable("Detect from drilled leads")) multi_edit("Process: detect from drilled leads", [](Package& q) { q.smt = !pkg_detect_th(q); });
        ImGui::EndCombo();
    }
    tooltip((std::string("SMT or through-hole. Detected on import from drilled leads / board holes (never from the name).\n") +
             (many ? std::to_string(ndet) + " of " + std::to_string(names.size()) + " selected differ from their drilled-lead detection."
                   : std::string("Drilled-lead detection for this package: ") + (pkg_detect_th(*p) ? "through-hole" : "SMT") + ".")).c_str());
    // polarity (P)
    ImGui::SameLine(0, S(16));
    {
        bool v = npol == (int)names.size();
        const bool mixed = npol && npol < (int)names.size();
        if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        if (ImGui::Checkbox("Polarity", &v)) toggle_polarity();
        if (mixed) ImGui::PopItemFlag();
        std::string why;
        const bool def = pkg_default_polarity_reason(*p, &why) > 0;
        tooltip(("Has polarity marker  [P]\nThe part has an orientation that must match the board silkscreen indicator.\n"
                 "On: a red dot is placed near lead 1. Mixed (\xe2\x80\x94): click adds a marker to the selected packages without one.\n" +
                 (many ? std::string() : "Default for this package: " + std::string(def ? "yes" : "no") + " (" + why + ").\n") +
                 "Information only: machine programs are not changed.").c_str());
    }
    // height
    ImGui::SameLine(0, S(16));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Height");
    ImGui::SameLine();
    {
        char hb[48];
        snprintf(hb, sizeof hb, "%s", hmixed ? "" : fmt_n(p->height).c_str());
        ImGui::SetNextItemWidth(S(78));
        ImGui::InputTextWithHint("##hheight", hmixed ? kMixed : "", hb, sizeof hb);
        if (ImGui::IsItemDeactivatedAfterEdit() && hb[0]) {
            const double nv = std::max(0.0, app::parse_len(ui::W(hb), p->height));
            multi_edit("Package height", [nv](Package& q) { q.height = nv; q.height_src = nv > 0 ? "user" : ""; });
        }
        // R24a (user v23 #2): say where the height came from; 0 = not in the data (machine height left unset)
        const std::string hs = hmixed ? std::string() : p->height <= 0 ? "not in the CAD data" : p->height_src == "cad" ? "from CAD"
                             : p->height_src == "user" ? "entered here" : "source unknown";
        tooltip(("Component height, mm (written to the machine program's component height; 0 = left unset).\n" +
                 (hs.empty() ? std::string("Mixed values in the selection.") : "Height: " + hs + ".") +
                 "\nThe program's component size is the BODY outline (legs excluded); without a body outline the lead extent is used.").c_str());
        if (!hmixed) {
            ImGui::SameLine(0, S(4));
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(p->height <= 0 ? th().warn : th().text_dim), "%s",
                               p->height <= 0 ? "(none)" : p->height_src == "cad" ? "CAD" : p->height_src == "user" ? "edited" : "");
        }
    }
    if (!many) {   // body outline provenance (REVIEW_V9 6A)
        ImGui::SameLine(0, S(16));
        const bool approx = p->body_source == "pads" || p->body_source == "cad_extent";
        ImGui::AlignTextToFramePadding();
        const char* bs = p->body_source == "cad" ? "CAD outline" : p->body_source == "cad_extent" ? "CAD box" : p->body_source == "pads" ? "suggested" : "drawn";
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(approx ? th().warn : th().text_dim), "Body: %s", bs);
        tooltip(p->body_source == "cad" ? "Body outline read from the CAD data (package outline records)."
                : p->body_source == "cad_extent" ? "Only the package extent box was in the CAD data: the outline is a rectangle, not the real body shape."
                : p->body_source == "pads" ? "Suggested from pads (approximate, no outline in the source).\nGraphics tab > BODY STYLE shapes / re-suggests it."
                : "Drawn or edited in PCB Plotter, or from an older project without provenance.");
    }
    ImGui::PopStyleVar();
}

// R18c: the old Radial / J-Lead / Notch toggles only shaped the body that "Render" suggests from the leads.
// They are one choice now. Render = "Suggest body from leads" (B); Auto = re-suggest when the style changes.
void PackagesView::draw_body_style() {
    Package* p = pkg();
    if (!p) return;
    const std::vector<std::string> names = targets();
    auto style_of = [](const Package& q) { return q.radial ? 3 : q.j_lead ? 2 : q.notched ? 1 : 0; };
    static const char* kStyles[] = {"Rectangle", "Rectangle, pin-1 notch", "J-lead (hugs the leads)", "Radial (round)"};
    int st = style_of(*p);
    bool mixed = false;
    for (auto& n : names) mixed = mixed || style_of(doc->prj.packages[n]) != st;
    section_header("BODY STYLE");
    prop_label("Style");
    if (ImGui::BeginCombo("##bstyle", mixed ? kMixed : kStyles[st])) {
        for (int k = 0; k < 4; ++k)
            if (ImGui::Selectable(kStyles[k], !mixed && k == st)) {
                const bool ar = auto_render_;
                int kept = 0;
                for (auto& n : names) kept += ar && !pkg_body_replaceable(doc->prj.packages[n]);
                multi_edit("Body style", [k, ar](Package& q) {
                    q.notched = k == 1; q.j_lead = k == 2; q.radial = k == 3;
                    if (ar) render_body(q);      // never replaces a CAD / drawn outline
                });
                if (kept) say(std::to_string(kept) + " real body outline(s) kept: the style only shapes suggested bodies", ToastKind::Info);
            }
        ImGui::EndCombo();
    }
    tooltip("Shape of the body suggested from the leads:\nRectangle, rectangle with a notch at the pin-1 end, J-lead (outline hugs the leads), or radial (round can).\nA body read from CAD or drawn by hand is never replaced by a style change.");
    if (button("Suggest body from leads", ui::Icon::None, false, ImVec2(-1, 0))) {
        if (names.size() == 1) auto_body();
        else {
            int kept = 0;
            for (auto& n : names) kept += !pkg_body_replaceable(doc->prj.packages[n]);
            multi_edit("Suggest body from leads", [](Package& q) { render_body(q); });
            if (kept) say(std::to_string(kept) + " package(s) with a CAD / drawn outline kept", ToastKind::Info);
        }
    }
    tooltip("(B) Draw an approximate body from the leads and the style.\nOnly fills an empty or earlier suggested body; a CAD / drawn outline is kept (single package: asks first).");
    ImGui::Checkbox("Re-suggest when the style changes", &auto_render_);
    tooltip("Auto: a style change redraws a suggested body at once.\nNever replaces a CAD or drawn outline.");
    ImGui::Dummy(ImVec2(0, S(6)));
}

void PackagesView::draw_edit_toolbar(float w) {
    const GTheme& t = th();
    const bool has = pkg() != nullptr;
    const float bs = S(26);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(o, o + ImVec2(w, bs + S(24)), t.panel2);
    dl->AddLine(o + ImVec2(0, bs + S(24) - 1), o + ImVec2(w, bs + S(24) - 1), t.border);
    ImGui::SetCursorScreenPos(o + ImVec2(S(10), S(3)));
    // groups: label above, buttons below
    struct Grp { const char* label; float x0; };
    std::vector<Grp> groups;
    auto begin_group = [&](const char* label) {
        if (!groups.empty()) {
            ImGui::SameLine(0, S(5));
            ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(p.x, o.y + S(6)), ImVec2(p.x, o.y + bs + S(18)), t.border);
            ImGui::Dummy(ImVec2(S(1), 1));
            ImGui::SameLine(0, S(5));
        }
        groups.push_back({label, ImGui::GetCursorScreenPos().x});
    };
    auto next = [&]() { ImGui::SameLine(0, S(2)); };
    ImGui::SetCursorScreenPos(o + ImVec2(S(10), S(18)));
    ImGui::BeginDisabled(!has);
    begin_group("EDIT");
    if (icon_button("t_sel", ui::Icon::Select, "Select / move (Esc)\nClick or box-select leads and graphics; drag to move, arrows nudge", tool_ == Tool::Select, has, bs)) set_tool(Tool::Select);
    next(); if (gbtn("t_add", G::AddPad, "Add lead (A): click to place; copies the selected lead", tool_ == Tool::AddPad, has, bs)) set_tool(Tool::AddPad);
    next(); if (gbtn("t_arr", G::Array, "Lead array / row generator...", false, has, bs)) pad_array();
    next(); if (icon_button("t_del", ui::Icon::Delete, "Delete selected leads / graphics (Del)", false, has && (!psel_.empty() || !gsel_.empty()), bs)) delete_selection();
    begin_group("DRAW");
    if (icon_button("t_line", ui::Icon::Line, "Line: drag", tool_ == Tool::DrawLine, has, bs)) set_tool(Tool::DrawLine);
    next(); if (icon_button("t_rect", ui::Icon::Rect, "Rectangle: drag corner to corner", tool_ == Tool::DrawRect, has, bs)) set_tool(Tool::DrawRect);
    next(); if (icon_button("t_circ", ui::Icon::Circle, "Circle: drag from the centre", tool_ == Tool::DrawCircle, has, bs)) set_tool(Tool::DrawCircle);
    next(); if (gbtn("t_arc", G::Arc, "Arc: drag centre to start point\n(sweep set in the Graphics tab, default 180\xc2\xb0 CCW)", tool_ == Tool::DrawArc, has, bs)) set_tool(Tool::DrawArc);
    next(); if (gbtn("t_poly", G::Poly, "Polyline: click points; double-click or Enter to finish,\nclick the first point to close a polygon, Esc cancels", tool_ == Tool::DrawPoly, has, bs)) set_tool(Tool::DrawPoly);
    next(); if (icon_button("t_text", ui::Icon::Text, "Text (T): click to place", tool_ == Tool::DrawText, has, bs)) set_tool(Tool::DrawText);
    next();
    {
        static const double widths[] = {0, 0.05, 0.1, 0.12, 0.15, 0.2, 0.25, 0.3, 0.5};
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, o.y + S(18) + (bs - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::SetNextItemWidth(S(98));
        std::string cur = stroke_ <= 0 ? std::string("Hairline") : fmt_len_u8(stroke_);
        if (ImGui::BeginCombo("##stroke", cur.c_str())) {
            for (double sw : widths) if (ImGui::Selectable(sw <= 0 ? "Hairline" : fmt_len_u8(sw).c_str(), std::fabs(sw - stroke_) < 1e-9)) {
                stroke_ = sw;
                if (Package* p = pkg(); p && !gsel_.empty()) { doc->checkpoint("Stroke width"); for (int i : gsel_) if (i < (int)p->body.size()) p->body[i].width = sw; edited(); }
            }
            ImGui::EndCombo();
        }
        tooltip("Stroke width for new graphics (also applied to selected graphics)");
        next();
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, o.y + S(18)));
        if (icon_button("t_fill", ui::Icon::FillSolid, "Filled rectangles / circles / polygons", fill_, has, bs)) fill_ = !fill_;
    }
    begin_group("ORIGIN");
    if (gbtn("t_org", G::OriginPick, "Set origin: click a point or lead centre", tool_ == Tool::SetOrigin, has, bs)) set_tool(Tool::SetOrigin);
    next(); if (gbtn("t_orgm", G::Centre, "Move origin to...", false, has, bs)) ImGui::OpenPopup("##orgpop");
    if (ImGui::BeginPopup("##orgpop")) {
        if (menu_item_icon("Centroid of leads", ui::Icon::VirtualOrigin, nullptr, false, has)) set_origin(OriginMode::LeadCentroid);
        if (menu_item_icon("Centre of lead extent", ui::Icon::Offset, nullptr, false, has)) set_origin(OriginMode::PadCentre);
        if (menu_item_icon("Pin 1", ui::Icon::PinOne, nullptr, false, has)) set_origin(OriginMode::Pin1);
        if (menu_item_icon("Body centre", ui::Icon::Rect, nullptr, false, has)) set_origin(OriginMode::BodyCentre);
        ImGui::Separator();
        // CircuitCAM "Offset Part Origin": dx/dy relative to the CURRENT origin
        ImGui::TextDisabled("Offset Part Origin (from current origin, mm)");
        ImGui::SetNextItemWidth(S(90)); ImGui::InputDouble("dx##odx", &off_dx_, 0, 0, "%.4f");
        ImGui::SameLine(); ImGui::SetNextItemWidth(S(90)); ImGui::InputDouble("dy##ody", &off_dy_, 0, 0, "%.4f");
        if (ImGui::Button("Offset origin") && (off_dx_ != 0 || off_dy_ != 0)) { set_origin(OriginMode::Click, {off_dx_, off_dy_}); ImGui::CloseCurrentPopup(); }
        ImGui::Separator();
        ImGui::MenuItem("Keep placements in place (compensate positions)", nullptr, &compensate_);
        if (doc) { std::string w = shared_warning(doc->prj, cur_); if (!w.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(th().warn), "%s", w.c_str()); }
        ImGui::EndPopup();
    }
    begin_group("TRANSFORM");
    if (icon_button("t_rot", ui::Icon::Rot90, "Rotate package 90\xc2\xb0 CCW (R)", false, has, bs)) rotate_package();
    next(); if (icon_button("t_rotn", ui::Icon::Rot270, "Rotate package by any angle...\n(normalise imported off-axis artwork)", false, has, bs)) {
        if (const Package* pp = pkg()) { AxisInfo ai = pkg_axis_info(*pp); rot_dlg_ = ai.off_axis ? -ai.off90 : -30; }
        ImGui::OpenPopup("##rotn");
    }
    draw_rotate_popup();
    next(); if (gbtn("t_mir", G::Mirror, "Mirror package left-right", false, has, bs)) mirror_package();
    next(); if (gbtn("t_ren", G::Renumber, "Renumber leads...", false, has, bs)) ImGui::OpenPopup("##renpop");
    if (ImGui::BeginPopup("##renpop")) {
        if (menu_item_icon("Counter-clockwise from pin 1", ui::Icon::Rot90)) renumber(0);
        if (menu_item_icon("Row by row (top-left first)", ui::Icon::ArrowDown)) renumber(1);
        if (menu_item_icon("Column by column", ui::Icon::Arrow)) renumber(2);
        ImGui::Separator();
        if (menu_item_icon("Custom click order...", ui::Icon::SelectPart)) renumber(3);
        ImGui::EndPopup();
    }
    next(); if (icon_button("t_p1", ui::Icon::PinOne, "Set pin 1: click a lead", tool_ == Tool::SetPin1, has, bs)) set_tool(Tool::SetPin1);
    next(); if (gbtn("t_ord", G::PinOrder, "Pin order: click leads in sequence", tool_ == Tool::PinOrder, has, bs)) set_tool(Tool::PinOrder);
    begin_group("VIEW");
    ImGui::EndDisabled();
    if (gbtn("t_meas", G::Measure, "Measure (M): drag between points", tool_ == Tool::Measure, has, bs)) set_tool(Tool::Measure);
    next(); if (icon_button("t_za", ui::Icon::ZoomAll, "Zoom all (F)", false, true, bs)) canvas_.zoom_all();
    for (auto& g : groups) dl->AddText(g_font, S(10), ImVec2(g.x0, o.y + S(4)), t.text_faint, g.label);
    ImGui::SetCursorScreenPos(o + ImVec2(0, bs + S(24)));
}

// P2-01 vertical tool strip beside the canvas: select / leads / array / delete, draw (flyout), origin, rotate
// 0-90-180-270 / any / mirror, pin numbering. Two columns when the editor is short (high UI scale).
void PackagesView::draw_vtoolbar(float h) {
    const GTheme& t = th();
    const bool has = pkg() != nullptr;
    const float bs = SI(26), gap = S(4);
    const int cols = h < S(560) ? 2 : 1;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int k = 0;
    float y = o.y + S(6);
    auto place = [&]() {
        const int c = k % cols;
        if (c == 0 && k > 0) y += bs + gap;
        ImGui::SetCursorScreenPos(ImVec2(o.x + S(4) + c * (bs + gap), y));
        ++k;
    };
    auto sep = [&]() {
        if (k % cols) k += cols - k % cols;   // finish the row
        y += bs + gap + S(3);
        dl->AddLine(ImVec2(o.x + S(6), y - S(3) - gap * 0.5f), ImVec2(o.x + S(4) + cols * (bs + gap) - S(6), y - S(3) - gap * 0.5f), t.border);
        y -= bs + gap;
    };
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    place(); if (icon_button("v_sel", ui::Icon::Select, "Select / move (Esc)\nClick or box-select leads and graphics; drag to move, arrows nudge", tool_ == Tool::Select, has, bs)) set_tool(Tool::Select);
    place(); if (gbtn("v_add", G::AddPad, "Add lead (A): click to place; copies the selected lead", tool_ == Tool::AddPad, has, bs)) set_tool(Tool::AddPad);
    place(); if (gbtn("v_arr", G::Array, "Lead array / row generator...", false, has, bs)) pad_array();
    place(); if (icon_button("v_del", ui::Icon::Delete, "Delete selected leads / graphics (Del)", false, has && (!psel_.empty() || !gsel_.empty()), bs)) delete_selection();
    sep();
    {   // draw tools: one button (last used tool) + flyout with every tool, fill and stroke width
        const bool drawing = is_draw_tool(tool_);
        const Tool dt = drawing ? tool_ : last_draw_;
        place();
        ImVec2 bp = ImGui::GetCursorScreenPos();
        bool clicked = false;
        switch (dt) {
        case Tool::DrawRect: clicked = icon_button("v_draw", ui::Icon::Rect, "Draw (click: rectangle, right-click: all drawing tools)", drawing, has, bs); break;
        case Tool::DrawCircle: clicked = icon_button("v_draw", ui::Icon::Circle, "Draw (click: circle, right-click: all drawing tools)", drawing, has, bs); break;
        case Tool::DrawArc: clicked = gbtn("v_draw", G::Arc, "Draw (click: arc, right-click: all drawing tools)", drawing, has, bs); break;
        case Tool::DrawPoly: clicked = gbtn("v_draw", G::Poly, "Draw (click: polyline, right-click: all drawing tools)", drawing, has, bs); break;
        case Tool::DrawText: clicked = icon_button("v_draw", ui::Icon::Text, "Draw (click: text, right-click: all drawing tools)", drawing, has, bs); break;
        default: clicked = icon_button("v_draw", ui::Icon::Line, "Draw (click: line, right-click: all drawing tools)", drawing, has, bs); break;
        }
        if (clicked) set_tool(dt);
        if (has && ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##drawfly");
        dl->AddTriangleFilled(bp + ImVec2(bs - S(2), bs - S(7)), bp + ImVec2(bs - S(2), bs - S(2)), bp + ImVec2(bs - S(7), bs - S(2)), t.text_dim);
        place();
        if (icon_button("v_drawm", ui::Icon::FillSolid, "Drawing tools, fill and stroke width", fill_, has, bs)) ImGui::OpenPopup("##drawfly");
        if (ImGui::BeginPopup("##drawfly")) {
            struct D { const char* l; Tool tl; } ds[] = {{"Line", Tool::DrawLine}, {"Rectangle", Tool::DrawRect}, {"Circle", Tool::DrawCircle},
                                                         {"Arc (centre, start; sweep in Graphics)", Tool::DrawArc}, {"Polyline / polygon", Tool::DrawPoly}, {"Text (T)", Tool::DrawText}};
            for (auto& d : ds) if (ImGui::MenuItem(d.l, nullptr, tool_ == d.tl)) { set_tool(d.tl); last_draw_ = d.tl; }
            ImGui::Separator();
            ImGui::MenuItem("Filled rectangles / circles / polygons", nullptr, &fill_);
            static const double widths[] = {0, 0.05, 0.1, 0.12, 0.15, 0.2, 0.25, 0.3, 0.5};
            if (ImGui::BeginMenu("Stroke width")) {
                for (double sw : widths) if (ImGui::MenuItem(sw <= 0 ? "Hairline" : fmt_len_u8(sw).c_str(), nullptr, std::fabs(sw - stroke_) < 1e-9)) {
                    stroke_ = sw;
                    if (Package* pp = pkg(); pp && !gsel_.empty()) { doc->checkpoint("Stroke width"); for (int i : gsel_) if (i < (int)pp->body.size()) pp->body[i].width = sw; edited(); }
                }
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
        if (drawing) last_draw_ = tool_;
    }
    sep();
    place(); if (gbtn("v_org", G::OriginPick, "Set origin: click a point or lead centre", tool_ == Tool::SetOrigin, has, bs)) set_tool(Tool::SetOrigin);
    place(); if (gbtn("v_orgm", G::Centre, "Move origin to (centroid, pin 1, body centre) / offset...", false, has, bs)) ImGui::OpenPopup("##orgpop");
    if (ImGui::BeginPopup("##orgpop")) {
        if (menu_item_icon("Centroid of leads", ui::Icon::VirtualOrigin, nullptr, false, has)) set_origin(OriginMode::LeadCentroid);
        if (menu_item_icon("Centre of lead extent", ui::Icon::Offset, nullptr, false, has)) set_origin(OriginMode::PadCentre);
        if (menu_item_icon("Pin 1", ui::Icon::PinOne, nullptr, false, has)) set_origin(OriginMode::Pin1);
        if (menu_item_icon("Body centre", ui::Icon::Rect, nullptr, false, has)) set_origin(OriginMode::BodyCentre);
        ImGui::Separator();
        ImGui::TextDisabled("Offset Part Origin (from current origin, mm)");
        ImGui::SetNextItemWidth(S(90)); ImGui::InputDouble("dx##odx", &off_dx_, 0, 0, "%.4f");
        ImGui::SameLine(); ImGui::SetNextItemWidth(S(90)); ImGui::InputDouble("dy##ody", &off_dy_, 0, 0, "%.4f");
        if (ImGui::Button("Offset origin") && (off_dx_ != 0 || off_dy_ != 0)) { set_origin(OriginMode::Click, {off_dx_, off_dy_}); ImGui::CloseCurrentPopup(); }
        ImGui::Separator();
        ImGui::MenuItem("Keep placements in place (compensate positions)", nullptr, &compensate_);
        if (doc) { std::string w = shared_warning(doc->prj, cur_); if (!w.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(th().warn), "%s", w.c_str()); }
        ImGui::EndPopup();
    }
    sep();
    {   // rotate to 0 / 90 / 180 / 270 (0 = undo the rotations applied since the package was opened), any angle, mirror
        const double acc = rot_acc_.count(cur_) ? rot_acc_[cur_] : 0.0;
        struct RB { const char* id; ui::Icon ic; double to; const char* tip; } rb[] = {
            {"v_r0", ui::Icon::Undo, 0, "Rotate back to 0\xc2\xb0 (undo the rotations applied since this package was opened)"},
            {"v_r90", ui::Icon::Rot90, 90, "Rotate the package 90\xc2\xb0 counter-clockwise (R)"},
            {"v_r180", ui::Icon::Rot180, 180, "Rotate the package 180\xc2\xb0"},
            {"v_r270", ui::Icon::Rot270, 270, "Rotate the package 270\xc2\xb0 CCW (= 90\xc2\xb0 clockwise)"}};
        for (auto& b : rb) {
            place();
            const bool en = has && (b.to != 0 || std::fabs(acc) > 1e-9);
            ImVec2 bp = ImGui::GetCursorScreenPos();
            if (icon_button(b.id, b.ic, b.tip, false, en, bs)) { if (b.to == 0) rotate_by(std::fmod(360.0 - acc, 360.0), compensate_); else rotate_by(b.to, compensate_); }
            if (b.to != 0) {   // small degree label so 90 / 180 / 270 read apart at a glance
                char lb[8]; snprintf(lb, sizeof lb, "%d", (int)b.to);
                dl->AddText(g_font, S(9), bp + ImVec2(S(1), bs - S(10)), en ? t.text_dim : t.text_faint, lb);
            }
        }
        place(); if (icon_button("v_rotn", ui::Icon::Settings, "Rotate by any angle...\n(normalise imported off-axis artwork)", false, has, bs)) {
            if (const Package* pp = pkg()) { AxisInfo ai = pkg_axis_info(*pp); rot_dlg_ = ai.off_axis ? -ai.off90 : -30; }
            ImGui::OpenPopup("##rotn");
        }
        draw_rotate_popup();
        place(); if (gbtn("v_mir", G::Mirror, "Mirror package left-right", false, has, bs)) mirror_package();
    }
    sep();
    place(); if (icon_button("v_p1", ui::Icon::PinOne, "Set pin 1: click a lead", tool_ == Tool::SetPin1, has, bs)) set_tool(Tool::SetPin1);
    place(); if (gbtn("v_ord", G::PinOrder, "Pin order: click leads in sequence", tool_ == Tool::PinOrder, has, bs)) set_tool(Tool::PinOrder);
    place(); if (gbtn("v_ren", G::Renumber, "Renumber leads...", false, has, bs)) ImGui::OpenPopup("##renpop");
    if (ImGui::BeginPopup("##renpop")) {
        if (menu_item_icon("Counter-clockwise from pin 1", ui::Icon::Rot90)) renumber(0);
        if (menu_item_icon("Row by row (top-left first)", ui::Icon::ArrowDown)) renumber(1);
        if (menu_item_icon("Column by column", ui::Icon::Arrow)) renumber(2);
        ImGui::Separator();
        if (menu_item_icon("Custom click order...", ui::Icon::SelectPart)) renumber(3);
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// P2-01 floating mini viewport controls (top-right of the canvas): grid, snap + step, zoom to fit, measure,
// dimension / stats overlays and layer visibility.
void PackagesView::draw_view_controls() {
    const GTheme& t = th();
    const float bs = SI(24), gap = S(3);
    const int n = 7;
    const float W = n * (bs + gap) + gap + S(54);
    const ImVec2 cb = canvas_.origin() + ImVec2(canvas_.size().x, 0);
    const ImVec2 a(cb.x - W - S(10), cb.y + S(28)), b(cb.x - S(10), a.y + bs + 2 * gap);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a + ImVec2(0, S(2)), b + ImVec2(0, S(2)), IM_COL32(0, 0, 0, t.dark ? 70 : 30), S(8));
    dl->AddRectFilled(a, b, col_alpha(t.panel, 0.92f), S(8));
    dl->AddRect(a, b, t.border, S(8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 0));
    ImGui::SetCursorScreenPos(a + ImVec2(gap, gap));
    if (icon_button("vc_grid", ui::Icon::Grid, "Show grid", canvas_.grid_on(), true, bs)) canvas_.set_grid(!canvas_.grid_on(), canvas_.grid());
    ImGui::SameLine();
    if (icon_button("vc_snap", ui::Icon::Snap, "Snap to grid", canvas_.snapping(), true, bs)) canvas_.set_snap(!canvas_.snapping());
    ImGui::SameLine();
    {
        static const double steps[] = {0.01, 0.025, 0.05, 0.1, 0.127, 0.25, 0.5, 0.635, 1.0, 1.27, 2.54};
        std::string cur = fmt_len_u8(canvas_.grid(), false);
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##vc_step", ImVec2(S(54), bs))) ImGui::OpenPopup("##gsteppop");
        const bool hv = ImGui::IsItemHovered();
        tooltip("Grid / snap step");
        if (hv) dl->AddRectFilled(p, p + ImVec2(S(54), bs), t.raised, S(5));
        ImVec2 ts = ImGui::CalcTextSize(cur.c_str());
        dl->AddText(p + ImVec2((S(54) - ts.x) * 0.5f, (bs - ts.y) * 0.5f), t.text_dim, cur.c_str());
        if (ImGui::BeginPopup("##gsteppop")) {
            for (double sv : steps) if (ImGui::Selectable(fmt_len_u8(sv).c_str(), std::fabs(sv - canvas_.grid()) < 1e-9)) canvas_.set_grid(canvas_.grid_on(), sv);
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine();
    if (icon_button("vc_fit", ui::Icon::ZoomAll, "Zoom to fit (F)", false, shown() != nullptr, bs)) canvas_.zoom_all();
    ImGui::SameLine();
    if (gbtn("vc_meas", G::Measure, "Measure (M): drag between points", tool_ == Tool::Measure, pkg() != nullptr, bs)) set_tool(Tool::Measure);
    ImGui::SameLine();
    if (icon_button("vc_dims", ui::Icon::Units, "Dimension lines (pad field width / height)", vis_dims_, true, bs)) { vis_dims_ = !vis_dims_; app::set_ui_pref("Pkg.Dims", vis_dims_); }
    ImGui::SameLine();
    if (icon_button("vc_stats", ui::Icon::About, "Package statistics card (pads, pitch, field, body)", vis_stats_, true, bs)) { vis_stats_ = !vis_stats_; app::set_ui_pref("Pkg.Stats", vis_stats_); }
    ImGui::SameLine();
    if (icon_button("vc_layers", ui::Icon::Eye, "Show: body / pads / pin numbers / origin", false, true, bs)) ImGui::OpenPopup("##vclayers");
    if (ImGui::BeginPopup("##vclayers")) {
        struct VB { const char* l; bool* v; } vb[] = {{"Body", &vis_body_}, {"Pads (and pin 1 marker)", &vis_pads_}, {"Pin numbers", &vis_pins_}, {"Origin", &vis_origin_}};
        for (auto& x : vb) if (ImGui::MenuItem(x.l, nullptr, x.v)) canvas_.invalidate();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// P1-09: the focused package list owns Up / Down / PgUp / PgDn / Home / End (selection + preview); the canvas
// only takes arrow keys while it is the focused pane.
void PackagesView::list_keys(const std::vector<std::string>& rows, bool lib) {
    if (rows.empty() || !ImGui::IsWindowFocused() || ImGui::GetIO().WantTextInput) return;
    claim_keyboard();
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {   // R18c: select every shown package
        auto& sel = lib ? llsel_ : lsel_;
        sel = std::set<std::string>(rows.begin(), rows.end());
        if (!lib && !sel.count(cur_)) { std::set<std::string> keep = sel; select_package(rows.front()); lsel_ = keep; }
        return;
    }
    if (!lib && pkg_hotkeys(false)) return;
    const std::string& cur = lib ? lcur_ : cur_;
    int i = (int)(std::find(rows.begin(), rows.end(), cur) - rows.begin());
    const int n = (int)rows.size();
    int j = i;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) j = i >= n ? n - 1 : i - 1;
    else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) j = i >= n ? 0 : i + 1;
    else if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) j = i - 10;
    else if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) j = i + 10;
    else if (ImGui::IsKeyPressed(ImGuiKey_Home)) j = 0;
    else if (ImGui::IsKeyPressed(ImGuiKey_End)) j = n - 1;
    else return;
    j = std::clamp(j, 0, n - 1);
    if (j == i) return;
    if (lib) mlib_click(rows[j]); else lib_click(rows[j]);
    scroll_to_cur_ = true;
}

void PackagesView::draw_editor(float w, float h) {
    const GTheme& t = th();
    ImGui::BeginChild("##editor", ImVec2(w, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const Package* p = shown();
    if (!p) {
        ImVec2 c = ImGui::GetWindowPos() + ImVec2(w * 0.5f, h * 0.42f);
        icon_draw(ImGui::GetWindowDrawList(), ui::Icon::PackageEditor, c - ImVec2(S(32), S(70)), S(64), 0.5f);
        const char* m = mode_ == 1 ? "Select a library package to preview it" : "Select a package, or create one";
        ImVec2 ts = ImGui::CalcTextSize(m);
        ImGui::SetCursorScreenPos(ImVec2(c.x - ts.x * 0.5f, c.y + S(4)));
        ImGui::TextDisabled("%s", m);
        if (mode_ == 0) {
            ImGui::SetCursorScreenPos(ImVec2(c.x - S(160), c.y + S(36)));
            if (button("New package", ui::Icon::New, false, ImVec2(S(150), 0))) act_new();
            ImGui::SameLine(0, S(20));
            if (button("From template", ui::Icon::PackageEditor, true, ImVec2(S(150), 0))) show_templates();
        }
        ImGui::EndChild();
        return;
    }
    const bool lib = mode_ == 1;
    const std::string& nm = lib ? lcur_ : cur_;
    // P2-01: one compact row (name + flags) instead of the header / chips / duplicate library controls and the
    // horizontal tool + rotate rows; tools sit in a vertical strip beside the canvas, view controls float on it.
    ImVec2 hp = ImGui::GetCursorScreenPos();
    const float rowh = ImGui::GetFrameHeight() + S(10);
    ImGui::GetWindowDrawList()->AddRectFilled(hp, hp + ImVec2(w, rowh), t.panel);
    ImGui::GetWindowDrawList()->AddLine(hp + ImVec2(0, rowh - 1), hp + ImVec2(w, rowh - 1), t.border);
    ImGui::SetCursorScreenPos(hp + ImVec2(S(10), S(5)));
    if (!lib) draw_header_bar(w);   // R18c: name / process / polarity / height (replaces the SMT/Radial/J-Lead/Notch row)
    else {
    ImGui::PushFont(g_font_bold);
    ImGui::AlignTextToFramePadding();
    {
        const float maxn = std::min(S(220), w * 0.3f);
        ImVec2 np = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(np, np + ImVec2(maxn, rowh), true);
        ImGui::TextUnformatted(nm.c_str());
        ImGui::PopClipRect();
        if (ImGui::IsItemHovered()) {
            auto it = use_comps_.find(cur_);
            std::string tip = nm + "\n" + std::to_string(p->pads.size()) + " leads" + (lib ? "\nLibrary preview (read-only)" : "\nUsed by " + std::to_string(it == use_comps_.end() ? 0 : it->second) + " placement(s)");
            tooltip(tip.c_str());
        }
        ImGui::SameLine(0, 0);
        ImGui::SetCursorScreenPos(ImVec2(std::min(ImGui::GetCursorScreenPos().x, np.x + maxn) + S(12), hp.y + S(5)));
    }
    ImGui::PopFont();
    ImGui::TextDisabled("Library preview (read-only)");
    }
    ImGui::SetCursorScreenPos(hp + ImVec2(0, rowh));
    const float top = ImGui::GetCursorScreenPos().y - hp.y;
    // vertical tool strip (left of the canvas)
    float tbw = 0;
    if (!lib) {
        ImVec2 tp = ImGui::GetCursorScreenPos();
        tbw = S(4) + (SI(26) + S(4)) * ((h - top) < S(560) ? 2 : 1) + S(2);   // two columns when the editor is short
        ImGui::GetWindowDrawList()->AddRectFilled(tp, tp + ImVec2(tbw, h - top), t.panel2);
        ImGui::GetWindowDrawList()->AddLine(tp + ImVec2(tbw - 1, 0), tp + ImVec2(tbw - 1, h - top), t.border);
        draw_vtoolbar(h - top);
        ImGui::SetCursorScreenPos(tp + ImVec2(tbw, 0));
    }
    const float w_canvas = w - tbw;
    // canvas
    canvas_.draw("##pkgcanvas", ImVec2(w_canvas, h - top));
    draw_view_controls();
    if (canvas_.ctx_pending) { canvas_.ctx_pending = false; if (!lib) ImGui::OpenPopup("##padctx"); }
    draw_canvas_context();
    // tool hint (top-centre pill)
    {
        const char* hint = nullptr;
        switch (tool_) {
        case Tool::AddPad: hint = "Add lead: click to place  \xc2\xb7  Esc to finish"; break;
        case Tool::Measure: hint = "Measure: drag between points (snaps to lead centres)"; break;
        case Tool::SetPin1: hint = "Set pin 1: click the lead that becomes pin 1"; break;
        case Tool::PinOrder: hint = "Pin order: click leads in sequence  \xc2\xb7  Esc to stop"; break;
        case Tool::SetOrigin: hint = "Set origin: click a point (snaps to lead centres)  \xc2\xb7  Esc to cancel"; break;
        case Tool::DrawLine: hint = "Line: drag from start to end"; break;
        case Tool::DrawRect: hint = "Rectangle: drag corner to corner"; break;
        case Tool::DrawCircle: hint = "Circle: drag from the centre outwards"; break;
        case Tool::DrawArc: hint = "Arc: drag from the centre to the start point (CCW sweep from Graphics tab)"; break;
        case Tool::DrawPoly: hint = "Polyline: click points  \xc2\xb7  double-click / Enter finishes  \xc2\xb7  click first point to close"; break;
        case Tool::DrawText: hint = "Text: click to place"; break;
        default: break;
        }
        if (hint && !lib)   // P1-07: tutorial hint (off by default), bottom centre, clear of the leads
            hint_pill(ImGui::GetWindowDrawList(), ("pkg.tool" + std::to_string((int)tool_)).c_str(), hint,
                      ImVec2(canvas_.origin().x + canvas_.size().x * 0.5f, canvas_.origin().y + canvas_.size().y - S(12)), true);
    }
    ImGui::EndChild();
}

void PackagesView::draw_canvas_context() {
    if (!ImGui::BeginPopup("##padctx")) return;
    Package* p = pkg();
    auto item = [&](const char* label, G g, bool en = true) {
        ImVec2 q = ImGui::GetCursorScreenPos();
        bool r = ImGui::MenuItem((std::string("      ") + label).c_str(), nullptr, false, en);
        glyph(ImGui::GetWindowDrawList(), g, ImVec2(q.x, q.y + 1), ImGui::GetFontSize(), en ? 1.f : 0.4f);
        return r;
    };
    auto uitem = [&](const char* label, ui::Icon ic, bool en = true) {
        ImVec2 q = ImGui::GetCursorScreenPos();
        bool r = ImGui::MenuItem((std::string("      ") + label).c_str(), nullptr, false, en);
        icon_draw(ImGui::GetWindowDrawList(), ic, ImVec2(q.x, q.y + 1), ImGui::GetFontSize(), en ? 1.f : 0.4f);
        return r;
    };
    if (p) {
        if (ImGui::IsWindowAppearing()) {
            int h = hit_pad(canvas_.ctx_world);
            if (h >= 0 && !psel_.count(h)) { psel_ = {h}; gsel_.clear(); }
            else if (h < 0) { int g = hit_prim(canvas_.ctx_world); if (g >= 0 && !gsel_.count(g)) { gsel_ = {g}; psel_.clear(); } }
        }
        const bool sel = !psel_.empty() || !gsel_.empty();
        if (item("Add lead here", G::AddPad)) add_pad(canvas_.snap_pt(canvas_.ctx_world));
        if (uitem("Make pin 1", ui::Icon::PinOne, psel_.size() == 1)) set_pin1(*psel_.begin());
        if (item("Rotate selected leads 90\xc2\xb0", G::RotPad, !psel_.empty())) rotate_pads();
        if (uitem("Delete selected", ui::Icon::Delete, sel)) delete_selection();
        if (uitem("Copy leads", ui::Icon::Copy, !psel_.empty())) copy_pads();
        if (uitem("Paste leads", ui::Icon::Paste, !clip_.empty())) paste_pads();
        ImGui::Separator();
        if (item("Set origin here", G::OriginPick)) { int k = hit_pad(canvas_.ctx_world, 6); set_origin(OriginMode::Click, k >= 0 ? p->pads[k].pos : canvas_.snap_pt(canvas_.ctx_world)); }
        if (item("Origin to lead centroid", G::Centre)) set_origin(OriginMode::LeadCentroid);
        if (item("Origin to pin 1", G::Centre)) set_origin(OriginMode::Pin1);
        if (item("Origin to body centre", G::Centre)) set_origin(OriginMode::BodyCentre);
        ImGui::Separator();
        if (uitem("Rotate package 90\xc2\xb0", ui::Icon::Rot90)) rotate_package();
        if (item("Mirror package", G::Mirror)) mirror_package();
        if (item("Renumber CCW from pin 1", G::Renumber)) renumber(0);
        if (item("Render body", G::AutoBody)) auto_body();
        ImGui::Separator();
        if (uitem("Zoom all", ui::Icon::ZoomAll)) canvas_.zoom_all();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// drawing: inspector (right)
// ---------------------------------------------------------------------------
// ---- rotate-by-angle (package-local transform) with before/after preview ----
void PackagesView::draw_rotate_popup() {
    if (!ImGui::BeginPopup("##rotn")) return;
    Package* p = pkg();
    if (!p) { ImGui::EndPopup(); return; }
    const int n = count_placements(doc->prj, cur_);
    const AxisInfo ai = pkg_axis_info(*p);
    ImGui::TextUnformatted("Rotate artwork + leads about the package origin");
    if (ai.off_axis) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(th().warn), "Detected off-axis %s: %+.2f\xc2\xb0 (normalise with %+.2f\xc2\xb0)", ai.why.c_str(), ai.off90, -ai.off90);
    ImGui::SetNextItemWidth(S(110));
    ImGui::InputDouble("degrees CCW##rd", &rot_dlg_, 1, 15, "%.3f");
    ImGui::RadioButton("Compensate placements (board unchanged)", &rot_mode_, 0);
    ImGui::RadioButton("Keep placements (board changes)", &rot_mode_, 1);
    // preview: before | after
    const float pw = S(150), ph = S(110);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(a, a + ImVec2(pw, ph), th().panel2, S(4));
    draw_pkg_dl(dl, *p, a + ImVec2(S(6), S(6)), a + ImVec2(pw - S(6), ph - S(6)), true);
    dl->AddText(a + ImVec2(S(6), S(2)), th().text_faint, "before");
    Package after = *p;
    pkg_geom_rotate(after, rot_dlg_);
    ImVec2 b = a + ImVec2(pw + S(8), 0);
    dl->AddRectFilled(b, b + ImVec2(pw, ph), th().panel2, S(4));
    draw_pkg_dl(dl, after, b + ImVec2(S(6), S(6)), b + ImVec2(pw - S(6), ph - S(6)), true);
    dl->AddText(b + ImVec2(S(6), S(2)), th().text_faint, "after");
    ImGui::Dummy(ImVec2(pw * 2 + S(8), ph));
    if (n) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(th().warn), "%s%s", shared_warning(doc->prj, cur_).c_str(),
                              rot_mode_ == 0 ? ": their rotation is adjusted so pads stay put" : ": they will be drawn rotated");
    else ImGui::TextDisabled("No placements use this package");
    if (ImGui::Button("Apply") && std::fabs(rot_dlg_) > 1e-9) { rotate_by(rot_dlg_, rot_mode_ == 0); ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---- R15: POLARITY MARKER (one concept; replaces the pin-1 mark / polarized checkboxes) ----
// ---------------------------------------------------------------------------
// R16b: unresolved packages (REVIEW_V15 §1) and tape orientation (§6)
// ---------------------------------------------------------------------------
void PackagesView::load_from_library(const std::string& target, const std::string& lib_name) {
    auto& L = LIB().master.packages;
    auto it = L.find(lib_name);
    if (it == L.end()) return;
    doc->checkpoint("Load " + target + " from library");
    const int n = assign_package_geometry(doc->prj, target, it->second, "library");
    thumbs_.erase(target);
    doc->changed(ui::CH_GEOMETRY);
    say(target + ": graphics of library package " + lib_name + " applied to " + std::to_string(n) + " placement(s)");
}

int PackagesView::resolve_exact_library(bool dry_run) {
    int n = 0;
    const auto un = unresolved_packages(doc->prj);
    for (auto& name : un) {
        const Package* m = library_exact_match(LIB().master.packages, name);
        if (!m || !package_resolved(*m)) continue;
        if (!dry_run) {
            if (n == 0) doc->checkpoint("Resolve exact library matches");
            assign_package_geometry(doc->prj, name, *m, "library");
            thumbs_.erase(name);
        }
        ++n;
    }
    if (!dry_run && n) { doc->changed(ui::CH_GEOMETRY); say(std::to_string(n) + " package(s) taken from the library (same name)"); }
    return n;
}

void PackagesView::draw_resolve_props(Package* p) {
    const GTheme& t = th();
    section_header("UNRESOLVED / NO GRAPHICS");
    const int used = placements_using(doc->prj, cur_);
    ImGui::PushStyleColor(ImGuiCol_Text, t.warn);
    ImGui::TextWrapped("%s", (cur_ + " is used by " + std::to_string(used) + " placement(s) but has no pads or outline. "
                              "Choose its graphics below; they apply to every placement using this package.").c_str());
    ImGui::PopStyleColor();
    const std::string key = cur_ + "|" + std::to_string(LIB().master.gen);
    if (key != sugg_key_) { sugg_key_ = key; sugg_ = suggest_packages(cur_, &LIB().master.packages); }
    if (!sugg_.empty()) {
        ImGui::TextDisabled("Suggestions (verify before use):");
        int k = 0;
        for (auto& sg : sugg_) {
            ImGui::PushID(k++);
            std::string lbl;
            if (sg.kind == PkgSuggestion::Library) lbl = "Library: " + sg.name;
            else {
                const int ti = tpl_from_id(sg.tpl);
                lbl = std::string("Template: ") + (ti >= 0 ? tspec((Tpl)ti).title : sg.tpl.c_str()) + (sg.preset.empty() ? "" : "  " + sg.preset);
            }
            if (ImGui::Selectable(lbl.c_str())) {
                if (sg.kind == PkgSuggestion::Library) {
                    const std::string tgt = cur_, ln = sg.name, why = sg.why;
                    defer([this, tgt, ln, why, used]() {
                        if (confirm("Load from Library", "Use the library package \"" + ln + "\" for " + tgt + " (" + std::to_string(used) +
                                    " placement(s))?\n\n" + why, "Use it"))
                            load_from_library(tgt, ln);
                    });
                } else if (int ti = tpl_from_id(sg.tpl); ti >= 0) {
                    int pi = 0;
                    const auto& pr = tspec((Tpl)ti).presets;
                    for (int i = 0; i < (int)pr.size(); ++i) if (sg.preset == pr[i].label) pi = i;
                    open_template((Tpl)ti, cur_, pi, sg.params);   // opens the parameter dialog: the user confirms
                }
            }
            tooltip(sg.why.c_str());
            ImGui::PopID();
        }
    } else ImGui::TextDisabled("No family recognised from the name.");
    ImGui::Dummy(ImVec2(0, S(4)));
    const float bw = ImGui::GetContentRegionAvail().x;
    if (button("Assign template...", ui::Icon::PackageEditor, true, ImVec2(bw, 0))) show_templates();
    tooltip(("Opens the Templates tab: the template you pick fills " + cur_ + " for all " + std::to_string(used) + " placements using it").c_str());
    if (button("Load from library...", ui::Icon::Folder, false, ImVec2(bw, 0))) { lib_pick_[0] = 0; ImGui::OpenPopup("##libpick"); }
    tooltip("Use a package from the master library for this package name");
    if (button("Build from Gerber...", ui::Icon::Link, false, ImVec2(bw, 0))) pkg_wizard_open(doc, cur_);
    tooltip(("Gerber tab: box the pads of one placement of " + cur_ + " on the paste / copper Gerber; the package is used for all " +
             std::to_string(used) + " placements of it").c_str());
    if (button("Auto-resolve all (library / equivalent names / templates)", ui::Icon::Check, false, ImVec2(bw, 0))) {
        doc->checkpoint("Auto-resolve packages");
        const PkgResolveReport rr = auto_resolve_packages(doc->prj, &LIB().master.packages, true);
        for (auto& e : rr.entries) if (e.changed) thumbs_.erase(e.name);
        if (rr.changed) doc->changed(ui::CH_GEOMETRY);
        say(rr.summary(), rr.unresolved ? ToastKind::Warning : ToastKind::Success);
    }
    tooltip("Same-name library package, a resolved package with an equivalent name (R_0603 = 0603 = RESC1608), or a built-in chip / SOT-23 template. "
            "Nothing is overwritten; check the result in this tab.");
    if (const int ne = resolve_exact_library(true)) {
        if (button(("Take " + std::to_string(ne) + " same-name package(s) from the library").c_str(), ui::Icon::Check, false, ImVec2(bw, 0)))
            defer([this, ne]() {
                if (confirm("Resolve from Library", std::to_string(ne) + " unresolved package(s) have a library package with the same name (case-insensitive). Use them?", "Use them"))
                    resolve_exact_library(false);
            });
    }
    if (ImGui::BeginPopup("##libpick")) {
        ImGui::SetNextItemWidth(S(260));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##lpq", "Search master library", lib_pick_, sizeof lib_pick_);
        ImGui::BeginChild("##lpl", ImVec2(S(260), S(260)));
        const std::string q = lower(trim(lib_pick_));
        int shown = 0;
        for (auto& kv : LIB().master.packages) {
            if (!package_resolved(kv.second)) continue;
            if (!q.empty() && lower(kv.first).find(q) == std::string::npos) continue;
            if (++shown > 400) break;
            if (ImGui::Selectable(kv.first.c_str())) { const std::string n = kv.first; ImGui::CloseCurrentPopup(); load_from_library(cur_, n); break; }
        }
        if (!shown) ImGui::TextDisabled("No library packages");
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::Dummy(ImVec2(0, S(6)));
}

void PackagesView::draw_tape_props(Package* p) {
    // R18a (specs/ANGLES.md §10): display only. Sprocket holes on top, tape feeding to the right.
    // R22c (REVIEW_V21 §11): the tape is drawn lying on its side (feed to the right), so the part in the pocket is
    // drawn turned 90 deg CLOCKWISE relative to the Shapes 2D view. Only a local copy is rotated for drawing: the
    // package, its leads and every placement angle stay untouched. The pin-1 highlight and the "pin 1 left of the
    // sprocket holes" check use the turned copy, so they move with the graphic.
    const GTheme& t = th();
    section_header("CUT TAPE PREVIEW");
    ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
    ImGui::TextWrapped("The package as it sits in cut tape (sprocket holes on top, tape feeding right): drawn turned 90\xC2\xB0 clockwise "
                       "from the 2D view because the tape lies on its side. Display only: it does not change any angle.");
    ImGui::PopStyleColor();
    const float w = ImGui::GetContentRegionAvail().x, h = S(118);
    ImVec2 a = ImGui::GetCursorScreenPos(), b = a + ImVec2(w, h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, t.panel2, S(6));
    const ImU32 tape = IM_COL32(222, 214, 196, 255), hole = t.panel2, edge = IM_COL32(150, 140, 120, 255);
    const ImU32 pin1 = IM_COL32(255, 70, 140, 255);   // pink: lead 1 / polarity marker
    ImVec2 ta = a + ImVec2(S(6), S(8)), tb = b - ImVec2(S(6), S(8));
    dl->AddRectFilled(ta, tb, tape, S(2));
    for (float x = ta.x + S(10); x < tb.x - S(4); x += S(20)) dl->AddCircleFilled(ImVec2(x, ta.y + S(9)), S(3.5f), hole);   // sprockets: top
    const float ph = tb.y - ta.y - S(36), pw = ph;
    bool have_pin1 = false, left = false;
    Package tp = *p;                   // display copy, turned 90 deg clockwise (= -90 deg in the CCW package frame)
    pkg_geom_rotate(tp, -90.0);
    for (int k = -1; k <= 1; ++k) {
        ImVec2 c((ta.x + tb.x) * 0.5f + k * (pw + S(14)), ta.y + S(20) + ph * 0.5f);
        ImVec2 pa = c - ImVec2(pw * 0.5f, ph * 0.5f), pb = c + ImVec2(pw * 0.5f, ph * 0.5f);
        dl->AddRectFilled(pa, pb, IM_COL32(60, 56, 50, 255), S(2));
        dl->AddRect(pa, pb, edge, S(2));
        if (!package_resolved(*p)) continue;
        ImVec2 at(-1e9f, 0);
        draw_pkg_dl(dl, tp, pa + ImVec2(S(4), S(4)), pb - ImVec2(S(4), S(4)), false, pin1, &at);
        if (k == 0 && at.x > -1e8f) { have_pin1 = true; left = at.x < c.x - 0.5f; }
    }
    {   // feed direction: arrow pointing RIGHT
        const ImU32 fc = IM_COL32(90, 84, 70, 255);
        const float y = tb.y - S(8), x1 = tb.x - S(8), x0 = x1 - S(46);
        dl->AddText(ImVec2(x0 - ImGui::CalcTextSize("feed").x - S(6), y - ImGui::GetTextLineHeight() * 0.5f), fc, "feed");
        dl->AddLine(ImVec2(x0, y), ImVec2(x1 - S(5), y), fc, S(2));
        dl->AddTriangleFilled(ImVec2(x1, y), ImVec2(x1 - S(7), y - S(4.5f)), ImVec2(x1 - S(7), y + S(4.5f)), fc);
    }
    ImGui::Dummy(ImVec2(w, h));
    if (!package_resolved(*p)) { ImGui::TextDisabled("Preview after the package has graphics."); ImGui::Dummy(ImVec2(0, S(4))); return; }
    if (!have_pin1) ImGui::TextDisabled("Pin 1 should be left of the sprocket holes for cut tape (no lead 1 / marker on this package).");
    else if (left) {   // drawn check mark (the UI font has no U+2713)
        const ImVec2 q = ImGui::GetCursorScreenPos(); const float lh = ImGui::GetTextLineHeight();
        const ImVec2 ck[3] = {q + ImVec2(S(1), lh * 0.55f), q + ImVec2(S(5), lh * 0.85f), q + ImVec2(S(12), lh * 0.2f)};
        ImGui::GetWindowDrawList()->AddPolyline(ck, 3, t.ok, 0, S(2));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(16));
        ImGui::PushStyleColor(ImGuiCol_Text, t.ok); ImGui::TextWrapped("Pin 1 (pink) is on the left: matches cut tape."); ImGui::PopStyleColor();
    }
    else { ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim); ImGui::TextWrapped("Pin 1 should be left of the sprocket holes for cut tape (pin 1 is pink)."); ImGui::PopStyleColor(); }
    tooltip("Rule of thumb for cut tape: pin 1 sits on the left half of the pocket, with the sprocket holes on top.\n"
            "The pocket shows the part turned 90\xC2\xB0 clockwise from the 2D view (the tape lies on its side).\n"
            "If pin 1 is not on the left, rotate the package (Rotate package keeps the board placements).\n"
            "Sticks and trays are handled by the part number's Supply angle, not here.");
    ImGui::Dummy(ImVec2(0, S(4)));
}

void PackagesView::draw_orientation_props(Package* p) {
    ImGui::Dummy(ImVec2(0, S(6)));
    section_header("POLARITY MARKER");
    const std::vector<std::string> names = targets();
    const bool many = names.size() > 1;
    const std::string w = shared_warning_many(doc->prj, names);
    if (!w.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, th().warn); ImGui::TextWrapped("%s: edits here are template edits.", w.c_str()); ImGui::PopStyleColor(); }
    int npol = 0, nkind_mixed = 0, ncol_mixed = 0;
    for (auto& n : names) {
        const Package& q = doc->prj.packages[n];
        npol += pkg_has_polarity(q);
    }
    {   // mixed marker type / colour: compared among the selected packages that have a marker
        const Package* ref = nullptr;
        for (auto& n : names) {
            const Package& q = doc->prj.packages[n];
            if (!pkg_has_polarity(q)) continue;
            if (!ref) { ref = &q; continue; }
            nkind_mixed += q.marker_kind != ref->marker_kind;
            ncol_mixed += q.marker_color != ref->marker_color;
        }
    }
    const bool has = many ? npol > 0 : pkg_has_polarity(*p);
    {
        bool v = npol == (int)names.size();
        const bool mixed = npol && npol < (int)names.size();
        if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        if (ImGui::Checkbox("Has polarity", &v)) toggle_polarity();
        if (mixed) ImGui::PopItemFlag();
        std::string why;
        const bool def = pkg_default_polarity_reason(*p, &why) > 0;
        tooltip(("Has polarity  [P]\nThe part has an orientation that must match the board silkscreen indicator.\n"
                 "Turning it on places a red dot near lead 1 (first lead when no lead 1); move it to line up with the silkscreen.\n"
                 "Default for this package: " + std::string(def ? "yes" : "no") + " (" + why + ").\n"
                 "Information only: machine programs are not changed.").c_str());
    }
    if (!has) {
        ImGui::PushStyleColor(ImGuiCol_Text, th().text_faint);
        ImGui::TextWrapped("No polarity marker: the part is treated as non-oriented (any fitting rotation is accepted).");
        ImGui::PopStyleColor();
    } else {
        prop_label("Marker type");
        {
            const int cur = p->marker_kind;
            if (ImGui::BeginCombo("##mk", nkind_mixed ? kMixed : marker_kind_name((MarkerKind)cur))) {
                for (int k = 1; k <= 2; ++k)
                    if (ImGui::Selectable(marker_kind_name((MarkerKind)k), !nkind_mixed && cur == k))
                        marker_edit(k == 1 ? "Marker type: Dot" : "Marker type: Arrow", [k](Package& q) { if (pkg_has_polarity(q)) q.marker_kind = (uint8_t)k; });
                if (cur == (int)MarkerKind::Custom) ImGui::Selectable("Custom artwork (older project)", true);
                ImGui::EndCombo();
            }
            tooltip("Dot or Arrow  [Shift+P cycles]");
        }
        prop_label("Colour");
        {
            static const uint32_t sw[] = {0xFFE03131, 0xFFFF922B, 0xFFFCC419, 0xFF51CF66, 0xFF339AF0, 0xFFCC5DE8, 0xFFFFFFFF};
            for (int k = 0; k < 7; ++k) {
                if (k) ImGui::SameLine(0, S(3));
                ImGui::PushID(k);
                const uint32_t c = sw[k];
                ImVec4 cv(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, 1);
                if (ImGui::ColorButton("##sw", cv, ImGuiColorEditFlags_NoTooltip | (!ncol_mixed && p->marker_color == c ? 0 : ImGuiColorEditFlags_NoBorder), ImVec2(S(16), S(16))))
                    marker_edit("Marker colour", [c](Package& q) { if (pkg_has_polarity(q)) q.marker_color = c; });
                ImGui::PopID();
            }
        }
        if (many) {
            ImGui::PushStyleColor(ImGuiCol_Text, th().text_faint);
            ImGui::TextWrapped("Position, size and rotation are per package: select one package to edit them.");
            ImGui::PopStyleColor();
        } else {
        prop_label("X");
        { double v = p->marker_pos.x; if (len_input("##mkx", v)) marker_edit("Marker X", [v](Package& q) { q.marker_user_pos = true; q.marker_pos.x = v; }); }
        prop_label("Y");
        { double v = p->marker_pos.y; if (len_input("##mky", v)) marker_edit("Marker Y", [v](Package& q) { q.marker_user_pos = true; q.marker_pos.y = v; }); }
        tooltip("Package coordinates. Drag the marker on the drawing, or click it and nudge with the arrow keys.");
        prop_label("Size");
        { double v = p->marker_size; if (len_input("##mks", v, 0)) marker_edit("Marker size", [v](Package& q) { q.marker_size = v; }); }
        tooltip("0 = automatic (a small fraction of the lead field)");
        if (p->marker_kind == (uint8_t)MarkerKind::Arrow) {
            prop_label("Rotation");
            double r = p->marker_rot;
            ImGui::SetNextItemWidth(S(90));
            ImGui::InputDouble("##mkr", &r, 0, 0, "%.1f\xc2\xb0");
            if (ImGui::IsItemDeactivatedAfterEdit() && r != p->marker_rot) marker_edit("Marker rotation", [r](Package& q) { q.marker_rot = norm_deg(r); });
            ImGui::SameLine(0, S(4));
            if (button("+90\xC2\xB0", ui::Icon::None, false)) marker_edit("Rotate marker", [](Package& q) { q.marker_rot = norm_deg(q.marker_rot + 90); });
            tooltip("Arrow direction (degrees, counter-clockwise from +X)");
        }
        }   // !many
        {
            const float bw = (ImGui::GetContentRegionAvail().x - S(6)) * 0.5f;
            if (button("Snap to lead 1", ui::Icon::None, false, ImVec2(bw, 0)))
                marker_edit("Marker to lead 1", [](Package& q) { if (!pkg_has_polarity(q)) return; MarkerGeom g = pkg_marker_auto(q); q.marker_user_pos = true; q.marker_pos = g.at; });
            tooltip("Move the marker back next to lead 1 (or the first lead)");
            ImGui::SameLine(0, S(6));
            if (button("Delete marker", ui::Icon::None, false, ImVec2(bw, 0))) { msel_ = false; marker_edit("Remove polarity marker", [](Package& q) { pkg_remove_polarity_marker(q); }); }
        }
        if (!many && psel_.size() == 1 && *psel_.begin() < (int)p->pads.size()) {
            if (button("Marker at selected lead", ui::Icon::None, false, ImVec2(-1, 0))) {
                const Vec2 at = p->pads[*psel_.begin()].pos;
                marker_edit("Marker position", [at](Package& q) { q.marker_user_pos = true; q.marker_pos = at; });
            }
        }
    }
    if (pkg_pin1(*p) < 0 && !p->pads.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, th().warn);
        ImGui::TextWrapped("No lead is numbered 1: select a lead and use Make Pin 1, or Renumber.");
        ImGui::PopStyleColor();
    }
    const AxisInfo ai = pkg_axis_info(*p);
    if (ai.off_axis) {
        ImGui::PushStyleColor(ImGuiCol_Text, th().warn);
        ImGui::TextWrapped("Artwork appears rotated %+.2f\xc2\xb0 off-axis (%s).", ai.off90, ai.why.c_str());
        ImGui::PopStyleColor();
        if (button("Normalise rotation...", ui::Icon::None, false, ImVec2(-1, 0))) { rot_dlg_ = -ai.off90; rot_mode_ = 0; ImGui::OpenPopup("##rotn"); }
        draw_rotate_popup();
    }
}

void PackagesView::draw_properties() {
    Package* p = pkg();
    if (!p) { ImGui::TextDisabled("No package selected."); return; }
    if (!package_resolved(*p)) draw_resolve_props(p);   // R16b
    char buf[512];
    // R18c: name / process / polarity / height live in the bar above the drawing; this panel keeps the rest
    const std::vector<std::string> names = targets();
    const bool many = names.size() > 1;
    auto& P = doc->prj.packages;
    if (many) {
        section_header("SELECTION");
        ImGui::PushStyleColor(ImGuiCol_Text, th().text_dim);
        ImGui::TextWrapped("%d packages selected. Each edit applies only that property to all of them (one undo step); mixed values show \xe2\x80\x94.", (int)names.size());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, S(4)));
    }
    section_header("DETAILS");
    {
        bool dm = false, pm = false;
        for (auto& n : names) { dm = dm || P[n].description != p->description; pm = pm || P[n].default_process != p->default_process; }
        prop_label("Description");
        snprintf(buf, sizeof buf, "%s", dm ? "" : p->description.c_str());
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##desc", dm ? kMixed : "", buf, sizeof buf);
        if (ImGui::IsItemDeactivatedAfterEdit() && (!dm || buf[0])) { const std::string v = buf; multi_edit("Package description", [v](Package& q) { q.description = v; }); }
        prop_label("Process step");
        if (ImGui::BeginCombo("##proc", pm ? kMixed : p->default_process.empty() ? "(none)" : p->default_process.c_str())) {
            if (ImGui::Selectable("(none)", !pm && p->default_process.empty())) multi_edit("Default process step", [](Package& q) { q.default_process.clear(); });
            for (auto& mp : doc->prj.model.points)
                if (ImGui::Selectable(mp.name.c_str(), !pm && mp.name == p->default_process)) { const std::string v = mp.name; multi_edit("Default process step", [v](Package& q) { q.default_process = v; }); }
            ImGui::EndCombo();
        }
        tooltip("Factory process point (model point) this package is routed to by default.\nSMT / through-hole is the Process field in the bar above the drawing.");
        if (!many) {
            prop_label("Leads");
            ImGui::Text("%d   (%s)", (int)p->pads.size(), is_th(*p) ? "through-hole" : "SMT");
            prop_label("Graphics");
            ImGui::Text("%d primitive(s)", (int)p->body.size());
        }
    }

    draw_orientation_props(p);
    if (many) return;     // R18c: the sections below describe / edit one package
    draw_tape_props(p);   // R16b
    p = pkg();
    if (!p) return;
    ImGui::Dummy(ImVec2(0, S(6)));
    section_header("MASTER LIBRARY");
    {
        auto ps = psync_.find(cur_);
        SyncState ss = ps == psync_.end() ? SyncState::LocalOnly : ps->second;
        ImGui::PushStyleColor(ImGuiCol_Text, ss == SyncState::Differs ? th().warn : ss == SyncState::Same ? th().ok : th().text_dim);
        ImGui::TextWrapped("%s", ss == SyncState::Same ? "Identical copy in the master library."
                                 : ss == SyncState::Differs ? "The master library has a different version of this package."
                                                            : "Local only: not in the master library yet.");
        ImGui::PopStyleColor();
        float bw = (ImGui::GetContentRegionAvail().x - S(4)) * 0.5f;
        if (button("Send to Library", ui::Icon::None, ss != SyncState::Same, ImVec2(bw, 0), !LIB().master.read_only && ss != SyncState::Same)) { lsel_ = {cur_}; act_transfer(true); }
        ImGui::SameLine(0, S(4));
        if (button("Revert from Library", ui::Icon::None, false, ImVec2(bw, 0), ss == SyncState::Differs)) {
            llsel_ = {cur_}; lsrc_ = 0;
            ConflictAction keep = policy_;
            policy_ = ConflictAction::Ask;
            act_transfer(false);
            policy_ = keep;
        }
        tooltip("Pull the master library version into this project (asks before overwriting)");
    }

    ImGui::Dummy(ImVec2(0, S(6)));
    section_header("USAGE");
    {
        auto a = use_comps_.find(cur_), b = use_parts_.find(cur_);
        int nc = a == use_comps_.end() ? 0 : a->second, np = b == use_parts_.end() ? 0 : b->second;
        ImGui::Text("%d placement(s)  \xc2\xb7  %d part number(s)", nc, np);
        ImVec2 bp = ImGui::GetCursorScreenPos();
        if (button("      Show users in plotter", ui::Icon::None, false, ImVec2(-1, 0), nc > 0)) act_show_users();
        glyph(ImGui::GetWindowDrawList(), G::Users, bp + ImVec2(S(10), (ImGui::GetFrameHeight() + S(2) - S(16)) * 0.5f), S(16), nc ? 1.f : 0.4f);
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    section_header("EDIT OPTIONS");
    ImGui::Checkbox("Keep placements in place", &compensate_);
    tooltip("Rotate 90 / Set origin compensate the rotation and position\nof every placement using this package so the board does not change.");
    prop_label("Precision");
    ImGui::SliderInt("##prec", &prec_, 1, 5, "%d decimals");
    // selected pads quick edit
    if (!psel_.empty()) {
        ImGui::Dummy(ImVec2(0, S(6)));
        snprintf(buf, sizeof buf, "SELECTED LEADS (%d)", (int)psel_.size());
        section_header(buf);
        const Pad f = p->pads[*psel_.begin()];
        auto len_field = [&](const char* label, const char* id, double cur, auto setter) {
            prop_label(label);
            double v = cur;
            if (len_input(id, v, 0)) {
                doc->checkpoint("Edit leads");
                for (int i : psel_) if (i < (int)p->pads.size()) setter(p->pads[i], v);
                edited();
            }
        };
        len_field("Width", "##sw", f.w, [](Pad& q, double v) { q.w = std::max(0.01, v); });
        len_field("Height", "##sh", f.h, [](Pad& q, double v) { q.h = std::max(0.01, v); });
        len_field("Drill", "##sd", f.drill, [](Pad& q, double v) { q.drill = std::max(0.0, v); });
        if (Package* q = pkg()) q->smt = !pkg_detect_th(*q);   // R18c: drilled leads decide SMT / through-hole
        prop_label("Shape");
        int sh = (int)f.shape;
        if (ImGui::Combo("##ssh", &sh, kShapeNames, 5)) { doc->checkpoint("Lead shape"); for (int i : psel_) if (i < (int)p->pads.size()) p->pads[i].shape = (PadShape)sh; edited(); }
    }
}

void PackagesView::draw_lib_preview_props() {
    const Package* p = shown();
    PackageLibrary& L = curlib();
    if (!p) {
        ImGui::TextDisabled("Select a library package.");
        ImGui::Dummy(ImVec2(0, S(8)));
        section_header("LIBRARY FILE");
        ImGui::TextWrapped("%s", L.path.c_str());
        ImGui::Text("%d packages%s", (int)L.packages.size(), L.read_only ? "  (read-only)" : "");
        return;
    }
    section_header("LIBRARY PACKAGE");
    prop_label("Name"); ImGui::TextUnformatted(lcur_.c_str());
    prop_label("Description"); ImGui::TextWrapped("%s", p->description.empty() ? "-" : p->description.c_str());
    prop_label("Leads"); ImGui::Text("%d  (%s)", (int)p->pads.size(), is_th(*p) ? "through-hole" : "SMT");
    prop_label("Graphics"); ImGui::Text("%d", (int)p->body.size());
    prop_label("Height"); ImGui::TextUnformatted(fmt_len_u8(p->height).c_str());
    ImGui::Dummy(ImVec2(0, S(6)));
    section_header("THIS PROJECT");
    auto it = lsync_.find(lcur_);
    SyncState ss = it == lsync_.end() ? SyncState::LibraryOnly : it->second;
    ImGui::PushStyleColor(ImGuiCol_Text, ss == SyncState::Differs ? th().warn : ss == SyncState::Same ? th().ok : th().text_dim);
    ImGui::TextWrapped("%s", ss == SyncState::Same ? "The project has an identical copy." : ss == SyncState::Differs ? "The project has a different version." : "Not in this project.");
    ImGui::PopStyleColor();
    if (button("\xe2\x97\x80  Pull into Project", ui::Icon::None, true, ImVec2(-1, 0), ss != SyncState::Same)) { if (llsel_.empty()) llsel_ = {lcur_}; act_transfer(false); }
    if (ss != SyncState::LibraryOnly && button("Open project copy", ui::Icon::None, false, ImVec2(-1, 0))) { std::string n = lcur_; mode_ = 0; select_package(n); scroll_to_cur_ = true; }
    ImGui::Dummy(ImVec2(0, S(6)));
    section_header("LIBRARY FILE");
    ImGui::PushStyleColor(ImGuiCol_Text, th().text_dim);
    ImGui::TextWrapped("%s", L.path.c_str());
    ImGui::PopStyleColor();
    ImGui::Text("%d packages%s", (int)L.packages.size(), L.read_only ? "  (read-only)" : "  (auto-saved)");
    // lead table (read-only)
    ImGui::Dummy(ImVec2(0, S(6)));
    section_header("LEADS");
    if (ImGui::BeginTable("##lpads", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchSame)) {
        for (const char* c : {"Pin", "X", "Y", "Width", "Height"}) ImGui::TableSetupColumn(c);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)p->pads.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const Pad& d = p->pads[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%d", d.pin);
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(fmt_n(d.pos.x).c_str());
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(fmt_n(d.pos.y).c_str());
                ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(fmt_n(d.w).c_str());
                ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(fmt_n(d.h).c_str());
            }
        ImGui::EndTable();
    }
}

// R13 (§7 Package Editor): focused inspector for the selected lead instead of the full grid
void PackagesView::draw_lead_inspector(Package* p) {
    const GTheme& t = th();
    if (psel_.empty() || *psel_.begin() >= (int)p->pads.size()) {
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
        ImGui::TextWrapped("Click a lead in the editor to inspect it (Ctrl+click adds to the selection). "
                           "The table button above shows every lead.");
        ImGui::PopStyleColor();
        return;
    }
    const int i = *psel_.begin();
    Pad& pd = p->pads[i];
    if (psel_.size() > 1) ImGui::TextColored(ImColor(t.text_dim), "%d leads selected; showing the first. Size / shape edits in Properties apply to all.", (int)psel_.size());
    if (ImGui::BeginTable("##lead_insp", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("l1", ImGuiTableColumnFlags_WidthFixed, S(56));
        ImGui::TableSetupColumn("v1", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("l2", ImGuiTableColumnFlags_WidthFixed, S(56));
        ImGui::TableSetupColumn("v2", ImGuiTableColumnFlags_WidthStretch);
        auto lab = [&](int col, const char* s2, const char* tip) {
            ImGui::TableSetColumnIndex(col); ImGui::AlignTextToFramePadding(); ImGui::TextColored(ImColor(t.text_dim), "%s", s2);
            if (tip) tooltip(tip);
            ImGui::TableSetColumnIndex(col + 1); ImGui::SetNextItemWidth(-1);
        };
        auto lenv = [&](const char* id, double& ref, double minv, const char* what) {
            double v = ref;
            if (len_input(id, v, minv)) { doc->checkpoint(what); ref = v; edited(); }
        };
        ImGui::TableNextRow();
        lab(0, "Pin #", "Pin number of this lead. Editing it changes only this lead.\nMake Pin 1 / Renumber change the others.");
        char pb[16]; snprintf(pb, sizeof pb, "%d", pd.pin);
        ImGui::InputText("##ipin", pb, sizeof pb, ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemDeactivatedAfterEdit()) { int v = atoi(pb); if (v != pd.pin) { doc->checkpoint("Pin number"); pd.pin = v; edited(); } }
        lab(2, "Shape", nullptr);
        int sh = (int)pd.shape;
        if (ImGui::Combo("##ish", &sh, kShapeNames, 5)) { doc->checkpoint("Lead shape"); pd.shape = (PadShape)sh; edited(); }
        ImGui::TableNextRow();
        lab(0, "X", "Package-relative X (arrow keys nudge the selection)"); lenv("##ix", pd.pos.x, -1e300, "Edit lead");
        lab(2, "Y", "Package-relative Y"); lenv("##iy", pd.pos.y, -1e300, "Edit lead");
        ImGui::TableNextRow();
        lab(0, "W", "Lead width"); lenv("##iw", pd.w, 0.01, "Edit lead");
        lab(2, "H", "Lead height"); lenv("##ih", pd.h, 0.01, "Edit lead");
        ImGui::TableNextRow();
        lab(0, "Rot", "Lead rotation (\xc2\xb0)");
        char rb[16]; snprintf(rb, sizeof rb, "%g", pd.rot);
        ImGui::InputText("##irot", rb, sizeof rb, ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemDeactivatedAfterEdit()) { double v = norm_deg(atof(rb)); if (v != pd.rot) { doc->checkpoint("Lead rotation"); pd.rot = v; edited(); } }
        lab(2, "Drill", "Drill diameter (0 = SMT)"); lenv("##idr", pd.drill, 0.0, "Edit lead");
        ImGui::EndTable();
    }
    if (pd.pin != 1) { if (button("Make Pin 1", ui::Icon::PinOne, false)) set_pin1(i); tooltip("Make this lead pin 1 (cyclic shift keeps the CCW order)"); }
    else ImGui::TextColored(ImColor(t.ok), "This lead is pin 1.");
}

void PackagesView::draw_pad_table() {
    Package* p = pkg();
    if (!p) { ImGui::TextDisabled("No package selected."); return; }
    const GTheme& t = th();
    // mini toolbar
    if (gbtn("tadd", G::AddPad, "Add lead (click in the editor)", tool_ == Tool::AddPad, true, S(28))) set_tool(Tool::AddPad);
    ImGui::SameLine(0, S(2));
    bool full = app::ui_pref("Pkg.LeadTable", 0) != 0;
    if (icon_button("ttab", ui::Icon::Grid, full ? "Show the selected-lead inspector" : "Show the full lead table (every lead)", full, true, S(28)))
        app::set_ui_pref("Pkg.LeadTable", full ? 0 : 1);
    ImGui::SameLine(0, S(2));
    if (gbtn("tarr", G::Array, "Lead row / array generator...", false, true, S(28))) pad_array();
    ImGui::SameLine(0, S(2));
    if (icon_button("tdel", ui::Icon::Delete, "Delete selected leads  [Del]\nOther leads keep their pin numbers", false, !psel_.empty(), S(28))) delete_pads();
    ImGui::SameLine(0, S(2));
    if (gbtn("tren", G::Renumber, "Renumber CCW from pin 1\nThe only command that changes pin numbers of other leads", false, true, S(28))) renumber(0);
    ImGui::SameLine(0, S(8));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%d leads, %d selected  \xc2\xb7  %s", (int)p->pads.size(), (int)psel_.size(), ui::U(app::units_label()).c_str());
    ImGui::Dummy(ImVec2(0, S(2)));
    if (!full) { draw_lead_inspector(p); return; }
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(3), S(2)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
    if (ImGui::BeginTable("##pads", 8, tf, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        const char* cols[] = {"Pin", "X", "Y", "Width", "Height", "Rot", "Shape", "Drill"};
        float wt[] = {0.6f, 1.f, 1.f, 1.f, 1.f, 0.7f, 1.1f, 0.85f};
        for (int i = 0; i < 8; ++i) ImGui::TableSetupColumn(cols[i], ImGuiTableColumnFlags_WidthStretch, wt[i]);
        ImGui::TableHeadersRow();
        static int last_sel_scroll = -1;
        ImGuiListClipper clip;
        clip.Begin((int)p->pads.size());
        if (psel_.size() == 1 && *psel_.begin() != last_sel_scroll && *psel_.begin() < (int)p->pads.size()) clip.IncludeItemByIndex(*psel_.begin());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                Pad& pd = p->pads[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);
                const bool sel = psel_.count(i) > 0;
                if (sel) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, with_alpha(t.accent, 0x38));
                ImGui::TableSetColumnIndex(0);
                if (psel_.size() == 1 && *psel_.begin() == i && last_sel_scroll != i) { ImGui::SetScrollHereY(0.4f); last_sel_scroll = i; }
                char pb[16];
                snprintf(pb, sizeof pb, "%d", pd.pin);
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##pin", pb, sizeof pb, ImGuiInputTextFlags_CharsDecimal);
                if (ImGui::IsItemActivated()) {
                    if (ImGui::GetIO().KeyCtrl) { if (!psel_.erase(i)) psel_.insert(i); } else psel_ = {i};
                    gsel_.clear();
                    canvas_.invalidate();
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) { int v = atoi(pb); if (v != pd.pin) { doc->checkpoint("Pin number"); pd.pin = v; edited(); } }
                auto lenc = [&](int col, const char* id, double& ref, double minv) {
                    ImGui::TableSetColumnIndex(col);
                    double v = ref;
                    bool ch = len_input(id, v, minv);
                    if (ImGui::IsItemActivated() && !psel_.count(i)) { psel_ = {i}; gsel_.clear(); canvas_.invalidate(); }
                    if (ch) { doc->checkpoint("Edit lead"); ref = v; edited(); }
                };
                // X/Y are package-relative (no virtual origin)
                lenc(1, "##x", pd.pos.x, -1e300);
                lenc(2, "##y", pd.pos.y, -1e300);
                lenc(3, "##w", pd.w, 0.01);
                lenc(4, "##h", pd.h, 0.01);
                ImGui::TableSetColumnIndex(5);
                char rb[16];
                snprintf(rb, sizeof rb, "%g", pd.rot);
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##r", rb, sizeof rb, ImGuiInputTextFlags_CharsDecimal);
                if (ImGui::IsItemDeactivatedAfterEdit()) { double v = norm_deg(atof(rb)); if (v != pd.rot) { doc->checkpoint("Lead rotation"); pd.rot = v; edited(); } }
                ImGui::TableSetColumnIndex(6);
                int sh = (int)pd.shape;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##s", &sh, kShapeNames, 5)) { doc->checkpoint("Lead shape"); pd.shape = (PadShape)sh; edited(); }
                lenc(7, "##d", pd.drill, 0);
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void PackagesView::draw_graphics() {
    Package* p = pkg();
    if (!p) { ImGui::TextDisabled("No package selected."); return; }
    const GTheme& t = th();
    draw_body_style();   // R18c
    section_header("NEW GRAPHICS");
    prop_label("Arc sweep");
    { float v = (float)arc_sweep_; if (ImGui::SliderFloat("##sweep", &v, -360, 360, "%.0f\xc2\xb0")) arc_sweep_ = v; }
    prop_label("Text height");
    len_input("##texth", text_h_, 0.1);
    ImGui::Checkbox("Filled shapes", &fill_);
    ImGui::Dummy(ImVec2(0, S(6)));
    char hb[64];
    snprintf(hb, sizeof hb, "BODY GRAPHICS (%d)", (int)p->body.size());
    section_header(hb);
    if (p->body.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
        ImGui::TextWrapped("No artwork. Use the Draw tools above the canvas, or Render to build a body from the flags.");
        ImGui::PopStyleColor();
    }
    // list
    const float lh = std::min(S(200), ImGui::GetTextLineHeightWithSpacing() * (float)std::max<size_t>(3, p->body.size()) + S(8));
    if (!p->body.empty() && ImGui::BeginListBox("##glist", ImVec2(-1, lh))) {
        for (size_t i = 0; i < p->body.size(); ++i) {
            const GPrim& g = p->body[i];
            char lb[128];
            snprintf(lb, sizeof lb, "%s%s  \xc2\xb7  %s%s##g%d", prim_kind_name(g.kind), g.kind == GKind::Text ? (" \"" + g.text + "\"").c_str() : "",
                     g.width > 0 ? fmt_len_u8(g.width).c_str() : "hairline", g.filled ? "  \xc2\xb7  filled" : "", (int)i);
            if (ImGui::Selectable(lb, gsel_.count((int)i) > 0)) {
                if (ImGui::GetIO().KeyCtrl) { if (!gsel_.erase((int)i)) gsel_.insert((int)i); } else gsel_ = {(int)i};
                psel_.clear();
                canvas_.invalidate();
            }
        }
        ImGui::EndListBox();
    }
    if (!p->body.empty()) {
        float bw = (ImGui::GetContentRegionAvail().x - S(8)) / 3.f;
        if (button("Select all", ui::Icon::None, false, ImVec2(bw, 0))) { gsel_.clear(); for (size_t i = 0; i < p->body.size(); ++i) gsel_.insert((int)i); psel_.clear(); canvas_.invalidate(); }
        ImGui::SameLine(0, S(4));
        if (button("Delete", ui::Icon::None, false, ImVec2(bw, 0), !gsel_.empty())) { psel_.clear(); delete_selection(); }
        ImGui::SameLine(0, S(4));
        if (button("Clear all", ui::Icon::None, false, ImVec2(bw, 0))) { doc->checkpoint("Clear graphics"); p->body.clear(); gsel_.clear(); edited(); }
    }
    // selected primitive editor
    if (gsel_.size() == 1 && *gsel_.begin() < (int)p->body.size()) {
        GPrim& g = p->body[*gsel_.begin()];
        ImGui::Dummy(ImVec2(0, S(6)));
        snprintf(hb, sizeof hb, "SELECTED %s", prim_kind_name(g.kind));
        std::string up = hb;
        for (auto& c : up) c = (char)std::toupper((unsigned char)c);
        section_header(up.c_str());
        ImGui::PushID("gprim");
        auto num = [&](const char* label, const char* id, double& ref, double minv = -1e300) {
            prop_label(label);
            double v = ref;
            if (len_input(id, v, minv)) { doc->checkpoint("Edit graphics"); ref = v; edited(); }
        };
        auto deg = [&](const char* label, const char* id, double& ref) {
            prop_label(label);
            float v = (float)ref;
            ImGui::InputFloat(id, &v, 0, 0, "%.1f");
            if (ImGui::IsItemDeactivatedAfterEdit() && std::fabs(v - ref) > 1e-6) { doc->checkpoint("Edit graphics"); ref = v; edited(); }
        };
        num("Stroke", "##gw", g.width, 0);
        if (g.kind == GKind::Rect || g.kind == GKind::Circle || g.kind == GKind::Polygon) {
            prop_label("Filled");
            bool f = g.filled;
            if (ImGui::Checkbox("##gf", &f)) { doc->checkpoint("Edit graphics"); g.filled = f; edited(); }
        }
        switch (g.kind) {
        case GKind::Line: case GKind::Rect:
            if (g.pts.size() >= 2) {
                num(g.kind == GKind::Rect ? "Corner 1 X" : "Start X", "##x0", g.pts[0].x); num(g.kind == GKind::Rect ? "Corner 1 Y" : "Start Y", "##y0", g.pts[0].y);
                num(g.kind == GKind::Rect ? "Corner 2 X" : "End X", "##x1", g.pts[1].x); num(g.kind == GKind::Rect ? "Corner 2 Y" : "End Y", "##y1", g.pts[1].y);
            }
            if (g.kind == GKind::Rect) deg("Rotation", "##grot", g.rot);
            break;
        case GKind::Circle: case GKind::Arc:
            if (!g.pts.empty()) { num("Centre X", "##cx", g.pts[0].x); num("Centre Y", "##cy", g.pts[0].y); }
            num("Radius", "##rad", g.radius, 0.001);
            if (g.kind == GKind::Arc) { deg("Start angle", "##a0", g.a0); deg("Sweep", "##a1", g.a1); }
            break;
        case GKind::Text: {
            if (!g.pts.empty()) { num("X", "##tx", g.pts[0].x); num("Y", "##ty", g.pts[0].y); }
            num("Height", "##th", g.text_h, 0.05);
            prop_label("Text");
            char tb[256];
            snprintf(tb, sizeof tb, "%s", g.text.c_str());
            ImGui::InputText("##tt", tb, sizeof tb);
            if (ImGui::IsItemDeactivatedAfterEdit() && g.text != tb) { doc->checkpoint("Edit text"); g.text = tb; edited(); }
        } break;
        case GKind::Polyline: case GKind::Polygon:
            ImGui::TextDisabled("%d vertices", (int)g.pts.size());
            for (size_t k = 0; k < g.pts.size() && k < 64; ++k) {
                ImGui::PushID((int)k);
                char l1[24], l2[24];
                snprintf(l1, sizeof l1, "P%d X", (int)k + 1); snprintf(l2, sizeof l2, "P%d Y", (int)k + 1);
                num(l1, "##px", g.pts[k].x); num(l2, "##py", g.pts[k].y);
                ImGui::PopID();
            }
            break;
        default: break;
        }
        ImGui::PopID();
    } else if (gsel_.size() > 1) {
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::TextDisabled("%d graphics selected (drag or arrow keys to move)", (int)gsel_.size());
    }
}

void PackagesView::draw_templates(float w) {
    const GTheme& t = th();
    ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
    if (const Package* cp = pkg(); cp && !package_resolved(*cp)) {
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, t.warn);
        ImGui::TextWrapped("%s", ("Assigning graphics to " + cur_ + ": pick the family; the result applies to all its placements.").c_str());
    } else
    ImGui::TextWrapped("Pick a package family, adjust its parameters with a live preview, then create it.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(4)));
    if (tpl_preview_.size() != (size_t)Tpl::Count) {
        tpl_preview_.clear();
        for (int i = 0; i < (int)Tpl::Count; ++i) tpl_preview_.push_back(make_template((Tpl)i, tdefaults((Tpl)i), nullptr));
    }
    const float gap = S(6);
    const int cols = w > S(330) ? 3 : 2;
    const float tw = std::floor((ImGui::GetContentRegionAvail().x - gap * (cols - 1)) / cols), th_ = tw * 0.72f + S(20);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < (int)Tpl::Count; ++i) {
        if (i % cols) ImGui::SameLine(0, gap);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        bool c = ImGui::InvisibleButton("##tpl", ImVec2(tw, th_));
        bool hov = ImGui::IsItemHovered();
        dl->AddRectFilled(p, p + ImVec2(tw, th_), hov ? t.raised : t.panel2, S(8));
        dl->AddRect(p, p + ImVec2(tw, th_), hov ? with_alpha(t.accent, 0xC0) : t.border, S(8));
        draw_pkg_dl(dl, tpl_preview_[i], p + ImVec2(S(4), S(4)), p + ImVec2(tw - S(4), th_ - S(20)), false);
        text_clip(dl, ImVec2(p.x + S(7), p.y + th_ - S(19)), tw - S(12), tspec((Tpl)i).title, t.text);
        if (hov) tooltip(tspec((Tpl)i).blurb);
        if (c) { const Package* cp = pkg(); open_template((Tpl)i, cp && !package_resolved(*cp) ? cur_ : std::string()); }
        ImGui::PopID();
        if (i % cols != cols - 1) {} else ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
    }
}

void PackagesView::draw_inspector(float w, float h) {
    const GTheme& t = th();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    ImGui::BeginChild("##insp", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    if (mode_ == 1) {
        ImGui::BeginChild("##insp_body", ImVec2(0, 0), ImGuiChildFlags_None);
        draw_lib_preview_props();
        ImGui::EndChild();
    } else {
        // segmented tabs
        const char* tabs[] = {"Properties", "Leads", "Graphics", "Templates", "Mach."};   // R24b Machine data tab
        const int nt = 5;
        const float cb = S(26);   // R12 §12: collapse toggle (arrow points the way the panel goes)
        {
            const ImVec2 c0 = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(c0.x + ImGui::GetContentRegionAvail().x - cb, c0.y + S(2)));
            if (icon_button("rcollapse", ui::Icon::ArrowRight, "Hide the properties panel (Ctrl+])", false, true, cb)) { show_right_ = false; app::set_ui_pref("Pkg.ShowProps", 0); }
            ImGui::SetCursorScreenPos(c0);
        }
        float avail = ImGui::GetContentRegionAvail().x - cb - S(4), bw = avail / nt, bh = S(30);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, p + ImVec2(avail, bh), t.panel2, S(8));
        for (int i = 0; i < nt; ++i) {
            ImGui::SetCursorScreenPos(p + ImVec2(bw * i, 0));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##tab", ImVec2(bw, bh))) right_tab_ = i;
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            ImVec2 a = p + ImVec2(bw * i + S(3), S(3)), b = p + ImVec2(bw * (i + 1) - S(3), bh - S(3));
            if (right_tab_ == i) dl->AddRectFilled(a, b, t.raised, S(6)), dl->AddRect(a, b, t.border, S(6));
            ImVec2 ts = ImGui::CalcTextSize(tabs[i]);
            float x = (a.x + b.x - ts.x) * 0.5f;
            dl->AddText(ImVec2(x, (a.y + b.y - ts.y) * 0.5f), right_tab_ == i || hov ? t.text : t.text_dim, tabs[i]);
        }
        ImGui::SetCursorScreenPos(p + ImVec2(0, bh + S(10)));
        ImGui::BeginChild("##insp_body", ImVec2(0, 0), ImGuiChildFlags_None, right_tab_ == 1 ? ImGuiWindowFlags_NoScrollbar : 0);
        if (right_tab_ == 0) draw_properties();
        else if (right_tab_ == 1) draw_pad_table();
        else if (right_tab_ == 2) draw_graphics();
        else if (right_tab_ == 3) draw_templates(w);
        else draw_machine_data(doc, mode_ == 0 ? targets() : std::vector<std::string>{}, cur_, [this](const std::string& n) { select_package(n); scroll_to_cur_ = true; });   // R24b
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PackagesView::draw(ImVec2 size) {
    ++frame_;
    sync();
    if (!panes_loaded_) {   // P2-01: pane state persists ([UI] Pkg.*)
        panes_loaded_ = true;
        show_left_ = app::ui_pref("Pkg.ShowList", 1) != 0; show_right_ = app::ui_pref("Pkg.ShowProps", 0) != 0;   // R12 §12: hidden by default (R / P / Up / Down cover the production workflow)
        lw_ = (float)app::ui_pref("Pkg.ListW", 0); rw_ = (float)app::ui_pref("Pkg.PropsW", 0);
        vis_dims_ = app::ui_pref("Pkg.Dims", 0) != 0; vis_stats_ = app::ui_pref("Pkg.Stats", 0) != 0;
    }
    if (mode_ == 1 && !mgr_open_) mode_ = 0;   // the library is only browsed inside the Library Manager
    // Ctrl+[ / Ctrl+] collapse the package list / properties (when no text field is active)
    ImGuiIO& io = ImGui::GetIO();
    // R18c: N shows / hides the Packages list (Blender-style panel toggle)
    if (!io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift && !mgr_open_ && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        show_left_ = !show_left_; app::set_ui_pref("Pkg.ShowList", show_left_);
    }
    if (!io.WantTextInput && io.KeyCtrl && !mgr_open_) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) { show_left_ = !show_left_; app::set_ui_pref("Pkg.ShowList", show_left_); }
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) { show_right_ = !show_right_; app::set_ui_pref("Pkg.ShowProps", show_right_); }
        // R12 §7/§12: Ctrl+Space = maximize / restore the drawing area (panels hidden, layout restored after)
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (!maximized_) { max_left_ = show_left_; max_right_ = show_right_; show_left_ = show_right_ = false; maximized_ = true; }
            else { show_left_ = max_left_; show_right_ = max_right_; maximized_ = false; }
            canvas_.invalidate();
        }
    }
    if (maximized_ && (show_left_ || show_right_)) maximized_ = false;   // a panel was reopened by hand
    const float rail = S(22);
    const float lw = show_left_ ? std::clamp(lw_ > 0 ? S(lw_) : size.x * 0.22f, S(240), size.x * 0.4f) : rail;
    const float rw = show_right_ ? std::clamp(rw_ > 0 ? S(rw_) : size.x * 0.24f, S(240), size.x * 0.45f) : rail;
    const float cw = std::max(S(200), size.x - lw - rw - S(8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(6), S(6)));
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto collapsed_rail = [&](const char* id, ImVec2 at, bool left) {
        ImGui::SetCursorScreenPos(at);
        dl->AddRectFilled(at, at + ImVec2(rail, size.y), th().panel);
        bool c = ImGui::InvisibleButton(id, ImVec2(rail, size.y));
        const bool hv = ImGui::IsItemHovered();
        tooltip(left ? "Show the package list (Ctrl+[)" : "Show the properties (Ctrl+])");
        const ImVec2 m = at + ImVec2(rail * 0.5f, S(24));
        const float k = S(5);
        const ImU32 col = hv ? th().text : th().text_dim;
        if (left) dl->AddTriangleFilled(m + ImVec2(-k * 0.6f, -k), m + ImVec2(-k * 0.6f, k), m + ImVec2(k * 0.8f, 0), col);
        else dl->AddTriangleFilled(m + ImVec2(k * 0.6f, -k), m + ImVec2(k * 0.6f, k), m + ImVec2(-k * 0.8f, 0), col);
        return c;
    };
    auto splitter = [&](const char* id, float x, float* store, const char* key, bool left) {
        ImGui::SetCursorScreenPos(ImVec2(x - S(3), o.y));
        ImGui::InvisibleButton(id, ImVec2(S(6), size.y));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive() && io.MouseDelta.x != 0) {
            const float cur = left ? lw : rw;
            *store = (cur + (left ? io.MouseDelta.x : -io.MouseDelta.x)) / dpi_scale();
        }
        if (ImGui::IsItemDeactivated()) app::set_ui_pref(key, (int)*store);
        dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + size.y), ImGui::IsItemHovered() || ImGui::IsItemActive() ? th().accent : th().border);
    };
    if (mgr_open_) {   // the editor is parked while the Library Manager uses the canvas
        dl->AddRectFilled(o, o + size, th().bg);
        if (modal_stack().empty()) draw_manager();   // a dialog opened from the manager (conflicts, files) takes the modal slot meanwhile
        ImGui::PopStyleVar();
        return;
    }
    if (show_left_) draw_library(lw, size.y);
    else if (collapsed_rail("##lrail", o, true)) { show_left_ = true; app::set_ui_pref("Pkg.ShowList", 1); }
    ImGui::SetCursorScreenPos(o + ImVec2(lw + S(4), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(6), 0));
    draw_editor(cw, size.y);
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(o + ImVec2(lw + cw + S(8), 0));
    if (show_right_) draw_inspector(size.x - lw - cw - S(8), size.y);
    else if (collapsed_rail("##rrail", o + ImVec2(size.x - rail, 0), false)) { show_right_ = true; app::set_ui_pref("Pkg.ShowProps", 1); }
    if (msim_open_) draw_merge_similar();
    if (show_left_) splitter("##lsplit", o.x + lw + S(2), &lw_, "Pkg.ListW", true);
    if (show_right_) splitter("##rsplit", o.x + lw + cw + S(6), &rw_, "Pkg.PropsW", false);
    ImGui::PopStyleVar();
}

// P2-05: central-library management (browse master / source libraries, pull into the project, send to the library,
// conflict policy) in its own large, resizable window; the editor itself works on the project's packages.
void PackagesView::draw_manager() {
    const GTheme& t = th();
    if (!ImGui::IsPopupOpen("Package Library Manager###pkglibmgr")) ImGui::OpenPopup("Package Library Manager###pkglibmgr");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.86f, vp->WorkSize.y * 0.84f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(vp->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::BeginPopupModal("Package Library Manager###pkglibmgr", &open, ImGuiWindowFlags_NoSavedSettings)) {
        mode_ = 1;
        const ImVec2 av = ImGui::GetContentRegionAvail();
        const float footer = ImGui::GetFrameHeight() + S(14);
        const float lw = std::clamp(av.x * 0.3f, S(240), S(420)), rw = std::clamp(av.x * 0.26f, S(240), S(380));
        const float h = av.y - footer;
        ImVec2 o = ImGui::GetCursorScreenPos();
        draw_library(lw, h);
        ImGui::SetCursorScreenPos(o + ImVec2(lw + S(6), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(6), 0));
        draw_editor(av.x - lw - rw - S(12), h);
        ImGui::PopStyleVar();
        ImGui::SetCursorScreenPos(o + ImVec2(av.x - rw, 0));
        draw_inspector(rw, h);
        ImGui::SetCursorScreenPos(o + ImVec2(0, h + S(8)));
        {
            const int np = (int)lib_targets().size();
            std::string sl = np > 1 ? "Send " + std::to_string(np) + " selected project packages to the library" : "Send the selected project package to the library";
            if (button(sl.c_str(), ui::Icon::None, false, ImVec2(0, 0), np > 0 && !LIB().master.read_only)) act_transfer(true);
            tooltip("Selection made in the project package list (behind this window)");
            ImGui::SameLine(0, S(16));
            ImGui::TextDisabled("Project packages stay the working copies.");
        }
        ImGui::SameLine(av.x - S(110));
        if (button("Close", ui::Icon::None, true, ImVec2(S(110), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { open = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (!open) { mgr_open_ = false; mode_ = 0; hover_ = ghover_ = -1; canvas_.cancel_draw(); canvas_.zoom_all(); canvas_.invalidate(); }
    (void)t;
}

void PackagesView::draw_toolbar() {
    // the editor carries its own vertical tool strip; the main toolbar keeps the panes + library manager
    if (icon_button("pk_lpane", ui::Icon::Toolbar, "Package list (N or Ctrl+[)", show_left_)) { show_left_ = !show_left_; app::set_ui_pref("Pkg.ShowList", show_left_); }
    ImGui::SameLine(0, S(2));
    if (icon_button("pk_rpane", ui::Icon::Settings, "Properties panel (Ctrl+])", show_right_)) { show_right_ = !show_right_; app::set_ui_pref("Pkg.ShowProps", show_right_); }
    tb_sep();
    if (gbtn("pk_mgr", G::Library, "Package Library Manager...\n(master library: browse, pull into the project, send to the library)", mgr_open_)) {
        mgr_open_ = true; mode_ = 1; hover_ = ghover_ = -1; canvas_.cancel_draw(); canvas_.zoom_all(); canvas_.invalidate();
    }
    tb_sep();
    if (icon_button("pk_za", ui::Icon::ZoomAll, "Zoom all (F)", false, shown() != nullptr)) canvas_.zoom_all();
}

bool PackagesView::on_command(int id) {
    switch (id) {
    case ID_EDIT_DELETE: if (mode_ == 1) act_lib_delete(); else if (lib_focus_ && psel_.empty() && gsel_.empty()) act_delete_unused(); else delete_selection(); return true;
    case ID_EDIT_SELECT_ALL:
        if (lib_focus_ && mode_ == 0 && !rows_.empty()) {   // R18c: list focused -> all shown packages
            std::set<std::string> all(rows_.begin(), rows_.end());
            if (!all.count(cur_)) select_package(rows_.front());
            lsel_ = all;
            return true;
        }
        if (Package* p = pkg()) { psel_.clear(); for (size_t i = 0; i < p->pads.size(); ++i) psel_.insert((int)i); canvas_.invalidate(); } return true;
    case ID_EDIT_COPY: copy_pads(); return true;
    case ID_EDIT_CUT: copy_pads(); delete_pads(); return true;
    case ID_EDIT_PASTE: paste_pads(); return true;
    case ID_VIEW_ZOOM_ALL: canvas_.zoom_all(); return true;
    case ID_VIEW_ZOOM_IN: canvas_.zoom_by(1.25); return true;
    case ID_VIEW_ZOOM_OUT: canvas_.zoom_by(0.8); return true;
    case ID_VIEW_ZOOM_100: canvas_.zoom_100(); return true;
    case ID_VIEW_ZOOM_RECT: canvas_.begin_zoom_rect(); return true;
    case ID_EDIT_SNAP: canvas_.set_snap(!canvas_.snapping()); return true;
    case ID_EDIT_GRID: canvas_.set_grid(!canvas_.grid_on(), canvas_.grid()); return true;
    }
    return false;
}

int PackagesView::cmd_state(int id) {
    const bool has = pkg() != nullptr;
    switch (id) {
    case ID_EDIT_DELETE: return (!psel_.empty() || !gsel_.empty() || (lib_focus_ && !lsel_.empty()) || (mode_ == 1 && !mlib_targets().empty())) ? 1 : 0;
    case ID_EDIT_SELECT_ALL: return has ? 1 : 0;
    case ID_EDIT_COPY: case ID_EDIT_CUT: return psel_.empty() ? 0 : 1;
    case ID_EDIT_PASTE: return clip_.empty() || !has ? 0 : 1;
    case ID_VIEW_ZOOM_ALL: case ID_VIEW_ZOOM_IN: case ID_VIEW_ZOOM_OUT: case ID_VIEW_ZOOM_100: case ID_VIEW_ZOOM_RECT: return 1;
    case ID_EDIT_SNAP: return 1 | (canvas_.snapping() ? 2 : 0);
    case ID_EDIT_GRID: return 1 | (canvas_.grid_on() ? 2 : 0);
    }
    return -1;
}

std::string PackagesView::status() const {
    if (mode_ == 1) {
        auto& L = const_cast<PackagesView*>(this)->curlib();
        auto it = L.packages.find(lcur_);
        return L.name + ": " + std::to_string(L.packages.size()) + " packages" + (it != L.packages.end() ? "  \xe2\x80\x94  " + lcur_ + " (" + std::to_string(it->second.pads.size()) + " pads)" : std::string());
    }
    if (cur_.empty()) return std::to_string(doc ? doc->prj.packages.size() : 0) + " packages";
    auto it = doc->prj.packages.find(cur_);
    if (it == doc->prj.packages.end()) return {};
    std::string s = cur_ + " \xe2\x80\x94 " + std::to_string(it->second.pads.size()) + " pads";
    if (!psel_.empty()) s += ", " + std::to_string(psel_.size()) + " selected";
    return s;
}

std::string PackagesView::status_right() const {
    if (!canvas_.hovered()) return {};
    Vec2 w = canvas_.mouse_world();
    return fmt_len_u8(w.x, false) + ", " + fmt_len_u8(w.y);
}

} // namespace

std::unique_ptr<View> make_packages_view() { return std::make_unique<PackagesView>(); }
// R17a: master package library for automatic resolution (pkg_wizard.h)
const std::map<std::string, Package>* master_package_library() { return &LIB().master.packages; }

} // namespace pcbp::gui
