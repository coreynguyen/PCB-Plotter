// ============================================================================
// shell.cpp — PCB Plotter v2 main window: Dear ImGui + D3D11.
//
//   [menu bar ............................................ theme switch]
//   [toolbar: global tools | active view tools]
//   [document tabs  (+)]
//   [               active view of the active document               ]
//   [view tabs: Plotter | Gerber | Visual Aid | Machine | Notes | ... ]
//   [status bar: message | view status | coords | units              ]
// ============================================================================
#include "../core/autosave.h"
#include "../platform/crash.h"
#include <thread>
#include "gapp.h"
#include "../core/package_edit.h"
#include "gfx.h"
#include "import_extras.h"
#include <ctime>
#include "about_audio.h"
#include <thread>
#include "../core/project_io.h"
#include "../core/sides.h"   // v17 project_program_name
#include "../core/machine_job.h"   // R18a tape_rot_notice
#include "../core/arrays.h"   // R22 outline / array commands
#include "../ui/form.h"
#include "../ui/theme.h"
#include "../platform/win_shell.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "input_resync.h"   // R20
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cwctype>   // std::iswdigit
#include <deque>
#include <map>
#include <set>
#include <unordered_map>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace pcbp::app { void recovery_startup(); }
namespace pcbp::gui {
void show_migrate_dialog();   // gui/migrate_dialog.cpp (declared at namespace scope: see builtin_plugins.cpp, MSVC 14.51)
void group_ops_menu_items(const app::DocPtr& d);   // group_ops.cpp
bool gerber_dialog_drop(const std::vector<std::wstring>&);
bool job_files_drop(const app::DocPtr& d, const std::vector<std::wstring>& paths);   // R19b job_files_view.cpp
void set_dpi_scale(float s);
void rebuild_fonts_for_dpi(float s);
void draw_modals();
bool import_auto(const std::wstring& path);   // file_assoc_dialog.cpp (content detection)
void file_assoc_dialog();                      // file_assoc_dialog.cpp
std::vector<ui::Form*>& modal_stack();
std::string g_walk_menu;   // ID walk (PCBP_IDWALK): top-level menu held open by draw_menu_bar

std::string g_focus_package;
int g_report_request = 0;

namespace {
HWND g_hwnd = nullptr;
bool g_quit = false;
bool g_in_frame = false;
int g_redraw = 3;
std::deque<std::function<void()>> g_deferred;
int g_bom_import_kind = 0;   // R12: BOM menu: 0 any, 1 text / CSV, 2 spreadsheet (consumed by ID_BOM_IMPORT)
std::vector<std::unique_ptr<DocUI>> g_docs;
int g_active = -1;              // index into g_docs; -1 = landing page
std::string g_status = "Ready";
double g_status_time = 0;
bool g_show_landing = true;

// tab labels indexed by ViewKind (Visual Aid kept in code but not shown)
const char* kViewNames[] = {"Plots", "Gerber", "Visual Aid", "Export", "Info", "Placements", "BOM", "Shapes", "Reports", "Part numbers & packages", "Fiducials", "Validation", "Files"};
// R10 §4 workflow order (Ctrl+Tab cycling): Project Info -> Plot -> Gerber -> Packages -> Review (Placements, BOM,
// Part numbers & DB, Reports) -> Export (always last)
// R19a: bottom tabs Info, Plots, Data, Gerber, Shapes, Files, Export (uniform width, short labels)
const ViewKind kTabOrder[] = {ViewKind::Notes, ViewKind::Plotter, ViewKind::Placements, ViewKind::Bom, ViewKind::Components, ViewKind::Fiducials,
                              ViewKind::Validation, ViewKind::Reports, ViewKind::Gerber, ViewKind::Packages, ViewKind::Files, ViewKind::Machine};
// top-level workspace tabs; Review groups its views as sub-tabs
struct TabGroup { const char* name; ViewKind first; std::vector<ViewKind> members; const char* tip; };
const std::vector<TabGroup>& tab_groups() {
    static const std::vector<TabGroup> g = {
        {"Info", ViewKind::Notes, {ViewKind::Notes}, "Project info: customer, assembly, revisions, author, notes and revision history"},
        {"Plots", ViewKind::Plotter, {ViewKind::Plotter}, "Board display: component locations, fiducials, manipulation"},
        {"Data", ViewKind::Placements, {ViewKind::Placements, ViewKind::Bom, ViewKind::Components, ViewKind::Fiducials, ViewKind::Validation, ViewKind::Reports},
         "Data: inspect and audit placements, BOM, part numbers & packages, fiducials, validation and reports (editing stays in Plots)"},   // R12 §3 (was "Components" / "Review")
        {"Gerber", ViewKind::Gerber, {ViewKind::Gerber}, "Artwork layers, pads, component creation from pads"},
        {"Shapes", ViewKind::Packages, {ViewKind::Packages}, "Package shapes: geometry, orientation and polarity markers"},
        {"Files", ViewKind::Files, {ViewKind::Files}, "Project files and documents"},
        {"Export", ViewKind::Machine, {ViewKind::Machine}, "Export layers, machine program configuration and generation (last step)"},
    };
    return g;
}
std::map<const app::Doc*, ViewKind> g_review_last;   // last Review sub-tab per document
int tab_group_of(ViewKind k) {
    auto& g = tab_groups();
    for (size_t i = 0; i < g.size(); ++i) for (ViewKind m : g[i].members) if (m == k) return (int)i;
    return 1;
}
constexpr int kTabCount = (int)(sizeof(kTabOrder) / sizeof(kTabOrder[0]));
// pastel accent per tab (by ViewKind): {dark theme, light theme}
const ImU32 kTabAccent[][2] = {
    {IM_COL32(110, 200, 255, 255), IM_COL32(20, 110, 190, 255)},   // Plot      sky
    {IM_COL32(120, 225, 150, 255), IM_COL32(25, 135, 60, 255)},    // Gerber    mint
    {IM_COL32(200, 200, 200, 255), IM_COL32(110, 110, 110, 255)},  // VisualAid (hidden)
    {IM_COL32(255, 150, 140, 255), IM_COL32(195, 55, 45, 255)},    // Export    coral
    {IM_COL32(240, 220, 120, 255), IM_COL32(150, 115, 0, 255)},    // Notes     butter
    {IM_COL32(150, 180, 255, 255), IM_COL32(55, 85, 200, 255)},    // Placements periwinkle
    {IM_COL32(255, 190, 120, 255), IM_COL32(190, 100, 10, 255)},   // BOM       apricot
    {IM_COL32(205, 160, 255, 255), IM_COL32(120, 60, 190, 255)},   // Packages  lavender
    {IM_COL32(130, 220, 220, 255), IM_COL32(0, 125, 125, 255)},    // Reports   aqua
    {IM_COL32(255, 160, 210, 255), IM_COL32(185, 50, 120, 255)},   // Components pink
    {IM_COL32(170, 230, 120, 255), IM_COL32(80, 140, 20, 255)},    // Fiducials lime
    {IM_COL32(255, 120, 120, 255), IM_COL32(190, 40, 40, 255)},    // Validation red
    {IM_COL32(180, 200, 170, 255), IM_COL32(80, 110, 70, 255)},    // Files     sage
};
constexpr int kDataGroup = 2;   // index of "Data" in tab_groups()
std::map<const app::Doc*, bool> g_view_bottom;   // viewed side per document (X-ray on)
ui::Icon kViewIcons[] = {ui::Icon::UniversalView, ui::Icon::GerberView, ui::Icon::VisualAid, ui::Icon::Export, ui::Icon::Notes,
                         ui::Icon::Locations, ui::Icon::Bom, ui::Icon::PackageEditor, ui::Icon::Report, ui::Icon::SelectPart,
                         ui::Icon::Fiducial, ui::Icon::Warning, ui::Icon::Folder};
static_assert(sizeof kViewNames / sizeof kViewNames[0] == (size_t)ViewKind::Count, "kViewNames per ViewKind");
static_assert(sizeof kTabAccent / sizeof kTabAccent[0] == (size_t)ViewKind::Count, "kTabAccent per ViewKind");
static_assert(sizeof kViewIcons / sizeof kViewIcons[0] == (size_t)ViewKind::Count, "kViewIcons per ViewKind");

std::string strip_menu(const wchar_t* t, std::string* shortcut) {
    std::wstring s = t ? t : L"";
    size_t tab = s.find(L'\t');
    if (shortcut) *shortcut = tab == std::wstring::npos ? std::string() : ui::U(s.substr(tab + 1));
    if (tab != std::wstring::npos) s.resize(tab);
    std::wstring o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'&') { if (i + 1 < s.size() && s[i + 1] == L'&') { o += L'&'; ++i; } continue; }
        o += s[i];
    }
    return ui::U(o);
}
} // namespace

// ===========================================================================
// small services
// ===========================================================================
HWND main_hwnd() { return g_hwnd; }
std::string strip_menu_u8(const wchar_t* t, std::string* shortcut) { return strip_menu(t, shortcut); }
void draw_feedback();
static void appearance_dialog();
int tab_placement();
static void add_placement_dialog(const app::DocPtr& d);
static void duplicate_placements(const app::DocPtr& d);
// R10 §13 export layers (= ModelPoints, see specs/EXPORT_LAYERS.md): multi-selection in the panel
std::map<const app::Doc*, std::set<int>> g_layer_sel;
static std::set<int> layer_selection(const app::Doc& d) {
    std::set<int> r;
    auto it = g_layer_sel.find(&d);
    if (it != g_layer_sel.end()) for (int i : it->second) if (i >= 0 && i < (int)d.prj.model.points.size()) r.insert(i);
    if (r.empty() && d.active_point >= 0 && d.active_point < (int)d.prj.model.points.size()) r.insert(d.active_point);
    return r;
}
static void delete_layers(const app::DocPtr& d, const std::set<int>& del);
bool perf_overlay_on();
void perf_overlay_toggle();
ui::Icon toast_icon(ToastKind k);
ImU32 toast_color(ToastKind k);
int g_status_kind = -1;
bool in_frame() { return g_in_frame; }
void request_redraw(int frames) { g_redraw = std::max(g_redraw, frames); }
void defer(std::function<void()> fn) { g_deferred.push_back(std::move(fn)); request_redraw(2); }
void status(const std::string& msg, ToastKind k) { status(msg); g_status_kind = (int)k; }
void status(const std::string& msg) { g_status_kind = -1; g_status = msg; g_status_time = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0; request_redraw(2); }
std::string W2U(const std::wstring& w) { return ui::U(w); }

std::string ref_list_capped(const std::vector<std::string>& refs, size_t total, size_t max_refs) {
    if (total < refs.size()) total = refs.size();
    std::string s;
    const size_t n = std::min(refs.size(), max_refs);
    for (size_t i = 0; i < n; ++i) { if (i) s += ", "; s += refs[i]; }
    if (total > n) s += (n ? "  \xE2\x80\xA6+" : "\xE2\x80\xA6+") + std::to_string(total - n) + " more";
    return s;
}
static ViewFactory* view_factories() { static ViewFactory f[(int)ViewKind::Count] = {}; return f; }
bool register_view_factory(ViewKind k, ViewFactory f) {
    if ((int)k < 0 || k >= ViewKind::Count) return false;
    view_factories()[(int)k] = f;
    return true;
}
namespace {
struct PendingView : View {   // R19a: placeholder until a view registers its factory
    ViewKind k;
    explicit PendingView(ViewKind kk) : k(kk) {}
    ViewKind kind() const override { return k; }
    const char* title() const override { return "Files"; }
    ui::Icon icon() const override { return ui::Icon::Folder; }
    void draw(ImVec2) override { ImGui::Dummy(ImVec2(0, 20)); ImGui::Indent(20); ImGui::TextDisabled("Project files are not available in this build."); ImGui::Unindent(20); }
};
}
View* DocUI::view(ViewKind k) {
    auto& v = views[(int)k];
    if (!v && (int)k >= 0 && k < ViewKind::Count && view_factories()[(int)k]) { v = view_factories()[(int)k](); if (v) v->doc = doc; }
    if (!v) {
        switch (k) {
        case ViewKind::Plotter: v = make_plotter_view(); break;
        case ViewKind::Gerber: v = make_gerber_view(); break;
        case ViewKind::VisualAid: v = make_visual_aid_view(); break;
        case ViewKind::Machine: v = make_machine_view(); break;
        case ViewKind::Notes: v = make_notes_view(); break;
        case ViewKind::Placements: v = make_placements_view(); break;
        case ViewKind::Bom: v = make_bom_view(); break;
        case ViewKind::Packages: v = make_packages_view(); break;
        case ViewKind::Reports: v = make_reports_view(); break;
        case ViewKind::Components: v = make_components_view(); break;
        case ViewKind::Fiducials: v = make_fiducials_view(); break;
        case ViewKind::Validation: v = make_validation_view(); break;
        case ViewKind::Files: v = std::make_unique<PendingView>(k); break;
        default: break;
        }
        if (v) v->doc = doc;
    }
    return v.get();
}

DocUI* active_docui() { return (g_active >= 0 && g_active < (int)g_docs.size() && !g_show_landing && !files_active()) ? g_docs[g_active].get() : nullptr; }
DocUI* docui_of(const app::Doc* d) { for (auto& x : g_docs) if (x->doc.get() == d) return x.get(); return nullptr; }

void activate_view(const app::DocPtr& d, ViewKind k) {
    for (size_t i = 0; i < g_docs.size(); ++i) if (g_docs[i]->doc == d) {
        g_active = (int)i;
        g_show_landing = false;
        g_docs[i]->active = k;
        if (View* v = g_docs[i]->view(k)) v->on_activate();
        request_redraw(3);
        return;
    }
}

void zoom_extents_doc(const app::DocPtr& d) {
    defer([d]() {
        for (auto& x : g_docs) if (x->doc == d) {
            for (ViewKind k : {ViewKind::Plotter, ViewKind::Gerber}) if (View* v = x->views[(int)k].get()) v->on_command(app::ID_VIEW_ZOOM_ALL);
        }
        request_redraw(3);
    });
}

View* find_view(const app::DocPtr& d, ViewKind k) {
    for (auto& x : g_docs) if (x->doc == d) return x->views[(int)k].get();
    return nullptr;
}

int current_point(const app::Doc& d) {
    if (d.prj.model.points.empty()) return -1;
    return std::clamp(d.active_point, 0, (int)d.prj.model.points.size() - 1);
}
void set_current_point(app::Doc& d, int p) { d.active_point = p; request_redraw(2); }

std::string fmt_len_u8(double mm, bool units) { return ui::U(app::fmt_len(mm, units)); }
std::string fmt_pos_u8(const app::Doc& d, Vec2 p) {
    Vec2 r = d.prj.to_user(p);
    return "(" + fmt_len_u8(r.x, false) + ", " + fmt_len_u8(r.y, false) + ")";
}

void post_command(int id);

// ===========================================================================
// commands
// ===========================================================================
static app::DocPtr adoc() { DocUI* u = active_docui(); return u ? u->doc : nullptr; }

int command_state(int id) {
    using namespace app;
    if (DocUI* u = active_docui()) if (View* v = u->current()) { int s = v->cmd_state(id); if (s >= 0) return s; }
    if (files_active() && (id == ID_FILE_SAVE || id == ID_FILE_SAVEAS || id == ID_FILE_CLOSE || id == ID_EDIT_UNDO || id == ID_EDIT_REDO || id == ID_EDIT_COPY || id == ID_EDIT_CUT || id == ID_EDIT_PASTE)) return 1;
    DocPtr d = adoc();
    auto& s = settings();
    const int EN = 1, CK = 2;
    auto en = [&](bool b) { return b ? EN : 0; };
    switch (id) {
    case ID_FILE_NEW: case ID_FILE_OPEN: case ID_FILE_OPEN_CPF: case ID_FILE_MIGRATE_BATCH: case ID_FILE_EXIT: case ID_EDIT_OPTIONS: case ID_HELP_ABOUT:
    case ID_HELP_CONTENTS: case ID_HELP_SHORTCUTS: case ID_HELP_VERIFY: case ID_LIB_PLUGINS: case ID_EDIT_GRID_SPACING:
    case ID_FILE_PRINT_SETUP: case ID_FILE_HOME: case ID_IMPORT_MACHINE:
        return EN;
    case ID_VIEW_APPEARANCE: return EN;
    case ID_FILE_PRINT: case ID_FILE_PRINT_1TO1: case ID_FILE_PRINT_PREVIEW: {
        DocUI* u = active_docui(); View* v = u ? u->current() : nullptr;
        return en(v && v->can_print());
    }
    case ID_VIEW_PERF_OVERLAY: return EN | (perf_overlay_on() ? CK : 0);
    case ID_PRJ_CHECKPOINT: return en(d != nullptr);
    case ID_FILE_RECOVERY_SNAPSHOTS: return EN;
    case ID_BOARD_ALIGN_CAD_GERBER: return en(d != nullptr);
    case ID_BOARD_CONFIRM_OUTLINE: return en(d && !d->prj.board_outline_confirmed());
    case ID_BOARD_CLEAR_PANEL_FRAME: return en(d && !d->prj.panel_frame.empty());   // R22 Delete Board Outline
    case ID_BOARD_CLEAR_IMAGE_FRAME: return en(d && !d->prj.board_frame.empty());   // R22 Delete Circuit Outline
    case ID_BOARD_ARRAY_DELETE: return en(d && d->prj.is_panel());                 // R22 Delete Array
    case ID_EDIT_DUPLICATE: return en(d && d->selected_count() > 0);
    case ID_EDIT_UNDO: return en(d && d->can_undo());
    case ID_EDIT_REDO: return en(d && d->can_redo());
    case ID_EDIT_SNAP: return EN | (s.snap ? CK : 0);
    case ID_EDIT_GRID: return EN | (s.grid_visible ? CK : 0);
    case ID_VIEW_THEME_SYSTEM: return EN | (ui::theme_mode() == ui::ThemeMode::System ? CK : 0);
    case ID_VIEW_THEME_LIGHT: return EN | (ui::theme_mode() == ui::ThemeMode::Light ? CK : 0);
    case ID_VIEW_THEME_DARK: return EN | (ui::theme_mode() == ui::ThemeMode::Dark ? CK : 0);
    case ID_VIEW_UNITS_MM: return EN | (s.units == 0 ? CK : 0);
    case ID_VIEW_UNITS_INCH: return EN | (s.units == 1 ? CK : 0);
    case ID_VIEW_UNITS_MIL: return EN | (s.units == 2 ? CK : 0);
    case ID_VIEW_STATUSBAR: return EN | (s.status_bar ? CK : 0);
    case ID_VIEW_BOARD_FRAME: return en(d != nullptr) | (d && d->show_frame ? CK : 0);
    case ID_VIEW_LABELS: return en(d != nullptr) | (d && d->show_labels ? CK : 0);
    case ID_VIEWER_TOP_VIS: return en(d != nullptr) | (d && d->show_top ? CK : 0);
    case ID_VIEWER_BOTTOM_VIS: return en(d != nullptr) | (d && d->show_bottom ? CK : 0);
    case ID_VIEWER_ALL_IMAGES: return en(d != nullptr) | (d && d->all_images ? CK : 0);
    case ID_VIEW_SECOND_SIDE: return en(d != nullptr) | (d && d->second_side ? CK : 0);
    case ID_VIEW_THEME_CYCLE: return EN;
    case ID_VIEW_SIDE_TOP: return en(d != nullptr) | (d && !view_bottom(*d) ? CK : 0);
    case ID_VIEW_SIDE_BOTTOM: return en(d != nullptr) | (d && view_bottom(*d) ? CK : 0);
    case ID_VIEW_SIDE_XRAY: return en(d != nullptr) | (d && side_xray(*d) ? CK : 0);
    case ID_VIEW_SIDE_NORMAL: return en(d != nullptr) | (d && d->side_display == app::Doc::SideDisplay::Normal ? CK : 0);
    case ID_VIEW_SIDE_GHOST: return en(d != nullptr) | (d && d->side_display == app::Doc::SideDisplay::Ghost ? CK : 0);
    case ID_VIEW_BACK_SELECT: return en(d != nullptr) | (d && d->back_selectable ? CK : 0);
    case ID_WIN_CLOSE_ALL: return en(!g_docs.empty());
    case ID_SEL_SET_SAVE: return en(d && d->selected_count() > 0);
    case ID_SEL_POINT_MEMBERS: return en(d && current_point(*d) >= 0);
    case ID_TOOL_SHOW_UNASSIGNED: return en(d != nullptr) | (d && d->highlight_unassigned ? CK : 0);
    case ID_TOOL_SHOW_NOREF: return en(d != nullptr) | (d && d->highlight_noref ? CK : 0);
    case ID_TOOL_VERIFY_ANGLES: return en(d != nullptr) | (d && d->verify_angles ? CK : 0);
    case ID_TOOL_MOVE: return en(d != nullptr) | (d && d->move_mode ? CK : 0);
    case ID_TOOL_ASSIGN: return en(d && d->selected_count() > 0 && current_point(*d) >= 0);
    case ID_TOOL_UNASSIGN: return en(d && d->selected_count() > 0);
    case ID_POINT_GENERATE: { int p = d ? current_point(*d) : -1; return en(p >= 0 && d->prj.model.points[p].has_machine); }
    case ID_POINT_OPEN: case ID_POINT_PROPERTIES: case ID_POINT_DELETE: case ID_POINT_UP: case ID_POINT_DOWN: return en(d && current_point(*d) >= 0);
    }
    if (id >= ID_FILE_MRU_FIRST && id <= ID_FILE_MRU_LAST) return EN;
    return en(d != nullptr);
}

bool menu_item(int id) {
    const app::CmdInfo* ci = app::cmd_info(id);
    if (!ci) return false;
    std::string sc;
    std::string t = strip_menu(ci->text, &sc);
    int st = command_state(id);
    bool r = menu_item_icon(t.c_str(), ci->icon, sc.empty() ? nullptr : sc.c_str(), (st & 2) != 0, (st & 1) != 0);
    if (ImGui::IsItemHovered() && ci->tip && *ci->tip) g_status = ui::U(ci->tip);
    if (r) post_command(id);
    return r;
}

static bool frame_cmd(int id);
static void draw_about();

static void run_command(int id) {
    if (files_active() && files_on_command(id)) { request_redraw(2); return; }   // Editors workspace (sheet / text editors)
    if (DocUI* u = active_docui()) if (View* v = u->current()) if (v->on_command(id)) { request_redraw(2); return; }
    frame_cmd(id);
    request_redraw(2);
}

void post_command(int id) { defer([id]() { run_command(id); }); }


// ===========================================================================
// R12 (REVIEW_V11 §4): autosave to rotating recovery snapshots + crash handling
// ===========================================================================
static int g_untitled_serial = 0;
static bool g_session_ok = false;
static double rec_now() { return GetTickCount64() / 1000.0; }
static int autosave_minutes() { return std::clamp(app::ui_pref("AutosaveMinutes", 2), 0, 60); }   // 0 = off
// Serialise on the calling thread (consistent snapshot), write on a worker thread.
static void rec_snapshot(DocUI& u, bool sync) {
    app::Doc& d = *u.doc;
    if (u.rec_busy->load()) return;
    if (u.rec_serial == 0) u.rec_serial = ++g_untitled_serial;
    std::string id = recovery::project_id(d.prj.path, u.rec_serial);
    if (!u.rec_id.empty() && u.rec_id != id) recovery::remove_project(u.rec_id);   // Save As moved it
    u.rec_id = id;
    std::string js = project_to_json(d.prj).dump(1);
    std::string orig = d.prj.path, name = ui::U(d.display_name());
    u.rec_gen = d.data_gen; u.rec_kick = d.significant; u.rec_t = rec_now();
    auto busy = u.rec_busy;
    busy->store(true);
    auto job = [busy, id, orig, name, js = std::move(js)]() {
        std::string err;
        if (!recovery::write_snapshot(id, orig, name, js, err)) OutputDebugStringW(ui::W("PCB Plotter autosave failed: " + err + "\n").c_str());
        busy->store(false);
    };
    if (sync) job(); else std::thread(std::move(job)).detach();
}
// automation (R13 dialog screenshots): PCBP_TEST_CMD=<menu text prefix> runs that command once, ~1 s after start
static void test_cmd_tick() {
    static const wchar_t* s_cmd = _wgetenv(L"PCBP_TEST_CMD");
    static ULONGLONG t0 = GetTickCount64();
    if (!s_cmd || !*s_cmd || GetTickCount64() - t0 < 1500) return;
    std::wstring want = s_cmd;
    s_cmd = nullptr;
    if (const wchar_t* sr = _wgetenv(L"PCBP_TEST_SELECT"); sr && adoc()) {   // R14 automation: "R1,U2" selected first
        app::Doc& d = *adoc();
        const std::string refs = "," + ui::U(sr) + ",";
        for (auto& c : d.prj.comps) c.selected = refs.find("," + c.ref + ",") != std::string::npos;
        d.changed(ui::CH_SELECTION);
    }
    for (auto& c : want) c = (wchar_t)towlower(c);
    if (want.rfind(L"view:", 0) == 0) {   // "view:export" activates that view of the active project
        for (int k = 0; k < (int)(sizeof kViewNames / sizeof kViewNames[0]); ++k) {
            std::wstring n = ui::W(kViewNames[k]);
            for (auto& c : n) c = (wchar_t)towlower(c);
            if (n == want.substr(5)) { if (app::DocPtr d = adoc()) activate_view(d, (ViewKind)k); return; }
        }
        return;
    }
    for (int id = 1; id < app::CMD_LAST; ++id) {
        const app::CmdInfo* ci = app::cmd_info(id);
        if (!ci || !ci->text) continue;
        std::wstring tx;
        for (const wchar_t* q = ci->text; *q; ++q) if (*q != L'&') tx += (wchar_t)towlower(*q);
        if (tx.rfind(want, 0) == 0) { OutputDebugStringW((L"PCBP_TEST_CMD -> " + std::to_wstring(id) + L"\n").c_str()); post_command(id); return; }
    }
}
static void autosave_tick() {
    test_cmd_tick();
    // automation (R12 BOM-import crash regression, tests/wine_bom_import.sh): run the BOM wizard once a project is open
    if (static bool s_bom = _wgetenv(L"PCBP_TEST_BOM_IMPORT") != nullptr; s_bom && !g_docs.empty()) { s_bom = false; post_command(app::ID_BOM_IMPORT); }
    if (static bool s_m1 = _wgetenv(L"PCBP_TEST_M1_TSV") != nullptr; s_m1 && !g_docs.empty()) { s_m1 = false; defer([]() { if (app::DocPtr dd = adoc()) app::dlg_import_bom_m1(g_hwnd, *dd); }); }   // R14d automation
    if (!g_session_ok) return;
    if (static int s_edits = _wgetenv(L"PCBP_TEST_EDITS") ? _wtoi(_wgetenv(L"PCBP_TEST_EDITS")) : 0; s_edits > 0 && !g_docs.empty() && !g_docs[0]->doc->prj.comps.empty()) {
        app::Doc& d = *g_docs[0]->doc;   // automation (recovery acceptance test): N unsaved placement edits, 2 mm each
        for (int i = 0; i < s_edits && i < (int)d.prj.comps.size(); ++i) {
            d.checkpoint("Move " + d.prj.comps[i].ref);
            d.prj.comps[i].pos.x += 2.0;
            d.changed(ui::CH_GEOMETRY);
        }
        ++d.significant;
        status(std::to_string(s_edits) + " test edits applied (unsaved)", ToastKind::Info);
        s_edits = 0;
    }
    const int mins = autosave_minutes();
    const double now = rec_now();
    for (auto& up : g_docs) {
        DocUI& u = *up;
        app::Doc& d = *u.doc;
        if (!d.prj.dirty || d.data_gen == u.rec_gen) continue;
        if (u.rec_t == 0) u.rec_t = now;   // first edit starts the clock
        const bool timer = mins > 0 && now - u.rec_t >= mins * 60.0;
        const bool kick = d.significant != u.rec_kick && now - u.rec_t >= 2.0;   // imports / every 25 edits
        if (timer || kick) rec_snapshot(u, false);
    }
}
static void rec_forget(const app::Doc* d) {   // saved cleanly / closed: the recovery copy is obsolete
    for (auto& up : g_docs) if (up->doc.get() == d && !up->rec_id.empty()) {
        while (up->rec_busy->load()) Sleep(5);
        recovery::close_clean(up->rec_id);   // R13: kept as a clean checkpoint (File > Recovery snapshots), pruned after 7 days
        up->rec_id.clear(); up->rec_gen = d->data_gen; up->rec_t = 0;
    }
}
// ===========================================================================
// documents (app:: API)
// ===========================================================================
} // namespace pcbp::gui

