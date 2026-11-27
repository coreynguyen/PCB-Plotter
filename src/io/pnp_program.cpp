// ============================================================================
// pnp_program.cpp — JUKI HLC / ISS program model for the Program Editor.
// See pnp_program.h for the design. Offsets: specs/JUKI_EXPORT.md (machine behaviour,
// [T] TrayTagPrinter real-file diffs, H8H_EDIT_FORMAT.md).
// ============================================================================
#include "io/pnp_program.h"
#include "io/juki_writer.h"
#include "core/project.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <cctype>

namespace pcbp::io {

// ===========================================================================
// source
// ===========================================================================
struct IssRng { size_t b = 0, e = 0; bool ok = false; bool self = false; };

struct PnpSource {
    PnpKind kind = PnpKind::None;
    std::vector<uint8_t> bytes;
    // ---- HLC layout
    std::vector<juki::HlcBlockEntry> blk;
    bool narrow = false, ext = false, tmpl9 = false, h8h = false, wide = false;
    size_t status_off = 31, count_off = 118, long_off = 0, ckt_off = 182, mark_es = 128;
    // ---- originals (decoded at load; save diffs the model against these)
    PnpBoard board;
    std::vector<PnpPlacement> placements;
    std::vector<PnpPart> parts;
    std::vector<PnpCircuit> circuits;
    std::vector<PnpMark> marks;
    // ---- ISS ranges
    std::string text;
    IssRng pwb_id, line_name, out_x, out_y, lay_x, lay_y, ckt_w, ckt_h, ckl_x, ckl_y;
    IssRng div_x, div_y, pitch_x, pitch_y, first_x, first_y;
    struct Place { IssRng id_core, id_model, comp, x, y, ang, skip; };
    struct Part { IssRng comment, len, wid, hgt, skip; };
    struct NameRef { IssRng r; std::string value; };
    std::vector<Place> iplace;
    std::vector<Part> ipart;
    std::vector<std::array<IssRng, 3>> ialloc;
    std::vector<std::array<IssRng, 2>> imark;
    std::vector<NameRef> inames;          // every <componentName> text in the document
};

namespace {

// ---- binary helpers --------------------------------------------------------
uint16_t g16(const std::vector<uint8_t>& d, size_t o) { return o + 2 <= d.size() ? (uint16_t)(d[o] | (d[o + 1] << 8)) : 0; }
uint32_t g32(const std::vector<uint8_t>& d, size_t o) {
    if (o + 4 > d.size()) return 0;
    return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
}
int32_t gi32(const std::vector<uint8_t>& d, size_t o) { return (int32_t)g32(d, o); }
std::string gstr(const std::vector<uint8_t>& d, size_t o, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && o + i < d.size() && d[o + i]; ++i) s += (char)d[o + i];
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
void p8(std::vector<uint8_t>& d, size_t o, uint8_t v) { if (o < d.size()) d[o] = v; }
void p16(std::vector<uint8_t>& d, size_t o, uint16_t v) { if (o + 2 <= d.size()) { d[o] = (uint8_t)v; d[o + 1] = (uint8_t)(v >> 8); } }
void p32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
    if (o + 4 > d.size()) return;
    for (int i = 0; i < 4; ++i) d[o + i] = (uint8_t)(v >> (8 * i));
}
// fixed field of n bytes: up to n-1 chars (n chars when full_width), zero padded
void pstr(std::vector<uint8_t>& d, size_t o, size_t n, const std::string& s, bool full_width = false) {
    if (o + n > d.size()) return;
    std::memset(&d[o], 0, n);
    const size_t m = std::min(s.size(), full_width ? n : n - 1);
    if (m) std::memcpy(&d[o], s.data(), m);
}
bool starts_with(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
double mm(int32_t raw) { return raw / 10000.0; }
int32_t raw_len(double v) { return (int32_t)std::clamp<double>(std::round(v * 10000.0), -2147483647.0, 2147483647.0); }
int16_t raw_ang(double deg) {
    double a = std::fmod(deg, 360.0);
    if (a < 0) a += 360.0;
    long r = std::lround(a * 80.0);
    if (r >= 28800) r -= 28800;
    return (int16_t)r;
}
std::string fmt_num(double v) {   // ISS style: up to 4 decimals, no trailing zeros
    char b[48];
    std::snprintf(b, sizeof b, "%.4f", v);
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}
std::string fmt4(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}

// ===========================================================================
// HLC decode
// ===========================================================================
std::string hlc_format_name(const juki::HlcParsed& hp, const PnpSource& S) {
    const auto& B = hp.blocks;
    const std::string& ver = hp.version;
    const uint8_t st = hp.station;
    if (B[4].recsize == 1822 || (B.size() == 28 && B[0].recsize == 4672) || (ver.size() == 4 && ver[0] == 'C')) return "H8H";
    if (S.ext) return "H7H";
    if (S.narrow) {
        if (st >= 11 && st <= 14) return "E4" + std::to_string(st - 10);
        return "H4H";
    }
    if (st == 32 || st == 33) return st == 32 ? "E45" : "E46";
    if (st == 48) return "E51";
    if (ver == "702" || B[4].recsize == 788) return "H5H";
    return "H4H";
}

const char* station_name(uint8_t t) {
    switch (t) {
        case 0x00: return "HLC (master)";
        case 0x0B: return "KE-2010"; case 0x0C: return "KE-2020"; case 0x0D: return "KE-2030"; case 0x0E: return "KE-2040";
        case 0x20: return "KE-2050"; case 0x21: return "KE-2060"; case 0x30: return "FX-1"; case 0x40: return "CX-1";
        case 0x50: return "KE-1070"; case 0x51: return "KE-1080"; case 0x60: return "FX-3"; case 0x80: return "FX-2";
    }
    return "?";
}

size_t rec_off(const juki::HlcBlockEntry& b, size_t i) { return (size_t)b.addr + b.prefix + (size_t)b.recsize * i; }

PnpPlacement hlc_place(const PnpSource& S, size_t i, size_t ncomp) {
    const auto& d = S.bytes;
    const auto& bp = S.blk[3];
    const auto& bn = S.blk[12];
    const size_t r = rec_off(bp, i);
    PnpPlacement p;
    p.src = (int)i;
    std::string shrt = gstr(d, r, 6);
    p.ref = shrt;
    if (bn.addr && i < bn.count) {
        const size_t q = rec_off(bn, i);
        std::string s = gstr(d, q + 1, 11);
        if (bn.recsize >= 32) {
            std::string l = gstr(d, q + 12, 20);
            if (l.size() > s.size() && starts_with(l, s)) s = l;
        }
        if (!s.empty() && (shrt.empty() || starts_with(s, shrt))) p.ref = s;
    }
    p.x = mm(gi32(d, r + 8));
    p.y = mm(gi32(d, r + 12));
    p.rot = (int16_t)g16(d, r + 16) / 80.0;
    p.layer = d[r + 18];
    p.station = d[r + 19];
    const uint16_t fl = g16(d, r + 27);
    p.skip = fl & 1;
    p.has_local = fl & 8;
    const size_t s = r + S.status_off;
    p.status = d[s];
    const uint16_t comp = g16(d, s + 2);
    p.mark_set = p.has_local ? g16(d, s + 4) : -1;
    if (comp < ncomp && (p.status & 0x1F) != 0x0F) p.part = comp;
    return p;
}

PnpPart hlc_part(const PnpSource& S, size_t k) {
    const auto& d = S.bytes;
    const auto& bc = S.blk[4];
    const size_t r = rec_off(bc, k);
    PnpPart c;
    c.src = (int)k;
    c.name = gstr(d, r, S.h8h ? 20 : 21);
    c.desc = gstr(d, r + 47, S.h8h ? 30 : 31);
    if (S.tmpl9 && !S.h8h) {
        const std::string ln = gstr(d, r + S.long_off + 1, 61), ld = gstr(d, r + S.long_off + 62, 128);
        if (c.name.size() == 20 && starts_with(ln, c.name)) c.name = ln;
        if (c.desc.size() == 30 && starts_with(ld, c.desc)) c.desc = ld;
    }
    if (S.tmpl9) {
        const int32_t L = gi32(d, r + 81), W = gi32(d, r + 85), H = gi32(d, r + 89);
        auto sane = [](int32_t v) { return v > 0 && v < 2000000; };
        if (sane(L) && sane(W)) { c.has_body = true; c.len = mm(L); c.wid = mm(W); c.hgt = sane(H) ? mm(H) : 0; }
        c.type = (int16_t)g16(d, r + 78);
        const int ft = d[r + 80];
        c.package = ft == 1 ? "Tape" : ft == 2 ? "Stick" : ft == 3 ? "Tray" : ft == 0 ? "" : "feeder " + std::to_string(ft);
    }
    if (S.h8h) { c.skip_known = true; c.skip = d[r + 716] & 1; }
    c.stored_count = g16(d, r + S.count_off);
    return c;
}

bool load_hlc(const std::vector<uint8_t>& data, PnpProgram& p, std::string& err) {
    juki::HlcParsed hp;
    if (!juki::parse_hlc(data, hp, err)) return false;
    auto S = std::make_shared<PnpSource>();
    S->kind = PnpKind::Hlc;
    S->bytes = data;
    S->blk = hp.blocks;
    const auto& d = S->bytes;
    const auto& bp = S->blk[3];
    const auto& bc = S->blk[4];
    const auto& bb = S->blk[2];
    const auto& bm = S->blk[6];
    if (bp.recsize != 71 && bp.recsize != 96 && bp.recsize != 256) { err = "unknown placement record size " + std::to_string(bp.recsize); return false; }
    S->narrow = bp.recsize == 71;
    S->ext = bp.recsize == 256;
    S->wide = !S->narrow;
    S->status_off = S->ext ? 31 : 29;
    S->tmpl9 = bc.recsize >= 1792;
    S->h8h = bc.recsize == 1822;
    S->count_off = bc.recsize == 504 ? 110 : 118;
    S->long_off = S->h8h ? 1143 : 1115;
    S->ckt_off = S->narrow ? 180 : 182;
    S->mark_es = bm.recsize == 3072 ? 128 : 124;
    p = PnpProgram{};
    p.kind = PnpKind::Hlc;
    p.format = hlc_format_name(hp, *S);
    if (p.format == "H8H" && !S->h8h) p.notes.push_back("H8H header with a non-1822-byte component record: component fields read as H7H");

    // ---- board
    PnpBoard& B = p.board;
    B.program_name = hp.name;
    B.target = station_name(hp.station);
    if (S->blk[1].addr && S->blk[1].recsize >= 54) {
        const size_t l = rec_off(S->blk[1], 0);
        B.line_name = gstr(d, l, 21);
        B.comment = gstr(d, l + 21, 33);
    }
    if (bb.addr && bb.recsize >= 2000) {
        const size_t b = rec_off(bb, 0);
        B.board_name = gstr(d, b, 33);
        B.timestamp = g32(d, b + 34);
        B.mode = g16(d, b + 42);
        B.rl = B.mode & 0x02;
        B.back = B.mode & 0x01;
        B.w = mm(gi32(d, b + 44)); B.h = mm(gi32(d, b + 48));
        B.origin_x = mm(gi32(d, b + 60)); B.origin_y = mm(gi32(d, b + 64));
        B.ckt_w = mm(gi32(d, b + 70)); B.ckt_h = mm(gi32(d, b + 74));
        B.ckt_x = mm(gi32(d, b + 78)); B.ckt_y = mm(gi32(d, b + 82));
        if (S->narrow) { B.thick = mm(gi32(d, b + bb.recsize - 4)); B.has_thick = true; }
        else if (bb.recsize > 10811) { B.thick = mm(gi32(d, b + 10807)); B.has_thick = true; }
        const int cnt = S->narrow ? d[b + S->ckt_off] : (int16_t)g16(d, b + S->ckt_off);
        if (S->wide && (B.mode & 0x08)) {
            B.layout = 1;
            B.div_x = std::max<int>(1, (int16_t)g16(d, b + 160));
            B.div_y = std::max<int>(1, (int16_t)g16(d, b + 162));
            B.first_x = mm(gi32(d, b + 164)); B.first_y = mm(gi32(d, b + 168));
            B.pitch_x = mm(gi32(d, b + 172)); B.pitch_y = mm(gi32(d, b + 176));
            if (B.div_x * B.div_y <= 10000)
                for (int j = 0; j < B.div_y; ++j)
                    for (int i = 0; i < B.div_x; ++i) {
                        PnpCircuit c; c.x = i * B.pitch_x; c.y = j * B.pitch_y; c.derived = true;
                        p.circuits.push_back(c);
                    }
        } else if (cnt > 0) {
            B.layout = 2;
            const int mx = S->narrow ? 100 : 200;
            for (int i = 0; i < cnt && i < mx; ++i) {
                const size_t e = b + S->ckt_off + 2 + 12 * (size_t)i;
                PnpCircuit c;
                c.x = mm(gi32(d, e)); c.y = mm(gi32(d, e + 4));
                c.rot = (int16_t)g16(d, e + 8) / 80.0;
                p.circuits.push_back(c);
            }
        }
        if (S->narrow) {
            const unsigned gc = std::min<unsigned>(d[b + 1425], 3);
            for (unsigned k = 0; k < gc; ++k) {
                PnpMark m;
                m.kind = MarkKind::Global;
                m.set = -1; m.slot = (int)k;
                m.x = mm(gi32(d, b + 1394 + 10 * k));
                m.y = mm(gi32(d, b + 1398 + 10 * k));
                m.dia = g16(d, b + 1426 + 2 * k) / 10000.0;
                m.name = "G" + std::to_string(k + 1);
                p.marks.push_back(m);
            }
        }
    } else {
        p.notes.push_back("board block missing or too small: board fields are not editable");
    }

    // ---- components
    for (uint32_t k = 0; bc.addr && k < bc.count; ++k) p.parts.push_back(hlc_part(*S, k));
    // ---- placements
    for (uint32_t i = 0; bp.addr && i < bp.count; ++i) p.placements.push_back(hlc_place(*S, i, p.parts.size()));
    if (S->blk[12].count != bp.count)
        p.notes.push_back("name table (block 12) has " + std::to_string(S->blk[12].count) + " records for " +
                          std::to_string(bp.count) + " placements: rows cannot be added, deleted or reordered");

    // ---- marks (block 6)
    if (bm.addr) {
        if (S->narrow) {
            for (uint32_t k = 0; k < bm.count; ++k) {
                const size_t e = rec_off(bm, k);
                PnpMark m;
                m.kind = MarkKind::Local;
                m.set = (int)(k / 2); m.slot = (int)k;
                m.x = mm(gi32(d, e)); m.y = mm(gi32(d, e + 4));
                m.name = "L" + std::to_string(k / 2 + 1) + (k % 2 ? "B" : "A");
                p.marks.push_back(m);
            }
        } else {
            std::set<int> used_sets;
            for (auto& q : p.placements) if (q.has_local) used_sets.insert(q.mark_set);
            const size_t es = S->mark_es;
            const size_t slots = bm.recsize > 40 ? (bm.recsize - 40) / es : 0;
            // H8H writes one PWB mark element per station (board +2596[station]); the
            // copies are byte-identical, so list the board marks once (first element).
            std::vector<size_t> pwb_elems;
            for (uint32_t s = 0; s < bm.count; ++s) {
                const size_t e = rec_off(bm, s);
                const uint8_t type = d[e];
                if (type == 1 && bm.recsize > 0 && e + bm.recsize <= d.size()) {
                    bool dup = false;
                    // equal apart from the element name (+2..+17: M001, M002, ...)
                    for (size_t o : pwb_elems)
                        dup |= d[o + 1] == d[e + 1] &&
                               std::equal(d.begin() + (ptrdiff_t)(o + 18), d.begin() + (ptrdiff_t)(o + bm.recsize), d.begin() + (ptrdiff_t)(e + 18));
                    if (dup) continue;
                    pwb_elems.push_back(e);
                }
                const size_t n = std::min<size_t>(g16(d, e + 18), slots);
                const std::string ename = gstr(d, e + 2, 16);
                for (size_t k = 0; k < n; ++k) {
                    const size_t f = e + 40 + es * k;
                    PnpMark m;
                    m.kind = type == 1 ? MarkKind::Global : used_sets.count((int)s) ? MarkKind::Local : MarkKind::Bad;
                    m.set = (int)s; m.slot = (int)k;
                    m.x = mm(gi32(d, f)); m.y = mm(gi32(d, f + 4));
                    if (es == 128) m.name = gstr(d, f + 14, 16);
                    if (m.name.empty()) m.name = (ename.empty() ? "SET" + std::to_string(s) : ename) + "." + std::to_string(k + 1);
                    p.marks.push_back(m);
                }
            }
        }
    }
    for (size_t i = 0; i < p.marks.size(); ++i) p.marks[i].src = (int)i;

    S->board = p.board;
    S->placements = p.placements;
    S->parts = p.parts;
    S->circuits = p.circuits;
    S->marks = p.marks;
    p.src = S;
    p.derive_circuits();
    return true;
}

// ===========================================================================
// HLC save
// ===========================================================================
bool save_hlc(const PnpProgram& p, std::vector<uint8_t>& out, std::string& err, std::vector<std::string>* notes) {
    const PnpSource& S = *p.src;
    const auto& d = S.bytes;
    const size_t nb = S.blk.size();
    auto note = [&](const std::string& s) { if (notes) notes->push_back(s); };
    std::vector<std::vector<uint8_t>> pay(nb);
    std::vector<juki::HlcBlockEntry> tab = S.blk;
    for (size_t i = 0; i < nb; ++i) {
        const auto& b = S.blk[i];
        if (!b.addr) continue;
        const size_t n = b.prefix + (size_t)b.recsize * b.count;
        pay[i].assign(d.begin() + b.addr, d.begin() + b.addr + n);
    }
    const auto& bp = S.blk[3];
    const auto& bn = S.blk[12];
    const size_t N0 = bp.count, N = p.placements.size();

    // ---- structure check
    bool restructured = N != N0;
    for (size_t i = 0; i < N && !restructured; ++i) if (p.placements[i].src != (int)i) restructured = true;
    if (restructured && !p.can_restructure()) { err = "this program's tables cannot be restructured (see load notes)"; return false; }
    if (p.parts.size() != S.parts.size()) { err = "component table size changed (not supported)"; return false; }
    for (size_t k = 0; k < p.parts.size(); ++k)
        if (p.parts[k].src != (int)k) { err = "component table order changed (not supported)"; return false; }

    // ---- block 3 + 12
    if (bp.addr) {
        std::vector<uint8_t> np(bp.prefix + (size_t)bp.recsize * N, 0);
        std::copy(pay[3].begin(), pay[3].begin() + bp.prefix, np.begin());
        std::vector<uint8_t> nn;
        const bool names = bn.addr && bn.count == N0;
        if (names) { nn.assign(bn.prefix + (size_t)bn.recsize * N, 0); std::copy(pay[12].begin(), pay[12].begin() + bn.prefix, nn.begin()); }
        bool trunc_ref = false;
        for (size_t i = 0; i < N; ++i) {
            const PnpPlacement& q = p.placements[i];
            const PnpPlacement o = q.src >= 0 && q.src < (int)S.placements.size() ? S.placements[(size_t)q.src] : PnpPlacement{};
            const size_t r = bp.prefix + (size_t)bp.recsize * i;
            const size_t rn = names ? bn.prefix + (size_t)bn.recsize * i : 0;
            if (q.src >= 0) {
                std::copy(pay[3].begin() + bp.prefix + (size_t)bp.recsize * q.src, pay[3].begin() + bp.prefix + (size_t)bp.recsize * (q.src + 1), np.begin() + r);
                if (names) std::copy(pay[12].begin() + bn.prefix + (size_t)bn.recsize * q.src, pay[12].begin() + bn.prefix + (size_t)bn.recsize * (q.src + 1), nn.begin() + rn);
            } else {
                np[r + 6] = 2; np[r + 18] = 4;
                if (names) nn[rn] = 2;
            }
            if (q.ref != o.ref || q.src < 0) {
                pstr(np, r, 6, q.ref.substr(0, 5));
                if (names) {
                    pstr(nn, rn + 1, 11, q.ref);
                    if (bn.recsize >= 32) pstr(nn, rn + 12, 20, q.ref);
                }
                if ((int)q.ref.size() > p.ref_cap()) trunc_ref = true;
            }
            if (q.x != o.x || q.src < 0) p32(np, r + 8, (uint32_t)raw_len(q.x));
            if (q.y != o.y || q.src < 0) p32(np, r + 12, (uint32_t)raw_len(q.y));
            if (q.rot != o.rot || q.src < 0) p16(np, r + 16, (uint16_t)raw_ang(q.rot));
            if (q.skip != o.skip || q.src < 0) np[r + 27] = (uint8_t)((np[r + 27] & ~1) | (q.skip ? 1 : 0));
            if (q.part != o.part || q.src < 0) {
                const size_t s = r + S.status_off;
                const bool linked = q.part >= 0 && q.part < (int)p.parts.size();
                np[s] = (uint8_t)((o.status & 0xE0) | (linked ? 0x1F : 0x0F));
                p16(np, s + 2, (uint16_t)(linked ? q.part : 0));
            }
        }
        if (trunc_ref) note("some reference designators are longer than " + std::to_string(p.ref_cap()) + " characters and were truncated");
        pay[3] = std::move(np);
        tab[3].count = (uint32_t)N;
        if (names) { pay[12] = std::move(nn); tab[12].count = (uint32_t)N; }
    }

    // ---- block 4: components
    {
        const auto& bc = S.blk[4];
        std::vector<int> cnt_now(p.parts.size(), 0), cnt_old(p.parts.size(), 0);
        for (auto& q : p.placements) if (q.part >= 0 && q.part < (int)cnt_now.size()) cnt_now[(size_t)q.part]++;
        for (auto& q : S.placements) if (q.part >= 0 && q.part < (int)cnt_old.size()) cnt_old[(size_t)q.part]++;
        bool trunc = false;
        for (size_t k = 0; bc.addr && k < p.parts.size(); ++k) {
            const PnpPart& c = p.parts[k];
            const PnpPart& o = S.parts[k];
            auto& v = pay[4];
            const size_t r = bc.prefix + (size_t)bc.recsize * k;
            if (c.name != o.name) {
                if (S.h8h) {                                  // two 20-byte copies [T: +0 / +1144]
                    pstr(v, r, 20, c.name, true);
                    pstr(v, r + 1144, 20, c.name, true);
                    if (c.name.size() > 20) trunc = true;
                } else {
                    pstr(v, r, 21, c.name);
                    if (S.tmpl9) {
                        p8(v, r + S.long_off, (uint8_t)(c.name.size() <= 20 ? 0 : c.name.size() <= 60 ? 1 : 2));
                        pstr(v, r + S.long_off + 1, 61, c.name);
                        if (c.name.size() > 60) trunc = true;
                    } else if (c.name.size() > 20) trunc = true;
                }
            }
            if (c.desc != o.desc) {
                if (S.h8h) pstr(v, r + 47, 30, c.desc, true);
                else {
                    pstr(v, r + 47, 31, c.desc);
                    if (S.tmpl9) pstr(v, r + S.long_off + 62, 128, c.desc);
                }
            }
            if (S.tmpl9 && (c.len != o.len || c.wid != o.wid || c.hgt != o.hgt)) {
                p32(v, r + 81, (uint32_t)raw_len(c.len));
                p32(v, r + 85, (uint32_t)raw_len(c.wid));
                p32(v, r + 89, (uint32_t)raw_len(c.hgt));
            }
            if (S.h8h && c.skip != o.skip) v[r + 716] = (uint8_t)((v[r + 716] & ~1) | (c.skip ? 1 : 0));
            if (cnt_now[k] != cnt_old[k]) p16(v, r + S.count_off, (uint16_t)cnt_now[k]);
        }
        if (trunc) note("some part names exceed the " + p.format + " field and were truncated");
    }

    // ---- block 2: board
    const PnpBoard& B = p.board;
    const PnpBoard& O = S.board;
    const auto& bb = S.blk[2];
    if (bb.addr && bb.recsize >= 2000) {
        auto& v = pay[2];
        const size_t b = bb.prefix;
        if (B.board_name != O.board_name) pstr(v, b, 33, B.board_name);
        uint16_t mode = B.mode;
        if (B.rl != O.rl) mode = (uint16_t)((mode & ~2) | (B.rl ? 2 : 0));
        if (B.back != O.back) mode = (uint16_t)((mode & ~1) | (B.back ? 1 : 0));
        if (mode != O.mode) p16(v, b + 42, mode);
        if (B.w != O.w) p32(v, b + 44, (uint32_t)raw_len(B.w));
        if (B.h != O.h) p32(v, b + 48, (uint32_t)raw_len(B.h));
        if (B.origin_x != O.origin_x) p32(v, b + 60, (uint32_t)raw_len(B.origin_x));
        if (B.origin_y != O.origin_y) p32(v, b + 64, (uint32_t)raw_len(B.origin_y));
        if (B.ckt_w != O.ckt_w) p32(v, b + 70, (uint32_t)raw_len(B.ckt_w));
        if (B.ckt_h != O.ckt_h) p32(v, b + 74, (uint32_t)raw_len(B.ckt_h));
        if (B.ckt_x != O.ckt_x) p32(v, b + 78, (uint32_t)raw_len(B.ckt_x));
        if (B.ckt_y != O.ckt_y) p32(v, b + 82, (uint32_t)raw_len(B.ckt_y));
        if (B.has_thick && B.thick != O.thick) p32(v, S.narrow ? b + bb.recsize - 4 : b + 10807, (uint32_t)raw_len(B.thick));
        if (O.layout == 1) {
            if (B.div_x != O.div_x || B.div_y != O.div_y) { p16(v, b + 160, (uint16_t)B.div_x); p16(v, b + 162, (uint16_t)B.div_y); }
            if (B.first_x != O.first_x || B.first_y != O.first_y) { p32(v, b + 164, (uint32_t)raw_len(B.first_x)); p32(v, b + 168, (uint32_t)raw_len(B.first_y)); }
            if (B.pitch_x != O.pitch_x || B.pitch_y != O.pitch_y) { p32(v, b + 172, (uint32_t)raw_len(B.pitch_x)); p32(v, b + 176, (uint32_t)raw_len(B.pitch_y)); }
        } else {
            const size_t mx = S.narrow ? 100 : 200;
            const size_t n = std::min(p.circuits.size(), mx);
            if (p.circuits.size() > mx) note("only " + std::to_string(mx) + " circuits fit this format; the rest were dropped");
            if (n != S.circuits.size()) {
                if (S.narrow) p8(v, b + S.ckt_off, (uint8_t)n); else p16(v, b + S.ckt_off, (uint16_t)n);
                for (size_t i = n; i < S.circuits.size(); ++i) std::memset(&v[b + S.ckt_off + 2 + 12 * i], 0, 12);
            }
            for (size_t i = 0; i < n; ++i) {
                const PnpCircuit& c = p.circuits[i];
                const bool fresh = i >= S.circuits.size();
                const PnpCircuit oc = fresh ? PnpCircuit{} : S.circuits[i];
                const size_t e = b + S.ckt_off + 2 + 12 * i;
                if (fresh || c.x != oc.x) p32(v, e, (uint32_t)raw_len(c.x));
                if (fresh || c.y != oc.y) p32(v, e + 4, (uint32_t)raw_len(c.y));
                if (fresh || c.rot != oc.rot) p16(v, e + 8, (uint16_t)raw_ang(c.rot));
                if (fresh) p16(v, e + 10, 7);
            }
        }
        if (S.narrow)
            for (const PnpMark& m : p.marks) {
                if (m.set != -1 || m.src < 0) continue;
                const PnpMark& om = S.marks[(size_t)m.src];
                const size_t e = b + 1394 + 10 * (size_t)m.slot;
                if (m.x != om.x) p32(v, e, (uint32_t)raw_len(m.x));
                if (m.y != om.y) p32(v, e + 4, (uint32_t)raw_len(m.y));
            }
    }

    // ---- block 6: marks
    const auto& bm = S.blk[6];
    if (bm.addr) {
        auto& v = pay[6];
        for (const PnpMark& m : p.marks) {
            if (m.src < 0 || (S.narrow && m.set == -1)) continue;
            const PnpMark& om = S.marks[(size_t)m.src];
            size_t f;
            if (S.narrow) f = bm.prefix + (size_t)bm.recsize * (size_t)m.slot;
            else f = bm.prefix + (size_t)bm.recsize * (size_t)m.set + 40 + S.mark_es * (size_t)m.slot;
            if (m.x != om.x) p32(v, f, (uint32_t)raw_len(m.x));
            if (m.y != om.y) p32(v, f + 4, (uint32_t)raw_len(m.y));
            if (!S.narrow && S.mark_es == 128 && m.name != om.name) pstr(v, f + 14, 16, m.name);
        }
    }

    // ---- block 1: line info (name, comment, placement totals)
    const auto& bl = S.blk[1];
    if (bl.addr && bl.recsize >= 302) {
        auto& v = pay[1];
        const size_t l = bl.prefix;
        if (B.line_name != O.line_name) pstr(v, l, 21, B.line_name);
        if (B.comment != O.comment) pstr(v, l + 21, 33, B.comment);
        if (N != N0)   // 20-byte table entries {size, size, max, n, n}: rewrite the placement / name totals
            for (size_t e = l + 62; e + 20 <= l + bl.recsize; e += 20) {
                const uint32_t rs = g32(v, e);
                if ((rs == bp.recsize || rs == bn.recsize) && g32(v, e + 4) == rs && g32(v, e + 12) == N0) {
                    p32(v, e + 12, (uint32_t)N);
                    if (g32(v, e + 16) == N0) p32(v, e + 16, (uint32_t)N);
                }
            }
    }

    // ---- assemble
    const size_t hdr = 38 + 16 * nb;
    bool same_layout = true;
    for (size_t i = 0; i < nb; ++i) {
        const auto& b = S.blk[i];
        if (b.addr && pay[i].size() != b.prefix + (size_t)b.recsize * b.count) same_layout = false;
    }
    if (same_layout) {
        out = d;
        for (size_t i = 0; i < nb; ++i)
            if (S.blk[i].addr) std::copy(pay[i].begin(), pay[i].end(), out.begin() + S.blk[i].addr);
    } else {
        // payloads in their original file order, recompute addresses (empty blocks get the running cursor)
        std::vector<size_t> order(nb);
        for (size_t i = 0; i < nb; ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return S.blk[a].addr < S.blk[b].addr; });
        size_t end_orig = hdr;
        for (size_t i = 0; i < nb; ++i) if (S.blk[i].addr) end_orig = std::max(end_orig, (size_t)S.blk[i].addr + S.blk[i].prefix + (size_t)S.blk[i].recsize * S.blk[i].count);
        out.assign(d.begin(), d.begin() + std::min(hdr, d.size()));
        size_t first = hdr;
        for (size_t i = 0; i < nb; ++i) if (S.blk[i].addr) first = std::min(first, (size_t)S.blk[i].addr);
        if (first > hdr) out.insert(out.end(), d.begin() + hdr, d.begin() + first);   // gap after the table
        for (size_t i : order) {
            if (!S.blk[i].addr) continue;
            tab[i].addr = (uint32_t)out.size();
            out.insert(out.end(), pay[i].begin(), pay[i].end());
        }
        if (end_orig < d.size()) out.insert(out.end(), d.begin() + end_orig, d.end());   // trailing bytes
        for (size_t i = 0; i < nb; ++i) {
            const size_t o = 38 + 16 * i;
            p32(out, o, tab[i].addr); p32(out, o + 4, tab[i].prefix); p32(out, o + 8, tab[i].recsize); p32(out, o + 12, tab[i].count);
        }
        note("tables resized: " + std::to_string(N0) + " -> " + std::to_string(N) + " placements; component counts and line-info totals updated");
    }
    if (B.program_name != O.program_name) pstr(out, 0, 32, B.program_name, true);
    return true;
}

// ===========================================================================
// ISS (XML) scanner with byte ranges
// ===========================================================================
struct Xs {
    const std::string& t;
    static bool delim(char c) { return c == ' ' || c == '>' || c == '/' || c == '\t' || c == '\r' || c == '\n'; }
    size_t open(const std::string& tag, size_t from, size_t to) const {
        const std::string pat = "<" + tag;
        for (size_t p = t.find(pat, from); p != std::string::npos && p < to; p = t.find(pat, p + 1)) {
            const char c = p + pat.size() < t.size() ? t[p + pat.size()] : '>';
            if (delim(c)) return p;
        }
        return std::string::npos;
    }
    size_t gt(size_t op) const {
        char q = 0;
        for (size_t i = op; i < t.size(); ++i) {
            const char c = t[i];
            if (q) { if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '>') return i;
        }
        return std::string::npos;
    }
    bool self_closing(size_t op) const { size_t g = gt(op); return g != std::string::npos && g > 0 && t[g - 1] == '/'; }
    size_t close(const std::string& tag, size_t from) const {
        const std::string pat = "</" + tag;
        for (size_t p = t.find(pat, from); p != std::string::npos; p = t.find(pat, p + 1)) {
            const char c = p + pat.size() < t.size() ? t[p + pat.size()] : '>';
            if (c == '>' || c == ' ' || c == '\t' || c == '\r' || c == '\n') return p;
        }
        return std::string::npos;
    }
    // index after the element opened at op
    size_t end(const std::string& tag, size_t op) const {
        size_t g = gt(op);
        if (g == std::string::npos) return t.size();
        if (t[g - 1] == '/') return g + 1;
        int depth = 1;
        size_t p = g + 1;
        while (depth > 0) {
            size_t o = open(tag, p, t.size());
            size_t c = close(tag, p);
            if (c == std::string::npos) return t.size();
            if (o != std::string::npos && o < c) {
                if (!self_closing(o)) ++depth;
                p = gt(o) + 1;
            } else {
                --depth;
                p = t.find('>', c) + 1;
            }
        }
        return p;
    }
    // inner content range of the element at op (close tag start), self-closing -> empty at '/'
    IssRng inner(const std::string& tag, size_t op) const {
        IssRng r;
        size_t g = gt(op);
        if (g == std::string::npos) return r;
        if (t[g - 1] == '/') { r.b = r.e = g - 1; r.ok = true; r.self = true; return r; }
        size_t e = end(tag, op);
        size_t c = e ? t.rfind("</", e - 1) : std::string::npos;
        if (c == std::string::npos || c < g) return r;
        r.b = g + 1; r.e = c; r.ok = true;
        return r;
    }
    IssRng text(const std::string& tag, size_t from, size_t to) const {
        size_t o = open(tag, from, to);
        if (o == std::string::npos) return {};
        IssRng r = inner(tag, o);
        if (!r.ok || r.e > to) return {};
        return r;
    }
    IssRng attr(size_t op, const std::string& name) const {
        IssRng r;
        size_t g = gt(op);
        if (g == std::string::npos) return r;
        size_t i = op + 1;
        while (i < g && !delim(t[i])) ++i;   // tag name
        while (i < g) {
            while (i < g && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) ++i;
            if (i >= g || t[i] == '/' || t[i] == '>') break;
            size_t nb = i;
            while (i < g && t[i] != '=' && !delim(t[i])) ++i;
            size_t ne = i;
            while (i < g && (t[i] == ' ' || t[i] == '\t')) ++i;
            if (i >= g || t[i] != '=') continue;
            ++i;
            while (i < g && (t[i] == ' ' || t[i] == '\t')) ++i;
            if (i >= g) break;
            const char q = t[i];
            if (q != '"' && q != '\'') continue;
            size_t vb = ++i;
            while (i < g && t[i] != q) ++i;
            size_t ve = i++;
            if (ne - nb == name.size() && t.compare(nb, name.size(), name) == 0) { r.b = vb; r.e = ve; r.ok = true; return r; }
        }
        return r;
    }
    IssRng attr_of(const std::string& tag, const std::string& name, size_t from, size_t to) const {
        size_t o = open(tag, from, to);
        if (o == std::string::npos) return {};
        return attr(o, name);
    }
    bool path(const std::vector<std::string>& tags, size_t& op, size_t& ed) const {
        size_t from = 0, to = t.size();
        for (auto& tg : tags) {
            size_t o = open(tg, from, to);
            if (o == std::string::npos) return false;
            op = o; ed = end(tg, o);
            from = gt(o) + 1; to = ed;
        }
        return true;
    }
};

std::string xml_decode(std::string s) {
    static const std::pair<const char*, char> ent[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'}};
    for (auto& e : ent)
        for (size_t p = s.find(e.first); p != std::string::npos; p = s.find(e.first, p + 1)) s.replace(p, std::strlen(e.first), 1, e.second);
    return s;
}
std::string xml_encode(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break; case '<': o += "&lt;"; break; case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break; default: o += c;
        }
    }
    return o;
}
std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::string val(const std::string& t, const IssRng& r) { return r.ok ? trim(xml_decode(t.substr(r.b, r.e - r.b))) : std::string(); }
double num(const std::string& t, const IssRng& r, double def = 0) {
    if (!r.ok) return def;
    std::string s = val(t, r);
    char* e = nullptr;
    double v = std::strtod(s.c_str(), &e);
    return e == s.c_str() ? def : v;
}
bool truthy(const std::string& v) {
    std::string u;
    for (char c : v) u += (char)std::toupper((unsigned char)c);
    return u == "YES" || u == "TRUE" || u == "1" || u == "NOUSE" || u == "ON";
}
// last "<tag" / "</tag" (followed by a delimiter) at or before pos
size_t last_tag(const std::string& t, const std::string& pat, size_t pos) {
    size_t from = pos;
    while (true) {
        size_t p = t.rfind(pat, from);
        if (p == std::string::npos) return p;
        const char ch = p + pat.size() < t.size() ? t[p + pat.size()] : '>';
        if (Xs::delim(ch)) return p;
        if (p == 0) return std::string::npos;
        from = p - 1;
    }
}
bool inside(const std::string& t, const std::string& tag, size_t pos) {
    const size_t o = last_tag(t, "<" + tag, pos);
    if (o == std::string::npos) return false;
    const size_t c = last_tag(t, "</" + tag, pos);
    return c == std::string::npos || c < o;
}

