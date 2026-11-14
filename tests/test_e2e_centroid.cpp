// ============================================================================
// test_e2e_centroid.cpp — R17a (REVIEW_V16 §1, §2, §14): end-to-end centroid workflow on a synthetic
// 2-up panelised KiCad project with repeated references (tests/data/e2e, gen_e2e.py; no customer data).
// Real importers (KiCad, centroid CSV, BOM CSV, Gerber), the functions the Gerber tab calls for
// Align Plot to Gerber (io/align_pads.h + core/plot_align.h) and package acquisition
// (io/align_pads.h + core/package_resolve.h), outlines, output-layer assignment, H8H export + validator.
// ============================================================================
#include "../src/core/plot_align.h"
#include "../src/core/package_resolve.h"
#include "../src/core/machine_job.h"
#include "../src/core/fileio.h"
#include "../src/io/align_pads.h"
#include "../src/io/bom_import2.h"
#include "../src/io/centroid_import.h"
#include "../src/io/digitize.h"
#include "../src/io/gerber.h"
#include "../src/io/juki_writer.h"
#include "../src/io/kicad_pcb.h"
#include <cmath>
#include <cstdio>
#include <set>

using namespace pcbp;
namespace gb = pcbp::gerber;
namespace pj = pcbp::juki;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool nearv(Vec2 a, Vec2 b, double t = 1e-3) { return std::fabs(a.x - b.x) <= t && std::fabs(a.y - b.y) <= t; }
static bool neara(double a, double b, double t = 1e-3) { return std::fabs(std::remainder(a - b, 360.0)) <= t; }

static const std::string D = "tests/data/e2e/";
// the generator's misregistrations (plot -> Gerber)
static Xform G_of(const std::string& tag) {
    if (tag == "e2e") return Xform::translate(100, 50);
    if (tag == "e2e_rot90") return Xform::translate(150, 20) * Xform::rotation(90);
    return Xform::translate(-30, 10) * Xform::rotation(30) * Xform::scale(-1, 1);
}

static gb::Layer load_gerber(const std::string& name) {
    std::string t, err;
    gb::Layer L;
    CHECK(read_text_file(D + name, t, err));
    gb::ParseOptions po;
    CHECK(gb::parse(t, po, L, err));
    return L;
}
// the box the operator drags around a placement: its true Gerber position +- r
static std::vector<Vec2> box_at(Vec2 c, double r) { BBox b; b.add(c - Vec2{r, r}); b.add(c + Vec2{r, r}); return io::box_quad(b); }
static std::vector<int> comps_ref(const Project& p, const char* ref) {
    std::vector<int> v;
    for (int i = 0; i < (int)p.comps.size(); ++i) if (p.comps[i].ref == ref) v.push_back(i);
    return v;
}
static bool pad_in(const std::vector<Vec2>& set, Vec2 q, double tol) { for (auto& s : set) if (nearv(s, q, tol)) return true; return false; }

// ---- 1. KiCad board (truth for the package graphics) ---------------------------------------------
static Project kicad_truth() {
    Project k;
    io::CadImportOptions o; io::CadImportResult r; std::string err;
    CHECK(io::import_kicad_pcb_file(D + "e2e.kicad_pcb", k, o, r, err));
    CHECK(k.comps.size() == 14);
    CHECK(comps_ref(k, "U1").size() == 2 && comps_ref(k, "R3").size() == 2);
    const auto u1 = comps_ref(k, "U1");
    if (u1.size() == 2) CHECK(nearv(k.comps[u1[0]].pos, {17, 20}) && nearv(k.comps[u1[1]].pos, {67, 20}));
    for (const char* pk : {"SOT-23", "SOIC-8", "R_0603", "C_0805"}) CHECK(k.packages.count(pk) && !k.packages[pk].pads.empty());
    if (!comps_ref(k, "R3").empty()) CHECK(k.comps[comps_ref(k, "R3")[0]].side == Side::Bottom);
    return k;
}

