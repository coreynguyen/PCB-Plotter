// ============================================================================
// content_detect.cpp — recursive content detection + import planning (see .h).
// Portable (no Win32); used by the shell integration and by tests.
// ============================================================================
#include "content_detect.h"
#include "fileio.h"
#include "../io/archive.h"
#include "../io/gerber.h"
#include "../io/juki_import.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace pcbp::detect {

namespace fs = std::filesystem;

namespace {

fs::path upath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool ends_with(const std::string& s, const char* t) { size_t n = std::strlen(t); return s.size() >= n && s.compare(s.size() - n, n, t) == 0; }
std::string base_name(const std::string& p) { size_t s = p.find_last_of("/\\"); return s == std::string::npos ? p : p.substr(s + 1); }
std::string dir_of(const std::string& v) { size_t s = v.find_last_of('/'); return s == std::string::npos ? std::string() : v.substr(0, s); }
std::string ext_of(const std::string& name) {
    std::string b = lower(base_name(name));
    if (ends_with(b, ".tar.gz")) return "tgz";
    size_t d = b.find_last_of('.');
    return d == std::string::npos ? std::string() : b.substr(d + 1);
}
bool in(const std::string& e, std::initializer_list<const char*> l) { for (auto* x : l) if (e == x) return true; return false; }

// normalised header key: lower-case alphanumerics only ("Center-X(mm)" -> "centerxmm")
std::string norm(const std::string& s) { std::string o; for (char c : s) if (std::isalnum((unsigned char)c)) o += (char)std::tolower((unsigned char)c); return o; }

bool starts_any(const std::string& k, std::initializer_list<const char*> l) { for (auto* x : l) if (k.rfind(x, 0) == 0) return true; return false; }

std::vector<std::string> split_cells(const std::string& line) {
    char sep = 0;
    for (char c : {'\t', ',', ';', '|'}) if (line.find(c) != std::string::npos) { sep = c; break; }
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (char c : line) {
        if (c == '"') { q = !q; continue; }
        if (!q && (sep ? c == sep : (c == ' '))) { if (sep || !cur.empty()) out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    if (!cur.empty() || sep) out.push_back(cur);
    return out;
}

struct TextGuess { int centroid = 0, bom = 0; };

// Header keyword heuristics for centroid (XY) and BOM text. Scans the first ~40 lines.
TextGuess guess_text_table(const std::string& name, const std::string& t) {
    TextGuess g;
    size_t bin = 0;
    for (size_t i = 0; i < std::min<size_t>(t.size(), 4096); ++i) { unsigned char c = (unsigned char)t[i]; if (c < 9 || (c > 13 && c < 32 && c != 26)) ++bin; }
    if (bin > 4 || t.empty()) return g;
    int lines = 0, rowlike = 0;
    for (size_t p = 0; p < t.size() && lines < 40;) {
        size_t e = t.find('\n', p); if (e == std::string::npos) e = t.size();
        std::string l = t.substr(p, e - p); p = e + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        size_t a = l.find_first_not_of(" \t#*;/");
        if (a == std::string::npos) continue;
        ++lines;
        auto cells = split_cells(l.substr(a));
        bool ref = false, x = false, y = false, rot = false, qty = false, part = false, refs_list = false;
        for (auto& c0 : cells) {
            const std::string k = norm(c0);
            if (k.empty()) continue;
            if (in(k, {"ref", "refs", "refdes", "reference", "references", "designator", "designators", "refdesignator", "partreference", "refdesignators", "component", "comp", "location", "locations", "position", "positions"})) ref = true;
            if (in(k, {"x", "xmm", "xmil", "xmils", "xin", "xinch"}) || starts_any(k, {"posx", "centerx", "centrex", "midx", "refx", "locationx", "xloc", "xcoord", "xpos", "xposition", "coordx"})) x = true;
            if (in(k, {"y", "ymm", "ymil", "ymils", "yin", "yinch"}) || starts_any(k, {"posy", "centery", "centrey", "midy", "refy", "locationy", "yloc", "ycoord", "ypos", "yposition", "coordy"})) y = true;
            if (starts_any(k, {"rot", "angle", "orient", "theta"})) rot = true;
            if (in(k, {"qty", "quantity", "qnty", "count", "qtyper", "qtyperboard", "usage"}) || starts_any(k, {"qty", "quantity"})) qty = true;
            if (in(k, {"pn", "mpn", "part", "partno", "partnumber", "value", "description", "comment", "manufacturerpartnumber", "internalpn", "cpn", "itemnumber", "stockcode"}) || starts_any(k, {"partnumber", "manufacturerpart", "mfrpart", "mfgpart"})) part = true;
            // consolidated reference lists inside a cell: "C1,C2" / "R1-R4"
            if (c0.size() > 3 && (c0.find(',') != std::string::npos || c0.find(' ') != std::string::npos) && std::isalpha((unsigned char)c0[0])) {
                int refs = 0; std::string tok;
                for (char ch : c0 + ",") { if (ch == ',' || ch == ' ') { if (tok.size() >= 2 && std::isalpha((unsigned char)tok[0]) && std::isdigit((unsigned char)tok.back())) ++refs; tok.clear(); } else tok += ch; }
                if (refs >= 2) refs_list = true;
            }
        }
        if (ref && x && y) g.centroid = std::max(g.centroid, rot ? 88 : 80);
        if (ref && (qty || part) && !(x && y)) g.bom = std::max(g.bom, qty && part ? 85 : 70);
        if (refs_list) g.bom = std::max(g.bom, 50);
        // headerless centroid row: REF  num  num  [num] ...
        if (cells.size() >= 3) {
            const std::string& r = cells[0];
            bool isref = r.size() >= 2 && r.size() <= 12 && std::isalpha((unsigned char)r[0]) && std::isdigit((unsigned char)r.back());
            int nums = 0;
            for (size_t i = 1; i < cells.size(); ++i) {
                const std::string& c = cells[i];
                char* end = nullptr;
                if (!c.empty()) { std::strtod(c.c_str(), &end); if (end && *end == 0) ++nums; }
            }
            if (isref && nums >= 2) ++rowlike;
        }
    }
    if (rowlike >= 3 && !g.centroid) g.centroid = 45;
    const std::string n = lower(base_name(name));
    for (const char* h : {"bom", "bill", "material", "partlist", "parts"}) if (n.find(h) != std::string::npos) { if (g.bom) g.bom += 8; else if (rowlike == 0 && lines > 2) g.bom = 30; }
    for (const char* h : {"pick", "place", "pnp", "xy", "centroid", "pos", "insert", "mount", "smt"}) if (n.find(h) != std::string::npos && g.centroid) g.centroid += 8;
    if (g.centroid && g.bom) { if (g.centroid >= g.bom) g.bom = 0; else g.centroid = 0; }
    g.centroid = std::min(g.centroid, 92); g.bom = std::min(g.bom, 92);
    return g;
}

bool is_archive_bytes(const std::string& name, const uint8_t* p, size_t n) {
    const std::string e = ext_of(name);
    // Office / ODF documents are zips but not containers we expand
    if (in(e, {"xlsx", "xlsm", "xltx", "docx", "pptx", "ods", "odt", "jar", "apk", "3mf"})) return false;
    return io::detect_archive(p, n) != io::ArchiveKind::None;
}

struct Scanner {
    const Options& opt;
    Result& r;
    uint64_t total = 0;

    void add_blob(const std::string& vpath, std::vector<uint8_t> data, int depth) {
        if (r.entries.size() >= opt.max_files) return;
        if (depth < opt.max_depth && is_archive_bytes(vpath, data.data(), data.size()) && total + data.size() < opt.max_total) {
            std::vector<io::ArchiveEntry> ents;
            std::string err, gzname = base_name(vpath);
            if (ends_with(lower(gzname), ".gz")) gzname.resize(gzname.size() - 3);
            if (io::read_archive(data, ents, err, gzname)) {
                for (auto& e : ents) {
                    if (e.dir) continue;
                    std::string nm = e.name;
                    std::replace(nm.begin(), nm.end(), '\\', '/');
                    while (nm.rfind("./", 0) == 0) nm.erase(0, 2);
                    while (!nm.empty() && nm[0] == '/') nm.erase(0, 1);
                    if (nm.empty() || nm.find("../") != std::string::npos) continue;   // never escape the tree
                    total += e.data.size();
                    add_blob(vpath + "/" + nm, std::move(e.data), depth + 1);
                }
                return;
            }
            r.warnings.push_back(vpath + ": " + err);
        }
        Entry en;
        en.vpath = vpath;
        en.size = data.size();
        en.data = std::move(data);
        r.entries.push_back(std::move(en));
    }

    void add_disk(const std::string& vpath, const fs::path& p, uint64_t size, int depth) {
        if (r.entries.size() >= opt.max_files) return;
        const std::string e = ext_of(vpath);
        if (depth < opt.max_depth && size < (512ull << 20) && total + size < opt.max_total &&
            in(e, {"zip", "tgz", "tar", "gz", "taz", "odb"})) {
            std::ifstream f(p, std::ios::binary);
            std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (is_archive_bytes(vpath, buf.data(), buf.size())) { total += buf.size(); add_blob(vpath, std::move(buf), depth); return; }
        }
        Entry en;
        en.vpath = vpath;
        auto u8 = p.u8string();
        en.disk_path.assign(u8.begin(), u8.end());
        en.size = size;
        r.entries.push_back(std::move(en));
    }
};

std::string head_of(const Entry& e, size_t n = 256 * 1024) {
    if (!e.disk_path.empty()) {
        std::ifstream f(upath(e.disk_path), std::ios::binary);
        std::string s(n, '\0');
        f.read(s.data(), (std::streamsize)n);
        s.resize((size_t)f.gcount());
        return s;
    }
    return std::string((const char*)e.data.data(), std::min(e.data.size(), n));
}

bool under(const std::string& v, const std::string& root) {
    if (root.empty()) return true;
    return v.size() > root.size() && v.compare(0, root.size(), root) == 0 && v[root.size()] == '/';
}

} // namespace

const char* kind_name(Kind k) {
    switch (k) {
    case Kind::Project: return "PCB Plotter project";
    case Kind::LegacyCpf: return "Legacy CAM project (.cpf)";
    case Kind::LegacyMdb: return "Legacy package library (.mdb)";
    case Kind::ViewMateJob: return "ViewMate job (.bin)";
    case Kind::Odb: return "ODB++ job";
    case Kind::Ipc2581: return "IPC-2581";
    case Kind::Cad: return "CAD data";
    case Kind::PnpProgram: return "Pick-and-place program";
    case Kind::Gerber: return "Gerber layer";
    case Kind::Drill: return "Excellon drill";
    case Kind::ApertureList: return "Aperture list";
    case Kind::Centroid: return "Centroid / XY";
    case Kind::Bom: return "BOM";
    case Kind::Spreadsheet: return "Spreadsheet";
    case Kind::Archive: return "Archive";
    default: return "Unknown";
    }
}

const char* action_name(Action a) {
    switch (a) {
    case Action::OpenProject: return "Open project";
    case Action::ImportFile: return "Import";
    case Action::ImportOdb: return "Import ODB++ job";
    case Action::LoadGerbers: return "Load Gerber layers";
    case Action::ImportCentroid: return "Import centroid";
    case Action::ImportBom: return "Import BOM";
    case Action::OpenSheet: return "Open in spreadsheet editor";
    case Action::Unsupported: return "Not supported";
    default: return "None";
    }
}

Kind classify(const std::string& name, const std::string& head, int& score, std::string& importer) {
    score = 0;
    importer.clear();
    const std::string e = ext_of(name);
    const std::string ln = lower(name);
    auto set = [&](Kind k, int s, const char* id = "") { score = s; importer = id; return k; };
    if (ends_with(ln, "matrix/matrix") || ln == "matrix") return set(Kind::Odb, 90, "odbpp");
    if (head.size() >= 19 && (head.compare(4, 15, "Standard Jet DB") == 0 || head.compare(4, 15, "Standard ACE DB") == 0))
        return e == "mdb" ? set(Kind::LegacyMdb, 90, "circuitcam.cpf") : set(Kind::LegacyCpf, 92, "circuitcam.cpf");
    if (head.substr(0, 96).find("V2001 JOB") != std::string::npos) return set(Kind::ViewMateJob, 90, "viewmate.bin");
    if (e == "pcbp") {
        size_t a = head.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
        return set(Kind::Project, a != std::string::npos && head[a] == '{' ? 98 : 40, "pcbp.json");
    }
    const std::string h8 = head.substr(0, 8192);
    if (h8.find("<IPC-2581") != std::string::npos || h8.find("webstds.ipc.org/2581") != std::string::npos) return set(Kind::Ipc2581, 95, "ipc2581");
    {
        const int j = juki::sniff_juki(std::vector<uint8_t>(head.begin(), head.begin() + std::min<size_t>(head.size(), 8192)));
        const bool pe = in(e, {"h8h", "h7h", "h5h", "h4h", "e41", "e42", "e43", "e44", "e45", "e46", "e51", "iss"});
        if (j >= 90) return set(Kind::PnpProgram, 90, "juki.hlc");
        if (pe && j >= 10) return set(Kind::PnpProgram, 70, "juki.hlc");
        if (pe && e != "iss") return set(Kind::PnpProgram, 40, "juki.hlc");
    }
    if (is_archive_bytes(name, (const uint8_t*)head.data(), head.size())) return set(Kind::Archive, 20);
    if (in(e, {"xlsx", "xlsm", "xls", "ods"})) return set(Kind::Spreadsheet, 60);
    // text formats
    const TextGuess tg = guess_text_table(name, head);
    if (tg.centroid >= 80) return set(Kind::Centroid, tg.centroid, "centroid");
    if (tg.bom >= 70) return set(Kind::Bom, tg.bom, "bom");
    switch (gerber::classify_file(head)) {
    case gerber::FileKind::Rs274x: return set(Kind::Gerber, 88, "gerber");
    case gerber::FileKind::Excellon: return set(Kind::Drill, 86, "excellon");
    case gerber::FileKind::Rs274d: return set(Kind::Gerber, 70, "gerber");
    case gerber::FileKind::ApertureList: return set(Kind::ApertureList, 70);
    case gerber::FileKind::Ipc356: return set(Kind::Cad, 75, "ipc356");
    default: break;
    }
    if (tg.centroid) return set(Kind::Centroid, tg.centroid, "centroid");
    if (tg.bom) return set(Kind::Bom, tg.bom, "bom");
    if (e == "csv" || e == "tsv") return set(Kind::Spreadsheet, 20);
    return set(Kind::Unknown, 0);
}

const Entry* Result::find(const std::string& vpath) const {
    for (auto& e : entries) if (e.vpath == vpath) return &e;
    return nullptr;
}

bool entry_bytes(const Entry& e, std::string& out, std::string& err) {
    if (!e.disk_path.empty()) return read_text_file(e.disk_path, out, err);
    out.assign(e.data.begin(), e.data.end());
    return true;
}

static void build_plans(Result& r, const Options& opt) {
    // classify
    for (auto& e : r.entries) {
        const std::string head = head_of(e);
        e.kind = classify(e.vpath, head, e.score, e.importer);
        if ((e.kind == Kind::Unknown || (e.kind == Kind::Spreadsheet && e.score < 50)) && opt.sniff) {
            std::string id; Kind k = Kind::Cad;
            int s = opt.sniff(e.vpath, head.substr(0, 8192), id, k);
            if (s >= 60 && !id.empty()) { e.kind = k; e.score = std::min(s, 89); e.importer = id; }
        }
    }
    // ODB++ roots (shortest root per job; nested jobs ignored)
    std::vector<std::string> odb_roots;
    for (auto& e : r.entries) {
        if (e.kind != Kind::Odb) continue;
        std::string v = e.vpath;
        if (v.size() > 13 && v[v.size() - 14] != '/') continue;
        std::string root = v.size() > 13 ? v.substr(0, v.size() - 14) : std::string();   // strip "/matrix/matrix"
        if (lower(v) == "matrix/matrix") root.clear();
        bool dup = false;
        for (auto& o : odb_roots) if (o == root || under(root, o)) dup = true;
        if (!dup) odb_roots.push_back(root);
    }
    auto in_odb = [&](const std::string& v) { for (auto& o : odb_roots) if (under(v, o)) return true; return false; };

    std::vector<std::string> gerbers, aplists, centroid, bom;
    for (auto& e : r.entries) {
        if (in_odb(e.vpath)) continue;
        if (e.kind == Kind::Gerber || e.kind == Kind::Drill) gerbers.push_back(e.vpath);
        else if (e.kind == Kind::ApertureList) aplists.push_back(e.vpath);
        else if (e.kind == Kind::Centroid) centroid.push_back(e.vpath);
        else if (e.kind == Kind::Bom) bom.push_back(e.vpath);
    }
    auto follow = [&](Plan& p, bool with_gerbers) {
        p.centroid = centroid; p.bom = bom;
        if (with_gerbers) { p.gerbers = gerbers; p.aperture_lists = aplists; }
    };
    for (auto& o : odb_roots) {
        Plan p; p.action = Action::ImportOdb; p.kind = Kind::Odb; p.importer = "odbpp"; p.files = {o}; p.score = 95;
        p.title = "ODB++ job" + (o.empty() ? std::string() : " '" + o + "'");
        follow(p, false);
        r.plans.push_back(p);
    }
    for (auto& e : r.entries) {
        if (in_odb(e.vpath)) continue;
        Plan p; p.kind = e.kind; p.importer = e.importer; p.files = {e.vpath}; p.score = e.score;
        switch (e.kind) {
        case Kind::Project: p.action = Action::OpenProject; p.score = 100; break;
        case Kind::Ipc2581: case Kind::LegacyCpf: case Kind::Cad: case Kind::PnpProgram: p.action = Action::ImportFile; follow(p, e.kind != Kind::PnpProgram); break;
        case Kind::LegacyMdb: p.action = Action::ImportFile; p.score = 35; break;
        case Kind::ViewMateJob: p.action = Action::ImportFile; p.score = 88; follow(p, false); break;   // viewmate_plugin.cpp
        default: continue;
        }
        if (e.kind == Kind::Ipc2581) p.score = 93;
        p.title = std::string(kind_name(e.kind)) + " '" + e.vpath + "'";
        r.plans.push_back(p);
    }
    if (!gerbers.empty()) {
        // one plan per folder holding layers; the folder with most layers first
        std::map<std::string, std::vector<std::string>> by_dir;
        for (auto& g : gerbers) by_dir[dir_of(g)].push_back(g);
        // folders that are parent/child of each other are one set (gerbers/ + gerbers/drill/)
        std::vector<std::pair<std::string, std::vector<std::string>>> sets;
        for (auto& [d, v] : by_dir) {
            bool merged = false;
            for (auto& s : sets) if (under(d, s.first) || d == s.first) { s.second.insert(s.second.end(), v.begin(), v.end()); merged = true; break; }
            if (!merged) sets.push_back({d, v});
        }
        std::stable_sort(sets.begin(), sets.end(), [](auto& a, auto& b) { return a.second.size() > b.second.size(); });
        for (auto& [d, v] : sets) {
            Plan p; p.action = Action::LoadGerbers; p.kind = Kind::Gerber; p.files = v;
            for (auto& a : aplists) {   // same folder, a parent folder or a sub-folder
                const std::string ad = dir_of(a);
                if (ad == d || under(ad, d) || under(d, ad) || (ad.empty())) p.aperture_lists.push_back(a);
            }
            bool all_d = true, any_d = false;
            for (auto& f : v) { const Entry* e = r.find(f); if (e && e->score != 70) all_d = false; if (e && e->score == 70) any_d = true; }
            p.score = all_d && p.aperture_lists.empty() ? 70 : 82;
            (void)any_d;
            p.title = std::to_string(v.size()) + " Gerber / drill layer(s)" + (d.empty() ? std::string() : " in '" + d + "'");
            p.centroid = centroid; p.bom = bom;
            r.plans.push_back(p);
        }
    }
    if (!centroid.empty()) {
        Plan p; p.action = Action::ImportCentroid; p.kind = Kind::Centroid; p.importer = "centroid"; p.files = {centroid.front()};
        p.bom = bom; p.score = 62; p.title = "Centroid '" + centroid.front() + "'" + (bom.empty() ? "" : " + BOM");
        r.plans.push_back(p);
    }
    if (!bom.empty()) {
        Plan p; p.action = Action::ImportBom; p.kind = Kind::Bom; p.importer = "bom"; p.files = {bom.front()}; p.score = 55;
        p.title = "BOM '" + bom.front() + "'";
        r.plans.push_back(p);
    }
    for (auto& e : r.entries) {
        if (e.kind != Kind::Spreadsheet || in_odb(e.vpath)) continue;
        Plan p; p.action = Action::OpenSheet; p.kind = Kind::Spreadsheet; p.files = {e.vpath}; p.score = e.score >= 50 ? 50 : 20;
        p.title = "Spreadsheet '" + e.vpath + "'";
        r.plans.push_back(p);
    }
    std::stable_sort(r.plans.begin(), r.plans.end(), [](const Plan& a, const Plan& b) { return a.score > b.score; });
}

bool scan_entries(std::vector<Entry> entries, Result& out, const Options& opt) {
    out.entries.clear(); out.plans.clear();
    Scanner sc{opt, out};
    for (auto& e : entries) {
        if (!e.disk_path.empty()) { out.entries.push_back(std::move(e)); continue; }
        sc.total += e.data.size();
        sc.add_blob(e.vpath, std::move(e.data), 0);
    }
    build_plans(out, opt);
    return true;
}

bool scan(const std::string& path, Result& out, std::string& err, const Options& opt) {
    out = Result{};
    out.source = path;
    std::error_code ec;
    const fs::path p = upath(path);
    Scanner sc{opt, out};
    if (fs::is_directory(p, ec)) {
        out.source_is_dir = true;
        for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it.depth() > 12) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(ec)) continue;
            auto rel = fs::relative(it->path(), p, ec).generic_u8string();
            sc.add_disk(std::string(rel.begin(), rel.end()), it->path(), (uint64_t)it->file_size(ec), 0);
            if (out.entries.size() >= opt.max_files) { out.warnings.push_back("file limit reached; scan truncated"); break; }
        }
    } else if (fs::is_regular_file(p, ec)) {
        const uint64_t size = fs::file_size(p, ec);
        std::string head;
        {
            std::ifstream f(p, std::ios::binary);
            head.resize(16);
            f.read(head.data(), 16);
            head.resize((size_t)f.gcount());
        }
        const std::string name = base_name(path);
        if (is_archive_bytes(name, (const uint8_t*)head.data(), head.size())) {
            out.source_is_archive = true;
            std::vector<uint8_t> buf;
            if (!read_file(path, buf, err)) return false;
            std::vector<io::ArchiveEntry> ents;
            std::string gz = name;
            if (ends_with(lower(gz), ".gz")) gz.resize(gz.size() - 3);
            if (!io::read_archive(buf, ents, err, gz)) return false;
            buf.clear(); buf.shrink_to_fit();
            for (auto& e : ents) {
                if (e.dir) continue;
                std::string nm = e.name;
                std::replace(nm.begin(), nm.end(), '\\', '/');
                while (nm.rfind("./", 0) == 0) nm.erase(0, 2);
                while (!nm.empty() && nm[0] == '/') nm.erase(0, 1);
                if (nm.empty() || nm.find("../") != std::string::npos) continue;
                sc.total += e.data.size();
                sc.add_blob(nm, std::move(e.data), 1);
            }
        } else {
            Entry en; en.vpath = name; en.disk_path = path; en.size = size;
            out.entries.push_back(std::move(en));
        }
    } else {
        err = "not found: " + path;
        return false;
    }
    build_plans(out, opt);
    return true;
}

