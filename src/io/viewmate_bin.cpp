// ============================================================================
// viewmate_bin.cpp — ViewMate job database (*.bin) reader / Gerber renderer /
// synthetic writer. Layout: specs/VIEWMATE_BIN.md. Clean-room code.
// ============================================================================
#include "viewmate_bin.h"
#include "../core/fileio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace pcbp::io::vm {

namespace {

constexpr int32_t kArc = 0x7FFFFFFF;
const char kSig[] = "Lavenir Technology V2001 JOB V";

struct Rd {
    const uint8_t* p; size_t n; size_t o = 0; bool bad = false;
    bool need(size_t k) { if (bad || o + k > n || o + k < o) { bad = true; return false; } return true; }
    uint8_t u8() { if (!need(1)) return 0; return p[o++]; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = uint16_t(p[o] | p[o + 1] << 8); o += 2; return v; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = uint32_t(p[o]) | uint32_t(p[o + 1]) << 8 | uint32_t(p[o + 2]) << 16 | uint32_t(p[o + 3]) << 24; o += 4; return v; }
    int32_t i32() { return (int32_t)u32(); }
    double f64() { if (!need(8)) return 0; double d; std::memcpy(&d, p + o, 8); o += 8; return d; }
    void skip(size_t k) { if (need(k)) o += k; }
    bool eof() const { return o >= n; }
};

std::string zstr(const uint8_t* p, size_t n) {
    size_t k = 0;
    while (k < n && p[k]) ++k;
    return std::string((const char*)p, k);
}

// layer attribute string: "name=..\x01color=..\x01cfg=state=..\x02type=..\x02bnum=.."
void parse_layer_attrs(Layer& L) {
    auto split = [](const std::string& s, char sep) {
        std::vector<std::string> v; std::string cur;
        for (char c : s) { if (c == sep) { v.push_back(cur); cur.clear(); } else cur += c; }
        v.push_back(cur);
        return v;
    };
    auto kv = [](const std::string& item, std::string& k, std::string& val) {
        size_t e = item.find('=');
        if (e == std::string::npos) { k = item; val.clear(); return; }
        k = item.substr(0, e); val = item.substr(e + 1);
    };
    auto num = [](const std::string& s) { return (int)std::strtol(s.c_str(), nullptr, 10); };
    for (auto& item : split(L.raw, '\x01')) {
        std::string k, v; kv(item, k, v);
        if (k == "name") L.name = v;
        else if (k == "color") L.color = num(v);
        else if (k == "cfg") {
            for (auto& sub : split(v, '\x02')) {
                std::string k2, v2; kv(sub, k2, v2);
                if (k2 == "state") L.state = num(v2);
                else if (k2 == "type") L.type = num(v2);
                else if (k2 == "bnum") L.bnum = num(v2);
            }
        }
    }
}

const int kTypeMap[8] = {0, 1, 2, 6, 3, 7, 4, 5};   // stored element type -> class
// classes: 0 pads, 1 traces, 2 arcs, 3 polygons, 4 routing paths, 6 step-repeat group, 7 text group

bool read_contour(Rd& r, Contour& c) {
    uint32_t nw = r.u32();
    c.hdr1 = r.u32();
    c.flags = r.u32();
    if (r.bad || nw > (r.n - r.o) / 4) { r.bad = true; return false; }
    c.words.resize(nw);
    for (uint32_t i = 0; i < nw; ++i) c.words[i] = r.i32();
    return !r.bad;
}

struct Ctx {
    Rd& r;
    Job& job;
    bool attr;          // tag 101 stream: one u32 after each element
    bool wide;          // version > 9: 24-byte group headers
    int depth = 0;
    std::vector<int> group_stack;
};

// Returns false at end of stream / pop marker. `pop` set when a step-repeat end marker was read.
bool read_group(Ctx& c, bool& pop);

bool read_block(Ctx& c, int max_groups) {
    // read groups until stream end / pop; max_groups < 0 = unlimited
    for (int k = 0; max_groups < 0 || k < max_groups; ++k) {
        bool pop = false;
        if (!read_group(c, pop)) return false;
        if (pop) return true;
    }
    return true;
}

bool read_group(Ctx& c, bool& pop) {
    Rd& r = c.r;
    if (r.eof()) return false;
    int32_t count, dcode, type, pol, layer;
    if (c.wide) {
        if (r.n - r.o < 24) { r.o = r.n; return false; }
        count = r.i32(); dcode = r.i32(); type = r.i32(); pol = r.i32(); layer = r.i32(); r.i32();
    } else {
        if (r.n - r.o < 12) { r.o = r.n; return false; }
        count = r.i32(); dcode = r.u16(); type = r.u8(); layer = r.u8(); pol = r.u8(); r.skip(3);
    }
    if (type < 0 || type >= 8) { c.job.warnings.push_back("bad element type " + std::to_string(type) + " at offset " + std::to_string(r.o)); r.bad = true; return false; }
    const int cls = kTypeMap[type];
    const int grp = c.group_stack.empty() ? 0 : c.group_stack.back();
    if (cls == 6) {                                    // step-repeat container
        if (count == 0) { pop = true; return true; }
        Group g; g.kind = Group::StepRepeat; g.parent = grp;
        for (int i = 0; i < 5; ++i) g.raw[i] = r.i32();
        c.job.groups.push_back(g);
        if (c.depth > 32) { r.bad = true; return false; }
        c.group_stack.push_back((int)c.job.groups.size());
        ++c.depth;
        bool more = read_block(c, -1);
        --c.depth;
        c.group_stack.pop_back();
        return more || !r.bad;
    }
    if (cls == 7) {                                    // text container
        Group g; g.kind = Group::Text; g.parent = grp;
        for (int i = 0; i < 10; ++i) g.raw[i] = r.i32();
        for (std::string* s : {&g.s1, &g.s2}) {
            uint32_t len = r.u32();
            if (r.bad || len > 1u << 20 || !r.need(len)) { r.bad = true; return false; }
            *s = zstr(r.p + r.o, len); r.o += len;
        }
        c.job.groups.push_back(g);
        c.group_stack.push_back((int)c.job.groups.size());
        int children = g.raw[9];
        bool ok = true;
        for (int i = 0; i < children && ok; ++i) { bool p2 = false; ok = read_group(c, p2); }
        c.group_stack.pop_back();
        return ok || !r.bad;
    }
    if (count == 0) return true;   // empty group (writer emits these as separators)
    if (count < 0 || (size_t)count > r.n) { r.bad = true; return false; }
    for (int32_t i = 0; i < count && !r.bad; ++i) {
        Element e;
        e.layer = layer; e.dcode = dcode; e.clear = pol != 0; e.group = grp;
        switch (cls) {
        case 0: e.kind = Element::Pad; e.v[0] = r.i32(); e.v[1] = r.i32(); break;
        case 1: e.kind = Element::Trace; for (int k = 0; k < 4; ++k) e.v[k] = r.i32(); break;
        case 2: e.kind = Element::Arc; for (int k = 0; k < 6; ++k) e.v[k] = r.i32(); break;
        case 3: {
            e.kind = Element::Polygon;
            uint32_t nc = r.u32();
            if (nc > 1u << 20) { r.bad = true; break; }
            for (uint32_t k = 0; k < nc && !r.bad; ++k) { Contour ct; if (read_contour(r, ct)) e.contours.push_back(std::move(ct)); }
            break;
        }
        case 4: {
            e.kind = Element::Route;
            Contour ct;
            if (read_contour(r, ct)) e.contours.push_back(std::move(ct));
            e.tool = r.i32(); r.skip(4 + 4 + 1 + 72);
            break;
        }
        default: r.bad = true; break;
        }
        if (c.attr) e.attr = r.u32();
        if (!r.bad) c.job.elements.push_back(std::move(e));
    }
    return !r.bad;
}

} // namespace

