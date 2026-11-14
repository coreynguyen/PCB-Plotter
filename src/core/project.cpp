// ============================================================================
// project.cpp — PCB Plotter document model helpers.
// ============================================================================
#include "project.h"
#include "package_edit.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

namespace pcbp {

// ---------------------------------------------------------------------------
// Graphics
// ---------------------------------------------------------------------------
BBox gprim_bbox(const GPrim& g) {
    BBox b;
    switch (g.kind) {
    case GKind::Circle:
    case GKind::Arc:
        if (!g.pts.empty()) {
            b.add({g.pts[0].x - g.radius, g.pts[0].y - g.radius});
            b.add({g.pts[0].x + g.radius, g.pts[0].y + g.radius});
        }
        break;
    case GKind::Rect:
        if (g.pts.size() >= 2) {
            if (g.rot == 0) { b.add(g.pts[0]); b.add(g.pts[1]); }
            else {
                Vec2 c = (g.pts[0] + g.pts[1]) * 0.5;
                Vec2 corners[4] = {{g.pts[0].x, g.pts[0].y}, {g.pts[1].x, g.pts[0].y},
                                   {g.pts[1].x, g.pts[1].y}, {g.pts[0].x, g.pts[1].y}};
                for (auto& p : corners) b.add(rotate_about(p, c, g.rot));
            }
        }
        break;
    case GKind::Text:
        if (!g.pts.empty()) {
            double w = g.text_h * 0.6 * (double)g.text.size();
            b.add(g.pts[0]);
            b.add(g.pts[0] + rotate({w, g.text_h}, g.rot));
        }
        break;
    case GKind::Group:
        for (auto& c : g.children) b.add(gprim_bbox(c));
        break;
    default:
        for (auto& p : g.pts) b.add(p);
        break;
    }
    if (g.width > 0) b.inflate(g.width * 0.5);
    return b;
}

BBox gprims_bbox(const std::vector<GPrim>& v) {
    BBox b;
    for (auto& g : v) b.add(gprim_bbox(g));
    return b;
}

// ---------------------------------------------------------------------------
// Package
// ---------------------------------------------------------------------------
BBox Package::extent() const {
    BBox b = gprims_bbox(body);
    for (auto& p : pads) {
        const double hw = p.w * 0.5, hh = p.h * 0.5;
        Vec2 c[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
        for (auto& q : c) b.add(rotate(q, p.rot) + p.pos);
    }
    if (b.empty()) { b.add({-0.5, -0.5}); b.add({0.5, 0.5}); }
    return b;
}

BBox pkg_body_box(const Package& p, std::string* src) {
    const bool real = !p.body.empty() && p.body_source != "pads" && p.body_source != "cad_extent";
    if (real) {
        // geometric outline only: the drawing pen width is not part of the body (gprims_bbox() includes it)
        std::vector<GPrim> v;
        for (auto g : p.body) if (g.kind != GKind::Text) { g.width = 0; v.push_back(std::move(g)); }
        BBox b = gprims_bbox(v);
        if (!b.empty() && b.w() > 1e-6 && b.h() > 1e-6) { if (src) *src = "body"; return b; }
    }
    if (src) *src = "pads";
    BBox b;   // lead / pad extent (a pad-derived body rectangle adds a margin: not used)
    for (auto& d : p.pads) {
        const double hw = d.w * 0.5, hh = d.h * 0.5;
        Vec2 c[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
        for (auto& q : c) b.add(rotate(q, d.rot) + d.pos);
    }
    return b.empty() ? p.extent() : b;
}

void Package::auto_body() {
    body.clear();
    body_source = "pads";
    BBox b;
    for (auto& p : pads) {
        const double hw = p.w * 0.5, hh = p.h * 0.5;
        Vec2 c[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
        for (auto& q : c) b.add(rotate(q, p.rot) + p.pos);
    }
    if (b.empty()) { b.add({-0.6, -0.4}); b.add({0.6, 0.4}); }
    // body hugs the pad field, slightly inside for two-terminal chips
    GPrim r;
    r.kind = GKind::Rect;
    double ix = 0, iy = 0;
    if (pads.size() == 2) { ix = b.w() * 0.12; iy = 0; }
    r.pts = {{b.x0 + ix, b.y0 + iy}, {b.x1 - ix, b.y1 - iy}};
    r.width = 0.1;
    body.push_back(r);
}

// ---------------------------------------------------------------------------
// Factory model
// ---------------------------------------------------------------------------
void FactoryModel::migrate_export_dir() {
    if (export_dir.empty() && !points.empty()) export_dir = points[0].machine.target_dir;
    for (auto& m : points) { if (export_dir.empty()) export_dir = m.machine.target_dir; }
    for (auto& m : points) m.machine.target_dir.clear();
}
FactoryModel FactoryModel::make_default() {
    // R10 (REVIEW_V9 export layers): ordinary SMT projects start with Top SMT + Bottom SMT.
    FactoryModel m = make_legacy_default();
    ModelPoint top = m.points[0];
    top.name = "Top SMT"; top.side = top.machine.side = Side::Top; top.machine.flipped = false;
    ModelPoint bot = top;
    bot.name = "Bottom SMT"; bot.color = 0xFF0CA678; bot.side = bot.machine.side = Side::Bottom; bot.machine.flipped = true;
    m.points[0] = top;
    m.points.insert(m.points.begin() + 1, bot);
    // R12 §16: new projects get the layers this SMT workflow uses: Top SMT, Bottom SMT, Hand Load, Through-Hole.
    // Reflow / Auto Insertion / Slide Line / Wave Solder / Inspection come from the CircuitCAM factory model
    // ("SMT/T-Hole (1 Side)"; process points of its factory modeler - not an IPC classification) and stay in
    // make_legacy_default() for legacy projects / .cpf imports.
    std::vector<ModelPoint> keep;
    for (auto& p : m.points) if (p.name == "Top SMT" || p.name == "Bottom SMT" || p.name == "Hand Load") keep.push_back(p);
    for (auto& p : m.points) if (p.name == "Hand Load") {
        ModelPoint th = p;
        th.name = "Through-Hole"; th.process_type = "TH"; th.color = 0xFF7048E8;
        th.filter_process = {"TH"};
        keep.push_back(th);
    }
    m.points = keep;
    return m;
}

FactoryModel FactoryModel::make_legacy_default() {
    FactoryModel m;
    m.name = "SMT/T-Hole (1 Side)";
    m.description = "Default factory model";
    auto add = [&](const char* name, const char* ptype, PointKind k, uint32_t col, bool mach) {
        ModelPoint p;
        p.name = name; p.process_type = ptype; p.kind = k; p.color = col;
        p.has_machine = mach;
        if (mach) p.machine.exporter_id = "juki.h8h";
        p.filter_process.push_back(ptype);
        m.points.push_back(p);
    };
    add("SMT Placement", "SMT", PointKind::Placement, 0xFF3C8CFF, true);
    add("Reflow", "REFLOW", PointKind::Process, 0xFFE8590C, false);
    add("Auto Insertion", "AUTO", PointKind::Placement, 0xFF7048E8, true);
    add("Slide Line", "SLIDE", PointKind::Manual, 0xFF2F9E44, false);
    add("Hand Load", "HAND", PointKind::Manual, 0xFFF59F00, false);
    add("Wave Solder", "WAVE", PointKind::Process, 0xFFC2255C, false);
    add("Inspection", "AOI", PointKind::Inspection, 0xFF1098AD, false);
    for (auto& p : m.points) if (p.kind == PointKind::Process || p.kind == PointKind::Inspection) p.filter_process.clear();
    return m;
}

// ---------------------------------------------------------------------------
// Project
// ---------------------------------------------------------------------------
const char* process_name(Process p) {
    switch (p) {
    case Process::SMT: return "SMT";
    case Process::TH: return "TH";
    case Process::Hardware: return "Hardware";
    case Process::Other: return "Other";
    default: return "Auto";
    }
}
Process process_from_name(const std::string& s0) {
    std::string s; for (char ch : s0) s += (char)std::toupper((unsigned char)ch);
    if (s == "SMT" || s == "SMD" || s == "SURFACE MOUNT") return Process::SMT;
    if (s == "TH" || s == "THT" || s == "PTH" || s == "THRU" || s == "THROUGH-HOLE" || s == "THROUGH HOLE") return Process::TH;
    if (s == "HARDWARE" || s == "HW" || s == "MECHANICAL") return Process::Hardware;
    if (s == "OTHER") return Process::Other;
    return Process::Auto;
}
Process Project::process_of(const Component& c) const {
    if (c.process != Process::Auto) return c.process;
    if (const PartNumber* pn = part_of(c)) {
        const Process p = process_from_name(pn->pop_type);
        if (p != Process::Auto) return p;
    }
    if (const Package* pk = package_of(c)) {
        if (!pk->smt) return Process::TH;
        for (const Pad& pd : pk->pads) if (pd.drill > 0) return Process::TH;
    }
    return Process::SMT;
}

Component& Project::add_component(const Component& c) {
    comps.push_back(c);
    Component& r = comps.back();
    r.rot = norm_deg(r.rot);
    if (r.id == 0) r.id = next_id++;
    else if (r.id >= next_id) next_id = r.id + 1;
    if (r.cad_pos == Vec2{} && r.rot_imported == 0) { r.cad_pos = r.pos; r.rot_imported = r.rot; }
    dirty = true;
    return r;
}

static bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::toupper((unsigned char)a[i]) != std::toupper((unsigned char)b[i])) return false;
    return true;
}

Component* Project::find_ref(const std::string& ref) {
    for (auto& c : comps) if (ieq(c.ref, ref)) return &c;
    return nullptr;
}
const Component* Project::find_ref(const std::string& ref) const {
    for (auto& c : comps) if (ieq(c.ref, ref)) return &c;
    return nullptr;
}

const Package* Project::package_of(const Component& c) const {
    std::string name = c.package;
    if (name.empty()) {
        auto it = parts.find(c.number);
        if (it != parts.end()) name = it->second.package;
    }
    auto it = packages.find(name);
    if (it == packages.end()) return nullptr;
    // R16b: an unresolved placeholder (no pads, no artwork; core/package_resolve.h) draws and measures like a
    // missing package, so geometry code never sees an empty extent.
    if (it->second.pads.empty() && it->second.body.empty()) return nullptr;
    return &it->second;
}

// R18a supply angle per supplied packaging (specs/ANGLES.md §10)
static std::string pkg_lc(const std::string& s) { std::string r; for (char c : s) r += (char)std::tolower((unsigned char)c); return r; }
double supply_angle_default(const std::string& packaging) {
    const std::string k = pkg_lc(packaging);
    if (k == "stick") return 270;
    if (k == "tray") return 180;
    return 0;   // tape, mtc, bulk, unknown
}
double effective_supply_angle(const PartNumber& pn) { return pn.supply_angle_set ? pn.supply_angle : supply_angle_default(pn.packaging); }
int packaging_feeder_kind(const std::string& packaging) {
    const std::string k = pkg_lc(packaging);
    if (k == "tape") return 1;
    if (k == "stick") return 2;
    if (k == "tray" || k == "mtc") return 3;
    if (k == "bulk") return 4;
    return 0;
}
std::string packaging_from_feeder_kind(int kind) {
    switch (kind) { case 1: return "Tape"; case 2: return "Stick"; case 3: return "Tray"; case 4: return "Bulk"; default: return ""; }
}

const PartNumber* Project::part_of(const Component& c) const {
    auto it = parts.find(c.number);
    return it == parts.end() ? nullptr : &it->second;
}

Xform Project::image_xform(int image) const {
    if (image <= 0 || image >= (int)images.size()) return Xform{};
    const PanelImage& im = images[image];
    return Xform::translate(im.offset.x, im.offset.y) * Xform::rotation(im.rot);
}

Xform Project::comp_xform(const Component& c, int image) const {
    Xform t = Xform::translate(c.pos.x, c.pos.y) * Xform::rotation(c.rot);
    if (c.side == Side::Bottom) t = t * Xform::scale(-1, 1);   // viewed from top: mirrored footprint
    return image_xform(image) * t;
}

BBox Project::comp_bbox(const Component& c) const {
    const Package* p = package_of(c);
    BBox local;
    if (p) local = p->extent();
    else { local.add({-0.4, -0.4}); local.add({0.4, 0.4}); }
    if (c.feature == Feature::Fiducial) { local = BBox{}; local.add({-0.75, -0.75}); local.add({0.75, 0.75}); }
    Xform t = comp_xform(c, 0);
    BBox b;
    b.add(t.apply({local.x0, local.y0})); b.add(t.apply({local.x1, local.y0}));
    b.add(t.apply({local.x1, local.y1})); b.add(t.apply({local.x0, local.y1}));
    return b;
}

const char* fid_scope_name(FiducialScope s) {
    switch (s) { case FiducialScope::Global: return "Global"; case FiducialScope::Local: return "Local"; case FiducialScope::Image: return "Image"; }
    return "?";
}
FiducialScope fid_scope_from_cpf(int t) {
    return t == 2 ? FiducialScope::Image : t == 3 ? FiducialScope::Local : FiducialScope::Global;
}
int fid_scope_to_cpf(FiducialScope s) {
    return s == FiducialScope::Image ? 2 : s == FiducialScope::Local ? 3 : 1;
}

bool Project::instanced(const Component& c) const {
    return !(c.feature == Feature::Fiducial && c.fid_scope == FiducialScope::Global);
}

Xform Project::instance_xform(const Component& c, int image) const {
    return comp_xform(c, instanced(c) ? image : 0);
}

BBox Project::circuit_frame_bbox() const {
    BBox b;
    for (auto& poly : board_frame) for (auto& p : poly) b.add(p);
    return b;
}

BBox Project::panel_frame_bbox() const {
    BBox b;
    for (auto& poly : panel_frame) for (auto& p : poly) b.add(p);
    if (b.empty())
        for (int i = 0; i < (int)images.size(); ++i)
            for (auto& poly : circuit_frame_at(i)) for (auto& p : poly) b.add(p);
    return b;
}

static std::vector<Vec2> rect_poly(const BBox& b) {
    return {{b.x0, b.y0}, {b.x1, b.y0}, {b.x1, b.y1}, {b.x0, b.y1}, {b.x0, b.y0}};
}
void Project::set_circuit_frame_rect(const BBox& b) {
    board_frame.clear();
    if (!b.empty()) board_frame.push_back(rect_poly(b));
    dirty = true;
}
void Project::set_panel_frame_rect(const BBox& b) {
    panel_frame.clear();
    if (!b.empty()) panel_frame.push_back(rect_poly(b));
    dirty = true;
}
BBox Project::placement_extents(double margin) const {
    BBox b;
    for (auto& c : comps) b.add(comp_bbox(c));
    if (!b.empty()) b.inflate(margin);
    return b;
}
bool Project::circuit_frame_is_placement_extent() const {
    if (board_frame.size() != 1) return false;
    const auto& pl = board_frame[0];
    if (pl.size() != 4 && pl.size() != 5) return false;
    BBox f; for (auto& q : pl) f.add(q);
    for (auto& q : pl) if (std::fabs(q.x - f.x0) > 1e-6 && std::fabs(q.x - f.x1) > 1e-6) return false;   // axis-aligned
    auto same = [&](const BBox& e) { const double t = 0.01; return !e.empty() && std::fabs(e.x0 - f.x0) < t && std::fabs(e.y0 - f.y0) < t &&
                                     std::fabs(e.x1 - f.x1) < t && std::fabs(e.y1 - f.y1) < t; };
    return same(placement_extents(2.0)) || same(placement_extents(0.0));
}
void Project::infer_circuit_outline() {
    if (!board_frame.empty() && board_outline_src != OutlineSource::Inferred) return;   // never resize a real outline
    BBox e = placement_extents(2.0);
    if (e.empty()) return;
    set_circuit_frame_rect(e);
    board_outline_src = OutlineSource::Inferred;
}
void Project::confirm_outlines() {
    if (!board_frame.empty() && board_outline_src == OutlineSource::Inferred) board_outline_src = OutlineSource::Confirmed;
    if (!panel_frame.empty() && panel_outline_src == OutlineSource::Inferred) panel_outline_src = OutlineSource::Confirmed;
    dirty = true;
}
bool Project::inferred_outline_acknowledged() const {
    if (board_outline_confirmed()) return true;
    if (outline_warn_off) return true;
    const BBox b = board_bbox();
    if (outline_ack_box.empty() || b.empty()) return false;
    const double t = 0.01;
    return std::fabs(b.x0 - outline_ack_box.x0) < t && std::fabs(b.y0 - outline_ack_box.y0) < t &&
           std::fabs(b.x1 - outline_ack_box.x1) < t && std::fabs(b.y1 - outline_ack_box.y1) < t;
}
void Project::acknowledge_inferred_outline(bool never_warn) {
    if (never_warn) outline_warn_off = true;
    outline_ack_box = board_bbox();
    dirty = true;
}
void Project::offset_circuit_frame(Vec2 d) {
    for (auto& poly : board_frame) for (auto& p : poly) p = p + d;
    dirty = true;
}
std::vector<std::vector<Vec2>> Project::circuit_frame_at(int image) const {
    std::vector<std::vector<Vec2>> out = board_frame;
    if (image <= 0 || image >= (int)images.size()) return out;
    const Xform t = image_xform(image);
    for (auto& poly : out) for (auto& p : poly) p = t.apply(p);
    return out;
}
const std::vector<std::vector<Vec2>>* Project::frame_for(const Component& c) const {
    if (!instanced(c) && !panel_frame.empty()) return &panel_frame;
    if (!instanced(c) && is_panel()) return nullptr;   // panel without panel frame: unknown extent
    return board_frame.empty() ? nullptr : &board_frame;
}

std::vector<std::vector<Vec2>> join_frame_segments(const std::vector<std::vector<Vec2>>& in, double tol) {
    std::vector<std::vector<Vec2>> out;
    std::vector<std::vector<Vec2>> open;
    auto closed = [&](const std::vector<Vec2>& p) { return p.size() > 2 && dist(p.front(), p.back()) <= tol; };
    for (auto& p : in) {
        if (p.size() < 2) continue;
        if (closed(p)) out.push_back(p); else open.push_back(p);
    }
    std::vector<bool> used(open.size(), false);
    for (size_t i = 0; i < open.size(); ++i) {
        if (used[i]) continue;
        used[i] = true;
        std::vector<Vec2> ch = open[i];
        for (bool grew = true; grew && !closed(ch);) {
            grew = false;
            for (size_t j = 0; j < open.size(); ++j) {
                if (used[j]) continue;
                const auto& s = open[j];
                if (dist(ch.back(), s.front()) <= tol) ch.insert(ch.end(), s.begin() + 1, s.end());
                else if (dist(ch.back(), s.back()) <= tol) ch.insert(ch.end(), s.rbegin() + 1, s.rend());
                else if (dist(ch.front(), s.back()) <= tol) ch.insert(ch.begin(), s.begin(), s.end() - 1);
                else if (dist(ch.front(), s.front()) <= tol) ch.insert(ch.begin(), s.rbegin(), s.rend() - 1);
                else continue;
                used[j] = true; grew = true;
                break;
            }
        }
        if (closed(ch)) ch.back() = ch.front();
        out.push_back(ch);
    }
    return out;
}

bool point_in_frame(const std::vector<std::vector<Vec2>>& frame, Vec2 p) {
    bool in = false;
    for (auto& poly : frame) {
        if (poly.size() < 3 || dist(poly.front(), poly.back()) > 0.05) continue;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const Vec2& a = poly[i];
            const Vec2& b = poly[j];
            if (((a.y > p.y) != (b.y > p.y)) && (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)) in = !in;
        }
    }
    return in;
}

BBox Project::board_bbox() const {
    BBox b;
    for (auto& poly : board_frame) for (auto& p : poly) b.add(p);
    if (b.empty()) {
        for (auto& c : comps) b.add(comp_bbox(c));
        b.inflate(2.0);
    }
    if (b.empty()) { b.add({0, 0}); b.add({100, 80}); }
    return b;
}

void Project::link_bom() {
    for (auto& c : comps) {
        if (c.package.empty()) {
            auto it = parts.find(c.number);
            if (it != parts.end()) c.package = it->second.package;
        }
    }
}

int Project::point_index(const std::string& name) const {
    for (size_t i = 0; i < model.points.size(); ++i)
        if (ieq(model.points[i].name, name)) return (int)i;
    return -1;
}

static bool in_list(const std::vector<std::string>& v, const std::string& s) {
    for (auto& x : v) if (ieq(x, s)) return true;
    return false;
}

int Project::auto_assign() {
    int n = 0;
    for (auto& c : comps) {
        if (c.feature != Feature::Component || c.assigned_point >= 0) continue;
        const PartNumber* pn = part_of(c);
        const std::string proc = pn ? pn->default_process : std::string();
        // pass 0 prefers points on the component's side (Top SMT / Bottom SMT); pass 1 = any side (legacy models)
        for (int pass = 0; pass < 2 && c.assigned_point < 0; ++pass)
        for (size_t i = 0; i < model.points.size(); ++i) {
            const ModelPoint& mp = model.points[i];
            if (pass == 0 && mp.side != c.side) continue;
            bool hit = false;
            if (!proc.empty() && in_list(mp.filter_process, proc)) hit = true;
            if (!hit && !c.number.empty() && in_list(mp.filter_numbers, c.number)) hit = true;
            if (!hit && !c.package.empty() && in_list(mp.filter_packages, c.package)) hit = true;
            if (!hit && !proc.empty() && ieq(mp.name, proc)) hit = true;
            if (hit) { c.assigned_point = (int)i; ++n; break; }
        }
    }
    if (n) dirty = true;
    return n;
}

void Project::make_all_like_root() {
    // Single-image model: all components are stored once (root image) and
    // replicated by the panel transform, so assignments are inherently shared.
    dirty = true;
}

// ---------------------------------------------------------------------------
// Ref lists
// ---------------------------------------------------------------------------
static void split_alpha_num(const std::string& s, std::string& pre, long& num, std::string& post, bool& has_num) {
    size_t i = 0;
    while (i < s.size() && !std::isdigit((unsigned char)s[i])) ++i;
    pre = s.substr(0, i);
    size_t j = i;
    while (j < s.size() && std::isdigit((unsigned char)s[j])) ++j;
    has_num = j > i;
    num = has_num ? std::strtol(s.substr(i, j - i).c_str(), nullptr, 10) : 0;
    post = s.substr(j);
}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> expand_ref_list(const std::string& s) {
    std::vector<std::string> out;
    std::string tok;
    std::vector<std::string> toks;
    for (char ch : s) {
        if (ch == ',' || ch == ';' || ch == ' ' || ch == '\t') { if (!trim(tok).empty()) toks.push_back(trim(tok)); tok.clear(); }
        else tok += ch;
    }
    if (!trim(tok).empty()) toks.push_back(trim(tok));
    // re-join "R1 - R5" style split by spaces
    std::vector<std::string> merged;
    for (size_t i = 0; i < toks.size(); ++i) {
        if (toks[i] == "-" && !merged.empty() && i + 1 < toks.size()) { merged.back() += "-" + toks[++i]; continue; }
        if (!toks[i].empty() && toks[i].back() == '-' && i + 1 < toks.size()) { merged.push_back(toks[i] + toks[i + 1]); ++i; continue; }
        if (!toks[i].empty() && toks[i][0] == '-' && !merged.empty()) { merged.back() += toks[i]; continue; }
        merged.push_back(toks[i]);
    }
    for (auto& t : merged) {
        size_t dash = t.find('-', 1);
        if (dash == std::string::npos) { out.push_back(t); continue; }
        std::string a = t.substr(0, dash), b = t.substr(dash + 1);
        std::string pa, sa, pb, sb; long na, nb; bool ha, hb;
        split_alpha_num(a, pa, na, sa, ha);
        split_alpha_num(b, pb, nb, sb, hb);
        if (!ha || !hb || !sa.empty() || !sb.empty() || (!pb.empty() && !ieq(pa, pb)) || nb < na || nb - na > 5000) {
            out.push_back(t);   // not a range (e.g. part like "U1-A")
            continue;
        }
        for (long k = na; k <= nb; ++k) out.push_back(pa + std::to_string(k));
    }
    return out;
}

bool ref_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[j];
        if (std::isdigit(ca) && std::isdigit(cb)) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit((unsigned char)a[i2])) ++i2;
            while (j2 < b.size() && std::isdigit((unsigned char)b[j2])) ++j2;
            long na = std::strtol(a.substr(i, i2 - i).c_str(), nullptr, 10);
            long nb = std::strtol(b.substr(j, j2 - j).c_str(), nullptr, 10);
            if (na != nb) return na < nb;
            i = i2; j = j2;
        } else {
            int x = std::toupper(ca), y = std::toupper(cb);
            if (x != y) return x < y;
            ++i; ++j;
        }
    }
    return (a.size() - i) < (b.size() - j);
}

