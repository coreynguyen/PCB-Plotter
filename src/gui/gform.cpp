// ============================================================================
// gform.cpp — ui::Form implemented with Dear ImGui modal popups.
// Form::run() spins nested frames (gui::pump_and_render) until closed, so the
// declarative blocking API used by the dialogs keeps working.
// ============================================================================
#include "../ui/form.h"
#include "gapp.h"
#include "imgui_internal.h"
#include <algorithm>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cmath>

namespace pcbp::gui {
bool pump_and_render();     // shell.cpp: one message pump + frame; false if WM_QUIT
HWND main_hwnd();
std::vector<ui::Form*>& modal_stack() { static std::vector<ui::Form*> s; return s; }
void form_report_natural(int serial, ImVec2 nat);
bool form_user_sized(int serial);
void form_toggle_maximize();

} // namespace pcbp::gui
namespace pcbp::ui { struct FormAccess { static int width(const Form& f) { return f.width_; } static int serial(const Form& f) { return f.serial_; } }; }
namespace pcbp::gui {

// P1-11: dialogs open centred on their parent (the main window, or the dialog that opened them). Optional:
// Appearance > Remember dialog positions (DlgPos.<title>).
// R13 (REVIEW_V12 P1-07/P1-08): every form is content-sized and resizable. Until the user resizes it the window
// height follows its content (header + body + pinned footer) up to the work area, then the body scrolls; the
// design width (Form(title, width_dip)) is the minimum width. Geometry is decided BEFORE Begin (never
// SetWindowPos inside the window: that moved the window after its background/clip rect were emitted and the
// frosted backdrop, which reads the final rect, split from the content). Dragging against the viewport edge clamps
// the position only; the size never changes.
// R17c (REVIEW_V16 §5): explicit geometry requests (wizard initial size, Maximize / Restore) go through the
// framework: a size set from inside the window was overwritten by the content-driven size on the next frame
// (vertically compressed wizards, Maximize buttons that did nothing). A request switches the form to
// user-sized; the size of a form the user resized is remembered per dialog title (DlgSize.<title>.w/.h)
// and restored, centred, the next time it opens. Double-clicking any dialog title bar maximizes / restores it.
struct DlgGeo {
    ImVec2 natural{0, 0}; ImVec2 asked{0, 0}; bool user = false; int frames = 0;
    bool req = false; ImVec2 req_pos{-1, -1}, req_size{0, 0};     // pending SetNextWindowPos/Size (pos < 0: centre)
    bool maxed = false; ImVec2 rest_pos{0, 0}, rest_size{0, 0};
    bool loaded = false, saved_size = false;
};
static std::map<int, DlgGeo>& geo_map() { static std::map<int, DlgGeo> m; return m; }
static int g_cur_serial = 0;   // the form whose content is being drawn (draw_modal)
void form_report_natural(int serial, ImVec2 nat) { geo_map()[serial].natural = nat; }
bool form_user_sized(int serial) { auto it = geo_map().find(serial); return it != geo_map().end() && it->second.user; }
// Initial size of the current form (centred) unless the user already sized this dialog before.
void form_initial_size(ImVec2 sz) {
    DlgGeo& g = geo_map()[g_cur_serial];
    if (g.saved_size || g.maxed) return;
    g.user = true; g.req = true; g.req_pos = ImVec2(-1, -1); g.req_size = sz;
    request_redraw(2);
}
static void form_toggle_maximize_of(int serial) { const int k = g_cur_serial; g_cur_serial = serial; form_toggle_maximize(); g_cur_serial = k; }
bool form_is_maximized() { auto it = geo_map().find(g_cur_serial); return it != geo_map().end() && it->second.maxed; }
void form_toggle_maximize() {
    DlgGeo& g = geo_map()[g_cur_serial];
    ImGuiWindow* w = ImGui::GetCurrentWindow() ? ImGui::GetCurrentWindow()->RootWindow : nullptr;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!g.maxed) {
        if (w) { g.rest_pos = w->Pos; g.rest_size = w->SizeFull; }
        g.maxed = true; g.req_pos = vp->WorkPos; g.req_size = vp->WorkSize;
    } else {
        g.maxed = false; g.req_pos = g.rest_pos; g.req_size = g.rest_size;
        if (g.req_size.x <= 0) g.req_pos = ImVec2(-1, -1), g.req_size = ImVec2(vp->WorkSize.x * 0.7f, vp->WorkSize.y * 0.7f);
    }
    g.user = true; g.req = true;
    request_redraw(2);
}