// ---- 2. centroid + BOM ---------------------------------------------------------------------------
static Project centroid_bom() {
    Project p;
    io::CentroidOptions co; io::CentroidReport cr; std::string err;
    co.mode = io::CentroidMode::Replace;
    CHECK(io::import_centroid_file(D + "e2e-pos.csv", p, co, cr, err));
    CHECK(cr.map.ref >= 0 && cr.map.x >= 0 && cr.map.y >= 0 && cr.map.rot >= 0 && cr.map.side >= 0 && cr.map.package >= 0);
    CHECK(cr.table.col_name(cr.map.x) == "PosX" && cr.table.col_name(cr.map.package) == "Package");
    CHECK(cr.imported == 14 && p.comps.size() == 14);
    auto u1 = comps_ref(p, "U1");
    CHECK(u1.size() == 2);
    if (u1.size() == 2) CHECK(nearv(p.comps[u1[0]].pos, {17, 20}) && nearv(p.comps[u1[1]].pos, {67, 20}) && neara(p.comps[u1[1]].rot, 90));
    // re-import (Merge) must keep the repeated references apart (R16 collapsed them onto one placement)
    Project q = p;
    io::CentroidOptions cm; cm.mode = io::CentroidMode::Merge; io::CentroidReport rm;
    CHECK(io::import_centroid_file(D + "e2e-pos.csv", q, cm, rm, err));
    CHECK(q.comps.size() == 14 && rm.updated == 14 && rm.added == 0);
    u1 = comps_ref(q, "U1");
    if (u1.size() == 2) CHECK(nearv(q.comps[u1[0]].pos, {17, 20}) && nearv(q.comps[u1[1]].pos, {67, 20}));
    // BOM: references listed once apply to every circuit
    io::BomOptions2 bo; io::BomReport br;
    CHECK(io::import_bom2_file(D + "e2e-bom.csv", p, bo, br, nullptr, err));
    for (auto& c : p.comps) {
        if (c.feature != Feature::Component) continue;
        CHECK(c.bom_state == BomState::Listed);
        if (c.ref == "U1") CHECK(c.number == "BC817-40");
        if (c.ref == "R2") CHECK(c.number == "RC0603-10K" && c.package == "R_0603");
        if (c.ref == "R3") CHECK(c.number == "RC0603-4K7" && c.side == Side::Bottom);
    }
    CHECK(br.comps_not_in_bom.empty() && br.refs_not_on_board.empty());
    CHECK(p.parts.count("LM358DR"));
    for (int ci : comps_ref(p, "U2")) CHECK(p.comps[ci].bom_package == "SOIC-8" && p.comps[ci].number == "LM358DR");   // package text kept per placement
    return p;
}

// ---- 3. packages: entries for the editing controls, auto-resolve, report -------------------------
static void packages(Project& p, const Project& k) {
    // every package the placements name is listed (component dropdowns are built from Project::packages)
    CHECK(ensure_package_entries(p) == 4);
    for (const char* n : {"SOT-23", "SOIC-8", "R_0603", "C_0805"}) CHECK(p.packages.count(n) == 1);
    PkgResolveReport r0 = package_report(p);
    CHECK(r0.entries.size() == 4 && r0.resolved == 0 && r0.unresolved == 4);
    PkgResolveReport r = auto_resolve_packages(p, nullptr, true);
    CHECK(r.resolved == 3 && r.unresolved == 1 && r.changed == 3);
    for (auto& e : r.entries) {
        if (e.name == "SOIC-8") CHECK(!e.resolved && e.placements == 2);
        if (e.name == "R_0603") CHECK(e.resolved && e.how == "template:chip" && e.placements == 6);
        if (e.name == "SOT-23") CHECK(e.resolved && e.how == "template:sot23");
    }
    CHECK(r.summary().find("unresolved: SOIC-8 (2)") != std::string::npos);
    // templates equal the KiCad land patterns (same centre and orientation, pin 1 first)
    for (const char* n : {"SOT-23", "R_0603", "C_0805"}) {
        const Package& a = p.packages[n];
        const Package& b = k.packages.at(n);
        CHECK(a.pads.size() == b.pads.size());
        for (size_t i = 0; i < a.pads.size() && i < b.pads.size(); ++i)
            CHECK(nearv(a.pads[i].pos, b.pads[i].pos, 1e-6) && std::fabs(a.pads[i].w - b.pads[i].w) < 1e-6 && std::fabs(a.pads[i].h - b.pads[i].h) < 1e-6);
    }
    // a second run changes nothing
    CHECK(auto_resolve_packages(p, nullptr, true).changed == 0);
    // library (equivalent name) wins over a template
    Project q; Component c; c.ref = "R9"; c.package = "RESC1608X55N"; q.add_component(c);
    std::map<std::string, Package> lib; lib["R0603"] = k.packages.at("R_0603"); lib["R0603"].pads[0].w = 0.77;
    PkgResolveReport rq = auto_resolve_packages(q, &lib, true);
    CHECK(rq.resolved == 1 && rq.entries[0].how == "library" && std::fabs(q.packages["RESC1608X55N"].pads[0].w - 0.77) < 1e-9);
}