std::string units_suffix(Units u) {
    switch (u) { case Units::MM: return "mm"; case Units::Inch: return "in"; default: return "mil"; }
}
double to_display(double mm, Units u) {
    switch (u) { case Units::MM: return mm; case Units::Inch: return mm / kMmPerInch; default: return mm / kMmPerMil; }
}
double from_display(double v, Units u) {
    switch (u) { case Units::MM: return v; case Units::Inch: return v * kMmPerInch; default: return v * kMmPerMil; }
}


// ---------------------------------------------------------------------------
// Lead geometry, orientation hints, marker geometry (moved from package_edit.cpp, R15)
// ---------------------------------------------------------------------------
int pkg_pin1(const Package& p) {
    for (size_t i = 0; i < p.pads.size(); ++i) if (p.pads[i].pin == 1) return (int)i;
    return -1;
}
Vec2 pkg_lead_centroid(const Package& p) {
    if (p.pads.empty()) return {};
    Vec2 s{};
    for (auto& d : p.pads) s += d.pos;
    return s * (1.0 / p.pads.size());
}
Vec2 pkg_body_centre(const Package& p) {
    BBox b = gprims_bbox(p.body);
    if (b.empty()) for (auto& d : p.pads) { b.add(d.pos - Vec2{d.w / 2, d.h / 2}); b.add(d.pos + Vec2{d.w / 2, d.h / 2}); }
    return b.empty() ? Vec2{} : b.center();
}

