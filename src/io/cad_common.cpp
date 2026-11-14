// ============================================================================
// cad_common.cpp — see cad_common.h.
// ============================================================================
#include "cad_common.h"
#include "io_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace pcbp::io {

const char* cad_status_name(CadImportStatus s) {
    switch (s) {
    case CadImportStatus::Complete: return "Complete";
    case CadImportStatus::ArtworkOnly: return "Artwork-only";
    case CadImportStatus::Partial: return "Partial";
    default: return "Error";
    }
}

void CadImportResult::classify(const std::string& extra) {
    std::string why;
    if (components > 0) {
        std::vector<std::string> miss;
        if (missing_packages > 0) miss.push_back(std::to_string(missing_packages) + " component(s) reference packages that are not defined");
        if (packages == 0) miss.push_back("no package geometry in the source");
        if (!extra.empty()) miss.push_back(extra);
        if (miss.empty()) {
            status = CadImportStatus::Complete;
            why = std::to_string(components) + " placements with package geometry";
        } else {
            status = CadImportStatus::Partial;
            why = std::to_string(components) + " placements, but ";
            for (size_t i = 0; i < miss.size(); ++i) why += (i ? "; " : "") + miss[i];
        }
    } else if (outline_polylines > 0 || layers > 0 || outline_items > 0) {
        status = CadImportStatus::ArtworkOnly;
        why = "no placements in the source \xE2\x80\x94 only ";
        why += outline_polylines > 0 || outline_items > 0 ? "board outline" : "";
        if (layers > 0) why += std::string(outline_polylines > 0 || outline_items > 0 ? " + " : "") + std::to_string(layers) + " artwork layer(s)";
        why += "; placements must come from a centroid / XY file or digitizing";
    } else {
        status = CadImportStatus::Error;
        why = "nothing importable";
    }
    status_reason = why;
}

std::string CadImportResult::status_line() const {
    return std::string("Import result: ") + cad_status_name(status) + " \xE2\x80\x94 " + status_reason;
}

std::string CadImportResult::describe() const {
    char b[512];
    std::snprintf(b, sizeof b,
                  "%s: %d components (%d top, %d bottom, %d SMT, %d TH, %d fiducials), %d packages, %d part numbers, "
                  "outline %d items -> %d polylines; units %s",
                  format.c_str(), components, top, bottom, smt, th, fiducials, packages, parts, outline_items,
                  outline_polylines, units.empty() ? "mm" : units.c_str());
    std::string s = status_line() + ". " + b;
    if (body_cad + body_extent + body_derived > 0)
        s += "; package bodies: " + std::to_string(body_cad) + " CAD outline, " + std::to_string(body_extent) + " CAD extent box, " +
             std::to_string(body_derived) + " derived from pads (approximate)";
    if (!warnings.empty()) s += "; " + std::to_string(warnings.size()) + " warning(s)";
    return s;
}

const char* body_source_label(const std::string& src) {
    if (src == "cad") return "CAD outline";
    if (src == "cad_extent") return "CAD extent box";
    if (src == "pads") return "Suggested from pads (approximate, no outline in source)";
    return "User drawn / unknown";
}

double units_override_mm(const std::string& s0) {
    std::string s = to_lower(trim(s0));
    if (s.empty() || s == "auto") return 0;
    if (s == "0.1mm" || s == "0.1 mm" || s == "dmm") return 0.1;
    if (s == "inch" || s == "in") return kMmPerInch;
    double k = unit_scale_word(s);
    if (k > 0) return k;
    if (auto v = parse_num(s); v && *v > 0) return *v;
    return 0;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
std::vector<std::string> split_char(std::string_view s, char d) {
    std::vector<std::string> v;
    size_t b = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == d) { v.emplace_back(s.substr(b, i - b)); b = i + 1; }
    }
    return v;
}

double num_or(std::string_view s, double def) {
    auto v = parse_num(s);
    return v ? *v : def;
}

int pin_number(const std::string& name) {
    std::string n = trim(name);
    if (n.empty()) return 0;
    bool digits = std::all_of(n.begin(), n.end(), [](char c) { return std::isdigit((unsigned char)c); });
    if (digits && n.size() < 9) return std::atoi(n.c_str());
    if (ieq(n, "A1") || ieq(n, "A01")) return 1;
    return 0;
}

bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit((unsigned char)a[i]) && std::isdigit((unsigned char)b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit((unsigned char)a[i2])) ++i2;
            while (j2 < b.size() && std::isdigit((unsigned char)b[j2])) ++j2;
            unsigned long long x = std::strtoull(a.substr(i, i2 - i).c_str(), nullptr, 10);
            unsigned long long y = std::strtoull(b.substr(j, j2 - j).c_str(), nullptr, 10);
            if (x != y) return x < y;
            i = i2; j = j2;
        } else {
            char x = (char)std::toupper((unsigned char)a[i]), y = (char)std::toupper((unsigned char)b[j]);
            if (x != y) return x < y;
            ++i; ++j;
        }
    }
    return (a.size() - i) < (b.size() - j);
}

void sort_pins_natural(std::vector<CadPin>& pins) {
    std::stable_sort(pins.begin(), pins.end(), [](const CadPin& a, const CadPin& b) { return natural_less(a.name, b.name); });
}

uint64_t pad_geometry_hash(const std::vector<Pad>& pads) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](double v) {
        long long q = std::llround(v * 100.0);      // 10 um quantum
        for (int i = 0; i < 8; ++i) { h ^= uint64_t(q >> (i * 8)) & 0xFF; h *= 1099511628211ull; }
    };
    for (auto& p : pads) {
        mix(p.pos.x); mix(p.pos.y); mix(p.w); mix(p.h); mix(norm_deg(p.rot)); mix(p.drill); mix(p.pin); mix((int)p.shape);
    }
    return h;
}

Vec2 local_to_abs(Vec2 l, Vec2 pos, double rot, bool mirror) {
    if (mirror) l.x = -l.x;
    return rotate(l, rot) + pos;
}

Vec2 abs_to_local(Vec2 a, Vec2 pos, double rot, bool mirror) {
    Vec2 l = rotate(a - pos, -rot);
    if (mirror) l.x = -l.x;
    return l;
}

double unit_scale_word(std::string_view w0) {
    std::string w = to_upper(trim(w0));
    if (w == "MM" || w == "MILLIMETER" || w == "MILLIMETERS" || w == "MILLIMETRE" || w == "METRIC" || w == "MILLIMETRES")
        return 1.0;
    if (w == "INCH" || w == "INCHES" || w == "IN" || w == "INCH_") return kMmPerInch;
    if (w == "MIL" || w == "MILS" || w == "THOU") return kMmPerMil;
    if (w == "MICRON" || w == "MICRONS" || w == "UM" || w == "MICROMETER") return 0.001;
    if (w == "CM" || w == "CENTIMETER") return 10.0;
    return 0;
}

