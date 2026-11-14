// ============================================================================
// group_ops.cpp — Edit ▸ Select By / Group Edit (brief §9).
// UI wrapper over core/group_ops: every edit is ONE undo checkpoint + a toast
// with counts. Rotation model: specs/ANGLES.md (rot is the exported angle).
// ============================================================================
#include "gapp.h"
#include "../core/group_ops.h"
#include "../ui/form.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace pcbp::gui {
using app::Doc;
using app::DocPtr;
using ui::U;
using ui::W;

namespace {

std::wstring fmt_deg(double a) {
    wchar_t b[48];
    swprintf(b, 48, L"%.3f", a);
    std::wstring s = b;
    while (!s.empty() && s.back() == L'0') s.pop_back();
    if (!s.empty() && s.back() == L'.') s.pop_back();
    return s + L"\x00B0";
}

void finish_select(Doc& d, int n, const std::string& what) {
    d.changed(ui::CH_SELECTION);
    toast("Selected " + std::to_string(n) + " placement(s) " + what, n ? ToastKind::Info : ToastKind::Warning);
}

SelMode mode_from_keys() {
    const ImGuiIO& io = ImGui::GetIO();
    return io.KeyCtrl ? SelMode::Subtract : io.KeyShift ? SelMode::Add : SelMode::Replace;
}

std::vector<std::wstring> package_names(const Doc& d) {
    std::set<std::string> s;
    for (auto& c : d.prj.comps) if (c.feature == Feature::Component && !c.package.empty()) s.insert(c.package);
    for (auto& kv : d.prj.packages) s.insert(kv.first);
    std::vector<std::wstring> v;
    for (auto& x : s) v.push_back(W(x));
    return v;
}

void select_by_ref_dialog(const DocPtr& d, SelMode mode) {
    ui::Form f(L"Select by Reference", 460);
    f.text("pat", L"References", L"");
    f.label(L"Examples:  R1-R20, C5, U*, TP?   (comma or space separated; * and ? wildcards)");
    if (!f.run(app::frame_hwnd())) return;
    const std::string pat = f.u8("pat");
    finish_select(*d, select_by_ref(d->prj, pat, mode), "matching '" + pat + "'");
}

void select_by_part_dialog(const DocPtr& d, SelMode mode) {
    std::set<std::string> s;
    std::string first;
    for (auto& c : d->prj.comps) {
        if (c.feature != Feature::Component || c.number.empty()) continue;
        s.insert(c.number);
        if (c.selected && first.empty()) first = c.number;
    }
    std::vector<std::wstring> v;
    for (auto& x : s) v.push_back(W(x));
    ui::Form f(L"Select by Part Number", 460);
    f.combo("pn", L"Part number", v, W(first), true);
    if (!f.run(app::frame_hwnd())) return;
    const std::string pn = f.u8("pn");
    finish_select(*d, select_by_part(d->prj, {pn}, mode), "with part number " + pn);
}

void select_by_package_dialog(const DocPtr& d, SelMode mode) {
    std::string first;
    for (auto& c : d->prj.comps) if (c.selected && c.feature == Feature::Component) { first = c.package; break; }
    ui::Form f(L"Select by Package", 460);
    f.combo("pk", L"Package", package_names(*d), W(first), true);
    if (!f.run(app::frame_hwnd())) return;
    const std::string pk = f.u8("pk");
    finish_select(*d, select_by_package(d->prj, {pk}, mode), "with package " + pk);
}

void group_edit_dialog(const DocPtr& d) {
    Project& prj = d->prj;
    std::vector<int> idx = selected_indices(prj);
    if (idx.empty()) { status("Select placements first (Edit \xE2\x96\xB8 Select By)"); return; }
    std::set<std::string> pk;
    for (int i : idx) pk.insert(prj.comps[i].package);
    ui::Form f(L"Group Edit \x2014 " + std::to_wstring(idx.size()) + L" placement(s)", 520);
    f.icon((int)ui::Icon::Rot90);
    f.label(std::to_wstring(idx.size()) + L" placement(s), " + std::to_wstring(pk.size()) + L" package(s). "
            L"Rotation is CCW and is exactly what the machine program receives.");
    f.combo("op", L"Operation",
            {L"Add rotation delta", L"Set absolute rotation", L"Reset to imported rotation", L"Set pre-rotation (CW, metadata)",
             L"Change package", L"Assign process", L"Assign group label", L"Inspect outliers"},
            L"Add rotation delta");
    f.number("deg", L"Degrees", 90, 3);
    f.combo("pkg", L"Package", package_names(*d), pk.size() == 1 ? W(*pk.begin()) : L"", true);
    f.combo("proc", L"Process", {L"Auto", L"SMT", L"TH", L"Hardware", L"Other"}, L"SMT");
    f.text("lbl", L"Group label", L"");
    auto sync = [](ui::Form& g, const std::string&) {
        const int op = g.index("op");
        g.enable("deg", op == 0 || op == 1 || op == 3);
        g.enable("pkg", op == 4);
        g.enable("proc", op == 5);
        g.enable("lbl", op == 6);
    };
    f.field("op")->on_change = sync;
    sync(f, "op");
    if (!f.run(app::frame_hwnd())) return;
    const int op = std::max(0, f.index("op"));
    const double deg = f.num("deg", 0);
    if (op == 7) {
        auto out = find_outliers(prj, idx);
        for (auto& c : prj.comps) c.selected = false;
        for (auto& o : out) prj.comps[o.index].selected = true;
        d->changed(ui::CH_SELECTION);
        std::wstring msg;
        if (out.empty()) msg = L"No outliers: every placement of the selected package(s) shares its package's majority rotation (mod 90\x00B0).";
        else {
            msg = std::to_wstring(out.size()) + L" outlier(s) selected (rotation mod 90\x00B0 differs from the package majority):\n\n";
            for (size_t k = 0; k < out.size() && k < 40; ++k)
                msg += W(prj.comps[out[k].index].ref) + L"   " + W(out[k].package) + L"   " + fmt_deg(out[k].rot) +
                       L"   (majority residue " + fmt_deg(out[k].majority_residue) + L")\n";
            if (out.size() > 40) msg += L"\x2026";
        }
        ui::message(app::frame_hwnd(), L"Inspect Outliers", msg);
        return;
    }
    static const char* labels[] = {"Rotate group", "Set rotation", "Reset rotation", "Set pre-rotation",
                                   "Change package", "Assign process", "Assign group label"};
    d->checkpoint(labels[op]);
    int n = 0;
    std::string what;
    switch (op) {
    case 0: n = rotate_add(prj, idx, deg); what = "rotated by " + U(fmt_deg(deg)); break;
    case 1: n = rotate_set(prj, idx, deg); what = "set to " + U(fmt_deg(norm_deg(deg))); break;
    case 2: n = rotate_reset(prj, idx); what = "reset to imported rotation"; break;
    case 3: n = set_pre_rotation(prj, idx, deg); what = "pre-rotation " + U(fmt_deg(norm_deg(deg))) + " CW"; break;
    case 4: n = set_package(prj, idx, f.u8("pkg")); what = "moved to package " + f.u8("pkg"); break;
    case 5: n = set_process(prj, idx, (Process)std::max(0, f.index("proc"))); what = std::string("process ") + process_name((Process)std::max(0, f.index("proc"))); break;
    case 6: n = set_group_label(prj, idx, f.u8("lbl")); what = "labelled '" + f.u8("lbl") + "'"; break;
    }
    d->changed(op == 4 ? (ui::CH_GEOMETRY | ui::CH_MODEL) : ui::CH_GEOMETRY);
    toast(std::to_string(n) + " of " + std::to_string(idx.size()) + " placement(s) " + what +
              (n < (int)idx.size() ? "  (" + std::to_string(idx.size() - n) + " already matched)" : std::string()),
          n ? ToastKind::Success : ToastKind::Info);
}

} // namespace

