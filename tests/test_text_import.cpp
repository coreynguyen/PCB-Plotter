// ============================================================================
// test_text_import.cpp — tests for the text_import engine, pnp_import and
// bom_import2 on synthetic real-world-like samples (tests/data/text) and the
// fixed-width samples (DemoBOM.prn, Reference File.txt; synthetic, tests/data/text/gen_fixed_samples.py).
// ============================================================================
#include "../src/io/bom_import2.h"
#include "../src/io/io_util.h"
#include "../src/io/pnp_import.h"
#include "../src/io/text_import.h"
#include "../src/io/xlsx.h"
#include <cmath>
#include <cstdio>
#include <string>

using namespace pcbp;
using namespace pcbp::io;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string D(const std::string& f) { return "tests/data/text/" + f; }
static bool near(double a, double b, double t = 1e-6) { return std::fabs(a - b) <= t; }

static ParsedText A(const std::string& f, Purpose p, const ImportLayout& l = {}) {
    ParsedText t;
    std::string err;
    bool ok = analyze_file(D(f), l, p, t, err);
    if (!ok) std::printf("analyze %s: %s\n", f.c_str(), err.c_str());
    CHECK(ok);
    return t;
}

static void test_helpers() {
    std::printf("== helpers\n");
    auto v = expand_refs("R1-R5");
    CHECK(v.size() == 5 && v[4] == "R5");
    CHECK(expand_refs("R1..R3").size() == 3);
    CHECK(expand_refs("R1:R3").size() == 3);
    CHECK(expand_refs("R1~R3").size() == 3);
    v = expand_refs("C1,C3-5");
    CHECK(v.size() == 4 && v[1] == "C3" && v[3] == "C5");
    v = expand_refs("U1 U2");
    CHECK(v.size() == 2);
    v = expand_refs("r1 - r3, c7");
    CHECK(v.size() == 4 && v[0] == "R1" && v[3] == "C7");
    v = expand_refs("R1-5");
    CHECK(v.size() == 5);
    v = expand_refs("C1-C25, C27-C34");
    CHECK(v.size() == 33);
    v = expand_refs("U1A-U1D");
    CHECK(v.size() == 4 && v[3] == "U1D");
    std::vector<std::string> w;
    v = expand_refs("R5-R1", true, &w);
    CHECK(w.size() == 1 && v.size() == 1);
    v = expand_refs("U1-A");
    CHECK(v.size() == 1 && v[0] == "U1-A");
    v = expand_refs("R1 to R3");
    CHECK(v.size() == 3);
    CHECK(is_refdes("U$1") && is_refdes("TP12") && is_refdes("FID1") && !is_refdes("0805") && !is_refdes("10k"));
    CHECK(is_ref_list("R1, R2, R5") && !is_ref_list("Resistor, 5k Ohm"));

    CHECK(near(*parse_number("12,5", true), 12.5));
    CHECK(near(*parse_number("1.234,5", true), 1234.5));
    CHECK(near(*parse_number("1,000", false), 1000));
    CHECK(near(*parse_number("-3.25", false), -3.25));
    CHECK(!parse_number("12a", false));
    LenUnit u;
    CHECK(near(*parse_length("12.5mm", false, &u), 12.5) && u == LenUnit::MM);
    CHECK(near(*parse_length("500 mil", false, &u), 500) && u == LenUnit::Mil);
    CHECK(near(*parse_length("0.5in", false, &u), 0.5) && u == LenUnit::Inch);
    CHECK(near(*parse_length("0.5\"", false, &u), 0.5) && u == LenUnit::Inch);
    CHECK(near(*parse_length("-3,2 mm", true, &u), -3.2));
    CHECK(near(*parse_angle("90\xC2\xB0", false), 90) && near(*parse_angle("-90 deg", false), -90));
    CHECK(near(normalize_angle(-90), 270) && near(normalize_angle(720), 0));
    CHECK(cell_means_dnp("DNP", false) && !cell_means_dnp("Yes", true) && cell_means_dnp("No", true));
    CHECK(*parse_flag("TRUE") && !*parse_flag("N"));

    double cf;
    CHECK(role_from_header("Center-X(mm)", Purpose::Pnp, &cf, &u) == Role::X && u == LenUnit::MM);
    CHECK(role_from_header("Manufacturer Part Number 1", Purpose::Bom) == Role::MPN);
    CHECK(role_from_header("Mfr. #", Purpose::Bom) == Role::MPN);
    CHECK(role_from_header("LCSC Part #", Purpose::Bom) == Role::VendorPN);
    CHECK(role_from_header("Digi-Key Part Number", Purpose::Bom) == Role::VendorPN);
    CHECK(role_from_header("PCB Footprint", Purpose::Bom) == Role::Package);
    CHECK(role_from_header("Gehäuse", Purpose::Bom) == Role::Package);
    CHECK(role_from_header("Référence", Purpose::Bom) == Role::Ref);
    CHECK(role_from_header("\xE5\xB0\x81\xE8\xA3\x85", Purpose::Bom) == Role::Package);   // 封装
    bool inv = false;
    CHECK(role_from_header("SYM_MIRROR", Purpose::Pnp, &cf, &u, &inv) == Role::Side && inv);
    CHECK(role_from_header("Fitted", Purpose::Bom, &cf, &u, &inv) == Role::Dnp && inv);

    // decoding
    Encoding e;
    std::string s16 = std::string("\xFF\xFE", 2) + std::string("A\0\xE9\0\r\0\n\0", 8);
    CHECK(decode_text(s16, Encoding::Auto, &e) == "A\xC3\xA9\n" && e == Encoding::Utf16LE);
    std::string s16be = std::string("\0A\0B", 4);
    CHECK(decode_text(s16be + std::string("\0C\0D", 4), Encoding::Auto, &e) == "ABCD" && e == Encoding::Utf16BE);
    CHECK(decode_text("caf\xE9\r\nx", Encoding::Auto, &e) == "caf\xC3\xA9\nx" && e == Encoding::Cp1252);
    CHECK(decode_text("\xEF\xBB\xBFok\r", Encoding::Auto, &e) == "ok\n" && e == Encoding::Utf8Bom);
    auto sp = split_delim("a,\"b,\"\"c\"\"\",d", ',');
    CHECK(sp.size() == 3 && sp[1] == "b,\"c\"");
}

