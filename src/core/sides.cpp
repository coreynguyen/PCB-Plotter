// sides.cpp — manufacturing sides + per-side export bundle (see sides.h)
#include "sides.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace pcbp {

SideCounts side_counts(const Project& p) {
    SideCounts n;
    for (auto& c : p.comps)
        if (c.feature == Feature::Component) (c.side == Side::Bottom ? n.bottom : n.top)++;
    return n;
}
bool is_two_sided(const Project& p) { SideCounts n = side_counts(p); return n.top > 0 && n.bottom > 0; }
const char* side_name(Side s) { return s == Side::Bottom ? "Bottom" : "Top"; }
std::string mfg_suffix(const MfgSide& s) { return s.suffix.empty() ? "[Side " + std::to_string(s.seq) + "]" : s.suffix; }
std::string mfg_label(const MfgSide& s) { return "Side " + std::to_string(s.seq) + " (" + side_name(s.physical) + ")"; }
std::string job_base_name(const Project& p) {
    std::string s = project_program_name(p);
    return s.empty() ? std::string("program") : s;
}
std::string sanitize_file_part(const std::string& s) {
    std::string o;
    for (char ch : s) o += (std::string("\\/:*?\"<>|").find(ch) != std::string::npos || (unsigned char)ch < 32) ? '_' : ch;
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    return o;
}
std::string side_file_base(const Project& p, const MfgSide& s) {
    return sanitize_file_part(job_base_name(p) + " " + mfg_suffix(s));
}
const char* suggest_hint() {
    return "Suggestion only: many lines run the lighter BOTTOM side first so its parts survive the\n"
           "second reflow upside down. The correct order depends on solder paste vs. adhesive,\n"
           "component weight, and later operations (wave, hand load, press-fit). Choose deliberately.";
}

int side_point_index(const Project& p, const MfgSide& s) { return s.point.empty() ? -1 : p.point_index(s.point); }
const MfgSide* side_by_seq(const Project& p, int seq) {
    for (auto& s : p.mfg.sides) if (s.seq == seq) return &s;
    return nullptr;
}

static std::string unique_point_name(const Project& p, const std::string& base) {
    std::string n = base;
    for (int i = 2; p.point_index(n) >= 0; ++i) n = base + " " + std::to_string(i);
    return n;
}

void setup_sides(Project& p, bool bottom_first, int point, bool separate) {
    p.mfg.sides.clear();
    const Side first = bottom_first ? Side::Bottom : Side::Top;
    const Side second = bottom_first ? Side::Top : Side::Bottom;
    if (point < 0 || point >= (int)p.model.points.size()) {
        point = -1;
        for (size_t i = 0; i < p.model.points.size(); ++i)
            if (p.model.points[i].has_machine && p.model.points[i].kind == PointKind::Placement) { point = (int)i; break; }
    }
    MfgSide a{1, first, point >= 0 ? p.model.points[point].name : std::string(), {}};
    MfgSide b{2, second, a.point, {}};
    const bool two = is_two_sided(p);
    if (point >= 0 && separate && two) {
        ModelPoint clone = p.model.points[point];
        const std::string base = clone.name;
        clone.name = unique_point_name(p, base + " (Side 2)");
        clone.filter_process.clear(); clone.filter_numbers.clear(); clone.filter_packages.clear();
        clone.machine.target_file.clear();
        p.model.points.push_back(clone);
        const int ci = (int)p.model.points.size() - 1;
        b.point = clone.name;
        // components of the Side-2 physical side move to the clone
        for (auto& c : p.comps)
            if (c.feature == Feature::Component && c.assigned_point == point && c.side == second) c.assigned_point = ci;
        ModelPoint& p1 = p.model.points[point];
        ModelPoint& p2 = p.model.points[ci];
        p1.side = p1.machine.side = first;  p1.machine.flipped = first == Side::Bottom;
        p2.side = p2.machine.side = second; p2.machine.flipped = second == Side::Bottom;
    }
    p.mfg.sides.push_back(a);
    if (two) p.mfg.sides.push_back(b);
    p.mfg.configured = true;
    p.dirty = true;
}