static void draw_modal(size_t i, ImRect parent) {
    auto& st = modal_stack();
    if (i >= st.size()) return;
    ui::Form* f = st[i];
    std::string id = f->popup_id();
    if (!ImGui::IsPopupOpen(id.c_str())) ImGui::OpenPopup(id.c_str());
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const int serial = ui::FormAccess::serial(*f);
    DlgGeo& g = geo_map()[serial];
    if (geo_map().size() > 64) for (auto it = geo_map().begin(); it != geo_map().end();) it = it->first < serial - 32 ? geo_map().erase(it) : std::next(it);
    const std::string pk = "DlgPos." + ui::U(f->title());
    const bool remember = app::ui_pref("DialogRememberPos", 0) != 0;
    const ImVec2 wa = vp->WorkPos, ws = vp->WorkSize;
    const float minw = std::min(S((float)ui::FormAccess::width(*f)) + S(4) + ImGui::GetStyle().ScrollbarSize + S(36), ws.x);
    const ImVec2 maxs(ws.x, ws.y);
    ImGuiWindow* pw = ImGui::FindWindowByName(id.c_str());
    const std::string sk = "DlgSize." + ui::U(f->title());
    if (!g.loaded) {   // R17c: restore the size the user gave this dialog last time (centred)
        g.loaded = true;
        const int sw = app::ui_pref((sk + ".w").c_str(), 0), sh = app::ui_pref((sk + ".h").c_str(), 0);
        if (sw > 0 && sh > 0) { g.saved_size = true; g.user = true; g.req = true; g.req_pos = ImVec2(-1, -1); g.req_size = ImVec2((float)sw, (float)sh); }
    }
    // size: content-driven until the user drags a border
    ImVec2 size = pw && g.frames > 0 ? pw->SizeFull : ImVec2(minw, S(160));
    bool req_now = false;
    if (g.req) {
        g.req = false; req_now = true;
        size = ImVec2(std::clamp(g.req_size.x, minw, maxs.x), std::clamp(g.req_size.y, S(80), maxs.y));
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    } else if (!g.user) {
        ImVec2 want(std::max(minw, g.natural.x), g.natural.y > 0 ? g.natural.y : size.y);
        size = ImVec2(std::clamp(want.x, minw, maxs.x), std::clamp(want.y, S(80), maxs.y));
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    } else size = ImVec2(std::clamp(size.x, minw, maxs.x), std::clamp(size.y, S(80), maxs.y));
    g.asked = size;
    ImGui::SetNextWindowSizeConstraints(ImVec2(minw, std::min(S(80), maxs.y)), maxs);
    // position: centred while the content settles, then only clamped into the work area
    const ImVec2 b(wa.x + ws.x, wa.y + ws.y);
    auto clampp = [&](ImVec2 p) { return ImVec2(std::floor(std::clamp(p.x, wa.x, std::max(wa.x, b.x - size.x))), std::floor(std::clamp(p.y, wa.y, std::max(wa.y, b.y - size.y)))); };
    if (req_now && g.req_pos.x >= 0) {
        ImGui::SetNextWindowPos(clampp(g.req_pos), ImGuiCond_Always);
    } else if (g.frames < 3 || req_now) {
        ImVec2 p(parent.GetCenter().x - size.x * 0.5f, parent.GetCenter().y - size.y * 0.5f);
        if (remember && app::ui_pref((pk + ".set").c_str(), 0)) p = ImVec2((float)app::ui_pref((pk + ".x").c_str(), 0), (float)app::ui_pref((pk + ".y").c_str(), 0));
        ImGui::SetNextWindowPos(clampp(p), ImGuiCond_Always);
    } else if (pw) {
        ImVec2 q = clampp(pw->Pos);   // pw->Pos already includes this frame's drag (ImGui moves windows in NewFrame)
        if (q.x != pw->Pos.x || q.y != pw->Pos.y) ImGui::SetNextWindowPos(q, ImGuiCond_Always);
    }
    // open animation: fade (alpha only on the modal's own window; dim bg fades with ImGui)
    static std::map<std::string, double> s_open_t;
    if (!s_open_t.count(id)) s_open_t[id] = ImGui::GetTime();
    float e = std::min(1.f, (float)(ImGui::GetTime() - s_open_t[id]) / 0.16f);
    e = 1 - (1 - e) * (1 - e);
    if (perf_mode()) e = 1;
    if (e < 1 || g.frames < 3) request_redraw(2);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(14)));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, (g.frames < 2 ? 0.01f : 0.25f + 0.75f * e));   // never 0: ImGui skips (hides) a window drawn at alpha 0
    bool open = true;
    static const bool s_trace = getenv("PCBP_FORM_TRACE") != nullptr;   // automation: geometry trace (debug output)
    if (s_trace) { char b[160]; std::snprintf(b, sizeof b, "form %s frames %d size %.0fx%.0f natural %.0fx%.0f user %d\n", id.c_str(), g.frames, size.x, size.y, g.natural.x, g.natural.y, (int)g.user); OutputDebugStringA(b); }
    if (ImGui::BeginPopupModal(id.c_str(), &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::PopStyleVar(2);
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        const bool resized = g.frames >= 3 && (std::fabs(w->SizeFull.x - g.asked.x) > 1.5f || std::fabs(w->SizeFull.y - g.asked.y) > 1.5f);
        if (resized) { g.user = true; g.maxed = false; }   // resize grip / border
        ++g.frames;
        // R17c: double-click the title bar = Maximize / Restore (every dialog); remember a user-given size
        const ImRect tbr = w->TitleBarRect();
        if (ImGui::IsMouseDoubleClicked(0) && tbr.Contains(ImGui::GetIO().MousePos) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) form_toggle_maximize_of(serial);
        if (g.user && !g.maxed && g.frames > 3 && ImGui::IsMouseReleased(0) && !req_now &&
            (app::ui_pref((sk + ".w").c_str(), 0) != (int)w->SizeFull.x || app::ui_pref((sk + ".h").c_str(), 0) != (int)w->SizeFull.y)) {
            app::set_ui_pref((sk + ".w").c_str(), (int)w->SizeFull.x); app::set_ui_pref((sk + ".h").c_str(), (int)w->SizeFull.y);
        }
        if (remember && g.frames > 3 && ImGui::IsMouseReleased(0)) {
            app::set_ui_pref((pk + ".x").c_str(), (int)w->Pos.x); app::set_ui_pref((pk + ".y").c_str(), (int)w->Pos.y); app::set_ui_pref((pk + ".set").c_str(), 1);
        }
        const ImRect me = w->Rect();
        const int prev_cur = g_cur_serial;
        g_cur_serial = serial;
        f->draw_imgui();
        g_cur_serial = prev_cur;
        if (!open) f->close(false);
        draw_modal(i + 1, me);
        ImGui::EndPopup();
    } else {
        ImGui::PopStyleVar(2);
        s_open_t.erase(id);
        if (!open) f->close(false);
    }
}

