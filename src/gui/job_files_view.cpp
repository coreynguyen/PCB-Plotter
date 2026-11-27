// ============================================================================
// job_files_view.cpp — Files tab: the project's job archive "<Project Name>.zip" (specs/DOCUMENTS.md).
//
// R22 (specs/REVIEW_V21.md §12): a plain job-document organiser.
//   * Categories = top folders of the archive: Original Circuit, Panel Gerbers, Stencil, Schematic, Drawing, Image,
//     Email / Correspondence, Notes, Other (core/job_archive.h). Sidebar: All files + the categories (colour chip,
//     icon, count; each one is a drop target) + the custom tags in use (filter).
//   * List: Name / Category / Size / Modified / Tags (click a header to sort), F2 renames in place, multi-select,
//     context menu with Change category. Folders (extracted Gerber directories) and zips open in place, read-only
//     for zips, with a back bar.
//   * Adding: a visible drop zone ("Drop files to add to <category>"), Add files..., drops on a sidebar category.
//     Every add goes through the confirm sheet: each file / folder with its proposed category (Gerber sets ask
//     Original / Panel / Stencil from file names, paste-only layers and repeated outlines) in an editable dropdown.
//   * Legacy archives (R19-R21 folder names): the index migrates silently; renaming the folders inside the zip is
//     offered in a banner and only done on the user's OK.
//   * Details pane: category chip + Change category, tags, note, Preview / Extract / Open externally / Import as,
//     inline image preview. Double-click / Enter previews in the built-in modal (file_preview.cpp).
// The archive itself is core/job_archive (plain zip, atomic rewrite, one .bak, Zip64 when needed); the
// project stores only the index (Project::documents).
// Registered through R19a's register_view_factory hook (no shell.cpp tab code here).
// ============================================================================
#include "gapp.h"
#include "gfx.h"
#include "file_preview.h"
#include "../core/job_archive.h"
#include "../core/aoi_align.h"
#include "../io/program_view.h"
#include "../core/fileio.h"
#include "../ui/form.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <shellapi.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <cctype>