namespace pcbp::app {
using namespace pcbp::gui;
bool g_quiet_import = false;

HWND frame_hwnd() { return g_hwnd; }
HWND mdi_client() { return nullptr; }
DocView* active_view() { return nullptr; }
DocPtr active_doc() { return gui::adoc() ? gui::adoc() : (g_docs.empty() ? nullptr : g_docs.back()->doc); }
std::vector<DocPtr>& open_docs() {
    static std::vector<DocPtr> v;
    v.clear();
    for (auto& d : g_docs) v.push_back(d->doc);
    return v;
}
void set_status(const std::wstring& msg) { gui::status(ui::U(msg)); }
void refresh_ui() { request_redraw(2); }
void factory_panel_refresh() { request_redraw(2); }
int selected_point() { DocPtr d = active_doc(); return d ? current_point(*d) : -1; }
DocView* find_view(const DocPtr&, const wchar_t*) { return nullptr; }

DocPtr new_document() {
    auto d = std::make_shared<Doc>();
    d->prj.frame_color = settings().frame_color;
    d->prj.display_units = (Units)settings().units;
    d->apply_side_view();   // default: view from top, Ghost (far side transparent, locked)
    d->prj.info.originator = ui::U(settings().company);
    auto u = std::make_unique<DocUI>();
    u->doc = d;
    g_docs.push_back(std::move(u));
    g_active = (int)g_docs.size() - 1;
    g_show_landing = false;
    request_redraw(3);
    return d;
}

DocView* open_universal(const DocPtr& d) { activate_view(d, ViewKind::Plotter); return nullptr; }
DocView* open_gerber(const DocPtr& d) { activate_view(d, ViewKind::Gerber); return nullptr; }
DocView* open_output(const DocPtr& d, int point) { if (point >= 0) set_current_point(*d, point); activate_view(d, ViewKind::Machine); return nullptr; }
DocView* open_locations(const DocPtr& d, int tab) { activate_view(d, tab == 1 ? ViewKind::Bom : ViewKind::Placements); return nullptr; }
DocView* open_package_editor(const DocPtr& d, const std::string& pkg) { g_focus_package = pkg; activate_view(d, ViewKind::Packages); return nullptr; }
void show_report(HWND, const DocPtr& d, int which) { if (which == ID_MFG_CHECK) { activate_view(d, ViewKind::Validation); return; } g_report_request = which; activate_view(d, ViewKind::Reports); }

// A board frame is only ever auto-created when none exists: the implicit extent
// (components + 2 mm) is frozen into a real frame right after an import, so later
// edits (e.g. adding a component from the gerber) never move an existing frame.
// R16 (specs/OUTLINES.md): the frozen extent is tagged Inferred (dashed, "Board outline unconfirmed", blocks
// machine export). refresh = after an import: an Inferred outline follows the new placement extents; a
// Confirmed / Imported outline is never touched.
static void ensure_board_frame(Doc& d, bool refresh = false) {
    if (d.prj.comps.empty()) return;
    if (!d.prj.board_frame.empty() && !(refresh && d.prj.board_outline_src == OutlineSource::Inferred)) return;
    d.prj.infer_circuit_outline();
}

// LEAD_BRIEF §17G: keep what was imported, from where, with which settings
// "Import result: <Complete|Artwork-only|Partial> — ..." line written by the CAD importers
static std::string import_result_of(const ImportReport& rep) {
    for (auto& m : rep.messages) {
        const std::string k = "Import result: ";
        auto p = m.find(k);
        if (p == std::string::npos) continue;
        std::string r = m.substr(p + k.size());
        return r.substr(0, r.find(' '));
    }
    return {};
}
// undo label for an import: "Import ODB++ <name>"
static std::string import_undo_label(Importer* im, const std::string& fname) {
    std::string n = im->info().name;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, " job") == 0) n.resize(n.size() - 4);
    return "Import " + n + " " + fname;
}

static void record_source(Project& prj, Importer* im, const std::wstring& path, const Options& o, const ImportReport& rep) {
    Project::SourceRef sr;
    sr.path = ui::U(path);
    sr.importer = im->info().id;
    char tb[32];
    std::time_t now = std::time(nullptr);
    std::strftime(tb, sizeof tb, "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    sr.imported_at = tb;
    sr.settings = o;
    sr.summary = rep.summary();
    for (size_t i = 0; i < rep.messages.size() && i < 200; ++i) sr.warnings.push_back(rep.messages[i]);
    sr.result = import_result_of(rep);   // REVIEW_V9 §6D classification
    prj.sources.push_back(std::move(sr));
}

bool run_importer(HWND owner, const DocPtr& d, Importer* im, const std::wstring& path) {
    Options o = default_options(im->info());
    if (im->info().id == "odbpp" && !g_quiet_import) { if (!dlg_odb_import(owner, path, o)) return false; }   // step chooser + options
    else if (!g_quiet_import && !im->info().options.empty() && !dlg_plugin_options(owner, im->info(), o)) return false;
    ImportReport rep;
    std::string err;
    bool ok = false;
    const std::string title = "Importing " + im->info().name;
    const std::string fname = ui::U(path).substr(ui::U(path).find_last_of("\\/") + 1);
    if (im->info().import_mode == ImportMode::Project) {
        Project prj;
        // parse on a worker (pure data); apply on the UI thread
        bool done = run_with_progress_wait(title.c_str(), [&](Progress& pr) {
            pr.text(("Reading " + fname).c_str());
            ok = im->run(ui::U(path), o, prj, rep, err);
        }, ProgressOpts{true, false, 0.15f});
        if (!done && err.empty()) err = "interrupted";
        ok = ok && done;
        if (ok) {
            DocPtr target = d;
            if (!d->prj.comps.empty() || !d->prj.layers.empty()) target = new_document();
            else target->checkpoint_import(import_undo_label(im, fname));
            target->prj = std::move(prj);
            target->prj.path = ui::U(path);
            record_source(target->prj, im, path, o, rep);
            target->parse_all_layers();
            ensure_board_frame(*target, true);
            target->changed(ui::CH_ALL);
            target->prj.dirty = false;
            activate_view(target, target->prj.comps.empty() && !target->prj.layers.empty() ? ViewKind::Gerber : ViewKind::Plotter);
            gui::zoom_extents_doc(target);
        }
    } else {
        size_t layers_before = d->prj.layers.size();
        const bool fresh_model = d->prj.comps.empty() && d->prj.model.points.size() == FactoryModel::make_default().points.size();   // R11 P1-03
        Project work = d->prj;   // merge into a copy on the worker; swap in on the UI thread
        bool done = run_with_progress_wait(title.c_str(), [&](Progress& pr) {
            pr.text(("Reading " + fname).c_str());
            ok = im->run(ui::U(path), o, work, rep, err);
        }, ProgressOpts{true, false, 0.15f});
        ok = ok && done;
        if (ok) { d->checkpoint_import(import_undo_label(im, fname)); d->prj = std::move(work); }
        if (ok && d->prj.layers.size() != layers_before) { d->parse_all_layers(); activate_view(d, ViewKind::Gerber); }
        if (ok && d->prj.info.original_filename.empty()) d->prj.info.original_filename = ui::U(path);
        if (ok) record_source(d->prj, im, path, o, rep);
        if (ok && fresh_model && !d->prj.comps.empty()) prune_import_export_layers(d->prj);   // R11 P1-03: only Top/Bottom SMT as needed
        if (ok) { ensure_board_frame(*d, true); d->changed(ui::CH_ALL); gui::zoom_extents_doc(d); }
    }
    if (!ok) {
        if (!err.empty()) toast(im->info().name + ": import failed \xE2\x80\x94 " + err, ToastKind::Error, 7.f);
        if (!err.empty() && !g_quiet_import) ui::message(owner, ui::W(im->info().name), L"Import failed:\n" + ui::W(err));
        return false;
    }
    gui::status(im->info().name + ": " + rep.summary(), ToastKind::Success);
    {
        const std::string res = import_result_of(rep);
        toast(fname + " imported \xE2\x80\x94 " + (res.empty() ? std::string() : res + " \xC2\xB7 ") + rep.summary(),
              rep.messages.empty() && (res.empty() || res == "Complete") ? ToastKind::Success : ToastKind::Warning, 4.5f);
    }
    if (!rep.messages.empty() && !g_quiet_import) {
        std::wstring t = ui::W(rep.summary()) + L"\n\n";
        for (size_t i = 0; i < rep.messages.size() && i < 300; ++i) t += ui::W(rep.messages[i]) + L"\n";
        ui::show_text(owner, ui::W(im->info().name) + L" \x2014 Import Report", t);
    }
    return true;
}

bool run_project_exporter(HWND owner, Doc& d, Exporter* ex) {
    Options o = default_options(ex->info());
    if (!ex->info().options.empty() && !dlg_plugin_options(owner, ex->info(), o)) return false;
    std::wstring pat;
    for (auto& e : ex->info().extensions) { if (!pat.empty()) pat += L";"; pat += L"*." + ui::W(e); }
    std::wstring p = ui::W((project_program_name(d.prj).empty() ? std::string("export") : project_program_name(d.prj)) + "." + ex->info().default_ext);   // v17
    if (!ui::save_file_dialog(owner, ui::W(ex->info().name), {{ui::W(ex->info().name), pat.empty() ? L"*.*" : pat}, {L"All files", L"*.*"}}, ui::W(ex->info().default_ext), p)) return false;
    std::vector<uint8_t> out;
    std::string err;
    if (!ex->export_project(d.prj, o, out, err)) { toast(ex->info().name + " export failed", ToastKind::Error, 6.f); ui::message(owner, ui::W(ex->info().name), L"Export failed:\n" + ui::W(err)); return false; }
    if (!write_file(ui::U(p), out.data(), out.size(), err)) { toast("Could not write " + ui::U(p), ToastKind::Error, 6.f); ui::message(owner, ui::W(ex->info().name), ui::W(err)); return false; }
    gui::status("Exported " + ui::U(p), ToastKind::Success);
    { std::string dir = file_dir(ui::U(p)); toast("Exported " + ui::U(p).substr(dir.size() + (dir.empty() ? 0 : 1)), ToastKind::Success, 5.f, "Show folder",
        [p]() { std::wstring arg = L"/select,\"" + p + L"\""; ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL); }); }
    return true;
}

DocPtr open_document(const std::wstring& path) {
    if (gui::files_route_open(path)) return active_doc();   // .xlsx/.xls/.csv/.txt/.prn: editor or import?
    if (gui::pnp_route_open(path)) return active_doc();    // machine programs: Program Editor or import?
    for (auto& u : g_docs) if (!u->doc->prj.path.empty() && !lstrcmpiW(ui::W(u->doc->prj.path).c_str(), path.c_str())) {
        activate_view(u->doc, u->active);
        return u->doc;
    }
    std::string p = ui::U(path);
    std::string ext = file_ext_lower(p);
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    DocPtr d;
    if (ext == "pcbp") {
        Project prj;
        std::string err;
        bool ok = false;
        const std::string fname = p.substr(p.find_last_of("\\/") + 1);
        run_with_progress_wait("Opening project", [&](Progress& pr) { pr.text(fname.c_str()); ok = load_project(p, prj, err); }, ProgressOpts{true, false, 0.15f});
        if (!ok) { toast("Could not open " + fname, ToastKind::Error, 6.f); ui::message(g_hwnd, L"Open", L"Could not open " + path + L"\n\n" + ui::W(err)); return nullptr; }
        d = new_document();
        d->prj = std::move(prj);
        d->parse_all_layers();
        ensure_board_frame(*d);
        d->changed(ui::CH_ALL);
        d->prj.dirty = false;
        activate_view(d, ViewKind::Plotter);
        gui::zoom_extents_doc(d);
        toast("Opened " + fname + " \xE2\x80\x94 " + std::to_string(d->prj.comps.size()) + " components", ToastKind::Success, 3.f);
        if (!d->prj.migration_notes.empty()) {   // R22 (arrays.h migrate_array_outlines): generated outlines demoted on load
            std::string t = "This project was saved by an older version. Outlines that version generated are no longer trusted:\n";
            for (auto& n : d->prj.migration_notes) t += "\n\xE2\x80\xA2 " + n;
            d->prj.migration_notes.clear();
            d->prj.dirty = true;
            ui::message(g_hwnd, L"Outlines updated", ui::W(t));
        }
        if (const std::string n = tape_rot_notice(d->prj); !n.empty()) {   // R18a: one-time (persisted with the next save)
            ui::message(g_hwnd, L"Tape orientation no longer applied", ui::W(n));
            d->prj.tape_rot_notice_ack = true;
        }
    } else {
        Importer* im = PluginRegistry::get().detect(p);
        if (!im) { gui::files_open(path, 0); return active_doc(); }   // not CAD / project data: built-in text / document / binary viewer
        d = new_document();
        if (!run_importer(g_hwnd, d, im, path)) {
            close_document(d);
            return nullptr;
        }
        if (im->info().import_mode == ImportMode::Merge) { d->prj.path.clear(); d->prj.dirty = true; activate_view(d, d->prj.comps.empty() && !d->prj.layers.empty() ? ViewKind::Gerber : ViewKind::Plotter); }
        // a Project import into a used doc may have created another doc; drop the empty one
        if (d->prj.comps.empty() && d->prj.layers.empty() && d->prj.path.empty()) { close_document(d); return active_doc(); }
    }
    settings().add_mru(path);
    return d;
}

bool save_document(const DocPtr& d, bool save_as) {
    std::string path = d->prj.path;
    if (file_ext_lower(path) != ".pcbp") { save_as = true; if (!path.empty()) path = file_dir(path) + "\\" + file_stem(path) + ".pcbp"; }
    if (save_as || path.empty()) {
        std::wstring p = ui::W(path.empty() ? project_program_name(d->prj) : path);   // v17
        if (!ui::save_file_dialog(g_hwnd, L"Save Project", {{L"PCB Plotter project (*.pcbp)", L"*.pcbp"}}, L"pcbp", p)) return false;
        path = ui::U(p);
    }
    char ts[32];
    SYSTEMTIME st; GetLocalTime(&st);
    std::snprintf(ts, sizeof ts, "%04d-%02d-%02dT%02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (d->prj.info.created.empty()) d->prj.info.created = ts;
    d->prj.info.modified = ts;
    std::string err;
    bool ok = false;
    {
        auto snap = std::make_shared<Project>(d->prj);   // workers never see the live project
        const std::string fname = path.substr(path.find_last_of("\\/") + 1);
        run_with_progress_wait("Saving", [&, snap](Progress& pr) { pr.text(fname.c_str()); ok = save_project(*snap, path, err); }, ProgressOpts{true, false, 0.25f});
    }
    if (!ok) { toast("Save failed", ToastKind::Error, 6.f); ui::message(g_hwnd, L"Save", L"Could not save:\n" + ui::W(err)); return false; }
    d->prj.path = path;
    d->prj.dirty = false;
    d->title_override.clear();
    rec_forget(d.get());
    settings().add_mru(ui::W(path));
    gui::status("Saved " + path, ToastKind::Success);
    toast("Saved " + path.substr(path.find_last_of("\\/") + 1), ToastKind::Success, 2.5f);
    return true;
}

bool close_document(const DocPtr& d) {
    if (d->prj.dirty && (!d->prj.comps.empty() || !d->prj.layers.empty())) {
        ui::Form f(L"PCB Plotter", 420);
        f.icon((int)ui::Icon::Save);
        f.label(L"Save changes to " + d->display_name() + L"?");
        int choice = 0;   // 0 cancel, 1 save, 2 discard
        f.ok_text(L"Save");
        f.extra_button(L"Don't Save", [&](ui::Form&) { choice = 2; return true; });
        bool ok = f.run(g_hwnd);
        if (ok && choice == 0) choice = 1;
        if (!ok) return false;
        if (choice == 1 && !save_document(d, false)) return false;
    }
    rec_forget(d.get());
    for (size_t i = 0; i < g_docs.size(); ++i) if (g_docs[i]->doc == d) {
        g_docs.erase(g_docs.begin() + (long)i);
        if (g_active >= (int)g_docs.size()) g_active = (int)g_docs.size() - 1;
        if (g_docs.empty()) g_show_landing = true;
        break;
    }
    request_redraw(3);
    return true;
}

// R12 recovery on startup: "closed unexpectedly" notice + Restore / Review / Discard per unfinished project.
static void show_recovery_dialog() {
    std::string crash;
    if (recovery::take_crash_marker(crash)) {
        std::wstring t = L"PCB Plotter closed unexpectedly.\nA crash report was written; unsaved work was saved to the recovery folder where possible.\n\n" + ui::W(crash);
        ui::Form f(L"PCB Plotter closed unexpectedly", 640);
        f.icon((int)ui::Icon::Warning).header(L"PCB Plotter closed unexpectedly");
        f.multiline("t", L"", t, 8).readonly();
        f.no_cancel().ok_text(L"Continue");
        f.extra_button(L"Open crash folder", [](ui::Form&) { ShellExecuteW(nullptr, L"open", ui::W(recovery::crash_dir()).c_str(), nullptr, nullptr, SW_SHOWNORMAL); return false; });
        f.run(g_hwnd);
    }
    auto list = recovery::find_unfinished();
    for (auto& e : list) {
        std::vector<std::vector<std::wstring>> rows;
        for (int i = (int)e.snaps.size() - 1; i >= 0; --i)
            rows.push_back({ui::W(e.snaps[i].time), i == (int)e.snaps.size() - 1 ? L"newest" : L"older", ui::W(e.snaps[i].file)});
        const std::wstring name = ui::W(e.display_name.empty() ? std::string("Untitled") : e.display_name);
        ui::Form f(L"Recover unsaved work", 660);
        f.icon((int)ui::Icon::Undo).header(L"Unsaved work found: " + name);
        f.subtitle(e.original_path.empty() ? L"Never saved (untitled project)" : L"Original file: " + ui::W(e.original_path) + L"  (not modified)");
        f.label(L"PCB Plotter did not close normally. Pick a snapshot:\n"
                L"Restore \x2014 open it as the project (Save writes to the original file).\n"
                L"Review \x2014 open a separate copy named 'Recovered: " + name + L"'; it is never saved over the original (Save asks for a new file) and the snapshots are kept.\n"
                L"Discard \x2014 delete these recovery snapshots.");
        f.list("snap", {L"Saved at", L"", L"File"}, rows, std::min<int>(4, (int)rows.size() + 1));
        int choice = 0;   // 1 restore, 2 review, 3 discard
        f.ok_text(L"Restore");
        f.extra_button(L"Review", [&](ui::Form&) { choice = 2; return true; });
        f.extra_button(L"Discard", [&](ui::Form&) { choice = 3; return true; });
        f.cancel_text(L"Decide later");
        if (!f.run(g_hwnd)) continue;   // kept: offered again on the next launch
        if (choice == 0) choice = 1;
        if (choice == 3) {
            if (ui::message(g_hwnd, L"Recover unsaved work", L"Discard the recovered work for " + name + L"?\nThe recovery snapshots are deleted; this cannot be undone.", true))
                recovery::remove_project(e.id);
            continue;
        }
        int sel = std::max(0, f.list_selection("snap"));
        const recovery::Snapshot& sn = e.snaps[e.snaps.size() - 1 - std::min<size_t>(sel, e.snaps.size() - 1)];
        Project prj;
        std::string err;
        if (!load_project(e.dir + "\\" + sn.file, prj, err)) { ui::warning(g_hwnd, L"Recover unsaved work", L"The snapshot could not be read:\n" + ui::W(err)); continue; }
        DocPtr d = new_document();
        d->prj = std::move(prj);
        d->parse_all_layers();
        ensure_board_frame(*d);
        d->changed(ui::CH_ALL);
        if (choice == 1) {
            d->prj.path = e.original_path;
            d->prj.dirty = true;   // still unsaved: the original file is older
            std::string ae;
            recovery::adopt(e.id, ae);   // this session owns the snapshots again (rotation continues)
            if (DocUI* u = docui_of(d.get())) { u->rec_id = e.id; u->rec_gen = d->data_gen; }
            toast("Restored " + ui::U(name) + " from " + sn.time + " \xE2\x80\x94 save to keep it", ToastKind::Success, 6.f);
        } else {
            d->prj.path.clear();
            d->prj.dirty = false;
            d->title_override = L"Recovered: " + name;
            toast("Opened a review copy of " + ui::U(name) + " (" + sn.time + "); the recovery snapshots are kept", ToastKind::Info, 6.f);
        }
        activate_view(d, ViewKind::Plotter);
        gui::zoom_extents_doc(d);
    }
}

// R13 (REVIEW_V12 §7): File > Recovery snapshots... — every autosave snapshot, available during normal work.
static void open_recovery_snapshot(const recovery::Entry& e, const recovery::Snapshot& sn, bool restore) {
    const std::wstring name = ui::W(e.display_name.empty() ? std::string("Untitled") : e.display_name);
    Project prj;
    std::string err;
    if (!load_project(e.dir + "\\" + sn.file, prj, err)) { ui::warning(g_hwnd, L"Recovery snapshots", L"The snapshot could not be read:\n" + ui::W(err)); return; }
    DocPtr d = new_document();
    d->prj = std::move(prj);
    d->parse_all_layers();
    ensure_board_frame(*d);
    d->changed(ui::CH_ALL);
    if (restore) {
        d->prj.path = e.original_path;
        d->prj.dirty = true;
        std::string ae;
        if (!e.alive) { recovery::adopt(e.id, ae); if (DocUI* u = docui_of(d.get())) { u->rec_id = e.id; u->rec_gen = d->data_gen; } }
        toast("Restored " + ui::U(name) + " from " + sn.time + " \xE2\x80\x94 save to keep it", ToastKind::Success, 6.f);
    } else {
        d->prj.path.clear();
        d->prj.dirty = false;
        d->title_override = L"Recovered: " + name;
        toast("Opened a copy of " + ui::U(name) + " (" + sn.time + "); the snapshot is kept", ToastKind::Info, 6.f);
    }
    activate_view(d, ViewKind::Plotter);
    gui::zoom_extents_doc(d);
}
static void recovery_snapshots_dialog() {
    for (;;) {
        auto list = recovery::list_entries();
        struct Row { size_t e; size_t s; };
        std::vector<Row> map;
        std::vector<std::vector<std::wstring>> rows;
        std::sort(list.begin(), list.end(), [](const recovery::Entry& a, const recovery::Entry& b) {
            return (a.snaps.empty() ? std::string() : a.snaps.back().time) > (b.snaps.empty() ? std::string() : b.snaps.back().time); });
        for (size_t i = 0; i < list.size(); ++i)
            for (int k = (int)list[i].snaps.size() - 1; k >= 0; --k) {
                const auto& e = list[i]; const auto& sn = e.snaps[k];
                char kb[32]; std::snprintf(kb, sizeof kb, "%.1f KB", recovery::snapshot_bytes(e, sn) / 1024.0);
                const std::wstring st = e.alive ? (e.session == recovery::session_id() ? L"open now" : L"other window") : e.clean ? L"closed normally" : L"not closed normally";
                rows.push_back({ui::W(e.display_name.empty() ? std::string("Untitled") : e.display_name), ui::W(sn.time), ui::W(kb), st,
                                ui::W(e.original_path.empty() ? std::string("(never saved)") : e.original_path)});
                map.push_back({i, (size_t)k});
            }
        ui::Form f(L"Recovery Snapshots", 760);
        f.icon((int)ui::Icon::Undo).header(L"Recovery snapshots");
        f.subtitle(L"Autosaved copies of your projects. The original project files are never changed by autosave.");
        if (rows.empty()) f.label(L"No recovery snapshots are stored.");
        else f.list("snap", {L"Project", L"Saved at", L"Size", L"State", L"Original file"}, rows, std::min<int>(12, (int)rows.size() + 1));
        f.label(L"Kept automatically: the last " + std::to_wstring(recovery::kKeepSnapshots) + L" snapshots per project. After a normal save / close they stay "
                L"available as checkpoints and are deleted automatically after " + std::to_wstring(recovery::kCleanRetentionDays) + L" days. "
                L"Named checkpoints for the project itself: Info > Revision history (Edit > Revision Checkpoint).");
        int choice = 0;   // 1 restore, 2 copy, 3 delete, 4 delete all, 5 folder
        f.no_cancel().ok_text(L"Close");
        if (!rows.empty()) {
            f.extra_button(L"Restore", [&](ui::Form& ff) { if (ff.list_selection("snap") < 0) { (void)ff; toast("Select a snapshot first", ToastKind::Warning, 3.f); return false; } choice = 1; return true; });
            f.extra_button(L"Open as copy", [&](ui::Form& ff) { if (ff.list_selection("snap") < 0) { (void)ff; toast("Select a snapshot first", ToastKind::Warning, 3.f); return false; } choice = 2; return true; });
            f.extra_button(L"Delete", [&](ui::Form& ff) { if (ff.list_selection("snap") < 0) { (void)ff; toast("Select a snapshot first", ToastKind::Warning, 3.f); return false; } choice = 3; return true; });
            f.extra_button(L"Delete all", [&](ui::Form&) { choice = 4; return true; });
        }
        f.extra_button(L"Open folder", [&](ui::Form&) { ShellExecuteW(nullptr, L"open", ui::W(recovery::root()).c_str(), nullptr, nullptr, SW_SHOWNORMAL); return false; });
        f.run(g_hwnd);
        const int sel = rows.empty() ? -1 : f.list_selection("snap");
        if (choice == 0) return;
        if (choice == 4) {
            if (ui::message(g_hwnd, L"Recovery snapshots", L"Delete all recovery snapshots?\nSnapshots of projects open in another PCB Plotter window are kept. This cannot be undone.", true)) recovery::remove_all();
            continue;
        }
        if (sel < 0 || sel >= (int)map.size()) continue;
        const recovery::Entry& e = list[map[sel].e];
        const recovery::Snapshot sn = e.snaps[map[sel].s];
        if (choice == 3) { recovery::delete_snapshot(e.id, sn.file); continue; }
        open_recovery_snapshot(e, sn, choice == 1);
        return;
    }
}

// Emergency autosave from the crash handler (helper thread, bounded): best effort, no UI.
static void emergency_autosave() {
    for (auto& up : g_docs) {
        DocUI& u = *up;
        if (!u.doc || !u.doc->prj.dirty || u.doc->data_gen == u.rec_gen) continue;
        if (u.rec_serial == 0) u.rec_serial = 999;
        std::string id = u.rec_id.empty() ? recovery::project_id(u.doc->prj.path, u.rec_serial) : u.rec_id;
        std::string js = project_to_json(u.doc->prj).dump(1), err;
        recovery::write_snapshot(id, u.doc->prj.path, ui::U(u.doc->display_name()), js, err);
    }
}
void recovery_startup() {
    std::string err;
    g_session_ok = recovery::begin_session(err);
    recovery::cleanup_stale_sessions();
    recovery::prune_clean();   // R13: cleanly closed projects' snapshots older than 7 days
    crash::set_emergency_save(emergency_autosave);
    defer([]() { show_recovery_dialog(); });
}

} // namespace pcbp::app