bool load_iss(const std::vector<uint8_t>& data, PnpProgram& p, std::string& err) {
    auto S = std::make_shared<PnpSource>();
    S->kind = PnpKind::Iss;
    S->bytes = data;
    S->text.assign(data.begin(), data.end());
    const std::string& t = S->text;
    Xs x{t};
    if (x.open("productionProgram", 0, t.size()) == std::string::npos && x.open("placementData", 0, t.size()) == std::string::npos) {
        err = "not an ISS program (no productionProgram element)";
        return false;
    }
    p = PnpProgram{};
    p.kind = PnpKind::Iss;
    p.format = "ISS";
    PnpBoard& B = p.board;
    S->pwb_id = x.text("pwbId", 0, t.size());
    B.program_name = val(t, S->pwb_id);
    B.board_name = B.program_name;
    size_t op, ed;
    if (x.path({"headerData", "lineConfiguration"}, op, ed)) { S->line_name = x.text("lineName", op, ed); B.line_name = val(t, S->line_name); }
    if (x.path({"headerData"}, op, ed)) B.target = val(t, x.text("targetMachine", op, ed));
    if (x.path({"core", "pwbData", "pwbConfiguration", "outline"}, op, ed)) {
        S->out_x = x.attr(op, "x"); S->out_y = x.attr(op, "y");
        B.w = num(t, S->out_x); B.h = num(t, S->out_y);
    }
    if (x.path({"model", "pwbData", "pwbConfiguration", "pwbLayoutOffset"}, op, ed)) {
        S->lay_x = x.attr(op, "x"); S->lay_y = x.attr(op, "y");
        B.origin_x = num(t, S->lay_x); B.origin_y = num(t, S->lay_y);
    }
    if (x.path({"model", "pwbData", "circuitConfigurationData"}, op, ed)) {
        size_t c0 = x.open("circuitConfiguration", x.gt(op) + 1, ed);
        if (c0 != std::string::npos) {
            size_t c0e = x.end("circuitConfiguration", c0);
            S->ckl_x = x.attr_of("circuitLayoutOffset", "x", c0, c0e);
            S->ckl_y = x.attr_of("circuitLayoutOffset", "y", c0, c0e);
            B.ckt_x = num(t, S->ckl_x); B.ckt_y = num(t, S->ckl_y);
        }
    }
    if (x.path({"core", "pwbData", "circuitConfigurationData"}, op, ed)) {
        size_t c0 = x.open("circuitConfiguration", x.gt(op) + 1, ed);
        if (c0 != std::string::npos) {
            size_t c0e = x.end("circuitConfiguration", c0);
            S->ckt_w = x.attr_of("circuitOutline", "x", c0, c0e);
            S->ckt_h = x.attr_of("circuitOutline", "y", c0, c0e);
            B.ckt_w = num(t, S->ckt_w); B.ckt_h = num(t, S->ckt_h);
            const std::string mode = val(t, x.text("circuitConfiguration", x.gt(c0) + 1, c0e));
            size_t nm = x.open("nonMatrix", c0, c0e);
            size_t mo = x.open("matrix", c0, c0e);
            if (mode == "NONMATRIX" || (mode.empty() && nm != std::string::npos)) {
                B.layout = 2;
                size_t nme = nm == std::string::npos ? c0 : x.end("nonMatrix", nm);
                for (size_t a = nm == std::string::npos ? std::string::npos : x.open("allocation", nm, nme); a != std::string::npos; a = x.open("allocation", x.gt(a) + 1, nme)) {
                    std::array<IssRng, 3> rr{x.attr(a, "x"), x.attr(a, "y"), x.attr(a, "angle")};
                    PnpCircuit c; c.x = num(t, rr[0]); c.y = num(t, rr[1]); c.rot = num(t, rr[2]);
                    p.circuits.push_back(c);
                    S->ialloc.push_back(rr);
                }
            } else if (mo != std::string::npos) {
                size_t me = x.end("matrix", mo);
                S->div_x = x.attr_of("divideNumber", "x", mo, me); S->div_y = x.attr_of("divideNumber", "y", mo, me);
                S->pitch_x = x.attr_of("matrixPitch", "x", mo, me); S->pitch_y = x.attr_of("matrixPitch", "y", mo, me);
                S->first_x = x.attr_of("matrixReferencePosition", "x", mo, me); S->first_y = x.attr_of("matrixReferencePosition", "y", mo, me);
                B.div_x = std::max(1, (int)std::lround(num(t, S->div_x, 1)));
                B.div_y = std::max(1, (int)std::lround(num(t, S->div_y, 1)));
                B.pitch_x = num(t, S->pitch_x); B.pitch_y = num(t, S->pitch_y);
                B.first_x = num(t, S->first_x); B.first_y = num(t, S->first_y);
                if (mode == "MATRIX" || B.div_x * B.div_y > 1) {
                    B.layout = 1;
                    if (B.div_x * B.div_y <= 10000)
                        for (int j = 0; j < B.div_y; ++j)
                            for (int i = 0; i < B.div_x; ++i) {
                                PnpCircuit c; c.x = i * B.pitch_x; c.y = j * B.pitch_y; c.derived = true;
                                p.circuits.push_back(c);
                            }
                }
            }
        }
    }

    // every <componentName> (cascade rename)
    for (size_t o = x.open("componentName", 0, t.size()); o != std::string::npos; o = x.open("componentName", x.gt(o) + 1, t.size())) {
        IssRng r = x.inner("componentName", o);
        if (r.ok && !r.self) S->inames.push_back({r, val(t, r)});
    }

    // components (core), model extras by name
    size_t core_o = x.open("core", 0, t.size()), core_e = core_o == std::string::npos ? 0 : x.end("core", core_o);
    size_t model_o = x.open("model", 0, t.size()), model_e = model_o == std::string::npos ? 0 : x.end("model", model_o);
    std::map<std::string, int> part_idx;
    if (core_o != std::string::npos) {
        size_t cd = x.open("componentData", core_o, core_e);
        size_t cde = cd == std::string::npos ? cd : x.end("componentData", cd);
        for (size_t c = cd == std::string::npos ? cd : x.open("component", cd, cde); c != std::string::npos; c = x.open("component", x.end("component", c), cde)) {
            size_t ce = x.end("component", c);
            PnpPart k;
            k.name = val(t, x.text("componentName", c, ce));
            if (k.name.empty() || part_idx.count(k.name)) continue;
            PnpSource::Part rr;
            rr.comment = x.text("comment", c, ce);
            k.desc = val(t, rr.comment);
            size_t cs = x.open("componentSize", c, ce);
            if (cs != std::string::npos) {
                rr.len = x.attr(cs, "length"); rr.wid = x.attr(cs, "witdh"); if (!rr.wid.ok) rr.wid = x.attr(cs, "width");
                rr.hgt = x.attr(cs, "height");
                k.len = num(t, rr.len); k.wid = num(t, rr.wid); k.hgt = num(t, rr.hgt);
                k.has_body = k.len > 0 && k.wid > 0;
            }
            k.src = (int)p.parts.size();
            part_idx[k.name] = k.src;
            p.parts.push_back(k);
            S->ipart.push_back(rr);
        }
    }
    if (model_o != std::string::npos) {
        size_t cd = x.open("componentData", model_o, model_e);
        size_t cde = cd == std::string::npos ? cd : x.end("componentData", cd);
        for (size_t c = cd == std::string::npos ? cd : x.open("component", cd, cde); c != std::string::npos; c = x.open("component", x.end("component", c), cde)) {
            size_t ce = x.end("component", c);
            auto it = part_idx.find(val(t, x.text("componentName", c, ce)));
            if (it == part_idx.end()) continue;
            PnpPart& k = p.parts[(size_t)it->second];
            k.package = val(t, x.text("package", c, ce));
            IssRng sk = x.text("componentSkip", c, ce);
            if (sk.ok && !sk.self) { S->ipart[(size_t)it->second].skip = sk; k.skip_known = true; k.skip = truthy(val(t, sk)); }
        }
    }

    // placements: core records (position etc.) + model records (layer / action) in the same order
    std::vector<std::pair<size_t, size_t>> mrec;
    if (model_o != std::string::npos) {
        size_t pd = x.open("placementData", model_o, model_e);
        size_t pde = pd == std::string::npos ? pd : x.end("placementData", pd);
        for (size_t q = pd == std::string::npos ? pd : x.open("placement", pd, pde); q != std::string::npos; q = x.open("placement", x.end("placement", q), pde))
            mrec.push_back({q, x.end("placement", q)});
    }
    if (core_o != std::string::npos) {
        size_t pd = x.open("placementData", core_o, core_e);
        size_t pde = pd == std::string::npos ? pd : x.end("placementData", pd);
        for (size_t q = pd == std::string::npos ? pd : x.open("placement", pd, pde); q != std::string::npos; q = x.open("placement", x.end("placement", q), pde)) {
            size_t qe = x.end("placement", q);
            PnpSource::Place rr;
            PnpPlacement pl;
            rr.id_core = x.text("placementId", q, qe);
            pl.ref = val(t, rr.id_core);
            rr.comp = x.text("componentName", q, qe);
            size_t pp = x.open("placementPosition", q, qe);
            if (pp != std::string::npos) { rr.x = x.attr(pp, "x"); rr.y = x.attr(pp, "y"); }
            pl.x = num(t, rr.x); pl.y = num(t, rr.y);
            rr.ang = x.attr_of("placementAngle", "angle", q, qe);
            pl.rot = num(t, rr.ang);
            rr.skip = x.attr_of("skip", "placement", q, qe);
            pl.skip = truthy(val(t, rr.skip));
            pl.station = (int)num(t, x.attr_of("station", "placement", q, qe));
            pl.head = (int)num(t, x.attr_of("headUnit", "placement", q, qe)) * 100 + (int)num(t, x.attr_of("head", "placement", q, qe));
            const std::string bc = val(t, x.text("baseCircuitId", q, qe));
            pl.circuit = bc.empty() ? -1 : std::atoi(bc.c_str());   // normalised below
            const std::string cn = val(t, rr.comp);
            if (!cn.empty()) {
                auto it = part_idx.find(cn);
                if (it == part_idx.end()) {   // referenced but not defined: keep as an unlinked name
                    PnpPart k; k.name = cn; k.src = (int)p.parts.size();
                    it = part_idx.emplace(cn, k.src).first;
                    p.parts.push_back(k);
                    S->ipart.push_back({});
                    p.notes.push_back("component '" + cn + "' is referenced by a placement but not defined");
                }
                pl.part = it->second;
            }
            const size_t i = S->iplace.size();
            if (i < mrec.size()) {
                rr.id_model = x.text("placementId", mrec[i].first, mrec[i].second);
                pl.layer = std::atoi(val(t, x.attr_of("layer", "placement", mrec[i].first, mrec[i].second)).c_str());
                pl.action = val(t, x.attr_of("action", "placement", mrec[i].first, mrec[i].second));
            }
            pl.src = (int)i;
            p.placements.push_back(pl);
            S->iplace.push_back(rr);
        }
    }
    {   // baseCircuitId: 0- or 1-based -> 0-based
        int lo = 1 << 30;
        for (auto& q : p.placements) if (q.circuit >= 0) lo = std::min(lo, q.circuit);
        for (auto& q : p.placements) q.circuit = q.circuit < 0 ? 0 : std::max(0, q.circuit - (lo >= 1 ? 1 : 0));
    }
    if (!mrec.empty() && mrec.size() != S->iplace.size())
        p.notes.push_back("model placement records (" + std::to_string(mrec.size()) + ") do not match core records (" + std::to_string(S->iplace.size()) + ")");

    // marks: every markPosition with x and y
    for (size_t m = x.open("markPosition", 0, t.size()); m != std::string::npos; m = x.open("markPosition", x.gt(m) + 1, t.size())) {
        std::array<IssRng, 2> rr{x.attr(m, "x"), x.attr(m, "y")};
        if (!rr[0].ok || !rr[1].ok) continue;
        PnpMark k;
        k.x = num(t, rr[0]); k.y = num(t, rr[1]);
        k.kind = inside(t, "badMarkData", m) || inside(t, "badMark", m) ? MarkKind::Bad : MarkKind::Global;
        k.set = 0; k.slot = (int)S->imark.size();
        k.name = (k.kind == MarkKind::Bad ? "BAD" : "M") + std::to_string(S->imark.size() + 1);
        k.src = (int)S->imark.size();
        p.marks.push_back(k);
        S->imark.push_back(rr);
    }
    if (p.placements.empty()) p.notes.push_back("no placements found");

    S->board = p.board;
    S->placements = p.placements;
    S->parts = p.parts;
    S->circuits = p.circuits;
    S->marks = p.marks;
    p.src = S;
    return true;
}