bool guess_padstack(const std::string& name0, double unit_mm, CadPin& pin) {
    std::string s = to_upper(name0);
    // '_' or 'P' between digits acts as decimal point ("1_0X0_5", "1P2X0P6")
    for (size_t i = 1; i + 1 < s.size(); ++i)
        if ((s[i] == '_' || s[i] == 'P') && std::isdigit((unsigned char)s[i - 1]) && std::isdigit((unsigned char)s[i + 1]))
            s[i] = '.';
    struct Num { double v; size_t b, e; };
    std::vector<Num> nums;
    for (size_t i = 0; i < s.size();) {
        if (std::isdigit((unsigned char)s[i]) || (s[i] == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1]))) {
            size_t b = i;
            while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) ++i;
            nums.push_back({std::atof(s.substr(b, i - b).c_str()), b, i});
        } else ++i;
    }
    if (nums.empty()) return false;
    auto before = [&](const Num& n) { return n.b > 0 ? s[n.b - 1] : '\0'; };
    auto after = [&](const Num& n) { return n.e < s.size() ? s[n.e] : '\0'; };
    double w = 0, h = 0, drill = 0;
    std::vector<Num> rest;
    for (auto& n : nums) {
        if ((before(n) == 'D' && n.b >= 2 && std::isdigit((unsigned char)s[n.b - 2])) || (after(n) == 'D' && n.e + 1 == s.size())) drill = n.v;
        else rest.push_back(n);
    }
    if (rest.empty()) return false;
    if (rest.size() >= 2 && rest[0].e < s.size() && s[rest[0].e] == 'X' && rest[1].b == rest[0].e + 1) {
        w = rest[0].v; h = rest[1].v;
    } else {
        w = h = rest[0].v;
    }
    if (w <= 0) return false;
    // names in thousandths of the file unit (mil names in an inch design, um in mm)
    if (w * unit_mm > 25.0 && unit_mm >= 1.0) { w /= 1000; h /= 1000; drill /= 1000; }
    pin.w = w * unit_mm; pin.h = h * unit_mm;
    pin.drill = drill * unit_mm;
    // R18c: Allegro binary padstack records are not decoded, so the name is the only drill source there. Keep it
    // strict: an SMD-named padstack never has a drill, and a "drill" not smaller than the pad is not a hole.
    if (s.find("SMD") != std::string::npos || s.rfind("SM", 0) == 0 || pin.drill >= std::min(pin.w, pin.h)) pin.drill = 0;
    const bool round = s.rfind("C", 0) == 0 || s.find("CIR") != std::string::npos || s.find("ROUND") != std::string::npos ||
                       (s.rfind("R", 0) == 0 && rest.size() == 1);
    if (s.find("OB") != std::string::npos || s.find("OVAL") != std::string::npos) pin.shape = PadShape::Oblong;
    else if (round && w == h) pin.shape = PadShape::Round;
    else if (w == h && (s.rfind("S", 0) == 0 || s.find("SQ") != std::string::npos)) pin.shape = PadShape::Square;
    else pin.shape = (w == h && rest.size() == 1 && !s.empty() && s[0] != 'S') ? PadShape::Round : PadShape::Rect;
    return true;
}

// ---------------------------------------------------------------------------
// OutlineBuilder
// ---------------------------------------------------------------------------
void OutlineBuilder::add_line(Vec2 a, Vec2 b) {
    if (dist(a, b) < 1e-9) return;
    pieces_.push_back({a, b});
    ++items_;
}

void OutlineBuilder::add_arc(Vec2 c, double r, double a0, double sweep) {
    if (r <= 0 || sweep == 0) return;
    const double step = std::clamp(seg_deg, 0.5, 45.0);
    int n = std::max(2, (int)std::ceil(std::fabs(sweep) / step));
    std::vector<Vec2> pts;
    pts.reserve(n + 1);
    for (int i = 0; i <= n; ++i) {
        const double a = deg2rad(a0 + sweep * i / n);
        pts.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
    }
    pieces_.push_back(std::move(pts));
    ++items_;
}

void OutlineBuilder::add_arc_ends(Vec2 s, Vec2 e, Vec2 c, bool ccw) {
    const double r = dist(s, c);
    const double a0 = rad2deg(std::atan2(s.y - c.y, s.x - c.x));
    const double a1 = rad2deg(std::atan2(e.y - c.y, e.x - c.x));
    double sw = a1 - a0;
    if (ccw) { while (sw <= 1e-9) sw += 360; }
    else { while (sw >= -1e-9) sw -= 360; }
    if (dist(s, e) < 1e-6) sw = ccw ? 360 : -360;
    add_arc(c, r, a0, sw);
    if (!pieces_.empty()) { pieces_.back().front() = s; if (std::fabs(std::fabs(sw) - 360) > 1e-9) pieces_.back().back() = e; else pieces_.back().back() = s; }
}

void OutlineBuilder::add_arc_3pt(Vec2 a, Vec2 m, Vec2 b) {
    const double d = 2 * (a.x * (m.y - b.y) + m.x * (b.y - a.y) + b.x * (a.y - m.y));
    if (std::fabs(d) < 1e-12) { add_line(a, b); return; }
    const double a2 = a.x * a.x + a.y * a.y, m2 = m.x * m.x + m.y * m.y, b2 = b.x * b.x + b.y * b.y;
    Vec2 c{(a2 * (m.y - b.y) + m2 * (b.y - a.y) + b2 * (a.y - m.y)) / d,
           (a2 * (b.x - m.x) + m2 * (a.x - b.x) + b2 * (m.x - a.x)) / d};
    // orientation of a->m->b
    const double cross = (m.x - a.x) * (b.y - a.y) - (m.y - a.y) * (b.x - a.x);
    add_arc_ends(a, b, c, cross > 0);
}

