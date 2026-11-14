// ============================================================================
// test_refexpand.cpp — reference-designator expansion rules + bounded fuzz
// (specs/REFEXPAND.md). Every input must produce bounded output, no crash.
// ============================================================================
#include "../src/io/text_import.h"
#include <chrono>
#include <cstdio>
#include <random>
#include <string>

using namespace pcbp::io;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool has(const RefExpansion& r, RefIssue k) { for (auto& i : r.issues) if (i.kind == k) return true; return false; }
static std::string J(const std::vector<std::string>& v) { std::string s; for (auto& x : v) { if (!s.empty()) s += ","; s += x; } return s; }

static void rules() {
    std::printf("== rules\n");
    auto r = expand_refs_ex("C1-C3, C7");
    CHECK(J(r.refs) == "C1,C2,C3,C7" && r.issues.empty());
    r = expand_refs_ex("R1;R2 R3\nR4/R5");
    CHECK(r.refs.size() == 5);
    r = expand_refs_ex("r1 - r3");
    CHECK(J(r.refs) == "R1,R2,R3");
    r = expand_refs_ex("R1-5");
    CHECK(r.refs.size() == 5 && r.refs[4] == "R5");
    // prefix must match on both ends
    r = expand_refs_ex("R1-C5");
    CHECK(has(r, RefIssue::PrefixMismatch) && r.refs.size() == 1);
    // mixed alnum: same suffix on both ends, or a letter range on one number
    r = expand_refs_ex("R1A-R3A");
    CHECK(J(r.refs) == "R1A,R2A,R3A");
    r = expand_refs_ex("U1A-U1D");
    CHECK(J(r.refs) == "U1A,U1B,U1C,U1D");
    r = expand_refs_ex("R1A-R5B");
    CHECK(has(r, RefIssue::SuffixMismatch) && r.refs.size() == 1);
    r = expand_refs_ex("U1D-U1A");
    CHECK(has(r, RefIssue::Reversed));
    // reversed
    r = expand_refs_ex("R5-R1");
    CHECK(has(r, RefIssue::Reversed) && r.refs.size() == 1 && r.refs[0] == "R5-R1");
    // zero padding
    r = expand_refs_ex("R08-R11");
    CHECK(J(r.refs) == "R08,R09,R10,R11");
    r = expand_refs_ex("C10-0805");
    CHECK(has(r, RefIssue::PadMismatch) && r.refs.size() == 1);
    r = expand_refs_ex("R001-R09");
    CHECK(has(r, RefIssue::PadMismatch));
    // bounded spans
    r = expand_refs_ex("R1-R2000");
    CHECK(r.refs.size() == 2000 && r.errors() == 0);
    r = expand_refs_ex("R1-R2001");
    CHECK(has(r, RefIssue::SpanTooLarge) && r.refs.size() == 1);
    r = expand_refs_ex("R1-R999999");
    CHECK(has(r, RefIssue::SpanTooLarge) && r.refs.size() == 1);
    r = expand_refs_ex("R1-R99999999999");      // > 6 digits: not a ref, literal
    CHECK(r.refs.size() == 1);
    RefExpandOptions small; small.max_span = 10;
    r = expand_refs_ex("R1-R11", small);
    CHECK(has(r, RefIssue::SpanTooLarge));
    // total limit across many ranges
    std::string many;
    for (int i = 0; i < 40; ++i) many += "R" + std::to_string(i * 2000 + 1) + "-R" + std::to_string(i * 2000 + 2000) + ",";
    r = expand_refs_ex(many);
    CHECK(r.refs.size() == 20000 && has(r, RefIssue::TotalLimit));
    // malformed
    r = expand_refs_ex("R1-");
    CHECK(r.refs.size() == 1);
    r = expand_refs_ex("R1-R");
    CHECK(has(r, RefIssue::Malformed));
    // hyphenated names stay names
    r = expand_refs_ex("U1-A, J-1, PCB-1");
    CHECK(J(r.refs) == "U1-A,J-1,PCB-1" && r.errors() == 0);
    // duplicates
    r = expand_refs_ex("C1, C2, c1, C1-C3");
    CHECK(J(r.refs) == "C1,C2,C3" && r.duplicates.size() == 3 && r.errors() == 0);
    // bare numbers (opt-in)
    RefExpandOptions bn; bn.bare_numbers = true;
    r = expand_refs_ex("C1, 3, 5", bn);
    CHECK(J(r.refs) == "C1,C3,C5" && has(r, RefIssue::BareNumber));
    r = expand_refs_ex("C1, 3, 5");
    CHECK(J(r.refs) == "C1,3,5");
    // Juki Tool pathological inputs (specs/REFEXPAND.md §2)
    r = expand_refs_ex("- C1");              // leading lone dash
    CHECK(r.refs.size() <= 2);
    r = expand_refs_ex("C1 -");              // trailing lone dash
    CHECK(r.refs.size() <= 2);
    r = expand_refs_ex("C1-2C100");          // digits glued across tokens
    CHECK(r.refs.size() == 1);
    r = expand_refs_ex("R1-R1");
    CHECK(J(r.refs) == "R1");
    // legacy wrapper
    std::vector<std::string> w;
    auto v = expand_refs("R1-R3, R5-R1", true, &w);
    CHECK(v.size() == 4 && w.size() == 1);
}

