// ============================================================================
// odb_import_dialog.cpp — ODB++ step chooser (LEAD_BRIEF §13).
//
// CircuitCAM shows a bare list of matrix STEP names when a job has more than
// one step; picking the panel step
// or a coupon silently yields nothing. Here every step is listed with what it
// actually contains, the importer's recommendation is preselected and the
// consequences of a choice are spelled out before importing.
// ============================================================================
#include "import_extras.h"
#include "../io/cad_common.h"
#include "../io/odbpp.h"
#include "../ui/form.h"
#include "gapp.h"
#include "imgui_internal.h"

#include <cstdio>

namespace pcbp { bool auto_link_smt_pair(Project& p); }   // R18d core/sides.cpp
namespace pcbp::app {
using namespace pcbp::ui;
using gui::S;
using gui::th;

bool dlg_odb_import(HWND owner, const std::wstring& path, Options& o) {
    io::OdbJobInfo info;
    std::string err;
    bool ok = false;
    gui::run_with_progress_wait("Reading ODB++ job", [&](gui::Progress& pr) { pr.text("Scanning steps and layers"); ok = io::odb_scan(U(path), info, err); },
                           gui::ProgressOpts{true, false, 0.15f});
    if (!ok) {
        message(owner, L"ODB++ Import", L"This ODB++ job cannot be read:\n\n" + W(err));
        return false;
    }
    // units: 0 as declared, 1 mm, 2 inch, 3 mil, 4 0.1 mm, 5 custom (mm per unit)
    struct St { int sel = 0; int mode = 0; bool layers = false; int unit = 0; double custom = 1.0; double scale = 1.0;
                bool replace = true, board = true, pkgs = true, fid = true; } st;
    st.replace = opt_bool(o, "replace", true); st.board = opt_bool(o, "import_board", true);
    st.pkgs = opt_bool(o, "import_packages", true); st.fid = opt_bool(o, "fiducial_by_name", true);   // mode 0 board, 1 images, 2 flat
    st.sel = std::max(0, info.recommended);
    st.mode = info.recommend_images ? 1 : 0;
    const auto& steps = info.steps;

    Form f(L"ODB++ Import", 760);
    f.icon((int)Icon::ImportCad).header(L"ODB++ Import \x2014 choose the step");
    f.subtitle(W(info.container + (info.root.empty() ? "" : "  \xC2\xB7  root " + info.root) + "  \xC2\xB7  " +
                 std::to_string(steps.size()) + " steps, " + std::to_string(info.layers.size()) + " layers" +
                 (info.compressed_members ? "  \xC2\xB7  " + std::to_string(info.compressed_members) + " compressed members" : "")));
    f.custom("steps", [&](Form& ff, float w) {
        const gui::GTheme& t = th();
        if (ImGui::BeginTable("odbsteps", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(w, std::min(S(150), ImGui::GetFrameHeight() * (float)(steps.size() + 1) + S(8))))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            for (const char* c : {"Step", "Type", "Size (mm)", "Top", "Bottom", "Layers", "Step-repeat"}) ImGui::TableSetupColumn(c);
            ImGui::TableHeadersRow();
            for (int i = 0; i < (int)steps.size(); ++i) {
                const auto& s = steps[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                std::string nm = s.name + (i == info.recommended ? "  (recommended)" : "");
                if (ImGui::Selectable(nm.c_str(), st.sel == i, ImGuiSelectableFlags_SpanAllColumns)) {
                    st.sel = i;
                    st.mode = (!s.repeats.empty() && s.comp_top + s.comp_bot == 0) ? 1 : 0;
                }
                ImGui::PopID();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(s.kind.c_str());
                ImGui::TableNextColumn();
                if (s.has_profile) ImGui::Text("%.2f x %.2f", s.w, s.h); else ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.warn), "no profile");
                ImGui::TableNextColumn(); ImGui::Text("%d", s.comp_top);
                ImGui::TableNextColumn(); ImGui::Text("%d", s.comp_bot);
                ImGui::TableNextColumn(); ImGui::Text("%d", s.layers_with_features);
                ImGui::TableNextColumn();
                if (s.repeats.empty()) ImGui::TextDisabled("\xE2\x80\x94");
                else {
                    std::string r;
                    for (auto& x : s.repeats) r += (r.empty() ? "" : ", ") + x.child + " " + std::to_string(x.nx) + "\xC3\x97" + std::to_string(x.ny);
                    ImGui::Text("%s (%d boards, %d comps)", r.c_str(), s.instances, s.leaf_comps);
                }
            }
            ImGui::EndTable();
        }
        const auto& s = steps[st.sel];
        ImGui::Spacing();
        if (!s.repeats.empty()) {
            ImGui::TextUnformatted("Panel step \xE2\x80\x94 import as:");
            ImGui::SameLine();
            ImGui::RadioButton("own data only", &st.mode, 0); ImGui::SameLine();
            ImGui::RadioButton("board + panel images", &st.mode, 1); ImGui::SameLine();
            ImGui::RadioButton("every board expanded", &st.mode, 2);
            gui::tooltip("Board + panel images: the child board is imported once and each step-repeat instance becomes a panel image (CircuitCAM Images).\n"
                         "Every board expanded: all instances become separate components (refs get _2, _3 ... suffixes).");
        }
        // what will happen
        std::string what;
        ImU32 col = t.text_dim;
        const int own = s.comp_top + s.comp_bot;
        if (!s.repeats.empty() && st.mode != 0) {
            what = "Imports the child board once with " + std::to_string(s.instances) + " panel images (" + std::to_string(s.leaf_comps) + " placements in total).";
            if (st.mode == 2) what = "Imports all " + std::to_string(s.leaf_comps) + " placements as separate components.";
        } else if (own > 0) {
            what = "Imports " + std::to_string(own) + " components (" + std::to_string(s.comp_top) + " top, " + std::to_string(s.comp_bot) + " bottom).";
        } else {
            col = t.warn;
            what = "This step has no component layers: only geometry (outline, copper, silk) will be imported.";
            for (auto& o2 : steps)
                if (o2.name != s.name && o2.leaf_comps > 0) { what += " Step '" + o2.name + "' has " + std::to_string(o2.leaf_comps) + " components."; break; }
        }
        for (auto& n : s.notes) what += "  (" + n + ")";
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", what.c_str());
        if (info.recommended >= 0) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "Recommended: %s \xE2\x80\x94 %s", steps[info.recommended].name.c_str(), info.recommend_reason.c_str());
        for (auto& wn : info.warnings) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.warn), "%s", wn.c_str());
        ImGui::PopTextWrapPos();

        // R11 P2-04: options and units side by side (Import button visible at 175% without scrolling)
        ImGui::Spacing();
        if (ImGui::BeginTable("odbopt", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextColumn();
            ImGui::SeparatorText("Import");
        ImGui::Checkbox("Gerber layers (copper / silk / mask / paste / drill)", &st.layers);
        gui::tooltip("Always done automatically when the step has no components.");
            ImGui::Checkbox("Board outline", &st.board); ImGui::SameLine(S(190)); ImGui::Checkbox("Packages", &st.pkgs);
            ImGui::Checkbox("Replace existing", &st.replace);
            gui::tooltip("Replace existing components / packages");
            ImGui::SameLine(S(190)); ImGui::Checkbox("FID* = fiducials", &st.fid);
            gui::tooltip("Refs FID* / FID packages are fiducials");
            ImGui::TableNextColumn();
        // ---- coordinate units (REVIEW_V9 §6C) ----
        ImGui::PushTextWrapPos(0);
        ImGui::SeparatorText("Coordinate units");
        const char* declared = s.units_conflict ? "mixed (each file converted with its own declaration)"
                             : !s.units_declared ? "inch \xE2\x80\x94 no UNITS in the files (ODB++ default)"
                             : std::fabs(s.units_k - 1.0) < 1e-9 ? "mm" : std::fabs(s.units_k - 25.4) < 1e-9 ? "inch" : "custom";
        ImGui::Text("Detected: %s", declared);
        {
            std::string ev;
            for (auto& e : s.unit_evidence) ev += e + "\n";
            if (!info.job_units.empty()) ev += "misc/info: " + info.job_units + "\n";
            if (ev.empty()) ev = "no unit declarations found";
            gui::tooltip(("Evidence:\n" + ev).c_str());
        }
        ImGui::SetNextItemWidth(S(170));
        const char* unames[] = {"As declared (recommended)", "Millimetres", "Inches", "Mils (0.001 in)", "0.1 mm", "Custom (mm per unit)"};
        ImGui::Combo("Read as", &st.unit, unames, 6);
        gui::tooltip("Unit conversion: tells the importer what unit the numbers in the files really are.\n"
                     "Use it when a job declares the wrong unit (e.g. mm values in a file without UNITS=MM).");
        if (st.unit == 5) { ImGui::SameLine(); ImGui::SetNextItemWidth(S(110)); ImGui::InputDouble("mm per unit", &st.custom, 0, 0, "%.6g"); if (st.custom <= 0) st.custom = 1; }
        ImGui::SetNextItemWidth(S(110));
        ImGui::InputDouble("Extra scale", &st.scale, 0, 0, "%.6g");
        if (st.scale <= 0) st.scale = 1;
        gui::tooltip("Arbitrary user scale, applied AFTER the unit conversion (1 = none).\n"
                     "Not a unit fix: use it only for artwork that was deliberately scaled.");
        static const double kk[] = {0, 1.0, 25.4, 0.0254, 0.1, 0};
        const double force = st.unit == 5 ? st.custom : kk[st.unit];
        const double base = s.units_k > 0 ? s.units_k : 1.0;
        const double fct = (force > 0 ? force / base : 1.0) * st.scale;
        if (s.has_profile) {
            ImGui::Text("Board: %.2f x %.2f mm  (declared %.2f x %.2f)", s.w * fct, s.h * fct, s.w, s.h);
            for (auto& wn : io::odb_unit_warnings(s.w * fct, s.h * fct, s.comps_outside_profile, s.comp_top + s.comp_bot))
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.warn), "Check units: %s", wn.c_str());
        } else ImGui::TextDisabled("No profile in this step: board size preview unavailable.");
        if (s.pkgs) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "Packages: %d (%d outline, %d box, %d from pads)",
                                       s.pkgs, s.pkg_outlines, s.pkg_extents, s.pkgs - s.pkg_outlines - s.pkg_extents);
            ImGui::PopTextWrapPos();
            ImGui::EndTable();
        }
        (void)ff;
    }, 340);
    f.ok_text(L"Import");
    f.extra_button(L"Details\x2026", [&](Form&) { show_text(owner, L"ODB++ job", W(io::odb_describe(info))); return false; });
    if (!f.run(owner)) return false;
    std::string step = steps[st.sel].name;
    if (!steps[st.sel].repeats.empty()) step += st.mode == 1 ? "|images" : st.mode == 2 ? "|flat" : "";
    if (st.layers) step += "|layers";
    o["step"] = step;
    {
        static const char* uw[] = {"", "mm", "inch", "mil", "0.1mm", ""};
        char b[48];
        if (st.unit == 5) { std::snprintf(b, sizeof b, "%.9g", st.custom); o["units"] = b; }
        else o["units"] = uw[st.unit];
        std::snprintf(b, sizeof b, "%.9g", st.scale);
        o["scale"] = b;
    }
    o["replace"] = st.replace ? "1" : "0"; o["import_board"] = st.board ? "1" : "0";
    o["import_packages"] = st.pkgs ? "1" : "0"; o["fiducial_by_name"] = st.fid ? "1" : "0";
    return true;
}

