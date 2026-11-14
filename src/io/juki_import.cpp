// ============================================================================
// juki_import.cpp — JUKI HLC / ISS program -> Project.
//
// Binary layout facts: specs/JUKI_EXPORT.md (§ refs below). Extra component
// record fields (type +78, length/width/height +81/+85/+89, H8H skip +716) are
// from Corey's TrayTagPrinter H8H work on real machine files [T]; the H7H
// record is assumed to share the +78..+92 block (same leading layout) [I].
// ISS tag names follow TTP's ISS inventory (pnp_lib iss.cpp, juki_iss_doc).
// ============================================================================
#include "io/juki_import.h"
#include "core/machine_job.h"
#include "core/machine_assign.h"   // R24b
#include "io/juki_writer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace pcbp::juki {

namespace {

// ---------------------------------------------------------------------------
// raw helpers
// ---------------------------------------------------------------------------
uint16_t rd_u16(const std::vector<uint8_t>& d, size_t o) {
    return o + 2 <= d.size() ? (uint16_t)(d[o] | (d[o + 1] << 8)) : 0;
}
uint32_t rd_u32(const std::vector<uint8_t>& d, size_t o) {
    if (o + 4 > d.size()) return 0;
    return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
}
int32_t rd_i32(const std::vector<uint8_t>& d, size_t o) { return (int32_t)rd_u32(d, o); }
std::string rd_str(const std::vector<uint8_t>& d, size_t o, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && o + i < d.size() && d[o + i]; ++i) s += (char)d[o + i];
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
bool starts_with(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }

double mm(int32_t raw) { return raw / 10000.0; }
std::string fmt4(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.4f", v);
    std::string s = b;
    if (s == "-0.0000") s = "0.0000";
    return s;
}
int ang_raw_diff(long a, long b) {
    long d = ((a - b) % 28800 + 28800) % 28800;
    return (int)std::min(d, 28800 - d);
}

// ---------------------------------------------------------------------------
// neutral intermediate (shared by HLC + ISS)
// ---------------------------------------------------------------------------
struct NPlace {
    std::string ref;
    double x = 0, y = 0, rot = 0;
    bool skip = false;
    int part = -1;                      // index into NProgram::parts
    int local_a = -1, local_b = -1;     // index into NProgram::marks
};
struct NPart {
    std::string name, desc;
    double len = 0, wid = 0, hgt = 0;   // mm, 0 = unknown
    int type = -1;
    bool skip = false;
    int feeder_kind = 0;        // R18a H8H +80
    double supply = 0;          // R18a H8H supply angle deg
    bool has_supply = false;
    int tape_code = 0, pitch_code = 0;   // R24b H8H +176 / +178 (tape)
};
struct NMark { std::string name; double x = 0, y = 0, dia = 0; bool global = true; };
struct NCircuit { double x = 0, y = 0, rot = 0; };
struct NProgram {
    std::string name, board_name, comment, line_name, format, exporter;
    double board_w = 0, board_h = 0, board_x0 = 0, board_y0 = 0, thick = 0;
    double ckt_w = 0, ckt_h = 0, ckt_x = 0, ckt_y = 0;
    bool has_board = false, has_ckt = false;
    bool rl = false, back = false, bottom = false, panel = false;
    std::string timestamp;
    std::vector<NCircuit> circuits;
    std::vector<NPlace> places;
    std::vector<NPart> parts;
    std::vector<NMark> marks;
    std::map<std::string, std::string> extra_opts;
};

// ---------------------------------------------------------------------------
// package reconstruction
// ---------------------------------------------------------------------------
std::string make_package(Project& prj, const NPart& p, ImportResult& res) {
    const bool known = p.len > 0 && p.wid > 0;
    char nm[96];
    if (known) std::snprintf(nm, sizeof nm, "RECON_%.2fx%.2fx%.2f", p.len, p.wid, p.hgt);
    else std::snprintf(nm, sizeof nm, "RECON_UNKNOWN");
    if (prj.packages.count(nm)) return nm;
    Package k;
    k.name = nm;
    k.smt = true;
    const double L = known ? p.len : 1.0, W = known ? p.wid : 0.5;
    k.height = p.hgt;
    const double big = std::max(L, W), small = std::min(L, W);
    const bool chip = big <= 8.0 && (big / std::max(small, 1e-6) >= 1.3 || big <= 2.5);
    if (chip) {                               // two terminals along the long axis
        const bool along_x = L >= W;
        const double pl = big * 0.3;
        for (int s = -1; s <= 1; s += 2) {
            Pad pd;
            pd.pos = along_x ? Vec2{s * (big - pl) / 2, 0} : Vec2{0, s * (big - pl) / 2};
            pd.w = along_x ? pl : small; pd.h = along_x ? small : pl;
            pd.pin = s < 0 ? 1 : 2;
            k.pads.push_back(pd);
        }
    } else {                                  // one synthetic lead row per side
        const double t = std::max(0.3, small * 0.12);
        const Vec2 pos[4] = {{-L / 2 + t / 2, 0}, {0, -W / 2 + t / 2}, {L / 2 - t / 2, 0}, {0, W / 2 - t / 2}};
        for (int i = 0; i < 4; ++i) {
            Pad pd;
            pd.pos = pos[i];
            pd.w = (i % 2 == 0) ? t : L * 0.6;
            pd.h = (i % 2 == 0) ? W * 0.6 : t;
            pd.pin = i + 1;
            k.pads.push_back(pd);
        }
    }
    k.leads = (int)k.pads.size();
    GPrim r; r.kind = GKind::Rect; r.pts = {{-L / 2, -W / 2}, {L / 2, W / 2}};
    k.body.push_back(r);
    k.description = known ? "reconstructed from machine program component record (body " + fmt4(L) + " x " + fmt4(W) +
                                " mm" + (p.type >= 0 ? ", type " + std::to_string(p.type) : std::string()) +
                                "; pads synthetic)"
                          : "reconstructed placeholder (program record has no body size)";
    prj.packages[nm] = k;
    ++res.packages;
    return nm;
}

std::string fid_package(Project& prj, double dia, ImportResult& res) {
    const double d = dia > 0 ? dia : 1.0;
    const std::string nm = "FID_" + fmt4(d);
    if (!prj.packages.count(nm)) {
        Package k; k.name = nm; k.smt = true; k.leads = 1;
        k.description = "fiducial mark (reconstructed from machine program)";
        Pad pd; pd.w = pd.h = d; pd.shape = PadShape::Round; pd.pin = 1;
        k.pads.push_back(pd);
        prj.packages[nm] = k;
        ++res.packages;
    }
    return nm;
}

// ---------------------------------------------------------------------------
// NProgram -> Project
// ---------------------------------------------------------------------------
bool build_project(NProgram& np, Project& prj, const ImportOptions& opt, ImportResult& res, std::string& err) {
    auto warn = [&](const std::string& s) { res.warnings.push_back(s); };
    res.format = np.format;
    res.exporter_id = np.exporter;

    prj.info.number = np.name;
    prj.info.name = np.board_name.empty() ? np.name : np.board_name;
    prj.display_units = Units::MM;
    prj.virtual_origin = {};
    prj.comps.clear();
    prj.parts.clear();
    prj.packages.clear();
    prj.board_frame.clear();
    prj.panel_frame.clear();

    // ---- parts -------------------------------------------------------------
    std::vector<std::string> part_name(np.parts.size());
    for (size_t i = 0; i < np.parts.size(); ++i) {
        const NPart& p = np.parts[i];
        std::string key = p.name;
        if (key.empty()) { key = "COMP" + std::to_string(i + 1); warn("component record " + std::to_string(i) + " has no name; named " + key); }
        part_name[i] = key;
        if (prj.parts.count(key)) { warn("duplicate component name '" + key + "' (merged)"); continue; }
        PartNumber pn;
        pn.number = key;
        pn.description = p.desc;
        pn.default_process = "SMT";
        pn.height = p.hgt;
        if (p.has_supply) {   // R18a: kept exactly (byte-identical re-export)
            pn.packaging = packaging_from_feeder_kind(p.feeder_kind);
            pn.supply_angle = p.supply; pn.supply_angle_set = true;
        }
        if (p.skip) { pn.pop_type = "SKIP"; warn("component '" + key + "' carries the machine skip flag (kept as pop type SKIP)"); }
        if (opt.reconstruct_packages) pn.package = make_package(prj, p, res);
        if (p.has_supply) { pn.prog_type = p.type > 0 ? p.type : 0; pn.prog_tape = p.tape_code; pn.prog_pitch = p.pitch_code; }   // R24b verbatim codes
        prj.parts[key] = pn;
        ++res.parts;
    }
    res.components = (int)np.parts.size();

    // ---- circuits / step & repeat -------------------------------------------
    std::vector<NCircuit> ck = np.circuits;
    if (ck.empty()) ck.push_back({});
    const size_t K = ck.size();
    const size_t N = np.places.size();
    std::vector<std::string> order;                      // first-appearance order
    std::map<std::string, std::vector<size_t>> occ;
    for (size_t i = 0; i < N; ++i) {
        auto& v = occ[np.places[i].ref];
        if (v.empty()) order.push_back(np.places[i].ref);
        v.push_back(i);
    }
    const double r0 = ck[0].rot;
    enum class Mode { Single, StepRepeat, RootOnly, Expanded } mode = Mode::Single;
    std::vector<bool> x_out(K, false);
    if (K > 1) {
        bool all_k = N == order.size() * K;
        for (auto& kv : occ) if (kv.second.size() != K) all_k = false;
        if (all_k) {
            // x-out circuits: every placement skipped while the root has a live one
            bool root_live = false;
            for (auto& kv : occ) if (!np.places[kv.second[0]].skip) root_live = true;
            for (size_t j = 1; j < K; ++j) {
                bool all = true;
                for (auto& kv : occ) if (!np.places[kv.second[j]].skip) all = false;
                x_out[j] = all && root_live;
            }
            bool ok = true;
            const double tol = opt.step_tol_mm;
            for (auto& kv : occ) {
                const NPlace& rp = np.places[kv.second[0]];
                const double crot = rp.rot - r0;
                for (size_t j = 1; j < K && ok; ++j) {
                    const NPlace& q = np.places[kv.second[j]];
                    const Vec2 off{ck[j].x - ck[0].x, ck[j].y - ck[0].y};
                    const Vec2 pr = off + rotate({rp.x, rp.y}, ck[j].rot);
                    const long pa = std::lround(norm_deg(crot + ck[j].rot) * 80.0);
                    if (std::fabs(pr.x - q.x) > tol || std::fabs(pr.y - q.y) > tol ||
                        ang_raw_diff(pa, std::lround(q.rot * 80.0)) > 1 || q.part != rp.part ||
                        q.skip != (x_out[j] || rp.skip))
                        ok = false;
                }
                if (!ok) break;
            }
            mode = ok ? Mode::StepRepeat : Mode::Expanded;
            if (!ok) warn("circuits are not a consistent step & repeat; every circuit imported as separate components (refs suffixed _<circuit>)");
        } else if (N == order.size()) {
            mode = Mode::RootOnly;
            warn("program holds root-circuit placements only; circuits imported as panel images (re-export writes circuit 1 + circuit layout)");
        } else {
            mode = Mode::Expanded;
            warn("placement count does not match circuits x refs; every placement imported (refs suffixed)");
        }
    } else if (N != order.size()) {
        warn("duplicate reference designators; repeats suffixed _<n>");
    }
    if ((mode == Mode::StepRepeat || mode == Mode::RootOnly) && (ck[0].x != 0 || ck[0].y != 0))
        warn("first circuit is not at (0,0); image offsets are taken relative to it");

    // images
    prj.images.clear();
    const bool images_from_circuits = mode == Mode::StepRepeat || mode == Mode::RootOnly;
    const size_t nimg = images_from_circuits ? K : 1;
    for (size_t j = 0; j < nimg; ++j) {
        PanelImage im;
        im.name = std::to_string(j + 1);
        im.root = j == 0;
        im.offset = j == 0 ? Vec2{} : Vec2{ck[j].x - ck[0].x, ck[j].y - ck[0].y};
        im.rot = ck[j].rot;
        im.x_out = x_out[j];
        im.x_index = (int)j;
        prj.images.push_back(im);
    }
    if (mode == Mode::Expanded) prj.images[0].rot = 0;
    res.images = (int)prj.images.size();
    res.step_repeat = mode == Mode::StepRepeat;

    // ---- model point ----------------------------------------------------------
    int pi = prj.point_index(opt.point_name);
    if (pi < 0) {
        ModelPoint mp;
        mp.name = opt.point_name;
        mp.process_type = "SMT";
        mp.filter_process.push_back("SMT");
        prj.model.points.push_back(mp);
        pi = (int)prj.model.points.size() - 1;
    }
    ModelPoint& mp = prj.model.points[(size_t)pi];
    mp.kind = PointKind::Placement;
    mp.has_machine = true;
    mp.side = np.bottom ? Side::Bottom : Side::Top;
    MachineSetup& ms = mp.machine;
    ms = MachineSetup{};
    ms.exporter_id = np.exporter;
    ms.side = mp.side;
    ms.single_board = !np.panel;
    ms.force_angle_calc = true;
    ms.options["juki.transport"] = np.rl ? "RL" : "LR";
    ms.options["juki.fixed_rail"] = np.back ? "back" : "front";
    ms.options["juki.comment"] = np.comment;
    if (!np.name.empty()) ms.options["juki.program_name"] = np.name;   // v17: the program's own name wins over Board ID / Project Name
    if (!np.line_name.empty()) ms.options["juki.line_name"] = np.line_name;
    if (!np.timestamp.empty()) ms.options["juki.timestamp"] = np.timestamp;
    if (np.has_board) {
        ms.options["juki.board_x0"] = fmt4(np.board_x0);
        ms.options["juki.board_y0"] = fmt4(np.board_y0);
    }
    if (np.has_ckt) {
        ms.options["juki.circuit_w"] = fmt4(np.ckt_w);
        ms.options["juki.circuit_h"] = fmt4(np.ckt_h);
        ms.options["juki.circuit_x"] = fmt4(np.ckt_x);
        ms.options["juki.circuit_y"] = fmt4(np.ckt_y);
    }
    if (np.thick > 0) ms.options["juki.board_t"] = fmt4(np.thick);
    for (auto& kv : np.extra_opts) ms.options[kv.first] = kv.second;
    // the board / circuit options above are verbatim program geometry for THIS setup
    ms.options["juki.geom_sig"] = setup_signature(ms, ms.has_origin ? ms.origin : prj.virtual_origin);

    // ---- board frame ------------------------------------------------------------
    auto rect = [](double x0, double y0, double w, double h) {
        return std::vector<Vec2>{{x0, y0}, {x0 + w, y0}, {x0 + w, y0 + h}, {x0, y0 + h}};
    };
    if (np.has_board) {
        auto r = rect(np.board_x0, np.board_y0, np.board_w, np.board_h);
        if (prj.images.size() > 1) {
            prj.panel_frame = {r};
            // R22 (REVIEW_V21 §2.3): no circuit size in the program -> circuit outline unknown. Never copy the board
            // rect into the circuit outline: every instance would then be the whole board (false "beyond the board").
            if (np.has_ckt) prj.board_frame = {rect(np.ckt_x, np.ckt_y, np.ckt_w, np.ckt_h)};
        } else {
            prj.board_frame = {r};
        }
        prj.board_outline_src = prj.panel_outline_src = OutlineSource::Imported;   // R16 provenance
    } else {
        warn("program has no board size; re-export derives it from the component extent");
    }

    // ---- fiducials -----------------------------------------------------------------
    const Side side = mp.side;
    std::set<std::string> used_refs;
    std::vector<int> mark_comp(np.marks.size(), -1);
    int gnum = 0;
    for (size_t pass = 0; pass < 2; ++pass)          // globals first: their order is the export order
        for (size_t i = 0; i < np.marks.size(); ++i) {
            const NMark& m = np.marks[i];
            if (m.global != (pass == 0)) continue;
            if (!m.global && !m.name.empty()) {          // same local mark shared by several sets
                bool reused = false;
                for (size_t j = 0; j < np.marks.size() && !reused; ++j)   // global or earlier local
                    if ((np.marks[j].global || j < i) && j != i && mark_comp[j] >= 0 && np.marks[j].name == m.name && np.marks[j].x == m.x && np.marks[j].y == m.y) {
                        mark_comp[i] = mark_comp[j];
                        reused = true;
                    }
                if (reused) continue;
            }
            Component c;
            c.feature = Feature::Fiducial;
            c.fid_scope = m.global ? FiducialScope::Global : FiducialScope::Local;
            c.fid_number = m.global ? ++gnum : 0;
            std::string ref = m.name.empty() ? std::string(m.global ? "FID" : "LFID") + std::to_string(i + 1) : m.name;
            for (int k = 2; used_refs.count(ref); ++k) ref = (m.name.empty() ? "FID" : m.name) + "_" + std::to_string(k);
            used_refs.insert(ref);
            c.ref = ref;
            c.pos = c.cad_pos = {m.x, m.y};
            c.side = side;
            c.package = fid_package(prj, m.dia, res);
            mark_comp[i] = (int)prj.add_component(c).id;
            ++res.fiducials;
        }
    auto comp_by_id = [&](int id) -> Component* {
        for (auto& c : prj.comps) if ((int)c.id == id) return &c;
        return nullptr;
    };

    // ---- placements ---------------------------------------------------------------
    auto add_place = [&](const NPlace& p, const std::string& ref, double rot) {
        Component c;
        c.ref = ref;
        c.pos = c.cad_pos = {p.x, p.y};
        c.rot = c.rot_imported = norm_deg(rot);
        c.side = side;
        c.dnp = p.skip;
        if (p.part >= 0 && p.part < (int)part_name.size()) {
            c.number = part_name[(size_t)p.part];
            c.package = prj.parts[c.number].package;
        }
        c.assigned_point = pi;
        Component& added = prj.add_component(c);
        ++res.placements;
        if (p.local_a >= 0 && p.local_b >= 0) {
            Component* a = comp_by_id(mark_comp[(size_t)p.local_a]);
            Component* b = comp_by_id(mark_comp[(size_t)p.local_b]);
            if (a && b) {
                if (!a->local_fid_owner && a->fid_scope == FiducialScope::Local) a->local_fid_owner = (int)added.id;
                if (!b->local_fid_owner && b->fid_scope == FiducialScope::Local) b->local_fid_owner = (int)added.id;
                ms.options["juki.local." + ref] = a->ref + "," + b->ref;
            }
        }
    };
    if (images_from_circuits) {
        for (auto& r : order) {
            const NPlace& p = np.places[occ[r][0]];
            add_place(p, r, p.rot - r0);
        }
    } else {
        std::map<std::string, int> seen;
        for (size_t i = 0; i < N; ++i) {
            const NPlace& p = np.places[i];
            const int n = ++seen[p.ref];
            std::string ref = n == 1 ? p.ref : p.ref + "_" + std::to_string(n);
            while (occ.count(ref) && n > 1) ref += "_";
            add_place(p, ref, p.rot);
        }
    }

    // sort-by-part detection: root order not natural-ref but grouped by part
    {
        std::vector<const NPlace*> seq;
        if (images_from_circuits) for (auto& r : order) seq.push_back(&np.places[occ[r][0]]);
        else for (auto& p : np.places) seq.push_back(&p);
        bool natural = true, by_part = true;
        auto pn = [&](const NPlace* p) { return p->part >= 0 && p->part < (int)part_name.size() ? part_name[(size_t)p->part] : std::string(); };
        for (size_t i = 1; i < seq.size(); ++i) {
            if (ref_less(seq[i]->ref, seq[i - 1]->ref)) natural = false;
            if (pn(seq[i]) < pn(seq[i - 1])) by_part = false;
        }
        if (!natural && by_part) ms.options["juki.sort_by_part"] = "1";
        else if (!natural) warn("placement order is neither by reference nor by part; re-export reorders by reference");
    }

    // parts never placed are dropped by the exporter
    {
        std::set<std::string> used;
        for (auto& c : prj.comps) if (!c.number.empty()) used.insert(c.number);
        int unused = 0;
        for (auto& kv : prj.parts) if (!used.count(kv.first)) ++unused;
        if (unused) warn(std::to_string(unused) + " component record(s) are not used by any placement (kept as part numbers; not re-exported)");
    }
    if (np.thick > 0 && std::fabs(np.thick - 1.6) > 1e-9)
        warn("board thickness " + fmt4(np.thick) + " mm kept in option juki.board_t");
    if (N == 0) { err = "program has no placements"; return false; }
    prj.dirty = true;
    return true;
}

// ---------------------------------------------------------------------------
// binary HLC -> NProgram
// ---------------------------------------------------------------------------
bool read_hlc(const std::vector<uint8_t>& d, NProgram& np, ImportResult& res, std::string& err) {
    HlcParsed hp;
    if (!parse_hlc(d, hp, err)) return false;
    const auto& B = hp.blocks;
    const HlcBlockEntry& bl = B[1];
    const HlcBlockEntry& bb = B[2];
    const HlcBlockEntry& bp = B[3];
    const HlcBlockEntry& bc = B[4];
    const HlcBlockEntry& bm = B[6];
    const HlcBlockEntry& bn = B[12];
    const bool narrow = bp.recsize == 71;
    if (bp.recsize != 71 && bp.recsize != 96 && bp.recsize != 256) {
        err = "unknown placement record size " + std::to_string(bp.recsize);
        return false;
    }

    // ---- format ----
    const std::string ver = hp.version;
    const uint8_t st = hp.station;
    if (bc.recsize == 1822 || (B.size() == 28 && B[0].recsize == 4672) || (ver.size() == 4 && ver[0] == 'C')) {
        np.format = "H8H"; np.exporter = "juki.h8h";
        if (ver != "C00A") np.extra_opts["juki.h8h.version"] = ver;
        // bytes the project does not model (optimizer, line name, editor defaults):
        // kept so a re-export reproduces the JUKI-saved program (HLC_FORMAT.md §11)
        if (auto raw = h8h_residual(d); !raw.empty()) np.extra_opts["juki.h8h.raw"] = raw;
    } else if (bp.recsize == 256) {
        np.format = "H7H"; np.exporter = "juki.h7h";
    } else if (narrow) {
        if (st >= 11 && st <= 14) { np.format = "E4" + std::to_string(st - 10); np.exporter = "juki.e4" + std::to_string(st - 10); }
        else { np.format = "H4H"; np.exporter = "juki.h4h"; np.extra_opts["juki.h4h_hlc5"] = "1"; }
    } else {
        if (st == 32 || st == 33) { np.format = st == 32 ? "E45" : "E46"; np.exporter = st == 32 ? "juki.e45" : "juki.e46"; }
        else if (st == 48) { np.format = "E51"; np.exporter = "juki.e51"; }
        else if (ver == "702" || bc.recsize == 788) { np.format = "H5H"; np.exporter = "juki.h5h"; }
        else { np.format = "H4H"; np.exporter = "juki.h4h"; }
    }

    np.name = hp.name;
    np.board_name = hp.board_name;
    if (bl.addr && bl.recsize >= 54) np.comment = rd_str(d, bl.addr + 21, 33);
    if (bl.addr && bl.recsize >= 54) np.line_name = rd_str(d, bl.addr, 21);

    // ---- board (§4.2) ----
    if (bb.addr && bb.recsize >= 2000) {
        const size_t b = bb.addr;
        np.timestamp = std::to_string(rd_u32(d, b + 34));
        const uint16_t mode = hp.board_mode;
        np.rl = mode & 0x02;
        np.back = mode & 0x01;
        np.bottom = (mode & 0x60) == 0x40;
        // +42 bits 3-4: 0 Single PWB, 1 Matrix, 2 Non-Matrix (HLC_FORMAT.md §4)
        const int cfg = (mode >> 3) & 3;
        if (hp.pcb_w > 0 && hp.pcb_h > 0) {
            np.has_board = true;
            np.board_w = mm(hp.pcb_w); np.board_h = mm(hp.pcb_h);
            np.board_x0 = np.rl ? mm(hp.layout_x) : mm(hp.layout_x - hp.pcb_w);
            np.board_y0 = np.back ? mm(hp.layout_y - hp.pcb_h) : mm(hp.layout_y);
        }
        if (hp.ckt_w != 0) {
            np.has_ckt = true;
            np.ckt_w = mm(hp.ckt_w); np.ckt_h = mm(hp.ckt_h);
            np.ckt_x = mm(rd_i32(d, b + 78)); np.ckt_y = mm(rd_i32(d, b + 82));
        }
        if (cfg == 2 || (cfg == 0 && narrow))
            for (size_t i = 0; i < hp.circuits.size(); ++i)
                np.circuits.push_back({mm(hp.circuits[i].x), mm(hp.circuits[i].y), norm_deg(hp.circuit_angles[i] / 80.0)});
        if (!narrow && cfg <= 2) {
            // keep the program's circuit configuration and its exact Non-Matrix list
            np.extra_opts["juki.circuit_mode"] = cfg == 0 ? "single" : cfg == 1 ? "matrix" : "nonmatrix";
            if (cfg == 2 && !hp.circuits.empty()) {
                std::string cl;
                for (size_t i = 0; i < hp.circuits.size(); ++i)
                    cl += (i ? ";" : "") + fmt4(mm(hp.circuits[i].x)) + "," + fmt4(mm(hp.circuits[i].y)) + "," +
                          fmt4(hp.circuit_angles[i] / 80.0);
                np.extra_opts["juki.circuit_list"] = cl;
            }
        }
        if (cfg == 1 && !narrow && hp.div_x > 0 && hp.div_y > 0 && hp.div_x * hp.div_y <= 400)
            for (int j = 0; j < hp.div_y; ++j)
                for (int i = 0; i < hp.div_x; ++i)
                    np.circuits.push_back({mm(hp.first_x + (int64_t)i * hp.pitch_x), mm(hp.first_y + (int64_t)j * hp.pitch_y), 0.0});
        np.panel = np.circuits.size() > 1;
        if (narrow) {
            np.thick = mm(rd_i32(d, b + bb.recsize - 4));
            const unsigned gc = std::min<unsigned>(d[b + 1425], 3);
            for (unsigned k = 0; k < gc; ++k) {
                NMark m;
                m.x = mm(rd_i32(d, b + 1394 + 10 * k));
                m.y = mm(rd_i32(d, b + 1398 + 10 * k));
                m.dia = rd_u16(d, b + 1426 + 2 * k) / 10000.0;
                np.marks.push_back(m);
            }
        } else if (bb.recsize > 10811) {
            np.thick = mm(rd_i32(d, b + 10807));
        }
    }

    // ---- components (§4.4) ----
    const bool tmpl9 = bc.recsize >= 1792;
    for (uint32_t i = 0; bc.addr && i < bc.count; ++i) {
        const size_t r = bc.addr + (size_t)bc.recsize * i;
        NPart p;
        p.name = rd_str(d, r, 21);
        p.desc = rd_str(d, r + 47, 31);
        if (tmpl9) {
            const size_t ln = bc.recsize == 1822 ? 1143 : 1115;
            const std::string lname = rd_str(d, r + ln + 1, 61);
            const std::string ldesc = rd_str(d, r + ln + 62, 128);
            if (p.name.size() == 20 && starts_with(lname, p.name)) p.name = lname;
            if (p.desc.size() == 30 && starts_with(ldesc, p.desc)) p.desc = ldesc;
            const int32_t L = rd_i32(d, r + 81), W = rd_i32(d, r + 85), H = rd_i32(d, r + 89);
            auto sane = [](int32_t v) { return v > 0 && v < 2000000; };
            if (sane(L) && sane(W)) {
                p.len = mm(L); p.wid = mm(W); p.hgt = sane(H) ? mm(H) : 0;
                p.type = (int16_t)rd_u16(d, r + 78);
            }
            if (bc.recsize == 1822) {
                p.skip = d[r + 716] & 1;
                // R18a (HLC_FORMAT.md §8.1): feeder kind +80, supply angle i16 1/80 deg (+180 tape, +178 other)
                p.feeder_kind = d[r + 80];
                p.supply = (int16_t)rd_u16(d, r + (p.feeder_kind == 1 ? 180 : 178)) / 80.0;
                p.has_supply = true;
                if (p.feeder_kind == 1) { p.tape_code = rd_u16(d, r + 176); p.pitch_code = rd_u16(d, r + 178); }   // R24b
            }
        }
        np.parts.push_back(p);
    }

    // ---- names (§4.7) ----
    std::vector<std::string> names;
    for (uint32_t i = 0; bn.addr && i < bn.count; ++i) {
        const size_t r = bn.addr + (size_t)bn.recsize * i;
        std::string s = rd_str(d, r + 1, 11);
        if (bn.recsize >= 32) {
            const std::string l = rd_str(d, r + 12, 20);
            if (l.size() > s.size() && starts_with(l, s)) s = l;
        }
        names.push_back(s);
    }

    // ---- marks (§4.6) ----
    std::vector<int> set_first(hp.mark_sets.size(), -1);   // wide: element -> index of first mark in np.marks
    if (bm.addr && !narrow) {
        const size_t es = bm.recsize == 3072 ? 128 : 124;
        bool have_global = false;
        for (size_t s = 0; s < hp.mark_sets.size(); ++s) {
            const HlcMarkSet& ms = hp.mark_sets[s];
            const size_t e = bm.addr + (size_t)bm.recsize * s;
            const bool global = ms.type == 1;
            if (global && have_global) continue;            // per-machine copies (TTP H8H)
            if (global) have_global = true;
            set_first[s] = (int)np.marks.size();
            for (size_t k = 0; k < ms.marks.size(); ++k) {
                NMark m;
                m.global = global;
                m.x = mm(ms.marks[k].x); m.y = mm(ms.marks[k].y);
                if (es == 128) m.name = rd_str(d, e + 40 + es * k + 14, 16);
                m.dia = rd_u16(d, e + 40 + es * k + 8) / 10000.0;   // wDiameter (0 = not set)
                np.marks.push_back(m);
            }
        }
    }
    const size_t narrow_local0 = np.marks.size();
    if (bm.addr && narrow) {
        for (auto& lm : hp.local_marks) {
            NMark m; m.global = false; m.x = mm(lm.x); m.y = mm(lm.y);
            np.marks.push_back(m);
        }
    }

    // ---- placements (§4.3) ----
    for (size_t i = 0; i < hp.placements.size(); ++i) {
        const HlcPlacementRec& q = hp.placements[i];
        NPlace p;
        p.ref = q.ref;
        if (i < names.size() && !names[i].empty() && (q.ref.empty() || starts_with(names[i], q.ref))) p.ref = names[i];
        if (p.ref.empty()) p.ref = "P" + std::to_string(i + 1);
        p.x = mm(q.x); p.y = mm(q.y);
        p.rot = norm_deg(q.angle / 80.0);
        p.skip = q.flags & 1;
        if ((q.status & 0x03)   /* component set (HLC_FORMAT.md §7) */ && q.comp < np.parts.size()) p.part = q.comp;
        if (q.flags & 8) {
            if (narrow) {
                const size_t a = narrow_local0 + 2 * (size_t)q.mark_set;
                if (a + 1 < np.marks.size()) { p.local_a = (int)a; p.local_b = (int)a + 1; }
            } else if (q.mark_set < set_first.size() && set_first[q.mark_set] >= 0 &&
                       hp.mark_sets[q.mark_set].marks.size() >= 2) {
                p.local_a = set_first[q.mark_set]; p.local_b = p.local_a + 1;
            }
            if (p.local_a < 0) res.warnings.push_back("placement " + p.ref + ": local mark set not found");
        }
        np.places.push_back(p);
    }
    // local mark names for formats without a name field
    for (const auto& p : np.places)
        if (p.local_a >= 0) {
            if (np.marks[(size_t)p.local_a].name.empty()) np.marks[(size_t)p.local_a].name = p.ref + "_LM1";
            if (np.marks[(size_t)p.local_b].name.empty()) np.marks[(size_t)p.local_b].name = p.ref + "_LM2";
        }
    if (!np.marks.empty() && narrow == false && bm.recsize != 3072)
        res.warnings.push_back("mark names are not stored in this format; fiducials named FID<n>");
    return true;
}

// ---------------------------------------------------------------------------
// ISS XML (tolerant tag scanner; no DTD / namespaces needed)
// ---------------------------------------------------------------------------
struct Xml {
    const std::string& t;
    // position of "<tag" (followed by space, '/' or '>') in [from, to)
    size_t open(const std::string& tag, size_t from, size_t to) const {
        const std::string pat = "<" + tag;
        for (size_t p = t.find(pat, from); p != std::string::npos && p < to; p = t.find(pat, p + 1)) {
            const char c = p + pat.size() < t.size() ? t[p + pat.size()] : '>';
            if (c == ' ' || c == '>' || c == '/' || c == '\t' || c == '\r' || c == '\n') return p;
        }
        return std::string::npos;
    }
    size_t gt(size_t op) const { return t.find('>', op); }
    // position of "</tag" followed by '>' or whitespace
    size_t close(const std::string& tag, size_t from) const {
        const std::string pat = "</" + tag;
        for (size_t p = t.find(pat, from); p != std::string::npos; p = t.find(pat, p + 1)) {
            const char c = p + pat.size() < t.size() ? t[p + pat.size()] : '>';
            if (c == '>' || c == ' ' || c == '\t' || c == '\r' || c == '\n') return p;
        }
        return std::string::npos;
    }
    bool self_closing(size_t op) const { size_t g = gt(op); return g != std::string::npos && g > 0 && t[g - 1] == '/'; }
    // end (index after the closing tag) of the element opened at `op`
    size_t end(const std::string& tag, size_t op) const {
        size_t g = gt(op);
        if (g == std::string::npos) return t.size();
        if (self_closing(op)) return g + 1;
        int depth = 1;
        size_t p = g + 1;
        while (depth > 0) {
            size_t o = open(tag, p, t.size());
            size_t c = close(tag, p);
            if (c == std::string::npos) return t.size();
            if (o != std::string::npos && o < c) {
                if (!self_closing(o)) ++depth;
                p = gt(o) + 1;
            } else {
                --depth;
                p = t.find('>', c) + 1;
            }
        }
        return p;
    }
    static std::string unescape(std::string s) {
        static const std::pair<const char*, char> ent[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
        for (auto& e : ent)
            for (size_t p = s.find(e.first); p != std::string::npos; p = s.find(e.first, p + 1)) s.replace(p, std::strlen(e.first), 1, e.second);
        return s;
    }
    std::string attr(size_t op, const std::string& name) const {
        size_t g = gt(op);
        if (g == std::string::npos) return {};
        for (size_t p = t.find(name, op); p != std::string::npos && p < g; p = t.find(name, p + 1)) {
            if (t[p - 1] != ' ' && t[p - 1] != '\t' && t[p - 1] != '\n' && t[p - 1] != '\r') continue;
            size_t q = p + name.size();
            while (q < g && t[q] == ' ') ++q;
            if (q >= g || t[q] != '=') continue;
            ++q;
            while (q < g && t[q] == ' ') ++q;
            const char quote = t[q];
            if (quote != '"' && quote != '\'') continue;
            size_t e = t.find(quote, q + 1);
            if (e == std::string::npos) return {};
            return unescape(t.substr(q + 1, e - q - 1));
        }
        return {};
    }
    double num(size_t op, const std::string& name, double def = 0) const {
        std::string s = attr(op, name);
        if (s.empty()) return def;
        char* e = nullptr;
        double v = std::strtod(s.c_str(), &e);
        return e == s.c_str() ? def : v;
    }
    std::string text(const std::string& tag, size_t from, size_t to) const {
        size_t o = open(tag, from, to);
        if (o == std::string::npos || self_closing(o)) return {};
        size_t g = gt(o) + 1;
        size_t c = close(tag, g);
        if (c == std::string::npos || c > to) return {};
        std::string s = unescape(t.substr(g, c - g));
        while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
        size_t a = s.find_first_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a);
    }
    // descend a path of tags; returns [open, end) of the last one
    bool path(const std::vector<std::string>& tags, size_t& op, size_t& ed) const {
        size_t from = 0, to = t.size();
        for (auto& tg : tags) {
            size_t o = open(tg, from, to);
            if (o == std::string::npos) return false;
            op = o; ed = end(tg, o);
            from = gt(o) + 1; to = ed;
        }
        return true;
    }
};

bool truthy(const std::string& v) {
    std::string u;
    for (char c : v) u += (char)std::toupper((unsigned char)c);
    return u == "YES" || u == "TRUE" || u == "1" || u == "NOUSE" || u == "ON";
}

bool read_iss(const std::string& text, NProgram& np, ImportResult& res, std::string& err) {
    Xml x{text};
    if (x.open("productionProgram", 0, text.size()) == std::string::npos && x.open("placement", 0, text.size()) == std::string::npos) {
        err = "not an ISS program (no productionProgram / placement elements)";
        return false;
    }
    np.format = "ISS";
    np.exporter = "juki.h8h";
    res.warnings.push_back("ISS has no PCB Plotter exporter; machine point set to H8H");
    np.name = x.text("pwbId", 0, text.size());
    np.board_name = np.name;
    size_t op, ed;
    if (x.path({"core", "pwbData", "pwbConfiguration", "outline"}, op, ed)) {
        np.board_w = x.num(op, "x"); np.board_h = x.num(op, "y");
        np.has_board = np.board_w > 0 && np.board_h > 0;
    }
    // circuits: nonMatrix allocations, else matrix grid
    size_t cop = std::string::npos, ced = 0;
    if (x.path({"core", "pwbData", "circuitConfigurationData"}, op, ed) ||
        x.path({"model", "pwbData", "circuitConfigurationData"}, op, ed)) { cop = op; ced = ed; }
    if (cop != std::string::npos) {
        size_t c0 = x.open("circuitConfiguration", x.gt(cop) + 1, ced);
        size_t c0e = c0 == std::string::npos ? ced : x.end("circuitConfiguration", c0);
        if (c0 != std::string::npos) {
            size_t co = x.open("circuitOutline", c0, c0e);
            if (co != std::string::npos) {
                np.ckt_w = x.num(co, "x"); np.ckt_h = x.num(co, "y");
                np.has_ckt = np.ckt_w > 0;
            }
        }
    }
    {
        size_t nm = std::string::npos, nme = 0;
        for (const char* root : {"model", "core"}) {
            if (x.path({root, "pwbData", "circuitConfigurationData"}, op, ed)) {
                nm = x.open("nonMatrix", op, ed);
                if (nm != std::string::npos) { nme = x.end("nonMatrix", nm); break; }
            }
        }
        if (nm != std::string::npos) {
            for (size_t p = x.open("allocation", nm, nme); p != std::string::npos; p = x.open("allocation", x.gt(p) + 1, nme))
                np.circuits.push_back({x.num(p, "x"), x.num(p, "y"), norm_deg(x.num(p, "angle"))});
        } else if (cop != std::string::npos) {
            size_t mo = x.open("matrix", cop, ced);
            if (mo != std::string::npos) {
                size_t me = x.end("matrix", mo);
                size_t dn = x.open("divideNumber", mo, me), mp = x.open("matrixPitch", mo, me);
                const int dx = dn == std::string::npos ? 1 : (int)std::lround(x.num(dn, "x", 1));
                const int dy = dn == std::string::npos ? 1 : (int)std::lround(x.num(dn, "y", 1));
                const double px = mp == std::string::npos ? 0 : x.num(mp, "x");
                const double py = mp == std::string::npos ? 0 : x.num(mp, "y");
                if (dx * dy > 1)
                    for (int j = 0; j < dy; ++j)
                        for (int i = 0; i < dx; ++i) np.circuits.push_back({i * px, j * py, 0});
            }
        }
    }
    np.panel = np.circuits.size() > 1;
    if (!np.panel) np.circuits.clear();

    // components (core)
    std::map<std::string, int> part_idx;
    size_t core_o = x.open("core", 0, text.size());
    size_t core_e = core_o == std::string::npos ? text.size() : x.end("core", core_o);
    if (core_o == std::string::npos) core_o = 0;
    for (size_t p = x.open("component", core_o, core_e); p != std::string::npos;) {
        size_t e = x.end("component", p);
        std::string nm = x.text("componentName", p, e);
        if (!nm.empty() && !part_idx.count(nm)) {
            NPart c;
            c.name = nm;
            c.desc = x.text("comment", p, e);
            size_t cs = x.open("componentSize", p, e);
            if (cs != std::string::npos) {
                c.len = x.num(cs, "length");
                c.wid = x.num(cs, "witdh", x.num(cs, "width"));
                c.hgt = x.num(cs, "height");
            }
            part_idx[nm] = (int)np.parts.size();
            np.parts.push_back(c);
        }
        p = x.open("component", e, core_e);
    }
    // placements (core)
    for (size_t p = x.open("placement", core_o, core_e); p != std::string::npos;) {
        size_t e = x.end("placement", p);
        size_t pp = x.open("placementPosition", p, e);
        if (pp != std::string::npos && !x.attr(p, "index").empty()) {
            NPlace q;
            q.ref = x.text("placementId", p, e);
            q.x = x.num(pp, "x"); q.y = x.num(pp, "y");
            size_t pa = x.open("placementAngle", p, e);
            if (pa != std::string::npos) q.rot = norm_deg(x.num(pa, "angle"));
            size_t sk = x.open("skip", p, e);
            if (sk != std::string::npos) q.skip = truthy(x.attr(sk, "placement"));
            const std::string cn = x.text("componentName", p, e);
            if (!cn.empty()) {
                auto it = part_idx.find(cn);
                if (it == part_idx.end()) {
                    NPart c; c.name = cn;
                    it = part_idx.emplace(cn, (int)np.parts.size()).first;
                    np.parts.push_back(c);
                }
                q.part = it->second;
            }
            if (q.ref.empty()) q.ref = "P" + std::to_string(np.places.size() + 1);
            np.places.push_back(q);
        }
        p = x.open("placement", e, core_e);
    }
    // marks (dedupe by position, 1 um)
    for (size_t p = x.open("markPosition", 0, text.size()); p != std::string::npos; p = x.open("markPosition", x.gt(p) + 1, text.size())) {
        NMark m; m.x = x.num(p, "x"); m.y = x.num(p, "y");
        bool dup = false;
        for (auto& o : np.marks) if (std::fabs(o.x - m.x) <= 0.001 && std::fabs(o.y - m.y) <= 0.001) dup = true;
        if (!dup) np.marks.push_back(m);
    }
    if (np.marks.size() > 3) res.warnings.push_back("more than 3 distinct marks; all imported as global fiducials (exporter writes the first 3)");
    if (np.places.empty()) { err = "ISS program has no placements"; return false; }
    return true;
}

} // namespace

