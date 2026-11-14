#include "local_data.h"
#include <chrono>
// ============================================================================
// test_gerber.cpp — tests for io/gerber + io/digitize (plain main, Linux).
//   g++ -std=c++20 -O2 -I src tests/test_gerber.cpp src/io/gerber.cpp src/io/digitize.cpp
// Uses legacy demo data from tests/local_data.map (read-only) when present.
// ============================================================================
#include "../src/io/digitize.h"
#include "../src/io/gerber.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace pcbp;
using namespace pcbp::gerber;
namespace dz = pcbp::digitize;

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                                   \
    do {                                                                           \
        if (c) ++g_pass;                                                           \
        else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } \
    } while (0)
static bool near(double a, double b, double t = 1e-3) { return std::fabs(a - b) <= t; }

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static const std::string kRoot = pcbp_test::local("legacy_demo_root") + "/";   // legacy CAM demo install (local only)

// ---------------------------------------------------------------------------
// inline RS-274X tests
// ---------------------------------------------------------------------------
static Layer px(const std::string& s) {
    Layer L;
    std::string err;
    ParseOptions po;
    bool ok = parse(s, po, L, err);
    CHECK(ok);
    for (auto& w : L.warnings) std::printf("    warn: %s\n", w.c_str());
    return L;
}

static void test_basic() {
    std::puts("[274X basic apertures/draws]");
    Layer L = px("%FSLAX24Y24*%%MOIN*%\n%ADD10C,0.0100*%%ADD11R,0.0600X0.0400*%%ADD12O,0.05X0.1*%%ADD13P,0.1X6X30*%\n"
                 "G01*D10*X0Y0D02*X10000Y0D01*D11*X20000Y5000D03*D12*X30000D03*D13*X40000D03*M02*");
    CHECK(L.objs.size() == 4);
    CHECK(L.aps.at(11).shape == Aperture::Rect && near(L.aps.at(11).w, 1.524) && near(L.aps.at(11).h, 1.016));
    CHECK(L.objs[0].kind == Object::Draw && near(L.objs[0].b.x, 25.4));
    CHECK(near(L.objs[1].a.x, 50.8) && near(L.objs[1].a.y, 12.7));
    CHECK(L.aps.at(13).verts == 6 && near(L.aps.at(13).rot, 30));
    CHECK(near(L.extent.x0, -0.127) && near(L.extent.x1, 101.6 + 1.27));
    // hit test
    CHECK(hit_test(L, {50.8 + 0.7, 12.7}, 0.0) == 1);
    CHECK(hit_test(L, {50.8 + 0.8, 12.7}, 0.0) == -1);
    CHECK(hit_test(L, {12.0, 0.1}, 0.0) == 0);
    auto ol = flash_outline(L.aps.at(12), {0, 0}, 32);
    BBox ob;
    for (auto v : ol) ob.add(v);
    CHECK(near(ob.w(), 1.27, 1e-6) && near(ob.h(), 2.54, 1e-6));
    // metric, trailing zero suppression, incremental
    Layer M = px("%FSTIX33Y33*%%MOMM*%%ADD10C,0.2*%D10*X001Y001D02*X01D01*Y01D01*M02*");
    CHECK(M.objs.size() == 2 && near(M.objs[0].a.x, 1.0) && near(M.objs[0].b.x, 11.0) && near(M.objs[1].b.y, 11.0));
}

static void test_macro() {
    std::puts("[274X aperture macros]");
    Layer L = px(
        "%FSLAX26Y26*%%MOMM*%\n"
        "%AMRRECT*0 rounded-ish rect with variables*\n$3=$1-$2x2*\n21,1,$3,$2,0,0,0*\n1,1,$2x2,0-$3/2,0*\n1,1,$2x2,$3/2,0*%\n"
        "%AMTHERM*7,0,0,1.0,0.6,0.2,45*%\n"
        "%AMOUT*4,1,4,0,0,1,0,1,1,0,1,0,0,0*%\n"
        "%AMPOLY*5,1,8,0,0,2.0,0*%\n"
        "%AMVEC*20,1,0.2,-1,0,1,0,90*%\n"
        "%AMLL*22,1,1.0,0.5,0,0,0*%\n"
        "%AMHOLE*1,1,2,0,0*1,0,1,0,0*%\n"
        "%ADD10RRECT,2.0X0.25*%%ADD11THERM*%%ADD12OUT*%%ADD13POLY*%%ADD14VEC*%%ADD15LL*%%ADD16HOLE*%\n"
        "D10*X0Y0D03*D11*X5000000Y0D03*D12*X10000000Y0D03*D13*X15000000Y0D03*D14*X20000000D03*D15*X25000000D03*D16*X30000000D03*M02*");
    const Aperture& r = L.aps.at(10);
    CHECK(r.shape == Aperture::Macro && r.macro_polys.size() == 3);
    CHECK(near(r.w, 2.0, 1e-3) && near(r.h, 0.5, 2e-3));  // 1.5 + 2*0.25 caps
    CHECK(L.aps.at(11).macro_polys.size() == 4 && near(L.aps.at(11).w, 1.0, 0.02));
    CHECK(near(L.aps.at(12).w, 1.0) && near(L.aps.at(12).h, 1.0));
    CHECK(L.aps.at(13).macro_polys[0].size() == 8 && near(L.aps.at(13).w, 2.0, 1e-6));
    CHECK(near(L.aps.at(14).w, 0.2, 1e-6) && near(L.aps.at(14).h, 2.0, 1e-6));   // rotated 90
    CHECK(near(L.aps.at(15).w, 1.0) && near(L.aps.at(15).h, 0.5));
    CHECK(L.aps.at(16).macro_clear.size() == 2 && L.aps.at(16).macro_clear[1]);
    CHECK(L.flash_count() == 7);
    auto txt = aperture_list_to_text(L.aps);
    CHECK(txt.find("macro") != std::string::npos);
}

static void test_regions_arcs() {
    std::puts("[274X regions / arcs / polarity]");
    Layer L = px("%FSLAX24Y24*%%MOIN*%%ADD10C,0.01*%\n"
                 "G36*X0Y0D02*G01X10000Y0D01*Y10000D01*G75*G03X0Y10000I-5000J0D01*G01Y0D01*G37*\n"
                 "%LPC*%G36*X2000Y2000D02*X4000Y2000D01*X4000Y4000D01*X2000Y4000D01*X2000Y2000D01*G37*%LPD*%\n"
                 "D10*G75*X20000Y0D02*G02X20000Y0I5000J0D01*\n"
                 "G74*X30000Y0D02*G02X35000Y5000I5000J0D01*G01*M02*");
    CHECK(L.region_count() == 2);
    CHECK(L.objs[1].clear && !L.objs[0].clear);
    BBox rb = object_bbox(L, L.objs[0]);
    CHECK(near(rb.y1, 25.4 + 12.7, 0.01) && near(rb.x1, 25.4, 1e-6));   // semicircle top
    const Object& full = L.objs[2];
    CHECK(full.kind == Object::Arc && near(std::fabs(arc_sweep_deg(full)), 360));
    BBox fb = object_bbox(L, full);
    CHECK(near(fb.w(), 25.4 + 0.254, 1e-6));
    const Object& q = L.objs[3];   // single quadrant: centre must be (35000,0)
    CHECK(q.kind == Object::Arc && near(q.center.x, 88.9, 1e-6) && near(q.center.y, 0, 1e-6));
    CHECK(near(arc_sweep_deg(q), -90, 1e-6));
}

