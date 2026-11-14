// ============================================================================
// package_resolve.cpp — R16b centroid -> package pipeline + acquisition wizard
// state (see package_resolve.h, specs/ANGLES.md §10).
// ============================================================================
#include "package_resolve.h"
#include "package_edit.h"
#include "package_name.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace pcbp {

namespace {
constexpr double kTwoPi = 6.283185307179586;
std::string up_trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    std::string u = s.substr(a, b - a);
    for (auto& ch : u) ch = (char)std::toupper((unsigned char)ch);
    return u;
}
bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
void erase_all(std::vector<std::string>& v, const std::string& s) { v.erase(std::remove(v.begin(), v.end(), s), v.end()); }
} // namespace

bool package_resolved(const Package& p) { return !p.pads.empty() || !p.body.empty(); }

bool placement_package_resolved(const Project& prj, const Component& c) {
    if (c.feature != Feature::Component) return true;
    if (c.package.empty()) return false;
    const Package* p = prj.package_of(c);
    return p && package_resolved(*p);
}

std::vector<std::string> referenced_packages(const Project& prj) {
    std::set<std::string> s;
    for (auto& c : prj.comps) if (c.feature == Feature::Component && !c.package.empty()) s.insert(c.package);
    std::vector<std::string> v(s.begin(), s.end());
    std::stable_sort(v.begin(), v.end(), ref_less);
    return v;
}

int ensure_package_entries(Project& prj) {
    int n = 0;
    for (auto& name : referenced_packages(prj)) {
        if (prj.packages.count(name)) continue;
        Package p;
        p.name = name;
        p.leads = 0;
        p.description = "";          // shown as "Unresolved / no graphics" by the UI (package_resolved() == false)
        prj.packages[name] = p;
        ++n;
    }
    return n;
}

std::vector<std::string> unresolved_packages(const Project& prj) {
    std::vector<std::string> v;
    for (auto& name : referenced_packages(prj)) {
        auto it = prj.packages.find(name);
        if (it == prj.packages.end() || !package_resolved(it->second)) v.push_back(name);
    }
    return v;
}

int placements_using(const Project& prj, const std::string& pkg) {
    int n = 0;
    for (auto& c : prj.comps) if (c.feature == Feature::Component && c.package == pkg) ++n;
    return n;
}

const Package* library_exact_match(const std::map<std::string, Package>& lib, const std::string& name) {
    const std::string u = up_trim(name);
    if (u.empty()) return nullptr;
    auto it = lib.find(name);
    if (it != lib.end()) return &it->second;
    for (auto& kv : lib) if (up_trim(kv.first) == u) return &kv.second;
    return nullptr;
}

// ---------------------------------------------------------------------------
// family recognition (suggestions only)
// ---------------------------------------------------------------------------
namespace {
// first number after token `t` in the key (separators removed), 0 = none
int num_after(const std::string& key, const std::string& t) {
    size_t p = key.find(t);
    if (p == std::string::npos) return 0;
    p += t.size();
    while (p < key.size() && !std::isdigit((unsigned char)key[p])) { if (std::isalpha((unsigned char)key[p]) && key[p] != 'X') break; ++p; }
    int v = 0, d = 0;
    while (p < key.size() && std::isdigit((unsigned char)key[p]) && d < 4) { v = v * 10 + (key[p] - '0'); ++p; ++d; }
    return v;
}
// first number in the original text after token t (keeps separators, so "SOT-23-5" -> 5 after "SOT-23")
int num_after_raw(const std::string& raw, const std::string& t) {
    std::string u = up_trim(raw);
    size_t p = u.find(t);
    if (p == std::string::npos) return 0;
    p += t.size();
    while (p < u.size() && (u[p] == '-' || u[p] == '_' || u[p] == ' ' || u[p] == '.')) ++p;
    int v = 0, d = 0;
    while (p < u.size() && std::isdigit((unsigned char)u[p]) && d < 4) { v = v * 10 + (u[p] - '0'); ++p; ++d; }
    return v;
}
bool has(const std::string& k, const char* t) { return k.find(t) != std::string::npos; }

const char* chip_preset(const std::string& code) {
    static const char* tab[][2] = {{"01005", "01005 / 0402M"}, {"0201", "0201 / 0603M"}, {"0402", "0402 / 1005M"}, {"0603", "0603 / 1608M"},
                                   {"0805", "0805 / 2012M"}, {"1206", "1206 / 3216M"}, {"1210", "1210 / 3225M"}, {"1812", "1812 / 4532M"},
                                   {"2010", "2010 / 5025M"}, {"2512", "2512 / 6332M"}};
    for (auto& r : tab) if (code == r[0]) return r[1];
    return nullptr;
}
const char* xtal_preset(const std::string& k) {
    static const char* tab[][2] = {{"7050", "7050 (7.0x5.0)"}, {"70X50", "7050 (7.0x5.0)"}, {"5032", "5032 (5.0x3.2)"}, {"50X32", "5032 (5.0x3.2)"},
                                   {"3225", "3225 (3.2x2.5)"}, {"32X25", "3225 (3.2x2.5)"}, {"2520", "2520 (2.5x2.0)"}, {"25X20", "2520 (2.5x2.0)"},
                                   {"2016", "2016 (2.0x1.6)"}, {"20X16", "2016 (2.0x1.6)"}, {"1612", "1612 (1.6x1.2)"}, {"16X12", "1612 (1.6x1.2)"}};
    for (auto& r : tab) if (has(k, r[0])) return r[1];
    return nullptr;
}
} // namespace

