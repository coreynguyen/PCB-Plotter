// ============================================================================
// test_jet.cpp — oracle test for src/io/jet_mdb (Linux only; needs mdbtools).
//
// For every .mdb in the sample data root, reads every table (user AND MSys*
// system tables, which carry the Jet3 long-value data) and compares row
// count, column names and every cell against
//     mdb-export -b hex -D FMT -T FMT <db> <table>
// Comparison rules (intentional formatting differences):
//   * Float/Double/Money compared numerically (mdbtools prints fixed digits,
//     we print shortest round-trip): Double rel 1e-12, Float rel 1e-6,
//     Money abs 5e-5.
//   * Binary/OLE compared as hex, case-insensitive (byte-for-byte).
//   * NULL == unquoted empty field; empty text == quoted "".
// Also: fuzz pass (random byte corruption + truncation) must not crash/hang;
// and fileio round-trip / path helper checks.
// Returns nonzero on any failure.
// ============================================================================
#include "local_data.h"
#include "../src/core/fileio.h"
#include "../src/io/jet_mdb.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace pcbp;

static int g_fail = 0;
#define CHECK(c, ...)                                   \
    do {                                                \
        if (!(c)) {                                     \
            ++g_fail;                                   \
            if (g_fail < 60) {                          \
                std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                std::printf(__VA_ARGS__);               \
                std::printf("\n");                      \
            }                                           \
        }                                               \
    } while (0)

static const std::string kRootS = pcbp_test::local("legacy_demo_root"); static const char* kRoot = kRootS.c_str();   // legacy CAM demo install (local only)
static const char* kFmt = "%Y-%m-%d %H:%M:%S";

static std::string shq(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else r += c;
    }
    return r + "'";
}

static bool run(const std::string& cmd, std::string& out) {
    out.clear();
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    return pclose(p) == 0;
}

struct Cell { std::string s; bool quoted = false; };

static std::vector<std::vector<Cell>> parse_csv(const std::string& t) {
    std::vector<std::vector<Cell>> rows;
    std::vector<Cell> row;
    Cell cur;
    size_t i = 0, n = t.size();
    bool field_started = false;
    while (i < n) {
        char c = t[i];
        if (c == '"' && !field_started) {
            cur.quoted = true;
            field_started = true;
            ++i;
            while (i < n) {
                if (t[i] == '"') {
                    if (i + 1 < n && t[i + 1] == '"') { cur.s += '"'; i += 2; }
                    else { ++i; break; }
                } else cur.s += t[i++];
            }
        } else if (c == ',') {
            row.push_back(cur); cur = Cell{}; field_started = false; ++i;
        } else if (c == '\n') {
            row.push_back(cur); cur = Cell{}; field_started = false; ++i;
            rows.push_back(std::move(row)); row.clear();
        } else if (c == '\r') {
            ++i;
        } else {
            cur.s += c; field_started = true; ++i;
        }
    }
    if (field_started || !row.empty()) { row.push_back(cur); rows.push_back(row); }
    return rows;
}

static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> v;
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find('\n', a);
        if (b == std::string::npos) b = s.size();
        if (b > a) v.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return v;
}

static bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

static bool cell_equal(const jet::Value& v, const Cell& e, std::string& why) {
    using jet::ColType;
    if (v.null) {
        if (e.quoted || !e.s.empty()) { why = "we=NULL oracle='" + e.s + "'"; return false; }
        return true;
    }
    std::string ours = v.as_string();
    switch (v.type) {
    case ColType::Float: case ColType::Double: case ColType::Money: {
        if (e.s.empty()) { why = "oracle NULL, we=" + ours; return false; }
        double o = std::strtod(e.s.c_str(), nullptr);
        double tol = v.type == ColType::Money ? 5e-5
                   : (v.type == ColType::Float ? 1e-6 : 1e-12) * std::max(1.0, std::fabs(o));
        if (std::fabs(o - v.d) > tol) { why = "num we=" + ours + " oracle=" + e.s; return false; }
        return true;
    }
    case ColType::Binary: case ColType::OLE:
        // mdbtools 1.0 renders Binary (type 9) columns as empty: accept (oracle limitation).
        if (v.type == ColType::Binary && !e.quoted && e.s.empty()) return true;
        if (!ieq(ours, e.s)) {
            why = "hex mismatch len we=" + std::to_string(ours.size() / 2) + " oracle=" + std::to_string(e.s.size() / 2);
            return false;
        }
        return true;
    case ColType::Text: case ColType::Memo:
        if (!e.quoted && e.s.empty()) { why = "oracle NULL, we='" + ours + "'"; return false; }
        [[fallthrough]];
    default:
        if (ours != e.s) { why = "we='" + ours.substr(0, 80) + "' oracle='" + e.s.substr(0, 80) + "'"; return false; }
        return true;
    }
}

