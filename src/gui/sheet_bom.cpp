// ============================================================================
// sheet_bom.cpp — BOM workflow in the spreadsheet editor:
//   * column role detection (Ref / Qty / P/N / Description / DNP / Package)
//   * Send to Project: visible (filtered) rows -> BOM / centroid import wizard
//     with the header row preselected (wizard guesses the column roles);
//     Import = Replace mode, Update = Merge mode
//   * BOM tools: expand / compress refdes, one ref per row, REFCOUNT column,
//     Qty vs ref-count check (conditional formatting), split DNP rows to a
//     sheet, merge rows with the same part number
//   * Project round trip: "Open project BOM in spreadsheet" builds a styled,
//     filterable sheet linked to the project; "Apply to Project" diffs the
//     sheet against the project (renames, fields, links, DNP) and applies the
//     checked changes in one undo step.
// ============================================================================
#include "sheet_editor.h"
#include "../app/app.h"
#include "../io/text_import.h"
#include "../ui/form.h"
#include "../ui/win.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace pcbp::gui {
using namespace sheet;

static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, e - a + 1);
}

// ---------------------------------------------------------------- column roles
SheetDoc::BomCols SheetDoc::guess_bom_cols() {
    BomCols bc;
    const Worksheet& W = ws();
    int nc = std::min(W.ncols(), 200);
    auto score_row = [&](int r, BomCols& out) {
        int sc = 0;
        BomCols t; t.header = r;
        for (int c = 0; c < nc; ++c) {
            std::string h = lower(trim(b.display_text(cur, r, c)));
            if (h.empty()) continue;
            auto has = [&](const char* k) { return h.find(k) != std::string::npos; };
            auto set = [&](int& f) { if (f < 0) { f = c; ++sc; } };
            if (has("ref") || has("designator") || has("location") || h == "refs" || has("position") || h == "parts") set(t.ref);
            else if (has("qty") || has("quantity") || h == "count" || h == "qnty") set(t.qty);
            else if (has("dnp") || has("populate") || has("fitted") || has("dni") || has("no pop") || has("nopop") || has("place") || has("install")) set(t.dnp);
            else if (has("desc")) set(t.desc);
            else if (has("package") || has("footprint") || h == "case" || has("pcb decal") || has("pattern")) set(t.pkg);
            else if (h == "value" || h == "val" || has("value")) set(t.value);
            else if (has("part n") || has("part#") || h == "pn" || h == "p/n" || has("part no") || has("item n") || has("internal") || has("stock") || has("cpn") || h == "part" || has("mpn") || has("manufacturer part")) set(t.pn);
            else if (h == "item" || h == "#" || h == "line" || h == "no." || h == "no") set(t.item);
        }
        if (sc > 0) out = t;
        return sc;
    };
    // explicit header: autofilter / frozen rows
    if (W.autofilter.active()) { score_row(W.autofilter.range.r0, bc); bc.header = W.autofilter.range.r0; }
    else if (W.freeze_rows > 0) { score_row(W.freeze_rows - 1, bc); bc.header = W.freeze_rows - 1; }
    else {
        int best = 0;
        for (int r = 0; r < std::min(W.nrows(), 40); ++r) { BomCols t; int s = score_row(r, t); if (s > best) { best = s; bc = t; } }
    }
    // fall back: refs column = the column with the most ref lists
    if (bc.ref < 0) {
        int bestc = -1, bestn = 0;
        for (int c = 0; c < nc; ++c) {
            int n = 0;
            for (int r = bc.header + 1; r < std::min(W.nrows(), bc.header + 200); ++r) {
                std::string v = b.display_text(cur, r, c);
                if (v.size() > 1 && io::is_ref_list(v) && !is_plain_number(v)) ++n;
            }
            if (n > bestn) { bestn = n; bestc = c; }
        }
        bc.ref = bestc;
    }
    return bc;
}

// ---------------------------------------------------------------- send to project
std::string SheetDoc::export_tsv(int& start_line) {
    const Worksheet& W = ws();
    BomCols bc = guess_bom_cols();
    std::vector<Row> rows;
    start_line = 0;
    int nr = W.nrows(), nc = W.ncols();
    for (int r = 0; r < nr; ++r) {
        if (W.row_hidden(r) && r != bc.header) continue;
        if (r == bc.header) start_line = (int)rows.size();
        Row row(nc);
        for (int c = 0; c < nc; ++c) if (!W.col_hidden(c) || c == bc.ref) row[c] = b.display_text(cur, r, c);
        while (!row.empty() && row.back().empty()) row.pop_back();
        rows.push_back(std::move(row));
    }
    return to_delimited(rows, '\t', false);
}

