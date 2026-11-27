// ============================================================================
// cc_archive.cpp — CircuitCAM graphics blob decoder (see cc_archive.h and
// specs/CPF_FORMAT.md).  Portable C++20, no OS headers.
// ============================================================================
#include "cc_archive.h"
#include "inflate.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace pcbp::cc {
namespace {

// ---------------------------------------------------------------------------
// Little-endian reader with MFC CArchive helpers
// ---------------------------------------------------------------------------
struct Reader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    std::string err;
    std::string ver;                    // "current archive version" (global in the original)
    std::vector<int> load;              // MFC load map: 0 = NULL slot, 1 = class, 2 = object
    std::vector<std::string> load_cls;  // class name for class slots
    int depth = 0;

    bool fail(const std::string& m) {
        if (err.empty()) err = m + " (at byte " + std::to_string(pos) + ")";
        return false;
    }
    bool need(size_t k) { return k <= n - pos ? true : fail("read past end"); }
    bool u8(uint8_t& v) { if (!need(1)) return false; v = p[pos++]; return true; }
    bool u16(uint16_t& v) {
        if (!need(2)) return false;
        v = uint16_t(p[pos] | (p[pos + 1] << 8)); pos += 2; return true;
    }
    bool u32(uint32_t& v) {
        if (!need(4)) return false;
        v = uint32_t(p[pos]) | (uint32_t(p[pos + 1]) << 8) | (uint32_t(p[pos + 2]) << 16) |
            (uint32_t(p[pos + 3]) << 24);
        pos += 4; return true;
    }
    bool i32(int32_t& v) { uint32_t u; if (!u32(u)) return false; v = int32_t(u); return true; }
    bool f64(double& v) {
        if (!need(8)) return false;
        uint64_t u = 0;
        for (int i = 7; i >= 0; --i) u = (u << 8) | p[pos + i];
        std::memcpy(&v, &u, 8); pos += 8; return true;
    }
    bool raw(size_t k, std::vector<uint8_t>& out) {
        if (!need(k)) return false;
        out.assign(p + pos, p + pos + k); pos += k; return true;
    }
    bool skip(size_t k) { if (!need(k)) return false; pos += k; return true; }

    // CArchive::ReadCount
    bool count(uint32_t& c) {
        uint16_t w;
        if (!u16(w)) return false;
        if (w != 0xFFFF) { c = w; return true; }
        if (!u32(c)) return false;
        if (c == 0xFFFFFFFFu) return fail("64-bit counts not supported");
        return true;
    }

    // CString (AfxReadStringLength): ANSI or UTF-16 (0xFF 0xFFFE marker) -> UTF-8
    bool cstring(std::string& s) {
        s.clear();
        bool wide = false;
        uint8_t b;
        uint16_t w;
        uint32_t len;
        if (!u8(b)) return false;
        len = b;
        if (b == 0xFF) {
            if (!u16(w)) return false;
            len = w;
            if (w == 0xFFFE) {
                wide = true;
                if (!u8(b)) return false;
                len = b;
                if (b == 0xFF) {
                    if (!u16(w)) return false;
                    len = w;
                    if (w == 0xFFFF && !u32(len)) return false;
                }
            } else if (w == 0xFFFF) {
                if (!u32(len)) return false;
            }
        }
        if (!wide) {
            if (!need(len)) return false;
            for (uint32_t i = 0; i < len; ++i) append_utf8(s, cp1252(p[pos + i]));
            pos += len;
            return true;
        }
        if (len > (n - pos) / 2) return fail("string past end");
        for (uint32_t i = 0; i < len; ++i) {
            uint32_t c = p[pos] | (p[pos + 1] << 8);
            pos += 2;
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < len) {
                const uint32_t lo = p[pos] | (p[pos + 1] << 8);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    pos += 2; ++i;
                }
            }
            append_utf8(s, c);
        }
        return true;
    }

    static uint32_t cp1252(uint8_t c) {
        static const uint16_t hi[32] = {
            0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
            0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
            0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
        return (c >= 0x80 && c < 0xA0) ? hi[c - 0x80] : c;
    }
    static void append_utf8(std::string& s, uint32_t c) {
        if (c < 0x80) s += char(c);
        else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F));
        } else {
            s += char(0xF0 | (c >> 18)); s += char(0x80 | ((c >> 12) & 0x3F));
            s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F));
        }
    }
};

