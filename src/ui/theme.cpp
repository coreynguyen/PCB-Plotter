// ============================================================================
// theme.cpp — light / dark theme implementation.
//
// Dark mode for native chrome uses the documented DWM attribute for the
// title bar plus the long-standing uxtheme ordinals (#104/#133/#135/#136)
// for popup menus, scrollbars and common controls. The menu bar is painted
// through WM_UAHDRAWMENU / WM_UAHDRAWMENUITEM (same approach as GX2).
// ============================================================================
#include "theme.h"
#include <dwmapi.h>

#ifndef WM_UAHDRAWMENU
#define WM_UAHDRAWMENU 0x0091
#define WM_UAHDRAWMENUITEM 0x0092
#endif

namespace pcbp::ui {

namespace {
typedef union { struct { DWORD cx, cy; } rgsizeBar[2]; struct { DWORD cx, cy; } rgsizePopup[4]; } UAHMENUITEMMETRICS;
typedef struct { DWORD rgcx[4]; DWORD fUpdateMaxWidths : 2; } UAHMENUPOPUPMETRICS;
typedef struct { HMENU hmenu; HDC hdc; DWORD dwFlags; } UAHMENU;
typedef struct { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; } UAHMENUITEM;
typedef struct { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; } UAHDRAWMENUITEM;

enum AppMode { AM_Default = 0, AM_AllowDark = 1, AM_ForceDark = 2, AM_ForceLight = 3 };
using fnSetPreferredAppMode = AppMode(WINAPI*)(AppMode);
using fnAllowDarkModeForWindow = bool(WINAPI*)(HWND, bool);
using fnRefresh = void(WINAPI*)();
using fnFlushMenuThemes = void(WINAPI*)();

HMODULE g_ux = nullptr;
fnSetPreferredAppMode g_setMode = nullptr;
fnAllowDarkModeForWindow g_allowWin = nullptr;
fnRefresh g_refresh = nullptr;
fnFlushMenuThemes g_flush = nullptr;
DWORD g_build = 0;
ThemeMode g_mode = ThemeMode::System;
bool g_dark = false;
HBRUSH g_bg = nullptr, g_panel = nullptr, g_edit = nullptr;
Palette g_pal;

DWORD os_build() {
    using fn = void(WINAPI*)(DWORD*, DWORD*, DWORD*);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto p = nt ? (fn)GetProcAddress(nt, "RtlGetNtVersionNumbers") : nullptr;
    if (!p) return 0;
    DWORD a = 0, b = 0, c = 0;
    p(&a, &b, &c);
    return c & 0x0FFFFFFF;
}

Palette make_light() {
    Palette p{};
    p.dark = false;
    p.bg = RGB(250, 250, 251);
    p.panel = RGB(240, 241, 244);
    p.panel_hover = RGB(224, 228, 236);
    p.panel_press = RGB(204, 212, 226);
    p.text = RGB(28, 30, 34);
    p.text_dim = RGB(108, 114, 124);
    p.border = RGB(212, 215, 222);
    p.accent = RGB(37, 112, 236);
    p.accent_text = RGB(255, 255, 255);
    p.edit_bg = RGB(255, 255, 255);
    p.sel_bg = RGB(210, 226, 252);
    p.sel_text = RGB(16, 18, 22);
    p.header_bg = RGB(246, 247, 249);
    p.menu_bg = RGB(250, 250, 251);
    p.tab_active = RGB(255, 255, 255);
    p.tab_inactive = RGB(232, 234, 239);
    p.canvas_bg = 0xFFE6E9EE;
    p.grid = 0x28000000;
    p.grid_major = 0x48000000;
    p.board_fill = 0xFF1F6B45;
    p.board_edge = 0xFF0E3B25;
    p.comp_body = 0xFF343A42;      // legible on the light canvas and on board green
    p.comp_body_fill = 0x24000000;
    p.pad_smt = 0xFFD9B44A;
    p.pad_th = 0xFFC9A646;
    p.label = 0xFFFFFFFF;
    p.selection = 0xFFFF2D95;
    p.origin = 0xFFFFD43B;
    p.fiducial = 0xFFFFD43B;
    p.unassigned = 0xFFB0B7C3;
    p.error_red = 0xFFE03131;
    p.rubber = 0xFF2570EC;
    p.panel_frame = 0xFF495057;
    p.gerber_bg = 0xFF101418;
    p.crosshair = 0xFF22B8CF;
    return p;
}

Palette make_dark() {
    Palette p = make_light();
    p.dark = true;
    p.bg = RGB(30, 31, 34);
    p.panel = RGB(39, 41, 45);
    p.panel_hover = RGB(54, 57, 63);
    p.panel_press = RGB(66, 71, 80);
    p.text = RGB(226, 228, 232);
    p.text_dim = RGB(150, 155, 164);
    p.border = RGB(58, 61, 67);
    p.accent = RGB(76, 141, 255);
    p.edit_bg = RGB(24, 25, 28);
    p.sel_bg = RGB(38, 79, 140);
    p.sel_text = RGB(255, 255, 255);
    p.header_bg = RGB(36, 38, 42);
    p.menu_bg = RGB(32, 33, 36);
    p.tab_active = RGB(30, 31, 34);
    p.tab_inactive = RGB(44, 46, 51);
    p.canvas_bg = 0xFF16181B;
    p.grid = 0x26FFFFFF;
    p.grid_major = 0x44FFFFFF;
    p.board_fill = 0xFF1A5C3B;
    p.comp_body = 0xFFF0F0F0;
    p.comp_body_fill = 0x30FFFFFF;
    p.board_edge = 0xFF46A36F;
    p.unassigned = 0xFF8C93A0;
    p.panel_frame = 0xFFADB5BD;
    p.gerber_bg = 0xFF0B0D10;
    return p;
}

void rebuild_brushes() {
    if (g_bg) DeleteObject(g_bg);
    if (g_panel) DeleteObject(g_panel);
    if (g_edit) DeleteObject(g_edit);
    g_bg = CreateSolidBrush(g_pal.bg);
    g_panel = CreateSolidBrush(g_pal.panel);
    g_edit = CreateSolidBrush(g_pal.edit_bg);
}

BOOL CALLBACK enum_top(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) {
        theme_window(h);
        theme_controls(h);
        SendMessageW(h, WM_THEMECHANGED, 0, 0);
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    return TRUE;
}
} // namespace

COLORREF blend(COLORREF a, COLORREF b, float t) {
    auto m = [&](int x, int y) { return (int)(x + (y - x) * t + 0.5f); };
    return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}

bool theme_os_dark() {
    DWORD val = 1, sz = sizeof val;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&val, &sz);
        RegCloseKey(k);
    }
    return val == 0;
}

