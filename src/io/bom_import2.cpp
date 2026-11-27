// ============================================================================
// bom_import2.cpp — BOM import on the text_import engine. See bom_import2.h.
// ============================================================================
#include "bom_import2.h"
#include "m1_bom.h"
#include "../core/package_name.h"
#include "io_util.h"
#include <algorithm>
#include <map>
#include <set>
#include <cctype>

namespace pcbp::io {

const char* row_reason_name(RowReason r) {
    switch (r) {
    case RowReason::Data: return "included";
    case RowReason::ForcedIn: return "included (by you)";
    case RowReason::Header: return "column header";
    case RowReason::BeforeFirst: return "excluded: above First Data Row";
    case RowReason::AfterLast: return "excluded: below Last Data Row";
    case RowReason::UserExcluded: return "excluded by you";
    case RowReason::Blank: return "excluded: blank row";
    case RowReason::Category: return "excluded: category / section title";
    case RowReason::Note: return "excluded: no reference (note or alternate)";
    case RowReason::Footer: return "excluded: footer after the last reference";
    case RowReason::Comment: return "excluded: comment line";
    case RowReason::RepeatedHeader: return "excluded: repeated header";
    }
    return "?";
}
const char* row_reason_badge(RowReason r) {
    switch (r) {
    case RowReason::Data: return "";
    case RowReason::ForcedIn: return "IN";
    case RowReason::Header: return "HDR";
    case RowReason::BeforeFirst: return "TOP";
    case RowReason::AfterLast: return "END";
    case RowReason::UserExcluded: return "OFF";
    case RowReason::Blank: return "BLANK";
    case RowReason::Category: return "CAT";
    case RowReason::Note: return "NOTE";
    case RowReason::Footer: return "FOOT";
    case RowReason::Comment: return "CMT";
    case RowReason::RepeatedHeader: return "HDR";
    }
    return "";
}
const char* bom_profile_name(BomProfile p) {
    switch (p) {
    case BomProfile::KiCadNative: return "KiCad native BOM";
    case BomProfile::KiCadJlcpcb: return "KiCad / JLCPCB assembly BOM";
    default: return "Generic BOM";
    }
}

// Check the actual headers instead of assuming that any four-column CSV is
// KiCad.  Role assignments alone cannot distinguish a supplier from an MPN.
static BomProfile detect_bom_profile(const ParsedText& t) {
    std::set<std::string> h;
    for (auto& c : t.cols) h.insert(norm_header(c.header));
    const auto has = [&](const char* k) { return h.count(k) != 0; };
    const bool lcsc = has("lcsc") || has("lcscpart") || has("lcscpartnumber") ||
                      has("jlcpcbpart") || has("jlcpcbpartnumber");
    if (has("comment") && has("designator") && has("footprint") && lcsc)
        return BomProfile::KiCadJlcpcb;
    if ((has("reference") || has("references")) && has("value") &&
        has("footprint") && (has("qty") || has("quantity")))
        return BomProfile::KiCadNative;
    return BomProfile::Generic;
}

static bool lcsc_code(const std::string& v) {
    if (v.size() < 2 || v.size() > 14 || (v[0] != 'C' && v[0] != 'c')) return false;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] < '0' || v[i] > '9') return false;
    return true;
}

const char* issue_sev_name(IssueSev s) { return s == IssueSev::Error ? "Error" : s == IssueSev::Warning ? "Warning" : "Info"; }

bool BomPreview::mapping_ok() const {
    if (text.col(Role::Ref) < 0) return false;
    return text.col(Role::PartNumber) >= 0 || text.col(Role::Value) >= 0 || text.col(Role::MPN) >= 0 ||
           (profile == BomProfile::KiCadJlcpcb && text.col(Role::VendorPN) >= 0);
}
const BomRowStatus* BomPreview::status_of_line(int raw) const {
    if (raw < 0 || raw >= (int)row_status.size()) return nullptr;
    return &row_status[raw];
}
int BomPreview::included_rows() const { int n = 0; for (auto& s : row_status) n += s.included; return n; }

// ---------------------------------------------------------------------------
// fields / DNP
// ---------------------------------------------------------------------------
bool m1_is_board_class(const std::string& cls) {
    std::string u;
    for (unsigned char c : cls) if (std::isalnum(c)) u += (char)std::toupper(c);
    return u == "PCB" || u == "PWB" || u == "BOARD" || u == "BAREBOARD" || u == "RAWPCB";
}


