// ============================================================================
// odbpp.cpp — ODB++ import (see odbpp.h, specs/ODB_RE.md). Written from the
// public ODB++ format description (matrix / stephdr / features / eda data /
// components records).
// ============================================================================
#include "odbpp.h"
#include "archive.h"
#include "io_util.h"
#include "lzw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

namespace pcbp::io {
namespace {

std::string strip_attrs(const std::string& l) {
    auto p = l.find(';');
    return p == std::string::npos ? l : l.substr(0, p);
}

// R24a (user v23 #2): ODB++ feature attributes. A record's text after ';' is a comma list of "<n>" (boolean) or
// "<n>=<value>"; <n> indexes the file's "@<n> <name>" attribute-name table (system attributes start with '.').
// Returns the value of attribute `name` (case-insensitive) on line l, or "" when absent.
std::string odb_attr_value(const std::string& l, const std::map<int, std::string>& names, const char* name) {
    auto p = l.find(';');
    if (p == std::string::npos) return {};
    std::string rest = l.substr(p + 1);
    size_t a = 0;
    while (a <= rest.size()) {
        size_t b = rest.find(',', a);
        if (b == std::string::npos) b = rest.size();
        std::string item = rest.substr(a, b - a);
        auto eq = item.find('=');
        const int idx = std::atoi(item.substr(0, eq).c_str());
        auto it = names.find(idx);
        if (it != names.end() && ieq(it->second, name) && eq != std::string::npos) {
            std::string v = item.substr(eq + 1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
            return v;
        }
        a = b + 1;
    }
    return {};
}
// "@<n> <name>" lines of a features / components / eda file
void odb_attr_names(const std::string& l, std::map<int, std::string>& names) {
    if (l.size() < 3 || l[0] != '@') return;
    const char* s = l.c_str() + 1;
    char* e = nullptr;
    const long n = std::strtol(s, &e, 10);
    if (e == s) return;
    std::string nm = e;
    size_t i = 0; while (i < nm.size() && (nm[i] == ' ' || nm[i] == '\t')) ++i;
    nm = nm.substr(i);
    while (!nm.empty() && (nm.back() == ' ' || nm.back() == '\r')) nm.pop_back();
    names[(int)n] = nm;
}

bool ends_with_ci(const std::string& s, const std::string& t) {
    return s.size() >= t.size() && ieq(std::string_view(s).substr(s.size() - t.size()), t);
}

// Declared units of one file: "UNITS=MM|INCH" or legacy "U MM|INCH" (mm per
// unit; 0 = none declared). `word` receives the declaration text.
double declared_units(const std::vector<std::string>& lines, std::string* word = nullptr) {
    size_t n = 0;
    for (auto& l0 : lines) {
        if (++n > 400) break;                       // declarations sit in the header
        std::string l = trim(l0);
        if (istarts_with(l, "UNITS")) {
            auto eq = l.find('=');
            if (eq != std::string::npos) {
                double k = unit_scale_word(trim(l.substr(eq + 1)));
                if (k > 0) { if (word) *word = l; return k; }
            }
        }
        if (l.size() > 2 && (l[0] == 'U' || l[0] == 'u') && l[1] == ' ') {
            double k = unit_scale_word(trim(l.substr(2)));
            if (k > 0) { if (word) *word = l; return k; }
        }
    }
    return 0;
}

// Import-time unit control (REVIEW_V9 §6C): forced source unit (mm per unit,
// 0 = as declared) and arbitrary user scale. Set only for the duration of one
// import_odbpp call (thread-local, so concurrent scans are unaffected).
struct UnitCtl { double force_k = 0; double scale = 1.0; };
thread_local UnitCtl g_units;
struct UnitScope {
    UnitCtl prev;
    UnitScope(double force, double scale) : prev(g_units) { g_units.force_k = force; g_units.scale = scale > 0 ? scale : 1.0; }
    ~UnitScope() { g_units = prev; }
};

// mm per coordinate unit for a whole file; default inch. Includes the forced
// unit / user scale of the running import.
double file_units(const std::vector<std::string>& lines, bool* found = nullptr) {
    double k = declared_units(lines);
    if (found) *found = k > 0;
    if (k <= 0) k = kMmPerInch;
    if (g_units.force_k > 0) k = g_units.force_k;
    return k * g_units.scale;
}

// mm per symbol-size unit ("r12" = 12 mil in inch files, 12 micron in mm files):
// 1/1000 of the coordinate unit; a per-symbol "I"/"M" flag wins unless the unit
// is forced by the user.
double symbol_units(double file_k, const std::string& flag) {
    if (g_units.force_k <= 0) {
        if (ieq(flag, "M")) return 0.001 * g_units.scale;
        if (ieq(flag, "I")) return kMmPerMil * g_units.scale;
    }
    return file_k / 1000.0;
}

// ---------------------------------------------------------------------------
// Job tree: FileTree + transparent .Z / .gz members + nested archives
// ---------------------------------------------------------------------------
struct Job {
    FileTree ft;
    std::string root;
    std::string container;
    mutable int compressed_reads = 0;
    mutable std::vector<std::string> decode_errors;

    // plain, .Z or .gz member
    bool get(const std::string& rel, std::string& out) const {
        if (ft.get(rel, out)) return true;
        std::string raw;
        for (const char* ext : {".Z", ".z", ".gz"}) {
            if (!ft.get(rel + ext, raw)) continue;
            std::vector<uint8_t> dec;
            std::string err;
            const auto* p = (const uint8_t*)raw.data();
            bool ok = is_lzw_compress(p, raw.size()) ? lzw_decompress(p, raw.size(), dec, err)
                                                       : gunzip(p, raw.size(), dec, err);
            if (!ok) { decode_errors.push_back(rel + ext + ": " + err); return false; }
            ++compressed_reads;
            out.assign(dec.begin(), dec.end());
            return true;
        }
        return false;
    }
    bool exists(const std::string& rel) const {
        return ft.exists(rel) || ft.exists(rel + ".Z") || ft.exists(rel + ".z") || ft.exists(rel + ".gz");
    }
};

std::string find_root(const FileTree& ft) {
    std::string best;
    bool found = false;
    for (auto& f : ft.list()) {
        std::string g = f;
        std::replace(g.begin(), g.end(), '\\', '/');
        std::string low = to_lower(g);
        for (std::string tail : {"matrix/matrix", "matrix/matrix.z", "matrix/matrix.gz"}) {
            if (low.size() >= tail.size() && low.compare(low.size() - tail.size(), tail.size(), tail) == 0 &&
                (low.size() == tail.size() || low[low.size() - tail.size() - 1] == '/')) {
                std::string root = g.substr(0, g.size() - tail.size());
                if (!found || root.size() < best.size()) { best = root; found = true; }
            }
        }
    }
    return found ? best : std::string("\x01");
}

bool decode_container(std::vector<uint8_t>& buf, std::vector<ArchiveEntry>& ents, std::string& kind, std::string& err) {
    if (is_lzw_compress(buf.data(), buf.size())) {
        std::vector<uint8_t> dec;
        if (!lzw_decompress(buf.data(), buf.size(), dec, err)) return false;
        buf.swap(dec);
        kind = "compress(.Z) ";
    }
    const ArchiveKind ak = detect_archive(buf.data(), buf.size());
    kind += ak == ArchiveKind::Zip ? "zip" : ak == ArchiveKind::Gzip ? "gzip/tar" : ak == ArchiveKind::Tar ? "tar" : "?";
    return read_archive(buf, ents, err, "matrix/matrix");
}

bool open_job(const std::string& path0, Job& job, std::string& err) {
    namespace fs = std::filesystem;
    std::string path = path0;
    {
        std::string low = to_lower(path);
        for (const char* tail : {"/matrix/matrix", "\\matrix\\matrix"}) {
            std::string t = tail;
            if (low.size() > t.size() && low.compare(low.size() - t.size(), t.size(), t) == 0) path = path.substr(0, path.size() - t.size());
        }
    }
    std::error_code ec;
    const fs::path p(std::u8string(path.begin(), path.end()));
    if (fs::is_directory(p, ec)) {
        if (!job.ft.open(path, err)) return false;
        job.container = "directory";
    } else {
        std::ifstream f(p, std::ios::binary);
        if (!f) { err = "cannot open " + path; return false; }
        if (fs::file_size(p, ec) > (size_t(1) << 30)) { err = "ODB++: archive larger than 1 GiB"; return false; }
        std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<ArchiveEntry> ents;
        std::string kind;
        if (!decode_container(buf, ents, kind, err)) { err = "ODB++: not a readable archive (" + err + ")"; return false; }
        job.ft.open_entries(std::move(ents));
        job.container = kind;
    }
    job.root = find_root(job.ft);
    if (job.root != "\x01") return true;
    // nested archive (e.g. a zip holding job.tgz)
    for (auto& name : job.ft.list()) {
        std::string low = to_lower(name);
        if (!(ends_with_ci(low, ".tgz") || ends_with_ci(low, ".tar") || ends_with_ci(low, ".gz") || ends_with_ci(low, ".zip") ||
              ends_with_ci(low, ".z")))
            continue;
        std::string raw;
        if (!job.ft.get(name, raw)) continue;
        std::vector<uint8_t> buf(raw.begin(), raw.end());
        std::vector<ArchiveEntry> ents;
        std::string kind, e2;
        if (!decode_container(buf, ents, kind, e2)) continue;
        FileTree inner;
        inner.open_entries(std::move(ents));
        std::string r = find_root(inner);
        if (r == "\x01") continue;
        job.ft = std::move(inner);
        job.root = r;
        job.container += " > nested " + name + " (" + kind + ")";
        return true;
    }
    err = "ODB++: matrix/matrix not found (" + std::to_string(job.ft.list().size()) +
          " files in " + job.container + "). This is not an ODB++ job, or the archive holds only part of one.";
    return false;
}

// ---------------------------------------------------------------------------
// matrix / stephdr
// ---------------------------------------------------------------------------
void parse_matrix(const std::string& text, std::vector<std::string>& steps, std::vector<OdbMatrixLayer>& layers) {
    std::string block;
    std::map<std::string, std::string> kv;
    for (auto& l0 : split_lines(text)) {
        std::string l = trim(l0);
        if (l.empty() || l[0] == '#') continue;
        if (l.back() == '{') { block = to_upper(trim(l.substr(0, l.size() - 1))); kv.clear(); continue; }
        if (l == "}") {
            if (block == "STEP" && !kv["NAME"].empty()) steps.push_back(kv["NAME"]);
            if (block == "LAYER") {
                OdbMatrixLayer L;
                L.name = kv["NAME"]; L.type = to_upper(kv["TYPE"]); L.context = to_upper(kv["CONTEXT"]);
                L.polarity = to_upper(kv["POLARITY"]); L.row = (int)num_or(kv["ROW"], (double)layers.size() + 1);
                layers.push_back(L);
            }
            block.clear();
            continue;
        }
        auto eq = l.find('=');
        if (eq != std::string::npos) kv[to_upper(trim(l.substr(0, eq)))] = trim(l.substr(eq + 1));
    }
    // sides: outer signal rows bound top/bottom
    int first = 1 << 30, last = -1;
    for (auto& L : layers) if (L.context == "BOARD" && (L.type == "SIGNAL" || L.type == "POWER_GROUND" || L.type == "MIXED")) { first = std::min(first, L.row); last = std::max(last, L.row); }
    for (auto& L : layers) {
        const std::string n = to_lower(L.name);
        if (L.type == "COMPONENT") { L.side = n.find("bot") != std::string::npos ? "bottom" : "top"; continue; }
        if (last < 0) { L.side = n.find("bot") != std::string::npos ? "bottom" : "top"; continue; }
        const bool sig = L.type == "SIGNAL" || L.type == "POWER_GROUND" || L.type == "MIXED";
        if (L.row < first || L.row == first) L.side = "top";
        else if (L.row > last || L.row == last) L.side = "bottom";
        else if (sig) L.side = "inner";
        else L.side = (n.find("bot") != std::string::npos || n.find("_b") != std::string::npos) ? "bottom" : "top";
        if (!sig && L.side == "top" && (n.find("bot") != std::string::npos)) L.side = "bottom";
    }
}

struct StepHdr { double xd = 0, yd = 0; std::vector<OdbRepeat> reps; };

StepHdr parse_stephdr(const std::string& text) {
    StepHdr h;
    auto lines = split_lines(text);
    const double k = file_units(lines);
    std::map<std::string, std::string> kv;
    bool in = false;
    for (auto& l0 : lines) {
        std::string l = trim(l0);
        if (l.empty() || l[0] == '#') continue;
        if (l.back() == '{') { in = istarts_with(l, "STEP-REPEAT"); kv.clear(); continue; }
        if (l == "}") {
            if (in && !kv["NAME"].empty()) {
                OdbRepeat r;
                r.child = kv["NAME"];
                r.x = num_or(kv["X"]) * k; r.y = num_or(kv["Y"]) * k;
                r.dx = num_or(kv["DX"]) * k; r.dy = num_or(kv["DY"]) * k;
                r.nx = std::max(1, (int)num_or(kv["NX"], 1)); r.ny = std::max(1, (int)num_or(kv["NY"], 1));
                r.angle_cw = num_or(kv["ANGLE"]);
                r.mirror = ieq(kv["MIRROR"], "YES") || ieq(kv["FLIP"], "YES");
                h.reps.push_back(r);
            }
            in = false;
            continue;
        }
        auto eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string key = to_upper(trim(l.substr(0, eq))), val = trim(l.substr(eq + 1));
        if (in) kv[key] = val;
        else if (key == "X_DATUM") h.xd = num_or(val) * k;
        else if (key == "Y_DATUM") h.yd = num_or(val) * k;
    }
    return h;
}

// ---------------------------------------------------------------------------
// eda / components / drills / profile
// ---------------------------------------------------------------------------
struct EdaPin { std::string name; Vec2 pos; double w = 0, h = 0; PadShape shape = PadShape::Round; bool th = false, smd = false; bool sized = false; };
struct EdaPkg {
    std::string name;
    std::vector<EdaPin> pins;
    std::vector<GPrim> outline;     // package body outline records (RC/CR/SQ/CT..CE), package-local mm
    BBox ext;                       // PKG record xmin ymin xmax ymax (CAD-declared extent), may be empty
    int outline_records = 0;
    double height = 0;              // R24a: PKG ".comp_height" attribute, mm (0 = not given)
};

// Arc from s to e around c as a GPrim (sweep + = CCW). s == e => full circle.
GPrim odb_arc_prim(Vec2 s, Vec2 e, Vec2 c, bool cw, double width) {
    GPrim g;
    g.kind = GKind::Arc;
    g.width = width;
    g.pts = {c};
    g.radius = dist(s, c);
    const double a0 = rad2deg(std::atan2(s.y - c.y, s.x - c.x));
    const double a1 = rad2deg(std::atan2(e.y - c.y, e.x - c.x));
    double sw;
    if (dist(s, e) < 1e-9) sw = cw ? -360.0 : 360.0;
    else if (cw) { sw = a1 - a0; while (sw >= 0) sw -= 360.0; while (sw < -360.0) sw += 360.0; }
    else { sw = a1 - a0; while (sw <= 0) sw += 360.0; while (sw > 360.0) sw -= 360.0; }
    g.a0 = a0;
    g.a1 = sw;
    return g;
}

// Outline records shared by package and pin level (ODB++ eda/data):
//   RC llx lly w h | CR xc yc r | SQ xc yc half | CT  OB x y I|H  OS x y  OC xe ye xc yc Y|N  OE ... CE
// Emits true geometry (lines + arcs) into `out` (package body artwork).
struct EdaOutlineReader {
    std::vector<GPrim>* out = nullptr;
    double k = 1, width = 0.1;
    bool in_ct = false, open = false;
    Vec2 cur, start;
    int records = 0;
    bool feed(const std::vector<std::string>& t) {      // true when the record was an outline record
        const std::string& r = t[0];
        auto P = [&](size_t i) { return Vec2{num_or(t[i]) * k, num_or(t[i + 1]) * k}; };
        if (r == "RC" && t.size() >= 5) {
            Vec2 a = P(1);
            GPrim g; g.kind = GKind::Rect; g.width = width;
            g.pts = {a, {a.x + num_or(t[3]) * k, a.y + num_or(t[4]) * k}};
            out->push_back(g); ++records; return true;
        }
        if (r == "CR" && t.size() >= 4) {
            GPrim g; g.kind = GKind::Circle; g.width = width; g.pts = {P(1)}; g.radius = num_or(t[3]) * k;
            out->push_back(g); ++records; return true;
        }
        if (r == "SQ" && t.size() >= 4) {
            Vec2 c = P(1); const double hs = num_or(t[3]) * k;
            GPrim g; g.kind = GKind::Rect; g.width = width; g.pts = {{c.x - hs, c.y - hs}, {c.x + hs, c.y + hs}};
            out->push_back(g); ++records; return true;
        }
        if (r == "CT") { in_ct = true; ++records; return true; }
        if (r == "CE") { in_ct = false; open = false; return true; }
        if (r == "OB" && t.size() >= 3) { cur = start = P(1); open = true; return true; }
        if (r == "OS" && t.size() >= 3 && open) {
            Vec2 p = P(1);
            GPrim g; g.kind = GKind::Line; g.width = width; g.pts = {cur, p};
            if (dist(cur, p) > 1e-9) out->push_back(g);
            cur = p; return true;
        }
        if (r == "OC" && t.size() >= 6 && open) {
            Vec2 e = P(1), c = P(3);
            out->push_back(odb_arc_prim(cur, e, c, ieq(t[5], "Y"), width));
            cur = e; return true;
        }
        if (r == "OE") {
            if (open && dist(cur, start) > 1e-9) { GPrim g; g.kind = GKind::Line; g.width = width; g.pts = {cur, start}; out->push_back(g); }
            open = false; return true;
        }
        return false;
    }
};

void parse_eda(const std::string& text, std::vector<EdaPkg>& pkgs, double& k) {
    auto lines = split_lines(text);
    k = file_units(lines);
    EdaPin* cur = nullptr;
    bool in_ct = false;
    BBox ctb;
    EdaOutlineReader body;              // package-level outline (between PKG and the first PIN)
    body.k = k;
    auto close_ct = [&]() {
        if (cur && !ctb.empty()) { cur->w = ctb.w(); cur->h = ctb.h(); cur->shape = PadShape::Rect; cur->sized = true; }
        in_ct = false;
        ctb = BBox{};
    };
    std::map<int, std::string> anames;   // R24a: "@n name" attribute table
    for (auto& l0 : lines) {
        odb_attr_names(trim(l0), anames);
        std::string l = trim(strip_attrs(l0));
        if (l.empty() || l[0] == '#') continue;
        auto t = split_ws_quoted(l);
        const std::string& r = t[0];
        if (r == "PKG" && t.size() >= 2) {
            if (in_ct) close_ct();
            if (!pkgs.empty()) pkgs.back().outline_records = body.records;
            pkgs.push_back({t[1], {}, {}, {}, 0});
            // PKG <name> <pitch> <xmin> <ymin> <xmax> <ymax>
            if (t.size() >= 7) {
                auto x0 = parse_num(t[3]), y0 = parse_num(t[4]), x1 = parse_num(t[5]), y1 = parse_num(t[6]);
                if (x0 && y0 && x1 && y1 && *x1 > *x0 && *y1 > *y0) { pkgs.back().ext.add({*x0 * k, *y0 * k}); pkgs.back().ext.add({*x1 * k, *y1 * k}); }
            }
            if (auto hv = parse_num(odb_attr_value(l0, anames, ".comp_height")); hv && *hv > 0) pkgs.back().height = *hv * k;
            cur = nullptr;
            body = EdaOutlineReader{};
            body.k = k;
            body.out = &pkgs.back().outline;
            continue;
        }
        if (pkgs.empty()) continue;
        if (r == "PIN" && t.size() >= 5) {
            if (in_ct) close_ct();
            body.in_ct = body.open = false;
            EdaPin p;
            p.name = t[1];
            p.th = ieq(t[2], "T");
            p.smd = ieq(t[2], "S");
            p.pos = {num_or(t[3]) * k, num_or(t[4]) * k};
            pkgs.back().pins.push_back(p);
            cur = &pkgs.back().pins.back();
            continue;
        }
        if (!cur) {
            // package outline: only before the first PIN of this PKG
            if (pkgs.back().pins.empty() && body.out) body.feed(t);
            continue;
        }
        if (r == "CT") { in_ct = true; ctb = BBox{}; continue; }
        if (r == "CE") { close_ct(); continue; }
        if (in_ct) {
            if ((r == "OB" || r == "OS" || r == "OC") && t.size() >= 3) ctb.add({num_or(t[1]) * k, num_or(t[2]) * k});
            continue;
        }
        if (cur->sized) continue;
        if (r == "RC" && t.size() >= 5) {
            cur->w = num_or(t[3]) * k; cur->h = num_or(t[4]) * k;
            cur->shape = std::fabs(cur->w - cur->h) < 1e-9 ? PadShape::Square : PadShape::Rect;
            cur->sized = true;
        } else if (r == "CR" && t.size() >= 4) {
            cur->w = cur->h = 2 * num_or(t[3]) * k; cur->shape = PadShape::Round; cur->sized = true;
        } else if (r == "SQ" && t.size() >= 4) {
            cur->w = cur->h = 2 * num_or(t[3]) * k; cur->shape = PadShape::Square; cur->sized = true;
        }
    }
    if (in_ct) close_ct();
    if (!pkgs.empty()) pkgs.back().outline_records = body.records;
}

struct OdbToe { std::string name; int pin = -1; Vec2 pos; };
struct OdbComp {
    CadComp c;
    double rot_odb = 0;
    int pkg = -1;
    std::vector<OdbToe> toes;
    std::vector<std::pair<std::string, std::string>> props;
};

int count_cmp(const std::string& text) {
    int n = 0;
    for (auto& l0 : split_lines(text)) {
        std::string l = trim(l0);
        if (l.size() > 4 && l.compare(0, 4, "CMP ") == 0) ++n;
    }
    return n;
}

void parse_components(const std::string& text, bool bottom, std::vector<OdbComp>& out) {
    auto lines = split_lines(text);
    const double k = file_units(lines);
    const size_t first = out.size();
    bool have = false;
    std::map<int, std::string> anames;   // R24a: "@n name" attribute table (.comp_height etc.)
    for (auto& l0 : lines) {
        std::string l = trim(l0);
        odb_attr_names(l, anames);
        if (l.empty() || l[0] == '#') continue;      // "# CMP n" comments are optional (see ODB_RE.md)
        if (istarts_with(l, "PRP ") && have) {
            auto t = split_ws_quoted(l);
            if (t.size() >= 2) {
                std::string v;
                auto q = l.find('\'');
                if (q != std::string::npos) {
                    auto q2 = l.find('\'', q + 1);
                    v = l.substr(q + 1, q2 == std::string::npos ? std::string::npos : q2 - q - 1);
                } else if (t.size() >= 3) v = t[2];
                out.back().props.push_back({to_upper(t[1]), trim(v)});
            }
            continue;
        }
        auto t = split_ws_quoted(strip_attrs(l));
        if (t.empty()) continue;
        if (t[0] == "CMP" && t.size() >= 7) {
            OdbComp oc;
            oc.pkg = (int)num_or(t[1], -1);
            oc.c.pos = {num_or(t[2]) * k, num_or(t[3]) * k};
            oc.rot_odb = num_or(t[4]);
            oc.c.rot = -oc.rot_odb;                    // ODB++: clockwise
            oc.c.bottom = bottom || ieq(t[5], "M");
            oc.c.ref = t[6];
            // R14: Allegro/Cadence exports write "???" when the part has no name; treat
            // placeholders as empty so the part key falls back to PRP / VALUE.
            if (t.size() >= 8 && t[7] != "???" && t[7] != "?" && t[7] != "*" && t[7] != "-") oc.c.part = t[7];
            // R24a: ".comp_height" system attribute (file units) -> the same path as a PRP HEIGHT property (mm)
            if (auto hv = parse_num(odb_attr_value(l, anames, ".comp_height")); hv && *hv > 0) {
                char hb[32]; std::snprintf(hb, sizeof hb, "%.6g", *hv * k);
                oc.props.push_back({"COMP_HEIGHT", hb});
            }
            out.push_back(oc);
            have = true;
        } else if (t[0] == "TOP" && have && t.size() >= 4) {
            OdbToe toe;
            toe.pin = (int)num_or(t[1], -1);
            toe.pos = {num_or(t[2]) * k, num_or(t[3]) * k};
            toe.name = t.size() >= 9 ? t[8] : std::to_string(toe.pin + 1);
            out.back().toes.push_back(toe);
        }
    }
    (void)first;
}

struct Hole { Vec2 pos; double d; };

// symbol table "$n name [I|M]" -> name, sizes in file unit system
struct SymTab {
    std::map<int, std::string> name;
    std::map<int, bool> metric;     // per-symbol unit override
};

void parse_drill(const std::string& text, std::vector<Hole>& holes) {
    auto lines = split_lines(text);
    const double k = file_units(lines);
    std::map<int, double> sym;
    for (auto& l0 : lines) {
        std::string l = trim(l0);
        if (l.size() > 1 && l[0] == '$') {
            auto t = split_ws_quoted(l);
            if (t.size() >= 2 && (t[1][0] == 'r' || t[1][0] == 'R')) {
                auto d = parse_num(t[1].substr(1));
                if (d) sym[std::atoi(t[0].c_str() + 1)] = *d * symbol_units(k, t.size() >= 3 ? t[2] : std::string());
            }
            continue;
        }
        if (l.size() > 2 && l[0] == 'P' && l[1] == ' ') {
            auto t = split_ws_quoted(strip_attrs(l));
            if (t.size() >= 4) {
                auto it = sym.find((int)num_or(t[3], -1));
                if (it != sym.end()) holes.push_back({{num_or(t[1]) * k, num_or(t[2]) * k}, it->second});
            }
        }
    }
}

void parse_profile(const std::string& text, OutlineBuilder& ob, BBox* box = nullptr) {
    auto lines = split_lines(text);
    const double k = file_units(lines);
    Vec2 cur, start;
    bool open = false;
    auto add = [&](Vec2 p) { if (box) box->add(p); };
    for (auto& l0 : lines) {
        auto t = split_ws_quoted(strip_attrs(trim(l0)));
        if (t.empty()) continue;
        if (t[0] == "OB" && t.size() >= 3) { cur = start = {num_or(t[1]) * k, num_or(t[2]) * k}; open = true; add(cur); }
        else if (t[0] == "OS" && t.size() >= 3 && open) {
            Vec2 p{num_or(t[1]) * k, num_or(t[2]) * k};
            ob.add_line(cur, p);
            cur = p; add(p);
        } else if (t[0] == "OC" && t.size() >= 6 && open) {
            Vec2 p{num_or(t[1]) * k, num_or(t[2]) * k}, c{num_or(t[3]) * k, num_or(t[4]) * k};
            ob.add_arc_ends(cur, p, c, !ieq(t[5], "Y"));
            const double r = dist(cur, c);
            add(p); box ? (box->add({c.x - r, c.y - r}), box->add({c.x + r, c.y + r})) : void();
            cur = p;
        } else if (t[0] == "OE" && open) {
            if (dist(cur, start) > 1e-9) ob.add_line(cur, start);
            open = false;
        }
    }
}

// rigid placement of a step instance in its parent (no mirror)
struct Rigid { double rot = 0; Vec2 t; bool mirror = false; };   // p' = R(rot) * (mirror? (-x,y)) p + t
Vec2 apply(const Rigid& r, Vec2 p) { if (r.mirror) p.x = -p.x; return rotate(p, r.rot) + r.t; }
Rigid compose(const Rigid& a, const Rigid& b) {   // a after b
    Rigid r;
    r.mirror = a.mirror != b.mirror;
    // a(b(p)) = Ra Ma (Rb Mb p + tb) + ta
    double brot = a.mirror ? -b.rot : b.rot;
    r.rot = a.rot + brot;
    r.t = apply(a, b.t);
    return r;
}

std::string fmt_mm(double v) { char b[32]; std::snprintf(b, sizeof b, "%.2f", v); return b; }

// ---------------------------------------------------------------------------
// scan
// ---------------------------------------------------------------------------
struct Scanner {
    const Job& job;
    std::vector<std::string> step_names;
    std::vector<OdbMatrixLayer> layers;
    std::map<std::string, StepHdr> hdr;

    std::string sdir(const std::string& s) const { return job.root + "steps/" + s + "/"; }
    std::string step_case(const std::string& s) const {
        for (auto& n : step_names) if (ieq(n, s)) return n;
        return s;
    }
    const StepHdr& stephdr(const std::string& s) {
        auto it = hdr.find(to_lower(s));
        if (it != hdr.end()) return it->second;
        std::string t;
        StepHdr h;
        if (job.get(sdir(s) + "stephdr", t)) h = parse_stephdr(t);
        return hdr[to_lower(s)] = h;
    }
    std::vector<std::pair<std::string, bool>> comp_layers() const {
        std::vector<std::pair<std::string, bool>> v;
        for (auto& L : layers) if (L.type == "COMPONENT") v.push_back({L.name, L.side == "bottom"});
        if (v.empty()) v = {{"comp_+_top", false}, {"comp_+_bot", true}};
        return v;
    }
    // flatten step-repeat tree into leaf instances
    void leaves(const std::string& s, const Rigid& T, int depth, std::vector<std::pair<std::string, Rigid>>& out,
                std::vector<std::string>* warns) {
        const StepHdr& h = stephdr(s);
        if (h.reps.empty() || depth > 6) { out.push_back({step_case(s), T}); return; }
        for (auto& r : h.reps) {
            const StepHdr& ch = stephdr(r.child);
            for (int j = 0; j < r.ny; ++j)
                for (int i = 0; i < r.nx; ++i) {
                    Rigid inst;
                    inst.mirror = r.mirror;
                    inst.rot = -r.angle_cw;
                    // child datum -> (X + i*DX, Y + j*DY)
                    Vec2 d{ch.xd, ch.yd};
                    Vec2 at{r.x + i * r.dx, r.y + j * r.dy};
                    Vec2 md = d; if (inst.mirror) md.x = -md.x;
                    inst.t = at - rotate(md, inst.rot);
                    leaves(r.child, compose(T, inst), depth + 1, out, warns);
                }
        }
    }
};

std::string guess_kind(const OdbStepInfo& s) {
    const std::string n = to_lower(s.name);
    auto has = [&](const char* w) { return n.find(w) != std::string::npos; };
    if (has("coupon") || has("cpn") || has("test")) return "coupon";
    if (!s.repeats.empty()) {
        if (has("array") || has("arr")) return "array";
        return "panel";
    }
    if (has("panel") || has("pnl") || has("array")) return "panel";
    if (has("pcb") || has("board") || has("single") || has("unit") || s.comp_top + s.comp_bot > 0) return "pcb";
    return "unknown";
}

bool scan_job(const Job& job, OdbJobInfo& info, Scanner& sc, std::string& err) {
    info.root = job.root;
    info.container = job.container;
    std::string matrix;
    if (!job.get(job.root + "matrix/matrix", matrix)) { err = "ODB++: matrix/matrix unreadable"; return false; }
    parse_matrix(matrix, sc.step_names, sc.layers);
    info.layers = sc.layers;
    if (sc.step_names.empty()) {
        info.warnings.push_back("matrix lists no STEP blocks; steps taken from the steps/ directory");
        std::set<std::string> seen;
        for (auto& f : job.ft.list()) {
            if (f.size() <= job.root.size()) continue;
            std::string rel = f.substr(job.root.size());
            if (!istarts_with(rel, "steps/")) continue;
            auto s = rel.find('/', 6);
            if (s == std::string::npos) continue;
            std::string nm = rel.substr(6, s - 6);
            if (seen.insert(to_lower(nm)).second) sc.step_names.push_back(nm);
        }
    }
    if (sc.step_names.empty()) { err = "ODB++: no step found (matrix has no STEP and steps/ is empty)"; return false; }
    int nlayers_board = 0;
    for (auto& L : sc.layers) if (L.context == "BOARD") ++nlayers_board;
    if (sc.layers.empty()) info.warnings.push_back("matrix lists no LAYER blocks");

    for (auto& name : sc.step_names) {
        OdbStepInfo s;
        s.name = name;
        const std::string sd = sc.sdir(name);
        std::string t;
        if (job.get(sd + "profile", t)) {
            OutlineBuilder ob;
            BBox b;
            parse_profile(t, ob, &b);
            if (!b.empty()) { s.has_profile = true; s.w = b.w(); s.h = b.h(); }
        } else s.notes.push_back("no profile (board outline)");
        s.has_eda = job.exists(sd + "eda/data");
        // unit declarations + extents
        std::set<long long> ks;
        auto note_units = [&](const std::string& what, const std::vector<std::string>& lines) {
            std::string w;
            const double k = declared_units(lines, &w);
            if (k > 0) { ks.insert(std::llround(k * 1e6)); s.units_declared = true; s.unit_evidence.push_back(what + ": " + w); }
            else s.unit_evidence.push_back(what + ": no UNITS (ODB++ default inch)");
        };
        if (job.get(sd + "profile", t)) {
            auto ln = split_lines(t);
            note_units("profile", ln);
            OutlineBuilder ob;
            parse_profile(t, ob, &s.profile_box);
        }
        if (job.get(sd + "eda/data", t)) {
            auto ln = split_lines(t);
            note_units("eda/data", ln);
            std::vector<EdaPkg> pk;
            double kk = 0;
            parse_eda(t, pk, kk);
            s.pkgs = (int)pk.size();
            for (auto& p : pk) { if (!p.outline.empty()) ++s.pkg_outlines; else if (!p.ext.empty()) ++s.pkg_extents; }
        }
        for (auto& cl : sc.comp_layers()) {
            if (job.get(sd + "layers/" + cl.first + "/components", t)) {
                (cl.second ? s.comp_bot : s.comp_top) += count_cmp(t);
                auto ln = split_lines(t);
                note_units("layers/" + cl.first + "/components", ln);
                const double k = file_units(ln);
                for (auto& l0 : ln) {
                    std::string l = trim(l0);
                    if (l.size() <= 4 || l.compare(0, 4, "CMP ") != 0) continue;
                    auto tk = split_ws_quoted(strip_attrs(l));
                    if (tk.size() < 4) continue;
                    Vec2 p{num_or(tk[2]) * k, num_or(tk[3]) * k};
                    s.comp_box.add(p);
                    if (!s.profile_box.empty() && (p.x < s.profile_box.x0 - 1 || p.x > s.profile_box.x1 + 1 ||
                                                   p.y < s.profile_box.y0 - 1 || p.y > s.profile_box.y1 + 1)) ++s.comps_outside_profile;
                }
            }
        }
        s.units_conflict = ks.size() > 1;
        s.units_k = ks.size() == 1 ? (double)*ks.begin() / 1e6 : (ks.empty() ? kMmPerInch : 0);
        if (s.units_conflict) s.notes.push_back("files declare different units (each file is converted with its own declaration)");
        for (auto& L : sc.layers) if (job.exists(sd + "layers/" + L.name + "/features")) ++s.layers_with_features;
        s.repeats = sc.stephdr(name).reps;
        for (auto& r : s.repeats) s.instances += r.nx * r.ny;
        if (!s.has_eda && s.comp_top + s.comp_bot > 0) s.notes.push_back("no eda/data: packages derived from toeprints");
        info.steps.push_back(s);
    }
    // leaf component totals (recursive)
    std::map<std::string, int> own;
    for (auto& s : info.steps) own[to_lower(s.name)] = s.comp_top + s.comp_bot;
    for (auto& s : info.steps) {
        std::vector<std::pair<std::string, Rigid>> lv;
        sc.leaves(s.name, Rigid{}, 0, lv, nullptr);
        int n = 0;
        for (auto& l : lv) n += own[to_lower(l.first)];
        s.leaf_comps = s.repeats.empty() ? s.comp_top + s.comp_bot : n + s.comp_top + s.comp_bot;
        s.kind = guess_kind(s);
        if (s.repeats.empty() == false && s.comp_top + s.comp_bot > 0 && s.comp_top + s.comp_bot < 12)
            s.notes.push_back("own components on panel step (fiducials / tooling?)");
    }
    // recommendation
    int best = -1;
    auto score = [](const OdbStepInfo& s) { return s.comp_top + s.comp_bot; };
    for (size_t i = 0; i < info.steps.size(); ++i) {
        auto& s = info.steps[i];
        if (!s.repeats.empty() || s.kind == "coupon") continue;
        if (score(s) > 0 && (best < 0 || score(s) > score(info.steps[best]))) best = (int)i;
    }
    if (best >= 0) {
        auto& s = info.steps[best];
        info.recommended = best;
        info.recommend_reason = "step '" + s.name + "' is a single board with " + std::to_string(s.comp_top) + " top + " +
                                std::to_string(s.comp_bot) + " bottom components";
        for (auto& p : info.steps)
            if (!p.repeats.empty() && p.leaf_comps > 0)
                info.recommend_reason += "; panel step '" + p.name + "' repeats it " + std::to_string(p.instances) +
                                         "x (choose it with 'step-repeat as images' to import the panel layout)";
    } else {
        for (size_t i = 0; i < info.steps.size(); ++i)
            if (info.steps[i].leaf_comps > 0 && (best < 0 || info.steps[i].leaf_comps > info.steps[best].leaf_comps)) best = (int)i;
        if (best >= 0) {
            info.recommended = best;
            info.recommend_images = !info.steps[best].repeats.empty();
            info.recommend_reason = "components are only reachable through panel step '" + info.steps[best].name + "'";
        } else {
            for (size_t i = 0; i < info.steps.size(); ++i) {
                auto& s = info.steps[i];
                if (s.kind == "pcb" || (best < 0 && s.has_profile) || (best >= 0 && s.layers_with_features > info.steps[best].layers_with_features && s.repeats.empty()))
                    best = (int)i;
                if (s.kind == "pcb") break;
            }
            if (best < 0) best = 0;
            info.recommended = best;
            info.recommend_reason = "no step has component layers (comp_+_top / comp_+_bot): the job carries geometry only; "
                                    "placements must come from a centroid / XY file or be digitized";
        }
    }
    if (std::string t; job.get(job.root + "misc/info", t)) {
        std::string w;
        if (declared_units(split_lines(t), &w) > 0) info.job_units = w;
    }
    info.compressed_members = 0;
    for (auto& f : job.ft.list()) if (ends_with_ci(f, ".z") || ends_with_ci(f, ".gz")) ++info.compressed_members;
    std::string t;
    if (info.recommended >= 0) {
        const std::string sd = sc.sdir(info.steps[info.recommended].name);
        std::vector<std::string> lines;
        if (job.get(sd + "eda/data", t)) lines = split_lines(t);
        bool f = false;
        if (!lines.empty()) { double k = file_units(lines, &f); info.units = f ? (k == 1.0 ? "mm" : "inch") : "inch (default)"; }
    }
    return true;
}

// ---------------------------------------------------------------------------
// features -> Gerber
// ---------------------------------------------------------------------------
struct GbrWriter {
    std::string out;
    std::map<std::string, int> apt;
    std::string apt_defs;
    int next_d = 10;
    int cur_d = -1;
    bool clear = false;
    std::map<std::string, int> unsupported;
    int items = 0;
    static std::string c(double mm) { char b[32]; std::snprintf(b, sizeof b, "%lld", (long long)std::llround(mm * 1e6)); return b; }
    static std::string f(double mm) { char b[32]; std::snprintf(b, sizeof b, "%.6g", mm); return b; }
    int aperture(const std::string& def) {
        auto it = apt.find(def);
        if (it != apt.end()) return it->second;
        int d = next_d++;
        apt[def] = d;
        apt_defs += "%ADD" + std::to_string(d) + def + "*%\n";
        return d;
    }
    void select(int d) { if (d != cur_d) { out += "D" + std::to_string(d) + "*\n"; cur_d = d; } }
    void polarity(bool neg) { if (neg != clear) { out += neg ? "%LPC*%\n" : "%LPD*%\n"; clear = neg; } }
    void flash(Vec2 p) { out += "X" + c(p.x) + "Y" + c(p.y) + "D03*\n"; ++items; }
    void move(Vec2 p) { out += "X" + c(p.x) + "Y" + c(p.y) + "D02*\n"; }
    void line(Vec2 p) { out += "X" + c(p.x) + "Y" + c(p.y) + "D01*\n"; ++items; }
    void arc(Vec2 s, Vec2 e, Vec2 ctr, bool cw) {
        out += cw ? "G02*\n" : "G03*\n";
        out += "X" + c(e.x) + "Y" + c(e.y) + "I" + c(ctr.x - s.x) + "J" + c(ctr.y - s.y) + "D01*\nG01*\n";
        ++items;
    }
    void polygon(const std::vector<Vec2>& pts) {
        if (pts.size() < 3) return;
        out += "G36*\n";
        move(pts[0]);
        for (size_t i = 1; i < pts.size(); ++i) line(pts[i]);
        line(pts[0]);
        out += "G37*\n";
    }
    std::string finish() const {
        return "%FSLAX46Y46*%\n%MOMM*%\nG75*\n%LPD*%\n" + apt_defs + "G01*\n" + out + "M02*\n";
    }
};

// Standard symbol decoded in mm. kind: "C" round (w = diameter), "R" rect, "O" oval (aperture-able at k*90),
// "P" outline polygon (rounded / chamfered rect, diamond, octagon, triangle, ellipse, hexagon, ...),
// "D" donut (w = outer, h = inner diameter; drawn as a ring). R12: no more square-corner approximation.
struct SymDim { std::string kind; double w = 0, h = 0; bool ok = false; std::vector<Vec2> poly; };

static void add_arc(std::vector<Vec2>& v, Vec2 c, double r, double a0, double a1, int n) {
    for (int i = 0; i <= n; ++i) { const double a = deg2rad(a0 + (a1 - a0) * i / n); v.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)}); }
}
// rect with rounded (round) or chamfered corners; corners = set of "1234" (1 upper right, 2 upper left,
// 3 lower left, 4 lower right; ODB++ spec), empty = all
static std::vector<Vec2> rect_corners(double w, double h, double r, bool round, const std::string& corners) {
    const double hw = w / 2, hh = h / 2;
    r = std::clamp(r, 0.0, std::min(hw, hh));
    auto on = [&](char k) { return corners.empty() || corners.find(k) != std::string::npos; };
    std::vector<Vec2> v;
    struct C { char k; Vec2 p; double a0; Vec2 c; };
    const C cs[4] = {{'1', {hw, hh}, 0, {hw - r, hh - r}}, {'2', {-hw, hh}, 90, {-hw + r, hh - r}},
                     {'3', {-hw, -hh}, 180, {-hw + r, -hh + r}}, {'4', {hw, -hh}, 270, {hw - r, -hh + r}}};
    for (auto& k : cs) {
        if (!on(k.k) || r <= 0) { v.push_back(k.p); continue; }
        if (round) add_arc(v, k.c, r, k.a0, k.a0 + 90, 8);
        else {
            // chamfer: cut the corner point along both edges by r (corners are visited CCW: 1 UR, 2 UL, 3 LL, 4 LR)
            const Vec2 e_in = k.k == '1' ? Vec2{0, -1} : k.k == '2' ? Vec2{1, 0} : k.k == '3' ? Vec2{0, 1} : Vec2{-1, 0};
            const Vec2 e_out = k.k == '1' ? Vec2{-1, 0} : k.k == '2' ? Vec2{0, -1} : k.k == '3' ? Vec2{1, 0} : Vec2{0, 1};
            v.push_back(k.p + e_in * r);
            v.push_back(k.p + e_out * r);
        }
    }
    return v;
}

