// ============================================================================
// test_importers.cpp — tests for pcbp::io text_table / centroid / BOM /
// GenCAD / IPC-D-356 importers on CircuitCAM demo data + synthetic strings.
// Build: g++ -std=c++20 -Wall -Isrc tests/test_importers.cpp src/io/{io_util,text_table,
//        centroid_import,bom_import,gencad,ipc356}.cpp src/core/project.cpp
// ============================================================================
#include "local_data.h"
#include "../src/io/bom_import.h"
#include "../src/io/centroid_import.h"
#include "../src/io/gencad.h"
#include "../src/io/io_util.h"
#include "../src/io/ipc356.h"
#include "../src/io/text_table.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>

using namespace pcbp;
using namespace pcbp::io;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const std::string kRoot = pcbp_test::local("legacy_demo_root");   // legacy CAM demo install (local only)
static const std::string kBom = kRoot + "/Demonstration Bill of Materials/DemoBOM.prn";
static const std::string kRef = kRoot + "/Centroid File/Reference File.txt";
static const std::string kCad = kRoot + "/Demonstration CAD Data/CCDemo.cad";

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// ---------------------------------------------------------------------------
static void test_text_table() {
    std::printf("== text_table\n");
    std::string err;
    TextTable t;
    CHECK(read_text_table_file(kBom, {}, t, err));
    std::printf("DemoBOM.prn: %s\n", t.describe().c_str());
    CHECK(t.delim == Delim::Fixed);
    CHECK(t.ncols() == 9);
    CHECK(t.rows.size() == 25);
    CHECK(t.has_header && t.header_from_comment);
    CHECK(t.col("Reference") == 0 && t.col("Part Number") == 1 && t.col("Polarity") == 8);
    CHECK(t.col("Sofware Rev") == 6 && t.col("Vendor Part Number") == 7);
    bool found_c = false, found_u = false;
    for (auto& r : t.rows) {
        if (r[0] == "C1-C25, C27-C34") { found_c = true; CHECK(r[1] == "8555-901-5236"); CHECK(r[2] == "0805"); }
        if (r[0] == "U11, U3, U14") { found_u = true; CHECK(r[2] == "DIP24"); CHECK(r[8] == "TRUE"); }
        if (r[0] == "P1") CHECK(r[1] == "45263");
        if (r[0] == "U5") CHECK(r[6] == "Vers. 52.1.02");
    }
    CHECK(found_c && found_u);

    TextTable rf;
    CHECK(read_text_table_file(kRef, {}, rf, err));
    std::printf("Reference File.txt: %s\n", rf.describe().c_str());
    CHECK(rf.ncols() == 4 && rf.rows.size() == 98 && !rf.has_header);

    // Altium-style CSV with preamble + quoted header
    const std::string altium =
        "Altium Designer Pick and Place Locations\r\n"
        "C:\\proj\\board.PcbDoc\r\n\r\n"
        "========================================================\r\n"
        "Units used:   mm\r\n\r\n"
        "\"Designator\",\"Comment\",\"Layer\",\"Footprint\",\"Center-X(mm)\",\"Center-Y(mm)\",\"Rotation\",\"Description\"\r\n"
        "\"C1\",\"100nF\",\"TopLayer\",\"C0402\",\"10.160\",\"20.320\",\"90\",\"Cap, ceramic\"\r\n"
        "\"R7\",\"10k\",\"BottomLayer\",\"R0603\",\"30.480\",\"5.080\",\"180\",\"Resistor\"\r\n"
        "\"U1\",\"MCU\",\"TopLayer\",\"QFN32\",\"25.000\",\"25.000\",\"45\",\"Micro \"\"A\"\"\"\r\n";
    TextTable a;
    CHECK(read_text_table(altium, {}, a, err));
    std::printf("Altium CSV: %s\n", a.describe().c_str());
    CHECK(a.delim == Delim::Comma && a.ncols() == 8 && a.rows.size() == 3 && a.has_header);
    CHECK(a.rows[0][7] == "Cap, ceramic");
    CHECK(a.rows[2][7] == "Micro \"A\"");
    CHECK(!a.preamble.empty());

    // semicolon / pipe / tab
    TextTable s;
    CHECK(read_text_table("Ref;X;Y;Rot\nR1;1,5;2,5;0\nR2;3,0;4,0;90\n", {}, s, err));
    CHECK(s.delim == Delim::Semicolon && s.rows.size() == 2 && s.has_header);
    TextTable p;
    CHECK(read_text_table("// pipes\nR1|100|200|0\nR2|150|250|90\nR3|175|275|180\n", {}, p, err));
    CHECK(p.delim == Delim::Pipe && p.rows.size() == 3 && !p.has_header && p.skipped_comment == 1);
    TextTable tb;
    CHECK(read_text_table("RefDes\tX\tY\nU1\t1.0\t2.0\nU2\t3.0\t4.0\n", {}, tb, err));
    CHECK(tb.delim == Delim::Tab && tb.has_header && tb.ncols() == 3);

    // unaligned whitespace (Eagle .mnt style)
    TextTable e;
    CHECK(read_text_table("R1 10.16 20.32 90 10k R0805\nC12 5.5 7.25 0 100n C0603\nU3 50.8 12.7 270 ATMEGA TQFP32\n", {}, e, err));
    std::printf("Eagle mnt: %s\n", e.describe().c_str());
    CHECK(e.ncols() == 6 && e.rows.size() == 3);
    CHECK(e.rows[1][0] == "C12" && e.rows[1][5] == "C0603");

    // skip_lines + explicit fixed cuts
    TextTableOptions fo;
    fo.skip_lines = 1;
    fo.delim = Delim::Fixed;
    fo.fixed_cuts = {0, 5, 10};
    TextTable f;
    CHECK(read_text_table("garbage line\nR1   100  200\nR22  101  201\n", fo, f, err));
    CHECK(f.rows.size() == 2 && f.rows[1][0] == "R22" && f.rows[1][2] == "201");
}