std::vector<Role> bom_wizard_roles() {
    return {Role::Unused, Role::LineItem, Role::Ref, Role::Manufacturer, Role::MPN, Role::PartNumber, Role::Qty,
            Role::Package, Role::Process, Role::Dnp, Role::Description, Role::Value,
            // less common, still BOM data
            Role::Vendor, Role::VendorPN, Role::CustPN, Role::Polarity, Role::PartRev, Role::Bin};
}
const char* bom_role_label(Role r) {
    switch (r) {
    case Role::Unused: return "(not imported)";
    case Role::LineItem: return "Line Item";
    case Role::Ref: return "Reference";
    case Role::Manufacturer: return "Manufacturer";
    case Role::MPN: return "MPN";
    case Role::PartNumber: return "Part Number";
    case Role::Qty: return "Quantity";
    case Role::Package: return "Package";
    case Role::Process: return "Process";
    case Role::Dnp: return "DNP";
    case Role::Description: return "Description";
    case Role::Value: return "Value";
    case Role::Vendor: return "Supplier";
    case Role::VendorPN: return "Supplier P/N";
    case Role::CustPN: return "Customer P/N";
    case Role::Polarity: return "Polarized";
    case Role::PartRev: return "Part Revision";
    case Role::Bin: return "Location / Bin";
    default: return role_name(r);
    }
}
bool bom_role_numeric(Role r) { return r == Role::Qty; }

bool bom_text_is_dnp(const std::string& s, const std::vector<std::string>& tokens, bool words_only_long) {
    std::string u = to_upper(trim(s));
    if (u.empty()) return false;
    // strip surrounding brackets "(DNP)" "[DNI]"
    while (u.size() > 2 && (u.front() == '(' || u.front() == '[') && (u.back() == ')' || u.back() == ']')) u = trim(u.substr(1, u.size() - 2));
    for (auto& t0 : tokens) {
        std::string t = to_upper(trim(t0));
        if (t.empty()) continue;
        if (u == t) return true;
        if (words_only_long && t.size() < 3) continue;     // "NS" only as a whole cell
        for (size_t p = u.find(t); p != std::string::npos; p = u.find(t, p + 1)) {
            bool l = p == 0 || !std::isalnum((unsigned char)u[p - 1]);
            bool r = p + t.size() >= u.size() || !std::isalnum((unsigned char)u[p + t.size()]);
            if (l && r) return true;
        }
    }
    return false;
}

// DNP / Option column cell: token list, else a boolean flag (Yes/No/X/1, inverted for "Fitted").
static bool dnp_column_cell(const std::string& s, bool invert, const std::vector<std::string>& tokens, bool& understood) {
    understood = true;
    std::string u = to_upper(trim(s));
    if (u.empty()) return false;           // empty cell = populated (also for "Fitted" columns)
    if (bom_text_is_dnp(u, tokens, false)) return true;
    if (auto f = parse_flag(u)) return invert ? !*f : *f;
    understood = false;
    return false;
}

// ---------------------------------------------------------------------------
// row model
// ---------------------------------------------------------------------------
static void classify_rows(BomPreview& p, const BomOptions2& opt) {
    ParsedText& t = p.text;
    const BomRowModel& M = opt.rows;
    const int nl = (int)t.lines.size();
    p.row_status.assign(nl, BomRowStatus{});
    for (int i = 0; i < nl; ++i) { p.row_status[i].line = i; p.row_status[i].included = false; p.row_status[i].reason = RowReason::Blank; }
    const int cr = t.col(Role::Ref), cpn = t.col(Role::PartNumber), cm = t.col(Role::MPN), cv = t.col(Role::Value);
    p.effective_header = M.header_row >= 0 ? M.header_row : t.header_line;
    const int hdr_end = p.effective_header >= 0 ? p.effective_header + std::max(1, t.header_rows) : 0;
    p.effective_first = M.first_data >= 0 ? M.first_data : hdr_end;
    // last line holding references (footer detection)
    int last_ref = -1;
    for (auto& r : t.rows)
        if ((r.kind == RowKind::Data || M.included.count(r.line)) && cr >= 0 && cr < (int)r.cells.size() && !trim(r.cells[cr]).empty() &&
            r.line >= p.effective_first && (M.last_data < 0 || r.line <= M.last_data))
            last_ref = std::max(last_ref, r.line);
    p.effective_last = M.last_data >= 0 ? M.last_data : (last_ref >= 0 ? last_ref : nl - 1);

    for (size_t i = 0; i < t.rows.size(); ++i) {
        Row& r = t.rows[i];
        if (r.line < 0 || r.line >= nl) continue;
        BomRowStatus& st = p.row_status[r.line];
        st.src = (int)i;
        for (int k = 1; k < r.line_count && r.line + k < nl; ++k) { p.row_status[r.line + k].src = (int)i; p.row_status[r.line + k].reason = RowReason::Data; }
        auto set = [&](RowReason why, bool inc) { st.reason = why; st.included = inc; };
        if (r.line >= p.effective_header && r.line < hdr_end && p.effective_header >= 0) { set(RowReason::Header, false); continue; }
        if (r.line < p.effective_first) { set(RowReason::BeforeFirst, false); continue; }
        if (M.last_data >= 0 && r.line > M.last_data) { set(RowReason::AfterLast, false); continue; }
        if (M.excluded.count(r.line)) { set(RowReason::UserExcluded, false); continue; }
        if (M.included.count(r.line)) {
            if (r.cells.size() < t.ncols()) r.cells.resize(t.ncols());
            set(RowReason::ForcedIn, true);
            continue;
        }
        if (r.kind == RowKind::Comment) { set(RowReason::Comment, false); continue; }
        if (r.kind == RowKind::RepeatedHeader || r.kind == RowKind::Header) { set(RowReason::RepeatedHeader, false); continue; }
        if (r.kind == RowKind::Preamble) { set(RowReason::BeforeFirst, false); continue; }
        if (r.kind == RowKind::Blank) { set(RowReason::Blank, false); continue; }
        if (r.kind != RowKind::Data) { set(r.line > last_ref ? RowReason::Footer : RowReason::Note, false); continue; }
        if (!opt.rows.auto_exclude) { set(RowReason::Data, true); continue; }
        int nonempty = 0;
        for (auto& c : r.cells) nonempty += !trim(c).empty();
        if (nonempty == 0) { set(RowReason::Blank, false); continue; }
        if (r.line < (int)p.merged.size() && p.merged[r.line]) { set(RowReason::Category, false); continue; }
        bool has_ref = cr >= 0 && cr < (int)r.cells.size() && !trim(r.cells[cr]).empty();
        if (cr >= 0 && !has_ref) {
            if (r.line > last_ref) set(RowReason::Footer, false);
            else if (nonempty == 1) set(RowReason::Category, false);
            else set(RowReason::Note, false);
            continue;
        }
        if (cr < 0) {   // nothing mapped yet: a lone text cell is still a title
            bool has_pn = (cpn >= 0 && !trim(r.cells[cpn]).empty()) || (cm >= 0 && !trim(r.cells[cm]).empty()) || (cv >= 0 && !trim(r.cells[cv]).empty());
            if (nonempty == 1 && !has_pn) { set(RowReason::Category, false); continue; }
        }
        set(RowReason::Data, true);
    }
    // blank raw lines that are not records stay "Blank"; lines before first data are "above"
    for (int i = 0; i < nl; ++i) {
        BomRowStatus& st = p.row_status[i];
        if (st.src >= 0) continue;
        if (i == p.effective_header) st.reason = RowReason::Header;
        else if (i < p.effective_first) st.reason = RowReason::BeforeFirst;
        else if (M.last_data >= 0 && i > M.last_data) st.reason = RowReason::AfterLast;
    }
}