namespace pcbp::gui {

HWND main_hwnd();
bool import_auto(const std::wstring& path);                    // file_assoc_dialog.cpp (content detection)
bool gerber_dialog_drop(const std::vector<std::wstring>&);     // gerber_workspace.cpp
bool job_files_drop(const app::DocPtr& d, const std::vector<std::wstring>& paths);

namespace {
namespace fs = std::filesystem;
namespace jz = pcbp::io::jobzip;
using ui::U;
using ui::W;

fs::path upath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string u8(const fs::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }
std::string base_of(const std::string& n) {
    std::string s = n; if (!s.empty() && s.back() == '/') s.pop_back();
    size_t k = s.find_last_of('/'); return k == std::string::npos ? s : s.substr(k + 1);
}
std::string parent_of(const std::string& n) {
    std::string s = n; if (!s.empty() && s.back() == '/') s.pop_back();
    size_t k = s.find_last_of('/'); return k == std::string::npos ? std::string() : s.substr(0, k + 1);
}
std::string disk_name(const std::string& p) { size_t k = p.find_last_of("/\\"); return k == std::string::npos ? p : p.substr(k + 1); }   // file name of a disk path
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string ext_of(const std::string& n) { size_t d = n.find_last_of('.'); return d == std::string::npos ? std::string() : lower(n.substr(d + 1)); }
std::string fmt_size(uint64_t b) {
    char t[48];
    if (b < 1024) std::snprintf(t, sizeof t, "%llu B", (unsigned long long)b);
    else if (b < (1u << 20)) std::snprintf(t, sizeof t, "%.1f KB", b / 1024.0);
    else if (b < (1ull << 30)) std::snprintf(t, sizeof t, "%.1f MB", b / 1048576.0);
    else std::snprintf(t, sizeof t, "%.2f GB", b / 1073741824.0);
    return t;
}
std::string join_tags(const std::vector<std::string>& t) { std::string s; for (auto& x : t) s += (s.empty() ? "" : ", ") + x; return s; }
std::vector<std::string> split_tags(const std::string& s) {
    std::vector<std::string> o; std::string cur;
    auto flush = [&]() { while (!cur.empty() && cur.back() == ' ') cur.pop_back(); size_t a = cur.find_first_not_of(' '); if (a != std::string::npos) o.push_back(cur.substr(a)); cur.clear(); };
    for (char c : s) { if (c == ',' || c == ';') flush(); else cur += c; }
    flush();
    return o;
}

// %TEMP%\PCBPlotter\Files\<n>\ (cleared per process start)
std::string temp_root() {
    static std::string root;
    if (root.empty()) {
        wchar_t b[MAX_PATH]; GetTempPathW(MAX_PATH, b);
        fs::path p = fs::path(b) / L"PCBPlotter" / L"Files" / std::to_wstring(GetCurrentProcessId());
        std::error_code ec; fs::remove_all(p, ec); fs::create_directories(p, ec);
        root = u8(p);
    }
    return root;
}
std::string temp_dir(const std::string& tag) {
    static int n = 0;
    fs::path p = upath(temp_root()) / upath(std::to_string(++n) + "-" + jz::sanitize_file_name(tag));
    std::error_code ec; fs::create_directories(p, ec);
    return u8(p);
}
void shell_open(const std::string& path) { ShellExecuteW(main_hwnd(), L"open", W(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// ---------------------------------------------------------------------------
// categories: colour chip + icon
// ---------------------------------------------------------------------------
using Cat = jz::Category;
constexpr int kCats = jz::kCategoryCount;
ImU32 cat_color(Cat c) {
    static const ImU32 k[kCats] = {
        IM_COL32(34, 160, 90, 255),    // Original Circuit: green
        IM_COL32(20, 140, 200, 255),   // Panel Gerbers: blue
        IM_COL32(150, 150, 165, 255),  // Stencil: steel
        IM_COL32(0, 150, 150, 255),    // CAD: teal
        IM_COL32(190, 90, 40, 255),    // BOM: rust
        IM_COL32(90, 110, 130, 255),   // Machine Programs: slate
        IM_COL32(140, 90, 210, 255),   // Schematic: violet
        IM_COL32(60, 100, 220, 255),   // Drawing: indigo
        IM_COL32(220, 70, 140, 255),   // Image: pink
        IM_COL32(230, 130, 30, 255),   // Email: orange
        IM_COL32(200, 170, 20, 255),   // Notes: amber
        IM_COL32(120, 120, 120, 255),  // Other: grey
    };
    const int i = (int)c;
    return k[i >= 0 && i < kCats ? i : kCats - 1];
}
ui::Icon cat_icon(Cat c) {
    switch (c) {
    case Cat::Original: return ui::Icon::Board;
    case Cat::Panel: return ui::Icon::Panelize;
    case Cat::Stencil: return ui::Icon::Layers;
    case Cat::Cad: return ui::Icon::ImportCad;
    case Cat::Bom: return ui::Icon::FileBom;
    case Cat::Programs: return ui::Icon::MachineProg;
    case Cat::Schematic: return ui::Icon::FileCad;
    case Cat::Drawing: return ui::Icon::FilePdf;
    case Cat::Image: return ui::Icon::FileImage;
    case Cat::Email: return ui::Icon::FileEmail;
    case Cat::Notes: return ui::Icon::Notes;
    default: return ui::Icon::FileGeneric;
    }
}
const char* cat_hint(Cat c) {
    switch (c) {
    case Cat::Original: return "Single-circuit Gerbers and drill, the customer's original Gerber zip";
    case Cat::Cad: return "ODB++ (.tgz), CAD and placement data, CircuitCAM projects";
    case Cat::Bom: return "Bills of materials (M1 dumps, customer BOMs)";
    case Cat::Programs: return "Machine programs kept for reference (the live ones stay next to the archive)";
    case Cat::Panel: return "Panelized Gerbers (often with less annotation than the original circuit)";
    case Cat::Stencil: return "Stencil (paste) Gerbers and stencil orders";
    case Cat::Schematic: return "Electrical schematics";
    case Cat::Drawing: return "Fabrication / assembly drawings (PDF, DXF)";
    case Cat::Image: return "Reference photos and pictures";
    case Cat::Email: return "Saved e-mails and correspondence";
    case Cat::Notes: return "Text and Markdown notes, instructions";
    default: return "Anything else (datasheets: tagged)";
    }
}
// small pill: colour dot + label; returns its width
float chip(ImDrawList* dl, ImVec2 p, Cat c, float h, bool compact = false) {
    const GTheme& t = th();
    const ImU32 col = cat_color(c);
    const char* lbl = compact && c == Cat::Email ? "Email" : jz::category_label(c);
    const ImVec2 ts = ImGui::CalcTextSize(lbl);
    const float w = ts.x + h + S(8);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col_alpha(col, t.dark ? 0.24f : 0.16f), h * 0.5f);
    dl->AddCircleFilled(ImVec2(p.x + h * 0.5f + S(1), p.y + h * 0.5f), h * 0.22f, col);
    dl->AddText(ImVec2(p.x + h * 0.5f + S(7), p.y + (h - ts.y) * 0.5f), t.text, lbl);
    return w;
}
// dropdown of the categories (icon + colour dot); true when changed
bool cat_combo(const char* id, Cat& c, float w) {
    const GTheme& t = th();
    bool changed = false;
    ImGui::SetNextItemWidth(w);
    const std::string preview = std::string("      ") + jz::category_label(c);
    const bool open = ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLarge);
    {   // dot + icon over the closed frame
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const float h = b.y - a.y;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(a.x + S(9), a.y + h * 0.5f), S(4), cat_color(c));
        icon_draw(dl, cat_icon(c), ImVec2(a.x + S(16), a.y + (h - S(16)) * 0.5f), S(16));
    }
    if (open) {
        for (int i = 0; i < kCats; ++i) {
            const Cat k = (Cat)i;
            ImGui::PushID(i);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetTextLineHeight() + S(6);
            if (ImGui::Selectable("##c", k == c, 0, ImVec2(0, h))) { c = k; changed = true; }
            if (ImGui::IsItemHovered()) tooltip(cat_hint(k));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddCircleFilled(ImVec2(p.x + S(6), p.y + h * 0.5f), S(4), cat_color(k));
            icon_draw(dl, cat_icon(k), ImVec2(p.x + S(14), p.y + (h - S(16)) * 0.5f), S(16));
            dl->AddText(ImVec2(p.x + S(36), p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), t.text, jz::category_label(k));
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}
void dashed_rect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float r, float th_, float dash) {
    auto seg = [&](ImVec2 p, ImVec2 q) {
        const float len = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y));
        const int n = std::max(1, (int)(len / (dash * 2)));
        for (int i = 0; i < n; ++i) {
            const float t0 = (float)i / n, t1 = t0 + 0.5f / n;
            dl->AddLine(ImVec2(p.x + (q.x - p.x) * t0, p.y + (q.y - p.y) * t0), ImVec2(p.x + (q.x - p.x) * t1, p.y + (q.y - p.y) * t1), col, th_);
        }
    };
    seg(ImVec2(a.x + r, a.y), ImVec2(b.x - r, a.y)); seg(ImVec2(b.x, a.y + r), ImVec2(b.x, b.y - r));
    seg(ImVec2(b.x - r, b.y), ImVec2(a.x + r, b.y)); seg(ImVec2(a.x, b.y - r), ImVec2(a.x, a.y + r));
    dl->PathArcTo(ImVec2(a.x + r, a.y + r), r, 3.14159f, 4.712f); dl->PathStroke(col, 0, th_);
    dl->PathArcTo(ImVec2(b.x - r, a.y + r), r, 4.712f, 6.283f); dl->PathStroke(col, 0, th_);
    dl->PathArcTo(ImVec2(b.x - r, b.y - r), r, 0.f, 1.571f); dl->PathStroke(col, 0, th_);
    dl->PathArcTo(ImVec2(a.x + r, b.y - r), r, 1.571f, 3.14159f); dl->PathStroke(col, 0, th_);
}

// ---------------------------------------------------------------------------
// archive state per document
// ---------------------------------------------------------------------------
struct Node {
    std::string name, full;     // full: archive name ("A/b.txt", folders "A/")
    int item = -1;              // index into the zip listing (-1 = implicit folder)
    std::map<std::string, Node> kids;
    bool folder() const { return !full.empty() && full.back() == '/'; }
};

struct State {
    std::string zip_path;
    bool exists = false, ok = false;
    std::string err;
    jz::Zip zip;
    fs::file_time_type mtime{};
    uint64_t fsize = 0;
    Node root;
    std::map<std::string, std::shared_ptr<jz::Zip>> nested;   // outer entry -> listing
    std::map<std::string, Node> nested_root;
    std::string sel;            // "name" or "outer|inner"
    // preview
    StaticTexture tex;
    std::string tex_key, tex_err;
    // detail editors
    std::string edit_key;
    char tags[512] = {}, note[2048] = {};
    int kinds = -1; std::string kinds_key;
    // R22 organiser
    std::string cwd;                         // "" = category list; "A/B/" folder, "outer.zip|" / "outer.zip|sub/" nested zip
    int cat = -1;                            // category filter (-1 = all files)
    std::string tag;                         // tag filter ("" = none)
    // ---- R23a mouse back/forward (XBUTTON1/2) history: begin ----
    struct NavPos { std::string cwd, tag; int cat = -1; bool jf = false; };
    std::vector<NavPos> nav_back, nav_fwd;
    bool nav_replay = false;
    // ---- R23a mouse back/forward: end ----
    std::vector<std::string> legacy;         // R19-R21 folder names still in the zip
    bool legacy_later = false;               // "Not now" (this session)
    int sort_col = 0; bool sort_asc = true;
    bool scroll_to_sel = false;
    std::string props_key, tags_key;         // Properties / Tags-and-note dialogs (ImGui modals)
    bool open_props = false, open_tags = false;
    char dlg_tags[512] = {}, dlg_note[2048] = {};
    struct DropTarget { ImVec2 a, b; int cat; };   // sidebar categories (last frame)
    std::vector<DropTarget> drop_targets;
    ImVec2 pane_a{0, 0}, pane_b{0, 0};
    std::string type_ahead; double type_ahead_t = 0;
    std::set<std::string> msel;              // selected keys (empty = just `sel`); `sel` is the focused item
    std::string anchor;                      // Shift range start
    std::string rename_key;                  // entry being renamed inline ("" = none)
    char rename_buf[260] = {};
    bool rename_focus = false;
    std::vector<std::string> tags_multi;     // Tags dialog for several files (adds tags)
    // drop confirm sheet
    std::unique_ptr<jz::DropSheet> sheet;
    bool open_sheet = false;
    int sheet_all = 0;
    // R25 pack the project folder: what stays next to the archive / is archived already (the sheet holds the rest);
    // after a verified pack, the originals that may move to _packed/
    std::unique_ptr<jz::PackPlan> pack;
    std::vector<std::string> pack_done;
    bool open_cleanup = false;
    std::string hash_note;                   // last "Verify archive" result
    // R23 job folder (sidecar files on disk next to the project)
    bool jf_mode = false;                    // sidebar "Job folder" selected
    std::vector<jz::SideFile> jf;
    std::string jf_dir, jf_sel;              // jf_sel: disk path
    int jf_frame = -10;                      // last frame the tab was drawn (activation -> rescan)
    double jf_time = -10;
};
std::map<const app::Doc*, std::unique_ptr<State>> g_state;
bool is_sel(const State& s, const std::string& k) { return s.msel.empty() ? s.sel == k : s.msel.count(k) != 0; }
void select_one(State& s, const std::string& k) { s.sel = k; s.anchor = k; s.msel.clear(); if (!k.empty()) s.msel.insert(k); }
void clear_sel(State& s) { s.sel.clear(); s.anchor.clear(); s.msel.clear(); }

State& state_of(const app::Doc* d) { auto& p = g_state[d]; if (!p) p = std::make_unique<State>(); return *p; }

std::string archive_path(const app::Doc& d) {
    const Project& p = d.prj;
    if (!p.documents_archive.empty()) {
        fs::path a = upath(p.documents_archive);
        if (a.is_relative() && !p.path.empty()) a = upath(p.path).parent_path() / a;
        return u8(a.lexically_normal());
    }
    return jz::archive_path_for(p.path, p.info.name);
}

void insert_node(Node& root, const std::string& name, int item) {
    Node* n = &root;
    size_t i = 0;
    while (i < name.size()) {
        size_t j = name.find('/', i);
        const bool last = j == std::string::npos || j + 1 == name.size();
        const std::string part = name.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (part.empty()) break;
        Node& k = n->kids[(j == std::string::npos ? "1" : "0") + lower(part)];   // folders first
        k.name = part;
        k.full = name.substr(0, j == std::string::npos ? name.size() : j + 1);
        if (last) { if (item >= 0) k.item = item; break; }
        n = &k;
        i = j + 1;
    }
}
Node build_tree(const jz::Zip& z) {
    Node r;
    for (size_t i = 0; i < z.items().size(); ++i)
        if (!jz::is_version_path(z.items()[i].name)) insert_node(r, z.items()[i].name, (int)i);   // R25: versions shown per file
    return r;
}

void reload(State& s, const app::Doc& d, bool force) {
    const std::string zp = archive_path(d);
    std::error_code ec;
    const bool ex = !zp.empty() && fs::exists(upath(zp), ec);
    const auto mt = ex ? fs::last_write_time(upath(zp), ec) : fs::file_time_type{};
    const uint64_t sz = ex ? fs::file_size(upath(zp), ec) : 0;
    if (!force && zp == s.zip_path && ex == s.exists && mt == s.mtime && sz == s.fsize) return;
    s.zip_path = zp; s.exists = ex; s.mtime = mt; s.fsize = sz;
    s.nested.clear(); s.nested_root.clear(); s.root = Node{}; s.ok = false; s.err.clear(); s.tex_key.clear(); s.kinds_key.clear(); s.legacy.clear();
    if (!ex) return;
    s.ok = s.zip.open_file(zp, s.err);
    if (s.ok) { s.root = build_tree(s.zip); s.legacy = jz::legacy_folders_in(s.zip); }
}

// item for a selection key; nested: "outer|inner"
const jz::Zip* zip_of(State& s, const std::string& key, std::string& inner) {
    size_t bar = key.find('|');
    if (bar == std::string::npos) { inner = key; return &s.zip; }
    inner = key.substr(bar + 1);
    auto it = s.nested.find(key.substr(0, bar));
    return it == s.nested.end() ? nullptr : it->second.get();
}
bool read_key(State& s, const std::string& key, std::vector<uint8_t>& out, std::string& err) {
    std::string inner;
    const jz::Zip* z = zip_of(s, key, inner);
    if (!z) { err = "archive not loaded"; return false; }
    return z->read(inner, out, err);
}
bool ensure_nested(State& s, const std::string& outer) {
    if (s.nested.count(outer)) return (bool)s.nested[outer];
    std::vector<uint8_t> b; std::string err;
    auto z = std::make_shared<jz::Zip>();
    if (!s.zip.read(outer, b, err) || !z->open_memory(std::move(b), err)) { s.nested[outer] = nullptr; toast("Cannot open " + base_of(outer) + ": " + err, ToastKind::Error, 6.f); return false; }
    s.nested_root[outer] = build_tree(*z);
    s.nested[outer] = z;
    return true;
}

// apply an edit to the archive, then refresh the listing and the project index (+ tags for new entries)
using TagMap = std::map<std::string, std::vector<std::string>>;
using HashMap = std::map<std::string, std::string>;
bool apply_now(const app::DocPtr& d, State& s, const jz::Edit& e, const std::string& what, const TagMap& tags = {}, const HashMap& hashes = {});
// Inside a frame the tree is being iterated: the rewrite runs after the frame.
bool apply(const app::DocPtr& d, State& s, const jz::Edit& e, const std::string& what, const TagMap& tags = {}) {
    if (in_frame()) { State* sp = &s; defer([d, sp, e, what, tags]() { apply_now(d, *sp, e, what, tags); }); return true; }
    return apply_now(d, s, e, what, tags);
}
bool apply_now(const app::DocPtr& d, State& s, const jz::Edit& e, const std::string& what, const TagMap& tags, const HashMap& hashes) {
    if (s.zip_path.empty()) { toast("Save the project first: the job archive lives next to the .pcbp file", ToastKind::Warning, 6.f); return false; }
    std::string err;
    bool ok = false;
    jz::Edit ev = e;
    if (s.ok) jz::carry_versions(s.zip, ev);   // R25: versions follow their file (delete / rename / move)
    run_with_progress_wait("Updating job archive", [&](Progress& p) { p.text(disk_name(s.zip_path).c_str()); ok = jz::rewrite(s.zip_path, ev, err); },
                           ProgressOpts{false, false, 0.3f});
    if (!ok) { toast("Job archive not changed: " + err, ToastKind::Error, 8.f); return false; }
    reload(s, *d, true);
    if (s.ok) {
        jz::sync_index(d->prj.documents, s.zip, ev.rename);
        for (auto& [path, tl] : tags)
            if (DocumentRef* r = jz::index_find(d->prj.documents, path))
                for (auto& t : tl) if (std::find(r->tags.begin(), r->tags.end(), t) == r->tags.end()) r->tags.push_back(t);
        for (auto& [path, h] : hashes) if (DocumentRef* r = jz::index_find(d->prj.documents, path)) r->sha256 = h;
    }
    s.edit_key.clear();
    d->prj.dirty = true;
    d->changed(ui::CH_INFO);
    toast(what, ToastKind::Success, 3.f);
    return true;
}
void sync_quiet(const app::DocPtr& d, State& s) {   // archive changed outside the app (or first load): sizes / CRCs / R22 categories
    if (!s.ok) return;
    auto before = d->prj.documents;
    jz::sync_index(d->prj.documents, s.zip);
    bool same = before.size() == d->prj.documents.size();
    for (size_t i = 0; same && i < before.size(); ++i)
        same = before[i].path == d->prj.documents[i].path && before[i].crc == d->prj.documents[i].crc && before[i].category == d->prj.documents[i].category && before[i].tags == d->prj.documents[i].tags;
    if (!same) { d->prj.dirty = true; d->changed(ui::CH_INFO); }
}

// ---------------------------------------------------------------------------
// adding files: confirm sheet (picker / drop) or straight into an open folder
// ---------------------------------------------------------------------------
std::string summary_of(const std::map<int, int>& pc) {
    std::string s; int total = 0;
    for (auto& [c, n] : pc) { s += (s.empty() ? "" : ", ") + std::string(jz::category_label((Cat)c)) + " " + std::to_string(n); total += n; }
    return "Added " + std::to_string(total) + (total == 1 ? " file" : " files") + " (" + s + ")";
}
bool can_add(State& s, const app::Doc& d) {
    reload(s, d, false);
    if (s.zip_path.empty()) { toast("Save the project first: the job archive \"<Project Name>.zip\" is created next to the .pcbp file", ToastKind::Warning, 8.f); return false; }
    if (s.exists && !s.ok) { toast("The job archive cannot be read (" + s.err + "); nothing was added", ToastKind::Error, 8.f); return false; }
    return true;
}
// Opens the confirm sheet for disk paths; `target` >= 0 preselects a category.
void offer_paths(const app::DocPtr& d, const std::vector<std::string>& paths, int target) {
    State& s = state_of(d.get());
    if (paths.empty() || !can_add(s, *d)) return;
    auto sh = std::make_unique<jz::DropSheet>();
    run_with_progress_wait("Looking at the files", [&](Progress& p) { p.text("Suggesting a category for each file"); *sh = jz::make_drop_sheet(paths, target); },
                           ProgressOpts{false, false, 0.3f});
    if (sh->rows.empty()) { toast("Nothing to add", ToastKind::Warning, 4.f); return; }
    s.sheet = std::move(sh);
    s.sheet_all = target >= 0 ? target : (int)s.sheet->rows[0].cat;
    s.open_sheet = true;
    request_redraw(3);
}
void commit_sheet(const app::DocPtr& d, State& s) {
    if (!s.sheet) return;
    jz::Edit e; TagMap tags; std::map<int, int> pc;
    std::vector<std::string> skipped;
    const bool any = s.sheet->build(s.ok ? &s.zip : nullptr, e, &tags, &pc, &skipped);
    s.sheet.reset();
    auto pack = std::move(s.pack);
    if (!any) {
        if (!skipped.empty()) toast(std::to_string(skipped.size()) + " file(s) already in the archive with the same content: nothing added", ToastKind::Info, 5.f);
        return;
    }
    HashMap hashes;   // SHA-256 of what goes in (R25)
    run_with_progress_wait("Hashing the files", [&](Progress& p) { p.text("SHA-256"); hashes = jz::hash_added(e); }, ProgressOpts{false, false, 0.3f});
    const bool created = !s.exists;
    std::string what = summary_of(pc) + (created ? "; created " + disk_name(s.zip_path) : std::string());
    if (!e.rename.empty()) what += "; " + std::to_string(e.rename.size()) + " older version(s) kept";
    if (!skipped.empty()) what += "; " + std::to_string(skipped.size()) + " unchanged skipped";
    if (!apply_now(d, s, e, what, tags, hashes) || !pack) return;
    // a pack: every packed file is checked against the archive before the originals may move
    std::string err;
    if (!s.ok || !jz::verify_packed(s.zip, e, err)) {
        toast("Packed, but the check against the originals failed (" + err + "): the originals stay where they are", ToastKind::Error, 10.f);
        return;
    }
    s.pack_done.clear();
    for (auto& a : e.add) if (!a.disk.empty()) s.pack_done.push_back(a.disk);
    s.open_cleanup = !s.pack_done.empty();
    request_redraw(3);
}
std::vector<std::string> jf_projects(const app::Doc& d, const State& s);
// R25: scan the project folder and offer everything that belongs in the archive (machine files and the project's
// sidecars stay next to it)
void pack_folder(const app::DocPtr& d) {
    State& s = state_of(d.get());
    if (!can_add(s, *d)) return;
    const std::string dir = u8(upath(s.zip_path).parent_path());
    auto plan = std::make_unique<jz::PackPlan>();
    run_with_progress_wait("Scanning the project folder", [&](Progress& p) {
        p.text(dir.c_str());
        *plan = jz::plan_pack(dir, jf_projects(*d, s), disk_name(s.zip_path), s.ok ? &s.zip : nullptr);
    }, ProgressOpts{false, false, 0.3f});
    if (plan->sheet.rows.empty()) {
        toast("Nothing new to pack: " + std::to_string(plan->already.size()) + " file(s) already in the archive, " + std::to_string(plan->stays.size()) +
                  " stay next to it", ToastKind::Info, 6.f);
        return;
    }
    s.sheet = std::make_unique<jz::DropSheet>(std::move(plan->sheet));
    s.pack = std::move(plan);
    s.sheet_all = (int)s.sheet->rows[0].cat;
    s.open_sheet = true;
    request_redraw(3);
}
// R25: hash every entry from the archive and compare with the stored hashes
void verify_archive(const app::DocPtr& d, State& s) {
    if (!s.ok) return;
    std::vector<std::string> bad;
    int added = 0;
    run_with_progress_wait("Verifying the job archive", [&](Progress& p) { p.text("SHA-256 of every file"); added = jz::fill_hashes(d->prj.documents, s.zip, {}, &bad); },
                           ProgressOpts{false, false, 0.3f});
    if (added) { d->prj.dirty = true; d->changed(ui::CH_INFO); }
    if (bad.empty()) {
        s.hash_note = "All " + std::to_string(d->prj.documents.size()) + " files verified";
        toast(s.hash_note + (added ? " (" + std::to_string(added) + " hashed for the first time)" : std::string()), ToastKind::Success, 5.f);
    } else {
        s.hash_note = std::to_string(bad.size()) + " file(s) unreadable or changed since hashed: " + bad[0] + (bad.size() > 1 ? ", ..." : "");
        toast(s.hash_note, ToastKind::Error, 10.f);
    }
}
// An open (non-nested) folder: dropped items go straight into it (the user chose the place).
void add_into_folder(const app::DocPtr& d, const std::vector<std::string>& paths, const std::string& folder) {
    State& s = state_of(d.get());
    if (!can_add(s, *d)) return;
    jz::Edit e; std::set<std::string> taken; int n = 0;
    for (auto& p : paths) {
        std::error_code ec;
        const std::string nm = u8(upath(p).filename());
        if (fs::is_directory(upath(p), ec)) {
            const std::string pre = jz::unique_name(s.ok ? &s.zip : nullptr, folder, nm, taken);
            for (auto it = fs::recursive_directory_iterator(upath(p), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                std::string rel = u8(fs::relative(it->path(), upath(p), ec)); for (auto& c : rel) if (c == '\\') c = '/';
                e.add.push_back({pre + "/" + rel, {}, u8(it->path())}); taken.insert(lower(pre + "/" + rel)); ++n;
            }
        } else {
            const std::string an = jz::unique_name(s.ok ? &s.zip : nullptr, folder, nm, taken);
            taken.insert(lower(an)); e.add.push_back({an, {}, p}); ++n;
        }
    }
    if (n) apply(d, s, e, "Added " + std::to_string(n) + (n == 1 ? " file to " : " files to ") + folder);
}

// ---------------------------------------------------------------------------
// extract / open / import
// ---------------------------------------------------------------------------
// Writes the entries of `key` (a file, a folder "X/", or a nested zip's folder) into a fresh temp folder.
// Returns the written paths (first = the file itself for a single entry).
std::vector<std::string> extract_temp(State& s, const std::string& key, const std::string& tag, std::string& dir, std::function<bool(const std::string&)> filter = nullptr) {
    std::vector<std::string> out;
    std::string inner;
    const jz::Zip* z = zip_of(s, key, inner);
    if (!z) return out;
    dir = temp_dir(tag);
    const bool folder = inner.empty() || inner.back() == '/';
    const std::string base = folder ? inner : parent_of(inner);
    for (auto& it : z->items()) {
        if (it.dir()) continue;
        if (folder ? it.name.compare(0, inner.size(), inner) != 0 : it.name != inner) continue;
        if (filter && !filter(it.name)) continue;
        std::vector<uint8_t> b; std::string err;
        if (!z->read(it, b, err)) { toast(err, ToastKind::Error, 6.f); continue; }
        fs::path dst = upath(dir) / upath(it.name.substr(base.size()));
        std::error_code ec; fs::create_directories(dst.parent_path(), ec);
        std::string w = u8(dst);
        if (write_file(w, b.data(), b.size(), err)) out.push_back(w);
    }
    return out;
}

bool is_sheet_ext(const std::string& e) { return e == "csv" || e == "tsv" || e == "xlsx" || e == "xls" || e == "xlsm" || e == "ods"; }
bool is_doc_ext(const std::string& e) { return e == "eml" || e == "msg" || e == "txt" || e == "rtf" || e == "html" || e == "htm" || e == "log"; }

void import_as(const app::DocPtr& d, State& s, const std::string& key, int kind) {
    std::string dir;
    const std::string nm = key.substr(key.find('|') == std::string::npos ? 0 : key.find('|') + 1);
    std::string inner; const jz::Zip* z = zip_of(s, key, inner);
    if (!z) return;
    if (kind == jz::ImpGerber && !(inner.empty() || inner.back() == '/') && jz::is_zip_name(inner)) {
        // a nested Gerber zip: extract it and let content detection plan the layer load
        auto f = extract_temp(s, key, "gerber", dir);
        if (!f.empty()) { std::wstring p = W(f[0]); defer([p]() { import_auto(p); }); }
        return;
    }
    if (kind == jz::ImpGerber) {
        auto f = extract_temp(s, key, "gerber", dir);
        if (f.empty()) { toast("Nothing to load", ToastKind::Warning, 4.f); return; }
        std::wstring wd = W(dir);
        defer([wd]() { if (!gerber_dialog_drop({wd})) toast("No Gerber layers could be loaded", ToastKind::Error, 6.f); });
        return;
    }
    auto f = extract_temp(s, key, "import", dir);
    if (f.empty()) return;
    std::wstring p = W(f[0]);
    app::DocPtr dp = d;
    if (kind == jz::ImpBom) defer([dp, p]() { if (app::dlg_import_bom(main_hwnd(), *dp, p)) dp->changed(ui::CH_ALL); });
    else if (kind == jz::ImpCentroid) defer([dp, p]() { if (app::dlg_import_centroid(main_hwnd(), *dp, p, false)) dp->changed(ui::CH_ALL); });
    else defer([p]() { import_auto(p); });
    (void)nm;
}

void extract_to(State& s, const std::string& key);
std::string key_inner(const std::string& key);
void extract_many(State& s, const std::vector<std::string>& keys) {   // R21: several entries -> one folder
    if (keys.size() == 1) { extract_to(s, keys[0]); return; }
    State* sp = &s;
    defer([sp, keys]() {
        std::wstring dst;
        if (!ui::pick_folder_dialog(main_hwnd(), L"Extract to folder", dst)) return;
        int n = 0;
        for (auto& k : keys) {
            std::string dir;
            const std::string inner = key_inner(k);
            const bool folder = inner.empty() || inner.back() == '/';
            auto f = extract_temp(*sp, k, "x", dir);
            for (auto& src : f) {
                std::error_code ec;
                fs::path rel = fs::relative(upath(src), upath(dir), ec);
                fs::path out = fs::path(dst) / (folder ? upath(base_of(inner)) / rel : rel);   // a folder keeps its name
                fs::create_directories(out.parent_path(), ec);
                fs::copy_file(upath(src), out, fs::copy_options::overwrite_existing, ec);
                if (!ec) ++n;
            }
        }
        toast("Extracted " + std::to_string(n) + (n == 1 ? " file to " : " files to ") + U(dst), ToastKind::Success, 5.f);
    });
}

void extract_to(State& s, const std::string& key) {
    std::string skey = key;
    State* sp = &s;
    defer([sp, skey]() {
        std::wstring dst;
        if (!ui::pick_folder_dialog(main_hwnd(), L"Extract to folder", dst)) return;
        std::string dir;
        auto f = extract_temp(*sp, skey, "x", dir);
        int n = 0;
        for (auto& src : f) {
            std::error_code ec;
            fs::path rel = fs::relative(upath(src), upath(dir), ec);
            fs::path out = fs::path(dst) / rel;
            fs::create_directories(out.parent_path(), ec);
            fs::copy_file(upath(src), out, fs::copy_options::overwrite_existing, ec);
            if (!ec) ++n;
        }
        toast("Extracted " + std::to_string(n) + (n == 1 ? " file to " : " files to ") + U(dst), ToastKind::Success, 5.f);
    });
}

// ---------------------------------------------------------------------------
// R20 preview / open externally
// ---------------------------------------------------------------------------
std::string key_inner(const std::string& key) { size_t b = key.find('|'); return b == std::string::npos ? key : key.substr(b + 1); }
bool key_nested(const std::string& key) { return key.find('|') != std::string::npos; }

void open_external(State& s, const std::string& key) {
    std::string dir;
    auto f = extract_temp(s, key, "open", dir);
    if (f.empty()) { toast("Nothing to open", ToastKind::Warning, 4.f); return; }
    shell_open(f[0]);
}

int kinds_of(State& s, const std::string& key) {
    const std::string inner = key_inner(key);
    if (inner.empty() || inner.back() == '/') return jz::ImpGerber;
    std::vector<uint8_t> b; std::string err;
    return read_key(s, key, b, err) ? jz::import_kinds(inner, b) : 0;
}

void preview_entry(const app::DocPtr& d, State& s, const std::string& key) {
    std::vector<uint8_t> b; std::string err;
    if (!read_key(s, key, b, err)) { toast("Cannot read " + base_of(key_inner(key)) + ": " + err, ToastKind::Error, 6.f); return; }
    preview::Request r;
    const std::string inner = key_inner(key);
    r.name = base_of(inner);
    r.where = key_nested(key) ? key.substr(0, key.find('|')) + " \xE2\x80\xBA " + inner : inner;
    r.import_kinds = jz::import_kinds(inner, b);
    r.bytes = std::move(b);
    State* sp = &s;
    app::DocPtr dp = d;
    r.extract = [sp, key]() { extract_to(*sp, key); };
    r.open_external = [sp, key]() { State* s2 = sp; std::string k2 = key; defer([s2, k2]() { open_external(*s2, k2); }); };
    r.import_as = [dp, sp, key](int kind) { preview::close(); import_as(dp, *sp, key, kind); };
    preview::open(std::move(r));
}


// ---------------------------------------------------------------------------
// listing
// ---------------------------------------------------------------------------
struct Entry {
    std::string key, name;                    // key: selection key ("A/b.pdf", "A/", "outer.zip|x.gbr")
    bool folder = false, zipdir = false, nested = false;
    uint64_t size = 0, csize = 0;
    uint32_t crc = 0;
    uint16_t mdate = 0, mtime = 0;
    jz::FileClass cls = jz::FileClass::Generic;
    Cat cat = Cat::Other;
    bool cat_known = true;                    // false inside nested zips (no index there)
    std::string type, tags, note, modified, where;
};

ui::Icon icon_of(jz::FileClass c) {
    switch (c) {
    case jz::FileClass::Pdf: return ui::Icon::FilePdf;
    case jz::FileClass::Doc: return ui::Icon::FileDoc;
    case jz::FileClass::Sheet: return ui::Icon::FileSheet;
    case jz::FileClass::Csv: return ui::Icon::FileCsv;
    case jz::FileClass::Zip: return ui::Icon::FileZip;
    case jz::FileClass::Gerber: return ui::Icon::FileGerber;
    case jz::FileClass::Drill: return ui::Icon::FileDrill;
    case jz::FileClass::Image: return ui::Icon::FileImage;
    case jz::FileClass::Email: return ui::Icon::FileEmail;
    case jz::FileClass::Cad: return ui::Icon::FileCad;
    case jz::FileClass::Bom: return ui::Icon::FileBom;
    case jz::FileClass::Text: return ui::Icon::FileText;
    default: return ui::Icon::FileGeneric;
    }
}
ui::Icon icon_of(const Entry& e) { return e.folder ? (e.zipdir ? ui::Icon::FileZip : ui::Icon::Folder) : icon_of(e.cls); }

// node of a folder key; nullptr when it no longer exists
const Node* resolve_dir(State& s, const std::string& cwd, std::string* outer_out = nullptr) {
    std::string outer, inner = cwd;
    const size_t bar = cwd.find('|');
    if (bar != std::string::npos) { outer = cwd.substr(0, bar); inner = cwd.substr(bar + 1); }
    if (outer_out) *outer_out = outer;
    const Node* n = &s.root;
    if (!outer.empty()) {
        if (s.zip.find(outer) < 0 || !ensure_nested(s, outer)) return nullptr;
        auto it = s.nested_root.find(outer);
        if (it == s.nested_root.end()) return nullptr;
        n = &it->second;
    }
    size_t i = 0;
    while (i < inner.size()) {
        size_t j = inner.find('/', i);
        if (j == std::string::npos) j = inner.size();
        const std::string part = inner.substr(i, j - i);
        const Node* nx = nullptr;
        for (auto& [k, c] : n->kids) if (c.folder() && c.name == part) { nx = &c; break; }
        if (!nx) return nullptr;
        n = nx;
        i = j + 1;
    }
    return n;
}

Entry make_entry(const app::Doc& d, State& s, const Node& c, const std::string& outer, const jz::Zip* z) {
    Entry e;
    e.name = c.name;
    e.nested = !outer.empty();
    e.key = outer.empty() ? c.full : outer + "|" + c.full;
    e.folder = c.folder();
    if (!e.folder && outer.empty() && jz::is_zip_name(c.full)) { e.folder = true; e.zipdir = true; }
    e.cls = jz::file_class(c.name);
    if (c.item >= 0 && z) {
        const auto& it = z->items()[(size_t)c.item];
        e.size = it.usize; e.csize = it.csize; e.crc = it.crc; e.mdate = it.mdate; e.mtime = it.mtime;
        e.modified = jz::dos_datetime_text(it.mdate, it.mtime);
    }
    if (e.folder && !e.zipdir) {   // folder: total size of its files
        uint64_t n = 0;
        if (z) for (auto& it : z->items()) if (!it.dir() && it.name.compare(0, c.full.size(), c.full) == 0) { e.size += it.usize; ++n; }
        e.type = "Folder, " + std::to_string(n) + (n == 1 ? " file" : " files");
    } else e.type = e.zipdir ? "Zip archive (opens as a folder)" : jz::file_class_label(e.cls);
    if (!e.folder && e.cls == jz::FileClass::Generic) {
        const std::string x = ext_of(c.name);
        if (!x.empty()) { std::string up = x; for (auto& ch : up) ch = (char)std::toupper((unsigned char)ch); e.type = up + " file"; }
    }
    if (outer.empty()) {
        bool known = false;
        e.cat = jz::category_of_path(c.full, &known);
        const DocumentRef* r = (!e.folder || e.zipdir) ? jz::index_find(const_cast<app::Doc&>(d).prj.documents, c.full) : nullptr;
        if (!known && r) e.cat = jz::category_from_name(r->category);
        if (r) { e.tags = join_tags(r->tags); e.note = r->note; }
    } else e.cat_known = false;
    return e;
}

void sort_entries(std::vector<Entry>& out, int col, bool asc) {
    std::stable_sort(out.begin(), out.end(), [col, asc](const Entry& a, const Entry& b) {
        if (a.folder != b.folder) return a.folder;   // folders first, either direction
        int c = 0;
        switch (col) {
        case 1: c = (int)a.cat - (int)b.cat; break;
        case 2: c = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
        case 3: { uint32_t x = ((uint32_t)a.mdate << 16) | a.mtime, y = ((uint32_t)b.mdate << 16) | b.mtime; c = x < y ? -1 : x > y ? 1 : 0; } break;
        case 4: c = lower(a.tags).compare(lower(b.tags)); break;
        default: break;
        }
        if (c == 0) c = lower(a.name).compare(lower(b.name));
        return asc ? c < 0 : c > 0;
    });
}

// Category list (cwd ""): the items directly inside each category folder (current or legacy name), top-level files
// and custom folders. Tag filter: every indexed file with that tag, at any depth.
std::vector<Entry> list_categories(const app::Doc& d, State& s) {
    std::vector<Entry> out;
    if (!s.tag.empty()) {
        for (auto& r : d.prj.documents) {
            if (std::find(r.tags.begin(), r.tags.end(), s.tag) == r.tags.end()) continue;
            const int i = s.zip.find(r.path);
            if (i < 0) continue;
            Node n; n.name = base_of(r.path); n.full = r.path; n.item = i;
            Entry e = make_entry(d, s, n, "", &s.zip);
            e.where = parent_of(r.path);
            if (s.cat < 0 || (int)e.cat == s.cat) out.push_back(std::move(e));
        }
    } else {
        for (auto& [k, c] : s.root.kids) {
            bool known = false;
            const Cat cc = jz::category_of_path(c.full, &known);
            if (c.folder() && known) {
                if (s.cat >= 0 && (int)cc != s.cat) continue;
                for (auto& [k2, c2] : c.kids) { Entry e = make_entry(d, s, c2, "", &s.zip); e.where = c.full; out.push_back(std::move(e)); }
            } else {
                Entry e = make_entry(d, s, c, "", &s.zip);
                if (s.cat < 0 || (int)e.cat == s.cat) out.push_back(std::move(e));
            }
        }
    }
    sort_entries(out, s.sort_col, s.sort_asc);
    return out;
}

std::vector<Entry> list_dir(const app::Doc& d, State& s) {
    std::vector<Entry> out;
    std::string outer;
    const Node* n = resolve_dir(s, s.cwd, &outer);
    if (!n) return out;
    const jz::Zip* z = outer.empty() ? &s.zip : s.nested[outer].get();
    for (auto& [k, c] : n->kids) out.push_back(make_entry(d, s, c, outer, z));
    sort_entries(out, s.sort_col, s.sort_asc);
    return out;
}

std::string parent_dir(const std::string& cwd) {
    const size_t bar = cwd.find('|');
    if (bar != std::string::npos) {
        const std::string outer = cwd.substr(0, bar), inner = cwd.substr(bar + 1);
        if (inner.empty()) return parent_of(outer);
        return outer + "|" + parent_of(inner);
    }
    return parent_of(cwd);
}
// "Original Circuit › gerbers rev B › sub" for the back bar
std::string where_label(const std::string& cwd) {
    std::string s = cwd;
    for (size_t p; (p = s.find('|')) != std::string::npos;) s.replace(p, 1, "/");
    while (!s.empty() && s.back() == '/') s.pop_back();
    std::string o;
    for (char c : s) { if (c == '/') o += " \xE2\x80\xBA "; else o += c; }
    return o;
}

// ---------------------------------------------------------------------------
// the view
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// R23 job folder: the project's sidecar files next to the .pcbp / job archive (on disk, never inside the zip)
// ---------------------------------------------------------------------------
std::vector<std::string> jf_projects(const app::Doc& d, const State& s) {
    std::vector<std::string> v;
    if (!d.prj.info.name.empty()) v.push_back(d.prj.info.name);
    if (!d.prj.path.empty()) v.push_back(u8(upath(d.prj.path).stem()));
    if (!s.zip_path.empty()) v.push_back(u8(upath(s.zip_path).stem()));
    return v;
}
std::string jf_dir_of(const app::Doc& d) { return d.prj.path.empty() ? std::string() : u8(upath(d.prj.path).parent_path()); }
void jf_rescan(State& s, const app::Doc& d) {
    s.jf_dir = jf_dir_of(d);
    s.jf = jz::scan_job_folder(s.jf_dir, jf_projects(d, s), s.zip_path.empty() ? std::string() : disk_name(s.zip_path));
    bool keep = false; for (auto& f : s.jf) keep |= f.path == s.jf_sel;
    if (!keep) s.jf_sel.clear();
}
int jf_index(const State& s, const std::string& path) { for (size_t i = 0; i < s.jf.size(); ++i) if (s.jf[i].path == path) return (int)i; return -1; }
std::string jf_time(int64_t t) {
    if (!t) return {};
    time_t tt = (time_t)t; struct tm lt{};
    localtime_s(&lt, &tt);
    char b[32]; std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &lt);
    return b;
}
ui::Icon jf_icon(jz::SideKind k) {
    switch (k) {
    case jz::SideKind::HlcMaster: case jz::SideKind::IssMaster: return ui::Icon::FileCad;
    case jz::SideKind::HlcSplit: case jz::SideKind::IssSplit: return ui::Icon::FileCad;
    case jz::SideKind::AoiImage: return ui::Icon::FileImage;
    case jz::SideKind::WorkOrder: return ui::Icon::FileSheet;
    case jz::SideKind::Archive: return ui::Icon::FileZip;
    default: return ui::Icon::FileGeneric;
    }
}
bool jf_is_program(jz::SideKind k) { return k == jz::SideKind::HlcMaster || k == jz::SideKind::HlcSplit || k == jz::SideKind::IssMaster || k == jz::SideKind::IssSplit; }
bool read_disk(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(upath(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
void reveal_in_explorer(const std::string& path) {
    std::wstring a = L"/select,\"" + W(path) + L"\"";
    ShellExecuteW(main_hwnd(), L"open", L"explorer.exe", a.c_str(), nullptr, SW_SHOWNORMAL);
}

// --- program viewer (modal) --------------------------------------------------
struct ProgViewer {
    bool want_open = false;
    std::string path, name, img_name;
    io::progview::Program prog;
    std::string err;
    StaticTexture tex;
    int iw = 0, ih = 0;
    std::vector<uint8_t> px;
    bool has_img = false, show_img = true, show_refs = true, aligned = false, manual = false;
    float img_alpha = 0.85f;
    aoi::Affine xf, base;
    std::vector<aoi::FidHit> hits;
    std::string note;
    int pick = -1, fa = 0, fb = 1;   // manual alignment: -1 off, 0 waiting first fid, 1 waiting second
    double p1x = 0, p1y = 0;
    float zoom = 1.f; ImVec2 pan{0, 0};
    int sel = -1; bool scroll_sel = false;
    char filter[64] = {};
    std::string calib_note;
};
ProgViewer g_pv;

void pv_pick_pair(ProgViewer& v) {   // the two fiducials farthest apart
    v.fa = 0; v.fb = 1; double best = -1;
    for (size_t i = 0; i < v.prog.fids.size(); ++i) for (size_t j = i + 1; j < v.prog.fids.size(); ++j) {
        const double d = std::hypot(v.prog.fids[i].x - v.prog.fids[j].x, v.prog.fids[i].y - v.prog.fids[j].y);
        if (d > best) { best = d; v.fa = (int)i; v.fb = (int)j; }
    }
}
void pv_auto_align(ProgViewer& v) {
    v.hits.clear();
    for (auto& f : v.prog.fids) { aoi::FidHit h; h.mm_x = f.x; h.mm_y = f.y; h.dia_mm = f.dia > 0 ? f.dia : 1.35; v.hits.push_back(h); }
    v.base = aoi::stretch_to_bbox(v.iw, v.ih, v.prog.x0, v.prog.y0, v.prog.x1, v.prog.y1);
    v.aligned = aoi::auto_align(v.px.data(), v.iw, v.ih, v.iw * 4, v.prog.x0, v.prog.y0, v.prog.x1, v.prog.y1, v.hits, v.xf);
    v.manual = false;
    int found = 0; for (auto& h : v.hits) found += h.found;
    v.note = v.aligned ? "Aligned on " + std::to_string(found) + " of " + std::to_string(v.hits.size()) + " fiducials (auto)"
           : v.prog.fids.size() < 2 ? "Fewer than 2 fiducials in the program: picture stretched over the board"
           : "Fiducials not found in the picture (" + std::to_string(found) + " of " + std::to_string(v.hits.size()) + "): stretched over the board. Use \"Click fiducials\" to align by hand.";
}
void open_program_viewer(const State& s, int index) {
    if (index < 0 || index >= (int)s.jf.size()) return;
    ProgViewer& v = g_pv;
    v.tex.release(); v.px.clear(); v.has_img = false; v.aligned = false; v.manual = false; v.pick = -1; v.sel = -1;
    v.zoom = 1.f; v.pan = ImVec2(0, 0); v.filter[0] = 0; v.hits.clear(); v.note.clear(); v.img_name.clear();
    v.path = s.jf[(size_t)index].path; v.name = s.jf[(size_t)index].name;
    std::vector<uint8_t> b;
    v.err.clear();
    if (!read_disk(v.path, b)) v.err = "cannot read the file";
    else if (!io::progview::load(b, v.prog, v.err)) { if (v.err.empty()) v.err = "not a readable machine program"; }
    if (v.err.empty() && !(v.prog.x1 > v.prog.x0 && v.prog.y1 > v.prog.y0)) {   // no board size: placement extent
        v.prog.x0 = v.prog.y0 = 1e300; v.prog.x1 = v.prog.y1 = -1e300;
        for (auto& p : v.prog.places) { v.prog.x0 = std::min(v.prog.x0, p.x); v.prog.y0 = std::min(v.prog.y0, p.y); v.prog.x1 = std::max(v.prog.x1, p.x); v.prog.y1 = std::max(v.prog.y1, p.y); }
        if (v.prog.x1 < v.prog.x0) { v.prog.x0 = v.prog.y0 = 0; v.prog.x1 = v.prog.y1 = 10; }
    }
    const int ai = v.err.empty() ? jz::aoi_for(s.jf, index) : -1;
    if (ai >= 0) {
        std::vector<uint8_t> ib;
        if (read_disk(s.jf[(size_t)ai].path, ib) && preview::decode_image(ib, 4096, v.iw, v.ih, v.px) && v.tex.create(v.iw, v.ih, v.px.data(), v.iw * 4)) {
            v.has_img = true; v.img_name = s.jf[(size_t)ai].name;
            pv_pick_pair(v);
            pv_auto_align(v);
        }
    }
    v.want_open = true;
}

void pv_draw_canvas(ProgViewer& v, ImVec2 size) {
    const GTheme& t = th();
    ImGui::BeginChild("##pvc", size, ImGuiChildFlags_Border, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##pvcanvas", av, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hov = ImGui::IsItemHovered();
    const double bw = v.prog.x1 - v.prog.x0, bh = v.prog.y1 - v.prog.y0;
    const double fit = std::min((av.x - S(30)) / std::max(bw, 1e-3), (av.y - S(30)) / std::max(bh, 1e-3));
    const double k = fit * v.zoom;
    const ImVec2 ctr(c0.x + av.x * 0.5f + v.pan.x, c0.y + av.y * 0.5f + v.pan.y);
    const double mx = (v.prog.x0 + v.prog.x1) * 0.5, my = (v.prog.y0 + v.prog.y1) * 0.5;
    auto to_s = [&](double x, double y) { return ImVec2((float)(ctr.x + (x - mx) * k), (float)(ctr.y - (y - my) * k)); };
    auto to_mm = [&](ImVec2 p, double& x, double& y) { x = mx + (p.x - ctr.x) / k; y = my - (p.y - ctr.y) / k; };
    ImGuiIO& io = ImGui::GetIO();
    if (hov && io.MouseWheel != 0) {
        const float nz = std::clamp(v.zoom * (io.MouseWheel > 0 ? 1.2f : 1 / 1.2f), 0.2f, 60.f);
        const ImVec2 m = io.MousePos;
        v.pan.x = (m.x - c0.x - av.x * 0.5f) - (m.x - ctr.x) * nz / v.zoom;
        v.pan.y = (m.y - c0.y - av.y * 0.5f) - (m.y - ctr.y) * nz / v.zoom;
        v.zoom = nz;
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) { v.pan.x += io.MouseDelta.x; v.pan.y += io.MouseDelta.y; }
    dl->PushClipRect(c0, ImVec2(c0.x + av.x, c0.y + av.y), true);
    dl->AddRectFilled(c0, ImVec2(c0.x + av.x, c0.y + av.y), t.dark ? IM_COL32(18, 20, 24, 255) : IM_COL32(236, 238, 242, 255));
    // board
    dl->AddRectFilled(to_s(v.prog.x0, v.prog.y1), to_s(v.prog.x1, v.prog.y0), IM_COL32(24, 70, 44, 255));
    aoi::Affine inv;
    if (v.has_img && v.show_img && v.xf.inverse(inv)) {
        double ax, ay; ImVec2 q[4];
        const double cx[4] = {0, (double)v.iw, (double)v.iw, 0}, cy[4] = {0, 0, (double)v.ih, (double)v.ih};
        for (int i = 0; i < 4; ++i) { inv.apply(cx[i], cy[i], ax, ay); q[i] = to_s(ax, ay); }
        dl->AddImageQuad((ImTextureID)v.tex.imgui_id(), q[0], q[1], q[2], q[3], ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1),
                         IM_COL32(255, 255, 255, (int)(v.img_alpha * 255)));
    }
    dl->AddRect(to_s(v.prog.x0, v.prog.y1), to_s(v.prog.x1, v.prog.y0), IM_COL32(200, 200, 120, 255), 0, 0, S(1.5f));
    // placements
    const float r = std::max(S(2.5f), (float)(0.5 * k));
    int hover = -1; float hd = S(8);
    for (int i = 0; i < (int)v.prog.places.size(); ++i) {
        const auto& p = v.prog.places[(size_t)i];
        const ImVec2 sp = to_s(p.x, p.y);
        const ImU32 col = p.skip ? IM_COL32(150, 150, 150, 230) : IM_COL32(255, 196, 40, 240);
        dl->AddCircleFilled(sp, r, col);
        const double a = p.rot * 3.14159265358979 / 180.0;
        dl->AddLine(sp, ImVec2(sp.x + (float)std::cos(a) * r * 2.2f, sp.y - (float)std::sin(a) * r * 2.2f), col, S(1.2f));
        if (v.show_refs && k > 6) dl->AddText(ImVec2(sp.x + r + S(2), sp.y - S(7)), IM_COL32(255, 255, 255, 220), p.ref.c_str());
        if (hov) { const float d = std::hypot(io.MousePos.x - sp.x, io.MousePos.y - sp.y); if (d < hd) { hd = d; hover = i; } }
    }
    if (v.sel >= 0 && v.sel < (int)v.prog.places.size()) {
        const ImVec2 sp = to_s(v.prog.places[(size_t)v.sel].x, v.prog.places[(size_t)v.sel].y);
        dl->AddCircle(sp, r + S(5), IM_COL32(80, 200, 255, 255), 0, S(2.5f));
    }
    // fiducials (program) + where they were found in the picture
    for (size_t i = 0; i < v.prog.fids.size(); ++i) {
        const auto& f = v.prog.fids[i];
        const ImVec2 sp = to_s(f.x, f.y);
        const bool want = v.pick >= 0 && (int)i == (v.pick == 0 ? v.fa : v.fb);
        dl->AddCircle(sp, std::max(S(6), (float)(0.7 * k)), want ? IM_COL32(255, 60, 60, 255) : IM_COL32(80, 230, 255, 255), 0, S(2));
        dl->AddText(ImVec2(sp.x + S(9), sp.y + S(4)), IM_COL32(80, 230, 255, 255), ("F" + std::to_string(i + 1)).c_str());
        if (i < v.hits.size() && v.hits[i].found && v.show_img && !v.manual) {
            double hx, hy; if (v.xf.inverse(inv)) { inv.apply(v.hits[i].px_x, v.hits[i].px_y, hx, hy); const ImVec2 hp = to_s(hx, hy);
                dl->AddLine(ImVec2(hp.x - S(5), hp.y), ImVec2(hp.x + S(5), hp.y), IM_COL32(255, 80, 200, 255), S(1.5f));
                dl->AddLine(ImVec2(hp.x, hp.y - S(5)), ImVec2(hp.x, hp.y + S(5)), IM_COL32(255, 80, 200, 255), S(1.5f)); }
        }
    }
    dl->PopClipRect();
    // clicks
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (v.pick >= 0 && v.has_img) {
            double x, y; to_mm(io.MousePos, x, y);
            double ppx, ppy; v.xf.apply(x, y, ppx, ppy);   // the picture pixel under the cursor
            if (v.pick == 0) { v.p1x = ppx; v.p1y = ppy; v.pick = 1; }
            else {
                const auto& a = v.prog.fids[(size_t)v.fa]; const auto& b = v.prog.fids[(size_t)v.fb];
                aoi::Affine out;
                if (aoi::align_pairs(v.base, a.x, a.y, b.x, b.y, v.p1x, v.p1y, ppx, ppy, out)) { v.xf = out; v.aligned = true; v.manual = true; v.note = "Aligned by hand on F" + std::to_string(v.fa + 1) + " / F" + std::to_string(v.fb + 1); }
                else v.note = "The two clicks were on the same spot; try again";
                v.pick = -1;
            }
        } else if (hover >= 0) { v.sel = hover; v.scroll_sel = true; }
    }
    if (hover >= 0 && v.pick < 0) {
        const auto& p = v.prog.places[(size_t)hover];
        char b[256]; std::snprintf(b, sizeof b, "%s  %s\nX %.3f  Y %.3f  %.2f\xC2\xB0%s%s%s", p.ref.c_str(), p.part.c_str(), p.x, p.y, p.rot,
                                   p.feeder.empty() ? "" : "\nFeeder ", p.feeder.c_str(), p.skip ? "\nskipped" : "");
        tooltip(b);
    }
    if (v.pick >= 0) {
        const char* m = v.pick == 0 ? "Click fiducial F%d on the picture (Esc cancels)" : "Now click fiducial F%d on the picture";
        char b[96]; std::snprintf(b, sizeof b, m, (v.pick == 0 ? v.fa : v.fb) + 1);
        const ImVec2 ts = ImGui::CalcTextSize(b);
        dl->AddRectFilled(ImVec2(c0.x + S(8), c0.y + S(8)), ImVec2(c0.x + S(20) + ts.x, c0.y + S(16) + ts.y), IM_COL32(0, 0, 0, 190), S(4));
        dl->AddText(ImVec2(c0.x + S(14), c0.y + S(12)), IM_COL32(255, 230, 120, 255), b);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) v.pick = -1;
    }
    ImGui::EndChild();
}

void draw_program_viewer() {
    ProgViewer& v = g_pv;
    if (v.want_open) { ImGui::OpenPopup("Program viewer##pv"); v.want_open = false; }
    const ImVec2 vs = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(vs.x * 0.86f, vs.y * 0.86f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(vs.x * 0.5f, vs.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (!ImGui::BeginPopupModal("Program viewer##pv", &open)) return;
    if (!open) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    ImGui::PushFont(g_font_bold); ImGui::TextUnformatted(v.name.c_str()); ImGui::PopFont();
    ImGui::SameLine();
    if (!v.err.empty()) {
        ImGui::TextDisabled("cannot show this program: %s", v.err.c_str());
    } else {
        int skip = 0; for (auto& p : v.prog.places) skip += p.skip;
        ImGui::TextDisabled("%s  \xC2\xB7  %d placements%s  \xC2\xB7  %d fiducials  \xC2\xB7  board %.2f x %.2f mm", v.prog.format.c_str(), (int)v.prog.places.size(),
                            skip ? (" (" + std::to_string(skip) + " skipped)").c_str() : "", (int)v.prog.fids.size(), v.prog.x1 - v.prog.x0, v.prog.y1 - v.prog.y0);
    }
    // toolbar
    if (v.has_img) {
        ImGui::Checkbox("AOI picture", &v.show_img); ImGui::SameLine();
        ImGui::SetNextItemWidth(S(110)); ImGui::SliderFloat("##alpha", &v.img_alpha, 0.1f, 1.f, "%.2f"); ImGui::SameLine();
        if (button("Auto-align", ui::Icon::Recent, false, ImVec2(0, 0), v.prog.fids.size() >= 2)) pv_auto_align(v);
        if (ImGui::IsItemHovered()) tooltip("Find the fiducials in the picture (bright round blobs near where the program puts them) and fit the picture to them.");
        ImGui::SameLine();
        if (button(v.pick >= 0 ? "Clicking\xE2\x80\xA6" : "Click fiducials", ui::Icon::Eye, v.pick >= 0, ImVec2(0, 0), v.prog.fids.size() >= 2)) { pv_pick_pair(v); v.pick = v.pick >= 0 ? -1 : 0; }
        if (ImGui::IsItemHovered()) tooltip("Align by hand: click the two marked fiducials on the picture.");
        ImGui::SameLine();
        if (button("Stretch", ui::Icon::None)) { v.xf = v.base = aoi::stretch_to_bbox(v.iw, v.ih, v.prog.x0, v.prog.y0, v.prog.x1, v.prog.y1); v.aligned = false; v.manual = false; v.note = "Picture stretched over the board (no fiducial correction)"; }
        ImGui::SameLine();
    } else if (v.err.empty()) { ImGui::TextDisabled("No AOI picture for this program in the job folder."); ImGui::SameLine(); }
    ImGui::Checkbox("Refs", &v.show_refs); ImGui::SameLine();
    if (button("Fit")) { v.zoom = 1; v.pan = ImVec2(0, 0); }
    ImGui::SameLine();
    if (button("Import program\xE2\x80\xA6", ui::Icon::ImportCad, false, ImVec2(0, 0), v.err.empty())) { std::wstring p = W(v.path); ImGui::CloseCurrentPopup(); defer([p]() { import_auto(p); }); }
    ImGui::SameLine();
    if (button("Open externally", ui::Icon::Open)) shell_open(v.path);
    if (v.has_img) { ImGui::TextColored(v.aligned ? ImVec4(0.45f, 0.85f, 0.5f, 1) : ImVec4(0.95f, 0.75f, 0.3f, 1), "%s", v.note.c_str()); ImGui::SameLine(); ImGui::TextDisabled("picture: %s (%dx%d)", v.img_name.c_str(), v.iw, v.ih); }
    if (!v.err.empty()) { ImGui::EndPopup(); return; }
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const float lw = std::clamp(av.x * 0.42f, S(360), S(640));
    ImGui::BeginChild("##pvl", ImVec2(lw, av.y), ImGuiChildFlags_None);
    ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##pvf", "Filter ref / part / feeder", v.filter, sizeof v.filter);
    const std::string fl = lower(v.filter);
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##pvt", 7, tf)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Ref"); ImGui::TableSetupColumn("Part", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("X"); ImGui::TableSetupColumn("Y"); ImGui::TableSetupColumn("Angle"); ImGui::TableSetupColumn("Feeder"); ImGui::TableSetupColumn("Skip");
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)v.prog.places.size(); ++i) {
            const auto& p = v.prog.places[(size_t)i];
            if (!fl.empty() && lower(p.ref).find(fl) == std::string::npos && lower(p.part).find(fl) == std::string::npos && lower(p.feeder).find(fl) == std::string::npos) continue;
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (ImGui::Selectable(p.ref.c_str(), v.sel == i, ImGuiSelectableFlags_SpanAllColumns)) v.sel = i;
            if (v.sel == i && v.scroll_sel) { ImGui::SetScrollHereY(0.4f); v.scroll_sel = false; }
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.part.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.3f", p.x);
            ImGui::TableNextColumn(); ImGui::Text("%.3f", p.y);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", p.rot);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.feeder.c_str());
            ImGui::TableNextColumn(); if (p.skip) ImGui::TextUnformatted("skip");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    pv_draw_canvas(v, ImVec2(0, av.y));
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && v.pick < 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// --- job folder actions --------------------------------------------------------
void jf_preview(const State& s, int i) {
    if (i < 0 || i >= (int)s.jf.size()) return;
    const auto& f = s.jf[(size_t)i];
    if (jf_is_program(f.kind)) { open_program_viewer(s, i); return; }
    std::vector<uint8_t> b;
    if (!read_disk(f.path, b)) { toast("Cannot read " + f.name, ToastKind::Error, 5.f); return; }
    preview::Request r;
    r.name = f.name;
    r.where = f.path;
    r.import_kinds = jz::import_kinds(f.name, b);
    r.bytes = std::move(b);
    const std::string p = f.path;
    r.open_external = [p]() { shell_open(p); };
    r.extract = [p]() { reveal_in_explorer(p); };
    r.import_as = [p](int) { preview::close(); std::wstring w = W(p); defer([w]() { import_auto(w); }); };
    preview::open(std::move(r));
}
void jf_rename(State& s, const app::Doc& d, int i) {
    if (i < 0 || i >= (int)s.jf.size()) return;
    const std::string from = s.jf[(size_t)i].path, nm = s.jf[(size_t)i].name;
    State* sp = &s; const app::Doc* dp = &d;
    defer([sp, dp, from, nm]() {
        std::wstring v = W(nm);
        if (!ui::prompt_text(main_hwnd(), L"Rename file on disk", L"New name (the file stays in the job folder):", v)) return;
        const std::string nn = jz::sanitize_file_name(U(v));
        if (nn.empty() || nn == nm) return;
        const fs::path to = upath(from).parent_path() / upath(nn);
        std::error_code ec;
        if (fs::exists(to, ec)) { toast("\"" + nn + "\" already exists in the job folder", ToastKind::Warning, 5.f); return; }
        fs::rename(upath(from), to, ec);
        if (ec) { toast("Rename failed: " + ec.message(), ToastKind::Error, 6.f); return; }
        toast("Renamed to " + nn, ToastKind::Info, 3.f);
        sp->jf_sel = u8(to);
        jf_rescan(*sp, *dp);
    });
}
void jf_delete(State& s, const app::Doc& d, int i) {
    if (i < 0 || i >= (int)s.jf.size()) return;
    const std::string p = s.jf[(size_t)i].path, nm = s.jf[(size_t)i].name;
    State* sp = &s; const app::Doc* dp = &d;
    defer([sp, dp, p, nm]() {
        if (ui::msg_box(main_hwnd(), W("Delete \"" + nm + "\" from the job folder?\n\nThe file goes to the Recycle Bin."), L"Delete file", MB_YESNO | MB_ICONWARNING) != IDYES) return;
        std::wstring from = W(p); from.push_back(L'\0');   // double-NUL list
        SHFILEOPSTRUCTW op{}; op.hwnd = main_hwnd(); op.wFunc = FO_DELETE; op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
        if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) { toast("Could not delete " + nm, ToastKind::Error, 6.f); }
        else toast("Deleted " + nm + " (Recycle Bin)", ToastKind::Info, 3.f);
        jf_rescan(*sp, *dp);
    });
}
void jf_menu(const app::DocPtr& d, State& s, int i) {
    const auto& f = s.jf[(size_t)i];
    const bool prog = jf_is_program(f.kind);
    if (menu_item_icon(prog ? "View program" : "Preview", ui::Icon::Eye, "Enter")) jf_preview(s, i);
    if (menu_item_icon("Open externally", ui::Icon::Open)) shell_open(f.path);
    if (menu_item_icon("Show in Explorer", ui::Icon::Folder)) reveal_in_explorer(f.path);
    if (menu_item_icon("Copy path", ui::Icon::Copy, "Ctrl+C")) ImGui::SetClipboardText(f.path.c_str());
    if (prog || f.kind == jz::SideKind::Other) {
        if (ImGui::BeginMenu("Import as")) {
            if (menu_item_icon(prog ? "Machine program (new project)" : "Detect content\xE2\x80\xA6", ui::Icon::ImportCad)) { std::wstring w = W(f.path); defer([w]() { import_auto(w); }); }
            ImGui::EndMenu();
        }
    }
    ImGui::Separator();
    if (f.kind != jz::SideKind::Archive && menu_item_icon("Add copy to archive\xE2\x80\xA6", ui::Icon::Plus, nullptr, false, !s.zip_path.empty())) {
        app::DocPtr dp = d; std::string p = f.path; defer([dp, p]() { offer_paths(dp, {p}, -1); });
    }
    if (menu_item_icon("Rename on disk\xE2\x80\xA6", ui::Icon::None, "F2")) jf_rename(s, *d, i);
    if (menu_item_icon("Delete\xE2\x80\xA6", ui::Icon::Delete, "Del")) jf_delete(s, *d, i);
}

// The Job folder pane (Files tab, sidebar "Job folder")
void draw_job_folder_pane(const app::DocPtr& d, State& s, float w, float h) {
    const GTheme& t = th();
    ImGui::BeginChild("##jfpane", ImVec2(w, h), ImGuiChildFlags_Border);
    ImGui::AlignTextToFramePadding();
    icon_image(ui::Icon::Folder, S(18)); ImGui::SameLine();
    ImGui::PushFont(g_font_bold); ImGui::TextUnformatted("Job folder"); ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", s.jf_dir.empty() ? "" : s.jf_dir.c_str());
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + S(8), ImGui::GetWindowWidth() - S(100)));
    if (button("Refresh", ui::Icon::Recent)) jf_rescan(s, *d);
    if (ImGui::IsItemHovered()) tooltip("Scan the folder again (F5)");
    if (d->prj.path.empty()) {
        ImGui::Dummy(ImVec2(0, S(14)));
        ImGui::TextWrapped("Save the project to see the files that sit next to it: machine programs (.h8h / .iss and their machine splits), the AOI picture, work orders and other side files.");
        ImGui::EndChild();
        return;
    }
    ImGui::TextDisabled("Files next to the project whose name starts with the project name. They stay on disk (not in the job archive).");
    if (s.jf.empty()) {
        ImGui::Dummy(ImVec2(0, S(12)));
        ImGui::TextDisabled("No files for this project in its folder.");
        ImGui::EndChild();
        return;
    }
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    int cur = jf_index(s, s.jf_sel);
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##jft", 7, tf)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Side"); ImGui::TableSetupColumn("Job ID"); ImGui::TableSetupColumn("Qty");
        ImGui::TableSetupColumn("Size"); ImGui::TableSetupColumn("Modified");
        ImGui::TableHeadersRow();
        int group = -1;
        for (int i = 0; i < (int)s.jf.size(); ++i) {
            const auto& f = s.jf[(size_t)i];
            const int g = (int)jz::side_group(f.kind);
            if (g != group) {
                group = g;
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
                ImGui::TextUnformatted(jz::side_group_label((jz::SideGroup)g));
                ImGui::PopStyleColor();
            }
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::PushID(i);
            const float ind = f.master >= 0 ? S(22) : S(4);
            ImGui::Indent(ind);
            icon_image(jf_icon(f.kind), S(16), f.master >= 0 ? 0.7f : 1.f); ImGui::SameLine();
            if (ImGui::Selectable(f.name.c_str(), s.jf_sel == f.path, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                s.jf_sel = f.path;
                if (ImGui::IsMouseDoubleClicked(0)) jf_preview(s, i);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) s.jf_sel = f.path;
            if (ImGui::BeginPopupContextItem("##jfm")) { jf_menu(d, s, i); ImGui::EndPopup(); }
            ImGui::Unindent(ind);
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(jz::side_kind_label(f.kind));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.side.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.job_id.c_str());
            ImGui::TableNextColumn(); if (f.qty >= 0) ImGui::Text("%d", f.qty);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(fmt_size(f.size).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(jf_time(f.mtime).c_str());
        }
        ImGui::EndTable();
    }
    if (focused && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        const int n = (int)s.jf.size();
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { cur = std::min(n - 1, cur + 1); s.jf_sel = s.jf[(size_t)cur].path; }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { cur = std::max(0, cur - 1); s.jf_sel = s.jf[(size_t)cur].path; }
        if (cur >= 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) jf_preview(s, cur);
        if (cur >= 0 && ImGui::IsKeyPressed(ImGuiKey_F2, false)) jf_rename(s, *d, cur);
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) jf_rescan(s, *d);
    }
    ImGui::EndChild();
}
void draw_job_folder_details(State& s, float w, float h) {
    ImGui::BeginChild("##jfdet", ImVec2(w, h), ImGuiChildFlags_Border);
    const int i = jf_index(s, s.jf_sel);
    if (i < 0) { ImGui::TextDisabled("Select a file."); ImGui::EndChild(); return; }
    const auto& f = s.jf[(size_t)i];
    icon_image(jf_icon(f.kind), S(28)); ImGui::SameLine();
    ImGui::PushFont(g_font_bold); ImGui::TextWrapped("%s", f.name.c_str()); ImGui::PopFont();
    ImGui::TextDisabled("%s", jz::side_kind_label(f.kind));
    ImGui::Separator();
    auto kv = [](const char* k, const std::string& v) { if (v.empty()) return; ImGui::TextDisabled("%s", k); ImGui::SameLine(S(90)); ImGui::TextWrapped("%s", v.c_str()); };
    kv("Side", f.side);
    if (f.master >= 0) kv("Split of", s.jf[(size_t)f.master].name);
    if (f.kind == jz::SideKind::HlcMaster || f.kind == jz::SideKind::IssMaster) {
        int n = 0; for (auto& o : s.jf) n += o.master == i;
        kv("Splits", std::to_string(n));
        const int ai = jz::aoi_for(s.jf, i);
        kv("AOI picture", ai >= 0 ? s.jf[(size_t)ai].name : std::string("none"));
    }
    kv("Job ID", f.job_id);
    if (f.qty >= 0) kv("Quantity", std::to_string(f.qty));
    kv("Size", fmt_size(f.size));
    kv("Modified", jf_time(f.mtime));
    kv("Path", f.path);
    ImGui::Dummy(ImVec2(0, S(6)));
    if (button(jf_is_program(f.kind) ? "View program" : "Preview", ui::Icon::Eye, true)) jf_preview(s, i);
    ImGui::SameLine();
    if (button("Show in Explorer", ui::Icon::Folder)) reveal_in_explorer(f.path);
    if (f.kind == jz::SideKind::AoiImage) {
        if (s.tex_key != "disk:" + f.path) {
            s.tex_key = "disk:" + f.path; s.tex_err.clear(); s.tex.release();
            std::vector<uint8_t> b; int iw = 0, ih = 0; std::vector<uint8_t> px;
            if (!read_disk(f.path, b) || !preview::decode_image(b, 1024, iw, ih, px) || !s.tex.create(iw, ih, px.data(), iw * 4)) s.tex_err = "cannot decode this image";
        }
        if (s.tex.imgui_id()) {
            const float aw = ImGui::GetContentRegionAvail().x, k = aw / std::max(1, s.tex.w);
            ImGui::Image((ImTextureID)s.tex.imgui_id(), ImVec2(s.tex.w * k, s.tex.h * k));
        } else if (!s.tex_err.empty()) ImGui::TextDisabled("%s", s.tex_err.c_str());
    }
    ImGui::EndChild();
}