static void test_sr_image() {
    std::puts("[274X step-repeat / OF / MI / SF]");
    Layer L = px("%FSLAX24Y24*%%MOIN*%%ADD10C,0.05*%%SRX3Y2I1.0J2.0*%D10*X0Y0D03*X1000Y0D03*%SR*%X50000Y0D03*M02*");
    CHECK(L.flash_count() == 13);
    BBox b;
    for (auto& o : L.objs) b.add(o.a);
    CHECK(near(b.x1, 127.0) && near(b.y1, 50.8));
    Layer M = px("%FSLAX24Y24*%%MOIN*%%OFA1.0B0*%%MIA1*%%ADD10C,0.05*%D10*X10000Y10000D03*M02*");
    CHECK(near(M.objs[0].a.x, -25.4 + 25.4) && near(M.objs[0].a.y, 25.4));
    Layer S = px("%FSLAX24Y24*%%MOIN*%%SFA2B1*%%ADD10C,0.05*%D10*X10000Y10000D03*M02*");
    CHECK(near(S.objs[0].a.x, 50.8));
    Layer X2 = px("%TF.FileFunction,Copper,L1,Top*%%FSLAX24Y24*%%MOIN*%%TA.AperFunction,SMDPad*%%ADD10C,0.05*%%TD*%G04 comment X123 D03*D10*X1Y1D03*M02*");
    CHECK(X2.flash_count() == 1 && X2.warnings.empty());
}

static void test_excellon() {
    std::puts("[Excellon]");
    std::string d = "M48\nINCH,TZ\nT1C0.035\nT2C0.125\n%\nT1\nX01Y01\nX02\nY02\nT2\nX1.5Y-0.5\nX0Y0G85X1Y0\nM30\n";
    Layer L;
    std::string err;
    CHECK(parse(d, ParseOptions{}, L, err));
    CHECK(L.excellon && L.flash_count() == 4 && L.draw_count() == 1);
    CHECK(near(L.objs[0].a.x, 0.0001 * 25.4) && near(L.objs[2].a.y, 0.0002 * 25.4));
    CHECK(near(L.aps.at(10).w, 0.889) && near(L.aps.at(11).hole, 3.175));
    CHECK(near(L.objs[3].a.x, 38.1) && near(L.objs[3].a.y, -12.7));
    std::string d2 = "M48\nMETRIC,LZ\nT01C0.80\n%\nT01\nX010000Y005\nM30\n";   // LZ: trailing suppressed, 3.3
    Layer L2;
    CHECK(parse(d2, ParseOptions{}, L2, err));
    CHECK(L2.flash_count() == 1 && near(L2.objs[0].a.x, 10.0) && near(L2.objs[0].a.y, 5.0));
}

static void test_aperture_list_text() {
    std::puts("[aperture list round trip]");
    std::string t = "; D code  size  shape\nD10 12 round DRAW\nD11  62X85  rect FLASH  pad\n  12 oblong 20x40\n13 0.05\nnonsense line\n";
    auto r = parse_aperture_list(t);
    std::printf("    %s\n", r.explanation.c_str());
    CHECK(r.aps.size() == 4 && r.skipped == 1);
    CHECK(r.aps.at(11).shape == Aperture::Rect && near(r.aps.at(11).h, 85 * kMmPerMil));
    CHECK(r.aps.at(12).shape == Aperture::Obround && near(r.aps.at(12).w, 20 * kMmPerMil));
    CHECK(r.aps.at(13).shape == Aperture::Circle);
    auto back = parse_aperture_list(aperture_list_to_text(r.aps), ListUnits::Mil);
    CHECK(back.aps.size() == 4);
    for (auto& [d, a] : r.aps) {
        CHECK(back.aps.count(d) && back.aps.at(d).shape == a.shape && near(back.aps.at(d).w, a.w, 1e-3) && near(back.aps.at(d).h, a.h, 1e-3));
    }
}

// ---------------------------------------------------------------------------
// demo data
// ---------------------------------------------------------------------------
struct Ref { std::string ref; Vec2 pos; double rot; };

static std::vector<std::string> expand(const std::string& field) {
    std::vector<std::string> out;
    std::string f;
    for (char c : field) f += (c == ',') ? ' ' : c;
    std::istringstream ss(f);
    std::string tok;
    while (ss >> tok) {
        size_t dash = tok.find('-');
        if (dash == std::string::npos) { out.push_back(tok); continue; }
        std::string a = tok.substr(0, dash), b = tok.substr(dash + 1);
        size_t k = 0;
        while (k < a.size() && !std::isdigit((unsigned char)a[k])) ++k;
        std::string pre = a.substr(0, k);
        int lo = std::atoi(a.c_str() + k);
        size_t kb = 0;
        while (kb < b.size() && !std::isdigit((unsigned char)b[kb])) ++kb;
        int hi = std::atoi(b.c_str() + kb);
        for (int i = lo; i <= hi; ++i) out.push_back(pre + std::to_string(i));
    }
    return out;
}

