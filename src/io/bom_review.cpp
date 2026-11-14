// bom_review.cpp — grouped BOM review + text / CSV report (see bom_review.h)
#include "bom_review.h"
#include "io_util.h"
#include <algorithm>
#include <ctime>
#include <map>

namespace pcbp::io {

const char* review_cat_title(ReviewCat c) {
    switch (c) {
    case ReviewCat::PnConflict: return "Part assignment conflicts (reference on lines with different parts)";
    case ReviewCat::RefError: return "Reference ranges that could not be expanded";
    case ReviewCat::QtyMismatch: return "Lines where Quantity differs from the reference count";
    case ReviewCat::NotOnBoard: return "BOM references not on the board";
    case ReviewCat::Unmatched: return "Board placements not in the BOM";
    case ReviewCat::NoPartNumber: return "BOM lines without a part number";
    case ReviewCat::Duplicate: return "References listed twice (same part)";
    case ReviewCat::OtherWarning: return "Other warnings";
    case ReviewCat::Dnp: return "References marked DNP";
    case ReviewCat::Info: return "Information";
    default: return "";
    }
}
const char* review_cat_key(ReviewCat c) {
    static const char* k[] = {"pn_conflict", "ref_error", "qty_mismatch", "not_on_board", "unmatched_placement",
                              "no_part_number", "duplicate_ref", "warning", "dnp", "info"};
    return (int)c < (int)ReviewCat::Count_ ? k[(int)c] : "";
}
IssueSev review_cat_sev(ReviewCat c) {
    switch (c) {
    case ReviewCat::PnConflict: case ReviewCat::RefError: return IssueSev::Error;
    case ReviewCat::Dnp: case ReviewCat::Info: return IssueSev::Info;
    default: return IssueSev::Warning;
    }
}

const ReviewGroup* BomReview::group(ReviewCat c) const {
    for (auto& g : groups) if (g.cat == c) return &g;
    return nullptr;
}
uint32_t BomReview::flags_of(const std::string& ref) const {
    auto it = std::lower_bound(ref_flags.begin(), ref_flags.end(), ref, [](const auto& a, const std::string& b) { return a.first < b; });
    return it != ref_flags.end() && it->first == ref ? it->second : 0u;
}

static bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

BomReview bom_review(const BomPreview& p, const std::set<std::string>& board, const std::string& source,
                     const std::string& job, const std::string& date) {
    BomReview r;
    r.source = source;
    r.job = job;
    if (date.empty()) {
        char b[32]; std::time_t now = std::time(nullptr); std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", std::localtime(&now));
        r.date = b;
    } else r.date = date;
    r.bom_lines = (int)p.lines.size();
    r.bom_refs = (int)p.by_comp.size();
    r.part_numbers = (int)p.by_part.size();
    r.board_placements = (int)board.size();
    std::vector<ReviewItem> g[(int)ReviewCat::Count_];
    auto add = [&](ReviewCat c, std::string ref, int line, std::string text) { g[(int)c].push_back({std::move(ref), line, std::move(text)}); };

    std::set<std::string> bom;
    for (auto& c : p.by_comp) {
        const std::string u = to_upper(c.ref);
        bom.insert(u);
        if (board.count(u)) ++r.matched;
        else add(ReviewCat::NotOnBoard, c.ref, c.line, c.ref + " (line " + std::to_string(c.line) + (c.pn.empty() ? "" : ", " + c.pn) + ")");
        if (c.dnp) add(ReviewCat::Dnp, c.ref, c.line, c.ref + " (line " + std::to_string(c.line) + ")");
    }
    std::vector<std::string> um;
    for (auto& b : board) if (!bom.count(b)) um.push_back(b);
    std::sort(um.begin(), um.end(), ref_less);
    for (auto& b : um) add(ReviewCat::Unmatched, b, 0, b);
    for (auto& c : p.pn_conflicts) {
        size_t k = c.find(':');
        add(ReviewCat::PnConflict, k == std::string::npos ? "" : c.substr(0, k), 0, c);
    }
    std::stable_sort(g[(int)ReviewCat::PnConflict].begin(), g[(int)ReviewCat::PnConflict].end(),
                     [](const ReviewItem& a, const ReviewItem& b) { return ref_less(a.ref, b.ref); });
    for (auto& i : p.issues) {
        const std::string& m = i.msg;
        if (starts(m, "Reference listed with different parts")) continue;              // from pn_conflicts
        std::string ref;
        ReviewCat c;
        if (i.sev == IssueSev::Error && starts(m, "references:")) c = ReviewCat::RefError;
        else if (starts(m, "Quantity says")) c = ReviewCat::QtyMismatch;
        else if (starts(m, "No part number")) c = ReviewCat::NoPartNumber;
        else if (starts(m, "Reference ") && m.find("more than one line") != std::string::npos) {
            c = ReviewCat::Duplicate;
            ref = m.substr(10, m.find(' ', 10) - 10);
        } else if (i.sev == IssueSev::Info) c = ReviewCat::Info;
        else c = i.sev == IssueSev::Error ? ReviewCat::RefError : ReviewCat::OtherWarning;
        add(c, ref, i.line, i.line ? "line " + std::to_string(i.line) + ": " + m : m);
    }
    std::map<std::string, uint32_t> fl;
    for (int c = 0; c < (int)ReviewCat::Count_; ++c)
        for (auto& it : g[c]) if (!it.ref.empty()) fl[to_upper(it.ref)] |= 1u << c;
    for (auto& b : p.lines) if (b.qty_mismatch) for (auto& x : b.refs) fl[to_upper(x)] |= 1u << (int)ReviewCat::QtyMismatch;
    for (auto& b : p.lines) if (b.pn.empty() && !b.dnp) for (auto& x : b.refs) fl[to_upper(x)] |= 1u << (int)ReviewCat::NoPartNumber;
    r.ref_flags.assign(fl.begin(), fl.end());
    for (int c = 0; c < (int)ReviewCat::Count_; ++c)
        if (!g[c].empty()) r.groups.push_back({(ReviewCat)c, std::move(g[c])});
    return r;
}

static bool ref_list_group(ReviewCat c) { return c == ReviewCat::Unmatched; }

std::string bom_review_group_text(const ReviewGroup& g) {
    std::string o = std::string(issue_sev_name(review_cat_sev(g.cat))) + ": " + review_cat_title(g.cat) + " (" + std::to_string(g.items.size()) + ")\n";
    if (g.cat == ReviewCat::NotOnBoard || g.cat == ReviewCat::PnConflict || g.cat == ReviewCat::Dnp) {   // the plain list first, details after
        std::string refs;
        for (auto& i : g.items) { if (!refs.empty()) refs += ", "; refs += i.ref; }
        o += "  " + refs + "\n";
    }
    if (ref_list_group(g.cat)) {
        std::string ln = "  ";
        for (size_t i = 0; i < g.items.size(); ++i) {
            std::string t = g.items[i].ref + (i + 1 < g.items.size() ? ", " : "");
            if (ln.size() + t.size() > 78 && ln.size() > 2) { o += ln + "\n"; ln = "  "; }
            ln += t;
        }
        if (ln.size() > 2) o += ln + "\n";
    } else
        for (auto& i : g.items) o += "  - " + i.text + "\n";
    return o;
}

std::string bom_review_text(const BomReview& r) {
    int ne = 0, nw = 0;
    for (auto& g : r.groups) {
        const IssueSev s = review_cat_sev(g.cat);
        if (s == IssueSev::Error) ne += (int)g.items.size();
        else if (s == IssueSev::Warning) nw += (int)g.items.size();
    }
    std::string o = "BOM review report\n=================\n";
    o += "Source:         " + r.source + "\n";
    if (!r.job.empty()) o += "Job / BOM no.:  " + r.job + "\n";
    o += "Date:           " + r.date + "\n";
    o += "BOM:            " + std::to_string(r.bom_lines) + " lines, " + std::to_string(r.bom_refs) + " references, " + std::to_string(r.part_numbers) + " part numbers\n";
    o += "Board:          " + std::to_string(r.board_placements) + " placements, " + std::to_string(r.matched) + " matched to the BOM\n";
    o += "Findings:       " + std::to_string(ne) + " error(s), " + std::to_string(nw) + " warning(s)\n";
    for (auto& g : r.groups) {
        o += "\n";
        o += bom_review_group_text(g);
    }
    if (r.groups.empty()) o += "\nNo findings.\n";
    return o;
}

static std::string csvq(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

std::string bom_review_csv(const BomReview& r) {
    std::string o = "severity,category,reference,line,detail\r\n";
    for (auto& g : r.groups)
        for (auto& i : g.items)
            o += std::string(issue_sev_name(review_cat_sev(g.cat))) + "," + review_cat_key(g.cat) + "," + csvq(i.ref) + "," +
                 (i.line ? std::to_string(i.line) : std::string()) + "," + csvq(i.text) + "\r\n";
    return o;
}

} // namespace pcbp::io
