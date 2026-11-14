#pragma once
// ============================================================================
// app.h — application-level API shared by all windows.
//
// Window factories (open_*) are implemented in the file that owns the window
// type; the frame calls them. Everything runs on the UI thread.
// ============================================================================
#include "../plugins/plugin.h"
#include "../ui/window.h"
#include "commands.h"
#include "doc.h"
#include <functional>

namespace pcbp::app {

// ---------------------------------------------------------------------------
// Settings (persisted to PCBPlotter.ini next to the exe, else %APPDATA%)
// ---------------------------------------------------------------------------
struct Settings {
    int theme = 0;                     // ui::ThemeMode
    int units = 0;                     // pcbp::Units (default mm)
    std::wstring company, company_addr;
    double print_scale_x = 1.0, print_scale_y = 1.0;
    uint32_t frame_color = 0xFF2F9E44;
    std::wstring dir_projects, dir_gerber, dir_cad, dir_bom, dir_centroid, dir_programs, dir_pictures, library_path;
    std::vector<std::wstring> mru;
    std::vector<std::wstring> pinned;  // Home page: pinned projects (shown first, never aged out)
    bool tb_file = true, tb_zoom = true, tb_master = true, tb_viewer = true, tb_align = false, tb_visaid = false;
    bool status_bar = true, factory_bar = true;
    int factory_width = 230;
    double grid_mm = 1.27;
    bool grid_visible = true, snap = false;
    bool import_netlist = false;
    RECT win{};
    bool maximized = true;
    std::vector<std::string> disabled_plugins;
    // R10 §3/§2 appearance + performance
    int ui_scale_pct = 100;            // text / control scale on top of the monitor DPI (80..250; R12: clean install 100%)
    int icon_scale_pct = 100;          // extra icon scale relative to the UI scale (50..250)
    int perf_mode = 0;                 // 0 automatic (suggest on slow/software adapter), 1 off, 2 on
    bool perf_suggest_seen = false;    // the auto-suggestion was shown once (override kept)
    // R14d: live ERP (M1) BOM fetch (specs/M1_BOM.md). No defaults: the user enters server / database.
    std::wstring m1_server, m1_database, m1_user;
    int m1_auth = 1;                   // 0 Windows (integrated), 1 SQL Server login (default: what TrayTagPrinter uses)
    std::wstring m1_pass_dpapi;        // SQL password, CryptProtectData (current user) blob, hex; never plain text