bool save_iss(const PnpProgram& p, std::vector<uint8_t>& out, std::string& err, std::vector<std::string>* notes) {
    const PnpSource& S = *p.src;
    const std::string& t = S.text;
    struct Patch { size_t b, e; std::string s; };
    std::vector<Patch> pt;
    std::map<size_t, size_t> at;   // b -> index (latest wins)
    bool self_skip = false;
    auto put = [&](const IssRng& r, const std::string& v) {
        if (!r.ok) return false;
        if (r.self) { self_skip = true; return false; }
        auto it = at.find(r.b);
        if (it != at.end()) pt[it->second].s = v;
        else { at[r.b] = pt.size(); pt.push_back({r.b, r.e, v}); }
        return true;
    };
    auto putn = [&](const IssRng& r, double v) { return put(r, fmt_num(v)); };
    bool missing = false;
    auto need = [&](bool ok) { if (!ok) missing = true; };
    if (p.placements.size() != S.placements.size()) { err = "ISS programs support field edits only (no rows added / removed)"; return false; }
    for (size_t i = 0; i < p.placements.size(); ++i)
        if (p.placements[i].src != (int)i) { err = "ISS programs cannot be reordered"; return false; }
    if (p.parts.size() != S.parts.size()) { err = "ISS component table changed size"; return false; }

    const PnpBoard& B = p.board;
    const PnpBoard& O = S.board;
    if (B.program_name != O.program_name) need(put(S.pwb_id, xml_encode(B.program_name)));
    if (B.line_name != O.line_name) need(put(S.line_name, xml_encode(B.line_name)));
    if (B.w != O.w) need(putn(S.out_x, B.w));
    if (B.h != O.h) need(putn(S.out_y, B.h));
    if (B.origin_x != O.origin_x) need(putn(S.lay_x, B.origin_x));
    if (B.origin_y != O.origin_y) need(putn(S.lay_y, B.origin_y));
    if (B.ckt_w != O.ckt_w) need(putn(S.ckt_w, B.ckt_w));
    if (B.ckt_h != O.ckt_h) need(putn(S.ckt_h, B.ckt_h));
    if (B.ckt_x != O.ckt_x) need(putn(S.ckl_x, B.ckt_x));
    if (B.ckt_y != O.ckt_y) need(putn(S.ckl_y, B.ckt_y));
    if (B.div_x != O.div_x) need(put(S.div_x, std::to_string(B.div_x)));
    if (B.div_y != O.div_y) need(put(S.div_y, std::to_string(B.div_y)));
    if (B.pitch_x != O.pitch_x) need(putn(S.pitch_x, B.pitch_x));
    if (B.pitch_y != O.pitch_y) need(putn(S.pitch_y, B.pitch_y));
    if (B.first_x != O.first_x) need(putn(S.first_x, B.first_x));
    if (B.first_y != O.first_y) need(putn(S.first_y, B.first_y));
    if (O.layout == 2) {
        if (p.circuits.size() != S.circuits.size()) { err = "ISS circuit allocations cannot be added or removed here"; return false; }
        for (size_t i = 0; i < p.circuits.size(); ++i) {
            const PnpCircuit& c = p.circuits[i];
            const PnpCircuit& o = S.circuits[i];
            if (c.x != o.x) need(putn(S.ialloc[i][0], c.x));
            if (c.y != o.y) need(putn(S.ialloc[i][1], c.y));
            if (c.rot != o.rot) need(putn(S.ialloc[i][2], c.rot));
        }
    }
    // part renames cascade to every <componentName> holding the old name
    std::map<std::string, std::string> ren;
    for (size_t k = 0; k < p.parts.size(); ++k)
        if (p.parts[k].name != S.parts[k].name) ren[S.parts[k].name] = p.parts[k].name;
    int cascaded = 0;
    for (const auto& nr : S.inames) {
        auto it = ren.find(nr.value);
        if (it != ren.end()) { put(nr.r, xml_encode(it->second)); ++cascaded; }
    }
    if (cascaded && notes) notes->push_back("part rename updated " + std::to_string(cascaded) + " <componentName> references");
    for (size_t k = 0; k < p.parts.size(); ++k) {
        const PnpPart& c = p.parts[k];
        const PnpPart& o = S.parts[k];
        const PnpSource::Part& r = S.ipart[k];
        if (c.desc != o.desc) need(put(r.comment, xml_encode(c.desc)));
        if (c.len != o.len) need(putn(r.len, c.len));
        if (c.wid != o.wid) need(putn(r.wid, c.wid));
        if (c.hgt != o.hgt) need(putn(r.hgt, c.hgt));
        if (c.skip != o.skip) need(put(r.skip, c.skip ? "YES" : "NO"));
    }
    for (size_t i = 0; i < p.placements.size(); ++i) {
        const PnpPlacement& q = p.placements[i];
        const PnpPlacement& o = S.placements[i];
        const PnpSource::Place& r = S.iplace[i];
        if (q.ref != o.ref) { need(put(r.id_core, xml_encode(q.ref))); if (r.id_model.ok) put(r.id_model, xml_encode(q.ref)); }
        if (q.x != o.x) need(putn(r.x, q.x));
        if (q.y != o.y) need(putn(r.y, q.y));
        if (q.rot != o.rot) need(putn(r.ang, q.rot));
        if (q.skip != o.skip) need(put(r.skip, q.skip ? "YES" : "NO"));
        if (q.part != o.part) {
            const std::string nm = q.part >= 0 && q.part < (int)p.parts.size() ? p.parts[(size_t)q.part].name : std::string();
            need(put(r.comp, xml_encode(nm)));
        }
    }
    for (const PnpMark& m : p.marks) {
        if (m.src < 0) continue;
        const PnpMark& o = S.marks[(size_t)m.src];
        if (m.x != o.x) need(putn(S.imark[(size_t)m.src][0], m.x));
        if (m.y != o.y) need(putn(S.imark[(size_t)m.src][1], m.y));
    }
    if (notes && (missing || self_skip)) notes->push_back("some edited values have no element / attribute in this ISS file and were not written");
    std::sort(pt.begin(), pt.end(), [](const Patch& a, const Patch& b) { return a.b < b.b; });
    std::string o;
    o.reserve(t.size() + 256);
    size_t cur = 0;
    for (auto& q : pt) {
        if (q.b < cur) continue;
        o.append(t, cur, q.b - cur);
        o += q.s;
        cur = q.e;
    }
    o.append(t, cur, std::string::npos);
    out.assign(o.begin(), o.end());
    return true;
}

} // namespace

