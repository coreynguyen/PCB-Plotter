// ============================================================================
// sample_plugin.cpp — minimal PCB Plotter plugin DLL (SDK example).
//
// Built only against sdk/pcbp_plugin.h; no PCB Plotter internals. Provides:
//   exporter "sample.semicolon"  MachineJob JSON -> "REF;X;Y;ROT;PART" lines
//   importer "sample.simplexy"   "REF X Y ROT [PART]" lines (mm, deg CCW)
//                                -> partial project JSON (components only)
//
// Build (mingw):  x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared -static -I sdk
//                     sdk/sample_plugin/sample_plugin.cpp -o sample_plugin.dll
// Build (MSVC):   cl /O2 /LD /EHsc /I sdk sdk\sample_plugin\sample_plugin.cpp
// Drop the DLL into "<PCBPlotter.exe dir>\plugins\".
// ============================================================================
#include "pcbp_plugin.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <cstdint>

namespace {

// ---------------------------------------------------------------------------
// Tiny JSON reader (enough for the host payloads: objects, arrays, strings,
// numbers, true/false/null). No exceptions; parse() returns false on error.
// ---------------------------------------------------------------------------
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;

    const J* get(const char* k) const {
        if (t != Obj) return nullptr;
        for (auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    double num(const char* k, double def = 0) const { const J* v = get(k); return v && v->t == Num ? v->n : def; }
    std::string str(const char* k) const { const J* v = get(k); return v && v->t == Str ? v->s : std::string(); }
    bool flag(const char* k) const { const J* v = get(k); return v && ((v->t == Bool && v->b) || (v->t == Num && v->n != 0)); }
};

struct Parser {
    const char* p;
    const char* e;
    void ws() { while (p < e && std::isspace((unsigned char)*p)) ++p; }
    bool lit(const char* w) { size_t n = std::strlen(w); if ((size_t)(e - p) < n || std::strncmp(p, w, n)) return false; p += n; return true; }
    static void put_utf8(std::string& s, unsigned c) {
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 63)); s += (char)(0x80 | (c & 63)); }
        else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 63)); s += (char)(0x80 | ((c >> 6) & 63)); s += (char)(0x80 | (c & 63)); }
    }
    bool hex4(unsigned& v) {
        if (e - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return false;
        }
        return true;
    }
    bool string(std::string& s) {
        if (p >= e || *p != '"') return false;
        ++p;
        while (p < e && *p != '"') {
            if (*p != '\\') { s += *p++; continue; }
            if (++p >= e) return false;
            char c = *p++;
            switch (c) {
            case 'n': s += '\n'; break; case 't': s += '\t'; break; case 'r': s += '\r'; break;
            case 'b': s += '\b'; break; case 'f': s += '\f'; break;
            case 'u': {
                unsigned v; if (!hex4(v)) return false;
                if (v >= 0xD800 && v < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2; unsigned lo; if (!hex4(lo)) return false;
                    v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                }
                put_utf8(s, v);
                break;
            }
            default: s += c;
            }
        }
        if (p >= e) return false;
        ++p;
        return true;
    }
    bool value(J& v, int depth = 0) {
        if (depth > 64) return false;
        ws();
        if (p >= e) return false;
        if (*p == '{') {
            ++p; v.t = J::Obj; ws();
            if (p < e && *p == '}') { ++p; return true; }
            for (;;) {
                ws(); std::string k;
                if (!string(k)) return false;
                ws(); if (p >= e || *p != ':') return false; ++p;
                J x; if (!value(x, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(x));
                ws(); if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; return true; }
                return false;
            }
        }
        if (*p == '[') {
            ++p; v.t = J::Arr; ws();
            if (p < e && *p == ']') { ++p; return true; }
            for (;;) {
                J x; if (!value(x, depth + 1)) return false;
                v.a.push_back(std::move(x));
                ws(); if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; return true; }
                return false;
            }
        }
        if (*p == '"') { v.t = J::Str; return string(v.s); }
        if (lit("true")) { v.t = J::Bool; v.b = true; return true; }
        if (lit("false")) { v.t = J::Bool; return true; }
        if (lit("null")) return true;
        char* end = nullptr;
        std::string tmp(p, (size_t)std::min<ptrdiff_t>(e - p, 64));
        v.n = std::strtod(tmp.c_str(), &end);
        if (end == tmp.c_str()) return false;
        p += end - tmp.c_str();
        v.t = J::Num;
        return true;
    }
};

