// ============================================================================
// gerber_set.cpp — see gerber_set.h. Rules moved from gui/gerber_workspace.cpp
// (dlg_import_gerber: add_content / finish_add / parse_ent, guess_role, role_color,
// stem_key) and app/doc.cpp (resolve_format / parse_layer).
// ============================================================================
#include "gerber_set.h"
#include "fileio.h"
#include "../io/archive.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace pcbp::gset {

namespace {
fs::path upath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string u8(const fs::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
} // namespace

uint32_t role_color(LayerRole r, int n) {
    switch (r) {
    case LayerRole::Copper: return 0xFFD08A30;
    case LayerRole::Paste: return 0xFFB8B8C0;
    case LayerRole::Mask: return 0xFF3DA35D;
    case LayerRole::Silk: return 0xFFF0F0E6;
    case LayerRole::Assembly: return 0xFF4FB8E0;
    case LayerRole::Drill: return 0xFFE05050;
    case LayerRole::Outline: return 0xFFE8D84A;
    default: break;
    }
    static const uint32_t cyc[] = {0xFFC08040, 0xFF8070E0, 0xFF40C0A0, 0xFFE070B0, 0xFFA0A040};
    return cyc[(n < 0 ? -n : n) % 5];
}

void guess_role(GerberLayerRef& r, int n, std::string* why) {
    const gerber::LayerGuess g = gerber::guess_layer_function(r.path.empty() ? r.name : r.path, r.content);
    r.role = (LayerRole)std::clamp(g.role, 0, 7);
    r.side = g.side == 1 ? Side::Bottom : Side::Top;
    r.color = role_color(r.role, n);
    if (why) *why = g.known ? g.why : std::string("no convention matched");
}

bool parse_format_string(const std::string& s, gerber::Format& fmt) {
    if (s.empty()) return false;
    char u[8] = {}, z = 'L';
    int a = 2, b = 4;
    if (std::sscanf(s.c_str(), "%7s %d.%d %c", u, &a, &b, &z) < 3) return false;
    fmt.inch = std::string(u) != "mm";
    fmt.int_digits = a;
    fmt.frac_digits = b;
    fmt.lead_supp = z != 'T';
    return true;
}
std::string format_string(const gerber::Format& f) {
    return std::string(f.inch ? "in " : "mm ") + std::to_string(f.int_digits) + "." + std::to_string(f.frac_digits) + (f.lead_supp ? " L" : " T");
}
bool is_rs274d(const GerberLayerRef& l) { return !gerber::looks_like_rs274x(l.content) && !gerber::looks_like_excellon(l.content); }
gerber::ListUnits list_units(const std::string& h) {
    return h == "mil" ? gerber::ListUnits::Mil : h == "inch" ? gerber::ListUnits::Inch : h == "mm" ? gerber::ListUnits::MM : gerber::ListUnits::Auto;
}

bool parse_layer_ref(GerberLayerRef& ref, gerber::Layer& out, std::string& err) {
    gerber::ParseOptions opt;
    if (!ref.aperture_list.empty()) opt.apertures = gerber::parse_aperture_list(ref.aperture_list, list_units(ref.units_hint)).aps;
    gerber::Format f;
    if (parse_format_string(ref.format, f)) { opt.fmt = f; opt.force_format = true; }
    else if (is_rs274d(ref)) {
        auto cands = gerber::guess_format(ref.content, opt.apertures);
        if (!cands.empty()) {
            opt.fmt = cands.front().fmt;
            opt.force_format = true;
            ref.format = format_string(opt.fmt);
        }
    }
    out = gerber::Layer{};
    if (!gerber::parse(ref.content, opt, out, err)) return false;
    out.name = ref.name;
    return true;
}

std::string aperture_stem_key(const std::string& path) {
    std::string s = lower(file_stem(path));
    for (const char* suf : {"_apertures", "-apertures", "_aperture", ".apr", "_apt", "-apt", ".apt", "_rep", "_list"}) {
        size_t p = s.rfind(suf);
        if (p != std::string::npos && p + std::strlen(suf) == s.size()) s = s.substr(0, p);
    }
    return s;
}

int stack_rank(const GerberLayerRef& l) {
    const gerber::LayerGuess g = gerber::guess_layer_function(l.path, l.content.substr(0, 8192));
    switch (l.role) {
    case LayerRole::Drill: return 9000;
    case LayerRole::Outline: return 8000;
    case LayerRole::Other: return 7000;
    default: break;
    }
    auto sub = [](LayerRole r) {
        switch (r) { case LayerRole::Assembly: return 0; case LayerRole::Silk: return 1; case LayerRole::Mask: return 2;
                     case LayerRole::Paste: return 3; case LayerRole::Copper: return 4; default: return 5; }
    };
    if (g.side == 2) return 2000 + (99 - std::min(99, g.inner));
    if (l.side == Side::Bottom) return 1000 + sub(l.role);
    return 3000 + (5 - sub(l.role));
}

bool load_gerber_set(const std::vector<std::string>& paths, SetResult& out, std::string& err, const SetOptions& opt) {
    out = SetResult{};
    err.clear();
    std::vector<GerberLayerRef> cand;
    std::map<std::string, std::string> ap_by_stem;   // aperture lists by stem key
    std::string ap_any;                              // the first aperture list (fallback for a single-list set)
    size_t files = 0;
    bool read_any = false;

    std::function<void(const std::string&, const std::string&, int)> add_content = [&](const std::string& path, const std::string& content, int depth) {
        if (++files > opt.max_files) return;
        read_any = true;
        const std::string ext = file_ext_lower(path);
        std::vector<uint8_t> bytes(content.begin(), content.end());
        if (io::detect_archive(bytes.data(), bytes.size()) != io::ArchiveKind::None) {
            if (depth >= opt.max_depth) { out.skipped.push_back({path, "archive too deep", ""}); return; }
            std::vector<io::ArchiveEntry> ents;
            std::string e;
            if (!io::read_archive(bytes, ents, e, file_stem(path))) { out.skipped.push_back({path, "archive", e}); return; }
            for (auto& a : ents) if (!a.dir) add_content(path + "/" + a.name, std::string(a.data.begin(), a.data.end()), depth + 1);
            return;
        }
        if (ext == ".gbrjob") { out.skipped.push_back({path, "not a Gerber", "Gerber job file (JSON); layer functions come from the X2 attributes in each layer"}); return; }
        switch (gerber::classify_file(content)) {
        case gerber::FileKind::Rs274x: case gerber::FileKind::Rs274d: case gerber::FileKind::Excellon: {
            GerberLayerRef r;
            r.path = path;
            r.content = content;
            r.name = file_stem(path);
            cand.push_back(std::move(r));
            break;
        }
        case gerber::FileKind::ApertureList:
            ap_by_stem[aperture_stem_key(path)] = content;
            if (ap_any.empty()) ap_any = content;
            out.notes.push_back(file_stem(path) + ext + ": aperture list");
            break;
        case gerber::FileKind::Ipc356: out.skipped.push_back({path, "not a Gerber", "IPC-D-356 netlist, not a layer"}); break;
        default: out.skipped.push_back({path, "not a Gerber", "not recognised as a Gerber, Excellon drill or aperture list file"}); break;
        }
    };
    std::function<void(const fs::path&, int)> add_path = [&](const fs::path& p, int depth) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            if (depth >= opt.max_depth) { out.skipped.push_back({u8(p), "folder too deep", ""}); return; }
            std::vector<fs::path> kids;
            for (auto it = fs::directory_iterator(p, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) kids.push_back(it->path());
            std::sort(kids.begin(), kids.end());
            for (auto& k : kids) {
                const std::string fn = u8(k.filename());
                if (fn.empty() || fn[0] == '.') continue;
                add_path(k, depth + 1);
            }
            return;
        }
        std::vector<uint8_t> data;
        std::string e;
        if (!read_file(u8(p), data, e)) { out.skipped.push_back({u8(p), "cannot read", e}); return; }
        add_content(u8(p), std::string(data.begin(), data.end()), depth);
    };
    for (auto& s : paths) add_path(upath(s), 0);
    if (!read_any) { err = "No readable files."; return false; }

