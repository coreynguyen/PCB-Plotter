// ============================================================================
// juki_writer.cpp — JUKI HLC program writer + minimal reader.
//
// Layout facts: specs/JUKI_EXPORT.md (sections referenced as "§n" below).
// Records are built as zero-filled buffers and fields are stored at their
// documented absolute offsets, so every non-zero byte has a spec entry.
// ============================================================================
#include "io/juki_writer.h"

#include <algorithm>
#include <tuple>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <sstream>

namespace pcbp::juki {

// ---------------------------------------------------------------------------
// Legacy (ANSI) text: UTF-8 -> printable 7-bit ASCII with a readable
// transliteration (REVIEW_V12 P0-03). Bytes that are not valid UTF-8 are read
// as Windows-1252 (old projects). Unmappable characters become '_'.
// ---------------------------------------------------------------------------
static const char* translit(uint32_t c) {
    switch (c) {
        case 0x00B1: return "+/-";  case 0x00B5: case 0x03BC: return "u";
        case 0x2126: case 0x03A9: return "Ohm";  case 0x00B0: case 0x02DA: return "deg";
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: case 0x00B4: case 0x0060: return "'";
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
        case 0x00D7: case 0x2715: return "x";  case 0x00F7: return "/";  case 0x2026: return "...";
        case 0x2264: return "<=";  case 0x2265: return ">=";  case 0x2248: return "~";  case 0x2260: return "!=";
        case 0x2122: return "(TM)";  case 0x00AE: return "(R)";  case 0x00A9: return "(C)";
        case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: case 0x0009: return " ";
        case 0x00B2: return "2";  case 0x00B3: return "3";  case 0x00B9: return "1";
        case 0x00BC: return "1/4";  case 0x00BD: return "1/2";  case 0x00BE: return "3/4";
        case 0x00DF: return "ss";  case 0x00C6: return "AE";  case 0x00E6: return "ae";
        case 0x0152: return "OE";  case 0x0153: return "oe";  case 0x00D8: return "O";  case 0x00F8: return "o";
        case 0x00D0: return "D";  case 0x00F0: return "d";  case 0x00DE: return "Th";  case 0x00FE: return "th";
        case 0x2030: return "permil";  case 0x20AC: return "EUR";  case 0x00A3: return "GBP";  case 0x00A5: return "JPY";
        case 0x2022: case 0x00B7: return "*";  case 0x2192: return "->";  case 0x2190: return "<-";
        case 0x221E: return "inf";  case 0x0394: return "D";  case 0x03C0: return "pi";
        default: break;
    }
    // Latin-1 / Latin Extended-A letters -> base letter
    static const char* l1 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (c >= 0xC0 && c <= 0xFF) { static char one[2]; one[0] = l1[c - 0xC0]; one[1] = 0; return one; }
    if (c >= 0x100 && c <= 0x17F) {
        static const char* ea = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
        static char one[2]; one[0] = ea[c - 0x100]; one[1] = 0; return one;
    }
    return nullptr;
}

static uint32_t cp1252(uint8_t b) {
    static const uint16_t hi[32] = {0x20AC,0,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0,0x017D,0,
                                    0,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0,0x017E,0x0178};
    return b >= 0x80 && b < 0xA0 ? hi[b - 0x80] : b;
}

std::string legacy_text(const std::string& s, int* changed) {
    std::string o;
    o.reserve(s.size());
    int ch = 0;
    for (size_t i = 0; i < s.size();) {
        const uint8_t b = (uint8_t)s[i];
        uint32_t c = b;
        size_t n = 1;
        if (b >= 0x80) {
            size_t need = b >= 0xF0 && b < 0xF8 ? 3 : b >= 0xE0 ? 2 : b >= 0xC2 && b < 0xE0 ? 1 : 0;
            bool ok = need != 0 && i + need < s.size();
            if (ok) {
                c = b & (0x3F >> need);
                for (size_t k = 1; k <= need; ++k) {
                    if (((uint8_t)s[i + k] & 0xC0) != 0x80) { ok = false; break; }
                    c = c << 6 | ((uint8_t)s[i + k] & 0x3F);
                }
            } else ok = false;
            if (ok) n = need + 1; else c = cp1252(b);
        }
        i += n;
        if (c >= 0x20 && c < 0x7F) { o += (char)c; continue; }
        ++ch;
        if (c < 0x20 || c == 0x7F) { if (c == '\t' || c == '\n' || c == '\r') o += ' '; continue; }
        if (c == 0xFEFF || c == 0x200B || c == 0x00AD) continue;   // BOM, zero-width, soft hyphen
        const char* t = translit(c);
        o += t ? t : "_";
    }
    if (changed) *changed = ch;
    return o;
}

std::vector<MachineName> machine_names(const std::vector<std::string>& parts, const std::vector<std::string>& preferred) {
    constexpr size_t kName = 20, kFull = 60;
    auto up = [](std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; };
    auto rtrim = [](std::string s) { while (!s.empty() && s.back() == ' ') s.pop_back(); return s; };
    const size_t n = parts.size();
    std::vector<MachineName> out(n);
    std::vector<std::string> base(n);
    for (size_t i = 0; i < n; ++i) {
        base[i] = legacy_text(parts[i]);
        if (base[i].empty()) base[i] = "PART";
    }
    // names that exist as they are: every part number of up to 20 characters, and the DB names of the others
    std::set<std::string> reserved, taken;
    std::map<std::string, int> prefix_users;   // first 20 characters of the long part numbers
    for (size_t i = 0; i < n; ++i) {
        if (base[i].size() <= kName) reserved.insert(up(base[i]));
        else ++prefix_users[up(rtrim(base[i].substr(0, kName)))];
        if (i < preferred.size() && !preferred[i].empty() && preferred[i].size() <= kName) reserved.insert(up(preferred[i]));
    }
    auto free_name = [&](const std::string& c, const std::string& own) {
        const std::string u = up(c);
        return !c.empty() && !taken.count(u) && (!reserved.count(u) || u == up(own));
    };
    auto numbered = [&](const std::string& b, const std::string& own) {
        for (int k = 1; k < 100000; ++k) {
            const std::string suf = "~" + std::to_string(k);
            const std::string c = rtrim(b.substr(0, kName - suf.size())) + suf;
            if (free_name(c, own)) return c;
        }
        return rtrim(b.substr(0, kName));
    };
    // part numbers of up to 20 characters first: they keep their name unless an earlier one has it already
    for (size_t i = 0; i < n; ++i) {
        if (base[i].size() > kName) continue;
        std::string nm = base[i];
        if (taken.count(up(nm))) { nm = numbered(base[i], std::string()); out[i].how = MachineName::Renamed; }
        taken.insert(up(nm));
        out[i].name = nm;
    }
    // longer part numbers: the DB name when free, else the first 20 characters when no other name has them
    std::map<std::string, std::vector<size_t>> clash;   // first 20 -> the long part numbers that need more
    for (size_t i = 0; i < n; ++i) {
        if (base[i].size() <= kName) continue;
        const std::string& b = base[i];
        const std::string pref = i < preferred.size() ? preferred[i] : std::string();
        const std::string head = rtrim(b.substr(0, kName));
        if (!pref.empty() && pref.size() <= kName && free_name(pref, pref)) {
            out[i].name = pref;
            out[i].how = up(pref) == up(head) ? MachineName::Truncated : MachineName::Renamed;
        } else if (prefix_users[up(head)] == 1 && free_name(head, std::string())) {
            out[i].name = head;
            out[i].how = MachineName::Truncated;
        } else {
            clash[up(head)].push_back(i);
            continue;
        }
        taken.insert(up(out[i].name));
    }
    // clashes: as much of the beginning as fits, "~", and an end of the part number that differs between all of the
    // clashing part numbers (whole tokens where it fits: "-TR" gives "TR"), at most 11 characters
    constexpr size_t kTail = 11;
    auto is_sep = [](char c) { return c == '-' || c == '_' || c == '/' || c == ' ' || c == '.' || c == ',' || c == '#'; };
    for (auto& [head, g] : clash) {
        std::vector<std::string> pick(g.size());
        auto tail_of = [&](const std::string& b, size_t t) {
            size_t from = b.size() - t;
            for (size_t q = from; q-- > 0 && b.size() - q - 1 <= kTail && q >= kName - 1 - kTail;)
                if (is_sep(b[q])) { from = q + 1; break; }
            return b.substr(from);
        };
        auto try_all = [&](const std::vector<std::string>& tails) {
            std::set<std::string> seen;
            std::vector<std::string> c(g.size());
            for (size_t k = 0; k < g.size(); ++k) {
                const std::string& b = base[g[k]];
                if (tails[k].empty() || tails[k].size() > kTail) return false;
                c[k] = rtrim(b.substr(0, kName - 1 - tails[k].size())) + "~" + tails[k];
                if (!seen.insert(up(c[k])).second || !free_name(c[k], std::string())) return false;
            }
            pick = c;
            return true;
        };
        bool done = false;
        for (size_t t = 1; !done && t <= kTail; ++t) {
            std::vector<std::string> tails(g.size()), raw(g.size());
            std::set<std::string> distinct;
            bool ok = true;
            for (size_t k = 0; k < g.size() && ok; ++k) {
                const std::string& b = base[g[k]];
                if (t >= b.size()) { ok = false; break; }
                raw[k] = up(b.substr(b.size() - t));
                ok = distinct.insert(raw[k]).second;
                tails[k] = tail_of(b, t);
            }
            if (ok) done = try_all(tails);
        }
        if (!done) {
            // the ends are the same: from where the part numbers start to differ
            std::vector<std::string> tails(g.size());
            for (size_t k = 0; k < g.size(); ++k) {
                const std::string& b = base[g[k]];
                size_t d = kName;
                for (size_t o = 0; o < g.size(); ++o) {
                    if (o == k) continue;
                    const std::string& x = base[g[o]];
                    size_t c = 0;
                    while (c < b.size() && c < x.size() && std::toupper((unsigned char)b[c]) == std::toupper((unsigned char)x[c])) ++c;
                    d = std::max(d, c);
                }
                tails[k] = d < b.size() ? b.substr(d, kTail) : std::string();
            }
            done = try_all(tails);
        }
        for (size_t k = 0; k < g.size(); ++k) {
            const size_t i = g[k];
            out[i].name = done ? pick[k] : numbered(base[i], std::string());
            out[i].how = MachineName::Renamed;
            taken.insert(up(out[i].name));
        }
    }
    // the long-name field: the part number itself, made unique where text conversion made two the same
    std::set<std::string> fulls;
    for (size_t i = 0; i < n; ++i) {
        std::string f = base[i].size() <= kName ? out[i].name : base[i];
        for (int k = 1; fulls.count(up(f.substr(0, kFull))); ++k) {
            const std::string suf = "~" + std::to_string(k);
            f = base[i].substr(0, std::min(base[i].size(), kFull - suf.size())) + suf;
            if (out[i].how == MachineName::Same) out[i].how = MachineName::Renamed;
        }
        fulls.insert(up(f.substr(0, kFull)));
        out[i].full = f;
    }
    return out;
}

namespace {

// ---------------------------------------------------------------------------
// Little-endian helpers
// ---------------------------------------------------------------------------
void put_u8(std::vector<uint8_t>& b, size_t off, uint8_t v) { b[off] = v; }
void put_u16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
    b[off] = (uint8_t)v; b[off + 1] = (uint8_t)(v >> 8);
}
void put_u32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = (uint8_t)(v >> (8 * i));
}
void put_i32(std::vector<uint8_t>& b, size_t off, int32_t v) { put_u32(b, off, (uint32_t)v); }
void put_i16(std::vector<uint8_t>& b, size_t off, int16_t v) { put_u16(b, off, (uint16_t)v); }

uint16_t get_u16(const std::vector<uint8_t>& b, size_t off) {
    return (uint16_t)(b[off] | (b[off + 1] << 8));
}
uint32_t get_u32(const std::vector<uint8_t>& b, size_t off) {
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
           ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}
int32_t get_i32(const std::vector<uint8_t>& b, size_t off) { return (int32_t)get_u32(b, off); }
std::string get_str(const std::vector<uint8_t>& b, size_t off, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && off + i < b.size() && b[off + i]; ++i) s += (char)b[off + i];
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// Fixed string field of `size` bytes: at most size-1 characters, NUL, zero
// padded (§2.3), text converted by legacy_text().
void put_str(std::vector<uint8_t>& b, size_t off, size_t size, const std::string& s) {
    // Every HLC text field is a native ANSI char[] (the JUKI tools marshal with
    // StringToHGlobalAnsi = the PC's ANSI code page: 1252 on the user's XP
    // stations, 932 on Japanese ones). 7-bit ASCII is the only common subset, so
    // UTF-8 project text is transliterated here (legacy_text, HLC_FORMAT §12).
    const std::string a = legacy_text(s);
    size_t n = std::min(a.size(), size - 1);
    for (size_t i = 0; i < n; ++i) b[off + i] = (uint8_t)a[i];
    for (size_t i = n; i < size; ++i) b[off + i] = 0;
}

// mm -> 1e-4 mm, angle deg -> 1/80 deg; round half away from zero (§2.2).
int32_t to_raw_len(double mm) { return (int32_t)std::lround(mm * 10000.0); }
int16_t to_raw_ang(double deg) {
    double a = std::fmod(deg, 360.0);
    if (a < 0) a += 360.0;
    long r = std::lround(a * 80.0);
    if (r >= 28800) r -= 28800;
    return (int16_t)r;
}

// ---------------------------------------------------------------------------
// Per-variant layout (§3, §4)
// ---------------------------------------------------------------------------
struct Layout {
    int code = 0;               // CircuitCAM format code, H8H uses 0 + overrides
    bool h8h = false;
    int entries = 28;           // block-table entries
    uint8_t station = 0, flag = 0x40;
    char ver[4] = {'9', '0', '0', 0};
    uint32_t opt_size = 4328;
    bool opt_marker = false;    // byte 1 at optimizer +39
    uint32_t li_size = 602;
    char li_ver[7] = {'9', '0', '0', '@', '9', '0', '0'};
    uint32_t max_place = 10000;
    uint32_t board_size = 40999;
    bool wide = true;           // board/mark layout family
    uint32_t place_size = 256;
    bool place_ext = true;      // extra u16 at placement +29 (H7H/H8H)
    uint32_t comp_size = 1792;
    int comp_tmpl = 9;          // 9, 7, 6, 5 (DEFCOMP family)
    uint32_t mark_elem = 3072;  // wide: element size
    uint32_t mark_entry = 128;  // wide: entry size
    uint32_t name_size = 32;
    uint16_t first_markset = 1;
};