// CString::Compare on short "x.y" version strings is a plain lexicographic
// compare; reproduce that (it is what the original does, quirks included).
inline bool ver_ge(const std::string& v, const char* w) { return v.compare(w) >= 0; }
inline bool ver_lt(const std::string& v, const char* w) { return v.compare(w) < 0; }

GrType type_of(const std::string& cls) {
    static const struct { const char* n; GrType t; } map[] = {
        {"CGrComp", GrType::Comp}, {"CGrGroup", GrType::Group}, {"CGrPolygon", GrType::Polygon},
        {"CGrRect", GrType::Rect}, {"CGrLine", GrType::Line}, {"CGrPolyLine", GrType::PolyLine},
        {"CGrEllipse", GrType::Ellipse}, {"CGrBitmap", GrType::Bitmap}, {"CGrArc", GrType::Arc},
        {"CGrText", GrType::Text}, {"CGrHandles", GrType::Handles},
        {"CGrSelection", GrType::Selection}, {"CGrBomBlock", GrType::BomBlock},
        {"CGrOleItem", GrType::OleItem}, {"CGrPolyArcLine", GrType::PolyArcLine},
        {"CGrEnhMetaFile", GrType::EnhMetaFile}, {"CGrBomCol", GrType::BomCol},
        {"CGrBomEntry", GrType::BomEntry}, {"CGrBomOb", GrType::BomOb}};
    for (const auto& m : map)
        if (cls == m.n) return m.t;
    return GrType::Unknown;
}

const char* class_of(int32_t id) {
    static const char* names[] = {nullptr, "CGrComp", "CGrGroup", "CGrPolygon", "CGrRect",
                                  "CGrLine", "CGrPolyLine", "CGrEllipse", "CGrBitmap", "CGrArc",
                                  "CGrText", "CGrHandles", "CGrSelection", "CGrBomBlock",
                                  "CGrOleItem", "CGrPolyArcLine", "CGrEnhMetaFile"};
    return (id >= 1 && id <= 16) ? names[id] : nullptr;
}

bool read_body(Reader& r, GrObject& o);
bool read_object(Reader& r, std::unique_ptr<GrObject>& out);

bool read_logfont(Reader& r, const std::string& ver, GrLogFont& f) {
    if (ver_ge(ver, "1.9") && !r.i32(f.size)) return false;
    if (!r.i32(f.height) || !r.i32(f.width) || !r.i32(f.escapement) || !r.i32(f.orientation) ||
        !r.i32(f.weight))
        return false;
    uint8_t b[8];
    for (auto& x : b)
        if (!r.u8(x)) return false;
    f.italic = b[0]; f.underline = b[1]; f.strikeout = b[2]; f.charset = b[3];
    f.out_prec = b[4]; f.clip_prec = b[5]; f.quality = b[6]; f.pitch_family = b[7];
    if (ver_lt(ver, "1.9")) {                       // fixed char[32] face name
        if (!r.need(32)) return false;
        f.face.clear();
        for (int i = 0; i < 32 && r.p[r.pos + i]; ++i) Reader::append_utf8(f.face, Reader::cp1252(r.p[r.pos + i]));
        r.pos += 32;
        return true;
    }
    return r.cstring(f.face);
}