const char* shape_letter(int s) {
    static const char* L[18] = {"C", "S", "R", "O", "X", "H", "T", "?", "D", "B", "?", "F", "?", "Y", "M", "N", "E", "Q"};
    return (s >= 0 && s < 18) ? L[s] : "?";
}

const Layer* Job::layer(int index) const {
    for (auto& l : layers) if (l.index == index) return &l;
    return nullptr;
}

int Job::count(int layer, Element::Kind k) const {
    int n = 0;
    for (auto& e : elements) if (e.layer == layer && e.kind == k) ++n;
    return n;
}

uint32_t default_palette(int i) {
    static const uint32_t pal[16] = {0xFF000000, 0xFF00008F, 0xFF007F00, 0xFF007F7F, 0xFF7F0000, 0xFF7F007F,
                                     0xFF7F7F00, 0xFF4F4F4F, 0xFFC0C0C0, 0xFF0000FF, 0xFF00FF00, 0xFF00FFFF,
                                     0xFFFF0000, 0xFFFF00FF, 0xFFFFFF00, 0xFFFF8080};
    return pal[std::clamp(i, 0, 15)];
}

bool is_job(const uint8_t* p, size_t n, int* version) {
    const size_t L = sizeof(kSig) - 1;
    if (n < L + 3 || std::memcmp(p, kSig, L) != 0) return false;
    int v = 0; size_t i = L;
    while (i < n && i < L + 6 && p[i] >= '0' && p[i] <= '9') v = v * 10 + (p[i++] - '0');
    if (i == L || i + 1 >= n || p[i] != '\r' || p[i + 1] != '\n') return false;
    if (version) *version = v;
    return true;
}