static void test_demo() {
    std::string cad = kRoot + "Demonstration CAD Data/";
    std::string altxt = slurp(cad + "Aperture List/Aperture List.txt");
    if (altxt.empty()) { std::puts("[demo] data not found, skipped"); return; }
    std::puts("[demo aperture list]");
    auto al = parse_aperture_list(altxt);
    std::printf("    %s\n", al.explanation.c_str());
    CHECK(al.aps.size() == 82);
    CHECK(near(al.aps.at(17).w, 62 * kMmPerMil) && al.aps.at(18).shape == Aperture::Rect);
    CHECK(al.aps.at(38).comment.find("DONUT") != std::string::npos);

    std::puts("[demo layers]");
    const char* files[] = {"Top Trace Gerber Layer.spl", "Top Mask Gerber Layer.spl", "Gerber Stencil Gerber.spl",
                           "Top Silk Gerber Layer.spl", "Gerber Assembly Drawing.spl"};
    std::map<std::string, Layer> layers;
    for (const char* fn : files) {
        std::string t = slurp(cad + fn);
        CHECK(!t.empty());
        auto cands = guess_format(t, al.aps);
        std::printf("  %s\n    best format: %s (%s); runner-up %s (%s)\n", fn, cands[0].fmt.describe().c_str(),
                    cands[0].note.c_str(), cands[1].fmt.describe().c_str(), cands[1].note.c_str());
        CHECK(cands[0].fmt.inch && cands[0].fmt.lead_supp && cands[0].fmt.frac_digits == 3);
        ParseOptions po;
        po.fmt = cands[0].fmt;
        po.apertures = al.aps;
        Layer L;
        std::string err;
        CHECK(parse(t, po, L, err));
        std::printf("    objects %zu (flash %d, draw %d, region %d), warnings %zu, extent %.3f x %.3f in [%.3f,%.3f]-[%.3f,%.3f]\n",
                    L.objs.size(), L.flash_count(), L.draw_count(), L.region_count(), L.warnings.size(),
                    L.extent.w() / 25.4, L.extent.h() / 25.4, L.extent.x0 / 25.4, L.extent.y0 / 25.4, L.extent.x1 / 25.4, L.extent.y1 / 25.4);
        for (auto& w : L.warnings) std::printf("    warn: %s\n", w.c_str());
        CHECK(L.extent.w() / 25.4 > 7.0 && L.extent.w() / 25.4 < 9.0);
        CHECK(L.extent.h() / 25.4 > 4.0 && L.extent.h() / 25.4 < 5.0);
        layers[fn] = std::move(L);
    }

    // centroid file (mils, origin offset to Gerber = fiducial X3)
    std::vector<Ref> refs;
    {
        std::istringstream ss(slurp(kRoot + "Centroid File/Reference File.txt"));
        std::string ln;
        while (std::getline(ss, ln)) {
            std::istringstream ls(ln);
            Ref r;
            double x, y;
            if (ls >> r.ref >> x >> y >> r.rot) { r.pos = {x * kMmPerMil, y * kMmPerMil}; refs.push_back(r); }
        }
    }
    // BOM: ref -> package type
    std::map<std::string, std::string> pkg_of;
    {
        std::istringstream ss(slurp(kRoot + "Demonstration Bill of Materials/DemoBOM.prn"));
        std::string ln;
        while (std::getline(ss, ln)) {
            if (ln.empty() || ln[0] == ';') continue;
            size_t gap = ln.find("   ");
            if (gap == std::string::npos) continue;
            std::istringstream rest(ln.substr(gap));
            std::string pn, type;
            rest >> pn >> type;
            for (auto& r : expand(ln.substr(0, gap))) {
                std::string u = r;
                for (auto& c : u) c = (char)std::toupper((unsigned char)c);
                pkg_of[u] = type;
            }
        }
    }
    std::printf("  centroid refs %zu, BOM refs %zu\n", refs.size(), pkg_of.size());
    CHECK(refs.size() > 90 && pkg_of.size() > 80);

    // origin: fiducials X1..X3 must coincide with flashes on the mask layer
    const Layer& mask = layers["Top Mask Gerber Layer.spl"];
    Vec2 org{505 * kMmPerMil, 850 * kMmPerMil};
    {
        int ok = 0;
        for (auto& r : refs)
            if (r.ref[0] == 'X' && r.ref.size() == 2 && r.ref[1] <= '3') {
                Vec2 p = r.pos + org;
                for (auto& o : mask.objs) if (o.kind == Object::Flash && dist(o.a, p) < 0.05) { ++ok; break; }
            }
        std::printf("  centroid->Gerber origin (505,850) mil verified on %d/3 fiducials\n", ok);
        CHECK(ok == 3);
    }
    auto ref_pos = [&](const std::string& n) {
        for (auto& r : refs) if (r.ref == n) return r;
        return Ref{};
    };

    // digitize tests on the trace layer
    std::puts("[digitize + auto-scan, Top Trace layer]");
    const Layer& trace = layers["Top Trace Gerber Layer.spl"];
    dz::DigitizeOptions dopt;
    dz::PadSet ps = dz::extract_pads(trace, dopt);
    int nr = 0, nf = 0;
    for (auto& p : ps.pads) { nr += p.raster; nf += p.flash; }
    std::printf("  pad features: %zu (%d flash, %d raster)\n", ps.pads.size(), nf, nr);

    struct Case { const char* ref; double w_mil, h_mil; const char* expect_prefix; };
    const Case cases[] = {
        {"U14", 480, 1300, "DIP24"}, {"U5", 480, 900, "DIP16"}, {"U17", 380, 320, "SOIC8"},
        {"U4", 400, 520, "SOIC16"}, {"C13", 230, 140, "CHIP_"}, {"U6", 1220, 1220, "QFP132_0.64"},
        {"U7", 560, 900, "SOIC32"}, {"U9", 600, 760, "PLCC32"},
    };
    int tot_refs = 0, tot_found = 0, tot_match = 0, tot_good = 0, tot_rot_ok = 0, tot_rot_n = 0;
    std::vector<int> accepted;
    for (const Case& cs : cases) {
        Ref r = ref_pos(cs.ref);
        std::string type = pkg_of[cs.ref];
        Vec2 c = r.pos + org;
        BBox sel;
        sel.add(c - Vec2{cs.w_mil / 2 * kMmPerMil, cs.h_mil / 2 * kMmPerMil});
        sel.add(c + Vec2{cs.w_mil / 2 * kMmPerMil, cs.h_mil / 2 * kMmPerMil});
        dz::DigitizeResult res;
        bool ok = dz::digitize_rect(ps, sel, dopt, res);
        CHECK(ok);
        if (!ok) continue;
        double off = dist(res.centroid, c) / kMmPerMil;
        std::printf("  %-4s (%s): %s  leads %d pitch %.3f mm %s layout %s pin1-%s body %.2fx%.2f sig %s, centroid off %.1f mil\n",
                    cs.ref, type.c_str(), res.name.c_str(), res.leads, res.pitch, res.smt ? "SMT" : "TH", res.layout.c_str(),
                    res.pin1_from_shape ? "shape" : "rule", res.body_w, res.body_h, res.signature.c_str(), off);
        CHECK(res.name.rfind(cs.expect_prefix, 0) == 0);
        CHECK(off < 30);
        dz::ScanOptions so;
        so.exclude = accepted;
        auto matches = dz::auto_scan(ps, res, so);
        // evaluate
        std::set<std::string> same;
        for (auto& [ref, t] : pkg_of) if (t == type) same.insert(ref);
        std::set<std::string> found;
        int good = 0;
        std::string list;
        for (auto& m : matches) {
            std::string hit;
            for (auto& rr : refs) {
                std::string u = rr.ref;
                if (same.count(u) && dist(rr.pos + org, m.centroid) < 40 * kMmPerMil) { hit = u; break; }
            }
            if (!hit.empty()) {
                ++good; found.insert(hit);
                // rotation agreement: instance rot = source rot + match rot (mod 180 if no pin-1 marker)
                double want = std::fmod(ref_pos(hit).rot - r.rot + 720.0, 360.0);
                double got = m.rot, md = res.pin1_from_shape ? 360.0 : 180.0;
                ++tot_rot_n;
                if (std::fabs(std::fmod(want - got + 720.0, md)) < 1e-6) ++tot_rot_ok;
            }
            else {  // report the nearest centroid-file ref for unexplained matches
                double bd = 1e300;
                for (auto& rr : refs) if (dist(rr.pos + org, m.centroid) < bd) { bd = dist(rr.pos + org, m.centroid); hit = "?" + rr.ref + "(" + pkg_of[rr.ref] + ")"; }
            }
            char b[96];
            std::snprintf(b, sizeof b, " %s@%g", hit.c_str(), m.rot);
            list += b;
        }
        int expected = 0;
        for (auto& rr : refs) if (same.count(rr.ref) && rr.ref != cs.ref) ++expected;
        int fnd = 0;
        for (auto& f : found) if (f != cs.ref) ++fnd;
        std::printf("       auto-scan: %zu matches, %d on same-type refs; recall %d/%d;%s\n", matches.size(), good, fnd, expected, list.c_str());
        std::string miss;
        for (auto& rr : refs) if (same.count(rr.ref) && rr.ref != cs.ref && !found.count(rr.ref)) miss += " " + rr.ref;
        if (!miss.empty()) std::printf("       not found:%s\n", miss.c_str());
        tot_refs += expected; tot_found += fnd; tot_match += (int)matches.size(); tot_good += good;
        for (int i : res.pad_indices) accepted.push_back(i);
        for (auto& m : matches) for (int i : m.pads) accepted.push_back(i);
    }
    std::printf("  HIT RATE: recall %d/%d (%.0f%%), precision %d/%d (%.0f%%)\n", tot_found, tot_refs,
                tot_refs ? 100.0 * tot_found / tot_refs : 0, tot_good, tot_match, tot_match ? 100.0 * tot_good / tot_match : 0);
    std::printf("  rotation agreement with centroid file: %d/%d\n", tot_rot_ok, tot_rot_n);
    CHECK(tot_rot_ok == tot_rot_n);
    CHECK(tot_refs == 0 || tot_found * 10 >= tot_refs * 8);
    CHECK(tot_match == 0 || tot_good * 10 >= tot_match * 8);

    // rotation invariance of the signature: digitize the same pattern rotated
    std::puts("[signature rotation invariance]");
    {
        dz::DigitizeResult a;
        Ref r = ref_pos("U17");
        Vec2 c = r.pos + org;
        BBox sel;
        sel.add(c - Vec2{190 * kMmPerMil, 160 * kMmPerMil});
        sel.add(c + Vec2{190 * kMmPerMil, 160 * kMmPerMil});
        CHECK(dz::digitize_rect(ps, sel, dopt, a));
        for (int k = 1; k < 4; ++k) {
            std::vector<Pad> rp = a.pkg.pads;
            for (auto& p : rp) { p.pos = rotate(p.pos, 90.0 * k); if (k & 1) std::swap(p.w, p.h); }
            CHECK(dz::pattern_signature(rp) == a.signature);
        }
    }
}