void OutlineBuilder::add_bulge(Vec2 a, Vec2 b, double bulge) {
    if (std::fabs(bulge) < 1e-12) { add_line(a, b); return; }
    const double theta = 4 * std::atan(bulge);       // included angle, + = CCW
    const double chord = dist(a, b);
    const double r = chord / (2 * std::sin(std::fabs(theta) / 2));
    const Vec2 mid = (a + b) * 0.5;
    const Vec2 dir = (b - a) * (1.0 / chord);
    Vec2 nrm{-dir.y, dir.x};
    const double hgt = std::sqrt(std::max(0.0, r * r - chord * chord / 4));
    // centre lies left of chord for CCW arcs < 180, right for > 180
    const double sgn = (bulge > 0) == (std::fabs(theta) < kPi) ? 1 : -1;
    const Vec2 c = mid + nrm * (hgt * sgn);
    add_arc_ends(a, b, c, bulge > 0);
}

void OutlineBuilder::add_circle(Vec2 c, double r) {
    add_arc(c, r, 0, 360);
    if (!pieces_.empty()) pieces_.back().back() = pieces_.back().front();
}

void OutlineBuilder::add_polyline(const std::vector<Vec2>& pts, bool closed) {
    if (pts.size() < 2) return;
    std::vector<Vec2> v = pts;
    if (closed && dist(v.front(), v.back()) > 1e-9) v.push_back(v.front());
    pieces_.push_back(std::move(v));
    ++items_;
}

std::vector<std::vector<Vec2>> OutlineBuilder::finish(double tol) const {
    std::vector<std::vector<Vec2>> out;
    std::vector<bool> used(pieces_.size(), false);
    for (size_t i = 0; i < pieces_.size(); ++i) {
        if (used[i]) continue;
        used[i] = true;
        std::vector<Vec2> poly = pieces_[i];
        bool grew = true;
        while (grew && dist(poly.front(), poly.back()) > tol) {
            grew = false;
            for (size_t j = 0; j < pieces_.size(); ++j) {
                if (used[j]) continue;
                const auto& p = pieces_[j];
                if (dist(poly.back(), p.front()) <= tol) { poly.insert(poly.end(), p.begin() + 1, p.end()); }
                else if (dist(poly.back(), p.back()) <= tol) { poly.insert(poly.end(), p.rbegin() + 1, p.rend()); }
                else if (dist(poly.front(), p.back()) <= tol) { poly.insert(poly.begin(), p.begin(), p.end() - 1); }
                else if (dist(poly.front(), p.front()) <= tol) { poly.insert(poly.begin(), p.rbegin(), p.rend() - 1); }
                else continue;
                used[j] = true; grew = true;
                break;
            }
        }
        if (poly.size() > 2 && dist(poly.front(), poly.back()) <= tol) poly.back() = poly.front();
        out.push_back(std::move(poly));
    }
    return out;
}

// ---------------------------------------------------------------------------
// CadBuilder
// ---------------------------------------------------------------------------
void CadBuilder::warn(const std::string& w) {
    if (++warn_count_ <= 200) res.warnings.push_back(w);
    else if (warn_count_ == 201) res.warnings.push_back("(further warnings suppressed)");
}

bool CadBuilder::has_package(const std::string& name) const {
    for (auto& v : variants_) if (v.first == name) return true;
    return false;
}

