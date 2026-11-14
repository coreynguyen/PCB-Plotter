// ============================================================================
// plotter_view.cpp — the Plotter view (CircuitCAM's Universal Window): central
// hub showing the assembled board colour-coded by model point, with the master
// bar tools and a collapsible side panel (Factory Model / Search / Review / Info).
// Logic ported from legacy/app/universal_view.cpp.
// ============================================================================
#include "gapp.h"
#include "gcanvas.h"
#include "../app/board_render.h"
#include "../app/commands.h"
#include "../core/placement_check.h"
#include "../core/package_edit.h"
#include "../ui/d2d.h"
#include "../ui/form.h"
#include "../ui/theme.h"
#include "../ui/win.h"
#include <algorithm>
#include <functional>
#include <cmath>
#include <cstring>
#include <cfloat>
#include <chrono>
#include <future>
#include <map>
#include <set>

namespace pcbp::gui {
using namespace pcbp::app;

namespace {
// v17: some (not all) placements assigned to an export layer -> show the unassigned highlight
bool assignment_partial(const Project& p) {
    if (p.model.points.empty()) return false;
    bool any_a = false, any_u = false;
    for (auto& c : p.comps) {
        if (c.feature != Feature::Component || c.dnp) continue;
        (c.assigned_point >= 0 ? any_a : any_u) = true;
        if (any_a && any_u) return true;
    }
    return false;
}

int g_orient_markers = 1;    // BoardRenderOpts::orient_markers (0 off, 1 oriented, 2 all)

enum class Tool { Info, Select, SelPart, SelPackage, SelTech, Pin1, Origin, Label, Measure, FrameCircuit, FramePanel, AlignMove, AlignRotate, Align2Ref };
const char* tool_name(Tool t) {
    static const char* n[] = {"Component info", "Select", "Select by part number", "Select by package", "Select by technology",
                              "Assign pin 1", "Assign origin", "Adjust labels", "Measure", "Circuit outline", "Board outline (panel) (pick corners)",
                              "Align: move by two points", "Align: rotate about point", "Align by two reference points"};
    return n[(int)t];
}
enum Tab { TAB_FACTORY, TAB_SEARCH, TAB_REVIEW, TAB_INFO, TAB_COMPS, TAB_COUNT };

// short badge text for a check code (shown on component rows and review groups)
const char* short_code(CheckCode c) {
    switch (c) {
    case CheckCode::DuplicateRef: return "DUP";
    case CheckCode::EmptyRef: return "R";
    case CheckCode::IllegalRefChars: return "R!";
    case CheckCode::Coincident: return "COIN";
    case CheckCode::NearCoincident: return "NEAR";
    case CheckCode::Overlap: return "OVL";
    case CheckCode::OutsideBoard: return "OUT";
    case CheckCode::PartlyOutsideBoard: return "EDGE";
    case CheckCode::NoBoardFrame: return "FRAME";
    case CheckCode::MissingPackage: return "P";
    case CheckCode::PackageNotInLibrary: return "P?";
    case CheckCode::ZeroPadPackage: return "P0";
    case CheckCode::MissingPartNumber: return "N";
    case CheckCode::PartNotInLibrary: return "N?";
    case CheckCode::PartPackageMismatch: return "N\xE2\x89\xA0P";
    case CheckCode::Unassigned: return "UA";
    case CheckCode::DnpAssigned: return "DNP";
    case CheckCode::FewFiducials: return "FID";
    case CheckCode::CollinearFiducials: return "FID/";
    case CheckCode::OffAxisRotation: return "ROT";
    case CheckCode::OffAxisPackage: return "PKG ANG";
    default: return check_code_badge(c);
    }
}
inline uint64_t code_bit(CheckCode c) { return 1ull << ((unsigned)c & 63); }

// panel layout remembered across documents for the session
float g_panel_w = 0;
bool g_panel_open = true;
int g_panel_tab = TAB_FACTORY;
int g_tab_req = TAB_FACTORY;   // tab to force-select on the next frame

std::vector<Component>& clip() { static std::vector<Component> c; return c; }

ImU32 argb2im(uint32_t c, float a = 1.f) {
    return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, (int)(((c >> 24) & 255) * a));
}
// Success toast with an Undo action (the edit was checkpointed just before).
void undo_toast(const std::string& msg) { toast(msg, ToastKind::Success, 4.5f, "Undo", [] { post_command(ID_EDIT_UNDO); }); }
ImU32 alpha(ImU32 c, float a) { return (c & 0x00FFFFFF) | ((ImU32)(((c >> 24) & 255) * a) << 24); }

bool is_pick_tool(Tool t) { return t == Tool::FramePanel || t == Tool::AlignMove || t == Tool::AlignRotate || t == Tool::Align2Ref; }
int picks_needed(Tool t) { return t == Tool::FramePanel ? 2 : t == Tool::AlignMove ? 2 : t == Tool::AlignRotate ? 3 : t == Tool::Align2Ref ? 4 : 0; }
const char* pick_prompt(Tool t, int n) {
    switch (t) {
    case Tool::FramePanel: return n == 0 ? "click the UPPER-LEFT panel corner" : "click the LOWER-RIGHT panel corner";
    case Tool::AlignMove: return n == 0 ? "click a point on the PLOT" : "click the TARGET point (e.g. on the gerber)";
    case Tool::AlignRotate: return n == 0 ? "click the PIVOT" : n == 1 ? "click a REFERENCE point on the plot" : "click where the reference should go";
    case Tool::Align2Ref: return n == 0 ? "click plot point A" : n == 1 ? "click target A (gerber)" : n == 2 ? "click plot point B" : "click target B (gerber)";
    default: return "";
    }
}
// same convention as the gerber workspace's layer_xform
Xform gerber_layer_xform(const GerberLayerRef& r) {
    Xform t = Xform::translate(r.offset.x, r.offset.y) * Xform::rotation(r.rot);
    if (r.mirror) t = t * Xform::scale(-1, 1);
    return t;
}
// move / rotate the whole plot (every location + circuit frame); panel frame and gerbers stay put
void apply_plot_xform(Doc& d, const Xform& T, double drot, const char* label) {
    d.checkpoint(label);
    Xform L = T; L.tx = L.ty = 0;
    for (auto& c : d.prj.comps) {
        c.pos = T.apply(c.pos);
        c.label_off = L.apply(c.label_off);
        if (drot != 0) { c.rot = norm_deg(c.rot + drot); c.label_rot = norm_deg(c.label_rot + drot); }
    }
    for (auto& pl : d.prj.board_frame) for (auto& q : pl) q = T.apply(q);
    d.changed(ui::CH_GEOMETRY);
}
// rotation-arrow colour by angle (dark, light)
ImU32 angle_col(double r, bool dark) {
    r = norm_deg(r);
    auto is_near = [&](double a) { double x = std::fabs(r - a); return x < 0.01 || x > 359.99; };
    if (is_near(0)) return dark ? IM_COL32(110, 230, 140, 255) : IM_COL32(20, 140, 60, 255);
    if (is_near(90)) return dark ? IM_COL32(110, 190, 255, 255) : IM_COL32(20, 100, 200, 255);
    if (is_near(180)) return dark ? IM_COL32(255, 185, 90, 255) : IM_COL32(200, 110, 0, 255);
    if (is_near(270)) return dark ? IM_COL32(210, 150, 255, 255) : IM_COL32(130, 60, 200, 255);
    return dark ? IM_COL32(255, 90, 90, 255) : IM_COL32(210, 30, 30, 255);
}

std::string lower(std::string s) { for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; }

bool wild(const char* p, const char* s) {   // case-insensitive * ? match (inputs already lowered)
    const char *star = nullptr, *ss = nullptr;
    while (*s) {
        if (*p == '?' || *p == *s) { ++p; ++s; continue; }
        if (*p == '*') { star = p++; ss = s; continue; }
        if (star) { p = star + 1; s = ++ss; continue; }
        return false;
    }
    while (*p == '*') ++p;
    return !*p;
}

// split "R12" -> ("r", 12). returns false when no trailing number
bool split_ref(const std::string& r, std::string& pre, long& num) {
    size_t i = r.size();
    while (i > 0 && std::isdigit((unsigned char)r[i - 1])) --i;
    if (i == r.size() || r.size() - i > 9) return false;
    pre = r.substr(0, i);
    num = std::stol(r.substr(i));
    return true;
}

struct Matcher {
    struct Tok { std::string pat; bool range = false; std::string pre; long a = 0, b = 0; bool wild = false; };
    std::vector<Tok> toks;
    explicit Matcher(const std::string& q0) {
        std::string q = lower(q0), cur;
        auto flush = [&]() {
            if (cur.empty()) return;
            Tok t;
            t.pat = cur;
            size_t dash = cur.find('-');
            if (dash != std::string::npos && dash > 0 && dash + 1 < cur.size()) {
                std::string l = cur.substr(0, dash), r = cur.substr(dash + 1), p1, p2;
                long n1, n2;
                if (split_ref(l, p1, n1)) {
                    if (split_ref(r, p2, n2) && (p2.empty() || p2 == p1)) { t.range = true; t.pre = p1; t.a = std::min(n1, n2); t.b = std::max(n1, n2); }
                }
            }
            t.wild = cur.find_first_of("*?") != std::string::npos;
            toks.push_back(t);
            cur.clear();
        };
        for (char c : q) { if (c == ',' || c == ';' || c == ' ' || c == '\t') flush(); else cur += c; }
        flush();
    }
    bool empty() const { return toks.empty(); }
    // 0 = no match, 2 = exact ref match, 1 = other match
    int match(const Project& p, const Component& c) const {
        std::string ref = lower(c.ref), num = lower(c.number), pkg = lower(c.package), val;
        if (const PartNumber* pn = p.part_of(c)) val = lower(pn->description);
        int best = 0;
        for (auto& t : toks) {
            if (t.range) {
                std::string pre; long n;
                if (split_ref(ref, pre, n) && pre == t.pre && n >= t.a && n <= t.b) best = std::max(best, 2);
                continue;
            }
            if (t.wild) {
                if (wild(t.pat.c_str(), ref.c_str())) best = std::max(best, 2);
                else if (wild(t.pat.c_str(), num.c_str()) || wild(t.pat.c_str(), pkg.c_str()) || wild(t.pat.c_str(), val.c_str())) best = std::max(best, 1);
                continue;
            }
            if (ref == t.pat) best = 2;
            else if (ref.find(t.pat) != std::string::npos || num.find(t.pat) != std::string::npos || pkg.find(t.pat) != std::string::npos ||
                     val.find(t.pat) != std::string::npos) best = std::max(best, 1);
        }
        return best;
    }
};

class PlotterView;

// ---------------------------------------------------------------------------
// canvas
// ---------------------------------------------------------------------------
class BoardCanvas : public GCanvas {
public:
    PlotterView* owner = nullptr;
    DocPtr doc;
    Tool tool = Tool::Select;
    int hover = -1;
    bool moving = false, label_drag = false;
    Vec2 drag_delta{};
    int label_comp = -1;
    // R13 (§7): layer-assignment feedback: brief pulse around the assigned parts + a layer chip
    std::vector<uint32_t> flash_ids;
    double flash_t0 = -1;
    std::string flash_label;
    ImU32 flash_col = 0;
    int meas_n = 0;
    Vec2 meas[2]{};
    bool ctx_pending = false;

    BBox sel_bbox() const {
        BBox b;
        for (auto& c : doc->prj.comps) if (c.selected) b.add(doc->prj.comp_bbox(c));
        return b;
    }
    static BBox frame_box(BBox b) {
        if (b.empty()) return b;
        b.inflate(std::max(2.5, std::max(b.w(), b.h()) * 0.08));
        return b;
    }
    bool zoom_selection() {
        BBox b = sel_bbox();
        if (b.empty()) return false;
        fly_to(frame_box(b));
        return true;
    }
    int hover_img = 0;
    bool frame_drag = false;
    Vec2 frame_d{};
    // circuit frame handles: 0 move, else bits L=1 R=2 B=4 T=8
    int fr_handle = -1;
    BBox fr_orig, fr_preview;
    // point-pick tools (panel corners / align)
    int pick_n = 0;
    std::string hint_text;
    int hover_seen = -1;
    double hover_t0 = 0;
    double hint_t0 = 0;
    Vec2 picks[4]{};
    std::vector<Vec2> snaps;
    uint64_t snaps_gen = 0;
    bool snap_on = false;
    Vec2 snap_cur{};

    void build_snaps() {
        if (snaps_gen == doc->data_gen && !snaps.empty()) return;
        snaps.clear();
        const Project& p = doc->prj;
        for (auto& c : p.comps) {
            snaps.push_back(c.pos);
            const Package* pk = p.package_of(c);
            if (pk && pk->pads.size() < 400) for (int i = 0; i < (int)pk->pads.size(); ++i) snaps.push_back(pad_world(*doc, c, i));
        }
        for (auto* v : {&p.board_frame, &p.panel_frame}) for (auto& pl : *v) for (auto& q : pl) snaps.push_back(q);
        for (size_t i = 0; i < p.layers.size() && i < doc->glayers.size(); ++i) {
            if (!p.layers[i].visible || !doc->glayers[i]) continue;
            const Xform x = gerber_layer_xform(p.layers[i]);
            const gerber::Layer& L = *doc->glayers[i];
            for (auto& o : L.objs) {
                if (o.clear) continue;
                if (o.kind == gerber::Object::Flash) snaps.push_back(x.apply(o.a));
                else if (o.kind == gerber::Object::Region) { for (auto& q : o.poly) snaps.push_back(x.apply(q)); }
                else { snaps.push_back(x.apply(o.a)); snaps.push_back(x.apply(o.b)); }
                if (snaps.size() > 600000) break;
            }
            if (!L.extent.empty()) for (Vec2 q : {Vec2{L.extent.x0, L.extent.y0}, Vec2{L.extent.x1, L.extent.y0}, Vec2{L.extent.x1, L.extent.y1}, Vec2{L.extent.x0, L.extent.y1}}) snaps.push_back(x.apply(q));
        }
        snaps_gen = doc->data_gen;
    }
    // nearest geometry point within 12 px, else grid snap
    Vec2 pick_pt(Vec2 w, bool* snapped = nullptr) {
        build_snaps();
        const double tol = 12.0 / scale_;
        double best = tol * tol;
        Vec2 r = w;
        bool hit = false;
        for (auto& q : snaps) {
            double dx = q.x - w.x, dy = q.y - w.y, d2 = dx * dx + dy * dy;
            if (d2 < best) { best = d2; r = q; hit = true; }
        }
        if (snapped) *snapped = hit;
        return hit ? r : snap_pt(w);
    }
    int frame_handle(const BBox& fb, Vec2 w) const {
        const double t = 7.0 / scale_;
        if (w.x < fb.x0 - t || w.x > fb.x1 + t || w.y < fb.y0 - t || w.y > fb.y1 + t) return -1;
        int h = 0;
        if (std::fabs(w.x - fb.x0) < t) h |= 1; else if (std::fabs(w.x - fb.x1) < t) h |= 2;
        if (std::fabs(w.y - fb.y0) < t) h |= 4; else if (std::fabs(w.y - fb.y1) < t) h |= 8;
        if (h) return h;
        return fb.contains(w) ? 0 : -1;
    }
    bool wants_rubber_band() const override { return !is_pick_tool(tool); }
    // instance-aware hit test: every panel image (global fiducials once). img = instance hit
    int hit(Vec2 w, int* img = nullptr) const {
        const Project& p = doc->prj;
        const double tol = 3.0 / scale_;
        int best = -1, bimg = 0;
        double best_area = 1e30;
        const int nimg = doc->all_images ? (int)p.images.size() : 1;
        for (size_t i = 0; i < p.comps.size(); ++i) {
            const Component& c = p.comps[i];
            if (c.side == Side::Top && !doc->show_top) continue;
            if (c.side == Side::Bottom && !doc->show_bottom) continue;
            const Package* pk = p.package_of(c);
            BBox e = pk ? pk->extent() : BBox(-0.5, -0.5, 0.5, 0.5);
            if (c.feature == Feature::Fiducial) e = BBox(-1, -1, 1, 1);
            e.inflate(tol);
            const int n = std::min(nimg, p.instance_count(c));
            for (int im = 0; im < n; ++im) {
                Vec2 local = p.instance_xform(c, im).inverse().apply(w);
                if (e.contains(local)) {
                    double a = e.w() * e.h();
                    if (a < best_area) { best_area = a; best = (int)i; bimg = im; }
                }
            }
        }
        if (img) *img = bimg;
        return best;
    }
    // map a world point on instance `img` of comp h back to root-image coordinates (for pad tests)
    Vec2 to_root(int h, int img, Vec2 w) const {
        const Project& p = doc->prj;
        const Component& c = p.comps[h];
        return p.comp_xform(c, 0).apply(p.instance_xform(c, img).inverse().apply(w));
    }
    // world-space bbox of comp i on instance img
    BBox inst_bbox(int i, int img) const {
        const Project& p = doc->prj;
        const Component& c = p.comps[i];
        if (img == 0) return p.comp_bbox(c);
        BBox rb = p.comp_bbox(c), out;
        Xform a = p.comp_xform(c, 0).inverse(), b = p.instance_xform(c, img);
        for (Vec2 q : {Vec2{rb.x0, rb.y0}, Vec2{rb.x1, rb.y0}, Vec2{rb.x0, rb.y1}, Vec2{rb.x1, rb.y1}}) out.add(b.apply(a.apply(q)));
        return out;
    }
    void clear_measure() { meas_n = 0; }

    // zoom extents: placements + circuit/panel frames (fiducials, marks and the origin marker excluded)
    BBox world_extent() const override {
        if (!doc) return BBox(0, 0, 100, 80);
        const Project& p = doc->prj;
        {
            BBox e;
            const int nimg = doc->all_images ? std::max(1, (int)p.images.size()) : 1;
            for (auto& c : p.comps) {
                if (c.feature != Feature::Component) continue;
                if ((c.side == Side::Top && !doc->show_top) || (c.side == Side::Bottom && !doc->show_bottom)) continue;
                const int n = std::min(nimg, p.instance_count(c));
                for (int im = 0; im < n; ++im) e.add(p.instance_xform(c, im).apply({0, 0}));
            }
            for (int im = 0; im < nimg; ++im) { Xform x = p.image_xform(im); for (auto& pl : p.board_frame) for (auto& q : pl) e.add(x.apply(q)); }
            for (auto& pl : p.panel_frame) for (auto& q : pl) e.add(q);
            if (!e.empty()) { e.inflate(std::max(1.0, std::max(e.w(), e.h()) * 0.02)); return e; }
        }
        BBox b = p.board_bbox();
        if (doc->all_images) for (size_t i = 1; i < p.images.size(); ++i) {
            Xform x = p.image_xform((int)i);
            BBox ib = p.board_bbox();
            b.add(x.apply({ib.x0, ib.y0})); b.add(x.apply({ib.x1, ib.y1}));
            b.add(x.apply({ib.x0, ib.y1})); b.add(x.apply({ib.x1, ib.y0}));
        }
        for (auto& pl : p.panel_frame) for (auto& q : pl) b.add(q);
        return b;
    }

    BoardRenderOpts opts() const {
        BoardRenderOpts o;
        o.world = world_matrix();
        o.px_per_mm = scale_;
        o.mirror = mirror_;
        o.show_labels = doc->show_labels;
        o.show_frame = doc->show_frame;
        o.show_top = doc->show_top;
        o.show_bottom = doc->show_bottom;
        o.all_images = doc->all_images;
        o.highlight_unassigned = doc->highlight_unassigned && assignment_partial(doc->prj);   // v17
        o.highlight_noref = doc->highlight_noref;
        o.verify_angles = false;        // drawn by the live overlay (bold arrows + legend)
        o.second_side = doc->second_side;
        o.ghost_far = doc->far_drawn();
        o.far_alpha = doc->back_selectable && doc->side_display == Doc::SideDisplay::Xray ? 0.7f : 0.45f;   // R17c: Ghost == X-ray locked
        o.show_origin = false;          // drawn by the live overlay
        o.orient_markers = g_orient_markers;
        o.dip_per_px = 1.f;
        return o;
    }

    void paint_world(ID2D1DeviceContext* rt) override {
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        BoardRenderOpts o = opts();
        Doc& d = *doc;
        if (moving && (drag_delta.x != 0 || drag_delta.y != 0)) {
            for (auto& c : d.prj.comps) if (c.selected) c.pos += drag_delta;
            render_board(rt, d, o);
            for (auto& c : d.prj.comps) if (c.selected) c.pos -= drag_delta;
        } else render_board(rt, d, o);
    }