struct FilesView : View {
    ViewKind kind() const override { return ViewKind::Files; }
    const char* title() const override { return "Files"; }
    ui::Icon icon() const override { return ui::Icon::Folder; }
    std::string status() const override {
        if (!doc) return {};
        auto it = g_state.find(doc.get());
        if (it == g_state.end() || !it->second->ok) return "Job archive: none";
        return "Job archive: " + disk_name(it->second->zip_path) + " - " + std::to_string(doc->prj.documents.size()) + " files";
    }
    // The shell maps Del / Ctrl+C / Ctrl+A to Edit commands: on this tab they act on the archive entry, never on
    // the project's placements.
    bool on_command(int id) override {
        if (!doc) return false;
        State& s = state_of(doc.get());
        if (s.jf_mode) {   // R23: Del / Ctrl+C act on the selected job folder file
            const int i = jf_index(s, s.jf_sel);
            if (id == app::ID_EDIT_DELETE) { if (i >= 0) jf_delete(s, *doc, i); return true; }
            if (id == app::ID_EDIT_COPY) { if (i >= 0) ImGui::SetClipboardText(s.jf_sel.c_str()); return true; }
            if (id == app::ID_EDIT_SELECT_ALL || id == app::ID_EDIT_CUT || id == app::ID_EDIT_PASTE || id == app::ID_EDIT_DUPLICATE) return true;
        }
        if (id == app::ID_EDIT_DELETE) { auto k = editable(selection(s)); if (!k.empty()) ask_delete_many(s, k); return true; }
        if (id == app::ID_EDIT_COPY) {
            std::string t; for (auto& k : selection(s)) t += (t.empty() ? "" : "\n") + base_of(key_inner(k));
            if (!t.empty()) ImGui::SetClipboardText(t.c_str());
            return true;
        }
        if (id == app::ID_EDIT_SELECT_ALL) { select_all(s); return true; }
        if (id == app::ID_EDIT_CUT || id == app::ID_EDIT_PASTE || id == app::ID_EDIT_DUPLICATE) return true;
        return false;
    }
    int cmd_state(int id) override {
        if (id == app::ID_EDIT_DELETE || id == app::ID_EDIT_COPY) { auto it = doc ? g_state.find(doc.get()) : g_state.end(); return it != g_state.end() && !it->second->sel.empty() ? 1 : 0; }
        if (id == app::ID_EDIT_SELECT_ALL) return doc && !list_.empty() ? 1 : 0;
        if (id == app::ID_EDIT_CUT || id == app::ID_EDIT_PASTE || id == app::ID_EDIT_DUPLICATE) return 0;
        return -1;
    }
    void draw(ImVec2 size) override;
private:
    void draw_banner(State& s);
    void draw_legacy(State& s);
    void draw_sidebar(State& s, float w, float h);
    void draw_pane(State& s, float w, float h);
    void draw_drop_zone(State& s, float w);
    void draw_details(State& s, float w, float h);
    void draw_sheet(State& s);
    void item_menu(State& s, const Entry& e);
    void empty_menu(State& s);
    void category_menu(State& s, const std::vector<std::string>& keys);
    void dialogs(State& s);
    void keys(State& s, const std::vector<Entry>& list);
    void activate(State& s, const Entry& e);
    void navigate(State& s, const std::string& cwd);
    void set_filter(State& s, int cat, const std::string& tag);
    void ask_rename(State& s, const std::string& name);
    void begin_rename(State& s, const std::string& key);
    void commit_rename(State& s, const std::string& name, const std::string& new_name);
    bool draw_rename_field(State& s, const Entry& e, ImVec2 pos, float w);
    void ask_delete(State& s, const std::string& name);
    void ask_delete_many(State& s, const std::vector<std::string>& names);
    void change_category(State& s, const std::vector<std::string>& keys, Cat c);
    void pick_files(State& s);
    void click_select(State& s, const Entry& e);
    void select_all(State& s) { s.msel.clear(); for (auto& e : list_) s.msel.insert(e.key); if (!list_.empty()) { if (s.sel.empty() || !s.msel.count(s.sel)) s.sel = list_[0].key; s.anchor = s.sel; } request_redraw(2); }
    std::vector<std::string> selection(const State& s) const {
        std::vector<std::string> v;
        if (!s.msel.empty()) for (auto& e : list_) if (s.msel.count(e.key)) v.push_back(e.key);
        if (v.empty() && !s.sel.empty()) v.push_back(s.sel);
        return v;
    }
    static std::vector<std::string> editable(const std::vector<std::string>& keys) {
        std::vector<std::string> v; for (auto& k : keys) if (!key_nested(k)) v.push_back(k);
        return v;
    }
    void import_menu(State& s, const std::string& key, bool folder);
    std::vector<Entry> list_;
};