SymDim decode_symbol(const std::string& nm0, double unit_mm) {   // unit_mm: mm per symbol unit
    SymDim s;
    std::string nm = to_lower(nm0);
    auto nums = [&](const std::string& rest, std::vector<double>& v) {
        v.clear();
        std::string cur;
        for (char ch : rest + "x") {
            if (ch == 'x' || ch == 'X') { if (auto d = parse_num(cur)) v.push_back(*d * unit_mm); else return false; cur.clear(); }
            else cur += ch;
        }
        return !v.empty();
    };
    std::vector<double> v;
    auto pre = [&](const char* p) { return nm.rfind(p, 0) == 0; };
    auto poly = [&](std::vector<Vec2> pts) { s.kind = "P"; s.poly = std::move(pts); BBox b; for (auto& q : s.poly) b.add(q); s.w = b.w(); s.h = b.h(); s.ok = s.poly.size() >= 3; };
    if (pre("rect")) {
        // "rect20x40", "rect16.2x40.6xr5.3[x<corners>]" (rounded), "rect..xc..[x<corners>]" (chamfered)
        std::string r = nm.substr(4);
        const size_t p = r.find_first_of("rc");
        std::string dims = r.substr(0, p);
        while (!dims.empty() && dims.back() == 'x') dims.pop_back();
        if (!nums(dims, v) || v.size() < 2) return s;
        const double w = v[0], h = v[1];
        if (p == std::string::npos) { s.kind = "R"; s.w = w; s.h = h; s.ok = true; return s; }
        const bool round = r[p] == 'r';
        std::string rest = r.substr(p + 1), rad = rest, corners;
        if (auto xp = rest.find('x'); xp != std::string::npos) { rad = rest.substr(0, xp); corners = rest.substr(xp + 1); }
        auto rv = parse_num(rad);
        if (!rv) { s.kind = "R"; s.w = w; s.h = h; s.ok = true; return s; }
        poly(rect_corners(w, h, *rv * unit_mm, round, corners));
        return s;
    }
    if (pre("oval_h") || pre("hex_") || pre("donut_s") || pre("donut_sr") || pre("donut_rc") || pre("donut_o") || pre("thr") || pre("ths") || pre("s_th") || pre("sr_th") || pre("rc_th") || pre("o_th") || pre("moire") || pre("hole") || pre("null") || pre("bfr") || pre("bfs")) {
        if (pre("hex_l") && nums(nm.substr(5), v) && v.size() >= 3) {   // horizontal hexagon w x h x corner
            const double hw = v[0] / 2, hh = v[1] / 2, c = std::min(v[2], hw);
            poly({{-hw, 0}, {-hw + c, hh}, {hw - c, hh}, {hw, 0}, {hw - c, -hh}, {-hw + c, -hh}});
        } else if (pre("hex_s") && nums(nm.substr(5), v) && v.size() >= 3) {
            const double hw = v[0] / 2, hh = v[1] / 2, c = std::min(v[2], hh);
            poly({{0, -hh}, {hw, -hh + c}, {hw, hh - c}, {0, hh}, {-hw, hh - c}, {-hw, -hh + c}});
        } else if ((pre("thr") || pre("ths")) && nums(nm.substr(3), v) && v.size() >= 2) {   // thermal: ring approximation
            s.kind = "D"; s.w = v[0]; s.h = v[1]; s.ok = true;
        } else if (pre("donut_s") && !pre("donut_sr") && nums(nm.substr(7), v) && v.size() >= 2) {   // square ring: outer square
            s.kind = "R"; s.w = s.h = v[0]; s.ok = true;
        } else if (pre("null")) { s.kind = "N"; s.ok = true; }
        return s;
    }
    if (pre("oval") && nums(nm.substr(4), v) && v.size() >= 2) { s.kind = "O"; s.w = v[0]; s.h = v[1]; s.ok = true; }
    else if (pre("donut_r") && nums(nm.substr(7), v) && v.size() >= 2) { s.kind = "D"; s.w = v[0]; s.h = v[1]; s.ok = true; }
    else if (pre("donut_r") && nums(nm.substr(7), v)) { s.kind = "C"; s.w = s.h = v[0]; s.ok = true; }
    else if (pre("di") && nums(nm.substr(2), v) && v.size() >= 2) { const double a = v[0] / 2, b = v[1] / 2; poly({{a, 0}, {0, b}, {-a, 0}, {0, -b}}); }
    else if (pre("oct") && nums(nm.substr(3), v) && v.size() >= 3) {
        const double a = v[0] / 2, b = v[1] / 2, c = std::min({v[2], a, b});
        poly({{a, b - c}, {a - c, b}, {-a + c, b}, {-a, b - c}, {-a, -b + c}, {-a + c, -b}, {a - c, -b}, {a, -b + c}});
    } else if (pre("tri") && nums(nm.substr(3), v) && v.size() >= 2) { const double a = v[0] / 2, b = v[1] / 2; poly({{-a, -b}, {a, -b}, {0, b}}); }
    else if (pre("el") && nums(nm.substr(2), v) && v.size() >= 2) {
        std::vector<Vec2> e;
        for (int i = 0; i < 48; ++i) { const double t = 2 * kPi * i / 48; e.push_back({v[0] / 2 * std::cos(t), v[1] / 2 * std::sin(t)}); }
        poly(e);
    }
    else if (pre("r") && nums(nm.substr(1), v)) { s.kind = "C"; s.w = s.h = v[0]; s.ok = true; }
    else if (pre("s") && nums(nm.substr(1), v)) { s.kind = "R"; s.w = s.h = v[0]; s.ok = true; }
    return s;
}