    void paint_overlay(ImDrawList* dl) override;

    void on_mouse_move(Vec2 w) override {
        int im = 0;
        int h = hit(w, &im);
        hover = h; hover_img = im;
    }

    bool hit_drag_target(Vec2 w, int mods) override {
        if (tool == Tool::Label) {
            label_comp = -1;
            for (size_t i = 0; i < doc->prj.comps.size(); ++i) {
                auto& c = doc->prj.comps[i];
                if (c.ref.empty()) continue;
                if (dist(c.pos + c.label_off, w) < 8.0 / scale_ + 0.3) { label_comp = (int)i; break; }
            }
            if (label_comp < 0) label_comp = hit(w);
            if (label_comp >= 0) { label_drag = true; return true; }
            return false;
        }
        if (tool == Tool::FrameCircuit) return false;          // R16: outlines are never dragged (Edit outline dialog)
        if (is_pick_tool(tool)) return false;                  // panel frame is never dragged loosely
        // placements are LOCKED unless the Move tool is on
        if (!doc->move_mode || tool != Tool::Select) return false;
        int h = hit(w);
        if (h < 0) return false;
        Doc& d = *doc;
        if (!d.prj.comps[h].selected) {
            if (!(mods & (KM_CTRL | KM_SHIFT))) d.select_none();
            d.prj.comps[h].selected = true;
            d.changed(ui::CH_SELECTION);
        }
        moving = true;
        drag_delta = {};
        return true;
    }

    void on_drag(Vec2 from, Vec2 to, int, bool done) override {
        Doc& d = *doc;
        if (label_drag && label_comp >= 0 && label_comp < (int)d.prj.comps.size()) {
            Component& c = d.prj.comps[label_comp];
            if (done) {
                Vec2 off = to - c.pos;
                d.checkpoint("Move label");
                c.label_off = off;
                c.label_moved = true;
                label_drag = false;
                d.changed(ui::CH_GEOMETRY);
            } else c.label_off = to - c.pos;
            invalidate();
            return;
        }
        if (frame_drag) {
            if (fr_handle == 0) frame_d = snap_pt(to) - snap_pt(from);
            else {
                Vec2 t = pick_pt(to);
                double x0 = fr_orig.x0, y0 = fr_orig.y0, x1 = fr_orig.x1, y1 = fr_orig.y1;
                if (fr_handle & 1) x0 = t.x;
                if (fr_handle & 2) x1 = t.x;
                if (fr_handle & 4) y0 = t.y;
                if (fr_handle & 8) y1 = t.y;
                fr_preview = BBox(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1));
            }
            if (done) {
                frame_drag = false;
                if (fr_handle == 0 && (frame_d.x != 0 || frame_d.y != 0)) {
                    d.checkpoint("Move circuit outline"); d.prj.offset_circuit_frame(frame_d); d.changed(ui::CH_GEOMETRY);
                } else if (fr_handle > 0 && fr_preview.w() > 0.1 && fr_preview.h() > 0.1) {
                    d.checkpoint("Resize circuit outline"); d.prj.set_circuit_frame_rect(fr_preview); d.changed(ui::CH_GEOMETRY);
                    gui::status("Circuit outline " + fmt_len_u8(fr_preview.w()) + " x " + fmt_len_u8(fr_preview.h()));
                }
                frame_d = {}; fr_handle = -1;
            }
            invalidate();
            return;
        }
        if (!moving) return;
        Vec2 dd = snap_pt(to) - snap_pt(from);
        drag_delta = dd;
        if (done) {
            moving = false;
            drag_delta = {};
            if (dd.x != 0 || dd.y != 0) {
                d.checkpoint("Move components");
                int n = 0;
                for (auto& c : d.prj.comps) if (c.selected) { c.pos += dd; n++; }
                d.changed(ui::CH_GEOMETRY);
                gui::status("Moved " + std::to_string(n) + " components by " + fmt_len_u8(dd.x) + ", " + fmt_len_u8(dd.y));
            }
        }
        invalidate();
    }

    void on_click(Vec2 w, int mods) override;
    void on_dblclick(Vec2 w, int) override {
        if (tool == Tool::FrameCircuit) { post_command(ID_BOARD_CIRCUIT_FRAME_DLG); return; }
        if (tool != Tool::Select && tool != Tool::Info) return;
        int h = hit(w);
        if (h < 0) return;
        Doc& d = *doc;
        d.select_none();
        d.prj.comps[h].selected = true;
        d.changed(ui::CH_SELECTION);
        post_command(ID_CTX_EDIT_LOCATION);
    }
    void on_rect(const BBox& b, int mods) override {
        Doc& d = *doc;
        if (is_pick_tool(tool)) return;
        if (tool == Tool::FrameCircuit) {
            return;   // R16: no rubber-band outlines (accidental manufacturing-dimension changes)
            if (b.w() < 0.5 || b.h() < 0.5) return;
            BBox s(snap_pt({b.x0, b.y0}).x, snap_pt({b.x0, b.y0}).y, snap_pt({b.x1, b.y1}).x, snap_pt({b.x1, b.y1}).y);
            d.checkpoint(tool == Tool::FrameCircuit ? "Set circuit outline" : "Set board outline (panel)");
            if (tool == Tool::FrameCircuit) d.prj.set_circuit_frame_rect(s); else d.prj.set_panel_frame_rect(s);
            d.changed(ui::CH_GEOMETRY);
            gui::status(std::string(tool == Tool::FrameCircuit ? "Circuit" : "Panel") + " frame set: " + fmt_len_u8(s.w()) + " x " + fmt_len_u8(s.h()));
            return;
        }
        if (tool == Tool::Info || tool == Tool::Pin1 || tool == Tool::Origin || tool == Tool::Measure || tool == Tool::Label) return;
        if (!(mods & (KM_SHIFT | KM_CTRL))) d.select_none();
        int n = 0;
        for (auto& c : d.prj.comps) {
            if (c.side == Side::Top && !d.show_top) continue;
            if (c.side == Side::Bottom && !d.show_bottom) continue;
            bool in = false;
            const int ni = d.all_images ? d.prj.instance_count(c) : 1;
            for (int im = 0; im < ni && !in; ++im) in = b.contains(d.prj.instance_xform(c, im).apply({0, 0}));
            if (in) { c.selected = (mods & KM_CTRL) ? !c.selected : true; n++; }
        }
        d.changed(ui::CH_SELECTION);
        gui::status(std::to_string(d.selected_count()) + " selected");
    }
    void on_context(Vec2 w) override {
        Doc& d = *doc;
        int h = hit(w);
        if (h >= 0 && !d.prj.comps[h].selected) { d.select_none(); d.prj.comps[h].selected = true; d.changed(ui::CH_SELECTION); }
        ctx_pending = true;
    }
    bool on_key(ImGuiKey k, int mods) override {
        Doc& d = *doc;
        switch (k) {
        case ImGuiKey_Escape:
            if (meas_n) { meas_n = 0; return true; }
            if (pick_n) { pick_n = 0; return true; }
            if (tool != Tool::Select) { tool = Tool::Select; return true; }
            d.select_none(); d.changed(ui::CH_SELECTION); return true;
        case ImGuiKey_R: if (!(mods & KM_CTRL)) { post_command((mods & KM_SHIFT) ? ID_TOOL_ROT270 : ID_TOOL_ROT90); return true; } return false;
        case ImGuiKey_Delete: return true;   // shell posts ID_EDIT_DELETE
        case ImGuiKey_M: tool = Tool::Measure; meas_n = 0; return true;
        case ImGuiKey_T: post_command(ID_VIEW_SIDE_TOP); return true;
        case ImGuiKey_B: post_command(ID_VIEW_SIDE_BOTTOM); return true;
        case ImGuiKey_F: if (!zoom_selection()) zoom_all(); return true;
        case ImGuiKey_Z: if (mods & KM_CTRL) return false; if (!zoom_selection()) zoom_all(); return true;   // 3ds Max style zoom extents
        default: return false;
        }
    }
    ImGuiMouseCursor cursor() const override {
        switch (tool) {
        case Tool::Select:
            if (doc->move_mode && hover >= 0) return ImGuiMouseCursor_ResizeAll;
            return hover >= 0 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_Arrow;
        case Tool::SelPart: case Tool::SelPackage: case Tool::SelTech: case Tool::Info: return hover >= 0 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_Arrow;
        case Tool::Label: return ImGuiMouseCursor_ResizeAll;
        case Tool::FrameCircuit: {
            BBox fb = doc->prj.circuit_frame_bbox();
            int h = fb.empty() ? -1 : frame_handle(fb, mouse_world());
            if (h == 0) return ImGuiMouseCursor_ResizeAll;
            if (h == 1 || h == 2) return ImGuiMouseCursor_ResizeEW;
            if (h == 4 || h == 8) return ImGuiMouseCursor_ResizeNS;
            if (h == 5 || h == 10) return mirror_ ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW;
            if (h == 6 || h == 9) return mirror_ ? ImGuiMouseCursor_ResizeNESW : ImGuiMouseCursor_ResizeNWSE;
            return ImGuiMouseCursor_Arrow;
        }
        default: return ImGuiMouseCursor_Arrow;
        }
    }
    friend class PlotterView;
};

// ---------------------------------------------------------------------------
// view
// ---------------------------------------------------------------------------
class PlotterView : public View {
public:
    BoardCanvas canvas;
    uint64_t seen_gen = 0;
    bool pending_fly = false;
    BBox fly_box;
    // search
    char query[256] = "";
    std::string last_query;
    uint64_t search_gen = 0;
    std::vector<int> results;
    bool focus_search = false;
    int search_cursor = -1;
    // review
    std::vector<PlacementIssue> issues;
    uint64_t review_gen = 0;
    bool review_ran = false;
    int review_err = 0, review_warn = 0, review_info = 0;
    int issue_cursor = -1;
    uint64_t review_key = 0;
    uint64_t review_filter = 0;                 // code mask; 0 = all
    std::vector<uint64_t> comp_codes;           // per component: mask of issue codes
    std::vector<uint8_t> comp_sev;              // per component: worst severity + 1 (0 = clean)
    // components tab
    char comp_q[128] = "";
    int comp_group = 0;                         // 0 part number, 1 package, 2 flat
    bool comp_flagged = false;
    uint64_t comp_filter = 0;                   // code mask; 0 = any
    std::vector<std::pair<std::string, std::vector<int>>> comp_groups;
    uint64_t comp_key = 0;
    std::string comp_last_q;
    int comp_cursor = -1;
    // info quick edit buffers
    int info_comp = -1;
    uint64_t info_gen = 0;
    char e_ref[64] = "", e_pn[128] = "", e_pkg[128] = "", e_x[32] = "", e_y[32] = "", e_rot[32] = "";

    ViewKind kind() const override { return ViewKind::Plotter; }
    const char* title() const override { return "Plot"; }
    ui::Icon icon() const override { return ui::Icon::UniversalView; }
    bool can_print() const override { return true; }

    void fly_to_comps(const std::vector<int>& idx) {
        BBox b;
        for (int i : idx) if (i >= 0 && i < (int)doc->prj.comps.size()) b.add(doc->prj.comp_bbox(doc->prj.comps[i]));
        if (b.empty()) return;
        fly_box = BoardCanvas::frame_box(b);
        pending_fly = true;
        request_redraw(3);
    }
    void select_only(const std::vector<int>& idx, bool add = false) {
        Doc& d = *doc;
        if (!add) d.select_none();
        for (int i : idx) if (i >= 0 && i < (int)d.prj.comps.size()) d.prj.comps[i].selected = true;
        d.changed(ui::CH_SELECTION);
    }
    void open_tab(int t) { g_panel_open = true; g_panel_tab = t; g_tab_req = t; request_redraw(3); }

    // --- search -----------------------------------------------------------
    void run_search() {
        results.clear();
        last_query = query;
        search_gen = doc->gen;
        Matcher m(query);
        if (m.empty()) return;
        std::vector<std::pair<int, int>> r;
        for (int i = 0; i < (int)doc->prj.comps.size(); ++i) {
            int s = m.match(doc->prj, doc->prj.comps[i]);
            if (s) r.push_back({s, i});
        }
        const auto& comps = doc->prj.comps;
        std::stable_sort(r.begin(), r.end(), [&](auto& a, auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return ref_less(comps[a.second].ref, comps[b.second].ref);
        });
        for (auto& x : r) results.push_back(x.second);
        search_cursor = results.empty() ? -1 : 0;
    }
    void select_results() {
        if (results.empty()) return;
        select_only(results);
        fly_to_comps(results);
        gui::status(std::to_string(results.size()) + " components selected");
    }