void bom_rebuild(BomPreview& p, const BomOptions2& opt) {
    ParsedText& t = p.text;
    assign_roles(t, opt.layout, Purpose::Bom);
    p.profile = detect_bom_profile(t);
    p.lines.clear(); p.by_comp.clear(); p.by_part.clear(); p.issues.clear();
    p.pn_conflicts.clear(); p.duplicate_refs.clear(); p.qty_mismatches = 0; p.board_pn.clear();
    // engine diagnostics ("delimiter detected: TAB") are information, not problems
    for (auto& n : t.notes) p.issues.push_back({0, n, IssueSev::Info});
    classify_rows(p, opt);
    const int cr = t.col(Role::Ref), cpn = t.col(Role::PartNumber), cq = t.col(Role::Qty), cd = t.col(Role::Dnp);
    if (cr < 0) p.issues.push_back({0, "No column is mapped to Reference / Designator: nothing can be linked to the board.", IssueSev::Error});
    if (cpn < 0) {
        if (t.col(Role::MPN) >= 0) p.issues.push_back({0, "No Part Number column: the Manufacturer Part Number is used as the part number.", IssueSev::Info});
        else if (p.profile == BomProfile::KiCadJlcpcb)
            p.issues.push_back({0, "KiCad/JLCPCB BOM: LCSC supplier codes are used as internal part identifiers (not as MPNs).", IssueSev::Info});
        else if (opt.synth_pn) p.issues.push_back({0, "No part number column: part numbers are built from value + package.", IssueSev::Info});
        else p.issues.push_back({0, "No part number column.", IssueSev::Error});
    }
    auto get = [&](const Row& r, Role role) { return trim(t.cell(r, role)); };
    RefExpandOptions ro = opt.ref_limits;
    ro.upper = opt.upper_refs;
    std::map<std::string, int> unknown_option;   // Option values not understood

    for (auto& st : p.row_status) {
        if (!st.included || st.src < 0 || st.line != t.rows[st.src].line) continue;
        const Row& r = t.rows[st.src];
        BomLine b;
        b.line = r.line + 1;
        b.src = st.src;
        b.item = get(r, Role::LineItem);
        b.pn = get(r, Role::PartNumber);
        b.package = get(r, Role::Package);
        b.value = get(r, Role::Value);
        b.description = get(r, Role::Description);
        b.manufacturer = get(r, Role::Manufacturer);
        b.mpn = get(r, Role::MPN);
        b.vendor = get(r, Role::Vendor);
        b.vendor_pn = get(r, Role::VendorPN);
        b.process = get(r, Role::Process);
        std::string refs = cr >= 0 && cr < (int)r.cells.size() ? r.cells[cr] : std::string();
        if (opt.expand_refs) {
            RefExpansion ex = expand_refs_ex(refs, ro);
            b.refs = std::move(ex.refs);
            for (auto& x : ex.issues)
                p.issues.push_back({b.line, x.kind == RefIssue::Duplicate ? "reference " + x.token + " appears twice on this line"
                                                                           : "references: " + x.msg,
                                    x.error ? IssueSev::Error : IssueSev::Info});
        } else if (!trim(refs).empty()) {
            b.refs.push_back(opt.upper_refs ? to_upper(trim(refs)) : trim(refs));
        }
        if (b.refs.empty() && b.pn.empty() && b.value.empty() && b.mpn.empty()) continue;
        if (b.refs.empty() && m1_is_board_class(b.process)) {   // ERP job BOM: the bare-board line (class PCB, no refs)
            if (p.board_pn.empty()) p.board_pn = b.pn;
            p.issues.push_back({b.line, "Bare board line (class " + b.process + ", part " + b.pn + "): not a placement line, skipped.", IssueSev::Info});
            continue;
        }
        if (cd >= 0) {
            bool understood = true;
            b.dnp_text = trim(r.cells[cd]);
            b.dnp = dnp_column_cell(r.cells[cd], t.cols[cd].invert, opt.dnp_tokens, understood);
            if (!understood) ++unknown_option[to_upper(b.dnp_text)];
        }
        if (!b.dnp && opt.dnp_from_text)
            b.dnp = bom_text_is_dnp(b.value, opt.dnp_tokens) || bom_text_is_dnp(b.description, opt.dnp_tokens) || bom_text_is_dnp(b.pn, opt.dnp_tokens);
        if (opt.dnp == DnpMode::Ignore) b.dnp = false;
        // Supplier enrichment is opt-in, uses a verified exact-match response,
        // and fills blanks ONLY.  KiCad's library-qualified footprint is never
        // overwritten by the supplier's package label (e.g. 0603 vs 1608Metric).
        if (p.profile == BomProfile::KiCadJlcpcb && lcsc_code(b.vendor_pn)) {
            b.vendor_pn = to_upper(b.vendor_pn);
            if (const auto it = opt.verified_lookups.find(b.vendor_pn); it != opt.verified_lookups.end() &&
                to_upper(it->second.code) == b.vendor_pn) {
                const BomLookup& f = it->second;
                if (b.manufacturer.empty()) b.manufacturer = f.manufacturer;
                if (b.mpn.empty()) b.mpn = f.mpn;
                if (b.description.empty()) b.description = f.description;
                // Do not turn a distributor package label into a KiCad land
                // pattern automatically.  It is only a review suggestion.
                if (b.package.empty() && !f.package.empty())
                    p.issues.push_back({b.line, "No KiCad footprint. Supplier lists package " + f.package + "; select/verify a matching land pattern.", IssueSev::Warning});
            }
        }
        if (b.pn.empty() && cpn < 0 && !b.dnp) {     // never synthesise a shared P/N for DNP lines
            // Keep LCSC identity stable before and after lookup; MPN is metadata.
            if (p.profile == BomProfile::KiCadJlcpcb && lcsc_code(b.vendor_pn)) b.pn = "LCSC:" + b.vendor_pn;
            else if (!b.mpn.empty()) b.pn = b.mpn;
            else if (opt.synth_pn && !b.value.empty()) b.pn = b.package.empty() ? b.value : b.value + "_" + b.package;
            b.pn_synth = !b.pn.empty();
        }
        if (b.pn.empty() && !b.dnp)
            p.issues.push_back({b.line, "No part number" + (b.refs.empty() ? std::string() : " for " + b.refs[0] + (b.refs.size() > 1 ? " ..." : "")) +
                                        ": these references will stay Unassigned.", IssueSev::Warning});
        if (b.refs.empty()) p.issues.push_back({b.line, "Part " + (b.pn.empty() ? b.mpn : b.pn) + " has no references: only the part library is updated.", IssueSev::Warning});
        if (cq >= 0) {
            auto q = parse_number(r.cells[cq], t.decimal_comma);
            if (q && *q >= 0) {
                b.qty = (int)(*q + 0.5);
                if (opt.qty_check && !b.refs.empty() && b.qty != (int)b.refs.size()) {
                    b.qty_mismatch = true;
                    ++p.qty_mismatches;
                    p.issues.push_back({b.line, "Quantity says " + std::to_string(b.qty) + " but " + std::to_string(b.refs.size()) +
                                                    " references are listed (" + trim(refs) + "). Check the BOM line.", IssueSev::Warning});
                }
            } else if (!trim(r.cells[cq]).empty())
                p.issues.push_back({b.line, "Quantity \"" + trim(r.cells[cq]) + "\" is not a number.", IssueSev::Warning});
        }
        p.lines.push_back(std::move(b));
    }
    // bare-board line (ERP job BOM, class PCB, no refs) excluded automatically as a note row: say so (Info)
    if (const int cpc = t.col(Role::Process); cpc >= 0)
        for (auto& st : p.row_status) {
            if (st.included || st.src < 0 || st.line != t.rows[st.src].line) continue;
            const Row& r = t.rows[st.src];
            if (r.kind != RowKind::Data || cpc >= (int)r.cells.size() || !m1_is_board_class(r.cells[cpc])) continue;
            if (cr >= 0 && cr < (int)r.cells.size() && !trim(r.cells[cr]).empty()) continue;
            const std::string bpn = get(r, Role::PartNumber);
            if (p.board_pn.empty()) p.board_pn = bpn;
            p.issues.push_back({r.line + 1, "Bare board line (class " + trim(r.cells[cpc]) + ", part " + bpn + "): not a placement line, skipped.", IssueSev::Info});
        }
    for (auto& [v, n] : unknown_option)
        p.issues.push_back({0, "Assembly option \"" + v + "\" (" + std::to_string(n) + " line" + (n > 1 ? "s" : "") +
                               ") is not a recognised DNP indicator; those lines are populated.", IssueSev::Info});

    // ---- views ----
    std::map<std::string, size_t> comp_idx;     // ref -> by_comp
    std::map<std::string, size_t> part_idx;     // pn -> by_part
    std::set<std::string> dup;
    for (auto& b : p.lines) {
        if (!b.pn.empty()) {
            // DNP lines are listed under their own P/N row and never merged into populated ones
            std::string key = b.dnp ? "\x01" + b.pn : b.pn;
            auto it = part_idx.find(key);
            if (it == part_idx.end()) {
                part_idx[key] = p.by_part.size();
                BomPartRow pr;
                pr.pn = b.pn; pr.package = b.package; pr.description = b.description; pr.value = b.value; pr.mpn = b.mpn; pr.dnp = b.dnp;
                p.by_part.push_back(pr);
                it = part_idx.find(key);
            }
            BomPartRow& pr = p.by_part[it->second];
            ++pr.lines;
            if (b.qty >= 0) pr.qty_file = (pr.qty_file < 0 ? 0 : pr.qty_file) + b.qty;
            if (pr.package.empty()) pr.package = b.package;
            if (pr.description.empty()) pr.description = b.description;
            for (auto& r : b.refs) pr.refs.push_back(r);
        }
        for (auto& r : b.refs) {
            std::string key = to_upper(r);
            auto f = comp_idx.find(key);
            if (f == comp_idx.end()) {
                comp_idx[key] = p.by_comp.size();
                p.by_comp.push_back({r, b.pn, b.package, b.description, b.line, b.dnp, false});
                continue;
            }
            BomCompRow& c = p.by_comp[f->second];
            if (dup.insert(key).second) p.duplicate_refs.push_back(r);
            if (c.pn != b.pn || c.dnp != b.dnp) {
                c.conflict = true;
                p.pn_conflicts.push_back(r + ": " + (c.pn.empty() ? "(none)" : c.pn) + (c.dnp ? " DNP" : "") + " (line " + std::to_string(c.line) + ") vs " +
                                         (b.pn.empty() ? "(none)" : b.pn) + (b.dnp ? " DNP" : "") + " (line " + std::to_string(b.line) + ")");
                if (!opt.multiple_pn_check) { c.pn = b.pn; c.package = b.package; c.description = b.description; c.line = b.line; c.dnp = b.dnp; }
            }
        }
    }
    for (auto& pr : p.by_part) std::sort(pr.refs.begin(), pr.refs.end(), ref_less);
    std::sort(p.by_comp.begin(), p.by_comp.end(), [](const BomCompRow& a, const BomCompRow& b) { return ref_less(a.ref, b.ref); });
    for (auto& d : p.duplicate_refs) {
        bool conflict = false;
        for (auto& c : p.pn_conflicts) if (c.compare(0, d.size() + 1, d + ":") == 0) conflict = true;
        if (!conflict) p.issues.push_back({0, "Reference " + d + " is listed on more than one line (same part number).", IssueSev::Warning});
    }
    for (auto& c : p.pn_conflicts)
        p.issues.push_back({0, "Reference listed with different parts: " + c + (opt.multiple_pn_check ? ". The first line is used." : ". The last line is used."),
                            IssueSev::Error});
    std::stable_sort(p.issues.begin(), p.issues.end(), [](const BomIssue& a, const BomIssue& b) { return (int)a.sev > (int)b.sev; });
}