// ---------------------------------------------------------------------------
static int count_comps(const Project& p, Feature f) {
    int n = 0;
    for (auto& c : p.comps) if (c.feature == f) ++n;
    return n;
}

static void test_gencad(Project& cad) {
    std::printf("== gencad import\n");
    std::string err;
    GencadResult r;
    CHECK(import_gencad_file(kCad, cad, {}, r, err));
    std::printf("%s\n", r.describe().c_str());
    CHECK(r.version == "1.4");
    CHECK(r.units == "INCH");
    CHECK(r.pads == 12 && r.shapes == 24 && r.components == 86 && r.devices == 25);
    CHECK(cad.comps.size() == 86);
    CHECK(count_comps(cad, Feature::Fiducial) == 3);
    CHECK(cad.packages.size() == 24);
    CHECK(!cad.board_frame.empty());
    const Package& pq = cad.packages.at("PQFP132");
    CHECK(pq.pads.size() == 132);
    CHECK(near(pq.pads[0].pos.x, -0.522 * 25.4, 1e-6) && near(pq.pads[0].w, 0.086 * 25.4, 1e-6));
    const Package& hdr = cad.packages.at("HDR6");
    CHECK(hdr.pads[0].shape == PadShape::Rect && hdr.pads[1].shape == PadShape::Round);
    CHECK(near(hdr.pads[1].w, 0.072 * 25.4, 1e-6));
    CHECK(cad.packages.at("TRIMR").body.size() == 4 && cad.packages.at("TRIMR").body[0].kind == GKind::Arc);
    const Component* u6 = cad.find_ref("U6");
    CHECK(u6 && near(u6->pos.x, 5.522 * 25.4, 1e-6) && u6->package == "PQFP132" && u6->number == "88521-201-0025");
    const Component* r1 = cad.find_ref("R1");
    CHECK(r1 && near(r1->rot, 90, 1e-9));
    CHECK(cad.parts.count("8555-901-5236") && cad.parts.at("8555-901-5236").description == "Capacitor (0805 SMT Package)");
    const Component* vr1 = cad.find_ref("VR1");
    CHECK(vr1 && vr1->number.empty());
    BBox bb = cad.board_bbox();
    std::printf("board bbox %.3f,%.3f - %.3f,%.3f mm, %zu polylines\n", bb.x0, bb.y0, bb.x1, bb.y1, cad.board_frame.size());
}