// A category folder as cwd is the category list filtered to it.
// ---- R23a mouse back/forward: begin ----
static State::NavPos nav_here(const State& s) { return State::NavPos{s.cwd, s.tag, s.cat, s.jf_mode}; }
static bool nav_same(const State::NavPos& a, const State::NavPos& b) { return a.cwd == b.cwd && a.tag == b.tag && a.cat == b.cat && a.jf == b.jf; }
static void nav_record(State& s, const State::NavPos& before) {   // after a navigate / filter change
    if (s.nav_replay || nav_same(before, nav_here(s))) return;
    s.nav_back.push_back(before);
    if (s.nav_back.size() > 100) s.nav_back.erase(s.nav_back.begin());
    s.nav_fwd.clear();
}
// ---- R23a mouse back/forward: end ----
void FilesView::navigate(State& s, const std::string& cwd) {
    const State::NavPos nav_before = nav_here(s);   // R23a
    struct NavRec { State& s; const State::NavPos& b; ~NavRec() { nav_record(s, b); } } nav_rec{s, nav_before};   // R23a
    s.jf_mode = false;
    std::string c = cwd;
    if (!c.empty() && c.find('|') == std::string::npos && c.back() == '/' && c.find('/') == c.size() - 1) {
        bool known = false;
        const Cat k = jz::category_of_path(c, &known);
        if (known) { s.cat = (int)k; c.clear(); }
    }
    if (c == s.cwd) return;
    s.cwd = c;
    clear_sel(s);
    s.rename_key.clear();
    request_redraw(2);
}
void FilesView::set_filter(State& s, int cat, const std::string& tag) {
    const State::NavPos nav_before = nav_here(s);   // R23a
    struct NavRec { State& s; const State::NavPos& b; ~NavRec() { nav_record(s, b); } } nav_rec{s, nav_before};   // R23a
    s.cat = cat; s.tag = tag;
    s.jf_mode = false;
    s.cwd.clear();
    clear_sel(s); s.rename_key.clear();
    request_redraw(2);
}