static void test_db(const std::string& path, int expect_ver, long& ncells, long& nrows, long& nblob, long& nskip) {
    jet::Database db;
    std::string err;
    bool ok = db.open(path, err);
    CHECK(ok, "open %s: %s", path.c_str(), err.c_str());
    if (!ok) return;
    CHECK(db.version() == expect_ver, "%s version %d expected %d", path.c_str(), db.version(), expect_ver);
    std::printf("%s: Jet%d, code page %d\n", path.c_str(), db.version(), db.code_page());

    // user table list must match mdb-tables (no MSys*)
    std::string out;
    run("mdb-tables -1 " + shq(path) + " 2>/dev/null", out);
    auto user = split_lines(out);
    auto ours = db.tables();
    CHECK(ours.size() == user.size(), "%s: table count %zu vs oracle %zu", path.c_str(), ours.size(), user.size());
    for (const auto& t : user) CHECK(db.has_table(t), "%s: missing table %s", path.c_str(), t.c_str());
    for (const auto& t : ours) CHECK(t.rfind("MSys", 0) != 0, "system table leaked: %s", t.c_str());

    run("mdb-tables -S -1 " + shq(path) + " 2>/dev/null", out);
    for (const auto& t : split_lines(out)) {
        jet::Table tab;
        bool rok = db.read_table(t, tab, err);
        CHECK(rok, "%s/%s read: %s", path.c_str(), t.c_str(), err.c_str());
        if (!rok) continue;
        std::string csv;
        std::string cmd = std::string("mdb-export -b hex -D '") + kFmt + "' -T '" + kFmt + "' " + shq(path) + " " +
                          shq(t) + " 2>/dev/null";
        if (!run(cmd, csv)) { std::printf("  (oracle cannot export %s, skipped)\n", t.c_str()); continue; }
        auto rows = parse_csv(csv);
        CHECK(!rows.empty(), "%s/%s: empty oracle output", path.c_str(), t.c_str());
        if (rows.empty()) continue;
        const auto& hdr = rows[0];
        CHECK(hdr.size() == tab.cols.size(), "%s/%s: col count %zu vs %zu", path.c_str(), t.c_str(),
              tab.cols.size(), hdr.size());
        std::vector<int> map;
        for (const auto& h : hdr) {
            int k = tab.col(h.s);
            CHECK(k >= 0, "%s/%s: missing column %s", path.c_str(), t.c_str(), h.s.c_str());
            map.push_back(k);
        }
        size_t erows = rows.size() - 1;
        CHECK(tab.rows.size() == erows, "%s/%s: rows %zu vs oracle %zu", path.c_str(), t.c_str(), tab.rows.size(),
              erows);
        // Oracle-unsupported column types (mdbtools prints nothing for them).
        auto skip_col = [&](size_t c) {
            int k = map[c];
            return k < 0 || static_cast<int>(tab.cols[static_cast<size_t>(k)].type) == 17;
        };
        auto row_eq = [&](const std::vector<jet::Value>& ours, const std::vector<Cell>& er, std::string* why) {
            for (size_t c = 0; c < std::min(er.size(), map.size()); ++c) {
                if (skip_col(c)) continue;
                std::string w;
                if (!cell_equal(ours[static_cast<size_t>(map[c])], er[c], w)) {
                    if (why) *why = "col " + hdr[c].s + ": " + w;
                    return false;
                }
            }
            return true;
        };
        // Rows are matched in order first; a row that differs is matched against any unused row
        // (mdbtools emits overflow-target rows at their physical slot, we at their logical slot).
        int tfail = 0, reordered = 0;
        std::vector<bool> used(tab.rows.size(), false);
        for (size_t r = 0; r < erows; ++r) {
            const auto& er = rows[r + 1];
            CHECK(er.size() == hdr.size(), "%s/%s row %zu: oracle field count", path.c_str(), t.c_str(), r);
            std::string why;
            long found = -1;
            if (r < tab.rows.size() && !used[r] && row_eq(tab.rows[r], er, &why)) found = static_cast<long>(r);
            for (size_t k = 0; found < 0 && k < tab.rows.size(); ++k)
                if (!used[k] && row_eq(tab.rows[k], er, nullptr)) { found = static_cast<long>(k); ++reordered; }
            if (found < 0) {
                ++tfail;
                CHECK(false, "%s/%s oracle row %zu unmatched (in-order diff: %s)", path.c_str(), t.c_str(), r,
                      why.c_str());
                continue;
            }
            used[static_cast<size_t>(found)] = true;
            const auto& ours = tab.rows[static_cast<size_t>(found)];
            for (size_t c = 0; c < std::min(er.size(), map.size()); ++c) {
                if (skip_col(c)) { ++nskip; continue; }
                const jet::Value& v = ours[static_cast<size_t>(map[c])];
                ++ncells;
                if ((v.type == jet::ColType::OLE || v.type == jet::ColType::Memo) && !v.null) ++nblob;
            }
        }
        nrows += static_cast<long>(tab.rows.size());
        std::printf("  %-28s rows=%-5zu cols=%-3zu %s", t.c_str(), tab.rows.size(), tab.cols.size(),
                    tfail ? "MISMATCH" : "ok");
        if (reordered) std::printf(" (%d rows in different order than mdbtools)", reordered);
        std::printf("\n");
    }
}