namespace pcbp::gui {
using namespace pcbp::app;

// ===========================================================================
// frame commands (everything that is not view-specific)
// ===========================================================================
static std::vector<ui::FileFilter> project_filters() {
    std::vector<ui::FileFilter> f{{L"All supported", L"*.pcbp;*.cpf"}, {L"PCB Plotter projects (*.pcbp)", L"*.pcbp"}, {L"Legacy CAM projects (*.cpf)", L"*.cpf"}};
    std::wstring all;
    for (auto& im : PluginRegistry::get().importers()) {
        std::wstring pat;
        for (auto& e : im->info().extensions) { if (!pat.empty()) pat += L";"; pat += L"*." + ui::W(e); }
        if (pat.empty()) continue;
        f.push_back({ui::W(im->info().name), pat});
        all += (all.empty() ? L"" : L";") + pat;
    }
    f[0].pattern += L";" + all + L";matrix";   // extensionless ODB++ matrix file
    f.push_back({L"ODB++ job folder ('matrix' file)", L"matrix"});
    f.push_back({L"All files", L"*.*"});
    return f;
}

static std::vector<ui::FileFilter> category_filters(const char* cat) {
    std::vector<ui::FileFilter> f;
    std::wstring all;
    for (auto& im : PluginRegistry::get().importers()) {
        if (cat && im->info().category != cat) continue;
        std::wstring pat;
        for (auto& e : im->info().extensions) { if (!pat.empty()) pat += L";"; pat += L"*." + ui::W(e); }
        if (pat.empty()) continue;
        f.push_back({ui::W(im->info().name), pat});
        all += (all.empty() ? L"" : L";") + pat;
    }
    // ODB++ jobs unpacked on disk: the extensionless 'matrix' file (job/matrix/matrix) is the
    // entry point; content auto-detect recognises it, so let "All supported" show it too
    if (!cat || std::string(cat) == "CAD") {
        f.push_back({L"ODB++ job folder ('matrix' file)", L"matrix"});
        all += (all.empty() ? L"" : L";") + std::wstring(L"matrix");
    }
    f.insert(f.begin(), ui::FileFilter{L"All supported", all.empty() ? L"*.*" : all});
    f.push_back({L"All files", L"*.*"});
    return f;
}

static void apply_theme_mode(ui::ThemeMode m) {
    settings().theme = (int)m;
    ui::theme_set_mode(m);
    ui::theme_window(g_hwnd);
    ui::icon_cache_clear();
    apply_theme();
    for (auto& u : g_docs) u->doc->changed(ui::CH_SELECTION);
}

static bool frame_cmd(int id) {
    DocPtr d = adoc();
    auto& s = settings();
    auto need = [&]() { return (bool)d; };
    HWND h = g_hwnd;
    switch (id) {
    case ID_FILE_HOME: g_show_landing = true; return true;
    case ID_FILE_NEW: { DocPtr n = new_document(); activate_view(n, ViewKind::Plotter); return true; }
    case ID_FILE_OPEN: case ID_FILE_OPEN_CPF: {
        std::wstring p = s.dir_projects;
        auto f = id == ID_FILE_OPEN_CPF ? std::vector<ui::FileFilter>{{L"Legacy CAM projects (*.cpf)", L"*.cpf"}, {L"All files", L"*.*"}} : project_filters();
        if (id == ID_FILE_OPEN_CPF ? ui::open_file_dialog(h, L"Migrate Legacy Project (.cpf)", f, p)
                                   : ui::open_file_or_folder_dialog(h, L"Open", f, p, L"Select ODB++ folder\x2026", L"Select ODB++ Job Folder") != 0)   // R14
            open_document(p);
        return true;
    }
    case ID_FILE_MIGRATE_BATCH: { show_migrate_dialog(); return true; }   // gui/migrate_dialog.cpp
    case ID_IMPORT_MACHINE: {
        std::wstring p = s.dir_programs;
        if (ui::open_file_dialog(h, L"Import Machine Program", {{L"Pick-and-place programs", L"*.h8h;*.h7h;*.h5h;*.h4h;*.e4?;*.e51;*.iss"}, {L"All files", L"*.*"}}, p)) { gui::pnp_skip_route_once(); open_document(p); }
        return true;
    }
    case ID_FILE_CLOSE: if (d) close_document(d); return true;
    case ID_FILE_SAVE: case ID_EDIT_COMPACT: if (need()) save_document(d, false); return true;
    case ID_FILE_SAVEAS: if (need()) save_document(d, true); return true;
    case ID_FILE_EXIT: PostMessageW(h, WM_CLOSE, 0, 0); return true;
    case ID_FILE_STATUS: if (need()) {
        Project& p = d->prj;
        int fid = 0, dnp = 0, unas = 0, noref = 0;
        for (auto& c : p.comps) { if (c.feature == Feature::Fiducial) fid++; if (c.dnp) dnp++; if (c.feature == Feature::Component && c.assigned_point < 0) unas++; if (c.ref.empty()) noref++; }
        BBox b = p.board_bbox();
        ui::message(h, L"Project Status",
                    L"File: " + (p.path.empty() ? std::wstring(L"(not saved)") : ui::W(p.path)) +
                    L"\nComponents: " + std::to_wstring(p.comps.size()) + L"   Fiducials: " + std::to_wstring(fid) +
                    L"\nUnassigned: " + std::to_wstring(unas) + L"   No reference: " + std::to_wstring(noref) + L"   DNP: " + std::to_wstring(dnp) +
                    L"\nPackages: " + std::to_wstring(p.packages.size()) + L"   Part numbers: " + std::to_wstring(p.parts.size()) +
                    L"\nPanel images: " + std::to_wstring(p.images.size()) + L"   Gerber layers: " + std::to_wstring(p.layers.size()) +
                    L"\nBoard extent: " + fmt_len(b.w()) + L" x " + fmt_len(b.h()));
        return true;
    }
    case ID_FILE_PRINT_SETUP: {
        PRINTDLGW pd{}; pd.lStructSize = sizeof pd; pd.hwndOwner = h; pd.Flags = PD_PRINTSETUP;
        PrintDlgW(&pd);
        if (pd.hDevMode) GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
        return true;
    }
    case ID_FILE_PRINT: case ID_FILE_PRINT_1TO1: case ID_FILE_PRINT_PREVIEW: {
        DocUI* u = active_docui();
        View* v = u ? u->current() : nullptr;
        if (!v || !v->can_print()) { ui::message(h, L"Print", L"The active view cannot be printed."); return true; }
        PRINTDLGW pd{}; pd.lStructSize = sizeof pd; pd.hwndOwner = h; pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOPAGENUMS;
        if (!PrintDlgW(&pd)) return true;
        DOCINFOW di{}; di.cbSize = sizeof di;
        std::wstring name = d ? d->display_name() : L"PCB Plotter";
        di.lpszDocName = name.c_str();
        if (StartDocW(pd.hDC, &di) > 0) {
            StartPage(pd.hDC);
            RECT page{0, 0, GetDeviceCaps(pd.hDC, HORZRES), GetDeviceCaps(pd.hDC, VERTRES)};
            v->print(pd.hDC, page, id == ID_FILE_PRINT_1TO1);
            EndPage(pd.hDC);
            EndDoc(pd.hDC);
        }
        DeleteDC(pd.hDC);
        if (pd.hDevMode) GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
        return true;
    }
    case ID_EDIT_UNDO:
        // REVIEW_V9 §19: undoing an import removes everything it brought in; ask first
        if (d && d->undo_is_import() && !confirm("Undo Import", "Undo \"" + d->undo_label() + "\"?\n\nThis removes the placements, packages, layers and import record it added.", "Undo Import", true)) return true;
        if (d && d->undo()) status("Undo " + d->redo_label(), ToastKind::Info); return true;
    case ID_EDIT_REDO: if (d && d->redo()) status("Redo", ToastKind::Info); return true;
    case ID_EDIT_OPTIONS: if (dlg_options(h)) { apply_theme_mode((ui::ThemeMode)s.theme); for (auto& u : g_docs) u->doc->changed(ui::CH_ALL); } return true;
    case ID_EDIT_GRID_SPACING: if (dlg_grid_spacing(h)) for (auto& u : g_docs) u->doc->changed(ui::CH_SELECTION); return true;
    case ID_EDIT_SNAP: s.snap = !s.snap; for (auto& u : g_docs) u->doc->changed(ui::CH_SELECTION); return true;
    case ID_EDIT_GRID: s.grid_visible = !s.grid_visible; for (auto& u : g_docs) u->doc->changed(ui::CH_SELECTION); return true;
    case ID_VIEW_STATUSBAR: s.status_bar = !s.status_bar; return true;
    case ID_VIEW_THEME_SYSTEM: apply_theme_mode(ui::ThemeMode::System); return true;
    case ID_VIEW_THEME_LIGHT: apply_theme_mode(ui::ThemeMode::Light); return true;
    case ID_VIEW_THEME_DARK: apply_theme_mode(ui::ThemeMode::Dark); return true;
    case ID_VIEW_THEME_CYCLE: {
        ui::ThemeMode m = ui::theme_mode();
        ui::ThemeMode n = m == ui::ThemeMode::System ? ui::ThemeMode::Light : m == ui::ThemeMode::Light ? ui::ThemeMode::Dark : ui::ThemeMode::System;
        apply_theme_mode(n);
        status(n == ui::ThemeMode::System ? "Theme: follow Windows" : n == ui::ThemeMode::Light ? "Theme: light" : "Theme: dark");
        return true;
    }
    case ID_VIEW_SIDE_TOP: if (need()) set_view_bottom(*d, false); return true;
    case ID_VIEW_SIDE_BOTTOM: if (need()) set_view_bottom(*d, true); return true;
    case ID_VIEW_SIDE_XRAY: if (need()) set_side_display(*d, 2); return true;
    case ID_VIEW_SIDE_CYCLE: if (need()) set_view_bottom(*d, !view_bottom(*d)); return true;
    case ID_BOARD_PANEL_FRAME_DLG: if (need() && dlg_panel_frame(h, *d)) d->changed(ui::CH_GEOMETRY); return true;
    case ID_BOARD_CIRCUIT_FRAME_DLG: if (need() && dlg_circuit_frame(h, *d)) d->changed(ui::CH_GEOMETRY); return true;
    case ID_TOOL_MEASURE: case ID_VIEW_ZOOM_EXTENTS: case ID_BOARD_ALIGN_MOVE: case ID_BOARD_ALIGN_ROTATE: case ID_BOARD_ALIGN_2REF:
        if (need()) { activate_view(d, ViewKind::Plotter); post_command(id); } return true;
    case ID_SEL_SET_SAVE: if (need()) selset_save(d); return true;
    case ID_TOOL_FRAME_CIRCUIT: case ID_TOOL_FRAME_PANEL: if (need()) { activate_view(d, ViewKind::Plotter); post_command(id); } return true;
    case ID_SEL_POINT_MEMBERS: if (need() && current_point(*d) >= 0) {
        int pt = current_point(*d), n = 0;
        bool add = GetKeyState(VK_SHIFT) & 0x8000;
        if (!add) d->select_none();
        for (auto& c : d->prj.comps) if (c.assigned_point == pt) { c.selected = true; n++; }
        d->changed(ui::CH_SELECTION);
        status(std::to_string(n) + " members of " + d->prj.model.points[pt].name + " selected");
    } return true;
    case ID_SEL_UNASSIGNED: if (need()) {
        bool add = GetKeyState(VK_SHIFT) & 0x8000;
        if (!add) d->select_none();
        int n = 0;
        for (auto& c : d->prj.comps) {
            if (c.feature != Feature::Component || c.assigned_point >= 0 || c.dnp) continue;
            if ((c.side == Side::Top && !d->show_top) || (c.side == Side::Bottom && !d->show_bottom)) continue;
            c.selected = true; n++;
        }
        d->changed(ui::CH_SELECTION);
        status(std::to_string(n) + " unassigned components selected");
    } return true;
    case ID_VIEW_UNITS_MM: s.units = 0; for (auto& u : g_docs) u->doc->changed(ui::CH_SELECTION); return true;
    case ID_VIEW_UNITS_INCH: s.units = 1; for (auto& u : g_docs) u->doc->changed(ui::CH_SELECTION); return true;
    case ID_VIEW_UNITS_MIL: s.units = 2; for (auto& u : g_docs) u->doc->changed(ui::CH_SELECTION); return true;
    case ID_VIEW_BOARD_FRAME: if (need()) { d->show_frame = !d->show_frame; d->changed(ui::CH_SELECTION); } return true;
    case ID_VIEW_LABELS: if (need()) { d->show_labels = !d->show_labels; d->changed(ui::CH_SELECTION); } return true;
    case ID_VIEWER_TOP_VIS: if (need()) { d->show_top = !d->show_top; d->changed(ui::CH_SELECTION); } return true;
    case ID_VIEWER_BOTTOM_VIS: if (need()) { d->show_bottom = !d->show_bottom; d->changed(ui::CH_SELECTION); } return true;
    case ID_VIEWER_ALL_IMAGES: if (need()) { d->all_images = !d->all_images; d->changed(ui::CH_SELECTION); } return true;
    case ID_VIEW_SECOND_SIDE: if (need()) set_side_display(*d, d->far_drawn() ? 0 : 1); return true;
    case ID_VIEW_SIDE_NORMAL: if (need()) set_side_display(*d, 0); return true;
    case ID_VIEW_SIDE_GHOST: if (need()) set_side_display(*d, 2); return true;   // R17c: Ghost == X-ray, locked (specs/VIEW_MODES.md)
    case ID_VIEW_BACK_SELECT: if (need()) set_back_selectable(*d, !d->back_selectable); return true;
    case ID_TOOL_SHOW_UNASSIGNED: if (need()) { d->highlight_unassigned = !d->highlight_unassigned; d->changed(ui::CH_SELECTION); } return true;
    case ID_TOOL_SHOW_NOREF: if (need()) { d->highlight_noref = !d->highlight_noref; d->changed(ui::CH_SELECTION); } return true;
    case ID_TOOL_VERIFY_ANGLES: if (need()) { d->verify_angles = !d->verify_angles; d->changed(ui::CH_SELECTION); } return true;
    case ID_TOOL_MOVE: if (need()) { d->move_mode = !d->move_mode; status(d->move_mode ? "Move tool: drag selected components" : "Placements locked"); } return true;
    case ID_VIEW_NEXT_TAB: case ID_VIEW_PREV_TAB: if (DocUI* u = active_docui()) {
        int cur = 0;
        for (int i = 0; i < kTabCount; ++i) if (kTabOrder[i] == u->active) cur = i;
        int k = (cur + (id == ID_VIEW_NEXT_TAB ? 1 : kTabCount - 1)) % kTabCount;
        activate_view(u->doc, kTabOrder[k]);
    } return true;

    case ID_IMPORT_GERBER: if (need()) { if (dlg_import_gerber(h, *d)) { d->changed(ui::CH_ALL); activate_view(d, ViewKind::Gerber); } } return true;
    case ID_IMPORT_CENTROID: case ID_BOARD_FUSION: if (need()) {
        std::wstring p = s.dir_centroid;
        if (const wchar_t* tf = _wgetenv(L"PCBP_TEST_IMPORT")) { if (dlg_import_centroid(h, *d, tf, id == ID_BOARD_FUSION)) { ensure_board_frame(*d, true); d->changed(ui::CH_ALL); } return true; }   // automation
        if (ui::open_file_dialog(h, id == ID_BOARD_FUSION ? L"Reference Fusion (centroid file)" : L"Import Pick && Place / Centroid File",
                                 {{L"Text / CSV", L"*.txt;*.csv;*.pos;*.xy;*.mnt;*.prn;*.rep;*.tsv"}, {L"All files", L"*.*"}}, p))
            if (dlg_import_centroid(h, *d, p, id == ID_BOARD_FUSION)) { ensure_board_frame(*d, true); d->changed(ui::CH_ALL); }
    } return true;
    case ID_IMPORT_BOARD_IMAGE: if (need()) cmd_import_board_image(h, *d); return true;
    case ID_BOARD_IMAGES: if (need()) dlg_board_images(h, *d); return true;
    case ID_BOM_IMPORT: if (need()) {
        std::wstring p = s.dir_bom;
        const int kind = g_bom_import_kind; g_bom_import_kind = 0;
        std::vector<ui::FileFilter> flt = kind == 1 ? std::vector<ui::FileFilter>{{L"Text / CSV BOM", L"*.csv;*.txt;*.prn;*.bom;*.tsv"}, {L"All files", L"*.*"}}
                                        : kind == 2 ? std::vector<ui::FileFilter>{{L"Excel / spreadsheet BOM", L"*.xlsx;*.xls;*.ods"}, {L"All files", L"*.*"}}
                                        : std::vector<ui::FileFilter>{{L"BOM files (spreadsheet or text)", L"*.xlsx;*.xls;*.ods;*.csv;*.txt;*.prn;*.bom;*.tsv"}, {L"All files", L"*.*"}};
        if (const wchar_t* tf = _wgetenv(L"PCBP_TEST_IMPORT")) { if (dlg_import_bom(h, *d, tf)) d->changed(ui::CH_ALL); return true; }   // automation
        if (ui::open_file_dialog(h, kind == 1 ? L"Import BOM (Text / CSV)" : kind == 2 ? L"Import BOM (Excel Spreadsheet)" : L"Import Bill of Materials", flt, p))
            if (dlg_import_bom(h, *d, p)) d->changed(ui::CH_ALL);
    } return true;
    case ID_IMPORT_CAD: case ID_IMPORT_CAD_AUTO: case ID_IMPORT_MERGE_CAD: if (need()) {
        std::wstring p = s.dir_cad;
        if (const wchar_t* tf = _wgetenv(L"PCBP_TEST_CAD")) p = tf;   // automation (R13 dialog screenshots)
        else if (!ui::open_file_or_folder_dialog(h, id == ID_IMPORT_MERGE_CAD ? L"Merge CAD File (ECN)" : L"Import CAD File", category_filters("CAD"), p,
                                                 L"Select ODB++ folder\x2026", L"Select ODB++ Job Folder")) return true;   // R14: folder = ODB++ job
        Importer* im = PluginRegistry::get().detect(ui::U(p));
        // one dialog: auto-detected format with an override combo
        {
            std::vector<Importer*> cands;
            for (auto& x : PluginRegistry::get().importers()) if (x->info().category == "CAD") cands.push_back(x.get());
            if (im && std::find(cands.begin(), cands.end(), im) == cands.end()) cands.insert(cands.begin(), im);
            std::vector<std::wstring> names;
            names.push_back(im ? L"Auto-detect: " + ui::W(im->info().name) : L"Auto-detect (not recognised \x2014 pick a format)");
            for (auto* x : cands) names.push_back(ui::W(x->info().name));
            ui::Form f(id == ID_IMPORT_MERGE_CAD ? L"Merge CAD File" : L"Import CAD File", 700);   // R13 P1-07: wider (file + format details)
            f.icon((int)ui::Icon::ImportCad);
            f.subtitle(p.substr(p.find_last_of(L"\\/") == std::wstring::npos ? 0 : p.find_last_of(L"\\/") + 1));
            f.combo("fmt", L"Format", names, names[0]).wide();
            f.label(im ? L"The format was detected from the file contents. Override it only if the detection is wrong."
                       : L"No importer recognised this file automatically. Choose the format to try.");
            f.validate([&](ui::Form& ff, std::wstring& err) {
                if (ff.index("fmt") <= 0 && !im) { err = L"Choose a format."; return false; }
                return true;
            });
            f.ok_text(L"Import");
            if (!f.run(h)) return true;
            int k = f.index("fmt");
            if (k > 0 && k - 1 < (int)cands.size()) im = cands[k - 1];
        }
        if (!im) return true;
        if (id == ID_IMPORT_MERGE_CAD) {
            Project tmp; ImportReport rep; std::string err;
            if (!im->run(ui::U(p), default_options(im->info()), tmp, rep, err)) { ui::message(h, L"Merge CAD", ui::W(err)); return true; }
            std::wstring ecn = L"ECN REPORT  " + ui::W(file_stem(ui::U(p))) + L"\n\n";
            int add = 0, rem = 0, chg = 0;
            for (auto& c : tmp.comps) {
                const Component* o2 = d->prj.find_ref(c.ref);
                if (!o2) { ecn += L"ADDED     " + ui::W(c.ref) + L"  " + ui::W(c.number) + L"\n"; add++; continue; }
                std::wstring what;
                if (o2->number != c.number && !c.number.empty()) what += L" P/N " + ui::W(o2->number) + L" -> " + ui::W(c.number);
                double dr = norm_deg(o2->rot_imported - c.rot);
                if (dr > 0.01 && dr < 359.99) what += L" ROT " + ui::fmt_num(o2->rot_imported, 1) + L" -> " + ui::fmt_num(c.rot, 1);
                if (dist(o2->cad_pos, c.pos) > 0.01) what += L" MOVED " + fmt_len(dist(o2->cad_pos, c.pos));
                if (!what.empty()) { ecn += L"CHANGED   " + ui::W(c.ref) + what + L"\n"; chg++; }
            }
            for (auto& c : d->prj.comps) if (c.feature == Feature::Component && !tmp.find_ref(c.ref)) { ecn += L"REMOVED   " + ui::W(c.ref) + L"  " + ui::W(c.number) + L"\n"; rem++; }
            ecn += L"\n" + std::to_wstring(add) + L" added, " + std::to_wstring(rem) + L" removed, " + std::to_wstring(chg) + L" changed.";
            if (!ui::message(h, L"Merge CAD File", L"Apply this CAD revision?\n\n" + std::to_wstring(add) + L" added, " + std::to_wstring(rem) + L" removed, " + std::to_wstring(chg) + L" changed.", true)) return true;
            d->checkpoint("Merge CAD");
            std::vector<Component> keep;
            for (auto& c : tmp.comps) {
                Component n = c;
                if (const Component* o2 = d->prj.find_ref(c.ref)) { n.assigned_point = o2->assigned_point; n.label_off = o2->label_off; n.label_moved = o2->label_moved; n.dnp = o2->dnp; if (n.number.empty()) n.number = o2->number; }
                n.id = 0;
                keep.push_back(n);
            }
            for (auto& c : d->prj.comps) if (c.feature != Feature::Component) keep.push_back(c);
            d->prj.comps.clear();
            for (auto& c : keep) d->prj.add_component(c);
            for (auto& kv : tmp.packages) {
                // R15: a revision never downgrades real data: keep a real body outline when the new CAD only has a
                // derived one, and keep the user's polarity marker
                Package np = kv.second;
                auto it = d->prj.packages.find(kv.first);
                if (it != d->prj.packages.end()) {
                    const Package& op = it->second;
                    if (!pkg_body_replaceable(op) && pkg_body_replaceable(np)) { np.body = op.body; np.body_source = op.body_source; }
                    if (op.polarity_set) {
                        np.marker_kind = op.marker_kind; np.marker_color = op.marker_color; np.marker_user_pos = op.marker_user_pos; np.marker_pos = op.marker_pos;
                        np.marker_size = op.marker_size; np.marker_rot = op.marker_rot; np.marker_art = op.marker_art; np.polarity_set = true;
                    }
                }
                d->prj.packages[kv.first] = np;
            }
            for (auto& kv : tmp.parts) if (!d->prj.parts.count(kv.first)) d->prj.parts[kv.first] = kv.second;
            if (!tmp.board_frame.empty()) { d->prj.board_frame = tmp.board_frame; d->prj.board_outline_src = tmp.board_outline_src; }
            d->prj.revision_notes.push_back(ui::U(ecn));
            d->changed(ui::CH_ALL);
            ui::show_text(h, L"ECN Report", ecn);
            return true;
        }
        run_importer(h, d, im, p);
    } return true;
    case ID_EXPORT_GENCAD: if (need()) { if (Exporter* e = PluginRegistry::get().exporter("gencad")) run_project_exporter(h, *d, e); } return true;

    case ID_BOARD_ASSEMBLY_INFO: if (need()) activate_view(d, ViewKind::Notes); return true;   // R13 P1-06: Project Info is the one place for assembly metadata
    case ID_BOARD_EDIT_PACKAGES: case ID_LIB_PACKAGE_LIBRARY: if (need()) { std::string pk; for (auto* c : d->selection()) { pk = c->package; break; } open_package_editor(d, pk); } return true;
    case ID_BOARD_EDIT_LOCATIONS: if (need()) activate_view(d, ViewKind::Placements); return true;
    case ID_BOM_EDIT: case ID_LIB_NUMBER_LIBRARY: if (need()) activate_view(d, ViewKind::Bom); return true;
    case ID_BOARD_INSERT_FREE: if (need()) add_placement_dialog(d); return true;
    case ID_EDIT_DUPLICATE: if (need()) duplicate_placements(d); return true;
    case ID_VIEW_APPEARANCE: appearance_dialog(); return true;
    case ID_FILE_RECOVERY_SNAPSHOTS: recovery_snapshots_dialog(); return true;
    case ID_BOARD_ALIGN_CAD_GERBER: if (need()) gui::align_cad_gerber_open(d); return true;
    case ID_VIEW_PERF_OVERLAY: perf_overlay_toggle(); return true;
    case ID_PRJ_CHECKPOINT: if (need()) {
        std::wstring c;
        if (ui::prompt_text(h, L"Revision Checkpoint", L"Optional comment for the revision history:", c)) {
            const Project& p = d->prj;
            app::add_history(*d, "checkpoint", std::to_string(p.comps.size()) + " placements, " + std::to_string(p.parts.size()) + " part numbers, " +
                             std::to_string(p.packages.size()) + " packages", ui::U(c), "", "");
            toast("Checkpoint recorded in Info > Revision history", ToastKind::Success);
        }
    } return true;
    case ID_BOARD_AUTOPLACE_LABELS: if (need()) {
        d->checkpoint("Auto-place labels");
        for (auto& c : d->prj.comps) { c.label_moved = false; c.label_off = {}; c.label_rot = 0; }
        d->changed(ui::CH_GEOMETRY);
    } return true;
    // R22 (REVIEW_V21 §4): Clear All References retired with the References submenu (no other path used it).
    case ID_BOARD_REBASE_ORIGIN: if (need()) {
        Vec2 o = d->prj.virtual_origin;
        if (o.x == 0 && o.y == 0) { ui::message(h, L"Rebase Coordinates", L"The virtual origin is already at 0,0.\nSet it first with the Assign Origin tool."); return true; }
        if (!ui::message(h, L"Rebase Coordinates", L"Move every location, frame and panel image by " + fmt_len(-o.x) + L", " + fmt_len(-o.y) +
                         L"\nso the virtual origin becomes 0,0? All locations move with it.", true)) return true;
        d->checkpoint("Rebase coordinates");
        for (auto& c : d->prj.comps) { c.pos -= o; c.cad_pos -= o; }
        for (auto& pl : d->prj.board_frame) for (auto& p : pl) p -= o;
        for (auto& pl : d->prj.panel_frame) for (auto& p : pl) p -= o;
        for (auto& l : d->prj.layers) l.offset -= o;
        for (auto& m : d->prj.model.points) if (m.machine.has_origin) m.machine.origin -= o;
        d->prj.virtual_origin = {};
        d->parse_all_layers();
        d->changed(ui::CH_ALL);
    } return true;
    case ID_BOARD_ADD_FRAME: if (need() && dlg_add_board_frame(h, *d)) d->changed(ui::CH_GEOMETRY); return true;
    // R16d: the outline submenu is Board / Circuit Outline... + Confirm + Clear. Copy Outline is retired (outlines are
    // deliberate hardware data, defined per outline); the id stays for old shortcuts / scripts and opens the dialog.
    case ID_BOARD_COPY_FRAME: if (need()) post_command(ID_BOARD_ADD_FRAME); return true;
    case ID_BOARD_CONFIRM_OUTLINE: if (need() && dlg_confirm_board_outline(h, *d)) d->changed(ui::CH_GEOMETRY); return true;
    case ID_BOARD_CLEAR_OUTLINE: return true;   // R22: Clear Outline and its selection dialog retired (Delete Board / Circuit Outline)
    // R22 (REVIEW_V21 §3): explicit deletes, no selection dialog (arrays.h delete_outline; Ctrl+Z undoes)
    case ID_BOARD_CLEAR_PANEL_FRAME: if (need() && outline_exists(d->prj, OutlineKind::Board)) {
        d->checkpoint("Delete board outline"); delete_outline(d->prj, OutlineKind::Board); d->changed(ui::CH_GEOMETRY);
        toast("Board outline deleted", ToastKind::Info, 4.f, "Undo", [] { post_command(ID_EDIT_UNDO); });
    } return true;
    case ID_BOARD_CLEAR_IMAGE_FRAME: if (need() && outline_exists(d->prj, OutlineKind::Circuit)) {
        d->checkpoint("Delete circuit outline"); delete_outline(d->prj, OutlineKind::Circuit); d->changed(ui::CH_GEOMETRY);
        toast("Circuit outline deleted", ToastKind::Info, 4.f, "Undo", [] { post_command(ID_EDIT_UNDO); });
    } return true;
    case ID_BOARD_FRAME_COLOR: if (need()) {
        ui::Form f(L"Outline Colour", 340);
        f.color("c", L"Outline colour", d->prj.frame_color);
        if (f.run(h)) { d->prj.frame_color = f.color_value("c"); d->changed(ui::CH_GEOMETRY); }
    } return true;
    case ID_BOARD_OFFSET_ROTATE: if (need()) dlg_transform(h, *d); return true;   // R16c: unified Transform (dlg_offset_rotate retired from the menu)
    case ID_BOARD_APPLY_OFFSET_FILE: if (need() && dlg_apply_offset_file(h, *d)) d->changed(ui::CH_GEOMETRY); return true;
    case ID_BOARD_PANELIZE: if (need() && dlg_panelize(h, *d)) d->changed(ui::CH_GEOMETRY); return true;
    case ID_BOARD_DETECT_REPEATED: if (need()) dlg_detect_repeated(h, *d); return true;   // v17 (R22: Gerber viewer only, no Plot menu entry)
    case ID_BOARD_ARRAY_LINEAR: if (need() && dlg_linear_array(h, *d)) d->changed(ui::CH_GEOMETRY); return true;   // R22
    case ID_BOARD_ARRAY_DELETE: if (need() && d->prj.is_panel()) {   // R22: instances go, the reference circuit stays
        const int n = (int)d->prj.images.size();
        d->checkpoint("Delete array"); delete_array(d->prj); d->changed(ui::CH_GEOMETRY);
        toast("Array deleted: " + std::to_string(n - 1) + " instance(s) removed, reference circuit kept", ToastKind::Info, 5.f, "Undo", [] { post_command(ID_EDIT_UNDO); });
    } return true;
    case ID_BOARD_CROP_PINS: ui::message(h, L"Crop Extended Pins", L"Extended pins are cropped automatically while digitizing."); return true;

    case ID_MFG_PREP_REPORT: case ID_MFG_OPPORTUNITY: case ID_MFG_ROUTING_REPORT: case ID_MFG_PLACEMENT_REPORT: case ID_MFG_CHECK:
        if (need()) show_report(h, d, id); return true;
    case ID_MFG_AUTO_ASSIGN: if (need()) {
        d->checkpoint("Auto-assign");
        int n = d->prj.auto_assign();
        d->changed(ui::CH_ASSIGN);
        status(std::to_string(n) + " components assigned by model point filters", n ? ToastKind::Success : ToastKind::Info);
        toast(std::to_string(n) + " components auto-assigned", n ? ToastKind::Success : ToastKind::Info);
    } return true;
    case ID_MFG_CLEAR_ASSIGN: if (need() && confirm("Clear Assignments", "Unassign every component from every model point?\nThis can be undone with Ctrl+Z.", "Clear Assignments", true)) {
        d->checkpoint("Clear assignments");
        for (auto& c : d->prj.comps) c.assigned_point = -1;
        d->changed(ui::CH_ASSIGN);
    } return true;
    case ID_MFG_CLEAR_UNUSED_POINTS: if (need()) {
        std::vector<int> used(d->prj.model.points.size(), 0);
        for (auto& c : d->prj.comps) if (c.assigned_point >= 0 && c.assigned_point < (int)used.size()) used[c.assigned_point]++;
        std::vector<int> remap(used.size(), -1);
        std::vector<ModelPoint> np;
        for (size_t i = 0; i < used.size(); ++i) if (used[i] || d->prj.model.points[i].kind == PointKind::Process) { remap[i] = (int)np.size(); np.push_back(d->prj.model.points[i]); }
        int removed = (int)(used.size() - np.size());
        if (!removed) { ui::message(h, L"Clear Unused Model Points", L"Every placement point has assignments."); return true; }
        if (!confirm("Clear Unused Model Points", "Delete " + std::to_string(removed) + " unused model point(s)?", "Delete", true)) return true;
        d->checkpoint("Clear unused points");
        d->prj.model.points = np;
        for (auto& c : d->prj.comps) if (c.assigned_point >= 0) c.assigned_point = remap[c.assigned_point];
        d->active_point = 0;
        d->changed(ui::CH_MODEL);
    } return true;
    case ID_LIB_FACTORY_MODEL: if (need()) { if (dlg_factory_model(h, *d)) d->changed(ui::CH_MODEL); } else ui::message(h, L"Factory Model", L"Open or create a project first; each project carries its own factory model."); return true;
    case ID_LIB_IMPORT_CCLIB: if (need()) {
        std::wstring p = s.library_path;
        if (!ui::open_file_dialog(h, L"Import Legacy Package Library", {{L"Legacy library / project", L"*.mdb;*.cpf"}, {L"All files", L"*.*"}}, p)) return true;
        Importer* im = PluginRegistry::get().importer("circuitcam.cpf");
        if (!im) return true;
        Project tmp; ImportReport rep; std::string err;
        Options o = default_options(im->info());
        o["library_only"] = "1";
        if (!im->run(ui::U(p), o, tmp, rep, err)) { ui::message(h, L"Import Library", ui::W(err)); return true; }
        d->checkpoint("Import library");
        int np = 0, nn = 0;
        for (auto& kv : tmp.packages) if (!d->prj.packages.count(kv.first)) { d->prj.packages[kv.first] = kv.second; np++; }
        for (auto& kv : tmp.parts) if (!d->prj.parts.count(kv.first)) { d->prj.parts[kv.first] = kv.second; nn++; }
        d->prj.link_bom();
        d->changed(ui::CH_GEOMETRY);
        s.library_path = p;
        ui::message(h, L"Import Library", std::to_wstring(np) + L" packages and " + std::to_wstring(nn) + L" part numbers added.");
    } return true;
    case ID_LIB_UPDATE_RENDERING: if (need()) {
        d->checkpoint("Update rendering");
        int n = 0;
        for (auto& c : d->prj.comps) if (auto* pn = d->prj.part_of(c)) if (!pn->package.empty() && pn->package != c.package) { c.package = pn->package; n++; }
        d->changed(ui::CH_GEOMETRY);
        status(std::to_string(n) + " component packages updated from part numbers");
    } return true;
    case ID_LIB_PLUGINS: dlg_plugins(h); return true;
    case ID_WIN_UNIVERSAL: if (need()) activate_view(d, ViewKind::Plotter); return true;
    case ID_WIN_GERBER: if (need()) activate_view(d, ViewKind::Gerber); return true;
    case ID_WIN_CLOSE_ALL: {
        std::vector<DocPtr> v; for (auto& u : g_docs) v.push_back(u->doc);
        int n = 0; for (auto& x : v) { if (!close_document(x)) break; ++n; }
        if (n) toast("Closed " + std::to_string(n) + (n == 1 ? " project" : " projects"), ToastKind::Info, 2.5f);
    } return true;
    case ID_HELP_ABOUT: show_about(); return true;
    case ID_HELP_CONTENTS: dlg_help(h, 0); return true;
    case ID_HELP_SHORTCUTS: dlg_help(h, 1); return true;
    case ID_HELP_VERIFY: dlg_help(h, 2); return true;

    case ID_TOOL_ASSIGN: case ID_TOOL_UNASSIGN: if (need()) {
        int pt = current_point(*d);
        auto sel = d->selection();
        if (sel.empty()) { status("Select components first"); return true; }
        if (id == ID_TOOL_ASSIGN && pt < 0) { status("Select a model point first"); return true; }
        d->checkpoint(id == ID_TOOL_ASSIGN ? "Assign" : "Unassign");
        int n = 0;
        int hidden = 0;
        for (auto* c : sel) {
            if (c->feature != Feature::Component) continue;
            // selected components on a hidden side are left alone (Top only / Bottom only workflow)
            if ((c->side == Side::Top && !d->show_top) || (c->side == Side::Bottom && !d->show_bottom)) { hidden++; continue; }
            c->assigned_point = id == ID_TOOL_ASSIGN ? pt : -1; n++;
        }
        d->changed(ui::CH_ASSIGN);
        {
            std::string m = std::to_string(n) + (id == ID_TOOL_ASSIGN ? " components assigned to " + d->prj.model.points[pt].name : std::string(" components unassigned")) +
                            (hidden ? "  (" + std::to_string(hidden) + " on the hidden side skipped)" : std::string());
            if (n) toast(m, ToastKind::Success, 4.5f, "Undo", [] { post_command(ID_EDIT_UNDO); });
            else toast(m, ToastKind::Warning);
        }
    } return true;
    case ID_POINT_OPEN: if (need() && current_point(*d) >= 0) activate_view(d, ViewKind::Machine); return true;
    case ID_POINT_PROPERTIES: if (need() && current_point(*d) >= 0 && dlg_model_point(h, *d, current_point(*d))) d->changed(ui::CH_MODEL); return true;
    case ID_POINT_ADD: if (need()) {
        d->checkpoint("Add model point");
        ModelPoint mp; mp.name = "New Point " + std::to_string(d->prj.model.points.size() + 1);
        d->prj.model.points.push_back(mp);
        int idx = (int)d->prj.model.points.size() - 1;
        if (dlg_model_point(h, *d, idx)) { d->active_point = idx; d->changed(ui::CH_MODEL); }
        else d->prj.model.points.pop_back();
    } return true;
    case ID_POINT_DELETE: if (need() && current_point(*d) >= 0) delete_layers(d, layer_selection(*d)); return true;
    case ID_POINT_UP: case ID_POINT_DOWN: if (need() && current_point(*d) >= 0) {
        int a = current_point(*d), b = a + (id == ID_POINT_UP ? -1 : 1);
        if (b < 0 || b >= (int)d->prj.model.points.size()) return true;
        d->checkpoint("Reorder model points");
        std::swap(d->prj.model.points[a], d->prj.model.points[b]);
        for (auto& c : d->prj.comps) { if (c.assigned_point == a) c.assigned_point = b; else if (c.assigned_point == b) c.assigned_point = a; }
        d->active_point = b;
        d->changed(ui::CH_MODEL);
    } return true;
    case ID_POINT_GENERATE: if (need()) {
        int pt = current_point(*d);
        if (pt < 0) return true;
        std::wstring out; std::string err;
        if (!generate_program(h, *d, pt, &out, err)) { if (err.find("cancel") == std::string::npos) ui::message(h, L"Generate Program", ui::W(err)); }
        else {
            status("Program written: " + ui::U(out), ToastKind::Success);
            std::string o8 = ui::U(out);
            toast("Program written: " + o8.substr(o8.find_last_of("\\/") + 1), ToastKind::Success, 5.f, "Show folder",
                  [out]() { std::wstring arg = L"/select,\"" + out + L"\""; ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL); });
            const std::string ge = file_ext_lower(o8);
            if (ge.size() == 4 && isdigit((unsigned char)ge[2]) && ((ge[1] == 'h' && ge[3] == 'h') || (ge[1] == 'e' && isdigit((unsigned char)ge[3]))))   // HLC program: offer the Program Editor
                toast("Review or edit the program in the Program Editor", ToastKind::Info, 6.f, "Open in editor", [out]() { pnp_open(out); });
        }
    } return true;
    }
    if (id >= ID_FILE_MRU_FIRST && id <= ID_FILE_MRU_LAST) {
        size_t i = (size_t)(id - ID_FILE_MRU_FIRST);
        if (i < s.mru.size()) open_document(s.mru[i]);
        return true;
    }
    return false;
}

