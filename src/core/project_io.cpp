// ============================================================================
// project_io.cpp — Project <-> JSON, .pcbp load/save.
// ============================================================================
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <algorithm>
#include "project_io.h"
#include "package_edit.h"
#include "fileio.h"
#include "arrays.h"

namespace pcbp {
// R18d: legacy Production sides -> export-layer links (declared in core/sides.h)
// Move layer `from` to index `to`, keeping component assignments.
static void r18d_move_point(Project& p, int from, int to) {
    auto& v = p.model.points;
    if (from == to) return;
    ModelPoint m = v[from];
    v.erase(v.begin() + from);
    v.insert(v.begin() + to, m);
    for (auto& c : p.comps) {
        int& a = c.assigned_point;
        if (a < 0) continue;
        if (a == from) a = to;
        else if (from < to && a > from && a <= to) --a;
        else if (from > to && a >= to && a < from) ++a;
    }
}
// Rules: (1) not configured / no sides -> nothing. (2) Two sides on two DIFFERENT existing layers that are both
// unlinked -> both get the next free ID; if Side 1's layer is below Side 2's it is moved directly above it, so
// the list order reproduces the stored run order; each layer's Board side is set to the side's physical side
// (Flipped / orientation / origin are left as stored in the layer profile).
// (3) Both sides on ONE layer, a missing layer or a single side -> no link (the layer exports standalone).
// Always: p.mfg is cleared (the block is gone), and the result is noted in mfg.last_validation for the report.
void migrate_sides_to_links(Project& p) {
    if (!p.mfg.configured || p.mfg.sides.empty()) { p.mfg = MfgSides{}; return; }
    std::vector<MfgSide> s = p.mfg.sides;
    std::sort(s.begin(), s.end(), [](const MfgSide& a, const MfgSide& b) { return a.seq < b.seq; });
    p.mfg = MfgSides{};
    if (s.size() != 2) return;
    int a = p.point_index(s[0].point), b = p.point_index(s[1].point);
    if (a < 0 || b < 0 || a == b) return;
    if (p.model.points[a].link || p.model.points[b].link) return;
    if (a > b) { r18d_move_point(p, a, b); a = b; b = b + 1; }
    const int id = [&] { int mx = 0; for (auto& q : p.model.points) mx = std::max(mx, q.link); return mx + 1; }();
    p.model.points[a].link = p.model.points[b].link = id;
    p.model.points[a].machine.side = s[0].physical;
    p.model.points[b].machine.side = s[1].physical;
}

using json::Value;

static Value v2(Vec2 p) { Value a = Value::array(); a.push(p.x); a.push(p.y); return a; }
static Vec2 v2(const Value& v) { return {v[0].num(), v[1].num()}; }
static Value poly(const std::vector<Vec2>& pts) { Value a = Value::array(); for (auto& p : pts) { a.push(p.x); a.push(p.y); } return a; }
static std::vector<Vec2> poly(const Value& v) {
    std::vector<Vec2> r;
    for (size_t i = 0; i + 1 < v.size(); i += 2) r.push_back({v[i].num(), v[i + 1].num()});
    return r;
}
static Value strs(const std::vector<std::string>& s) { Value a = Value::array(); for (auto& x : s) a.push(x); return a; }
static std::vector<std::string> strs(const Value& v) { std::vector<std::string> r; for (auto& x : v.items()) r.push_back(x.str()); return r; }

static const char* kind_name(GKind k) {
    switch (k) {
    case GKind::Line: return "line"; case GKind::Polyline: return "polyline"; case GKind::Polygon: return "polygon";
    case GKind::Rect: return "rect"; case GKind::Circle: return "circle"; case GKind::Arc: return "arc";
    case GKind::Text: return "text"; case GKind::Group: return "group";
    }
    return "line";
}
static GKind kind_from(const std::string& s) {
    if (s == "polyline") return GKind::Polyline; if (s == "polygon") return GKind::Polygon;
    if (s == "rect") return GKind::Rect; if (s == "circle") return GKind::Circle; if (s == "arc") return GKind::Arc;
    if (s == "text") return GKind::Text; if (s == "group") return GKind::Group;
    return GKind::Line;
}

Value gprim_to_json(const GPrim& g) {
    Value o = Value::object();
    o.set("k", kind_name(g.kind));
    if (!g.pts.empty()) o.set("p", poly(g.pts));
    if (g.width) o.set("w", g.width);
    if (g.filled) o.set("fill", true);
    if (g.color) o.set("c", (double)g.color);
    if (g.radius) o.set("r", g.radius);
    if (g.a0 || g.a1) { o.set("a0", g.a0); o.set("a1", g.a1); }
    if (g.rot) o.set("rot", g.rot);
    if (g.text_h) o.set("th", g.text_h);
    if (!g.text.empty()) o.set("t", g.text);
    if (!g.children.empty()) { Value a = Value::array(); for (auto& c : g.children) a.push(gprim_to_json(c)); o.set("ch", a); }
    return o;
}

GPrim gprim_from_json(const Value& v) {
    GPrim g;
    g.kind = kind_from(v["k"].str());
    g.pts = poly(v["p"]);
    g.width = v["w"].num(); g.filled = v["fill"].b(); g.color = (uint32_t)v["c"].num();
    g.radius = v["r"].num(); g.a0 = v["a0"].num(); g.a1 = v["a1"].num(); g.rot = v["rot"].num();
    g.text_h = v["th"].num(); g.text = v["t"].str();
    for (auto& c : v["ch"].items()) g.children.push_back(gprim_from_json(c));
    return g;
}

Value package_to_json(const Package& p) {
    Value o = Value::object();
    o.set("name", p.name);
    o.set("desc", p.description);
    o.set("smt", p.smt);
    o.set("jlead", p.j_lead); o.set("radial", p.radial); o.set("notched", p.notched);
    o.set("pinmarked", p.pin_marked); o.set("polarized", p.polarized); o.set("confirmed", p.shape_confirmed);
    o.set("leads", p.leads); o.set("height", p.height); o.set("process", p.default_process);
    if (!p.height_src.empty()) o.set("h_src", p.height_src);   // R24a
    Value pads = Value::array();
    for (auto& d : p.pads) {
        Value q = Value::object();
        q.set("at", v2(d.pos)); q.set("w", d.w); q.set("h", d.h);
        if (d.rot) q.set("rot", d.rot);
        q.set("shape", (int)d.shape);
        if (d.drill) q.set("drill", d.drill);
        q.set("pin", d.pin);
        pads.push(q);
    }
    o.set("pads", pads);
    Value body = Value::array();
    for (auto& g : p.body) body.push(gprim_to_json(g));
    o.set("body", body);
    if (!p.body_source.empty()) o.set("body_src", p.body_source);
    if (p.orient_class) o.set("orient", (int)p.orient_class);
    if (!p.keep_separate.empty()) { Value k = Value::array(); for (auto& n : p.keep_separate) k.push(Value(n)); o.set("keep_sep", k); }
    if (p.marker_kind) {
        o.set("marker", (int)p.marker_kind); o.set("marker_c", (double)p.marker_color);
        if (p.marker_user_pos) o.set("marker_at", v2(p.marker_pos));
        if (p.marker_size) o.set("marker_sz", p.marker_size);
        if (!p.marker_art.empty()) { Value a = Value::array(); for (auto& g : p.marker_art) a.push(gprim_to_json(g)); o.set("marker_art", a); }
        if (p.marker_rot) o.set("marker_rot", p.marker_rot);
    }
    if (p.polarity_set) o.set("pol", 1);      // R15 polarity-marker model (absent = pre-R15 file -> migrate on load)
    if (p.tape_rot) o.set("tape_rot", p.tape_rot);               // R16b feeder / tape orientation
    if (!p.resolved_by.empty()) o.set("resolved_by", p.resolved_by);
    if (!p.mach.empty() || p.mach_assigned) {   // R24b machine data overrides
        Value m = Value::object();
        const auto& q = p.mach;
        if (q.nozzle) m.set("noz", q.nozzle);
        if (q.tape_w) m.set("tw", q.tape_w);
        if (q.pitch) m.set("pitch", q.pitch);
        if (q.type_code) m.set("type", q.type_code);
        if (!q.packaging.empty()) m.set("pkg", q.packaging);
        if (!q.carrier.empty()) m.set("carrier", q.carrier);
        if (!q.centring.empty()) m.set("centre", q.centring);
        if (q.supply_angle_set) m.set("angle", q.supply_angle);
        if (p.mach_assigned) m.set("assigned", true);
        o.set("mach", m);
    }
    return o;
}

Package package_from_json(const Value& o) {
    Package p;
    p.name = o["name"].str(); p.description = o["desc"].str(); p.smt = o["smt"].b(true);
    p.j_lead = o["jlead"].b(); p.radial = o["radial"].b(); p.notched = o["notched"].b();
    p.pin_marked = o["pinmarked"].b(); p.polarized = o["polarized"].b(); p.shape_confirmed = o["confirmed"].b();
    p.leads = o["leads"].i(); p.height = o["height"].num(); p.default_process = o["process"].str();
    p.height_src = o["h_src"].str();   // R24a
    for (auto& q : o["pads"].items()) {
        Pad d;
        d.pos = v2(q["at"]); d.w = q["w"].num(0.5); d.h = q["h"].num(0.5); d.rot = q["rot"].num();
        d.shape = (PadShape)q["shape"].i(); d.drill = q["drill"].num(); d.pin = q["pin"].i();
        p.pads.push_back(d);
    }
    for (auto& g : o["body"].items()) p.body.push_back(gprim_from_json(g));
    p.body_source = o["body_src"].str();
    p.orient_class = (uint8_t)o["orient"].i();
    for (auto& k : o["keep_sep"].items()) p.keep_separate.push_back(k.str());
    p.marker_kind = (uint8_t)o["marker"].i();
    if (o.has("marker_c")) p.marker_color = (uint32_t)o["marker_c"].num();
    if (o.has("marker_at")) { p.marker_user_pos = true; p.marker_pos = v2(o["marker_at"]); }
    p.marker_size = o["marker_sz"].num();
    for (auto& g : o["marker_art"].items()) p.marker_art.push_back(gprim_from_json(g));
    p.marker_rot = o["marker_rot"].num();
    if (o.has("pol")) p.polarity_set = true;  // R15 (pre-R15 projects are migrated in project_from_json)
    p.tape_rot = ((o["tape_rot"].i() % 360) + 360) % 360 / 90 * 90;   // R16b
    p.resolved_by = o["resolved_by"].str();
    if (o.has("mach")) {   // R24b
        const Value& m = o["mach"];
        p.mach.nozzle = m["noz"].i(); p.mach.tape_w = m["tw"].i(); p.mach.pitch = m["pitch"].i(); p.mach.type_code = m["type"].i();
        p.mach.packaging = m["pkg"].str(); p.mach.carrier = m["carrier"].str(); p.mach.centring = m["centre"].str();
        if (m.has("angle")) { p.mach.supply_angle = m["angle"].num(); p.mach.supply_angle_set = true; }
        p.mach_assigned = m["assigned"].b();
    }
    if (!p.leads) p.leads = (int)p.pads.size();
    return p;
}

static Value part_to_json(const PartNumber& n) {
    Value o = Value::object();
    o.set("number", n.number); o.set("desc", n.description); o.set("package", n.package);
    o.set("polarized", n.polarized); o.set("socket", n.socket);
    o.set("cust_pn", n.cust_pn); o.set("vend_pn", n.vend_pn); o.set("manf_pn", n.manf_pn);
    if (!n.manufacturer.empty()) o.set("manufacturer", n.manufacturer);
    o.set("part_rev", n.part_rev); o.set("soft_ver", n.soft_ver); o.set("stock_code", n.stock_code);
    o.set("bin", n.bin); o.set("line_item", n.line_item); o.set("process", n.default_process);
    o.set("pop_type", n.pop_type); o.set("marking", n.part_marking); o.set("height", n.height); o.set("cost", n.cost);
    if (!n.packaging.empty()) o.set("packaging", n.packaging);                 // R18a
    if (n.supply_angle_set) o.set("supply_angle", n.supply_angle);             // R18a (absent = packaging default)
    if (n.prog_type || n.prog_tape || n.prog_pitch) { o.set("prog_type", n.prog_type); o.set("prog_tape", n.prog_tape); o.set("prog_pitch", n.prog_pitch); }   // R24b
    return o;
}
static PartNumber part_from_json(const Value& o) {
    PartNumber n;
    n.number = o["number"].str(); n.description = o["desc"].str(); n.package = o["package"].str();
    n.polarized = o["polarized"].b(); n.socket = o["socket"].b();
    n.cust_pn = o["cust_pn"].str(); n.vend_pn = o["vend_pn"].str(); n.manf_pn = o["manf_pn"].str();
    n.manufacturer = o["manufacturer"].str();
    n.part_rev = o["part_rev"].str(); n.soft_ver = o["soft_ver"].str(); n.stock_code = o["stock_code"].str();
    n.bin = o["bin"].str(); n.line_item = o["line_item"].str(); n.default_process = o["process"].str();
    n.pop_type = o["pop_type"].str(); n.part_marking = o["marking"].str(); n.height = o["height"].num(); n.cost = o["cost"].num();
    n.packaging = o["packaging"].str();                                        // R18a
    if (o.has("supply_angle")) { n.supply_angle = o["supply_angle"].num(); n.supply_angle_set = true; }
    n.prog_type = o["prog_type"].i(); n.prog_tape = o["prog_tape"].i(); n.prog_pitch = o["prog_pitch"].i();   // R24b
    return n;
}

static Value comp_to_json(const Component& c) {
    Value o = Value::object();
    o.set("id", (double)c.id); o.set("ref", c.ref); o.set("at", v2(c.pos)); o.set("rot", c.rot);
    o.set("side", c.side == Side::Bottom ? "B" : "T");
    o.set("number", c.number); o.set("package", c.package);
    if (c.feature != Feature::Component) o.set("feature", (int)c.feature);
    if (c.feature == Feature::Fiducial) { o.set("fid_scope", (int)c.fid_scope); o.set("fid_no", c.fid_number); o.set("fid_owner", c.local_fid_owner); }
    if (c.dnp) o.set("dnp", true);
    if (c.pin1_set) o.set("pin1", c.pin1_index);
    if (c.label_moved) { o.set("label_off", v2(c.label_off)); o.set("label_rot", c.label_rot); }
    o.set("cad_at", v2(c.cad_pos)); o.set("cad_rot", c.rot_imported);
    if (c.pre_rotation != 0) o.set("pre_rot", c.pre_rotation);
    if (c.has_legacy_mach_rot) o.set("legacy_mach_rot", c.legacy_mach_rot);
    if (c.process != Process::Auto) o.set("process", process_name(c.process));
    if (!c.group_label.empty()) o.set("group", c.group_label);
    if (!c.url.empty()) o.set("url", c.url);
    if (c.bom_state != BomState::NotChecked) o.set("bom_state", (int)c.bom_state);   // R10 (r10b)
    if (c.pn_src != DataSrc::Unknown) o.set("pn_src", (int)c.pn_src);
    if (c.pkg_src != DataSrc::Unknown) o.set("pkg_src", (int)c.pkg_src);
    if (!c.bom_package.empty()) o.set("bom_pkg", c.bom_package);
    if (c.from_gerber) o.set("from_gerber", true);
    if (c.review_ok) o.set("review_ok", true);   // R16c
    o.set("point", c.assigned_point);
    return o;
}
static Component comp_from_json(const Value& o) {
    Component c;
    c.id = (uint32_t)o["id"].num(); c.ref = o["ref"].str(); c.pos = v2(o["at"]); c.rot = o["rot"].num();
    c.rot = norm_deg(c.rot);
    // migration: old "mach_rot" override -> legacy comparison value (never exported; ANGLES.md §6)
    if (o.has("legacy_mach_rot")) { c.has_legacy_mach_rot = true; c.legacy_mach_rot = norm_deg(o["legacy_mach_rot"].num()); }
    else if (o.has("mach_rot")) { c.has_legacy_mach_rot = true; c.legacy_mach_rot = norm_deg(o["mach_rot"].num()); }
    c.side = o["side"].str() == "B" ? Side::Bottom : Side::Top;
    c.number = o["number"].str(); c.package = o["package"].str();
    c.feature = (Feature)o["feature"].i(); c.fid_scope = (FiducialScope)o["fid_scope"].i();
    c.fid_number = o["fid_no"].i(); c.local_fid_owner = o["fid_owner"].i();
    c.dnp = o["dnp"].b();
    if (o.has("pin1")) { c.pin1_set = true; c.pin1_index = o["pin1"].i(); }
    if (o.has("label_off")) { c.label_moved = true; c.label_off = v2(o["label_off"]); c.label_rot = o["label_rot"].num(); }
    c.cad_pos = o.has("cad_at") ? v2(o["cad_at"]) : c.pos; c.rot_imported = norm_deg(o.has("cad_rot") ? o["cad_rot"].num() : c.rot);
    c.pre_rotation = o["pre_rot"].num();
    if (o.has("process")) c.process = process_from_name(o["process"].str());
    c.group_label = o["group"].str();
    c.url = o["url"].str();
    c.bom_state = (BomState)std::clamp(o["bom_state"].i(), 0, 3);   // R10 (r10b)
    c.pn_src = (DataSrc)std::clamp(o["pn_src"].i(), 0, 5);
    c.pkg_src = (DataSrc)std::clamp(o["pkg_src"].i(), 0, 5);
    c.bom_package = o["bom_pkg"].str();
    c.from_gerber = o["from_gerber"].b();
    c.review_ok = o["review_ok"].b();   // R16c
    c.assigned_point = o.has("point") ? o["point"].i(-1) : -1;
    return c;
}

static Value point_to_json(const ModelPoint& m) {
    Value o = Value::object();
    o.set("name", m.name); o.set("process", m.process_type); o.set("kind", (int)m.kind);
    o.set("color", (double)m.color); o.set("side", (int)m.side); o.set("line", m.line);
    o.set("wip", m.wip); o.set("repair", m.repair); o.set("inspection", m.inspection);
    o.set("f_pkg", strs(m.filter_packages)); o.set("f_num", strs(m.filter_numbers)); o.set("f_proc", strs(m.filter_process));
    o.set("notes", m.notes); o.set("va_title", m.visual_aid_title);
    if (m.link > 0) o.set("link", m.link);   // R18d
    o.set("has_machine", m.has_machine);
    if (m.has_machine || !m.machine.options.empty()) {
        const MachineSetup& s = m.machine;
        Value ms = Value::object();
        ms.set("exporter", s.exporter_id); ms.set("dir", s.target_dir); ms.set("file", s.target_file);
        ms.set("orientation", s.orientation); ms.set("flipped", s.flipped); ms.set("side", (int)s.side);
        ms.set("offset", v2(s.hard_offset)); ms.set("force_angles", s.force_angle_calc);
        ms.set("single_board", s.single_board); if (s.mirror_angle_when_flipped) ms.set("mirror_angle", true); ms.set("cad_coords", s.use_cad_coords);
        if (s.has_origin) ms.set("origin", v2(s.origin));
        if (s.no_marks) ms.set("no_marks", true);   // R21
        Value opt = Value::object();
        for (auto& kv : s.options) opt.set(kv.first, kv.second);
        ms.set("options", opt);
        o.set("machine", ms);
    }
    return o;
}
static ModelPoint point_from_json(const Value& o) {
    ModelPoint m;
    m.name = o["name"].str(); m.process_type = o["process"].str(); m.kind = (PointKind)o["kind"].i();
    m.color = (uint32_t)o["color"].num(0xFF3C8CFF); m.side = (Side)o["side"].i(); m.line = o["line"].str();
    m.wip = o["wip"].b(); m.repair = o["repair"].b(); m.inspection = o["inspection"].b();
    m.filter_packages = strs(o["f_pkg"]); m.filter_numbers = strs(o["f_num"]); m.filter_process = strs(o["f_proc"]);
    m.notes = o["notes"].str(); m.visual_aid_title = o["va_title"].str();
    m.link = std::max(0, (int)o["link"].num());   // R18d
    const Value& ms = o["machine"];
    if (ms.is_object()) {
        m.has_machine = o.has("has_machine") ? o["has_machine"].b() : true;
        MachineSetup& s = m.machine;
        s.exporter_id = ms["exporter"].str(); s.target_dir = ms["dir"].str(); s.target_file = ms["file"].str();
        s.orientation = ms["orientation"].i(); s.flipped = ms["flipped"].b(); s.side = (Side)ms["side"].i();
        s.hard_offset = v2(ms["offset"]); s.force_angle_calc = ms["force_angles"].b(true);
        s.single_board = ms["single_board"].b(); s.mirror_angle_when_flipped = ms["mirror_angle"].b(); s.use_cad_coords = ms["cad_coords"].b();
        if (ms.has("origin")) { s.has_origin = true; s.origin = v2(ms["origin"]); }
        s.no_marks = ms["no_marks"].b();   // R21
        for (auto& kv : ms["options"].members()) s.options[kv.first] = kv.second.str();
    }
    return m;
}

Value project_to_json(const Project& p, bool include_layers_content) {
    Value o = Value::object();
    o.set("format", "PCBPlotter.Project");
    o.set("version", kProjectFormatVersion);
    Value info = Value::object();
    const AssemblyInfo& a = p.info;
    info.set("name", a.name); info.set("number", a.number); info.set("description", a.description);
    info.set("originator", a.originator); info.set("bom_number", a.bom_number);
    info.set("assembly_rev", a.assembly_rev); info.set("bom_rev", a.bom_rev); info.set("manf_rev", a.manf_rev);
    info.set("standard", a.standard); info.set("customer", a.customer); info.set("url", a.url);
    if (!a.pcb_id.empty()) info.set("pcb_id", a.pcb_id);
    Value cu = Value::array(); for (auto& c : a.custom) cu.push(c); info.set("custom", cu);
    info.set("created", a.created); info.set("modified", a.modified); info.set("original_file", a.original_filename);
    o.set("assembly", info);
    o.set("units", (int)p.display_units);
    o.set("origin", v2(p.virtual_origin));
    Value bf = Value::array(); for (auto& pl : p.board_frame) bf.push(poly(pl)); o.set("board_frame", bf);
    if (p.board_frame_from_gerber) o.set("board_frame_from_gerber", true);
    if (p.panel_frame_from_gerber) o.set("panel_frame_from_gerber", true);
    if (p.plot_aligned) o.set("plot_aligned", true);   // R16b
    {   // R16b package acquisition wizard state (resume across sessions)
        const auto& w = p.pkg_wizard;
        if (w.started || !w.done.empty() || !w.skipped.empty()) {
            Value q = Value::object();
            Value dn = Value::array(); for (auto& n : w.done) dn.push(Value(n)); q.set("done", dn);
            Value sk = Value::array(); for (auto& n : w.skipped) sk.push(Value(n)); q.set("skipped", sk);
            q.set("current", w.current); q.set("placement", w.placement); q.set("started", w.started);
            o.set("pkg_wizard", q);
        }
    }
    o.set("board_outline_src", outline_source_name(p.board_outline_src));   // R16 outline provenance
    o.set("panel_outline_src", outline_source_name(p.panel_outline_src));
    if (p.outline_warn_off) o.set("outline_warn_off", true);   // R17b inferred-outline acknowledgement
    if (p.tape_rot_notice_ack) o.set("tape_rot_ack", true);   // R18a
    if (!p.outline_ack_box.empty()) { Value ab = Value::array(); ab.push(p.outline_ack_box.x0); ab.push(p.outline_ack_box.y0); ab.push(p.outline_ack_box.x1); ab.push(p.outline_ack_box.y1); o.set("outline_ack_box", ab); }
    Value pf = Value::array(); for (auto& pl : p.panel_frame) pf.push(poly(pl)); o.set("panel_frame", pf);
    o.set("frame_color", (double)p.frame_color);
    Value pk = Value::array(); for (auto& kv : p.packages) pk.push(package_to_json(kv.second)); o.set("packages", pk); o.set("polarity_model", 1);
    Value pn = Value::array(); for (auto& kv : p.parts) pn.push(part_to_json(kv.second)); o.set("parts", pn);
    Value cs = Value::array(); for (auto& c : p.comps) cs.push(comp_to_json(c)); o.set("components", cs);
    Value im = Value::array();
    for (auto& i : p.images) {
        Value q = Value::object();
        q.set("name", i.name); q.set("offset", v2(i.offset)); q.set("rot", i.rot); q.set("root", i.root);
        q.set("xout", i.x_out); q.set("xi", i.x_index); q.set("yi", i.y_index);
        im.push(q);
    }
    o.set("images", im);
    o.set("arrays_model", 1);   // R22: reference circuit + linked instances, no generated outlines (arrays.h migration)
    {   // R22 Board > Panelize dialog parameters (OK saves them without applying)
        const auto& a = p.array_params;
        if (a.nl_saved || a.lin_saved) {
            Value q = Value::object();
            if (a.nl_saved) {
                Value cs = Value::array();
                for (auto& c : a.nl_cells) { Value e = Value::object(); e.set("x", c.x); e.set("y", c.y); e.set("rot", c.rot); if (c.root) e.set("root", true); if (c.x_out) e.set("xout", true); cs.push(e); }
                q.set("nl_cells", cs);
                Value gs = Value::array(); for (auto& g : a.nl_groups) for (double v : g) gs.push(v); q.set("nl_groups", gs);
                q.set("nl_units", (int)a.nl_units); q.set("nl_ref", a.nl_ref);
            }
            if (a.lin_saved) {
                q.set("lin_nx", a.lin_nx); q.set("lin_ny", a.lin_ny); q.set("lin_rc", a.lin_root_col); q.set("lin_rr", a.lin_root_row);
                q.set("lin_px", a.lin_pitch_x); q.set("lin_py", a.lin_pitch_y); q.set("lin_units", (int)a.lin_units);
            }
            o.set("array_params", q);
        }
    }
    Value ly = Value::array();
    for (auto& l : p.layers) {
        Value q = Value::object();
        q.set("name", l.name); q.set("path", l.path);
        if (include_layers_content) { q.set("content", l.content); q.set("apertures", l.aperture_list); }
        q.set("role", (int)l.role); q.set("side", (int)l.side); q.set("color", (double)l.color);
        q.set("visible", l.visible); q.set("digitize", l.digitize); q.set("offset", v2(l.offset));
        q.set("rot", l.rot); q.set("mirror", l.mirror); q.set("format", l.format); q.set("ap_units", l.units_hint);
        Value hd = Value::array(); for (int d : l.hidden_dcodes) hd.push(d); q.set("hidden", hd);
        ly.push(q);
    }
    o.set("layers", ly);
    Value fm = Value::object();
    fm.set("name", p.model.name); fm.set("description", p.model.description);
    Value pts = Value::array(); for (auto& m : p.model.points) pts.push(point_to_json(m)); fm.set("points", pts);
    if (!p.model.export_dir.empty()) fm.set("export_dir", p.model.export_dir);   // R19a
    o.set("factory_model", fm);
    o.set("revision_notes", strs(p.revision_notes));
    if (!p.project_notes.empty()) o.set("project_notes", p.project_notes);
    if (!p.history.empty()) {
        Value h = Value::array();
        for (auto& r : p.history) {
            Value e = Value::object();
            e.set("time", r.time); e.set("kind", r.kind); e.set("rev", r.rev); e.set("summary", r.summary);
            e.set("comment", r.comment); e.set("program_file", r.program_file); e.set("program_hash", r.program_hash); e.set("author", r.author);
            if (!r.snapshot.empty()) { Value sv; std::string se; if (Value::parse(r.snapshot, sv, se)) e.set("snapshot", sv); }   // R17d
            h.push(e);
        }
        o.set("history", h);
    }
    if (!p.selection_sets.empty()) o.set("selection_sets", selection_sets_to_json(p.selection_sets));
    if (!p.documents.empty() || !p.documents_archive.empty()) {   // R19b job-archive index (never the bytes)
        Value dv = Value::object(), da = Value::array();
        if (!p.documents_archive.empty()) dv.set("archive", p.documents_archive);
        for (auto& r : p.documents) {
            Value e = Value::object();
            e.set("path", r.path); e.set("size", (double)r.size); e.set("crc", (double)r.crc);
            if (!r.category.empty()) e.set("category", r.category);
            if (!r.tags.empty()) { Value t = Value::array(); for (auto& x : r.tags) t.push(x); e.set("tags", t); }
            if (!r.note.empty()) e.set("note", r.note);
            da.push(e);
        }
        dv.set("items", da);
        o.set("documents", dv);
    }
    if (p.mfg.configured || !p.mfg.sides.empty()) {
        Value m = Value::object(), a = Value::array();
        m.set("configured", p.mfg.configured);
        for (auto& sd : p.mfg.sides) {
            Value e = Value::object();
            e.set("seq", sd.seq); e.set("physical", sd.physical == Side::Bottom ? "bottom" : "top");
            e.set("point", sd.point); e.set("suffix", sd.suffix);
            a.push(e);
        }
        m.set("sides", a);
        if (!p.mfg.last_validation.empty()) m.set("last_validation", p.mfg.last_validation);
        o.set("mfg_sides", m);
    }
    if (!p.sources.empty()) {
        Value a = Value::array();
        for (auto& sr : p.sources) {
            Value e = Value::object();
            e.set("path", sr.path); e.set("importer", sr.importer); e.set("at", sr.imported_at); e.set("summary", sr.summary); if (!sr.result.empty()) e.set("result", sr.result);
            Value st = Value::object(); for (auto& kv : sr.settings) st.set(kv.first, kv.second); e.set("settings", st);
            e.set("warnings", strs(sr.warnings));
            a.push(e);
        }
        o.set("sources", a);
    }
    if (!p.board_images.empty()) {
        Value a = Value::array();
        for (auto& bi : p.board_images) {
            Value e = Value::object();
            e.set("name", bi.name); e.set("path", bi.path); e.set("embed", bi.embed);
            if (bi.embed && include_layers_content && !bi.data.empty()) e.set("data", base64_encode(bi.data));
            e.set("w", bi.px_w); e.set("h", bi.px_h);
            Value h = Value::array(); for (double v : bi.H) h.push(v); e.set("H", h);
            e.set("calib", bi.calib); e.set("mirrored", bi.mirrored);
            Value pr = Value::array();
            for (auto& q : bi.pairs) { Value x = Value::array(); x.push(q.img.x); x.push(q.img.y); x.push(q.board.x); x.push(q.board.y); pr.push(x); }
            e.set("pairs", pr);
            e.set("opacity", (double)bi.opacity); e.set("visible", bi.visible); e.set("locked", bi.locked);
            e.set("side", bi.side); e.set("rms", bi.rms_mm);
            a.push(e);
        }
        o.set("board_images", a);
    }
    return o;
}

bool project_from_json(const Value& o, Project& p, std::string& err) {
    if (o["format"].str() != "PCBPlotter.Project") { err = "not a PCB Plotter project"; return false; }
    if (o["version"].i() > kProjectFormatVersion) { err = "project was saved by a newer PCB Plotter"; return false; }
    p = Project{};
    const Value& info = o["assembly"];
    AssemblyInfo& a = p.info;
    a.name = info["name"].str(); a.number = info["number"].str(); a.description = info["description"].str();
    a.originator = info["originator"].str(); a.bom_number = info["bom_number"].str();
    a.assembly_rev = info["assembly_rev"].str(); a.bom_rev = info["bom_rev"].str(); a.manf_rev = info["manf_rev"].str();
    a.standard = info["standard"].str(); a.customer = info["customer"].str(); a.url = info["url"].str();
    a.pcb_id = info["pcb_id"].str();
    for (int i = 0; i < 4; ++i) a.custom[i] = info["custom"][i].str();
    a.created = info["created"].str(); a.modified = info["modified"].str(); a.original_filename = info["original_file"].str();
    p.display_units = (Units)o["units"].i(1);
    p.virtual_origin = v2(o["origin"]);
    for (auto& pl : o["board_frame"].items()) p.board_frame.push_back(poly(pl));
    for (auto& pl : o["panel_frame"].items()) p.panel_frame.push_back(poly(pl));
    p.board_frame_from_gerber = o["board_frame_from_gerber"].b();
    p.panel_frame_from_gerber = o["panel_frame_from_gerber"].b();
    p.plot_aligned = o["plot_aligned"].b();   // R16b
    if (o.has("pkg_wizard")) {
        const Value& q = o["pkg_wizard"];
        for (auto& n : q["done"].items()) p.pkg_wizard.done.push_back(n.str());
        for (auto& n : q["skipped"].items()) p.pkg_wizard.skipped.push_back(n.str());
        p.pkg_wizard.current = q["current"].str(); p.pkg_wizard.placement = q["placement"].i(); p.pkg_wizard.started = q["started"].b();
    }
    {   // R16 (specs/OUTLINES.md): outline provenance; older projects: an outline equal to the placement
        // extents was auto-generated -> Inferred, anything else -> Confirmed
        auto src = [](const std::string& t) { return t == "Inferred" ? OutlineSource::Inferred : t == "Imported" ? OutlineSource::Imported : OutlineSource::Confirmed; };
        if (o.has("board_outline_src")) p.board_outline_src = src(o["board_outline_src"].str());
        if (o.has("panel_outline_src")) p.panel_outline_src = src(o["panel_outline_src"].str());
        p.outline_warn_off = o["outline_warn_off"].b();   // R17b
        p.tape_rot_notice_ack = o["tape_rot_ack"].b();    // R18a
        if (o.has("outline_ack_box")) { const Value& ab = o["outline_ack_box"]; p.outline_ack_box = BBox(ab[0].num(), ab[1].num(), ab[2].num(), ab[3].num()); }
    }
    p.frame_color = (uint32_t)o["frame_color"].num(0xFF2F9E44);
    for (auto& q : o["packages"].items()) { Package k = package_from_json(q); p.packages[k.name] = k; }
    // R15 (specs/POLARITY.md): projects saved before the polarity-marker model: the old pin-1 mark / polarized
    // flags (explicit or automatic) become a marker at the position the old version drew its pin-1 dot
    if (!o.has("polarity_model")) for (auto& kv : p.packages) pkg_migrate_polarity_legacy(kv.second);
    for (auto& q : o["parts"].items()) { PartNumber n = part_from_json(q); p.parts[n.number] = n; }
    for (auto& q : o["components"].items()) p.add_component(comp_from_json(q));
    if (!o.has("board_outline_src") && p.circuit_frame_is_placement_extent()) p.board_outline_src = OutlineSource::Inferred;   // R16 migration
    p.images.clear();
    for (auto& q : o["images"].items()) {
        PanelImage i;
        i.name = q["name"].str(); i.offset = v2(q["offset"]); i.rot = q["rot"].num(); i.root = q["root"].b();
        i.x_out = q["xout"].b(); i.x_index = q["xi"].i(); i.y_index = q["yi"].i();
        p.images.push_back(i);
    }
    if (p.images.empty()) p.images.push_back(PanelImage{});
    if (o.has("array_params")) {   // R22
        const Value& q = o["array_params"];
        auto& a = p.array_params;
        if (q.has("nl_cells")) {
            a.nl_saved = true;
            for (auto& e : q["nl_cells"].items()) a.nl_cells.push_back({e["x"].i(), e["y"].i(), e["rot"].i(), e["root"].b(), e["xout"].b()});
            int k = 0; for (auto& v : q["nl_groups"].items()) { if (k < 16) a.nl_groups[k / 4][k % 4] = v.num(); ++k; }
            a.nl_units = (Units)std::clamp(q["nl_units"].i(), 0, 2); a.nl_ref = q["nl_ref"].str();
        }
        if (q.has("lin_nx")) {
            a.lin_saved = true;
            a.lin_nx = std::max(1, q["lin_nx"].i(2)); a.lin_ny = std::max(1, q["lin_ny"].i(1));
            a.lin_root_col = std::max(0, q["lin_rc"].i()); a.lin_root_row = std::max(0, q["lin_rr"].i());
            a.lin_pitch_x = q["lin_px"].num(); a.lin_pitch_y = q["lin_py"].num(); a.lin_units = (Units)std::clamp(q["lin_units"].i(), 0, 2);
        }
    }
    for (auto& q : o["layers"].items()) {
        GerberLayerRef l;
        l.name = q["name"].str(); l.path = q["path"].str(); l.content = q["content"].str(); l.aperture_list = q["apertures"].str();
        l.role = (LayerRole)q["role"].i(); l.side = (Side)q["side"].i(); l.color = (uint32_t)q["color"].num(0xFFC08040);
        l.visible = q["visible"].b(true); l.digitize = q["digitize"].b(); l.offset = v2(q["offset"]);
        l.rot = q["rot"].num(); l.mirror = q["mirror"].b(); l.format = q["format"].str(); l.units_hint = q["ap_units"].str();
        for (auto& d : q["hidden"].items()) l.hidden_dcodes.push_back(d.i());
        p.layers.push_back(l);
    }
    const Value& fm = o["factory_model"];
    if (fm.is_object()) {
        p.model.name = fm["name"].str(); p.model.description = fm["description"].str();
        p.model.points.clear();
        for (auto& q : fm["points"].items()) p.model.points.push_back(point_from_json(q));
        p.model.export_dir = fm["export_dir"].str();
        p.model.migrate_export_dir();   // R19a
    } else {
        p.model = FactoryModel::make_legacy_default();   // legacy file without a model: keep pre-R10 default
    }
    p.revision_notes = strs(o["revision_notes"]);
    p.project_notes = o["project_notes"].str();
    for (auto& e : o["history"].items()) {
        RevisionEntry r;
        r.time = e["time"].str(); r.kind = e["kind"].str(); r.rev = e["rev"].str(); r.summary = e["summary"].str();
        r.comment = e["comment"].str(); r.program_file = e["program_file"].str(); r.program_hash = e["program_hash"].str(); r.author = e["author"].str();
        if (e["snapshot"].is_object()) r.snapshot = e["snapshot"].dump(-1);   // R17d restorable checkpoint
        p.history.push_back(r);
    }
    p.selection_sets = selection_sets_from_json(o["selection_sets"]);
    p.documents.clear(); p.documents_archive = o["documents"]["archive"].str();
    for (auto& e : o["documents"]["items"].items()) {
        DocumentRef r;
        r.path = e["path"].str(); r.size = (uint64_t)e["size"].num(); r.crc = (uint32_t)e["crc"].num();
        r.category = e["category"].str(); r.note = e["note"].str();
        for (auto& t : e["tags"].items()) r.tags.push_back(t.str());
        if (!r.path.empty()) p.documents.push_back(r);
    }
    {
        const Value& m = o["mfg_sides"];
        p.mfg = MfgSides{};
        p.mfg.configured = m["configured"].b();
        for (auto& e : m["sides"].items()) {
            MfgSide sd;
            sd.seq = (int)e["seq"].num(); if (sd.seq < 1) sd.seq = (int)p.mfg.sides.size() + 1;
            sd.physical = e["physical"].str() == "top" ? Side::Top : Side::Bottom;
            sd.point = e["point"].str(); sd.suffix = e["suffix"].str();
            p.mfg.sides.push_back(sd);
        }
        p.mfg.last_validation = m["last_validation"].str();
        migrate_sides_to_links(p);   // R18d: production-sides config -> export-layer links (defined above)
    }
    for (auto& e : o["sources"].items()) {
        Project::SourceRef sr;
        sr.path = e["path"].str(); sr.importer = e["importer"].str(); sr.imported_at = e["at"].str(); sr.summary = e["summary"].str(); sr.result = e["result"].str();
        for (auto& kv : e["settings"].members()) sr.settings[kv.first] = kv.second.str();
        sr.warnings = strs(e["warnings"]);
        p.sources.push_back(sr);
    }
    for (auto& e : o["board_images"].items()) {
        BoardImage bi;
        bi.name = e["name"].str(); bi.path = e["path"].str(); bi.embed = e["embed"].b(true);
        if (e.has("data")) base64_decode(e["data"].str(), bi.data);
        bi.px_w = e["w"].i(); bi.px_h = e["h"].i();
        for (int k = 0; k < 9; ++k) bi.H[k] = e["H"][k].num(k % 4 == 0 ? 1.0 : 0.0);
        bi.calib = e["calib"].i(); bi.mirrored = e["mirrored"].b();
        for (auto& q : e["pairs"].items()) bi.pairs.push_back({{q[0].num(), q[1].num()}, {q[2].num(), q[3].num()}});
        bi.opacity = (float)e["opacity"].num(0.6); bi.visible = e["visible"].b(true); bi.locked = e["locked"].b();
        bi.side = e["side"].i(); bi.rms_mm = e["rms"].num();
        p.board_images.push_back(std::move(bi));
    }
    p.dirty = false;
    if (!o.has("arrays_model")) {   // R22 migration: demote outlines older versions generated (arrays.h)
        p.migration_notes = migrate_array_outlines(p);
        p.dirty = !p.migration_notes.empty();
    }
    return true;
}

// R12 (REVIEW_V11 §4): temp file in the same folder + flush + atomic replace. An interrupted save never leaves a
// truncated project: either the old file or the complete new one exists.
bool atomic_write_file(const std::string& path, const void* data, size_t n, std::string& err) {
#ifdef _WIN32
    auto wide = [](const std::string& u) {
        std::wstring w(MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), nullptr, 0), L'\0');
        if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), w.data(), (int)w.size());
        return w;
    };
    const std::wstring wp = wide(path);
    const std::wstring tmp = wp + L".tmp" + std::to_wstring(GetCurrentProcessId());
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "Cannot create " + path + ".tmp (error " + std::to_string(GetLastError()) + ")"; return false; }
    const char* q = (const char*)data;
    size_t left = n;
    bool ok = true;
    while (left && ok) {
        DWORD w = 0, chunk = (DWORD)std::min<size_t>(left, 1u << 24);
        ok = WriteFile(h, q, chunk, &w, nullptr) && w == chunk;
        q += w; left -= w;
    }
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); err = "Write failed: " + path + " (disk full?)"; return false; }
    if (!MoveFileExW(tmp.c_str(), wp.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD e = GetLastError();
        DeleteFileW(tmp.c_str());
        err = "Cannot replace " + path + " (error " + std::to_string(e) + "; is it open in another program?)";
        return false;
    }
    return true;
#else
    const std::string tmp = path + ".tmp" + std::to_string((long)getpid());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { err = "Cannot create " + tmp; return false; }
    const char* q = (const char*)data;
    size_t left = n;
    while (left) { ssize_t w = ::write(fd, q, left); if (w <= 0) { ::close(fd); ::unlink(tmp.c_str()); err = "Write failed: " + path; return false; } q += w; left -= (size_t)w; }
    bool ok = ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); err = "Cannot replace " + path; return false; }
    return true;
#endif
}

bool save_project(const Project& p, const std::string& path, std::string& err) {
    std::string s = project_to_json(p).dump(1);
    return atomic_write_file(path, s.data(), s.size(), err);
}

bool load_project(const std::string& path, Project& p, std::string& err) {
    std::string text;
    if (!read_text_file(path, text, err)) return false;
    Value v;
    if (!Value::parse(text, v, err)) return false;
    if (!project_from_json(v, p, err)) return false;
    p.path = path;
    return true;
}

} // namespace pcbp
