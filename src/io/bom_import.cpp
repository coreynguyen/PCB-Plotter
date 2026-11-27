// ============================================================================
// bom_import.cpp — BOM import / link. See bom_import.h.
// ============================================================================
#include "bom_import.h"
#include "io_util.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

namespace pcbp::io {

bool parse_bool_cell(const std::string& s, bool& out) {
    std::string k = to_upper(trim(s));
    if (k.empty()) return false;
    static const char* yes[] = {"TRUE", "T", "Y", "YES", "1", "X", "POL", "POLARIZED", "POLARISED", "ON", "+"};
    static const char* no[] = {"FALSE", "F", "N", "NO", "0", "-", "NONE", "OFF", "NP", "NONPOLARIZED", "N/A", "NA"};
    for (auto* y : yes) if (k == y) { out = true; return true; }
    for (auto* n : no) if (k == n) { out = false; return true; }
    return false;
}

namespace {

struct Syn { int BomMapping::*slot; std::vector<std::string> exact; std::vector<std::string> contains; };

const std::vector<Syn>& syn_table() {
    static const std::vector<Syn> t = {
        {&BomMapping::refs, {"reference", "references", "ref", "refs", "refdes", "designator", "designators",
                             "referencedesignator", "referencedesignators", "refdesignators", "refdesignator",
                             "location", "locations", "partreference", "componentreference"},
                            {"refdes", "designator", "reference"}},
        {&BomMapping::cust_pn, {"customerpartnumber", "custpn", "customerpn", "custpartnumber", "customerpartno", "cpn", "custpartno"},
                               {"cust"}},
        {&BomMapping::vend_pn, {"vendorpartnumber", "vendorpn", "vendpn", "supplierpartnumber", "supplierpn",
                                "distributorpartnumber", "vendorpartno", "vendpartno"},
                               {"vendor", "supplier", "distributor", "vend"}},
        {&BomMapping::manf_pn, {"manufacturerpartnumber", "mfrpn", "mpn", "manfpn", "mfgpn", "mfgpartnumber",
                                "manufacturerpn", "mfrpartnumber", "manfpartnumber", "mfrpartno"},
                               {"manuf", "mfr", "mfg", "manf"}},
        {&BomMapping::number, {"partnumber", "partno", "pn", "number", "internalpartnumber", "internalpn",
                               "itemnumber", "part", "partnum", "companypartnumber"},
                              {"partnumber", "partno", "partnum"}},
        {&BomMapping::package, {"type", "package", "footprint", "case", "pkg", "shape", "pattern", "packagetype",
                                "parttype", "casepackage"},
                               {"package", "footprint"}},
        {&BomMapping::description, {"description", "desc", "partdescription", "comment", "comments"}, {"desc"}},
        {&BomMapping::value, {"value", "val"}, {}},
        {&BomMapping::process, {"process", "proc", "assemblyprocess", "processtype", "mounttype", "technology", "processpoint"},
                               {"process"}},
        {&BomMapping::polarity, {"polarity", "polarized", "polar", "pol", "polarised"}, {"polar"}},
        {&BomMapping::soft_rev, {"softwarerev", "sofwarerev", "softrev", "softwareversion", "softver", "firmware",
                                 "firmwarerev", "swrev", "programrev", "softwarerevision"},
                                {"soft", "firmware", "sofw"}},
        {&BomMapping::part_rev, {"partrev", "partrevision", "rev", "revision"}, {}},
        {&BomMapping::qty, {"qty", "quantity", "count", "qnty", "qtyper"}, {"qty", "quantity"}},
        {&BomMapping::socket, {"socket", "socketed"}, {}},
        {&BomMapping::height, {"height", "ht"}, {}},
        {&BomMapping::cost, {"cost", "price", "unitcost", "unitprice"}, {}},
        {&BomMapping::stock_code, {"stockcode", "stock", "stockno"}, {}},
        {&BomMapping::bin, {"bin", "binlocation"}, {}},
        {&BomMapping::line_item, {"lineitem", "line", "item", "find", "findnumber", "findno", "itemno"}, {}},
        {&BomMapping::pop_type, {"poptype", "populationtype", "populate", "pop"}, {}},
        {&BomMapping::part_marking, {"partmarking", "marking"}, {}},
        {&BomMapping::dnp, {"dnp", "dni", "nopop", "donotplace", "donotpopulate"}, {}},
    };
    return t;
}

bool is_ref_list(const std::string& s) {
    auto v = expand_ref_list(s);
    if (v.empty()) return false;
    for (auto& r : v) if (!looks_like_refdes(r)) return false;
    return true;
}

} // namespace

BomMapping auto_map_bom(const TextTable& t) {
    BomMapping m;
    const int nc = (int)t.ncols();
    std::vector<char> taken(nc, 0);
    if (t.has_header) {
        for (auto& s : syn_table()) {
            for (int c = 0; c < nc; ++c) {
                if (taken[c]) continue;
                std::string k = norm_key(t.columns[c]);
                if (std::find(s.exact.begin(), s.exact.end(), k) != s.exact.end()) {
                    m.*s.slot = c; taken[c] = 1; break;
                }
            }
        }
        for (auto& s : syn_table()) {
            if (m.*s.slot >= 0) continue;
            for (int c = 0; c < nc && m.*s.slot < 0; ++c) {
                if (taken[c]) continue;
                std::string k = norm_key(t.columns[c]);
                for (auto& sub : s.contains)
                    if (k.find(sub) != std::string::npos) { m.*s.slot = c; taken[c] = 1; break; }
            }
        }
    }
    // content fallback: refs = column of ref lists, number = next unused column
    if (m.refs < 0) {
        for (int c = 0; c < nc; ++c) {
            if (taken[c]) continue;
            int n = 0, ok = 0;
            for (size_t r = 0; r < t.rows.size() && r < 200; ++r) {
                if (t.rows[r][c].empty()) continue;
                ++n;
                if (is_ref_list(t.rows[r][c])) ++ok;
            }
            if (n && ok * 5 >= n * 4) { m.refs = c; taken[c] = 1; break; }
        }
    }
    if (m.number < 0 && m.refs >= 0) {
        for (int c = 0; c < nc; ++c) {
            if (taken[c] || c == m.refs) continue;
            int n = 0;
            for (size_t r = 0; r < t.rows.size() && r < 200; ++r) if (!t.rows[r][c].empty()) ++n;
            if (n * 2 >= (int)std::min<size_t>(t.rows.size(), 200)) { m.number = c; taken[c] = 1; break; }
        }
    }
    return m;
}

bool import_bom_text(const std::string& text, Project& prj, const BomOptions& opt,
                     BomReport& rep, std::string& err) {
    rep = BomReport{};
    if (!read_text_table(text, opt.table, rep.table, err)) return false;
    const TextTable& t = rep.table;
    const int nc = (int)t.ncols();

    BomMapping m = opt.map;
    if (opt.auto_map) {
        BomMapping a = auto_map_bom(t);
        for (auto& s : syn_table()) if (m.*s.slot < 0) m.*s.slot = a.*s.slot;
    }
    rep.map = m;
    if (!m.valid()) { err = "BOM mapping incomplete (need References and Part Number): " + t.describe(); return false; }
    {
        std::vector<char> used(nc, 0);
        for (auto& s : syn_table()) if (m.*s.slot >= 0 && m.*s.slot < nc) used[m.*s.slot] = 1;
        for (int c = 0; c < nc; ++c) if (!used[c]) rep.unmapped_columns.push_back(t.col_name(c));
    }
    auto cell = [&](const std::vector<std::string>& row, int c) -> std::string {
        return (c >= 0 && c < nc) ? row[c] : std::string();
    };

    if (opt.replace_parts) prj.parts.clear();
    if (opt.clear_comp_numbers) for (auto& c : prj.comps) c.number.clear();

    struct RefHit { std::string ref, pn; int line; bool dnp; };
    std::map<std::string, RefHit> refmap;           // upper(ref) -> first hit
    std::set<std::string> dup_seen;
    std::set<std::string> touched_parts;

    for (size_t r = 0; r < t.rows.size(); ++r) {
        const auto& row = t.rows[r];
        std::string refs = cell(row, m.refs), pn = cell(row, m.number);
        ++rep.rows;
        if (pn.empty() && refs.empty()) continue;
        if (pn.empty()) {
            rep.messages.push_back("line " + std::to_string(t.row_line[r]) + ": refs without part number skipped");
            continue;
        }
        // ---- part number record ----
        auto it = prj.parts.find(pn);
        bool is_new = it == prj.parts.end();
        PartNumber& p = prj.parts[pn];
        p.number = pn;
        auto setf = [&](std::string& f, int c) { std::string v = cell(row, c); if (!v.empty()) f = v; };
        setf(p.description, m.description);
        if (p.description.empty()) setf(p.description, m.value);
        setf(p.package, m.package);
        setf(p.default_process, m.process);
        setf(p.cust_pn, m.cust_pn);
        setf(p.vend_pn, m.vend_pn);
        setf(p.manf_pn, m.manf_pn);
        setf(p.soft_ver, m.soft_rev);
        setf(p.part_rev, m.part_rev);
        setf(p.stock_code, m.stock_code);
        setf(p.bin, m.bin);
        setf(p.line_item, m.line_item);
        setf(p.pop_type, m.pop_type);
        setf(p.part_marking, m.part_marking);
        bool b = false;
        if (m.polarity >= 0) {
            std::string pv = cell(row, m.polarity);
            if (parse_bool_cell(pv, b)) p.polarized = b;
            else if (!pv.empty()) rep.messages.push_back("line " + std::to_string(t.row_line[r]) + ": polarity \"" + pv + "\" not understood");
        }
        if (m.socket >= 0 && parse_bool_cell(cell(row, m.socket), b)) p.socket = b;
        if (auto h = parse_num_suffix(cell(row, m.height), nullptr)) p.height = *h;
        if (auto cst = parse_num(cell(row, m.cost))) p.cost = *cst;
        if (is_new) ++rep.parts_added;
        else if (!touched_parts.count(pn)) ++rep.parts_updated;
        touched_parts.insert(pn);

        bool row_dnp = false;
        if (m.dnp >= 0) parse_bool_cell(cell(row, m.dnp), row_dnp);

        // ---- references ----
        for (auto& ref : expand_ref_list(refs)) {
            std::string key = to_upper(ref);
            auto f = refmap.find(key);
            if (f == refmap.end()) { refmap[key] = {ref, pn, t.row_line[r], row_dnp}; continue; }
            if (dup_seen.insert(key).second) rep.duplicate_refs.push_back(ref);
            if (f->second.pn != pn) {
                rep.pn_conflicts.push_back(ref + ": " + f->second.pn + " (line " + std::to_string(f->second.line) +
                                           ") vs " + pn + " (line " + std::to_string(t.row_line[r]) + ")");
                if (!opt.multiple_pn_check) f->second = {ref, pn, t.row_line[r], row_dnp};
            }
        }
    }
    rep.refs_total = (int)refmap.size();

    // ---- link to components ----
    std::set<const Component*> linked;
    for (auto& [key, hit] : refmap) {
        Component* c = prj.find_ref(hit.ref);
        if (!c) { rep.refs_not_on_board.push_back(hit.ref); continue; }
        c->number = hit.pn;
        if (hit.dnp) c->dnp = true;
        const PartNumber& p = prj.parts[hit.pn];
        if (!p.package.empty()) {
            if (c->package.empty()) c->package = p.package;
            else if (!ieq(c->package, p.package)) {
                rep.package_mismatch.push_back(c->ref + ": board " + c->package + ", BOM " + p.package);
                if (opt.overwrite_package) c->package = p.package;
            }
        }
        linked.insert(c);
    }
    for (auto& c : prj.comps) {
        if (c.feature != Feature::Component) continue;
        ++rep.board_components;
        if (!linked.count(&c)) {
            rep.comps_not_in_bom.push_back(c.ref.empty() ? "(id " + std::to_string(c.id) + ")" : c.ref);
            if (opt.dnp_unlisted) c.dnp = true;
        }
    }
    rep.linked = (int)linked.size();
    auto srt = [](std::vector<std::string>& v) { std::sort(v.begin(), v.end(), ref_less); };
    srt(rep.refs_not_on_board); srt(rep.comps_not_in_bom); srt(rep.duplicate_refs);
    prj.dirty = true;
    return true;
}

bool import_bom_file(const std::string& path, Project& prj, const BomOptions& opt,
                     BomReport& rep, std::string& err) {
    std::string text;
    if (!read_file(path, text, err)) return false;
    return import_bom_text(text, prj, opt, rep, err);
}

std::string BomReport::describe() const {
    std::ostringstream s;
    s << table.describe() << "\n";
    auto nm = [&](int c) { return c < 0 ? std::string("-") : table.col_name(c); };
    s << "mapping: refs=" << nm(map.refs) << " number=" << nm(map.number) << " package=" << nm(map.package)
      << " desc=" << nm(map.description) << " process=" << nm(map.process) << " vend=" << nm(map.vend_pn)
      << " polarity=" << nm(map.polarity) << " soft=" << nm(map.soft_rev);
    if (!unmapped_columns.empty()) {
        s << "; unmapped:";
        for (auto& u : unmapped_columns) s << " [" << u << "]";
    }
    s << "\nrows " << rows << ", parts added " << parts_added << ", updated " << parts_updated
      << ", BOM refs " << refs_total << ", linked " << linked << "/" << board_components
      << " (" << coverage() << "%)";
    auto list = [&](const char* what, const std::vector<std::string>& v) {
        s << "\n" << what << " (" << v.size() << "):";
        for (size_t i = 0; i < v.size() && i < 40; ++i) s << " " << v[i];
        if (v.size() > 40) s << " ...";
    };
    list("refs not on board", refs_not_on_board);
    list("board comps not in BOM", comps_not_in_bom);
    list("duplicate refs", duplicate_refs);
    list("PN conflicts", pn_conflicts);
    list("package mismatches", package_mismatch);
    return s.str();
}

} // namespace pcbp::io