// ---------------------------------------------------------------------------
static void test_centroid_vs_gencad(const Project& cad) {
    std::printf("== centroid (Reference File.txt) vs GenCAD\n");
    std::string err;
    Project cen;
    CentroidOptions o;
    o.mode = CentroidMode::Replace;
    CentroidReport rep;
    CHECK(import_centroid_file(kRef, cen, o, rep, err));
    std::printf("%s\n", rep.describe().c_str());
    CHECK(rep.units == LenUnit::Mil);
    CHECK(rep.map.ref == 0 && rep.map.x == 1 && rep.map.y == 2 && rep.map.rot == 3);
    CHECK(cen.comps.size() == 98);

    // refs in common
    int common = 0;
    std::set<std::string> cad_refs, cen_refs;
    for (auto& c : cad.comps) cad_refs.insert(c.ref);
    for (auto& c : cen.comps) { cen_refs.insert(c.ref); if (cad_refs.count(c.ref)) ++common; }
    int cad_only = 0, cen_only = 0;
    std::string co, eo;
    for (auto& r : cad_refs) if (!cen_refs.count(r)) { ++cad_only; co += " " + r; }
    for (auto& r : cen_refs) if (!cad_refs.count(r)) { ++cen_only; eo += " " + r; }
    std::printf("CCDemo.cad comps %zu, Reference File refs %zu, common %d\n  CAD only:%s\n  centroid only:%s\n",
                cad.comps.size(), cen.comps.size(), common, co.c_str(), eo.c_str());
    CHECK(common == 83);
    CHECK(cad_only == 3 && cen_only == 15);

    // origin delta: from X3 (0,0) == FID1
    const Component* fid1 = cad.find_ref("FID1");
    const Component* x3 = cen.find_ref("X3");
    CHECK(fid1 && x3);
    Vec2 d = fid1->pos - x3->pos;
    std::printf("origin delta (FID1 - X3) = %.4f, %.4f mm (%.1f, %.1f mil)\n", d.x, d.y, d.x / kMmPerMil, d.y / kMmPerMil);
    double maxdev = 0; std::string worst, outliers;
    int n_out = 0;
    std::map<std::string, std::map<int, int>> rotdelta;     // package -> delta -> count
    for (auto& c : cen.comps) {
        const Component* g = cad.find_ref(c.ref);
        if (!g) continue;
        double dv = dist(g->pos, c.pos + d);
        if (dv > 0.1) {     // genuine data disagreement between the two demo files
            ++n_out;
            char b[96]; std::snprintf(b, sizeof b, " %s(%.0f mil)", c.ref.c_str(), dv / kMmPerMil);
            outliers += b;
            continue;
        }
        if (dv > maxdev) { maxdev = dv; worst = c.ref; }
        int rd = (int)std::lround(norm_deg(g->rot - c.rot));
        rotdelta[g->package][rd]++;
    }
    std::printf("position: %d outliers >0.1 mm:%s; max deviation over remaining %d refs: %.4f mm (%.2f mil) at %s\n",
                n_out, outliers.c_str(), common - n_out, maxdev, maxdev / kMmPerMil, worst.c_str());
    CHECK(n_out == 2 && outliers.find("Y1") != std::string::npos && outliers.find("VR1") != std::string::npos);
    CHECK(maxdev < 0.03);
    int consistent = 0, total_pk = 0;
    std::printf("rotation delta (GenCAD - centroid) per package:");
    for (auto& [pk, m] : rotdelta) {
        ++total_pk;
        if (m.size() == 1) ++consistent;
        std::printf(" %s{", pk.c_str());
        for (auto& [dd, n] : m) std::printf("%d:%d ", dd, n);
        std::printf("}");
    }
    std::printf("\n%d/%d packages have a constant rotation delta\n", consistent, total_pk);
}

// ---------------------------------------------------------------------------
static void test_fusion(const Project& cad) {
    std::printf("== fusion\n");
    std::string err;
    std::string text;
    CHECK(read_file(kRef, text, err));
    // 1) auto-align, no refs at all
    {
        Project p = cad;
        std::map<uint32_t, std::string> truth;
        for (auto& c : p.comps) { truth[c.id] = c.ref; c.ref.clear(); }
        CentroidOptions o;
        o.mode = CentroidMode::Fusion;
        o.fusion_tol_mm = 0.25;
        CentroidReport rep;
        CHECK(import_centroid_text(text, p, o, rep, err));
        std::printf("auto-align: %s\n", rep.describe().c_str());
        int correct = 0, wrong = 0;
        for (auto& c : p.comps) {
            if (c.ref.empty()) continue;
            if (c.ref == truth[c.id]) ++correct;
            else if (truth[c.id].rfind("FID", 0) != 0) ++wrong;
        }
        std::printf("fused %d: %d refs restored correctly, %d wrong (fiducials take X-refs)\n", rep.fused, correct, wrong);
        CHECK(correct == 81 && wrong == 0);      // Y1 / VR1 positions differ between the demo files
        CHECK(p.find_ref("FID1") == nullptr && p.find_ref("X3") != nullptr);   // FID1 sits on X3 (0,0)
        CHECK(near(rep.fusion_delta.x / kMmPerMil, 505, 1.0) && near(rep.fusion_delta.y / kMmPerMil, 850, 1.0));
    }
    // 2) key component + clear refs, Pin method
    {
        Project p = cad;
        for (auto& c : p.comps) if (c.ref != "U6") c.ref = "?" + c.ref;
        CentroidOptions o;
        o.mode = CentroidMode::Fusion;
        o.key_ref = "U6";
        o.clear_refs = true;
        o.fusion_method = FusionMethod::Pin;
        CentroidReport rep;
        CHECK(import_centroid_text(text, p, o, rep, err));
        std::printf("key U6 / pin method: fused %d, unmatched file refs %zu, max dev %.4f mm\n",
                    rep.fused, rep.unmatched_refs.size(), rep.fusion_max_dev);
        CHECK(rep.fused >= 83);
        CHECK(p.find_ref("U14") && near(p.find_ref("U14")->pos.x, cad.find_ref("U14")->pos.x, 1e-9));
    }
}

