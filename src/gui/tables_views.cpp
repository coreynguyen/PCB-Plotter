// ============================================================================
// tables_views.cpp — Placements, BOM and Reports views (ImGui tables).
//
//   Placements  CircuitCAM "Edit Component/BOM Information" Components page as a
//               spreadsheet: flags (R/N/P/M/D), sortable, filter + quick chips,
//               inline edit, multi-select synced with Component::selected, row
//               context menu, and a Placement Review side panel (placement_check).
//   BOM         Part numbers with computed Qty / compressed refs and flags
//               (U = in BOM not on board, ? = not in BOM, M = package mismatch);
//               "by Part Number" or "by Component" views, inline edit.
//   Reports     Component Prep, Opportunity Count, Routing, Placement list,
//               Placement Review: table + Save CSV / HTML, Copy, GDI Print.
// ============================================================================
#include "gapp.h"
#include "../core/package_edit.h"
#include "../core/fileio.h"
#include "../core/placement_check.h"
namespace pcbp { std::string project_program_name(const Project& p); }   // v17 (core/sides.h; its side_name clashes here)
#include "../io/bom_import2.h"
#include "../ui/form.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace pcbp::gui {
HWND main_hwnd();

namespace {
using app::Doc;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
ImU32 argb_im(uint32_t c) { return (c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16); }
ImU32 alpha(ImU32 c, int a) { return (c & 0x00FFFFFFu) | ((ImU32)a << 24); }
std::string fnum(double v, int dec) { char b[64]; std::snprintf(b, sizeof b, "%.*f", dec, v); return b; }
std::string fmt_rot(double r) {
    std::string s = fnum(norm_deg(r), 2);
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// "R1, R2, R3, R5" -> "R1-R3, R5"
std::string compress_refs(std::vector<std::string> refs) {
    std::sort(refs.begin(), refs.end(), ref_less);
    struct P { std::string pre; long n = 0; bool ok = false; };
    auto split = [](const std::string& r) {
        P p;
        size_t i = r.size();
        while (i > 0 && std::isdigit((unsigned char)r[i - 1])) --i;
        if (i < r.size() && i > 0 && r.size() - i < 9) { p.pre = r.substr(0, i); p.n = std::strtol(r.c_str() + i, nullptr, 10); p.ok = true; }
        return p;
    };
    std::string out;
    size_t i = 0;
    while (i < refs.size()) {
        P a = split(refs[i]);
        size_t j = i;
        if (a.ok)
            while (j + 1 < refs.size()) {
                P b = split(refs[j + 1]);
                if (!b.ok || b.pre != a.pre || b.n != a.n + (long)(j + 1 - i)) break;
                ++j;
            }
        if (!out.empty()) out += ", ";
        if (j - i >= 2) { out += refs[i] + "-" + refs[j]; i = j + 1; }
        else { out += refs[i]; ++i; }
    }
    return out;
}

// content hash of the project ignoring the selection (selection changes bump gen too)
uint64_t content_hash(const Project& p) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* d, size_t n) { const unsigned char* b = (const unsigned char*)d; for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; } };
    auto mixs = [&](const std::string& s) { mix(s.data(), s.size()); mix("\x1f", 1); };
    auto mixd = [&](double v) { mix(&v, sizeof v); };
    for (auto& c : p.comps) {
        mixs(c.ref); mixs(c.number); mixs(c.package);
        mixd(c.pos.x); mixd(c.pos.y); mixd(c.rot);
        uint32_t f = (uint32_t)c.side | (uint32_t)c.feature << 2 | (uint32_t)c.fid_scope << 5 | (uint32_t)c.dnp << 8;
        mix(&f, 4); mix(&c.assigned_point, 4); mix(&c.fid_number, 4);
    }
    for (auto& kv : p.parts) {
        auto& n = kv.second;
        mixs(kv.first); mixs(n.description); mixs(n.package); mixs(n.default_process);
        mixs(n.cust_pn); mixs(n.vend_pn); mixs(n.manf_pn);
        mix(&n.polarized, 1);
    }
    for (auto& kv : p.packages) { mixs(kv.first); size_t np = kv.second.pads.size(); mix(&np, sizeof np); mix(&kv.second.smt, 1); mix(&kv.second.leads, 4); mix(&kv.second.polarized, 1); mix(&kv.second.marker_kind, 1); }
    for (auto& m : p.model.points) { mixs(m.name); mix(&m.color, 4); mix(&m.side, 1); }
    mixd(p.virtual_origin.x); mixd(p.virtual_origin.y);
    size_t nf = p.board_frame.size(); mix(&nf, sizeof nf);
    for (auto& pl : p.board_frame) for (auto& v : pl) { mixd(v.x); mixd(v.y); }
    size_t ni = p.images.size(); mix(&ni, sizeof ni);
    for (auto& im : p.images) mix(&im.x_out, 1);
    int u = app::settings().units; mix(&u, 4);
    mixs(p.info.name); mixs(p.info.number); mixs(p.info.pcb_id);
    return h;
}

std::string point_name(const Project& p, int pt) {
    if (pt < 0 || pt >= (int)p.model.points.size()) return {};
    return p.model.points[pt].name;
}

// coloured chip "● name"; returns width used
void point_chip(const Project& p, int pt) {
    if (pt < 0 || pt >= (int)p.model.points.size()) { ImGui::TextColored(ImColor(th().text_faint), "unassigned"); return; }
    const ModelPoint& mp = p.model.points[pt];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    ImVec2 ts = ImGui::CalcTextSize(mp.name.c_str());
    const float w = ts.x + S(20);
    ImU32 col = argb_im(mp.color | 0xFF000000u);
    dl->AddRectFilled(ImVec2(c.x, c.y - S(1)), ImVec2(c.x + w, c.y + h + S(1)), alpha(col, 0x30), S(8));
    dl->AddCircleFilled(ImVec2(c.x + S(8), c.y + h * 0.5f), S(3.5f), col);
    dl->AddText(ImVec2(c.x + S(15), c.y), th().text, mp.name.c_str());
    ImGui::Dummy(ImVec2(w, h));
}

// small chip button (quick filter); returns clicked
// filter chip: pill with optional icon + count, animated hover / on state
bool chip(const char* label, bool on, int count = -1, ui::Icon ic = ui::Icon::None, const char* tip = nullptr) {
    char buf[96];
    if (count >= 0) std::snprintf(buf, sizeof buf, "%s  %d", label, count); else std::snprintf(buf, sizeof buf, "%s", label);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    const float isz = ic != ui::Icon::None ? std::round(ImGui::GetFontSize()) : 0.f;
    const float ipad = isz > 0 ? isz + S(5) : 0.f;
    ImVec2 sz(ts.x + ipad + S(18), ImGui::GetFrameHeight());
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    bool clk = ImGui::InvisibleButton("##chip", sz);
    bool hov = ImGui::IsItemHovered();
    float h = anim(ImGui::GetID("h"), hov ? 1.f : 0.f, 16.f);
    float o = anim(ImGui::GetID("o"), on ? 1.f : 0.f, 12.f);
    ImGui::PopID();
    if (hov && tip) tooltip(tip);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const GTheme& t = th();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = col_lerp(col_lerp(t.panel2, t.raised, h), alpha(t.accent, 0x40), o);
    dl->AddRectFilled(p, p + sz, bg, sz.y * 0.5f);
    dl->AddRect(p, p + sz, col_lerp(t.border, alpha(t.accent, 0xC0), o), sz.y * 0.5f, 0, 1.f);
    if (isz > 0) icon_draw(dl, ic, ImVec2(p.x + S(8), p.y + (sz.y - isz) * 0.5f), isz, 0.6f + 0.4f * std::max(h, o));
    dl->AddText(ImVec2(p.x + S(9) + ipad, p.y + (sz.y - ts.y) * 0.5f), col_lerp(t.text_dim, t.text, std::max(h, o)), buf);
    return clk;
}

// flag letters drawn as tiny coloured badges
void flag_badges(const std::string& fl) {
    if (fl.empty()) { ImGui::Dummy(ImVec2(1, ImGui::GetTextLineHeight())); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    float x = c.x;
    for (char ch : fl) {
        ImU32 col = th().warn;
        if (ch == 'P' || ch == 'R' || ch == 'X') col = th().error;          // blocks a correct program
        if (ch == 'U' || ch == 'u' || ch == 'I' || ch == 'A') col = th().text_faint;     // informational (A = reviewed / OK, R16c)
        char s[2] = {ch, 0};
        ImVec2 ts = ImGui::CalcTextSize(s);
        float w = std::max(ts.x + S(6), h);
        dl->AddRectFilled(ImVec2(x, c.y), ImVec2(x + w, c.y + h), alpha(col, 0x38), S(4));
        dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, c.y), col, s);
        x += w + S(2);
    }
    ImGui::Dummy(ImVec2(x - c.x, h));
}

// One legend entry per flag letter: badge + plain explanation; hover gives the detail
// and the severity (P1-03). Wraps to the available width.
struct FlagLegend { char ch; const char* label; const char* sev; const char* detail; };
static void flag_legend(const FlagLegend* e, int n, const char* trailer) {
    const GTheme& t = th();
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Flags:");
    for (int k = 0; k < n; ++k) {
        std::string lab = std::string(e[k].label) + "  ";
        const float w = ImGui::GetTextLineHeight() + S(6) + ImGui::CalcTextSize(lab.c_str()).x;
        ImGui::SameLine(0, S(8));
        if (ImGui::GetCursorScreenPos().x + w > right) ImGui::NewLine();
        ImGui::BeginGroup();
        flag_badges(std::string(1, e[k].ch));
        ImGui::SameLine(0, S(4));
        ImGui::TextUnformatted(e[k].label);
        ImGui::EndGroup();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%c = %s  [%s]\n%s", e[k].ch, e[k].label, e[k].sev, e[k].detail);
    }
    if (trailer && *trailer) {
        ImGui::SameLine(0, S(14));
        if (ImGui::GetCursorScreenPos().x + ImGui::CalcTextSize(trailer).x > right) ImGui::NewLine();
        ImGui::TextColored(ImColor(t.text_faint), "%s", trailer);
    }
    ImGui::PopStyleColor();
}
static const FlagLegend kPlacementFlags[] = {
    {'R', "No reference", "Error", "The location has no reference designator."},
    {'N', "No part number", "Warning", "Not linked to a BOM line yet (unassigned)."},
    {'P', "No package", "Error", "No package, or the package is not in the project library: the machine cannot pick it."},
    {'X', "Duplicate reference", "Error", "Another location uses the same reference (case-insensitive)."},
    {'D', "Double location", "Warning", "Another location sits at exactly the same X/Y on the same side."},
    {'A', "Reviewed / OK", "Info", "A duplicate reference or double location that was reviewed and accepted (e.g. a battery holder\n"
                                   "and the battery fitted in it). Right-click > Mark Reviewed / OK; saved in the project. The source data is unchanged."},
};
static const FlagLegend kBomFlags[] = {
    {'U', "Unused", "Info", "In the BOM but no board location uses this part number. Usually a BOM line for a part that is\nnot on this board revision, or a reference that does not exist in the CAD data."},
    {'?', "Not in BOM", "Warning", "Placed on the board with this part number, but the imported BOM has no such line."},
    {'M', "Package disagrees", "Warning", "The BOM SUPPLIED a package for these references and it differs from the board package\n(after name normalisation: 0402 = C0402 = CAP_0402 = CAPC1005X55N). A blank BOM package is never M."},
    {'I', "Package inferred", "Info", "The BOM gave no package; the package shown comes from the board / CAD data (nothing to confirm it)."},
    {'P', "Package missing", "Error", "Neither the BOM nor the board gives a package for these references."},
};

std::string pretty_code(CheckCode c) {
    std::string s = check_code_name(c);
    for (auto& ch : s) if (ch == '_') ch = ' ';
    if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}

void copy_text(const std::string& s) { ImGui::SetClipboardText(s.c_str()); }