// CGrComp::Serialize — common base of every drawable
bool read_base(Reader& r, GrObject& o) {
    if (!r.cstring(r.ver)) return false;
    o.ver = r.ver;
    for (auto& l : o.labels)
        if (!r.i32(l)) return false;
    const int nstr = ver_lt(r.ver, "1.3") ? 3 : 4;
    o.strs.resize(size_t(nstr));
    for (auto& s : o.strs)
        if (!r.cstring(s)) return false;
    int32_t* a[22] = {&o.attr.pen_size, &o.attr.pen_color, &o.attr.pen_style, &o.attr.brush_color,
                      &o.attr.brush_style, &o.attr.hatch_style, &o.attr.bk_mode, &o.attr.bk_color,
                      &o.attr.flag96, &o.attr.own_pen, &o.attr.own_brush, &o.attr.printable,
                      &o.attr.visible, &o.attr.fixed_aspect, &o.attr.scalable_pen,
                      &o.attr.selectable, &o.attr.moveable, &o.attr.sizeable,
                      &o.attr.select_width, &o.attr.handle_size, &o.attr.select_style,
                      &o.attr.select_color};
    for (auto* v : a)
        if (!r.i32(*v)) return false;
    const bool dbl = ver_ge(r.ver, "2.3");
    if (dbl) {
        if (!r.f64(o.org.x) || !r.f64(o.org.y)) return false;
    } else {
        int32_t x, y;
        if (!r.i32(x) || !r.i32(y)) return false;
        o.org = {double(x), double(y)};
    }
    if (!r.f64(o.scale.x) || !r.f64(o.scale.y)) return false;
    if (dbl) {
        if (!r.f64(o.rot)) return false;
    } else {
        int32_t rr;
        if (!r.i32(rr)) return false;
        o.rot = rr;
    }
    int32_t np;
    if (!r.i32(np)) return false;
    if (np < 0 || size_t(np) > (r.n - r.pos) / 8) return r.fail("bad point count");
    o.pts.resize(size_t(np));
    for (auto& v : o.pts) {
        if (dbl) {
            if (!r.f64(v.x) || !r.f64(v.y)) return false;
        } else {
            int32_t x, y;
            if (!r.i32(x) || !r.i32(y)) return false;
            v = {double(x), double(y)};
        }
    }
    return true;
}

bool read_group(Reader& r, GrObject& o) {
    if (!read_base(r, o)) return false;
    if (ver_lt(r.ver, "1.8")) {                     // CObList::Serialize
        uint32_t c;
        if (!r.count(c)) return false;
        for (uint32_t i = 0; i < c; ++i) {
            std::unique_ptr<GrObject> ch;
            if (!read_object(r, ch)) return false;
            if (ch) o.children.push_back(std::move(ch));
        }
    } else {                                        // count + (type id, body)*
        int32_t c;
        if (!r.i32(c)) return false;
        if (c < 0 || size_t(c) > r.n - r.pos) return r.fail("bad child count");
        for (int32_t i = 0; i < c; ++i) {
            int32_t id;
            if (!r.i32(id)) return false;
            const char* cn = class_of(id);
            if (!cn) return r.fail("unknown child type id " + std::to_string(id));
            auto ch = std::make_unique<GrObject>();
            ch->cls = cn;
            ch->type = type_of(ch->cls);
            if (!read_body(r, *ch)) return false;
            o.children.push_back(std::move(ch));
        }
    }
    return r.i32(o.group_tail);
}

bool read_bytearray(Reader& r, std::vector<uint8_t>& v) {
    uint32_t c;
    return r.count(c) && r.raw(c, v);
}

bool read_bomob(Reader& r, GrBomOb& b) {
    return r.cstring(b.name) && r.cstring(b.value) && r.i32(b.a) && r.i32(b.b) && r.i32(b.c) &&
           r.i32(b.d);
}

bool read_obarray(Reader& r, GrObject& o) {
    uint32_t c;
    if (!r.count(c)) return false;
    for (uint32_t i = 0; i < c; ++i) {
        std::unique_ptr<GrObject> ch;
        if (!read_object(r, ch)) return false;
        if (ch) o.children.push_back(std::move(ch));
    }
    return true;
}