    // 274D: aperture list by stem (exact, then prefix either way), else the only list
    for (auto& l : cand) {
        if (!is_rs274d(l)) continue;
        const std::string sk = aperture_stem_key(l.path);
        auto it = ap_by_stem.find(sk);
        if (it == ap_by_stem.end())
            for (auto j = ap_by_stem.begin(); j != ap_by_stem.end(); ++j)
                if (!j->first.empty() && (sk.rfind(j->first, 0) == 0 || j->first.rfind(sk, 0) == 0)) { it = j; break; }
        if (it != ap_by_stem.end()) l.aperture_list = it->second;
        else if (ap_by_stem.size() == 1) l.aperture_list = ap_any;
        if (l.aperture_list.empty()) out.notes.push_back(l.name + ": RS-274D without an aperture list (shapes default; add one)");
    }
    // parse: empty / failed files go to skipped
    int n = 0;
    for (auto& r : cand) {
        Loaded L;
        L.ref = std::move(r);
        guess_role(L.ref, n, &L.why);
        gerber::Layer g;
        std::string e;
        if (!parse_layer_ref(L.ref, g, e)) { out.skipped.push_back({L.ref.path, "parse error", e}); continue; }
        L.objects = (int)g.objs.size();
        L.flashes = g.flash_count();
        L.draws = g.draw_count();
        L.regions = g.region_count();
        L.apertures = (int)g.aps.size();
        L.w = g.extent.w();
        L.h = g.extent.h();
        L.excellon = g.excellon;
        for (size_t i = 0; i < g.warnings.size() && i < 5; ++i) L.warnings.push_back(g.warnings[i]);
        if (g.objs.empty()) {
            if (!opt.keep_empty) {
                out.skipped.push_back({L.ref.path, "empty layer", std::string("valid ") + (g.excellon ? "Excellon" : "Gerber") + " file without any objects"});
                continue;
            }
            L.ref.visible = false;
        }
        ++n;
        out.layers.push_back(std::move(L));
    }
    std::stable_sort(out.layers.begin(), out.layers.end(), [](const Loaded& a, const Loaded& b) { return stack_rank(a.ref) < stack_rank(b.ref); });
    for (size_t i = 0; i < out.layers.size(); ++i)
        if (out.layers[i].ref.role == LayerRole::Other) out.layers[i].ref.color = role_color(LayerRole::Other, (int)i);
    if (out.layers.empty() && err.empty()) err = "No Gerber or drill layers found (" + std::to_string(out.skipped.size()) + " file(s) skipped).";
    return true;
}

int add_layers(Project& p, const SetResult& r, const std::string& name_prefix) {
    if (r.layers.empty()) return -1;
    std::set<std::string> names;
    for (auto& l : p.layers) names.insert(lower(l.name));
    const int first = (int)p.layers.size();
    for (auto& L : r.layers) {
        GerberLayerRef ref = L.ref;
        std::string base = name_prefix + ref.name, nm = base;
        for (int k = 2; names.count(lower(nm)); ++k) nm = base + " (" + std::to_string(k) + ")";
        names.insert(lower(nm));
        ref.name = nm;
        ref.digitize = false;
        p.layers.push_back(std::move(ref));
    }
    bool any = false;
    for (auto& l : p.layers) any |= l.digitize;
    if (!any) {
        int best = -1;
        for (LayerRole role : {LayerRole::Paste, LayerRole::Copper, LayerRole::Mask})
            for (int i = first; i < (int)p.layers.size() && best < 0; ++i)
                if (p.layers[i].role == role && p.layers[i].side == Side::Top) best = i;
        if (best < 0) best = first;
        p.layers[best].digitize = true;
    }
    return first;
}

} // namespace pcbp::gset