std::string csv_field(const std::string& u) {
    if (u.find_first_of(",\"\r\n") == std::string::npos) return u;
    std::string o = "\"";
    for (char c : u) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

// save a UTF-8 string through a (deferred) save dialog
void save_text_file(const std::string& data, const std::wstring& title, const std::wstring& filter_name, const std::wstring& pattern,
                    const std::wstring& ext, const std::wstring& def_name) {
    defer([=] {
        std::wstring path = def_name;
        if (!ui::save_file_dialog(main_hwnd(), title, {{filter_name, pattern}, {L"All files", L"*.*"}}, ext, path)) return;
        auto err = std::make_shared<std::string>();
        std::string up = ui::U(path);
        run_with_progress("Saving file", [data, up, err](Progress& pr) {
            pr.set(-1.f, up.c_str());
            if (!write_file(up, data.data(), data.size(), *err)) throw std::runtime_error("Could not save: " + *err);
        }, [=](bool ok) {
        if (!ok) return;
        std::wstring dir = path;
        size_t sl = dir.find_last_of(L"\\/");
        if (sl != std::wstring::npos) dir.resize(sl);
        std::wstring fn = sl != std::wstring::npos ? path.substr(sl + 1) : path;
        char kb[32]; std::snprintf(kb, sizeof kb, "%.1f KB", data.size() / 1024.0);
        toast("Saved " + ui::U(fn) + " (" + kb + ")", ToastKind::Success, 6.f, "Open folder",
              [dir] { ShellExecuteW(main_hwnd(), L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
        }, ProgressOpts{true, false, 0.25f});
    });
}

const char* side_name(Side s) { return s == Side::Bottom ? "Bottom" : "Top"; }
const char* fid_name(const Component& c) {
    if (c.feature == Feature::Fiducial) {
        // specs/FIDUCIALS.md: Global=Board, Image=Circuit, Local=Component; 1/2/3 -> Fiducial N, 4+ -> Additional.
        static thread_local char buf[48];
        const char* ty = c.fid_scope == FiducialScope::Local ? "Component" : c.fid_scope == FiducialScope::Image ? "Circuit" : "Board";
        if (c.fid_number >= 1 && c.fid_number <= 3) std::snprintf(buf, sizeof buf, "%s - Fiducial %d", ty, c.fid_number);
        else if (c.fid_number >= 4) std::snprintf(buf, sizeof buf, "%s - Additional", ty);
        else std::snprintf(buf, sizeof buf, "%s fiducial", ty);
        return buf;
    }
    if (c.feature == Feature::BadMark) return "Bad mark";
    if (c.feature == Feature::Hole) return "Hole";
    return "";
}

int next_fid_number(const Project& p) {
    int n = 1;
    for (auto& o : p.comps) if (o.feature == Feature::Fiducial) n = std::max(n, o.fid_number + 1);
    return n;
}

// ===========================================================================
// PLACEMENTS
// ===========================================================================
enum PCol { PC_Flags, PC_Ref, PC_X, PC_Y, PC_Rot, PC_Side, PC_Part, PC_Pkg, PC_Point, PC_Dnp, PC_Fid, PC_Desc, PC_Count };
// R16c (REVIEW_V15 §11): column names, default visibility and what each column means
struct PlColMeta { const char* name; bool def_on; float w; const char* tip; };
static const PlColMeta kPlCol[PC_Count] = {
    {"Issues", true, 56, "Validation badges for the row; hover a badge or the legend below for its meaning. Sortable."},
    {"Reference", true, 78, "Reference designator (R1, C5, U3...)."},
    {"X", true, 76, "Placement X, relative to the virtual origin."},
    {"Y", true, 76, "Placement Y, relative to the virtual origin."},
    {"Rotation (\xC2\xB0)", true, 70, "Placement angle in degrees, counter-clockwise, as in the CAD / centroid data."},
    {"Side", true, 56, "Top or Bottom of the board."},
    {"Part Number", true, 130, "Part number linked from the BOM (or the CAD data)."},
    {"Package", true, 110, "Package / footprint name; orange = no package graphics in the project yet."},
    {"Export Layer", false, 120, "Machine-program export layer the placement is assigned to (Export tab). Not part of the CSV."},
    {"Placement State", false, 96, "DNP (do not place), Not in BOM (orphan) or Unassigned (no part number). Empty = placed normally."},
    {"Fiducial Type", false, 110, "Board / circuit / component fiducial, bad mark or hole. Empty for normal parts."},
    {"Value / Description", true, 220, "Part description from the BOM / part library."},
};
enum QFilter { QF_All, QF_Issues, QF_Unassigned, QF_Top, QF_Bottom, QF_Dnp, QF_Fid, QF_Count };
const char* kQNames[QF_Count] = {"All", "Issues", "Unassigned", "Top", "Bottom", "DNP", "Fiducials"};

struct PlacementsView : View {
    ViewKind kind() const override { return ViewKind::Placements; }
    const char* title() const override { return "Placements"; }
    ui::Icon icon() const override { return ui::Icon::Locations; }

    // cached model
    uint64_t gen_seen = 0, hash_seen = 0;
    std::vector<int> rows;                 // comp indices shown, sorted
    std::vector<std::string> flags;        // per comp
    std::vector<uint8_t> has_issue;        // per comp (any placement_check issue)
    std::vector<PlacementIssue> issues;
    int qcount[QF_Count] = {};
    bool dirty_rows = true, dirty_check = true;
    char filter[128] = "";
    int qf = QF_All;
    int sort_col = PC_Ref; bool sort_asc = true;
    PlacementCheckOptions copt;
    bool show_review = true;
    int anchor = -1;                       // row index for shift-select
    // inline editing
    uint32_t ed_id = 0; int ed_col = -1; char ed_buf[256] = ""; bool ed_focus = false;
    uint32_t pick_id = 0; int pick_col = -1; bool open_pick = false;
    bool open_ctx = false;
    int scroll_to_row = -1;
    bool col_on[PC_Count] = {}, col_apply = false;   // R16c column chooser (synced from the table each frame)
    std::set<int> open_groups;

    int find_id(uint32_t id) const {
        for (size_t i = 0; i < doc->prj.comps.size(); ++i) if (doc->prj.comps[i].id == id) return (int)i;
        return -1;
    }

    std::string cell_text(const Component& c, int col) const {
        const Project& p = doc->prj;
        switch (col) {
        case PC_Ref: return c.ref;
        case PC_X: return fmt_len_u8(p.user_pos(c).x, false);
        case PC_Y: return fmt_len_u8(p.user_pos(c).y, false);
        case PC_Rot: return fmt_rot(c.rot);
        case PC_Side: return side_name(c.side);
        case PC_Part: return c.number;
        case PC_Pkg: return c.package;
        case PC_Point: return point_name(p, c.assigned_point);
        case PC_Dnp: return c.feature != Feature::Component ? std::string() : comp_state(c) == CompState::Placed ? std::string() : comp_state_name(comp_state(c));
        case PC_Fid: return fid_name(c);
        case PC_Desc: { const PartNumber* pn = p.part_of(c); return pn ? pn->description : std::string(); }
        }
        return {};
    }

    void recheck() {
        const Project& p = doc->prj;
        issues = check_placements(p, copt);
        // R16c (REVIEW_V15 §11): duplicate refs / coincident locations whose rows are all marked Reviewed / OK drop out
        issues.erase(std::remove_if(issues.begin(), issues.end(), [&](const PlacementIssue& is) {
            if (is.code != CheckCode::DuplicateRef && is.code != CheckCode::Coincident && is.code != CheckCode::NearCoincident && is.code != CheckCode::Overlap) return false;
            if (is.comps.empty()) return false;
            for (int i : is.comps) if (i < 0 || i >= (int)p.comps.size() || !p.comps[i].review_ok) return false;
            return true;
        }), issues.end());
        has_issue.assign(p.comps.size(), 0);
        for (auto& is : issues) {
            if (is.severity == Severity::Info) continue;
            for (int i : is.comps) if (i >= 0 && i < (int)has_issue.size()) has_issue[i] = 1;
        }
        // CircuitCAM error flags
        std::map<std::string, int> refcount;
        std::map<std::pair<long long, long long>, int> poscount;
        auto pkey = [](const Component& c) { return std::make_pair(std::llround(c.pos.x * 1000) * 2 + (int)c.side, std::llround(c.pos.y * 1000)); };
        for (auto& c : p.comps) { if (!c.ref.empty()) refcount[upper(c.ref)]++; poscount[pkey(c)]++; }
        flags.assign(p.comps.size(), "");
        for (size_t i = 0; i < p.comps.size(); ++i) {
            const Component& c = p.comps[i];
            std::string& fl = flags[i];
            if (c.ref.empty()) fl += 'R';
            if (c.feature == Feature::Component) {
                if (c.number.empty()) fl += 'N';
                if (!p.package_of(c)) fl += 'P';
            }
            const bool dup = !c.ref.empty() && refcount[upper(c.ref)] > 1, dbl = poscount[pkey(c)] > 1;
            if (c.review_ok && (dup || dbl)) fl += 'A';   // R16c: reviewed / accepted; stays visible, no longer an issue
            else {
                if (dup) fl += 'X';   // duplicate ref ('M' is the BOM package flag)
                if (dbl) fl += 'D';
            }
            if (!fl.empty()) has_issue[i] = 1;
        }
        dirty_check = false;
        dirty_rows = true;
    }

    bool pass_quick(const Component& c, size_t i, int f) const {
        switch (f) {
        case QF_Issues: return i < has_issue.size() && has_issue[i];
        case QF_Unassigned: return c.feature == Feature::Component && c.assigned_point < 0;
        case QF_Top: return c.side == Side::Top;
        case QF_Bottom: return c.side == Side::Bottom;
        case QF_Dnp: return c.dnp;
        case QF_Fid: return c.feature == Feature::Fiducial;
        }
        return true;
    }

    void rebuild_rows() {
        const Project& p = doc->prj;
        const std::string flt = lower(trim(filter));
        rows.clear();
        for (int q = 0; q < QF_Count; ++q) qcount[q] = 0;
        for (size_t i = 0; i < p.comps.size(); ++i) {
            const Component& c = p.comps[i];
            for (int q = 0; q < QF_Count; ++q) if (pass_quick(c, i, q)) qcount[q]++;
            if (!pass_quick(c, i, qf)) continue;
            if (!flt.empty()) {
                bool hit = false;
                for (int col = PC_Ref; col < PC_Count && !hit; ++col) if (lower(cell_text(c, col)).find(flt) != std::string::npos) hit = true;
                if (!hit && lower(flags[i]).find(flt) != std::string::npos) hit = true;
                if (!hit) continue;
            }
            rows.push_back((int)i);
        }
        const int col = sort_col; const bool asc = sort_asc;
        auto key_less = [&](int a, int b) -> bool {
            const Component& A = p.comps[a]; const Component& B = p.comps[b];
            switch (col) {
            case PC_Flags: if (flags[a] != flags[b]) return flags[a] > flags[b]; break;
            case PC_X: if (A.pos.x != B.pos.x) return A.pos.x < B.pos.x; break;
            case PC_Y: if (A.pos.y != B.pos.y) return A.pos.y < B.pos.y; break;
            case PC_Rot: if (norm_deg(A.rot) != norm_deg(B.rot)) return norm_deg(A.rot) < norm_deg(B.rot); break;
            case PC_Side: if (A.side != B.side) return A.side < B.side; break;
            case PC_Point: if (A.assigned_point != B.assigned_point) return (unsigned)A.assigned_point < (unsigned)B.assigned_point; break;
            case PC_Dnp: if (A.dnp != B.dnp) return A.dnp > B.dnp; break;
            case PC_Fid: if (A.feature != B.feature) return A.feature > B.feature; break;
            case PC_Ref: break;
            default: { std::string x = cell_text(A, col), y = cell_text(B, col); if (x != y) return ref_less(x, y); }
            }
            if (A.ref != B.ref) return ref_less(A.ref, B.ref);
            return a < b;
        };
        std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) { return asc ? key_less(a, b) : key_less(b, a); });
        dirty_rows = false;
    }

    void sync() {
        if (doc->gen != gen_seen) {
            gen_seen = doc->gen;
            uint64_t h = content_hash(doc->prj);
            if (h != hash_seen || flags.size() != doc->prj.comps.size()) { hash_seen = h; dirty_check = true; }
        }
        if (dirty_check) recheck();
        if (dirty_rows) rebuild_rows();
    }

    void sel_changed() { doc->changed(ui::CH_SELECTION); }

    std::vector<int> selected_idx() const {
        std::vector<int> r;
        for (size_t i = 0; i < doc->prj.comps.size(); ++i) if (doc->prj.comps[i].selected) r.push_back((int)i);
        return r;
    }

    void select_only(const std::vector<int>& idx) {
        for (auto& c : doc->prj.comps) c.selected = false;
        for (int i : idx) if (i >= 0 && i < (int)doc->prj.comps.size()) doc->prj.comps[i].selected = true;
        sel_changed();
    }

    void scroll_to_comp(int ci) {
        for (size_t r = 0; r < rows.size(); ++r) if (rows[r] == ci) { scroll_to_row = (int)r; return; }
    }

    // ---- editing --------------------------------------------------------
    static bool is_text_col(int c) { return c == PC_Ref || c == PC_X || c == PC_Y || c == PC_Rot || c == PC_Part || c == PC_Pkg || c == PC_Desc; }

    void begin_edit(int ci, int col) {
        if (ci < 0) return;
        Component& c = doc->prj.comps[ci];
        if (is_text_col(col)) {
            if (col == PC_Desc && c.number.empty()) { toast("Assign a part number before editing its description", ToastKind::Warning); return; }
            ed_id = c.id; ed_col = col; ed_focus = true;
            std::snprintf(ed_buf, sizeof ed_buf, "%s", cell_text(c, col).c_str());
        } else if (col == PC_Dnp) {
            doc->checkpoint("Toggle DNP");
            c.dnp = !c.dnp;
            doc->changed(ui::CH_GEOMETRY);
        } else if (col == PC_Side || col == PC_Point || col == PC_Fid) {
            pick_id = c.id; pick_col = col; open_pick = true;
        }
    }

    void commit_edit() {
        int ci = find_id(ed_id);
        const int col = ed_col;
        ed_id = 0; ed_col = -1;
        if (ci < 0) return;
        Project& p = doc->prj;
        Component& c = p.comps[ci];
        const std::string t = trim(ed_buf);
        unsigned fl = ui::CH_GEOMETRY;
        switch (col) {
        case PC_Ref:
            if (t == c.ref) return;
            doc->checkpoint("Edit reference");
            {
                std::string was = c.ref;
                c.ref = t;
                bool dup = false;
                for (auto& o : p.comps) if (&o != &c && !t.empty() && upper(o.ref) == upper(t)) { dup = true; break; }
                if (dup) toast("Renamed " + was + " \xE2\x86\x92 " + t + " - reference is used more than once", ToastKind::Warning, 5.f);
                else toast("Renamed " + was + " \xE2\x86\x92 " + t, ToastKind::Success, 3.f, "Undo", [] { post_command(app::ID_EDIT_UNDO); });
            }
            break;
        case PC_X: case PC_Y: {
            double v = app::parse_len(ui::W(t), std::nan(""));
            if (v != v) { toast("Not a number: " + t, ToastKind::Error); flash(cell_key(c.id, -1)); return; }
            { const Vec2 u0 = p.user_pos(c); v = col == PC_X ? p.from_user({v, u0.y}).x : p.from_user({u0.x, v}).y; }
            double& cur = col == PC_X ? c.pos.x : c.pos.y;
            if (std::fabs(v - cur) < 1e-9) return;
            doc->checkpoint("Move location");
            cur = v;
            break;
        }
        case PC_Rot: {
            double v = ui::parse_num(ui::W(t), std::nan(""));
            if (v != v) { toast("Not a number: " + t, ToastKind::Error); flash(cell_key(c.id, -1)); return; }
            v = norm_deg(v);
            if (std::fabs(v - norm_deg(c.rot)) < 1e-9) return;
            doc->checkpoint("Rotate location");
            c.rot = v;
            break;
        }
        case PC_Part:
            if (t == c.number) return;
            doc->checkpoint("Change part number");
            c.number = t;
            c.pn_src = DataSrc::User;
            if (c.package.empty()) if (const PartNumber* pn = p.part_of(c)) { c.package = pn->package; if (!c.package.empty()) c.pkg_src = DataSrc::Library; }
            break;
        case PC_Pkg:
            if (t == c.package) return;
            doc->checkpoint("Change package");
            c.package = t;
            c.pkg_src = DataSrc::User;
            if (!t.empty() && !p.packages.count(t)) toast("Package " + t + " is not in the package library yet", ToastKind::Warning, 5.f);
            break;
        case PC_Desc: {
            PartNumber& pn = p.parts[c.number];
            if (pn.number.empty()) pn.number = c.number;
            if (pn.description == t) return;
            doc->checkpoint("Edit description");
            pn.description = t;
            fl = ui::CH_INFO;
            break;
        }
        default: return;
        }
        flash(cell_key(c.id, col));
        doc->changed(fl);
    }
    static ImGuiID cell_key(int id, int col) { return (ImGuiID)id * 2654435761u ^ (ImGuiID)(col + 101) * 40503u ^ 0x7AB1E5u; }

    // apply to the selection (or one comp)
    template <class F> void for_sel(const char* label, unsigned fl, F fn) {
        auto sel = selected_idx();
        if (sel.empty()) return;
        doc->checkpoint(label);
        for (int i : sel) fn(doc->prj.comps[i]);
        doc->changed(fl);
    }

    void delete_selected() {
        auto sel = selected_idx();
        if (sel.empty()) return;
        doc->checkpoint(sel.size() == 1 ? "Delete " + doc->prj.comps[sel[0]].ref : "Delete " + std::to_string(sel.size()) + " locations");
        auto& v = doc->prj.comps;
        v.erase(std::remove_if(v.begin(), v.end(), [](const Component& c) { return c.selected; }), v.end());
        doc->changed(ui::CH_GEOMETRY);
        toast(std::to_string(sel.size()) + " location(s) deleted", ToastKind::Info, 5.f, "Undo", [] { post_command(app::ID_EDIT_UNDO); });
    }

    void toggle_fiducial() {
        auto sel = selected_idx();
        if (sel.empty()) return;
        bool make = false;
        for (int i : sel) if (doc->prj.comps[i].feature != Feature::Fiducial) make = true;
        doc->checkpoint(make ? "Make fiducial" : "Clear fiducial");
        for (int i : sel) {
            Component& c = doc->prj.comps[i];
            if (make) {
                if (c.feature != Feature::Fiducial) { c.fid_number = next_fid_number(doc->prj); c.fid_scope = FiducialScope::Global; }
                c.feature = Feature::Fiducial;
            } else { c.feature = Feature::Component; c.fid_number = 0; }
        }
        doc->changed(ui::CH_GEOMETRY);
    }

    void assign_sel(int pt) {
        for_sel(pt < 0 ? "Unassign" : "Assign", ui::CH_ASSIGN, [&](Component& c) { c.assigned_point = pt; });
    }

    void edit_location(uint32_t id) {
        auto d = doc;
        defer([d, id] {
            for (auto& c : d->prj.comps) if (c.id == id) {
                if (app::dlg_location(main_hwnd(), *d, c)) d->changed(ui::CH_GEOMETRY);
                return;
            }
        });
    }

    void fly(const std::vector<int>& idx, bool to_plotter) {
        if (idx.empty()) return;
        focus_components(doc, idx, true);
        if (to_plotter) activate_view(doc, ViewKind::Plotter);
    }

    // ---- view API --------------------------------------------------------
    bool on_command(int id) override {
        switch (id) {
        case app::ID_EDIT_SELECT_ALL: {
            for (auto& c : doc->prj.comps) c.selected = false;
            for (int i : rows) doc->prj.comps[i].selected = true;
            sel_changed();
            return true;
        }
        case app::ID_EDIT_DELETE: delete_selected(); return true;
        case app::ID_EDIT_COPY: {
            std::string s = "Reference\tX\tY\tRotation\tSide\tPart Number\tPackage\tExport Layer\tPlacement State\tFiducial Type\tDescription\r\n";
            for (int i : rows) {
                const Component& c = doc->prj.comps[i];
                if (!c.selected) continue;
                for (int col = PC_Ref; col < PC_Count; ++col) { if (col > PC_Ref) s += '\t'; s += cell_text(c, col); }
                s += "\r\n";
            }
            copy_text(s);
            toast("Copied selected rows", ToastKind::Success);
            return true;
        }
        }
        return false;
    }
    int cmd_state(int id) override {
        switch (id) {
        case app::ID_EDIT_SELECT_ALL: return 1;
        case app::ID_EDIT_DELETE: case app::ID_EDIT_COPY: return doc->selected_count() > 0 ? 1 : 0;
        }
        return -1;
    }
    std::string status() const override {
        int sel = 0;
        for (auto& c : doc->prj.comps) sel += c.selected;
        return std::to_string(rows.size()) + " of " + std::to_string(doc->prj.comps.size()) + " locations  \xC2\xB7  " + std::to_string(sel) + " selected";
    }

    void draw_toolbar() override {
        const bool any = doc->selected_count() > 0;
        if (icon_button("pl_add", ui::Icon::Locations, "Add location...")) post_command(app::ID_BOARD_INSERT_FREE);
        ImGui::SameLine(0, S(2));
        if (icon_button("pl_del", ui::Icon::Delete, "Delete selected locations", false, any)) delete_selected();
        ImGui::SameLine(0, S(2));
        if (icon_button("pl_fly", ui::Icon::ZoomRect, "Fly to selection in the Plotter", false, any)) fly(selected_idx(), true);
        ImGui::SameLine(0, S(2));
        if (icon_button("pl_fid", ui::Icon::Fiducial, "Toggle fiducial", false, any)) toggle_fiducial();
        ImGui::SameLine(0, S(2));
        if (icon_button("pl_rev", ui::Icon::Report, "Placement Review panel", show_review)) show_review = !show_review;
        ImGui::SameLine(0, S(2));
        if (icon_button("pl_cols", ui::Icon::Settings, "Columns: show / hide (Export Layer, State, Fiducial Type...)")) ImGui::OpenPopup("##plcols");   // R16c
        if (ImGui::BeginPopup("##plcols")) {
            ImGui::TextDisabled("Columns");
            ImGui::Separator();
            for (int col = 0; col < PC_Count; ++col) {
                bool on = col_on[col];
                ImGui::BeginDisabled(col == PC_Ref);
                if (ImGui::Checkbox(kPlCol[col].name, &on)) { col_on[col] = on; col_apply = true; }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", kPlCol[col].tip);
            }
            ImGui::Separator();
            if (ImGui::Selectable("Restore defaults", false, ImGuiSelectableFlags_DontClosePopups)) { for (int col = 0; col < PC_Count; ++col) col_on[col] = kPlCol[col].def_on; col_apply = true; }
            ImGui::EndPopup();
        }
    }

    void draw(ImVec2 size) override {
        sync();
        const GTheme& t = th();
        const float review_w = show_review ? std::min(S(340), size.x * 0.4f) : 0;
        ImGui::BeginChild("##pl_main", ImVec2(size.x - review_w - (show_review ? S(6) : 0), size.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        draw_filter_bar();
        if (doc->prj.comps.empty()) {
            if (empty_state(ui::Icon::Locations, "No placements yet", "Import CAD or centroid data, or add locations by hand.", "Import CAD...", ui::Icon::ImportCad))
                post_command(app::ID_IMPORT_CAD);
        } else {
            draw_table();
            draw_legend();
        }
        ImGui::EndChild();
        if (show_review) {
            ImGui::SameLine(0, S(6));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(8));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
            ImGui::BeginChild("##pl_review", ImVec2(review_w, size.y), ImGuiChildFlags_AlwaysUseWindowPadding);
            draw_review();
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        }
    }

    void draw_filter_bar() {
        ImGui::Dummy(ImVec2(0, S(2)));
        if (search_box("##plf", filter, sizeof filter, "Filter ref, part, package, point...", S(260))) dirty_rows = true;
        for (int q = 0; q < QF_Count; ++q) {
            ImGui::SameLine(0, q == 0 ? S(12) : S(4));
            static const ui::Icon qi[QF_Count] = {ui::Icon::SelectAll, ui::Icon::Warning, ui::Icon::ShowUnassigned, ui::Icon::ViewTop,
                                                  ui::Icon::ViewBottom, ui::Icon::EyeOff, ui::Icon::Fiducial};
            if (chip(kQNames[q], qf == q, qcount[q], qi[q])) { qf = q; dirty_rows = true; }
        }
        ImGui::Dummy(ImVec2(0, S(2)));
    }

    void draw_legend() {
        ImGui::PushStyleColor(ImGuiCol_Text, th().text_faint);
        ImGui::PopStyleColor();
        flag_legend(kPlacementFlags, (int)(sizeof kPlacementFlags / sizeof kPlacementFlags[0]), "Double-click a cell to edit.");
        return;
        ImGui::PopStyleColor();
    }

    void draw_table() {
        Project& p = doc->prj;
        const GTheme& t = th();
        ImGuiTableFlags tf = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                             ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable |
                             ImGuiTableFlags_SizingFixedFit;
        const float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2 - S(4);   // legend may wrap to 2 lines
        // R16c (REVIEW_V15 §11): plain column names; nonessential columns hidden by default (Columns chooser / header right-click)
        if (!ImGui::BeginTable("##placements_v16", PC_Count, tf, ImVec2(0, h))) return;
        const std::string u = " (" + ui::U(app::units_label()) + ")";
        ImGui::TableSetupScrollFreeze(2, 1);
        for (int col = 0; col < PC_Count; ++col) {
            ImGuiTableColumnFlags cf = col == PC_Desc ? ImGuiTableColumnFlags_WidthStretch : ImGuiTableColumnFlags_WidthFixed;
            if (col == PC_Ref) cf |= ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_NoHide;
            if (!kPlCol[col].def_on) cf |= ImGuiTableColumnFlags_DefaultHide;
            std::string nm = kPlCol[col].name;
            if (col == PC_X || col == PC_Y) nm += u;
            ImGui::TableSetupColumn(nm.c_str(), cf, S(kPlCol[col].w));
        }
        if (col_apply) { for (int col = 0; col < PC_Count; ++col) ImGui::TableSetColumnEnabled(col, col_on[col]); col_apply = false; }
        else for (int col = 0; col < PC_Count; ++col) col_on[col] = (ImGui::TableGetColumnFlags(col) & ImGuiTableColumnFlags_IsEnabled) != 0;
        // header row with a plain-language tooltip per column
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int col = 0; col < PC_Count; ++col) {
            if (!ImGui::TableSetColumnIndex(col)) continue;
            ImGui::TableHeader(ImGui::TableGetColumnName(col));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kPlCol[col].tip);
        }
        if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs()) {
            if (ss->SpecsDirty && ss->SpecsCount > 0) {
                sort_col = ss->Specs[0].ColumnIndex;
                sort_asc = ss->Specs[0].SortDirection != ImGuiSortDirection_Descending;
                dirty_rows = true;
                ss->SpecsDirty = false;
                rebuild_rows();
            }
        }
        const float row_h = std::round(ImGui::GetTextLineHeight() + S(9));
        if (scroll_to_row >= 0) {
            float y = scroll_to_row * row_h;
            float vis = ImGui::GetWindowHeight() - row_h * 2;
            if (y < ImGui::GetScrollY() || y > ImGui::GetScrollY() + vis) ImGui::SetScrollY(std::max(0.f, y - vis * 0.3f));
            scroll_to_row = -1;
        }
        int ctx_row = -1;
        bool click_handled = false;
        ImGuiListClipper clip;
        clip.Begin((int)rows.size(), row_h);
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const int ci = rows[r];
                if (ci >= (int)p.comps.size()) continue;
                Component& c = p.comps[ci];
                ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);
                ImGui::PushID((int)c.id);
                ImGui::TableSetColumnIndex(0);
                const float ty = (row_h - ImGui::GetTextLineHeight()) * 0.5f;
                ImGui::PushStyleColor(ImGuiCol_Header, alpha(t.accent, 0x50));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0);
                ImGui::Selectable("##row", c.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                                  ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, row_h));
                ImGui::PopStyleColor(2);
                const bool hov = ImGui::IsItemHovered();
                {
                    float hv = anim(ImGui::GetID("hv"), hov ? 1.f : 0.f, 18.f);
                    float fs = anim(ImGui::GetID("sf"), c.selected ? 0.f : 1.f, 10.f);   // selection flash: fades in from bright
                    ImU32 bg = 0;
                    if (hv > 0.01f) bg = col_alpha(t.accent, 0.12f * hv);
                    if (c.selected && fs > 0.02f) bg = col_alpha(t.accent, 0.30f * fs + 0.12f * hv);
                    float fe = flash_amount(cell_key(c.id, -1));
                    if (fe > 0.01f) bg = col_alpha(t.error, 0.35f * fe);
                    if (bg) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, bg);
                }
                if (hov && !click_handled) {
                    int hc = ImGui::TableGetHoveredColumn();
                    if (ImGui::IsMouseDoubleClicked(0)) { begin_edit(ci, hc); click_handled = true; }
                    else if (ImGui::IsMouseClicked(0)) { click_row(r); click_handled = true; }
                    else if (ImGui::IsMouseClicked(1)) { ctx_row = r; click_handled = true; }
                }
                ImGui::SameLine(0, 0);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
                flag_badges(flags[ci]);
                if (!flags[ci].empty() && ImGui::IsItemHovered()) {   // R16c: what each badge on this row means
                    std::string tip;
                    for (char fch : flags[ci])
                        for (auto& e : kPlacementFlags) if (e.ch == fch) tip += std::string(1, fch) + " = " + e.label + "  [" + e.sev + "]\n" + e.detail + "\n\n";
                    if (flags[ci].find('X') != std::string::npos || flags[ci].find('D') != std::string::npos)
                        tip += "Legitimate (e.g. a holder and the part in it)? Right-click > Mark Reviewed / OK.";
                    if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
                }
                for (int col = PC_Ref; col < PC_Count; ++col) {
                    if (!ImGui::TableSetColumnIndex(col)) continue;
                    if (ed_id == c.id && ed_col == col) {
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (ed_focus) { ImGui::SetKeyboardFocusHere(); ed_focus = false; }
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(4), std::max(0.f, (row_h - ImGui::GetFontSize()) * 0.5f - S(1))));
                        ImGui::InputText("##ed", ed_buf, sizeof ed_buf, ImGuiInputTextFlags_AutoSelectAll);
                        ImGui::PopStyleVar();
                        if (ImGui::IsItemDeactivated()) {
                            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { ed_id = 0; ed_col = -1; }
                            else commit_edit();
                        }
                        continue;
                    }
                    if (float fc = flash_amount(cell_key(c.id, col)); fc > 0.01f) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, col_alpha(t.ok, 0.45f * fc));
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
                    switch (col) {
                    case PC_Point: point_chip(p, c.assigned_point); break;
                    case PC_Dnp: {
                        if (c.feature != Feature::Component) break;
                        const CompState cs = comp_state(c);
                        if (cs == CompState::Dnp) {
                            ImGui::TextColored(ImColor(t.warn), "DNP");
                            tooltip(c.bom_state == BomState::Dnp ? "DNP: explicitly not populated in the BOM.\nKept on the board with its own package; no part number; not placed."
                                                                 : "DNP: set by hand (not from a BOM).");
                        } else if (cs == CompState::Orphan) {
                            ImGui::TextColored(ImColor(t.error), "Not in BOM");
                            tooltip("Orphan: on the board but missing from the imported BOM.\nNot treated as DNP. Reconcile with the customer or the BOM.");
                        } else if (cs == CompState::Unassigned) {
                            ImGui::TextColored(ImColor(t.text_dim), "Unassigned");
                            tooltip("Unassigned: no part number yet.");
                        }
                        break;
                    }
                    case PC_Fid:
                        if (c.feature == Feature::Fiducial) {
                            icon_image(ui::Icon::Fiducial, ImGui::GetTextLineHeight());
                            ImGui::SameLine(0, S(3));
                            ImGui::TextUnformatted(fid_name(c));
                        } else if (c.feature != Feature::Component) ImGui::TextUnformatted(fid_name(c));
                        break;
                    case PC_Side:
                        ImGui::TextColored(ImColor(c.side == Side::Bottom ? t.accent : t.text_dim), "%s", side_name(c.side));
                        break;
                    case PC_Ref:
                        if (c.ref.empty()) ImGui::TextColored(ImColor(t.error), "(none)");
                        else { ImGui::PushFont(g_font_bold); ImGui::TextUnformatted(c.ref.c_str()); ImGui::PopFont(); }
                        break;
                    case PC_X: case PC_Y: case PC_Rot: {
                        std::string s = cell_text(c, col);
                        float w = ImGui::CalcTextSize(s.c_str()).x;
                        float cw = ImGui::GetColumnWidth();
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, cw - w - S(4)));
                        ImGui::TextUnformatted(s.c_str());
                        break;
                    }
                    case PC_Pkg: {
                        const bool disagree = !c.bom_package.empty() && !c.package.empty() && !packages_equivalent(c.bom_package, c.package);
                        if (c.feature == Feature::Component && !p.package_of(c)) ImGui::TextColored(ImColor(t.warn), "%s", c.package.empty() ? "-" : c.package.c_str());
                        else if (disagree) ImGui::TextColored(ImColor(t.warn), "%s", c.package.c_str());
                        else ImGui::TextUnformatted(c.package.c_str());
                        if (c.feature == Feature::Component && ImGui::IsItemHovered()) {
                            // package provenance (REVIEW_V9 §11G / §19)
                            std::string tip = "Package: " + (c.package.empty() ? std::string("(none)") : c.package) + "\nSource: " + data_src_name(c.pkg_src);
                            if (c.pkg_src == DataSrc::Unknown) tip += " (CAD import or earlier project data)";
                            if (!c.bom_package.empty()) tip += "\nBOM lists: " + c.bom_package + (disagree ? "  \xE2\x80\x94 disagrees" : "  \xE2\x80\x94 agrees");
                            else if (c.bom_state != BomState::NotChecked) tip += "\nBOM: no package value (nothing to compare)";
                            if (const PartNumber* pn = p.part_of(c); pn && !pn->package.empty()) tip += "\nPart library lists: " + pn->package;
                            ImGui::SetTooltip("%s", tip.c_str());
                        }
                        break;
                    }
                    case PC_Part: {
                        ImGui::TextUnformatted(c.number.c_str());
                        if (c.feature == Feature::Component && ImGui::IsItemHovered()) {
                            std::string tip = "Part number: " + (c.number.empty() ? std::string("(none)") : c.number) + "\nSource: " + data_src_name(c.pn_src);
                            if (c.pn_src == DataSrc::Unknown) tip += " (CAD import or earlier project data)";
                            if (c.bom_state == BomState::NotChecked) tip += "\nNo BOM imported yet";
                            else if (c.bom_state == BomState::Dnp) tip += "\nBOM: DNP (no part number assigned on purpose)";
                            else if (c.bom_state == BomState::Orphan) tip += "\nBOM: this reference is not in the imported BOM";
                            ImGui::SetTooltip("%s", tip.c_str());
                        }
                        break;
                    }
                    case PC_Desc: ImGui::TextColored(ImColor(t.text_dim), "%s", cell_text(c, col).c_str()); break;
                    default: ImGui::TextUnformatted(cell_text(c, col).c_str());
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
        // background click clears selection
        if (!click_handled && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(0) &&
            !ImGui::IsAnyItemHovered() && ImGui::GetIO().MousePos.y > ImGui::GetItemRectMin().y + row_h) {
            // keep selection (clicking empty area / scrollbars is common); no-op
        }
        if (ctx_row >= 0) {
            int ci = rows[ctx_row];
            if (!p.comps[ci].selected) { select_only({ci}); anchor = ctx_row; }
            open_ctx = true;
        }
        if (open_ctx) { ImGui::OpenPopup("##plctx"); open_ctx = false; }
        draw_context_menu();
        if (open_pick) { ImGui::OpenPopup("##plpick"); open_pick = false; }
        draw_picker();
        // keyboard: Enter / F2 edits first selected row's ref
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_F2) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                for (int r : rows) if (p.comps[r].selected) { begin_edit(r, PC_Ref); break; }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { doc->select_none(); sel_changed(); }
        }
    }

    void click_row(int r) {
        Project& p = doc->prj;
        const ImGuiIO& io = ImGui::GetIO();
        const int ci = rows[r];
        if (io.KeyShift && anchor >= 0 && anchor < (int)rows.size()) {
            if (!io.KeyCtrl) for (auto& c : p.comps) c.selected = false;
            int a = std::min(anchor, r), b = std::max(anchor, r);
            for (int i = a; i <= b; ++i) p.comps[rows[i]].selected = true;
        } else if (io.KeyCtrl) {
            p.comps[ci].selected = !p.comps[ci].selected;
            anchor = r;
        } else {
            for (auto& c : p.comps) c.selected = false;
            p.comps[ci].selected = true;
            anchor = r;
        }
        if (ed_id && find_id(ed_id) != ci) commit_edit();
        sel_changed();
    }

    void draw_context_menu() {
        if (!ImGui::BeginPopup("##plctx")) return;
        Project& p = doc->prj;
        auto sel = selected_idx();
        const int n = (int)sel.size();
        ImGui::TextDisabled("%d location%s", n, n == 1 ? "" : "s");
        ImGui::Separator();
        if (menu_item_icon("Edit Placement...", ui::Icon::CompInfo, "Dbl-click", false, n == 1)) edit_location(p.comps[sel[0]].id);
        if (menu_item_icon("Fly to in Plotter", ui::Icon::ZoomRect, nullptr, false, n > 0)) fly(sel, true);
        if (begin_menu_icon("Assign to point", ui::Icon::Assign, n > 0 && !p.model.points.empty())) {
            for (size_t i = 0; i < p.model.points.size(); ++i) {
                const ModelPoint& mp = p.model.points[i];
                ImVec2 cp = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(cp.x + S(5), cp.y + ImGui::GetTextLineHeight() * 0.5f), S(4), argb_im(mp.color | 0xFF000000u));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(14));
                if (ImGui::MenuItem((mp.name + "##pt" + std::to_string(i)).c_str())) assign_sel((int)i);
            }
            ImGui::Separator();
            if (menu_item_icon("Unassign", ui::Icon::Unassign)) assign_sel(-1);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (menu_item_icon("Toggle DNP", ui::Icon::EyeOff, nullptr, false, n > 0)) {
            bool any_off = false;
            for (int i : sel) if (!p.comps[i].dnp) any_off = true;
            for_sel("Toggle DNP", ui::CH_GEOMETRY, [&](Component& c) { c.dnp = any_off; });
        }
        if (menu_item_icon("Make fiducial / component", ui::Icon::Fiducial, nullptr, false, n > 0)) toggle_fiducial();
        {   // R16c (REVIEW_V15 §11): acknowledge a legitimate duplicate reference / double location
            bool all_ok = n > 0;
            for (int i : sel) if (!p.comps[i].review_ok) all_ok = false;
            if (menu_item_icon("Mark Reviewed / OK", ui::Icon::Check, nullptr, all_ok, n > 0)) {
                const bool v = !all_ok;
                for_sel(v ? "Mark reviewed" : "Clear reviewed", ui::CH_GEOMETRY, [v](Component& c) { c.review_ok = v; });
                dirty_check = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Accept a duplicate reference or double location after checking it (e.g. a battery holder and the battery in it).\n"
                                  "The rows keep their data and show an 'A' badge instead of X / D; the warning no longer counts as an issue.\n"
                                  "Saved in the project; undoable. Click again to clear.");
        }
        if (begin_menu_icon("Side", ui::Icon::Flip, n > 0)) {
            if (menu_item_icon("Top", ui::Icon::ViewTop)) for_sel("Set side", ui::CH_GEOMETRY, [](Component& c) { c.side = Side::Top; });
            if (menu_item_icon("Bottom", ui::Icon::ViewBottom)) for_sel("Set side", ui::CH_GEOMETRY, [](Component& c) { c.side = Side::Bottom; });
            ImGui::EndMenu();
        }
        if (menu_item_icon("Copy rows", ui::Icon::Copy, "Ctrl+C", false, n > 0)) on_command(app::ID_EDIT_COPY);
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, th().error);
        if (menu_item_icon("Delete", ui::Icon::Delete, "Del", false, n > 0)) delete_selected();
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }

    void draw_picker() {
        if (!ImGui::BeginPopup("##plpick")) return;
        int ci = find_id(pick_id);
        if (ci < 0) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        Project& p = doc->prj;
        Component& c = p.comps[ci];
        ImGui::TextDisabled("%s", c.ref.c_str());
        ImGui::Separator();
        if (pick_col == PC_Side) {
            for (int s = 0; s < 2; ++s)
                if (ImGui::Selectable(s ? "Bottom" : "Top", (int)c.side == s) && (int)c.side != s) { doc->checkpoint("Change side"); c.side = (Side)s; doc->changed(ui::CH_GEOMETRY); }
        } else if (pick_col == PC_Point) {
            if (ImGui::Selectable("(unassigned)", c.assigned_point < 0) && c.assigned_point >= 0) { doc->checkpoint("Unassign"); c.assigned_point = -1; doc->changed(ui::CH_ASSIGN); }
            for (size_t i = 0; i < p.model.points.size(); ++i) {
                ImVec2 cp = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(cp.x + S(5), cp.y + ImGui::GetTextLineHeight() * 0.5f), S(4), argb_im(p.model.points[i].color | 0xFF000000u));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(14));
                if (ImGui::Selectable((p.model.points[i].name + "##p" + std::to_string(i)).c_str(), c.assigned_point == (int)i) && c.assigned_point != (int)i) {
                    doc->checkpoint("Assign"); c.assigned_point = (int)i; doc->changed(ui::CH_ASSIGN);
                }
            }
        } else if (pick_col == PC_Fid) {
            static const char* names[] = {"Not a fiducial", "Board fiducial", "Component fiducial", "Circuit fiducial"};
            int cur = c.feature != Feature::Fiducial ? 0 : 1 + (int)c.fid_scope;
            for (int i = 0; i < 4; ++i)
                if (ImGui::Selectable(names[i], cur == i) && cur != i) {
                    doc->checkpoint("Fiducial type");
                    if (i == 0) { c.feature = Feature::Component; c.fid_number = 0; }
                    else {
                        if (c.feature != Feature::Fiducial || c.fid_number <= 0) c.fid_number = next_fid_number(p);
                        c.feature = Feature::Fiducial;
                        c.fid_scope = (FiducialScope)(i - 1);
                    }
                    doc->changed(ui::CH_GEOMETRY);
                }
        }
        ImGui::EndPopup();
    }

    // ---- review panel ----------------------------------------------------
    void draw_review() {
        const GTheme& t = th();
        Project& p = doc->prj;
        ImGui::PushFont(g_font_bold);
        icon_image(ui::Icon::Report, ImGui::GetTextLineHeight());
        ImGui::SameLine(0, S(6));
        ImGui::TextUnformatted("Placement Review");
        ImGui::PopFont();
        const int ne = count_issues(issues, Severity::Error), nw = count_issues(issues, Severity::Warning), ni = count_issues(issues, Severity::Info);
        ImGui::TextColored(ImColor(ne ? t.error : t.text_faint), "%d error%s", ne, ne == 1 ? "" : "s");
        ImGui::SameLine(0, S(10));
        ImGui::TextColored(ImColor(nw ? t.warn : t.text_faint), "%d warning%s", nw, nw == 1 ? "" : "s");
        ImGui::SameLine(0, S(10));
        ImGui::TextColored(ImColor(t.text_faint), "%d info", ni);
        ImGui::Dummy(ImVec2(0, S(4)));
        section_header("TOLERANCES");
        const Units u = (Units)app::settings().units;
        const char* us = u == Units::MM ? "mm" : u == Units::Mil ? "mil" : "in";
        float near_v = (float)to_display(copt.near_tol, u);
        char fmt[32];
        std::snprintf(fmt, sizeof fmt, "%%.%df %s", u == Units::Inch ? 4 : u == Units::Mil ? 1 : 3, us);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##near", &near_v, 0.f, (float)to_display(2.0, u), fmt)) { copt.near_tol = std::max(0.0, from_display(near_v, u)); dirty_check = true; }
        tooltip("Near-coincident distance (same side)");
        if (ImGui::Checkbox("Overlapping bodies", &copt.check_overlap)) dirty_check = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("Include DNP", &copt.overlap_include_dnp)) dirty_check = true;
        ImGui::Dummy(ImVec2(0, S(4)));
        // auto fixes
        int ndup = 0, nmisspk = 0, nrot = 0, ndnpa = 0;
        for (auto& is : issues) {
            if (is.code == CheckCode::DuplicateRef) ndup++;
            if (is.code == CheckCode::MissingPackage) nmisspk++;
            if (is.code == CheckCode::OffAxisRotation) nrot++;
            if (is.code == CheckCode::DnpAssigned) ndnpa++;
        }
        section_header("AUTO-FIX");
        if (button("Auto-fix duplicates", ui::Icon::Select, ndup > 0, ImVec2(-1, 0), ndup > 0)) {
            doc->checkpoint("Fix duplicate references");
            int n = fix_duplicate_refs(p);
            doc->changed(ui::CH_GEOMETRY);
            toast(std::to_string(n) + " duplicate reference(s) renamed", ToastKind::Success, 5.f, "Undo", [] { post_command(app::ID_EDIT_UNDO); });
        }
        float hw = (ImGui::GetContentRegionAvail().x - S(4)) * 0.5f;
        if (button("Fill packages", ui::Icon::PackageEditor, false, ImVec2(hw, 0), nmisspk > 0)) {
            doc->checkpoint("Fill packages from BOM");
            int n = fill_packages_from_parts(p);
            doc->changed(ui::CH_GEOMETRY);
            toast(std::to_string(n) + " package(s) filled from part numbers", ToastKind::Success, 5.f, "Undo", [] { post_command(app::ID_EDIT_UNDO); });
        }
        tooltip("Fill missing packages from the part number's BOM package");
        ImGui::SameLine(0, S(4));
        if (button("Snap rotations", ui::Icon::Rot90, false, ImVec2(hw, 0), nrot > 0)) {
            doc->checkpoint("Snap rotations");
            int n = snap_rotations(p);
            doc->changed(ui::CH_GEOMETRY);
            toast(std::to_string(n) + " rotation(s) snapped to 90\xC2\xB0", ToastKind::Success, 5.f, "Undo", [] { post_command(app::ID_EDIT_UNDO); });
        }
        if (ndnpa && button("Unassign DNP parts", ui::Icon::Unassign, false, ImVec2(-1, 0))) {
            doc->checkpoint("Unassign DNP");
            int n = unassign_dnp(p);
            doc->changed(ui::CH_ASSIGN);
            toast(std::to_string(n) + " DNP part(s) unassigned", ToastKind::Success, 5.f, "Undo", [] { post_command(app::ID_EDIT_UNDO); });
        }
        ImGui::Dummy(ImVec2(0, S(4)));
        section_header("FINDINGS");
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_faint);
        ImGui::TextWrapped("Click selects the rows; double-click flies to them in the Plotter.");
        ImGui::PopStyleColor();
        ImGui::BeginChild("##issues", ImVec2(0, 0), ImGuiChildFlags_None);
        if (issues.empty()) {
            ImGui::Dummy(ImVec2(0, S(12)));
            icon_image(ui::Icon::Select, S(18));
            ImGui::SameLine();
            ImGui::TextColored(ImColor(t.ok), "No problems found.");
        }
        // group by code, errors first
        std::map<int, std::vector<int>> groups;
        for (size_t i = 0; i < issues.size(); ++i) groups[(int)issues[i].code].push_back((int)i);
        std::vector<std::pair<int, std::vector<int>>> order(groups.begin(), groups.end());
        std::stable_sort(order.begin(), order.end(), [&](auto& a, auto& b) { return issues[a.second[0]].severity > issues[b.second[0]].severity; });
        for (auto& g : order) {
            const PlacementIssue& first = issues[g.second[0]];
            ImU32 col = first.severity == Severity::Error ? t.error : first.severity == Severity::Warning ? t.warn : t.text_faint;
            ImGui::PushID(g.first);
            bool open = open_groups.count(g.first) > 0;
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            float hh = ImGui::GetFrameHeight();
            if (ImGui::InvisibleButton("##grp", ImVec2(w, hh))) { if (open) open_groups.erase(g.first); else open_groups.insert(g.first); open = !open; }
            bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, p0 + ImVec2(w, hh), hov ? t.raised : t.panel2, S(6));
            dl->AddRectFilled(p0, p0 + ImVec2(S(4), hh), col, S(6), ImDrawFlags_RoundCornersLeft);
            ImGui::RenderArrow(dl, ImVec2(p0.x + S(9), p0.y + (hh - ImGui::GetFontSize()) * 0.5f), t.text_dim, open ? ImGuiDir_Down : ImGuiDir_Right, 0.7f);
            dl->AddText(ImVec2(p0.x + S(24), p0.y + (hh - ImGui::GetFontSize()) * 0.5f), t.text, pretty_code((CheckCode)g.first).c_str());
            char cnt[16]; std::snprintf(cnt, sizeof cnt, "%d", (int)g.second.size());
            ImVec2 cs = ImGui::CalcTextSize(cnt);
            ImVec2 b0(p0.x + w - cs.x - S(16), p0.y + (hh - cs.y - S(2)) * 0.5f);
            dl->AddRectFilled(b0, b0 + ImVec2(cs.x + S(10), cs.y + S(2)), alpha(col, 0x40), S(8));
            dl->AddText(ImVec2(b0.x + S(5), b0.y + S(1)), col, cnt);
            if (hov) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Click to expand. Double-click selects every location in this group.");
                ImGui::EndTooltip();
                if (ImGui::IsMouseDoubleClicked(0)) {
                    std::vector<int> all;
                    for (int ii : g.second) all.insert(all.end(), issues[ii].comps.begin(), issues[ii].comps.end());
                    select_issue(all, false);
                }
            }
            if (open) {
                ImGui::Indent(S(10));
                int shown = 0;
                for (int ii : g.second) {
                    if (++shown > 500) { ImGui::TextDisabled("... %d more", (int)g.second.size() - 500); break; }
                    const PlacementIssue& is = issues[ii];
                    ImGui::PushID(ii);
                    if (ImGui::Selectable(is.message.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick))
                        select_issue(is.comps, ImGui::IsMouseDoubleClicked(0));
                    if (ImGui::IsItemHovered() && ImGui::CalcTextSize(is.message.c_str()).x > ImGui::GetItemRectSize().x) tooltip(is.message.c_str());
                    ImGui::PopID();
                }
                ImGui::Unindent(S(10));
            }
            ImGui::Dummy(ImVec2(0, S(2)));
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    // click: select + scroll to the rows; double-click: fly the Plotter to them
    void select_issue(const std::vector<int>& comps, bool fly_to) {
        if (comps.empty()) return;
        select_only(comps);
        if (fly_to) { focus_components(doc, comps, true); return; }
        // make sure the rows are visible
        bool vis = false;
        for (int r : rows) if (r == comps[0]) vis = true;
        if (!vis) { qf = QF_All; filter[0] = 0; rebuild_rows(); }
        scroll_to_comp(comps[0]);
    }
};