bool dlg_rescale_coordinates(HWND owner, Doc& d) {
    BBox bb = d.prj.board_bbox();
    struct R { int kind = 0; int from = 0, to = 1; double factor = 1.0; } r;   // kind 0 unit fix, 1 factor
    static const char* un[] = {"inch", "mm", "mil", "0.1 mm", "micron"};
    static const double mm[] = {25.4, 1.0, 0.0254, 0.1, 0.001};
    Form f(L"Rescale Coordinates", 560);
    f.icon((int)Icon::Board).header(L"Rescale Coordinates");
    f.subtitle(L"Scales placements, packages, frames, panel images and Gerber layers about (0,0). One undo step.");
    f.custom("rs", [&](Form&, float w) {
        const gui::GTheme& t = th();
        ImGui::RadioButton("Unit fix (data was read in the wrong unit)", &r.kind, 0);
        if (r.kind == 0) {
            ImGui::Indent();
            ImGui::SetNextItemWidth(S(110)); ImGui::Combo("values are really", &r.from, un, 5); ImGui::SameLine();
            ImGui::SetNextItemWidth(S(110)); ImGui::Combo("but were read as", &r.to, un, 5);
            ImGui::Unindent();
        }
        ImGui::RadioButton("Arbitrary scale factor (user correction, not a unit change)", &r.kind, 1);
        if (r.kind == 1) { ImGui::Indent(); ImGui::SetNextItemWidth(S(140)); ImGui::InputDouble("factor", &r.factor, 0, 0, "%.9g"); ImGui::Unindent(); }
        const double fct = r.kind == 0 ? mm[r.from] / mm[r.to] : r.factor;
        ImGui::Spacing();
        ImGui::Text("Factor: %.9g", fct);
        if (!bb.empty()) {
            ImGui::Text("Board extent now: %.2f x %.2f mm  \xE2\x86\x92  after: %.2f x %.2f mm", bb.w(), bb.h(), bb.w() * fct, bb.h() * fct);
            for (auto& wn : io::odb_unit_warnings(bb.w() * fct, bb.h() * fct, 0, 0))
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.warn), "%s", wn.c_str());
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "%zu placements, %zu packages, %zu layers. Package sizes scale too (the package library is per project).",
                           d.prj.comps.size(), d.prj.packages.size(), d.prj.layers.size());
        ImGui::PopTextWrapPos();
    }, 210);
    f.ok_text(L"Rescale");
    if (!f.run(owner)) return false;
    const double fct = r.kind == 0 ? mm[r.from] / mm[r.to] : r.factor;
    if (!(fct > 0) || std::fabs(fct - 1.0) < 1e-12) return false;
    char lb[96];
    if (r.kind == 0) std::snprintf(lb, sizeof lb, "Rescale coordinates (%s read as %s)", un[r.from], un[r.to]);
    else std::snprintf(lb, sizeof lb, "Rescale coordinates \xC3\x97%.6g", fct);
    d.checkpoint_import(lb);          // also snapshots layers (rewritten below)
    std::vector<std::string> notes;
    const int nl = io::rescale_project(d.prj, fct, notes);
    d.parse_all_layers();
    d.changed(ui::CH_ALL);
    std::string msg = std::string(lb) + ": " + std::to_string(d.prj.comps.size()) + " placements, " + std::to_string(nl) + " layer(s) rescaled";
    gui::toast(msg, notes.empty() ? gui::ToastKind::Success : gui::ToastKind::Warning, 5.f);
    if (!notes.empty()) {
        std::wstring t = W(msg) + L"\n\n";
        for (auto& n : notes) t += W(n) + L"\n";
        show_text(owner, L"Rescale Coordinates", t);
    }
    return true;
}

