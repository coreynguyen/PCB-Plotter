// test_sides.cpp — manufacturing sides (Side 1/Side 2), side jobs, CSV/report, persistence.
#include "../src/core/sides.h"
#include "../src/core/project_io.h"
#include "../src/core/placement_check.h"
#include <cstdio>

using namespace pcbp;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static Project make() {
    Project p;
    p.info.number = "10001";
    Package r; r.name = "0603"; r.pads = {Pad{{-0.75, 0}, 0.8, 0.9}, Pad{{0.75, 0}, 0.8, 0.9}};
    p.packages["0603"] = r;
    PartNumber a; a.number = "CAP-100N"; a.description = "100nF 50V"; a.manf_pn = "GRM188R71H104KA93D"; a.manufacturer = "Murata";
    p.parts[a.number] = a;
    auto add = [&](const char* ref, double x, Side s, const char* pn) {
        Component c; c.ref = ref; c.pos = {x, 5}; c.package = "0603"; c.number = pn; c.side = s; c.assigned_point = 0;
        p.add_component(c);
    };
    add("C1", 1, Side::Top, "CAP-100N"); add("C2", 2, Side::Top, "CAP-100N");     // same P/N repeats: legal
    add("C3", 3, Side::Bottom, "CAP-100N"); add("R1", 4, Side::Bottom, "RES-1K");
    Component f; f.ref = "FID1"; f.feature = Feature::Fiducial; f.pos = {0, 0}; p.add_component(f);
    return p;
}