bool parse_json(const char* text, J& out) {
    if (!text) return false;
    Parser ps{text, text + std::strlen(text)};
    if (!ps.value(out)) return false;
    ps.ws();
    return ps.p == ps.e;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
char* dup_str(const std::string& s) {
    char* m = (char*)std::malloc(s.size() + 1);
    if (m) std::memcpy(m, s.c_str(), s.size() + 1);
    return m;
}

std::string json_escape(const std::string& s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { r += '\\'; r += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); r += b; }
        else r += (char)c;
    }
    return r + '"';
}

std::string fmt(double v, int dec) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", dec, v);   // host locale is "C" for plugins; keep it simple
    for (char* q = b; *q; ++q) if (*q == ',') *q = '.';
    return b;
}

void log(const pcbp_host* h, int lvl, const std::string& m) {
    if (h && h->log) h->log(h->ctx, lvl, m.c_str());
}

bool parse_number(const std::string& t, double& v) {
    if (t.empty()) return false;
    char* end = nullptr;
    v = std::strtod(t.c_str(), &end);
    return end && *end == 0;
}

std::vector<std::string> split_ws(const std::string& line) {
    std::vector<std::string> r;
    std::string cur;
    for (char c : line) {
        if (std::isspace((unsigned char)c)) { if (!cur.empty()) { r.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) r.push_back(cur);
    return r;
}

std::vector<std::string> split_lines(const char* data, size_t n) {
    std::vector<std::string> r;
    std::string cur;
    for (size_t i = 0; i < n; ++i) {
        char c = data[i];
        if (c == '\n' || c == '\r') { r.push_back(cur); cur.clear(); if (c == '\r' && i + 1 < n && data[i + 1] == '\n') ++i; }
        else cur += c;
    }
    if (!cur.empty()) r.push_back(cur);
    return r;
}

bool is_comment(const std::string& l) {
    size_t i = 0;
    while (i < l.size() && std::isspace((unsigned char)l[i])) ++i;
    return i == l.size() || l[i] == '#' || l[i] == ';';
}

// ---------------------------------------------------------------------------
// exporter: sample.semicolon
// ---------------------------------------------------------------------------
int PCBP_CALL export_semicolon(const pcbp_host* host, const char* input_json, const char* options_json,
                               uint8_t** out_data, size_t* out_size, char** out_error) {
    J job, opts;
    if (!parse_json(input_json, job) || job.str("format") != "PCBPlotter.MachineJob") {
        *out_error = dup_str("input is not a PCBPlotter.MachineJob");
        return 0;
    }
    parse_json(options_json, opts);            // options are optional
    const int dec = (int)opts.num("decimals", 3);
    const bool skipped = opts.flag("include_skipped");
    const J* pl = job.get("placements");
    std::string out;
    int n = 0;
    if (pl && pl->t == J::Arr) {
        for (const J& p : pl->a) {
            if (p.flag("skip") && !skipped) continue;
            out += p.str("ref") + ";" + fmt(p.num("x"), dec) + ";" + fmt(p.num("y"), dec) + ";" +
                   fmt(p.num("rot"), 2) + ";" + p.str("part") + "\n";
            ++n;
        }
    }
    log(host, PCBP_LOG_INFO, "sample.semicolon: wrote " + std::to_string(n) + " placements");
    *out_data = (uint8_t*)std::malloc(out.size() ? out.size() : 1);
    if (!*out_data) { *out_error = dup_str("out of memory"); return 0; }
    std::memcpy(*out_data, out.data(), out.size());
    *out_size = out.size();
    return 1;
}

// ---------------------------------------------------------------------------
// importer: sample.simplexy
// ---------------------------------------------------------------------------
int PCBP_CALL sniff_simplexy(const char* path, const uint8_t* head, size_t size) {
    (void)path;
    int good = 0, bad = 0;
    for (auto& l : split_lines((const char*)head, size)) {
        if (is_comment(l)) continue;
        auto t = split_ws(l);
        double v;
        if ((t.size() == 4 || t.size() == 5) && parse_number(t[1], v) && parse_number(t[2], v) && parse_number(t[3], v)) ++good;
        else ++bad;
        if (good + bad >= 20) break;
    }
    return (good > 0 && bad == 0) ? 25 : 0;    // low: generic centroid importers usually score higher
}

int PCBP_CALL import_simplexy(const pcbp_host* host, const char* path, const uint8_t* data, size_t size,
                              const char* options_json, char** out_json, char** out_error) {
    (void)path; (void)options_json;
    std::string comps;
    int n = 0, line_no = 0;
    for (auto& l : split_lines((const char*)data, size)) {
        ++line_no;
        if (is_comment(l)) continue;
        auto t = split_ws(l);
        double x, y, r;
        if (t.size() < 4 || t.size() > 5 || !parse_number(t[1], x) || !parse_number(t[2], y) || !parse_number(t[3], r)) {
            *out_error = dup_str("line " + std::to_string(line_no) + ": expected 'REF X Y ROT [PART]'");
            return 0;
        }
        if (n++) comps += ",\n";
        comps += "  {\"ref\":" + json_escape(t[0]) + ",\"at\":[" + fmt(x, 6) + "," + fmt(y, 6) + "],\"rot\":" +
                 fmt(r, 6) + ",\"side\":\"T\",\"number\":" + json_escape(t.size() == 5 ? t[4] : "") + "}";
    }
    std::string js = "{\"format\":\"PCBPlotter.Project\",\"version\":1,\"components\":[\n" + comps + "\n]}";
    *out_json = dup_str(js);
    log(host, PCBP_LOG_INFO, "sample.simplexy: " + std::to_string(n) + " components");
    return *out_json ? 1 : 0;
}

void PCBP_CALL free_mem(void* p) { std::free(p); }

pcbp_plugin_desc make_exporter() {
    pcbp_plugin_desc d{};
    d.struct_size = sizeof(pcbp_plugin_desc);
    d.api_version = PCBP_PLUGIN_API_VERSION;
    d.id = "sample.semicolon";
    d.name = "Sample semicolon list";
    d.vendor = "PCB Plotter SDK sample";
    d.kind = PCBP_KIND_EXPORTER;
    d.file_filter = "*.txt";
    d.default_ext = "txt";
    d.options_schema_json =
        "[{\"key\":\"decimals\",\"label\":\"Decimals\",\"type\":\"int\",\"default\":3},"
        "{\"key\":\"include_skipped\",\"label\":\"Include skipped placements\",\"type\":\"bool\",\"default\":false}]";
    d.export_input = PCBP_EXPORT_MACHINE_JOB;
    d.export_file = export_semicolon;
    d.free_mem = free_mem;
    return d;
}

pcbp_plugin_desc make_importer() {
    pcbp_plugin_desc d{};
    d.struct_size = sizeof(pcbp_plugin_desc);
    d.api_version = PCBP_PLUGIN_API_VERSION;
    d.id = "sample.simplexy";
    d.name = "Sample simple XY (mm)";
    d.vendor = "PCB Plotter SDK sample";
    d.kind = PCBP_KIND_IMPORTER;
    d.file_filter = "*.sxy;*.txt";
    d.default_ext = "sxy";
    d.options_schema_json = "[]";
    d.import_mode = PCBP_IMPORT_MERGE;
    d.sniff = sniff_simplexy;
    d.import_file = import_simplexy;
    d.free_mem = free_mem;
    return d;
}

const pcbp_plugin_desc g_list[2] = {make_exporter(), make_importer()};

} // namespace

extern "C" PCBP_EXPORT int PCBP_CALL pcbp_plugin_enumerate(uint32_t host_api_version, const pcbp_plugin_desc** list,
                                                           uint32_t* count) {
    if (host_api_version < PCBP_PLUGIN_API_VERSION || !list || !count) return 0;
    *list = g_list;
    *count = 2;
    return 1;
}