// ---------------------------------------------------------------------------
static void test_bom(const Project& cad) {
    std::printf("== BOM\n");
    std::string err;
    Project p = cad;
    for (auto& c : p.comps) c.number.clear();
    p.parts.clear();
    BomOptions o;
    BomReport rep;
    CHECK(import_bom_file(kBom, p, o, rep, err));
    std::printf("%s\n", rep.describe().c_str());
    CHECK(rep.rows == 25);
    CHECK(rep.parts_added == 25);
    CHECK(rep.map.refs == 0 && rep.map.number == 1 && rep.map.package == 2 && rep.map.description == 3);
    CHECK(rep.map.process == 4 && rep.map.soft_rev == 6 && rep.map.vend_pn == 7 && rep.map.polarity == 8);
    CHECK(p.parts.at("8555-901-5236").package == "0805");
    CHECK(!p.parts.at("8555-901-5236").polarized);
    CHECK(p.parts.at("4589632-89-88").polarized == false);    // "N"
    CHECK(p.parts.at("4151354964").polarized == true);        // "Y"
    CHECK(p.parts.at("8452-901-0001").soft_ver == "Vers. 52.1.02");
    CHECK(p.parts.at("452-25005").default_process == "SLIDE");
    CHECK(p.find_ref("C34") && p.find_ref("C34")->number == "8555-901-5236");
    CHECK(p.find_ref("U16") && p.find_ref("U16")->number == "853236-856");   // BOM says "u16"
    CHECK(p.find_ref("R7") && p.find_ref("R7")->number == "7854-888-001");   // "R6-8"
    CHECK(rep.duplicate_refs.empty() && rep.pn_conflicts.empty());
    std::printf("BOM coverage %.1f%% (%d/%d)\n", rep.coverage(), rep.linked, rep.board_components);
    CHECK(rep.coverage() > 90.0);

    // synthetic CSV with duplicates / conflicts / polarity forms
    Project q = cad;
    const std::string csv =
        "Item,Qty,Designator,Part No,Manufacturer Part Number,Customer PN,Package,Polarized\n"
        "1,3,\"R1,R2,R9\",PN-A,MPN-A,CUST-A,AXIAL,0\n"
        "2,2,\"R9-R10\",PN-B,MPN-B,CUST-B,AXIAL,no\n"
        "3,1,ZZ99,PN-C,MPN-C,,QFP,yes\n";
    BomReport r2;
    CHECK(import_bom_text(csv, q, o, r2, err));
    std::printf("synthetic: %s\n", r2.describe().c_str());
    CHECK(r2.duplicate_refs.size() == 1 && r2.pn_conflicts.size() == 1);
    CHECK(q.find_ref("R9")->number == "PN-A");                   // multiple PN check keeps first
    CHECK(r2.refs_not_on_board.size() == 1 && r2.refs_not_on_board[0] == "ZZ99");
    CHECK(q.parts.at("PN-A").manf_pn == "MPN-A" && q.parts.at("PN-A").cust_pn == "CUST-A");
    CHECK(q.parts.at("PN-C").polarized);
    CHECK(r2.map.line_item == 0 && r2.map.qty == 1);
}