static void test_digitize_synthetic() {
    std::puts("[digitize synthetic: drill layer, regions, rotation]");
    // 2x3 header (100 mil pitch, square pin 1) + same header rotated 90 deg + a via
    std::string g = "%FSLAX24Y24*%%MOIN*%%ADD10C,0.06*%%ADD11R,0.06X0.06*%%ADD12C,0.02*%\n"
                    "D11*X10000Y10000D03*D10*X11000Y10000D03*X12000Y10000D03*X10000Y11000D03*X11000Y11000D03*X12000Y11000D03*\n"
                    "D12*X10500Y10500D03*\n"
                    "D11*X30000Y10000D03*D10*X30000Y11000D03*X30000Y12000D03*X29000Y10000D03*X29000Y11000D03*X29000Y12000D03*\n"
                    // a rectangular region pad pair (SMT chip) twice
                    "G36*X50000Y10000D02*X50400Y10000D01*X50400Y10500D01*X50000Y10500D01*X50000Y10000D01*G37*\n"
                    "G36*X51000Y10000D02*X51400Y10000D01*X51400Y10500D01*X51000Y10500D01*X51000Y10000D01*G37*\n"
                    "G36*X60000Y20000D02*X60500Y20000D01*X60500Y20400D01*X60000Y20400D01*X60000Y20000D01*G37*\n"
                    "G36*X60000Y21000D02*X60500Y21000D01*X60500Y21400D01*X60000Y21400D01*X60000Y21000D01*G37*M02*";
    Layer L;
    std::string err;
    CHECK(parse(g, ParseOptions{}, L, err));
    std::string drl = "M48\nINCH,TZ\nT1C0.035\n%\nT1\nX1.0Y1.0\nX1.1Y1.0\nX1.2Y1.0\nX1.0Y1.1\nX1.1Y1.1\nX1.2Y1.1\nM30\n";
    Layer D;
    CHECK(parse(drl, ParseOptions{}, D, err));
    dz::DigitizeOptions o;
    o.drill = &D;
    dz::DigitizeResult r;
    BBox sel;
    sel.add({0.9 * 25.4, 0.9 * 25.4});
    sel.add({1.3 * 25.4, 1.2 * 25.4});
    CHECK(dz::digitize_rect(L, sel, o, r));
    std::printf("    %s leads %d pitch %.3f %s pin1-%s dropped %zu\n", r.name.c_str(), r.leads, r.pitch, r.smt ? "SMT" : "TH",
                r.pin1_from_shape ? "shape" : "rule", r.dropped.size());
    CHECK(r.leads == 6 && !r.smt && r.pin1_from_shape && r.dropped.size() == 1 && near(r.pitch, 2.54, 1e-6));
    CHECK(r.pkg.pads[0].shape == PadShape::Square && r.pkg.pads[0].pin == 1 && near(r.pkg.pads[0].drill, 0.889));
    CHECK(near(r.centroid.x, 1.1 * 25.4) && near(r.centroid.y, 1.05 * 25.4));
    auto m = dz::auto_scan(L, r, dz::ScanOptions{});
    CHECK(m.size() == 1 && near(m[0].rot, 90) && near(m[0].centroid.x, 2.95 * 25.4) && near(m[0].centroid.y, 1.1 * 25.4));
    // region chip pads
    dz::DigitizeResult c;
    BBox s2;
    s2.add({4.9 * 25.4, 0.9 * 25.4});
    s2.add({5.2 * 25.4, 1.1 * 25.4});
    CHECK(dz::digitize_rect(L, s2, dz::DigitizeOptions{}, c));
    std::printf("    %s leads %d %s\n", c.name.c_str(), c.leads, c.smt ? "SMT" : "TH");
    CHECK(c.leads == 2 && c.smt && c.name.rfind("CHIP_", 0) == 0);
    auto m2 = dz::auto_scan(L, c, dz::ScanOptions{});
    CHECK(m2.size() == 1 && near(m2[0].rot, 90));
}

// ---------------------------------------------------------------------------
// ViewMate parity probes (tools/vm/gerber_probe.cpp) promoted to tests
// ---------------------------------------------------------------------------
static Layer pr(const std::string& s, ParseOptions po = {}) {
    Layer L;
    std::string err;
    parse(s, po, L, err);
    return L;
}
static void test_viewmate_probes() {
    std::printf("[viewmate parity probes]\n");
    { auto L = pr("%FSLAX24Y24*%%MOIN*%%ADD10C,0.01*%%ABD12*%D10*X0Y0D03*X1000Y0D03*%AB*%D12*X50000Y0D03*X60000Y0D03*M02*");
      CHECK(L.flash_count() == 4 && L.extent.x0 > 100); }
    { auto L = pr("%FSLAX24Y24*%%MOIN*%%ADD10R,0.1X0.02*%%LR90*%D10*X0Y0D03*M02*");
      auto bb = object_bbox(L, L.objs.at(0)); CHECK(bb.h() > bb.w()); }
    { auto L = pr("%FSLAX24Y24*%%MOIN*%%AMOFF*21,1,0.1,0.02,0.2,0,0*%%ADD10OFF*%%LMX*%D10*X0Y0D03*M02*");
      CHECK(L.extent.x1 < 0); }
    { auto L = pr("%FSLAX24Y24*%%MOIN*%%ADD10C,0.1*%%LS0.5*%D10*X0Y0D03*M02*");
      auto bb = object_bbox(L, L.objs.at(0)); CHECK(near(bb.w(), 1.27, 0.01)); }
    { auto L = pr("%FSLAX24Y24*%%MOIN*%%ADD05C,0.01*%D05*X0Y0D03*M02*");
      CHECK(L.flash_count() == 1 && L.objs[0].dcode == 5); }
    { auto L = pr("%FSLAX24Y24*%%MOIN*%%ADD10C,0.01*%D10$X0Y0D02$X10000Y0D01$M02$");
      CHECK(L.draw_count() == 1 && L.objs[0].dcode == 10); }
    { auto L = pr("G54D10\nX0Y0D02\nX10000Y0D01\nM02\n");
      CHECK(L.draw_count() == 1 && L.objs[0].dcode == 10); }
    { auto L = pr("%FSLAX24Y24*%%MOIN*%%IPNEG*%%ADD10C,0.01*%D10*X0Y0D03*M02*");
      CHECK(L.warnings.empty() && L.negative); }
    { auto L = pr("%FSLAX24Y24*%%MOIN*%%IR90*%%ADD10C,0.01*%D10*X10000Y0D03*M02*");
      CHECK(L.objs.size() && near(L.objs[0].a.y, 25.4)); }
    { auto L = pr("%FSLAX24Y24*%%MOIN*%%ASAYBX*%%ADD10C,0.01*%D10*X10000Y0D03*M02*");
      CHECK(L.objs.size() && near(L.objs[0].a.y, 25.4)); }
    { auto L = pr("%FSLAX24Y35*%%MOIN*%%ADD10C,0.01*%D10*X10000Y100000D03*M02*");
      CHECK(L.objs.size() && near(L.objs[0].a.x, 25.4) && near(L.objs[0].a.y, 25.4)); }
    { auto L = pr("%FSTAX24Y24*%%MOIN*%%ADD10C,0.01*%D10*X1Y1D03*M02*");
      CHECK(L.objs.size() && near(L.objs[0].a.x, 254.0)); }
    // Excellon variants
    { auto L = pr("M48\nINCH,TZ\nT1C0.0125\n%\nT1\nX0Y0\nR3X01\nM30\n"); CHECK(L.flash_count() == 4); }
    { auto L = pr(";FILE_FORMAT=2:5\nM48\nINCH,LZ\nT1C0.0125\n%\nT1\nX0100000Y0100000\nM30\n");
      CHECK(L.objs.size() && near(L.objs[0].a.x, 25.4)); }
    { auto L = pr("M48\nINCH\nT1C0.1\n%\nT1\nG00X0Y0\nM15\nG03X1.0Y1.0A1.0\nM16\nM30\n");
      CHECK(L.objs.size() == 1 && L.objs[0].kind == Object::Arc && near(dist(L.objs[0].center, L.objs[0].a), 25.4, 0.01)); }
    { auto L = pr("M48\nINCH\nT1C0.01\n%\nT1\nM97,ABC\nX1.0Y1.0\nM30\n"); CHECK(L.flash_count() == 1); }
    { auto L = pr("M48\nINCH\nT1C0.01\n%\nT1\nM25\nX1.0Y1.0\nM01\nM02X2.0Y0\nM08\nM30\n");
      CHECK(L.flash_count() == 2 && near(L.objs[1].a.x, 76.2)); }
    { auto L = pr("M48\nINCH\nT1C0.01\n%\nG93X1.0Y1.0\nT1\nX1.0Y1.0\nM30\n");
      CHECK(L.objs.size() == 1 && near(L.objs[0].a.x, 50.8)); }
    { auto L = pr("%%3000\nX100Y100T1\nX200Y200\nM30\n"); CHECK(L.flash_count() == 2 && L.excellon); }
}

