// ============================================================================
// allegro_brd.cpp — Cadence Allegro binary .brd import (clean-room; layout
// documented in specs/ALLEGRO_BRD.md, derived empirically from a sample
// board cross-checked against its ODB++ export).
//
// Approach: after the fixed header and the string table, the database is a
// sequence of 4-byte aligned records "u8 type, u8 0, u8 a, u8 b, u32 key, ...".
// Rather than depending on a per-type size table we index every aligned
// position that looks like a record header by key, then follow typed key
// references from placed symbols (0x2D) outward, checking at every hop that
// the referenced record has the expected type and back-reference.
// ============================================================================
#include "allegro_brd.h"
#include "io_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

namespace pcbp::io {
namespace {

constexpr size_t kHeaderSize = 0x1200;     // v17.x fixed header; string table follows
constexpr size_t kStrCountOff = 0x194;
constexpr size_t kUnitsOff = 0x268;        // u32 unit code
constexpr size_t kDivisorOff = 0x26c;      // u32 database units per design unit
constexpr uint32_t kMaxStrings = 1u << 20;
constexpr size_t kMaxStrLen = 4096;
constexpr int kMaxChain = 100000;

// record types (first byte)
enum : uint8_t {
    T_ARC = 0x01, T_DEVICE = 0x06, T_COMPINST = 0x07, T_PIN = 0x0D, T_GRAPHIC = 0x14,
    T_LINE_H = 0x15, T_LINE_D = 0x16, T_LINE_V = 0x17, T_PADSTACK = 0x1C, T_SHAPE = 0x28,
    T_FOOTPRINT = 0x2B, T_PLACED = 0x2D, T_PAD = 0x32,
};
constexpr uint8_t kClassBoardGeom = 0x01;
constexpr uint8_t kSubDesignOutline = 0xEA, kSubOutline = 0xFD, kSubCutout = 0xE9;

struct Db {
    const uint8_t* p = nullptr;
    size_t n = 0;
    std::unordered_map<uint32_t, std::string> str;
    std::vector<std::pair<uint32_t, uint32_t>> idx;   // (key, offset) sorted by key
    double div = 100.0, unit_mm = 0.0254;