bool read_body(Reader& r, GrObject& o) {
    if (++r.depth > 64) return r.fail("nesting too deep");
    bool ok = false;
    switch (o.type) {
        case GrType::Comp:
        case GrType::Ellipse:
        case GrType::Arc:
            ok = read_base(r, o);
            break;
        case GrType::Group:
            ok = read_group(r, o);
            break;
        case GrType::Polygon:
        case GrType::Rect:
        case GrType::Line:
        case GrType::PolyLine:
        case GrType::Handles:
            ok = read_base(r, o) && r.i32(o.closed) && r.i32(o.arrow1) && r.i32(o.arrow0);
            if (ok && o.type == GrType::Handles) ok = r.i32(o.handles_tail);
            break;
        case GrType::PolyArcLine: {
            uint8_t c = 0;
            int32_t k = 0;
            ok = read_base(r, o) && r.u8(c) && r.i32(k);
            if (ok) {
                o.closed = c;
                if (k < 0) ok = r.fail("bad segment count");
                else if (k > 0) ok = r.raw(size_t(k), o.seg_types);
            }
            break;
        }
        case GrType::Bitmap:
            ok = read_base(r, o);
            if (ok) {
                o.bmp_mode = 2;
                if (ver_ge(r.ver, "1.6")) ok = r.i32(o.bmp_mode);
                ok = ok && read_bytearray(r, o.data);
            }
            break;
        case GrType::EnhMetaFile:
            ok = read_base(r, o) && read_bytearray(r, o.data2) && read_bytearray(r, o.data);
            break;
        case GrType::Text: {
            ok = read_base(r, o) && r.cstring(o.text);
            for (auto& v : o.text_i) ok = ok && r.i32(v);
            ok = ok && r.cstring(o.url);
            const std::string v = r.ver;
            ok = ok && read_logfont(r, v, o.font);
            if (ok && ver_ge(v, "1.1")) ok = r.i32(o.text_h552);
            if (ok && ver_ge(v, "2.1")) ok = r.f64(o.text_d576);
            break;
        }
        case GrType::BomOb: {
            GrBomOb b;
            ok = read_bomob(r, b);
            if (ok) { o.bom_obs.push_back(b); o.text = b.name; }
            break;
        }
        case GrType::BomCol: {
            std::string v;
            int32_t a = 0, b = 0;
            std::string field, title;
            GrLogFont f;
            ok = r.cstring(v) && r.i32(a) && r.cstring(field) && r.cstring(title) &&
                 read_logfont(r, v, f) && r.i32(b);
            if (ok) {
                o.ver = v; o.bom_ints = {a, b}; o.bom_strs = {field, title}; o.fonts = {f};
            }
            break;
        }
        case GrType::BomEntry: {
            std::string v;
            GrLogFont f;
            int32_t a = 0, b = 0;
            ok = r.cstring(v) && read_obarray(r, o) && read_logfont(r, v, f) && r.i32(a) && r.i32(b);
            if (ok) { o.ver = v; o.fonts = {f}; o.bom_ints = {a, b}; }
            break;
        }
        case GrType::BomBlock: {
            ok = read_group(r, o);
            if (!ok) break;
            const std::string v = r.ver;            // global version after the children (as original)
            int32_t rc[4];
            for (auto& x : rc) ok = ok && r.i32(x);
            o.bom_ints.assign(rc, rc + 4);
            for (int i = 0; i < 3 && ok; ++i) { std::string s; ok = r.cstring(s); o.bom_strs.push_back(s); }
            for (int i = 0; i < 5 && ok; ++i) { GrLogFont f; ok = read_logfont(r, v, f); o.fonts.push_back(f); }
            for (int i = 0; i < 7 && ok; ++i) { int32_t x; ok = r.i32(x); o.bom_ints.push_back(x); }
            if (ok) { std::string s; ok = r.cstring(s); o.bom_strs.push_back(s); }
            for (int i = 0; i < 4 && ok; ++i) { int32_t x; ok = r.i32(x); o.bom_ints.push_back(x); }
            for (int i = 0; i < 2 && ok; ++i) { GrBomOb b; ok = read_bomob(r, b); o.bom_obs.push_back(b); }
            for (int i = 0; i < 3 && ok; ++i) ok = read_obarray(r, o);     // columns, entries, misc
            if (ok) {
                uint32_t c;
                ok = r.count(c) && (c <= (r.n - r.pos) / 4 || r.fail("bad dword count"));
                for (uint32_t i = 0; ok && i < c; ++i) { uint32_t d; ok = r.u32(d); o.bom_dwords.push_back(d); }
            }
            if (ok && ver_ge(v, "1.2")) {
                uint32_t c;
                ok = r.count(c);
                for (uint32_t i = 0; ok && i < c; ++i) { std::string s; ok = r.cstring(s); o.bom_strs.push_back(s); }
            }
            if (ok && ver_ge(v, "1.1"))
                for (int i = 0; i < 5 && ok; ++i) { int32_t x; ok = r.i32(x); o.bom_ints.push_back(x); }
            for (const char* w : {"1.4", "1.5", "2.0"})
                if (ok && ver_ge(v, w)) { int32_t x; ok = r.i32(x); o.bom_ints.push_back(x); }
            break;
        }
        case GrType::Selection:
        case GrType::OleItem:
        default:
            ok = r.fail("unsupported class " + o.cls);
            break;
    }
    --r.depth;
    return ok;
}

