// ============================================================================
// gdialogs.cpp — ImGui-driven dialogs built on ui::Form::custom:
//   * Fiducial Properties: pictogram cards for the fiducial type (Board /
//     Circuit / Component fiducial, see specs/FIDUCIALS.md) and number
//     (Fiducial 1 / 2 / 3 / Additional), linked component, live preview.
//   * Panelization (#202 + #321): rows x cols cell grid (rotation per cell,
//     Yellow = root, Red / Blue reference cells per rotation group), Image
//     Creation offsets, Panelize, Step And Repeat All Images, live preview.
//     Logic lives in core/panelize.
//   * PNP / centroid (#508) and BOM (#166) import on io::text_import: raw
//     preview with per-column role combos in the header, separator / fixed
//     width cut editing, skip / header line, decimal comma, units + bottom side
//     conventions, Replace / Merge / Fusion, templates, issues.
// ============================================================================
#include <ctime>
#include "../app/app.h"
#include "../core/fileio.h"
#include "../core/panelize.h"
#include "../core/plot_align.h"
#include "../core/arrays.h"   // R22
#include "../core/transform.h"
#include "../io/cad_common.h"
#include "../io/bom_import2.h"
#include "../io/m1_bom.h"
#include "bom_review_ui.h"
#include "../io/io_util.h"
#include "../io/pnp_import.h"
#include "../ui/form.h"
#include "gapp.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <winhttp.h>  // opt-in supplier lookup (one-shot; no background service)
#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif

namespace pcbp::gui {   // gform.cpp (R17c): framework-owned dialog geometry requests
void form_initial_size(ImVec2 sz);
bool form_is_maximized();
void form_toggle_maximize();
}
namespace pcbp::app {
using namespace pcbp::ui;
using gui::th;
using gui::S;

namespace {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
int iround(double v) { return (int)std::lround(v); }
ImU32 alpha(ImU32 c, int a) { return (c & 0x00FFFFFF) | ((ImU32)a << 24); }
const ImU32 kYellow = IM_COL32(240, 196, 40, 255), kRed = IM_COL32(226, 70, 64, 255), kBlue = IM_COL32(60, 128, 236, 255);

DocPtr docptr_of(const Doc& d) {
    for (auto& p : open_docs()) if (p.get() == &d) return p;
    return nullptr;
}
void after_import(Doc& d) {
    if (DocPtr dp = docptr_of(d)) { gui::activate_view(dp, gui::ViewKind::Plotter); gui::post_command(ID_VIEW_ZOOM_ALL); }
}

// world (mm, +Y up) -> screen fit
struct Fit {
    BBox bb; ImVec2 a, b; float sc = 1; ImVec2 o;
    Fit(BBox box, ImVec2 a_, ImVec2 b_, float pad) : bb(box), a(a_), b(b_) {
        if (bb.empty()) bb = BBox(0, 0, 10, 10);
        double w = std::max(1e-3, bb.w()), h = std::max(1e-3, bb.h());
        sc = (float)std::min((b.x - a.x - 2 * pad) / w, (b.y - a.y - 2 * pad) / h);
        o = ImVec2((a.x + b.x) * 0.5f - (float)(bb.center().x * sc), (a.y + b.y) * 0.5f + (float)(bb.center().y * sc));
    }
    ImVec2 operator()(Vec2 p) const { return ImVec2(o.x + (float)p.x * sc, o.y - (float)p.y * sc); }
};

void panel_bg(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const gui::GTheme& t = th();
    dl->AddRectFilled(a, b, t.canvas, S(8));
    dl->AddRect(a, b, t.border, S(8));
}

void draw_board(ImDrawList* dl, const Project& p, const Fit& f, Xform x = Xform{}, ImU32 fill = 0, bool comps = true) {
    const gui::GTheme& t = th();
    const BBox bb = p.board_bbox();
    ImVec2 q[4] = {f(x.apply({bb.x0, bb.y0})), f(x.apply({bb.x1, bb.y0})), f(x.apply({bb.x1, bb.y1})), f(x.apply({bb.x0, bb.y1}))};
    dl->AddConvexPolyFilled(q, 4, fill ? fill : alpha(t.ok, 40));
    if (!p.board_frame.empty()) {
        for (auto& pl : p.board_frame) {
            std::vector<ImVec2> v;
            for (auto& pt : pl) v.push_back(f(x.apply(pt)));
            if (v.size() > 1) dl->AddPolyline(v.data(), (int)v.size(), alpha(t.ok, 200), 0, 1.2f);
        }
    } else dl->AddPolyline(q, 4, alpha(t.ok, 200), ImDrawFlags_Closed, 1.2f);
    if (comps) for (auto& c : p.comps) {
        if (c.feature == Feature::Fiducial) continue;
        ImVec2 s = f(x.apply(c.pos));
        dl->AddRectFilled(ImVec2(s.x - 1.2f, s.y - 1.2f), ImVec2(s.x + 1.2f, s.y + 1.2f), alpha(t.text_faint, 160));
    }
}

void fid_mark(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool filled = true) {
    if (filled) dl->AddCircleFilled(c, r * 0.55f, col);
    dl->AddCircle(c, r, col, 0, 1.4f);
    dl->AddLine(ImVec2(c.x - r * 1.5f, c.y), ImVec2(c.x - r * 1.1f, c.y), col, 1.2f);
    dl->AddLine(ImVec2(c.x + r * 1.1f, c.y), ImVec2(c.x + r * 1.5f, c.y), col, 1.2f);
}

// A selectable pictogram card. Returns true when clicked.
bool picto_card(const char* id, const char* label, bool on, bool enabled, float w, float h,
                const std::function<void(ImDrawList*, ImVec2, ImVec2, ImU32, ImU32)>& paint) {
    const gui::GTheme& t = th();
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, on ? alpha(t.accent, 34) : hov ? t.raised : t.panel2, S(10));
    dl->AddRect(a, b, on ? t.accent : hov ? alpha(t.accent, 120) : t.border, S(10), 0, on ? 2.f : 1.f);
    ImU32 fg = enabled ? t.text_dim : t.text_faint, ac = enabled ? t.accent : t.text_faint;
    float lh = ImGui::GetFontSize() + S(10);
    paint(dl, ImVec2(a.x + S(10), a.y + S(8)), ImVec2(b.x - S(10), b.y - lh), fg, ac);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(a.x + (w - ts.x) * 0.5f, b.y - lh + S(3)), on ? t.text : (enabled ? t.text_dim : t.text_faint), label);
    if (on) { dl->AddCircleFilled(ImVec2(b.x - S(11), a.y + S(11)), S(6), t.accent); ImVec2 k(b.x - S(11), a.y + S(11)); ImVec2 pts[3] = {ImVec2(k.x - S(3), k.y), ImVec2(k.x - S(1), k.y + S(2.5f)), ImVec2(k.x + S(3), k.y - S(2.5f))}; dl->AddPolyline(pts, 3, t.accent_text, 0, 1.6f); }
    return clicked && enabled;
}

// pictogram primitives in a normalized box
struct Box { ImVec2 a, b; ImVec2 at(float u, float v) const { return ImVec2(a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * v); } };

void picto_panel(ImDrawList* dl, Box bx, ImU32 fg, ImU32 ac, int mode) {   // mode 0 master, 1 image, 2 local, 3 component
    float r = std::max(2.5f, (bx.b.y - bx.a.y) * 0.07f);
    if (mode == 2 || mode == 3) {
        ImVec2 c0 = bx.at(0.30f, 0.25f), c1 = bx.at(0.70f, 0.75f);
        dl->AddRectFilled(c0, c1, alpha(fg, 50), S(2));
        dl->AddRect(c0, c1, fg, S(2), 0, 1.2f);
        for (int i = 0; i < 4; ++i) {
            float u = 0.36f + i * 0.093f;
            dl->AddLine(bx.at(u, 0.14f), bx.at(u, 0.25f), fg, 1.2f); dl->AddLine(bx.at(u, 0.75f), bx.at(u, 0.86f), fg, 1.2f);
            float v = 0.31f + i * 0.127f;
            dl->AddLine(bx.at(0.20f, v), bx.at(0.30f, v), fg, 1.2f); dl->AddLine(bx.at(0.70f, v), bx.at(0.80f, v), fg, 1.2f);
        }
        if (mode == 2) { fid_mark(dl, bx.at(0.12f, 0.92f), r, ac); fid_mark(dl, bx.at(0.88f, 0.08f), r, ac); }
        else dl->AddCircleFilled(bx.at(0.36f, 0.33f), r * 0.6f, ac);
        return;
    }
    // panel with 2x2 images
    dl->AddRect(bx.a, bx.b, fg, S(3), 0, 1.2f);
    for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) {
        ImVec2 a = bx.at(0.14f + i * 0.40f, 0.16f + j * 0.38f), b = bx.at(0.46f + i * 0.40f, 0.46f + j * 0.38f);
        bool hl = mode == 1 && i == 0 && j == 1;
        dl->AddRectFilled(a, b, hl ? alpha(ac, 60) : alpha(fg, 40), S(2));
        dl->AddRect(a, b, hl ? ac : fg, S(2), 0, hl ? 1.6f : 1.f);
    }
    if (mode == 0) { fid_mark(dl, bx.at(0.06f, 0.93f), r, ac); fid_mark(dl, bx.at(0.94f, 0.07f), r, ac); fid_mark(dl, bx.at(0.94f, 0.93f), r, ac); }
    else { fid_mark(dl, bx.at(0.19f, 0.78f), r * 0.8f, ac); fid_mark(dl, bx.at(0.41f, 0.60f), r * 0.8f, ac); }
}

void picto_role(ImDrawList* dl, Box bx, ImU32 fg, ImU32 ac, int role) {   // 0 zero, 1 first skew, 2 second skew, 3 other
    float r = std::max(2.5f, (bx.b.y - bx.a.y) * 0.08f);
    ImVec2 a = bx.at(0.10f, 0.12f), b = bx.at(0.90f, 0.88f);
    dl->AddRectFilled(a, b, alpha(fg, 30), S(3));
    dl->AddRect(a, b, fg, S(3), 0, 1.1f);
    ImVec2 p0 = bx.at(0.22f, 0.76f), p1 = bx.at(0.78f, 0.24f), p2 = bx.at(0.78f, 0.76f);
    ImVec2 pts[3] = {p0, p1, p2};
    for (int i = 0; i < 3; ++i) {
        bool hl = i == role;
        fid_mark(dl, pts[i], r, hl ? ac : alpha(fg, 140), hl);
    }
    if (role == 0) {   // origin axes
        dl->AddLine(p0, ImVec2(p0.x + (b.x - a.x) * 0.30f, p0.y), ac, 1.6f);
        dl->AddLine(p0, ImVec2(p0.x, p0.y - (b.y - a.y) * 0.40f), ac, 1.6f);
    } else if (role == 1) {
        dl->AddLine(p0, p1, alpha(ac, 170), 1.3f);
    } else if (role == 2) {
        dl->AddLine(p0, p2, alpha(ac, 170), 1.3f);
    } else {
        ImVec2 c = bx.at(0.5f, 0.5f);
        dl->AddCircleFilled(c, r * 1.9f, alpha(ac, 50));
        ImVec2 ts = ImGui::CalcTextSize("#");
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), ac, "#");
    }
}

// rotated mini board glyph (for grid cells): rectangle with a pin-1 notch
void board_glyph(ImDrawList* dl, ImVec2 c, float s, int rot, ImU32 fg, ImU32 fill) {
    Vec2 pts[4] = {{-0.42, -0.28}, {0.42, -0.28}, {0.42, 0.28}, {-0.42, 0.28}};
    ImVec2 q[4];
    for (int i = 0; i < 4; ++i) { Vec2 r = rotate(pts[i], rot); q[i] = ImVec2(c.x + (float)r.x * s, c.y - (float)r.y * s); }
    dl->AddConvexPolyFilled(q, 4, fill);
    dl->AddPolyline(q, 4, fg, ImDrawFlags_Closed, 1.2f);
    Vec2 n = rotate(Vec2{-0.28, 0.14}, rot);
    dl->AddCircleFilled(ImVec2(c.x + (float)n.x * s, c.y - (float)n.y * s), std::max(1.5f, s * 0.07f), fg);
}

} // namespace

// ===========================================================================
// Fiducial properties
// ===========================================================================
bool dlg_fiducial(HWND owner, Doc& d, std::vector<Component*> comps) {
    if (comps.empty()) comps = d.selection();
    if (comps.empty()) { message(owner, L"Fiducial Properties", L"Select the component(s) to turn into fiducials first."); return false; }
    Project& p = d.prj;
    Component& c0 = *comps[0];
    std::wstring names;
    for (size_t i = 0; i < comps.size() && i < 12; ++i) { if (i) names += L", "; names += comps[i]->ref.empty() ? L"(no ref)" : W(comps[i]->ref); }
    if (comps.size() > 12) names += L", \x2026";

    struct St { int type = 0, role = 0; } st;
    st.type = c0.feature != Feature::Fiducial ? 0 : c0.fid_scope == FiducialScope::Image ? 1 : c0.fid_scope == FiducialScope::Local ? 2 : 0;
    int next = 1;
    std::set<int> used;
    for (auto& c : p.comps)
        if (c.feature == Feature::Fiducial && std::find(comps.begin(), comps.end(), &c) == comps.end()) { next = std::max(next, c.fid_number + 1); used.insert(c.fid_number); }
    int num = c0.feature == Feature::Fiducial && c0.fid_number > 0 ? c0.fid_number : 0;
    if (!num) { num = 1; while (used.count(num) && num < 3) ++num; if (used.count(num)) num = std::max(4, next); }
    st.role = num >= 1 && num <= 3 ? num - 1 : 3;

    std::wstring dia = L"\x2014";
    if (const Package* pk = p.package_of(c0)) {
        if (!pk->pads.empty()) {
            const Pad& pd = pk->pads[0];
            dia = pd.w == pd.h ? fmt_len(pd.w) : fmt_len(pd.w) + L" \x00D7 " + fmt_len(pd.h);
            dia += pd.shape == PadShape::Round ? L" round" : L" pad";
        } else dia = fmt_len(std::max(pk->extent().w(), pk->extent().h())) + L" body";
    }
    std::vector<std::wstring> owners{L"(none)"};
    {
        std::vector<std::string> r;
        for (auto& c : p.comps) if (!c.ref.empty() && c.feature == Feature::Component) r.push_back(c.ref);
        std::sort(r.begin(), r.end(), ref_less);
        for (auto& x : r) owners.push_back(W(x));
    }
    std::wstring owner_sel = L"(none)";
    for (auto& c : p.comps) if ((int)c.id == c0.local_fid_owner && !c.ref.empty()) owner_sel = W(c.ref);

    // P2-03: only the three real fiducial classes (removal is the context menu's Remove Fiducial); compact layout
    // (type cards, one number row, short preview) so OK is reachable without scrolling at 175 % UI scale.
    if (st.type > 2) st.type = 0;
    Form f(L"Fiducial", 600);
    f.icon((int)Icon::Fiducial).header(c0.feature == Feature::Fiducial ? L"Edit Fiducial" : L"Add Fiducial").subtitle(L"Mark: " + names + L"   \x00B7   size " + dia);
    f.custom("types", [&](Form& ff, float w) {
        const float gap = S(10), cw = (w - gap * 2) / 3, ch = S(68);
        // R10 §12 (specs/FIDUCIALS.md): Board = FiducialScope::Global, Circuit = Image, Component = Local.
        const char* lbl[] = {"Board", "Circuit", "Component"};
        const char* tips[] = {"Board fiducial \xE2\x80\x94 aligns the whole PCB or panel; measured once per board setup (exported once)",
                              "Circuit fiducial \xE2\x80\x94 belongs to each repeated circuit of a panel; every circuit is aligned on its own (exported once per circuit)",
                              "Component fiducial \xE2\x80\x94 local mark for fine alignment of one component (BGA, fine pitch); exported only with that component"};
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine(0, gap);
            ImGui::PushID(i);
            if (picto_card("t", lbl[i], st.type == i, true, cw, ch, [i](ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fg, ImU32 ac) {
                    float s = std::min(b.x - a.x, b.y - a.y);
                    ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                    picto_panel(dl, Box{ImVec2(c.x - s * 0.62f, c.y - s * 0.46f), ImVec2(c.x + s * 0.62f, c.y + s * 0.46f)}, fg, ac, i);
                })) st.type = i;
            gui::tooltip(tips[i]);
            ImGui::PopID();
        }
        ff.enable("owner", st.type == 2);
    });
    f.combo("owner", L"Linked component", owners, owner_sel).tip(L"Component fiducials belong to one component (BGA, fine-pitch parts).");
    int addnum = std::max(4, num);
    f.custom("roles", [&](Form&, float w) {
        const gui::GTheme& t = th();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Number");
        const char* lbl[] = {"1", "2", "3", "4+"};
        const char* tips[] = {"Fiducial 1 \xE2\x80\x94 first alignment mark", "Fiducial 2 \xE2\x80\x94 second alignment mark",
                              "Fiducial 3 \xE2\x80\x94 third alignment mark", "Additional fiducial: number 4 or higher (enter it below)"};
        const float bw = std::min(S(56), (w - S(170) - S(110)) / 4);
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine(0, S(6)); else ImGui::SameLine(S(170));
            ImGui::PushID(100 + i);
            if (gui::button(lbl[i], Icon::None, st.role == i, ImVec2(bw, 0))) st.role = i;
            gui::tooltip(tips[i]);
            ImGui::PopID();
        }
        if (st.role == 3) {   // 4+: the number, inline (no extra row)
            ImGui::SameLine(0, S(8));
            ImGui::SetNextItemWidth(S(90));
            ImGui::InputInt("##fidnum", &addnum, 1, 1);
            gui::tooltip("Additional fiducials use numbers 4 and higher");
        }
        (void)t;
    });
    f.custom("preview", [&](Form& ff, float w) {
        const gui::GTheme& t = th();
        const float ph = S(96);
        ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + ph);
        ImGui::Dummy(ImVec2(w, ph));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        panel_bg(dl, a, b);
        BBox bb = p.board_bbox();
        for (auto& c : p.comps) bb.add(c.pos);
        Fit fit(bb, a, b, S(14));
        dl->PushClipRect(a, b, true);
        draw_board(dl, p, fit);
        const float r = S(5);
        int n0 = st.role < 3 ? st.role + 1 : std::max(4, addnum);
        for (auto& c : p.comps) {
            bool mine = std::find(comps.begin(), comps.end(), &c) != comps.end();
            if (mine || c.feature != Feature::Fiducial) continue;
            ImVec2 s = fit(c.pos);
            fid_mark(dl, s, r, alpha(t.text_dim, 200), false);
            dl->AddText(ImVec2(s.x + r + S(3), s.y - S(16)), t.text_dim, std::to_string(c.fid_number).c_str());
        }
        if (st.type == 2) {
            if (const Component* oc = p.find_ref(ff.u8("owner"))) {
                ImVec2 s = fit(oc->pos);
                dl->AddRect(ImVec2(s.x - S(9), s.y - S(9)), ImVec2(s.x + S(9), s.y + S(9)), t.warn, S(3), 0, 1.5f);
                for (auto* c : comps) dl->AddLine(s, fit(c->pos), alpha(t.warn, 150), 1.f);
            }
        }
        for (size_t i = 0; i < comps.size(); ++i) {
            ImVec2 s = fit(comps[i]->pos);
            dl->AddCircleFilled(s, r * 2.4f, alpha(t.accent, 40));
            fid_mark(dl, s, r, t.accent);
            if (comps.size() > 12) continue;
            std::string lbl = "#" + std::to_string(n0 + (int)i);
            dl->AddText(gui::g_font_bold, ImGui::GetFontSize(), ImVec2(s.x + r + S(4), s.y - S(18)), t.accent, lbl.c_str());
        }
        dl->PopClipRect();
        dl->AddText(ImVec2(a.x + S(6), b.y - ImGui::GetFontSize() - S(4)), t.text_faint, "\xE2\x97\x8F this mark   \xE2\x97\x8B other fiducials");
    });
    if (comps.size() > 1) f.label(L"Multiple marks are numbered consecutively starting from the chosen number.");
    f.validate([&](Form& ff, std::wstring& err) {
        if (st.role == 3 && addnum <= 3) { err = L"Additional fiducials use numbers 4 and higher."; return false; }
        if (st.type == 2 && ff.index("owner") <= 0) { err = L"Choose the component this component fiducial belongs to."; return false; }
        return true;
    });
    if (!f.run(owner)) return false;
    const int t = st.type;
    const int n = st.role < 3 ? st.role + 1 : std::max(4, addnum);
    uint32_t owner_id = 0;
    if (t == 2) if (const Component* oc = p.find_ref(f.u8("owner"))) owner_id = oc->id;
    d.checkpoint(comps[0]->feature == Feature::Fiducial ? "Edit fiducial" : "Add fiducial");
    for (size_t i = 0; i < comps.size(); ++i) {
        Component& c = *comps[i];
        c.feature = Feature::Fiducial;
        c.fid_scope = t == 1 ? FiducialScope::Image : t == 2 ? FiducialScope::Local : FiducialScope::Global;
        c.fid_number = n + (int)i;
        c.local_fid_owner = (int)owner_id;
        c.dnp = false;
    }
    d.changed(CH_GEOMETRY);
    return true;
}