    // --- review -----------------------------------------------------------
    PlacementCheckOptions review_options() const {
        PlacementCheckOptions o;
        o.check_unassigned = !doc->prj.model.points.empty();
        return o;
    }
    uint64_t review_key_now() const { return doc->data_gen * 2 + (doc->all_images ? 1 : 0); }
    // Placement checks run on a worker thread against a snapshot of the project; results are applied
    // on the UI thread when ready (the Review tab shows a spinner meanwhile). Stale results (document
    // changed while checking) trigger another run via the key comparison.
    std::future<std::vector<PlacementIssue>> review_job;
    uint64_t review_job_key = 0;
    bool review_busy() const { return review_job.valid(); }
    void ensure_review() {
        poll_review();
        if (!review_busy() && (!review_ran || review_key != review_key_now())) run_review();
    }
    void run_review() {
        if (review_busy()) return;
        auto snap = std::make_shared<Project>(doc->prj);
        PlacementCheckOptions o = review_options();
        review_job_key = review_key_now();
        review_job = std::async(std::launch::async, [snap, o] { return check_placements(*snap, o); });
        request_redraw(3);
    }
    void poll_review() {
        if (!review_job.valid()) return;
        if (review_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { request_redraw(2); return; }
        try { issues = review_job.get(); } catch (...) { issues.clear(); }
        review_job = {};
        review_key = review_job_key;
        review_gen = doc->gen;
        comp_codes.assign(doc->prj.comps.size(), 0);
        comp_sev.assign(doc->prj.comps.size(), 0);
        for (auto& is : issues) for (int ci : is.comps) if (ci >= 0 && ci < (int)comp_codes.size()) {
            comp_codes[ci] |= code_bit(is.code);
            comp_sev[ci] = std::max<uint8_t>(comp_sev[ci], (uint8_t)((int)is.severity + 1));
        }
        review_ran = true;
        review_err = count_issues(issues, Severity::Error);
        review_warn = count_issues(issues, Severity::Warning);
        review_info = count_issues(issues, Severity::Info);
        std::stable_sort(issues.begin(), issues.end(), [](const PlacementIssue& a, const PlacementIssue& b) {
            if (a.severity != b.severity) return a.severity > b.severity;
            return a.code < b.code;
        });
        comp_key = 0;   // regroup parts tab
    }
    ~PlotterView() override { if (review_job.valid()) review_job.wait(); }

    // --- edits ------------------------------------------------------------
    void rotate_selection(double deg) {
        auto sel = doc->selection();
        if (sel.empty()) { gui::status("Select components to rotate"); return; }
        doc->checkpoint("Rotate");
        for (auto* c : sel) { c->rot = norm_deg(c->rot + deg); c->label_off = rotate(c->label_off, deg); }
        doc->changed(ui::CH_GEOMETRY);
        undo_toast("Rotated " + std::to_string(sel.size()) + (sel.size() == 1 ? " component" : " components") + " by " + std::to_string((int)deg) + "\xC2\xB0");
    }

    // ----------------------------------------------------------------------
    void draw_toolbar() override;
    void draw(ImVec2 size) override;
    PlotterView() { if (const char* e = getenv("PCBP_PANEL_TAB")) { g_panel_tab = g_tab_req = atoi(e); } }   // automation / screenshots
    void draw_panel(float w, float h);
    void draw_activity_bar(float w, float h);
    void draw_search();
    void draw_review();
    void draw_info();
    void draw_comps();
    void draw_badge(ImDrawList* dl, ImVec2& p, const char* txt, ImU32 col, bool right_to_left);
    void draw_side_toggle();
    float assign_group_width();      // R17c (REVIEW_V16 §8)
    void draw_assign_group();
    void draw_viewport_tools(ImVec2 c0, ImVec2 c1);   // R17c (REVIEW_V16 §9): floating zoom / pan strip
    void draw_selset_combo();
    void draw_context_menu();
    bool on_command(int id) override;
    int cmd_state(int id) override;
    void print(HDC dc, const RECT& page, bool one_to_one) override;
    void on_activate() override { canvas.invalidate(); }

    std::string status() const override {
        int sel = doc->selected_count();
        std::string s = std::to_string(doc->prj.comps.size()) + " components \xC2\xB7 " + std::to_string(sel) + " selected \xC2\xB7 " + tool_name(canvas.tool);
        if (doc->move_mode) s += " (move)";
        return s;
    }
    std::string status_right() const override {
        if (!canvas.hovered()) return {};
        return fmt_pos_u8(*doc, canvas.mouse_world());
    }
};

// ---------------------------------------------------------------------------
void BoardCanvas::on_click(Vec2 w, int mods) {
    Doc& d = *doc;
    const double tol = 3.0 / scale_;
    int himg = 0;
    int h = hit(w, &himg);
    switch (tool) {
    case Tool::Info:
        if (h < 0) return;
        d.select_none();
        d.prj.comps[h].selected = true;
        d.changed(ui::CH_SELECTION);
        owner->open_tab(TAB_INFO);
        return;
    case Tool::Select:
        if (!(mods & (KM_SHIFT | KM_CTRL))) d.select_none();
        if (h >= 0) d.prj.comps[h].selected = (mods & KM_CTRL) ? !d.prj.comps[h].selected : true;
        d.changed(ui::CH_SELECTION);
        return;
    case Tool::SelPart: case Tool::SelPackage: case Tool::SelTech: {
        if (h < 0) return;
        const Component c = d.prj.comps[h];
        const Package* pk = d.prj.package_of(c);
        const bool smt = pk ? pk->smt : true;
        if (!(mods & (KM_SHIFT | KM_CTRL))) d.select_none();
        int n = 0;
        for (auto& o : d.prj.comps) {
            bool m = false;
            if (tool == Tool::SelPart) m = !c.number.empty() && o.number == c.number;
            else if (tool == Tool::SelPackage) m = !c.package.empty() && o.package == c.package;
            else { const Package* p2 = d.prj.package_of(o); m = (p2 ? p2->smt : true) == smt && o.feature == Feature::Component; }
            if (m) { o.selected = true; n++; }
        }
        d.changed(ui::CH_SELECTION);
        gui::status(std::to_string(n) + " components selected (" +
               (tool == Tool::SelPart ? c.number : tool == Tool::SelPackage ? c.package : std::string(smt ? "SMT" : "thru-hole")) + ")");
        return;
    }
    case Tool::Pin1: {
        if (h < 0) { gui::status("Click a pad of a component to make it pin one"); return; }
        Component& c = d.prj.comps[h];
        int pad = hit_pad(d, c, to_root(h, himg, w), tol);
        if (pad < 0) { gui::status("Click on a pad to make it pin one"); return; }
        // Assign Pin One = rotate THIS placement so pin 1 lands on the clicked lead.
        // Never renumbers the shared package (CircuitCAM SetNewPinOne does; specs/PACKAGES_RE.md).
        const Package* pk = d.prj.package_of(c);
        if (!pk) { gui::status(c.ref + " has no package"); return; }
        Component trial = c;
        std::string msg;
        const double delta = placement_assign_pin_one(trial, *pk, pad, &msg);
        if (std::isnan(delta) || delta == 0) { gui::status(msg); return; }
        d.checkpoint("Assign pin 1 (rotate " + c.ref + ")");
        c = trial;
        d.changed(ui::CH_GEOMETRY);
        gui::status(msg);
        return;
    }
    case Tool::Origin: {
        Vec2 o;
        std::string what;
        if (h >= 0) {
            const Component& c = d.prj.comps[h];
            int pad = hit_pad(d, c, to_root(h, himg, w), tol);
            // component -> its centroid (root image), so its X/Y read exactly 0,0.
            // Shift+click a pad to put the origin on that pad instead.
            const bool on_pad = pad >= 0 && himg == 0 && ImGui::GetIO().KeyShift;
            o = on_pad ? pad_world(d, c, pad) : c.pos;
            what = on_pad ? c.ref + " pad " + std::to_string(pad + 1) : c.ref;
        } else { o = snap_pt(w); what = "point"; }
        d.checkpoint("Assign origin");
        if (h >= 0 && what == d.prj.comps[h].ref) d.prj.set_origin_at(d.prj.comps[h]);   // exact centroid
        else d.prj.virtual_origin = o;
        d.changed(ui::CH_GEOMETRY);
        gui::status("Virtual origin set at " + what + " \xE2\x80\x94 coordinates now read relative to it. Board \xE2\x96\xB8 Rebase Coordinates makes it 0,0 permanently.");
        return;
    }
    case Tool::Measure: {
        Vec2 p = w;
        if (h >= 0) {   // snap to pad / centroid when close
            const Component& c = d.prj.comps[h];
            int pad = hit_pad(d, c, to_root(h, himg, w), tol);
            Vec2 q = pad >= 0 ? pad_world(d, c, pad) : c.pos;
            if (himg != 0) { Xform a = d.prj.comp_xform(c, 0).inverse(), b = d.prj.instance_xform(c, himg); q = b.apply(a.apply(q)); }
            if (dist(q, w) < 10.0 / scale_) p = q;
        } else p = snap_pt(w);
        if (meas_n == 0 || meas_n == 2) { meas[0] = p; meas_n = 1; }
        else {
            meas[1] = p; meas_n = 2;
            Vec2 dd = meas[1] - meas[0];
            gui::status("Distance " + fmt_len_u8(std::hypot(dd.x, dd.y)) + "   dx " + fmt_len_u8(dd.x) + "   dy " + fmt_len_u8(dd.y));
        }
        return;
    }
    case Tool::Label: case Tool::FrameCircuit: return;
    case Tool::FramePanel: case Tool::AlignMove: case Tool::AlignRotate: case Tool::Align2Ref: {
        const Tool tl = tool;
        picks[pick_n++] = pick_pt(w);
        if (pick_n < picks_needed(tl)) { gui::status(std::string(tool_name(tl)) + ": " + pick_prompt(tl, pick_n)); return; }
        pick_n = 0;
        DocPtr dp = doc;
        if (tl == Tool::FramePanel) {
            BBox r(std::min(picks[0].x, picks[1].x), std::min(picks[0].y, picks[1].y), std::max(picks[0].x, picks[1].x), std::max(picks[0].y, picks[1].y));
            defer([dp, r]() { if (dlg_confirm_panel_frame(frame_hwnd(), *dp, r)) dp->changed(ui::CH_GEOMETRY); });
            return;
        }
        Xform T; double drot = 0; std::string msg;
        if (tl == Tool::AlignMove) {
            Vec2 dd = picks[1] - picks[0];
            T = Xform::translate(dd.x, dd.y);
            msg = "Plot moved by " + fmt_len_u8(dd.x) + ", " + fmt_len_u8(dd.y);
        } else if (tl == Tool::AlignRotate) {
            Vec2 pv = picks[0], a = picks[1] - pv, b = picks[2] - pv;
            drot = rad2deg(std::atan2(b.y, b.x) - std::atan2(a.y, a.x));
            drot = std::remainder(drot, 360.0);
            T = Xform::translate(pv.x, pv.y) * Xform::rotation(drot) * Xform::translate(-pv.x, -pv.y);
            msg = "Plot rotated " + ui::U(ui::fmt_num(drot, 3)) + "\xC2\xB0 about " + fmt_pos_u8(d, pv);
        } else {
            Vec2 sa = picks[0], ta = picks[1], sb = picks[2], tb = picks[3];
            Vec2 vs = sb - sa, vt = tb - ta;
            if (std::hypot(vs.x, vs.y) < 1e-6) { gui::status("Points A and B coincide; pick two distinct references"); return; }
            drot = std::remainder(rad2deg(std::atan2(vt.y, vt.x) - std::atan2(vs.y, vs.x)), 360.0);
            T = Xform::translate(ta.x, ta.y) * Xform::rotation(drot) * Xform::translate(-sa.x, -sa.y);
            double ls = std::hypot(vs.x, vs.y), lt = std::hypot(vt.x, vt.y);
            msg = "Plot aligned: rotation " + ui::U(ui::fmt_num(drot, 3)) + "\xC2\xB0, A\xE2\x86\x92" "B length mismatch " + fmt_len_u8(lt - ls) + " (not scaled)";
        }
        apply_plot_xform(d, T, drot, tool_name(tl));
        tool = Tool::Select;
        gui::status(msg + "  \xC2\xB7  Ctrl+Z undoes");
        return;
    }
    }
}

void BoardCanvas::paint_overlay(ImDrawList* dl) {
    const GTheme& t = th();
    const Project& p = doc->prj;
    ImVec2 o0 = origin(), sz = size();
    if (flash_t0 >= 0) {
        const double k = (ImGui::GetTime() - flash_t0) / 1.4;
        if (k >= 1) { flash_t0 = -1; flash_ids.clear(); }
        else {
            const float a = (float)(1 - k);
            const ImU32 col = col_alpha(flash_col ? flash_col : t.accent, a);
            std::set<uint32_t> ids(flash_ids.begin(), flash_ids.end());
            for (auto& c : p.comps) if (ids.count(c.id)) {
                ImVec2 q = w2s(c.pos);
                dl->AddCircle(q, S(10) + S(14) * (float)k, col, 28, S(2.5f));
            }
            ImVec2 ts = ImGui::CalcTextSize(flash_label.c_str());
            ImVec2 a0(o0.x + (sz.x - ts.x) * 0.5f - S(14), o0.y + S(44)), a1(a0.x + ts.x + S(28), a0.y + ts.y + S(12));
            dl->AddRectFilled(a0, a1, col_alpha(t.panel, 0.92f * a), S(12));
            dl->AddRect(a0, a1, col, S(12), 0, S(1.5f));
            dl->AddCircleFilled(ImVec2(a0.x + S(10), (a0.y + a1.y) * 0.5f), S(4.5f), col);
            dl->AddText(ImVec2(a0.x + S(20), a0.y + S(6)), col_alpha(t.text, a), flash_label.c_str());
            request_redraw(2);
        }
    }
    // R17c (REVIEW_V16 §8): assignment status on the plot: unassigned placements on a selectable side carry an amber ring.
    // v17: the rings follow "Highlight unassigned" (on by default) and appear only while assignment is partial
    // (some placements assigned, some not): right after an import nothing is assigned and the rings would cover the board.
    if (doc->highlight_unassigned && assignment_partial(p) && p.comps.size() <= 20000) {
        const ImU32 rc = IM_COL32(255, 166, 0, 230);
        const int nimg = doc->all_images ? std::max(1, (int)p.images.size()) : 1;
        const ImVec2 cmin = o0, cmax(o0.x + sz.x, o0.y + sz.y);
        for (auto& c : p.comps) {
            if (c.feature != Feature::Component || c.assigned_point >= 0 || c.dnp || !doc->selectable(c.side)) continue;
            for (int im = 0; im < nimg; ++im) {
                if (im > 0 && !p.instanced(c)) break;
                const ImVec2 q = w2s(p.instance_xform(c, im).apply({0, 0}));
                if (q.x < cmin.x || q.y < cmin.y || q.x > cmax.x || q.y > cmax.y) continue;
                dl->AddCircle(q, S(5.5f), rc, 14, S(1.8f));
            }
        }
    }
    // origin marker
    {
        ImVec2 s = w2s(p.virtual_origin);
        ImU32 c = argb2im(ui::pal().origin);
        float r = S(7);
        dl->AddCircle(s, r, c, 24, S(1.6f));
        dl->AddLine(ImVec2(s.x - r * 1.8f, s.y), ImVec2(s.x + r * 1.8f, s.y), c, S(1.3f));
        dl->AddLine(ImVec2(s.x, s.y - r * 1.8f), ImVec2(s.x, s.y + r * 1.8f), c, S(1.3f));
        if (p.virtual_origin.x != 0 || p.virtual_origin.y != 0) dl->AddText(ImVec2(s.x + r + S(3), s.y + S(2)), c, "0,0");
    }
    // R17b (REVIEW_V16 §3): an Inferred outline is labelled as such on the canvas (its corner)
    if (doc->show_frame && !p.comps.empty() && p.board_outline_source() == OutlineSource::Inferred) {
        const BBox b = !p.panel_frame.empty() ? p.panel_frame_bbox() : p.board_bbox();
        const ImVec2 a = w2s({b.x0, b.y0}), c = w2s({b.x1, b.y1});
        const ImVec2 tl(std::min(a.x, c.x), std::min(a.y, c.y));
        char lab[96]; std::snprintf(lab, sizeof lab, "INFERRED OUTLINE (estimated)  %s \xC3\x97 %s", fmt_len_u8(b.w(), false).c_str(), fmt_len_u8(b.h()).c_str());
        const ImVec2 ts = ImGui::CalcTextSize(lab);
        const ImVec2 q0(tl.x, tl.y - ts.y - S(8)), q1(q0.x + ts.x + S(12), tl.y - S(2));
        dl->AddRectFilled(q0, q1, IM_COL32(64, 192, 87, 235), S(4));   // R22: same green as the hatched dashed outline
        dl->AddText(ImVec2(q0.x + S(6), q0.y + S(3)), IM_COL32(6, 24, 10, 255), lab);
    }
    // hover highlight + tooltip
    if (hover >= 0 && hover < (int)p.comps.size() && hovered() && !moving && !label_drag) {
        const Component& c = p.comps[hover];
        BBox b = inst_bbox(hover, hover_img);
        b.inflate(0.25);
        ImVec2 a = w2s({b.x0, b.y0}), bb = w2s({b.x1, b.y1});
        ImVec2 mn(std::min(a.x, bb.x), std::min(a.y, bb.y)), mx(std::max(a.x, bb.x), std::max(a.y, bb.y));
        if (hover != hover_seen) { hover_seen = hover; hover_t0 = ImGui::GetTime(); }
        float hk = std::min(1.f, (float)(ImGui::GetTime() - hover_t0) / 0.12f);
        if (hk < 1) request_redraw(2);
        float grow = (1 - hk) * S(4);
        mn.x -= grow; mn.y -= grow; mx.x += grow; mx.y += grow;
        dl->AddRectFilled(mn, mx, alpha(t.accent, 0.16f * hk), S(3));
        dl->AddRect(mn, mx, alpha(t.accent, 0.35f + 0.65f * hk), S(3), 0, S(1.5f));
        if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(8)));
            ImGui::BeginTooltip();
            ImGui::PushFont(g_font_bold);
            ImGui::TextUnformatted(c.ref.empty() ? "(no reference)" : c.ref.c_str());
            ImGui::PopFont();
            if (c.feature == Feature::Fiducial) { ImGui::SameLine(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.accent), "FIDUCIAL"); }
            if (c.dnp) { ImGui::SameLine(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.warn), "DNP"); }
            ImGui::TextDisabled("Part");    ImGui::SameLine(S(70)); ImGui::TextUnformatted(c.number.empty() ? "\xE2\x80\x94" : c.number.c_str());
            if (const PartNumber* pn = p.part_of(c)) if (!pn->description.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", pn->description.c_str()); }
            ImGui::TextDisabled("Package"); ImGui::SameLine(S(70)); ImGui::TextUnformatted(c.package.empty() ? "\xE2\x80\x94" : c.package.c_str());
            ImGui::TextDisabled("Position"); ImGui::SameLine(S(70));
            ImGui::Text("%s  %s\xC2\xB0  %s", fmt_pos_u8(*doc, c.pos).c_str(), ui::U(ui::fmt_num(c.rot, 2)).c_str(), c.side == Side::Top ? "Top" : "Bottom");
            if (c.assigned_point >= 0 && c.assigned_point < (int)p.model.points.size()) {
                ImGui::TextDisabled("Point"); ImGui::SameLine(S(70));
                ImVec2 q = ImGui::GetCursorScreenPos();
                float lh = ImGui::GetTextLineHeight();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(q.x + S(5), q.y + lh * 0.5f), S(4.5f), argb2im(p.model.points[c.assigned_point].color));
                ImGui::Dummy(ImVec2(S(12), lh)); ImGui::SameLine();
                ImGui::TextUnformatted(p.model.points[c.assigned_point].name.c_str());
            } else if (c.feature == Feature::Component) { ImGui::TextDisabled("Point"); ImGui::SameLine(S(70)); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.warn), "unassigned"); }
            ImGui::EndTooltip();
            ImGui::PopStyleVar();
        }
    }
    // measure
    if (tool == Tool::Measure || meas_n == 2) {
        if (meas_n >= 1) {
            Vec2 b = meas_n == 2 ? meas[1] : mouse_world();
            Vec2 a = meas[0];
            ImVec2 sa = w2s(a), sb = w2s(b), sc = w2s({b.x, a.y});
            ImU32 col = t.accent, dim = alpha(t.accent, 0.55f);
            dl->AddLine(sa, sc, dim, S(1)); dl->AddLine(sc, sb, dim, S(1));
            dl->AddLine(sa, sb, col, S(2));
            dl->AddCircleFilled(sa, S(4), col);
            dl->AddCircleFilled(sb, S(4), col);
            Vec2 dd = b - a;
            std::string l1 = fmt_len_u8(std::hypot(dd.x, dd.y));
            std::string l2 = "dx " + fmt_len_u8(dd.x) + "   dy " + fmt_len_u8(dd.y);
            ImGui::PushFont(g_font_mono);
            ImVec2 s1 = ImGui::CalcTextSize(l1.c_str()), s2 = ImGui::CalcTextSize(l2.c_str());
            float bw = std::max(s1.x, s2.x) + S(16), bh = s1.y + s2.y + S(12);
            ImVec2 m((sa.x + sb.x) * 0.5f + S(10), (sa.y + sb.y) * 0.5f + S(10));
            dl->AddRectFilled(m, ImVec2(m.x + bw, m.y + bh), alpha(t.panel, 0.94f), S(6));
            dl->AddRect(m, ImVec2(m.x + bw, m.y + bh), t.border, S(6));
            dl->AddText(ImVec2(m.x + S(8), m.y + S(5)), t.text, l1.c_str());
            dl->AddText(ImVec2(m.x + S(8), m.y + S(6) + s1.y), t.text_dim, l2.c_str());
            ImGui::PopFont();
        }
    }
    // rotation arrows (Verify Angles): bold, colour-coded, with a legend
    if (doc->verify_angles) {
        const bool dk = t.dark;
        const int nimg = doc->all_images ? std::max(1, (int)p.images.size()) : 1;
        const ImVec2 lo = o0, hi(o0.x + sz.x, o0.y + sz.y);
        const ImU32 halo = dk ? IM_COL32(0, 0, 0, 200) : IM_COL32(255, 255, 255, 220);
        int drawn = 0;
        for (auto& c : p.comps) {
            if (c.feature != Feature::Component) continue;
            if ((c.side == Side::Top && !doc->show_top) || (c.side == Side::Bottom && !doc->show_bottom)) continue;
            const Package* pk = p.package_of(c);
            BBox e = pk ? pk->extent() : BBox(-0.5, -0.5, 0.5, 0.5);
            const double L = std::max(0.4, std::max(e.w(), e.h()) * 0.5);
            const ImU32 col = angle_col(c.rot, dk);
            const int n = std::min(nimg, p.instance_count(c));
            for (int im = 0; im < n; ++im) {
                Xform x = p.instance_xform(c, im);
                ImVec2 s0 = w2s(x.apply({0, 0})), s1 = w2s(x.apply({L, 0}));
                if (s0.x < lo.x - 40 || s0.y < lo.y - 40 || s0.x > hi.x + 40 || s0.y > hi.y + 40) continue;
                ImVec2 dv(s1.x - s0.x, s1.y - s0.y);
                float len = std::sqrt(dv.x * dv.x + dv.y * dv.y);
                if (len < 1e-3f) continue;
                ImVec2 u(dv.x / len, dv.y / len);
                len = std::clamp(len, S(14), S(70));
                ImVec2 tip(s0.x + u.x * len, s0.y + u.y * len), nrm(-u.y, u.x);
                const float hw = std::max(S(4), len * 0.22f), hl = std::max(S(6), len * 0.35f);
                ImVec2 base(tip.x - u.x * hl, tip.y - u.y * hl);
                dl->AddLine(s0, base, halo, S(5));
                dl->AddLine(s0, base, col, S(2.6f));
                dl->AddTriangleFilled(ImVec2(tip.x + u.x * S(1.5f), tip.y + u.y * S(1.5f)), ImVec2(base.x + nrm.x * (hw + S(1.5f)), base.y + nrm.y * (hw + S(1.5f))),
                                      ImVec2(base.x - nrm.x * (hw + S(1.5f)), base.y - nrm.y * (hw + S(1.5f))), halo);
                dl->AddTriangleFilled(tip, ImVec2(base.x + nrm.x * hw, base.y + nrm.y * hw), ImVec2(base.x - nrm.x * hw, base.y - nrm.y * hw), col);
                dl->AddCircleFilled(s0, S(2.5f), col);
                if (len >= S(40) && drawn < 400) {
                    std::string a = ui::U(ui::fmt_num(norm_deg(c.rot), 1)) + "\xC2\xB0";
                    ImVec2 tp(tip.x + u.x * S(4) - S(8), tip.y + u.y * S(4) - S(7));
                    dl->AddText(ImVec2(tp.x + 1, tp.y + 1), halo, a.c_str());
                    dl->AddText(tp, col, a.c_str());
                    ++drawn;
                }
            }
        }
        // legend (bottom-left)
        struct Lg { double a; const char* t; } lg[] = {{0, "0\xC2\xB0  \xE2\x86\x92"}, {90, "90\xC2\xB0  \xE2\x86\x91"}, {180, "180\xC2\xB0  \xE2\x86\x90"}, {270, "270\xC2\xB0  \xE2\x86\x93"}, {45, "off-axis"}};
        const float lh = ImGui::GetTextLineHeight() + S(4), lw = S(250);
        ImVec2 a(o0.x + S(10), o0.y + sz.y - S(10) - lh * 7.3f), b(a.x + lw, o0.y + sz.y - S(10));
        dl->AddRectFilled(a, b, alpha(t.panel, 0.94f), S(8));
        dl->AddRect(a, b, t.border, S(8));
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImVec2(a.x + S(10), a.y + S(6)), t.text, "Rotation arrows");
        ImGui::PopFont();
        dl->AddText(ImVec2(a.x + S(10), a.y + S(6) + lh), t.text_dim, mirror_ ? "Rotation, CCW +  (viewed from below)" : "Rotation, CCW +  (0\xC2\xB0 points right)");
        for (int i = 0; i < 5; ++i) {
            float y = a.y + S(8) + lh * (2 + i) + lh * 0.5f;
            ImU32 col = angle_col(lg[i].a, t.dark);
            dl->AddLine(ImVec2(a.x + S(12), y), ImVec2(a.x + S(34), y), col, S(3));
            dl->AddTriangleFilled(ImVec2(a.x + S(42), y), ImVec2(a.x + S(33), y - S(5)), ImVec2(a.x + S(33), y + S(5)), col);
            dl->AddText(ImVec2(a.x + S(52), y - ImGui::GetTextLineHeight() * 0.5f), t.text, lg[i].t);
        }
    }
    // circuit frame tool: frame + 8 resize handles + size readout (live preview while dragging)
    if (tool == Tool::FrameCircuit) {
        BBox fb = p.circuit_frame_bbox();
        if (frame_drag && fr_handle > 0) fb = fr_preview;
        else if (!fb.empty()) fb = BBox(fb.x0 + frame_d.x, fb.y0 + frame_d.y, fb.x1 + frame_d.x, fb.y1 + frame_d.y);
        if (!fb.empty()) {
            ImVec2 a = w2s({fb.x0, fb.y0}), bb = w2s({fb.x1, fb.y1});
            ImVec2 mn(std::min(a.x, bb.x), std::min(a.y, bb.y)), mx(std::max(a.x, bb.x), std::max(a.y, bb.y));
            dl->AddRectFilled(mn, mx, alpha(t.accent, 0.06f));
            dl->AddRect(mn, mx, t.accent, 0, 0, S(2));
            const int hv = hovered() ? frame_handle(p.circuit_frame_bbox(), mouse_world()) : -1;
            ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
            for (ImVec2 q : {mn, mx, ImVec2(mn.x, mx.y), ImVec2(mx.x, mn.y), ImVec2(c.x, mn.y), ImVec2(c.x, mx.y), ImVec2(mn.x, c.y), ImVec2(mx.x, c.y)}) {
                dl->AddRectFilled(q - ImVec2(S(5), S(5)), q + ImVec2(S(5), S(5)), t.panel);
                dl->AddRect(q - ImVec2(S(5), S(5)), q + ImVec2(S(5), S(5)), t.accent, 0, 0, S(1.6f));
            }
            (void)hv;
            std::string l = fmt_len_u8(fb.w(), false) + " x " + fmt_len_u8(fb.h()) + "   " + fmt_pos_u8(*doc, {fb.x0, fb.y1}) + " \xE2\x80\x93 " + fmt_pos_u8(*doc, {fb.x1, fb.y0});
            ImVec2 ts = ImGui::CalcTextSize(l.c_str());
            ImVec2 la(c.x - ts.x * 0.5f - S(8), mx.y + S(8));
            dl->AddRectFilled(la, ImVec2(la.x + ts.x + S(16), la.y + ts.y + S(8)), alpha(t.panel, 0.94f), S(6));
            dl->AddText(ImVec2(la.x + S(8), la.y + S(4)), t.text, l.c_str());
        }
    }
    // point-pick tools: show gerber geometry + current panel frame, snap marker, picked points
    if (is_pick_tool(tool)) {
        const ImVec2 lo = o0, hi(o0.x + sz.x, o0.y + sz.y);
        auto vis = [&](ImVec2 q) { return q.x >= lo.x && q.y >= lo.y && q.x <= hi.x && q.y <= hi.y; };
        int budget = 120000;
        for (size_t i = 0; i < p.layers.size() && i < doc->glayers.size() && budget > 0; ++i) {
            if (!p.layers[i].visible || !doc->glayers[i]) continue;
            const Xform x = gerber_layer_xform(p.layers[i]);
            const ImU32 col = argb2im(p.layers[i].color, 0.75f);
            for (auto& o : doc->glayers[i]->objs) {
                if (o.clear || --budget <= 0) continue;
                if (o.kind == gerber::Object::Flash) {
                    ImVec2 q = w2s(x.apply(o.a));
                    if (vis(q)) dl->AddRectFilled(q - ImVec2(S(1.5f), S(1.5f)), q + ImVec2(S(1.5f), S(1.5f)), col);
                } else if (o.kind == gerber::Object::Region) {
                    for (size_t k = 0; k + 1 < o.poly.size(); ++k) dl->AddLine(w2s(x.apply(o.poly[k])), w2s(x.apply(o.poly[k + 1])), col, 1.f);
                } else {
                    ImVec2 qa = w2s(x.apply(o.a)), qb = w2s(x.apply(o.b));
                    if (vis(qa) || vis(qb)) dl->AddLine(qa, qb, col, 1.f);
                }
            }
        }
        BBox pf = p.panel_frame_bbox();
        if (!p.panel_frame.empty() && !pf.empty()) {
            ImVec2 a = w2s({pf.x0, pf.y0}), bb = w2s({pf.x1, pf.y1});
            dl->AddRect(ImVec2(std::min(a.x, bb.x), std::min(a.y, bb.y)), ImVec2(std::max(a.x, bb.x), std::max(a.y, bb.y)), alpha(t.warn, 0.8f), 0, 0, S(1.5f));
        }
        if (hovered()) {
            bool sn = false;
            Vec2 q = pick_pt(mouse_world(), &sn);
            ImVec2 sq = w2s(q);
            ImU32 c = sn ? t.ok : t.text_dim;
            dl->AddCircle(sq, S(7), c, 20, S(2));
            dl->AddLine(ImVec2(sq.x - S(11), sq.y), ImVec2(sq.x + S(11), sq.y), c, S(1.2f));
            dl->AddLine(ImVec2(sq.x, sq.y - S(11)), ImVec2(sq.x, sq.y + S(11)), c, S(1.2f));
            std::string l = fmt_pos_u8(*doc, q) + (sn ? "  snap" : "");
            dl->AddText(ImVec2(sq.x + S(12), sq.y + S(8)), c, l.c_str());
            if (pick_n > 0) {
                Vec2 last = picks[pick_n - 1];
                if (tool == Tool::FramePanel) {
                    ImVec2 a = w2s(last);
                    dl->AddRect(ImVec2(std::min(a.x, sq.x), std::min(a.y, sq.y)), ImVec2(std::max(a.x, sq.x), std::max(a.y, sq.y)), t.warn, 0, 0, S(2));
                    std::string wh = fmt_len_u8(std::fabs(q.x - last.x), false) + " x " + fmt_len_u8(std::fabs(q.y - last.y));
                    dl->AddText(ImVec2(sq.x + S(12), sq.y + S(24)), t.warn, wh.c_str());
                } else dl->AddLine(w2s(last), sq, t.accent, S(1.5f));
            }
        }
        for (int i = 0; i < pick_n; ++i) {
            ImVec2 q = w2s(picks[i]);
            dl->AddCircleFilled(q, S(8), t.accent);
            char n[4]; std::snprintf(n, sizeof n, "%d", i + 1);
            dl->AddText(ImVec2(q.x - S(3.5f), q.y - S(7)), t.accent_text, n);
            if (tool == Tool::Align2Ref && (i == 1 || i == 3)) dl->AddLine(w2s(picks[i - 1]), q, t.accent, S(2));
        }
    }
    // tool chip (top-left)
    {
        std::string s = tool_name(tool);
        if (doc->move_mode) s += "  \xC2\xB7  MOVE";
        s += view_bottom(*doc) ? "  \xC2\xB7  FROM BOTTOM (mirrored)" : "  \xC2\xB7  FROM TOP";
        // R17c (specs/VIEW_MODES.md): visibility and selectability of the other side, stated separately
        s += !doc->far_drawn() ? "  \xC2\xB7  OTHER SIDE HIDDEN"
           : doc->side_display == Doc::SideDisplay::Xray && doc->back_selectable ? "  \xC2\xB7  OTHER SIDE SHOWN + SELECTABLE" : "  \xC2\xB7  OTHER SIDE SHOWN (locked)";
        ImVec2 ts = ImGui::CalcTextSize(s.c_str());
        ImVec2 a(o0.x + S(10), o0.y + S(10)), b(a.x + ts.x + S(34), a.y + ts.y + S(10));
        dl->AddRectFilled(a, b, alpha(t.panel, 0.9f), S(8));
        dl->AddRect(a, b, doc->move_mode ? t.warn : t.border, S(8), 0, doc->move_mode ? S(1.5f) : 1.f);
        dl->AddCircleFilled(ImVec2(a.x + S(13), (a.y + b.y) * 0.5f), S(4), doc->move_mode ? t.warn : t.accent);
        dl->AddText(ImVec2(a.x + S(24), a.y + S(5)), t.text, s.c_str());
    }
    // active-tool hint pill (bottom centre), fades in when the tool / stage changes
    {
        std::string h;
        if (tool == Tool::Measure) h = meas_n == 1 ? "Click the second point  \xC2\xB7  Esc to cancel" : "Click two points  \xC2\xB7  Esc to clear";
        else if (tool == Tool::Origin) h = "Click a component, pad or point to set the origin  \xC2\xB7  Esc to cancel";
        else if (tool == Tool::FrameCircuit) h = "Outlines are edited numerically: double-click or Board > Outline > Add Outline...";
        else if (is_pick_tool(tool)) h = std::string(pick_prompt(tool, pick_n)) + "  \xC2\xB7  Esc resets";
        else if (tool == Tool::Pin1) h = "Click the lead where pin 1 should be: the placement rotates (package unchanged)  \xC2\xB7  Esc to cancel";
        else if (tool == Tool::Label) h = "Drag a reference label to move it  \xC2\xB7  Esc to cancel";
        else if (tool == Tool::Info) h = "Click a component to show its details";
        else if (tool == Tool::SelPart) h = "Click a component to select every part with the same P/N";
        else if (tool == Tool::SelPackage) h = "Click a component to select every part with the same package";
        else if (tool == Tool::SelTech) h = "Click a component to select every part with the same technology";
        else if (doc->move_mode) h = "Move mode: drag components to reposition them  \xC2\xB7  R rotates";
        if (h != hint_text) { hint_text = h; hint_t0 = ImGui::GetTime(); }
        if (!h.empty()) {   // P1-07: tutorial hint (off by default, dismissable per tool)
            float k = std::min(1.f, (float)(ImGui::GetTime() - hint_t0) / 0.25f);
            float e = perf_mode() ? 1.f : 1 - (1 - k) * (1 - k);
            const std::string key = "plot.tool" + std::to_string((int)tool) + (doc->move_mode && tool == Tool::Select ? "m" : "");
            if (hint_pill(dl, key.c_str(), h, ImVec2(o0.x + sz.x * 0.5f, o0.y + sz.y - S(14) + (1 - e) * S(10)), true,
                          doc->move_mode && tool == Tool::Select ? ui::Icon::Move : ui::Icon::Question, e) && k < 1) request_redraw(2);
        }
    }
    (void)sz;
}