// ===========================================================================
// PnpProgram
// ===========================================================================
bool PnpProgram::can_restructure() const {
    if (!src || kind != PnpKind::Hlc) return false;
    return src->blk.size() > 12 && src->blk[3].addr && (src->blk[12].addr == 0 || src->blk[12].count == src->blk[3].count);
}
bool PnpProgram::can_rename_part() const { return (bool)src; }
bool PnpProgram::part_body_editable() const {
    if (!src) return false;
    return kind == PnpKind::Iss || src->tmpl9;
}
bool PnpProgram::circuits_editable() const { return board.layout == 2 || (kind == PnpKind::Hlc && board.layout == 0); }
int PnpProgram::ref_cap() const {
    if (!src || kind == PnpKind::Iss) return 64;
    const auto& bn = src->blk[12];
    return bn.addr ? (bn.recsize >= 32 ? 19 : 10) : 5;
}
int PnpProgram::part_cap() const {
    if (!src || kind == PnpKind::Iss) return 64;
    return src->h8h ? 20 : src->tmpl9 ? 60 : 20;
}
Vec2 PnpProgram::board_corner() const {
    if (kind != PnpKind::Hlc || board.w <= 0 || board.h <= 0) return {0, 0};
    Vec2 a{board.rl ? board.origin_x : board.origin_x - board.w, board.back ? board.origin_y - board.h : board.origin_y};
    auto score = [&](Vec2 c) {
        int n = 0;
        for (auto& q : placements) if (q.x >= c.x - 0.01 && q.x <= c.x + board.w + 0.01 && q.y >= c.y - 0.01 && q.y <= c.y + board.h + 0.01) ++n;
        return n;
    };
    const Vec2 z{0, 0};
    return score(z) > score(a) ? z : a;
}
void PnpProgram::derive_circuits() {
    if (kind != PnpKind::Hlc) return;
    for (auto& q : placements) q.circuit = 0;
    const size_t K = circuits.size();
    if (K <= 1) return;
    std::map<std::string, int> seen, total;
    for (auto& q : placements) total[q.ref]++;
    bool all_k = true;
    for (auto& kv : total) if (kv.second != (int)K) all_k = false;
    if (!all_k) return;
    for (auto& q : placements) q.circuit = seen[q.ref]++;
}