std::vector<PkgSuggestion> suggest_packages(const std::string& name, const std::map<std::string, Package>* lib) {
    std::vector<PkgSuggestion> out;
    const std::string key = package_key(name);
    std::string u;   // uppercase, separators removed (package_key reduces chips; keep the raw family text too)
    for (unsigned char ch : name) if (!(std::isspace(ch) || ch == '-' || ch == '_' || ch == '.')) u += (char)std::toupper(ch);
    if (lib) {
        if (const Package* e = library_exact_match(*lib, name)) {
            PkgSuggestion s; s.kind = PkgSuggestion::Library; s.name = e->name.empty() ? name : e->name; s.score = 100;
            s.why = "Same name in the package library"; out.push_back(s);
        }
        for (auto& kv : *lib) {
            if (!package_resolved(kv.second) || up_trim(kv.first) == up_trim(name)) continue;
            if (!key.empty() && package_key(kv.first) == key) {
                PkgSuggestion s; s.kind = PkgSuggestion::Library; s.name = kv.first; s.score = 80;
                s.why = "Library package with an equivalent name (" + key + ") - verify pads"; out.push_back(s);
            }
        }
    }
    auto tpl = [&](const char* id, const char* preset, const std::string& why, std::map<std::string, double> prm = {}) {
        PkgSuggestion s; s.kind = PkgSuggestion::Template; s.tpl = id; s.preset = preset ? preset : ""; s.why = why; s.params = std::move(prm); s.score = 50;
        out.push_back(s);
    };
    const bool xtal = has(u, "XTAL") || has(u, "CRYSTAL") || has(u, "OSC") || has(u, "XO") || has(u, "TCXO") || (u.size() >= 1 && u[0] == 'Y' && xtal_preset(u));
    if (xtal) tpl("xtal4", xtal_preset(u), "Crystal / oscillator name: 4-pad crystal template - verify pad count");
    else if (const char* cp = chip_preset(key)) {
        if (has(u, "MELF")) tpl("melf", nullptr, "MELF family");
        else tpl("chip", cp, "Chip size " + key + " (R/C/L/LED chip)");
    }
    if (has(u, "SOT223")) tpl("sot223", nullptr, "SOT-223 family");
    else if (has(u, "SOT89")) tpl("sot89", nullptr, "SOT-89 family");
    else if (has(u, "SC70") || has(u, "SOT323")) tpl("sot23", "SC-70 (SOT-323)", "SC-70 / SOT-323 family");
    else if (has(u, "SOT23") || has(u, "SOT353") || has(u, "SOT363") || has(u, "TSOT")) {
        int n = num_after_raw(name, "SOT-23");
        if (!n) n = num_after_raw(name, "SOT23");
        if (n != 5 && n != 6) n = has(u, "SOT353") ? 5 : has(u, "SOT363") ? 6 : 3;
        tpl("sot23", n == 3 ? "SOT-23-3" : n == 5 ? "SOT-23-5" : "SOT-23-6", "SOT-23 family, " + std::to_string(n) + " leads");
    }
    if (has(u, "MINIMELF") || has(u, "SOD80")) tpl("melf", "MiniMELF (SOD-80)", "MiniMELF / SOD-80");
    else if (has(u, "MELF") && !chip_preset(key)) tpl("melf", nullptr, "MELF family");
    else if (has(u, "SOD123")) tpl("sod", "SOD-123", "SOD-123");
    else if (has(u, "SOD323")) tpl("sod", "SOD-323", "SOD-323");
    else if (has(u, "SOD523")) tpl("sod", "SOD-523", "SOD-523");
    else if (has(u, "SMA") || has(u, "SMB") || has(u, "SMC") || has(u, "DO214")) tpl("sod", nullptr, "Diode (SMA/SMB/SMC): SOD template, adjust sizes");
    auto pins = [&](const char* t) { return num_after(u, t); };
    if (has(u, "TSSOP")) { int n = pins("TSSOP"); tpl("soic", "TSSOP", "TSSOP family", n ? std::map<std::string, double>{{"n", (double)n}} : std::map<std::string, double>{}); }
    else if (has(u, "MSOP")) { int n = pins("MSOP"); tpl("soic", "MSOP", "MSOP family", n ? std::map<std::string, double>{{"n", (double)n}} : std::map<std::string, double>{}); }
    else if (has(u, "SSOP")) { int n = pins("SSOP"); tpl("soic", "SSOP", "SSOP family", n ? std::map<std::string, double>{{"n", (double)n}} : std::map<std::string, double>{}); }
    else if (has(u, "SOIC") || has(u, "SOP") || (u.rfind("SO", 0) == 0 && u.size() > 2 && std::isdigit((unsigned char)u[2]))) {
        int n = pins("SOIC"); if (!n) n = pins("SOP"); if (!n) n = pins("SO");
        tpl("soic", has(u, "W") && n >= 16 ? "SOIC wide" : "SOIC", "SOIC family" + (n ? ", " + std::to_string(n) + " pins" : std::string()),
            n ? std::map<std::string, double>{{"n", (double)n}} : std::map<std::string, double>{});
    }
    if (has(u, "QFP")) {
        int n = pins("QFP");
        std::map<std::string, double> m; if (n >= 8 && n % 4 == 0) m["n"] = n / 4;
        tpl("qfp", nullptr, "QFP family" + (n ? ", " + std::to_string(n) + " pins" : std::string()) + " - set pitch / span", m);
    }
    if (has(u, "QFN") || has(u, "DFN") || has(u, "MLF") || has(u, "LGA")) {
        int n = pins("QFN"); if (!n) n = pins("DFN"); if (!n) n = pins("MLF");
        std::map<std::string, double> m; if (n >= 8 && n % 4 == 0) m["n"] = n / 4;
        tpl("qfn", nullptr, "QFN / DFN family" + (n ? ", " + std::to_string(n) + " pins" : std::string()) + " - set pitch / exposed pad", m);
    }
    if (has(u, "BGA")) tpl("bga", nullptr, "BGA family - set rows, columns, pitch");
    if (has(u, "PLCC")) { int n = pins("PLCC"); std::map<std::string, double> m; if (n >= 20 && n % 4 == 0) m["n"] = n / 4; tpl("plcc", nullptr, "PLCC family", m); }
    if (has(u, "DIP")) { int n = pins("DIP"); tpl("dip", nullptr, "DIP family", n ? std::map<std::string, double>{{"n", (double)n}} : std::map<std::string, double>{}); }
    if (has(u, "HDR") || has(u, "HEADER") || has(u, "PINHEAD")) tpl("header", nullptr, "Pin header");
    if (has(u, "RADIAL") || has(u, "CPRAD") || has(u, "ELEC")) tpl("radial", nullptr, "Radial through-hole");
    if (has(u, "AXIAL")) tpl("axial", nullptr, "Axial through-hole");
    std::stable_sort(out.begin(), out.end(), [](const PkgSuggestion& a, const PkgSuggestion& b) { return a.score > b.score; });
    return out;
}