static ImportLayout effective_layout(const BomOptions2& opt) {
    ImportLayout L = opt.layout;
    if (opt.rows.header_row >= 0) L.header_line = opt.rows.header_row;
    if (L.header_rows <= 0) L.header_rows = 1;      // BOM headers are one row; a section title below it is data-area
    if (opt.rows.last_data >= 0) L.stop_line = opt.rows.last_data + 1;
    return L;
}

static void bom_csv_cell(std::string& o, const std::string& s) {
    const bool quoted = s.find_first_of(",\r\n\"") != std::string::npos;
    if (quoted) o += '"';
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    if (quoted) o += '"';
}

std::string bom_completed_csv(const BomPreview& p, const BomOptions2& opt) {
    std::string out = "Comment,Designator,Footprint,LCSC,Manufacturer,MPN,Supplier Package,Description,Datasheet,Lookup Status\r\n";
    for (const BomLine& b : p.lines) {
        // One row per BOM line, retaining grouped references from the original.
        std::string refs;
        for (const auto& r : b.refs) { if (!refs.empty()) refs += ", "; refs += r; }
        const auto it = opt.verified_lookups.find(b.vendor_pn);
        const bool checked = it != opt.verified_lookups.end() && it->second.code == b.vendor_pn;
        const std::string supplier_pkg = checked ? it->second.package : std::string();
        const std::string datasheet = checked ? it->second.datasheet : std::string();
        const std::vector<std::string> cols = {
            b.value, refs, b.package, b.vendor_pn, b.manufacturer, b.mpn,
            supplier_pkg, b.description, datasheet,
            checked ? "Catalog verified" : (b.vendor_pn.empty() ? "No LCSC code" : "Not looked up")
        };
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i) out += ',';
            bom_csv_cell(out, cols[i]);
        }
        out += "\r\n";
    }
    return out;
}

