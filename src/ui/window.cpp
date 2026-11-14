// ============================================================================
// window.cpp — Window base implementation.
// ============================================================================
#include "window.h"
#include "theme.h"

namespace pcbp::ui {

Window::~Window() {
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        HWND h = hwnd_;
        hwnd_ = nullptr;
        if (IsWindow(h)) DestroyWindow(h);
    }
}

void Window::register_class(const wchar_t* cls, UINT style, HBRUSH bg, HICON icon, HCURSOR cursor) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    if (GetClassInfoExW(app_instance(), cls, &wc)) return;
    wc.cbSize = sizeof wc;
    wc.style = style;
    wc.lpfnWndProc = proc;
    wc.hInstance = app_instance();
    wc.hCursor = cursor ? cursor : LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = bg;
    wc.hIcon = icon;
    wc.hIconSm = icon;
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
}

Window* Window::from(HWND h) { return h ? (Window*)GetWindowLongPtrW(h, GWLP_USERDATA) : nullptr; }

HWND Window::create(const wchar_t* cls, const wchar_t* title, DWORD style, DWORD ex, int x, int y, int w, int h,
                    HWND parent, HMENU menu) {
    CreateWindowExW(ex, cls, title, style, x, y, w, h, parent, menu, app_instance(), this);
    return hwnd_;
}

HWND Window::create_mdi_child(HWND client, const wchar_t* cls, const wchar_t* title, DWORD style) {
    mdi_ = true;
    MDICREATESTRUCTW mc{};
    mc.szClass = cls;
    mc.szTitle = title;
    mc.hOwner = app_instance();
    mc.x = mc.y = mc.cx = mc.cy = CW_USEDEFAULT;
    mc.style = style;
    mc.lParam = (LPARAM)this;
    SendMessageW(client, WM_MDICREATE, 0, (LPARAM)&mc);
    return hwnd_;
}

LRESULT Window::def(UINT msg, WPARAM w, LPARAM l) {
    return mdi_ ? DefMDIChildProcW(hwnd_, msg, w, l) : DefWindowProcW(hwnd_, msg, w, l);
}

LRESULT Window::handle(UINT msg, WPARAM w, LPARAM l) { return def(msg, w, l); }

LRESULT CALLBACK Window::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Window* self = from(h);
    if (m == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)l;
        void* param = cs->lpCreateParams;
        if (cs->dwExStyle & WS_EX_MDICHILD) {
            MDICREATESTRUCTW* mc = (MDICREATESTRUCTW*)param;
            param = (void*)mc->lParam;
        }
        self = (Window*)param;
        if (self) {
            self->hwnd_ = h;
            self->dpi_ = window_dpi(h);
            if (cs->dwExStyle & WS_EX_MDICHILD) self->mdi_ = true;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        }
    }
    if (!self) return DefWindowProcW(h, m, w, l);
    if (m == WM_DPICHANGED) self->dpi_ = HIWORD(w);
    LRESULT r = self->handle(m, w, l);
    if (m == WM_NCDESTROY) {
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        self->on_destroyed();
    }
    return r;
}

} // namespace pcbp::ui
