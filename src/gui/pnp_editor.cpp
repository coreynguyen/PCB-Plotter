// ============================================================================
// pnp_editor.cpp — Pick & Place Program Editor (Files workspace document tab).
//
// Opens JUKI HLC (.h8h/.h7h/.h5h/.h4h/.e4x/.e51) and ISS programs through
// io::PnpProgram (src/io/pnp_program.*): unedited programs save byte-identical.
//   left   board preview in machine coordinates (GCanvas): board, circuits,
//          marks (global / local / bad), placements as rotated bodies or dots
//          with an angle tick, origin + transport direction; hover, click, band
//   right  tabs: Placements / Parts / Circuits / Marks / Board / Validation
//          (inline edit, multi-select, bulk transform / skip / sort / delete /
//          duplicate, find), snapshot undo / redo
//   tools  Save / Save As (same format or converted, lossy warning), CSV,
//          Create PCB Plotter project (juki_import), Compare with project.
// ============================================================================
#include "file_editors.h"
#include "gcanvas.h"
#include "../app/app.h"
#include "../app/commands.h"
#include "../core/fileio.h"
#include "../core/machine_job.h"
#include "../core/project.h"
#include "../io/juki_import.h"
#include "../io/pnp_program.h"
#include "../ui/d2d.h"
#include "../ui/form.h"
#include "../ui/theme.h"
#include "../ui/win.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <cctype>

namespace pcbp::app { extern bool g_quiet_import; }

namespace pcbp::gui {

namespace {

using io::PnpProgram;
using io::PnpPlacement;
using io::PnpPart;
using io::MarkKind;

std::string f4(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.4f", v);
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}
bool parse_d(const char* s, double& v) {
    while (*s == ' ') ++s;
    char* e = nullptr;
    double x = std::strtod(s, &e);
    if (e == s) return false;
    v = x;
    return true;
}
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool icontains(const std::string& h, const std::string& n) { return n.empty() || lower(h).find(lower(n)) != std::string::npos; }
std::string ext_of(const std::wstring& p) {
    std::string u = ui::U(p);
    size_t k = u.find_last_of('.'), s = u.find_last_of("\\/");
    if (k == std::string::npos || (s != std::string::npos && k < s)) return {};
    return lower(u.substr(k + 1));
}

class PnpDoc;
std::vector<PnpDoc*>& registry() { static std::vector<PnpDoc*> v; return v; }

// ---------------------------------------------------------------------------
// canvas
// ---------------------------------------------------------------------------
class PnpCanvas : public GCanvas {
public:
    PnpDoc* d = nullptr;
    int hover = -1;
    BBox world_extent() const override;
protected:
    void paint_world(ID2D1DeviceContext* rt) override;
    void paint_overlay(ImDrawList* dl) override;
    void on_click(Vec2 w, int mods) override;
    void on_dblclick(Vec2 w, int mods) override;
    void on_rect(const BBox& w, int mods) override;
    void on_context(Vec2 w) override;
    bool on_key(ImGuiKey k, int mods) override;
};

enum Tab { T_PLACE, T_PARTS, T_CIRC, T_MARKS, T_BOARD, T_VALID, T_COUNT };

struct CmpRow { std::string ref, kind, detail; int row = -1; };

// ---------------------------------------------------------------------------
// document
// ---------------------------------------------------------------------------
class PnpDoc final : public FileDoc {
public:
    PnpProgram prog;
    struct Snap { PnpProgram p; std::string label; };
    std::vector<Snap> undo_, redo_;
    uint64_t gen = 1, saved_gen = 1, issues_gen = 0, sel_gen = 1, cv_gen = 0, cv_sel = 0;
    std::vector<uint8_t> sel;            // placements
    int cursor = -1;                     // last clicked placement
    int scroll_to = -1;                  // table row to bring into view (placement index)
    int part_sel = -1, mark_sel = -1, circ_sel = -1;
    int tab = T_PLACE, want_tab = -1;
    char find[96] = "";
    std::vector<int> view;               // visible placement rows (filtered + sorted)
    uint64_t view_gen = ~0ull;
    std::string view_find;
    int sort_col = -1; bool sort_desc = false;
    // inline editing
    int ed_tab = -1, ed_row = -1, ed_col = -1;
    char ebuf[256] = "";
    bool ed_focus = false;
    // board form scratch
    io::PnpBoard bform;
    uint64_t bform_gen = ~0ull;
    // canvas
    PnpCanvas cv;
    float split = 0.46f;
    bool show_labels = true, show_ghosts = true, show_marks = true, show_ckt = true;
    bool fit_pending = true;
    std::vector<io::PnpIssue> issues;
    std::string msg;
    // transform popup
    float t_dx = 0, t_dy = 0, t_rot = 0;
    int t_about = 0;                    // 0 selection centre, 1 origin, 2 each placement
    bool t_mx = false, t_my = false;
    // compare
    std::vector<CmpRow> cmp;
    std::string cmp_title;
    bool cmp_open = false;

    PnpDoc() { cv.d = this; registry().push_back(this); }
    ~PnpDoc() override { auto& r = registry(); r.erase(std::remove(r.begin(), r.end(), this), r.end()); }

    // ------------------------------------------------------------------ FileDoc
    bool is_sheet() const override { return false; }
    bool dirty() const override { return gen != saved_gen; }
    void draw(ImVec2 size) override;
    bool save(bool save_as) override;
    bool on_command(int id) override;
    std::string status() const override;

    // ------------------------------------------------------------------ model ops
    void checkpoint(const std::string& label) {
        undo_.push_back({prog, label});
        if (undo_.size() > 200) undo_.erase(undo_.begin());
        redo_.clear();
    }
    void changed() { ++gen; sync_sel(); }
    void sync_sel() { if (sel.size() != prog.placements.size()) { sel.assign(prog.placements.size(), 0); ++sel_gen; } }
    bool do_undo() {
        if (undo_.empty()) return false;
        redo_.push_back({prog, undo_.back().label});
        msg = "Undo " + undo_.back().label;
        prog = std::move(undo_.back().p);
        undo_.pop_back();
        changed();
        return true;
    }
    bool do_redo() {
        if (redo_.empty()) return false;
        undo_.push_back({prog, redo_.back().label});
        msg = "Redo " + redo_.back().label;
        prog = std::move(redo_.back().p);
        redo_.pop_back();
        changed();
        return true;
    }
    int nsel() const { int n = 0; for (auto s : sel) n += s; return n; }
    std::vector<int> selected_or_all() const {
        std::vector<int> v;
        for (size_t i = 0; i < sel.size(); ++i) if (sel[i]) v.push_back((int)i);
        if (v.empty()) for (size_t i = 0; i < prog.placements.size(); ++i) v.push_back((int)i);
        return v;
    }
    void select_only(int i) { std::fill(sel.begin(), sel.end(), 0); if (i >= 0 && i < (int)sel.size()) sel[(size_t)i] = 1; cursor = i; ++sel_gen; }
    std::string part_name(int k) const { return k >= 0 && k < (int)prog.parts.size() ? prog.parts[(size_t)k].name : std::string(); }
    const PnpPart* part_of(const PnpPlacement& q) const { return q.part >= 0 && q.part < (int)prog.parts.size() ? &prog.parts[(size_t)q.part] : nullptr; }
    BBox place_box(const PnpPlacement& q) const {
        const PnpPart* k = part_of(q);
        double L = 0.6, W = 0.6;
        if (k && k->has_body) { L = std::max(0.2, k->len); W = std::max(0.2, k->wid); }
        BBox b;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) b.add(Vec2{q.x, q.y} + rotate({sx * L / 2, sy * W / 2}, q.rot));
        return b;
    }
    void focus_selection() {
        BBox b;
        for (size_t i = 0; i < sel.size(); ++i) if (sel[i]) b.add(place_box(prog.placements[i]));
        if (b.empty()) return;
        b.inflate(std::max(3.0, std::max(b.w(), b.h()) * 0.25));
        cv.fly_to(b);
    }
    // placement hit test (world)
    int hit(Vec2 w, double tol_mm) const {
        int best = -1;
        double bd = 1e18;
        for (size_t i = 0; i < prog.placements.size(); ++i) {
            const PnpPlacement& q = prog.placements[i];
            const PnpPart* k = part_of(q);
            Vec2 l = rotate(w - Vec2{q.x, q.y}, -q.rot);
            double L = 0.6, W = 0.6;
            if (k && k->has_body) { L = k->len; W = k->wid; }
            const double dx = std::max(0.0, std::fabs(l.x) - L / 2), dy = std::max(0.0, std::fabs(l.y) - W / 2);
            const double dd = std::hypot(dx, dy) + std::hypot(l.x, l.y) * 1e-3;
            if (dd < bd && std::hypot(dx, dy) <= tol_mm) { bd = dd; best = (int)i; }
        }
        return best;
    }
    void rebuild_view();
    void ensure_issues() { if (issues_gen != gen) { issues = io::pnp_validate(prog); issues_gen = gen; } }
    int count_issues(int sev) { ensure_issues(); int n = 0; for (auto& i : issues) n += i.severity == sev; return n; }

    // ------------------------------------------------------------------ actions
    void apply_transform();
    void set_skip(int mode);              // 0 off 1 on 2 toggle
    void delete_selected();
    void duplicate_selected();
    void sort_program(int key);           // 0 part 1 position 2 ref 3 circuit+part 4 current view
    void select_part(int k);
    void export_csv();
    void create_project();
    void compare_project();
    bool write_to(const std::wstring& p, bool convert, const std::string& target);