// CArchive::ReadObject
bool read_object(Reader& r, std::unique_ptr<GrObject>& out) {
    if (r.load.empty()) { r.load.push_back(0); r.load_cls.emplace_back(); }
    uint16_t tag;
    if (!r.u16(tag)) return false;
    uint32_t idx;
    bool is_class;
    if (tag == 0x7FFF) {                            // big tag
        uint32_t big;
        if (!r.u32(big)) return false;
        is_class = (big & 0x80000000u) != 0;
        idx = big & 0x7FFFFFFFu;
    } else {
        is_class = (tag & 0x8000) != 0;
        idx = tag & 0x7FFF;
    }
    std::string cls;
    if (tag == 0xFFFF) {                            // new class: schema, name
        uint16_t schema, len;
        if (!r.u16(schema) || !r.u16(len) || !r.need(len)) return false;
        cls.assign(reinterpret_cast<const char*>(r.p + r.pos), len);
        r.pos += len;
        r.load.push_back(1);
        r.load_cls.push_back(cls);
    } else if (is_class) {
        if (idx >= r.load.size() || r.load[idx] != 1) return r.fail("bad class reference");
        cls = r.load_cls[idx];
    } else {
        if (idx == 0) { out.reset(); return true; } // NULL
        if (idx >= r.load.size() || r.load[idx] != 2) return r.fail("bad object reference");
        out = std::make_unique<GrObject>();
        out->ref_index = int32_t(idx);
        out->cls = "<ref>";
        return true;
    }
    r.load.push_back(2);                            // object slot registered before its body
    r.load_cls.emplace_back();
    out = std::make_unique<GrObject>();
    out->cls = cls;
    out->type = type_of(cls);
    if (out->type == GrType::Unknown) return r.fail("unknown class " + cls);
    return read_body(r, *out);
}

// ---------------------------------------------------------------------------
// GPrim conversion
// ---------------------------------------------------------------------------
Vec2 mm(Vec2 v) { return {v.x * kMmPerUnit, v.y * kMmPerUnit}; }

BBox raw_bbox(const std::vector<Vec2>& pts, size_t count) {
    BBox b;
    for (size_t i = 0; i < pts.size() && i < count; ++i) b.add(pts[i]);
    return b;
}

void style(const GrObject& o, GPrim& g) {
    const auto& a = o.attr;
    g.filled = a.own_brush && a.brush_style == 0;          // BS_SOLID
    if (a.own_pen) g.color = colorref_to_argb(a.pen_color);
    else if (g.filled) g.color = colorref_to_argb(a.brush_color);
    if (a.own_pen && a.pen_size > 1 && a.scalable_pen) g.width = a.pen_size * kMmPerUnit;
}

