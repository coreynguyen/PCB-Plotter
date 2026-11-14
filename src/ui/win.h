#pragma once
// ============================================================================
// win.h — common Win32 / Direct2D includes and tiny helpers for the UI layer.
// ============================================================================
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include "../core/fileio.h"

namespace pcbp::ui {

// --- COM smart pointer (minimal, no WRL dependency) ------------------------
template <class T>
class Com {
public:
    Com() = default;
    Com(const Com& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~Com() { reset(); }
    Com& operator=(const Com& o) { if (this != &o) { reset(); p_ = o.p_; if (p_) p_->AddRef(); } return *this; }
    Com& operator=(Com&& o) noexcept { if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; } return *this; }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T** put() { reset(); return &p_; }
    explicit operator bool() const { return p_ != nullptr; }
private:
    T* p_ = nullptr;
};

inline std::wstring W(const std::string& s) { return utf8_to_wide(s); }
inline std::string U(const std::wstring& s) { return wide_to_utf8(s); }

// DPI helpers -----------------------------------------------------------------
UINT window_dpi(HWND h);
inline int scale(int v, UINT dpi) { return MulDiv(v, (int)dpi, 96); }

// misc
std::wstring get_window_text(HWND h);
std::wstring fmt_num(double v, int decimals);
double parse_num(const std::wstring& s, double def = 0);
HFONT ui_font(UINT dpi, int pt_tenths = 90, bool bold = false);   // cached Segoe UI
HINSTANCE app_instance();

inline COLORREF argb_to_colorref(uint32_t c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }
inline uint32_t colorref_to_argb(COLORREF c) { return 0xFF000000u | ((uint32_t)GetRValue(c) << 16) | ((uint32_t)GetGValue(c) << 8) | GetBValue(c); }
inline D2D1_COLOR_F d2c(uint32_t argb, float alpha_mul = 1.0f) {
    return D2D1::ColorF(((argb >> 16) & 255) / 255.f, ((argb >> 8) & 255) / 255.f, (argb & 255) / 255.f,
                        ((argb >> 24) & 255) / 255.f * alpha_mul);
}

// file dialogs (filter = L"Name\0*.x;*.y\0...\0\0" style built from pairs)
struct FileFilter { std::wstring name, pattern; };
bool open_file_dialog(HWND owner, const std::wstring& title, const std::vector<FileFilter>& f,
                      std::wstring& path, bool multi = false, std::vector<std::wstring>* multi_out = nullptr);
bool save_file_dialog(HWND owner, const std::wstring& title, const std::vector<FileFilter>& f,
                      const std::wstring& def_ext, std::wstring& path);
bool pick_folder_dialog(HWND owner, const std::wstring& title, std::wstring& path);   // IFileOpenDialog + FOS_PICKFOLDERS
// R14: file picker with an extra "folder" push button (e.g. "Select ODB++ folder..."). Pressing it closes the
// file dialog and opens a FOS_PICKFOLDERS picker. Returns 0 = cancelled, 1 = file chosen, 2 = folder chosen.
int open_file_or_folder_dialog(HWND owner, const std::wstring& title, const std::vector<FileFilter>& f,
                               std::wstring& path, const std::wstring& folder_label, const std::wstring& folder_title);
bool pick_color_dialog(HWND owner, uint32_t& argb);
// R20 loop guard for the file dialogs above (core/dialog_guard.h): the app window reports real key-down /
// mouse-button-down messages; a cancelled dialog cannot reappear (same title) without such fresh input.
void dialog_note_input();
int dialog_blocked_count();

int msg_box(HWND owner, const std::wstring& text, const std::wstring& caption, UINT flags);

} // namespace pcbp::ui