// Corrupt random bytes / truncate: must not crash or hang.
static void fuzz(const std::string& path, int iters) {
    std::vector<uint8_t> base;
    std::string err;
    if (!read_file(path, base, err)) return;
    std::mt19937 rng(12345);
    auto t0 = std::chrono::steady_clock::now();
    for (int it = 0; it < iters; ++it) {
        std::vector<uint8_t> d = base;
        int mode = it % 4;
        if (mode == 3) {
            d.resize(rng() % d.size());
        } else {
            int nflip = 1 + static_cast<int>(rng() % (mode == 0 ? 8 : 200));
            for (int k = 0; k < nflip; ++k) {
                size_t pos = rng() % d.size();
                if (mode == 2) pos = (rng() % 8) * (d[0x14] ? 4096 : 2048) + rng() % 4096;  // hammer catalog pages
                if (pos < d.size()) d[pos] = static_cast<uint8_t>(rng());
            }
        }
        jet::Database db;
        if (!db.open_memory(std::move(d), err)) continue;
        for (const auto& t : db.tables()) {
            jet::Table tab;
            db.read_table(t, tab, err);
            for (const auto& r : tab.rows)
                for (const auto& v : r) (void)v.as_string();
        }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("fuzz %s: %d iterations, %.1fs, no crash\n", file_stem(path).c_str(), iters, secs);
}

static void test_fileio() {
    CHECK(file_ext_lower("C:\\a.b\\Cclib.MDB") == ".mdb", "ext");
    CHECK(file_ext_lower("C:\\a.b\\noext") == "", "ext none");
    CHECK(file_stem("C:\\x\\cclib.mdb") == "cclib", "stem");
    CHECK(file_stem("/a/b/.hidden") == ".hidden", "stem hidden");
    CHECK(file_dir("C:\\x\\cclib.mdb") == "C:\\x", "dir");
    CHECK(file_dir("cclib.mdb") == "", "dir none");
    std::string u = "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";  // A é € 😀
    CHECK(wide_to_utf8(utf8_to_wide(u)) == u, "utf8 round trip");
    CHECK(utf8_to_wide("\xFF").size() == 1, "invalid utf8 -> U+FFFD");
    std::string tmp = "/tmp/pcbp_fileio_test.bin", err;
    CHECK(write_file(tmp, u.data(), u.size(), err), "write_file: %s", err.c_str());
    std::string back;
    CHECK(read_text_file(tmp, back, err) && back == u, "read_text_file");
    std::remove(tmp.c_str());
    std::vector<uint8_t> dummy;
    CHECK(!read_file("/nonexistent/x", dummy, err), "read missing");
    CHECK(jet::format_jet_date(0.0) == "1899-12-30 00:00:00", "date epoch");
    CHECK(jet::format_jet_date(36526.5) == "2000-01-01 12:00:00", "date 2000");
    CHECK(jet::format_jet_date(-1.25) == "1899-12-29 06:00:00", "date negative");
}

int main() {
    test_fileio();
    struct { const char* f; int v; } dbs[] = {
        {"cclib.mdb", 4}, {"Defaults.mdb", 4}, {"reports.mdb", 3}, {"labels.mdb", 3}};
    long cells = 0, rows = 0, blobs = 0, skipped = 0;
    for (auto& d : dbs) test_db(std::string(kRoot) + "/" + d.f, d.v, cells, rows, blobs, skipped);

    // Packages.Rendering (OLE) byte-for-byte + API checks.
    {
        jet::Database db;
        std::string err;
        jet::Table t;
        if (db.open(std::string(kRoot) + "/cclib.mdb", err) && db.read_table("packages", t, err)) {
            int rc = t.col("RENDERING");
            CHECK(rc >= 0 && t.cols[static_cast<size_t>(rc)].type == jet::ColType::OLE, "Rendering col");
            size_t total = 0, maxlen = 0;
            for (size_t r = 0; r < t.rows.size(); ++r) {
                const jet::Value* v = t.get(r, "Rendering");
                if (v && !v->null) { total += v->bin.size(); maxlen = std::max(maxlen, v->bin.size()); }
            }
            std::printf("Packages.Rendering: %zu rows, %zu blob bytes, largest %zu\n", t.rows.size(), total, maxlen);
            CHECK(t.get(0, "nope") == nullptr, "get missing col");
            CHECK(!db.read_table("NoSuchTable", t, err), "missing table");
        }
        std::vector<uint8_t> junk(10000, 0);
        CHECK(!db.open_memory(junk, err), "junk rejected");
    }
    std::printf("compared %ld cells in %ld rows (%ld non-null MEMO/OLE); %ld cells of oracle-unsupported type 17 skipped\n",
                cells, rows, blobs, skipped);

    for (auto& d : dbs) fuzz(std::string(kRoot) + "/" + d.f, 300);

    if (g_fail) { std::printf("FAILED: %d checks\n", g_fail); return 1; }
    std::printf("ALL PASS\n");
    return 0;
}