// ---- 4. alignment (the Gerber tab's Align Plot to Gerber) ---------------------------------------
// Old R16 path, kept as the root-cause regression: a centroid-only reference went through the pad-to-pad
// nearest-neighbour fit as ONE point and was pulled onto the single nearest Gerber pad.
static void root_cause(const Project& p) {
    const gb::Layer L = load_gerber("e2e-F_Paste.gbr");
    const Xform G = G_of("e2e");
    const int u1 = comps_ref(p, "U1")[0];
    const Vec2 truth = G.apply(p.comps[u1].pos);
    const auto pads = io::gerber_pad_centres(L, Xform{}, box_at(truth, 2.2));
    CHECK(pads.size() == 3);
    AlignFit old;
    CHECK(fit_cad_to_gerber({p.comps[u1].pos}, pads, 0, false, false, old));
    CHECK(old.matched == 1 && dist(old.T.apply(p.comps[u1].pos), truth) > 0.5);   // anchored to one pad (the bug)
    // the mean of 3 SOT-23 pads is not the footprint centre either; the bounding-box centre is
    Vec2 mean; for (auto& q : pads) mean = mean + q; mean = mean * (1.0 / 3);
    CHECK(dist(mean, truth) > 0.3 && nearv(pad_selection_centre(pads), truth, 1e-6));
}

// every placement instance (top on F_Paste, bottom on B_Paste) has all its package pads on Gerber pads
static int check_on_gerber(const Project& p, const std::vector<Vec2>& top, const std::vector<Vec2>& bot, const Xform& plot_to_ger, double tol) {
    int bad = 0, n = 0;
    for (auto& c : p.comps) {
        if (c.feature != Feature::Component) continue;
        const Package* pk = p.package_of(c);
        if (!pk || pk->pads.empty()) continue;
        // the source side (bottom pads are on B_Paste whatever the mirror did to the side flag)
        const bool src_bottom = c.ref == "R3";
        for (int i = 0; i < std::max(1, p.instance_count(c)); ++i)
            for (auto& d : pk->pads) {
                ++n;
                if (!pad_in(src_bottom ? bot : top, plot_to_ger.apply(p.instance_xform(c, i).apply(d.pos)), tol)) ++bad;
            }
    }
    return n ? bad : -1;
}