// ===========================================================================
// BOM
// ===========================================================================
static const char* kSupplyTip =
    "Supply angle (per part number): how the part is turned in its feeder compared with the package drawing,\n"
    "written to the machine's component data. The package is drawn as the part sits in cut tape (pin 1 left\n"
    "of the sprocket holes), so tape is 0\xC2\xB0. Sticks are usually 270\xC2\xB0 and trays 180\xC2\xB0 (MTC 0\xC2\xB0).\n"
    "Two parts can share a package and still differ here (e.g. 0603 capacitor on tape, 0603 tantalum).\n"
    "It never changes placement angles.";
enum BCol { BC_Flags, BC_Num, BC_Desc, BC_Pkg, BC_Qty, BC_Refs, BC_Proc, BC_Pol, BC_Cust, BC_Vend, BC_Manf, BC_Maker, BC_Mark, BC_Height, BC_Pop, BC_Packaging, BC_Supply, BC_Count };   // R18a: +Packaging, Supply angle
struct BomRow {
    std::string number;
    bool in_parts = true;
    int qty = 0;
    std::string refs, flags;
    std::string bom_pkg, board_pkg;      // BOM-supplied package of its refs / board package ("*" = several)
    std::vector<int> comps;
};

struct BomView : View {
    ViewKind kind() const override { return ViewKind::Bom; }
    const char* title() const override { return "BOM"; }
    ui::Icon icon() const override { return ui::Icon::Bom; }

