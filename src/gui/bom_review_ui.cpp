// bom_review_ui.cpp — BOM wizard Review step (R14d). See bom_review_ui.h.
#include "bom_review_ui.h"
#include "../app/app.h"
#include "../core/fileio.h"
#include "../io/io_util.h"
#include "gapp.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>

namespace pcbp::gui {
using io::ReviewCat;

namespace {
ImVec4 C4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
uint32_t bit(ReviewCat c) { return 1u << (int)c; }
const uint32_t kErr = bit(ReviewCat::PnConflict) | bit(ReviewCat::RefError);
const uint32_t kWarn = bit(ReviewCat::QtyMismatch) | bit(ReviewCat::NotOnBoard) | bit(ReviewCat::Unmatched) |
                       bit(ReviewCat::NoPartNumber) | bit(ReviewCat::Duplicate) | bit(ReviewCat::OtherWarning);

ImU32 flag_col(uint32_t f, ImU32 normal) {
    const GTheme& t = th();
    return f & kErr ? t.error : f & kWarn ? t.warn : normal;
}
std::string flag_text(uint32_t f) {
    static const std::pair<ReviewCat, const char*> names[] = {
        {ReviewCat::PnConflict, "P/N conflict"}, {ReviewCat::RefError, "bad range"}, {ReviewCat::QtyMismatch, "qty mismatch"},
        {ReviewCat::NotOnBoard, "not on board"}, {ReviewCat::Unmatched, "not in BOM"}, {ReviewCat::NoPartNumber, "no P/N"},
        {ReviewCat::Duplicate, "listed twice"}, {ReviewCat::Dnp, "DNP"}};
    std::string s;
    for (auto& [c, n] : names) if (f & bit(c)) { if (!s.empty()) s += ", "; s += n; }
    return s;
}
bool contains_ci(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    return io::to_upper(hay).find(io::to_upper(needle)) != std::string::npos;
}

// clickable counter chip: toggles a category filter
bool chip(const char* label, int n, ImU32 col, bool on, const char* tip) {
    const GTheme& t = th();
    std::string s = std::to_string(n) + "  " + label;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(s.c_str());
    ImVec2 sz(ts.x + S(20), ImGui::GetFrameHeight());
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##chip", sz);
    bool hv = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), col_alpha(col, on ? 0.45f : hv ? 0.24f : 0.14f), S(8));
    if (on) dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), col, S(8), 0, S(1.5f));
    dl->AddText(ImVec2(p.x + S(10), p.y + (sz.y - ts.y) * 0.5f), t.text, s.c_str());
    if (tip) tooltip(tip);
    ImGui::SameLine(0, S(6));
    return clicked;
}

struct Row {                            // component view row (BOM ref or board-only placement)
    std::string ref, state, pn, package, description;
    int line = 0;
    uint32_t flags = 0;
    bool on_board = false, dnp = false, in_bom = true;
};

void export_report(const io::BomReview& r) {
    defer([r]() {   // native dialog between frames
        std::wstring p = app::settings().dir_bom;
        std::wstring base = ui::W(r.job.empty() ? std::string("BOM review") : "BOM review " + r.job);
        for (auto& ch : base) if (ch == L'/' || ch == L'\\' || ch == L':') ch = L'_';
        p = (p.empty() ? L"" : p + L"\\") + base + L".txt";
        if (!ui::save_file_dialog(app::frame_hwnd(), L"Export BOM Review Report",
                                  {{L"Text report (*.txt)", L"*.txt"}, {L"CSV (*.csv)", L"*.csv"}}, L"txt", p)) return;
        const bool csv = p.size() > 4 && (_wcsicmp(p.c_str() + p.size() - 4, L".csv") == 0);
        std::string body = csv ? io::bom_review_csv(r) : io::bom_review_text(r);
        if (!csv) { std::string crlf; for (char c : body) { if (c == '\n') crlf += '\r'; crlf += c; } body = crlf; }
        std::string err;
        if (write_file(ui::U(p), body.data(), body.size(), err)) toast("Report saved: " + ui::U(p), ToastKind::Success);
        else toast("Could not save the report: " + err, ToastKind::Error, 5.f);
    });
}
} // namespace