void SheetDoc::send_to_project(int mode) {
    commit_edit();
    if (mode == 2 && !app::active_doc()) { toast("Open or create a project first (Update BOM merges into the current project)", ToastKind::Warning, 4.f); return; }
    int start = 0;
    std::string bytes = export_tsv(start);
    std::wstring disp = path.empty() ? ui::W(name) : path;
    bool bom = mode != 1;
    int hint = mode == 0 ? 1 : mode == 2 ? 0 : -1;
    defer([bytes, start, bom, disp, hint]() {
        if (hint >= 0) set_import_mode_hint(hint);
        import_from_bytes(bytes, start, bom, disp);
    });
}

// ---------------------------------------------------------------- tools
static Range data_rows(const Worksheet& W, int header, int col) { return Range(header + 1, col, std::max(header + 1, W.nrows() - 1), col); }

void SheetDoc::bom_expand_refs(bool expand) {
    commit_edit();
    Range g = prim();
    Range rg = g;
    if (g.rows() == 1 && g.cols() == 1) {
        BomCols bc = guess_bom_cols();
        if (bc.ref < 0) { toast("Select the reference cells (no reference column detected)", ToastKind::Warning, 3.f); return; }
        rg = data_rows(ws(), bc.header, bc.ref);
    }
    rg.r1 = std::min(rg.r1, ws().nrows() - 1); rg.c1 = std::min(rg.c1, ws().ncols() - 1);
    uint64_t g0 = b.gen;
    b.map_cells(cur, rg, [expand](const std::string& v) {
        if (!io::is_ref_list(v)) return v;
        return expand ? expand_refs_cell(v) : compress_refs_cell(v);
    }, expand ? "Expand references" : "Compress references");
    if (b.gen != g0 && expand) autofit_cols(rg.c0, rg.c1);
    msg = std::string(expand ? "Expanded" : "Compressed") + " references in " + range_name(rg);
    toast(msg, ToastKind::Success, 2.5f);
}

void SheetDoc::bom_expand_to_rows() {
    commit_edit();
    BomCols bc = guess_bom_cols();
    int col = bc.ref >= 0 ? bc.ref : ac();
    const Worksheet& W = ws();
    int nr = W.nrows(), nc = W.ncols();
    std::vector<std::vector<std::string>> out;
    int added = 0;
    for (int r = bc.header + 1; r < nr; ++r) {
        std::vector<std::string> row(nc);
        for (int c = 0; c < nc; ++c) row[c] = b.input_text(cur, r, c);
        std::string v = b.display_text(cur, r, col);
        auto refs = io::is_ref_list(v) ? io::expand_refs(v, false) : std::vector<std::string>{};
        if (refs.size() <= 1) { out.push_back(std::move(row)); continue; }
        for (auto& ref : refs) {
            auto x = row;
            x[col] = ref;
            if (bc.qty >= 0) x[bc.qty] = "1";
            out.push_back(std::move(x));
        }
        added += (int)refs.size() - 1;
    }
    if (!added) { toast("No multi-reference cells in column " + col_name(col), ToastKind::Info, 2.5f); return; }
    b.set_inputs(cur, bc.header + 1, 0, out);
    msg = "One reference per row: +" + std::to_string(added) + " rows";
    toast(msg, ToastKind::Success, 2.5f);
}

void SheetDoc::bom_count_refs() {
    commit_edit();
    BomCols bc = guess_bom_cols();
    if (bc.ref < 0) { toast("No reference column detected", ToastKind::Warning, 3.f); return; }
    const Worksheet& W = ws();
    int nr = W.nrows();
    Book::Batch bt(b, "Count references");
    int at = bc.ref + 1;
    b.insert_cols(cur, at, 1);
    std::vector<std::vector<std::string>> blk;
    blk.push_back({"REFCOUNT"});
    for (int r = bc.header + 1; r < nr; ++r) {
        bool empty = ws().value(r, bc.ref).is_empty();
        blk.push_back({empty ? std::string() : "=REFCOUNT(" + cell_name(r, bc.ref) + ")"});
    }
    b.set_inputs(cur, bc.header, at, blk);
    Style hs = b.style_of(cur, bc.header, bc.ref);
    b.apply_style(cur, Range(bc.header, at, bc.header, at), [hs](Style& s) { s = hs; }, "Header style");
    autofit_cols(at, at);
    toast("Added REFCOUNT column " + col_name(at), ToastKind::Success, 2.5f);
}