// ===========================================================================
// Panelization (#202) + Step And Repeat (#321)
// ===========================================================================
bool dlg_panelize(HWND owner, Doc& d) {
    Project& p = d.prj;
    const BBox bb = p.board_bbox();
    // R22 (REVIEW_V21 §1.1): reopening restores the saved parameters (OK saves without applying); else the array
    PanelSetup ps;
    const bool from_saved = panel_setup_from_params(p.array_params, ps);
    if (!from_saved) {
        ps = p.images.empty() ? PanelSetup{} : PanelSetup::from_images(p.images);
        ps.units = (Units)std::clamp(settings().units, 0, 2);
    }
    if (ps.cells.empty()) ps.reset();
    std::vector<std::wstring> refs{L""};
    {
        std::vector<std::string> r;
        for (auto& c : p.comps) if (!c.ref.empty() && c.feature == Feature::Component) r.push_back(c.ref);
        std::sort(r.begin(), r.end(), ref_less);
        for (auto& x : r) refs.push_back(W(x));
        Vec2 tmp;
        if (!r.empty() && (ps.ref_location.empty() || !panel_ref_pos(p, ps.ref_location, tmp))) ps.ref_location = r[0];
    }
    if (!from_saved && p.is_panel()) {   // R22: describe the current array, so Apply without edits reproduces it
        Vec2 rp;
        if (!panel_ref_pos(p, ps.ref_location, rp)) rp = bb.center();
        panel_backfill_offsets(ps, rp);
    }
    int sel_x = 0, sel_y = 0;
    for (auto& c : ps.cells) if (c.root) { sel_x = c.x; sel_y = c.y; }
    std::string msg;
    bool msg_err = false;
    const std::vector<std::wstring> unit_names{L"mm", L"inch", L"mil"};
    auto disp = [&](double mm) { return to_display(mm, ps.units); };

    // live preview: offsets are computed from the current entries every frame (the old "Panelize" step is part of Apply)
    auto computed = [&](std::string& err) {
        PanelSetup t = ps;
        Vec2 rp;
        if (!panel_ref_pos(p, t.ref_location, rp)) rp = bb.center();
        err.clear();
        panelize(t, rp, err);
        return t;
    };
    Form f(L"Create Nonlinear Array", 900);
    f.icon((int)Icon::Panelize).header(L"Create Nonlinear Array").subtitle(L"Yellow = reference circuit \x00B7 Red / Blue = circuits measured for the offsets \x00B7 instances are linked to the reference circuit");
    f.custom("grid", [&](Form& ff, float w) {
        (void)ff;
        const gui::GTheme& t = th();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float gw = std::floor(w * 0.52f), H = S(300);
        PanelMarkers mk = panel_markers(ps);
        // ---------------- grid
        ImGui::BeginGroup();
        const int ncols = std::max(ps.max_x() + 2, 5), nrows = std::max(ps.max_y() + 2, 4);
        const float hdr = S(22);
        const float cs = std::floor(std::min((gw - hdr) / ncols, (H - S(44) - hdr) / nrows));
        ImVec2 a = ImGui::GetCursorScreenPos();
        ImVec2 g0(a.x + hdr, a.y + hdr);
        ImGui::Dummy(ImVec2(gw, hdr + nrows * cs + S(4)));
        dl->AddRectFilled(ImVec2(g0.x, g0.y), ImVec2(g0.x + ncols * cs, g0.y + nrows * cs), t.field, S(4));
        for (int x = 0; x < ncols; ++x) {
            std::string s = std::to_string(x);
            ImVec2 ts = ImGui::CalcTextSize(s.c_str());
            dl->AddText(ImVec2(g0.x + x * cs + (cs - ts.x) * 0.5f, a.y + S(2)), t.text_faint, s.c_str());
        }
        for (int y = 0; y < nrows; ++y) {
            std::string s = std::to_string(y);
            ImVec2 ts = ImGui::CalcTextSize(s.c_str());
            float cy = g0.y + (nrows - 1 - y) * cs;
            dl->AddText(ImVec2(a.x + (hdr - ts.x) * 0.5f - S(2), cy + (cs - ts.y) * 0.5f), t.text_faint, s.c_str());
        }
        for (int x = 0; x < ncols; ++x)
            for (int y = 0; y < nrows; ++y) {
                ImVec2 c0(g0.x + x * cs, g0.y + (nrows - 1 - y) * cs), c1(c0.x + cs, c0.y + cs);
                ImGui::SetCursorScreenPos(c0);
                ImGui::PushID(x * 1000 + y);
                if (ImGui::InvisibleButton("c", ImVec2(cs, cs))) { sel_x = x; sel_y = y; }
                bool hov = ImGui::IsItemHovered();
                if (hov && ImGui::IsMouseDoubleClicked(0)) {
                    if (!ps.at(x, y)) { ps.set(x, y, 0); msg = "Circuit added at (" + std::to_string(x) + ", " + std::to_string(y) + ")"; msg_err = false; }
                }
                ImGui::PopID();
                const PanelCell* pc = ps.at(x, y);
                ImVec2 i0(c0.x + 2, c0.y + 2), i1(c1.x - 2, c1.y - 2);
                if (pc) {
                    ImU32 bg = pc->assigned ? (t.dark ? IM_COL32(88, 92, 100, 255) : IM_COL32(196, 200, 207, 255)) : t.panel2;
                    ImU32 ring = 0;
                    bool red = false, blue = false;
                    for (int g = 0; g < 4; ++g) {
                        red |= mk.red_x[g] == x && mk.red_y[g] == y;
                        blue |= mk.blue_x[g] == x && mk.blue_y[g] == y;
                    }
                    if (pc->root) bg = kYellow;
                    else if (red) ring = kRed;
                    else if (blue) ring = kBlue;
                    dl->AddRectFilled(i0, i1, bg, S(5));
                    if (ring) { dl->AddRectFilled(i0, i1, alpha(ring, 70), S(5)); dl->AddRect(i0, i1, ring, S(5), 0, 2.f); }
                    ImU32 fg = pc->root ? IM_COL32(40, 30, 0, 255) : t.text;
                    board_glyph(dl, ImVec2((c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f - cs * 0.08f), cs * 0.62f, pc->rot, fg, alpha(fg, 40));
                    std::string r = std::to_string(pc->rot) + "\xC2\xB0";
                    ImGui::SetWindowFontScale(0.78f);
                    ImVec2 ts = ImGui::CalcTextSize(r.c_str());
                    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2((c0.x + c1.x - ts.x) * 0.5f, c1.y - ts.y - S(3)), fg, r.c_str());
                    ImGui::SetWindowFontScale(1.f);
                    if (pc->x_out) { dl->AddLine(i0, i1, t.error, 2.f); dl->AddLine(ImVec2(i0.x, i1.y), ImVec2(i1.x, i0.y), t.error, 2.f); }
                    if (hov) {
                        std::string tip = "Circuit (" + std::to_string(x) + ", " + std::to_string(y) + ")  rotation " + std::to_string(pc->rot) +
                                          (pc->root ? "\nReference circuit" : "") + (red ? "\nRed reference" : "") + (blue ? "\nBlue reference" : "") +
                                          (pc->assigned ? "\nOffset " + gui::fmt_len_u8(pc->offset.x) + ", " + gui::fmt_len_u8(pc->offset.y) : "\nNot panelized yet");
                        ImGui::SetTooltip("%s", tip.c_str());
                    }
                } else if (hov) {
                    dl->AddRect(i0, i1, alpha(t.accent, 120), S(5), 0, 1.f);
                    ImGui::SetTooltip("Empty cell (%d, %d)\nDouble-click or use a rotation button to add an circuit", x, y);
                }
                dl->AddRect(c0, c1, alpha(t.border, 160), 0, 0, 1.f);
                if (x == sel_x && y == sel_y) dl->AddRect(ImVec2(c0.x + 1, c0.y + 1), ImVec2(c1.x - 1, c1.y - 1), t.accent, S(5), 0, 2.5f);
            }
        ImGui::SetCursorScreenPos(ImVec2(a.x, g0.y + nrows * cs + S(8)));
        // grid buttons (Reset / Delete / Assign Root ... 0 90 180 270)
        if (gui::button("Reset", Icon::Undo)) { ps.reset(); sel_x = sel_y = 0; msg = "Panel reset to the reference circuit"; msg_err = false; }
        ImGui::SameLine(0, S(6));
        if (gui::button("Delete", Icon::Delete, false, ImVec2(0, 0), ps.at(sel_x, sel_y) && !ps.at(sel_x, sel_y)->root)) { ps.remove(sel_x, sel_y); }
        ImGui::SameLine(0, S(6));
        if (gui::button("Assign Root", Icon::Home, false, ImVec2(0, 0), ps.at(sel_x, sel_y) != nullptr)) { ps.assign_root(sel_x, sel_y); msg = "Root moved; press Panelize to recompute offsets"; msg_err = false; }
        ImGui::SameLine(0, S(6));
        {
            PanelCell* pc = ps.at(sel_x, sel_y);
            bool xo = pc && pc->x_out;
            if (gui::icon_button("xout", Icon::Unassign, "X-out: mark the selected circuit as a bad board (skipped when placing)", xo, pc != nullptr, ImGui::GetFrameHeight() + S(2)) && pc) pc->x_out = !pc->x_out;
        }
        const int rots[4] = {0, 90, 180, 270};
        const Icon ric[4] = {Icon::None, Icon::Rot90, Icon::Rot180, Icon::Rot270};
        float rx = a.x + gw - 4 * (ImGui::GetFrameHeight() + S(6)) - S(2);
        for (int i = 0; i < 4; ++i) {
            ImGui::SameLine(0, S(4));
            if (i == 0) ImGui::SetCursorScreenPos(ImVec2(std::max(rx, ImGui::GetCursorScreenPos().x), ImGui::GetCursorScreenPos().y));
            ImGui::PushID(i + 50);
            const PanelCell* pc = ps.at(sel_x, sel_y);
            bool on = pc && pc->rot == rots[i];
            std::string tip = "Set the selected cell to " + std::to_string(rots[i]) + "\xC2\xB0 (adds an circuit when empty)";
            bool clicked;
            if (i == 0) clicked = gui::button("0", Icon::None, on, ImVec2(ImGui::GetFrameHeight() + S(2), 0), !(pc && pc->root));
            else clicked = gui::icon_button("r", ric[i], tip.c_str(), on, !(pc && pc->root), ImGui::GetFrameHeight() + S(2));
            if (i == 0) gui::tooltip(tip.c_str());
            if (clicked) ps.set(sel_x, sel_y, rots[i]);
            ImGui::PopID();
        }
        ImGui::EndGroup();
        // ---------------- live preview
        ImGui::SameLine(0, S(14));
        ImGui::BeginGroup();
        ImVec2 pa = ImGui::GetCursorScreenPos(), pb(pa.x + (w - gw - S(14)), pa.y + H);
        ImGui::Dummy(ImVec2(pb.x - pa.x, H));
        panel_bg(dl, pa, pb);
        std::string perr;
        const PanelSetup pv = computed(perr);
        std::vector<PanelImage> imgs = panel_images(pv);
        BBox all;
        for (auto& im : imgs) {
            const PanelCell* pc = pv.at(im.x_index, im.y_index);
            if (pc && !pc->assigned) continue;
            Xform x = Xform::translate(im.offset.x, im.offset.y) * Xform::rotation(im.rot);
            all.add(x.apply({bb.x0, bb.y0})); all.add(x.apply({bb.x1, bb.y0})); all.add(x.apply({bb.x1, bb.y1})); all.add(x.apply({bb.x0, bb.y1}));
        }
        Fit fit(all, pa, pb, S(16));
        dl->PushClipRect(pa, pb, true);
        int unassigned = 0;
        for (auto& im : imgs) {
            const PanelCell* pc = pv.at(im.x_index, im.y_index);
            if (pc && !pc->assigned) { ++unassigned; continue; }
            Xform x = Xform::translate(im.offset.x, im.offset.y) * Xform::rotation(im.rot);
            bool sel = im.x_index == sel_x && im.y_index == sel_y;
            draw_board(dl, p, fit, x, im.root ? alpha(kYellow, 70) : sel ? alpha(t.accent, 60) : 0, true);
            Vec2 c = x.apply(bb.center());
            std::string lbl = im.name + (im.rot ? "  " + std::to_string((int)im.rot) + "\xC2\xB0" : "");
            ImVec2 s = fit(c), ts = ImGui::CalcTextSize(lbl.c_str());
            dl->AddText(ImVec2(s.x - ts.x * 0.5f, s.y - ts.y * 0.5f), sel ? t.accent : t.text, lbl.c_str());
            if (im.x_out) { ImVec2 q0 = fit(x.apply({bb.x0, bb.y0})), q1 = fit(x.apply({bb.x1, bb.y1})); dl->AddLine(q0, q1, t.error, 2.f); }
            if (!ps.ref_location.empty() && im.root) {
                Vec2 rp;
                if (panel_ref_pos(p, ps.ref_location, rp)) gui::icon_draw(dl, Icon::VirtualOrigin, ImVec2(fit(rp).x - S(7), fit(rp).y - S(7)), S(14));
            }
        }
        dl->PopClipRect();
        std::string info = std::to_string(imgs.size()) + " circuit(s)";
        if (unassigned) info += " \xC2\xB7 " + std::to_string(unassigned) + " without offsets";
        if (!all.empty()) info += " \xC2\xB7 circuits span " + gui::fmt_len_u8(all.w()) + " \xC3\x97 " + gui::fmt_len_u8(all.h());   // not a board outline
        dl->AddText(ImVec2(pa.x + S(10), pb.y - ImGui::GetFontSize() - S(6)), t.text_dim, info.c_str());
        ImGui::EndGroup();
    });
    f.heading(L"Circuit creation");
    f.custom("groups", [&](Form& ff, float w) {
        const gui::GTheme& t = th();
        PanelMarkers mk = panel_markers(ps);
        const float lw = S(170), fw = std::floor((w - lw - S(40)) / 4) - S(6);
        // column headers
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto chip = [&](ImVec2 at, ImU32 c) { dl->AddRectFilled(at, ImVec2(at.x + S(12), at.y + S(12)), c, S(3)); };
        float x0 = p0.x + lw + S(28);
        chip(ImVec2(x0, p0.y + S(3)), kYellow); dl->AddText(ImVec2(x0 + S(16), p0.y), t.text_dim, "\xE2\x86\x92");
        chip(ImVec2(x0 + S(32), p0.y + S(3)), kRed);
        dl->AddText(ImVec2(x0 + S(50), p0.y), t.text_dim, "Yellow to Red   X / Y");
        float x1 = x0 + 2 * (fw + S(6));
        chip(ImVec2(x1, p0.y + S(3)), kYellow); dl->AddText(ImVec2(x1 + S(16), p0.y), t.text_dim, "\xE2\x86\x92");
        chip(ImVec2(x1 + S(32), p0.y + S(3)), kBlue);
        dl->AddText(ImVec2(x1 + S(50), p0.y), t.text_dim, "Yellow to Blue   X / Y");
        ImGui::Dummy(ImVec2(w, S(18)));
        for (int g = 0; g < 4; ++g) {
            ImGui::PushID(g + 300);
            bool en = panel_group_enabled(ps, g);
            ImGui::BeginDisabled(!en);
            ImVec2 rp = ImGui::GetCursorScreenPos();
            board_glyph(dl, ImVec2(rp.x + S(11), rp.y + ImGui::GetFrameHeight() * 0.5f), S(20), g * 90, en ? t.text : t.text_faint, alpha(t.accent, en ? 50 : 15));
            ImGui::SetCursorScreenPos(ImVec2(rp.x + S(28), rp.y));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d\xC2\xB0 circuits", g * 90);
            RotGroupOffsets& o = ps.groups[g];
            auto fld = [&](const char* id, double& v, bool on, float x) {
                ImGui::SameLine(0, 0);
                ImGui::SetCursorScreenPos(ImVec2(p0.x + lw + S(28) + x, rp.y));
                ImGui::SetNextItemWidth(fw);
                ImGui::BeginDisabled(!on);
                double dv = v;
                if (ImGui::InputDouble(id, &dv, 0, 0, "%.4f")) v = dv;
                ImGui::EndDisabled();
            };
            fld("##rx", o.red_x, true, 0);
            fld("##ry", o.red_y, g != 0, fw + S(6));
            fld("##bx", o.blue_x, g != 0, 2 * (fw + S(6)));
            fld("##by", o.blue_y, true, 3 * (fw + S(6)));
            ImGui::EndDisabled();
            if (en) {
                std::string tip;
                if (mk.red_x[g] >= 0) tip += "Red (" + std::to_string(mk.red_x[g]) + ", " + std::to_string(mk.red_y[g]) + ")  ";
                if (mk.blue_x[g] >= 0) tip += "Blue (" + std::to_string(mk.blue_x[g]) + ", " + std::to_string(mk.blue_y[g]) + ")";
                if (!tip.empty() && ImGui::IsMouseHoveringRect(rp, ImVec2(rp.x + lw, rp.y + ImGui::GetFrameHeight()))) ImGui::SetTooltip("%s", tip.c_str());
            }
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, S(4)));
        // units / reference location (R22: the "Panelize" button is the footer Apply)
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "Offset units");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(90));
        if (ImGui::BeginCombo("##units", U(unit_names[(int)ps.units]).c_str())) {
            for (int i = 0; i < 3; ++i) if (ImGui::Selectable(U(unit_names[i]).c_str(), (int)ps.units == i)) {
                Units old = ps.units, nu = (Units)i;
                for (auto& g : ps.groups) {
                    g.red_x = to_display(from_display(g.red_x, old), nu); g.red_y = to_display(from_display(g.red_y, old), nu);
                    g.blue_x = to_display(from_display(g.blue_x, old), nu); g.blue_y = to_display(from_display(g.blue_y, old), nu);
                }
                ps.units = nu;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, S(16));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "Reference location");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(140));
        if (ImGui::BeginCombo("##refloc", ps.ref_location.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (auto& r : refs) if (!r.empty() && ImGui::Selectable(U(r).c_str(), U(r) == ps.ref_location)) ps.ref_location = U(r);
            ImGui::EndCombo();
        }
        gui::tooltip("Component whose centroid is measured between circuits (needed for rotated circuits)");
        // requirements / status. R22 (REVIEW_V21 §1.1): a FIXED-height scroll area, so more rotation groups never
        // grow the dialog; the full text is also the tooltip.
        auto req = panel_requirements(ps);
        std::string perr;
        (void)computed(perr);
        const std::string line = !msg.empty() ? msg : !perr.empty() ? "Not ready: " + perr : "Ready: Apply creates the array";
        ImGui::PushStyleColor(ImGuiCol_Text, (!msg.empty() ? msg_err : !perr.empty()) ? t.error : t.ok);
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
        const float lh = ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("##req", ImVec2(w, lh * 2.6f), ImGuiChildFlags_None, ImGuiWindowFlags_None);
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_faint);
        std::string all;
        for (auto& r : req) { ImGui::BulletText("%s", r.c_str()); all += r + "\n"; }
        ImGui::PopStyleColor();
        ImGui::EndChild();
        if (!all.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", all.c_str());
        (void)disp;
    });
    // R22 footer: [Apply] [OK] [Cancel]. Apply commits the array (one undo step, dialog stays open); OK saves the
    // parameters in the project and closes WITHOUT applying; Cancel discards unapplied changes. No outline is made.
    int applied = 0;
    f.extras_right();
    f.extra_button(L"Apply", [&](Form&) {
        std::string err;
        PanelSetup t = computed(err);
        if (!err.empty()) { msg = err; msg_err = true; return false; }
        std::vector<ArrayInstance> inst;
        for (auto& im : panel_images(t)) if (!im.root) inst.push_back({im.offset, im.rot, im.x_out, im.x_index, im.y_index});
        d.checkpoint("Create nonlinear array");
        if (inst.empty()) delete_array(p);
        else if (!create_nonlinear_array(p, inst, &err)) { msg = err; msg_err = true; return false; }
        for (auto& c : t.cells) if (c.root && !p.images.empty()) { p.images[0].x_index = c.x; p.images[0].y_index = c.y; p.images[0].x_out = c.x_out; }
        panel_setup_to_params(ps, p.array_params);
        d.all_images = true;
        d.changed(CH_GEOMETRY);
        ++applied;
        msg = "Applied: " + std::to_string(p.images.size()) + " circuit(s), linked to the reference circuit (Ctrl+Z undoes)";
        msg_err = false;
        return false;
    });
    const bool ok = f.run(owner);
    if (ok) {   // OK = save the parameters, no geometry change
        panel_setup_to_params(ps, p.array_params);
        p.dirty = true;
        set_status(applied ? L"Array parameters saved" : L"Array parameters saved (not applied)");
    }
    if (applied) set_status(L"Array has " + std::to_wstring(p.images.size()) + L" circuit(s)");
    return applied > 0;
}


// v17: flattened panel -> circuit instances (REVIEW_V16 §14). Detects N repeated circuits, previews
// their count and pitch, folds on confirm. One undo step.
bool dlg_detect_repeated(HWND owner, Doc& d) {
    Project& p = d.prj;
    InstanceFold fold;
    if (!detect_repeated_instances(p, fold)) {
        ui::message(owner, L"Detect Repeated Circuits", W("No repeated circuits found: " + fold.why + "."));
        return false;
    }
    const Units u = (Units)std::clamp(settings().units, 0, 2);
    const char* ul = u == Units::MM ? "mm" : u == Units::Inch ? "inch" : "mil";
    char b[200];
    std::string txt;
    const int per = fold.members.empty() ? 0 : (int)fold.members[0].size();
    std::snprintf(b, sizeof b, "%d circuits of %d placements each (worst deviation %.3f mm).\n", fold.n, per, fold.max_dev);
    txt += b;
    for (size_t k = 0; k < fold.images.size() && k < 12; ++k) {
        const PanelImage& im = fold.images[k];
        if (k == 0) { txt += "Circuit 1: reference (kept as imported)\n"; continue; }
        const PanelImage& pr = fold.images[k - 1];
        std::snprintf(b, sizeof b, "Circuit %d: offset %.4f, %.4f %s, rotation %.1f\xC2\xB0, pitch from %d: %.4f, %.4f %s\n", (int)k + 1,
                      to_display(im.offset.x, u), to_display(im.offset.y, u), ul, im.rot, (int)k,
                      to_display(im.offset.x - pr.offset.x, u), to_display(im.offset.y - pr.offset.y, u), ul);
        txt += b;
    }
    if (fold.images.size() > 12) txt += "...\n";
    std::snprintf(b, sizeof b, "Apply keeps circuit 1's %d placements and turns the other %d into circuit instances (Ctrl+Z undoes).", per, per * (fold.n - 1));
    txt += b;
    Form f(L"Detect Repeated Circuits", 520);
    f.icon((int)Icon::AllImages).header(L"Detect Repeated Circuits").subtitle(L"Every reference occurs once per circuit: fold the copies into circuit instances");
    f.label(W(txt));
    if (!f.run(owner)) return false;
    d.checkpoint("Detect Repeated Circuits");
    const int removed = fold_repeated_instances(p, fold);
    d.all_images = true;
    d.changed(CH_ALL);
    set_status(L"Repeated circuits: " + std::to_wstring(fold.n) + L" circuits, " + std::to_wstring(removed) + L" placements folded into instances");
    return true;
}