bool read_job(const uint8_t* p, size_t n, Job& job, std::string& err) {
    job = Job{};
    int ver = 0;
    if (!is_job(p, n, &ver)) { err = "not a ViewMate job file (signature missing)"; return false; }
    job.version = ver;
    for (int i = 0; i < 16; ++i) job.palette[i] = default_palette(i);
    Rd r{p, n};
    while (r.o < n && p[r.o] != '\n') ++r.o;
    r.o += 2;                        // "\n" + NUL
    if (r.o > n) { err = "truncated header"; return false; }
    const bool lengths = ver >= 2;
    bool elements = false, attr = false;
    std::map<int, int> skipped;
    while (!r.eof() && !r.bad) {
        const int tag = r.u8();
        if (tag == 1) {             // D-code table: u16 dcode, 16-byte record [, extra]; 0 terminates
            for (;;) {
                const int dc = r.u16();
                if (r.bad || dc == 0) break;
                Aperture a; a.dcode = dc;
                a.p0 = r.i32(); a.w = r.i32(); a.h = r.i32(); a.shape = r.u8(); a.shape2 = r.u8(); r.skip(2);
                if (a.shape == ShThermal || a.shape == ShCustom) {
                    uint32_t len = (uint32_t)a.p0;
                    if (len > 1u << 24 || !r.need(len)) { r.bad = true; break; }
                    a.extra.assign(p + r.o, p + r.o + len); r.o += len;
                    if (a.shape == ShThermal && len >= 16) {
                        Rd t{a.extra.data(), a.extra.size()};
                        a.th_inner = t.i32(); a.th_spokes = (int)t.u32(); a.th_angle = t.f64();
                    }
                }
                if (dc < 4 || dc > 100000) { job.warnings.push_back("D" + std::to_string(dc) + " out of range, ignored"); continue; }
                job.apertures[dc] = std::move(a);
            }
            continue;
        }
        if (tag == 100 || tag == 101) {
            if (tag == 101) { attr = true; r.u32(); }
            elements = true;
            break;
        }
        if (tag == 7) { err = "job file reports an error record (tag 7)"; return false; }
        uint32_t len = 0;
        if (lengths) len = r.u32();
        else { err = "job format version " + std::to_string(ver) + " (no record lengths) is not supported"; return false; }
        if (r.bad || len > n - r.o) { err = "record " + std::to_string(tag) + " overruns the file"; return false; }
        Rd s{p + r.o, len};
        switch (tag) {
        case 29: {                  // layer: u32 number, u32 len, attribute string
            Layer L; L.index = (int)s.u32();
            uint32_t sl = s.u32();
            if (!s.bad && sl <= len - 8) L.raw = zstr(p + r.o + 8, sl);
            parse_layer_attrs(L);
            if (L.index >= 1 && L.index <= 255) job.layers.push_back(std::move(L));
            break;
        }
        case 22: job.flags = s.u32(); break;
        case 34: job.resolution = (int)s.u32(); break;
        case 3:
            job.units = s.u8();
            for (int i = 0; i < 4; ++i) job.bbox[i] = s.i32();
            break;
        case 27:
            for (uint32_t i = 0; i + 4 <= len; i += 4) job.tool_sizes.push_back(s.i32());
            break;
        case 28:
            if (len >= 64) {          // 16 (+1 background) COLORREFs: R,G,B,0
                for (int i = 0; i < 16; ++i) {
                    uint8_t c[4]; for (auto& b : c) b = s.u8();
                    job.palette[i] = 0xFF000000u | uint32_t(c[0]) << 16 | uint32_t(c[1]) << 8 | c[2];
                }
                job.has_palette = true;
            }
            break;
        default: skipped[tag]++; break;
        }
        r.o += len;
    }
    job.unknown_records = skipped;
    if (r.bad) { err = "truncated job file (header records)"; return false; }
    if (job.resolution != 100 && job.resolution != 1000) {
        job.warnings.push_back("unusual resolution " + std::to_string(job.resolution) + ", assuming 100");
        job.resolution = 100;
    }
    double u = job.metric() ? (job.resolution == 1000 ? 1e-6 : 1e-5) : (job.resolution == 1000 ? 2.54e-6 : 2.54e-5);
    if (!(job.flags & 1)) u *= job.resolution;    // low-resolution database
    job.unit_mm = u;
    if (elements) {
        Ctx c{r, job, attr, ver > 9};
        read_block(c, -1);
        if (r.bad) job.warnings.push_back("element stream ended early at offset " + std::to_string(r.o) + " of " + std::to_string(n) +
                                          " (" + std::to_string(job.elements.size()) + " elements read)");
    } else {
        job.warnings.push_back("job has no element stream");
    }
    return true;
}