    // ------------------------------------------------------------------ UI
    void draw_toolbar();
    void draw_placements(ImVec2 sz);
    void draw_parts(ImVec2 sz);
    void draw_circuits(ImVec2 sz);
    void draw_marks(ImVec2 sz);
    void draw_board(ImVec2 sz);
    void draw_validation(ImVec2 sz);
    void draw_compare();
    void draw_canvas_menu();
    // cell editor; returns true when committed (value in ebuf)
    bool cell_edit(int tabi, int row, int col, bool* cancelled = nullptr);
    void begin_edit(int tabi, int row, int col, const std::string& v) {
        ed_tab = tabi; ed_row = row; ed_col = col;
        std::snprintf(ebuf, sizeof ebuf, "%s", v.c_str());
        ed_focus = true;
    }
    bool editing(int tabi, int row, int col) const { return ed_tab == tabi && ed_row == row && ed_col == col; }
    void commit_place(int row, int col, const std::string& v);
    void commit_part(int row, int col, const std::string& v);
};

// ===========================================================================
// canvas
// ===========================================================================
BBox PnpCanvas::world_extent() const {
    const PnpProgram& p = d->prog;
    BBox b;
    if (p.board.w > 0 && p.board.h > 0) {
        Vec2 c = p.board_corner();
        b.add(c); b.add(c + Vec2{p.board.w, p.board.h});
    }
    for (auto& q : p.placements) b.add(Vec2{q.x, q.y});
    for (auto& m : p.marks) b.add(Vec2{m.x, m.y});
    for (auto& f : io::pnp_circuit_frames(p)) for (auto& c : f.corners) b.add(c);
    if (b.empty()) b = BBox(0, 0, 100, 80);
    b.inflate(std::max(2.0, std::max(b.w(), b.h()) * 0.04));
    return b;
}

void PnpCanvas::paint_world(ID2D1DeviceContext* rt) {
    const PnpProgram& p = d->prog;
    const ui::Palette& pl = ui::pal();
    const D2D1_MATRIX_3X2_F W = world_matrix();
    rt->SetTransform(W);
    Com<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(ui::d2c(0xFFFFFFFF), br.put());
    if (!br) return;
    const float px = (float)(1.0 / scale_);
    auto col = [&](uint32_t c, float a = 1.f) { br->SetColor(ui::d2c(c, a)); return br.get(); };
    auto line = [&](Vec2 a, Vec2 b, float w) { rt->DrawLine(D2D1::Point2F((float)a.x, (float)a.y), D2D1::Point2F((float)b.x, (float)b.y), br.get(), w); };
    Com<ID2D1StrokeStyle> dash;
    {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
        sp.dashStyle = D2D1_DASH_STYLE_DASH;
        Com<ID2D1Factory> f;
        rt->GetFactory(f.put());
        if (f) f->CreateStrokeStyle(sp, nullptr, 0, dash.put());
    }
    // board
    if (p.board.w > 0 && p.board.h > 0) {
        const Vec2 c = p.board_corner();
        D2D1_RECT_F r{(float)c.x, (float)c.y, (float)(c.x + p.board.w), (float)(c.y + p.board.h)};
        rt->FillRectangle(r, col(pl.board_fill));
        rt->DrawRectangle(r, col(pl.board_edge), 1.6f * px);
    }
    // circuits
    const auto frames = io::pnp_circuit_frames(p);
    if (d->show_ckt) {
        for (const auto& f : frames) {
            const bool sel = d->tab == T_CIRC && d->circ_sel == f.index;
            col(sel ? pl.selection : pl.panel_frame, sel ? 1.f : 0.85f);
            for (int i = 0; i < 4; ++i) rt->DrawLine(D2D1::Point2F((float)f.corners[i].x, (float)f.corners[i].y),
                                                     D2D1::Point2F((float)f.corners[(i + 1) % 4].x, (float)f.corners[(i + 1) % 4].y),
                                                     br.get(), (sel ? 2.2f : 1.2f) * px, dash.get());
        }
        if (frames.empty() && p.circuits.size() > 1)   // no circuit object: mark circuit offsets
            for (size_t k = 0; k < p.circuits.size(); ++k) {
                Vec2 o = {p.circuits[k].x, p.circuits[k].y};
                col(pl.panel_frame);
                const double s = 6 * px;
                line(o - Vec2{s, 0}, o + Vec2{s, 0}, px * 1.2f);
                line(o - Vec2{0, s}, o + Vec2{0, s}, px * 1.2f);
            }
    }
    // ghosts: root-only programs replicated over the other circuits
    bool root_only = p.circuits.size() > 1;
    for (auto& q : p.placements) if (q.circuit != 0) { root_only = false; break; }
    if (d->show_ghosts && root_only) {
        col(pl.comp_body, 0.25f);
        for (size_t k = 1; k < p.circuits.size(); ++k) {
            Vec2 off; double rot;
            if (!io::pnp_circuit_xform(p, (int)k, off, rot)) continue;
            for (auto& q : p.placements) {
                const PnpPart* kp = d->part_of(q);
                const double L = kp && kp->has_body ? kp->len : 0.5, Wd = kp && kp->has_body ? kp->wid : 0.5;
                const Vec2 c = rotate({q.x, q.y}, rot) + off;
                rt->SetTransform(D2D1::Matrix3x2F::Rotation((float)(q.rot + rot)) * D2D1::Matrix3x2F::Translation((float)c.x, (float)c.y) * W);
                rt->DrawRectangle(D2D1::RectF((float)(-L / 2), (float)(-Wd / 2), (float)(L / 2), (float)(Wd / 2)), br.get(), px);
            }
        }
        rt->SetTransform(W);
    }
    // placements
    for (size_t i = 0; i < p.placements.size(); ++i) {
        const PnpPlacement& q = p.placements[i];
        const PnpPart* kp = d->part_of(q);
        const bool s = i < d->sel.size() && d->sel[i];
        const bool body = kp && kp->has_body && kp->len > 0 && kp->wid > 0;
        const double L = body ? kp->len : 0.0, Wd = body ? kp->wid : 0.0;
        rt->SetTransform(D2D1::Matrix3x2F::Rotation((float)q.rot) * D2D1::Matrix3x2F::Translation((float)q.x, (float)q.y) * W);
        const uint32_t edge = s ? pl.selection : q.skip ? 0xFF8A8A8A : (q.part < 0 ? pl.error_red : pl.comp_body);
        const float a = q.skip ? 0.55f : 1.f;
        double tick;
        if (body) {
            D2D1_RECT_F r = D2D1::RectF((float)(-L / 2), (float)(-Wd / 2), (float)(L / 2), (float)(Wd / 2));
            rt->FillRectangle(r, col(s ? pl.selection : pl.comp_body_fill, s ? 0.35f : 0.8f * a));
            rt->DrawRectangle(r, col(edge, a), (s ? 2.f : 1.f) * px);
            tick = L / 2;
        } else {
            const float rr = (float)std::max(0.35, 3.0 * px);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), rr, rr), col(edge, a));
            tick = rr * 2.4;
        }
        col(s ? pl.selection : 0xFFFF8C1A, a);
        rt->DrawLine(D2D1::Point2F(0, 0), D2D1::Point2F((float)std::max(tick, 6.0 * px), 0), br.get(), 1.4f * px);
        if (q.skip) {
            const float e = (float)std::max(body ? std::min(L, Wd) / 2 : 0.35, 3.0 * px);
            col(0xFF8A8A8A, 0.9f);   // skipped = neutral grey cross (x-out circuits are intentional, not errors)
            rt->DrawLine(D2D1::Point2F(-e, -e), D2D1::Point2F(e, e), br.get(), 1.2f * px);
            rt->DrawLine(D2D1::Point2F(-e, e), D2D1::Point2F(e, -e), br.get(), 1.2f * px);
        }
    }
    rt->SetTransform(W);
    // marks
    if (d->show_marks)
        for (size_t i = 0; i < p.marks.size(); ++i) {
            const auto& m = p.marks[i];
            const bool s = d->tab == T_MARKS && d->mark_sel == (int)i;
            const float r = (float)(m.dia > 0 ? m.dia / 2 : 0.5);
            const D2D1_POINT_2F c = D2D1::Point2F((float)m.x, (float)m.y);
            if (m.kind == MarkKind::Bad) {
                col(pl.error_red);
                rt->DrawLine(D2D1::Point2F(c.x - r * 1.4f, c.y - r * 1.4f), D2D1::Point2F(c.x + r * 1.4f, c.y + r * 1.4f), br.get(), 2 * px);
                rt->DrawLine(D2D1::Point2F(c.x - r * 1.4f, c.y + r * 1.4f), D2D1::Point2F(c.x + r * 1.4f, c.y - r * 1.4f), br.get(), 2 * px);
                rt->DrawRectangle(D2D1::RectF(c.x - r * 1.6f, c.y - r * 1.6f, c.x + r * 1.6f, c.y + r * 1.6f), br.get(), px);
            } else {
                const uint32_t mc = s ? pl.selection : m.kind == MarkKind::Local ? 0xFF2CB1A0 : pl.fiducial;
                rt->FillEllipse(D2D1::Ellipse(c, r, r), col(mc, 0.85f));
                const float o = r * 2.2f;
                col(mc);
                rt->DrawEllipse(D2D1::Ellipse(c, o, o), br.get(), 1.2f * px);
                rt->DrawLine(D2D1::Point2F(c.x - o * 1.3f, c.y), D2D1::Point2F(c.x + o * 1.3f, c.y), br.get(), px);
                rt->DrawLine(D2D1::Point2F(c.x, c.y - o * 1.3f), D2D1::Point2F(c.x, c.y + o * 1.3f), br.get(), px);
            }
        }
    // origin (HLC PCB layout corner) + transport direction
    {
        const Vec2 o = p.kind == io::PnpKind::Hlc ? Vec2{p.board.origin_x, p.board.origin_y} : Vec2{0, 0};
        const double L = 18 * px;
        col(pl.origin);
        line(o, o + Vec2{L, 0}, 2 * px);
        line(o, o + Vec2{0, L}, 2 * px);
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((float)o.x, (float)o.y), 3 * px, 3 * px), br.get(), 1.5f * px);
        if (p.board.w > 0 && p.board.h > 0) {
            const Vec2 c = p.board_corner();
            const double y = c.y - 10 * px, x0 = c.x + p.board.w * 0.35, x1 = c.x + p.board.w * 0.65;
            const double dir = p.board.rl ? -1 : 1;
            const Vec2 a{dir > 0 ? x0 : x1, y}, b{dir > 0 ? x1 : x0, y};
            col(pl.label, 0.7f);
            line(a, b, 1.5f * px);
            line(b, b + Vec2{-dir * 7 * px, 4 * px}, 1.5f * px);
            line(b, b + Vec2{-dir * 7 * px, -4 * px}, 1.5f * px);
            // fixed rail
            const double ry = p.board.back ? c.y + p.board.h + 3 * px : c.y - 3 * px;
            col(pl.origin, 0.6f);
            line({c.x, ry}, {c.x + p.board.w, ry}, 3 * px);
        }
    }
}

void PnpCanvas::paint_overlay(ImDrawList* dl) {
    const PnpProgram& p = d->prog;
    const GTheme& t = th();
    // labels when they fit
    if (d->show_labels && scale_ > 2.5) {
        const ImVec2 a = origin(), b = a + size();
        int drawn = 0;
        ImGui::PushFont(g_font);
        const float fs = std::clamp((float)(scale_ * 0.9), S(9), S(13));
        for (size_t i = 0; i < p.placements.size() && drawn < 1500; ++i) {
            const auto& q = p.placements[i];
            ImVec2 s = w2s({q.x, q.y});
            if (s.x < a.x || s.y < a.y || s.x > b.x || s.y > b.y) continue;
            ImVec2 ts = g_font->CalcTextSizeA(fs, FLT_MAX, 0, q.ref.c_str());
            dl->AddText(g_font, fs, ImVec2(s.x - ts.x / 2, s.y - ts.y - S(3)), col_alpha(t.text, q.skip ? 0.45f : 0.9f), q.ref.c_str());
            ++drawn;
        }
        if (d->show_marks)
            for (auto& m : p.marks) {
                ImVec2 s = w2s({m.x, m.y});
                if (s.x < a.x || s.y < a.y || s.x > b.x || s.y > b.y) continue;
                dl->AddText(g_font, fs, ImVec2(s.x + S(8), s.y + S(4)), col_alpha(t.text_dim, 0.9f), m.name.c_str());
            }
        ImGui::PopFont();
    }
    // circuit numbers
    if (d->show_ckt) {
        for (const auto& f : io::pnp_circuit_frames(p)) {
            ImVec2 s = w2s(f.corners[3]);
            int n = 0, k = 0;
            for (auto& q : p.placements) if (q.circuit == f.index) { ++n; k += q.skip; }
            char b[32]; std::snprintf(b, sizeof b, n && k == n ? "%d  x-out (all skipped)" : "%d", f.index + 1);
            dl->AddText(ImVec2(s.x + S(4), s.y + S(2)), col_alpha(t.text_dim, 0.9f), b);
        }
    }
    hover = hovered() ? d->hit(mouse_world(), 4.0 / scale_) : -1;
    if (hover >= 0) {
        const auto& q = p.placements[(size_t)hover];
        BBox b = d->place_box(q);
        ImVec2 s0 = w2s({b.x0, b.y1}), s1 = w2s({b.x1, b.y0});
        if (mirror()) std::swap(s0.x, s1.x);
        dl->AddRect(s0 - ImVec2(S(3), S(3)), s1 + ImVec2(S(3), S(3)), t.accent, S(3), 0, S(1.5f));
        const PnpPart* k = d->part_of(q);
        ImGui::BeginTooltip();
        ImGui::PushFont(g_font_bold); ImGui::TextUnformatted(q.ref.c_str()); ImGui::PopFont();
        ImGui::Text("Step %d   %s", hover + 1, k ? k->name.c_str() : "(no part link)");
        if (k && !k->desc.empty()) ImGui::TextDisabled("%s", k->desc.c_str());
        ImGui::Text("X %s   Y %s   %s\xC2\xB0", f4(q.x).c_str(), f4(q.y).c_str(), f4(q.rot).c_str());
        if (p.circuits.size() > 1) ImGui::Text("Circuit %d", q.circuit + 1);
        if (q.skip) ImGui::TextColored(ImColor(t.warn), "Skipped");
        if (q.has_local) ImGui::TextDisabled("Local mark set %d", q.mark_set);
        ImGui::EndTooltip();
    }
}