static bool pattern_maps(const Package& p, Vec2 c, double deg, double tol) {
    for (auto& a : p.pads) {
        Vec2 r = rotate_about(a.pos, c, deg);
        bool hit = false;
        for (auto& b : p.pads) if (dist(r, b.pos) < tol) { hit = true; break; }
        if (!hit) return false;
    }
    return true;
}

int lead_symmetry_deg(const Package& p, double tol) {
    if (p.pads.size() < 2) return 360;
    const Vec2 c = pkg_lead_centroid(p);
    if (pattern_maps(p, c, 90, tol)) return 90;
    if (pattern_maps(p, c, 180, tol)) return 180;
    return 360;
}

static std::string upper_alnum(const std::string& s) {
    std::string u;
    for (char ch : s) u += (char)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch);
    return u;
}
static bool has_tok(const std::string& u, const char* t) {
    // token at start or after a non-letter, so "SOT" matches "SOT-23", "PKG_SOT23" but not "ISOTOPE"
    const size_t n = std::strlen(t);
    for (size_t at = u.find(t); at != std::string::npos; at = u.find(t, at + 1))
        if (at == 0 || !(u[at - 1] >= 'A' && u[at - 1] <= 'Z')) return true;
    (void)n;
    return false;
}

int pkg_name_orientation_hint(const std::string& name) {
    const std::string u = upper_alnum(name);
    static const char* polar[] = {"SOD", "DIODE", "LED", "ELEC", "ELCO", "ECAP", "CPOL", "CAP_POL", "CAPPOL", "POLCAP", "TANT", "TAN_", "CASE_A", "CASE_B",
                                  "CASE_C", "CASE_D", "SMA", "SMB", "SMC", "DO-", "DO2", "MINIMELF", "ZENER", "SCHOTTKY", "CP_", "PANASONIC_",
                                  // R18c: IPC-7351 class prefixes (DIOC / DIOM / DIOMELF / DIOSMA, LEDC / LEDM, CAPMP tantalum,
                                  // CAPAE aluminium electrolytic, CAPPR radial polarized). "MELF" alone is NOT polarized (RESMELF).
                                  "DIO", "LEDC", "LEDM", "CAPMP", "CAPAE", "CAPPR"};
    static const char* orient[] = {"QFP", "LQFP", "TQFP", "PQFP", "QFN", "DFN", "VQFN", "WQFN", "SON", "SOIC", "SOP", "SSOP", "TSSOP", "MSOP", "VSSOP",
                                   "SO-", "SO8", "SO14", "SO16", "SOT", "SC70", "SC-70", "TSOT", "BGA", "LGA", "CSP", "WLCSP", "PLCC", "DPAK", "D2PAK",
                                   "TO-", "TO2", "DIP", "PDIP", "SIP", "HDR", "CONN", "USB", "XTAL4", "ESOP", "HSOP", "POWERPAK", "LFCSP"};
    static const char* plain[] = {"RES", "R0", "C0", "CAP0", "L0", "IND", "FB", "FERRITE", "FUSE", "CHIP",
                                  "RESC", "RESM", "CAPC", "CAPM", "CAPRD", "INDC", "INDM", "INDP", "FUSC", "FUSM", "BEAD", "0201", "0402", "0603", "0805", "1206", "1210", "2512"};
    for (auto t : polar) if (has_tok(u, t)) return 2;
    for (auto t : orient) if (has_tok(u, t)) return 1;
    for (auto t : plain) if (has_tok(u, t)) return -1;
    return 0;
}

