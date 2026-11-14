// machine_data_ui.cpp — Packages view "Machine" tab (R24b): suggested machine component data per package
// (nozzle, packaging, tape, pitch, carrier, supply angle, class code, centring) with confidence + rule,
// editable overrides (override always wins, saved in the project), bulk assign, low-confidence filter.
// Logic lives in core/machine_assign.*; this file is UI only.
#include "gapp.h"
#include "../app/doc.h"
#include "../core/machine_assign.h"
#include "../core/project.h"
#include "imgui.h"
#include <algorithm>
#include <functional>
#include <cstdio>
#include <string>
#include <vector>

namespace pcbp::gui {

namespace {

void lbl(const char* l) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, th().text_dim);
    ImGui::TextUnformatted(l);
    ImGui::PopStyleColor();
    ImGui::SameLine(S(96));
}

std::string fmt_f(const mach::Fields& f, int which) {
    char b[48];
    switch (which) {
        case 0: return f.packaging.empty() ? "-" : f.packaging;
        case 1: if (!f.nozzle) return "-"; std::snprintf(b, sizeof b, "%d", f.nozzle); return b;
        case 2: if (!f.tape_w) return "-"; std::snprintf(b, sizeof b, "%d mm", f.tape_w); return b;
        case 3: if (!f.pitch) return "-"; std::snprintf(b, sizeof b, "%d mm", f.pitch); return b;
        case 4: return f.carrier.empty() ? "-" : f.carrier;
        case 5: std::snprintf(b, sizeof b, "%g\xC2\xB0", f.supply_angle); return b;
        case 6: if (!f.type_code) return "-"; std::snprintf(b, sizeof b, "%d", f.type_code); return b;
        case 7: return f.centring.empty() ? "-" : f.centring;
    }
    return "-";
}

void edit_all(const app::DocPtr& d, const std::vector<std::string>& names, const char* label,
              const std::function<void(Package::MachineOverride&)>& fn) {
    d->checkpoint(label);
    for (auto& n : names) {
        auto it = d->prj.packages.find(n);
        if (it != d->prj.packages.end()) fn(it->second.mach);
    }
    d->changed(ui::CH_GEOMETRY);
}

// combo over int choices, 0 = Auto
void int_combo(const char* id, int cur, const int* vals, int nv, const char* unit, const std::function<void(int)>& set) {
    char cb[32];
    if (cur) std::snprintf(cb, sizeof cb, "%d%s", cur, unit); else std::snprintf(cb, sizeof cb, "Auto");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo(id, cb)) {
        if (ImGui::Selectable("Auto", cur == 0)) set(0);
        for (int i = 0; i < nv; ++i) {
            std::snprintf(cb, sizeof cb, "%d%s", vals[i], unit);
            if (ImGui::Selectable(cb, cur == vals[i])) set(vals[i]);
        }
        ImGui::EndCombo();
    }
}
void str_combo(const char* id, const std::string& cur, const char* const* vals, int nv, const std::function<void(const std::string&)>& set) {
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo(id, cur.empty() ? "Auto" : cur.c_str())) {
        if (ImGui::Selectable("Auto", cur.empty())) set("");
        for (int i = 0; i < nv; ++i) if (ImGui::Selectable(vals[i], cur == vals[i])) set(vals[i]);
        ImGui::EndCombo();
    }
}

struct ListState { bool low_only = false, unassigned_only = false; float conf_min = 0.7f; };
ListState g_ls;

} // namespace