bool is_axis_rect(const std::vector<Vec2>& p) {
    if (p.size() != 4) return false;
    auto eq = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    return (eq(p[0].y, p[1].y) && eq(p[1].x, p[2].x) && eq(p[2].y, p[3].y) && eq(p[3].x, p[0].x)) ||
           (eq(p[0].x, p[1].x) && eq(p[1].y, p[2].y) && eq(p[2].x, p[3].x) && eq(p[3].y, p[0].y));
}

void ellipse_prims(const GrObject& o, GPrim& g) {
    const BBox b = raw_bbox(o.pts, 4);
    const Vec2 c = mm(b.center());
    const double rx = b.w() * 0.5 * kMmPerUnit, ry = b.h() * 0.5 * kMmPerUnit;
    if (std::fabs(rx - ry) <= 1e-6 * std::max(1.0, rx)) {
        g.kind = GKind::Circle;
        g.pts = {c};
        g.radius = rx;
        return;
    }
    g.kind = GKind::Polygon;                         // approximate a true ellipse
    const int seg = 48;
    for (int i = 0; i < seg; ++i) {
        const double t = 2 * kPi * i / seg;
        g.pts.push_back({c.x + rx * std::cos(t), c.y + ry * std::sin(t)});
    }
}

} // namespace

// ---------------------------------------------------------------------------
bool unwrap_blob(const uint8_t* p, size_t n, std::vector<uint8_t>& payload, std::string& err) {
    auto le32 = [p](size_t o) {
        return uint32_t(p[o]) | (uint32_t(p[o + 1]) << 8) | (uint32_t(p[o + 2]) << 16) |
               (uint32_t(p[o + 3]) << 24);
    };
    bool wrapped = n >= 32 && std::memcmp(p, "CC_COMPRESS", 11) == 0;
    for (int i = 11; wrapped && i < 22; ++i) wrapped = p[i] == 0;
    if (!wrapped) {                                  // stored uncompressed
        payload.assign(p, p + n);
        return true;
    }
    // +22 u16 header version (1), +24 u32 inflate buffer size (= raw size + 4),
    // +28 u32 compressed size, +32 raw DEFLATE
    const uint32_t bufsize = le32(24), csize = le32(28);
    if (csize > n - 32) { err = "CC_COMPRESS: compressed size exceeds blob"; return false; }
    const size_t hint = bufsize >= 4 ? bufsize - 4 : 0;
    const size_t cap = hint ? hint + 4096 : (size_t(1) << 30);
    if (!inflate_raw(p + 32, csize, payload, hint, err, nullptr, cap)) return false;
    if (bufsize && payload.size() + 4 != bufsize) {
        err = "CC_COMPRESS: size mismatch (" + std::to_string(payload.size()) + "+4 != " +
              std::to_string(bufsize) + ")";
        return false;
    }
    return true;
}

bool parse_archive(const uint8_t* p, size_t n, GrObject& root, std::string& err, const char* root_class) {
    static const char* const candidates[] = {"CGrGroup", "CGrBitmap", "CGrEnhMetaFile", "CGrText",
                                             "CGrRect", "CGrPolygon", "CGrBomBlock"};
    std::string first_err;
    const char* const* list = root_class ? &root_class : candidates;
    const size_t cnt = root_class ? 1 : sizeof(candidates) / sizeof(candidates[0]);
    for (size_t i = 0; i < cnt; ++i) {
        Reader r{p, n, 0, {}, {}, {}, {}, 0};
        GrObject o;
        o.cls = list[i];
        o.type = type_of(o.cls);
        if (o.type == GrType::Unknown) { err = "unknown root class"; return false; }
        if (!read_body(r, o)) {
            if (first_err.empty()) first_err = o.cls + ": " + r.err;
            continue;
        }
        if (r.pos != n) {
            if (first_err.empty())
                first_err = o.cls + ": " + std::to_string(n - r.pos) + " trailing bytes";
            continue;
        }
        root = std::move(o);
        return true;
    }
    err = first_err;
    return false;
}

bool decode_blob(const uint8_t* p, size_t n, GrObject& root, std::string& err, const char* root_class) {
    std::vector<uint8_t> payload;
    if (!unwrap_blob(p, n, payload, err)) return false;
    return parse_archive(payload.data(), payload.size(), root, err, root_class);
}