bool make_layout(HlcVariant v, Layout& L, std::string& err) {
    L = Layout{};
    if (v == HlcVariant::H8H) {
        L.h8h = true;
        L.code = 0;
        L.ver[0] = 'C'; L.ver[1] = '0'; L.ver[2] = '0'; L.ver[3] = 'A';
        L.opt_size = 4672;
        const char lv[7] = {'C', '0', '0', '@', 'C', '0', '0'};
        std::memcpy(L.li_ver, lv, 7);
        L.comp_size = 1822;
        return true;
    }
    const int c = (int)v;
    if (c < 0 || c > 12 || c == 10 || c == 11) { err = "unsupported HLC variant"; return false; }
    L.code = c;
    if (c == 0) return true;   // H7H defaults above
    const bool narrow = (c >= 3 && c <= 7);
    L.wide = !narrow;
    L.place_ext = false;
    L.entries = narrow ? 27 : 26;
    switch (c) {   // header station byte / flag byte (§3.1)
        case 4: L.station = 11; L.flag = 136; break;
        case 5: L.station = 12; L.flag = 135; break;
        case 6: L.station = 13; L.flag = 134; break;
        case 7: L.station = 14; L.flag = 133; break;
        case 8: L.station = 32; break;
        case 9: L.station = 33; break;
        case 12: L.station = 48; break;
        default: break;
    }
    const char* hv = "111";
    const char* lv = "900@900";
    switch (c) {
        case 1: hv = "702"; lv = "700@702"; break;
        case 2: hv = "600"; lv = "600@600"; break;
        case 3: hv = "510"; lv = "500@500"; break;
        case 4: case 5: case 6: case 7: hv = "111"; lv = "500@500"; break;
        case 8: case 9: hv = "100"; break;
        case 12: hv = "150"; break;
    }
    std::memcpy(L.ver, hv, 3); L.ver[3] = 0;
    std::memcpy(L.li_ver, lv, 7);
    L.opt_marker = true;
    if (c == 1 || c == 12) L.opt_size = 1348;
    else if (c == 2 || c == 8 || c == 9) L.opt_size = 1340;
    else L.opt_size = 1276;
    L.li_size = narrow ? 582 : 562;
    L.max_place = (c == 1) ? 6000 : 3000;
    L.board_size = narrow ? 27118 : 10856;
    L.place_size = narrow ? 71 : 96;
    if (c == 1 || c == 12) { L.comp_size = 788; L.comp_tmpl = 7; }
    else if (c == 2 || c == 8 || c == 9) { L.comp_size = 744; L.comp_tmpl = 6; }
    else { L.comp_size = 504; L.comp_tmpl = 5; }
    L.mark_elem = 536;
    L.mark_entry = 124;
    L.name_size = 12;
    L.first_markset = narrow ? 0 : 1;
    return true;
}

// Component-record defaults (§4.5): non-zero bytes CircuitCAM's per-format
// default record carries outside the fields it overwrites. Meaning mostly
// unknown; values reproduced as documented defaults.
struct Def { uint16_t off; uint8_t len; uint8_t b[4]; };
const Def kDef9[] = {
    {33, 4, {0x78, 0xEC, 0xFF, 0xFF}}, {117, 1, {1}}, {248, 2, {1, 1}}, {278, 1, {1}},
    {308, 1, {1}}, {312, 1, {1}}, {316, 1, {1}}, {354, 3, {4, 0, 4}}, {422, 2, {1, 1}},
    {444, 1, {1}}, {448, 1, {1}}, {452, 1, {1}}, {465, 1, {1}}, {470, 1, {1}},
    {684, 1, {1}}, {687, 2, {0x88, 0x13}}, {695, 4, {4, 0, 0x88, 0x13}},
    {705, 2, {0x88, 0x13}}, {745, 2, {0x88, 0x13}}};
const Def kDef7[] = {
    {117, 1, {1}}, {248, 2, {1, 1}}, {278, 1, {1}}, {308, 1, {1}}, {354, 3, {4, 0, 4}},
    {402, 2, {1, 1}}, {424, 1, {1}}, {445, 1, {1}}, {477, 1, {1}}, {566, 1, {1}},
    {569, 2, {0x88, 0x13}}, {577, 4, {4, 0, 0x88, 0x13}}, {587, 2, {0x88, 0x13}},
    {591, 2, {0xD0, 0x07}}, {595, 2, {0x88, 0x13}}, {599, 2, {0xD0, 0x07}},
    {618, 2, {0x88, 0x13}}};
const Def kDef6[] = {
    {117, 1, {1}}, {248, 2, {1, 1}}, {278, 1, {1}}, {308, 1, {1}}, {354, 3, {4, 0, 4}},
    {402, 2, {1, 1}}, {424, 1, {1}}, {476, 1, {1}}, {526, 1, {1}},
    {529, 2, {0x88, 0x13}}, {537, 4, {4, 0, 0x88, 0x13}}, {547, 2, {0x88, 0x13}},
    {551, 2, {0xD0, 0x07}}, {574, 2, {0x88, 0x13}}};
const Def kDef5[] = {
    {109, 1, {1}}, {240, 2, {1, 1}}, {270, 1, {1}}, {306, 3, {4, 0, 4}}, {344, 2, {1, 1}},
    {387, 1, {1}}, {390, 2, {0x88, 0x13}}, {398, 4, {4, 0, 0x88, 0x13}}};
const Def kDef8[] = {{117, 1, {1}}, {715, 1, {4}}};   // H8H: JUKI editor defaults (§8, §11)

template <size_t N>
void apply_defs(std::vector<uint8_t>& r, size_t base, const Def (&d)[N]) {
    for (const Def& e : d)
        for (int i = 0; i < e.len; ++i) r[base + e.off + i] = e.b[i];
}

std::string opt(const MachineJob& j, const std::string& k, const std::string& def = {}) {
    auto it = j.options.find(k);
    return it == j.options.end() ? def : it->second;
}
double opt_d(const MachineJob& j, const std::string& k, double def) {
    auto it = j.options.find(k);
    if (it == j.options.end() || it->second.empty()) return def;
    char* e = nullptr;
    double v = std::strtod(it->second.c_str(), &e);
    return (e && *e == 0) ? v : def;
}
std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else if (c != ' ') cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

struct LocalPair { const JobFiducial* a; const JobFiducial* b; };

// ---------------------------------------------------------------------------
// H8H as the JUKI editor writes it (HLC_FORMAT.md §11, from the user's
// Single / Matrix / Non-Matrix resaves of one program). Every value below is
// a byte the JUKI editor wrote into a program that did not set it.
// ---------------------------------------------------------------------------
constexpr uint32_t kH8hB11Size = 2427, kH8hB26Size = 2064, kH8hRecs = 3;
constexpr uint32_t kH8hB15Pre = 336, kH8hB17Pre = 112;

// line info entries {offset, size, max, fixed count}
struct H8hLi { uint16_t off; uint32_t size, max, count; };
const H8hLi kH8hLi[] = {
    {62, 40999, 1, 0}, {82, 256, 10000, 0}, {102, 1822, 700, 0}, {122, 28416, 7, 0},
    {142, 3072, 500, 0}, {162, 1052, 700, 0}, {182, 4672, 1, 0}, {202, 24768, 7, 0},
    {222, 744, 400, 0}, {242, 290, 7, 0}, {262, 2427, 7, 3}, {282, 32, 10000, 0},
    {302, 256, 2800, 0}, {322, 256, 280, 0}, {342, 48, 112, 0}, {362, 12830, 70, 0},
    {382, 1024, 14, 0}, {402, 5240, 700, 0}, {422, 36, 700, 0}, {442, 170, 1024, 0},
    {462, 112, 4096, 0}, {482, 3142, 256, 0}, {502, 184, 16, 0}, {522, 0, 16, 0},
    {542, 22, 80000, 0}, {562, 2064, 7, 3}, {582, 22, 10000, 0}};

// Block-table entries of blocks JUKI lists without data (addr 0) — verbatim.
struct H8hEmpty { uint8_t k; uint32_t rs, cnt; };
const H8hEmpty kH8hEmpty[] = {
    {5, 28416, 3}, {7, 1052, 0}, {8, 24768, 0}, {9, 744, 0}, {10, 290, 0}, {13, 256, 1200},
    {14, 256, 120}, {18, 5240, 0}, {19, 36, 0}, {20, 170, 0}, {21, 112, 0}, {22, 3142, 0},
    {23, 184, 0}, {25, 22, 0}, {27, 22, 0}};

void h8h_fill_block(int k, int cfg, uint8_t* p, size_t n) {
    std::memset(p, 0, n);
    auto set = [&](size_t o, uint8_t v) { if (o < n) p[o] = v; };
    switch (k) {
        case 0: {   // optimizer defaults
            const uint8_t a[] = {127, 0, 100, 60, 40, 40, 50, 50, 50};
            for (size_t i = 0; i < sizeof a; ++i) set(i, a[i]);
            for (size_t i = 14; i < 18; ++i) set(i, 1);
            set(2759, 1); set(2788, 100); set(2791, 156); set(4464, 156);
            break;
        }
        case 1:
            for (const H8hLi& e : kH8hLi) {
                if ((size_t)e.off + 20 > n) continue;
                for (int i = 0; i < 4; ++i) {
                    p[e.off + i] = (uint8_t)(e.size >> (8 * i));
                    p[e.off + 4 + i] = (uint8_t)(e.size >> (8 * i));
                    p[e.off + 8 + i] = (uint8_t)(e.max >> (8 * i));
                    p[e.off + 12 + i] = (uint8_t)(e.count >> (8 * i));
                    p[e.off + 16 + i] = (uint8_t)(e.count >> (8 * i));
                }
            }
            if (n >= 62) {
                p[54] = 0x40;
                std::memcpy(p + 55, "C00@C00", 7);
            }
            break;
        case 2:
            set(33, 2);
            for (size_t i = 2596; i < 2624; ++i) set(i, 0xFF);   // BOC mark index per station: none
            for (int i = 0; i < 4; ++i) {
                set(10673 + i, (uint8_t)(370000u >> (8 * i)));
                set(10825 + i, (uint8_t)(5000u >> (8 * i)));
            }
            set(10853, 1);
            break;
        case 11:   // 3 records
            set(0, 128);
            set(kH8hB11Size + 0, 81); set(kH8hB11Size + 2, 6); set(kH8hB11Size + 3, 255);
            set(2 * kH8hB11Size, 96);
            break;
        case 26:   // 3 records; JUKI fills 2 and 3 only in Single PWB
            for (uint32_t r = 0; r < (cfg == 0 ? 3u : 1u); ++r) {
                const size_t b = (size_t)kH8hB26Size * r;
                set(b + 6, 255); set(b + 7, 100); set(b + 8, 100); set(b + 10, 1);
                set(b + 11, 100); set(b + 12, 59); set(b + 13, 12);
            }
            break;
        default: break;
    }
}

// Bytes of blocks 1/2 that the writer derives from the job; the residual patch
// (juki.h8h.raw) never touches them, so edits made in the app win.
bool h8h_protected(int k, size_t o) {
    if (k == 1) {
        if (o >= 21 && o < 54) return true;
        static const int modeled[] = {0, 1, 2, 4, 6, 11};
        for (int i : modeled) {
            const size_t e = 62 + 20 * (size_t)i;
            if (o >= e + 12 && o < e + 20) return true;
        }
        return false;
    }
    if (k == 2)
        return o < 33 || (o >= 34 && o < 38) || (o >= 42 && o < 88) || (o >= 160 && o < 2584) ||
               (o >= 10807 && o < 10811);
    return false;
}

const int kH8hPatchBlocks[] = {0, 1, 2, 11, 15, 17, 26};

} // namespace

std::string h8h_residual(const std::vector<uint8_t>& d) {
    if (d.size() < 38 + 16 * 28) return {};
    const uint32_t a0 = get_u32(d, 38);
    if (a0 != 38 + 16 * 28) return {};
    const uint32_t ba = get_u32(d, 38 + 16 * 2);
    const int cfg = ba && ba + 44 <= d.size() ? (get_u16(d, ba + 42) >> 3) & 3 : 0;
    std::string out;
    static const char* hx = "0123456789abcdef";
    for (int k : kH8hPatchBlocks) {
        const size_t o = 38 + 16 * (size_t)k;
        const uint32_t addr = get_u32(d, o), pre = get_u32(d, o + 4), rs = get_u32(d, o + 8), cnt = get_u32(d, o + 12);
        const size_t n = (size_t)pre + (size_t)rs * cnt;
        if (!addr || addr + n > d.size()) continue;
        std::vector<uint8_t> def(n);
        h8h_fill_block(k, cfg, def.data(), n);
        for (size_t i = 0; i < n;) {
            if (d[addr + i] == def[i] || h8h_protected(k, i)) { ++i; continue; }
            size_t j = i;
            while (j < n && d[addr + j] != def[j] && !h8h_protected(k, j)) ++j;
            if (!out.empty()) out += ';';
            out += std::to_string(k) + "." + std::to_string(i) + "=";
            for (size_t q = i; q < j; ++q) { out += hx[d[addr + q] >> 4]; out += hx[d[addr + q] & 15]; }
            i = j;
        }
    }
    return out;
}

// ===========================================================================
HlcVariant variant_for(HlcFormat f, const MachineJob& job) {
    switch (f) {
        case HlcFormat::H4H: return opt(job, "juki.h4h_hlc5") == "1" ? HlcVariant::H4H_v5
                                                                     : HlcVariant::H4H_v6;
        case HlcFormat::H5H: return HlcVariant::H5H;
        case HlcFormat::H7H: return HlcVariant::H7H;
        case HlcFormat::H8H: return HlcVariant::H8H;
    }
    return HlcVariant::H7H;
}

const char* hlc_extension(HlcVariant v) {
    switch (v) {
        case HlcVariant::H7H: return "H7H";
        case HlcVariant::H5H: return "H5H";
        case HlcVariant::H4H_v6: case HlcVariant::H4H_v5: return "H4H";
        case HlcVariant::E41: return "E41"; case HlcVariant::E42: return "E42";
        case HlcVariant::E43: return "E43"; case HlcVariant::E44: return "E44";
        case HlcVariant::E45: return "E45"; case HlcVariant::E46: return "E46";
        case HlcVariant::E51: return "E51";
        case HlcVariant::H8H: return "H8H";
    }
    return "H7H";
}

bool write_hlc(const MachineJob& job, HlcFormat fmt, std::vector<uint8_t>& out, std::string& err,
               HlcWriteInfo* info) {
    return write_hlc_variant(job, variant_for(fmt, job), out, err, info);
}

const char* circuit_mode_name(CircuitMode m) {
    switch (m) {
        case CircuitMode::Single: return "Single PWB";
        case CircuitMode::Matrix: return "Matrix";
        case CircuitMode::NonMatrix: return "Non-Matrix";
    }
    return "?";
}