// ---------------------------------------------------------------------------
void PlotterView::draw_toolbar() {
    // Groups are kept by priority; whatever does not fit the strip goes into a
    // trailing '»' overflow popup (UI_FEEDBACK: toolbar hid controls at 1600 px).
    // R17c (REVIEW_V16 §7-§10): Component Info / Label mode / Select by P/N, package, technology left the toolbar
    // (keys I / L, menus, and right-click > Select Similar); zoom / pan moved to the floating viewport toolbar on the
    // canvas; Assign / Unassign to the ACTIVE export layer is a first-class group with a layer chip.
    auto tb = [](int id) { cmd_button(id); };
    const float btn = S(32), sepw = S(13);
    static const int kTools[] = {ID_TOOL_SELECT, ID_TOOL_PIN1, ID_TOOL_ORIGIN, ID_TOOL_FRAME_CIRCUIT};
    static const int kRot[] = {ID_TOOL_ROT90, ID_TOOL_ROT180, ID_TOOL_ROT270};
    static const int kShow[] = {ID_TOOL_SHOW_NOREF, ID_TOOL_VERIFY_ANGLES};
    auto pin1 = [this]() {   // R17c: polarity markers = icon toggle (was the "Polarity: on/off" text button)
        const bool on = g_orient_markers != 0;
        if (icon_button("##polarity", ui::Icon::Polarity,
                        on ? "Polarity markers: ON  (click to hide)\nEach package's polarity marker (dot / arrow, placed in the package editor) is drawn,\n"
                             "rotated and mirrored with the placement, to compare with the board silkscreen.\nParts without a polarity marker show nothing."
                           : "Polarity markers: OFF  (click to show)\nShow each package's polarity marker (dot / arrow) rotated and mirrored with the placement.",
                        on, true, S(30))) { g_orient_markers = on ? 0 : 1; canvas.invalidate(); }
        ImGui::SameLine(0, S(2));
    };
    auto side_w = []() {
        float w = 0;
        for (const char* l : {"Top", "Bottom"}) w += ImGui::CalcTextSize(l).x + S(36);
        return w + S(6) + S(30) * 2 + S(8);
    };
    struct G { int prio; float w; std::function<void()> draw; };
    std::vector<G> gs;
    // R14: Board > Align CAD to Gerber sits with the tools (always visible; cmd_button -> post_command; REVIEW_V12 §6)
    gs.push_back({0, btn * 5, [&]() { for (int id : kTools) tb(id); tb(ID_BOARD_ALIGN_CAD_GERBER); }});
    gs.push_back({2, side_w(), [&]() { draw_side_toggle(); }});
    gs.push_back({3, btn, pin1});
    gs.push_back({4, btn * 3, [&]() { for (int id : kRot) tb(id); }});
    gs.push_back({1, assign_group_width(), [&]() { draw_assign_group(); }});
    gs.push_back({5, btn * 2, [&]() { for (int id : kShow) tb(id); }});
    gs.push_back({6, btn, [&]() { tb(ID_VIEWER_ALL_IMAGES); }});
    if (selsets_visible(*doc)) gs.push_back({7, S(32) + S(150), [&]() { draw_selset_combo(); }});   // P2-10: legacy projects only
    else gs.push_back({7, 0, [&]() {}});
    // display order: tools | assign | rot | side | polarity | images | show | selsets
    static const int kOrder[] = {0, 4, 3, 1, 2, 6, 5, 7};
    const float avail = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetCursorScreenPos().x - S(16);
    std::vector<bool> keep(gs.size(), false);
    float used = 0;
    const float ovw = S(34);
    float total = 0;
    for (auto& g : gs) total += g.w + sepw;
    const bool all_fit = total <= avail;
    for (int pr = 0; pr < (int)gs.size(); ++pr)
        for (size_t i = 0; i < gs.size(); ++i)
            if (gs[i].prio == pr && (all_fit || used + gs[i].w + sepw + ovw <= avail)) { keep[i] = true; used += gs[i].w + sepw; }
    bool first = true, hidden = false;
    for (int i : kOrder) {
        if (gs[(size_t)i].w <= 0) continue;
        if (!keep[(size_t)i]) { hidden = true; continue; }
        if (!first) toolbar_separator();
        first = false;
        gs[(size_t)i].draw();
    }
    if (hidden) {
        toolbar_separator();
        if (ImGui::Button("\xC2\xBB##tbover", ImVec2(S(30), S(30)))) ImGui::OpenPopup("##tbover");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("More tools (the window is too narrow to show them all)");
        ImGui::SameLine(0, S(2));
        if (ImGui::BeginPopup("##tbover")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(2), S(4)));
            for (int i : kOrder) {
                if (keep[(size_t)i]) continue;
                ImGui::PushID(i);
                gs[(size_t)i].draw();
                ImGui::NewLine();
                ImGui::PopID();
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }
    }
}

void PlotterView::draw_activity_bar(float w, float h) {
    const GTheme& t = th();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), t.panel);
    dl->AddLine(p, ImVec2(p.x, p.y + h), t.border);
    struct It { int tab; ui::Icon ic; const char* tip; };
    const It items[] = {{TAB_FACTORY, ui::Icon::FactoryModel, "Factory Model"}, {TAB_SEARCH, ui::Icon::FindRef, "Search  (Ctrl+F)"},
                        {TAB_COMPS, ui::Icon::Locations, "Components (tree, flags)"},
                        {TAB_REVIEW, ui::Icon::Report, "Placement Review"}, {TAB_INFO, ui::Icon::CompInfo, "Component Info"}};
    float y = p.y + S(6);
    for (auto& it : items) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + (w - S(30)) * 0.5f, y));
        ImGui::PushID(it.tab);
        bool on = g_panel_open && g_panel_tab == it.tab;
        if (icon_button("ab", it.ic, it.tip, on, true, S(30))) {
            if (on) g_panel_open = false;
            else open_tab(it.tab);
            if (it.tab == TAB_SEARCH && g_panel_open) focus_search = true;
        }
        ImGui::PopID();
        if (it.tab == TAB_REVIEW && review_ran && (review_err + review_warn) > 0) {
            std::string n = std::to_string(review_err + review_warn);
            ImVec2 ns = ImGui::CalcTextSize(n.c_str());
            float bx = p.x + w - S(6), by = y + S(2);
            float bw2 = std::max(ns.x * 0.75f + S(6), S(14));
            ImVec2 a(bx - bw2, by), b(bx, by + S(13));
            dl->AddRectFilled(a, b, review_err ? t.error : t.warn, S(7));
            ImGui::SetWindowFontScale(0.75f);
            ImVec2 ns2 = ImGui::CalcTextSize(n.c_str());
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2((a.x + b.x - ns2.x) * 0.5f, (a.y + b.y - ns2.y) * 0.5f), IM_COL32(255, 255, 255, 255), n.c_str());
            ImGui::SetWindowFontScale(1.f);
        }
        y += S(38);
    }
    // collapse / expand toggle at the bottom
    ImGui::SetCursorScreenPos(ImVec2(p.x + (w - S(30)) * 0.5f, p.y + h - S(38)));
    if (icon_button("abtog", g_panel_open ? ui::Icon::Arrow : ui::Icon::Layers, g_panel_open ? "Hide side panel" : "Show side panel", false, true, S(30)))
        g_panel_open = !g_panel_open;
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
}

void PlotterView::draw(ImVec2 size) {
    Doc& d = *doc;
    if (canvas.owner != this) { canvas.owner = this; canvas.doc = doc; }
    if (g_panel_w <= 0) g_panel_w = S(330);
    if (d.gen != seen_gen) {
        seen_gen = d.gen;
        canvas.invalidate();
        if (canvas.hover >= (int)d.prj.comps.size()) canvas.hover = -1;
    }
    if (canvas.mirror() != view_bottom(d)) canvas.set_mirror(view_bottom(d));
    canvas.set_grid(settings().grid_visible, settings().grid_mm);
    canvas.set_snap(settings().snap);

    // R12 §7/§12: Ctrl+Space = maximize / restore the plot canvas (side panel + activity bar hidden)
    static bool plot_max = false;
    {
        ImGuiIO& io0 = ImGui::GetIO();
        if (canvas.hovered() && io0.KeyCtrl && !io0.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false)) { plot_max = !plot_max; canvas.invalidate(); }
    }
    const bool panel_open = g_panel_open && !plot_max;
    const float act_w = plot_max ? 0.f : S(42), split_w = S(6);
    float pw = panel_open ? std::clamp(g_panel_w, S(240), std::max(S(240), size.x * 0.55f)) : 0;
    float cw = size.x - act_w - (panel_open ? pw + split_w : 0);
    ImVec2 start = ImGui::GetCursorScreenPos();

    // keyboard tool shortcuts (canvas hovered, not typing)
    ImGuiIO& io = ImGui::GetIO();
    if (canvas.hovered() && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt) {
        if (ImGui::IsKeyPressed(ImGuiKey_S) || ImGui::IsKeyPressed(ImGuiKey_V)) canvas.tool = Tool::Select;
        if (ImGui::IsKeyPressed(ImGuiKey_I)) canvas.tool = Tool::Info;
        if (ImGui::IsKeyPressed(ImGuiKey_O)) canvas.tool = Tool::Origin;
        if (ImGui::IsKeyPressed(ImGuiKey_L)) canvas.tool = Tool::Label;
        if (ImGui::IsKeyPressed(ImGuiKey_P)) canvas.tool = Tool::Pin1;
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) post_command(io.KeyShift ? ID_VIEW_SIDE_CYCLE : d.far_drawn() ? ID_VIEW_SIDE_NORMAL : ID_VIEW_SIDE_XRAY);
        if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_1, false)) post_command(ID_VIEW_SIDE_TOP);
        if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_2, false)) post_command(ID_VIEW_SIDE_BOTTOM);
        if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_3, false)) post_command(ID_VIEW_SIDE_XRAY);
        if (!io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_K, false)) post_command(ID_VIEW_BACK_SELECT);
        // R17c (specs/VIEW_MODES.md): Ghost merged into "other side shown" -> G toggles like X; N hides it
        if (!io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_G, false)) post_command(d.far_drawn() ? ID_VIEW_SIDE_NORMAL : ID_VIEW_SIDE_XRAY);
        if (!io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N, false)) post_command(ID_VIEW_SIDE_NORMAL);
        // R17c (REVIEW_V16 §8): A = assign the selection to the active export layer, Shift+A / U = unassign
        if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_A, false)) post_command(ID_TOOL_ASSIGN);
        if ((io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_A, false)) || (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_U, false))) post_command(ID_TOOL_UNASSIGN);
        if ((canvas.pan_mode() || canvas.zoom_rect_mode()) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) canvas.leave_nav_modes();   // R24a
        if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_H, false)) canvas.set_pan_mode(!canvas.pan_mode());
    }

    if (d.prj.comps.empty() && d.prj.layers.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, th().canvas);
        ImGui::BeginChild("##plotempty", ImVec2(cw, size.y));
        if (empty_state(ui::Icon::UniversalView, "Nothing to plot yet", "Import a CAD or centroid file, or load Gerber layers and add components from pads.",
                        "Import CAD...", ui::Icon::ImportCad))
            post_command(ID_IMPORT_CAD);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else
    {
        canvas.draw("##plotcanvas", ImVec2(cw, size.y));
        draw_viewport_tools(start, ImVec2(start.x + cw, start.y + size.y));   // R17c: floating, inside the canvas rect
    }
    if (pending_fly) { pending_fly = false; canvas.fly_to(fly_box); }
    if (canvas.ctx_pending) { canvas.ctx_pending = false; ImGui::OpenPopup("##plotctx"); }
    draw_context_menu();
    // R16/R17b (REVIEW_V16 §4, specs/OUTLINES.md): an Inferred outline is never silently authoritative. Compact
    // amber warning pinned to the canvas's lower-right corner (clear of the placements and the viewport tools);
    // acknowledge (Use as-is / Don't warn) without opening the editor.
    if (!d.prj.comps.empty() && !d.prj.inferred_outline_acknowledged()) {
        const ImVec2 keep = ImGui::GetCursorScreenPos();
        const float bw = std::min(S(560), cw - S(16)), bh = S(36);
        ImGui::SetCursorScreenPos(ImVec2(start.x + cw - bw - S(10), start.y + size.y - bh - S(10)));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(64, 42, 4, 240));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 159, 26, 255));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(4)));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(8));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, S(1.5f));
        ImGui::BeginChild("##outline_unconfirmed", ImVec2(bw, bh), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        outline_ack_strip(doc, true);
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        ImGui::SetCursorScreenPos(keep);
    }

    if (panel_open) {
        // splitter
        ImGui::SetCursorScreenPos(ImVec2(start.x + cw, start.y));
        ImGui::InvisibleButton("##split", ImVec2(split_w, size.y));
        bool sh = ImGui::IsItemHovered(), sa = ImGui::IsItemActive();
        if (sh || sa) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (sa && io.MouseDelta.x != 0) { g_panel_w = std::clamp(pw - io.MouseDelta.x, S(240), std::max(S(240), size.x * 0.55f)); }
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(start.x + cw, start.y), ImVec2(start.x + cw + split_w, start.y + size.y),
                                                  sa ? th().accent : (sh ? th().border : th().bg));
        ImGui::SetCursorScreenPos(ImVec2(start.x + cw + split_w, start.y));
        draw_panel(pw, size.y);
    }
    if (!plot_max) {
        ImGui::SetCursorScreenPos(ImVec2(start.x + size.x - act_w, start.y));
        draw_activity_bar(act_w, size.y);
    }
}