static void test_layer_guess() {
    struct C { const char* n; int role, side; } cs[] = {
        {"b.GTL", 1, 0}, {"b.GBL", 1, 1}, {"b.GTP", 2, 0}, {"b.GBS", 3, 1}, {"b.GTO", 4, 0}, {"b.GKO", 7, 0}, {"b.GM1", 7, 0},
        {"d.cmp", 1, 0}, {"d.sol", 1, 1}, {"d.crs", 2, 1}, {"p-F_Cu.gbr", 1, 0}, {"p-B_Paste.gbr", 2, 1}, {"p-Edge_Cuts.gbr", 7, 0},
        {"p-In2_Cu.gbr", 1, 2}, {"pastetop.art", 2, 0}, {"pastebot.art", 2, 1}, {"top.art", 1, 0}, {"bot.art", 1, 1}, {"smt.art", 3, 0},
        {"int1.art", 1, 2}, {"x.drl", 6, 0}, {"x.xln", 6, 0}, {"b.G2", 1, 2}, {"Top Trace Gerber Layer.spl", 1, 0}, {"Gerber Stencil Gerber.spl", 2, 0}};
    for (auto& c : cs) { auto g = guess_layer_function(c.n, ""); CHECK(g.role == c.role && g.side == c.side); }
    CHECK(guess_layer_function("drill.txt", "M48\nINCH,LZ\nT1C0.035\n%\nT1\nX1000Y1000\nM30\n").role == 6);
}

// Load Gerber Layers dialog sniffing: every sample must be classified correctly (and CAD / BOM /
// centroid text must NOT be mistaken for RS-274D); every Gerber sample must parse with objects.
static void test_classify_samples() {
    std::puts("[classify samples]");
    CHECK(classify_file("%FSLAX24Y24*%%MOIN*%%ADD10C,0.01*%D10*X0Y0D03*M02*") == FileKind::Rs274x);
    CHECK(classify_file("M48\nINCH,LZ\nT1C0.035\n%\nT1\nX1000Y1000\nM30\n") == FileKind::Excellon);
    CHECK(classify_file("D10*\r\nX100Y100D02*\r\nX200Y100D01*\r\nX300Y300D03*\r\nM02*\r\n" + std::string(300, '\0')) == FileKind::Rs274d);
    CHECK(classify_file("$HEADER\nGENCAD 1.4\n$ENDHEADER\nX100Y100 D01 PAD R1 U1\nX1Y2 D02\nX3Y4 D03\nX5Y5\n") == FileKind::Unknown);
    CHECK(classify_file(std::string("\x89PNG\r\n\x1a\n") + std::string(64, '\0')) == FileKind::Unknown);
    struct S { std::string path; FileKind k; };
    const std::string cad = kRoot + "Demonstration CAD Data/";
    std::vector<S> ss = {
        {cad + "Top Trace Gerber Layer.spl", FileKind::Rs274d}, {cad + "Top Mask Gerber Layer.spl", FileKind::Rs274d},
        {cad + "Gerber Stencil Gerber.spl", FileKind::Rs274d}, {cad + "Top Silk Gerber Layer.spl", FileKind::Rs274d},
        {cad + "Gerber Assembly Drawing.spl", FileKind::Rs274d}, {cad + "Aperture List/Aperture List.txt", FileKind::ApertureList},
        {cad + "CCDemo.cad", FileKind::Unknown}, {kRoot + "Centroid File/Reference File.txt", FileKind::Unknown},
        {pcbp_test::local("cpf_d"), FileKind::Unknown}, {pcbp_test::local("flex_csv"), FileKind::Unknown},
        {pcbp_test::local("trial_bom"), FileKind::Unknown}, {pcbp_test::local("trial_big274x"), FileKind::Rs274x}};
    const std::string al = slurp(cad + "Aperture List/Aperture List.txt");
    for (auto& c : ss) {
        const std::string t = slurp(c.path);
        if (t.empty()) { std::printf("    (missing %s)\n", c.path.c_str()); continue; }
        const FileKind k = classify_file(t);
        std::printf("    %-40s %s\n", c.path.substr(c.path.rfind('/') + 1).c_str(), file_kind_name(k));
        CHECK(k == c.k);
        if (k == FileKind::Rs274d || k == FileKind::Rs274x) {
            ParseOptions po;
            if (k == FileKind::Rs274d) {
                po.apertures = parse_aperture_list(al).aps;
                auto cands = guess_format(t, po.apertures);
                CHECK(!cands.empty());
                if (!cands.empty()) { po.fmt = cands[0].fmt; po.force_format = true; }
            }
            Layer L; std::string err;
            CHECK(parse(t, po, L, err) && L.objs.size() > 100);
        }
    }
}


// KiCad 10 export (user report): inner plane = ONE G36 region with ~23k vertices and cut-in
// holes; unused user layers are header-only files. Must parse fast and empty files are valid.
static void test_kicad10_plane_and_empty() {
    auto rd = [](const char* p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); };
    std::string a = rd("tests/data/gerber/kicad10/MVCU_F407-In1_Cu.g1");
    if (a.empty()) { std::printf("kicad10 sample missing, skipped\n"); return; }
    Layer L; std::string err; ParseOptions o;
    auto t0 = std::chrono::steady_clock::now();
    CHECK(parse(a, o, L, err));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 2000);
    size_t mx = 0; for (auto& ob : L.objs) mx = std::max(mx, ob.poly.size());
    CHECK(mx > 20000);
    CHECK(L.extent.w() > 100 && L.extent.w() < 160);
    auto g = guess_layer_function("MVCU_F407-In1_Cu.g1", a.substr(0, 4096));
    CHECK(g.role == 1 && g.side == 2);
    Layer E; std::string e2;
    CHECK(parse(rd("tests/data/gerber/kicad10/MVCU_F407-Margin.gbr"), o, E, e2));
    CHECK(E.objs.empty() && E.empty_file);
}