// Circuit configuration (HLC_FORMAT.md §5). Single PWB unless the job really is a
// multi-circuit panel; Matrix when the circuits form a full, unrotated, regular
// grid with circuit 1 at its lower-left; Non-Matrix otherwise.
CircuitPlan plan_circuits(const MachineJob& job) {
    CircuitPlan P;
    const std::string want = lower(opt(job, "juki.circuit_mode", "auto"));
    const bool multi = !job.single_board && job.circuits.size() > 1;
    if (!multi) {
        P.mode = CircuitMode::Single;
        if (want == "matrix" || want == "nonmatrix")
            P.notes.push_back("only one circuit: written as Single PWB (juki.circuit_mode=" + want + " ignored)");
        return P;
    }
    if (want == "single") {
        P.mode = CircuitMode::Single;
        P.error = "juki.circuit_mode=single needs one circuit (or 'Treat all images as one board')";
        return P;
    }
    const auto& C = job.circuits;
    auto near = [](double a, double b, double t) { return std::fabs(a - b) <= t; };
    bool grid = want != "nonmatrix";
    for (const auto& c : C) {
        double r = std::fmod(c.arot, 360.0); if (r < 0) r += 360.0;
        if (!(near(r, 0, 1e-6) || near(r, 360, 1e-6))) grid = false;
    }
    std::vector<double> xs, ys;
    auto uniq_add = [&](std::vector<double>& v, double x) {
        for (double e : v) if (near(e, x, 1e-3)) return;
        v.push_back(x);
    };
    if (grid) {
        for (const auto& c : C) { uniq_add(xs, c.ax); uniq_add(ys, c.ay); }
        std::sort(xs.begin(), xs.end()); std::sort(ys.begin(), ys.end());
        const int nx = (int)xs.size(), ny = (int)ys.size();
        grid = nx * ny == (int)C.size() && near(xs[0], 0, 1e-3) && near(ys[0], 0, 1e-3);
        const double px = nx > 1 ? (xs.back() - xs.front()) / (nx - 1) : 0;
        const double py = ny > 1 ? (ys.back() - ys.front()) / (ny - 1) : 0;
        for (int i = 0; grid && i < nx; ++i) grid = near(xs[i], i * px, 1e-3);
        for (int j = 0; grid && j < ny; ++j) grid = near(ys[j], j * py, 1e-3);
        for (int j = 0; grid && j < ny; ++j)
            for (int i = 0; grid && i < nx; ++i) {
                bool f = false;
                for (const auto& c : C) f |= near(c.ax, i * px, 1e-3) && near(c.ay, j * py, 1e-3);
                grid = f;
            }
        if (grid) { P.nx = nx; P.ny = ny; P.px = px; P.py = py; }
    }
    if (want == "matrix" && !grid) {
        P.error = "juki.circuit_mode=matrix: the circuits are not a full unrotated regular grid starting at circuit 1";
        return P;
    }
    P.mode = grid ? CircuitMode::Matrix : CircuitMode::NonMatrix;
    for (const auto& c : C)
        if (c.skip) {
            P.notes.push_back("circuit " + std::to_string(c.index) +
                              " is an x-out: HLC has no circuit-skip field here, its placements are written skipped only in Single PWB mode");
            break;
        }
    return P;
}

// R21 (AUDIT_V20 P1): the ONE board / circuit mark selection (writer, check_job, Export review).
MarkSelection select_marks(const MachineJob& job) {
    MarkSelection R;
    auto find_fid = [&](const std::string& n) -> const JobFiducial* {
        for (const auto& f : job.fiducials) if (f.name == n) return &f;
        return nullptr;
    };
    const CircuitPlan CP = plan_circuits(job);
    const bool multi = CP.error.empty() && CP.mode != CircuitMode::Single;
    const int c1 = multi ? job.circuits[0].index : 0;
    R.fid_circuit = lower(opt(job, "juki.fid_mode", "board")) == "circuit";
    std::vector<const JobFiducial*>& cand = R.marks;
    const std::string gm = opt(job, "juki.global_marks");
    if (!gm.empty()) {   // explicit list: its order is the operator's order
        for (const auto& n : split_csv(gm)) {
            const JobFiducial* f = find_fid(n);
            if (!f) { R.error = "juki.global_marks: unknown fiducial " + n; R.marks.clear(); return R; }
            cand.push_back(f);
        }
        R.explicit_list = true;
    } else if (R.fid_circuit) {
        for (const auto& f : job.fiducials)
            if (f.local && f.owner.empty() && (f.circuit == 1 || f.circuit == c1)) cand.push_back(&f);
        if (cand.empty() && !multi)
            for (const auto& f : job.fiducials) if (!f.local && f.circuit == 0) cand.push_back(&f);
        if (cand.empty() && !job.fiducials.empty())
            R.notes.push_back("circuit fiducial mode: no circuit (image) fiducials on circuit 1");
    } else {
        for (const auto& f : job.fiducials)
            if (!f.local && f.circuit == 0) cand.push_back(&f);
        if (cand.empty() && !multi)   // single PWB: circuit marks share the board frame
            for (const auto& f : job.fiducials)
                if (f.local && f.owner.empty() && f.circuit <= 1) cand.push_back(&f);
    }
    R.candidates = (int)cand.size();
    if (!R.explicit_list) {   // Fiducial 1, 2, 3 ... in number order; unnumbered after them in storage order
        std::stable_sort(cand.begin(), cand.end(), [](const JobFiducial* a, const JobFiducial* b) {
            const int ka = a->number > 0 ? a->number : INT_MAX, kb = b->number > 0 ? b->number : INT_MAX;
            return ka < kb;
        });
        for (size_t i = 1; i < cand.size(); ++i)
            if (cand[i]->number > 0 && cand[i]->number == cand[i - 1]->number) {
                R.notes.push_back("fiducial number " + std::to_string(cand[i]->number) + " is assigned to more than one mark (" +
                                  cand[i - 1]->name + ", " + cand[i]->name + "); list order decides");
                break;
            }
    }
    if (cand.size() > 3) {
        std::string w;
        for (int k = 0; k < 3; ++k) w += (k ? ", " : "") + cand[k]->name;
        R.notes.push_back(std::to_string(cand.size()) + " board marks found; the first 3 by fiducial number are written (" + w + ")");
        cand.resize(3);   // CircuitCAM writes at most 3
    }
    return R;
}

namespace {
H8hResearchHook g_research_hook;
H8hResearchHook& autopart_hook() { static H8hResearchHook h; return h; }   // set at static init (juki_autopart.cpp)
}
void set_h8h_research_hook(H8hResearchHook hook) { g_research_hook = std::move(hook); }
void set_h8h_autopart(H8hResearchHook rules) { autopart_hook() = std::move(rules); }
bool h8h_autopart_available() { return (bool)autopart_hook(); }
bool h8h_research_available() { return (bool)g_research_hook; }