void FilesView::activate(State& s, const Entry& e) {
    if (e.folder) { navigate(s, e.zipdir ? e.key + "|" : e.key); return; }
    select_one(s, e.key);
    preview_entry(doc, s, e.key);
}

void FilesView::pick_files(State& s) {
    app::DocPtr d = doc;
    const int target = s.cwd.empty() ? s.cat : -1;
    const std::string folder = (!s.cwd.empty() && !key_nested(s.cwd)) ? s.cwd.substr(0, s.cwd.size() - 1) : std::string();
    defer([d, target, folder]() {
        std::wstring p; std::vector<std::wstring> many;
        if (!ui::open_file_dialog(main_hwnd(), L"Add files to the job archive", {{L"All files", L"*.*"}}, p, true, &many)) return;
        if (many.empty() && !p.empty()) many.push_back(p);
        std::vector<std::string> u; for (auto& w : many) u.push_back(U(w));
        if (!folder.empty()) add_into_folder(d, u, folder);
        else offer_paths(d, u, target);
    });
}

void FilesView::change_category(State& s, const std::vector<std::string>& keys, Cat c) {
    jz::Edit e;
    std::set<std::string> taken;
    for (auto& it : s.zip.items()) taken.insert(lower(it.name));
    int n = 0;
    for (auto& k : keys) {
        if (key_nested(k)) continue;
        std::string to = jz::path_in_category(k, c);
        if (to == k) continue;
        const bool folder = k.back() == '/';
        if (taken.count(lower(to))) {
            std::string p = to; if (folder) p.pop_back();
            const std::string par = parent_of(p);
            to = jz::unique_name(nullptr, par.empty() ? std::string() : par.substr(0, par.size() - 1), base_of(p), taken) + (folder ? "/" : "");
        }
        taken.insert(lower(to));
        e.rename[k] = to;
        ++n;
    }
    if (!n) return;
    const std::string what = (n == 1 ? base_of(keys[0]) : std::to_string(n) + " items") + " \xE2\x86\x92 " + jz::category_label(c);
    if (apply(doc, s, e, what)) clear_sel(s);
}

void FilesView::commit_rename(State& s, const std::string& name, const std::string& new_name) {
    std::string nn = jz::sanitize_file_name(new_name);
    if (nn.empty() || nn == base_of(name)) return;
    std::string full = parent_of(name) + nn + (name.back() == '/' ? "/" : "");
    if (s.zip.find(full) >= 0) { toast("\"" + nn + "\" already exists here", ToastKind::Warning, 5.f); return; }
    jz::Edit e; e.rename[name] = full;
    app::DocPtr d = doc;
    State* sp = &s;
    defer([d, sp, e, name, full, nn]() {   // the listing is being iterated: rewrite after the frame
        if (apply_now(d, *sp, e, "Renamed to " + nn)) {
            select_one(*sp, full);
            if (sp->cwd.compare(0, name.size(), name) == 0 && name.back() == '/') sp->cwd = full + sp->cwd.substr(name.size());
        }
    });
}
void FilesView::ask_rename(State& s, const std::string& name) {   // dialog fallback (entries not in the list)
    app::DocPtr d = doc;
    State* sp = &s;
    defer([this, d, sp, name]() {
        std::wstring v = W(base_of(name));
        if (!ui::prompt_text(main_hwnd(), L"Rename", L"New name:", v)) return;
        commit_rename(*sp, name, U(v));
    });
}
// F2 / Rename edits the name in place (Enter or click away commits, Esc cancels)
void FilesView::begin_rename(State& s, const std::string& key) {
    if (key.empty() || key_nested(key)) return;
    bool listed = false; for (auto& e : list_) listed |= e.key == key;
    if (!listed) { ask_rename(s, key); return; }
    select_one(s, key);
    s.rename_key = key;
    std::snprintf(s.rename_buf, sizeof s.rename_buf, "%s", base_of(key).c_str());
    s.rename_focus = true;
    request_redraw(3);
}
bool FilesView::draw_rename_field(State& s, const Entry& e, ImVec2 pos, float w) {
    if (s.rename_key != e.key) return false;
    ImGui::SetCursorScreenPos(pos);
    ImGui::SetNextItemWidth(std::max(S(80), w));
    if (s.rename_focus) { ImGui::SetKeyboardFocusHere(); s.rename_focus = false; }
    const bool enter = ImGui::InputText("##rn", s.rename_buf, sizeof s.rename_buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape, false) && (ImGui::IsItemActive() || ImGui::IsItemDeactivated());
    const bool away = ImGui::IsItemDeactivated() && !ImGui::IsItemActive();
    if (esc) { s.rename_key.clear(); request_redraw(2); }
    else if (enter || away) {
        const std::string key = s.rename_key, nn = s.rename_buf;
        s.rename_key.clear();
        commit_rename(s, key, nn);
        request_redraw(2);
    }
    return true;
}

void FilesView::ask_delete(State& s, const std::string& name) {
    app::DocPtr d = doc; State* sp = &s;
    defer([d, sp, name]() {
        const size_t nv = sp->ok && name.back() != '/' ? jz::versions_of(sp->zip, name).size() : 0;
        if (ui::msg_box(main_hwnd(), W("Delete \"" + base_of(name) + "\"" + (name.back() == '/' ? " and everything in it" : "") +
                                      (nv ? " and its " + std::to_string(nv) + " older version(s)" : std::string()) +
                                      " from the job archive?\n\nThe previous archive is kept as " + disk_name(sp->zip_path) + ".bak."),
                        L"Delete", MB_YESNO | MB_ICONWARNING) != IDYES) return;
        jz::Edit e; e.remove.insert(name);
        if (apply(d, *sp, e, "Deleted " + base_of(name))) {
            clear_sel(*sp);
            if (name.back() == '/' && sp->cwd.compare(0, name.size(), name) == 0) sp->cwd.clear();
        }
    });
}

void FilesView::ask_delete_many(State& s, const std::vector<std::string>& names) {
    if (names.size() == 1) { ask_delete(s, names[0]); return; }
    app::DocPtr d = doc; State* sp = &s;
    defer([d, sp, names]() {
        int folders = 0; for (auto& n : names) folders += n.back() == '/';
        if (ui::msg_box(main_hwnd(), W("Delete " + std::to_string(names.size()) + " items" + (folders ? " (" + std::to_string(folders) + " folder(s) with everything in them)" : std::string()) +
                                      " from the job archive?\n\nThe previous archive is kept as " + disk_name(sp->zip_path) + ".bak."),
                        L"Delete", MB_YESNO | MB_ICONWARNING) != IDYES) return;
        jz::Edit e; for (auto& n : names) e.remove.insert(n);
        if (apply(d, *sp, e, "Deleted " + std::to_string(names.size()) + " items")) {
            clear_sel(*sp);
            for (auto& n : names) if (n.back() == '/' && sp->cwd.compare(0, n.size(), n) == 0) sp->cwd.clear();
        }
    });
}

// Explorer-style click selection
void FilesView::click_select(State& s, const Entry& e) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyShift && !s.anchor.empty()) {
        int a = -1, b = -1;
        for (int i = 0; i < (int)list_.size(); ++i) { if (list_[(size_t)i].key == s.anchor) a = i; if (list_[(size_t)i].key == e.key) b = i; }
        if (a >= 0 && b >= 0) {
            if (!io.KeyCtrl) s.msel.clear();
            for (int i = std::min(a, b); i <= std::max(a, b); ++i) s.msel.insert(list_[(size_t)i].key);
            s.sel = e.key;
            return;
        }
    }
    if (io.KeyCtrl) {
        if (s.msel.empty() && !s.sel.empty()) s.msel.insert(s.sel);
        if (s.msel.count(e.key)) { s.msel.erase(e.key); s.sel = s.msel.empty() ? std::string() : e.key == s.sel ? *s.msel.begin() : s.sel; }
        else { s.msel.insert(e.key); s.sel = e.key; }
        s.anchor = e.key;
        return;
    }
    select_one(s, e.key);
}

void FilesView::category_menu(State& s, const std::vector<std::string>& keys) {
    Cat cur = Cat::Other; bool same = true, first = true;
    for (auto& k : keys) {
        bool known = false; Cat c = jz::category_of_path(k, &known);
        if (!known) if (const DocumentRef* r = jz::index_find(doc->prj.documents, k)) c = jz::category_from_name(r->category);
        if (first) cur = c; else same &= c == cur;
        first = false;
    }
    for (int i = 0; i < kCats; ++i) {
        const Cat c = (Cat)i;
        if (menu_item_icon(jz::category_label(c), cat_icon(c), nullptr, same && c == cur)) change_category(s, keys, c);
    }
}

void FilesView::import_menu(State& s, const std::string& key, bool folder) {
    const int k = folder ? jz::ImpGerber : kinds_of(s, key);
    if (menu_item_icon("Gerbers", ui::Icon::ImportGerber, nullptr, false, (k & jz::ImpGerber) != 0)) import_as(doc, s, key, jz::ImpGerber);
    if (menu_item_icon("CAD", ui::Icon::ImportCad, nullptr, false, !folder && (k & jz::ImpCad) != 0)) import_as(doc, s, key, jz::ImpCad);
    if (menu_item_icon("BOM", ui::Icon::ImportBom, nullptr, false, !folder && (k & jz::ImpBom) != 0)) import_as(doc, s, key, jz::ImpBom);
    if (menu_item_icon("Centroid", ui::Icon::ImportCentroid, nullptr, false, !folder && (k & jz::ImpCentroid) != 0)) import_as(doc, s, key, jz::ImpCentroid);
}