// ---------------------------------------------------------------------------
// rotated pads: detection, canonical package, any-angle scan
// ---------------------------------------------------------------------------
struct QfpCopy { double x, y, ang; int missing; double pitch = 0.5; };
static std::string qfp_gerber(const std::vector<QfpCopy>& copies, int per_side, double row) {
    std::string g = "%FSLAX46Y46*%\n%MOMM*%\n%AMRR*\n21,1,$1,$2,0,0,$3*%\n";
    std::map<long long, int> dc;   // angle(0.001) -> dcode
    std::string body;
    int next = 10;
    auto ap = [&](double ang) {
        long long k = std::llround(std::fmod(ang + 3600.0, 360.0) * 1000);
        auto it = dc.find(k);
        if (it != dc.end()) return it->second;
        char b[96];
        std::snprintf(b, sizeof b, "%%ADD%dRR,1.500000X0.300000X%.4f*%%\n", next, k / 1000.0);
        g += b;
        return dc[k] = next++;
    };
    for (auto& c : copies) {
        int idx = 0;
        for (int side = 0; side < 4; ++side)
            for (int i = 0; i < per_side; ++i, ++idx) {
                if (idx == c.missing) continue;
                double t = (i - (per_side - 1) / 2.0) * c.pitch;
                Vec2 p = side == 0 ? Vec2{-row, -t} : side == 1 ? Vec2{t, -row} : side == 2 ? Vec2{row, t} : Vec2{-t, row};
                double pa = (side & 1) ? 90 : 0;
                Vec2 q = rotate(p, c.ang);
                char b[96];
                std::snprintf(b, sizeof b, "D%d*\nX%lldY%lldD03*\n", ap(c.ang + pa), std::llround((c.x + q.x) * 1e6), std::llround((c.y + q.y) * 1e6));
                body += b;
            }
    }
    return g + body + "M02*\n";
}

static void test_rotated_pads() {
    // min-area rectangle
    {
        std::vector<Vec2> r;
        for (Vec2 v : {Vec2{-1, -0.25}, Vec2{1, -0.25}, Vec2{1, 0.25}, Vec2{-1, 0.25}}) r.push_back(rotate(v, 37.0));
        auto ob = dz::min_area_rect(r);
        CHECK(near(ob.w, 2.0, 1e-6) && near(ob.h, 0.5, 1e-6) && near(ob.ang, 37.0, 1e-3));
    }
    // synthetic QFP32 (0.5 pitch): source at 30 deg + copies at 0, 90, 30, 45, 17.3, 200 deg,
    // a 45 deg decoy with one pad missing and a 45 deg decoy with a different pitch
    std::vector<QfpCopy> cp{{20, 20, 30, -1}, {40, 20, 0, -1}, {60, 20, 90, -1}, {20, 40, 30, -1}, {40, 40, 45, -1},
                            {60, 40, 17.3, -1}, {20, 60, 200, -1}, {40, 60, 45, 5}, {60, 60, 45, -1, 0.65}};
    std::string g = qfp_gerber(cp, 8, 3.0);
    Layer L = px(g);
    auto ps = dz::extract_pads(L, {});
    CHECK(ps.pads.size() == 8 * 32 - 1 + 32);
    int oriented = 0;
    for (auto& f : ps.pads) oriented += f.oriented;
    CHECK(oriented == (int)ps.pads.size());
    // pick the source group
    std::vector<int> src;
    for (int i = 0; i < (int)ps.pads.size(); ++i) if (dist(ps.pads[i].c, {20, 20}) < 5) src.push_back(i);
    CHECK(src.size() == 32);
    auto rg = dz::estimate_rotation(ps, src);
    std::printf("  synthetic QFP32 @30: detected %.1f (%s)\n", rg.angle, rg.why.c_str());
    CHECK(near(rg.angle, 30.0, 0.05));
    CHECK(near(rg.pad_angle, 30.0, 0.05) && near(rg.row_angle, 30.0, 0.05));
    dz::DigitizeResult r;
    CHECK(dz::digitize_rotated(ps, src, rg.angle, {}, r));
    CHECK(r.layout == "quad" && r.pkg.pads.size() == 32 && near(r.pitch, 0.5, 0.01) && near(r.rot, 30.0, 1e-9));
    CHECK(near(r.centroid.x, 20, 1e-4) && near(r.centroid.y, 20, 1e-4));
    bool sizes = true;
    for (auto& p : r.pkg.pads) sizes &= near(std::max(p.w, p.h), 1.5, 1e-3) && near(std::min(p.w, p.h), 0.3, 1e-3) && p.rot == 0;
    CHECK(sizes);
    CHECK(r.name == "QFP32_0.5");
    // rotation-invariant signature equal for copies
    {
        std::vector<Pad> rp = r.pkg.pads;
        for (auto& p : rp) { p.pos = rotate(p.pos, 63.0); }
        CHECK(dz::rot_invariant_signature(rp) == dz::rot_invariant_signature(r.pkg.pads));
    }
    // default scan: only axis-aligned placements of the canonical package (world 0 and 90)
    dz::ScanOptions so;
    auto m0 = dz::auto_scan(ps, r, so);
    int got0 = 0, fp0 = 0;
    for (auto& m : m0) {
        bool ok = (dist(m.centroid, {40, 20}) < 0.05 || dist(m.centroid, {60, 20}) < 0.05) && !m.rotated;
        (ok ? got0 : fp0)++;
    }
    CHECK(got0 == 2 && fp0 == 0);
    // any angle: + the 30 / 45 / 17.3 / 200 deg copies; decoys rejected
    so.any_angle = true;
    auto m1 = dz::auto_scan(ps, r, so);
    struct Exp { Vec2 c; double a; } exp[] = {{{40, 20}, 0}, {{60, 20}, 90}, {{20, 40}, 30}, {{40, 40}, 45}, {{60, 40}, 17.3}, {{20, 60}, 200}};
    int tp = 0, fp = 0, fn = 0, rot_ok = 0, review = 0;
    for (auto& m : m1) {
        bool hit = false;
        for (auto& e : exp)
            if (dist(m.centroid, e.c) < 0.05) {
                hit = true;
                // instance angle = r.rot + m.rot (QFP symmetric: modulo 90)
                double d = std::remainder(m.rot - e.a, 90.0);   // canonical pattern: instance angle = m.rot (mod 90, symmetric)
                rot_ok += std::fabs(d) < 0.05;
            }
        (hit ? tp : fp)++;
        review += m.rotated;
        CHECK(!m.rotated || m.confidence > 0.9);
    }
    fn = 6 - tp;
    std::printf("  synthetic QFP32 any-angle scan: TP %d  FP %d  FN %d  angle-correct %d  for review %d\n", tp, fp, fn, rot_ok, review);
    CHECK(tp == 6 && fp == 0 && fn == 0 && rot_ok == 6 && review == 4);

    // KiCad 10 set: the 45 deg LQFP-100 (0.5 pitch, RoundRect macro pads) on F_Paste and F_Cu
    for (const char* fn2 : {"tests/data/gerber/kicad10/MVCU_F407-F_Paste.gtp", "tests/data/gerber/kicad10/MVCU_F407-F_Cu.gtl"}) {
        std::string t = slurp(fn2);
        if (t.empty()) { std::printf("  (skip %s)\n", fn2); continue; }
        Layer K;
        std::string err;
        CHECK(parse(t, {}, K, err));
        auto kp = dz::extract_pads(K, {});
        // the QFP pads: 1.6 x 0.3 at 45 / 135 deg
        std::vector<int> q;
        for (int i = 0; i < (int)kp.pads.size(); ++i) {
            auto& f = kp.pads[i];
            if (f.oriented && near(f.ow, 1.6, 0.01) && near(f.oh, 0.3, 0.01)) q.push_back(i);
        }
        CHECK(q.size() == 100);
        BBox qb;
        for (int i : q) qb.add(kp.pads[i].c);
        // the user's box: everything whose pad lies inside the QFP's footprint box
        std::vector<int> sel;
        BBox sb = qb;
        sb.inflate(1.0);
        dz::DigitizeResult r0;
        CHECK(dz::digitize_rect(kp, sb, {}, r0));
        std::printf("  %s: axis-aligned box around the QFP holds %zu pad features\n", fn2 + 26, r0.pad_indices.size());
        // the box the user draws in a view rotated by 45 deg (Align View to Line): a rotated rectangle
        {
            const Vec2 qc = qb.center();
            double hx = 0, hy = 0;
            for (int i : q) { Vec2 l = rotate(kp.pads[i].c - qc, -45.0); hx = std::max(hx, std::fabs(l.x)); hy = std::max(hy, std::fabs(l.y)); }
            std::vector<int> in;
            for (int i = 0; i < (int)kp.pads.size(); ++i) {
                Vec2 l = rotate(kp.pads[i].c - qc, -45.0);
                if (std::fabs(l.x) <= hx + 1.0 && std::fabs(l.y) <= hy + 1.0) in.push_back(i);
            }
            r0.pad_indices = in;
        }
        auto g2 = dz::estimate_rotation(kp, r0.pad_indices);
        dz::DigitizeResult kr;
        CHECK(dz::digitize_rotated(kp, r0.pad_indices, g2.angle, {}, kr));
        std::printf("  %s: box %zu pads, detected %.1f deg (%s), conf %.2f -> %s %s pitch %.3f\n", fn2 + 26, r0.pad_indices.size(), g2.angle, g2.why.c_str(),
                    g2.confidence, kr.name.c_str(), kr.layout.c_str(), kr.pitch);
        CHECK(near(std::fabs(g2.angle), 45.0, 0.05));
        CHECK(kr.layout == "quad" && kr.pkg.pads.size() == 100 && kr.name == "QFP100_0.5");
        bool ks = true;
        for (auto& p : kr.pkg.pads) ks &= near(std::max(p.w, p.h), 1.6, 0.01) && near(std::min(p.w, p.h), 0.3, 0.01) && p.rot == 0;
        CHECK(ks);
        // scan the whole layer for the canonical package incl. the source: exactly one rotated hit
        dz::ScanOptions ks2;
        ks2.skip_source = false;
        ks2.any_angle = true;
        auto km = dz::auto_scan(kp, kr, ks2);
        int ktp = 0, kfp = 0;
        for (auto& m : km) (dist(m.centroid, kr.centroid) < 0.05 ? ktp : kfp)++;
        std::printf("  %s LQFP-100 @45 any-angle scan: TP %d  FP %d  FN %d\n", fn2 + 26, ktp, kfp, 1 - ktp);
        CHECK(ktp == 1 && kfp == 0);
        // the 2-pad 45 deg chips (1.45 x 1.175 RoundRect): detection on each pair
        int pairs = 0, ok45 = 0;
        std::vector<char> seen(kp.pads.size(), 0);
        for (int i = 0; i < (int)kp.pads.size(); ++i) {
            auto& f = kp.pads[i];
            if (seen[i] || !f.oriented || !near(f.ow, 1.45, 0.01) || !near(f.oh, 1.175, 0.01) || std::fabs(std::remainder(f.oang, 90.0)) < 1) continue;
            int best = -1;
            double bd = 3.0;
            for (int j = 0; j < (int)kp.pads.size(); ++j)
                if (j != i && !seen[j] && kp.pads[j].dcode == f.dcode && dist(kp.pads[j].c, f.c) < bd) { bd = dist(kp.pads[j].c, f.c); best = j; }
            if (best < 0) continue;
            seen[i] = seen[best] = 1;
            ++pairs;
            auto gg = dz::estimate_rotation(kp, {i, best});
            ok45 += near(std::fabs(gg.angle), 45.0, 0.05);
        }
        std::printf("  %s 45 deg chip pairs: %d, angle detected 45: %d\n", fn2 + 26, pairs, ok45);
        CHECK(pairs > 0 && ok45 == pairs);
    }
}