bool write_hlc_variant(const MachineJob& job, HlcVariant v, std::vector<uint8_t>& out,
                       std::string& err, HlcWriteInfo* info) {
    // H8H: fill each part's machine records the way the HLC editor's Database -> Research does, when the
    // application provided the data (component DB + researched programs) and the job did not opt out
    // Parts not in the DB get machine data from a package-library profile (the hook) or the built-in rules
    // (juki_autopart), unless the job opts out (juki.autopart off).
    if (v == HlcVariant::H8H && opt(job, "juki.research") != "off") {
        bool any = false;
        for (const JobPart& p : job.parts) if (!p.h8h_b4.empty()) { any = true; break; }
        const bool rules = opt(job, "juki.autopart") != "off" && (bool)autopart_hook();
        if (!any && (g_research_hook || rules)) {
            MachineJob j2 = job;
            int db = 0, lib = 0, gen = 0;
            std::vector<std::pair<std::string, std::string>> made;
            for (JobPart& p : j2.parts) {
                if (g_research_hook && g_research_hook(p) && !p.h8h_b4.empty()) {
                    if (p.h8h_source == "library") { ++lib; made.push_back({p.part, "library: " + p.h8h_note}); }
                    else { p.h8h_source = "database"; ++db; }
                    continue;
                }
                if (!rules || !autopart_hook()(p) || p.h8h_b4.empty()) continue;
                p.h8h_source = "rules";
                ++gen;
                made.push_back({p.part, "rules: " + p.h8h_note});
            }
            j2.options["juki.research"] = "off";
            const bool ok = write_hlc_variant(j2, v, out, err, info);
            if (ok && info) {
                info->comps_researched = db;
                info->comps_from_library = lib;
                info->comps_from_rules = gen;
                info->generated = std::move(made);
            }
            return ok;
        }
    }
    out.clear();
    HlcWriteInfo local_info;
    HlcWriteInfo& WI = info ? *info : local_info;
    WI = HlcWriteInfo{};
    Layout L;
    if (!make_layout(v, L, err)) return false;
    if (L.h8h) {
        std::string hv = opt(job, "juki.h8h.version");
        if (!hv.empty()) {
            if (hv.size() > 4) { err = "juki.h8h.version must be <= 4 chars"; return false; }
            for (int i = 0; i < 4; ++i) L.ver[i] = i < (int)hv.size() ? hv[i] : 0;
        }
    }

    // ---- placement order (§4.3: optional stable sort by part number) -----
    // ---- circuit configuration (HLC_FORMAT.md §5) ------------------------------
    CircuitPlan CP = plan_circuits(job);
    if (!CP.error.empty()) { err = CP.error; return false; }
    if (CP.mode == CircuitMode::Matrix && !L.wide) {   // narrow board block: matrix offsets unverified
        CP.mode = CircuitMode::NonMatrix;
        CP.notes.push_back("matrix panel written as a Non-Matrix circuit list (narrow format)");
    }
    WI.mode = CP.mode;
    WI.notes = CP.notes;
    const bool multi = CP.mode != CircuitMode::Single;
    const int c1 = multi ? job.circuits[0].index : 0;

    std::vector<const JobPlacement*> pl;
    pl.reserve(job.placements.size());
    for (const auto& p : job.placements)
        if (!multi || p.circuit == c1) pl.push_back(&p);
    if (multi) {
        // Matrix / Non-Matrix programs hold circuit 1 only; the machine repeats it.
        // Every other circuit must be exactly that repetition, else the program lies.
        std::map<std::string, const JobPlacement*> base;
        for (const auto* p : pl) base[p->ref] = p;
        for (const auto& p : job.placements) {
            if (p.circuit == c1) continue;
            const JobCircuit* ck = nullptr;
            for (const auto& c : job.circuits) if (c.index == p.circuit) ck = &c;
            auto it = base.find(p.ref);
            if (!ck || it == base.end()) {
                err = "placement " + p.ref + " (circuit " + std::to_string(p.circuit) +
                      ") has no counterpart in circuit 1; a " + circuit_mode_name(CP.mode) +
                      " program can only repeat circuit 1 (use 'Treat all images as one board' to write a Single PWB)";
                return false;
            }
            const double t = ck->arot * 3.14159265358979323846 / 180.0;
            const double ex = ck->ax + std::cos(t) * it->second->x - std::sin(t) * it->second->y;
            const double ey = ck->ay + std::sin(t) * it->second->x + std::cos(t) * it->second->y;
            if (std::hypot(ex - p.x, ey - p.y) > 0.01) {
                char b[160];
                std::snprintf(b, sizeof b, " is %.3f mm away from the repeated circuit-1 position", std::hypot(ex - p.x, ey - p.y));
                err = "placement " + p.ref + " in circuit " + std::to_string(p.circuit) + b +
                      "; the panel is not a step-and-repeat of circuit 1";
                return false;
            }
        }
    }
    if (opt(job, "juki.sort_by_part") == "1")
        std::stable_sort(pl.begin(), pl.end(),
                         [](const JobPlacement* a, const JobPlacement* b) { return a->part < b->part; });
    const uint32_t N = (uint32_t)pl.size();
    if (N > L.max_place) {
        err = "too many placements for " + std::string(hlc_extension(v)) + " (max " +
              std::to_string(L.max_place) + ")";
        return false;
    }

    // ---- component table: job.parts order, then unseen placement parts ----
    std::vector<const JobPart*> comps;
    std::vector<JobPart> extra;
    std::map<std::string, int> comp_idx;
    for (const auto& p : job.parts)
        if (!p.part.empty() && !comp_idx.count(p.part)) {
            comp_idx[p.part] = (int)comps.size();
            comps.push_back(&p);
        }
    for (const auto* p : pl)
        if (!p->part.empty() && !comp_idx.count(p->part) &&
            std::none_of(extra.begin(), extra.end(), [&](const JobPart& e) { return e.part == p->part; })) {
            JobPart e; e.part = p->part; e.package = p->package; e.description = p->description;
            extra.push_back(e);
        }
    for (const auto& e : extra) {
        comp_idx[e.part] = (int)comps.size();
        comps.push_back(&e);   // extra is not resized after this point
    }
    const uint32_t M = (uint32_t)comps.size();
    if (M > 700) { err = "too many components (max 700)"; return false; }
    std::vector<uint32_t> comp_count(M, 0);
    for (const auto* p : pl) {
        auto it = comp_idx.find(p->part);
        if (it != comp_idx.end()) ++comp_count[(size_t)it->second];
    }
    // ---- part names (P0-03): 20-character name + long name, unique across the program ---------------------
    std::vector<std::string> cname(M), cfull(M);   // written name field / long-name field
    {
        std::vector<std::string> parts(M), pref(M);
        for (uint32_t i = 0; i < M; ++i) { parts[i] = comps[i]->part; pref[i] = comps[i]->h8h_name; }
        const std::vector<MachineName> mn = machine_names(parts, pref);
        for (uint32_t i = 0; i < M; ++i) {
            cname[i] = mn[i].name;
            cfull[i] = mn[i].full;
            int ch = 0;
            legacy_text(comps[i]->part, &ch);
            if (mn[i].how == MachineName::Renamed) WI.renamed.push_back({comps[i]->part, mn[i].name});
            else if (mn[i].how == MachineName::Truncated) WI.truncated.push_back({comps[i]->part, mn[i].name});
            else if (ch) ++WI.text_converted;
        }
        for (auto& r : WI.renamed)
            WI.notes.push_back("part '" + r.first + "' written as '" + r.second + "' (name clash after legacy-text conversion / the 20-character limit)");
    }

    // Imported verbatim geometry (juki.board_x0.., circuit_*, circuit_list) is valid only
    // for the setup it was imported with; after a flip / rotation / origin change the
    // job's own (transformed) geometry is used instead.
    MachineJob jg_store;
    const MachineJob* jgp = &job;
    if (const auto gs = opt(job, "juki.geom_sig"); !gs.empty() && !job.setup_sig.empty() && gs != job.setup_sig) {
        jg_store.options = job.options;
        for (const char* k : {"juki.board_x0", "juki.board_y0", "juki.circuit_w", "juki.circuit_h",
                              "juki.circuit_x", "juki.circuit_y", "juki.circuit_list"})
            jg_store.options.erase(k);
        jgp = &jg_store;
        WI.notes.push_back("setup changed since import: board / circuit geometry recomputed from the project");
    }
    const MachineJob& jg = *jgp;

    // ---- fiducials (§4.6) ---------------------------------------------------
    auto find_fid = [&](const std::string& n) -> const JobFiducial* {
        for (const auto& f : job.fiducials) if (f.name == n) return &f;
        return nullptr;
    };
    // R21 (AUDIT_V20 P1): board / circuit marks come from the shared selector (also used by the Export review
    // and check_job), so the UI shows exactly what is written: Fiducial 1/2/3 first, storage order as tie-break.
    MarkSelection MS = select_marks(job);
    if (!MS.error.empty()) { err = MS.error; return false; }
    const bool fid_circuit = MS.fid_circuit;
    WI.fid_circuit = fid_circuit;
    std::vector<const JobFiducial*> global = MS.marks;
    for (auto& n : MS.notes) WI.notes.push_back(n);
    WI.marks = (int)global.size();

    std::vector<LocalPair> pairs;          // in placement output order
    std::vector<int> place_pair(N, -1);
    for (uint32_t i = 0; i < N; ++i) {
        auto s = opt(job, "juki.local." + pl[i]->ref);
        if (s.empty()) {
            // placement-local fiducials bound to this component (same circuit)
            std::vector<const JobFiducial*> own;
            for (const auto& f : job.fiducials)
                if (f.local && !f.owner.empty() && f.owner == pl[i]->ref && (f.circuit == pl[i]->circuit || f.circuit == 0))
                    own.push_back(&f);
            if (own.size() >= 2) {
                place_pair[i] = (int)pairs.size();
                pairs.push_back({own[0], own[1]});
            }
            continue;
        }
        auto names = split_csv(s);
        if (names.size() != 2) { err = "juki.local." + pl[i]->ref + " needs exactly 2 fiducials"; return false; }
        const JobFiducial* a = find_fid(names[0]);
        const JobFiducial* b = find_fid(names[1]);
        if (!a || !b) { err = "juki.local." + pl[i]->ref + ": unknown fiducial"; return false; }
        place_pair[i] = (int)pairs.size();
        pairs.push_back({a, b});
    }
    const uint32_t sets = (uint32_t)pairs.size();
    const uint32_t local_marks = sets * 2;

    // ---- panel ---------------------------------------------------------------
    const bool panel = CP.mode == CircuitMode::NonMatrix;   // explicit circuit list
    const size_t max_ckt = L.wide ? 200 : 100;
    if (panel && job.circuits.size() > max_ckt) {
        err = "too many circuits (max " + std::to_string(max_ckt) + ")";
        return false;
    }

    // ---- block geometry (§3.2) ----------------------------------------------
    const uint32_t hdr_size = 38 + 16u * (uint32_t)L.entries;
    const uint32_t a_opt = hdr_size;
    const uint32_t a_li = a_opt + L.opt_size;
    const uint32_t a_board = a_li + L.li_size;
    const uint32_t a_place = a_board + L.board_size;
    const uint32_t a_comp = a_place + L.place_size * N;
    // researched component data (blocks 7 / 18 / 19 follow the component table, one record per component)
    bool researched = false;
    if (L.h8h && L.comp_tmpl == 9)
        for (uint32_t i = 0; i < M; ++i) if (comps[i]->h8h_b4.size() == L.comp_size) researched = true;
    const uint32_t a_mark = a_comp + L.comp_size * M;
    // H8H: JUKI drops the mark element when there are no marks at all (§11).
    const bool has_marks = L.h8h ? (!global.empty() || sets > 0) : (L.wide || local_marks > 0);
    // H8H (HLC_FORMAT §6, R13): block 6 holds ONE BOC element PER STATION (element
    // index == line/station index, block 11), linked from board +2596 i16[station].
    // Placement-local mark sets follow the station elements.
    const uint32_t boc_elems = !has_marks ? 0 : (L.h8h ? (global.empty() ? 0 : kH8hRecs) : 1);
    const uint16_t first_set = L.h8h ? (uint16_t)boc_elems : L.first_markset;
    const uint32_t mark_elems = has_marks ? (L.h8h ? boc_elems + sets : sets + 1) : 0;
    const uint32_t mark_bytes = !has_marks ? 0
                              : L.wide ? L.mark_elem * mark_elems
                                       : 1274 + 74 * local_marks;
    const uint32_t a_b7 = a_mark + mark_bytes;                  // H8H researched only
    const uint32_t b7_bytes = researched ? 1052u * M : 0;
    const uint32_t a_b11 = a_b7 + b7_bytes;                     // H8H only
    const uint32_t a_name = L.h8h ? a_b11 + kH8hB11Size * kH8hRecs : a_mark + mark_bytes;
    const uint32_t a_b15 = a_name + L.name_size * N;            // H8H only
    // block 16: ball grids of custom-pattern BGAs, shared by pattern name (Research: library index at block 7 +135)
    std::vector<const std::vector<uint8_t>*> bga_lib;
    std::vector<int> bga_of(M, -2);   // -2: no grid, -1: library full
    if (researched)
        for (uint32_t i = 0; i < M; ++i) {
            const JobPart& c = *comps[i];
            if (c.h8h_bga.size() != 12830 || c.h8h_b7.size() != 1052 || c.h8h_b7[121] != 0xFF) continue;
            if (c.h8h_b7[1] != 2 && c.h8h_b7[1] != 3 && c.h8h_b7[1] != 4) continue;
            const char* nm = (const char*)c.h8h_bga.data() + 4;
            int k = 0;
            for (; k < (int)bga_lib.size(); ++k) {
                const char* o = (const char*)bga_lib[(size_t)k]->data() + 4;
                size_t q = 0;
                while (q < 26 && nm[q] && std::tolower((unsigned char)nm[q]) == std::tolower((unsigned char)o[q])) ++q;
                if (q == 26 || (!nm[q] && !o[q])) break;
            }
            if (k == (int)bga_lib.size()) {
                if (bga_lib.size() >= 70) { bga_of[i] = -1; continue; }   // the line's library is full
                bga_lib.push_back(&c.h8h_bga);
            }
            bga_of[i] = k;
        }
    const uint32_t a_b16 = a_b15 + kH8hB15Pre;
    const uint32_t a_b17 = a_b16 + 12830u * (uint32_t)bga_lib.size();
    const uint32_t a_b18 = a_b17 + kH8hB17Pre;                   // H8H researched only
    const uint32_t a_b19 = a_b18 + (researched ? 5240u * M : 0);
    // blocks 20 / 21 / 22: element groups, elements, extended arrays of element-group parts, renumbered program-wide
    std::vector<uint8_t> el_groups, el_elems, el_arrays;
    std::vector<std::vector<uint8_t>> el_b19(M);
    if (researched)
        for (uint32_t i = 0; i < M; ++i) {
            const JobPart& c = *comps[i];
            if (c.h8h_b19.size() != 36 || !c.h8h_b19[1]) continue;
            std::vector<uint8_t> h = c.h8h_b19, g = c.h8h_groups, e = c.h8h_elements, a = c.h8h_arrays;
            if (el_groups.size() / 170 + g.size() / 170 > 1024 || el_elems.size() / 112 + e.size() / 112 > 4096 ||
                el_arrays.size() / 3142 + a.size() / 3142 > 256)
            {   // the line's element lists are full: the part keeps no element data
                el_b19[i].assign(36, 0);
                el_b19[i][20] = el_b19[i][21] = el_b19[i][24] = el_b19[i][25] = 0xFF;
                continue;
            }
            h8h_relocate_elements(h, g, e, a, (uint16_t)(el_groups.size() / 170), (uint16_t)(el_elems.size() / 112),
                                  (uint16_t)(el_arrays.size() / 3142));
            el_groups.insert(el_groups.end(), g.begin(), g.end());
            el_elems.insert(el_elems.end(), e.begin(), e.end());
            el_arrays.insert(el_arrays.end(), a.begin(), a.end());
            el_b19[i] = std::move(h);
        }
    const uint32_t a_b20 = a_b19 + (researched ? 36u * M : 0);
    const uint32_t a_b21 = a_b20 + (uint32_t)el_groups.size();
    const uint32_t a_b22 = a_b21 + (uint32_t)el_elems.size();
    const uint32_t a_b26 = a_b22 + (uint32_t)el_arrays.size();
    const uint32_t total = L.h8h ? a_b26 + kH8hB26Size * kH8hRecs : a_name + L.name_size * N;

    out.assign(total, 0);

    // ---- header (§3.1) -----------------------------------------------------
    put_str(out, 0, 32, job.program_name);
    put_u8(out, 32, L.station);
    put_u8(out, 33, L.flag);
    for (int i = 0; i < 4; ++i) put_u8(out, 34 + i, (uint8_t)L.ver[i]);
    auto entry = [&](int idx, uint32_t addr, uint32_t pre, uint32_t rs, uint32_t cnt) {
        size_t o = 38 + 16 * (size_t)idx;
        put_u32(out, o, addr); put_u32(out, o + 4, pre);
        put_u32(out, o + 8, rs); put_u32(out, o + 12, cnt);
    };
    entry(0, a_opt, 0, L.opt_size, 1);
    entry(1, a_li, 0, L.li_size, 1);
    entry(2, a_board, 0, L.board_size, 1);
    entry(3, a_place, 0, L.place_size, N);
    entry(4, a_comp, 0, L.comp_size, M);
    if (L.h8h) {
        entry(6, a_mark, 0, L.mark_elem, mark_elems);   // listed even when empty
        for (const H8hEmpty& e : kH8hEmpty) entry(e.k, 0, 0, e.rs, e.cnt);
        if (researched) {
            entry(7, a_b7, 0, 1052, M);
            entry(18, a_b18, 0, 5240, M);
            entry(19, a_b19, 0, 36, M);
            if (!el_groups.empty()) entry(20, a_b20, 0, 170, (uint32_t)(el_groups.size() / 170));
            if (!el_elems.empty()) entry(21, a_b21, 0, 112, (uint32_t)(el_elems.size() / 112));
            if (!el_arrays.empty()) entry(22, a_b22, 0, 3142, (uint32_t)(el_arrays.size() / 3142));
        }
        entry(11, a_b11, 0, kH8hB11Size, kH8hRecs);
        entry(15, a_b15, kH8hB15Pre, 48, 0);
        entry(16, a_b16, 0, 12830, (uint32_t)bga_lib.size());
        entry(17, a_b17, kH8hB17Pre, 1024, 0);
        entry(26, a_b26, 0, kH8hB26Size, kH8hRecs);
        const int cfg = (int)CP.mode;
        h8h_fill_block(0, cfg, &out[a_opt], L.opt_size);
        h8h_fill_block(1, cfg, &out[a_li], L.li_size);
        h8h_fill_block(2, cfg, &out[a_li + L.li_size], L.board_size);
        h8h_fill_block(11, cfg, &out[a_b11], kH8hB11Size * kH8hRecs);
        h8h_fill_block(26, cfg, &out[a_b26], kH8hB26Size * kH8hRecs);
    } else if (has_marks) {
        if (L.wide) entry(6, a_mark, 0, L.mark_elem, mark_elems);
        else entry(6, a_mark, 1274, 74, local_marks);
    }
    entry(12, a_name, 0, L.name_size, N);

    // ---- block 0: optimizer (§4.0) -----------------------------------------
    if (L.opt_marker) put_u8(out, a_opt + 39, 1);

    // ---- block 1: line info (§4.1) -----------------------------------------
    {
        const size_t b = a_li;
        // +21 comment: the operator's text only (never app-generated text, P1-06)
        std::string cmt = opt(job, "juki.comment");
        // the old app default ("Generated by PCB Plotter", pre-v13 projects) is dropped;
        // a program imported with its own line name keeps its comment byte-for-byte
        if (cmt.rfind("Generated by PCB Plotter", 0) == 0 && opt(job, "juki.line_name").empty()) cmt.clear();
        put_str(out, b + 21, 33, cmt);
        put_u8(out, b + 54, L.flag);
        for (int i = 0; i < 7; ++i) put_u8(out, b + 55 + i, (uint8_t)L.li_ver[i]);
        put_u32(out, b + 62, L.board_size);
        put_u32(out, b + 66, L.board_size);
        put_u32(out, b + 70, 1); put_u32(out, b + 74, 1); put_u32(out, b + 78, 1);
        put_u32(out, b + 82, L.place_size); put_u32(out, b + 86, L.place_size);
        put_u32(out, b + 90, L.max_place);
        put_u32(out, b + 94, N); put_u32(out, b + 98, N);
        put_u32(out, b + 102, L.comp_size); put_u32(out, b + 106, L.comp_size);
        put_u32(out, b + 110, 700);
        put_u32(out, b + 114, M);
        put_u32(out, b + 118, researched ? M : 0);
        if (researched) {
            // line-info rows of blocks 7 / 18 / 19: records in use (non-empty), as the editor counts them
            auto used = [&](auto field, size_t size) {
                uint32_t n = 0;
                for (uint32_t i = 0; i < M; ++i) {
                    const std::vector<uint8_t>& v = comps[i]->*field;
                    if (v.size() == size && std::any_of(v.begin(), v.end(), [](uint8_t x) { return x != 0; })) ++n;
                }
                return n;
            };
            const uint32_t n7 = used(&JobPart::h8h_b7, 1052), n18 = used(&JobPart::h8h_b18, 5240);
            uint32_t n19 = 0;   // block 19 counts the parts with element data
            for (uint32_t i = 0; i < M; ++i) n19 += !el_b19[i].empty() && el_b19[i][1];
            // element lists: in use / complete
            auto el_row = [&](uint32_t row, const std::vector<uint8_t>& v, size_t rs) {
                uint32_t n = 0, done = 0;
                for (size_t o = 0; o + rs <= v.size(); o += rs) { ++n; done += v[o] == 2; }
                put_u32(out, b + row + 12, n); put_u32(out, b + row + 16, done);
            };
            el_row(442, el_groups, 170);
            el_row(462, el_elems, 112);
            el_row(482, el_arrays, 3142);
            const uint32_t n16 = (uint32_t)bga_lib.size();
            put_u32(out, b + 362 + 12, n16); put_u32(out, b + 362 + 16, n16);
            for (auto [row, rs, n] : {std::tuple<uint32_t, uint32_t, uint32_t>{162, 1052, n7}, {402, 5240, n18}, {422, 36, n19}}) {
                put_u32(out, b + row, rs); put_u32(out, b + row + 4, rs); put_u32(out, b + row + 8, 700);
                put_u32(out, b + row + 12, n); put_u32(out, b + row + 16, n);
            }
        }
        put_u32(out, b + 282, L.name_size); put_u32(out, b + 286, L.name_size);
        put_u32(out, b + 290, L.max_place);
        put_u32(out, b + 294, N); put_u32(out, b + 298, N);
        if (L.h8h) {
            // H8H: table pre-filled with the JUKI editor's entries (h8h_fill_block, §11);
            // only the job-derived counts here. +0 line name = the editor's line setting.
            // +0 = the editor's line name / user (JUKI resave: the company). Project
            // Info > Originator unless the program was imported with its own line name.
            const std::string ln = opt(job, "juki.line_name");
            put_str(out, b + 0, 21, ln.empty() ? job.originator : ln);
            put_u32(out, b + 74, 1); put_u32(out, b + 78, 1);   // board count
            put_u32(out, b + 194, 1); put_u32(out, b + 198, 1); // optimizer count
            put_u32(out, b + 154, mark_elems); put_u32(out, b + 158, mark_elems);
        }
    }

    // ---- block 2: board (§4.2) ----------------------------------------------
    {
        const size_t b = a_board;
        put_str(out, b + 0, 33, job.pcb_id.empty() ? job.program_name : job.pcb_id);   // PWB ID
        uint32_t ts = 0;
        auto ts_s = opt(job, "juki.timestamp");
        ts = ts_s.empty() ? (uint32_t)std::time(nullptr) : (uint32_t)std::strtoul(ts_s.c_str(), nullptr, 10);
        put_u32(out, b + 34, ts);
        // +38 last optimized: the JUKI editor writes 0 for a program it did not optimize (§11)
        if (!L.h8h) put_u32(out, b + 38, ts);
        const bool rl = lower(opt(job, "juki.transport", "LR")) == "rl";
        const bool back = lower(opt(job, "juki.fixed_rail", "front")) == "back";
        // +42 (MemoryConversion PwbDataConvert): bit0 fixed rail back, bit1 R->L,
        // bit2 reference method (CircuitCAM: 1), bits3-4 circuit configuration
        // (0 single, 1 matrix, 2 non-matrix), bits5-6 BOC mark type (1 PWB, 2 circuit).
        uint16_t mode = 0x04 | (uint16_t)((int)CP.mode << 3);
        if (!global.empty()) mode |= fid_circuit ? 0x40 : 0x20;
        if (rl) mode |= 0x02;
        if (back) mode |= 0x01;
        put_u16(out, b + 42, mode);
        WI.board_mode = mode;
        const double x0 = opt_d(jg, "juki.board_x0", job.board_x0);
        const double y0 = opt_d(jg, "juki.board_y0", job.board_y0);
        if (job.board_w > 0 && job.board_h > 0) {
            put_i32(out, b + 44, to_raw_len(job.board_w));
            put_i32(out, b + 48, to_raw_len(job.board_h));
            // +52/+56 PWB reference position: 0 as CircuitCAM writes it
            // +60/+64 PWB layout offset = the origin corner in placement coordinates
            put_i32(out, b + 60, to_raw_len(rl ? x0 : x0 + job.board_w));
            put_i32(out, b + 64, to_raw_len(back ? y0 + job.board_h : y0));
            put_u16(out, b + 68, 0x3F);   // set flags: W, L, ref X/Y, layout X/Y
        }
        // Circuit block (+70..+87): outline + layout offset (= the circuit's
        // lower-left corner in placement coordinates), set flags 15. Always present.
        double cw, ch, cx, cy;
        if (!multi) {
            cw = job.board_w; ch = job.board_h; cx = x0; cy = y0;
        } else if (job.has_circuit) {
            cw = job.circuit_w; ch = job.circuit_h; cx = job.circuit_x; cy = job.circuit_y;
        } else {
            double mnx = 1e30, mny = 1e30, mxx = -1e30, mxy = -1e30;
            for (const auto* q : pl) {
                mnx = std::min(mnx, q->x); mny = std::min(mny, q->y);
                mxx = std::max(mxx, q->x); mxy = std::max(mxy, q->y);
            }
            if (pl.empty()) mnx = mny = mxx = mxy = 0;
            cx = mnx - 1; cy = mny - 1; cw = mxx - mnx + 2; ch = mxy - mny + 2;
            WI.notes.push_back("no circuit frame: circuit outline taken from the circuit-1 placements + 1 mm");
        }
        cw = opt_d(jg, "juki.circuit_w", cw); ch = opt_d(jg, "juki.circuit_h", ch);
        cx = opt_d(jg, "juki.circuit_x", cx); cy = opt_d(jg, "juki.circuit_y", cy);
        if (cw > 0 && ch > 0) {
            put_i32(out, b + 70, to_raw_len(cw));
            put_i32(out, b + 74, to_raw_len(ch));
            put_i32(out, b + 78, to_raw_len(cx));
            put_i32(out, b + 82, to_raw_len(cy));
            put_u16(out, b + 86, 15);
        }
        WI.ckt_x = cx; WI.ckt_y = cy; WI.ckt_w = cw; WI.ckt_h = ch;
        if (CP.mode == CircuitMode::Matrix && L.wide) {
            put_u16(out, b + 160, (uint16_t)CP.nx);
            put_u16(out, b + 162, (uint16_t)CP.ny);
            // +164/+168 start position 0: circuit-1 placements stay absolute
            put_i32(out, b + 172, to_raw_len(CP.px));
            put_i32(out, b + 176, to_raw_len(CP.py));
            put_u16(out, b + 180, 0x3F);   // set flags: div X/Y, start X/Y, pitch X/Y
        }
        const size_t cnt_off = L.wide ? 182 : 180;
        const size_t arr_off = cnt_off + 2;
        if (panel || (CP.mode == CircuitMode::Matrix && !L.wide)) {
            // juki.circuit_list "x,y,deg;..." (mm, from an imported program) replaces the
            // computed list when it has the same number of circuits.
            struct CE { double x, y, a; };
            std::vector<CE> list;
            for (const auto& c : job.circuits) list.push_back({c.ax, c.ay, c.arot});
            if (auto cl = opt(jg, "juki.circuit_list"); !cl.empty()) {
                std::vector<CE> raw;
                std::stringstream ss(cl);
                std::string item;
                while (std::getline(ss, item, ';')) {
                    CE e{};
                    if (std::sscanf(item.c_str(), "%lf,%lf,%lf", &e.x, &e.y, &e.a) == 3) raw.push_back(e);
                }
                if (raw.size() == list.size()) list = raw;
                else WI.notes.push_back("juki.circuit_list ignored: " + std::to_string(raw.size()) +
                                        " entries for " + std::to_string(list.size()) + " circuits");
            }
            put_u8(out, b + cnt_off, (uint8_t)list.size());
            for (size_t i = 0; i < list.size(); ++i) {
                size_t e = b + arr_off + 12 * i;
                const int16_t ang = to_raw_ang(list[i].a);
                put_i32(out, e, to_raw_len(list[i].x));
                put_i32(out, e + 4, to_raw_len(list[i].y));
                put_i16(out, e + 8, ang);
                // set flags b0 X, b1 Y, b2 angle; H8H: JUKI adds b3 when the angle is not 0 (§11)
                put_u16(out, e + 10, (uint16_t)(7 | (L.h8h && ang != 0 ? 8 : 0)));
            }
        }
        const uint32_t thick = (uint32_t)to_raw_len(opt_d(job, "juki.board_t", job.board_t > 0 ? job.board_t : 1.6));
        if (L.wide) {
            put_u32(out, b + 10673, 370000);
            put_u32(out, b + 10807, thick);
            if (L.code <= 1) put_u32(out, b + 10825, 5000);
        } else {
            for (size_t k = 0; k < global.size(); ++k) {
                size_t e = b + 1394 + 10 * k;
                put_i32(out, e, to_raw_len(global[k]->x));
                put_i32(out, e + 4, to_raw_len(global[k]->y));
                put_u16(out, e + 8, 6);
            }
            put_u8(out, b + 1424, 90);
            put_u8(out, b + 1425, (uint8_t)global.size());
            for (size_t k = 0; k < global.size(); ++k)
                put_u16(out, b + 1426 + 2 * k,
                        (uint16_t)std::clamp<long>(std::lround(global[k]->diameter * 10000.0), 0, 65535));
            put_u32(out, b + L.board_size - 4, thick);
        }
    }

    // ---- block 3: placements (§4.3) ----------------------------------------
    for (uint32_t i = 0; i < N; ++i) {
        const JobPlacement& p = *pl[i];
        const size_t r = a_place + (size_t)L.place_size * i;
        put_str(out, r + 0, 6, p.ref);
        put_u8(out, r + 6, 2);
        put_i32(out, r + 8, to_raw_len(p.x));
        put_i32(out, r + 12, to_raw_len(p.y));
        put_i16(out, r + 16, to_raw_ang(p.rot));
        put_u8(out, r + 18, 4);
        // +27 (PlacementDataConvert): bit0 skip, bit3 area (local) fiducial, bit6
        // "Placement" attribute (place, not adhesive-only) — H7H/H8H only.
        uint16_t flags = (uint16_t)((p.skip ? 1 : 0) + (place_pair[i] >= 0 ? 8 : 0) + (L.place_ext ? 0x40 : 0));
        put_u16(out, r + 27, flags);
        const size_t s = r + (L.place_ext ? 31 : 29);   // +29 is u16 0 in H7H/H8H
        auto it = comp_idx.find(p.part);
        const bool linked = it != comp_idx.end();
        // status (PlacementDataConvert): bit0|bit1 component set, bit2 X, bit3 Y,
        // bit4 angle set, bit5 RangeOver = outside the circuit outline.
        uint16_t st = (uint16_t)(0x1C | (linked ? 0x03 : 0));
        const bool inside = WI.ckt_w <= 0 ||
            (p.x >= WI.ckt_x - 1e-4 && p.x <= WI.ckt_x + WI.ckt_w + 1e-4 &&
             p.y >= WI.ckt_y - 1e-4 && p.y <= WI.ckt_y + WI.ckt_h + 1e-4);
        if (!inside) { st |= 0x20; ++WI.outside; }
        put_u16(out, s, st);
        put_u16(out, s + 2, linked ? (uint16_t)it->second : 0);
        put_u16(out, s + 4, place_pair[i] >= 0 ? (uint16_t)(first_set + place_pair[i]) : 0);
    }

    // ---- blocks 20 / 21 / 22: element lists ---------------------------------
    if (!el_groups.empty()) std::memcpy(&out[a_b20], el_groups.data(), el_groups.size());
    if (!el_elems.empty()) std::memcpy(&out[a_b21], el_elems.data(), el_elems.size());
    if (!el_arrays.empty()) std::memcpy(&out[a_b22], el_arrays.data(), el_arrays.size());

    // ---- block 16: BGA ball grids -------------------------------------------
    for (size_t k = 0; k < bga_lib.size(); ++k) std::memcpy(&out[a_b16 + 12830 * k], bga_lib[k]->data(), 12830);

    // ---- block 4: components (§4.4) ----------------------------------------
    for (uint32_t i = 0; i < M; ++i) {
        const JobPart& c = *comps[i];
        const size_t r = a_comp + (size_t)L.comp_size * i;
        const bool rs = researched && c.h8h_b4.size() == L.comp_size;
        auto vis_of = [](const JobPart& p) {
            return p.h8h_b7.size() == 1052 && std::any_of(p.h8h_b7.begin(), p.h8h_b7.end(), [](uint8_t b) { return b != 0; });
        };
        if (rs) {
            auto put_rec = [&](const std::vector<uint8_t>& v, uint32_t base, uint32_t size) {
                if (v.size() == size) std::memcpy(&out[base + (size_t)size * i], v.data(), size);
            };
            put_rec(c.h8h_b7, a_b7, 1052);
            put_rec(c.h8h_b18, a_b18, 5240);
            put_rec(el_b19[i].empty() ? c.h8h_b19 : el_b19[i], a_b19, 36);
            // own index in the vision cross-references (only when the part has vision data)
            if (vis_of(c))
                for (size_t o : {291u, 391u, 491u, 591u}) put_u16(out, a_b7 + 1052 * (size_t)i + o, (uint16_t)i);
            if (bga_of[i] != -2) {
                const size_t r7 = a_b7 + 1052 * (size_t)i;
                if (bga_of[i] >= 0) out[r7 + 135] = (uint8_t)bga_of[i];
                else { std::memset(&out[r7 + 121], 0, 16); out[r7] = 1; }   // no room in the library: no grid
            }
        }
        switch (L.comp_tmpl) {
            case 9:
                if (L.h8h) apply_defs(out, r, kDef8);
                else apply_defs(out, r, kDef9);
                break;
            case 7: apply_defs(out, r, kDef7); break;
            case 6: apply_defs(out, r, kDef6); break;
            default: apply_defs(out, r, kDef5); break;
        }
        put_str(out, r + 0, 21, cname[i]);
        put_str(out, r + 47, 31, c.description);
        { int ch = 0; legacy_text(c.description, &ch); if (ch) ++WI.text_converted; }
        if (L.comp_tmpl == 9 && !rs) {
            // Outline (ComponentsDataConvert): +81 width (X at 0 deg), +85 length (Y),
            // +89 height, 1e-4 mm; set flags at +113 bit1/bit2/bit3.
            uint16_t sf = 0;
            if (c.length > 0) { put_i32(out, r + 81, to_raw_len(c.length)); sf |= 0x02; }
            if (c.width > 0) { put_i32(out, r + 85, to_raw_len(c.width)); sf |= 0x04; }
            if (c.height > 0) { put_i32(out, r + 89, to_raw_len(c.height)); sf |= 0x08; }
            if (sf) put_u16(out, r + 113, sf);
            else ++WI.comps_without_size;
            if (sf && c.size_src == "pads") ++WI.comps_size_from_pads;   // R24a: legs included (no body outline)
            if (!(c.height > 0)) ++WI.comps_without_height;             // R24a: never fabricated
        }
        if (L.h8h && !rs) {
            // R18a (HLC_FORMAT.md §8.1): +80 u8 feeder kind (only when the part number has a packaging), supply
            // angle i16 in 1/80 deg at +180 for tape (+176/+178 = tape width / pitch codes), at +178 otherwise.
            if (c.feeder_kind > 0) put_u8(out, r + 80, (uint8_t)c.feeder_kind);
            const long raw = std::lround(c.supply_angle * 80.0);   // as stored (an imported -90 stays -90)
            if (raw) put_u16(out, r + (c.feeder_kind == 1 ? 180 : 178), (uint16_t)(int16_t)std::clamp<long>(raw, -32768, 32767));
            // R24b (HLC_FORMAT.md §8.2, confirmed on real machine programs vs the parts DB): +78 u16 component class
            // code; tape only: +176 u16 tape code (width + paper/emboss), +178 u16 feed-pitch code.
            if (c.type_code > 0) put_u16(out, r + 78, (uint16_t)c.type_code);
            if (c.feeder_kind == 1 && c.tape_code > 0) put_u16(out, r + 176, (uint16_t)c.tape_code);
            if (c.feeder_kind == 1 && c.pitch_code > 0) put_u16(out, r + 178, (uint16_t)c.pitch_code);
        }
        if (L.comp_tmpl == 9) {
            const size_t ln = L.h8h ? 1143 : 1115;   // H8H offset inferred (§5.2)
            put_u8(out, r + ln, cfull[i].size() <= 20 ? 0 : cfull[i].size() <= 60 ? 1 : 2);
            put_str(out, r + ln + 1, 61, cfull[i]);
            put_str(out, r + ln + 62, 128, c.description);
        }
        // researched: the database part of the record over the program's own record (as the editor's Research)
        if (rs) {
            research_merge_b4(&out[r], c.h8h_b4.data());
            if (vis_of(c)) put_u16(out, r + 338, (uint16_t)i);
            else put_u16(out, r + 338, 0);
        }
        put_u16(out, r + (L.comp_tmpl == 5 ? 110 : 118), (uint16_t)comp_count[i]);
    }

    // ---- block 6: marks (§4.6) ---------------------------------------------
    if (has_marks && L.wide) {
        auto put_mark = [&](size_t e, const JobFiducial& f) {
            put_i32(out, e, to_raw_len(f.x));
            put_i32(out, e + 4, to_raw_len(f.y));
            // +8 wDiameter (1e-4 mm), +10 u16 3 (TTP corpus: always 3)
            put_u16(out, e + 8, (uint16_t)std::clamp<long>(std::lround(f.diameter * 10000.0), 0, 65535));
            put_u8(out, e + 10, 3);
            // H7H (CircuitCAM): name at +14. H8H: +12 wTemplateLink / +14 wMarkId
            // (TTP JUKI_BOC_FORMAT §3) -- leave 0, a name there reads as a library id.
            if (L.mark_entry == 128 && !L.h8h) put_str(out, e + 14, 16, f.name);
        };
        const uint32_t nboc = L.h8h ? boc_elems : 1;
        for (uint32_t st = 0; st < nboc; ++st) {
            const size_t e0 = a_mark + (size_t)L.mark_elem * st;
            char nm[8];
            std::snprintf(nm, sizeof nm, "M%03u", (unsigned)(st + 1));
            put_u8(out, e0, 1); put_u8(out, e0 + 1, 1);       // BOCMARK, untaught
            put_str(out, e0 + 2, 16, nm);
            put_u16(out, e0 + 18, (uint16_t)global.size());   // wEffectiveCount
            // OCC group 0 = fid[0..3]; the machine teaches the image data
            for (size_t k = 0; k < global.size(); ++k) put_mark(e0 + 40 + L.mark_entry * k, *global[k]);
            if (L.h8h && !global.empty()) put_u16(out, a_board + 2596 + 2 * st, (uint16_t)st);
        }
        for (uint32_t s = 0; s < sets; ++s) {
            size_t e = a_mark + (size_t)L.mark_elem * (first_set + s);
            char nm[8];
            std::snprintf(nm, sizeof nm, "M%03u", (unsigned)(first_set + s + 1));
            put_u8(out, e, 2); put_u8(out, e + 1, 1);
            put_str(out, e + 2, 16, nm);
            put_u8(out, e + 18, 2);
            put_mark(e + 40, *pairs[s].a);
            put_mark(e + 40 + L.mark_entry, *pairs[s].b);
        }
    } else if (has_marks) {
        put_u8(out, a_mark, 1);
        for (uint32_t s = 0; s < sets; ++s)
            for (int k = 0; k < 2; ++k) {
                const JobFiducial& f = k ? *pairs[s].b : *pairs[s].a;
                size_t e = a_mark + 1274 + 74 * (2 * s + k);
                put_i32(out, e, to_raw_len(f.x));
                put_i32(out, e + 4, to_raw_len(f.y));
                put_u16(out, e + 8, 7);
            }
    }

    // ---- block 12: names (§4.7) --------------------------------------------
    for (uint32_t i = 0; i < N; ++i) {
        const size_t r = a_name + (size_t)L.name_size * i;
        put_u8(out, r, 2);
        put_str(out, r + 1, 11, pl[i]->ref);
        if (L.name_size == 32) put_str(out, r + 12, 20, pl[i]->ref);
    }

    // ---- H8H residual patch (juki.h8h.raw, §11): bytes of an imported program
    // that the job does not model. Never touches job-derived fields.
    if (L.h8h) {
        if (auto raw = opt(job, "juki.h8h.raw"); !raw.empty()) {
            auto blk = [&](int k, uint32_t& a, uint32_t& n) {
                const size_t o = 38 + 16 * (size_t)k;
                a = get_u32(out, o);
                n = get_u32(out, o + 4) + get_u32(out, o + 8) * get_u32(out, o + 12);
            };
            std::stringstream ss(raw);
            std::string item;
            int bad = 0;
            while (std::getline(ss, item, ';')) {
                int k = -1; unsigned long off = 0;
                const size_t eq = item.find('=');
                if (eq == std::string::npos || std::sscanf(item.c_str(), "%d.%lu", &k, &off) != 2 ||
                    std::find(std::begin(kH8hPatchBlocks), std::end(kH8hPatchBlocks), k) == std::end(kH8hPatchBlocks)) { ++bad; continue; }
                uint32_t a = 0, n = 0;
                blk(k, a, n);
                const std::string hex = item.substr(eq + 1);
                for (size_t q = 0; q + 1 < hex.size(); q += 2) {
                    const size_t o = off + q / 2;
                    if (!a || o >= n) { ++bad; break; }
                    if (h8h_protected(k, o)) continue;
                    out[a + o] = (uint8_t)std::strtoul(hex.substr(q, 2).c_str(), nullptr, 16);
                }
            }
            if (bad) WI.notes.push_back("juki.h8h.raw: " + std::to_string(bad) + " entries did not fit this program and were skipped");
        }
    }
    return true;
}