void PnpCanvas::on_click(Vec2 w, int mods) {
    int h = d->hit(w, 4.0 / scale_);
    d->sync_sel();
    if (h < 0) {
        if (!(mods & (KM_CTRL | KM_SHIFT))) { std::fill(d->sel.begin(), d->sel.end(), 0); ++d->sel_gen; }
        return;
    }
    if (mods & KM_CTRL) d->sel[(size_t)h] ^= 1;
    else if (mods & KM_SHIFT) d->sel[(size_t)h] = 1;
    else d->select_only(h);
    d->cursor = h;
    d->scroll_to = h;
    d->want_tab = T_PLACE;
    ++d->sel_gen;
}
void PnpCanvas::on_dblclick(Vec2 w, int) {
    int h = d->hit(w, 4.0 / scale_);
    if (h >= 0) { d->select_only(h); d->scroll_to = h; d->want_tab = T_PLACE; d->focus_selection(); }
}
void PnpCanvas::on_rect(const BBox& b, int mods) {
    d->sync_sel();
    if (!(mods & (KM_CTRL | KM_SHIFT))) std::fill(d->sel.begin(), d->sel.end(), 0);
    int first = -1;
    for (size_t i = 0; i < d->prog.placements.size(); ++i) {
        const auto& q = d->prog.placements[i];
        if (b.contains({q.x, q.y})) { d->sel[i] = (mods & KM_CTRL) ? (uint8_t)!d->sel[i] : 1; if (first < 0) first = (int)i; }
    }
    if (first >= 0) { d->scroll_to = first; d->want_tab = T_PLACE; }
    ++d->sel_gen;
}
void PnpCanvas::on_context(Vec2 w) {
    int h = d->hit(w, 4.0 / scale_);
    if (h >= 0 && !(h < (int)d->sel.size() && d->sel[(size_t)h])) d->select_only(h);
    ImGui::OpenPopup("##pnp_canvas_ctx");
}
bool PnpCanvas::on_key(ImGuiKey k, int) {
    if (k == ImGuiKey_Escape) { std::fill(d->sel.begin(), d->sel.end(), 0); ++d->sel_gen; return true; }
    return false;
}

// ===========================================================================
// actions
// ===========================================================================
void PnpDoc::rebuild_view() {
    if (view_gen == gen && view_find == find) return;
    view_gen = gen;
    view_find = find;
    view.clear();
    const std::string f = find;
    for (size_t i = 0; i < prog.placements.size(); ++i) {
        const auto& q = prog.placements[i];
        if (!f.empty() && !icontains(q.ref, f) && !icontains(part_name(q.part), f)) continue;
        view.push_back((int)i);
    }
    if (sort_col >= 0) {
        auto key_less = [&](int a, int b) {
            const auto& x = prog.placements[(size_t)a];
            const auto& y = prog.placements[(size_t)b];
            switch (sort_col) {
                case 1: return ref_less(x.ref, y.ref);
                case 2: return part_name(x.part) < part_name(y.part);
                case 3: return x.x < y.x;
                case 4: return x.y < y.y;
                case 5: return x.rot < y.rot;
                case 6: return x.circuit < y.circuit;
                case 7: return x.skip < y.skip;
                case 8: return x.station < y.station;
                default: return a < b;
            }
        };
        std::stable_sort(view.begin(), view.end(), [&](int a, int b) { return sort_desc ? key_less(b, a) : key_less(a, b); });
    }
}

void PnpDoc::apply_transform() {
    auto idx = selected_or_all();
    if (idx.empty()) return;
    checkpoint("Transform placements");
    Vec2 c{0, 0};
    if (t_about == 0) {
        BBox b;
        for (int i : idx) b.add(Vec2{prog.placements[(size_t)i].x, prog.placements[(size_t)i].y});
        c = b.center();
    }
    for (int i : idx) {
        auto& q = prog.placements[(size_t)i];
        Vec2 p{q.x, q.y};
        Vec2 pivot = t_about == 2 ? p : c;
        if (t_mx) { p.x = 2 * pivot.x - p.x; q.rot = 180.0 - q.rot; }
        if (t_my) { p.y = 2 * pivot.y - p.y; q.rot = -q.rot; }
        if (t_rot != 0) { p = rotate_about(p, pivot, t_rot); q.rot += t_rot; }
        q.rot = norm_deg(q.rot);
        p.x += t_dx; p.y += t_dy;
        q.x = std::round(p.x * 1e4) / 1e4;
        q.y = std::round(p.y * 1e4) / 1e4;
    }
    changed();
    toast("Transformed " + std::to_string(idx.size()) + " placement(s)", ToastKind::Success);
}

void PnpDoc::set_skip(int mode) {
    auto idx = selected_or_all();
    if (idx.empty()) return;
    checkpoint(mode == 1 ? "Skip placements" : mode == 0 ? "Unskip placements" : "Toggle skip");
    for (int i : idx) { auto& q = prog.placements[(size_t)i]; q.skip = mode == 2 ? !q.skip : mode == 1; }
    changed();
}

void PnpDoc::delete_selected() {
    if (!prog.can_restructure()) { toast("This program format supports field edits only (rows cannot be deleted)", ToastKind::Warning, 5.f); return; }
    int n = nsel();
    if (!n) return;
    checkpoint("Delete placements");
    std::vector<PnpPlacement> v;
    for (size_t i = 0; i < prog.placements.size(); ++i) if (!sel[i]) v.push_back(prog.placements[i]);
    prog.placements = std::move(v);
    sel.assign(prog.placements.size(), 0);
    changed();
    toast("Deleted " + std::to_string(n) + " placement(s)", ToastKind::Success, 3.f, "Undo", [this]() { do_undo(); });
}

void PnpDoc::duplicate_selected() {
    if (!prog.can_restructure()) { toast("This program format supports field edits only (rows cannot be added)", ToastKind::Warning, 5.f); return; }
    std::vector<int> idx;
    for (size_t i = 0; i < sel.size(); ++i) if (sel[i]) idx.push_back((int)i);
    if (idx.empty()) return;
    checkpoint("Duplicate placements");
    std::set<std::string> used;
    for (auto& q : prog.placements) used.insert(q.ref);
    for (int i : idx) {
        PnpPlacement c = prog.placements[(size_t)i];
        std::string r;
        for (int k = 2;; ++k) { r = c.ref + "_" + std::to_string(k); if (!used.count(r)) break; }
        used.insert(r);
        c.ref = r;
        c.x += 1.0; c.y += 1.0;
        prog.placements.push_back(c);
    }
    sel.assign(prog.placements.size(), 0);
    for (size_t k = 0; k < idx.size(); ++k) sel[prog.placements.size() - 1 - k] = 1;
    changed();
    scroll_to = (int)prog.placements.size() - 1;
    toast("Duplicated " + std::to_string(idx.size()) + " placement(s) (cloned records, offset +1 mm)", ToastKind::Success);
}

void PnpDoc::sort_program(int key) {
    if (!prog.can_restructure()) { toast("This program format cannot be reordered", ToastKind::Warning, 5.f); return; }
    std::vector<int> ord(prog.placements.size());
    for (size_t i = 0; i < ord.size(); ++i) ord[i] = (int)i;
    const auto& P = prog.placements;
    if (key == 4) { rebuild_view(); if (view.size() != P.size()) { toast("Clear the find filter first: the view must show every row", ToastKind::Warning); return; } ord = view; }
    else
        std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) {
            const auto& x = P[(size_t)a]; const auto& y = P[(size_t)b];
            switch (key) {
                case 0: return part_name(x.part) < part_name(y.part);
                case 1: return std::make_pair(std::round(x.y * 10), x.x) < std::make_pair(std::round(y.y * 10), y.x);
                case 2: return ref_less(x.ref, y.ref);
                default: return std::make_tuple(x.circuit, part_name(x.part)) < std::make_tuple(y.circuit, part_name(y.part));
            }
        });
    bool same = true;
    for (size_t i = 0; i < ord.size(); ++i) if (ord[i] != (int)i) same = false;
    if (same) { toast("Already in that order", ToastKind::Info); return; }
    static const char* names[] = {"Sort by part", "Sort by position", "Sort by reference", "Sort by circuit + part", "Renumber steps"};
    checkpoint(names[key]);
    std::vector<PnpPlacement> v;
    std::vector<uint8_t> s;
    for (int i : ord) { v.push_back(P[(size_t)i]); s.push_back(sel[(size_t)i]); }
    prog.placements = std::move(v);
    sel = std::move(s);
    sort_col = -1;
    changed();
    toast(std::string(names[key]) + ": steps renumbered", ToastKind::Success);
}

void PnpDoc::select_part(int k) {
    sync_sel();
    int first = -1;
    for (size_t i = 0; i < prog.placements.size(); ++i) {
        sel[i] = prog.placements[i].part == k;
        if (sel[i] && first < 0) first = (int)i;
    }
    ++sel_gen;
    if (first >= 0) { scroll_to = first; want_tab = T_PLACE; focus_selection(); }
}

void PnpDoc::export_csv() {
    std::wstring p = path.empty() ? ui::W(name) : path;
    size_t k = p.find_last_of(L'.');
    if (k != std::wstring::npos) p = p.substr(0, k);
    p += L"_placements.csv";
    if (!ui::save_file_dialog(app::frame_hwnd(), L"Export placements (CSV)", {{L"CSV (*.csv)", L"*.csv"}}, L"csv", p)) return;
    const std::string s = io::pnp_csv(prog);
    std::string err;
    if (!write_file(ui::U(p), s.data(), s.size(), err)) { toast("Could not write " + ui::U(p) + ": " + err, ToastKind::Error, 6.f); return; }
    toast("Exported " + std::to_string(prog.placements.size()) + " placements to CSV", ToastKind::Success, 4.f, "Open", [p]() { files_open(p, 1); });
}

void PnpDoc::create_project() {
    std::vector<uint8_t> bytes;
    std::string err;
    std::vector<std::string> notes;
    if (!io::pnp_save(prog, bytes, err, &notes)) { toast("Could not serialise the program: " + err, ToastKind::Error, 6.f); return; }
    app::DocPtr d = app::new_document();
    juki::ImportOptions io_;
    juki::ImportResult res;
    if (!juki::import_juki(bytes, d->prj, io_, res, err)) {
        app::close_document(d);
        toast("Create project failed: " + err, ToastKind::Error, 6.f);
        return;
    }
    d->prj.info.original_filename = ui::U(path);
    d->prj.dirty = true;
    d->changed(ui::CH_ALL);
    files_set_active(false);
    activate_view(d, ViewKind::Plotter);
    zoom_extents_doc(d);
    toast("Project created from " + name + ": " + std::to_string(res.placements) + " placements, " + std::to_string(res.parts) + " parts" +
              (res.warnings.empty() ? "" : " (" + std::to_string(res.warnings.size()) + " notes)"),
          ToastKind::Success, 5.f);
    if (!res.warnings.empty()) {
        std::wstring t;
        for (auto& w : res.warnings) t += L"• " + ui::W(w) + L"\r\n";
        defer([t]() { ui::show_text(app::frame_hwnd(), L"Create project from program", t, false); });
    }
}