int assign_package_geometry(Project& prj, const std::string& name, const Package& src, const std::string& how) {
    Package& dst = prj.packages[name];
    const int tape = dst.tape_rot;
    const std::string desc = dst.description;
    dst = src;
    dst.name = name;
    dst.tape_rot = tape;
    if (dst.description.empty() || (!desc.empty() && how == "gerber")) dst.description = desc;
    dst.resolved_by = how;
    if (!dst.leads) dst.leads = (int)dst.pads.size();
    pkg_apply_polarity_default(dst);   // once; a source that already carries a user marker keeps it
    prj.dirty = true;
    return placements_using(prj, name);
}

Package package_from_board_pads(const std::vector<Pad>& board, const Component& c) {
    Package pk;
    Xform t = Xform::translate(c.pos.x, c.pos.y) * Xform::rotation(c.rot);
    const bool mir = c.side == Side::Bottom;
    if (mir) t = t * Xform::scale(-1, 1);
    const Xform inv = t.inverse();
    for (const Pad& b : board) {
        Pad p = b;
        p.pos = inv.apply(b.pos);
        double r = norm_deg(b.rot - c.rot);
        if (mir) r = norm_deg(-r);
        const double r90 = std::fmod(r, 180.0);
        if (std::fabs(r90 - 90) < 0.05) { std::swap(p.w, p.h); p.rot = 0; }
        else if (r90 < 0.05 || r90 > 179.95) p.rot = 0;
        else p.rot = r;
        p.pin = 0;
        pk.pads.push_back(p);
    }
    // pin numbering: pin 1 = left-most pad (lowest for two rows along X, else top-most), then CCW about the pad centroid
    const size_t n = pk.pads.size();
    if (n) {
        Vec2 ctr;
        for (auto& p : pk.pads) ctr = ctr + p.pos;
        ctr = ctr * (1.0 / (double)n);
        double minx = 1e300;
        for (auto& p : pk.pads) minx = std::min(minx, p.pos.x);
        std::set<long long> ys;
        for (auto& p : pk.pads) ys.insert(std::llround(p.pos.y / 0.05));
        const bool two_rows_x = ys.size() == 2 && n > 2;
        int i1 = -1;
        for (size_t i = 0; i < n; ++i) {
            const Pad& p = pk.pads[i];
            if (p.pos.x > minx + 0.05) continue;
            if (i1 < 0) { i1 = (int)i; continue; }
            const double y = p.pos.y, y1 = pk.pads[i1].pos.y;
            if (two_rows_x ? y < y1 : y > y1) i1 = (int)i;
        }
        auto ang = [&](const Pad& p) { return std::atan2(p.pos.y - ctr.y, p.pos.x - ctr.x); };
        const double a1 = ang(pk.pads[i1]);
        double span = 0;
        for (auto& p : pk.pads) span = std::max(span, std::hypot(p.pos.x - ctr.x, p.pos.y - ctr.y));
        std::vector<int> idx(n);
        for (size_t i = 0; i < n; ++i) idx[i] = (int)i;
        auto key = [&](int i) {
            const Pad& p = pk.pads[i];
            if (i != i1 && std::hypot(p.pos.x - ctr.x, p.pos.y - ctr.y) < span * 0.2) return 1e9;   // centre / exposed pad last
            double d = ang(p) - a1;
            while (d < -1e-9) d += kTwoPi;
            while (d >= kTwoPi - 1e-9) d -= kTwoPi;
            return i == i1 ? -1.0 : d;
        };
        std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return key(a) < key(b); });
        std::vector<Pad> sorted;
        for (size_t k = 0; k < n; ++k) { Pad p = pk.pads[idx[k]]; p.pin = (int)k + 1; sorted.push_back(p); }
        pk.pads = sorted;
    }
    bool th = false;
    for (auto& p : pk.pads) th = th || p.drill > 0;
    pk.smt = !th;
    pk.leads = (int)pk.pads.size();
    pk.body_source = "pads";
    pk.auto_body();
    return pk;
}