void FilesView::item_menu(State& s, const Entry& e) {
    const bool ro = e.nested;                  // inside a nested zip: read-only
    const std::string full = key_inner(e.key);
    if (const auto keys = selection(s); keys.size() > 1 && is_sel(s, e.key)) {   // several selected entries
        const auto ed = editable(keys);
        const std::string n = std::to_string(keys.size());
        ImGui::TextDisabled("%s items selected", n.c_str());
        ImGui::Separator();
        if (begin_menu_icon("Change category", ui::Icon::Tag, !ed.empty())) { category_menu(s, ed); ImGui::EndMenu(); }
        std::vector<std::string> files; for (auto& k : ed) if (k.back() != '/') files.push_back(k);
        if (menu_item_icon("Add tags\xE2\x80\xA6", ui::Icon::Tag, nullptr, false, !files.empty())) { s.tags_multi = files; s.open_tags = true; }
        if (menu_item_icon(("Extract " + n + " items\xE2\x80\xA6").c_str(), ui::Icon::Export)) extract_many(s, keys);
        ImGui::Separator();
        if (menu_item_icon(("Delete " + std::to_string(ed.size()) + " items").c_str(), ui::Icon::Delete, "Del", false, !ed.empty())) ask_delete_many(s, ed);
        if (menu_item_icon("Copy names", ui::Icon::Copy, "Ctrl+C")) { std::string t; for (auto& k : keys) t += (t.empty() ? "" : "\n") + base_of(key_inner(k)); ImGui::SetClipboardText(t.c_str()); }
        if (ed.size() < keys.size()) { ImGui::Separator(); ImGui::TextDisabled("Entries inside a nested zip are read-only"); }
        return;
    }
    if (e.folder) { if (menu_item_icon(e.zipdir ? "Open (browse the zip)" : "Open", ui::Icon::Folder, "Enter")) activate(s, e); }
    else if (menu_item_icon("Preview", ui::Icon::Eye, "Enter")) preview_entry(doc, s, e.key);
    if (menu_item_icon("Open externally", ui::Icon::Open, nullptr, false, !e.folder || e.zipdir)) { std::string k = e.key; State* sp = &s; defer([sp, k]() { open_external(*sp, k); }); }
    if (menu_item_icon(e.folder && !e.zipdir ? "Extract folder\xE2\x80\xA6" : "Extract\xE2\x80\xA6", ui::Icon::Export)) extract_to(s, e.key);
    if (begin_menu_icon("Import as", ui::Icon::ImportCad)) { import_menu(s, e.key, e.folder && !e.zipdir); ImGui::EndMenu(); }
    ImGui::Separator();
    if (menu_item_icon("Rename", ui::Icon::Text, "F2", false, !ro)) begin_rename(s, full);
    if (begin_menu_icon("Change category", ui::Icon::Tag, !ro)) { category_menu(s, {full}); ImGui::EndMenu(); }
    if (menu_item_icon("Tags / Note\xE2\x80\xA6", ui::Icon::Tag, nullptr, false, !ro && !(e.folder && !e.zipdir))) { s.tags_key = full; s.tags_multi.clear(); s.open_tags = true; }
    if (menu_item_icon("Delete", ui::Icon::Delete, "Del", false, !ro)) ask_delete(s, full);
    ImGui::Separator();
    if (menu_item_icon("Copy name", ui::Icon::Copy, "Ctrl+C")) ImGui::SetClipboardText(e.name.c_str());
    if (menu_item_icon("Properties", ui::Icon::Settings, "Alt+Enter")) { s.props_key = e.key; s.open_props = true; }
    if (ro) { ImGui::Separator(); ImGui::TextDisabled("Inside a nested zip: read-only"); }
}

void FilesView::empty_menu(State& s) {
    const bool ro = key_nested(s.cwd);
    if (menu_item_icon("Add files\xE2\x80\xA6", ui::Icon::Plus, nullptr, false, !s.zip_path.empty() && !ro)) pick_files(s);
    if (menu_item_icon("Pack project folder\xE2\x80\xA6", ui::Icon::FileZip, nullptr, false, !s.zip_path.empty())) { app::DocPtr d = doc; defer([d]() { pack_folder(d); }); }
    if (menu_item_icon("Verify archive (SHA-256)", ui::Icon::Check, nullptr, false, s.ok)) { app::DocPtr d = doc; State* sp = &s; defer([d, sp]() { verify_archive(d, *sp); }); }
    if (!s.cwd.empty() && menu_item_icon("Extract this folder\xE2\x80\xA6", ui::Icon::Export)) extract_to(s, s.cwd);
    if (menu_item_icon("Refresh", ui::Icon::Recent, "F5")) reload(s, *doc, true);
    ImGui::Separator();
    if (menu_item_icon("Properties", ui::Icon::Settings)) { s.props_key = s.cwd.empty() ? std::string("<archive>") : s.cwd; s.open_props = true; }
}