// ===========================================================================
// public
// ===========================================================================
int pnp_sniff(const std::vector<uint8_t>& h) {
    if (h.size() >= 5) {
        std::string s(h.begin(), h.begin() + (long)std::min<size_t>(h.size(), 4096));
        if (s.find("<productionProgram") != std::string::npos) return 95;
        if (s.find("<?xml") != std::string::npos) return 0;
    }
    if (h.size() < 38 + 16 * 13) return 0;
    const uint32_t a0 = g32(h, 38);
    if (a0 != 38 + 16 * 26 && a0 != 38 + 16 * 27 && a0 != 38 + 16 * 28) return 0;
    const uint32_t prs = g32(h, 38 + 16 * 3 + 8);
    if (prs != 71 && prs != 96 && prs != 256) return 10;
    return 90;
}

bool pnp_load(const std::vector<uint8_t>& data, PnpProgram& out, std::string& err) {
    size_t i = 0;
    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) i = 3;
    while (i < data.size() && (data[i] == ' ' || data[i] == '\r' || data[i] == '\n' || data[i] == '\t')) ++i;
    if (i < data.size() && data[i] == '<') return load_iss(data, out, err);
    return load_hlc(data, out, err);
}

bool pnp_save(const PnpProgram& p, std::vector<uint8_t>& out, std::string& err, std::vector<std::string>* notes) {
    if (!p.src) { err = "no program loaded"; return false; }
    if (p.kind == PnpKind::Hlc) return save_hlc(p, out, err, notes);
    if (p.kind == PnpKind::Iss) return save_iss(p, out, err, notes);
    err = "unknown program kind";
    return false;
}