    uint64_t gen_seen = 0, hash_seen = 0;
    bool dirty = true;
    std::vector<BomRow> rows_all;
    std::vector<int> rows;               // indices into rows_all (by-number) or comps (by-component)
    std::vector<int> comp_rows;
    int mode = 0;                        // 0 by part number, 1 by component
    int qf = 0;                          // 0 all 1 flagged 2 unused 3 not in BOM
    int qcount[4] = {};
    char filter[128] = "";
    int sort_col = BC_Num; bool sort_asc = true;
    std::string sel_number;
    std::string ed_number; int ed_col = -1; char ed_buf[256] = ""; bool ed_focus = false;
    bool open_proc = false; std::string proc_number;
    bool open_supply = false; std::string supply_number; float supply_other = 0;   // R18a

    const PartNumber* part(const std::string& n) const { auto it = doc->prj.parts.find(n); return it == doc->prj.parts.end() ? nullptr : &it->second; }

    // R15: polarity = the board package's polarity marker (any placement of the row); -1 no package
    int row_polarity(const BomRow& r) const {
        int res = -1;
        for (int ci : r.comps) {
            if (ci < 0 || ci >= (int)doc->prj.comps.size()) continue;
            if (const Package* pk = doc->prj.package_of(doc->prj.comps[ci])) { if (pkg_has_polarity(*pk)) return 1; res = 0; }
        }
        return res;
    }
    std::string cell(const BomRow& r, int col) const {
        const PartNumber* pn = part(r.number);
        switch (col) {
        case BC_Flags: return r.flags;
        case BC_Num: return r.number;
        case BC_Desc: return pn ? pn->description : "";
        case BC_Pkg: {   // what the BOM says, else what the board uses (the Package cell is never blank while a flag refers to it)
            std::string b = pn && !pn->package.empty() ? pn->package : r.bom_pkg;
            if (b == "*") b = "(several)";
            if (!b.empty()) return b;
            return r.board_pkg == "*" ? std::string("(several)") : r.board_pkg;
        }
        case BC_Qty: return std::to_string(r.qty);
        case BC_Refs: return r.refs;
        case BC_Proc: return pn ? pn->default_process : "";
        case BC_Pol: { const int m = row_polarity(r); return m > 0 ? "Yes" : (pn && pn->polarized ? "BOM only (no marker)" : ""); }
        case BC_Cust: return pn ? pn->cust_pn : "";
        case BC_Vend: return pn ? pn->vend_pn : "";
        case BC_Manf: return pn ? pn->manf_pn : "";
        case BC_Maker: return pn ? pn->manufacturer : "";
        case BC_Mark: return pn ? pn->part_marking : "";
        case BC_Height: { if (!pn || pn->height <= 0) return ""; char b[32]; std::snprintf(b, sizeof b, "%.2f", pn->height); return b; }
        case BC_Pop: return pn ? pn->pop_type : "";
        case BC_Packaging: return pn ? pn->packaging : "";
        case BC_Supply: {   // R18a
            if (!pn) return "0\xC2\xB0 (default)";
            char b[48]; std::snprintf(b, sizeof b, "%g\xC2\xB0%s", effective_supply_angle(*pn), pn->supply_angle_set ? "" : " (default)");
            return b;
        }
        }
        return {};
    }

    void rebuild() {
        const Project& p = doc->prj;
        std::map<std::string, std::vector<int>> used;
        for (size_t i = 0; i < p.comps.size(); ++i) {
            const Component& c = p.comps[i];
            if (c.feature == Feature::Component && !c.number.empty()) used[c.number].push_back((int)i);
        }
        rows_all.clear();
        auto add = [&](const std::string& num, bool in_parts) {
            BomRow r;
            r.number = num;
            r.in_parts = in_parts;
            auto it = used.find(num);
            const PartNumber* pn = part(num);
            bool mismatch = false, inferred = false, missing = false;
            auto merge = [](std::string& dst, const std::string& v) { if (v.empty()) return; if (dst.empty()) dst = v; else if (!packages_equivalent(dst, v) && dst != "*") dst = "*"; };
            if (it != used.end()) {
                r.comps = it->second;
                r.qty = 0;
                std::vector<std::string> refs;
                for (int ci : it->second) {
                    const Component& c = p.comps[ci];
                    if (!c.dnp) r.qty++;
                    refs.push_back(c.ref.empty() ? std::string("?") : c.ref);
                    // M only when the BOM supplied a package for this ref and it disagrees (not library / inferred values)
                    if (!c.bom_package.empty() && !c.package.empty() && !packages_equivalent(c.bom_package, c.package)) mismatch = true;
                    merge(r.bom_pkg, c.bom_package);
                    merge(r.board_pkg, c.package);
                    if (c.dnp) continue;
                    if (c.bom_package.empty() && !c.package.empty()) inferred = true;
                    if (c.bom_package.empty() && c.package.empty() && !(pn && !pn->package.empty())) missing = true;
                }
                r.refs = compress_refs(refs);
            }
            if (!in_parts) r.flags += '?';
            else if (r.comps.empty()) r.flags += 'U';
            if (mismatch) r.flags += 'M';
            else if (missing) r.flags += 'P';
            else if (inferred && r.bom_pkg.empty() && !(pn && !pn->package.empty())) r.flags += 'I';
            if (r.bom_pkg.empty() && pn && !pn->package.empty()) r.bom_pkg = pn->package;
            rows_all.push_back(std::move(r));
        };
        for (auto& kv : p.parts) add(kv.first, true);
        for (auto& kv : used) if (!p.parts.count(kv.first)) add(kv.first, false);
        dirty = false;
        filter_rows();
    }

    bool pass_q(const BomRow& r, int q) const {
        switch (q) {
        case 1: return !r.flags.empty();
        case 2: return r.flags.find('U') != std::string::npos;
        case 3: return r.flags.find('?') != std::string::npos;
        }
        return true;
    }