void SheetDoc::bom_check_qty() {
    commit_edit();
    BomCols bc = guess_bom_cols();
    if (bc.ref < 0 || bc.qty < 0) { toast("Need both a reference and a Qty column (header row)", ToastKind::Warning, 3.5f); return; }
    const Worksheet& W = ws();
    int nr = W.nrows(), bad = 0;
    for (int r = bc.header + 1; r < nr; ++r) {
        if (W.row_hidden(r)) continue;
        std::string refs = b.display_text(cur, r, bc.ref);
        const Value& q = W.value(r, bc.qty);
        if (refs.empty() && q.is_empty()) continue;
        double qv = q.is_num() ? q.num : atof(b.display_text(cur, r, bc.qty).c_str());
        if ((int)std::lround(qv) != refdes_count(refs)) ++bad;
    }
    // conditional formatting rule on the Qty column (live: re-evaluates while editing)
    CondFormat cf;
    cf.ranges.push_back(Range(bc.header + 1, bc.qty, std::max(bc.header + 1, nr - 1), bc.qty));
    cf.ranges.push_back(Range(bc.header + 1, bc.ref, std::max(bc.header + 1, nr - 1), bc.ref));
    CondRule ru;
    ru.type = CondRule::Expression;
    ru.f1 = "AND(" + cell_name(bc.header + 1, bc.ref, false, true) + "<>\"\"," + "REFCOUNT(" + cell_name(bc.header + 1, bc.ref, false, true) + ")<>" + cell_name(bc.header + 1, bc.qty, false, true) + ")";
    Dxf dx; dx.fill_color = 0xFFFFC7CE; dx.font_color = 0xFF9C0006; dx.bold = true;
    b.dxfs.push_back(dx);
    ru.dxf = (int)b.dxfs.size() - 1;
    cf.rules.push_back(ru);
    // replace an earlier Qty check rule (same formula) instead of stacking
    auto list = W.cond;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const CondFormat& c) { return c.rules.size() == 1 && c.rules[0].type == CondRule::Expression && c.rules[0].f1 == ru.f1; }), list.end());
    list.push_back(cf);
    b.set_cond_formats(cur, list);
    if (bad) toast(std::to_string(bad) + " row(s) where Qty differs from the reference count (highlighted red)", ToastKind::Warning, 5.f);
    else toast("Qty matches the reference count on every row", ToastKind::Success, 3.f);
}

static bool row_is_dnp(const Book& b, int si, int r, const SheetDoc::BomCols& bc) {
    if (bc.dnp >= 0) {
        std::string h = lower(b.display_text(si, bc.header, bc.dnp));
        bool invert = h.find("fitted") != std::string::npos || h.find("populate") != std::string::npos || h.find("place") != std::string::npos || h.find("install") != std::string::npos;
        if (h.find("not") != std::string::npos || h.find("dnp") != std::string::npos || h.find("no ") != std::string::npos) invert = false;
        std::string v = trim(b.display_text(si, r, bc.dnp));
        if (!v.empty() && io::cell_means_dnp(v, invert)) return true;
    }
    for (int c : {bc.desc, bc.value, bc.pn}) {
        if (c < 0) continue;
        std::string v = " " + lower(b.display_text(si, r, c)) + " ";
        for (const char* k : {" dnp ", " dni ", " dnf ", "not fitted", "no pop", "do not populate", "(dnp)", " nf "}) if (v.find(k) != std::string::npos) return true;
    }
    return false;
}

void SheetDoc::bom_split_dnp() {
    commit_edit();
    BomCols bc = guess_bom_cols();
    const Worksheet& W = ws();
    int nr = W.nrows(), nc = W.ncols();
    std::vector<int> dn;
    for (int r = bc.header + 1; r < nr; ++r) if (row_is_dnp(b, cur, r, bc)) dn.push_back(r);
    if (dn.empty()) { toast("No DNP rows found (DNP column, or DNP / not fitted in the description)", ToastKind::Info, 3.5f); return; }
    int src = cur;
    Book::Batch bt(b, "Split DNP rows");
    int si = b.add_sheet(W.name.substr(0, 26) + " DNP", src + 1);
    sync_ui();
    ClipBlock hdr = b.copy(src, Range(0, 0, bc.header, nc - 1));
    b.paste(si, 0, 0, hdr, PasteAll);
    int out = bc.header + 1;
    for (int r : dn) { ClipBlock row = b.copy(src, Range(r, 0, r, nc - 1)); b.paste(si, out++, 0, row, PasteAll); }
    for (int c = 0; c < nc; ++c) if (c < (int)b.sheets[src].cols.size() && b.sheets[src].cols[c].width >= 0) b.set_col_width(si, c, c, b.sheets[src].cols[c].width);
    b.set_freeze(si, bc.header + 1, 0);
    for (int i = (int)dn.size() - 1; i >= 0;) {
        int e = dn[i], s = e;
        while (i > 0 && dn[i - 1] == s - 1) { --i; --s; }
        b.delete_rows(src, s, e - s + 1);
        --i;
    }
    cur = src;
    toast("Moved " + std::to_string(dn.size()) + " DNP row(s) to sheet \"" + b.sheets[si].name + "\"", ToastKind::Success, 4.f);
}