static void alignment(const Project& p0) {
    for (std::string tag : {"e2e", "e2e_rot90", "e2e_mir"}) {
        const gb::Layer F = load_gerber(tag + "-F_Paste.gbr"), B = load_gerber(tag + "-B_Paste.gbr");
        const Xform G = G_of(tag);
        BBox all; all.add({-1e4, -1e4}); all.add({1e4, 1e4});
        const auto top = io::gerber_pad_centres(F, Xform{}, io::box_quad(all)), bot = io::gerber_pad_centres(B, Xform{}, io::box_quad(all));
        CHECK(top.size() == 2 * (3 + 8 + 2 + 2 + 2) && bot.size() == 4);
        const int rq = tag == "e2e" ? 0 : tag == "e2e_rot90" ? 1 : -1;   // the quarter turn the user would pick
        const bool mir = tag == "e2e_mir";
        Project p = p0;
        // (a) one centroid-only reference: U1 of circuit 2 (repeated reference: the one picked, not the first)
        const int u1b = comps_ref(p, "U1")[1];
        const Vec2 w = G.apply(p.comps[u1b].pos);
        CHECK(nearest_instance(p, p.comps[u1b], w) == 0);
        std::vector<AlignRef> refs{make_align_ref(p, u1b, 0, io::gerber_pad_centres(F, Xform{}, box_at(w, 2.2)))};
        AlignFit f; std::string why;
        if (rq >= 0) {
            // centroid only: SOT-23 template removed for this check
            Project c = p; c.packages["SOT-23"].pads.clear();
            std::vector<AlignRef> r1{make_align_ref(c, u1b, 0, refs[0].ger_pads)};
            CHECK(r1[0].cad_pads.empty() && r1[0].ger_pads.size() == 3);
            CHECK(solve_alignment(r1, rq, false, f, &why));
            CHECK(nearv(f.T.apply(c.comps[u1b].pos), w, 1e-6) && r1[0].residual < 1e-6);
            for (Vec2 q : {Vec2{0, 0}, Vec2{80, 33}}) CHECK(nearv(f.T.apply(q), G.apply(q), 1e-6));
            // wrong quarter turn: the preview shows it (ghosts off the pads), nothing is applied
            AlignFit wrong;
            CHECK(solve_alignment(r1, (rq + 1) % 4, false, wrong, &why));
            CHECK(check_on_gerber(c, top, bot, wrong.T, 0.05) > 0);
        }
        // (b) with package pads (templates resolved): the pads give the rotation ("From references")
        if (!mir) {
            CHECK(refs[0].cad_pads.size() == 3);
            CHECK(solve_alignment(refs, 4, false, f, &why));
            CHECK(neara(f.rot, tag == "e2e" ? 0 : 90, 1e-6) && f.rms < 1e-6);
            CHECK(nearv(f.T.apply({0, 0}), G.apply({0, 0}), 1e-5) && nearv(f.T.apply({100, 40}), G.apply({100, 40}), 1e-5));
        }
        // (c) two / three centroid references, rotation from the references (arbitrary angle + mirror)
        {
            const int r1a = comps_ref(p, "R1")[0], u2b = comps_ref(p, "U2")[1], c1a = comps_ref(p, "R2")[0];
            std::vector<AlignRef> r2;
            for (int ci : {u1b, r1a}) r2.push_back(make_align_ref(p, ci, 0, io::gerber_pad_centres(F, Xform{}, box_at(G.apply(p.comps[ci].pos), 1.6))));
            CHECK(solve_alignment(r2, 4, mir, f, &why));
            for (Vec2 q : {Vec2{0, 0}, Vec2{100, 40}}) CHECK(nearv(f.T.apply(q), G.apply(q), 1e-5));
            CHECK(neara(f.rot, mir ? 30 : rq * 90.0, 1e-6) && f.mirror == mir);
            if (!neara(f.rot, mir ? 30 : rq * 90.0, 1e-6)) std::printf("  %s rot %.6f rms %.5f pads %zu %zu\n", tag.c_str(), f.rot, f.rms, r2[0].ger_pads.size(), r2[1].ger_pads.size());
            // third reference with the wrong mirror state: residual + hint
            r2.push_back(make_align_ref(p, u2b, 0, io::gerber_pad_centres(F, Xform{}, box_at(G.apply(p.comps[u2b].pos), 3.6))));
            r2.push_back(make_align_ref(p, c1a, 0, io::gerber_pad_centres(F, Xform{}, box_at(G.apply(p.comps[c1a].pos), 1.6))));
            AlignFit g;
            CHECK(solve_alignment(r2, 4, !mir, g, &why));
            if (!(g.rms > 1 && why.find("Mirror") != std::string::npos)) std::printf("  %s wrong-mirror rms %.4f why '%s' pads %zu %zu %zu %zu\n", tag.c_str(), g.rms, why.c_str(), r2[0].ger_pads.size(), r2[1].ger_pads.size(), r2[2].ger_pads.size(), r2[3].ger_pads.size());
            CHECK(g.rms > 1 && why.find("Mirror") != std::string::npos);
            CHECK(solve_alignment(r2, 4, mir, g, &why) && g.rms < 1e-5);
        }
        // preview: every placement instance of the whole set lands on its Gerber pads (both circuits, bottom side)
        CHECK(check_on_gerber(p, top, bot, f.T, 0.002) == 0);
        const auto ghosts = preview_placements(p, f.T);
        CHECK(ghosts.size() == p.comps.size());
        // Apply, direction "Move plot to Gerbers": same result in the project
        Project a = p;
        move_plot(a, f.T);
        CHECK(check_on_gerber(a, top, bot, Xform{}, 0.002) == 0);
        if (mir) { CHECK(a.comps[comps_ref(a, "U1")[0]].side == Side::Bottom); }
        // Apply, direction "Move Gerbers to plot": layers get T^-1, Gerber world == plot
        Project b = p;
        GerberLayerRef lr; lr.name = "F_Paste"; b.layers.push_back(lr);
        premultiply_layer(b.layers[0], f.T.inverse());
        const Xform lx = layer_ref_xform(b.layers[0]);
        const auto top2 = io::gerber_pad_centres(F, lx, io::box_quad(all)), bot2 = io::gerber_pad_centres(B, lx, io::box_quad(all));
        CHECK(check_on_gerber(b, top2, bot2, Xform{}, 0.002) == 0);
        // repeat: a second alignment on the aligned data is the identity
        std::vector<AlignRef> again{make_align_ref(a, u1b, 0, io::gerber_pad_centres(F, Xform{}, box_at(w, 2.2)))};
        AlignFit id;
        if (!solve_alignment(again, 4, false, id, &why) || !nearv(id.T.apply({0, 0}), {0, 0}, 1e-4)) std::printf("  %s again: rot %.5f T0 %.5f,%.5f rms %.5f matched %d cad %zu ger %zu why %s\n", tag.c_str(), id.rot, id.T.tx, id.T.ty, id.rms, id.matched, again[0].cad_pads.size(), again[0].ger_pads.size(), why.c_str());
        CHECK(solve_alignment(again, 4, false, id, &why) && nearv(id.T.apply({0, 0}), {0, 0}, 1e-4) && nearv(id.T.apply({50, 30}), {50, 30}, 1e-4));
    }
    // panel instances: a folded panel (images); the reference picked on circuit 2 is that instance
    {
        Project p = p0;
        InstanceFold fo;
        CHECK(detect_repeated_instances(p, fo));
        fold_repeated_instances(p, fo);
        const gb::Layer F = load_gerber("e2e_rot90-F_Paste.gbr");
        const Xform G = G_of("e2e_rot90");
        const int u1 = comps_ref(p, "U1")[0];
        const Vec2 w = G.apply(p.instance_xform(p.comps[u1], 1).apply({0, 0}));   // circuit 2's U1 on the Gerber
        const int inst = nearest_instance(p, p.comps[u1], G.inverse().apply(w));
        CHECK(inst == 1);
        std::vector<AlignRef> refs{make_align_ref(p, u1, inst, io::gerber_pad_centres(F, Xform{}, box_at(w, 2.2)))};
        AlignFit f;
        CHECK(solve_alignment(refs, 1, false, f) && nearv(f.T.apply({3, 4}), G.apply({3, 4}), 1e-6));
        move_plot(p, f.T);
        CHECK(nearv(p.instance_xform(p.comps[u1], 1).apply({0, 0}), w, 1e-6));
        CHECK(nearv(p.instance_xform(p.comps[u1], 0).apply({0, 0}), G.apply({17, 20}), 1e-6));
        CHECK(p.images.size() == 2 && neara(p.images[1].rot, 0, 1e-6));   // rigid: the instance stays unrotated relative to circuit 1
    }
    // global Gerber rotate / mirror before aligning: composes with the layer transform
    {
        Project p; GerberLayerRef lr; lr.offset = {3, 4}; p.layers.push_back(lr);
        transform_gerbers(p, 90, true, {10, 10});
        const Xform want = Xform::translate(10, 10) * Xform::rotation(90) * Xform::scale(-1, 1) * Xform::translate(-10, -10) * Xform::translate(3, 4);
        CHECK(nearv(layer_ref_xform(p.layers[0]).apply({7, 2}), want.apply({7, 2}), 1e-9) && p.layers[0].mirror);
    }
}