// ===========================================================================
// Reader
// ===========================================================================
bool parse_hlc(const std::vector<uint8_t>& d, HlcParsed& o, std::string& err) {
    o = HlcParsed{};
    if (d.size() < 38 + 16 * 8) { err = "file too small"; return false; }
    o.name = get_str(d, 0, 32);
    o.station = d[32];
    o.flag = d[33];
    o.version = get_str(d, 34, 4);
    const uint32_t a0 = get_u32(d, 38);
    if (a0 < 38 + 16 * 8 || (a0 - 38) % 16 != 0 || a0 > d.size()) { err = "bad block table"; return false; }
    const size_t n = (a0 - 38) / 16;
    for (size_t i = 0; i < n; ++i) {
        size_t off = 38 + 16 * i;
        o.blocks.push_back({get_u32(d, off), get_u32(d, off + 4), get_u32(d, off + 8), get_u32(d, off + 12)});
    }
    for (size_t i = 0; i < n; ++i) {
        const auto& b = o.blocks[i];
        if (!b.addr) continue;
        uint64_t end = (uint64_t)b.addr + b.prefix + (uint64_t)b.recsize * b.count;
        if (end > d.size()) { err = "block " + std::to_string(i) + " past EOF"; return false; }
    }
    if (n < 13) { err = "too few blocks"; return false; }
    const auto& bl = o.blocks[1];
    const auto& bb = o.blocks[2];
    const auto& bp = o.blocks[3];
    const auto& bc = o.blocks[4];
    const auto& bm = o.blocks[6];
    const auto& bn = o.blocks[12];
    const bool narrow = bp.recsize == 71;
    const bool ext = bp.recsize == 256;
    if (bl.addr && bl.recsize >= 302) {
        o.li_version = std::string((const char*)&d[bl.addr + 55], 7);
        o.li_place_count = get_u32(d, bl.addr + 94);
        o.li_comp_count = get_u32(d, bl.addr + 114);
        o.li_place_count2 = get_u32(d, bl.addr + 294);
    }
    if (bb.addr && bb.recsize >= 2000) {
        const size_t b = bb.addr;
        o.board_name = get_str(d, b, 33);
        o.board_mode = get_u16(d, b + 42);
        o.pcb_w = get_i32(d, b + 44); o.pcb_h = get_i32(d, b + 48);
        o.layout_x = get_i32(d, b + 60); o.layout_y = get_i32(d, b + 64);
        o.ckt_w = get_i32(d, b + 70); o.ckt_h = get_i32(d, b + 74);
        o.ckt_x = get_i32(d, b + 78); o.ckt_y = get_i32(d, b + 82);
        if (!narrow) {
            o.div_x = (int16_t)get_u16(d, b + 160); o.div_y = (int16_t)get_u16(d, b + 162);
            o.first_x = get_i32(d, b + 164); o.first_y = get_i32(d, b + 168);
            o.pitch_x = get_i32(d, b + 172); o.pitch_y = get_i32(d, b + 176);
        }
        const size_t co = narrow ? 180 : 182;
        o.circuit_count = d[b + co];
        for (size_t i = 0; i < o.circuit_count && i < (narrow ? 100u : 200u); ++i) {
            size_t e = b + co + 2 + 12 * i;
            o.circuits.push_back({get_i32(d, e), get_i32(d, e + 4)});
            o.circuit_angles.push_back((int16_t)get_u16(d, e + 8));
        }
    }
    for (uint32_t i = 0; bp.addr && i < bp.count; ++i) {
        size_t r = bp.addr + (size_t)bp.recsize * i;
        HlcPlacementRec p;
        p.ref = get_str(d, r, 6);
        p.x = get_i32(d, r + 8); p.y = get_i32(d, r + 12);
        p.angle = (int16_t)get_u16(d, r + 16);
        p.flags = get_u16(d, r + 27);
        size_t s = r + (ext ? 31 : 29);
        p.status = get_u16(d, s); p.comp = get_u16(d, s + 2); p.mark_set = get_u16(d, s + 4);
        o.placements.push_back(p);
    }
    for (uint32_t i = 0; bc.addr && i < bc.count; ++i) {
        size_t r = bc.addr + (size_t)bc.recsize * i;
        o.components.push_back(get_str(d, r, 21));
        o.comp_desc.push_back(get_str(d, r + 47, 31));
        o.comp_counts.push_back(get_u16(d, r + (bc.recsize == 504 ? 110 : 118)));
        const bool big = bc.recsize >= 1792;
        o.comp_w.push_back(big ? get_i32(d, r + 81) : 0);
        o.comp_l.push_back(big ? get_i32(d, r + 85) : 0);
        o.comp_h.push_back(big ? get_i32(d, r + 89) : 0);
        if (bc.recsize == 1822) {   // R18a H8H feeder kind + supply angle (§8.1)
            const uint8_t k = d[r + 80];
            o.comp_kind.push_back(k);
            o.comp_supply.push_back((int16_t)get_u16(d, r + (k == 1 ? 180 : 178)) / 80.0);
            o.comp_type.push_back(get_u16(d, r + 78));                    // R24b
            o.comp_tape.push_back(k == 1 ? get_u16(d, r + 176) : 0);
            o.comp_pitch.push_back(k == 1 ? get_u16(d, r + 178) : 0);
        } else { o.comp_kind.push_back(0); o.comp_supply.push_back(0); o.comp_type.push_back(0); o.comp_tape.push_back(0); o.comp_pitch.push_back(0); }
    }
    for (uint32_t i = 0; bn.addr && i < bn.count; ++i)
        o.names.push_back(get_str(d, bn.addr + (size_t)bn.recsize * i + 1, 11));
    if (bm.addr) {
        if (bm.prefix == 1274 && bm.recsize == 74) {
            for (uint32_t i = 0; i < bm.count; ++i) {
                size_t e = bm.addr + 1274 + 74 * i;
                o.local_marks.push_back({get_i32(d, e), get_i32(d, e + 4)});
            }
        } else {
            const size_t es = bm.recsize == 3072 ? 128 : 124;
            for (uint32_t i = 0; i < bm.count; ++i) {
                size_t e = bm.addr + (size_t)bm.recsize * i;
                HlcMarkSet ms;
                ms.type = d[e]; ms.name = get_str(d, e + 2, 16); ms.count = d[e + 18];
                for (size_t k = 0; k < ms.count && k < 3; ++k)
                    ms.marks.push_back({get_i32(d, e + 40 + es * k), get_i32(d, e + 44 + es * k)});
                o.mark_sets.push_back(ms);
            }
        }
    }
    return true;
}