static void fuzz() {
    std::printf("== fuzz\n");
    std::mt19937 rng(12345);
    const char* atoms[] = {"R", "C", "U", "r", "1", "0", "9", "99", "123456", "1234567", "-", "-", ",", " ", ";", "/", "..", "~", ":",
                           "A", "Z", "to", "\n", "\t", "$", "_", "999999", "0001", "\xE2\x80\x93", "(", ")", "\"", "R1-R2000", "x"};
    const int na = sizeof atoms / sizeof *atoms;
    auto t0 = std::chrono::steady_clock::now();
    size_t max_out = 0;
    bool bounded = true;
    for (int it = 0; it < 200000; ++it) {
        std::string s;
        int n = 1 + rng() % 24;
        for (int k = 0; k < n; ++k) s += atoms[rng() % na];
        if (rng() % 50 == 0) for (int k = 0; k < 8; ++k) s += (char)(rng() % 256);
        RefExpandOptions o;
        o.bare_numbers = rng() & 1;
        auto r = expand_refs_ex(s, o);
        max_out = std::max(max_out, r.refs.size());
        if (r.refs.size() > o.max_total) bounded = false;
        for (auto& x : r.refs) if (x.empty()) bounded = false;
    }
    // adversarial: many large ranges + very long input
    std::string big;
    for (int i = 0; i < 100000; ++i) big += "R1-R2000,";
    auto r = expand_refs_ex(big);
    CHECK(r.refs.size() <= 20000);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("   200k cases, max output %zu refs, %.0f ms\n", max_out, ms);
    CHECK(bounded);
    CHECK(ms < 60000);
}

// ERP (M1) job-BOM forms: '~' ranges and hyphenated single names, mixed in one cell.
static void m1_forms() {
    std::printf("== m1 forms\n");
    auto r = expand_refs_ex("B-J2 B-P10");
    CHECK(J(r.refs) == "B-J2,B-P10" && r.errors() == 0);
    r = expand_refs_ex("C1~C19 C101~C120");
    CHECK(r.refs.size() == 39 && r.errors() == 0);
    CHECK(r.refs.front() == "C1" && r.refs[18] == "C19" && r.refs[19] == "C101" && r.refs.back() == "C120");
    r = expand_refs_ex("C45~C50 C141~C144");
    CHECK(J(r.refs) == "C45,C46,C47,C48,C49,C50,C141,C142,C143,C144" && r.errors() == 0);
    r = expand_refs_ex("B-J2 C1~C3 B-P10");
    CHECK(J(r.refs) == "B-J2,C1,C2,C3,B-P10" && r.errors() == 0);
}

int main() {
    rules();
    m1_forms();
    fuzz();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