// ---- 5. package from Gerber (the wizard's Build & assign) ---------------------------------------
static void build_from_gerber(Project& p, const Project& k) {
    // aligned project: plot moved onto the e2e Gerbers (offset)
    const Xform G = G_of("e2e");
    move_plot(p, G);
    const gb::Layer F = load_gerber("e2e-F_Paste.gbr"), B = load_gerber("e2e-B_Paste.gbr");
    digitize::DigitizeOptions dop;
    const digitize::PadSet psF = digitize::extract_pads(F, dop), psB = digitize::extract_pads(B, dop);
    // SOIC-8 from circuit 2's U2 (a repeated reference): pads boxed around the yellow cross
    const int u2b = comps_ref(p, "U2")[1];
    const Component c = p.comps[u2b];
    auto pads = io::padset_board_pads(psF, Xform{}, box_at(c.pos, 3.8));
    CHECK(pads.size() == 8);
    Package pk = package_from_board_pads(pads, c);
    const Package& t = k.packages.at("SOIC-8");
    CHECK(pk.pads.size() == t.pads.size());
    for (size_t i = 0; i < pk.pads.size() && i < t.pads.size(); ++i)
        CHECK(pk.pads[i].pin == (int)i + 1 && nearv(pk.pads[i].pos, t.pads[i].pos, 1e-3) && std::fabs(pk.pads[i].w - t.pads[i].w) < 1e-3 && std::fabs(pk.pads[i].h - t.pads[i].h) < 1e-3);
    CHECK(nearv(package_pad_centre(pk), {0, 0}, 1e-3));
    const int n = assign_package_geometry(p, "SOIC-8", pk, "gerber");
    CHECK(n == 2 && placements_using(p, "SOIC-8") == 2);
    for (int ci : comps_ref(p, "U2")) CHECK(placement_package_resolved(p, p.comps[ci]));
    CHECK(unresolved_packages(p).empty() && package_report(p).unresolved == 0);
    // rotated placement (U1, 90 deg) and a bottom placement (R3, 90 deg, mirrored) rebuild their land pattern
    {
        const Component u1 = p.comps[comps_ref(p, "U1")[0]];
        Package s = package_from_board_pads(io::padset_board_pads(psF, Xform{}, box_at(u1.pos, 2.2)), u1);
        const Package& st = k.packages.at("SOT-23");
        CHECK(s.pads.size() == 3);
        for (size_t i = 0; i < s.pads.size() && i < 3; ++i) CHECK(nearv(s.pads[i].pos, st.pads[i].pos, 1e-3) && std::fabs(s.pads[i].w - st.pads[i].w) < 1e-3);
        const Component r3 = p.comps[comps_ref(p, "R3")[1]];
        Package r = package_from_board_pads(io::padset_board_pads(psB, Xform{}, box_at(r3.pos, 1.5)), r3);
        CHECK(r.pads.size() == 2);
        for (auto& d : r.pads) CHECK(std::fabs(std::fabs(d.pos.x) - 0.8) < 1e-3 && std::fabs(d.pos.y) < 1e-3 && std::fabs(d.w - 0.9) < 1e-3 && std::fabs(d.h - 0.95) < 1e-3);
    }
    CHECK(check_on_gerber(p, io::gerber_pad_centres(F, Xform{}, box_at({150, 70}, 200)), io::gerber_pad_centres(B, Xform{}, box_at({150, 70}, 200)), Xform{}, 0.002) == 0);
}