    void filter_rows() {
        const Project& p = doc->prj;
        const std::string flt = lower(trim(filter));
        rows.clear();
        for (auto& q : qcount) q = 0;
        for (size_t i = 0; i < rows_all.size(); ++i) {
            const BomRow& r = rows_all[i];
            for (int q = 0; q < 4; ++q) if (pass_q(r, q)) qcount[q]++;
            if (!pass_q(r, qf)) continue;
            if (!flt.empty()) {
                bool hit = false;
                for (int c = 0; c < BC_Count && !hit; ++c) if (lower(cell(r, c)).find(flt) != std::string::npos) hit = true;
                if (!hit) continue;
            }
            rows.push_back((int)i);
        }
        const int col = sort_col; const bool asc = sort_asc;
        auto less = [&](int a, int b) {
            const BomRow& A = rows_all[a]; const BomRow& B = rows_all[b];
            if (col == BC_Qty && A.qty != B.qty) return A.qty < B.qty;
            if (col != BC_Num && col != BC_Qty) { std::string x = cell(A, col), y = cell(B, col); if (x != y) return ref_less(x, y); }
            return ref_less(A.number, B.number);
        };
        std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) { return asc ? less(a, b) : less(b, a); });
        // by component
        comp_rows.clear();
        for (size_t i = 0; i < p.comps.size(); ++i) {
            const Component& c = p.comps[i];
            if (c.feature != Feature::Component) continue;
            if (!flt.empty()) {
                const PartNumber* pn = part(c.number);
                std::string hay = lower(c.ref + " " + c.number + " " + c.package + " " + (pn ? pn->description : ""));
                if (hay.find(flt) == std::string::npos) continue;
            }
            if (qf == 3 && (c.number.empty() || part(c.number))) continue;
            if (qf == 1 && !(c.number.empty() || !part(c.number))) continue;
            if (qf == 2) continue;
            comp_rows.push_back((int)i);
        }
        std::sort(comp_rows.begin(), comp_rows.end(), [&](int a, int b) {
            const Component& A = p.comps[a]; const Component& B = p.comps[b];
            if (sort_col == BC_Num && A.number != B.number) return asc ? ref_less(A.number, B.number) : ref_less(B.number, A.number);
            return asc ? ref_less(A.ref, B.ref) : ref_less(B.ref, A.ref);
        });
    }

    void sync() {
        if (doc->gen != gen_seen) {
            gen_seen = doc->gen;
            uint64_t h = content_hash(doc->prj);
            if (h != hash_seen) { hash_seen = h; dirty = true; }
        }
        if (dirty) rebuild();
    }

    std::string status() const override {
        int on_board = 0;
        for (auto& r : rows_all) on_board += r.qty;
        return std::to_string(rows_all.size()) + " part numbers  \xC2\xB7  " + std::to_string(on_board) + " placements";
    }

    void draw_toolbar() override {
        if (icon_button("bom_imp", ui::Icon::ImportBom, "Import BOM...")) post_command(app::ID_BOM_IMPORT);
        ImGui::SameLine(0, S(2));
        {
            bool any_bom_pkg = false;
            for (auto& c : doc->prj.comps) if (!c.bom_package.empty() && c.bom_state == BomState::Listed) { any_bom_pkg = true; break; }
            if (icon_button("bom_commit_pkg", ui::Icon::PackageEditor,
                            "Commit BOM packages to library\nWrite the package each BOM line gave into its part number's library entry.\n"
                            "Part numbers whose references list different packages are left unchanged.", false, any_bom_pkg)) {
                Project work = doc->prj;
                int n = io::bom_commit_packages(work);
                if (n) { doc->checkpoint("Commit BOM packages to library"); doc->prj = std::move(work); doc->changed(ui::CH_GEOMETRY); dirty = true; }
                toast(n ? std::to_string(n) + " part number package(s) set from the BOM" : std::string("Part library already matches the BOM packages"),
                      n ? ToastKind::Success : ToastKind::Info);
            }
        }
        toolbar_separator();
        if (icon_button("bom_add", ui::Icon::Plus, "Add part number")) add_part();
        ImGui::SameLine(0, S(2));
        if (icon_button("bom_del", ui::Icon::Delete, "Delete part number  (Del)", false, !sel_number.empty() && part(sel_number))) delete_part(sel_number);
        toolbar_separator();
        if (icon_button("bom_copy", ui::Icon::Copy, "Copy BOM rows  (Ctrl+C)", false, !rows.empty())) on_command(app::ID_EDIT_COPY);
        ImGui::SameLine(0, S(2));
        if (icon_button("bom_csv", ui::Icon::Export, "Export BOM as CSV...", false, !rows_all.empty())) export_csv();
    }

    bool on_command(int id) override {
        if (id == app::ID_EDIT_DELETE && !sel_number.empty()) { delete_part(sel_number); return true; }
        if (id == app::ID_EDIT_COPY) {
            std::string s = "Part Number\tDescription\tPackage\tQty\tReferences\tProcess\tPolarized\tCust PN\tVendor PN\tManf PN\r\n";
            for (int i : rows) { const BomRow& r = rows_all[i]; for (int c = BC_Num; c < BC_Count; ++c) { if (c > BC_Num) s += '\t'; s += cell(r, c); } s += "\r\n"; }
            copy_text(s);
            toast("BOM copied to the clipboard (" + std::to_string(rows.size()) + " rows)", ToastKind::Success);
            return true;
        }
        return false;
    }
    int cmd_state(int id) override {
        if (id == app::ID_EDIT_COPY) return 1;
        if (id == app::ID_EDIT_DELETE) return part(sel_number) ? 1 : 0;
        return -1;
    }

    void add_part() {
        auto d = doc;
        defer([d] {
            std::wstring n;
            ui::Form f(L"Add Part Number", 380);
            f.text("n", L"Part number", L"");
            f.text("d", L"Description", L"");
            if (!f.run(main_hwnd())) return;
            std::string num = trim(f.u8("n"));
            if (num.empty()) return;
            if (d->prj.parts.count(num)) { ui::message(main_hwnd(), L"Add Part Number", L"That part number already exists."); return; }
            d->checkpoint("Add part number");
            PartNumber pn; pn.number = num; pn.description = trim(f.u8("d"));
            d->prj.parts[num] = pn;
            d->changed(ui::CH_INFO);
        });
    }

    void delete_part(const std::string& num) {
        if (!part(num)) return;
        int used = 0;
        for (auto& c : doc->prj.comps) if (c.number == num) used++;
        auto d = doc;
        defer([d, num, used] {
            if (used && !ui::message(main_hwnd(), L"Delete Part Number", L"'" + ui::W(num) + L"' is used by " + std::to_wstring(used) +
                                     L" location(s). Delete it from the BOM anyway?\n(The locations keep their part number.)", true)) return;
            d->checkpoint("Delete part number");
            d->prj.parts.erase(num);
            d->changed(ui::CH_INFO);
        });
    }

    void export_csv() {
        std::string s = "\xEF\xBB\xBF" "Part Number,Description,Package,Qty,References,Process,Polarized,Cust PN,Vendor PN,MPN,Manufacturer,Marking,Height mm,Pop. type,Flags\r\n";
        for (int i : rows) {
            const BomRow& r = rows_all[i];
            for (int c = BC_Num; c < BC_Count; ++c) { if (c > BC_Num) s += ','; s += csv_field(cell(r, c)); }
            s += "," + r.flags + "\r\n";
        }
        save_text_file(s, L"Export BOM", L"CSV files", L"*.csv", L"csv", L"bom.csv");
    }

    void begin_edit(const BomRow& r, int col) {
        if (col == BC_Pol) { gui::status("Polarity is the package's polarity marker: edit it in Shapes (P adds / removes it)."); return; }
        if (col == BC_Proc) { proc_number = r.number; open_proc = true; return; }
        if (col == BC_Packaging || col == BC_Supply) { supply_number = r.number; open_supply = true; return; }   // R18a
        if (col == BC_Qty || col == BC_Refs || col == BC_Flags) return;
        ed_number = r.number; ed_col = col; ed_focus = true;
        std::snprintf(ed_buf, sizeof ed_buf, "%s", cell(r, col).c_str());
    }

    PartNumber& ensure(const std::string& num) {
        PartNumber& pn = doc->prj.parts[num];
        if (pn.number.empty()) {
            pn.number = num;
            for (auto& c : doc->prj.comps) if (c.number == num && !c.package.empty()) { pn.package = c.package; break; }
        }
        return pn;
    }

    void commit_edit() {
        const std::string num = ed_number;
        const int col = ed_col;
        ed_col = -1; ed_number.clear();
        const std::string t = trim(ed_buf);
        Project& p = doc->prj;
        if (col == BC_Num) {
            if (t == num || t.empty()) return;
            if (p.parts.count(t)) { toast("Part number " + t + " already exists", ToastKind::Error); flash(ImHashStr(num.c_str()) ^ 0xE77u); return; }
            doc->checkpoint("Rename part number");
            auto it = p.parts.find(num);
            if (it != p.parts.end()) { PartNumber pn = it->second; pn.number = t; p.parts.erase(it); p.parts[t] = pn; }
            for (auto& c : p.comps) if (c.number == num) c.number = t;
            if (sel_number == num) sel_number = t;
            flash(ImHashStr(t.c_str()));
            doc->changed(ui::CH_GEOMETRY);
            toast("Renamed " + num + " \xE2\x86\x92 " + t, ToastKind::Success, 3.f, "Undo", [] { post_command(app::ID_EDIT_UNDO); });
            return;
        }
        if (cell_by_num(num, col) == t) return;
        doc->checkpoint("Edit BOM");
        PartNumber& pn = ensure(num);
        switch (col) {
        case BC_Desc: pn.description = t; break;
        case BC_Pkg: {
            std::string old = pn.package;
            pn.package = t;
            // components that followed the BOM package follow the change
            for (auto& c : p.comps) if (c.number == num && (c.package.empty() || c.package == old)) { c.package = t; c.pkg_src = DataSrc::Library; }
            break;
        }
        case BC_Cust: pn.cust_pn = t; break;
        case BC_Vend: pn.vend_pn = t; break;
        case BC_Manf: pn.manf_pn = t; break;
        case BC_Maker: pn.manufacturer = t; break;
        case BC_Mark: pn.part_marking = t; break;
        case BC_Height: pn.height = std::max(0.0, std::atof(t.c_str())); break;
        case BC_Pop: pn.pop_type = t; break;
        default: break;
        }
        flash(ImHashStr(num.c_str()));
        doc->changed(ui::CH_GEOMETRY);
    }
    std::string cell_by_num(const std::string& num, int col) const {
        for (auto& r : rows_all) if (r.number == num) return cell(r, col);
        return {};
    }

    void draw(ImVec2 size) override {
        sync();
        const GTheme& t = th();
        ImGui::Dummy(ImVec2(0, S(2)));
        // mode segmented control
        {
            const char* names[2] = {"By Part Number", "By Component"};
            for (int i = 0; i < 2; ++i) {
                if (i) ImGui::SameLine(0, S(4));
                if (chip(names[i], mode == i, -1, i ? ui::Icon::Locations : ui::Icon::Bom)) mode = i;
            }
        }
        ImGui::SameLine(0, S(14));
        if (search_box("##bf", filter, sizeof filter, "Filter part number, description, package...", S(280))) filter_rows();
        static const char* qn[4] = {"All", "Flagged", "Unused (U)", "Not in BOM (?)"};
        static const ui::Icon qi[4] = {ui::Icon::SelectAll, ui::Icon::Warning, ui::Icon::NoRef, ui::Icon::Question};
        static const char* qt[4] = {"Every part number", "Any flag: unused, not in BOM or package mismatch",
                                    "In the BOM but not placed on the board", "Placed on the board but missing from the BOM"};
        for (int q = 0; q < 4; ++q) {
            ImGui::SameLine(0, q == 0 ? S(12) : S(4));
            if (chip(qn[q], qf == q, qcount[q], qi[q], qt[q])) { qf = q; filter_rows(); }
        }
        ImGui::SameLine(0, S(12));
        if (button("Import BOM...", ui::Icon::ImportBom, true)) post_command(app::ID_BOM_IMPORT);
        if (ImGui::IsItemHovered()) tooltip("Import a bill of materials (Excel .xlsx / .xls, CSV or text) and link it to the placements");
        {   // R12 §3: attachment state is always visible
            const Project::SourceRef* bs = nullptr;
            for (auto& x : doc->prj.sources) if (x.importer == "bom") bs = &x;
            int linked = 0, nc = 0;
            for (auto& c : doc->prj.comps) if (c.feature == Feature::Component) { ++nc; linked += !c.number.empty(); }
            std::string line;
            if (doc->prj.parts.empty()) line = "Project BOM: empty \xE2\x80\x94 nothing is attached to this project yet.";
            else {
                line = "Project BOM: " + std::to_string(doc->prj.parts.size()) + " part numbers, " + std::to_string(linked) + " of " + std::to_string(nc) + " components linked";
                if (bs) {
                    std::string fn = bs->path; if (size_t k = fn.find_last_of("\\/"); k != std::string::npos) fn = fn.substr(k + 1);
                    auto sh = bs->settings.find("sheet");
                    line += "   \xE2\x80\x94 from " + fn + (sh != bs->settings.end() && !sh->second.empty() ? " [" + sh->second + "]" : "") + ", " + bs->imported_at;
                } else line += "   \xE2\x80\x94 from CAD / centroid data";
            }
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(doc->prj.parts.empty() ? t.text_faint : t.ok), "%s", line.c_str());
        }
        ImGui::Dummy(ImVec2(0, S(2)));
        if (rows_all.empty()) {
            if (empty_state(ui::Icon::Bom, "No BOM loaded", "Import a bill of materials, or import CAD / centroid data with part numbers.",
                            "Import BOM...", ui::Icon::ImportBom))
                post_command(app::ID_BOM_IMPORT);
            return;
        }
        if (rows.empty()) {
            if (empty_state(ui::Icon::FindRef, "No part numbers match", "Nothing matches the filter text / chip.", "Clear filter", ui::Icon::Eraser)) {
                filter[0] = 0; qf = 0; filter_rows();
            }
            return;
        }
        const float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2 - S(4);   // legend may wrap to 2 lines
        if (mode == 0) draw_numbers(h); else draw_components(h);
        if (mode == 0) flag_legend(kBomFlags, (int)(sizeof kBomFlags / sizeof kBomFlags[0]), "Double-click a cell to edit; Qty excludes DNP.");
        else {
            ImGui::PushStyleColor(ImGuiCol_Text, t.text_faint);
            ImGui::TextUnformatted("Every placed component with its BOM data. Double-click a row to jump to its part number.");
            ImGui::PopStyleColor();
        }
        if (open_supply) { ImGui::OpenPopup("##supply"); open_supply = false; const PartNumber* q = part(supply_number); supply_other = q ? (float)effective_supply_angle(*q) : 0.f; }
        if (ImGui::BeginPopup("##supply")) { draw_supply_popup(); ImGui::EndPopup(); }   // R18a
        if (open_proc) { ImGui::OpenPopup("##proc"); open_proc = false; }
        if (ImGui::BeginPopup("##proc")) {
            Project& p = doc->prj;
            const PartNumber* pn = part(proc_number);
            std::string cur = pn ? pn->default_process : "";
            auto setp = [&](const std::string& v) { if (v == cur) return; doc->checkpoint("BOM process"); ensure(proc_number).default_process = v; doc->changed(ui::CH_INFO); };
            if (ImGui::Selectable("(none)", cur.empty())) setp("");
            for (size_t i = 0; i < p.model.points.size(); ++i) {
                ImVec2 cp = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(cp.x + S(5), cp.y + ImGui::GetTextLineHeight() * 0.5f), S(4), argb_im(p.model.points[i].color | 0xFF000000u));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(14));
                const std::string& nm = p.model.points[i].name;
                if (ImGui::Selectable((nm + "##pp" + std::to_string(i)).c_str(), cur == nm)) setp(nm);
            }
            ImGui::EndPopup();
        }
    }

    // R18a: packaging + supply angle of one part number (specs/ANGLES.md §10). Never changes placement angles.
    void draw_supply_popup() {
        const PartNumber* pn = part(supply_number);
        const std::string pk = pn ? pn->packaging : "";
        const bool set = pn && pn->supply_angle_set;
        const double cur = pn ? effective_supply_angle(*pn) : 0;
        ImGui::PushFont(g_font_bold); ImGui::TextUnformatted(supply_number.c_str()); ImGui::PopFont();
        ImGui::TextDisabled("Packaging");
        static const char* kp[] = {"Tape", "Stick", "Tray", "MTC", "Bulk"};
        auto setpk = [&](const std::string& v) {
            if (v == pk) return;
            doc->checkpoint("Packaging " + supply_number);
            ensure(supply_number).packaging = v;   // an unset supply angle follows the new packaging default
            doc->changed(ui::CH_INFO);
        };
        if (ImGui::RadioButton("Unknown", pk.empty())) setpk("");
        for (int i = 0; i < 5; ++i) { ImGui::SameLine(); if (ImGui::RadioButton(kp[i], pk == kp[i])) setpk(kp[i]); }
        tooltip("How the part is supplied. MTC = matrix tray changer.\nSets the default Supply angle: Tape 0\xC2\xB0, Stick 270\xC2\xB0, Tray 180\xC2\xB0, MTC 0\xC2\xB0.");
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::TextDisabled("Supply angle");
        auto seta = [&](double v, bool user) {
            doc->checkpoint("Supply angle " + supply_number);
            PartNumber& q = ensure(supply_number);
            q.supply_angle = user ? v : 0; q.supply_angle_set = user;
            doc->changed(ui::CH_INFO);
        };
        const double angs[] = {0, 90, 180, 270};
        bool std_ang = false;
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            char l[16]; std::snprintf(l, sizeof l, "%g\xC2\xB0", angs[i]);
            const bool on = std::fabs(cur - angs[i]) < 1e-6;
            std_ang |= on;
            if (ImGui::RadioButton(l, on) && !(on && set)) seta(angs[i], true);
        }
        ImGui::SameLine();
        ImGui::RadioButton("Other", !std_ang);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(70));
        if (ImGui::InputFloat("##sao", &supply_other, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) seta(supply_other, true);
        tooltip(kSupplyTip);
        char db[96]; std::snprintf(db, sizeof db, "Use packaging default (%g\xC2\xB0)", supply_angle_default(pk));
        ImGui::BeginDisabled(!set);
        if (ImGui::Button(db)) seta(0, false);
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, th().text_faint);
        ImGui::TextUnformatted(set ? "Set for this part number." : "Following the packaging default.");
        ImGui::PopStyleColor();
    }

    void select_number(const std::string& num, bool fly) {
        sel_number = num;
        std::vector<int> idx;
        for (auto& r : rows_all) if (r.number == num) idx = r.comps;
        for (auto& c : doc->prj.comps) c.selected = false;
        for (int i : idx) doc->prj.comps[i].selected = true;
        doc->changed(ui::CH_SELECTION);
        if (fly && !idx.empty()) { focus_components(doc, idx, true); activate_view(doc, ViewKind::Plotter); }
    }

    void draw_numbers(float h) {
        const GTheme& t = th();
        ImGuiTableFlags tf = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                             ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable |
                             ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("##bom", BC_Count, tf, ImVec2(0, h))) return;
        ImGui::TableSetupScrollFreeze(2, 1);
        ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, S(48));
        ImGui::TableSetupColumn("Part Number", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_NoHide, S(140));
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthFixed, S(230));
        ImGui::TableSetupColumn("Package", ImGuiTableColumnFlags_WidthFixed, S(110));
        ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthFixed, S(40));
        ImGui::TableSetupColumn("References", ImGuiTableColumnFlags_WidthFixed, S(240));
        ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthFixed, S(110));
        ImGui::TableSetupColumn("Polarity", ImGuiTableColumnFlags_WidthFixed, S(64));
        ImGui::TableSetupColumn("Cust PN", ImGuiTableColumnFlags_WidthFixed, S(100));
        ImGui::TableSetupColumn("Vendor PN", ImGuiTableColumnFlags_WidthFixed, S(100));
        ImGui::TableSetupColumn("MPN", ImGuiTableColumnFlags_WidthFixed, S(150));
        // 'more' columns: hidden by default, right-click the header to show
        ImGui::TableSetupColumn("Manufacturer", ImGuiTableColumnFlags_WidthFixed, S(120));
        ImGui::TableSetupColumn("Marking", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, S(80));
        ImGui::TableSetupColumn("Height mm", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, S(70));
        ImGui::TableSetupColumn("Pop. type", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, S(80));
        ImGui::TableSetupColumn("Packaging", ImGuiTableColumnFlags_WidthFixed, S(80));        // R18a
        ImGui::TableSetupColumn("Supply angle", ImGuiTableColumnFlags_WidthFixed, S(110));  // R18a
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs())
            if (ss->SpecsDirty && ss->SpecsCount > 0) {
                sort_col = ss->Specs[0].ColumnIndex;
                sort_asc = ss->Specs[0].SortDirection != ImGuiSortDirection_Descending;
                ss->SpecsDirty = false;
                filter_rows();
            }
        const float row_h = std::round(ImGui::GetTextLineHeight() + S(9));
        const float ty = (row_h - ImGui::GetTextLineHeight()) * 0.5f;
        bool handled = false;
        ImGuiListClipper clip;
        clip.Begin((int)rows.size(), row_h);
        while (clip.Step())
            for (int ri = clip.DisplayStart; ri < clip.DisplayEnd; ++ri) {
                const BomRow& r = rows_all[rows[ri]];
                ImGui::TableNextRow(0, row_h);
                ImGui::PushID(ri);
                ImGui::TableSetColumnIndex(0);
                ImGui::PushStyleColor(ImGuiCol_Header, alpha(t.accent, 0x50));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0);
                ImGui::Selectable("##r", r.number == sel_number, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                                  ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, row_h));
                ImGui::PopStyleColor(2);
                {
                    const ImGuiID rk = ImHashStr(r.number.c_str());
                    float hv = anim(ImGui::GetID("hv"), ImGui::IsItemHovered() ? 1.f : 0.f, 18.f);
                    float fl = flash_amount(rk), fe = flash_amount(rk ^ 0xE77u);
                    ImU32 bg = hv > 0.01f ? col_alpha(t.accent, 0.12f * hv) : 0;
                    if (fl > 0.01f) bg = col_alpha(t.ok, 0.35f * fl);
                    if (fe > 0.01f) bg = col_alpha(t.error, 0.40f * fe);
                    if (bg) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, bg);
                }
                if (ImGui::IsItemHovered() && !handled) {
                    int hc = ImGui::TableGetHoveredColumn();
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        handled = true;
                        if (hc == BC_Refs || hc == BC_Qty) select_number(r.number, true);
                        else begin_edit(r, hc);
                    } else if (ImGui::IsMouseClicked(0)) { handled = true; if (!ed_number.empty() && ed_number != r.number) commit_edit(); select_number(r.number, false); }
                }
                ImGui::SameLine(0, 0);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
                flag_badges(r.flags);
                if (ImGui::IsItemHovered() && !r.flags.empty()) {
                    std::string tip;
                    for (char fc : r.flags) {
                        if (!tip.empty()) tip += "\n";
                        bool found = false;
                        for (auto& e : kBomFlags) if (e.ch == fc) { tip += std::string(1, fc) + " = " + e.label + " [" + e.sev + "]: " + e.detail; found = true; }
                        if (!found) tip += std::string(1, fc);
                    }
                    tooltip(tip.c_str());
                }
                const PartNumber* pn = part(r.number);
                for (int col = BC_Num; col < BC_Count; ++col) {
                    if (!ImGui::TableSetColumnIndex(col)) continue;
                    if (ed_col == col && ed_number == r.number) {
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (ed_focus) { ImGui::SetKeyboardFocusHere(); ed_focus = false; }
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(4), std::max(0.f, (row_h - ImGui::GetFontSize()) * 0.5f - S(1))));
                        ImGui::InputText("##ed", ed_buf, sizeof ed_buf, ImGuiInputTextFlags_AutoSelectAll);
                        ImGui::PopStyleVar();
                        if (ImGui::IsItemDeactivated()) { if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { ed_col = -1; ed_number.clear(); } else commit_edit(); }
                        continue;
                    }
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
                    switch (col) {
                    case BC_Num: ImGui::PushFont(g_font_bold); ImGui::TextUnformatted(r.number.c_str()); ImGui::PopFont(); break;
                    case BC_Qty: {
                        std::string s = std::to_string(r.qty);
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetColumnWidth() - ImGui::CalcTextSize(s.c_str()).x - S(4)));
                        ImGui::TextColored(ImColor(r.qty ? t.text : t.text_faint), "%s", s.c_str());
                        break;
                    }
                    case BC_Pkg:
                        if (r.flags.find('M') != std::string::npos) {
                            std::string s2 = cell(r, col) + "  \xE2\x89\xA0  " + (r.board_pkg == "*" ? std::string("(several)") : r.board_pkg);
                            ImGui::TextColored(ImColor(t.warn), "%s", s2.c_str());
                            tooltip(("BOM package: " + cell(r, col) + "\nBoard package: " + (r.board_pkg == "*" ? std::string("several") : r.board_pkg) +
                                     "\nThe BOM supplied a package and it disagrees with the board (names are compared after\nnormalisation: 0402 = C0402 = CAP_0402 = CAPC1005X55N). Fix the board package or the BOM.").c_str());
                        }
                        else if (r.flags.find('I') != std::string::npos) { ImGui::TextColored(ImColor(t.text_dim), "%s", cell(r, col).c_str()); tooltip("Inferred from the board: the BOM gave no package for these references."); }
                        else if (pn && !pn->package.empty() && !doc->prj.packages.count(pn->package)) ImGui::TextColored(ImColor(t.warn), "%s", pn->package.c_str());
                        else ImGui::TextUnformatted(cell(r, col).c_str());
                        break;
                    case BC_Proc: {
                        int pt = pn ? doc->prj.point_index(pn->default_process) : -1;
                        if (pt >= 0) point_chip(doc->prj, pt);
                        else ImGui::TextUnformatted(cell(r, col).c_str());
                        break;
                    }
                    case BC_Pol: {
                        // R15: read-only view of the package polarity marker; BOM says polarized but the package has
                        // no marker -> warning (validation), never auto-fixed
                        const int m = row_polarity(r);
                        const bool v = m > 0, bom_only = !v && pn && pn->polarized;
                        ImVec2 c0 = ImGui::GetCursorScreenPos();
                        float s = ImGui::GetTextLineHeight();
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        if (v) dl->AddCircleFilled(c0 + ImVec2(s * 0.5f, s * 0.5f), s * 0.3f, IM_COL32(224, 49, 49, 255), 16);
                        else if (bom_only) dl->AddText(c0, t.warn, "!");
                        ImGui::Dummy(ImVec2(s, s));
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", v ? "Package has a polarity marker" : bom_only ? "BOM marks this part polarized, but its package has no polarity marker.\nAdd one in Shapes (P)." : "No polarity marker");
                        break;
                    }
                    case BC_Packaging: case BC_Supply: {   // R18a
                        const bool def = !pn || !pn->supply_angle_set;
                        ImGui::TextColored(ImColor(col == BC_Supply && def ? t.text_dim : t.text), "%s", cell(r, col).c_str());
                        if (ImGui::IsItemHovered()) tooltip(kSupplyTip);
                        break;
                    }
                    case BC_Refs: case BC_Desc: ImGui::TextColored(ImColor(col == BC_Refs ? t.text : t.text_dim), "%s", cell(r, col).c_str()); break;
                    default: ImGui::TextUnformatted(cell(r, col).c_str());
                    }
                }
                ImGui::PopID();
            }
        ImGui::EndTable();
    }

    void draw_components(float h) {
        Project& p = doc->prj;
        const GTheme& t = th();
        ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("##bomc", 7, tf, ImVec2(0, h))) return;
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Reference", ImGuiTableColumnFlags_DefaultSort, S(80));
        ImGui::TableSetupColumn("Part Number", 0, S(140));
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_NoSort, S(240));
        ImGui::TableSetupColumn("Package", ImGuiTableColumnFlags_NoSort, S(110));
        ImGui::TableSetupColumn("BOM Package", ImGuiTableColumnFlags_NoSort, S(110));
        ImGui::TableSetupColumn("Model Point", ImGuiTableColumnFlags_NoSort, S(130));
        ImGui::TableSetupColumn("DNP", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthStretch, S(40));
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs())
            if (ss->SpecsDirty && ss->SpecsCount > 0) {
                sort_col = ss->Specs[0].ColumnIndex == 1 ? BC_Num : BC_Flags;
                sort_asc = ss->Specs[0].SortDirection != ImGuiSortDirection_Descending;
                ss->SpecsDirty = false;
                filter_rows();
            }
        const float row_h = std::round(ImGui::GetTextLineHeight() + S(9));
        const float ty = (row_h - ImGui::GetTextLineHeight()) * 0.5f;
        ImGuiListClipper clip;
        clip.Begin((int)comp_rows.size(), row_h);
        while (clip.Step())
            for (int ri = clip.DisplayStart; ri < clip.DisplayEnd; ++ri) {
                Component& c = p.comps[comp_rows[ri]];
                const PartNumber* pn = part(c.number);
                ImGui::TableNextRow(0, row_h);
                ImGui::PushID(ri);
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable("##r", c.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, row_h))) {
                    if (ImGui::IsMouseDoubleClicked(0)) { mode = 0; select_number(c.number, false); }
                    else {
                        if (!ImGui::GetIO().KeyCtrl) for (auto& o : p.comps) o.selected = false;
                        c.selected = !c.selected || !ImGui::GetIO().KeyCtrl;
                        doc->changed(ui::CH_SELECTION);
                    }
                }
                ImGui::SameLine(0, 0);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
                ImGui::PushFont(g_font_bold); ImGui::TextUnformatted(c.ref.c_str()); ImGui::PopFont();
                auto txt = [&](int col, const std::string& s, ImU32 color) {
                    ImGui::TableSetColumnIndex(col);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
                    ImGui::TextColored(ImColor(color), "%s", s.c_str());
                };
                txt(1, c.number.empty() ? "(no number)" : c.number, c.number.empty() ? t.error : pn ? t.text : t.warn);
                txt(2, pn ? pn->description : "", t.text_dim);
                txt(3, c.package, t.text);
                bool mism = pn && !pn->package.empty() && !c.package.empty() && upper(pn->package) != upper(c.package);
                txt(4, pn ? pn->package : "", mism ? t.error : t.text_dim);
                ImGui::TableSetColumnIndex(5);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
                point_chip(p, c.assigned_point);
                txt(6, c.dnp ? "DNP" : "", t.warn);
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
};