bool pnp_modified(const PnpProgram& p) {
    if (!p.src) return false;
    const PnpSource& S = *p.src;
    auto same_place = [](const PnpPlacement& a, const PnpPlacement& b) {
        return a.ref == b.ref && a.part == b.part && a.x == b.x && a.y == b.y && a.rot == b.rot && a.skip == b.skip && a.src == b.src;
    };
    auto same_part = [](const PnpPart& a, const PnpPart& b) {
        return a.name == b.name && a.desc == b.desc && a.len == b.len && a.wid == b.wid && a.hgt == b.hgt && a.skip == b.skip;
    };
    if (p.placements.size() != S.placements.size() || p.parts.size() != S.parts.size() || p.circuits.size() != S.circuits.size() ||
        p.marks.size() != S.marks.size())
        return true;
    for (size_t i = 0; i < p.placements.size(); ++i) if (!same_place(p.placements[i], S.placements[i])) return true;
    for (size_t i = 0; i < p.parts.size(); ++i) if (!same_part(p.parts[i], S.parts[i])) return true;
    for (size_t i = 0; i < p.circuits.size(); ++i)
        if (p.circuits[i].x != S.circuits[i].x || p.circuits[i].y != S.circuits[i].y || p.circuits[i].rot != S.circuits[i].rot) return true;
    for (size_t i = 0; i < p.marks.size(); ++i)
        if (p.marks[i].x != S.marks[i].x || p.marks[i].y != S.marks[i].y || p.marks[i].name != S.marks[i].name) return true;
    const PnpBoard &a = p.board, &b = S.board;
    return a.program_name != b.program_name || a.board_name != b.board_name || a.comment != b.comment || a.line_name != b.line_name ||
           a.w != b.w || a.h != b.h || a.origin_x != b.origin_x || a.origin_y != b.origin_y || a.ckt_w != b.ckt_w || a.ckt_h != b.ckt_h ||
           a.ckt_x != b.ckt_x || a.ckt_y != b.ckt_y || a.thick != b.thick || a.rl != b.rl || a.back != b.back || a.div_x != b.div_x ||
           a.div_y != b.div_y || a.pitch_x != b.pitch_x || a.pitch_y != b.pitch_y || a.first_x != b.first_x || a.first_y != b.first_y;
}