bool bom_preview(const std::string& bytes, const BomOptions2& opt, BomPreview& out, std::string& err) {
    out = BomPreview{};
    if (!analyze_text(bytes, effective_layout(opt), Purpose::Bom, out.text, err)) return false;
    bom_rebuild(out, opt);
    return true;
}

bool bom_preview_src(const BomSource& src, const BomOptions2& opt, BomPreview& out, std::string& err) {
    out = BomPreview{};
    BomOptions2 o = opt;
    if (src.kind != "text" && o.layout.mode == SplitMode::Auto && o.layout.sep == Sep::Auto) {
        o.layout.mode = SplitMode::Delimited;           // spreadsheet cells: tab separated, never re-split
        o.layout.delims = "\t";
        o.layout.quote = 0;                             // cell text is literal
    }
    if (!analyze_text(src.text, effective_layout(o), Purpose::Bom, out.text, err)) return false;
    out.merged = src.merged;
    bom_rebuild(out, o);
    return true;
}

bool bom_preview_file(const std::string& path, const BomOptions2& opt, BomPreview& out, std::string& err) {
    std::string bytes;
    if (!read_file(path, bytes, err)) return false;
    return bom_preview(bytes, opt, out, err);
}

// ---------------------------------------------------------------------------
// apply
// ---------------------------------------------------------------------------
bool bom_apply(const BomPreview& p, Project& prj, const BomOptions2& opt, BomReport& rep, std::string& err) {
    rep = BomReport{};
    if (!p.mapping_ok()) { err = "BOM mapping incomplete (need Reference and Part Number)"; return false; }
    const ParsedText& t = p.text;
    for (auto& c : t.cols) rep.table.columns.push_back(c.header);
    for (auto& b : p.lines) { rep.table.rows.push_back(t.rows[b.src].cells); rep.table.row_line.push_back(b.line); }
    for (size_t i = 0; i < t.cols.size(); ++i)
        if (t.cols[i].role == Role::Unused) rep.unmapped_columns.push_back(t.cols[i].header.empty() ? "Col" + std::to_string(i + 1) : t.cols[i].header);
    rep.map.refs = t.col(Role::Ref); rep.map.number = t.col(Role::PartNumber); rep.map.package = t.col(Role::Package);
    rep.map.description = t.col(Role::Description); rep.map.qty = t.col(Role::Qty); rep.map.dnp = t.col(Role::Dnp);
    rep.map.value = t.col(Role::Value);
    rep.map.process = t.col(Role::Process); rep.map.polarity = t.col(Role::Polarity); rep.map.soft_rev = t.col(Role::SoftRev);
    rep.map.cust_pn = t.col(Role::CustPN); rep.map.part_rev = t.col(Role::PartRev); rep.map.socket = t.col(Role::Socket);
    rep.map.height = t.col(Role::Height); rep.map.cost = t.col(Role::Cost); rep.map.stock_code = t.col(Role::StockCode);
    rep.map.bin = t.col(Role::Bin); rep.map.line_item = t.col(Role::LineItem); rep.map.pop_type = t.col(Role::PopType);
    rep.map.part_marking = t.col(Role::PartMarking);
    rep.table.has_header = t.header_line >= 0;
    rep.table.header_line = t.header_line + 1;
    rep.table.delim = t.sep == Sep::Fixed ? Delim::Fixed : t.sep == Sep::Comma ? Delim::Comma : t.sep == Sep::Semicolon ? Delim::Semicolon
                    : t.sep == Sep::Tab ? Delim::Tab : t.sep == Sep::Pipe ? Delim::Pipe : Delim::Whitespace; rep.map.manf_pn = t.col(Role::MPN); rep.map.vend_pn = t.col(Role::VendorPN);
    for (auto& i : p.issues) if (i.sev != IssueSev::Info) rep.messages.push_back(i.line ? "line " + std::to_string(i.line) + ": " + i.msg : i.msg);
    rep.pn_conflicts = p.pn_conflicts;
    rep.duplicate_refs = p.duplicate_refs;

    if (opt.replace_parts) prj.parts.clear();
    // ---- part library (DNP lines never create or change library parts) ----
    std::set<std::string> touched;
    for (auto& b : p.lines) {
        ++rep.rows;
        if (b.pn.empty() || b.dnp) continue;
        const Row& r = t.rows[b.src];
        bool is_new = !prj.parts.count(b.pn);
        PartNumber& pn = prj.parts[b.pn];
        pn.number = b.pn;
        auto setf = [&](std::string& f, Role role) { std::string v = trim(t.cell(r, role)); if (!v.empty()) f = v; };
        setf(pn.description, Role::Description);
        if (pn.description.empty()) setf(pn.description, Role::Value);
        if (opt.commit_packages) setf(pn.package, Role::Package);   // explicit only (REVIEW_V9 §11G)
        setf(pn.default_process, Role::Process);
        if (pn.pop_type.empty()) {   // SMD / TH class -> the component process flag (Project::process_of)
            std::string pv = trim(t.cell(r, Role::Process));
            if (process_from_name(pv) != Process::Auto) pn.pop_type = pv;
        }
        setf(pn.cust_pn, Role::CustPN);
        setf(pn.vend_pn, Role::VendorPN);
        setf(pn.manf_pn, Role::MPN);
        setf(pn.manufacturer, Role::Manufacturer);
        // bom_rebuild has applied only verified supplier enrichment to blanks.
        // Persist it to the EXISTING Project::parts (.pcbp JSON), no SQL cache.
        if (pn.manf_pn.empty()) pn.manf_pn = b.mpn;
        if (pn.manufacturer.empty()) pn.manufacturer = b.manufacturer;
        if (pn.description.empty()) pn.description = b.description;
        setf(pn.soft_ver, Role::SoftRev);
        setf(pn.part_rev, Role::PartRev);
        setf(pn.stock_code, Role::StockCode);
        setf(pn.bin, Role::Bin);
        setf(pn.line_item, Role::LineItem);
        setf(pn.pop_type, Role::PopType);
        setf(pn.part_marking, Role::PartMarking);
        if (auto v = parse_flag(t.cell(r, Role::Polarity))) pn.polarized = *v;
        else if (!trim(t.cell(r, Role::Polarity)).empty())
            rep.messages.push_back("line " + std::to_string(b.line) + ": polarity \"" + trim(t.cell(r, Role::Polarity)) + "\" not understood");
        if (auto v = parse_flag(t.cell(r, Role::Socket))) pn.socket = *v;
        if (auto h = parse_length(t.cell(r, Role::Height), t.decimal_comma, nullptr)) pn.height = *h;
        if (auto c = parse_number(t.cell(r, Role::Cost), t.decimal_comma)) pn.cost = *c;
        if (is_new) ++rep.parts_added;
        else if (!touched.count(b.pn)) ++rep.parts_updated;
        touched.insert(b.pn);
    }
    rep.refs_total = (int)p.by_comp.size();
    if (opt.link == BomLinkMode::PartsOnly) { prj.dirty = true; return true; }

    // ---- link ----
    if (opt.link == BomLinkMode::Relink)
        for (auto& c : prj.comps) {
            if (c.feature != Feature::Component) continue;
            c.number.clear(); c.pn_src = DataSrc::Unknown; c.bom_package.clear();
            if (c.bom_state == BomState::Dnp) c.dnp = false;
            c.bom_state = BomState::NotChecked;
        }
    std::set<const Component*> linked, listed;
    int dnp_n = 0;
    for (auto& cr : p.by_comp) {
        // R17a (REVIEW_V16 §14): a panelised board repeats references (U1 on every circuit); the BOM
        // line of U1 applies to every placement with that reference, not only the first one.
        std::vector<Component*> targets;
        for (auto& k : prj.comps) if (ieq(k.ref, cr.ref)) targets.push_back(&k);
        if (targets.empty()) { rep.refs_not_on_board.push_back(cr.ref); continue; }
        for (Component* c : targets) {
        listed.insert(c);
        c->bom_package = cr.package;
        if (cr.dnp && opt.dnp != DnpMode::Ignore) {
            ++dnp_n;
            c->bom_state = BomState::Dnp;
            if (opt.dnp == DnpMode::Skip) continue;
            // DNP: keep identity (ref, position, package) + excluded status; no active P/N link
            c->dnp = true;
            c->number.clear();
            c->pn_src = DataSrc::Bom;
            if (c->assigned_point >= 0) c->assigned_point = -1;
            continue;
        }
        c->bom_state = BomState::Listed;
        if (opt.dnp != DnpMode::Ignore) c->dnp = false;
        if (cr.pn.empty()) continue;     // stays Unassigned
        c->number = cr.pn;
        c->pn_src = DataSrc::Bom;
        // package: compared only against a value the BOM actually supplied
        if (!cr.package.empty()) {
            if (c->package.empty()) { c->package = cr.package; c->pkg_src = DataSrc::Bom; }
            else if (!packages_equivalent(c->package, cr.package)) {   // "0402" == "C0402" == "CAPC1005X55N"
                rep.package_mismatch.push_back(c->ref + ": board " + c->package + ", BOM " + cr.package);
                if (opt.overwrite_package) { c->package = cr.package; c->pkg_src = DataSrc::Bom; }
            }
        }
        linked.insert(c);
        }   // targets
    }
    for (auto& c : prj.comps) {
        if (c.feature != Feature::Component) continue;
        ++rep.board_components;
        if (!listed.count(&c)) {
            // Orphan: on the board, not in this BOM. Never made DNP automatically.
            rep.comps_not_in_bom.push_back(c.ref.empty() ? "(id " + std::to_string(c.id) + ")" : c.ref);
            if (c.bom_state == BomState::Dnp) c.dnp = false;
            c.bom_state = BomState::Orphan;
            c.bom_package.clear();
        }
    }
    if (dnp_n) rep.messages.push_back(std::to_string(dnp_n) + " reference(s) marked DNP by the BOM (kept on the board, excluded from placement)");
    rep.linked = (int)linked.size();
    auto srt = [](std::vector<std::string>& v) { std::sort(v.begin(), v.end(), ref_less); };
    srt(rep.refs_not_on_board); srt(rep.comps_not_in_bom); srt(rep.duplicate_refs);
    prj.dirty = true;
    return true;
}