void draw_modals() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    draw_modal(0, ImRect(vp->WorkPos, ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y)));
}


} // namespace pcbp::gui

namespace pcbp::ui {
using namespace pcbp::gui;

static std::wstring color_text(uint32_t c) { wchar_t b[16]; std::swprintf(b, 16, L"#%08X", c); return b; }
static uint32_t parse_color(const std::wstring& s) {
    if (s.size() >= 2 && s[0] == L'#') return (uint32_t)std::wcstoul(s.c_str() + 1, nullptr, 16) | (s.size() <= 7 ? 0xFF000000u : 0u);
    return 0xFF808080;
}
static int g_serial = 0;

Form::~Form() {
    auto& st = modal_stack();
    st.erase(std::remove(st.begin(), st.end(), this), st.end());
}

std::string Form::popup_id() const { return U(title_) + "###form" + std::to_string(serial_); }

Form& Form::heading(const std::wstring& t, const std::wstring& page) { Field f; f.type = Field::Type::Heading; f.label = t; f.page = page; f.full_width = true; fields_.push_back(f); return *this; }
Form& Form::label(const std::wstring& t, const std::wstring& page) { Field f; f.type = Field::Type::Label; f.label = t; f.page = page; f.full_width = true; fields_.push_back(f); return *this; }
Form& Form::separator(const std::wstring& page) { Field f; f.type = Field::Type::Separator; f.page = page; f.full_width = true; fields_.push_back(f); return *this; }
Form& Form::text(const std::string& k, const std::wstring& l, const std::wstring& v, const std::wstring& p) { Field f; f.type = Field::Type::Text; f.key = k; f.label = l; f.value = v; f.page = p; fields_.push_back(f); return *this; }
Form& Form::number(const std::string& k, const std::wstring& l, double v, int d, const std::wstring& p) { Field f; f.type = Field::Type::Number; f.key = k; f.label = l; f.value = fmt_num(v, d); f.decimals = d; f.page = p; fields_.push_back(f); return *this; }
Form& Form::check(const std::string& k, const std::wstring& l, bool v, const std::wstring& p) { Field f; f.type = Field::Type::Check; f.key = k; f.label = l; f.value = v ? L"1" : L"0"; f.page = p; fields_.push_back(f); return *this; }
Form& Form::combo(const std::string& k, const std::wstring& l, const std::vector<std::wstring>& c, const std::wstring& v, bool e, const std::wstring& p) {
    Field f; f.type = e ? Field::Type::ComboEdit : Field::Type::Combo; f.key = k; f.label = l; f.choices = c; f.value = v; f.page = p;
    if (!e && !c.empty() && std::find(c.begin(), c.end(), v) == c.end()) f.value = c[0];
    fields_.push_back(f); return *this;
}
Form& Form::file(const std::string& k, const std::wstring& l, const std::wstring& v, const std::vector<FileFilter>& flt, bool save, const std::wstring& p) { Field f; f.type = save ? Field::Type::SaveFile : Field::Type::File; f.key = k; f.label = l; f.value = v; f.filters = flt; f.page = p; fields_.push_back(f); return *this; }
Form& Form::folder(const std::string& k, const std::wstring& l, const std::wstring& v, const std::wstring& p) { Field f; f.type = Field::Type::Folder; f.key = k; f.label = l; f.value = v; f.page = p; fields_.push_back(f); return *this; }
Form& Form::color(const std::string& k, const std::wstring& l, uint32_t c, const std::wstring& p) { Field f; f.type = Field::Type::Color; f.key = k; f.label = l; f.value = color_text(c); f.page = p; fields_.push_back(f); return *this; }
Form& Form::multiline(const std::string& k, const std::wstring& l, const std::wstring& v, int rows, const std::wstring& p) { Field f; f.type = Field::Type::Multiline; f.key = k; f.label = l; f.value = v; f.rows = rows; f.page = p; f.full_width = true; fields_.push_back(f); return *this; }
Form& Form::button(const std::string& k, const std::wstring& l, std::function<void(Form&)> fn, const std::wstring& p) { Field f; f.type = Field::Type::Button; f.key = k; f.label = l; f.on_click = std::move(fn); f.page = p; fields_.push_back(f); return *this; }
Form& Form::list(const std::string& k, const std::vector<std::wstring>& cols, const std::vector<std::vector<std::wstring>>& rows, int vis, bool checks, const std::wstring& p) {
    Field f; f.type = Field::Type::List; f.key = k; f.columns = cols; f.list_rows = rows; f.rows = vis; f.list_check = checks; f.page = p; f.full_width = true;
    f.list_checked.assign(rows.size(), true);
    fields_.push_back(f); return *this;
}
Form& Form::custom(const std::string& k, std::function<void(Form&, float)> fn, float h, const std::wstring& p) {
    Field f; f.type = Field::Type::Custom; f.key = k; f.custom_draw = std::move(fn); f.custom_h = h; f.page = p; f.full_width = true; fields_.push_back(f); return *this;
}
Form& Form::extra_button(const std::wstring& text, std::function<bool(Form&)> fn) { extra_.push_back({text, std::move(fn)}); return *this; }

Field* Form::field(const std::string& key) { for (auto& f : fields_) if (f.key == key) return &f; return nullptr; }
std::wstring Form::str(const std::string& key) const { for (auto& f : fields_) if (f.key == key) return f.value; return L""; }
double Form::num(const std::string& key, double def) const { return parse_num(str(key), def); }
bool Form::checked(const std::string& key) const { return str(key) == L"1"; }
int Form::index(const std::string& key) const {
    for (auto& f : fields_) if (f.key == key) for (size_t i = 0; i < f.choices.size(); ++i) if (f.choices[i] == f.value) return (int)i;
    return -1;
}
uint32_t Form::color_value(const std::string& key) const { return parse_color(str(key)); }
std::vector<bool> Form::list_checks(const std::string& key) const { for (auto& f : fields_) if (f.key == key) return f.list_checked; return {}; }
int Form::list_selection(const std::string& key) const { for (auto& f : fields_) if (f.key == key) return f.list_sel; return -1; }
void Form::set(const std::string& key, const std::wstring& v) { if (Field* f = field(key)) { f->value = v; f->buf.clear(); } }
void Form::set_list(const std::string& key, const std::vector<std::vector<std::wstring>>& rows) {
    if (Field* f = field(key)) { f->list_rows = rows; f->list_checked.assign(rows.size(), true); if (f->list_sel >= (int)rows.size()) f->list_sel = -1; }
}
void Form::set_list_columns(const std::string& key, const std::vector<std::wstring>& cols) { if (Field* f = field(key)) f->columns = cols; }
void Form::set_choices(const std::string& key, const std::vector<std::wstring>& c) { if (Field* f = field(key)) f->choices = c; }
void Form::enable(const std::string& key, bool en) { if (Field* f = field(key)) f->enabled = en; }

void Form::run_pending() {
    auto p = std::move(pending_);
    pending_.clear();
    for (auto& fn : p) fn();
}

// --------------------------------------------------------------------------
static bool edit_text(Field& f, bool multiline, float w, float h) {
    if (f.buf.empty() || U(f.value) != f.buf) { f.buf = U(f.value); }
    std::vector<char> b(f.buf.size() + 4096, 0);
    std::memcpy(b.data(), f.buf.data(), f.buf.size());
    ImGuiInputTextFlags fl = f.readonly ? ImGuiInputTextFlags_ReadOnly : 0;
    bool ch;
    std::string id = "##" + f.key + std::to_string((size_t)&f);
    if (multiline) ch = ImGui::InputTextMultiline(id.c_str(), b.data(), b.size(), ImVec2(w, h), fl);
    else {
        ImGui::SetNextItemWidth(w);
        if (f.type == Field::Type::Number) fl |= ImGuiInputTextFlags_CharsScientific;
        ch = ImGui::InputText(id.c_str(), b.data(), b.size(), fl);
    }
    if (ch) { f.buf = b.data(); f.value = W(f.buf); }
    return ch;
}

void Form::draw_imgui() {
    const GTheme& t = th();
    // R13 (P1-07): every form: body in a scrolling child sized to its content (reported to draw_modal, which
    // sizes the window up to the work area), footer pinned below it. Width: the design width, or the window
    // width once the user resized it.
    const bool responsive = gui::form_user_sized(serial_);
    const float sbw = ImGui::GetStyle().ScrollbarSize;
    const float W0 = responsive ? std::max(S(240), ImGui::GetContentRegionAvail().x - S(4) - sbw)
                                : std::min(S((float)width_), ImGui::GetMainViewport()->WorkSize.x - S(40) - S(4) - sbw);
    const float y_top = ImGui::GetCursorPosY();
    std::vector<std::string> changed;
    if (!header_.empty() || icon_) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float hh = S(subtitle_.empty() ? 40 : 46);
        float tx = 0;
        if (icon_) {
            const float bs = S(40);
            dl->AddRectFilled(p, ImVec2(p.x + bs, p.y + bs), (t.accent & 0x00FFFFFF) | 0x26000000, S(10));
            dl->AddRect(p, ImVec2(p.x + bs, p.y + bs), (t.accent & 0x00FFFFFF) | 0x50000000, S(10));
            icon_draw(dl, (Icon)icon_, ImVec2(p.x + S(7), p.y + S(7)), S(26));
            tx = bs + S(12);
        }
        std::string h8 = U(header_.empty() ? title_ : header_);
        ImGui::PushFont(g_font_bold);
        float ty = p.y + (subtitle_.empty() ? (S(40) - ImGui::GetFontSize()) * 0.5f : S(2));
        ImGui::PushClipRect(p, ImVec2(p.x + W0, p.y + hh), true);
        dl->AddText(ImVec2(p.x + tx, ty), t.text, h8.c_str());
        ImGui::PopFont();
        if (!subtitle_.empty()) dl->AddText(ImVec2(p.x + tx, ty + ImGui::GetFontSize() + S(4)), t.text_dim, U(subtitle_).c_str());
        ImGui::PopClipRect();
        ImGui::Dummy(ImVec2(W0, hh));
        ImGui::Dummy(ImVec2(0, S(2)));
    }
    // pages
    for (auto& f : fields_) if (!f.page.empty() && std::find(pages_.begin(), pages_.end(), f.page) == pages_.end()) pages_.push_back(f.page);
    std::wstring cur_page;
    if (!pages_.empty()) {
        if (ImGui::BeginTabBar("##pages")) {
            for (size_t i = 0; i < pages_.size(); ++i) {
                if (ImGui::BeginTabItem(U(pages_[i]).c_str())) { page_ = (int)i; ImGui::EndTabItem(); }
            }
            ImGui::EndTabBar();
        }
        cur_page = pages_[std::clamp(page_, 0, (int)pages_.size() - 1)];
    }
    static std::map<int, float> s_body_h;   // body content height measured last frame
    static std::map<int, float> s_foot_h;   // footer height (separator, error, buttons, bottom padding) measured last frame
    const float body_top = ImGui::GetCursorPosY();
    const float fh = s_foot_h.count(serial_) ? s_foot_h[serial_] : ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2 + S(10) + (error_.empty() ? 0.f : ImGui::GetTextLineHeightWithSpacing() * 2) + ImGui::GetStyle().WindowPadding.y;
    const float avail_body = std::max(S(60), ImGui::GetContentRegionAvail().y - fh);
    const float bh_nat = s_body_h.count(serial_) ? s_body_h[serial_] : 0.f;
    const float body_h = responsive ? avail_body : (bh_nat > 0 ? std::min(bh_nat, avail_body) : avail_body);
    ImGui::BeginChild("##formbody", ImVec2(W0 + S(4) + sbw, body_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    const bool body_child = true;
    int last_multi = -1;
    for (size_t k = 0; k < fields_.size(); ++k) if (fields_[k].type == Field::Type::Multiline && (pages_.empty() || fields_[k].page == cur_page)) last_multi = (int)k;
    const float label_w = S(two_col_ ? 120.f : 170.f);
    const float col_gap = S(18);
    const float colw = two_col_ ? (W0 - col_gap) * 0.5f : W0;
    int col = 0;
    for (size_t idx = 0; idx < fields_.size(); ++idx) {
        Field& f = fields_[idx];
        if (!pages_.empty() && f.page != cur_page) continue;
        ImGui::PushID((int)idx);
        const bool wide = f.full_width || !two_col_ || f.type == Field::Type::Heading;
        if (wide && col == 1) col = 0;
        if (!wide && col == 1) { ImGui::SameLine(colw + col_gap + ImGui::GetStyle().WindowPadding.x); }
        float cw = wide ? W0 : colw;
        ImGui::BeginDisabled(!f.enabled);
        ImGui::BeginGroup();
        auto lbl = [&]() {
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
            ImGui::TextUnformatted(U(f.label).c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(ImGui::GetCursorPosX() > 0 ? 0 : 0);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX());
        };
        auto at_ctl = [&]() {
            // place cursor at label column
            float x0 = ImGui::GetItemRectMin().x - ImGui::GetWindowPos().x;
            (void)x0;
        };
        (void)at_ctl;
        float ctl_x = (col == 1 ? colw + col_gap + ImGui::GetStyle().WindowPadding.x : ImGui::GetStyle().WindowPadding.x) + label_w;
        float ctl_w = cw - label_w;
        const float ctl_rel = label_w; // SameLine() inside BeginGroup is relative to the group origin
        switch (f.type) {
        case Field::Type::Heading: {
            ImGui::Dummy(ImVec2(0, S(4)));
            ImVec2 hp = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(hp.x, hp.y + S(2)), ImVec2(hp.x + S(3), hp.y + ImGui::GetFontSize() - S(1)), t.accent, S(2));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(10));
            ImGui::PushFont(g_font_bold);
            ImGui::TextUnformatted(U(f.label).c_str());
            ImGui::PopFont();
            float lx = ImGui::GetItemRectMax().x + S(10);
            float ly = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
            if (lx < hp.x + cw) ImGui::GetWindowDrawList()->AddLine(ImVec2(lx, ly), ImVec2(hp.x + cw, ly), t.border);
            break;
        }
        case Field::Type::Label:
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
            ImGui::TextUnformatted(U(f.label).c_str());
            ImGui::PopTextWrapPos();
            break;
        case Field::Type::Separator:
            ImGui::Dummy(ImVec2(cw, S(2)));
            ImGui::Separator();
            break;
        case Field::Type::Custom:
            if (f.custom_draw) f.custom_draw(*this, cw);
            if (f.custom_h > 0) ImGui::Dummy(ImVec2(0, 0));
            break;
        case Field::Type::Text: case Field::Type::Number:
            lbl(); ImGui::SameLine(ctl_rel);
            if (edit_text(f, false, ctl_w, 0)) changed.push_back(f.key);
            break;
        case Field::Type::Multiline:
            if (!f.label.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim); ImGui::TextUnformatted(U(f.label).c_str()); ImGui::PopStyleColor(); }
            {   // the last multi-line field fills a resized window (reports)
                float mh = ImGui::GetTextLineHeight() * f.rows + S(10);
                if (responsive && (int)idx == last_multi) mh = std::max(S(80), ImGui::GetContentRegionAvail().y - S(4));
                else if ((int)idx == last_multi) mh = std::min(mh, std::max(S(80), ImGui::GetMainViewport()->WorkSize.y * 0.6f));
                if (edit_text(f, true, cw, mh)) changed.push_back(f.key);
            }
            break;
        case Field::Type::Check: {
            if (!two_col_) ImGui::SetCursorPosX(ctl_x);
            bool v = f.value == L"1";
            if (ImGui::Checkbox(U(f.label).c_str(), &v)) { f.value = v ? L"1" : L"0"; changed.push_back(f.key); }
            break;
        }
        case Field::Type::Combo: case Field::Type::ComboEdit: {
            lbl(); ImGui::SameLine(ctl_rel);
            if (f.type == Field::Type::ComboEdit) {
                if (edit_text(f, false, ctl_w - ImGui::GetFrameHeight() - S(4), 0)) changed.push_back(f.key);
                ImGui::SameLine(0, S(4));
                if (ImGui::BeginCombo("##ce", "", ImGuiComboFlags_NoPreview)) {
                    for (auto& c : f.choices) if (ImGui::Selectable(U(c).c_str(), c == f.value)) { f.value = c; f.buf.clear(); changed.push_back(f.key); }
                    ImGui::EndCombo();
                }
            } else {
                ImGui::SetNextItemWidth(ctl_w);
                if (ImGui::BeginCombo("##c", U(f.value).c_str(), ImGuiComboFlags_HeightLarge)) {
                    for (auto& c : f.choices) if (ImGui::Selectable(U(c).c_str(), c == f.value)) { f.value = c; changed.push_back(f.key); }
                    ImGui::EndCombo();
                }
            }
            break;
        }
        case Field::Type::File: case Field::Type::SaveFile: case Field::Type::Folder: {
            lbl(); ImGui::SameLine(ctl_rel);
            if (edit_text(f, false, ctl_w - S(38), 0)) changed.push_back(f.key);
            ImGui::SameLine(0, S(4));
            if (ImGui::Button("...", ImVec2(S(34), 0))) {
                std::string key = f.key;
                pending_.push_back([this, key]() {
                    Field* ff = field(key);
                    if (!ff) return;
                    std::wstring p = ff->value;
                    bool ok = ff->type == Field::Type::Folder ? pick_folder_dialog(main_hwnd(), ff->label, p)
                            : ff->type == Field::Type::SaveFile ? save_file_dialog(main_hwnd(), ff->label, ff->filters, L"", p)
                            : open_file_dialog(main_hwnd(), ff->label, ff->filters, p);
                    if (ok) { ff->value = p; ff->buf.clear(); if (ff->on_change) ff->on_change(*this, key); }
                });
            }
            break;
        }
        case Field::Type::Color: {
            lbl(); ImGui::SameLine(ctl_rel);
            uint32_t c = parse_color(f.value);
            float col4[4] = {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, 1};
            if (ImGui::ColorEdit3("##col", col4, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                uint32_t n = 0xFF000000u | ((uint32_t)(col4[0] * 255 + .5f) << 16) | ((uint32_t)(col4[1] * 255 + .5f) << 8) | (uint32_t)(col4[2] * 255 + .5f);
                f.value = color_text(n);
                changed.push_back(f.key);
            }
            break;
        }
        case Field::Type::Button: {
            if (!two_col_) ImGui::SetCursorPosX(ctl_x);
            if (gui::button(U(f.label).c_str(), Icon::None, false, ImVec2(0, 0), f.enabled)) {
                std::string key = f.key;
                pending_.push_back([this, key]() { Field* ff = field(key); if (ff && ff->on_click) ff->on_click(*this); });
            }
            break;
        }
        case Field::Type::List: {
            const int nc = std::max(1, (int)f.columns.size()) + (f.list_check ? 1 : 0);
            float h = ImGui::GetTextLineHeightWithSpacing() * (f.rows + 1) + S(6);
            if (ImGui::BeginTable("##lst", nc, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                               ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(cw, h))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                if (f.list_check) ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(24));
                for (auto& c : f.columns) ImGui::TableSetupColumn(U(c).c_str());
                ImGui::TableHeadersRow();
                if (f.list_checked.size() != f.list_rows.size()) f.list_checked.assign(f.list_rows.size(), true);
                ImGuiListClipper clip;
                clip.Begin((int)f.list_rows.size());
                while (clip.Step()) {
                    for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                        ImGui::TableNextRow();
                        ImGui::PushID(r);
                        int c0 = 0;
                        if (f.list_check) {
                            ImGui::TableSetColumnIndex(0);
                            bool v = f.list_checked[r];
                            if (ImGui::Checkbox("##k", &v)) { f.list_checked[r] = v; changed.push_back(f.key); }
                            c0 = 1;
                        }
                        for (size_t c = 0; c < f.columns.size(); ++c) {
                            ImGui::TableSetColumnIndex((int)c + c0);
                            const std::string cell = c < f.list_rows[r].size() ? U(f.list_rows[r][c]) : std::string();
                            if (c == 0) {
                                if (ImGui::Selectable((cell + "##s").c_str(), f.list_sel == r, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                                    f.list_sel = r;
                                    changed.push_back(f.key);
                                }
                            } else ImGui::TextUnformatted(cell.c_str());
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            break;
        }
        }
        ImGui::EndGroup();
        ImGui::EndDisabled();
        if (!f.tip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", U(f.tip).c_str());
        ImGui::PopID();
        if (!wide && col == 0) col = 1;
        else col = 0;
    }
    if (body_child) {
        ImGuiWindow* cw = ImGui::GetCurrentWindow();
        s_body_h[serial_] = std::ceil(cw->DC.CursorMaxPos.y - cw->DC.CursorStartPos.y + S(6));   // + slack: no scrollbar for a pixel
        if (s_body_h.size() > 64) for (auto it = s_body_h.begin(); it != s_body_h.end();) it = it->first < serial_ - 32 ? s_body_h.erase(it) : std::next(it);
        ImGui::EndChild();
    }
    // footer (pinned: outside the scrolling body)
    const float foot_top = ImGui::GetCursorScreenPos().y;
    ImGui::Dummy(ImVec2(W0, S(4)));
    if (!error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, t.error);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + W0);
        ImGui::TextUnformatted(U(error_).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
    if (extras_right_ && !extra_.empty()) {   // R13: wizard layout [Back][Next][OK][Cancel] at bottom-right
        float ew = 0;
        for (auto& e : extra_) ew += ImGui::CalcTextSize(U(e.first).c_str()).x + ImGui::GetStyle().FramePadding.x * 2 + S(30) + ImGui::GetStyle().ItemSpacing.x;
        const float right0 = ImGui::GetStyle().WindowPadding.x + W0 - S(96) - (cancel_ ? S(96) + S(8) : 0);
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right0 - ew));
    }
    for (size_t i = 0; i < extra_.size(); ++i) {
        ImGui::PushID((int)i + 9000);
        if (gui::button(U(extra_[i].first).c_str())) {
            size_t k = i;
            pending_.push_back([this, k]() { if (extra_[k].second(*this)) { result_ = true; done_ = true; } });
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    // R21: buttons grow to fit their text ("Generate anyway" was clipped at 96 dip)
    const float bw = std::max({S(96), ImGui::CalcTextSize(U(ok_text_).c_str()).x + S(28), cancel_ ? ImGui::CalcTextSize(U(cancel_text_).c_str()).x + S(28) : 0.f});
    float right = ImGui::GetStyle().WindowPadding.x + W0;
    float x = right - bw - (cancel_ ? bw + S(8) : 0);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x));
    if (danger_) { ImGui::PushStyleColor(ImGuiCol_Button, t.error); }
    bool ok_pressed;
    if (danger_) {
        ImVec2 bp = ImGui::GetCursorScreenPos();
        ok_pressed = ImGui::InvisibleButton("##okd", ImVec2(bw, ImGui::GetFrameHeight() + S(2)));
        bool hv = ImGui::IsItemHovered();
        ImGui::GetWindowDrawList()->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + ImGui::GetFrameHeight() + S(2)), hv ? (t.error & 0x00FFFFFF) | 0xD0000000 : t.error, S(7));
        std::string ot = U(ok_text_);
        ImVec2 ts = ImGui::CalcTextSize(ot.c_str());
        ImGui::GetWindowDrawList()->AddText(ImVec2(bp.x + (bw - ts.x) * 0.5f, bp.y + (ImGui::GetFrameHeight() + S(2) - ts.y) * 0.5f), IM_COL32(255, 255, 255, 255), ot.c_str());
        ImGui::PopStyleColor();
    } else ok_pressed = gui::button(U(ok_text_).c_str(), Icon::None, !default_cancel_, ImVec2(bw, 0));
    {   // P1-08: Enter = default action (OK) unless the keyboard focus is on another button (then ImGui activates that
        // one, like Space does); Space activates the focused button (ImGui keyboard navigation).
        const ImGuiID ok_id = ImGui::GetItemID();
        ImGuiContext& g = *GImGui;
        const bool nav_other = g.NavCursorVisible && g.NavId != 0 && g.NavId != ok_id && g.NavWindow && g.NavWindow->RootWindow == ImGui::GetCurrentWindow()->RootWindow;
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const bool enter = (focused && !nav_other && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) ||
                           (focused && !nav_other && ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
        if (enter && default_cancel_ && cancel_) { result_ = false; done_ = true; }   // R21: Cancel is the default action
        else if (enter) ok_pressed = true;
    }
    if (ok_pressed) {
        pending_.push_back([this]() {
            error_.clear();
            if (validate_) { std::wstring e; if (!validate_(*this, e)) { error_ = e; return; } }
            result_ = true; done_ = true;
        });
    }
    if (cancel_) {
        ImGui::SameLine();
        if (gui::button(U(cancel_text_).c_str(), Icon::None, default_cancel_, ImVec2(bw, 0)) || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))) {
            result_ = false; done_ = true;
        }
    } else if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)) { result_ = false; done_ = true; }
    {   // natural window size = decorations + header + body content + footer
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        const float used_h = (win->DC.CursorMaxPos.y - win->Pos.y) - body_h + (s_body_h.count(serial_) ? s_body_h[serial_] : body_h) + ImGui::GetStyle().WindowPadding.y;
        gui::form_report_natural(serial_, ImVec2(S((float)width_) + S(4) + sbw + ImGui::GetStyle().WindowPadding.x * 2, std::ceil(used_h)));
        s_foot_h[serial_] = std::ceil(win->DC.CursorMaxPos.y - foot_top + ImGui::GetStyle().ItemSpacing.y);   // ContentRegionAvail already excludes the bottom padding
        if (s_foot_h.size() > 64) for (auto it = s_foot_h.begin(); it != s_foot_h.end();) it = it->first < serial_ - 32 ? s_foot_h.erase(it) : std::next(it);
        (void)y_top; (void)body_top;
    }
    for (auto& k : changed) {
        Field* fp = field(k);
        if (fp && fp->on_change) { std::string key = k; pending_.push_back([this, key]() { Field* ff = field(key); if (ff && ff->on_change) ff->on_change(*this, key); }); }
    }
}

