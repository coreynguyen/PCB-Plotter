// recentre.cpp — see recentre.h
#include "recentre.h"
#include "pnp_program.h"
#include "../core/machine_job.h"
#include "../core/project_io.h"
#include "../core/fileio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace pcbp::io {

namespace {
struct Cand { double ox, oy, cx, cy; std::string part, package; };   // origin point and corrected point, machine frame
typedef std::multimap<std::string, Cand> CandMap;

// the project's placements for one process point, both ways
bool point_cands(const Project& prj, int pt, CandMap& m) {
    Project po = prj, pc = prj;
    po.model.points[(size_t)pt].machine.place_at_origin = true;
    pc.model.points[(size_t)pt].machine.place_at_origin = false;
    MachineJob jo, jc; std::string e;
    if (!build_machine_job(po, pt, jo, e) || !build_machine_job(pc, pt, jc, e)) return false;
    if (jo.placements.size() != jc.placements.size()) return false;
    for (size_t i = 0; i < jo.placements.size(); ++i) {
        const JobPlacement& a = jo.placements[i]; const JobPlacement& b = jc.placements[i];
        m.insert({a.ref, Cand{a.x, a.y, b.x, b.y, a.part, a.package}});
    }
    return !m.empty();
}
// The constant shift program - project: the most common difference over every (placement, same-reference candidate)
// pair, on a 0.01 mm grid, taken against the origin points and against the corrected points; whichever explains more.
struct Shift { double x = 0, y = 0; int votes = 0; };
Shift best_shift(const PnpProgram& p, const CandMap& m, bool corrected) {
    std::map<std::pair<long, long>, int> h;
    for (auto& q : p.placements) {
        auto r = m.equal_range(q.ref);
        for (auto it = r.first; it != r.second; ++it) {
            const double dx = q.x - (corrected ? it->second.cx : it->second.ox), dy = q.y - (corrected ? it->second.cy : it->second.oy);
            ++h[{std::lround(dx * 100), std::lround(dy * 100)}];
        }
    }
    Shift s;
    for (auto& kv : h) if (kv.second > s.votes) { s.votes = kv.second; s.x = kv.first.first / 100.0; s.y = kv.first.second / 100.0; }
    // refine to the mean of the pairs in that cell (the grid is coarser than the data)
    double sx = 0, sy = 0; int n = 0;
    for (auto& q : p.placements) {
        auto r = m.equal_range(q.ref);
        for (auto it = r.first; it != r.second; ++it) {
            const double dx = q.x - (corrected ? it->second.cx : it->second.ox), dy = q.y - (corrected ? it->second.cy : it->second.oy);
            if (std::fabs(dx - s.x) < 0.011 && std::fabs(dy - s.y) < 0.011) { sx += dx; sy += dy; ++n; }
        }
    }
    if (n) { s.x = sx / n; s.y = sy / n; }
    return s;
}
}   // namespace