void SheetDoc::bom_dedupe_pn() {
    commit_edit();
    BomCols bc = guess_bom_cols();
    if (bc.pn < 0) { toast("No part number column detected in the header row", ToastKind::Warning, 3.f); return; }
    const Worksheet& W = ws();
    int nr = W.nrows();
    std::map<std::string, std::vector<int>> groups;
    std::vector<std::string> order;
    for (int r = bc.header + 1; r < nr; ++r) {
        if (W.row_hidden(r)) continue;
        std::string pn = trim(b.display_text(cur, r, bc.pn));
        if (pn.empty()) continue;
        std::string k = lower(pn);
        if (!groups.count(k)) order.push_back(k);
        groups[k].push_back(r);
    }
    std::vector<int> dels;
    int merged = 0;
    Book::Batch bt(b, "Merge rows by part number");
    for (auto& k : order) {
        auto& rs = groups[k];
        if (rs.size() < 2) continue;
        ++merged;
        if (bc.ref >= 0) {
            std::vector<std::string> refs;
            std::set<std::string> seen;
            for (int r : rs) for (auto& x : io::expand_refs(b.display_text(cur, r, bc.ref), false)) if (seen.insert(x).second) refs.push_back(x);
            std::sort(refs.begin(), refs.end(), [](const std::string& a, const std::string& c) { return natural_cmp(a, c) < 0; });
            b.set_input(cur, rs[0], bc.ref, compress_refs(refs));
            if (bc.qty >= 0) b.set_input(cur, rs[0], bc.qty, std::to_string(refs.size()));
        } else if (bc.qty >= 0) {
            double q = 0;
            for (int r : rs) { const Value& v = W.value(r, bc.qty); q += v.is_num() ? v.num : atof(b.display_text(cur, r, bc.qty).c_str()); }
            char buf[32]; std::snprintf(buf, sizeof buf, "%g", q);
            b.set_input(cur, rs[0], bc.qty, buf);
        }
        dels.insert(dels.end(), rs.begin() + 1, rs.end());
    }
    std::sort(dels.begin(), dels.end());
    for (int i = (int)dels.size() - 1; i >= 0;) {
        int e = dels[i], s = e;
        while (i > 0 && dels[i - 1] == s - 1) { --i; --s; }
        b.delete_rows(cur, s, e - s + 1);
        --i;
    }
    if (!merged) toast("Every part number is already unique", ToastKind::Info, 2.5f);
    else toast("Merged " + std::to_string(merged) + " part number(s), removed " + std::to_string(dels.size()) + " row(s)", ToastKind::Success, 3.5f);
}

// ===========================================================================
// Project BOM round trip
// ===========================================================================
enum PCol { PC_ITEM, PC_PN, PC_DESC, PC_QTY, PC_REFS, PC_PKG, PC_DNP, PC_MPN, PC_CPN, PC_ORIG, PC_N };
static const char* k_pcols[PC_N] = {"Item", "Part Number", "Description", "Qty", "References", "Package", "DNP", "Manufacturer P/N", "Customer P/N", "Original P/N"};