// R22 Create Linear Array (REVIEW_V21 §1.1, specs/PANELIZE.md §2): nx x ny unrotated instances of the reference
// circuit. Counts, pitch and the reference cell ("origin"); a units dropdown (mm / in / mil) converts the typed
// pitch to mm internally. Footer [Apply] [OK] [Cancel]: Apply commits (one undo step, stays open), OK saves the
// parameters without applying, Cancel discards. Never creates an outline. prefill = values from the Gerber pitch tool.
static bool linear_array_form(HWND owner, Doc& d, bool prefill, int nx, int ny, double dx, double dy, int rc, int rr) {
    Project& p = d.prj;
    auto& ap = p.array_params;
    const BBox bb = p.has_circuit_frame() ? p.circuit_frame_bbox() : p.board_bbox();
    Units u = (Units)std::clamp(settings().units, 0, 2);
    if (!prefill && ap.lin_saved) { nx = ap.lin_nx; ny = ap.lin_ny; dx = ap.lin_pitch_x; dy = ap.lin_pitch_y; rc = ap.lin_root_col; rr = ap.lin_root_row; u = ap.lin_units; }
    else if (!prefill && p.is_panel()) {   // describe the current array when it is a regular grid
        int mx = 0, my = 0;
        for (auto& im : p.images) { mx = std::max(mx, im.x_index); my = std::max(my, im.y_index); }
        nx = mx + 1; ny = my + 1; rc = p.images[0].x_index; rr = p.images[0].y_index;
        for (auto& im : p.images) {
            if (im.x_index != rc && dx == 0) dx = (im.offset.x) / (im.x_index - rc);
            if (im.y_index != rr && dy == 0) dy = (im.offset.y) / (im.y_index - rr);
        }
    }
    if (dx == 0) dx = bb.empty() ? 50 : bb.w() + 2.0;
    if (dy == 0) dy = bb.empty() ? 50 : bb.h() + 2.0;
    static const std::vector<std::wstring> unames{L"mm", L"in", L"mil"};
    auto fmtu = [](double mm, Units uu) { return fmt_num(mm_to_units(mm, uu), uu == Units::MM ? 4 : uu == Units::Inch ? 5 : 2); };
    auto rdu = [](const Form& ff, const char* k, Units uu, double def_mm) {
        const std::wstring t = ff.str(k);
        wchar_t* e = nullptr;
        const double v = std::wcstod(t.c_str(), &e);
        return e == t.c_str() ? def_mm : units_to_mm(v, uu);
    };
    Units cur = u;
    std::string msg;
    bool msg_err = false;
    int applied = 0;
    Form f(L"Create Linear Array", 520);
    f.icon((int)Icon::AllImages).header(L"Create Linear Array").subtitle(L"Regular grid of instances linked to the reference circuit");
    f.combo("units", L"Units", unames, unames[(int)u]).wide().on_change([&](Form& ff, const std::string&) {
        const Units nu = (Units)std::clamp(ff.index("units"), 0, 2);
        if (nu == cur) return;
        ff.set("dx", fmtu(rdu(ff, "dx", cur, 0), nu));
        ff.set("dy", fmtu(rdu(ff, "dy", cur, 0), nu));
        cur = nu;
    });
    f.two_columns();
    f.number("nx", L"Circuits along X", nx, 0);
    f.number("ny", L"Circuits along Y", ny, 0);
    f.text("dx", L"X pitch (horizontal)", fmtu(dx, u));
    f.text("dy", L"Y pitch (vertical)", fmtu(dy, u));
    f.number("rc", L"Reference column", rc + 1, 0).tip(L"Grid column of the reference circuit, 1 = left");
    f.number("rr", L"Reference row", rr + 1, 0).tip(L"Grid row of the reference circuit, 1 = bottom");
    f.two_columns(false);
    f.custom("st", [&](Form& ff, float) {
        const gui::GTheme& t = th();
        const int a = iround(ff.num("nx", 1)), b = iround(ff.num("ny", 1));
        char buf[200];
        std::snprintf(buf, sizeof buf, "%d circuits, pitch %.4f x %.4f mm (negative = left / down)", std::max(0, a * b),
                      rdu(ff, "dx", cur, 0), rdu(ff, "dy", cur, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, msg.empty() ? t.text_dim : msg_err ? t.error : t.ok);
        ImGui::TextUnformatted(msg.empty() ? buf : msg.c_str());
        ImGui::PopStyleColor();
    }, 22).wide();
    auto save = [&](Form& ff) {
        ap.lin_saved = true;
        ap.lin_nx = std::max(1, iround(ff.num("nx", 1))); ap.lin_ny = std::max(1, iround(ff.num("ny", 1)));
        ap.lin_pitch_x = rdu(ff, "dx", cur, 0); ap.lin_pitch_y = rdu(ff, "dy", cur, 0);
        ap.lin_root_col = std::max(0, iround(ff.num("rc", 1)) - 1); ap.lin_root_row = std::max(0, iround(ff.num("rr", 1)) - 1);
        ap.lin_units = cur;
        p.dirty = true;
    };
    f.extras_right();
    f.extra_button(L"Apply", [&](Form& ff) {
        std::string err;
        Project tmp; tmp.images = p.images;
        const int a = iround(ff.num("nx", 1)), b = iround(ff.num("ny", 1));
        if (!create_linear_array(tmp, a, b, rdu(ff, "dx", cur, 0), rdu(ff, "dy", cur, 0), iround(ff.num("rc", 1)) - 1, iround(ff.num("rr", 1)) - 1, &err)) {
            msg = "Not applied: " + err; msg_err = true; return false;
        }
        d.checkpoint("Create linear array");
        create_linear_array(p, a, b, rdu(ff, "dx", cur, 0), rdu(ff, "dy", cur, 0), iround(ff.num("rc", 1)) - 1, iround(ff.num("rr", 1)) - 1);
        save(ff);
        d.all_images = true;
        d.changed(CH_GEOMETRY);
        ++applied;
        std::string issues;
        for (auto& fi : check_outlines(p)) if (fi.error) issues += (issues.empty() ? "" : "; ") + fi.msg;
        msg = "Applied: " + std::to_string(p.images.size()) + " circuits" + (issues.empty() ? std::string(" (Ctrl+Z undoes)") : " \xE2\x80\x94 check: " + issues);
        msg_err = !issues.empty();
        return false;
    });
    if (f.run(owner)) { save(f); set_status(applied ? L"Linear array parameters saved" : L"Linear array parameters saved (not applied)"); }
    if (applied) set_status(L"Linear array: " + std::to_wstring(p.images.size()) + L" circuits");
    return applied > 0;
}
bool dlg_linear_array(HWND owner, Doc& d) { return linear_array_form(owner, d, false, 2, 1, 0, 0, 0, 0); }
// R14 entry kept for the Gerber pitch tool (pre-filled); same dialog
bool dlg_step_repeat(HWND owner, Doc& d, int nx, int ny, double dx, double dy, int rc, int rr) { return linear_array_form(owner, d, true, nx, ny, dx, dy, rc, rr); }

// ===========================================================================
// Text import wizard shared by PNP (#508 ASCII Import), Reference Fusion and
// BOM (#166). Four steps (Excel Text Import Wizard style), every step reachable
// directly from the step pills:
//   1 Source   encoding, Delimited / Fixed width, start / header / stop lines,
//              comment prefixes, raw text with line numbers (+ visible whitespace)
//   2 Split    delimiter set / qualifier / consecutive-as-one, or a fixed-width
//              ruler (click = add cut, drag = move, double/right-click = remove)
//   3 Columns  role + data format + unit / negate per column, number format,
//              purpose options (PNP units & sides & mode / BOM link & DNP)
//   4 Review   parsed result + issues
// ===========================================================================
namespace {

std::wstring templates_path() { return exe_dir() + L"\\PCBPlotter.templates.json"; }

ImVec4 C4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

struct ImportWizard {
    std::string bytes;
    io::Purpose purpose = io::Purpose::Pnp;
    io::ImportLayout* layout = nullptr;
    io::ParsedText* text = nullptr;
    std::function<void()> reanalyze;          // structure changed
    std::function<void()> rebuild;            // roles / options changed
    std::function<void(float)> options;       // purpose options (Columns step)
    std::function<void(float, float)> review; // purpose results (Review step)
    std::function<void(float, float)> source_override;   // BOM: explicit row model replaces the generic Source step
    std::function<bool()> mapping_ok;
    std::function<int()> result_count;
    std::function<int()> issue_count;
    const char* mapping_hint = "";
    std::vector<io::FormatTemplate> templates;
    std::string tpl_status;
    int step = 0;
    bool show_ws = false;
    int drag_cut = -1;                        // index into fixed_cuts while dragging
    bool drag_moved = false;
    int pending_add = -1;
    char other_buf[24] = {};
    char comment_buf[64] = {};

    void sync_buffers() {
        std::snprintf(other_buf, sizeof other_buf, "%s", layout->other_delim.c_str());
        std::string c;
        for (auto& p : layout->comment_prefixes) { if (!c.empty()) c += ' '; c += p; }
        std::snprintf(comment_buf, sizeof comment_buf, "%s", c.c_str());
    }
    io::SplitMode eff_mode() const {
        if (layout->mode != io::SplitMode::Auto) return layout->mode;
        return text ? text->mode : io::SplitMode::Delimited;
    }
    // lines inside the table window (for cut suggestions)
    std::vector<std::string> window_lines() const {
        std::vector<std::string> v;
        int a = std::max(0, layout->skip_lines), b = (int)text->lines.size();
        if (layout->stop_line >= 0) b = std::min(b, layout->stop_line);
        for (int i = a; i < b && (int)v.size() < 2000; ++i) {
            bool cm = false;
            std::string s = text->lines[i];
            size_t k = s.find_first_not_of(" \t");
            if (k != std::string::npos)
                for (auto& p : layout->comment_prefixes) if (!p.empty() && s.compare(k, p.size(), p) == 0) cm = true;
            if (!cm) v.push_back(s);
        }
        return v;
    }
    void set_mode(io::SplitMode m) {
        io::ImportLayout& L = *layout;
        if (m == io::SplitMode::Delimited && L.mode != m) {
            if (L.delims.empty() && L.other_delim.empty()) {
                if (text && text->mode == io::SplitMode::Delimited) {
                    L.delims = text->delims; L.other_delim = text->other_delim; L.consecutive_as_one = text->consecutive_as_one;
                } else L.delims = ",";
            }
        }
        if (m == io::SplitMode::Fixed && L.mode != m) {
            if (L.fixed_cuts.empty()) L.fixed_cuts = text && text->mode == io::SplitMode::Fixed ? text->cuts : io::suggest_fixed_cuts(window_lines(), L.tab_size);
            if (L.fixed_cuts.empty()) L.fixed_cuts = {0};
        }
        L.mode = m;
        L.sep = io::Sep::Auto;
        sync_buffers();
        reanalyze();
    }
};

ImU32 role_color(io::Role r) {
    const gui::GTheme& t = th();
    switch (r) {
    case io::Role::Unused: return t.text_faint;
    case io::Role::Ref: return t.accent;
    case io::Role::X: case io::Role::Y: case io::Role::Rotation: case io::Role::Side: return t.ok;
    case io::Role::PartNumber: case io::Role::Package: return t.warn;
    default: return t.text_dim;
    }
}

void dim_label(const char* s) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(C4(th().text_dim), "%s", s);
    ImGui::SameLine();
}
void faint(const char* fmt, const std::string& s) {
    ImGui::SameLine(0, S(4));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(C4(th().text_faint), fmt, s.c_str());
}

// --------------------------------------------------------------------------
// step pills + templates bar
// --------------------------------------------------------------------------
void draw_steps(ImportWizard& ui, float w) {
    const gui::GTheme& t = th();
    static const char* names[] = {"Source", "Split", "Columns", "Review"};
    static const char* subs[] = {"file & rows", "delimiters / widths", "roles & formats", "result & issues"};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pw = std::min(S(176), (w - S(360)) / 4.f), ph = S(40);
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine(0, S(6));
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##pill", ImVec2(pw, ph))) ui.step = i;
        bool hov = ImGui::IsItemHovered(), cur = ui.step == i;
        ImU32 bg = cur ? t.accent : hov ? t.field_hover : t.panel2;
        dl->AddRectFilled(p, ImVec2(p.x + pw, p.y + ph), bg, S(9));
        // number bubble
        ImVec2 c(p.x + S(18), p.y + ph * 0.5f);
        bool warn = (i == 2 && ui.mapping_ok && !ui.mapping_ok()) || (i == 3 && ui.issue_count && ui.issue_count() > 0);
        dl->AddCircleFilled(c, S(11), cur ? alpha(t.accent_text, 60) : warn ? alpha(t.warn, 90) : alpha(t.text_faint, 60));
        char n[4] = {char('1' + i), 0};
        ImVec2 ns = ImGui::CalcTextSize(n);
        dl->AddText(ImVec2(c.x - ns.x * 0.5f, c.y - ns.y * 0.5f), cur ? t.accent_text : t.text, n);
        ImGui::PushFont(gui::g_font_bold);
        dl->AddText(ImVec2(p.x + S(36), p.y + S(4)), cur ? t.accent_text : t.text, names[i]);
        ImGui::PopFont();
        dl->AddText(ImVec2(p.x + S(36), p.y + S(21)), cur ? alpha(t.accent_text, 200) : t.text_faint, subs[i]);
        ImGui::PopID();
    }
}

void draw_templates(ImportWizard& ui, Form& f, HWND owner) {
    if (gui::button("Save template", Icon::Save)) {
        f.later([&ui, owner] {
            std::wstring name = L"My " + std::wstring(ui.purpose == io::Purpose::Bom ? L"BOM" : L"PNP") + L" format";
            if (!prompt_text(owner, L"Save Template", L"Template name", name) || name.empty()) return;
            io::FormatTemplate tp = io::make_template(U(name), ui.purpose, *ui.text, *ui.layout);
            bool rep = false;
            for (auto& x : ui.templates) if (x.name == tp.name && x.purpose == tp.purpose) { x = tp; rep = true; }
            if (!rep) ui.templates.push_back(tp);
            std::string err;
            if (io::save_templates(U(templates_path()), ui.templates, err)) ui.tpl_status = "Template \"" + tp.name + "\" saved";
            else warning(owner, L"Save Template", L"Could not save the template file.\n" + W(err));
        });
    }
    ImGui::SameLine(0, S(6));
    std::vector<int> mine;
    for (int i = 0; i < (int)ui.templates.size(); ++i) if (ui.templates[i].purpose == ui.purpose) mine.push_back(i);
    ImGui::SetNextItemWidth(S(170));
    if (ImGui::BeginCombo("##tpl", mine.empty() ? "No saved templates" : "Load template...")) {
        for (int i : mine) if (ImGui::Selectable(ui.templates[i].name.c_str())) {
            *ui.layout = io::template_layout(ui.templates[i]);
            ui.sync_buffers();
            ui.reanalyze();
            io::apply_template_roles(ui.templates[i], *ui.text, *ui.layout);
            ui.rebuild();
            ui.tpl_status = "Template \"" + ui.templates[i].name + "\" applied";
        }
        ImGui::EndCombo();
    }
}

void auto_match_template(ImportWizard& ui) {
    std::string err;
    io::load_templates(U(templates_path()), ui.templates, err);
    if (ui.purpose == io::Purpose::Bom && io::ensure_builtin_bom_templates(ui.templates))   // R14d: built-in "M1 BOM" profile, saved so it is user-editable
        io::save_templates(U(templates_path()), ui.templates, err);
    double score = 0;
    int i = io::match_template(ui.templates, *ui.text, ui.purpose, 0.75, &score);
    if (i < 0) return;
    *ui.layout = io::template_layout(ui.templates[i]);
    ui.sync_buffers();
    ui.reanalyze();
    io::apply_template_roles(ui.templates[i], *ui.text, *ui.layout);
    ui.rebuild();
    ui.tpl_status = "Template \"" + ui.templates[i].name + "\" matched (" + std::to_string((int)std::lround(score * 100)) + "%)";
    if (ui.purpose == io::Purpose::Bom && io::is_m1_bom(*ui.text)) ui.tpl_status = "Detected: M1 BOM (profile \"" + ui.templates[i].name + "\")";
}

// --------------------------------------------------------------------------
// shared controls
// --------------------------------------------------------------------------
void mode_radios(ImportWizard& ui) {
    io::ImportLayout& L = *ui.layout;
    dim_label("Original data type");
    int m = (int)L.mode;
    if (ImGui::RadioButton("Auto detect", m == 0)) ui.set_mode(io::SplitMode::Auto);
    ImGui::SameLine();
    if (ImGui::RadioButton("Delimited", m == 1)) ui.set_mode(io::SplitMode::Delimited);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Characters such as commas, semicolons or tabs separate each field.");
    ImGui::SameLine();
    if (ImGui::RadioButton("Fixed width", m == 2)) ui.set_mode(io::SplitMode::Fixed);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fields are aligned in columns with spaces between each field.");
    if (m == 0 && ui.text) {
        std::string d = ui.text->mode == io::SplitMode::Fixed ? "fixed width, " + std::to_string(ui.text->cuts.size()) + " columns"
                                                              : std::string("delimited: ") + io::sep_name(ui.text->sep);
        faint("(detected %s)", d);
    }
}

std::string visible_ws(const std::string& s, int tab) {
    std::string o;
    int col = 0;
    for (char c : s) {
        if (c == '\t') {
            int n = tab - col % tab;
            o += "\xE2\x86\x92";
            for (int k = 1; k < n; ++k) o += ' ';
            col += n;
        } else if (c == ' ') { o += "\xC2\xB7"; ++col; }
        else { o += c; if (((unsigned char)c & 0xC0) != 0x80) ++col; }
    }
    return o;
}