bool Form::run(HWND) {
    // R12: a modal spins nested frames; started from inside a frame (draw code) ImGui::NewFrame() would run in the
    // middle of the outer frame and corrupt its window stack (the R11 BOM-import crash). Refuse instead of crashing;
    // callers must open dialogs from commands / gui::defer / Form::later.
    if (gui::in_frame()) {
        OutputDebugStringW((L"PCB Plotter: Form::run inside a frame refused: " + title_ + L"\n").c_str());
        gui::toast("Internal: dialog \"" + U(title_) + "\" opened from drawing code was skipped", gui::ToastKind::Error, 6.f);
        return false;
    }
    serial_ = ++g_serial;
    done_ = false;
    result_ = false;
    modal_stack().push_back(this);
    request_redraw(3);
    // automation: PCBP_TEST_FORM_AUTOOK=<seconds> presses OK (with validation) on every dialog after that time
    static const double s_auto = []() { const char* e = getenv("PCBP_TEST_FORM_AUTOOK"); return e ? atof(e) : -1.0; }();
    const ULONGLONG t0 = GetTickCount64();
    bool auto_done = false;
    while (!done_) {
        if (!pump_and_render()) { done_ = true; result_ = false; PostQuitMessage(0); break; }
        if (!pending_.empty()) { run_pending(); request_redraw(2); }
        if (s_auto >= 0 && !auto_done && !done_ && (GetTickCount64() - t0) / 1000.0 >= s_auto) {
            auto_done = true;
            pending_.push_back([this]() {
                error_.clear();
                if (validate_) { std::wstring e; if (!validate_(*this, e)) { error_ = e; return; } }
                result_ = true; done_ = true;
            });
            request_redraw(2);
        }
    }
    auto& st = modal_stack();
    st.erase(std::remove(st.begin(), st.end(), this), st.end());
    request_redraw(2);
    return result_;
}