static bool two_pads_differ(const Package& p) {
    if (p.pads.size() != 2) return false;
    const Pad &a = p.pads[0], &b = p.pads[1];
    const double ea = a.w * a.h, eb = b.w * b.h;
    return a.shape != b.shape || std::fabs(ea - eb) > 0.15 * std::max(ea, eb);
}

bool pkg_electrically_polarized_guess(const Package& p) {
    if (p.polarized) return true;
    const int h = pkg_name_orientation_hint(p.name);
    if (h == 2) return true;
    return h != -1 && p.radial && p.pads.size() == 2;     // radial 2-lead TH: electrolytic / LED
}

OrientClass orient_class_of(const Package& p) {
    if (p.orient_class >= 1 && p.orient_class <= 3) return (OrientClass)p.orient_class;
    if (p.polarized || p.pin_marked) return OrientClass::Polarized;
    if (p.pads.size() <= 1) return OrientClass::Symmetric;
    const int h = pkg_name_orientation_hint(p.name);
    if (p.pads.size() == 2) {
        if (h == 2 || h == 1 || (h == 0 && two_pads_differ(p))) return OrientClass::Polarized;   // SOD / SMA / tantalum / LED
        return OrientClass::Symmetric;                         // non-polar 2-lead (R/C)
    }
    // >2 leads: if the pads physically fit in several rotations, orientation is ambiguous (QFP / QFN / SOIC / BGA);
    // otherwise only one rotation fits (SOT-23, DPAK) - still orientation-critical
    return lead_symmetry_deg(p) < 360 ? OrientClass::Ambiguous : OrientClass::Polarized;
}