// ===========================================================================
// Validator (HLC_FORMAT.md §9) — independent of parse_hlc: raw offsets only.
// ===========================================================================
std::vector<HlcCheck> validate_hlc(const std::vector<uint8_t>& d) {
    std::vector<HlcCheck> v;
    auto E = [&](const std::string& m) { v.push_back({true, m}); };
    auto W = [&](const std::string& m) { v.push_back({false, m}); };
    auto u16 = [&](size_t o) -> uint32_t { return o + 2 <= d.size() ? get_u16(d, o) : 0; };
    auto i32 = [&](size_t o) -> int32_t { return o + 4 <= d.size() ? get_i32(d, o) : 0; };
    auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= d.size() ? get_u32(d, o) : 0; };
    auto mmf = [](int32_t r) { char b[32]; std::snprintf(b, sizeof b, "%.3f", r / 1e4); return std::string(b); };
    if (d.size() < 38 + 16 * 26) { E("file too small for a block table"); return v; }
    const uint32_t a0 = u32(38);
    const size_t n = a0 >= 38 ? (a0 - 38) / 16 : 0;
    if ((a0 - 38) % 16 != 0 || (n != 26 && n != 27 && n != 28)) { E("block table length " + std::to_string(n) + " is not 26/27/28"); return v; }
    struct B { uint32_t addr, pre, rs, cnt; };
    std::vector<B> bl(n);
    for (size_t i = 0; i < n; ++i) bl[i] = {u32(38 + 16 * i), u32(42 + 16 * i), u32(46 + 16 * i), u32(50 + 16 * i)};
    std::vector<std::pair<uint64_t, uint64_t>> spans;
    for (size_t i = 0; i < n; ++i) {
        if (!bl[i].addr) continue;
        const uint64_t end = (uint64_t)bl[i].addr + bl[i].pre + (uint64_t)bl[i].rs * bl[i].cnt;
        if (bl[i].addr < a0 || end > d.size()) { E("block " + std::to_string(i) + " lies outside the file"); return v; }
        if (end > bl[i].addr) spans.push_back({bl[i].addr, end});
    }
    std::sort(spans.begin(), spans.end());
    for (size_t i = 1; i < spans.size(); ++i)
        if (spans[i].first < spans[i - 1].second) E("blocks overlap");
    for (int k : {0, 1, 2, 3, 4, 12})
        if (!bl[k].addr || !bl[k].rs || !bl[k].cnt) E("required block " + std::to_string(k) + " missing");
    if (!hlc_check_ok(v)) return v;
    if (n == 28 && bl[4].rs == 1822) {
        // H8H as the JUKI editor writes it (HLC_FORMAT.md §11). PCB Plotter v11 files
        // lacked these and crashed the IS editor / were rejected by the XP HLC software.
        std::string miss;
        if (!bl[11].addr || bl[11].rs != kH8hB11Size || bl[11].cnt != kH8hRecs) miss += " block 11 (3 x 2427 B)";
        if (!bl[26].addr || bl[26].rs != kH8hB26Size || bl[26].cnt != kH8hRecs) miss += " block 26 (3 x 2064 B)";
        if (!bl[15].addr || !bl[17].addr) miss += " blocks 15/17";
        if (bl[0].addr && d[bl[0].addr] == 0) miss += " optimizer defaults";
        if (!miss.empty()) E("H8H program is missing what the JUKI editor always writes:" + miss);
    }
    const B& li = bl[1]; const B& bb = bl[2]; const B& bp = bl[3]; const B& bc = bl[4]; const B& bn = bl[12];
    const uint32_t N = bp.cnt, M = bc.cnt;
    if (bp.rs != 256 && bp.rs != 96 && bp.rs != 71) E("unknown placement record size " + std::to_string(bp.rs));
    if (bn.cnt != N) E("name table count " + std::to_string(bn.cnt) + " != placements " + std::to_string(N));
    if (li.rs >= 302) {
        const size_t l = li.addr;
        if (u32(l + 62) != bb.rs || u32(l + 82) != bp.rs || u32(l + 102) != bc.rs || u32(l + 282) != bn.rs)
            E("line-info record sizes disagree with the block table");
        if (u32(l + 94) != N || u32(l + 294) != N) E("line-info placement count != block 3 count");
        if (u32(l + 114) != M) E("line-info component count != block 4 count");
    }
    // components: per-part placement count
    const size_t cnt_off = bc.rs == 504 ? 110 : 118;
    const size_t st_off = bp.rs == 256 ? 31 : 29;
    std::vector<uint32_t> used(M, 0);
    for (uint32_t i = 0; i < N; ++i) {
        const size_t r = bp.addr + (size_t)bp.rs * i;
        const uint32_t st = u16(r + st_off), ci = u16(r + st_off + 2);
        if (!d[r]) E("placement " + std::to_string(i + 1) + " has no reference");
        if (st & 0x02) {
            if (ci >= M) E("placement " + get_str(d, r, 6) + " links component " + std::to_string(ci) + " of " + std::to_string(M));
            else ++used[ci];
        } else W("placement " + get_str(d, r, 6) + " has no component");
        if ((st & 0x1C) != 0x1C) E("placement " + get_str(d, r, 6) + " has no X/Y/angle set flags");
    }
    for (uint32_t c = 0; c < M; ++c) {
        const size_t r = bc.addr + (size_t)bc.rs * c;
        if (!d[r]) E("component " + std::to_string(c) + " has no part number");
        if (u16(r + cnt_off) != used[c])
            E("component " + get_str(d, r, 21) + " placement count " + std::to_string(u16(r + cnt_off)) +
              " != " + std::to_string(used[c]));
        if (bc.rs >= 1792 && (i32(r + 81) <= 0 || i32(r + 85) <= 0))
            W("component " + get_str(d, r, 21) + " has no outline size");
    }
    // board geometry
    const size_t b = bb.addr;
    const uint32_t mode = u16(b + 42);
    const int cfg = (mode >> 3) & 3;
    const bool rl = mode & 2, back = mode & 1;
    const int32_t BW = i32(b + 44), BH = i32(b + 48), LX = i32(b + 60), LY = i32(b + 64);
    const int32_t CW = i32(b + 70), CH = i32(b + 74), CX = i32(b + 78), CY = i32(b + 82);
    const bool narrow = bp.rs == 71;
    const size_t co = narrow ? 180 : 182;
    const int ncl = d[b + co];
    if (cfg == 3) E("board +42 circuit configuration 3 is undefined");
    if (BW <= 0 || BH <= 0) E("board size missing");
    if (CW <= 0 || CH <= 0) E("circuit dimensions missing (every placement reads as outside the circuit)");
    if (cfg == 0 && ncl) W("Single PWB with a non-empty circuit list");
    if (cfg == 2 && ncl == 0) E("Non-Matrix with no circuits");
    if (cfg == 2 && ncl == 1) W("one circuit written as Non-Matrix; Single PWB is expected");
    if (cfg == 1 && (narrow || (int16_t)u16(b + 160) <= 0 || (int16_t)u16(b + 162) <= 0)) E("Matrix without divisions");
    const int64_t bx0 = rl ? LX : (int64_t)LX - BW, by0 = back ? (int64_t)LY - BH : LY;
    auto in_board = [&](double x, double y) {
        return x >= bx0 - 1 && x <= bx0 + BW + 1 && y >= by0 - 1 && y <= by0 + BH + 1;
    };
    const double sx = cfg == 1 ? i32(b + 164) : 0, sy = cfg == 1 ? i32(b + 168) : 0;
    int out_b = 0, out_c = 0, flag_bad = 0;
    std::string first_b, first_c;
    for (uint32_t i = 0; BW > 0 && i < N; ++i) {
        const size_t r = bp.addr + (size_t)bp.rs * i;
        const double x = i32(r + 8), y = i32(r + 12);
        const bool inc = CW <= 0 || (x >= CX + sx - 1 && x <= CX + sx + CW + 1 && y >= CY + sy - 1 && y <= CY + sy + CH + 1);
        if (!in_board(x + sx, y + sy)) { if (!out_b++) first_b = get_str(d, r, 6) + " (" + mmf((int32_t)x) + ", " + mmf((int32_t)y) + ")"; }
        if (!inc) { if (!out_c++) first_c = get_str(d, r, 6) + " (" + mmf((int32_t)x) + ", " + mmf((int32_t)y) + ")"; }
        if (bp.rs == 256 && (((u16(r + 31) & 0x20) != 0) != !inc)) ++flag_bad;
    }
    if (out_b) E(std::to_string(out_b) + " placement(s) outside the board, first " + first_b);
    if (out_c) E(std::to_string(out_c) + " placement(s) outside the circuit, first " + first_c);
    if (flag_bad) W(std::to_string(flag_bad) + " placement(s) with a stale outside-circuit flag (+31 bit5)");
    // repeated circuits must stay on the board
    auto rect_on_board = [&](double ox, double oy, double deg) {
        const double t = deg * 3.14159265358979323846 / 180.0, c = std::cos(t), s2 = std::sin(t);
        const double px[4] = {(double)CX, (double)CX + CW, (double)CX + CW, (double)CX};
        const double py[4] = {(double)CY, (double)CY, (double)CY + CH, (double)CY + CH};
        for (int k = 0; k < 4; ++k)
            if (!in_board(ox + c * px[k] - s2 * py[k], oy + s2 * px[k] + c * py[k])) return false;
        return true;
    };
    if (BW > 0 && CW > 0) {
        if (cfg == 1) {
            const int nx = (int16_t)u16(b + 160), ny = (int16_t)u16(b + 162);
            const double ppx = i32(b + 172), ppy = i32(b + 176);
            for (int j = 0; j < ny && j < 100; ++j)
                for (int k = 0; k < nx && k < 100; ++k)
                    if (!rect_on_board(sx + k * ppx, sy + j * ppy, 0))
                        E("matrix circuit " + std::to_string(j * nx + k + 1) + " lies outside the board");
        } else if (cfg == 2) {
            for (int k = 0; k < ncl; ++k) {
                const size_t e = b + co + 2 + 12 * (size_t)k;
                if (!rect_on_board(i32(e), i32(e + 4), (int16_t)u16(e + 8) / 80.0))
                    E("circuit " + std::to_string(k + 1) + " lies outside the board");
            }
        } else if (CX < bx0 - 1 || CY < by0 - 1 || CX + CW > bx0 + BW + 1 || CY + CH > by0 + BH + 1) {
            E("circuit outline is not inside the board");
        }
    }
    // marks
    const uint32_t boc = (mode >> 5) & 3;
    const B& bm = bl[6];
    int nm = 0;
    if (bm.addr && bm.cnt && (bm.rs == 3072 || bm.rs == 536 || bm.rs == 552)) {
        const size_t e = bm.addr, es = bm.rs == 3072 ? 128 : 124;
        nm = (int)u16(e + 18);
        if (d[e] != 1) W("mark element 0 is not a BOC mark");
        if (nm > 3) E("more than 3 BOC marks");
        for (int k = 0; k < nm && k < 3; ++k) {
            const double x = i32(e + 40 + es * k), y = i32(e + 44 + es * k);
            const bool ok = boc == 2 ? (x >= CX - 1 && x <= CX + CW + 1 && y >= CY - 1 && y <= CY + CH + 1) : in_board(x, y);
            if (BW > 0 && !ok) E("BOC mark " + std::to_string(k + 1) + " (" + mmf((int32_t)x) + ", " + mmf((int32_t)y) + ") outside the " + (boc == 2 ? "circuit" : "board"));
        }
    }
    if (boc && !nm && !narrow) E("BOC mark type set but no marks");
    // H8H: the editor reads each station's BOC element through board +2596 i16[station]
    // (DivideDataGx6: -1 = no mark). BOC enabled with no linked element = the user's
    // "PWB marks enabled, coordinates empty" (REVIEW_V12 P0-01).
    if (boc && n == 28 && bl[4].rs == 1822 && bm.rs == 3072 && bl[2].addr && bl[11].addr) {
        for (uint32_t st = 0; st < bl[11].cnt && st < 7; ++st) {
            const int16_t ix = (int16_t)u16(bl[2].addr + 2596 + 2 * st);
            if (ix < 0 || (uint32_t)ix >= bm.cnt) { E("BOC marks enabled but station " + std::to_string(st + 1) + " has no mark record (board +2596)"); continue; }
            const size_t e = bm.addr + (size_t)bm.rs * ix;
            const int n = (int)u16(e + 18);
            if (!n) E("BOC mark record for station " + std::to_string(st + 1) + " has 0 marks");
            for (int k = 0; k < n && k < 4; ++k)
                if (!i32(e + 40 + 128 * k) && !i32(e + 44 + 128 * k))
                    E("BOC mark " + std::to_string(k + 1) + " of station " + std::to_string(st + 1) + " has no coordinates");
        }
    }
    if (!boc && nm) W("marks present but BOC mark type is 'not used'");
    return v;
}

