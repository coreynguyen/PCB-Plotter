// ============================================================================
// test_placement_check.cpp — tests for core/placement_check.
// ============================================================================
#include "../src/core/placement_check.h"
#include <cstdio>

using namespace pcbp;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int count(const std::vector<PlacementIssue>& v, CheckCode c) {
    int n = 0;
    for (auto& i : v) n += i.code == c;
    return n;
}
static bool has_ref(const Project& p, const std::vector<PlacementIssue>& v, CheckCode c, const std::string& ref) {
    for (auto& i : v)
        if (i.code == c)
            for (int k : i.comps) if (p.comps[k].ref == ref) return true;
    return false;
}

static Component comp(const std::string& ref, double x, double y, const std::string& pkg, const std::string& pn = "P1",
                      double rot = 0, Side s = Side::Top) {
    Component c;
    c.ref = ref; c.pos = {x, y}; c.package = pkg; c.number = pn; c.rot = rot; c.side = s;
    return c;
}

int main() {
    // R13 package-name equivalence (specs/BOM_PACKAGES.md)
    CHECK(packages_equivalent("0402", "C0402"));
    CHECK(packages_equivalent("CAP_0402", "CAPC1005X55N"));
    CHECK(packages_equivalent("R0603", "RESC1608X55N"));
    CHECK(packages_equivalent("1608M", "0603"));
    CHECK(packages_equivalent("SOT-23", "sot23"));
    CHECK(!packages_equivalent("0402", "0603"));
    CHECK(!packages_equivalent("CAPC1005X55N", "1206"));
    CHECK(!packages_equivalent("SOIC-8", "SOIC-14"));
    Project p;
    p.comps.clear();
    Package r;
    r.name = "0603";
    r.pads = {Pad{{-0.75, 0}, 0.8, 0.9}, Pad{{0.75, 0}, 0.8, 0.9}};
    p.packages["0603"] = r;
    Package e;
    e.name = "EMPTY";
    p.packages["EMPTY"] = e;
    PartNumber pn;
    pn.number = "P1"; pn.package = "0603";
    p.parts["P1"] = pn;
    pn.number = "P2"; pn.package = "0805";
    p.parts["P2"] = pn;
    p.board_frame = {{{0, 0}, {100, 0}, {100, 80}, {0, 80}, {0, 0}}};

    p.add_component(comp("R1", 10, 10, "0603"));
    p.add_component(comp("R2", 10, 10, "0603"));          // coincident with R1
    p.add_component(comp("R3", 10.1, 10, "0603"));        // near + overlap
    p.add_component(comp("r1", 50, 50, "0603"));          // duplicate (case)
    p.add_component(comp("U1", 10, 10, "0603", "P1", 0, Side::Bottom));   // other side: fine
    p.add_component(comp("C1", 200, 200, "0603"));        // outside
    p.add_component(comp("C2", 99.5, 40, "0603"));        // partly outside
    p.add_component(comp("R 9", 30, 30, "0603"));         // illegal char
    p.add_component(comp("R10", 30, 40, ""));             // no package
    p.add_component(comp("R11", 30, 45, "XYZ"));          // not in library
    p.add_component(comp("R12", 30, 50, "EMPTY"));        // zero pads
    p.add_component(comp("R13", 30, 55, "0603", "P2"));   // PN package mismatch
    p.add_component(comp("R14", 30, 60, "0603", ""));     // missing PN
    p.add_component(comp("R15", 30, 65, "0603", "P9"));   // PN not in library
    p.add_component(comp("R16", 40, 60, "0603", "P1", 45));       // off-axis rotation
    p.add_component(comp("R17", 40, 65, "0603", "P1", 89.6));     // snap candidate
    Component d = comp("R18", 40, 70, "0603");
    d.dnp = true; d.assigned_point = 0;
    p.add_component(d);
    Component z = comp("R19", 60, 70, "0603", "P1", 90);
    p.add_component(z);
    Component w = comp("R20", 60, 70.9, "0603", "P1", 0);   // rotated R19 body overlaps
    p.add_component(w);
    for (double x : {5.0, 50.0, 95.0}) {
        Component f;
        f.ref = "FID" + std::to_string((int)x);
        f.pos = {x, 5};
        f.feature = Feature::Fiducial;
        p.add_component(f);
    }

    auto v = check_placements(p);
    std::printf("%s", describe_issues(v).c_str());
    CHECK(count(v, CheckCode::DuplicateRef) == 1 && has_ref(p, v, CheckCode::DuplicateRef, "r1"));
    CHECK(count(v, CheckCode::Coincident) == 1 && has_ref(p, v, CheckCode::Coincident, "R2"));
    CHECK(has_ref(p, v, CheckCode::NearCoincident, "R3"));
    CHECK(has_ref(p, v, CheckCode::Overlap, "R3"));
    CHECK(!has_ref(p, v, CheckCode::Coincident, "U1") && !has_ref(p, v, CheckCode::Overlap, "U1"));
    CHECK(has_ref(p, v, CheckCode::Overlap, "R19") && has_ref(p, v, CheckCode::Overlap, "R20"));
    CHECK(has_ref(p, v, CheckCode::OutsideBoard, "C1"));
    CHECK(has_ref(p, v, CheckCode::PartlyOutsideBoard, "C2"));
    CHECK(has_ref(p, v, CheckCode::IllegalRefChars, "R 9"));
    CHECK(has_ref(p, v, CheckCode::MissingPackage, "R10"));
    CHECK(has_ref(p, v, CheckCode::PackageNotInLibrary, "R11"));
    CHECK(has_ref(p, v, CheckCode::ZeroPadPackage, "R12"));
    CHECK(has_ref(p, v, CheckCode::PartPackageMismatch, "R13"));
    CHECK(has_ref(p, v, CheckCode::MissingPartNumber, "R14"));
    CHECK(has_ref(p, v, CheckCode::PartNotInLibrary, "R15"));
    CHECK(has_ref(p, v, CheckCode::OffAxisRotation, "R16") && has_ref(p, v, CheckCode::OffAxisRotation, "R17"));
    CHECK(has_ref(p, v, CheckCode::DnpAssigned, "R18"));
    CHECK(count(v, CheckCode::CollinearFiducials) == 1);
    CHECK(count(v, CheckCode::FewFiducials) == 1);         // bottom side has U1 and no fiducials
    CHECK(has_ref(p, v, CheckCode::Unassigned, "R1"));
    CHECK(!has_ref(p, v, CheckCode::Overlap, "R16"));
    CHECK(v.front().severity == Severity::Error);

    // fixes
    CHECK(fix_duplicate_refs(p) == 1 && p.find_ref("r1_2"));
    CHECK(snap_rotations(p, 90, 1.0) >= 1 && p.find_ref("R17")->rot == 90);
    CHECK(sanitize_refs(p) == 1 && p.find_ref("R_9"));
    CHECK(unassign_dnp(p) == 1);
    auto v2 = check_placements(p);
    CHECK(count(v2, CheckCode::DuplicateRef) == 0 && count(v2, CheckCode::IllegalRefChars) == 0 && count(v2, CheckCode::DnpAssigned) == 0);
    CHECK(!has_ref(p, v2, CheckCode::OffAxisRotation, "R17"));

    // non-collinear fiducials, enough
    p.comps.back().pos = {95, 75};
    auto v3 = check_placements(p);
    CHECK(count(v3, CheckCode::CollinearFiducials) == 0);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