void PnpDoc::compare_project() {
    auto& docs = app::open_docs();
    if (docs.empty()) { toast("Open a PCB Plotter project to compare with", ToastKind::Warning, 5.f); return; }
    app::DocPtr d = docs.size() == 1 ? docs[0] : nullptr;
    if (!d) {
        std::vector<std::wstring> ch;
        for (auto& x : docs) ch.push_back(ui::W(x->prj.info.name.empty() ? (x->prj.path.empty() ? "(untitled)" : x->prj.path) : x->prj.info.name));
        int c = ui::prompt_choice(app::frame_hwnd(), L"Compare with project", L"Project:", ch, 0);
        if (c < 0) return;
        d = docs[(size_t)c];
    }
    std::vector<int> pts;
    for (size_t i = 0; i < d->prj.model.points.size(); ++i) if (d->prj.model.points[i].has_machine) pts.push_back((int)i);
    if (pts.empty()) { toast("The project has no model point with a machine setup", ToastKind::Warning, 5.f); return; }
    int pt = pts[0];
    if (pts.size() > 1) {
        std::vector<std::wstring> ch;
        for (int i : pts) ch.push_back(ui::W(d->prj.model.points[(size_t)i].name));
        int c = ui::prompt_choice(app::frame_hwnd(), L"Compare with project", L"Model point (machine program):", ch, 0);
        if (c < 0) return;
        pt = pts[(size_t)c];
    }
    MachineJob job;
    std::string err;
    if (!build_machine_job(d->prj, pt, job, err)) { toast("Could not build the machine job: " + err, ToastKind::Error, 6.f); return; }
    // program circuits: root-only programs are compared against circuit 1
    bool prog_circ = false;
    for (auto& q : prog.placements) if (q.circuit) prog_circ = true;
    std::map<std::string, int> pmap;
    for (size_t i = 0; i < prog.placements.size(); ++i) pmap[prog.placements[i].ref + "#" + std::to_string(prog_circ ? prog.placements[i].circuit + 1 : 1)] = (int)i;
    std::set<std::string> seen;
    cmp.clear();
    int same = 0;
    for (auto& jp : job.placements) {
        if (!prog_circ && jp.circuit > 1) continue;
        const std::string key = jp.ref + "#" + std::to_string(prog_circ ? jp.circuit : 1);
        seen.insert(key);
        auto it = pmap.find(key);
        const std::string label = jp.ref + (prog_circ ? " (c" + std::to_string(jp.circuit) + ")" : "");
        if (it == pmap.end()) { cmp.push_back({label, "Missing in program", "project " + jp.part + " at " + f4(jp.x) + ", " + f4(jp.y), -1}); continue; }
        const auto& q = prog.placements[(size_t)it->second];
        std::string det;
        const double dx = q.x - jp.x, dy = q.y - jp.y;
        if (std::hypot(dx, dy) > 0.005) det += "moved " + f4(dx) + ", " + f4(dy) + " mm; ";
        double da = std::fabs(norm_deg(q.rot) - norm_deg(jp.rot));
        da = std::min(da, 360 - da);
        if (da > 0.01) det += "angle " + f4(norm_deg(jp.rot)) + " -> " + f4(norm_deg(q.rot)) + "; ";
        const std::string pn = part_name(q.part);
        if (pn != jp.part && !(pn.size() == 20 && jp.part.compare(0, 20, pn) == 0)) det += "part " + jp.part + " -> " + pn + "; ";
        if (q.skip != jp.skip) det += q.skip ? "skipped in program; " : "placed in program but DNP in project; ";
        if (det.empty()) { ++same; continue; }
        det.resize(det.size() - 2);
        cmp.push_back({label, "Different", det, it->second});
    }
    for (auto& kv : pmap)
        if (!seen.count(kv.first)) {
            const auto& q = prog.placements[(size_t)kv.second];
            cmp.push_back({q.ref + (prog_circ ? " (c" + std::to_string(q.circuit + 1) + ")" : ""), "Only in program", part_name(q.part) + " at " + f4(q.x) + ", " + f4(q.y), kv.second});
        }
    cmp_title = "Program vs '" + (d->prj.info.name.empty() ? std::string("project") : d->prj.info.name) + "' / " + d->prj.model.points[(size_t)pt].name +
                ": " + std::to_string(same) + " identical, " + std::to_string(cmp.size()) + " differences";
    cmp_open = true;
}

bool PnpDoc::write_to(const std::wstring& p, bool convert, const std::string& target) {
    std::vector<uint8_t> out;
    std::string err;
    std::vector<std::string> notes;
    if (convert) {
        std::vector<std::string> lossy;
        if (!io::pnp_convert(prog, target, out, lossy, err)) { toast("Conversion failed: " + err, ToastKind::Error, 6.f); return false; }
        std::string t = "Converting " + prog.format + " to " + target + " rebuilds the program from the editor's model. Not carried over:\n";
        for (auto& l : lossy) t += "\n\xE2\x80\xA2 " + l;
        if (!confirm(("Save As " + target).c_str(), t, "Convert and save", true)) return false;
    } else if (!io::pnp_save(prog, out, err, &notes)) {
        toast("Save failed: " + err, ToastKind::Error, 6.f);
        return false;
    }
    if (!write_file(ui::U(p), out.data(), out.size(), err)) { toast("Could not write " + ui::U(p) + ": " + err, ToastKind::Error, 6.f); return false; }
    // the saved file becomes the new baseline (record indices, raw bytes)
    PnpProgram np;
    if (io::pnp_load(out, np, err)) {
        if (convert) { prog = std::move(np); undo_.clear(); redo_.clear(); }
        else {
            // keep the undo history (snapshots carry their own baseline) but re-anchor the live model
            prog = std::move(np);
        }
        sel.assign(prog.placements.size(), 0);
    }
    path = p;
    name = ui::U(file_name_of(p));
    ++gen;
    saved_gen = gen;
    std::string m = "Saved " + name + " (" + std::to_string(out.size()) + " bytes)";
    toast(m, ToastKind::Success, 3.5f);
    for (auto& n : notes) toast(n, ToastKind::Warning, 6.f);
    return true;
}

// ===========================================================================
// FileDoc
// ===========================================================================
bool PnpDoc::save(bool save_as) {
    std::wstring p = path;
    const std::string ext = ext_of(path);
    if (save_as || p.empty()) {
        std::vector<ui::FileFilter> f;
        const std::wstring same = ext.empty() ? (prog.kind == io::PnpKind::Iss ? L"iss" : L"h8h") : ui::W(ext);
        f.push_back({ui::W(prog.format) + L" (same format, lossless) (*." + same + L")", L"*." + same});
        const auto targets = io::pnp_convert_targets(prog);
        for (auto& t : targets) f.push_back({ui::W(t) + L" (convert, lossy) (*." + ui::W(lower(t)) + L")", L"*." + ui::W(lower(t))});
        if (!ui::save_file_dialog(app::frame_hwnd(), L"Save Program As", f, same, p)) return false;
        const std::string ne = ext_of(p);
        if (ne != ext && !ne.empty()) {
            for (auto& t : targets)
                if (lower(t) == ne) return write_to(p, true, t);
            const bool hlc_ext = io::pnp_is_program_ext(ne) && ne != "iss" && ne != "x01" && ne != "x52" && ne != "s01x";
            if ((prog.kind == io::PnpKind::Iss) == hlc_ext) {
                toast("Unsupported target format ." + ne, ToastKind::Error, 5.f);
                return false;
            }
        }
    }
    return write_to(p, false, {});
}

bool PnpDoc::on_command(int id) {
    using namespace app;
    switch (id) {
        case ID_EDIT_UNDO: if (!do_undo()) gui::status("Nothing to undo"); return true;
        case ID_EDIT_REDO: if (!do_redo()) gui::status("Nothing to redo"); return true;
        case ID_EDIT_SELECT_ALL: sync_sel(); std::fill(sel.begin(), sel.end(), 1); ++sel_gen; return true;
        case ID_EDIT_DELETE: delete_selected(); return true;
        case ID_EDIT_COPY: {
            std::string s = "Step\tRef\tPart\tX\tY\tAngle\tCircuit\tSkip\r\n";
            for (size_t i = 0; i < prog.placements.size(); ++i) {
                if (nsel() && !sel[i]) continue;
                const auto& q = prog.placements[i];
                s += std::to_string(i + 1) + "\t" + q.ref + "\t" + part_name(q.part) + "\t" + f4(q.x) + "\t" + f4(q.y) + "\t" + f4(q.rot) + "\t" +
                     std::to_string(q.circuit + 1) + "\t" + (q.skip ? "1" : "0") + "\r\n";
            }
            ImGui::SetClipboardText(s.c_str());
            gui::status("Copied placements to the clipboard");
            return true;
        }
    }
    return false;
}

std::string PnpDoc::status() const {
    std::string s = prog.format + " program  \xC2\xB7  " + std::to_string(prog.placements.size()) + " placements, " + std::to_string(prog.parts.size()) +
                    " parts, " + std::to_string(prog.marks.size()) + " marks";
    if (prog.circuits.size() > 1) s += ", " + std::to_string(prog.circuits.size()) + " circuits";
    if (gen != saved_gen) s += "  \xC2\xB7  modified";
    if (!msg.empty()) s += "  \xC2\xB7  " + msg;
    return s;
}

// ===========================================================================
// UI
// ===========================================================================
bool PnpDoc::cell_edit(int tabi, int row, int col, bool* cancelled) {
    if (!editing(tabi, row, col)) return false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ed_focus) { ImGui::SetKeyboardFocusHere(); ed_focus = false; }
    char id[32]; std::snprintf(id, sizeof id, "##ed%d_%d", row, col);
    bool enter = ImGui::InputText(id, ebuf, sizeof ebuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { ed_tab = -1; if (cancelled) *cancelled = true; return false; }
    if (enter || ImGui::IsItemDeactivatedAfterEdit() || (ImGui::IsItemDeactivated() && !ImGui::IsItemActive())) {
        ed_tab = -1;
        return true;
    }
    return false;
}

void PnpDoc::commit_place(int row, int col, const std::string& v) {
    if (row < 0 || row >= (int)prog.placements.size()) return;
    PnpPlacement q = prog.placements[(size_t)row];
    double d;
    switch (col) {
        case 1: if (v == q.ref) return; q.ref = v; break;
        case 3: if (!parse_d(v.c_str(), d) || d == q.x) return; q.x = d; break;
        case 4: if (!parse_d(v.c_str(), d) || d == q.y) return; q.y = d; break;
        case 5: if (!parse_d(v.c_str(), d) || d == q.rot) return; q.rot = d; break;
        default: return;
    }
    // multi-row edit of the same column when the row is part of the selection
    std::vector<int> rows{row};
    if (row < (int)sel.size() && sel[(size_t)row] && nsel() > 1 && col != 1)
        for (size_t i = 0; i < sel.size(); ++i) if (sel[i] && (int)i != row) rows.push_back((int)i);
    checkpoint(rows.size() > 1 ? "Edit " + std::to_string(rows.size()) + " placements" : "Edit " + q.ref);
    for (int r : rows) {
        auto& t = prog.placements[(size_t)r];
        if (col == 1) t.ref = q.ref;
        else if (col == 3) t.x = q.x;
        else if (col == 4) t.y = q.y;
        else if (col == 5) t.rot = q.rot;
    }
    changed();
}

void PnpDoc::commit_part(int row, int col, const std::string& v) {
    if (row < 0 || row >= (int)prog.parts.size()) return;
    PnpPart& k = prog.parts[(size_t)row];
    double d;
    switch (col) {
        case 1: {
            if (v == k.name || v.empty()) return;
            for (auto& o : prog.parts) if (o.name == v) { toast("Part name '" + v + "' already exists", ToastKind::Warning); return; }
            checkpoint("Rename part " + k.name);
            prog.parts[(size_t)row].name = v;
            if (prog.kind == io::PnpKind::Iss) msg = "rename cascades to every <componentName> on save";
            break;
        }
        case 2: if (v == k.desc) return; checkpoint("Edit description"); prog.parts[(size_t)row].desc = v; break;
        case 4: case 5: case 6: {
            if (!parse_d(v.c_str(), d)) return;
            double& f = col == 4 ? k.len : col == 5 ? k.wid : k.hgt;
            if (f == d) return;
            checkpoint("Edit body size");
            double& g = col == 4 ? prog.parts[(size_t)row].len : col == 5 ? prog.parts[(size_t)row].wid : prog.parts[(size_t)row].hgt;
            g = d;
            PnpPart& kk = prog.parts[(size_t)row];
            kk.has_body = kk.len > 0 && kk.wid > 0;
            break;
        }
        default: return;
    }
    changed();
}