// ---------------------------------------------------------------------------
static void test_gencad_roundtrip(const Project& cad) {
    std::printf("== gencad round trip\n");
    std::string err, out;
    GencadExportOptions eo;
    CHECK(export_gencad(cad, eo, out, err));
    CHECK(out.find("USER \"Generated by PCB Plotter\"\r\n") != std::string::npos);
    CHECK(out.find("SHAPE PQFP132 0 0\r\n") != std::string::npos);
    Project p2;
    GencadResult r2;
    CHECK(import_gencad_text(out, p2, {}, r2, err));
    std::printf("re-import: %s\n", r2.describe().c_str());
    CHECK(p2.comps.size() == cad.comps.size());
    CHECK(p2.packages.size() == cad.packages.size());
    double maxpos = 0, maxpad = 0;
    int shape_mismatch = 0, rot_mismatch = 0, num_mismatch = 0;
    for (auto& c : cad.comps) {
        const Component* d = p2.find_ref(c.ref);
        if (!d) { ++shape_mismatch; continue; }
        maxpos = std::max(maxpos, dist(c.pos, d->pos));
        if (c.package != d->package) ++shape_mismatch;
        if (!near(c.rot, d->rot, 1e-6) || c.side != d->side) ++rot_mismatch;
        if (c.number != d->number) ++num_mismatch;
    }
    for (auto& [n, pk] : cad.packages) {
        auto it = p2.packages.find(n);
        if (it == p2.packages.end() || it->second.pads.size() != pk.pads.size()) { ++shape_mismatch; continue; }
        for (size_t i = 0; i < pk.pads.size(); ++i) {
            auto& a = pk.pads[i]; auto& b = it->second.pads[i];
            maxpad = std::max({maxpad, dist(a.pos, b.pos), std::fabs(a.w - b.w), std::fabs(a.h - b.h)});
            if (a.shape != b.shape || a.pin != b.pin) ++shape_mismatch;
        }
        if (it->second.body.size() != pk.body.size()) ++shape_mismatch;
    }
    std::printf("round trip: max pos dev %.6f mm, max pad dev %.6f mm, shape mismatches %d, rot %d, number %d\n",
                maxpos, maxpad, shape_mismatch, rot_mismatch, num_mismatch);
    CHECK(maxpos < 1e-6 && maxpad < 1e-6 && shape_mismatch == 0 && rot_mismatch == 0 && num_mismatch == 0);

    // second export must be byte-identical to the first (fixed point)
    std::string out2;
    CHECK(export_gencad(p2, eo, out2, err));
    CHECK(out2 == out);

    // filters
    GencadExportOptions bo; bo.side = SideFilter::Bottom;
    std::string ob;
    CHECK(!export_gencad(cad, bo, ob, err));        // demo board is top-only
    Project p3 = cad;
    int n0 = 0;
    for (auto& c : p3.comps) if (c.package == "0805") { c.assigned_point = 0; ++n0; }
    GencadExportOptions mo; mo.model_points = {0};
    std::string om;
    CHECK(export_gencad(p3, mo, om, err));
    Project p4; GencadResult r4;
    CHECK(import_gencad_text(om, p4, {}, r4, err));
    CHECK((int)p4.comps.size() == n0 && p4.packages.size() == 1);
    std::printf("model-point filter: %d comps exported, 1 shape\n", n0);

    // bottom side + MM units + arcs + padstacks (synthetic)
    const std::string g =
        "$HEADER\nGENCAD 1.4\nUNITS MM\n$ENDHEADER\n"
        "$BOARD\nLINE 0 0 50 0\nLINE 50 0 50 30\nARC 50 30 40 40 40 30\nLINE 40 40 0 40\nLINE 0 40 0 0\n$ENDBOARD\n"
        "$PADS\nPAD P1 ROUND 0.8\nCIRCLE 0 0 0.75\nPAD P2 RECTANGULAR -1\nRECTANGLE -0.5 -0.3 1.0 0.6\n$ENDPADS\n"
        "$PADSTACKS\nPADSTACK PS1 0.9\nPAD P1 TOP 0 0\nPAD P1 BOTTOM 0 0\n$ENDPADSTACKS\n"
        "$SHAPES\nSHAPE CHIP\nINSERT SMD\nPIN 1 P2 -1 0 TOP 0 0\nPIN 2 P2 1 0 TOP 0 0\n"
        "SHAPE \"HDR 2\"\nINSERT TH\nPIN 1 PS1 0 0 TOP 0 0\nPIN 2 PS1 2.54 0 TOP 0 0\n"
        "SHAPE FIDSHAPE\nFIDUCIAL P1 0 0 TOP 0 0\n$ENDSHAPES\n"
        "$COMPONENTS\nCOMPONENT R1\nDEVICE RES_10K\nPLACE 10 10\nLAYER BOTTOM\nROTATION 270\nSHAPE CHIP MIRRORY FLIP\n"
        "COMPONENT J1\nDEVICE CONN\nPLACE 20 20\nLAYER TOP\nROTATION 0\nSHAPE \"HDR 2\" 0 0\n"
        "COMPONENT F1\nPLACE 1 1\nLAYER TOP\nSHAPE FIDSHAPE\n$ENDCOMPONENTS\n"
        "$DEVICES\nDEVICE RES_10K\nPART 123-10K\nTYPE RES\nVALUE 10k\nDEVICE CONN\nPART 999\nDESC Header\n$ENDDEVICES\n"
        "$SIGNALS\nSIGNAL GND\nNODE R1 1\nNODE J1 2\n$ENDSIGNALS\n"
        "$ROUTES\nROUTE GND\nTRACK 1\n$ENDROUTES\n";
    Project s;
    GencadResult rs;
    CHECK(import_gencad_text(g, s, {}, rs, err));
    std::printf("synthetic: %s\n", rs.describe().c_str());
    CHECK(s.comps.size() == 3);
    CHECK(s.find_ref("R1")->side == Side::Bottom && near(s.find_ref("R1")->rot, 270, 1e-9));
    CHECK(s.find_ref("R1")->number == "123-10K" && s.parts.at("123-10K").description == "10k");
    CHECK(s.packages.at("HDR 2").smt == false && near(s.packages.at("HDR 2").pads[0].drill, 0.9, 1e-9));
    CHECK(s.packages.at("CHIP").smt == true);
    CHECK(s.find_ref("F1")->feature == Feature::Fiducial);
    CHECK(rs.nets.size() == 1 && rs.nets[0].nodes.size() == 2);
    CHECK(s.board_frame.size() == 1 && s.board_frame[0].front() == s.board_frame[0].back());
    std::string so;
    CHECK(export_gencad(s, {}, so, err));
    CHECK(so.find("LAYER BOTTOM") != std::string::npos && so.find("SHAPE CHIP MIRRORY FLIP") != std::string::npos);
    CHECK(so.find("SHAPE \"HDR 2\" 0 0") != std::string::npos);
    Project s2; GencadResult rs2;
    CHECK(import_gencad_text(so, s2, {}, rs2, err));
    CHECK(s2.find_ref("R1") && s2.find_ref("R1")->side == Side::Bottom && near(s2.find_ref("R1")->pos.x, 10, 1e-3));
    CHECK(s2.packages.at("HDR 2").pads[1].drill > 0.89);
}