bool pkg_orientation_marking_needed(const Package& p, std::string* why) {
    const OrientClass k = orient_class_of(p);
    const bool need = k == OrientClass::Polarized || k == OrientClass::Ambiguous;
    if (why) {
        if (p.orient_class) *why = need ? "set on this package" : "set on this package (not needed)";
        else if (p.polarized) *why = "electrically polarized";
        else if (p.pads.size() > 2) *why = k == OrientClass::Ambiguous ? "lead pattern fits in several rotations" : "lead pattern fits one way only";
        else if (need) *why = pkg_name_orientation_hint(p.name) > 0 ? "package name (diode / polarized family)" : "unequal pads";
        else *why = "2-lead non-polarized";
    }
    return need;
}

bool pkg_auto_orientation(Package& p) {
    bool ch = false;
    if (!p.polarized && !p.orient_class && pkg_electrically_polarized_guess(p)) { p.polarized = true; ch = true; }
    if (pkg_apply_polarity_default(p)) ch = true;     // R15: polarity marker default (once; never overwrites)
    return ch;
}

MarkerGeom marker_geom(const Package& p) {
    MarkerGeom m;
    const int i1 = pkg_pin1(p);
    const Vec2 c = pkg_lead_centroid(p);
    // nominal size from the package (not one lead): a small fraction of the body, so a QFP and a SOT get
    // comparable dots; the renderer clamps it to kMarkerMin/MaxPx on screen.
    BBox fb;
    for (auto& d : p.pads) { fb.add(d.pos - Vec2{d.w / 2, d.h / 2}); fb.add(d.pos + Vec2{d.w / 2, d.h / 2}); }
    const double span = fb.empty() ? 1.0 : std::min(fb.w(), fb.h());
    if (p.marker_user_pos) { m.valid = true; m.at = p.marker_pos; }
    if (!m.valid && i1 < 0) return m;
    double s = p.marker_size > 0 ? p.marker_size : std::clamp(span * 0.07, 0.15, 0.6);
    m.size = s;
    if (!m.valid) {
        const Pad& d = p.pads[i1];
        m.valid = true;
        if (p.pads.size() <= 2) m.at = d.pos;                 // cathode / pin-1 pad itself
        else {
            // R12 §9: just inside the inner end of the pin-1 lead (between the lead and the body edge), on the
            // axis perpendicular to its row - follows the real pin-1 geometry for SOIC / QFP / SOT; diagonal for grids.
            Vec2 u = c - d.pos;
            const double ax = std::fabs(u.x), ay = std::fabs(u.y);
            const double lx = std::fabs(std::cos(deg2rad(d.rot))) * d.w + std::fabs(std::sin(deg2rad(d.rot))) * d.h;
            const double ly = std::fabs(std::sin(deg2rad(d.rot))) * d.w + std::fabs(std::cos(deg2rad(d.rot))) * d.h;
            Vec2 off;
            // R13 (P1-05, U10 QFP corner pin): an elongated lead points at the body along its long axis, so the
            // marker goes just past its inner end on that axis. Only round / square leads (BGA, LGA) use the
            // pattern-centroid direction (corner pins of a QFP used to get a diagonal dot "in the middle of nowhere").
            if (lx > 1.5 * ly && ax > 1e-9) off = {(u.x > 0 ? 1 : -1) * (lx / 2 + s * 1.6), 0};
            else if (ly > 1.5 * lx && ay > 1e-9) off = {0, (u.y > 0 ? 1 : -1) * (ly / 2 + s * 1.6)};
            else if (ax > 1.8 * ay) off = {(u.x > 0 ? 1 : -1) * (lx / 2 + s * 1.6), 0};
            else if (ay > 1.8 * ax) off = {0, (u.y > 0 ? 1 : -1) * (ly / 2 + s * 1.6)};
            else { const double L = std::hypot(u.x, u.y); off = L > 1e-9 ? u * ((std::max(lx, ly) / 2 + s * 1.6) / L) : Vec2{}; }
            // never past the centroid on tiny parts
            if (std::hypot(off.x, off.y) > 0.8 * std::hypot(u.x, u.y)) off = u * 0.5;
            m.at = d.pos + off;
        }
    }
    Vec2 dv = (i1 >= 0 ? p.pads[i1].pos : m.at) - c;
    const double L = std::hypot(dv.x, dv.y);
    m.dir = L > 1e-9 ? dv * (1.0 / L) : Vec2{-1, 0};
    return m;
}

