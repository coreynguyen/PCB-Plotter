// ============================================================================
// hex_viewer.cpp — safe inspector for unknown / binary files (REVIEW_V9 §15).
// Identifies common signatures, shows a virtualised hex + ASCII dump or the
// printable strings, and offers "Open as text" / "Save copy". The content is
// never interpreted or executed.
// ============================================================================
#include "file_editors.h"
#include "../app/app.h"
#include "../app/commands.h"
#include "../core/fileio.h"
#include "../ui/win.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cstring>

namespace pcbp::gui {

namespace {

std::string identify(const std::vector<uint8_t>& d) {
    auto at = [&](size_t o, const char* s, size_t n) { return d.size() >= o + n && std::memcmp(d.data() + o, s, n) == 0; };
    if (d.empty()) return "Empty file";
    if (at(0, "MZ", 2)) return "Windows executable / DLL (PE) \xE2\x80\x94 not run";
    if (at(0, "\x7F" "ELF", 4)) return "ELF executable \xE2\x80\x94 not run";
    if (at(0, "%PDF", 4)) return "PDF document";
    if (at(0, "PK\x03\x04", 4)) return "ZIP archive (or Office Open XML package)";
    if (at(0, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8)) return "OLE2 compound file (Office 97-2003 / Outlook / other)";
    if (at(0, "\x89PNG", 4)) return "PNG image";
    if (at(0, "\xFF\xD8\xFF", 3)) return "JPEG image";
    if (at(0, "GIF8", 4)) return "GIF image";
    if (at(0, "BM", 2) && d.size() > 14) return "BMP image (probable)";
    if (at(0, "\x1F\x8B", 2)) return "gzip compressed data";
    if (at(0, "7z\xBC\xAF\x27\x1C", 6)) return "7-Zip archive";
    if (at(0, "Rar!", 4)) return "RAR archive";
    if (at(0, "{\\rtf", 5)) return "RTF document";
    if (at(0, "Standard Jet DB", 15) || at(4, "Standard Jet DB", 15)) return "Access / Jet database (.mdb)";
    if (at(0, "Standard ACE DB", 15) || at(4, "Standard ACE DB", 15)) return "Access ACE database (.accdb)";
    if (at(0, "SQLite format 3", 15)) return "SQLite database";
    if (d.size() >= 262 && at(257, "ustar", 5)) return "tar archive";
    if (at(0, "II*\0", 4) || at(0, "MM\0*", 4)) return "TIFF image";
    return "Unrecognised binary data";
}

class HexDoc final : public FileDoc {
public:
    std::vector<uint8_t> data;
    std::string what;
    int mode = 0;                       // 0 hex, 1 strings
    int width = 16;
    std::vector<std::pair<size_t, std::string>> strings;
    bool strings_built = false;

    bool is_sheet() const override { return false; }
    bool dirty() const override { return false; }
    const char* kind() const override { return "Binary"; }
    ui::Icon icon() const override { return ui::Icon::Query; }
    std::string status() const override { return what + "  \xC2\xB7  " + std::to_string(data.size()) + " bytes  \xC2\xB7  shown as data only"; }

    void build_strings() {
        strings_built = true;
        strings.clear();
        std::string cur;
        size_t start = 0;
        for (size_t i = 0; i <= data.size(); ++i) {
            uint8_t c = i < data.size() ? data[i] : 0;
            bool pr = (c >= 32 && c < 127) || c == '\t';
            if (pr) { if (cur.empty()) start = i; cur += (char)c; }
            else {
                if (cur.size() >= 4) strings.emplace_back(start, cur);
                cur.clear();
            }
            if (strings.size() > 200000) break;
        }
        // UTF-16LE strings (Windows binaries / OLE)
        cur.clear();
        for (size_t i = 0; i + 1 < data.size() && strings.size() < 400000; i += 2) {
            uint8_t c = data[i], h = data[i + 1];
            if (h == 0 && c >= 32 && c < 127) { if (cur.empty()) start = i; cur += (char)c; }
            else { if (cur.size() >= 6) strings.emplace_back(start, cur + "  (UTF-16)"); cur.clear(); }
        }
        std::sort(strings.begin(), strings.end());
    }

