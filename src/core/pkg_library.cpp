// pkg_library.cpp — master package library file + sync helpers (portable)
#include "pkg_library.h"
#include "fileio.h"
#include "json.h"
#include "project_io.h"
#include <cmath>
#include <cstdio>

namespace pcbp {

bool PackageLibrary::load(const std::string& p, std::string& err) {
    path = p;
    packages.clear();
    read_only = false;
    touch();
    std::vector<uint8_t> raw;
    std::string e2;
    if (!read_file(p, raw, e2)) return true;   // missing file: new empty library
    std::string text(raw.begin(), raw.end());
    json::Value v;
    if (!json::Value::parse(text, v, err)) { err = "Library file is not valid JSON: " + err; return false; }
    if (v["format"].str() != "pcbplib") { err = "Not a PCB Plotter package library (format != pcbplib)"; return false; }
    name = v["name"].str("Master library");
    for (auto& q : v["packages"].items()) {
        Package k = package_from_json(q);
        if (!k.name.empty()) packages[k.name] = k;
    }
    return true;
}

bool PackageLibrary::save(std::string& err) const {
    if (read_only) { err = "Library is read-only"; return false; }
    if (path.empty()) { err = "Library has no file path"; return false; }
    json::Value o = json::Value::object();
    o.set("format", "pcbplib");
    o.set("version", 1);
    o.set("name", name.empty() ? std::string("Master library") : name);
    json::Value a = json::Value::array();
    for (auto& kv : packages) a.push(package_to_json(kv.second));
    o.set("packages", a);
    std::string s = o.dump(1);
    return write_file(path, s.data(), s.size(), err);
}

static void canon_num(std::string& s, double v) {
    char b[32];
    long long q = std::llround(v * 10000);
    snprintf(b, sizeof b, "%lld,", q);
    s += b;
}
static void canon_prim(std::string& s, const GPrim& g) {
    s += "g"; canon_num(s, (int)g.kind); canon_num(s, g.width); canon_num(s, g.filled); canon_num(s, g.radius);
    canon_num(s, g.a0); canon_num(s, g.a1); canon_num(s, g.rot); canon_num(s, g.text_h); s += g.text; s += "|";
    for (auto& p : g.pts) { canon_num(s, p.x); canon_num(s, p.y); }
    for (auto& c : g.children) canon_prim(s, c);
    s += ";";
}

std::string package_canon(const Package& p) {
    std::string s;
    s.reserve(64 + p.pads.size() * 64);
    canon_num(s, p.smt); canon_num(s, p.j_lead); canon_num(s, p.radial); canon_num(s, p.notched);
    canon_num(s, p.pin_marked); canon_num(s, p.polarized); canon_num(s, p.height);
    s += "P";
    for (auto& d : p.pads) {
        canon_num(s, d.pos.x); canon_num(s, d.pos.y); canon_num(s, d.w); canon_num(s, d.h); canon_num(s, d.rot);
        canon_num(s, (int)d.shape); canon_num(s, d.drill); canon_num(s, d.pin); s += ";";
    }
    s += "B";
    for (auto& g : p.body) canon_prim(s, g);
    s += "M";
    canon_num(s, p.orient_class); canon_num(s, p.marker_kind); canon_num(s, (double)p.marker_color);
    canon_num(s, p.marker_user_pos); canon_num(s, p.marker_pos.x); canon_num(s, p.marker_pos.y); canon_num(s, p.marker_size);
    if (p.marker_kind && p.marker_rot) canon_num(s, p.marker_rot);    // R15 arrow direction (absent: unchanged canon)
    for (auto& g : p.marker_art) canon_prim(s, g);
    return s;
}

bool package_same(const Package& a, const Package& b) {
    if (a.pads.size() != b.pads.size() || a.body.size() != b.body.size()) return false;
    return package_canon(a) == package_canon(b);
}

SyncState sync_state(const std::map<std::string, Package>& local, const std::map<std::string, Package>& lib, const std::string& name, bool from_local) {
    auto l = local.find(name);
    auto m = lib.find(name);
    if (from_local) {
        if (m == lib.end()) return SyncState::LocalOnly;
        if (l == local.end()) return SyncState::LocalOnly;
        return package_same(l->second, m->second) ? SyncState::Same : SyncState::Differs;
    }
    if (l == local.end()) return SyncState::LibraryOnly;
    if (m == lib.end()) return SyncState::LibraryOnly;
    return package_same(l->second, m->second) ? SyncState::Same : SyncState::Differs;
}

std::string unique_package_name(const std::map<std::string, Package>& m, const std::string& base, const char* suffix) {
    std::string n = base.empty() ? "PACKAGE" : base;
    if (!m.count(n)) return n;
    for (int i = 2;; ++i) { std::string t = n + suffix + std::to_string(i); if (!m.count(t)) return t; }
}

static bool pkg_meta_same(const Package& a, const Package& b) {
    return a.description == b.description && a.default_process == b.default_process && a.polarity_set == b.polarity_set &&
           a.leads == b.leads && a.body_source == b.body_source && package_canon(a) == package_canon(b);
}
std::vector<std::string> pkg_edit_many(Project& prj, const std::vector<std::string>& pkgs, const std::function<void(Package&)>& f) {
    std::vector<std::string> done;
    for (auto& k : pkgs) {
        auto it = prj.packages.find(k);
        if (it == prj.packages.end()) continue;
        Package nv = it->second;
        f(nv);
        nv.name = k;
        if (pkg_meta_same(nv, it->second)) continue;
        it->second = std::move(nv);
        done.push_back(k);
    }
    if (!done.empty()) prj.dirty = true;
    return done;
}
} // namespace pcbp