void draw_bom_review(BomReviewUi& st, const io::BomPreview& prev, const std::set<std::string>& board_refs,
                     const std::string& source, const std::string& job, float w, float h) {
    const GTheme& t = th();
    st.review = io::bom_review(prev, board_refs, source, job);
    const io::BomReview& R = st.review;
    int dnp_n = R.count(ReviewCat::Dnp);

    // ---- counters / filter chips ----
    {
        ImGui::AlignTextToFramePadding();
        auto stat = [&](const char* label, int n, ImU32 col) {
            std::string s = std::to_string(n) + "  " + label;
            ImVec2 p = ImGui::GetCursorScreenPos(), ts = ImGui::CalcTextSize(s.c_str());
            ImVec2 b(p.x + ts.x + S(20), p.y + ImGui::GetFrameHeight());
            ImGui::GetWindowDrawList()->AddRectFilled(p, b, col_alpha(col, 0.14f), S(8));
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + S(10), p.y + (b.y - p.y - ts.y) * 0.5f), t.text, s.c_str());
            ImGui::Dummy(ImVec2(b.x - p.x, b.y - p.y));
            ImGui::SameLine(0, S(6));
        };
        stat("records", R.bom_lines, t.ok);
        stat("part numbers", R.part_numbers, t.accent);
        stat(("matched of " + std::to_string(R.board_placements) + " placements").c_str(), R.matched, R.matched ? t.ok : t.warn);
        struct C { ReviewCat c; const char* label; const char* tip; };
        static const C chips[] = {
            {ReviewCat::NotOnBoard, "missing refs (not on board)", "BOM references with no board component. Click: show only these rows."},
            {ReviewCat::Unmatched, "unmatched placements", "Board placements the BOM does not list. Click: show only these rows."},
            {ReviewCat::QtyMismatch, "qty mismatches", "Lines whose Quantity differs from the reference count. Click: show only their references."},
            {ReviewCat::PnConflict, "P/N conflicts", "References on several lines with different parts. Click: show only these rows."},
            {ReviewCat::Dnp, "DNP", "References marked not populated. Click: show only these rows."}};
        for (auto& c : chips) {
            int n = 0;
            if (c.c == ReviewCat::QtyMismatch) n = prev.qty_mismatches;
            else n = R.count(c.c);
            (void)dnp_n;
            ImU32 col = n == 0 ? t.text_faint : io::review_cat_sev(c.c) == io::IssueSev::Error ? t.error : io::review_cat_sev(c.c) == io::IssueSev::Info ? t.accent : t.warn;
            if (chip(c.label, n, col, (st.chip_mask & bit(c.c)) != 0, c.tip)) st.chip_mask ^= bit(c.c);
        }
        if (st.chip_mask) { if (button("Clear filter")) st.chip_mask = 0; }
        ImGui::NewLine();
    }
    // ---- toolbar: placement report, view mode, filter, issues only, report ----
    if (button("Placement report without BOM", ui::Icon::Report)) post_command(app::ID_MFG_PLACEMENT_REPORT);
    tooltip("Skip the BOM: list / export the raw placement coordinates as they are");
    ImGui::SameLine(0, S(14));
    if (button("Component", ui::Icon::PackageEditor, st.view == 0)) st.view = 0;
    ImGui::SameLine(0, S(4));
    if (button("Part Number", ui::Icon::Bom, st.view == 1)) st.view = 1;
    ImGui::SameLine(0, S(14));
    ImGui::SetNextItemWidth(S(180));
    ImGui::InputTextWithHint("##flt", "Filter (ref, P/N, text)", st.filter, sizeof st.filter);
    ImGui::SameLine(0, S(10));
    ImGui::Checkbox("Show issues only", &st.issues_only);
    ImGui::SameLine(0, S(14));
    if (button("Copy report", ui::Icon::Copy)) { ImGui::SetClipboardText(io::bom_review_text(R).c_str()); toast("Review report copied to the clipboard"); }
    tooltip("Plain-text report (header, counts, every finding by category), ready to paste into an e-mail");
    ImGui::SameLine(0, S(4));
    if (button("Export report\xE2\x80\xA6", ui::Icon::Export)) export_report(R);
    tooltip("Save the review report as .txt (e-mail ready) or .csv (one finding per row)");

    // ---- splitter geometry ----
    const float top = ImGui::GetCursorPosY();
    const float avail = std::max(S(160), h - top - S(4));
    const float bar = S(8), hdr = ImGui::GetFrameHeight() + S(4);
    float table_h = st.summary_max ? 0.f : std::clamp(avail * st.split, S(80), avail - hdr - bar - S(60));
    const float sum_h = avail - table_h - bar - hdr;
    const std::string f = st.filter;

    // ---- table ----
    const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                               ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;
    if (table_h > 0 && st.view == 0) {
        std::vector<Row> rows;
        for (auto& c : prev.by_comp) {
            Row r;
            r.ref = c.ref; r.pn = c.dnp ? "" : c.pn; r.package = c.package; r.description = c.description; r.line = c.line;
            r.on_board = board_refs.count(io::to_upper(c.ref)) > 0; r.dnp = c.dnp;
            r.flags = R.flags_of(io::to_upper(c.ref));
            if (c.conflict) r.flags |= bit(ReviewCat::PnConflict);
            r.state = c.dnp ? "DNP" : !r.on_board ? "not on board" : c.pn.empty() ? "unassigned" : "linked";
            rows.push_back(std::move(r));
        }
        if (const io::ReviewGroup* g = R.group(ReviewCat::Unmatched))
            for (auto& it : g->items) { Row r; r.ref = it.ref; r.in_bom = false; r.on_board = true; r.state = "not in BOM"; r.flags = bit(ReviewCat::Unmatched); rows.push_back(r); }
        std::vector<int> idx;
        for (int i = 0; i < (int)rows.size(); ++i) {
            const Row& r = rows[i];
            if (st.issues_only && !(r.flags & (kErr | kWarn))) continue;
            if (st.chip_mask && !(r.flags & st.chip_mask)) continue;
            if (!f.empty() && !contains_ci(r.ref, f) && !contains_ci(r.pn, f) && !contains_ci(r.description, f) && !contains_ci(r.state, f) &&
                !contains_ci(flag_text(r.flags), f)) continue;
            idx.push_back(i);
        }
        if (ImGui::BeginTable("##rv_comp", 7, tf, ImVec2(w, table_h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Reference", ImGuiTableColumnFlags_DefaultSort, 0.8f);
            ImGui::TableSetupColumn("State", 0, 0.9f);
            ImGui::TableSetupColumn("Issues", 0, 1.3f);
            ImGui::TableSetupColumn("Part Number", 0, 1.4f);
            ImGui::TableSetupColumn("Package (BOM)", 0, 0.8f);
            ImGui::TableSetupColumn("Description", 0, 2.0f);
            ImGui::TableSetupColumn("Line", 0, 0.4f);
            ImGui::TableHeadersRow();
            if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount > 0) {
                const int col = ss->Specs[0].ColumnIndex;
                const bool asc = ss->Specs[0].SortDirection != ImGuiSortDirection_Descending;
                std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
                    const Row &x = rows[a], &y = rows[b];
                    bool lt, gt;
                    switch (col) {
                    case 1: lt = x.state < y.state; gt = y.state < x.state; break;
                    case 2: lt = flag_text(x.flags) < flag_text(y.flags); gt = flag_text(y.flags) < flag_text(x.flags); break;
                    case 3: lt = x.pn < y.pn; gt = y.pn < x.pn; break;
                    case 4: lt = x.package < y.package; gt = y.package < x.package; break;
                    case 5: lt = x.description < y.description; gt = y.description < x.description; break;
                    case 6: lt = x.line < y.line; gt = y.line < x.line; break;
                    default: lt = ref_less(x.ref, y.ref); gt = ref_less(y.ref, x.ref); break;
                    }
                    return asc ? lt : gt;
                });
            }
            ImGuiListClipper clip;
            clip.Begin((int)idx.size());
            while (clip.Step())
                for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
                    const Row& r = rows[idx[k]];
                    const ImU32 rc = flag_col(r.flags, t.text);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextColored(C4(rc), "%s", r.ref.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextColored(C4(r.flags & (kErr | kWarn) ? rc : r.dnp ? t.accent : r.state == "linked" ? t.ok : t.text_dim), "%s", r.state.c_str());
                    if (ImGui::IsItemHovered())
                        tooltip(r.dnp ? "Explicitly not populated in the BOM: kept on the board, excluded, no part number."
                              : !r.in_bom ? "Board placement that no BOM line lists."
                              : !r.on_board ? "Listed in the BOM but no board component has this reference."
                              : r.pn.empty() ? "On the board and in the BOM, but the BOM line has no part number." : "Linked to the BOM part number.");
                    ImGui::TableNextColumn();
                    if (r.flags) ImGui::TextColored(C4(flag_col(r.flags, t.accent)), "%s", flag_text(r.flags).c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(r.pn.c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(r.package.c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(r.description.c_str());
                    ImGui::TableNextColumn(); if (r.line) ImGui::TextDisabled("%d", r.line);
                }
            ImGui::EndTable();
        }
    } else if (table_h > 0) {
        std::vector<int> idx;
        std::vector<uint32_t> pf(prev.by_part.size(), 0);
        for (size_t i = 0; i < prev.by_part.size(); ++i) {
            const io::BomPartRow& x = prev.by_part[i];
            for (auto& r : x.refs) pf[i] |= R.flags_of(io::to_upper(r));
            if (x.qty_file >= 0 && x.qty_file != (int)x.refs.size()) pf[i] |= bit(ReviewCat::QtyMismatch);
            if (st.issues_only && !(pf[i] & (kErr | kWarn))) continue;
            if (st.chip_mask && !(pf[i] & st.chip_mask)) continue;
            bool refhit = false;
            for (auto& r : x.refs) refhit |= contains_ci(r, f);
            if (!f.empty() && !refhit && !contains_ci(x.pn, f) && !contains_ci(x.description, f) && !contains_ci(x.value, f)) continue;
            idx.push_back((int)i);
        }
        if (ImGui::BeginTable("##rv_part", 7, tf, ImVec2(w, table_h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Part Number", ImGuiTableColumnFlags_DefaultSort, 1.3f);
            ImGui::TableSetupColumn("Qty", 0, 0.4f);
            ImGui::TableSetupColumn("File Qty", 0, 0.5f);
            ImGui::TableSetupColumn("Issues", 0, 1.2f);
            ImGui::TableSetupColumn("References", 0, 2.6f);
            ImGui::TableSetupColumn("Package (BOM)", 0, 0.8f);
            ImGui::TableSetupColumn("Description", 0, 1.8f);
            ImGui::TableHeadersRow();
            if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount > 0) {
                const int col = ss->Specs[0].ColumnIndex;
                const bool asc = ss->Specs[0].SortDirection != ImGuiSortDirection_Descending;
                std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
                    const io::BomPartRow &x = prev.by_part[a], &y = prev.by_part[b];
                    bool lt, gt;
                    switch (col) {
                    case 1: lt = x.refs.size() < y.refs.size(); gt = y.refs.size() < x.refs.size(); break;
                    case 2: lt = x.qty_file < y.qty_file; gt = y.qty_file < x.qty_file; break;
                    case 3: lt = flag_text(pf[a]) < flag_text(pf[b]); gt = flag_text(pf[b]) < flag_text(pf[a]); break;
                    case 4: lt = (x.refs.empty() ? "" : x.refs[0]) < (y.refs.empty() ? "" : y.refs[0]); gt = (y.refs.empty() ? "" : y.refs[0]) < (x.refs.empty() ? "" : x.refs[0]); break;
                    case 5: lt = x.package < y.package; gt = y.package < x.package; break;
                    case 6: lt = x.description < y.description; gt = y.description < x.description; break;
                    default: lt = x.pn < y.pn; gt = y.pn < x.pn; break;
                    }
                    return asc ? lt : gt;
                });
            }
            ImGuiListClipper clip;
            clip.Begin((int)idx.size());
            while (clip.Step())
                for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
                    const io::BomPartRow& x = prev.by_part[idx[k]];
                    const uint32_t fl = pf[idx[k]];
                    const ImU32 rc = flag_col(fl, t.text);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextColored(C4(rc), "%s%s", x.pn.c_str(), x.dnp ? "  (DNP)" : "");
                    ImGui::TableNextColumn(); ImGui::Text("%d", (int)x.refs.size());
                    ImGui::TableNextColumn(); if (x.qty_file >= 0) ImGui::TextColored(C4(x.qty_file != (int)x.refs.size() ? t.warn : t.text_dim), "%d", x.qty_file);
                    ImGui::TableNextColumn(); if (fl) ImGui::TextColored(C4(flag_col(fl, t.accent)), "%s", flag_text(fl).c_str());
                    ImGui::TableNextColumn();
                    {
                        std::string s;
                        for (size_t i = 0; i < x.refs.size(); ++i) { if (i) s += ", "; s += x.refs[i]; }
                        ImGui::TextUnformatted(s.c_str());
                        if (ImGui::IsItemHovered() && x.refs.size() > 8) tooltip((std::to_string(x.refs.size()) + " references\n" + ref_list_capped(x.refs)).c_str());
                    }
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(x.package.c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(x.description.c_str());
                }
            ImGui::EndTable();
        }
    }

    // ---- splitter bar ----
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##rv_split", ImVec2(w, bar));
        const bool act = ImGui::IsItemActive(), hv = ImGui::IsItemHovered();
        if (hv || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (act && ImGui::GetIO().MouseDelta.y != 0 && !st.summary_max)
            st.split = std::clamp((table_h + ImGui::GetIO().MouseDelta.y) / avail, 0.15f, 0.9f);
        if (hv && ImGui::IsMouseDoubleClicked(0)) st.split = 0.62f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + w * 0.5f - S(24), p.y + bar * 0.5f - S(1.5f)), ImVec2(p.x + w * 0.5f + S(24), p.y + bar * 0.5f + S(1.5f)),
                                                  hv || act ? t.accent : t.border, S(2));
        if (hv) tooltip("Drag to resize the table and the summary (double-click: default)");
    }
    // ---- summary header ----
    int ne = 0, nw = 0;
    for (auto& g : R.groups) {
        auto s = io::review_cat_sev(g.cat);
        if (s == io::IssueSev::Error) ne += (int)g.items.size(); else if (s == io::IssueSev::Warning) nw += (int)g.items.size();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Summary");
    ImGui::SameLine();
    ImGui::TextColored(C4(ne ? t.error : t.text_faint), "%d error(s)", ne);
    ImGui::SameLine();
    ImGui::TextColored(C4(nw ? t.warn : t.text_faint), "%d warning(s)", nw);
    ImGui::SameLine();
    ImGui::TextColored(C4(t.text_faint), "\xC2\xB7 click to select, Ctrl+click for more, right-click to copy");
    ImGui::SameLine(w - S(110));
    if (button(st.summary_max ? "Restore" : "Maximize", ui::Icon::Maximize, st.summary_max)) st.summary_max = !st.summary_max;
    tooltip(st.summary_max ? "Show the review table again" : "Give the whole step to the summary");

    // ---- summary body: grouped, expandable, selectable ----
    std::function<std::string()> copy_sel;
    if (ImGui::BeginChild("##rv_sum", ImVec2(w, std::max(S(60), sum_h)), ImGuiChildFlags_Borders)) {
        std::string hovered_line;
        const ReviewCat* hovered_group = nullptr;
        for (size_t gi = 0; gi < R.groups.size(); ++gi) {
            const io::ReviewGroup& g = R.groups[gi];
            const io::IssueSev sev = io::review_cat_sev(g.cat);
            const ImU32 col = sev == io::IssueSev::Error ? t.error : sev == io::IssueSev::Warning ? t.warn : t.text_dim;
            ImGui::PushID((int)gi);
            ImGui::PushStyleColor(ImGuiCol_Text, C4(col));
            std::string head = std::string(io::issue_sev_name(sev)) + "   " + io::review_cat_title(g.cat) + "  (" + std::to_string(g.items.size()) + ")";
            const bool open = ImGui::CollapsingHeader(head.c_str(), sev != io::IssueSev::Info ? ImGuiTreeNodeFlags_DefaultOpen : 0);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) hovered_group = &g.cat;
            if (open) {
                ImGui::Indent(S(14));
                if (g.cat == ReviewCat::Unmatched || g.cat == ReviewCat::NotOnBoard || g.cat == ReviewCat::PnConflict) {
                    std::string refs;
                    for (auto& it : g.items) { if (!refs.empty()) refs += ", "; refs += it.ref; }
                    ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - S(8));
                    ImGui::TextColored(C4(t.text), "%s", refs.c_str());
                    ImGui::PopTextWrapPos();
                    if (ImGui::IsItemHovered()) hovered_line = refs;
                }
                if (g.cat != ReviewCat::Unmatched)
                    for (size_t i = 0; i < g.items.size(); ++i) {
                        const std::string key = std::to_string(gi) + ":" + std::to_string(i);
                        const bool sel = st.sel.count(key) > 0;
                        ImGui::PushID((int)i);
                        if (ImGui::Selectable(g.items[i].text.c_str(), sel)) {
                            if (!ImGui::GetIO().KeyCtrl) st.sel.clear();
                            if (sel && ImGui::GetIO().KeyCtrl) st.sel.erase(key); else st.sel.insert(key);
                        }
                        if (ImGui::IsItemHovered()) hovered_line = g.items[i].text;
                        ImGui::PopID();
                    }
                ImGui::Unindent(S(14));
            }
            ImGui::PopID();
        }
        if (R.groups.empty()) ImGui::TextColored(C4(t.ok), "No findings: every BOM reference is on the board and every placement is in the BOM.");
        auto selected_text = [&]() {
            std::string o;
            for (size_t gi = 0; gi < R.groups.size(); ++gi)
                for (size_t i = 0; i < R.groups[gi].items.size(); ++i)
                    if (st.sel.count(std::to_string(gi) + ":" + std::to_string(i))) o += R.groups[gi].items[i].text + "\n";
            return o;
        };
        if (ImGui::IsWindowFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && !st.sel.empty())
            ImGui::SetClipboardText(selected_text().c_str());
        static std::string s_ctx_line;
        static int s_ctx_group = -1;
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            s_ctx_line = hovered_line;
            s_ctx_group = hovered_group ? (int)*hovered_group : -1;
            ImGui::OpenPopup("##rv_ctx");
        }
        if (ImGui::BeginPopup("##rv_ctx")) {
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, !st.sel.empty() || !s_ctx_line.empty()))
                ImGui::SetClipboardText(!st.sel.empty() ? selected_text().c_str() : s_ctx_line.c_str());
            if (s_ctx_group >= 0)
                if (const io::ReviewGroup* g = R.group((ReviewCat)s_ctx_group))
                    if (ImGui::MenuItem("Copy this section")) ImGui::SetClipboardText(io::bom_review_group_text(*g).c_str());
            if (ImGui::MenuItem("Copy all")) {
                std::string o;
                for (auto& g : R.groups) o += io::bom_review_group_text(g) + "\n";
                ImGui::SetClipboardText(o.c_str());
            }
            if (ImGui::MenuItem("Copy as report")) { ImGui::SetClipboardText(io::bom_review_text(R).c_str()); toast("Review report copied to the clipboard"); }
            ImGui::Separator();
            if (ImGui::MenuItem("Export report\xE2\x80\xA6")) export_report(R);
            if (ImGui::MenuItem("Clear selection", nullptr, false, !st.sel.empty())) st.sel.clear();
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
}

} // namespace pcbp::gui