void theme_init() {
    g_build = os_build();
    if (g_build >= 17763) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n && n < MAX_PATH - 20) {
            lstrcatW(path, L"\\uxtheme.dll");
            g_ux = LoadLibraryW(path);
        }
        if (g_ux) {
            g_refresh = (fnRefresh)GetProcAddress(g_ux, MAKEINTRESOURCEA(104));
            g_allowWin = (fnAllowDarkModeForWindow)GetProcAddress(g_ux, MAKEINTRESOURCEA(133));
            g_setMode = (fnSetPreferredAppMode)GetProcAddress(g_ux, MAKEINTRESOURCEA(135));
            g_flush = (fnFlushMenuThemes)GetProcAddress(g_ux, MAKEINTRESOURCEA(136));
        }
    }
    g_dark = theme_os_dark();
    g_pal = g_dark ? make_dark() : make_light();
    rebuild_brushes();
}

ThemeMode theme_mode() { return g_mode; }
bool theme_dark() { return g_dark; }
const Palette& pal() { return g_pal; }
HBRUSH theme_brush_bg() { return g_bg; }
HBRUSH theme_brush_panel() { return g_panel; }
HBRUSH theme_brush_edit() { return g_edit; }

void theme_set_mode(ThemeMode m) {
    g_mode = m;
    g_dark = (m == ThemeMode::Dark) || (m == ThemeMode::System && theme_os_dark());
    g_pal = g_dark ? make_dark() : make_light();
    rebuild_brushes();
    if (g_setMode && g_build >= 18362) g_setMode(g_dark ? AM_ForceDark : AM_ForceLight);
    if (g_flush) g_flush();
    if (g_refresh) g_refresh();
    EnumThreadWindows(GetCurrentThreadId(), enum_top, 0);
}

void theme_window(HWND h) {
    if (g_allowWin) g_allowWin(h, g_dark);
    if (g_build >= 17763) {
        BOOL b = g_dark ? TRUE : FALSE;
        if (FAILED(DwmSetWindowAttribute(h, 20, &b, sizeof b))) DwmSetWindowAttribute(h, 19, &b, sizeof b);
        // Windows 11: rounded corners + matching caption colour
        if (g_build >= 22000) {
            COLORREF cap = g_pal.panel;
            DwmSetWindowAttribute(h, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof cap);
            COLORREF txt = g_pal.text;
            DwmSetWindowAttribute(h, 36 /*DWMWA_TEXT_COLOR*/, &txt, sizeof txt);
        }
    }
}

