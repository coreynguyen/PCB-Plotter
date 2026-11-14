// m1_bom.cpp — built-in "M1 BOM" header profile + helpers (see m1_bom.h, specs/M1_BOM.md)
#include "m1_bom.h"
#include <algorithm>
#include <cctype>

namespace pcbp::io {

const std::vector<std::string>& m1_bom_headers() {
    static const std::vector<std::string> v = {
        "jmmJobID", "jmmJobAssemblyID", "jmmJobMaterialID", "jmmPartRevisionID",
        "jmmPartWarehouseLocationID", "jmmPartBinID", "jmmPartID", "impPartClassID",
        "jmmPartShortDescription", "ujmmPCRefText", "jmmQuantityPerAssembly", "jmmUnitOfMeasure",
        "uimpMfgPartID", "uimpManufacturer", "jmmCreatedBy", "jmmCreatedDate"};
    return v;
}

static Role m1_role(const std::string& h) {
    if (h == "jmmPartID") return Role::PartNumber;
    if (h == "ujmmPCRefText") return Role::Ref;
    if (h == "jmmQuantityPerAssembly") return Role::Qty;
    if (h == "jmmPartShortDescription") return Role::Description;
    if (h == "impPartClassID") return Role::Process;
    if (h == "uimpMfgPartID") return Role::MPN;
    if (h == "uimpManufacturer") return Role::Manufacturer;
    if (h == "jmmJobMaterialID") return Role::LineItem;
    if (h == "jmmPartBinID") return Role::Bin;
    return Role::Unused;   // jmmJobID is read separately (Project Info BOM no.)
}

FormatTemplate m1_bom_template() {
    FormatTemplate t;
    t.name = kM1BomTemplateName;
    t.purpose = Purpose::Bom;
    for (auto& h : m1_bom_headers()) {
        std::string k = norm_header(h);
        t.signature.push_back(k);
        t.header_roles.push_back({k, m1_role(h)});
        t.index_roles.push_back(m1_role(h));
    }
    t.ncols = (int)m1_bom_headers().size();
    return t;
}

bool ensure_builtin_bom_templates(std::vector<FormatTemplate>& v) {
    for (auto& t : v)
        if (t.name == kM1BomTemplateName && (t.purpose == Purpose::Bom || t.purpose == Purpose::Generic)) return false;
    v.push_back(m1_bom_template());
    return true;
}

static int col_of(const ParsedText& t, const char* h) {
    const std::string k = norm_header(h);
    for (size_t i = 0; i < t.cols.size(); ++i) if (norm_header(t.cols[i].header) == k) return (int)i;
    return -1;
}

bool is_m1_bom(const ParsedText& t) {
    return col_of(t, "jmmPartID") >= 0 && col_of(t, "ujmmPCRefText") >= 0 && col_of(t, "jmmQuantityPerAssembly") >= 0;
}

static std::string trim_s(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string m1_job_id(const ParsedText& t) {
    if (!is_m1_bom(t)) return {};
    const int c = col_of(t, "jmmJobID");
    if (c < 0) return {};
    for (auto& r : t.rows) {
        if (r.kind != RowKind::Data || c >= (int)r.cells.size()) continue;
        std::string v = trim_s(r.cells[c]);
        if (!v.empty() && norm_header(v) != norm_header("jmmJobID")) return v;
    }
    return {};
}

bool m1_normalize_job_id(const std::string& in, std::string& out) {
    std::string s = trim_s(in);
    if (s.empty() || s.size() > 40) return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/')) return false;
    out = s;
    return true;
}

std::string m1_rows_to_tsv(const std::vector<std::string>& headers, const std::vector<std::vector<std::string>>& rows) {
    auto cell = [](const std::string& s) {
        std::string o = s;
        for (char& c : o) if (c == '\t' || c == '\r' || c == '\n') c = ' ';
        return o;
    };
    std::string out;
    auto line = [&](const std::vector<std::string>& v) {
        for (size_t i = 0; i < v.size(); ++i) { if (i) out += '\t'; out += cell(v[i]); }
        out += '\n';
    };
    line(headers);
    for (auto& r : rows) line(r);
    return out;
}

// m1_is_board_class() lives in bom_import2.cpp (used by the generic BOM rules).

} // namespace pcbp::io
