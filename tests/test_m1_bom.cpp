// ============================================================================
// test_m1_bom.cpp — built-in "M1 BOM" header profile (ERP job-material export):
// detection, mapping, bare-board line, '~' ranges, qty-vs-refs warning, TH
// process, live-fetch TSV path. Synthetic fixture only (tests/data/m1_bom_synthetic.xlsx);
// the real customer sample is used only when PCBP_M1_SAMPLE points at it (never committed).
// ============================================================================
#include "../src/io/bom_import2.h"
#include "../src/io/m1_bom.h"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace pcbp;
using namespace pcbp::io;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// Same steps as the wizard (BomFlow::init + auto_match_template).
static bool wizard_preview(const BomSource& src, BomOptions2& opt, BomPreview& p, std::string& tpl_name) {
    std::string err;
    if (!bom_preview_src(src, opt, p, err)) { std::printf("preview: %s\n", err.c_str()); return false; }
    std::vector<FormatTemplate> v;
    ensure_builtin_bom_templates(v);
    double score = 0;
    int i = match_template(v, p.text, Purpose::Bom, 0.75, &score);
    if (i < 0) return true;
    tpl_name = v[i].name;
    opt.layout = template_layout(v[i]);
    p = BomPreview{};
    if (!bom_preview_src(src, opt, p, err)) return false;
    apply_template_roles(v[i], p.text, opt.layout);
    bom_rebuild(p, opt);
    return true;
}

static const BomLine* line_with(const BomPreview& p, const std::string& pn) {
    for (auto& b : p.lines) if (b.pn == pn) return &b;
    return nullptr;
}
static bool has_issue(const BomPreview& p, const std::string& frag, IssueSev sev) {
    for (auto& i : p.issues) if (i.sev == sev && i.msg.find(frag) != std::string::npos) return true;
    return false;
}

static void synthetic_xlsx() {
    std::printf("== synthetic M1 xlsx\n");
    BomSource src;
    std::string err, tpl;
    CHECK(bom_load_source("tests/data/m1_bom_synthetic.xlsx", src, err));
    BomOptions2 opt;
    BomPreview p;
    CHECK(wizard_preview(src, opt, p, tpl));
    CHECK(tpl == kM1BomTemplateName);
    CHECK(is_m1_bom(p.text));
    CHECK(m1_job_id(p.text) == "900001-01-01");
    CHECK(p.text.col(Role::PartNumber) == 6 && p.text.col(Role::Ref) == 9 && p.text.col(Role::Qty) == 10);
    CHECK(p.text.col(Role::Description) == 8 && p.text.col(Role::Process) == 7 && p.text.col(Role::MPN) == 12);
    CHECK(p.text.col(Role::Manufacturer) == 13 && p.text.col(Role::LineItem) == 2 && p.text.col(Role::Bin) == 5);
    CHECK(p.mapping_ok());
    // bare board: skipped, Info only
    CHECK(line_with(p, "BRD-0001") == nullptr);
    CHECK(p.board_pn == "BRD-0001");
    CHECK(has_issue(p, "Bare board line", IssueSev::Info));
    for (auto& i : p.issues) if (i.sev == IssueSev::Error) std::printf("  unexpected error: %s\n", i.msg.c_str());
    for (auto& i : p.issues) CHECK(i.sev != IssueSev::Error);
    // hyphenated singles + '~' ranges
    const BomLine* b = line_with(p, "CON-0010");
    CHECK(b && b->refs.size() == 2 && b->refs[0] == "B-J2" && b->refs[1] == "B-P10" && !b->qty_mismatch);
    b = line_with(p, "CAP-0030");
    CHECK(b && b->refs.size() == 39 && b->qty == 38 && b->qty_mismatch);
    CHECK(has_issue(p, "Quantity says 38 but 39", IssueSev::Warning));
    CHECK(p.qty_mismatches == 1);
    b = line_with(p, "CAP-0060");
    CHECK(b && b->refs.size() == 10 && !b->qty_mismatch && b->item == "60");
    // apply: TH class -> process flag
    Project prj;
    for (const char* r : {"J5", "J6", "C1", "B-J2"}) { Component c; c.ref = r; c.feature = Feature::Component; prj.add_component(c); }
    BomReport rep;
    CHECK(bom_apply(p, prj, opt, rep, err));
    CHECK(prj.parts.count("CON-0070") && prj.parts["CON-0070"].pop_type == "TH");
    CHECK(prj.parts["CAP-0030"].manf_pn == "CC0402-104" && prj.parts["CAP-0030"].manufacturer == "CAPCO" && prj.parts["CAP-0030"].bin == "A04");
    CHECK(!prj.parts.count("BRD-0001"));
    if (Component* c = prj.find_ref("J5")) CHECK(prj.process_of(*c) == Process::TH); else CHECK(false);
    if (Component* c = prj.find_ref("C1")) CHECK(prj.process_of(*c) == Process::SMT && c->number == "CAP-0030"); else CHECK(false);
}