void FilesView::dialogs(State& s) {
    // R25: after a verified pack, offer to move the originals out of the way (never deleted)
    if (s.open_cleanup) { ImGui::OpenPopup("Packed###jfpacked"); s.open_cleanup = false; }
    ImGui::SetNextWindowSize(ImVec2(S(520), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Packed###jfpacked", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%d file(s) packed and checked against the archive.", (int)s.pack_done.size());
        ImGui::PushTextWrapPos(S(500));
        ImGui::TextDisabled("Move the originals into a \"%s\" folder next to the archive? Nothing is deleted; machine programs, the traveller "
                            "and the AOI picture are not touched.", jz::kPackedFolder);
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        if (button("Move to _packed", ui::Icon::Move, true)) {
            std::vector<std::string> failed;
            const int n = jz::move_packed(u8(upath(s.zip_path).parent_path()), s.pack_done, failed);
            if (failed.empty()) toast("Moved " + std::to_string(n) + " original(s) to " + jz::kPackedFolder, ToastKind::Success, 4.f);
            else toast("Moved " + std::to_string(n) + "; " + std::to_string(failed.size()) + " could not be moved (in use?): " + failed[0], ToastKind::Warning, 8.f);
            s.pack_done.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (button("Keep them where they are") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { s.pack_done.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (s.open_props) { ImGui::OpenPopup("Properties###jfprops"); s.open_props = false; }
    if (s.open_tags && !s.tags_multi.empty()) { ImGui::OpenPopup("Add tags###jftagsm"); s.open_tags = false; s.dlg_tags[0] = 0; }
    ImGui::SetNextWindowSize(ImVec2(S(460), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Add tags###jftagsm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {   // R21: tag several files
        ImGui::Text("Add tags to %d files", (int)s.tags_multi.size());
        ImGui::TextDisabled("Tags (comma separated); existing tags are kept");
        ImGui::SetNextItemWidth(S(430));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##tm", s.dlg_tags, sizeof s.dlg_tags, ImGuiInputTextFlags_EnterReturnsTrue);
        bool ok = button("Add", ui::Icon::Check, true) || enter;
        ImGui::SameLine();
        bool cancel = button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
        if (ok) {
            const auto add = split_tags(s.dlg_tags);
            int n = 0;
            for (auto& k : s.tags_multi)
                if (DocumentRef* r = jz::index_find(doc->prj.documents, k)) {
                    for (auto& t : add) if (std::find(r->tags.begin(), r->tags.end(), t) == r->tags.end()) r->tags.push_back(t);
                    ++n;
                }
            if (n && !add.empty()) { doc->prj.dirty = true; doc->changed(ui::CH_INFO); s.edit_key.clear(); toast("Tagged " + std::to_string(n) + " files", ToastKind::Success, 3.f); }
        }
        if (ok || cancel) { s.tags_multi.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (s.open_tags) {
        ImGui::OpenPopup("Tags and note###jftags"); s.open_tags = false;
        DocumentRef* r = jz::index_find(doc->prj.documents, s.tags_key);
        std::snprintf(s.dlg_tags, sizeof s.dlg_tags, "%s", r ? join_tags(r->tags).c_str() : "");
        std::snprintf(s.dlg_note, sizeof s.dlg_note, "%s", r ? r->note.c_str() : "");
    }
    ImGui::SetNextWindowSize(ImVec2(S(520), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Properties###jfprops", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string& k = s.props_key;
        auto row = [](const char* a, const std::string& b) { ImGui::TextDisabled("%s", a); ImGui::SameLine(S(130)); ImGui::TextWrapped("%s", b.c_str()); };
        if (k == "<archive>") {
            icon_image(ui::Icon::FileZip, S(32)); ImGui::SameLine(); ImGui::TextUnformatted(disk_name(s.zip_path).c_str());
            ImGui::Separator();
            row("Location", s.zip_path);
            row("Size", fmt_size(s.fsize));
            row("Entries", std::to_string(s.zip.items().size()) + (s.zip.zip64() ? " (Zip64)" : ""));
            row("Indexed files", std::to_string(doc->prj.documents.size()));
        } else {
            std::string inner = key_inner(k);
            std::string ik; const jz::Zip* z = zip_of(s, k, ik);
            const int idx = z ? z->find(ik) : -1;
            const bool folder = !inner.empty() && inner.back() == '/';
            icon_image(folder ? ui::Icon::Folder : icon_of(jz::file_class(inner)), S(32)); ImGui::SameLine(); ImGui::TextUnformatted(base_of(inner).c_str());
            ImGui::Separator();
            row("Type", folder ? "File folder" : jz::file_class_label(jz::file_class(inner)));
            row("Location", key_nested(k) ? disk_name(s.zip_path) + " \xE2\x80\xBA " + k.substr(0, k.find('|')) + " \xE2\x80\xBA " + parent_of(inner) : disk_name(s.zip_path) + " \xE2\x80\xBA " + parent_of(inner));
            if (folder && z) {
                uint64_t n = 0, bytes = 0;
                for (auto& it : z->items()) if (!it.dir() && it.name.compare(0, inner.size(), inner) == 0) { ++n; bytes += it.usize; }
                row("Contains", std::to_string(n) + " files, " + fmt_size(bytes));
            } else if (idx >= 0) {
                const auto& it = z->items()[(size_t)idx];
                char b[200];
                std::snprintf(b, sizeof b, "%s (%llu bytes)", fmt_size(it.usize).c_str(), (unsigned long long)it.usize); row("Size", b);
                std::snprintf(b, sizeof b, "%s, %s", fmt_size(it.csize).c_str(), it.method == 8 ? "deflated" : it.method == 0 ? "stored" : "other method"); row("Packed", b);
                std::snprintf(b, sizeof b, "%08X", it.crc); row("CRC-32", b);
                row("Modified", jz::dos_datetime_text(it.mdate, it.mtime));
                const int kk = kinds_of(s, k);
                std::string imp;
                if (kk & jz::ImpGerber) imp += "Gerbers ";
                if (kk & jz::ImpCad) imp += "CAD ";
                if (kk & jz::ImpBom) imp += "BOM ";
                if (kk & jz::ImpCentroid) imp += "Centroid ";
                row("Import as", imp.empty() ? "-" : imp);
                if (!key_nested(k)) if (DocumentRef* r = jz::index_find(doc->prj.documents, inner)) {
                    row("Category", r->category);
                    row("Tags", r->tags.empty() ? "-" : join_tags(r->tags));
                    row("Note", r->note.empty() ? "-" : r->note);
                }
            }
            if (key_nested(k)) row("Access", "Read-only (inside a nested zip)");
        }
        ImGui::Spacing();
        if (button("Close", ui::Icon::None, true) || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowSize(ImVec2(S(460), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Tags and note###jftags", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(base_of(s.tags_key).c_str());
        ImGui::TextDisabled("Tags (comma separated)");
        ImGui::SetNextItemWidth(S(430));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##t", s.dlg_tags, sizeof s.dlg_tags);
        ImGui::TextDisabled("Note");
        ImGui::InputTextMultiline("##n", s.dlg_note, sizeof s.dlg_note, ImVec2(S(430), S(90)));
        bool ok = button("OK", ui::Icon::Check, true);
        ImGui::SameLine();
        bool cancel = button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
        if (ok) {
            if (DocumentRef* r = jz::index_find(doc->prj.documents, s.tags_key)) {
                r->tags = split_tags(s.dlg_tags); r->note = s.dlg_note;
                doc->prj.dirty = true; doc->changed(ui::CH_INFO);
                s.edit_key.clear();   // refresh the details pane editors
            }
        }
        if (ok || cancel) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void FilesView::draw_sheet(State& s) {
    if (s.open_sheet && s.sheet) { ImGui::OpenPopup("Add files to the job archive###jfsheet"); s.open_sheet = false; }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(S(860), vp->WorkSize.x * 0.9f), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Add files to the job archive###jfsheet", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    const GTheme& t = th();
    if (!s.sheet) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    jz::DropSheet& sh = *s.sheet;
    const int nfiles = sh.included_files();
    int gerber_rows = 0; for (auto& r : sh.rows) gerber_rows += r.include && r.p.gerber_set;
    ImGui::PushFont(g_font_bold);
    if (s.pack) ImGui::Text("Pack %d %s from the project folder into %s", nfiles, nfiles == 1 ? "file" : "files", disk_name(s.zip_path).c_str());
    else if (sh.target >= 0) ImGui::Text("Add %d %s to %s", nfiles, nfiles == 1 ? "file" : "files", jz::category_label((Cat)sh.target));
    else ImGui::Text("Add %d %s to the job archive", nfiles, nfiles == 1 ? "file" : "files");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
    ImGui::TextWrapped("%s", gerber_rows ? "Check the category of each item. Gerber sets: is it the original circuit, the panel or the stencil? "
                                           "(Original-circuit Gerbers usually carry more annotation than the panel set.)"
                                         : "Check the category of each item; change it in the dropdown if the suggestion is wrong.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    const float rows_h = std::min(S(440), (float)sh.rows.size() * S(gerber_rows ? 52 : 34) + S(30));
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
    if (ImGui::BeginTable("##sheet", 4, tf, ImVec2(0, rows_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(26));
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, S(230));
        ImGui::TableSetupColumn("Why", ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < sh.rows.size(); ++i) {
            jz::DropRow& r = sh.rows[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow(0, S(30));
            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox("##inc", &r.include);
            if (ImGui::IsItemHovered()) tooltip("Untick to leave this item out");
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginDisabled(!r.include);
            icon_image(r.folder ? ui::Icon::Folder : (jz::is_zip_name(r.name) ? ui::Icon::FileZip : icon_of(jz::file_class(r.name))), S(18));
            ImGui::SameLine();
            ImGui::TextUnformatted(r.name.c_str());
            if (r.folder) { ImGui::SameLine(); ImGui::TextDisabled("(folder, %d files)", r.files()); }
            if (ImGui::IsItemHovered()) tooltip(r.disk.c_str());
            if (r.p.gerber_set) {   // the three-way question, inline
                ImGui::TextDisabled("Gerber set:");
                const Cat q[3] = {Cat::Original, Cat::Panel, Cat::Stencil};
                const char* ql[3] = {"Original circuit", "Panel", "Stencil"};
                for (int k = 0; k < 3; ++k) {
                    ImGui::SameLine(0, S(4));
                    const bool on = r.cat == q[k];
                    ImGui::PushStyleColor(ImGuiCol_Button, on ? col_alpha(cat_color(q[k]), 0.55f) : col_alpha(t.text, 0.06f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col_alpha(cat_color(q[k]), 0.40f));
                    if (ImGui::SmallButton(ql[k])) r.cat = q[k];
                    ImGui::PopStyleColor(2);
                }
            }
            ImGui::EndDisabled();
            ImGui::TableSetColumnIndex(2);
            ImGui::BeginDisabled(!r.include);
            Cat c = r.cat;
            if (cat_combo("##cat", c, S(222))) r.cat = c;
            ImGui::EndDisabled();
            ImGui::TableSetColumnIndex(3);
            ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
            std::string why = r.p.why;
            if (r.cat != r.p.cat) why = (sh.target == (int)r.cat ? std::string("dropped on ") + jz::category_label(r.cat) + "; " : std::string()) + "suggested " + jz::category_label(r.p.cat) + " (" + r.p.why + ")";
            if (!r.p.tags.empty()) why += "; tag " + join_tags(r.p.tags);
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextUnformatted(why.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Set all to");
    ImGui::SameLine();
    Cat all = (Cat)s.sheet_all;
    if (cat_combo("##all", all, S(222))) { s.sheet_all = (int)all; sh.set_all(all); }
    ImGui::SameLine();
    ImGui::TextDisabled("  Datasheets go to Other with a tag. Same name, new content: the old file is kept as a version.");
    if (s.pack) {   // R25: what the pack leaves alone
        auto list = [&](const char* title, const std::vector<jz::PackSkip>& v) {
            if (v.empty()) return;
            const std::string h = std::string(title) + " (" + std::to_string(v.size()) + ")###" + title;
            if (ImGui::CollapsingHeader(h.c_str())) {
                ImGui::BeginChild(title, ImVec2(0, std::min(S(160), (float)v.size() * S(22) + S(8))), ImGuiChildFlags_Border);
                for (auto& x : v) {
                    ImGui::TextUnformatted(x.name.c_str());
                    ImGui::SameLine(S(320));
                    ImGui::TextDisabled("%s", x.why.c_str());
                }
                ImGui::EndChild();
            }
        };
        list("Stays next to the archive", s.pack->stays);
        list("Already in the archive", s.pack->already);
    }
    ImGui::Separator();
    const std::string addl = (s.pack ? "Pack " : "Add ") + std::to_string(nfiles) + (nfiles == 1 ? " file" : " files");
    const bool ok = button(addl.c_str(), ui::Icon::Plus, true, ImVec2(0, 0), nfiles > 0) || (nfiles > 0 && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !ImGui::IsAnyItemActive());
    ImGui::SameLine();
    const bool cancel = button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (ok) { State* sp = &s; app::DocPtr d = doc; ImGui::CloseCurrentPopup(); defer([d, sp]() { commit_sheet(d, *sp); }); }
    else if (cancel) { s.sheet.reset(); s.pack.reset(); ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void FilesView::keys(State& s, const std::vector<Entry>& list) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || preview::is_open()) return;
    claim_keyboard();
    int cur = -1;
    for (int i = 0; i < (int)list.size(); ++i) if (list[(size_t)i].key == s.sel) cur = i;
    const int n = (int)list.size();
    auto pick = [&](int i) {
        if (n == 0) return;
        i = std::clamp(i, 0, n - 1);
        if (io.KeyShift && !s.anchor.empty()) {   // Shift+arrow extends the range from the anchor
            int a = -1; for (int k = 0; k < n; ++k) if (list[(size_t)k].key == s.anchor) a = k;
            if (a >= 0) { s.msel.clear(); for (int k = std::min(a, i); k <= std::max(a, i); ++k) s.msel.insert(list[(size_t)k].key); s.sel = list[(size_t)i].key; }
            else select_one(s, list[(size_t)i].key);
        } else select_one(s, list[(size_t)i].key);
        s.scroll_to_sel = true; request_redraw(2);
    };
    if (io.KeyAlt) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !s.cwd.empty()) navigate(s, parent_dir(s.cwd));
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && cur >= 0) { s.props_key = list[(size_t)cur].key; s.open_props = true; }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) pick(cur < 0 ? 0 : cur + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) pick(cur < 0 ? 0 : cur - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) pick(0);
    if (ImGui::IsKeyPressed(ImGuiKey_End)) pick(n - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) pick(cur + 12);
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) pick(cur - 12);
    if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) && cur >= 0) activate(s, list[(size_t)cur]);
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !s.cwd.empty()) navigate(s, parent_dir(s.cwd));
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && cur >= 0 && !list[(size_t)cur].nested) begin_rename(s, list[(size_t)cur].key);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) select_all(s);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && s.msel.size() > 1) select_one(s, s.sel);
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) reload(s, *doc, true);
    // type-ahead: jump to the next name starting with the typed letters
    if (!io.KeyCtrl)
        for (ImWchar c : io.InputQueueCharacters) {
            if (c < 32 || c >= 127) continue;
            const double now = ImGui::GetTime();
            if (now - s.type_ahead_t > 1.0) s.type_ahead.clear();
            s.type_ahead_t = now;
            s.type_ahead += (char)std::tolower((int)c);
            for (int k = 0; k < n; ++k) {
                const int i = (std::max(cur, 0) + (s.type_ahead.size() == 1 ? 1 : 0) + k) % n;
                if (lower(list[(size_t)i].name).compare(0, s.type_ahead.size(), s.type_ahead) == 0) { pick(i); break; }
            }
        }
}

void FilesView::draw_sidebar(State& s, float w, float h) {
    const GTheme& t = th();
    ImGui::BeginChild("##cats", ImVec2(w, h), ImGuiChildFlags_Border);
    std::array<int, kCats> cnt{}; int total = 0;
    std::map<std::string, int> tagc;
    for (auto& r : doc->prj.documents) {
        cnt[(size_t)jz::category_from_name(r.category)]++; ++total;
        for (auto& tg : r.tags) tagc[tg]++;
    }
    const float rh = S(30);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto row = [&](int cat, const char* label, ui::Icon ic, int n, ImU32 col) {
        ImGui::PushID(cat + 100);
        const bool on = s.cat == cat && s.tag.empty() && s.cwd.empty();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##r", on, 0, ImVec2(0, rh))) set_filter(s, cat, "");
        const ImVec2 b = ImGui::GetItemRectMax();
        if (cat >= 0) {
            s.drop_targets.push_back({a, b, cat});
            if (ImGui::IsItemHovered()) tooltip((std::string(cat_hint((Cat)cat)) + "\nDrop files here to add them to " + label + ".").c_str());
            dl->AddRectFilled(ImVec2(a.x, a.y + S(5)), ImVec2(a.x + S(4), b.y - S(5)), col, S(2));
        }
        icon_draw(dl, ic, ImVec2(a.x + S(10), a.y + (rh - S(18)) * 0.5f), S(18), n ? 1.f : 0.55f);
        dl->AddText(ImVec2(a.x + S(34), a.y + (rh - ImGui::GetTextLineHeight()) * 0.5f), n || cat < 0 ? t.text : t.text_dim, label);
        if (n) {
            const std::string ns = std::to_string(n);
            const ImVec2 ts = ImGui::CalcTextSize(ns.c_str());
            const ImVec2 pa(b.x - ts.x - S(14), a.y + (rh - ts.y) * 0.5f - S(1));
            dl->AddRectFilled(pa, ImVec2(b.x - S(4), pa.y + ts.y + S(2)), cat >= 0 ? col_alpha(col, t.dark ? 0.30f : 0.20f) : col_alpha(t.text, 0.10f), S(8));
            dl->AddText(ImVec2(pa.x + S(5), pa.y + S(1)), t.text, ns.c_str());
        }
        ImGui::PopID();
    };
    row(-1, "All files", ui::Icon::Folder, total, t.text);
    ImGui::Dummy(ImVec2(0, S(2)));
    section_header("JOB FOLDER");   // R23: files next to the project on disk
    if (doc->prj.path.empty()) ImGui::TextDisabled("  Save the project to list\n  its programs and side files.");
    else {
        const ImVec2 a = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##jfrow", s.jf_mode, 0, ImVec2(0, rh))) { const State::NavPos nb = nav_here(s); s.jf_mode = true; nav_record(s, nb); /* R23a */ jf_rescan(s, *doc); }
        if (ImGui::IsItemHovered()) tooltip("Programs, AOI picture, work orders and machine files next to the project (on disk, not in the zip)");
        const ImVec2 b = ImGui::GetItemRectMax();
        icon_draw(dl, ui::Icon::FileCad, ImVec2(a.x + S(10), a.y + (rh - S(18)) * 0.5f), S(18), 1.f);
        dl->AddText(ImVec2(a.x + S(34), a.y + (rh - ImGui::GetTextLineHeight()) * 0.5f), t.text, "Job folder");
        int progs = 0; for (auto& f : s.jf) progs += f.kind == jz::SideKind::HlcMaster || f.kind == jz::SideKind::IssMaster;
        const std::string ns = std::to_string(s.jf.size()) + (progs ? "  \xC2\xB7 " + std::to_string(progs) + " prog" : std::string());
        const ImVec2 ts = ImGui::CalcTextSize(ns.c_str());
        dl->AddText(ImVec2(b.x - ts.x - S(8), a.y + (rh - ts.y) * 0.5f), t.text_dim, ns.c_str());
    }
    ImGui::Dummy(ImVec2(0, S(2)));
    section_header("CATEGORIES");
    for (int i = 0; i < kCats; ++i) row(i, jz::category_label((Cat)i), cat_icon((Cat)i), cnt[(size_t)i], cat_color((Cat)i));
    if (!tagc.empty()) {
        ImGui::Dummy(ImVec2(0, S(6)));
        section_header("TAGS");
        for (auto& [tg, n] : tagc) {
            ImGui::PushID(tg.c_str());
            const bool on = s.tag == tg;
            const std::string l = "#" + tg + "  (" + std::to_string(n) + ")";
            if (ImGui::Selectable(l.c_str(), on)) { if (on) set_filter(s, s.cat, ""); else set_filter(s, -1, tg); }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void FilesView::draw_drop_zone(State& s, float w) {
    const GTheme& t = th();
    const bool ro = key_nested(s.cwd);
    const float h = S(46);
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    ImGui::InvisibleButton("##dz", ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked() && !ro && !s.zip_path.empty()) pick_files(s);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ro ? t.text_faint : s.cwd.empty() && s.cat >= 0 ? cat_color((Cat)s.cat) : t.accent;
    dl->AddRectFilled(a, b, col_alpha(col, hov ? 0.12f : 0.06f), S(8));
    dashed_rect(dl, a, b, col_alpha(col, 0.8f), S(8), S(1.5f), S(5));
    std::string msg, sub;
    if (ro) { msg = "Inside a zip: read-only"; sub = "Preview, extract or import its files. Go back to add files."; }
    else if (!s.cwd.empty()) { msg = "Drop files to add to " + where_label(s.cwd); sub = "They go into this folder."; }
    else if (s.cat >= 0) { msg = std::string("Drop files to add to ") + jz::category_label((Cat)s.cat); sub = "Or click to pick files. You confirm each file before it is added."; }
    else { msg = "Drop files here to add them to the job archive"; sub = "A category is suggested for each file; you confirm before anything is added. Click to pick files."; }
    icon_draw(dl, ro ? ui::Icon::FileZip : ui::Icon::Plus, ImVec2(a.x + S(14), a.y + (h - S(22)) * 0.5f), S(22), ro ? 0.5f : 1.f);
    ImGui::PushFont(g_font_bold);
    dl->AddText(ImVec2(a.x + S(46), a.y + S(6)), t.text, msg.c_str());
    ImGui::PopFont();
    dl->PushClipRect(a, b, true);
    dl->AddText(ImVec2(a.x + S(46), a.y + S(6) + ImGui::GetTextLineHeight() + S(2)), t.text_dim, sub.c_str());
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(0, S(2)));
}

void FilesView::draw_pane(State& s, float w, float h) {
    const GTheme& t = th();
    if (s.jf_mode) { draw_job_folder_pane(doc, s, w, h); return; }
    ImGui::BeginChild("##pane", ImVec2(w, h), ImGuiChildFlags_Border);
    s.pane_a = ImGui::GetWindowPos(); s.pane_b = ImVec2(s.pane_a.x + ImGui::GetWindowWidth(), s.pane_a.y + ImGui::GetWindowHeight());
    const float iw = ImGui::GetContentRegionAvail().x;
    // where am I: category / tag / folder
    if (!s.cwd.empty()) {
        if (!resolve_dir(s, s.cwd)) s.cwd.clear();   // folder vanished (renamed / deleted outside)
    }
    if (!s.cwd.empty()) {
        if (button("Back", ui::Icon::ArrowLeft)) navigate(s, parent_dir(s.cwd));
        if (ImGui::IsItemHovered()) tooltip("Back (Backspace)");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        icon_image(key_nested(s.cwd) ? ui::Icon::FileZip : ui::Icon::Folder, S(18));
        ImGui::SameLine();
        ImGui::PushFont(g_font_bold); ImGui::TextUnformatted(where_label(s.cwd).c_str()); ImGui::PopFont();
    } else {
        ImGui::AlignTextToFramePadding();
        if (!s.tag.empty()) { icon_image(ui::Icon::Tag, S(18)); ImGui::SameLine(); ImGui::PushFont(g_font_bold); ImGui::Text("Tagged #%s", s.tag.c_str()); ImGui::PopFont(); ImGui::SameLine(); if (ImGui::SmallButton("Clear tag")) set_filter(s, -1, ""); }
        else if (s.cat >= 0) {
            const Cat c = (Cat)s.cat;
            icon_image(cat_icon(c), S(18)); ImGui::SameLine();
            ImGui::PushFont(g_font_bold); ImGui::TextUnformatted(jz::category_label(c)); ImGui::PopFont();
            ImGui::SameLine(); ImGui::TextDisabled("%s", cat_hint(c));
        } else { icon_image(ui::Icon::Folder, S(18)); ImGui::SameLine(); ImGui::PushFont(g_font_bold); ImGui::TextUnformatted("All files"); ImGui::PopFont(); }
    }
    draw_drop_zone(s, iw);
    if (!s.ok) {
        ImGui::Dummy(ImVec2(0, S(16)));
        ImGui::TextDisabled(s.zip_path.empty() ? "Save the project to get a job archive." : "No job archive yet: dropping or adding files creates it.");
        ImGui::EndChild();
        return;
    }
    list_ = s.cwd.empty() ? list_categories(*doc, s) : list_dir(*doc, s);
    ImGui::BeginChild("##list", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs);   // own arrow-key navigation (keys())
    if (ImGui::IsWindowFocused()) keys(s, list_);
    if (list_.empty()) {
        ImGui::Dummy(ImVec2(0, S(12)));
        ImGui::TextDisabled("%s", !s.cwd.empty() ? "This folder is empty." : !s.tag.empty() ? "No file has this tag." : s.cat >= 0 ? "Nothing in this category yet." : "The job archive is empty.");
    }
    auto item_common = [&](const Entry& e) {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) { if (s.rename_key != e.key) s.rename_key.clear(); click_select(s, e); }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !is_sel(s, e.key)) select_one(s, e.key);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { Entry ec = e; activate(s, ec); }
        if (ImGui::BeginPopupContextItem("##ictx")) { if (!is_sel(s, e.key)) select_one(s, e.key); item_menu(s, e); ImGui::EndPopup(); }
        if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
            std::string tip = e.name + "\n" + e.type + (e.where.empty() ? std::string() : "\nin " + where_label(e.where));
            if (!e.note.empty()) tip += "\n\n" + e.note;
            tooltip(tip.c_str());
        }
        if (s.scroll_to_sel && s.sel == e.key) { ImGui::SetScrollHereY(0.5f); s.scroll_to_sel = false; }
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!list_.empty()) {
        ImGuiTableFlags tf = ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                             ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("##det", 5, tf)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_NoHide, 3.f, 0);
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, S(176), 1);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, S(76), 2);
            ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, S(126), 3);
            ImGui::TableSetupColumn("Tags", ImGuiTableColumnFlags_WidthFixed, S(120), 4);
            ImGui::TableHeadersRow();
            if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs())
                if (sp->SpecsDirty && sp->SpecsCount > 0) {
                    s.sort_col = (int)sp->Specs[0].ColumnUserID; s.sort_asc = sp->Specs[0].SortDirection != ImGuiSortDirection_Descending;
                    sp->SpecsDirty = false; sort_entries(list_, s.sort_col, s.sort_asc);
                }
            const float rh = std::max(ImGui::GetTextLineHeight(), S(18)) + S(8);
            for (const Entry& e : list_) {
                ImGui::PushID(e.key.c_str());
                ImGui::TableNextRow(0, rh);
                ImGui::TableSetColumnIndex(0);
                const bool selected = is_sel(s, e.key);
                ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rh));
                const ImVec2 a = ImGui::GetItemRectMin();
                item_common(e);
                const float isz = S(18);
                if (e.cat_known) dl->AddRectFilled(ImVec2(a.x, a.y + S(3)), ImVec2(a.x + S(3), a.y + rh - S(3)), cat_color(e.cat), S(1.5f));   // colour edge
                icon_draw(dl, icon_of(e), ImVec2(a.x + S(7), a.y + (rh - isz) * 0.5f), isz);
                const float ty = a.y + (rh - ImGui::GetTextLineHeight()) * 0.5f;
                const bool renaming = draw_rename_field(s, e, ImVec2(a.x + isz + S(10), a.y + (rh - ImGui::GetFrameHeight()) * 0.5f), ImGui::GetColumnWidth(0) - isz - S(16));
                if (!renaming) {
                    dl->AddText(ImVec2(a.x + isz + S(12), ty), t.text, e.name.c_str());
                    float nx = a.x + isz + S(18) + ImGui::CalcTextSize(e.name.c_str()).x;
                    auto badge = [&](const char* txt, ImU32 c) {
                        const ImVec2 bs = ImGui::CalcTextSize(txt);
                        dl->AddRectFilled(ImVec2(nx, ty - S(1)), ImVec2(nx + bs.x + S(10), ty + bs.y + S(1)), col_alpha(c, 0.25f), S(6));
                        dl->AddText(ImVec2(nx + S(5), ty), t.text_dim, txt);
                        nx += bs.x + S(14);
                    };
                    if (e.zipdir) badge("zip", t.warn);
                    else if (e.folder) badge(e.type.c_str() + 8, t.text);   // "N files"
                    if (!e.where.empty() && !s.tag.empty()) badge(where_label(e.where).c_str(), t.text);
                }
                ImGui::TableSetColumnIndex(1);
                if (e.cat_known) {
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    chip(dl, ImVec2(p.x, a.y + (rh - S(20)) * 0.5f), e.cat, S(20), true);
                }
                auto cell = [&](int col, const std::string& txt, bool right = false) {
                    ImGui::TableSetColumnIndex(col);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rh - ImGui::GetTextLineHeight()) * 0.5f - S(1));
                    if (right) { const float tw = ImGui::CalcTextSize(txt.c_str()).x; ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - tw)); }
                    ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
                    ImGui::TextUnformatted(txt.c_str());
                    ImGui::PopStyleColor();
                };
                cell(2, fmt_size(e.size), true);
                cell(3, e.modified);
                cell(4, e.tags);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    // click on empty space clears the selection; right-click opens the pane menu
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(0)) { clear_sel(s); s.rename_key.clear(); }
    if (ImGui::BeginPopupContextWindow("##emptyctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) { empty_menu(s); ImGui::EndPopup(); }
    ImGui::EndChild();
    ImGui::EndChild();
}

void FilesView::draw_banner(State& s) {
    const GTheme& t = th();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col_alpha(t.warn, 0.14f));
    ImGui::BeginChild("##banner", ImVec2(-1, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
    icon_image(ui::Icon::Warning, S(18));
    ImGui::SameLine();
    if (s.zip_path.empty()) {
        ImGui::TextWrapped("The project has not been saved yet. The job archive \"<Project Name>.zip\" is kept next to the .pcbp file; save the project first.");
    } else {
        ImGui::TextWrapped("No job archive yet (%s). Dropping or adding files creates it.", disk_name(s.zip_path).c_str());
        if (!doc->prj.documents.empty()) { ImGui::SameLine(); ImGui::TextDisabled("(the project lists %d files)", (int)doc->prj.documents.size()); }
        if (button("Locate existing\xE2\x80\xA6", ui::Icon::Open)) {
            app::DocPtr d = doc;
            defer([d]() {
                std::wstring p;
                if (!ui::open_file_dialog(main_hwnd(), L"Locate the job archive", {{L"Zip archives", L"*.zip"}}, p)) return;
                std::error_code ec;
                fs::path rel = d->prj.path.empty() ? upath(U(p)) : fs::relative(upath(U(p)), upath(d->prj.path).parent_path(), ec);
                d->prj.documents_archive = u8(ec || rel.empty() ? upath(U(p)) : rel);
                d->prj.dirty = true; d->changed(ui::CH_INFO);
                State& s = state_of(d.get()); reload(s, *d, true); sync_quiet(d, s);
                toast("Job archive linked: " + U(p), ToastKind::Success, 4.f);
            });
        }
        ImGui::SameLine();
        if (button("Create empty", ui::Icon::New)) { jz::Edit e; apply(doc, s, e, "Created " + disk_name(s.zip_path)); }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// R19-R21 folder names inside the zip: the list already shows the new categories; renaming the folders is offered.
void FilesView::draw_legacy(State& s) {
    const GTheme& t = th();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col_alpha(t.accent, 0.10f));
    ImGui::BeginChild("##legacy", ImVec2(-1, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
    icon_image(ui::Icon::Folder, S(18));
    ImGui::SameLine();
    std::string names;
    for (auto& n : s.legacy) {
        const jz::LegacyFolder* l = jz::legacy_folder(n);
        names += (names.empty() ? "" : ", ") + n + " \xE2\x86\x92 " + (l ? jz::category_folder(l->to) : "?") + (l && l->tag ? std::string(" (tag ") + l->tag + ")" : std::string());
    }
    ImGui::TextWrapped("This job archive still uses the old folder names. The list already shows the new categories; "
                       "rename the folders inside the zip as well? %s", names.c_str());
    if (button("Rename folders", ui::Icon::Check, true)) {
        jz::Edit e;
        if (jz::plan_folder_migration(s.zip, e)) apply(doc, s, e, "Folders renamed to the new categories (previous archive kept as .bak)");
    }
    ImGui::SameLine();
    if (button("Not now")) s.legacy_later = true;
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void FilesView::draw_details(State& s, float w, float h) {
    const GTheme& t = th();
    ImGui::BeginChild("##details", ImVec2(w, h), ImGuiChildFlags_Border);
    if (const auto keys = selection(s); keys.size() > 1) {   // several entries selected
        const auto ed = editable(keys);
        icon_image(ui::Icon::Folder, S(36));
        ImGui::SameLine();
        ImGui::PushFont(g_font_bold); ImGui::Text("%d items selected", (int)keys.size()); ImGui::PopFont();
        uint64_t bytes = 0; int files = 0;
        for (auto& k : keys) { std::string in; const jz::Zip* z = zip_of(s, k, in); const int i = z ? z->find(in) : -1; if (i >= 0 && !z->items()[(size_t)i].dir()) { bytes += z->items()[(size_t)i].usize; ++files; } }
        ImGui::TextDisabled("%d file(s), %s", files, fmt_size(bytes).c_str());
        ImGui::Separator();
        if (!ed.empty()) {
            ImGui::TextDisabled("Change category");
            Cat c = Cat::Other; bool same = true;
            for (size_t i = 0; i < ed.size(); ++i) { const Cat k = jz::category_of_path(ed[i]); if (i == 0) c = k; else same &= k == c; }
            Cat pick = c;
            if (cat_combo("##mcat", pick, -1) && (!same || pick != c)) change_category(s, ed, pick);
        }
        if (button("Extract\xE2\x80\xA6", ui::Icon::Export)) extract_many(s, keys);
        ImGui::SameLine();
        std::vector<std::string> fk; for (auto& k : ed) if (k.back() != '/') fk.push_back(k);
        if (button("Add tags\xE2\x80\xA6", ui::Icon::Tag, false, ImVec2(0, 0), !fk.empty())) { s.tags_multi = fk; s.open_tags = true; }
        ImGui::SameLine();
        if (button("Delete", ui::Icon::Delete, false, ImVec2(0, 0), !ed.empty())) ask_delete_many(s, ed);
        ImGui::EndChild();
        return;
    }
    if (s.sel.empty()) {
        ImGui::TextDisabled("Select a file to see its details.");
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Double-click or Enter previews it inside PCB Plotter. F2 renames; Change category moves it to another category. Right-click for more.");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        return;
    }
    std::string inner; const jz::Zip* z = zip_of(s, s.sel, inner);
    const int idx = z ? z->find(inner) : -1;
    const bool nested = key_nested(s.sel);
    const bool is_file = !inner.empty() && inner.back() != '/';
    icon_image(is_file ? (jz::is_zip_name(inner) && !nested ? ui::Icon::FileZip : icon_of(jz::file_class(inner))) : ui::Icon::Folder, S(36));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - S(70));
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted(base_of(inner).c_str());
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
    if (idx >= 0) {
        const auto& it = z->items()[(size_t)idx];
        ImGui::Text("%s  -  %s", fmt_size(it.usize).c_str(), jz::dos_datetime_text(it.mdate, it.mtime).c_str());
    }
    ImGui::TextWrapped("%s", where_label(nested ? s.sel.substr(0, s.sel.find('|')) + "/" + parent_of(inner) : parent_of(inner)).c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    if (is_file && s.kinds_key != s.sel) {
        std::vector<uint8_t> b; std::string err;
        s.kinds = read_key(s, s.sel, b, err) ? jz::import_kinds(inner, b) : 0;
        s.kinds_key = s.sel;
    }
    // category + tags + note (not inside a nested zip: the index is per archive entry)
    if (!nested) {
        ImGui::Spacing();
        bool known = false;
        Cat c = jz::category_of_path(inner, &known);
        DocumentRef* r = is_file ? jz::index_find(doc->prj.documents, inner) : nullptr;
        if (!known && r) c = jz::category_from_name(r->category);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Category");
        ImGui::SameLine(S(80));
        Cat pick = c;
        if (cat_combo("##dcat", pick, -1) && pick != c) change_category(s, {inner}, pick);
        if (ImGui::IsItemHovered()) tooltip("Change category: moves the file to that category's folder in the job archive");
        if (is_file) {
            if (s.edit_key != s.sel) {
                s.edit_key = s.sel;
                std::snprintf(s.tags, sizeof s.tags, "%s", r ? join_tags(r->tags).c_str() : "");
                std::snprintf(s.note, sizeof s.note, "%s", r ? r->note.c_str() : "");
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Tags");
            ImGui::SameLine(S(80));
            ImGui::SetNextItemWidth(-1);
            bool ch = ImGui::InputTextWithHint("##tags", "e.g. rev B, customer (comma separated)", s.tags, sizeof s.tags);
            ImGui::SetNextItemWidth(-1);
            ch |= ImGui::InputTextMultiline("##note", s.note, sizeof s.note, ImVec2(-1, S(48)));
            if (ImGui::IsItemHovered() && !s.note[0]) tooltip("Note");
            if (ch && r) {
                r->tags = split_tags(s.tags); r->note = s.note;
                doc->prj.dirty = true; doc->changed(ui::CH_INFO);
            }
        }
    }
    // R25: content hash and the older versions of this file
    if (is_file && !nested && idx >= 0) {
        DocumentRef* r = jz::index_find(doc->prj.documents, inner);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("SHA-256");
        ImGui::SameLine(S(80));
        if (r && !r->sha256.empty()) {
            ImGui::TextDisabled("%.16s\xE2\x80\xA6", r->sha256.c_str());
            if (ImGui::IsItemHovered()) tooltip((r->sha256 + "\nClick to copy").c_str());
            if (ImGui::IsItemClicked()) ImGui::SetClipboardText(r->sha256.c_str());
        } else if (r && ImGui::SmallButton("Compute")) {
            r->sha256 = jz::entry_sha256(*z, z->items()[(size_t)idx]);
            doc->prj.dirty = true; doc->changed(ui::CH_INFO);
        }
        const auto vs = jz::versions_of(*z, inner);
        if (!vs.empty()) {
            ImGui::TextDisabled("Older versions (%d)", (int)vs.size());
            for (size_t k = 0; k < vs.size(); ++k) {
                const jz::Item* v = vs[k];
                ImGui::PushID((int)k);
                ImGui::BulletText("%s  %s", jz::dos_datetime_text(v->mdate, v->mtime).c_str(), fmt_size(v->usize).c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Extract")) extract_to(s, v->name);
                ImGui::SameLine();
                if (ImGui::SmallButton("Restore")) {
                    jz::Edit e;
                    if (jz::plan_restore(*z, v->name, e)) apply(doc, s, e, "Restored the version of " + jz::dos_datetime_text(v->mdate, v->mtime) + "; the current file is kept as a version");
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::Spacing();
    if (is_file) {
        const bool zipf = !nested && jz::is_zip_name(inner);
        if (zipf) { if (button("Browse", ui::Icon::Folder, true)) navigate(s, inner + "|"); }
        else if (button("Preview", ui::Icon::Eye, true)) preview_entry(doc, s, s.sel);
        ImGui::SameLine();
        if (button("Extract\xE2\x80\xA6", ui::Icon::Export)) extract_to(s, s.sel);
        const float ow = ImGui::CalcTextSize("Open externally").x + S(44);
        if (ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - ImGui::GetItemRectMax().x - S(8) > ow) ImGui::SameLine();   // else wrap
        if (button("Open externally", ui::Icon::Open)) { State* sp = &s; std::string k = s.sel; defer([sp, k]() { open_external(*sp, k); }); }
        if (s.kinds > 0) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Import as:");
            const char* names[] = {"Gerbers", "CAD", "BOM", "Centroid"};
            const int bits[] = {jz::ImpGerber, jz::ImpCad, jz::ImpBom, jz::ImpCentroid};
            for (int i = 0; i < 4; ++i) if (s.kinds & bits[i]) { ImGui::SameLine(); if (ImGui::SmallButton(names[i])) import_as(doc, s, s.sel, bits[i]); }
        }
    } else {
        if (button("Open", ui::Icon::Folder, true)) navigate(s, s.sel);
        ImGui::SameLine();
        if (button("Extract\xE2\x80\xA6", ui::Icon::Export)) extract_to(s, s.sel);
        ImGui::SameLine();
        if (button("Import as Gerbers", ui::Icon::ImportGerber)) import_as(doc, s, s.sel, jz::ImpGerber);
    }
    // inline preview: images
    if (is_file && jz::is_image_name(inner)) {
        ImGui::Separator();
        if (s.tex_key != s.sel) {
            s.tex_key = s.sel; s.tex.release(); s.tex.w = s.tex.h = 0; s.tex_err.clear();
            std::vector<uint8_t> b, px; std::string err; int iw = 0, ih = 0;
            if (!read_key(s, s.sel, b, err)) s.tex_err = err;
            else if (!preview::decode_image(b, 2048, iw, ih, px) || !s.tex.create(iw, ih, px.data(), iw * 4)) s.tex_err = "cannot decode this image";
            else { s.tex.w = iw; s.tex.h = ih; }
        }
        if (s.tex.imgui_id() && s.tex.w > 0) {
            const ImVec2 av = ImGui::GetContentRegionAvail();
            const float k = std::min(av.x / s.tex.w, std::max(S(60), av.y) / s.tex.h);
            ImGui::Image((ImTextureID)s.tex.imgui_id(), ImVec2(s.tex.w * std::min(k, 4.f), s.tex.h * std::min(k, 4.f)));
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) preview_entry(doc, s, s.sel);
            if (ImGui::IsItemHovered()) tooltip("Double-click: open in the preview window (zoom / pan)");
        } else ImGui::TextDisabled("%s", s.tex_err.c_str());
    }
    ImGui::EndChild();
}

void FilesView::draw(ImVec2 size) {
    if (!doc) return;
    State& s = state_of(doc.get());
    {   // cheap stat each ~second; reload when the archive changed on disk
        static double last = 0;
        const double now = ImGui::GetTime();
        if (now - last > 1.0 || s.zip_path != archive_path(*doc)) {
            last = now;
            const bool was = s.ok; const auto mt = s.mtime;
            reload(s, *doc, false);
            if (s.ok && (!was || mt != s.mtime)) sync_quiet(doc, s);   // also migrates R19-R21 index categories
        }
    }
    {   // automation / screenshots: PCBP_TEST_R22C="cat=N|tag=T|open=Folder/|sel=name|later|drop=p1;p2|dropcat=N|preview=name"
        static bool done = false;
        const char* ev = getenv("PCBP_TEST_R22C");
        if (!done && ev && s.ok) {
            done = true;
            std::string all = ev, drop; int dropcat = -1;
            size_t a = 0;
            while (a <= all.size()) {
                size_t b = all.find('|', a); if (b == std::string::npos) b = all.size();
                const std::string tok = all.substr(a, b - a), k = tok.substr(0, tok.find('=')), v = tok.find('=') == std::string::npos ? std::string() : tok.substr(tok.find('=') + 1);
                if (k == "cat") set_filter(s, atoi(v.c_str()), "");
                else if (k == "tag") set_filter(s, -1, v);
                else if (k == "open") navigate(s, v);
                else if (k == "sel") { select_one(s, v); s.kinds_key.clear(); }
                else if (k == "later") s.legacy_later = true;
                else if (k == "drop") drop = v;
                else if (k == "dropcat") dropcat = atoi(v.c_str());
                else if (k == "preview") { app::DocPtr d = doc; State* sp = &s; defer([d, sp, v]() { preview_entry(d, *sp, v); }); }
                a = b + 1;
            }
            if (!drop.empty()) {
                std::vector<std::string> ps; size_t p = 0;
                while (p <= drop.size()) { size_t q = drop.find(';', p); if (q == std::string::npos) q = drop.size(); if (q > p) ps.push_back(drop.substr(p, q - p)); p = q + 1; }
                app::DocPtr d = doc; defer([d, ps, dropcat]() { offer_paths(d, ps, dropcat); });
            }
        }
    }
    // ---- R23a mouse back/forward: XBUTTON1 = back, XBUTTON2 = forward (ImGui mouse buttons 3 / 4) ----
    if (!preview::is_open() && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        auto go = [&](std::vector<State::NavPos>& from, std::vector<State::NavPos>& to) {
            if (from.empty()) return;
            to.push_back(nav_here(s));
            const State::NavPos p = from.back(); from.pop_back();
            s.nav_replay = true;
            set_filter(s, p.cat, p.tag);
            if (!p.cwd.empty()) navigate(s, p.cwd);
            s.jf_mode = p.jf;
            s.nav_replay = false;
            request_redraw(2);
        };
        if (ImGui::IsMouseClicked(3)) go(s.nav_back, s.nav_fwd);
        else if (ImGui::IsMouseClicked(4)) go(s.nav_fwd, s.nav_back);
    }
    // ---- R23a mouse back/forward: end ----
    {   // R23: rescan the job folder when the tab becomes active (cheap: one directory listing)
        const int fr = ImGui::GetFrameCount();
        const double now = ImGui::GetTime();
        if (fr - s.jf_frame > 2 || now - s.jf_time > 1.0 || s.jf_dir != jf_dir_of(*doc)) jf_rescan(s, *doc);   // gap = tab was hidden / app idle
        s.jf_frame = fr; s.jf_time = now;
    }
    {   // automation / screenshots: PCBP_TEST_R23B="jf|sel=<file name>|prog=<file name>|pick"
        static bool done = false;
        const char* ev = getenv("PCBP_TEST_R23B");
        if (!done && ev) {
            done = true;
            std::string all = ev; size_t a = 0;
            while (a <= all.size()) {
                size_t b = all.find('|', a); if (b == std::string::npos) b = all.size();
                const std::string tok = all.substr(a, b - a), k = tok.substr(0, tok.find('=')), v = tok.find('=') == std::string::npos ? std::string() : tok.substr(tok.find('=') + 1);
                if (k == "jf") s.jf_mode = true;
                else if (k == "sel" || k == "prog") { for (int i = 0; i < (int)s.jf.size(); ++i) if (s.jf[(size_t)i].name == v) { s.jf_sel = s.jf[(size_t)i].path; if (k == "prog") open_program_viewer(s, i); } }
                else if (k == "pick") { pv_pick_pair(g_pv); g_pv.pick = 0; }
                else if (k == "sel1") g_pv.sel = atoi(v.c_str());
                a = b + 1;
            }
        }
    }
    s.drop_targets.clear();
    ImGui::BeginChild("##files", size, 0, ImGuiWindowFlags_NoScrollbar);
    // header
    icon_image(ui::Icon::FileZip, S(20));
    ImGui::SameLine();
    ImGui::PushFont(g_font_bold); ImGui::TextUnformatted("Job files"); ImGui::PopFont();
    ImGui::SameLine();
    if (s.ok) ImGui::TextDisabled("%s  \xC2\xB7  %s, %d files%s", disk_name(s.zip_path).c_str(), fmt_size(s.fsize).c_str(), (int)doc->prj.documents.size(), s.zip.zip64() ? ", Zip64" : "");
    else ImGui::TextDisabled("%s", s.zip_path.empty() ? "no archive (project not saved)" : disk_name(s.zip_path).c_str());
    const float bw = ImGui::CalcTextSize("Add files...Pack folder...Show in folder").x + S(160);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + S(12), size.x - bw));
    if (button("Add files\xE2\x80\xA6", ui::Icon::Plus, true, ImVec2(0, 0), !s.zip_path.empty() && !key_nested(s.cwd))) pick_files(s);
    ImGui::SameLine();
    if (button("Pack folder\xE2\x80\xA6", ui::Icon::FileZip, false, ImVec2(0, 0), !s.zip_path.empty())) { app::DocPtr d = doc; defer([d]() { pack_folder(d); }); }
    if (ImGui::IsItemHovered())
        tooltip("Put everything in the project's folder into the job archive, sorted into categories.\n"
                "Machine programs (.h8h / .h81.., .iss / .i01..), the M1 traveller, the AOI picture and the project's machine side files stay next to it.");
    ImGui::SameLine();
    if (button("Show in folder", ui::Icon::Open, false, ImVec2(0, 0), s.exists)) {
        std::wstring a = L"/select,\"" + W(s.zip_path) + L"\"";
        ShellExecuteW(main_hwnd(), L"open", L"explorer.exe", a.c_str(), nullptr, SW_SHOWNORMAL);
    }
    if (!s.exists || s.zip_path.empty()) draw_banner(s);
    else if (!s.ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, th().error);
        ImGui::TextWrapped("The job archive cannot be read: %s", s.err.c_str());
        ImGui::PopStyleColor();
    } else if (!s.legacy.empty() && !s.legacy_later) draw_legacy(s);
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const float tw = std::clamp(av.x * 0.17f, S(200), S(260));
    const float dw = std::clamp(av.x * 0.24f, S(260), S(400));
    draw_sidebar(s, tw, av.y);
    ImGui::SameLine();
    draw_pane(s, av.x - tw - dw - S(16), av.y);
    ImGui::SameLine();
    if (s.jf_mode) draw_job_folder_details(s, dw, av.y); else draw_details(s, dw, av.y);
    dialogs(s);
    draw_sheet(s);
    ImGui::EndChild();
    preview::draw();
    draw_program_viewer();
}

std::unique_ptr<View> make_files_view() { return std::make_unique<FilesView>(); }
const bool reg_ = register_view_factory(ViewKind::Files, make_files_view);

} // namespace

// Drop onto the app: with the Files tab active, the dropped files / folders go to the confirm sheet (a category
// proposed for each; dropped on a sidebar category = that category preselected; an open folder takes them as they
// are). On other tabs the normal import runs and a toast offers to keep a copy in the job archive. true = consumed.
bool job_files_drop(const app::DocPtr& d, const std::vector<std::wstring>& paths) {
    if (!d || paths.empty()) return false;
    std::vector<std::string> u; for (auto& w : paths) u.push_back(U(w));
    DocUI* ui_ = docui_of(d.get());
    if (ui_ && ui_->active == ViewKind::Files) {
        State& s = state_of(d.get());
        POINT pt{}; GetCursorPos(&pt); ScreenToClient(main_hwnd(), &pt);
        const ImVec2 m((float)pt.x, (float)pt.y);
        auto in = [&](ImVec2 a, ImVec2 b) { return m.x >= a.x && m.y >= a.y && m.x < b.x && m.y < b.y; };
        int target = -1;
        bool hit = false;
        for (auto& t : s.drop_targets) if (in(t.a, t.b)) { target = t.cat; hit = true; break; }
        const bool in_pane = in(s.pane_a, s.pane_b);
        if (!hit && in_pane && !s.cwd.empty() && !key_nested(s.cwd)) { app::DocPtr dp = d; std::string f = s.cwd.substr(0, s.cwd.size() - 1); defer([dp, u, f]() { add_into_folder(dp, u, f); }); return true; }
        if (!hit && s.cwd.empty()) target = s.cat;
        app::DocPtr dp = d;
        defer([dp, u, target]() { offer_paths(dp, u, target); });
        return true;
    }
    if (!d->prj.path.empty() && std::none_of(u.begin(), u.end(), [](const std::string& p) { return lower(ext_of(p)) == "pcbp"; })) {
        app::DocPtr dp = d;
        toast(std::to_string(u.size()) + (u.size() == 1 ? " item" : " items") + " dropped. Keep a copy in the job archive?", ToastKind::Info, 10.f,
              "Add to Files", [dp, u]() { offer_paths(dp, u, -1); });
    }
    return false;
}

} // namespace pcbp::gui