// outline of R / O shapes (for arbitrary rotation / mirror)
static std::vector<Vec2> shape_outline(const SymDim& s) {
    if (s.kind == "P") return s.poly;
    const double hw = s.w / 2, hh = s.h / 2;
    if (s.kind == "O") {
        std::vector<Vec2> v;
        const double r = std::min(hw, hh);
        if (hw >= hh) { add_arc(v, {hw - r, 0}, r, -90, 90, 12); add_arc(v, {-hw + r, 0}, r, 90, 270, 12); }
        else { add_arc(v, {0, hh - r}, r, 0, 180, 12); add_arc(v, {0, -hh + r}, r, 180, 360, 12); }
        return v;
    }
    return {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
}

} // namespace

namespace {
struct FeatCtx {
    GbrWriter& g;
    const OdbSymbolLookup* lookup;
    std::map<std::string, std::string> cache;   // user symbol features
    std::map<std::string, int> missing_user;
    int user_syms = 0, user_flashes = 0;
};

// emit one features text with transform X (feature mm -> board mm), mirror flag (X has det < 0), polarity invert
static void emit_features(FeatCtx& C, const std::string& features, const Xform& X, bool inv, int depth) {
    GbrWriter& g = C.g;
    auto lines = split_lines(features);
    bool found = false;
    const double k = file_units(lines, &found);
    const double xrot = rad2deg(std::atan2(X.b, X.a));             // rotation part of X (CCW)
    const bool xmir = X.a * X.d - X.b * X.c < 0;
    std::map<int, SymDim> sym;
    std::map<int, std::string> symname;
    bool in_surf = false, surf_neg = false;
    std::vector<Vec2> contour;
    bool contour_hole = false;
    std::vector<std::pair<std::vector<Vec2>, bool>> contours;
    Vec2 cpos;
    auto P = [&](const std::string& a, const std::string& b) { return X.apply(Vec2{num_or(a) * k, num_or(b) * k}); };
    auto PL = [&](const std::string& a, const std::string& b) { return Vec2{num_or(a) * k, num_or(b) * k}; };
    auto arc_pts = [&](Vec2 s, Vec2 e, Vec2 c, bool cw, std::vector<Vec2>& v) {   // local coords
        double a0 = std::atan2(s.y - c.y, s.x - c.x), a1 = std::atan2(e.y - c.y, e.x - c.x), r = dist(s, c);
        double sw = a1 - a0;
        if (cw) { while (sw >= -1e-12) sw -= 2 * kPi; } else { while (sw <= 1e-12) sw += 2 * kPi; }
        int n = std::max(2, (int)std::ceil(std::fabs(sw) / (kPi / 18)));
        for (int i = 1; i <= n; ++i) { double a = a0 + sw * i / n; v.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)}); }
    };
    auto user_symbol = [&](const std::string& name, std::string& text) -> bool {
        if (!C.lookup || depth >= 4) return false;
        auto it = C.cache.find(name);
        if (it != C.cache.end()) { text = it->second; return !text.empty(); }
        std::string t;
        const bool ok = (*C.lookup)(name, t);
        C.cache[name] = ok ? t : std::string();
        if (ok) ++C.user_syms;
        text = t;
        return ok && !t.empty();
    };
    for (auto& l0 : lines) {
        std::string l = trim(l0);
        if (l.empty() || l[0] == '#') continue;
        if (l[0] == '$') {
            auto t = split_ws_quoted(l);
            if (t.size() >= 2) {
                int id = std::atoi(t[0].c_str() + 1);
                sym[id] = decode_symbol(t[1], symbol_units(k, t.size() >= 3 ? t[2] : std::string()));
                symname[id] = t[1];
            }
            continue;
        }
        auto t = split_ws_quoted(strip_attrs(l));
        if (t.empty()) continue;
        const std::string& r = t[0];
        auto pen = [&](int id) -> int {   // line / arc pen: round or square
            auto it = sym.find(id);
            if (it == sym.end() || !it->second.ok || (it->second.kind != "C" && it->second.kind != "R")) {
                ++g.unsupported[symname.count(id) ? symname[id] : "#" + std::to_string(id)]; return -1;
            }
            const SymDim& s = it->second;
            if (s.kind == "C") return g.aperture("C," + GbrWriter::f(s.w));
            return g.aperture("R," + GbrWriter::f(s.w) + "X" + GbrWriter::f(s.h));
        };
        if (r == "P" && t.size() >= 5) {
            const Vec2 pl = PL(t[1], t[2]);
            const int id = (int)num_or(t[3], -1);
            const bool neg = ieq(t[4], "N") != inv;
            double rot = 0; bool mir = false;
            if (t.size() >= 7) {
                const int o = (int)num_or(t[6]);
                if (o >= 8 && t.size() >= 8) { rot = num_or(t[7]); mir = o == 9; }
                else { rot = (o & 3) * 90.0; mir = o >= 4; }
            }
            // local placement transform: symbol -> feature coords (ODB++: mirror X first, then rotate clockwise)
            const Xform L = Xform::translate(pl.x, pl.y) * Xform::rotation(-rot) * (mir ? Xform::scale(-1, 1) : Xform());
            const Xform M = X * L;
            auto it = sym.find(id);
            const std::string nm = symname.count(id) ? symname[id] : "#" + std::to_string(id);
            if (it == sym.end() || !it->second.ok) {
                std::string text;
                if (user_symbol(nm, text)) { ++C.user_flashes; emit_features(C, text, M, neg, depth + 1); continue; }
                if (C.lookup) ++C.missing_user[nm];   // looked up: the job has no symbols/<name>
                else ++g.unsupported[nm];
                continue;
            }
            const SymDim& s = it->second;
            if (s.kind == "N") continue;
            const Vec2 p = M.apply({0, 0});
            const double mrot = std::fmod(rad2deg(std::atan2(M.b, M.a)) + 720.0, 360.0);
            g.polarity(neg);
            if (s.kind == "C") { g.select(g.aperture("C," + GbrWriter::f(s.w))); g.flash(p); continue; }
            if (s.kind == "D") {   // ring: circular stroke of width (od - id) / 2 at the mean radius
                const double rr = (s.w + s.h) / 4, wd = std::max(1e-4, (s.w - s.h) / 2);
                g.select(g.aperture("C," + GbrWriter::f(wd)));
                g.move(p + Vec2{rr, 0});
                g.arc(p + Vec2{rr, 0}, p + Vec2{rr, 0}, p, false);
                continue;
            }
            const int q = (int)std::lround(mrot / 90.0);
            if ((s.kind == "R" || s.kind == "O") && std::fabs(mrot - q * 90.0) < 1e-6) {
                double w = s.w, h = s.h;
                if (q & 1) std::swap(w, h);
                g.select(g.aperture(s.kind + "," + GbrWriter::f(w) + "X" + GbrWriter::f(h)));
                g.flash(p);
                continue;
            }
            std::vector<Vec2> v;
            for (Vec2 c : shape_outline(s)) v.push_back(M.apply(c));
            g.polygon(v);
            ++g.items;
        } else if (r == "L" && t.size() >= 7) {
            const int d = pen((int)num_or(t[5], -1));
            if (d < 0) continue;
            g.polarity(ieq(t[6], "N") != inv);
            g.select(d);
            g.move(P(t[1], t[2]));
            g.line(P(t[3], t[4]));
        } else if (r == "A" && t.size() >= 10) {
            const int d = pen((int)num_or(t[7], -1));
            if (d < 0) continue;
            g.polarity(ieq(t[8], "N") != inv);
            g.select(d);
            const Vec2 s = P(t[1], t[2]);
            g.move(s);
            const bool cw = t.size() >= 11 ? ieq(t[10], "Y") : false;
            g.arc(s, P(t[3], t[4]), P(t[5], t[6]), cw != xmir);
        } else if (r == "S" && t.size() >= 2) {
            in_surf = true; surf_neg = ieq(t[1], "N") != inv; contours.clear();
        } else if (in_surf && r == "OB" && t.size() >= 3) {
            contour.clear(); cpos = PL(t[1], t[2]); contour.push_back(cpos);
            contour_hole = t.size() >= 4 && ieq(t[3], "H");
        } else if (in_surf && r == "OS" && t.size() >= 3) {
            cpos = PL(t[1], t[2]); contour.push_back(cpos);
        } else if (in_surf && r == "OC" && t.size() >= 6) {
            Vec2 e = PL(t[1], t[2]);
            arc_pts(cpos, e, PL(t[3], t[4]), ieq(t[5], "Y"), contour);
            cpos = e;
        } else if (in_surf && r == "OE") {
            std::vector<Vec2> w;
            for (auto& q : contour) w.push_back(X.apply(q));
            contours.push_back({w, contour_hole});
        } else if (in_surf && r == "SE") {
            for (auto& c : contours) if (!c.second) { g.polarity(surf_neg); g.polygon(c.first); ++g.items; }
            for (auto& c : contours) if (c.second) { g.polarity(!surf_neg); g.polygon(c.first); }
            in_surf = false;
        }
    }
    (void)xrot;
}
} // namespace