std::string Result::summary() const {
    std::string s;
    std::map<Kind, int> n;
    for (auto& e : entries) n[e.kind]++;
    s += std::to_string(entries.size()) + " file(s):";
    for (auto& [k, c] : n) s += std::string(" ") + std::to_string(c) + " " + kind_name(k) + ";";
    s += "\n";
    for (auto& p : plans) s += std::string("  [") + std::to_string(p.score) + "] " + action_name(p.action) + ": " + p.title + "\n";
    for (auto& w : warnings) s += "  warning: " + w + "\n";
    return s;
}

std::string materialized_path(const std::string& vprefix, const std::string& dest_dir, const std::string& vpath) {
    std::string rel = vpath;
    if (!vprefix.empty()) rel = vpath == vprefix ? base_name(vpath) : vpath.substr(vprefix.size() + 1);
    fs::path out = upath(dest_dir) / upath(rel);
    auto u8 = out.make_preferred().u8string();
    return std::string(u8.begin(), u8.end());
}

bool materialize(const Result& r, const std::string& vprefix, const std::string& dest_dir,
                 std::vector<std::string>& written, std::string& err) {
    std::error_code ec;
    for (auto& e : r.entries) {
        if (!vprefix.empty() && e.vpath != vprefix && !under(e.vpath, vprefix)) continue;
        const std::string dst = materialized_path(vprefix, dest_dir, e.vpath);
        fs::create_directories(upath(dst).parent_path(), ec);
        std::string bytes;
        if (!entry_bytes(e, bytes, err)) return false;
        if (!write_file(dst, bytes.data(), bytes.size(), err)) return false;
        written.push_back(dst);
    }
    if (written.empty()) { err = "nothing to extract under '" + vprefix + "'"; return false; }
    return true;
}

} // namespace pcbp::detect