std::string CadBuilder::add_package(const CadPackage& cp) {
    Package p;
    p.name = cp.name;
    p.height = cp.height > 0 ? cp.height : 0;
    if (p.height > 0) p.height_src = "cad";   // R24a
    // R18c: through-hole is decided ONLY from drilled pads (real or format-declared hole sizes the importer
    // resolved into cpin.drill), never from a package/pin type hint alone or from names.
    bool th = false;
    for (auto& cpin : cp.pins) {
        Pad d;
        d.pos = cpin.pos;
        d.w = cpin.w > 0 ? cpin.w : 0.5;
        d.h = cpin.h > 0 ? cpin.h : d.w;
        d.rot = norm_deg(cpin.rot);
        if (std::fabs(std::fmod(d.rot, 180.0) - 90.0) < 1e-6) { std::swap(d.w, d.h); d.rot = norm_deg(d.rot - 90.0); }
        if (std::fabs(d.rot - 180.0) < 1e-6) d.rot = 0;     // pads are symmetric
        d.shape = cpin.shape;
        d.drill = cpin.drill;
        d.pin = pin_number(cpin.name);
        if (d.drill > 0) th = true;
        p.pads.push_back(d);
    }
    p.smt = !th;
    p.leads = (int)p.pads.size();
    p.pin_marked = std::any_of(p.pads.begin(), p.pads.end(), [](const Pad& d) { return d.pin == 1; });
    p.body = cp.body;
    p.body_source = p.body.empty() ? "" : (cp.body_source.empty() ? "cad" : cp.body_source);
    if (p.body.empty() && !p.pads.empty()) p.auto_body();      // sets body_source = "pads"
    const uint64_t h = pad_geometry_hash(p.pads);
    for (auto& v : variants_) {
        if (v.first != cp.name) continue;
        for (auto& pv : v.second) if (pv.hash == h) return pv.final_name;
        std::string nn = cp.name + "~" + std::to_string(v.second.size() + 1);
        warn("package " + cp.name + ": differing pad geometry, variant stored as " + nn);
        p.name = nn;
        v.second.push_back({h, nn});
        pkgs_.push_back(std::move(p));
        return nn;
    }
    variants_.push_back({cp.name, {{h, cp.name}}});
    pkgs_.push_back(std::move(p));
    return cp.name;
}

void CadBuilder::add_component(const CadComp& c) { comps_.push_back(c); }

namespace {
void scale_prim(GPrim& g, double f) {
    for (auto& p : g.pts) p = p * f;
    g.radius *= f; g.text_h *= f;
    for (auto& c : g.children) scale_prim(c, f);
}
void scale_package(Package& p, double f) {
    for (auto& d : p.pads) { d.pos = d.pos * f; d.w *= f; d.h *= f; d.drill *= f; }
    for (auto& g : p.body) scale_prim(g, f);
    for (auto& g : p.marker_art) scale_prim(g, f);
    p.marker_pos = p.marker_pos * f; p.marker_size *= f;
}
} // namespace

bool CadBuilder::commit(Project& prj, std::string& err) {
    (void)err;
    auto frame = outline.finish();
    res.outline_items = outline.items();
    if (opt.scale > 0 && std::fabs(opt.scale - 1.0) > 1e-12) {
        const double f = opt.scale;
        for (auto& p : pkgs_) scale_package(p, f);
        for (auto& c : comps_) c.pos = c.pos * f;
        for (auto& pl : frame) for (auto& q : pl) q = q * f;
        warn("user scale \xC3\x97" + std::to_string(f) + " applied after unit conversion (placements, packages, outline)");
    }
    for (auto& p : pkgs_) {
        if (p.body_source == "cad") ++res.body_cad;
        else if (p.body_source == "cad_extent") ++res.body_extent;
        else ++res.body_derived;
    }
    if (opt.replace) {
        prj.comps.clear();
        if (opt.import_packages) prj.packages.clear();
        prj.parts.clear();
        if (opt.import_board && !frame.empty()) prj.board_frame.clear();
    }
    std::map<std::string, const Package*> local;
    for (auto& p : pkgs_) local[p.name] = &p;
    if (opt.import_packages) {
        for (auto& p : pkgs_) prj.packages[p.name] = p;
        res.packages = (int)pkgs_.size();
    }
    std::set<std::string> refs;
    for (auto& c : comps_) {
        Component k;
        k.ref = c.ref;
        if (!refs.insert(c.ref).second) warn("duplicate reference " + c.ref);
        k.pos = c.pos;
        k.rot = norm_deg(c.rot);
        if (std::fabs(k.rot - std::round(k.rot)) < 1e-6) k.rot = std::round(k.rot);
        k.side = c.bottom ? Side::Bottom : Side::Top;
        k.package = c.package;
        k.number = !c.part.empty() ? c.part : c.value;
        const Package* pk = nullptr;
        if (auto it = local.find(c.package); it != local.end()) pk = it->second;
        else if (auto it2 = prj.packages.find(c.package); it2 != prj.packages.end()) pk = &it2->second;
        else { ++res.missing_packages; if (!c.package.empty()) warn(c.ref + ": package " + c.package + " not defined"); }
        const bool fid = c.fiducial || (opt.fiducial_by_name && (istarts_with(c.ref, "FID") || icontains(c.package, "FID")));
        if (fid) { k.feature = Feature::Fiducial; ++res.fiducials; }
        if (pk) {
            for (size_t i = 0; i < pk->pads.size(); ++i)
                if (pk->pads[i].pin == 1) { k.pin1_set = true; k.pin1_index = (int)i; break; }
            if (!fid) { if (pk->smt) ++res.smt; else ++res.th; }
        }
        if (!k.number.empty() && !fid) {
            auto& pn = prj.parts[k.number];
            if (pn.number.empty()) {
                pn.number = k.number;
                pn.description = c.value;
                pn.package = c.package;
                ++res.parts;
            }
        }
        if (c.bottom) ++res.bottom; else ++res.top;
        ++res.components;
        prj.add_component(k);
    }
    if (opt.import_board) {
        for (auto& f : frame) if (f.size() >= 2) { prj.board_frame.push_back(f); ++res.outline_polylines; }
        if (res.outline_polylines) prj.board_outline_src = OutlineSource::Imported;   // R16 provenance
    }
    prj.dirty = true;
    res.classify(res.partial_reason);
    return true;
}

} // namespace pcbp::io