void to_gprims(const GrObject& o, std::vector<GPrim>& out) {
    if (o.ref_index || !o.attr.visible) return;
    GPrim g;
    style(o, g);
    g.rot = o.rot;
    switch (o.type) {
        case GrType::Group:
        case GrType::BomBlock:
        case GrType::BomEntry:
            g.kind = GKind::Group;
            for (const auto& c : o.children) to_gprims(*c, g.children);
            if (o.type == GrType::BomBlock && o.pts.size() >= 2) {
                // BOM block: cell contents are generated at print time from the
                // project BOM; keep its frame and title so the layout is visible.
                const BBox b = raw_bbox(o.pts, o.pts.size());
                GPrim frame;
                frame.kind = GKind::Rect;
                frame.pts = {mm({b.x0, b.y0}), mm({b.x1, b.y1})};
                g.children.push_back(frame);
                if (!o.bom_strs.empty() && !o.bom_strs[0].empty()) {
                    GPrim t;
                    t.kind = GKind::Text;
                    const double th = std::min(b.h(), 150.0);
                    t.pts = {mm({b.x0, b.y1 - th})};
                    t.text_h = th * kMmPerUnit;
                    t.text = o.bom_strs[0];
                    g.children.push_back(t);
                }
            }
            if (g.children.empty()) return;
            break;
        case GrType::Rect:
        case GrType::Polygon:
        case GrType::PolyLine:
        case GrType::Line:
        case GrType::PolyArcLine:
        case GrType::Handles:
            if (o.pts.size() < 2) return;
            if (o.type == GrType::Rect && is_axis_rect(o.pts)) {
                const BBox b = raw_bbox(o.pts, 4);
                g.kind = GKind::Rect;
                g.pts = {mm({b.x0, b.y0}), mm({b.x1, b.y1})};
                g.rot = 0;
                break;
            }
            for (const auto& v : o.pts) g.pts.push_back(mm(v));
            if (o.type == GrType::Rect || o.closed) g.kind = GKind::Polygon;
            else g.kind = g.pts.size() == 2 ? GKind::Line : GKind::Polyline;
            if (g.kind != GKind::Polygon) g.filled = false;
            break;
        case GrType::Ellipse:
            if (o.pts.size() < 4) return;
            ellipse_prims(o, g);
            break;
        case GrType::Arc: {
            // INFERRED: pts[0..3] bounding box, pts[4] start, pts[5] end (GDI Arc style)
            if (o.pts.size() < 6) return;
            const BBox b = raw_bbox(o.pts, 4);
            const Vec2 c = b.center();
            g.kind = GKind::Arc;
            g.pts = {mm(c)};
            g.radius = 0.25 * (b.w() + b.h()) * kMmPerUnit;
            const double s = rad2deg(std::atan2(o.pts[4].y - c.y, o.pts[4].x - c.x));
            const double e = rad2deg(std::atan2(o.pts[5].y - c.y, o.pts[5].x - c.x));
            g.a0 = norm_deg(s);
            g.a1 = norm_deg(e - s);
            if (g.a1 == 0) g.a1 = 360;
            g.filled = false;
            break;
        }
        case GrType::Text: {
            if (o.pts.empty()) return;
            const BBox b = raw_bbox(o.pts, o.pts.size());
            g.kind = GKind::Text;
            g.pts = {mm({b.x0, b.y0})};
            g.text_h = b.h() * kMmPerUnit;
            g.text = o.text;
            g.filled = false;
            break;
        }
        default:
            return;     // bitmaps, metafiles, OLE items, BOM cells
    }
    out.push_back(std::move(g));
}