bool odb_features_to_gerber(const std::string& features, std::string& gerber, std::string& notes) {
    return odb_features_to_gerber(features, gerber, notes, nullptr);
}

bool odb_features_to_gerber(const std::string& features, std::string& gerber, std::string& notes, const OdbSymbolLookup* lookup) {
    GbrWriter g;
    FeatCtx C{g, lookup, {}, {}};
    emit_features(C, features, Xform(), false, 0);
    notes.clear();
    for (auto& u : g.unsupported) notes += (notes.empty() ? "unsupported symbols skipped: " : ", ") + u.first + " x" + std::to_string(u.second);
    if (!C.missing_user.empty()) {
        notes += (notes.empty() ? "" : "; ") + std::string("user symbols missing from the job (no symbols/<name>/features) skipped: ");
        bool first = true;
        for (auto& u : C.missing_user) { notes += (first ? "" : ", ") + u.first + " x" + std::to_string(u.second); first = false; }
    }
    if (C.user_flashes)
        notes += (notes.empty() ? "" : "; ") + std::to_string(C.user_flashes) + " user-defined symbol instance(s) drawn from " + std::to_string(C.user_syms) + " symbol(s)";
    if (!g.items) return false;
    gerber = g.finish();
    return true;
}

namespace {
} // namespace

