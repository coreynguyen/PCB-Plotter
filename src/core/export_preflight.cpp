// ============================================================================
// export_preflight.cpp — see export_preflight.h. Moved from gui/output_views.cpp
// (app::export_validation) so every Generate path and the tests share it.
// ============================================================================
#include "export_preflight.h"
#include "sides.h"
#include <algorithm>
#include <map>

namespace pcbp {

const char* export_entry_name(ExportEntry e) {
    switch (e) {
        case ExportEntry::GenerateCommand: return "Generate command";
        case ExportEntry::ExportViewLayer: return "Export view (layer)";
        case ExportEntry::ExportViewPair: return "Export view (linked pair)";
        case ExportEntry::ExportAllSides: return "Export all sides";
        case ExportEntry::PluginExport: return "Plugin export";
    }
    return "?";
}

ExportTarget export_target(const Project& p, const ExportRequest& r) {
    ExportTarget t;
    t.point = r.point;
    const bool pair = r.entry == ExportEntry::ExportViewPair || r.entry == ExportEntry::ExportAllSides;
    if (pair && r.point >= 0 && r.point < (int)p.model.points.size()) {
        const int link = p.model.points[r.point].link;
        if (link > 0 && link_sides(p, link).configured) { t.point = -1; t.link = link; }
    }
    return t;
}

static std::vector<ExportIssue> preflight(const Project& p, int point, int link) {
    std::vector<ExportIssue> v;
    MfgSides mfg;
    if (link > 0) mfg = link_sides(p, link);
    const bool sides = mfg.configured && !mfg.sides.empty();
    if (sides)
        for (auto& i : validate_sides(p, mfg)) v.push_back({i.blocking ? ExportIssue::Error : ExportIssue::Warning, "", i.text});
    std::map<std::string, Side> side_of;
    for (auto& c : p.comps) if (c.feature == Feature::Component) side_of[c.ref] = c.side;
    auto add_point = [&](int pi, const MfgSide* sd) {
        int shared = 0;
        if (sd) for (auto& o : mfg.sides) if (side_point_index(p, o) == pi) ++shared;
        const std::string pre = sd ? mfg_label(*sd) + ": " : std::string();
        for (auto i : validate_for_export(p, pi)) {
            if (sd && !i.ref.empty()) {
                auto it = side_of.find(i.ref);
                const bool other = it != side_of.end() && it->second != sd->physical;
                if (other && shared > 1) continue;                       // belongs to the other side's job
                if (i.msg.rfind("Placement is on the ", 0) == 0 && shared > 1) continue;
            }
            if (sd && i.msg.rfind("No components are assigned to model point", 0) == 0) continue;
            if (sd && shared > 1 && i.msg.rfind("Top/bottom separation", 0) == 0) continue;   // R16: one profile, both sides; validate_sides reports it
            i.msg = pre + i.msg;
            v.push_back(std::move(i));
        }
    };
    if (link > 0) {
        if (sides) for (auto& sd : mfg.sides) { const int pi = side_point_index(p, sd); if (pi >= 0) add_point(pi, &sd); }
        else v.push_back({ExportIssue::Error, "", "Link " + std::to_string(link) + " does not have exactly two export layers."});
    } else if (point >= 0) add_point(point, nullptr);
    else v.push_back({ExportIssue::Error, "", "No export layer selected."});
    // R12 §17: a difference from the legacy program angle is a comparison note, never an error / warning
    for (auto& i : v) if (i.msg.find("egacy") != std::string::npos && i.msg.find("angle") != std::string::npos) i.sev = ExportIssue::Info;
    std::stable_sort(v.begin(), v.end(), [](const ExportIssue& a, const ExportIssue& b) { return a.sev < b.sev; });
    return v;
}

std::vector<ExportIssue> export_preflight(const Project& p, const ExportTarget& t) { return preflight(p, t.point, t.link); }
std::vector<ExportIssue> export_preflight_point(const Project& p, int point) { return preflight(p, point, 0); }
std::vector<ExportIssue> export_preflight_link(const Project& p, int link) { return preflight(p, -1, link); }

std::string export_review_text(const std::vector<ExportIssue>& v, size_t max_lines, bool headers) {
    std::string s;
    for (int g = 0; g < 2; ++g) {
        const ExportIssue::Sev sev = g == 0 ? ExportIssue::Error : ExportIssue::Warning;
        size_t n = 0, shown = 0;
        for (auto& i : v) n += i.sev == sev;
        if (!n) continue;
        if (!s.empty()) s += "\n";
        if (headers) s += g == 0 ? "ERRORS (" + std::to_string(n) + ") - the program is probably wrong for the machine:\n"
                    : "Warnings (" + std::to_string(n) + ") - review:\n";
        for (auto& i : v) {
            if (i.sev != sev) continue;
            if (++shown > max_lines) { s += "  ... and " + std::to_string(n - max_lines) + " more\n"; break; }
            s += (g == 0 ? "  [Error] " : "  [Warning] ") + (i.ref.empty() ? "" : i.ref + " - ") + i.msg + "\n";
        }
    }
    return s;
}

} // namespace pcbp