// ---- conversion ------------------------------------------------------------
std::vector<std::string> pnp_convert_targets(const PnpProgram& p) {
    std::vector<std::string> v;
    for (const char* f : {"H8H", "H7H", "H5H", "H4H"}) if (p.format != f) v.push_back(f);
    return v;
}

bool pnp_to_job(const PnpProgram& p, MachineJob& job, std::vector<std::string>& lossy) {
    job = MachineJob{};
    const PnpBoard& B = p.board;
    job.program_name = B.program_name.empty() ? B.board_name : B.program_name;
    job.assembly = B.board_name;
    job.board_w = B.w; job.board_h = B.h;
    const Vec2 c = p.board_corner();
    job.board_x0 = c.x; job.board_y0 = c.y;
    job.options["juki.board_x0"] = fmt4(c.x);
    job.options["juki.board_y0"] = fmt4(c.y);
    job.options["juki.transport"] = B.rl ? "RL" : "LR";
    job.options["juki.fixed_rail"] = B.back ? "back" : "front";
    job.options["juki.comment"] = B.comment;
    if (B.timestamp) job.options["juki.timestamp"] = std::to_string(B.timestamp);
    if (B.has_thick && B.thick > 0) job.board_t = B.thick;
    job.bottom_side = p.kind == PnpKind::Hlc && (B.mode & 0x60) == 0x40;
    if (B.ckt_w > 0) {
        job.has_circuit = true;
        job.circuit_w = B.ckt_w; job.circuit_h = B.ckt_h; job.circuit_x = B.ckt_x; job.circuit_y = B.ckt_y;
    }
    job.single_board = p.circuits.size() <= 1;
    if (!job.single_board) {
        // matrix vs non-matrix is re-derived by the writer (juki::plan_circuits)
        for (size_t i = 0; i < p.circuits.size(); ++i) {
            JobCircuit jc; jc.index = (int)i + 1; jc.x = p.circuits[i].x; jc.y = p.circuits[i].y; jc.rot = p.circuits[i].rot;
            // HLC circuit entries are the circuit-1 -> circuit-k map (HLC_FORMAT.md §5)
            jc.ax = p.circuits[i].x - p.circuits[0].x; jc.ay = p.circuits[i].y - p.circuits[0].y;
            jc.arot = p.circuits[i].rot - p.circuits[0].rot;
            job.circuits.push_back(jc);
        }
    }
    // parts
    std::map<int, int> used;
    for (auto& q : p.placements) if (q.part >= 0) used[q.part]++;
    for (size_t k = 0; k < p.parts.size(); ++k) {
        const PnpPart& pp = p.parts[k];
        JobPart jp;
        jp.part = pp.name; jp.description = pp.desc;
        jp.length = pp.len; jp.width = pp.wid; jp.height = pp.hgt;
        jp.count = used.count((int)k) ? used[(int)k] : 0;
        if (jp.count) job.parts.push_back(jp);
    }
    if (used.size() < p.parts.size()) lossy.push_back(std::to_string(p.parts.size() - used.size()) + " unused component record(s) dropped");
    int unlinked = 0;
    for (auto& q : p.placements) {
        JobPlacement jp;
        jp.ref = q.ref;
        jp.part = q.part >= 0 && q.part < (int)p.parts.size() ? p.parts[(size_t)q.part].name : std::string();
        if (jp.part.empty()) ++unlinked;
        jp.x = q.x; jp.y = q.y; jp.rot = norm_deg(q.rot);
        jp.circuit = q.circuit + 1;
        jp.skip = q.skip;
        job.placements.push_back(jp);
    }
    if (unlinked) lossy.push_back(std::to_string(unlinked) + " placement(s) without a component link");
    // marks
    int g = 0, bad = 0;
    std::map<int, std::vector<const PnpMark*>> sets;
    std::set<std::string> names;
    auto uniq = [&](std::string n) { std::string b = n.empty() ? "FID" : n; n = b; for (int k = 2; names.count(n); ++k) n = b + "_" + std::to_string(k); names.insert(n); return n; };
    bool global_set_taken = false;
    int first_global_set = -2;
    for (const PnpMark& m : p.marks) {
        if (m.kind == MarkKind::Bad) { ++bad; continue; }
        if (m.kind == MarkKind::Global) {
            if (first_global_set == -2) first_global_set = m.set;
            if (m.set != first_global_set) { global_set_taken = true; continue; }
            JobFiducial f; f.name = uniq(m.name); f.x = m.x; f.y = m.y; f.diameter = m.dia > 0 ? m.dia : 1.0; f.circuit = 0;
            job.fiducials.push_back(f);
            if (++g == 4) lossy.push_back("more than 3 global marks: the HLC writer keeps the first 3");
        } else sets[m.set].push_back(&m);
    }
    if (global_set_taken) lossy.push_back("per-machine copies of the global mark set merged into one");
    if (bad) lossy.push_back(std::to_string(bad) + " bad mark(s) dropped");
    for (auto& q : p.placements) {
        if (!q.has_local) continue;
        auto it = sets.find(q.mark_set);
        if (it == sets.end() || it->second.size() < 2) continue;
        std::string nm[2];
        for (int k = 0; k < 2; ++k) {
            const PnpMark& m = *it->second[(size_t)k];
            JobFiducial f; f.name = uniq(m.name.empty() ? q.ref + "_LM" + std::to_string(k + 1) : m.name);
            f.x = m.x; f.y = m.y; f.local = true; f.owner = q.ref; f.circuit = 0;
            job.fiducials.push_back(f);
            nm[k] = f.name;
        }
        job.options["juki.local." + q.ref] = nm[0] + "," + nm[1];
    }
    lossy.push_back("machine data not modelled by the editor (feeder / nozzle / vision / speed / station / MTC blocks, mark recognition) is replaced by writer defaults");
    return true;
}