bool decode_graphics(const uint8_t* p, size_t n, std::vector<GPrim>& out, std::string& err) {
    GrObject root;
    if (!decode_blob(p, n, root, err)) return false;
    out.clear();
    if (root.type == GrType::Group) {
        for (const auto& c : root.children) to_gprims(*c, out);
    } else {
        to_gprims(root, out);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Packages.Rendering
// ---------------------------------------------------------------------------
namespace {

bool pad_from_shape(const GrObject& s, Pad& pad) {
    if (s.pts.size() < 2) return false;
    const BBox b = raw_bbox(s.pts, s.type == GrType::Ellipse || s.type == GrType::Arc ? 4 : s.pts.size());
    pad.pos = mm(b.center());
    pad.w = b.w() * kMmPerUnit;
    pad.h = b.h() * kMmPerUnit;
    pad.rot = 0;
    switch (s.type) {
        case GrType::Ellipse:
            pad.shape = std::fabs(pad.w - pad.h) < 1e-6 ? PadShape::Round : PadShape::Oblong;
            break;
        case GrType::Rect:
        case GrType::Polygon:
            if (s.pts.size() == 8) { pad.shape = PadShape::Octagon; break; }
            if (s.pts.size() == 4 && !is_axis_rect(s.pts)) {
                // rotated rectangle: size from edges, angle from first edge
                const Vec2 e1 = s.pts[1] - s.pts[0], e2 = s.pts[3] - s.pts[0];
                pad.w = e1.len() * kMmPerUnit;
                pad.h = e2.len() * kMmPerUnit;
                pad.rot = norm_deg(rad2deg(std::atan2(e1.y, e1.x)));
            }
            pad.shape = std::fabs(pad.w - pad.h) < 1e-6 ? PadShape::Square : PadShape::Rect;
            break;
        default:
            pad.shape = PadShape::Rect;
            break;
    }
    return true;
}

const GrObject* first_shape(const GrObject& g) {
    for (const auto& c : g.children) {
        if (c->ref_index) continue;
        if (c->type == GrType::Group) {
            if (const GrObject* s = first_shape(*c)) return s;
        } else if (c->pts.size() >= 2) {
            return c.get();
        }
    }
    return nullptr;
}

void collect_pads(const GrObject& g, PackageRendering& r) {
    for (const auto& c : g.children) {
        if (c->ref_index) continue;
        const GrObject* shape = nullptr;
        int pin = c->labels[kLabelPin];
        if (c->type == GrType::Group) shape = first_shape(*c);
        else if (c->pts.size() >= 2) shape = c.get();
        if (!shape) continue;
        if (pin == 0) pin = shape->labels[kLabelPin];
        Pad pad;
        if (!pad_from_shape(*shape, pad)) continue;
        pad.pin = pin;
        r.pads.push_back(pad);
        to_gprims(*c, r.pad_shapes);
    }
}

} // namespace

bool decode_package_rendering(const uint8_t* p, size_t n, PackageRendering& r, std::string& err) {
    GrObject root;
    if (!decode_blob(p, n, root, err, "CGrGroup")) return false;
    r = PackageRendering{};
    for (const auto& c : root.children) {
        if (c->ref_index) continue;
        switch (c->labels[kLabelRole]) {
            case kRolePads: collect_pads(*c, r); break;
            case kRoleArtwork:
                for (const auto& a : c->children) to_gprims(*a, r.artwork);
                break;
            case kRoleHighlight:
                for (const auto& a : c->children) to_gprims(*a, r.highlight);
                break;
            default: to_gprims(*c, r.other); break;
        }
    }
    std::stable_sort(r.pads.begin(), r.pads.end(), [](const Pad& a, const Pad& b) {
        if ((a.pin > 0) != (b.pin > 0)) return a.pin > 0;
        return a.pin < b.pin;
    });
    for (size_t i = 0; i < r.pads.size(); ++i)
        if (r.pads[i].pin == 1) { r.pin_one_index = int(i); break; }
    return true;
}

bool decode_package_rendering(const uint8_t* p, size_t n, Package& pkg, std::string& err) {
    PackageRendering r;
    if (!decode_package_rendering(p, n, r, err)) return false;
    pkg.pads = std::move(r.pads);
    pkg.body = std::move(r.artwork);
    if (pkg.body.empty()) pkg.body = std::move(r.other);
    return true;
}

} // namespace pcbp::cc