    void draw(ImVec2 size) override {
        const GTheme& t = th();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + S(38)), t.panel);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + S(8), p0.y + S(9)));
        icon_image(ui::Icon::Query, S(20));
        ImGui::SameLine();
        ImGui::TextUnformatted(what.c_str());
        ImGui::SameLine(0, S(16));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(3));
        if (ImGui::RadioButton("Hex", mode == 0)) mode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Text strings", mode == 1)) { mode = 1; if (!strings_built) build_strings(); }
        ImGui::SameLine(0, S(16));
        if (mode == 0) {
            ImGui::SetNextItemWidth(S(70));
            if (ImGui::BeginCombo("##w", std::to_string(width).c_str())) {
                for (int w : {8, 16, 24, 32}) if (ImGui::Selectable(std::to_string(w).c_str(), w == width)) width = w;
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) tooltip("Bytes per row");
            ImGui::SameLine();
        }
        if (ImGui::Button("Open as text")) {
            std::string s(data.begin(), data.end());
            std::string n = name;
            defer([s, n]() { files_add(make_text_doc_from(s, n)); });
        }
        if (ImGui::IsItemHovered()) tooltip("Force the bytes into the Text editor (may show garbage)");
        ImGui::SameLine();
        if (ImGui::Button("Save copy\xE2\x80\xA6")) defer([this]() { save(true); });
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + S(38)));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.field);
        ImGui::BeginChild("##hex", ImVec2(size.x, size.y - S(38)), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PopStyleColor();
        ImGui::PushFont(g_font_mono);
        ImGuiListClipper clip;
        if (mode == 0) {
            int rows = (int)((data.size() + width - 1) / width);
            clip.Begin(rows);
            std::string line;
            while (clip.Step())
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                    char b[16];
                    size_t o = (size_t)r * width;
                    std::snprintf(b, sizeof b, "%08zX  ", o);
                    line = b;
                    std::string asc;
                    for (int k = 0; k < width; ++k) {
                        if (o + k < data.size()) { std::snprintf(b, sizeof b, "%02X ", data[o + k]); line += b; uint8_t c = data[o + k]; asc += (c >= 32 && c < 127) ? (char)c : '.'; }
                        else line += "   ";
                        if (k % 8 == 7) line += ' ';
                    }
                    ImGui::TextUnformatted(line.c_str());
                    ImGui::SameLine(0, 0);
                    ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
                    ImGui::TextUnformatted(asc.c_str());
                    ImGui::PopStyleColor();
                }
        } else {
            clip.Begin((int)strings.size());
            while (clip.Step())
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                    ImGui::TextDisabled("%08zX", strings[r].first);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(strings[r].second.c_str());
                }
            if (strings.empty()) ImGui::TextDisabled("No printable strings of 4+ characters.");
        }
        ImGui::PopFont();
        ImGui::EndChild();
    }
    bool save(bool) override {
        std::wstring p = ui::W(name);
        if (!ui::save_file_dialog(app::frame_hwnd(), L"Save Copy", {{L"All files", L"*.*"}}, L"", p)) return false;
        std::string err;
        if (!write_file(ui::U(p), data.data(), data.size(), err)) { toast("Save failed: " + err, ToastKind::Error, 6.f); return false; }
        gui::status("Saved " + ui::U(file_name_of(p)), ToastKind::Success);
        return true;
    }
};

} // namespace

std::unique_ptr<FileDoc> make_hex_doc_from(std::vector<uint8_t> data, const std::string& name) {
    auto d = std::make_unique<HexDoc>();
    d->what = identify(data);
    d->data = std::move(data);
    d->name = name;
    return d;
}

std::unique_ptr<FileDoc> make_hex_doc(const std::wstring& path, std::string& err) {
    std::vector<uint8_t> data;
    if (!read_file(ui::U(path), data, err)) return nullptr;
    auto d = make_hex_doc_from(std::move(data), ui::U(file_name_of(path)));
    d->path = path;
    return d;
}

} // namespace pcbp::gui