// raw lines with line numbers; click = start row, context menu = header / stop
void draw_raw_lines(ImportWizard& ui, float w, float h) {
    const gui::GTheme& t = th();
    io::ImportLayout& L = *ui.layout;
    const io::ParsedText& T = *ui.text;
    const int n = (int)T.lines.size();
    std::vector<int8_t> kind(n, -1);
    for (auto& r : T.rows)
        for (int k = r.line; k < r.line + r.line_count && k < n; ++k) kind[k] = (int8_t)r.kind;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.field);
    if (ImGui::BeginChild("##rawlines", ImVec2(w, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::PushFont(gui::g_font_mono);
        const float lh = ImGui::GetTextLineHeightWithSpacing();
        const float gut = ImGui::CalcTextSize("00000").x + S(10);
        const float tagw = S(84);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiListClipper clip;
        clip.Begin(n, lh);
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                ImGui::PushID(i);
                ImVec2 p = ImGui::GetCursorScreenPos();
                bool before = i < L.skip_lines, after = T.stop_line >= 0 && i >= T.stop_line;
                std::string s = ui.show_ws ? visible_ws(T.lines[i], L.tab_size) : T.lines[i];
                float tw = ImGui::CalcTextSize(s.c_str()).x;
                ImGui::Selectable("##ln", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(std::max(w, gut + tagw + tw + S(20)), lh - ImGui::GetStyle().ItemSpacing.y));
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) { L.skip_lines = i; ui.reanalyze(); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click: start import at line %d\nRight-click: header / stop options", i + 1);
                if (ImGui::BeginPopupContextItem("##lnctx")) {
                    ImGui::PushFont(gui::g_font);
                    if (ImGui::MenuItem("Start import at this line")) { L.skip_lines = i; ui.reanalyze(); }
                    if (ImGui::MenuItem("Header row is this line")) { L.header_line = i; if (L.skip_lines > i) L.skip_lines = i; ui.reanalyze(); }
                    if (ImGui::MenuItem("No header row")) { L.header_line = -2; ui.reanalyze(); }
                    if (ImGui::MenuItem("Stop before this line")) { L.stop_line = i; ui.reanalyze(); }
                    if (L.stop_line >= 0 && ImGui::MenuItem("Read to end of file")) { L.stop_line = -1; ui.reanalyze(); }
                    ImGui::PopFont();
                    ImGui::EndPopup();
                }
                int k = kind[i];
                if (i >= T.header_line && i < T.header_line + std::max(1, T.header_rows) && T.header_line >= 0) k = (int)io::RowKind::Header;
                ImU32 tc = t.text;
                const char* tag = "";
                if (before) { tc = t.text_faint; tag = "skipped"; }
                else if (after) { tc = t.text_faint; tag = "after stop"; }
                else switch ((io::RowKind)k) {
                    case io::RowKind::Header: tc = t.accent; tag = "header"; break;
                    case io::RowKind::Comment: tc = t.text_faint; tag = "comment"; break;
                    case io::RowKind::Preamble: tc = t.text_faint; tag = "preamble"; break;
                    case io::RowKind::Summary: tc = t.warn; tag = "summary"; break;
                    case io::RowKind::RepeatedHeader: tc = t.text_faint; tag = "rep. header"; break;
                    case io::RowKind::Blank: tag = ""; break;
                    default: break;
                }
                if (i == L.skip_lines) dl->AddRectFilled(p, ImVec2(p.x + S(3), p.y + lh), t.accent);
                if (!before && !after && k == (int)io::RowKind::Header) dl->AddRectFilled(ImVec2(p.x + gut - S(4), p.y), ImVec2(p.x + gut + tagw + tw + S(8), p.y + lh - S(1)), alpha(t.accent, 26), S(3));
                char num[16];
                std::snprintf(num, sizeof num, "%5d", i + 1);
                dl->AddText(ImVec2(p.x + S(4), p.y), t.text_faint, num);
                if (*tag) dl->AddText(ImVec2(p.x + gut, p.y), alpha(tc, 170), tag);
                if (ui.show_ws) {
                    // whitespace markers in faint colour, text on top
                    std::string vis = s, txt = s;
                    dl->AddText(ImVec2(p.x + gut + tagw, p.y), t.text_faint, vis.c_str());
                    std::string plain = T.lines[i];
                    std::string masked;
                    int col = 0;
                    for (char c : plain) {
                        if (c == '\t') { int nn = L.tab_size - col % L.tab_size; masked.append(nn, ' '); col += nn; }
                        else { masked += c; if (((unsigned char)c & 0xC0) != 0x80) ++col; }
                    }
                    dl->AddText(ImVec2(p.x + gut + tagw, p.y), tc, masked.c_str());
                } else dl->AddText(ImVec2(p.x + gut + tagw, p.y), tc, s.c_str());
                if (after) dl->AddLine(ImVec2(p.x + gut + tagw, p.y + lh * 0.5f), ImVec2(p.x + gut + tagw + tw, p.y + lh * 0.5f), alpha(t.text_faint, 120));
                ImGui::PopID();
            }
        }
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// --------------------------------------------------------------------------
// Step 1: Source
// --------------------------------------------------------------------------
void step_source(ImportWizard& ui, float w, float h) {
    if (ui.source_override) { ui.source_override(w, h); return; }
    io::ImportLayout& L = *ui.layout;
    const io::ParsedText& T = *ui.text;
    bool ch = false;
    const float y0 = ImGui::GetCursorPosY();
    // row 1: encoding + data type
    dim_label("File origin / encoding");
    static const io::Encoding encs[] = {io::Encoding::Auto, io::Encoding::Utf8, io::Encoding::Utf16LE, io::Encoding::Utf16BE, io::Encoding::Cp1252};
    static const char* enc_lbl[] = {"Auto detect", "UTF-8", "UTF-16 LE", "UTF-16 BE", "Windows-1252 (ANSI)"};
    int ce = 0;
    for (int i = 0; i < 5; ++i) if (encs[i] == L.encoding || (L.encoding == io::Encoding::Utf8Bom && i == 1)) ce = i;
    ImGui::SetNextItemWidth(S(170));
    if (ImGui::BeginCombo("##enc", enc_lbl[ce])) {
        for (int i = 0; i < 5; ++i) if (ImGui::Selectable(enc_lbl[i], i == ce)) { L.encoding = encs[i]; ch = true; }
        ImGui::EndCombo();
    }
    if (ce == 0) faint("(%s)", io::encoding_name(T.encoding));
    ImGui::SameLine(0, S(24));
    mode_radios(ui);

    // row 2: start / header / stop
    dim_label("Start import at line");
    int st = L.skip_lines + 1;
    ImGui::SetNextItemWidth(S(96));
    if (ImGui::InputInt("##start", &st, 1, 10)) { L.skip_lines = std::clamp(st - 1, 0, std::max(0, (int)T.lines.size() - 1)); ch = true; }
    ImGui::SameLine(0, S(20));
    dim_label("Headers");
    const char* hdr_lbl[] = {"Auto detect", "My data has no headers", "Header on line"};
    int hm = L.header_line == -1 ? 0 : L.header_line == -2 ? 1 : 2;
    ImGui::SetNextItemWidth(S(178));
    if (ImGui::BeginCombo("##hdr", hdr_lbl[hm])) {
        for (int i = 0; i < 3; ++i) if (ImGui::Selectable(hdr_lbl[i], i == hm)) {
            L.header_line = i == 0 ? -1 : i == 1 ? -2 : std::max(L.skip_lines, T.header_line >= 0 ? T.header_line : L.skip_lines);
            ch = true;
        }
        ImGui::EndCombo();
    }
    if (hm == 2) {
        ImGui::SameLine();
        int hl = L.header_line + 1;
        ImGui::SetNextItemWidth(S(96));
        if (ImGui::InputInt("##hl", &hl, 1, 5)) { L.header_line = std::max(0, hl - 1); ch = true; }
    } else if (hm == 0) faint("%s", T.header_line >= 0 ? "(line " + std::to_string(T.header_line + 1) + ")" : std::string("(none found)"));
    if (hm != 1) {
        ImGui::SameLine(0, S(14));
        dim_label("rows");
        int hr = L.header_rows;
        ImGui::SetNextItemWidth(S(80));
        if (ImGui::InputInt("##hrows", &hr, 1, 1)) { L.header_rows = std::clamp(hr, 0, 6); ch = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Number of header lines (multi-row headers such as name + unit). 0 = auto.");
    }
    ImGui::SameLine(0, S(20));
    dim_label("Stop at line");
    int sl = L.stop_line < 0 ? 0 : L.stop_line + 1;
    ImGui::SetNextItemWidth(S(96));
    if (ImGui::InputInt("##stop", &sl, 1, 10)) { L.stop_line = sl <= 0 ? -1 : sl - 1; ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lines from here on are ignored (0 = read to the end of the file).");

    // row 3: comments, summary, whitespace
    dim_label("Ignore lines starting with");
    ImGui::SetNextItemWidth(S(120));
    if (ImGui::InputText("##cpre", ui.comment_buf, sizeof ui.comment_buf)) {
        L.comment_prefixes.clear();
        std::istringstream is(ui.comment_buf);
        std::string tok;
        while (is >> tok) L.comment_prefixes.push_back(tok);
        ch = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Comment prefixes separated by spaces, e.g.  ; # // *");
    ImGui::SameLine(0, S(20));
    if (ImGui::Checkbox("Ignore trailing summary rows", &L.skip_summary)) ch = true;
    ImGui::SameLine(0, S(20));
    ImGui::Checkbox("Show whitespace", &ui.show_ws);
    ImGui::SameLine(0, S(20));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(C4(th().text_faint), "%d lines \xC2\xB7 click a line to start there, right-click for header / stop", (int)T.lines.size());
    if (ch) ui.reanalyze();
    draw_raw_lines(ui, w, h - (ImGui::GetCursorPosY() - y0) - S(6));
}

// --------------------------------------------------------------------------
// fixed-width ruler editor
// --------------------------------------------------------------------------
void draw_fixed_editor(ImportWizard& ui, float w, float h) {
    const gui::GTheme& t = th();
    io::ImportLayout& L = *ui.layout;
    const io::ParsedText& T = *ui.text;
    std::vector<int>& cuts = L.fixed_cuts;
    ImGui::PushFont(gui::g_font_mono);
    const float cw = ImGui::CalcTextSize("MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM").x / 100.f, lh = ImGui::GetTextLineHeight();
    // lines to show: the table window, tab-expanded
    std::vector<std::pair<int, std::string>> lines;
    int a = std::max(0, L.skip_lines), b = (int)T.lines.size();
    if (L.stop_line >= 0) b = std::min(b, L.stop_line);
    size_t maxlen = 0;
    for (int i = a; i < b && (int)lines.size() < 300; ++i) {
        std::string s;
        int col = 0;
        for (char c : T.lines[i]) {
            if (c == '\t') { int nn = L.tab_size - col % L.tab_size; s.append(nn, ' '); col += nn; }
            else { s += c; if (((unsigned char)c & 0xC0) != 0x80) ++col; }
        }
        maxlen = std::max(maxlen, (size_t)col);
        lines.push_back({i, s});
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.field);
    if (ImGui::BeginChild("##fixed", ImVec2(w, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float gut = ImGui::CalcTextSize("00000").x + S(10);
        ImVec2 o = ImGui::GetCursorScreenPos();
        const float ruler = lh + S(8);
        const int n = (int)lines.size();
        const float total_w = gut + std::max((float)maxlen + 8, 60.f) * cw + S(8);
        const float x0 = o.x + gut;
        ImGui::InvisibleButton("##area", ImVec2(total_w, ruler + n * lh + S(4)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hov = ImGui::IsItemHovered();
        const float mx = ImGui::GetMousePos().x;
        int col = std::max(0, (int)std::floor((mx - x0) / cw + 0.5f));
        auto near_cut = [&]() -> int {
            for (int k = 0; k < (int)cuts.size(); ++k)
                if (cuts[k] > 0 && std::fabs(x0 + cuts[k] * cw - mx) <= std::max(cw * 0.5f, S(4))) return k;
            return -1;
        };
        // interactions
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            ui.drag_cut = near_cut();
            ui.drag_moved = false;
            ui.pending_add = ui.drag_cut < 0 ? col : -1;
        }
        if (ImGui::IsItemActive() && ui.drag_cut >= 0 && ui.drag_cut < (int)cuts.size()) {
            int nc = std::max(1, col);
            bool dup = false;
            for (int k = 0; k < (int)cuts.size(); ++k) if (k != ui.drag_cut && cuts[k] == nc) dup = true;
            if (!dup && nc != cuts[ui.drag_cut]) { cuts[ui.drag_cut] = nc; ui.drag_moved = true; }
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (ImGui::IsItemDeactivated()) {
            bool ch = false;
            if (ui.drag_cut >= 0 && ui.drag_moved) ch = true;
            if (ui.drag_cut < 0 && ui.pending_add > 0 && std::find(cuts.begin(), cuts.end(), ui.pending_add) == cuts.end()) { cuts.push_back(ui.pending_add); ch = true; }
            ui.drag_cut = -1;
            ui.pending_add = -1;
            if (ch) {
                L.mode = io::SplitMode::Fixed; L.sep = io::Sep::Auto;
                std::sort(cuts.begin(), cuts.end());
                if (cuts.empty() || cuts[0] != 0) cuts.insert(cuts.begin(), 0);
                ui.reanalyze();
            }
        }
        if (hov && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
            int k = near_cut();
            if (k >= 0) { cuts.erase(cuts.begin() + k); L.mode = io::SplitMode::Fixed; L.sep = io::Sep::Auto; ui.drag_cut = -1; ui.pending_add = -1; ui.reanalyze(); }
        }
        // ruler
        dl->AddRectFilled(o, ImVec2(o.x + total_w, o.y + ruler), t.panel2);
        for (int c = 0; c <= (int)maxlen + 6; ++c) {
            float x = x0 + c * cw;
            float hg = c % 10 == 0 ? ruler * 0.55f : c % 5 == 0 ? ruler * 0.35f : ruler * 0.18f;
            dl->AddLine(ImVec2(x, o.y + ruler - hg), ImVec2(x, o.y + ruler), t.text_faint);
            if (c % 10 == 0 && c) dl->AddText(ImVec2(x + 2, o.y), t.text_faint, std::to_string(c).c_str());
        }
        // column bands
        for (size_t k = 0; k < cuts.size(); ++k) {
            float xa = x0 + cuts[k] * cw, xb = k + 1 < cuts.size() ? x0 + cuts[k + 1] * cw : x0 + (maxlen + 4) * cw;
            if (k % 2 == 1) dl->AddRectFilled(ImVec2(xa, o.y + ruler), ImVec2(xb, o.y + ruler + n * lh), alpha(t.accent, 14));
        }
        // text (visible lines only)
        float sy0 = ImGui::GetWindowPos().y, sy1 = sy0 + ImGui::GetWindowHeight();
        int i0 = std::max(0, (int)((sy0 - o.y - ruler) / lh)), i1 = std::min(n, (int)((sy1 - o.y - ruler) / lh) + 1);
        for (int i = i0; i < i1; ++i) {
            float y = o.y + ruler + i * lh;
            char num[16];
            std::snprintf(num, sizeof num, "%5d", lines[i].first + 1);
            dl->AddText(ImVec2(o.x + S(2), y), t.text_faint, num);
            dl->AddText(ImVec2(x0, y), lines[i].first == T.header_line ? t.accent : t.text, lines[i].second.c_str());
        }
        // cuts
        for (int k = 0; k < (int)cuts.size(); ++k) {
            if (cuts[k] <= 0) continue;
            float x = x0 + cuts[k] * cw;
            bool act = k == ui.drag_cut;
            dl->AddLine(ImVec2(x, o.y + S(2)), ImVec2(x, o.y + ruler + n * lh), act ? t.warn : t.accent, act ? 2.5f : 1.5f);
            dl->AddTriangleFilled(ImVec2(x - S(5), o.y + ruler - S(8)), ImVec2(x + S(5), o.y + ruler - S(8)), ImVec2(x, o.y + ruler), act ? t.warn : t.accent);
        }
        if (hov && ui.drag_cut < 0) {
            int k = near_cut();
            if (k >= 0) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                ImGui::PushFont(gui::g_font);
                ImGui::SetTooltip("Break at column %d \xE2\x80\x94 drag to move, double-click or right-click to delete", cuts[k]);
                ImGui::PopFont();
            } else if (col > 0) {
                float x = x0 + col * cw;
                dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + ruler + n * lh), alpha(t.accent, 90), 1.f);
                ImGui::PushFont(gui::g_font);
                ImGui::SetTooltip("Column %d \xE2\x80\x94 click to add a break line", col);
                ImGui::PopFont();
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// --------------------------------------------------------------------------
// data grid: mode 0 = split preview, 1 = column setup (role / format / unit rows)
// --------------------------------------------------------------------------
void draw_grid(ImportWizard& ui, float w, float h, bool setup) {
    const gui::GTheme& t = th();
    io::ParsedText& T = *ui.text;
    io::ImportLayout& L = *ui.layout;
    const int nc = (int)T.ncols();
    if (nc == 0) { ImGui::TextColored(C4(t.warn), "No columns detected \xE2\x80\x94 change the data type, delimiters or start line."); return; }
    const bool bom = ui.purpose == io::Purpose::Bom;
    auto roles = bom ? io::bom_wizard_roles() : io::roles_for(ui.purpose);
    if (std::find(roles.begin(), roles.end(), io::Role::Unused) == roles.end()) roles.insert(roles.begin(), io::Role::Unused);
    auto rname = [bom](io::Role r) { return bom ? io::bom_role_label(r) : r == io::Role::Value ? "Value (optional)" : io::role_name(r); };   // R16c: Value is metadata only
    static const char* fmt_lbl[] = {"General", "Text", "Number", "Angle", "Do not import"};
    static const io::LenUnit units[] = {io::LenUnit::Auto, io::LenUnit::MM, io::LenUnit::Mil, io::LenUnit::Inch, io::LenUnit::CM};
    static const char* unit_lbl[] = {"unit: auto", "mm", "mil", "inch", "cm"};
    if (ImGui::BeginTable(setup ? "##setup" : "##prev", nc + 1, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                                          ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(w, h))) {
        ImGui::TableSetupScrollFreeze(1, setup ? 4 : 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, S(44));
        for (int c = 0; c < nc; ++c) ImGui::TableSetupColumn(("c" + std::to_string(c)).c_str(), ImGuiTableColumnFlags_WidthFixed, setup ? S(128) : S(112));
        if (setup) {
            // role row
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(C4(t.text_faint), "Role");
            for (int c = 0; c < nc; ++c) {
                ImGui::TableSetColumnIndex(c + 1);
                ImGui::PushID(c);
                io::Column& col = T.cols[c];
                bool skip = col.format == io::ColFormat::Skip;
                ImU32 rc = role_color(col.role);
                ImGui::BeginDisabled(skip);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, col.role == io::Role::Unused ? t.field : alpha(rc, 50));
                ImGui::PushStyleColor(ImGuiCol_Text, col.role == io::Role::Unused ? t.text_faint : t.text);
                ImGui::SetNextItemWidth(-1);
                bool open = ImGui::BeginCombo("##role", skip ? "(skipped)" : rname(col.role), ImGuiComboFlags_HeightLarge);
                ImGui::PopStyleColor(2);
                if (open) {
                    for (size_t ri = 0; ri < roles.size(); ++ri) {
                        auto r = roles[ri];
                        if (bom && r == io::Role::Vendor) ImGui::Separator();   // less common fields below
                        if (ImGui::Selectable(rname(r), r == col.role)) {
                            // manual reassignment: a field lives in one column only
                            if (r != io::Role::Unused)
                                for (int o = 0; o < nc; ++o) if (o != c && T.cols[o].role == r) L.role_override[o] = io::Role::Unused;
                            L.role_override[c] = r; ui.rebuild();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    const char* src[] = {"none", "header text", "value pattern", "template", "you"};
                    std::string tip = "Column " + std::to_string(c + 1) + (col.header.empty() ? "" : " \"" + col.header + "\"") +
                                      "\nDetected type: " + io::vtype_name(col.type) + "\nRole from: " + src[std::min((int)col.role_src, 4)];
                    if (col.unit != io::LenUnit::Auto) tip += std::string("\nUnit: ") + io::len_unit_name(col.unit);
                    if (!col.samples.empty()) tip += "\nSample: " + col.samples[0];
                    ImGui::SetTooltip("%s", tip.c_str());
                }
                ImGui::PopID();
            }
            // format row (BOM: every field is text; only "import / skip" is offered)
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(C4(t.text_faint), bom ? "Import" : "Format");
            for (int c = 0; bom && c < nc; ++c) {
                ImGui::TableSetColumnIndex(c + 1);
                ImGui::PushID(c + 1000);
                bool on = T.cols[c].format != io::ColFormat::Skip;
                if (ImGui::Checkbox(T.cols[c].role == io::Role::Qty ? "number" : "text", &on)) {
                    if (on) L.col_format.erase(c); else { L.col_format[c] = io::ColFormat::Skip; L.role_override.erase(c); }
                    ui.reanalyze();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("BOM fields are kept exactly as written (part numbers, references, line items).\nOnly Quantity is read as a number. Untick to skip the column.");
                ImGui::PopID();
            }
            for (int c = 0; !bom && c < nc; ++c) {
                ImGui::TableSetColumnIndex(c + 1);
                ImGui::PushID(c + 1000);
                int f = (int)T.cols[c].format;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##fmt", fmt_lbl[f])) {
                    for (int k = 0; k < 5; ++k) if (ImGui::Selectable(fmt_lbl[k], k == f)) {
                        if (k == 0) L.col_format.erase(c); else L.col_format[c] = (io::ColFormat)k;
                        if (k == (int)io::ColFormat::Skip) L.role_override.erase(c);
                        ui.reanalyze();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("General: auto\nText: keep as typed (no number handling)\nNumber / Angle: thousands / trailing minus / negate applied\nDo not import: column is skipped");
                ImGui::PopID();
            }
            // unit / negate / invert row
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(C4(t.text_faint), "Options");
            for (int c = 0; c < nc; ++c) {
                ImGui::TableSetColumnIndex(c + 1);
                ImGui::PushID(c + 2000);
                io::Column& col = T.cols[c];
                io::Role r = col.role;
                bool coord = r == io::Role::X || r == io::Role::Y || r == io::Role::Height;
                bool numeric = coord || r == io::Role::Rotation || col.format == io::ColFormat::Number || col.format == io::ColFormat::Angle ||
                               col.type == io::VType::Number || col.type == io::VType::Integer || col.type == io::VType::Length || col.type == io::VType::Angle;
                if (col.format == io::ColFormat::Skip) { ImGui::TextDisabled("\xE2\x80\x94"); ImGui::PopID(); continue; }
                if (bom) { coord = false; numeric = false; }   // no numeric transforms for BOM fields
                if (coord) {
                    int u = 0;
                    auto it = L.unit_override.find(c);
                    for (int k = 0; k < 5; ++k) if (it != L.unit_override.end() && units[k] == it->second) u = k;
                    ImGui::SetNextItemWidth(S(80));
                    std::string prev = u == 0 && col.unit != io::LenUnit::Auto ? std::string("auto: ") + io::len_unit_name(col.unit) : unit_lbl[u];
                    if (ImGui::BeginCombo("##unit", prev.c_str())) {
                        for (int k = 0; k < 5; ++k) if (ImGui::Selectable(unit_lbl[k], k == u)) {
                            if (k == 0) L.unit_override.erase(c); else L.unit_override[c] = units[k];
                            ui.reanalyze();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine(0, S(4));
                }
                if (numeric) {
                    bool ng = col.negate;
                    if (ImGui::Checkbox("\xE2\x88\x92##neg", &ng)) { if (ng) L.negate[c] = true; else L.negate.erase(c); ui.reanalyze(); }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Negate the values of this column");
                } else if (r == io::Role::Dnp) {
                    bool inv = col.invert;
                    if (ImGui::Checkbox("means fitted##inv", &inv)) { L.invert_override[c] = inv; ui.rebuild(); }
                } else if (r == io::Role::Side) {   // R16c: read through the side table under the grid
                    ImGui::TextColored(C4(t.text_faint), "see Side mapping");
                } else ImGui::TextDisabled(" ");
                ImGui::PopID();
            }
        }
        // header text row
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(C4(t.text_faint), "Line");
        for (int c = 0; c < nc; ++c) {
            ImGui::TableSetColumnIndex(c + 1);
            ImGui::PushFont(gui::g_font_bold);
            bool skip = T.cols[c].format == io::ColFormat::Skip;
            ImGui::PushStyleColor(ImGuiCol_Text, skip ? t.text_faint : t.text);
            ImGui::TextUnformatted(T.cols[c].header.empty() ? ("Column " + std::to_string(c + 1)).c_str() : T.cols[c].header.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        std::vector<int> idx;
        for (int r = 0; r < (int)T.rows.size(); ++r) {
            auto k = T.rows[r].kind;
            if (k == io::RowKind::Blank || k == io::RowKind::Header) continue;
            if (setup && k != io::RowKind::Data) continue;
            idx.push_back(r);
        }
        ImGuiListClipper clip;
        clip.Begin((int)idx.size());
        while (clip.Step()) {
            for (int q = clip.DisplayStart; q < clip.DisplayEnd; ++q) {
                const io::Row& row = T.rows[idx[q]];
                ImGui::TableNextRow();
                bool data = row.kind == io::RowKind::Data;
                ImU32 tc = data ? t.text : t.text_faint;
                if (!data) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, alpha(t.text_faint, 18));
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(C4(t.text_faint), "%d", row.line + 1);
                if ((!data || !row.note.empty()) && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s%s%s", io::row_kind_name(row.kind), row.note.empty() ? "" : ": ", row.note.c_str());
                for (int c = 0; c < nc && c < (int)row.cells.size(); ++c) {
                    ImGui::TableSetColumnIndex(c + 1);
                    bool skip = T.cols[c].format == io::ColFormat::Skip;
                    if (skip) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, alpha(t.text_faint, 22));
                    ImGui::PushStyleColor(ImGuiCol_Text, skip ? t.text_faint : tc);
                    ImGui::TextUnformatted(row.cells[c].c_str());
                    ImGui::PopStyleColor();
                }
                if (!data && row.cells.empty() && row.line < (int)T.lines.size()) {
                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushStyleColor(ImGuiCol_Text, tc);
                    ImGui::TextUnformatted(T.lines[row.line].c_str());
                    ImGui::PopStyleColor();
                }
            }
        }
        ImGui::EndTable();
    }
}

// --------------------------------------------------------------------------
// Step 2: Split
// --------------------------------------------------------------------------
void step_split(ImportWizard& ui, float w, float h) {
    io::ImportLayout& L = *ui.layout;
    const io::ParsedText& T = *ui.text;
    mode_radios(ui);
    const float y0 = ImGui::GetCursorPosY();
    if (ui.eff_mode() == io::SplitMode::Delimited) {
        const bool expl = L.mode == io::SplitMode::Delimited;
        const std::string& ds = expl ? L.delims : T.delims;
        dim_label("Delimiters");
        struct D { const char* lbl; char c; };
        static const D dd[] = {{"Tab", '\t'}, {"Semicolon", ';'}, {"Comma", ','}, {"Space", ' '}, {"Pipe  |", '|'}};
        for (auto& d : dd) {
            bool on = ds.find(d.c) != std::string::npos;
            if (ImGui::Checkbox(d.lbl, &on)) {
                if (!expl) ui.set_mode(io::SplitMode::Delimited);
                std::string& m = L.delims;
                if (on && m.find(d.c) == std::string::npos) m += d.c;
                if (!on) m.erase(std::remove(m.begin(), m.end(), d.c), m.end());
                ui.reanalyze();
            }
            ImGui::SameLine(0, S(14));
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Other:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(70));
        if (!expl) std::snprintf(ui.other_buf, sizeof ui.other_buf, "%s", T.other_delim.c_str());
        if (ImGui::InputText("##other", ui.other_buf, sizeof ui.other_buf)) {
            if (!expl) { std::string keep = ui.other_buf; ui.set_mode(io::SplitMode::Delimited); std::snprintf(ui.other_buf, sizeof ui.other_buf, "%s", keep.c_str()); }
            L.other_delim = ui.other_buf;
            ui.reanalyze();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Any other delimiter; several characters are matched as one string (e.g. \"::\" or \" - \").");
        bool cons = expl ? L.consecutive_as_one : T.consecutive_as_one;
        if (ImGui::Checkbox("Treat consecutive delimiters as one", &cons)) {
            if (!expl) ui.set_mode(io::SplitMode::Delimited);
            L.consecutive_as_one = cons;
            ui.reanalyze();
        }
        ImGui::SameLine(0, S(24));
        dim_label("Text qualifier");
        const char* ql[] = {"\"", "'", "{none}"};
        int qi = L.quote == '"' ? 0 : L.quote == '\'' ? 1 : 2;
        ImGui::SetNextItemWidth(S(90));
        if (ImGui::BeginCombo("##q", ql[qi])) {
            for (int i = 0; i < 3; ++i) if (ImGui::Selectable(ql[i], i == qi)) { L.quote = i == 0 ? '"' : i == 1 ? '\'' : 0; ui.reanalyze(); }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, S(24));
        if (ImGui::Checkbox("Trim spaces", &L.trim)) ui.reanalyze();
        if (!expl) {
            ImGui::SameLine(0, S(14));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(C4(th().text_faint), "(auto-detected \xE2\x80\x94 any change fixes the delimiters)");
        }
    } else {
        if (gui::button("Auto-detect breaks", Icon::AutoScan)) {
            L.fixed_cuts = io::suggest_fixed_cuts(ui.window_lines(), L.tab_size);
            if (L.fixed_cuts.empty()) L.fixed_cuts = {0};
            if (L.mode != io::SplitMode::Fixed) L.mode = io::SplitMode::Fixed;
            ui.reanalyze();
        }
        ImGui::SameLine(0, S(6));
        if (gui::button("Clear all", Icon::Delete)) { L.mode = io::SplitMode::Fixed; L.fixed_cuts = {0}; ui.reanalyze(); }
        ImGui::SameLine(0, S(18));
        dim_label("Tab width");
        int tw = L.tab_size;
        ImGui::SetNextItemWidth(S(90));
        if (ImGui::InputInt("##tabw", &tw, 1, 2)) { L.tab_size = std::clamp(tw, 1, 16); ui.reanalyze(); }
        ImGui::SameLine(0, S(18));
        if (ImGui::Checkbox("Trim spaces", &L.trim)) ui.reanalyze();
        ImGui::SameLine(0, S(18));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(C4(th().text_dim), "Click to add a break line \xC2\xB7 drag to move \xC2\xB7 double-click / right-click to delete  (%d columns)",
                           (int)std::max<size_t>(1, L.mode == io::SplitMode::Fixed ? L.fixed_cuts.size() : T.cuts.size()));
        if (L.mode != io::SplitMode::Fixed && !T.cuts.empty()) { L.fixed_cuts = T.cuts; }
        draw_fixed_editor(ui, w, S(190));
    }
    float used = ImGui::GetCursorPosY() - y0;
    gui::section_header("Data preview");
    draw_grid(ui, w, std::max(S(120), h - used - S(70)), false);
}

// R16c: number format row (was inline in step_columns; also used by the centroid wizard's Advanced panel)
void number_format_row(ImportWizard& ui) {
    io::ImportLayout& L = *ui.layout;
    const io::ParsedText& T = *ui.text;
    bool ch = false;
    // number format
    gui::section_header("Number format");
    dim_label("Decimal separator");
    const char* dl[] = {"Auto", "Period  .", "Comma  ,"};
    int dc = L.decimal_comma + 1;
    ImGui::SetNextItemWidth(S(110));
    if (ImGui::BeginCombo("##dc", dl[dc])) {
        for (int i = 0; i < 3; ++i) if (ImGui::Selectable(dl[i], i == dc)) { L.decimal_comma = i - 1; ch = true; }
        ImGui::EndCombo();
    }
    if (dc == 0) faint("(%s)", T.decimal_comma ? "comma" : "period");
    ImGui::SameLine(0, S(20));
    dim_label("Thousands separator");
    const char tch[] = {0, ',', '.', ' ', '\''};
    const char* tl[] = {"None", "Comma  ,", "Period  .", "Space", "Apostrophe  '"};
    int ti = 0;
    for (int i = 0; i < 5; ++i) if (tch[i] == L.thousands_sep) ti = i;
    ImGui::SetNextItemWidth(S(130));
    if (ImGui::BeginCombo("##ths", tl[ti])) {
        for (int i = 0; i < 5; ++i) if (ImGui::Selectable(tl[i], i == ti)) { L.thousands_sep = tch[i]; ch = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, S(20));
    if (ImGui::Checkbox("Trailing minus for negative numbers", &L.trailing_minus)) ch = true;
    
    if (ch) ui.reanalyze();
}

// --------------------------------------------------------------------------
// Step 3: Columns
// --------------------------------------------------------------------------
void step_columns(ImportWizard& ui, float w, float h) {
    if (ui.mapping_ok && !ui.mapping_ok()) {
        gui::icon_image(Icon::Warning, ImGui::GetFontSize());
        ImGui::SameLine();
        ImGui::TextColored(C4(th().error), "%s", ui.mapping_hint);
    } else {
        ImGui::TextColored(C4(th().text_dim), "Pick a role for each column; set \"Do not import\" for columns to skip. Hover a role for the detected type.");
    }
    draw_grid(ui, w, S(230), true);
    bool ch = false;
    if (ui.purpose != io::Purpose::Bom) number_format_row(ui);
    if (ch) ui.reanalyze();
    gui::section_header(ui.purpose == io::Purpose::Bom ? "BOM options" : "Placement options");
    if (ui.options) ui.options(w);
    (void)h;
}

void draw_issues(const std::vector<std::pair<int, std::string>>& issues, float w, float h) {
    const gui::GTheme& t = th();
    if (ImGui::BeginChild("##issues", ImVec2(w, h), ImGuiChildFlags_Borders)) {
        if (issues.empty()) {
            gui::icon_image(Icon::Check, ImGui::GetFontSize());
            ImGui::SameLine();
            ImGui::TextColored(C4(t.ok), "No issues found");
        }
        for (size_t i = 0; i < issues.size() && i < 2000; ++i) {
            ImGui::PushID((int)i);
            gui::icon_image(issues[i].first ? Icon::Warning : Icon::About, ImGui::GetFontSize());
            ImGui::SameLine();
            if (issues[i].first) ImGui::TextColored(C4(t.text_dim), "Line %d:", issues[i].first), ImGui::SameLine();
            ImGui::TextUnformatted(issues[i].second.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

template <class F>
void result_table(const char* id, const std::vector<const char*>& cols, int n, float w, float h, F cell) {
    if (ImGui::BeginTable(id, (int)cols.size(), ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                                ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(w, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (auto* c : cols) ImGui::TableSetupColumn(c, 0, S(100));
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(n);
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                ImGui::TableNextRow();
                for (int c = 0; c < (int)cols.size(); ++c) { ImGui::TableSetColumnIndex(c); cell(r, c); }
            }
        ImGui::EndTable();
    }
}

void stat_chip(const char* label, const std::string& v, ImU32 col) {
    const gui::GTheme& t = th();
    ImVec2 p = ImGui::GetCursorScreenPos();
    std::string s = v + (*label ? "  " : "") + label;
    ImVec2 ts = ImGui::CalcTextSize(s.c_str());
    ImVec2 b(p.x + ts.x + S(20), p.y + ImGui::GetFrameHeight());
    ImGui::GetWindowDrawList()->AddRectFilled(p, b, alpha(col, 34), S(8));
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + S(10), p.y + (b.y - p.y - ts.y) * 0.5f), t.text, s.c_str());
    ImGui::Dummy(ImVec2(b.x - p.x, b.y - p.y));
    ImGui::SameLine(0, S(6));
}

// the whole wizard body (one Form custom field)
void draw_wizard(ImportWizard& ui, Form& f, HWND owner, float w) {
    const float H = S(560);
    draw_steps(ui, w);
    ImGui::SameLine(0, S(12));
    ImGui::BeginGroup();
    draw_templates(ui, f, owner);
    if (!ui.tpl_status.empty()) ImGui::TextColored(C4(th().ok), "%s", ui.tpl_status.c_str());
    else ImGui::TextColored(C4(th().text_faint), "%s", ui.text->describe().substr(0, 60).c_str());
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, S(2)));
    if (ImGui::BeginChild("##step", ImVec2(w, H), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const float cw = ImGui::GetContentRegionAvail().x;
        switch (ui.step) {
        case 0: step_source(ui, cw, H); break;
        case 1: step_split(ui, cw, H); break;
        case 2: step_columns(ui, cw, H); break;
        default: if (ui.review) ui.review(cw, H); break;
        }
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// R16c: modern wizard chrome shared by the BOM and the centroid wizards (was inline in the BOM wizard):
// initial large centred size, one Maximize / Restore title-bar toggle, and the numbered stepper.
// ---------------------------------------------------------------------------
void wizard_chrome(bool& maximize, bool& first, ImVec2 init_size) {
    const gui::GTheme& t = th();
    ImGuiWindow* win = ImGui::GetCurrentWindow()->RootWindow;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (first) {   // initial size: large, centred, inside the work area (unless the user sized this wizard before)
        first = false;
        gui::form_initial_size(ImVec2(std::min(init_size.x, vp->WorkSize.x * 0.92f), std::min(init_size.y, vp->WorkSize.y * 0.90f)));
    }
    maximize = gui::form_is_maximized();
    // ---- R13 (REVIEW_V12 §7): Maximize / Restore is ONE title-bar toggle (double-click on the title bar is handled
    // by the dialog framework for every dialog)
    {
        const ImRect tb = win->TitleBarRect();
        const float bh = tb.GetHeight() - S(8), bw2 = bh + S(6);
        const ImVec2 bp(tb.Max.x - S(10) - bw2 - (win->HasCloseButton ? tb.GetHeight() : 0.f), tb.Min.y + S(4));
        ImGui::PushClipRect(win->Rect().Min, win->Rect().Max, false);
        const ImVec2 keep = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(bp);
        bool tog = ImGui::InvisibleButton("##bom_max", ImVec2(bw2, bh));
        const bool hv = ImGui::IsItemHovered();
        ImDrawList* tdl = ImGui::GetWindowDrawList();
        if (hv) tdl->AddRectFilled(bp, ImVec2(bp.x + bw2, bp.y + bh), t.raised, S(4));
        const float q = std::min(bw2, bh) * 0.32f;
        const ImVec2 c(bp.x + bw2 * 0.5f, bp.y + bh * 0.5f);
        const ImU32 ic = hv ? t.text : t.text_dim;
        if (!maximize) tdl->AddRect(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q, c.y + q), ic, S(1), 0, S(1.4f));
        else {
            tdl->AddRect(ImVec2(c.x - q + S(3), c.y - q - S(1)), ImVec2(c.x + q + S(1), c.y + q - S(3)), ic, S(1), 0, S(1.2f));
            tdl->AddRectFilled(ImVec2(c.x - q - S(1), c.y - q + S(2)), ImVec2(c.x + q - S(2), c.y + q + S(1)), t.panel);
            tdl->AddRect(ImVec2(c.x - q - S(1), c.y - q + S(2)), ImVec2(c.x + q - S(2), c.y + q + S(1)), ic, S(1), 0, S(1.4f));
        }
        if (hv) ImGui::SetTooltip(maximize ? "Restore" : "Maximize (or double-click the title bar)");
        ImGui::SetCursorScreenPos(keep);
        ImGui::PopClipRect();
        if (tog) { gui::form_toggle_maximize(); maximize = !maximize; }
    }
}

// Numbered stepper; a step is reachable when it is not ahead of the current one or fwd_ok.
void wizard_stepper(const char* const* names, const char* const* tips, int n, int& stage, bool fwd_ok, const std::function<bool(int)>& warn_of) {
    const gui::GTheme& t = th();
    ImDrawList* sdl = ImGui::GetWindowDrawList();
    const float hh = ImGui::GetFrameHeight() + S(4);
    float x = ImGui::GetCursorScreenPos().x;
    const float y = ImGui::GetCursorScreenPos().y;
    for (int i = 0; i < n; ++i) {
        const bool cur = stage == i, done = stage > i;
        const bool warn = warn_of && warn_of(i);
        const bool reachable = i <= stage || fwd_ok;
        const float tw = ImGui::CalcTextSize(names[i]).x;
        const float r = S(10), wseg = r * 2 + S(8) + tw + S(10);
        if (i) {   // connector
            sdl->AddLine(ImVec2(x, y + hh * 0.5f), ImVec2(x + S(26), y + hh * 0.5f), done || cur ? t.accent : t.border, S(2));
            x += S(32);
        }
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(i + 4100);
        if (ImGui::InvisibleButton("st", ImVec2(wseg, hh)) && reachable) stage = i;
        const bool hv = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hv && tips) ImGui::SetTooltip("%s", tips[i]);
        const ImVec2 cc(x + r, y + hh * 0.5f);
        if (cur) sdl->AddRectFilled(ImVec2(x - S(4), y), ImVec2(x + wseg, y + hh), gui::col_alpha(t.accent, 0.14f), hh * 0.5f);
        else if (hv && reachable) sdl->AddRectFilled(ImVec2(x - S(4), y), ImVec2(x + wseg, y + hh), t.raised, hh * 0.5f);
        sdl->AddCircleFilled(cc, r, cur || done ? t.accent : t.panel2);
        if (!cur && !done) sdl->AddCircle(cc, r, t.border, 0, S(1.2f));
        char nb[4]; std::snprintf(nb, sizeof nb, "%d", i + 1);
        if (done) {   // v17: drawn tick (U+2713 is not in the UI font atlas)
            const ImVec2 pts[3] = {ImVec2(cc.x - r * 0.45f, cc.y), ImVec2(cc.x - r * 0.1f, cc.y + r * 0.35f), ImVec2(cc.x + r * 0.45f, cc.y - r * 0.35f)};
            sdl->AddPolyline(pts, 3, t.accent_text, 0, S(1.8f));
        } else {
            const ImVec2 ns = ImGui::CalcTextSize(nb);
            sdl->AddText(ImVec2(cc.x - ns.x * 0.5f, cc.y - ns.y * 0.5f), cur ? t.accent_text : t.text_dim, nb);
        }
        if (cur) ImGui::PushFont(gui::g_font_bold);
        sdl->AddText(ImVec2(x + r * 2 + S(8), y + (hh - ImGui::GetTextLineHeight()) * 0.5f), warn ? t.warn : cur ? t.text : reachable ? t.text_dim : t.text_faint, names[i]);
        if (cur) ImGui::PopFont();
        x += wseg;
    }
    ImGui::SetCursorScreenPos(ImVec2(x + S(24), y + S(2)));
}

// R16c (REVIEW_V15 §7): "what am I importing" segmented switch at the top of both CSV wizards.
// Returns the kind clicked (0 BOM, 1 centroid) when it differs from cur, else -1.
int import_kind_switch(int cur) {
    const gui::GTheme& t = th();
    static const char* lab[] = {"Bill of Materials", "Centroid / Pick-and-Place"};
    static const Icon ic[] = {Icon::ImportBom, Icon::ImportCentroid};
    static const char* tip[] = {"BOM: part numbers, quantities and the references that use them (no coordinates)",
                                "Centroid / pick-and-place: one row per placement with Reference, X, Y, rotation and side"};
    int res = -1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetFrameHeight() + S(4), isz = ImGui::GetFontSize();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float wtot = 0;
    for (int i = 0; i < 2; ++i) wtot += ImGui::CalcTextSize(lab[i]).x + isz + S(30);
    dl->AddRectFilled(p0, ImVec2(p0.x + wtot + S(4), p0.y + h), t.panel2, h * 0.5f);
    float x = p0.x + S(2);
    for (int i = 0; i < 2; ++i) {
        const float w = ImGui::CalcTextSize(lab[i]).x + isz + S(30);
        ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
        ImGui::PushID(i + 4200);
        if (ImGui::InvisibleButton("kind", ImVec2(w, h)) && i != cur) res = i;
        const bool hv = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hv) ImGui::SetTooltip("%s%s", tip[i], i == cur ? "" : "\nClick to open this file in that wizard instead.");
        if (i == cur) dl->AddRectFilled(ImVec2(x, p0.y + S(2)), ImVec2(x + w, p0.y + h - S(2)), t.accent, (h - S(4)) * 0.5f);
        else if (hv) dl->AddRectFilled(ImVec2(x, p0.y + S(2)), ImVec2(x + w, p0.y + h - S(2)), t.raised, (h - S(4)) * 0.5f);
        gui::icon_draw(dl, ic[i], ImVec2(x + S(12), p0.y + (h - isz) * 0.5f), isz, i == cur ? 1.f : 0.7f);
        if (i == cur) ImGui::PushFont(gui::g_font_bold);
        dl->AddText(ImVec2(x + S(18) + isz, p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f), i == cur ? t.accent_text : t.text_dim, lab[i]);
        if (i == cur) ImGui::PopFont();
        x += w;
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y));
    ImGui::Dummy(ImVec2(wtot + S(4), h));
    return res;
}

void opt_label(const char* s, float x) {
    ImGui::SetCursorPosX(x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(C4(th().text_dim), "%s", s);
    ImGui::SameLine(x + S(150));
}
bool opt_combo(const char* id, int& v, const std::vector<const char*>& items, float wdt) {
    ImGui::SetNextItemWidth(wdt);
    bool ch = false;
    if (ImGui::BeginCombo(id, items[std::clamp(v, 0, (int)items.size() - 1)])) {
        for (int i = 0; i < (int)items.size(); ++i) if (ImGui::Selectable(items[i], i == v)) { v = i; ch = true; }
        ImGui::EndCombo();
    }
    return ch;
}

} // namespace

// ===========================================================================
// Pick-and-place / centroid import (ASCII Import #508, Reference Fusion #459)
// ===========================================================================
// R16c (REVIEW_V15 §7): rebuilt on the modern BOM wizard chrome: BOM / Centroid switch, numbered stepper
// Source -> Mapping -> Review & Import, maximisable floating window, Back / Next / Import footer.
// Auto-detection, delimiter parsing, column mapping, templates and Replace / Merge / Fusion are unchanged.
// The Side column is read through a per-value table (io::analyze_sides) with a preview count and overrides.
namespace {
void side_mapping_panel(io::PnpOptions& opt, io::PnpPreview& prev, ImportWizard& ui, int& dside, float w) {
    const gui::GTheme& t = th();
    const io::SideAnalysis& A = prev.sides;
    stat_chip("Top", std::to_string(A.top), t.ok);
    stat_chip("Bottom", std::to_string(A.bottom), A.bottom ? t.accent : t.text_faint);
    stat_chip("not understood", std::to_string(A.unknown), A.unknown ? t.error : t.text_faint);
    ImGui::NewLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(C4(t.text_dim), "Side column:");
    ImGui::SameLine();
    if (A.col < 0) ImGui::TextColored(C4(t.text_faint), "none mapped \xE2\x80\x94 every placement goes on the default side");
    else {
        const std::string h = prev.text.cols[A.col].header.empty() ? "Column " + std::to_string(A.col + 1) : prev.text.cols[A.col].header;
        ImGui::Text("\"%s\"  \xC2\xB7  read as %s", h.c_str(), io::side_conv_name(A.conv));
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(C4(t.text_dim), "Blank / no side column \xE2\x86\x92");
    ImGui::SameLine();
    int ds = dside;
    if (ImGui::RadioButton("Top##ds", ds == 0)) ds = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Bottom##ds", ds == 1)) ds = 1;
    gui::tooltip("Side used for rows whose side cell is empty, and for every row when the file has no side column");
    if (ds != dside) { dside = ds; opt.default_side = (Side)ds; ui.rebuild(); return; }
    if (A.col >= 0) {
        ImGui::SameLine(0, S(16));
        bool inv = prev.text.cols[A.col].invert;
        if (ImGui::Checkbox("Column is a mirrored flag", &inv)) { opt.layout.invert_override[A.col] = inv; ui.rebuild(); return; }
        gui::tooltip("For columns such as \"Mirror\" / \"Flipped\": Yes / 1 / M = bottom, No / 0 / blank = top");
    }
    if (A.col < 0 || A.values.empty()) return;
    std::string pend_tok;
    int pend_side = -2;
    const float th_ = std::min(S(200), ImGui::GetFrameHeightWithSpacing() * (float)(A.values.size() + 1) + S(6));
    if (ImGui::BeginTable("##sidemap", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(w, th_))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Value in file", 0, S(150));
        ImGui::TableSetupColumn("Rows", 0, S(50));
        ImGui::TableSetupColumn("Detected", 0, S(110));
        ImGui::TableSetupColumn("Import as", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < A.values.size(); ++i) {
            const io::SideValue& v = A.values[i];
            ImGui::TableNextRow();
            ImGui::PushID((int)i + 4300);
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            if (v.token.empty()) ImGui::TextColored(C4(t.text_faint), "(blank)");
            else ImGui::TextUnformatted(v.token.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", v.count);
            ImGui::TableSetColumnIndex(2);
            if (v.token.empty()) ImGui::TextColored(C4(t.text_faint), "default side");
            else if (v.auto_side < 0) ImGui::TextColored(C4(t.error), "not understood");
            else ImGui::TextColored(C4(t.text_dim), "%s", v.auto_side ? "Bottom" : "Top");
            ImGui::TableSetColumnIndex(3);
            static const char* ch[] = {"Top", "Bottom"};
            int cur = v.side;
            ImGui::SetNextItemWidth(S(120));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, v.side < 0 ? alpha(t.error, 60) : v.overridden ? alpha(t.warn, 50) : t.field);
            const bool open = ImGui::BeginCombo("##as", cur < 0 ? "choose..." : ch[cur]);
            ImGui::PopStyleColor();
            if (open) {
                for (int k = 0; k < 2; ++k) if (ImGui::Selectable(ch[k], k == cur)) { pend_tok = v.token; pend_side = k; }
                if (v.overridden && ImGui::Selectable("Automatic")) { pend_tok = v.token; pend_side = -1; }
                ImGui::EndCombo();
            }
            if (v.overridden) { ImGui::SameLine(); ImGui::TextColored(C4(t.warn), "override"); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (pend_side != -2) {   // applied after the table: rebuild replaces prev.sides
        if (pend_side < 0) opt.side_override.erase(pend_tok); else opt.side_override[pend_tok] = pend_side;
        ui.rebuild();
    }
}
} // namespace

bool dlg_import_centroid(HWND owner, Doc& d, const std::wstring& path, bool fusion_mode) {
    if (gui::in_frame()) {   // never spin a modal inside a frame (same rule as the BOM wizard)
        std::string b; int st = -1;
        const bool mem = gui::take_import_source(b, st);
        DocPtr dp = docptr_of(d);
        std::wstring p = path;
        gui::defer([owner, dp, p, fusion_mode, mem, b, st]() { if (mem) gui::set_import_source(b, st); if (dp && dlg_import_centroid(owner, *dp, p, fusion_mode)) dp->changed(ui::CH_ALL); });
        return false;
    }
    ImportWizard ui;
    std::string err;
    int src_start = -1;   // editor source (sheet / text editor): bytes in memory, no temp file
    if (!gui::take_import_source(ui.bytes, src_start) && !read_text_file(U(path), ui.bytes, err)) { warning(owner, L"Centroid Import", L"Could not read " + path + L"\n\n" + W(err)); return false; }
    io::PnpOptions opt;
    opt.apply.mode = fusion_mode ? io::CentroidMode::Fusion : (d.prj.comps.empty() ? io::CentroidMode::Replace : io::CentroidMode::Merge);
    opt.apply.set_part = opt.apply.set_package = true;
    opt.apply.auto_align = true;
    io::PnpPreview prev;
    ui.purpose = io::Purpose::Pnp;
    ui.layout = &opt.layout;
    ui.text = &prev.text;
    ui.reanalyze = [&]() { std::string e; prev = io::PnpPreview{}; io::pnp_preview(ui.bytes, opt, prev, e); };
    ui.rebuild = [&]() { io::pnp_rebuild(prev, opt); };
    ui.mapping_ok = [&]() { return prev.mapping_ok(); };
    ui.issue_count = [&]() { return (int)prev.issues.size(); };
    ui.mapping_hint = "Map at least the Reference, X and Y columns.";
    if (src_start > 0) opt.layout.skip_lines = opt.layout.header_line = src_start;
    ui.sync_buffers();
    ui.reanalyze();
    auto_match_template(ui);
    ui.sync_buffers();
    int stage = 0;
    if (const char* e = getenv("PCBP_TEST_IMPORT_STEP")) stage = std::clamp(atoi(e), 0, 2);   // automation / screenshots

    std::string bw_buf, tol_buf = U(fmt_len(opt.apply.fusion_tol_mm, false)), key_buf;
    std::vector<std::string> refs;
    for (auto& c : d.prj.comps) if (!c.ref.empty()) refs.push_back(c.ref);
    std::sort(refs.begin(), refs.end());
    int units_i = 0, dside = 0, bx = 0, brot = 0, sidesel = 0, mode_i = (int)opt.apply.mode, method = 0;
    double factor = 1.0, roff = 0;
    bool adv_split = false, adv_opts = false, maximize = false, first = true, edit_src = false;
    int switch_to = -1;

    auto apply_opts = [&]() {
        opt.units = (io::LenUnit)units_i;
        opt.factor = factor == 0 ? 1.0 : factor;
        opt.rot_offset = roff;
        opt.bottom_x = (io::BottomX)bx;
        opt.board_width = parse_len(W(bw_buf), 0);
        opt.bottom_rot = (io::BottomRot)brot;
        opt.side_select = (io::SideSelect)sidesel;
        opt.default_side = (Side)dside;
        opt.apply.mode = (io::CentroidMode)mode_i;
        ui.rebuild();
    };
    auto advanced = [&](float w) {
        bool ch = false;
        const float c1 = 0, c2 = w * 0.5f + S(8), ww = w * 0.5f - S(170);
        auto text_in = [&](const char* id, std::string& s, float wd) {
            char b[64];
            std::snprintf(b, sizeof b, "%s", s.c_str());
            ImGui::SetNextItemWidth(wd);
            if (ImGui::InputText(id, b, sizeof b)) { s = b; return true; }
            return false;
        };
        opt_label("Units", c1); ch |= opt_combo("##units", units_i, {"Auto detect", "Mil", "Inch", "Millimetre", "Centimetre"}, ww);
        opt_label("Import side(s)", c2); ch |= opt_combo("##sidesel", sidesel, {"Both sides", "Top only", "Bottom only"}, ww);
        opt_label("Factor", c1); ImGui::SetNextItemWidth(ww); ch |= ImGui::InputDouble("##factor", &factor, 0, 0, "%.6g");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Multiplier applied to X/Y before the units (e.g. 0.1 for 1/10 mil files).");
        opt_label("Rotation offset (\xC2\xB0)", c2); ImGui::SetNextItemWidth(ww); ch |= ImGui::InputDouble("##roff", &roff, 0, 0, "%.3f");
        opt_label("Bottom X", c1); ch |= opt_combo("##bx", bx, {"As exported (no change)", "Negate X (viewed from bottom)", "Mirror about board width (W \xE2\x88\x92 x)"}, ww);
        opt_label("Bottom rotation", c2); ch |= opt_combo("##brot", brot, {"As exported", "Negate (\xE2\x88\x92rot)", "Mirror (180 \xE2\x88\x92 rot)", "Flip (rot + 180)"}, ww);
        opt_label(("Board width (" + U(units_label()) + ")").c_str(), c1); ch |= text_in("##bw", bw_buf, ww);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("For 'Mirror about board width'; empty = largest X in the file.");
        ImGui::SetCursorPosX(c1);
        ch |= ImGui::Checkbox("Mirror X (whole file)", &opt.mirror_x); ImGui::SameLine(0, S(14));
        ch |= ImGui::Checkbox("Mirror Y (whole file)", &opt.mirror_y); ImGui::SameLine(0, S(14));
        ch |= ImGui::Checkbox("Skip DNP rows", &opt.skip_dnp); ImGui::SameLine(0, S(14));
        ch |= ImGui::Checkbox("Upper-case refs", &opt.upper_refs); ImGui::SameLine(0, S(14));
        ImGui::Checkbox("Apply part numbers", &opt.apply.set_part); ImGui::SameLine(0, S(14));
        ImGui::Checkbox("Apply packages", &opt.apply.set_package);
        if (ch) apply_opts();
        number_format_row(ui);
    };
    auto fusion_opts = [&](float w) {
        const float c1 = 0, c2 = w * 0.5f + S(8), ww = w * 0.5f - S(170);
        char b[64];
        opt_label("Fusion tolerance", c1);
        std::snprintf(b, sizeof b, "%s", tol_buf.c_str()); ImGui::SetNextItemWidth(ww);
        if (ImGui::InputText("##tol", b, sizeof b)) tol_buf = b;
        opt_label("Matching method", c2); opt_combo("##method", method, {"Centroid", "Pin (any pad)"}, ww);
        opt_label("Key component", c1);
        ImGui::SetNextItemWidth(ww);
        if (ImGui::BeginCombo("##key", key_buf.empty() ? "(none)" : key_buf.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::Selectable("(none)", key_buf.empty())) key_buf.clear();
            for (auto& r : refs) if (ImGui::Selectable(r.c_str(), r == key_buf)) key_buf = r;
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, S(14));
        ImGui::Checkbox("Clear all refs first", &opt.apply.clear_refs); ImGui::SameLine(0, S(14));
        ImGui::Checkbox("Auto-align", &opt.apply.auto_align); ImGui::SameLine(0, S(14));
        ImGui::Checkbox("Copy file rotation", &opt.apply.fusion_update_rot);
    };
    auto commit = [&]() {
        opt.apply.mode = (io::CentroidMode)mode_i;
        opt.apply.fusion_tol_mm = std::max(0.001, parse_len(W(tol_buf), opt.apply.fusion_tol_mm));
        opt.apply.fusion_method = method == 1 ? io::FusionMethod::Pin : io::FusionMethod::Centroid;
        opt.apply.key_ref = key_buf;
    };
    auto review = [&](float w, float h) {
        const gui::GTheme& t = th();
        stat_chip("placements", std::to_string(prev.rows.size()), t.ok);
        int nt = 0, nb = 0;
        for (auto& r : prev.rows) (r.side == Side::Bottom ? nb : nt)++;
        stat_chip("top", std::to_string(nt), t.text_faint);
        stat_chip("bottom", std::to_string(nb), nb ? t.accent : t.text_faint);
        stat_chip("skipped", std::to_string(prev.skipped), prev.skipped ? t.warn : t.text_faint);
        stat_chip("filtered by side", std::to_string(prev.filtered), t.text_faint);
        stat_chip("DNP", std::to_string(prev.dnp), t.text_faint);
        stat_chip("issues", std::to_string(prev.issues.size()), prev.issues.empty() ? t.text_faint : t.warn);
        stat_chip("", std::string("Units: ") + io::len_unit_name(prev.units) + (prev.units_reason.empty() ? "" : " (" + prev.units_reason + ")"), t.accent);
        ImGui::NewLine();
        if (!prev.mapping_ok()) {
            gui::icon_image(Icon::Warning, ImGui::GetFontSize());
            ImGui::SameLine();
            ImGui::TextColored(C4(t.error), "Map at least the Reference, X and Y columns (step 2).");
        }
        result_table("##res", {"Line", "Reference", "X", "Y", "Rotation", "Side", "Package", "Part Number", "Value", "DNP"}, (int)prev.rows.size(), w, std::max(S(120), h - S(170)), [&](int r, int c) {
            const io::CentroidRow& row = prev.rows[r];
            switch (c) {
            case 0: ImGui::TextDisabled("%d", row.line); break;
            case 1: ImGui::TextUnformatted(row.ref.c_str()); break;
            case 2: ImGui::TextUnformatted(gui::fmt_len_u8(row.pos.x, false).c_str()); break;
            case 3: ImGui::TextUnformatted(gui::fmt_len_u8(row.pos.y, false).c_str()); break;
            case 4: if (row.has_rot) ImGui::Text("%.2f", row.rot); break;
            case 5: ImGui::TextColored(C4(row.side == Side::Top ? t.text : t.accent), "%s%s", row.side == Side::Top ? "Top" : "Bottom", row.has_side ? "" : " (default)"); break;
            case 6: ImGui::TextUnformatted(row.package.c_str()); break;
            case 7: ImGui::TextUnformatted(row.part.c_str()); break;
            case 8: ImGui::TextColored(C4(t.text_dim), "%s", row.value.c_str()); break;
            case 9: if (row.dnp) ImGui::TextColored(C4(t.warn), "DNP"); break;
            }
        });
        gui::section_header("Issues");
        std::vector<std::pair<int, std::string>> is;
        for (auto& x : prev.issues) is.push_back({x.line, x.msg});
        draw_issues(is, w, S(100));
    };

    auto body = [&](Form& form) {
        const gui::GTheme& t = th();
        wizard_chrome(maximize, first, ImVec2(S(1180), S(780)));
        {
            static const char* names[] = {"Source", "Mapping", "Review & Import"};
            static const char* tips[] = {"Step 1: rows, encoding and delimiters (auto-detected)", "Step 2: what each column means (auto-mapped) and how sides are read",
                                         "Step 3: check the placements, then Import"};
            wizard_stepper(names, tips, 3, stage, prev.mapping_ok(), [&](int i) { return (i == 1 && (!prev.mapping_ok() || prev.sides.unknown > 0)) || (i == 2 && !prev.issues.empty()); });
        }
        if (!fusion_mode && import_kind_switch(1) == 0) { switch_to = 0; form.close(false); }
        if (!fusion_mode) ImGui::SameLine(0, S(16));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(C4(t.text_dim), "Into the project:");
        ImGui::SameLine();
        {
            static const char* modes[] = {"Replace all placements", "Add / update (merge)", "Reference Fusion (attach refs by position)"};
            static const char* mt[] = {"Removes the current placements and imports this file (one undo step).",
                                       "Updates placements with the same reference and adds the new ones.",
                                       "Keeps the placements; attaches the file's references to the nearest existing placement."};
            int m = mode_i;
            ImGui::SetNextItemWidth(S(260));
            if (ImGui::BeginCombo("##pnp_into", modes[m])) {
                for (int i = 0; i < 3; ++i) { if (ImGui::Selectable(modes[i], m == i)) m = i; gui::tooltip(mt[i]); }
                ImGui::EndCombo();
            }
            gui::tooltip(mt[m]);
            if (m != mode_i) { mode_i = m; apply_opts(); }
        }
        ImGui::SameLine(0, S(10));
        ImGui::TextColored(C4(!d.prj.comps.empty() && mode_i == 0 ? t.warn : t.text_faint), "%s",
                           d.prj.comps.empty() ? "project has no placements" : (std::to_string(d.prj.comps.size()) + (mode_i == 0 ? " placements will be replaced" : " placements in the project")).c_str());
        if (!ui.tpl_status.empty()) { ImGui::SameLine(0, S(14)); ImGui::TextColored(C4(t.ok), "%s", ui.tpl_status.c_str()); }
        form.ok_text(stage < 2 ? L"Next \x2192" : L"Import");
        ImGui::Separator();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = std::max(S(220), ImGui::GetContentRegionAvail().y - S(2));
        if (ImGui::BeginChild("##pnpflow", ImVec2(w, h), ImGuiChildFlags_None)) {
            const float cw = ImGui::GetContentRegionAvail().x, ch = ImGui::GetContentRegionAvail().y;
            if (stage == 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
                if (ImGui::Selectable(adv_split ? "Delimiters / columns (auto-detected) \xE2\x96\xBE" : "Delimiters / columns (auto-detected) \xE2\x96\xB8", false, 0, ImVec2(S(320), 0))) adv_split = !adv_split;
                ImGui::PopStyleColor();
                ImGui::SameLine(0, S(12));
                ImGui::TextColored(C4(t.text_faint), "%s", ui.text->describe().substr(0, 80).c_str());
                if (adv_split) {
                    if (ImGui::BeginChild("##split", ImVec2(cw, S(300)), ImGuiChildFlags_Borders)) step_split(ui, ImGui::GetContentRegionAvail().x, S(290));
                    ImGui::EndChild();
                }
                step_source(ui, cw, ch - ImGui::GetCursorPosY());
            } else if (stage == 1) {
                if (!prev.mapping_ok()) {
                    gui::icon_image(Icon::Warning, ImGui::GetFontSize());
                    ImGui::SameLine();
                    ImGui::TextColored(C4(t.error), "%s", ui.mapping_hint);
                } else ImGui::TextColored(C4(t.text_dim), "Columns were mapped automatically. Required: Reference, X, Y.  Optional: Rotation, Side, Package, Part Number, Value (component metadata only).");
                const float side_h = S(250);
                const float gh = std::max(S(150), ch - ImGui::GetCursorPosY() - side_h - (adv_opts ? S(150) : 0) - (mode_i == 2 ? S(70) : 0) - ImGui::GetFrameHeightWithSpacing() - S(8));
                draw_grid(ui, cw, gh, true);
                gui::section_header("Side mapping");
                side_mapping_panel(opt, prev, ui, dside, std::min(cw, S(620)));
                if (mode_i == 2) { gui::section_header("Reference Fusion"); fusion_opts(cw); }
                ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
                if (ImGui::Selectable(adv_opts ? "Advanced: units, bottom-side conventions, mirror \xE2\x96\xBE" : "Advanced: units, bottom-side conventions, mirror \xE2\x96\xB8", false, 0, ImVec2(S(420), 0))) adv_opts = !adv_opts;
                ImGui::PopStyleColor();
                if (adv_opts) advanced(cw);
            } else review(cw, ch);
        }
        ImGui::EndChild();
    };

    const wchar_t* title = fusion_mode ? L"Reference Fusion" : L"Import Centroid / Pick-and-Place";
    std::wstring fname = path;
    if (size_t k = fname.find_last_of(L"\\/"); k != std::wstring::npos) fname = fname.substr(k + 1);
    Form f(title, 1180);
    f.icon((int)(fusion_mode ? Icon::Fusion : Icon::ImportCentroid)).header(title).subtitle(fname);
    f.custom("pnp", [&](Form& ff, float) { body(ff); });
    f.extra_button(L"\x2190 Back", [&](Form&) { stage = std::max(0, stage - 1); return false; });
    f.extra_button(L"Edit source\x2026", [&](Form& ff) { edit_src = true; ff.close(false); return false; });
    f.extras_right();
    f.ok_text(L"Next \x2192");
    f.validate([&](Form&, std::wstring& e) {
        commit();
        ui.rebuild();
        if (!prev.mapping_ok()) { stage = 1; e = L"Assign the Reference, X and Y columns first."; return false; }
        if (stage < 2) { stage++; e.clear(); return false; }   // OK = Next until the Review step
        if (prev.sides.unknown > 0 &&
            !message(owner, L"Centroid Import", std::to_wstring(prev.sides.unknown) + L" placement(s) have a side value that was not understood and go on the default side.\nImport anyway? (Mapping step: set them in the side table.)", true)) { stage = 1; e.clear(); return false; }
        if (opt.apply.mode == io::CentroidMode::Replace && !d.prj.comps.empty() &&
            !message(owner, L"Centroid Import", L"Replace all " + std::to_wstring(d.prj.comps.size()) + L" existing placements?\nThe current placements are removed; Undo restores them.", true)) { e.clear(); return false; }
        return true;
    });
    if (!f.run(owner)) {
        if (edit_src) { std::wstring pp = path; gui::defer([pp]() { gui::files_open(pp); }); }
        if (switch_to == 0) {   // same file in the BOM wizard
            gui::set_import_source(ui.bytes, std::max(0, src_start));
            DocPtr dp = docptr_of(d);
            std::wstring p = path;
            gui::defer([owner, dp, p]() { if (dp && dlg_import_bom(owner, *dp, p)) dp->changed(ui::CH_ALL); });
        }
        return false;
    }
    commit();
    ui.rebuild();

    Project tmp = d.prj;
    io::CentroidReport rep;
    err.clear();
    if (!io::pnp_apply(prev, tmp, opt, rep, err)) {
        std::wstring t = L"Import failed: " + W(err);
        if (opt.apply.mode == io::CentroidMode::Fusion && !opt.apply.key_ref.empty() && err.find("Key") != std::string::npos)
            t = L"Key Component Not Found in ASCII File.  Cannot Continue.\n\n" + W(err);
        warning(owner, L"Centroid Import", t);
        return false;
    }
    d.checkpoint(opt.apply.mode == io::CentroidMode::Fusion ? "Reference fusion" : "Import centroid");
    d.prj = std::move(tmp);
    if (d.prj.info.original_filename.empty()) d.prj.info.original_filename = U(path);
    d.changed(CH_ALL);
    settings().dir_centroid = W(file_dir(U(path)));
    std::wstring t = W(rep.describe());
    if (!rep.unmatched_refs.empty()) {
        t += L"\n\nUnmatched file references:\n";
        for (size_t i = 0; i < rep.unmatched_refs.size(); ++i) t += W(rep.unmatched_refs[i]) + ((i % 12 == 11) ? L"\n" : L" ");
    }
    if (!rep.messages.empty()) {
        t += L"\n\nMessages:\n";
        for (size_t i = 0; i < rep.messages.size() && i < 300; ++i) t += W(rep.messages[i]) + L"\n";
    }
    set_status(opt.apply.mode == io::CentroidMode::Fusion ? std::to_wstring(rep.fused) + L" references fused" : std::to_wstring(rep.imported) + L" placements imported");
    gui::toast(opt.apply.mode == io::CentroidMode::Fusion ? std::to_string(rep.fused) + " references fused" : std::to_string(rep.imported) + " placements imported",
               rep.messages.empty() ? gui::ToastKind::Success : gui::ToastKind::Warning);
    show_text(owner, fusion_mode ? L"Reference Fusion Report" : L"Centroid Import Report", t);
    after_import(d);
    return true;
}

// ===========================================================================
// BOM import (#166)
// ===========================================================================
// LCSC offers a signed official API; until configured, this opt-in fallback
// extracts the product JSON embedded in LCSC's public web page.  The lookup is
// best-effort (not an availability or sourcing guarantee), and can fail if the
// website changes, returns a challenge, or disallows automated requests.
struct BomWebTask {
    std::atomic<int> attempted{0};
    std::atomic<bool> done{false};
    int total = 0;
    std::mutex mutex;
    std::map<std::string, io::BomLookup> found;
    std::vector<std::string> failures;
};

static bool bom_lcsc_web_lookup(const std::string& code, io::BomLookup& fact, std::string& err) {
    if (code.size() < 2 || code.size() > 14 || code[0] != 'C' ||
        !std::all_of(code.begin() + 1, code.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        err = "Invalid LCSC identifier"; return false;
    }
    struct HttpHandle { HINTERNET h = nullptr; ~HttpHandle() { if (h) WinHttpCloseHandle(h); } };
    HttpHandle session, conn, req;
    session.h = WinHttpOpen(L"PCBPlotter/24 (manual BOM lookup)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) { err = "Cannot open Windows HTTP session"; return false; }
    WinHttpSetTimeouts(session.h, 6000, 6000, 8000, 12000);
    conn.h = WinHttpConnect(session.h, L"www.lcsc.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn.h) { err = "Cannot connect to LCSC"; return false; }
    const std::wstring path = L"/product-detail/" + ui::W(code) + L".html";
    req.h = WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req.h || !WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                                     0, 0, 0) || !WinHttpReceiveResponse(req.h, nullptr)) {
        err = "LCSC request failed or timed out"; return false;
    }
    DWORD status = 0, n = sizeof status;
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &n, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        err = "LCSC HTTP " + std::to_string(status); return false;
    }
    std::string html;
    char buf[16384];
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf, sizeof buf, &got)) { err = "LCSC response interrupted"; return false; }
        if (!got) break;
        if (html.size() + got > 4 * 1024 * 1024) { err = "LCSC response exceeds 4 MiB"; return false; }
        html.append(buf, got);
    }
    // Locate the JSON script, handling other attributes/order on the <script> element.
    size_t tag = 0, start = std::string::npos;
    while ((tag = html.find("<script", tag)) != std::string::npos) {
        const size_t gt = html.find('>', tag);
        if (gt == std::string::npos) break;
        const std::string open = html.substr(tag, gt - tag);
        if (open.find("__NEXT_DATA__") != std::string::npos) { start = gt + 1; break; }
        tag = gt + 1;
    }
    if (start == std::string::npos) { err = "Embedded LCSC product JSON not found"; return false; }
    const size_t end = html.find("</script>", start);
    if (end == std::string::npos) { err = "Product JSON truncated"; return false; }
    json::Value page;
    if (!json::Value::parse(html.substr(start, end - start), page, err)) return false;
    const json::Value& data = page["props"]["pageProps"]["webData"];
    if (!data.is_object() || io::to_upper(data["productCode"].str()) != code) {
        err = "Product ID does not match the requested LCSC code"; return false;
    }
    fact.code = code;
    fact.manufacturer = data["brandNameEn"].str();
    fact.mpn = data["productModel"].str();
    fact.package = data["encapStandard"].str();
    fact.description = data["productIntroEn"].str();
    if (fact.description.empty()) fact.description = data["productDescription"].str();
    fact.datasheet = data["pdfUrl"].str();
    if (fact.mpn.empty() || fact.manufacturer.empty()) {
        err = "LCSC did not return a manufacturer and MPN"; return false;
    }
    return true;
}

// R12 (REVIEW_V11 §3): floating, resizable, maximisable wizard (BOM menu): Source -> Mapping -> Review & Import.
// All r10/r11 logic (row model with H/F/L markers, DNP / orphan handling, bounded reference expansion,
// undo history) is unchanged.
namespace {
struct BomFlow {
    Doc* doc = nullptr;
    std::wstring path;
    ImportWizard ui;
    io::BomSource src;
    io::BomOptions2 opt;
    io::BomPreview prev;
    io::BomWizardHistory hist;
    gui::BomReviewUi review_ui;
    int sheet_i = -1, stage = 0, view = 0, mode_i = 0, dnp_i = 0, drag_marker = -1;
    int switch_to = -1;          // R16c: 1 = the user switched to the centroid wizard
    int src_start = -1;          // editor source start line (in-memory bytes)
    std::shared_ptr<BomWebTask> web_task;
    std::vector<std::string> web_failures;
    bool show_catalog = false;
    bool adv_split = false, adv_opts = false;
    char tok[128] = {};
    std::set<std::string> board_refs;
    int board_n = 0;

    io::BomWizardState state() const { return io::BomWizardState{opt.rows, opt.layout.role_override}; }
    void commit_rows() { hist.push(state()); ui.reanalyze(); }
    void apply_state(const io::BomWizardState& s) { opt.rows = s.rows; opt.layout.role_override = s.roles; ui.reanalyze(); }
    bool spreadsheet() const { return src.kind != "text"; }
    void refresh_board() {
        board_refs.clear(); board_n = 0;
        for (auto& c : doc->prj.comps) if (c.feature == Feature::Component) { ++board_n; board_refs.insert(io::to_upper(c.ref)); }
    }
    bool init(Doc& d, const std::wstring& p, std::string& err);
    void draw_rows(float w, float h);
    void draw_options(float w);
    void draw_review(float w, float h);
    bool do_import();
};

bool BomFlow::init(Doc& d, const std::wstring& p, std::string& err) {
    doc = &d; path = p;
    if (gui::take_import_source(ui.bytes, src_start)) {   // editor source (sheet / text editor): bytes in memory, no temp file
        src.kind = "text"; src.text = ui.bytes;
    } else if (!io::bom_load_source(U(path), src, err)) return false;   // .xlsx / .xls / .ods / text, opened directly
    else ui.bytes = src.text;
    ui.purpose = io::Purpose::Bom;
    ui.layout = &opt.layout;
    ui.text = &prev.text;
    for (size_t i = 0; i < src.sheets.size(); ++i) if (src.sheets[i] == src.sheet) sheet_i = (int)i;
    ui.reanalyze = [this]() { std::string e; prev = io::BomPreview{}; io::bom_preview_src(src, opt, prev, e); };
    ui.rebuild = [this]() { io::bom_rebuild(prev, opt); hist.push(state()); };
    ui.mapping_ok = [this]() { return prev.mapping_ok(); };
    ui.issue_count = [this]() { int n = 0; for (auto& i : prev.issues) n += i.sev != io::IssueSev::Info; return n; };
    ui.mapping_hint = "Map at least Reference and a part number column (Part Number or MPN).";
    if (src_start > 0) opt.rows.header_row = src_start;
    ui.sync_buffers();
    ui.reanalyze();
    // Re-use verified metadata already in this project's parts.  Project::parts
    // persists in the existing .pcbp JSON; no SQL database or extra cache.
    if (prev.profile == io::BomProfile::KiCadJlcpcb) {
        for (const auto& [part_id, part] : d.prj.parts) {
            if (part_id.rfind("LCSC:", 0) != 0 || part.vend_pn.empty() ||
                part.manufacturer.empty() || part.manf_pn.empty()) continue;
            const std::string code = io::to_upper(part.vend_pn);
            if (code.size() > 1 && code[0] == 'C' &&
                std::all_of(code.begin() + 1, code.end(), [](char c) { return c >= '0' && c <= '9'; }))
                opt.verified_lookups[code] = {code, part.manufacturer, part.manf_pn, part.package, part.description, ""};
        }
        if (!opt.verified_lookups.empty()) ui.rebuild();
    }
    auto_match_template(ui);
    ui.sync_buffers();
    hist.reset_to(state());
    if (const char* e = getenv("PCBP_TEST_IMPORT_STEP")) stage = std::clamp(atoi(e), 0, 2);   // automation / screenshots
    if (int hint = gui::take_import_mode_hint(); hint >= 0 && hint <= 2) {   // spreadsheet "Send to Project": Import = Replace, Update = Merge
        mode_i = hint;
        opt.link = mode_i == 0 ? io::BomLinkMode::Link : mode_i == 1 ? io::BomLinkMode::Relink : io::BomLinkMode::PartsOnly;
        opt.replace_parts = mode_i == 1;
        ui.rebuild();
    }
    { std::string s; for (auto& x : opt.dnp_tokens) { if (!s.empty()) s += ", "; s += x; } std::snprintf(tok, sizeof tok, "%s", s.c_str()); }
    refresh_board();
    return true;
}

void BomFlow::draw_rows(float w, float h) {
        const gui::GTheme& t = th();
        io::BomRowModel& M = opt.rows;
        const io::ParsedText& T = prev.text;
        const int n = (int)T.lines.size();
        if (src.sheets.size() > 1) {
            dim_label("Worksheet");
            ImGui::SetNextItemWidth(S(200));
            if (ImGui::BeginCombo("##sheet", src.sheet.c_str())) {
                for (int i = 0; i < (int)src.sheets.size(); ++i)
                    if (ImGui::Selectable(src.sheets[i].c_str(), i == sheet_i)) {
                        std::string e;
                        io::BomSource s2;
                        if (io::bom_load_source(U(path), s2, e, i)) { src = std::move(s2); sheet_i = i; ui.bytes = src.text; opt.rows = {}; hist.reset_to(state()); ui.reanalyze(); }
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine(0, S(20));
        }
        auto line_field = [&](const char* lbl, const char* id, int& v, int eff, const char* tip) {
            dim_label(lbl);
            int x = v >= 0 ? v + 1 : 0;
            ImGui::SetNextItemWidth(S(92));
            bool ch = ImGui::InputInt(id, &x, 1, 10);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n0 = automatic.", tip);
            if (v < 0) faint("%s", eff >= 0 ? "(auto: " + std::to_string(eff + 1) + ")" : std::string("(auto)"));
            if (ch) { v = x <= 0 ? -1 : std::min(x - 1, std::max(0, n - 1)); commit_rows(); }
            ImGui::SameLine(0, S(18));
        };
        line_field("Header Row", "##hr", M.header_row, prev.effective_header, "Line holding the column titles (Designator, Part Number, ...).");
        line_field("First Data Row", "##fr", M.first_data, prev.effective_first, "First line that can hold BOM records (title block above it is ignored).");
        line_field("Last Data Row", "##lr", M.last_data, prev.effective_last, "Last line that can hold BOM records (footer below it is ignored).");
        ImGui::NewLine();
        bool ae = M.auto_exclude;
        if (ImGui::Checkbox("Skip category / blank / note / footer rows", &ae)) { M.auto_exclude = ae; commit_rows(); }
        ImGui::SameLine(0, S(18));
        ImGui::BeginDisabled(!hist.can_undo());
        if (gui::button("Undo", Icon::Undo)) apply_state(hist.undo());
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Undo the last row or column change in this wizard");
        ImGui::SameLine(0, S(4));
        ImGui::BeginDisabled(!hist.can_redo());
        if (gui::button("Redo", Icon::Redo)) apply_state(hist.redo());
        ImGui::EndDisabled();
        ImGui::SameLine(0, S(4));
        if (gui::button("Reset", Icon::Undo)) apply_state(hist.reset());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back to the automatic detection (can be undone)");
        ImGui::SameLine(0, S(18));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(C4(t.text_faint), "%d records \xC2\xB7 click a row to exclude / include \xC2\xB7 right-click: Set as... \xC2\xB7 drag H / F / L",
                           prev.included_rows());
        const float top = ImGui::GetCursorPosY();
        const float lw = w, ph = h - top - S(6);   // R11: single full-width row view (no duplicate Included Records table)
        // ---- raw rows with markers + reason badges ----
        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.field);
        if (ImGui::BeginChild("##bomrows", ImVec2(lw, ph), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGui::PushFont(gui::g_font_mono);
            const float lh = ImGui::GetTextLineHeightWithSpacing();
            const float gut = ImGui::CalcTextSize("00000").x + S(8), mk = S(40), bw = S(52);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            int hover_line = -1;
            ImGuiListClipper clip;
            clip.Begin(n, lh);
            while (clip.Step()) {
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                    ImGui::PushID(i);
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    const io::BomRowStatus* st = prev.status_of_line(i);
                    const io::RowReason why = st ? st->reason : io::RowReason::Blank;
                    const bool inc = st && st->included;
                    float tw = ImGui::CalcTextSize(T.lines[i].c_str()).x;
                    ImGui::Selectable("##r", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(std::max(lw, gut + mk + bw + tw + S(20)), lh - ImGui::GetStyle().ItemSpacing.y));
                    if (ImGui::IsItemHovered()) hover_line = i;
                    const bool on_marker = ImGui::GetIO().MousePos.x < p.x + gut + mk && ImGui::GetIO().MousePos.x > p.x + gut;
                    int which = i == prev.effective_header ? 0 : i == prev.effective_first ? 1 : i == prev.effective_last ? 2 : -1;
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                        if (on_marker && which >= 0) drag_marker = which;
                        else if (why != io::RowReason::Header && why != io::RowReason::BeforeFirst && why != io::RowReason::AfterLast) {
                            // explicit toggle: included -> excluded; excluded (auto or by you) -> included
                            if (inc) { M.included.erase(i); M.excluded.insert(i); }
                            else if (M.excluded.count(i)) M.excluded.erase(i);
                            else M.included.insert(i);
                            commit_rows();
                        }
                    }
                    if (ImGui::IsItemHovered() && drag_marker < 0)
                        ImGui::SetTooltip("Line %d: %s\nClick: %s", i + 1, io::row_reason_name(why), inc ? "exclude this row" : "include this row");
                    if (ImGui::BeginPopupContextItem("##ctx")) {
                        ImGui::PushFont(gui::g_font);
                        if (ImGui::MenuItem("Set as Header Row")) { M.header_row = i; if (M.first_data >= 0 && M.first_data <= i) M.first_data = -1; commit_rows(); }
                        if (ImGui::MenuItem("Set as First Data Row")) { M.first_data = i; commit_rows(); }
                        if (ImGui::MenuItem("Set as Last Data Row")) { M.last_data = i; commit_rows(); }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Exclude this row", nullptr, false, inc)) { M.included.erase(i); M.excluded.insert(i); commit_rows(); }
                        if (ImGui::MenuItem("Include this row", nullptr, false, !inc)) { M.excluded.erase(i); M.included.insert(i); commit_rows(); }
                        if (ImGui::MenuItem("Automatic for this row", nullptr, false, M.excluded.count(i) || M.included.count(i))) { M.excluded.erase(i); M.included.erase(i); commit_rows(); }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Last Data Row: automatic", nullptr, false, M.last_data >= 0)) { M.last_data = -1; commit_rows(); }
                        ImGui::PopFont();
                        ImGui::EndPopup();
                    }
                    // background + text colour by state
                    ImU32 tc = inc ? t.text : t.text_faint;
                    if (why == io::RowReason::Header) { tc = t.accent; dl->AddRectFilled(p, ImVec2(p.x + lw + tw, p.y + lh - S(1)), alpha(t.accent, 26)); }
                    else if (inc) dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + S(3), p.y + lh), t.ok);
                    char num[16];
                    std::snprintf(num, sizeof num, "%5d", i + 1);
                    dl->AddText(ImVec2(p.x + S(2), p.y), t.text_faint, num);
                    // markers
                    std::string m;
                    if (i == prev.effective_header) m += "H";
                    if (i == prev.effective_first) m += m.empty() ? "F" : " F";
                    if (i == prev.effective_last) m += m.empty() ? "L" : " L";
                    if (!m.empty()) {
                        ImVec2 a(p.x + gut, p.y + S(1)), b(p.x + gut + ImGui::CalcTextSize(m.c_str()).x + S(8), p.y + lh - S(2));
                        dl->AddRectFilled(a, b, alpha(t.accent, 70), S(3));
                        dl->AddText(ImVec2(a.x + S(4), p.y), t.text, m.c_str());
                    }
                    const char* badge = io::row_reason_badge(why);
                    if (*badge && why != io::RowReason::Header) {
                        bool user = why == io::RowReason::UserExcluded || why == io::RowReason::ForcedIn;
                        dl->AddText(ImVec2(p.x + gut + mk, p.y), user ? t.warn : alpha(t.text_faint, 200), badge);
                    }
                    dl->AddText(ImVec2(p.x + gut + mk + bw, p.y), tc, T.lines[i].c_str());
                    if (drag_marker >= 0 && hover_line == i) dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + lw, p.y), t.accent, S(2));
                    ImGui::PopID();
                }
            }
            if (drag_marker >= 0) {
                ImGui::SetTooltip("Drop on the new %s", drag_marker == 0 ? "Header Row" : drag_marker == 1 ? "First Data Row" : "Last Data Row");
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    if (hover_line >= 0) {
                        int& tgt = drag_marker == 0 ? M.header_row : drag_marker == 1 ? M.first_data : M.last_data;
                        int cur = drag_marker == 0 ? prev.effective_header : drag_marker == 1 ? prev.effective_first : prev.effective_last;
                        if (hover_line != cur) { tgt = hover_line; commit_rows(); }
                    }
                    drag_marker = -1;
                }
            }
            ImGui::PopFont();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
}

void BomFlow::draw_options(float w) {
        bool ch = false;
        const float c1 = 0, c2 = w * 0.5f + S(8), ww = w * 0.5f - S(170);
        opt_label("Import mode", c1);
        ch |= opt_combo("##mode", mode_i, {"Merge \xE2\x80\x94 update parts, link by reference", "Replace \xE2\x80\x94 clear BOM and re-link every component", "Parts library only (no component links)"}, ww);
        opt_label("DNP handling", c2);
        ch |= opt_combo("##dnp", dnp_i, {"DNP: keep on board, exclude, no part number (recommended)", "DNP: leave components unchanged", "Ignore DNP indicators"}, ww);
        ImGui::SetCursorPosX(c1);
        ImGui::BeginGroup();
        ch |= ImGui::Checkbox("Expand references (e.g. R1-R5)", &opt.expand_refs);
        gui::tooltip("C1-C4 becomes C1, C2, C3, C4. Ranges are limited to 2000 references; bad ranges are reported, never guessed.");
        ch |= ImGui::Checkbox("Upper-case references", &opt.upper_refs);
        ch |= ImGui::Checkbox("Multiple P/N check (keep first P/N per reference)", &opt.multiple_pn_check);
        ch |= ImGui::Checkbox("Check Quantity against the reference count", &opt.qty_check);
        ImGui::EndGroup();
        ImGui::SameLine(c2);
        ImGui::BeginGroup();
        ch |= ImGui::Checkbox("DNP / DNI / DNS / NS in value or description marks the line DNP", &opt.dnp_from_text);
        ch |= ImGui::Checkbox("No P/N column: use the MPN, else value + package", &opt.synth_pn);
        ch |= ImGui::Checkbox("Replace component packages that disagree with the BOM", &opt.overwrite_package);
        gui::tooltip("Off: a disagreement is only reported. Components without a package always take the BOM package.");
        ch |= ImGui::Checkbox("Commit BOM packages to the part library", &opt.commit_packages);
        gui::tooltip("Write each part number's BOM package into the part library. Off by default: the BOM package is kept per component for comparison only.");
        ImGui::EndGroup();
        if (!tok[0]) { std::string s; for (auto& x : opt.dnp_tokens) { if (!s.empty()) s += ", "; s += x; } std::snprintf(tok, sizeof tok, "%s", s.c_str()); }
        opt_label("Not-populated indicators", c1);
        ImGui::SetNextItemWidth(w - S(200));
        if (ImGui::InputText("##dnptok", tok, sizeof tok)) {
            opt.dnp_tokens.clear();
            std::string cur;
            for (const char* q = tok;; ++q) {
                if (*q == ',' || *q == ';' || !*q) { std::string x = io::trim(cur); if (!x.empty()) opt.dnp_tokens.push_back(x); cur.clear(); if (!*q) break; }
                else cur += *q;
            }
            ch = true;
        }
        gui::tooltip("Comma-separated. Defaults: DNP (do not populate), DNI (do not install), DNS (do not stuff), NS (no stuff).\nAdd others (e.g. DNF, NF) only when your customer confirms their meaning.");
        if (ch) {
            opt.link = mode_i == 0 ? io::BomLinkMode::Link : mode_i == 1 ? io::BomLinkMode::Relink : io::BomLinkMode::PartsOnly;
            opt.replace_parts = mode_i == 1;
            opt.dnp = dnp_i == 0 ? io::DnpMode::Mark : dnp_i == 1 ? io::DnpMode::Skip : io::DnpMode::Ignore;
            ui.rebuild();
        }
}

void BomFlow::draw_review(float w, float h) {   // R14d: gui/bom_review_ui.cpp (sortable / filterable table, grouped summary, report)
    if (web_task && web_task->done.load()) {
        {
            std::lock_guard<std::mutex> lock(web_task->mutex);
            for (const auto& [code, record] : web_task->found) opt.verified_lookups[code] = record;
            web_failures = web_task->failures;
        }
        web_task.reset();
        ui.rebuild();  // update preview with the verified facts before Import
    }
    const float review_y = ImGui::GetCursorPosY();
    if (prev.profile == io::BomProfile::KiCadJlcpcb) {
        ImGui::TextUnformatted("Detected: KiCad / JLCPCB assembly BOM (LCSC supplier codes)");
        ImGui::SameLine(0, S(18));
        if (web_task) {
            ImGui::Text("Supplier lookups: %d / %d", web_task->attempted.load(), web_task->total);
        } else if (gui::button("Complete missing BOM fields online", Icon::ImportBom)) {
            std::set<std::string> missing;
            for (const auto& b : prev.lines) {
                const std::string& c = b.vendor_pn;
                if (c.size() < 2 || c.size() > 14 || c[0] != 'C' ||
                    !std::all_of(c.begin() + 1, c.end(), [](char x) { return x >= '0' && x <= '9'; })) continue;
                if (b.manufacturer.empty() || b.mpn.empty()) missing.insert(c);
            }
            if (missing.empty()) gui::toast("All LCSC parts already have manufacturer and MPN", gui::ToastKind::Info);
            else {
                auto task = std::make_shared<BomWebTask>();
                task->total = (int)missing.size();
                web_task = task;
                std::thread([task, codes = std::move(missing)]() {
                    for (const auto& code : codes) {
                        io::BomLookup f; std::string err;
                        bool ok = bom_lcsc_web_lookup(code, f, err);
                        {
                            std::lock_guard<std::mutex> lock(task->mutex);
                            if (ok) task->found[code] = std::move(f);
                            else task->failures.push_back(code + ": " + err);
                        }
                        ++task->attempted;
                    }
                    task->done = true;
                }).detach();  // task owns its state if wizard closes
            }
        }
        gui::tooltip("Opt-in LCSC exact-code lookup. Never changes existing footprints. Public website may rate-limit or change; failed parts remain unresolved.");
        if (!opt.verified_lookups.empty()) {
            ImGui::SameLine(0, S(12));
            ImGui::TextDisabled("%zu known supplier part(s)", opt.verified_lookups.size());
        }
        ImGui::SameLine(0, S(12));
        if (gui::button("Export completed BOM CSV", Icon::Export)) {
            const std::string csv = io::bom_completed_csv(prev, opt);
            const std::wstring proposed = path + L".completed.csv";
            gui::defer([csv, proposed]() {
                std::wstring destination = proposed;
                if (!ui::save_file_dialog(frame_hwnd(), L"Save completed BOM", {{L"CSV BOM (*.csv)", L"*.csv"}}, L"csv", destination)) return;
                std::string err;
                if (write_file(ui::U(destination), csv.data(), csv.size(), err)) gui::toast("Completed BOM saved", gui::ToastKind::Success);
                else warning(frame_hwnd(), L"Save completed BOM", ui::W(err));
            });
        }
        gui::tooltip("Export a normal CSV containing original KiCad fields plus verified supplier metadata. Missing fields stay blank.");
        if (!opt.verified_lookups.empty()) {
            ImGui::SameLine(0, S(12));
            if (gui::button(show_catalog ? "Hide resolved details" : "Review resolved details")) show_catalog = !show_catalog;
        }
        if (show_catalog && !opt.verified_lookups.empty()) {
            if (ImGui::BeginChild("##lcsc_results", ImVec2(w, S(120)), ImGuiChildFlags_Borders)) {
                if (ImGui::BeginTable("##lcsc_results_table", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
                    ImGui::TableSetupColumn("LCSC", ImGuiTableColumnFlags_WidthFixed, S(110));
                    ImGui::TableSetupColumn("Manufacturer");
                    ImGui::TableSetupColumn("MPN");
                    ImGui::TableSetupColumn("Supplier package (not KiCad footprint)");
                    ImGui::TableHeadersRow();
                    for (const auto& [code, rec] : opt.verified_lookups) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(code.c_str());
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(rec.manufacturer.c_str());
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(rec.mpn.c_str());
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(rec.package.c_str());
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndChild();
        }
        if (!web_failures.empty()) {
            if (ImGui::TreeNode("##lcsc_unresolved", "Unresolved supplier lookups (%zu)", web_failures.size())) {
                for (auto& x : web_failures) ImGui::TextWrapped("%s", x.c_str());
                ImGui::TreePop();
            }
        }
    }
    std::string fname = U(path);
    if (size_t k = fname.find_last_of("\\/"); k != std::string::npos) fname = fname.substr(k + 1);
    std::string job = io::m1_job_id(prev.text);
    if (job.empty()) job = doc->prj.info.bom_number;
    gui::draw_bom_review(review_ui, prev, board_refs, fname + (src.sheet.empty() ? "" : " [" + src.sheet + "]"), job,
                         w, std::max(S(170), h - (ImGui::GetCursorPosY() - review_y)));
}

bool BomFlow::do_import() {
    io::bom_rebuild(prev, opt);
    if (!prev.mapping_ok()) { stage = 1; gui::toast("Map Reference and a part number column first.", gui::ToastKind::Warning, 4.f); return false; }
    Doc& d = *doc;
    std::string err;
    Project tmp = d.prj;
    io::BomReport rep;
    err.clear();
    if (!io::bom_apply(prev, tmp, opt, rep, err)) { warning(frame_hwnd(), L"BOM Import", L"Import failed: " + W(err)); return false; }
    // R17d (REVIEW_V16 §12): BOM no. left the Project Info UI; the M1 Job ID names the project when it has no name yet
    if (std::string job = io::m1_job_id(prev.text); !job.empty() && tmp.info.name.empty()) tmp.info.name = job;
    if (!prev.board_pn.empty() && tmp.info.pcb_id.empty()) tmp.info.pcb_id = prev.board_pn;   // bare-board line -> PCB ID
    // one atomic, visibly labelled undo step (REVIEW_V9 §19)
    std::string fname = U(path);
    if (size_t k = fname.find_last_of("\\/"); k != std::string::npos) fname = fname.substr(k + 1);
    d.checkpoint_import("Import BOM (" + fname + ")");
    d.prj = std::move(tmp);
    {   // R12 §3: make the BOM attachment visible (Data > BOM shows the file, sheet, time and mode)
        Project::SourceRef sr;
        sr.path = U(path);
        sr.importer = "bom";
        char tb[32]; std::time_t now = std::time(nullptr); std::strftime(tb, sizeof tb, "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
        sr.imported_at = tb;
        sr.settings["sheet"] = src.sheet;
        sr.settings["mode"] = mode_i == 0 ? "add" : mode_i == 1 ? "replace" : "parts-only";
        sr.summary = std::to_string(rep.linked) + " components linked, " + std::to_string(rep.parts_added) + " part numbers added, " + std::to_string(rep.parts_updated) + " updated";
        if (mode_i == 1) d.prj.sources.erase(std::remove_if(d.prj.sources.begin(), d.prj.sources.end(), [](const Project::SourceRef& x) { return x.importer == "bom"; }), d.prj.sources.end());
        d.prj.sources.push_back(sr);
    }
    d.changed(CH_ALL);
    if (path.find_first_of(L"\\/") != std::wstring::npos) settings().dir_bom = W(file_dir(U(path)));   // not for live M1 fetches

    std::wostringstream t;
    t << L"BOM: " << path << (src.sheet.empty() ? L"" : L"  [sheet " + W(src.sheet) + L"]") << L"\n\n";
    for (auto& s : io::bom_summary(prev, &rep))
        t << (s.sev == io::IssueSev::Error ? L"ERROR    " : s.sev == io::IssueSev::Warning ? L"WARNING  " : L"info     ") << W(s.title)
          << (s.detail.empty() ? L"" : L"\n         " + W(s.detail)) << L"\n";
    t << L"\nRows " << rep.rows << L", part numbers added " << rep.parts_added << L", updated " << rep.parts_updated << L"\n";
    auto list = [&](const wchar_t* what, const std::vector<std::string>& v) {
        if (v.empty()) return;
        t << L"\n" << what << L" (" << v.size() << L"):\n";
        for (size_t i = 0; i < v.size() && i < 2000; ++i) t << L"  " << W(v[i]) << L"\n";
    };
    list(L"BOM references not found on the board", rep.refs_not_on_board);
    list(L"Board components not in the BOM (orphans \x2014 not marked DNP)", rep.comps_not_in_bom);
    list(L"References listed more than once", rep.duplicate_refs);
    list(L"References listed with different parts", rep.pn_conflicts);
    list(L"Package disagreements (BOM value vs board package)", rep.package_mismatch);
    list(L"Line messages", rep.messages);
    set_status(std::to_wstring(rep.linked) + L" components linked to the BOM");
    bool bad = false;
    for (auto& s : io::bom_summary(prev, &rep)) bad |= s.sev != io::IssueSev::Info;
    // R14d: one flow, no surprise popup: land on the project BOM table; the full report is one click away
    std::wstring rtxt = t.str();
    std::string rv = io::bom_review_text(review_ui.review);
    if (DocPtr dp = docptr_of(d)) gui::activate_view(dp, gui::ViewKind::Bom);
    gui::toast(std::to_string(rep.linked) + " components linked to the BOM. Showing the project BOM" + (bad ? " (review findings: see report)" : ""),
               bad ? gui::ToastKind::Warning : gui::ToastKind::Success, 8.f, "Report", [rtxt, rv]() {
                   gui::defer([rtxt, rv]() { show_text(frame_hwnd(), L"BOM Import Report", W(rv) + L"\n\n" + rtxt); });   // never a modal inside a frame
               });
    return true;
}
} // namespace

// R12 (REVIEW_V11 §3): the BOM import is a floating, resizable, maximisable wizard again (ui::Form modal).
// Root cause of the R11 crash (§4): the in-tab workflow called do_import() -> show_text()/warning() from INSIDE
// the frame (bom_flow_draw ran during draw_main). Form::run() spins nested frames, so ImGui::NewFrame() ran in the
// middle of the outer frame; after the report was dismissed the outer frame resumed on a reset window stack
// (EndChild/End on destroyed state) and the flow erased itself through a deferred lambda -> crash. Now the wizard
// is a modal opened from a command (outside any frame) and the import runs after Form::run() returns.
namespace {
void draw_bom_body(BomFlow& F, ui::Form& form, bool& maximize, bool& first) {
    const gui::GTheme& t = th();
    F.refresh_board();
    wizard_chrome(maximize, first, ImVec2(S(1180), S(780)));
    {   // ---- stepper: 1 Source -> 2 Mapping -> 3 Review (clickable to go back; forward only when mapping is OK)
        static const char* names[] = {"Source", "Mapping", "Review & Import"};
        static const char* tips[] = {"Step 1: which rows of the file are BOM lines", "Step 2: what each column means (auto-mapped)", "Step 3: check the result, then Import"};
        wizard_stepper(names, tips, 3, F.stage, F.prev.mapping_ok(), [&F](int i) { return (i == 1 && !F.prev.mapping_ok()) || (i == 2 && F.ui.issue_count() > 0); });
    }
    if (import_kind_switch(0) == 1) { F.switch_to = 1; form.close(false); }   // R16c (REVIEW_V15 §7): BOM vs centroid is explicit
    ImGui::SameLine(0, S(16));
    // compact "Into the project" choice (was three radio buttons)
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(C4(t.text_dim), "Into the project:");
    ImGui::SameLine();
    int m = F.mode_i;
    {
        static const char* modes[] = {"Add / update (merge)", "Replace project BOM", "Parts library only"};
        static const char* mtips[] = {"Keeps the existing part numbers and links; BOM lines update or add part numbers by reference.",
                                      "Clears the project's BOM (part numbers and every component link) and re-links from this file. One undo step.",
                                      "Adds / updates part numbers only; no component is linked."};
        ImGui::SetNextItemWidth(S(190));
        if (ImGui::BeginCombo("##bom_into", modes[m])) {
            for (int i = 0; i < 3; ++i) { if (ImGui::Selectable(modes[i], m == i)) m = i; gui::tooltip(mtips[i]); }
            ImGui::EndCombo();
        }
        gui::tooltip(mtips[m]);
    }
    if (m != F.mode_i) {
        F.mode_i = m;
        F.opt.link = m == 0 ? io::BomLinkMode::Link : m == 1 ? io::BomLinkMode::Relink : io::BomLinkMode::PartsOnly;
        F.opt.replace_parts = m == 1;
        F.ui.rebuild();
    }
    ImGui::SameLine(0, S(10));
    const int existing = (int)F.doc->prj.parts.size();
    ImGui::TextColored(C4(existing && m == 1 ? t.warn : t.text_faint), "%s", existing ? (std::to_string(existing) + (m == 1 ? " existing part numbers will be replaced" : " existing part numbers")).c_str() : "project BOM is empty");
    if (!F.ui.tpl_status.empty()) { ImGui::SameLine(0, S(14)); ImGui::TextColored(C4(t.ok), "%s", F.ui.tpl_status.c_str()); }   // R14d: "Detected: M1 BOM"
    // footer: [Back] [Next -> | Import] [Cancel] bottom-right; Import exists only on the Review step
    form.ok_text(F.stage < 2 ? L"Next \x2192" : L"Import");
    ImGui::Separator();
    const float w = ImGui::GetContentRegionAvail().x;
    // fill the window down to the form footer (Back / Next / Import / Cancel stay visible at any size)
    const float h = std::max(S(220), ImGui::GetContentRegionAvail().y - S(2));   // the form body child ends above the pinned footer
    if (ImGui::BeginChild("##bomflow", ImVec2(w, h), ImGuiChildFlags_None)) {
        const float cw = ImGui::GetContentRegionAvail().x, ch = ImGui::GetContentRegionAvail().y;
        if (F.stage == 0) {
            if (!F.spreadsheet()) {
                ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
                if (ImGui::Selectable(F.adv_split ? "Delimiters / columns (auto-detected) \xE2\x96\xBE" : "Delimiters / columns (auto-detected) \xE2\x96\xB8", false, 0, ImVec2(S(320), 0))) F.adv_split = !F.adv_split;
                ImGui::PopStyleColor();
                ImGui::SameLine(0, S(12));
                ImGui::TextColored(C4(t.text_faint), "%s", F.ui.text->describe().substr(0, 80).c_str());
                if (F.adv_split) {
                    if (ImGui::BeginChild("##split", ImVec2(cw, S(300)), ImGuiChildFlags_Borders)) step_split(F.ui, ImGui::GetContentRegionAvail().x, S(290));
                    ImGui::EndChild();
                }
            }
            F.draw_rows(cw, ch - ImGui::GetCursorPosY());
        } else if (F.stage == 1) {
            if (!F.prev.mapping_ok()) {
                gui::icon_image(Icon::Warning, ImGui::GetFontSize());
                ImGui::SameLine();
                ImGui::TextColored(C4(t.error), "%s", F.ui.mapping_hint);
            } else ImGui::TextColored(C4(t.text_dim), "Columns were mapped automatically. Change a column's role only where it is wrong.");
            const float gh = std::max(S(160), ch - ImGui::GetCursorPosY() - (F.adv_opts ? S(230) : ImGui::GetFrameHeightWithSpacing() + S(8)));
            draw_grid(F.ui, cw, gh, true);
            ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
            if (ImGui::Selectable(F.adv_opts ? "Advanced: DNP, transforms \xE2\x96\xBE" : "Advanced: DNP, transforms \xE2\x96\xB8", false, 0, ImVec2(S(360), 0))) F.adv_opts = !F.adv_opts;
            ImGui::PopStyleColor();
            if (F.adv_opts) F.draw_options(cw);
        } else F.draw_review(cw, ch);
    }
    ImGui::EndChild();
}
} // namespace

bool dlg_import_bom(HWND owner, Doc& d, const std::wstring& path) {
    if (gui::in_frame()) {   // never spin a modal inside a frame (R12 crash root cause): re-enter between frames
        DocPtr dp = docptr_of(d);
        std::wstring p = path;
        gui::defer([owner, dp, p]() { if (dp && dlg_import_bom(owner, *dp, p)) dp->changed(ui::CH_ALL); });
        return false;
    }
    auto F = std::make_unique<BomFlow>();
    std::string err;
    if (!F->init(d, path, err)) {
        warning(owner, L"BOM Import", L"Could not read " + path + L"\n\n" + W(err));
        return false;
    }
    bool maximize = false, first = true;
    std::wstring fname = path;
    if (size_t k = fname.find_last_of(L"\\/"); k != std::wstring::npos) fname = fname.substr(k + 1);
    Form f(L"Import Bill of Materials", 1180);
    f.icon((int)Icon::ImportBom).header(L"Import Bill of Materials").subtitle(fname + (F->src.sheet.empty() ? L"" : L"   [worksheet " + W(F->src.sheet) + L"]"));
    BomFlow* FP = F.get();
    f.custom("bom", [FP, &maximize, &first](Form& ff, float) {
        draw_bom_body(*FP, ff, maximize, first);
    });
    f.extra_button(L"\x2190 Back", [FP](Form&) { FP->stage = std::max(0, FP->stage - 1); return false; });
    f.extras_right();
    f.ok_text(L"Next \x2192");   // becomes "Import" on the Review step (draw_bom_body)
    f.validate([FP, owner, &d](Form&, std::wstring& e) {
        if (FP->web_task && !FP->web_task->done.load()) { e = L"Supplier lookups are still running. Finish the lookup or Cancel the wizard."; return false; }
        io::bom_rebuild(FP->prev, FP->opt);
        if (!FP->prev.mapping_ok()) { FP->stage = 1; e = W(FP->ui.mapping_hint); return false; }
        if (FP->stage < 2) { FP->stage++; e.clear(); return false; }   // OK = Next until the Review step
        if (FP->mode_i == 1 && !d.prj.parts.empty() &&
            !message(owner, L"BOM Import", L"Replace the project BOM?\n" + std::to_wstring(d.prj.parts.size()) + L" part numbers and every component link are replaced by this file. Undo restores them.", true)) { e.clear(); return false; }
        return true;
    });
    if (!f.run(owner)) {
        if (F->switch_to == 1) {   // R16c: same file in the centroid wizard
            if (!F->src.text.empty()) gui::set_import_source(F->src.text, std::max(0, F->src_start));   // spreadsheets: their TSV rendition
            DocPtr dp = docptr_of(d);
            std::wstring p = path;
            gui::defer([owner, dp, p]() { if (dp && dlg_import_centroid(owner, *dp, p, false)) dp->changed(ui::CH_ALL); });
        }
        return false;
    }
    // outside any frame: report / warning modals are safe here
    return F->do_import();
}

// R16c (REVIEW_V15 §7): drag-and-drop of a delimited text file routes to the right wizard by content.
// Content kind of a dropped delimited text file (Unknown: not a BOM / centroid table, or not a table extension).
static io::CsvKind dropped_table_kind(const std::wstring& path, std::string* why) {
    std::string e = file_ext_lower(U(path));
    static const char* exts[] = {".csv", ".tsv", ".txt", ".prn", ".pos", ".xy", ".mnt", ".cpl", ".bom", ".rep", ".asc"};
    if (std::none_of(std::begin(exts), std::end(exts), [&](const char* x) { return e == x; })) return io::CsvKind::Unknown;
    std::string bytes, err;
    if (!read_text_file(U(path), bytes, err) || bytes.size() > (32u << 20)) return io::CsvKind::Unknown;
    return io::sniff_csv_kind(bytes, why);
}
bool is_dropped_table(const std::wstring& path) { return dropped_table_kind(path, nullptr) != io::CsvKind::Unknown; }

bool route_dropped_table(HWND owner, Doc& d, const std::wstring& path) {
    std::string e = file_ext_lower(U(path));
    static const char* exts[] = {".csv", ".tsv", ".txt", ".prn", ".pos", ".xy", ".mnt", ".cpl", ".bom", ".rep", ".asc"};
    if (std::none_of(std::begin(exts), std::end(exts), [&](const char* x) { return e == x; })) return false;
    std::string bytes, err;
    if (!read_text_file(U(path), bytes, err) || bytes.size() > (32u << 20)) return false;
    std::string why;
    const io::CsvKind k = io::sniff_csv_kind(bytes, &why);
    if (k == io::CsvKind::Unknown) return false;
    std::wstring fname = path;
    if (size_t s = fname.find_last_of(L"\\/"); s != std::wstring::npos) fname = fname.substr(s + 1);
    gui::toast(U(fname) + ": detected " + io::csv_kind_name(k) + " (" + why + ")", gui::ToastKind::Info, 6.f);
    const bool ok = k == io::CsvKind::Bom ? dlg_import_bom(owner, d, path) : dlg_import_centroid(owner, d, path, false);
    if (ok) d.changed(CH_ALL);
    return true;
}

// ===========================================================================
// R16c (REVIEW_V15 §9): one Transform dialog. Replaces Global Offset/Rotation and Rescale Coordinates.
// Sections are independent (each has its own switch); order Scale -> Mirror -> Rotate -> Translate.
// ===========================================================================
bool dlg_transform(HWND owner, Doc& d) {
    if (gui::in_frame()) {
        DocPtr dp = docptr_of(d);
        gui::defer([owner, dp]() { if (dp) dlg_transform(owner, *dp); });
        return false;
    }
    Project& p = d.prj;
    TransformSpec sp;
    const BBox bb0 = p.board_bbox();
    int pivot_i = 0;            // 0 origin (0,0), 1 board centre, 2 virtual origin, 3 custom
    double cx = 0, cy = 0;      // custom pivot (display units)
    double dx_u = 0, dy_u = 0, sc = 1, scy = 1;
    int rot_i = 0;              // 0 90, 1 180, 2 270, 3 custom
    double ang = 0;
    bool unit_fix = false;
    const double upm = parse_len(L"1", 1);   // mm per display unit
    auto pivot = [&]() -> Vec2 {
        switch (pivot_i) {
        case 1: return bb0.center();
        case 2: return p.virtual_origin;
        case 3: return {cx * upm, cy * upm};
        }
        return {};
    };
    auto sync = [&]() {
        sp.dx = dx_u * upm; sp.dy = dy_u * upm;
        sp.angle = rot_i == 0 ? 90 : rot_i == 1 ? 180 : rot_i == 2 ? 270 : ang;
        sp.sx = sc; sp.sy = scy;
        sp.pivot = pivot();
    };
    Form f(L"Transform", 980);
    f.icon((int)Icon::Offset).header(L"Transform Placements & Outlines");
    f.subtitle(L"Enable only the sections you need. Applied in the order Scale \x2192 Mirror \x2192 Rotate \x2192 Translate; one undo step.");
    f.custom("tr", [&](Form&, float w) {
        const gui::GTheme& t = th();
        sync();
        const float lw = std::min(S(470), w * 0.52f), pw = w - lw - S(14);
        const std::string u = U(units_label());
        ImGui::BeginGroup();
        ImGui::PushItemWidth(S(120));
        auto section = [&](const char* id, const char* label, bool& on, const char* tip) {
            ImGui::PushFont(gui::g_font_bold);
            ImGui::Checkbox(id, &on);
            ImGui::PopFont();
            ImGui::SameLine(0, S(2));
            ImGui::TextColored(C4(on ? t.text : t.text_dim), "%s", label);
            gui::tooltip(tip);
        };
        section("##tr_t", "Translate (X / Y offset)", sp.translate, "Move everything by a fixed X / Y distance");
        ImGui::BeginDisabled(!sp.translate);
        ImGui::Indent(S(26));
        ImGui::InputDouble(("X (" + u + ")##dx").c_str(), &dx_u, 0, 0, "%.4f"); ImGui::SameLine(0, S(12));
        ImGui::InputDouble(("Y (" + u + ")##dy").c_str(), &dy_u, 0, 0, "%.4f");
        if (gui::button("Virtual origin \xE2\x86\x92 0,0", Icon::VirtualOrigin)) { dx_u = -p.virtual_origin.x / upm; dy_u = -p.virtual_origin.y / upm; }
        gui::tooltip("Fills the offset that moves the current virtual origin to 0,0 (the same as Rebase Coordinates to Origin).\n"
                     "Setting the origin (Assign Origin tool) only changes where coordinates are measured from;\nrebasing rewrites the stored coordinates so that point becomes 0,0.");
        ImGui::Unindent(S(26));
        ImGui::EndDisabled();
        ImGui::Spacing();
        section("##tr_r", "Rotate", sp.rotate, "Rotate about the centre point; placement angles turn with the board");
        ImGui::BeginDisabled(!sp.rotate);
        ImGui::Indent(S(26));
        ImGui::RadioButton("90\xC2\xB0##r0", &rot_i, 0); ImGui::SameLine(); ImGui::RadioButton("180\xC2\xB0##r1", &rot_i, 1); ImGui::SameLine();
        ImGui::RadioButton("270\xC2\xB0##r2", &rot_i, 2); ImGui::SameLine(); ImGui::RadioButton("Custom##r3", &rot_i, 3);
        if (rot_i == 3) { ImGui::SameLine(); ImGui::InputDouble("\xC2\xB0 CCW##ang", &ang, 0, 0, "%.3f"); }
        ImGui::Unindent(S(26));
        ImGui::EndDisabled();
        ImGui::Spacing();
        section("##tr_s", "Scale", sp.scale, "Multiply coordinates (about the centre point)");
        ImGui::BeginDisabled(!sp.scale);
        ImGui::Indent(S(26));
        ImGui::InputDouble(sp.uniform ? "factor##sx" : "X factor##sx", &sc, 0, 0, "%.6g");
        if (!sp.uniform) { ImGui::SameLine(0, S(12)); ImGui::InputDouble("Y factor##sy", &scy, 0, 0, "%.6g"); }
        ImGui::BeginDisabled(unit_fix);
        ImGui::Checkbox("Same factor for X and Y", &sp.uniform);
        ImGui::EndDisabled();
        if (unit_fix) sp.uniform = true;
        ImGui::Checkbox("Unit fix: also scale package sizes and Gerber layers (about 0,0)", &unit_fix);
        gui::tooltip("For data read in the wrong unit (e.g. 25.4 for inch read as mm). Scales the project's packages and Gerber layers too.\n"
                     "Without it only positions and outlines move (package graphics keep their size).");
        ImGui::TextColored(C4(t.text_faint), "inch\xE2\x86\x92mm 25.4 \xC2\xB7 mm\xE2\x86\x92inch 0.03937 \xC2\xB7 mil\xE2\x86\x92mm 0.0254");
        ImGui::Unindent(S(26));
        ImGui::EndDisabled();
        ImGui::Spacing();
        section("##tr_m", "Mirror", sp.mirror, "Reflect the coordinates (e.g. a centroid file exported viewed from the bottom)");
        ImGui::BeginDisabled(!sp.mirror);
        ImGui::Indent(S(26));
        ImGui::RadioButton("Mirror X (left \xE2\x86\x94 right)", &sp.mirror_axis, 0); ImGui::SameLine();
        ImGui::RadioButton("Mirror Y (top \xE2\x86\x94 bottom)", &sp.mirror_axis, 1);
        ImGui::Checkbox("Turn the board over (parts change side)", &sp.flip_side);
        gui::tooltip("Off: only the coordinates are reflected; angles follow the geometry, sides stay.\nOn: the board is flipped; every part moves to the other side (bottom-view angle convention).");
        ImGui::Unindent(S(26));
        ImGui::EndDisabled();
        ImGui::Spacing();
        gui::section_header("Centre point and scope");
        static const char* pv[] = {"Origin (0, 0)", "Board centre", "Virtual origin", "Custom point"};
        ImGui::SetNextItemWidth(S(160));
        ImGui::Combo("Rotate / scale / mirror about", &pivot_i, pv, 4);
        if (pivot_i == 3) { ImGui::InputDouble(("X (" + u + ")##cx").c_str(), &cx, 0, 0, "%.4f"); ImGui::SameLine(0, S(12)); ImGui::InputDouble(("Y (" + u + ")##cy").c_str(), &cy, 0, 0, "%.4f"); }
        static const char* sides[] = {"Both sides", "Top side only", "Bottom side only"};
        ImGui::SetNextItemWidth(S(160));
        ImGui::Combo("Placements", &sp.sides, sides, 3);
        ImGui::Checkbox("Board outline", &sp.board_outline); ImGui::SameLine(0, S(12));
        ImGui::Checkbox("Circuit outline(s)", &sp.circuit_outline); ImGui::SameLine(0, S(12));
        ImGui::Checkbox("Virtual origin", &sp.move_origin);
        ImGui::PopItemWidth();
        ImGui::EndGroup();
        // ---- live preview
        ImGui::SameLine(0, S(14));
        ImGui::BeginGroup();
        sync();
        const Xform T = transform_xform(sp);
        BBox all = bb0;
        if (all.empty()) all = BBox(0, 0, 10, 10);
        for (Vec2 c : {Vec2{bb0.x0, bb0.y0}, Vec2{bb0.x1, bb0.y0}, Vec2{bb0.x1, bb0.y1}, Vec2{bb0.x0, bb0.y1}}) all.add(T.apply(c));
        all.add(sp.pivot);
        const float ph = S(470);
        ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + pw, a.y + ph);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        panel_bg(dl, a, b);
        dl->PushClipRect(a, b, true);
        Fit fit(all, a, b, S(18));
        const ImVec2 o0 = fit({0, 0});
        dl->AddLine(ImVec2(o0.x - S(8), o0.y), ImVec2(o0.x + S(8), o0.y), t.text_faint);
        dl->AddLine(ImVec2(o0.x, o0.y - S(8)), ImVec2(o0.x, o0.y + S(8)), t.text_faint);
        // before: faint outline + dots
        auto poly = [&](const std::vector<Vec2>& pl, const Xform& X, ImU32 col, float th2) {
            std::vector<ImVec2> v;
            for (auto& q : pl) v.push_back(fit(X.apply(q)));
            if (v.size() > 1) dl->AddPolyline(v.data(), (int)v.size(), col, ImDrawFlags_Closed, th2);
        };
        const Xform I;
        for (auto& pl : p.panel_frame) poly(pl, I, alpha(t.text_faint, 110), 1.f);
        for (auto& pl : p.board_frame) poly(pl, I, alpha(t.text_faint, 140), 1.f);
        if (p.board_frame.empty()) poly({{bb0.x0, bb0.y0}, {bb0.x1, bb0.y0}, {bb0.x1, bb0.y1}, {bb0.x0, bb0.y1}}, I, alpha(t.text_faint, 140), 1.f);
        for (auto& c : p.comps) { ImVec2 s0 = fit(c.pos); dl->AddRectFilled(ImVec2(s0.x - 1.2f, s0.y - 1.2f), ImVec2(s0.x + 1.2f, s0.y + 1.2f), alpha(t.text_faint, 120)); }
        // after
        if (sp.any()) {
            if (sp.board_outline) for (auto& pl : p.panel_frame) poly(pl, T, alpha(t.accent, 200), 1.6f);
            if (sp.circuit_outline) for (auto& pl : p.board_frame) poly(pl, T, t.ok, 1.8f);
            if (p.board_frame.empty() && sp.circuit_outline) poly({{bb0.x0, bb0.y0}, {bb0.x1, bb0.y0}, {bb0.x1, bb0.y1}, {bb0.x0, bb0.y1}}, T, alpha(t.ok, 160), 1.2f);
            for (auto& c : p.comps) {
                const bool moves = sp.sides == 0 || (sp.sides == 1) == (c.side == Side::Top);
                Component k = c;
                if (moves) transform_component(k, T, sp.mirror && sp.flip_side);
                const ImVec2 s1 = fit(k.pos);
                const ImU32 col = !moves ? alpha(t.text_dim, 160) : k.side == Side::Bottom ? t.accent : t.warn;
                const float r = S(2.2f);
                dl->AddRectFilled(ImVec2(s1.x - r, s1.y - r), ImVec2(s1.x + r, s1.y + r), col);
                if (p.comps.size() < 400) {   // angle tick
                    const double ar = deg2rad(k.rot);
                    dl->AddLine(s1, ImVec2(s1.x + (float)std::cos(ar) * S(6), s1.y - (float)std::sin(ar) * S(6)), col, 1.f);
                }
            }
        }
        const ImVec2 pc = fit(sp.pivot);
        dl->AddCircle(pc, S(5), t.error, 0, 1.5f);
        dl->AddText(ImVec2(pc.x + S(7), pc.y - S(7)), t.error, "centre");
        dl->PopClipRect();
        ImGui::Dummy(ImVec2(pw, ph));
        ImGui::TextColored(C4(t.text_faint), "Grey = now  \xC2\xB7  coloured = after (orange top, blue bottom)  \xC2\xB7  red ring = centre point");
        if (sp.any() && !bb0.empty()) {
            const double fx = sp.scale ? sp.sx : 1, fy = sp.scale ? (sp.uniform ? sp.sx : sp.sy) : 1;
            ImGui::TextColored(C4(t.text_dim), "Board extent %.2f x %.2f mm%s", bb0.w(), bb0.h(),
                               sp.scale ? (" \xE2\x86\x92 " + io::fmt_fixed(bb0.w() * std::fabs(fx), 2) + " x " + io::fmt_fixed(bb0.h() * std::fabs(fy), 2) + " mm").c_str() : "");
        }
        ImGui::EndGroup();
    }, 560);
    f.ok_text(L"Apply");
    f.validate([&](Form&, std::wstring& e) {
        sync();
        if (!sp.any()) { e = L"Enable at least one section (Translate, Rotate, Scale or Mirror)."; return false; }
        if (sp.scale && (!(sp.sx > 0 || sp.sx < 0) || (!sp.uniform && sp.sy == 0))) { e = L"The scale factor must not be 0."; return false; }
        if (sp.scale && unit_fix && !(sp.sx > 0)) { e = L"A unit fix needs a positive factor."; return false; }
        return true;
    });
    if (!f.run(owner)) return false;
    sync();
    std::string lab = "Transform";
    if (sp.translate) lab += " move";
    if (sp.rotate) lab += " rotate " + io::fmt_fixed(sp.angle, 1);
    if (sp.scale) lab += " scale x" + io::fmt_fixed(sp.sx, 4);
    if (sp.mirror) lab += sp.mirror_axis ? " mirror Y" : " mirror X";
    std::vector<std::string> notes;
    int nl = 0;
    if (sp.scale && unit_fix) {
        d.checkpoint_import(lab);            // also snapshots the Gerber layers rewritten by the unit fix
        nl = io::rescale_project(d.prj, sp.sx, notes);
        d.parse_all_layers();
        sp.scale = false;
    } else d.checkpoint(lab);
    const int n = apply_transform(d.prj, sp);
    d.changed(CH_ALL);
    std::string msg = lab + ": " + std::to_string(n) + " placement(s)" + (nl ? ", " + std::to_string(nl) + " layer(s) rescaled" : std::string());
    gui::toast(msg, notes.empty() ? gui::ToastKind::Success : gui::ToastKind::Warning, 5.f, "Undo", [] { gui::post_command(ID_EDIT_UNDO); });
    if (!notes.empty()) {
        std::wstring t = W(msg) + L"\n\n";
        for (auto& x : notes) t += W(x) + L"\n";
        show_text(owner, L"Transform", t);
    }
    return true;
}

} // namespace pcbp::app
