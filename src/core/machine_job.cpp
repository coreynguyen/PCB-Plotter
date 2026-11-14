// ============================================================================
// machine_job.cpp — Project + model point -> neutral MachineJob.
//
// Transform order (per placement, per panel image):
//   1. board position  = image transform (offset+rotation) applied to the
//                        root-image centroid (or CAD centroid if requested)
//   2. relative to origin (machine-specific virtual origin, else global)
//   3. flip (bottom side): mirror X about the origin
//   4. board orientation: rotate about the origin by 0/90/180/270
//   5. hard offset added
// Angles: export_angle() only (specs/ANGLES.md §5).
// ============================================================================
#include <set>
#include "machine_job.h"
#include "project.h"
#include "plot_align.h"
#include "machine_assign.h"   // R24b   // R14 outline checks
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace pcbp {

static Vec2 xf_point(Vec2 p, const MachineSetup& ms, Vec2 origin) {
    p = p - origin;
    if (ms.flipped) p.x = -p.x;
    p = rotate(p, (double)ms.orientation);
    return p + ms.hard_offset;
}

static double xf_angle(double a, const MachineSetup& ms) {
    if (ms.flipped && ms.mirror_angle_when_flipped) a = -a;
    return norm_deg(a + ms.orientation);
}

// R16b tape orientation. R18a (specs/ANGLES.md §10): no longer applied to the machine angle; the package graphic
// is drawn as the part sits in cut tape and the supply angle is per part number (component data).
int package_tape_rot(const Project& prj, const Component& c) {
    auto it = prj.packages.find(c.package);
    if (it != prj.packages.end()) return it->second.tape_rot;
    const Package* pk = prj.package_of(c);   // part-number package fallback
    return pk ? pk->tape_rot : 0;
}

double export_angle(const Project& prj, const Component& c, int image, const MachineSetup& ms) {
    double a = c.rot;
    if (image >= 0 && image < (int)prj.images.size()) a += prj.images[image].rot;
    if (ms.flipped && ms.mirror_angle_when_flipped) a = -a;
    return norm_deg(a + ms.orientation);
}

std::string tape_rot_notice(const Project& prj) {
    if (prj.tape_rot_notice_ack) return {};
    std::string list;
    int n = 0;
    for (auto& [name, pk] : prj.packages)
        if (pk.tape_rot % 360) {
            if (n < 20) list += "\n  " + name + " (" + std::to_string(pk.tape_rot) + "\xC2\xB0)";
            ++n;
        }
    if (!n) return {};
    if (n > 20) list += "\n  ... and " + std::to_string(n - 20) + " more";
    return "These packages have a tape orientation from an earlier version:" + list +
           "\n\nTape orientation is no longer applied to placement angles. Machine angle = placement rotation"
           " (plus the package rules). Draw the package as the part sits in cut tape, and set the Supply angle"
           " per part number for sticks / trays. The stored value is kept in the file but has no effect.";
}

std::string angle_convention_text(const MachineSetup& ms) {
    std::string s = "machine angle = placement rotation (CCW)";
    s += " + panel image rotation";
    if (ms.flipped) s += ms.mirror_angle_when_flipped ? ", negated (flipped, mirror rule)" : ", unchanged by flip (CircuitCAM rule)";
    if (ms.orientation) s += " + " + std::to_string(ms.orientation) + " board orientation";
    s += "; pre-rotation (CW) not added";
    return s;
}
std::string setup_signature(const MachineSetup& ms, Vec2 origin) {
    char b[160];
    std::snprintf(b, sizeof b, "f%d o%d h%.4f,%.4f g%.4f,%.4f", ms.flipped ? 1 : 0, (int)ms.orientation,
                  ms.hard_offset.x, ms.hard_offset.y, origin.x, origin.y);
    return b;
}

// v17: Board ID, else Project Name, else the legacy Assembly No. (no longer shown in Project Info), else "".
std::string project_program_name(const Project& p) {
    if (!p.info.name.empty()) return p.info.name;      // R18d: Project Name first, then Board ID
    if (!p.info.pcb_id.empty()) return p.info.pcb_id;
    return p.info.number;
}

int linked_point_index(const Project& prj, int point_index) {
    if (point_index < 0 || point_index >= (int)prj.model.points.size()) return -1;
    const ModelPoint& me = prj.model.points[point_index];
    if (me.link <= 0) return -1;
    for (int i = 0; i < (int)prj.model.points.size(); ++i)
        if (i != point_index && prj.model.points[i].link == me.link && prj.model.points[i].has_machine &&
            prj.model.points[i].machine.side != me.machine.side) return i;
    return -1;
}

bool build_machine_job(const Project& prj, int point_index, MachineJob& job, std::string& err) {
    job = MachineJob{};
    if (point_index < 0 || point_index >= (int)prj.model.points.size()) { err = "invalid model point"; return false; }
    const ModelPoint& mp = prj.model.points[point_index];
    const MachineSetup& ms = mp.machine;
    const Vec2 origin = ms.has_origin ? ms.origin : prj.virtual_origin;

    // v17: an imported program keeps its own name (option juki.program_name, set by the importer);
    // otherwise Project Name, then Board ID, then the legacy hidden Assembly No., then "PROGRAM".
    if (auto it = ms.options.find("juki.program_name"); it != ms.options.end() && !it->second.empty()) job.program_name = it->second;
    else job.program_name = project_program_name(prj);
    if (job.program_name.empty()) job.program_name = "PROGRAM";
    job.assembly = prj.info.name;
    job.assembly_rev = prj.info.assembly_rev;
    job.customer = prj.info.customer;
    job.point_name = mp.name;
    job.pcb_id = prj.info.pcb_id;
    job.originator = prj.info.originator;
    job.setup_sig = setup_signature(ms, origin);
    job.machine = ms.exporter_id;
    job.bottom_side = (ms.side == Side::Bottom) || ms.flipped;
    job.single_board = ms.single_board;
    job.options = ms.options;

    // board extent in machine orientation
    BBox bb = prj.board_bbox();
    // R18d: the board is the panel/board outline whenever one is drawn, also for a single circuit (marks in the
    // margin around the circuit are on the board); the circuit frame is only the circuit block.
    if (!prj.panel_frame.empty()) {
        BBox pb; for (auto& poly : prj.panel_frame) for (auto& p : poly) pb.add(p);
        if (!pb.empty()) { if (prj.images.size() > 1) bb = pb; else { bb.add({pb.x0, pb.y0}); bb.add({pb.x1, pb.y1}); } }
    } else if (prj.images.size() > 1) {
        // R22: an array without a board outline: the board is at least every instance's circuit extent (an
        // inferred size, flagged by validate_job), never only the reference circuit
        const BBox cb = bb;
        for (int i = 1; i < (int)prj.images.size(); ++i) {
            const Xform t = prj.image_xform(i);
            for (Vec2 q : {Vec2{cb.x0, cb.y0}, Vec2{cb.x1, cb.y0}, Vec2{cb.x1, cb.y1}, Vec2{cb.x0, cb.y1}}) bb.add(t.apply(q));
        }
    }
    {
        BBox mb;
        Vec2 corners[4] = {{bb.x0, bb.y0}, {bb.x1, bb.y0}, {bb.x1, bb.y1}, {bb.x0, bb.y1}};
        for (auto& c : corners) mb.add(xf_point(c, ms, origin));
        job.board_w = mb.w(); job.board_h = mb.h();
        job.board_x0 = mb.x0; job.board_y0 = mb.y0;
    }
    // Circuit (one panel image) frame. A panel without a drawn circuit frame uses
    // the root image's board extent so the HLC circuit block is never empty.
    if (prj.is_panel()) {
        const BBox cb = prj.has_circuit_frame() ? prj.circuit_frame_bbox() : prj.board_bbox();
        BBox mb;
        Vec2 corners[4] = {{cb.x0, cb.y0}, {cb.x1, cb.y0}, {cb.x1, cb.y1}, {cb.x0, cb.y1}};
        for (auto& c : corners) mb.add(xf_point(c, ms, origin));
        job.has_circuit = !mb.empty() && mb.w() > 0 && mb.h() > 0;
        job.circuit_x = mb.x0; job.circuit_y = mb.y0; job.circuit_w = mb.w(); job.circuit_h = mb.h();
    }

    // circuits
    std::vector<int> imgs;
    for (int i = 0; i < (int)prj.images.size(); ++i) {
        const PanelImage& im = prj.images[i];
        JobCircuit jc;
        jc.index = i + 1;
        jc.name = im.name.empty() ? std::to_string(i + 1) : im.name;
        Vec2 o = i == 0 ? Vec2{} : im.offset;          // delta from root image
        if (ms.flipped) o.x = -o.x;
        o = rotate(o, (double)ms.orientation);
        jc.x = o.x; jc.y = o.y; jc.rot = xf_angle(im.rot, ms);
        jc.skip = im.x_out;
        {   // circuit-1 -> circuit-i rigid map in machine coords (two-point fit)
            const Xform ix = prj.image_xform(i);
            const Vec2 a1 = xf_point({0, 0}, ms, origin), b1 = xf_point({1, 0}, ms, origin);
            const Vec2 ai = xf_point(ix.apply({0, 0}), ms, origin), bi = xf_point(ix.apply({1, 0}), ms, origin);
            const double th = std::atan2(bi.y - ai.y, bi.x - ai.x) - std::atan2(b1.y - a1.y, b1.x - a1.x);
            const Vec2 r1 = rotate(a1, rad2deg(th));
            jc.arot = i == 0 ? 0.0 : norm_deg(rad2deg(th));
            if (jc.arot > 359.9999999) jc.arot = 0;
            jc.ax = i == 0 ? 0.0 : ai.x - r1.x;
            jc.ay = i == 0 ? 0.0 : ai.y - r1.y;
        }
        job.circuits.push_back(jc);
        imgs.push_back(i);
        if (ms.single_board) break;
    }
    if (ms.single_board) { imgs.clear(); for (int i = 0; i < (int)prj.images.size(); ++i) imgs.push_back(i); }

    std::map<std::string, JobPart> parts;
    int fid_n = 0;
    job.no_marks = ms.no_marks;
    // R24a (user v23, replaces AUDIT_V20 "strictly this side"): fiducials are normally pass-through holes / pads at
    // the same physical spot on both sides. A side with its OWN fiducials uses only those. A side without any takes
    // the fiducials of its LINKED export layer (same link ID, other side). They are physical board positions in the
    // same root board coordinates as every placement, so they go through this side's xf_point() (orientation,
    // flip mirror, origin, offset) exactly like this side's placements -- never the other side's raw machine values.
    Side fid_side = ms.side;
    {
        bool own = false;
        for (const Component& c : prj.comps) own |= c.feature == Feature::Fiducial && c.side == ms.side;
        if (!own) {
            const int lp = linked_point_index(prj, point_index);
            if (lp >= 0) {
                const Side ls = prj.model.points[lp].machine.side;
                bool theirs = false;
                for (const Component& c : prj.comps) theirs |= c.feature == Feature::Fiducial && c.side == ls && c.fid_scope != FiducialScope::Local;
                if (theirs && ls != ms.side) { fid_side = ls; job.marks_from = prj.model.points[lp].name; }
            }
        }
    }
    for (const Component& c : prj.comps) {
        const PartNumber* pn = prj.part_of(c);
        const Package* pk = prj.package_of(c);
        if (c.feature == Feature::Fiducial) {
            // R24a: this side's own marks, or (only when it has none) the linked layer's board / circuit marks
            if (c.side != fid_side) continue;
            if (fid_side != ms.side && c.fid_scope == FiducialScope::Local) continue;   // component marks stay with their side
            const Component* owner = nullptr;
            if (c.fid_scope == FiducialScope::Local) {
                for (auto& o : prj.comps) if ((int)o.id == c.local_fid_owner) { owner = &o; break; }
                // placement-local: only when its owner is placed by this point
                if (owner && owner->assigned_point != point_index) continue;
            }
            const bool local = c.fid_scope != FiducialScope::Global;
            const std::vector<int> fi = local ? imgs : std::vector<int>{0};
            for (int im : fi) {
                JobFiducial f;
                f.name = c.ref.empty() ? ("FID" + std::to_string(++fid_n)) : c.ref;
                Vec2 p = prj.instance_xform(c, im).apply({0, 0});
                p = xf_point(p, ms, origin);
                f.x = p.x; f.y = p.y;
                f.local = local;
                f.circuit = local ? (ms.single_board ? 1 : im + 1) : 0;
                if (owner) f.owner = owner->ref;
                f.number = c.fid_number;   // R21 P1: Fiducial 1/2/3 selects the written marks
                if (pk && !pk->pads.empty()) f.diameter = std::max(pk->pads[0].w, pk->pads[0].h);
                job.fiducials.push_back(f);
            }
            continue;
        }
        if (c.feature != Feature::Component || c.assigned_point != point_index) continue;
        for (int im : imgs) {
            JobPlacement p;
            p.ref = c.ref;
            p.part = c.number.empty() ? c.package : c.number;
            p.package = pk ? pk->name : c.package;
            p.description = pn ? pn->description : std::string();
            // One coordinate set and one rotation: use_cad_coords / force_angle_calc and any
            // legacy machine angle are kept only so legacy projects round-trip (ANGLES.md §5).
            const Vec2 q = xf_point(prj.image_xform(im).apply(c.pos), ms, origin);
            p.x = q.x; p.y = q.y;
            p.rot = export_angle(prj, c, im, ms);
            p.pre_rot = c.pre_rotation;
            p.circuit = ms.single_board ? 1 : im + 1;
            p.skip = c.dnp || prj.images[im].x_out;
            p.polarized = pn ? pn->polarized : (pk ? pk->polarized : false);
            p.smt = pk ? pk->smt : true;
            p.leads = pk ? pk->leads : 0;
            p.height = pn && pn->height > 0 ? pn->height : (pk ? pk->height : 0);
            if (ms.exporter_id == "europlacer.bla" && pk && pk->pads.size() == 2) {
                const Pad& first = pk->pads[0];
                const Pad& second = pk->pads[1];
                if ((first.pin == 1 && second.pin == 2) || (first.pin == 2 && second.pin == 1)) {
                    const Xform xf = prj.instance_xform(c, im);
                    p.pin1 = xf_point(xf.apply(first.pin == 1 ? first.pos : second.pos), ms, origin);
                    p.pin2 = xf_point(xf.apply(first.pin == 2 ? first.pos : second.pos), ms, origin);
                    p.has_ordered_pin_pair = true;
                }
            }
            if ((ms.exporter_id == "contact.cs400" || ms.exporter_id == "robotas.mascot" || ms.exporter_id == "universal.radial2" || ms.exporter_id == "universal.vcd" || ms.exporter_id == "universal.vcd2") && pk && pk->pads.size() >= 2) {
                const Pad* first = nullptr;
                for (const auto& pad : pk->pads) if (pad.pin == 1) {
                    if (first) { first = nullptr; break; }
                    first = &pad;
                }
                if (first) {
                    const Pad* farthest = first;
                    double distance = 0;
                    for (const auto& pad : pk->pads) {
                        const double dx = pad.pos.x - first->pos.x, dy = pad.pos.y - first->pos.y;
                        if (dx * dx + dy * dy > distance) {
                            distance = dx * dx + dy * dy;
                            farthest = &pad;
                        }
                    }
                    const Xform xf = prj.instance_xform(c, im);
                    p.scan1 = xf_point(xf.apply(first->pos), ms, origin);
                    p.scan2 = xf_point(xf.apply(farthest->pos), ms, origin);
                    p.has_scan_pair = distance > 0;
                }
            }
            const char* hsrc = pn && pn->height > 0 ? "part" : (pk && pk->height > 0 ? "package" : "");
            job.placements.push_back(p);

            JobPart& jp = parts[p.part];
            if (jp.part.empty()) {
                jp.part = p.part; jp.package = p.package; jp.description = p.description;
                jp.polarized = p.polarized; jp.smt = p.smt; jp.leads = p.leads; jp.height = p.height;
                jp.height_src = hsrc;
                if (pk) {   // R24a (user v23 #2): the package BODY (laser-checked), not the lead / pad extent
                    BBox e = pkg_body_box(*pk, &jp.size_src);
                    jp.length = e.w(); jp.width = e.h();
                }
                if (pn) { jp.feeder_kind = packaging_feeder_kind(pn->packaging); jp.supply_angle = effective_supply_angle(*pn); }   // R18a
                // R24b machine data: package suggestion + overrides (core/machine_assign.h). A part number imported
                // from a machine program (supply angle kept verbatim) keeps its bytes unless the user set overrides /
                // assigned the package, so a re-export stays byte-identical.
                const bool verbatim = pn && (pn->supply_angle_set || !pn->packaging.empty()) && !(pk && (pk->mach_assigned || !pk->mach.empty()));   // explicit part data (BOM / program) wins until assigned
                if (verbatim) {   // only what the imported program carried
                    jp.type_code = pn->prog_type;
                    if (jp.feeder_kind == 1) { jp.tape_code = pn->prog_tape; jp.pitch_code = pn->prog_pitch; }
                    jp.mach_conf = 1.0;
                } else if (pk) {
                    mach::Suggestion sg;
                    mach::Fields f = mach::effective_for(*pk, pn, &sg);
                    jp.feeder_kind = packaging_feeder_kind(f.packaging);   // suggest() already honours PartNumber::packaging
                    jp.supply_angle = (pn && pn->supply_angle_set) ? pn->supply_angle : f.supply_angle;
                    jp.type_code = f.type_code;
                    jp.nozzle = f.nozzle;
                    jp.centring = f.centring;
                    if (f.packaging == "Tape") { jp.tape_code = mach::h8h_tape_code(f.tape_w, f.carrier); jp.pitch_code = mach::h8h_pitch_code(f.pitch); }
                    jp.mach_conf = pk->mach.empty() ? sg.conf : 1.0;
                    jp.mach_assigned = pk->mach_assigned;
                }
            }
            jp.count++;
        }
    }
    for (auto& kv : parts) job.parts.push_back(kv.second);
    std::sort(job.placements.begin(), job.placements.end(), [](const JobPlacement& a, const JobPlacement& b) {
        if (a.circuit != b.circuit) return a.circuit < b.circuit;
        return ref_less(a.ref, b.ref);
    });
    if (job.placements.empty()) {
        err = "No components are assigned to model point '" + mp.name + "'.";
        return false;
    }
    if (job.fiducials.empty() && !ms.no_marks)
        job.warnings.push_back(std::string("No fiducial marks on the ") + (ms.side == Side::Bottom ? "bottom" : "top") + " side" +
                               (linked_point_index(prj, point_index) >= 0 ? " or on its linked layer." : " (and no linked layer)."));
    return true;
}

// ---------------------------------------------------------------------------
// transform chain (§18)
// ---------------------------------------------------------------------------
static std::string fnum(double v, int prec = 4) {
    char b[64]; std::snprintf(b, sizeof b, "%.*f", prec, v);
    std::string s = b;
    if (s.find('.') != std::string::npos) { while (s.back() == '0') s.pop_back(); if (s.back() == '.') s.pop_back(); }
    if (s == "-0") s = "0";
    return s;
}
static std::string fxy(Vec2 p) { return "(" + fnum(p.x) + ", " + fnum(p.y) + ") mm"; }
static std::string fdeg(double a) { return fnum(a) + "\xC2\xB0"; }

bool trace_placement(const Project& prj, int point_index, size_t ci, int im, PlacementTrace& t, std::string& err) {
    t = PlacementTrace{};
    if (point_index < 0 || point_index >= (int)prj.model.points.size()) { err = "invalid model point"; return false; }
    if (ci >= prj.comps.size()) { err = "invalid component"; return false; }
    if (im < 0 || im >= (int)prj.images.size()) { err = "invalid panel image"; return false; }
    const MachineSetup& ms = prj.model.points[point_index].machine;
    const Component& c = prj.comps[ci];
    const Vec2 origin = ms.has_origin ? ms.origin : prj.virtual_origin;
    auto add = [&](const std::string& l, const std::string& v) { t.steps.push_back({l, v}); };
    t.ref = c.ref;
    add("1 Source placement", fxy(prj.to_user(c.cad_pos)) + "  rot " + fdeg(c.rot_imported));
    const Package* pk = prj.package_of(c);
    if (pk) {
        const BBox e = pk->extent();
        const Vec2 ctr{(e.x0 + e.x1) * 0.5, (e.y0 + e.y1) * 0.5};
        add("2 Package", pk->name + "  origin->body centre " + fxy(ctr) + " (artwork rotation 0\xC2\xB0)");
    } else {
        add("2 Package", c.package.empty() ? "(none)" : c.package + " (not in library)");
    }
    add("3 Edited placement", fxy(prj.to_user(c.pos)) + "  rot " + fdeg(c.rot) + "  (delta " +
        fdeg(norm_deg(c.rot - c.rot_imported)) + " from imported)");
    if (c.pre_rotation != 0) add("  Pre-rotation", fdeg(c.pre_rotation) + " CW (metadata, not added)");
    add("4 Physical side", std::string(c.side == Side::Bottom ? "Bottom" : "Top") + "; setup programs " +
        (ms.side == Side::Bottom ? "Bottom" : "Top") + (ms.flipped ? ", flipped" : ""));
    const PanelImage& pi = prj.images[im];
    const Vec2 bp = prj.image_xform(im).apply(c.pos);
    add("5 Panel image", std::to_string(im + 1) + " offset " + fxy(pi.offset) + " rot " + fdeg(pi.rot) + " -> " +
        fxy(prj.to_user(bp)) + "  rot " + fdeg(norm_deg(c.rot + pi.rot)));
    add("6 Export origin", std::string(ms.has_origin ? "machine origin " : "virtual origin ") + fxy(origin));
    add("7 Mirror", ms.flipped ? std::string("X mirrored; angle ") + (ms.mirror_angle_when_flipped ? "negated" : "kept")
                               : std::string("none"));
    add("8 Orientation", fdeg(ms.orientation) + " CCW; hard offset " + fxy(ms.hard_offset));
    const Vec2 q = xf_point(bp, ms, origin);
    t.x = q.x; t.y = q.y; t.rot = export_angle(prj, c, im, ms);
    if (const int tr = package_tape_rot(prj, c)) add("  Tape orientation", fdeg(tr) + " (package " + c.package + "; NOT applied since R18)");
    add("9 Machine", fxy(q) + "  angle " + fdeg(t.rot));
    add("  Convention", angle_convention_text(ms));
    if (c.has_legacy_mach_rot) add("  Legacy CircuitCAM machine angle", fdeg(c.legacy_mach_rot) + " (comparison only, not exported)");
    return true;
}

std::string trace_to_text(const PlacementTrace& t) {
    std::string s = t.ref + "\n";
    for (auto& st : t.steps) s += "  " + st.label + ": " + st.value + "\n";
    return s;
}

// ---------------------------------------------------------------------------
// validation (§19 P0)
// ---------------------------------------------------------------------------
namespace {
std::vector<std::pair<std::string, JobChecker>>& job_checkers() {
    static std::vector<std::pair<std::string, JobChecker>> v;
    return v;
}
}
void register_job_checker(const char* prefix, JobChecker fn) {
    for (auto& e : job_checkers()) if (e.first == prefix) { e.second = fn; return; }
    job_checkers().push_back({prefix, fn});
}

std::vector<ExportIssue> validate_for_export(const Project& prj, int pi) {
    std::vector<ExportIssue> v;
    auto E = [&](ExportIssue::Sev s, const std::string& r, const std::string& m) { v.push_back({s, r, m}); };
    if (pi < 0 || pi >= (int)prj.model.points.size()) { E(ExportIssue::Error, "", "Invalid model point."); return v; }
    const ModelPoint& mp = prj.model.points[pi];
    const MachineSetup& ms = mp.machine;
    if (!mp.has_machine || ms.exporter_id.empty()) E(ExportIssue::Error, "", "Model point '" + mp.name + "' has no machine setup.");
    {   // R14: board / circuit outlines are physical (conveyor size, per-circuit placement limits)
        auto lim = [&](const char* k) { auto it = ms.options.find(k); return it == ms.options.end() ? 0.0 : std::atof(it->second.c_str()); };
        for (auto& fi : check_outlines(prj, lim("board_max_length"), lim("board_max_width")))
            E(fi.error ? ExportIssue::Error : ExportIssue::Warning, fi.ref, fi.ref.empty() ? "Outline: " + fi.msg : "Placement " + fi.msg + ".");
    }
    {   // R16/R17b (REVIEW_V16 §4, specs/OUTLINES.md): an unconfirmed (Inferred) board outline is an advisory
        // Warning the user can acknowledge (Use as-is / Don't warn for this project -> Info). It blocks only when
        // the machine setup demands measured geometry (require_confirmed_outline=1) or when no board size exists.
        auto opt = [&](const char* k) { auto it = ms.options.find(k); return it == ms.options.end() ? std::string() : it->second; };
        const bool require = opt("require_confirmed_outline") == "1";
        const BBox b = prj.board_bbox();
        if ((prj.board_frame.empty() && prj.panel_frame.empty() && prj.placement_extents(0.0).empty()) || b.empty() || !(b.w() > 0.01) || !(b.h() > 0.01) || !std::isfinite(b.w()) || !std::isfinite(b.h()))
            E(ExportIssue::Error, "", "Board outline missing: no outline and no placement extent, so the board size is unknown. Define the board outline (Board > Outline > Add Outline, or from the Gerbers).");
        else if (!prj.board_outline_confirmed()) {
            char wh[64]; std::snprintf(wh, sizeof wh, "%.2f x %.2f mm", b.w(), b.h());
            if (require)
                E(ExportIssue::Error, "", std::string("Board outline unconfirmed: ") + wh + " is inferred from the placement extents and this machine setup requires a confirmed outline. Confirm or define it.");
            else if (prj.inferred_outline_acknowledged())
                E(ExportIssue::Info, "", std::string("Board outline inferred (") + wh + ", placement extents + 2 mm) used as-is" + (prj.outline_warn_off ? " (warning off for this project)." : " (acknowledged)."));
            else
                E(ExportIssue::Warning, "", std::string("Board outline unconfirmed: ") + wh +
                  " is inferred from the placement extents, not measured board geometry. Use as-is, confirm it, or define it from the Gerbers.");
        } else if (prj.is_panel() && prj.circuit_outline_source() == OutlineSource::Inferred)
            E(ExportIssue::Warning, "", "Circuit outline unconfirmed: inferred from the placement extents.");
    }
    {   // R16 §12: transform consistency
        const MachineSetup& m = ms;
        if (m.orientation % 90 != 0 || m.orientation < 0 || m.orientation >= 360)
            E(ExportIssue::Error, "", "Transform: board rotation " + std::to_string(m.orientation) + " is not 0/90/180/270.");
        if (!std::isfinite(prj.virtual_origin.x) || !std::isfinite(prj.virtual_origin.y) || !std::isfinite(m.hard_offset.x) || !std::isfinite(m.hard_offset.y) ||
            (m.has_origin && (!std::isfinite(m.origin.x) || !std::isfinite(m.origin.y))))
            E(ExportIssue::Error, "", "Transform: invalid origin or offset.");
        for (auto& im : prj.images)
            if (!std::isfinite(im.rot) || !std::isfinite(im.offset.x) || !std::isfinite(im.offset.y))
                E(ExportIssue::Error, "", "Transform: panel circuit '" + im.name + "' has an invalid offset or rotation.");
        if (mp.side != m.side)
            E(ExportIssue::Warning, "", std::string("Transform: the model point is for the ") + (mp.side == Side::Bottom ? "bottom" : "top") +
              " side but its machine setup is for the " + (m.side == Side::Bottom ? "bottom" : "top") + ".");
        if (m.side == Side::Bottom && !m.flipped)
            E(ExportIssue::Warning, "", "Transform: bottom-side program without board flip: coordinates are written as seen from the top.");
        if (m.side == Side::Top && m.flipped)
            E(ExportIssue::Warning, "", "Transform: top-side program with board flip enabled.");
    }
    std::map<std::string, int> refs;
    int n = 0, fids = 0, other_side = 0, th_in_smt = 0;
    std::string th_refs;
    std::set<std::string> th_nodrill_pk;
    std::map<std::string, std::pair<int, std::string>> bom_pkg_diff;   // BOM package -> count, first refs
    std::set<std::string> unresolved;
    for (const Component& c : prj.comps) {
        if (c.feature == Feature::Fiducial && c.side == ms.side) ++fids;
        if (c.feature != Feature::Component || c.assigned_point != pi) continue;
        ++n;
        if (++refs[c.ref] == 2) E(ExportIssue::Error, c.ref, "Duplicate reference designator in this program.");
        if (c.ref.empty()) E(ExportIssue::Error, "", "Placement without reference designator.");
        if (!std::isfinite(c.rot) || !std::isfinite(c.pos.x) || !std::isfinite(c.pos.y))
            E(ExportIssue::Error, c.ref, "Invalid position or rotation.");
        if (c.package.empty() && c.number.empty()) E(ExportIssue::Error, c.ref, "No part number or package: the placement has no component.");
        else if (!prj.package_of(c)) {   // R16 §12: same rule as R16b placement_package_resolved()
            std::string nm = c.package;
            if (nm.empty()) { auto pt = prj.parts.find(c.number); if (pt != prj.parts.end()) nm = pt->second.package; }
            if (!prj.packages.count(nm)) E(ExportIssue::Warning, c.ref, "Package '" + nm + "' is not in the library: no component outline is written.");
            else {
            unresolved.insert(nm);
            E(ExportIssue::Warning, c.ref, "Package '" + c.package + "' is unresolved (no graphics): orientation and size cannot be checked.");
            }
        }
        switch (comp_state(c)) {   // BOM reconciliation (P1-04)
            case CompState::Unassigned: E(ExportIssue::Warning, c.ref, "No part number (BOM unresolved): written with the package name '" + c.package + "' as part."); break;
            case CompState::Orphan: E(ExportIssue::Warning, c.ref, "Not in the BOM but placed."); break;
            case CompState::Dnp: E(ExportIssue::Info, c.ref, "DNP: written as skipped."); break;
            default: break;
        }
        if (!c.bom_package.empty() && !c.package.empty() && c.bom_package != c.package) {   // R18d: grouped below
            auto& g = bom_pkg_diff[c.bom_package]; if (g.first++ < 6) g.second += (g.second.empty() ? "" : ", ") + c.ref;
        }
        if (c.side != ms.side) ++other_side;
        if (c.side != ms.side) E(ExportIssue::Warning, c.ref, std::string("Placement is on the ") +
                                 (c.side == Side::Bottom ? "bottom" : "top") + " side but this program is for the " +
                                 (ms.side == Side::Bottom ? "bottom" : "top") + ".");
        const Package* pk = prj.package_of(c);
        if (pk && !pk->smt && prj.process_of(c) == Process::TH && mp.kind == PointKind::Placement && mp.process_type == "SMT")
            {   // R18d: grouped Info below; TH without any drilled lead is likely the old SMT->TH import bug
                if (th_in_smt++ < 8) th_refs += (th_refs.empty() ? "" : ", ") + c.ref;
                bool drilled = false; for (auto& pd : pk->pads) drilled |= pd.drill > 0;
                if (!drilled) th_nodrill_pk.insert(c.package);
            }
        if (const auto* fr = prj.has_circuit_frame() ? prj.frame_for(c) : nullptr; fr && !point_in_frame(*fr, c.pos))   // R22: global fiducial on a panel without board outline = nullptr
            E(ExportIssue::Warning, c.ref, fr == &prj.panel_frame ? "Centroid lies outside the board outline." : "Centroid lies outside the circuit outline.");
        const double a = export_angle(prj, c, 0, ms);
        if (c.has_legacy_mach_rot && std::fabs(norm_deg(c.legacy_mach_rot - norm_deg(c.rot + prj.images[0].rot)) ) > 1e-3 &&
            std::fabs(norm_deg(c.legacy_mach_rot - norm_deg(c.rot + prj.images[0].rot)) - 360.0) > 1e-3)
            E(ExportIssue::Info, c.ref, "Legacy program angle was " + fdeg(c.legacy_mach_rot) +
              "; exported " + fdeg(a) + " (the Board View angle). For review only, not an error.");
        if (c.pre_rotation != 0 && ms.exporter_id.rfind("juki", 0) == 0)
            E(ExportIssue::Info, c.ref, "Pre-rotation " + fdeg(c.pre_rotation) + " CW is not written (no JUKI HLC placement field).");
    }
    // R18d: grouped lines instead of one per placement; process is categorisation only (Info)
    if (th_in_smt)
        E(ExportIssue::Info, "", "Through-hole package on " + std::to_string(th_in_smt) + " placement(s) in an SMT program (process is categorisation only): " +
          th_refs + (th_in_smt > 8 ? ", ..." : "") + ".");
    if (!th_nodrill_pk.empty())
        E(ExportIssue::Info, "", std::to_string(th_nodrill_pk.size()) + " package(s) are marked through-hole but have no drilled leads (" +
          *th_nodrill_pk.begin() + (th_nodrill_pk.size() > 1 ? ", ..." : "") + "): Packages > Process > Detect from drilled leads can correct them.");
    for (auto& g : bom_pkg_diff)
        E(ExportIssue::Info, "", "BOM package '" + g.first + "' differs from CAD package on " + std::to_string(g.second.first) + " placement(s): " +
          g.second.second + (g.second.first > 6 ? ", ..." : "") + ". Check the BOM column mapping if this is a process or category value.");
    if (n == 0) E(ExportIssue::Warning, "", "No components are assigned to model point '" + mp.name + "'.");   // R18d: never an error
    // R21 (AUDIT_V20 P0/P1): marks are validated on the built job (strictly this side's marks); the exporter's
    // checker below then validates exactly the marks its Board / Circuit mode writes.
    MachineJob vjob; std::string vjob_err;
    const bool built = mp.has_machine && build_machine_job(prj, pi, vjob, vjob_err);
    const char* sname = ms.side == Side::Bottom ? "bottom" : "top";
    const int side_marks = built ? (int)vjob.fiducials.size() : fids;
    // R24a (user v23): a side without its own marks takes its linked layer's (pass-through, mirrored with this
    // side's transform). Zero marks is a Warning only when neither the side nor its linked layer has any.
    if (side_marks == 0) {
        const bool linked = linked_point_index(prj, pi) >= 0;
        if (ms.no_marks) E(ExportIssue::Info, "", std::string("No fiducial marks on the ") + sname + " side (intentional: 'No marks' is set for this layer).");
        else E(ExportIssue::Warning, "", std::string("No fiducial marks on the ") + sname + " side" +
               (linked ? " or on its linked layer" : " and no linked layer to take them from") + ": the program has no marks the camera can find. "
               "Add fiducials, or set 'No marks (intentional)' for this layer if it really runs without marks.");
    } else if (built && !vjob.marks_from.empty())
        E(ExportIssue::Info, "", std::string("No ") + sname + "-side fiducials: the " + std::to_string(side_marks) + " mark(s) of linked layer '" +
          vjob.marks_from + "' are used (pass-through), mirrored into " + sname + "-side coordinates.");
    if (other_side)   // R16 §12: top/bottom separation
        E(ExportIssue::Error, "", "Top/bottom separation: " + std::to_string(other_side) + " placement(s) on the " +
          (ms.side == Side::Bottom ? "top" : "bottom") + " side are assigned to this " + (ms.side == Side::Bottom ? "bottom" : "top") + "-side program.");
    if (!unresolved.empty()) {
        std::string l; int k = 0;
        for (auto& u : unresolved) { if (k++ == 8) { l += ", ..."; break; } l += (l.empty() ? "" : ", ") + u; }
        E(ExportIssue::Warning, "", std::to_string(unresolved.size()) + " unresolved package(s) without graphics: " + l + ".");
    }
    // R21: the exporter check runs whenever a job can be built (also with other errors), so the review lists
    // what the written program would contain -- nothing is blocked, everything is shown.
    if (mp.has_machine)
        for (auto& ck : job_checkers())
            if (ms.exporter_id.rfind(ck.first, 0) == 0) {
                if (built) ck.second(vjob, v);
                else if (!has_errors(v)) E(ExportIssue::Error, "", vjob_err);
            }
    std::stable_sort(v.begin(), v.end(), [](const ExportIssue& a, const ExportIssue& b) { return a.sev < b.sev; });
    return v;
}

} // namespace pcbp