// ---------------------------------------------------------------------------
// R10 §7: component orientation from the selected pads (C53 vertical / C61 horizontal)
// ---------------------------------------------------------------------------
static std::vector<Pad> inst_of(const std::vector<Pad>& pkg, double R, bool mirror = false) {
    std::vector<Pad> o = pkg;
    for (Pad& p : o) {
        if (mirror) { p.pos.x = -p.pos.x; p.rot = -p.rot; }
        p.pos = rotate(p.pos, R);
        p.rot += R;
    }
    return o;
}
static Pad mkpad(double x, double y, double w, double h) { Pad p; p.pos = {x, y}; p.w = w; p.h = h; return p; }
static bool rot_eq(double a, double b, double period = 360, double t = 0.11) { return std::fabs(std::remainder(a - b, period)) <= t; }

static void test_orientation_fit() {
    std::puts("[R10 orientation: chip horizontal/vertical, SOT-23 x4, package reuse]");
    // two-pad chip, canonical horizontal (pads 1.0 x 1.3, centres +-0.95)
    const std::vector<Pad> chipH{mkpad(-0.95, 0, 1.0, 1.3), mkpad(0.95, 0, 1.0, 1.3)};
    const std::vector<Pad> chipV{mkpad(0, 0.95, 1.3, 1.0), mkpad(0, -0.95, 1.3, 1.0)};   // same part drawn vertically
    dz::OrientFit f = dz::fit_orientation(chipH, chipH);
    CHECK(f.ok && rot_eq(f.rot, 0));                       // prefers 0 over the symmetric 180
    f = dz::fit_orientation(chipH, chipV);
    CHECK(f.ok && rot_eq(f.rot, 90, 180));
    // the bug: a package first built from vertical C53 pads, reused for horizontal C61 pads
    f = dz::fit_orientation(chipV, chipH);
    CHECK(f.ok && rot_eq(f.rot, 90, 180));                 // never the previous placement's 0
    f = dz::fit_orientation(chipV, chipV);
    CHECK(f.ok && rot_eq(f.rot, 0, 180));
    // wrong pad size: no fit (a different package must be created)
    f = dz::fit_orientation(chipH, {mkpad(-1.5, 0, 1.6, 2.0), mkpad(1.5, 0, 1.6, 2.0)});
    CHECK(!f.ok);
    // SOT-23 (pin 1 bottom-left, pin 3 top centre) at 0/90/180/270, top and bottom side
    const std::vector<Pad> sot{mkpad(-0.95, -1.1, 0.6, 0.7), mkpad(0.95, -1.1, 0.6, 0.7), mkpad(0, 1.1, 0.6, 0.7)};
    for (double R : {0.0, 90.0, 180.0, 270.0}) {
        f = dz::fit_orientation(sot, inst_of(sot, R));
        CHECK(f.ok && f.axis && rot_eq(f.rot, R) && f.rms < 1e-6);
        CHECK(f.ok && f.match[2] == 2);                    // pin 3 lands on pin 3
        f = dz::fit_orientation(sot, inst_of(sot, R, true), true);
        CHECK(f.ok && rot_eq(f.rot, R));
    }
    // any angle (30 deg) + offset of the package origin
    {
        std::vector<Pad> in = inst_of(sot, 30);
        for (Pad& p : in) p.pos = p.pos + Vec2{5, -3};
        f = dz::fit_orientation(sot, in);
        CHECK(f.ok && !f.axis && rot_eq(f.rot, 30, 360, 0.15));
        CHECK(!dz::fit_orientation(sot, in, false, 0.15, false).ok);
    }
    // canonicalize: a vertical pair becomes a horizontal package (pin 1 left), instance unchanged
    {
        std::string g = "%FSLAX24Y24*%%MOMM*%%ADD10R,1.3X1.0*%%ADD11R,1.0X1.3*%\n"
                        "D10*X100000Y109500D03*X100000Y90500D03*\n"      // C53 vertical at (10,10)
                        "D11*X290500Y100000D03*X309500Y100000D03*M02*";  // C61 horizontal at (30,10)
        Layer L;
        std::string err;
        CHECK(parse(g, ParseOptions{}, L, err));
        dz::PadSet ps = dz::extract_pads(L, dz::DigitizeOptions{});
        dz::DigitizeResult r53, r61;
        CHECK(dz::digitize_rect(ps, BBox(8, 7, 12, 13), dz::DigitizeOptions{}, r53));
        CHECK(dz::digitize_rect(ps, BBox(28, 8, 32, 12), dz::DigitizeOptions{}, r61));
        std::vector<Pad> i53 = inst_of(r53.pkg.pads, r53.rot), i61 = inst_of(r61.pkg.pads, r61.rot);
        CHECK(dz::canonicalize_orientation(r53));
        CHECK(!dz::canonicalize_orientation(r61));
        CHECK(r53.pkg.pads[0].pos.x < r53.pkg.pads[1].pos.x && std::fabs(r53.pkg.pads[0].pos.y) < 1e-9);
        CHECK(rot_eq(r53.rot, 270) && rot_eq(r61.rot, 0));
        const std::vector<Pad> c53 = inst_of(r53.pkg.pads, r53.rot);
        for (size_t k = 0; k < 2; ++k) CHECK(near(c53[k].pos.x, i53[k].pos.x) && near(c53[k].pos.y, i53[k].pos.y));
        // reuse C53's package for C61 (same name / pad count): orientation from C61's pads
        f = dz::fit_orientation(r53.pkg.pads, i61);
        CHECK(f.ok && rot_eq(f.rot, 0, 180));
        f = dz::fit_orientation(r53.pkg.pads, i53, false, 0.15, true, r53.rot);
        CHECK(f.ok && rot_eq(f.rot, 270));
        std::printf("    C53 %s rot %.0f, C61 %s rot %.0f\n", r53.name.c_str(), r53.rot, r61.name.c_str(), r61.rot);
        CHECK(r53.name == r61.name);
    }
}

