#pragma once
// ============================================================================
// form.h — declarative modal dialogs (themed). Every settings / edit dialog in
// PCB Plotter is a Form: a list of typed fields laid out automatically, with
// optional tab pages, extra buttons and change callbacks.
// ============================================================================
#include "win.h"
#include <functional>
#include <utility>

namespace pcbp::ui {

class Form;

struct Field {
    enum class Type { Heading, Label, Text, Number, Check, Combo, ComboEdit, File, SaveFile, Folder, Color,
                      Multiline, Button, List, Separator, Custom };
    Type type = Type::Text;
    std::string key;
    std::wstring label;
    std::wstring value;                     // text / number / combo text / path; Check: L"1"/L"0"; Color: L"#AARRGGBB"
    std::vector<std::wstring> choices;      // Combo / ComboEdit
    std::vector<FileFilter> filters;        // File / SaveFile
    std::wstring tip;
    std::wstring page;                      // tab page name ("" = single page)
    bool readonly = false;
    bool full_width = false;                // spans both columns
    int rows = 1;                           // Multiline / List height in text rows
    int decimals = 4;
    std::function<void(Form&)> on_click;    // Button
    std::function<void(Form&, const std::string& key)> on_change;
    // List
    std::vector<std::wstring> columns;
    std::vector<std::vector<std::wstring>> list_rows;
    bool list_check = false;                // checkboxes in list
    // runtime
    bool enabled = true;
    int list_sel = -1;
    std::vector<bool> list_checked;
    std::string buf;                        // UTF-8 edit buffer
    std::function<void(Form&, float)> custom_draw;
    float custom_h = 0;
};

class Form {
public:
    explicit Form(std::wstring title, int width_dip = 420) : title_(std::move(title)), width_(width_dip) {}
    ~Form();

    // builders (return *this for chaining)
    Form& heading(const std::wstring& text, const std::wstring& page = L"");
    Form& label(const std::wstring& text, const std::wstring& page = L"");
    Form& text(const std::string& key, const std::wstring& label, const std::wstring& value, const std::wstring& page = L"");
    Form& number(const std::string& key, const std::wstring& label, double value, int decimals = 4, const std::wstring& page = L"");
    Form& check(const std::string& key, const std::wstring& label, bool value, const std::wstring& page = L"");
    Form& combo(const std::string& key, const std::wstring& label, const std::vector<std::wstring>& choices,
                const std::wstring& value, bool editable = false, const std::wstring& page = L"");
    Form& file(const std::string& key, const std::wstring& label, const std::wstring& value,
               const std::vector<FileFilter>& filters, bool save = false, const std::wstring& page = L"");
    Form& folder(const std::string& key, const std::wstring& label, const std::wstring& value, const std::wstring& page = L"");
    Form& color(const std::string& key, const std::wstring& label, uint32_t argb, const std::wstring& page = L"");
    Form& multiline(const std::string& key, const std::wstring& label, const std::wstring& value, int rows = 6, const std::wstring& page = L"");
    Form& button(const std::string& key, const std::wstring& label, std::function<void(Form&)> fn, const std::wstring& page = L"");
    Form& list(const std::string& key, const std::vector<std::wstring>& cols, const std::vector<std::vector<std::wstring>>& rows,
               int visible_rows = 8, bool checks = false, const std::wstring& page = L"");
    Form& separator(const std::wstring& page = L"");
    // custom ImGui content drawn in the flow (pictograms, previews). fn(form, available width)
    Form& custom(const std::string& key, std::function<void(Form&, float)> fn, float height_dip = 0, const std::wstring& page = L"");
    Form& readonly(bool r = true) { if (!fields_.empty()) fields_.back().readonly = r; return *this; }
    Form& wide(bool w = true) { if (!fields_.empty()) fields_.back().full_width = w; return *this; }
    Form& tip(const std::wstring& t) { if (!fields_.empty()) fields_.back().tip = t; return *this; }
    Form& on_change(std::function<void(Form&, const std::string&)> f) { if (!fields_.empty()) fields_.back().on_change = std::move(f); return *this; }
    Form& two_columns(bool v = true) { two_col_ = v; return *this; }
    Form& ok_text(const std::wstring& t) { ok_text_ = t; return *this; }
    Form& no_cancel() { cancel_ = false; return *this; }
    Form& cancel_text(const std::wstring& t) { cancel_text_ = t; return *this; }
    Form& extra_button(const std::wstring& text, std::function<bool(Form&)> fn);   // fn returns true to close with OK
    Form& extras_right(bool v = true) { extras_right_ = v; return *this; }   // extra buttons sit right-aligned next to OK (wizard Back / Next)
    Form& validate(std::function<bool(Form&, std::wstring& err)> v) { validate_ = std::move(v); return *this; }
    Form& header(const std::wstring& text) { header_ = text; return *this; }
    Form& icon(int ui_icon) { icon_ = ui_icon; return *this; }     // ui::Icon shown in header
    Form& subtitle(const std::wstring& t) { subtitle_ = t; return *this; }   // dim line under the header
    Form& danger(bool d = true) { danger_ = d; return *this; }  // OK button drawn as destructive
    Form& default_cancel(bool d = true) { default_cancel_ = d; return *this; }   // R21: Enter = Cancel, Cancel drawn as the primary button