// ===========================================================================
// REPORTS
// ===========================================================================
enum RowKind { R_NORMAL = 0, R_SECTION = 1, R_TOTAL = 2, R_ERROR = 3, R_WARN = 4 };
struct RRow { std::vector<std::string> cells; int kind = R_NORMAL; std::vector<int> comps; };
struct Report {
    std::string title;
    std::vector<std::string> header, cols;
    std::vector<int> align;   // 0 left 2 right
    std::vector<RRow> rows;
    std::string footer;
};
struct ROpts { int point = -1; int consolidate = 1; bool per_image = false; bool include_dnp = false; };

enum RKind { RK_Prep, RK_Opp, RK_Routing, RK_Place, RK_Review, RK_Count, RK_Fid = 100 };   // RK_Fid: Data > Fiducials only
const char* kRNames[RK_Count] = {"Component Prep", "Opportunity Count", "Routing", "Placement List", "Placement Review"};
const char* kRDesc[RK_Count] = {"Leads, span and pitch", "Joints and polarity", "Parts per model point", "Ref, XY, rotation, side", "Duplicates, overlaps, data"};
const ui::Icon kRIcons[RK_Count] = {ui::Icon::PackageEditor, ui::Icon::Grid, ui::Icon::FactoryModel, ui::Icon::Locations, ui::Icon::Report};

struct PkgMetrics { int leads = 0; double span = 0, pitch = 0; bool has_span = false, has_pitch = false; };
PkgMetrics package_metrics(const Package* pk) {
    PkgMetrics m;
    if (!pk) return m;
    m.leads = pk->leads > 0 ? pk->leads : (int)pk->pads.size();
    const auto& pads = pk->pads;
    if (pads.size() >= 2) {
        BBox cb;
        for (auto& p : pads) cb.add(p.pos);
        const bool along_x = cb.w() >= cb.h();
        double lo = 1e30, hi = -1e30;
        for (auto& p : pads) {
            const double r = deg2rad(p.rot), c = std::fabs(std::cos(r)), s = std::fabs(std::sin(r));
            const double half = along_x ? (p.w / 2 * c + p.h / 2 * s) : (p.w / 2 * s + p.h / 2 * c);
            const double v = along_x ? p.pos.x : p.pos.y;
            lo = std::min(lo, v - half);
            hi = std::max(hi, v + half);
        }
        m.span = hi - lo;
        m.has_span = m.span > 0;
    }
    if (pads.size() >= 3) {
        std::map<long long, int> hist;
        for (size_t i = 0; i < pads.size(); ++i) {
            double best = 1e30;
            for (size_t j = 0; j < pads.size(); ++j) {
                if (i == j) continue;
                double dd = dist(pads[i].pos, pads[j].pos);
                if (dd > 0.01 && dd < best) best = dd;
            }
            if (best < 1e29) hist[std::llround(best * 1000)]++;
        }
        long long key = -1; int cnt = 0;
        for (auto& kv : hist) if (kv.second > cnt) { cnt = kv.second; key = kv.first; }
        if (key > 0) { m.pitch = key / 1000.0; m.has_pitch = true; }
    }
    return m;
}

std::string join_refs(std::vector<std::string> refs) {
    std::sort(refs.begin(), refs.end(), ref_less);
    std::string s;
    for (size_t i = 0; i < refs.size(); ++i) { if (i) s += ", "; s += refs[i]; }
    return s;
}

std::string now_text() {
    SYSTEMTIME st; GetLocalTime(&st);
    char b[48]; std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return b;
}