void files_open_project_bom(const app::DocPtr& d) {
    if (!d) return;
    const Project& P = d->prj;
    struct Line { std::string pn; bool dnp; std::vector<std::string> refs; };
    std::map<std::pair<std::string, bool>, Line> lines;
    for (auto& c : P.comps) {
        if (c.feature != Feature::Component) continue;
        auto& L = lines[{c.number, c.dnp}];
        L.pn = c.number; L.dnp = c.dnp; L.refs.push_back(c.ref);
    }
    for (auto& [pn, part] : P.parts) {
        bool used = lines.count({pn, false}) || lines.count({pn, true});
        if (!used) { auto& L = lines[{pn, false}]; L.pn = pn; L.dnp = false; }
    }
    std::vector<Line> v;
    for (auto& [k, L] : lines) v.push_back(L);
    std::sort(v.begin(), v.end(), [](const Line& a, const Line& b2) {
        if (a.dnp != b2.dnp) return !a.dnp;
        if (a.pn.empty() != b2.pn.empty()) return !a.pn.empty();
        return natural_cmp(a.pn, b2.pn) < 0;
    });
    std::vector<std::vector<std::string>> blk;
    blk.push_back(std::vector<std::string>(k_pcols, k_pcols + PC_N));
    int item = 0;
    for (auto& L : v) {
        std::sort(L.refs.begin(), L.refs.end(), [](const std::string& a, const std::string& b2) { return natural_cmp(a, b2) < 0; });
        auto it = P.parts.find(L.pn);
        const PartNumber* pp = it == P.parts.end() ? nullptr : &it->second;
        std::vector<std::string> row(PC_N);
        row[PC_ITEM] = std::to_string(++item);
        row[PC_PN] = L.pn.empty() ? std::string() : "'" + L.pn;     // keep "0805"-style numbers as text
        row[PC_DESC] = pp ? pp->description : "";
        row[PC_QTY] = std::to_string(L.refs.size());
        row[PC_REFS] = compress_refs(L.refs);
        row[PC_PKG] = pp ? pp->package : "";
        row[PC_DNP] = L.dnp ? "DNP" : "";
        row[PC_MPN] = pp ? pp->manf_pn : "";
        row[PC_CPN] = pp ? pp->cust_pn : "";
        row[PC_ORIG] = L.pn;
        for (int c : {PC_DESC, PC_REFS, PC_PKG, PC_MPN, PC_CPN, PC_ORIG}) if (!row[c].empty() && row[c][0] != '\'') row[c] = "'" + row[c];   // always text
        blk.push_back(std::move(row));
    }
    Book bk;
    bk.rename_sheet(0, "BOM");
    bk.set_inputs(0, 0, 0, blk);
    int n = (int)blk.size();
    // styling: header, widths, freeze, filter, Qty check, DNP rows grey
    bk.apply_style(0, Range(0, 0, 0, PC_N - 1), [](Style& s) {
        s.font.bold = true; s.font.color = 0xFFFFFFFF; s.fill = Fill{FillPattern::Solid, 0xFF305496, 0};
        s.border.bottom = {BorderStyle::Medium, 0xFF1F3864}; s.valign = VAlign::Center;
    }, "Header");
    bk.set_row_height(0, 0, 0, 20);
    static const double widths[PC_N] = {6, 22, 40, 6, 40, 16, 7, 22, 18, 18};
    for (int c = 0; c < PC_N; ++c) bk.set_col_width(0, c, c, widths[c]);
    bk.apply_style(0, Range(1, PC_REFS, std::max(1, n - 1), PC_REFS), [](Style& s) { s.wrap = true; }, "Wrap");
    bk.apply_style(0, Range(1, PC_ITEM, std::max(1, n - 1), PC_ITEM), [](Style& s) { s.halign = HAlign::Center; s.font.color = 0xFF808080; }, "Item");
    bk.apply_style(0, Range(1, PC_DNP, std::max(1, n - 1), PC_DNP), [](Style& s) { s.halign = HAlign::Center; s.font.bold = true; s.font.color = 0xFFC00000; }, "DNP");
    bk.apply_style(0, Range(0, 0, std::max(1, n - 1), PC_N - 1), [](Style& s) { s.valign = VAlign::Top; }, "Top");
    bk.apply_style(0, Range(0, 0, 0, PC_N - 1), [](Style& s) { s.valign = VAlign::Center; }, "Header");
    bk.apply_style(0, Range(1, 0, std::max(1, n - 1), PC_N - 1), [](Style& s) {
        BorderSide h{BorderStyle::Hair, 0xFFBFBFBF}; s.border.bottom = h;
    }, "Grid");
    bk.set_freeze(0, 1, 2);
    bk.set_autofilter(0, Range(0, 0, std::max(1, n - 1), PC_N - 1));
    bk.hide_cols(0, PC_ORIG, PC_ORIG, true);
    {
        CondFormat cf;
        cf.ranges.push_back(Range(1, PC_QTY, std::max(1, n - 1), PC_QTY));
        CondRule ru; ru.type = CondRule::Expression; ru.f1 = "AND($E2<>\"\",REFCOUNT($E2)<>$D2)";
        Dxf dx; dx.fill_color = 0xFFFFC7CE; dx.font_color = 0xFF9C0006; dx.bold = true;
        bk.dxfs.push_back(dx); ru.dxf = (int)bk.dxfs.size() - 1; ru.priority = 1;
        cf.rules.push_back(ru);
        CondFormat cf2;
        cf2.ranges.push_back(Range(1, 0, std::max(1, n - 1), PC_N - 1));
        CondRule r2; r2.type = CondRule::Expression; r2.f1 = "$G2=\"DNP\"";
        Dxf d2; d2.fill_color = 0xFFF2F2F2; d2.font_color = 0xFF808080; d2.italic = true;
        bk.dxfs.push_back(d2); r2.dxf = (int)bk.dxfs.size() - 1; r2.priority = 2;
        cf2.rules.push_back(r2);
        CondFormat cf3;
        cf3.ranges.push_back(Range(1, PC_PN, std::max(1, n - 1), PC_PN));
        CondRule r3; r3.type = CondRule::ContainsBlanks; r3.priority = 3;
        Dxf d3; d3.fill_color = 0xFFFFEB9C;
        bk.dxfs.push_back(d3); r3.dxf = (int)bk.dxfs.size() - 1;
        cf3.rules.push_back(r3);
        bk.add_cond_format(0, cf); bk.add_cond_format(0, cf2); bk.add_cond_format(0, cf3);
    }
    {
        DataValidation dv; dv.type = DataValidation::List; dv.f1 = "\"DNP\"";
        dv.ranges.push_back(Range(1, PC_DNP, std::max(1, n - 1) + 200, PC_DNP));
        dv.prompt_title = "DNP"; dv.prompt = "Pick DNP to mark the line's references not placed; clear to place them.";
        bk.add_validation(0, dv);
    }
    bk.sheets[0].active_r = 1; bk.sheets[0].active_c = PC_PN;
    bk.clear_undo();
    std::string nm = ui::U(d->display_name());
    size_t k = nm.find_last_of('.');
    if (k != std::string::npos) nm = nm.substr(0, k);
    auto doc = make_sheet_doc_book(std::move(bk), nm + " BOM", L"");
    auto* sd = static_cast<SheetDoc*>(doc.get());
    sd->link_doc = d;
    sd->how = "Project BOM (" + std::to_string(n - 1) + " lines). Edit, then BOM \xE2\x86\x92 Apply to Project.";
    sd->ribbon_tab = 2;
    files_add(std::move(doc));
    toast("Project BOM opened in the spreadsheet. Edit it, then use Apply to Project.", ToastKind::Info, 5.f);
}