void swap_side_order(Project& p) {
    for (auto& s : p.mfg.sides) s.seq = s.seq == 1 ? 2 : (s.seq == 2 ? 1 : s.seq);
    std::sort(p.mfg.sides.begin(), p.mfg.sides.end(), [](const MfgSide& a, const MfgSide& b) { return a.seq < b.seq; });
    p.dirty = true;
}
void clear_sides(Project& p) { p.mfg = MfgSides{}; p.dirty = true; }

std::vector<SideIssue> validate_sides(const Project& p) { return validate_sides(p, p.mfg); }
std::vector<SideIssue> validate_sides(const Project& p, const MfgSides& mfg) {
    std::vector<SideIssue> v;
    auto err = [&](std::string t) { v.push_back({true, std::move(t)}); };
    auto warn = [&](std::string t) { v.push_back({false, std::move(t)}); };
    const SideCounts n = side_counts(p);
    if (!mfg.configured) {
        if (n.top && n.bottom) err("Production order not chosen: decide which physical side runs as Side 1.");
        return v;
    }
    std::set<int> seqs; std::set<int> phys; std::set<std::string> files;
    for (auto& s : mfg.sides) {
        const std::string L = mfg_label(s);
        if (!seqs.insert(s.seq).second) err(L + ": duplicate sequence number.");
        if (!phys.insert((int)s.physical).second) err(L + ": physical side mapped twice.");
        if (!files.insert(side_file_base(p, s)).second) err(L + ": file name collides with another side.");
        const int pi = side_point_index(p, s);
        if (pi < 0) { warn(L + ": export layer '" + s.point + "' not found."); continue; }
        const ModelPoint& mp = p.model.points[pi];
        if (!mp.has_machine) err(L + ": process point '" + mp.name + "' has no machine interface.");
        int cnt = 0, wrong = 0;
        for (auto& c : p.comps) {
            if (c.feature != Feature::Component || c.assigned_point != pi) continue;
            (c.side == s.physical ? cnt : wrong)++;
        }
        if (!cnt) warn(L + ": no components of the " + side_name(s.physical) + " side are assigned to '" + mp.name + "'.");
        int unassigned = 0;
        for (auto& c : p.comps)
            if (c.feature == Feature::Component && c.side == s.physical && c.assigned_point < 0 && !c.dnp) unassigned++;
        if (unassigned) warn(L + ": " + std::to_string(unassigned) + " " + side_name(s.physical) + " component(s) are not assigned to any process point.");
        if (mp.machine.side != s.physical) warn(L + ": profile '" + mp.name + "' is set for the " + side_name(mp.machine.side) + " side (exported as " + side_name(s.physical) + ").");
        if (s.physical == Side::Bottom && !mp.machine.flipped) warn(L + ": bottom side exported without mirroring - check the profile.");
    }
    if (mfg.sides.size() == 2 && mfg.sides[0].point == mfg.sides[1].point && !mfg.sides[0].point.empty())
        warn("Both sides share process point '" + mfg.sides[0].point + "': origin / rotation / mirroring cannot differ per side. Use 'Separate profiles'.");
    if (n.top && n.bottom && mfg.sides.size() < 2) err("Board has components on both sides but only one manufacturing side is defined.");
    return v;
}

std::string validation_summary(const std::vector<SideIssue>& v) {
    int e = 0, w = 0;
    for (auto& i : v) (i.blocking ? e : w)++;
    if (!e && !w) return "OK";
    std::string s = std::to_string(e) + " error(s), " + std::to_string(w) + " warning(s)";
    for (auto& i : v) s += std::string("\n") + (i.blocking ? "ERROR: " : "warning: ") + i.text;
    return s;
}

bool build_side_job(const Project& p, const MfgSide& s, MachineJob& out, std::string& err) {
    const int pi = side_point_index(p, s);
    if (pi < 0) { err = mfg_label(s) + ": no process point."; return false; }
    Project q = p;                      // cheap enough at export; keeps machine_job untouched
    q.layers.clear();
    for (auto& c : q.comps)
        if (c.feature == Feature::Component && c.assigned_point == pi && c.side != s.physical) c.assigned_point = -1;
    q.model.points[pi].machine.side = s.physical;
    if (!build_machine_job(q, pi, out, err)) { err = mfg_label(s) + ": " + err; return false; }
    out.bottom_side = s.physical == Side::Bottom;
    return true;
}