bool recentre_program(const std::vector<uint8_t>& in, const Project& prj, int point_index, double tol,
                      std::vector<uint8_t>& out, RecentreReport& rep, std::string& err) {
    rep = RecentreReport();
    if (tol <= 0) tol = 0.02;
    PnpProgram p;
    if (!pnp_load(in, p, err)) return false;
    rep.format = p.format; rep.placements = (int)p.placements.size();
    if (p.placements.empty()) { err = "the program has no placements"; return false; }
    // which process point of the project is this program? The one whose placements explain the most of it.
    int best_pt = -1; CandMap best_m; Shift best_s; bool best_corr = false;
    for (int pt = 0; pt < (int)prj.model.points.size(); ++pt) {
        if (point_index >= 0 && pt != point_index) continue;
        if (!prj.model.points[(size_t)pt].has_machine) continue;
        CandMap m;
        if (!point_cands(prj, pt, m)) continue;
        const Shift so = best_shift(p, m, false), sc = best_shift(p, m, true);
        const bool corr = sc.votes > so.votes;
        const Shift& s = corr ? sc : so;
        if (s.votes > best_s.votes) { best_s = s; best_pt = pt; best_m = m; best_corr = corr; }
    }
    if (best_pt < 0 || best_s.votes < 3 || best_s.votes * 2 < rep.placements) {
        char b[200]; std::snprintf(b, sizeof b, "the project does not match this program (%d of %d placements line up)", best_s.votes, rep.placements);
        err = b; return false;
    }
    rep.point = prj.model.points[(size_t)best_pt].name; rep.shift_x = best_s.x; rep.shift_y = best_s.y;
    // When most of the program lines up with the corrected points, the shift was measured against those; turn it into
    // the shift against the origin points (the same number: both sets live in one frame) and go on.
    (void)best_corr;
    for (auto& q : p.placements) {
        RecentreRow row; row.ref = q.ref; row.circuit = q.circuit; row.x0 = row.x1 = q.x; row.y0 = row.y1 = q.y;
        auto r = best_m.equal_range(q.ref);
        if (r.first == r.second) { row.what = 3; ++rep.unknown; rep.rows.push_back(row); continue; }
        ++rep.matched;
        // the candidate (panel image) this placement is: nearest by origin point, else by corrected point
        const Cand* at_o = nullptr; const Cand* at_c = nullptr; const Cand* any = nullptr; double bo = 1e18, bc = 1e18;
        for (auto it = r.first; it != r.second; ++it) {
            const Cand& c = it->second; any = &c;
            const double eo = std::hypot(q.x - best_s.x - c.ox, q.y - best_s.y - c.oy), ec = std::hypot(q.x - best_s.x - c.cx, q.y - best_s.y - c.cy);
            if (eo < bo) { bo = eo; at_o = &c; }
            if (ec < bc) { bc = ec; at_c = &c; }
        }
        const Cand* c = bo <= tol ? at_o : bc <= tol ? at_c : any;
        row.part = c->part; row.package = c->package;
        const double mv = std::hypot(c->cx - c->ox, c->cy - c->oy);
        if (mv < 1e-9) { row.what = 4; ++rep.centred; }                       // centred package: origin = lands centre
        else if (bo <= tol) {                                                  // still on the CAD origin: move it
            q.x += at_o->cx - at_o->ox; q.y += at_o->cy - at_o->oy;
            row.x1 = q.x; row.y1 = q.y; row.what = 0; ++rep.moved; rep.largest = std::max(rep.largest, mv);
        }
        else if (bc <= tol) { row.what = 1; ++rep.already; }
        else { row.what = 2; ++rep.elsewhere; }
        rep.rows.push_back(row);
    }
    if (!rep.moved) { out = in; return true; }
    std::vector<std::string> notes;
    if (!pnp_save(p, out, err, &notes)) return false;
    for (auto& n : notes) rep.notes.push_back(n);
    // read it back: the same placements at the new points, nothing else different in the model
    PnpProgram chk; std::string e2;
    if (!pnp_load(out, chk, e2) || chk.placements.size() != p.placements.size()) { err = "the corrected file does not read back: " + e2; return false; }
    for (size_t i = 0; i < p.placements.size(); ++i)
        if (std::fabs(chk.placements[i].x - p.placements[i].x) > 0.0006 || std::fabs(chk.placements[i].y - p.placements[i].y) > 0.0006 ||
            chk.placements[i].ref != p.placements[i].ref || std::fabs(chk.placements[i].rot - p.placements[i].rot) > 1e-6) {
            err = "the corrected file reads back differently at " + p.placements[i].ref; return false;
        }
    return true;
}