static void test_bom_files() {
    std::printf("== BOM samples\n");
    {
        auto t = A("DemoBOM.prn", Purpose::Bom);
        std::printf("%s", t.describe().c_str());
        CHECK(t.sep == Sep::Fixed && t.ncols() == 9);
        CHECK(t.header_commented && t.cols[1].header == "Part Number");
        CHECK(t.cols[5].header == "Cage Code");
        CHECK(t.col(Role::Ref) == 0 && t.col(Role::PartNumber) == 1 && t.col(Role::Package) == 2);
        CHECK(t.col(Role::Description) == 3 && t.col(Role::Process) == 4 && t.col(Role::Polarity) == 8);
        CHECK(t.col(Role::VendorPN) == 7 && t.col(Role::SoftRev) == 6);
        CHECK(t.data_rows() == 25);
    }
    {
        auto t = A("altium_bom.csv", Purpose::Bom);
        CHECK(t.sep == Sep::Comma && t.col(Role::Ref) == 2 && t.col(Role::PartNumber) == 4);
        CHECK(t.col(Role::Value) == 0 && t.col(Role::Qty) == 5 && t.col(Role::Package) == 3);
    }
    {
        auto t = A("jlc_bom.csv", Purpose::Bom);
        CHECK(t.col(Role::VendorPN) == 3 && t.col(Role::Ref) == 1 && t.col(Role::Value) == 0);
    }
    {
        auto t = A("excel_quotes.csv", Purpose::Bom);
        CHECK(t.excel_sep_hint && t.data_rows() == 2);
        CHECK(t.rows.size() >= 3);
        int data = 0, summ = 0;
        for (auto& r : t.rows) {
            if (r.kind == RowKind::Data) {
                if (data == 0) { CHECK(r.cells[0] == "R1,R2"); CHECK(r.cells[2] == "0805"); CHECK(r.cells[3] == "multi\nline note"); CHECK(r.line_count == 2); }
                if (data == 1) CHECK(r.cells[3] == "say \"hi\"");
                ++data;
            }
            summ += r.kind == RowKind::Summary;
        }
        CHECK(summ == 1);
    }
    {
        auto t = A("utf16.csv", Purpose::Bom);
        CHECK(t.encoding == Encoding::Utf16LE && t.sep == Sep::Tab && t.cols[0].header == "Reference");
        CHECK(t.col(Role::Qty) == 2 && t.data_rows() == 2);
    }
    {
        auto t = A("orcad_bom.txt", Purpose::Bom);
        CHECK(t.header_line == 2 && t.col(Role::Ref) == 2 && t.col(Role::Package) == 4 && t.col(Role::Qty) == 1);
        CHECK(t.col(Role::LineItem) == 0);
    }
    {
        auto t = A("chinese_bom.csv", Purpose::Bom);
        CHECK(t.encoding == Encoding::Utf8Bom && t.col(Role::Ref) == 1 && t.col(Role::Qty) == 2 && t.col(Role::Package) == 3);
    }
    {
        auto t = A("dnp_bom.csv", Purpose::Bom);
        CHECK(t.col(Role::Dnp) == 2 && t.cols[2].invert);
    }
}

static void test_pnp_files() {
    std::printf("== PNP samples\n");
    {
        auto t = A("Reference File.txt", Purpose::Pnp);
        CHECK(t.sep == Sep::Fixed && t.ncols() == 4 && t.header_line < 0);
        CHECK(t.col(Role::Ref) == 0 && t.col(Role::X) == 1 && t.col(Role::Y) == 2 && t.col(Role::Rotation) == 3);
    }
    {
        auto t = A("altium_pnp.txt", Purpose::Pnp);
        CHECK(t.sep == Sep::Comma && t.header_line == 6 && t.data_rows() == 3);
        CHECK(t.col(Role::X) == 4 && t.cols[4].unit == LenUnit::MM && t.col(Role::Side) == 2 && t.unit_hint == LenUnit::MM);
    }
    {
        auto t = A("kicad.pos", Purpose::Pnp);
        CHECK(t.header_commented && t.data_rows() == 3);
        CHECK(t.col(Role::Ref) == 0 && t.col(Role::Value) == 1 && t.col(Role::Package) == 2 && t.col(Role::X) == 3 &&
              t.col(Role::Y) == 4 && t.col(Role::Rotation) == 5 && t.col(Role::Side) == 6);
        CHECK(t.unit_hint == LenUnit::MM);
    }
    {
        auto t = A("eagle.mnt", Purpose::Pnp);
        CHECK(t.ncols() == 6 && t.col(Role::Ref) == 0 && t.col(Role::X) == 1 && t.col(Role::Y) == 2 &&
              t.col(Role::Rotation) == 3 && t.col(Role::Value) == 4 && t.col(Role::Package) == 5);
    }
    {
        auto t = A("cadence_fixed.txt", Purpose::Pnp);
        CHECK(t.sep == Sep::Fixed && t.ncols() == 7 && t.header_line == 3);
        CHECK(t.col(Role::Side) == 6 && t.cols[6].invert && t.col(Role::PartNumber) == 1 && t.col(Role::Package) == 2);
        // empty X cell on the J1 row stays empty (fixed columns)
        for (auto& r : t.rows) if (r.kind == RowKind::Data && r.cells[0] == "J1") CHECK(r.cells[3].empty() && r.cells[4] == "100.00");
    }
    {
        auto t = A("semicolon_dc.csv", Purpose::Pnp);
        CHECK(t.encoding == Encoding::Cp1252 && t.sep == Sep::Semicolon && t.decimal_comma);
        CHECK(t.col(Role::Ref) == 0 && t.col(Role::Side) == 4 && t.col(Role::Package) == 5);
    }
    {
        auto t = A("headerless.csv", Purpose::Pnp);
        CHECK(t.header_line < 0 && t.col(Role::Ref) == 0 && t.col(Role::X) == 1 && t.col(Role::Rotation) == 3 && t.col(Role::Package) == 4);
    }
    {
        auto t = A("multirow_header.csv", Purpose::Pnp);
        CHECK(t.header_rows == 2 && t.cols[1].unit == LenUnit::Mil && t.col(Role::X) == 1 && t.data_rows() == 2);
    }
    {
        auto t = A("pads_pnp.txt", Purpose::Pnp);
        CHECK(t.col(Role::Ref) == 0 && t.col(Role::PartNumber) == 1 && t.col(Role::Package) == 2 && t.col(Role::Side) == 6);
    }
}