// ===========================================================================
// drawing
// ===========================================================================
// R10 §4: File holds only New / Open / Save / Save As / Recent / Import > / Export > / Close.
// Specialised tools live in Tools, legacy conversion in Migration.
static void draw_import_items() {
        for (int id : {ID_IMPORT_CAD, ID_IMPORT_MERGE_CAD}) menu_item(id);
        ImGui::Separator();
        for (int id : {ID_IMPORT_GERBER, ID_IMPORT_CENTROID, ID_BOM_IMPORT, ID_IMPORT_MACHINE}) menu_item(id);
        ImGui::Separator();
        for (int id : {ID_IMPORT_BOARD_IMAGE, ID_BOARD_IMAGES}) menu_item(id);
        ImGui::Separator();
        if (begin_menu_icon("All Formats", ui::Icon::Plugin)) {
            for (auto& im : PluginRegistry::get().importers()) {
                std::string t = im->info().name + (im->info().builtin ? "" : "  [plugin]");
                if (menu_item_icon(t.c_str(), im->info().builtin ? ui::Icon::Open : ui::Icon::Plugin)) {
                    Importer* imp = im.get();
                    defer([imp]() {
                        DocPtr d = adoc();
                        if (!d) return;
                        std::vector<ui::FileFilter> f;
                        std::wstring pat;
                        for (auto& e : imp->info().extensions) { if (!pat.empty()) pat += L";"; pat += L"*." + ui::W(e); }
                        f.push_back({ui::W(imp->info().name), pat.empty() ? L"*.*" : pat});
                        f.push_back({L"All files", L"*.*"});
                        std::wstring p;
                        if (ui::open_file_dialog(g_hwnd, ui::W(imp->info().name), f, p)) run_importer(g_hwnd, d, imp, p);
                    });
                }
            }
            ImGui::EndMenu();
        }
    ImGui::Separator();
    if (menu_item_icon("Archive (Auto-detect)\xE2\x80\xA6", ui::Icon::Wand)) defer([]() { std::wstring p; if (ui::open_file_dialog(g_hwnd, L"Import Archive", {{L"Archives", L"*.zip;*.tgz;*.tar.gz;*.tar;*.gz"}, {L"All files", L"*.*"}}, p)) import_auto(p); });
    if (menu_item_icon("Folder (Auto-detect)\xE2\x80\xA6", ui::Icon::Folder)) defer([]() { std::wstring p; if (ui::pick_folder_dialog(g_hwnd, L"Import Folder", p)) import_auto(p); });
}
static void draw_export_items() {
        menu_item(ID_EXPORT_GENCAD);
        for (auto& ex : PluginRegistry::get().exporters()) {
            if (ex->info().export_input != ExportInput::Project || ex->info().id == "gencad") continue;
            std::string t = ex->info().name + "..." + (ex->info().builtin ? "" : "  [plugin]");
            if (menu_item_icon(t.c_str(), ex->info().builtin ? ui::Icon::Export : ui::Icon::Plugin)) { Exporter* e = ex.get(); defer([e]() { if (DocPtr d = adoc()) run_project_exporter(g_hwnd, *d, e); }); }
        }
        ImGui::Separator();
        if (begin_menu_icon("Pick-and-place program formats", ui::Icon::Machine)) {
            for (auto& ex : PluginRegistry::get().exporters()) {
                if (ex->info().export_input == ExportInput::Project) continue;
                std::string t = ex->info().name + (ex->info().builtin ? "" : "  [plugin]");
                if (menu_item_icon(t.c_str(), ex->info().builtin ? ui::Icon::Machine : ui::Icon::Plugin)) {
                    std::string id = ex->info().id;
                    defer([id]() {   // use it for the current process point, then open the Machine view
                        DocPtr d = adoc();
                        if (!d) return;
                        int pi = current_point(*d);
                        if (pi < 0) { toast("Add a process point to the factory model first", ToastKind::Warning); return; }
                        ModelPoint& mp = d->prj.model.points[pi];
                        if (!mp.has_machine || mp.machine.exporter_id != id) {
                            d->checkpoint("Machine format");
                            mp.has_machine = true; mp.machine.exporter_id = id;
                            d->changed(ui::CH_INFO);
                        }
                        activate_view(d, ViewKind::Machine);
                    });
                }
            }
            ImGui::EndMenu();
        }
        menu_item(ID_POINT_GENERATE);
    ImGui::Separator();
    for (int id : {ID_FILE_PRINT, ID_FILE_PRINT_1TO1, ID_FILE_PRINT_SETUP}) menu_item(id);
}
// ---------------------------------------------------------------------------
// R10 §14: Add Placement (manual reference / X / Y / angle / side / package, X/Y paste) + Duplicate
// ---------------------------------------------------------------------------
static std::string next_free_ref(const Project& p, const std::string& ref) {
    size_t k = ref.size();
    while (k > 0 && std::isdigit((unsigned char)ref[k - 1])) --k;
    std::string base = ref.substr(0, k);
    long n = k < ref.size() ? std::atol(ref.c_str() + k) : 0;
    for (int guard = 0; guard < 100000; ++guard) {
        std::string c = base + std::to_string(++n);
        if (!p.find_ref(c)) return c;
    }
    return ref + "_copy";
}
// "12.3, 45.6" / "12.3\t45.6" / "X12.3 Y45.6" -> two numbers
static bool parse_xy_text(const std::wstring& t, double& x, double& y) {
    std::vector<double> v;
    const wchar_t* q = t.c_str();
    while (*q && v.size() < 2) {
        if (std::iswdigit(*q) || ((*q == L'-' || *q == L'+' || *q == L'.') && (std::iswdigit(q[1]) || q[1] == L'.'))) {
            wchar_t* e = nullptr; double d = std::wcstod(q, &e);
            if (e == q) { ++q; continue; }
            v.push_back(d); q = e;
        } else ++q;
    }
    if (v.size() < 2) return false;
    x = v[0]; y = v[1];
    return true;
}
static std::wstring clipboard_text() {
    std::wstring r;
    if (!OpenClipboard(g_hwnd)) return r;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) { r = p; GlobalUnlock(h); }
    CloseClipboard();
    return r;
}
static void add_placement_dialog(const DocPtr& d) {
    Project& p = d->prj;
    std::vector<std::wstring> pk;
    for (auto& kv : p.packages) pk.push_back(ui::W(kv.first));
    std::vector<std::wstring> pn{L""};
    for (auto& kv : p.parts) pn.push_back(ui::W(kv.first));
    std::string ref0 = "U1";
    if (!p.comps.empty()) ref0 = next_free_ref(p, p.comps.back().ref.empty() ? "U0" : p.comps.back().ref);
    Vec2 c = p.board_bbox().center();
    for (;;) {
        ui::Form f(L"Add Placement", 520);
        f.icon((int)ui::Icon::Plus).header(L"Add Placement")
            .subtitle(L"Coordinates in " + units_label() + L", relative to the project origin; angle in degrees CCW");
        f.text("ref", L"Reference", ui::W(ref0));
        f.text("x", L"X", fmt_len(c.x - p.virtual_origin.x, false));
        f.text("y", L"Y", fmt_len(c.y - p.virtual_origin.y, false));
        f.button("paste", L"Paste X/Y from clipboard", [](ui::Form& F) {
            double x, y;
            if (parse_xy_text(clipboard_text(), x, y)) { F.set("x", ui::fmt_num(x, 4)); F.set("y", ui::fmt_num(y, 4)); }
            else toast("The clipboard does not contain two numbers (X, Y)", ToastKind::Warning);
        }).tip(L"Accepts \u201C12.5, 30\u201D, \u201C12.5<tab>30\u201D or \u201CX12.5 Y30\u201D (current display units)");
        f.number("rot", L"Angle", 0, 3);
        f.combo("side", L"Side", {L"Top", L"Bottom"}, view_bottom(*d) ? L"Bottom" : L"Top");
        f.combo("pkg", L"Package", pk, pk.empty() ? L"" : pk.front(), true);
        f.combo("pn", L"Part number", pn, L"", true);
        f.check("again", L"Add another after this one", false);
        f.validate([&p](ui::Form& F, std::wstring& e) {
            const std::string r = F.u8("ref");
            if (r.empty()) { e = L"Enter a reference."; return false; }
            if (p.find_ref(r)) { e = L"Reference " + ui::W(r) + L" already exists."; return false; }
            if (parse_len(F.str("x"), NAN) != parse_len(F.str("x"), NAN) || parse_len(F.str("y"), NAN) != parse_len(F.str("y"), NAN)) { e = L"Enter numeric X and Y."; return false; }
            return true;
        });
        f.ok_text(L"Add");
        if (!f.run(g_hwnd)) return;
        Component nc;
        nc.ref = f.u8("ref");
        nc.pos = Vec2{parse_len(f.str("x"), 0) + p.virtual_origin.x, parse_len(f.str("y"), 0) + p.virtual_origin.y};
        nc.rot = std::fmod(std::fmod(f.num("rot", 0), 360.0) + 360.0, 360.0);
        nc.rot_imported = nc.rot;
        nc.side = f.index("side") == 1 ? Side::Bottom : Side::Top;
        nc.package = f.u8("pkg");
        nc.number = f.u8("pn");
        d->checkpoint("Add placement " + nc.ref);
        p.add_component(nc);
        d->changed(ui::CH_GEOMETRY);
        status("Added placement " + nc.ref, ToastKind::Success);
        if (!f.checked("again")) return;
        ref0 = next_free_ref(p, nc.ref);
        c = nc.pos;
    }
}
static void duplicate_placements(const DocPtr& d) {
    Project& p = d->prj;
    std::vector<Component> add;
    for (auto& c : p.comps) if (c.selected && c.feature == Feature::Component) add.push_back(c);
    if (add.empty()) { toast("Select the placements to duplicate first", ToastKind::Info); return; }
    d->checkpoint("Duplicate " + std::to_string(add.size()) + " placement(s)");
    const double off = settings().grid_mm > 0 ? settings().grid_mm : 1.0;
    for (auto& c : p.comps) c.selected = false;
    for (auto c : add) {
        c.ref = next_free_ref(p, c.ref.empty() ? "X0" : c.ref);
        c.pos.x += off; c.pos.y += off;
        c.selected = true;
        p.add_component(c);
    }
    d->changed(ui::CH_GEOMETRY);
    toast(std::to_string(add.size()) + " placement(s) duplicated (offset " + ui::U(fmt_len(off)) + ")", ToastKind::Success, 4.f, "Undo", []() { post_command(ID_EDIT_UNDO); });
}

// R10 §3: Settings > Appearance — UI scale (80-250 %), separate icon scale, performance mode. Applied live.
static void appearance_dialog() {
    auto& st = settings();
    struct S2 { int ui, icon, perf; int ui0, icon0, perf0; };
    auto v = std::make_shared<S2>(S2{st.ui_scale_pct, st.icon_scale_pct, st.perf_mode, st.ui_scale_pct, st.icon_scale_pct, st.perf_mode});
    auto apply = [v]() {
        auto& s2 = settings();
        s2.ui_scale_pct = v->ui; s2.icon_scale_pct = v->icon; s2.perf_mode = v->perf;
        set_ui_scale(v->ui / 100.f, v->icon / 100.f);
        set_perf_mode(v->perf == 2);
        for (auto& u : g_docs) u->doc->changed(ui::CH_ALL);   // canvases re-render at the new scale
    };
    // R13 (REVIEW_V12 P1-07): two columns so Close is reachable without scrolling at 100 % and 120 %
    ui::Form f(L"Appearance", 820);
    f.icon((int)ui::Icon::Theme).header(L"Appearance").subtitle(L"Text, control and icon size; changes apply immediately");
    f.custom("sc", [v, apply](ui::Form& F, float w_all) {
        const float gap = gui::S(28);
        const float w = std::floor((w_all - gap) * 0.5f);
        ImGui::BeginGroup();
        ImGui::PushItemWidth(w - gui::S(150));
        auto row = [&](const char* label, const char* id, int* val, int lo, int hi, const char* tip) {
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label); ImGui::SameLine(gui::S(150));
            ImGui::SliderInt(id, val, lo, hi, "%d %%", ImGuiSliderFlags_AlwaysClamp);
            tooltip(tip);
            if (ImGui::IsItemDeactivatedAfterEdit()) { *val = (*val + 2) / 5 * 5; F.later(apply); }
        };
        row("UI scale", "##ui", &v->ui, 80, 250, "Text, menus, toolbars, panels and dialogs. 130 % is the default (about 30 % larger than 100 %). Applied on top of the Windows display scaling.");
        row("Icon scale", "##icon", &v->icon, 50, 250, "Extra icon size relative to the UI scale (toolbar and list icons).");
        ImGui::PopItemWidth();
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Presets"); ImGui::SameLine(gui::S(150));
        for (int pr : {100, 130, 160, 200}) {
            char b[16]; std::snprintf(b, sizeof b, "%d %%", pr);
            if (button(b, ui::Icon::None, v->ui == pr)) { v->ui = pr; F.later(apply); }
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ImGui::Spacing();
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Performance mode"); ImGui::SameLine(gui::S(150));
        const char* pm[] = {"Automatic (suggest when slow)", "Off", "On"};
        ImGui::SetNextItemWidth(w - gui::S(150));
        if (ImGui::Combo("##perf", &v->perf, pm, 3)) F.later(apply);
        tooltip("On: no animations, fades or glow effects (useful on virtual machines and software graphics).\nAutomatic: PCB Plotter suggests it once when it detects a software graphics adapter or slow frames.");
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextDisabled("Monitor scaling %.0f %%  \xC2\xB7  effective %.0f %%  \xC2\xB7  F12 shows the performance overlay", monitor_dpi_scale() * 100, dpi_scale() * 100);
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        section_header("LAYOUT");
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Project tabs"); ImGui::SameLine(gui::S(150));
        int tp = tab_placement();
        const char* tpn[] = {"Top (horizontal)", "Left edge (vertical)", "Right edge (vertical)"};
        ImGui::SetNextItemWidth(w - gui::S(150));
        if (ImGui::Combo("##tabplace", &tp, tpn, 3)) app::set_ui_pref("TabPlacement", tp);
        tooltip("Vertical strips free the row under the toolbar; the mouse wheel over the strip switches projects.");
        bool sb = settings().status_bar;
        if (ImGui::Checkbox("Status bar (coordinates, dimensions, messages)", &sb)) { settings().status_bar = sb; settings().save(); }
        bool gl = app::ui_pref("Glass", 1) != 0;
        if (ImGui::Checkbox("Frosted menus, tooltips and dialogs", &gl)) { app::set_ui_pref("Glass", gl ? 1 : 0); F.later([]() { apply_theme(); }); }
        tooltip("Translucent blurred backgrounds behind popups. Performance mode always uses solid backgrounds.");
        bool rp = app::ui_pref("DialogRememberPos", 0) != 0;
        if (ImGui::Checkbox("Remember dialog positions", &rp)) app::set_ui_pref("DialogRememberPos", rp ? 1 : 0);
        tooltip("Off: dialogs open centred on the window that opened them.");
        bool tm = tutorial_mode();
        if (ImGui::Checkbox("Tutorial hints over the editing views", &tm)) set_tutorial_mode(tm);
        ImGui::EndGroup();
        ImGui::SameLine(0, gap);
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (w_all - w - gap));
        // R12 (REVIEW_V11 §14): notification filters; errors always show
        section_header("NOTIFICATIONS");
        bool nn = app::ui_pref("NotifyShow", 1) != 0, nt = app::ui_pref("NotifyTips", 1) != 0, nc = app::ui_pref("NotifyConfirm", 1) != 0;
        if (ImGui::Checkbox("Show notifications (information and warnings)", &nn)) app::set_ui_pref("NotifyShow", nn ? 1 : 0);
        if (ImGui::Checkbox("Show tutorial tips (only while Tutorial Mode is on)", &nt)) app::set_ui_pref("NotifyTips", nt ? 1 : 0);
        tooltip("Tips appear only in Tutorial Mode; a tip you close is never shown again (Help > Reset Hints brings them back).");
        if (ImGui::Checkbox("Show confirmations (saved, imported, copied...)", &nc)) app::set_ui_pref("NotifyConfirm", nc ? 1 : 0);
        ImGui::BeginDisabled(); bool ce = true; ImGui::Checkbox("Critical errors (always shown)", &ce); ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0, gui::S(4)));
        section_header("RECOVERY");
        int am = app::ui_pref("AutosaveMinutes", 2);
        ImGui::SetNextItemWidth(gui::S(110));
        if (ImGui::InputInt("Autosave every N min", &am)) app::set_ui_pref("AutosaveMinutes", std::clamp(am, 0, 60));
        tooltip("0 = only after imports. Unsaved work is snapshotted to %LOCALAPPDATA%\\PCBPlotter\\Recovery (last 3 per project; kept 7 days after a normal close). "
                "The project file itself is never touched. File > Recovery Snapshots lists them.");
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
    });
    f.ok_text(L"Close").cancel_text(L"Revert");
    if (!f.run(g_hwnd)) { v->ui = v->ui0; v->icon = v->icon0; v->perf = v->perf0; apply(); }
    settings().save();
}

