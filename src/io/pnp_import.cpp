// ============================================================================
// pnp_import.cpp — pick-and-place import on the text_import engine.
// ============================================================================
#include "pnp_import.h"
#include "io_util.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

namespace pcbp::io {

bool PnpPreview::mapping_ok() const {
    return text.col(Role::Ref) >= 0 && text.col(Role::X) >= 0 && text.col(Role::Y) >= 0;
}

int parse_side_token(const std::string& cell, const PnpOptions& o, bool invert) {
    std::string k = to_upper(trim(cell));
    if (k.empty()) return -1;
    if (invert) {   // "Mirror" / "SYM_MIRROR" style column: YES = bottom
        if (auto f = parse_flag(k)) return *f ? 1 : 0;
    }
    for (auto& t : o.top_tokens) if (ieq(k, t)) return 0;
    for (auto& t : o.bottom_tokens) if (ieq(k, t)) return 1;
    if (k.rfind("TOP", 0) == 0 || k.rfind("FRONT", 0) == 0) return 0;
    if (k.rfind("BOT", 0) == 0 || k.rfind("BACK", 0) == 0) return 1;
    if (k == "\xE9\xA1\xB6\xE5\xB1\x82" || k == "\xE9\xA1\xB6") return 0;     // 顶层 / 顶
    if (k == "\xE5\xBA\x95\xE5\xB1\x82" || k == "\xE5\xBA\x95") return 1;     // 底层 / 底
    return -1;
}

// ---------------------------------------------------------------------------
// R16c: side column analysis
// ---------------------------------------------------------------------------
const char* side_conv_name(SideConv c) {
    switch (c) {
    case SideConv::NoColumn: return "no side column";
    case SideConv::Words: return "Top / Bottom words";
    case SideConv::Letters: return "T / B letters";
    case SideConv::Mirror: return "mirrored flag (M / yes = bottom)";
    case SideConv::Numeric: return "layer numbers (lowest = top, highest = bottom)";
    case SideConv::Mixed: return "mixed values";
    }
    return "";
}

int SideAnalysis::side_of(const std::string& cell) const {
    const std::string k = to_upper(trim(cell));
    for (auto& v : values) if (v.token == k) return v.side;
    return -1;
}

static bool mirror_word(const std::string& k) { return k == "M" || k == "MIRROR" || k == "MIRRORED" || k == "MIR" || k == "FLIPPED"; }

SideAnalysis analyze_sides(const ParsedText& t, int col, bool invert, const PnpOptions& o) {
    SideAnalysis a;
    a.col = col;
    const int def = o.default_side == Side::Bottom ? 1 : 0;
    std::map<std::string, int> cnt;
    int nrows = 0;
    for (auto& r : t.rows) {
        if (r.kind != RowKind::Data) continue;
        ++nrows;
        if (col >= 0 && col < (int)r.cells.size()) cnt[to_upper(trim(r.cells[col]))]++;
    }
    if (col < 0) { a.top = def ? 0 : nrows; a.bottom = def ? nrows : 0; return a; }
    // numeric layer convention: every non-blank value is an integer layer number
    std::vector<long> nums;
    bool all_num = true, any = false, any_mirror = false, all_letters = true;
    for (auto& [k, n] : cnt) {
        if (k.empty()) continue;
        any = true;
        char* e = nullptr;
        long v = std::strtol(k.c_str(), &e, 10);
        if (!e || *e) all_num = false; else nums.push_back(v);
        if (mirror_word(k)) any_mirror = true;
        if (k.size() != 1) all_letters = false;
    }
    std::sort(nums.begin(), nums.end());
    nums.erase(std::unique(nums.begin(), nums.end()), nums.end());
    const bool numeric = any && all_num && !invert;
    int words = 0, letters = 0, mirrors = 0, numerics = 0;
    for (auto& [k, n] : cnt) {
        SideValue v;
        v.token = k; v.count = n;
        if (k.empty()) v.auto_side = def;
        else if (numeric) {
            const long x = std::strtol(k.c_str(), nullptr, 10);
            if (nums.size() >= 2) v.auto_side = x == nums.front() ? 0 : x == nums.back() ? 1 : -1;   // inner layers: ask
            else v.auto_side = (x == 0 || x == 1) ? 0 : (x == 2 || x == 16 || x == 31 || x == 32) ? 1 : -1;
            ++numerics;
        } else if (mirror_word(k)) { v.auto_side = 1; ++mirrors; }
        else if (any_mirror && (k == "N" || k == "NO" || k == "-")) { v.auto_side = 0; ++mirrors; }   // M / N column
        else {
            v.auto_side = parse_side_token(k, o, invert);
            if (v.auto_side >= 0) { if (invert) ++mirrors; else if (k.size() == 1) ++letters; else ++words; }
        }
        v.side = v.auto_side;
        if (auto it = o.side_override.find(k); it != o.side_override.end() && (it->second == 0 || it->second == 1)) { v.side = it->second; v.overridden = true; }
        if (k.empty() && !v.overridden) v.side = def;
        a.values.push_back(v);
    }
    (void)all_letters;
    const int kinds = (words > 0) + (letters > 0) + (mirrors > 0) + (numerics > 0);
    a.conv = !any ? SideConv::NoColumn : numerics ? SideConv::Numeric : kinds > 1 ? SideConv::Mixed
           : mirrors ? SideConv::Mirror : letters ? SideConv::Letters : SideConv::Words;
    std::stable_sort(a.values.begin(), a.values.end(), [](const SideValue& x, const SideValue& y) {
        if (x.token.empty() != y.token.empty()) return y.token.empty();
        return x.count > y.count;
    });
    for (auto& v : a.values) {
        if (v.token.empty()) a.blank += v.count;
        if (v.side == 0) a.top += v.count; else if (v.side == 1) a.bottom += v.count; else a.unknown += v.count;
    }
    return a;
}

// ---------------------------------------------------------------------------
// R16c: BOM vs centroid content sniffing
// ---------------------------------------------------------------------------
const char* csv_kind_name(CsvKind k) { return k == CsvKind::Bom ? "BOM" : k == CsvKind::Centroid ? "Centroid / pick-and-place" : "Unknown"; }

CsvKind sniff_csv_kind(const std::string& bytes, std::string* why) {
    auto say = [&](const std::string& s) { if (why) *why = s; };
    ImportLayout L;
    ParsedText pt, bt;
    std::string err;
    if (!analyze_text(bytes, L, Purpose::Pnp, pt, err) || pt.data_rows() == 0) { say("no table found"); return CsvKind::Unknown; }
    const int cx = pt.col(Role::X), cy = pt.col(Role::Y), cr = pt.col(Role::Ref);
    auto numeric_frac = [&](int c) {
        int n = 0, ok = 0;
        for (auto& r : pt.rows) {
            if (r.kind != RowKind::Data || c >= (int)r.cells.size()) continue;
            if (trim(r.cells[c]).empty()) continue;
            ++n;
            if (parse_length(r.cells[c], pt.decimal_comma, nullptr)) ++ok;
        }
        return n ? (double)ok / n : 0.0;
    };
    // a reference column holding lists ("C1,C2,C5-C9") is a BOM even when the PNP role guesser finds X / Y
    int ref_lists = 0, ref_rows = 0;
    if (cr >= 0)
        for (auto& r : pt.rows) {
            if (r.kind != RowKind::Data || cr >= (int)r.cells.size()) continue;
            const std::string& v = r.cells[cr];
            if (trim(v).empty()) continue;
            ++ref_rows;
            if (v.find_first_of(",; ") != std::string::npos || v.find('-') != std::string::npos) ++ref_lists;
        }
    const bool lists = ref_rows && ref_lists * 3 >= ref_rows;
    if (cx >= 0 && cy >= 0 && cx != cy && !lists && numeric_frac(cx) >= 0.9 && numeric_frac(cy) >= 0.9) {
        say("Reference + numeric X / Y columns (\"" + pt.cols[cx].header + "\", \"" + pt.cols[cy].header + "\")");
        return CsvKind::Centroid;
    }
    if (!analyze_text(bytes, L, Purpose::Bom, bt, err)) { say("no table found"); return CsvKind::Unknown; }
    const bool ref = bt.col(Role::Ref) >= 0;
    const bool bomish = bt.col(Role::Qty) >= 0 || bt.col(Role::PartNumber) >= 0 || bt.col(Role::MPN) >= 0 ||
                        bt.col(Role::Value) >= 0 || bt.col(Role::Description) >= 0;
    if (ref && (bomish || lists)) {
        say(std::string("Reference column") + (lists ? " with reference lists" : "") + (bt.col(Role::Qty) >= 0 ? " + quantity" : "") + ", no X / Y pair");
        return CsvKind::Bom;
    }
    say("neither X / Y coordinates nor BOM columns were recognised");
    return CsvKind::Unknown;
}

// Unit guess from coordinate magnitudes (values in file units, no suffix).
static LenUnit guess_units(const std::vector<double>& v, const std::vector<int>& decimals, std::string& why, double* factor) {
    if (v.empty()) { why = "no coordinates; default mm"; return LenUnit::MM; }
    double mx = 0;
    bool all_int = true;
    for (double x : v) { mx = std::max(mx, std::fabs(x)); if (std::fabs(x - std::round(x)) > 1e-9) all_int = false; }
    std::vector<int> d = decimals;
    std::sort(d.begin(), d.end());
    int med = d.empty() ? 0 : d[d.size() / 2];
    if (mx > 40000) {
        if (factor) *factor = 0.1;
        why = "coordinates up to " + fmt_fixed(mx, 0) + " -> 1/10 mil (factor 0.1)";
        return LenUnit::Mil;
    }
    if (mx > 650 || (all_int && mx > 100)) {
        why = "coordinates up to " + fmt_fixed(mx, 1) + " too large for mm -> mil";
        return LenUnit::Mil;
    }
    if (mx <= 24 && med >= 3) {
        why = "coordinates <= " + fmt_fixed(mx, 3) + " with " + std::to_string(med) + " decimals -> inch";
        return LenUnit::Inch;
    }
    why = "coordinates up to " + fmt_fixed(mx, 2) + " -> mm";
    return LenUnit::MM;
}

static int count_decimals(const std::string& s) {
    size_t p = s.find_first_of(".,");
    if (p == std::string::npos) return 0;
    int n = 0;
    for (size_t i = p + 1; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) ++n;
    return n;
}

void pnp_rebuild(PnpPreview& p, const PnpOptions& opt) {
    ParsedText& t = p.text;
    assign_roles(t, opt.layout, Purpose::Pnp);
    p.rows.clear(); p.row_src.clear(); p.issues.clear();
    p.skipped = p.filtered = p.dnp = 0;
    for (auto& n : t.notes) p.issues.push_back({0, n});
    const bool dc = t.decimal_comma;
    const int cr = t.col(Role::Ref), cx = t.col(Role::X), cy = t.col(Role::Y), crot = t.col(Role::Rotation);
    const int cs = t.col(Role::Side), cpn = t.col(Role::PartNumber), cpk = t.col(Role::Package), cv = t.col(Role::Value);
    const int cd = t.col(Role::Dnp), cu = t.col(Role::Unit);
    p.sides = analyze_sides(t, cs, cs >= 0 && t.cols[cs].invert, opt);
    if (cr < 0) p.issues.push_back({0, "no Reference column"});
    if (cx < 0 || cy < 0) p.issues.push_back({0, "X / Y columns not mapped"});

    // ---- units ----
    double factor = opt.factor > 0 ? opt.factor : 1.0;
    LenUnit fu = opt.units;
    if (fu != LenUnit::Auto) p.units_reason = "user";
    if (fu == LenUnit::Auto && cx >= 0 && t.cols[cx].unit != LenUnit::Auto) { fu = t.cols[cx].unit; p.units_reason = "X column header/suffix"; }
    if (fu == LenUnit::Auto && cy >= 0 && t.cols[cy].unit != LenUnit::Auto) { fu = t.cols[cy].unit; p.units_reason = "Y column header/suffix"; }
    if (fu == LenUnit::Auto && t.unit_hint != LenUnit::Auto) { fu = t.unit_hint; p.units_reason = "file comment / preamble"; }
    if (fu == LenUnit::Auto && cx >= 0 && cy >= 0) {
        std::vector<double> v;
        std::vector<int> dec;
        for (auto& r : t.rows) {
            if (r.kind != RowKind::Data) continue;
            for (int c : {cx, cy}) {
                LenUnit lu;
                auto x = parse_length(r.cells[c], dc, &lu);
                if (x && lu == LenUnit::Auto) { v.push_back(*x); dec.push_back(count_decimals(r.cells[c])); }
            }
        }
        double f2 = 1.0;
        fu = guess_units(v, dec, p.units_reason, &f2);
        if (opt.factor == 1.0) factor = f2;
        p.units_reason = "guess: " + p.units_reason;
    }
    p.units = fu;

    // ---- rows ----
    std::set<std::string> seen;
    double maxx = -1e300;
    std::vector<size_t> bottoms;
    for (size_t i = 0; i < t.rows.size(); ++i) {
        const Row& r = t.rows[i];
        if (r.kind != RowKind::Data) continue;
        const int line = r.line + 1;
        CentroidRow o;
        o.line = line;
        o.ref = cr >= 0 ? trim(r.cells[cr]) : std::string();
        if (opt.upper_refs) o.ref = to_upper(o.ref);
        if (o.ref.empty()) { ++p.skipped; p.issues.push_back({line, "no reference; row skipped"}); continue; }
        LenUnit row_unit = fu;
        if (cu >= 0) {
            LenUnit u = len_unit_from_word(r.cells[cu]);
            if (u != LenUnit::Auto) row_unit = u;
        }
        double xy[2];
        bool ok = true;
        for (int k = 0; k < 2; ++k) {
            int c = k ? cy : cx;
            LenUnit lu = LenUnit::Auto;
            auto v = c >= 0 ? parse_length(r.cells[c], dc, &lu) : std::nullopt;
            if (!v) { ok = false; break; }
            double mm = lu != LenUnit::Auto ? *v * len_unit_mm(lu) : *v * len_unit_mm(row_unit) * factor;
            xy[k] = mm;
        }
        if (!ok) { ++p.skipped; p.issues.push_back({line, o.ref + ": X/Y not numeric; row skipped"}); continue; }
        o.pos = {xy[0], xy[1]};
        if (crot >= 0 && !trim(r.cells[crot]).empty()) {
            if (auto a = parse_angle(r.cells[crot], dc)) { o.rot = *a; o.has_rot = true; }
            else p.issues.push_back({line, o.ref + ": rotation \"" + r.cells[crot] + "\" not understood"});
        }
        o.side = opt.default_side;
        if (cs >= 0) {   // R16c: per-value table (auto + overrides; blank = default side)
            const bool blank = trim(r.cells[cs]).empty();
            int sd = p.sides.side_of(r.cells[cs]);
            if (sd >= 0) { o.side = sd ? Side::Bottom : Side::Top; o.has_side = !blank; }
            else if (!trim(r.cells[cs]).empty()) p.issues.push_back({line, o.ref + ": side \"" + r.cells[cs] + "\" not recognised (set it in the side table)"});
        }
        if (opt.side_select == SideSelect::Top && o.side != Side::Top) { ++p.filtered; continue; }
        if (opt.side_select == SideSelect::Bottom && o.side != Side::Bottom) { ++p.filtered; continue; }
        if (cpn >= 0) o.part = trim(r.cells[cpn]);
        if (cpk >= 0) o.package = trim(r.cells[cpk]);
        if (cv >= 0) o.value = trim(r.cells[cv]);
        if (cd >= 0) o.dnp = cell_means_dnp(r.cells[cd], t.cols[cd].invert);
        if (o.dnp) {
            ++p.dnp;
            if (opt.skip_dnp) { ++p.skipped; continue; }
        }
        if (!seen.insert(to_upper(o.ref)).second) p.issues.push_back({line, "duplicate reference " + o.ref});
        maxx = std::max(maxx, o.pos.x);
        if (o.side == Side::Bottom) bottoms.push_back(p.rows.size());
        p.rows.push_back(o);
        p.row_src.push_back((int)i);
    }
    // ---- conventions ----
    const double W = opt.board_width > 0 ? opt.board_width : (maxx > -1e299 ? maxx : 0);
    for (auto& o : p.rows) {
        if (opt.mirror_x) { o.pos.x = -o.pos.x; o.rot = 180.0 - o.rot; }
        if (opt.mirror_y) { o.pos.y = -o.pos.y; o.rot = -o.rot; }
        if (o.side == Side::Bottom) {
            if (opt.bottom_x == BottomX::Negate) o.pos.x = -o.pos.x;
            else if (opt.bottom_x == BottomX::MirrorWidth) o.pos.x = W - o.pos.x;
            switch (opt.bottom_rot) {
            case BottomRot::Negate: o.rot = -o.rot; break;
            case BottomRot::Mirror180: o.rot = 180.0 - o.rot; break;
            case BottomRot::Plus180: o.rot = o.rot + 180.0; break;
            default: break;
            }
        }
        o.rot = normalize_angle(o.rot + opt.rot_offset);
    }
}

bool pnp_preview(const std::string& bytes, const PnpOptions& opt, PnpPreview& out, std::string& err) {
    out = PnpPreview{};
    if (!analyze_text(bytes, opt.layout, Purpose::Pnp, out.text, err)) return false;
    pnp_rebuild(out, opt);
    return true;
}

bool pnp_preview_file(const std::string& path, const PnpOptions& opt, PnpPreview& out, std::string& err) {
    std::string bytes;
    if (!read_file(path, bytes, err)) return false;
    return pnp_preview(bytes, opt, out, err);
}

bool pnp_apply(const PnpPreview& p, Project& prj, const PnpOptions& opt, CentroidReport& rep, std::string& err) {
    rep = CentroidReport{};
    if (!p.mapping_ok()) { err = "column mapping incomplete: need Reference, X and Y"; return false; }
    if (p.rows.empty()) { err = "no rows to import"; return false; }
    // compatibility fields for existing report consumers
    const ParsedText& t = p.text;
    for (auto& c : t.cols) rep.table.columns.push_back(c.header);
    for (auto& r : t.rows) if (r.kind == RowKind::Data) { rep.table.rows.push_back(r.cells); rep.table.row_line.push_back(r.line + 1); }
    rep.table.has_header = t.header_line >= 0;
    rep.table.header_line = t.header_line + 1;
    rep.map.ref = t.col(Role::Ref); rep.map.x = t.col(Role::X); rep.map.y = t.col(Role::Y);
    rep.map.rot = t.col(Role::Rotation); rep.map.side = t.col(Role::Side); rep.map.part = t.col(Role::PartNumber);
    rep.map.package = t.col(Role::Package); rep.map.value = t.col(Role::Value);
    rep.units = p.units;
    rep.units_reason = p.units_reason;
    rep.rows_parsed = p.rows;
    rep.rows = t.data_rows();
    rep.skipped = p.skipped;
    rep.filtered = p.filtered;
    for (auto& i : p.issues) rep.messages.push_back(i.line ? "line " + std::to_string(i.line) + ": " + i.msg : i.msg);
    if (!apply_centroid_rows(prj, opt.apply, rep)) { err = "apply failed"; return false; }
    return true;
}

bool import_pnp_file(const std::string& path, Project& prj, const PnpOptions& opt, CentroidReport& rep,
                     PnpPreview* preview, std::string& err) {
    PnpPreview local;
    PnpPreview& p = preview ? *preview : local;
    if (!pnp_preview_file(path, opt, p, err)) return false;
    return pnp_apply(p, prj, opt, rep, err);
}

} // namespace pcbp::io