// ---------------------------------------------------------------------------
// Polarity marker (R15, specs/POLARITY.md)
// One concept: a package either has a polarity marker (a Dot or an Arrow drawn by / for the user, placed to
// line up with the board silkscreen indicator) or it does not. Presence = "this part has orientation" for
// silkscreen verification. Metadata only: machine programs are not affected.
// ---------------------------------------------------------------------------
bool pkg_has_polarity(const Package& p) { return p.marker_kind != (uint8_t)MarkerKind::None; }
bool pkg_body_replaceable(const Package& p) { return p.body.empty() || p.body_source == "pads"; }

static bool name_has_any(const std::string& u, std::initializer_list<const char*> toks) {
    for (auto t : toks) if (has_tok(u, t)) return true;
    return false;
}

int pkg_default_polarity_reason(const Package& p, std::string* why) {
    auto say = [&](const char* s) { if (why) *why = s; };
    const std::string u = upper_alnum(p.name + " " + p.description);
    const int h = std::max(pkg_name_orientation_hint(p.name), pkg_name_orientation_hint(p.description));
    if (p.pads.size() >= 3) {
        // ICs and every asymmetric multi-lead package (QFP, QFN, SOIC, TSOP, BGA, SOT-23, DPAK ...)
        say(h == 1 ? "IC / multi-lead package" : "3 or more leads");
        return 1;
    }
    if (p.pads.size() <= 1) { say("single lead"); return 0; }
    // two terminals: only LEDs, diodes, tantalum / electrolytic capacitors are polarized
    if (name_has_any(u, {"LED", "DIODE", "DIO", "SOD", "TANT", "ZENER", "SCHOTTKY", "ELEC", "ECAP", "CPOL", "POLCAP"}) || h == 2) {
        say("LED / diode / tantalum by name");
        return 1;
    }
    if (p.radial) { say("radial 2-lead (electrolytic / LED)"); return 1; }
    if (h == -1) { say("2-terminal chip (R / C / L)"); return 0; }
    if (two_pads_differ(p)) { say("unequal pads"); return 1; }
    say("2-terminal chip");
    return 0;
}
bool pkg_default_has_polarity(const Package& p) { return pkg_default_polarity_reason(p, nullptr) > 0; }