// ---------------------------------------------------------------------------
static void test_centroid_synthetic() {
    std::printf("== centroid synthetic\n");
    std::string err;
    // Altium CSV, mm, bottom side
    const std::string altium =
        "Altium Designer Pick and Place Locations\n"
        "Units used:   mm\n\n"
        "\"Designator\",\"Comment\",\"Layer\",\"Footprint\",\"Center-X(mm)\",\"Center-Y(mm)\",\"Rotation\",\"Description\"\n"
        "\"C1\",\"100nF\",\"TopLayer\",\"C0402\",\"10.160\",\"20.320\",\"90\",\"Cap\"\n"
        "\"R7\",\"10k\",\"BottomLayer\",\"R0603\",\"30.480\",\"5.080\",\"180\",\"Resistor\"\n"
        "\"U1\",\"MCU\",\"TopLayer\",\"QFN32\",\"25.000\",\"25.000\",\"45\",\"Micro\"\n";
    {
        Project p;
        CentroidOptions o;
        CentroidReport r;
        CHECK(import_centroid_text(altium, p, o, r, err));
        std::printf("Altium: %s\n", r.describe().c_str());
        CHECK(r.units == LenUnit::MM);
        CHECK(p.comps.size() == 3);
        const Component* r7 = p.find_ref("R7");
        CHECK(r7 && r7->side == Side::Bottom && near(r7->pos.x, 30.48, 1e-9) && near(r7->rot, 180, 1e-9));
        CHECK(r7->package == "R0603");
        CHECK(r.map.value >= 0 && r.table.col_name(r.map.value) == "Comment");
        // side select + bottom mirror
        Project q;
        CentroidOptions o2;
        o2.side_select = SideSelect::Bottom;
        o2.bottom_mirror_x = true;
        CentroidReport r2;
        CHECK(import_centroid_text(altium, q, o2, r2, err));
        CHECK(q.comps.size() == 1 && near(q.comps[0].pos.x, -30.48, 1e-9) && r2.filtered == 2);
        // merge: move C1, add nothing new
        CentroidOptions o3;
        o3.mode = CentroidMode::Merge;
        o3.rot_offset = 90;
        CentroidReport r3;
        CHECK(import_centroid_text(altium, p, o3, r3, err));
        CHECK(p.comps.size() == 3 && r3.updated == 3 && near(p.find_ref("C1")->rot, 180, 1e-9));
    }
    // KiCad .pos (aligned, commented header, "## Unit = mm")
    const std::string kicad =
        "### Module positions - created on 2024-01-01 ###\n"
        "### Printed by Pcbnew version 7.0\n"
        "## Unit = mm, Angle = deg.\n"
        "## Side : All\n"
        "# Ref     Val       Package                PosX       PosY       Rot  Side\n"
        "C1        100n      C_0402_1005Metric   120.6500   -85.0900   90.0000  top\n"
        "C2        4.7u      C_0805_2012Metric   122.1000   -80.0000  180.0000  bottom\n"
        "R10       10k       R_0603_1608Metric     5.0000    -3.2500    0.0000  top\n"
        "## End\n";
    {
        Project p;
        CentroidReport r;
        CHECK(import_centroid_text(kicad, p, {}, r, err));
        std::printf("KiCad: %s\n", r.describe().c_str());
        CHECK(r.table.delim == Delim::Fixed && r.table.has_header);
        CHECK(r.units == LenUnit::MM);
        CHECK(p.comps.size() == 3);
        CHECK(p.find_ref("C2") && p.find_ref("C2")->side == Side::Bottom && near(p.find_ref("C2")->pos.y, -80, 1e-9));
        CHECK(p.find_ref("R10")->package == "R_0603_1608Metric");
    }
    // KiCad CSV, inch units guessed from header text "(in)" absent -> magnitude
    {
        Project p;
        CentroidReport r;
        CHECK(import_centroid_text("Ref,Val,Package,PosX,PosY,Rot,Side\nU1,LM358,SOIC8,1.2500,2.0000,90,top\nU2,LM358,SOIC8,3.1250,0.7500,0,bottom\n",
                                   p, {}, r, err));
        std::printf("KiCad CSV: units %s (%s)\n", len_unit_name(r.units), r.units_reason.c_str());
        CHECK(r.units == LenUnit::Inch && near(p.find_ref("U1")->pos.x, 1.25 * 25.4, 1e-9));
    }
    // Eagle mount (no header), value + package columns
    {
        Project p;
        CentroidReport r;
        CHECK(import_centroid_text("R1 10.16 20.32 90 10k R0805\nC12 5.5 7.25 0 100n C0603\nU3 50.8 12.7 270 ATMEGA TQFP32\n", p, {}, r, err));
        std::printf("Eagle: %s\n", r.describe().c_str());
        CHECK(r.map.ref == 0 && r.map.x == 1 && r.map.y == 2 && r.map.rot == 3 && r.map.value == 4 && r.map.package == 5);
        CHECK(r.units == LenUnit::MM);
        CHECK(p.find_ref("U3")->package == "TQFP32");
    }
    // Old Altium text with "mil" suffixed values and T/B side
    {
        Project p;
        CentroidReport r;
        const std::string t =
            "Designator Footprint  Mid X     Mid Y     Ref X     Ref Y     Pad X     Pad Y     TB  Rotation Comment\n"
            "C1         0805       1000mil   2000mil   1000mil   2000mil   960mil    2000mil   T   90.00    100n\n"
            "U2         SO8        2500mil   500mil    2500mil   500mil    2400mil   450mil    B   180.00   OPAMP\n";
        CHECK(import_centroid_text(t, p, {}, r, err));
        std::printf("Altium text: %s\n", r.describe().c_str());
        CHECK(r.table.col_name(r.map.x) == "Mid X" && r.table.col_name(r.map.side) == "TB");
        CHECK(r.table.col_name(r.map.ref) == "Designator" && r.table.col_name(r.map.package) == "Footprint");
        CHECK(r.table.col_name(r.map.value) == "Comment" && p.find_ref("U2")->package == "SO8");
        CHECK(p.find_ref("U2") && p.find_ref("U2")->side == Side::Bottom && near(p.find_ref("U2")->pos.x, 2.5 * 25.4, 1e-9));
    }
}