static void draw_menu_bar() {
    if (!ImGui::BeginMenuBar()) return;
    const bool doc = active_docui() != nullptr;
    if (ImGui::BeginMenu("File")) {
        for (int id : {ID_FILE_HOME, ID_FILE_NEW, ID_FILE_OPEN}) menu_item(id);
        auto& mru = settings().mru;
        if (begin_menu_icon("Recent Projects", ui::Icon::Recent, !mru.empty())) {
            for (size_t i = 0; i < mru.size(); ++i) {
                std::string t = std::to_string(i + 1) + "  " + ui::U(mru[i]);
                std::string ext = file_ext_lower(ui::U(mru[i]));
                if (menu_item_icon(t.c_str(), ext == ".cpf" ? ui::Icon::ImportCad : ext == ".pcbp" ? ui::Icon::Board : ui::Icon::Open)) post_command(ID_FILE_MRU_FIRST + (int)i);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        for (int id : {ID_FILE_SAVE, ID_FILE_SAVEAS}) menu_item(id);
        menu_item(ID_FILE_RECOVERY_SNAPSHOTS);
        ImGui::Separator();
        if (begin_menu_icon("Import", ui::Icon::ImportCad, doc)) { draw_import_items(); ImGui::EndMenu(); }
        if (begin_menu_icon("Export", ui::Icon::Export, doc)) { draw_export_items(); ImGui::EndMenu(); }
        ImGui::Separator();
        for (int id : {ID_FILE_CLOSE, ID_WIN_CLOSE_ALL}) menu_item(id);
        ImGui::Separator();
        menu_item(ID_FILE_EXIT);
        ImGui::EndMenu();
    }
    static const wchar_t* s_demo_menu = _wgetenv(L"PCBP_DEMO_MENU");   // automation: hold a menu open for screenshots
    if (s_demo_menu && !ImGui::IsPopupOpen(ImGui::GetID(ui::U(s_demo_menu).c_str()), 0)) ImGui::OpenPopup(ui::U(s_demo_menu).c_str());
    if (!g_walk_menu.empty() && !ImGui::IsPopupOpen(ImGui::GetID(g_walk_menu.c_str()), 0)) ImGui::OpenPopup(g_walk_menu.c_str());   // ID walk
    if (ImGui::BeginMenu("Edit")) {
        for (int id : {ID_EDIT_UNDO, ID_EDIT_REDO}) menu_item(id);
        ImGui::Separator();
        for (int id : {ID_EDIT_CUT, ID_EDIT_COPY, ID_EDIT_PASTE, ID_EDIT_DELETE, ID_EDIT_SELECT_ALL, ID_TOOL_SEARCH}) menu_item(id);
        ImGui::Separator();
        for (int id : {ID_BOARD_INSERT_FREE, ID_EDIT_DUPLICATE, ID_CTX_EDIT_LOCATION, ID_PRJ_CHECKPOINT}) menu_item(id);
        ImGui::Separator();
        if (begin_menu_icon("Assign to Layer", ui::Icon::Assign, doc)) { move_to_layer_menu_items(adoc()); ImGui::EndMenu(); }
        if (doc && selsets_visible(*adoc()) && begin_menu_icon("Selection Sets (legacy)", ui::Icon::SelectAll, doc)) { selset_menu_items(adoc()); ImGui::EndMenu(); }
        group_ops_menu_items(adoc());   // Select By + Group Edit (group_ops.cpp)
        for (int id : {ID_SEL_POINT_MEMBERS, ID_SEL_UNASSIGNED}) menu_item(id);
        ImGui::Separator();
        for (int id : {ID_TOOL_MEASURE, ID_TOOL_MOVE}) menu_item(id);
        ImGui::Separator();
        for (int id : {ID_EDIT_SNAP, ID_EDIT_GRID, ID_EDIT_GRID_SPACING}) menu_item(id);
        ImGui::Separator();
        menu_item(ID_EDIT_OPTIONS);
        menu_item(ID_VIEW_APPEARANCE);
        if (menu_item_icon("Reset Suppressed Warnings", ui::Icon::Warning)) { int n = reset_suppressed_warnings(); status(std::to_string(n) + " suppressed warning(s) re-enabled", ToastKind::Info); }
        if (ImGui::IsItemHovered()) tooltip("Show again every informational message that was dismissed with \"Don't show this message again\"");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (int id : {ID_VIEW_ZOOM_IN, ID_VIEW_ZOOM_OUT, ID_VIEW_ZOOM_RECT, ID_VIEW_ZOOM_100, ID_VIEW_ZOOM_EXTENTS}) menu_item(id);
        ImGui::Separator();
        for (int id : {ID_VIEW_SIDE_TOP, ID_VIEW_SIDE_BOTTOM, ID_VIEW_SIDE_CYCLE, ID_VIEW_SIDE_NORMAL, ID_VIEW_SIDE_XRAY, ID_VIEW_BACK_SELECT}) menu_item(id);   // R17c: Ghost merged into X-ray (specs/VIEW_MODES.md)
        ImGui::Separator();
        for (int id : {ID_VIEWER_ALL_IMAGES, ID_VIEW_BOARD_FRAME, ID_BOARD_FRAME_COLOR, ID_VIEW_LABELS, ID_BOARD_AUTOPLACE_LABELS, ID_TOOL_VERIFY_ANGLES}) menu_item(id);   // R16c: Auto-Place Labels moved from Board
        ImGui::Separator();
        if (begin_menu_icon("Units", ui::Icon::Units)) { for (int id : {ID_VIEW_UNITS_MM, ID_VIEW_UNITS_INCH, ID_VIEW_UNITS_MIL}) menu_item(id); ImGui::EndMenu(); }
        if (begin_menu_icon("Theme", ui::Icon::Theme)) { for (int id : {ID_VIEW_THEME_SYSTEM, ID_VIEW_THEME_LIGHT, ID_VIEW_THEME_DARK}) menu_item(id); ImGui::Separator(); menu_item(ID_VIEW_THEME_CYCLE); ImGui::EndMenu(); }
        menu_item(ID_VIEW_APPEARANCE);
        menu_item(ID_VIEW_STATUSBAR);
        ImGui::EndMenu();
    }
    if (const char* sm = getenv("PCBP_DEMO_SUBMENU"); doc && sm && (!strcmp(sm, "Panelize") || !strcmp(sm, "Outline")))   // R22 automation: open Board too
        if (!ImGui::IsPopupOpen(ImGui::GetID("Board"), 0)) ImGui::OpenPopup("Board");
    if (doc && ImGui::BeginMenu("Board")) {   // R16c (REVIEW_V15 §8): geometry, placement manipulation and transformation only
        menu_item(ID_BOARD_INSERT_FREE);
        ImGui::Separator();
        if (const char* sm = getenv("PCBP_DEMO_SUBMENU"); sm && (!strcmp(sm, "Panelize") || !strcmp(sm, "Outline"))) {   // automation: screenshots
            const std::string l = std::string("      ") + sm;
            if (!ImGui::IsPopupOpen(ImGui::GetID(l.c_str()), 0)) ImGui::OpenPopup(l.c_str());
        }
        // R22 (REVIEW_V21 §1/§3/§4): Panelize and Outline submenus, three explicit commands each. Detect Repeated
        // Circuits lives in the Gerber viewer; Confirm-as-measured lives on the inferred-outline banner; the
        // References submenu (Reference Fusion / Clear All References) is retired from the menu.
        if (begin_menu_icon("Panelize", ui::Icon::Panelize)) {
            for (int id : {ID_BOARD_PANELIZE, ID_BOARD_ARRAY_LINEAR, ID_BOARD_ARRAY_DELETE}) menu_item(id);
            ImGui::EndMenu();
        }
        if (begin_menu_icon("Outline", ui::Icon::Board)) {
            for (int id : {ID_BOARD_ADD_FRAME, ID_BOARD_CLEAR_PANEL_FRAME, ID_BOARD_CLEAR_IMAGE_FRAME}) menu_item(id);
            ImGui::EndMenu();
        }
        if (begin_menu_icon("Align / Transform", ui::Icon::Offset)) {
            menu_item(ID_BOARD_OFFSET_ROTATE);   // Transform... (translate / rotate / scale / mirror; replaces Global Offset + Rescale)
            ImGui::Separator();
            menu_item(ID_BOARD_ALIGN_CAD_GERBER);   // R13 §6
            for (int id : {ID_BOARD_ALIGN_MOVE, ID_BOARD_ALIGN_ROTATE, ID_BOARD_ALIGN_2REF}) menu_item(id);
            ImGui::Separator();
            menu_item(ID_BOARD_REBASE_ORIGIN);
            menu_item(ID_BOARD_APPLY_OFFSET_FILE);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (doc && ImGui::BeginMenu("BOM")) {   // R12 §3: the floating BOM wizard; R14d: audited (one import entry, one view entry)
        menu_item(ID_BOM_IMPORT);
        if (ImGui::IsItemHovered()) gui::tooltip("Open a BOM file (.xlsx / .xls / .ods / .csv / .txt) in the BOM wizard: rows, column mapping, review, import");
        if (menu_item_icon("Import from M1 by Job ID\xE2\x80\xA6", ui::Icon::ImportBom)) defer([]() { if (app::DocPtr dd = adoc()) app::dlg_import_bom_m1(g_hwnd, *dd); });   // R14d
        if (ImGui::IsItemHovered()) { gui::tooltip("Fetch a job's BOM from the M1 ERP database (read-only) and open it in the BOM wizard"); g_status = "Fetch a job's BOM from M1 by Job ID and open it in the BOM wizard"; }
        ImGui::Separator();
        menu_item(ID_BOM_EDIT);
        if (ImGui::IsItemHovered()) gui::tooltip("Open the Components > BOM table: every part number, its references, process and package; edit in place");
        ImGui::EndMenu();
    }
    if (doc && ImGui::BeginMenu("Manufacturing")) {
        menu_item(ID_MFG_CHECK);
        ImGui::Separator();
        for (int id : {ID_MFG_PREP_REPORT, ID_MFG_OPPORTUNITY, ID_MFG_ROUTING_REPORT, ID_MFG_PLACEMENT_REPORT}) menu_item(id);
        ImGui::Separator();
        for (int id : {ID_MFG_AUTO_ASSIGN, ID_MFG_CLEAR_ASSIGN, ID_MFG_CLEAR_UNUSED_POINTS}) menu_item(id);
        ImGui::Separator();
        for (int id : {ID_POINT_PROPERTIES, ID_POINT_ADD, ID_POINT_GENERATE}) menu_item(id);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Library")) {
        for (int id : {ID_LIB_FACTORY_MODEL, ID_LIB_PACKAGE_LIBRARY, ID_LIB_NUMBER_LIBRARY}) menu_item(id);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (menu_item_icon("Open Document / Spreadsheet / Email\xE2\x80\xA6", ui::Icon::Notes)) defer([]() { files_open_dialog(0); });
        files_project_menu_items();   // attach open document to the project, Project Documents (sheet_editor.cpp)
        if (menu_item_icon("Open Machine Program in Editor\xE2\x80\xA6", ui::Icon::MachineProg)) defer([]() { pnp_open_dialog(); });
        if (menu_item_icon("New Spreadsheet", ui::Icon::New)) defer([]() { files_new_sheet(); });
        ImGui::Separator();
        menu_item(ID_FILE_STATUS);
        menu_item(ID_HELP_VERIFY);
        ImGui::Separator();
        if (menu_item_icon("File Associations\xE2\x80\xA6", ui::Icon::Link)) defer([]() { file_assoc_dialog(); });
        menu_item(ID_LIB_PLUGINS);
        ImGui::Separator();
        menu_item(ID_VIEW_PERF_OVERLAY);
        if (menu_item_icon("Performance Mode", ui::Icon::Settings, nullptr, perf_mode())) defer([]() { auto& st = settings(); st.perf_mode = perf_mode() ? 1 : 2; st.save(); set_perf_mode(st.perf_mode == 2); });
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Migration")) {
        for (int id : {ID_FILE_OPEN_CPF, ID_FILE_MIGRATE_BATCH}) menu_item(id);
        ImGui::Separator();
        for (int id : {ID_LIB_IMPORT_CCLIB, ID_LIB_UPDATE_RENDERING}) menu_item(id);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        for (int id : {ID_HELP_CONTENTS, ID_HELP_SHORTCUTS}) menu_item(id);
        ImGui::Separator();
        if (menu_item_icon("Tutorial Mode", ui::Icon::Question, nullptr, tutorial_mode())) set_tutorial_mode(!tutorial_mode());
        if (ImGui::IsItemHovered()) tooltip("Show instructional hints over the editing views (off by default). Hover tooltips are always available.");
        if (menu_item_icon("Reset Hints", ui::Icon::Undo, nullptr, false, tutorial_mode())) { int n = hints_reset(); status(std::to_string(n) + " dismissed hint(s) shown again", ToastKind::Info); }
        if (menu_item_icon("Reset Suppressed Warnings", ui::Icon::Warning)) { int n = reset_suppressed_warnings(); status(std::to_string(n) + " suppressed warning(s) re-enabled", ToastKind::Info); }
        ImGui::Separator();
        menu_item(ID_HELP_ABOUT);
        ImGui::EndMenu();
    }
    // theme toggle: ONE button, vertically centred in the menu bar; click cycles System -> Light -> Dark
    {
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        ImRect mb = win->MenuBarRect();
        const float bh = std::max(S(16), mb.GetHeight() - S(4)), bwid = bh + S(8);
        ImVec2 a(mb.Max.x - bwid - S(10), mb.Min.y + (mb.GetHeight() - bh) * 0.5f);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushClipRect(mb.Min, mb.Max, false);
        bool clicked = ImGui::InvisibleButton("##themetoggle", ImVec2(bwid, bh));
        bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const GTheme& t = th();
        ui::ThemeMode m = ui::theme_mode();
        dl->AddRectFilled(a, ImVec2(a.x + bwid, a.y + bh), hov ? t.raised : t.panel2, bh * 0.5f);
        if (hov) dl->AddRect(a, ImVec2(a.x + bwid, a.y + bh), t.border, bh * 0.5f);
        ImVec2 c(a.x + bwid * 0.5f, a.y + bh * 0.5f);
        const float r = bh * 0.26f;
        const bool dark = t.dark;
        if (dark) {   // moon: disc minus offset disc
            dl->AddCircleFilled(c, r, t.text, 24);
            dl->AddCircleFilled(ImVec2(c.x + r * 0.55f, c.y - r * 0.45f), r * 0.85f, hov ? t.raised : t.panel2, 24);
        } else {      // sun: disc + 8 rays
            dl->AddCircleFilled(c, r * 0.62f, t.text, 20);
            for (int k = 0; k < 8; ++k) {
                float an = k * 3.14159265f / 4;
                ImVec2 u(std::cos(an), std::sin(an));
                dl->AddLine(c + u * (r * 0.95f), c + u * (r * 1.35f), t.text, S(1.5f));
            }
        }
        if (m == ui::ThemeMode::System) {   // "auto" marker
            ImVec2 q(a.x + bwid - S(7), a.y + bh - S(6));
            dl->AddCircleFilled(q, S(3.2f), t.accent, 12);
        }
        ImGui::PopClipRect();
        if (hov) tooltip(m == ui::ThemeMode::System ? (dark ? "Theme: follow Windows (currently dark)\nClick: switch to Light" : "Theme: follow Windows (currently light)\nClick: switch to Light")
                         : m == ui::ThemeMode::Light ? "Theme: Light\nClick: switch to Dark" : "Theme: Dark\nClick: follow Windows");
        if (clicked) post_command(ID_VIEW_THEME_CYCLE);
    }
    ImGui::EndMenuBar();
}

static void draw_toolbar() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(2), 0));
    ImGui::SetCursorPosX(S(8));
    cmd_button(ID_FILE_HOME); cmd_button(ID_FILE_NEW); cmd_button(ID_FILE_OPEN); cmd_button(ID_FILE_SAVE);
    toolbar_separator();
    cmd_button(ID_EDIT_UNDO); cmd_button(ID_EDIT_REDO);
    toolbar_separator();
    cmd_button(ID_IMPORT_CAD); cmd_button(ID_IMPORT_GERBER); cmd_button(ID_IMPORT_CENTROID); cmd_button(ID_BOM_IMPORT);
    toolbar_separator();
    cmd_button(ID_TOOL_SEARCH); cmd_button(ID_MFG_CHECK);
    // R17c (REVIEW_V16 §7): Print left the toolbar (File > Print, Ctrl+P).
    if (DocUI* u = active_docui()) if (View* v = u->current()) { toolbar_separator(); v->draw_toolbar(); }
    if (busy()) { ImGui::SameLine(0, S(12)); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(5)); spinner(S(8)); }
    ImGui::PopStyleVar();
    ImGui::NewLine();
}

// ---------------------------------------------------------------------------
// Home page: generic SMT CAM start screen (actions, workflow, recent projects)
// ---------------------------------------------------------------------------
// Run `cmd` from the Home page. Commands that work on a project get one: the
// most recently active project for workflow shortcuts, else a new empty one.
static void landing_run(int cmd, bool needs_doc, bool reuse_doc, ViewKind view = ViewKind::Count) {
    defer([cmd, needs_doc, reuse_doc, view]() {
        DocPtr d;
        if (needs_doc || view != ViewKind::Count) {
            if (reuse_doc && !g_docs.empty()) {
                int i = (g_active >= 0 && g_active < (int)g_docs.size()) ? g_active : (int)g_docs.size() - 1;
                g_active = i; d = g_docs[i]->doc;
            } else d = new_document();
            g_show_landing = false;
            activate_view(d, view != ViewKind::Count ? view : ViewKind::Plotter);
        }
        if (cmd) run_command(cmd);
    });
}

static std::string landing_date(const std::wstring& path, bool* exists) {
    struct E { std::string s; bool ok; double t; };
    static std::map<std::wstring, E> cache;
    const double now = ImGui::GetTime();
    auto it = cache.find(path);
    if (it == cache.end() || now - it->second.t > 5.0) {
        E e{"", false, now};
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
            FILETIME lt; SYSTEMTIME st;
            FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt);
            FileTimeToSystemTime(&lt, &st);
            char b[48];
            std::snprintf(b, sizeof b, "%04d-%02d-%02d  %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
            e.s = b; e.ok = true;
        } else e.s = "file not found";
        it = cache.insert_or_assign(path, e).first;
    }
    if (exists) *exists = it->second.ok;
    return it->second.s;
}

// filled pill with centered text; returns its width
static float landing_chip(ImDrawList* dl, ImVec2 a, const char* txt, ImU32 bg, ImU32 fg, float fs) {
    ImVec2 ts = g_font->CalcTextSizeA(fs, FLT_MAX, 0, txt);
    ImVec2 b(a.x + ts.x + S(12), a.y + fs + S(5));
    dl->AddRectFilled(a, b, bg, S(5));
    dl->AddText(g_font, fs, ImVec2(a.x + S(6), a.y + S(2.5f)), fg, txt);
    return b.x - a.x;
}