bool read_job_file(const std::string& path, Job& job, std::string& err) {
    std::vector<uint8_t> d;
    if (!read_file(path, d, err)) return false;
    return read_job(d.data(), d.size(), job, err);
}

// ---------------------------------------------------------------------------
std::string type_code(int t) {
    static const char* low[] = {"SIG", "PAD", "DRI", "VIA", "VID", "CPU", "CPL", "SKU", "SKL", "MSU", "MSL", "SPU", "SPL"};
    static const char* hi[] = {"PTH", "NTH", "BLI", "BUR", "ROU", "RPE", "RNE", "RPI", "RNI", "PLA", "SCG", "STU", "STL",
                               "CMU", "CML", "BDU", "BDL"};
    char b[16];
    if (t >= 0 && t <= 12) return low[t];
    if (t >= 13 && t <= 17) { std::snprintf(b, sizeof b, "GD%d", t - 12); return b; }
    if (t >= 18 && t <= 22) { std::snprintf(b, sizeof b, "GS%d", t - 17); return b; }
    if (t >= 23 && t <= 27) { std::snprintf(b, sizeof b, "PW%d", t - 22); return b; }
    if (t >= 28 && t <= 32) { std::snprintf(b, sizeof b, "PS%d", t - 27); return b; }
    if (t == 33) return "OTH";
    if (t == 34) return "BOL";
    if (t == 35) return "NFL";
    if (t >= 36 && t <= 134) { std::snprintf(b, sizeof b, "P%02d", t - 35); return b; }
    if (t >= 135 && t <= 151) return hi[t - 135];
    return "OTH";
}

std::string type_name(int t) {
    const std::string c = type_code(t);
    static const std::map<std::string, const char*> m = {
        {"SIG", "Signal"}, {"PAD", "Padmaster"}, {"DRI", "Drill"}, {"VIA", "Via"}, {"VID", "Drill via"},
        {"CPU", "Copper upper"}, {"CPL", "Copper lower"}, {"SKU", "Silkscreen upper"}, {"SKL", "Silkscreen lower"},
        {"MSU", "Soldermask upper"}, {"MSL", "Soldermask lower"}, {"SPU", "Solderpaste upper"}, {"SPL", "Solderpaste lower"},
        {"OTH", "Other"}, {"BOL", "Board outline"}, {"NFL", "Non-functional"}, {"PTH", "Plated holes"},
        {"NTH", "Non-plated holes"}, {"BLI", "Blind via"}, {"BUR", "Buried via"}, {"ROU", "Rout"}, {"STU", "Stencil upper"},
        {"STL", "Stencil lower"}, {"CMU", "Components upper"}, {"CML", "Components lower"}, {"SCG", "Scoring"}, {"PLA", "Plating"}};
    auto it = m.find(c);
    if (it != m.end()) return it->second;
    if (c.rfind("GD", 0) == 0) return "Ground plane " + c.substr(2);
    if (c.rfind("PW", 0) == 0) return "Power plane " + c.substr(2);
    if (c.rfind("GS", 0) == 0 || c.rfind("PS", 0) == 0) return "Plane split " + c.substr(2);
    if (c[0] == 'P') return "Plane " + c.substr(1);
    if (c[0] == 'R') return "Rout";
    if (c[0] == 'B') return "Back drill";
    return c;
}