// ---------------------------------------------------------------- apply back
struct SheetDoc::BomApplyPlan {
    struct Op {
        enum Kind { Rename, Field, AddPart, Link, Dnp, Warn } kind;
        std::string pn, old_pn, field, value, ref;
        bool flag = false;
    };
    std::vector<Op> ops;            // parallel to bom_diff
};

void SheetDoc::bom_apply_preview() {
    commit_edit();
    auto d = link_doc.lock();
    if (!d) d = app::active_doc();
    if (!d) { toast("No project to apply to", ToastKind::Warning, 3.f); return; }
    const Project& P = d->prj;
    // find columns by header text (works for the generated sheet and for edited / reordered ones)
    BomCols bc = guess_bom_cols();
    int orig = -1;
    const Worksheet& W = ws();
    for (int c = 0; c < W.ncols(); ++c) if (lower(trim(b.display_text(cur, bc.header, c))) == "original p/n") orig = c;
    if (bc.pn < 0 || bc.ref < 0) { toast("Need a Part Number and a References column", ToastKind::Warning, 3.5f); return; }
    // the generated sheet: "Manufacturer P/N" / "Customer P/N" may be matched as P/N — prefer exact "part number"
    int mpn = -1, cpn = -1;
    for (int c = 0; c < W.ncols(); ++c) {
        std::string h = lower(trim(b.display_text(cur, bc.header, c)));
        if (h == "part number" || h == "part no" || h == "p/n" || h == "pn") bc.pn = c;
        if (h == "manufacturer p/n" || h == "mpn" || h == "manufacturer part number") mpn = c;
        if (h == "customer p/n" || h == "cpn") cpn = c;
    }
    auto plan = std::make_shared<BomApplyPlan>();
    bom_diff.clear();
    auto add = [&](BomApplyPlan::Op op, const std::string& kind, const std::string& pn, const std::string& what, const std::string& before, const std::string& after) {
        plan->ops.push_back(op);
        bom_diff.push_back({kind, pn, what, before, after, op.kind != BomApplyPlan::Op::Warn});
    };
    std::set<std::string> renamed_from, sheet_refs, new_parts;
    std::map<std::string, std::string> renames;
    int nr = W.nrows();
    for (int r = bc.header + 1; r < nr; ++r) {
        std::string pn = trim(b.display_text(cur, r, bc.pn));
        std::string refs_s = b.display_text(cur, r, bc.ref);
        std::string op_ = orig >= 0 ? trim(b.display_text(cur, r, orig)) : pn;
        auto refs = io::expand_refs(refs_s, true);
        if (pn.empty() && refs.empty()) continue;
        bool dnp = row_is_dnp(b, cur, r, bc);
        // rename
        if (!op_.empty() && !pn.empty() && op_ != pn && P.parts.count(op_) && !P.parts.count(pn) && !renamed_from.count(op_)) {
            renamed_from.insert(op_); renames[op_] = pn;
            BomApplyPlan::Op o{BomApplyPlan::Op::Rename, pn, op_};
            add(o, "Rename", pn, "Part number", op_, pn);
        }
        const PartNumber* pp = nullptr;
        if (!pn.empty()) {
            auto it = P.parts.find(pn);
            if (it == P.parts.end() && renames.count(op_) && renames[op_] == pn) it = P.parts.find(op_);
            if (it != P.parts.end()) pp = &it->second;
            else if (!new_parts.count(pn)) {
                new_parts.insert(pn);
                BomApplyPlan::Op o{BomApplyPlan::Op::AddPart, pn};
                add(o, "Add", pn, "New part number", "", pn);
            }
        }
        auto field = [&](int col, const char* fname, const std::string& curv) {
            if (col < 0 || pn.empty()) return;
            std::string nv = trim(b.display_text(cur, r, col));
            if (nv == curv) return;
            if (!pp && nv.empty()) return;
            BomApplyPlan::Op o{BomApplyPlan::Op::Field, pn, "", fname, nv};
            add(o, "Update", pn, fname, curv, nv);
        };
        field(bc.desc, "Description", pp ? pp->description : "");
        field(bc.pkg, "Package", pp ? pp->package : "");
        field(mpn, "Manufacturer P/N", pp ? pp->manf_pn : "");
        field(cpn, "Customer P/N", pp ? pp->cust_pn : "");
        // links + DNP
        for (auto& ref : refs) {
            sheet_refs.insert(ref);
            const Component* c = P.find_ref(ref);
            if (!c) { BomApplyPlan::Op o{BomApplyPlan::Op::Warn, pn, "", "", "", ref}; add(o, "Warning", pn, "Reference " + ref + " is not on the board", "", ""); continue; }
            std::string target = pn;
            if (c->number != target && !(renames.count(c->number) && renames[c->number] == target)) {
                BomApplyPlan::Op o{BomApplyPlan::Op::Link, pn, "", "", "", ref};
                add(o, "Link", pn, ref, c->number.empty() ? "(none)" : c->number, target.empty() ? "(none)" : target);
            }
            if (c->dnp != dnp) {
                BomApplyPlan::Op o{BomApplyPlan::Op::Dnp, pn, "", "", "", ref, dnp};
                add(o, "DNP", pn, ref, c->dnp ? "DNP" : "placed", dnp ? "DNP" : "placed");
            }
        }
        // qty check
        if (bc.qty >= 0 && !refs.empty()) {
            const Value& q = W.value(r, bc.qty);
            double qv = q.is_num() ? q.num : atof(b.display_text(cur, r, bc.qty).c_str());
            if ((int)std::lround(qv) != (int)refs.size()) {
                BomApplyPlan::Op o{BomApplyPlan::Op::Warn, pn};
                add(o, "Warning", pn, "Qty " + b.display_text(cur, r, bc.qty) + " but " + std::to_string(refs.size()) + " references", "", "");
            }
        }
    }
    int missing = 0;
    for (auto& c : P.comps) if (c.feature == Feature::Component && !sheet_refs.count(c.ref)) ++missing;
    if (missing) {
        BomApplyPlan::Op o{BomApplyPlan::Op::Warn};
        add(o, "Warning", "", std::to_string(missing) + " board reference(s) are not in the sheet (left unchanged)", "", "");
    }
    bom_plan = plan;
    open_dialog(Dlg::ApplyBom);
}