static void test_layout_overrides() {
    std::printf("== layout overrides\n");
    ImportLayout l;
    l.sep = Sep::Fixed;
    l.fixed_cuts = {0, 24, 34};
    auto t = A("Reference File.txt", Purpose::Pnp, l);
    CHECK(t.ncols() == 3 && t.col(Role::X) == 1);   // Y+rot merged into col 2
    l = ImportLayout{};
    l.role_override[3] = Role::Unused;
    l.role_override[1] = Role::Y;
    l.role_override[2] = Role::X;
    t = A("Reference File.txt", Purpose::Pnp, l);
    CHECK(t.col(Role::Rotation) < 0 && t.col(Role::X) == 2 && t.col(Role::Y) == 1 && t.cols[1].role_src == RoleSource::User);
    l = ImportLayout{};
    l.header_line = -2;
    t = A("altium_bom.csv", Purpose::Bom, l);
    CHECK(t.header_line < 0 && t.data_rows() == 5);
    l = ImportLayout{};
    l.skip_lines = 6;
    t = A("altium_pnp.txt", Purpose::Pnp, l);
    CHECK(t.header_line == 6 && t.data_rows() == 3);
}

static void test_templates() {
    std::printf("== templates\n");
    ImportLayout l;
    l.role_override[3] = Role::Unused;
    auto t = A("Reference File.txt", Purpose::Pnp, l);
    FormatTemplate f = make_template("CCAM ref", Purpose::Pnp, t, l);
    f.units = LenUnit::Mil;
    f.bottom_rot = BottomRot::Mirror180;
    auto kt = A("kicad.pos", Purpose::Pnp);
    FormatTemplate k = make_template("KiCad", Purpose::Pnp, kt, {});
    std::string path = "build/tests/templates.json", err;
    CHECK(save_templates(path, {f, k}, err));
    std::vector<FormatTemplate> v;
    CHECK(load_templates(path, v, err) && v.size() == 2);
    CHECK(v[0].name == "CCAM ref" && v[0].units == LenUnit::Mil && v[0].bottom_rot == BottomRot::Mirror180);
    CHECK(v[0].layout.sep == Sep::Fixed && v[0].layout.fixed_cuts == t.cuts && v[0].index_roles.size() == 4);
    CHECK(v[1].signature.size() == 7);
    double sc = 0;
    CHECK(match_template(v, kt, Purpose::Pnp, 0.75, &sc) == 1 && sc > 0.99);
    auto rt = A("Reference File.txt", Purpose::Pnp);
    CHECK(match_template(v, rt, Purpose::Pnp) == 0);
    // apply template roles (rotation unused as stored)
    ImportLayout tl = template_layout(v[0]);
    ParsedText t2;
    CHECK(analyze_file(D("Reference File.txt"), tl, Purpose::Pnp, t2, err));
    apply_template_roles(v[0], t2, tl);
    CHECK(t2.col(Role::Rotation) < 0 && t2.col(Role::X) == 1 && t2.cols[1].role_src == RoleSource::Template);
}