void apply_desc_mode(const Project& p, MachineJob& job, const std::string& mode) {
    if (mode.empty() || mode == "desc") return;
    auto conv = [&](const std::string& part, std::string& d) {
        auto it = p.parts.find(part);
        if (it == p.parts.end() || it->second.manf_pn.empty()) return;
        const std::string& m = it->second.manf_pn;
        if (mode == "mpn") d = m;
        else if (mode == "desc_mpn") d = d.empty() ? m : d + " | " + m;
    };
    for (auto& jp : job.parts) conv(jp.part, jp.description);
    for (auto& pl : job.placements) conv(pl.part, pl.description);
}

static std::string csv(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}
static std::string num(double v, int prec = 3) { char b[48]; std::snprintf(b, sizeof b, "%.*f", prec, v); return b; }

std::string placements_csv(const Project& p, const MfgSide& s, const MachineJob& job) {
    std::string o = "Ref,Part Number,MPN,Manufacturer,Description,Package,X (mm),Y (mm),Rotation,Physical Side,Manufacturing Side,Process,Circuit,Skip\r\n";
    for (auto& pl : job.placements) {
        const Component* c = p.find_ref(pl.ref);
        const PartNumber* pn = c ? p.part_of(*c) : nullptr;
        o += csv(pl.ref) + "," + csv(pl.part) + "," + csv(pn ? pn->manf_pn : "") + "," + csv(pn ? pn->manufacturer : "") + "," +
             csv(pn ? pn->description : pl.description) + "," + csv(pl.package) + "," + num(pl.x) + "," + num(pl.y) + "," + num(pl.rot, 2) + "," +
             side_name(s.physical) + "," + csv(mfg_suffix(s)) + "," + csv(job.point_name) + "," + std::to_string(pl.circuit) + "," + (pl.skip ? "1" : "0") + "\r\n";
    }
    return o;
}

static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;"; else if (c == '"') o += "&quot;"; else o += c; }
    return o;
}

std::string sides_report_html(const Project& p, const std::vector<SideExportResult>& r) {
    std::string h = "<!doctype html><html><head><meta charset=\"utf-8\"><title>" + esc(job_base_name(p)) + " - export report</title>"
        "<style>body{font:14px Segoe UI,Arial,sans-serif;margin:24px;color:#1b1f24;background:#fff}"
        "table{border-collapse:collapse;margin:12px 0}td,th{border:1px solid #c9ced6;padding:6px 10px;text-align:left}"
        "th{background:#eef1f5}.err{color:#b42318}.warn{color:#9a6700}.big{font-size:20px;font-weight:600}</style></head><body>";
    h += "<div class=big>" + esc(job_base_name(p)) + " &mdash; machine programs</div>";
    h += "<p>Assembly: " + esc(p.info.name) + " &nbsp; BOM rev: " + esc(p.info.bom_rev) + " &nbsp; Customer: " + esc(p.info.customer) + "</p>";
    h += "<p><b>Side 1 / Side 2 = production order.</b> Top / Bottom = physical PCB side.</p>";
    h += "<table><tr><th>Run</th><th>Physical side</th><th>Process point</th><th>Machine</th><th>Placements</th><th>Parts</th><th>Fiducials</th>"
         "<th>Origin (mm)</th><th>Board rotation</th><th>Mirrored</th><th>Program file</th><th>Placements CSV</th><th>Status</th></tr>";
    for (auto& e : r) {
        h += "<tr><td><b>" + esc(mfg_suffix(e.side)) + "</b></td><td><b>" + side_name(e.side.physical) + "</b></td><td>" + esc(e.point) + "</td><td>" + esc(e.exporter) +
             "</td><td>" + std::to_string(e.placements) + "</td><td>" + std::to_string(e.parts) + "</td><td>" + std::to_string(e.fiducials) +
             "</td><td>" + num(e.origin_x) + ", " + num(e.origin_y) + "</td><td>" + std::to_string(e.orientation) + "&deg;</td><td>" + (e.flipped ? "yes" : "no") +
             "</td><td>" + esc(e.program_file) + "</td><td>" + esc(e.csv_file) + "</td><td>" + (e.ok ? "OK" : "<span class=err>" + esc(e.err) + "</span>") + "</td></tr>";
    }
    h += "</table>";
    for (auto& e : r) for (auto& w : e.warnings) h += "<div class=warn>" + esc(mfg_label(e.side)) + ": " + esc(w) + "</div>";
    if (!p.mfg.last_validation.empty() && p.mfg.last_validation != "OK")
        h += "<h3>Pre-export validation</h3><pre>" + esc(p.mfg.last_validation) + "</pre>";
    h += "<p style=\"color:#667\">Generated by PCB Plotter. Coordinates in the CSV are machine coordinates (mm) after origin, mirroring and board rotation.</p></body></html>";
    return h;
}

