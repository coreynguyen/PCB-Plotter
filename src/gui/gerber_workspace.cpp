// ============================================================================
// gerber_workspace.cpp — Gerber workspace v2 (ViewMate-style viewer + the
// CircuitCAM "add component from pads" workflow) and the Load Gerber Layers
// dialog.
//
//   * GPU renderer (gerber_gpu.*): layers tessellated once, drawn per frame
//     into a D3D11 texture; Direct2D fallback when D3D11 setup fails.
//   * Left panel: layers (eye / colour / name / type+side badges / pad
//     source layer; multi-select, move up/down, visibility menu, step
//     up/down with [ ]), Add Components actions. D-code details live in the
//     Query card and the Apertures... menu item.
//   * Gerber toolbar along the top of the canvas; readout strip under it.
//     Crosshair (ViewMate): docked to the cursor, Space undocks / docks, C
//     centres it on the selection, Shift+Space sets the REL anchor; the
//     readout shows the crosshair (ABS / REL, mm / in / mil).
//   * Layer list: sortable (draw order / name / type / side), group by side
//     or type; PgUp/PgDn or [ ] show previous / next layer only (active),
//     Ctrl+[ ] reorder (draw order). Pads used by added components hatched.
//   * View flip X / Y (working on the bottom side) with a clear banner.
//   * Tools: select, digitize, query, measure (snap), clearance (edge to
//     edge), relative origin, align layers, crop in/out, board frame,
//     zoom window. Fill modes solid / outline / sketch, layer compare (XOR),
//     Top/Bottom mirror.
//   * Navigation (ViewMate): wheel x1.39 at cursor, middle click = recentre,
//     middle drag = pan, arrows pan (Ctrl = screenful), +/- zoom,
//     Home fit, End = centre at cursor, 1..9 toggle layers (Ctrl = solo).
//
// Layer placement: world = translate(offset) * rotate(rot) * (mirror ? flipX : I).
// View state only (not persisted): crop masks, digitize buffer, matches,
// negative flags set in the UI, measure / relative origin.
// ============================================================================
#include "gapp.h"
#include "../core/gerber_set.h"
#include "imgui_internal.h"
#include <ole2.h>
#include <shellapi.h>
#include "gerber_gpu.h"
#include "import_extras.h"   // board image pixels (stencil vs bare-board compare)
#include "../app/board_render.h"
#include "../app/commands.h"
#include "../core/fileio.h"
#include "../core/package_edit.h"
#include "../core/plot_align.h"
#include "../core/package_resolve.h"   // R16b
#include "pkg_wizard.h"
#include "../io/archive.h"
#include "../io/digitize.h"
#include "../io/align_pads.h"   // R17a
#include "../io/gerber_match.h"  // R22b
#include "../core/arrays.h"      // R22a outline / array API
#include "../plugins/plugin.h"
#include "../ui/d2d.h"
#include "../ui/form.h"
#include "../ui/theme.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>
#include <cctype>

namespace pcbp::gui {
HWND main_hwnd();   // shell.cpp
bool plot_align_request_take(const app::Doc* d);   // align_tool.cpp (R14)
bool gerber_outline_tool_take(const app::Doc* d, bool& board);
using ui::W;
using ui::U;
using ui::Icon;
namespace gb = pcbp::gerber;
namespace dz = pcbp::digitize;
namespace gmatch = pcbp::gmatch;
using app::Doc;
using namespace pcbp::app;   // command ids

namespace {

// ---------------------------------------------------------------------------
// geometry helpers
// ---------------------------------------------------------------------------
Xform layer_xform(const GerberLayerRef& r) {
    Xform t = Xform::translate(r.offset.x, r.offset.y) * Xform::rotation(r.rot);
    if (r.mirror) t = t * Xform::scale(-1, 1);
    return t;
}
Vec2 lin(const Xform& x, Vec2 p) { return {x.a * p.x + x.c * p.y, x.b * p.x + x.d * p.y}; }
BBox xform_box(const Xform& t, const BBox& b) {
    if (b.empty()) return b;
    BBox r;
    r.add(t.apply({b.x0, b.y0})); r.add(t.apply({b.x1, b.y0}));
    r.add(t.apply({b.x1, b.y1})); r.add(t.apply({b.x0, b.y1}));
    return r;
}
bool box_inside(const BBox& in, const BBox& out) {
    return !in.empty() && in.x0 >= out.x0 && in.x1 <= out.x1 && in.y0 >= out.y0 && in.y1 <= out.y1;
}
double seg_dist(Vec2 p, Vec2 a, Vec2 b, Vec2* cp = nullptr) {
    Vec2 d = b - a;
    double l2 = d.x * d.x + d.y * d.y;
    double t = l2 > 1e-18 ? std::clamp(((p.x - a.x) * d.x + (p.y - a.y) * d.y) / l2, 0.0, 1.0) : 0;
    Vec2 c{a.x + d.x * t, a.y + d.y * t};
    if (cp) *cp = c;
    return dist(p, c);
}
bool point_in_poly(Vec2 p, const std::vector<Vec2>& poly) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec2 &a = poly[i], &b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}
// closest points between two segments
double segseg(Vec2 p1, Vec2 q1, Vec2 p2, Vec2 q2, Vec2& c1, Vec2& c2) {
    auto cr = [](Vec2 a, Vec2 b, Vec2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); };
    double d1 = cr(p1, q1, p2), d2 = cr(p1, q1, q2), d3 = cr(p2, q2, p1), d4 = cr(p2, q2, q1);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
        double t = d3 / (d3 - d4);
        c1 = c2 = Vec2{p1.x + (q1.x - p1.x) * t, p1.y + (q1.y - p1.y) * t};
        return 0;
    }
    double best = 1e300;
    Vec2 t;
    double d = seg_dist(p1, p2, q2, &t); if (d < best) { best = d; c1 = p1; c2 = t; }
    d = seg_dist(q1, p2, q2, &t); if (d < best) { best = d; c1 = q1; c2 = t; }
    d = seg_dist(p2, p1, q1, &t); if (d < best) { best = d; c1 = t; c2 = p2; }
    d = seg_dist(q2, p1, q1, &t); if (d < best) { best = d; c1 = t; c2 = q2; }
    return best;
}

const std::vector<std::wstring>& role_names() {
    static const std::vector<std::wstring> v{L"Other", L"Copper", L"Paste", L"Mask", L"Silk", L"Assembly", L"Drill", L"Outline"};
    return v;
}
std::string role_name_u8(LayerRole r) { size_t i = (size_t)r; return i < role_names().size() ? U(role_names()[i]) : "Other"; }
uint32_t role_color(LayerRole r, int n) { return gset::role_color(r, n); }   // LLM_API G1: shared with the headless import
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
// Menu item with a leading icon (keeps labels aligned with gui::menu_item()).
void undo_toast(const std::string& msg) { toast(msg, ToastKind::Success, 4.5f, "Undo", [] { post_command(ID_EDIT_UNDO); }); }
bool imenu(const char* label, Icon ic, const char* sc = nullptr, bool sel = false, bool en = true) {
    return menu_item_icon(label, ic, sc, sel, en);
}
bool has(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }

// layer type / side from file name conventions + X2 attributes (io/gerber.cpp)
gb::LayerGuess layer_guess(const GerberLayerRef& r) {
    static std::map<std::string, gb::LayerGuess> cache;
    auto it = cache.find(r.path);
    if (it != cache.end()) return it->second;
    gb::LayerGuess g = gb::guess_layer_function(r.path.empty() ? r.name : r.path, r.content);
    if (cache.size() > 4096) cache.clear();
    cache[r.path] = g;
    return g;
}
void guess_role(GerberLayerRef& r, int n) {
    gb::LayerGuess g = layer_guess(r);   // cached; same rule as gset::guess_role
    r.role = (LayerRole)std::clamp(g.role, 0, 7);
    r.side = g.side == 1 ? Side::Bottom : Side::Top;
    r.color = role_color(r.role, n);
}
const char* role_abbr(LayerRole r) {
    static const char* ab[] = {"Other", "Copper", "Paste", "Mask", "Silk", "Assy", "Drill", "Outline"};
    return ab[std::min((size_t)r, (size_t)7)];
}
// side badge text: TOP / BOT / IN<n> (inner from the file name), "-" for side-less layers
std::string side_badge(const GerberLayerRef& r) {
    if (r.role == LayerRole::Drill || r.role == LayerRole::Outline) return "ALL";
    if (r.side == Side::Bottom) return "BOT";
    gb::LayerGuess g = layer_guess(r);
    if (g.side == 2 && r.role == LayerRole::Copper) return "IN" + std::to_string(std::max(1, g.inner));
    return "TOP";
}
// small rounded badge; returns true when clicked
bool badge(const char* id, const std::string& text, ImU32 col, const char* tip, float min_w = 0) {
    const GTheme& T = th();
    ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    ImVec2 sz(std::max(min_w, ts.x + S(10)), ts.y + S(4));
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool r = ImGui::InvisibleButton(id, sz);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hv = ImGui::IsItemHovered();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), (col & 0x00FFFFFF) | (hv ? 0x60000000u : 0x38000000u), S(4));
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), (col & 0x00FFFFFF) | 0xA0000000u, S(4));
    dl->AddText(ImVec2(p.x + (sz.x - ts.x) / 2, p.y + S(2)), T.text, text.c_str());
    if (hv && tip) tooltip(tip);
    return r;
}
// small vector arrow button (up / down) in icon_button style
bool arrow_button(const char* id, bool up, const char* tip, bool enabled, float size) {
    const GTheme& T = th();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    bool r = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImGui::EndDisabled();
    const bool hv = enabled && ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hv) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), T.raised, S(5));
    const float c = size / 2, a = size * 0.24f;
    const ImU32 col = enabled ? T.text : T.text_faint;
    ImVec2 t0(p.x + c, p.y + c + (up ? -a : a)), t1(p.x + c - a, p.y + c + (up ? a * 0.4f : -a * 0.4f)), t2(p.x + c + a, p.y + c + (up ? a * 0.4f : -a * 0.4f));
    dl->AddTriangleFilled(t0, up ? t1 : t2, up ? t2 : t1, col);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && tip) tooltip(tip);
    return r && enabled;
}

std::string fmt_wh(double w, double h) { return fmt_len_u8(w, false) + " x " + fmt_len_u8(h); }
// aperture size: mils when the display unit is inch/mil, else mm with 3 decimals
std::string fmt_ap(double mm) {
    char b[32];
    if ((Units)app::settings().units == Units::MM) std::snprintf(b, sizeof b, "%.3f", mm);
    else std::snprintf(b, sizeof b, "%.1f", mm / 0.0254);
    std::string s = b;
    if (s.find('.') != std::string::npos) { while (s.back() == '0') s.pop_back(); if (s.back() == '.') s.pop_back(); }
    return s;
}

void normalize_pad(Pad& p) {
    double r = norm_deg(p.rot);
    if (std::fabs(r - 90) < 0.01 || std::fabs(r - 270) < 0.01) { std::swap(p.w, p.h); r = 0; }
    else if (std::fabs(r - 180) < 0.01) r = 0;
    p.rot = r;
}
std::string ref_prefix(const std::string& r) {
    size_t i = 0;
    while (i < r.size() && !std::isdigit((unsigned char)r[i])) ++i;
    return r.substr(0, i);
}
Vec2 object_center(const gb::Layer& L, const gb::Object& o) {
    switch (o.kind) {
    case gb::Object::Flash: return o.a;
    case gb::Object::Draw: return (o.a + o.b) * 0.5;
    case gb::Object::Arc: return o.center;
    default: return gb::object_bbox(L, o).center();
    }
}
std::string shape_name(const gb::Aperture& a) {
    switch (a.shape) {
    case gb::Aperture::Circle: return "Round";
    case gb::Aperture::Rect: return "Rectangle";
    case gb::Aperture::Obround: return "Obround";
    case gb::Aperture::Polygon: return "Polygon (" + std::to_string(a.verts) + ")";
    case gb::Aperture::Macro: return "Macro " + a.macro_name;
    }
    return "?";
}
const char* shape_short(const gb::Aperture& a) {
    switch (a.shape) {
    case gb::Aperture::Circle: return "C";
    case gb::Aperture::Rect: return "R";
    case gb::Aperture::Obround: return "O";
    case gb::Aperture::Polygon: return "P";
    case gb::Aperture::Macro: return "M";
    }
    return "?";
}
ImU32 col32(uint32_t argb, float a = 1.f) {
    int al = (int)(((argb >> 24) & 255) * a);
    return IM_COL32((argb >> 16) & 255, (argb >> 8) & 255, argb & 255, al);
}
ImU32 with_alpha(ImU32 c, float a) { return (c & 0x00FFFFFF) | ((ImU32)(std::clamp(a, 0.f, 1.f) * 255) << 24); }

// ---------------------------------------------------------------------------
// Spatial index of one parsed layer (hit test, snap, window queries)
// ---------------------------------------------------------------------------
struct ObjIndex {
    BBox ext;
    int nx = 1, ny = 1;
    double cw = 1, ch = 1;
    std::vector<std::vector<int>> cells;
    std::vector<int> big;
    std::vector<BBox> boxes;
    mutable std::vector<uint32_t> mark;
    mutable uint32_t stamp = 0;
    void build(const gb::Layer& L) {
        boxes.resize(L.objs.size());
        ext = BBox{};
        for (size_t i = 0; i < L.objs.size(); ++i) { boxes[i] = gb::object_bbox(L, L.objs[i]); ext.add(boxes[i]); }
        mark.assign(L.objs.size(), 0);
        if (ext.empty()) return;
        int n = std::clamp((int)std::sqrt(L.objs.size() / 3.0), 1, 300);
        nx = ny = n;
        cw = std::max(ext.w(), 1e-6) / nx; ch = std::max(ext.h(), 1e-6) / ny;
        cells.assign((size_t)nx * ny, {});
        for (size_t i = 0; i < boxes.size(); ++i) {
            const BBox& b = boxes[i];
            int x0 = cx(b.x0), x1 = cx(b.x1), y0 = cy(b.y0), y1 = cy(b.y1);
            if ((x1 - x0 + 1) * (y1 - y0 + 1) > 64) { big.push_back((int)i); continue; }
            for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) cells[(size_t)y * nx + x].push_back((int)i);
        }
    }
    int cx(double x) const { return std::clamp((int)((x - ext.x0) / cw), 0, nx - 1); }
    int cy(double y) const { return std::clamp((int)((y - ext.y0) / ch), 0, ny - 1); }
    template <class F> void query(const BBox& q, F&& f) const {
        if (ext.empty() || !q.intersects(ext)) { for (int i : big) if (boxes[i].intersects(q)) f(i); return; }
        if (++stamp == 0) { std::fill(mark.begin(), mark.end(), 0); stamp = 1; }
        int x0 = cx(q.x0), x1 = cx(q.x1), y0 = cy(q.y0), y1 = cy(q.y1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                for (int i : cells[(size_t)y * nx + x])
                    if (mark[i] != stamp) { mark[i] = stamp; if (boxes[i].intersects(q)) f(i); }
        for (int i : big) if (boxes[i].intersects(q)) f(i);
    }
};

double pen_half(const gb::Aperture* ap) {
    if (!ap) return 0;
    return ap->shape == gb::Aperture::Circle ? ap->w / 2 : std::max(ap->w, ap->h) / 2;
}

bool obj_hit(const gb::Layer& L, const gb::Object& o, Vec2 p, double tol) {
    const gb::Aperture* ap = o.kind == gb::Object::Region ? nullptr : gb::find_ap(L, o.dcode);
    switch (o.kind) {
    case gb::Object::Flash: {
        if (!ap) return dist(p, o.a) <= tol + 0.05;
        if (ap->shape == gb::Aperture::Circle) return dist(p, o.a) <= ap->w / 2 + tol;
        std::vector<bool> clr;
        auto polys = gb::flash_polys(*ap, o.a, 32, &clr);
        for (size_t k = 0; k < polys.size(); ++k) {
            if (k < clr.size() && clr[k]) continue;
            if (point_in_poly(p, polys[k])) return true;
            for (size_t i = 0; i < polys[k].size(); ++i) if (seg_dist(p, polys[k][i], polys[k][(i + 1) % polys[k].size()]) <= tol) return true;
        }
        return false;
    }
    case gb::Object::Draw: return seg_dist(p, o.a, o.b) <= pen_half(ap) + tol;
    case gb::Object::Arc: {
        auto pts = gb::arc_points(o, 72);
        for (size_t k = 1; k < pts.size(); ++k) if (seg_dist(p, pts[k - 1], pts[k]) <= pen_half(ap) + tol) return true;
        return false;
    }
    case gb::Object::Region:
        if (o.poly.size() >= 3 && point_in_poly(p, o.poly)) return true;
        for (size_t i = 0; i + 1 < o.poly.size(); ++i) if (seg_dist(p, o.poly[i], o.poly[i + 1]) <= tol) return true;
        return false;
    }
    return false;
}

// object as "fat segments" (for edge-to-edge clearance)
struct Fat { Vec2 a, b; double r; };
void object_fat(const gb::Layer& L, const gb::Object& o, std::vector<Fat>& out, std::vector<std::vector<Vec2>>& areas) {
    const gb::Aperture* ap = o.kind == gb::Object::Region ? nullptr : gb::find_ap(L, o.dcode);
    switch (o.kind) {
    case gb::Object::Flash: {
        if (!ap) { out.push_back({o.a, o.a, 0.05}); break; }
        if (ap->shape == gb::Aperture::Circle) { out.push_back({o.a, o.a, ap->w / 2}); areas.push_back({}); break; }
        std::vector<bool> clr;
        auto polys = gb::flash_polys(*ap, o.a, 48, &clr);
        for (size_t k = 0; k < polys.size(); ++k) {
            if (k < clr.size() && clr[k]) continue;
            for (size_t i = 0; i < polys[k].size(); ++i) out.push_back({polys[k][i], polys[k][(i + 1) % polys[k].size()], 0});
            areas.push_back(polys[k]);
        }
        break;
    }
    case gb::Object::Draw: out.push_back({o.a, o.b, pen_half(ap)}); break;
    case gb::Object::Arc: {
        auto pts = gb::arc_points(o, 180);
        for (size_t k = 1; k < pts.size(); ++k) out.push_back({pts[k - 1], pts[k], pen_half(ap)});
        break;
    }
    case gb::Object::Region:
        for (size_t i = 0; i < o.poly.size(); ++i) out.push_back({o.poly[i], o.poly[(i + 1) % o.poly.size()], 0});
        areas.push_back(o.poly);
        break;
    }
}

// ---------------------------------------------------------------------------
// Direct2D fallback geometry (used only when the D3D11 renderer fails)
// ---------------------------------------------------------------------------
Com<ID2D1PathGeometry> make_path(const std::vector<std::vector<Vec2>>& polys, bool filled) {
    Com<ID2D1PathGeometry> g;
    if (polys.empty()) return g;
    if (FAILED(gfx_d2d_factory()->CreatePathGeometry(g.put()))) return {};
    Com<ID2D1GeometrySink> s;
    if (FAILED(g->Open(s.put()))) return {};
    s->SetFillMode(D2D1_FILL_MODE_WINDING);
    std::vector<D2D1_POINT_2F> pts;
    for (auto& poly : polys) {
        if (poly.size() < 2) continue;
        pts.clear();
        for (size_t i = 1; i < poly.size(); ++i) pts.push_back(D2D1::Point2F((float)poly[i].x, (float)poly[i].y));
        s->BeginFigure(D2D1::Point2F((float)poly[0].x, (float)poly[0].y), filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddLines(pts.data(), (UINT32)pts.size());
        s->EndFigure(filled ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    }
    s->Close();
    return g;
}
ID2D1StrokeStyle* stroke_style(bool square) {
    static Com<ID2D1StrokeStyle> rnd, sq;
    Com<ID2D1StrokeStyle>& s = square ? sq : rnd;
    if (!s) {
        D2D1_CAP_STYLE cap = square ? D2D1_CAP_STYLE_SQUARE : D2D1_CAP_STYLE_ROUND;
        gfx_d2d_factory()->CreateStrokeStyle(D2D1::StrokeStyleProperties(cap, cap, cap, D2D1_LINE_JOIN_ROUND), nullptr, 0, s.put());
    }
    return s.get();
}
struct D2DStroke { double w = 0; bool square = false; Com<ID2D1PathGeometry> geo; };
struct D2DRun { bool clear = false; Com<ID2D1PathGeometry> fill, holes; std::vector<D2DStroke> strokes; };
void build_d2d(std::vector<D2DRun>& runs, const gb::Layer& L, const std::vector<int>& hidden, const std::vector<char>* crop) {
    runs.clear();
    std::set<int> hid(hidden.begin(), hidden.end());
    struct Acc { double w = 0; bool sq = false; std::vector<std::vector<Vec2>> lines; };
    std::vector<std::vector<Vec2>> fills, holes;
    std::map<int, Acc> strokes;
    bool cur_clear = false, any = false;
    auto flush = [&]() {
        if (!any) return;
        D2DRun r;
        r.clear = cur_clear;
        r.fill = make_path(fills, true);
        if (!cur_clear) r.holes = make_path(holes, true);
        for (auto& [k, a] : strokes) { (void)k; D2DStroke b; b.w = a.w; b.square = a.sq; b.geo = make_path(a.lines, false); if (b.geo) r.strokes.push_back(std::move(b)); }
        runs.push_back(std::move(r));
        fills.clear(); holes.clear(); strokes.clear();
        any = false;
    };
    for (size_t i = 0; i < L.objs.size(); ++i) {
        const gb::Object& o = L.objs[i];
        if (crop && i < crop->size() && (*crop)[i]) continue;
        if (o.kind != gb::Object::Region && hid.count(o.dcode)) continue;
        if (any && o.clear != cur_clear) flush();
        cur_clear = o.clear;
        any = true;
        const gb::Aperture* ap = o.kind == gb::Object::Region ? nullptr : gb::find_ap(L, o.dcode);
        auto add_flash = [&](Vec2 at) {
            if (!ap) return;
            std::vector<bool> clr;
            auto polys = gb::flash_polys(*ap, at, 40, &clr);
            for (size_t k = 0; k < polys.size(); ++k) (k < clr.size() && clr[k] ? holes : fills).push_back(std::move(polys[k]));
        };
        auto acc = [&]() -> Acc& {
            Acc& a = strokes[o.dcode];
            if (ap) { a.w = ap->shape == gb::Aperture::Rect ? (ap->w + ap->h) * 0.5 : ap->shape == gb::Aperture::Circle ? ap->w : std::min(ap->w, ap->h); a.sq = ap->shape == gb::Aperture::Rect; }
            return a;
        };
        switch (o.kind) {
        case gb::Object::Flash: add_flash(o.a); break;
        case gb::Object::Draw: if (dist(o.a, o.b) < 1e-7) add_flash(o.a); else acc().lines.push_back({o.a, o.b}); break;
        case gb::Object::Arc: acc().lines.push_back(gb::arc_points(o, 72)); break;
        case gb::Object::Region: if (o.poly.size() >= 3) fills.push_back(o.poly); break;
        }
    }
    flush();
}

// ---------------------------------------------------------------------------
// tools
// ---------------------------------------------------------------------------
namespace { bool outline_elem(const gb::Object& o); }   // R17b (outline tools section)
enum class Tool { Select, Digitize, Query, Measure, Clearance, RelOrigin, Add, Remove, DeleteComp, AlignSrc, AlignDst, CropIn, CropOut, Frame, ZoomRect, AlignView,
                  PlotAlign, FrameSel, PitchPick, Detect, PkgWizard };   // R14, R16b, R22b (Detect = repeated circuits)

const char* tool_hint(Tool t) {
    switch (t) {
    case Tool::Select: return "Select: click a component, drag a window to select components.";
    case Tool::Digitize: return "Add Component: click pads or drag boxes around them (always adds; Ctrl+click / Ctrl+drag removes), then right-click, Enter or Confirm.";
    case Tool::Query: return "Query: hover a feature to see its D-code, shape, size and position; click to pin the info.";
    case Tool::Measure: return "Measure: click two points (snaps to pad centres, endpoints, arc centres; hold Alt to disable snap).";
    case Tool::Clearance: return "Clearance: click two features to measure the edge-to-edge distance.";
    case Tool::RelOrigin: return "Relative origin: click a point (snapped); coordinates are shown relative to it.";
    case Tool::Add: return "Add Pads: click a pad or drag a box to add pads to the selection.";
    case Tool::Remove: return "Remove Pads: click a pad or drag a box to remove pads from the selection.";
    case Tool::DeleteComp: return "Delete Component: click an element of an assigned component to delete that component (Esc returns to Add Component).";
    case Tool::AlignSrc: return "Align Layer: click a feature on the layer to move (source).";
    case Tool::AlignDst: return "Align Layer: click the matching feature on the reference layer (target).";
    case Tool::CropIn: return "Crop In: drag a box; Gerber data inside it is hidden.";
    case Tool::CropOut: return "Crop Out: drag a box; Gerber data outside it is hidden.";
    case Tool::Frame: return "Circuit outline: drag a box around one circuit (one shot).";
    case Tool::ZoomRect: return "Zoom: drag a window to zoom into it.";
    case Tool::AlignView: return "Align View to Line: drag along a pad row or pad edge (snaps to pad centres / edges), adjust the handles or the angle, then Commit.";
    case Tool::PlotAlign: return "Align Plot to Gerber: click a placement (any circuit), drag a box around its pads on the Gerber, check the ghosts, Apply.";
    case Tool::FrameSel: return "Outline: Add = click / box line, arc and region elements (Ctrl removes); Draw = pick two corners, drag the corner handles (snapped, Alt = no snap). Right-click, Enter or Confirm, then choose Board or Circuit.";
    case Tool::PitchPick: return "Panel pitch: box features on the reference circuit, Enter; box the same features on the circuit one across and one up, Enter; then the counts.";
    case Tool::Detect: return "Detect repeated circuits: box a distinctive group of pads / elements on the reference circuit, Enter; review the candidates, Confirm.";
    case Tool::PkgWizard: return "Assign Packages from Gerber: box the pads at the marked placement (Ctrl+drag removes), then right-click / Enter / Build: the package is assigned to every placement using it and the next one is shown.";
    }
    return "";
}

Icon tool_icon(Tool t) {
    switch (t) {
    case Tool::Select: return Icon::Select;  case Tool::Digitize: return Icon::Digitize; case Tool::Query: return Icon::Query;
    case Tool::Measure: return Icon::Measure; case Tool::Clearance: return Icon::Clearance; case Tool::RelOrigin: return Icon::VirtualOrigin;
    case Tool::Add: return Icon::Assign; case Tool::Remove: return Icon::Unassign; case Tool::DeleteComp: return Icon::Delete;
    case Tool::AlignSrc: case Tool::AlignDst: return Icon::Offset; case Tool::CropIn: return Icon::CropIn; case Tool::CropOut: return Icon::CropOut;
    case Tool::Frame: return Icon::Board; case Tool::ZoomRect: return Icon::ZoomRect; case Tool::AlignView: return Icon::VerifyAngle;
    case Tool::PlotAlign: return Icon::Link; case Tool::FrameSel: return Icon::Board; case Tool::PitchPick: return Icon::AllImages; case Tool::Detect: return Icon::Wand;
    case Tool::PkgWizard: return Icon::PackageEditor;
    }
    return Icon::Question;
}

struct LayerCache {
    std::shared_ptr<gb::Layer> src;
    std::vector<int> hidden;
    unsigned crop_gen = ~0u;
    std::shared_ptr<ggpu::LayerMesh> mesh;
    std::vector<D2DRun> d2d;
    bool d2d_built = false;
    std::shared_ptr<gb::Layer> idx_src;
    ObjIndex index;
    std::map<int, int> usage;     // dcode -> count
};

struct Hit { int layer = -1, obj = -1; };

// ===========================================================================
// GerberWorkspace
// ===========================================================================
class GerberWorkspace : public View {
public:
    ViewKind kind() const override { return ViewKind::Gerber; }
    const char* title() const override { return "Gerber"; }
    ui::Icon icon() const override { return Icon::GerberView; }
    void draw(ImVec2 size) override;
    void draw_toolbar() override;
    bool on_command(int id) override;
    int cmd_state(int id) override;
    std::string status() const override;
    std::string status_right() const override;
    void on_activate() override { dirty_ = true; }

private:
    // ---- model helpers
    int nlayers() const { return doc ? (int)doc->prj.layers.size() : 0; }
    const gb::Layer* glayer(int i) const {
        if (!doc || i < 0 || i >= (int)doc->glayers.size() || i >= (int)doc->prj.layers.size()) return nullptr;
        return doc->glayers[i].get();
    }
    Xform lx(int i) const { return layer_xform(doc->prj.layers[i]); }
    BBox layer_world_box(int i) const { const gb::Layer* L = glayer(i); return L ? xform_box(lx(i), L->extent) : BBox{}; }
    BBox extent() const;
    const std::vector<char>* crop_of(int i) const;
    bool obj_visible(int li, int oi) const;
    bool negative(int i) const { auto it = neg_.find(doc->prj.layers[i].name); return it != neg_.end() && it->second; }
    void sync();
    void load_layers_async();
    bool loading_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
public:
    ~GerberWorkspace() override { *alive_ = false; }
private:
    void sync_active();
    void set_active(int i);
    LayerCache& cache(int i) { if ((int)cache_.size() < nlayers()) cache_.resize(nlayers()); return cache_[i]; }
    void ensure_cache(int i);
    const ObjIndex* index_of(int i);
    void swap_layers(int a, int b);
    void set_tool(Tool t);
    void refresh() { dirty_ = true; }

    // ---- view
    // Display-only view rotation vrot_ (deg, CCW) about the view centre; coordinates stay unrotated.
    Vec2 s2w(ImVec2 s) const {
        double dx = (s.x - pos_.x - size_.x * 0.5) / scale_;
        double dy = (s.y - pos_.y - size_.y * 0.5) / scale_;
        if (mirror_) dx = -dx;
        if (mirror_y_) dy = -dy;
        const double vx = dx, vy = -dy, c = vcos_, sn = vsin_;
        return {cx_ + c * vx + sn * vy, cy_ - sn * vx + c * vy};
    }
    ImVec2 w2s(Vec2 w) const {
        const double ux = w.x - cx_, uy = w.y - cy_;
        double x = (vcos_ * ux - vsin_ * uy) * scale_, y = (vsin_ * ux + vcos_ * uy) * scale_;
        if (mirror_) x = -x;
        if (mirror_y_) y = -y;
        return ImVec2((float)(pos_.x + size_.x * 0.5 + x), (float)(pos_.y + size_.y * 0.5 - y));
    }
    void set_view_rot(double deg) {
        deg = std::remainder(deg, 360.0);
        if (std::fabs(deg) < 1e-9) deg = 0;
        vrot_ = deg; vcos_ = std::cos(deg2rad(deg)); vsin_ = std::sin(deg2rad(deg)); dirty_ = true;
    }
    // world quad covered by a screen rectangle (handles view rotation)
    std::vector<Vec2> screen_quad(ImVec2 a, ImVec2 b) const {
        return {s2w(a), s2w(ImVec2(b.x, a.y)), s2w(b), s2w(ImVec2(a.x, b.y))};
    }
    void zoom_to(const BBox& b, double margin = 0.05);
    void zoom_all() { zoom_to(extent()); }
    void zoom_at(double f, ImVec2 s);
    void render_scene();
    void prof_finish(double view_ms, double input_ms, double render_ms);
    void render_d2d();
    void handle_input(bool hovered, bool active);
    void draw_overlay(ImDrawList* dl);
    void draw_panel(float w, float h);
    void draw_layers_panel();
    void draw_dcode_panel();
    void draw_digitize_panel();
    void draw_info_card(ImDrawList* dl);
    void draw_coord_box(ImDrawList* dl);
    void context_menu();
    void visibility_menu_items();
    void layer_menu_items(int i);
    void vis_op(int op);
    void step_layer(int dir);
    void move_selected(int dir);
    void show_apertures(int layer);
    void transform_layers();
    bool layer_selected(int i) const { return i >= 0 && i < (int)lsel_.size() && lsel_[i]; }
    int primary_layer() const { return sel_layer_ >= 0 && sel_layer_ < nlayers() ? sel_layer_ : act_; }
    double ysgn() const { return mirror_y_ ? -1 : 1; }

    // ---- picking
    bool hit_feature(Vec2 w, Hit& h, bool any_layer = true) const;
    bool snap_point(Vec2 w, double tol, Vec2& out, std::string* what = nullptr);
    std::string object_info(const Hit& h) const;

    // ---- actions
    void click(Vec2 w, int mods);
    void rect(const BBox& w, int mods);
    void digitize(const BBox& w, int mode);   // 0 new selection, 1 add, 2 subtract
    void modify_buffer(const BBox& lr, bool point, Vec2 lp, bool add);
    bool rebuild_from(std::vector<int> idx, bool fresh = false);
    bool ensure_ps();
    dz::DigitizeOptions dopt() const;
    std::vector<int> used_pads();
    std::vector<Pad> world_pads(const std::vector<Pad>& local, int layer) const;
    BBox pads_world_box(const std::vector<int>& idx) const;
    void clear_buffer() { have_res_ = false; res_ = {}; matches_.clear(); cur_match_ = -1; review_.clear(); review_cur_ = -1; }
    double view_angle() const { return vrot_ == 0 ? 0.0 : -vrot_; }   // angle of the aligned line (deg)
    void pan_px(double dx, double dy) {   // move the content by (dx, dy) screen pixels
        const ImVec2 o(pos_.x + size_.x * 0.5f, pos_.y + size_.y * 0.5f);
        const Vec2 a = s2w(o), b = s2w(ImVec2(o.x + (float)dx, o.y + (float)dy));
        cx_ -= b.x - a.x; cy_ -= b.y - a.y; dirty_ = true;
    }
    Vec2 av_snap(Vec2 w, bool free);
    void av_commit();
    void draw_align_view_card();
    int choose_pad_layer(const std::vector<Vec2>& quad);
    void orient_result();
    void draw_rot_banner(ImDrawList* dl);
    void accept();
    // compact, non-modal Add Component card (R10 §7)
    void open_add_card();
    void draw_add_card();
    bool commit_add();
    void close_add_card() { ac_ = {}; refresh(); }
    struct AddCard {
        bool open = false, adv = false, rot_edited = false, ever_active = false, focus = true, caret_end = true;
        bool want_ref = true;   // Reference should hold keyboard focus (re-acquired after canvas pan/zoom)
        bool dragging = false;
        int frames = 0, side = 0, method = 0;
        std::string ref, pkg, auto_pkg, err, fit_note;
        double rot = 0, det_rot = 0;
        ImVec2 size{0, 0};
    } ac_;
    ImVec2 ac_off_{0, 0};                  // user drag offset of the Add Component card (session; R11 P1-02)
    struct Recent { Vec2 pos; std::string pkg; };
    std::vector<Recent> recent_;           // components added in this session (highlighted)
    void export_layers(int mode);          // 0 selected, 1 visible, 2 choose
    std::vector<int> layer_targets(int i) const;   // context-menu target set: selection containing i, else {i}
    float panel_w_ = 0, dig_h_ = 0;        // resizable layer panel (splitters)
    int lhover_ = -1;
    void multi_accept();
    void walk_matches();
    void accept_all(bool number, const std::string& prefix, int start);
    std::string make_package(const std::string& name, const std::vector<Pad>& wpads, double rot, Side side);
    void scan(const dz::DigitizeResult& pattern, const std::string& pkg, double base_rot, Side side);
    void delete_comp(Vec2 w);
    void delete_selected();
    void align_click(Vec2 w);
    void clearance_click(Vec2 w);
    void crop(const BBox& w, bool inside);
    void show_hidden(int layer, bool all);
    void set_frame(const BBox& w);
    void frame_from_outline();
    void rotate_layers(int layer, bool all);
    void flip_layer(int layer);
    void reset_offset(int layer, bool all);
    void hide_aperture(int layer, int dcode, bool all);
    std::string next_ref(const std::string& prefix, int& n) const;
    // ---- layer list order (display only; doc order = draw order) / crosshair / added pads
    void compute_order();
    Vec2 crosshair() const { return ch_docked_ ? mouse_w_ : ch_w_; }
    bool selection_centroid(Vec2& out) const;
    void centre_crosshair_on_selection();
    void draw_gbar(float w);
    void draw_vbar(float w, float h);   // R22b
    void rename_layer(int i);
    const std::vector<char>& used_mask();
    std::vector<int> disp_order_;          // layer indices in list order (sorted / grouped)
    std::vector<int> disp_group_;          // group key per disp_order_ entry (-1 = no grouping)
    int sort_col_ = 0;                     // 0 draw order, 1 name, 2 type, 3 side
    bool sort_desc_ = false;
    int group_by_ = 0;                     // 0 none, 1 side, 2 type
    bool ch_docked_ = true;                // crosshair follows the cursor
    Vec2 ch_w_{};                          // crosshair position when undocked
    bool show_added_ = true;               // tint pads already used by added components
    uint64_t used_gen_ = ~0ull;
    const void* used_ps_ = nullptr;
    size_t used_npads_ = 0;
    std::vector<char> used_mask_;

    // ---- state
    double scale_ = 4, cx_ = 50, cy_ = 40;
    double vrot_ = 0, vcos_ = 1, vsin_ = 0;    // display-only view rotation (render angle = -aligned line angle)
    // Align View to Line tool
    bool av_has_ = false;          // a line exists
    Vec2 av_a_{}, av_b_{};
    int av_drag_ = -1;             // 0 = handle a, 1 = handle b, 2 = new line (b follows the cursor)
    int av_step_ = 0;              // angle snap step in degrees (0 = free)
    std::string av_snap_what_;
    std::vector<Vec2> band_quad_;  // world quad of the last rubber band (view rotation)
    // auto-scan: rotated (any angle) matches wait for review
    bool scan_any_angle_ = false;
    struct Review { Vec2 pos; double rot = 0; Side side = Side::Top; std::vector<int> pads; std::string pkg; BBox wbox; double conf = 0; };
    std::vector<Review> review_;
    int review_cur_ = -1;
    dz::RotationGuess rot_guess_;
    bool mirror_ = false, mirror_y_ = false, fitted_ = false;
    std::vector<char> lsel_;      // multi-selection in the layer list
    int lsel_anchor_ = -1;
    std::set<std::string> used_layers_;   // layers already used to add components (view state)
    // coordinate readout (inspection only)
    bool anchor_on_ = false;
    Vec2 anchor_{};
    int coord_mode_ = 0;          // 0 absolute, 1 relative to anchor
    int coord_units_ = -1;        // -1 = display units, 0 mm, 1 in, 2 mil
    bool space_down_ = false, space_panned_ = false;
    bool crosshair_ = true;
    ImVec2 pos_{}, size_{};
    bool dirty_ = true;
    uint64_t seen_gen_ = 0;
    size_t seen_layers_ = (size_t)-1;
    ggpu::Renderer gpu_;
    bool gpu_tried_ = false, gpu_ok_ = false;
    std::string gpu_err_;
    RenderTexture d2d_tex_;
    std::vector<LayerCache> cache_;
    double render_ms_ = 0, render_ms_avg_ = 0;
    // R11 P2-12: per-redraw CPU breakdown of this view (event-driven: one sample per redraw, never an
    // FPS estimate). input = handle_input (hover pick / snapping / hit-test), render = render_scene
    // (only when dirty), overlay = everything drawn on top of the cached texture, view = whole draw().
    double prof_input_ = 0, prof_render_ = 0, prof_overlay_ = 0, prof_view_ = 0, prof_view_max_ = 0;
    long long prof_n_ = 0; FILE* prof_log_ = nullptr; bool prof_checked_ = false;
    ggpu::FillMode fill_ = ggpu::FillMode::Solid;
    bool compare_ = false;
    int cmp_a_ = -1, cmp_b_ = -1;
    int hl_layer_ = -1, hl_dcode_ = -1, hl_prev_layer_ = -1, hl_prev_dcode_ = -1;
    int sel_layer_ = -1;          // layer shown in the D-code panel
    std::map<std::string, bool> neg_;
    // Screen blend per layer (view state): overlapping screened layers add up (magenta + green = white), like
    // ViewMate's compare; 0 = the layer's own colour, else a view-only colour
    std::map<std::string, uint32_t> screen_;
    bool screened(int i) const { return screen_.count(doc->prj.layers[i].name) != 0; }
    uint32_t screen_color(int i) const { auto it = screen_.find(doc->prj.layers[i].name); return it == screen_.end() || !it->second ? doc->prj.layers[i].color : it->second; }
    // board image under the Gerbers (the project's registered board images): black-and-white with a threshold for
    // the bare-board vs stencil / paste compare
    bool img_show_ = false, img_bw_ = true, img_inv_ = false, img_screen_ = true;
    int img_idx_ = 0;
    float img_thr_ = 0.5f, img_alpha_ = 1.f;
    uint32_t img_tint_ = 0xFF00E000;
    gui::StaticTexture img_tex_;
    uint64_t img_key_ = 0;
    std::string img_err_;
    void draw_board_image();
    void board_image_popup();
    bool show_unused_ = false;

    Tool tool_ = Tool::Digitize;
    std::string hint_text_;
    double hint_t0_ = 0;
    int act_ = -1;
    bool hide_comps_ = false;
    // interaction
    enum class Drag { None, Pending, Band, Pan, MidPending } drag_ = Drag::None;
    ImVec2 down_s_{};
    Vec2 down_w_{};
    int down_mods_ = 0;
    Vec2 mouse_w_{};
    bool hovered_ = false;
    Vec2 ctx_w_{};
    Hit ctx_hit_;
    // query / measure
    Hit hover_hit_, pinned_hit_;
    bool have_pin_ = false;
    bool meas_has_a_ = false, meas_done_ = false;
    Vec2 meas_a_{}, meas_b_{};
    std::string meas_snap_;
    bool snap_valid_ = false;
    Vec2 snap_pt_{};
    bool have_rel_ = false;
    Vec2 rel_{};
    Hit clr_a_;
    bool clr_done_ = false;
    Vec2 clr_p_{}, clr_q_{};
    double clr_d_ = 0;
    // digitize
    dz::PadSet ps_;
    std::shared_ptr<gb::Layer> ps_layer_;
    std::vector<int> ps_hidden_;
    unsigned ps_crop_ = ~0u;
    int ps_index_ = -1;
    const gb::Layer* ps_drill_ = nullptr;
    bool have_res_ = false;
    dz::DigitizeResult res_;
    struct BufMatch { Vec2 pos; double rot = 0; Side side = Side::Top; std::vector<int> pads; std::string pkg; BBox wbox; };
    std::vector<BufMatch> matches_;
    int cur_match_ = -1;
    std::string last_prefix_;
    // crops
    struct Crop { std::shared_ptr<gb::Layer> L; std::vector<char> hide; };
    std::map<int, Crop> crops_;
    unsigned crop_gen_ = 1;
    // align
    int align_layer_ = -1;
    Vec2 align_pt_{};
    bool align_two_ = false;      // two point align (move + rotate)
    int align_stage_ = 0;         // 0 = first pair, 1 = second pair
    Vec2 align_s1_{}, align_d1_{};
    // ---- R14: plot <-> Gerber alignment (specs/PANELIZE.md §1), frames from Gerber, panel tools
    bool show_all_plot_ = false;          // Gerber tab: show plot elements not built from Gerber
    bool plot_visible(const Component& c) const { return c.from_gerber || show_all_plot_ || tool_ == Tool::PlotAlign || tool_ == Tool::PkgWizard; }
    // R17a (REVIEW_V16 §1): pick a placement -> box its pads -> ghost preview -> Apply (core/plot_align.h)
    struct PlotAlignState {
        int ref = -1, image = 0;          // placement being referenced (index into comps) + panel instance
        int layer = -1;                   // Gerber layer the pads were selected on
        std::vector<AlignRef> refs;       // committed references (placement + boxed pads)
        int rot_mode = 0;                 // 0..3 = 0/90/180/270, 4 = from the references / package pads
        bool mirror = false, all_layers = true, preview = true;
        int dir = 0;                      // 0 move Gerbers to plot, 1 move plot to Gerbers
        bool ok = false;
        AlignFit fit;
        std::string why;                  // solve_alignment hint
        ImVec2 card_off{0, 0};            // the card is movable (drag its title)
        int applied = 0;                  // Applies in this session (Undo last)
        std::vector<GhostPlacement> ghosts;
    } pa_;
    std::vector<Vec2> pa_cad_pads() const;
    void pa_refit();
    void pa_rect(const BBox& w);
    void pa_apply();
    void enter_plot_align();
    int pa_hit(Vec2 w, double tol, int& image) const;
    void pa_reset() { pa_.refs.clear(); pa_.ok = false; pa_.ghosts.clear(); pa_.why.clear(); }
    void pa_global(double rot, bool mirror);
    void draw_pa_overlay(ImDrawList* dl);
    void draw_pa_card();
    // R17b (REVIEW_V16 §3, specs/OUTLINES.md): Board Outline / Circuit Outline tools. prefer_board = target
    // (true = board outline = Project::panel_frame, false = circuit outline = board_frame). mode 0 = draw a
    // rectangle (drag / two clicks, snapped), 1 = pick Gerber line / arc / region elements across layers.
    struct FrameCand { std::vector<Vec2> outline; BBox box; bool pending = false; int n = 0; bool prefer_board = false; int corners = 0; Vec2 c0;
                       int mode = 0; std::vector<Hit> picks; int drag_corner = -1; } fsel_;
    // R22b (REVIEW_V21 §6.2): one Confirm for every select / draw tool (right-click, Enter, Confirm button)
    bool can_confirm() const;
    void confirm_action();
    const char* confirm_label() const;
    bool otype_open_ = false;                          // Board / Circuit choice popup after an outline Confirm
    bool otype_board_ = false;                         // last choice (popup default)
    void draw_outline_type_popup();
    bool outline_commit_as(bool board);                // through core/arrays.h (R22a API)
    void outline_draw_handles_input(bool hovered);     // Draw Outline corner handles (snapped)
    // R22b: generic Gerber element selection (pitch / detect): click / box additive, Ctrl removes
    std::vector<gmatch::ElemRef> elems_in_box(const BBox& w, bool outline_only) const;
    std::vector<gmatch::Feature> sel_features(const gmatch::ElemSelection& s) const;
    void draw_esel(ImDrawList* dl, const gmatch::ElemSelection& s, ImU32 col);
    // R22b (REVIEW_V21 §9): two-selection diagonal panel pitch -> linear array (R22a create_linear_array)
    struct PitchUi { int stage = 0; int mode = 0; gmatch::ElemSelection sel; Vec2 c0, c1; BBox b0, b1; int n0 = 0;
                     gmatch::PitchResult r; int nx = 2, ny = 2, root_c = 0, root_r = 0; std::string warn; } pt_;
    void pitch_confirm();
    void pitch_apply();
    void tool_exit(const std::string& msg);            // clean exit: temp state cleared, back to Add Component
    // R22b (REVIEW_V21 §10): repeated-circuit detection from Gerber geometry -> nonlinear array
    struct DetectUi { int stage = 0; gmatch::ElemSelection sel; std::vector<gmatch::Candidate> cands; std::vector<char> on;
                      double tol = 0.05; bool rot[4] = {true, true, true, true}; std::string why; Vec2 ref_c; BBox ref_box; int hover = -1; } dt_;
    void detect_run();
    void detect_apply();
    void draw_sel_card();
    void draw_sel_overlay(ImDrawList* dl);
    void test_r22b();
    void frame_sel(const BBox& w);
    void commit_frame(bool panel, bool outline);
    void outline_enter(bool board, int mode);
    void outline_reset_keep();                         // clear the candidate, keep target + mode
    void outline_pick_click(Vec2 w, bool remove);
    void outline_pick_box(const BBox& w, bool remove);
    void outline_rebuild();                            // picks -> box / closed contour
    void outline_draw_rect(const BBox& w);             // drag in Draw mode (corners snapped)
    bool outline_ready() const { return fsel_.mode == 0 ? fsel_.corners == 2 && fsel_.box.w() > 0.01 && fsel_.box.h() > 0.01 : !fsel_.picks.empty() && fsel_.box.w() > 0.01 && fsel_.box.h() > 0.01; }
    bool outline_commit();
    void outline_cancel();
    void draw_outline_overlay(ImDrawList* dl);
    void test_r17b();
    void draw_r14_overlay(ImDrawList* dl);
    void draw_r14_card();
    void test_r14();
    // ---- R16b: package acquisition wizard (core/package_resolve.h, pkg_wizard.h)
    struct WizardUi { std::vector<Pad> pads; int comp = -1; std::string pkg; bool need_align = false;
                      std::string built, result; ImVec2 card_off{0, 0}; } wz_;   // R17a: last build stays visible
    bool wz_resume_ = false;              // re-enter the wizard after Align Plot to Gerber was applied
    void enter_pkg_wizard(const std::string& start);
    void wz_goto();
    void wz_rect(const BBox& w, int mods);
    void wz_build();
    void draw_wz_overlay(ImDrawList* dl);
    void draw_wz_card();
    void test_r16b();
};

// ===========================================================================
// model
// ===========================================================================
BBox GerberWorkspace::extent() const {
    BBox b;
    if (!doc) return b;
    for (int i = 0; i < nlayers(); ++i) if (doc->prj.layers[i].visible) b.add(layer_world_box(i));
    if (b.empty()) for (int i = 0; i < nlayers(); ++i) b.add(layer_world_box(i));
    for (auto& poly : doc->prj.board_frame) for (auto& p : poly) b.add(p);
    if (b.empty() && !doc->prj.comps.empty()) b = doc->prj.board_bbox();
    return b;
}

const std::vector<char>* GerberWorkspace::crop_of(int i) const {
    auto it = crops_.find(i);
    if (it == crops_.end() || it->second.L.get() != glayer(i)) return nullptr;
    return &it->second.hide;
}

bool GerberWorkspace::obj_visible(int li, int oi) const {
    const gb::Layer* L = glayer(li);
    if (!L) return false;
    const auto* cr = crop_of(li);
    if (cr && oi < (int)cr->size() && (*cr)[oi]) return false;
    const gb::Object& o = L->objs[oi];
    if (o.kind == gb::Object::Region) return true;
    const auto& h = doc->prj.layers[li].hidden_dcodes;
    return std::find(h.begin(), h.end(), o.dcode) == h.end();
}

// Parse (and, with the GPU renderer, tessellate) every Gerber layer on a worker thread with a
// progress card. The worker uses a scratch Doc holding a copy of the layer refs; results (parsed
// layers, auto-detected formats, meshes) are applied on the UI thread if the layer list is unchanged.
void GerberWorkspace::load_layers_async() {
    if (loading_ || doc->prj.layers.empty()) { if (doc->prj.layers.empty()) doc->glayers.clear(); return; }
    loading_ = true;
    struct Job { app::Doc tmp; std::vector<std::shared_ptr<ggpu::LayerMesh>> meshes; size_t n = 0; };
    auto job = std::make_shared<Job>();
    job->tmp.prj.layers = doc->prj.layers;
    job->n = doc->prj.layers.size();
    const bool mesh = gpu_ok_;
    auto alive = alive_;
    app::DocPtr d = doc;
    run_with_progress("Loading Gerber layers", [job, mesh](Progress& p) {
        const size_t n = job->n;
        job->tmp.glayers.assign(n, nullptr);
        job->meshes.assign(n, nullptr);
        for (size_t i = 0; i < n; ++i) {
            if (p.cancelled()) return;
            std::string m = "Parsing " + job->tmp.prj.layers[i].name + "  (" + std::to_string(i + 1) + "/" + std::to_string(n) + ")";
            p.set((float)i / (float)(mesh ? 2 * n : n), m.c_str());
            std::string e;
            job->tmp.parse_layer(i, e);
        }
        if (mesh)
            for (size_t i = 0; i < n; ++i) {
                if (p.cancelled()) return;
                const auto& L = job->tmp.glayers[i];
                if (!L || !job->tmp.prj.layers[i].visible) continue;
                std::string m = "Tessellating " + job->tmp.prj.layers[i].name;
                p.set((float)(n + i) / (float)(2 * n), m.c_str());
                job->meshes[i] = ggpu::build_mesh(*L, job->tmp.prj.layers[i].hidden_dcodes, nullptr);
            }
        p.set(1.f, "Done");
    }, [this, job, alive, d](bool ok) {
        if (!*alive) return;
        loading_ = false;
        if (d->prj.layers.size() != job->n) { d->parse_all_layers(); refresh(); return; }
        if (!ok) { d->parse_all_layers(); refresh(); return; }   // cancelled: fall back to a direct parse so the view is usable
        d->glayers = job->tmp.glayers;
        size_t objs = 0;
        int bad = 0;
        for (size_t i = 0; i < job->n; ++i) {
            if (d->prj.layers[i].format.empty()) d->prj.layers[i].format = job->tmp.prj.layers[i].format;
            if (d->glayers[i]) objs += d->glayers[i]->objs.size(); else ++bad;
            if (job->meshes[i] && d->glayers[i]) {
                LayerCache& c = cache((int)i);
                c = LayerCache{};
                c.src = d->glayers[i];
                c.hidden = d->prj.layers[i].hidden_dcodes;
                c.crop_gen = 0;
                c.mesh = job->meshes[i];
                for (auto& o : c.src->objs) if (o.kind != gb::Object::Region) ++c.usage[o.dcode];
            }
        }
        fitted_ = false;
        seen_layers_ = job->n;   // keep the prefilled caches
        seen_gen_ = 0;           // force a sync pass
        refresh();
        toast("Loaded " + std::to_string(job->n) + " Gerber layers (" + std::to_string(objs) + " objects)" +
              (bad ? ", " + std::to_string(bad) + " failed to parse" : std::string()), bad ? ToastKind::Warning : ToastKind::Success);
    });
}

void GerberWorkspace::sync() {
    if (!doc) return;
    if (doc->glayers.size() != doc->prj.layers.size()) { load_layers_async(); if (loading_) return; }
    if (doc->gen != seen_gen_ || doc->prj.layers.size() != seen_layers_) {
        if (doc->prj.layers.size() != seen_layers_) { cache_.clear(); if (seen_layers_ == 0 || seen_layers_ == (size_t)-1) fitted_ = false; }
        seen_gen_ = doc->gen;
        seen_layers_ = doc->prj.layers.size();
        sync_active();
        if (ps_index_ >= 0 && (ps_index_ != act_ || act_ < 0 || ps_layer_ != doc->glayers[act_])) { ps_layer_.reset(); ps_index_ = -1; clear_buffer(); }
        for (auto it = crops_.begin(); it != crops_.end();) {
            if (it->first >= nlayers() || it->second.L.get() != glayer(it->first)) it = crops_.erase(it);
            else ++it;
        }
        if (sel_layer_ >= nlayers()) sel_layer_ = -1;
        lsel_.resize(nlayers(), 0);
        if (cmp_a_ >= nlayers() || cmp_b_ >= nlayers()) compare_ = false;
        dirty_ = true;
    }
}

void GerberWorkspace::sync_active() {
    int n = nlayers();
    int flagged = -1;
    for (int i = 0; i < n; ++i) if (doc->prj.layers[i].digitize && glayer(i)) { flagged = i; break; }
    if (flagged >= 0) { act_ = flagged; return; }
    act_ = -1;
    for (LayerRole r : {LayerRole::Paste, LayerRole::Copper, LayerRole::Mask})
        for (int i = 0; i < n && act_ < 0; ++i) if (doc->prj.layers[i].role == r && glayer(i)) act_ = i;
    for (int i = 0; i < n && act_ < 0; ++i) if (glayer(i)) act_ = i;
    if (act_ >= 0) doc->prj.layers[act_].digitize = true;   // view state; not a user edit
}

void GerberWorkspace::set_active(int i) {
    if (i < 0 || i >= nlayers() || i == act_) return;
    for (int k = 0; k < nlayers(); ++k) doc->prj.layers[k].digitize = k == i;
    act_ = i;
    clear_buffer();
    gui::status("Pads are read from " + doc->prj.layers[i].name + " (the selected layer)");
    refresh();
}

void GerberWorkspace::ensure_cache(int i) {
    LayerCache& c = cache(i);
    const gb::Layer* L = glayer(i);
    if (!L) { c = LayerCache{}; return; }
    const auto& hid = doc->prj.layers[i].hidden_dcodes;
    const std::vector<char>* cr = crop_of(i);
    unsigned cg = cr ? crop_gen_ : 0;
    if (c.src.get() == L && c.hidden == hid && c.crop_gen == cg && (c.mesh || c.d2d_built)) return;
    c.src = doc->glayers[i];
    c.hidden = hid;
    c.crop_gen = cg;
    c.mesh.reset();
    c.d2d.clear();
    c.d2d_built = false;
    if (gpu_ok_) c.mesh = ggpu::build_mesh(*L, hid, cr);
    else { build_d2d(c.d2d, *L, hid, cr); c.d2d_built = true; }
    c.usage.clear();
    for (auto& o : L->objs) if (o.kind != gb::Object::Region) ++c.usage[o.dcode];
}

const ObjIndex* GerberWorkspace::index_of(int i) {
    const gb::Layer* L = glayer(i);
    if (!L) return nullptr;
    LayerCache& c = cache(i);
    if (c.idx_src.get() != L) { c.index = ObjIndex{}; c.index.build(*L); c.idx_src = doc->glayers[i]; }
    return &c.index;
}

void GerberWorkspace::swap_layers(int a, int b) {
    auto& ls = doc->prj.layers;
    auto& gs = doc->glayers;
    if (a < 0 || b < 0 || a >= (int)ls.size() || b >= (int)ls.size()) return;
    std::swap(ls[a], ls[b]);
    if ((int)gs.size() > std::max(a, b)) std::swap(gs[a], gs[b]);
    if ((int)cache_.size() > std::max(a, b)) std::swap(cache_[a], cache_[b]);
    Crop ca, cb;
    bool ha = crops_.count(a), hb = crops_.count(b);
    if (ha) ca = crops_[a];
    if (hb) cb = crops_[b];
    crops_.erase(a); crops_.erase(b);
    if (ha) crops_[b] = ca;
    if (hb) crops_[a] = cb;
    if (sel_layer_ == a) sel_layer_ = b; else if (sel_layer_ == b) sel_layer_ = a;
    if ((int)lsel_.size() > std::max(a, b)) std::swap(lsel_[a], lsel_[b]);
    act_ = -1;
    ps_layer_.reset(); ps_index_ = -1; clear_buffer();
    doc->changed(ui::CH_LAYERS);
}

void GerberWorkspace::set_tool(Tool t) {
    tool_ = (tool_ == t && t != Tool::Digitize) ? Tool::Digitize : t;
    if (tool_ != Tool::PitchPick) pt_ = PitchUi{};
    if (tool_ != Tool::Detect) dt_ = DetectUi{};
    if (tool_ != Tool::FrameSel) { fsel_ = FrameCand{}; otype_open_ = false; }
    if (tool_ != Tool::AlignDst) align_layer_ = -1;
    if (tool_ != Tool::AlignSrc && tool_ != Tool::AlignDst) align_stage_ = 0;
    if (tool_ != Tool::Measure) { meas_has_a_ = false; meas_done_ = false; }
    if (tool_ != Tool::Clearance) { clr_a_ = {}; clr_done_ = false; }
    if (tool_ != Tool::Query) have_pin_ = false;
    gui::status(tool_hint(tool_));
    refresh();
}

std::string GerberWorkspace::next_ref(const std::string& prefix, int& n) const {
    for (;; ++n) {
        std::string r = prefix + std::to_string(n);
        if (!doc->prj.find_ref(r)) { ++n; return r; }
    }
}

// ===========================================================================
// view / rendering
// ===========================================================================
void GerberWorkspace::zoom_to(const BBox& b0, double margin) {
    BBox b = b0.empty() ? BBox(0, 0, 100, 80) : b0;
    if (size_.x < 4 || size_.y < 4) return;
    margin = std::clamp(margin, 0.0, 0.45);   // R17a: a margin >= 0.5 gave a negative fit = maximum zoom-out
    double w = std::max(b.w(), 0.2), h = std::max(b.h(), 0.2);
    scale_ = std::clamp(std::min(size_.x * (1 - 2 * margin) / w, size_.y * (1 - 2 * margin) / h), 0.005, 200000.0);
    cx_ = b.center().x;
    cy_ = b.center().y;
    dirty_ = true;
}

void GerberWorkspace::zoom_at(double f, ImVec2 s) {
    Vec2 before = s2w(s);
    scale_ = std::clamp(scale_ * f, 0.005, 200000.0);
    Vec2 after = s2w(s);
    cx_ += before.x - after.x;
    cy_ += before.y - after.y;
    dirty_ = true;
}

// ---- board image under the Gerbers ------------------------------------------------------------------------
// The project's board image (Import > Board Image, registered in Board Images) in plot coordinates; after the
// Gerbers are moved onto the plot, Gerber world = plot. Drawn as a 16 x 16 grid of its homography.
void GerberWorkspace::draw_board_image() {
    const auto& imgs = doc->prj.board_images;
    if (imgs.empty()) return;
    if (img_idx_ < 0 || img_idx_ >= (int)imgs.size()) img_idx_ = 0;
    const BoardImage& bi = imgs[(size_t)img_idx_];
    if (bi.px_w <= 0 || bi.px_h <= 0) return;
    const uint64_t key = ((uint64_t)bi.revision << 32) ^ std::hash<std::string>()(bi.path) ^ (uint64_t)bi.data.size() ^ ((uint64_t)img_idx_ << 56);
    if (!img_tex_.imgui_id() || key != img_key_) {
        img_tex_.release();
        img_err_.clear();
        int w = 0, h = 0;
        const std::vector<uint8_t>* px = nullptr;
        if (app::board_image_pixels(bi, 8192, w, h, px, img_err_) && px && img_tex_.create(w, h, px->data(), w * 4)) { img_tex_.w = w; img_tex_.h = h; }
        img_key_ = key;
    }
    if (!img_tex_.imgui_id()) return;
    constexpr int N = 16;
    std::vector<Vec2> grid((size_t)(N + 1) * (N + 1));
    for (int j = 0; j <= N; ++j)
        for (int i = 0; i <= N; ++i) grid[(size_t)j * (N + 1) + i] = h_apply(bi.H, Vec2{bi.px_w * (double)i / N, bi.px_h * (double)j / N});
    gpu_.draw_image(img_tex_.imgui_id(), grid.data(), N, img_tint_, img_alpha_, img_bw_ ? img_thr_ : -1.f, img_inv_, img_screen_);
}

void GerberWorkspace::board_image_popup() {
    section_header("BOARD IMAGE");
    const auto& imgs = doc->prj.board_images;
    if (imgs.empty()) { ImGui::TextDisabled("No board image: File > Import > Board Image."); return; }
    if (ImGui::Checkbox("Show under the Gerbers", &img_show_)) dirty_ = true;
    if (imgs.size() > 1) {
        ImGui::SetNextItemWidth(S(240));
        if (img_idx_ >= (int)imgs.size()) img_idx_ = 0;
        if (ImGui::BeginCombo("Image", imgs[(size_t)img_idx_].name.c_str())) {
            for (int k = 0; k < (int)imgs.size(); ++k) if (ImGui::Selectable(imgs[(size_t)k].name.c_str(), k == img_idx_)) { img_idx_ = k; dirty_ = true; }
            ImGui::EndCombo();
        }
    }
    if (ImGui::Checkbox("Black and white", &img_bw_)) dirty_ = true;
    if (img_bw_) {
        ImGui::SetNextItemWidth(S(240));
        if (ImGui::SliderFloat("Threshold", &img_thr_, 0.02f, 0.98f, "%.2f")) dirty_ = true;
        if (ImGui::Checkbox("Invert (dark pads)", &img_inv_)) dirty_ = true;
        float c[3] = {((img_tint_ >> 16) & 255) / 255.f, ((img_tint_ >> 8) & 255) / 255.f, (img_tint_ & 255) / 255.f};
        if (ImGui::ColorEdit3("Colour", c, ImGuiColorEditFlags_NoInputs)) {
            img_tint_ = 0xFF000000u | (uint32_t)std::lround(c[0] * 255) << 16 | (uint32_t)std::lround(c[1] * 255) << 8 | (uint32_t)std::lround(c[2] * 255);
            dirty_ = true;
        }
    }
    ImGui::SetNextItemWidth(S(240));
    if (ImGui::SliderFloat("Opacity", &img_alpha_, 0.1f, 1.f, "%.2f")) dirty_ = true;
    if (ImGui::Checkbox("Screen blend", &img_screen_)) dirty_ = true;
    if (!img_err_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", img_err_.c_str());
    ImGui::Separator();
    if (button("Bare board vs stencil", Icon::Compare, true)) {
        // the scan in green, the paste / stencil layers screened in magenta: white = opening on a pad, green = a pad
        // with no opening, magenta = an opening with no pad
        img_show_ = img_bw_ = img_screen_ = true;
        img_tint_ = 0xFF00FF00u;
        screen_.clear();
        int n = 0;
        for (int i = 0; i < nlayers(); ++i) {
            GerberLayerRef& r = doc->prj.layers[i];
            if (r.role == LayerRole::Paste && r.visible) { screen_[r.name] = 0xFFFF00FFu; ++n; }
        }
        gui::status(n ? "White = stencil opening on a pad; green = pad with no opening; magenta = opening with no pad. Adjust the threshold until the pads are solid."
                      : "No paste layer found: right-click the stencil / paste layer > Screen Blend, and give it magenta.", n ? ToastKind::Info : ToastKind::Warning);
        dirty_ = true;
    }
    ImGui::TextDisabled("Register the image in Board Images (fiducials / pads) first.");
    if (button("Board Images...", Icon::Image)) { app::DocPtr d = doc; gui::defer([d]() { app::dlg_board_images(gui::main_hwnd(), *d); }); }
}

void GerberWorkspace::render_scene() {
    auto t0 = std::chrono::steady_clock::now();
    const int W = std::max(8, (int)size_.x), H = std::max(8, (int)size_.y);
    for (int i = 0; i < nlayers(); ++i) if (doc->prj.layers[i].visible || (compare_ && (i == cmp_a_ || i == cmp_b_)) || i == hl_layer_) ensure_cache(i);
    if (gpu_ok_) {
        ggpu::View v;
        v.w = W; v.h = H; v.scale = scale_; v.cx = cx_; v.cy = cy_; v.mirror = mirror_; v.mirror_y = mirror_y_; v.rot = vrot_;
        if (gpu_.begin(v, ui::pal().gerber_bg)) {
            if (compare_ && glayer(cmp_a_) && glayer(cmp_b_) && cache(cmp_a_).mesh && cache(cmp_b_).mesh) {
                gpu_.draw_compare(*cache(cmp_a_).mesh, lx(cmp_a_), *cache(cmp_b_).mesh, lx(cmp_b_), 0xFFFF4040, 0xFF30D0FF, 0xFF5A6068);
            } else {
                // Draw order = layer list (doc) order, identical blending for every layer: selecting or
                // activating a layer must never change how the stack renders (R10 §8; the active layer
                // used to be moved to the top and drawn opaque).
                // R11 P1-03: the row at the TOP of the list is drawn LAST (on top); dragging a row up
                // brings that layer forward. Same rule in render_d2d() and in picking.
                const int nvis = (int)std::count_if(doc->prj.layers.begin(), doc->prj.layers.end(), [](auto& l) { return l.visible; });
                if (img_show_) draw_board_image();
                for (int i = nlayers() - 1; i >= 0; --i) {
                    const GerberLayerRef& r = doc->prj.layers[i];
                    if (!r.visible || !glayer(i) || !cache(i).mesh) continue;
                    const bool scr = screened(i);
                    const float alpha = scr ? 1.f : nvis > 1 ? 0.78f : 1.f;
                    gpu_.draw_layer(*cache(i).mesh, lx(i), (scr ? screen_color(i) : r.color) | 0xFF000000u, alpha, fill_, negative(i) != glayer(i)->negative, scr);
                }
            }
            if (hl_layer_ >= 0 && hl_dcode_ >= 0 && glayer(hl_layer_) && cache(hl_layer_).mesh) {
                const double t = ImGui::GetTime();
                gpu_.draw_highlight(*cache(hl_layer_).mesh, lx(hl_layer_), hl_dcode_, ui::pal().selection, 0.7f + 0.25f * (float)std::sin(t * 6));
            }
            gpu_.end();
        } else {
            gpu_ok_ = false;
            gpu_err_ = "render target creation failed";
            cache_.clear();
        }
    }
    if (!gpu_ok_) render_d2d();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    render_ms_ = ms;
    render_ms_avg_ = render_ms_avg_ <= 0 ? ms : render_ms_avg_ * 0.9 + ms * 0.1;
    dirty_ = false;
}

void GerberWorkspace::prof_finish(double view_ms, double input_ms, double render_ms) {
    const double a = prof_n_ ? 0.15 : 1.0;   // light smoothing for the status text; the log has raw samples
    prof_input_ += (input_ms - prof_input_) * a;
    prof_render_ = render_ms > 0 ? prof_render_ + (render_ms - prof_render_) * a : prof_render_;
    prof_overlay_ += (std::max(0.0, view_ms - input_ms - render_ms) - prof_overlay_) * a;
    prof_view_ += (view_ms - prof_view_) * a;
    prof_view_max_ = std::max(prof_view_max_ * 0.98, view_ms);
    ++prof_n_;
    if (!prof_checked_) { prof_checked_ = true; if (const wchar_t* lp = _wgetenv(L"PCBP_VIEW_PROF")) prof_log_ = _wfopen(lp, L"a"); }
    if (prof_log_) { std::fprintf(prof_log_, "gerber view=%.3f input=%.3f render=%.3f overlay=%.3f\n", view_ms, input_ms, render_ms, std::max(0.0, view_ms - input_ms - render_ms)); std::fflush(prof_log_); }
}

void GerberWorkspace::render_d2d() {
    const int W = std::max(8, (int)size_.x), H = std::max(8, (int)size_.y);
    for (int i = 0; i < nlayers(); ++i) if (doc->prj.layers[i].visible) ensure_cache(i);
    d2d_tex_.ensure(W, H);
    ID2D1DeviceContext* rt = d2d_tex_.begin(1.0f);
    if (!rt) return;
    const uint32_t bg = ui::pal().gerber_bg;
    rt->Clear(ui::d2c(bg));
    const double sx = mirror_ ? -scale_ : scale_, sy = mirror_y_ ? scale_ : -scale_;
    const Xform Vx = Xform::translate(W * 0.5, H * 0.5) * Xform::scale(sx, sy) * Xform::rotation(vrot_) * Xform::translate(-cx_, -cy_);
    D2D1_MATRIX_3X2_F Wm = D2D1::Matrix3x2F((float)Vx.a, (float)Vx.b, (float)Vx.c, (float)Vx.d, (float)Vx.tx, (float)Vx.ty);
    Com<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(ui::d2c(0xFFFFFFFF), br.put());
    const double hair = 1.0 / std::max(scale_, 1e-6);
    for (int i = nlayers() - 1; i >= 0; --i) {   // list top drawn last (on top), as on the GPU path
        const GerberLayerRef& r = doc->prj.layers[i];
        if (!r.visible || !glayer(i)) continue;
        Xform x = lx(i);
        rt->SetTransform(D2D1::Matrix3x2F((float)x.a, (float)x.b, (float)x.c, (float)x.d, (float)x.tx, (float)x.ty) * Wm);
        for (const D2DRun& run : cache(i).d2d) {
            br->SetColor(ui::d2c(run.clear ? bg : (r.color | 0xFF000000u)));
            if (run.fill) {
                if (fill_ == ggpu::FillMode::Solid) rt->FillGeometry(run.fill.get(), br.get());
                else rt->DrawGeometry(run.fill.get(), br.get(), (float)hair);
            }
            for (const D2DStroke& s : run.strokes)
                rt->DrawGeometry(s.geo.get(), br.get(), (float)(fill_ == ggpu::FillMode::Sketch ? hair : std::max(s.w, hair)), stroke_style(s.square));
            if (run.holes) { br->SetColor(ui::d2c(bg)); rt->FillGeometry(run.holes.get(), br.get()); }
        }
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    d2d_tex_.end();
}

// ===========================================================================
// picking
// ===========================================================================
bool GerberWorkspace::hit_feature(Vec2 w, Hit& h, bool any_layer) const {
    const double tol = 3.0 / std::max(scale_, 1e-6);
    std::vector<int> order;
    if (act_ >= 0 && act_ < nlayers()) order.push_back(act_);
    for (int i = 0; i < nlayers(); ++i) if (i != act_) order.push_back(i);   // list top = drawn on top = picked first
    for (int li : order) {
        if (!any_layer && li != act_) continue;
        const gb::Layer* L = glayer(li);
        if (!L || !doc->prj.layers[li].visible) continue;
        Vec2 lp = lx(li).inverse().apply(w);
        const ObjIndex* ix = const_cast<GerberWorkspace*>(this)->index_of(li);
        int best = -1;
        ix->query(BBox(lp.x - tol, lp.y - tol, lp.x + tol, lp.y + tol), [&](int i) {
            if (i <= best || L->objs[i].clear || !obj_visible(li, i)) return;
            if (obj_hit(*L, L->objs[i], lp, tol)) best = i;
        });
        if (best >= 0) { h.layer = li; h.obj = best; return true; }
    }
    return false;
}

bool GerberWorkspace::snap_point(Vec2 w, double tol, Vec2& out, std::string* what) {
    double best = tol;
    bool found = false;
    for (int li = 0; li < nlayers(); ++li) {
        const gb::Layer* L = glayer(li);
        if (!L || !doc->prj.layers[li].visible) continue;
        Xform x = lx(li);
        Vec2 lp = x.inverse().apply(w);
        const ObjIndex* ix = index_of(li);
        ix->query(BBox(lp.x - tol, lp.y - tol, lp.x + tol, lp.y + tol), [&](int i) {
            if (!obj_visible(li, i)) return;
            const gb::Object& o = L->objs[i];
            auto cand = [&](Vec2 p, const char* k) {
                double d = dist(p, lp);
                if (d < best) { best = d; out = x.apply(p); found = true; if (what) *what = k; }
            };
            switch (o.kind) {
            case gb::Object::Flash: {
                cand(o.a, "pad centre");
                // pad edges: closest point on the flash outline (lower priority than the centre)
                if (const gb::Aperture* ap = gb::find_ap(*L, o.dcode)) {
                    auto ol = gb::flash_outline(*ap, o.a, 24);
                    for (size_t k = 0; k + 1 < ol.size() + 1 && ol.size() >= 2; ++k) {
                        Vec2 a = ol[k], b = ol[(k + 1) % ol.size()], cp;
                        const double d = seg_dist(lp, a, b, &cp) + 0.35 * tol;
                        if (d < best) { best = d; out = x.apply(cp); found = true; if (what) *what = "pad edge"; }
                    }
                }
                break;
            }
            case gb::Object::Draw: cand(o.a, "endpoint"); cand(o.b, "endpoint"); if (dist(o.a, o.b) > 1e-6) cand((o.a + o.b) * 0.5, "midpoint"); break;
            case gb::Object::Arc: cand(o.a, "endpoint"); cand(o.b, "endpoint"); cand(o.center, "arc centre"); break;
            case gb::Object::Region: {
                BBox b = gb::object_bbox(*L, o);
                if (b.w() < 10 && b.h() < 10) cand(b.center(), "region centre");
                for (auto& v : o.poly) cand(v, "vertex");
                break;
            }
            }
        });
    }
    // digitized component centroids
    for (const Component& c : doc->prj.comps) {
        double d = dist(c.pos, w);
        if (d < best) { best = d; out = c.pos; found = true; if (what) *what = "component " + c.ref; }
    }
    return found;
}

std::string GerberWorkspace::object_info(const Hit& h) const {
    const gb::Layer* L = glayer(h.layer);
    if (!L || h.obj < 0 || h.obj >= (int)L->objs.size()) return {};
    const gb::Object& o = L->objs[h.obj];
    const gb::Aperture* ap = o.kind == gb::Object::Region ? nullptr : gb::find_ap(*L, o.dcode);
    const Xform x = lx(h.layer);
    std::string t = "Layer\t" + doc->prj.layers[h.layer].name + "\n";
    if (o.kind == gb::Object::Region) t += "D-code\tregion (G36/G37)\n";
    else t += "D-code\tD" + std::to_string(o.dcode) + (ap && !ap->defined ? " (undefined)" : "") + "\n";
    if (ap) {
        t += "Shape\t" + shape_name(*ap) + "\n";
        t += "Size\t" + fmt_wh(ap->w, ap->h) + (ap->hole > 0 ? ", hole " + fmt_len_u8(ap->hole) : std::string()) + "\n";
    }
    BBox eb = gb::object_bbox(*L, o);
    switch (o.kind) {
    case gb::Object::Flash: t += "Type\tflash (D03)\n"; break;
    case gb::Object::Draw: t += "Type\tdraw (D01), length " + fmt_len_u8(dist(o.a, o.b)) + "\n"; break;
    case gb::Object::Arc: {
        char b[64]; std::snprintf(b, sizeof b, "%.1f", gb::arc_sweep_deg(o));
        t += std::string("Type\tarc ") + (o.cw ? "G02" : "G03") + ", r " + fmt_len_u8(dist(o.a, o.center)) + ", sweep " + b + "\xC2\xB0\n";
        break;
    }
    case gb::Object::Region: t += "Type\tregion, " + std::to_string(o.poly.size()) + " vertices\n"; break;
    }
    t += "Extent\t" + fmt_wh(eb.w(), eb.h()) + "\n";
    if (o.kind == gb::Object::Draw || o.kind == gb::Object::Arc) {
        t += "From\t" + fmt_pos_u8(*doc, x.apply(o.a)) + "\n";
        t += "To\t" + fmt_pos_u8(*doc, x.apply(o.b)) + "\n";
        if (o.kind == gb::Object::Arc) t += "Centre\t" + fmt_pos_u8(*doc, x.apply(o.center)) + "\n";
    } else {
        t += "Position\t" + fmt_pos_u8(*doc, x.apply(object_center(*L, o))) + "\n";
    }
    t += std::string("Polarity\t") + (o.clear ? "clear (LPC)" : "dark (LPD)") + "\n";
    if (o.kind != gb::Object::Region) {
        auto it = const_cast<GerberWorkspace*>(this)->cache(h.layer).usage.find(o.dcode);
        if (it != cache_[h.layer].usage.end()) t += "Uses\t" + std::to_string(it->second) + " on this layer\n";
    }
    t += "Object\t#" + std::to_string(h.obj);
    return t;
}

// ===========================================================================
// input
// ===========================================================================
void GerberWorkspace::handle_input(bool hovered, bool active) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mp = io.MousePos;
    const int mods = (io.KeyShift ? 1 : 0) | (io.KeyCtrl ? 2 : 0) | (io.KeyAlt ? 4 : 0);
    if (hovered || active) {
        Vec2 w = s2w(mp);
        if (w.x != mouse_w_.x || w.y != mouse_w_.y) {
            mouse_w_ = w;
            const double tol = 12.0 / scale_;
            snap_valid_ = false;
            if ((tool_ == Tool::Measure || tool_ == Tool::RelOrigin || (tool_ == Tool::FrameSel && fsel_.mode == 0)) && !io.KeyAlt) snap_valid_ = snap_point(w, tol, snap_pt_, &meas_snap_);
            if (tool_ == Tool::Query || tool_ == Tool::Clearance || tool_ == Tool::AlignSrc || tool_ == Tool::AlignDst ||
                tool_ == Tool::PitchPick || (tool_ == Tool::Detect && dt_.stage == 0)) {
                Hit h;
                hover_hit_ = hit_feature(w, h) ? h : Hit{};
            }
            if (tool_ == Tool::FrameSel && fsel_.mode == 1) { Hit h; hover_hit_ = hit_feature(w, h, true) && outline_elem(glayer(h.layer)->objs[h.obj]) ? h : Hit{}; }   // R17b
        }
    }
    // wheel: x1.39 per notch about the cursor (ViewMate); Shift = horizontal pan
    if (hovered && io.MouseWheel != 0) {
        if (io.KeyShift) pan_px(io.MouseWheel * size_.x * 0.1, 0);
        else zoom_at(std::pow(1.39, io.MouseWheel), mp);
    }
    if (hovered && io.MouseWheelH != 0) pan_px(-io.MouseWheelH * size_.x * 0.1, 0);
    // middle: click = recentre, drag = pan (real time)
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) { drag_ = Drag::MidPending; down_s_ = mp; }
    if (drag_ == Drag::MidPending && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 3)) drag_ = Drag::Pan;
    const bool space = hovered && !io.WantTextInput && !io.KeyCtrl && ImGui::IsKeyDown(ImGuiKey_Space);   // Ctrl+Space = maximize
    // Crosshair (ViewMate): docked to the cursor by default. Space tap = undock / dock;
    // Shift+Space = set the REL anchor at the crosshair; Space held + drag = pan.
    if (space && !space_down_) { space_down_ = true; space_panned_ = false; }
    if (space_down_ && !ImGui::IsKeyDown(ImGuiKey_Space)) {
        space_down_ = false;
        if (!space_panned_ && hovered) {
            if (io.KeyShift) {
                anchor_ = crosshair(); anchor_on_ = true; coord_mode_ = 1;
                gui::status("Anchor set at the crosshair: REL shows dX dY distance angle from it.");
            } else if (ch_docked_) {
                ch_w_ = mouse_w_; ch_docked_ = false;
                gui::status("Crosshair undocked (arrows nudge it, Shift = fine). Space docks it to the cursor again.");
            } else { ch_docked_ = true; gui::status("Crosshair docked to the cursor."); }
        }
    }
    if (drag_ == Drag::Pan || (active && space && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        ImVec2 d = io.MouseDelta;
        if (d.x != 0 || d.y != 0) { pan_px(d.x, d.y); if (space) space_panned_ = true; }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) {
        if (drag_ == Drag::MidPending) { Vec2 w = s2w(mp); cx_ = w.x; cy_ = w.y; dirty_ = true; }
        if (drag_ == Drag::MidPending || drag_ == Drag::Pan) drag_ = Drag::None;
    }
    // Align View to Line: draw / edit the reference line (handles), snapped to pad centres / edges
    if (tool_ == Tool::AlignView && !space) {
        const bool free = io.KeyAlt;
        auto quant = [&](Vec2 fixed, Vec2 m) {
            if (av_step_ <= 0 || free) return m;
            Vec2 d = m - fixed;
            const double len = d.len();
            if (len < 1e-9) return m;
            const double q = deg2rad(std::round(rad2deg(std::atan2(d.y, d.x)) / av_step_) * av_step_);
            return fixed + Vec2{std::cos(q), std::sin(q)} * len;
        };
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            av_drag_ = 2;
            if (av_has_) {
                const ImVec2 sa = w2s(av_a_), sb = w2s(av_b_);
                auto d2 = [&](ImVec2 q) { return (q.x - mp.x) * (q.x - mp.x) + (q.y - mp.y) * (q.y - mp.y); };
                const float hr = S(10) * S(10);
                if (d2(sa) < hr) av_drag_ = 0;
                else if (d2(sb) < hr) av_drag_ = 1;
            }
            if (av_drag_ == 2) { av_a_ = av_snap(s2w(mp), free); av_b_ = av_a_; av_has_ = true; }
        }
        if (av_drag_ >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const Vec2 w = av_snap(s2w(mp), free);
            if (av_drag_ == 0) av_a_ = quant(av_b_, w);
            else av_b_ = quant(av_a_, w);
        }
        if (av_drag_ >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            av_drag_ = -1;
            if (dist(av_a_, av_b_) * scale_ < 3) av_has_ = false;
        }
        if (hovered && !io.WantTextInput && av_has_ && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) av_commit();
    }
    // R22b: Draw Outline corner handles (consume the press when a handle is grabbed)
    if (!space) outline_draw_handles_input(hovered);
    // left
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !space && tool_ != Tool::AlignView && fsel_.drag_corner < 0) {
        down_s_ = mp; down_w_ = s2w(mp); down_mods_ = mods; drag_ = Drag::Pending;
    }
    const bool band_tool = tool_ != Tool::Measure && tool_ != Tool::Clearance && tool_ != Tool::RelOrigin && tool_ != Tool::Query &&
                           tool_ != Tool::AlignSrc && tool_ != Tool::AlignDst && tool_ != Tool::DeleteComp &&
                           !(tool_ == Tool::Detect && dt_.stage == 1);
    if (drag_ == Drag::Pending && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4)) drag_ = band_tool ? Drag::Band : Drag::Pending;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && (drag_ == Drag::Pending || drag_ == Drag::Band)) {
        Vec2 up = s2w(mp);
        BBox b; b.add(down_w_); b.add(up);
        band_quad_.clear();
        if (drag_ == Drag::Band && vrot_ != 0) {   // rotated view: the band is a rotated rectangle in world space
            band_quad_ = screen_quad(down_s_, mp);
            b = BBox{};
            for (auto& q : band_quad_) b.add(q);
        }
        if (drag_ == Drag::Band) rect(b, down_mods_);
        else click(up, down_mods_);
        drag_ = Drag::None;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && (tool_ == Tool::Select || tool_ == Tool::Digitize)) {
        int ci = app::hit_component(*doc, s2w(mp), 4.0 / scale_, doc->show_top, doc->show_bottom);
        if (ci >= 0) {
            app::DocPtr d = doc;
            gui::defer([d, ci]() { if (ci < (int)d->prj.comps.size()) { app::dlg_location(nullptr, *d, d->prj.comps[ci]); } });
        }
    }
    // right click: context menu
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
        if (dd.x * dd.x + dd.y * dd.y < 16 && !io.KeyShift && !io.KeyCtrl && can_confirm()) {
            // R22b §6.2: right-click = Confirm (Add Component card opens the same frame: no input is lost)
            if (tool_ == Tool::Digitize && have_res_) accept();
            else gui::defer([this]() { confirm_action(); });
        } else if (dd.x * dd.x + dd.y * dd.y < 16) {
            ctx_w_ = s2w(mp);
            ctx_hit_ = {};
            hit_feature(ctx_w_, ctx_hit_);
            ImGui::OpenPopup("##gctx");
        }
    }
    // keys (suspended while the Add Component card is open: typed characters belong to it)
    if (hovered && !io.WantTextInput && !ac_.open) {
        const double step = size_.x * 0.15 / scale_;
        auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k); };
        if (pressed(ImGuiKey_Home)) zoom_all();
        // layer stepping: PgUp/PgDn or [ ] = show previous / next layer only (and make it active);
        // Ctrl+[ ] = move the selected layer up / down in draw order
        if (pressed(ImGuiKey_PageUp)) step_layer(-1);
        if (pressed(ImGuiKey_PageDown)) step_layer(+1);
        if (pressed(ImGuiKey_LeftBracket)) { if (io.KeyCtrl) move_selected(-1); else step_layer(-1); }
        if (pressed(ImGuiKey_RightBracket)) { if (io.KeyCtrl) move_selected(+1); else step_layer(+1); }
        if (pressed(ImGuiKey_KeypadAdd) || pressed(ImGuiKey_Equal)) zoom_at(1.39, mp);
        if (pressed(ImGuiKey_KeypadSubtract) || pressed(ImGuiKey_Minus)) zoom_at(1 / 1.39, mp);
        if (pressed(ImGuiKey_End)) { Vec2 w = s2w(mp); cx_ = w.x; cy_ = w.y; dirty_ = true; }
        if (pressed(ImGuiKey_C) && !io.KeyCtrl && !io.KeyAlt) centre_crosshair_on_selection();
        if (!ch_docked_ && !io.KeyCtrl) {
            // undocked crosshair: arrows nudge it (10 px, Shift = 1 px)
            const double nd = (io.KeyShift ? 1.0 : 10.0) / scale_;
            const float npx = (float)(nd * scale_);
            auto nudge = [&](float dx, float dy) { ImVec2 s = w2s(ch_w_); ch_w_ = s2w(ImVec2(s.x + dx, s.y + dy)); };
            if (pressed(ImGuiKey_LeftArrow)) nudge(-npx, 0);
            if (pressed(ImGuiKey_RightArrow)) nudge(npx, 0);
            if (pressed(ImGuiKey_UpArrow)) nudge(0, -npx);
            if (pressed(ImGuiKey_DownArrow)) nudge(0, npx);
        } else {
            const double ps = io.KeyCtrl ? size_.x * 0.9 / scale_ : step;
            const double psy = io.KeyCtrl ? size_.y * 0.9 / scale_ : step;
            if (pressed(ImGuiKey_LeftArrow)) pan_px(ps * scale_, 0);
            if (pressed(ImGuiKey_RightArrow)) pan_px(-ps * scale_, 0);
            if (pressed(ImGuiKey_UpArrow)) pan_px(0, psy * scale_);
            if (pressed(ImGuiKey_DownArrow)) pan_px(0, -psy * scale_);
        }
        if (pressed(ImGuiKey_F) && !io.KeyCtrl) { if (io.KeyShift) mirror_y_ = !mirror_y_; else mirror_ = !mirror_; dirty_ = true; }
        for (int k = 0; k < 9; ++k) {
            if (!pressed((ImGuiKey)(ImGuiKey_1 + k)) || k >= nlayers()) continue;
            if (io.KeyCtrl) { for (int i = 0; i < nlayers(); ++i) doc->prj.layers[i].visible = i == k; }
            else if (io.KeyAlt) set_active(k);
            else doc->prj.layers[k].visible = !doc->prj.layers[k].visible;
            doc->changed(ui::CH_LAYERS);
        }
        if (pressed(ImGuiKey_0)) { for (auto& l : doc->prj.layers) l.visible = true; doc->changed(ui::CH_LAYERS); }
        if (pressed(ImGuiKey_Escape)) {
            // first press clears a pad selection in the pad tools, the next one leaves the tool
            if ((have_res_ || !matches_.empty()) && tool_ == Tool::Digitize) { clear_buffer(); gui::status("Pad selection cleared."); }
            else if (tool_ == Tool::FrameSel && (!fsel_.picks.empty() || fsel_.corners)) { outline_reset_keep(); gui::status("Outline selection cleared (Esc again leaves the tool)."); }
            else if (tool_ == Tool::FrameSel) outline_cancel();   // R17b: Esc = cancel the outline and leave the tool
            else if (tool_ == Tool::PitchPick && !pt_.sel.empty()) { pt_.sel.clear(); gui::status("Selection cleared (Esc again cancels the pitch tool)."); }
            else if (tool_ == Tool::Detect && dt_.stage == 1) { dt_.stage = 0; dt_.cands.clear(); dt_.on.clear(); gui::status("Candidates discarded; the reference selection is kept."); }
            else if (tool_ == Tool::Detect && !dt_.sel.empty()) { dt_.sel.clear(); gui::status("Selection cleared (Esc again cancels detection)."); }
            else if (tool_ == Tool::PitchPick || tool_ == Tool::Detect) tool_exit(tool_ == Tool::PitchPick ? "Panel pitch cancelled (nothing changed)." : "Detect Repeated Circuits cancelled (nothing changed).");
            else if (tool_ == Tool::Measure && (meas_has_a_ || meas_done_)) { meas_has_a_ = meas_done_ = false; }
            else if (tool_ == Tool::AlignView && av_has_) { av_has_ = false; av_drag_ = -1; }
            else if (tool_ != Tool::Digitize) { align_stage_ = 0; set_tool(Tool::Digitize); }
            else if (have_res_ || !matches_.empty()) { clear_buffer(); gui::status("Buffer cleared."); }
            have_pin_ = false;
            refresh();
        }
        if ((pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) && tool_ != Tool::AlignView) {   // R22b: Enter = Confirm
            if (tool_ == Tool::Digitize && have_res_) accept();
            else if (can_confirm()) gui::defer([this]() { confirm_action(); });
            else if (tool_ == Tool::Digitize) gui::defer([this]() { accept(); });
        }
        if (!io.KeyCtrl && !io.KeyAlt) {
            if (pressed(ImGuiKey_Q)) set_tool(Tool::Query);
            if (pressed(ImGuiKey_D)) set_tool(Tool::Measure);
            if (pressed(ImGuiKey_E)) set_tool(Tool::Clearance);
            if (pressed(ImGuiKey_Z)) set_tool(Tool::ZoomRect);
            if (pressed(ImGuiKey_O)) { fill_ = fill_ == ggpu::FillMode::Solid ? ggpu::FillMode::Outline : fill_ == ggpu::FillMode::Outline ? ggpu::FillMode::Sketch : ggpu::FillMode::Solid; dirty_ = true; }
            if (pressed(ImGuiKey_R) && io.KeyShift) set_tool(Tool::RelOrigin);
            if (pressed(ImGuiKey_L)) set_tool(Tool::AlignView);
        }
    }
    if (hovered) {
        ImGuiMouseCursor cur = ImGuiMouseCursor_Arrow;
        if (space || drag_ == Drag::Pan) cur = ImGuiMouseCursor_ResizeAll;
        else if (tool_ == Tool::AlignSrc || tool_ == Tool::AlignDst || tool_ == Tool::DeleteComp) cur = ImGuiMouseCursor_Hand;
        else if (crosshair_ && ch_docked_) cur = ImGuiMouseCursor_None;
        ImGui::SetMouseCursor(cur);
    }
}

void GerberWorkspace::click(Vec2 w, int mods) {
    const double tol = 4.0 / std::max(scale_, 1e-6);
    switch (tool_) {
    case Tool::Query: {
        Hit h;
        if (hit_feature(w, h)) { pinned_hit_ = h; have_pin_ = true; }
        else { have_pin_ = false; gui::status("No Gerber feature under the cursor."); }
        return;
    }
    case Tool::Measure: {
        Vec2 p = snap_valid_ ? snap_pt_ : w;
        if (!meas_has_a_ || meas_done_) { meas_a_ = p; meas_has_a_ = true; meas_done_ = false; }
        else { meas_b_ = p; meas_done_ = true; }
        return;
    }
    case Tool::Clearance: clearance_click(w); return;
    case Tool::RelOrigin: rel_ = snap_valid_ ? snap_pt_ : w; have_rel_ = true; gui::status("Relative origin set at " + fmt_pos_u8(*doc, rel_)); return;
    case Tool::DeleteComp: delete_comp(w); return;
    case Tool::AlignSrc: case Tool::AlignDst: align_click(w); return;
    case Tool::PitchPick: case Tool::Detect: {   // R22b: element click, additive (Ctrl removes)
        if (tool_ == Tool::Detect && dt_.stage == 1) {   // review: click a candidate to toggle it
            for (int i = 0; i < (int)dt_.cands.size(); ++i) {
                BBox b = dt_.cands[i].box; b.inflate(tol);
                if (b.contains(w)) { dt_.on[i] = !dt_.on[i]; refresh(); return; }
            }
            return;
        }
        if (tool_ == Tool::PitchPick && pt_.stage >= 2) return;
        Hit h;
        if (!hit_feature(w, h, true)) { gui::status("No Gerber element under the cursor (visible layers)."); return; }
        gmatch::ElemSelection& S = tool_ == Tool::PitchPick ? pt_.sel : dt_.sel;
        const int n = S.apply({{h.layer, h.obj}}, (mods & 2) != 0);
        gui::status(std::string(n ? ((mods & 2) ? "Removed 1 element" : "Added 1 element") : "No change") + "; " + std::to_string(S.size()) + " selected  \xC2\xB7  Enter / right-click confirms");
        refresh();
        return;
    }
    case Tool::FrameSel: {
        if (fsel_.mode == 1) { outline_pick_click(w, (mods & 2) != 0); return; }   // R17b: pick elements
        const Vec2 q = snap_valid_ ? snap_pt_ : w;   // two snapped corner clicks = a rectangle
        if (fsel_.corners != 1) { outline_reset_keep(); fsel_.c0 = q; fsel_.corners = 1; gui::status("First corner " + fmt_pos_u8(*doc, q) + ": click the opposite corner (or drag a rectangle)."); }
        else { fsel_.box = BBox{}; fsel_.box.add(fsel_.c0); fsel_.box.add(q); fsel_.corners = 2; fsel_.n = 0; fsel_.outline.clear();
               if (outline_ready()) gui::status("Rectangle " + fmt_wh(fsel_.box.w(), fsel_.box.h()) + ": drag a corner to adjust; right-click / Enter / Confirm saves it, Esc cancels.");
               else fsel_.corners = 0; }
        refresh();
        return;
    }
    case Tool::PlotAlign: {   // R17a: any panel instance; the pads boxed next belong to it
        int img = 0;
        const int ci = pa_hit(w, tol, img);
        if (ci >= 0) {
            pa_.ref = ci; pa_.image = img;
            const bool panel = doc->prj.instance_count(doc->prj.comps[ci]) > 1;
            gui::status("Reference " + doc->prj.comps[ci].ref + (panel ? " (circuit " + std::to_string(img + 1) + ")" : std::string()) +
                        ": now drag a box around its pads on the Gerber.");
            refresh();
        } else gui::status("No placement under the cursor. Click a placement (yellow), then box its pads.");
        return;
    }
    case Tool::ZoomRect: zoom_at((mods & 1) ? 1 / 2.0 : 2.0, w2s(w)); return;
    case Tool::Digitize: {   // R22b §6.1: click adds the pad under the cursor, Ctrl+click removes it
        const bool remove = (mods & 2) != 0;
        if (remove && !have_res_) { gui::status("Nothing selected (Ctrl+click removes pads from the selection)."); return; }
        if (!have_res_) {
            const double r = 2.0 / std::max(scale_, 1e-6);
            const int li = choose_pad_layer({{w.x - r, w.y - r}, {w.x + r, w.y - r}, {w.x + r, w.y + r}, {w.x - r, w.y + r}});
            if (li >= 0 && li != act_) set_active(li);
        }
        if (!ensure_ps()) { gui::status("No Gerber layer to read pads from: select a copper or paste layer in the Layers list."); return; }
        modify_buffer(BBox{}, true, lx(act_).inverse().apply(w), !remove);
        return;
    }
    case Tool::Select: {
        int ci = app::hit_component(*doc, w, tol, doc->show_top, doc->show_bottom);
        if (!(mods & 3)) doc->select_none();
        if (ci >= 0) {
            Component& c = doc->prj.comps[ci];
            c.selected = !(mods & 2) || !c.selected;
            gui::status("Component " + (c.ref.empty() ? std::string("(no ref)") : c.ref) + "  " + c.package + "  at " + fmt_pos_u8(*doc, c.pos));
        }
        doc->changed(ui::CH_SELECTION);
        return;
    }
    default: return;
    }
}

void GerberWorkspace::rect(const BBox& w, int mods) {
    switch (tool_) {
    // Add from Pads: drag adds to the selection, Ctrl+drag subtracts (R10 §7); creation is explicit
    case Tool::Digitize: digitize(w, (mods & 2) ? 2 : have_res_ ? 1 : 0); break;
    case Tool::Select: {
        if (!(mods & 3)) doc->select_none();
        int n = 0;
        for (auto& c : doc->prj.comps) if (plot_visible(c) && (band_quad_.empty() ? w.contains(c.pos) : point_in_poly(c.pos, band_quad_))) { c.selected = true; ++n; }
        doc->changed(ui::CH_SELECTION);
        gui::status("Selected " + std::to_string(n) + " component(s).");
        break;
    }
    case Tool::PitchPick: case Tool::Detect: {   // R22b: box adds every element entirely inside, Ctrl+box removes
        if (tool_ == Tool::PitchPick && pt_.stage >= 2) break;
        gmatch::ElemSelection& S = tool_ == Tool::PitchPick ? pt_.sel : dt_.sel;
        const int n = S.apply(elems_in_box(w, false), (mods & 2) != 0);
        gui::status(std::to_string(n) + ((mods & 2) ? " element(s) removed; " : " element(s) added; ") + std::to_string(S.size()) + " selected  \xC2\xB7  Enter / right-click confirms");
        refresh();
        break;
    }
    case Tool::CropIn: crop(w, true); break;
    case Tool::CropOut: crop(w, false); break;
    case Tool::Frame: { BBox b = w; gui::defer([this, b]() { set_frame(b); }); break; }   // legacy (no toolbar entry since R17b)
    case Tool::PlotAlign: pa_rect(w); break;
    case Tool::PkgWizard: wz_rect(w, mods); break;
    case Tool::FrameSel: if (fsel_.mode == 1) outline_pick_box(w, (mods & 2) != 0); else outline_draw_rect(w); break;
    default: zoom_to(w, 0.02); break;
    }
}

void GerberWorkspace::clearance_click(Vec2 w) {
    Hit h;
    if (!hit_feature(w, h)) { gui::status("No Gerber feature under the cursor."); return; }
    if (clr_a_.layer < 0 || clr_done_) { clr_a_ = h; clr_done_ = false; gui::status("First feature picked; click the second feature."); return; }
    std::vector<Fat> fa, fb;
    std::vector<std::vector<Vec2>> aa, ab;
    object_fat(*glayer(clr_a_.layer), glayer(clr_a_.layer)->objs[clr_a_.obj], fa, aa);
    object_fat(*glayer(h.layer), glayer(h.layer)->objs[h.obj], fb, ab);
    const Xform xa = lx(clr_a_.layer), xb = lx(h.layer);
    for (auto& f : fa) { f.a = xa.apply(f.a); f.b = xa.apply(f.b); }
    for (auto& f : fb) { f.a = xb.apply(f.a); f.b = xb.apply(f.b); }
    double best = 1e300;
    Vec2 bp, bq;
    for (auto& a : fa)
        for (auto& b : fb) {
            Vec2 c1, c2;
            double d = segseg(a.a, a.b, b.a, b.b, c1, c2);
            double g = d - a.r - b.r;
            if (g < best) {
                best = g;
                Vec2 dir = d > 1e-12 ? (c2 - c1) * (1.0 / d) : Vec2{0, 0};
                bp = c1 + dir * a.r;
                bq = c2 - dir * b.r;
            }
        }
    clr_d_ = std::max(0.0, best);
    clr_p_ = bp; clr_q_ = bq;
    clr_done_ = true;
    gui::status("Clearance " + fmt_len_u8(clr_d_) + (best <= 0 ? " (features touch / overlap)" : ""));
    clr_a_ = {};
}

// ===========================================================================
// digitizing (ported from the earlier legacy view)
// ===========================================================================
dz::DigitizeOptions GerberWorkspace::dopt() const {
    dz::DigitizeOptions o;
    if (act_ >= 0 && act_ < nlayers()) o.hidden_dcodes = doc->prj.layers[act_].hidden_dcodes;
    for (int i = 0; i < nlayers(); ++i)
        if (i != act_ && doc->prj.layers[i].role == LayerRole::Drill && glayer(i)) { o.drill = glayer(i); break; }
    return o;
}

bool GerberWorkspace::ensure_ps() {
    if (act_ < 0 || act_ >= nlayers() || !doc->glayers[act_]) return false;
    const auto& L = doc->glayers[act_];
    const auto& hid = doc->prj.layers[act_].hidden_dcodes;
    const std::vector<char>* cr = crop_of(act_);
    unsigned cg = cr ? crop_gen_ : 0;
    dz::DigitizeOptions o = dopt();
    if (ps_layer_ == L && ps_index_ == act_ && ps_hidden_ == hid && ps_crop_ == cg && ps_drill_ == o.drill) return true;
    if (cr) {
        gb::Layer f;
        f.aps = L->aps;
        f.fmt = L->fmt;
        for (size_t i = 0; i < L->objs.size(); ++i) if (i >= cr->size() || !(*cr)[i]) f.objs.push_back(L->objs[i]);
        f.recompute_extent();
        ps_ = dz::extract_pads(f, o);
    } else {
        ps_ = dz::extract_pads(*L, o);
    }
    ps_layer_ = L; ps_index_ = act_; ps_hidden_ = hid; ps_crop_ = cg; ps_drill_ = o.drill;
    clear_buffer();
    return true;
}

std::vector<int> GerberWorkspace::used_pads() {
    std::vector<int> out;
    if (!ensure_ps()) return out;
    Xform ti = lx(act_).inverse();
    for (const Component& c : doc->prj.comps) {
        const Package* pk = doc->prj.package_of(c);
        if (!pk) continue;
        for (int i = 0; i < (int)pk->pads.size(); ++i) {
            Vec2 l = ti.apply(app::pad_world(*doc, c, i));
            const double tol = std::max(0.1, 0.3 * std::min(pk->pads[i].w, pk->pads[i].h));
            for (int j : ps_.query(BBox(l.x - tol, l.y - tol, l.x + tol, l.y + tol)))
                if (dist(ps_.pads[j].c, l) <= tol) out.push_back(j);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<Pad> GerberWorkspace::world_pads(const std::vector<Pad>& local, int layer) const {
    const GerberLayerRef& r = doc->prj.layers[layer];
    Xform t = lx(layer);
    std::vector<Pad> out = local;
    for (auto& p : out) { p.pos = lin(t, p.pos); p.rot = norm_deg((r.mirror ? -p.rot : p.rot) + r.rot); }
    return out;
}

BBox GerberWorkspace::pads_world_box(const std::vector<int>& idx) const {
    BBox b;
    if (act_ < 0) return b;
    Xform t = lx(act_);
    for (int i : idx) if (i >= 0 && i < (int)ps_.pads.size()) b.add(xform_box(t, ps_.pads[i].box));
    return b;
}

// Pad source = the layer selected in the layer list (act_). If the box holds no pads there but
// pads on other visible layers, the top-most visible copper / paste layer with pads is used.
int GerberWorkspace::choose_pad_layer(const std::vector<Vec2>& quad) {
    auto count_in = [&](int li, const dz::PadSet& ps) {
        const Xform ti = lx(li).inverse();
        std::vector<Vec2> lq;
        for (auto& q : quad) lq.push_back(ti.apply(q));
        BBox lb;
        for (auto& q : lq) lb.add(q);
        int n = 0;
        for (int j : ps.query(lb)) if (point_in_poly(ps.pads[j].c, lq)) ++n;
        return n;
    };
    if (act_ >= 0 && ensure_ps() && count_in(act_, ps_) > 0) return act_;
    // top-most first: the active layer, then list order (list top is drawn on top)
    for (int i = 0; i < nlayers(); ++i) {
        if (i == act_ || !glayer(i)) continue;
        const GerberLayerRef& r = doc->prj.layers[i];
        if (!r.visible || (r.role != LayerRole::Copper && r.role != LayerRole::Paste)) continue;
        if (layer_guess(r).side == 2) continue;   // inner layers: no component pads
        dz::DigitizeOptions o = dopt();
        o.hidden_dcodes = r.hidden_dcodes;
        dz::PadSet ps = dz::extract_pads(*glayer(i), o);
        if (count_in(i, ps) > 0) return i;
    }
    return act_;
}

void GerberWorkspace::digitize(const BBox& w, int mode) {
    const bool add = mode != 0;
    if (mode == 2 && !have_res_) { gui::status("Nothing selected to subtract from (Ctrl+drag removes pads from the selection)."); return; }
    std::vector<Vec2> quad = band_quad_;
    if (quad.empty()) quad = {{w.x0, w.y0}, {w.x1, w.y0}, {w.x1, w.y1}, {w.x0, w.y1}};
    if (!add || !have_res_) {
        const int li = choose_pad_layer(quad);
        if (li >= 0 && li != act_) {
            set_active(li);
            sel_layer_ = li;
            lsel_.assign(nlayers(), 0);
            lsel_[li] = 1;
            gui::status("No pads on the selected layer inside the box: using " + doc->prj.layers[li].name);
        }
    }
    if (!ensure_ps()) { gui::status("No Gerber layer to read pads from: select a copper or paste layer in the Layers list."); return; }
    const Xform ti = lx(act_).inverse();
    std::vector<Vec2> lq;
    for (auto& q : quad) lq.push_back(ti.apply(q));
    BBox lr;
    for (auto& q : lq) lr.add(q);
    // pads whose (oriented) box lies fully inside the (possibly rotated) selection
    std::vector<int> idx;
    BBox qb = lr;
    qb.inflate(8);
    for (int j : ps_.query(qb)) {
        const dz::PadFeature& f = ps_.pads[j];
        std::vector<Vec2> corners;
        if (f.oriented) {
            for (Vec2 c : {Vec2{-f.ow / 2, -f.oh / 2}, Vec2{f.ow / 2, -f.oh / 2}, Vec2{f.ow / 2, f.oh / 2}, Vec2{-f.ow / 2, f.oh / 2}}) corners.push_back(f.c + rotate(c, f.oang));
        } else {
            corners = {{f.box.x0, f.box.y0}, {f.box.x1, f.box.y0}, {f.box.x1, f.box.y1}, {f.box.x0, f.box.y1}};
        }
        bool in = true;
        for (auto& c : corners) in = in && point_in_poly(c, lq);
        if (in) idx.push_back(j);
    }
    if (add && have_res_) {
        std::vector<int> cur = res_.pad_indices;
        const size_t before = cur.size();
        if (mode == 2) cur.erase(std::remove_if(cur.begin(), cur.end(), [&](int k) { return std::find(idx.begin(), idx.end(), k) != idx.end(); }), cur.end());
        else cur.insert(cur.end(), idx.begin(), idx.end());
        matches_.clear(); cur_match_ = -1;
        if (rebuild_from(cur)) {
            const long d = (long)res_.pad_indices.size() - (long)before;
            gui::status(std::string(mode == 2 ? "Removed " : "Added ") + std::to_string(std::labs(d)) + " pad(s); selection: " + std::to_string(res_.pad_indices.size()) +
                        " pad(s) " + res_.name + "  \xE2\x80\x94 drag adds, Ctrl+drag removes, Enter / right-click creates, Esc clears");
        } else gui::status("Selection empty.");
        return;
    }
    if (idx.empty()) { gui::status("No pads found in the box (pads partly outside the box are ignored)."); return; }
    matches_.clear();
    cur_match_ = -1;
    review_.clear();
    if (!rebuild_from(idx, true)) { gui::status("No pads found in the box"); return; }
    char rb[48] = "";
    if (res_.rot != 0) std::snprintf(rb, sizeof rb, "  rotated %.1f\xC2\xB0", res_.rot);
    gui::status("Selected " + std::to_string(res_.pad_indices.size()) + " pad(s): " + res_.name + rb + "  \xE2\x80\x94 right-click or Enter to add the component");
}

// Rotation of the buffer: detected from the pad geometry; with a rotated view the view angle
// (in layer coordinates) is the default. Rotated groups are rebuilt in canonical orientation.
void GerberWorkspace::orient_result() {
    if (!have_res_ || act_ < 0) return;
    rot_guess_ = dz::estimate_rotation(ps_, res_.pad_indices);
    double ang = rot_guess_.angle;
    if (vrot_ != 0) {
        const GerberLayerRef& r = doc->prj.layers[act_];
        double la = r.mirror ? r.rot - view_angle() : view_angle() - r.rot;
        la = std::remainder(la, 90.0);
        if (la <= -45 + 1e-9) la += 90;
        ang = std::round(la * 10) / 10;
        rot_guess_.why = "view aligned at " + dz::fmt_mm(view_angle(), 1) + "\xC2\xB0; detected " + dz::fmt_mm(rot_guess_.angle, 1) + "\xC2\xB0 (" + rot_guess_.why + ")";
    }
    bool misaligned = false;
    for (int i : res_.pad_indices) {
        const auto& f = ps_.pads[i];
        if (f.oriented && std::fabs(std::remainder(f.oang, 90.0)) > 0.5) misaligned = true;
    }
    if (std::fabs(ang) < 0.05 && !misaligned) return;
    dz::DigitizeOptions o = dopt();
    o.drop_vias = false;
    dz::DigitizeResult r;
    if (dz::digitize_rotated(ps_, res_.pad_indices, ang, o, r)) res_ = std::move(r);
}

bool GerberWorkspace::rebuild_from(std::vector<int> idx, bool fresh) {
    std::sort(idx.begin(), idx.end());
    idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
    if (idx.empty()) { have_res_ = false; res_ = {}; return false; }
    dz::PadSet sub;
    sub.cell = 1e-4;
    BBox all;
    for (int i : idx) { sub.pads.push_back(ps_.pads[i]); all.add(ps_.pads[i].box); }
    all.inflate(0.01);
    dz::DigitizeOptions o = dopt();
    if (!fresh) { o.hidden_dcodes.clear(); o.drop_vias = false; }
    dz::DigitizeResult r;
    if (!dz::digitize_rect(sub, all, o, r)) { have_res_ = false; res_ = {}; return false; }
    for (int& k : r.pad_indices) k = idx[k];
    for (int& k : r.dropped) k = idx[k];
    res_ = std::move(r);
    have_res_ = true;
    orient_result();
    dz::canonicalize_orientation(res_);   // two-pad parts: horizontal package, angle from the pads
    return true;
}

void GerberWorkspace::modify_buffer(const BBox& lr, bool point, Vec2 lp, bool add) {
    std::vector<int> hit;
    if (point) {
        int best = -1;
        double area = 1e30;
        for (int j : ps_.query(BBox(lp.x - 8, lp.y - 8, lp.x + 8, lp.y + 8))) {
            const auto& f = ps_.pads[j];
            if (!f.box.contains(lp)) continue;
            if (f.oriented) {
                const Vec2 l = rotate(lp - f.c, -f.oang);
                if (std::fabs(l.x) > f.ow / 2 || std::fabs(l.y) > f.oh / 2) continue;
            }
            double a = f.oriented ? f.ow * f.oh : f.w * f.h;
            if (a < area) { area = a; best = j; }
        }
        if (best >= 0) hit.push_back(best);
    } else {
        const Xform ti = lx(act_).inverse();
        std::vector<Vec2> lq;
        for (auto& q : band_quad_) lq.push_back(ti.apply(q));
        BBox q = lr;
        q.inflate(8);
        for (int j : ps_.query(q)) {
            if (lq.empty() ? box_inside(ps_.pads[j].box, lr) : point_in_poly(ps_.pads[j].c, lq)) hit.push_back(j);
        }
    }
    if (hit.empty()) { gui::status("No pad feature there."); return; }
    std::vector<int> idx = have_res_ ? res_.pad_indices : std::vector<int>{};
    if (add) idx.insert(idx.end(), hit.begin(), hit.end());
    else idx.erase(std::remove_if(idx.begin(), idx.end(), [&](int k) { return std::find(hit.begin(), hit.end(), k) != hit.end(); }), idx.end());
    matches_.clear();
    cur_match_ = -1;
    if (rebuild_from(idx)) gui::status("Buffer: " + std::to_string(res_.pad_indices.size()) + " pad(s): " + res_.name);
    else gui::status("Buffer empty.");
}

std::string GerberWorkspace::make_package(const std::string& name0, const std::vector<Pad>& wpads, double R, Side side) {
    std::string name = name0;
    auto it = doc->prj.packages.find(name);
    if (it != doc->prj.packages.end()) {
        if (it->second.pads.size() == wpads.size()) return name;
        int n = 2;
        while (doc->prj.packages.count(name0 + "_" + std::to_string(n))) ++n;
        name = name0 + "_" + std::to_string(n);
        gui::status("Package " + name0 + " exists with a different pad count; created " + name);
    }
    Package p = res_.pkg;
    p.name = name;
    p.pads.clear();
    for (Pad q : wpads) {
        Vec2 l = rotate(q.pos, -R);
        if (side == Side::Bottom) { l.x = -l.x; q.rot = norm_deg(R - q.rot); }
        else q.rot = norm_deg(q.rot - R);
        q.pos = l;
        normalize_pad(q);
        p.pads.push_back(q);
    }
    p.leads = (int)p.pads.size();
    p.smt = res_.smt;
    if (p.description.empty())
        p.description = "Created from Gerber pads (" + res_.layout + (res_.pitch > 0 ? ", pitch " + dz::fmt_mm(res_.pitch) + " mm" : std::string()) + ")";
    p.auto_body();
    pkg_auto_orientation(p);   // R12 §9: polarized family by name (diodes, tantalum, LEDs ...)
    doc->prj.packages[name] = std::move(p);
    return name;
}

void GerberWorkspace::scan(const dz::DigitizeResult& pattern, const std::string& pkg, double base_rot, Side side) {
    if (!ensure_ps()) return;
    dz::ScanOptions so;
    so.exclude = used_pads();
    for (auto& m : matches_) so.exclude.insert(so.exclude.end(), m.pads.begin(), m.pads.end());
    for (auto& m : review_) so.exclude.insert(so.exclude.end(), m.pads.begin(), m.pads.end());
    so.any_angle = scan_any_angle_;
    auto found = dz::auto_scan(ps_, pattern, so);
    const GerberLayerRef& lr = doc->prj.layers[act_];
    Xform t = lx(act_);
    for (auto& m : found) {
        // pattern is canonical; base_rot is the source component's rotation (= pattern.rot in world)
        const double rel = m.rot - pattern.rot;
        if (m.rotated) {
            Review rv;
            rv.pos = t.apply(m.centroid);
            rv.rot = norm_deg(base_rot + (lr.mirror ? -rel : rel));
            rv.side = side; rv.pads = m.pads; rv.pkg = pkg; rv.wbox = pads_world_box(m.pads); rv.conf = m.confidence;
            review_.push_back(std::move(rv));
            continue;
        }
        BufMatch b;
        b.pos = t.apply(m.centroid);
        b.rot = norm_deg(base_rot + (lr.mirror ? -rel : rel));
        b.side = side;
        b.pads = m.pads;
        b.pkg = pkg;
        b.wbox = pads_world_box(m.pads);
        matches_.push_back(std::move(b));
    }
}

void GerberWorkspace::accept() {
    if (!have_res_) {
        if (!matches_.empty()) multi_accept();
        else gui::status("No pads selected: drag a box around a component's pads first.");
        return;
    }
    open_add_card();
}

// ---------------------------------------------------------------------------
// Add Component card: compact, non-modal, semi-transparent, placed beside (never over) the
// selected pads and their silkscreen. Keyboard focus goes to Reference on the frame it opens;
// characters typed before the field is active are taken from ImGui's input queue into the field
// (and removed from the queue), and canvas shortcuts are suspended while the card is open, so
// nothing typed is lost and nothing typed is executed as a command.
// ---------------------------------------------------------------------------
void GerberWorkspace::open_add_card() {
    if (act_ < 0 || !ensure_ps() || !have_res_) return;
    const GerberLayerRef& lr = doc->prj.layers[act_];
    const AddCard prev = ac_;
    ac_ = {};
    ac_.open = true;
    ac_.det_rot = norm_deg((lr.mirror ? -res_.rot : res_.rot) + lr.rot);
    if (std::fabs(ac_.det_rot) < 1e-9) ac_.det_rot = 0;
    ac_.rot = ac_.det_rot;
    ac_.side = lr.side == Side::Bottom ? 1 : 0;
    ac_.auto_pkg = res_.name.empty() ? dz::auto_package_name(res_) : res_.name;
    ac_.pkg = ac_.auto_pkg;
    ac_.adv = prev.adv;
    ac_.method = 0;   // R22b §6.3: no automatic "find others" search from this card
    ac_.size = prev.size;
    refresh();
}

bool GerberWorkspace::commit_add() {
    AddCard& A = ac_;
    A.err.clear();
    if (!have_res_ || act_ < 0 || !ensure_ps()) { close_add_card(); return false; }
    if (!A.ref.empty() && doc->prj.find_ref(A.ref)) { A.err = "Reference " + A.ref + " already exists."; A.focus = true; A.caret_end = true; return false; }
    if (A.pkg.empty()) { A.err = "Enter a package name."; return false; }
    const GerberLayerRef& lr = doc->prj.layers[act_];
    const Vec2 cw = lx(act_).apply(res_.centroid);
    std::vector<Pad> inst = res_.pkg.pads;   // canonical package placed at res_.rot (layer coords)
    for (Pad& p : inst) { p.pos = rotate(p.pos, res_.rot); p.rot = norm_deg(p.rot + res_.rot); }
    const std::vector<Pad> wp = world_pads(inst, act_);   // board orientation, relative to the centroid
    const Side side = A.side == 1 ? Side::Bottom : Side::Top;
    double R = norm_deg(A.rot_edited ? A.rot : A.det_rot);
    Vec2 pos = cw;
    std::string pkg = A.pkg;
    bool reuse = false;
    auto it = doc->prj.packages.find(pkg);
    if (it != doc->prj.packages.end() && it->second.pads.size() == wp.size()) {
        if (A.rot_edited) reuse = true;   // explicit angle: honour it
        else {
            // orientation from the selected pads relative to the existing package's canonical
            // orientation, never the angle of an earlier placement (R10 §7, C53 / C61)
            const dz::OrientFit f = dz::fit_orientation(it->second.pads, wp, side == Side::Bottom, 0.15, true, A.det_rot);
            if (f.ok) { reuse = true; R = norm_deg(f.rot); pos = cw + f.offset; }
        }
    }
    // R12 §8: tolerance-aware reuse - a new pad group that matches an existing package within the package tolerance
    // (default 0.05 mm / 5 %, same pad count / shapes / pitch, same 0 deg orientation up to the fitted rotation)
    // reuses it instead of creating chip_1.67x0.80 next to chip_1.65x0.75. "Merge similar packages..." in the package
    // editor offers "keep separate" for genuinely different parts.
    std::string similar;
    if (!reuse && !A.rot_edited && A.pkg == A.auto_pkg) {   // a name typed by the user is honoured
        PkgMatchTol tol;
        tol.abs_mm = app::ui_pref("Pkg.TolAbsUm", 50) / 1000.0;
        tol.rel = app::ui_pref("Pkg.TolRelPct", 5) / 100.0;
        tol.orientation_aware = false;      // the fit below decides the placement rotation
        Package cand = res_.pkg;
        cand.name = pkg;
        for (auto& [n, q] : doc->prj.packages) {
            if (q.pads.size() != wp.size() || n == pkg) continue;
            if (!pkg_similar(q, cand, tol) || pkg_kept_separate(q, cand)) continue;
            const dz::OrientFit f = dz::fit_orientation(q.pads, wp, side == Side::Bottom, std::max(0.15, tol.abs_mm * 3), true, A.det_rot);
            if (f.ok) { reuse = true; R = norm_deg(f.rot); pos = cw + f.offset; similar = n; pkg = n; break; }
        }
    }
    if (!reuse) pkg = make_package(pkg, wp, R, side);
    doc->checkpoint("Add Component");
    used_layers_.insert(lr.name);
    Component c;
    c.ref = A.ref; c.pos = pos; c.rot = std::fabs(R) < 1e-9 ? 0 : R; c.side = side; c.package = pkg;
    c.from_gerber = true; doc->prj.add_component(c);
    recent_.push_back({pos, pkg});
    if (!A.ref.empty()) last_prefix_ = ref_prefix(A.ref);
    const int method = A.method;
    const std::string ref = A.ref;
    dz::DigitizeResult pattern = res_;
    have_res_ = false;
    res_ = {};
    ac_.open = false;
    ac_.ref.clear();
    doc->changed(ui::CH_GEOMETRY);
    char rb[32];
    std::snprintf(rb, sizeof rb, " at %.1f\xC2\xB0", c.rot);
    gui::status("Added " + (ref.empty() ? std::string("component") : ref) + " (" + pkg + ")" + rb +
                (!similar.empty() ? "  \xC2\xB7  reused similar package " + similar + " (within tolerance; Merge similar packages... to keep separate)"
                 : reuse ? "  \xC2\xB7  existing package, orientation from the pads" : ""), ToastKind::Success);
    if (method == 0) return true;
    scan(pattern, pkg, R, side);
    if (matches_.empty()) {
        if (!review_.empty()) { toast(std::to_string(review_.size()) + " rotated placement(s) of " + pkg + " found: review them in the panel", ToastKind::Info, 6.f); return true; }
        toast("No other placements of " + pkg + " were found.", ToastKind::Info, 4.f);
        return true;
    }
    if (method == 1) gui::defer([this]() { multi_accept(); });
    else if (method == 2) gui::defer([this]() { walk_matches(); });
    else accept_all(false, last_prefix_, 1);
    return true;
}

void GerberWorkspace::draw_add_card() {
    AddCard& A = ac_;
    if (!A.open) return;
    if (!have_res_) { close_add_card(); return; }
    const GTheme& T = th();
    ImGuiIO& io = ImGui::GetIO();
    ++A.frames;
    // selection box on screen + the area around it where the silkscreen reference usually is
    const BBox wb = pads_world_box(res_.pad_indices);
    ImVec2 mn(1e9f, 1e9f), mx(-1e9f, -1e9f);
    for (Vec2 q : {Vec2{wb.x0, wb.y0}, Vec2{wb.x1, wb.y0}, Vec2{wb.x1, wb.y1}, Vec2{wb.x0, wb.y1}}) {
        const ImVec2 v = w2s(q);
        mn.x = std::min(mn.x, v.x); mn.y = std::min(mn.y, v.y); mx.x = std::max(mx.x, v.x); mx.y = std::max(mx.y, v.y);
    }
    const float m = std::max({S(36), (mx.x - mn.x) * 0.9f, (mx.y - mn.y) * 0.9f});
    const ImVec2 amn(mn.x - m, mn.y - m), amx(mx.x + m, mx.y + m);   // keep-out
    const ImVec2 cs = A.size.x > 0 ? A.size : ImVec2(S(292), S(150));
    const float g = S(10);
    const ImVec2 c0(pos_.x + g, pos_.y + g), c1(pos_.x + size_.x - g, pos_.y + size_.y - g);
    const float midy = std::clamp((mn.y + mx.y) * 0.5f - cs.y * 0.5f, c0.y, std::max(c0.y, c1.y - cs.y));
    const float midx = std::clamp((mn.x + mx.x) * 0.5f - cs.x * 0.5f, c0.x, std::max(c0.x, c1.x - cs.x));
    const ImVec2 cand[] = {ImVec2(amx.x, midy), ImVec2(amn.x - cs.x, midy), ImVec2(midx, amx.y), ImVec2(midx, amn.y - cs.y),
                           ImVec2(c1.x - cs.x, c0.y + S(70)), ImVec2(c0.x, c0.y + S(70)), ImVec2(c1.x - cs.x, c1.y - cs.y), ImVec2(c0.x, c1.y - cs.y)};
    auto overlap = [&](ImVec2 p) {
        const float ox = std::max(0.f, std::min(p.x + cs.x, amx.x) - std::max(p.x, amn.x));
        const float oy = std::max(0.f, std::min(p.y + cs.y, amx.y) - std::max(p.y, amn.y));
        return ox * oy;
    };
    ImVec2 best = cand[0];
    float bo = 1e30f;
    for (ImVec2 p : cand) {
        p.x = std::clamp(p.x, c0.x, std::max(c0.x, c1.x - cs.x));
        p.y = std::clamp(p.y, c0.y, std::max(c0.y, c1.y - cs.y));
        const float o = overlap(p);
        if (o < bo - 1) { bo = o; best = p; }
        if (o <= 0) break;
    }
    // manual drag offset (kept for the session) on top of the automatic placement
    best.x = std::clamp(best.x + ac_off_.x, c0.x, std::max(c0.x, c1.x - cs.x));
    best.y = std::clamp(best.y + ac_off_.y, c0.y, std::max(c0.y, c1.y - cs.y));
    ImGui::SetNextWindowPos(ImVec2(std::floor(best.x), std::floor(best.y)), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(280), 0), ImVec2(S(340), FLT_MAX));
    const bool hot = ImGui::IsMouseHoveringRect(best, ImVec2(best.x + cs.x, best.y + cs.y), false);
    ImGui::SetNextWindowBgAlpha(hot || A.adv ? 0.97f : 0.86f);
    // focus: the card window takes focus whenever Reference should be typing-ready but is not
    // (first invocation: the new auto-resize window is hidden on its first frame and the focus
    // request was lost; after a middle-drag pan / wheel zoom the canvas took the active id)
    const bool mouse_busy = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right);
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool refocus = A.want_ref && !mouse_busy && !popup && !ImGui::IsAnyItemActive();
    if (A.frames == 1 || refocus) ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(6), S(6)));
    ImGui::PushStyleColor(ImGuiCol_Border, with_alpha(T.accent, 0.85f));
    ImGui::Begin("##addcard", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoMove);
    bool ok = false, cancel = false;
    // title row (drag handle: moves the card; double-click returns to automatic placement)
    {
        const ImVec2 tp = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##acdrag", ImVec2(S(266), ImGui::GetTextLineHeight()));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) { ac_off_.x += io.MouseDelta.x; ac_off_.y += io.MouseDelta.y; request_redraw(2); }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ac_off_ = ImVec2(0, 0);
        if (ImGui::IsItemDeactivated()) A.want_ref = true;
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) { ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll); tooltip("Drag to move the card (kept for this session); double-click for automatic placement"); }
        ImGui::SetCursorScreenPos(tp);
    }
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted("Add Component");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
    ImGui::Text("%zu pad%s \xC2\xB7 %s", res_.pad_indices.size(), res_.pad_indices.size() == 1 ? "" : "s", res_.smt ? "SMT" : "thru-hole");
    ImGui::PopStyleColor();
    const float fw = S(266);
    // Reference
    const ImGuiID ref_id = ImGui::GetID("##ref");
    const bool ref_active = ImGui::GetActiveID() == ref_id;
    const bool route = !ref_active && (!A.ever_active || (A.want_ref && !ImGui::IsAnyItemActive()));
    if (route) {
        // typed before the field became active: route the characters into the field
        for (ImWchar ch : io.InputQueueCharacters) {
            if (ch < 32 || ch == 127 || ch == ' ') continue;
            if (ch < 128) A.ref += (char)std::toupper((int)ch);
            else { char u[8] = {}; ImTextCharToUtf8(u, ch); A.ref += u; }
        }
        io.InputQueueCharacters.resize(0);
    }
    if (ref_active) A.ever_active = true;
    if (!ref_active && A.want_ref && !mouse_busy && !popup && !ImGui::IsAnyItemActive()) { A.focus = true; A.caret_end = true; }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Reference");
    ImGui::SameLine(S(78));
    char buf[64] = {};
    std::snprintf(buf, sizeof buf, "%s", A.ref.c_str());
    if (A.focus) { ImGui::SetKeyboardFocusHere(); A.focus = false; }
    ImGui::SetNextItemWidth(fw - S(78));
    ImGui::PushFont(g_font_bold);
    // keyboard activation selects all; the caret must go after the pre-typed characters instead
    // (otherwise the next key replaces them)
    const bool enter = ImGui::InputTextWithHint("##ref", "e.g. C53", buf, sizeof buf,
                                                ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_EnterReturnsTrue |
                                                    ImGuiInputTextFlags_CallbackAlways,
                                                [](ImGuiInputTextCallbackData* d) {
                                                    bool* first = (bool*)d->UserData;
                                                    if (*first) { d->CursorPos = d->SelectionStart = d->SelectionEnd = d->BufTextLen; *first = false; }
                                                    return 0;
                                                },
                                                &A.caret_end);
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) tooltip("Reference designator as printed on the board. Enter adds; leave empty to assign later (Board > Reference Fusion).");
    A.ref = buf;
    if (enter) ok = true;
    if (ImGui::IsItemActivated()) A.want_ref = true;
    // Package (auto)
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Package");
    ImGui::SameLine(S(78));
    char pb[128] = {};
    std::snprintf(pb, sizeof pb, "%s", A.pkg.c_str());
    ImGui::SetNextItemWidth(fw - S(78) - ImGui::GetFrameHeight() - S(2));
    if (ImGui::InputText("##pkg", pb, sizeof pb, ImGuiInputTextFlags_EnterReturnsTrue)) ok = true;
    A.pkg = pb;
    ImGui::SameLine(0, S(2));
    if (ImGui::BeginCombo("##pkgl", nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable((A.auto_pkg + "  (detected)").c_str(), A.pkg == A.auto_pkg)) A.pkg = A.auto_pkg;
        for (auto& [n, p] : doc->prj.packages)
            if (p.pads.size() == res_.pad_indices.size() && n != A.auto_pkg && ImGui::Selectable(n.c_str(), A.pkg == n)) A.pkg = n;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) tooltip("Existing packages with the same pad count");
    {
        auto it = doc->prj.packages.find(A.pkg);
        const bool exists = it != doc->prj.packages.end();
        std::string note = A.pkg == A.auto_pkg ? (exists ? "auto \xC2\xB7 existing package" : "auto \xC2\xB7 new package") : (exists ? "existing package" : "new package");
        if (exists && it->second.pads.size() != res_.pad_indices.size()) note += " (different pad count: a variant is created)";
        ImGui::PushStyleColor(ImGuiCol_Text, T.text_faint);
        ImGui::SetCursorPosX(S(12) + S(78));
        ImGui::TextUnformatted(note.c_str());
        ImGui::PopStyleColor();
    }
    // Advanced
    ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
    if (ImGui::Selectable(A.adv ? "Advanced \xE2\x96\xBE" : "Advanced \xE2\x96\xB8", false, 0, ImVec2(fw, 0))) A.adv = !A.adv;
    ImGui::PopStyleColor();
    if (A.adv) {
        ImGui::Indent(S(6));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Rotation");
        ImGui::SameLine(S(84));
        ImGui::SetNextItemWidth(S(80));
        double r = A.rot_edited ? A.rot : A.det_rot;
        if (ImGui::InputDouble("##rot", &r, 0, 0, "%.1f")) { A.rot = r; A.rot_edited = std::fabs(norm_deg(r) - norm_deg(A.det_rot)) > 1e-6; }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, T.text_faint);
        ImGui::Text(A.rot_edited ? "manual" : "auto %d%%", (int)std::lround(rot_guess_.confidence * 100));
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) tooltip(("Auto: from the selected pads (" + rot_guess_.why + "). With an existing package the angle that maps its pads onto the selection is used.").c_str());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(78));
        for (double q : {0.0, 90.0, 180.0, 270.0}) {
            char lb[24];
            std::snprintf(lb, sizeof lb, "%.0f\xC2\xB0##q%.0f", q, q);
            if (ImGui::SmallButton(lb)) { A.rot = q; A.rot_edited = true; }
            ImGui::SameLine(0, S(3));
        }
        if (ImGui::SmallButton("Auto")) { A.rot_edited = false; A.rot = A.det_rot; }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Side");
        ImGui::SameLine(S(84));
        ImGui::SetNextItemWidth(S(110));
        ImGui::Combo("##side", &A.side, "Top\0Bottom\0");
        ImGui::PushStyleColor(ImGuiCol_Text, T.text_faint);
        ImGui::Text("%s%s", res_.layout.c_str(), res_.pitch > 0 ? ("  \xC2\xB7  pitch " + fmt_len_u8(res_.pitch)).c_str() : "");
        ImGui::PopStyleColor();
        ImGui::Unindent(S(6));
    }
    if (!A.err.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, T.error); ImGui::TextUnformatted(A.err.c_str()); ImGui::PopStyleColor(); }
    // buttons
    const float bw2 = S(84);
    ImGui::SetCursorPosX(S(12) + fw - bw2 * 2 - S(6));
    if (button("Confirm", Icon::Check, true, ImVec2(bw2, S(26)))) ok = true;
    if (ImGui::IsItemHovered()) tooltip("Create the component (Enter)");
    ImGui::SameLine(0, S(6));
    if (button("Cancel", Icon::None, false, ImVec2(bw2, S(26)))) cancel = true;
    if (ImGui::IsItemHovered()) tooltip("Close without adding (Esc); the pad selection is kept");
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancel = true;
    // another card control taken by the user (package, rotation, ...): stop pulling focus back
    if (ImGui::IsAnyItemActive() && ImGui::GetActiveID() != ref_id && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::GetActiveID() != ImGui::GetID("##acdrag"))
        A.want_ref = false;
    // Enter always commits while the card is open (whatever holds focus: card field, canvas, nothing)
    if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) && A.frames > 1 && !popup) ok = true;
    A.size = ImGui::GetWindowSize();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (A.frames < 4) request_redraw(2);
    if (cancel) { close_add_card(); gui::status("Add Component cancelled; the pads stay selected (Esc again clears them)."); }
    else if (ok) commit_add();
}

void GerberWorkspace::multi_accept() {
    if (matches_.empty()) return;
    bool one = false;
    std::string pre = last_prefix_.empty() ? std::string("U") : last_prefix_;
    int start = 1;
    { int n = 1; next_ref(pre, n); start = n - 1; }
    ui::Form f(L"Accept Multiple Components", 400);
    f.icon((int)Icon::AutoScan);
    f.label(L"Found " + std::to_wstring(matches_.size()) + L" more placement(s) with the same pads.");
    f.check("num", L"Auto-number references (Accept All)", false);
    f.text("prefix", L"Prefix", W(pre));
    f.number("start", L"First number", start, 0);
    f.label(L"Without auto-numbering the references stay empty;\nassign them later with Board > Reference Fusion.");
    f.ok_text(L"Accept All");
    f.extra_button(L"Accept One at a Time", [&](ui::Form&) { one = true; return true; });
    if (!f.run(nullptr)) { gui::status("Matches remain highlighted. Press Enter to add them, Esc to clear."); return; }
    if (one) walk_matches();
    else accept_all(f.checked("num"), f.u8("prefix"), (int)f.num("start", 1));
}

void GerberWorkspace::accept_all(bool number, const std::string& prefix, int start) {
    if (matches_.empty()) return;
    doc->checkpoint("Add All Components");
    if (act_ >= 0 && act_ < nlayers()) used_layers_.insert(doc->prj.layers[act_].name);
    int n = std::max(0, start);
    size_t cnt = matches_.size();
    for (auto& m : matches_) {
        Component c;
        if (number) c.ref = next_ref(prefix, n);
        c.pos = m.pos; c.rot = m.rot; c.side = m.side; c.package = m.pkg;
        c.from_gerber = true; doc->prj.add_component(c);
    }
    matches_.clear();
    cur_match_ = -1;
    doc->changed(ui::CH_GEOMETRY);
    undo_toast("Added " + std::to_string(cnt) + " components");
}

void GerberWorkspace::walk_matches() {
    if (matches_.empty()) return;
    doc->checkpoint("Accept One at a Time");
    std::string pre = last_prefix_.empty() ? std::string("U") : last_prefix_;
    int n = 1;
    size_t i = 0, total = matches_.size(), accepted = 0, k = 0;
    while (i < matches_.size()) {
        ++k;
        cur_match_ = (int)i;
        BBox b = matches_[i].wbox;
        if (b.empty()) b.add(matches_[i].pos);
        double m = std::max({b.w(), b.h(), 2.0});
        b.inflate(m * 1.5);
        zoom_to(b, 0.05);
        bool skip = false;
        int nn = n;
        std::string sug = next_ref(pre, nn);
        ui::Form f(L"One at a Time Acceptance", 340);
        f.header(L"Component " + std::to_wstring(k) + L" of " + std::to_wstring(total) + L" \x2014 " + W(matches_[i].pkg));
        f.text("ref", L"Reference", W(sug));
        f.ok_text(L"Accept");
        f.extra_button(L"Skip", [&](ui::Form&) { skip = true; return true; });
        f.validate([this](ui::Form& F, std::wstring& err) {
            std::string r = F.u8("ref");
            if (!r.empty() && doc->prj.find_ref(r)) { err = L"Reference " + W(r) + L" already exists."; return false; }
            return true;
        });
        if (!f.run(nullptr)) break;
        if (skip) { ++i; continue; }
        Component c;
        c.ref = f.u8("ref");
        c.pos = matches_[i].pos; c.rot = matches_[i].rot; c.side = matches_[i].side; c.package = matches_[i].pkg;
        c.from_gerber = true; doc->prj.add_component(c);
        if (!c.ref.empty()) {
            pre = ref_prefix(c.ref);
            last_prefix_ = pre;
            const char* num = c.ref.c_str() + pre.size();
            n = *num ? (int)std::strtol(num, nullptr, 10) + 1 : nn;
        }
        matches_.erase(matches_.begin() + (long)i);
        ++accepted;
        doc->changed(ui::CH_GEOMETRY);
    }
    cur_match_ = -1;
    gui::status("Accepted " + std::to_string(accepted) + " component(s); " + std::to_string(matches_.size()) + " left in the buffer.");
}

void GerberWorkspace::delete_comp(Vec2 w) {
    const double tol = 4.0 / std::max(scale_, 1e-6);
    int ci = app::hit_component(*doc, w, tol, doc->show_top, doc->show_bottom);
    if (ci < 0) { gui::status("No added component there."); return; }
    std::string r = doc->prj.comps[ci].ref;
    doc->checkpoint("Delete Component");
    doc->prj.comps.erase(doc->prj.comps.begin() + ci);
    doc->changed(ui::CH_GEOMETRY);
    undo_toast("Deleted " + (r.empty() ? std::string("component") : r));
}

void GerberWorkspace::delete_selected() {
    if (have_res_ || !matches_.empty()) { clear_buffer(); gui::status("Buffer cleared."); return; }
    const int n = doc->selected_count();
    if (!n) return;
    doc->checkpoint("Delete Components");
    auto& v = doc->prj.comps;
    v.erase(std::remove_if(v.begin(), v.end(), [](const Component& c) { return c.selected; }), v.end());
    doc->changed(ui::CH_GEOMETRY);
    undo_toast("Deleted " + std::to_string(n) + (n == 1 ? " component" : " components"));
}

// ===========================================================================
// layer tools
// ===========================================================================
// Layer align (CircuitCAM "Align Layers"): pick a feature on the layer to move, then the
// matching feature on the reference layer; the source layer is translated so the two
// feature centres coincide. Two point mode: a second pair also rotates the layer about
// the first target point (angle between the source and target pair vectors).
void GerberWorkspace::align_click(Vec2 w) {
    Hit h;
    if (!hit_feature(w, h)) { gui::status("No Gerber feature under the cursor."); return; }
    int li = h.layer;
    Vec2 c = lx(li).apply(object_center(*glayer(li), glayer(li)->objs[h.obj]));
    if (tool_ == Tool::AlignSrc) {
        if (align_stage_ == 1 && li != align_layer_) {
            gui::status("Pick the second source feature on " + doc->prj.layers[align_layer_].name + ".");
            return;
        }
        align_layer_ = li;
        align_pt_ = c;
        tool_ = Tool::AlignDst;
        gui::status(std::string(align_stage_ ? "Second source" : "Source") + ": " + doc->prj.layers[li].name + ". Now click the matching feature on the reference layer.");
        return;
    }
    if (li == align_layer_) {
        bool found = false;
        const double tol = 3.0 / std::max(scale_, 1e-6);
        for (int i = nlayers() - 1; i >= 0 && !found; --i) {
            if (i == align_layer_ || !glayer(i) || !doc->prj.layers[i].visible) continue;
            int o = gb::hit_test(*glayer(i), lx(i).inverse().apply(w), tol);
            if (o >= 0) { li = i; c = lx(i).apply(object_center(*glayer(i), glayer(i)->objs[o])); found = true; }
        }
        if (!found) { gui::status("Pick the target feature on a different layer."); return; }
    }
    GerberLayerRef& src = doc->prj.layers[align_layer_];
    if (align_stage_ == 0) {
        doc->checkpoint("Align Layer");
        Vec2 d = c - align_pt_;
        src.offset += d;
        align_s1_ = c;          // source point 1 now sits on target 1
        align_d1_ = c;
        if (align_two_) {
            align_stage_ = 1;
            tool_ = Tool::AlignSrc;
            gui::status("Moved " + src.name + " by " + fmt_len_u8(d.x, false) + ", " + fmt_len_u8(d.y) + ". Now pick a second, distant feature on " + src.name + " (rotation).");
            clear_buffer();
            doc->changed(ui::CH_LAYERS);
            return;
        }
        gui::status("Moved " + src.name + " by " + fmt_len_u8(d.x, false) + ", " + fmt_len_u8(d.y) + " to align with " + doc->prj.layers[li].name);
    } else {
        // rotate about target 1 so that source 2 lies on the direction of target 2
        const Vec2 vs = align_pt_ - align_d1_, vt = c - align_d1_;
        if (std::hypot(vs.x, vs.y) < 1e-3 || std::hypot(vt.x, vt.y) < 1e-3) { gui::status("Second pair too close to the first; pick distant features."); align_stage_ = 0; tool_ = Tool::AlignSrc; return; }
        double a = rad2deg(std::atan2(vt.y, vt.x) - std::atan2(vs.y, vs.x));
        a = norm_deg(a);
        if (a > 180) a -= 360;
        src.offset = align_d1_ + rotate(src.offset - align_d1_, a);
        src.rot = norm_deg(src.rot + a);
        char b[48];
        std::snprintf(b, sizeof b, "%.3f", a);
        const double res = std::fabs(std::hypot(vt.x, vt.y) - std::hypot(vs.x, vs.y));
        gui::status("Rotated " + src.name + " by " + b + "\xC2\xB0 about the first point; residual " + fmt_len_u8(res));
        align_stage_ = 0;
    }
    align_layer_ = -1;
    tool_ = Tool::AlignSrc;
    clear_buffer();
    doc->changed(ui::CH_LAYERS);
}

// Transform dialog: offset / rotate / mirror the selected layers (or the pad source layer)
void GerberWorkspace::transform_layers() {
    std::vector<int> ls;
    for (int i = 0; i < nlayers(); ++i) if (layer_selected(i)) ls.push_back(i);
    if (ls.empty() && primary_layer() >= 0) ls.push_back(primary_layer());
    if (ls.empty()) return;
    std::wstring names;
    for (int i : ls) names += (names.empty() ? L"" : L", ") + W(doc->prj.layers[i].name);
    ui::Form f(L"Transform Layers", 420);
    f.icon((int)Icon::Offset);
    f.header(L"Move / rotate / mirror " + std::to_wstring(ls.size()) + L" layer(s)");
    f.label(names);
    f.text("dx", L"Offset X", W(fmt_len_u8(0, false)));
    f.text("dy", L"Offset Y", W(fmt_len_u8(0, false)));
    f.number("rot", L"Rotate (deg, CCW, about the layers' centre)", 0, 3);
    f.check("mir", L"Mirror (flip left / right about the layers' centre)", false);
    f.ok_text(L"Apply");
    if (!f.run(nullptr)) return;
    const double dx = app::parse_len(f.str("dx"), 0), dy = app::parse_len(f.str("dy"), 0);
    const double a = f.num("rot", 0);
    const bool mir = f.checked("mir");
    BBox pb;
    for (int i : ls) pb.add(layer_world_box(i));
    const Vec2 P = pb.empty() ? Vec2{} : pb.center();
    doc->checkpoint("Transform Layers");
    for (int i : ls) {
        GerberLayerRef& r = doc->prj.layers[i];
        if (mir) { r.offset.x = 2 * P.x - r.offset.x; r.rot = norm_deg(-r.rot); r.mirror = !r.mirror; }
        if (a != 0) { r.offset = P + rotate(r.offset - P, a); r.rot = norm_deg(r.rot + a); }
        r.offset += Vec2{dx, dy};
    }
    clear_buffer();
    doc->changed(ui::CH_LAYERS);
}

void GerberWorkspace::crop(const BBox& w, bool inside) {
    int changed = 0;
    for (int i = 0; i < nlayers(); ++i) {
        const gb::Layer* L = glayer(i);
        if (!L || !doc->prj.layers[i].visible) continue;
        Crop& c = crops_[i];
        if (c.L.get() != L) { c.L = doc->glayers[i]; c.hide.assign(L->objs.size(), 0); }
        Xform t = lx(i);
        const ObjIndex* ix = index_of(i);
        for (size_t k = 0; k < L->objs.size(); ++k) {
            if (c.hide[k]) continue;
            BBox b = xform_box(t, ix->boxes[k]);
            bool h = inside ? box_inside(b, w) : !b.intersects(w);
            if (h) { c.hide[k] = 1; ++changed; }
        }
    }
    ++crop_gen_;
    gui::status("Hid " + std::to_string(changed) + " Gerber feature(s). Use Show Hidden to restore.");
    refresh();
}

void GerberWorkspace::show_hidden(int layer, bool all) {
    for (int i = 0; i < nlayers(); ++i) {
        if (!all && i != layer) continue;
        crops_.erase(i);
        doc->prj.layers[i].hidden_dcodes.clear();
    }
    ++crop_gen_;
    doc->changed(ui::CH_LAYERS);
    gui::status("Hidden graphics restored.");
}

void GerberWorkspace::set_frame(const BBox& w) {
    if (w.w() <= 0 || w.h() <= 0) return;
    tool_ = Tool::Digitize;
    if (!doc->prj.board_frame.empty() && !ui::message(nullptr, L"Circuit Outline", L"Replace the existing circuit outline?", true)) return;
    doc->checkpoint("Set Circuit Outline");
    doc->prj.board_frame = {{{w.x0, w.y0}, {w.x1, w.y0}, {w.x1, w.y1}, {w.x0, w.y1}}};
    doc->prj.board_frame_from_gerber = true;
    doc->prj.board_outline_src = OutlineSource::Confirmed;   // R16: user-defined on the Gerber
    doc->changed(ui::CH_GEOMETRY);
    gui::status("Circuit outline set: " + fmt_wh(w.w(), w.h()));
}

void GerberWorkspace::frame_from_outline() {
    int oi = -1;
    for (int i = 0; i < nlayers() && oi < 0; ++i) if (doc->prj.layers[i].role == LayerRole::Outline && glayer(i)) oi = i;
    if (oi < 0) { ui::message(nullptr, L"Circuit Outline", L"No layer has the Outline role. Set it in the layer's context menu."); return; }
    const gb::Layer& L = *glayer(oi);
    std::vector<std::vector<Vec2>> segs, closed;
    for (auto& o : L.objs) {
        if (o.clear) continue;
        if (o.kind == gb::Object::Draw && dist(o.a, o.b) > 1e-6) segs.push_back({o.a, o.b});
        else if (o.kind == gb::Object::Arc) segs.push_back(gb::arc_points(o, 72));
        else if (o.kind == gb::Object::Region && o.poly.size() >= 3) closed.push_back(o.poly);
    }
    const double tol = 0.02;
    auto key = [&](Vec2 p, int dx, int dy) {
        long long x = (long long)std::floor(p.x / tol) + dx, y = (long long)std::floor(p.y / tol) + dy;
        return (x << 32) ^ (y & 0xFFFFFFFFLL);
    };
    std::unordered_multimap<long long, int> ends;
    for (int s = 0; s < (int)segs.size(); ++s) { ends.emplace(key(segs[s].front(), 0, 0), s * 2); ends.emplace(key(segs[s].back(), 0, 0), s * 2 + 1); }
    std::vector<char> used(segs.size(), 0);
    auto find_next = [&](Vec2 p) -> int {
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                auto r = ends.equal_range(key(p, dx, dy));
                for (auto it = r.first; it != r.second; ++it) {
                    int s = it->second / 2, e = it->second % 2;
                    if (used[s]) continue;
                    if (dist(e ? segs[s].back() : segs[s].front(), p) <= tol) return it->second;
                }
            }
        return -1;
    };
    for (int s = 0; s < (int)segs.size(); ++s) {
        if (used[s]) continue;
        used[s] = 1;
        std::vector<Vec2> chain = segs[s];
        for (int guard = 0; guard < (int)segs.size(); ++guard) {
            if (chain.size() > 2 && dist(chain.back(), chain.front()) <= tol) break;
            int nx = find_next(chain.back());
            if (nx < 0) break;
            int ns = nx / 2;
            used[ns] = 1;
            std::vector<Vec2> add = segs[ns];
            if (nx % 2) std::reverse(add.begin(), add.end());
            chain.insert(chain.end(), add.begin() + 1, add.end());
        }
        if (chain.size() > 3 && dist(chain.back(), chain.front()) <= tol) { chain.pop_back(); closed.push_back(chain); }
    }
    if (closed.empty()) { ui::message(nullptr, L"Circuit Outline", L"No closed contour was found on layer " + W(doc->prj.layers[oi].name) + L"."); return; }
    size_t best = 0;
    double best_a = -1;
    for (size_t i = 0; i < closed.size(); ++i) {
        BBox b;
        for (auto& p : closed[i]) b.add(p);
        if (b.w() * b.h() > best_a) { best_a = b.w() * b.h(); best = i; }
    }
    Xform t = lx(oi);
    std::vector<Vec2> poly;
    for (auto& p : closed[best]) poly.push_back(t.apply(p));
    if (!doc->prj.board_frame.empty() && !ui::message(nullptr, L"Circuit Outline", L"Replace the existing circuit outline with the outline contour?", true)) return;
    doc->checkpoint("Circuit Outline From Outline");
    doc->prj.board_frame = {poly};
    doc->prj.board_frame_from_gerber = true;
    doc->prj.board_outline_src = OutlineSource::Imported;    // R16: real Gerber outline geometry
    doc->changed(ui::CH_GEOMETRY);
    BBox b;
    for (auto& p : poly) b.add(p);
    gui::status("Circuit outline from outline: " + fmt_wh(b.w(), b.h()) + ", " + std::to_string(poly.size()) + " vertices");
}

void GerberWorkspace::rotate_layers(int layer, bool all) {
    double a = 90;
    if (!ui::prompt_number(nullptr, all ? L"Rotate All Layers" : L"Rotate This Layer", L"Angle (degrees, counter-clockwise):", a, 3)) return;
    if (a == 0) return;
    BBox pb;
    for (int i = 0; i < nlayers(); ++i) if (all || i == layer) pb.add(layer_world_box(i));
    Vec2 P = pb.empty() ? Vec2{} : pb.center();
    for (int i = 0; i < nlayers(); ++i) {
        if (!all && i != layer) continue;
        GerberLayerRef& r = doc->prj.layers[i];
        r.offset = P + rotate(r.offset - P, a);
        r.rot = norm_deg(r.rot + a);
    }
    clear_buffer();
    doc->changed(ui::CH_LAYERS);
}

void GerberWorkspace::flip_layer(int layer) {
    if (layer < 0 || layer >= nlayers()) return;
    BBox b = layer_world_box(layer);
    Vec2 P = b.empty() ? Vec2{} : b.center();
    GerberLayerRef& r = doc->prj.layers[layer];
    r.offset.x = 2 * P.x - r.offset.x;
    r.rot = norm_deg(-r.rot);
    r.mirror = !r.mirror;
    clear_buffer();
    doc->changed(ui::CH_LAYERS);
    gui::status("Flipped layer " + r.name);
}

void GerberWorkspace::reset_offset(int layer, bool all) {
    for (int i = 0; i < nlayers(); ++i) {
        if (!all && i != layer) continue;
        GerberLayerRef& r = doc->prj.layers[i];
        r.offset = {}; r.rot = 0; r.mirror = false;
    }
    clear_buffer();
    doc->changed(ui::CH_LAYERS);
}

void GerberWorkspace::hide_aperture(int layer, int dcode, bool all) {
    for (int i = 0; i < nlayers(); ++i) {
        if (!all && i != layer) continue;
        auto& h = doc->prj.layers[i].hidden_dcodes;
        if (std::find(h.begin(), h.end(), dcode) == h.end()) h.push_back(dcode);
    }
    doc->changed(ui::CH_LAYERS);
    gui::status("Hid aperture D" + std::to_string(dcode) + (all ? " on all layers" : ""));
}

// ===========================================================================
// UI: left panel
// ===========================================================================
void GerberWorkspace::draw_panel(float w, float h) {
    ImGui::BeginChild("##gpanel", ImVec2(w, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    // layer list / Add Components split: drag the bar between them (R10 §8: resizable)
    if (dig_h_ <= 0) dig_h_ = S(236);
    dig_h_ = std::clamp(dig_h_, S(120), std::max(S(120), h - S(140)));
    const float lh = std::max(S(140), h - dig_h_ - S(6));
    ImGui::BeginChild("##glayers", ImVec2(0, lh), ImGuiChildFlags_None);
    draw_layers_panel();
    ImGui::EndChild();
    {
        ImGui::InvisibleButton("##hsplit", ImVec2(std::max(1.f, ImGui::GetContentRegionAvail().x), S(6)));
        const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
        if (hv || ac) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ac) dig_h_ -= ImGui::GetIO().MouseDelta.y;
        if (hv && !ac) tooltip("Drag to resize the layer list");
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + S(4), (a.y + b.y) * 0.5f), ImVec2(b.x - S(4), (a.y + b.y) * 0.5f), hv || ac ? th().accent : th().border, hv || ac ? S(2) : S(1));
    }
    ImGui::BeginChild("##gdig", ImVec2(0, 0), ImGuiChildFlags_None);
    draw_digitize_panel();
    ImGui::EndChild();
    ImGui::EndChild();
}

// ---- layer visibility / order operations
// ops: 0 unhide all, 1 hide all, 2 hide unselected, 3 hide selection, 4 hide bottom, 5 hide top, 6 hide already added
void GerberWorkspace::vis_op(int op) {
    auto& ls = doc->prj.layers;
    const int n = nlayers();
    bool anysel = false;
    for (int i = 0; i < n; ++i) anysel |= layer_selected(i);
    const int prim = primary_layer();
    auto sel = [&](int i) { return anysel ? layer_selected(i) : i == prim; };
    int cnt = 0;
    for (int i = 0; i < n; ++i) {
        bool before = ls[i].visible;
        switch (op) {
        case 0: ls[i].visible = true; break;
        case 1: ls[i].visible = false; break;
        case 2: if (!sel(i)) ls[i].visible = false; break;
        case 3: if (sel(i)) ls[i].visible = false; break;
        case 4: if (ls[i].side == Side::Bottom && ls[i].role != LayerRole::Drill && ls[i].role != LayerRole::Outline) ls[i].visible = false; break;
        case 5: if (ls[i].side == Side::Top && ls[i].role != LayerRole::Drill && ls[i].role != LayerRole::Outline) ls[i].visible = false; break;
        case 6: if (used_layers_.count(ls[i].name)) ls[i].visible = false; break;
        }
        if (before != ls[i].visible) ++cnt;
    }
    if (op == 6 && used_layers_.empty()) gui::status("No layer has been used to add components yet.");
    doc->changed(ui::CH_LAYERS);
    dirty_ = true;
}

// ViewMate layer stepping: hide everything and show only the next layer above (-1) / below (+1)
// Show next (+1) / previous (-1) layer: the next layer IN THE LIST (as sorted / grouped) becomes
// the active (pad source) layer and the only visible one. Draw order is not changed.
void GerberWorkspace::step_layer(int dir) {
    const int n = nlayers();
    if (n == 0) return;
    compute_order();
    const std::vector<int>& ord = disp_order_;
    int cur = act_ >= 0 ? act_ : primary_layer();
    int nvis = 0, vis1 = -1;
    for (int i = 0; i < n; ++i) if (doc->prj.layers[i].visible) { ++nvis; vis1 = i; }
    if (nvis == 1) cur = vis1;
    int pos = -1;
    for (int k = 0; k < (int)ord.size(); ++k) if (ord[k] == cur) pos = k;
    int np = pos < 0 ? (dir > 0 ? 0 : n - 1) : ((pos + dir) % n + n) % n;
    if (nvis > 1 && pos >= 0 && cur == act_) np = pos;   // first press from "many visible": solo the active layer
    const int nx = ord[np];
    for (int i = 0; i < n; ++i) doc->prj.layers[i].visible = i == nx;
    sel_layer_ = nx;
    lsel_.assign(n, 0);
    lsel_[nx] = 1;
    lsel_anchor_ = nx;
    if (glayer(nx)) set_active(nx);
    doc->changed(ui::CH_LAYERS);
    dirty_ = true;
    gui::status("Layer " + std::to_string(np + 1) + "/" + std::to_string(n) + ": " + doc->prj.layers[nx].name +
                " (active, only visible)   PgDn / ] next, PgUp / [ previous, 0 shows all");
}

// list order: draw order (doc order), or sorted by name / type / side, optionally grouped
void GerberWorkspace::compute_order() {
    const int n = nlayers();
    auto side_key = [&](int i) {
        const std::string sb = side_badge(doc->prj.layers[i]);
        return sb == "TOP" ? 0 : sb.rfind("IN", 0) == 0 ? 1 : sb == "BOT" ? 2 : 3;   // top / inner / bottom / none
    };
    auto type_key = [&](int i) { return (int)doc->prj.layers[i].role; };
    auto group_key = [&](int i) { return group_by_ == 1 ? side_key(i) : group_by_ == 2 ? type_key(i) : 0; };
    disp_order_.resize(n);
    for (int i = 0; i < n; ++i) disp_order_[i] = i;
    std::stable_sort(disp_order_.begin(), disp_order_.end(), [&](int a, int b) {
        const int ga = group_key(a), gb_ = group_key(b);
        if (ga != gb_) return ga < gb_;
        int c = 0;
        switch (sort_col_) {
        case 1: c = lower(doc->prj.layers[a].name).compare(lower(doc->prj.layers[b].name)); break;
        case 2: c = type_key(a) - type_key(b); break;
        case 3: c = side_key(a) - side_key(b); break;
        default: c = a - b; break;
        }
        if (c == 0) c = a - b;
        return sort_desc_ ? c > 0 : c < 0;
    });
    disp_group_.assign(n, -1);
    if (group_by_) for (int k = 0; k < n; ++k) disp_group_[k] = group_key(disp_order_[k]);
}

bool GerberWorkspace::selection_centroid(Vec2& out) const {
    if (have_res_ && act_ >= 0 && act_ < nlayers()) { out = lx(act_).apply(res_.centroid); return true; }
    if (cur_match_ >= 0 && cur_match_ < (int)matches_.size()) { out = matches_[cur_match_].pos; return true; }
    if (!matches_.empty()) { BBox b; for (auto& m : matches_) b.add(m.pos); out = b.center(); return true; }
    BBox b;
    int n = 0;
    for (const Component& c : doc->prj.comps) if (c.selected) { b.add(c.pos); ++n; }
    if (n) { out = b.center(); return true; }
    if (have_pin_ && glayer(pinned_hit_.layer)) { out = lx(pinned_hit_.layer).apply(object_center(*glayer(pinned_hit_.layer), glayer(pinned_hit_.layer)->objs[pinned_hit_.obj])); return true; }
    return false;
}

void GerberWorkspace::centre_crosshair_on_selection() {
    Vec2 c;
    if (!selection_centroid(c)) { gui::status("C: nothing selected (select pads, a component or pin a feature first)."); return; }
    ch_w_ = c;
    ch_docked_ = false;
    // bring it into view if needed
    ImVec2 s = w2s(c);
    if (s.x < pos_.x || s.y < pos_.y || s.x > pos_.x + size_.x || s.y > pos_.y + size_.y) { cx_ = c.x; cy_ = c.y; dirty_ = true; }
    gui::status("Crosshair centred on the selection centroid (undocked; Space docks it again).");
}

void GerberWorkspace::rename_layer(int i) {
    if (i < 0 || i >= nlayers()) return;
    app::DocPtr d = doc;
    const std::string old = d->prj.layers[i].name;
    gui::defer([d, i, old]() {
        if (i >= (int)d->prj.layers.size() || d->prj.layers[i].name != old) return;
        std::wstring v = W(old);
        if (!ui::prompt_text(nullptr, L"Rename Layer", L"Layer name", v) || v.empty() || U(v) == old) return;
        d->checkpoint("Rename Gerber Layer");
        d->prj.layers[i].name = U(v);
        d->changed(ui::CH_LAYERS);
    });
}

// pads of the pad-source layer that are covered by an added component (cached per doc generation)
const std::vector<char>& GerberWorkspace::used_mask() {
    if (!ensure_ps()) { used_mask_.clear(); return used_mask_; }
    if (used_gen_ != doc->data_gen || used_ps_ != (const void*)ps_layer_.get() || used_npads_ != ps_.pads.size() || used_mask_.size() != ps_.pads.size()) {
        used_mask_.assign(ps_.pads.size(), 0);
        for (int k : used_pads()) if (k >= 0 && k < (int)used_mask_.size()) used_mask_[k] = 1;
        used_gen_ = doc->data_gen;
        used_ps_ = ps_layer_.get();
        used_npads_ = ps_.pads.size();
    }
    return used_mask_;
}

void GerberWorkspace::move_selected(int dir) {
    std::vector<int> v;
    for (int i = 0; i < nlayers(); ++i) if (layer_selected(i)) v.push_back(i);
    if (v.empty() && primary_layer() >= 0) { v.push_back(primary_layer()); lsel_.resize(nlayers(), 0); lsel_[v[0]] = 1; }
    if (v.empty()) return;
    if (dir < 0 ? v.front() == 0 : v.back() + 1 >= nlayers()) return;
    const std::string act_name = act_ >= 0 ? doc->prj.layers[act_].name : std::string();
    if (dir < 0) for (int i : v) swap_layers(i, i - 1);
    else for (auto it = v.rbegin(); it != v.rend(); ++it) swap_layers(*it, *it + 1);
    if (sel_layer_ >= 0) sel_layer_ = std::clamp(sel_layer_, 0, nlayers() - 1);
}

void GerberWorkspace::show_apertures(int li) {
    const gb::Layer* L = glayer(li);
    if (!L) return;
    ensure_cache(li);
    const LayerCache& c = cache(li);
    std::string t = "Apertures of " + doc->prj.layers[li].name + "  (" + ((Units)app::settings().units == Units::MM ? "mm" : "mil") + ")\n\n";
    t += "D-code   Shape        Size                 Uses\n";
    for (auto& [d, ap] : L->aps) {
        auto it = c.usage.find(d);
        char b[160];
        std::string sz = ap.shape == gb::Aperture::Circle || ap.shape == gb::Aperture::Polygon ? fmt_ap(ap.w) : fmt_ap(ap.w) + " x " + fmt_ap(ap.h);
        if (ap.hole > 0) sz += " h" + fmt_ap(ap.hole);
        const auto& hid = doc->prj.layers[li].hidden_dcodes;
        std::snprintf(b, sizeof b, "D%-7d %-12s %-20s %d%s\n", d, shape_name(ap).substr(0, 12).c_str(), sz.c_str(), it == c.usage.end() ? 0 : it->second,
                      std::find(hid.begin(), hid.end(), d) != hid.end() ? "  (hidden)" : "");
        t += b;
    }
    ui::show_text(nullptr, L"Apertures", W(t));
}

void GerberWorkspace::visibility_menu_items() {
    if (imenu("Unhide All", Icon::Eye, "0")) vis_op(0);
    if (imenu("Hide All", Icon::EyeOff)) vis_op(1);
    ImGui::Separator();
    if (imenu("Hide Unselected", Icon::Solo)) vis_op(2);
    if (imenu("Hide Selection", Icon::EyeOff)) vis_op(3);
    ImGui::Separator();
    if (imenu("Hide Bottom Layers", Icon::BottomVisible)) vis_op(4);
    if (imenu("Hide Top Layers", Icon::TopVisible)) vis_op(5);
    if (imenu("Hide Already Added", Icon::Check, nullptr, false, !used_layers_.empty())) vis_op(6);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip("Hide the layers whose pads were already used to add components");
    ImGui::Separator();
    if (imenu("Show Previous Layer Only \xE2\x96\xB2", Icon::ArrowUp, "PgUp / [")) step_layer(-1);
    if (imenu("Show Next Layer Only \xE2\x96\xBC", Icon::ArrowDown, "PgDn / ]")) step_layer(+1);
}

void GerberWorkspace::layer_menu_items(int i) {
    GerberLayerRef& L = doc->prj.layers[i];
    const std::vector<int> tg = layer_targets(i);   // the selected set (right-click selects the row first)
    if (tg.size() > 1) { ImGui::PushStyleColor(ImGuiCol_Text, th().text_faint); ImGui::Text("%zu selected layers", tg.size()); ImGui::PopStyleColor(); }
    if (imenu("Make Working Layer (pads source)", Icon::Digitize, "double-click", i == act_)) set_active(i);
    if (ImGui::IsItemHovered()) tooltip("Add Component from Pads reads the pads of this layer");
    {
        bool allv = true;
        for (int q : tg) allv &= doc->prj.layers[q].visible;
        if (imenu("Visible", Icon::Eye, "Space", allv)) { for (int q : tg) doc->prj.layers[q].visible = !allv; doc->changed(ui::CH_LAYERS); }
    }
    if (imenu(tg.size() > 1 ? "Solo (show only these layers)" : "Solo (show only this layer)", Icon::Solo, "Ctrl+click eye")) {
        for (int k = 0; k < nlayers(); ++k) doc->prj.layers[k].visible = std::find(tg.begin(), tg.end(), k) != tg.end();
        doc->changed(ui::CH_LAYERS);
    }
    bool ng = negative(i);
    if (imenu("Negative", Icon::FillOutline, nullptr, ng)) { for (int q : tg) neg_[doc->prj.layers[q].name] = !ng; dirty_ = true; }
    if (begin_menu_icon("Visibility", Icon::Eye)) { visibility_menu_items(); ImGui::EndMenu(); }
    ImGui::Separator();
    if (begin_menu_icon("Colour", Icon::Palette)) {
        float c[3] = {((L.color >> 16) & 255) / 255.f, ((L.color >> 8) & 255) / 255.f, (L.color & 255) / 255.f};
        if (ImGui::ColorPicker3("##mcp", c, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview)) {
            const uint32_t col = 0xFF000000u | ((uint32_t)(c[0] * 255 + .5f) << 16) | ((uint32_t)(c[1] * 255 + .5f) << 8) | (uint32_t)(c[2] * 255 + .5f);
            for (int q : tg) doc->prj.layers[q].color = col;
            doc->changed(ui::CH_LAYERS);
        }
        if (ImGui::MenuItem("By layer type")) { for (int q : tg) doc->prj.layers[q].color = role_color(doc->prj.layers[q].role, q); doc->changed(ui::CH_LAYERS); }
        ImGui::EndMenu();
    }
    if (imenu("Rename...", Icon::Text, nullptr, false, tg.size() == 1)) rename_layer(i);
    if (begin_menu_icon("Layer Type", Icon::Layers)) {
        for (size_t r = 0; r < role_names().size(); ++r)
            if (ImGui::MenuItem(U(role_names()[r]).c_str(), nullptr, (size_t)L.role == r)) { for (int q : tg) doc->prj.layers[q].role = (LayerRole)r; doc->changed(ui::CH_LAYERS); }
        ImGui::EndMenu();
    }
    if (begin_menu_icon("Side", Icon::SecondSide)) {
        if (menu_item_icon("Top", Icon::TopVisible, nullptr, L.side == Side::Top)) { for (int q : tg) doc->prj.layers[q].side = Side::Top; doc->changed(ui::CH_LAYERS); }
        if (menu_item_icon("Bottom", Icon::BottomVisible, nullptr, L.side == Side::Bottom)) { for (int q : tg) doc->prj.layers[q].side = Side::Bottom; doc->changed(ui::CH_LAYERS); }
        ImGui::EndMenu();
    }
    if (imenu("Auto-detect Type and Side", Icon::Wand)) { for (int q : tg) guess_role(doc->prj.layers[q], q); doc->changed(ui::CH_LAYERS); }
    if (imenu(tg.size() > 1 ? "Export Selected Layers..." : "Export Layer...", Icon::Export)) gui::defer([this]() { export_layers(0); });
    ImGui::Separator();
    if (imenu("Move Layer Up (draw order)", Icon::Move, "Ctrl+[", false, i > 0)) move_selected(-1);
    if (imenu("Move Layer Down (draw order)", Icon::Move, "Ctrl+]", false, i + 1 < nlayers())) move_selected(+1);
    ImGui::Separator();
    if (imenu("Align Layer (pick source, then target)...", Icon::Offset)) { align_two_ = false; set_tool(Tool::AlignSrc); }
    if (imenu("Align Layer, Two Points (move + rotate)...", Icon::Offset)) { align_two_ = true; set_tool(Tool::AlignSrc); }
    if (imenu("Transform Layers (offset / rotate / mirror)...", Icon::Offset)) gui::defer([this]() { transform_layers(); });
    if (imenu("Rotate Layer...", Icon::Rot90)) { int k = i; gui::defer([this, k]() { rotate_layers(k, false); }); }
    if (imenu("Mirror Layer", Icon::Flip)) flip_layer(i);
    if (imenu("Reset Offset / Rotation", Icon::Undo)) reset_offset(i, false);
    ImGui::Separator();
    if (imenu("Apertures (D-codes)...", Icon::Report)) { int k = i; gui::defer([this, k]() { show_apertures(k); }); }
    if (imenu("Show Hidden Graphics", Icon::Eye)) show_hidden(i, false);
    if (imenu("Compare With Pad Source Layer", Icon::Compare, nullptr, false, act_ >= 0 && act_ != i)) { cmp_a_ = act_; cmp_b_ = i; compare_ = true; dirty_ = true; }
    {   // Screen blend: the ViewMate compare (two layers in complementary colours: overlap white, a miss stays coloured)
        const bool sc = screened(i);
        if (imenu("Screen Blend (overlaps add up)", Icon::Compare, nullptr, sc)) {
            for (int q : tg) { if (sc) screen_.erase(doc->prj.layers[q].name); else screen_[doc->prj.layers[q].name] = 0; }
            dirty_ = true;
        }
        if (imenu("Screen Compare With Active Layer (magenta / green)", Icon::Compare, nullptr, false, act_ >= 0 && act_ != i)) {
            screen_.clear();
            screen_[doc->prj.layers[act_].name] = 0xFFFF00FFu;
            screen_[doc->prj.layers[i].name] = 0xFF00FF00u;
            doc->prj.layers[act_].visible = doc->prj.layers[i].visible = true;
            gui::status("Screen compare: white = on both layers, magenta = only on " + doc->prj.layers[act_].name + ", green = only on " +
                        doc->prj.layers[i].name + ". Hide the other layers to see just these two.", ToastKind::Info);
            dirty_ = true;
        }
        if (!screen_.empty() && imenu("Clear Screen Blend (all layers)", Icon::Undo)) { screen_.clear(); dirty_ = true; }
    }
    ImGui::Separator();
    if (imenu("Remove Layer", Icon::Delete)) {
        int k = i;
        app::DocPtr d = doc;
        gui::defer([d, k]() {
            if (k >= (int)d->prj.layers.size()) return;
            if (!ui::message(nullptr, L"Remove Layer", L"Remove layer " + W(d->prj.layers[k].name) + L" from the project?", true)) return;
            d->checkpoint("Remove Gerber Layer");
            d->prj.layers.erase(d->prj.layers.begin() + k);
            if (k < (int)d->glayers.size()) d->glayers.erase(d->glayers.begin() + k);
            d->changed(ui::CH_LAYERS);
        });
    }
    if (imenu("Load / Edit Layers...", Icon::ImportGerber)) post_command(ID_IMPORT_GERBER);
}

std::vector<int> GerberWorkspace::layer_targets(int i) const {
    std::vector<int> t;
    if (layer_selected(i)) { for (int k = 0; k < nlayers(); ++k) if (layer_selected(k)) t.push_back(k); }
    else if (i >= 0 && i < nlayers()) t.push_back(i);
    return t;
}

// Export layers as RS-274X (Excellon for drill layers): mode 0 selected, 1 all visible, 2 chosen.
// Coordinates are written in layer coordinates (as in the source file); hidden D-codes and
// cropped graphics can be left out. Runs from gui::defer (file dialogs are modal).
void GerberWorkspace::export_layers(int mode) {
    const int n = nlayers();
    std::vector<int> ls;
    for (int i = 0; i < n; ++i) {
        if (!glayer(i)) continue;
        if (mode == 0 && layer_selected(i)) ls.push_back(i);
        if (mode == 1 && doc->prj.layers[i].visible) ls.push_back(i);
    }
    if (mode == 0 && ls.empty() && glayer(primary_layer())) ls.push_back(primary_layer());
    bool visible_only = true;
    if (mode == 2 || !ls.empty()) {
        std::vector<std::vector<std::wstring>> rows;
        std::vector<int> idx;
        for (int i = 0; i < n; ++i) {
            if (!glayer(i)) continue;
            idx.push_back(i);
            const GerberLayerRef& L = doc->prj.layers[i];
            rows.push_back({W(L.name), W(role_name_u8(L.role)), W(side_badge(L)), glayer(i)->excellon || L.role == LayerRole::Drill ? L"Excellon" : L"RS-274X"});
        }
        ui::Form f(L"Export Layers", 460);
        f.icon((int)Icon::Export);
        f.header(mode == 2 ? L"Choose the layers to export" : L"Export " + std::to_wstring(ls.size()) + L" layer(s)");
        f.list("ls", {L"Layer", L"Type", L"Side", L"Format"}, rows, std::min<int>(10, (int)rows.size()), true);
        f.check("vis", L"Leave out hidden D-codes and cropped graphics", true);
        f.tip(L"Exports what the viewer shows for each layer. Off: every object of the layer.");
        f.label(L"One file per layer, metric 4.6, in layer coordinates (the source file's coordinates).");
        f.ok_text(L"Export...");
        // pre-check
        ui::Field* lf = f.field("ls");
        if (lf) {
            lf->list_checked.assign(rows.size(), false);
            for (size_t r = 0; r < idx.size(); ++r) lf->list_checked[r] = std::find(ls.begin(), ls.end(), idx[r]) != ls.end();
        }
        if (!f.run(nullptr)) return;
        visible_only = f.checked("vis");
        auto ch = f.list_checks("ls");
        ls.clear();
        for (size_t r = 0; r < idx.size() && r < ch.size(); ++r) if (ch[r]) ls.push_back(idx[r]);
    }
    if (ls.empty()) { gui::status("No layers to export."); return; }
    auto base_name = [&](int i) {
        const GerberLayerRef& L = doc->prj.layers[i];
        std::string b = L.path;
        size_t sl = b.find_last_of("/\\");
        if (sl != std::string::npos) b = b.substr(sl + 1);
        if (b.empty()) {
            b = L.name;
            for (char& c : b) if (std::strchr("<>:\"/\\|?*", c)) c = '_';
            b += glayer(i)->excellon ? ".drl" : ".gbr";
        }
        return b;
    };
    auto render = [&](int i) {
        const gb::Layer* G = glayer(i);
        std::vector<char> keep;
        if (visible_only) { keep.resize(G->objs.size()); for (size_t k = 0; k < keep.size(); ++k) keep[k] = obj_visible(i, (int)k) ? 1 : 0; }
        const std::string cm = "layer " + doc->prj.layers[i].name;
        return G->excellon ? gb::write_excellon(*G, visible_only ? &keep : nullptr, cm) : gb::write_rs274x(*G, visible_only ? &keep : nullptr, cm);
    };
    std::vector<std::pair<std::string, int>> out;   // path, layer
    if (ls.size() == 1) {
        const bool xl = glayer(ls[0])->excellon;
        std::wstring path = W(base_name(ls[0]));
        if (!ui::save_file_dialog(nullptr, L"Export Layer", xl ? std::vector<ui::FileFilter>{{L"Excellon drill", L"*.drl;*.xln;*.txt"}, {L"All files", L"*.*"}}
                                                             : std::vector<ui::FileFilter>{{L"Gerber RS-274X", L"*.gbr;*.ger;*.gtl;*.gbl;*.gtp;*.gts;*.gto"}, {L"All files", L"*.*"}},
                                  xl ? L"drl" : L"gbr", path))
            return;
        out.push_back({U(path), ls[0]});
    } else {
        std::wstring dir;
        if (!ui::pick_folder_dialog(nullptr, L"Export Layers to Folder", dir)) return;
        std::set<std::string> names;
        for (int i : ls) {
            std::string b = base_name(i), nm = b;
            for (int k = 2; names.count(lower(nm)); ++k) {
                size_t dot = b.find_last_of('.');
                nm = (dot == std::string::npos ? b : b.substr(0, dot)) + "_" + std::to_string(k) + (dot == std::string::npos ? "" : b.substr(dot));
            }
            names.insert(lower(nm));
            out.push_back({U(dir) + "\\" + nm, i});
        }
    }
    int ok = 0;
    std::string err, last;
    for (auto& [path, i] : out) {
        const GerberLayerRef& L = doc->prj.layers[i];
        std::string p = path;
        if (lower(p) == lower(L.path)) {   // never overwrite the source file
            size_t dot = p.find_last_of('.');
            p = (dot == std::string::npos ? p : p.substr(0, dot)) + "_export" + (dot == std::string::npos ? "" : p.substr(dot));
        }
        const std::string data = render(i);
        if (write_file(p, data.data(), data.size(), err)) { ++ok; last = p; }
        else gui::status("Export failed: " + p + ": " + err);
    }
    if (ok) toast("Exported " + std::to_string(ok) + " layer(s)" + (ok == 1 ? ": " + last : std::string()), ToastKind::Success, 5.f);
}

// Layer list (R10 §8; R17c / REVIEW_V16 §6). Interaction model:
//   click = select it AND make it the active layer (pads for Add Component / alignment are read from it),
//   Shift+click = range in list order, Ctrl+click = toggle selection (active layer unchanged),
//   double-click = show / hide, eye icon = show / hide, gear icon = layer properties,
//   Space = toggle visibility of the selection, right-click = menu acting on the selected set.
//   Selecting never changes draw order or blending.
// Visuals: active layer = accent row tint + accent bar + bold name; selected = selection fill; hovered = faint.
void GerberWorkspace::draw_layers_panel() {
    const GTheme& T = th();
    lsel_.resize(nlayers(), 0);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(4));
    section_header("LAYERS");
    const float bs = S(22);
    const int nbtn = 7;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - (bs + S(2)) * nbtn - S(6));
    const int prim = primary_layer();
    const bool anysel = std::any_of(lsel_.begin(), lsel_.end(), [](char c) { return c; });
    // compact group / sort dropdown
    {
        static const char* gn[] = {"None", "Side", "Type"};
        static const char* sn[] = {"Draw order", "Name", "Type", "Side"};
        const std::string tip = std::string("Group: ") + gn[std::clamp(group_by_, 0, 2)] + "   Sort: " + sn[std::clamp(sort_col_, 0, 3)] + (sort_desc_ ? " (descending)" : "");
        if (icon_button("##lgrp", Icon::Layers, tip.c_str(), group_by_ != 0 || sort_col_ != 0 || sort_desc_, nlayers() > 0, bs)) ImGui::OpenPopup("##lgrppop");
        if (ImGui::BeginPopup("##lgrppop")) {
            section_header("GROUP BY");
            ImGui::PushID("grp");   // P1-01: "Side" / "Type" exist in both lists
            for (int g = 0; g < 3; ++g) if (ImGui::MenuItem(gn[g], nullptr, group_by_ == g)) group_by_ = g;
            ImGui::PopID();
            ImGui::Separator();
            section_header("SORT BY");
            ImGui::PushID("sort");
            for (int c = 0; c < 4; ++c) if (ImGui::MenuItem(sn[c], nullptr, sort_col_ == c)) sort_col_ = c;
            ImGui::PopID();
            if (ImGui::MenuItem("Descending", nullptr, sort_desc_)) sort_desc_ = !sort_desc_;
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine(0, S(2));
    if (icon_button("##lstu", Icon::ArrowUp, "Show previous layer \xE2\x96\xB2 (PgUp / [)\nMakes the previous layer in the list the active layer and the only visible one", false, nlayers() > 0, bs)) step_layer(-1);
    ImGui::SameLine(0, S(2));
    if (icon_button("##lstd", Icon::ArrowDown, "Show next layer \xE2\x96\xBC (PgDn / ])\nMakes the next layer in the list the active layer and the only visible one", false, nlayers() > 0, bs)) step_layer(+1);
    ImGui::SameLine(0, S(2));
    if (icon_button("##lvis", Icon::Eye, "Visibility: unhide all, hide all / unselected / selection / bottom / top / already added, step", false, nlayers() > 0, bs)) ImGui::OpenPopup("##lvismenu");
    if (ImGui::BeginPopup("##lvismenu")) { visibility_menu_items(); ImGui::EndPopup(); }
    ImGui::SameLine(0, S(2));
    if (icon_button("##lsolo", Icon::Solo, "Show only the selected layer(s) (Ctrl+1..9)", false, prim >= 0, bs)) {
        for (int i = 0; i < nlayers(); ++i) doc->prj.layers[i].visible = anysel ? layer_selected(i) : i == prim;
        doc->changed(ui::CH_LAYERS);
    }
    ImGui::SameLine(0, S(2));
    if (icon_button("##lexp", Icon::Export, "Export layers as Gerber RS-274X / Excellon files: selected, all visible, or chosen layers", false, nlayers() > 0, bs)) ImGui::OpenPopup("##lexpop");
    if (ImGui::BeginPopup("##lexpop")) {
        if (imenu("Export Selected Layers...", Icon::Export, nullptr, false, anysel || prim >= 0)) gui::defer([this]() { export_layers(0); });
        if (imenu("Export All Visible Layers...", Icon::Export)) gui::defer([this]() { export_layers(1); });
        if (imenu("Export Layers (choose)...", Icon::Export)) gui::defer([this]() { export_layers(2); });
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, S(2));
    if (icon_button("##ledit", Icon::ImportGerber, "Load / edit Gerber layers...", false, true, bs)) post_command(ID_IMPORT_GERBER);
    if (nlayers() == 0) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
        ImGui::TextWrapped("  No Gerber layers yet.");
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(8));
        if (button("Load Gerber Layers...", Icon::ImportGerber, true)) post_command(ID_IMPORT_GERBER);
        return;
    }
    // Space: toggle visibility of the selected layers (list hovered / focused, no text input)
    {
        ImGuiIO& io = ImGui::GetIO();
        const bool here = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) || ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        if (here && !io.WantTextInput && !io.KeyCtrl && !ac_.open && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            std::vector<int> t;
            for (int k = 0; k < nlayers(); ++k) if (layer_selected(k)) t.push_back(k);
            if (t.empty() && prim >= 0) t.push_back(prim);
            bool any_hidden = false;
            for (int k : t) any_hidden |= !doc->prj.layers[k].visible;
            for (int k : t) doc->prj.layers[k].visible = any_hidden;
            if (!t.empty()) { doc->changed(ui::CH_LAYERS); gui::status(std::string(any_hidden ? "Shown " : "Hidden ") + std::to_string(t.size()) + " layer(s)."); }
        }
    }
    compute_order();
    const bool draw_order_view = sort_col_ == 0 && !sort_desc_ && group_by_ == 0;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(3), S(3)));
    // selected = selection fill, hovered = faint (distinct from selected), active = accent bar
    ImGui::PushStyleColor(ImGuiCol_Header, with_alpha(T.accent, 0.30f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, with_alpha(T.text, 0.07f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, with_alpha(T.accent, 0.40f));
    int new_hover = -1;
    if (ImGui::BeginTable("##lt", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_ScrollY, ImVec2(0, -ImGui::GetTextLineHeightWithSpacing()))) {
        ImGui::TableSetupScrollFreeze(0, 0);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(22));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(14));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(36));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(20));
        int move_from = -1, move_to = -1;
        static const char* side_groups[] = {"Top", "Inner", "Bottom", "No side (drill / outline)"};
        for (int k = 0; k < (int)disp_order_.size(); ++k) {
            const int i = disp_order_[k];
            if (i >= nlayers()) continue;
            GerberLayerRef& L = doc->prj.layers[i];
            if (group_by_ && (k == 0 || disp_group_[k] != disp_group_[k - 1])) {
                ImGui::TableNextRow(0, S(22));
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, with_alpha(T.accent, 0.10f));
                ImGui::TableSetColumnIndex(2);
                int cnt = 0;
                for (int q = k; q < (int)disp_group_.size() && disp_group_[q] == disp_group_[k]; ++q) ++cnt;
                const std::string gname = group_by_ == 1 ? side_groups[std::clamp(disp_group_[k], 0, 3)] : role_name_u8((LayerRole)disp_group_[k]);
                ImGui::PushFont(g_font_bold);
                ImGui::Text("%s  (%d)", gname.c_str(), cnt);
                ImGui::PopFont();
            }
            ImGui::PushID(i);
            ImGui::TableNextRow(0, S(24));
            if (i == act_) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, with_alpha(T.accent, 0.16f));   // R17c: active layer row
            else if (i == lhover_ && !layer_selected(i)) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, with_alpha(T.text, 0.05f));
            ImGui::TableNextColumn();
            if (i == act_) {   // active layer: accent bar at the row's left edge
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - S(3), p.y - S(3)), ImVec2(p.x, p.y + S(24)), T.accent);
            }
            const std::vector<int> tg = layer_targets(i);
            if (icon_button("##v", L.visible ? Icon::Eye : Icon::EyeOff, i < 9 ? ("Visible (key " + std::to_string(i + 1) + ", Space toggles the selected layers)").c_str() : "Visible (Space toggles the selected layers)", false, true, S(24))) {
                if (ImGui::GetIO().KeyCtrl) for (int q = 0; q < nlayers(); ++q) doc->prj.layers[q].visible = q == i;
                else { const bool v = !L.visible; for (int q : tg) doc->prj.layers[q].visible = v; }
                doc->changed(ui::CH_LAYERS);
            }
            ImGui::TableNextColumn();
            {
                ImVec2 p = ImGui::GetCursorScreenPos();
                const float sz = S(13);
                p.y += S(4);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), col32(L.color | 0xFF000000u, L.visible ? 1.f : 0.35f), S(3));
                dl->AddRect(p, ImVec2(p.x + sz, p.y + sz), negative(i) ? T.text : with_alpha(T.text, 0.25f), S(3), 0, negative(i) ? S(1.5f) : S(1));
                ImGui::InvisibleButton("##c", ImVec2(sz, S(20)));
                if (ImGui::IsItemHovered()) tooltip(tg.size() > 1 ? "Layer colour (click to change; applies to the selected layers)" : "Layer colour (click to change)");
                if (ImGui::IsItemClicked()) ImGui::OpenPopup("##colpop");
                if (ImGui::BeginPopup("##colpop")) {
                    float c[3] = {((L.color >> 16) & 255) / 255.f, ((L.color >> 8) & 255) / 255.f, (L.color & 255) / 255.f};
                    if (ImGui::ColorPicker3("##cp", c, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview)) {
                        const uint32_t col = 0xFF000000u | ((uint32_t)(c[0] * 255 + .5f) << 16) | ((uint32_t)(c[1] * 255 + .5f) << 8) | (uint32_t)(c[2] * 255 + .5f);
                        for (int q : tg) doc->prj.layers[q].color = col;
                        doc->changed(ui::CH_LAYERS);
                    }
                    if (ImGui::Button("By layer type")) { for (int q : tg) doc->prj.layers[q].color = role_color(doc->prj.layers[q].role, q); doc->changed(ui::CH_LAYERS); }
                    ImGui::EndPopup();
                }
            }
            ImGui::TableNextColumn();
            const bool is_sel = layer_selected(i);
            ImGui::PushStyleColor(ImGuiCol_Text, L.visible ? T.text : T.text_dim);
            // R24a: remember whether the bold font was pushed -- a click below makes this row active (set_active), and
            // re-testing i == act_ at the Pop unbalanced the font stack: ImGui error recovery flashed the list red.
            const bool bold_row = i == act_;
            if (bold_row) ImGui::PushFont(g_font_bold);
            std::string label = (i < 9 ? std::to_string(i + 1) + "  " : "   ") + L.name;
            if (ImGui::Selectable(label.c_str(), is_sel, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, S(20)))) {
                ImGuiIO& io = ImGui::GetIO();
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {   // R17c: double-click = show / hide
                    const bool v = !L.visible;
                    for (int q : tg) doc->prj.layers[q].visible = v;
                    doc->changed(ui::CH_LAYERS);
                    gui::status(L.name + (v ? " shown" : " hidden") + std::string(tg.size() > 1 ? " (with the selected layers)" : ""));
                } else if (io.KeyCtrl) {
                    lsel_[i] = !lsel_[i];
                    lsel_anchor_ = i;
                } else if (io.KeyShift && lsel_anchor_ >= 0 && lsel_anchor_ < nlayers()) {
                    int ka = -1;
                    for (int q = 0; q < (int)disp_order_.size(); ++q) if (disp_order_[q] == lsel_anchor_) ka = q;
                    std::fill(lsel_.begin(), lsel_.end(), 0);
                    if (ka < 0) ka = k;
                    for (int q = std::min(k, ka); q <= std::max(k, ka); ++q) lsel_[disp_order_[q]] = 1;
                } else {
                    std::fill(lsel_.begin(), lsel_.end(), 0);
                    lsel_[i] = 1;
                    lsel_anchor_ = i;
                    if (glayer(i)) set_active(i);   // R17c: single click = active layer
                }
                sel_layer_ = i;   // D-code panel shows this layer
            }
            if (ImGui::IsItemHovered()) new_hover = i;
            if (bold_row) ImGui::PopFont();
            ImGui::PopStyleColor();
            if (draw_order_view && ImGui::BeginDragDropSource()) { ImGui::SetDragDropPayload("GLAYER", &i, sizeof i); ImGui::Text("%s", L.name.c_str()); ImGui::EndDragDropSource(); }
            if (draw_order_view && ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GLAYER")) { move_from = *(const int*)pl->Data; move_to = i; }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(0)) {
                const gb::Layer* G = glayer(i);
                std::string tip = L.name + "\n" + role_name_u8(L.role) + ", " + side_badge(L) +
                                  (G ? "\n" + std::to_string(G->objs.size()) + " objects" : std::string("\n(not parsed)")) +
                                  (used_layers_.count(L.name) ? "\nAlready used to add components" : "") +
                                  (i == act_ ? "\nActive layer: Add Component and alignment read their pads from here" : "") +
                                  std::string("\nClick = make active \xC2\xB7 double-click = show / hide \xC2\xB7 Shift range \xC2\xB7 Ctrl toggle selection \xC2\xB7 Space = show / hide selected \xC2\xB7 right-click for options");
                tooltip(tip.c_str());
            }
            if (ImGui::BeginPopupContextItem("##lctx")) {
                if (!layer_selected(i)) { std::fill(lsel_.begin(), lsel_.end(), 0); lsel_[i] = 1; sel_layer_ = i; lsel_anchor_ = i; }
                layer_menu_items(i);
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(1));
                const std::string sb = side_badge(L);
                const ImU32 sc = sb == "BOT" ? IM_COL32(80, 150, 255, 255) : sb == "TOP" ? IM_COL32(230, 120, 60, 255) : IM_COL32(150, 150, 150, 255);
                if (badge("##sb", sb, sc, tg.size() > 1 ? "Side (click to toggle top / bottom for the selected layers)" : "Side (click to toggle top / bottom)", S(32))) {
                    const Side ns = L.side == Side::Bottom ? Side::Top : Side::Bottom;
                    for (int q : tg) doc->prj.layers[q].side = ns;
                    doc->changed(ui::CH_LAYERS);
                }
            }
            ImGui::TableNextColumn();
            if (icon_button("##prop", Icon::Settings, "Layer properties: type, file, format, negative, rename", false, true, S(20))) ImGui::OpenPopup("##lprops");
            if (ImGui::BeginPopup("##lprops")) {
                const gb::Layer* G = glayer(i);
                section_header("LAYER PROPERTIES");
                ImGui::PushFont(g_font_bold);
                ImGui::TextUnformatted(L.name.c_str());
                ImGui::PopFont();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Type");
                ImGui::SameLine(S(70));
                ImGui::SetNextItemWidth(S(170));
                if (ImGui::BeginCombo("##ptype", role_name_u8(L.role).c_str())) {
                    for (size_t r = 0; r < role_names().size(); ++r)
                        if (ImGui::Selectable(U(role_names()[r]).c_str(), (size_t)L.role == r)) { for (int q : tg) { doc->prj.layers[q].role = (LayerRole)r; doc->prj.layers[q].color = role_color((LayerRole)r, q); } doc->changed(ui::CH_LAYERS); }
                    ImGui::EndCombo();
                }
                gb::LayerGuess g = layer_guess(L);
                ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
                if (g.known) ImGui::Text("Detected: %s", g.why.c_str());
                if (G) ImGui::Text("%zu objects \xC2\xB7 %s", G->objs.size(), G->fmt.describe().c_str());
                if (G && !G->warnings.empty()) ImGui::Text("%zu warning(s): %s", G->warnings.size(), G->warnings[0].c_str());
                if (!L.path.empty()) ImGui::TextUnformatted(L.path.c_str());
                ImGui::PopStyleColor();
                bool ng = negative(i);
                if (ImGui::Checkbox("Negative (draw inverted)", &ng)) { for (int q : tg) neg_[doc->prj.layers[q].name] = ng; dirty_ = true; }
                if (button("Rename...", Icon::Text)) { ImGui::CloseCurrentPopup(); rename_layer(i); }
                ImGui::SameLine();
                if (button("Auto-detect", Icon::Wand)) { for (int q : tg) guess_role(doc->prj.layers[q], q); doc->changed(ui::CH_LAYERS); }
                if (tg.size() > 1) { ImGui::PushStyleColor(ImGuiCol_Text, T.text_faint); ImGui::Text("Type / negative apply to the %zu selected layers.", tg.size()); ImGui::PopStyleColor(); }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (move_from >= 0 && move_to >= 0 && move_from != move_to) {
            int s = move_from;
            while (s != move_to) { int t = s < move_to ? s + 1 : s - 1; swap_layers(s, t); s = t; }
            sel_layer_ = move_to;
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    lhover_ = new_hover;
    ImGui::PushStyleColor(ImGuiCol_Text, T.text_faint);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(4));
    ImGui::TextUnformatted(draw_order_view ? "Drag rows to change draw order" : "Grouped / sorted view (draw order unchanged)");
    ImGui::PopStyleColor();
}

void GerberWorkspace::draw_dcode_panel() {
    const GTheme& T = th();
    const int li = sel_layer_ >= 0 ? sel_layer_ : act_;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(4));
    section_header("D-CODES");
    const gb::Layer* L = glayer(li);
    if (!L) { ImGui::TextDisabled("  Select a layer."); hl_layer_ = hl_dcode_ = -1; return; }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, T.text_faint);
    ImGui::TextUnformatted(doc->prj.layers[li].name.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(24));
    if (icon_button("##unused", show_unused_ ? Icon::Eye : Icon::EyeOff, show_unused_ ? "Hide unused D-codes" : "Show unused D-codes", show_unused_, true, S(20)))
        show_unused_ = !show_unused_;
    LayerCache& c = cache(li);
    if (c.src.get() != L) ensure_cache(li);
    int hover = -1;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(3), S(2)));
    if (ImGui::BeginTable("##dt", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, S(20));
        ImGui::TableSetupColumn("D", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, S(40));
        ImGui::TableSetupColumn("Sh", ImGuiTableColumnFlags_WidthFixed, S(24));
        ImGui::TableSetupColumn((Units)app::settings().units == Units::MM ? "Size mm" : "Size mil", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Uses", ImGuiTableColumnFlags_WidthFixed, S(52));
        ImGui::TableHeadersRow();
        std::vector<int> codes;
        for (auto& [d, ap] : L->aps) { (void)ap; if (show_unused_ || c.usage.count(d) || c.usage.empty()) codes.push_back(d); }
        for (auto& [d, n] : c.usage) { (void)n; if (!L->aps.count(d)) codes.push_back(d); }
        if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount > 0) {
            const int col = ss->Specs[0].ColumnIndex;
            const bool asc = ss->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            std::sort(codes.begin(), codes.end(), [&](int a, int b) {
                auto use = [&](int d) { auto it = c.usage.find(d); return it == c.usage.end() ? 0 : it->second; };
                auto sz = [&](int d) { auto it = L->aps.find(d); return it == L->aps.end() ? 0.0 : std::max(it->second.w, it->second.h); };
                auto sh = [&](int d) { auto it = L->aps.find(d); return it == L->aps.end() ? 9 : (int)it->second.shape; };
                bool lt = col == 4 ? use(a) < use(b) : col == 3 ? sz(a) < sz(b) : col == 2 ? sh(a) < sh(b) : a < b;
                bool gt = col == 4 ? use(a) > use(b) : col == 3 ? sz(a) > sz(b) : col == 2 ? sh(a) > sh(b) : a > b;
                if (!lt && !gt) return a < b;
                return asc ? lt : gt;
            });
        }
        auto& hidden = doc->prj.layers[li].hidden_dcodes;
        for (int d : codes) {
            auto it = L->aps.find(d);
            const gb::Aperture* ap = it == L->aps.end() ? nullptr : &it->second;
            auto ut = c.usage.find(d);
            const int uses = ut == c.usage.end() ? 0 : ut->second;
            const bool hid = std::find(hidden.begin(), hidden.end(), d) != hidden.end();
            ImGui::PushID(d);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (icon_button("##h", hid ? Icon::EyeOff : Icon::Eye, hid ? "Hidden (click to show)" : "Visible (click to blank this D-code)", false, true, S(18))) {
                if (hid) hidden.erase(std::remove(hidden.begin(), hidden.end(), d), hidden.end());
                else hidden.push_back(d);
                doc->changed(ui::CH_LAYERS);
            }
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, hid || !uses ? T.text_dim : T.text);
            char lb[16];
            std::snprintf(lb, sizeof lb, "D%d", d);
            ImGui::Selectable(lb, hl_dcode_ == d && hl_layer_ == li, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
            if (ImGui::IsItemHovered()) hover = d;
            if (ImGui::BeginPopupContextItem("##dctx")) {
                if (imenu("Hide This D-code", Icon::EyeOff, nullptr, hid)) { if (hid) hidden.erase(std::remove(hidden.begin(), hidden.end(), d), hidden.end()); else hidden.push_back(d); doc->changed(ui::CH_LAYERS); }
                if (imenu("Hide On All Layers", Icon::EyeOff)) hide_aperture(li, d, true);
                if (imenu("Show Only This D-code", Icon::Solo)) {
                    hidden.clear();
                    for (int e : codes) if (e != d) hidden.push_back(e);
                    doc->changed(ui::CH_LAYERS);
                }
                if (imenu("Show All D-codes", Icon::Eye)) { hidden.clear(); doc->changed(ui::CH_LAYERS); }
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ap ? shape_short(*ap) : "?");
            if (ap && ImGui::IsItemHovered()) tooltip(shape_name(*ap).c_str());
            ImGui::TableNextColumn();
            if (ap) {
                std::string s = ap->shape == gb::Aperture::Circle || ap->shape == gb::Aperture::Polygon ? fmt_ap(ap->w) : fmt_ap(ap->w) + " x " + fmt_ap(ap->h);
                if (ap->hole > 0) s += " h" + fmt_ap(ap->hole);
                if (!ap->defined) s = "undefined";
                ImGui::TextUnformatted(s.c_str());
            } else ImGui::TextDisabled("undefined");
            ImGui::TableNextColumn();
            ImGui::Text("%d", uses);
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    int nl = hover >= 0 ? li : -1;
    if (hover != hl_dcode_ || nl != hl_layer_) { hl_dcode_ = hover; hl_layer_ = nl; dirty_ = true; }
    if (hl_dcode_ >= 0) { dirty_ = true; request_redraw(2); }   // pulsing highlight
}

// R22b (REVIEW_V21 §5/§6): below the layer list = selection modes, component creation, outline editing and
// the shared Confirm. Add Component is the default mode; one model everywhere: select or draw -> Confirm.
void GerberWorkspace::draw_digitize_panel() {
    const GTheme& T = th();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(4));
    section_header("SELECT \xC2\xB7 CREATE \xC2\xB7 CONFIRM");
    const float bw = (ImGui::GetContentRegionAvail().x - S(12)) / 2;
    auto btn = [&](const char* label, Icon ic, bool checked, bool en, const char* tip, ImU32 on_col = 0) {
        const ImU32 c = on_col ? on_col : T.accent;
        ImGui::PushStyleColor(ImGuiCol_Button, checked ? with_alpha(c, 0.45f) : T.raised);
        ImGui::PushStyleColor(ImGuiCol_Border, checked ? c : T.border);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, checked ? S(1.5f) : S(1));
        bool r = button(label, ic, false, ImVec2(bw, S(30)), en);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip(tip);
        return r;
    };
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(4));
    if (btn("Add Component", Icon::Digitize, tool_ == Tool::Digitize, true, "Add Component mode (default): click pads or drag boxes around them - always adds;\nCtrl+click / Ctrl+drag removes. Right-click, Enter or Confirm creates the component."))
        { if (tool_ == Tool::FrameSel) outline_cancel(); else set_tool(Tool::Digitize); }
    ImGui::SameLine(0, S(4));
    if (btn("Delete Component", Icon::Delete, tool_ == Tool::DeleteComp, !doc->prj.comps.empty(), "Delete Component mode: click an element of an assigned component to delete that component.\nEsc or Add Component returns to Add mode.", T.error))
        set_tool(tool_ == Tool::DeleteComp ? Tool::Digitize : Tool::DeleteComp);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(4));
    if (btn("Add Outline", Icon::Board, tool_ == Tool::FrameSel && fsel_.mode == 1, true, "Select existing Gerber line / arc / region elements (Ctrl removes), Confirm, then Board or Circuit"))
        { if (tool_ == Tool::FrameSel && fsel_.mode == 1) outline_cancel(); else outline_enter(otype_board_, 1); }
    ImGui::SameLine(0, S(4));
    if (btn("Draw Outline", Icon::Rect, tool_ == Tool::FrameSel && fsel_.mode == 0, true, "Pick two corners, drag the corner handles (snapped), Confirm, then Board or Circuit"))
        { if (tool_ == Tool::FrameSel && fsel_.mode == 0) outline_cancel(); else outline_enter(otype_board_, 0); }
    // selection state of the active tool
    const bool sel_any = have_res_ || !matches_.empty() || (tool_ == Tool::FrameSel && (!fsel_.picks.empty() || fsel_.corners)) ||
                         (tool_ == Tool::PitchPick && !pt_.sel.empty()) || (tool_ == Tool::Detect && !dt_.sel.empty());
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(4));
    if (button("Clear Selection", Icon::Eraser, false, ImVec2(bw * 2 + S(4), S(28)), sel_any)) {
        if (tool_ == Tool::FrameSel) outline_reset_keep();
        else if (tool_ == Tool::PitchPick) pt_.sel.clear();
        else if (tool_ == Tool::Detect) dt_.sel.clear();
        else clear_buffer();
        gui::status("Selection cleared.");
        refresh();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip("Clear the current selection (Esc)");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(4));
    {
        const bool ok = can_confirm();
        ImGui::PushFont(g_font_bold);
        if (button("Confirm", Icon::Check, true, ImVec2(bw * 2 + S(4), S(36)), ok)) {
            if (tool_ == Tool::Digitize && have_res_) accept();
            else gui::defer([this]() { confirm_action(); });
        }
        ImGui::PopFont();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip((std::string(confirm_label()) + "\nSame as right-click or Enter on the canvas.").c_str());
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(4));
    switch (tool_) {
    case Tool::FrameSel:
        if (fsel_.mode == 1) {
            if (fsel_.picks.empty()) ImGui::TextWrapped("Add Outline: click or box the outline's line / arc / region elements on any visible layer.");
            else ImGui::TextWrapped("%d element(s), %s %s. Confirm, then Board or Circuit.", (int)fsel_.picks.size(), fsel_.outline.size() >= 3 ? "closed contour" : "open chain", fmt_wh(fsel_.box.w(), fsel_.box.h()).c_str());
        } else {
            if (fsel_.corners == 2) ImGui::TextWrapped("Rectangle %s. Drag a corner handle to adjust (snapped), then Confirm.", fmt_wh(fsel_.box.w(), fsel_.box.h()).c_str());
            else ImGui::TextWrapped(fsel_.corners == 1 ? "Pick the opposite corner." : "Draw Outline: pick the first corner (or drag a rectangle).");
        }
        break;
    case Tool::PitchPick: ImGui::TextWrapped("Panel pitch: %d element(s) selected (step %d of 3).", (int)pt_.sel.size(), pt_.stage + 1); break;
    case Tool::Detect: ImGui::TextWrapped(dt_.stage == 0 ? "Detect: %d element(s) in the reference group." : "Detect: %d candidate(s) to review.", dt_.stage == 0 ? (int)dt_.sel.size() : (int)dt_.cands.size()); break;
    case Tool::DeleteComp: ImGui::TextWrapped("Delete mode: click an element of an assigned component."); break;
    default:
        if (act_ >= 0) ImGui::TextWrapped("Pads from: %s (%s), the layer selected in the list", doc->prj.layers[act_].name.c_str(), side_badge(doc->prj.layers[act_]).c_str());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(4));
        if (have_res_) {
            ImGui::TextWrapped("Selected: %zu pads, %s  %s%s", res_.pad_indices.size(), res_.smt ? "SMT" : "TH", res_.name.c_str(),
                               res_.pitch > 0 ? ("  pitch " + fmt_len_u8(res_.pitch)).c_str() : "");
            if (res_.rot != 0) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(4));
                ImGui::TextWrapped("Rotated %.1f\xC2\xB0 (%s)", res_.rot, rot_guess_.why.c_str());
            }
        } else if (!matches_.empty()) {
            ImGui::TextWrapped("%zu matching placements found", matches_.size());
        } else {
            ImGui::TextWrapped("Click pads or drag a box around a component's pads, then right-click (Confirm). Rotated part? Align View to Line (L) first, or just box it: the angle is detected from the pads.");
        }
        break;
    }
    ImGui::PopStyleColor();
}

// ===========================================================================
// Align View to Line (display-only view rotation) + rotated-match review
// ===========================================================================
Vec2 GerberWorkspace::av_snap(Vec2 w, bool free) {
    av_snap_what_.clear();
    if (free) return w;
    Vec2 p;
    if (snap_point(w, 10.0 / std::max(scale_, 1e-6), p, &av_snap_what_)) return p;
    return w;
}

void GerberWorkspace::av_commit() {
    if (!av_has_ || dist(av_a_, av_b_) < 1e-9) return;
    double a = rad2deg(std::atan2(av_b_.y - av_a_.y, av_b_.x - av_a_.x));
    a = std::round(a * 10) / 10;
    // keep the line where it is on screen: rotate about its midpoint
    const Vec2 mid = (av_a_ + av_b_) * 0.5;
    const ImVec2 sm = w2s(mid);
    set_view_rot(-a);
    const Vec2 now = s2w(sm);
    cx_ += mid.x - now.x; cy_ += mid.y - now.y;
    av_has_ = false;
    av_drag_ = -1;
    set_tool(Tool::Digitize);
    if (have_res_) orient_result();
    char b[96];
    std::snprintf(b, sizeof b, "View rotated %.1f\xC2\xB0 (display only; coordinates are unchanged)", view_angle());
    toast(b, ToastKind::Success, 4.f, "Reset", [this] { set_view_rot(0); });
}

// Banner while the view is rotated: "View rotated 45.0 deg - Reset"
void GerberWorkspace::draw_rot_banner(ImDrawList* dl) {
    if (vrot_ == 0) return;
    const GTheme& T = th();
    char b[64];
    std::snprintf(b, sizeof b, "View rotated %.1f\xC2\xB0", view_angle());
    ImGui::PushFont(g_font_bold);
    const ImVec2 ts = ImGui::CalcTextSize(b);
    ImGui::PopFont();
    const ImVec2 rs = ImGui::CalcTextSize("Reset");
    const float pad = S(10), wpx = ts.x + rs.x + pad * 4 + S(22) + S(18);
    const ImVec2 a(pos_.x + (size_.x - wpx) * 0.5f, pos_.y + S(8)), e(a.x + wpx, a.y + S(30));
    dl->AddRectFilled(a, e, col_alpha(T.accent, 0.92f), S(15));
    // rotated compass glyph
    const ImVec2 c(a.x + pad + S(9), (a.y + e.y) * 0.5f);
    const float r = S(8);
    dl->AddCircle(c, r, T.accent_text, 0, S(1.3f));
    const double ang = deg2rad(view_angle());
    // north of the board in the rotated view
    const ImVec2 n0 = w2s(Vec2{cx_, cy_}), n1 = w2s(Vec2{cx_, cy_ + 1.0 / std::max(scale_, 1e-9)});
    float dx = n1.x - n0.x, dy = n1.y - n0.y, l = std::sqrt(dx * dx + dy * dy);
    (void)ang;
    if (l > 0) { dx /= l; dy /= l; }
    dl->AddLine(c, ImVec2(c.x + dx * r, c.y + dy * r), T.accent_text, S(2));
    ImGui::PushFont(g_font_bold);
    dl->AddText(ImVec2(c.x + S(14), a.y + (S(30) - ts.y) * 0.5f), T.accent_text, b);
    ImGui::PopFont();
    // Reset button (pill inside the banner)
    const ImVec2 ra(e.x - rs.x - pad * 2 - S(4), a.y + S(4)), re(e.x - S(4), e.y - S(4));
    ImGui::SetCursorScreenPos(ra);
    ImGui::PushID("##vrotreset");
    const bool click = ImGui::InvisibleButton("##r", ImVec2(re.x - ra.x, re.y - ra.y));
    const bool hv = ImGui::IsItemHovered();
    ImGui::PopID();
    dl->AddRectFilled(ra, re, hv ? T.accent_text : col_alpha(T.accent_text, 0.25f), S(11));
    dl->AddText(ImVec2(ra.x + pad, ra.y + (re.y - ra.y - rs.y) * 0.5f), hv ? T.accent : T.accent_text, "Reset");
    if (hv) tooltip("Show the board unrotated again (the view rotation never changes coordinates)");
    if (click) {
        const ImVec2 sc(pos_.x + size_.x * 0.5f, pos_.y + size_.y * 0.5f);
        const Vec2 keep = s2w(sc);
        set_view_rot(0);
        cx_ = keep.x; cy_ = keep.y;
        if (have_res_) orient_result();
        gui::status("View rotation reset.");
    }
}

// Floating card of the Align View tool: angle field, snap presets, Commit / Cancel
void GerberWorkspace::draw_align_view_card() {
    if (tool_ != Tool::AlignView) return;
    const GTheme& T = th();
    ImGui::SetCursorScreenPos(ImVec2(pos_.x + size_.x - S(300), pos_.y + S(100)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col_alpha(T.panel, 0.96f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    ImGui::BeginChild("##avcard", ImVec2(S(288), 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding);
    icon_draw(ImGui::GetWindowDrawList(), Icon::VerifyAngle, ImGui::GetCursorScreenPos(), ImGui::GetFontSize());
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetFontSize() + S(6));
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted("Align View to Line");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
    ImGui::TextWrapped(av_has_ ? "Drag the handles to adjust (Alt = no snap), or type the angle." : "Drag along a pad row or a pad edge. Snaps to pad centres and edges.");
    ImGui::PopStyleColor();
    double ang = av_has_ ? rad2deg(std::atan2(av_b_.y - av_a_.y, av_b_.x - av_a_.x)) : (vrot_ != 0 ? view_angle() : 0.0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Angle");
    ImGui::SameLine(S(64));
    ImGui::SetNextItemWidth(S(110));
    if (ImGui::InputDouble("\xC2\xB0##avang", &ang, 0.1, 1.0, "%.1f")) {
        if (!av_has_) {   // no line yet: a line through the view centre
            av_a_ = Vec2{cx_, cy_};
            av_b_ = av_a_ + Vec2{1, 0} * (60.0 / std::max(scale_, 1e-9));
            av_has_ = true;
        }
        const double len = std::max(dist(av_a_, av_b_), 1e-6);
        av_b_ = av_a_ + Vec2{std::cos(deg2rad(ang)), std::sin(deg2rad(ang))} * len;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Snap");
    ImGui::SameLine(S(64));
    for (int st : {0, 15, 30, 45, 90}) {
        const bool on = av_step_ == st;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? T.accent : T.raised);
        ImGui::PushStyleColor(ImGuiCol_Text, on ? T.accent_text : T.text);
        char lb[24];
        std::snprintf(lb, sizeof lb, st == 0 ? "free##avs" : "%d\xC2\xB0##avs%d", st, st);
        if (ImGui::SmallButton(lb)) {
            av_step_ = st;
            if (st > 0 && av_has_) {
                double q = std::round(ang / st) * st, len = dist(av_a_, av_b_);
                av_b_ = av_a_ + Vec2{std::cos(deg2rad(q)), std::sin(deg2rad(q))} * len;
            }
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) tooltip(st ? "Snap the line angle to multiples of this step" : "Free angle (snaps to pad centres / edges only)");
        ImGui::SameLine(0, S(3));
    }
    ImGui::NewLine();
    if (av_has_) {
        ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
        ImGui::Text("Length %s", fmt_len_u8(dist(av_a_, av_b_)).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    if (button("Commit", Icon::Check, true, ImVec2(S(84), S(26)), av_has_)) av_commit();
    ImGui::SameLine(0, S(6));
    if (button("Cancel", Icon::Undo, false, ImVec2(S(80), S(26)))) { av_has_ = false; set_tool(Tool::Digitize); }
    ImGui::SameLine(0, S(6));
    if (button("Reset", Icon::Rot180, false, ImVec2(S(76), S(26)), vrot_ != 0)) set_view_rot(0);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ===========================================================================
// UI: overlay
// ===========================================================================
void GerberWorkspace::draw_overlay(ImDrawList* dl) {
    const GTheme& T = th();
    const ui::Palette& P = ui::pal();
    // board / circuit outlines (R17b: distinct colours, halo, labels; see draw_outline_overlay)
    draw_outline_overlay(dl);
    // digitized components
    if (!hide_comps_) {
        // added components: bright cyan outline + ref (distinct from the Gerber pads); just added = thicker + glow
        const ImU32 cbody = IM_COL32(0, 229, 255, 245), csel = col32(P.selection), cpad = col32(P.fiducial, 0.45f);
        const ImVec2 vmin = pos_, vmax(pos_.x + size_.x, pos_.y + size_.y);
        for (const Component& c : doc->prj.comps) {
            if ((c.side == Side::Top && !doc->show_top) || (c.side == Side::Bottom && !doc->show_bottom)) continue;
            if (!plot_visible(c)) continue;   // R14/R16: only Gerber-built plot elements unless "Show Placement Overlay"
            ImVec2 sp = w2s(c.pos);
            if (sp.x < vmin.x - 200 || sp.y < vmin.y - 200 || sp.x > vmax.x + 200 || sp.y > vmax.y + 200) continue;
            const Package* pk = doc->prj.package_of(c);
            const Xform cx = doc->prj.comp_xform(c, 0);
            const ImU32 oc = c.selected ? csel : cbody;
            bool fresh = false;
            for (const Recent& r : recent_) fresh |= r.pkg == c.package && std::fabs(r.pos.x - c.pos.x) < 1e-6 && std::fabs(r.pos.y - c.pos.y) < 1e-6;
            if (c.feature == Feature::Fiducial || !pk) { dl->AddCircle(sp, std::max(3.f, (float)(0.6 * scale_)), oc, 0, S(1.5f)); continue; }
            if (scale_ > 2) {
                for (const Pad& p : pk->pads) {
                    Xform px = cx * Xform::translate(p.pos.x, p.pos.y) * Xform::rotation(p.rot);
                    if (p.shape == PadShape::Round) {
                        dl->AddCircleFilled(w2s(px.apply({0, 0})), (float)(std::min(p.w, p.h) / 2 * scale_), cpad);
                    } else {
                        ImVec2 q[4] = {w2s(px.apply({-p.w / 2, -p.h / 2})), w2s(px.apply({p.w / 2, -p.h / 2})), w2s(px.apply({p.w / 2, p.h / 2})), w2s(px.apply({-p.w / 2, p.h / 2}))};
                        dl->AddConvexPolyFilled(q, 4, cpad);
                    }
                }
            }
            BBox e = pk->extent();
            if (!e.empty()) {
                ImVec2 q[4] = {w2s(cx.apply({e.x0, e.y0})), w2s(cx.apply({e.x1, e.y0})), w2s(cx.apply({e.x1, e.y1})), w2s(cx.apply({e.x0, e.y1}))};
                if (fresh && !c.selected) dl->AddPolyline(q, 4, IM_COL32(0, 229, 255, 70), ImDrawFlags_Closed, S(6));
                dl->AddPolyline(q, 4, oc, ImDrawFlags_Closed, c.selected ? S(2) : fresh ? S(2.2f) : S(1.5f));
                if (!pk->pads.empty() && scale_ > 2) {
                    int p1 = c.pin1_set ? std::clamp(c.pin1_index, 0, (int)pk->pads.size() - 1) : 0;
                    dl->AddCircle(w2s(cx.apply(pk->pads[p1].pos)), S(3.5f), oc, 0, S(1.2f));
                }
            }
            if ((doc->show_labels || show_added_) && !c.ref.empty() && scale_ > 1.5) {
                ImVec2 ts = ImGui::CalcTextSize(c.ref.c_str());
                const ImVec2 tp(sp.x - ts.x / 2, sp.y - ts.y / 2);
                dl->AddRectFilled(ImVec2(tp.x - S(3), tp.y - S(1)), ImVec2(tp.x + ts.x + S(3), tp.y + ts.y + S(1)), IM_COL32(0, 0, 0, 150), S(3));
                dl->AddText(tp, c.selected ? csel : cbody, c.ref.c_str());
            }
        }
    }
    // digitize buffer / matches (active layer coordinates)
    if (show_added_ && !doc->prj.comps.empty() && act_ >= 0 && glayer(act_) && !have_res_ && matches_.empty()) ensure_ps();
    if (act_ >= 0 && glayer(act_) && ps_index_ == act_) {
        const Xform t = lx(act_);
        auto pad_quad = [&](int i, ImU32 fill, ImU32 edge, float th_) {
            const dz::PadFeature& f = ps_.pads[i];
            const BBox& b = f.box;
            ImVec2 q[4] = {w2s(t.apply({b.x0, b.y0})), w2s(t.apply({b.x1, b.y0})), w2s(t.apply({b.x1, b.y1})), w2s(t.apply({b.x0, b.y1}))};
            if (f.oriented) {   // rotated pads: their real (oriented) box
                const Vec2 cs[4] = {{-f.ow / 2, -f.oh / 2}, {f.ow / 2, -f.oh / 2}, {f.ow / 2, f.oh / 2}, {-f.ow / 2, f.oh / 2}};
                for (int k = 0; k < 4; ++k) q[k] = w2s(t.apply(f.c + rotate(cs[k], f.oang)));
            }
            if (fill) dl->AddConvexPolyFilled(q, 4, fill);
            if (edge) dl->AddPolyline(q, 4, edge, ImDrawFlags_Closed, th_);
        };
        // pads already used by added components: dark tint + diagonal hatch (toggle: Show added)
        if (show_added_ && !doc->prj.comps.empty()) {
            const std::vector<char>& um = used_mask();
            const ImU32 tint = IM_COL32(0, 0, 0, 120), hatch = col32(P.label, 0.75f);
            const ImVec2 vmin = pos_, vmax(pos_.x + size_.x, pos_.y + size_.y);
            for (size_t i = 0; i < um.size() && i < ps_.pads.size(); ++i) {
                if (!um[i]) continue;
                const BBox& b = ps_.pads[i].box;
                ImVec2 q[4] = {w2s(t.apply({b.x0, b.y0})), w2s(t.apply({b.x1, b.y0})), w2s(t.apply({b.x1, b.y1})), w2s(t.apply({b.x0, b.y1}))};
                ImVec2 mn = q[0], mx = q[0];
                for (auto& v : q) { mn.x = std::min(mn.x, v.x); mn.y = std::min(mn.y, v.y); mx.x = std::max(mx.x, v.x); mx.y = std::max(mx.y, v.y); }
                if (mx.x < vmin.x || mx.y < vmin.y || mn.x > vmax.x || mn.y > vmax.y) continue;
                dl->AddConvexPolyFilled(q, 4, tint);
                if (mx.x - mn.x > 5 && mx.y - mn.y > 5) {
                    dl->PushClipRect(mn, mx, true);
                    const float stp = S(5), hgt = mx.y - mn.y;
                    for (float x = mn.x - hgt; x < mx.x; x += stp) dl->AddLine(ImVec2(x, mx.y), ImVec2(x + hgt, mn.y), hatch, 1.f);
                    dl->PopClipRect();
                }
            }
        }
        // rotated matches awaiting review: amber fill + dashed frame + confidence tag
        for (size_t m = 0; m < review_.size(); ++m) {
            const bool cur = (int)m == review_cur_;
            for (int i : review_[m].pads)
                if (i >= 0 && i < (int)ps_.pads.size()) pad_quad(i, with_alpha(T.warn, cur ? 0.8f : 0.5f), cur ? IM_COL32(255, 255, 255, 220) : 0, S(1.5f));
            BBox b = review_[m].wbox;
            if (b.empty()) continue;
            b.inflate(0.3);
            const ImVec2 q[4] = {w2s({b.x0, b.y0}), w2s({b.x1, b.y0}), w2s({b.x1, b.y1}), w2s({b.x0, b.y1})};
            for (int k = 0; k < 4; ++k) {
                const ImVec2 a = q[k], e = q[(k + 1) % 4];
                const float len = std::hypot(e.x - a.x, e.y - a.y), dash = S(6);
                for (float u = 0; u < len; u += dash * 2) {
                    const float u1 = std::min(len, u + dash);
                    dl->AddLine(ImVec2(a.x + (e.x - a.x) * u / len, a.y + (e.y - a.y) * u / len), ImVec2(a.x + (e.x - a.x) * u1 / len, a.y + (e.y - a.y) * u1 / len), T.warn, S(1.5f));
                }
            }
            char tg[48];
            std::snprintf(tg, sizeof tg, "? %.1f\xC2\xB0 %d%%", review_[m].rot, (int)std::lround(review_[m].conf * 100));
            ImVec2 tp = q[3];
            for (auto& v : q) if (v.y < tp.y) tp = v;
            dl->AddText(ImVec2(tp.x - S(20), tp.y - S(18)), T.warn, tg);
        }
        for (size_t m = 0; m < matches_.size(); ++m) {
            const bool cur = (int)m == cur_match_;
            for (int i : matches_[m].pads)
                if (i >= 0 && i < (int)ps_.pads.size()) pad_quad(i, col32(cur ? P.crosshair : P.rubber, cur ? 0.75f : 0.5f), 0, 0);
        }
        if (have_res_) {
            for (size_t k = 0; k < res_.pad_indices.size(); ++k) {
                int i = res_.pad_indices[k];
                if (i < 0 || i >= (int)ps_.pads.size()) continue;
                // current selection: bright fill + white halo + accent edge (pin 1 in the origin colour)
                pad_quad(i, col32(P.selection, 0.70f), IM_COL32(255, 255, 255, 230), S(3.5f));
                pad_quad(i, 0, k == 0 ? col32(P.origin) : col32(P.selection), S(1.8f));
            }
            ImVec2 c = w2s(t.apply(res_.centroid));
            if (res_.rot != 0) {
                // rotated component: axis cross along the detected angle + arc label
                const Vec2 lc = res_.centroid;
                const double L = 9.0 / std::max(scale_, 1e-9) * dpi_scale();
                const ImVec2 ax = w2s(t.apply(lc + rotate(Vec2{L * 2.2, 0}, res_.rot))), bx = w2s(t.apply(lc - rotate(Vec2{L, 0}, res_.rot)));
                const ImVec2 ay = w2s(t.apply(lc + rotate(Vec2{0, L}, res_.rot))), by = w2s(t.apply(lc - rotate(Vec2{0, L}, res_.rot)));
                dl->AddLine(bx, ax, col32(P.crosshair), S(1.8f));
                dl->AddLine(by, ay, col32(P.crosshair), S(1.5f));
                dl->AddCircleFilled(ax, S(3), col32(P.crosshair));
                char rb[32];
                std::snprintf(rb, sizeof rb, "%.1f\xC2\xB0", res_.rot);
                dl->AddText(ImVec2(ax.x + S(5), ax.y - S(8)), col32(P.crosshair), rb);
            } else {
                dl->AddLine(ImVec2(c.x - S(9), c.y), ImVec2(c.x + S(9), c.y), col32(P.crosshair), S(1.5f));
                dl->AddLine(ImVec2(c.x, c.y - S(9)), ImVec2(c.x, c.y + S(9)), col32(P.crosshair), S(1.5f));
            }
        }
    }
    if (cur_match_ >= 0 && cur_match_ < (int)matches_.size()) {
        ImVec2 p = w2s(matches_[cur_match_].pos);
        dl->AddLine(ImVec2(p.x - 30, p.y), ImVec2(p.x + 30, p.y), col32(P.crosshair), 1.5f);
        dl->AddLine(ImVec2(p.x, p.y - 30), ImVec2(p.x, p.y + 30), col32(P.crosshair), 1.5f);
    }
    // hovered / pinned feature outline
    auto outline_obj = [&](const Hit& h, ImU32 col, float wpx) {
        const gb::Layer* L = glayer(h.layer);
        if (!L || h.obj < 0 || h.obj >= (int)L->objs.size()) return;
        const gb::Object& o = L->objs[h.obj];
        const Xform x = lx(h.layer);
        const gb::Aperture* ap = o.kind == gb::Object::Region ? nullptr : gb::find_ap(*L, o.dcode);
        std::vector<ImVec2> pts;
        auto poly = [&](const std::vector<Vec2>& v, bool closed) {
            pts.clear();
            for (auto& p : v) pts.push_back(w2s(x.apply(p)));
            if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), col, closed ? ImDrawFlags_Closed : 0, wpx);
        };
        switch (o.kind) {
        case gb::Object::Flash: if (ap) poly(gb::flash_outline(*ap, o.a, 48), true); break;
        case gb::Object::Draw: case gb::Object::Arc: {
            const double r = pen_half(ap);
            std::vector<Vec2> c = o.kind == gb::Object::Draw ? std::vector<Vec2>{o.a, o.b} : gb::arc_points(o, 96);
            pts.clear();
            for (auto& p : c) pts.push_back(w2s(x.apply(p)));
            const float wp = (float)std::max(2.0 * r * scale_, 1.0);
            dl->AddPolyline(pts.data(), (int)pts.size(), with_alpha(col, 0.35f), 0, wp);
            dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, S(1));
            dl->AddCircleFilled(pts.front(), S(3), col);
            dl->AddCircleFilled(pts.back(), S(3), col);
            break;
        }
        case gb::Object::Region: poly(o.poly, true); break;
        }
    };
    if (tool_ == Tool::Query || tool_ == Tool::Clearance || tool_ == Tool::AlignSrc || tool_ == Tool::AlignDst) {
        if (hover_hit_.layer >= 0) outline_obj(hover_hit_, T.accent, S(1.5f));
    }
    if (tool_ == Tool::Query && have_pin_) outline_obj(pinned_hit_, col32(P.crosshair), S(2.5f));
    if (tool_ == Tool::Clearance && clr_a_.layer >= 0) outline_obj(clr_a_, col32(P.crosshair), S(2.5f));
    auto label_box = [&](ImVec2 at, const std::string& s) {
        ImVec2 ts = ImGui::CalcTextSize(s.c_str());
        ImVec2 a(at.x + S(10), at.y + S(10)), b(a.x + ts.x + S(12), a.y + ts.y + S(8));
        dl->AddRectFilled(a, b, with_alpha(T.panel, 0.92f), S(6));
        dl->AddRect(a, b, T.border, S(6));
        dl->AddText(ImVec2(a.x + S(6), a.y + S(4)), T.text, s.c_str());
    };
    auto meas_text = [&](Vec2 a, Vec2 b) {
        Vec2 d = b - a;
        char ang[32];
        std::snprintf(ang, sizeof ang, "%.2f\xC2\xB0", rad2deg(std::atan2(d.y, d.x)));
        return "dX " + fmt_len_u8(d.x, false) + "   dY " + fmt_len_u8(d.y, false) + "\nDist " + fmt_len_u8(dist(a, b)) + "   " + ang;
    };
    // measure
    if (tool_ == Tool::Measure && meas_has_a_) {
        Vec2 b = meas_done_ ? meas_b_ : (snap_valid_ ? snap_pt_ : mouse_w_);
        ImVec2 sa = w2s(meas_a_), sb = w2s(b);
        dl->AddLine(sa, sb, col32(P.crosshair), S(1.5f));
        dl->AddLine(sa, ImVec2(sb.x, sa.y), with_alpha(col32(P.crosshair), 0.5f), S(1));
        dl->AddLine(ImVec2(sb.x, sa.y), sb, with_alpha(col32(P.crosshair), 0.5f), S(1));
        dl->AddCircle(sa, S(4), col32(P.crosshair), 0, S(1.5f));
        dl->AddCircle(sb, S(4), col32(P.crosshair), 0, S(1.5f));
        label_box(sb, meas_text(meas_a_, b));
    }
    if ((tool_ == Tool::Measure || tool_ == Tool::RelOrigin) && snap_valid_) {
        ImVec2 s = w2s(snap_pt_);
        dl->AddRect(ImVec2(s.x - S(5), s.y - S(5)), ImVec2(s.x + S(5), s.y + S(5)), T.accent, 0, 0, S(1.5f));
        if (!meas_has_a_ || meas_done_) {
            ImVec2 ts = ImGui::CalcTextSize(meas_snap_.c_str());
            dl->AddText(ImVec2(s.x + S(8), s.y - ts.y - S(4)), T.accent, meas_snap_.c_str());
        }
    }
    // clearance
    if (tool_ == Tool::Clearance && clr_done_) {
        ImVec2 a = w2s(clr_p_), b = w2s(clr_q_);
        dl->AddLine(a, b, col32(0xFFFFD040), S(2));
        dl->AddCircleFilled(a, S(3), col32(0xFFFFD040));
        dl->AddCircleFilled(b, S(3), col32(0xFFFFD040));
        label_box(ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), "Clearance " + fmt_len_u8(clr_d_));
    }
    // relative origin
    if (have_rel_) {
        ImVec2 r = w2s(rel_);
        const ImU32 c = col32(P.origin);
        dl->AddCircle(r, S(7), c, 0, S(1.5f));
        dl->AddLine(ImVec2(r.x - S(11), r.y), ImVec2(r.x + S(11), r.y), c, S(1.2f));
        dl->AddLine(ImVec2(r.x, r.y - S(11)), ImVec2(r.x, r.y + S(11)), c, S(1.2f));
    }
    // align source marker
    if (tool_ == Tool::AlignDst && align_layer_ >= 0) {
        ImVec2 a = w2s(align_pt_);
        dl->AddLine(ImVec2(a.x - S(12), a.y), ImVec2(a.x + S(12), a.y), col32(P.crosshair), S(1.5f));
        dl->AddLine(ImVec2(a.x, a.y - S(12)), ImVec2(a.x, a.y + S(12)), col32(P.crosshair), S(1.5f));
        dl->AddLine(a, ImGui::GetIO().MousePos, with_alpha(col32(P.crosshair), 0.5f), S(1));
    }
    // Align View to Line: reference line with handles + angle label
    if (tool_ == Tool::AlignView) {
        if (av_has_) {
            const ImVec2 a = w2s(av_a_), b = w2s(av_b_);
            const ImU32 c = col32(P.crosshair);
            // extend faintly across the canvas so the row direction is easy to judge
            const float dx = b.x - a.x, dy = b.y - a.y, l = std::sqrt(dx * dx + dy * dy);
            if (l > 1) {
                const float ux = dx / l, uy = dy / l, ext = size_.x + size_.y;
                dl->AddLine(ImVec2(a.x - ux * ext, a.y - uy * ext), ImVec2(b.x + ux * ext, b.y + uy * ext), with_alpha(c, 0.25f), 1.f);
            }
            dl->AddLine(a, b, c, S(2));
            for (int k = 0; k < 2; ++k) {
                const ImVec2 h = k ? b : a;
                const bool hot = av_drag_ == k;
                dl->AddCircleFilled(h, S(6), hot ? c : with_alpha(T.panel, 0.9f));
                dl->AddCircle(h, S(6), c, 0, S(2));
            }
            char lb[48];
            std::snprintf(lb, sizeof lb, "%.1f\xC2\xB0", rad2deg(std::atan2(av_b_.y - av_a_.y, av_b_.x - av_a_.x)));
            label_box(ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), lb);
        }
        if (hovered_ && !av_snap_what_.empty() && av_drag_ >= 0) {
            const ImVec2 sp = w2s(av_drag_ == 0 ? av_a_ : av_b_);
            dl->AddText(ImVec2(sp.x + S(10), sp.y + S(8)), T.accent, av_snap_what_.c_str());
        }
    }
    draw_r14_overlay(dl);
    draw_wz_overlay(dl);   // R16b
    // rubber band
    if (drag_ == Drag::Band) {
        ImVec2 a = down_s_, b = ImGui::GetIO().MousePos;
        ImVec2 mn(std::min(a.x, b.x), std::min(a.y, b.y)), mx(std::max(a.x, b.x), std::max(a.y, b.y));
        ImU32 c = tool_ == Tool::CropIn || tool_ == Tool::CropOut ? T.warn : tool_ == Tool::Digitize ? col32(P.selection) : T.accent;
        dl->AddRectFilled(mn, mx, with_alpha(c, 0.12f));
        dl->AddRect(mn, mx, c, 0, 0, S(1.5f));
        if (tool_ == Tool::Digitize || tool_ == Tool::Frame) {
            std::string s = fmt_wh((mx.x - mn.x) / scale_, (mx.y - mn.y) / scale_);
            dl->AddText(ImVec2(mx.x + S(6), mx.y + S(2)), T.text, s.c_str());
        }
    }
    // compare legend
    if (compare_ && cmp_a_ >= 0 && cmp_b_ >= 0 && cmp_a_ < nlayers() && cmp_b_ < nlayers()) {
        std::string a = "only in " + doc->prj.layers[cmp_a_].name, b = "only in " + doc->prj.layers[cmp_b_].name;
        ImVec2 p(pos_.x + S(12), pos_.y + size_.y - S(70));
        p.y -= S(24);
        dl->AddRectFilled(p, ImVec2(p.x + S(280), p.y + S(82)), with_alpha(T.panel, 0.92f), S(8));
        dl->AddRectFilled(ImVec2(p.x + S(10), p.y + S(58)), ImVec2(p.x + S(22), p.y + S(70)), col32(0xFF5A6068), S(2));
        dl->AddText(ImVec2(p.x + S(30), p.y + S(56)), T.text, "in both");
        dl->AddRectFilled(ImVec2(p.x + S(10), p.y + S(10)), ImVec2(p.x + S(22), p.y + S(22)), col32(0xFFFF4040), S(2));
        dl->AddText(ImVec2(p.x + S(30), p.y + S(8)), T.text, a.c_str());
        dl->AddRectFilled(ImVec2(p.x + S(10), p.y + S(34)), ImVec2(p.x + S(22), p.y + S(46)), col32(0xFF30D0FF), S(2));
        dl->AddText(ImVec2(p.x + S(30), p.y + S(32)), T.text, b.c_str());
    }
    // empty state
    if (nlayers() == 0) {
        const char* m = "No Gerber layers. Import Gerber / drill files (folder or .zip work too).";
        ImVec2 ts = ImGui::CalcTextSize(m);
        dl->AddText(ImVec2(pos_.x + (size_.x - ts.x) / 2, pos_.y + size_.y / 2 - S(30)), T.text_dim, m);
    }
}

void GerberWorkspace::draw_info_card(ImDrawList* dl) {
    (void)dl;
    Hit h = have_pin_ ? pinned_hit_ : hover_hit_;
    if (tool_ != Tool::Query || h.layer < 0) return;
    std::string info = object_info(h);
    if (info.empty()) return;
    const GTheme& T = th();
    ImGui::SetCursorScreenPos(ImVec2(pos_.x + size_.x - S(290), pos_.y + S(12)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, with_alpha(T.panel, 0.94f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    ImGui::BeginChild("##ginfo", ImVec2(S(278), 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted(have_pin_ ? "Feature (pinned)" : "Feature");
    ImGui::PopFont();
    ImGui::Separator();
    size_t s = 0;
    while (s < info.size()) {
        size_t e = info.find('\n', s);
        if (e == std::string::npos) e = info.size();
        std::string line = info.substr(s, e - s);
        size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
            ImGui::TextUnformatted(line.substr(0, tab).c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(S(84));
            ImGui::TextWrapped("%s", line.substr(tab + 1).c_str());
        } else ImGui::TextUnformatted(line.c_str());
        s = e + 1;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ViewMate-style crosshair + coordinate readout. The crosshair is docked to the cursor by default
// (Space undocks / docks, C centres it on the selection, arrows nudge it when undocked); the readout
// in the strip under the canvas shows the CROSSHAIR position, ABS or REL (anchor: Shift+Space).
// Inspection only: never changes where pads or centroids are created.
void GerberWorkspace::draw_coord_box(ImDrawList* dl) {
    const GTheme& T = th();
    const ui::Palette& P = ui::pal();
    const Vec2 ch = crosshair();
    dl->PushClipRect(pos_, ImVec2(pos_.x + size_.x, pos_.y + size_.y), true);
    if (crosshair_ && drag_ != Drag::Pan && (!ch_docked_ || hovered_)) {
        ImVec2 m = w2s(ch);
        const ImU32 c = with_alpha(col32(P.crosshair), ch_docked_ ? 0.55f : 0.85f);
        dl->AddLine(ImVec2(pos_.x, m.y), ImVec2(pos_.x + size_.x, m.y), c, ch_docked_ ? 1.f : S(1.2f));
        dl->AddLine(ImVec2(m.x, pos_.y), ImVec2(m.x, pos_.y + size_.y), c, ch_docked_ ? 1.f : S(1.2f));
        if (ch_docked_) dl->AddCircle(m, S(4), col32(P.crosshair), 0, S(1.2f));
        else {
            dl->AddRect(ImVec2(m.x - S(6), m.y - S(6)), ImVec2(m.x + S(6), m.y + S(6)), col32(P.crosshair), 0, 0, S(1.5f));
            dl->AddCircleFilled(m, S(2), col32(P.crosshair));
        }
    }
    if (anchor_on_) {
        ImVec2 a = w2s(anchor_);
        const ImU32 c = col32(P.origin);
        dl->AddCircleFilled(ImVec2(a.x, a.y - S(14)), S(5), c);
        dl->AddLine(ImVec2(a.x, a.y - S(10)), a, c, S(2));
        dl->AddCircle(a, S(3), c, 0, S(1.5f));
        if (coord_mode_ == 1 && (hovered_ || !ch_docked_)) dl->AddLine(a, w2s(ch), with_alpha(c, 0.5f), S(1));
    }
    dl->PopClipRect();

    // ---- readout strip (below the canvas)
    const float sh = S(30);
    ImGui::SetCursorScreenPos(ImVec2(pos_.x, pos_.y + size_.y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, T.panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(4), S(3)));
    ImGui::BeginChild("##gcoord", ImVec2(size_.x, sh), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::GetWindowDrawList()->AddLine(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + size_.x, ImGui::GetWindowPos().y), T.border);
    const int units = coord_units_ >= 0 ? coord_units_ : ((Units)app::settings().units == Units::MM ? 0 : (Units)app::settings().units == Units::Inch ? 1 : 2);
    auto fu = [&](double mm) {
        char b[48];
        if (units == 0) std::snprintf(b, sizeof b, "%10.4f", mm);
        else if (units == 1) std::snprintf(b, sizeof b, "%10.5f", mm / 25.4);
        else std::snprintf(b, sizeof b, "%10.2f", mm / 0.0254);
        return std::string(b);
    };
    const bool rel = coord_mode_ == 1 && anchor_on_;
    auto chip = [&](const char* label, bool on, const char* tip) {
        ImGui::PushStyleColor(ImGuiCol_Button, on ? T.accent : T.raised);
        ImGui::PushStyleColor(ImGuiCol_Text, on ? T.accent_text : T.text_dim);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(6), S(2)));
        bool r = ImGui::SmallButton(label);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        if (tip && ImGui::IsItemHovered()) tooltip(tip);
        ImGui::SameLine();
        return r;
    };
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(1));
    if (chip(ch_docked_ ? "DOCKED" : "UNDOCKED", !ch_docked_, "Crosshair: Space docks / undocks it from the cursor; C centres it on the selection;\narrows nudge it when undocked (Shift = 1 px)"))
        { if (ch_docked_) { ch_w_ = mouse_w_; ch_docked_ = false; } else ch_docked_ = true; }
    ImGui::TextDisabled("|"); ImGui::SameLine();
    if (chip("ABS", !rel, "Absolute coordinates of the crosshair (relative to the virtual origin)")) coord_mode_ = 0;
    if (chip("REL", rel, "Relative to the anchor (Shift+Space sets the anchor at the crosshair)")) {
        coord_mode_ = 1;
        if (!anchor_on_) { anchor_ = crosshair(); anchor_on_ = true; }
    }
    ImGui::TextDisabled("|"); ImGui::SameLine();
    if (chip("mm", units == 0, "Millimetres")) coord_units_ = 0;
    if (chip("in", units == 1, "Inches")) coord_units_ = 1;
    if (chip("mil", units == 2, "Mils")) coord_units_ = 2;
    ImGui::TextDisabled("|"); ImGui::SameLine();
    std::string l;
    if (nlayers() > 0) {
        if (rel) {
            const Vec2 d = ch - anchor_;
            char ang[32];
            std::snprintf(ang, sizeof ang, "%8.3f\xC2\xB0", rad2deg(std::atan2(d.y, d.x)));
            l = "dX " + fu(d.x) + "   dY " + fu(d.y) + "   D " + fu(dist(ch, anchor_)) + "   A " + ang;
        } else {
            const Vec2 o = doc->prj.virtual_origin;
            l = "X " + fu(ch.x - o.x) + "   Y " + fu(ch.y - o.y);
            if (anchor_on_) l += "   D " + fu(dist(ch, anchor_)) + " from anchor";
        }
    }
    ImGui::PushFont(g_font_mono);
    ImGui::TextUnformatted(l.c_str());
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// Canvas right-click menu: actions on the SELECTION / the point under the cursor. Layer operations
// (visibility, colour, type, side, rename, order, align, transform) live in the layer list's menu.
void GerberWorkspace::context_menu() {
    if (!ImGui::BeginPopup("##gctx")) return;
    const bool buf = have_res_ || !matches_.empty();
    const gb::Layer* HL = glayer(ctx_hit_.layer);
    const int li = ctx_hit_.layer >= 0 ? ctx_hit_.layer : act_;
    const int dcode = HL && HL->objs[ctx_hit_.obj].kind != gb::Object::Region ? HL->objs[ctx_hit_.obj].dcode : -1;
    section_header("SELECTION");
    if (imenu("Confirm", Icon::Check, "Enter", false, can_confirm())) gui::defer([this]() { confirm_action(); });
    if (imenu("Clear Selection", Icon::Eraser, "Esc", false, buf || doc->selected_count() > 0)) {
        clear_buffer();
        if (doc->selected_count()) { doc->select_none(); doc->changed(ui::CH_SELECTION); }
    }
    Vec2 sc;
    const bool hs = selection_centroid(sc);
    if (imenu("Zoom to Selection", Icon::ZoomRect, nullptr, false, hs)) {
        BBox b = have_res_ ? pads_world_box(res_.pad_indices) : BBox{};
        if (b.empty()) { b.add(sc); b.inflate(2.0); }
        zoom_to(b, 0.25);
    }
    if (imenu("Centre Crosshair on Selection", Icon::VirtualOrigin, "C", false, hs)) centre_crosshair_on_selection();
    ImGui::Separator();
    section_header("HERE");
    if (imenu("Query Feature Here", Icon::Query, "Q", false, ctx_hit_.layer >= 0)) { set_tool(Tool::Query); pinned_hit_ = ctx_hit_; have_pin_ = true; }
    if (imenu("Centre Crosshair Here", Icon::Pan)) { Vec2 p; ch_w_ = snap_point(ctx_w_, 8 / scale_, p) ? p : ctx_w_; ch_docked_ = false; }
    if (imenu("Measure from Here", Icon::Measure, "D")) {
        Vec2 p;
        set_tool(Tool::Measure);
        meas_a_ = snap_point(ctx_w_, 12 / scale_, p) ? p : ctx_w_;
        meas_has_a_ = true; meas_done_ = false;
    }
    if (imenu(anchor_on_ ? "Move REL Anchor Here" : "Set REL Anchor Here", Icon::Link, "Shift+Space")) {
        Vec2 p; anchor_ = snap_point(ctx_w_, 8 / scale_, p) ? p : ctx_w_; anchor_on_ = true; coord_mode_ = 1;
    }
    if (anchor_on_ && imenu("Remove REL Anchor", Icon::Link)) { anchor_on_ = false; coord_mode_ = 0; }
    if (ctx_hit_.layer >= 0 && ctx_hit_.layer != act_ && imenu(("Make " + doc->prj.layers[ctx_hit_.layer].name + " Active").c_str(), Icon::Digitize)) set_active(ctx_hit_.layer);
    const std::string dl = dcode >= 0 ? " (D" + std::to_string(dcode) + ")" : "";
    if (imenu(("Hide This Aperture" + dl).c_str(), Icon::EyeOff, nullptr, false, dcode >= 0)) hide_aperture(li, dcode, false);
    ImGui::Separator();
    if (imenu("Centre View Here", Icon::Pan, "Middle click")) { cx_ = ctx_w_.x; cy_ = ctx_w_.y; dirty_ = true; }
    if (imenu("Zoom All", Icon::ZoomAll, "Home")) zoom_all();
    if (imenu("Show Added Components", Icon::CompInfo, nullptr, !hide_comps_)) hide_comps_ = !hide_comps_;
    ImGui::EndPopup();
}

// ===========================================================================
// View interface
// ===========================================================================
// ===========================================================================
// R14: Align Plot to Gerber, frames from Gerber, panel pitch / rotated instance (specs/PANELIZE.md)
// ===========================================================================

// R17a (REVIEW_V16 §1): the reference is a placement instance; with an unresolved package it is its centroid.
std::vector<Vec2> GerberWorkspace::pa_cad_pads() const {
    std::vector<Vec2> v;
    if (!doc || pa_.ref < 0 || pa_.ref >= (int)doc->prj.comps.size()) return v;
    const Component& c = doc->prj.comps[pa_.ref];
    const Package* pk = doc->prj.package_of(c);
    const Xform t = doc->prj.instance_xform(c, pa_.image);
    if (pk && !pk->pads.empty()) for (auto& pd : pk->pads) v.push_back(t.apply(pd.pos));
    else v.push_back(t.apply({0, 0}));
    return v;
}

// placement (any instance) under w: package extent, or a 1.6 mm box for a package without graphics
int GerberWorkspace::pa_hit(Vec2 w, double tol, int& image) const {
    const Project& p = doc->prj;
    int best = -1;
    double best_area = 1e30;
    for (int i = 0; i < (int)p.comps.size(); ++i) {
        const Component& c = p.comps[i];
        const Package* pk = p.package_of(c);
        BBox e = pk && (!pk->pads.empty() || !pk->body.empty()) ? pk->extent() : BBox(-0.8, -0.8, 0.8, 0.8);
        e.inflate(tol);
        for (int k = 0; k < std::max(1, p.instance_count(c)); ++k) {
            if (!e.contains(p.instance_xform(c, k).inverse().apply(w))) continue;
            const double a = e.w() * e.h();
            if (a < best_area) { best_area = a; best = i; image = k; }
        }
    }
    return best;
}

void GerberWorkspace::pa_refit() {
    pa_.ok = false;
    pa_.ghosts.clear();
    // references follow the current placement data (package resolved meanwhile, instance moved ...)
    for (auto& r : pa_.refs) { const auto g = r.ger_pads; r = make_align_ref(doc->prj, r.comp, r.image, g); }
    pa_.ok = solve_alignment(pa_.refs, pa_.rot_mode, pa_.mirror, pa_.fit, &pa_.why);
    if (pa_.ok) pa_.ghosts = preview_placements(doc->prj, pa_.fit.T);
}

void GerberWorkspace::pa_rect(const BBox& w) {
    const int li = act_;
    const gb::Layer* L = glayer(li);
    if (!L) { gui::status("Select a Gerber layer in the list first (its pads are used)."); return; }
    if (pa_.ref < 0 || pa_.ref >= (int)doc->prj.comps.size()) { gui::status("First click the placement whose pads you box (or pick it in the card)."); return; }
    const std::vector<Vec2> quad = band_quad_.empty() ? io::box_quad(w) : band_quad_;
    const auto pads = io::gerber_pad_centres(*L, lx(li), quad, [&](int oi) { return obj_visible(li, oi); });
    if (pads.empty()) { gui::status("No pads of " + doc->prj.layers[li].name + " inside the box (pick the paste or copper layer)."); return; }
    pa_.layer = li;
    AlignRef r = make_align_ref(doc->prj, pa_.ref, pa_.image, pads);
    bool replaced = false;
    for (auto& e : pa_.refs) if (e.comp == r.comp && e.image == r.image) { e = r; replaced = true; }
    if (!replaced) pa_.refs.push_back(r);
    pa_refit();
    const Component& c = doc->prj.comps[pa_.ref];
    gui::status(std::to_string(pads.size()) + " pad(s) of " + doc->prj.layers[li].name + " for " + c.ref +
                (r.cad_pads.empty() ? ": its centroid goes to the centre of the pads" : ": its package pads are matched") +
                ". Check the ghosts, then Apply" + (pa_.refs.size() < 2 ? " (a second reference far away sets the rotation)." : "."));
}

void GerberWorkspace::enter_plot_align() {
    if (tool_ != Tool::PlotAlign) set_tool(Tool::PlotAlign);
    for (auto it = pa_.refs.begin(); it != pa_.refs.end();) it = it->comp < (int)doc->prj.comps.size() ? std::next(it) : pa_.refs.erase(it);
    if (pa_.ref < 0 || pa_.ref >= (int)doc->prj.comps.size()) {
        pa_.ref = -1; pa_.image = 0;
        // default reference: the component with the most pads (easy to box, unambiguous)
        size_t best = 0;
        for (size_t i = 0; i < doc->prj.comps.size(); ++i) {
            const Component& c = doc->prj.comps[i];
            if (c.feature != Feature::Component) continue;
            const Package* pk = doc->prj.package_of(c);
            const size_t n = pk ? pk->pads.size() : 0;
            if (n > best || pa_.ref < 0) { best = n; pa_.ref = (int)i; }
        }
    }
    pa_refit();   // R17a: the view is kept (R16 zoomed out to the minimum scale: margin 3.0 => negative fit)
    refresh();
}

// R17a: optional step before aligning: rotate / mirror all Gerber layers about their centre
void GerberWorkspace::pa_global(double rot, bool mirror) {
    BBox b;
    for (int i = 0; i < nlayers(); ++i) b.add(layer_world_box(i));
    if (b.empty()) return;
    doc->checkpoint(mirror ? "Mirror all Gerbers" : "Rotate all Gerbers");
    transform_gerbers(doc->prj, rot, mirror, b.center());
    pa_reset();   // boxed pads moved with the Gerbers
    doc->changed(ui::CH_LAYERS | ui::CH_GEOMETRY);
    clear_buffer();
    refresh();
    undo_toast(std::string(mirror ? "All Gerbers mirrored (X)" : "All Gerbers rotated " + digitize::fmt_mm(rot, 0) + "\xC2\xB0") + " about their centre  \xC2\xB7  Ctrl+Z undoes");
}

void GerberWorkspace::pa_apply() {
    if (!pa_.ok) { gui::status(pa_.why.empty() ? "Nothing to apply: pick a placement and box its pads first." : pa_.why, ToastKind::Warning); return; }
    Project& p = doc->prj;
    const AlignFit f = pa_.fit;
    if (pa_.dir == 0) {
        doc->checkpoint("Move Gerbers to Plot");
        const Xform M = f.T.inverse();
        int n = 0;
        for (int i = 0; i < nlayers(); ++i) if (pa_.all_layers || i == pa_.layer) { premultiply_layer(p.layers[i], M); ++n; }
        if (pa_.all_layers) move_gerber_built(p, M);   // components / frames built from Gerber follow the Gerbers
        p.dirty = true;
        doc->changed(ui::CH_LAYERS | ui::CH_GEOMETRY);
        // keep the same part of the board on screen: the view follows the Gerbers
        const Vec2 c = M.apply({cx_, cy_});
        cx_ = c.x; cy_ = c.y;
        char b[200];
        std::snprintf(b, sizeof b, "Moved %d Gerber layer(s) onto the plot: offset %.4f, %.4f mm, rotation %.4f\xC2\xB0%s  \xC2\xB7  Ctrl+Z undoes",
                      n, M.tx, M.ty, -f.rot, f.mirror ? ", mirrored" : "");
        undo_toast(b);
    } else {
        doc->checkpoint("Move Plot to Gerbers");
        move_plot(p, f.T);
        doc->changed(ui::CH_GEOMETRY);
        char b[200];
        std::snprintf(b, sizeof b, "Moved the plot onto the Gerbers: offset %.4f, %.4f mm, rotation %.4f\xC2\xB0%s  \xC2\xB7  Ctrl+Z undoes",
                      f.T.tx, f.T.ty, f.rot, f.mirror ? ", mirrored" : "");
        undo_toast(b);
    }
    ++pa_.applied;
    pa_reset();   // the tool stays open: adjust / repeat with new references, or Undo
    p.plot_aligned = true;   // R16b: the package acquisition wizard builds relative to placements
    clear_buffer();
    refresh();
    if (wz_resume_) { wz_resume_ = false; enter_pkg_wizard(""); }
}

// closed contours from open segments (endpoint chaining, tol mm)
static std::vector<std::vector<Vec2>> chain_closed(const std::vector<std::vector<Vec2>>& segs, double tol) {
    std::vector<std::vector<Vec2>> closed;
    auto key = [&](Vec2 p, int dx, int dy) {
        long long x = (long long)std::floor(p.x / tol) + dx, y = (long long)std::floor(p.y / tol) + dy;
        return (x << 32) ^ (y & 0xFFFFFFFFLL);
    };
    std::unordered_multimap<long long, int> ends;
    for (int s = 0; s < (int)segs.size(); ++s) { ends.emplace(key(segs[s].front(), 0, 0), s * 2); ends.emplace(key(segs[s].back(), 0, 0), s * 2 + 1); }
    std::vector<char> used(segs.size(), 0);
    auto find_next = [&](Vec2 p) -> int {
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                auto r = ends.equal_range(key(p, dx, dy));
                for (auto it = r.first; it != r.second; ++it) {
                    int s = it->second / 2, e = it->second % 2;
                    if (used[s]) continue;
                    if (dist(e ? segs[s].back() : segs[s].front(), p) <= tol) return it->second;
                }
            }
        return -1;
    };
    for (int s = 0; s < (int)segs.size(); ++s) {
        if (used[s]) continue;
        used[s] = 1;
        std::vector<Vec2> chain = segs[s];
        for (int guard = 0; guard < (int)segs.size(); ++guard) {
            if (chain.size() > 2 && dist(chain.back(), chain.front()) <= tol) break;
            int nx = find_next(chain.back());
            if (nx < 0) break;
            int ns = nx / 2;
            used[ns] = 1;
            std::vector<Vec2> add = segs[ns];
            if (nx % 2) std::reverse(add.begin(), add.end());
            chain.insert(chain.end(), add.begin() + 1, add.end());
        }
        if (chain.size() > 3 && dist(chain.back(), chain.front()) <= tol) { chain.pop_back(); closed.push_back(chain); }
    }
    return closed;
}

// ===========================================================================
// R17b (REVIEW_V16 §3/§11, specs/OUTLINES.md): Board Outline / Circuit Outline tools
// ===========================================================================
namespace {
constexpr ImU32 kBoardOutlineCol = IM_COL32(255, 40, 200, 255);    // magenta: unlike every layer role colour
constexpr ImU32 kCircuitOutlineCol = IM_COL32(150, 255, 40, 255);  // lime
constexpr ImU32 kInferredCol = IM_COL32(0x40, 0xC0, 0x57, 255);    // R22 (REVIEW_V21 §2.1): inferred = dashed + hatched green, as the plot view
ImU32 outline_col(bool board) { return board ? kBoardOutlineCol : kCircuitOutlineCol; }
bool outline_elem(const gb::Object& o) {
    return (o.kind == gb::Object::Draw && dist(o.a, o.b) > 1e-6) || o.kind == gb::Object::Arc || (o.kind == gb::Object::Region && o.poly.size() >= 3);
}
}

// legacy entry (R14 hook / test): box-select outline graphics
void GerberWorkspace::frame_sel(const BBox& w) {
    fsel_.mode = 1;
    outline_pick_box(w, false);
    fsel_.pending = false;
}

void GerberWorkspace::commit_frame(bool panel, bool outline) {
    fsel_.prefer_board = panel;
    if (!outline) { fsel_.outline.clear(); }
    outline_commit();
}

void GerberWorkspace::outline_enter(bool board, int mode) {
    const int m = mode >= 0 ? mode : fsel_.mode;
    fsel_ = FrameCand{};
    fsel_.prefer_board = board;
    fsel_.mode = m;
    tool_ = Tool::FrameSel;
    pt_ = PitchUi{}; dt_ = DetectUi{}; otype_open_ = false; meas_has_a_ = meas_done_ = false; have_pin_ = false;
    hover_hit_ = Hit{};
    gui::status(std::string(m == 0 ? "Draw Outline: pick two corners (or drag), then drag the corner handles; corners snap to Gerber features (Alt = no snap)." :
                "Add Outline: click / box line, arc and region elements on any visible layer (always adds; Ctrl removes).") +
                " Right-click / Enter / Confirm, then choose Board or Circuit. Esc cancels.");
    refresh();
}

void GerberWorkspace::outline_reset_keep() {
    const bool b = fsel_.prefer_board; const int m = fsel_.mode;
    fsel_ = FrameCand{}; fsel_.prefer_board = b; fsel_.mode = m;
}

void GerberWorkspace::outline_rebuild() {
    fsel_.box = BBox{}; fsel_.outline.clear(); fsel_.n = (int)fsel_.picks.size();
    std::vector<std::vector<Vec2>> open, closed;
    for (const Hit& h : fsel_.picks) {
        const gb::Layer* L = glayer(h.layer);
        if (!L || h.obj < 0 || h.obj >= (int)L->objs.size()) continue;
        bool cl = false;
        auto pl = gmatch::element_polyline(*L, L->objs[h.obj], lx(h.layer), cl);   // centre lines: the board edge is the stroke centre
        if (pl.size() >= 2) (cl ? closed : open).push_back(std::move(pl));
    }
    const gmatch::OutlineBuild ob = gmatch::build_outline(open, closed);   // same function as the commit (preview = result)
    fsel_.box = ob.box;
    if (ob.closed) fsel_.outline = ob.polys[0];
}

void GerberWorkspace::outline_pick_click(Vec2 w, bool remove) {
    Hit h;
    if (!hit_feature(w, h, true)) { gui::status("No Gerber element under the cursor (visible layers)."); return; }
    const gb::Object& o = glayer(h.layer)->objs[h.obj];
    if (!outline_elem(o)) { gui::status("That is a pad / flash: pick line, arc or region elements of the outline."); return; }
    auto it = std::find_if(fsel_.picks.begin(), fsel_.picks.end(), [&](const Hit& q) { return q.layer == h.layer && q.obj == h.obj; });
    if (remove) { if (it != fsel_.picks.end()) fsel_.picks.erase(it); }   // R22b: Ctrl+click removes ...
    else if (it == fsel_.picks.end()) fsel_.picks.push_back(h);          // ... a plain click always adds (never toggles)
    outline_rebuild();
    if (fsel_.picks.empty()) gui::status("No elements picked.");
    else gui::status(std::to_string(fsel_.picks.size()) + " element(s), " + fmt_wh(fsel_.box.w(), fsel_.box.h()) + (fsel_.outline.size() >= 3 ? ", closed contour" : ", open chain") +
                     ": right-click / Enter / Confirm saves the outline.");
    refresh();
}

void GerberWorkspace::outline_pick_box(const BBox& w, bool remove) {
    int added = 0;
    for (int li = 0; li < nlayers(); ++li) {
        const gb::Layer* L = glayer(li);
        if (!L || !doc->prj.layers[li].visible) continue;
        const Xform t = lx(li);
        for (int oi = 0; oi < (int)L->objs.size(); ++oi) {
            const gb::Object& o = L->objs[oi];
            if (o.clear || !obj_visible(li, oi) || !outline_elem(o)) continue;
            const BBox ob = xform_box(t, gb::object_bbox(*L, o));
            if (ob.empty() || !box_inside(ob, w)) continue;
            if (!band_quad_.empty() && !point_in_poly(ob.center(), band_quad_)) continue;
            auto it = std::find_if(fsel_.picks.begin(), fsel_.picks.end(), [&](const Hit& q) { return q.layer == li && q.obj == oi; });
            if (remove) { if (it != fsel_.picks.end()) { fsel_.picks.erase(it); ++added; } }
            else if (it == fsel_.picks.end()) { fsel_.picks.push_back({li, oi}); ++added; }
        }
    }
    outline_rebuild();
    if (!added) gui::status(remove ? "No picked elements inside the box." : "No visible line / arc / region elements entirely inside the box.");
    else gui::status(std::to_string(fsel_.picks.size()) + " element(s), " + fmt_wh(fsel_.box.w(), fsel_.box.h()) + (fsel_.outline.size() >= 3 ? ", closed contour" : ", open chain") +
                     ": right-click / Enter / Confirm saves the outline.");
    refresh();
}

void GerberWorkspace::outline_draw_rect(const BBox& w) {
    // the drag's own corners, snapped to nearby Gerber geometry (Alt = no snap)
    const Vec2 a = down_w_;
    const Vec2 b{std::fabs(a.x - w.x0) < std::fabs(a.x - w.x1) ? w.x1 : w.x0, std::fabs(a.y - w.y0) < std::fabs(a.y - w.y1) ? w.y1 : w.y0};
    const double tol = 12.0 / std::max(scale_, 1e-9);
    Vec2 sa = a, sb = b;
    if (!ImGui::GetIO().KeyAlt) { Vec2 q; if (snap_point(a, tol, q)) sa = q; if (snap_point(b, tol, q)) sb = q; }
    outline_reset_keep();
    fsel_.box.add(sa); fsel_.box.add(sb);
    fsel_.corners = 2;
    if (!outline_ready()) { fsel_.corners = 0; gui::status("Rectangle too small."); return; }
    gui::status("Rectangle " + fmt_wh(fsel_.box.w(), fsel_.box.h()) + ": drag a corner to adjust; right-click / Enter / Confirm saves it, Esc cancels.");
    refresh();
}

// legacy entry (test hooks): commit as the pre-selected type
bool GerberWorkspace::outline_commit() { return outline_commit_as(fsel_.prefer_board); }

void GerberWorkspace::outline_cancel() {
    tool_exit("Outline cancelled (nothing changed).");
}

void GerberWorkspace::draw_outline_overlay(ImDrawList* dl) {
    const Project& p = doc->prj;
    const bool tool = tool_ == Tool::FrameSel;
    auto poly_line = [&](const std::vector<Vec2>& poly, ImU32 col, float wpx, bool dashed) {
        std::vector<ImVec2> pts; for (auto& q : poly) pts.push_back(w2s(q));
        if (pts.size() < 2) return;
        if (!dashed) {
            dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(0, 0, 0, 170), ImDrawFlags_Closed, wpx + S(3));   // halo
            dl->AddPolyline(pts.data(), (int)pts.size(), col, ImDrawFlags_Closed, wpx);
            return;
        }
        // dashed: 10 px on / 7 px off in screen space; the halo is dashed in phase (a solid halo made it read solid)
        for (int pass = 0; pass < 2; ++pass)
            for (size_t i = 0; i < pts.size(); ++i) {
                const ImVec2 a = pts[i], b = pts[(i + 1) % pts.size()];
                const float L = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                if (L < 0.5f) continue;
                for (float t = 0; t < L; t += S(17)) {
                    const float t1 = std::min(L, t + S(10));
                    dl->AddLine(ImVec2(a.x + (b.x - a.x) * t / L, a.y + (b.y - a.y) * t / L), ImVec2(a.x + (b.x - a.x) * t1 / L, a.y + (b.y - a.y) * t1 / L),
                                pass ? col : IM_COL32(0, 0, 0, 170), pass ? wpx : wpx + S(3));
                }
            }
    };
    auto tag = [&](const std::vector<std::vector<Vec2>>& fr, ImU32 col, const std::string& text) {
        BBox b; for (auto& pl : fr) for (auto& q : pl) b.add(q);
        if (b.empty()) return;
        const ImVec2 a = w2s({b.x0, b.y0}), c = w2s({b.x1, b.y1});
        const ImVec2 tl(std::min(a.x, c.x), std::min(a.y, c.y));
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        const ImVec2 q0(tl.x, tl.y - ts.y - S(7)), q1(q0.x + ts.x + S(12), tl.y - S(1));
        dl->AddRectFilled(q0, q1, col, S(4));
        dl->AddText(ImVec2(q0.x + S(6), q0.y + S(3)), IM_COL32(10, 10, 10, 255), text.c_str());
    };
    const bool show = doc->show_frame && (p.board_frame_from_gerber || p.panel_frame_from_gerber || show_all_plot_ || tool_ == Tool::PlotAlign || tool);
    if (show) {
        auto label = [&](bool board, OutlineSource s, const std::vector<std::vector<Vec2>>& fr) {
            BBox b; for (auto& pl : fr) for (auto& q : pl) b.add(q);
            char t[128]; std::snprintf(t, sizeof t, "%s%s  %s", board ? "BOARD OUTLINE" : "CIRCUIT OUTLINE", s == OutlineSource::Inferred ? " (INFERRED)" : "", fmt_wh(b.w(), b.h()).c_str());
            return std::string(t);
        };
        auto hatch = [&](const std::vector<std::vector<Vec2>>& fr) {
            BBox b; for (auto& pl : fr) for (auto& q : pl) b.add(q);
            if (b.empty()) return;
            const ImVec2 a = w2s({b.x0, b.y0}), c = w2s({b.x1, b.y1});
            const ImVec2 lo(std::min(a.x, c.x), std::min(a.y, c.y)), hi(std::max(a.x, c.x), std::max(a.y, c.y));
            dl->PushClipRect(lo, hi, true);
            const float step = S(14), h = hi.y - lo.y;
            for (float x = lo.x - h; x < hi.x; x += step) dl->AddLine(ImVec2(x, hi.y), ImVec2(x + h, lo.y), IM_COL32(0x8C, 0xE9, 0x9A, 70), S(1));
            dl->PopClipRect();
        };
        if (!p.board_frame.empty()) {
            const bool inf = p.board_outline_src == OutlineSource::Inferred;
            if (inf) hatch(p.board_frame);
            const ImU32 c = inf ? kInferredCol : kCircuitOutlineCol;
            for (auto& poly : p.board_frame) poly_line(poly, c, inf ? S(4.f) : S(2.5f), inf);
            if (!(tool && (fsel_.corners == 2 || fsel_.n))) tag(p.board_frame, c, label(false, p.board_outline_src, p.board_frame));
        }
        if (!p.panel_frame.empty()) {
            const bool inf = p.panel_outline_src == OutlineSource::Inferred;
            const ImU32 c = inf ? kInferredCol : kBoardOutlineCol;
            if (inf) hatch(p.panel_frame);
            for (auto& poly : p.panel_frame) poly_line(poly, c, inf ? S(4.5f) : S(3.f), inf);
            if (!(tool && (fsel_.corners == 2 || fsel_.n))) tag(p.panel_frame, c, label(true, p.panel_outline_src, p.panel_frame));
        }
    }
    if (!tool) return;
    const ImU32 col = IM_COL32(0, 220, 255, 255);   // candidate: neutral (Board / Circuit is chosen at Confirm)
    // snap indicator (Draw mode)
    if (fsel_.mode == 0 && snap_valid_) {
        const ImVec2 q = w2s(snap_pt_);
        dl->AddRect(ImVec2(q.x - S(6), q.y - S(6)), ImVec2(q.x + S(6), q.y + S(6)), col, 0, 0, S(2));
        dl->AddText(ImVec2(q.x + S(9), q.y - S(18)), col, meas_snap_.empty() ? "snap" : meas_snap_.c_str());
    }
    if (fsel_.mode == 0 && fsel_.corners == 1) {
        const ImVec2 a = w2s(fsel_.c0), b = w2s(snap_valid_ ? snap_pt_ : mouse_w_);
        dl->AddCircleFilled(a, S(5), col);
        dl->AddRect(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), col, 0, 0, S(1.5f));
    }
    if (fsel_.mode == 1 && hover_hit_.layer >= 0 && glayer(hover_hit_.layer) && hover_hit_.obj < (int)glayer(hover_hit_.layer)->objs.size()) {
        const gb::Object& o = glayer(hover_hit_.layer)->objs[hover_hit_.obj];
        std::vector<Vec2> pts;
        if (o.kind == gb::Object::Draw) pts = {o.a, o.b}; else if (o.kind == gb::Object::Arc) pts = gb::arc_points(o, 72); else pts = o.poly;
        std::vector<ImVec2> sp; const Xform t = lx(hover_hit_.layer); for (auto& q : pts) sp.push_back(w2s(t.apply(q)));
        if (sp.size() >= 2) dl->AddPolyline(sp.data(), (int)sp.size(), (col & 0x00FFFFFF) | 0xA0000000u, o.kind == gb::Object::Region ? ImDrawFlags_Closed : 0, S(3));
    }
    // picked elements: thick highlight in the target colour
    for (const Hit& h : fsel_.picks) {
        const gb::Layer* L = glayer(h.layer);
        if (!L || h.obj >= (int)L->objs.size()) continue;
        const gb::Object& o = L->objs[h.obj];
        const Xform t = lx(h.layer);
        std::vector<Vec2> pts;
        if (o.kind == gb::Object::Draw) pts = {o.a, o.b}; else if (o.kind == gb::Object::Arc) pts = gb::arc_points(o, 72); else pts = o.poly;
        std::vector<ImVec2> sp; for (auto& q : pts) sp.push_back(w2s(t.apply(q)));
        if (sp.size() < 2) continue;
        dl->AddPolyline(sp.data(), (int)sp.size(), IM_COL32(0, 0, 0, 200), o.kind == gb::Object::Region ? ImDrawFlags_Closed : 0, S(7));
        dl->AddPolyline(sp.data(), (int)sp.size(), col, o.kind == gb::Object::Region ? ImDrawFlags_Closed : 0, S(4));
    }
    // candidate: filled tint + boundary + size tag
    if (fsel_.corners == 2 || (fsel_.mode == 1 && fsel_.n)) {
        std::vector<Vec2> poly = fsel_.outline.size() >= 3 ? fsel_.outline
                               : std::vector<Vec2>{{fsel_.box.x0, fsel_.box.y0}, {fsel_.box.x1, fsel_.box.y0}, {fsel_.box.x1, fsel_.box.y1}, {fsel_.box.x0, fsel_.box.y1}};
        const ImVec2 a = w2s({fsel_.box.x0, fsel_.box.y0}), c = w2s({fsel_.box.x1, fsel_.box.y1});
        dl->AddRectFilled(ImVec2(std::min(a.x, c.x), std::min(a.y, c.y)), ImVec2(std::max(a.x, c.x), std::max(a.y, c.y)), (col & 0x00FFFFFF) | 0x26000000u);
        poly_line(poly, col, S(3), fsel_.mode == 1 && fsel_.outline.size() < 3);
        char t[128]; std::snprintf(t, sizeof t, "OUTLINE  %s  %s  (right-click / Enter = Confirm)", fmt_wh(fsel_.box.w(), fsel_.box.h()).c_str(),
                                   fsel_.mode == 0 ? "rectangle" : fsel_.outline.size() >= 3 ? "closed contour" : "open chain");
        tag({poly}, col, t);
        if (fsel_.mode == 0 && fsel_.corners == 2)   // draggable corner handles
            for (Vec2 q : {Vec2{fsel_.box.x0, fsel_.box.y0}, Vec2{fsel_.box.x1, fsel_.box.y0}, Vec2{fsel_.box.x1, fsel_.box.y1}, Vec2{fsel_.box.x0, fsel_.box.y1}}) {
                const ImVec2 h = w2s(q);
                dl->AddRectFilled(ImVec2(h.x - S(6), h.y - S(6)), ImVec2(h.x + S(6), h.y + S(6)), IM_COL32(0, 0, 0, 200), S(2));
                dl->AddRectFilled(ImVec2(h.x - S(4.5f), h.y - S(4.5f)), ImVec2(h.x + S(4.5f), h.y + S(4.5f)), col, S(1.5f));
            }
    }
}

// ===========================================================================
// R22b (REVIEW_V21 §6.2): one interaction model — select or draw -> Confirm (right-click / Enter /
// the Confirm button) for components, outlines, panel pitch, repeated-circuit detection, packages.
// ===========================================================================
bool GerberWorkspace::can_confirm() const {
    switch (tool_) {
    case Tool::Digitize: return have_res_ || !matches_.empty();
    case Tool::FrameSel: return outline_ready();
    case Tool::PitchPick: return pt_.stage < 2 ? !pt_.sel.empty() : pt_.r.ok;
    case Tool::Detect: return dt_.stage == 0 ? dt_.sel.size() >= 2 : std::count(dt_.on.begin(), dt_.on.end(), (char)1) > 0;
    case Tool::PkgWizard: return !wz_.need_align && wz_.comp >= 0 && !wz_.pads.empty();
    default: return false;
    }
}

const char* GerberWorkspace::confirm_label() const {
    switch (tool_) {
    case Tool::FrameSel: return "Confirm outline (then Board or Circuit)";
    case Tool::PitchPick: return pt_.stage == 0 ? "Confirm reference selection" : pt_.stage == 1 ? "Confirm second selection" : "Confirm: create linear array";
    case Tool::Detect: return dt_.stage == 0 ? "Confirm: find repeated circuits" : "Confirm: create nonlinear array";
    case Tool::PkgWizard: return "Confirm: build package";
    default: return "Confirm: create component";
    }
}

void GerberWorkspace::confirm_action() {
    if (!can_confirm()) {
        switch (tool_) {
        case Tool::FrameSel: gui::status(fsel_.mode == 0 ? "Pick two corners first." : "Select the outline's line / arc / region elements first."); break;
        case Tool::PitchPick: gui::status("Box or click the features first."); break;
        case Tool::Detect: gui::status(dt_.stage == 0 ? "Select at least two elements of a distinctive group first." : "Tick at least one candidate."); break;
        default: gui::status("Nothing selected: click pads or drag a box around them first."); break;
        }
        return;
    }
    switch (tool_) {
    case Tool::Digitize: accept(); break;
    case Tool::FrameSel: otype_open_ = true; refresh(); break;
    case Tool::PitchPick: if (pt_.stage < 2) pitch_confirm(); else pitch_apply(); break;
    case Tool::Detect: if (dt_.stage == 0) detect_run(); else detect_apply(); break;
    case Tool::PkgWizard: wz_build(); break;
    default: break;
    }
}

void GerberWorkspace::tool_exit(const std::string& msg) {
    pt_ = PitchUi{};
    dt_ = DetectUi{};
    fsel_ = FrameCand{};
    otype_open_ = false;
    hover_hit_ = Hit{};
    band_quad_.clear();
    drag_ = Drag::None;
    tool_ = Tool::Digitize;
    if (!msg.empty()) gui::status(msg);
    refresh();
}

// small popup after an outline Confirm: Board or Circuit (the only choice the user has to make)
void GerberWorkspace::draw_outline_type_popup() {
    if (otype_open_) { ImGui::OpenPopup("##otype"); otype_open_ = false; }
    if (!ImGui::BeginPopup("##otype")) return;
    if (tool_ != Tool::FrameSel || !outline_ready()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }   // committed / cancelled meanwhile
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted("Save outline as");
    ImGui::PopFont();
    ImGui::TextDisabled("%s  %s", fsel_.mode == 0 ? "Rectangle" : (fsel_.outline.size() >= 3 ? "Closed contour" : "Selected elements"), fmt_wh(fsel_.box.w(), fsel_.box.h()).c_str());
    auto choice = [&](const char* label, bool board, const char* tip) {
        const ImU32 c = outline_col(board);
        ImGui::PushStyleColor(ImGuiCol_Button, col_alpha(c, otype_board_ == board ? 0.85f : 0.35f));
        ImGui::PushStyleColor(ImGuiCol_Text, otype_board_ == board ? IM_COL32(12, 12, 12, 255) : th().text);
        const bool r = ImGui::Button(label, ImVec2(S(150), S(30)));
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) tooltip(tip);
        return r;
    };
    int pick = -1;
    if (choice("Board Outline", true, "Outside edge of the whole PCB / panel on the conveyor (machine board size)")) pick = 1;
    ImGui::SameLine(0, S(6));
    if (choice("Circuit Outline", false, "Boundary of one circuit (the reference circuit; every array instance gets its own transformed copy)")) pick = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_B, false)) pick = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_C, false)) pick = 0;
    if (pick >= 0) {
        otype_board_ = pick == 1;
        ImGui::CloseCurrentPopup();
        const bool b = otype_board_;
        gui::defer([this, b]() { outline_commit_as(b); });
    }
    if (button("Cancel", Icon::None, false, ImVec2(S(80), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Draw Outline: the rectangle's four corners are handles; dragging one snaps it to Gerber features
void GerberWorkspace::outline_draw_handles_input(bool hovered) {
    if (tool_ != Tool::FrameSel || fsel_.mode != 0 || fsel_.corners != 2) { fsel_.drag_corner = -1; return; }
    BBox& b = fsel_.box;
    const Vec2 q[4] = {{b.x0, b.y0}, {b.x1, b.y0}, {b.x1, b.y1}, {b.x0, b.y1}};
    const ImVec2 mp = ImGui::GetIO().MousePos;
    if (hovered && fsel_.drag_corner < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        for (int k = 0; k < 4; ++k) {
            const ImVec2 s = w2s(q[k]);
            if ((s.x - mp.x) * (s.x - mp.x) + (s.y - mp.y) * (s.y - mp.y) <= S(9) * S(9)) { fsel_.drag_corner = k; break; }
        }
    }
    if (fsel_.drag_corner < 0) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const Vec2 m = snap_valid_ && !ImGui::GetIO().KeyAlt ? snap_pt_ : s2w(mp);
        int& k = fsel_.drag_corner;
        if (k == 0 || k == 3) b.x0 = m.x; else b.x1 = m.x;
        if (k == 0 || k == 1) b.y0 = m.y; else b.y1 = m.y;
        if (b.x0 > b.x1) { std::swap(b.x0, b.x1); k = k == 0 ? 1 : k == 1 ? 0 : k == 2 ? 3 : 2; }
        if (b.y0 > b.y1) { std::swap(b.y0, b.y1); k = 3 - k; }
        refresh();
    } else {
        fsel_.drag_corner = -1;
        if (!outline_ready()) { fsel_.corners = 0; gui::status("Rectangle too small: pick two corners again."); }
        else gui::status("Rectangle " + fmt_wh(b.w(), b.h()) + ": drag a corner to adjust, right-click / Enter / Confirm to save it.");
        refresh();
    }
}

bool GerberWorkspace::outline_commit_as(bool board) {
    if (tool_ != Tool::FrameSel || !outline_ready()) { gui::status("Nothing to save yet."); return false; }
    Project& p = doc->prj;
    const OutlineKind k = board ? OutlineKind::Board : OutlineKind::Circuit;
    if (outline_exists(p, k) && !ui::message(nullptr, board ? L"Board Outline" : L"Circuit Outline",
                                             board ? L"Replace the existing board outline?" : L"Replace the existing circuit outline?", true)) {
        gui::status("Outline kept; the selection is still there (Confirm again, or Esc).");
        return false;
    }
    std::string err;
    bool ok = false, contour = false;
    doc->checkpoint(board ? "Set Board Outline" : "Set Circuit Outline");
    if (fsel_.mode == 0) ok = commit_outline_rect(p, k, fsel_.box, OutlineSource::Confirmed, true, &err);
    else {
        std::vector<std::vector<Vec2>> open, closed;
        for (const Hit& h : fsel_.picks) {
            const gb::Layer* L = glayer(h.layer);
            if (!L || h.obj < 0 || h.obj >= (int)L->objs.size()) continue;
            bool cl = false;
            auto pl = gmatch::element_polyline(*L, L->objs[h.obj], lx(h.layer), cl);
            if (pl.size() >= 2) (cl ? closed : open).push_back(std::move(pl));
        }
        const gmatch::OutlineBuild ob = gmatch::build_outline(open, closed);
        contour = ob.closed;
        ok = commit_outline(p, k, ob.polys, OutlineSource::Imported, true, &err);   // exact vector geometry, never rasterised
    }
    if (!ok) { gui::status("Outline not saved: " + (err.empty() ? std::string("no usable geometry") : err), ToastKind::Warning); return false; }
    doc->show_frame = true;
    doc->changed(ui::CH_GEOMETRY);
    BBox b;
    for (auto& pl : board ? p.panel_frame : p.board_frame) for (auto& q : pl) b.add(q);
    std::string msg = std::string(board ? "Board outline set: " : "Circuit outline set: ") + fmt_wh(b.w(), b.h()) + (contour ? " (closed contour from Gerber)" : fsel_.mode == 1 ? " (Gerber elements)" : " (drawn rectangle)");
    auto iss = check_outlines(p);
    if (!iss.empty()) msg += "  \xC2\xB7  check: " + (iss[0].ref.empty() ? iss[0].msg : iss[0].ref + " " + iss[0].msg) + (iss.size() > 1 ? " (+" + std::to_string(iss.size() - 1) + " more)" : "");
    undo_toast(msg);
    tool_exit(msg);   // the tool exits after a commit
    return true;
}

// ---- generic element selection ----------------------------------------------------------------
std::vector<gmatch::ElemRef> GerberWorkspace::elems_in_box(const BBox& w, bool outline_only) const {
    std::vector<gmatch::ElemRef> v;
    for (int li = 0; li < nlayers(); ++li) {
        const gb::Layer* L = glayer(li);
        if (!L || !doc->prj.layers[li].visible) continue;
        const Xform t = lx(li);
        for (int oi = 0; oi < (int)L->objs.size(); ++oi) {
            const gb::Object& o = L->objs[oi];
            if (o.clear || !obj_visible(li, oi) || (outline_only && !gmatch::outline_element(o))) continue;
            const BBox ob = xform_box(t, gb::object_bbox(*L, o));
            if (ob.empty() || !box_inside(ob, w)) continue;
            if (!band_quad_.empty() && !point_in_poly(ob.center(), band_quad_)) continue;
            v.push_back({li, oi});
        }
    }
    return v;
}

std::vector<gmatch::Feature> GerberWorkspace::sel_features(const gmatch::ElemSelection& s) const {
    std::vector<gmatch::Feature> v;
    for (auto& r : s.items) {
        const gb::Layer* L = glayer(r.layer);
        if (!L || r.obj < 0 || r.obj >= (int)L->objs.size()) continue;
        v.push_back(gmatch::object_feature(*L, L->objs[r.obj], lx(r.layer), r.layer, r.obj));
    }
    return v;
}

void GerberWorkspace::draw_esel(ImDrawList* dl, const gmatch::ElemSelection& s, ImU32 col) {
    for (auto& f : sel_features(s)) {
        const ImVec2 a = w2s({f.c.x - f.w / 2, f.c.y - f.h / 2}), b = w2s({f.c.x + f.w / 2, f.c.y + f.h / 2});
        const ImVec2 mn(std::min(a.x, b.x) - S(1), std::min(a.y, b.y) - S(1)), mx(std::max(a.x, b.x) + S(1), std::max(a.y, b.y) + S(1));
        dl->AddRectFilled(mn, mx, (col & 0x00FFFFFF) | 0x50000000u);
        dl->AddRect(mn, mx, col, 0, 0, S(1.6f));
    }
}

// ---- panel pitch: two selections -> pitch -> counts -> linear array ------------------------------
void GerberWorkspace::pitch_confirm() {
    const auto F = sel_features(pt_.sel);
    if (F.empty()) { gui::status("Box or click the features first."); return; }
    const Vec2 c = gmatch::selection_centre(F);
    const BBox fb = gmatch::features_box(F);
    if (pt_.stage == 0) {
        pt_.c0 = c; pt_.b0 = fb; pt_.n0 = (int)F.size();
        pt_.sel.clear();
        pt_.stage = 1;
        static const char* nx[] = {"one column across AND one row up (diagonal)", "one circuit across (same row)", "one circuit up (same column)"};
        gui::status("Reference centre " + fmt_pos_u8(*doc, c) + " from " + std::to_string(F.size()) + " element(s). Now select the same features on the circuit " +
                    nx[std::clamp(pt_.mode, 0, 2)] + ", then Enter.");
        refresh();
        return;
    }
    if ((int)F.size() != pt_.n0) {   // nothing changes: the user fixes the selection (additive / Ctrl) and confirms again
        gui::status(std::to_string(F.size()) + " element(s) selected, the reference had " + std::to_string(pt_.n0) +
                    ": select the SAME features (Ctrl+click removes extra ones).", ToastKind::Warning);
        return;
    }
    const gmatch::PitchResult r = gmatch::pitch_from_centres(pt_.c0, c, (gmatch::PitchMode)std::clamp(pt_.mode, 0, 2));
    if (!r.ok) { gui::status("Pitch not taken: " + r.why + ".", ToastKind::Warning); return; }   // still in step 2, selection kept
    pt_.c1 = c; pt_.b1 = fb; pt_.r = r; pt_.warn = r.why;
    BBox circ = doc->prj.circuit_frame_bbox();
    if (circ.empty()) circ = pt_.b0;
    BBox ger;
    for (int i = 0; i < nlayers(); ++i) if (doc->prj.layers[i].visible) ger.add(layer_world_box(i));
    const GridSuggest g = suggest_grid(circ, ger, r.dx, r.dy);
    pt_.nx = std::max(1, g.nx); pt_.ny = std::max(1, g.ny); pt_.root_c = g.root_col; pt_.root_r = g.root_row;
    if (pt_.mode == 1) { pt_.ny = 1; pt_.root_r = 0; pt_.nx = std::max(2, pt_.nx); }
    if (pt_.mode == 2) { pt_.nx = 1; pt_.root_c = 0; pt_.ny = std::max(2, pt_.ny); }
    if (pt_.mode == 0 && pt_.nx * pt_.ny < 2) { pt_.nx = std::max(2, pt_.nx); pt_.ny = std::max(2, pt_.ny); }
    pt_.sel.clear();
    pt_.stage = 2;
    gui::status("Pitch X " + fmt_len_u8(r.dx) + ", Y " + fmt_len_u8(r.dy) + (r.why.empty() ? "" : "  \xC2\xB7  " + r.why) + ". Check the counts, then Create Linear Array.");
    refresh();
}

void GerberWorkspace::pitch_apply() {
    if (pt_.stage != 2 || !pt_.r.ok) return;
    Project& p = doc->prj;
    const int nx = std::max(1, pt_.nx), ny = std::max(1, pt_.ny);
    if (nx * ny < 2) { gui::status("An array needs at least 2 circuits.", ToastKind::Warning); return; }
    if (has_array(p) && !confirm("Replace Array", "The panel already has " + std::to_string(array_instance_count(p)) + " circuits. Replace the array with " +
                                 std::to_string(nx) + " \xC3\x97 " + std::to_string(ny) + "?", "Replace")) return;
    std::string err;
    doc->checkpoint("Linear Array from Gerber Pitch");
    if (!create_linear_array(p, nx, ny, pt_.mode == 2 ? 0.0 : pt_.r.dx, pt_.mode == 1 ? 0.0 : pt_.r.dy, std::clamp(pt_.root_c, 0, nx - 1), std::clamp(pt_.root_r, 0, ny - 1), &err)) {
        gui::status("Array not created: " + (err.empty() ? std::string("invalid parameters") : err), ToastKind::Warning);   // the tool stays on the counts
        return;
    }
    doc->all_images = true;
    doc->changed(ui::CH_GEOMETRY);
    const std::string msg = "Linear array " + std::to_string(nx) + " \xC3\x97 " + std::to_string(ny) + ", pitch " + fmt_len_u8(pt_.r.dx) + " \xC3\x97 " + fmt_len_u8(pt_.r.dy);
    undo_toast(msg);
    tool_exit(msg + " created.");
}

// ---- repeated circuits: reference group -> candidates (0/90/180/270) -> review -> nonlinear array -
void GerberWorkspace::detect_run() {
    if (dt_.sel.size() < 2) { gui::status("Select at least two elements of a distinctive group (one element has no orientation)."); return; }
    std::set<int> layers;
    for (auto& r : dt_.sel.items) layers.insert(r.layer);
    std::vector<gmatch::Feature> all;
    std::map<std::pair<int, int>, int> at;
    for (int li : layers) {
        const gb::Layer* L = glayer(li);
        if (!L) continue;
        std::vector<char> hide(L->objs.size(), 0);
        for (int oi = 0; oi < (int)L->objs.size(); ++oi) hide[oi] = !obj_visible(li, oi);
        for (auto& f : gmatch::layer_features(*L, lx(li), li, &hide)) { at[{li, f.obj}] = (int)all.size(); all.push_back(f); }
    }
    std::vector<int> ref;
    for (auto& r : dt_.sel.items) { auto it = at.find({r.layer, r.obj}); if (it != at.end()) ref.push_back(it->second); }
    gmatch::MatchOptions o;
    o.tol = std::max(0.001, dt_.tol);
    for (int q = 0; q < 4; ++q) o.rot[q] = dt_.rot[q];
    std::vector<gmatch::Feature> rf;
    for (int i : ref) rf.push_back(all[i]);
    dt_.ref_box = gmatch::features_box(rf);
    dt_.ref_c = gmatch::selection_centre(rf);
    std::string why;
    auto c = gmatch::find_pattern(all, ref, o, &why);
    if (c.empty()) {
        dt_.why = why;
        gui::status("No repeated circuits found: " + why + ". Try a larger tolerance or a different group.", ToastKind::Warning);
        return;   // still selecting; nothing changed
    }
    dt_.cands = std::move(c);
    dt_.on.assign(dt_.cands.size(), 0);
    int amb = 0;
    for (size_t i = 0; i < dt_.cands.size(); ++i) { dt_.on[i] = !dt_.cands[i].ambiguous; amb += dt_.cands[i].ambiguous; }
    dt_.why.clear();
    dt_.stage = 1;
    gui::status(std::to_string(dt_.cands.size()) + " candidate circuit(s) found" + (amb ? " (" + std::to_string(amb) + " ambiguous: the group is symmetric, unticked)" : std::string()) +
                ". Click a candidate to toggle it, then Confirm.", ToastKind::Success);
    BBox all_b = dt_.ref_box;
    for (auto& k : dt_.cands) all_b.add(k.box);
    zoom_to(all_b, 0.08);
    refresh();
}

void GerberWorkspace::detect_apply() {
    if (dt_.stage != 1) return;
    std::vector<ArrayInstance> v;
    for (size_t i = 0; i < dt_.cands.size(); ++i) {
        if (!dt_.on[i]) continue;
        ArrayInstance a;
        a.offset = dt_.cands[i].offset;
        a.rot = dt_.cands[i].rot;
        v.push_back(a);
    }
    if (v.empty()) { gui::status("Tick at least one candidate."); return; }
    Project& p = doc->prj;
    if (has_array(p) && !confirm("Replace Array", "The panel already has " + std::to_string(array_instance_count(p)) + " circuits. Replace the array with the " +
                                 std::to_string(v.size() + 1) + " detected circuits?", "Replace")) return;
    std::string err;
    doc->checkpoint("Nonlinear Array from Detected Circuits");
    if (!create_nonlinear_array(p, v, &err)) {
        gui::status("Array not created: " + (err.empty() ? std::string("invalid instances") : err), ToastKind::Warning);   // review stays open
        return;
    }
    doc->all_images = true;
    doc->changed(ui::CH_GEOMETRY);
    const std::string msg = "Nonlinear array: reference + " + std::to_string(v.size()) + " detected circuit(s)";
    undo_toast(msg);
    tool_exit(msg + " created.");
}

// card for Panel Pitch / Detect Repeated Circuits (top right of the canvas)
void GerberWorkspace::draw_sel_card() {
    if (tool_ != Tool::PitchPick && tool_ != Tool::Detect) return;
    const GTheme& T = th();
    ImGui::SetCursorScreenPos(ImVec2(pos_.x + size_.x - S(352), pos_.y + S(56)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col_alpha(T.panel, 0.96f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    ImGui::BeginChild("##selcard", ImVec2(S(340), 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding);
    icon_draw(ImGui::GetWindowDrawList(), tool_icon(tool_), ImGui::GetCursorScreenPos(), ImGui::GetFontSize());
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetFontSize() + S(6));
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted(tool_ == Tool::PitchPick ? "Panel Pitch" : "Detect Repeated Circuits");
    ImGui::PopFont();
    bool cancel = false;
    if (tool_ == Tool::PitchPick) {
        static const char* modes[] = {"Diagonal (one across + one up)", "One across (single row)", "One up (single column)"};
        ImGui::BeginDisabled(pt_.stage >= 1);
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##ptmode", &pt_.mode, modes, 3);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip("Diagonal needs the circuit one column across and one row up.\nSingle row / column arrays: measure one across or one up only; the other pitch is 0.");
        ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
        static const char* second[] = {"one across AND one up", "one across", "one up"};
        if (pt_.stage == 0) ImGui::TextWrapped("1/3  Box identifiable features on the REFERENCE circuit (Ctrl removes), then Enter.  %d selected.", (int)pt_.sel.size());
        else if (pt_.stage == 1) ImGui::TextWrapped("2/3  Select the SAME %d feature(s) on the circuit %s, then Enter.  %d selected.", pt_.n0, second[std::clamp(pt_.mode, 0, 2)], (int)pt_.sel.size());
        else ImGui::TextWrapped("3/3  Number of circuits, then Create.");
        ImGui::PopStyleColor();
        if (pt_.stage >= 1) ImGui::TextDisabled("Reference centre  %s", fmt_pos_u8(*doc, pt_.c0).c_str());
        if (pt_.stage == 2) {
            ImGui::BeginDisabled(pt_.mode == 2);
            ImGui::Text("X pitch  %s", pt_.mode == 2 ? "-" : fmt_len_u8(pt_.r.dx).c_str());
            ImGui::EndDisabled();
            ImGui::SameLine(S(170));
            ImGui::BeginDisabled(pt_.mode == 1);
            ImGui::Text("Y pitch  %s", pt_.mode == 1 ? "-" : fmt_len_u8(pt_.r.dy).c_str());
            ImGui::EndDisabled();
            if (!pt_.warn.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, T.warn); ImGui::TextWrapped("%s", pt_.warn.c_str()); ImGui::PopStyleColor(); }
            auto num = [&](const char* lb, int& v, bool en, int lo, int hi) {
                ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(lb); ImGui::SameLine(S(116));
                ImGui::BeginDisabled(!en);
                ImGui::SetNextItemWidth(S(90));
                ImGui::InputInt((std::string("##") + lb).c_str(), &v, 1, 1);
                ImGui::EndDisabled();
                v = std::clamp(v, lo, hi);
            };
            num("Circuits in X", pt_.nx, pt_.mode != 2, 1, 200);
            num("Circuits in Y", pt_.ny, pt_.mode != 1, 1, 200);
            num("Reference col", pt_.root_c, pt_.mode != 2 && pt_.nx > 1, 0, std::max(0, pt_.nx - 1));
            num("Reference row", pt_.root_r, pt_.mode != 1 && pt_.ny > 1, 0, std::max(0, pt_.ny - 1));
            ImGui::TextDisabled("Reference cell is 0-based from the lower left.");
        }
        ImGui::Spacing();
        if (button(pt_.stage == 2 ? "Create Linear Array" : "Confirm", Icon::Check, true, ImVec2(S(170), 0), can_confirm())) gui::defer([this]() { confirm_action(); });
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip(pt_.stage == 2 ? "One undo step; the tool closes afterwards" : "Enter / right-click");
        ImGui::SameLine(0, S(6));
        if (pt_.stage > 0 && button("Back", Icon::ArrowLeft, false, ImVec2(0, 0))) {
            if (pt_.stage == 2) { pt_.stage = 1; pt_.r = {}; pt_.warn.clear(); }
            else { const int m = pt_.mode; pt_ = PitchUi{}; pt_.mode = m; }
            pt_.sel.clear();
            refresh();
        }
        ImGui::SameLine(0, S(6));
        if (button("Cancel", Icon::None, false, ImVec2(0, 0))) cancel = true;
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
        if (dt_.stage == 0) ImGui::TextWrapped("Select a distinctive group of pads / elements on the reference circuit (click / box adds, Ctrl removes), then Enter.  %d selected.", (int)dt_.sel.size());
        else ImGui::TextWrapped("Candidates are evidence, not proof: check each one (click it on the canvas or tick it here), then Confirm.");
        ImGui::PopStyleColor();
        if (!doc->prj.comps.empty() && !doc->prj.plot_aligned) {
            ImGui::PushStyleColor(ImGuiCol_Text, T.warn);
            ImGui::TextWrapped("Placements are not aligned to the Gerbers yet: instance offsets are taken in Gerber coordinates (Align Plot to Gerber first).");
            ImGui::PopStyleColor();
        }
        ImGui::BeginDisabled(dt_.stage != 0);
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Tolerance"); ImGui::SameLine(S(84));
        ImGui::SetNextItemWidth(S(80));
        ImGui::InputDouble("mm##dtol", &dt_.tol, 0, 0, "%.3f");
        dt_.tol = std::clamp(dt_.tol, 0.001, 2.0);
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Rotations"); ImGui::SameLine(S(84));
        static const char* rn[] = {"0\xC2\xB0##r0", "90\xC2\xB0##r1", "180\xC2\xB0##r2", "270\xC2\xB0##r3"};
        for (int q = 0; q < 4; ++q) { if (q) ImGui::SameLine(); ImGui::Checkbox(rn[q], &dt_.rot[q]); }
        ImGui::EndDisabled();
        dt_.hover = -1;
        if (dt_.stage == 1) {
            const float rowh = ImGui::GetFrameHeight();
            ImGui::BeginChild("##dtlist", ImVec2(0, std::min<float>(rowh * (float)dt_.cands.size() + S(8), S(220))), ImGuiChildFlags_Border);
            for (int i = 0; i < (int)dt_.cands.size(); ++i) {
                const auto& c = dt_.cands[i];
                ImGui::PushID(i);
                bool on = dt_.on[i] != 0;
                char lb[200];
                std::snprintf(lb, sizeof lb, "Circuit %d  %3.0f\xC2\xB0  at %s  %d/%d%s", i + 2, c.rot, fmt_pos_u8(*doc, c.centre).c_str(), c.matched, c.total, c.ambiguous ? "  symmetric?" : "");
                if (ImGui::Checkbox(lb, &on)) dt_.on[i] = on;
                if (ImGui::IsItemHovered()) {
                    dt_.hover = i;
                    char t[200]; std::snprintf(t, sizeof t, "Offset %s, %s   rotation %.0f\xC2\xB0   residual %.4f mm%s", fmt_len_u8(c.offset.x, false).c_str(), fmt_len_u8(c.offset.y).c_str(), c.rot, c.rms,
                                               c.ambiguous ? "\nThe group also matches here at another rotation (symmetric): pick a less symmetric group if unsure." : "");
                    tooltip(t);
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (ImGui::SmallButton("All")) std::fill(dt_.on.begin(), dt_.on.end(), (char)1);
            ImGui::SameLine();
            if (ImGui::SmallButton("None")) std::fill(dt_.on.begin(), dt_.on.end(), (char)0);
        }
        ImGui::Spacing();
        const int non = (int)std::count(dt_.on.begin(), dt_.on.end(), (char)1);
        const std::string ok = dt_.stage == 0 ? std::string("Find Matches") : "Create Nonlinear Array (" + std::to_string(non + 1) + ")";
        if (button(ok.c_str(), Icon::Check, true, ImVec2(S(210), 0), can_confirm())) gui::defer([this]() { confirm_action(); });
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip(dt_.stage == 0 ? "Enter / right-click: search all visible artwork of the selected layers at the ticked rotations"
                                                                                             : "Enter / right-click: reference circuit + the ticked candidates (one undo step); the tool closes afterwards");
        ImGui::SameLine(0, S(6));
        if (dt_.stage == 1 && button("Back", Icon::ArrowLeft, false, ImVec2(0, 0))) { dt_.stage = 0; dt_.cands.clear(); dt_.on.clear(); refresh(); }
        if (dt_.stage == 1) ImGui::SameLine(0, S(6));
        if (button("Cancel", Icon::None, false, ImVec2(0, 0))) cancel = true;
        if (!dt_.why.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, T.warn); ImGui::TextWrapped("No repeated circuits found: %s.", dt_.why.c_str()); ImGui::PopStyleColor(); }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (cancel) tool_exit(tool_ == Tool::PitchPick ? "Panel pitch cancelled (nothing changed)." : "Detect Repeated Circuits cancelled (nothing changed).");
}

void GerberWorkspace::draw_sel_overlay(ImDrawList* dl) {
    const ImU32 cy = IM_COL32(255, 214, 10, 255), cr = IM_COL32(255, 80, 80, 255), cg = IM_COL32(60, 230, 120, 255), cgr = IM_COL32(150, 150, 150, 200);
    auto cross = [&](Vec2 w, ImU32 c, const char* t) {
        const ImVec2 p = w2s(w);
        dl->AddCircle(p, S(7), c, 16, S(2));
        dl->AddLine(ImVec2(p.x - S(11), p.y), ImVec2(p.x + S(11), p.y), c, S(1.2f));
        dl->AddLine(ImVec2(p.x, p.y - S(11)), ImVec2(p.x, p.y + S(11)), c, S(1.2f));
        if (t) dl->AddText(ImVec2(p.x + S(9), p.y - S(20)), c, t);
    };
    auto boxr = [&](const BBox& b, ImU32 c, float th) {
        if (b.empty()) return;
        const ImVec2 a = w2s({b.x0, b.y0}), d = w2s({b.x1, b.y1});
        dl->AddRect(ImVec2(std::min(a.x, d.x), std::min(a.y, d.y)), ImVec2(std::max(a.x, d.x), std::max(a.y, d.y)), c, S(3), 0, th);
    };
    // hover highlight: the element a click would add / remove
    if (hover_hit_.layer >= 0 && glayer(hover_hit_.layer) && hover_hit_.obj < (int)glayer(hover_hit_.layer)->objs.size() &&
        ((tool_ == Tool::PitchPick && pt_.stage < 2) || (tool_ == Tool::Detect && dt_.stage == 0))) {
        gmatch::ElemSelection h; h.items.push_back({hover_hit_.layer, hover_hit_.obj});
        draw_esel(dl, h, IM_COL32(255, 255, 255, 160));
    }
    if (tool_ == Tool::PitchPick) {
        draw_esel(dl, pt_.sel, pt_.stage == 0 ? cy : cr);
        if (pt_.stage >= 1) { boxr(pt_.b0, cy, S(1.5f)); cross(pt_.c0, cy, "REF"); }
        if (pt_.stage == 2) {
            boxr(pt_.b1, cr, S(1.5f));
            cross(pt_.c1, cr, pt_.mode == 0 ? "+1 / +1" : pt_.mode == 1 ? "+1 across" : "+1 up");
            dl->AddLine(w2s(pt_.c0), w2s(pt_.c1), cr, S(1.5f));
            // grid preview: every cell's reference-selection box
            for (int x = 0; x < pt_.nx; ++x)
                for (int y = 0; y < pt_.ny; ++y) {
                    if (x == pt_.root_c && y == pt_.root_r) continue;
                    const Vec2 d{(x - pt_.root_c) * (pt_.mode == 2 ? 0.0 : pt_.r.dx), (y - pt_.root_r) * (pt_.mode == 1 ? 0.0 : pt_.r.dy)};
                    BBox b = pt_.b0; b.x0 += d.x; b.x1 += d.x; b.y0 += d.y; b.y1 += d.y;
                    boxr(b, (cg & 0x00FFFFFF) | 0xC0000000u, S(1.2f));
                }
        }
    } else if (tool_ == Tool::Detect) {
        draw_esel(dl, dt_.sel, cy);
        if (dt_.stage == 1) {
            boxr(dt_.ref_box, cy, S(2));
            cross(dt_.ref_c, cy, "REF");
            const auto& fr = doc->prj.board_frame;   // circuit outline preview at every ticked candidate
            for (int i = 0; i < (int)dt_.cands.size(); ++i) {
                const auto& c = dt_.cands[i];
                const ImU32 col = !dt_.on[i] ? cgr : c.ambiguous ? IM_COL32(255, 159, 26, 255) : cg;
                BBox b = c.box; b.inflate(0.2);
                boxr(b, col, i == dt_.hover ? S(3.5f) : S(2));
                char t[48]; std::snprintf(t, sizeof t, "%d  %.0f\xC2\xB0%s", i + 2, c.rot, dt_.on[i] ? "" : "  (off)");
                const ImVec2 a = w2s({b.x0, b.y1}), d = w2s({b.x1, b.y0});
                dl->AddText(ImVec2(std::min(a.x, d.x), std::min(a.y, d.y) - S(16)), col, t);
                if (dt_.on[i] && !fr.empty()) {
                    const Xform I = Xform::translate(c.offset.x, c.offset.y) * Xform::rotation(c.rot);
                    for (auto& poly : fr) {
                        std::vector<ImVec2> pts;
                        for (auto& q : poly) pts.push_back(w2s(I.apply(q)));
                        if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), (col & 0x00FFFFFF) | 0x90000000u, ImDrawFlags_Closed, S(1.5f));
                    }
                }
            }
        }
    }
}

// ===========================================================================
// R16b: Gerber-assisted package acquisition wizard (REVIEW_V15 §2; core/package_resolve.h)
// One unresolved package at a time: go to a placement using it, box its pads on the paste / copper
// layer, Build = package relative to the placement centroid and rotation, applied to every placement.
// State (done / skipped / current) lives in Project::pkg_wizard and is saved with the project.
// ===========================================================================
void GerberWorkspace::enter_pkg_wizard(const std::string& start) {
    Project& p = doc->prj;
    ensure_package_entries(p);
    if (tool_ != Tool::PkgWizard) set_tool(Tool::PkgWizard);
    if (!start.empty()) {
        const auto q = wizard_queue(p);
        if (std::find(q.begin(), q.end(), start) != q.end()) { p.pkg_wizard.current = start; p.pkg_wizard.placement = 0; }
    }
    wizard_resume(p);
    wz_.need_align = !p.plot_aligned;
    if (!wz_.need_align) wz_goto();
    refresh();
}

void GerberWorkspace::wz_goto() {
    Project& p = doc->prj;
    wz_.pads.clear();
    wz_.pkg = p.pkg_wizard.current;
    wz_.comp = wz_.pkg.empty() ? -1 : wizard_placement(p, wz_.pkg, p.pkg_wizard.placement);
    if (wz_.comp < 0) { gui::status(wz_.pkg.empty() ? "Every package used by a placement has graphics." : "No placement uses " + wz_.pkg + "."); return; }
    const Component& c = p.comps[wz_.comp];
    BBox b; b.add(c.pos - Vec2{4, 4}); b.add(c.pos + Vec2{4, 4});
    zoom_to(b, 0.05);
    gui::status(wz_.pkg + ": box the pads of " + c.ref + " on the paste or copper layer (drag adds, Ctrl+drag removes), then Build.");
}

void GerberWorkspace::wz_rect(const BBox& w, int mods) {
    if (wz_.need_align || wz_.comp < 0) return;
    std::vector<Vec2> quad = band_quad_;
    if (quad.empty()) quad = io::box_quad(w);
    if (wz_.pads.empty()) { const int li = choose_pad_layer(quad); if (li >= 0 && li != act_) set_active(li); }
    if (!ensure_ps()) { gui::status("Select a paste or copper Gerber layer in the Layers list first."); return; }
    int n = 0;
    for (const Pad& pd : io::padset_board_pads(ps_, lx(act_), quad)) {   // R17a: same function as the tests
        auto same = [&](const Pad& q) { return dist(q.pos, pd.pos) < 0.01; };
        if (mods & 2) { const size_t b0 = wz_.pads.size(); wz_.pads.erase(std::remove_if(wz_.pads.begin(), wz_.pads.end(), same), wz_.pads.end()); n += (int)(b0 - wz_.pads.size()); }
        else if (std::none_of(wz_.pads.begin(), wz_.pads.end(), same)) { wz_.pads.push_back(pd); ++n; }
    }
    gui::status(std::to_string(n) + ((mods & 2) ? " pad(s) removed" : " pad(s) added") + " on " + doc->prj.layers[act_].name + "; " +
                std::to_string(wz_.pads.size()) + " selected for " + wz_.pkg + ".");
}

void GerberWorkspace::wz_build() {
    Project& p = doc->prj;
    if (wz_.comp < 0 || wz_.comp >= (int)p.comps.size() || wz_.pads.empty()) return;
    const Component c = p.comps[wz_.comp];
    BBox pb;
    for (auto& d : wz_.pads) pb.add(d.pos);
    {   // the pads must surround this placement (they are stored relative to its centroid)
        const double off = dist(pb.center(), c.pos);
        if (off > std::max(1.0, 0.5 * std::max(pb.w(), pb.h()))) {
            gui::status("The selected pads are not centred on " + c.ref + " (" + digitize::fmt_mm(off, 2) +
                        " mm away). Box the pads at the yellow cross, or align the plot to the Gerbers first.", ToastKind::Warning);
            return;
        }
    }
    Package pk = package_from_board_pads(wz_.pads, c);
    pk.description = "Built from Gerber pads at " + c.ref;
    const std::string name = wz_.pkg;
    const Vec2 pc = package_pad_centre(pk);   // pad centre vs centroid (package coords)
    doc->checkpoint("Build package " + name + " from Gerber");
    const int n = assign_package_geometry(p, name, pk, "gerber");
    wizard_mark_done(p, name);
    doc->changed(ui::CH_GEOMETRY);
    // R17a: the result stays on screen (the built pads are drawn on every placement); Next moves on
    wz_.built = name;
    wz_.result = name + ": " + std::to_string(pk.pads.size()) + " pads from " + c.ref + ", used by all " + std::to_string(n) + " placement(s). Pad centre " +
                 (std::hypot(pc.x, pc.y) < 0.005 ? std::string("= centroid") : digitize::fmt_mm(pc.x, 3) + ", " + digitize::fmt_mm(pc.y, 3) + " mm from the centroid") + ".";
    wz_.pads.clear();
    undo_toast(name + ": " + std::to_string(pk.pads.size()) + " pads from " + c.ref + ", applied to " + std::to_string(n) + " placement(s)  \xC2\xB7  Ctrl+Z undoes");
    if (p.pkg_wizard.current.empty()) gui::status("All packages resolved. Orientation and polarity markers can be corrected in the Shapes tab.", ToastKind::Success);
    else {   // R22b (REVIEW_V21 §7 step 9): continue with the next unassigned package right away
        wizard_resume(p);
        if (!p.pkg_wizard.current.empty() && p.pkg_wizard.current != name) wz_goto();
    }
}

void GerberWorkspace::draw_wz_overlay(ImDrawList* dl) {
    if (tool_ != Tool::PkgWizard || wz_.need_align) return;
    const GTheme& T = th();
    Project& p = doc->prj;
    // R17a: the package just built, drawn on every placement using it (result immediately visible)
    if (!wz_.built.empty()) {
        auto it = p.packages.find(wz_.built);
        if (it != p.packages.end())
            for (auto& c : p.comps) {
                if (c.package != wz_.built) continue;
                for (int k = 0; k < std::max(1, p.instance_count(c)); ++k) {
                    const Xform t = p.instance_xform(c, k);
                    for (auto& d : it->second.pads) {
                        const ImVec2 s = w2s(t.apply(d.pos));
                        dl->AddCircleFilled(s, S(3.5f), col_alpha(T.ok, 0.85f), 10);
                    }
                }
            }
    }
    if (wz_.comp >= 0 && wz_.comp < (int)p.comps.size()) {
        const Component& c = p.comps[wz_.comp];
        const ImU32 cy = IM_COL32(255, 214, 10, 255);
        const ImVec2 s = w2s(c.pos);
        dl->AddLine(ImVec2(s.x - S(14), s.y), ImVec2(s.x + S(14), s.y), cy, S(1.5f));
        dl->AddLine(ImVec2(s.x, s.y - S(14)), ImVec2(s.x, s.y + S(14)), cy, S(1.5f));
        // rotation arrow: placement 0 deg direction (+X of the package)
        const Vec2 tip = p.comp_xform(c).apply({1.2, 0});
        dl->AddLine(s, w2s(tip), cy, S(2));
        dl->AddText(ImVec2(s.x + S(10), s.y - S(30)), cy, (c.ref + "  " + wz_.pkg + "  rot " + digitize::fmt_mm(c.rot, 1) + "\xC2\xB0" + (c.side == Side::Bottom ? "  bottom" : "")).c_str());
        if (!wz_.pads.empty()) {   // preview of the built package placed back through the placement transform
            const Package pk = package_from_board_pads(wz_.pads, c);
            const Xform t = p.comp_xform(c);
            for (auto& d : pk.pads) {
                const Vec2 q = t.apply(d.pos);
                dl->AddCircle(w2s(q), S(5), T.ok, 14, S(1.6f));
                char b[8]; snprintf(b, sizeof b, "%d", d.pin);
                dl->AddText(w2s(q) + ImVec2(S(5), S(2)), d.pin == 1 ? IM_COL32(255, 80, 80, 255) : T.ok, b);
            }
        }
    }
    for (auto& d : wz_.pads) {
        const ImVec2 s2 = w2s(d.pos);
        dl->AddRect(ImVec2(s2.x - S(4), s2.y - S(4)), ImVec2(s2.x + S(4), s2.y + S(4)), IM_COL32(255, 140, 40, 255), 0, 0, S(1.6f));
    }
}

// movable floating card: the title row is a drag handle; off = persistent offset from the default spot
static bool card_drag_handle(const char* id, ImVec2& off, float w, float h) {
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool act = ImGui::IsItemActive();
    if (act && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) { off.x += ImGui::GetIO().MouseDelta.x; off.y += ImGui::GetIO().MouseDelta.y; }
    if (ImGui::IsItemHovered() || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) off = ImVec2(0, 0);
    ImGui::SetCursorScreenPos(c0);
    return act;
}
static ImVec2 card_pos(ImVec2 base, ImVec2& off, ImVec2 area0, ImVec2 area1, float w) {
    ImVec2 p(base.x + off.x, base.y + off.y);
    p.x = std::clamp(p.x, area0.x - w + 60.f, area1.x - 60.f);   // keep the title reachable
    p.y = std::clamp(p.y, area0.y, area1.y - 30.f);
    off = ImVec2(p.x - base.x, p.y - base.y);
    return p;
}

void GerberWorkspace::draw_wz_card() {
    if (tool_ != Tool::PkgWizard) return;
    const GTheme& T = th();
    Project& p = doc->prj;
    ImGui::SetCursorScreenPos(card_pos(ImVec2(pos_.x + size_.x - S(392), pos_.y + S(56)), wz_.card_off, pos_, pos_ + size_, S(380)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col_alpha(T.panel, 0.96f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    ImGui::BeginChild("##wzcard", ImVec2(S(380), 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding);
    card_drag_handle("##wzdrag", wz_.card_off, ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() + S(2));
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted("Assign Packages from Gerber");
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) tooltip("Drag the title to move this card (double-click: back to its place)");
    const PkgResolveReport rep = package_report(p);
    {
        const int total = (int)rep.entries.size();
        char b[96]; snprintf(b, sizeof b, "%d of %d packages resolved  \xC2\xB7  %d unresolved", rep.resolved, total, rep.unresolved);
        ImGui::ProgressBar(total ? (float)rep.resolved / total : 1.f, ImVec2(-1, S(6)), "");
        ImGui::TextDisabled("%s", b);
        if (ImGui::IsItemHovered()) {
            std::string t;
            for (auto& e : rep.entries) t += (e.resolved ? "OK  " : "\xE2\x80\xA2 ") + e.name + "  (" + std::to_string(e.placements) + " placements)  " + e.detail + "\n";
            tooltip(t.c_str());
        }
    }
    if (rep.unresolved) {
        if (button("Auto-resolve", Icon::Check, false, ImVec2(0, 0))) {
            doc->checkpoint("Auto-resolve packages");
            const PkgResolveReport rr = auto_resolve_packages(p, master_package_library(), true);
            if (rr.changed) doc->changed(ui::CH_GEOMETRY);
            wz_.result = rr.summary();
            gui::status(rr.summary(), rr.unresolved ? ToastKind::Warning : ToastKind::Success);
            wizard_resume(p);
            if (!wz_.need_align) wz_goto();
        }
        tooltip("Same-name library package, a resolved package with an equivalent name (R_0603 = 0603 = RESC1608), or a built-in chip / SOT-23 template. "
                "Only the rest needs Gerber pads.");
    }
    if (!wz_.result.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, T.ok);
        ImGui::TextWrapped("%s", wz_.result.c_str());
        ImGui::PopStyleColor();
    }
    if (wz_.need_align) {
        ImGui::PushStyleColor(ImGuiCol_Text, T.warn);
        ImGui::TextWrapped("The placements are not aligned to the Gerbers yet. Packages are built relative to each placement centroid, "
                           "so align first (Align Plot to Gerber), or confirm they already line up.");
        ImGui::PopStyleColor();
        if (button("Align Plot to Gerber", Icon::Link, true)) { wz_resume_ = true; enter_plot_align(); }
        ImGui::SameLine();
        if (button("They line up", Icon::Check)) { p.plot_aligned = true; p.dirty = true; wz_.need_align = false; wz_goto(); }
        tooltip("Placement crosses sit on the pad groups of the Gerber: no alignment needed");
    } else if (p.pkg_wizard.current.empty() && rep.unresolved == 0) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(T.ok), "Every package used by a placement has graphics.");
        ImGui::TextDisabled("Correct orientation / polarity markers in the Shapes tab.");
    } else {
        if (p.pkg_wizard.current.empty() || wz_.pkg != p.pkg_wizard.current) {
            if (!wz_.built.empty() && button("Next package", Icon::ArrowRight, true)) { wz_.built.clear(); wz_.result.clear(); wizard_resume(p); wz_goto(); }
            else if (wz_.built.empty()) { wizard_resume(p); wz_goto(); }
        }
        const auto q = wizard_queue(p);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##wzpkg", (wz_.pkg + "  (" + std::to_string(placements_using(p, wz_.pkg)) + " placements)").c_str(), ImGuiComboFlags_HeightLarge)) {
            for (auto& n : q) {
                const bool sk = std::find(p.pkg_wizard.skipped.begin(), p.pkg_wizard.skipped.end(), n) != p.pkg_wizard.skipped.end();
                if (ImGui::Selectable((n + (sk ? "   (skipped)" : "")).c_str(), n == wz_.pkg)) { p.pkg_wizard.current = n; p.pkg_wizard.placement = 0; p.dirty = true; wz_.built.clear(); wz_.result.clear(); wz_goto(); }
            }
            ImGui::EndCombo();
        }
        tooltip("Unresolved packages; skipped ones come last (revisit any time)");
        if (wz_.comp >= 0 && wz_.comp < (int)p.comps.size()) {
            const Component& c = p.comps[wz_.comp];
            ImGui::Text("Placement %s  \xC2\xB7  rot %s\xC2\xB0%s", c.ref.c_str(), digitize::fmt_mm(c.rot, 2).c_str(), c.side == Side::Bottom ? "  \xC2\xB7  bottom (un-mirrored)" : "");
        }
        ImGui::TextDisabled("%d pad(s) selected on %s", (int)wz_.pads.size(), act_ >= 0 && act_ < nlayers() ? doc->prj.layers[act_].name.c_str() : "-");
        const int nuse = placements_using(p, wz_.pkg);
        const std::string bl = "Build & use for all " + std::to_string(nuse) + " placements of " + wz_.pkg;
        if (button(bl.c_str(), Icon::Check, true, ImVec2(-1, 0), !wz_.pads.empty() && wz_.comp >= 0)) wz_build();
        tooltip("Creates the package from the selected pads, relative to this placement's centroid and rotation; every placement naming this package uses it");
        if (button("Clear", Icon::Undo, false, ImVec2(0, 0), !wz_.pads.empty())) wz_.pads.clear();
        ImGui::SameLine();
        if (button("Next placement", Icon::ArrowRight)) { ++p.pkg_wizard.placement; wz_goto(); }
        tooltip("Show another placement of the same package (pads easier to box)");
        ImGui::SameLine();
        if (button("Skip", Icon::ArrowDown)) { wizard_skip(p, wz_.pkg); wz_.built.clear(); wz_goto(); }
        tooltip("Leave this package for later; it moves to the end of the list");
        ImGui::SameLine();
        if (button("In Packages", Icon::PackageEditor)) { const std::string n = wz_.pkg; open_package_editor(doc, n); }
        tooltip("Assign a template or library package instead (Shapes tab)");
    }
    if (button("Close", Icon::None)) { set_tool(Tool::Select); gui::status("Package acquisition paused; progress is saved with the project.", ToastKind::Info); }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// Wine screenshot hook (R16b verification): PCBP_TEST_R16B="pkg=NAME;aligned=1;box=x0,y0,x1,y1;build=0|1;log=Z:\path"
void GerberWorkspace::test_r16b() {
    static int stage = -1, frames = 0;
    static std::map<std::string, std::string> kv;
    if (stage == -2) return;
    if (stage == -1) {
        wchar_t buf[1024] = {};
        if (!GetEnvironmentVariableW(L"PCBP_TEST_R16B", buf, 1024)) { stage = -2; return; }
        std::string all = U(buf), item;
        for (size_t i = 0; i <= all.size(); ++i) {
            if (i == all.size() || all[i] == ';') { auto e = item.find('='); if (e != std::string::npos) kv[item.substr(0, e)] = item.substr(e + 1); item.clear(); }
            else item += all[i];
        }
        stage = 0;
    }
    if (loading_ || !glayer(0)) return;
    ++frames;
    request_redraw(2);
    auto log = [&](const std::string& line) {
        if (!kv.count("log")) return;
        if (FILE* f = _wfopen(W(kv["log"]).c_str(), L"a")) { std::fprintf(f, "%s\n", line.c_str()); std::fclose(f); }
    };
    if (stage == 0 && frames > 30) {
        if (kv["aligned"] == "1") doc->prj.plot_aligned = true;
        set_active(0);
        enter_pkg_wizard(kv["pkg"]);
        std::vector<double> b; { std::string c; const std::string& t = kv["box"]; for (size_t i = 0; i <= t.size(); ++i) { if (i == t.size() || t[i] == ',') { if (!c.empty()) b.push_back(std::atof(c.c_str())); c.clear(); } else c += t[i]; } }
        if (b.size() == 4 && !wz_.need_align) { BBox w; w.add({b[0], b[1]}); w.add({b[2], b[3]}); band_quad_.clear(); wz_rect(w, 0); }
        log("wizard pkg=" + wz_.pkg + " comp=" + (wz_.comp >= 0 ? doc->prj.comps[wz_.comp].ref : std::string("-")) + " pads=" + std::to_string(wz_.pads.size()) +
            " need_align=" + std::to_string(wz_.need_align));
        stage = 1; frames = 0;
    }
    if (stage == 1 && frames > 60) {
        if (kv["build"] == "1" && !wz_.pads.empty()) {
            const std::string n = wz_.pkg;
            wz_build();
            const Package& pk = doc->prj.packages[n];
            std::string s = "built " + n + " pads=" + std::to_string(pk.pads.size());
            for (auto& d : pk.pads) { char t[96]; snprintf(t, sizeof t, " [%d %.4f,%.4f %.3fx%.3f]", d.pin, d.pos.x, d.pos.y, d.w, d.h); s += t; }
            log(s + " next=" + doc->prj.pkg_wizard.current);
        }
        stage = -2;
    }
}

// R17a (REVIEW_V16 §1): references, selection centres and the ghost preview of every placement at T
void GerberWorkspace::draw_pa_overlay(ImDrawList* dl) {
    const GTheme& T = th();
    const Project& p = doc->prj;
    const ImU32 cy = IM_COL32(255, 214, 10, 255), cg = IM_COL32(255, 140, 40, 255), cgh = IM_COL32(0, 220, 255, 255);
    auto cross = [&](Vec2 w, float r, ImU32 c, float th) { const ImVec2 s = w2s(w); dl->AddLine(ImVec2(s.x - r, s.y), ImVec2(s.x + r, s.y), c, th); dl->AddLine(ImVec2(s.x, s.y - r), ImVec2(s.x, s.y + r), c, th); };
    // ghosts: where every placement instance goes (pads, or centroid + 0 deg tick); mirrored ones dashed by a dot
    if (pa_.ok && pa_.preview) {
        const ImU32 gc = (cgh & 0x00FFFFFF) | 0xB0000000;
        for (auto& g : pa_.ghosts) {
            const ImVec2 s = w2s(g.pos);
            if (s.x < pos_.x - 50 || s.y < pos_.y - 50 || s.x > pos_.x + size_.x + 50 || s.y > pos_.y + size_.y + 50) continue;
            if (!g.pads.empty()) for (auto& q : g.pads) { const ImVec2 a = w2s(q); dl->AddRect(ImVec2(a.x - S(3), a.y - S(3)), ImVec2(a.x + S(3), a.y + S(3)), gc, 0, 0, S(1.3f)); }
            cross(g.pos, S(5), gc, S(1.3f));
            const double l = std::hypot(g.xdir.x, g.xdir.y);
            if (l > 1e-9) dl->AddLine(s, w2s(g.pos + g.xdir * (1.0 / l)), gc, S(2));   // 0 deg direction (rotation feedback)
            if (g.bottom) dl->AddCircle(s, S(7), gc, 12, S(1));                           // mirrored / bottom side
        }
    }
    // pending reference: yellow
    if (pa_.ref >= 0 && pa_.ref < (int)p.comps.size()) {
        const Component& c = p.comps[pa_.ref];
        for (auto& q : pa_cad_pads()) dl->AddCircle(w2s(q), S(4), cy, 12, S(1.6f));
        const Vec2 o = p.instance_xform(c, pa_.image).apply({0, 0});
        cross(o, S(12), cy, S(1.6f));
        const ImVec2 a = w2s(o);
        const bool panel = p.instance_count(c) > 1;
        dl->AddText(ImVec2(a.x + S(10), a.y - S(26)), cy, ("REF " + c.ref + (panel ? "  circuit " + std::to_string(pa_.image + 1) : std::string())).c_str());
    }
    // committed references: boxed pads (orange), selection centre (green), centroid -> centre
    for (auto& r : pa_.refs) {
        for (auto& q : r.ger_pads) { const ImVec2 s2 = w2s(q); dl->AddRect(ImVec2(s2.x - S(4), s2.y - S(4)), ImVec2(s2.x + S(4), s2.y + S(4)), cg, 0, 0, S(1.6f)); }
        cross(r.ger_centre, S(9), T.ok, S(2));
        dl->AddLine(w2s(r.plot), w2s(r.ger_centre), (T.ok & 0x00FFFFFF) | 0x90000000, S(1.2f));
        if (r.comp >= 0 && r.comp < (int)p.comps.size()) {
            const ImVec2 a = w2s(r.ger_centre);
            dl->AddText(ImVec2(a.x + S(8), a.y + S(4)), T.ok, p.comps[r.comp].ref.c_str());
        }
    }
}

void GerberWorkspace::draw_pa_card() {
    const GTheme& T = th();
    Project& p = doc->prj;
    // v17: the card sizes to its widest fixed row (rotation radios, button row, reference rows), min 360,
    // capped to the view so nothing is cut off.
    const ImGuiStyle& st = ImGui::GetStyle();
    auto tw = [](const char* t) { return ImGui::CalcTextSize(t).x; };
    const float fh = ImGui::GetFrameHeight(), isp = st.ItemInnerSpacing.x, sp = st.ItemSpacing.x;
    auto radio_w = [&](const char* t) { return fh + isp + tw(t); };
    auto btn_w = [&](const char* t, bool ic) { return tw(t) + st.FramePadding.x * 2 + (ic ? std::round(ImGui::GetFontSize() * 1.15f) + S(6) : 0) + S(6); };
    float need = tw("Rotation") + sp;
    for (const char* r : {"0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0", "From refs"}) need += radio_w(r) + sp;
    need = std::max(need, S(90) + btn_w("Reset", true) + btn_w("Undo Apply", true) + btn_w("Close", false) + sp * 3);
    need = std::max(need, radio_w("Move Gerbers to plot") + radio_w("Move plot to Gerbers") + sp);
    for (auto& r : pa_.refs) if (r.comp >= 0 && r.comp < (int)p.comps.size()) {
        const Component& c = p.comps[r.comp];
        need = std::max(need, tw((c.ref + "  " + c.package + "  (circuit 00): 00 pad(s), package pads  \xC2\xB7  off by 0.000 mm").c_str()) + fh + sp);
    }
    const float cw = std::clamp(need + S(24) + S(4), S(360), std::max(S(360), size_.x - S(24)));
    ImGui::SetCursorScreenPos(card_pos(ImVec2(pos_.x + size_.x - cw - S(12), pos_.y + S(56)), pa_.card_off, pos_, pos_ + size_, cw));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col_alpha(T.panel, 0.96f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    ImGui::BeginChild("##pacard", ImVec2(cw, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding);
    card_drag_handle("##padrag", pa_.card_off, ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() + S(2));
    icon_draw(ImGui::GetWindowDrawList(), Icon::Link, ImGui::GetCursorScreenPos(), ImGui::GetFontSize());
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetFontSize() + S(6));
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted("Align Plot to Gerber");
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) tooltip("Drag the title to move this card (double-click: back to its place)");
    ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
    ImGui::TextWrapped("1 Click a placement (or pick it)   2 Box its pads   3 Check the blue ghosts   4 Apply");
    ImGui::PopStyleColor();
    // 1. placement
    ImGui::SetNextItemWidth(-1);
    auto lbl = [&](int i, int img) {
        const Component& c = p.comps[i];
        return c.ref + "  " + c.package + (p.instance_count(c) > 1 ? "  (circuit " + std::to_string(img + 1) + ")" : std::string());
    };
    const std::string cur = pa_.ref >= 0 && pa_.ref < (int)p.comps.size() ? lbl(pa_.ref, pa_.image) : std::string("(click a placement)");
    if (ImGui::BeginCombo("##paref", cur.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < (int)p.comps.size(); ++i) {
            if (p.comps[i].ref.empty()) continue;
            ImGui::PushID(i);
            if (ImGui::Selectable(lbl(i, 0).c_str(), i == pa_.ref)) {
                pa_.ref = i; pa_.image = 0;
                BBox b = p.comp_bbox(p.comps[i]);   // zoom to the reference (a few mm around it)
                b.inflate(4);
                zoom_to(b, 0.1);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    tooltip("The placement whose pads you box next. With a package that has no pads yet its centroid is used: it goes to the centre of the boxed pads");
    // references
    int del = -1;
    for (int k = 0; k < (int)pa_.refs.size(); ++k) {
        const AlignRef& r = pa_.refs[k];
        if (r.comp < 0 || r.comp >= (int)p.comps.size()) continue;
        ImGui::PushID(k);
        char b[160];
        std::snprintf(b, sizeof b, "%s: %d pad(s), %s  \xC2\xB7  off by %.3f mm", lbl(r.comp, r.image).c_str(), (int)r.ger_pads.size(),
                      r.cad_pads.empty() ? "centroid" : "package pads", r.residual);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(r.residual < 0.05 ? T.ok : T.warn), "%s", b);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::GetFrameHeight() + S(4));
        if (ImGui::SmallButton("x")) del = k;
        ImGui::PopID();
    }
    if (del >= 0) { pa_.refs.erase(pa_.refs.begin() + del); pa_refit(); }
    if (pa_.refs.empty()) ImGui::TextDisabled("No pads boxed yet (active layer: %s).", act_ >= 0 && act_ < nlayers() ? p.layers[act_].name.c_str() : "none");
    // rotation / mirror
    ImGui::TextUnformatted("Rotation");
    tooltip("How the Gerbers are turned against the plot. The ghosts show the result live");
    ImGui::SameLine();
    static const char* rn[] = {"0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0", "From refs"};
    for (int i = 0; i < 5; ++i) {
        if (ImGui::RadioButton(rn[i], pa_.rot_mode == i)) { pa_.rot_mode = i; pa_refit(); }
        if (i == 4) tooltip("Rotation measured from two or more references, or from the package pads of one reference (any angle)");
        if (i < 4) ImGui::SameLine();
    }
    if (ImGui::Checkbox("Mirror", &pa_.mirror)) pa_refit();
    tooltip("The Gerbers are mirrored against the plot (e.g. bottom-side data viewed from the bottom)");
    ImGui::SameLine();
    ImGui::Checkbox("Preview", &pa_.preview);
    tooltip("Blue ghosts: every placement (all circuits) where Apply puts it. They should sit on the Gerber pads");
    ImGui::SameLine();
    if (ImGui::SmallButton("Detect")) {
        int best = -1; bool bm = false; double bs = 1e30;
        for (int m = 0; m < 2; ++m) for (int r = 0; r < 4; ++r) {
            std::vector<AlignRef> t = pa_.refs; AlignFit f;
            if (!solve_alignment(t, r, m != 0, f)) continue;
            int bad = 0;   // ghosts that land on no pad of the reference layer
            if (pa_.layer >= 0 && glayer(pa_.layer)) {
                BBox all; for (int i = 0; i < nlayers(); ++i) all.add(layer_world_box(i));
                const auto pads = io::gerber_pad_centres(*glayer(pa_.layer), lx(pa_.layer), io::box_quad(all));
                for (auto& g : preview_placements(p, f.T)) for (auto& q : g.pads) { bool hit = false; for (auto& z : pads) if (dist(z, q) < 0.1) { hit = true; break; } if (!hit) ++bad; }
            }
            const double sc = f.rms + bad;
            if (sc < bs) { bs = sc; best = r; bm = m != 0; }
        }
        if (best >= 0) { pa_.rot_mode = best; pa_.mirror = bm; pa_refit(); }
    }
    tooltip("Try 0/90/180/270, with and without Mirror, and keep the one whose ghosts hit the most Gerber pads");
    if (pa_.ok) {
        char b[220];
        std::snprintf(b, sizeof b, "Plot \xE2\x86\x92 Gerber: rotation %.3f\xC2\xB0%s, plot 0,0 at %.4f, %.4f mm\nfit %.4f mm over %d reference(s)",
                      pa_.fit.rot, pa_.fit.mirror ? ", mirrored" : "", pa_.fit.T.tx, pa_.fit.T.ty, pa_.fit.rms, pa_.fit.matched);
        ImGui::PushStyleColor(ImGuiCol_Text, pa_.fit.rms < 0.05 ? T.ok : T.warn);
        ImGui::TextWrapped("%s", b);
        ImGui::PopStyleColor();
    }
    if (!pa_.why.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, T.warn); ImGui::TextWrapped("%s", pa_.why.c_str()); ImGui::PopStyleColor(); }
    ImGui::RadioButton("Move Gerbers to plot", &pa_.dir, 0);
    tooltip("The Gerber layers get the inverse transform; placement coordinates stay as imported (recommended)");
    ImGui::SameLine();
    ImGui::RadioButton("Move plot to Gerbers", &pa_.dir, 1);
    tooltip("Every placement, outline and circuit instance is moved / rotated onto the Gerbers");
    ImGui::BeginDisabled(pa_.dir != 0);
    ImGui::Checkbox("Apply to all Gerber layers", &pa_.all_layers);
    ImGui::EndDisabled();
    if (button("Apply", Icon::Check, true, ImVec2(S(90), 0), pa_.ok)) pa_apply();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip(pa_.ok ? "One undo step; the tool stays open to adjust or repeat" : "Pick a placement and box its pads first");
    ImGui::SameLine();
    if (button("Reset", Icon::Undo, false, ImVec2(0, 0), !pa_.refs.empty())) { pa_reset(); gui::status("References cleared: click a placement, then box its pads."); }
    tooltip("Clear the references (nothing applied is changed)");
    ImGui::SameLine();
    if (button("Undo Apply", Icon::Undo, false, ImVec2(0, 0), pa_.applied > 0 && doc->can_undo())) { --pa_.applied; pa_reset(); post_command(app::ID_EDIT_UNDO); }
    tooltip("Undo the last Apply (same as Ctrl+Z)");
    ImGui::SameLine();
    if (button("Close")) set_tool(Tool::Digitize);
    // optional global step before aligning
    if (ImGui::TreeNodeEx("Before aligning: turn / mirror all Gerbers", ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
        if (ImGui::SmallButton("90\xC2\xB0 left")) pa_global(90, false);
        ImGui::SameLine();
        if (ImGui::SmallButton("90\xC2\xB0 right")) pa_global(-90, false);
        ImGui::SameLine();
        if (ImGui::SmallButton("180\xC2\xB0")) pa_global(180, false);
        ImGui::SameLine();
        if (ImGui::SmallButton("Mirror X")) pa_global(0, true);
        ImGui::TextDisabled("All layers about their centre, one undo step. Then align.");
    }
    ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
    ImGui::TextWrapped("Board menu: Move by Two Points / Rotate About Point / Two Reference Points are manual plot moves; Rebase Coordinates only re-zeroes.");
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void GerberWorkspace::draw_r14_overlay(ImDrawList* dl) {
    if (tool_ == Tool::PlotAlign) draw_pa_overlay(dl);   // R17a
    draw_sel_overlay(dl);   // R22b panel pitch / repeated circuits
}

void GerberWorkspace::draw_r14_card() {
    // R22b (REVIEW_V21 §8.3): the outline tools have no card (selection / drawing on the canvas, then the
    // Board / Circuit popup); panel pitch and repeated-circuit detection share one compact card
    if (tool_ == Tool::PlotAlign) { draw_pa_card(); return; }   // R17a
    draw_sel_card();
}

// Wine screenshot hook (R14 verification only): PCBP_TEST_R14="ref=U1;box=x0,y0,x1,y1;rot=4;apply=1;dir=0;
// pitch=x,y,x,y,x,y;log=Z:\path". Drives the real tool code paths with fixed inputs and logs the fit.
void GerberWorkspace::test_r14() {
    static int stage = -1, frames = 0;
    static std::map<std::string, std::string> kv;
    if (stage == -2) return;
    if (stage == -1) {
        wchar_t buf[1024] = {};
        if (!GetEnvironmentVariableW(L"PCBP_TEST_R14", buf, 1024)) { stage = -2; return; }
        std::string all = U(buf), item;
        for (size_t i = 0; i <= all.size(); ++i) {
            if (i == all.size() || all[i] == ';') { auto e = item.find('='); if (e != std::string::npos) kv[item.substr(0, e)] = item.substr(e + 1); item.clear(); }
            else item += all[i];
        }
        stage = 0;
    }
    if (loading_ || !glayer(0)) return;
    ++frames;
    if (stage < 9) request_redraw(2);
    auto nums = [](const std::string& t) { std::vector<double> v; std::string c; for (size_t i = 0; i <= t.size(); ++i) { if (i == t.size() || t[i] == ',') { if (!c.empty()) v.push_back(std::atof(c.c_str())); c.clear(); } else c += t[i]; } return v; };
    auto log = [&](const std::string& line) {
        if (!kv.count("log")) return;
        if (FILE* f = _wfopen(W(kv["log"]).c_str(), L"a")) { std::fprintf(f, "%s\n", line.c_str()); std::fclose(f); }
    };
    if (stage == 0 && frames > 20) {
        if (kv.count("showall")) show_all_plot_ = kv["showall"] == "1";
        if (!kv.count("ref")) { stage = 9; return; }
        set_active(0);
        enter_plot_align();
        pa_.rot_mode = kv.count("rot") ? std::atoi(kv["rot"].c_str()) : 4;
        pa_.dir = kv.count("dir") ? std::atoi(kv["dir"].c_str()) : 0;
        pa_.mirror = kv["mirror"] == "1";
        // R17a: ref / box, optional second reference ref2 / box2; nth / nth2 = occurrence of a repeated reference
        for (int k = 0; k < 2; ++k) {
            const std::string sf = k ? "2" : "";
            if (!kv.count("ref" + sf)) continue;
            int want = kv.count("nth" + sf) ? std::atoi(kv["nth" + sf].c_str()) : 0;
            for (int i = 0; i < (int)doc->prj.comps.size(); ++i) if (doc->prj.comps[i].ref == kv["ref" + sf] && want-- == 0) { pa_.ref = i; pa_.image = 0; break; }
            auto b = nums(kv["box" + sf]);
            if (b.size() == 4) { BBox w; w.add({b[0], b[1]}); w.add({b[2], b[3]}); band_quad_.clear(); pa_rect(w); }
        }
        auto z = nums(kv["zoom"]);
        if (z.size() == 4) { BBox w; w.add({z[0], z[1]}); w.add({z[2], z[3]}); zoom_to(w, 0.05); }
        char t[256];
        std::snprintf(t, sizeof t, "fit ok=%d rot=%.6f mirror=%d T=[%.6f %.6f %.6f %.6f %.6f %.6f] rms=%.6f matched=%d refs=%d",
                      pa_.ok, pa_.fit.rot, pa_.fit.mirror, pa_.fit.T.a, pa_.fit.T.b, pa_.fit.T.c, pa_.fit.T.d, pa_.fit.T.tx, pa_.fit.T.ty, pa_.fit.rms, pa_.fit.matched, (int)pa_.refs.size());
        log(t);
        stage = 1; frames = 0;
    }
    if (stage == 1 && frames > 60) {
        if (kv["apply"] == "1" && pa_.ok) {
            pa_apply();
            for (int i = 0; i < nlayers(); ++i) {
                const GerberLayerRef& L = doc->prj.layers[i];
                char t[200];
                std::snprintf(t, sizeof t, "layer %s offset=%.6f,%.6f rot=%.6f mirror=%d", L.name.c_str(), L.offset.x, L.offset.y, L.rot, L.mirror);
                log(t);
            }
            set_tool(Tool::Digitize);
            zoom_all();
        }
        stage = 2; frames = 0;
    }
    if (stage == 2 && frames > 60) {
        auto ob = nums(kv["outline"]);
        if (ob.size() == 4) {
            set_tool(Tool::FrameSel);
            BBox w; w.add({ob[0], ob[1]}); w.add({ob[2], ob[3]}); band_quad_.clear();
            frame_sel(w);
            fsel_.pending = false;
            commit_frame(true, true);
            const BBox b = doc->prj.panel_frame_bbox();
            char t[200];
            std::snprintf(t, sizeof t, "board outline %.4f,%.4f - %.4f,%.4f (%.4f x %.4f)", b.x0, b.y0, b.x1, b.y1, b.w(), b.h());
            log(t);
        }
        // R22b: the 3-point pitch tool was replaced by the two-selection tool (PCBP_TEST_R22B drives it)
        stage = 3; frames = 0;
    }
    if (stage == 3 && frames > 90) {
        char t[200];
        std::snprintf(t, sizeof t, "images=%d", (int)doc->prj.images.size());
        log(t);
        for (auto& im : doc->prj.images) { std::snprintf(t, sizeof t, "  image %s off=%.4f,%.4f rot=%.1f", im.name.c_str(), im.offset.x, im.offset.y, im.rot); log(t); }
        for (auto& fi : check_outlines(doc->prj)) log("outline check: " + fi.ref + " " + fi.msg);
        if (kv["plot"] == "1") activate_view(doc, ViewKind::Plotter);
        stage = 9;
    }
}

// Wine screenshot hook (R17b verification only): PCBP_TEST_R17B="board=1;mode=0|1;box=x0,y0,x1,y1;pts=x,y,x,y...;
// commit=1;cancel=1;plot=1;log=Z:\path". Drives the real outline tool paths with fixed inputs and logs the result.
void GerberWorkspace::test_r17b() {
    static int stage = -1, frames = 0;
    static std::map<std::string, std::string> kv;
    if (stage == -2) return;
    if (stage == -1) {
        wchar_t buf[1024] = {};
        if (!GetEnvironmentVariableW(L"PCBP_TEST_R17B", buf, 1024)) { stage = -2; return; }
        std::string all = U(buf), item;
        for (size_t i = 0; i <= all.size(); ++i) {
            if (i == all.size() || all[i] == ';') { auto e = item.find('='); if (e != std::string::npos) kv[item.substr(0, e)] = item.substr(e + 1); item.clear(); }
            else item += all[i];
        }
        stage = 0;
    }
    if (loading_ || !glayer(0)) return;
    ++frames;
    if (stage < 9) request_redraw(2);
    auto nums = [](const std::string& t) { std::vector<double> v; std::string c; for (size_t i = 0; i <= t.size(); ++i) { if (i == t.size() || t[i] == ',') { if (!c.empty()) v.push_back(std::atof(c.c_str())); c.clear(); } else c += t[i]; } return v; };
    auto log = [&](const std::string& line) {
        if (!kv.count("log")) return;
        if (FILE* f = _wfopen(W(kv["log"]).c_str(), L"a")) { std::fprintf(f, "%s\n", line.c_str()); std::fclose(f); }
    };
    auto dump = [&](const char* when) {
        const Project& p = doc->prj;
        char t[400];
        const BBox c = p.circuit_frame_bbox(), b = p.panel_frame_bbox();
        std::snprintf(t, sizeof t, "%s: tool=%d mode=%d board=%d picks=%d contour=%d box=%.4f,%.4f-%.4f,%.4f | circuit %s %.4f x %.4f | board %s %.4f x %.4f | src=%s",
                      when, (int)tool_, fsel_.mode, fsel_.prefer_board, (int)fsel_.picks.size(), (int)(fsel_.outline.size() >= 3), fsel_.box.x0, fsel_.box.y0, fsel_.box.x1, fsel_.box.y1,
                      p.board_frame.empty() ? "-" : outline_source_name(p.board_outline_src), c.empty() ? 0 : c.w(), c.empty() ? 0 : c.h(),
                      p.panel_frame.empty() ? "-" : outline_source_name(p.panel_outline_src), p.panel_frame.empty() ? 0 : b.w(), p.panel_frame.empty() ? 0 : b.h(),
                      outline_source_name(p.board_outline_source()));
        log(t);
    };
    if (stage == 0 && frames > 30) {
        if (kv.count("showall")) show_all_plot_ = kv["showall"] == "1";
        zoom_all();
        dump("start");
        outline_enter(kv["board"] == "1", std::atoi(kv["mode"].c_str()));
        auto b = nums(kv["box"]);
        if (b.size() == 4) {
            BBox w; w.add({b[0], b[1]}); w.add({b[2], b[3]}); band_quad_.clear();
            down_w_ = {b[0], b[1]};
            if (fsel_.mode == 0) outline_draw_rect(w); else outline_pick_box(w, false);
        }
        auto q = nums(kv["pts"]);
        for (size_t k = 0; k + 1 < q.size(); k += 2) outline_pick_click({q[k], q[k + 1]}, false);
        dump("candidate");
        stage = 1; frames = 0;
    }
    if (stage == 1 && frames > 60) {
        if (kv["commit"] == "1") { const bool ok = outline_commit(); log(ok ? "commit ok" : "commit refused"); }
        if (kv["cancel"] == "1") outline_cancel();
        dump("after");
        stage = 2; frames = 0;
    }
    if (stage == 2 && frames > 30) {
        if (kv["plot"] == "1") activate_view(doc, ViewKind::Plotter);
        stage = 9;
    }
}

// Wine screenshot hook (R22b verification only): PCBP_TEST_R22B="tool=pitch|detect|addoutline|drawoutline|comp;
// box1=x0,y0,x1,y1;box2=...;ctrlbox=...;mode=0..2;board=0|1;steps=N;popup=1;off=k;zoom=x0,y0,x1,y1;log=Z:\path".
// Drives the real tool code paths (rect / confirm / apply) step by step, one step every 40 frames, and stops
// after `steps` steps so a screenshot shows that state.
void GerberWorkspace::test_r22b() {
    static int stage = -1, frames = 0, step = 0;
    static std::map<std::string, std::string> kv;
    if (stage == -2) return;
    if (stage == -1) {
        wchar_t buf[1024] = {};
        if (!GetEnvironmentVariableW(L"PCBP_TEST_R22B", buf, 1024)) { stage = -2; return; }
        std::string all = U(buf), item;
        for (size_t i = 0; i <= all.size(); ++i) {
            if (i == all.size() || all[i] == ';') { auto e = item.find('='); if (e != std::string::npos) kv[item.substr(0, e)] = item.substr(e + 1); item.clear(); }
            else item += all[i];
        }
        stage = 0;
    }
    if (loading_ || !glayer(0)) return;
    ++frames;
    request_redraw(2);
    auto nums = [](const std::string& t) { std::vector<double> v; std::string c; for (size_t i = 0; i <= t.size(); ++i) { if (i == t.size() || t[i] == ',') { if (!c.empty()) v.push_back(std::atof(c.c_str())); c.clear(); } else c += t[i]; } return v; };
    auto box = [&](const char* k, BBox& b) { auto v = nums(kv[k]); if (v.size() != 4) return false; b = BBox{}; b.add({v[0], v[1]}); b.add({v[2], v[3]}); down_w_ = {v[0], v[1]}; band_quad_.clear(); return true; };
    auto log = [&](const std::string& line) {
        if (!kv.count("log")) return;
        if (FILE* f = _wfopen(W(kv["log"]).c_str(), L"a")) { std::fprintf(f, "%s\n", line.c_str()); std::fclose(f); }
    };
    auto dump = [&](const char* when) {
        const Project& p = doc->prj;
        char t[400];
        std::snprintf(t, sizeof t, "%s: tool=%d pitch(stage=%d sel=%d dx=%.6f dy=%.6f ok=%d nx=%d ny=%d) detect(stage=%d sel=%d cands=%d) outline(mode=%d picks=%d corners=%d contour=%d box=%.4f,%.4f-%.4f,%.4f) images=%d buffer=%d comps=%d",
                      when, (int)tool_, pt_.stage, (int)pt_.sel.size(), pt_.r.dx, pt_.r.dy, pt_.r.ok, pt_.nx, pt_.ny, dt_.stage, (int)dt_.sel.size(), (int)dt_.cands.size(),
                      fsel_.mode, (int)fsel_.picks.size(), fsel_.corners, (int)(fsel_.outline.size() >= 3), fsel_.box.x0, fsel_.box.y0, fsel_.box.x1, fsel_.box.y1,
                      (int)p.images.size(), have_res_ ? (int)res_.pad_indices.size() : 0, (int)p.comps.size());
        log(t);
        for (size_t i = 0; i < dt_.cands.size(); ++i) {
            const auto& c = dt_.cands[i];
            std::snprintf(t, sizeof t, "  cand %d off=%.4f,%.4f rot=%.0f matched=%d/%d amb=%d on=%d", (int)i + 2, c.offset.x, c.offset.y, c.rot, c.matched, c.total, (int)c.ambiguous, (int)dt_.on[i]);
            log(t);
        }
        for (auto& im : p.images) { std::snprintf(t, sizeof t, "  image %s off=%.4f,%.4f rot=%.1f", im.name.c_str(), im.offset.x, im.offset.y, im.rot); log(t); }
        if (!p.panel_frame.empty()) { const BBox b = p.panel_frame_bbox(); std::snprintf(t, sizeof t, "  board outline %.4f x %.4f src=%s pts=%d", b.w(), b.h(), outline_source_name(p.panel_outline_src), (int)p.panel_frame[0].size()); log(t); }
        if (!p.board_frame.empty()) { const BBox b = p.circuit_frame_bbox(); std::snprintf(t, sizeof t, "  circuit outline %.4f x %.4f src=%s pts=%d", b.w(), b.h(), outline_source_name(p.board_outline_src), (int)p.board_frame[0].size()); log(t); }
    };
    const int steps = kv.count("steps") ? std::atoi(kv["steps"].c_str()) : 99;
    if (stage == 0 && frames > 30) {
        if (kv.count("showall")) show_all_plot_ = kv["showall"] == "1";
        BBox z;
        if (box("zoom", z)) zoom_to(z, 0.03); else zoom_all();
        const std::string& t = kv["tool"];
        if (t == "pitch") { set_tool(Tool::PitchPick); pt_.mode = std::atoi(kv["mode"].c_str()); }
        else if (t == "detect") set_tool(Tool::Detect);
        else if (t == "addoutline") outline_enter(false, 1);
        else if (t == "drawoutline") outline_enter(false, 0);
        else set_tool(Tool::Digitize);
        dump("start");
        stage = 1; frames = 0; step = 0;
    }
    if (stage == 1 && frames > 40) {
        frames = 0;
        if (step >= steps) { dump("hold"); stage = 9; return; }
        const std::string& t = kv["tool"];
        BBox b;
        // step plan per tool
        if (t == "pitch") {
            if (step == 0) { if (box("box1", b)) rect(b, 0); }
            else if (step == 1) confirm_action();
            else if (step == 2) { if (box("box2", b)) rect(b, 0); }
            else if (step == 3) confirm_action();
            else if (step == 4) confirm_action();   // apply
            else { stage = 9; dump("end"); return; }
        } else if (t == "detect") {
            if (step == 0) { if (box("box1", b)) rect(b, 0); }
            else if (step == 1) { if (box("ctrlbox", b)) rect(b, 2); }
            else if (step == 2) confirm_action();
            else if (step == 3) { if (kv.count("off")) { const int k = std::atoi(kv["off"].c_str()) - 2; if (k >= 0 && k < (int)dt_.on.size()) dt_.on[k] = 0; } }
            else if (step == 4) confirm_action();
            else { stage = 9; dump("end"); return; }
        } else if (t == "addoutline" || t == "drawoutline") {
            if (step == 0) { if (box("box1", b)) rect(b, 0); }
            else if (step == 1) { if (box("ctrlbox", b)) rect(b, 2); }
            else if (step == 2) { if (kv["popup"] == "1") confirm_action(); }
            else if (step == 3) { otype_open_ = false; const bool ok = outline_commit_as(kv["board"] == "1"); log(ok ? "commit ok" : "commit refused"); }
            else { stage = 9; dump("end"); return; }
        } else {   // comp: box adds, ctrlbox removes, then the Confirm card
            if (step == 0) { if (box("box1", b)) rect(b, 0); }
            else if (step == 1) { if (box("box2", b)) rect(b, 0); }
            else if (step == 2) { if (box("ctrlbox", b)) rect(b, 2); }
            else if (step == 3) confirm_action();
            else { stage = 9; dump("end"); return; }
        }
        char nm[32]; std::snprintf(nm, sizeof nm, "step %d", step);
        dump(nm);
        ++step;
    }
}

void GerberWorkspace::draw(ImVec2 size) {
    if (!doc) return;
    const auto prof_t0 = std::chrono::steady_clock::now();
    auto prof_ms = [](std::chrono::steady_clock::time_point a) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count(); };
    double p_input = 0, p_render = 0;
    struct ProfEnd {   // records the view total on every exit path
        GerberWorkspace* w; std::chrono::steady_clock::time_point t0; double* in; double* rd;
        ~ProfEnd() { w->prof_finish(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), *in, *rd); }
    } prof_end{this, prof_t0, &p_input, &p_render};
    if (!gpu_tried_) {
        gpu_tried_ = true;
        wchar_t ev[8] = {};
        if (GetEnvironmentVariableW(L"PCBP_GERBER_D2D", ev, 8) && ev[0] == L'1') gpu_err_ = "disabled by PCBP_GERBER_D2D";
        else gpu_ok_ = gpu_.init(gpu_err_);
        if (!gpu_ok_) gui::status("Gerber GPU renderer unavailable (" + gpu_err_ + "); using Direct2D.");
        cache_.clear();
    }
    sync();
    if (plot_align_request_take(doc.get())) enter_plot_align();   // Board > Align CAD to Gerber
    { bool brd = false; if (gerber_outline_tool_take(doc.get(), brd)) outline_enter(brd, -1); }   // R17b: dialog "Draw / pick on the Gerber"
    test_r17b();
    test_r14();
    test_r22b();
    { std::string st; if (pkg_wizard_request_take(doc.get(), st)) enter_pkg_wizard(st); }   // R16b
    test_r16b();
    // R12 §7/§12: Ctrl+Space = maximize / restore the Gerber canvas (layer panel hidden)
    static bool gmax = false;
    if (hovered_ && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput && !ac_.open && ImGui::IsKeyPressed(ImGuiKey_Space, false)) { gmax = !gmax; dirty_ = true; }
    if (panel_w_ <= 0) panel_w_ = S(318);
    const float panel_w0 = gmax ? 0.f : std::clamp(panel_w_, S(220), std::max(S(220), size.x * 0.6f));
    const float spl = gmax ? 0.f : S(5);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(4), S(4)));
    if (!gmax) { draw_panel(panel_w0, size.y); ImGui::SameLine(0, 0); }
    if (!gmax) {   // vertical splitter: panel width (R10 §8)
        ImGui::InvisibleButton("##vsplit", ImVec2(spl, std::max(8.f, size.y)));
        const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
        if (hv || ac) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ac) panel_w_ = panel_w0 + ImGui::GetIO().MouseDelta.x;
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(a, b, hv || ac ? th().accent : th().border);
        ImGui::SameLine(0, 0);
    }
    const float panel_w = panel_w0 + spl;
    // gerber toolbar (top) + canvas + readout strip (bottom)
    const bool has_bar = nlayers() > 0 && !loading_;
    const float bar_h = has_bar ? S(38) : 0.f, strip_h = has_bar ? S(30) : 0.f;
    ImGui::BeginGroup();
    if (has_bar) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        draw_gbar(std::max(8.f, size.x - panel_w));
        ImGui::PopStyleVar();
    }
    ImVec2 cpos = ImGui::GetCursorScreenPos();
    const float vbar_w = S(78);   // R22b right-hand vertical toolbar (two icon columns)
    ImVec2 csize(std::max(8.f, size.x - panel_w - vbar_w), std::max(8.f, size.y - bar_h - strip_h));
    if (csize.x != size_.x || csize.y != size_.y || cpos.x != pos_.x || cpos.y != pos_.y) {
        bool first = size_.x < 8;
        pos_ = cpos; size_ = csize; dirty_ = true;
        if (first) fitted_ = false;
    }
    if (!fitted_ && size_.x > 8) { zoom_all(); fitted_ = true; }
    if (nlayers() == 0 || loading_) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col32(ui::pal().gerber_bg));
        ImGui::BeginChild("##gempty", csize);
        if (loading_) {
            ImVec2 av = ImGui::GetContentRegionAvail();
            ImGui::SetCursorPos(ImVec2(av.x * 0.5f - S(12), av.y * 0.5f - S(12)));
            spinner(S(12));
        } else if (empty_state(Icon::GerberView, "No Gerber layers", "Load Gerber / Excellon files (or a zip), then add components from their pads.",
                               "Load Layers...", Icon::ImportGerber))
            post_command(ID_IMPORT_GERBER);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        ImGui::PopStyleVar();
        return;
    }
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##gcanvas", csize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    hovered_ = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const double ps = scale_, pcx = cx_, pcy = cy_;
    { const auto ti = std::chrono::steady_clock::now(); handle_input(hovered_, active); p_input = prof_ms(ti); }
    if (ps != scale_ || pcx != cx_ || pcy != cy_) dirty_ = true;
    context_menu();
    if (dirty_) { const auto tr = std::chrono::steady_clock::now(); render_scene(); p_render = prof_ms(tr); }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(pos_, ImVec2(pos_.x + size_.x, pos_.y + size_.y), true);
    void* tex = gpu_ok_ ? gpu_.texture() : d2d_tex_.imgui_id();
    if (tex) dl->AddImage((ImTextureID)tex, pos_, ImVec2(pos_.x + size_.x, pos_.y + size_.y));
    else dl->AddRectFilled(pos_, ImVec2(pos_.x + size_.x, pos_.y + size_.y), col32(ui::pal().gerber_bg));
    draw_overlay(dl);
    // view badge (top-left): view side + fill mode; flipped views get a coloured banner + frame
    {
        const bool flipped = mirror_ || mirror_y_;
        std::string b = flipped ? std::string("FLIPPED VIEW") + (mirror_ ? "  X (left/right)" : "") + (mirror_y_ ? "  Y (up/down)" : "") + "  \xC2\xB7  bottom side"
                                : std::string("TOP VIEW");
        b += (fill_ == ggpu::FillMode::Outline ? "  \xC2\xB7  outline" : fill_ == ggpu::FillMode::Sketch ? "  \xC2\xB7  sketch" : "");
        b += compare_ ? "  \xC2\xB7  compare" : "";
        if (tool_ == Tool::AlignSrc || tool_ == Tool::AlignDst)
            b += std::string("  \xC2\xB7  ALIGN ") + (align_two_ ? (align_stage_ ? "2/2 " : "1/2 ") : "") + (tool_ == Tool::AlignSrc ? "pick source" : "pick target");
        ImVec2 ts = ImGui::CalcTextSize(b.c_str());
        ImVec2 a(pos_.x + S(10), pos_.y + S(10));
        if (flipped) {
            dl->AddRect(ImVec2(pos_.x + S(1), pos_.y + S(1)), ImVec2(pos_.x + size_.x - S(1), pos_.y + size_.y - S(1)), th().warn, 0, 0, S(3));
            dl->AddRectFilled(a, ImVec2(a.x + ts.x + S(14), a.y + ts.y + S(8)), th().warn, S(6));
            ImGui::PushFont(g_font_bold);
            dl->AddText(ImVec2(a.x + S(7), a.y + S(4)), IM_COL32(20, 20, 20, 255), b.c_str());
            ImGui::PopFont();
        } else {
            dl->AddRectFilled(a, ImVec2(a.x + ts.x + S(14), a.y + ts.y + S(8)), with_alpha(th().panel, 0.85f), S(6));
            dl->AddText(ImVec2(a.x + S(7), a.y + S(4)), th().text_dim, b.c_str());
        }
    }
    // active-tool hint pill (top centre, clear of the coordinate box), slides/fades in on tool change
    if (tool_ != Tool::Select && !ac_.open) {
        std::string h = tool_hint(tool_);
        size_t c = h.find(": ");
        if (c != std::string::npos && c < 32) h = h.substr(c + 2);
        if (!h.empty() && h.back() == '.') h.pop_back();
        if (!h.empty()) h[0] = (char)std::toupper((unsigned char)h[0]);
        if (have_res_ && tool_ == Tool::Digitize)
            h = std::to_string(res_.pad_indices.size()) + " pad(s) selected  \xC2\xB7  click / drag adds, Ctrl removes  \xC2\xB7  right-click / Enter = Confirm  \xC2\xB7  Esc clears";
        else h += "  \xC2\xB7  Esc to cancel";
        if (h != hint_text_) { hint_text_ = h; hint_t0_ = ImGui::GetTime(); }
        if (!gui::hint_shown(("gerber.tool" + std::to_string((int)tool_)).c_str())) h.clear();   // P1-07: tutorial mode only (status bar keeps the count)
        if (!h.empty()) {
        float k = std::min(1.f, (float)(ImGui::GetTime() - hint_t0_) / 0.25f), e = 1 - (1 - k) * (1 - k);
        if (k < 1) request_redraw(2);
        float is = ImGui::GetFontSize(), maxw = std::min(size_.x - S(60), S(760));
        ImVec2 ts = ImGui::CalcTextSize(h.c_str(), nullptr, false, maxw - is - S(30));
        float wpx = ts.x + is + S(30);
        ImVec2 a(pos_.x + (size_.x - wpx) * 0.5f, pos_.y + (vrot_ != 0 ? S(46) : S(44)) - (1 - e) * S(10)), b(a.x + wpx, a.y + ts.y + S(12));
        float r = std::min(S(14), (b.y - a.y) * 0.5f);
        dl->AddRectFilled(a, b, with_alpha(th().panel, 0.94f * e), r);
        dl->AddRect(a, b, with_alpha(th().accent, 0.8f * e), r, 0, S(1.2f));
        icon_draw(dl, tool_icon(tool_), ImVec2(a.x + S(10), a.y + S(6)), is, e);
        dl->AddText(nullptr, 0, ImVec2(a.x + S(16) + is, a.y + S(6)), with_alpha(th().text, e), h.c_str(), nullptr, maxw - is - S(30));
        }
    }
    // mode tag beside the cursor (R10 §7 mode feedback): what a click / drag does right now
    if (hovered_ && !ac_.open && drag_ != Drag::Pan) {
        const ImGuiIO& io = ImGui::GetIO();
        const char* tag = nullptr;
        ImU32 tc = th().accent;
        switch (tool_) {
        // R12 §13: no floating "pads" / "+ pads" label at the crosshair (the tool banner already says it); only the
        // non-obvious Ctrl = remove state is tagged
        case Tool::Digitize: if (io.KeyCtrl && have_res_) { tag = "\xE2\x88\x92 pads"; tc = th().warn; } break;
        case Tool::FrameSel: case Tool::PitchPick: case Tool::Detect: if (io.KeyCtrl) { tag = "\xE2\x88\x92 remove"; tc = th().warn; } break;
        case Tool::DeleteComp: tag = "delete"; tc = th().error; break;
        case Tool::CropIn: tag = "crop in"; tc = th().warn; break;
        case Tool::CropOut: tag = "crop out"; tc = th().warn; break;
        case Tool::Measure: tag = "measure"; break;
        case Tool::Clearance: tag = "clearance"; break;
        case Tool::Query: tag = "query"; break;
        case Tool::ZoomRect: tag = "zoom"; break;
        case Tool::Frame: tag = "frame"; break;
        case Tool::AlignSrc: case Tool::AlignDst: tag = "align"; break;
        default: break;
        }
        if (tag) {
            const ImVec2 mp = io.MousePos, ts = ImGui::CalcTextSize(tag);
            const ImVec2 a(mp.x + S(14), mp.y + S(14)), b(a.x + ts.x + S(10), a.y + ts.y + S(4));
            dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 170), S(4));
            dl->AddRect(a, b, tc, S(4), 0, S(1));
            dl->AddText(ImVec2(a.x + S(5), a.y + S(2)), tc, tag);
        }
    }
    draw_rot_banner(dl);
    dl->PopClipRect();
    draw_coord_box(dl);
    draw_info_card(dl);
    draw_align_view_card();
    draw_r14_card();
    draw_wz_card();   // R16b
    draw_add_card();
    draw_outline_type_popup();   // R22b
    // R22b (REVIEW_V21 §5): secondary tools in a vertical toolbar on the right of the viewport
    ImGui::SetCursorScreenPos(ImVec2(pos_.x + size_.x, pos_.y));
    draw_vbar(vbar_w, size_.y + strip_h);
    ImGui::EndGroup();
    ImGui::PopStyleVar();
}

// main app toolbar: only the entry point; the Gerber tools live in the canvas' own toolbar (draw_gbar)
void GerberWorkspace::draw_toolbar() {
    if (icon_button("##gload", Icon::ImportGerber, "Load / edit Gerber layers...")) post_command(ID_IMPORT_GERBER);
    ImGui::SameLine(0, S(2));
}

// R22b (REVIEW_V21 §5): top Gerber toolbar = the important Gerber operations, the outline tools and the
// array tools, as large icon + short label buttons. Nothing important lives in a dropdown any more.
// Secondary tools are in the vertical toolbar on the right (draw_vbar); selection / component creation /
// Confirm are below the layer list (draw_digitize_panel).
void GerberWorkspace::draw_gbar(float w) {
    const GTheme& T = th();
    const float bh = S(38);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, T.panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(3), S(2)));
    ImGui::BeginChild("##gbar", ImVec2(w, bh), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_HorizontalScrollbar);
    {
        ImVec2 wp = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x, wp.y + bh - 1), ImVec2(wp.x + w, wp.y + bh - 1), T.border);
    }
    // narrow canvas: secondary labels go first (icon + tooltip), then the long labels are shortened
    const int compact = w >= S(1150) ? 0 : w >= S(980) ? 1 : 2;
    // icon + label button, same height everywhere (S(30)); on = the tool is active; empty label = icon only
    auto lb = [&](const char* id, const char* label, Icon ic, const char* tip, bool on, bool en = true) {
        const float ih = S(20), hgt = S(30);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const ImVec2 sz(*label ? ih + ts.x + S(18) : hgt, hgt);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? with_alpha(T.accent, 0.45f) : T.raised);
        ImGui::PushStyleColor(ImGuiCol_Border, on ? T.accent : T.border);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, on ? S(1.5f) : S(1));
        ImGui::BeginDisabled(!en);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const bool r = ImGui::Button(id, sz);
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImDrawList* d = ImGui::GetWindowDrawList();
        icon_draw(d, ic, ImVec2(p0.x + (*label ? S(6) : (hgt - ih) * 0.5f), p0.y + (hgt - ih) * 0.5f), ih, en ? 1.f : 0.4f);
        d->AddText(ImVec2(p0.x + S(12) + ih, p0.y + (hgt - ts.y) * 0.5f), en ? T.text : T.text_faint, label);
        if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip(tip);
        ImGui::SameLine(0, S(3));
        return r && en;
    };
    auto sep = [&]() { toolbar_separator(); };
    // ---- Gerber operations
    if (lb("##gpa", compact ? "" : "Align Plot", Icon::Link, "Align Plot to Gerber: click a placement, box its pads on the Gerber, check the ghost preview, Apply (one undo step)", tool_ == Tool::PlotAlign))
        { if (tool_ == Tool::PlotAlign) set_tool(Tool::Digitize); else enter_plot_align(); }
    if (lb("##gwz", compact < 2 ? "Assign Packages from Gerber" : "Assign Packages", Icon::PackageEditor,
           "Package assignment wizard: jumps to the first placement whose package has no pads, you box its Gerber pads,\n"
           "Confirm builds the package and assigns it to every placement using it, then it moves to the next one.", tool_ == Tool::PkgWizard))
        { if (tool_ == Tool::PkgWizard) set_tool(Tool::Digitize); else enter_pkg_wizard(""); }
    if (lb("##gal", compact ? "" : "Align Layers", Icon::Offset, "Align one Gerber layer to another (one or two points), or transform layers", tool_ == Tool::AlignSrc || tool_ == Tool::AlignDst)) ImGui::OpenPopup("##galpop");
    if (ImGui::BeginPopup("##galpop")) {
        if (imenu("Align Layer (one point: move)", Icon::Offset, nullptr, (tool_ == Tool::AlignSrc || tool_ == Tool::AlignDst) && !align_two_)) { align_two_ = false; set_tool(Tool::AlignSrc); }
        if (imenu("Align Layer (two points: move + rotate)", Icon::Offset, nullptr, (tool_ == Tool::AlignSrc || tool_ == Tool::AlignDst) && align_two_)) { align_two_ = true; set_tool(Tool::AlignSrc); }
        if (imenu("Transform Layers (offset / rotate / mirror)...", Icon::Offset)) gui::defer([this]() { transform_layers(); });
        ImGui::EndPopup();
    }
    sep();
    // R24a (user v23 #5): Add Outline / Draw Outline live only in the Select · Create · Confirm panel under the layer list
    // ---- arrays
    if (lb("##gpp", "Panel Pitch", Icon::AllImages, "Panel pitch from two selections: box features on the reference circuit (Enter), the same features on the circuit\none across and one up (Enter); pitch = difference of the centres; then the counts -> linear array.",
           tool_ == Tool::PitchPick))
        set_tool(Tool::PitchPick);
    if (lb("##gdr", compact < 2 ? "Detect Repeated Circuits" : "Detect Circuits", Icon::Wand, "Select a distinctive group of pads / elements on the reference circuit, Enter: every copy in the artwork at\n0 / 90 / 180 / 270\xC2\xB0 is proposed; review (toggle) the candidates, Confirm -> nonlinear array.",
           tool_ == Tool::Detect))
        set_tool(Tool::Detect);
    sep();
    if (lb("##gpall", compact ? "" : "Overlay", Icon::Locations, show_all_plot_ ? "Placement overlay: ON (click to hide)\nEvery placement and the board outline are drawn over the Gerbers."
                                                                       : "Placement overlay: OFF (click to show)\nGerber-only view; placements built on this tab stay visible.", show_all_plot_))
        show_all_plot_ = !show_all_plot_;
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// R22b: vertical toolbar on the right of the viewport: inspection, view, fill, layers, zoom
void GerberWorkspace::draw_vbar(float w, float h) {
    const GTheme& T = th();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, T.panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(5), S(5)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(2), S(3)));
    ImGui::BeginChild("##gvbar", ImVec2(w, std::max(8.f, h)), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    {
        const ImVec2 wp = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(wp, ImVec2(wp.x, wp.y + h), T.border);
    }
    // two columns of large icons (fits a 650 px tall canvas without scrolling)
    const float isz = (w - S(12)) * 0.5f;
    int col = 0;
    auto ib = [&](const char* id, Icon ic, const char* tip, bool on, bool en = true) {
        if (col == 1) ImGui::SameLine(0, S(2));
        const bool r = icon_button(id, ic, tip, on, en, isz);
        col = (col + 1) % 2;
        return r;
    };
    auto tb = [&](const char* id, Icon ic, const char* tip, Tool t) { if (ib(id, ic, tip, tool_ == t)) set_tool(t); };
    auto sep = [&]() {
        col = 0;
        ImGui::Dummy(ImVec2(isz * 2, S(2)));
        const ImVec2 p = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + S(1)), ImVec2(p.x + isz * 2 + S(2), p.y + S(1)), T.border);
    };
    tb("##gq", Icon::Query, "Query feature: D-code, shape, size, position (Q)", Tool::Query);
    tb("##gm", Icon::Measure, "Measure point to point with snap (D)", Tool::Measure);
    tb("##gc", Icon::Clearance, "Edge-to-edge clearance between two features (E)", Tool::Clearance);
    tb("##gro", Icon::VirtualOrigin, "Set relative origin (Shift+R)", Tool::RelOrigin);
    tb("##gav", Icon::VerifyAngle, "Align View to Line (L): draw a line along a rotated pad row; the view is rotated so it becomes horizontal (display only)", Tool::AlignView);
    if (vrot_ != 0) {
        char rb[64];
        std::snprintf(rb, sizeof rb, "Reset view rotation (now %.1f\xC2\xB0)", view_angle());
        if (ib("##gavr", Icon::Rot180, rb, true)) set_view_rot(0);
    }
    sep();
    tb("##gci", Icon::CropIn, "Crop in: hide data inside a box", Tool::CropIn);
    tb("##gco", Icon::CropOut, "Crop out: hide data outside a box", Tool::CropOut);
    sep();
    if (ib("##gf1", Icon::FillSolid, "Solid fill (O cycles)", fill_ == ggpu::FillMode::Solid)) { fill_ = ggpu::FillMode::Solid; dirty_ = true; }
    if (ib("##gf2", Icon::FillOutline, "Outline mode", fill_ == ggpu::FillMode::Outline)) { fill_ = ggpu::FillMode::Outline; dirty_ = true; }
    if (ib("##gf3", Icon::FillSketch, "Sketch mode (trace centre lines, pad outlines)", fill_ == ggpu::FillMode::Sketch)) { fill_ = ggpu::FillMode::Sketch; dirty_ = true; }
    if (ib("##gcmp", Icon::Compare, "Compare two layers (XOR)", compare_, nlayers() >= 2)) {
        if (compare_) { compare_ = false; dirty_ = true; }
        else ImGui::OpenPopup("##cmppop");
    }
    if (ImGui::BeginPopup("##cmppop")) {
        section_header("COMPARE LAYERS");
        if (cmp_a_ < 0 || cmp_a_ >= nlayers()) cmp_a_ = std::max(0, act_);
        if (cmp_b_ < 0 || cmp_b_ >= nlayers() || cmp_b_ == cmp_a_) cmp_b_ = cmp_a_ == 0 ? 1 : 0;
        auto combo = [&](const char* lbl, int& v) {
            ImGui::SetNextItemWidth(S(220));
            if (ImGui::BeginCombo(lbl, doc->prj.layers[v].name.c_str())) {
                for (int i = 0; i < nlayers(); ++i) if (ImGui::Selectable(doc->prj.layers[i].name.c_str(), i == v)) v = i;
                ImGui::EndCombo();
            }
        };
        combo("Layer A", cmp_a_);
        combo("Layer B", cmp_b_);
        ImGui::TextDisabled("Red = only in A, cyan = only in B, grey = both.");
        if (button("Compare", Icon::Compare, true) && cmp_a_ != cmp_b_) { compare_ = true; dirty_ = true; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (ib("##gimg", Icon::Image, "Board image under the Gerbers (bare-board scan vs stencil / paste, Screen blend)", img_show_, !doc->prj.board_images.empty()))
        ImGui::OpenPopup("##gimgpop");
    if (ImGui::BeginPopup("##gimgpop")) { board_image_popup(); ImGui::EndPopup(); }
    sep();
    if (ib("##gtop", Icon::ViewTop, "View from top (not flipped)", !mirror_ && !mirror_y_)) { mirror_ = mirror_y_ = false; dirty_ = true; }
    if (ib("##gbot", Icon::ViewBottom, "Flip view left / right (X) for bottom-side work (F)", mirror_)) { mirror_ = !mirror_; dirty_ = true; }
    if (ib("##gfy", Icon::Flip, "Flip view up / down (Y) (Shift+F)", mirror_y_)) { mirror_y_ = !mirror_y_; dirty_ = true; }
    if (ib("##gvis", Icon::Eye, "Layer visibility", false, nlayers() > 0)) ImGui::OpenPopup("##gvispop");
    if (ImGui::BeginPopup("##gvispop")) { visibility_menu_items(); ImGui::EndPopup(); }
    if (ib("##gstu", Icon::ArrowUp, "Show previous layer (PgUp / [): active + only visible", false, nlayers() > 0)) step_layer(-1);
    if (ib("##gstd", Icon::ArrowDown, "Show next layer (PgDn / ]): active + only visible", false, nlayers() > 0)) step_layer(+1);
    sep();
    if (ib("##gzi", Icon::ZoomIn, "Zoom in (+ / wheel)", false)) zoom_at(1.39, ImVec2(pos_.x + size_.x / 2, pos_.y + size_.y / 2));
    if (ib("##gzo", Icon::ZoomOut, "Zoom out (- / wheel)", false)) zoom_at(1 / 1.39, ImVec2(pos_.x + size_.x / 2, pos_.y + size_.y / 2));
    tb("##gzr", Icon::ZoomRect, "Zoom window (Z)", Tool::ZoomRect);
    if (ib("##gza", Icon::ZoomAll, "Zoom all (Home)", false)) zoom_all();
    sep();
    if (ib("##gadd", Icon::Check, "Show added: tint pads already used by added components (hatched) and draw their outlines + refs", show_added_)) show_added_ = !show_added_;
    if (ib("##gctr", Icon::Pan, "Centre the crosshair on the selection (C)", false, have_res_ || !matches_.empty() || doc->selected_count() > 0)) centre_crosshair_on_selection();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

bool GerberWorkspace::on_command(int id) {
    switch (id) {
    case ID_VIEW_ZOOM_IN: zoom_at(1.39, ImVec2(pos_.x + size_.x / 2, pos_.y + size_.y / 2)); return true;
    case ID_VIEW_ZOOM_OUT: zoom_at(1 / 1.39, ImVec2(pos_.x + size_.x / 2, pos_.y + size_.y / 2)); return true;
    case ID_VIEW_ZOOM_RECT: set_tool(Tool::ZoomRect); return true;
    case ID_BOARD_ALIGN_CAD_GERBER: enter_plot_align(); return true;
    case ID_VIEW_ZOOM_100: scale_ = 96.0 / 25.4 * dpi_scale(); dirty_ = true; return true;
    case ID_VIEW_ZOOM_ALL: zoom_all(); return true;
    case ID_VIEWER_TOP: mirror_ = false; dirty_ = true; return true;
    case ID_VIEWER_BOTTOM: mirror_ = true; dirty_ = true; return true;
    case ID_EDIT_DELETE: delete_selected(); return true;
    case ID_EDIT_SELECT_ALL: for (auto& c : doc->prj.comps) c.selected = true; doc->changed(ui::CH_SELECTION); return true;
    case ID_TOOL_INFO: set_tool(Tool::Query); return true;
    case ID_TOOL_SELECT: set_tool(Tool::Select); return true;
    }
    return false;
}

int GerberWorkspace::cmd_state(int id) {
    switch (id) {
    case ID_VIEW_ZOOM_IN: case ID_VIEW_ZOOM_OUT: case ID_VIEW_ZOOM_100: case ID_VIEW_ZOOM_ALL: return 1;
    case ID_VIEW_ZOOM_RECT: return 1 | (tool_ == Tool::ZoomRect ? 2 : 0);
    case ID_VIEWER_TOP: return 1 | (!mirror_ ? 2 : 0);
    case ID_VIEWER_BOTTOM: return 1 | (mirror_ ? 2 : 0);
    case ID_EDIT_DELETE: return (have_res_ || !matches_.empty() || doc->selected_count()) ? 1 : 0;
    case ID_EDIT_SELECT_ALL: return doc->prj.comps.empty() ? 0 : 1;
    case ID_TOOL_INFO: return 1 | (tool_ == Tool::Query ? 2 : 0);
    case ID_TOOL_SELECT: return 1 | (tool_ == Tool::Select ? 2 : 0);
    }
    return -1;
}

std::string GerberWorkspace::status() const {
    if (!doc) return {};
    size_t objs = 0, vis = 0;
    for (int i = 0; i < nlayers(); ++i)
        if (const gb::Layer* L = glayer(i)) { objs += L->objs.size(); if (doc->prj.layers[i].visible) ++vis; }
    char b[160];
    std::snprintf(b, sizeof b, "%d/%d layers \xC2\xB7 %s objects", (int)vis, nlayers(),
                  objs >= 10000 ? (std::to_string(objs / 1000) + "k").c_str() : std::to_string(objs).c_str());
    std::string s = b;
    if (have_res_) s += " \xC2\xB7 buffer " + std::to_string(res_.pad_indices.size());
    if (!matches_.empty()) s += " \xC2\xB7 " + std::to_string(matches_.size()) + " matches";
    // honest, event-driven numbers: CPU ms per redraw (the view only redraws on input / change)
    std::snprintf(b, sizeof b, " \xC2\xB7 %s render %.1f ms \xC2\xB7 view %.1f ms/redraw (input %.1f, overlay %.1f)", gpu_ok_ ? "GPU" : "D2D",
                  render_ms_avg_, prof_view_, prof_input_, prof_overlay_);
    return s + b;
}

std::string GerberWorkspace::status_right() const {
    if (!doc || !hovered_) return {};
    Vec2 p = (tool_ == Tool::Measure || tool_ == Tool::RelOrigin) && snap_valid_ ? snap_pt_ : crosshair();
    std::string s = "X " + fmt_len_u8(p.x - doc->prj.virtual_origin.x, false) + "  Y " + fmt_len_u8(p.y - doc->prj.virtual_origin.y);
    if (anchor_on_) s += "   anchor d " + fmt_len_u8(dist(p, anchor_));
    if (have_rel_) s += "   d " + fmt_len_u8(p.x - rel_.x, false) + ", " + fmt_len_u8(p.y - rel_.y) + "  r " + fmt_len_u8(dist(p, rel_));
    if (tool_ == Tool::Measure && meas_has_a_) {
        Vec2 b = meas_done_ ? meas_b_ : p;
        s += "   dist " + fmt_len_u8(dist(meas_a_, b));
    }
    return s;
}

} // namespace

std::unique_ptr<View> make_gerber_view() { return std::make_unique<GerberWorkspace>(); }

} // namespace pcbp::gui

// ===========================================================================
// Gerber Import dialog (multi-file / folder / zip, content-based detection)
// ===========================================================================
namespace pcbp::gui {
namespace {

using FKind = gb::FileKind;
FKind classify(const std::string& t) { return gb::classify_file(t); }

std::string fmt_string(const gb::Format& f) {
    return std::string(f.inch ? "in " : "mm ") + std::to_string(f.int_digits) + "." + std::to_string(f.frac_digits) + (f.lead_supp ? " L" : " T");
}
gb::ListUnits list_units(int idx) {
    return idx == 1 ? gb::ListUnits::Mil : idx == 2 ? gb::ListUnits::Inch : idx == 3 ? gb::ListUnits::MM : gb::ListUnits::Auto;
}
const char* units_hint_of(int idx) { return idx == 1 ? "mil" : idx == 2 ? "inch" : idx == 3 ? "mm" : ""; }
int units_index(const std::string& h) { return h == "mil" ? 1 : h == "inch" ? 2 : h == "mm" ? 3 : 0; }

std::string layer_fmt_u8(const GerberLayerRef& l) {
    if (gb::looks_like_rs274x(l.content)) return "274X";
    if (gb::looks_like_excellon(l.content)) return "Excellon";
    return std::string("274D ") + (l.format.empty() ? "auto" : l.format) + (l.aperture_list.empty() ? " (no apertures)" : "");
}
// ---------------------------------------------------------------------------
// Drag & drop into the Load Gerber Layers dialog.
// The dialog is a nested-frame modal (Form::run), so the shell's deferred WM_DROPFILES handler
// would only run AFTER the dialog closed. While the dialog is open we therefore (1) register an
// OLE IDropTarget on the main window (drag-over highlight + drop) and (2) subclass the main
// window's WNDPROC to take WM_DROPFILES directly (fallback when OLE registration fails, and the
// path used by the PCBP_TEST_DROP automation hook). Both feed g_drop_queue, which the dialog
// drains on its next frame.
// ---------------------------------------------------------------------------
std::vector<std::wstring> g_drop_queue;
std::vector<std::wstring> g_preload;      // files dropped on the app (dialog closed) that open the dialog
bool g_drop_active = false;
bool g_drag_hover = false;                // an OLE drag with files is over the window

std::vector<std::wstring> hdrop_paths(HDROP hd) {
    std::vector<std::wstring> v;
    const UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        const UINT len = DragQueryFileW(hd, i, nullptr, 0);
        std::wstring p(len + 1, L'\0');
        DragQueryFileW(hd, i, p.data(), len + 1);
        p.resize(len);
        if (!p.empty()) v.push_back(p);
    }
    return v;
}

struct GerberDropTarget final : IDropTarget {
    virtual ~GerberDropTarget() = default;
    LONG refs = 1;
    bool files = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *pv = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        *pv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return (ULONG)r; }
    static bool has_files(IDataObject* d) {
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return d && d->QueryGetData(&fe) == S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        files = has_files(d);
        *eff = files ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        g_drag_hover = files;
        request_redraw(4);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* eff) override {
        *eff = files ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        g_drag_hover = files;
        request_redraw(2);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { g_drag_hover = false; files = false; request_redraw(2); return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        g_drag_hover = false;
        *eff = DROPEFFECT_NONE;
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        if (d && d->GetData(&fe, &sm) == S_OK) {
            if (HDROP hd = (HDROP)GlobalLock(sm.hGlobal)) {
                auto v = hdrop_paths(hd);
                GlobalUnlock(sm.hGlobal);
                g_drop_queue.insert(g_drop_queue.end(), v.begin(), v.end());
                *eff = DROPEFFECT_COPY;
            }
            ReleaseStgMedium(&sm);
        }
        request_redraw(4);
        return S_OK;
    }
};

WNDPROC g_prev_proc = nullptr;
LRESULT CALLBACK drop_subclass_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DROPFILES && g_drop_active) {
        HDROP hd = (HDROP)w;
        auto v = hdrop_paths(hd);
        DragFinish(hd);
        g_drop_queue.insert(g_drop_queue.end(), v.begin(), v.end());
        g_drag_hover = false;
        request_redraw(4);
        return 0;
    }
    return CallWindowProcW(g_prev_proc, h, m, w, l);
}

// RAII: active while the dialog runs
struct DropScope {
    HWND hwnd = nullptr;
    GerberDropTarget* tgt = nullptr;
    bool ole = false, registered = false;
    DropScope() {
        hwnd = main_hwnd();
        g_drop_active = true;
        g_drop_queue.clear();
        g_drag_hover = false;
        if (!hwnd) return;
        DragAcceptFiles(hwnd, TRUE);
        g_prev_proc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)drop_subclass_proc);
        const HRESULT hr = OleInitialize(nullptr);
        ole = SUCCEEDED(hr);
        if (ole) {
            tgt = new GerberDropTarget();
            registered = RegisterDragDrop(hwnd, tgt) == S_OK;
            if (!registered) { tgt->Release(); tgt = nullptr; }
        }
    }
    ~DropScope() {
        if (hwnd) {
            if (registered) RevokeDragDrop(hwnd);
            if (tgt) tgt->Release();
            if (g_prev_proc && (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC) == drop_subclass_proc) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)g_prev_proc);
            g_prev_proc = nullptr;
            if (ole) OleUninitialize();
        }
        g_drop_active = false;
        g_drag_hover = false;
        g_drop_queue.clear();
    }
};

// automation (Wine has no Explorer drag source): PCBP_TEST_DROP="path|path|..." is delivered to the
// dialog as a real WM_DROPFILES (HDROP built here, sent to the main window -> subclass -> queue)
void test_drop_post(HWND hwnd, const std::wstring& list) {
    std::vector<std::wstring> v;
    size_t p = 0;
    while (p <= list.size()) {
        size_t e = list.find(L'|', p);
        if (e == std::wstring::npos) e = list.size();
        if (e > p) v.push_back(list.substr(p, e - p));
        p = e + 1;
    }
    size_t chars = 1;
    for (auto& s : v) chars += s.size() + 1;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t));
    if (!g) return;
    auto* df = (DROPFILES*)GlobalLock(g);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* q = (wchar_t*)((char*)df + sizeof(DROPFILES));
    for (auto& s : v) { std::memcpy(q, s.c_str(), s.size() * sizeof(wchar_t)); q += s.size() + 1; }
    GlobalUnlock(g);
    SendMessageW(hwnd, WM_DROPFILES, (WPARAM)g, 0);   // the subclass calls DragFinish (frees g)
}
std::string stem_key(const std::string& path) { return gset::aperture_stem_key(path); }

} // namespace
} // namespace pcbp::gui

namespace pcbp::app {
using namespace pcbp::gui;

// Load Gerber Layers: one page. Add files / folder / zip (or drop files), layer list with
// colour swatch, detected type + side badges and format, live thumbnail of the selected layer
// (or of all visible layers), RS-274D format + aperture list in a collapsible section.
bool dlg_import_gerber(HWND, Doc& d) {
    struct Ent {
        GerberLayerRef r;
        std::shared_ptr<gb::Layer> parsed;
        std::string info;
        std::vector<std::string> cands;
        bool tried = false;
        std::string err;          // forced ('Add anyway') file that still could not be used: shown inline, never loaded
        bool force = false;       // keep even when empty / unparsable (already loaded, or 'Add anyway')
        std::string skip, skip_detail;   // set by parse_ent: moved to the skipped list
        // cached preview geometry
        std::shared_ptr<ggpu::LayerMesh> mesh;
        std::vector<D2DRun> d2d;
        const gb::Layer* geo_src = nullptr;
    };
    // files not listed as layers (not a Gerber / empty layer / parse error), shown in a Details popup
    struct Skipped { std::string path, name, reason, detail, content; };
    struct St {
        std::vector<Ent> E;
        int cur = -1;
        std::string aplist;
        std::wstring ap_path;
        std::map<int, gb::Aperture> aps;
        std::string ap_expl;
        std::map<std::string, std::string> ap_by_stem;
        std::string report;
        int ap_units = 0;
        bool apall = true;
        bool composite = false;
        bool open_fmt = false;
        char name_buf[256] = {};
        int name_for = -2;
        ImVec2 last_ws{};
        int frames = 0;
        std::vector<Skipped> skipped;
        size_t sk_mark = 0;
        bool open_skipped = false;
        // cached preview (GPU renderer, D2D fallback): re-rendered only when its key changes
        ggpu::Renderer gpu;
        bool gpu_tried = false, gpu_ok = false;
        RenderTexture d2d;
        std::string prev_key;
        void* prev_tex = nullptr;
        BBox prev_box;
        int prev_w = 0, prev_h = 0;
    } st;
    for (auto& l : d.prj.layers) { Ent e; e.r = l; e.force = true; st.E.push_back(std::move(e)); }
    for (auto& e : st.E) if (!e.r.aperture_list.empty()) { st.aplist = e.r.aperture_list; st.ap_units = units_index(e.r.units_hint); break; }
    auto reparse_list = [&]() {
        st.aps.clear();
        st.ap_expl.clear();
        if (st.aplist.empty()) return;
        auto r = gb::parse_aperture_list(st.aplist, list_units(st.ap_units));
        st.aps = std::move(r.aps);
        st.ap_expl = r.explanation;
    };
    reparse_list();
    auto is_d = [](const GerberLayerRef& l) { return !gb::looks_like_rs274x(l.content) && !gb::looks_like_excellon(l.content); };
    auto invalidate_d = [&]() { for (auto& e : st.E) if (is_d(e.r)) e.tried = false; };
    auto parse_ent = [&](Ent& e) {
        e.tried = true;
        if (!e.err.empty()) return;
        e.parsed.reset();
        e.info.clear();
        e.cands.clear();
        const GerberLayerRef& l = e.r;
        if (l.content.empty()) { e.info = "no data"; return; }
        gb::ParseOptions po;
        po.apertures = st.aps;
        if (!l.aperture_list.empty()) po.apertures = gb::parse_aperture_list(l.aperture_list, list_units(units_index(l.units_hint))).aps;
        if (is_d(l)) {
            if (po.apertures.empty()) e.info += "RS-274D without apertures: add an aperture list below.  ";
            if (!l.format.empty()) {
                Doc tmp;
                tmp.prj.layers = {l};
                gb::Format fm;
                tmp.resolve_format(0, fm);
                po.fmt = fm;
                po.force_format = true;
            }
            auto c = gb::guess_format(l.content, po.apertures);
            for (size_t i = 0; i < c.size() && i < 4; ++i)
                e.cands.push_back(c[i].fmt.describe() + "   extent " + fmt_wh(c[i].extent.w(), c[i].extent.h()) + (c[i].note.empty() ? "" : "  (" + c[i].note + ")"));
            if (l.format.empty() && !c.empty()) { po.fmt = c[0].fmt; po.force_format = true; }
        }
        auto L = std::make_shared<gb::Layer>();
        std::string err;
        if (!gb::parse(l.content, po, *L, err)) {
            e.info += "parse failed: " + err;
            if (!e.force) { e.skip = "parse error"; e.skip_detail = err; }
            return;
        }
        e.info += L->fmt.describe() + "  \xC2\xB7  " + std::to_string(L->objs.size()) + " objects (" + std::to_string(L->flash_count()) + " flashes, " +
                  std::to_string(L->draw_count()) + " draws, " + std::to_string(L->region_count()) + " regions)  \xC2\xB7  " + std::to_string(L->aps.size()) +
                  " apertures  \xC2\xB7  " + fmt_wh(L->extent.w(), L->extent.h());
        if (!L->warnings.empty()) e.info += "\n" + std::to_string(L->warnings.size()) + " warning(s), first: " + L->warnings[0];
        if (L->objs.empty()) {
            e.info = "Empty layer (valid file, no objects) - hidden by default"; e.r.visible = false;
            if (!e.force) { e.skip = "empty layer"; e.skip_detail = "valid " + std::string(L->excellon ? "Excellon" : "Gerber") + " file without any objects"; }
        }
        e.parsed = L;
        e.mesh.reset(); e.d2d.clear(); e.geo_src = nullptr;
    };
    auto ensure = [&](int i) { if (i >= 0 && i < (int)st.E.size() && !st.E[i].tried) parse_ent(st.E[i]); };

    // a file that cannot be used becomes ONE inline error row (red badge + message); never a modal
    auto add_skip = [&](const std::string& path, const std::string& reason, const std::string& detail, const std::string& content) {
        for (auto& k : st.skipped) if (k.path == path) return;
        st.skipped.push_back({path, file_stem(path) + file_ext_lower(path), reason, detail, content});
    };
    auto add_error = [&](const std::string& path, const std::string& msg) { add_skip(path, "unreadable", msg, std::string()); };
    // entries whose parse found them empty / unparsable leave the table for the skipped list
    auto sweep = [&]() {
        for (int i = 0; i < (int)st.E.size();) {
            Ent& e = st.E[i];
            if (e.skip.empty() || e.force) { ++i; continue; }
            add_skip(e.r.path, e.skip, e.skip_detail, e.r.content);
            st.E.erase(st.E.begin() + i);
            if (st.cur > i) --st.cur;
            else if (st.cur == i) st.cur = std::min(st.cur, (int)st.E.size() - 1);
        }
        if (st.cur < 0 && !st.E.empty()) st.cur = 0;
    };
    std::function<void(const std::string&, const std::string&)> add_content = [&](const std::string& path, const std::string& content) {
        const std::string ext = file_ext_lower(path);
        std::vector<uint8_t> bytes(content.begin(), content.end());
        if (io::detect_archive(bytes.data(), bytes.size()) != io::ArchiveKind::None) {
            std::vector<io::ArchiveEntry> ents;
            std::string err;
            if (!io::read_archive(bytes, ents, err, file_stem(path))) { add_error(path, "archive: " + err); return; }
            for (auto& e : ents) if (!e.dir) add_content(path + "/" + e.name, std::string(e.data.begin(), e.data.end()));
            return;
        }
        const std::string fname = file_stem(path) + ext;
        if (ext == ".gbrjob") { add_skip(path, "not a Gerber", "Gerber job file (JSON); layer functions come from the X2 attributes in each layer", content); return; }
        switch (classify(content)) {
        case FKind::Rs274x: case FKind::Rs274d: case FKind::Excellon: {
            Ent e;
            e.r.path = path;
            e.r.content = content;
            e.r.name = file_stem(path);
            guess_role(e.r, (int)st.E.size());
            st.E.push_back(std::move(e));
            break;
        }
        case FKind::ApertureList:
            st.ap_by_stem[stem_key(path)] = content;
            if (st.aplist.empty()) { st.aplist = content; st.ap_path = W(path); }
            st.report += fname + ": aperture list\n";
            break;
        case FKind::Ipc356: add_skip(path, "not a Gerber", "IPC-D-356 netlist, not a layer", content); break;
        default: add_skip(path, "not a Gerber", "not recognised as a Gerber, Excellon drill or aperture list file", content); break;
        }
    };
    auto finish_add = [&](int first_new) {
        for (int i = first_new; i < (int)st.E.size(); ++i) {
            GerberLayerRef& l = st.E[i].r;
            if (!st.E[i].err.empty() || !is_d(l)) continue;
            std::string sk = stem_key(l.path);
            auto it = st.ap_by_stem.find(sk);
            if (it == st.ap_by_stem.end())
                for (auto j = st.ap_by_stem.begin(); j != st.ap_by_stem.end(); ++j)
                    if (!j->first.empty() && (sk.rfind(j->first, 0) == 0 || j->first.rfind(sk, 0) == 0)) { it = j; break; }
            if (it != st.ap_by_stem.end()) l.aperture_list = it->second;
            else if (!st.aplist.empty()) l.aperture_list = st.aplist;
            l.units_hint = units_hint_of(st.ap_units);
            if (l.aperture_list.empty()) st.open_fmt = true;
        }
        // Default draw order for a freshly added set = physical stack seen from the top, so a
        // full inner plane (one huge region) no longer paints over the top layers:
        // bottom assy/silk/mask/paste/copper -> inner (deepest first) -> top copper/paste/mask/silk/assy
        // -> other -> outline -> drill (drawn last = on top). Error rows sink to the end.
        if (first_new < (int)st.E.size()) {
            auto rank = [](const Ent& e) -> int { return e.err.empty() ? gset::stack_rank(e.r) : 10000; };
            std::vector<std::pair<int, int>> keys;
            for (int i = first_new; i < (int)st.E.size(); ++i) keys.push_back({rank(st.E[i]), i});
            std::stable_sort(keys.begin(), keys.end(), [](auto& a, auto& b) { return a.first < b.first; });
            std::vector<Ent> tmp;
            tmp.reserve(keys.size());
            for (auto& k : keys) tmp.push_back(std::move(st.E[k.second]));
            for (size_t k = 0; k < tmp.size(); ++k) st.E[first_new + k] = std::move(tmp[k]);
        }
        bool any = false;
        for (auto& e : st.E) any |= e.r.digitize && e.err.empty();
        if (!any) {
            int best = -1;
            for (LayerRole role : {LayerRole::Paste, LayerRole::Copper, LayerRole::Mask})
                for (int i = 0; i < (int)st.E.size() && best < 0; ++i) if (st.E[i].err.empty() && st.E[i].r.role == role && st.E[i].r.side == Side::Top) best = i;
            for (int i = 0; i < (int)st.E.size() && best < 0; ++i) if (st.E[i].err.empty()) best = i;
            if (best >= 0) st.E[best].r.digitize = true;
        }
        reparse_list();
        invalidate_d();
        // parse the new files now: empty layers / parse errors go to the skipped list instead of the table
        const size_t sk0 = std::min(st.sk_mark, st.skipped.size());
        for (int i = first_new; i < (int)st.E.size(); ++i) if (!st.E[i].tried) parse_ent(st.E[i]);
        const int before_sweep = (int)st.E.size();
        sweep();
        const int added = std::max(0, (int)st.E.size() - first_new);
        (void)before_sweep;
        const size_t nsk = st.skipped.size() - sk0;
        st.report = "Added " + std::to_string(added) + " layer(s)" + (nsk ? ", " + std::to_string(nsk) + " file(s) skipped" : std::string()) + "." +
                    (st.report.empty() ? std::string() : "  " + st.report);
        while (!st.report.empty() && st.report.back() == '\n') st.report.pop_back();
        for (auto& c : st.report) if (c == '\n') c = ';';
        if (added > 0) st.cur = first_new;
    };
    auto add_paths = [&](const std::vector<std::wstring>& paths) {
        const int first = (int)st.E.size();
        st.report.clear();
        st.sk_mark = st.skipped.size();
        for (auto& p : paths) {
            const std::string u = U(p);
            io::FileTree ft;
            std::string err;
            DWORD at = GetFileAttributesW(p.c_str());
            if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) {
                if (!ft.open(u, err)) { add_error(u, err); continue; }
                for (const std::string& rel : ft.list()) { std::string c; if (ft.get(rel, c) && !c.empty()) add_content(u + "/" + rel, c); }
                continue;
            }
            std::string c;
            if (!read_text_file(u, c, err) || c.empty()) { add_error(u, err.empty() ? std::string("empty file") : err); continue; }
            add_content(u, c);
        }
        if (!paths.empty()) settings().dir_gerber = W(file_dir(U(paths.front())));
        // RS-274D files without any aperture list: look next to them (same folder + one level of
        // sub-folders, small text files only), e.g. CircuitCAM's "Aperture List/Aperture List.txt"
        bool any_d = false;
        for (int i = first; i < (int)st.E.size(); ++i) any_d |= st.E[i].err.empty() && is_d(st.E[i].r);
        if (st.aplist.empty() && any_d) {
            std::set<std::wstring> dirs;
            for (int i = first; i < (int)st.E.size(); ++i) if (st.E[i].err.empty() && is_d(st.E[i].r)) dirs.insert(W(file_dir(st.E[i].r.path)));
            int budget = 400;
            std::function<void(const std::wstring&, int)> scan_dir = [&](const std::wstring& dir, int depth) {
                WIN32_FIND_DATAW fd;
                HANDLE fh = FindFirstFileW((dir + L"\\*").c_str(), &fd);
                if (fh == INVALID_HANDLE_VALUE) return;
                do {
                    if (--budget < 0 || !st.aplist.empty()) break;
                    const std::wstring nm = fd.cFileName;
                    if (nm == L"." || nm == L"..") continue;
                    const std::wstring full = dir + L"\\" + nm;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 1) scan_dir(full, depth + 1); continue; }
                    if (fd.nFileSizeHigh || fd.nFileSizeLow > 256 * 1024 || fd.nFileSizeLow < 16) continue;
                    std::string c, e2;
                    if (!read_text_file(U(full), c, e2)) continue;
                    if (gb::classify_file(c) == FKind::ApertureList) {
                        st.aplist = c;
                        st.ap_path = full;
                        st.ap_by_stem[stem_key(U(full))] = c;
                    }
                } while (FindNextFileW(fh, &fd));
                FindClose(fh);
            };
            for (auto& dname : dirs) if (st.aplist.empty() && !dname.empty()) scan_dir(dname, 0);
            if (!st.aplist.empty()) st.report += "aperture list found: " + U(st.ap_path) + "\n";
        }
        finish_add(first);
    };
    auto pick_files = [&]() {
        std::wstring path;
        std::vector<std::wstring> paths;
        std::vector<ui::FileFilter> flt{
            {L"Gerber / drill / aperture files", L"*.gbr;*.ger;*.gbx;*.gtl;*.gbl;*.gts;*.gbs;*.gto;*.gbo;*.gtp;*.gbp;*.gko;*.gm1;*.g1;*.g2;*.g3;*.g4;*.cmp;*.sol;*.stc;*.sts;*.plc;*.pls;*.crc;*.crs;*.top;*.bot;*.smt;*.smb;*.sst;*.ssb;*.spt;*.spb;*.spl;*.pho;*.274;*.art;*.drl;*.xln;*.exc;*.drd;*.txt;*.apr;*.apt;*.rep;*.zip"},
            {L"All files (*.*)", L"*.*"}};
        if (!ui::open_file_dialog(nullptr, L"Add Gerber Files", flt, path, true, &paths)) return;
        if (paths.empty() && !path.empty()) paths.push_back(path);
        add_paths(paths);
    };
    auto pick_folder = [&]() { std::wstring path; if (ui::pick_folder_dialog(nullptr, L"Add Gerber Folder", path)) add_paths({path}); };
    auto pick_zip = [&]() {
        std::wstring path;
        if (ui::open_file_dialog(nullptr, L"Add Gerber Archive", {{L"Archives", L"*.zip;*.tgz;*.tar.gz;*.tar;*.gz"}, {L"All files", L"*.*"}}, path)) add_paths({path});
    };
    auto pick_aplist = [&]() {
        std::wstring p;
        if (!ui::open_file_dialog(nullptr, L"Aperture List", {{L"Aperture lists", L"*.txt;*.apt;*.apr;*.lst;*.rep;*.ap*"}, {L"All files (*.*)", L"*.*"}}, p)) return;
        std::string txt, err;
        if (!read_text_file(U(p), txt, err)) { st.report = "Aperture list: " + err; return; }
        st.ap_path = p;
        st.aplist = txt;
        st.ap_by_stem[stem_key(U(p))] = txt;
        for (auto& e : st.E) if (e.err.empty() && is_d(e.r) && e.r.aperture_list.empty()) e.r.aperture_list = txt;
        reparse_list();
        invalidate_d();
    };
    if (!st.E.empty()) st.cur = 0;

    // ---- preview of parsed layers (layer xform applied): rendered ONCE into an offscreen texture
    // with the workspace's GPU renderer (exact tessellation, polarity, regions with cut-ins), D2D
    // geometry as fallback; re-rendered only when the layer set, colours, geometry or size change.
    auto ensure_geo = [&](Ent& e) {
        if (!e.parsed) return;
        if (e.geo_src == e.parsed.get() && (e.mesh || !e.d2d.empty())) return;
        e.mesh.reset(); e.d2d.clear();
        e.geo_src = e.parsed.get();
        if (st.gpu_ok) e.mesh = ggpu::build_mesh(*e.parsed, {}, nullptr);
        else build_d2d(e.d2d, *e.parsed, {}, nullptr);
    };
    auto draw_thumb = [&](ImDrawList* dl, ImVec2 p, ImVec2 sz, const std::vector<int>& which) {
        const ImVec2 p1(p.x + sz.x, p.y + sz.y);
        dl->AddRectFilled(p, p1, col32(ui::pal().gerber_bg), S(8));
        BBox b;
        for (int i : which) if (st.E[i].parsed && !st.E[i].parsed->objs.empty()) b.add(xform_box(layer_xform(st.E[i].r), st.E[i].parsed->extent));
        if (b.empty()) { dl->AddText(ImVec2(p.x + S(12), p.y + S(10)), th().text_dim, which.empty() ? "Select a layer" : "No preview (see the info line)"); return; }
        if (!st.gpu_tried) {
            st.gpu_tried = true;
            std::string err;
            wchar_t ev[8] = {};
            st.gpu_ok = !(GetEnvironmentVariableW(L"PCBP_GERBER_D2D", ev, 8) && ev[0] == L'1') && st.gpu.init(err);
        }
        const int W = std::max(16, (int)sz.x), H = std::max(16, (int)sz.y);
        const float foot = S(22);
        const double sc = std::min((W - S(20)) / std::max(b.w(), 1e-3), (H - S(20) - foot) / std::max(b.h(), 1e-3));
        const double cx = b.center().x, cy = b.center().y - (foot * 0.5) / sc;
        // cache key: layers (+ geometry identity, colour, transform), size
        std::string key = std::to_string(W) + "x" + std::to_string(H) + (st.gpu_ok ? "g" : "d");
        for (int i : which) {
            const GerberLayerRef& r = st.E[i].r;
            char kb[200];
            std::snprintf(kb, sizeof kb, "|%p:%08x:%.6f,%.6f,%.6f,%d", (const void*)st.E[i].parsed.get(), (unsigned)r.color, r.offset.x, r.offset.y, r.rot, (int)r.mirror);
            key += kb;
        }
        if (key != st.prev_key) {
            st.prev_key = key;
            st.prev_tex = nullptr;
            const double alpha = which.size() > 1 ? 0.78 : 1.0;
            if (st.gpu_ok) {
                ggpu::View v;
                v.w = W; v.h = H; v.scale = sc; v.cx = cx; v.cy = cy;
                if (st.gpu.begin(v, ui::pal().gerber_bg)) {
                    for (int i : which) {
                        Ent& e = st.E[i];
                        ensure_geo(e);
                        if (e.mesh) st.gpu.draw_layer(*e.mesh, layer_xform(e.r), e.r.color | 0xFF000000u, (float)alpha, ggpu::FillMode::Solid, e.parsed->negative);
                    }
                    st.gpu.end();
                    st.prev_tex = st.gpu.texture();
                } else st.gpu_ok = false;
            }
            if (!st.gpu_ok) {
                st.d2d.ensure(W, H);
                if (ID2D1DeviceContext* rt = st.d2d.begin(1.0f)) {
                    const uint32_t bg = ui::pal().gerber_bg;
                    rt->Clear(ui::d2c(bg));
                    const Xform Vx = Xform::translate(W * 0.5, H * 0.5) * Xform::scale(sc, -sc) * Xform::translate(-cx, -cy);
                    const D2D1_MATRIX_3X2_F Wm = D2D1::Matrix3x2F((float)Vx.a, (float)Vx.b, (float)Vx.c, (float)Vx.d, (float)Vx.tx, (float)Vx.ty);
                    Com<ID2D1SolidColorBrush> br;
                    rt->CreateSolidColorBrush(ui::d2c(0xFFFFFFFF), br.put());
                    const double hair = 1.0 / std::max(sc, 1e-6);
                    for (int i : which) {
                        Ent& e = st.E[i];
                        ensure_geo(e);
                        const Xform x = layer_xform(e.r);
                        rt->SetTransform(D2D1::Matrix3x2F((float)x.a, (float)x.b, (float)x.c, (float)x.d, (float)x.tx, (float)x.ty) * Wm);
                        const uint32_t col = e.r.color | 0xFF000000u;
                        for (const D2DRun& run : e.d2d) {
                            br->SetColor(ui::d2c(run.clear ? bg : col));
                            br->SetOpacity(run.clear ? 1.f : (float)alpha);
                            if (run.fill) rt->FillGeometry(run.fill.get(), br.get());
                            for (const D2DStroke& sk : run.strokes) rt->DrawGeometry(sk.geo.get(), br.get(), (float)std::max(sk.w, hair), stroke_style(sk.square));
                            if (run.holes) { br->SetColor(ui::d2c(bg)); br->SetOpacity(1.f); rt->FillGeometry(run.holes.get(), br.get()); }
                        }
                    }
                    rt->SetTransform(D2D1::Matrix3x2F::Identity());
                    st.d2d.end();
                    st.prev_tex = st.d2d.imgui_id();
                }
            }
            st.prev_box = b;
        }
        if (st.prev_tex) dl->AddImageRounded((ImTextureID)st.prev_tex, p, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, S(8));
        std::string ext = "extent " + fmt_wh(b.w(), b.h()) + (which.size() > 1 ? "  \xC2\xB7  " + std::to_string(which.size()) + " layers" : std::string());
        dl->AddText(ImVec2(p.x + S(10), p1.y - S(20)), th().text_dim, ext.c_str());
    };

    // Fixed initial size clamped to the work area; the body is a user-resizable child (no width
    // feedback: nothing inside is sized from the dialog's own width), the dialog re-centres when
    // its size changes.
    const ImGuiViewport* vp0 = ImGui::GetMainViewport();
    const float sc1 = S(1.f);
    const int dlg_w = (int)std::clamp((vp0->WorkSize.x * 0.94f - S(40)) / sc1, 520.f, 1000.f);
    const float body_h = std::clamp(vp0->WorkSize.y * 0.90f - S(210), S(300), S(640));
    ui::Form f(L"Load Gerber Layers", dlg_w);
    f.icon((int)Icon::ImportGerber);
    f.header(L"Load Gerber and drill layers");
    f.subtitle(L"Types and sides are detected from file names and contents; click a badge to change it.");
    static const wchar_t* s_test_drop = _wgetenv(L"PCBP_TEST_DROP");
    f.custom("main", [&](ui::Form& F, float) {
        const GTheme& T = th();
        {   // keep the dialog centred and inside the work area whenever its size changes
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const ImVec2 ws = ImGui::GetWindowSize();
            if (std::fabs(ws.x - st.last_ws.x) > 0.5f || std::fabs(ws.y - st.last_ws.y) > 0.5f) {
                st.last_ws = ws;
                ImVec2 c(vp->WorkPos.x + (vp->WorkSize.x - ws.x) * 0.5f, vp->WorkPos.y + (vp->WorkSize.y - ws.y) * 0.5f);
                c.x = std::max(c.x, vp->WorkPos.x); c.y = std::max(c.y, vp->WorkPos.y);
                ImGui::SetWindowPos(c);
            }
            ImGui::SetNextWindowSizeConstraints(ImVec2(S(480), S(260)), ImVec2(vp->WorkSize.x * 0.94f - S(40), vp->WorkSize.y * 0.92f - S(200)));
        }
        ImGui::BeginChild("##gdlgbody", ImVec2(S((float)dlg_w), body_h), ImGuiChildFlags_ResizeX | ImGuiChildFlags_ResizeY, ImGuiWindowFlags_NoSavedSettings);
        const float W0 = ImGui::GetContentRegionAvail().x - S(2);
        const float H0 = ImGui::GetContentRegionAvail().y;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (s_test_drop && ++st.frames == 8) test_drop_post(main_hwnd(), s_test_drop);
        // dropped files (OLE drop target / WM_DROPFILES subclass while this dialog is open)
        if (!g_drop_queue.empty()) { auto q = std::move(g_drop_queue); g_drop_queue.clear(); F.later([&, q]() { add_paths(q); }); }
        // parse the remaining layers one per frame so failures show up as red badges right away
        for (auto& e : st.E) if (!e.tried) { parse_ent(e); request_redraw(2); break; }
        sweep();
        // ---- drop zone + add buttons
        const float bh = S(64), bw = S(92);
        const float zw = std::max(S(160), W0 - (bw + S(8)) * 3);
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##drop", ImVec2(zw, bh));
            const bool drag = g_drag_hover;
            if (drag) request_redraw(2);
            const bool hv = ImGui::IsItemHovered() || drag;
            dl->AddRectFilled(p, ImVec2(p.x + zw, p.y + bh), drag ? col_alpha(T.accent, 0.18f + 0.10f * pulse(1.2f)) : hv ? T.raised : T.panel2, S(10));
            if (drag) dl->AddRect(p, ImVec2(p.x + zw, p.y + bh), T.accent, S(10), 0, S(2.5f));
            // dashed border
            const ImU32 bc = hv ? T.accent : T.border;
            const float dash = S(7);
            for (float x = p.x + S(10); x < p.x + zw - S(10); x += dash * 2) {
                dl->AddLine(ImVec2(x, p.y + S(1)), ImVec2(std::min(x + dash, p.x + zw - S(10)), p.y + S(1)), bc, S(1.5f));
                dl->AddLine(ImVec2(x, p.y + bh - S(1)), ImVec2(std::min(x + dash, p.x + zw - S(10)), p.y + bh - S(1)), bc, S(1.5f));
            }
            for (float y = p.y + S(10); y < p.y + bh - S(10); y += dash * 2) {
                dl->AddLine(ImVec2(p.x + S(1), y), ImVec2(p.x + S(1), std::min(y + dash, p.y + bh - S(10))), bc, S(1.5f));
                dl->AddLine(ImVec2(p.x + zw - S(1), y), ImVec2(p.x + zw - S(1), std::min(y + dash, p.y + bh - S(10))), bc, S(1.5f));
            }
            icon_draw(dl, Icon::ImportGerber, ImVec2(p.x + S(18), p.y + (bh - S(32)) / 2), S(32));
            ImGui::PushFont(g_font_bold);
            dl->PushClipRect(p, ImVec2(p.x + zw - S(6), p.y + bh), true);
            dl->AddText(ImVec2(p.x + S(62), p.y + S(13)), T.text, drag ? "Release to add the files" : "Drop Gerber / drill files, folders or a .zip here");
            ImGui::PopFont();
            dl->AddText(ImVec2(p.x + S(62), p.y + S(35)), T.text_dim, "or click to browse files  \xC2\xB7  aperture lists are paired with RS-274D files automatically");
            dl->PopClipRect();
            if (clicked) F.later(pick_files);
        }
        auto big = [&](const char* id, Icon ic, const char* label, const char* tip) {
            ImGui::SameLine(0, S(8));
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool r = ImGui::InvisibleButton(id, ImVec2(bw, bh));
            const bool hv = ImGui::IsItemHovered();
            dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + bh), hv ? T.raised : T.panel2, S(10));
            dl->AddRect(p, ImVec2(p.x + bw, p.y + bh), hv ? T.accent : T.border, S(10));
            icon_draw(dl, ic, ImVec2(p.x + (bw - S(24)) / 2, p.y + S(9)), S(24));
            ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(p.x + (bw - ts.x) / 2, p.y + S(39)), T.text, label);
            if (hv) tooltip(tip);
            return r;
        };
        if (big("##bfiles", Icon::Open, "Files...", "Add Gerber, drill and aperture list files")) F.later(pick_files);
        if (big("##bfolder", Icon::Layers, "Folder...", "Add every Gerber / drill file in a folder")) F.later(pick_folder);
        if (big("##bzip", Icon::Export, "Zip...", "Add every Gerber / drill file in a .zip / .tgz archive")) F.later(pick_zip);
        ImGui::Spacing();
        // ---- skipped files: one compact warning strip + Details popup (never rows in the table)
        float strip_h = 0;
        if (!st.skipped.empty()) {
            const ImVec2 sp = ImGui::GetCursorScreenPos();
            strip_h = S(28);
            dl->AddRectFilled(sp, ImVec2(sp.x + W0, sp.y + strip_h), col_alpha(T.warn, 0.14f), S(7));
            dl->AddRect(sp, ImVec2(sp.x + W0, sp.y + strip_h), col_alpha(T.warn, 0.55f), S(7));
            icon_draw(dl, Icon::Warning, ImVec2(sp.x + S(8), sp.y + (strip_h - S(16)) / 2), S(16));
            std::map<std::string, int> by;
            for (auto& k : st.skipped) by[k.reason]++;
            std::string msg = std::to_string(st.skipped.size()) + (st.skipped.size() == 1 ? " file skipped" : " files skipped") + "  (";
            bool first = true;
            for (auto& [r, c] : by) { msg += (first ? "" : ", ") + std::to_string(c) + " " + r; first = false; }
            msg += ")";
            ImGui::SetCursorScreenPos(ImVec2(sp.x + S(30), sp.y + (strip_h - ImGui::GetTextLineHeight()) / 2));
            ImGui::PushStyleColor(ImGuiCol_Text, T.text);
            ImGui::TextUnformatted(msg.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(0, S(10));
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, sp.y + S(3)));
            if (ImGui::SmallButton("Details...")) st.open_skipped = true;
            ImGui::SetCursorScreenPos(ImVec2(sp.x, sp.y + strip_h + S(4)));
            ImGui::Dummy(ImVec2(0, 0));
        }
        if (st.open_skipped) { ImGui::OpenPopup("Skipped files##gsk"); st.open_skipped = false; }
        ImGui::SetNextWindowSizeConstraints(ImVec2(S(560), S(160)), ImVec2(S(900), S(560)));
        if (ImGui::BeginPopupModal("Skipped files##gsk", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
            ImGui::TextWrapped("These files were not added as layers. If one is a Gerber we fail to recognise, use 'Add anyway' (it is parsed as Gerber / Excellon) and send us the report.");
            ImGui::PopStyleColor();
            int add_i = -1;
            if (ImGui::BeginTable("##skt", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                                  ImVec2(S(820), std::min(S(380), S(30) + S(28) * (float)st.skipped.size())))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthFixed, S(230));
                ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthFixed, S(110));
                ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(96));
                ImGui::TableHeadersRow();
                for (int k = 0; k < (int)st.skipped.size(); ++k) {
                    const Skipped& sk = st.skipped[k];
                    ImGui::PushID(k);
                    ImGui::TableNextRow(0, S(26));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(sk.name.c_str());
                    if (ImGui::IsItemHovered()) tooltip(sk.path.c_str());
                    ImGui::TableNextColumn();
                    const ImU32 rc = sk.reason == "parse error" || sk.reason == "unreadable" ? T.error : sk.reason == "empty layer" ? T.text_dim : T.warn;
                    badge("##rs", sk.reason, rc, nullptr, S(100));
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
                    ImGui::TextUnformatted(sk.detail.c_str());
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered() && !sk.detail.empty()) tooltip(sk.detail.c_str());
                    ImGui::TableNextColumn();
                    if (button("Add anyway", Icon::Plus, false, ImVec2(S(92), S(22)), !sk.content.empty())) add_i = k;
                    if (sk.content.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip("The file could not be read");
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (add_i >= 0) {
                Skipped sk = st.skipped[add_i];
                st.skipped.erase(st.skipped.begin() + add_i);
                Ent e;
                e.r.path = sk.path;
                e.r.content = sk.content;
                e.r.name = file_stem(sk.path);
                e.force = true;
                guess_role(e.r, (int)st.E.size());
                st.E.push_back(std::move(e));
                parse_ent(st.E.back());
                if (!st.E.back().parsed) st.E.back().err = "parse failed: " + st.E.back().info;
                st.cur = (int)st.E.size() - 1;
            }
            ImGui::Spacing();
            if (button("Copy report", Icon::Copy)) {
                std::string rep = "PCB Plotter - Load Gerber Layers: skipped files\n";
                for (auto& sk : st.skipped) {
                    rep += "\n" + sk.path + "\n  reason: " + sk.reason + (sk.detail.empty() ? std::string() : "\n  detail: " + sk.detail) +
                           "\n  size: " + std::to_string(sk.content.size()) + " bytes";
                    if (!sk.content.empty()) {
                        std::string head = sk.content.substr(0, 240);
                        for (auto& c : head) if ((unsigned char)c < 32 && c != '\n') c = '.';
                        rep += "\n  head: " + head;
                        if (sk.content.size() > 240) rep += "...";
                    }
                    rep += "\n";
                }
                ImGui::SetClipboardText(rep.c_str());
                toast("Report copied to the clipboard", ToastKind::Success, 3.f);
            }
            ImGui::SameLine();
            if (button("Close", Icon::Check, true)) ImGui::CloseCurrentPopup();
            if (st.skipped.empty()) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // ---- layer list (left) + preview (right)
        const float listh = std::max(S(170), H0 - bh - S(150) - strip_h), prevw = std::floor(W0 * 0.4f), listw = W0 - prevw - S(10);
        const int n = (int)st.E.size();
        ImGui::BeginGroup();
        {
            // list toolbar
            ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
            ImGui::Text("%d layer%s", n, n == 1 ? "" : "s");
            ImGui::PopStyleColor();
            ImGui::SameLine(listw - (S(24) * 6 + S(8)));
            const bool hs = st.cur >= 0 && st.cur < n;
            if (arrow_button("##iu", true, "Move up", hs && st.cur > 0, S(22))) { std::swap(st.E[st.cur], st.E[st.cur - 1]); --st.cur; }
            ImGui::SameLine(0, S(2));
            if (arrow_button("##id", false, "Move down", hs && st.cur + 1 < n, S(22))) { std::swap(st.E[st.cur], st.E[st.cur + 1]); ++st.cur; }
            ImGui::SameLine(0, S(2));
            if (icon_button("##iauto", Icon::Question, "Auto-detect type and side of all layers again", false, n > 0, S(22)))
                for (int i = 0; i < n; ++i) guess_role(st.E[i].r, i);
            ImGui::SameLine(0, S(2));
            if (icon_button("##irm", Icon::Delete, "Remove the selected layer", false, hs, S(22)) && hs) { st.E.erase(st.E.begin() + st.cur); st.cur = std::min(st.cur, (int)st.E.size() - 1); }
            ImGui::SameLine(0, S(2));
            if (icon_button("##irmall", Icon::Unassign, "Remove all layers", false, n > 0, S(22))) { st.E.clear(); st.cur = -1; }
            ImGui::SameLine(0, S(2));
            {
                int nbad = 0;
                for (auto& e : st.E) nbad += !e.err.empty() || (e.tried && !e.parsed);
                if (icon_button("##irmbad", Icon::Eraser, "Remove the files that cannot be loaded (red rows)", false, nbad > 0, S(22))) {
                    st.E.erase(std::remove_if(st.E.begin(), st.E.end(), [](const Ent& e) { return !e.err.empty() || (e.tried && !e.parsed); }), st.E.end());
                    st.cur = st.E.empty() ? -1 : 0;
                }
            }
        }
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(4), S(3)));
        if (ImGui::BeginTable("##il", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingFixedFit, ImVec2(listw, listh - S(30)))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(20));
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(14));
            ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, S(62));
            ImGui::TableSetupColumn("Side", ImGuiTableColumnFlags_WidthFixed, S(40));
            ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, S(118));
            ImGui::TableHeadersRow();
            if (n == 0) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TableNextColumn(); ImGui::TableNextColumn();
                ImGui::TextDisabled("No layers yet: add files above.");
            }
            for (int i = 0; i < (int)st.E.size(); ++i) {
                GerberLayerRef& l = st.E[i].r;
                ImGui::PushID(i);
                ImGui::TableNextRow(0, S(24));
                if (!st.E[i].err.empty()) {
                    // unusable file: one red row with the reason (shown once, never a modal)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, col_alpha(T.error, 0.10f));
                    ImGui::TableNextColumn();
                    icon_draw(ImGui::GetWindowDrawList(), Icon::ErrorX, ImVec2(ImGui::GetCursorScreenPos().x + S(2), ImGui::GetCursorScreenPos().y + S(2)), S(16));
                    ImGui::Dummy(ImVec2(S(20), S(20)));
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(l.name.c_str(), i == st.cur, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, S(20)))) st.cur = i;
                    if (ImGui::IsItemHovered()) tooltip((l.path + "\n" + st.E[i].err).c_str());
                    ImGui::TableNextColumn();
                    badge("##eb", "ERROR", T.error, st.E[i].err.c_str(), S(58));
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, T.error);
                    ImGui::TextUnformatted(st.E[i].err.c_str());
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) tooltip(st.E[i].err.c_str());
                    ImGui::PopID();
                    continue;
                }
                ImGui::TableNextColumn();
                if (icon_button("##v", l.visible ? Icon::Eye : Icon::EyeOff, "Visible after loading", false, true, S(20))) l.visible = !l.visible;
                ImGui::TableNextColumn();
                {
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    p.y += S(4);
                    const float sz = S(13);
                    dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), col32(l.color | 0xFF000000u), S(3));
                    ImGui::InvisibleButton("##sw", ImVec2(sz, S(20)));
                    if (ImGui::IsItemHovered()) tooltip("Colour (click to change)");
                    if (ImGui::IsItemClicked()) ImGui::OpenPopup("##cp");
                    if (ImGui::BeginPopup("##cp")) {
                        float c[3] = {((l.color >> 16) & 255) / 255.f, ((l.color >> 8) & 255) / 255.f, (l.color & 255) / 255.f};
                        if (ImGui::ColorPicker3("##p", c, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview))
                            l.color = 0xFF000000u | ((uint32_t)(c[0] * 255 + .5f) << 16) | ((uint32_t)(c[1] * 255 + .5f) << 8) | (uint32_t)(c[2] * 255 + .5f);
                        if (ImGui::Button("By layer type")) l.color = role_color(l.role, i);
                        ImGui::EndPopup();
                    }
                }
                ImGui::TableNextColumn();
                if (ImGui::Selectable(l.name.c_str(), i == st.cur, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, S(20)))) st.cur = i;
                if (ImGui::IsItemHovered()) {
                    gb::LayerGuess g = layer_guess(l);
                    tooltip((l.path + (g.known ? "\nDetected: " + g.why : std::string("\nNot recognised from the name: set type and side")) ).c_str());
                }
                ImGui::TableNextColumn();
                if (badge("##rb", role_abbr(l.role), col32(role_color(l.role, i)), "Layer type (click to change)", S(58))) { st.cur = i; ImGui::OpenPopup("##rp"); }
                if (ImGui::BeginPopup("##rp")) {
                    for (size_t r = 0; r < role_names().size(); ++r)
                        if (ImGui::MenuItem(U(role_names()[r]).c_str(), nullptr, (size_t)l.role == r)) { l.role = (LayerRole)r; l.color = role_color(l.role, i); }
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                {
                    const std::string sb = side_badge(l);
                    const ImU32 sc = sb == "BOT" ? IM_COL32(80, 150, 255, 255) : sb == "TOP" ? IM_COL32(230, 120, 60, 255) : IM_COL32(150, 150, 150, 255);
                    if (badge("##sb", sb, sc, "Side (click to toggle top / bottom)", S(36))) l.side = l.side == Side::Bottom ? Side::Top : Side::Bottom;
                }
                ImGui::TableNextColumn();
                {
                    const std::string ft = layer_fmt_u8(l);
                    const bool bad = is_d(l) && l.aperture_list.empty() && st.aplist.empty();
                    if (st.E[i].tried && !st.E[i].parsed) {
                        badge("##pf", "FAILED", T.error, st.E[i].info.c_str(), S(58));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text, bad ? T.warn : T.text_dim);
                        ImGui::TextUnformatted(ft.c_str());
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) tooltip(bad ? "RS-274D needs an aperture list: open 'Format & apertures' below" : "File format");
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        ImGui::EndGroup();
        ImGui::SameLine(0, S(10));
        ImGui::BeginGroup();
        {
            ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
            ImGui::TextUnformatted("Preview");
            ImGui::PopStyleColor();
            ImGui::SameLine(prevw - S(150));
            auto seg = [&](const char* lb, bool on) {
                ImGui::PushStyleColor(ImGuiCol_Button, on ? T.accent : T.raised);
                ImGui::PushStyleColor(ImGuiCol_Text, on ? T.accent_text : T.text);
                bool r = ImGui::SmallButton(lb);
                ImGui::PopStyleColor(2);
                return r;
            };
            if (seg("Selected", !st.composite)) st.composite = false;
            ImGui::SameLine(0, S(2));
            if (seg("All visible", st.composite)) st.composite = true;
            std::vector<int> which;
            if (st.composite) { for (int i = 0; i < (int)st.E.size(); ++i) if (st.E[i].r.visible && st.E[i].err.empty()) { ensure(i); which.push_back(i); } }
            else if (st.cur >= 0 && st.cur < (int)st.E.size()) { ensure(st.cur); which.push_back(st.cur); }
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(prevw, listh - S(30)));
            draw_thumb(ImGui::GetWindowDrawList(), p, ImVec2(prevw, listh - S(30)), which);
        }
        ImGui::EndGroup();

        // ---- selected layer details
        if (st.cur >= 0 && st.cur < (int)st.E.size()) {
            Ent& e = st.E[st.cur];
            ensure(st.cur);
            if (st.name_for != st.cur) { std::snprintf(st.name_buf, sizeof st.name_buf, "%s", e.r.name.c_str()); st.name_for = st.cur; }
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Name");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(S(220));
            if (ImGui::InputText("##nm", st.name_buf, sizeof st.name_buf)) e.r.name = st.name_buf;
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, T.text_faint);
            ImGui::TextUnformatted(e.r.path.c_str());
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, e.parsed ? T.text_dim : T.warn);
            ImGui::TextWrapped("%s", e.info.c_str());
            ImGui::PopStyleColor();
        } else ImGui::Dummy(ImVec2(0, S(40)));

        // ---- format & apertures (collapsible)
        if (st.open_fmt) { ImGui::SetNextItemOpen(true); st.open_fmt = false; }
        if (ImGui::CollapsingHeader("Format & apertures  (RS-274D / legacy files only)")) {
            const bool hs = st.cur >= 0 && st.cur < (int)st.E.size();
            if (hs && is_d(st.E[st.cur].r)) {
                Ent& e = st.E[st.cur];
                GerberLayerRef& l = e.r;
                gb::Format fm;
                bool ex = false;
                if (!l.format.empty()) { Doc tmp; tmp.prj.layers = {l}; ex = !tmp.resolve_format(0, fm).empty(); }
                ImGui::AlignTextToFramePadding();
                ImGui::Text("Coordinate format of %s:", l.name.c_str());
                ImGui::SameLine();
                bool changed = false;
                int mode = ex ? 1 : 0;
                if (ImGui::RadioButton("Auto (best guess)", mode == 0) && mode != 0) { l.format.clear(); changed = true; }
                ImGui::SameLine();
                if (ImGui::RadioButton("Explicit", mode == 1) && mode != 1) {
                    if (!e.cands.empty() || true) {
                        auto c = gb::guess_format(l.content, st.aps);
                        gb::Format g = c.empty() ? gb::Format{} : c[0].fmt;
                        l.format = fmt_string(g);
                    }
                    changed = true;
                }
                if (ex) {
                    int u = fm.inch ? 0 : 1, id = fm.int_digits, fd = fm.frac_digits, z = fm.lead_supp ? 0 : 1;
                    ImGui::SetNextItemWidth(S(90));
                    changed |= ImGui::Combo("Units", &u, "Inch\0mm\0");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(S(90));
                    changed |= ImGui::InputInt("Integer digits", &id);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(S(90));
                    changed |= ImGui::InputInt("Decimals", &fd);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(S(110));
                    changed |= ImGui::Combo("Zero suppression", &z, "Leading\0Trailing\0");
                    if (changed && !l.format.empty()) {
                        fm.inch = u == 0; fm.int_digits = std::clamp(id, 1, 7); fm.frac_digits = std::clamp(fd, 1, 7); fm.lead_supp = z == 0;
                        l.format = fmt_string(fm);
                    }
                }
                if (!e.cands.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
                    ImGui::TextUnformatted("Best guesses:");
                    for (size_t k = 0; k < e.cands.size(); ++k) ImGui::BulletText("%s", e.cands[k].c_str());
                    ImGui::PopStyleColor();
                }
                if (changed) e.tried = false;
            } else {
                ImGui::TextDisabled(hs ? "The selected layer defines its own format (RS-274X / Excellon)." : "Select a layer.");
            }
            ImGui::Separator();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Aperture list:");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, st.aplist.empty() ? T.warn : T.text_dim);
            ImGui::TextUnformatted(st.aplist.empty() ? "none" : (st.ap_path.empty() ? std::string("(paired by name)") : U(st.ap_path)).c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (button("Browse...", Icon::Open)) F.later(pick_aplist);
            ImGui::SameLine();
            if (button("Clear", Icon::Delete, false, ImVec2(0, 0), !st.aplist.empty())) { st.aplist.clear(); st.ap_path.clear(); st.aps.clear(); st.ap_expl.clear(); invalidate_d(); }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(S(80));
            if (ImGui::Combo("List units", &st.ap_units, "Auto\0mil\0inch\0mm\0")) {
                reparse_list();
                for (auto& e : st.E) if (is_d(e.r)) e.r.units_hint = units_hint_of(st.ap_units);
                invalidate_d();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Use for 274D layers without their own list", &st.apall);
            if (!st.aplist.empty()) {
                std::string t = st.ap_expl + "\n" + gb::aperture_list_to_text(st.aps);
                ImGui::PushFont(g_font_mono);
                ImGui::InputTextMultiline("##apv", t.data(), t.size() + 1, ImVec2(-FLT_MIN, S(110)), ImGuiInputTextFlags_ReadOnly);
                ImGui::PopFont();
            }
        }
        // ---- status line
        if (!st.report.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim);
            ImGui::TextWrapped("%s", st.report.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
    }, 0);
    f.ok_text(L"Load Layers");
    f.validate([&](ui::Form&, std::wstring& err) {
        bool any = false;
        for (auto& e : st.E) any |= e.err.empty();
        if (!any && d.prj.layers.empty()) { err = L"Add at least one Gerber or drill file."; return false; }
        return true;
    });
    if (!g_preload.empty()) { auto q = std::move(g_preload); g_preload.clear(); add_paths(q); }
    bool ok;
    {
        DropScope drop_scope;
        ok = f.run(nullptr);
    }
    if (!ok) return false;

    std::vector<GerberLayerRef> out;
    for (auto& e : st.E) if (e.err.empty()) out.push_back(std::move(e.r));
    for (auto& l : out) {
        const bool d274 = is_d(l);
        if (d274 && l.aperture_list.empty() && st.apall) l.aperture_list = st.aplist;
        if (d274) l.units_hint = units_hint_of(st.ap_units);
    }
    bool dig = false;
    for (auto& l : out) { if (l.digitize && dig) l.digitize = false; dig |= l.digitize; }

    d.checkpoint("Load Gerber Layers");
    d.prj.layers = std::move(out);
    d.glayers.assign(d.prj.layers.size(), nullptr);
    std::string errs;
    int nerr = 0;
    for (size_t i = 0; i < d.prj.layers.size(); ++i) {
        std::string err;
        if (!d.parse_layer(i, err)) { ++nerr; if (errs.size() < 300) errs += (errs.empty() ? "" : "; ") + d.prj.layers[i].name + ": " + err; }
    }
    d.changed(ui::CH_LAYERS | ui::CH_GEOMETRY);
    // one notification, never a modal (a modal per failed file is what trapped users in an error loop)
    if (nerr) toast(std::to_string(nerr) + " layer(s) could not be parsed: " + errs, ToastKind::Warning, 8.f);
    gui::status("Loaded " + std::to_string(d.prj.layers.size()) + " Gerber layer(s).");
    return true;
}

} // namespace pcbp::app

namespace pcbp::gui {
// Files dropped on the main window while the Load Gerber Layers dialog is open are added to it.
// Call from the shell's WM_DROPFILES handler; returns true when the dialog consumed them.
// When the dialog is closed, a drop made only of Gerber / drill / aperture files (or folders /
// archives containing them, and nothing another importer claims) opens the dialog with them.
bool gerber_dialog_drop(const std::vector<std::wstring>& paths) {
    if (g_drop_active) {
        g_drop_queue.insert(g_drop_queue.end(), paths.begin(), paths.end());
        request_redraw(2);
        return true;
    }
    if (paths.empty() || !app::active_doc()) return false;
    for (auto& p : paths) {
        const std::string u = U(p);
        const DWORD at = GetFileAttributesW(p.c_str());
        if (at == INVALID_FILE_ATTRIBUTES) return false;
        bool gerber = false;
        if (at & FILE_ATTRIBUTE_DIRECTORY) {
            io::FileTree ft;
            std::string err;
            if (!ft.open(u, err)) return false;
            int n = 0;
            for (const std::string& rel : ft.list()) {
                if (++n > 64) break;
                std::string c;
                if (ft.get(rel, c) && c.size() < (64u << 20)) { auto k = gb::classify_file(c); if (k == gb::FileKind::Rs274x || k == gb::FileKind::Rs274d || k == gb::FileKind::Excellon) { gerber = true; break; } }
            }
        } else {
            std::string c, err;
            if (!read_text_file(u, c, err)) return false;
            std::vector<uint8_t> b(c.begin(), c.end());
            if (io::detect_archive(b.data(), b.size()) != io::ArchiveKind::None) {
                if (PluginRegistry::get().detect(u)) return false;   // a project / CAD archive: the shell opens it
                io::FileTree ft;
                if (!ft.open(u, err)) return false;
                for (const std::string& rel : ft.list()) {
                    std::string cc;
                    if (ft.get(rel, cc)) { auto k = gb::classify_file(cc); if (k == gb::FileKind::Rs274x || k == gb::FileKind::Rs274d || k == gb::FileKind::Excellon) { gerber = true; break; } }
                }
            } else {
                const auto k = gb::classify_file(c);
                gerber = k == gb::FileKind::Rs274x || k == gb::FileKind::Rs274d || k == gb::FileKind::Excellon ||
                         (k == gb::FileKind::ApertureList && !PluginRegistry::get().detect(u));
            }
        }
        if (!gerber) return false;
    }
    g_preload = paths;
    post_command(ID_IMPORT_GERBER);
    return true;
}
} // namespace pcbp::gui