int bom_commit_packages(Project& prj) {
    std::map<std::string, std::string> want;
    std::set<std::string> conflict;
    for (auto& c : prj.comps) {
        if (c.bom_state != BomState::Listed || c.number.empty() || c.bom_package.empty()) continue;
        auto [it, ins] = want.emplace(c.number, c.bom_package);
        if (!ins && !ieq(it->second, c.bom_package)) conflict.insert(c.number);
    }
    int n = 0;
    for (auto& [pn, pkg] : want) {
        if (conflict.count(pn)) continue;
        auto it = prj.parts.find(pn);
        if (it == prj.parts.end() || it->second.package == pkg) continue;
        it->second.package = pkg;
        ++n;
    }
    if (n) prj.dirty = true;
    return n;
}

bool import_bom2_file(const std::string& path, Project& prj, const BomOptions2& opt, BomReport& rep,
                      BomPreview* preview, std::string& err) {
    BomPreview local;
    BomPreview& p = preview ? *preview : local;
    BomSource src;
    if (!bom_load_source(path, src, err)) return false;
    if (!bom_preview_src(src, opt, p, err)) return false;
    return bom_apply(p, prj, opt, rep, err);
}

// ---------------------------------------------------------------------------
// summary
// ---------------------------------------------------------------------------
std::vector<BomSummaryItem> bom_summary(const BomPreview& p, const BomReport* rep) {
    std::vector<BomSummaryItem> v;
    auto list = [](const std::vector<std::string>& x, size_t n = 12) {
        std::string s;
        for (size_t i = 0; i < x.size() && i < n; ++i) { if (i) s += ", "; s += x[i]; }
        if (x.size() > n) s += ", ... (" + std::to_string(x.size()) + ")";
        return s;
    };
    if (!p.mapping_ok()) v.push_back({IssueSev::Error, "Column mapping incomplete", "Map Reference / Designator and a part number column (Internal or Manufacturer)."});
    int ref_err = 0;
    for (auto& i : p.issues) if (i.sev == IssueSev::Error && i.msg.rfind("references:", 0) == 0) ++ref_err;
    if (ref_err) v.push_back({IssueSev::Error, std::to_string(ref_err) + " reference range(s) could not be expanded",
                              "Those entries are kept as written and will not match a board reference. Fix the range in the BOM (see Issues)."});
    if (!p.pn_conflicts.empty()) v.push_back({IssueSev::Error, std::to_string(p.pn_conflicts.size()) + " reference(s) listed with different parts", list(p.pn_conflicts, 4)});
    if (p.qty_mismatches) v.push_back({IssueSev::Warning, std::to_string(p.qty_mismatches) + " line(s) where Quantity differs from the reference count", "Check those BOM lines before building."});
    int dnp = 0;
    for (auto& c : p.by_comp) dnp += c.dnp;
    if (rep) {
        if (!rep->refs_not_on_board.empty())
            v.push_back({IssueSev::Warning, std::to_string(rep->refs_not_on_board.size()) + " BOM reference(s) not found on the board", list(rep->refs_not_on_board)});
        if (!rep->comps_not_in_bom.empty())
            v.push_back({IssueSev::Warning, std::to_string(rep->comps_not_in_bom.size()) + " board component(s) not in the BOM (orphans)",
                         "They keep their data and are NOT marked DNP. Reconcile: " + list(rep->comps_not_in_bom)});
        if (!rep->package_mismatch.empty())
            v.push_back({IssueSev::Warning, std::to_string(rep->package_mismatch.size()) + " package disagreement(s) between BOM and board", list(rep->package_mismatch, 6)});
        v.push_back({IssueSev::Info, std::to_string(rep->linked) + " of " + std::to_string(rep->board_components) + " board components linked to a part number", ""});
    }
    if (dnp) v.push_back({IssueSev::Info, std::to_string(dnp) + " reference(s) marked DNP in the BOM", "Kept on the board with their own package; no part number is assigned."});
    v.push_back({IssueSev::Info, std::to_string(p.lines.size()) + " BOM lines, " + std::to_string(p.by_comp.size()) + " references, " + std::to_string(p.by_part.size()) + " part numbers", ""});
    for (auto& i : p.issues) if (i.sev == IssueSev::Info && i.line == 0) v.push_back({IssueSev::Info, i.msg, ""});
    std::stable_sort(v.begin(), v.end(), [](const BomSummaryItem& a, const BomSummaryItem& b) { return (int)a.sev > (int)b.sev; });
    return v;
}

} // namespace pcbp::io