// ---------------------------------------------------------------------------
// R17a: automatic resolution + report
// ---------------------------------------------------------------------------
bool builtin_template_package(const std::string& name, Package& out, std::string* why) {
    const std::string key = package_key(name);
    struct Chip { const char* code; double c, w, h; };
    static const Chip chips[] = {{"0201", 0.60, 0.35, 0.35}, {"0402", 1.00, 0.55, 0.60}, {"0603", 1.60, 0.90, 0.95},
                                 {"0805", 1.90, 1.00, 1.45}, {"1206", 3.00, 1.15, 1.80}, {"1210", 3.00, 1.15, 2.70},
                                 {"1812", 4.20, 1.40, 3.40}, {"2010", 4.60, 1.30, 2.60}, {"2512", 6.00, 1.50, 3.40}};
    std::string u;
    for (unsigned char ch : name) if (!(std::isspace(ch) || ch == '-' || ch == '_' || ch == '.')) u += (char)std::toupper(ch);
    Package p;
    p.name = name;
    if (u.find("MELF") == std::string::npos)
        for (auto& ch : chips)
            if (key == ch.code) {
                p.pads = {Pad{{-ch.c / 2, 0}, ch.w, ch.h, 0, PadShape::Rect, 0, 1}, Pad{{ch.c / 2, 0}, ch.w, ch.h, 0, PadShape::Rect, 0, 2}};
                p.resolved_by = "template:chip";
                if (why) *why = std::string("chip ") + ch.code + " template";
                break;
            }
    if (p.pads.empty() && (key == "SOT23" || key == "SOT233" || key == "TO236" || key == "TO236AB")) {
        p.pads = {Pad{{-1.1, 0.95}, 1.325, 0.6, 0, PadShape::Rect, 0, 1}, Pad{{-1.1, -0.95}, 1.325, 0.6, 0, PadShape::Rect, 0, 2},
                  Pad{{1.1, 0}, 1.325, 0.6, 0, PadShape::Rect, 0, 3}};
        p.resolved_by = "template:sot23";
        if (why) *why = "SOT-23 (3 leads) template";
    }
    if (p.pads.empty()) return false;
    p.smt = true;
    p.leads = (int)p.pads.size();
    p.body_source = "pads";
    p.auto_body();
    p.description = "Built-in template (" + p.resolved_by.substr(9) + "), verify pads";
    out = p;
    return true;
}