    bool in(size_t o, size_t len) const { return o <= n && len <= n - o; }
    uint8_t u8(size_t o) const { return in(o, 1) ? p[o] : 0; }
    uint32_t u32(size_t o) const {
        if (!in(o, 4)) return 0;
        return (uint32_t)p[o] | ((uint32_t)p[o + 1] << 8) | ((uint32_t)p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24);
    }
    int32_t s32(size_t o) const { return (int32_t)u32(o); }
    // doubles are stored as two little-endian 32-bit halves, high half first
    double f64s(size_t o) const {
        if (!in(o, 8)) return 0;
        uint64_t hi = u32(o), lo = u32(o + 4), v = (hi << 32) | lo;
        double d; std::memcpy(&d, &v, 8);
        return std::isfinite(d) ? d : 0.0;
    }
    double mm(int32_t v) const { return (double)v / div * unit_mm; }
    Vec2 pt(size_t o) const { return {mm(s32(o)), mm(s32(o + 4))}; }
    // record of `type` whose key is `k` (0 = none)
    size_t get(uint32_t k, uint8_t type, size_t need = 0x10) const {
        if (!k) return 0;
        auto it = std::lower_bound(idx.begin(), idx.end(), std::make_pair(k, 0u));
        for (; it != idx.end() && it->first == k; ++it)
            if (p[it->second] == type && in(it->second, need)) return it->second;
        return 0;
    }
    std::string s(uint32_t id) const {
        auto it = str.find(id);
        return it == str.end() ? std::string() : it->second;
    }
};

bool parse_strings(Db& db, size_t& end, std::string& err) {
    const uint32_t cnt = db.u32(kStrCountOff);
    if (cnt > kMaxStrings) { err = "Allegro .brd: implausible string count " + std::to_string(cnt); return false; }
    size_t o = kHeaderSize;
    for (uint32_t i = 0; i < cnt; ++i) {
        if (!db.in(o, 5)) { err = "Allegro .brd: string table truncated"; return false; }
        const uint32_t id = db.u32(o);
        size_t e = o + 4, lim = std::min(db.n, o + 4 + kMaxStrLen);
        while (e < lim && db.p[e]) ++e;
        if (e >= lim) { err = "Allegro .brd: unterminated string in string table"; return false; }
        db.str[id].assign((const char*)db.p + o + 4, e - (o + 4));
        o = (e + 1 + 3) & ~(size_t)3;
    }
    end = o;
    return true;
}

void build_index(Db& db, size_t start) {
    db.idx.reserve((db.n - start) / 16);
    for (size_t o = (start + 3) & ~(size_t)3; o + 8 <= db.n; o += 4) {
        const uint8_t t = db.p[o];
        if (t >= 1 && t <= 0x3F && db.p[o + 1] == 0) {
            const uint32_t k = db.u32(o + 4);
            if (k) db.idx.emplace_back(k, (uint32_t)o);
        }
    }
    std::sort(db.idx.begin(), db.idx.end());
}

size_t get_segment(const Db& db, uint32_t k) {
    for (uint8_t t : {T_LINE_H, T_LINE_D, T_LINE_V}) if (size_t o = db.get(k, t, 0x2c)) return o;
    return db.get(k, T_ARC, 0x44);
}

// walks a segment chain owned by record `owner` starting at key `first`
int add_segments(const Db& db, uint32_t owner, uint32_t first, OutlineBuilder& ob) {
    int n = 0;
    uint32_t k = first;
    while (k && n < kMaxChain) {
        size_t o = get_segment(db, k);
        if (!o || db.u32(o + 0xc) != owner) break;
        const Vec2 a = db.pt(o + 0x1c), b = db.pt(o + 0x24);
        if (db.p[o] == T_ARC) {
            const Vec2 c{db.f64s(o + 0x2c) / db.div * db.unit_mm, db.f64s(o + 0x34) / db.div * db.unit_mm};
            const bool cw = (db.u8(o + 2) & 0x40) != 0;
            ob.add_arc_ends(a, b, c, !cw);
        } else {
            ob.add_line(a, b);
        }
        ++n;
        k = db.u32(o + 8);
    }
    return n;
}

void read_outline(const Db& db, CadBuilder& b) {
    struct Sh { size_t o; uint8_t sub; };
    std::vector<Sh> shapes, graphics;
    for (auto& [k, o] : db.idx) {
        const uint8_t t = db.p[o];
        if ((t != T_SHAPE && t != T_GRAPHIC) || db.u8(o + 2) != kClassBoardGeom) continue;
        // symbol-definition copies (owner = footprint definition) are skipped; placed copies are absolute
        if (db.get(db.u32(o + 0xc), T_FOOTPRINT)) continue;
        (t == T_SHAPE ? shapes : graphics).push_back({o, db.u8(o + 3)});
    }
    auto use = [&](std::vector<Sh>& v, uint8_t sub, size_t first_off) {
        int n = 0;
        for (auto& s : v)
            if (s.sub == sub) n += add_segments(db, db.u32(s.o + 4), db.u32(s.o + first_off), b.outline);
        return n;
    };
    int n = use(shapes, kSubDesignOutline, 0x28);
    if (!n) n = use(shapes, kSubOutline, 0x28);
    if (!n) n = use(graphics, kSubOutline, 0x18);
    if (!n) n = use(graphics, kSubDesignOutline, 0x18);
    if (!n) { b.warn("Allegro .brd: no board outline found (BOARD GEOMETRY / DESIGN_OUTLINE or OUTLINE)"); return; }
    use(shapes, kSubCutout, 0x28);
}

double norm_deg(double a) { a = std::fmod(a, 360.0); if (a < 0) a += 360.0; return a; }

} // namespace

int sniff_allegro_brd(const std::string& path, const std::string& head) {
    (void)path;
    if (head.size() < 8) return 0;
    const uint8_t* h = (const uint8_t*)head.data();
    // magic: 00 vv MM 00  (MM = major db version 0x0F..0x18)
    if (h[0] == 0 && h[3] == 0 && h[2] >= 0x0F && h[2] <= 0x18 && head.find("<eagle") == std::string::npos) {
        return h[2] == 0x14 ? 90 : 60;   // other versions still claim the file so the user gets a clear error
    }
    return 0;
}

bool import_allegro_brd_data(const std::string& data, Project& prj, const CadImportOptions& opt,
                             CadImportResult& res, std::string& err) {
    res = CadImportResult{};
    res.format = "Allegro board";
    Db db;
    db.p = (const uint8_t*)data.data();
    db.n = data.size();
    if (db.n < kHeaderSize + 16) { err = "Allegro .brd: file too small"; return false; }
    const uint32_t magic = db.u32(0);
    const unsigned major = (magic >> 16) & 0xFF, minor = (magic >> 8) & 0xFF;
    char mbuf[16]; std::snprintf(mbuf, sizeof mbuf, "0x%08X", magic);
    if ((magic & 0xFF0000FFu) != 0 || major < 0x0F || major > 0x18) {
        err = "Allegro .brd: not an Allegro board database (magic " + std::string(mbuf) + ")";
        return false;
    }
    if (major != 0x14 || minor > 0x08) {
        err = "Allegro .brd: unsupported database version " + std::string(mbuf) +
              " (supported: Allegro 17.x, 0x0014xx00 up to 0x00140800). Export ODB++, IPC-2581 or an extract file instead.";
        return false;
    }
    res.source_name = "database " + std::string(mbuf);
    if (minor != 0x04) res.warnings.push_back("Allegro .brd: database " + std::string(mbuf) + " not verified (verified: 0x00140400)");

    const uint32_t ucode = db.u32(kUnitsOff), divisor = db.u32(kDivisorOff);
    switch (ucode) {
    case 1: db.unit_mm = 0.0254; res.units = "mil"; break;
    case 2: db.unit_mm = 25.4; res.units = "inch"; break;
    case 3: db.unit_mm = 1.0; res.units = "mm"; break;
    case 4: db.unit_mm = 10.0; res.units = "cm"; break;
    case 5: db.unit_mm = 0.001; res.units = "um"; break;
    default: err = "Allegro .brd: unknown design unit code " + std::to_string(ucode); return false;
    }
    if (divisor == 0 || divisor > 100000) { err = "Allegro .brd: bad units divisor " + std::to_string(divisor); return false; }
    db.div = divisor;
    const double ovr = units_override_mm(opt.units);
    if (ovr > 0) db.unit_mm = ovr;
    if (opt.scale > 0 && opt.scale != 1.0) db.unit_mm *= opt.scale;
    res.unit_mm = db.unit_mm;
    res.units_evidence = "header unit code " + std::to_string(ucode) + ", " + std::to_string(divisor) + " db units per unit";

    size_t objs = 0;
    if (!parse_strings(db, objs, err)) return false;
    build_index(db, objs);

    CadBuilder b(opt, res);
    if (opt.import_board) read_outline(db, b);

    std::map<std::string, std::string> pkg_final;   // footprint name -> name used
    int noref = 0, nonortho = 0, ncomp = 0;
    for (auto& [key, o] : db.idx) {
        if (db.p[o] != T_PLACED || !db.in(o, 0x48)) continue;
        const uint8_t side = db.u8(o + 2);
        if (side > 1) continue;
        const size_t fp = db.get(db.u32(o + 0xc), T_FOOTPRINT, 0x28);
        if (!fp) continue;
        const std::string fpname = db.s(db.u32(fp + 8));
        if (fpname.empty()) continue;
        const uint32_t rot_md = db.u32(o + 0x1c);
        if (rot_md >= 360000u * 4) continue;

        CadComp c;
        c.pos = db.pt(o + 0x20);
        c.rot = norm_deg(rot_md / 1000.0);
        c.bottom = side == 1;
        const size_t ci = db.get(db.u32(o + 0x28), T_COMPINST, 0x30);
        if (ci && db.u32(ci + 0x18) == key) {
            c.ref = db.s(db.u32(ci + 0x1c));
            if (size_t dev = db.get(db.u32(ci + 0x20), T_DEVICE, 0x14)) c.part = db.s(db.u32(dev + 0xc));
        }

        // pins: placed pads of this instance
        CadPackage pk;
        pk.name = fpname;
        uint32_t pk_first = db.u32(o + 0x30);
        int guard = 0;
        for (uint32_t k = pk_first; k && guard < 10000; ++guard) {
            const size_t pd = db.get(k, T_PAD, 0x54);
            if (!pd || db.u32(pd + 0x1c) != key) break;
            CadPin pin;
            if (size_t pn = db.get(db.u32(pd + 0x24), T_PIN, 0x2c)) {
                pin.name = db.s(db.u32(pn + 8));
                pin.pos = db.pt(pn + 0x10);
                pin.rot = norm_deg(db.u32(pn + 0x28) / 1000.0);
                if (size_t ps = db.get(db.u32(pn + 0x18), T_PADSTACK, 0x10)) {
                    CadPin g;
                    if (guess_padstack(db.s(db.u32(ps + 0xc)), db.unit_mm, g)) { pin.drill = g.drill; pin.shape = g.shape; }
                }
            }
            const Vec2 lo = db.pt(pd + 0x44), hi = db.pt(pd + 0x4c);
            double w = std::fabs(hi.x - lo.x), h = std::fabs(hi.y - lo.y);
            const double tr = norm_deg(c.rot + pin.rot);
            const double q = std::fmod(tr, 90.0);
            if (q > 0.01 && q < 89.99) ++nonortho;
            else if (std::fabs(std::fmod(tr + 45.0, 180.0) - 135.0) < 45.0) std::swap(w, h);   // ~90 / 270
            // stored pad extent is in pad-rotated frame; CadPin w/h are before pin.rot
            pin.w = w; pin.h = h;
            pk.pins.push_back(pin);
            k = db.u32(pd + 0x18);
        }
        if (c.ref.empty()) {
            if (pk.pins.empty()) continue;           // drawing / logo symbols without pins
            c.ref = "no_refdes+" + std::to_string(++noref);
        }
        if (opt.import_packages && !pk.pins.empty()) {
            sort_pins_natural(pk.pins);
            auto f = pkg_final.find(fpname);
            if (f == pkg_final.end()) f = pkg_final.emplace(fpname, b.add_package(pk)).first;
            c.package = f->second;
        } else {
            c.package = fpname;
        }
        b.add_component(c);
        ++ncomp;
    }
    if (noref) b.warn("Allegro .brd: " + std::to_string(noref) + " placed symbols without refdes named no_refdes+N");
    if (nonortho) b.warn("Allegro .brd: " + std::to_string(nonortho) + " pads at non-orthogonal angles; pad size is the rotated extent");
    if (ncomp == 0) {
        err = "Allegro .brd: no placed components decoded (database layout not recognised)";
        return false;
    }
    return b.commit(prj, err);
}

bool import_allegro_brd_file(const std::string& path, Project& prj, const CadImportOptions& opt,
                             CadImportResult& res, std::string& err) {
    std::string data;
    if (!read_file(path, data, err)) return false;
    return import_allegro_brd_data(data, prj, opt, res, err);
}

} // namespace pcbp::io