// ---------------------------------------------------------------------------
// Gerber output
// ---------------------------------------------------------------------------
namespace {

struct GerberOut {
    const Job& job;
    std::string s;
    int frac = 6;
    double unit = 1e-6;          // one database unit in file units (inch or mm)
    char buf[160];
    void line(const char* t) { s += t; s += '\n'; }
    std::string num(double v) {   // decimal in file units
        std::snprintf(buf, sizeof buf, "%.*f", std::min(frac + 1, 9), v);
        std::string r = buf;
        if (r.find('.') != std::string::npos) { while (r.back() == '0') r.pop_back(); if (r.back() == '.') r.pop_back(); }
        if (r == "-0") r = "0";
        return r;
    }
    std::string sz(int32_t v) { return num(std::max(0, v) * unit); }
    void xy(int32_t x, int32_t y, const char* d) {
        std::snprintf(buf, sizeof buf, "X%dY%d%s*\n", (int)x, (int)y, d);
        s += buf;
    }
};

// polygon contour -> list of (point, arc?) for G36
struct Seg { int32_t x, y; bool arc; int32_t cx, cy; bool ccw; };

std::vector<Seg> contour_vertices(const Contour& c) {
    std::vector<Seg> v;
    const auto& w = c.words;
    size_t i = 0;
    while (i + 1 < w.size()) {
        if (w[i] == kArc) {
            if (i + 6 >= w.size()) break;
            v.push_back({w[i + 5], w[i + 6], true, w[i + 1], w[i + 2], (w[i + 4] & 2) != 0});
            i += 7;
        } else {
            v.push_back({w[i], w[i + 1], false, 0, 0, false});
            i += 2;
        }
    }
    return v;
}

void emit_contour(GerberOut& g, const Contour& c, bool& arc_mode) {
    auto v = contour_vertices(c);
    if (v.size() < 2) return;
    g.line("G36*");
    g.xy(v[0].x, v[0].y, "D02");
    for (size_t k = 0; k < v.size(); ++k) {
        const Seg& a = v[k];
        const Seg& b = v[(k + 1) % v.size()];
        if (a.arc) {
            if (!arc_mode) { g.line("G75*"); arc_mode = true; }
            std::snprintf(g.buf, sizeof g.buf, "%sX%dY%dI%dJ%dD01*\n", a.ccw ? "G03" : "G02", (int)b.x, (int)b.y,
                          (int)(a.cx - a.x), (int)(a.cy - a.y));
            g.s += g.buf;
        } else {
            std::snprintf(g.buf, sizeof g.buf, "G01X%dY%dD01*\n", (int)b.x, (int)b.y);
            g.s += g.buf;
        }
    }
    g.line("G37*");
}

} // namespace