// names = the packages the inspector edits (selection, first = current). cur = current package name (row click sets it).
void draw_machine_data(const app::DocPtr& d, const std::vector<std::string>& names, const std::string& cur,
                       const std::function<void(const std::string&)>& pick) {
    if (!d) return;
    Project& prj = d->prj;
    const GTheme& t = th();
    Package* p = nullptr;
    if (!names.empty()) { auto it = prj.packages.find(names.front()); if (it != prj.packages.end()) p = &it->second; }
    if (p) {
        section_header(names.size() > 1 ? "MACHINE DATA (SELECTION)" : "MACHINE DATA");
        mach::Suggestion sg;
        const PartNumber* pn = mach::part_for_package(prj, p->name);
        const mach::Fields eff = mach::effective_for(*p, pn, &sg);
        {   // confidence bar + rule
            const float w = ImGui::GetContentRegionAvail().x, h = S(8);
            ImVec2 a = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(a, a + ImVec2(w, h), t.panel2, S(3));
            const ImU32 c = sg.conf >= 0.8 ? t.ok : sg.conf >= 0.6 ? t.warn : t.error;
            dl->AddRectFilled(a, a + ImVec2(w * (float)sg.conf, h), c, S(3));
            ImGui::Dummy(ImVec2(w, h + S(2)));
            ImGui::Text("Confidence %.0f%%   %s%s", sg.conf * 100.0, mach::fam_name(sg.fam),
                        p->mach_assigned ? "   (assigned)" : "   (not assigned yet)");
            ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
            ImGui::TextWrapped("Rule: %s", sg.rule.c_str());
            if (sg.dims_src != "cad")
                ImGui::TextWrapped("No CAD body outline: sizes come from the package code. The laser checks the BODY, so draw or import the body for better results.");
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, S(4)));
        if (ImGui::BeginTable("##mach", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, S(86));
            ImGui::TableSetupColumn("Suggested", ImGuiTableColumnFlags_WidthFixed, S(80));
            ImGui::TableSetupColumn("Override");
            ImGui::TableHeadersRow();
            auto& m = p->mach;
            auto row = [&](const char* f, int which) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(f);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(fmt_f(sg.f, which).c_str());
                ImGui::TableNextColumn();
            };
            static const char* const kPk[] = {"Tape", "Tray", "Stick", "Bulk"};
            row("Packaging", 0);
            str_combo("##mpk", m.packaging, kPk, 4, [&](const std::string& v) { edit_all(d, names, "Machine data: packaging", [v](auto& q) { q.packaging = v; }); });
            tooltip("House rule: cut tape for everything except QFP / BGA types (tray). The tray magazine (MTC) is never used.");
            static const int kNoz[] = {500, 504, 505, 506, 507, 508};
            row("Nozzle", 1);
            int_combo("##mnz", m.nozzle, kNoz, 6, "", [&](int v) { edit_all(d, names, "Machine data: nozzle", [v](auto& q) { q.nozzle = v; }); });
            tooltip("Only the nozzles in use are offered (500, 504-508). Not written into the program (field not confirmed): shown for setup.");
            static const int kTw[] = {8, 12, 16, 24, 32, 44, 56};
            row("Tape width", 2);
            int_combo("##mtw", m.tape_w, kTw, 7, " mm", [&](int v) { edit_all(d, names, "Machine data: tape width", [v](auto& q) { q.tape_w = v; }); });
            static const int kPi[] = {2, 4, 8, 12, 16, 20, 24};
            row("Feed pitch", 3);
            int_combo("##mpi", m.pitch, kPi, 7, " mm", [&](int v) { edit_all(d, names, "Machine data: pitch", [v](auto& q) { q.pitch = v; }); });
            static const char* const kCa[] = {"Paper", "Embossed"};
            row("Carrier", 4);
            if (eff.tape_w == 8) { ImGui::TextDisabled("Paper (8 mm rule)"); }
            else str_combo("##mca", m.carrier, kCa, 2, [&](const std::string& v) { edit_all(d, names, "Machine data: carrier", [v](auto& q) { q.carrier = v; }); });
            row("Supply angle", 5);
            {
                float a = (float)(m.supply_angle_set ? m.supply_angle : eff.supply_angle);
                bool set = m.supply_angle_set;
                if (ImGui::Checkbox("##masset", &set)) edit_all(d, names, "Machine data: supply angle", [set, a](auto& q) { q.supply_angle_set = set; q.supply_angle = a; });
                tooltip("Tick to override. Default: tape 0, tray 180, stick 270.");
                ImGui::SameLine(); ImGui::SetNextItemWidth(-1);
                ImGui::BeginDisabled(!set);
                ImGui::InputFloat("##masv", &a, 0, 0, "%.0f");
                if (ImGui::IsItemDeactivatedAfterEdit()) edit_all(d, names, "Machine data: supply angle", [a](auto& q) { q.supply_angle = a; q.supply_angle_set = true; });
                ImGui::EndDisabled();
            }
            row("Class code", 6);
            {
                int v = m.type_code;
                ImGui::SetNextItemWidth(-1);
                ImGui::InputInt("##mty", &v, 0, 0);
                if (ImGui::IsItemDeactivatedAfterEdit()) edit_all(d, names, "Machine data: class code", [v](auto& q) { q.type_code = std::max(0, v); });
                tooltip("Machine component class code (0 = Auto). Chip 1, SOT 11, SOP 101, QFP 102, BGA 107, QFN 120, connector 201 ...");
            }
            static const char* const kCe[] = {"Laser", "Vision"};
            row("Centring", 7);
            str_combo("##mce", m.centring, kCe, 2, [&](const std::string& v) { edit_all(d, names, "Machine data: centring", [v](auto& q) { q.centring = v; }); });
            ImGui::EndTable();
        }
        {
            std::string e = "Effective: " + fmt_f(eff, 0) + ", nozzle " + fmt_f(eff, 1);
            if (eff.tape_w) e += ", " + fmt_f(eff, 2) + " " + fmt_f(eff, 4) + ", pitch " + fmt_f(eff, 3);
            e += ", angle " + fmt_f(eff, 5);
            ImGui::TextWrapped("%s", e.c_str());
        }
        if (button("Clear overrides", ui::Icon::None, false, ImVec2(0, 0), !p->mach.empty()))
            edit_all(d, names, "Machine data: clear overrides", [](auto& q) { q = Package::MachineOverride{}; });
        ImGui::SameLine();
        if (button(p->mach_assigned ? "Assigned" : "Accept", ui::Icon::None, !p->mach_assigned, ImVec2(0, 0), !p->mach_assigned)) {
            d->checkpoint("Assign machine data");
            mach::assign_all(prj, 1, names);
            d->changed(ui::CH_GEOMETRY);
        }
        ImGui::Dummy(ImVec2(0, S(8)));
    }

    // ---- all packages
    section_header("ALL PACKAGES");
    ImGui::Checkbox("Low confidence only", &g_ls.low_only);
    ImGui::SameLine(); ImGui::Checkbox("Unassigned only", &g_ls.unassigned_only);
    {
        int na = 0;
        for (auto& kv : prj.packages) if (!kv.second.mach_assigned) ++na;
        if (button("Assign all", ui::Icon::None, true)) { d->checkpoint("Assign machine data (all)"); int n = mach::assign_all(prj, 0, {}); d->changed(ui::CH_GEOMETRY); toast("Machine data assigned to " + std::to_string(n) + " packages", ToastKind::Success); }
        tooltip("Accept the suggested machine data for every package (overrides are kept). Export uses the suggestion either way; this marks them reviewed.");
        ImGui::SameLine();
        if (button("Assign selected", ui::Icon::None, false, ImVec2(0, 0), !names.empty())) { d->checkpoint("Assign machine data (selected)"); mach::assign_all(prj, 1, names); d->changed(ui::CH_GEOMETRY); }
        ImGui::SameLine();
        char b[48]; std::snprintf(b, sizeof b, "Assign unassigned (%d)", na);
        if (button(b, ui::Icon::None, false, ImVec2(0, 0), na > 0)) { d->checkpoint("Assign machine data (unassigned)"); mach::assign_all(prj, 2, {}); d->changed(ui::CH_GEOMETRY); }
    }
    if (ImGui::BeginTable("##machall", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, std::max(S(160), ImGui::GetContentRegionAvail().y - S(4))))) {
        for (const char* c : {"Package", "Type", "Nozzle", "Tape", "Pack", "Conf", "Done"}) ImGui::TableSetupColumn(c);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (auto& kv : prj.packages) {
            const Package& q = kv.second;
            if (g_ls.unassigned_only && q.mach_assigned) continue;
            mach::Suggestion s;
            const mach::Fields e = mach::effective_for(q, mach::part_for_package(prj, q.name), &s);
            const double conf = q.mach.empty() ? s.conf : 1.0;
            if (g_ls.low_only && conf >= g_ls.conf_min) continue;
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            const bool sel = cur == kv.first;
            if (ImGui::Selectable((kv.first + "##m").c_str(), sel, ImGuiSelectableFlags_SpanAllColumns) && pick) pick(kv.first);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.rule.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mach::fam_name(s.fam));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(fmt_f(e, 1).c_str());
            ImGui::TableNextColumn();
            if (e.tape_w) ImGui::Text("%d/%d%s", e.tape_w, e.pitch, e.carrier == "Paper" ? " P" : " E"); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.packaging.c_str());
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, conf >= 0.8 ? t.ok : conf >= 0.6 ? t.warn : t.error);
            ImGui::Text("%.0f%%%s", conf * 100, q.mach.empty() ? "" : " *");
            ImGui::PopStyleColor();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(q.mach_assigned ? "yes" : "");
        }
        ImGui::EndTable();
    }
}

} // namespace pcbp::gui