    void load();
    void save() const;
    void add_mru(const std::wstring& path);
    void remove_mru(const std::wstring& path);   // also unpins
    bool is_pinned(const std::wstring& path) const;
    void set_pinned(const std::wstring& path, bool pin);
};

Settings& settings();

// ---------------------------------------------------------------------------
// Base class for MDI child windows bound to a document.
// ---------------------------------------------------------------------------
class DocView : public ui::Window {
public:
    DocPtr doc;
    virtual const wchar_t* kind() const = 0;      // "Universal", "Gerber", ...
    virtual bool on_command(int id) { (void)id; return false; }
    // -1 = not handled; else bit0 = enabled, bit1 = checked
    virtual int cmd_state(int id) { (void)id; return -1; }
    virtual void on_doc_changed(unsigned flags) { (void)flags; invalidate(); }
    virtual void print(HDC dc, const RECT& page, bool one_to_one) { (void)dc; (void)page; (void)one_to_one; }
    virtual bool can_print() const { return false; }
    virtual std::wstring status_text() const { return {}; }   // e.g. component count pane
    void update_title();
    virtual std::wstring title_suffix() const { return kind(); }

protected:
    LRESULT handle(UINT msg, WPARAM w, LPARAM l) override;
    void on_destroyed() override;
};

// ---------------------------------------------------------------------------
// Frame + global services
// ---------------------------------------------------------------------------
HWND frame_hwnd();
HWND mdi_client();
DocView* active_view();
DocPtr active_doc();
std::vector<DocPtr>& open_docs();
void set_status(const std::wstring& msg);           // message pane
void refresh_ui();                                   // toolbar/menu states, factory panel
void factory_panel_refresh();
int selected_point();                                // factory panel selection for active doc (-1 none)

// documents
DocPtr new_document();
DocPtr open_document(const std::wstring& path);      // .pcbp or anything a Project importer handles
bool save_document(const DocPtr& d, bool save_as);
bool close_document(const DocPtr& d);                // prompts if dirty; closes its views

// windows (implemented by the owning modules)
DocView* open_universal(const DocPtr& d);            // universal_view.cpp
DocView* open_gerber(const DocPtr& d);               // gerber_view.cpp
DocView* open_output(const DocPtr& d, int point);    // output_view.cpp (visual aid / notes / machine)
DocView* open_locations(const DocPtr& d, int tab);   // grids.cpp  (tab 0 components, 1 BOM)
DocView* open_package_editor(const DocPtr& d, const std::string& pkg);   // package_editor.cpp
DocView* find_view(const DocPtr& d, const wchar_t* kind);

// dialogs (dialogs.cpp)
bool dlg_options(HWND owner);
bool dlg_assembly_info(HWND owner, Doc& d);
bool dlg_grid_spacing(HWND owner);
bool dlg_location(HWND owner, Doc& d, Component& c);
bool dlg_part_number(HWND owner, Doc& d, const std::string& number);
bool dlg_fiducial(HWND owner, Doc& d, std::vector<Component*> comps);
bool dlg_panelize(HWND owner, Doc& d);
// R14 Step & Repeat (X/Y count, pitch, origin = root circuit cell); pre-filled from the Gerber pitch tool
bool dlg_detect_repeated(HWND owner, Doc& d);   // v17: fold a flattened panel (plot_align detect/fold_repeated_instances)
bool dlg_step_repeat(HWND owner, Doc& d, int nx = 2, int ny = 1, double dx = 0, double dy = 0, int root_col = 0, int root_row = 0);
bool dlg_linear_array(HWND owner, Doc& d);   // R22 Board > Panelize > Create Linear Array (saved parameters; Apply commits)
bool dlg_offset_rotate(HWND owner, Doc& d);
bool dlg_add_board_frame(HWND owner, Doc& d);
bool dlg_confirm_board_outline(HWND owner, Doc& d);   // R16d; R22: "Confirm as measured" (inferred-outline banner)
bool dlg_panel_frame(HWND owner, Doc& d);      // typed upper-left / lower-right panel frame (dialogs.cpp)
bool dlg_circuit_frame(HWND owner, Doc& d);    // exact circuit frame coordinates (dialogs.cpp)
bool dlg_confirm_panel_frame(HWND owner, Doc& d, BBox r);   // confirm + apply a picked panel frame
bool dlg_model_point(HWND owner, Doc& d, int point);            // properties incl. machine setup
bool dlg_factory_model(HWND owner, Doc& d);
bool dlg_import_centroid(HWND owner, Doc& d, const std::wstring& path, bool fusion_mode);
bool dlg_import_bom(HWND owner, Doc& d, const std::wstring& path);
// R16c (REVIEW_V15 §7): a dropped .csv/.txt/.tsv/... is content-sniffed (BOM vs centroid) and opened in that wizard.
// Returns false (caller falls back to the open / import prompt) when the file is not a recognisable table.
bool route_dropped_table(HWND owner, Doc& d, const std::wstring& path);
bool is_dropped_table(const std::wstring& path);   // R16c: BOM / centroid table by content
// R16c (REVIEW_V15 §9): Board > Align / Transform > Transform...: translate / rotate / scale / mirror, live preview, one undo step.
bool dlg_transform(HWND owner, Doc& d);
bool dlg_import_bom_m1(HWND owner, Doc& d);   // R14d: BOM > Import from M1 by Job ID (gui/m1_import.cpp)
bool dlg_import_gerber(HWND owner, Doc& d);
bool dlg_find_ref(HWND owner, Doc& d);
bool dlg_apply_offset_file(HWND owner, Doc& d);
bool dlg_plugin_options(HWND owner, const PluginInfo& info, Options& opts);
void dlg_about(HWND owner);
void dlg_plugins(HWND owner);
void dlg_help(HWND owner, int topic);

// machine program generation (output_view.cpp) — returns false + err
// R21 (AUDIT_V20 P0/P1): every call runs the shared export preflight and the single "Generate anyway?" review
// first (no path skips it); after_review (optional, e.g. the tooling check) runs after the review and may cancel.
// The generated bytes are checked before the destination is touched; success is recorded only after the
// written file reads back identical and passes the program checks.
bool generate_program(HWND owner, Doc& d, int point, std::wstring* out_path, std::string& err,
                      const std::function<bool()>& after_review = {});
// R10 §16: append a Project Info revision-history entry (kind "export" / "checkpoint"); marks the project dirty
void add_history(Doc& d, const std::string& kind, const std::string& summary, const std::string& comment,
                 const std::string& file, const std::string& hash);

// reports (reports.cpp)
void show_report(HWND owner, const DocPtr& d, int which);   // ID_MFG_* command id

// plugin-driven import/export (frame.cpp)
bool run_importer(HWND owner, const DocPtr& d, Importer* imp, const std::wstring& path);
bool run_project_exporter(HWND owner, Doc& d, Exporter* exp);

// helpers
std::wstring units_label();
std::wstring fmt_len(double mm, bool with_units = true);    // in current display units
double parse_len(const std::wstring& s, double def_mm);      // from display units
std::wstring exe_dir();
void load_plugins_dir(PluginRegistry& r, const std::wstring& dir);   // plugin_dll_win.cpp

} // namespace pcbp::app