bool odb_features_to_excellon(const std::string& features, std::string& drill, int& holes_out) {
    std::vector<Hole> holes;
    parse_drill(features, holes);
    holes_out = (int)holes.size();
    if (holes.empty()) return false;
    std::map<long long, int> tools;
    for (auto& h : holes) tools.emplace(std::llround(h.d * 1000), 0);
    int n = 1;
    std::string s = "M48\nMETRIC,LZ,000.000\n";
    for (auto& t : tools) { t.second = n++; char b[64]; std::snprintf(b, sizeof b, "T%02dC%.3f\n", t.second, t.first / 1000.0); s += b; }
    s += "%\nG90\nG05\n";
    for (auto& t : tools) {
        char b[64];
        std::snprintf(b, sizeof b, "T%02d\n", t.second);
        s += b;
        for (auto& h : holes)
            if (std::llround(h.d * 1000) == t.first) { std::snprintf(b, sizeof b, "X%.3fY%.3f\n", h.pos.x, h.pos.y); s += b; }
    }
    s += "M30\n";
    drill = s;
    return true;
}

std::vector<std::string> odb_unit_warnings(double w, double h, int outside, int total) {
    std::vector<std::string> v;
    char b[200];
    const double big = std::max(w, h), small = std::min(w, h);
    if (big > 1000) { std::snprintf(b, sizeof b, "board %.1f x %.1f mm is larger than 1 m: coordinates are probably in a smaller unit (mm read as inch?)", w, h); v.push_back(b); }
    else if (big > 0 && small < 5) { std::snprintf(b, sizeof b, "board %.2f x %.2f mm is smaller than 5 mm: coordinates are probably in a larger unit (inch read as mm?)", w, h); v.push_back(b); }
    if (total > 0 && outside > 0) {
        std::snprintf(b, sizeof b, "%d of %d placements lie outside the board profile", outside, total);
        v.push_back(b);
    }
    return v;
}