// ---------------------------------------------------------------------------
static std::string ipc_rec(const char* type, const char* net, const char* ref, const char* pin,
                           int drill, bool plated, int access, long x, long y, int w, int h, int rot) {
    // Build an 80-column IPC-D-356 record exactly as the column layout requires.
    std::string s(80, ' ');
    auto put = [&](size_t col1, const std::string& v) { for (size_t i = 0; i < v.size(); ++i) s[col1 - 1 + i] = v[i]; };
    put(1, type);
    put(4, net);
    put(21, ref);
    put(27, "-");
    put(28, pin);
    char buf[64];
    if (drill > 0) { std::snprintf(buf, sizeof buf, "D%04d%c", drill, plated ? 'P' : 'U'); put(33, buf); }
    std::snprintf(buf, sizeof buf, "A%02d", access); put(39, buf);
    std::snprintf(buf, sizeof buf, "X%c%06ldY%c%06ld", x < 0 ? '-' : '+', std::labs(x), y < 0 ? '-' : '+', std::labs(y)); put(42, buf);
    std::snprintf(buf, sizeof buf, "X%04dY%04dR%03d", w, h, rot); put(58, buf);
    put(74, "S0");
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static void test_ipc356() {
    std::printf("== IPC-D-356\n");
    std::string f;
    f += "C  IPC-D-356A test file\n";
    f += "P  JOB   TESTBOARD\n";
    f += "P  UNITS CUST 0\n";
    // R1: 0805 top SMT, pads 0.1" apart centred at (1.000, 1.000)"
    f += ipc_rec("327", "VCC", "R1", "1", 0, true, 1, 9500, 10000, 500, 600, 0) + "\n";
    f += ipc_rec("327", "GND", "R1", "2", 0, true, 1, 10500, 10000, 500, 600, 0) + "\n";
    // R2: same pattern elsewhere -> package dedupe
    f += ipc_rec("327", "SIG1", "R2", "2", 0, true, 1, 20500, 5000, 500, 600, 0) + "\n";
    f += ipc_rec("027", "N/C", "R2", "1", 0, true, 1, 19500, 5000, 500, 600, 0) + "\n";
    // C5: bottom-side SMT
    f += ipc_rec("327", "GND", "C5", "1", 0, true, 2, 30000, 30000, 400, 400, 0) + "\n";
    f += ipc_rec("327", "VCC", "C5", "2", 0, true, 2, 30000, 30800, 400, 400, 0) + "\n";
    // J1: 3-pin TH header, 0.1" pitch, drill 0.040"
    f += ipc_rec("317", "VCC", "J1", "1", 400, true, 0, 5000, 20000, 650, 650, 0) + "\n";
    f += ipc_rec("317", "GND", "J1", "2", 400, true, 0, 6000, 20000, 650, 650, 0) + "\n";
    f += ipc_rec("317", "SIG1", "J1", "3", 400, true, 0, 7000, 20000, 650, 650, 0) + "\n";
    // via + tooling hole (skipped)
    f += ipc_rec("317", "GND", "VIA", "", 120, true, 0, 15000, 15000, 250, 250, 0) + "\n";
    f += ipc_rec("367", "", "", "", 1250, false, 0, 1000, 1000, 0, 0, 0) + "\n";
    f += "999\n";
    Project p;
    Ipc356Result r;
    std::string err;
    CHECK(import_ipc356_text(f, p, {}, r, err));
    std::printf("%s\n", r.describe().c_str());
    CHECK(r.records == 10 && r.pins == 9 && r.vias == 1 && r.skipped == 1);
    CHECK(p.comps.size() == 4);
    CHECK(r.packages == 3);                  // R1/R2 share one package
    const Component* r1 = p.find_ref("R1");
    CHECK(r1 && near(r1->pos.x, 25.4, 1e-6) && near(r1->pos.y, 25.4, 1e-6));
    CHECK(p.find_ref("R2")->package == r1->package);
    CHECK(r1->pin1_set && p.packages.at(r1->package).pads[r1->pin1_index].pin == 1);
    const Package& rp = p.packages.at(r1->package);
    CHECK(near(rp.pads[0].pos.x, -1.27, 1e-6) && near(rp.pads[0].w, 1.27, 1e-6) && near(rp.pads[0].h, 0.6 * 2.54, 1e-6));
    const Component* c5 = p.find_ref("C5");
    CHECK(c5 && c5->side == Side::Bottom);
    // world pad positions for the bottom comp must match the file
    {
        Xform x = p.comp_xform(*c5, 0);
        const Package& cp = p.packages.at(c5->package);
        CHECK(near(x.apply(cp.pads[0].pos).y, 3.0 * 25.4, 1e-6) && near(x.apply(cp.pads[1].pos).y, 3.08 * 25.4, 1e-6));
    }
    const Component* j1 = p.find_ref("J1");
    const Package& jp = p.packages.at(j1->package);
    CHECK(!jp.smt && jp.pads.size() == 3 && near(jp.pads[0].drill, 1.016, 1e-6) && jp.pads[0].shape == PadShape::Round);
    CHECK(near(j1->pos.x, 0.6 * 25.4, 1e-6));
    size_t gnd_nodes = 0;
    for (auto& n : r.nets) if (n.name == "GND") gnd_nodes = n.nodes.size();
    CHECK(gnd_nodes == 3);
    CHECK(r.nets.size() == 3);               // VCC, GND, SIG1 (N/C dropped)

    // metric units
    std::string m = "P  UNITS CUST 1\n" + ipc_rec("327", "A", "U9", "A1", 0, true, 1, 12000, 8000, 300, 300, 0) + "\n" +
                    ipc_rec("327", "B", "U9", "B1", 0, true, 1, 12000, 7000, 300, 300, 0) + "\n999\n";
    Project q; Ipc356Result rq;
    CHECK(import_ipc356_text(m, q, {}, rq, err));
    const Component* u9 = q.find_ref("U9");
    CHECK(u9 && near(u9->pos.x, 12.0, 1e-9) && near(u9->pos.y, 7.5, 1e-9) && u9->pin1_set);
    CHECK(near(q.packages.at(u9->package).pads[0].w, 0.3, 1e-9));
}

int main() {
    test_text_table();
    Project cad;
    test_gencad(cad);
    test_centroid_vs_gencad(cad);
    test_fusion(cad);
    test_bom(cad);
    test_gencad_roundtrip(cad);
    test_centroid_synthetic();
    test_ipc356();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