static void test_pnp_import() {
    std::printf("== pnp_import\n");
    std::string err;
    {
        PnpOptions o;
        PnpPreview p;
        CHECK(pnp_preview_file(D("Reference File.txt"), o, p, err));
        CHECK(p.mapping_ok() && p.units == LenUnit::Mil && p.rows.size() == 98);
        CHECK(p.rows[0].ref == "U14" && near(p.rows[0].pos.x, 3120 * 0.0254) && near(p.rows[0].rot, 90));
        Project prj;
        prj.comps.clear();
        CentroidReport rep;
        o.apply.mode = CentroidMode::Replace;
        CHECK(pnp_apply(p, prj, o, rep, err));
        CHECK(rep.added == 98 && prj.comps.size() == 98);
    }
    {
        PnpOptions o;
        PnpPreview p;
        CHECK(pnp_preview_file(D("kicad.pos"), o, p, err));
        CHECK(p.units == LenUnit::MM && p.rows.size() == 3 && p.rows[2].side == Side::Bottom);
        CHECK(near(p.rows[0].pos.x, 101.25) && near(p.rows[0].pos.y, -45.5));
        o.side_select = SideSelect::Top;
        pnp_rebuild(p, o);
        CHECK(p.rows.size() == 2 && p.filtered == 1);
    }
    {
        PnpOptions o;
        o.bottom_x = BottomX::Negate;
        o.bottom_rot = BottomRot::Mirror180;
        o.rot_offset = 90;
        PnpPreview p;
        CHECK(pnp_preview_file(D("jlc_cpl.csv"), o, p, err));
        CHECK(p.rows.size() == 3 && near(p.rows[0].pos.x, 10.5) && near(p.rows[0].rot, 180));
        CHECK(p.rows[1].side == Side::Bottom && near(p.rows[1].pos.x, -12) && near(p.rows[1].rot, 90));   // 180-180+90
    }
    {
        PnpOptions o;
        PnpPreview p;
        CHECK(pnp_preview_file(D("semicolon_dc.csv"), o, p, err));
        CHECK(p.rows.size() == 3 && near(p.rows[0].pos.x, 12.5) && near(p.rows[0].pos.y, 3.25) && p.rows[1].side == Side::Bottom);
        CHECK(p.rows[0].package == "0805");
    }
    {
        PnpOptions o;
        PnpPreview p;
        CHECK(pnp_preview_file(D("cadence_fixed.txt"), o, p, err));
        CHECK(p.units == LenUnit::Mil && p.rows.size() == 3 && p.skipped == 1);   // J1 has no X
        CHECK(p.rows[1].ref == "R12" && p.rows[1].side == Side::Bottom && p.rows[0].side == Side::Top);
    }
    {
        PnpOptions o;
        PnpPreview p;
        CHECK(pnp_preview_file(D("pads_pnp.txt"), o, p, err));
        CHECK(p.units == LenUnit::Inch && near(p.rows[0].pos.x, 1.25 * 25.4));
    }
    {
        PnpOptions o;
        PnpPreview p;
        CHECK(pnp_preview_file(D("eagle.mnt"), o, p, err));
        CHECK(p.units == LenUnit::MM && p.rows.size() == 4 && p.rows[2].package == "TQFP32");
    }
    {
        PnpOptions o;
        PnpPreview p;
        CHECK(pnp_preview_file(D("multirow_header.csv"), o, p, err));
        CHECK(p.units == LenUnit::Mil && near(p.rows[0].pos.x, 25.4));
    }
}

static void test_bom_import() {
    std::printf("== bom_import2\n");
    std::string err;
    Project prj;
    {
        PnpOptions o;
        o.apply.mode = CentroidMode::Replace;
        CentroidReport rep;
        CHECK(import_pnp_file(D("Reference File.txt"), prj, o, rep, nullptr, err));
    }
    BomOptions2 bo;
    BomPreview p;
    CHECK(bom_preview_file(D("DemoBOM.prn"), bo, p, err));
    CHECK(p.mapping_ok() && p.lines.size() == 25);
    CHECK(p.by_part.size() == 25);
    bool c_ok = false;
    for (auto& pr : p.by_part) if (pr.pn == "CAP-0805-100N") c_ok = pr.refs.size() == 33;
    CHECK(c_ok);
    BomReport rep;
    CHECK(bom_apply(p, prj, bo, rep, err));
    std::printf("%s\n", rep.describe().c_str());
    // BOM packages are not written into the part library unless committed explicitly (REVIEW_V9 §11G)
    CHECK(rep.linked > 80 && prj.parts.count("CAP-0805-100N") && prj.parts["CAP-0805-100N"].package.empty());
    CHECK(bom_commit_packages(prj) > 0 && prj.parts["CAP-0805-100N"].package == "0805");
    CHECK(prj.parts["RNET-SIP10-10K"].polarized && !prj.parts["RES-AX-5K"].polarized);
    // qty mismatch / dnp
    {
        BomPreview q;
        CHECK(bom_preview_file(D("altium_bom.csv"), bo, q, err));
        CHECK(q.qty_mismatches == 0 && q.lines.size() == 4);
        CHECK(q.lines[3].dnp);   // "DNP" comment
        std::string csv = "Ref,PN,Qty\nR1-R3,P1,2\nR4,P2,1\nR4,P3,1\n";
        CHECK(bom_preview(csv, bo, q, err));
        CHECK(q.qty_mismatches == 1 && q.pn_conflicts.size() == 1 && q.duplicate_refs.size() == 1);
        bool keep_first = false;
        for (auto& c : q.by_comp) if (c.ref == "R4") keep_first = c.pn == "P2" && c.conflict;
        CHECK(keep_first);
        BomOptions2 b2 = bo;
        b2.multiple_pn_check = false;
        bom_rebuild(q, b2);
        for (auto& c : q.by_comp) if (c.ref == "R4") CHECK(c.pn == "P3");
    }
    {
        Project p2;
        Component c;
        c.ref = "R1"; p2.add_component(c);
        c.ref = "R2"; p2.add_component(c);
        c.ref = "C1"; p2.add_component(c);
        BomPreview q;
        CHECK(bom_preview_file(D("dnp_bom.csv"), bo, q, err));
        BomReport r2;
        CHECK(bom_apply(q, p2, bo, r2, err));
        // DNP: identity + excluded status kept, no active part-number link
        CHECK(!p2.find_ref("R1")->dnp && p2.find_ref("R2")->dnp && p2.find_ref("R2")->number.empty());
        CHECK(p2.find_ref("R2")->bom_state == BomState::Dnp && p2.find_ref("R1")->bom_state == BomState::Listed);
        CHECK(comp_state(*p2.find_ref("R2")) == CompState::Dnp);
        BomOptions2 b3 = bo;
        b3.dnp = DnpMode::Skip;
        b3.link = BomLinkMode::Relink;
        CHECK(bom_apply(q, p2, b3, r2, err));
        CHECK(p2.find_ref("R2")->number.empty());
    }
    {
        // no PN column: synthesized from value + package
        BomPreview q;
        CHECK(bom_preview_file(D("jlc_bom.csv"), bo, q, err));
        CHECK(q.mapping_ok() && q.lines[0].pn == "100nF_0402" && q.lines[0].pn_synth && q.lines[1].refs.size() == 3);
    }
}