void PlotterView::draw_panel(float w, float h) {
    const GTheme& t = th();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(8)));
    ImGui::BeginChild("##sidepanel", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    // auto-refresh review when the document changed and the tab is visible
    if (g_panel_tab == TAB_REVIEW || g_panel_tab == TAB_COMPS || review_ran) ensure_review();
    if (ImGui::BeginTabBar("##ptabs", ImGuiTabBarFlags_FittingPolicyResizeDown)) {
        // tabs: icon + label + optional count badge (pulses briefly when the count changes)
        auto tab = [&](int id, ui::Icon ic, const char* name, const char* sid, int count = -1, ImU32 bcol = 0) {
            ImGuiTabItemFlags f = g_tab_req == id ? ImGuiTabItemFlags_SetSelected : 0;
            const float is = ImGui::GetFontSize();
            std::string cnt = count >= 0 ? std::to_string(count) : std::string();
            float bw = cnt.empty() ? 0 : ImGui::CalcTextSize(cnt.c_str()).x + S(10);
            float spw = ImGui::CalcTextSize(" ").x;
            int pad_l = (int)std::ceil((is + S(4)) / spw), pad_r = bw > 0 ? (int)std::ceil((bw + S(4)) / spw) : 0;
            // compact (icon + badge only, name in tooltip) when the panel is too narrow for full labels
            const float per_tab = ImGui::GetContentRegionAvail().x / 5.f - ImGui::GetStyle().ItemInnerSpacing.x;
            const float full = is + S(4) + ImGui::CalcTextSize(name).x + (bw > 0 ? bw + S(4) : 0) + ImGui::GetStyle().FramePadding.x * 2;
            const bool compact = id != g_panel_tab && full > per_tab;
            std::string label = std::string(pad_l, ' ') + (compact ? "" : name) + std::string(pad_r, ' ') + sid;
            bool open = ImGui::BeginTabItem(label.c_str(), nullptr, f);
            ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
            if (compact && ImGui::IsItemHovered()) tooltip(name);
            ImDrawList* tdl = ImGui::GetWindowDrawList();
            const float fp = ImGui::GetStyle().FramePadding.x;
            icon_draw(tdl, ic, ImVec2(r0.x + fp, (r0.y + r1.y - is) * 0.5f), is, open ? 1.f : 0.6f);
            if (bw > 0) {
                static std::map<int, std::pair<int, double>> seen;
                auto& sv = seen[id];
                if (sv.first != count) { sv = {count, ImGui::GetTime()}; }
                float k = std::min(1.f, (float)(ImGui::GetTime() - sv.second) / 0.4f);
                if (k < 1) request_redraw(2);
                float pulse = (1 - k) * S(2);
                float h = ImGui::GetTextLineHeight();
                ImVec2 a(r1.x - fp - bw - pulse, (r0.y + r1.y - h) * 0.5f - pulse), b(r1.x - fp + pulse, (r0.y + r1.y + h) * 0.5f + pulse);
                tdl->AddRectFilled(a, b, bcol ? bcol : alpha(t.text_dim, 0.35f), h);
                tdl->AddText(ImVec2(a.x + S(5) + pulse, a.y + pulse), bcol ? IM_COL32(255, 255, 255, 255) : t.text, cnt.c_str());
            }
            if (open) g_panel_tab = id;
            return open;
        };
        if (tab(TAB_FACTORY, ui::Icon::FactoryModel, "Layers", "###tf")) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
            draw_factory_panel(doc, ImGui::GetContentRegionAvail().x);
            ImGui::PopStyleColor();
            ImGui::EndTabItem();
        }
        if (tab(TAB_SEARCH, ui::Icon::FindRef, "Search", "###ts", *query ? (int)results.size() : -1)) { draw_search(); ImGui::EndTabItem(); }
        if (tab(TAB_COMPS, ui::Icon::CompInfo, "Parts", "###tc", (int)doc->prj.comps.size())) { draw_comps(); ImGui::EndTabItem(); }
        if (tab(TAB_REVIEW, review_err ? ui::Icon::Warning : ui::Icon::Check, "Review", "###tr", review_ran ? (int)issues.size() : -1,
                review_err ? t.error : review_warn ? t.warn : 0)) { draw_review(); ImGui::EndTabItem(); }
        if (tab(TAB_INFO, ui::Icon::About, "Info", "###ti")) { draw_info(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
        g_tab_req = -1;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PlotterView::draw_search() {
    const GTheme& t = th();
    Doc& d = *doc;
    if (focus_search) { ImGui::SetKeyboardFocusHere(); focus_search = false; }
    bool entered = false;
    {
        ImGui::SetNextItemWidth(-1);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x + S(20), ImGui::GetStyle().FramePadding.y + S(2)));
        entered = ImGui::InputTextWithHint("##q", "R1-R20, C*, 0603, 10k ...", query, sizeof query, ImGuiInputTextFlags_EnterReturnsTrue);
        bool active = ImGui::IsItemActive();
        ImGui::PopStyleVar();
        float is = ImGui::GetFontSize();
        icon_draw(ImGui::GetWindowDrawList(), ui::Icon::FindRef, ImVec2(p.x + S(6), p.y + (ImGui::GetFrameHeight() + S(4) - is) * 0.5f), is, 0.6f);
        if (active && !results.empty()) {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { search_cursor = std::min(search_cursor + 1, (int)results.size() - 1); focus_components(doc, {results[search_cursor]}); }
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { search_cursor = std::max(search_cursor - 1, 0); focus_components(doc, {results[search_cursor]}); }
        }
    }
    if (last_query != query || search_gen != d.gen) run_search();
    if (entered) { select_results(); focus_search = true; }

    ImGui::Spacing();
    if (!*query) {
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
        ImGui::TextWrapped("Type a reference, part number, package or value. Wildcards (* ?) and ranges (R1-R20, C5-9) work; separate several terms with commas.");
        ImGui::Spacing();
        ImGui::TextWrapped("Enter selects every match and frames them. Click a result to fly to it; the checkbox toggles its selection.");
        ImGui::PopStyleColor();
        return;
    }
    int nsel = 0;
    for (int i : results) if (d.prj.comps[i].selected) nsel++;
    ImGui::TextDisabled("%d match%s  \xC2\xB7  %d selected", (int)results.size(), results.size() == 1 ? "" : "es", nsel);
    float bw = (ImGui::GetContentRegionAvail().x - S(6)) * 0.5f;
    if (button("Select all results", ui::Icon::Select, true, ImVec2(bw, 0), !results.empty())) select_results();
    ImGui::SameLine(0, S(6));
    if (button("Clear", ui::Icon::None, false, ImVec2(bw, 0))) { query[0] = 0; run_search(); focus_search = true; }
    ImGui::Spacing();

    if (results.empty()) { empty_state(ui::Icon::FindRef, "No matches", "Check the spelling, or widen the search with wildcards (C*, *0603*) or ranges (R1-R20)."); return; }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel2);
    ImGui::BeginChild("##results", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clip;
    const float rh = ImGui::GetTextLineHeight() * 2 + S(10);
    clip.Begin((int)results.size(), rh);
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            int i = results[r];
            Component& c = d.prj.comps[i];
            ImGui::PushID(i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            bool clicked = ImGui::InvisibleButton("row", ImVec2(w, rh));
            bool hov = ImGui::IsItemHovered();
            if (c.selected) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), t.sel, S(5));
            else if (hov || r == search_cursor) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), t.raised, S(5));
            // checkbox
            float cb = S(15);
            ImVec2 ca(p.x + S(6), p.y + (rh - cb) * 0.5f), cb2(ca.x + cb, ca.y + cb);
            bool cb_hit = ImGui::IsMouseHoveringRect(ca - ImVec2(S(3), S(3)), cb2 + ImVec2(S(3), S(3)));
            dl->AddRectFilled(ca, cb2, c.selected ? t.accent : t.field, S(3));
            dl->AddRect(ca, cb2, c.selected ? t.accent : t.border, S(3));
            if (c.selected) {
                dl->PathLineTo(ImVec2(ca.x + cb * 0.22f, ca.y + cb * 0.52f));
                dl->PathLineTo(ImVec2(ca.x + cb * 0.42f, ca.y + cb * 0.72f));
                dl->PathLineTo(ImVec2(ca.x + cb * 0.78f, ca.y + cb * 0.28f));
                dl->PathStroke(t.accent_text, 0, S(2));
            }
            // point colour dot
            float tx = cb2.x + S(10);
            if (c.assigned_point >= 0 && c.assigned_point < (int)d.prj.model.points.size())
                dl->AddCircleFilled(ImVec2(p.x + w - S(12), p.y + rh * 0.5f), S(4.5f), argb2im(d.prj.model.points[c.assigned_point].color));
            ImU32 tc = c.selected ? t.sel_text : t.text;
            ImGui::PushFont(g_font_bold);
            dl->AddText(ImVec2(tx, p.y + S(4)), tc, c.ref.empty() ? "(no ref)" : c.ref.c_str());
            float rw = ImGui::CalcTextSize(c.ref.empty() ? "(no ref)" : c.ref.c_str()).x;
            ImGui::PopFont();
            std::string side = std::string(c.side == Side::Top ? "Top" : "Bot") + (c.dnp ? " \xC2\xB7 DNP" : "") + (c.feature == Feature::Fiducial ? " \xC2\xB7 FID" : "");
            dl->AddText(ImVec2(tx + rw + S(8), p.y + S(4)), t.text_faint, side.c_str());
            std::string l2 = (c.number.empty() ? std::string("\xE2\x80\x94") : c.number) + "   " + c.package;
            dl->PushClipRect(ImVec2(tx, p.y), ImVec2(p.x + w - S(22), p.y + rh), true);
            dl->AddText(ImVec2(tx, p.y + S(6) + ImGui::GetTextLineHeight()), t.text_dim, l2.c_str());
            dl->PopClipRect();
            if (clicked) {
                search_cursor = r;
                if (cb_hit || ImGui::GetIO().KeyCtrl) { c.selected = !c.selected; d.changed(ui::CH_SELECTION); }
                else if (ImGui::GetIO().KeyShift) { c.selected = true; d.changed(ui::CH_SELECTION); fly_to_comps({i}); }
                else focus_components(doc, {i});
            }
            if (hov && ImGui::IsMouseDoubleClicked(0)) post_command(ID_CTX_EDIT_LOCATION);
            ImGui::PopID();
        }
    }
    if (results.empty()) ImGui::TextDisabled("No matches.");
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void PlotterView::draw_review() {
    const GTheme& t = th();
    Doc& d = *doc;
    const bool busy = review_busy();
    if (button(busy ? "Checking..." : "Re-run", ui::Icon::Report, true, ImVec2(0, 0), !busy)) run_review();
    ImGui::SameLine(0, S(10));
    ImGui::AlignTextToFramePadding();
    if (busy) { spinner(); ImGui::SameLine(0, S(6)); ImGui::TextDisabled("Checking %d placements...", (int)d.prj.comps.size()); }
    else ImGui::TextDisabled("%d issues", (int)issues.size());
    if (!review_ran) { ImGui::Spacing(); progress_bar(-1, ImVec2(-1, S(4))); return; }
    auto pill = [&](int n, ImU32 col, const char* lbl) {
        std::string s = std::to_string(n) + " " + lbl;
        ImVec2 ts = ImGui::CalcTextSize(s.c_str());
        ImVec2 p = ImGui::GetCursorScreenPos();
        float hh = ImGui::GetFrameHeight();
        p.y += S(2);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + ts.x + S(22), p.y + hh - S(2)), alpha(col, n ? 0.22f : 0.08f), S(9));
        dl->AddCircleFilled(ImVec2(p.x + S(9), p.y + (hh - S(2)) * 0.5f), S(3.5f), n ? col : t.text_faint);
        dl->AddText(ImVec2(p.x + S(16), p.y + (hh - S(2) - ts.y) * 0.5f), n ? t.text : t.text_dim, s.c_str());
        ImGui::Dummy(ImVec2(ts.x + S(22), hh));
        ImGui::SameLine(0, S(4));
    };
    ImGui::NewLine();
    pill(review_err, t.error, "errors");
    pill(review_warn, t.warn, "warnings");
    pill(review_info, t.accent, "info");
    ImGui::NewLine();
    // code filter chips
    {
        std::map<int, int> per;
        for (auto& is : issues) per[(int)is.code]++;
        ImDrawList* dl0 = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float x0 = p.x, maxx = p.x + ImGui::GetContentRegionAvail().x, rowh = ImGui::GetFontSize() * 0.78f + S(4);
        for (auto& kv : per) {
            std::string sc = std::string(short_code((CheckCode)kv.first)) + " " + std::to_string(kv.second);
            float w = g_font_bold->CalcTextSizeA(ImGui::GetFontSize() * 0.78f, FLT_MAX, 0, sc.c_str()).x + S(8);
            if (p.x + w > maxx) { p.x = x0; p.y += rowh + S(3); }
            bool on = (review_filter >> kv.first) & 1;
            ImGui::SetCursorScreenPos(p);
            ImGui::PushID(kv.first + 700);
            if (ImGui::InvisibleButton("rf", ImVec2(w, rowh))) review_filter ^= 1ull << kv.first;
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hov) tooltip((std::string(check_code_name((CheckCode)kv.first)) + "\nClick to show only this code (combine several; click again to clear)").c_str());
            ImVec2 q = p;
            draw_badge(dl0, q, sc.c_str(), on ? t.accent : (hov ? t.text : t.text_dim), false);
            p.x = q.x;
        }
        if (!per.empty()) { ImGui::SetCursorScreenPos(ImVec2(x0, p.y + rowh + S(4))); ImGui::Dummy(ImVec2(0, 0)); }
        if (review_filter) {
            if (button("Show all codes", ui::Icon::None, false)) review_filter = 0;
        }
    }
    ImGui::Spacing();
    if (issues.empty()) {
        empty_state(ui::Icon::Check, "No placement issues", "Every placement passed the checks. They re-run automatically when the project changes.");
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel2);
    ImGui::BeginChild("##issues", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    size_t i = 0;
    int row = 0;
    while (i < issues.size()) {
        size_t j = i;
        Severity worst = issues[i].severity;
        while (j < issues.size() && issues[j].code == issues[i].code && issues[j].severity == issues[i].severity) ++j;
        if (review_filter && !(review_filter & code_bit(issues[i].code))) { i = j; continue; }
        ImU32 sc = worst == Severity::Error ? t.error : worst == Severity::Warning ? t.warn : t.accent;
        ImGui::PushID((int)i);
        std::string hdr = std::string(check_code_name(issues[i].code)) + "  (" + std::to_string(j - i) + ")";
        ImVec2 hp = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemOpen(j - i <= 25 || worst == Severity::Error, ImGuiCond_Once);
        ImGui::PushStyleColor(ImGuiCol_Header, alpha(sc, 0.14f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, alpha(sc, 0.24f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, alpha(sc, 0.30f));
        bool open = ImGui::CollapsingHeader(("      " + hdr).c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::PopStyleColor(3);
        {   // short code badge at the right of the header
            float hh = ImGui::GetFrameHeight();
            ImVec2 bp(hp.x + ImGui::GetContentRegionAvail().x - S(4), hp.y + (hh - (ImGui::GetFontSize() * 0.78f + S(4))) * 0.5f);
            draw_badge(dl, bp, short_code(issues[i].code), sc, true);
        }
        {   // severity glyph
            float hh = ImGui::GetFrameHeight();
            ImVec2 c(hp.x + S(30), hp.y + hh * 0.5f);
            dl->AddCircleFilled(c, S(7), sc);
            const char* g = worst == Severity::Info ? "i" : "!";
            ImGui::PushFont(g_font_bold);
            ImVec2 gs = ImGui::CalcTextSize(g);
            dl->AddText(ImVec2(c.x - gs.x * 0.5f, c.y - gs.y * 0.5f), IM_COL32(255, 255, 255, 255), g);
            ImGui::PopFont();
        }
        if (open) {
            for (size_t k = i; k < j; ++k) {
                const PlacementIssue& is = issues[k];
                ImGui::PushID((int)k);
                ImVec2 p = ImGui::GetCursorScreenPos();
                float w = ImGui::GetContentRegionAvail().x;
                float wrap = w - S(26);
                ImVec2 ts = ImGui::CalcTextSize(is.message.c_str(), nullptr, false, wrap);
                float rh = ts.y + S(10);
                bool clicked = ImGui::InvisibleButton("iss", ImVec2(w, rh));
                bool hov = ImGui::IsItemHovered();
                bool sel = issue_cursor == (int)k;
                if (sel) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), t.sel, S(5));
                else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), t.raised, S(5));
                dl->AddRectFilled(ImVec2(p.x + S(8), p.y + S(5)), ImVec2(p.x + S(11), p.y + rh - S(5)), sc, S(1.5f));
                dl->AddText(nullptr, 0, ImVec2(p.x + S(20), p.y + S(5)), sel ? t.sel_text : t.text, is.message.c_str(), nullptr, wrap);
                if (hov && !is.comps.empty()) {
                    std::vector<std::string> rv;
                    for (size_t q = 0; q < is.comps.size() && rv.size() < 8; ++q) {
                        int ci = is.comps[q];
                        if (ci >= 0 && ci < (int)d.prj.comps.size()) rv.push_back(d.prj.comps[ci].ref);
                    }
                    tooltip(("Click to select and fly to: " + ref_list_capped(rv, is.comps.size())).c_str());
                }
                if (clicked) { issue_cursor = (int)k; if (!is.comps.empty()) focus_components(doc, is.comps); }
                ImGui::PopID();
                row++;
            }
        }
        ImGui::PopID();
        i = j;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// badges / side toggle / selection sets / components tab
// ---------------------------------------------------------------------------
void PlotterView::draw_badge(ImDrawList* dl, ImVec2& p, const char* txt, ImU32 col, bool rtl) {
    const float fs = ImGui::GetFontSize() * 0.78f;
    ImVec2 ts = g_font_bold->CalcTextSizeA(fs, FLT_MAX, 0, txt);
    float w = ts.x + S(8), h = fs + S(4);
    ImVec2 a = rtl ? ImVec2(p.x - w, p.y) : p;
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), alpha(col, 0.22f), S(4));
    dl->AddRect(a, ImVec2(a.x + w, a.y + h), alpha(col, 0.75f), S(4));
    dl->AddText(g_font_bold, fs, ImVec2(a.x + S(4), a.y + S(2)), col, txt);
    p.x = rtl ? a.x - S(3) : a.x + w + S(3);
}