int sniff_odbpp(const std::string& path, const std::string& head) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(std::u8string(path.begin(), path.end()));
    if (fs::is_directory(p, ec)) {
        if (fs::exists(p / "matrix" / "matrix", ec) || fs::exists(p / "matrix" / "matrix.Z", ec)) return 95;
        return 0;
    }
    const std::string low = to_lower(path);
    if (ends_with_ci(low, "matrix/matrix") || ends_with_ci(low, "matrix\\matrix")) return 90;
    const bool z = is_lzw_compress((const uint8_t*)head.data(), head.size());
    const ArchiveKind ak = detect_archive((const uint8_t*)head.data(), head.size());
    if (ak == ArchiveKind::None && !z) return 0;
    if ((ak == ArchiveKind::Zip || ak == ArchiveKind::Tar) && head.find("matrix/matrix") != std::string::npos) return 90;
    if (fs::file_size(p, ec) < (size_t(256) << 20)) {
        Job job;
        std::string err;
        if (open_job(path, job, err)) return 92;
    }
    return 0;
}

bool odb_scan(const std::string& path, OdbJobInfo& info, std::string& err) {
    info = OdbJobInfo{};
    Job job;
    if (!open_job(path, job, err)) return false;
    Scanner sc{job};
    if (!scan_job(job, info, sc, err)) return false;
    for (auto& e : job.decode_errors) info.warnings.push_back("could not decompress " + e);
    return true;
}

std::string odb_describe(const OdbJobInfo& info) {
    std::string s = "ODB++ job (" + info.container + (info.root.empty() ? "" : ", root '" + info.root + "'") + "): " +
                    std::to_string(info.steps.size()) + " step(s), " + std::to_string(info.layers.size()) + " layer(s)";
    if (info.compressed_members) s += ", " + std::to_string(info.compressed_members) + " compressed member(s)";
    s += "\n";
    for (size_t i = 0; i < info.steps.size(); ++i) {
        auto& st = info.steps[i];
        s += ((int)i == info.recommended ? "  * " : "    ") + st.name + "  [" + st.kind + "]";
        if (st.has_profile) s += "  " + fmt_mm(st.w) + " x " + fmt_mm(st.h) + " mm";
        s += "  comps " + std::to_string(st.comp_top) + " top / " + std::to_string(st.comp_bot) + " bot";
        if (!st.repeats.empty()) {
            s += "  step-repeat:";
            for (auto& r : st.repeats) s += " " + r.child + " " + std::to_string(r.nx) + "x" + std::to_string(r.ny);
            s += " (" + std::to_string(st.leaf_comps) + " comps total)";
        }
        s += "  " + std::to_string(st.layers_with_features) + " layers";
        if (st.pkgs) s += "  packages " + std::to_string(st.pkgs) + " (" + std::to_string(st.pkg_outlines) + " with body outline, " +
                          std::to_string(st.pkg_extents) + " extent only)";
        for (auto& n : st.notes) s += "; " + n;
        s += "\n";
        for (auto& e : st.unit_evidence) s += "        units " + e + "\n";
    }
    if (info.recommended >= 0) s += "Recommended: " + info.steps[info.recommended].name + (info.recommend_images ? " (step-repeat as images)" : "") + " \xE2\x80\x94 " + info.recommend_reason + "\n";
    if (!info.job_units.empty()) s += "misc/info: " + info.job_units + "\n";
    for (auto& w : info.warnings) s += "Warning: " + w + "\n";
    return s;
}

