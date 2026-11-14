// group_ops.cpp — see group_ops.h
#include "group_ops.h"
#include <cctype>
#include <cmath>
#include <map>
#include <set>

namespace pcbp {

static std::string up(std::string s) { for (auto& ch : s) ch = (char)std::toupper((unsigned char)ch); return s; }

static bool glob(const char* p, const char* s) {
    if (!*p) return !*s;
    if (*p == '*') return glob(p + 1, s) || (*s && glob(p, s + 1));
    if (*s && (*p == '?' || *p == *s)) return glob(p + 1, s + 1);
    return false;
}

bool ref_matches(const std::string& ref0, const std::string& pattern) {
    const std::string ref = up(ref0);
    std::string tok;
    auto test = [&](const std::string& t0) -> bool {
        if (t0.empty()) return false;
        const std::string t = up(t0);
        if (t.find_first_of("*?") != std::string::npos) return glob(t.c_str(), ref.c_str());
        if (t.find('-') != std::string::npos)
            for (auto& r : expand_ref_list(t)) if (up(r) == ref) return true;
        return t == ref;
    };
    for (char ch : pattern + ",") {
        if (ch == ',' || ch == ';' || ch == ' ' || ch == '\t') { if (test(tok)) return true; tok.clear(); }
        else tok += ch;
    }
    return false;
}

int select_by_package(Project& prj, const std::vector<std::string>& pkgs, SelMode mode) {
    std::set<std::string> s(pkgs.begin(), pkgs.end());
    return select_where(prj, [&](const Component& c) { return s.count(c.package) > 0; }, mode);
}
int select_by_ref(Project& prj, const std::string& pattern, SelMode mode) {
    return select_where(prj, [&](const Component& c) { return ref_matches(c.ref, pattern); }, mode);
}
int select_by_part(Project& prj, const std::vector<std::string>& parts, SelMode mode) {
    std::set<std::string> s(parts.begin(), parts.end());
    return select_where(prj, [&](const Component& c) { return s.count(c.number) > 0; }, mode);
}
int select_by_process(Project& prj, Process p, SelMode mode) {
    return select_where(prj, [&](const Component& c) { return prj.process_of(c) == p; }, mode);
}
int select_by_side(Project& prj, Side sd, SelMode mode) {
    return select_where(prj, [&](const Component& c) { return c.side == sd; }, mode);
}

std::vector<int> selected_indices(const Project& prj) {
    std::vector<int> v;
    for (size_t i = 0; i < prj.comps.size(); ++i)
        if (prj.comps[i].selected && prj.comps[i].feature == Feature::Component) v.push_back((int)i);
    return v;
}

template <class F> static int each(Project& prj, const std::vector<int>& idx, F f) {
    int n = 0;
    for (int i : idx) if (i >= 0 && i < (int)prj.comps.size()) n += f(prj.comps[i]) ? 1 : 0;
    if (n) prj.dirty = true;
    return n;
}
static bool set_rot(Component& c, double a) {
    a = norm_deg(a);
    if (std::fabs(a - c.rot) < 1e-12) return false;
    c.rot = a;
    return true;
}
int rotate_add(Project& prj, const std::vector<int>& idx, double d) {
    return each(prj, idx, [&](Component& c) { return set_rot(c, c.rot + d); });
}
int rotate_set(Project& prj, const std::vector<int>& idx, double a) {
    return each(prj, idx, [&](Component& c) { return set_rot(c, a); });
}
int rotate_reset(Project& prj, const std::vector<int>& idx) {
    return each(prj, idx, [&](Component& c) { return set_rot(c, c.rot_imported); });
}
int set_package(Project& prj, const std::vector<int>& idx, const std::string& pkg) {
    return each(prj, idx, [&](Component& c) { if (c.package == pkg) return false; c.package = pkg; return true; });
}
int set_process(Project& prj, const std::vector<int>& idx, Process p) {
    return each(prj, idx, [&](Component& c) { if (c.process == p) return false; c.process = p; return true; });
}
int set_group_label(Project& prj, const std::vector<int>& idx, const std::string& l) {
    return each(prj, idx, [&](Component& c) { if (c.group_label == l) return false; c.group_label = l; return true; });
}
int set_pre_rotation(Project& prj, const std::vector<int>& idx, double cw) {
    return each(prj, idx, [&](Component& c) { const double v = norm_deg(cw); if (c.pre_rotation == v) return false; c.pre_rotation = v; return true; });
}

static double residue(double a) {
    double r = std::fmod(norm_deg(a), 90.0);
    if (r > 90.0 - 0.005) r = 0;
    return std::round(r * 100.0) / 100.0;
}

std::vector<Outlier> find_outliers(const Project& prj, const std::vector<int>& idx) {
    std::set<std::string> pk;
    for (int i : idx) if (i >= 0 && i < (int)prj.comps.size()) pk.insert(prj.comps[i].package);
    std::vector<Outlier> out;
    for (const std::string& p : pk) {
        std::map<double, int> hist;
        std::vector<int> members;
        for (size_t i = 0; i < prj.comps.size(); ++i) {
            const Component& c = prj.comps[i];
            if (c.feature != Feature::Component || c.package != p) continue;
            members.push_back((int)i);
            hist[residue(c.rot)]++;
        }
        double maj = 0; int best = -1;
        for (auto& kv : hist) if (kv.second > best) { best = kv.second; maj = kv.first; }
        for (int i : members) {
            const double r = residue(prj.comps[i].rot);
            if (std::fabs(r - maj) > 0.01) out.push_back({i, p, prj.comps[i].rot, maj});
        }
    }
    return out;
}

} // namespace pcbp