void PlotterView::draw_side_toggle() {
    // [View from: Top | Bottom]  [Normal | Ghost | X-ray]  [lock]   (LEAD_BRIEF 10)
    const GTheme& t = th();
    Doc& d = *doc;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = S(30);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float x = p0.x;
    struct Seg { ui::Icon ic; bool has_ic; const char* lbl; const char* tip; int cmd; bool on; };
    auto group = [&](const char* id, std::initializer_list<Seg> segs) {
        float total = 0;
        for (auto& sg : segs) total += ImGui::CalcTextSize(sg.lbl).x + (sg.has_ic ? S(36) : S(18));
        dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + total, p0.y + h), t.panel2, S(8));
        dl->AddRect(ImVec2(x, p0.y), ImVec2(x + total, p0.y + h), t.border, S(8));
        int k = 0;
        for (auto& sg : segs) {
            float w = ImGui::CalcTextSize(sg.lbl).x + (sg.has_ic ? S(36) : S(18));
            ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
            ImGui::PushID(id); ImGui::PushID(k++);
            bool clicked = ImGui::InvisibleButton("sm", ImVec2(w, h));
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID(); ImGui::PopID();
            if (sg.on) dl->AddRectFilled(ImVec2(x + S(2), p0.y + S(2)), ImVec2(x + w - S(2), p0.y + h - S(2)), t.accent, S(6));
            else if (hov) dl->AddRectFilled(ImVec2(x + S(2), p0.y + S(2)), ImVec2(x + w - S(2), p0.y + h - S(2)), t.raised, S(6));
            float tx = x + S(9);
            if (sg.has_ic) { icon_draw(dl, sg.ic, ImVec2(x + S(7), p0.y + (h - S(18)) * 0.5f), S(18), 1.f, sg.on); tx = x + S(28); }
            dl->AddText(ImVec2(tx, p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f), sg.on ? t.accent_text : t.text, sg.lbl);
            if (hov) tooltip(sg.tip);
            if (clicked) post_command(sg.cmd);
            x += w;
        }
        x += S(6);
    };
    const bool vb = d.view_from_bottom;
    group("view", {
        {ui::Icon::ViewTop, true, "Top", "View from TOP  (T)\nThe top side faces you and is the active side.", ID_VIEW_SIDE_TOP, !vb},
        {ui::Icon::ViewBottom, true, "Bottom", "View from BOTTOM  (B)\nThe board is turned over (mirrored) so bottom silkscreen,\nmarkings and pads read as on the real board. Bottom becomes the active side.", ID_VIEW_SIDE_BOTTOM, vb}});
    // R17c (REVIEW_V16 §10, specs/VIEW_MODES.md): two independent toggles replace Normal / Ghost / X-ray + lock.
    //   eye  = VISIBILITY of the other side (hidden, or drawn see-through behind the active side)
    //   lock = SELECTABILITY of the other side (locked by default; unlocking also shows it)
    auto tile = [&](const char* id, bool on, ImU32 on_col, const char* tip, int cmd, auto&& draw_glyph) {
        const float w = h;
        ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
        bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
        bool hov = ImGui::IsItemHovered();
        dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + w, p0.y + h), on ? alpha(on_col, 0.25f) : (hov ? t.raised : t.panel2), S(8));
        dl->AddRect(ImVec2(x, p0.y), ImVec2(x + w, p0.y + h), on ? on_col : t.border, S(8), 0, on ? S(1.5f) : 1.f);
        draw_glyph(x, w, on ? on_col : t.text);
        if (hov) tooltip(tip);
        if (clicked) post_command(cmd);
        x += w + S(2);
    };
    const bool far_shown = d.far_drawn();
    const bool open = d.back_selectable && d.side_display == Doc::SideDisplay::Xray;
    tile("##farvis", far_shown, t.accent,
         far_shown ? "Other side: SHOWN see-through  (X / G toggles)\nThe side facing away is drawn transparent behind the active side.\nIt is NOT selectable unless the lock next to this button is opened.\nClick to hide it."
                   : "Other side: HIDDEN  (X / G toggles)\nOnly the active side is drawn.\nClick to draw the side facing away see-through (still not selectable).",
         far_shown ? ID_VIEW_SIDE_NORMAL : ID_VIEW_SIDE_XRAY,
         [&](float x0, float w, ImU32) { icon_draw(dl, ui::Icon::OtherSide, ImVec2(x0 + (w - S(18)) * 0.5f, p0.y + (h - S(18)) * 0.5f), S(18), far_shown ? 1.f : 0.6f, false); });
    tile("##backsel", open, t.warn,
         open ? "Other side: SELECTABLE  (K)\nClicks and box selection also pick parts on the side facing away.\nClick to lock: only the active side can be selected or edited."
              : "Other side: LOCKED  (K)\nOnly the active side can be selected or edited (default).\nClick to unlock: the other side becomes selectable (and is shown).",
         ID_VIEW_BACK_SELECT,
         [&](float x0, float w, ImU32 lc) {
             const float cx = x0 + w * 0.5f, by = p0.y + S(14);
             dl->AddRectFilled(ImVec2(cx - S(6.5f), by), ImVec2(cx + S(6.5f), by + S(10)), lc, S(2));
             const float sx0 = cx - S(4), sx1 = cx + S(4), top = by - S(7);
             dl->PathArcTo(ImVec2(cx, top + S(4)), S(4), 3.14159f, 2 * 3.14159f, 10);
             dl->PathStroke(lc, 0, S(1.8f));
             dl->AddLine(ImVec2(sx0, top + S(4)), ImVec2(sx0, by), lc, S(1.8f));
             dl->AddLine(ImVec2(sx1, top + S(4)), ImVec2(sx1, open ? top + S(6) : by), lc, S(1.8f));
         });
    ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
    ImGui::Dummy(ImVec2(0, h));
    ImGui::SameLine(0, S(4));
}

// R17c (REVIEW_V16 §8): [● Top SMT ▾ · 12 unassigned] [Assign] [Unassign] [Highlight unassigned]
static std::string active_layer_label(const Doc& d) {
    const int cur = current_point(d);
    if (cur < 0 || cur >= (int)d.prj.model.points.size()) return "No export layer";
    const std::string& n = d.prj.model.points[cur].name;
    return n.empty() ? "Layer " + std::to_string(cur + 1) : n;
}
static int unassigned_count(const Doc& d) {
    int n = 0;
    for (auto& c : d.prj.comps) n += c.feature == Feature::Component && c.assigned_point < 0 && !c.dnp;
    return n;
}
float PlotterView::assign_group_width() {
    const std::string l = active_layer_label(*doc) + "  \xC2\xB7  " + std::to_string(unassigned_count(*doc)) + " unassigned";
    return ImGui::CalcTextSize(l.c_str()).x + S(46) + S(32) * 3 + S(6);
}
void PlotterView::draw_assign_group() {
    const GTheme& t = th();
    Doc& d = *doc;
    const int cur = current_point(d);
    const bool have = cur >= 0 && cur < (int)d.prj.model.points.size();
    const int nun = unassigned_count(d);
    const std::string name = active_layer_label(d);
    const std::string cnt = std::to_string(nun) + " unassigned";
    // chip: colour dot + active export layer name (click: choose the active layer) + unassigned count
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = S(30);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float wn = ImGui::CalcTextSize(name.c_str()).x, wc = ImGui::CalcTextSize(cnt.c_str()).x;
        const float w = S(24) + wn + S(14) + wc + S(18);
        const bool clicked = ImGui::InvisibleButton("##actlayer", ImVec2(w, h));
        const bool hov = ImGui::IsItemHovered();
        const ImU32 lc = have ? argb2im(d.prj.model.points[cur].color | 0xFF000000u) : t.text_dim;
        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), hov ? t.raised : t.panel2, S(15));
        dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), have ? lc : t.border, S(15), 0, have ? S(1.5f) : 1.f);
        dl->AddCircleFilled(ImVec2(p0.x + S(13), p0.y + h * 0.5f), S(5), lc);
        const float ty = p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImVec2(p0.x + S(24), ty), t.text, name.c_str());
        ImGui::PopFont();
        dl->AddText(ImVec2(p0.x + S(24) + wn + S(4), ty), t.text_dim, "\xE2\x96\xBE");
        dl->AddText(ImVec2(p0.x + S(24) + wn + S(18), ty), nun ? t.warn : t.text_dim, cnt.c_str());
        if (hov) tooltip(have ? "Active export layer: Assign puts the selected placements here (one machine program per layer).\n"
                                "Click to choose another layer. While assignment is partial, unassigned placements are marked with an amber ring (Highlight unassigned toggle)."
                              : "No export layer yet: click to create one (Assign needs an active export layer).");
        if (clicked) ImGui::OpenPopup("##actlayerpop");
        if (ImGui::BeginPopup("##actlayerpop")) {
            section_header("ACTIVE EXPORT LAYER");
            auto& pts = d.prj.model.points;
            for (int i = 0; i < (int)pts.size(); ++i) {
                ImGui::PushID(i);
                const std::string l = pts[i].name.empty() ? "Layer " + std::to_string(i + 1) : pts[i].name;
                if (ImGui::MenuItem(l.c_str(), nullptr, i == cur)) set_current_point(d, i);
                ImGui::PopID();
            }
            if (!pts.empty()) ImGui::Separator();
            if (menu_item_icon("New Export Layer\xE2\x80\xA6", ui::Icon::Plus)) post_command(ID_POINT_ADD);
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, S(4));
    }
    const int nsel = d.selected_count();
    const std::string at = "Assign to " + name + "  (A)\nPut the " + (nsel ? std::to_string(nsel) + " selected placement(s)" : std::string("selected placements")) +
                           " on the visible side(s) into the active export layer.\nOther layers keep their members. One undo step.";
    if (icon_button("##assign", ui::Icon::Assign, at.c_str(), false, (command_state(ID_TOOL_ASSIGN) & 1) != 0, S(30))) post_command(ID_TOOL_ASSIGN);
    ImGui::SameLine(0, S(2));
    if (icon_button("##unassign", ui::Icon::Unassign, "Unassign  (Shift+A / U)\nRemove the selected placements from their export layer (they stay in the project).",
                    false, (command_state(ID_TOOL_UNASSIGN) & 1) != 0, S(30))) post_command(ID_TOOL_UNASSIGN);
    ImGui::SameLine(0, S(2));
    if (icon_button("##hlun", ui::Icon::ShowUnassigned, d.highlight_unassigned ? "Highlight unassigned: ON  (click to turn off)\nWhile some placements are assigned and some are not, the unassigned ones are drawn red with an amber ring.\nNothing is marked while none (or all) are assigned."
                                                  : "Highlight unassigned: OFF\nClick to mark unassigned placements (red + amber ring) while assignment is partial",
                    d.highlight_unassigned, true, S(30))) post_command(ID_TOOL_SHOW_UNASSIGNED);
    ImGui::SameLine(0, S(2));
}

// R17c (REVIEW_V16 §9): compact floating viewport toolbar, stacked vertically in the canvas' lower-left corner.
// It lives inside the canvas rect, so it moves with the canvas edge when the side panel opens or is resized.
void PlotterView::draw_viewport_tools(ImVec2 c0, ImVec2 c1) {
    if (c1.x - c0.x < S(120) || c1.y - c0.y < S(240)) return;
    const GTheme& t = th();
    const float b = S(28), pad = S(4);
    const int n = 6;
    const ImVec2 sz(b + pad * 2, b * n + S(2) * (n - 1) + pad * 2 + S(12));
    const ImVec2 keep = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(c0.x + S(10), c1.y - sz.y - S(10)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col_alpha(t.panel, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Border, t.border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(2)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(8));
    ImGui::BeginChild("##viewtools", sz, ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {   // R24a (user v23 #3): Select / arrow -- lit while clicks go to the selection tool; one click leaves Pan / Zoom box
        const bool sel_mode = !canvas.pan_mode() && !canvas.zoom_rect_mode() && canvas.tool == Tool::Select;
        if (icon_button("##vsel", ui::Icon::Select, sel_mode ? "Select: ON  (click / drag box selects placements)"
                                                             : "Select  (Esc)\nLeave Pan / Zoom rectangle and select placements again", sel_mode, true, b)) {
            canvas.leave_nav_modes();
            canvas.tool = Tool::Select;
        }
        const ImVec2 q = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(q.x + S(3), q.y + S(2)), ImVec2(q.x + b - S(3), q.y + S(2)), t.border);
        ImGui::Dummy(ImVec2(b, S(4)));
    }
    if (icon_button("##vzi", ui::Icon::ZoomIn, "Zoom in  (+ / mouse wheel)", false, true, b)) canvas.zoom_by(1.3);
    if (icon_button("##vzo", ui::Icon::ZoomOut, "Zoom out  (- / mouse wheel)", false, true, b)) canvas.zoom_by(1 / 1.3);
    if (icon_button("##vzr", ui::Icon::ZoomRect, "Zoom rectangle: drag a box to zoom into", canvas.zoom_rect_mode(), true, b)) { canvas.set_pan_mode(false); canvas.begin_zoom_rect(); }
    if (icon_button("##vpan", ui::Icon::Pan, canvas.pan_mode() ? "Pan: ON  (H / Esc to leave)\nLeft-drag moves the view; tools get no clicks.\nMiddle-drag or Space+drag pans with any tool."
                                                             : "Pan  (H)\nLeft-drag moves the view. Middle-drag or Space+drag pans with any tool.", canvas.pan_mode(), true, b))
        canvas.set_pan_mode(!canvas.pan_mode());
    {   // separator
        const ImVec2 q = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(q.x + S(3), q.y + S(2)), ImVec2(q.x + b - S(3), q.y + S(2)), t.border);
        ImGui::Dummy(ImVec2(b, S(4)));
    }
    if (icon_button("##vfit", ui::Icon::ZoomAll, "Fit: zoom to the whole board  (Home / Z / F)", false, true, b)) canvas.zoom_all();
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::SetCursorScreenPos(keep);
}

void PlotterView::draw_selset_combo() {
    Doc& d = *doc;
    auto& v = d.prj.selection_sets;
    // [save] [sets v]
    if (icon_button("selsave", ui::Icon::Select, "Save the current selection as a named selection set\n(stored in the project, like 3ds Max Named Selection Sets)",
                    false, d.selected_count() > 0))
        selset_save(doc);
    ImGui::SameLine(0, S(2));
    std::string prev = v.empty() ? std::string("Selection sets") : "Sets (" + std::to_string(v.size()) + ")";
    ImGui::SetNextItemWidth(S(150));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), (S(30) - ImGui::GetTextLineHeight()) * 0.5f));
    if (ImGui::BeginCombo("##selsets", prev.c_str(), ImGuiComboFlags_HeightLarge)) {
        ImGui::PopStyleVar();
        const bool shift = ImGui::GetIO().KeyShift, ctrl = ImGui::GetIO().KeyCtrl;
        ImGui::TextDisabled("Click: select  \xC2\xB7  Shift: add  \xC2\xB7  Ctrl: subtract");
        ImGui::Separator();
        if (v.empty()) ImGui::TextDisabled("No sets. Select parts, then press the save button.");
        for (int i = 0; i < (int)v.size(); ++i) {
            ImGui::PushID(i);
            std::string l = v[i].name + "##s";
            if (ImGui::Selectable(l.c_str(), false, ImGuiSelectableFlags_DontClosePopups | ImGuiSelectableFlags_AllowOverlap, ImVec2(ImGui::GetContentRegionAvail().x - S(60), 0)))
                selset_apply(d, i, ctrl ? 2 : shift ? 1 : 0);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(56));
            ImGui::TextDisabled("%d", (int)v[i].refs.size());
            ImGui::PopID();
        }
        ImGui::Separator();
        if (begin_menu_icon("Manage", ui::Icon::Settings)) { selset_menu_items(doc); ImGui::EndMenu(); }
        ImGui::EndCombo();
    } else ImGui::PopStyleVar();
    if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen("##selsets")) tooltip("Named selection sets: recall (replace / add / subtract), rename, delete");
    ImGui::SameLine(0, S(2));
}