bool import_odbpp(const std::string& path, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                  std::string& err) {
    res = CadImportResult{};
    res.format = "ODB++";
    Job job;
    if (!open_job(path, job, err)) return false;
    OdbJobInfo info;
    Scanner sc{job};
    if (!scan_job(job, info, sc, err)) return false;
    job.compressed_reads = 0;
    res.source_name = job.root;
    // units: forced source unit and user scale apply to every file read below
    const double force_k = units_override_mm(opt.units);
    const double uscale = opt.scale > 0 ? opt.scale : 1.0;
    UnitScope unit_scope(force_k, uscale);
    CadImportOptions bopt = opt;
    bopt.scale = 1.0;                    // already applied through file_units

    // ---- step selection: "<step>[|images][|flat][|layers][|nolayers]" ----
    std::string want = opt.step;
    bool f_images = false, f_flat = false, f_layers = false, f_nolayers = false;
    {
        auto parts = split_char(want, '|');
        want = parts.empty() ? "" : trim(parts[0]);
        for (size_t i = 1; i < parts.size(); ++i) {
            std::string f = to_lower(trim(parts[i]));
            if (f == "images") f_images = true; else if (f == "flat") f_flat = true;
            else if (f == "layers") f_layers = true; else if (f == "nolayers") f_nolayers = true;
        }
    }
    int si = -1;
    if (!want.empty()) {
        for (size_t i = 0; i < info.steps.size(); ++i) if (ieq(info.steps[i].name, want)) si = (int)i;
        if (si < 0) {
            err = "ODB++: step '" + want + "' not found. Steps in this job:";
            for (auto& s : info.steps) err += " " + s.name;
            return false;
        }
    } else {
        si = info.recommended;
        if (info.recommend_images) f_images = true;
    }
    const OdbStepInfo& S = info.steps[si];
    CadBuilder b(bopt, res);
    {
        std::string ev = S.units_conflict ? "mixed (per file)" : !S.units_declared ? "inch (no UNITS declared; ODB++ default)"
                                         : (std::fabs(S.units_k - 1.0) < 1e-9 ? "mm" : std::fabs(S.units_k - kMmPerInch) < 1e-9 ? "inch" : "custom");
        res.units_evidence = "declared " + ev + ":";
        for (auto& e : S.unit_evidence) res.units_evidence += " " + e + ";";
        if (!info.job_units.empty()) res.units_evidence += " misc/info: " + info.job_units + ";";
        if (force_k > 0) {
            char bb[96]; std::snprintf(bb, sizeof bb, "%.6g mm per unit", force_k);
            b.warn("unit conversion override: every coordinate read as '" + opt.units + "' (" + bb + "), declared units ignored");
        }
    }
    b.warn("step '" + S.name + "' [" + S.kind + "] of " + std::to_string(info.steps.size()) + ": " +
           (want.empty() ? "chosen automatically \xE2\x80\x94 " + info.recommend_reason : std::string("chosen by user")));
    for (auto& w : info.warnings) b.warn(w);
    if (S.repeats.empty()) f_images = f_flat = false;
    else if (!f_images && !f_flat) f_images = S.comp_top + S.comp_bot == 0;

    // ---- instances: which leaf steps to read and where ----
    std::vector<std::pair<std::string, Rigid>> inst;
    if (f_images || f_flat) sc.leaves(S.name, Rigid{}, 0, inst, nullptr);
    else inst.push_back({S.name, Rigid{}});
    if (f_images) {
        bool same = true, mirrored = false;
        for (auto& i : inst) { if (!ieq(i.first, inst[0].first)) same = false; if (i.second.mirror) mirrored = true; }
        if (!same || mirrored) {
            b.warn(std::string("panel images need one board step without mirroring (") + (mirrored ? "mirrored instances" : "different child steps") +
                   "): instances expanded as separate components instead");
            f_images = false; f_flat = true;
        }
    }

    // ---- per-leaf data cache ----
    struct Leaf {
        std::vector<OdbComp> comps;
        std::vector<EdaPkg> eda;
        double k_eda = kMmPerInch;
        bool eda_ok = false;
        std::vector<Hole> holes;
    };
    std::map<std::string, Leaf> leafs;
    std::set<std::string> unmapped_props;
    auto load_leaf = [&](const std::string& step) -> Leaf& {
        auto it = leafs.find(to_lower(step));
        if (it != leafs.end()) return it->second;
        Leaf& L = leafs[to_lower(step)];
        const std::string sd = sc.sdir(step);
        std::string text;
        if (job.get(sd + "eda/data", text)) { parse_eda(text, L.eda, L.k_eda); L.eda_ok = true; }
        bool any_layer = false;
        for (auto& cl : sc.comp_layers()) {
            if (job.get(sd + "layers/" + cl.first + "/components", text)) { any_layer = true; parse_components(text, cl.second, L.comps); }
        }
        if (!any_layer) b.warn("step '" + step + "': no component layer files (layers/comp_+_top|comp_+_bot/components)");
        for (auto& Lr : sc.layers)
            if (Lr.type == "DRILL" && job.get(sd + "layers/" + Lr.name + "/features", text)) parse_drill(text, L.holes);
        return L;
    };

    // ---- packages (from the first leaf with eda; per leaf step) ----
    struct PkgOut { std::map<int, std::string> by_index; };
    std::map<std::string, PkgOut> pkgmap;
    std::map<std::string, std::string> derived_done;
    int guessed = 0;
    int verified = 0, flipped_sign_top = 0, flipped_sign_bot = 0;

    auto build_packages = [&](const std::string& step, Leaf& L) {
        PkgOut& po = pkgmap[to_lower(step)];
        // toeprint check of the rotation convention (needs eda pins + TOP records)
        double err_neg[2] = {0, 0}, err_pos[2] = {0, 0};
        int n_side[2] = {0, 0};
        for (auto& oc : L.comps) {
            if (oc.pkg < 0 || oc.pkg >= (int)L.eda.size() || oc.toes.empty()) continue;
            double r = std::fmod(std::fabs(oc.rot_odb), 180.0);
            if (std::fabs(r) < 1 || std::fabs(r - 180) < 1) continue;     // sign does not matter
            auto& pins = L.eda[oc.pkg].pins;
            const int side = oc.c.bottom ? 1 : 0;
            int used = 0;
            double en = 0, ep = 0;
            for (auto& t : oc.toes) {
                if (t.pin < 0 || t.pin >= (int)pins.size()) continue;
                en += dist(local_to_abs(pins[t.pin].pos, oc.c.pos, -oc.rot_odb, oc.c.bottom), t.pos);
                ep += dist(local_to_abs(pins[t.pin].pos, oc.c.pos, oc.rot_odb, oc.c.bottom), t.pos);
                ++used;
            }
            if (!used) continue;
            err_neg[side] += en / used; err_pos[side] += ep / used; ++n_side[side];
        }
        for (int s = 0; s < 2; ++s) {
            if (!n_side[s]) continue;
            verified += n_side[s];
            if (err_pos[s] < err_neg[s] * 0.5) {
                (s ? flipped_sign_bot : flipped_sign_top) += n_side[s];
                for (auto& oc : L.comps) if ((oc.c.bottom ? 1 : 0) == s) oc.c.rot = oc.rot_odb;
            }
        }
        // hole lookup
        std::unordered_map<long long, std::vector<size_t>> grid;
        auto cell = [](Vec2 p) { return (long long)std::floor(p.x / 0.5) * 1000003LL + (long long)std::floor(p.y / 0.5); };
        for (size_t i = 0; i < L.holes.size(); ++i) grid[cell(L.holes[i].pos)].push_back(i);
        auto hole_at = [&](Vec2 p) -> double {
            double best = 0, bd = 0.05;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy) {
                    auto it = grid.find(cell({p.x + dx * 0.5, p.y + dy * 0.5}));
                    if (it == grid.end()) continue;
                    for (size_t i : it->second) { double d = dist(L.holes[i].pos, p); if (d < bd) { bd = d; best = L.holes[i].d; } }
                }
            return best;
        };
        std::vector<CadPackage> cpk(L.eda.size());
        std::vector<bool> used(L.eda.size(), false);
        // R18c: per pin, how many placements have a hole at that pin and the largest such hole. A via next to /
        // in one placement's pad must not turn the shared package into through-hole: the pin counts as drilled
        // only when a hole is found at it in at least half of the package's placements.
        std::vector<int> inst(L.eda.size(), 0);
        std::vector<std::vector<int>> hit(L.eda.size());
        std::vector<std::vector<double>> hd(L.eda.size());
        for (size_t i = 0; i < L.eda.size(); ++i) {
            cpk[i].name = L.eda[i].name;
            cpk[i].height = L.eda[i].height;   // R24a: PKG .comp_height (0 = not in the data)
            // package body: true CAD outline when present, else the CAD-declared
            // PKG extent box, else empty (CadBuilder derives it from the pads and
            // marks it approximate). Never invented geometry labelled as CAD.
            if (!L.eda[i].outline.empty()) { cpk[i].body = L.eda[i].outline; cpk[i].body_source = "cad"; }
            else if (!L.eda[i].ext.empty()) {
                GPrim r; r.kind = GKind::Rect; r.width = 0.1;
                r.pts = {{L.eda[i].ext.x0, L.eda[i].ext.y0}, {L.eda[i].ext.x1, L.eda[i].ext.y1}};
                cpk[i].body = {r}; cpk[i].body_source = "cad_extent";
            }
            for (auto& ep : L.eda[i].pins) {
                CadPin p;
                p.name = ep.name; p.pos = ep.pos; p.w = ep.w; p.h = ep.h; p.shape = ep.shape; p.th_hint = ep.th; p.smd_hint = ep.smd;
                if (!ep.sized) p.w = p.h = 0.5;
                cpk[i].pins.push_back(p);
            }
        }
        for (auto& oc : L.comps) {
            if (oc.pkg >= 0 && oc.pkg < (int)cpk.size()) {
                used[oc.pkg] = true;
                auto& pins = cpk[oc.pkg].pins;
                hit[oc.pkg].resize(pins.size(), 0); hd[oc.pkg].resize(pins.size(), 0.0);
                ++inst[oc.pkg];
                for (size_t q = 0; q < pins.size(); ++q) {
                    double d = hole_at(local_to_abs(pins[q].pos, oc.c.pos, oc.c.rot, oc.c.bottom));
                    if (d > 0) { ++hit[oc.pkg][q]; hd[oc.pkg][q] = std::max(hd[oc.pkg][q], d); }
                }
            } else {
                std::string name = "ODBPKG" + std::to_string(oc.pkg);
                if (!derived_done.count(name)) {
                    CadPackage P;
                    P.name = name;
                    for (auto& t : oc.toes) {
                        CadPin p;
                        p.name = t.name;
                        p.pos = abs_to_local(t.pos, oc.c.pos, oc.c.rot, oc.c.bottom);
                        p.w = p.h = 0.5;
                        p.drill = hole_at(t.pos);
                        P.pins.push_back(p);
                    }
                    derived_done[name] = b.add_package(P);
                }
                po.by_index[oc.pkg] = derived_done[name];
            }
        }
        for (size_t i = 0; i < cpk.size(); ++i) {
            if (!used[i]) continue;
            // a through-hole part has a hole at (nearly) every lead: holes at fewer than half of the pins are vias
            std::vector<size_t> drilled;
            for (size_t q = 0; q < cpk[i].pins.size(); ++q)
                if (q < hit[i].size() && hit[i][q] * 2 >= inst[i] && !cpk[i].pins[q].smd_hint && cpk[i].pins[q].drill <= 0) drilled.push_back(q);
            if (drilled.size() * 2 >= cpk[i].pins.size())
                for (size_t q : drilled) cpk[i].pins[q].drill = hd[i][q];
            // the EDA pin type ("T") is only used when the job has no drill data at all
            if (L.holes.empty())
                for (auto& pin : cpk[i].pins)
                    if (pin.th_hint && pin.drill <= 0) { pin.drill = 0.6 * std::min(pin.w, pin.h); ++guessed; }
            po.by_index[(int)i] = b.add_package(cpk[i]);
        }
    };

    // ---- properties -> part fields ----
    struct PartExtra { std::string manf_pn, vend_pn, cust_pn, desc; double height = 0; };
    std::map<std::string, PartExtra> part_extra;
    auto apply_props = [&](OdbComp& oc) {
        PartExtra pe;
        for (auto& kv : oc.props) {
            const std::string& k = kv.first;
            const std::string& v = kv.second;
            if (v.empty()) continue;
            if (k == "VALUE" || k == "VAL" || k == "COMP_VALUE") oc.c.value = v;
            else if (k == "PART_NUMBER" || k == "PARTNUMBER" || k == "PART_NO" || k == "PN" || k == "MPN" || k == "MFR_PN" ||
                     k == "MFG_PN" || k == "MANUFACTURER_PART_NUMBER" || k == "MANUFACTURER_PN" || k == "MFR_PART_NUMBER") {
                if (oc.c.part.empty()) oc.c.part = v;
                if (k != "PART_NUMBER" && k != "PARTNUMBER" && k != "PART_NO" && k != "PN") pe.manf_pn = v;
            } else if (k == "DESCRIPTION" || k == "DESC") pe.desc = v;
            else if (k == "PART_NAME" || k == "DEVICE" || k == "DEVICE_TYPE") { if (pe.desc.empty()) pe.desc = v; }   // R14: schematic device name
            else if (k == "VENDOR_PN" || k == "SUPPLIER_PN" || k == "VENDOR_PART_NUMBER") pe.vend_pn = v;
            else if (k == "CUSTOMER_PN" || k == "CUST_PN" || k == "INTERNAL_PN") pe.cust_pn = v;
            else if (k == "HEIGHT" || k == "COMP_HEIGHT") pe.height = num_or(v);
            else unmapped_props.insert(k);
        }
        std::string key = !oc.c.part.empty() ? oc.c.part : oc.c.value;
        if (!key.empty()) {
            auto& d = part_extra[key];
            if (d.manf_pn.empty()) d.manf_pn = pe.manf_pn;
            if (d.vend_pn.empty()) d.vend_pn = pe.vend_pn;
            if (d.cust_pn.empty()) d.cust_pn = pe.cust_pn;
            if (d.desc.empty()) d.desc = pe.desc;
            if (d.height <= 0) d.height = pe.height;
        }
    };

    // ---- outline ----
    std::string text;
    Rigid T0 = inst.empty() ? Rigid{} : inst[0].second;
    if (opt.import_board) {
        const std::string leaf0 = inst.empty() ? S.name : inst[0].first;
        if (job.get(sc.sdir(f_images ? leaf0 : S.name) + "profile", text)) parse_profile(text, b.outline);
        else b.warn("step '" + (f_images ? leaf0 : S.name) + "': no profile, board outline not imported");
    }

    // ---- components ----
    int n_comps = 0;
    std::vector<Rigid> image_xf;
    for (size_t ii = 0; ii < inst.size(); ++ii) {
        const std::string& step = inst[ii].first;
        Leaf& L = load_leaf(step);
        if (!pkgmap.count(to_lower(step))) build_packages(step, L);
        if (f_images && ii > 0) { image_xf.push_back(inst[ii].second); continue; }
        if (f_images) image_xf.push_back(inst[ii].second);
        const Rigid& T = (f_images || f_flat) ? inst[ii].second : Rigid{};
        for (auto& oc0 : L.comps) {
            OdbComp oc = oc0;
            apply_props(oc);
            auto& po = pkgmap[to_lower(step)];
            if (auto it = po.by_index.find(oc.pkg); it != po.by_index.end()) oc.c.package = it->second;
            if (T.mirror) { b.warn("mirrored step-repeat instance of '" + step + "' skipped"); break; }
            oc.c.pos = apply(T, oc.c.pos);
            oc.c.rot += T.rot;
            if (f_flat && inst.size() > 1 && ii > 0) oc.c.ref += "_" + std::to_string(ii + 1);
            b.add_component(oc.c);
            ++n_comps;
        }
    }
    // components on the panel step itself (fiducials / tooling) in panel coordinates
    if ((f_images || f_flat) && S.comp_top + S.comp_bot > 0) {
        Leaf& P = load_leaf(S.name);
        if (!pkgmap.count(to_lower(S.name))) build_packages(S.name, P);
        for (auto& oc0 : P.comps) {
            OdbComp oc = oc0;
            apply_props(oc);
            auto& po = pkgmap[to_lower(S.name)];
            if (auto it = po.by_index.find(oc.pkg); it != po.by_index.end()) oc.c.package = it->second;
            oc.c.fiducial = oc.c.fiducial || istarts_with(oc.c.ref, "FID") || icontains(oc.c.package, "FID");
            b.add_component(oc.c);
            ++n_comps;
        }
    }
    if (guessed) b.warn(std::to_string(guessed) + " through-hole pins without drill data: drill estimated");
    if (verified) {
        if (flipped_sign_top || flipped_sign_bot)
            b.warn("rotation convention: toeprints fit counter-clockwise angles for " + std::to_string(flipped_sign_top) + " top / " +
                   std::to_string(flipped_sign_bot) + " bottom components (non-standard exporter); used as CCW");
        else b.warn("rotation convention verified: ODB++ clockwise angles match toeprints of " + std::to_string(verified) + " rotated components");
    }
    if (!unmapped_props.empty()) {
        std::string s;
        for (auto& p : unmapped_props) s += (s.empty() ? "" : ", ") + p;
        b.warn("component properties without a matching part field (not stored): " + s);
    }
    {
        const std::string sd = sc.sdir(f_images ? inst[0].first : S.name);
        if (n_comps == 0) {
            std::string why = "No placements imported from step '" + S.name + "'.";
            if (S.comp_top + S.comp_bot == 0 && S.repeats.empty()) why += " The step has no component layers (comp_+_top / comp_+_bot).";
            for (auto& o : info.steps)
                if (!ieq(o.name, S.name) && o.leaf_comps > 0)
                    why += " Step '" + o.name + "' has " + std::to_string(o.leaf_comps) + " components \xE2\x80\x94 re-import and choose it.";
            why += " Geometry (outline / copper / silk) is imported; placements need a centroid / XY file or digitizing.";
            b.warn(why);
            f_layers = true;
        }
        (void)sd;
    }
    if (f_nolayers) f_layers = false;
    for (auto& e : job.decode_errors) b.warn("could not decompress " + e);
    if (job.compressed_reads) b.warn(std::to_string(job.compressed_reads) + " compressed member(s) (.Z / .gz) decoded");

    // ---- geometry layers ----
    std::vector<GerberLayerRef> new_layers;
    if (f_layers) {
        const std::string step = f_images ? inst[0].first : S.name;
        const std::string sd = sc.sdir(step);
        // R12: user-defined symbols (<job>/symbols/<name>/features), by name or by the attrlist ".source_name" alias
        std::map<std::string, std::pair<int, std::string>> sym_alias;   // lower-case alias -> (entity_version, dir name)
        {
            const std::string pre = job.root + "symbols/";
            std::set<std::string> dirs;
            for (auto& f0 : job.ft.list()) {
                std::string f = f0; std::replace(f.begin(), f.end(), '\\', '/');
                if (f.compare(0, pre.size(), pre) != 0) continue;
                const size_t sl = f.find('/', pre.size());
                if (sl != std::string::npos) dirs.insert(f.substr(pre.size(), sl - pre.size()));
            }
            for (auto& d : dirs) {
                std::string at;
                if (!job.get(pre + d + "/attrlist", at)) continue;
                std::string src; int ver = 0;
                for (auto& ln : split_lines(at)) {
                    const std::string q = trim(ln);
                    auto val = [&](const char* key) -> std::string {
                        const size_t n = std::strlen(key);
                        if (q.compare(0, n, key) != 0) return {};
                        const size_t e = q.find('=', n);
                        return e == std::string::npos ? std::string() : trim(q.substr(e + 1));
                    };
                    if (auto v = val(".source_name"); !v.empty()) src = v;
                    if (auto v = val(".entity_version"); !v.empty()) ver = std::atoi(v.c_str());
                }
                if (src.empty()) continue;
                auto& slot = sym_alias[to_lower(src)];
                if (slot.second.empty() || ver > slot.first) slot = {ver, d};
            }
        }
        const OdbSymbolLookup sym_lookup = [&](const std::string& name, std::string& out) {
            const std::string pre = job.root + "symbols/";
            if (job.get(pre + name + "/features", out)) return true;
            if (job.get(pre + to_lower(name) + "/features", out)) return true;
            auto it = sym_alias.find(to_lower(name));
            return it != sym_alias.end() && job.get(pre + it->second.second + "/features", out);
        };
        for (auto& L : sc.layers) {
            if (L.context != "BOARD" && L.type != "DRILL") continue;
            if (L.side == "inner") continue;
            LayerRole role;
            if (L.type == "SIGNAL" || L.type == "MIXED" || L.type == "POWER_GROUND") role = LayerRole::Copper;
            else if (L.type == "SILK_SCREEN") role = LayerRole::Silk;
            else if (L.type == "SOLDER_MASK") role = LayerRole::Mask;
            else if (L.type == "SOLDER_PASTE") role = LayerRole::Paste;
            else if (L.type == "DRILL") role = LayerRole::Drill;
            else continue;
            if (!job.get(sd + "layers/" + L.name + "/features", text)) continue;
            GerberLayerRef g;
            g.name = L.name;
            g.path = "odb:" + path + "#" + step + "/" + L.name;
            g.role = role;
            g.side = L.side == "bottom" ? Side::Bottom : Side::Top;
            g.visible = role != LayerRole::Paste && role != LayerRole::Mask;
            g.color = role == LayerRole::Copper ? (g.side == Side::Top ? 0xFFC08040u : 0xFF4080C0u)
                    : role == LayerRole::Silk ? 0xFFE8E8E8u : role == LayerRole::Drill ? 0xFF303030u : 0xFF40A040u;
            std::string notes;
            bool ok;
            if (role == LayerRole::Drill) { int n = 0; ok = odb_features_to_excellon(text, g.content, n); }
            else ok = odb_features_to_gerber(text, g.content, notes, &sym_lookup);
            if (!notes.empty()) b.warn("layer " + L.name + ": " + notes);
            if (ok) new_layers.push_back(std::move(g));
        }
        if (new_layers.empty()) b.warn("no drawable copper / silk / drill features found in step '" + step + "'");
        else b.warn(std::to_string(new_layers.size()) + " ODB++ layer(s) added as Gerber/Excellon geometry");
    }

    res.unit_mm = 1.0;
    if (!info.units.empty()) res.units = info.units;
    if (force_k > 0) res.units = opt.units + " (override)";
    else if (S.units_conflict) res.units = "mixed (per file)";
    else if (S.units_declared) res.units = std::fabs(S.units_k - 1.0) < 1e-9 ? "mm" : "inch";
    else res.units = "inch (ODB++ default, not declared)";
    if (uscale != 1.0) { char bb[48]; std::snprintf(bb, sizeof bb, " \xC3\x97%.6g user scale", uscale); res.units += bb; }
    // suspicious ranges after conversion
    if (S.has_profile) {
        const double f = (force_k > 0 && S.units_k > 0 ? force_k / S.units_k : 1.0) * uscale;
        for (auto& w : odb_unit_warnings(S.w * f, S.h * f, S.comps_outside_profile, S.comp_top + S.comp_bot)) b.warn("check units: " + w);
    }
    // partial data (placements present, but packages / attributes missing)
    {
        std::vector<std::string> why;
        int no_pkg = 0, no_part = 0, from_toes = 0, tot = 0;
        for (auto& kv : leafs) {
            for (auto& oc : kv.second.comps) {
                ++tot;
                if (oc.pkg < 0 || oc.pkg >= (int)kv.second.eda.size()) { if (oc.toes.empty()) ++no_pkg; else ++from_toes; }
                if (oc.c.part.empty() && oc.props.empty()) ++no_part;
            }
        }
        if (from_toes) why.push_back(std::to_string(from_toes) + " component(s) without eda/data package: pad positions from toeprints, pad sizes unknown");
        if (no_pkg) why.push_back(std::to_string(no_pkg) + " component(s) without any package or toeprint data");
        if (tot > 0 && no_part == tot) why.push_back("no part numbers / properties on any component");
        for (size_t i = 0; i < why.size(); ++i) res.partial_reason += (i ? "; " : "") + why[i];
    }
    if (n_comps == 0 && new_layers.empty() && b.outline.empty()) {
        err = "ODB++: nothing importable in step '" + S.name + "'.\n" + odb_describe(info);
        return false;
    }
    if (!b.commit(prj, err)) return false;

    // part fields from PRP records
    for (auto& kv : part_extra) {
        auto it = prj.parts.find(kv.first);
        if (it == prj.parts.end()) continue;
        auto& pn = it->second;
        if (pn.manf_pn.empty()) pn.manf_pn = kv.second.manf_pn;
        if (pn.vend_pn.empty()) pn.vend_pn = kv.second.vend_pn;
        if (pn.cust_pn.empty()) pn.cust_pn = kv.second.cust_pn;
        if (!kv.second.desc.empty() && (pn.description.empty() || pn.description == pn.number)) pn.description = kv.second.desc;
        if (pn.height <= 0) pn.height = kv.second.height;
    }
    // panel images
    if (f_images && image_xf.size() > 1 && (opt.replace || prj.images.size() <= 1)) {
        // root image = first instance; image i = T_i o T0^-1
        prj.images.clear();
        for (size_t i = 0; i < image_xf.size(); ++i) {
            PanelImage im;
            im.name = std::to_string(i + 1);
            im.root = i == 0;
            const double rot = image_xf[i].rot - T0.rot;
            im.rot = rot;
            im.offset = image_xf[i].t - rotate(T0.t, rot);
            prj.images.push_back(im);
        }
        // panel frame from the panel profile (panel coordinates == root image coordinates)
        if (opt.import_board && job.get(sc.sdir(S.name) + "profile", text)) {
            OutlineBuilder ob;
            parse_profile(text, ob);
            auto fr = ob.finish();
            if (!fr.empty()) { prj.panel_frame = fr; prj.panel_outline_src = OutlineSource::Imported; }
        }
        res.warnings.push_back("panel: " + std::to_string(image_xf.size()) + " images from step-repeat of '" + S.name + "'");
    }
    res.layers = (int)new_layers.size();
    res.classify(res.partial_reason);
    if (!res.units_evidence.empty()) res.warnings.insert(res.warnings.begin(), "units: " + res.units_evidence);
    if (!new_layers.empty()) {
        if (opt.replace) prj.layers.erase(std::remove_if(prj.layers.begin(), prj.layers.end(), [](const GerberLayerRef& l) { return l.path.rfind("odb:", 0) == 0; }), prj.layers.end());
        for (auto& l : new_layers) prj.layers.push_back(std::move(l));
    }
    return true;
}

} // namespace pcbp::io