void build_report(const Doc& d, int which, const ROpts& o, Report& r) {
    const Project& p = d.prj;
    r = Report{};
    r.title = which == RK_Fid ? std::string("Fiducials & Marks") : std::string(kRNames[which]) + (which == RK_Review ? "" : " Report");
    const AssemblyInfo& a = p.info;
    if (!app::settings().company.empty()) r.header.push_back("Company: " + ui::U(app::settings().company));
    r.header.push_back("Assembly: " + (a.name.empty() ? ui::U(d.display_name()) : a.name));
    if (!a.pcb_id.empty()) r.header.push_back("Board ID: " + a.pcb_id);   // v17: visible Project Info fields only
    if (!a.bom_rev.empty()) r.header.push_back("BOM revision: " + a.bom_rev);
    if (!a.customer.empty()) r.header.push_back("Customer: " + a.customer);
    r.header.push_back("Generated: " + now_text());
    const int np = (int)p.model.points.size();
    auto in_scope = [&](int pt) { return o.point < 0 || pt == o.point; };
    auto pname = [&](int pt) { return pt >= 0 && pt < np ? p.model.points[pt].name : std::string("(unassigned)"); };
    if (which != RK_Routing && which != RK_Review && which != RK_Fid) r.header.push_back("Scope: " + (o.point < 0 ? std::string("All locations") : "Model point " + pname(o.point)));
    const std::string u = " (" + ui::U(app::units_label()) + ")";
    auto len = [&](bool has, double v) { return has ? fmt_len_u8(v, false) : std::string("-"); };
    std::vector<const Component*> comps;
    for (auto& c : p.comps)
        if (c.feature == Feature::Component && in_scope(c.assigned_point) && (o.include_dnp || !c.dnp)) comps.push_back(&c);

    if (which == RK_Prep) {
        static const char* cons[] = {"Location", "Number", "Package"};
        r.header.push_back(std::string("Consolidation: ") + cons[std::clamp(o.consolidate, 0, 2)]);
        if (o.consolidate == 0) {
            r.cols = {"Reference", "Part Number", "Package", "Leads", "Lead Span" + u, "Pitch" + u, "Model Point"};
            r.align = {0, 0, 0, 2, 2, 2, 0};
            std::sort(comps.begin(), comps.end(), [](const Component* x, const Component* y) { return ref_less(x->ref, y->ref); });
            for (auto* c : comps) {
                PkgMetrics m = package_metrics(p.package_of(*c));
                r.rows.push_back({{c->ref, c->number, c->package, std::to_string(m.leads), len(m.has_span, m.span), len(m.has_pitch, m.pitch), pname(c->assigned_point)}});
                r.rows.back().comps = {(int)(c - p.comps.data())};
            }
            r.footer = std::to_string(comps.size()) + " location(s)";
        } else if (o.consolidate == 1) {
            r.cols = {"Part Number", "Description", "Package", "Qty", "Leads", "Lead Span" + u, "Pitch" + u, "References"};
            r.align = {0, 0, 0, 2, 2, 2, 2, 0};
            std::map<std::string, std::vector<const Component*>> g;
            for (auto* c : comps) g[c->number.empty() ? "(" + c->package + ")" : c->number].push_back(c);
            for (auto& kv : g) {
                const Component* c0 = kv.second.front();
                const PartNumber* pn = p.part_of(*c0);
                PkgMetrics m = package_metrics(p.package_of(*c0));
                std::vector<std::string> refs; std::vector<int> idx;
                for (auto* c : kv.second) { refs.push_back(c->ref); idx.push_back((int)(c - p.comps.data())); }
                r.rows.push_back({{kv.first, pn ? pn->description : "", c0->package, std::to_string(kv.second.size()), std::to_string(m.leads),
                                   len(m.has_span, m.span), len(m.has_pitch, m.pitch), compress_refs(refs)}, R_NORMAL, idx});
            }
            r.footer = std::to_string(g.size()) + " part number(s), " + std::to_string(comps.size()) + " location(s)";
        } else {
            r.cols = {"Package", "Qty", "Leads", "Lead Span" + u, "Pitch" + u, "SMT", "Part Numbers"};
            r.align = {0, 2, 2, 2, 2, 0, 0};
            std::map<std::string, std::vector<const Component*>> g;
            for (auto* c : comps) g[c->package.empty() ? std::string("(no package)") : c->package].push_back(c);
            for (auto& kv : g) {
                const Package* pk = p.package_of(*kv.second.front());
                PkgMetrics m = package_metrics(pk);
                std::set<std::string> nums; std::vector<int> idx;
                for (auto* c : kv.second) { if (!c->number.empty()) nums.insert(c->number); idx.push_back((int)(c - p.comps.data())); }
                std::string ns;
                for (auto& n : nums) { if (!ns.empty()) ns += ", "; ns += n; }
                r.rows.push_back({{kv.first, std::to_string(kv.second.size()), std::to_string(m.leads), len(m.has_span, m.span), len(m.has_pitch, m.pitch),
                                   pk ? (pk->smt ? "SMT" : "TH") : "?", ns}, R_NORMAL, idx});
            }
            r.footer = std::to_string(g.size()) + " package(s), " + std::to_string(comps.size()) + " location(s)";
        }
    } else if (which == RK_Opp) {
        int images = 1;
        if (o.per_image) { images = 0; for (auto& im : p.images) if (!im.x_out) ++images; images = std::max(images, 1); }
        r.header.push_back(o.per_image ? "Counts per panel (" + std::to_string(images) + " good image(s))" : "Counts per board");
        r.cols = {"Model Point", "Components", "SMT Joints", "TH Joints", "Total Joints", "Polarity", "No Package"};
        r.align = {0, 2, 2, 2, 2, 2, 2};
        struct Acc { long long comps = 0, smt = 0, th = 0, pol = 0, nopkg = 0; };
        std::vector<Acc> acc(np + 1);
        for (auto* cp : comps) {
            const Component& c = *cp;
            int pt = c.assigned_point;
            Acc& ac = acc[pt >= 0 && pt < np ? pt : np];
            const Package* pk = p.package_of(c);
            int leads = pk ? (pk->leads > 0 ? pk->leads : (int)pk->pads.size()) : 0;
            ac.comps += images;
            if (!pk) ac.nopkg += images;
            else if (pk->smt) ac.smt += (long long)leads * images;
            else ac.th += (long long)leads * images;
            const PartNumber* pn = p.part_of(c);
            (void)pn;
            if (pk && pkg_has_polarity(*pk)) ac.pol += images;      // R15: polarity marker
        }
        Acc tot;
        auto row = [&](const std::string& name, const Acc& ac, int kind) {
            r.rows.push_back({{name, std::to_string(ac.comps), std::to_string(ac.smt), std::to_string(ac.th), std::to_string(ac.smt + ac.th),
                               std::to_string(ac.pol), std::to_string(ac.nopkg)}, kind});
        };
        for (int i = 0; i <= np; ++i) {
            const Acc& ac = acc[i];
            if (ac.comps == 0) continue;
            row(i < np ? p.model.points[i].name : "(unassigned)", ac, R_NORMAL);
            tot.comps += ac.comps; tot.smt += ac.smt; tot.th += ac.th; tot.pol += ac.pol; tot.nopkg += ac.nopkg;
        }
        row("Total", tot, R_TOTAL);
        r.footer = "Solder joints = leads per package (package lead count, else pad count).";
    } else if (which == RK_Routing) {
        r.cols = {"Part Number", "Description", "References", "Qty"};
        r.align = {0, 0, 0, 2};
        std::vector<std::map<std::string, std::vector<const Component*>>> g(np + 1);
        for (auto& c : p.comps) {
            if (c.feature != Feature::Component || (c.dnp && !o.include_dnp)) continue;
            int pt = c.assigned_point >= 0 && c.assigned_point < np ? c.assigned_point : np;
            g[pt][c.number.empty() ? "(" + c.package + ")" : c.number].push_back(&c);
        }
        size_t total = 0;
        for (int i = 0; i <= np; ++i) {
            size_t n = 0;
            for (auto& kv : g[i]) n += kv.second.size();
            if (i == np && n == 0) break;
            std::string sec;
            if (i < np) {
                const ModelPoint& mp = p.model.points[i];
                sec = std::to_string(i + 1) + ". " + mp.name;
                if (!mp.process_type.empty()) sec += "  [" + mp.process_type + "]";
                if (!mp.line.empty()) sec += "  line " + mp.line;
                sec += mp.side == Side::Bottom ? "  \xE2\x80\x94 Bottom" : "  \xE2\x80\x94 Top";
            } else sec = "Unassigned";
            sec += "  (" + std::to_string(g[i].size()) + " part numbers)";
            r.rows.push_back({{sec, "", "", std::to_string(n)}, R_SECTION});
            for (auto& kv : g[i]) {
                const PartNumber* pn = p.part_of(*kv.second.front());
                std::vector<std::string> refs; std::vector<int> idx;
                for (auto* c : kv.second) { refs.push_back(c->ref); idx.push_back((int)(c - p.comps.data())); }
                r.rows.push_back({{kv.first, pn ? pn->description : "", join_refs(refs), std::to_string(kv.second.size())}, R_NORMAL, idx});
            }
            total += n;
        }
        r.rows.push_back({{"Total placements", "", "", std::to_string(total)}, R_TOTAL});
        r.footer = std::to_string(np) + " model point(s) in factory model \"" + p.model.name + "\"";
    } else if (which == RK_Place) {
        r.header.push_back("Coordinates relative to the virtual origin");
        r.cols = {"Reference", "Part Number", "Package", "X" + u, "Y" + u, "Rotation", "Side", "Model Point"};
        r.align = {0, 0, 0, 2, 2, 2, 0, 0};
        std::sort(comps.begin(), comps.end(), [](const Component* x, const Component* y) { return ref_less(x->ref, y->ref); });
        for (auto* c : comps) {
            Vec2 v = p.user_pos(*c);
            r.rows.push_back({{c->ref + (c->dnp ? " (DNP)" : ""), c->number, c->package, fmt_len_u8(v.x, false), fmt_len_u8(v.y, false),
                               fnum(norm_deg(c->rot), 2), side_name(c->side), pname(c->assigned_point)}, R_NORMAL, {(int)(c - p.comps.data())}});
        }
        r.footer = std::to_string(comps.size()) + " placement(s)";
    } else if (which == RK_Fid) {   // R12 §3: Data > Fiducials (fiducials, bad marks; machine reference data)
        r.cols = {"Reference", "Type", "X" + u, "Y" + u, "Side", "Shape / Package", "Model Point"};
        r.align = {0, 0, 2, 2, 0, 0, 0};
        std::vector<const Component*> f;
        for (auto& c : p.comps) if (c.feature == Feature::Fiducial || c.feature == Feature::BadMark) f.push_back(&c);
        std::sort(f.begin(), f.end(), [](const Component* x, const Component* y) { return ref_less(x->ref, y->ref); });
        for (auto* c : f) {
            Vec2 v = p.user_pos(*c);
            r.rows.push_back({{c->ref.empty() ? "(no ref)" : c->ref, c->feature == Feature::Fiducial ? "Fiducial" : "Bad mark", fmt_len_u8(v.x, false), fmt_len_u8(v.y, false),
                               side_name(c->side), c->package, pname(c->assigned_point)}, R_NORMAL, {(int)(c - p.comps.data())}});
        }
        r.footer = f.empty() ? "No fiducials yet: add them in Plot (Board > Fiducial)." : std::to_string(f.size()) + " mark(s). Click a row to select; double-click to fly to it in the Plotter.";
    } else {
        auto issues = check_placements(p);
        r.cols = {"Severity", "Check", "Message", "References"};
        r.align = {0, 0, 0, 0};
        std::stable_sort(issues.begin(), issues.end(), [&](const PlacementIssue& x, const PlacementIssue& y) {
            if (x.severity != y.severity) return x.severity > y.severity;
            if (x.code != y.code) return x.code < y.code;
            if (x.comps.empty() || y.comps.empty()) return x.comps.size() < y.comps.size();
            return ref_less(p.comps[x.comps[0]].ref, p.comps[y.comps[0]].ref);
        });
        for (auto& is : issues) {
            std::vector<std::string> refs;
            for (int i : is.comps) if (i >= 0 && i < (int)p.comps.size()) refs.push_back(p.comps[i].ref.empty() ? "?" : p.comps[i].ref);
            r.rows.push_back({{severity_name(is.severity), pretty_code(is.code), is.message, compress_refs(refs)},
                              is.severity == Severity::Error ? R_ERROR : is.severity == Severity::Warning ? R_WARN : R_NORMAL, is.comps});
        }
        r.header.push_back(std::to_string(count_issues(issues, Severity::Error)) + " error(s), " + std::to_string(count_issues(issues, Severity::Warning)) +
                           " warning(s), " + std::to_string(count_issues(issues, Severity::Info)) + " info");
        r.footer = issues.empty() ? "No problems found." : "Click a row to select the locations; double-click to fly to them in the Plotter.";
    }
    if (r.align.size() < r.cols.size()) r.align.resize(r.cols.size(), 0);
}

std::string to_csv(const Report& r) {
    std::string s = "\xEF\xBB\xBF";
    for (size_t i = 0; i < r.cols.size(); ++i) { if (i) s += ','; s += csv_field(r.cols[i]); }
    s += "\r\n";
    for (auto& row : r.rows) {
        for (size_t i = 0; i < r.cols.size(); ++i) { if (i) s += ','; s += csv_field(i < row.cells.size() ? row.cells[i] : ""); }
        s += "\r\n";
    }
    return s;
}
std::string html_esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break; case '<': o += "&lt;"; break; case '>': o += "&gt;"; break; case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}
std::string to_html(const Report& r) {
    std::string s = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>" + html_esc(r.title) + "</title>\n<style>\n"
        "body{font-family:'Segoe UI',Arial,sans-serif;font-size:13px;color:#111;margin:24px}\n"
        "h1{font-size:20px;margin:0 0 6px}\n.hdr{color:#444;margin:0 0 14px}\n.hdr span{margin-right:18px;white-space:nowrap}\n"
        "table{border-collapse:collapse;width:100%}\nth,td{border:1px solid #ccc;padding:3px 7px;text-align:left;vertical-align:top}\n"
        "th{background:#eef1f4}\ntd.r{text-align:right}\ntr.sec td{background:#e3ecf7;font-weight:600}\n"
        "tr.tot td{font-weight:600;border-top:2px solid #888}\ntr.err td:first-child{color:#c62828;font-weight:600}\n"
        "tr.warn td:first-child{color:#b26a00;font-weight:600}\n.ftr{color:#555;margin-top:10px}\n@media print{body{margin:0}}\n</style></head><body>\n";
    s += "<h1>" + html_esc(r.title) + "</h1>\n<div class=\"hdr\">";
    for (auto& h : r.header) s += "<span>" + html_esc(h) + "</span>";
    s += "</div>\n<table>\n<thead><tr>";
    for (auto& c : r.cols) s += "<th>" + html_esc(c) + "</th>";
    s += "</tr></thead>\n<tbody>\n";
    for (auto& row : r.rows) {
        if (row.kind == R_SECTION) {
            s += "<tr class=\"sec\"><td colspan=\"" + std::to_string(std::max<size_t>(1, r.cols.size() - 1)) + "\">" + html_esc(row.cells.empty() ? "" : row.cells[0]) +
                 "</td><td class=\"r\">" + html_esc(row.cells.size() >= r.cols.size() && !r.cols.empty() ? row.cells[r.cols.size() - 1] : "") + "</td></tr>\n";
            continue;
        }
        s += row.kind == R_TOTAL ? "<tr class=\"tot\">" : row.kind == R_ERROR ? "<tr class=\"err\">" : row.kind == R_WARN ? "<tr class=\"warn\">" : "<tr>";
        for (size_t i = 0; i < r.cols.size(); ++i)
            s += std::string(r.align[i] == 2 ? "<td class=\"r\">" : "<td>") + html_esc(i < row.cells.size() ? row.cells[i] : "") + "</td>";
        s += "</tr>\n";
    }
    s += "</tbody></table>\n";
    if (!r.footer.empty()) s += "<div class=\"ftr\">" + html_esc(r.footer) + "</div>\n";
    return s + "</body></html>\n";
}
std::string to_tsv(const Report& r) {
    std::string s;
    for (size_t i = 0; i < r.cols.size(); ++i) { if (i) s += '\t'; s += r.cols[i]; }
    s += "\r\n";
    for (auto& row : r.rows) {
        for (size_t i = 0; i < r.cols.size(); ++i) {
            if (i) s += '\t';
            std::string c = i < row.cells.size() ? row.cells[i] : "";
            for (auto& ch : c) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
            s += c;
        }
        s += "\r\n";
    }
    return s;
}

struct ReportsView : View {
    ViewKind kind() const override { return vk; }
    const char* title() const override { return vk == ViewKind::Validation ? "Validation" : vk == ViewKind::Fiducials ? "Fiducials" : "Reports"; }
    ui::Icon icon() const override { return ui::Icon::Report; }

    ViewKind vk = ViewKind::Reports;
    int locked = -1;              // Data > Validation / Fiducials: fixed report, no report list
    int which = RK_Place;
    ROpts opts;
    Report rep;
    uint64_t gen_seen = 0, hash_seen = 0;
    bool dirty = true;
    int sel_row = -1;
    std::vector<float> col_w;     // last on-screen widths (print proportions)

    void take_request() {
        if (locked >= 0) { if (which != locked) { which = locked; dirty = true; } return; }
        if (!g_report_request) return;
        switch (g_report_request) {
        case app::ID_MFG_PREP_REPORT: which = RK_Prep; break;
        case app::ID_MFG_OPPORTUNITY: which = RK_Opp; break;
        case app::ID_MFG_ROUTING_REPORT: which = RK_Routing; break;
        case app::ID_MFG_PLACEMENT_REPORT: which = RK_Place; break;
        case app::ID_MFG_CHECK: which = RK_Review; break;
        }
        g_report_request = 0;
        dirty = true;
    }
    void on_activate() override { take_request(); }

    void sync() {
        take_request();
        if (doc->gen != gen_seen) {
            gen_seen = doc->gen;
            uint64_t h = content_hash(doc->prj);
            if (h != hash_seen) { hash_seen = h; dirty = true; }
        }
        if (opts.point >= (int)doc->prj.model.points.size()) { opts.point = -1; dirty = true; }
        if (dirty) { build_report(*doc, which, opts, rep); dirty = false; sel_row = -1; flash(ImHashStr("rp_rebuilt")); }
    }

    std::wstring default_file(const wchar_t* ext) const {
        std::wstring base = project_program_name(doc->prj).empty() ? std::wstring(L"report") : ui::W(project_program_name(doc->prj));   // v17
        std::wstring t = ui::W(rep.title);
        for (auto& c : t) if (c == L' ') c = L'_';
        std::wstring f = base + L"_" + t + L"." + ext;
        for (auto& c : f) if (wcschr(L"<>:\"/\\|?*", c)) c = L'_';
        return f;
    }
    void save_csv() { save_text_file(to_csv(rep), L"Save Report as CSV", L"CSV files", L"*.csv", L"csv", default_file(L"csv")); }
    void save_html() { save_text_file(to_html(rep), L"Save Report as HTML", L"HTML files", L"*.html;*.htm", L"html", default_file(L"html")); }
    void copy() { copy_text(to_tsv(rep)); toast(rep.title + " copied to the clipboard", ToastKind::Success); }