// ---------------------------------------------------------------------------
bool prompt_text(HWND owner, const std::wstring& title, const std::wstring& label, std::wstring& value) {
    Form f(title, 380);
    f.text("v", label, value);
    if (!f.run(owner)) return false;
    value = f.str("v");
    return true;
}
bool prompt_number(HWND owner, const std::wstring& title, const std::wstring& label, double& value, int decimals) {
    Form f(title, 340);
    f.number("v", label, value, decimals);
    if (!f.run(owner)) return false;
    value = f.num("v", value);
    return true;
}
int prompt_choice(HWND owner, const std::wstring& title, const std::wstring& label, const std::vector<std::wstring>& choices, int sel) {
    Form f(title, 380);
    f.combo("v", label, choices, sel >= 0 && sel < (int)choices.size() ? choices[sel] : L"");
    if (!f.run(owner)) return -1;
    return f.index("v");
}
static void add_msg_body(Form& f, Icon ic, const std::wstring& text) {
    std::string body = U(text);
    f.custom("msg", [ic, body](Form&, float w) {
        const GTheme& t = th();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float bs = S(44);
        ImU32 tint = ic == Icon::Warning ? t.warn : t.accent;
        dl->AddRectFilled(p, ImVec2(p.x + bs, p.y + bs), (tint & 0x00FFFFFF) | 0x28000000, S(12));
        icon_draw(dl, ic, ImVec2(p.x + S(8), p.y + S(8)), S(28));
        ImGui::SetCursorScreenPos(ImVec2(p.x + bs + S(14), p.y + S(2)));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - bs - S(14));
        // first line bold (CircuitCAM-style "question" headline) when the text has several paragraphs
        size_t nl = body.find('\n');
        if (nl != std::string::npos && nl < 120) {
            ImGui::PushFont(g_font_bold); ImGui::TextUnformatted(body.substr(0, nl).c_str()); ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
            std::string rest = body.substr(nl + 1);
            while (!rest.empty() && rest[0] == '\n') rest.erase(0, 1);
            ImGui::TextUnformatted(rest.c_str());
            ImGui::PopStyleColor();
        } else ImGui::TextUnformatted(body.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        float used = ImGui::GetItemRectMax().y - p.y;
        if (used < bs) ImGui::Dummy(ImVec2(0, bs - used - ImGui::GetStyle().ItemSpacing.y));
    });
}
static bool message_box(HWND owner, const std::wstring& title, const std::wstring& text, Icon ic, bool question, bool danger) {
    Form f(title, 460);
    add_msg_body(f, ic, text);
    if (question) { f.ok_text(L"Yes").cancel_text(L"No"); }
    else f.no_cancel();
    if (danger) f.danger();
    return f.run(owner);
}
bool message(HWND owner, const std::wstring& title, const std::wstring& text, bool question) {
    bool warnish = text.find(L"failed") != std::wstring::npos || text.find(L"Could not") != std::wstring::npos ||
                   text.find(L"Cannot") != std::wstring::npos || text.find(L"Delete") == 0 || text.find(L"Replace") == 0;
    Icon ic = question ? (warnish ? Icon::Warning : Icon::Question) : (warnish ? Icon::Warning : Icon::About);
    return message_box(owner, title, text, ic, question, question && warnish);
}
// themed replacement for MessageBoxW (ui::msg_box routes here once ImGui is up)
int themed_msg_box(HWND owner, const std::wstring& text, const std::wstring& caption, UINT flags) {
    if (!ImGui::GetCurrentContext()) return MessageBoxW(owner, text.c_str(), caption.c_str(), flags);
    const UINT kind = flags & MB_TYPEMASK, icf = flags & MB_ICONMASK;
    Icon ic = (icf == MB_ICONWARNING || icf == MB_ICONERROR) ? Icon::Warning : icf == MB_ICONQUESTION ? Icon::Question : Icon::About;
    Form f(caption, 480);
    add_msg_body(f, ic, text);
    int extra = 0;
    switch (kind) {
    case MB_YESNOCANCEL:
        f.ok_text(L"Yes").cancel_text(L"Cancel");
        f.extra_button(L"No", [&](Form&) { extra = IDNO; return true; });
        break;
    case MB_YESNO: f.ok_text(L"Yes").cancel_text(L"No"); break;
    case MB_OKCANCEL: f.ok_text(L"OK").cancel_text(L"Cancel"); break;
    case MB_RETRYCANCEL: f.ok_text(L"Retry").cancel_text(L"Cancel"); break;
    default: f.no_cancel(); break;
    }
    bool ok = f.run(owner);
    if (extra) return extra;
    switch (kind) {
    case MB_YESNOCANCEL: return ok ? IDYES : IDCANCEL;
    case MB_YESNO: return ok ? IDYES : IDNO;
    case MB_OKCANCEL: return ok ? IDOK : IDCANCEL;
    case MB_RETRYCANCEL: return ok ? IDRETRY : IDCANCEL;
    default: return IDOK;
    }
}
void warning(HWND owner, const std::wstring& title, const std::wstring& text) { message_box(owner, title, text, Icon::Warning, false, false); }
void show_text(HWND owner, const std::wstring& title, const std::wstring& text, bool monospace) {
    (void)monospace;
    Form f(title, 780);
    f.icon((int)Icon::Report).header(title);
    f.multiline("t", L"", text, 26).readonly();
    f.no_cancel().ok_text(L"Close");
    f.extra_button(L"Copy", [](Form& ff) { ImGui::SetClipboardText(ff.u8("t").c_str()); return false; });
    f.run(owner);
}

} // namespace pcbp::ui