    bool run(HWND owner);    // modal (nested frame loop); true = OK

    // values (valid during and after run)
    std::wstring str(const std::string& key) const;
    std::string u8(const std::string& key) const { return U(str(key)); }
    double num(const std::string& key, double def = 0) const;
    bool checked(const std::string& key) const;
    int index(const std::string& key) const;
    uint32_t color_value(const std::string& key) const;
    std::vector<bool> list_checks(const std::string& key) const;
    int list_selection(const std::string& key) const;
    // live updates
    void set(const std::string& key, const std::wstring& v);
    void set_list(const std::string& key, const std::vector<std::vector<std::wstring>>& rows);
    void set_list_columns(const std::string& key, const std::vector<std::wstring>& cols);
    void set_choices(const std::string& key, const std::vector<std::wstring>& choices);
    void enable(const std::string& key, bool en);
    Field* field(const std::string& key);
    void close(bool ok) { done_ = true; result_ = ok; }
    // run fn between frames (from custom ImGui content: nested dialogs, file pickers)
    void later(std::function<void()> fn) { pending_.push_back(std::move(fn)); }

    // ---- driven by the GUI shell (gui/gform.cpp) ----
    void draw_imgui();                 // inside the modal popup
    const std::wstring& title() const { return title_; }
    std::string popup_id() const;
    bool done() const { return done_; }
    void run_pending();                // executes queued callbacks between frames

private:
    friend struct FormAccess;
    std::wstring title_, header_, subtitle_;
    bool danger_ = false;
    bool default_cancel_ = false;
    int width_;
    int icon_ = 0;
    bool two_col_ = false, cancel_ = true;
    bool extras_right_ = false;
    std::wstring ok_text_ = L"OK", cancel_text_ = L"Cancel";
    std::vector<Field> fields_;
    std::vector<std::pair<std::wstring, std::function<bool(Form&)>>> extra_;
    std::function<bool(Form&, std::wstring&)> validate_;
    std::vector<std::wstring> pages_;
    int page_ = 0;
    bool done_ = false, result_ = false;
    std::vector<std::function<void()>> pending_;
    std::wstring error_;
    int serial_ = 0;
};

// Simple prompts
bool prompt_text(HWND owner, const std::wstring& title, const std::wstring& label, std::wstring& value);
bool prompt_number(HWND owner, const std::wstring& title, const std::wstring& label, double& value, int decimals = 4);
int prompt_choice(HWND owner, const std::wstring& title, const std::wstring& label, const std::vector<std::wstring>& choices, int sel = 0);
// themed message (info / question); returns true for OK/Yes
bool message(HWND owner, const std::wstring& title, const std::wstring& text, bool question = false);
// themed warning / error message (warning icon, single OK)
void warning(HWND owner, const std::wstring& title, const std::wstring& text);
// read-only text viewer (program output, reports)
void show_text(HWND owner, const std::wstring& title, const std::wstring& text, bool monospace = true);

} // namespace pcbp::ui