static ParsedText AT(const std::string& text, Purpose p, const ImportLayout& l) {
    ParsedText t;
    std::string err;
    bool ok = analyze_text(text, l, p, t, err);
    if (!ok) std::printf("analyze_text: %s\n", err.c_str());
    CHECK(ok);
    return t;
}

static void test_wizard_layout() {
    std::printf("== wizard layout (explicit mode / delimiters / formats)\n");
    // split_multi primitives
    auto v = split_multi("a;b\tc", ";\t", "");
    CHECK(v.size() == 3 && v[2] == "c");
    v = split_multi("a::b::\"c::d\"", "", "::");
    CHECK(v.size() == 3 && v[2] == "c::d");
    v = split_multi("  R1   10.5    20  ", " ", "", '"', true);
    CHECK(v.size() == 3 && v[0] == "R1" && v[2] == "20");
    v = split_multi("a,,b", ",", "", '"', false);
    CHECK(v.size() == 3 && v[1].empty());
    v = split_multi("a,,b", ",", "", '"', true);
    CHECK(v.size() == 2);
    v = split_multi("'x, y',z", ",", "", '\'');
    CHECK(v.size() == 2 && v[0] == "x, y");
    v = split_multi("\"x, y\",z", ",", "", 0);
    CHECK(v.size() == 3);
    // numeric normalisation
    CHECK(normalize_numeric_cell("1,234.5", ',', false, false, false) == "1234.5");
    CHECK(normalize_numeric_cell("1.234,5", '.', true, false, false) == "1234,5");
    CHECK(normalize_numeric_cell("12.5-", 0, false, true, false) == "-12.5");
    CHECK(normalize_numeric_cell("12.5mm", 0, false, false, true) == "-12.5mm");
    CHECK(normalize_numeric_cell("-3", 0, false, false, true) == "3");
    CHECK(normalize_numeric_cell("R12", ',', false, true, true) == "R12");
    CHECK(normalize_numeric_cell("0", 0, false, false, true) == "0");

    // explicit multi-char delimiter
    {
        ImportLayout l;
        l.mode = SplitMode::Delimited;
        l.other_delim = " :: ";
        auto t = AT("Ref :: X :: Y\nR1 :: 1.5 :: 2\nC2 :: 3 :: 4\n", Purpose::Pnp, l);
        CHECK(t.ncols() == 3 && t.data_rows() == 2 && t.mode == SplitMode::Delimited && t.other_delim == " :: ");
        CHECK(t.col(Role::X) == 1 && t.col(Role::Y) == 2);
    }
    // several delimiters at once + consecutive as one (mixed tabs / spaces)
    {
        ImportLayout l;
        l.mode = SplitMode::Delimited;
        l.delims = " \t";
        l.consecutive_as_one = true;
        auto t = AT("Designator\tX   Y\tRotation\nR1\t  10   20\t90\nR2  11\t\t21  180\n", Purpose::Pnp, l);
        CHECK(t.ncols() == 4 && t.data_rows() == 2);
        CHECK(t.col(Role::Rotation) == 3);
        if (t.data_rows() == 2) CHECK(t.rows.back().cells[2] == "21");
    }
    // consecutive spaces NOT merged -> empty cells kept
    {
        ImportLayout l;
        l.mode = SplitMode::Delimited;
        l.delims = " ";
        auto t = AT("a b  c\n1 2  3\n", Purpose::Generic, l);
        CHECK(t.ncols() == 4);
    }
    // text qualifier: single quote, and "none"
    {
        ImportLayout l;
        l.mode = SplitMode::Delimited;
        l.delims = ",";
        l.quote = '\'';
        auto t = AT("Ref,Description,PN\nR1,'Res, 10k',P1\n", Purpose::Bom, l);
        CHECK(t.ncols() == 3);
        int dcol = t.col(Role::Description);
        CHECK(dcol == 1 && t.rows.back().cells[1] == "Res, 10k");
        l.quote = 0;
        t = AT("Ref,Description,PN\nR1,\"x\",P1\n", Purpose::Bom, l);
        CHECK(t.rows.back().cells[1] == "\"x\"");
    }
    // quoted multi-line cell with explicit delimiter
    {
        ImportLayout l;
        l.mode = SplitMode::Delimited;
        l.delims = ";";
        auto t = AT("Ref;Note;PN\nR1;\"two\nlines\";P1\nR2;x;P2\n", Purpose::Bom, l);
        CHECK(t.data_rows() == 2 && t.rows[1].line_count == 2);
    }
    // fixed width with user cuts (the aperture-like file that auto-detect mangles)
    {
        std::string txt =
            "D10 ROUNDED 0.0100 0.0100\n"
            "D11 RECT    0.0600 0.0250\n"
            "D123ROUNDED 0.1000 0.1000\n";
        ImportLayout l;
        l.mode = SplitMode::Fixed;
        l.fixed_cuts = {4, 12, 19};
        l.header_line = -2;
        auto t = AT(txt, Purpose::Generic, l);
        CHECK(t.sep == Sep::Fixed && t.mode == SplitMode::Fixed && t.ncols() == 4);
        CHECK(t.data_rows() == 3);
        CHECK(t.rows[2].cells[0] == "D123" && t.rows[2].cells[1] == "ROUNDED" && t.rows[2].cells[3] == "0.1000");
        // the cut list without 0 gets a 0 prepended
        CHECK(t.cuts.size() == 4 && t.cuts[0] == 0);
        // suggestion helper
        auto c = suggest_fixed_cuts({"R1   10.0  20.0", "R22  11.5  21.0", "C3   12.0  22.5"});
        CHECK(c.size() == 3 && c[0] == 0 && c[1] == 5 && c[2] == 11);
    }
    // forced fixed mode on the Cadence report keeps working
    {
        ImportLayout l;
        l.mode = SplitMode::Fixed;
        auto t = A("cadence_fixed.txt", Purpose::Pnp, l);
        CHECK(t.sep == Sep::Fixed && t.col(Role::X) >= 0);
    }
    // stop line + comment prefix + start row
    {
        std::string txt = "junk title\n# comment\nRef,X,Y\nR1,1,2\nR2,3,4\n--- end ---\nTotal 2\n";
        ImportLayout l;
        l.skip_lines = 1;
        l.stop_line = 5;
        l.comment_prefixes = {"#"};
        auto t = AT(txt, Purpose::Pnp, l);
        CHECK(t.data_rows() == 2 && t.stop_line == 5 && t.header_line == 2);
    }
    // thousands separator, trailing minus, negate, per-column unit, skip column
    {
        std::string txt = "Ref;X;Y;Rot;Junk\nR1;1.250,5;2,0-;90;7\nR2;3,5;4;180;8\n";
        ImportLayout l;
        l.mode = SplitMode::Delimited;
        l.delims = ";";
        l.thousands_sep = '.';
        l.trailing_minus = true;
        l.negate[2] = true;
        l.unit_override[1] = LenUnit::Mil;
        l.col_format[4] = ColFormat::Skip;
        l.role_override[4] = Role::Height;     // a skipped column never gets a role
        auto t = AT(txt, Purpose::Pnp, l);
        CHECK(t.decimal_comma);
        CHECK(t.rows[1].cells[1] == "1250,5" && t.rows[1].cells[2] == "2,0");
        CHECK(t.rows[2].cells[2] == "-4");
        CHECK(t.cols[1].unit == LenUnit::Mil && t.cols[4].role == Role::Unused && t.cols[4].format == ColFormat::Skip);
        PnpOptions o;
        o.layout = l;
        PnpPreview pv;
        std::string err;
        CHECK(pnp_preview(txt, o, pv, err));
        CHECK(pv.rows.size() == 2 && pv.units == LenUnit::Mil);
        if (pv.rows.size() == 2) CHECK(near(pv.rows[0].pos.x, 1250.5 * 0.0254, 1e-6) && near(pv.rows[1].pos.y, -4 * 0.0254, 1e-6));
    }
    // Text format keeps leading zeros / disables numeric typing
    {
        ImportLayout l;
        l.col_format[1] = ColFormat::Text;
        auto t = AT("Ref,Code\nR1,0805\nR2,0603\n", Purpose::Bom, l);
        CHECK(t.cols[1].type == VType::Text && t.rows.back().cells[1] == "0603");
    }
    // template round trip of the new fields
    {
        ImportLayout l;
        l.mode = SplitMode::Delimited;
        l.delims = "\t;";
        l.other_delim = "||";
        l.consecutive_as_one = true;
        l.quote = '\'';
        l.stop_line = 40;
        l.thousands_sep = ',';
        l.trailing_minus = true;
        l.col_format[2] = ColFormat::Angle;
        l.unit_override[1] = LenUnit::Inch;
        l.negate[1] = true;
        auto t = AT("Ref\tX\tRot\nR1\t1\t2\n", Purpose::Pnp, l);
        auto tp = make_template("w", Purpose::Pnp, t, l);
        std::vector<FormatTemplate> vv{tp}, back;
        std::string err;
        CHECK(templates_from_json(templates_to_json(vv), back, err) && back.size() == 1);
        if (back.size() == 1) {
            const auto& b = back[0].layout;
            CHECK(b.mode == SplitMode::Delimited && b.delims == "\t;" && b.other_delim == "||" && b.consecutive_as_one);
            CHECK(b.quote == '\'' && b.stop_line == 40 && b.thousands_sep == ',' && b.trailing_minus);
            CHECK(b.col_format.at(2) == ColFormat::Angle && b.unit_override.at(1) == LenUnit::Inch && b.negate.at(1));
        }
    }
    // auto mode reports what it detected
    {
        auto t = A("semicolon_dc.csv", Purpose::Pnp);
        CHECK(t.mode == SplitMode::Delimited && t.delims == ";");
        auto f = A("cadence_fixed.txt", Purpose::Pnp);
        CHECK(f.mode == SplitMode::Fixed);
    }
}