static void live_tsv() {
    std::printf("== live-fetch TSV path\n");
    std::vector<std::vector<std::string>> rows = {
        {"900002-01-01", "0", "1", "", "W1", "A01", "BRD-0002", "PCB", "PCB, TEST", "", "1", "EA", "", "", "t", "2026-01-01"},
        {"900002-01-01", "0", "10", "", "W1", "A02", "RES-1", "SMD", "RES\tTEST", "R1~R3", "3", "EA", "M-1", "MK", "t", "2026-01-01"}};
    BomSource src;
    std::string err, tpl;
    CHECK(bom_source_from_bytes(m1_rows_to_tsv(m1_bom_headers(), rows), "M1 job 900002-01-01.tsv", src, err));
    BomOptions2 opt;
    BomPreview p;
    CHECK(wizard_preview(src, opt, p, tpl));
    CHECK(tpl == kM1BomTemplateName);
    CHECK(m1_job_id(p.text) == "900002-01-01");
    CHECK(p.lines.size() == 1 && p.lines[0].refs.size() == 3 && p.lines[0].description == "RES TEST");
    std::string j;
    CHECK(m1_normalize_job_id("  123456-10-01 ", j) && j == "123456-10-01");
    CHECK(!m1_normalize_job_id("1'; DROP TABLE x--", j));
    CHECK(!m1_normalize_job_id("", j));
    // user-saved profile of the same name wins (editable)
    std::vector<FormatTemplate> v{m1_bom_template()};
    v[0].header_roles[5].second = Role::Unused;
    CHECK(!ensure_builtin_bom_templates(v) && v.size() == 1);
}

static void real_sample() {
    const char* path = std::getenv("PCBP_M1_SAMPLE");
    if (!path || !*path) { std::printf("== real sample: skipped (PCBP_M1_SAMPLE not set)\n"); return; }
    std::printf("== real sample\n");
    BomSource src;
    std::string err, tpl;
    CHECK(bom_load_source(path, src, err));
    BomOptions2 opt;
    BomPreview p;
    CHECK(wizard_preview(src, opt, p, tpl));
    CHECK(tpl == kM1BomTemplateName);
    int errors = 0;
    for (auto& i : p.issues) {
        if (i.sev != IssueSev::Info) std::printf("   %s line %d: %s\n", issue_sev_name(i.sev), i.line, i.msg.c_str());
        errors += i.sev == IssueSev::Error;
    }
    for (auto& b : p.lines)
        if (b.qty_mismatch) std::printf("   qty mismatch line %d: qty %d, %zu refs\n", b.line, b.qty, b.refs.size());
    std::printf("   %zu lines, %zu refs, %d qty mismatch, board %s, job %s\n", p.lines.size(), p.by_comp.size(),
                p.qty_mismatches, p.board_pn.empty() ? "-" : "(set)", m1_job_id(p.text).empty() ? "-" : "(set)");
    std::printf("   %d error(s) above are data findings in the sample, reported not asserted\n", errors);
    CHECK(p.mapping_ok() && !p.lines.empty());
    Project prj;
    BomReport rep;
    CHECK(bom_apply(p, prj, opt, rep, err));
}

int main() {
    synthetic_xlsx();
    live_tsv();
    real_sample();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