// ===========================================================================
int sniff_juki(const std::vector<uint8_t>& h) {
    if (h.size() >= 5) {
        std::string s(h.begin(), h.begin() + std::min<size_t>(h.size(), 4096));
        if (s.find("<productionProgram") != std::string::npos) return 95;
        if (s.find("<?xml") != std::string::npos) return 0;
    }
    if (h.size() < 38 + 16 * 13) return 0;
    const uint32_t a0 = rd_u32(h, 38);
    if (a0 != 38 + 16 * 26 && a0 != 38 + 16 * 27 && a0 != 38 + 16 * 28) return 0;
    const uint32_t prs = rd_u32(h, 38 + 16 * 3 + 8);
    if (prs != 71 && prs != 96 && prs != 256) return 10;
    return 90;
}

bool import_hlc(const std::vector<uint8_t>& data, Project& prj, const ImportOptions& opt, ImportResult& res,
                std::string& err) {
    res = ImportResult{};
    NProgram np;
    if (!read_hlc(data, np, res, err)) return false;
    return build_project(np, prj, opt, res, err);
}

bool import_iss(const std::string& xml, Project& prj, const ImportOptions& opt, ImportResult& res,
                std::string& err) {
    res = ImportResult{};
    NProgram np;
    if (!read_iss(xml, np, res, err)) return false;
    return build_project(np, prj, opt, res, err);
}

bool import_juki(const std::vector<uint8_t>& data, Project& prj, const ImportOptions& opt, ImportResult& res,
                 std::string& err) {
    size_t i = 0;
    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) i = 3;
    while (i < data.size() && (data[i] == ' ' || data[i] == '\r' || data[i] == '\n' || data[i] == '\t')) ++i;
    if (i < data.size() && data[i] == '<')
        return import_iss(std::string(data.begin() + (long)i, data.end()), prj, opt, res, err);
    return import_hlc(data, prj, opt, res, err);
}

} // namespace pcbp::juki