std::string recentre_text(const RecentreReport& r, bool all) {
    char b[400]; std::string s;
    std::snprintf(b, sizeof b, "%s program, %d placements; matches process point '%s' (program - project = %.3f, %.3f mm)\n",
                  r.format.c_str(), r.placements, r.point.c_str(), r.shift_x, r.shift_y); s += b;
    std::snprintf(b, sizeof b, "  moved to the centre of the lands : %d (largest move %.3f mm)\n  already there                    : %d\n"
                  "  somewhere else, left alone       : %d\n  package centred, nothing to do   : %d\n  not in the project               : %d\n",
                  r.moved, r.largest, r.already, r.elsewhere, r.centred, r.unknown); s += b;
    static const char* w[] = {"moved", "already", "ELSEWHERE", "unknown", "centred"};
    for (auto& q : r.rows) {
        if (!all && q.what != 0 && q.what != 2 && q.what != 3) continue;
        std::snprintf(b, sizeof b, "  %-10s %-9s %9.3f %9.3f -> %9.3f %9.3f  %s  %s\n", q.ref.c_str(), w[q.what], q.x0, q.y0, q.x1, q.y1, q.part.c_str(), q.package.c_str()); s += b;
    }
    for (auto& n : r.notes) s += "  note: " + n + "\n";
    return s;
}

int recentre_cli(const std::vector<std::string>& args, std::string& out) {
    std::vector<std::string> files; std::string proj, outp; bool inplace = false, dry = false, all = false; int point = -1; double tol = 0.02;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&](std::string& v) { if (i + 1 >= args.size()) return false; v = args[++i]; return true; };
        std::string v;
        if (a == "--project") { if (!next(proj)) { out = "--project needs a file\n"; return 2; } }
        else if (a == "--out") { if (!next(outp)) { out = "--out needs a file\n"; return 2; } }
        else if (a == "--point") { if (!next(v)) { out = "--point needs a number\n"; return 2; } point = std::atoi(v.c_str()); }
        else if (a == "--tol") { if (!next(v)) { out = "--tol needs mm\n"; return 2; } tol = std::atof(v.c_str()); }
        else if (a == "--in-place") inplace = true;
        else if (a == "--dry-run") dry = true;
        else if (a == "--all") all = true;
        else if (a.size() > 2 && a[0] == '-' && a[1] == '-') { out = "unknown option " + a + "\n"; return 2; }
        else files.push_back(a);
    }
    if (files.empty() || proj.empty() || (!outp.empty() && files.size() > 1) || (!outp.empty() && inplace)) {
        out = "usage: --recentre <program> [<program> ...] --project <file.pcbp> [--out <file> | --in-place] [--dry-run] [--point <n>] [--tol <mm>] [--all]\n"
              "Moves the placements of a program exported before R25 from the CAD footprint origin to the centre of the\n"
              "package's lands. Only placements still on the CAD origin are moved; taught ones are listed and left alone.\n";
        return 2;
    }
    Project prj; std::string err;
    if (!load_project(proj, prj, err)) { out = "cannot open the project " + proj + ": " + err + "\n"; return 1; }
    int rc = 0;
    for (auto& f : files) {
        out += f + "\n";
        std::vector<uint8_t> in, res; RecentreReport rep;
        if (!read_file(f, in, err)) { out += "  cannot read: " + err + "\n"; rc = 1; continue; }
        if (!recentre_program(in, prj, point, tol, res, rep, err)) { out += "  not changed: " + err + "\n"; rc = 1; continue; }
        out += recentre_text(rep, all);
        if (!rep.moved) { out += "  nothing to move: no file written\n"; continue; }
        if (dry) { out += "  dry run: no file written\n"; continue; }
        std::string dst = outp;
        if (inplace) {
            const std::string keep = f + ".orig"; std::vector<uint8_t> probe; std::string e2;
            if (!read_file(keep, probe, e2)) { if (!write_file_verified(keep, in.data(), in.size(), err)) { out += "  cannot keep the original as " + keep + ": " + err + "\n"; rc = 1; continue; } }
            dst = f;
        } else if (dst.empty()) {
            const size_t dot = f.find_last_of('.'), sl = f.find_last_of("\\/");
            dst = (dot == std::string::npos || (sl != std::string::npos && dot < sl)) ? f + " (centred)" : f.substr(0, dot) + " (centred)" + f.substr(dot);
        }
        if (!write_file_verified(dst, res.data(), res.size(), err)) { out += "  cannot write " + dst + ": " + err + "\n"; rc = 1; continue; }
        out += "  written: " + dst + "\n";
    }
    return rc;
}

} // namespace pcbp::io