// Edit menu hook: "Select By" submenu + "Group Edit...". Shift = add, Ctrl = subtract.
void group_ops_menu_items(const DocPtr& d) {
    if (!d) return;
    if (ImGui::BeginMenu("Select By")) {
        const SelMode mode = mode_from_keys();
        const bool any_sel = d->selected_count() > 0;
        if (ImGui::MenuItem("Package of Selection", nullptr, false, any_sel)) {
            std::vector<std::string> pk;
            for (auto& c : d->prj.comps) if (c.selected) pk.push_back(c.package);
            finish_select(*d, select_by_package(d->prj, pk, mode), "with the same package");
        }
        if (ImGui::MenuItem("Part Number of Selection", nullptr, false, any_sel)) {
            std::vector<std::string> pn;
            for (auto& c : d->prj.comps) if (c.selected && !c.number.empty()) pn.push_back(c.number);
            finish_select(*d, select_by_part(d->prj, pn, mode), "with the same part number");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Package...")) defer([d, mode]() { select_by_package_dialog(d, mode); });
        if (ImGui::MenuItem("Reference...")) defer([d, mode]() { select_by_ref_dialog(d, mode); });
        if (ImGui::MenuItem("Part Number...")) defer([d, mode]() { select_by_part_dialog(d, mode); });
        if (ImGui::BeginMenu("Process")) {
            for (Process p : {Process::SMT, Process::TH, Process::Hardware, Process::Other})
                if (ImGui::MenuItem(process_name(p))) finish_select(*d, select_by_process(d->prj, p, mode), std::string("with process ") + process_name(p));
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Side")) {
            if (ImGui::MenuItem("Top")) finish_select(*d, select_by_side(d->prj, Side::Top, mode), "on the top side");
            if (ImGui::MenuItem("Bottom")) finish_select(*d, select_by_side(d->prj, Side::Bottom, mode), "on the bottom side");
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("All Visible")) {
            const bool xr = side_xray(*d), bot = view_bottom(*d);
            const int n = select_where(d->prj, [&](const Component& c) { return xr || (c.side == Side::Bottom) == bot; }, mode);
            finish_select(*d, n, "visible on the current side display");
        }
        ImGui::Separator();
        ImGui::TextDisabled("Shift: add   Ctrl: subtract");
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Group Edit / Rotate...", nullptr, false, d->selected_count() > 0)) defer([d]() { group_edit_dialog(d); });
}

} // namespace pcbp::gui