static void draw_landing(ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const GTheme& t = th();
    const float view_h = size.y;
    const float col_w = std::min(size.x - S(48), S(1040));
    const float x0 = p.x + (size.x - col_w) * 0.5f;
    float y = p.y + S(18);
    const float fs_small = ImGui::GetFontSize() * 0.88f;

    // ---- compact title (R12 §11: no banner; functional content first, fits at 130-175 %)
    {
        const float L = S(40);
        draw_logo(dl, ImVec2(x0, y), L);
        const float tx = x0 + L + S(14);
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImVec2(tx, y + S(1)), t.text, "PCB Plotter");
        ImGui::PopFont();
        dl->PushClipRect(ImVec2(tx, y), ImVec2(x0 + col_w, y + L + S(4)), true);
        dl->AddText(g_font, fs_small, ImVec2(tx, y + S(22)), t.text_dim, "SMT assembly CAM \xE2\x80\x94 CAD, Gerber and BOM data to panels and placement programs.");
        dl->PopClipRect();
        y += L + S(18);
    }

    // ---- primary actions
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    section_header("START");
    y += S(24);
    struct Card { ui::Icon ic; const char* title; const char* sub; int cmd; bool needs_doc; bool primary; };
    static const Card cards[] = {
        {ui::Icon::New, "New Project", "Empty assembly with the default process model", ID_FILE_NEW, false, true},
        {ui::Icon::Open, "Open Project", "Open a .pcbp project or any supported file", ID_FILE_OPEN, false, false},
        {ui::Icon::ImportGerber, "Import Gerber Layers", "RS-274X / 274D copper, paste, silk, drill", ID_IMPORT_GERBER, true, false},
        {ui::Icon::ImportCad, "Import CAD", "ODB++, IPC-2581, GenCAD, KiCad, EAGLE and more", ID_IMPORT_CAD, true, false},
        {ui::Icon::ImportCentroid, "Import Centroid", "Pick-and-place / XY text with column auto-detect", ID_IMPORT_CENTROID, true, false},
        {ui::Icon::ImportBom, "Import BOM", "Spreadsheet / text BOM, reference ranges, DNP", ID_BOM_IMPORT, true, false},
        {ui::Icon::Wand, "Migrate Legacy Project", "Import a legacy CAM project (.cpf) into a new project", ID_FILE_OPEN_CPF, false, false},
        {ui::Icon::Machine, "Rebuild from Program", "Recover a project from a pick-and-place program", ID_IMPORT_MACHINE, false, false},
    };
    const int ncard = (int)(sizeof cards / sizeof cards[0]);
    const int cols = col_w < S(620) ? 2 : 4;
    const float gap = S(12), cw = (col_w - gap * (cols - 1)) / cols;
    float ch = S(78);   // compact (R12 §11); grows to fit the wrapped subtitle at large UI scales
    for (int i = 0; i < ncard; ++i)
        ch = std::max(ch, S(48) + g_font->CalcTextSizeA(fs_small, FLT_MAX, cw - S(24), cards[i].sub).y + S(10));
    for (int i = 0; i < ncard; ++i) {
        const Card& cd = cards[i];
        ImVec2 a(x0 + (i % cols) * (cw + gap), y + (i / cols) * (ch + gap)), b(a.x + cw, a.y + ch);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("card", ImVec2(cw, ch));
        bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
        ImGui::PopID();
        if (hov) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); }
        float h = anim(ImGui::GetID("landcard") + (ImGuiID)i, hov ? 1.f : 0.f, 16.f);
        float lift = S(3) * h - (act ? S(1) : 0);
        a.y -= lift; b.y -= lift;
        dl->AddRectFilled(ImVec2(a.x + S(1), a.y + S(4) + lift), ImVec2(b.x + S(1), b.y + S(4) + lift), IM_COL32(0, 0, 0, (int)((t.dark ? 80 : 30) * h)), S(12));
        const ImU32 base = cd.primary ? col_lerp(t.panel2, t.accent, 0.12f) : t.panel2;
        dl->AddRectFilled(a, b, col_lerp(base, t.raised, h), S(12));
        dl->AddRect(a, b, col_lerp(cd.primary ? col_alpha(t.accent, 0.6f) : t.border, t.accent, h), S(12), 0, 1.f + 0.6f * h);
        // icon tile
        ImVec2 ia(a.x + S(12), a.y + S(10)), ib(ia.x + S(30), ia.y + S(30));
        dl->AddRectFilled(ia, ib, col_alpha(t.accent, 0.12f + 0.12f * h), S(8));
        icon_draw(dl, cd.ic, ImVec2(ia.x + S(4), ia.y + S(4)), S(22));
        // arrow on hover
        if (h > 0.02f) dl->AddText(ImVec2(b.x - S(22) - S(4) * (1 - h), a.y + S(14)), col_alpha(t.accent, h), "\xE2\x86\x92");
        ImGui::PushFont(g_font_bold);
        dl->PushClipRect(a, ImVec2(b.x - S(8), b.y), true);
        dl->AddText(ImVec2(a.x + S(52), a.y + S(10) + (S(30) - ImGui::GetFontSize()) * 0.5f), t.text, cd.title);
        dl->PopClipRect();
        ImGui::PopFont();
        dl->AddText(g_font, fs_small, ImVec2(a.x + S(14), a.y + S(46)), t.text_dim, cd.sub, nullptr, cw - S(24));
        if (clicked) {
            if (cd.needs_doc) landing_run(cd.cmd, true, false);
            else post_command(cd.cmd);
        }
    }
    y += ((ncard + cols - 1) / cols) * (ch + gap) + S(18);

    // ---- typical workflow
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    section_header("TYPICAL WORKFLOW");
    y += S(24);
    {
        struct Step { ui::Icon ic; const char* title; const char* tip; int cmd; ViewKind view; };
        static const Step steps[] = {
            {ui::Icon::Layers, "Load data", "Import CAD, Gerber, centroid and BOM data", ID_IMPORT_CAD, ViewKind::Plotter},
            {ui::Icon::PackageEditor, "Shapes", "Check and complete package footprints in the package library", 0, ViewKind::Packages},
            {ui::Icon::Panelize, "Assemble / Panel", "Step-and-repeat images, panel and circuit outlines", ID_BOARD_PANELIZE, ViewKind::Plotter},
            {ui::Icon::FactoryModel, "Assign process", "Assign components to process / machine points", ID_MFG_AUTO_ASSIGN, ViewKind::Plotter},
            {ui::Icon::Export, "Export", "Generate pick-and-place programs, visual aids and reports", 0, ViewKind::Machine},
        };
        const int ns = 5;
        const float sg = S(26), sw = (col_w - sg * (ns - 1)) / ns, sh = S(54);
        for (int i = 0; i < ns; ++i) {
            const Step& st = steps[i];
            ImVec2 a(x0 + i * (sw + sg), y), b(a.x + sw, a.y + sh);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(i + 50);
            bool clicked = ImGui::InvisibleButton("step", ImVec2(sw, sh));
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            float h = anim(ImGui::GetID("landstep") + (ImGuiID)i, hov ? 1.f : 0.f, 18.f);
            if (hov) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                std::string tip = std::string(st.tip) + (g_docs.empty() ? "\n(starts a new project)" : "\n(opens it in the current project)");
                tooltip(tip.c_str());
            }
            dl->AddRectFilled(a, b, col_lerp(t.panel, t.raised, h), S(sh * 0.5f / dpi_scale()));
            dl->AddRect(a, b, col_lerp(t.border, t.accent, h), S(sh * 0.5f / dpi_scale()));
            // number bubble
            ImVec2 c(a.x + S(26), a.y + sh * 0.5f);
            dl->AddCircleFilled(c, S(17), col_lerp(col_alpha(t.accent, 0.16f), t.accent, h), 28);
            icon_draw(dl, st.ic, ImVec2(c.x - S(10), c.y - S(10)), S(20), 1.f, h > 0.5f);
            char num[8]; std::snprintf(num, sizeof num, "%d", i + 1);
            dl->AddText(g_font, fs_small * 0.85f, ImVec2(a.x + S(48), a.y + S(9)), t.text_faint, num);
            ImGui::PushFont(g_font_bold);
            dl->PushClipRect(a, ImVec2(b.x - S(6), b.y), true);
            dl->AddText(ImVec2(a.x + S(48), a.y + S(24)), t.text, st.title);
            dl->PopClipRect();
            ImGui::PopFont();
            if (i + 1 < ns) {   // chevron connector
                float mx = b.x + sg * 0.5f, my = a.y + sh * 0.5f;
                dl->AddLine(ImVec2(mx - S(4), my - S(6)), ImVec2(mx + S(3), my), t.text_faint, S(2));
                dl->AddLine(ImVec2(mx + S(3), my), ImVec2(mx - S(4), my + S(6)), t.text_faint, S(2));
            }
            if (clicked) landing_run(st.cmd, true, true, st.view);
        }
        y += sh + S(26);
    }

    // ---- recent projects (pinned first)
    auto& s = settings();
    std::vector<std::pair<std::wstring, bool>> rec;   // path, pinned
    for (auto& q : s.pinned) rec.push_back({q, true});
    for (auto& q : s.mru) if (!s.is_pinned(q)) rec.push_back({q, false});
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    section_header("RECENT PROJECTS");
    y += S(24);
    if (rec.empty()) {
        ImVec2 a(x0, y), b(x0 + col_w, y + S(46));
        dl->AddRectFilled(a, b, t.panel, S(10));
        icon_draw(dl, ui::Icon::Recent, ImVec2(a.x + S(14), a.y + S(13)), S(20), 0.6f);
        dl->AddText(ImVec2(a.x + S(44), a.y + S(15)), t.text_faint, "No recent projects yet \xE2\x80\x94 open or import a file to get started.");
        y += S(56);
    } else {
        ImVec2 la(x0, y), lb(x0 + col_w, y + rec.size() * S(52) + S(8));
        dl->AddRectFilled(la, lb, t.panel, S(12));
        dl->AddRect(la, lb, t.border, S(12));
        y += S(4);
    }
    std::wstring to_remove, to_toggle;
    bool toggle_to = false;
    for (size_t i = 0; i < rec.size(); ++i) {
        const std::wstring& path = rec[i].first;
        const bool pinned = rec[i].second;
        const float rh = S(52), rw = col_w - S(8);
        ImVec2 a(x0 + S(4), y), b(a.x + rw, y + rh);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID((int)i + 100);
        ImGui::SetNextItemAllowOverlap();
        bool clicked = ImGui::InvisibleButton("r", ImVec2(rw, rh));
        bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
        float h = anim(ImGui::GetID("landmru"), hov ? 1.f : 0.f, 18.f);
        if (h > 0.01f) {
            dl->AddRectFilled(a, b, col_alpha(t.raised, h), S(9));
            dl->AddRectFilled(ImVec2(a.x, a.y + S(10)), ImVec2(a.x + S(3), b.y - S(10)), col_alpha(t.accent, h), S(2));
        }
        std::string full = ui::U(path);
        size_t sl = full.find_last_of("\\/");
        std::string name = sl == std::string::npos ? full : full.substr(sl + 1);
        std::string dir = sl == std::string::npos ? std::string() : full.substr(0, sl);
        std::string ext = file_ext_lower(full);
        bool exists = true;
        std::string date = landing_date(path, &exists);
        // thumbnail tile: type colour + icon + extension
        const ImU32 tc = ext == ".pcbp" ? t.accent : ext == ".cpf" ? IM_COL32(200, 140, 40, 255) : IM_COL32(90, 150, 220, 255);
        ImVec2 ta(a.x + S(10), a.y + S(7)), tb(ta.x + S(38), ta.y + S(38));
        dl->AddRectFilled(ta, tb, col_alpha(tc, 0.16f + 0.08f * h), S(8));
        dl->AddRect(ta, tb, col_alpha(tc, 0.5f), S(8));
        icon_draw(dl, ext == ".cpf" ? ui::Icon::Wand : ext == ".pcbp" ? ui::Icon::Board : ui::Icon::ImportCad, ImVec2(ta.x + S(9), ta.y + S(5)), S(20), exists ? 1.f : 0.4f);
        {
            std::string e = ext.size() > 1 ? ext.substr(1) : "?";
            for (auto& ch : e) ch = (char)toupper((unsigned char)ch);
            float fs = fs_small * 0.72f;
            ImVec2 es = g_font->CalcTextSizeA(fs, FLT_MAX, 0, e.c_str());
            dl->AddText(g_font, fs, ImVec2((ta.x + tb.x - es.x) * 0.5f, tb.y - fs - S(2)), t.text_dim, e.c_str());
        }
        const float tx = a.x + S(60) + h * S(2);
        const float right_w = S(220);
        dl->PushClipRect(ImVec2(tx, a.y), ImVec2(b.x - right_w, b.y), true);
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImVec2(tx, a.y + S(8)), exists ? t.text : t.text_faint, name.c_str());
        ImGui::PopFont();
        dl->AddText(g_font, fs_small, ImVec2(tx, a.y + S(28)), t.text_faint, dir.c_str());
        dl->PopClipRect();
        // date (right side)
        dl->AddText(g_font, fs_small, ImVec2(b.x - right_w + S(16), a.y + S(18)), exists ? t.text_dim : t.warn, date.c_str());
        if (pinned) icon_draw(dl, ui::Icon::Tag, ImVec2(tx - S(18), a.y + S(4)), S(12), 0.9f);
        // hover actions: pin / remove
        if (h > 0.05f || pinned) {
            const float bs = S(26);
            ImGui::SetCursorScreenPos(ImVec2(b.x - bs * 2 - S(14), a.y + (rh - bs) * 0.5f));
            if (icon_button("pin", ui::Icon::Tag, pinned ? "Unpin" : "Pin to the top of this list", pinned, true, bs)) { to_toggle = path; toggle_to = !pinned; clicked = false; }
            ImGui::SameLine(0, S(4));
            if (icon_button("rm", ui::Icon::CloseDoc, "Remove from this list (the file is not deleted)", false, true, bs)) { to_remove = path; clicked = false; }
        } else if (hov) {
        }
        if (hov && !ImGui::IsAnyItemActive()) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); }
        ImGui::PopID();
        if (clicked) { std::wstring pth = path; defer([pth]() { open_document(pth); }); }
        y += rh;
    }
    if (!to_remove.empty()) { s.remove_mru(to_remove); s.save(); }
    if (!to_toggle.empty()) { s.set_pinned(to_toggle, toggle_to); if (toggle_to) s.add_mru(to_toggle); s.save(); }
    if (!rec.empty()) y += S(22);

    // ---- drop zone hint
    {
        ImVec2 a(x0, y), b(x0 + col_w, y + S(58));
        const float dash = S(8);
        const ImU32 dc = col_alpha(t.accent, 0.45f);
        for (float x = a.x + S(10); x < b.x - S(10); x += dash * 2) {
            dl->AddLine(ImVec2(x, a.y), ImVec2(std::min(x + dash, b.x - S(10)), a.y), dc, S(1.5f));
            dl->AddLine(ImVec2(x, b.y), ImVec2(std::min(x + dash, b.x - S(10)), b.y), dc, S(1.5f));
        }
        for (float yy = a.y + S(10); yy < b.y - S(10); yy += dash * 2) {
            dl->AddLine(ImVec2(a.x, yy), ImVec2(a.x, std::min(yy + dash, b.y - S(10))), dc, S(1.5f));
            dl->AddLine(ImVec2(b.x, yy), ImVec2(b.x, std::min(yy + dash, b.y - S(10))), dc, S(1.5f));
        }
        icon_draw(dl, ui::Icon::Folder, ImVec2(a.x + S(18), a.y + S(15)), S(28), 0.9f);
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImVec2(a.x + S(58), a.y + S(10)), t.text, "Drop files anywhere on this window");
        ImGui::PopFont();
        dl->AddText(g_font, fs_small, ImVec2(a.x + S(58), a.y + S(31)), t.text_dim,
                    "Projects, Gerber / drill layers, CAD (ODB++, IPC-2581, GenCAD...), centroid and BOM files are detected automatically.");
        y += S(58) + S(30);
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(size.x, std::max(view_h, y - p.y)));
}

// ---------------------------------------------------------------------------
// P1-10 / P2-06 project + document tabs: one tab style (gui::tab_item) with icon, close and the same context menu;
// unique "Untitled (n)" names; Home is not a tab while anything is open (toolbar Home / File > Home, and the app
// returns to Home when the last tab closes). Placement: top strip (default) or a vertical strip at the left / right
// edge (Appearance > Project tabs), where the mouse wheel switches between open projects.
// ---------------------------------------------------------------------------
std::map<const app::Doc*, int> g_untitled_no;
static std::string doc_tab_name(const DocPtr& d) {
    if (!d->prj.path.empty() || !d->prj.info.name.empty()) return ui::U(d->display_name());
    auto it = g_untitled_no.find(d.get());
    if (it == g_untitled_no.end()) {
        std::set<int> used;
        for (auto& u : g_docs) { auto q = g_untitled_no.find(u->doc.get()); if (q != g_untitled_no.end() && u->doc != d) used.insert(q->second); }
        int n = 1; while (used.count(n)) ++n;
        it = g_untitled_no.emplace(d.get(), n).first;
    }
    return it->second == 1 ? "Untitled" : "Untitled (" + std::to_string(it->second) + ")";
}
static void rename_project(const DocPtr& d) {
    std::wstring v = ui::W(doc_tab_name(d));
    if (!d->prj.path.empty()) v = ui::W(file_stem(d->prj.path));
    if (!ui::prompt_text(g_hwnd, L"Rename Project", d->prj.path.empty() ? L"Project name" : L"File name (renames the .pcbp file)", v) || v.empty()) return;
    if (d->prj.path.empty()) { d->prj.info.name = ui::U(v); d->prj.dirty = true; request_redraw(2); return; }
    std::wstring oldp = ui::W(d->prj.path);
    size_t sl = oldp.find_last_of(L"\\/");
    std::wstring newp = (sl == std::wstring::npos ? L"" : oldp.substr(0, sl + 1)) + v + (v.size() > 5 && !_wcsicmp(v.c_str() + v.size() - 5, L".pcbp") ? L"" : L".pcbp");
    if (newp == oldp) return;
    if (!MoveFileW(oldp.c_str(), newp.c_str())) { toast("Could not rename the project file (open elsewhere, or the name exists).", ToastKind::Error, 6.f); return; }
    d->prj.path = ui::U(newp);
    settings().remove_mru(oldp); settings().add_mru(newp);
    status("Renamed to " + ui::U(v), ToastKind::Success);
}
static void duplicate_project(const DocPtr& d) {
    Project copy = d->prj;
    DocPtr nd = new_document();
    nd->prj = std::move(copy);
    nd->prj.path.clear();
    nd->prj.info.name.clear();
    nd->prj.dirty = true;
    nd->apply_side_view();
    nd->changed(ui::CH_ALL);
    activate_view(nd, docui_of(d.get()) ? docui_of(d.get())->active : ViewKind::Plotter);
    status("Duplicated as " + doc_tab_name(nd) + " (not saved yet)", ToastKind::Info);
}
// project tab context menu (same actions as document tabs where they apply)
static void project_tab_menu(const DocPtr& d) {
    if (menu_item_icon("Close", ui::Icon::CloseDoc, "Ctrl+W")) defer([d]() { close_document(d); });
    if (menu_item_icon("Close Others", ui::Icon::CloseDoc, nullptr, false, g_docs.size() > 1))
        defer([d]() { std::vector<DocPtr> v; for (auto& u : g_docs) if (u->doc != d) v.push_back(u->doc); for (auto& x : v) if (!close_document(x)) return; });
    ImGui::Separator();
    if (menu_item_icon("Save", ui::Icon::Save, "Ctrl+S")) defer([d]() { save_document(d, false); });
    if (menu_item_icon("Save As\xE2\x80\xA6", ui::Icon::SaveAs)) defer([d]() { save_document(d, true); });
    if (menu_item_icon("Rename\xE2\x80\xA6", ui::Icon::Notes)) defer([d]() { rename_project(d); });
    if (menu_item_icon("Duplicate", ui::Icon::Copy)) defer([d]() { duplicate_project(d); });
    if (!d->prj.path.empty() && menu_item_icon("Copy Path", ui::Icon::Copy)) ImGui::SetClipboardText(d->prj.path.c_str());
}
int tab_placement() { return std::clamp(app::ui_pref("TabPlacement", 0), 0, 2); }   // 0 top, 1 left, 2 right
static void select_project(int i) { g_active = i; g_show_landing = false; files_set_active(false); request_redraw(2); }
static void project_tabs(float width) {
    const bool side = width > 0;
    const bool doc_page = files_active();
    for (size_t i = 0; i < g_docs.size(); ++i) {
        DocPtr d = g_docs[i]->doc;
        const bool act = !g_show_landing && !doc_page && (int)i == g_active;
        const std::string name = doc_tab_name(d);
        const std::string tip = name + (d->prj.path.empty() ? "\n(not saved)" : "\n" + d->prj.path) + (d->prj.dirty ? "\nUnsaved changes" : "") +
                                "\nRight-click for Close / Save / Rename / Duplicate";
        const std::string id = "##ptab" + std::to_string((uintptr_t)d.get());
        TabHit h = tab_item(id.c_str(), name, ui::Icon::Board, act, d->prj.dirty, tip.c_str(), side ? width : 0);
        if (ImGui::BeginPopupContextItem(nullptr)) { project_tab_menu(d); ImGui::EndPopup(); }
        if (h.clicked) select_project((int)i);
        if (h.close) defer([d]() { close_document(d); });
        if (!side) ImGui::SameLine(0, S(4));
    }
}
float g_tab_strip_w = 0;
float tab_strip_width() { return g_tab_strip_w; }
static void draw_tab_strip(float width) {   // width 0: horizontal (top), else vertical strip of that width
    const bool side = width > 0;
    g_tab_strip_w = side ? width - S(12) : 0;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(4), S(4)));
    project_tabs(side ? width - S(12) : 0);
    files_doc_tabs();   // document / spreadsheet / email tabs (sheet_editor.cpp), same tab_item style
    if (side) {
        if (icon_button("##newprj", ui::Icon::New, "New project (Ctrl+N)", false, true, S(28))) post_command(ID_FILE_NEW);
        ImGui::SameLine(0, S(4));
        if (icon_button("##homebtn", ui::Icon::Home, "Home", g_show_landing, true, S(28))) { g_show_landing = true; files_set_active(false); }
    } else {
        if (icon_button("##newprj", ui::Icon::New, "New project (Ctrl+N)", false, true, tab_item_height())) post_command(ID_FILE_NEW);
    }
    ImGui::PopStyleVar();
    // mouse wheel over the strip: previous / next project (vertical) or horizontal scroll (top)
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::GetIO().MouseWheel != 0 && !g_docs.empty()) {
        if (side) {
            int n = (int)g_docs.size();
            int cur = (g_show_landing || files_active()) ? (ImGui::GetIO().MouseWheel < 0 ? -1 : n) : g_active;
            select_project(((cur + (ImGui::GetIO().MouseWheel < 0 ? 1 : -1)) % n + n) % n);
        } else ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * S(60));
    }
}

static void draw_view_tabs(DocUI* u, float h, float W) {
    const GTheme& t = th();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + W, p.y + h), t.panel);
    dl->AddLine(p, ImVec2(p.x + W, p.y), t.border);
    float x = p.x + S(8);
    auto& groups = tab_groups();
    const int gact = tab_group_of(u->active);
    // R19a: every tab gets the same fixed width (widest bold label + icon + padding)
    float tabw = S(96);
    ImGui::PushFont(g_font_bold);
    for (auto& g : groups) tabw = std::max(tabw, ImGui::CalcTextSize(g.name).x + S(46));
    ImGui::PopFont();
    for (int ti = 0; ti < (int)groups.size(); ++ti) {
        const TabGroup& G = groups[ti];
        const int k = (int)G.first;
        const char* name = G.name;
        const ImU32 acc = kTabAccent[k][t.dark ? 0 : 1];
        bool act = gact == ti;
        if (act) ImGui::PushFont(g_font_bold);
        ImVec2 ts = ImGui::CalcTextSize(name);
        if (act) ImGui::PopFont();
        const float w = tabw;
        ImVec2 a(x, p.y + S(4)), b(x + w, p.y + h - S(4));
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(k + 500);
        bool clicked = ImGui::InvisibleButton("vt", ImVec2(w, b.y - a.y));
        bool hov = ImGui::IsItemHovered();
        if (hov && !act) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (hov) { std::string tip = std::string(name) + ": " + G.tip + "\n(Ctrl+Tab / Ctrl+Shift+Tab to cycle)"; tooltip(tip.c_str()); }
        ImGui::PopID();
        const float hv = anim(ImGui::GetID("vtab") + (ImGuiID)k, hov ? 1.f : 0.f, 18.f);
        const float ac = anim(ImGui::GetID("vtabact") + (ImGuiID)k, act ? 1.f : 0.f, 14.f);
        const ImU32 tint = (acc & 0x00FFFFFF) | ((ImU32)(14 + 32 * ac + 16 * hv * (1 - ac)) << 24);
        a.y -= S(1.5f) * hv * (1 - ac); b.y -= S(1.5f) * hv * (1 - ac);
        dl->AddRectFilled(a, b, col_lerp(col_lerp(t.panel, t.raised, hv), t.tab_active, ac), S(7));
        dl->AddRectFilled(a, b, tint, S(7));
        dl->AddRect(a, b, act ? acc : ((acc & 0x00FFFFFF) | (70u << 24)), S(7), 0, act ? S(1.6f) : 1.f);
        // colour pill behind the icon + underline
        const float is = S(18);
        ImVec2 ic(a.x + S(8), a.y + (b.y - a.y - is) * 0.5f);
        dl->AddRectFilled(ImVec2(ic.x - S(3), ic.y - S(3)), ImVec2(ic.x + is + S(3), ic.y + is + S(3)), (acc & 0x00FFFFFF) | ((ImU32)(act ? 110 : 60) << 24), S(5));
        icon_draw(dl, G.members.size() > 1 ? ui::Icon::Check : kViewIcons[k], ic, is, act ? 1.f : 0.85f);
        if (ac > 0.02f) { float cx = (a.x + b.x) * 0.5f, hw = (b.x - a.x - S(16)) * 0.5f * ac; dl->AddRectFilled(ImVec2(cx - hw, b.y - S(4)), ImVec2(cx + hw, b.y - S(1)), acc, S(2)); }
        if (act) ImGui::PushFont(g_font_bold);
        dl->AddText(ImVec2(a.x + S(34) + (w - S(46) - ts.x) * 0.5f, a.y + (b.y - a.y - ts.y) * 0.5f), act ? t.text : t.text_dim, name);
        if (act) ImGui::PopFont();
        if (clicked && !act) {
            ViewKind target = (ViewKind)k;
            if (G.members.size() > 1) { auto it = g_review_last.find(u->doc.get()); if (it != g_review_last.end()) target = it->second; }
            activate_view(u->doc, target);
        }
        x += w + S(5);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
}

// Review sub-tabs (Placements / BOM / Part numbers & DB / Reports) above the view content
static float review_subtabs_h(DocUI* u) { return u && tab_group_of(u->active) == kDataGroup ? S(34) : 0.f; }
static void draw_review_subtabs(DocUI* u, float h, float W) {
    const GTheme& t = th();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + W, p.y + h), t.panel2);
    dl->AddLine(ImVec2(p.x, p.y + h - 1), ImVec2(p.x + W, p.y + h - 1), t.border);
    g_review_last[u->doc.get()] = u->active;
    static const char* tips[] = {"Placements: every physical component instance (reference, X/Y, angle, side, package). Add / edit / duplicate here.",
                                 "BOM: imported bill of materials lines and their match against the placements.",
                                 "Part numbers & packages: part-number assignments (package / feeder data per part number) and the component database.",
                                 "Fiducials: fiducial and bad-mark locations used by the machine.",
                                 "Validation: placement data checks (duplicates, overlaps, missing data). Click a row to select, double-click to fly to it.",
                                 "Reports: printable reports."};
    float x = p.x + S(10);
    int i = 0;
    for (ViewKind k : tab_groups()[kDataGroup].members) {
        const char* name = kViewNames[(int)k];
        bool act = u->active == k;
        if (act) ImGui::PushFont(g_font_bold);
        ImVec2 ts = ImGui::CalcTextSize(name);
        if (act) ImGui::PopFont();
        const float isz = S(15);
        float w = ts.x + S(24) + isz + S(6);   // R13: icon + label
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + S(3)));
        ImGui::PushID((int)k + 700);
        bool clicked = ImGui::InvisibleButton("st", ImVec2(w, h - S(6)));
        bool hov = ImGui::IsItemHovered();
        tooltip(tips[i]);
        ImGui::PopID();
        const ImU32 acc = kTabAccent[(int)k][t.dark ? 0 : 1];
        ImVec2 a(x, p.y + S(3)), b(x + w, p.y + h - S(3));
        // R13 (REVIEW_V12 §7 Data tab): recognisable tabs — icon, raised active tab with accent
        // outline + underline + bold label; inactive tabs keep a faint outline so they read as tabs
        if (act || hov) dl->AddRectFilled(a, b, act ? t.tab_active : t.raised, S(6));
        dl->AddRect(a, b, act ? col_alpha(acc, 0.7f) : col_alpha(t.border, 0.55f), S(6), 0, act ? S(1.5f) : S(1));
        if (act) dl->AddRectFilled(ImVec2(a.x + S(6), b.y - S(3)), ImVec2(b.x - S(6), b.y), acc, S(2));
        icon_draw(dl, kViewIcons[(int)k], ImVec2(a.x + S(10), a.y + (b.y - a.y - isz) * 0.5f), isz, act ? 1.f : 0.6f);
        if (act) ImGui::PushFont(g_font_bold);
        dl->AddText(ImVec2(a.x + S(16) + isz, a.y + (b.y - a.y - ts.y) * 0.5f), act ? t.text : t.text_dim, name);
        if (act) ImGui::PopFont();
        if (clicked && !act) activate_view(u->doc, k);
        x += w + S(4); ++i;
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
}

static void draw_status(float h) {
    const GTheme& t = th();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float W = ImGui::GetWindowWidth();
    dl->AddRectFilled(p, ImVec2(p.x + W, p.y + h), t.statusbar);
    DocUI* u = active_docui();
    View* v = u ? u->current() : nullptr;
    std::string mid = v ? v->status() : std::string();
    if (mid.empty() && u) mid = std::to_string(u->doc->prj.comps.size()) + " components" +
                                (u->doc->selected_count() ? "  \xC2\xB7  " + std::to_string(u->doc->selected_count()) + " selected" : std::string());
    std::string right = v ? v->status_right() : std::string();
    std::string units = ui::U(units_label());
    float ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    {
        float x = p.x + S(12);
        float age = (float)(ImGui::GetTime() - g_status_time);
        if (g_status_kind >= 0) {
            ToastKind k = (ToastKind)g_status_kind;
            float fl = std::max(0.f, 1.f - age / 1.2f);
            if (fl > 0) { dl->AddRectFilled(p, ImVec2(p.x + W * 0.5f, p.y + h), col_alpha(toast_color(k), 0.45f * fl)); request_redraw(2); }
            float is = h - S(8);
            icon_draw(dl, toast_icon(k), ImVec2(x, p.y + S(4)), is, 1.f, true);
            x += is + S(8);
        }
        float in = std::min(1.f, age / 0.2f);
        if (in < 1) request_redraw(2);
        dl->AddText(ImVec2(x - (1 - in) * S(8), ty), col_alpha(t.statusbar_text, 0.4f + 0.6f * in), g_status.c_str());
    }
    float xr = p.x + W - S(12);
    ImVec2 us = ImGui::CalcTextSize(units.c_str());
    xr -= us.x;
    dl->AddText(ImVec2(xr, ty), t.statusbar_text, units.c_str());
    xr -= S(22);
    if (!right.empty()) {
        ImGui::PushFont(g_font_mono);
        ImVec2 rs = ImGui::CalcTextSize(right.c_str());
        xr -= rs.x;
        dl->AddText(ImVec2(xr, ty), t.statusbar_text, right.c_str());
        ImGui::PopFont();
        xr -= S(22);
    }
    if (!mid.empty()) {
        ImVec2 ms = ImGui::CalcTextSize(mid.c_str());
        xr -= ms.x;
        dl->AddText(ImVec2(xr, ty), (t.statusbar_text & 0x00FFFFFF) | 0xD8000000, mid.c_str());
    }
    ImGui::Dummy(ImVec2(W, h));
}