void SheetDoc::dlg_apply_bom() {
    ImGui::SetNextWindowSize(ImVec2(S(820), S(520)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Apply BOM to Project###sab", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    const GTheme& t = th();
    auto d = link_doc.lock();
    if (!d) d = app::active_doc();
    int on = 0, warn = 0;
    std::map<std::string, int> kinds;
    for (auto& x : bom_diff) { if (x.kind == "Warning") ++warn; else { kinds[x.kind]++; if (x.on) ++on; } }
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "Target: %s", d ? ui::U(d->display_name()).c_str() : "(none)");
    for (auto& [k, n] : kinds) { ImGui::SameLine(0, S(14)); ImGui::Text("%s: %d", k.c_str(), n); }
    if (warn) { ImGui::SameLine(0, S(14)); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.warn), "Warnings: %d", warn); }
    if (bom_diff.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.ok), "The project already matches the sheet.");
    if (button("Check all")) for (auto& x : bom_diff) if (x.kind != "Warning") x.on = true;
    ImGui::SameLine();
    if (button("Uncheck all")) for (auto& x : bom_diff) x.on = false;
    float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - S(14);
    if (ImGui::BeginTable("##diff", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(28));
        ImGui::TableSetupColumn("Change", ImGuiTableColumnFlags_WidthFixed, S(80));
        ImGui::TableSetupColumn("Part number", ImGuiTableColumnFlags_WidthFixed, S(150));
        ImGui::TableSetupColumn("What");
        ImGui::TableSetupColumn("Before");
        ImGui::TableSetupColumn("After");
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)bom_diff.size());
        while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            auto& x = bom_diff[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableNextColumn();
            if (x.kind != "Warning") ImGui::Checkbox("##on", &x.on);
            else icon_image(ui::Icon::Warning, ImGui::GetTextLineHeight());
            ImGui::TableNextColumn();
            ImU32 kc = x.kind == "Warning" ? t.warn : x.kind == "Add" ? t.ok : x.kind == "Rename" ? t.accent : t.text;
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kc), "%s", x.kind.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(x.pn.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(x.what.c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "%s", x.before.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(x.after.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - S(310));
    char lbl[64]; std::snprintf(lbl, sizeof lbl, "Apply %d change%s", on, on == 1 ? "" : "s");
    if (button(lbl, ui::Icon::Check, true, ImVec2(S(200), 0), on > 0 && d != nullptr)) { bom_apply_commit(); ImGui::CloseCurrentPopup(); }
    ImGui::SameLine(0, S(8));
    if (button("Cancel", ui::Icon::None, false, ImVec2(S(96), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void SheetDoc::bom_apply_commit() {
    auto d = link_doc.lock();
    if (!d) d = app::active_doc();
    if (!d || !bom_plan) return;
    Project& P = d->prj;
    d->checkpoint("Apply BOM from spreadsheet");
    int n = 0;
    using Op = BomApplyPlan::Op;
    // order: renames, adds, fields, links, dnp
    for (int pass = 0; pass < 5; ++pass) {
        for (size_t i = 0; i < bom_plan->ops.size() && i < bom_diff.size(); ++i) {
            if (!bom_diff[i].on) continue;
            const Op& o = bom_plan->ops[i];
            int want = o.kind == Op::Rename ? 0 : o.kind == Op::AddPart ? 1 : o.kind == Op::Field ? 2 : o.kind == Op::Link ? 3 : o.kind == Op::Dnp ? 4 : -1;
            if (want != pass) continue;
            switch (o.kind) {
            case Op::Rename: {
                auto it = P.parts.find(o.old_pn);
                if (it == P.parts.end() || P.parts.count(o.pn)) break;
                PartNumber pn = it->second; pn.number = o.pn;
                P.parts.erase(it);
                P.parts[o.pn] = pn;
                for (auto& c : P.comps) if (c.number == o.old_pn) c.number = o.pn;
                ++n; break;
            }
            case Op::AddPart: if (!P.parts.count(o.pn)) { PartNumber pn; pn.number = o.pn; P.parts[o.pn] = pn; ++n; } break;
            case Op::Field: {
                auto& pn = P.parts[o.pn];
                pn.number = o.pn;
                if (o.field == "Description") pn.description = o.value;
                else if (o.field == "Package") pn.package = o.value;
                else if (o.field == "Manufacturer P/N") pn.manf_pn = o.value;
                else if (o.field == "Customer P/N") pn.cust_pn = o.value;
                ++n; break;
            }
            case Op::Link: if (Component* c = P.find_ref(o.ref)) { c->number = o.pn; ++n; } break;
            case Op::Dnp: if (Component* c = P.find_ref(o.ref)) { c->dnp = o.flag; ++n; } break;
            default: break;
            }
        }
    }
    P.link_bom();
    d->changed(ui::CH_ALL);
    // the sheet's "Original P/N" now equals the applied P/N (re-apply is a no-op)
    const Worksheet& W = ws();
    BomCols bc = guess_bom_cols();
    int orig = -1;
    for (int c = 0; c < W.ncols(); ++c) if (lower(trim(b.display_text(cur, bc.header, c))) == "original p/n") orig = c;
    int pncol = -1;
    for (int c = 0; c < W.ncols(); ++c) if (lower(trim(b.display_text(cur, bc.header, c))) == "part number") pncol = c;
    if (orig >= 0 && pncol >= 0) {
        std::vector<std::vector<std::string>> blk;
        for (int r = bc.header + 1; r < W.nrows(); ++r) blk.push_back({b.input_text(cur, r, pncol)});
        if (!blk.empty()) b.set_inputs(cur, bc.header + 1, orig, blk);
    }
    toast("Applied " + std::to_string(n) + " change(s) to " + ui::U(d->display_name()), ToastKind::Success, 4.f, "Show BOM", [d]() { files_set_active(false); activate_view(d, ViewKind::Bom); });
    bom_plan.reset();
}

} // namespace pcbp::gui