std::string to_gerber(const Job& job, int layer_index, std::vector<std::string>* warnings) {
    GerberOut g{job};
    const bool mm = job.metric();
    // unit_mm = 10^-frac (file units)
    const double unit_file = mm ? job.unit_mm : job.unit_mm / 25.4;
    g.frac = std::clamp((int)std::lround(-std::log10(unit_file)), 0, 9);
    g.unit = unit_file;
    const int intd = std::max(2, 10 - g.frac);
    std::set<std::string> warned;
    auto warn = [&](const std::string& w) { if (warnings && warned.insert(w).second) warnings->push_back(w); };
    const Layer* L = job.layer(layer_index);
    g.s += "G04 Converted from a ViewMate job by PCB Plotter*\n";
    if (L) g.s += "G04 Layer " + std::to_string(layer_index) + " " + L->name + " (" + type_code(L->type) + ")*\n";
    std::snprintf(g.buf, sizeof g.buf, "%%FSLAX%d%dY%d%d*%%\n%%MO%s*%%\n", intd, g.frac, intd, g.frac, mm ? "MM" : "IN");
    g.s += g.buf;
    if (L && L->negative()) g.line("%IPNEG*%");   // negative film (planes): render inverted

    // used D-codes -> Gerber D-codes (>= 10), tools for routes
    std::map<int, int> dmap;             // vm dcode -> gerber dcode
    std::map<int, int> tmap;             // tool -> gerber dcode
    int next_free = 10;
    std::set<int> taken;
    for (auto& e : job.elements) if (e.layer == layer_index && e.kind != Element::Polygon && e.kind != Element::Route && e.dcode >= 10) taken.insert(e.dcode);
    auto alloc = [&]() { while (taken.count(next_free)) ++next_free; taken.insert(next_free); return next_free; };
    std::string defs, macros;
    std::set<std::string> macro_done;
    auto def_ap = [&](int gd, const Aperture* a, int32_t tool_dia) {
        char b[256];
        if (!a) {
            std::snprintf(b, sizeof b, "%%ADD%dC,%s*%%\n", gd, g.sz(tool_dia).c_str());
            defs += b; return;
        }
        const std::string w = g.sz(a->w), h = g.sz(a->h);
        switch (a->shape) {
        case ShRound: case ShTool: case ShPolyOutline: case ShBlank: case ShUndefined:
            std::snprintf(b, sizeof b, "%%ADD%dC,%s*%%\n", gd, w.c_str()); break;
        case ShSquare: std::snprintf(b, sizeof b, "%%ADD%dR,%sX%s*%%\n", gd, w.c_str(), w.c_str()); break;
        case ShRect:
            if (a->h <= 0) std::snprintf(b, sizeof b, "%%ADD%dR,%sX%s*%%\n", gd, w.c_str(), w.c_str());
            else std::snprintf(b, sizeof b, "%%ADD%dR,%sX%s*%%\n", gd, w.c_str(), h.c_str());
            break;
        case ShObround: case ShDrawnObround:
            if (a->h <= 0) std::snprintf(b, sizeof b, "%%ADD%dC,%s*%%\n", gd, w.c_str());
            else std::snprintf(b, sizeof b, "%%ADD%dO,%sX%s*%%\n", gd, w.c_str(), h.c_str());
            break;
        case ShOctagon: {
            const double across = (a->w) * g.unit / std::cos(3.14159265358979323846 / 8);   // w = across flats (assumed)
            std::snprintf(b, sizeof b, "%%ADD%dP,%sX8X22.5*%%\n", gd, g.num(across).c_str());
            warn("octagon apertures: size taken as across-flats (unverified)");
            break;
        }
        case ShDiamond: {
            const std::string name = "VMDIA" + std::to_string(gd);
            const double hw = a->w * g.unit / 2, hh = (a->h > 0 ? a->h : a->w) * g.unit / 2;
            macros += "%AM" + name + "*\n4,1,4," + g.num(-hw) + ",0,0," + g.num(-hh) + "," + g.num(hw) + ",0,0," + g.num(hh) +
                      "," + g.num(-hw) + ",0,0*%\n";
            std::snprintf(b, sizeof b, "%%ADD%d%s*%%\n", gd, name.c_str());
            break;
        }
        case ShRoundRect: {
            const double W = a->w * g.unit, H = (a->h > 0 ? a->h : a->w) * g.unit;
            double r = a->p0 > 0 ? a->p0 * g.unit : std::min(W, H) * 0.25;
            r = std::min(r, std::min(W, H) / 2);
            const std::string name = "VMRR" + std::to_string(gd);
            const double ix = W / 2 - r, iy = H / 2 - r;
            macros += "%AM" + name + "*\n21,1," + g.num(W) + "," + g.num(H - 2 * r) + ",0,0,0*\n21,1," + g.num(W - 2 * r) + "," +
                      g.num(H) + ",0,0,0*\n";
            for (int sx : {-1, 1}) for (int sy : {-1, 1})
                macros += "1,1," + g.num(2 * r) + "," + g.num(sx * ix) + "," + g.num(sy * iy) + "*\n";
            macros += "%\n";
            std::snprintf(b, sizeof b, "%%ADD%d%s*%%\n", gd, name.c_str());
            warn("rounded-rectangle apertures: corner radius from the first parameter word (unverified)");
            break;
        }
        case ShThermal: {
            const std::string name = "VMTH" + std::to_string(gd);
            const double od = a->w * g.unit, id = a->th_inner * g.unit, gap = a->h * g.unit;
            macros += "%AM" + name + "*\n7,0,0," + g.num(od) + "," + g.num(std::min(id, od * 0.999)) + "," + g.num(gap) + "," +
                      g.num(a->th_angle) + "*%\n";
            std::snprintf(b, sizeof b, "%%ADD%d%s*%%\n", gd, name.c_str());
            if (a->th_spokes != 4) warn("thermal with " + std::to_string(a->th_spokes) + " spokes exported with 4");
            break;
        }
        default:   // target, drawn target, custom
            std::snprintf(b, sizeof b, "%%ADD%dC,%s*%%\n", gd, w.c_str());
            warn(std::string("aperture shape ") + shape_letter(a->shape) + " approximated by a circle");
            break;
        }
        defs += b;
    };
    for (auto& e : job.elements) {
        if (e.layer != layer_index) continue;
        if (e.kind == Element::Polygon) continue;
        if (e.kind == Element::Route) {
            if (tmap.count(e.tool)) continue;
            int32_t dia = (e.tool >= 0 && e.tool < (int)job.tool_sizes.size()) ? job.tool_sizes[e.tool] : 0;
            int gd = alloc(); tmap[e.tool] = gd; def_ap(gd, nullptr, dia);
            continue;
        }
        if (dmap.count(e.dcode)) continue;
        auto it = job.apertures.find(e.dcode);
        int gd = e.dcode >= 10 ? e.dcode : alloc();
        dmap[e.dcode] = gd;
        if (it == job.apertures.end()) { warn("D" + std::to_string(e.dcode) + " is not in the D-code table (zero size)"); def_ap(gd, nullptr, 0); }
        else def_ap(gd, &it->second, 0);
    }
    g.s += macros;
    g.s += defs;
    g.line("G01*");
    bool clear = false, arc_mode = false;
    int cur_d = -1, cur_sr = 0;
    g.line("%LPD*%");
    auto set_d = [&](int gd) { if (gd != cur_d) { std::snprintf(g.buf, sizeof g.buf, "D%d*\n", gd); g.s += g.buf; cur_d = gd; } };
    auto set_pol = [&](bool c) { if (c != clear) { g.line(c ? "%LPC*%" : "%LPD*%"); clear = c; } };
    // innermost step-repeat group of an element (text groups are transparent)
    auto sr_of = [&](int grp) {
        for (int guard = 0; grp > 0 && grp <= (int)job.groups.size() && guard < 64; ++guard) {
            if (job.groups[grp - 1].kind == Group::StepRepeat) return grp;
            grp = job.groups[grp - 1].parent;
        }
        return 0;
    };
    for (auto& e : job.elements) {
        if (e.layer != layer_index) continue;
        const int sr = sr_of(e.group);
        if (sr != cur_sr) {
            if (cur_sr) g.line("%SR*%");
            cur_sr = 0;
            if (sr) {
                const Group& G = job.groups[sr - 1];
                const int nx = G.raw[0], ny = G.raw[1];
                if (nx >= 1 && ny >= 1 && nx <= 1000 && ny <= 1000) {
                    g.s += "%SRX" + std::to_string(nx) + "Y" + std::to_string(ny) + "I" + g.num(G.raw[2] * g.unit) + "J" +
                           g.num(G.raw[3] * g.unit) + "*%\n";
                    cur_sr = sr;
                    warn("step-and-repeat block expanded as nx,ny,dx,dy (interpretation not verified against a sample)");
                } else {
                    warn("step-and-repeat block with unexpected parameters exported once");
                }
            }
        }
        set_pol(e.clear);
        switch (e.kind) {
        case Element::Pad:
            set_d(dmap[e.dcode]);
            g.xy(e.v[0], e.v[1], "D03");
            break;
        case Element::Trace:
            set_d(dmap[e.dcode]);
            g.xy(e.v[0], e.v[1], "D02");
            g.xy(e.v[2], e.v[3], "D01");
            break;
        case Element::Arc: {
            set_d(dmap[e.dcode]);
            if (!arc_mode) { g.line("G75*"); arc_mode = true; }
            const int32_t cx = e.v[4], cy = e.v[5];
            g.xy(cx + e.v[0], cy + e.v[1], "D02");
            std::snprintf(g.buf, sizeof g.buf, "G02X%dY%dI%dJ%dD01*\nG01*\n", (int)(cx + e.v[2]), (int)(cy + e.v[3]), (int)-e.v[0], (int)-e.v[1]);
            g.s += g.buf;
            break;
        }
        case Element::Polygon:
            for (size_t k = 0; k < e.contours.size(); ++k) {
                set_pol(k == 0 ? e.clear : !e.clear);   // first contour = outline, the rest are holes
                emit_contour(g, e.contours[k], arc_mode);
            }
            if (e.contours.size() > 1) warn("polygon holes exported as clear-polarity regions");
            set_pol(e.clear);
            break;
        case Element::Route: {
            set_d(tmap[e.tool]);
            auto v = e.contours.empty() ? std::vector<Seg>{} : contour_vertices(e.contours[0]);
            if (v.size() >= 2) {
                g.xy(v[0].x, v[0].y, "D02");
                for (size_t k = 0; k + 1 < v.size(); ++k) {
                    const Seg& a = v[k]; const Seg& b = v[k + 1];
                    if (a.arc) {
                        if (!arc_mode) { g.line("G75*"); arc_mode = true; }
                        std::snprintf(g.buf, sizeof g.buf, "%sX%dY%dI%dJ%dD01*\nG01*\n", a.ccw ? "G03" : "G02", (int)b.x, (int)b.y,
                                      (int)(a.cx - a.x), (int)(a.cy - a.y));
                        g.s += g.buf;
                    } else g.xy(b.x, b.y, "D01");
                }
            }
            break;
        }
        }
    }
    if (cur_sr) g.line("%SR*%");
    g.line("M02*");
    return g.s;
}