// Spreadsheet entry (REVIEW_V9 §11A): xlsx opened directly, merged category row excluded,
// numeric-looking part numbers kept as typed.
static void test_bom_xlsx() {
    std::printf("== bom xlsx source\n");
    sheet::Book bk;
    auto& ws = bk.sheet(0);
    ws.name = "Notes";
    bk.sheets.emplace_back();
    bk.sheets[1].name = "Assembly BOM";
    auto& b = bk.sheets[1];
    auto put = [&](int r, int c, const std::string& v) { b.set_value_raw(r, c, sheet::Value::text(v)); };
    auto num = [&](int r, int c, double v) { b.set_value_raw(r, c, sheet::Value::number(v)); };
    put(0, 0, "Assembly BOM");
    b.merges.push_back(sheet::Range(0, 0, 0, 5));
    const char* hdr[] = {"Item", "Option", "QTY", "Designator", "Part Number", "Footprint"};
    for (int c = 0; c < 6; ++c) put(2, c, hdr[c]);
    put(3, 0, "CAPACITORS"); b.merges.push_back(sheet::Range(3, 0, 3, 5));
    num(4, 0, 1); num(4, 2, 2); put(4, 3, "C1, C2"); num(4, 4, 865080349013.0); put(4, 5, "0603");
    num(5, 0, 2); put(5, 1, "DNP"); num(5, 2, 1); put(5, 3, "C3"); put(5, 4, "0805B104"); put(5, 5, "0805");
    num(6, 0, 3); num(6, 2, 1); put(6, 3, "U1"); put(6, 4, "IC-1"); put(6, 5, "SOIC-8");
    put(8, 0, "Total"); num(8, 2, 4);
    auto bytes = io::xlsx_write(bk);
    BomSource src;
    std::string err;
    CHECK(bom_source_from_bytes(std::string(bytes.begin(), bytes.end()), "x.xlsx", src, err));
    CHECK(src.kind == "xlsx" && src.sheet == "Assembly BOM" && src.sheets.size() == 2);
    BomOptions2 bo;
    BomPreview p;
    CHECK(bom_preview_src(src, bo, p, err));
    CHECK(p.effective_header == 2 && p.lines.size() == 3);
    CHECK(p.status_of_line(3) && p.status_of_line(3)->reason == RowReason::Category);
    CHECK(p.lines.size() && p.lines[0].pn == "865080349013" && p.lines[1].dnp && p.lines[0].refs.size() == 2);
}