void theme_control(HWND h) {
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    if (g_allowWin) g_allowWin(h, g_dark);
    if (!lstrcmpiW(cls, WC_LISTVIEWW) || !lstrcmpiW(cls, WC_TREEVIEWW)) {
        SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        if (!lstrcmpiW(cls, WC_LISTVIEWW)) {
            ListView_SetBkColor(h, g_pal.edit_bg);
            ListView_SetTextBkColor(h, g_pal.edit_bg);
            ListView_SetTextColor(h, g_pal.text);
            HWND hdr = ListView_GetHeader(h);
            if (hdr) { if (g_allowWin) g_allowWin(hdr, g_dark); SetWindowTheme(hdr, g_dark ? L"DarkMode_ItemsView" : L"ItemsView", nullptr); }
        } else {
            TreeView_SetBkColor(h, g_pal.edit_bg);
            TreeView_SetTextColor(h, g_pal.text);
        }
    } else if (!lstrcmpiW(cls, L"Edit") || !lstrcmpiW(cls, L"ComboBox") || !lstrcmpiW(cls, L"ListBox")) {
        SetWindowTheme(h, g_dark ? L"DarkMode_CFD" : nullptr, nullptr);
    } else if (!lstrcmpiW(cls, L"Button")) {
        UINT bs = (UINT)(GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK);
        if (bs == BS_GROUPBOX || bs == BS_RADIOBUTTON || bs == BS_AUTORADIOBUTTON || bs == BS_CHECKBOX ||
            bs == BS_AUTOCHECKBOX || bs == BS_3STATE || bs == BS_AUTO3STATE)
            SetWindowTheme(h, g_dark ? L"" : nullptr, g_dark ? L"" : nullptr);
        else
            SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    } else if (!lstrcmpiW(cls, L"ScrollBar") || !lstrcmpiW(cls, L"msctls_trackbar32") || !lstrcmpiW(cls, WC_TABCONTROLW)) {
        SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    } else if (!lstrcmpiW(cls, L"MDIClient")) {
        SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }
    SendMessageW(h, WM_THEMECHANGED, 0, 0);
}

static BOOL CALLBACK enum_child(HWND h, LPARAM) { theme_control(h); return TRUE; }
void theme_controls(HWND parent) { EnumChildWindows(parent, enum_child, 0); }

bool theme_ctlcolor(UINT msg, WPARAM w, LPARAM l, LRESULT* res) {
    (void)l;
    HDC dc = (HDC)w;
    switch (msg) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor(dc, g_pal.text);
        SetBkColor(dc, g_pal.bg);
        *res = (LRESULT)g_bg;
        return true;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(dc, g_pal.text);
        SetBkColor(dc, g_pal.edit_bg);
        *res = (LRESULT)g_edit;
        return true;
    }
    return false;
}

bool theme_menubar_message(HWND hwnd, UINT msg, WPARAM w, LPARAM l, LRESULT* res) {
    if (!g_dark) return false;
    if (msg == WM_UAHDRAWMENU) {
        UAHMENU* um = (UAHMENU*)l;
        MENUBARINFO mbi{};
        mbi.cbSize = sizeof mbi;
        GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi);
        RECT rw;
        GetWindowRect(hwnd, &rw);
        RECT rc = mbi.rcBar;
        OffsetRect(&rc, -rw.left, -rw.top);
        HBRUSH b = CreateSolidBrush(g_pal.menu_bg);
        FillRect(um->hdc, &rc, b);
        DeleteObject(b);
        *res = 1;
        return true;
    }
    if (msg == WM_UAHDRAWMENUITEM) {
        UAHDRAWMENUITEM* di = (UAHDRAWMENUITEM*)l;
        wchar_t buf[256] = L"";
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof mii;
        mii.fMask = MIIM_STRING;
        mii.dwTypeData = buf;
        mii.cch = 255;
        GetMenuItemInfoW(di->um.hmenu, (UINT)di->umi.iPosition, TRUE, &mii);
        const bool hot = (di->dis.itemState & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
        HBRUSH b = CreateSolidBrush(hot ? g_pal.panel_hover : g_pal.menu_bg);
        FillRect(di->um.hdc, &di->dis.rcItem, b);
        DeleteObject(b);
        SetBkMode(di->um.hdc, TRANSPARENT);
        SetTextColor(di->um.hdc, (di->dis.itemState & ODS_GRAYED) ? g_pal.text_dim : g_pal.text);
        UINT f = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
        if (di->dis.itemState & ODS_NOACCEL) f |= DT_HIDEPREFIX;
        DrawTextW(di->um.hdc, buf, -1, &di->dis.rcItem, f);
        *res = 1;
        return true;
    }
    if (msg == WM_NCPAINT || msg == WM_NCACTIVATE) {
        *res = DefWindowProcW(hwnd, msg, w, l);
        MENUBARINFO mbi{};
        mbi.cbSize = sizeof mbi;
        if (GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) {
            RECT rw;
            GetWindowRect(hwnd, &rw);
            RECT rc = mbi.rcBar;
            OffsetRect(&rc, -rw.left, -rw.top);
            rc.top = rc.bottom;
            rc.bottom += 1;
            HDC dc = GetWindowDC(hwnd);
            HBRUSH b = CreateSolidBrush(g_pal.menu_bg);
            FillRect(dc, &rc, b);
            DeleteObject(b);
            ReleaseDC(hwnd, dc);
        }
        return true;
    }
    return false;
}

} // namespace pcbp::ui