static PkgResolveEntry entry_of(const Project& prj, const std::string& name) {
    PkgResolveEntry e;
    e.name = name;
    e.placements = placements_using(prj, name);
    auto it = prj.packages.find(name);
    e.resolved = it != prj.packages.end() && package_resolved(it->second);
    if (e.resolved) {
        e.how = it->second.resolved_by;
        e.detail = std::to_string(it->second.pads.size()) + " pads" + (e.how.empty() ? std::string() : " (" + e.how + ")");
    } else e.detail = "no graphics yet";
    return e;
}

static void tally(PkgResolveReport& r) {
    r.resolved = r.unresolved = r.changed = 0;
    for (auto& e : r.entries) { (e.resolved ? r.resolved : r.unresolved)++; if (e.changed) ++r.changed; }
}

PkgResolveReport package_report(const Project& prj) {
    PkgResolveReport r;
    for (auto& n : referenced_packages(prj)) r.entries.push_back(entry_of(prj, n));
    tally(r);
    return r;
}

PkgResolveReport auto_resolve_packages(Project& prj, const std::map<std::string, Package>* lib, bool templates) {
    ensure_package_entries(prj);
    PkgResolveReport r;
    for (auto& name : referenced_packages(prj)) {
        PkgResolveEntry e = entry_of(prj, name);
        if (!e.resolved) {
            const Package* src = nullptr;
            std::string how, detail;
            if (lib) if (const Package* m = library_exact_match(*lib, name); m && package_resolved(*m)) { src = m; how = "library"; detail = "same name in the package library"; }
            const std::string key = package_key(name);
            if (!src && !key.empty())
                for (auto& kv : prj.packages)
                    if (kv.first != name && package_resolved(kv.second) && package_key(kv.first) == key) { src = &kv.second; how = "project:" + kv.first; detail = "same package as " + kv.first; break; }
            if (!src && lib && !key.empty())
                for (auto& kv : *lib)
                    if (package_resolved(kv.second) && package_key(kv.first) == key) { src = &kv.second; how = "library"; detail = "library package " + kv.first + " (equivalent name)"; break; }
            Package tp;
            std::string tw;
            if (!src && templates && builtin_template_package(name, tp, &tw)) { src = &tp; how = tp.resolved_by; detail = tw; }
            if (src) {
                const Package copy = *src;   // src may point into prj.packages
                assign_package_geometry(prj, name, copy, how.rfind("project:", 0) == 0 ? "library" : how);
                prj.packages[name].resolved_by = how;
                e = entry_of(prj, name);
                e.changed = true;
                e.detail = detail + ", " + std::to_string(copy.pads.size()) + " pads";
            }
        }
        r.entries.push_back(e);
    }
    tally(r);
    return r;
}

