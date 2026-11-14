#pragma once
// ============================================================================
// window.h — thin C++ wrapper over Win32 windows (one WndProc, virtual handler).
// ============================================================================
#include "win.h"

namespace pcbp::ui {

class Window {
public:
    virtual ~Window();
    HWND hwnd() const { return hwnd_; }
    // Creates a window of a class registered by register_class(); `this` is bound to the HWND.
    HWND create(const wchar_t* cls, const wchar_t* title, DWORD style, DWORD ex_style,
                int x, int y, int w, int h, HWND parent, HMENU id_or_menu = nullptr);
    // Creates an MDI child through the MDI client (WM_MDICREATE)
    HWND create_mdi_child(HWND mdi_client, const wchar_t* cls, const wchar_t* title, DWORD style = 0);
    static void register_class(const wchar_t* cls, UINT style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
                               HBRUSH bg = nullptr, HICON icon = nullptr, HCURSOR cursor = nullptr);
    static Window* from(HWND h);
    UINT dpi() const { return dpi_; }
    int S(int v) const { return scale(v, dpi_); }
    void invalidate() const { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    bool mdi_child() const { return mdi_; }

protected:
    virtual LRESULT handle(UINT msg, WPARAM w, LPARAM l);
    LRESULT def(UINT msg, WPARAM w, LPARAM l);
    virtual void on_destroyed() {}          // after WM_NCDESTROY; may `delete this`
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    bool mdi_ = false;
    bool delete_on_destroy_ = false;

private:
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l);
};

// app-wide private messages
enum : UINT {
    WM_APP_DOCCHANGED = WM_APP + 1,   // wParam = change flags
    WM_APP_STATUS = WM_APP + 2,       // lParam = const wchar_t* (coords text), wParam = pane
    WM_APP_TOOLCHANGED = WM_APP + 3,
    WM_APP_POINTSEL = WM_APP + 4,     // factory model selection changed
};

enum ChangeFlags : unsigned {
    CH_GEOMETRY = 1,      // components / packages moved / edited
    CH_SELECTION = 2,
    CH_ASSIGN = 4,
    CH_LAYERS = 8,
    CH_MODEL = 16,
    CH_INFO = 32,
    CH_ALL = 0xFFFF
};

} // namespace pcbp::ui