void PlotterView::draw_comps() {
    const GTheme& t = th();
    Doc& d = *doc;
    ensure_review();
    // --- controls ---
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##cq", "Filter: ref, P/N, package...", comp_q, sizeof comp_q);
    ImGui::SetNextItemWidth(S(120));
    ImGui::Combo("##cg", &comp_group, "By part number\0By package\0Flat list\0");
    ImGui::SameLine();
    ImGui::Checkbox("Flagged only", &comp_flagged);
    // code chips (only codes that occur)
    {
        uint64_t present = 0;
        for (auto m : comp_codes) present |= m;
        if (present) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float maxx = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            ImVec2 p = ImGui::GetCursorScreenPos();
            float x0 = p.x, rowh = ImGui::GetFontSize() * 0.78f + S(4);
            for (int c = 0; c < 64; ++c) {
                if (!(present & (1ull << c))) continue;
                const char* sc = short_code((CheckCode)c);
                float w = g_font_bold->CalcTextSizeA(ImGui::GetFontSize() * 0.78f, FLT_MAX, 0, sc).x + S(8);
                if (p.x + w > maxx) { p.x = x0; p.y += rowh + S(3); }
                bool on = (comp_filter >> c) & 1;
                ImGui::SetCursorScreenPos(p);
                ImGui::PushID(c + 900);
                if (ImGui::InvisibleButton("cf", ImVec2(w, rowh))) comp_filter ^= 1ull << c;
                bool hov = ImGui::IsItemHovered();
                ImGui::PopID();
                if (hov) tooltip((std::string(check_code_name((CheckCode)c)) + "\nClick to filter by this code (combine several)").c_str());
                ImVec2 q = p;
                draw_badge(dl, q, sc, on ? t.accent : (hov ? t.text : t.text_dim), false);
                p.x = q.x;
            }
            ImGui::SetCursorScreenPos(ImVec2(x0, p.y + rowh + S(4)));
            ImGui::Dummy(ImVec2(0, 0));
        }
    }
    // --- rebuild groups when needed ---
    uint64_t key = d.gen * 1315423911ull ^ ((uint64_t)comp_group << 1) ^ ((uint64_t)comp_flagged << 3) ^ (comp_filter * 2654435761ull) ^ review_key;
    if (key != comp_key || comp_last_q != comp_q) {
        comp_key = key; comp_last_q = comp_q;
        Matcher m(comp_q);
        std::map<std::string, std::vector<int>> g;
        for (int i = 0; i < (int)d.prj.comps.size(); ++i) {
            const Component& c = d.prj.comps[i];
            uint64_t cc = i < (int)comp_codes.size() ? comp_codes[i] : 0;
            if (comp_flagged && !cc) continue;
            if (comp_filter && !(cc & comp_filter)) continue;
            if (!m.empty() && !m.match(d.prj, c)) continue;
            std::string k = comp_group == 0 ? (c.number.empty() ? std::string("(no part number)") : c.number)
                          : comp_group == 1 ? (c.package.empty() ? std::string("(no package)") : c.package) : std::string();
            g[k].push_back(i);
        }
        comp_groups.clear();
        for (auto& kv : g) {
            auto v = kv.second;
            std::sort(v.begin(), v.end(), [&](int a, int b) { return ref_less(d.prj.comps[a].ref, d.prj.comps[b].ref); });
            comp_groups.push_back({kv.first, std::move(v)});
        }
    }
    int total = 0, flagged = 0;
    for (auto& g : comp_groups) for (int i : g.second) { total++; if (i < (int)comp_codes.size() && comp_codes[i]) flagged++; }
    ImGui::TextDisabled("%d parts  \xC2\xB7  %d flagged  \xC2\xB7  %d groups", total, flagged, comp_group == 2 ? 1 : (int)comp_groups.size());

    // --- list ---
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel2);
    ImGui::BeginChild("##complist", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rh = ImGui::GetTextLineHeight() * 2 + S(8);
    auto row = [&](int i) {
        Component& c = d.prj.comps[i];
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        bool clicked = ImGui::InvisibleButton("r", ImVec2(w, rh));
        bool hov = ImGui::IsItemHovered();
        if (c.selected) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), t.sel, S(5));
        else if (hov || comp_cursor == i) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), t.raised, S(5));
        // point chip
        bool has_pt = c.assigned_point >= 0 && c.assigned_point < (int)d.prj.model.points.size();
        uint32_t pc = c.feature == Feature::Fiducial ? ui::pal().fiducial : point_color(d, c.assigned_point);
        dl->AddRectFilled(ImVec2(p.x + S(4), p.y + S(5)), ImVec2(p.x + S(8), p.y + rh - S(5)), has_pt || c.feature == Feature::Fiducial ? argb2im(pc) : alpha(t.text_faint, 0.4f), S(2));
        ImU32 tc = c.selected ? t.sel_text : t.text;
        float tx = p.x + S(14);
        ImGui::PushFont(g_font_bold);
        const char* rs = c.ref.empty() ? "(no ref)" : c.ref.c_str();
        dl->AddText(ImVec2(tx, p.y + S(3)), tc, rs);
        float rw = ImGui::CalcTextSize(rs).x;
        ImGui::PopFont();
        std::string meta = std::string(c.side == Side::Top ? "T" : "B") + (c.dnp ? " \xC2\xB7 DNP" : "") + (c.feature == Feature::Fiducial ? " \xC2\xB7 FID" : "");
        if (has_pt) meta += "  \xC2\xB7  " + d.prj.model.points[c.assigned_point].name;
        else if (c.feature == Feature::Component) meta += "  \xC2\xB7  unassigned";
        // badges right-aligned on line 1
        ImVec2 bp(p.x + w - S(4), p.y + S(3));
        uint64_t cc = i < (int)comp_codes.size() ? comp_codes[i] : 0;
        ImU32 bc = i < (int)comp_sev.size() ? (comp_sev[i] == 3 ? t.error : comp_sev[i] == 2 ? t.warn : t.accent) : t.accent;
        int nb = 0;
        for (int k = 63; k >= 0; --k) if (cc & (1ull << k)) { if (nb++ < 4) draw_badge(dl, bp, short_code((CheckCode)k), bc, true); }
        dl->PushClipRect(ImVec2(tx + rw, p.y), ImVec2(bp.x, p.y + rh), true);
        dl->AddText(ImVec2(tx + rw + S(8), p.y + S(3)), t.text_faint, meta.c_str());
        dl->PopClipRect();
        std::string l2 = (c.number.empty() ? std::string("\xE2\x80\x94") : c.number) + "   " + (c.package.empty() ? std::string("\xE2\x80\x94") : c.package);
        dl->PushClipRect(ImVec2(tx, p.y), ImVec2(p.x + w - S(4), p.y + rh), true);
        dl->AddText(ImVec2(tx, p.y + S(5) + ImGui::GetTextLineHeight()), c.selected ? t.sel_text : t.text_dim, l2.c_str());
        dl->PopClipRect();
        if (hov && cc) {
            std::string tip;
            for (auto& is : issues) if (std::find(is.comps.begin(), is.comps.end(), i) != is.comps.end()) {
                tip += std::string(short_code(is.code)) + "  " + is.message + "\n";
                if (tip.size() > 1200) { tip += "..."; break; }
            }
            tooltip(tip.c_str());
        }
        if (clicked) {
            comp_cursor = i;
            ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl) { c.selected = !c.selected; d.changed(ui::CH_SELECTION); }
            else if (io.KeyShift) { c.selected = true; d.changed(ui::CH_SELECTION); fly_to_comps({i}); }
            else focus_components(doc, {i});
        }
        if (hov && ImGui::IsMouseDoubleClicked(0)) post_command(ID_CTX_EDIT_LOCATION);
        ImGui::PopID();
    };
    if (comp_group == 2) {
        const auto& v = comp_groups.empty() ? std::vector<int>() : comp_groups[0].second;
        ImGuiListClipper clip;
        clip.Begin((int)v.size(), rh + ImGui::GetStyle().ItemSpacing.y);
        while (clip.Step()) for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) row(v[r]);
    } else {
        for (size_t gi = 0; gi < comp_groups.size(); ++gi) {
            auto& g = comp_groups[gi];
            int nf = 0, nsel = 0;
            for (int i : g.second) { if (i < (int)comp_codes.size() && comp_codes[i]) nf++; if (d.prj.comps[i].selected) nsel++; }
            ImGui::PushID((int)gi);
            ImVec2 hp = ImGui::GetCursorScreenPos();
            float hw = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemAllowOverlap();
            bool open = ImGui::TreeNodeEx("g", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding, "%s", g.first.c_str());
            float fh = ImGui::GetFrameHeight();
            // count + flag count + select-group button
            std::string cnt = std::to_string(g.second.size()) + (nsel ? " (" + std::to_string(nsel) + " sel)" : "");
            ImVec2 cs = ImGui::CalcTextSize(cnt.c_str());
            float bx = hp.x + hw - S(26);
            ImGui::SetCursorScreenPos(ImVec2(bx, hp.y + (fh - S(22)) * 0.5f));
            if (icon_button("sg", ui::Icon::Select, "Select every part in this group\nShift: add to the selection", false, true, S(22))) {
                std::vector<int> idx = g.second;
                if (!ImGui::GetIO().KeyShift) d.select_none();
                for (int i : idx) d.prj.comps[i].selected = true;
                d.changed(ui::CH_SELECTION);
                fly_to_comps(idx);
                gui::status(std::to_string(idx.size()) + " parts of " + g.first + " selected");
            }
            dl->AddText(ImVec2(bx - cs.x - S(6), hp.y + (fh - cs.y) * 0.5f), t.text_dim, cnt.c_str());
            if (nf) {
                std::string f = "!" + std::to_string(nf);
                ImVec2 bp(bx - cs.x - S(12), hp.y + (fh - (ImGui::GetFontSize() * 0.78f + S(4))) * 0.5f);
                draw_badge(dl, bp, f.c_str(), t.warn, true);
            }
            ImGui::SetCursorScreenPos(ImVec2(hp.x, hp.y + fh + ImGui::GetStyle().ItemSpacing.y));
            if (open) {
                ImGui::Unindent(ImGui::GetStyle().IndentSpacing * 0.6f);
                for (int i : g.second) row(i);
                ImGui::Indent(ImGui::GetStyle().IndentSpacing * 0.6f);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    if (comp_groups.empty()) ImGui::TextDisabled(comp_flagged || comp_filter ? "No flagged parts match." : "No parts.");
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void PlotterView::draw_info() {
    const GTheme& t = th();
    Doc& d = *doc;
    int idx = -1, nsel = 0;
    for (int i = 0; i < (int)d.prj.comps.size(); ++i) if (d.prj.comps[i].selected) { if (idx < 0) idx = i; nsel++; }
    if (idx < 0) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
        ImGui::TextWrapped("Select a component (or use the Component Info tool and click one) to see and edit its details here.");
        ImGui::PopStyleColor();
        return;
    }
    Component& c = d.prj.comps[idx];
    const Vec2 rel = d.prj.user_pos(c);
    if (idx != info_comp || info_gen != d.gen) {
        info_comp = idx; info_gen = d.gen;
        snprintf(e_ref, sizeof e_ref, "%s", c.ref.c_str());
        snprintf(e_pn, sizeof e_pn, "%s", c.number.c_str());
        snprintf(e_pkg, sizeof e_pkg, "%s", c.package.c_str());
        snprintf(e_x, sizeof e_x, "%s", fmt_len_u8(rel.x, false).c_str());
        snprintf(e_y, sizeof e_y, "%s", fmt_len_u8(rel.y, false).c_str());
        snprintf(e_rot, sizeof e_rot, "%s", ui::U(ui::fmt_num(c.rot, 2)).c_str());
    }
    // header card
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = S(58);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), t.panel2, S(8));
        uint32_t pc = c.feature == Feature::Fiducial ? ui::pal().fiducial : point_color(d, c.assigned_point);
        dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + S(5), p.y + h), argb2im(pc), S(8), ImDrawFlags_RoundCornersLeft);
        ImGui::PushFont(g_font_big);
        float bs = ImGui::GetFontSize();
        ImGui::PopFont();
        float fs = std::min(bs, S(24));
        dl->AddText(g_font_bold, fs, ImVec2(p.x + S(14), p.y + S(6)), t.text, c.ref.empty() ? "(no reference)" : c.ref.c_str());
        std::string sub = std::string(c.side == Side::Top ? "Top side" : "Bottom side");
        if (c.assigned_point >= 0 && c.assigned_point < (int)d.prj.model.points.size()) sub += "  \xC2\xB7  " + d.prj.model.points[c.assigned_point].name;
        else if (c.feature == Feature::Component) sub += "  \xC2\xB7  unassigned";
        if (c.dnp) sub += "  \xC2\xB7  DNP";
        if (c.feature == Feature::Fiducial) sub += "  \xC2\xB7  Fiducial";
        dl->AddText(ImVec2(p.x + S(14), p.y + S(10) + fs), t.text_dim, sub.c_str());
        if (nsel > 1) {
            std::string m = "+" + std::to_string(nsel - 1) + " more";
            ImVec2 ms = ImGui::CalcTextSize(m.c_str());
            dl->AddText(ImVec2(p.x + w - ms.x - S(10), p.y + S(8)), t.text_faint, m.c_str());
        }
        ImGui::Dummy(ImVec2(w, h + S(6)));
    }
    auto commit = [&](const char* label) { d.checkpoint(label); };
    const float lw = S(84);
    auto field = [&](const char* lbl, const char* id, char* buf, size_t n) -> bool {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", lbl);
        ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText(id, buf, n);
        return ImGui::IsItemDeactivatedAfterEdit();
    };
    section_header("LOCATION");
    if (field("Reference", "##ref", e_ref, sizeof e_ref) && c.ref != e_ref) { commit("Edit reference"); c.ref = e_ref; d.changed(ui::CH_GEOMETRY); }
    if (field("Part number", "##pn", e_pn, sizeof e_pn) && c.number != e_pn) { commit("Edit part number"); c.number = e_pn; d.changed(ui::CH_GEOMETRY); }
    if (const PartNumber* pn = d.prj.part_of(c)) if (!pn->description.empty()) { ImGui::SetCursorPosX(lw); ImGui::TextDisabled("%s", pn->description.c_str()); }
    if (field("Package", "##pkg", e_pkg, sizeof e_pkg) && c.package != e_pkg) { commit("Edit package"); c.package = e_pkg; d.changed(ui::CH_GEOMETRY); }
    if (const Package* pk = d.prj.package_of(c)) {
        ImGui::SetCursorPosX(lw);
        ImGui::TextDisabled("%d pads \xC2\xB7 %s%s", (int)pk->pads.size(), pk->smt ? "SMT" : "thru-hole", pkg_has_polarity(*pk) ? " \xC2\xB7 polarity marker" : "");
    } else if (!c.package.empty()) { ImGui::SetCursorPosX(lw); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.warn), "not in package library"); }
    std::string ul = " " + ui::U(units_label());
    if (field(("X" + ul).c_str(), "##x", e_x, sizeof e_x)) {
        double v = parse_len(ui::W(e_x), rel.x);
        if (std::fabs(v - rel.x) > 1e-9) { commit("Edit location"); c.pos.x = d.prj.from_user({v, rel.y}).x; d.changed(ui::CH_GEOMETRY); }
    }
    if (field(("Y" + ul).c_str(), "##y", e_y, sizeof e_y)) {
        double v = parse_len(ui::W(e_y), rel.y);
        if (std::fabs(v - rel.y) > 1e-9) { commit("Edit location"); c.pos.y = d.prj.from_user({rel.x, v}).y; d.changed(ui::CH_GEOMETRY); }
    }
    if (field("Rotation \xC2\xB0", "##rot", e_rot, sizeof e_rot)) {
        double v = atof(e_rot);
        if (std::fabs(v - c.rot) > 1e-9) { commit("Edit rotation"); double dr = v - c.rot; c.rot = norm_deg(v); c.label_off = rotate(c.label_off, dr); d.changed(ui::CH_GEOMETRY); }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Side");
    ImGui::SameLine(lw);
    int side = (int)c.side;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##side", &side, "Top\0Bottom\0") && side != (int)c.side) { commit("Flip side"); c.side = (Side)side; d.changed(ui::CH_GEOMETRY); }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Model point");
    ImGui::SameLine(lw);
    ImGui::SetNextItemWidth(-1);
    std::string cur = c.assigned_point >= 0 && c.assigned_point < (int)d.prj.model.points.size() ? d.prj.model.points[c.assigned_point].name : "(unassigned)";
    if (ImGui::BeginCombo("##pt", cur.c_str())) {
        if (ImGui::Selectable("(unassigned)", c.assigned_point < 0)) { commit("Unassign"); c.assigned_point = -1; d.changed(ui::CH_ASSIGN); }
        for (int i = 0; i < (int)d.prj.model.points.size(); ++i) {
            ImGui::PushID(i);
            ImVec2 q = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable(("     " + d.prj.model.points[i].name).c_str(), c.assigned_point == i)) { commit("Assign"); c.assigned_point = i; d.changed(ui::CH_ASSIGN); }
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(q.x + S(7), q.y + ImGui::GetTextLineHeight() * 0.5f), S(4.5f), argb2im(d.prj.model.points[i].color));
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    bool dnp = c.dnp, fid = c.feature == Feature::Fiducial;
    ImGui::SetCursorPosX(lw);
    if (ImGui::Checkbox("Do not place", &dnp)) { commit("Toggle DNP"); c.dnp = dnp; d.changed(ui::CH_GEOMETRY); }
    ImGui::SameLine();
    if (ImGui::Checkbox("Fiducial", &fid)) {
        if (fid) post_command(ID_CTX_MAKE_FIDUCIAL);
        else { commit("Fiducial to component"); c.feature = Feature::Component; d.changed(ui::CH_GEOMETRY); }
    }
    if (c.feature == Feature::Fiducial) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Fiducial type");
        ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(-1);
        int sc = c.fid_scope == FiducialScope::Global ? 0 : c.fid_scope == FiducialScope::Image ? 1 : 2;
        if (ImGui::Combo("##fscope", &sc, "Board fiducial (once)\0Circuit fiducial (every circuit)\0Component fiducial\0")) {
            FiducialScope ns = sc == 0 ? FiducialScope::Global : sc == 1 ? FiducialScope::Image : FiducialScope::Local;
            if (ns != c.fid_scope) { commit("Fiducial scope"); c.fid_scope = ns; d.changed(ui::CH_GEOMETRY); }
        }
        if (ImGui::IsItemHovered()) tooltip("Board fiducial: aligns the whole PCB / panel, exported once.\nCircuit fiducial: repeated on every circuit of a panel, each circuit aligned on its own.\nComponent fiducial: local mark for one component (BGA, fine pitch).");
        if (c.fid_scope == FiducialScope::Local) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Owner");
            ImGui::SameLine(lw);
            ImGui::SetNextItemWidth(-1);
            std::string own = "(none)";
            for (auto& o : d.prj.comps) if ((int)o.id == c.local_fid_owner && !o.ref.empty()) own = o.ref;
            if (ImGui::BeginCombo("##fowner", own.c_str(), ImGuiComboFlags_HeightLarge)) {
                static char fq[64] = "";
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##fq", "filter", fq, sizeof fq);
                std::string ql = lower(fq);
                if (ImGui::Selectable("(none)", c.local_fid_owner == 0)) { commit("Fiducial owner"); c.local_fid_owner = 0; d.changed(ui::CH_GEOMETRY); }
                for (auto& o : d.prj.comps) {
                    if (o.feature != Feature::Component || o.ref.empty()) continue;
                    if (!ql.empty() && lower(o.ref).find(ql) == std::string::npos) continue;
                    ImGui::PushID((int)o.id);
                    if (ImGui::Selectable(o.ref.c_str(), (int)o.id == c.local_fid_owner)) { commit("Fiducial owner"); c.local_fid_owner = (int)o.id; d.changed(ui::CH_GEOMETRY); }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        }
    }
    ImGui::Spacing();
    if (d.prj.virtual_origin.x != 0 || d.prj.virtual_origin.y != 0) ImGui::TextDisabled("X/Y relative to the virtual origin (at %s, %s)", fmt_len_u8(d.prj.virtual_origin.x, false).c_str(), fmt_len_u8(d.prj.virtual_origin.y, false).c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    float bw = (ImGui::GetContentRegionAvail().x - S(6)) * 0.5f;
    if (button("Edit Placement...", ui::Icon::Locations, false, ImVec2(bw, 0))) post_command(ID_CTX_EDIT_LOCATION);
    ImGui::SameLine(0, S(6));
    if (button("Edit P/N...", ui::Icon::Bom, false, ImVec2(bw, 0))) post_command(ID_CTX_EDIT_PN);
    if (button("Go to Package", ui::Icon::PackageEditor, false, ImVec2(bw, 0))) post_command(ID_CTX_EDIT_PACKAGE);
    ImGui::SameLine(0, S(6));
    if (button("Zoom to", ui::Icon::ZoomIn, false, ImVec2(bw, 0))) fly_to_comps({idx});
}

void PlotterView::draw_context_menu() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
    if (ImGui::BeginPopup("##plotctx")) {
        int n = doc->selected_count();
        if (n) {
            std::string h = n == 1 ? std::string() : std::to_string(n) + " components";
            if (n == 1) for (auto& c : doc->prj.comps) if (c.selected) { h = c.ref.empty() ? "(no reference)" : c.ref; break; }
            ImGui::TextDisabled("%s", h.c_str());
            ImGui::Separator();
        }
        // R16c (REVIEW_V15 §10): placement-first order; Edit P/N lives in the BOM / Data views now
        std::set<std::string> pk;
        for (auto& c : doc->prj.comps) if (c.selected && !c.package.empty()) pk.insert(c.package);
        menu_item(ID_CTX_EDIT_LOCATION);   // Edit Placement...
        {
            std::string lab = pk.size() == 1 ? "Go to Package (" + *pk.begin() + ")" : std::string("Go to Package");
            if (menu_item_icon(lab.c_str(), ui::Icon::PackageEditor, nullptr, false, n > 0)) post_command(ID_CTX_EDIT_PACKAGE);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open this placement's package in the package editor (graphics, pads, pick orientation)");
        }
        menu_item(ID_BOARD_INSERT_FREE);   // Add Placement...
        ImGui::Separator();
        if (begin_menu_icon("Rotate", ui::Icon::Rot90, n > 0)) {   // one submenu instead of three items
            if (menu_item_icon("90\xC2\xB0 counter-clockwise", ui::Icon::Rot90)) post_command(ID_TOOL_ROT90);
            if (menu_item_icon("180\xC2\xB0", ui::Icon::Rot180)) post_command(ID_TOOL_ROT180);
            if (menu_item_icon("90\xC2\xB0 clockwise", ui::Icon::Rot270)) post_command(ID_TOOL_ROT270);
            ImGui::EndMenu();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turn the selected placements about their own centre (angles add up; one undo step each)");
        menu_item(ID_CTX_SIDE_FLIP);
        {
            int nd = 0;
            for (auto& c : doc->prj.comps) if (c.selected && c.dnp) ++nd;
            if (menu_item_icon("Do Not Place", ui::Icon::NoRef, nullptr, n > 0 && nd == n, n > 0)) post_command(ID_CTX_TOGGLE_DNP);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Do Not Place (DNP): the part stays in the project and on the plot (drawn as DNP),\n"
                                  "but the machine program skips it and it is not counted as a placement.\n"
                                  "Use it for parts fitted by hand later, optional variants or parts the customer leaves out.\n"
                                  "Hiding (eye icon / side filter) only changes what you see; hidden parts are still placed.\n"
                                  "Click again to place the part normally.");
        }
        ImGui::Separator();
        for (int id : {ID_CTX_MAKE_FIDUCIAL, ID_CTX_MAKE_COMPONENT}) menu_item(id);
        ImGui::Separator();
        // R17c (REVIEW_V16 §7): Select Similar replaces the Select-by-P/N / package / technology toolbar modes
        if (begin_menu_icon("Select Similar", ui::Icon::SelectAll, n > 0)) {
            std::set<std::string> pn;
            std::set<int> tech;   // 1 = SMT, 0 = thru-hole
            for (auto& c : doc->prj.comps) if (c.selected) {
                if (!c.number.empty()) pn.insert(c.number);
                if (c.feature == Feature::Component) { const Package* pp = doc->prj.package_of(c); tech.insert(pp ? (int)pp->smt : 1); }
            }
            auto apply = [&](const char* what, auto&& match) {
                int k = 0;
                for (auto& c : doc->prj.comps) { c.selected = match(c); k += c.selected; }
                doc->changed(ui::CH_SELECTION);
                gui::status(std::to_string(k) + " components selected with the same " + what);
            };
            const std::string lp = pk.size() == 1 ? "Same Package (" + *pk.begin() + ")" : std::string("Same Package");
            if (menu_item_icon(lp.c_str(), ui::Icon::SelectPackage, nullptr, false, !pk.empty())) apply("package", [&](const Component& c) { return pk.count(c.package) > 0; });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select every component that uses the same package as the selection (all sides, visible or not)");
            const std::string lpn = pn.size() == 1 ? "Same Part Number (" + *pn.begin() + ")" : std::string("Same Part Number");
            if (menu_item_icon(lpn.c_str(), ui::Icon::SelectPart, nullptr, false, !pn.empty())) apply("part number", [&](const Component& c) { return !c.number.empty() && pn.count(c.number) > 0; });
            const std::string lt = tech.size() == 1 ? std::string("Same Technology (") + (*tech.begin() ? "SMT" : "thru-hole") + ")" : std::string("Same Technology");
            if (menu_item_icon(lt.c_str(), ui::Icon::SelectTech, nullptr, false, !tech.empty())) apply("technology", [&](const Component& c) {
                if (c.feature != Feature::Component) return false;
                const Package* pp = doc->prj.package_of(c);
                return tech.count(pp ? (int)pp->smt : 1) > 0;
            });
            ImGui::EndMenu();
        }
        ImGui::Separator();
        {   // R17c (REVIEW_V16 §8): Assign / Unassign against the ACTIVE export layer
            const int cur = current_point(*doc);
            const bool have = cur >= 0 && cur < (int)doc->prj.model.points.size();
            const std::string la = have ? "Assign to " + (doc->prj.model.points[cur].name.empty() ? "Layer " + std::to_string(cur + 1) : doc->prj.model.points[cur].name)
                                        : std::string("Assign to Active Layer");
            if (menu_item_icon(la.c_str(), ui::Icon::Assign, "A", false, n > 0 && have)) post_command(ID_TOOL_ASSIGN);
            if (menu_item_icon("Unassign", ui::Icon::Unassign, "Shift+A", false, n > 0)) post_command(ID_TOOL_UNASSIGN);
        }
        if (begin_menu_icon("Assign to Other Layer", ui::Icon::Assign, n > 0)) { move_to_layer_menu_items(doc); ImGui::EndMenu(); }   // P2-10
        ImGui::Separator();
        menu_item(ID_CTX_DELETE);
        ImGui::Separator();
        if (menu_item_icon("Zoom to Selection", ui::Icon::ZoomRect, "F", false, n > 0)) canvas.zoom_selection();
        menu_item(ID_VIEW_ZOOM_ALL);
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
bool PlotterView::on_command(int id) {
    if (canvas.owner != this) { canvas.owner = this; canvas.doc = doc; }
    Doc& d = *doc;
    HWND h = frame_hwnd();
    auto set_tool = [&](Tool t) { canvas.tool = t; canvas.meas_n = 0; canvas.pick_n = 0; return true; };
    switch (id) {
    case ID_TOOL_INFO: return set_tool(Tool::Info);
    case ID_TOOL_SELECT: return set_tool(Tool::Select);
    case ID_TOOL_SEL_PART: return set_tool(Tool::SelPart);
    case ID_TOOL_SEL_PACKAGE: return set_tool(Tool::SelPackage);
    case ID_TOOL_ASSIGN: {   // R13: remember what is being assigned for the canvas pulse; the frame command does the work
        const int pt = current_point(d);
        canvas.flash_ids.clear();
        for (auto* c : d.selection()) if (c->feature == Feature::Component) canvas.flash_ids.push_back(c->id);
        if (pt >= 0 && pt < (int)d.prj.model.points.size() && !canvas.flash_ids.empty()) {
            canvas.flash_t0 = ImGui::GetTime();
            canvas.flash_col = argb2im(d.prj.model.points[pt].color);
            canvas.flash_label = std::to_string(canvas.flash_ids.size()) + " \xE2\x86\x92 " + d.prj.model.points[pt].name;
        }
        return false;
    }
    case ID_TOOL_SEL_TECH: return set_tool(Tool::SelTech);
    case ID_TOOL_PIN1: return set_tool(Tool::Pin1);
    case ID_TOOL_ORIGIN: set_tool(Tool::Origin); gui::status("Click a component, pad or point to set the virtual origin"); return true;
    case ID_TOOL_LABEL: return set_tool(Tool::Label);
    // R16 (REVIEW_V15 §4): no free-form drag / resize of outlines; the tool opens the explicit outline editor
    case ID_TOOL_FRAME_CIRCUIT: post_command(ID_BOARD_ADD_FRAME); return true;
    case ID_TOOL_FRAME_PANEL: set_tool(Tool::FramePanel); gui::status("Board outline (panel): click the upper-left corner, then the lower-right (snaps to geometry)"); return true;
    case ID_TOOL_MEASURE: set_tool(Tool::Measure); gui::status("Measure: click two points; Esc clears"); return true;
    case ID_BOARD_ALIGN_MOVE: set_tool(Tool::AlignMove); gui::status("Align: click a point on the plot, then the target point"); return true;
    case ID_BOARD_ALIGN_ROTATE: set_tool(Tool::AlignRotate); gui::status("Rotate: click the pivot, a reference point, then its target"); return true;
    case ID_BOARD_ALIGN_2REF: set_tool(Tool::Align2Ref); gui::status("Two-point align: plot A, target A, plot B, target B"); return true;
    case ID_VIEW_ZOOM_EXTENTS: if (!canvas.zoom_selection()) canvas.zoom_all(); return true;
    case ID_TOOL_ROT90: rotate_selection(90); return true;
    case ID_TOOL_ROT180: rotate_selection(180); return true;
    case ID_TOOL_ROT270: rotate_selection(270); return true;
    case ID_VIEW_ZOOM_IN: canvas.zoom_by(1.4); return true;
    case ID_VIEW_ZOOM_OUT: canvas.zoom_by(1 / 1.4); return true;
    case ID_VIEW_ZOOM_RECT: canvas.begin_zoom_rect(); gui::status("Drag a rectangle to zoom"); return true;
    case ID_VIEW_ZOOM_100: canvas.zoom_100(); return true;
    case ID_VIEW_ZOOM_ALL: canvas.zoom_all(); return true;
    case ID_MFG_CHECK: open_tab(TAB_REVIEW); run_review(); return true;
    case ID_VIEWER_TOP: set_view_bottom(d, false); return true;
    case ID_VIEWER_BOTTOM: set_view_bottom(d, true); return true;
    case ID_TOOL_SEARCH: case ID_TOOL_FIND_REF: open_tab(TAB_SEARCH); focus_search = true; return true;
    case ID_EDIT_SELECT_ALL: {
        for (auto& c : d.prj.comps) {
            if (c.side == Side::Top && !d.show_top) continue;
            if (c.side == Side::Bottom && !d.show_bottom) continue;
            c.selected = true;
        }
        d.changed(ui::CH_SELECTION);
        return true;
    }
    case ID_EDIT_DELETE: case ID_CTX_DELETE: case ID_EDIT_CUT: {
        int n = d.selected_count();
        if (!n) return true;
        if (id == ID_EDIT_CUT) on_command(ID_EDIT_COPY);
        else if (n > 1 && !confirm("Delete Components", "Delete " + std::to_string(n) + " selected components?", "Delete", true)) return true;
        d.checkpoint(id == ID_EDIT_CUT ? "Cut components" : "Delete components");
        std::vector<Component> keep;
        for (auto& c : d.prj.comps) if (!c.selected) keep.push_back(c);
        d.prj.comps = keep;
        canvas.hover = -1;
        d.changed(ui::CH_GEOMETRY);
        undo_toast(std::to_string(n) + (n == 1 ? " component " : " components ") + (id == ID_EDIT_CUT ? "cut" : "deleted"));
        return true;
    }
    case ID_EDIT_COPY: {
        clip().clear();
        for (auto& c : d.prj.comps) if (c.selected) clip().push_back(c);
        gui::status(std::to_string(clip().size()) + " components copied", ToastKind::Success);
        return true;
    }
    case ID_EDIT_PASTE: {
        if (clip().empty()) return true;
        d.checkpoint("Paste components");
        d.select_none();
        for (auto c : clip()) {
            c.id = 0; c.pos += Vec2{1.27, -1.27};
            std::string base = c.ref;
            int k = 1;
            do c.ref = base + "_" + std::to_string(k++); while (!base.empty() && d.prj.find_ref(c.ref));
            if (base.empty()) c.ref.clear();
            c.selected = true;
            d.prj.add_component(c);
        }
        d.changed(ui::CH_GEOMETRY);
        undo_toast(std::to_string(clip().size()) + " components pasted (offset 1.27 mm)");
        return true;
    }
    case ID_CTX_EDIT_LOCATION: {
        auto sel = d.selection();
        if (sel.empty()) return true;
        // edit the project component in place (a copy would open as "Add New Component" and
        // flag its own reference as a duplicate); dlg_location checkpoints + notifies itself
        dlg_location(h, d, *sel[0]);
        return true;
    }
    case ID_CTX_EDIT_PN: {
        auto sel = d.selection();
        if (!sel.empty() && dlg_part_number(h, d, sel[0]->number)) d.changed(ui::CH_GEOMETRY);
        return true;
    }
    case ID_CTX_EDIT_PACKAGE: {
        auto sel = d.selection();
        open_package_editor(doc, sel.empty() ? std::string() : sel[0]->package);
        return true;
    }
    case ID_CTX_MAKE_FIDUCIAL: {
        int n = d.selected_count();
        if (n && dlg_fiducial(h, d, d.selection())) { d.changed(ui::CH_GEOMETRY); undo_toast(std::to_string(n) + (n == 1 ? " component" : " components") + " made fiducial"); }
        return true;
    }
    case ID_CTX_MAKE_COMPONENT:
        d.checkpoint("Fiducial to component");
        for (auto* c : d.selection()) c->feature = Feature::Component;
        d.changed(ui::CH_GEOMETRY);
        undo_toast(std::to_string(d.selected_count()) + " fiducial(s) changed to components");
        return true;
    case ID_CTX_TOGGLE_DNP:
        d.checkpoint("Toggle DNP");
        for (auto* c : d.selection()) c->dnp = !c->dnp;
        d.changed(ui::CH_GEOMETRY);
        undo_toast("Toggled DNP on " + std::to_string(d.selected_count()) + " components");
        return true;
    case ID_CTX_SIDE_FLIP:
        d.checkpoint("Flip side");
        for (auto* c : d.selection()) c->side = c->side == Side::Top ? Side::Bottom : Side::Top;
        d.changed(ui::CH_GEOMETRY);
        undo_toast("Moved " + std::to_string(d.selected_count()) + " components to the other side");
        return true;
    default: return false;
    }
}

int PlotterView::cmd_state(int id) {
    auto ck = [&](Tool t) { return 1 | (canvas.tool == t ? 2 : 0); };
    const bool sel = doc->selected_count() > 0;
    switch (id) {
    case ID_TOOL_INFO: return ck(Tool::Info);
    case ID_TOOL_SELECT: return ck(Tool::Select);
    case ID_TOOL_SEL_PART: return ck(Tool::SelPart);
    case ID_TOOL_SEL_PACKAGE: return ck(Tool::SelPackage);
    case ID_TOOL_SEL_TECH: return ck(Tool::SelTech);
    case ID_TOOL_PIN1: return ck(Tool::Pin1);
    case ID_TOOL_ORIGIN: return ck(Tool::Origin);
    case ID_TOOL_LABEL: return ck(Tool::Label);
    case ID_TOOL_FRAME_CIRCUIT: return ck(Tool::FrameCircuit);
    case ID_TOOL_FRAME_PANEL: return ck(Tool::FramePanel);
    case ID_TOOL_MEASURE: return ck(Tool::Measure);
    case ID_BOARD_ALIGN_MOVE: return ck(Tool::AlignMove);
    case ID_BOARD_ALIGN_ROTATE: return ck(Tool::AlignRotate);
    case ID_BOARD_ALIGN_2REF: return ck(Tool::Align2Ref);
    case ID_VIEW_ZOOM_EXTENTS: return 1;
    case ID_VIEWER_TOP: return 1 | (!canvas.mirror() ? 2 : 0);
    case ID_VIEWER_BOTTOM: return 1 | (canvas.mirror() ? 2 : 0);
    case ID_TOOL_SEARCH: case ID_TOOL_FIND_REF: return 1 | (g_panel_open && g_panel_tab == TAB_SEARCH ? 2 : 0);
    case ID_VIEW_ZOOM_IN: case ID_VIEW_ZOOM_OUT: case ID_VIEW_ZOOM_RECT: case ID_VIEW_ZOOM_100: case ID_VIEW_ZOOM_ALL: case ID_EDIT_SELECT_ALL: return 1;
    case ID_TOOL_ROT90: case ID_TOOL_ROT180: case ID_TOOL_ROT270: case ID_EDIT_DELETE: case ID_EDIT_CUT: case ID_EDIT_COPY:
    case ID_CTX_EDIT_LOCATION: case ID_CTX_EDIT_PN: case ID_CTX_MAKE_FIDUCIAL: case ID_CTX_TOGGLE_DNP: case ID_CTX_SIDE_FLIP: case ID_CTX_DELETE:
        return sel ? 1 : 0;
    case ID_CTX_EDIT_PACKAGE: return 1;
    case ID_CTX_MAKE_COMPONENT: {
        for (auto& c : doc->prj.comps) if (c.selected && c.feature != Feature::Component) return 1;
        return 0;
    }
    case ID_EDIT_PASTE: return clip().empty() ? 0 : 1;
    }
    return -1;
}

void PlotterView::print(HDC dc, const RECT& page, bool one_to_one) {
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 0, 0);
    Com<ID2D1DCRenderTarget> rt;
    if (FAILED(ui::d2d_factory()->CreateDCRenderTarget(&rp, rt.put()))) return;
    if (FAILED(rt->BindDC(dc, &page))) return;
    float dpix = (float)GetDeviceCaps(dc, LOGPIXELSX), dpiy = (float)GetDeviceCaps(dc, LOGPIXELSY);
    rt->SetDpi(dpix, dpiy);
    const float pw = page.right * 96.f / dpix, ph = page.bottom * 96.f / dpiy;   // DIPs
    BBox b = doc->prj.board_bbox();
    if (b.empty()) b = BBox(0, 0, 100, 80);
    double s;
    if (one_to_one) s = 96.0 / 25.4 * settings().print_scale_x;
    else s = std::min((pw - 60) / std::max(b.w(), 1.0), (ph - 100) / std::max(b.h(), 1.0));
    const bool m = canvas.mirror();
    const double sy = s * (one_to_one ? settings().print_scale_y / settings().print_scale_x : 1.0);
    BoardRenderOpts o;
    o.world = D2D1::Matrix3x2F((float)(m ? -s : s), 0, 0, (float)-sy, (float)(pw / 2 - (m ? -s : s) * b.center().x), (float)(ph / 2 + sy * b.center().y));
    o.px_per_mm = s;
    o.mirror = m;
    o.print_mode = true;
    o.show_labels = doc->show_labels; o.show_frame = doc->show_frame; o.all_images = doc->all_images;
    o.show_top = doc->show_top; o.show_bottom = doc->show_bottom; o.second_side = doc->second_side;
    o.ghost_far = doc->far_drawn(); o.far_alpha = 0.45f;
    o.show_selection = false;
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(1, 1, 1));
    render_board(rt.get(), *doc, o);
    Com<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), br.put());
    const AssemblyInfo& in = doc->prj.info;
    std::wstring hdr = ui::W(in.name.empty() ? ui::U(doc->display_name()) : in.name);
    if (!in.number.empty()) hdr += L"  " + ui::W(in.number);
    if (!in.assembly_rev.empty()) hdr += L"  Rev " + ui::W(in.assembly_rev);
    hdr += m ? L"   (bottom view)" : L"   (top view)";
    if (one_to_one) hdr += L"   1:1";
    ui::draw_text(rt.get(), hdr, D2D1::RectF(20, 10, pw - 20, 34), ui::shared_text_format(14.f, true), br.get());
    rt->EndDraw();
}

} // namespace