int prune_import_export_layers(Project& prj) {
    // R18d: after a CAD import with exactly two SMT layers (top + bottom), link them as one two-sided job
    struct AutoLink { Project& p; ~AutoLink() { pcbp::auto_link_smt_pair(p); } } autolink{prj};
    auto& pts = prj.model.points;
    if (pts.empty()) return 0;
    bool top = false, bot = false;
    std::vector<char> used(pts.size(), 0);
    for (const Component& c : prj.comps) {
        if (c.feature != Feature::Component) continue;
        (c.side == Side::Bottom ? bot : top) = true;
        if (c.assigned_point >= 0 && c.assigned_point < (int)pts.size()) used[c.assigned_point] = 1;
    }
    if (!top && !bot) top = true;   // geometry-only import: one export layer to start from
    std::vector<int> remap(pts.size(), -1);
    std::vector<ModelPoint> keep;
    for (size_t i = 0; i < pts.size(); ++i) {
        const ModelPoint& p = pts[i];
        const bool k = used[i] || (p.name == "Top SMT" && top) || (p.name == "Bottom SMT" && bot);
        if (!k) continue;
        remap[i] = (int)keep.size();
        keep.push_back(p);
    }
    if (keep.empty()) return 0;   // unknown model (not the default): leave it alone
    const int removed = (int)(pts.size() - keep.size());
    if (!removed) return 0;
    for (Component& c : prj.comps)
        if (c.assigned_point >= 0) c.assigned_point = c.assigned_point < (int)remap.size() ? remap[c.assigned_point] : -1;
    pts = std::move(keep);
    for (auto& s : prj.mfg.sides) {
        bool found = false;
        for (auto& p : pts) found |= p.name == s.point;
        if (!found) s.point.clear();
    }
    return removed;
}

} // namespace pcbp::app