// ---- 6. panel instances, outlines, assignment, H8H -----------------------------------------------
static void export_h8h(Project& p) {
    // fold the flattened panel into 2 circuits; references stay associated with their own circuit
    const Vec2 u1b = p.comps[comps_ref(p, "U1")[1]].pos;
    InstanceFold fo;
    CHECK(detect_repeated_instances(p, fo) && fo.n == 2 && fo.max_dev < 1e-6);
    CHECK(fold_repeated_instances(p, fo) == 6);
    CHECK(p.comps.size() == 8 && p.images.size() == 2 && nearv(p.images[1].offset, {50, 0}, 1e-6));
    CHECK(nearv(p.instance_xform(*p.find_ref("U1"), 1).apply({0, 0}), u1b, 1e-6));
    CHECK(!p.instanced(*p.find_ref("FID1")));
    // outlines from the Edge_Cuts Gerber (aligned: Gerber world == plot): board = outer frame, circuit = circuit 1
    const gb::Layer E = load_gerber("e2e-Edge_Cuts.gbr");
    BBox ext = E.extent; ext.inflate(-0.05);   // 0.1 mm pen
    CHECK(nearv({ext.x0, ext.y0}, {100, 50}, 1e-3) && nearv({ext.x1, ext.y1}, {200, 90}, 1e-3));
    p.set_panel_frame_rect(ext);
    p.set_circuit_frame_rect(BBox(105, 55, 145, 85));
    p.panel_outline_src = OutlineSource::Imported; p.board_outline_src = OutlineSource::Imported;
    CHECK(check_outlines(p).empty());
    // output layer: Top SMT (JUKI H8H); bottom part unassigned
    p.model.points.clear();
    ModelPoint mp; mp.name = "Top SMT"; mp.has_machine = true; mp.machine.exporter_id = "juki.h8h"; mp.machine.side = Side::Top;
    p.model.points.push_back(mp);
    for (auto& c : p.comps) c.assigned_point = (c.feature == Feature::Component && c.side == Side::Top) ? 0 : -1;
    p.find_ref("R2")->assigned_point = -1;   // unassign one ...
    p.find_ref("R2")->assigned_point = 0;    // ... and assign it again
    for (auto& c : p.comps) if (c.feature == Feature::Fiducial) { c.assigned_point = 0; }
    auto v = validate_for_export(p, 0);
    for (auto& i : v) if (i.sev == ExportIssue::Error) std::printf("  export error: %s %s\n", i.ref.c_str(), i.msg.c_str());
    CHECK(!has_errors(v));
    MachineJob j; std::string err;
    CHECK(build_machine_job(p, 0, j, err));
    CHECK(j.circuits.size() == 2);
    int nU1 = 0;
    for (auto& pl : j.placements) if (pl.ref == "U1") ++nU1;
    CHECK(nU1 == 2);
    // circuit 2's U1 = circuit 1's U1 + pitch in machine coordinates
    const JobPlacement* a = nullptr; const JobPlacement* b = nullptr;
    for (auto& pl : j.placements) if (pl.ref == "U1") (pl.circuit == 1 ? a : b) = &pl;
    CHECK(a && b);
    if (a && b) CHECK(std::fabs(std::hypot(b->x - a->x, b->y - a->y) - 50) < 1e-6 && neara(a->rot, b->rot));
    j.options["juki.timestamp"] = "1700000000";
    std::vector<uint8_t> bytes;
    pj::HlcWriteInfo wi;
    CHECK(pj::write_hlc_variant(j, pj::HlcVariant::H8H, bytes, err, &wi));
    const auto chk = pj::validate_hlc(bytes);
    for (auto& c : chk) if (c.error) std::printf("  validator: %s\n", c.msg.c_str());
    CHECK(!bytes.empty() && pj::hlc_check_ok(chk));
    pj::HlcParsed hp;
    CHECK(pj::parse_hlc(bytes, hp, err));
    int top_parts = 0;
    for (auto& c : p.comps) if (c.feature == Feature::Component && c.side == Side::Top) ++top_parts;
    // circuit 1 records + a 2 x 1 matrix at 50 mm pitch (the repeated circuit)
    CHECK((int)hp.placements.size() == top_parts && wi.mode == pj::CircuitMode::Matrix && hp.div_x == 2 && hp.div_y == 1 && hp.pitch_x == 500000);
    std::set<std::string> refs;
    for (auto& r : hp.placements) refs.insert(r.ref);
    CHECK(refs.count("U1") && refs.count("U2") && refs.count("C1") && !refs.count("R3"));
}

int main() {
    const Project k = kicad_truth();
    Project p = centroid_bom();
    packages(p, k);
    root_cause(p);
    alignment(p);
    build_from_gerber(p, k);
    export_h8h(p);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