// R10: sectioned customer-style BOM (synthetic look-alike of a real export: title block,
// notes, header on line 8, category rows, alternates continuation row, Option=DNP, footer).
static void test_bom_r10() {
    std::printf("== bom r10 (row model / DNP / orphans / packages / history)\n");
    std::string err;
    BomOptions2 bo;
    BomSource src;
    CHECK(bom_load_source(D("bom_sectioned.csv"), src, err) && src.kind == "text");
    BomPreview p;
    CHECK(bom_preview_src(src, bo, p, err));
    CHECK(p.effective_header == 7 && p.text.col(Role::LineItem) == 0 && p.text.col(Role::Dnp) == 1 && p.text.col(Role::Ref) == 3);
    CHECK(p.text.col(Role::Manufacturer) == 4 && p.text.col(Role::Package) == 11);
    CHECK(p.lines.size() == 6 && p.by_comp.size() == 12);
    CHECK(p.status_of_line(8)->reason == RowReason::Category && p.status_of_line(13)->reason == RowReason::Category);
    CHECK(p.status_of_line(12)->reason == RowReason::Note && p.status_of_line(18)->reason == RowReason::Footer);
    CHECK(p.status_of_line(0)->reason == RowReason::BeforeFirst && p.status_of_line(7)->reason == RowReason::Header);
    CHECK(p.lines[0].item == "I1" && p.lines[4].pn == "000123");     // text, never numeric
    // delimiter detection is Info, never a warning
    bool info_only = true;
    for (auto& i : p.issues) if (i.msg.find("delimiter") != std::string::npos && i.sev != IssueSev::Info) info_only = false;
    CHECK(info_only);
    // DNP lines: separate part rows, never merged
    int dnp_rows = 0;
    for (auto& pr : p.by_part) if (pr.dnp) { ++dnp_rows; CHECK(pr.refs.size() == 1); }
    CHECK(dnp_rows == 2);
    CHECK(bom_text_is_dnp("DNS", bo.dnp_tokens) && bom_text_is_dnp("(DNI)", bo.dnp_tokens) && bom_text_is_dnp("NS", bo.dnp_tokens) &&
          !bom_text_is_dnp("1.5 NS delay", bo.dnp_tokens) && bom_text_is_dnp("10k DNP", bo.dnp_tokens) && !bom_text_is_dnp("DNPX", bo.dnp_tokens));
    // explicit row model: exclude a data row, force a note row in, move Last Data Row
    BomOptions2 b2 = bo;
    b2.rows.excluded.insert(9);
    bom_rebuild(p, b2);
    CHECK(p.lines.size() == 5 && p.status_of_line(9)->reason == RowReason::UserExcluded);
    b2.rows.excluded.clear();
    b2.rows.last_data = 14;
    CHECK(bom_preview_src(src, b2, p, err));
    CHECK(p.lines.size() == 4 && p.status_of_line(15)->reason == RowReason::AfterLast);
    // apply: board with C1..C5, R1..R6, U1 + an extra orphan TP1; DNP C2/C5 keep their own packages
    Project prj;
    for (const char* r : {"C1", "C2", "C3", "C4", "C5", "R1", "R2", "R3", "R4", "R5", "R6", "U1", "TP1"}) {
        Component c; c.ref = r;
        if (r[0] == 'C') c.package = r == std::string("C5") ? "CAP_1206" : "CAP_0603";
        if (r == std::string("R1")) c.package = "RES_0402_HAND";   // user-made package name
        prj.add_component(c);
    }
    CHECK(bom_preview_src(src, bo, p, err));
    BomReport rep;
    CHECK(bom_apply(p, prj, bo, rep, err));
    Component* c2 = prj.find_ref("C2"); Component* c5 = prj.find_ref("C5"); Component* tp = prj.find_ref("TP1");
    CHECK(c2->dnp && c2->number.empty() && c2->package == "CAP_0603" && c5->dnp && c5->package == "CAP_1206");
    CHECK(!prj.parts.count("CAP-0603-105") && !prj.parts.count("CAP-1206-475"));     // DNP never creates library parts
    CHECK(tp->bom_state == BomState::Orphan && !tp->dnp && comp_state(*tp) == CompState::Orphan);
    CHECK(prj.find_ref("R5")->number == "000123" && prj.find_ref("R5")->pn_src == DataSrc::Bom);
    CHECK(prj.find_ref("R5")->package == "0402" && prj.find_ref("R5")->pkg_src == DataSrc::Bom);   // empty -> filled from BOM
    // R13 (specs/BOM_PACKAGES.md): CAP_0603 == 0603 after normalisation; only the user-made
    // RES_0402_HAND (not a recognised chip name) disagrees with the BOM's 0402
    CHECK(rep.package_mismatch.size() == 1);   // R1
    CHECK(prj.find_ref("C1")->bom_package == "0603");
    // without a Package column there is no mismatch at all
    {
        Project q = Project{};
        Component c; c.ref = "R1"; c.package = "RES_0402_HAND"; q.add_component(c);
        BomPreview p2;
        CHECK(bom_preview(std::string("Ref,PN\nR1,P1\n"), bo, p2, err));
        BomReport r2;
        CHECK(bom_apply(p2, q, bo, r2, err) && r2.package_mismatch.empty() && q.find_ref("R1")->bom_package.empty());
        CHECK(q.parts["P1"].package.empty());
    }
    // reference errors are bounded + reported as errors
    {
        BomPreview p3;
        CHECK(bom_preview(std::string("Ref,PN\nR1-R999999,P1\nC5-C1,P2\n"), bo, p3, err));
        int errs = 0;
        for (auto& i : p3.issues) errs += i.sev == IssueSev::Error;
        CHECK(errs == 2 && p3.by_comp.size() == 2);
        auto sum = bom_summary(p3);
        CHECK(!sum.empty() && sum[0].sev == IssueSev::Error);
    }
    // summary: orphans are a warning, delimiter info stays info
    auto sum = bom_summary(p, &rep);
    bool orphan_warn = false;
    for (auto& x : sum) if (x.title.find("orphans") != std::string::npos) orphan_warn = x.sev == IssueSev::Warning;
    CHECK(orphan_warn);
    // wizard history: undo / redo / reset always consistent
    BomWizardHistory h;
    BomWizardState s0; h.reset_to(s0);
    BomWizardState s1 = s0; s1.rows.header_row = 7; h.push(s1);
    BomWizardState s2 = s1; s2.rows.excluded.insert(9); h.push(s2);
    h.push(s2);                                 // unchanged: no step
    CHECK(h.depth() == 2);
    CHECK(h.undo() == s1 && h.undo() == s0 && !h.can_undo() && h.redo() == s1);
    BomWizardState s3 = s1; s3.roles[2] = Role::Qty; h.push(s3);
    CHECK(!h.can_redo() && h.current() == s3);
    CHECK(h.reset() == s0 && h.undo() == s3);   // reset is undoable
}