void PnpDoc::draw_toolbar() {
    const GTheme& t = th();
    ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(S(2), S(3)));
    ImGui::Dummy(ImVec2(S(1), S(28)));
    if (fe_tool("##save", ui::Icon::Save, "Save (Ctrl+S)", true)) defer([this]() { save(false); });
    if (fe_tool("##saveas", ui::Icon::SaveAs, "Save As / convert format\xE2\x80\xA6")) defer([this]() { save(true); });
    ImGui::SameLine(0, S(6)); toolbar_separator();
    if (fe_tool("##undo", ui::Icon::Undo, undo_.empty() ? "Undo" : ("Undo " + undo_.back().label + " (Ctrl+Z)").c_str(), !undo_.empty())) do_undo();
    if (fe_tool("##redo", ui::Icon::Redo, redo_.empty() ? "Redo" : ("Redo " + redo_.back().label + " (Ctrl+Y)").c_str(), !redo_.empty())) do_redo();
    ImGui::SameLine(0, S(6)); toolbar_separator();
    if (fe_tool("##zall", ui::Icon::ZoomAll, "Zoom to board (Home)")) cv.zoom_all();
    if (fe_tool("##zsel", ui::Icon::ZoomRect, "Zoom to selection", nsel() > 0)) focus_selection();
    if (fe_tool("##lbl", ui::Icon::Tag, "Reference labels", true, show_labels)) { show_labels = !show_labels; cv.invalidate(); }
    if (fe_tool("##fid", ui::Icon::Fiducial, "Marks / fiducials", true, show_marks)) { show_marks = !show_marks; cv.invalidate(); }
    if (fe_tool("##ckt", ui::Icon::Panelize, "Circuit outlines", true, show_ckt)) { show_ckt = !show_ckt; cv.invalidate(); }
    if (fe_tool("##gh", ui::Icon::AllImages, "Ghost placements in the other circuits (root-circuit programs)", true, show_ghosts)) { show_ghosts = !show_ghosts; cv.invalidate(); }
    ImGui::SameLine(0, S(6)); toolbar_separator();
    if (fe_tool("##csv", ui::Icon::Export, "Export placements as CSV\xE2\x80\xA6")) defer([this]() { export_csv(); });
    if (fe_tool("##prj", ui::Icon::Wand, "Create PCB Plotter project from this program")) defer([this]() { create_project(); });
    if (fe_tool("##cmp", ui::Icon::Compare, "Compare with an open project's machine point\xE2\x80\xA6")) defer([this]() { compare_project(); });
    ImGui::SameLine(0, S(10));
    // format badge
    {
        std::string b = prog.format + (prog.board.target.empty() || prog.board.target == "?" ? "" : "  \xC2\xB7  " + prog.board.target);
        ImVec2 p = ImGui::GetCursorScreenPos() + ImVec2(0, S(4));
        ImVec2 ts = ImGui::CalcTextSize(b.c_str());
        ImGui::GetWindowDrawList()->AddRectFilled(p, p + ts + ImVec2(S(14), S(6)), col_alpha(t.accent, 0.18f), S(8));
        ImGui::GetWindowDrawList()->AddText(p + ImVec2(S(7), S(3)), t.text, b.c_str());
        ImGui::Dummy(ts + ImVec2(S(14), S(10)));
    }
    ImGui::SameLine(0, S(8));
    const int ne = count_issues(2), nw = count_issues(1);
    {
        char b[64];
        std::snprintf(b, sizeof b, "%d errors, %d warnings##val", ne, nw);
        ImGui::PushStyleColor(ImGuiCol_Text, ne ? t.error : nw ? t.warn : t.ok);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(1));
        if (button(b, ne ? ui::Icon::ErrorX : nw ? ui::Icon::Warning : ui::Icon::Check)) want_tab = T_VALID;
        ImGui::PopStyleColor();
    }
    ImGui::SameLine(0, S(8));
    float w = std::min(S(240), ImGui::GetContentRegionAvail().x - S(8));
    if (w > S(80)) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(2));
        if (search_box("##pnpfind", find, sizeof find, "Find ref / part", w)) {}
        if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Enter) && find[0]) {
            rebuild_view();
            sync_sel();
            std::fill(sel.begin(), sel.end(), 0);
            for (int i : view) sel[(size_t)i] = 1;
            ++sel_gen;
            if (!view.empty()) { scroll_to = view[0]; want_tab = T_PLACE; focus_selection(); }
        }
    }
}

void PnpDoc::draw_canvas_menu() {
    if (!ImGui::BeginPopup("##pnp_canvas_ctx")) return;
    const int n = nsel();
    ImGui::TextDisabled("%d selected", n);
    ImGui::Separator();
    if (menu_item_icon("Zoom to selection", ui::Icon::ZoomRect, nullptr, false, n > 0)) focus_selection();
    if (menu_item_icon("Skip", ui::Icon::EyeOff, nullptr, false, n > 0)) set_skip(1);
    if (menu_item_icon("Unskip", ui::Icon::Eye, nullptr, false, n > 0)) set_skip(0);
    if (cursor >= 0 && cursor < (int)prog.placements.size() && menu_item_icon("Select same part", ui::Icon::SelectPart, nullptr, false, prog.placements[(size_t)cursor].part >= 0))
        select_part(prog.placements[(size_t)cursor].part);
    if (menu_item_icon("Duplicate", ui::Icon::Copy, nullptr, false, n > 0 && prog.can_restructure())) duplicate_selected();
    if (menu_item_icon("Delete", ui::Icon::Delete, "Del", false, n > 0 && prog.can_restructure())) delete_selected();
    ImGui::EndPopup();
}

void PnpDoc::draw_placements(ImVec2 sz) {
    const GTheme& t = th();
    sync_sel();
    const bool rs = prog.can_restructure();
    const int n = nsel();
    // bulk bar
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", n ? (std::to_string(n) + " selected").c_str() : "No selection: bulk tools apply to all rows");
        ImGui::SameLine(0, S(10));
        if (button("Transform\xE2\x80\xA6", ui::Icon::Move)) ImGui::OpenPopup("##xf");
        ImGui::SameLine(0, S(4));
        if (button("Skip", ui::Icon::EyeOff)) ImGui::OpenPopup("##skipm");
        ImGui::SameLine(0, S(4));
        if (button("Order", ui::Icon::ArrowDown, false, ImVec2(0, 0), rs)) ImGui::OpenPopup("##sortm");
        ImGui::SameLine(0, S(4));
        if (button("Duplicate", ui::Icon::Copy, false, ImVec2(0, 0), rs && n > 0)) duplicate_selected();
        ImGui::SameLine(0, S(4));
        if (button("Delete", ui::Icon::Delete, false, ImVec2(0, 0), rs && n > 0)) delete_selected();
        if (!rs && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            tooltip(prog.kind == io::PnpKind::Iss ? "ISS programs: field edits only (rows cannot be added, deleted or reordered)" : "This program's tables cannot be restructured (see Board > notes)");
        if (ImGui::BeginPopup("##skipm")) {
            if (menu_item_icon("Skip", ui::Icon::EyeOff)) set_skip(1);
            if (menu_item_icon("Unskip", ui::Icon::Eye)) set_skip(0);
            if (menu_item_icon("Toggle", ui::Icon::Flip)) set_skip(2);
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##sortm")) {
            ImGui::TextDisabled("Reorders the program (step numbers)");
            if (menu_item_icon("By part number", ui::Icon::SelectPart)) sort_program(0);
            if (menu_item_icon("By position (rows, then X)", ui::Icon::Locations)) sort_program(1);
            if (menu_item_icon("By reference", ui::Icon::FindRef)) sort_program(2);
            if (menu_item_icon("By circuit, then part", ui::Icon::Panelize)) sort_program(3);
            ImGui::Separator();
            if (menu_item_icon("Renumber steps in the current view order", ui::Icon::Check, nullptr, false, sort_col >= 0)) sort_program(4);
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##xf")) {
            ImGui::TextDisabled("%s", n ? "Selected placements" : "All placements");
            ImGui::SetNextItemWidth(S(110)); ImGui::InputFloat("Offset X (mm)", &t_dx, 0, 0, "%.4f");
            ImGui::SetNextItemWidth(S(110)); ImGui::InputFloat("Offset Y (mm)", &t_dy, 0, 0, "%.4f");
            ImGui::SetNextItemWidth(S(110)); ImGui::InputFloat("Rotate (\xC2\xB0 CCW)", &t_rot, 0, 0, "%.3f");
            ImGui::Checkbox("Mirror X", &t_mx); ImGui::SameLine(); ImGui::Checkbox("Mirror Y", &t_my);
            ImGui::TextUnformatted("Pivot");
            ImGui::RadioButton("Selection centre", &t_about, 0); ImGui::SameLine();
            ImGui::RadioButton("Origin", &t_about, 1); ImGui::SameLine();
            ImGui::RadioButton("Each placement", &t_about, 2);
            if (button("Apply", ui::Icon::Check, true)) { apply_transform(); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (button("Reset")) { t_dx = t_dy = t_rot = 0; t_mx = t_my = false; }
            ImGui::EndPopup();
        }
    }
    rebuild_view();
    struct Col { const char* name; float w; int id; };
    std::vector<Col> cols = {{"Step", 48, 0}, {"Ref", 70, 1}, {"Part", 150, 2}, {"X", 80, 3}, {"Y", 80, 4}, {"Angle", 62, 5}};
    if (prog.circuits.size() > 1) cols.push_back({"Circ", 40, 6});
    cols.push_back({"Skip", 38, 7});
    cols.push_back({"Station", 52, 8});
    if (prog.kind == io::PnpKind::Hlc) { cols.push_back({"Local", 46, 9}); cols.push_back({"Status", 50, 10}); }
    else { cols.push_back({"Head", 46, 11}); cols.push_back({"Action", 70, 12}); }
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (prog.placements.empty()) { empty_state(ui::Icon::Locations, "No placements", "This program holds no placement records."); return; }
    const ImGuiTableFlags fl = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate | ImGuiTableFlags_Reorderable;
    if (!ImGui::BeginTable("##place", (int)cols.size(), fl, avail)) return;
    ImGui::TableSetupScrollFreeze(2, 1);
    for (auto& c : cols) ImGui::TableSetupColumn(c.name, ImGuiTableColumnFlags_WidthFixed, S(c.w), (ImGuiID)c.id);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs()) {
        if (ss->SpecsDirty) {
            if (ss->SpecsCount > 0) { sort_col = (int)ss->Specs[0].ColumnUserID; sort_desc = ss->Specs[0].SortDirection == ImGuiSortDirection_Descending; if (sort_col == 0) sort_col = -1; }
            else sort_col = -1;
            view_gen = ~0ull;
            ss->SpecsDirty = false;
            rebuild_view();
        }
    }
    int scroll_row = -1;
    if (scroll_to >= 0) {
        for (size_t k = 0; k < view.size(); ++k) if (view[k] == scroll_to) { scroll_row = (int)k; break; }
        scroll_to = -1;
    }
    const float rh = ImGui::GetFrameHeight();
    if (scroll_row >= 0) {
        float y = scroll_row * rh;
        float sy = ImGui::GetScrollY(), wh = ImGui::GetWindowHeight() - rh * 2;
        if (y < sy || y > sy + wh) ImGui::SetScrollY(std::max(0.f, y - wh * 0.4f));
    }
    ImGuiListClipper clip;
    clip.Begin((int)view.size(), rh);
    int clicked = -1, clicked_col = -1; bool dbl = false;
    while (clip.Step()) {
        for (int vr = clip.DisplayStart; vr < clip.DisplayEnd; ++vr) {
            const int i = view[(size_t)vr];
            PnpPlacement& q = prog.placements[(size_t)i];
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rh);
            ImGui::PushID(i);
            const bool s = sel[(size_t)i];
            const float fa = flash_amount(ImGui::GetID("fl"));
            if (fa > 0) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, col_alpha(t.accent, 0.35f * fa));
            if (cv.hover == i) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, col_alpha(t.accent, 0.12f));
            for (size_t c = 0; c < cols.size(); ++c) {
                ImGui::TableSetColumnIndex((int)c);
                const int id = cols[c].id;
                if (c == 0) {
                    char b[16]; std::snprintf(b, sizeof b, "%d", i + 1);
                    ImGui::AlignTextToFramePadding();
                    if (ImGui::Selectable(b, s, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rh))) {
                        clicked = i; clicked_col = ImGui::TableGetHoveredColumn(); dbl = ImGui::IsMouseDoubleClicked(0);
                        if (clicked_col >= 0 && clicked_col < (int)cols.size()) clicked_col = cols[(size_t)clicked_col].id;
                    }
                    if (ImGui::BeginPopupContextItem("##rowctx")) {
                        if (!s) select_only(i);
                        ImGui::TextDisabled("%s", q.ref.c_str());
                        if (menu_item_icon("Zoom to", ui::Icon::ZoomRect)) focus_selection();
                        if (menu_item_icon("Select same part", ui::Icon::SelectPart, nullptr, false, q.part >= 0)) select_part(q.part);
                        if (menu_item_icon("Toggle skip", ui::Icon::Flip)) set_skip(2);
                        if (menu_item_icon("Duplicate", ui::Icon::Copy, nullptr, false, rs)) duplicate_selected();
                        if (menu_item_icon("Delete", ui::Icon::Delete, "Del", false, rs)) delete_selected();
                        ImGui::EndPopup();
                    }
                    continue;
                }
                if (editing(T_PLACE, i, id)) {
                    if (id == 2) {
                        ImGui::TextUnformatted(part_name(q.part).c_str());
                        if (ed_focus) { ImGui::OpenPopup("##partpick"); ed_focus = false; ebuf[0] = 0; }
                        if (ImGui::BeginPopup("##partpick")) {
                            ImGui::TextDisabled("Part for %s%s", q.ref.c_str(), s && nsel() > 1 ? " and the selection" : "");
                            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                            search_box("##pf", ebuf, sizeof ebuf, "Filter parts", S(240));
                            ImGui::BeginChild("##pl", ImVec2(S(240), S(220)));
                            for (size_t k = 0; k < prog.parts.size(); ++k) {
                                const PnpPart& pk = prog.parts[k];
                                if (!icontains(pk.name, ebuf) && !icontains(pk.desc, ebuf)) continue;
                                if (ImGui::Selectable(pk.name.c_str(), (int)k == q.part)) {
                                    std::vector<int> rows{i};
                                    if (s && nsel() > 1) for (size_t r = 0; r < sel.size(); ++r) if (sel[r] && (int)r != i) rows.push_back((int)r);
                                    checkpoint("Set part");
                                    for (int r : rows) prog.placements[(size_t)r].part = (int)k;
                                    changed();
                                    ed_tab = -1;
                                    ImGui::CloseCurrentPopup();
                                }
                                if (!pk.desc.empty() && ImGui::IsItemHovered()) tooltip(pk.desc.c_str());
                            }
                            ImGui::EndChild();
                            ImGui::EndPopup();
                        } else ed_tab = -1;
                        continue;
                    }
                    bool canc = false;
                    if (cell_edit(T_PLACE, i, id, &canc)) commit_place(i, id, ebuf);
                    continue;
                }
                ImGui::AlignTextToFramePadding();
                switch (id) {
                    case 1: ImGui::TextUnformatted(q.ref.c_str()); break;
                    case 2: if (q.part >= 0) ImGui::TextUnformatted(part_name(q.part).c_str()); else ImGui::TextColored(ImColor(t.error), "(none)"); break;
                    case 3: ImGui::TextUnformatted(f4(q.x).c_str()); break;
                    case 4: ImGui::TextUnformatted(f4(q.y).c_str()); break;
                    case 5: ImGui::TextUnformatted(f4(q.rot).c_str()); break;
                    case 6: ImGui::Text("%d", q.circuit + 1); break;
                    case 7: {
                        bool b = q.skip;
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(2), S(2)));
                        if (ImGui::Checkbox("##sk", &b)) {
                            checkpoint(b ? "Skip " + q.ref : "Unskip " + q.ref);
                            if (s && nsel() > 1) { for (size_t r = 0; r < sel.size(); ++r) if (sel[r]) prog.placements[r].skip = b; }
                            else prog.placements[(size_t)i].skip = b;
                            changed();
                        }
                        ImGui::PopStyleVar();
                        break;
                    }
                    case 8: if (q.station) ImGui::Text("%d", q.station); break;
                    case 9: if (q.has_local) ImGui::Text("M%03d", q.mark_set + 1); break;
                    case 10: ImGui::TextDisabled("0x%02X", q.status); break;
                    case 11: if (q.head) ImGui::Text("%d/%d", q.head / 100, q.head % 100); break;
                    case 12: ImGui::TextDisabled("%s", q.action.c_str()); break;
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    if (clicked >= 0) {
        ImGuiIO& io = ImGui::GetIO();
        if (dbl) {
            const PnpPlacement& q = prog.placements[(size_t)clicked];
            switch (clicked_col) {
                case 1: begin_edit(T_PLACE, clicked, 1, q.ref); break;
                case 2: begin_edit(T_PLACE, clicked, 2, ""); break;
                case 3: begin_edit(T_PLACE, clicked, 3, f4(q.x)); break;
                case 4: begin_edit(T_PLACE, clicked, 4, f4(q.y)); break;
                case 5: begin_edit(T_PLACE, clicked, 5, f4(q.rot)); break;
                default: focus_selection(); break;
            }
        } else if (io.KeyCtrl) { sel[(size_t)clicked] ^= 1; cursor = clicked; }
        else if (io.KeyShift && cursor >= 0) {
            int a = -1, b = -1;
            for (size_t k = 0; k < view.size(); ++k) { if (view[k] == cursor) a = (int)k; if (view[k] == clicked) b = (int)k; }
            if (a >= 0 && b >= 0) { if (a > b) std::swap(a, b); for (int k = a; k <= b; ++k) sel[(size_t)view[(size_t)k]] = 1; }
        } else { select_only(clicked); }
        ++sel_gen;
    }
    // keyboard
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput && ed_tab < 0) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && n && rs) delete_selected();
        if (ImGui::IsKeyPressed(ImGuiKey_F2) && cursor >= 0 && cursor < (int)prog.placements.size()) begin_edit(T_PLACE, cursor, 1, prog.placements[(size_t)cursor].ref);
        if (ImGui::IsKeyPressed(ImGuiKey_Space) && n) set_skip(2);
        if ((ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) && !view.empty()) {
            int k = 0;
            for (size_t v = 0; v < view.size(); ++v) if (view[v] == cursor) k = (int)v;
            k = std::clamp(k + (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : -1), 0, (int)view.size() - 1);
            select_only(view[(size_t)k]);
            scroll_to = view[(size_t)k];
        }
    }
    (void)sz;
}

