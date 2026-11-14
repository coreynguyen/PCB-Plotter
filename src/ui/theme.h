#pragma once
// ============================================================================
// theme.h — light / dark theme for PCB Plotter (Win32 + Direct2D).
// ============================================================================
#include "win.h"

namespace pcbp::ui {

enum class ThemeMode { System = 0, Light = 1, Dark = 2 };

struct Palette {
    bool dark = false;
    // chrome (COLORREF for GDI)
    COLORREF bg, panel, panel_hover, panel_press, text, text_dim, border, accent, accent_text;
    COLORREF edit_bg, sel_bg, sel_text, header_bg, menu_bg, tab_active, tab_inactive;
    // canvas (ARGB for D2D)
    uint32_t canvas_bg, grid, grid_major, board_fill, board_edge, comp_body, comp_body_fill;
    uint32_t pad_smt, pad_th, label, selection, origin, fiducial, unassigned, error_red;
    uint32_t rubber, panel_frame, gerber_bg, crosshair;
};

void theme_init();                         // load uxtheme ordinals (call once at startup)
void theme_set_mode(ThemeMode m);          // applies + broadcasts WM_THEMECHANGED to top windows
ThemeMode theme_mode();
bool theme_dark();                         // effective
const Palette& pal();
bool theme_os_dark();

// apply to windows / controls
void theme_window(HWND top);               // title bar + allow dark
void theme_controls(HWND parent);          // recursively theme child common controls
void theme_control(HWND ctl);
HBRUSH theme_brush_bg();
HBRUSH theme_brush_panel();
HBRUSH theme_brush_edit();
// handles WM_CTLCOLOR* for dialogs/forms; returns true if handled
bool theme_ctlcolor(UINT msg, WPARAM w, LPARAM l, LRESULT* res);
// dark menu bar painting (WM_UAHDRAWMENU / WM_UAHDRAWMENUITEM / WM_NCPAINT seam)
bool theme_menubar_message(HWND hwnd, UINT msg, WPARAM w, LPARAM l, LRESULT* res);

// colour helpers
COLORREF blend(COLORREF a, COLORREF b, float t);

} // namespace pcbp::ui