static void test_kicad_jlc_bom_completion() {
    std::printf("== KiCad/JLCPCB BOM completion\n");
    const std::string csv =
        "Comment,Designator,Footprint,LCSC\n"
        "LED RED,D1,LED_SMD:LED_0603_1608Metric,C2286\n"
        "1k,R1,Resistor_SMD:R_0603_1608Metric,C21190\n"
        "HX6286,U1,Package_TO_SOT_SMD:SOT-23W,C495736\n";
    BomOptions2 opt;
    BomPreview p;
    std::string err;
    CHECK(bom_preview(csv, opt, p, err));
    CHECK(p.profile == BomProfile::KiCadJlcpcb);
    CHECK(p.lines.size() == 3);
    if (p.lines.size() != 3) return;
    CHECK(p.mapping_ok());
    CHECK(p.lines[1].pn == "LCSC:C21190");
    CHECK(p.lines[1].mpn.empty());  // supplier code is not an MPN
    CHECK(p.lines[1].vendor_pn == "C21190");
    CHECK(p.lines[1].package == "Resistor_SMD:R_0603_1608Metric");
    opt.verified_lookups["C21190"] = {"C21190", "UNI-ROYAL", "0603WAF1001T5E", "0603", "Resistor", ""};
    bom_rebuild(p, opt);
    CHECK(p.lines[1].pn == "LCSC:C21190"); // stable identity, unchanged by lookup
    CHECK(p.lines[1].manufacturer == "UNI-ROYAL");
    CHECK(p.lines[1].mpn == "0603WAF1001T5E");
    CHECK(p.lines[1].package == "Resistor_SMD:R_0603_1608Metric"); // never replace KiCad footprint
    Project project;
    BomReport report;
    CHECK(bom_apply(p, project, opt, report, err));
    CHECK(project.parts.count("LCSC:C21190") == 1);
    if (project.parts.count("LCSC:C21190")) {
        CHECK(project.parts.at("LCSC:C21190").manf_pn == "0603WAF1001T5E");
        CHECK(project.parts.at("LCSC:C21190").manufacturer == "UNI-ROYAL");
        CHECK(project.parts.at("LCSC:C21190").vend_pn == "C21190");
    }
    BomPreview generic;
    CHECK(bom_preview("Comment,Designator,Footprint,LCSC\n1k,R1,R_0603,CABCD\n", BomOptions2{}, generic, err));
    CHECK(generic.profile == BomProfile::KiCadJlcpcb);
    CHECK(generic.lines.size() == 1 && generic.lines[0].pn != "LCSC:CABCD");
    BomPreview native;
    CHECK(bom_preview("Reference,Value,Footprint,Qty\nR1,10k,R_0603,1\n", BomOptions2{}, native, err));
    CHECK(native.profile == BomProfile::KiCadNative);
}

int main() {
    test_helpers();
    test_bom_files();
    test_pnp_files();
    test_layout_overrides();
    test_templates();
    test_pnp_import();
    test_bom_import();
    test_wizard_layout();
    test_bom_r10();
    test_bom_xlsx();
    test_kicad_jlc_bom_completion();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