void PnpDoc::draw_parts(ImVec2) {
    const GTheme& t = th();
    std::vector<int> used(prog.parts.size(), 0);
    for (auto& q : prog.placements) if (q.part >= 0 && q.part < (int)used.size()) used[(size_t)q.part]++;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%zu parts  \xC2\xB7  double-click to edit%s", prog.parts.size(),
                        prog.kind == io::PnpKind::Iss ? "  \xC2\xB7  renames cascade to every reference" : "  \xC2\xB7  placements link by index: renames are safe");
    ImGui::SameLine(0, S(10));
    if (button("Select placements", ui::Icon::SelectPart, false, ImVec2(0, 0), part_sel >= 0)) select_part(part_sel);
    const bool body = prog.part_body_editable();
    if (prog.parts.empty()) { empty_state(ui::Icon::Bom, "No component records"); return; }
    const ImGuiTableFlags fl = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##parts", 11, fl, ImGui::GetContentRegionAvail())) return;
    ImGui::TableSetupScrollFreeze(2, 1);
    const char* hd[] = {"#", "Part", "Description", "Package / feeder", "Length", "Width", "Height", "Type", "Skip", "Used", "Stored"};
    const float wd[] = {34, 160, 200, 90, 60, 60, 60, 44, 38, 42, 50};
    for (int c = 0; c < 11; ++c) ImGui::TableSetupColumn(hd[c], ImGuiTableColumnFlags_WidthFixed, S(wd[c]));
    ImGui::TableHeadersRow();
    const float rh = ImGui::GetFrameHeight();
    ImGuiListClipper clip;
    clip.Begin((int)prog.parts.size(), rh);
    int dbl_row = -1, dbl_col = -1;
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            PnpPart& k = prog.parts[(size_t)r];
            ImGui::TableNextRow(0, rh);
            ImGui::PushID(r);
            ImGui::TableSetColumnIndex(0);
            char b[16]; std::snprintf(b, sizeof b, "%d", r);
            ImGui::AlignTextToFramePadding();
            if (ImGui::Selectable(b, part_sel == r, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rh))) {
                part_sel = r;
                if (ImGui::IsMouseDoubleClicked(0)) { dbl_row = r; dbl_col = ImGui::TableGetHoveredColumn(); }
            }
            for (int c = 1; c < 11; ++c) {
                ImGui::TableSetColumnIndex(c);
                if (editing(T_PARTS, r, c)) { if (cell_edit(T_PARTS, r, c)) commit_part(r, c, ebuf); continue; }
                ImGui::AlignTextToFramePadding();
                switch (c) {
                    case 1: ImGui::TextUnformatted(k.name.c_str()); if ((int)k.name.size() > prog.part_cap()) { ImGui::SameLine(); ImGui::TextColored(ImColor(t.warn), "!"); } break;
                    case 2: ImGui::TextUnformatted(k.desc.c_str()); break;
                    case 3: ImGui::TextDisabled("%s", k.package.c_str()); break;
                    case 4: if (k.has_body || k.len) ImGui::TextUnformatted(f4(k.len).c_str()); else ImGui::TextDisabled("-"); break;
                    case 5: if (k.has_body || k.wid) ImGui::TextUnformatted(f4(k.wid).c_str()); else ImGui::TextDisabled("-"); break;
                    case 6: if (k.hgt) ImGui::TextUnformatted(f4(k.hgt).c_str()); else ImGui::TextDisabled("-"); break;
                    case 7: if (k.type >= 0) ImGui::TextDisabled("%d", k.type); break;
                    case 8:
                        if (k.skip_known) {
                            bool v = k.skip;
                            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(2), S(2)));
                            if (ImGui::Checkbox("##cs", &v)) { checkpoint(v ? "Skip part " + k.name : "Unskip part " + k.name); prog.parts[(size_t)r].skip = v; changed(); }
                            ImGui::PopStyleVar();
                        } else ImGui::TextDisabled("-");
                        break;
                    case 9: if (used[(size_t)r]) ImGui::Text("%d", used[(size_t)r]); else ImGui::TextColored(ImColor(t.text_faint), "0"); break;
                    case 10: if (prog.kind == io::PnpKind::Hlc) ImGui::TextDisabled("%d", k.stored_count); break;
                }
            }
            ImGui::PopID();
        }
    ImGui::EndTable();
    if (dbl_row >= 0) {
        const PnpPart& k = prog.parts[(size_t)dbl_row];
        switch (dbl_col) {
            case 1: begin_edit(T_PARTS, dbl_row, 1, k.name); break;
            case 2: begin_edit(T_PARTS, dbl_row, 2, k.desc); break;
            case 4: case 5: case 6:
                if (body) begin_edit(T_PARTS, dbl_row, dbl_col, f4(dbl_col == 4 ? k.len : dbl_col == 5 ? k.wid : k.hgt));
                else toast(prog.format + " component records carry no body size", ToastKind::Info);
                break;
            default: select_part(dbl_row); break;
        }
    }
}