bool read_hlc_summary(const std::vector<uint8_t>& data, std::string& report) {
    HlcParsed p;
    std::string err;
    std::ostringstream os;
    if (!parse_hlc(data, p, err)) { report = "parse error: " + err; return false; }
    char buf[256];
    std::snprintf(buf, sizeof buf, "name='%s' station=0x%02X flag=0x%02X version='%s' size=%zu\n",
                  p.name.c_str(), p.station, p.flag, p.version.c_str(), data.size());
    os << buf << "blocks (" << p.blocks.size() << "):\n";
    for (size_t i = 0; i < p.blocks.size(); ++i) {
        const auto& b = p.blocks[i];
        if (!b.addr && !b.prefix && !b.recsize && !b.count) continue;
        std::snprintf(buf, sizeof buf, "  [%2zu] addr=%-8u prefix=%-5u rec=%-6u count=%u\n", i, b.addr,
                      b.prefix, b.recsize, b.count);
        os << buf;
    }
    os << "lineinfo ver='" << p.li_version << "' N=" << p.li_place_count << " M=" << p.li_comp_count << "\n";
    std::snprintf(buf, sizeof buf, "board '%s' mode=0x%02X pcb=%.4fx%.4f layout=(%.4f,%.4f) circuits=%u\n",
                  p.board_name.c_str(), p.board_mode, p.pcb_w / 1e4, p.pcb_h / 1e4, p.layout_x / 1e4,
                  p.layout_y / 1e4, p.circuit_count);
    os << buf;
    {
        const int cfg = (p.board_mode >> 3) & 3;
        std::snprintf(buf, sizeof buf, "circuit config: %s  BOC: %s  circuit %.4fx%.4f @(%.4f,%.4f)\n",
                      cfg == 0 ? "Single PWB" : cfg == 1 ? "Matrix" : cfg == 2 ? "Non-Matrix" : "?",
                      ((p.board_mode >> 5) & 3) == 1 ? "PWB" : ((p.board_mode >> 5) & 3) == 2 ? "circuit" : "none",
                      p.ckt_w / 1e4, p.ckt_h / 1e4, p.ckt_x / 1e4, p.ckt_y / 1e4);
        os << buf;
        if (cfg == 1) {
            std::snprintf(buf, sizeof buf, "matrix %dx%d pitch %.4f,%.4f\n", p.div_x, p.div_y, p.pitch_x / 1e4, p.pitch_y / 1e4);
            os << buf;
        }
    }
    for (size_t i = 0; i < p.circuits.size(); ++i) {
        std::snprintf(buf, sizeof buf, "  ckt %zu (%.4f,%.4f) a=%.3f\n", i + 1, p.circuits[i].x / 1e4,
                      p.circuits[i].y / 1e4, p.circuit_angles[i] / 80.0);
        os << buf;
    }
    os << "components (" << p.components.size() << "):\n";
    for (size_t i = 0; i < p.components.size(); ++i)
        os << "  " << i << " " << p.components[i] << " x" << p.comp_counts[i] << "\n";
    os << "placements (" << p.placements.size() << "):\n";
    for (size_t i = 0; i < p.placements.size(); ++i) {
        const auto& q = p.placements[i];
        std::snprintf(buf, sizeof buf, "  %-6s (%10.4f,%10.4f) a=%7.3f comp=%u st=0x%02X fl=0x%X ms=%u name='%s'\n",
                      q.ref.c_str(), q.x / 1e4, q.y / 1e4, q.angle / 80.0, q.comp, q.status, q.flags,
                      q.mark_set, i < p.names.size() ? p.names[i].c_str() : "");
        os << buf;
    }
    for (const auto& ms : p.mark_sets) {
        os << "markset type=" << (int)ms.type << " '" << ms.name << "' n=" << (int)ms.count;
        for (const auto& m : ms.marks) {
            std::snprintf(buf, sizeof buf, " (%.4f,%.4f)", m.x / 1e4, m.y / 1e4);
            os << buf;
        }
        os << "\n";
    }
    for (const auto& m : p.local_marks) {
        std::snprintf(buf, sizeof buf, "local mark (%.4f,%.4f)\n", m.x / 1e4, m.y / 1e4);
        os << buf;
    }
    os << "validation:\n";
    const auto chk = validate_hlc(data);
    if (chk.empty()) os << "  OK\n";
    for (const auto& c : chk) os << (c.error ? "  ERROR " : "  warn  ") << c.msg << "\n";
    report = os.str();
    return true;
}

} // namespace pcbp::juki