// ---------------------------------------------------------------------------
// writer (tests)
// ---------------------------------------------------------------------------
namespace {
struct Wr {
    std::vector<uint8_t> o;
    void u8(int v) { o.push_back(uint8_t(v)); }
    void u16(int v) { u8(v & 255); u8((v >> 8) & 255); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8((v >> (8 * i)) & 255); }
    void f64(double d) { uint8_t b[8]; std::memcpy(b, &d, 8); o.insert(o.end(), b, b + 8); }
    void bytes(const void* p, size_t n) { o.insert(o.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void rec(int tag, const std::vector<uint8_t>& body) { u8(tag); u32((uint32_t)body.size()); bytes(body.data(), body.size()); }
};
} // namespace

std::vector<uint8_t> write_job(const Job& job) {
    Wr w;
    char hdr[64];
    std::snprintf(hdr, sizeof hdr, "%s%d\r\n", kSig, 12);
    w.bytes(hdr, std::strlen(hdr));
    w.u8(0);
    for (auto& L : job.layers) {
        std::string a = L.raw;
        if (a.empty())
            a = "name=" + L.name + "\x01" "color=" + std::to_string(L.color) + "\x01" "cfg=state=" + std::to_string(L.state) +
                "\x02" "type=" + std::to_string(L.type) + "\x02" "bnum=" + std::to_string(L.bnum);
        Wr b; b.u32((uint32_t)L.index); b.u32((uint32_t)a.size() + 1); b.bytes(a.c_str(), a.size() + 1);
        w.rec(29, b.o);
    }
    { Wr b; b.u32(job.flags); w.rec(22, b.o); }
    { Wr b; b.u32((uint32_t)job.resolution); w.rec(34, b.o); }
    { Wr b; b.u8(job.units); for (int i = 0; i < 4; ++i) b.u32((uint32_t)job.bbox[i]); w.rec(3, b.o); }
    if (!job.tool_sizes.empty()) { Wr b; for (auto t : job.tool_sizes) b.u32((uint32_t)t); w.rec(27, b.o); }
    w.u8(1);
    for (auto& [dc, a] : job.apertures) {
        w.u16(dc);
        std::vector<uint8_t> extra = a.extra;
        if (a.shape == ShThermal && extra.empty()) {
            Wr e; e.u32((uint32_t)a.th_inner); e.u32((uint32_t)a.th_spokes); e.f64(a.th_angle); extra = e.o;
        }
        const bool has_extra = a.shape == ShThermal || a.shape == ShCustom;
        w.u32(has_extra ? (uint32_t)extra.size() : (uint32_t)a.p0); w.u32((uint32_t)a.w); w.u32((uint32_t)a.h);
        w.u8(a.shape); w.u8(a.shape2); w.u16(0);
        if (has_extra) w.bytes(extra.data(), extra.size());
    }
    w.u16(0);
    w.u8(101); w.u32(0x7FFFFFFF);
    // groups: consecutive elements with identical (kind, dcode, pol, layer)
    static const int stored_type[5] = {0, 1, 2, 4, 6};   // class -> stored type (inverse of kTypeMap)
    size_t i = 0;
    const auto& E = job.elements;
    while (i < E.size()) {
        size_t j = i;
        while (j < E.size() && E[j].kind == E[i].kind && E[j].dcode == E[i].dcode && E[j].clear == E[i].clear && E[j].layer == E[i].layer) ++j;
        w.u32(uint32_t(j - i)); w.u32((uint32_t)E[i].dcode); w.u32((uint32_t)stored_type[E[i].kind]); w.u32(E[i].clear ? 1 : 0);
        w.u32((uint32_t)E[i].layer); w.u32(1);
        for (size_t k = i; k < j; ++k) {
            const Element& e = E[k];
            switch (e.kind) {
            case Element::Pad: w.u32(e.v[0]); w.u32(e.v[1]); break;
            case Element::Trace: for (int q = 0; q < 4; ++q) w.u32(e.v[q]); break;
            case Element::Arc: for (int q = 0; q < 6; ++q) w.u32(e.v[q]); break;
            case Element::Polygon:
            case Element::Route:
                if (e.kind == Element::Polygon) w.u32((uint32_t)e.contours.size());
                for (auto& c : e.contours) {
                    w.u32((uint32_t)c.words.size()); w.u32(c.hdr1); w.u32(c.flags);
                    for (auto x : c.words) w.u32((uint32_t)x);
                    if (e.kind == Element::Route) break;
                }
                if (e.kind == Element::Route) { w.u32((uint32_t)e.tool); w.u32(0); w.u32(0); w.u8(0); for (int q = 0; q < 72; ++q) w.u8(0); }
                break;
            }
            w.u32(e.attr);
        }
        i = j;
    }
    return w.o;
}

} // namespace pcbp::io::vm