static void draw_main() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, th().bg);
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), S(6)));
    draw_menu_bar();
    ImGui::PopStyleVar();
    // toolbar strip
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float W = ImGui::GetWindowWidth();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + W, p.y + S(38)), th().panel);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + S(38)), ImVec2(p.x + W, p.y + S(38)), th().border);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(4)));
        draw_toolbar();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(39)));
    }
    const float status_h = settings().status_bar ? S(26) : 0;
    const bool any_tabs = !g_docs.empty() || files_any();
    const int place = any_tabs ? tab_placement() : 0;
    DocUI* u = active_docui();
    if (any_tabs && place == 0) {
        ImGui::SetCursorPosX(S(6));
        ImGui::BeginChild("##tabstrip", ImVec2(ImGui::GetContentRegionAvail().x - S(6), tab_item_height() + S(8)), 0,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::SetCursorPos(ImVec2(0, S(4)));
        draw_tab_strip(0);
        ImGui::EndChild();
        u = active_docui();
    }
    if (any_tabs && g_docs.empty() && !files_any()) g_show_landing = true;
    const float tabs_h = u ? S(40) : 0;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 content(avail.x, avail.y - tabs_h - status_h);
    const float side_w = place ? S(210) : 0;
    const ImVec2 area0 = ImGui::GetCursorScreenPos();
    if (place) {   // vertical project strip at the left / right edge
        ImGui::SetCursorScreenPos(ImVec2(place == 1 ? area0.x : area0.x + avail.x - side_w, area0.y));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, th().panel);
        ImGui::BeginChild("##tabside", ImVec2(side_w, avail.y - status_h), 0, ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetCursorPos(ImVec2(S(6), S(6)));
        ImGui::BeginGroup();
        draw_tab_strip(side_w);
        ImGui::EndGroup();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        u = active_docui();
        ImGui::SetCursorScreenPos(ImVec2(place == 1 ? area0.x + side_w : area0.x, area0.y));
        content.x -= side_w;
    }
    const float x0 = ImGui::GetCursorScreenPos().x;
    if (files_active()) {
        ImGui::BeginChild("##files_ws", content, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        files_draw(ImGui::GetContentRegionAvail());
        ImGui::EndChild();
    } else if (!u) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, th().bg);
        ImGui::BeginChild("##landing", content, 0, 0);
        draw_landing(ImGui::GetContentRegionAvail());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else {
        View* v = u->current();
        if (float sh = review_subtabs_h(u)) { ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y)); draw_review_subtabs(u, sh, content.x); content.y -= sh; }
        ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, th().bg);
        ImGui::BeginChild("##view", content, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (v) v->draw(ImGui::GetContentRegionAvail());
        else ImGui::TextDisabled("This view is not available.");
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
        draw_view_tabs(u, tabs_h, content.x);
    }
    if (status_h > 0) {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - status_h));
        draw_status(status_h);
    }
    draw_about();
    ImGui::End();
}

// ===========================================================================
// factory model panel
// ===========================================================================
static void delete_layers(const app::DocPtr& d, const std::set<int>& del) {
    if (del.empty()) return;
    auto& pts = d->prj.model.points;
    int members = 0;
    for (auto& c : d->prj.comps) if (del.count(c.assigned_point)) ++members;
    std::string names;
    int k = 0;
    for (int i : del) { if (k++ < 6) names += (names.empty() ? "" : ", ") + pts[i].name; }
    if (del.size() > 6) names += ", ...";
    if (!confirm(del.size() == 1 ? "Delete Export Layer" : "Delete Export Layers",
                 "Delete " + std::to_string(del.size()) + " export layer(s): " + names + "?\n" + std::to_string(members) +
                 " placement(s) become unassigned (the placements themselves are kept).\nCtrl+Z undoes this.", "Delete", true)) return;
    d->checkpoint("Delete " + std::to_string(del.size()) + " export layer(s)");
    std::vector<int> remap(pts.size(), -1);
    std::vector<ModelPoint> keep;
    for (int i = 0; i < (int)pts.size(); ++i) if (!del.count(i)) { remap[i] = (int)keep.size(); keep.push_back(pts[i]); }
    const int old_active = d->active_point;
    pts = std::move(keep);
    for (auto& c : d->prj.comps) c.assigned_point = c.assigned_point >= 0 && c.assigned_point < (int)remap.size() ? remap[c.assigned_point] : -1;
    int na = old_active >= 0 && old_active < (int)remap.size() ? remap[old_active] : -1;
    d->active_point = pts.empty() ? -1 : (na >= 0 ? na : 0);
    g_layer_sel[d.get()].clear();
    d->changed(ui::CH_MODEL);
    toast(std::to_string(del.size()) + " export layer(s) deleted", ToastKind::Success, 5.f, "Undo", []() { post_command(ID_EDIT_UNDO); });
}

void draw_factory_panel(const app::DocPtr& d, float width) {
    const GTheme& t = th();
    ImGui::BeginChild("##factory", ImVec2(width, 0), ImGuiChildFlags_Borders, 0);
    section_header("EXPORT LAYERS");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - S(14));
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        tooltip("Export layers group placements into machine programs (one program per layer).\n"
                "\xE2\x80\xA2 Every placement belongs to at most ONE layer (its colour on the board).\n"
                "\xE2\x80\xA2 Move to layer puts the SELECTED, VISIBLE placements into the current layer; other layers keep theirs.\n"
                "\xE2\x80\xA2 Click selects a layer, Ctrl+click / Shift+click select several (batch delete), double-click opens Export.\n"
                "\xE2\x80\xA2 Right-click a layer for rename / delete / reorder / select members.\n"
                "\xE2\x80\xA2 Plot: right-click > Assign to Layer (or the toolbar button) assigns the selection directly.");
    const int np = (int)d->prj.model.points.size();
    std::vector<int> counts(np, 0), ctop(np, 0), csel(np, 0);
    std::vector<std::vector<std::string>> sample(np);
    int unassigned = 0;
    for (auto& c : d->prj.comps) {
        if (c.assigned_point >= 0 && c.assigned_point < np) {
            counts[c.assigned_point]++;
            if (c.side == Side::Top) ctop[c.assigned_point]++;
            if (c.selected) csel[c.assigned_point]++;
            auto& sm = sample[c.assigned_point];
            if (sm.size() < 8 && !c.ref.empty()) sm.push_back(c.ref);
        } else if (c.feature == Feature::Component && !c.dnp) unassigned++;
    }
    int cur = current_point(*d);
    auto& msel = g_layer_sel[d.get()];
    for (auto it = msel.begin(); it != msel.end();) it = *it >= np ? msel.erase(it) : std::next(it);
    static int s_anchor = -1;
    static int s_rename = -1;
    static char s_name[128];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < np; ++i) {
        const ModelPoint& mp = d->prj.model.points[i];
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = S(34);
        const bool in_sel = msel.count(i) || (msel.empty() && i == cur);
        if (s_rename == i) {
            ImGui::SetNextItemWidth(w);
            ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("##rn", s_name, sizeof s_name, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                if (s_name[0] && mp.name != s_name) {
                    d->checkpoint("Rename export layer");
                    const std::string old = mp.name;
                    d->prj.model.points[i].name = s_name;
                    for (auto& sd : d->prj.mfg.sides) if (sd.point == old) sd.point = s_name;   // keep side mapping
                    d->changed(ui::CH_MODEL);
                }
                s_rename = -1;
            } else if (ImGui::IsItemDeactivated() || ImGui::IsKeyPressed(ImGuiKey_Escape)) s_rename = -1;
            ImGui::PopID();
            continue;
        }
        ImGui::SetNextItemAllowOverlap();
        bool clicked = ImGui::InvisibleButton("pt", ImVec2(w, h));
        bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) post_command(ID_POINT_OPEN);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            tooltip((mp.name + ": " + std::to_string(counts[i]) + " placement(s)" + (sample[i].empty() ? std::string() : "\n" + ref_list_capped(sample[i], (size_t)counts[i])) +
                     "\nDouble-click: open in Export \xC2\xB7 F2 / right-click: rename").c_str());
        if (hov && ImGui::IsKeyPressed(ImGuiKey_F2)) { s_rename = i; std::snprintf(s_name, sizeof s_name, "%s", mp.name.c_str()); }
        if (i == cur || in_sel) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), i == cur ? t.sel : col_alpha(t.sel, 0.55f), S(6));
        else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), t.raised, S(6));
        ImU32 col = IM_COL32((mp.color >> 16) & 255, (mp.color >> 8) & 255, mp.color & 255, 255);
        dl->AddCircleFilled(ImVec2(p.x + S(12), p.y + h * 0.5f), S(5.5f), col);
        float lh = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(p.x + S(24), p.y + S(2)), i == cur ? t.sel_text : t.text, mp.name.c_str());
        std::string sub = "T " + std::to_string(ctop[i]) + "  \xC2\xB7  B " + std::to_string(counts[i] - ctop[i]);
        if (csel[i]) sub += "  \xC2\xB7  " + std::to_string(csel[i]) + " selected";
        dl->AddText(g_font, ImGui::GetFontSize() * 0.86f, ImVec2(p.x + S(24), p.y + S(4) + lh), i == cur ? t.sel_text : t.text_faint, sub.c_str());
        if (mp.has_machine) icon_draw(dl, ui::Icon::Machine, ImVec2(p.x + w - S(88), p.y + S(9)), S(16), 0.8f);
        std::string cnt = std::to_string(counts[i]);
        ImVec2 cs = ImGui::CalcTextSize(cnt.c_str());
        ImVec2 ba(p.x + w - S(40), p.y + S(8)), bb(p.x + w - S(6), p.y + h - S(8));
        dl->AddRectFilled(ba, bb, counts[i] ? ((col & 0x00FFFFFF) | 0x50000000) : t.raised, S(8));
        dl->AddText(ImVec2((ba.x + bb.x - cs.x) * 0.5f, (ba.y + bb.y - cs.y) * 0.5f), t.text, cnt.c_str());
        if (hov || i == cur) {
            ImVec2 keep = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(p.x + w - S(66), p.y + (h - S(22)) * 0.5f));
            if (icon_button("selm", ui::Icon::Select, "Select the placements in this layer\nShift: add to the current selection", false, counts[i] > 0, S(22))) {
                set_current_point(*d, i);
                post_command(ID_SEL_POINT_MEMBERS);
            }
            ImGui::SetCursorScreenPos(keep);
        }
        if (clicked) {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl) { if (msel.empty() && cur >= 0) msel.insert(cur); if (!msel.erase(i)) msel.insert(i); }
            else if (io.KeyShift && s_anchor >= 0) { msel.clear(); for (int k = std::min(s_anchor, i); k <= std::max(s_anchor, i); ++k) msel.insert(k); }
            else { msel.clear(); s_anchor = i; }
            set_current_point(*d, i);
        }
        if (ImGui::BeginPopupContextItem("ptctx")) {
            if (!msel.count(i)) { msel.clear(); set_current_point(*d, i); }
            const int nsel = std::max<int>(1, (int)msel.size());
            menu_item(ID_SEL_POINT_MEMBERS);
            menu_item(ID_TOOL_ASSIGN);
            ImGui::Separator();
            if (menu_item_icon("Rename\tF2", ui::Icon::Text)) { s_rename = i; std::snprintf(s_name, sizeof s_name, "%s", mp.name.c_str()); }
            for (int id : {ID_POINT_PROPERTIES, ID_POINT_OPEN, ID_POINT_GENERATE}) menu_item(id);
            ImGui::Separator();
            for (int id : {ID_POINT_UP, ID_POINT_DOWN}) menu_item(id);
            std::string dl_ = nsel > 1 ? "Delete " + std::to_string(nsel) + " Layers..." : std::string("Delete Layer...");
            if (menu_item_icon(dl_.c_str(), ui::Icon::Delete)) { auto ds = layer_selection(*d); defer([d, ds]() { delete_layers(d, ds); }); }
            ImGui::Separator();
            menu_item(ID_POINT_ADD);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Delete) && np > 0 && s_rename < 0) {
        auto ds = layer_selection(*d); defer([d, ds]() { delete_layers(d, ds); });
    }
    ImGui::TextDisabled("%d unassigned  \xC2\xB7  %d selected", unassigned, d->selected_count());
    ImGui::Spacing();
    float bw = (ImGui::GetContentRegionAvail().x - S(6)) * 0.5f;
    std::string al = cur >= 0 ? "Move \xE2\x86\x92 " + d->prj.model.points[cur].name : std::string("Move to layer");
    if (ImGui::CalcTextSize(al.c_str()).x + S(40) > bw) al = "Move to layer";
    if (button(al.c_str(), ui::Icon::Assign, true, ImVec2(bw, 0), command_state(ID_TOOL_ASSIGN) & 1)) post_command(ID_TOOL_ASSIGN);
    if (ImGui::IsItemHovered()) tooltip((std::string("Move the selected placements on the visible side(s) into ") + (cur >= 0 ? d->prj.model.points[cur].name : std::string("the current layer")) + ".\nOther layers keep their members.").c_str());
    ImGui::SameLine(0, S(6));
    if (button("Remove", ui::Icon::Unassign, false, ImVec2(bw, 0), command_state(ID_TOOL_UNASSIGN) & 1)) post_command(ID_TOOL_UNASSIGN);
    tooltip("Remove the selected placements from their export layer (they stay in the project)");
    if (button("Select members", ui::Icon::Select, false, ImVec2(bw, 0), command_state(ID_SEL_POINT_MEMBERS) & 1)) post_command(ID_SEL_POINT_MEMBERS);
    tooltip("Select every placement in the current layer");
    ImGui::SameLine(0, S(6));
    if (button("Select unassigned", ui::Icon::ShowUnassigned, false, ImVec2(bw, 0), unassigned > 0)) post_command(ID_SEL_UNASSIGNED);
    tooltip("Select placements that are in no export layer");
    if (button("New layer", ui::Icon::Plus, false, ImVec2(bw, 0))) post_command(ID_POINT_ADD);
    tooltip("Add an export layer");
    ImGui::SameLine(0, S(6));
    {
        const int nsel = (int)layer_selection(*d).size();
        std::string lbl = nsel > 1 ? "Delete " + std::to_string(nsel) : std::string("Delete");
        if (button(lbl.c_str(), ui::Icon::Delete, false, ImVec2(bw, 0), np > 0)) { auto ds = layer_selection(*d); defer([d, ds]() { delete_layers(d, ds); }); }
        tooltip("Delete the selected export layer(s) after confirmation (Ctrl+Z undoes). Placements are kept, only unassigned.");
    }
    if (button("Keep only SMT layers...", ui::Icon::Eraser, false, ImVec2(-1, 0), np > 0)) {
        std::set<int> del;
        for (int i = 0; i < np; ++i) {
            const ModelPoint& mp = d->prj.model.points[i];
            if (mp.kind != PointKind::Placement || (!mp.has_machine && counts[i] == 0) || (counts[i] == 0 && i > 1)) del.insert(i);
        }
        if (del.empty()) toast("Only placement layers are left", ToastKind::Info);
        else defer([d, del]() { delete_layers(d, del); });
    }
    tooltip("Delete process / inspection / manual layers and empty extra layers in one step (confirmation + undo).\nUseful after imports that created many layers.");
    if (button("Generate Program", ui::Icon::Generate, false, ImVec2(-1, 0), command_state(ID_POINT_GENERATE) & 1)) post_command(ID_POINT_GENERATE);
    tooltip("Generate the machine program for the current layer (Export page shows destination and status)");
    if (button("Layer details...", ui::Icon::FactoryModel, false, ImVec2(-1, 0))) post_command(ID_LIB_FACTORY_MODEL);
    tooltip("Edit all export layers: names, colours, filters, machine interface");
    ImGui::EndChild();
}

// ===========================================================================
// side mode
// ===========================================================================
bool side_xray(const app::Doc& d) { return d.side_display == app::Doc::SideDisplay::Xray; }
bool view_bottom(const app::Doc& d) { return d.view_from_bottom; }
static void side_status(const app::Doc& d) {
    using SD = app::Doc::SideDisplay;
    const char* m = d.side_display == SD::Normal ? "Normal: only this side shown"
                  : d.side_display == SD::Ghost ? "Ghost: other side transparent, not selectable"
                  : d.back_selectable ? "X-ray: both sides shown, other side SELECTABLE" : "X-ray: both sides shown, other side locked";
    status(std::string(d.view_from_bottom ? "Viewing from BOTTOM (board turned over)" : "Viewing from TOP") + " \xC2\xB7 " + m);
}
void set_view_bottom(app::Doc& d, bool bottom) {
    g_view_bottom[&d] = bottom;
    d.view_from_bottom = bottom;     // flipping the view makes the facing side active
    d.apply_side_view();
    d.changed(ui::CH_SELECTION);
    side_status(d);
}
void set_side_display(app::Doc& d, int mode) {
    d.side_display = (app::Doc::SideDisplay)std::clamp(mode, 0, 2);
    d.apply_side_view();
    d.changed(ui::CH_SELECTION);
    side_status(d);
}
void set_back_selectable(app::Doc& d, bool on) {
    d.back_selectable = on;
    if (on && d.side_display != app::Doc::SideDisplay::Xray) d.side_display = app::Doc::SideDisplay::Xray;
    d.apply_side_view();
    d.changed(ui::CH_SELECTION);
    side_status(d);
}
void set_xray(app::Doc& d, bool on) { set_side_display(d, on ? 2 : 1); }
// legacy 3-way API: 0 top only, 1 bottom only, 2 x-ray
int side_mode(const app::Doc& d) { return side_xray(d) ? 2 : (d.view_from_bottom ? 1 : 0); }
void set_side_mode(app::Doc& d, int m) {
    if (m == 2) set_xray(d, true);
    else { d.view_from_bottom = m == 1; set_xray(d, false); }
}

// ===========================================================================
// selection sets
// ===========================================================================
void selset_save(const app::DocPtr& d) {
    if (!d) return;
    std::vector<std::string> refs;
    int noref = 0;
    for (auto& c : d->prj.comps) if (c.selected) { if (c.ref.empty()) noref++; else refs.push_back(c.ref); }
    if (refs.empty()) { status("Select components (with references) first"); return; }
    defer([d, refs, noref]() {
        std::wstring name = L"Set " + std::to_wstring(d->prj.selection_sets.size() + 1);
        if (!ui::prompt_text(g_hwnd, L"Save Selection Set", L"Name for the " + std::to_wstring(refs.size()) + L" selected components:", name)) return;
        std::string n = ui::U(name);
        if (n.empty()) return;
        auto& v = d->prj.selection_sets;
        auto it = std::find_if(v.begin(), v.end(), [&](const SelectionSet& s) { return s.name == n; });
        if (it != v.end()) it->refs = refs;
        else v.push_back({n, refs});
        d->changed(ui::CH_INFO);
        status("Selection set '" + n + "' saved (" + std::to_string(refs.size()) + " components" + (noref ? ", " + std::to_string(noref) + " without reference skipped)" : ")"));
    });
}

void selset_apply(app::Doc& d, int idx, int mode) {
    if (idx < 0 || idx >= (int)d.prj.selection_sets.size()) return;
    const SelectionSet& ss = d.prj.selection_sets[idx];
    std::set<std::string> want(ss.refs.begin(), ss.refs.end());
    if (mode == 0) d.select_none();
    int n = 0;
    for (auto& c : d.prj.comps) if (!c.ref.empty() && want.count(c.ref)) { c.selected = mode != 2; n++; }
    d.changed(ui::CH_SELECTION);
    status(std::string(mode == 0 ? "Selected " : mode == 1 ? "Added " : "Removed ") + std::to_string(n) + " components of '" + ss.name + "'" +
           (n < (int)ss.refs.size() ? "  (" + std::to_string(ss.refs.size() - n) + " references not found)" : std::string()));
}

bool selsets_visible(const app::Doc& d) { return !d.prj.selection_sets.empty(); }
void move_to_layer_menu_items(const app::DocPtr& d) {
    if (!d) return;
    const int nsel = d->selected_count();
    auto& pts = d->prj.model.points;
    for (int i = 0; i < (int)pts.size(); ++i) {
        ImGui::PushID(i);
        std::string l = pts[i].name.empty() ? "Layer " + std::to_string(i + 1) : pts[i].name;
        if (menu_item_icon(l.c_str(), ui::Icon::Assign, nullptr, false, nsel > 0)) { set_current_point(*d, i); post_command(ID_TOOL_ASSIGN); }
        ImGui::PopID();
    }
    if (!pts.empty()) ImGui::Separator();
    if (menu_item_icon("New Export Layer from Selection\xE2\x80\xA6", ui::Icon::New, nullptr, false, nsel > 0)) {
        const size_t before = pts.size();
        post_command(ID_POINT_ADD);   // layer dialog (name, colour, filters)
        defer([d, before]() {
            auto& p2 = d->prj.model.points;
            if (p2.size() <= before) return;   // cancelled
            set_current_point(*d, (int)p2.size() - 1);
            post_command(ID_TOOL_ASSIGN);
        });
    }
    if (menu_item_icon("Remove from Export Layer", ui::Icon::Unassign, nullptr, false, nsel > 0)) post_command(ID_TOOL_UNASSIGN);
}

void selset_menu_items(const app::DocPtr& d) {
    if (!d) return;
    menu_item(ID_SEL_SET_SAVE);
    auto& v = d->prj.selection_sets;
    if (v.empty()) { ImGui::TextDisabled("No selection sets yet"); return; }
    ImGui::Separator();
    const bool shift = ImGui::GetIO().KeyShift, ctrl = ImGui::GetIO().KeyCtrl;
    for (int i = 0; i < (int)v.size(); ++i) {
        ImGui::PushID(i);
        std::string label = v[i].name + "  (" + std::to_string(v[i].refs.size()) + ")";
        bool open = ImGui::BeginMenu(label.c_str());
        // quick recall: click the submenu header (Shift adds, Ctrl subtracts)
        if (ImGui::IsItemClicked()) selset_apply(*d, i, ctrl ? 2 : shift ? 1 : 0);
        if (open) {
            if (ImGui::MenuItem("Select (replace)")) selset_apply(*d, i, 0);
            if (ImGui::MenuItem("Add to selection")) selset_apply(*d, i, 1);
            if (ImGui::MenuItem("Remove from selection")) selset_apply(*d, i, 2);
            ImGui::Separator();
            if (ImGui::MenuItem("Update from current selection", nullptr, false, d->selected_count() > 0)) {
                v[i].refs.clear();
                for (auto& c : d->prj.comps) if (c.selected && !c.ref.empty()) v[i].refs.push_back(c.ref);
                d->changed(ui::CH_INFO);
            }
            if (ImGui::MenuItem("Rename...")) {
                defer([d, i]() {
                    if (i >= (int)d->prj.selection_sets.size()) return;
                    std::wstring name = ui::W(d->prj.selection_sets[i].name);
                    if (ui::prompt_text(g_hwnd, L"Rename Selection Set", L"New name:", name) && !name.empty()) {
                        d->prj.selection_sets[i].name = ui::U(name);
                        d->changed(ui::CH_INFO);
                    }
                });
            }
            if (ImGui::MenuItem("Delete")) {
                std::string n = v[i].name;
                v.erase(v.begin() + i);
                d->changed(ui::CH_INFO);
                status("Selection set '" + n + "' deleted");
                ImGui::EndMenu();
                ImGui::PopID();
                break;
            }
            ImGui::EndMenu();
        }
        ImGui::PopID();
    }
}

// ===========================================================================
// About (animated)
// ===========================================================================
namespace {
bool g_about_req = false;
uint32_t g_about_seed = 1;
}
void show_about() { g_about_req = true; g_about_seed = (uint32_t)GetTickCount() * 2654435761u + 1; request_redraw(3); }