void PnpDoc::draw_circuits(ImVec2) {
    const GTheme& t = th();
    const io::PnpBoard& B = prog.board;
    ImGui::AlignTextToFramePadding();
    const char* lay = B.layout == 1 ? "Matrix (edit pitch / divisions on the Board tab)" : B.layout == 2 ? "Non-matrix (explicit circuit list)" : "Single board";
    ImGui::TextDisabled("%s", lay);
    const bool hlc_list = prog.kind == io::PnpKind::Hlc && B.layout == 2;
    if (hlc_list) {
        ImGui::SameLine(0, S(10));
        if (button("Add circuit", ui::Icon::Plus)) {
            checkpoint("Add circuit");
            io::PnpCircuit c = prog.circuits.empty() ? io::PnpCircuit{} : prog.circuits.back();
            c.x += B.ckt_w > 0 ? B.ckt_w : 10;
            prog.circuits.push_back(c);
            changed();
        }
        ImGui::SameLine(0, S(4));
        if (button("Remove last", ui::Icon::Delete, false, ImVec2(0, 0), prog.circuits.size() > 1)) {
            checkpoint("Remove circuit");
            prog.circuits.pop_back();
            changed();
        }
    }
    if (prog.circuits.empty()) { empty_state(ui::Icon::Panelize, "Single board", "This program has no circuit (panel) list."); return; }
    std::vector<int> cnt(prog.circuits.size(), 0), skp(prog.circuits.size(), 0);
    for (auto& q : prog.placements)
        if (q.circuit >= 0 && q.circuit < (int)cnt.size()) { cnt[(size_t)q.circuit]++; skp[(size_t)q.circuit] += q.skip; }
    const ImGuiTableFlags fl = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##ckt", 6, fl, ImGui::GetContentRegionAvail())) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    const char* hd[] = {"#", "X offset", "Y offset", "Angle", "Placements", "Skip circuit"};
    const float wd[] = {34, 90, 90, 70, 80, 120};
    for (int c = 0; c < 6; ++c) ImGui::TableSetupColumn(hd[c], ImGuiTableColumnFlags_WidthFixed, S(wd[c]));
    ImGui::TableHeadersRow();
    const bool editable = prog.circuits_editable();
    int dbl_row = -1, dbl_col = -1;
    for (int r = 0; r < (int)prog.circuits.size(); ++r) {
        io::PnpCircuit& c = prog.circuits[(size_t)r];
        ImGui::TableNextRow(0, ImGui::GetFrameHeight());
        ImGui::PushID(r);
        ImGui::TableSetColumnIndex(0);
        char b[16]; std::snprintf(b, sizeof b, "%d", r + 1);
        ImGui::AlignTextToFramePadding();
        if (ImGui::Selectable(b, circ_sel == r, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick)) {
            circ_sel = r; cv.invalidate();
            if (ImGui::IsMouseDoubleClicked(0)) { dbl_row = r; dbl_col = ImGui::TableGetHoveredColumn(); }
        }
        for (int k = 1; k < 4; ++k) {
            ImGui::TableSetColumnIndex(k);
            if (editing(T_CIRC, r, k)) {
                if (cell_edit(T_CIRC, r, k)) {
                    double v;
                    double& f = k == 1 ? c.x : k == 2 ? c.y : c.rot;
                    if (parse_d(ebuf, v) && v != f) {
                        checkpoint("Edit circuit " + std::to_string(r + 1));
                        io::PnpCircuit& cc = prog.circuits[(size_t)r];
                        (k == 1 ? cc.x : k == 2 ? cc.y : cc.rot) = v;
                        changed();
                    }
                }
                continue;
            }
            ImGui::AlignTextToFramePadding();
            const double v = k == 1 ? c.x : k == 2 ? c.y : c.rot;
            if (c.derived) ImGui::TextDisabled("%s", f4(v).c_str()); else ImGui::TextUnformatted(f4(v).c_str());
        }
        ImGui::TableSetColumnIndex(4);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d", cnt[(size_t)r]);
        if (skp[(size_t)r]) { ImGui::SameLine(); ImGui::TextColored(ImColor(t.warn), "(%d skipped)", skp[(size_t)r]); }
        ImGui::TableSetColumnIndex(5);
        if (cnt[(size_t)r]) {
            const bool all = skp[(size_t)r] == cnt[(size_t)r];
            if (ImGui::SmallButton(all ? "Unskip all" : "Skip all")) {
                checkpoint((all ? "Unskip circuit " : "Skip circuit ") + std::to_string(r + 1));
                for (auto& q : prog.placements) if (q.circuit == r) q.skip = !all;
                changed();
            }
            if (ImGui::IsItemHovered()) tooltip("Sets the skip flag of every placement in this circuit (HLC / ISS have no per-circuit skip field)");
        } else ImGui::TextDisabled("-");
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (dbl_row >= 0 && dbl_col >= 1 && dbl_col <= 3) {
        const auto& c = prog.circuits[(size_t)dbl_row];
        if (!editable || c.derived) toast(c.derived ? "Matrix circuits are generated: edit pitch / divisions on the Board tab" : "Circuits are not editable in this program", ToastKind::Info);
        else begin_edit(T_CIRC, dbl_row, dbl_col, f4(dbl_col == 1 ? c.x : dbl_col == 2 ? c.y : c.rot));
    }
}

void PnpDoc::draw_marks(ImVec2) {
    if (prog.marks.empty()) { empty_state(ui::Icon::Fiducial, "No marks", "This program has no fiducial / bad marks."); return; }
    const ImGuiTableFlags fl = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##marks", 7, fl, ImGui::GetContentRegionAvail())) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    const char* hd[] = {"#", "Kind", "Set", "Name", "X", "Y", "Used by"};
    const float wd[] = {30, 70, 40, 110, 80, 80, 140};
    for (int c = 0; c < 7; ++c) ImGui::TableSetupColumn(hd[c], ImGuiTableColumnFlags_WidthFixed, S(wd[c]));
    ImGui::TableHeadersRow();
    int dbl_row = -1, dbl_col = -1;
    for (int r = 0; r < (int)prog.marks.size(); ++r) {
        io::PnpMark& m = prog.marks[(size_t)r];
        ImGui::TableNextRow(0, ImGui::GetFrameHeight());
        ImGui::PushID(r);
        ImGui::TableSetColumnIndex(0);
        char b[16]; std::snprintf(b, sizeof b, "%d", r + 1);
        ImGui::AlignTextToFramePadding();
        if (ImGui::Selectable(b, mark_sel == r, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick)) {
            mark_sel = r; cv.invalidate();
            if (ImGui::IsMouseDoubleClicked(0)) { dbl_row = r; dbl_col = ImGui::TableGetHoveredColumn(); }
        }
        ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(io::pnp_mark_kind_name(m.kind));
        ImGui::TableSetColumnIndex(2); ImGui::AlignTextToFramePadding(); if (m.set >= 0) ImGui::TextDisabled("%d", m.set); else ImGui::TextDisabled("board");
        for (int k = 3; k < 6; ++k) {
            ImGui::TableSetColumnIndex(k);
            if (editing(T_MARKS, r, k)) {
                if (cell_edit(T_MARKS, r, k)) {
                    double v;
                    if (k == 3 && std::string(ebuf) != m.name) { checkpoint("Rename mark"); prog.marks[(size_t)r].name = ebuf; changed(); }
                    else if (k > 3 && parse_d(ebuf, v) && v != (k == 4 ? m.x : m.y)) {
                        checkpoint("Move mark " + m.name);
                        (k == 4 ? prog.marks[(size_t)r].x : prog.marks[(size_t)r].y) = v;
                        changed();
                    }
                }
                continue;
            }
            ImGui::AlignTextToFramePadding();
            if (k == 3) ImGui::TextUnformatted(m.name.c_str());
            else ImGui::TextUnformatted(f4(k == 4 ? m.x : m.y).c_str());
        }
        ImGui::TableSetColumnIndex(6);
        ImGui::AlignTextToFramePadding();
        if (m.kind == MarkKind::Local) {
            std::string u;
            for (auto& q : prog.placements) if (q.has_local && q.mark_set == m.set) { u += (u.empty() ? "" : ", ") + q.ref; if (u.size() > 40) break; }
            ImGui::TextDisabled("%s", u.c_str());
        } else if (m.kind == MarkKind::Global) ImGui::TextDisabled("board offset correction");
        else ImGui::TextDisabled("circuit skip detection");
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (dbl_row >= 0) {
        const auto& m = prog.marks[(size_t)dbl_row];
        if (dbl_col == 3) {
            const bool named = prog.kind == io::PnpKind::Hlc && prog.format != "H5H" && prog.format != "H4H" && m.set >= 0 && !(prog.format.size() == 3 && prog.format[0] == 'E');
            if (named) begin_edit(T_MARKS, dbl_row, 3, m.name); else toast("Mark names are not stored in this format", ToastKind::Info);
        } else if (dbl_col == 4 || dbl_col == 5) begin_edit(T_MARKS, dbl_row, dbl_col, f4(dbl_col == 4 ? m.x : m.y));
        else cv.fly_to(BBox(m.x - 8, m.y - 8, m.x + 8, m.y + 8));
    }
}

void PnpDoc::draw_board(ImVec2) {
    if (bform_gen != gen) { bform = prog.board; bform_gen = gen; }
    io::PnpBoard& b = bform;
    const bool hlc = prog.kind == io::PnpKind::Hlc;
    bool commit = false;
    auto done = [&]() { if (ImGui::IsItemDeactivatedAfterEdit()) commit = true; };
    auto text = [&](const char* label, std::string& s, bool enabled = true, const char* tip = nullptr) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s", s.c_str());
        ImGui::BeginDisabled(!enabled);
        ImGui::SetNextItemWidth(S(260));
        if (ImGui::InputText(label, buf, sizeof buf)) s = buf;
        done();
        ImGui::EndDisabled();
        if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip(tip);
    };
    auto num2 = [&](const char* label, double& x, double& y, bool enabled = true) {
        double v[2] = {x, y};
        ImGui::BeginDisabled(!enabled);
        ImGui::SetNextItemWidth(S(260));
        if (ImGui::InputScalarN(label, ImGuiDataType_Double, v, 2, nullptr, nullptr, "%.4f")) { x = v[0]; y = v[1]; }
        done();
        ImGui::EndDisabled();
    };
    ImGui::BeginChild("##boardform", ImGui::GetContentRegionAvail(), ImGuiChildFlags_None);
    section_header("PROGRAM");
    text("Program name", b.program_name, true, hlc ? "HLC header +0 (32 bytes)" : "ISS <pwbId>");
    if (hlc) text("Board ID", b.board_name, true, "HLC board record +0");
    text("Line name", b.line_name);
    if (hlc) text("Comment", b.comment, true, "HLC line info +21 (32 characters)");
    ImGui::TextDisabled("Format %s   \xC2\xB7   target %s", prog.format.c_str(), b.target.c_str());
    if (b.timestamp) {
        time_t ts = (time_t)b.timestamp;
        char tb[64] = "";
        if (std::tm* tm = std::gmtime(&ts)) std::strftime(tb, sizeof tb, "%Y-%m-%d %H:%M UTC", tm);
        ImGui::TextDisabled("Last edited stamp %s (kept as is)", tb);
    }
    ImGui::Spacing();
    section_header("BOARD");
    num2("Size W x H (mm)", b.w, b.h);
    num2(hlc ? "PCB layout origin (mm)" : "Layout offset (mm)", b.origin_x, b.origin_y);
    if (b.has_thick) {
        ImGui::SetNextItemWidth(S(126));
        ImGui::InputDouble("Thickness (mm)", &b.thick, 0, 0, "%.3f");
        done();
    }
    if (hlc) {
        int tr = b.rl ? 1 : 0, fr = b.back ? 1 : 0;
        ImGui::SetNextItemWidth(S(160));
        if (ImGui::Combo("Transport", &tr, "Left \xE2\x86\x92 Right\0Right \xE2\x86\x92 Left\0")) { b.rl = tr == 1; commit = true; }
        ImGui::SetNextItemWidth(S(160));
        if (ImGui::Combo("Fixed rail", &fr, "Front\0Back\0")) { b.back = fr == 1; commit = true; }
        ImGui::TextDisabled("Mode flags 0x%04X%s%s%s", b.mode, (b.mode & 0x20) ? "  global BOC" : "", (b.mode & 0x40) ? "  local BOC" : "",
                            (b.mode & 0x08) ? "  matrix" : (b.mode & 0x10) ? "  non-matrix" : "");
    }
    ImGui::Spacing();
    section_header("CIRCUIT");
    num2("Circuit W x H (mm)", b.ckt_w, b.ckt_h);
    num2("Circuit origin (mm)", b.ckt_x, b.ckt_y);
    if (b.layout == 1) {
        int dv[2] = {b.div_x, b.div_y};
        ImGui::SetNextItemWidth(S(260));
        if (ImGui::InputInt2("Divisions X x Y", dv)) { b.div_x = std::clamp(dv[0], 1, 100); b.div_y = std::clamp(dv[1], 1, 100); }
        done();
        num2("Matrix pitch (mm)", b.pitch_x, b.pitch_y);
        num2("First circuit (mm)", b.first_x, b.first_y);
    }
    if (!prog.notes.empty()) {
        ImGui::Spacing();
        section_header("LOAD NOTES");
        for (auto& n : prog.notes) ImGui::BulletText("%s", n.c_str());
    }
    ImGui::Spacing();
    section_header("FORMAT KNOWLEDGE");
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextDisabled("%s", hlc ? "Edited here: names, board / circuit geometry, transport, circuit list or matrix, placements (ref, X, Y, angle, skip, "
                                    "component link, order, add / delete), component name / description / body / skip, mark positions. Feeder, nozzle, "
                                    "vision, station and MTC data are preserved byte for byte but not shown."
                                  : "ISS is edited by patching values in place: everything else in the XML (feeders, stations, optimisation, vision) is "
                                    "kept verbatim. Rows cannot be added, removed or reordered.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    if (commit) {
        io::PnpBoard nb = b;
        if (nb.layout == 1 && (nb.div_x != prog.board.div_x || nb.div_y != prog.board.div_y || nb.pitch_x != prog.board.pitch_x || nb.pitch_y != prog.board.pitch_y)) {
            checkpoint("Edit board");
            prog.board = nb;
            prog.circuits.clear();
            for (int j = 0; j < nb.div_y; ++j)
                for (int i = 0; i < nb.div_x; ++i) { io::PnpCircuit c; c.x = i * nb.pitch_x; c.y = j * nb.pitch_y; c.derived = true; prog.circuits.push_back(c); }
            changed();
        } else {
            checkpoint("Edit board");
            prog.board = nb;
            changed();
        }
    }
}

void PnpDoc::draw_validation(ImVec2) {
    const GTheme& t = th();
    ensure_issues();
    if (issues.empty()) { empty_state(ui::Icon::Check, "No issues found", "Duplicates, links, board bounds, angles and marks all check out."); return; }
    static const char* tn[] = {"Placement", "Part", "Circuit", "Mark", "Board"};
    const ImGuiTableFlags fl = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##val", 3, fl, ImGui::GetContentRegionAvail())) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(26));
    ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthFixed, S(110));
    ImGui::TableSetupColumn("Issue", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)issues.size());
    int go = -1;
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            const auto& is = issues[(size_t)r];
            ImGui::TableNextRow();
            ImGui::PushID(r);
            ImGui::TableSetColumnIndex(0);
            icon_image(is.severity == 2 ? ui::Icon::ErrorX : is.severity == 1 ? ui::Icon::Warning : ui::Icon::Question, S(16));
            ImGui::TableSetColumnIndex(1);
            std::string w = tn[std::clamp(is.table, 0, 4)];
            if (is.row >= 0) w += " " + std::to_string(is.row + (is.table == 1 ? 0 : 1));
            if (ImGui::Selectable(w.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) go = r;
            ImGui::TableSetColumnIndex(2);
            ImGui::PushStyleColor(ImGuiCol_Text, is.severity == 2 ? t.error : is.severity == 1 ? t.text : t.text_dim);
            ImGui::TextUnformatted(is.text.c_str());
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    ImGui::EndTable();
    if (go >= 0) {
        const auto is = issues[(size_t)go];
        if (is.row < 0) { want_tab = is.table == 0 ? T_PLACE : is.table == 1 ? T_PARTS : is.table == 3 ? T_MARKS : T_BOARD; return; }
        switch (is.table) {
            case 0: select_only(is.row); scroll_to = is.row; focus_selection(); want_tab = T_PLACE; break;
            case 1: part_sel = is.row; want_tab = T_PARTS; break;
            case 2: circ_sel = is.row; want_tab = T_CIRC; cv.invalidate(); break;
            case 3: mark_sel = is.row; want_tab = T_MARKS; cv.fly_to(BBox(prog.marks[(size_t)is.row].x - 8, prog.marks[(size_t)is.row].y - 8, prog.marks[(size_t)is.row].x + 8, prog.marks[(size_t)is.row].y + 8)); break;
            default: want_tab = T_BOARD;
        }
    }
}

