// ============================================================================
// test_bom_review.cpp — grouped BOM review + plain-text / CSV report (R14d).
// Synthetic only; PCBP_REVIEW_BOM + PCBP_REVIEW_PROJECT (local customer files,
// never committed) additionally print the review of a real BOM vs a real board.
// ============================================================================
#include "../src/core/project_io.h"
#include "../src/io/bom_review.h"
#include "../src/io/io_util.h"
#include "../src/io/m1_bom.h"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace pcbp;
using namespace pcbp::io;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool has(const std::string& s, const std::string& f) { return s.find(f) != std::string::npos; }

static void preview_m1(const BomSource& src, BomOptions2& opt, BomPreview& p) {
    std::string err;
    bom_preview_src(src, opt, p, err);
    std::vector<FormatTemplate> v;
    ensure_builtin_bom_templates(v);
    int i = match_template(v, p.text, Purpose::Bom);
    if (i < 0) return;
    opt.layout = template_layout(v[i]);
    p = BomPreview{};
    bom_preview_src(src, opt, p, err);
    apply_template_roles(v[i], p.text, opt.layout);
    bom_rebuild(p, opt);
}

static void synthetic() {
    std::printf("== synthetic review\n");
    // line 3 C1~C3 qty 2 (mismatch); C3 also on line 4 with another part (conflict); R9 not on board; J1 on board not in BOM
    std::vector<std::vector<std::string>> rows = {
        {"J-1", "0", "1", "", "W", "B1", "BRD-1", "PCB", "PCB", "", "1", "EA", "", "", "t", ""},
        {"J-1", "0", "10", "", "W", "B2", "CAP-A", "SMD", "CAP A, 0402", "C1~C3", "2", "EA", "MA", "MK", "t", ""},
        {"J-1", "0", "20", "", "W", "B3", "CAP-B", "SMD", "CAP B", "C3 C4", "2", "EA", "MB", "MK", "t", ""},
        {"J-1", "0", "30", "", "W", "B4", "RES-A", "SMD", "RES \"A\"", "R1 R9", "2", "EA", "MR", "MK", "t", ""}};
    BomSource src;
    std::string err;
    CHECK(bom_source_from_bytes(m1_rows_to_tsv(m1_bom_headers(), rows), "syn.tsv", src, err));
    BomOptions2 opt;
    BomPreview p;
    preview_m1(src, opt, p);
    std::set<std::string> board{"C1", "C2", "C3", "C4", "R1", "J1", "U7"};
    BomReview r = bom_review(p, board, "syn.xlsx", "J-1", "2026-11-10 08:00");
    CHECK(r.count(ReviewCat::PnConflict) == 1);
    CHECK(r.count(ReviewCat::QtyMismatch) == 1);
    CHECK(r.count(ReviewCat::NotOnBoard) == 1 && r.group(ReviewCat::NotOnBoard)->items[0].ref == "R9");
    CHECK(r.count(ReviewCat::Unmatched) == 2);
    CHECK(r.count(ReviewCat::Info) >= 1);
    CHECK(r.bom_refs == 6 && r.matched == 5 && r.board_placements == 7);
    CHECK(r.flags_of("C3") & (1u << (int)ReviewCat::PnConflict));
    CHECK(r.flags_of("C2") & (1u << (int)ReviewCat::QtyMismatch));
    CHECK(r.flags_of("R9") & (1u << (int)ReviewCat::NotOnBoard));
    CHECK(r.flags_of("J1") & (1u << (int)ReviewCat::Unmatched));
    CHECK(r.flags_of("C4") == 0);
    std::string t = bom_review_text(r);
    CHECK(has(t, "Source:         syn.xlsx") && has(t, "Job / BOM no.:  J-1") && has(t, "Date:           2026-11-10 08:00"));
    CHECK(has(t, "Findings:       1 error(s), 4 warning(s)"));
    CHECK(has(t, "Error: Part assignment conflicts"));
    CHECK(has(t, "C3: CAP-A (line 3) vs CAP-B (line 4)"));
    CHECK(has(t, "line 3: Quantity says 2 but 3 references are listed (C1~C3)"));
    CHECK(has(t, "Warning: BOM references not on the board (1)\n  R9\n"));
    CHECK(has(t, "Warning: Board placements not in the BOM (2)\n  J1, U7\n"));
    CHECK(has(t, "Bare board line"));
    std::string c = bom_review_csv(r);
    CHECK(c.rfind("severity,category,reference,line,detail\r\n", 0) == 0);
    CHECK(has(c, "Error,pn_conflict,C3,,C3: CAP-A (line 3) vs CAP-B (line 4)\r\n"));
    CHECK(has(c, "Warning,unmatched_placement,U7,,U7\r\n"));
    CHECK(has(c, "Warning,qty_mismatch,,3,line 3: Quantity says 2 but 3 references are listed (C1~C3). Check the BOM line.\r\n"));
    // quoting
    BomReview q;
    q.groups.push_back({ReviewCat::OtherWarning, {{"X1", 2, "a, \"b\""}}});
    CHECK(has(bom_review_csv(q), "Warning,warning,X1,2,\"a, \"\"b\"\"\"\r\n"));
    // empty
    BomReview e = bom_review(BomPreview{}, {}, "none");
    CHECK(has(bom_review_text(e), "No findings."));
    // nothing truncated: 300 unmatched placements all listed
    std::set<std::string> big;
    for (int i = 1; i <= 300; ++i) big.insert("TP" + std::to_string(i));
    BomReview b = bom_review(p, big, "x");
    std::string bt = bom_review_text(b);
    CHECK(b.count(ReviewCat::Unmatched) == 300 && has(bt, "TP1,") && has(bt, "TP300\n"));
}

static void real() {
    const char* bp = std::getenv("PCBP_REVIEW_BOM");
    const char* pp = std::getenv("PCBP_REVIEW_PROJECT");
    if (!bp || !pp) { std::printf("== real: skipped (PCBP_REVIEW_BOM / PCBP_REVIEW_PROJECT not set)\n"); return; }
    Project prj;
    std::string err;
    CHECK(load_project(pp, prj, err));
    std::set<std::string> board;
    for (auto& c : prj.comps) if (c.feature == Feature::Component) board.insert(to_upper(c.ref));
    BomSource src;
    CHECK(bom_load_source(bp, src, err));
    BomOptions2 opt;
    BomPreview p;
    preview_m1(src, opt, p);
    BomReview r = bom_review(p, board, "real BOM", m1_job_id(p.text));
    std::printf("%s", bom_review_text(r).c_str());
}

int main() {
    synthetic();
    real();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