// ===========================================================================
// Post-import rescale (REVIEW_V9 §6C)
// ===========================================================================
namespace pcbp::io {
namespace {

std::string fmt_scaled_num(const std::string& num, double f) {
    // keeps integer-ness: integers stay integers (rounded), decimals get 6 places
    const bool dec = num.find('.') != std::string::npos;
    const double v = std::strtod(num.c_str(), nullptr) * f;
    char b[64];
    if (dec) std::snprintf(b, sizeof b, "%.6f", v);
    else std::snprintf(b, sizeof b, "%lld", (long long)std::llround(v));
    return b;
}

// Scales the numbers following any letter in `letters` inside one statement.
std::string scale_coords(const std::string& st, double f, const char* letters) {
    std::string out;
    out.reserve(st.size() + 8);
    for (size_t i = 0; i < st.size();) {
        const char c = st[i];
        if (std::strchr(letters, c) && i + 1 < st.size() &&
            (std::isdigit((unsigned char)st[i + 1]) || st[i + 1] == '-' || st[i + 1] == '+' || st[i + 1] == '.')) {
            size_t j = i + 1;
            if (st[j] == '-' || st[j] == '+') ++j;
            while (j < st.size() && (std::isdigit((unsigned char)st[j]) || st[j] == '.')) ++j;
            out += c;
            out += fmt_scaled_num(st.substr(i + 1, j - i - 1), f);
            i = j;
        } else out += st[i++];
    }
    return out;
}

} // namespace

bool scale_rs274x(std::string& text, double f, std::string& err) {
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '%') {
            size_t e = text.find('%', i + 1);
            if (e == std::string::npos) { err = "unterminated % block"; return false; }
            std::string blk = text.substr(i + 1, e - i - 1);
            std::string b = trim(blk);
            if (istarts_with(b, "FS")) {
                if (b.size() > 2 && (b[2] == 'T' || b[2] == 't')) { err = "trailing-zero coordinate format"; return false; }
                out += "%" + blk + "%";
            } else if (istarts_with(b, "AM")) {
                err = "aperture macros (%AM) cannot be rescaled safely"; return false;
            } else if (istarts_with(b, "AD")) {
                // %ADD10C,0.5X0.25*%   C/R/O: all params; P: dia, (verts, rot), hole
                auto comma = blk.find(',');
                size_t k = 3;
                while (k < blk.size() && std::isdigit((unsigned char)blk[k])) ++k;
                std::string name = blk.substr(k, comma == std::string::npos ? std::string::npos : comma - k);
                std::string nm = trim(name);
                if (nm != "C" && nm != "R" && nm != "O" && nm != "P") { err = "macro aperture '" + nm + "' cannot be rescaled safely"; return false; }
                if (comma == std::string::npos) { out += "%" + blk + "%"; }
                else {
                    std::string params = blk.substr(comma + 1);
                    std::string tail;
                    if (auto st = params.find('*'); st != std::string::npos) { tail = params.substr(st); params = params.substr(0, st); }
                    auto ps = split_char(params, 'X');
                    for (size_t q = 0; q < ps.size(); ++q) {
                        if (nm == "P" && (q == 1 || q == 2)) continue;
                        if (!trim(ps[q]).empty()) ps[q] = fmt_scaled_num(trim(ps[q]), f);
                    }
                    std::string np;
                    for (size_t q = 0; q < ps.size(); ++q) np += (q ? "X" : "") + ps[q];
                    out += "%" + blk.substr(0, comma + 1) + np + tail + "%";
                }
            } else if (istarts_with(b, "SR")) {
                out += "%" + scale_coords(blk, f, "IJ") + "%";
            } else if (istarts_with(b, "OF")) {
                out += "%" + scale_coords(blk, f, "AB") + "%";
            } else out += "%" + blk + "%";
            i = e + 1;
            continue;
        }
        size_t e = text.find_first_of("*%", i);
        if (e == std::string::npos) e = text.size();
        else if (text[e] == '*') ++e;
        std::string st = text.substr(i, e - i);
        std::string ts = trim(st);
        if (istarts_with(ts, "G04") || istarts_with(ts, "G4 ")) out += st;
        else out += scale_coords(st, f, "XYIJ");
        i = e;
    }
    text.swap(out);
    return true;
}