void PnpDoc::draw_compare() {
    if (cmp_open) { ImGui::OpenPopup("Compare with project##pnpcmp"); cmp_open = false; }
    ImGui::SetNextWindowSize(ImVec2(S(760), S(460)), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::BeginPopupModal("Compare with project##pnpcmp", &open)) return;
    ImGui::TextUnformatted(cmp_title.c_str());
    ImGui::Spacing();
    if (cmp.empty()) {
        empty_state(ui::Icon::Check, "Program matches the project", "Every placement has the same position, angle, part and skip state.");
    } else if (ImGui::BeginTable("##cmpt", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                                 ImVec2(0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - S(6)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Ref", ImGuiTableColumnFlags_WidthFixed, S(90));
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, S(130));
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)cmp.size());
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const auto& c = cmp[(size_t)r];
                ImGui::TableNextRow();
                ImGui::PushID(r);
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(c.ref.c_str(), false, ImGuiSelectableFlags_SpanAllColumns) && c.row >= 0) { select_only(c.row); scroll_to = c.row; want_tab = T_PLACE; focus_selection(); }
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(c.kind.c_str());
                ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%s", c.detail.c_str());
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    if (button("Select differing placements", ui::Icon::Select, false, ImVec2(0, 0), !cmp.empty())) {
        sync_sel();
        std::fill(sel.begin(), sel.end(), 0);
        for (auto& c : cmp) if (c.row >= 0) sel[(size_t)c.row] = 1;
        ++sel_gen;
        want_tab = T_PLACE;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (button("Close", ui::Icon::None, true)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void PnpDoc::draw(ImVec2 size) {
    const GTheme& t = th();
    sync_sel();
    // toolbar
    ImGui::BeginChild("##pnptb", ImVec2(size.x, S(40)), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    draw_toolbar();
    ImGui::EndChild();
    const float h = size.y - S(40);
    const float lw = std::clamp(size.x * split, S(220), size.x - S(320));
    // canvas
    if (cv_gen != gen || cv_sel != sel_gen) { cv_gen = gen; cv_sel = sel_gen; cv.invalidate(); }
    ImGui::BeginChild("##pnpcv", ImVec2(lw, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    cv.draw("##pnpcanvas", ImGui::GetContentRegionAvail());
    if (fit_pending && cv.size().x > 8) { cv.zoom_all(); fit_pending = false; }
    draw_canvas_menu();
    ImGui::EndChild();
    // splitter
    ImGui::SameLine(0, 0);
    ImGui::InvisibleButton("##split", ImVec2(S(6), h));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) { split = std::clamp((lw + ImGui::GetIO().MouseDelta.x) / std::max(1.f, size.x), 0.2f, 0.8f); cv.invalidate(); }
    {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2((a.x + b.x) / 2, a.y), ImVec2((a.x + b.x) / 2, b.y), ImGui::IsItemHovered() ? t.accent : t.border, S(1));
    }
    ImGui::SameLine(0, 0);
    ImGui::BeginChild("##pnpright", ImVec2(0, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(S(6), S(2)));
    if (ImGui::BeginTabBar("##pnptabs")) {
        static const char* names[T_COUNT] = {"Placements", "Parts", "Circuits", "Marks", "Board", "Validation"};
        for (int i = 0; i < T_COUNT; ++i) {
            std::string lbl = names[i];
            if (i == T_PLACE) lbl += " (" + std::to_string(prog.placements.size()) + ")";
            if (i == T_PARTS) lbl += " (" + std::to_string(prog.parts.size()) + ")";
            if (i == T_CIRC && prog.circuits.size() > 1) lbl += " (" + std::to_string(prog.circuits.size()) + ")";
            if (i == T_MARKS) lbl += " (" + std::to_string(prog.marks.size()) + ")";
            if (i == T_VALID) { const int e = count_issues(2), w = count_issues(1); if (e + w) lbl += " (" + std::to_string(e + w) + ")"; }
            lbl += "###t" + std::to_string(i);
            ImGuiTabItemFlags fl = want_tab == i ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(lbl.c_str(), nullptr, fl)) {
                if (tab != i) { tab = i; cv.invalidate(); ed_tab = -1; }
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(6));
                ImGui::BeginChild("##tabbody", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
                ImVec2 sz = ImGui::GetContentRegionAvail();
                switch (i) {
                    case T_PLACE: draw_placements(sz); break;
                    case T_PARTS: draw_parts(sz); break;
                    case T_CIRC: draw_circuits(sz); break;
                    case T_MARKS: draw_marks(sz); break;
                    case T_BOARD: draw_board(sz); break;
                    case T_VALID: draw_validation(sz); break;
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        want_tab = -1;
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    draw_compare();
    // shortcuts while the editor has focus
    if (!ImGui::GetIO().WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) { std::fill(sel.begin(), sel.end(), 1); ++sel_gen; }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) ImGui::SetKeyboardFocusHere();
    }
}

bool g_skip_route = false;

} // namespace

// ===========================================================================
// public entry points (gapp.h)
// ===========================================================================
bool pnp_open(const std::wstring& path) {
    for (PnpDoc* d : registry())
        if (!d->path.empty() && !lstrcmpiW(d->path.c_str(), path.c_str())) return files_open(path, 0);   // activates the existing tab
    std::vector<uint8_t> data;
    std::string err;
    io::PnpProgram p;
    const std::wstring fname = file_name_of(path);
    bool ok = run_with_progress_wait("Opening program", [&](Progress& pr) {
        pr.text(ui::U(fname).c_str());
        if (!read_file(ui::U(path), data, err)) return;
        if (!io::pnp_load(data, p, err)) return;
    }, ProgressOpts{true, false, 0.2f});
    if (!ok || !p.src) { toast("Could not open " + ui::U(fname) + (err.empty() ? "" : ": " + err), ToastKind::Error, 6.f); return false; }
    auto d = std::make_unique<PnpDoc>();
    d->prog = std::move(p);
    d->path = path;
    d->name = ui::U(fname);
    d->sync_sel();
    if (const wchar_t* t = _wgetenv(L"PCBP_PNP_TAB")) {   // automation / smoke tests: initial tab, select first rows
        d->want_tab = std::clamp(_wtoi(t), 0, (int)T_COUNT - 1);
        for (size_t i = 0; i < d->sel.size() && i < 4; ++i) d->sel[i] = 1;
        d->cursor = 0;
    }
    const size_t np = d->prog.placements.size();
    const std::string fmt = d->prog.format;
    const size_t notes = d->prog.notes.size();
    files_add(std::move(d));
    app::settings().add_mru(path);
    toast("Opened " + ui::U(fname) + ": " + fmt + ", " + std::to_string(np) + " placements" + (notes ? " (" + std::to_string(notes) + " load notes on the Board tab)" : ""),
          notes ? ToastKind::Warning : ToastKind::Success, 3.5f);
    return true;
}

void pnp_open_dialog() {
    std::wstring p = app::settings().dir_programs;
    if (!ui::open_file_dialog(app::frame_hwnd(), L"Open Machine Program in Editor",
                              {{L"Pick-and-place programs", L"*.h8h;*.h7h;*.h5h;*.h4h;*.e4?;*.e51;*.iss;*.x01;*.x52;*.s01x"},
                               {L"JUKI HLC (*.h8h;*.h7h;*.h5h;*.h4h)", L"*.h8h;*.h7h;*.h5h;*.h4h"}, {L"JUKI per-machine (*.e4?;*.e51)", L"*.e4?;*.e51"},
                               {L"JUKI ISS (*.iss)", L"*.iss;*.x01;*.x52;*.s01x"}, {L"All files", L"*.*"}}, p)) return;
    pnp_open(p);
}

void pnp_skip_route_once() { g_skip_route = true; }

bool pnp_route_open(const std::wstring& path) {
    if (g_skip_route) { g_skip_route = false; return false; }
    if (!io::pnp_is_program_ext(ext_of(path))) return false;
    if (app::g_quiet_import) {   // command line / automation: import unless asked for the editor
        if (!_wgetenv(L"PCBP_PNP_EDITOR")) return false;
        pnp_open(path);
        return true;
    }
    int ch = ui::prompt_choice(app::frame_hwnd(), L"Open " + file_name_of(path),
                               L"Open this machine program in the Program Editor (edit and save it in place), or rebuild a PCB Plotter project from it?",
                               {L"Open in Program Editor", L"Import as a new project"}, 0);
    if (ch < 0) return true;
    if (ch == 1) return false;
    pnp_open(path);
    return true;
}

} // namespace pcbp::gui