    bool can_print() const override { return true; }
    bool on_command(int id) override {
        if (id == app::ID_EDIT_COPY) { copy(); return true; }
        return false;
    }
    int cmd_state(int id) override {
        if (id == app::ID_EDIT_COPY) return 1;
        if (id == app::ID_EDIT_DELETE || id == app::ID_EDIT_CUT || id == app::ID_EDIT_PASTE) return 0;
        return -1;
    }
    std::string status() const override { return rep.title + "  \xC2\xB7  " + std::to_string(rep.rows.size()) + " rows"; }

    void draw_toolbar() override {
        if (icon_button("rp_ref", ui::Icon::Redo, "Regenerate report")) dirty = true;
        toolbar_separator();
        if (icon_button("rp_csv", ui::Icon::Export, "Save as CSV...")) save_csv();
        ImGui::SameLine(0, S(2));
        if (icon_button("rp_html", ui::Icon::Save, "Save as HTML...")) save_html();
        ImGui::SameLine(0, S(2));
        if (icon_button("rp_copy", ui::Icon::Copy, "Copy (tab separated)  (Ctrl+C)")) copy();
        toolbar_separator();
        ImGui::SameLine(0, S(2));
        if (icon_button("rp_print", ui::Icon::Print, "Print...")) post_command(app::ID_FILE_PRINT);
    }

    void draw(ImVec2 size) override {
        sync();
        const GTheme& t = th();
        // ---- left list (not for the fixed Data > Validation / Fiducials views)
        if (locked < 0) {
        const float lw = std::min(S(250), size.x * 0.3f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(8));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(10)));
        ImGui::BeginChild("##rlist", ImVec2(lw, size.y), ImGuiChildFlags_AlwaysUseWindowPadding);
        section_header("  REPORTS");
        ImGui::Dummy(ImVec2(0, S(4)));
        for (int k = 0; k < RK_Count; ++k) {
            ImGui::PushID(k);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = S(50);
            if (ImGui::InvisibleButton("##r", ImVec2(w, h)) && which != k) { which = k; dirty = true; }
            const bool hov = ImGui::IsItemHovered(), on = which == k;
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            const float ha = anim(ImGui::GetID("h"), hov ? 1.f : 0.f, 16.f), oa = anim(ImGui::GetID("o"), on ? 1.f : 0.f, 12.f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (ha > 0.01f) dl->AddRectFilled(p0, p0 + ImVec2(w, h), col_alpha(t.raised, ha), S(8));
            if (oa > 0.01f) dl->AddRectFilled(p0, p0 + ImVec2(w, h), col_alpha(t.accent, 0.19f * oa), S(8));
            if (oa > 0.01f) { float bh = (h - S(20)) * oa; dl->AddRectFilled(ImVec2(p0.x, p0.y + h * 0.5f - bh * 0.5f), ImVec2(p0.x + S(3), p0.y + h * 0.5f + bh * 0.5f), t.accent, S(2)); }
            const float is = S(24);
            icon_draw(dl, kRIcons[k], ImVec2(p0.x + S(12) + S(2) * ha, p0.y + (h - is) * 0.5f), is);
            dl->AddText(g_font_bold, g_font_bold->FontSize, ImVec2(p0.x + S(46), p0.y + S(8)), t.text, kRNames[k]);
            ImGui::PushClipRect(p0, p0 + ImVec2(w - S(4), h), true);
            dl->AddText(ImVec2(p0.x + S(46), p0.y + S(27)), t.text_faint, kRDesc[k]);
            ImGui::PopClipRect();
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, S(2)));
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::SameLine(0, S(10));
        }
        // ---- right
        ImGui::BeginChild("##rbody", ImVec2(0, size.y));
        ImGui::PushFont(g_font_big);
        ImGui::TextUnformatted(rep.title.c_str());
        ImGui::PopFont();
        {
            std::string h;
            for (size_t i = 0; i < rep.header.size(); ++i) { if (i) h += "    \xC2\xB7    "; h += rep.header[i]; }
            ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
            ImGui::TextWrapped("%s", h.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, S(2)));
        draw_options();
        ImGui::Dummy(ImVec2(0, S(2)));
        const float th_ = ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() - S(6);
        draw_table(th_);
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_faint);
        ImGui::TextUnformatted(rep.footer.c_str());
        ImGui::PopStyleColor();
        ImGui::EndChild();
    }

    void draw_options() {
        const Project& p = doc->prj;
        ImGui::AlignTextToFramePadding();
        if (which != RK_Routing && which != RK_Review && which != RK_Fid) {
            ImGui::TextUnformatted("Include");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(S(190));
            std::string cur = opts.point < 0 ? "All locations" : p.model.points[opts.point].name;
            if (ImGui::BeginCombo("##scope", cur.c_str())) {
                if (ImGui::Selectable("All locations", opts.point < 0)) { opts.point = -1; dirty = true; }
                for (size_t i = 0; i < p.model.points.size(); ++i)
                    if (ImGui::Selectable((p.model.points[i].name + "##s" + std::to_string(i)).c_str(), opts.point == (int)i)) { opts.point = (int)i; dirty = true; }
                ImGui::EndCombo();
            }
            ImGui::SameLine(0, S(14));
        }
        if (which == RK_Prep) {
            ImGui::TextUnformatted("Consolidate by");
            static const char* cons[] = {"Location", "Number", "Package"};
            static const ui::Icon ci[] = {ui::Icon::Locations, ui::Icon::Tag, ui::Icon::PackageEditor};
            for (int i = 0; i < 3; ++i) { ImGui::SameLine(0, S(4)); if (chip(cons[i], opts.consolidate == i, -1, ci[i])) { opts.consolidate = i; dirty = true; } }
            ImGui::SameLine(0, S(14));
        }
        if (which == RK_Opp) { if (ImGui::Checkbox("Count every good panel image", &opts.per_image)) dirty = true; ImGui::SameLine(0, S(14)); }
        if (which != RK_Review && which != RK_Fid) { if (ImGui::Checkbox("Include DNP", &opts.include_dnp)) dirty = true; ImGui::SameLine(0, S(14)); }
        // right-aligned actions
        float bw = S(96);
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail > bw * 4 + S(12)) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - (bw * 4 + S(12)));
        if (button("CSV", ui::Icon::Export, false, ImVec2(bw, 0))) save_csv();
        ImGui::SameLine(0, S(4));
        if (button("HTML", ui::Icon::Save, false, ImVec2(bw, 0))) save_html();
        ImGui::SameLine(0, S(4));
        if (button("Copy", ui::Icon::Copy, false, ImVec2(bw, 0))) copy();
        ImGui::SameLine(0, S(4));
        if (button("Print", ui::Icon::Print, true, ImVec2(bw, 0))) post_command(app::ID_FILE_PRINT);
    }

    void draw_table(float h) {
        const GTheme& t = th();
        if (dirty) { build_report(*doc, which, opts, rep); dirty = false; sel_row = -1; flash(ImHashStr("rp_rebuilt")); }   // list/options clicked this frame
        const int nc = (int)rep.cols.size();
        if (nc == 0 || rep.rows.empty()) {
            ImGui::BeginChild("##rep_empty", ImVec2(0, h));
            if (empty_state(ui::Icon::Report, "Nothing to report", doc->prj.comps.empty() ? "Import CAD data to fill this report." : "No rows for the current options.",
                            doc->prj.comps.empty() ? "Import CAD..." : nullptr, ui::Icon::ImportCad))
                post_command(app::ID_IMPORT_CAD);
            ImGui::EndChild();
            return;
        }
        // fade the table in after a rebuild (tab switch / options)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (1.f - 0.7f * flash_amount(ImHashStr("rp_rebuilt"), 0.35f)));
        struct PopA { ~PopA() { ImGui::PopStyleVar(); } } pop_a;
        ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
        ImGui::PushID(which * 10 + opts.consolidate);
        if (!ImGui::BeginTable("##rep", nc, tf, ImVec2(0, h))) { ImGui::PopID(); return; }
        ImGui::TableSetupScrollFreeze(1, 1);
        // default widths from content (sampled)
        for (int i = 0; i < nc; ++i) {
            float w = ImGui::CalcTextSize(rep.cols[i].c_str()).x + S(16);
            size_t step = std::max<size_t>(1, rep.rows.size() / 200);
            for (size_t r = 0; r < rep.rows.size(); r += step) {
                if (rep.rows[r].kind == R_SECTION || i >= (int)rep.rows[r].cells.size()) continue;
                w = std::max(w, ImGui::CalcTextSize(rep.rows[r].cells[i].c_str()).x + S(14));
            }
            w = std::min(w, S(420));
            ImGui::TableSetupColumn(rep.cols[i].c_str(), ImGuiTableColumnFlags_WidthFixed, w);
        }
        ImGui::TableHeadersRow();
        col_w.resize(nc);
        for (int i = 0; i < nc; ++i) { ImGui::TableSetColumnIndex(i); col_w[i] = ImGui::GetColumnWidth(); }
        ImGuiListClipper clip;
        clip.Begin((int)rep.rows.size());
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const RRow& row = rep.rows[r];
                ImGui::TableNextRow();
                ImGui::PushID(r);
                if (row.kind == R_SECTION) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, alpha(t.accent, 0x2C));
                ImGui::TableSetColumnIndex(0);
                if (!row.comps.empty() || row.kind == R_NORMAL) {
                    if (ImGui::Selectable("##s", sel_row == r, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick)) {
                        sel_row = r;
                        if (!row.comps.empty()) {
                            for (auto& c : doc->prj.comps) c.selected = false;
                            for (int i : row.comps) if (i >= 0 && i < (int)doc->prj.comps.size()) doc->prj.comps[i].selected = true;
                            doc->changed(ui::CH_SELECTION);
                            if (ImGui::IsMouseDoubleClicked(0)) { focus_components(doc, row.comps, true); activate_view(doc, ViewKind::Plotter); }
                        }
                    }
                    ImGui::SameLine(0, 0);
                }
                const bool bold = row.kind == R_SECTION || row.kind == R_TOTAL;
                if (bold) ImGui::PushFont(g_font_bold);
                for (int i = 0; i < nc; ++i) {
                    if (row.kind == R_SECTION && i > 0 && i < nc - 1) continue;
                    ImGui::TableSetColumnIndex(i);
                    const std::string& s = i < (int)row.cells.size() ? row.cells[i] : std::string();
                    ImU32 col = t.text;
                    if (row.kind == R_SECTION) col = t.accent;
                    if (i == 0 && row.kind == R_ERROR) col = t.error;
                    if (i == 0 && row.kind == R_WARN) col = t.warn;
                    if (i == 0 && row.kind == R_NORMAL && which == RK_Review) col = t.text_faint;
                    if (rep.align[i] == 2) {
                        float w = ImGui::CalcTextSize(s.c_str()).x;
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetColumnWidth() - w - S(4)));
                    }
                    if (row.kind == R_SECTION && i == 0) {
                        // section text overflows across the empty middle columns
                        ImVec2 cp = ImGui::GetCursorScreenPos();
                        ImGui::PushClipRect(ImVec2(cp.x - S(8), cp.y - S(4)), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), cp.y + ImGui::GetTextLineHeight() + S(4)), false);
                        ImGui::GetWindowDrawList()->AddText(cp, col, s.c_str());
                        ImGui::PopClipRect();
                        ImGui::Dummy(ImVec2(1, ImGui::GetTextLineHeight()));
                    } else ImGui::TextColored(ImColor(col), "%s", s.c_str());
                }
                if (bold) ImGui::PopFont();
                ImGui::PopID();
            }
        ImGui::EndTable();
        ImGui::PopID();
    }

    // ---- GDI print (multi-page). The shell has called StartPage and calls EndPage after.
    void print(HDC dc, const RECT& page, bool) override {
        const int dpy = GetDeviceCaps(dc, LOGPIXELSY), dpx = GetDeviceCaps(dc, LOGPIXELSX);
        auto mkfont = [&](int pt10, bool b) {
            return CreateFontW(-MulDiv(pt10, dpy, 720), 0, 0, 0, b ? FW_SEMIBOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        };
        HFONT ft = mkfont(140, true), fh = mkfont(85, false), fb = mkfont(85, false), fbb = mkfont(85, true);
        HGDIOBJ old = SelectObject(dc, fb);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0, 0, 0));
        const int m = dpx / 2;
        const int L = page.left + m, R = page.right - m, T = page.top + m, B = page.bottom - m;
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        const int lh = tm.tmHeight + tm.tmExternalLeading + dpy / 30;
        const int pad = dpx / 30;
        const size_t nc = rep.cols.size();
        std::vector<double> w(nc, 1.0);
        double sum = 0;
        for (size_t i = 0; i < nc; ++i) { w[i] = i < col_w.size() ? std::max(20.f, col_w[i]) : 100.0; sum += w[i]; }
        std::vector<int> x(nc + 1, L);
        for (size_t i = 0; i < nc; ++i) x[i + 1] = x[i] + (int)((R - L) * w[i] / std::max(sum, 1.0));
        x[nc] = R;
        HPEN pen = CreatePen(PS_SOLID, std::max(1, dpx / 200), RGB(160, 160, 160));
        HGDIOBJ oldpen = SelectObject(dc, pen);
        HBRUSH hdrbr = CreateSolidBrush(RGB(232, 236, 240)), secbr = CreateSolidBrush(RGB(222, 233, 246));
        int pageno = 1, y = T;
        auto cell = [&](const std::string& s8, int x0, int x1, int y0, int align) {
            std::wstring s = ui::W(s8);
            RECT rc{x0 + pad, y0, x1 - pad, y0 + lh};
            DrawTextW(dc, s.c_str(), -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | (align == 2 ? DT_RIGHT : DT_LEFT));
        };
        auto table_header = [&]() {
            RECT hr{L, y, R, y + lh};
            FillRect(dc, &hr, hdrbr);
            SelectObject(dc, fbb);
            for (size_t i = 0; i < nc; ++i) cell(rep.cols[i], x[i], x[i + 1], y, rep.align[i]);
            MoveToEx(dc, L, y + lh, nullptr); LineTo(dc, R, y + lh);
            y += lh;
            SelectObject(dc, fb);
        };
        auto footer = [&]() {
            SelectObject(dc, fh);
            std::wstring f = ui::W(rep.title) + L"  \x2014  " + doc->display_name() + L"    Page " + std::to_wstring(pageno);
            RECT fr{L, B, R, B + lh};
            DrawTextW(dc, f.c_str(), -1, &fr, DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX);
            SelectObject(dc, fb);
        };
        SelectObject(dc, ft);
        {
            std::wstring tt = ui::W(rep.title);
            RECT tr{L, y, R, y + lh * 2};
            DrawTextW(dc, tt.c_str(), -1, &tr, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);
            y += MulDiv(22, dpy, 72);
        }
        SelectObject(dc, fh);
        {
            std::wstring h;
            for (size_t i = 0; i < rep.header.size(); ++i) { if (i) h += L"     "; h += ui::W(rep.header[i]); }
            RECT hr{L, y, R, B}, calc = hr;
            DrawTextW(dc, h.c_str(), -1, &calc, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            DrawTextW(dc, h.c_str(), -1, &hr, DT_WORDBREAK | DT_NOPREFIX);
            y += (calc.bottom - calc.top) + lh / 2;
        }
        SelectObject(dc, fb);
        table_header();
        for (size_t r = 0; r < rep.rows.size(); ++r) {
            if (y + lh > B - lh) {
                footer();
                EndPage(dc);
                StartPage(dc);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(0, 0, 0));
                SelectObject(dc, pen);
                ++pageno;
                y = T;
                table_header();
            }
            const RRow& row = rep.rows[r];
            if (row.kind == R_SECTION) {
                RECT sr{L, y, R, y + lh};
                FillRect(dc, &sr, secbr);
                SelectObject(dc, fbb);
                cell(row.cells.empty() ? "" : row.cells[0], L, x[nc - 1], y, 0);
                if (row.cells.size() >= nc) cell(row.cells[nc - 1], x[nc - 1], R, y, rep.align[nc - 1]);
                SelectObject(dc, fb);
            } else {
                if (row.kind == R_TOTAL) { SelectObject(dc, fbb); MoveToEx(dc, L, y, nullptr); LineTo(dc, R, y); }
                for (size_t i = 0; i < nc && i < row.cells.size(); ++i) {
                    if (i == 0 && row.kind == R_ERROR) SetTextColor(dc, RGB(198, 40, 40));
                    if (i == 0 && row.kind == R_WARN) SetTextColor(dc, RGB(178, 106, 0));
                    cell(row.cells[i], x[i], x[i + 1], y, rep.align[i]);
                    SetTextColor(dc, RGB(0, 0, 0));
                }
                if (row.kind == R_TOTAL) SelectObject(dc, fb);
            }
            y += lh;
            MoveToEx(dc, L, y, nullptr); LineTo(dc, R, y);
        }
        if (!rep.footer.empty() && y + lh <= B - lh) {
            SelectObject(dc, fh);
            std::wstring f = ui::W(rep.footer);
            RECT fr{L, y + lh / 3, R, y + lh / 3 + lh};
            DrawTextW(dc, f.c_str(), -1, &fr, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        footer();
        SelectObject(dc, oldpen);
        SelectObject(dc, old);
        DeleteObject(pen); DeleteObject(hdrbr); DeleteObject(secbr);
        DeleteObject(ft); DeleteObject(fh); DeleteObject(fb); DeleteObject(fbb);
    }
};

} // namespace

std::unique_ptr<View> make_placements_view() { return std::make_unique<PlacementsView>(); }
std::unique_ptr<View> make_bom_view() { return std::make_unique<BomView>(); }
std::unique_ptr<View> make_reports_view() { return std::make_unique<ReportsView>(); }
std::unique_ptr<View> make_validation_view() { auto v = std::make_unique<ReportsView>(); v->vk = ViewKind::Validation; v->locked = v->which = RK_Review; return v; }
std::unique_ptr<View> make_fiducials_view() { auto v = std::make_unique<ReportsView>(); v->vk = ViewKind::Fiducials; v->locked = v->which = RK_Fid; return v; }

} // namespace pcbp::gui