std::string sides_report_txt(const Project& p, const std::vector<SideExportResult>& r) {
    std::string t = job_base_name(p) + " - machine programs\r\nSide N = production order; Top/Bottom = physical side\r\n\r\n";
    for (auto& e : r) {
        t += mfg_suffix(e.side) + " = " + side_name(e.side.physical) + "  point '" + e.point + "'  " + e.exporter + "\r\n";
        t += "  placements " + std::to_string(e.placements) + ", parts " + std::to_string(e.parts) + ", fiducials " + std::to_string(e.fiducials) +
             ", origin " + num(e.origin_x) + "," + num(e.origin_y) + ", rotation " + std::to_string(e.orientation) + ", mirrored " + (e.flipped ? "yes" : "no") + "\r\n";
        t += "  program " + e.program_file + "\r\n  csv     " + e.csv_file + "\r\n" + (e.ok ? "" : "  FAILED: " + e.err + "\r\n");
    }
    return t;
}

// ---- R18d export-layer links ------------------------------------------------------------------------
std::string link_label(int link) { return link > 0 ? std::to_string(link) : std::string("None"); }
int max_link(const Project& p, int exclude_point) {
    int m = 0;
    for (size_t i = 0; i < p.model.points.size(); ++i) if ((int)i != exclude_point) m = std::max(m, p.model.points[i].link);
    return m;
}
int cycle_link(Project& p, int point) {
    if (point < 0 || point >= (int)p.model.points.size()) return 0;
    int& l = p.model.points[point].link;
    const int cap = max_link(p, point) + 1;          // never past the next unused ID
    l = l + 1 > cap ? 0 : l + 1;
    p.dirty = true;
    return l;
}
std::vector<int> link_members(const Project& p, int link) {
    std::vector<int> v;
    if (link <= 0) return v;
    for (size_t i = 0; i < p.model.points.size(); ++i) if (p.model.points[i].link == link) v.push_back((int)i);
    return v;
}
MfgSides link_sides(const Project& p, int link) {
    MfgSides m;
    const auto mem = link_members(p, link);
    if (mem.size() != 2) return m;
    for (int k = 0; k < 2; ++k) {
        const ModelPoint& mp = p.model.points[mem[k]];
        MfgSide s;
        s.seq = k + 1;
        s.physical = mp.machine.side;
        s.point = mp.name;
        m.sides.push_back(s);
    }
    m.configured = true;
    return m;
}
static bool is_smt_layer(const ModelPoint& m) {
    if (m.kind != PointKind::Placement) return false;
    std::string t = m.process_type;
    for (auto& c : t) c = (char)toupper((unsigned char)c);
    return t == "SMT" || (t.empty() && m.name.find("SMT") != std::string::npos);
}
bool auto_link_smt_pair(Project& p) {
    std::vector<int> smt;
    for (size_t i = 0; i < p.model.points.size(); ++i) if (is_smt_layer(p.model.points[i])) smt.push_back((int)i);
    if (smt.size() != 2) return false;
    ModelPoint& a = p.model.points[smt[0]];
    ModelPoint& b = p.model.points[smt[1]];
    if (a.machine.side == b.machine.side || a.link || b.link) return false;
    a.link = b.link = max_link(p) + 1;
    p.dirty = true;
    return true;
}
// migrate_sides_to_links(): src/core/project_io.cpp (linked by every CORE test without sides.cpp)

} // namespace pcbp