bool scale_excellon(std::string& text, double f, std::string& err) {
    auto lines = split_lines(text);
    bool lz = false;
    for (auto& l0 : lines) {
        std::string l = to_upper(trim(l0));
        if ((istarts_with(l, "METRIC") || istarts_with(l, "INCH")) && l.find("LZ") != std::string::npos) lz = true;
    }
    std::string out;
    for (auto& l0 : lines) {
        std::string l = trim(l0);
        std::string u = to_upper(l);
        if (u.empty() || u[0] == ';' || u == "M48" || u == "%" || istarts_with(u, "METRIC") || istarts_with(u, "INCH") ||
            istarts_with(u, "FMAT") || istarts_with(u, "M") || istarts_with(u, "G9")) { out += l0 + "\n"; continue; }
        if (u[0] == 'T') {     // T01C0.300F.. : only the C (diameter) value
            out += scale_coords(l, f, "C") + "\n";
            continue;
        }
        if (u.find('X') != std::string::npos || u.find('Y') != std::string::npos) {
            const bool has_dec = l.find('.') != std::string::npos;
            if (!has_dec && lz) { err = "leading-zero integer Excellon coordinates cannot be rescaled safely"; return false; }
            out += scale_coords(l, f, "XY") + "\n";
            continue;
        }
        out += l0 + "\n";
    }
    text.swap(out);
    return true;
}

int rescale_project(Project& prj, double f, std::vector<std::string>& notes) {
    if (!(f > 0) || std::fabs(f - 1.0) < 1e-15) return 0;
    for (auto& c : prj.comps) { c.pos = c.pos * f; c.cad_pos = c.cad_pos * f; c.label_off = c.label_off * f; }
    for (auto& kv : prj.packages) scale_package(kv.second, f);
    for (auto& pl : prj.board_frame) for (auto& q : pl) q = q * f;
    for (auto& pl : prj.panel_frame) for (auto& q : pl) q = q * f;
    for (auto& im : prj.images) im.offset = im.offset * f;
    prj.virtual_origin = prj.virtual_origin * f;
    for (auto& bi : prj.board_images) {
        for (int r = 0; r < 6; ++r) bi.H[r] *= f;          // board = H * px: scale the x/y rows
        for (auto& pp : bi.pairs) pp.board = pp.board * f;
    }
    int n = 0;
    for (auto& L : prj.layers) {
        std::string t = L.content, err;
        const bool drill = L.role == LayerRole::Drill || (t.find("M48") != std::string::npos && t.find("%FS") == std::string::npos);
        const bool ok = drill ? scale_excellon(t, f, err) : scale_rs274x(t, f, err);
        if (ok && !L.aperture_list.empty()) { notes.push_back("layer " + L.name + ": separate aperture list not rescaled"); }
        if (ok) { L.content.swap(t); L.offset = L.offset * f; ++n; }
        else notes.push_back("layer " + L.name + " not rescaled: " + err);
    }
    prj.dirty = true;
    return n;
}

} // namespace pcbp::io
