// Spreadsheet model + xlsx/xls I/O tests (Linux, portable).
#include "core/sheet.h"
#include "core/fileio.h"
#include "io/xlsx.h"
#include <cstdio>
#include <string>

using namespace pcbp;
static int g_fail = 0, g_ok = 0;
#define CHECK(c) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (_a == _b) ++g_ok; else { ++g_fail; std::printf("FAIL %s:%d  %s == %s  ('%s' vs '%s')\n", __FILE__, __LINE__, #a, #b, std::string(_a).c_str(), std::string(_b).c_str()); } } while (0)

static std::vector<uint8_t> rd(const char* p) { std::vector<uint8_t> v; std::string e; read_file(p, v, e); return v; }

int main() {
    // ---- CSV quoting ----
    {
        std::vector<sheet::Row> rows = {{"a", "b,c", "say \"hi\""}, {"multi\nline", "", " lead"}, {}, {"x"}};
        std::string csv = sheet::to_delimited(rows, ',', true);
        CHECK_EQ(csv, std::string("a,\"b,c\",\"say \"\"hi\"\"\"\r\n\"multi\nline\",,\" lead\"\r\n\r\nx\r\n"));
        auto back = sheet::parse_delimited(csv, ',');
        CHECK(back.size() == 4);
        CHECK(back == rows);
        auto t = sheet::parse_delimited("1;\"2;3\";4\n5", ';');
        CHECK(t.size() == 2 && t[0].size() == 3 && t[0][1] == "2;3" && t[1][0] == "5");
        // Excel clipboard round trip
        std::string clip = sheet::to_clipboard({{"R1", "10k"}, {"tab\there", "q\"x"}});
        CHECK_EQ(clip, std::string("R1\t10k\r\n\"tab\there\"\t\"q\"\"x\"\r\n"));
        auto cb = sheet::from_clipboard(clip);
        CHECK(cb.size() == 2 && cb[1][0] == "tab\there" && cb[1][1] == "q\"x");
    }
    // ---- natural sort ----
    CHECK(sheet::natural_cmp("R2", "R10") < 0);
    CHECK(sheet::natural_cmp("r10", "R10") == 0);
    CHECK(sheet::natural_cmp("C1", "R1") < 0);
    CHECK_EQ(sheet::col_name(0), std::string("A"));
    CHECK_EQ(sheet::col_name(27), std::string("AB"));
    CHECK(sheet::col_index("AB") == 27);
    // ---- ref expand / compress ----
    CHECK_EQ(sheet::expand_refs_cell("R1-R4, C7"), std::string("R1, R2, R3, R4, C7"));
    CHECK_EQ(sheet::compress_refs({"R3", "R1", "R2", "R4", "C7", "R9", "R10", "C8"}), std::string("C7, C8, R1-R4, R9, R10"));
    CHECK_EQ(sheet::compress_refs_cell("R1 R2 R3 R5"), std::string("R1-R3, R5"));
    CHECK_EQ(sheet::compress_refs_cell(sheet::expand_refs_cell("U1-U12")), std::string("U1-U12"));
    // ---- ops ----
    {
        sheet::Sheet s;
        s.rows = {{"Ref", "PN"}, {"R10", " a  b "}, {"R2", "x;y;z"}, {}, {"R10", " a  b "}};
        CHECK(sheet::trim_cells(s, 0, 1) == 2);
        CHECK(s.at(1, 1) == "a b");
        CHECK(sheet::remove_empty_rows(s) == 1);
        CHECK(sheet::dedupe_rows(s, 1) == 1);
        sheet::sort_rows(s, 0, false, 1);
        CHECK(s.at(1, 0) == "R2" && s.at(2, 0) == "R10");
        CHECK(sheet::split_column(s, 1, ";", 1) == 1);
        CHECK(s.ncols() == 4 && s.at(1, 3) == "z");
        CHECK(sheet::merge_columns(s, 1, 3, "-", 1) >= 1);
        CHECK(s.at(1, 1) == "x-y-z" && s.ncols() == 2);
        CHECK(sheet::is_plain_number("12.5") && !sheet::is_plain_number("0805") && !sheet::is_plain_number("1e") && sheet::is_plain_number("0.1"));
    }
    // ---- text -> sheet ----
    {
        std::vector<uint8_t> b = rd("tests/data/text/altium_bom.csv");
        if (!b.empty()) {
            std::string how;
            sheet::Sheet s = sheet::sheet_from_text(std::string(b.begin(), b.end()), &how);
            CHECK(s.nrows() > 2 && s.ncols() >= 3);
            std::printf("altium_bom.csv: %d x %d (%s)\n", s.nrows(), s.ncols(), how.c_str());
        }
        b = rd("tests/data/text/DemoBOM.prn");
        if (!b.empty()) {
            std::string how;
            sheet::Sheet s = sheet::sheet_from_text(std::string(b.begin(), b.end()), &how);
            CHECK(s.nrows() > 5 && s.ncols() >= 4);
            std::printf("DemoBOM.prn: %d x %d (%s) row6: [%s|%s|%s]\n", s.nrows(), s.ncols(), how.c_str(), s.at(5, 0).c_str(), s.at(5, 1).c_str(), s.at(5, 2).c_str());
        }
    }
    // ---- xlsx round trip ----
    {
        sheet::Workbook wb;
        sheet::Sheet a; a.name = "BOM";
        a.rows = {{"Ref", "PN", "Qty"}, {"R1-R5", "0805", "5"}, {"C1", "x&y<z>\"", "0.1"}, {}, {"", " pad ", "-3e5"}, {"multi\nline", "\xE6\x97\xA5\xE6\x9C\xAC", "1234567"}};
        sheet::Sheet b; b.name = "Two"; b.set(9, 30, "far");
        wb.sheets = {a, b};
        auto bytes = io::xlsx_write(wb);
        CHECK(io::is_xlsx_bytes(bytes));
        sheet::Workbook rb; std::string err;
        CHECK(io::xlsx_read(bytes, rb, err));
        CHECK(rb.sheets.size() == 2);
        if (rb.sheets.size() == 2) {
            CHECK_EQ(rb.sheets[0].name, std::string("BOM"));
            const sheet::Sheet& r = rb.sheets[0];
            for (int i = 0; i < a.nrows(); ++i) for (int c = 0; c < 3; ++c)
                if (a.at(i, c) != r.at(i, c) && !(a.at(i, c) == "-3e5" && r.at(i, c) == "-300000")) { ++g_fail; std::printf("FAIL xlsx cell %d,%d '%s' vs '%s'\n", i, c, a.at(i, c).c_str(), r.at(i, c).c_str()); }
            CHECK_EQ(rb.sheets[1].at(9, 30), std::string("far"));
        }
        std::string e2;
        write_file("build/tests/roundtrip.xlsx", bytes.data(), bytes.size(), e2);
    }
    // ---- xlsx written by openpyxl (shared strings) ----
    {
        auto bytes = rd("tests/data/sheet/sample_openpyxl.xlsx");
        sheet::Workbook wb; std::string err;
        CHECK(io::xlsx_read(bytes, wb, err));
        if (wb.sheets.size() >= 2) {
            const auto& s = wb.sheets[0];
            CHECK_EQ(s.at(1, 0), std::string("R1-R5"));
            CHECK_EQ(s.at(1, 3), std::string("10000"));
            CHECK_EQ(s.at(2, 3), std::string("0.1"));
            CHECK_EQ(s.at(2, 4), std::string("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E cap"));
            CHECK_EQ(s.at(3, 1), std::string("0805"));
            CHECK_EQ(s.at(6, 0), std::string("  padded  "));
            CHECK_EQ(wb.sheets[1].at(1, 1), std::string("x&y<z>"));
        } else { ++g_fail; std::printf("FAIL openpyxl read: %s\n", err.c_str()); }
    }
    // ---- xls (BIFF8 by python xlwt) ----
    {
        auto bytes = rd("tests/data/sheet/sample.xls");
        if (bytes.empty()) std::printf("SKIP xls: tests/data/sheet/sample.xls missing\n");
        else {
            sheet::Workbook wb; std::string err;
            bool ok = io::xls_read(bytes, wb, err);
            CHECK(ok);
            if (!ok) std::printf("xls err: %s\n", err.c_str());
            if (wb.sheets.size() == 2) {
                const auto& s = wb.sheets[0];
                CHECK_EQ(s.name, std::string("BOM"));
                CHECK_EQ(s.at(0, 1), std::string("Part Number"));
                CHECK_EQ(s.at(1, 2), std::string("5"));
                CHECK_EQ(s.at(1, 4), std::string("R\xC3\xA9sistance 10k"));
                CHECK_EQ(s.at(2, 3), std::string("0.1"));
                CHECK_EQ(s.at(2, 4), std::string("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E cap"));
                CHECK_EQ(s.at(3, 4), std::string("quote \" and, comma"));
                CHECK_EQ(wb.sheets[1].at(2, 1), std::string("-42"));
                CHECK_EQ(wb.sheets[1].at(309, 0), std::string("row299"));
            } else { ++g_fail; std::printf("FAIL xls sheets=%d\n", (int)wb.sheets.size()); }
        }
    }
    std::printf("test_sheet: %d ok, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