std::unique_ptr<View> make_plotter_view() { return std::make_unique<PlotterView>(); }

void focus_components(const app::DocPtr& d, const std::vector<int>& idx, bool select) {
    if (!d) return;
    activate_view(d, ViewKind::Plotter);
    DocUI* u = docui_of(d.get());
    auto* v = u ? dynamic_cast<PlotterView*>(u->view(ViewKind::Plotter)) : nullptr;
    if (select) {
        d->select_none();
        for (int i : idx) if (i >= 0 && i < (int)d->prj.comps.size()) d->prj.comps[i].selected = true;
        d->changed(ui::CH_SELECTION);
    }
    if (v) v->fly_to_comps(idx);
    if (idx.size() == 1 && idx[0] >= 0 && idx[0] < (int)d->prj.comps.size()) {
        const Component& c = d->prj.comps[idx[0]];
        gui::status((c.ref.empty() ? std::string("(no ref)") : c.ref) + "  " + fmt_pos_u8(*d, c.pos) + "  " + c.number + "  " + c.package);
    } else if (!idx.empty()) gui::status(std::to_string(idx.size()) + " components");
}

// R17b (REVIEW_V16 §4): inferred board outline acknowledgement row, shared by the plot banner and the Export
// validation panel. compact = one line (banner). Actions: Use as-is (these dims), Don't warn for this
// project, Confirm (explicit, -> Confirmed), Define (outline editor / Gerber tools). All undoable.
void outline_ack_strip(const app::DocPtr& dp, bool compact) {
    if (!dp) return;
    app::Doc& d = *dp;
    Project& p = d.prj;
    if (p.board_outline_confirmed()) return;
    const BBox b = !p.panel_frame.empty() ? p.panel_frame_bbox() : p.board_bbox();
    const std::string wh = fmt_len_u8(b.w(), false) + " \xC3\x97 " + fmt_len_u8(b.h());
    ImGui::PushID("##outack");
    const ImVec4 amber(1.f, 0.72f, 0.25f, 1.f);
    ImGui::AlignTextToFramePadding();
    if (compact) ImGui::TextColored(amber, "Board outline inferred: %s", wh.c_str());
    else {
        if (p.inferred_outline_acknowledged()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(th().text_dim), "Board outline inferred (%s), used as-is%s.", wh.c_str(), p.outline_warn_off ? ", warning off for this project" : "");
        else ImGui::TextColored(amber, "Board outline inferred from the placements: %s", wh.c_str());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("No measured board outline: the size is the placement extent + 2 mm.\nThe machine program uses this board size. Check it against the board drawing.");
    auto act = [&](const char* label, const char* tip) {
        ImGui::SameLine(0, S(6));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(7), S(3)));
        const bool r = ImGui::Button(label);
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        return r;
    };
    if (!p.inferred_outline_acknowledged() && act("Use as-is", "Accept these dimensions for the machine program (no more warning while they stay the same).")) {
        d.checkpoint("Use inferred board outline as-is"); p.acknowledge_inferred_outline(false); d.changed(ui::CH_GEOMETRY);
        toast("Inferred board outline accepted: " + wh, ToastKind::Info, 4.f, "Undo", [] { post_command(app::ID_EDIT_UNDO); });
    }
    // R22 (REVIEW_V21 §3): "Confirm as measured" lives here (the Confirm Board Outline menu item is retired)
    if (act("Confirm as measured...", "You measured the board and these dimensions match the drawing: the inferred outline becomes a confirmed outline.")) post_command(app::ID_BOARD_CONFIRM_OUTLINE);
    if (act("Define...", "Type the measured corners (Board > Outline > Add Outline). From Gerber geometry: Gerber viewer outline tools.")) post_command(app::ID_BOARD_ADD_FRAME);
    if (!p.outline_warn_off) {
        ImGui::SameLine(0, S(4));
        if (ImGui::SmallButton("...##outmore")) ImGui::OpenPopup("##outackmore");   // v17: ASCII (U+22EF rendered as ?)
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("More");
        if (ImGui::BeginPopup("##outackmore")) {
            if (ImGui::MenuItem("Don't warn for this project")) {
                d.checkpoint("Don't warn about the inferred outline"); p.acknowledge_inferred_outline(true); d.changed(ui::CH_GEOMETRY);
                toast("Inferred-outline warning off for this project", ToastKind::Info, 4.f, "Undo", [] { post_command(app::ID_EDIT_UNDO); });
            }
            ImGui::EndPopup();
        }
    }
    ImGui::PopID();
}

} // namespace pcbp::gui