// ---------------------------------------------------------------------------
// R10 §8: RS-274X / Excellon export, round-trip by reparsing
// ---------------------------------------------------------------------------
static bool same_layer(const Layer& A, const Layer& B, double t = 2e-3) {
    if (A.objs.size() != B.objs.size()) { std::printf("    objs %zu vs %zu\n", A.objs.size(), B.objs.size()); return false; }
    for (size_t i = 0; i < A.objs.size(); ++i) {
        const Object &a = A.objs[i], &b = B.objs[i];
        if (a.kind != b.kind || a.clear != b.clear || dist(a.a, b.a) > t) { std::printf("    obj %zu differs\n", i); return false; }
        if (a.kind == Object::Draw || a.kind == Object::Arc) if (dist(a.b, b.b) > t) return false;
        if (a.kind == Object::Arc && (dist(a.center, b.center) > t || a.cw != b.cw)) return false;
        if (a.kind == Object::Region && a.poly.size() + 1 < b.poly.size()) return false;
        if (a.kind != Object::Region) {
            const Aperture *pa = find_ap(A, a.dcode), *pb = find_ap(B, b.dcode);
            if (!pa || !pb || pa->shape != pb->shape || std::fabs(pa->w - pb->w) > t || std::fabs(pa->h - pb->h) > t || std::fabs(pa->hole - pb->hole) > t) {
                std::printf("    aperture of obj %zu differs\n", i); return false;
            }
        }
    }
    BBox ea = A.extent, eb = B.extent;
    return std::fabs(ea.x0 - eb.x0) < 0.01 && std::fabs(ea.y1 - eb.y1) < 0.01 && std::fabs(ea.w() - eb.w()) < 0.01 && std::fabs(ea.h() - eb.h()) < 0.01;
}

static void test_export_roundtrip() {
    std::puts("[R10 export: RS-274X + Excellon round trip]");
    Layer L = px("%FSLAX24Y24*%%MOIN*%\n%ADD10C,0.0100*%%ADD11R,0.0600X0.0400X0.02*%%ADD12O,0.05X0.1*%%ADD13P,0.1X6X30*%\n"
                 "%AMTHERM*4,1,4,0,0,0.05,0,0.05,0.05,0,0.05,0,0,0*%%ADD14THERM*%\n"
                 "G01*D10*X0Y0D02*X10000Y0D01*G75*G03X20000Y10000I0J10000D01*D11*X20000Y5000D03*D12*X30000D03*D13*X40000D03*D14*X50000Y0D03*\n"
                 "%LPC*%D10*X0Y-1000D02*X5000Y-1000D01*%LPD*%G36*X60000Y0D02*X61000Y0D01*X61000Y1000D01*X60000Y1000D01*X60000Y0D01*G37*M02*");
    std::string out = write_rs274x(L, nullptr, "test*export");
    Layer R;
    std::string err;
    CHECK(parse(out, ParseOptions{}, R, err));
    CHECK(R.rs274x && !R.fmt.inch);
    CHECK(same_layer(L, R));
    // keep-mask: drop the flashes
    std::vector<char> keep(L.objs.size(), 1);
    int dropped = 0;
    for (size_t i = 0; i < L.objs.size(); ++i) if (L.objs[i].kind == Object::Flash) { keep[i] = 0; ++dropped; }
    Layer R2;
    CHECK(parse(write_rs274x(L, &keep), ParseOptions{}, R2, err));
    CHECK((int)R2.objs.size() == (int)L.objs.size() - dropped && R2.flash_count() == 0);
    // Excellon with a slot
    Layer D;
    CHECK(parse("M48\nINCH,TZ\nT1C0.035\nT2C0.040\n%\nT1\nX1.0Y1.0\nX1.1Y1.0\nT2\nX2.0Y2.0G85X2.2Y2.0\nM30\n", ParseOptions{}, D, err));
    std::string xo = write_excellon(D);
    Layer DR;
    CHECK(parse(xo, ParseOptions{}, DR, err));
    CHECK(DR.excellon && same_layer(D, DR));
    // real data (local only): KiCad set + legacy demo .spl
    int files = 0, ok = 0;
    std::vector<std::string> paths;
    for (const char* n : {"F_Cu.gtl", "B_Cu.gbl", "F_Paste.gtp", "F_Mask.gts", "F_Silkscreen.gto", "Edge_Cuts.gm1", "In1_Cu.g1", "F_Fab.gbr"})
        paths.push_back(pcbp_test::local("kicad_mvcu") + "-" + n);
    paths.push_back(pcbp_test::local("kicad_mvcu") + ".drl");
    for (const char* n : {"Top Trace Gerber Layer.spl", "Top Mask Gerber Layer.spl", "Top Silk Gerber Layer.spl", "Gerber Stencil Gerber.spl", "Gerber Assembly Drawing.spl"})
        paths.push_back(pcbp_test::local("legacy_demo_spl") + n);
    for (auto& p : paths) {
        std::string txt = slurp(p);
        if (txt.empty()) continue;
        Layer A;
        if (!parse(txt, ParseOptions{}, A, err) || A.objs.empty()) continue;
        ++files;
        Layer B;
        const std::string w = A.excellon ? write_excellon(A) : write_rs274x(A);
        const bool good = parse(w, ParseOptions{}, B, err) && B.warnings.empty() && same_layer(A, B);
        if (good) ++ok; else std::printf("    round trip failed: %s (%zu warnings)\n", p.c_str(), B.warnings.size());
    }
    std::printf("    real files round-tripped: %d / %d\n", ok, files);
    CHECK(ok == files);
}

int main() {
    test_kicad10_plane_and_empty();
    test_classify_samples();
    test_layer_guess();
    test_basic();
    test_macro();
    test_regions_arcs();
    test_sr_image();
    test_excellon();
    test_aperture_list_text();
    test_digitize_synthetic();
    test_rotated_pads();
    test_demo();
    test_viewmate_probes();
    test_orientation_fit();
    test_export_roundtrip();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