// ---------------------------------------------------------------------------
// Pre-export machine check (REVIEW_V12 P1-04; HLC_FORMAT.md §13): the program is
// written in memory and run through validate_hlc, so "Ready to export" means the
// file passes the same checks the JUKI editors apply.
// ---------------------------------------------------------------------------
namespace pcbp::juki {
bool variant_from_exporter(const std::string& id, HlcVariant& v) {
    static const struct { const char* id; HlcVariant v; } m[] = {
        {"juki.h8h", HlcVariant::H8H}, {"juki.h7h", HlcVariant::H7H}, {"juki.h5h", HlcVariant::H5H},
        {"juki.h4h", HlcVariant::H4H_v6}, {"juki.e41", HlcVariant::E41}, {"juki.e42", HlcVariant::E42},
        {"juki.e43", HlcVariant::E43}, {"juki.e44", HlcVariant::E44}, {"juki.e45", HlcVariant::E45},
        {"juki.e46", HlcVariant::E46}, {"juki.e51", HlcVariant::E51}};
    for (auto& e : m) if (id == e.id) { v = e.v; return true; }
    return false;
}

bool verify_hlc_bytes(const std::vector<uint8_t>& data, std::vector<HlcCheck>& checks, std::string& err) {
    checks.clear();
    HlcParsed P;
    std::string pe;
    if (!parse_hlc(data, P, pe)) { err = "the generated program cannot be read back (" + pe + ")"; return false; }
    checks = validate_hlc(data);
    return true;
}

std::string marks_text(const MarkSelection& m) {
    std::string s;
    char b[96];
    for (size_t k = 0; k < m.marks.size(); ++k) {
        const JobFiducial& f = *m.marks[k];
        std::snprintf(b, sizeof b, " (%s, X %.3f, Y %.3f)", f.number > 0 ? ("#" + std::to_string(f.number)).c_str() : "no number", f.x, f.y);
        s += (k ? "; " : "") + f.name + b;
    }
    return s;
}

void check_job(const MachineJob& job, std::vector<ExportIssue>& out) {
    HlcVariant v;
    if (!variant_from_exporter(job.machine, v)) return;
    if (v == HlcVariant::H4H_v6 && opt(job, "juki.h4h_hlc5") == "1") v = HlcVariant::H4H_v5;
    std::vector<uint8_t> buf; std::string err; HlcWriteInfo wi;
    if (!write_hlc_variant(job, v, buf, err, &wi)) { out.push_back({ExportIssue::Error, "", "Machine program: " + err}); return; }
    for (const auto& c : validate_hlc(buf))
        out.push_back({c.error ? ExportIssue::Error : ExportIssue::Warning, "", "Machine program check: " + c.msg});
    for (const auto& r : wi.renamed)
        out.push_back({ExportIssue::Warning, "", "Part '" + r.first + "' is written as '" + r.second +
                       "': its first 20 characters clash with another part name (full part number kept in the long name)."});
    if (!wi.truncated.empty()) {
        std::string ex;
        for (size_t k = 0; k < wi.truncated.size() && k < 4; ++k) ex += (k ? ", " : "") + wi.truncated[k].first + " -> " + wi.truncated[k].second;
        out.push_back({ExportIssue::Warning, "", std::to_string(wi.truncated.size()) +
                       " part number(s) are longer than 20 characters and are named by their first 20 (" + ex +
                       (wi.truncated.size() > 4 ? ", ..." : "") + "); the full part number is in the long name."});
    }
    if (v == HlcVariant::H8H && opt(job, "juki.research") != "off") {
        const int all = (int)job.parts.size();
        const int made = wi.comps_from_library + wi.comps_from_rules, none = all - wi.comps_researched - made;
        if (wi.comps_researched)
            out.push_back({ExportIssue::Info, "", std::to_string(wi.comps_researched) + " of " + std::to_string(all) +
                           " component(s) carry full machine data from the component database (as Database -> Research writes it)."});
        if (made) {
            std::string ex;
            for (size_t k = 0; k < wi.generated.size() && k < 4; ++k) ex += (k ? "; " : "") + wi.generated[k].first + " (" + wi.generated[k].second + ")";
            out.push_back({ExportIssue::Warning, "", std::to_string(made) + " component(s) are not in the component database; machine data created from " +
                           (wi.comps_from_library ? std::to_string(wi.comps_from_library) + " package-library profile(s)" : std::string()) +
                           (wi.comps_from_library && wi.comps_from_rules ? " and " : "") +
                           (wi.comps_from_rules ? std::string("the built-in rules (") + std::to_string(wi.comps_from_rules) + ")" : std::string()) +
                           ": " + ex + (wi.generated.size() > 4 ? " and " + std::to_string(wi.generated.size() - 4) + " more" : "") +
                           ". Check them in the editor before running."});
        }
        if (none > 0)
            out.push_back({ExportIssue::Warning, "", std::to_string(none) +
                           " component(s) have no machine data (not in the component database, no body size): teach them in the editor."});
    }
    if (wi.comps_without_size)
        out.push_back({ExportIssue::Warning, "", std::to_string(wi.comps_without_size) +
                       " component(s) have no package outline: the machine vision data must be set in the JUKI editor."});
    if (wi.comps_size_from_pads)   // R24a (user v23 #2)
        out.push_back({ExportIssue::Warning, "", std::to_string(wi.comps_size_from_pads) +
                       " component(s) have no body outline: their size is the lead / pad extent (legs included), not the body the machine measures. "
                       "Draw the body in the package editor (Shapes) or import CAD with package outlines."});
    if (wi.comps_without_height)
        out.push_back({ExportIssue::Info, "", std::to_string(wi.comps_without_height) +
                       " component(s) have no height (none in the CAD, BOM or package): height left unset. Enter it in the package editor (Shapes) if the machine needs it."});
    if (wi.text_converted)
        out.push_back({ExportIssue::Info, "", std::to_string(wi.text_converted) +
                       " text field(s) contain characters the JUKI software cannot show; written as ASCII (e.g. \xC2\xB1 -> +/-). The project keeps the original text."});
    if (v == HlcVariant::H8H) {   // R24b machine data: warnings only, never blocks
        int low = 0, unassigned = 0; std::string ex;
        for (const auto& c : job.parts) {
            if (!c.mach_assigned) ++unassigned;
            if (c.mach_conf < 0.6) { if (low < 5) ex += (low ? ", " : "") + c.part; ++low; }
        }
        if (low) out.push_back({ExportIssue::Warning, "", std::to_string(low) + " part(s) have low-confidence machine data (" + ex +
                                (low > 5 ? ", ..." : "") + "): check Packages > Machine before running."});
        if (unassigned) out.push_back({ExportIssue::Info, "", std::to_string(unassigned) +
                                       " part(s) use suggested machine data that was not reviewed (Packages > Machine > Assign)."});
    }
    // R21: validate exactly the marks the selected Board / Circuit mode writes (zero side marks: core Error already)
    const MarkSelection ms = select_marks(job);
    if (wi.marks == 0 && !job.fiducials.empty()) {
        const std::string m = wi.fid_circuit ? "Circuit fiducial mode, but no circuit fiducial on circuit 1: no marks are written."
                                             : "Board fiducial mode, but no board (global) fiducial on this side: no marks are written.";
        if (job.no_marks) out.push_back({ExportIssue::Info, "", m + " ('No marks' is set for this layer.)"});
        else out.push_back({ExportIssue::Error, "", m + " Switch Fiducial marks to " + (wi.fid_circuit ? "Board" : "Circuit") +
                                                    ", add marks, or set 'No marks (intentional)'."});
    }
    if (!ms.marks.empty()) out.push_back({ExportIssue::Info, "", "Marks written (" + std::string(ms.fid_circuit ? "Circuit" : "Board") + " mode): " + marks_text(ms) + "."});

    for (const auto& n : wi.notes)
        if (n.find("ignored") != std::string::npos || n.find("first 3") != std::string::npos || n.find("more than one mark") != std::string::npos)
            out.push_back({ExportIssue::Warning, "", "Machine program: " + n});
}

namespace { struct RegisterJukiCheck { RegisterJukiCheck() { register_job_checker("juki.", &check_job); } } g_register_juki_check; }
// ---- research merge (Database -> Research semantics) ----------------------------------------------------------
namespace {
int32_t rm_rd32(const uint8_t* p) { int32_t x; std::memcpy(&x, p, 4); return x; }
uint16_t rm_rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
void rm_wr16(uint8_t* p, uint16_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); }
void rm_wr32(uint8_t* p, int32_t x) { std::memcpy(p, &x, 4); }
// byte ranges [from, to) of the component record that research takes from the database read
constexpr int kMergeRanges[][2] = {
    {21, 45}, {78, 105}, {115, 118}, {168, 176}, {176, 226}, {226, 704}, {704, 944}, {944, 1139}, {1139, 1143}, {1340, 1342},
};
int max_dim(const uint8_t* r) { return std::max(rm_rd32(r + 81), rm_rd32(r + 85)); }
// small-chip classes whose auxiliary chip data (+978, 20 B) is kept
bool keeps_chip_aux(const uint8_t* db) {
    if (max_dim(db) > 100000) return false;
    const uint16_t c = rm_rd16(db + 78);
    if (c != 1 && c != 3 && c != 6 && c != 14) return false;
    return (rm_rd16(db + 113) & 1) && (db[80] == 1 || db[80] == 4);
}
// vision lead classes whose lead data (+964, 14 B) is kept
bool keeps_lead_aux(const uint8_t* db) {
    if (db[246] != 1) return false;
    switch (rm_rd16(db + 78)) {
        case 101: case 102: case 104: case 105: case 106: case 203: case 302: return true;
        default: return false;
    }
}
}  // namespace

void research_merge_b4(uint8_t* a, const uint8_t* b) {
    // comment: up to the first NUL of the database text, line breaks end it, leading breaks are skipped
    std::memset(a + 47, 0, 31);
    std::memset(a + 1205, 0, 128);
    for (int i = 0; i < 31 && b[47 + i]; ++i) a[47 + i] = b[47 + i];
    for (int i = 0; i < 31; ++i) if (a[47 + i] == 13 || a[47 + i] == 10) a[47 + i] = 0;
    if (!a[47]) {
        int skip = 0;
        bool found = false;
        for (int j = 0; j < 30; ++j) { ++skip; if (!a[47 + j] && a[48 + j]) { found = true; break; } }
        if (found) for (int k = 0; k < 30 - skip; ++k) a[47 + k] = a[47 + k + skip];
    }
    for (int m = 0; m < 30; ++m) if (!a[47 + m]) a[48 + m] = 0;
    a[77] = 0;
    std::memcpy(a + 1205, a + 47, std::strlen((const char*)a + 47));
    const uint8_t keep716 = a[716];
    for (auto& r : kMergeRanges) std::memcpy(a + r[0], b + r[0], (size_t)(r[1] - r[0]));
    // outline-flag word: bits 0-3 and 5 from the database, the rest stays
    const uint16_t m = 0x2F;
    rm_wr16(a + 113, (uint16_t)((rm_rd16(b + 113) & m) | (rm_rd16(a + 113) & ~m)));
    // laser recognition offsets: force the "set" bits and keep the X/Y pair consistent
    if (a[392] == 1) {
        uint16_t f603 = rm_rd16(a + 603), f605 = rm_rd16(a + 605);
        if (!(f603 & 0x200)) { f603 |= 0x200; rm_wr32(a + 410, 0); }
        if (!(f603 & 0x400)) { f603 |= 0x400; rm_wr32(a + 410, 0); }
        if (!(f605 & 0x08)) { f605 |= 0x08; rm_wr32(a + 432, 0); }
        if (!(f605 & 0x10)) { f605 |= 0x10; rm_wr32(a + 432, 0); }
        rm_wr16(a + 603, f603); rm_wr16(a + 605, f605);
        for (int o : {410, 414, 432, 436}) if (rm_rd32(a + o) < -990000 || rm_rd32(a + o) > 990000) rm_wr32(a + o, 0);
        rm_wr32(a + 432, rm_rd32(a + 410));
        rm_wr32(a + 436, rm_rd32(a + 414));
    }
    a[716] = keep716;
    if (!a[715] || a[715] > 7) a[715] = 4;
    if (!keeps_chip_aux(b)) std::memset(a + 978, 0, 20);
    if (!keeps_lead_aux(b)) std::memset(a + 964, 0, 14);
    rm_wr16(a + 118, 1);
    // researched mark: at least the number of assigned feeders, 1 for a complete record
    int used = 0;
    for (int i = 0; i < 7; ++i) used += rm_rd16(a + 134 + 2 * i) + rm_rd16(a + 150 + 2 * i);
    if ((int)(int16_t)rm_rd16(a + 148) < used) rm_wr16(a + 148, (uint16_t)used);
    if (!rm_rd16(a + 148)) rm_wr16(a + 148, 1);   // researched (also when the record is incomplete)
    // a complete record gets at least one pick position (+1116) and its copy (+1118)
    if (a[117] == 2 && !rm_rd16(a + 1116)) rm_wr16(a + 1116, 1);
    if (a[117] == 2 && !rm_rd16(a + 1118)) rm_wr16(a + 1118, rm_rd16(a + 1116));
}


void h8h_relocate_elements(std::vector<uint8_t>& b19, std::vector<uint8_t>& groups, std::vector<uint8_t>& elements,
                           std::vector<uint8_t>& arrays, uint16_t g0, uint16_t e0, uint16_t a0) {
    auto add = [](uint8_t* p, uint16_t base) {
        const uint16_t v = (uint16_t)(p[0] | p[1] << 8);
        if (v == 0xFFFF) return;
        const uint16_t n = (uint16_t)(v + base);
        p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
    };
    if (b19.size() == 36) { add(&b19[20], g0); add(&b19[24], a0); }
    for (size_t o = 0; o + 170 <= groups.size(); o += 170) { add(&groups[o + 8], g0); add(&groups[o + 10], g0); add(&groups[o + 12], e0); }
    for (size_t o = 0; o + 3142 <= arrays.size(); o += 3142) { add(&arrays[o + 8], a0); add(&arrays[o + 10], a0); add(&arrays[o + 12], e0); }
    for (size_t o = 0; o + 112 <= elements.size(); o += 112) { add(&elements[o + 12], e0); add(&elements[o + 14], e0); }
}

} // namespace pcbp::juki
