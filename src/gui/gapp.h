#pragma once
// ============================================================================
// gapp.h — PCB Plotter v2 UI framework (Dear ImGui docking + D3D11 + D2D).
//
// Model:
//   * One main window. Top: menu bar (+ theme switcher at the right), toolbar
//     (global tools + the active view's tools), document tabs.
//   * Each open document shows ONE active View at a time; the persistent tab
//     strip at the bottom switches views in workflow order (Plot, Gerber,
//     Placements, BOM, Packages, Components, Reports, Notes, Export).
//   * No documents: landing page.
//   * Everything that can block (dialogs, file pickers, imports) runs OUTSIDE
//     the ImGui frame: UI code calls gui::defer(fn) or app::post_command(id).
//     ui::Form / ui::message keep their blocking API: they spin nested frames.
// ============================================================================
#include "../app/app.h"
#include "../app/doc.h"
#include "../ui/icons.h"
#include "imgui.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <memory>
#include <string>

namespace pcbp::app {
// R11 UI preferences ([UI] section of PCBPlotter.ini, written immediately; settings.cpp)
int ui_pref(const char* key, int def = 0);
void set_ui_pref(const char* key, int v);
int reset_ui_prefs(const char* prefix);   // removes every key starting with prefix; returns the count
}

namespace pcbp::gui {

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
struct GTheme {
    bool dark = true;
    ImU32 bg, panel, panel2, raised, text, text_dim, text_faint, border, accent, accent_hover, accent_text;
    ImU32 field, field_hover, sel, sel_text, warn, error, ok, canvas, statusbar, statusbar_text, tab_active, tab_idle;
};
const GTheme& th();
float S(float px);                       // DPI scale helper (logical px -> physical)
float dpi_scale();                       // effective scale = monitor DPI x UI scale
float SI(float px);                      // icon size helper (S() x icon scale)
float monitor_dpi_scale();
float ui_scale();                        // Settings > Appearance (1.0 = 100 %)
float icon_scale();
void set_ui_scale(float ui, float icon); // live rebuild of fonts / style / icon atlas (outside a frame)
bool perf_mode();                        // Performance mode: animations, fades, glow off
void set_perf_mode(bool on);
void apply_theme();                      // rebuild ImGui style + icon atlas from ui::theme_dark()
extern ImFont* g_font;                   // UI font
extern ImFont* g_font_bold;
extern ImFont* g_font_mono;
extern ImFont* g_font_big;               // landing / headings
// Best raster of a font family (g_font / g_font_bold / g_font_mono) for drawing at `px` pixels:
// zoomed / small text uses a bake near its destination size instead of a scaled 1x atlas (gtheme.cpp).
ImFont* font_for_px(ImFont* family, float px);

// ---------------------------------------------------------------------------
// Icons (vector icons from ui/icons.cpp rasterised into a theme-tinted atlas)
// ---------------------------------------------------------------------------
void icon_draw(ImDrawList* dl, ui::Icon ic, ImVec2 pos, float size, float alpha = 1.f, bool on_accent = false);
void icon_image(ui::Icon ic, float size, float alpha = 1.f);   // inline item
void draw_logo(ImDrawList* dl, ImVec2 pos, float size);        // app logo (vector, crisp at any size)
// Square icon button. Returns true when clicked. `checked` draws the toggled state.
bool icon_button(const char* id, ui::Icon ic, const char* tooltip, bool checked = false, bool enabled = true, float size = 0);
// Rounded button with optional leading icon. primary => accent filled.
bool button(const char* label, ui::Icon ic = ui::Icon::None, bool primary = false, ImVec2 size = ImVec2(0, 0), bool enabled = true);
void tooltip(const char* text);
// R19a: capped reference list for tooltips: "R1, R2, ... R8 …+N more" (total = full count when refs holds only a sample)
std::string ref_list_capped(const std::vector<std::string>& refs, size_t total = 0, size_t max_refs = 8);
// P1-10 one tab style for project AND document tabs: type icon, label, dirty dot, close (x on hover / active,
// middle-click closes). Draws at the cursor and submits one item (use BeginPopupContextItem() after it for the
// context menu). width 0 = fit the label (horizontal strip); > 0 = fixed (vertical strip).
struct TabHit { bool clicked = false, close = false; };
TabHit tab_item(const char* id, const std::string& label, ui::Icon ic, bool active, bool dirty, const char* tip = nullptr, float width = 0);
float tab_item_height();
float tab_strip_width();   // width of the vertical tab strip being drawn (shell), 0 = top strip
void section_header(const char* text);   // small caps dim header
bool search_box(const char* id, char* buf, size_t n, const char* hint, float width = -1);


// ---------------------------------------------------------------------------
// User feedback (v2.3) — see specs/UI_FEEDBACK.md
// ---------------------------------------------------------------------------
enum class ToastKind { Info, Success, Warning, Error };
// Stacked bottom-right notifications (slide + fade, icon, click to dismiss, hover pauses).
// Thread-safe (may be called from worker threads). secs <= 0: sticky until clicked.
// action_label/action: optional button; action runs deferred on the UI thread.
void toast(const std::string& msg, ToastKind kind = ToastKind::Info, float secs = 3.5f,
           const char* action_label = nullptr, std::function<void()> action = {});
// Transient status-bar message with a leading kind icon (and a short colour flash). UI thread.
void status(const std::string& msg, ToastKind kind);

// Background work with a progress overlay. work() runs on a WORKER thread: it must not touch
// ImGui, Docs or settings; parse into local data and capture it. done(ok) runs on the UI thread
// (deferred, may block / open dialogs) after work returns; ok=false when cancelled or work threw
// (exception text is shown as an Error toast). Apply document mutations in done().
class Progress {
public:
    void set(float frac, const char* text = nullptr);   // frac 0..1, or <0 = indeterminate
    void text(const char* t);
    bool cancelled() const;
    // internal state (read by the overlay)
    std::atomic<float> frac{-1.f};
    std::atomic<bool> cancel{false};
    mutable std::mutex mx;
    std::string msg;
};
struct ProgressOpts {
    bool modal = true;        // dim + block input to the app while running (overlay card)
    bool cancellable = true;  // show Cancel (work must poll cancelled())
    float delay = 0.15f;      // seconds before the overlay appears (fast jobs never flash)
};
void run_with_progress(const char* title, std::function<void(Progress&)> work,
                       std::function<void(bool ok)> done = {}, ProgressOpts opts = {});
// Blocking variant for code that already runs OUTSIDE a frame (deferred commands, dialogs): spins
// nested frames (overlay animates, app stays responsive) until work finishes. Returns true when
// work completed (not cancelled / no exception). Inside a frame it runs work inline.
bool run_with_progress_wait(const char* title, std::function<void(Progress&)> work, ProgressOpts opts = {});
bool busy();                  // a run_with_progress job is active
// Inline widgets
void progress_bar(float frac, ImVec2 size = ImVec2(-1, 0), const char* overlay = nullptr);  // frac<0: indeterminate (animated)
void spinner(float radius = 0, ImU32 col = 0);   // radius 0 = text height/2; col 0 = accent

// Animation helpers (frame-time based; keep the app redrawing while moving)
float anim(ImGuiID id, float target, float speed = 14.f);   // exponential approach; returns current value
float anim(const char* key, float target, float speed = 14.f);
float pulse(float period = 1.2f);                           // 0..1..0 sine
void flash(ImGuiID key);                                    // start a highlight for key (e.g. a changed row)
float flash_amount(ImGuiID key, float secs = 0.9f);         // 1 -> 0 over secs since flash(); 0 when idle
ImU32 col_lerp(ImU32 a, ImU32 b, float t);
ImU32 col_alpha(ImU32 c, float a);                          // replace alpha (0..1)

// Menus / buttons
// Menu item with an icon column, right-aligned shortcut and disabled state. Returns true when clicked.
bool menu_item_icon(const char* label, ui::Icon ic, const char* shortcut = nullptr, bool checked = false, bool enabled = true);
bool begin_menu_icon(const char* label, ui::Icon ic, bool enabled = true);   // submenu with icon; EndMenu() when true
// Toolbar button for a command id (icon, state, tooltip "Text (Shortcut)\nprompt"); posts the command.
bool cmd_button(int id);
void toolbar_separator();
// Empty-state block centred in the available region: big faded icon, title, hint, optional primary button.
bool empty_state(ui::Icon ic, const char* title, const char* hint = nullptr, const char* button_label = nullptr, ui::Icon button_icon = ui::Icon::None);
// Themed yes/no confirmation (blocking, via ui::Form). danger => red primary icon.
bool confirm(const char* title, const std::string& text, const char* yes = "OK", bool danger = false);
// P1-08 informational confirmation with "Don't show this message again" (persisted as Confirm.<key>; Settings >
// Reset suppressed warnings). Returns true at once when suppressed. NOT for destructive / irreversible actions.
bool confirm_info(const char* key, const char* title, const std::string& text, const char* yes = "OK");
int reset_suppressed_warnings();      // re-enables every confirm_info; returns how many were suppressed

// P1-07 tutorial hints: instructional overlays are off by default (Help > Tutorial Mode). Each hint has a key and can be
// dismissed for good with its x (Hint.<key>). Ordinary hover tooltips are independent of this.
bool tutorial_mode();
void set_tutorial_mode(bool on);
bool hint_shown(const char* key);     // tutorial mode on and not dismissed
void hint_dismiss(const char* key);
int hints_reset();                    // un-dismiss all
// Hint pill centred on anchor.x; anchor.y is its top (below = false) or bottom edge. Draws only when hint_shown(key).
// Returns true when drawn. The x (right end) dismisses it; canvases ignore clicks over a pill (hint_hit).
bool hint_pill(ImDrawList* dl, const char* key, const std::string& text, ImVec2 anchor, bool bottom = false, ui::Icon ic = ui::Icon::Question, float alpha = 1.f);
bool hint_hit(ImVec2 screen);         // a pill drawn last frame covers this point
// P1-09 keyboard ownership: a focused list that handles arrow keys calls claim_keyboard() each frame; canvases then
// ignore their keyboard shortcuts for this and the next frame (the viewport keys apply when the viewport has focus).
void claim_keyboard();
bool keyboard_claimed();

// ---------------------------------------------------------------------------
// Deferred actions (run after the current frame, may block)
// ---------------------------------------------------------------------------
void defer(std::function<void()> fn);
void request_redraw(int frames = 2);
bool in_frame();

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------
enum class ViewKind { Plotter, Gerber, VisualAid, Machine, Notes, Placements, Bom, Packages, Reports, Components, Fiducials, Validation, Files, Count };

class View {
public:
    virtual ~View() = default;
    app::DocPtr doc;
    virtual ViewKind kind() const = 0;
    virtual const char* title() const = 0;            // tab label
    virtual ui::Icon icon() const = 0;
    // draw content into the current ImGui window region (size = available)
    virtual void draw(ImVec2 size) = 0;
    // optional view-specific toolbar items (drawn on the main toolbar after the global group)
    virtual void draw_toolbar() {}
    virtual bool on_command(int id) { (void)id; return false; }
    virtual int cmd_state(int id) { (void)id; return -1; }   // -1 not handled; bit0 enabled bit1 checked
    virtual std::string status() const { return {}; }       // left status pane text
    virtual std::string status_right() const { return {}; } // coords pane
    virtual bool can_print() const { return false; }
    virtual void print(HDC dc, const RECT& page, bool one_to_one) { (void)dc; (void)page; (void)one_to_one; }
    virtual void on_activate() {}
};

// factory for every view kind (implemented in the view's own file)
std::unique_ptr<View> make_plotter_view();     // plotter_view.cpp
std::unique_ptr<View> make_gerber_view();      // gerber_workspace.cpp
std::unique_ptr<View> make_visual_aid_view();  // output_views.cpp
std::unique_ptr<View> make_machine_view();     // output_views.cpp  (Machine Programming + Machine View sub-tabs)
std::unique_ptr<View> make_notes_view();       // output_views.cpp
std::unique_ptr<View> make_placements_view();  // tables_views.cpp
std::unique_ptr<View> make_bom_view();         // tables_views.cpp
std::unique_ptr<View> make_packages_view();    // package_library.cpp
std::unique_ptr<View> make_reports_view();     // tables_views.cpp
std::unique_ptr<View> make_validation_view();  // tables_views.cpp (Data > Validation)
std::unique_ptr<View> make_fiducials_view();   // tables_views.cpp (Data > Fiducials)
std::unique_ptr<View> make_components_view();  // flexdb_view.cpp (FlexLine component DB)
// R19a tab hook: a view file can register its factory from a static initializer, e.g.
//   const bool reg_ = register_view_factory(ViewKind::Files, make_files_view);
// Registered factories are used by DocUI::view(); an unregistered Files tab shows a placeholder.
using ViewFactory = std::unique_ptr<View> (*)();
bool register_view_factory(ViewKind k, ViewFactory f);

// per-document UI state
struct DocUI {
    app::DocPtr doc;
    std::unique_ptr<View> views[(int)ViewKind::Count];
    ViewKind active = ViewKind::Plotter;
    // R12 autosave state (shell.cpp)
    std::string rec_id;           // recovery project id ("" until the first snapshot)
    uint64_t rec_gen = 0, rec_kick = 0;
    double rec_t = 0;
    int rec_serial = 0;
    std::shared_ptr<std::atomic<bool>> rec_busy = std::make_shared<std::atomic<bool>>(false);
    View* view(ViewKind k);
    View* current() { return view(active); }
};

DocUI* active_docui();
DocUI* docui_of(const app::Doc* d);
// factory model panel (model point list, assign/unassign, generate) — embeddable by views
void draw_factory_panel(const app::DocPtr& d, float width);
// set by app::open_package_editor; read + cleared by the packages view
extern std::string g_focus_package;
// set by app::show_report; read + cleared by the reports view
extern int g_report_request;
// run a command id (from commands.h) after the current frame
void post_command(int id);
int command_state(int id);           // combined view/global state (bit0 enabled, bit1 checked)
bool menu_item(int id);               // ImGui MenuItem for a command (text/shortcut/icon/state); posts on click
void activate_view(const app::DocPtr& d, ViewKind k);
View* find_view(const app::DocPtr& d, ViewKind k);

// selected model point (shared by visual aid / machine / notes views and the factory panel)
int current_point(const app::Doc& d);
void set_current_point(app::Doc& d, int p);

// Status text (left pane), transient
void status(const std::string& msg);

// Fly the plotter camera to a set of components (indices into prj.comps); selects them
void focus_components(const app::DocPtr& d, const std::vector<int>& idx, bool select = true);

// Procedural PCB art with travelling pulses (About box, landing background). 60 fps while drawn.
// slot: independent cached scene (0 landing, 1 about). seed: new layout when it changes.
void draw_circuit_fx(ImDrawList* dl, ImVec2 a, ImVec2 b, float intensity, uint32_t seed, int slot = 0);
void show_about();                    // animated About overlay (non-blocking)

// Named selection sets (Project::selection_sets). mode: 0 replace, 1 add, 2 subtract
void selset_save(const app::DocPtr& d);
void selset_apply(app::Doc& d, int idx, int mode);
void selset_menu_items(const app::DocPtr& d);   // recall / rename / delete items for a menu or popup
// P2-10 Export Layers replace Selection Sets in the UI (data still loaded / saved for old projects; the Selection
// Sets controls only appear for projects that already contain sets). Items: one per export layer (moves the
// selected, visible placements into it), "New export layer from selection", "Remove from export layer".
void move_to_layer_menu_items(const app::DocPtr& d);
bool selsets_visible(const app::Doc& d);
// Side display mode: 0 top only, 1 bottom only, 2 x-ray both sides
int side_mode(const app::Doc& d);
void set_side_mode(app::Doc& d, int m);
// Side display (v2.2): viewed side (top = side 1, bottom = side 2, mirrored) + independent X-ray toggle.
// X-ray off: only the viewed side is drawn / selectable. X-ray on: both sides, far side semi-transparent.
bool view_bottom(const app::Doc& d);
void set_view_bottom(app::Doc& d, bool bottom);
bool side_xray(const app::Doc& d);
void set_xray(app::Doc& d, bool on);
void set_side_display(app::Doc& d, int mode);     // 0 Normal, 1 Ghost, 2 X-ray
void set_back_selectable(app::Doc& d, bool on);
// zoom the document's views to extents (placements + frames; fiducials / origin excluded)
void zoom_extents_doc(const app::DocPtr& d);
// R13 (REVIEW_V12 §6) Board > Align CAD to Gerber (align_tool.cpp): floating tool window
void align_cad_gerber_open(const app::DocPtr& d);
void align_cad_gerber_draw();
void gerber_outline_tool_request(const app::Doc* d, bool board_outline);   // R14: Gerber tab outline tool (align_tool.cpp)
void outline_ack_strip(const app::DocPtr& d, bool compact);   // R17b: inferred-outline Use as-is / Confirm / Define / Don't warn (plotter_view.cpp)

// Format helpers
std::string fmt_len_u8(double mm, bool units = true);     // display units, relative values
std::string fmt_pos_u8(const app::Doc& d, Vec2 p);        // "(x, y)" relative to virtual origin
std::string W2U(const std::wstring& w);


// File editors (sheet_editor.cpp / text_editor.cpp): document-independent "Editors" workspace (R21; was "Files")
// (spreadsheet + text editor tabs for BOM / centroid source files). Shell shows it instead of a doc.
bool files_any();                                   // any editor file open
bool files_active();                                // the Editors workspace is the current page
void files_set_active(bool on);
void files_draw(ImVec2 size);                       // workspace content (own file tabs + editor)
bool files_on_command(int id);                      // Save / Save As / Close / Edit ids while active
bool files_open(const std::wstring& path, int mode = 0);   // mode 0 by extension, 1 spreadsheet, 2 text
void files_open_dialog(int mode = 0);               // file picker -> editor (call outside a frame)
// Open routing for .xlsx/.xls/.csv/.tsv/.txt/.prn: asks editor vs import. true = handled here
// (opened in an editor, or cancelled); false = caller continues with its importer.
bool files_route_open(const std::wstring& path);
// Import wizard source override (set by the editors, consumed once by dlg_import_bom / _centroid).
void set_import_source(std::string bytes, int start_line);
bool take_import_source(std::string& bytes, int& start_line);
// BOM wizard import-mode preselection (0 merge, 1 replace, 2 parts only), consumed once by dlg_import_bom.
void set_import_mode_hint(int mode);
int take_import_mode_hint();           // -1 when none
void files_new_sheet();                 // blank spreadsheet tab in the Editors workspace
// Document tabs (REVIEW_V9 §10): one top-level tab per open file document (type icon + label,
// "[linked]" when attached to the current project). Call inside the shell's document BeginTabBar.
void files_doc_tabs();
// Project menu items: attach the current document to the active project (reference / copy) and
// reopen project-linked documents. Semantics: specs/DOCS_EVAL.md.
void files_project_menu_items();
// BOM menu: export the project's BOM to a spreadsheet tab linked to the project
// ("Apply back to project" with a diff preview). Call outside a frame.
void files_open_project_bom(const app::DocPtr& d);

// Pick & Place Program Editor (pnp_editor.cpp): HLC .h8h/.h7h/.h5h/.h4h/.e4x/.e51 + ISS programs as Files tabs.
bool pnp_open(const std::wstring& path);            // open (or activate) in the program editor; call outside a frame
void pnp_open_dialog();                              // file picker -> program editor
bool pnp_route_open(const std::wstring& path);      // program extension: editor vs import prompt; true = handled
void pnp_skip_route_once();                          // next pnp_route_open() returns false (explicit Import Machine Program)

} // namespace pcbp::gui