// auto anchor: near lead 1, or near the first lead when no lead is numbered 1
MarkerGeom pkg_marker_auto(const Package& p) {
    Package t = p;
    t.marker_user_pos = false;
    t.marker_size = 0;
    if (pkg_pin1(t) < 0 && !t.pads.empty()) t.pads[0].pin = 1;
    MarkerGeom g = marker_geom(t);
    if (!g.valid) { g.valid = true; g.at = pkg_body_centre(p); g.size = 0.3; g.dir = {-1, 0}; return g; }
    if (t.pads.size() == 2) {
        // 2-terminal: just outside the outer end of lead 1 (on the lead it would hide the pad / be hard to pick)
        const Pad& d = t.pads[pkg_pin1(t)];
        const double ext = 0.5 * (std::fabs(g.dir.x) * (std::fabs(std::cos(deg2rad(d.rot))) * d.w + std::fabs(std::sin(deg2rad(d.rot))) * d.h) +
                                  std::fabs(g.dir.y) * (std::fabs(std::sin(deg2rad(d.rot))) * d.w + std::fabs(std::cos(deg2rad(d.rot))) * d.h));
        g.at = d.pos + g.dir * (ext + g.size * 1.6);
    }
    return g;
}

bool pkg_add_polarity_marker(Package& p, MarkerKind k) {
    p.polarity_set = true;
    if (pkg_has_polarity(p)) return false;          // never overwrite a marker the user placed
    const MarkerGeom g = pkg_marker_auto(p);
    p.marker_kind = (uint8_t)(k == MarkerKind::None ? MarkerKind::Dot : k);
    p.marker_color = 0xFFE03131;                    // red
    p.marker_user_pos = true;                       // fixed: later lead edits never move it
    p.marker_pos = g.at;
    p.marker_size = 0;
    p.marker_rot = rad2deg(std::atan2(g.dir.y, g.dir.x));
    return true;
}
void pkg_remove_polarity_marker(Package& p) {
    p.polarity_set = true;
    p.marker_kind = (uint8_t)MarkerKind::None;
    p.marker_art.clear();
}