static void draw_about() {
    static bool s_about_open = false;
    if (g_about_req) { ImGui::OpenPopup("##about"); g_about_req = false; s_about_open = true; about_audio_start(g_about_seed); }
    const GTheme& t = th();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float W = std::min(S(780), vp->Size.x - S(40)), FXH = std::min(S(340), vp->Size.y * 0.45f);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(W, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(12));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, t.panel);
    if (ImGui::BeginPopupModal("##about", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + W, a.y + FXH);
        dl->PushClipRect(a, b, true);
        draw_circuit_fx(dl, a, b, 1.f, g_about_seed, 1);
        // vignette + title plate
        ImU32 base = t.dark ? IM_COL32(4, 10, 10, 0) : IM_COL32(236, 244, 240, 0);
        ImU32 dark = (base & 0x00FFFFFF) | 0xC8000000;
        dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - FXH * 0.55f), b, base, base, dark, dark);
        float L = S(64);
        ImVec2 lp(a.x + S(28), b.y - L - S(30));
        draw_logo(dl, lp, L);
        ImGui::PushFont(g_font_big);
        const char* title = "PCB Plotter";
        ImVec2 tp(lp.x + L + S(16), lp.y + S(2));
        float gpulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 2.2f);
        ImU32 glow = t.dark ? IM_COL32(60, 230, 255, (int)(18 + 22 * gpulse)) : IM_COL32(0, 120, 140, (int)(14 + 16 * gpulse));
        for (int k = 0; k < 8; ++k) {   // soft glow ring behind the title
            float an = k * 3.14159265f / 4, r = S(1.5f);
            dl->AddText(ImVec2(tp.x + std::cos(an) * r, tp.y + std::sin(an) * r), glow, title);
        }
        dl->AddText(tp, t.dark ? IM_COL32(240, 252, 255, 255) : IM_COL32(10, 30, 30, 255), title);
        ImGui::PopFont();
        dl->AddText(ImVec2(tp.x + S(2), tp.y + S(46)), t.dark ? IM_COL32(150, 220, 210, 255) : IM_COL32(30, 90, 80, 255),
                    "Version 1.0  \xC2\xB7  SMT CAM for pick-and-place programming");
        dl->PopClipRect();
        ImGui::Dummy(ImVec2(W, FXH));
        ImGui::SetCursorPos(ImVec2(S(28), FXH + S(16)));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(W - S(28));
        ImGui::TextUnformatted("Gerber / CAD / centroid / BOM import, factory model and model points, visual aids and pick-and-place program generation.");
        ImGui::Spacing();
        ImGui::TextDisabled("Independent software. Not affiliated with any CAM software or equipment vendor; format names are used only to identify file formats.");
        ImGui::TextDisabled("Dear ImGui + Direct3D 11 + Direct2D. No other external libraries.");
        ImGui::TextDisabled("Plugins: %d importers, %d exporters.", (int)PluginRegistry::get().importers().size(), (int)PluginRegistry::get().exporters().size());
        ImGui::Spacing();
        ImGui::TextDisabled("Author: Corey Nguyen \xE2\x80\x94 design, development, shop-floor testing.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Spacing();
        if (button("New circuit", ui::Icon::Board, false)) g_about_seed = g_about_seed * 1664525u + 1013904223u;
        ImGui::SameLine(W - S(56) - S(110));
        bool close = button("Close", ui::Icon::None, true, ImVec2(S(110), 0));
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(W, S(20)));
        if (close || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
            (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_ChildWindows)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (s_about_open && !ImGui::IsPopupOpen("##about")) { s_about_open = false; std::thread([]() { about_audio_stop(); }).detach(); }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// ===========================================================================
// frame loop
// ===========================================================================
// ---------------------------------------------------------------------------
// R10 §2 performance instrumentation: F12 overlay + optional log (PCBP_PERF_LOG=<file> or overlay on).
// Measures CPU time per frame (build + submit, excluding the vsync wait), presented frames,
// frames per second, process CPU % over the last window, and whether the loop is idle.
// ---------------------------------------------------------------------------
namespace {
struct PerfStats {
    bool overlay = false;
    long long frames = 0;              // total frames rendered
    double last_ms = 0, avg_ms = 0, max_ms = 0, present_ms = 0;
    double win_t0 = 0; long long win_frames = 0; double win_ms_sum = 0, win_ms_max = 0;
    ULONGLONG cpu0 = 0; double fps = 0, cpu_pct = 0, wall0 = 0;
    double rep_avg = 0, rep_max = 0;   // last completed window
    std::string adapter; bool software = false; bool suggested = false;
    FILE* log = nullptr; bool log_checked = false;
    double first_t = 0;
} g_perf_st;
std::map<unsigned, int> g_perf_msgs;     // message id -> count in the current 1 s window (PCBP_PERF_LOG)
double now_s() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; }
ULONGLONG proc_cpu_100ns() { FILETIME a, b, k, u; GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u); return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime); }
void perf_detect_adapter() {
    auto& P = g_perf_st;
    if (!P.adapter.empty()) return;
    P.adapter = "unknown";
    if (ID3D11Device* dev = gfx_device()) {
        IDXGIDevice* dx = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx)) && dx) {
            IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(dx->GetAdapter(&ad)) && ad) {
                DXGI_ADAPTER_DESC d{};
                if (SUCCEEDED(ad->GetDesc(&d))) {
                    P.adapter = ui::U(d.Description);
                    // WARP / Basic Render Driver (VendorId 0x1414 DeviceId 0x8c) / llvmpipe-style software rasterisers
                    std::string l = P.adapter; for (auto& c : l) c = (char)std::tolower((unsigned char)c);
                    P.software = (d.VendorId == 0x1414 && d.DeviceId == 0x8c) || l.find("basic render") != std::string::npos ||
                                 l.find("warp") != std::string::npos || l.find("llvmpipe") != std::string::npos || l.find("software") != std::string::npos;
                }
                ad->Release();
            }
            dx->Release();
        }
    }
}
void perf_log_line(const char* tag) {
    auto& P = g_perf_st;
    if (!P.log_checked) {
        P.log_checked = true;
        if (const wchar_t* lp = _wgetenv(L"PCBP_PERF_LOG")) P.log = _wfopen(lp, L"a");
    }
    if (!P.log) return;
    std::fprintf(P.log, "%s t=%.1fs frames=%lld fps=%.1f avg_ms=%.2f max_ms=%.2f cpu=%.1f%% adapter=\"%s\" sw=%d perf_mode=%d ui=%.0f%%\n",
                 tag, now_s() - P.first_t, P.frames, P.fps, P.rep_avg, P.rep_max, P.cpu_pct, P.adapter.c_str(), (int)P.software, (int)perf_mode(), ui_scale() * 100);
    std::fflush(P.log);
}
// once per second: roll the window; logs; auto-suggest performance mode (once, never forced)
void perf_tick() {
    auto& P = g_perf_st;
    const double t = now_s();
    if (P.first_t == 0) { P.first_t = t; P.win_t0 = P.wall0 = t; P.cpu0 = proc_cpu_100ns(); }
    if (t - P.win_t0 < 1.0) return;
    const ULONGLONG c = proc_cpu_100ns();
    P.cpu_pct = (double)(c - P.cpu0) / 1e7 / (t - P.wall0) * 100.0;
    P.fps = P.win_frames / (t - P.win_t0);
    P.rep_avg = P.win_frames ? P.win_ms_sum / P.win_frames : 0;
    P.rep_max = P.win_ms_max;
    P.cpu0 = c; P.wall0 = t; P.win_t0 = t; P.win_frames = 0; P.win_ms_sum = 0; P.win_ms_max = 0;
    perf_log_line(P.fps < 0.5 ? "idle" : "busy");
    if (P.log && !g_perf_msgs.empty()) {
        std::fprintf(P.log, "  msgs:");
        for (auto& kv : g_perf_msgs) std::fprintf(P.log, " %04X x%d", kv.first, kv.second);
        std::fprintf(P.log, "\n");
        g_perf_msgs.clear();
    }
    auto& st = settings();
    if (!P.suggested && st.perf_mode == 0 && !st.perf_suggest_seen && t - P.first_t > 4 && !perf_mode() &&
        (P.software || P.rep_avg > 30.0)) {
        P.suggested = true;
        st.perf_suggest_seen = true; st.save();
        toast(P.software ? "Software graphics adapter detected (" + P.adapter + "). Performance mode turns off animations and fades."
                         : "Frames are slow on this system. Performance mode turns off animations and fades.",
              ToastKind::Info, 14.f, "Enable", []() { auto& s2 = settings(); s2.perf_mode = 2; s2.save(); set_perf_mode(true); });
    }
}
// which messages woke the loop (logged with PCBP_PERF_LOG; finds idle redraw sources)
void perf_note_msg(unsigned id) { if (g_perf_st.log) g_perf_msgs[id]++; }
// MsgWaitFor timeout path: keep the 1 s window rolling while idle without rendering
} // namespace
bool perf_overlay_on() { return g_perf_st.overlay; }
void perf_overlay_toggle() { g_perf_st.overlay = !g_perf_st.overlay; request_redraw(2); }
static void draw_perf_overlay() {
    auto& P = g_perf_st;
    if (!P.overlay) return;
    const GTheme& t = th();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - S(12), vp->WorkPos.y + S(64)), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::Begin("##perf", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    ImGui::PushFont(g_font_mono);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.accent), "Performance (F12)");
    ImGui::Text("frame CPU  %6.2f ms  (avg %.2f, max %.2f)", P.last_ms, P.rep_avg, P.rep_max);
    ImGui::Text("present    %6.2f ms", P.present_ms);
    ImGui::Text("redraws    %6.1f /s   total %lld", P.fps, P.frames);
    ImGui::Text("process    %6.1f %% CPU", P.cpu_pct);
    ImGui::Text("adapter    %s%s", P.adapter.c_str(), P.software ? "  [software]" : "");
    ImGui::Text("scale      %.0f%% UI x %.0f%% DPI, icons %.0f%%", ui_scale() * 100, monitor_dpi_scale() * 100, icon_scale() * 100);
    ImGui::Text("perf mode  %s", perf_mode() ? "on" : "off");
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "Overlay itself redraws once per second.");
    ImGui::PopFont();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// P1-01 ID-conflict check. PCBP_IDCHECK=<log>: every frame, every interactive item id (ItemHoverable, patched
// imgui.cpp hook) is recorded; an id submitted twice in one frame is a conflict (the "2 visible items with
// conflicting ID" error, found without having to hover it). PCBP_IDWALK=1 additionally walks every view of the
// open project (and Home) and exits with the number of distinct conflicts as the process exit code (tests/idwalk.sh).
// ---------------------------------------------------------------------------
} // namespace pcbp::gui
extern void (*GImGuiIdProbe)(ImGuiID id, const ImRect& bb);
namespace pcbp::gui {
using namespace pcbp::app;
namespace {
struct IdSeen { const ImGuiWindow* win; ImRect bb; };
std::unordered_map<ImGuiID, IdSeen> g_id_frame;
std::set<std::string> g_id_conflicts;
FILE* g_id_log = nullptr;
std::string g_id_where;   // walk step label
int g_walk_step = -1, g_walk_wait = 0;
void id_probe(ImGuiID id, const ImRect& bb) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    auto it = g_id_frame.find(id);
    if (it == g_id_frame.end()) { g_id_frame.emplace(id, IdSeen{w, bb}); return; }
    if (it->second.win == w && it->second.bb.Min.x == bb.Min.x && it->second.bb.Min.y == bb.Min.y && it->second.bb.Max.x == bb.Max.x) return;   // same item, two behaviours
    char b[512];
    std::snprintf(b, sizeof b, "id %08X  window \"%s\"  at (%.0f,%.0f)-(%.0f,%.0f) and \"%s\" (%.0f,%.0f)  [%s]", id, w ? w->Name : "?", bb.Min.x, bb.Min.y, bb.Max.x, bb.Max.y,
                  it->second.win ? it->second.win->Name : "?", it->second.bb.Min.x, it->second.bb.Min.y, g_id_where.c_str());
    std::string key = std::string(w ? w->Name : "?") + "#" + std::to_string(id);
    if (g_id_conflicts.insert(key).second && g_id_log) { std::fprintf(g_id_log, "%s\n", b); std::fflush(g_id_log); }
}
} // namespace
int idcheck_conflicts() { return (int)g_id_conflicts.size(); }
static void idcheck_init() {
    const wchar_t* p = _wgetenv(L"PCBP_IDCHECK");
    if (!p || !*p) return;
    g_id_log = _wfopen(p, L"w");
    GImGuiIdProbe = id_probe;
    if (_wgetenv(L"PCBP_IDWALK")) g_walk_step = 0;
}
// one walk step every ~12 frames: Home, every view of the first project, every top-level menu held open, the
// common dialogs (opened, then closed again from inside their nested frame loop), back to Home, quit.
static void idcheck_walk() {
    if (g_walk_step < 0) return;
    request_redraw(3);
    static const bool selftest = _wgetenv(L"PCBP_IDWALK_SELFTEST") != nullptr;   // proves the probe: one deliberate conflict
    if (selftest && g_walk_step == 1) { ImGui::Begin("##idselftest"); ImGui::Button("dup"); ImGui::Button("dup"); ImGui::End(); }
    if (++g_walk_wait < 12) return;
    g_walk_wait = 0;
    struct Step { std::string where; std::function<void()> fn; };
    static std::vector<Step> steps;
    if (steps.empty()) {
        auto view = [](ViewKind k) { return [k]() { if (g_docs.empty()) return; g_show_landing = false; files_set_active(false); g_active = 0; activate_view(g_docs[0]->doc, k); }; };
        steps.push_back({"Home", []() { g_show_landing = true; }});
        for (ViewKind k : {ViewKind::Notes, ViewKind::Plotter, ViewKind::Gerber, ViewKind::Packages, ViewKind::Placements, ViewKind::Bom,
                           ViewKind::Components, ViewKind::Fiducials, ViewKind::Validation, ViewKind::Reports, ViewKind::Machine})
            steps.push_back({kViewNames[(int)k], view(k)});
        steps.push_back({"Plot", view(ViewKind::Plotter)});
        for (const char* m : {"File", "Edit", "View", "Board", "BOM", "Manufacturing", "Library", "Tools", "Migration", "Help"})
            steps.push_back({std::string("menu ") + m, [m]() { g_walk_menu = m; }});
        steps.push_back({"menus closed", []() { g_walk_menu.clear(); }});
        for (int id : {(int)ID_EDIT_OPTIONS, (int)ID_VIEW_APPEARANCE, (int)ID_BOARD_ASSEMBLY_INFO, (int)ID_EDIT_GRID_SPACING, (int)ID_BOARD_OFFSET_ROTATE,
                       (int)ID_HELP_SHORTCUTS, (int)ID_FILE_STATUS, (int)ID_LIB_FACTORY_MODEL, (int)ID_CTX_MAKE_FIDUCIAL}) {
            steps.push_back({"dialog " + std::to_string(id), [id]() {
                if (id == ID_CTX_MAKE_FIDUCIAL && !g_docs.empty()) { auto& c = g_docs[0]->doc->prj.comps; for (size_t i = 0; i < c.size(); ++i) c[i].selected = i == 0; }
                post_command(id); }});
            steps.push_back({"dialog closed", []() { for (ui::Form* f : modal_stack()) f->close(false); }});
        }
        if (_wgetenv(L"PCBP_TEST_IMPORT")) {   // BOM import controls (wizard / inline flow) with a synthetic BOM
            steps.push_back({"BOM import", []() { post_command(ID_BOM_IMPORT); }});
            for (int k = 0; k < 4; ++k) steps.push_back({"BOM import (wait)", []() {}});
            steps.push_back({"BOM import closed", []() { for (ui::Form* f : modal_stack()) f->close(false); }});
            steps.push_back({"BOM view", view(ViewKind::Bom)});
        }
        steps.push_back({"Home (end)", []() { g_show_landing = true; }});
    }
    const int s = g_walk_step++;
    if (s < (int)steps.size()) { g_id_where = steps[s].where; if (g_id_log && _wgetenv(L"PCBP_IDWALK_TRACE")) { std::fprintf(g_id_log, "step %d %s (modals %d)\n", s, g_id_where.c_str(), (int)modal_stack().size()); std::fflush(g_id_log); } steps[s].fn(); return; }
    if (!modal_stack().empty()) { for (ui::Form* f : modal_stack()) f->close(false); --g_walk_step; return; }
    if (g_id_log) { std::fprintf(g_id_log, "walk done: %d conflict(s), %d steps\n", idcheck_conflicts(), (int)steps.size()); std::fclose(g_id_log); g_id_log = nullptr; }
    for (auto& u : g_docs) u->doc->prj.dirty = false;   // never prompt to save
    ExitProcess((UINT)idcheck_conflicts());
}


// R12 (REVIEW_V11 §18): app-wide text-field context menu (native Windows menu: Cut / Copy / Paste / Select All)
// for every ImGui text input. Detection is generic (right-click while the hovered item shows the text cursor);
// the action re-activates the field (ImGui keeps its selection when the same field is re-activated) and replays
// the standard shortcut, so Ctrl+X/C/V/A semantics are identical to the keyboard.
static struct { ImGuiID id = 0; int action = 0, phase = 0, frames = 0; bool had_sel = false; } g_tctx;
static void text_ctx_frame() {
    ImGuiContext& g = *GImGui;
    ImGuiIO& io = ImGui::GetIO();
    if (g_tctx.phase == 2) {
        if (g.ActiveId == g_tctx.id) {
            auto chord = [&](ImGuiKey k) { io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(k, true); io.AddKeyEvent(k, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); };
            const bool whole = !g_tctx.had_sel;   // nothing selected: Cut / Copy act on the whole field
            switch (g_tctx.action) {
            case 1: if (whole) chord(ImGuiKey_A); chord(ImGuiKey_X); break;
            case 2: if (whole) chord(ImGuiKey_A); chord(ImGuiKey_C); break;
            case 3: chord(ImGuiKey_V); break;
            case 4: chord(ImGuiKey_A); break;
            }
            g_tctx.phase = 0;
            request_redraw(6);
        } else if (++g_tctx.frames > 20) g_tctx.phase = 0;
        else request_redraw(2);
        return;
    }
    if (g_tctx.phase == 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::GetMouseCursor() == ImGuiMouseCursor_TextInput && g.HoveredId) {
        g_tctx.id = g.HoveredId;
        g_tctx.had_sel = g.ActiveId == g.HoveredId && g.InputTextState.ID == g.HoveredId && g.InputTextState.HasSelection();
        g_tctx.phase = 1;
        defer([]() {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"Cu&t\tCtrl+X");
            AppendMenuW(m, MF_STRING, 2, L"&Copy\tCtrl+C");
            AppendMenuW(m, MF_STRING | (IsClipboardFormatAvailable(CF_UNICODETEXT) ? 0 : MF_GRAYED), 3, L"&Paste\tCtrl+V");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 4, L"Select &All\tCtrl+A");
            POINT pt; GetCursorPos(&pt);
            int r = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, nullptr);
            DestroyMenu(m);
            if (r <= 0) { g_tctx.phase = 0; return; }
            g_tctx.action = r; g_tctx.frames = 0; g_tctx.phase = 2;
            ImGui::ActivateItemByID(g_tctx.id);
            request_redraw(4);
        });
    }
}
static bool g_keys_stale = false;   // R20: window regained focus -> ImGui key state is stale
static void render_frame() {
    const double t0 = now_s();
    if (GImGuiIdProbe) g_id_frame.clear();
    perf_detect_adapter();
    if (g_keys_stale) { g_keys_stale = false; imgui_drop_stale_keys(ImGui::GetIO()); }   // R20 (input_resync.h)
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    g_in_frame = true;
    draw_main();
    gui::align_cad_gerber_draw();   // R13: floating Align CAD to Gerber tool
    draw_modals();
    draw_feedback();
    text_ctx_frame();
    draw_perf_overlay();
    idcheck_walk();
    g_in_frame = false;
    ImGui::Render();
    const GTheme& t = th();
    gfx_begin_frame(((t.bg) & 255) / 255.f, ((t.bg >> 8) & 255) / 255.f, ((t.bg >> 16) & 255) / 255.f);
    gfx_render_imgui(ImGui::GetDrawData());
    const double t1 = now_s();
    gfx_present(true);
    auto& P = g_perf_st;
    P.last_ms = (t1 - t0) * 1000.0;
    P.present_ms = (now_s() - t1) * 1000.0;
    P.frames++; P.win_frames++; P.win_ms_sum += P.last_ms; P.win_ms_max = std::max(P.win_ms_max, P.last_ms);
}

bool pump_and_render() {
    MSG m;
    bool any = false;
    if (g_redraw <= 0) {
        // idle: wait for input (or a timer tick so status/animations stay fresh)
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT);
    }
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) { g_quit = true; return false; }
        TranslateMessage(&m);
        DispatchMessageW(&m);
        // R10 §2: WM_SYSTIMER (0x118, caret blink / system timers; Wine posts it ~3x per second) is not a reason to
        // redraw: it caused a steady ~5 frames/s idle loop. Everything else (input, paint, size...) still redraws.
        if (m.message != 0x0118) any = true;
        perf_note_msg(m.message);
    }
    if (any) g_redraw = std::max(g_redraw, 2);
    if (IsIconic(g_hwnd)) { Sleep(30); return true; }
    if (g_redraw > 0 || any) {
        g_perf_msgs[any ? 0xFFFE : 0xFFFF]++;   // frame cause: FFFE input/message, FFFF request_redraw
        render_frame();
        if (g_redraw > 0) g_redraw--;
    }
    perf_tick();
    if (g_perf_st.overlay && g_redraw <= 0) {   // overlay refresh: at most 1 frame / s (not a redraw loop)
        static double s_last = 0;
        if (now_s() - s_last > 1.0) { s_last = now_s(); g_redraw = 1; }
    }
    return true;
}

static void open_paths(const std::vector<std::wstring>& paths) {
    std::vector<std::string> bad;
    for (auto& path : paths) {
        std::string ext = file_ext_lower(ui::U(path));
        DocPtr d = adoc();
        Importer* im = PluginRegistry::get().detect(ui::U(path));
        if (!d && app::is_dropped_table(path)) { d = new_document(); activate_view(d, ViewKind::Plotter); }   // R16c: BOM / centroid onto the start page
        if (d && app::route_dropped_table(g_hwnd, *d, path)) continue;   // R16c: CSV content-sniffed -> BOM or centroid wizard
        if (ext != ".pcbp" && d && files_route_open(path)) continue;   // no doc: open_document() routes
        if (ext == ".pcbp" || !d || (im && im->info().import_mode == ImportMode::Project)) open_document(path);
        else if (im) run_importer(g_hwnd, d, im, path);
        else if ((GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) import_auto(path);   // folder: content detection
        else if (ext == ".zip" || ext == ".tgz" || ext == ".tar" || ext == ".gz") import_auto(path);
        else { std::string u = ui::U(path); size_t sl = u.find_last_of("\\/"); bad.push_back(sl == std::string::npos ? u : u.substr(sl + 1)); }
    }
    if (!bad.empty()) {   // one combined message instead of one box per file
        std::string m = std::to_string(bad.size()) + (bad.size() == 1 ? " file was" : " files were") + " not recognised: ";
        for (size_t i = 0; i < bad.size() && i < 6; ++i) m += (i ? ", " : "") + bad[i];
        if (bad.size() > 6) m += ", ...";
        toast(m, ToastKind::Error, 8.f);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    // R20 open-dialog loop (core/dialog_guard.h, gui/input_resync.h): count real user input for the dialog
    // loop guard, and drop ImGui's key state when the window regains focus (key-ups went to a modal dialog).
    switch (msg) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_DROPFILES: ui::dialog_note_input(); break;
    case WM_SETFOCUS: g_keys_stale = true; break;
    case WM_ACTIVATEAPP: if (w) g_keys_stale = true; break;
    }
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) return 1;
    switch (msg) {
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) { gfx_resize(LOWORD(l), HIWORD(l)); request_redraw(2); if (ImGui::GetCurrentContext() && !g_in_frame && modal_stack().empty()) render_frame(); }
        return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        float s = HIWORD(w) / 96.f;
        defer([s]() { rebuild_fonts_for_dpi(s); apply_theme(); });
        return 0;
    }
    case WM_GETMINMAXINFO: ((MINMAXINFO*)l)->ptMinTrackSize = {800, 560}; return 0;
    case WM_COPYDATA: if (platform::handle_copydata(h, (long long)l)) return TRUE; break;   // second instance forwarded its files
    case WM_DROPFILES: {
        HDROP hd = (HDROP)w;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        for (UINT i = 0; i < n; ++i) { wchar_t p[MAX_PATH * 2]; DragQueryFileW(hd, i, p, MAX_PATH * 2); paths.push_back(p); }
        DragFinish(hd);
        if (!gerber_dialog_drop(paths)) defer([paths]() { if (!job_files_drop(adoc(), paths)) open_paths(paths); });   // gerber dialog consumes drops synchronously; R19b Files tab files them into the job archive
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (l && !lstrcmpW((LPCWSTR)l, L"ImmersiveColorSet") && ui::theme_mode() == ui::ThemeMode::System)
            defer([]() { apply_theme_mode(ui::ThemeMode::System); });
        break;
    case WM_CLOSE:
        defer([h]() {
            std::vector<DocPtr> v;
            for (auto& u : g_docs) v.push_back(u->doc);
            for (auto& d : v) if (!close_document(d)) return;
            WINDOWPLACEMENT wp{}; wp.length = sizeof wp;
            GetWindowPlacement(h, &wp);
            settings().win = wp.rcNormalPosition;
            settings().maximized = wp.showCmd == SW_SHOWMAXIMIZED;
            settings().save();
            recovery::end_session();   // clean exit: no recovery offer next time
            g_session_ok = false;
            DestroyWindow(h);
        });
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_KEYDOWN: {
        if (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput) break;
        const bool ctrl = GetKeyState(VK_CONTROL) & 0x8000, shift = GetKeyState(VK_SHIFT) & 0x8000;
        if (!modal_stack().empty()) break;
        int id = 0;
        if (ctrl) {
            switch (w) {
            case 'N': id = ID_FILE_NEW; break; case 'O': id = ID_FILE_OPEN; break; case 'S': id = shift ? ID_FILE_SAVEAS : ID_FILE_SAVE; break;
            case 'P': id = ID_FILE_PRINT; break; case 'Z': id = ID_EDIT_UNDO; break; case 'Y': id = ID_EDIT_REDO; break;
            case 'X': id = ID_EDIT_CUT; break; case 'C': id = ID_EDIT_COPY; break; case 'V': id = ID_EDIT_PASTE; break;
            case 'A': id = ID_EDIT_SELECT_ALL; break; case 'D': id = ID_EDIT_DUPLICATE; break; case 'F': id = ID_TOOL_SEARCH; break; case 'G': id = ID_POINT_GENERATE; break;
            case 'R': id = ID_MFG_CHECK; break; case 'W': id = shift ? ID_WIN_CLOSE_ALL : ID_FILE_CLOSE; break;
            case VK_TAB: id = shift ? ID_VIEW_PREV_TAB : ID_VIEW_NEXT_TAB; break;
            }
        } else if (w == VK_DELETE) id = ID_EDIT_DELETE;
        else if (w == VK_F1) id = ID_HELP_CONTENTS;
        else if (w == VK_F12) { perf_overlay_toggle(); return 0; }
        if (id) { post_command(id); return 0; }
        break;
    }
    }
    return DefWindowProcW(h, msg, w, l);
}

int run_app(int show) {
    crash::install();   // R12: minidump + log + emergency autosave on any crash
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.hIconSm = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PCBPlotter2";
    RegisterClassExW(&wc);
    auto& s = settings();
    int x = s.win.left, y = s.win.top, w = s.win.right - s.win.left, h = s.win.bottom - s.win.top;
    if (x == CW_USEDEFAULT || w < 600 || h < 400) { x = y = CW_USEDEFAULT; w = 1440; h = 900; }
    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"PCB Plotter", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return 1;
    ui::theme_window(g_hwnd);
    std::string err;
    if (!gfx_init(g_hwnd, err)) {
        MessageBoxW(g_hwnd, (L"Graphics initialisation failed: " + ui::W(err)).c_str(), L"PCB Plotter", MB_ICONERROR);
        return 1;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // >64k vertices per drawlist
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ImGui_ImplWin32_Init(g_hwnd);
    float scale = ui::window_dpi(g_hwnd) / 96.f;
    set_dpi_scale(scale);
    set_ui_scale(s.ui_scale_pct / 100.f, s.icon_scale_pct / 100.f);   // context exists; fonts rebuilt below
    set_perf_mode(s.perf_mode == 2 || (_wgetenv(L"PCBP_PERF_MODE") && _wgetenv(L"PCBP_PERF_MODE")[0] == L'1'));
    if (_wgetenv(L"PCBP_PERF_OVERLAY")) g_perf_st.overlay = true;
    apply_theme();
    rebuild_fonts_for_dpi(scale);
    apply_theme();
    idcheck_init();
    ShowWindow(g_hwnd, s.maximized ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(g_hwnd);
    request_redraw(3);
    app::recovery_startup();   // R12: session lock, crash notice, recovery dialog
    if (_wgetenv(L"PCBP_TEST_CRASH")) defer([]() { crash::test_crash(); });   // automation: crash handler test
    if (const wchar_t* fe = _wgetenv(L"PCBP_OPEN_EDITOR")) {   // automation: "path[|t];path..." opens file editors (|t = text editor)
        std::wstring all = fe;
        for (size_t a = 0; a < all.size();) {
            size_t b = all.find(L';', a); if (b == std::wstring::npos) b = all.size();
            std::wstring p = all.substr(a, b - a); int mode = 1;
            if (p.size() > 2 && p.compare(p.size() - 2, 2, L"|t") == 0) { p.resize(p.size() - 2); mode = 2; }
            if (p.size() > 2 && p.compare(p.size() - 2, 2, L"|a") == 0) { p.resize(p.size() - 2); mode = 0; }   // |a = auto (documents / email / binary)
            defer([p, mode]() { files_open(p, mode); });
            a = b + 1;
        }
    }
    if (_wgetenv(L"PCBP_TEST_BOARD_IMAGE")) defer([]() { post_command(ID_IMPORT_BOARD_IMAGE); });   // automation: board image import + calibration
    if (const wchar_t* oj = _wgetenv(L"PCBP_TEST_ODB")) { std::wstring p = oj; defer([p]() { open_document(p); }); }   // automation: interactive ODB++ import
    if (const wchar_t* demo = _wgetenv(L"PCBP_DEMO_FEEDBACK")) {   // automation: exercise toasts + progress overlay
        toast("Opened demo.pcbp \xE2\x80\x94 412 components", ToastKind::Success, 0);
        toast("3 parts have no package assigned", ToastKind::Warning, 0, "Show");
        toast("Could not read panel.gbr", ToastKind::Error, 0);
        toast("Tip: Ctrl+F searches references", ToastKind::Info, 0);
        status("Saved demo.pcbp", ToastKind::Success);
        if (demo[0] == L'2') run_with_progress("Importing BOM", [](Progress& p) { for (int i = 0; i < 600 && !p.cancelled(); ++i) { p.set(i / 600.f * 1.6f > 1 ? 1 : i / 600.f * 1.6f, "Reading rows... (bom_rev_c.csv)"); Sleep(50); } });
    }
    while (!g_quit) {
        if (!pump_and_render()) break;
        while (!g_deferred.empty() && !g_quit) {
            auto fn = std::move(g_deferred.front());
            g_deferred.pop_front();
            fn();
        }
        autosave_tick();
    }
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    gfx_shutdown();
    return 0;
}

} // namespace pcbp::gui