bool pnp_convert(const PnpProgram& p, const std::string& target, std::vector<uint8_t>& out, std::vector<std::string>& lossy,
                 std::string& err) {
    MachineJob job;
    pnp_to_job(p, job, lossy);
    juki::HlcFormat f;
    if (target == "H8H") f = juki::HlcFormat::H8H;
    else if (target == "H7H") f = juki::HlcFormat::H7H;
    else if (target == "H5H") f = juki::HlcFormat::H5H;
    else if (target == "H4H") f = juki::HlcFormat::H4H;
    else { err = "cannot convert to " + target; return false; }
    for (auto& q : p.placements)
        if ((int)q.ref.size() > (target == "H8H" || target == "H7H" ? 19 : 10)) { lossy.push_back("long reference designators are truncated by " + target); break; }
    for (auto& c : p.parts)
        if ((int)c.name.size() > (target == "H7H" ? 60 : 20)) { lossy.push_back("part names longer than the " + target + " field are rejected or truncated"); break; }
    if (target != "H7H" && target != "H8H") lossy.push_back(target + " component records carry no body size");
    return juki::write_hlc(job, f, out, err);
}

// ---- checks ------------------------------------------------------------------
std::vector<PnpIssue> pnp_validate(const PnpProgram& p) {
    std::vector<PnpIssue> v;
    auto add = [&](int sev, int tab, int row, const std::string& s) { v.push_back({sev, tab, row, s}); };
    const PnpBoard& B = p.board;
    if (B.w <= 0 || B.h <= 0) add(1, 4, -1, "board size is not set");
    const Vec2 c = p.board_corner();
    const double tol = 0.01;
    std::map<std::pair<std::string, int>, int> seen;
    std::vector<int> uses(p.parts.size(), 0);
    int skipped = 0;
    for (size_t i = 0; i < p.placements.size(); ++i) {
        const PnpPlacement& q = p.placements[i];
        const int r = (int)i;
        if (q.ref.empty()) add(1, 0, r, "placement has no reference designator");
        else {
            auto key = std::make_pair(q.ref, q.circuit);
            auto it = seen.find(key);
            if (it != seen.end()) add(2, 0, r, "duplicate reference " + q.ref + " (also step " + std::to_string(it->second + 1) + ")");
            else seen[key] = r;
        }
        if ((int)q.ref.size() > p.ref_cap()) add(1, 0, r, q.ref + ": longer than " + std::to_string(p.ref_cap()) + " characters (truncated on save)");
        if (q.part < 0 || q.part >= (int)p.parts.size())   // skipped (e.g. x-out circuit): never placed, so only a warning
            add(q.skip ? 1 : 2, 0, r, (q.ref.empty() ? "placement" : q.ref) + ": no component (part) link" + (q.skip ? " (skipped)" : ""));
        else uses[(size_t)q.part]++;
        if (B.w > 0 && B.h > 0 && (q.x < c.x - tol || q.y < c.y - tol || q.x > c.x + B.w + tol || q.y > c.y + B.h + tol))
            add(1, 0, r, q.ref + ": outside the board");
        if (q.rot < 0 || q.rot >= 360) add(0, 0, r, q.ref + ": angle " + fmt_num(q.rot) + " is outside 0..360");
        else if (std::fabs(q.rot * 80 - std::round(q.rot * 80)) > 1e-6) add(0, 0, r, q.ref + ": angle " + fmt_num(q.rot) + " is not a multiple of 0.0125 (rounded on save)");
        if (q.status & 0x20) add(0, 0, r, q.ref + ": flagged by the machine software as outside its circuit");
        if (q.skip) ++skipped;
        if (q.has_local) {
            int n = 0;
            for (auto& m : p.marks) if (m.kind == MarkKind::Local && m.set == q.mark_set) ++n;
            if (n < 2) add(2, 0, r, q.ref + ": local mark pair is missing");
        }
    }
    if (skipped) add(0, 0, -1, std::to_string(skipped) + " placement(s) are skipped");
    std::map<std::string, int> names;
    for (size_t k = 0; k < p.parts.size(); ++k) {
        const PnpPart& q = p.parts[k];
        if (q.name.empty()) add(2, 1, (int)k, "component record " + std::to_string(k) + " has no name");
        auto it = names.find(q.name);
        if (!q.name.empty() && it != names.end()) add(2, 1, (int)k, "duplicate part name " + q.name);
        names[q.name] = (int)k;
        if ((int)q.name.size() > p.part_cap()) add(1, 1, (int)k, q.name + ": longer than " + std::to_string(p.part_cap()) + " characters");
        if (!uses[k]) add(0, 1, (int)k, q.name + ": not used by any placement");
        if (q.skip && uses[k]) add(0, 1, (int)k, q.name + ": component skip is on (" + std::to_string(uses[k]) + " placements not placed)");
    }
    int glob = 0;
    int first_set = -2;
    for (size_t i = 0; i < p.marks.size(); ++i) {
        const PnpMark& m = p.marks[i];
        if (m.kind == MarkKind::Global) {
            if (first_set == -2) first_set = m.set;
            if (m.set == first_set) ++glob;
        }
        if (B.w > 0 && B.h > 0 && (m.x < c.x - tol || m.y < c.y - tol || m.x > c.x + B.w + tol || m.y > c.y + B.h + tol))
            add(1, 3, (int)i, m.name + ": mark outside the board");
    }
    if (glob == 0) add(1, 3, -1, "no global marks (board offset correction disabled)");
    else if (glob == 1) add(1, 3, -1, "only 1 global mark: rotation cannot be corrected (2 or 3 recommended)");
    else if (glob > 3) add(0, 3, -1, std::to_string(glob) + " global marks (HLC uses at most 3)");
    for (size_t i = 0; i < p.circuits.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (std::fabs(p.circuits[i].x - p.circuits[j].x) < 1e-3 && std::fabs(p.circuits[i].y - p.circuits[j].y) < 1e-3 &&
                std::fabs(p.circuits[i].rot - p.circuits[j].rot) < 1e-6) {
                add(1, 2, (int)i, "circuit " + std::to_string(i + 1) + " sits on circuit " + std::to_string(j + 1));
                break;
            }
    std::stable_sort(v.begin(), v.end(), [](const PnpIssue& a, const PnpIssue& b) { return a.severity > b.severity; });
    return v;
}

std::string pnp_csv(const PnpProgram& p) {
    auto q = [](const std::string& s) {
        if (s.find_first_of(",\"\n") == std::string::npos) return s;
        std::string o = "\"";
        for (char c : s) { if (c == '"') o += '"'; o += c; }
        return o + "\"";
    };
    std::string o = "Step,Ref,Part,Description,X,Y,Angle,Circuit,Skip,Station\r\n";
    for (size_t i = 0; i < p.placements.size(); ++i) {
        const PnpPlacement& r = p.placements[i];
        const PnpPart* k = r.part >= 0 && r.part < (int)p.parts.size() ? &p.parts[(size_t)r.part] : nullptr;
        o += std::to_string(i + 1) + "," + q(r.ref) + "," + q(k ? k->name : "") + "," + q(k ? k->desc : "") + "," + fmt4(r.x) + "," +
             fmt4(r.y) + "," + fmt_num(r.rot) + "," + std::to_string(r.circuit + 1) + "," + (r.skip ? "1" : "0") + "," +
             std::to_string(r.station) + "\r\n";
    }
    return o;
}

bool pnp_circuit_xform(const PnpProgram& p, int k, Vec2& off, double& rot) {
    if (k < 0 || k >= (int)p.circuits.size()) return false;
    off = {p.circuits[(size_t)k].x - p.circuits[0].x, p.circuits[(size_t)k].y - p.circuits[0].y};
    rot = p.circuits[(size_t)k].rot - p.circuits[0].rot;
    return true;
}

std::vector<PnpFrame> pnp_circuit_frames(const PnpProgram& p) {
    std::vector<PnpFrame> v;
    const PnpBoard& B = p.board;
    if (B.ckt_w <= 0 || B.ckt_h <= 0) return v;
    const Vec2 base[4] = {{B.ckt_x, B.ckt_y}, {B.ckt_x + B.ckt_w, B.ckt_y}, {B.ckt_x + B.ckt_w, B.ckt_y + B.ckt_h}, {B.ckt_x, B.ckt_y + B.ckt_h}};
    const size_t K = std::max<size_t>(1, p.circuits.size());
    for (size_t k = 0; k < K; ++k) {
        PnpFrame f;
        f.index = (int)k;
        Vec2 off{0, 0};
        double rot = 0;
        if (!p.circuits.empty()) pnp_circuit_xform(p, (int)k, off, rot);
        for (int i = 0; i < 4; ++i) f.corners[i] = rotate(base[i], rot) + off;
        v.push_back(f);
    }
    return v;
}

const char* pnp_mark_kind_name(MarkKind k) {
    switch (k) { case MarkKind::Global: return "Global"; case MarkKind::Local: return "Local"; case MarkKind::Bad: return "Bad mark"; }
    return "?";
}

bool pnp_is_program_ext(const std::string& e) {
    static const char* x[] = {"h8h", "h7h", "h5h", "h4h", "e41", "e42", "e43", "e44", "e45", "e46", "e47", "e48", "e51", "iss", "x01", "x52", "s01x"};
    for (auto* s : x) if (e == s) return true;
    return false;
}

} // namespace pcbp::io