std::string PkgResolveReport::summary() const {
    std::string s = std::to_string(resolved) + " of " + std::to_string(entries.size()) + " packages resolved";
    if (changed) {
        s += " (" + std::to_string(changed) + " now:";
        int k = 0;
        for (auto& e : entries) if (e.changed) { if (k++ < 6) s += " " + e.name; }
        if (k > 6) s += " ...";
        s += ")";
    }
    if (unresolved) {
        s += "; unresolved:";
        int k = 0;
        for (auto& e : entries) if (!e.resolved) { if (k++ < 6) s += " " + e.name + " (" + std::to_string(e.placements) + ")"; }
        if (k > 6) s += " ...";
    }
    return s;
}

Vec2 package_pad_centre(const Package& p) {
    BBox b;
    for (auto& d : p.pads) b.add(d.pos);
    return b.empty() ? Vec2{} : b.center();
}

// ---------------------------------------------------------------------------
// wizard
// ---------------------------------------------------------------------------
std::vector<std::string> wizard_queue(const Project& prj) {
    const auto un = unresolved_packages(prj);
    std::vector<std::string> q, sk;
    for (auto& n : un) (contains(prj.pkg_wizard.skipped, n) ? sk : q).push_back(n);
    for (auto& n : prj.pkg_wizard.skipped) if (contains(sk, n)) q.push_back(n);   // revisit in skip order
    return q;
}

WizardProgress wizard_progress(const Project& prj) {
    WizardProgress w;
    const auto refd = referenced_packages(prj);
    const auto un = unresolved_packages(prj);
    for (auto& n : refd) {
        const bool unres = contains(un, n);
        if (!unres && contains(prj.pkg_wizard.done, n)) ++w.done;
        if (unres || contains(prj.pkg_wizard.done, n)) ++w.total;
        if (unres && contains(prj.pkg_wizard.skipped, n)) ++w.skipped;
    }
    w.remaining = (int)un.size();
    return w;
}

std::string wizard_resume(Project& prj) {
    auto& w = prj.pkg_wizard;
    const auto q = wizard_queue(prj);
    w.started = true;
    if (!w.current.empty() && contains(q, w.current)) return w.current;
    w.current = q.empty() ? std::string() : q.front();
    w.placement = 0;
    return w.current;
}

std::string wizard_advance(Project& prj) {
    auto& w = prj.pkg_wizard;
    const auto q = wizard_queue(prj);
    w.placement = 0;
    if (q.empty()) { w.current.clear(); return w.current; }
    auto it = std::find(q.begin(), q.end(), w.current);
    if (it == q.end()) w.current = q.front();
    else { ++it; w.current = it == q.end() ? q.front() : *it; }
    return w.current;
}

void wizard_skip(Project& prj, const std::string& pkg) {
    auto& w = prj.pkg_wizard;
    // the next package is chosen from the order before the skip moves pkg to the end
    const auto q = wizard_queue(prj);
    std::string next;
    auto it = std::find(q.begin(), q.end(), pkg);
    if (it != q.end() && std::next(it) != q.end()) next = *std::next(it);
    erase_all(w.skipped, pkg);
    w.skipped.push_back(pkg);
    w.started = true;
    w.placement = 0;
    if (next.empty() || next == pkg) { const auto q2 = wizard_queue(prj); next = q2.empty() ? std::string() : q2.front(); }
    w.current = next;
    prj.dirty = true;
}

void wizard_mark_done(Project& prj, const std::string& pkg) {
    auto& w = prj.pkg_wizard;
    erase_all(w.skipped, pkg);
    if (!contains(w.done, pkg)) w.done.push_back(pkg);
    w.started = true;
    if (w.current == pkg) { const auto q = wizard_queue(prj); w.current = q.empty() ? std::string() : q.front(); w.placement = 0; }
    prj.dirty = true;
}

int wizard_placement(const Project& prj, const std::string& pkg, int n) {
    std::vector<int> v;
    for (size_t i = 0; i < prj.comps.size(); ++i)
        if (prj.comps[i].feature == Feature::Component && prj.comps[i].package == pkg) v.push_back((int)i);
    if (v.empty()) return -1;
    std::stable_sort(v.begin(), v.end(), [&](int a, int b) { return ref_less(prj.comps[a].ref, prj.comps[b].ref); });
    const int k = ((n % (int)v.size()) + (int)v.size()) % (int)v.size();
    return v[k];
}

} // namespace pcbp