bool pkg_apply_polarity_default(Package& p) {
    if (p.polarity_set) return false;
    // explicit older data (library / plugin packages written before R15): honour it like a project migration
    if (p.marker_kind || p.orient_class) return pkg_migrate_polarity_legacy(p);
    p.polarity_set = true;
    if (pkg_default_has_polarity(p)) pkg_add_polarity_marker(p);
    return true;
}
int prj_apply_polarity_defaults(Project& prj) {
    int n = 0;
    for (auto& kv : prj.packages) if (!kv.second.polarity_set && pkg_apply_polarity_default(kv.second)) ++n;
    return n;
}

// Files written before R15: "Pin-1 mark" (= orientation marking needed, explicit or derived) and "Polarized"
// become a marker at the position the old version drew its automatic pin-1 dot.
bool pkg_migrate_polarity_legacy(Package& p) {
    if (p.polarity_set) return false;
    const bool had = pkg_orientation_marking_needed(p) || p.polarized || p.pin_marked;
    const MarkerKind k = p.marker_kind ? (MarkerKind)p.marker_kind : MarkerKind::Dot;
    if (!had) { p.marker_kind = 0; p.polarity_set = true; return true; }
    MarkerGeom g = marker_geom(p);                  // old auto position (or the user position it had)
    if (!g.valid) g = pkg_marker_auto(p);           // no lead 1: first lead
    p.marker_kind = (uint8_t)k;
    if (!p.marker_color) p.marker_color = 0xFFE03131;
    p.marker_user_pos = true;
    p.marker_pos = g.at;
    p.marker_rot = rad2deg(std::atan2(g.dir.y, g.dir.x));
    p.polarity_set = true;
    return true;
}

PolarityMarkerOnBoard polarity_marker_on_board(const Project& prj, const Component& c, const Package& p, int image) {
    PolarityMarkerOnBoard r;
    if (!pkg_has_polarity(p)) return r;
    const Xform t = prj.comp_xform(c, image);
    r.valid = true;
    r.at = t.apply(p.marker_pos);
    const Vec2 d0{std::cos(deg2rad(p.marker_rot)), std::sin(deg2rad(p.marker_rot))};
    const Vec2 o = t.apply({0, 0}), d1 = t.apply(d0) - o;
    r.rot = norm_deg(rad2deg(std::atan2(d1.y, d1.x)));
    return r;
}


} // namespace pcbp