int main() {
    Project p = make();
    CHECK(is_two_sided(p));
    CHECK(!p.mfg.configured);
    auto v0 = validate_sides(p);
    CHECK(!v0.empty() && v0[0].blocking);          // order must be chosen by the user
    setup_sides(p, true, 0, true);
    CHECK(p.mfg.configured && p.mfg.sides.size() == 2);
    CHECK(p.mfg.sides[0].physical == Side::Bottom && p.mfg.sides[0].seq == 1);
    CHECK(p.mfg.sides[1].physical == Side::Top);
    CHECK(p.model.points.back().name == "Top SMT (Side 2)");
    CHECK(side_file_base(p, p.mfg.sides[0]) == "10001 [Side 1]");
    {   // R18d: Project Name, then Board ID, then the legacy Assembly No.
        Project q = p; q.info.pcb_id = "PWB-7"; CHECK(job_base_name(q) == "PWB-7");
        q.info.name = "Ctrl"; CHECK(job_base_name(q) == "Ctrl");
        Project e; CHECK(job_base_name(e) == "program");
    }
    CHECK(mfg_label(p.mfg.sides[1]) == "Side 2 (Top)");
    int e = 0; for (auto& i : validate_sides(p)) e += i.blocking;
    CHECK(e == 0);
    MachineJob j1, j2; std::string err;
    CHECK(build_side_job(p, p.mfg.sides[0], j1, err));
    CHECK(build_side_job(p, p.mfg.sides[1], j2, err));
    CHECK(j1.placements.size() == 2 && j1.bottom_side);
    CHECK(j2.placements.size() == 2 && !j2.bottom_side);
    CHECK(j1.placements[0].ref == "C3");
    // bottom mirrored: x negated about origin
    CHECK(j1.placements[0].x < 0);
    std::string csv = placements_csv(p, p.mfg.sides[0], j1);
    CHECK(csv.find("C3,CAP-100N,GRM188R71H104KA93D,Murata,100nF 50V,0603") != std::string::npos);
    CHECK(csv.find(",Bottom,[Side 1],") != std::string::npos);
    apply_desc_mode(p, j1, "desc_mpn");
    CHECK(j1.parts[0].description == "100nF 50V | GRM188R71H104KA93D");
    swap_side_order(p);
    CHECK(p.mfg.sides[0].physical == Side::Top && p.mfg.sides[0].seq == 1);
    std::vector<SideExportResult> rr(1); rr[0].side = p.mfg.sides[0]; rr[0].ok = true; rr[0].program_file = "10001 [Side 1].h7h";
    std::string h = sides_report_html(p, rr);
    CHECK(h.find("[Side 1]") != std::string::npos && h.find("Top") != std::string::npos);
    // persistence
    p.mfg.sides[1].suffix = "[B2]";
    p.mfg.last_validation = validation_summary(validate_sides(p));
    const std::string path = "build/tests/sides_rt.pcbp";
    CHECK(save_project(p, path, err));
    Project q;
    CHECK(load_project(path, q, err));
    // R18d migration: the sides block is gone; Side 1's layer (the clone, below) moves above Side 2's, both get link 1
    CHECK(!q.mfg.configured && q.mfg.sides.empty());
    CHECK(q.model.points[0].name == "Top SMT (Side 2)" && q.model.points[0].link == 1);
    CHECK(q.model.points[1].name == p.model.points[0].name && q.model.points[1].link == 1);
    CHECK(q.model.points.size() == p.model.points.size());
    {
        MfgSides m = link_sides(q, 1);
        CHECK(m.configured && m.sides.size() == 2 && m.sides[0].physical == Side::Top && m.sides[0].point == "Top SMT (Side 2)");
        CHECK(m.sides[1].physical == Side::Bottom && m.sides[1].seq == 2);
        int ca = 0, cb = 0;   // component assignments followed the move
        for (auto& c : q.comps) if (c.feature == Feature::Component) (c.side == Side::Top ? ca : cb) += c.assigned_point == (c.side == Side::Top ? 0 : 1);
        CHECK(ca == 2 && cb == 2);
        int be = 0; for (auto& i : validate_sides(q, m)) be += i.blocking;
        CHECK(be == 0);
        MachineJob jb; std::string e3;
        CHECK(build_side_job(q, m.sides[1], jb, e3) && jb.bottom_side && jb.placements.size() == 2);
    }
    CHECK(q.parts["CAP-100N"].manufacturer == "Murata");
    CHECK(q.model.points[0].machine.flipped == false && q.model.points[1].machine.flipped);
    // R18d link cycle: None -> 1 -> .. -> max(other)+1 -> None
    {
        Project c = make();
        while (c.model.points.size() < 3) c.model.points.push_back(c.model.points[0]);
        for (auto& m : c.model.points) m.link = 0;
        CHECK(cycle_link(c, 0) == 1 && cycle_link(c, 0) == 0);          // no other links: cap 1
        CHECK(cycle_link(c, 0) == 1 && cycle_link(c, 1) == 1 && cycle_link(c, 1) == 2 && cycle_link(c, 1) == 0);
        CHECK(cycle_link(c, 2) == 1 && cycle_link(c, 2) == 2 && cycle_link(c, 2) == 0);
        CHECK(link_members(c, 1).size() == 1 && link_sides(c, 1).sides.empty());   // one member: standalone
        CHECK(link_label(0) == "None" && link_label(3) == "3");
    }
    // R18d auto-link: exactly two SMT layers, top + bottom
    {
        Project a = make();
        a.model.points.clear();
        ModelPoint t; t.name = "Top SMT"; t.process_type = "SMT"; t.machine.side = Side::Top;
        ModelPoint b = t; b.name = "Bottom SMT"; b.machine.side = Side::Bottom; b.machine.flipped = true;
        ModelPoint h = t; h.name = "Hand Load"; h.process_type = "HAND";
        a.model.points = {t, b, h};
        CHECK(auto_link_smt_pair(a) && a.model.points[0].link == 1 && a.model.points[1].link == 1 && a.model.points[2].link == 0);
        Project one = a; one.model.points.erase(one.model.points.begin() + 1);
        for (auto& m : one.model.points) m.link = 0;
        CHECK(!auto_link_smt_pair(one));
    }
    // R18d migration: both sides on one layer -> no link, block cleared
    {
        Project m = make();
        m.mfg.configured = true;
        m.mfg.sides = {MfgSide{1, Side::Bottom, m.model.points[0].name, {}}, MfgSide{2, Side::Top, m.model.points[0].name, {}}};
        migrate_sides_to_links(m);
        CHECK(m.mfg.sides.empty() && max_link(m) == 0);
    }
    // single-sided board: one side, one step
    Project s = make();
    for (auto& c : s.comps) c.side = Side::Top;
    CHECK(!is_two_sided(s));
    CHECK(validate_sides(s).empty());
    setup_sides(s, true, 0, true);
    CHECK(s.mfg.sides.size() == 1);
    // refdes uniqueness per context: panel fiducial F1 vs fuse F1 is legal; two circuit C1 is not
    {
        Project u = make();
        Component g; g.ref = "F1"; g.feature = Feature::Fiducial; g.fid_scope = FiducialScope::Global; u.add_component(g);
        Component fu; fu.ref = "F1"; fu.package = "0603"; fu.number = "CAP-100N"; fu.pos = {9, 9}; u.add_component(fu);
        int dup = 0; for (auto& i : check_placements(u, PlacementCheckOptions{})) dup += i.code == CheckCode::DuplicateRef;
        CHECK(dup == 0);
        Component c1; c1.ref = "C1"; c1.package = "0603"; c1.pos = {20, 20}; c1.side = Side::Bottom; u.add_component(c1);
        dup = 0; for (auto& i : check_placements(u, PlacementCheckOptions{})) dup += i.code == CheckCode::DuplicateRef;
        CHECK(dup == 1);
    }
    std::printf("test_sides: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
