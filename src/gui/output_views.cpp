// ============================================================================
// output_views.cpp — model-point output views of the v2 UI:
//   * Visual Aid  — printable page (header block, colour-coded board image,
//                   BOM block, annotations) with zoom/pan, print + 1:1.
//   * Machine     — "Programming" (machine setup form + job preview) and
//                   "Machine View" (board in machine coordinates, angle tools).
//   * Notes       — per-point notes + project revision notes.
// plus app::generate_program() (machine program generation through the
// MachineJob exporter plugins), shared with the factory panel / menu.
//
// Visual-aid layout + annotations are stored as JSON under "__visual_aid" in
// ModelPoint::machine.options (keys starting with "__" never reach exporters).
// Logic ported from legacy/app/output_view.cpp.
// ============================================================================
#include "gapp.h"
#include "gcanvas.h"
#include "../app/board_render.h"
#include "../core/fileio.h"
#include "../core/json.h"
#include "../core/machine_job.h"
#include "../core/export_preflight.h"
#include "../core/project_io.h"
#include "../core/sides.h"
#include "../io/juki_writer.h"
#include "../io/io_util.h"
#include "../io/flexdb.h"
#include "../ui/d2d.h"
#include "../ui/form.h"
#include "../ui/theme.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <shellapi.h>
#include <shlobj.h>

namespace pcbp::app {
bool export_all_sides(HWND owner, Doc& d, int link, std::string& err);
int written_mark_count(const MachineJob& j);   // R21 (below)
// R21 (AUDIT_V20 P0): the review list comes from the ONE core preflight (core/export_preflight.h).
// point < 0 = the linked pair of export layer -point-1 (kept for the view's callers).
std::vector<ExportIssue> export_validation(const Project& p, int point) {
    if (point < 0) return export_preflight(p, ExportRequest{ExportEntry::ExportViewPair, -point - 1});
    return export_preflight(p, ExportRequest{ExportEntry::ExportViewLayer, point});
}
// R18d/R21: validation never blocks. One confirmation lists errors apart from warnings: [Generate anyway] [Cancel];
// with errors the header says so in red, OK is drawn as destructive and Cancel is the default (Enter) button.
bool confirm_generate(HWND owner, const wchar_t* title, const std::vector<ExportIssue>& iss) {
    int ne = 0, nw = 0;
    for (auto& i : iss) if (i.sev == ExportIssue::Error) ++ne; else if (i.sev == ExportIssue::Warning) ++nw;
    if (!ne && !nw) return true;
    ui::Form f(title, 680);
    if (ne) f.header(ui::W(std::to_string(ne) + " ERROR(S)" + (nw ? " and " + std::to_string(nw) + " warning(s)" : std::string()) + " - generate anyway?"))
             .icon((int)ui::Icon::ErrorX);
    else f.header(ui::W(std::to_string(nw) + " warning(s) - generate anyway?")).icon((int)ui::Icon::Warning);
    if (ne) f.subtitle(L"The program is probably wrong for the machine. Cancel unless you are sure.");
    // notes (Info) are never listed here
    if (ne) {
        std::vector<ExportIssue> e; for (auto& i : iss) if (i.sev == ExportIssue::Error) e.push_back(i);
        f.heading(L"ERRORS (" + std::to_wstring(ne) + L") - the program is probably wrong");
        f.label(ui::W(export_review_text(e, 12, false)));
    }
    if (nw) {
        std::vector<ExportIssue> w; for (auto& i : iss) if (i.sev == ExportIssue::Warning) w.push_back(i);
        f.heading(L"Warnings (" + std::to_wstring(nw) + L") - review");
        f.label(ui::W(export_review_text(w, ne ? 6 : 12, false)));
    }
    f.label(L"The full list is in Validation on the Export page. Check the program in the machine software before production.");
    f.ok_text(L"Generate anyway");
    f.cancel_text(L"Cancel");
    if (ne) f.danger().default_cancel();
    return f.run(owner);
}
}
namespace pcbp::gui {
using app::Doc;
using app::DocPtr;
using ui::U;
using ui::W;
using namespace pcbp::app;   // command ids, services

// FlexLine tooling readiness (flexdb_view.cpp)
bool tooling_readiness(const app::DocPtr& d, const std::set<std::string>* pns, flexdb::Readiness& out);
void tooling_open(const app::DocPtr& d, bool find_next);

namespace {

const char* kVaKey = "__visual_aid";
const char* kCancelled = "Program generation cancelled.";

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
D2D1_MATRIX_3X2_F to_d2d(const Xform& x) {
    return D2D1::Matrix3x2F((float)x.a, (float)x.b, (float)x.c, (float)x.d, (float)x.tx, (float)x.ty);
}
D2D1_POINT_2F xfp(const D2D1_MATRIX_3X2_F& m, double x, double y) {
    return D2D1::Point2F((float)(x * m._11 + y * m._21 + m._31), (float)(x * m._12 + y * m._22 + m._32));
}
ImU32 im_col(uint32_t argb, int a = 255) { return IM_COL32((argb >> 16) & 255, (argb >> 8) & 255, argb & 255, a); }
ImU32 with_alpha(ImU32 c, int a) { return (c & 0x00FFFFFF) | ((ImU32)a << 24); }

std::wstring today_str() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t b[32];
    swprintf(b, 32, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return b;
}

std::string sanitize_filename(std::string s) {
    for (char& c : s) if (strchr("<>:\"/\\|?*", c) || (unsigned char)c < 32) c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? std::string("program") : s;
}

bool is_machine_exporter(const Exporter* e) { return e && e->info().export_input == ExportInput::MachineJob; }
Exporter* machine_exporter(const std::string& id) {
    if (id.empty()) return nullptr;
    Exporter* e = PluginRegistry::get().exporter(id);
    return is_machine_exporter(e) ? e : nullptr;
}
// R18d: no machine type = no extension (was a silent ".txt" placeholder, shown in the old Production sides block)
std::string exporter_ext(const Exporter* e) {
    if (!e) return "";
    if (!e->info().default_ext.empty()) return e->info().default_ext;
    if (!e->info().extensions.empty()) return e->info().extensions[0];
    return "txt";
}
std::string default_program_file(const Project& p, int point, const Exporter* e) {
    std::string base = job_base_name(p);   // R18d: Project Name, Board ID, legacy Assembly No.
    std::string pn = point >= 0 && point < (int)p.model.points.size() ? p.model.points[point].name : std::string();
    const std::string ext = exporter_ext(e);
    return sanitize_filename(base + (pn.empty() ? std::string() : " " + pn)) + (ext.empty() ? std::string() : "." + ext);
}
std::wstring join_path(const std::wstring& dir, const std::wstring& file) {
    if (dir.empty()) return file;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + file;
    return dir + L"\\" + file;
}
bool file_exists(const std::wstring& p) { return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool dir_exists(const std::wstring& p) {
    if (p.empty()) return false;
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring parent_dir(std::wstring p) {
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s + (s == 2 && p[1] == L':' ? 1 : 0));
}
// the folder's parent is on this PC (so creating it is a sensible offer)
bool parent_exists(const std::wstring& p) { std::wstring q = parent_dir(p); return !q.empty() && dir_exists(q); }
bool ensure_dirs(const std::wstring& p, std::string& err) {
    if (dir_exists(p)) return true;
    int r = SHCreateDirectoryExW(nullptr, p.c_str(), nullptr);
    if (r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || dir_exists(p)) return true;
    err = "Cannot create the folder " + U(p) + (r == ERROR_PATH_NOT_FOUND || r == ERROR_BAD_PATHNAME ? " (the drive or path does not exist on this PC)."
                                                : r == ERROR_ACCESS_DENIED ? " (access denied)." : ".");
    return false;
}
// last export folder -> project folder -> Documents
std::wstring fallback_export_dir(const Doc& d) {
    std::wstring s = app::settings().dir_programs;
    if (dir_exists(s)) return s;
    std::wstring pd = W(file_dir(d.prj.path));
    if (dir_exists(pd)) return pd;
    wchar_t b[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, b))) return b;
    return {};
}

std::map<std::pair<const Doc*, std::string>, std::wstring>& last_programs() {
    static std::map<std::pair<const Doc*, std::string>, std::wstring> m;
    return m;
}

// R10 §17: explicit output status per (doc, process point). "Written" = the file was saved and read back
// byte-identical; it is NOT proof of a valid machine program (that needs the machine vendor's software / a dry run).
struct ExportStatus { std::wstring path; long long bytes = 0; bool readback = false; int placements = 0, fids = 0; std::string time, hash, format; };
std::map<std::pair<const Doc*, std::string>, ExportStatus>& export_status() {
    static std::map<std::pair<const Doc*, std::string>, ExportStatus> m;
    return m;
}
std::string iso_now() {
    SYSTEMTIME t; GetLocalTime(&t);
    char b[32]; std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}
std::string fnv64_hex(const void* data, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= ((const uint8_t*)data)[i]; h *= 1099511628211ull; }
    char b[24]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return b;
}
} // namespace (temporarily closed for the shared history helper)
} // namespace pcbp::gui
namespace pcbp::app {
// R17d (REVIEW_V16 §12): what a checkpoint stores. The same serialiser as autosave / Save (project_to_json),
// minus the Gerber layer content and minus the revision history itself (no nesting). Restore uses the
// undo-step field set: placements, packages, part numbers, panel images, board / panel outlines and their
// provenance, factory model (export layers + machine setups + layer notes), virtual origin, Project Info.
// Not restored: Gerber layers / import sources / board images, project notes, revision notes, history,
// selection sets, production sides.
std::string checkpoint_snapshot(Project& p) {
    std::vector<RevisionEntry> h;
    h.swap(p.history);
    std::string js = project_to_json(p, false).dump(-1);
    h.swap(p.history);
    return js;
}
bool restore_checkpoint(Doc& d, size_t index, std::string& err) {
    if (index >= d.prj.history.size()) { err = "no such revision"; return false; }
    const RevisionEntry r = d.prj.history[index];
    if (r.snapshot.empty()) { err = "this revision has no stored snapshot"; return false; }
    json::Value v;
    if (!json::Value::parse(r.snapshot, v, err)) return false;
    Project t;
    if (!project_from_json(v, t, err)) return false;
    d.checkpoint("Restore " + r.rev);          // undoable: the undo step holds exactly the fields replaced below
    Project& p = d.prj;
    p.comps = std::move(t.comps); p.packages = std::move(t.packages); p.parts = std::move(t.parts); p.images = std::move(t.images);
    p.board_frame = std::move(t.board_frame); p.panel_frame = std::move(t.panel_frame);
    p.board_outline_src = t.board_outline_src; p.panel_outline_src = t.panel_outline_src;
    p.model = std::move(t.model); p.virtual_origin = t.virtual_origin; p.info = std::move(t.info);
    for (auto& c : p.comps) if (c.id >= p.next_id) p.next_id = c.id + 1;
    add_history(d, "restore", "Restored " + r.rev + " (" + r.time + ")", r.comment, "", "");
    d.changed(ui::CH_ALL);
    return true;
}
// R10 §16 revision history entry (export checkpoint / explicit checkpoint). Not an undo step.
void add_history(Doc& d, const std::string& kind, const std::string& summary, const std::string& comment,
                 const std::string& file, const std::string& hash) {
    RevisionEntry r;
    r.time = gui::iso_now(); r.kind = kind; r.summary = summary; r.comment = comment; r.program_file = file; r.program_hash = hash;
    r.author = d.prj.info.originator;
    r.rev = "R" + std::to_string(d.prj.history.size() + 1);
    r.snapshot = checkpoint_snapshot(d.prj);
    d.prj.history.push_back(r);
    // only the newest kKeepCheckpointSnapshots entries stay restorable (bounded project file size)
    int kept = 0;
    for (auto it = d.prj.history.rbegin(); it != d.prj.history.rend(); ++it)
        if (!it->snapshot.empty() && ++kept > kKeepCheckpointSnapshots) it->snapshot.clear();
    d.prj.dirty = true;
    d.changed(ui::CH_INFO);
}
} // namespace pcbp::app
namespace pcbp::gui {
namespace {

Xform machine_xform(const Project& p, const MachineSetup& ms) {
    const Vec2 o = ms.has_origin ? ms.origin : p.virtual_origin;
    Xform f = ms.flipped ? Xform::scale(-1, 1) : Xform{};
    return Xform::translate(ms.hard_offset.x, ms.hard_offset.y) * Xform::rotation((double)ms.orientation) * f *
           Xform::translate(-o.x, -o.y);
}
// Root-image machine angle: same rule as export_angle() (core/machine_job, ANGLES.md §5).
double cur_mach_angle(const Component& c, const MachineSetup& ms) {
    double a = c.rot;
    if (ms.flipped && ms.mirror_angle_when_flipped) a = -a;
    return norm_deg(a + ms.orientation);
}
// legacy CircuitCAM machine angle present and different from the Board View angle
static bool legacy_angle_differs(const Component& c) {
    if (!c.has_legacy_mach_rot) return false;
    const double d = norm_deg(c.legacy_mach_rot - c.rot);
    return d > 1e-3 && d < 360.0 - 1e-3;
}

BBox images_bbox(const Project& p, bool all_images) {
    BBox bb = p.board_bbox();
    if (bb.empty()) return bb;
    BBox out;
    int n = all_images ? (int)p.images.size() : 1;
    for (int i = 0; i < std::max(n, 1); ++i) {
        Xform x = p.image_xform(i);
        out.add(x.apply({bb.x0, bb.y0})); out.add(x.apply({bb.x1, bb.y0}));
        out.add(x.apply({bb.x1, bb.y1})); out.add(x.apply({bb.x0, bb.y1}));
    }
    for (auto& poly : p.panel_frame) for (auto& v : poly) out.add(v);
    return out;
}

std::vector<std::string> split_fields(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false, any_delim = line.find_first_of(",;\t") != std::string::npos;
    for (char c : line) {
        if (c == '"') { q = !q; continue; }
        bool delim = !q && (any_delim ? (c == ',' || c == ';' || c == '\t') : (c == ' '));
        if (delim) { out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    out.push_back(cur);
    for (auto& f : out) {
        size_t a = f.find_first_not_of(" \t\r\n"), b = f.find_last_not_of(" \t\r\n");
        f = a == std::string::npos ? std::string() : f.substr(a, b - a + 1);
    }
    if (!any_delim) out.erase(std::remove(out.begin(), out.end(), std::string()), out.end());
    return out;
}
bool parse_double(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* e = nullptr;
    v = strtod(s.c_str(), &e);
    return e && e != s.c_str();
}
[[maybe_unused]] static std::map<std::string, double> parse_pairs(const std::string& text) {
    std::map<std::string, double> m;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        std::string line = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? text.size() : e + 1;
        auto f = split_fields(line);
        double v;
        if (f.size() >= 2 && !f[0].empty() && parse_double(f[1], v)) m[f[0]] = v;
    }
    return m;
}

// ImGui std::string input
int str_resize_cb(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = (std::string*)d->UserData;
        s->resize((size_t)d->BufTextLen);
        d->Buf = s->data();
    }
    return 0;
}
bool input_str(const char* id, std::string& s, const char* hint = nullptr, ImGuiInputTextFlags fl = 0) {
    fl |= ImGuiInputTextFlags_CallbackResize;
    if (hint) return ImGui::InputTextWithHint(id, hint, s.data(), s.capacity() + 1, fl, str_resize_cb, &s);
    return ImGui::InputText(id, s.data(), s.capacity() + 1, fl, str_resize_cb, &s);
}
bool input_multi(const char* id, std::string& s, ImVec2 size, ImGuiInputTextFlags fl = 0) {
    fl |= ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputTextMultiline(id, s.data(), s.capacity() + 1, size, fl, str_resize_cb, &s);
}
// a text field bound to a model value: refreshed from the model unless being edited; true = committed
bool bound_field(const char* id, std::string& buf, const std::string& model, const char* hint = nullptr) {
    ImGuiID gid = ImGui::GetID(id);
    if (ImGui::GetActiveID() != gid) buf = model;
    input_str(id, buf, hint);
    return ImGui::IsItemDeactivatedAfterEdit();
}

// Rounded segmented control. Returns the (possibly new) index.
int segmented(const char* id, const std::vector<std::string>& items, int cur, float item_w = 0, bool enabled = true) {
    const GTheme& t = th();
    ImGui::PushID(id);
    float h = ImGui::GetFrameHeight();
    float pad = S(3);
    std::vector<float> ws;
    float total = pad;
    for (auto& s : items) {
        float w = item_w > 0 ? item_w : ImGui::CalcTextSize(s.c_str()).x + S(22);
        ws.push_back(w);
        total += w;
    }
    total += pad;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + total, p.y + h), t.field, S(8));
    dl->AddRect(p, ImVec2(p.x + total, p.y + h), t.border, S(8));
    float x = p.x + pad;
    int out = cur;
    for (size_t i = 0; i < items.size(); ++i) {
        ImGui::SetCursorScreenPos(ImVec2(x, p.y));
        ImGui::PushID((int)i);
        ImGui::BeginDisabled(!enabled);
        bool cl = ImGui::InvisibleButton("seg", ImVec2(ws[i], h));
        bool hov = ImGui::IsItemHovered();
        ImGui::EndDisabled();
        ImGui::PopID();
        ImVec2 a(x, p.y + pad), b(x + ws[i], p.y + h - pad);
        if ((int)i == cur) dl->AddRectFilled(a, b, enabled ? t.accent : with_alpha(t.accent, 110), S(6));
        else if (hov && enabled) dl->AddRectFilled(a, b, t.raised, S(6));
        ImVec2 ts = ImGui::CalcTextSize(items[i].c_str());
        ImU32 tc = (int)i == cur ? t.accent_text : (enabled ? t.text : t.text_faint);
        dl->AddText(ImVec2(x + (ws[i] - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), tc, items[i].c_str());
        if (cl && enabled) out = (int)i;
        x += ws[i];
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(total, h));
    ImGui::PopID();
    return out;
}

// Toggle switch with label (rounded)
bool toggle(const char* label, bool* v, const char* tip = nullptr) {
    const GTheme& t = th();
    ImGui::PushID(label);
    float h = ImGui::GetFrameHeight() * 0.72f, w = h * 1.85f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    float rowh = ImGui::GetFrameHeight();
    bool cl = ImGui::InvisibleButton("tg", ImVec2(w + S(10) + ts.x, rowh));
    bool hov = ImGui::IsItemHovered();
    if (cl) *v = !*v;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = p.y + (rowh - h) * 0.5f;
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), *v ? (hov ? t.accent_hover : t.accent) : (hov ? t.border : t.raised), h * 0.5f);
    float r = h * 0.5f - S(2.5f);
    float cx = *v ? p.x + w - h * 0.5f : p.x + h * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, y + h * 0.5f), r, *v ? t.accent_text : t.text_dim);
    dl->AddText(ImVec2(p.x + w + S(10), p.y + (rowh - ts.y) * 0.5f), t.text, label, ImGui::FindRenderedTextEnd(label));
    if (tip) tooltip(tip);
    ImGui::PopID();
    return cl;
}

// ---------------------------------------------------------------------------
// Process point strip (header of every output view)
// ---------------------------------------------------------------------------
float strip_height() { return S(50); }

// Draws the strip; `right_w` is reserved at the right for `right()` content.
void point_strip(Doc& d, const char* caption, bool machine_marks, float right_w, const std::function<void()>& right) {
    const GTheme& t = th();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float W0 = ImGui::GetContentRegionAvail().x, H = strip_height();
    dl->AddRectFilled(p0, ImVec2(p0.x + W0, p0.y + H), t.panel);
    dl->AddLine(ImVec2(p0.x, p0.y + H - 1), ImVec2(p0.x + W0, p0.y + H - 1), t.border);
    auto& pts = d.prj.model.points;
    const int cur = current_point(d);
    std::vector<int> counts(pts.size(), 0);
    for (auto& c : d.prj.comps)
        if (c.feature == Feature::Component && c.assigned_point >= 0 && c.assigned_point < (int)counts.size()) counts[c.assigned_point]++;
    // caption
    float x = p0.x + S(14);
    ImGui::PushFont(g_font_bold);
    ImVec2 cs = ImGui::CalcTextSize(caption);
    dl->AddText(ImVec2(x, p0.y + (H - cs.y) * 0.5f), t.text_dim, caption);
    ImGui::PopFont();
    x += cs.x + S(14);
    const float chip_h = S(32), cy = p0.y + (H - chip_h) * 0.5f;
    const float avail = p0.x + W0 - right_w - S(14) - x;
    // measure chips
    std::vector<float> cw(pts.size());
    float total = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        std::string cnt = std::to_string(counts[i]);
        ImGui::PushFont(g_font_bold);
        float nw = ImGui::CalcTextSize(pts[i].name.c_str()).x;
        ImGui::PopFont();
        cw[i] = S(28) + nw + S(10) + ImGui::CalcTextSize(cnt.c_str()).x + S(14) +
                (machine_marks && pts[i].has_machine ? S(20) : 0);
        total += cw[i] + S(6);
    }
    if (pts.empty()) {
        dl->AddText(ImVec2(x, p0.y + (H - ImGui::GetTextLineHeight()) * 0.5f), t.text_faint, "This factory model has no process points.");
    } else if (total <= avail) {
        for (size_t i = 0; i < pts.size(); ++i) {
            const ModelPoint& mp = pts[i];
            ImGui::SetCursorScreenPos(ImVec2(x, cy));
            ImGui::PushID((int)i);
            bool cl = ImGui::InvisibleButton("chip", ImVec2(cw[i], chip_h));
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool sel = (int)i == cur;
            const ImU32 pc = im_col(mp.color);
            ImVec2 a(x, cy), b(x + cw[i], cy + chip_h);
            if (sel) {
                dl->AddRectFilled(a, b, im_col(mp.color, t.dark ? 70 : 45), chip_h * 0.5f);
                dl->AddRect(a, b, pc, chip_h * 0.5f, 0, S(1.6f));
            } else {
                dl->AddRectFilled(a, b, hov ? t.raised : t.panel2, chip_h * 0.5f);
                dl->AddRect(a, b, t.border, chip_h * 0.5f);
            }
            float tx = x + S(12);
            dl->AddCircleFilled(ImVec2(tx + S(4), cy + chip_h * 0.5f), S(5), pc);
            tx += S(16);
            ImFont* f = sel ? g_font_bold : g_font;
            ImGui::PushFont(f);
            ImVec2 ns = ImGui::CalcTextSize(mp.name.c_str());
            dl->AddText(ImVec2(tx, cy + (chip_h - ns.y) * 0.5f), sel ? t.text : (counts[i] ? t.text : t.text_dim), mp.name.c_str());
            tx += ns.x + S(8);
            ImGui::PopFont();
            if (machine_marks && mp.has_machine) {
                icon_draw(dl, ui::Icon::Machine, ImVec2(tx, cy + (chip_h - S(16)) * 0.5f), S(16), sel ? 1.f : 0.7f);
                tx += S(20);
            }
            std::string cnt = std::to_string(counts[i]);
            ImVec2 ks = ImGui::CalcTextSize(cnt.c_str());
            ImVec2 ba(tx, cy + S(7)), bb(tx + ks.x + S(12), cy + chip_h - S(7));
            dl->AddRectFilled(ba, bb, counts[i] ? with_alpha(pc, sel ? 230 : 140) : t.raised, S(8));
            dl->AddText(ImVec2(ba.x + S(6), (ba.y + bb.y - ks.y) * 0.5f), counts[i] ? IM_COL32(255, 255, 255, 255) : t.text_dim, cnt.c_str());
            if (hov) {
                char tip[256];
                snprintf(tip, sizeof tip, "%s  (%s)\n%d placements%s", mp.name.c_str(), mp.process_type.c_str(), counts[i],
                         mp.has_machine ? "\nMachine interface" : "");
                tooltip(tip);
            }
            if (cl) set_current_point(d, (int)i);
            x += cw[i] + S(6);
        }
    } else {
        // compact: combo
        ImGui::SetCursorScreenPos(ImVec2(x, p0.y + (H - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::SetNextItemWidth(std::min(std::max(S(160), avail), S(320)));
        std::string prev = cur >= 0 ? pts[cur].name + "  (" + std::to_string(counts[cur]) + ")" : std::string("-");
        if (ImGui::BeginCombo("##pt", prev.c_str())) {
            for (size_t i = 0; i < pts.size(); ++i) {
                ImGui::PushID((int)i);
                ImVec2 pp = ImGui::GetCursorScreenPos();
                std::string lbl = "      " + pts[i].name + "  (" + std::to_string(counts[i]) + ")";
                if (ImGui::Selectable(lbl.c_str(), (int)i == cur)) set_current_point(d, (int)i);
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(pp.x + S(8), pp.y + ImGui::GetTextLineHeight() * 0.5f), S(5), im_col(pts[i].color));
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (cur >= 0)
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(x - S(8), p0.y + H * 0.5f), S(5), im_col(pts[cur].color));
    }
    if (right) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + W0 - right_w - S(10), p0.y + (H - ImGui::GetFrameHeight()) * 0.5f));
        right();
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + H));
}

ModelPoint* point_of(Doc& d) {
    int p = current_point(d);
    return p >= 0 ? &d.prj.model.points[p] : nullptr;
}


// Render through an ID2D1DCRenderTarget in horizontal bands (keeps memory low at printer DPI)
template <class F>
void print_bands(HDC dc, const RECT& page, F&& paint) {
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
    Com<ID2D1DCRenderTarget> rt;
    if (FAILED(ui::d2d_factory()->CreateDCRenderTarget(&rp, rt.put()))) return;
    const int band = 768;
    for (int y = page.top; y < page.bottom; y += band) {
        RECT r{page.left, y, page.right, std::min<LONG>(page.bottom, y + band)};
        if (FAILED(rt->BindDC(dc, &r))) break;
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(1, 1, 1, 1));
        paint(rt.get(), D2D1::Matrix3x2F::Translation((float)-page.left, (float)-y));
        if (rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) break;
    }
}

// ===========================================================================
// Visual aid model (page millimetres, origin top-left, +Y down)
// ===========================================================================
struct RectMM {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    RectMM norm() const { return {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)}; }
    double w() const { return std::fabs(x1 - x0); }
    double h() const { return std::fabs(y1 - y0); }
    bool contains(double x, double y, double tol = 0) const {
        RectMM n = norm();
        return x >= n.x0 - tol && x <= n.x1 + tol && y >= n.y0 - tol && y <= n.y1 + tol;
    }
    D2D1_RECT_F f() const { RectMM n = norm(); return D2D1::RectF((float)n.x0, (float)n.y0, (float)n.x1, (float)n.y1); }
};

enum AnnotType { A_TEXT = 0, A_LINE, A_ARROW, A_RECT, A_CIRCLE };

struct Annot {
    int type = A_LINE;
    RectMM r;                   // line/arrow: p0 -> p1; text: (x0,y0) anchor
    std::string text;
    uint32_t color = 0xFFE03131;
    double width = 0.5;         // mm
    double size = 5;            // text height mm
};

struct VAState {
    int paper = 0;              // 0 Letter, 1 A4
    bool portrait = false;
    RectMM board, bom;
    bool show_bom = true;
    std::vector<Annot> annots;
};

void paper_size(int paper, bool portrait, double& w, double& h) {
    if (paper == 1) { w = 297.0; h = 210.0; }
    else { w = 279.4; h = 215.9; }
    if (portrait) std::swap(w, h);
}
void paper_size(const VAState& s, double& w, double& h) { paper_size(s.paper, s.portrait, w, h); }

constexpr double kMargin = 8.0, kHeaderH = 24.0;

void va_default_layout(VAState& s) {
    double pw, ph;
    paper_size(s, pw, ph);
    double top = kMargin + kHeaderH + 4;
    if (s.portrait) {
        double bh = (ph - top - kMargin) * 0.58;
        s.board = {kMargin, top, pw - kMargin, top + bh};
        s.bom = {kMargin, top + bh + 4, pw - kMargin, ph - kMargin};
    } else {
        double bw = (pw - 2 * kMargin) * 0.62;
        s.board = {kMargin, top, kMargin + bw, ph - kMargin};
        s.bom = {kMargin + bw + 4, top, pw - kMargin, ph - kMargin};
    }
}

json::Value rect_json(const RectMM& r) {
    json::Value a = json::Value::array();
    a.push(r.x0); a.push(r.y0); a.push(r.x1); a.push(r.y1);
    return a;
}
bool rect_from(const json::Value& v, RectMM& r) {
    if (!v.is_array() || v.size() < 4) return false;
    r = {v[0].num(), v[1].num(), v[2].num(), v[3].num()};
    return true;
}

std::string va_to_json(const VAState& s) {
    json::Value o = json::Value::object();
    o.set("paper", s.paper);
    o.set("portrait", s.portrait);
    o.set("board", rect_json(s.board));
    o.set("bom", rect_json(s.bom));
    o.set("show_bom", s.show_bom);
    json::Value arr = json::Value::array();
    for (auto& a : s.annots) {
        json::Value x = json::Value::object();
        x.set("t", a.type);
        x.set("p", rect_json(a.r));
        if (!a.text.empty()) x.set("text", a.text);
        x.set("color", (double)a.color);
        x.set("w", a.width);
        x.set("size", a.size);
        arr.push(x);
    }
    o.set("annots", arr);
    return o.dump();
}

void va_from_json(const std::string& text, VAState& s) {
    s = VAState{};
    va_default_layout(s);
    if (text.empty()) return;
    json::Value o;
    std::string err;
    if (!json::Value::parse(text, o, err) || !o.is_object()) return;
    s.paper = std::clamp(o["paper"].i(0), 0, 1);
    s.portrait = o["portrait"].b(false);
    va_default_layout(s);
    RectMM r;
    double pw, ph;
    paper_size(s, pw, ph);
    auto sane = [&](const RectMM& q) { RectMM n = q.norm(); return n.w() >= 5 && n.h() >= 5 && n.x0 > -pw && n.x1 < 2 * pw && n.y0 > -ph && n.y1 < 2 * ph; };
    if (rect_from(o["board"], r) && sane(r)) s.board = r.norm();
    if (rect_from(o["bom"], r) && sane(r)) s.bom = r.norm();
    s.show_bom = o["show_bom"].b(true);
    for (auto& x : o["annots"].items()) {
        Annot a;
        a.type = std::clamp(x["t"].i(A_LINE), 0, 4);
        rect_from(x["p"], a.r);
        a.text = x["text"].str();
        a.color = (uint32_t)x["color"].num(0xFFE03131);
        a.width = std::clamp(x["w"].num(0.5), 0.05, 10.0);
        a.size = std::clamp(x["size"].num(5), 1.0, 60.0);
        s.annots.push_back(a);
    }
}

std::string va_json_of(const ModelPoint* m) {
    if (!m) return {};
    auto it = m->machine.options.find(kVaKey);
    return it == m->machine.options.end() ? std::string() : it->second;
}

// ---------------------------------------------------------------------------
// page rendering (screen + printer)
// ---------------------------------------------------------------------------
struct PageCtx {
    ID2D1RenderTarget* rt = nullptr;
    D2D1_MATRIX_3X2_F m{};       // page mm -> target DIPs
    float px_per_mm = 3;
    float dip_per_px = 1;
    bool printing = false;
    bool one_to_one = false;
    ID2D1SolidColorBrush* br = nullptr;
    float hair() const { return std::max(0.2f, dip_per_px / std::max(px_per_mm, 0.01f)); }
    ID2D1SolidColorBrush* col(uint32_t c, float a = 1.f) const { br->SetColor(ui::d2c(c, a)); return br; }
};

IDWriteTextFormat* wrap_format(float size) {
    static std::map<int, Com<IDWriteTextFormat>> cache;
    int key = (int)(size * 100);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.get();
    Com<IDWriteTextFormat> f;
    ui::dwrite_factory()->CreateTextFormat(ui::resolve_face(L"Segoe UI"), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", f.put());
    if (f) {
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    IDWriteTextFormat* raw = f.get();
    cache[key] = f;
    return raw;
}

void page_text(const PageCtx& c, const std::wstring& s, double x0, double y0, double x1, double y1, float size,
               bool bold = false, int align = 0, uint32_t color = 0xFF202020) {
    if (s.empty()) return;
    // Render at real device size: tiny (mm-sized) fonts under a scaling transform get hinted/snapped per glyph
    // at the pre-transform size, which breaks letter spacing (seen under Wine). Map the rect, scale the font.
    const D2D1_MATRIX_3X2_F& m = c.m;
    const float k = std::sqrt(m._11 * m._11 + m._12 * m._12);
    c.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    D2D1_RECT_F r = D2D1::RectF((float)x0 * m._11 + m._31, (float)y0 * m._22 + m._32, (float)x1 * m._11 + m._31, (float)y1 * m._22 + m._32);
    ui::draw_text(c.rt, s, r, ui::shared_text_format(size * k, bold, align), c.col(color));
    c.rt->SetTransform(c.m);
}
float page_scale(const PageCtx& c) { return std::sqrt(c.m._11 * c.m._11 + c.m._12 * c.m._12); }

struct BomRow {
    std::string number, description, package;
    std::vector<std::string> refs;
};

std::vector<BomRow> point_bom(const Project& p, int point, int* placements) {
    std::map<std::string, BomRow> m;
    int n = 0;
    for (const Component& c : p.comps) {
        if (c.feature != Feature::Component || c.assigned_point != point) continue;
        std::string key = c.number.empty() ? "(" + (c.package.empty() ? std::string("no number") : c.package) + ")" : c.number;
        BomRow& r = m[key];
        if (r.number.empty()) {
            r.number = key;
            const PartNumber* pn = p.part_of(c);
            r.description = pn ? pn->description : std::string();
            r.package = c.package;
        }
        r.refs.push_back(c.ref.empty() ? std::string("?") : c.ref);
        ++n;
    }
    std::vector<BomRow> out;
    for (auto& kv : m) {
        std::sort(kv.second.refs.begin(), kv.second.refs.end(), ref_less);
        out.push_back(std::move(kv.second));
    }
    if (placements) *placements = n;
    return out;
}

void draw_header(const PageCtx& c, const Doc& d, int point, double pw) {
    const Project& p = d.prj;
    const ModelPoint* mp = point >= 0 && point < (int)p.model.points.size() ? &p.model.points[point] : nullptr;
    ID2D1RenderTarget* rt = c.rt;
    rt->SetTransform(c.m);
    const double x0 = kMargin, y0 = kMargin, x1 = pw - kMargin, y1 = kMargin + kHeaderH;
    const uint32_t pc = app::point_color(d, point);
    rt->FillRectangle(D2D1::RectF((float)x0, (float)y0, (float)(x0 + 3), (float)y1), c.col(pc));
    rt->DrawRectangle(D2D1::RectF((float)x0, (float)y0, (float)x1, (float)y1), c.col(0xFF404040), c.hair() * 1.5f);
    const double cw = (x1 - x0 - 3) / 3.0;
    const double ca = x0 + 3, cb = ca + cw, cc = cb + cw;
    rt->DrawLine(D2D1::Point2F((float)cb, (float)y0), D2D1::Point2F((float)cb, (float)y1), c.col(0xFF909090), c.hair());
    rt->DrawLine(D2D1::Point2F((float)cc, (float)y0), D2D1::Point2F((float)cc, (float)y1), c.col(0xFF909090), c.hair());
    const double lh = 5.2, pad = 2.0;
    const app::Settings& s = app::settings();
    std::wstring company = !s.company.empty() ? s.company : W(p.info.originator);
    page_text(c, company.empty() ? L"PCB Plotter" : company, ca + pad, y0 + 1.5, cb - pad, y0 + 1.5 + lh + 1, 4.2f, true);
    std::wstring addr = s.company_addr;
    for (auto& ch : addr) if (ch == L'\r' || ch == L'\n') ch = L' ';
    page_text(c, addr, ca + pad, y0 + 1.5 + lh + 1, cb - pad, y0 + 1.5 + 2 * lh + 1, 2.8f, false, 0, 0xFF505050);
    page_text(c, L"Date: " + today_str(), ca + pad, y1 - lh - 1, cb - pad, y1 - 1, 2.8f, false, 0, 0xFF505050);
    auto line2 = [&](int i, const std::wstring& lbl, const std::string& v) {
        page_text(c, lbl, cb + pad, y0 + 1.5 + i * lh, cb + pad + 20, y0 + 1.5 + (i + 1) * lh, 2.8f, false, 0, 0xFF606060);
        page_text(c, W(v), cb + pad + 20, y0 + 1.5 + i * lh, cc - pad, y0 + 1.5 + (i + 1) * lh, 3.1f, true);
    };
    // R17d (REVIEW_V16 §12): the visible Project Info fields only
    line2(0, L"Project", p.info.name.empty() ? std::string("-") : p.info.name);
    line2(1, L"Board ID", p.info.pcb_id.empty() ? std::string("-") : p.info.pcb_id);
    line2(2, L"BOM rev.", p.info.bom_rev.empty() ? std::string("-") : p.info.bom_rev);
    line2(3, L"Customer", p.info.customer.empty() ? std::string("-") : p.info.customer);
    page_text(c, L"Process Point", cc + pad, y0 + 1.5, x1 - pad, y0 + 1.5 + lh, 2.8f, false, 0, 0xFF606060);
    page_text(c, mp ? W(mp->name) : L"-", cc + pad, y0 + 1.5 + lh, x1 - pad, y0 + 1.5 + 2 * lh + 1, 4.6f, true, 0, pc);
    if (mp && !mp->visual_aid_title.empty())
        page_text(c, W(mp->visual_aid_title), cc + pad, y0 + 2.5 + 2 * lh, x1 - pad, y0 + 2.5 + 3 * lh, 3.0f);
    std::wstring side = mp ? (mp->side == Side::Bottom ? L"Bottom side" : L"Top side") : L"";
    page_text(c, side, cc + pad, y1 - lh - 1, x1 - pad, y1 - 1, 2.8f, false, 0, 0xFF505050);
    page_text(c, L"Page 1 of 1", cc + pad, y1 - lh - 1, x1 - pad, y1 - 1, 2.8f, false, 2, 0xFF505050);
}

void draw_board_in(const PageCtx& c, const Doc& d, int point, const RectMM& r0) {
    const Project& p = d.prj;
    RectMM r = r0.norm();
    ID2D1RenderTarget* rt = c.rt;
    BBox bb = images_bbox(p, d.all_images);
    if (bb.empty() || bb.w() <= 0 || bb.h() <= 0) {
        page_text(c, L"(no board data)", r.x0, r.y0 + r.h() * 0.45, r.x1, r.y1, 4.f, false, 1, 0xFF808080);
        return;
    }
    const ModelPoint* mp = point >= 0 && point < (int)p.model.points.size() ? &p.model.points[point] : nullptr;
    const bool mirror = mp && mp->side == Side::Bottom;
    double kx, ky;
    if (c.one_to_one) {
        kx = app::settings().print_scale_x > 0 ? app::settings().print_scale_x : 1.0;
        ky = app::settings().print_scale_y > 0 ? app::settings().print_scale_y : 1.0;
    } else {
        double k = std::min(r.w() / bb.w(), r.h() / bb.h()) * 0.96;
        kx = ky = k;
    }
    const double sx = mirror ? -kx : kx;
    const Vec2 bc = bb.center();
    const double cx = (r.x0 + r.x1) * 0.5, cy = (r.y0 + r.y1) * 0.5;
    D2D1_MATRIX_3X2_F bm = D2D1::Matrix3x2F((float)sx, 0, 0, (float)-ky, (float)(cx - sx * bc.x), (float)(cy + ky * bc.y));
    app::BoardRenderOpts o;
    o.world = bm * c.m;
    o.px_per_mm = kx * c.px_per_mm;
    o.dip_per_px = c.dip_per_px;
    o.mirror = mirror;
    o.focus_point = point;
    o.only_focus = false;
    o.show_labels = d.show_labels;
    o.show_frame = true;
    o.all_images = d.all_images;
    o.show_origin = false;
    o.show_selection = false;
    o.print_mode = true;
    rt->SetTransform(c.m);
    rt->PushAxisAlignedClip(r.f(), D2D1_ANTIALIAS_MODE_ALIASED);
    app::render_board(rt, d, o);
    rt->SetTransform(c.m);
    rt->PopAxisAlignedClip();
    if (mirror) page_text(c, L"Viewed from bottom", r.x0 + 1, r.y1 - 5, r.x1 - 1, r.y1 - 1, 2.6f, false, 2, 0xFF707070);
}

void draw_bom_block(const PageCtx& c, const Doc& d, int point, const RectMM& r0) {
    const Project& p = d.prj;
    RectMM r = r0.norm();
    ID2D1RenderTarget* rt = c.rt;
    rt->SetTransform(c.m);
    const uint32_t pc = app::point_color(d, point);
    int placements = 0;
    std::vector<BomRow> rows = point_bom(p, point, &placements);
    rt->PushAxisAlignedClip(r.f(), D2D1_ANTIALIAS_MODE_ALIASED);
    rt->FillRectangle(r.f(), c.col(0xFFFFFFFF));
    const double th_ = 6.5;
    rt->FillRectangle(D2D1::RectF((float)r.x0, (float)r.y0, (float)r.x1, (float)(r.y0 + th_)), c.col(pc));
    std::wstring name = point >= 0 && point < (int)p.model.points.size() ? W(p.model.points[point].name) : L"";
    page_text(c, name + L" \x2014 " + std::to_wstring(rows.size()) + L" part numbers, " + std::to_wstring(placements) + L" placements",
              r.x0 + 2, r.y0, r.x1 - 2, r.y0 + th_, 3.2f, true, 0, 0xFFFFFFFF);
    const double W0 = r.w();
    const double cs = r.x0 + 1, cq = cs + 4, cp = cq + std::max(8.0, W0 * 0.09), cdsc = cp + W0 * 0.25, crf = cdsc + W0 * 0.28;
    const float fs = 2.6f;
    const double lh = 3.9;
    double y = r.y0 + th_ + 0.5;
    page_text(c, L"Qty", cq, y, cp - 1, y + lh, fs, true, 0, 0xFF404040);
    page_text(c, L"Part Number", cp, y, cdsc - 1, y + lh, fs, true, 0, 0xFF404040);
    page_text(c, L"Description", cdsc, y, crf - 1, y + lh, fs, true, 0, 0xFF404040);
    page_text(c, L"References", crf, y, r.x1 - 1, y + lh, fs, true, 0, 0xFF404040);
    y += lh;
    rt->SetTransform(c.m);
    rt->DrawLine(D2D1::Point2F((float)r.x0, (float)y), D2D1::Point2F((float)r.x1, (float)y), c.col(0xFF606060), c.hair());
    y += 0.6;
    const float pk = std::max(page_scale(c), 1e-3f);
    IDWriteTextFormat* wf = wrap_format(fs * pk);
    const float refw = (float)std::max(5.0, r.x1 - crf - 1) * pk;
    for (size_t i = 0; i < rows.size(); ++i) {
        const BomRow& b = rows[i];
        std::wstring refs;
        for (size_t k = 0; k < b.refs.size(); ++k) { if (k) refs += L", "; refs += W(b.refs[k]); }
        float rh = (float)lh;
        Com<IDWriteTextLayout> lay;
        if (wf && SUCCEEDED(ui::dwrite_factory()->CreateTextLayout(refs.c_str(), (UINT32)refs.size(), wf, refw, 1000.f, lay.put()))) {
            DWRITE_TEXT_METRICS tm{};
            lay->GetMetrics(&tm);
            rh = std::max(rh, tm.height / pk + 0.6f);
        }
        if (y + rh > r.y1 - lh && i + 1 < rows.size()) {
            page_text(c, L"\x2026 " + std::to_wstring(rows.size() - i) + L" more part numbers", cq, y, r.x1 - 1, y + lh, fs, false, 0, 0xFF808080);
            break;
        }
        rt->SetTransform(c.m);
        if (i % 2) rt->FillRectangle(D2D1::RectF((float)r.x0, (float)y, (float)r.x1, (float)(y + rh)), c.col(0xFFF1F3F5));
        rt->FillRectangle(D2D1::RectF((float)cs, (float)(y + 0.6), (float)(cs + 2.6), (float)(y + 3.2)), c.col(pc));
        page_text(c, std::to_wstring(b.refs.size()), cq, y, cp - 1, y + lh, fs, true);
        page_text(c, W(b.number), cp, y, cdsc - 1, y + lh, fs);
        page_text(c, W(b.description.empty() ? b.package : b.description), cdsc, y, crf - 1, y + lh, fs, false, 0, 0xFF404040);
        if (lay) {
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
            rt->DrawTextLayout(D2D1::Point2F((float)crf * c.m._11 + c.m._31, (float)(y + 0.3) * c.m._22 + c.m._32), lay.get(), c.col(0xFF202020));
            rt->SetTransform(c.m);
        }
        y += rh;
    }
    if (rows.empty()) page_text(c, L"No components are assigned to this process point.", cq, y, r.x1 - 1, y + lh, fs, false, 0, 0xFF808080);
    rt->SetTransform(c.m);
    rt->PopAxisAlignedClip();
    rt->DrawRectangle(r.f(), c.col(0xFF404040), c.hair() * 1.5f);
}

D2D1_RECT_F text_annot_rect(const Annot& a) {
    std::wstring t = W(a.text.empty() ? std::string(" ") : a.text);
    IDWriteTextFormat* f = ui::shared_text_format((float)a.size, false, 0);
    D2D1_SIZE_F s = ui::measure_text(t, f);
    float h = std::max(s.height, (float)a.size * 1.3f);
    return D2D1::RectF((float)a.r.x0, (float)a.r.y0, (float)a.r.x0 + std::max(s.width, (float)a.size), (float)a.r.y0 + h);
}

void draw_annot(const PageCtx& c, const Annot& a) {
    ID2D1RenderTarget* rt = c.rt;
    rt->SetTransform(c.m);
    float w = std::max((float)a.width, c.hair());
    auto* b = c.col(a.color);
    D2D1_POINT_2F p0 = D2D1::Point2F((float)a.r.x0, (float)a.r.y0), p1 = D2D1::Point2F((float)a.r.x1, (float)a.r.y1);
    switch (a.type) {
    case A_LINE: rt->DrawLine(p0, p1, b, w); break;
    case A_ARROW: {
        double dx = a.r.x1 - a.r.x0, dy = a.r.y1 - a.r.y0, L = std::sqrt(dx * dx + dy * dy);
        if (L > 0.01) {
            double ux = dx / L, uy = dy / L, hl = std::min(L * 0.5, 2.5 + w * 3), hw = hl * 0.45;
            rt->DrawLine(p0, D2D1::Point2F((float)(a.r.x1 - ux * hl * 0.6), (float)(a.r.y1 - uy * hl * 0.6)), b, w);
            D2D1_POINT_2F q1 = D2D1::Point2F((float)(a.r.x1 - ux * hl - uy * hw), (float)(a.r.y1 - uy * hl + ux * hw));
            D2D1_POINT_2F q2 = D2D1::Point2F((float)(a.r.x1 - ux * hl + uy * hw), (float)(a.r.y1 - uy * hl - ux * hw));
            Com<ID2D1PathGeometry> g;
            ui::d2d_factory()->CreatePathGeometry(g.put());
            Com<ID2D1GeometrySink> k;
            if (g && SUCCEEDED(g->Open(k.put()))) {
                k->BeginFigure(p1, D2D1_FIGURE_BEGIN_FILLED);
                k->AddLine(q1);
                k->AddLine(q2);
                k->EndFigure(D2D1_FIGURE_END_CLOSED);
                k->Close();
                rt->FillGeometry(g.get(), b);
            }
        }
        break;
    }
    case A_RECT: rt->DrawRectangle(a.r.f(), b, w); break;
    case A_CIRCLE: {
        RectMM n = a.r.norm();
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((float)(n.x0 + n.x1) / 2, (float)(n.y0 + n.y1) / 2), (float)n.w() / 2, (float)n.h() / 2), b, w);
        break;
    }
    case A_TEXT: {
        D2D1_RECT_F tr = text_annot_rect(a);
        ui::draw_text(rt, W(a.text), D2D1::RectF(tr.left, tr.top, tr.right + 2, tr.bottom), ui::shared_text_format((float)a.size, false, 0), b);
        break;
    }
    }
}

void draw_page(const PageCtx& c, const Doc& d, int point, const VAState& s) {
    double pw, ph;
    paper_size(s, pw, ph);
    ID2D1RenderTarget* rt = c.rt;
    rt->SetTransform(c.m);
    if (!c.printing) rt->FillRectangle(D2D1::RectF(0, 0, (float)pw, (float)ph), c.col(0xFFFFFFFF));
    rt->DrawRectangle(D2D1::RectF(5, 5, (float)(pw - 5), (float)(ph - 5)), c.col(0xFF303030), std::max(0.35f, c.hair()));
    draw_header(c, d, point, pw);
    draw_board_in(c, d, point, s.board);
    if (s.show_bom) draw_bom_block(c, d, point, s.bom);
    for (auto& a : s.annots) draw_annot(c, a);
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

void print_va_page(HDC dc, const RECT& page, bool one_to_one, const Doc& d, int pt, const VAState& st) {
    const int dpx = GetDeviceCaps(dc, LOGPIXELSX), dpy = GetDeviceCaps(dc, LOGPIXELSY);
    const float ppmx = dpx / 25.4f, ppmy = dpy / 25.4f;
    const float W0 = (float)(page.right - page.left), H0 = (float)(page.bottom - page.top);
    double pw, ph;
    paper_size(st, pw, ph);
    const bool rotate = (pw > ph) != (W0 > H0);
    const double ew = rotate ? ph : pw, eh = rotate ? pw : ph;
    float kx, ky;
    if (one_to_one) { kx = ppmx; ky = ppmy; }
    else { float k = (float)std::min(W0 / (ew * ppmx), H0 / (eh * ppmy)); kx = ppmx * k; ky = ppmy * k; }
    D2D1_MATRIX_3X2_F pm = D2D1::Matrix3x2F::Translation((float)(-pw / 2), (float)(-ph / 2));
    if (rotate) pm = pm * D2D1::Matrix3x2F::Rotation(90.f);
    pm = pm * D2D1::Matrix3x2F::Scale(kx, ky) * D2D1::Matrix3x2F::Translation(page.left + W0 / 2, page.top + H0 / 2);
    print_bands(dc, page, [&](ID2D1RenderTarget* rt, const D2D1_MATRIX_3X2_F& band) {
        Com<ID2D1SolidColorBrush> br;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), br.put());
        if (!br) return;
        PageCtx c;
        c.rt = rt;
        c.m = pm * band;
        c.px_per_mm = kx;
        c.dip_per_px = std::max(1.f, dpx / 150.f);
        c.printing = true;
        c.one_to_one = one_to_one;
        c.br = br.get();
        draw_page(c, d, pt, st);
    });
}

const uint32_t kSwatches[] = {0xFFE03131, 0xFFF76707, 0xFFF59F00, 0xFF2F9E44, 0xFF1C7ED6, 0xFF7048E8, 0xFFD6336C, 0xFF202020};

// ===========================================================================
// Visual Aid view
// ===========================================================================
class VisualAidView : public View {
public:
    ViewKind kind() const override { return ViewKind::VisualAid; }
    const char* title() const override { return "Visual Aid"; }
    ui::Icon icon() const override { return ui::Icon::VisualAid; }
    bool can_print() const override { return true; }

    void draw(ImVec2 size) override;
    void draw_toolbar() override;
    bool on_command(int id) override;
    int cmd_state(int id) override;
    std::string status() const override {
        if (!doc) return {};
        int pt = current_point(*doc), n = 0;
        for (auto& c : doc->prj.comps) if (c.feature == Feature::Component && c.assigned_point == pt) ++n;
        return std::to_string(n) + " placements";
    }
    void print(HDC dc, const RECT& page, bool one_to_one) override {
        if (!doc) return;
        int pt = current_point(*doc);
        if (pt < 0) return;
        VAState st;
        va_from_json(va_json_of(&doc->prj.model.points[pt]), st);
        print_va_page(dc, page, one_to_one, *doc, pt, st);
    }

private:
    enum SelKind { SelNone, SelBoard, SelBom, SelAnnot };
    enum DragKind { DNone, DMove, DHandle, DCreate, DPan };

    void sync();
    void commit(const std::string& label);
    void save_live();               // write st_ into the model without checkpoint (live property edits)
    void begin_live(const char* label) { if (!live_cp_) { doc->checkpoint(label); live_cp_ = true; } }
    void end_live() { if (live_cp_) { live_cp_ = false; doc->changed(ui::CH_INFO); gen_ = doc->gen; } }
    void set_paper(int paper, bool portrait);
    void delete_selected();
    bool has_selection() const { return sel_kind_ == SelAnnot && sel_ >= 0 && sel_ < (int)st_.annots.size(); }
    RectMM* sel_rect();
    std::vector<ImVec2> handles() const;
    int hit_handle(ImVec2 s) const;
    int hit_annot(Vec2 p) const;
    Vec2 to_page(ImVec2 s) const { return {(s.x - ox_) / s_, (s.y - oy_) / s_}; }
    ImVec2 to_scr(double x, double y) const { return ImVec2((float)(ox_ + x * s_), (float)(oy_ + y * s_)); }
    void draw_canvas(ImVec2 size);
    void draw_props(float width);
    void render();
    void context_menu();

    VAState st_;
    std::string loaded_json_;
    int loaded_point_ = -2;
    uint64_t gen_ = 0;
    RenderTexture tex_;
    bool dirty_ = true;
    ImVec2 cpos_{}, csize_{};
    double zoom_ = 1, s_ = 1, ox_ = 0, oy_ = 0;
    ImVec2 pan_{0, 0};
    int tool_ = 0;                 // 0 select, 1 text, 2 line, 3 arrow, 4 rect, 5 circle
    SelKind sel_kind_ = SelNone;
    int sel_ = -1;
    DragKind drag_ = DNone;
    int drag_handle_ = -1;
    Vec2 down_{};
    bool moved_ = false;
    RectMM orig_{};
    Annot tmp_;
    bool focus_text_ = false, live_cp_ = false;
    std::string title_buf_;
    uint32_t next_color_ = 0xFFE03131;
    double next_width_ = 0.5;
    double next_size_ = 5;
};

void VisualAidView::sync() {
    if (!doc) return;
    int pt = current_point(*doc);
    if (pt != loaded_point_) {
        loaded_point_ = pt;
        sel_kind_ = SelNone; sel_ = -1; drag_ = DNone;
        loaded_json_ = "\x01";
    }
    if (doc->gen != gen_ && drag_ == DNone) {
        gen_ = doc->gen;
        dirty_ = true;
        std::string js = pt >= 0 ? va_json_of(&doc->prj.model.points[pt]) : std::string();
        if (js != loaded_json_ && !live_cp_) {
            loaded_json_ = js;
            va_from_json(js, st_);
            if (sel_kind_ == SelAnnot && (sel_ < 0 || sel_ >= (int)st_.annots.size())) { sel_kind_ = SelNone; sel_ = -1; }
            if (sel_kind_ == SelBom && !st_.show_bom) sel_kind_ = SelNone;
        }
    }
    if (loaded_json_ == "\x01") {
        loaded_json_ = pt >= 0 ? va_json_of(&doc->prj.model.points[pt]) : std::string();
        va_from_json(loaded_json_, st_);
        dirty_ = true;
    }
}

void VisualAidView::save_live() {
    int pt = current_point(*doc);
    if (pt < 0) return;
    loaded_json_ = va_to_json(st_);
    doc->prj.model.points[pt].machine.options[kVaKey] = loaded_json_;
    doc->prj.dirty = true;
    dirty_ = true;
}

void VisualAidView::commit(const std::string& label) {
    if (!doc || current_point(*doc) < 0) return;
    doc->checkpoint(label);
    save_live();
    doc->changed(ui::CH_INFO);
    gen_ = doc->gen;
}

void VisualAidView::delete_selected() {
    if (!has_selection()) return;
    st_.annots.erase(st_.annots.begin() + sel_);
    sel_kind_ = SelNone;
    sel_ = -1;
    commit("Delete annotation");
}

void VisualAidView::set_paper(int paper, bool portrait) {
    if (paper == st_.paper && portrait == st_.portrait) return;
    double ow, oh, nw, nh;
    paper_size(st_, ow, oh);
    const bool orient_change = portrait != st_.portrait;
    st_.paper = paper;
    st_.portrait = portrait;
    paper_size(st_, nw, nh);
    const double fx = nw / ow, fy = nh / oh;
    auto sc = [&](RectMM& q) { q.x0 *= fx; q.x1 *= fx; q.y0 *= fy; q.y1 *= fy; };
    if (orient_change) va_default_layout(st_);
    else { sc(st_.board); sc(st_.bom); }
    for (auto& an : st_.annots) { an.r.x0 *= fx; an.r.y0 *= fy; if (an.type != A_TEXT) { an.r.x1 *= fx; an.r.y1 *= fy; } }
    commit(orient_change ? "Change page orientation" : "Change paper size");
}

RectMM* VisualAidView::sel_rect() {
    switch (sel_kind_) {
    case SelBoard: return &st_.board;
    case SelBom: return &st_.bom;
    case SelAnnot: return sel_ >= 0 && sel_ < (int)st_.annots.size() ? &st_.annots[sel_].r : nullptr;
    default: return nullptr;
    }
}

std::vector<ImVec2> VisualAidView::handles() const {
    std::vector<ImVec2> h;
    const RectMM* r = nullptr;
    int type = A_RECT;
    if (sel_kind_ == SelBoard) r = &st_.board;
    else if (sel_kind_ == SelBom) r = &st_.bom;
    else if (sel_kind_ == SelAnnot && sel_ >= 0 && sel_ < (int)st_.annots.size()) { r = &st_.annots[sel_].r; type = st_.annots[sel_].type; }
    if (!r || type == A_TEXT) return h;
    if (type == A_LINE || type == A_ARROW) { h.push_back(to_scr(r->x0, r->y0)); h.push_back(to_scr(r->x1, r->y1)); return h; }
    RectMM n = r->norm();
    double xm = (n.x0 + n.x1) / 2, ym = (n.y0 + n.y1) / 2;
    return {to_scr(n.x0, n.y0), to_scr(xm, n.y0), to_scr(n.x1, n.y0), to_scr(n.x1, ym),
            to_scr(n.x1, n.y1), to_scr(xm, n.y1), to_scr(n.x0, n.y1), to_scr(n.x0, ym)};
}

int VisualAidView::hit_handle(ImVec2 s) const {
    auto h = handles();
    for (size_t i = 0; i < h.size(); ++i)
        if (std::fabs(h[i].x - s.x) <= S(6) && std::fabs(h[i].y - s.y) <= S(6)) return (int)i;
    return -1;
}

int VisualAidView::hit_annot(Vec2 p) const {
    const double tol = S(4) / s_ + 0.5;
    for (int i = (int)st_.annots.size() - 1; i >= 0; --i) {
        const Annot& a = st_.annots[i];
        switch (a.type) {
        case A_LINE: case A_ARROW: {
            Vec2 a0{a.r.x0, a.r.y0}, a1{a.r.x1, a.r.y1};
            Vec2 dd = a1 - a0;
            double L2 = dd.x * dd.x + dd.y * dd.y;
            double t = L2 > 0 ? std::clamp(((p.x - a0.x) * dd.x + (p.y - a0.y) * dd.y) / L2, 0.0, 1.0) : 0;
            if (dist(p, a0 + dd * t) <= tol + a.width) return i;
            break;
        }
        case A_TEXT: {
            D2D1_RECT_F r = text_annot_rect(a);
            if (p.x >= r.left - tol && p.x <= r.right + tol && p.y >= r.top - tol && p.y <= r.bottom + tol) return i;
            break;
        }
        case A_CIRCLE: {
            RectMM n = a.r.norm();
            double rx = n.w() / 2 + tol, ry = n.h() / 2 + tol;
            double dx = (p.x - (n.x0 + n.x1) / 2) / std::max(rx, 0.01), dy = (p.y - (n.y0 + n.y1) / 2) / std::max(ry, 0.01);
            if (dx * dx + dy * dy <= 1.0) return i;
            break;
        }
        default:
            if (a.r.contains(p.x, p.y, tol)) return i;
        }
    }
    return -1;
}

void VisualAidView::render() {
    if (csize_.x < 4 || csize_.y < 4) return;
    tex_.ensure((int)csize_.x, (int)csize_.y);
    ID2D1DeviceContext* rt = tex_.begin(1.0f);
    if (!rt) return;
    rt->Clear(ui::d2c(ui::pal().canvas_bg));
    double pw, ph;
    paper_size(st_, pw, ph);
    const float lx = (float)(ox_ - cpos_.x), ly = (float)(oy_ - cpos_.y);
    Com<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), br.put());
    if (br) {
        // soft shadow
        for (int i = 3; i >= 1; --i) {
            float o = S((float)i * 2.f);
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.07f));
            D2D1_ROUNDED_RECT rr{D2D1::RectF(lx - o * 0.3f, ly + o * 0.2f, lx + (float)(pw * s_) + o * 0.6f, ly + (float)(ph * s_) + o), o, o};
            rt->FillRoundedRectangle(rr, br.get());
        }
        if (doc) {
            PageCtx c;
            c.rt = rt;
            c.m = D2D1::Matrix3x2F::Scale((float)s_, (float)s_) * D2D1::Matrix3x2F::Translation(lx, ly);
            c.px_per_mm = (float)s_;
            c.dip_per_px = 1.f;
            c.br = br.get();
            draw_page(c, *doc, current_point(*doc), st_);
            if (drag_ == DCreate && moved_) draw_annot(c, tmp_);
        }
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    tex_.end();
    dirty_ = false;
}

void VisualAidView::context_menu() {
    if (!ImGui::BeginPopup("##vactx")) return;
    const bool annot = has_selection();
    if (annot) {
        Annot& a = st_.annots[sel_];
        ImGui::TextDisabled("%s", a.type == A_TEXT ? "Text" : a.type == A_LINE ? "Line" : a.type == A_ARROW ? "Arrow" : a.type == A_RECT ? "Rectangle" : "Circle");
        if (a.type == A_TEXT && menu_item_icon("Edit Text", ui::Icon::Text, "Enter")) focus_text_ = true;
        if (ImGui::BeginMenu("Colour")) {
            for (uint32_t c : kSwatches) {
                ImGui::PushID((int)c);
                ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##sw", a.color == c, 0, ImVec2(S(120), 0))) { a.color = c; commit("Annotation colour"); }
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + S(2), p.y + S(2)), ImVec2(p.x + S(118), p.y + ImGui::GetTextLineHeight() - S(2)), im_col(c), S(4));
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        if (menu_item_icon("Bring to Front", ui::Icon::ArrowUp)) {
            Annot x = a;
            st_.annots.erase(st_.annots.begin() + sel_);
            st_.annots.push_back(x);
            sel_ = (int)st_.annots.size() - 1;
            commit("Reorder annotation");
        }
        if (menu_item_icon("Duplicate", ui::Icon::Copy)) {
            Annot x = a;
            x.r.x0 += 4; x.r.y0 += 4; x.r.x1 += 4; x.r.y1 += 4;
            st_.annots.push_back(x);
            sel_ = (int)st_.annots.size() - 1;
            commit("Duplicate annotation");
        }
        if (menu_item_icon("Delete", ui::Icon::Delete, "Del")) delete_selected();
        ImGui::Separator();
    }
    if (menu_item_icon("Show BOM Block", ui::Icon::BomBlock, nullptr, st_.show_bom)) { st_.show_bom = !st_.show_bom; if (!st_.show_bom && sel_kind_ == SelBom) sel_kind_ = SelNone; commit("Toggle BOM block"); }
    if (menu_item_icon("Paper: Letter", ui::Icon::PrintSetup, nullptr, st_.paper == 0)) set_paper(0, st_.portrait);
    if (menu_item_icon("Paper: A4", ui::Icon::PrintSetup, nullptr, st_.paper == 1)) set_paper(1, st_.portrait);
    if (menu_item_icon("Landscape", ui::Icon::Rot90, nullptr, !st_.portrait)) set_paper(st_.paper, false);
    if (menu_item_icon("Portrait", ui::Icon::Rect, nullptr, st_.portrait)) set_paper(st_.paper, true);
    if (menu_item_icon("Reset Layout", ui::Icon::Undo)) { va_default_layout(st_); commit("Reset visual aid layout"); }
    ImGui::Separator();
    if (menu_item_icon("Fit Page", ui::Icon::ZoomAll)) { zoom_ = 1; pan_ = ImVec2(0, 0); dirty_ = true; }
    menu_item(ID_FILE_PRINT);
    menu_item(ID_FILE_PRINT_1TO1);
    ImGui::EndPopup();
}

void VisualAidView::draw_canvas(ImVec2 size) {
    const GTheme& t = th();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    if (size.x != csize_.x || size.y != csize_.y || pos.x != cpos_.x || pos.y != cpos_.y) dirty_ = true;
    cpos_ = pos;
    csize_ = size;
    double pw, ph;
    paper_size(st_, pw, ph);
    const float m = S(24);
    double fit = std::max(0.2, std::min((size.x - 2 * m) / pw, (size.y - 2 * m) / ph));
    double s = fit * zoom_;
    double ox = pos.x + (size.x - pw * s) * 0.5 + pan_.x, oy = pos.y + (size.y - ph * s) * 0.5 + pan_.y;
    if (s != s_ || ox != ox_ || oy != oy_) { s_ = s; ox_ = ox; oy_ = oy; dirty_ = true; }

    ImGui::InvisibleButton("##vapage", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 mp = io.MousePos;
    const Vec2 p = to_page(mp);
    const bool space = hov && ImGui::IsKeyDown(ImGuiKey_Space);
    // zoom (wheel) about the cursor
    if (hov && io.MouseWheel != 0) {
        double nz = std::clamp(zoom_ * std::pow(1.2, io.MouseWheel), 0.25, 12.0);
        double ns = fit * nz;
        // keep page point under cursor fixed
        double nox = mp.x - p.x * ns, noy = mp.y - p.y * ns;
        pan_.x = (float)(nox - (pos.x + (size.x - pw * ns) * 0.5));
        pan_.y = (float)(noy - (pos.y + (size.y - ph * ns) * 0.5));
        zoom_ = nz;
        dirty_ = true;
    }
    // pan: middle drag or space+left
    if (hov && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (space && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) drag_ = DPan;
    if (drag_ == DPan) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) { pan_.x += io.MouseDelta.x; pan_.y += io.MouseDelta.y; dirty_ = true; }
        } else drag_ = DNone;
    }
    // left button
    if (hov && !space && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && drag_ == DNone && doc && current_point(*doc) >= 0) {
        down_ = p;
        moved_ = false;
        if (tool_ == 0) {
            int h = hit_handle(mp);
            if (h >= 0 && sel_rect()) {
                drag_ = DHandle; drag_handle_ = h; orig_ = *sel_rect();
            } else {
                int ai = hit_annot(p);
                if (ai >= 0) { sel_kind_ = SelAnnot; sel_ = ai; }
                else if (st_.show_bom && st_.bom.contains(p.x, p.y)) { sel_kind_ = SelBom; sel_ = -1; }
                else if (st_.board.contains(p.x, p.y)) { sel_kind_ = SelBoard; sel_ = -1; }
                else { sel_kind_ = SelNone; sel_ = -1; }
                if (sel_rect()) { drag_ = DMove; orig_ = *sel_rect(); }
            }
        } else if (tool_ == 1) {
            Annot a;
            a.type = A_TEXT;
            a.r = {p.x, p.y, p.x, p.y};
            a.text = "Text";
            a.color = next_color_ == 0xFFE03131 ? 0xFF202020 : next_color_;
            a.size = next_size_;
            st_.annots.push_back(a);
            sel_kind_ = SelAnnot;
            sel_ = (int)st_.annots.size() - 1;
            tool_ = 0;
            focus_text_ = true;
            commit("Add text annotation");
        } else {
            drag_ = DCreate;
            tmp_ = Annot{};
            tmp_.type = tool_ == 2 ? A_LINE : tool_ == 3 ? A_ARROW : tool_ == 4 ? A_RECT : A_CIRCLE;
            tmp_.r = {p.x, p.y, p.x, p.y};
            tmp_.color = next_color_;
            tmp_.width = next_width_;
        }
        dirty_ = true;
    }
    if ((drag_ == DMove || drag_ == DHandle || drag_ == DCreate)) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            Vec2 dd = p - down_;
            if (moved_ || std::fabs(dd.x) * s_ >= 2 || std::fabs(dd.y) * s_ >= 2) {
                moved_ = true;
                if (drag_ == DCreate) {
                    tmp_.r.x1 = p.x; tmp_.r.y1 = p.y;
                    if (io.KeyShift && (tmp_.type == A_RECT || tmp_.type == A_CIRCLE)) {
                        double k = std::max(std::fabs(p.x - down_.x), std::fabs(p.y - down_.y));
                        tmp_.r.x1 = down_.x + (p.x >= down_.x ? k : -k);
                        tmp_.r.y1 = down_.y + (p.y >= down_.y ? k : -k);
                    }
                } else if (RectMM* r = sel_rect()) {
                    if (drag_ == DMove) {
                        *r = {orig_.x0 + dd.x, orig_.y0 + dd.y, orig_.x1 + dd.x, orig_.y1 + dd.y};
                    } else {
                        const bool line = sel_kind_ == SelAnnot && (st_.annots[sel_].type == A_LINE || st_.annots[sel_].type == A_ARROW);
                        *r = line ? orig_ : orig_.norm();
                        if (line) {
                            if (drag_handle_ == 0) { r->x0 = p.x; r->y0 = p.y; } else { r->x1 = p.x; r->y1 = p.y; }
                        } else {
                            const int h = drag_handle_;
                            if (h == 0 || h == 6 || h == 7) r->x0 = p.x;
                            if (h == 2 || h == 3 || h == 4) r->x1 = p.x;
                            if (h == 0 || h == 1 || h == 2) r->y0 = p.y;
                            if (h == 4 || h == 5 || h == 6) r->y1 = p.y;
                        }
                    }
                }
                dirty_ = true;
            }
        } else {
            DragKind dk = drag_;
            drag_ = DNone;
            if (dk == DCreate) {
                if (moved_ && (tmp_.r.w() > 1 || tmp_.r.h() > 1)) {
                    if (tmp_.type == A_RECT || tmp_.type == A_CIRCLE) tmp_.r = tmp_.r.norm();
                    st_.annots.push_back(tmp_);
                    sel_kind_ = SelAnnot;
                    sel_ = (int)st_.annots.size() - 1;
                    tool_ = 0;
                    commit("Add annotation");
                }
            } else if (moved_) {
                if (RectMM* r = sel_rect()) {
                    const bool line = sel_kind_ == SelAnnot && (st_.annots[sel_].type == A_LINE || st_.annots[sel_].type == A_ARROW);
                    if (!line && !(sel_kind_ == SelAnnot && st_.annots[sel_].type == A_TEXT)) {
                        RectMM n = r->norm();
                        if (n.w() < 5) n.x1 = n.x0 + 5;
                        if (n.h() < 5) n.y1 = n.y0 + 5;
                        *r = n;
                    }
                }
                commit(sel_kind_ == SelBoard ? "Move board image" : sel_kind_ == SelBom ? "Move BOM block" : "Edit annotation");
            }
            moved_ = false;
            dirty_ = true;
        }
    }
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        int ai = hit_annot(p);
        if (ai >= 0 && st_.annots[ai].type == A_TEXT) { sel_kind_ = SelAnnot; sel_ = ai; focus_text_ = true; }
    }
    if (hov && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        int ai = hit_annot(p);
        if (ai >= 0) { sel_kind_ = SelAnnot; sel_ = ai; }
        ImGui::OpenPopup("##vactx");
    }
    if (hov && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) delete_selected();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (drag_ != DNone) { drag_ = DNone; moved_ = false; loaded_json_ = "\x01"; }
            tool_ = 0; sel_kind_ = SelNone; sel_ = -1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) && has_selection() && st_.annots[sel_].type == A_TEXT) focus_text_ = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Home) || ImGui::IsKeyPressed(ImGuiKey_F)) { zoom_ = 1; pan_ = ImVec2(0, 0); dirty_ = true; }
    }
    if (hov) ImGui::SetMouseCursor(space || drag_ == DPan ? ImGuiMouseCursor_ResizeAll : tool_ == 1 ? ImGuiMouseCursor_TextInput :
                                   tool_ >= 2 ? ImGuiMouseCursor_Hand : (drag_ == DMove ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Arrow));

    if (dirty_) render();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (tex_.valid()) dl->AddImage((ImTextureID)tex_.imgui_id(), pos, ImVec2(pos.x + size.x, pos.y + size.y));
    dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
    // selection overlay
    auto outline = [&](const RectMM& r, ImU32 c, float w) {
        RectMM n = r.norm();
        dl->AddRect(to_scr(n.x0, n.y0), to_scr(n.x1, n.y1), c, 0, 0, w);
    };
    if (tool_ == 0) {
        ImU32 faint = with_alpha(t.accent, 70);
        if (sel_kind_ != SelBoard) outline(st_.board, faint, 1.f);
        if (st_.show_bom && sel_kind_ != SelBom) outline(st_.bom, faint, 1.f);
    }
    if (sel_kind_ == SelBoard) outline(st_.board, t.accent, S(1.5f));
    if (sel_kind_ == SelBom) outline(st_.bom, t.accent, S(1.5f));
    if (has_selection()) {
        const Annot& a = st_.annots[sel_];
        if (a.type == A_TEXT) {
            D2D1_RECT_F r = text_annot_rect(a);
            ImVec2 p0 = to_scr(r.left, r.top), p1 = to_scr(r.right, r.bottom);
            dl->AddRect(ImVec2(p0.x - S(3), p0.y - S(3)), ImVec2(p1.x + S(3), p1.y + S(3)), t.accent, S(3), 0, S(1.5f));
        } else if (a.type == A_RECT || a.type == A_CIRCLE) outline(a.r, with_alpha(t.accent, 160), 1.f);
    }
    for (auto& h : handles()) {
        dl->AddRectFilled(ImVec2(h.x - S(4), h.y - S(4)), ImVec2(h.x + S(4), h.y + S(4)), IM_COL32(255, 255, 255, 255), S(1.5f));
        dl->AddRect(ImVec2(h.x - S(4), h.y - S(4)), ImVec2(h.x + S(4), h.y + S(4)), t.accent, S(1.5f), 0, S(1.5f));
    }
    // hint pill
    {
        const char* hint = tool_ == 1 ? "Click on the page to place text" : tool_ >= 2 ? "Drag on the page to draw  (Shift = square / circle)" :
                           sel_kind_ == SelBoard ? "Board image: drag to move, drag handles to resize" :
                           sel_kind_ == SelBom ? "BOM block: drag to move, drag handles to resize" : nullptr;
        if (hint) {
            ImVec2 ts = ImGui::CalcTextSize(hint);
            ImVec2 a(pos.x + (size.x - ts.x) * 0.5f - S(12), pos.y + size.y - ts.y - S(26)), b(a.x + ts.x + S(24), a.y + ts.y + S(12));
            dl->AddRectFilled(a, b, with_alpha(t.panel, 235), S(10));
            dl->AddRect(a, b, t.border, S(10));
            dl->AddText(ImVec2(a.x + S(12), a.y + S(6)), t.text_dim, hint);
        }
        char z[32];
        snprintf(z, sizeof z, "%d%%", (int)std::lround(s_ / (96.0 / 25.4 * dpi_scale()) * 100));
        ImVec2 zs = ImGui::CalcTextSize(z);
        ImVec2 a(pos.x + size.x - zs.x - S(26), pos.y + S(10)), b(pos.x + size.x - S(10), pos.y + S(10) + zs.y + S(8));
        dl->AddRectFilled(a, b, with_alpha(t.panel, 220), S(8));
        dl->AddText(ImVec2(a.x + S(8), a.y + S(4)), t.text_dim, z);
    }
    dl->PopClipRect();
    context_menu();
}

void VisualAidView::draw_props(float width) {
    const GTheme& t = th();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
    ImGui::BeginChild("##vaprops", ImVec2(width, 0), ImGuiChildFlags_AlwaysUseWindowPadding, 0);
    ImGui::GetWindowDrawList()->AddLine(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y + ImGui::GetWindowHeight()), t.border);
    const float fw = ImGui::GetContentRegionAvail().x;
    ModelPoint* mp = point_of(*doc);

    section_header("PAGE");
    ImGui::Spacing();
    {
        int pp = segmented("paper", {"Letter", "A4"}, st_.paper, (fw - S(6)) / 2);
        if (pp != st_.paper) set_paper(pp, st_.portrait);
        int po = segmented("orient", {"Landscape", "Portrait"}, st_.portrait ? 1 : 0, (fw - S(6)) / 2);
        if (po != (st_.portrait ? 1 : 0)) set_paper(st_.paper, po == 1);
    }
    ImGui::Spacing();
    bool sb = st_.show_bom;
    if (toggle("Show BOM block", &sb)) { st_.show_bom = sb; if (!sb && sel_kind_ == SelBom) sel_kind_ = SelNone; commit("Toggle BOM block"); }
    ImGui::Spacing();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "Title");
    ImGui::SetNextItemWidth(-1);
    if (mp) {
        ImGuiID gid = ImGui::GetID("##vatitle");
        if (ImGui::GetActiveID() != gid) title_buf_ = mp->visual_aid_title;
        if (input_str("##vatitle", title_buf_, "Shown under the process point")) {
            if (!live_cp_) { doc->checkpoint("Visual aid title"); live_cp_ = true; }
            mp->visual_aid_title = title_buf_;
            doc->prj.dirty = true;
            dirty_ = true;
        }
        if (ImGui::IsItemDeactivated()) end_live();
    }
    ImGui::Spacing();
    if (button("Reset Layout", ui::Icon::Board, false, ImVec2(fw, 0))) { va_default_layout(st_); commit("Reset visual aid layout"); }

    ImGui::Dummy(ImVec2(0, S(8)));
    section_header("ANNOTATION");
    ImGui::Spacing();
    Annot* a = has_selection() ? &st_.annots[sel_] : nullptr;
    uint32_t col = a ? a->color : next_color_;
    // swatches
    {
        const float sw = (fw - S(4) * 7) / 8;
        for (int i = 0; i < 8; ++i) {
            if (i) ImGui::SameLine(0, S(4));
            ImGui::PushID(i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool cl = ImGui::InvisibleButton("sw", ImVec2(sw, sw));
            bool hv = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + sw, p.y + sw), im_col(kSwatches[i]), S(6));
            if ((col & 0xFFFFFF) == (kSwatches[i] & 0xFFFFFF)) dl->AddRect(ImVec2(p.x - S(2), p.y - S(2)), ImVec2(p.x + sw + S(2), p.y + sw + S(2)), t.text, S(7), 0, S(2));
            else if (hv) dl->AddRect(p, ImVec2(p.x + sw, p.y + sw), t.text_dim, S(6), 0, S(1.5f));
            if (cl) { if (a) { a->color = kSwatches[i]; commit("Annotation colour"); } else next_color_ = kSwatches[i]; }
            ImGui::PopID();
        }
    }
    {
        float c3[3] = {((col >> 16) & 255) / 255.f, ((col >> 8) & 255) / 255.f, (col & 255) / 255.f};
        ImGui::SetNextItemWidth(fw);
        if (ImGui::ColorEdit3("##acol", c3, ImGuiColorEditFlags_DisplayHex)) {
            uint32_t v = 0xFF000000u | ((uint32_t)std::lround(c3[0] * 255) << 16) | ((uint32_t)std::lround(c3[1] * 255) << 8) | (uint32_t)std::lround(c3[2] * 255);
            if (a) { begin_live("Annotation colour"); a->color = v; save_live(); } else next_color_ = v;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() || (!ImGui::IsItemActive() && live_cp_ && !ImGui::IsAnyItemActive())) end_live();
    }
    const bool is_text = a && a->type == A_TEXT;
    if (!a || !is_text) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "Line width (mm)");
        float w = (float)(a ? a->width : next_width_);
        ImGui::SetNextItemWidth(fw);
        if (ImGui::SliderFloat("##aw", &w, 0.1f, 4.f, "%.2f mm")) {
            if (a) { begin_live("Annotation width"); a->width = w; save_live(); } else next_width_ = w;
        }
        if (ImGui::IsItemDeactivated()) end_live();
    }
    if (!a || is_text) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "Font size (mm)");
        float fs = (float)(a ? a->size : next_size_);
        ImGui::SetNextItemWidth(fw);
        if (ImGui::SliderFloat("##afs", &fs, 1.f, 30.f, "%.1f mm")) {
            if (a) { begin_live("Annotation font size"); a->size = fs; save_live(); } else next_size_ = fs;
        }
        if (ImGui::IsItemDeactivated()) end_live();
    }
    if (is_text) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "Text");
        if (focus_text_) { ImGui::SetKeyboardFocusHere(); focus_text_ = false; }
        if (input_multi("##atext", a->text, ImVec2(fw, ImGui::GetTextLineHeight() * 4 + S(10)))) {
            begin_live("Edit text annotation");
            save_live();
        }
        if (ImGui::IsItemDeactivated()) {
            if (a->text.empty()) a->text = "Text", save_live();
            end_live();
        }
    }
    if (a) {
        ImGui::Spacing();
        float bw = (fw - S(6)) / 2;
        if (button("To Front", ui::Icon::None, false, ImVec2(bw, 0))) {
            Annot x = *a;
            st_.annots.erase(st_.annots.begin() + sel_);
            st_.annots.push_back(x);
            sel_ = (int)st_.annots.size() - 1;
            commit("Reorder annotation");
        } else {
            ImGui::SameLine(0, S(6));
            if (button("Delete", ui::Icon::Delete, false, ImVec2(bw, 0))) delete_selected();
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_faint);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fw);
        ImGui::TextUnformatted(st_.annots.empty() ? "Pick a drawing tool in the toolbar to add text, lines, arrows, rectangles or circles."
                                                  : "Select an annotation on the page to edit it. New annotations use these settings.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, S(8)));
    section_header("VIEW & PRINT");
    ImGui::Spacing();
    {
        float bw = (fw - S(12)) / 3;
        if (button("Fit", ui::Icon::ZoomAll, false, ImVec2(bw, 0))) { zoom_ = 1; pan_ = ImVec2(0, 0); dirty_ = true; }
        ImGui::SameLine(0, S(6));
        if (button("100%", ui::Icon::Zoom100, false, ImVec2(bw, 0))) {
            double pw, ph; paper_size(st_, pw, ph);
            double fit = std::max(0.2, std::min((csize_.x - 2 * S(24)) / pw, (csize_.y - 2 * S(24)) / ph));
            zoom_ = (96.0 / 25.4 * dpi_scale()) / fit; pan_ = ImVec2(0, 0); dirty_ = true;
        }
        ImGui::SameLine(0, S(6));
        if (button("200%", ui::Icon::ZoomIn, false, ImVec2(bw, 0))) {
            double pw, ph; paper_size(st_, pw, ph);
            double fit = std::max(0.2, std::min((csize_.x - 2 * S(24)) / pw, (csize_.y - 2 * S(24)) / ph));
            zoom_ = (2 * 96.0 / 25.4 * dpi_scale()) / fit; pan_ = ImVec2(0, 0); dirty_ = true;
        }
    }
    if (button("Print Visual Aid", ui::Icon::Print, true, ImVec2(fw, 0))) post_command(ID_FILE_PRINT);
    if (button("Print 1:1 (board actual size)", ui::Icon::Print, false, ImVec2(fw, 0))) post_command(ID_FILE_PRINT_1TO1);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void VisualAidView::draw(ImVec2 size) {
    if (!doc) return;
    sync();
    ImVec2 start = ImGui::GetCursorScreenPos();
    point_strip(*doc, "VISUAL AID", false, 0, nullptr);
    float h = size.y - strip_height();
    if (current_point(*doc) < 0) { if (gui::empty_state(ui::Icon::VisualAid, "No process point", "Add process points to the factory model first.", "Factory Model...", ui::Icon::FactoryModel)) post_command(app::ID_LIB_FACTORY_MODEL); return; }
    const float pw = std::min(S(290), size.x * 0.4f);
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + strip_height()));
    draw_canvas(ImVec2(size.x - pw, h));
    ImGui::SetCursorScreenPos(ImVec2(start.x + size.x - pw, start.y + strip_height()));
    ImGui::BeginGroup();
    ImGui::PushID("props");
    ImGui::SetNextWindowSize(ImVec2(pw, h));
    draw_props(pw);
    ImGui::PopID();
    ImGui::EndGroup();
}

void VisualAidView::draw_toolbar() {
    struct T { ui::Icon ic; const char* tip; };
    static const T tools[] = {{ui::Icon::Select, "Select / move / resize"}, {ui::Icon::Text, "Text"}, {ui::Icon::Line, "Line"},
                              {ui::Icon::Arrow, "Arrow"}, {ui::Icon::Rect, "Rectangle"}, {ui::Icon::Circle, "Circle"}};
    for (int i = 0; i < 6; ++i) {
        if (i) ImGui::SameLine(0, S(2));
        ImGui::PushID(i);
        if (icon_button("vat", tools[i].ic, tools[i].tip, tool_ == i)) { tool_ = i; if (i) { sel_kind_ = SelNone; sel_ = -1; } }
        ImGui::PopID();
    }
    ImGui::SameLine(0, S(10));
    if (icon_button("vabom", ui::Icon::BomBlock, "Show / hide BOM block", st_.show_bom)) {
        st_.show_bom = !st_.show_bom; if (!st_.show_bom && sel_kind_ == SelBom) sel_kind_ = SelNone; commit("Toggle BOM block");
    }
    ImGui::SameLine(0, S(2));
    if (icon_button("vareset", ui::Icon::Board, "Reset layout")) { va_default_layout(st_); commit("Reset visual aid layout"); }
    ImGui::SameLine(0, S(2));
    if (icon_button("vadel", ui::Icon::Delete, "Delete annotation (Del)", false, has_selection())) delete_selected();
    ImGui::SameLine(0, S(2));
    if (icon_button("vafit", ui::Icon::ZoomAll, "Fit page (Home)")) { zoom_ = 1; pan_ = ImVec2(0, 0); dirty_ = true; }
}

bool VisualAidView::on_command(int id) {
    switch (id) {
    case ID_VIEW_ZOOM_IN: zoom_ = std::min(12.0, zoom_ * 1.25); dirty_ = true; return true;
    case ID_VIEW_ZOOM_OUT: zoom_ = std::max(0.25, zoom_ / 1.25); dirty_ = true; return true;
    case ID_VIEW_ZOOM_ALL: case ID_VIEW_ZOOM_RECT: zoom_ = 1; pan_ = ImVec2(0, 0); dirty_ = true; return true;
    case ID_VIEW_ZOOM_100: {
        double pw, ph; paper_size(st_, pw, ph);
        double fit = std::max(0.2, std::min((csize_.x - 2 * S(24)) / pw, (csize_.y - 2 * S(24)) / ph));
        zoom_ = (96.0 / 25.4 * dpi_scale()) / fit; pan_ = ImVec2(0, 0); dirty_ = true;
        return true;
    }
    case ID_EDIT_DELETE: if (has_selection()) { delete_selected(); return true; } return false;
    }
    return false;
}
int VisualAidView::cmd_state(int id) {
    switch (id) {
    case ID_VIEW_ZOOM_IN: case ID_VIEW_ZOOM_OUT: case ID_VIEW_ZOOM_ALL: case ID_VIEW_ZOOM_100: return 1;
    case ID_VIEW_ZOOM_RECT: return 0;
    case ID_EDIT_DELETE: return has_selection() ? 1 : 0;
    case ID_FILE_PRINT: case ID_FILE_PRINT_1TO1: case ID_FILE_PRINT_PREVIEW: return 1;
    }
    return -1;
}

// ===========================================================================
// Machine view (Programming + Machine View sub-tabs)
// ===========================================================================
class MachineView;

class MachCanvas : public GCanvas {
public:
    MachineView* v = nullptr;
    bool origin_mode = false;
    int ctx_comp = -1;
    Xform mx() const;
    int hit(Vec2 w) const;
    BBox world_extent() const override;
protected:
    void paint_world(ID2D1DeviceContext* rt) override;
    void paint_overlay(ImDrawList* dl) override;
    void on_click(Vec2 w, int mods) override;
    void on_context(Vec2 w) override;
    bool wants_rubber_band() const override { return false; }
    bool on_key(ImGuiKey k, int mods) override;
    ImGuiMouseCursor cursor() const override { return origin_mode ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_Arrow; }
};

class MachineView : public View {
public:
    MachineView() { cv_.v = this; }
    ViewKind kind() const override { return ViewKind::Machine; }
    const char* title() const override { return "Export"; }
    ui::Icon icon() const override { return ui::Icon::MachineProg; }
    bool can_print() const override { return sub_ == 1; }
    void draw(ImVec2 size) override;
    void draw_toolbar() override;
    bool on_command(int id) override;
    int cmd_state(int id) override;
    std::string status() const override;
    std::string status_right() const override {
        if (sub_ != 1 || !cv_.hovered()) return {};
        Vec2 w = cv_.mouse_world();
        return "Machine X " + fmt_len_u8(w.x, false) + "   Y " + fmt_len_u8(w.y, false) + " " + U(app::units_label());
    }
    void print(HDC dc, const RECT& page, bool one_to_one) override;

    int point() const { return doc ? current_point(*doc) : -1; }
    ModelPoint* mp() const { int p = point(); return p >= 0 ? &doc->prj.model.points[p] : nullptr; }

    // actions (called from deferred contexts)
    void run_generate();
    void machine_options();
    void ctx_action(int action, int ci);
    void draw_angle_preview();
    bool angle_preview_open_ = false;
    std::string trace_text_;

    int sub_ = 0;                   // 0 = export page, 1 = machine-origin picking canvas (temporary)
    int validated_point_ = -2;      // R17d: layer whose validation list is shown (after Validate / Generate)
    // R12 §17: validation panel state + "Back to Export validation" breadcrumb
    bool val_open_ = true;
    std::string val_ref_;
    MachCanvas cv_;
    std::vector<bool> angle_err_;

private:
    void draw_programming(ImVec2 size);
    void draw_form(float width);
    void draw_machine_view(ImVec2 size);
    void draw_origin_pick(ImVec2 size);
    void draw_layer_list(float width);
    void draw_orient_preview(ImVec2 size);
    void draw_readiness(float width);
    void draw_sides(float width);
    void draw_validation(float width);
    void fly_to_ref(const std::string& ref);
    void refresh_readiness();
    void ensure_job();
    void apply(const char* label, const std::function<void(MachineSetup&)>& fn) {
        ModelPoint* m = mp();
        if (!m) return;
        doc->checkpoint(label);
        fn(m->machine);
        doc->changed(ui::CH_MODEL);
    }

    uint64_t cv_gen_ = 0;
    int cv_point_ = -2;
    long long mx_sig_ = -1;
    std::string dir_buf_, file_buf_, offx_buf_, offy_buf_;
    MachineJob job_;
    std::string job_err_;
    uint64_t job_gen_ = 0;
    int job_point_ = -2;
    std::string job_buf_;
    bool adv_open_ = false;
    flexdb::Readiness rd_;
    bool rd_ok_ = false;
    uint64_t rd_gen_ = 0;
    int rd_point_ = -2, rd_frame_ = -1000;
    void draw_destination(float fw);
    struct LastGen { int point = -1; bool ok = false; std::wstring path; int placements = 0, parts = 0, circuits = 0, fids = 0, warnings = 0; long long bytes = 0; };
    LastGen last_;
};

// --- canvas ---------------------------------------------------------------
Xform MachCanvas::mx() const {
    const ModelPoint* m = v->mp();
    if (!m) return Xform{};
    return machine_xform(v->doc->prj, m->machine);
}

BBox MachCanvas::world_extent() const {
    BBox out;
    if (!v->doc) return BBox(0, 0, 100, 80);
    BBox bb = images_bbox(v->doc->prj, true);
    Xform m = mx();
    if (!bb.empty()) {
        out.add(m.apply({bb.x0, bb.y0})); out.add(m.apply({bb.x1, bb.y0}));
        out.add(m.apply({bb.x1, bb.y1})); out.add(m.apply({bb.x0, bb.y1}));
    }
    out.add(Vec2{0, 0});
    if (out.w() < 1 || out.h() < 1) out.inflate(10);
    out.inflate(std::max(out.w(), out.h()) * 0.04);
    return out;
}

void MachCanvas::paint_world(ID2D1DeviceContext* rt) {
    const ModelPoint* m = v->mp();
    if (!m) return;
    const Doc& d = *v->doc;
    const MachineSetup& ms = m->machine;
    const Xform X = mx();
    app::BoardRenderOpts o;
    o.world = to_d2d(X) * world_matrix();
    o.px_per_mm = scale_;
    o.dip_per_px = 1.f;
    o.mirror = ms.flipped;
    o.focus_point = v->point();
    o.only_focus = true;
    o.show_labels = d.show_labels;
    o.show_frame = d.show_frame;
    o.all_images = true;
    o.show_origin = false;
    o.verify_angles = d.verify_angles;
    if (v->angle_err_.size() == d.prj.comps.size()) o.angle_errors = &v->angle_err_;
    app::render_board(rt, d, o);
    // machine-angle arrows (root image, components of this point)
    rt->SetTransform(world_matrix());
    Com<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(ui::d2c(0xFFFF8C1A), br.put());
    if (!br) return;
    const float lw = (float)(1.6 / scale_);
    for (size_t i = 0; i < d.prj.comps.size(); ++i) {
        const Component& c = d.prj.comps[i];
        if (c.feature != Feature::Component || c.assigned_point != v->point()) continue;
        BBox e = d.prj.comp_bbox(c);
        double L = std::clamp(std::min(e.w(), e.h()) * 0.5, 0.6, 6.0);
        if (L * scale_ < 9) continue;
        const bool err = v->angle_err_.size() == d.prj.comps.size() && v->angle_err_[i];
        br->SetColor(ui::d2c(err ? ui::pal().error_red : (legacy_angle_differs(c) ? 0xFFFF8C1A : 0xFF2CB1A0)));
        Vec2 p = X.apply(c.pos);
        double a = deg2rad(cur_mach_angle(c, ms));
        double ux = std::cos(a), uy = std::sin(a);
        Vec2 tip{p.x + ux * L, p.y + uy * L};
        double hl = std::min(L * 0.45, 12.0 / scale_), hw = hl * 0.55;
        rt->DrawLine(D2D1::Point2F((float)p.x, (float)p.y), D2D1::Point2F((float)(tip.x - ux * hl * 0.5), (float)(tip.y - uy * hl * 0.5)), br.get(), lw);
        Com<ID2D1PathGeometry> g;
        ui::d2d_factory()->CreatePathGeometry(g.put());
        Com<ID2D1GeometrySink> k;
        if (g && SUCCEEDED(g->Open(k.put()))) {
            k->BeginFigure(D2D1::Point2F((float)tip.x, (float)tip.y), D2D1_FIGURE_BEGIN_FILLED);
            k->AddLine(D2D1::Point2F((float)(tip.x - ux * hl - uy * hw), (float)(tip.y - uy * hl + ux * hw)));
            k->AddLine(D2D1::Point2F((float)(tip.x - ux * hl + uy * hw), (float)(tip.y - uy * hl - ux * hw)));
            k->EndFigure(D2D1_FIGURE_END_CLOSED);
            k->Close();
            rt->FillGeometry(g.get(), br.get());
        }
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F((float)p.x, (float)p.y), lw * 1.2f, lw * 1.2f), br.get());
    }
}

void MachCanvas::paint_overlay(ImDrawList* dl) {
    const ModelPoint* m = v->mp();
    if (!m) return;
    const GTheme& t = th();
    const MachineSetup& ms = m->machine;
    const ImU32 oc = im_col(ui::pal().origin);
    // machine origin + axes
    ImVec2 o = w2s({0, 0});
    const float L = S(54);
    ImVec2 px(o.x + L, o.y), py(o.x, o.y - L);
    auto arrow = [&](ImVec2 a, ImVec2 b, ImU32 c) {
        dl->AddLine(a, b, c, S(2));
        float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
        if (len < 1) return;
        dx /= len; dy /= len;
        dl->AddTriangleFilled(b, ImVec2(b.x - dx * S(10) - dy * S(5), b.y - dy * S(10) + dx * S(5)),
                              ImVec2(b.x - dx * S(10) + dy * S(5), b.y - dy * S(10) - dx * S(5)), c);
    };
    arrow(o, px, IM_COL32(230, 70, 70, 255));
    arrow(o, py, IM_COL32(60, 190, 90, 255));
    ImGui::PushFont(g_font_bold);
    dl->AddText(ImVec2(px.x + S(4), px.y - S(8)), IM_COL32(230, 70, 70, 255), "X");
    dl->AddText(ImVec2(py.x - S(4), py.y - S(22)), IM_COL32(60, 190, 90, 255), "Y");
    ImGui::PopFont();
    dl->AddCircle(o, S(5), oc, 0, S(2));
    // virtual / machine-specific origin
    ImVec2 vo = w2s(ms.hard_offset);
    ImU32 yc = ms.has_origin ? IM_COL32(255, 200, 40, 255) : oc;
    dl->AddCircle(vo, S(8), yc, 0, S(1.5f));
    dl->AddLine(ImVec2(vo.x - S(13), vo.y), ImVec2(vo.x + S(13), vo.y), yc, S(1.2f));
    dl->AddLine(ImVec2(vo.x, vo.y - S(13)), ImVec2(vo.x, vo.y + S(13)), yc, S(1.2f));
    if (ms.has_origin) dl->AddCircleFilled(vo, S(3), yc);
    // origin-mode hover target
    if (origin_mode && hovered()) {
        int ci = hit(mouse_world());
        if (ci >= 0) {
            const Component& c = v->doc->prj.comps[ci];
            BBox e = v->doc->prj.comp_bbox(c);
            Xform X = mx();
            ImVec2 a = w2s(X.apply({e.x0, e.y0})), b = w2s(X.apply({e.x1, e.y1}));
            dl->AddRect(ImVec2(std::min(a.x, b.x) - S(3), std::min(a.y, b.y) - S(3)), ImVec2(std::max(a.x, b.x) + S(3), std::max(a.y, b.y) + S(3)),
                        IM_COL32(255, 200, 40, 255), S(3), 0, S(2));
        }
    }
    // info pill
    Exporter* ex = machine_exporter(ms.exporter_id);
    std::string info = (ex ? ex->info().name : std::string("No machine type")) + "   \xE2\x80\xA2   " + std::to_string(ms.orientation) + "\xC2\xB0" +
                       (ms.flipped ? "   \xE2\x80\xA2   Flipped" : "") + (ms.side == Side::Bottom ? "   \xE2\x80\xA2   Bottom" : "   \xE2\x80\xA2   Top") +
                       (ms.has_origin ? "   \xE2\x80\xA2   Machine-specific origin" : "") +
                       (ms.flipped ? (ms.mirror_angle_when_flipped ? "   \xE2\x80\xA2   Angles mirrored" : "   \xE2\x80\xA2   Angles as shown") : "");
    if (origin_mode) info = "Click a component or pad to set the machine origin; click empty space to clear it (Esc cancels)";
    ImVec2 ts = ImGui::CalcTextSize(info.c_str());
    ImVec2 c0 = origin();
    ImVec2 a(c0.x + S(10), c0.y + S(10)), b(a.x + ts.x + S(24), a.y + ts.y + S(14));
    dl->AddRectFilled(a, b, with_alpha(origin_mode ? t.accent : t.panel, origin_mode ? 240 : 230), S(10));
    if (!origin_mode) dl->AddRect(a, b, t.border, S(10));
    dl->AddText(ImVec2(a.x + S(12), a.y + S(7)), origin_mode ? t.accent_text : t.text, info.c_str());
    size_t nerr = 0;
    for (bool e : v->angle_err_) nerr += e;
    if (nerr) {
        std::string s2 = std::to_string(nerr) + " component(s) flagged (red)";
        dl->AddText(ImVec2(a.x + S(4), b.y + S(8)), t.error, s2.c_str());
    }
    // legend
    {
        const char* items[] = {"Exported angle", "Differs from legacy CircuitCAM angle"};
        ImU32 cols[] = {IM_COL32(0x2C, 0xB1, 0xA0, 255), IM_COL32(0xFF, 0x8C, 0x1A, 255)};
        float y = c0.y + size().y - S(28);
        float x = c0.x + S(12);
        for (int i = 0; i < 2; ++i) {
            dl->AddLine(ImVec2(x, y + S(8)), ImVec2(x + S(16), y + S(8)), cols[i], S(2));
            dl->AddTriangleFilled(ImVec2(x + S(20), y + S(8)), ImVec2(x + S(13), y + S(4)), ImVec2(x + S(13), y + S(12)), cols[i]);
            dl->AddText(ImVec2(x + S(26), y), t.text_dim, items[i]);
            x += S(34) + ImGui::CalcTextSize(items[i]).x + S(16);
        }
    }
}

int MachCanvas::hit(Vec2 w) const {
    if (!v->doc) return -1;
    const Doc& d = *v->doc;
    Vec2 b = mx().inverse().apply(w);
    const int pt = v->point();
    int ci = app::hit_component(d, b, 3.0 / scale_, true, true);
    if (ci >= 0 && d.prj.comps[ci].assigned_point != pt) {
        ci = -1;
        double best = 1e30;
        for (size_t i = 0; i < d.prj.comps.size(); ++i) {
            const Component& c = d.prj.comps[i];
            if (c.assigned_point != pt) continue;
            BBox e = d.prj.comp_bbox(c);
            e.inflate(3.0 / scale_);
            if (e.contains(b) && e.w() * e.h() < best) { best = e.w() * e.h(); ci = (int)i; }
        }
    }
    return ci;
}

bool MachCanvas::on_key(ImGuiKey k, int) {
    if (k == ImGuiKey_Escape && origin_mode) { origin_mode = false; invalidate(); return true; }
    return false;
}

void MachCanvas::on_click(Vec2 w, int mods) {
    ModelPoint* m = v->mp();
    if (!m) return;
    Doc& d = *v->doc;
    if (origin_mode) {
        origin_mode = false;
        Vec2 b = mx().inverse().apply(w);
        int ci = app::hit_component(d, b, 3.0 / scale_, true, true);
        d.checkpoint("Machine origin");
        if (ci >= 0) {
            const Component& c = d.prj.comps[ci];
            int pad = app::hit_pad(d, c, b, 1.0 / scale_);
            m->machine.has_origin = true;
            m->machine.origin = pad >= 0 ? app::pad_world(d, c, pad) : c.pos;
            toast("Machine origin set to " + c.ref + (pad >= 0 ? " pad " + std::to_string(pad + 1) : std::string()));
        } else {
            m->machine.has_origin = false;
            toast("Machine origin cleared (using the global virtual origin)");
        }
        d.changed(ui::CH_MODEL);
        return;
    }
    int ci = hit(w);
    if (!(mods & (KM_SHIFT | KM_CTRL))) for (auto& c : d.prj.comps) c.selected = false;
    if (ci >= 0) {
        Component& c = d.prj.comps[ci];
        c.selected = (mods & KM_CTRL) ? !c.selected : true;
        Vec2 p = mx().apply(c.pos);
        char ang[32];
        snprintf(ang, sizeof ang, "%.2f", cur_mach_angle(c, m->machine));
        gui::status(c.ref + "  " + c.number + "  [" + c.package + "]   X " + fmt_len_u8(p.x, false) + "  Y " + fmt_len_u8(p.y, false) +
                    "  Machine angle " + ang + "\xC2\xB0 (board " + io::fmt_fixed(c.rot, 2) + "\xC2\xB0)" +
                    (legacy_angle_differs(c) ? "  legacy CircuitCAM " + io::fmt_fixed(c.legacy_mach_rot, 2) + "\xC2\xB0" : std::string()));
    }
    d.changed(ui::CH_SELECTION);
}

void MachCanvas::on_context(Vec2 w) {
    ctx_comp = hit(w);
    ImGui::OpenPopup("##mvctx");
}

// --- actions -------------------------------------------------------------
// Legacy Calculate/Adjust/Reverse/Import/Offset/Set-Neutralized angle commands are gone:
// the exported angle IS the Board View rotation (specs/ANGLES.md). Edit rotations with
// Group Rotate (Edit menu); this view only previews them.
enum { M_PREVIEW = 1, M_TRACE, M_ORIGIN, M_CLR_ORIGIN };

MachineView* machine_view_of(const DocPtr& d) { return dynamic_cast<MachineView*>(find_view(d, ViewKind::Machine)); }

void MachineView::ctx_action(int action, int ci) {
    ModelPoint* m = mp();
    if (!m) return;
    Doc& d = *doc;
    const int pt = point();
    HWND h = app::frame_hwnd();
    MachineSetup& ms = m->machine;
    auto& comps = d.prj.comps;
    auto in_point = [&](const Component& c) { return c.feature == Feature::Component && c.assigned_point == pt; };
    (void)h; (void)comps; (void)in_point;
    switch (action) {
    case M_PREVIEW: angle_preview_open_ = true; break;
    case M_TRACE: {
        if (ci < 0) for (size_t i = 0; i < comps.size(); ++i) if (comps[i].selected && in_point(comps[i])) { ci = (int)i; break; }
        if (ci < 0 || ci >= (int)comps.size()) { ui::message(h, L"Transform Chain", L"Right-click a component of this process point."); return; }
        PlacementTrace tr;
        std::string err;
        if (!trace_placement(d.prj, pt, (size_t)ci, 0, tr, err)) { ui::message(h, L"Transform Chain", W(err)); return; }
        trace_text_ = trace_to_text(tr);
        angle_preview_open_ = true;
        break;
    }
    case M_ORIGIN: cv_.origin_mode = !cv_.origin_mode; sub_ = 1; cv_.invalidate(); break;
    case M_CLR_ORIGIN:
        if (ms.has_origin) { d.checkpoint("Clear machine origin"); ms.has_origin = false; d.changed(ui::CH_MODEL); }
        break;
    }
}

static constexpr ImGuiID kGenDoneKey = 0x6E4D0E1u;
static constexpr ImGuiID kValKey = 0x6E4D0E2u;
void MachineView::run_generate() {
    if (!doc) return;
    const int pt = point();
    HWND h = app::frame_hwnd();
    // R21 (AUDIT_V20 P0): the review (shared preflight + one "Generate anyway?" confirmation) runs inside
    // app::generate_program for every path; this view only shows the list in its Validation area.
    validated_point_ = pt;
    flash(kValKey);
    request_redraw(3);
    // tooling check (after the review): parts the machine cannot place until they are set up in its component database
    auto tooling = [this, h]() -> bool {
    rd_gen_ = 0;
    refresh_readiness();
    if (rd_ok_ && rd_.library && rd_.open()) {
        bool fix = false;
        std::wstring names;
        for (size_t i = 0; i < rd_.attention.size(); ++i) names += (i ? L", " : L"") + W(rd_.attention[i]);
        ui::Form f(L"Generate Program", 500);
        f.header(L"Parts not set up in the machine database").icon((int)ui::Icon::Warning);
        f.label(std::to_wstring(rd_.untaught) + L" untaught and " + std::to_wstring(rd_.conflict) + L" conflicting part number(s) in this program: " + names +
                (rd_.open() > (int)rd_.attention.size() ? L", ..." : L""));
        f.label(L"The program file is still valid, but the machine cannot place these parts until they have a component profile in the machine database.");
        f.ok_text(L"Generate");
        f.extra_button(L"Fix in Components", [&fix](ui::Form& ff) { fix = true; ff.close(false); return false; });
        if (!f.run(h)) {
            if (fix) tooling_open(doc, true);
            return false;
        }
    }
    return true;
    };
    std::wstring out;
    std::string err;
    if (!app::generate_program(h, *doc, pt, &out, err, tooling)) {
        if (!err.empty() && err != kCancelled) ui::warning(h, L"Generate Program", W(err));
        return;
    }
    // summary
    MachineJob j;
    std::string e2;
    build_machine_job(doc->prj, pt, j, e2);
    last_ = LastGen{};
    last_.point = pt; last_.ok = true; last_.path = out;
    last_.placements = (int)j.placements.size(); last_.parts = (int)j.parts.size();
    last_.circuits = (int)j.circuits.size(); last_.fids = app::written_mark_count(j); last_.warnings = (int)j.warnings.size();
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(out.c_str(), GetFileExInfoStandard, &fa)) last_.bytes = ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    // R21: the written file's read-back + program checks ran inside generate_program (a failure there is a failed export)
    flash(kGenDoneKey);
    request_redraw(3);
}

void MachineView::machine_options() {
    ModelPoint* m = mp();
    if (!m) return;
    HWND h = app::frame_hwnd();
    Exporter* ex = machine_exporter(m->machine.exporter_id);
    if (!ex) { ui::message(h, L"Machine Options", L"Select a machine type first."); return; }
    if (ex->info().options.empty()) { ui::message(h, L"Machine Options", W(ex->info().name) + L" has no additional properties."); return; }
    Options o = default_options(ex->info());
    for (auto& kv : m->machine.options) if (kv.first.rfind("__", 0) != 0) o[kv.first] = kv.second;
    if (!app::dlg_plugin_options(h, ex->info(), o)) return;
    m = mp();
    if (!m) return;
    doc->checkpoint("Machine options");
    for (auto& kv : o) m->machine.options[kv.first] = kv.second;
    doc->changed(ui::CH_MODEL);
}

std::string MachineView::status() const {
    const ModelPoint* m = mp();
    if (!m) return {};
    int n = 0;
    for (auto& c : doc->prj.comps) if (c.feature == Feature::Component && c.assigned_point == point()) ++n;
    return std::to_string(n) + " placements" + (m->has_machine ? "" : "  \xE2\x80\xA2  no machine interface");
}

void MachineView::ensure_job() {
    if (!doc) return;
    if (job_gen_ == doc->gen && job_point_ == point()) return;
    job_gen_ = doc->gen;
    job_point_ = point();
    job_ = MachineJob{};
    job_err_.clear();
    const ModelPoint* m = mp();
    if (!m) { job_err_ = "No process point selected."; return; }
    if (!m->has_machine) { job_err_ = "This process point has no machine interface."; return; }
    if (!build_machine_job(doc->prj, point(), job_, job_err_) && job_err_.empty()) job_err_ = "Nothing to program.";
}

// --- export page: process point cards ----------------------------------------
std::string strip_ext(const std::string& f) {
    size_t d = f.find_last_of('.'), s = f.find_last_of("\\/");
    return d == std::string::npos || (s != std::string::npos && d < s) ? f : f.substr(0, d);
}
void help_text(const char* s, float w) {
    ImGui::PushStyleColor(ImGuiCol_Text, th().text_faint);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextUnformatted(s);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}
const char* kHelpOrient = "Rotates the whole board counter-clockwise for the machine (clockwise when Flipped). "
                          "Pick the direction that runs the carrier strips along the conveyor and keeps fiducials clear of the clamps.";
const char* kHelpFlip = "Mirrors the board for placing the bottom side. Turned on automatically when you choose Bottom; "
                        "turn it off only if the CAD data is already mirrored.";
const char* kHelpOffset = "Shifts every placement and fiducial by this amount (machine coordinates).";
// R13 (REVIEW_V12 §5): plain-language origin glossary
const char* kOriginGlossary =
    "Origins, in plain words:\n"
    "  Board origin - the 0,0 of the CAD / Gerber data as imported (where the designer put it).\n"
    "  Virtual origin - a project reference point you can move (Board menu); positions in this app are shown from it.\n"
    "  Global offset - a shift applied to the whole CAD data set to line it up with the Gerber (see Board > Align CAD to Gerber).\n"
    "  Machine origin - the point the placement machine calls X0 Y0, normally the board corner at the machine's datum.\n"
    "    By default it follows the virtual origin; override it here only if the machine needs a different datum.\n"
    "  Hard offset - a final fixed X/Y shift added to every machine coordinate.";
const char* kHelpForce = "Only matters for bottom-side (flipped) programs. Off (default, matches CircuitCAM output that runs on JUKI): "
                         "a part's angle is written as you see it in Board View. On: the angle is negated, as if the part were viewed "
                         "through the board. Leave off unless a machine places bottom parts turned the wrong way.";
const char* kHelpSingle = "Panels only. On: the program is one Single PWB containing every placement of every image. "
                          "Off (default): the program holds circuit 1 once and the machine repeats it (Matrix or Non-Matrix). "
                          "Turn on when the images are not identical copies, which the exporter otherwise refuses.";

void MachineView::draw_form(float width) {
    const GTheme& t = th();
    Doc& d = *doc;
    const float lw = S(128);
    const float cw = width - lw;
    auto row = [&](const char* label, const char* tip = nullptr) {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        if (tip) tooltip(tip);
        ImGui::SameLine(lw);
    };
    const int cur = point();
    draw_layer_list(width);
    if (cur < 0) return;
    {
        const int pi = cur;
        const bool sel = true;
        ModelPoint& mp = d.prj.model.points[pi];
        MachineSetup& ms = mp.machine;
        Exporter* ex = machine_exporter(ms.exporter_id);
        ImGui::PushID(pi);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
        ImGui::PushStyleColor(ImGuiCol_Border, t.border);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
        ImGui::BeginChild("##pcard", ImVec2(width + S(28), 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        {
            ImGui::PushFont(g_font_bold);
            ImGui::Text("Settings  \xE2\x80\x94  %s", mp.name.c_str());
            ImGui::PopFont();
            ImGui::SameLine(0, S(8));
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "%s", ms.side == Side::Bottom ? "bottom side" : "top side");
        }
        if (sel && !mp.has_machine) {
            help_text("This process point is not programmed by a machine (hand load, reflow, inspection...).", width);
            if (button("Enable Machine Interface", ui::Icon::Machine, true)) {
                d.checkpoint("Enable machine interface");
                mp.has_machine = true;
                if (ms.exporter_id.empty()) ms.exporter_id = "juki.h7h";
                ms.force_angle_calc = false;
                ms.side = mp.side;
                ms.flipped = mp.side == Side::Bottom;
                d.changed(ui::CH_MODEL);
            }
        } else if (sel) {
            ImGui::Dummy(ImVec2(0, S(4)));
            // ---- machine type
            row("Machine type");
            {
                std::vector<Exporter*> exps;
                for (auto& e : PluginRegistry::get().exporters()) if (is_machine_exporter(e.get())) exps.push_back(e.get());
                std::string prev = ex ? ex->info().name : (ms.exporter_id.empty() ? std::string("(select a machine)") : ms.exporter_id + " (not installed)");
                ImGui::SetNextItemWidth(cw - ImGui::GetFrameHeight() - S(4));
                if (ImGui::BeginCombo("##mtype", prev.c_str(), ImGuiComboFlags_HeightLarge)) {
                    for (Exporter* e : exps) {
                        bool is = e == ex;
                        std::string lbl = e->info().name + "##" + e->info().id;
                        if (ImGui::Selectable(lbl.c_str(), is) && !is) {
                            std::string id = e->info().id;
                            apply("Machine type", [id](MachineSetup& s) { s.exporter_id = id; });
                        }
                        if (ImGui::IsItemHovered()) tooltip(("." + exporter_ext(e) + "   " + e->info().id).c_str());
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine(0, S(4));
                int nopt = 0;
                for (auto& kv : ms.options) if (kv.first.rfind("__", 0) != 0) ++nopt;
                std::string tip = "Machine properties (transport direction, fixed rail, comment...)\n" + std::to_string(nopt) + " option(s) set";
                if (icon_button("mopt", ui::Icon::Settings, tip.c_str(), false, ex != nullptr, ImGui::GetFrameHeight())) {
                    DocPtr dp = doc;
                    defer([dp]() { if (MachineView* v = machine_view_of(dp)) v->machine_options(); });
                }
            }
            // ---- job / file name
            const std::string def = default_program_file(d.prj, pi, ex);
            const std::string ext = ex ? "." + exporter_ext(ex) : std::string();
            row("Job name", "Name of the program file written for the machine");
            {
                ImGui::SetNextItemWidth(cw - ImGui::CalcTextSize(ext.c_str()).x - S(10));
                std::string cur_stem = strip_ext(ms.target_file.empty() ? def : ms.target_file);
                if (bound_field("##job", job_buf_, cur_stem, strip_ext(def).c_str())) {
                    std::string v = job_buf_.empty() ? std::string() : sanitize_filename(strip_ext(job_buf_)) + ext;
                    if (v == def) v.clear();
                    if (v != ms.target_file) apply("Job name", [v](MachineSetup& s) { s.target_file = v; });
                }
                ImGui::SameLine(0, S(6));
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "%s", ext.c_str());
            }
            // ---- program identification (P1-06: from Project Info, the single source)
            row("Program ID", "PCB / PWB ID written into the program (Info tab). Empty = the job name is used.");
            {
                const AssemblyInfo& ai = d.prj.info;
                const std::string idv = !ai.pcb_id.empty() ? ai.pcb_id : std::string();
                ImGui::AlignTextToFramePadding();
                if (idv.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "(job name)");
                else ImGui::TextUnformatted(idv.c_str());
                ImGui::SameLine(0, S(8));
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "Author: %s", ai.originator.empty() ? "-" : ai.originator.c_str());
                tooltip("Written to the program's user / originator field");
                ImGui::SameLine(0, S(8));
                if (ImGui::SmallButton("Edit in Info")) { DocPtr dp = doc; defer([dp]() { activate_view(dp, ViewKind::Notes); }); }
            }
            // R19a: the output folder is project-wide (Export page bottom bar), not per layer
            ImGui::Dummy(ImVec2(0, S(2)));
            // ---- orientation / fiducials / side, with the live orientation preview beside them (REVIEW_V16 §13)
            const float pvw = S(190), pvh = S(132);
            const bool pv_side = width > S(600);
            const ImVec2 o_top = ImGui::GetCursorScreenPos();
            ImGui::BeginGroup();
            row("Orientation", kHelpOrient);
            {
                int oi = ((ms.orientation % 360 + 360) % 360) / 90;
                int no = segmented("orient", {"0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0"}, oi, S(54));
                tooltip(kHelpOrient);
                if (no != oi) apply("Board orientation", [no](MachineSetup& s) { s.orientation = no * 90; });
            }
            if (ms.exporter_id.rfind("juki.", 0) == 0) {
                row("Fiducial marks", "Board: the panel/PWB fiducials (board frame).\nCircuit: each circuit's own fiducials; the machine repeats them for every circuit.");
                auto it = ms.options.find("juki.fid_mode");
                const int fi = it != ms.options.end() && it->second == "circuit" ? 1 : 0;
                const int nf = segmented("fidmode", {"Board", "Circuit"}, fi, S(76));
                if (nf != fi) apply("Fiducial marks", [nf](MachineSetup& s) { s.options["juki.fid_mode"] = nf ? "circuit" : "board"; });
            }
            row("Board side", "Which side is placed; selects that side's fiducials");
            {
                int si = ms.side == Side::Bottom ? 1 : 0;
                int ns = segmented("side", {"Top", "Bottom"}, si, S(76));
                if (ns != si) apply("Board side", [ns](MachineSetup& s) { s.side = ns ? Side::Bottom : Side::Top; s.flipped = ns != 0; });
                ImGui::SameLine(0, S(16));
                bool fl = ms.flipped;
                if (toggle("Flipped", &fl, kHelpFlip)) apply("Flip board", [fl](MachineSetup& s) { s.flipped = fl; });
                if ((ms.side == Side::Bottom) != ms.flipped) {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + lw);
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.warn), ms.flipped ? "Flipped on a top-side program" : "Bottom side is usually flipped");
                }
            }
            {   // R21 (AUDIT_V20 P0/P1): the marks the program carries -- the writer's own selection (side, mode, Fiducial 1/2/3)
                row("Marks written", "Board / circuit fiducial marks written to the program, after the machine format's selection:\n"
                                     "only the programmed side's marks, chosen by Fiducial number (1, 2, 3), list order for unnumbered marks, at most 3.");
                ensure_job();
                MachineJob jm = job_;
                jm.machine = ms.exporter_id;
                std::vector<const JobFiducial*> wm;
                bool circ = false;
                if (ms.exporter_id.rfind("juki.", 0) == 0) { auto sel = juki::select_marks(jm); wm = sel.marks; circ = sel.fid_circuit; }
                else for (auto& f : jm.fiducials) wm.push_back(&f);
                if (job_err_.empty() && !wm.empty()) {
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.ok), "%d %s mark(s)", (int)wm.size(), circ ? "circuit" : "board");
                    for (auto* f : wm) {
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + lw);
                        char b[160];
                        std::snprintf(b, sizeof b, "%s  %s  X %.3f  Y %.3f", f->name.c_str(), f->number > 0 ? ("Fiducial " + std::to_string(f->number)).c_str() : "(no number)", f->x, f->y);
                        ImGui::TextUnformatted(b);
                    }
                } else ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ms.no_marks ? t.text_dim : t.error), "%s",
                                          !job_err_.empty() ? "- (nothing to program yet)" : ms.no_marks ? "none (intentional)" : "NONE - the program has no fiducial marks");
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + lw);
                bool nm = ms.no_marks;
                if (toggle("No marks (intentional)", &nm, "This layer really runs without fiducial marks: zero marks is then a note instead of an Error.\n"
                                                          "Marks are never borrowed from the other board side."))
                    apply("No marks (intentional)", [nm](MachineSetup& s) { s.no_marks = nm; });
            }
            ImGui::EndGroup();
            {
                const ImVec2 o_end = ImGui::GetCursorScreenPos();
                const float gx = ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x;
                if (pv_side) ImGui::SetCursorScreenPos(ImVec2(gx + width - pvw, o_top.y));
                draw_orient_preview(ImVec2(pvw, pvh));
                if (pv_side) ImGui::SetCursorScreenPos(ImVec2(o_end.x, std::max(o_end.y, o_top.y + pvh + S(4))));
            }
            // ---- advanced
            ImGui::Dummy(ImVec2(0, S(2)));
            {
                const bool non_default = ms.mirror_angle_when_flipped || ms.single_board || ms.has_origin;
                // R13 (REVIEW_V12 §5): the arrow is an icon (the UI font has no U+25B8/U+25BE: it rendered as "?")
                const bool nd = non_default || ms.hard_offset.x != 0 || ms.hard_offset.y != 0;
                std::string lbl = std::string("Advanced") + (nd && !adv_open_ ? "  (changed)" : "") + "##advx";
                ImGui::PushStyleColor(ImGuiCol_Text, nd ? t.accent : t.text_dim);
                const float isz = ImGui::GetFontSize();
                ImVec2 ap = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable(("      " + lbl).c_str(), false, 0, ImVec2(ImGui::CalcTextSize(lbl.c_str()).x + isz + S(30), 0))) adv_open_ = !adv_open_;
                ImGui::PopStyleColor();
                tooltip("Hard offsets, angle mirroring, single-board mode, machine-origin override, machine/line settings and the coordinate trace.\nNot needed for a normal export.");
                ImVec2 keep = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(ImVec2(ap.x + S(2), ap.y + (ImGui::GetTextLineHeight() - isz) * 0.5f));
                icon_image(adv_open_ ? ui::Icon::ArrowDown : ui::Icon::ArrowRight, isz);
                ImGui::SetCursorScreenPos(keep);
            }
            if (adv_open_) {
                ImGui::Indent(S(14));
                const float hw = width - S(14);
                auto arow = [&](const char* label, const char* tip) { row(label, tip); };
            std::string ul = U(app::units_label());
            arow(("Hard offset (" + ul + ")").c_str(), kHelpOffset);
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "X");
                ImGui::SameLine(0, S(4));
                ImGui::SetNextItemWidth(S(96));
                if (bound_field("##offx", offx_buf_, fmt_len_u8(ms.hard_offset.x, false))) {
                    double v = app::parse_len(W(offx_buf_), ms.hard_offset.x);
                    if (std::fabs(v - ms.hard_offset.x) > 1e-9) apply("Hard offset", [v](MachineSetup& s) { s.hard_offset.x = v; });
                }
                ImGui::SameLine(0, S(10));
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "Y");
                ImGui::SameLine(0, S(4));
                ImGui::SetNextItemWidth(S(96));
                if (bound_field("##offy", offy_buf_, fmt_len_u8(ms.hard_offset.y, false))) {
                    double v = app::parse_len(W(offy_buf_), ms.hard_offset.y);
                    if (std::fabs(v - ms.hard_offset.y) > 1e-9) apply("Hard offset", [v](MachineSetup& s) { s.hard_offset.y = v; });
                }
            }
                ImGui::Dummy(ImVec2(0, S(2)));
                bool fa = ms.mirror_angle_when_flipped, sb = ms.single_board;
                if (toggle("Mirror angles when flipped", &fa)) apply("Mirror angles when flipped", [fa](MachineSetup& s) { s.mirror_angle_when_flipped = fa; });
                help_text(kHelpForce, hw);
                if (toggle("Treat all images as a single board", &sb)) apply("Single board", [sb](MachineSetup& s) { s.single_board = sb; });
                help_text(kHelpSingle, hw);
                ImGui::Dummy(ImVec2(0, S(2)));
                ImGui::AlignTextToFramePadding();
                std::string org = ms.has_origin ? "Machine origin at " + fmt_len_u8(d.prj.to_user(ms.origin).x) + ", " +
                                                      fmt_len_u8(d.prj.to_user(ms.origin).y)
                                                : std::string("Machine origin: project virtual origin");
                ImGui::TextUnformatted(org.c_str());
                tooltip(kOriginGlossary);
                if (button("Set by Clicking", ui::Icon::VirtualOrigin)) { sub_ = 1; cv_.origin_mode = true; cv_.invalidate(); }
                tooltip("Override: click the board point the machine treats as X0 Y0 (a machine-coordinate view opens until you press Done).");
                ImGui::SameLine(0, S(6));
                if (button("Clear", ui::Icon::Delete, false, ImVec2(0, 0), ms.has_origin)) apply("Clear machine origin", [](MachineSetup& s) { s.has_origin = false; });
                ImGui::SameLine(0, S(8));
                ImGui::TextDisabled("(?)");
                tooltip(kOriginGlossary);
                ImGui::Dummy(ImVec2(0, S(2)));
                if (button("Machine / line settings...", ui::Icon::Settings, false, ImVec2(0, 0), ex != nullptr)) {
                    DocPtr dp = doc;
                    defer([dp]() { if (MachineView* v = machine_view_of(dp)) v->machine_options(); });
                }
                tooltip("Transport direction, fixed rail, program comment and other machine-specific options");
                ImGui::SameLine(0, S(6));
                {   // coordinate trace (was "Transform chain")
                    Project& pp = d.prj;
                    int sel_c = -1;
                    for (size_t k = 0; k < pp.comps.size(); ++k) if (pp.comps[k].selected && pp.comps[k].feature == Feature::Component) { sel_c = (int)k; break; }
                    if (button("Coordinate trace", ui::Icon::Report, false, ImVec2(0, 0), sel_c >= 0 && pp.comps[(size_t)sel_c].assigned_point >= 0)) {
                        PlacementTrace tr; std::string err;
                        if (trace_placement(pp, pp.comps[(size_t)sel_c].assigned_point, (size_t)sel_c, 0, tr, err)) { trace_text_ = trace_to_text(tr); angle_preview_open_ = true; }
                        else gui::status(err, ToastKind::Error);
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        tooltip("Coordinate trace: shows, step by step, how ONE selected placement's board X/Y/angle become the machine X/Y/angle\n"
                                "(circuit position -> board orientation -> bottom-side mirroring -> machine origin -> hard offset -> angle rules).\n"
                                "Use it to check a part that lands in the wrong place. Select a placement first.");
                }
                ImGui::Unindent(S(14));
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, S(4)));
    }
}

// --- tooling readiness (machine component database) ------------------------------
// R18d: machine-component setup will become a dedicated modal tool (specs/MACHINE_COMPONENTS.md, status deferred,
// Option 1). Until then the button only explains that and shows the readiness numbers; it no longer opens Data.
static void component_setup_modal(const flexdb::Readiness* r) {
    ui::Form f(L"Component setup (machine database)", 520);
    f.header(L"Component setup is planned").icon((int)ui::Icon::SelectPart);
    f.label(L"A dedicated tool for setting up parts in the machine's component database (package, nozzle, tape) is planned. "
            L"It will suggest settings read-only, you confirm each one, and it writes a decisions CSV; it never writes to a vendor database.");
    f.label(L"Status: deferred (specs/MACHINE_COMPONENTS.md, Option 1).");
    if (r && r->library) {
        f.label(L"Readiness of this export layer: " + std::to_wstring(r->parts) + L" part number(s): " + std::to_wstring(r->taught) + L" set up, " +
                std::to_wstring(r->autoa) + L" auto, " + std::to_wstring(r->untaught) + L" not set up, " + std::to_wstring(r->conflict) + L" conflict(s).");
        if (!r->attention.empty()) {
            std::wstring a;
            for (size_t i = 0; i < r->attention.size(); ++i) a += (i ? L", " : L"") + W(r->attention[i]);
            f.label(L"Needs setup: " + a + (r->open() > (int)r->attention.size() ? L", ..." : L""));
        }
    } else f.label(L"No machine component library is loaded, so readiness cannot be shown.");
    f.ok_text(L"Close");
    f.run(app::frame_hwnd());
}
void MachineView::refresh_readiness() {
    if (!doc) return;
    const int pt = point();
    const int fc = ImGui::GetFrameCount();
    if (rd_gen_ == doc->gen && rd_point_ == pt && fc - rd_frame_ < 30) return;
    rd_gen_ = doc->gen; rd_point_ = pt; rd_frame_ = fc;
    std::set<std::string> pns;
    for (auto& c : doc->prj.comps)
        if (c.feature == Feature::Component && c.assigned_point == pt) pns.insert(c.number.empty() ? c.package : c.number);
    rd_ok_ = tooling_readiness(doc, &pns, rd_);
}

void MachineView::draw_readiness(float width) {
    const GTheme& t = th();
    refresh_readiness();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
    ImGui::PushStyleColor(ImGuiCol_Border, rd_.library && rd_.open() ? t.warn : t.border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
    ImGui::BeginChild("##ready", ImVec2(width + S(28), 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    icon_image(ui::Icon::SelectPart, S(18));
    ImGui::SameLine(0, S(8));
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted("Tooling readiness");
    ImGui::PopFont();
    ImGui::SameLine(0, S(8));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "Component setup (machine database)");
    DocPtr d = doc;
    if (!rd_ok_ || !rd_.library) {
        help_text("Parts must be set up in the machine's component database (package, nozzle, tape) before the machine can run "
                  "this program. No machine component library is loaded, so readiness cannot be checked yet.", width);
        if (button("Component setup...", ui::Icon::SelectPart)) defer([]() { component_setup_modal(nullptr); });
    } else {
        const flexdb::Readiness& r = rd_;
        const int n = std::max(1, r.parts);
        // stacked bar
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float bh = S(8);
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + bh), t.raised, S(4));
        float x = p.x;
        auto seg = [&](int k, ImU32 c) { if (k <= 0) return; float w = width * k / n; dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + w, p.y + bh), c, S(4)); x += w; };
        seg(r.taught, t.ok); seg(r.autoa, t.accent); seg(r.conflict, t.error); seg(r.untaught, t.text_faint);
        ImGui::Dummy(ImVec2(width, bh + S(2)));
        auto stat = [&](int k, const char* label, ImU32 c) {
            ImVec2 q = ImGui::GetCursorScreenPos();
            dl->AddCircleFilled(ImVec2(q.x + S(5), q.y + ImGui::GetTextLineHeight() * 0.5f), S(4), c);
            ImGui::SetCursorScreenPos(ImVec2(q.x + S(13), q.y));
            ImGui::Text("%d %s", k, label);
            ImGui::SameLine(0, S(14));
        };
        stat(r.taught, "taught", t.ok);
        stat(r.autoa, "auto", t.accent);
        stat(r.untaught, "untaught", t.text_faint);
        stat(r.conflict, "conflicts", t.error);
        ImGui::NewLine();
        // tape / nozzle implications
        auto chips = [&](const char* cap, const std::map<std::string, int>& m) {
            if (m.empty()) return;
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "%s", cap);
            ImGui::SameLine(0, S(6));
            for (auto& kv : m) {
                std::string s = kv.first + "  \xC3\x97" + std::to_string(kv.second);
                ImVec2 ts = ImGui::CalcTextSize(s.c_str());
                if (ImGui::GetCursorScreenPos().x + ts.x + S(14) > ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - S(14)) { ImGui::NewLine(); }
                ImVec2 q = ImGui::GetCursorScreenPos();
                dl->AddRectFilled(q, ImVec2(q.x + ts.x + S(14), q.y + ts.y + S(6)), t.raised, S(8));
                dl->AddText(ImVec2(q.x + S(7), q.y + S(3)), t.text, s.c_str());
                ImGui::Dummy(ImVec2(ts.x + S(14), ts.y + S(6)));
                ImGui::SameLine(0, S(4));
            }
            ImGui::NewLine();
        };
        chips("Tape", r.tape);
        chips("Nozzle", r.nozzle);
        if (r.parts == 0) {
            help_text("The placements in this program carry no part numbers (package names only), so the machine database cannot be checked. "
                      "Import the BOM so every placement has a part number.", width);
        } else if (r.open()) {
            std::string a = "Needs teaching: ";
            for (size_t i = 0; i < r.attention.size(); ++i) a += (i ? ", " : "") + r.attention[i];
            if (r.open() > (int)r.attention.size()) a += ", ...";
            ImGui::PushStyleColor(ImGuiCol_Text, t.warn);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            ImGui::TextUnformatted(a.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            if (button("Fix next untaught", ui::Icon::FindRef, true)) defer([d]() { tooling_open(d, true); });
            ImGui::SameLine(0, S(6));
        } else {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.ok), "All %d part numbers have a machine component profile.", r.parts);
        }
        if (button("Component setup...", ui::Icon::SelectPart)) { flexdb::Readiness rr = r; defer([rr]() { component_setup_modal(&rr); }); }
        if (r.pending) {
            ImGui::SameLine(0, S(10));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.warn), "%d change(s) not yet written to the machine database", r.pending);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// --- export layer list (REVIEW_V16 §13: replaces the layer chips along the top and the per-layer cards) ------
void MachineView::draw_layer_list(float width) {
    const GTheme& t = th();
    Doc& d = *doc;
    auto& pts = d.prj.model.points;
    const int cur = point();
    std::vector<int> counts(pts.size(), 0);
    for (auto& c : d.prj.comps)
        if (c.feature == Feature::Component && c.assigned_point >= 0 && c.assigned_point < (int)counts.size()) counts[c.assigned_point]++;
    ImGui::PushFont(g_font_bold); ImGui::TextUnformatted("Export layers"); ImGui::PopFont();
    ImGui::SameLine(0, S(8));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "layers with a machine interface; assign placements in the layer list");
    const float rh = ImGui::GetFrameHeight() + S(6);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int shown = 0;
    for (int i = 0; i < (int)pts.size(); ++i) {
        const ModelPoint& m = pts[i];
        if (!m.has_machine && i != cur) continue;
        ++shown;
        const MachineSetup& ms = m.machine;
        Exporter* ex = machine_exporter(ms.exporter_id);
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = width + S(28);
        const bool sel = i == cur;
        // R18d: link button (PSP-style layer link) at the right end of the row: None, 1, 2, ...
        const float lw = S(54), lh = rh - S(10);
        const ImVec2 lp(p.x + w - lw - S(6), p.y + S(4));
        if (ImGui::InvisibleButton("row", ImVec2(w - lw - S(10), rh)) && !sel) set_current_point(d, i);
        const bool hov = ImGui::IsItemHovered();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh - S(2)), sel ? im_col(m.color, t.dark ? 60 : 38) : hov ? t.raised : t.panel, S(6));
        if (sel) dl->AddRect(p, ImVec2(p.x + w, p.y + rh - S(2)), im_col(m.color), S(6), 0, S(1.4f));
        ImGui::SetCursorScreenPos(lp);
        {
            DocPtr dp = doc;
            if (ImGui::InvisibleButton("link", ImVec2(lw, lh)))
                defer([dp, i]() { dp->checkpoint("Layer link"); cycle_link(dp->prj, i); dp->changed(ui::CH_MODEL); });
            const bool lhov = ImGui::IsItemHovered();
            const auto mem = link_members(d.prj, m.link);
            const bool bad = m.link && mem.size() != 2;
            const ImU32 lc = !m.link ? t.text_faint : bad ? t.warn : t.accent;
            dl->AddRectFilled(lp, ImVec2(lp.x + lw, lp.y + lh), with_alpha(lc, m.link ? 70 : (lhov ? 50 : 26)), S(5));
            dl->AddRect(lp, ImVec2(lp.x + lw, lp.y + lh), with_alpha(lc, 200), S(5));
            // chain glyph + ID
            const float gy = lp.y + lh * 0.5f;
            dl->AddCircle(ImVec2(lp.x + S(9), gy), S(3.5f), lc, 0, S(1.4f));
            dl->AddCircle(ImVec2(lp.x + S(15), gy), S(3.5f), lc, 0, S(1.4f));
            const std::string lt = link_label(m.link);
            dl->AddText(ImVec2(lp.x + S(22), gy - ImGui::GetTextLineHeight() * 0.5f), m.link ? t.text : t.text_faint, lt.c_str());
            if (lhov) {
                std::string tip = "Layer link: " + lt + ". Click to cycle None, 1, 2, ... (up to the next unused ID).\n"
                                  "Two layers with the same ID are one two-sided SMT job: the upper layer in the list is Side 1 (runs first), the lower is Side 2.\n"
                                  "Unlinked layers export on their own.";
                if (m.link && mem.size() == 2) tip += "\nThis layer is Side " + std::to_string(mem[0] == i ? 1 : 2) + " of link " + lt + ".";
                else if (bad) tip += "\nLink " + lt + " has " + std::to_string(mem.size()) + " layer(s); a pair needs exactly 2.";
                tooltip(tip.c_str());
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
        const float cy = p.y + (rh - S(2)) * 0.5f, ty = cy - ImGui::GetTextLineHeight() * 0.5f;
        dl->AddCircleFilled(ImVec2(p.x + S(14), cy), S(5), im_col(m.color));
        ImGui::PushFont(sel ? g_font_bold : g_font);
        dl->AddText(ImVec2(p.x + S(28), ty), t.text, m.name.c_str());
        float x = p.x + S(28) + ImGui::CalcTextSize(m.name.c_str()).x + S(10);
        ImGui::PopFont();
        const bool bot = ms.side == Side::Bottom;
        const char* chip = !m.has_machine ? "no machine" : bot ? "Bottom" : "Top";
        ImVec2 cs = ImGui::CalcTextSize(chip);
        dl->AddRectFilled(ImVec2(x, cy - cs.y * 0.5f - S(2)), ImVec2(x + cs.x + S(12), cy + cs.y * 0.5f + S(2)),
                          with_alpha(!m.has_machine ? t.text_faint : bot ? t.warn : t.accent, 60), S(7));
        dl->AddText(ImVec2(x + S(6), ty), t.text, chip);
        x += cs.x + S(22);
        const std::string info = std::to_string(counts[i]) + " placements" +
                                 (m.has_machine ? "   " + std::to_string(ms.orientation) + "\xC2\xB0" + (ms.flipped ? " flipped" : "") : std::string());
        dl->AddText(ImVec2(x, ty), counts[i] ? t.text_dim : t.warn, info.c_str());
        x += ImGui::CalcTextSize(info.c_str()).x + S(12);
        if (m.has_machine) {   // R18d: machine type always visible; none = warning, never a silent default
            const std::string mt = ex ? ex->info().name : ms.exporter_id.empty() ? std::string("No machine type") : ms.exporter_id + " (not installed)";
            dl->AddText(ImVec2(x, ty), ex ? t.text : t.warn, mt.c_str());
            x += ImGui::CalcTextSize(mt.c_str()).x;
        }
        if (m.has_machine && ex) {
            const std::string fn = ms.target_file.empty() ? default_program_file(d.prj, i, ex) : ms.target_file;
            ImVec2 fs = ImGui::CalcTextSize(fn.c_str());
            const float fx = std::max(x + S(16), p.x + w - lw - fs.x - S(18));
            dl->PushClipRect(ImVec2(fx, p.y), ImVec2(p.x + w - lw - S(12), p.y + rh), true);
            dl->AddText(ImVec2(fx, ty), t.text_faint, fn.c_str());
            dl->PopClipRect();
        }
        if (hov) tooltip(sel ? "Selected export layer: its settings are below." : "Show this layer's export settings");
        ImGui::PopID();
    }
    if (!shown) help_text("No layer has a machine interface yet.", width);
    ImGui::Dummy(ImVec2(0, S(6)));
}

// --- orientation preview: the board as the machine sees it (rotation, mirror, flip, origin) ----------------
void MachineView::draw_orient_preview(ImVec2 size) {
    const GTheme& t = th();
    const ModelPoint* m = mp();
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##orientpv", size);
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(o, o + size, t.canvas, S(8));
    dl->AddRect(o, o + size, t.border, S(8));
    if (!m) return;
    const Project& p = doc->prj;
    const MachineSetup& ms = m->machine;
    const int pt = point();
    BBox bb = images_bbox(p, true);
    if (bb.empty())
        for (auto& c : p.comps) if (c.feature == Feature::Component && c.assigned_point == pt) bb.add(c.pos);
    const char* cap = "machine view";
    dl->AddText(ImVec2(o.x + S(8), o.y + S(5)), t.text_faint, cap);
    if (bb.empty()) { dl->AddText(ImVec2(o.x + S(8), o.y + size.y * 0.5f), t.text_faint, "no board yet"); return; }
    if (bb.w() < 1e-6) bb.inflate(1);
    const Xform X = machine_xform(p, ms);
    const Vec2 q[4] = {X.apply({bb.x0, bb.y0}), X.apply({bb.x1, bb.y0}), X.apply({bb.x1, bb.y1}), X.apply({bb.x0, bb.y1})};
    BBox mb;
    for (auto& v : q) mb.add(v);
    mb.add(Vec2{0, 0});
    const float top = S(22), bot = S(22), sd = S(12);
    const double sc = std::min((size.x - 2 * sd) / std::max(1e-6, mb.w()), (size.y - top - bot) / std::max(1e-6, mb.h()));
    const Vec2 c = mb.center();
    const ImVec2 cc(o.x + size.x * 0.5f, o.y + top + (size.y - top - bot) * 0.5f);
    auto P = [&](Vec2 v) { return ImVec2(cc.x + (float)((v.x - c.x) * sc), cc.y - (float)((v.y - c.y) * sc)); };
    dl->PushClipRect(o, o + size, true);
    ImVec2 poly[4] = {P(q[0]), P(q[1]), P(q[2]), P(q[3])};
    dl->AddConvexPolyFilled(poly, 4, with_alpha(t.ok, t.dark ? 40 : 46));
    dl->AddPolyline(poly, 4, with_alpha(t.ok, 220), ImDrawFlags_Closed, S(1.5f));
    // placements of this layer (first image) and fiducials of its side
    int n = 0;
    for (auto& cm : p.comps) {
        if (cm.feature == Feature::Component && cm.assigned_point == pt && ++n < 3000)
            dl->AddCircleFilled(P(X.apply(cm.pos)), S(1.3f), with_alpha(t.text_dim, 150));
        else if (cm.feature == Feature::Fiducial && cm.side == ms.side) {
            const ImVec2 f = P(X.apply(cm.pos));
            dl->AddCircle(f, S(4), t.warn, 0, S(1.5f));
        }
    }
    // asymmetric "F" glyph in board coordinates: shows rotation and mirroring at a glance
    auto B = [&](double fx, double fy) { return P(X.apply({bb.x0 + bb.w() * fx, bb.y0 + bb.h() * fy})); };
    const float gw = S(3);
    dl->AddLine(B(0.12, 0.2), B(0.12, 0.8), t.accent, gw);
    dl->AddLine(B(0.12, 0.8), B(0.42, 0.8), t.accent, gw);
    dl->AddLine(B(0.12, 0.52), B(0.32, 0.52), t.accent, gw);
    // machine origin + X direction
    const ImVec2 z = P(Vec2{0, 0});
    dl->AddLine(ImVec2(z.x - S(6), z.y), ImVec2(z.x + S(6), z.y), t.error, S(1.5f));
    dl->AddLine(ImVec2(z.x, z.y - S(6)), ImVec2(z.x, z.y + S(6)), t.error, S(1.5f));
    dl->PopClipRect();
    const float ay = o.y + size.y - S(11);
    dl->AddLine(ImVec2(o.x + S(10), ay), ImVec2(o.x + S(44), ay), t.text_dim, S(1.5f));
    dl->AddTriangleFilled(ImVec2(o.x + S(48), ay), ImVec2(o.x + S(42), ay - S(4)), ImVec2(o.x + S(42), ay + S(4)), t.text_dim);
    dl->AddText(ImVec2(o.x + S(52), ay - ImGui::GetTextLineHeight() * 0.5f), t.text_faint, "X");
    char s[48];
    std::snprintf(s, sizeof s, "%d\xC2\xB0%s", ms.orientation, ms.flipped ? "  flipped" : "");
    ImVec2 ts = ImGui::CalcTextSize(s);
    dl->AddText(ImVec2(o.x + size.x - ts.x - S(8), o.y + size.y - ts.y - S(4)), t.text, s);
    if (hov)
        tooltip("The board as the machine sees it, updated live.\nThe F reads normally on the unrotated, unflipped top side; "
                "orientation turns it, Flipped mirrors it.\nDots: this layer's placements. Rings: fiducials of the placed side. "
                "Red cross: machine origin (X0 Y0).");
}

void MachineView::draw_sides(float width) {
    // R18d: the Production sides block is replaced by layer links (link button in Export layers). This card only
    // explains the selected layer's pair; it never creates layers or changes anything.
    const GTheme& t = th();
    const Project& p = doc->prj;
    const int cur = point();
    if (cur < 0 || cur >= (int)p.model.points.size() || !p.model.points[cur].link) return;
    const int link = p.model.points[cur].link;
    const auto mem = link_members(p, link);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel2);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    ImGui::BeginChild("##linkpair", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders, 0);
    ImGui::PushFont(g_font_bold);
    ImGui::Text("Linked pair %d", link);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "one two-sided SMT job; order = layer order");
    if (mem.size() != 2) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.warn));
        ImGui::TextWrapped("Link %d has %d layer(s). Give exactly two layers this link ID, or set this layer's link to None to export it on its own.",
                           link, (int)mem.size());
        ImGui::PopStyleColor();
    } else if (ImGui::BeginTable("##pair", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Run"); ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Board side"); ImGui::TableSetupColumn("Machine type");
        ImGui::TableHeadersRow();
        for (int k = 0; k < 2; ++k) {
            const ModelPoint& m = p.model.points[mem[k]];
            Exporter* ex = m.has_machine ? machine_exporter(m.machine.exporter_id) : nullptr;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::PushFont(g_font_bold); ImGui::Text("Side %d", k + 1); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s%s", side_name(m.machine.side), m.machine.flipped ? ", flipped" : "");
            ImGui::TableNextColumn();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ex ? t.text : t.warn), "%s", ex ? ex->info().name.c_str() : "No machine type");
        }
        ImGui::EndTable();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.text_faint));
        ImGui::TextWrapped("Move a layer up or down in the layer list to swap the run order. Each layer keeps its own machine type, "
                           "origin, rotation and Board side / Flipped; the program comment carries the side number.");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(6)));
}

// R17d: a validation row selects the placement and shows it in the board view (the machine-view tab is gone).
void MachineView::fly_to_ref(const std::string& ref) {
    if (!doc || ref.empty()) return;
    std::vector<int> idx;
    const auto& comps = doc->prj.comps;
    for (size_t i = 0; i < comps.size(); ++i)
        if (comps[i].ref == ref && comps[i].feature != Feature::Hole) idx.push_back((int)i);
    if (idx.empty()) { toast(ref + " is not on the board", ToastKind::Warning, 3.f); return; }
    focus_components(doc, idx, true);
}

// Pre-export validation list (sides + core checks), click a row to fly to it.
void MachineView::draw_validation(float width) {
    const GTheme& t = th();
    if (!doc) return;
    Project& p = doc->prj;
    const bool all = point() >= 0 && link_sides(p, p.model.points[point()].link).configured;   // R18d: linked pair
    const auto iss = app::export_validation(p, all ? -point() - 1 : point());
    int ne = 0, nw = 0, ni = 0;
    for (auto& i : iss) (i.sev == ExportIssue::Error ? ne : i.sev == ExportIssue::Warning ? nw : ni)++;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
    ImGui::PushStyleColor(ImGuiCol_Border, validated_point_ != point() ? t.border : ne ? t.error : nw ? t.warn : t.border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
    ImGui::BeginChild("##valid", ImVec2(width + S(28), 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    if (float fv = flash_amount(kValKey, 1.4f); fv > 0.01f)
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetWindowPos(), ImGui::GetWindowPos() + ImGui::GetWindowSize(), col_alpha(ne ? t.error : t.warn, 0.18f * fv), S(10));
    icon_image(validated_point_ == point() && ne ? ui::Icon::Warning : ui::Icon::Check, S(18));
    ImGui::SameLine(0, S(8));
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted("Validation");
    ImGui::PopFont();
    ImGui::SameLine(0, S(8));
    // REVIEW_V16 §13: the list appears once Validate or Generate was requested for this layer (then stays live)
    const bool checked = validated_point_ == point();
    if (!checked) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "not checked yet");
    else if (!ne && !nw) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.ok), "Ready%s", ni ? (" (" + std::to_string(ni) + " note(s))").c_str() : "");
    else if (ne) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.error), "%d error(s) - review before generating%s", ne, nw ? (", " + std::to_string(nw) + " warning(s)").c_str() : "");
    else ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.warn), "%d warning(s) - review, then generate", nw);
    tooltip("Errors and warnings are confirmed once on Generate (Generate anyway / Cancel). Notes never ask.");
    if (!checked) {
        help_text("Press Validate (or Generate) in the bar below to list errors and warnings here. Clicking a listed reference shows it on the board.", width);
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        ImGui::Dummy(ImVec2(0, S(6)));
        return;
    }
    if (iss.empty()) help_text("No problems found for this export.", width);
    if (!iss.empty()) {
        // collapsible panel: Errors / Warnings / Info groups (R12 §17)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.text_dim));
        if (ImGui::Selectable(val_open_ ? "Hide details" : "Show details...", false, 0, ImVec2(S(140), 0))) val_open_ = !val_open_;
        ImGui::PopStyleColor();
        if (val_open_) {
            static const char* gname[3] = {"Errors", "Warnings", "Information"};
            static const char* ghelp[3] = {"Conditions that prevent a valid export.", "Possible discrepancies: review, then generate anyway if they are expected.",
                                           "Non-critical diagnostics and comparisons (e.g. legacy program angles). Never blocks export."};
            const ExportIssue::Sev gsev[3] = {ExportIssue::Error, ExportIssue::Warning, ExportIssue::Info};
            int row = 0;
            for (int g = 0; g < 3; ++g) {
                int n = 0;
                for (auto& i : iss) n += i.sev == gsev[g];
                if (!n) continue;
                const ImU32 col = g == 0 ? t.error : g == 1 ? t.warn : t.text_dim;
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
                const std::string hdr = std::string(gname[g]) + " (" + std::to_string(n) + ")###vg" + std::to_string(g);
                const bool open = ImGui::CollapsingHeader(hdr.c_str(), g < 2 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) tooltip(ghelp[g]);
                if (!open) continue;
                const float h = std::min(S(260), (float)n * (ImGui::GetTextLineHeightWithSpacing() + S(2)) + S(8));
                ImGui::PushID(g);
                if (ImGui::BeginTable("##vlist", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(width, h))) {
                    ImGui::TableSetupColumn("ref", ImGuiTableColumnFlags_WidthFixed, S(80));
                    ImGui::TableSetupColumn("msg", ImGuiTableColumnFlags_WidthStretch);
                    for (auto& i : iss) {
                        if (i.sev != gsev[g]) continue;
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushID(row++);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(i.ref.empty() ? t.text_faint : t.accent));
                        if (ImGui::Selectable(i.ref.empty() ? "\xE2\x80\x94" : i.ref.c_str(), false, ImGuiSelectableFlags_SpanAllColumns) && !i.ref.empty()) {
                            std::string r = i.ref; DocPtr d = doc;
                            defer([d, r]() { if (MachineView* v = machine_view_of(d)) v->fly_to_ref(r); });
                        }
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered() && !i.ref.empty()) tooltip(("Select " + i.ref + " and show it on the board").c_str());
                        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", i.msg.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0, S(6)));
}

// R10 §17: explicit destination, file name, format and output status. Wording keeps "written" (file saved and
// read back byte-identical) apart from "verified" (checked in the machine software / on the machine).
void MachineView::draw_destination(float fw) {
    const GTheme& t = th();
    ModelPoint* m = mp();
    if (!m || !doc) return;
    Exporter* ex = m->has_machine ? machine_exporter(m->machine.exporter_id) : nullptr;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(8)));
    ImGui::BeginChild("##dest", ImVec2(fw, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(g_font_bold); ImGui::TextUnformatted("Output"); ImGui::PopFont();
    auto row = [&](const char* k, const std::string& v, ImU32 c) {
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim); ImGui::TextUnformatted(k); ImGui::PopStyleColor();
        ImGui::SameLine(S(110)); ImGui::PushStyleColor(ImGuiCol_Text, c); ImGui::TextWrapped("%s", v.c_str()); ImGui::PopStyleColor();
    };
    const std::string file = m->machine.target_file.empty() ? default_program_file(doc->prj, point(), ex) : m->machine.target_file;
    row("Format", ex ? ex->info().name + "  (." + exporter_ext(ex) + ")" : std::string("not selected"), ex ? t.text : t.warn);
    const std::string& odir = doc->prj.model.export_dir;   // R19a: project-wide output folder
    row("Folder", odir.empty() ? std::string("asked on Generate (none saved for this project)") : odir,
        odir.empty() ? t.text_faint : (dir_exists(W(odir)) ? t.text : t.warn));
    row("File name", file, t.text);
    auto it = export_status().find({doc.get(), m->name});
    if (it == export_status().end()) row("Status", "not generated in this session", t.text_faint);
    else {
        const ExportStatus& e = it->second;
        char kb[48]; std::snprintf(kb, sizeof kb, "%.1f KB", e.bytes / 1024.0);
        row("Status", e.readback ? "Written " + e.time + " (" + kb + ", " + std::to_string(e.placements) + " placements, " + std::to_string(e.fids) +
                                   " fiducials); read-back matches"
                                 : "Written " + e.time + " but the read-back did NOT match - regenerate", e.readback ? t.ok : t.error);
        row("Verified", "Not verified - check the program in the machine software or with a dry run before production", t.warn);
        row("Program id", "FNV-1a " + e.hash + "  (recorded in Info > Revision history)", t.text_dim);
        tooltip(U(e.path).c_str());
    }
    if (last_.point == point() && last_.ok) {   // result of the last Generate in this session
        if (float fg = flash_amount(kGenDoneKey, 1.6f); fg > 0.01f) {
            ImVec2 a0 = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a0.x - S(6), a0.y - S(2)), ImVec2(a0.x + fw - S(18), a0.y + ImGui::GetFrameHeight() * 2 + S(6)), col_alpha(t.ok, 0.22f * fg), S(6));
        }
        char kb[32];
        if (last_.bytes >= 1024) std::snprintf(kb, sizeof kb, "%.1f KB", last_.bytes / 1024.0);
        else std::snprintf(kb, sizeof kb, "%lld bytes", last_.bytes);
        std::string r = std::to_string(last_.placements) + " placements  \xE2\x80\xA2  " + std::to_string(last_.parts) + " parts  \xE2\x80\xA2  " +
                        std::to_string(last_.circuits) + " circuit(s)  \xE2\x80\xA2  " + std::to_string(last_.fids) + " fiducials  \xE2\x80\xA2  " + kb +
                        (last_.warnings ? "  \xE2\x80\xA2  " + std::to_string(last_.warnings) + " warning(s)" : std::string());
        icon_image(ui::Icon::Check, ImGui::GetFontSize());
        ImGui::SameLine(0, S(6));
        ImGui::PushStyleColor(ImGuiCol_Text, t.ok); ImGui::TextWrapped("%s", r.c_str()); ImGui::PopStyleColor();
        const std::wstring path = last_.path;
        const bool ok = file_exists(path);
        if (button("Open in program editor", ui::Icon::MachineProg, true, ImVec2(0, 0), ok)) defer([path]() { pnp_open(path); });
        tooltip("Open the generated program in the program editor");
        ImGui::SameLine(0, S(8));
        if (button("Open folder", ui::Icon::Folder, false, ImVec2(0, 0), ok)) defer([path]() {
            std::wstring a1 = L"/select,\"" + path + L"\"";
            ShellExecuteW(app::frame_hwnd(), L"open", L"explorer.exe", a1.c_str(), nullptr, SW_SHOWNORMAL);
        });
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(4)));
}

// R17d (REVIEW_V16 §13) export workspace:
//   left : export layers, production sides, settings of the selected layer (orientation preview beside
//          Orientation / Fiducial marks / Board side)
//   right: Validation, Output (incl. the last result + Open in program editor), Tool readiness
//   bottom: action bar with Validate and Generate
void MachineView::draw_programming(ImVec2 size) {
    const GTheme& t = th();
    ModelPoint* m = mp();
    Project& p = doc->prj;
    const bool all = point() >= 0 && link_sides(p, p.model.points[point()].link).configured;   // R18d: linked pair
    const int link = all ? p.model.points[point()].link : 0;
    const float bar_h = S(64);
    const bool two_col = size.x > S(1020);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float body_h = size.y - bar_h;
    const float left_w = two_col ? std::clamp(std::floor(size.x * 0.52f), S(560), S(820)) : size.x;
    const float right_w = size.x - left_w;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(14)));
    ImGui::BeginChild("##progform", ImVec2(left_w, body_h), ImGuiChildFlags_AlwaysUseWindowPadding, 0);
    {
        const float fw = ImGui::GetContentRegionAvail().x - S(30);
        ImGui::PushFont(g_font_big);
        ImGui::TextUnformatted("Export");
        ImGui::PopFont();
        ImGui::SameLine(0, S(10));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "machine program per export layer");
        ImGui::Dummy(ImVec2(0, S(2)));
        draw_form(fw);
        draw_sides(fw + S(28));
        if (!two_col) {
            draw_validation(fw);
            draw_destination(fw + S(28));
            if (m->has_machine) draw_readiness(fw);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    if (two_col) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + left_w, p0.y));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel2);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
        ImGui::BeginChild("##progright", ImVec2(right_w, body_h), ImGuiChildFlags_AlwaysUseWindowPadding, 0);
        ImGui::GetWindowDrawList()->AddLine(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y + body_h), t.border);
        const float fw = ImGui::GetContentRegionAvail().x - S(30);
        draw_validation(fw);
        draw_destination(fw + S(28));
        if (m->has_machine) draw_readiness(fw);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    // ---- action bar (full width)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 bp(p0.x, p0.y + body_h);
    dl->AddRectFilled(bp, ImVec2(bp.x + size.x, bp.y + bar_h), t.panel);
    dl->AddLine(bp, ImVec2(bp.x + size.x, bp.y), t.border, S(1.5f));
    Exporter* ex = m->has_machine ? machine_exporter(m->machine.exporter_id) : nullptr;
    const auto iss = app::export_validation(p, all ? -point() - 1 : point());
    int ne = 0;
    for (auto& i : iss) ne += i.sev == ExportIssue::Error;
    DocPtr d = doc;
    const float bh = S(40), by = bp.y + (bar_h - bh) * 0.5f;
    const bool have_prog = last_.point == point() && last_.ok && file_exists(last_.path);
    const float gw = all ? S(250) : S(200), vw = S(130), ow = S(200), aw = all ? S(150) : 0.f;
    float x = bp.x + size.x - S(20) - gw - S(10) - aw - (all ? S(10) : 0) - vw - (have_prog ? S(10) + ow : 0);
    // R19a: left = the project-wide output folder with Browse / Copy path / Open folder (no status text)
    {
        const std::string& od = p.model.export_dir;
        const std::wstring wod = W(od);
        const bool exists = dir_exists(wod);
        const float cbw = S(118), obw = S(128), bbw = bh, gap = S(8);
        const float lx = bp.x + S(20);
        const float lab_w = ImGui::CalcTextSize("Output folder").x + S(12);
        const float path_w = std::max(S(60), x - S(16) - lx - lab_w - bbw - cbw - obw - gap * 3);
        const float ty = bp.y + (bar_h - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText(ImVec2(lx, ty), t.text_dim, "Output folder");
        const float px = lx + lab_w;
        const ImVec2 pa(px, by), pb(px + path_w, by + bh);
        dl->AddRectFilled(pa, pb, t.panel2, S(6));
        dl->AddRect(pa, pb, od.empty() || exists ? t.border : t.warn, S(6));
        const std::string shown = od.empty() ? std::string("Ask where to save on Generate") : od;
        dl->PushClipRect(ImVec2(pa.x + S(8), pa.y), ImVec2(pb.x - S(8), pb.y), true);
        dl->AddText(ImVec2(pa.x + S(10), ty), od.empty() ? t.text_faint : exists ? t.text : t.warn, shown.c_str());
        dl->PopClipRect();
        ImGui::SetCursorScreenPos(pa);
        ImGui::InvisibleButton("##odir", ImVec2(path_w, bh));
        if (ImGui::IsItemHovered())
            {
                std::string tp = od.empty() ? std::string("No output folder saved: Generate asks where to save.") : od + (exists ? "" : "\nFolder not found on this PC.");
                tp += "\nOne folder for every export layer; file names stay per layer (<Project Name> <layer name>).";
                tooltip(tp.c_str());
            }
        ImGui::SetCursorScreenPos(ImVec2(pb.x + gap, by));
        if (icon_button("odbrowse", ui::Icon::Open, "Choose the output folder for all export layers", false, true, bbw)) {
            DocPtr dp = doc;
            defer([dp]() {
                std::wstring w = W(dp->prj.model.export_dir);
                if (w.empty() || !dir_exists(w)) w = fallback_export_dir(*dp);
                if (!ui::pick_folder_dialog(app::frame_hwnd(), L"Output Folder", w)) return;
                dp->checkpoint("Output folder");
                dp->prj.model.export_dir = U(w);
                dp->changed(ui::CH_MODEL);
            });
        }
        ImGui::SetCursorScreenPos(ImVec2(pb.x + gap * 2 + bbw, by));
        const std::string full = (od.empty() || !ex) ? od : U(join_path(wod, W(m->machine.target_file.empty() ? default_program_file(p, point(), ex) : m->machine.target_file)));
        if (button("Copy path", ui::Icon::Copy, false, ImVec2(cbw, bh), !od.empty())) { ImGui::SetClipboardText(full.c_str()); toast("Path copied", ToastKind::Success, 2.f); }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip(od.empty() ? "No output folder saved yet." : ("Copy the program path of this layer\n" + full).c_str());
        ImGui::SetCursorScreenPos(ImVec2(pb.x + gap * 3 + bbw + cbw, by));
        if (button("Open folder", ui::Icon::Open, false, ImVec2(obw, bh), exists)) { std::wstring w = wod; defer([w]() { ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }); }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip(exists ? "Open the output folder in Explorer" : "The folder does not exist on this PC yet (Generate creates it).");
    }
    // right: [Open in program editor] [Validate] [Generate]
    ImGui::SetCursorScreenPos(ImVec2(x, by));
    if (have_prog) {
        std::wstring pp = last_.path;
        if (button("Open in program editor", ui::Icon::MachineProg, false, ImVec2(ow, bh))) defer([pp]() { pnp_open(pp); });
        tooltip(("Open the generated program in the program editor\n" + U(pp)).c_str());
        ImGui::SameLine(0, S(10));
    }
    if (button("Validate", ui::Icon::Check, false, ImVec2(vw, bh), m->has_machine)) { validated_point_ = point(); flash(kValKey); }
    tooltip("Check this export now; errors and warnings are listed in Validation.");
    ImGui::SameLine(0, S(10));
    if (all) {
        const int cp = point();
        if (button("Generate linked pair", ui::Icon::Generate, true, ImVec2(gw, bh), m->has_machine))
            defer([d, cp, link]() {
                MachineView* v = machine_view_of(d);
                if (v) { v->validated_point_ = v->point(); flash(kValKey); }
                std::string e;   // R18d: export_all_sides asks once (Generate anyway / Cancel); nothing blocks
                if (!app::export_all_sides(app::frame_hwnd(), *d, link, e) && !e.empty() && e != kCancelled) ui::message(app::frame_hwnd(), L"Generate linked pair", ui::W(e));
            });
        tooltip("Validates, then writes one program per layer of this link (upper layer = Side 1, lower = Side 2), each with its own file name, + placements CSV + report.\nErrors and warnings are confirmed once (Generate anyway / Cancel).");
        ImGui::SameLine(0, S(10));
        if (button("This layer only", ui::Icon::Generate, false, ImVec2(aw, bh), ex != nullptr))
            defer([d]() { if (MachineView* v = machine_view_of(d)) v->run_generate(); });
    } else {
        if (button("Generate program", ui::Icon::Generate, true, ImVec2(gw, bh), ex != nullptr))
            defer([d]() { if (MachineView* v = machine_view_of(d)) v->run_generate(); });
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            tooltip(!ex ? "Select a machine type first."
                        : ne ? "Validation has errors: Generate asks once (Generate anyway / Cancel)."
                             : "Validates, asks you to confirm any warnings, then writes the program, a placements CSV and a report.");
    }
}

// Temporary machine-coordinate canvas, only for "Set by Clicking" (machine origin override).
void MachineView::draw_origin_pick(ImVec2 size) {
    const GTheme& t = th();
    const float bh = S(40);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + bh), col_alpha(t.accent, 0.16f));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + S(10), p0.y + S(6)));
    if (button("Done", ui::Icon::Check, true)) { sub_ = 0; cv_.origin_mode = false; }
    ImGui::SameLine(0, S(12));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "%s", cv_.origin_mode ? "Click the board point the machine uses as X0 Y0 (machine coordinates)." : "Machine origin set. Press Done to return to the export settings.");
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + bh));
    draw_machine_view(ImVec2(size.x, size.y - bh));
}

void MachineView::draw_machine_view(ImVec2 size) {
    ModelPoint* m = mp();
    const MachineSetup& ms = m->machine;
    long long sig = (long long)ms.orientation * 8 + (ms.flipped ? 4 : 0) + (ms.has_origin ? 2 : 0);
    sig ^= (long long)(ms.hard_offset.x * 1000) * 31 + (long long)(ms.hard_offset.y * 1000) * 17 + (long long)(ms.origin.x * 1000) * 7 +
           (long long)(ms.origin.y * 1000) + (long long)point() * 1000003;
    if (sig != mx_sig_) { if (mx_sig_ != -1) cv_.zoom_all(); mx_sig_ = sig; cv_.invalidate(); }
    if (doc->gen != cv_gen_ || point() != cv_point_) { cv_gen_ = doc->gen; cv_point_ = point(); cv_.invalidate(); }
    if (doc->prj.comps.size() != angle_err_.size()) angle_err_.clear();
    cv_.set_grid(app::settings().grid_visible, app::settings().grid_mm);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    cv_.draw("##machcv", size);
    // floating tool bar (top-right)
    {
        const GTheme& t = th();
        const float bs = S(30), n = 5;
        ImVec2 a(p0.x + size.x - (bs + S(2)) * n - S(18), p0.y + S(10));
        ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + (bs + S(2)) * n + S(8), a.y + bs + S(8)), with_alpha(t.panel, 235), S(10));
        ImGui::GetWindowDrawList()->AddRect(a, ImVec2(a.x + (bs + S(2)) * n + S(8), a.y + bs + S(8)), t.border, S(10));
        ImGui::SetCursorScreenPos(ImVec2(a.x + S(4), a.y + S(4)));
        DocPtr d = doc;
        if (icon_button("mzall", ui::Icon::ZoomAll, "Zoom all (Home)")) cv_.zoom_all();
        ImGui::SameLine(0, S(2));
        if (icon_button("morg", ui::Icon::VirtualOrigin, "Set machine-specific origin by clicking", cv_.origin_mode)) { cv_.origin_mode = !cv_.origin_mode; cv_.invalidate(); }
        ImGui::SameLine(0, S(2));
        if (icon_button("mcalc", ui::Icon::Rot90, "Preview machine angles", angle_preview_open_)) angle_preview_open_ = !angle_preview_open_;
        ImGui::SameLine(0, S(2));
        if (icon_button("mlbl", ui::Icon::MoveLabel, "Show labels", doc->show_labels)) { doc->show_labels = !doc->show_labels; doc->changed(ui::CH_SELECTION); }
        ImGui::SameLine(0, S(2));
        if (icon_button("mgen", ui::Icon::Generate, "Generate program", false, machine_exporter(ms.exporter_id) != nullptr))
            defer([d]() { if (MachineView* v = machine_view_of(d)) v->run_generate(); });
    }
    // context menu
    if (ImGui::BeginPopup("##mvctx")) {
        const int ci = cv_.ctx_comp;
        if (ci >= 0 && ci < (int)doc->prj.comps.size()) {
            const Component& c = doc->prj.comps[ci];
            ImGui::TextDisabled("%s  %s  [%s]", c.ref.c_str(), c.number.c_str(), c.package.c_str());
            ImGui::Separator();
        }
        DocPtr d = doc;
        auto item = [&](const char* label, int action, bool enabled = true, bool checked = false) {
            if (ImGui::MenuItem(label, nullptr, checked, enabled)) {
                if (action == M_ORIGIN) { cv_.origin_mode = !cv_.origin_mode; cv_.invalidate(); }
                else defer([d, action, ci]() { if (MachineView* v = machine_view_of(d)) v->ctx_action(action, ci); });
            }
        };
        item("Preview Machine Angles", M_PREVIEW);
        item("Show Transform Chain...", M_TRACE, ci >= 0 || doc->selected_count() > 0);
        ImGui::Separator();
        item("Set Machine-Specific Origin", M_ORIGIN, true, cv_.origin_mode);
        item("Clear Machine-Specific Origin", M_CLR_ORIGIN, ms.has_origin);
        ImGui::Separator();
        if (menu_item_icon("Zoom All", ui::Icon::ZoomAll, "Home")) cv_.zoom_all();
        ImGui::EndPopup();
    }
}

// Read-only "Preview machine angles" window: exported angle per placement + convention.
void MachineView::draw_angle_preview() {
    if (!angle_preview_open_) return;
    ModelPoint* m = mp();
    if (!m) return;
    const Project& prj = doc->prj;
    const int pt = point();
    ImGui::SetNextWindowSize(ImVec2(S(720), S(460)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Preview Machine Angles###mvangles", &angle_preview_open_)) { ImGui::End(); return; }
    ImGui::TextWrapped("%s", angle_convention_text(m->machine).c_str());
    ImGui::TextDisabled("Read-only. Change rotations in Board View (Group Rotate). Nothing to neutralize.");
    if (!trace_text_.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted(trace_text_.c_str());
        if (ImGui::SmallButton("Copy chain")) ImGui::SetClipboardText(trace_text_.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Close chain")) trace_text_.clear();
    }
    ImGui::Separator();
    if (ImGui::BeginTable("##angt", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* h : {"Ref", "Package", "Side", "Board rot", "Pre-rot (CW)", "Machine angle", "Legacy CircuitCAM"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < prj.comps.size(); ++i) {
            const Component& c = prj.comps[i];
            if (c.feature != Feature::Component || c.assigned_point != pt) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(c.ref.c_str(), c.selected, ImGuiSelectableFlags_SpanAllColumns)) {
                PlacementTrace tr; std::string err;
                if (trace_placement(prj, pt, i, 0, tr, err)) trace_text_ = trace_to_text(tr);
            }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.package.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.side == Side::Bottom ? "Bottom" : "Top");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", c.rot);
            ImGui::TableNextColumn(); if (c.pre_rotation != 0) ImGui::Text("%.3f", c.pre_rotation);
            ImGui::TableNextColumn(); ImGui::Text("%.3f", export_angle(prj, c, 0, m->machine));
            ImGui::TableNextColumn();
            if (c.has_legacy_mach_rot) {
                if (legacy_angle_differs(c)) ImGui::TextColored(ImVec4(1.f, .55f, .1f, 1.f), "%.3f (differs)", c.legacy_mach_rot);
                else ImGui::TextDisabled("%.3f", c.legacy_mach_rot);
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void MachineView::draw(ImVec2 size) {
    if (!doc) return;
    // R17d (REVIEW_V16 §13): no layer chips / Setup-Machine View toggle; the export layer list is in the page
    if (!mp()) { if (gui::empty_state(ui::Icon::Machine, "No export layer", "Add export layers to the factory model first.", "Factory Model...", ui::Icon::FactoryModel)) post_command(app::ID_LIB_FACTORY_MODEL); return; }
    if (sub_ == 1 && mp()->has_machine) draw_origin_pick(size);
    else { sub_ = 0; draw_programming(size); }
    draw_angle_preview();   // floating; from Advanced > Coordinate trace and the toolbar
}

void MachineView::draw_toolbar() {
    ModelPoint* m = mp();
    const bool en = m && m->has_machine;
    DocPtr d = doc;
    if (icon_button("tgen", ui::Icon::Generate, "Generate program", false, en && machine_exporter(m->machine.exporter_id)))
        defer([d]() { if (MachineView* v = machine_view_of(d)) v->run_generate(); });
    ImGui::SameLine(0, S(2));
    if (icon_button("topt", ui::Icon::Settings, "Machine options", false, en && machine_exporter(m->machine.exporter_id)))
        defer([d]() { if (MachineView* v = machine_view_of(d)) v->machine_options(); });
    ImGui::SameLine(0, S(2));
    if (icon_button("tang", ui::Icon::Rot90, "Machine angle preview\nBoard angle, pre-rotation and machine angle of every placement,\nplus the transform chain of the selected one.", angle_preview_open_, en))
        angle_preview_open_ = !angle_preview_open_;
    if (sub_ == 1 && en) {
        ImGui::SameLine(0, S(10));
        if (icon_button("tzall", ui::Icon::ZoomAll, "Zoom all")) cv_.zoom_all();
        ImGui::SameLine(0, S(2));
        if (icon_button("tzin", ui::Icon::ZoomIn, "Zoom in")) cv_.zoom_by(1.25);
        ImGui::SameLine(0, S(2));
        if (icon_button("tzout", ui::Icon::ZoomOut, "Zoom out")) cv_.zoom_by(0.8);
        ImGui::SameLine(0, S(2));
        if (icon_button("torg", ui::Icon::VirtualOrigin, "Set machine-specific origin by clicking", cv_.origin_mode)) { cv_.origin_mode = !cv_.origin_mode; cv_.invalidate(); }
    }
}

bool MachineView::on_command(int id) {
    switch (id) {
    case ID_VIEW_ZOOM_IN: if (sub_ == 1) { cv_.zoom_by(1.25); return true; } return false;
    case ID_VIEW_ZOOM_OUT: if (sub_ == 1) { cv_.zoom_by(0.8); return true; } return false;
    case ID_VIEW_ZOOM_ALL: if (sub_ == 1) { cv_.zoom_all(); return true; } return false;
    case ID_VIEW_ZOOM_100: if (sub_ == 1) { cv_.zoom_100(); return true; } return false;
    case ID_VIEW_ZOOM_RECT: if (sub_ == 1) { cv_.begin_zoom_rect(); return true; } return false;
    case ID_POINT_GENERATE: {
        ModelPoint* m = mp();
        if (!m || !m->has_machine) return false;
        run_generate();   // already outside the frame (posted command)
        return true;
    }
    }
    return false;
}

int MachineView::cmd_state(int id) {
    switch (id) {
    case ID_VIEW_ZOOM_IN: case ID_VIEW_ZOOM_OUT: case ID_VIEW_ZOOM_ALL: case ID_VIEW_ZOOM_100: case ID_VIEW_ZOOM_RECT:
        return sub_ == 1 ? 1 : 0;
    case ID_FILE_PRINT: case ID_FILE_PRINT_1TO1: case ID_FILE_PRINT_PREVIEW: return sub_ == 1 ? 1 : 0;
    case ID_POINT_GENERATE: { ModelPoint* m = mp(); return m && m->has_machine ? 1 : 0; }
    }
    return -1;
}

void MachineView::print(HDC dc, const RECT& page, bool one_to_one) {
    ModelPoint* m = mp();
    if (!m) return;
    const Doc& d = *doc;
    const MachineSetup& ms = m->machine;
    const int dpx = GetDeviceCaps(dc, LOGPIXELSX), dpy = GetDeviceCaps(dc, LOGPIXELSY);
    const float ppmx = dpx / 25.4f, ppmy = dpy / 25.4f;
    const float W0 = (float)(page.right - page.left), H0 = (float)(page.bottom - page.top);
    Xform mxf = machine_xform(d.prj, ms);
    BBox bb = images_bbox(d.prj, true), mb;
    if (!bb.empty()) {
        mb.add(mxf.apply({bb.x0, bb.y0})); mb.add(mxf.apply({bb.x1, bb.y0}));
        mb.add(mxf.apply({bb.x1, bb.y1})); mb.add(mxf.apply({bb.x0, bb.y1}));
    }
    mb.add(Vec2{0, 0});
    if (mb.w() < 1 || mb.h() < 1) mb.inflate(10);
    float sx, sy;
    if (one_to_one) { sx = ppmx * (float)app::settings().print_scale_x; sy = ppmy * (float)app::settings().print_scale_y; }
    else { float k = std::min(W0 * 0.92f / (float)mb.w(), H0 * 0.92f / (float)mb.h()); sx = sy = k; }
    const Vec2 c = mb.center();
    D2D1_MATRIX_3X2_F wm = D2D1::Matrix3x2F(sx, 0, 0, -sy, (float)(W0 / 2 - sx * c.x + page.left), (float)(H0 / 2 + sy * c.y + page.top));
    std::vector<bool> errs = angle_err_;
    const int pt = point();
    print_bands(dc, page, [&](ID2D1RenderTarget* rt, const D2D1_MATRIX_3X2_F& band) {
        app::BoardRenderOpts o;
        o.world = to_d2d(mxf) * wm * band;
        o.px_per_mm = sx;
        o.dip_per_px = std::max(1.f, dpx / 150.f);
        o.mirror = ms.flipped;
        o.focus_point = pt;
        o.only_focus = true;
        o.show_labels = d.show_labels;
        o.show_origin = false;
        o.show_selection = false;
        o.print_mode = true;
        if (errs.size() == d.prj.comps.size()) o.angle_errors = &errs;
        app::render_board(rt, d, o);
        Com<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.1f, 0.1f), b.put());
        if (b) {
            D2D1_MATRIX_3X2_F m2 = wm * band;
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
            D2D1_POINT_2F o0 = xfp(m2, 0, 0);
            float L = 0.25f * dpx;
            rt->DrawLine(D2D1::Point2F(o0.x - L, o0.y), D2D1::Point2F(o0.x + L, o0.y), b.get(), dpx / 100.f);
            rt->DrawLine(D2D1::Point2F(o0.x, o0.y - L), D2D1::Point2F(o0.x, o0.y + L), b.get(), dpx / 100.f);
        }
    });
}

// ===========================================================================
// Project Info view (R17d, REVIEW_V16 §12)
//   left : identity fields (5) in a responsive grid, project notes, layer notes (collapsible)
//   right: revision notes (one-line list) + revision history with restorable checkpoints
// Fields removed from the UI (number, PCB rev, BOM no., mfg rev, standard, project ids, description) stay in
// the file; while a legacy project still carries any of them a small "Older fields" disclosure shows them
// (read-only, with Clear) because Assembly / board no. still names the program file when it is set.
// ===========================================================================
class NotesView : public View {
public:
    ViewKind kind() const override { return ViewKind::Notes; }
    const char* title() const override { return "Project Info"; }
    ui::Icon icon() const override { return ui::Icon::Notes; }
    bool can_print() const override { return true; }
    void draw(ImVec2 size) override;
    void print(HDC dc, const RECT& page, bool one_to_one) override;
    std::string status() const override {
        if (!doc) return {};
        int ck = 0;
        for (auto& r : doc->prj.history) ck += !r.snapshot.empty();
        return std::to_string(doc->prj.history.size()) + " revision(s), " + std::to_string(ck) + " restorable";
    }
private:
    void draw_identity(float w);
    void draw_notes(float w, float h);
    void draw_layer_notes(float w);
    void draw_revision_notes(float w, float h);
    void draw_history(float w, float h);
    std::string buf_, rev_buf_, pnotes_, cp_comment_;
    bool pcp_ = false, fcp_ = false, cp_ = false;
    int edit_rev_ = -1;
    std::string edit_buf_;
    int layer_ = -1;
    bool layer_open_ = false, layer_init_ = false, legacy_open_ = false;
};

void section_title(const char* s, const char* sub = nullptr) {
    ImGui::PushFont(g_font_bold); ImGui::TextUnformatted(s); ImGui::PopFont();
    if (sub) { ImGui::SameLine(0, S(8)); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(th().text_faint), "%s", sub); }
}

void NotesView::draw(ImVec2 size) {
    if (!doc) return;
    const float pad = S(18), gap = S(18);
    const bool wide = size.x > S(1000);
    ImGui::SetCursorPos(ImVec2(pad, S(12)));
    ImGui::PushFont(g_font_big); ImGui::TextUnformatted("Project Info"); ImGui::PopFont();
    ImGui::SameLine(0, S(10)); ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(th().text_dim), "identity, notes and revisions");
    const float top = ImGui::GetCursorPosY() + S(4);
    if (wide) {
        const float lw = std::floor((size.x - 2 * pad - gap) * 0.56f), rw = size.x - 2 * pad - gap - lw, h = size.y - top - S(12);
        ImGui::SetCursorPos(ImVec2(pad, top));
        ImGui::BeginChild("##pi_l", ImVec2(lw, h), 0, 0);
        draw_identity(lw);
        draw_layer_notes(lw);
        draw_notes(lw, ImGui::GetContentRegionAvail().y);
        ImGui::EndChild();
        ImGui::SetCursorPos(ImVec2(pad + lw + gap, top));
        ImGui::BeginChild("##pi_r", ImVec2(rw, h), 0, 0);
        const float rh = std::clamp(h * 0.34f, S(150), S(300));
        draw_revision_notes(rw, rh);
        ImGui::Dummy(ImVec2(0, S(8)));
        draw_history(rw, ImGui::GetContentRegionAvail().y);
        ImGui::EndChild();
    } else {
        ImGui::SetCursorPos(ImVec2(pad, top));
        const float w = size.x - 2 * pad;
        ImGui::BeginChild("##pi_s", ImVec2(w + S(8), size.y - top - S(8)), 0, 0);
        const float cw = w - S(14);
        draw_identity(cw);
        draw_layer_notes(cw);
        draw_notes(cw, S(150));
        ImGui::Dummy(ImVec2(0, S(8)));
        draw_revision_notes(cw, S(200));
        ImGui::Dummy(ImVec2(0, S(8)));
        draw_history(cw, S(320));
        ImGui::EndChild();
    }
}

void NotesView::draw_identity(float w) {
    const GTheme& t = th();
    AssemblyInfo& a = doc->prj.info;
    struct FT { const char* label; std::string* v; const char* hint; const char* tip; float span; };
    // REVIEW_V16 §12 field table: Customer, Project Name, Board ID, BOM Revision, Author (+ Notes below)
    FT fields[] = {
        {"Project name", &a.name, "e.g. Controller board", "Program (job) name and file name (an imported program keeps its own name)", 2},
        {"Customer", &a.customer, "Customer name", "Printed on reports and visual aids", 1},
        {"Board ID", &a.pcb_id, "Bare board part number", "Board (PWB) ID written into the program.\nNames the program only when the Project name is empty.", 1},
        {"BOM revision", &a.bom_rev, "e.g. 3", "Printed on reports and visual aids", 1},
        {"Author", &a.originator, "Who programs this job", "Written to the machine program's line name / user field and to the revision history", 1},
    };
    const int cols = w > S(760) ? 3 : w > S(440) ? 2 : 1;
    const float cg = S(12), colw = (w - cg * (cols - 1)) / cols;
    int col = 0;
    float row_y = ImGui::GetCursorPosY();
    for (int i = 0; i < (int)(sizeof fields / sizeof fields[0]); ++i) {
        const int span = std::min(cols, (int)fields[i].span);
        if (col + span > cols) col = 0;
        if (col > 0) { ImGui::SameLine(0, cg); ImGui::SetCursorPosY(row_y); }
        else row_y = ImGui::GetCursorPosY();
        ImGui::PushID(i);
        ImGui::BeginGroup();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "%s", fields[i].label);
        if (fields[i].tip) tooltip(fields[i].tip);
        ImGui::SetNextItemWidth(colw * span + cg * (span - 1));
        std::string v = *fields[i].v;
        if (input_str("##f", v, fields[i].hint)) {
            if (!fcp_) { doc->checkpoint(std::string("Edit ") + fields[i].label); fcp_ = true; }
            *fields[i].v = v; doc->prj.dirty = true;
        }
        if (ImGui::IsItemHovered() && fields[i].tip) tooltip(fields[i].tip);
        if (ImGui::IsItemDeactivated() && fcp_) { fcp_ = false; doc->changed(ui::CH_INFO); }
        ImGui::EndGroup();
        ImGui::PopID();
        col += span;
        if (col >= cols) col = 0;
    }
    // older fields (kept in the file for compatibility; shown only while a project still carries them)
    struct LF { const char* label; std::string* v; const char* note; };
    LF legacy[] = {
        {"Assembly / board no.", &a.number, "names the program only when Project name and Board ID are empty"},
        {"PCB revision", &a.assembly_rev, nullptr}, {"BOM number", &a.bom_number, nullptr}, {"Manufacturing rev.", &a.manf_rev, nullptr},
        {"Standard", &a.standard, nullptr}, {"Project id 1", &a.custom[0], nullptr}, {"Project id 2", &a.custom[1], nullptr},
        {"Description", &a.description, "copy it into Notes, then Clear"},
    };
    int nl = 0;
    for (auto& l : legacy) nl += !l.v->empty();
    if (nl) {
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(a.number.empty() || !a.pcb_id.empty() || !a.name.empty() ? t.text_faint : t.warn));
        std::string lbl = std::string(legacy_open_ ? "Hide" : "Show") + " " + std::to_string(nl) + " older field(s) from this project##legacy";
        if (ImGui::Selectable(lbl.c_str(), false, 0, ImVec2(ImGui::CalcTextSize(lbl.c_str()).x - ImGui::CalcTextSize("##legacy").x, 0))) legacy_open_ = !legacy_open_;
        ImGui::PopStyleColor();
        tooltip("Fields earlier versions edited. They are kept in the file (older versions and imports still read them) but are no longer edited here.");
        if (legacy_open_) {
            ImGui::Indent(S(12));
            for (int i = 0; i < (int)(sizeof legacy / sizeof legacy[0]); ++i) {
                if (legacy[i].v->empty()) continue;
                ImGui::PushID(100 + i);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "%s:", legacy[i].label);
                ImGui::SameLine(); ImGui::TextUnformatted(legacy[i].v->c_str());
                if (legacy[i].note) { ImGui::SameLine(0, S(8)); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "(%s)", legacy[i].note); }
                ImGui::SameLine(0, S(8));
                if (ImGui::SmallButton("Clear")) { doc->checkpoint(std::string("Clear ") + legacy[i].label); legacy[i].v->clear(); doc->changed(ui::CH_INFO); }
                ImGui::PopID();
            }
            ImGui::Unindent(S(12));
        }
    }
    ImGui::Dummy(ImVec2(0, S(8)));
}

void NotesView::draw_notes(float w, float h) {
    section_title("Notes", "project-wide; printed with File \xE2\x96\xB8 Print");
    ImGuiID gid = ImGui::GetID("##pn");
    if (ImGui::GetActiveID() != gid) pnotes_ = doc->prj.project_notes;
    const float eh = std::max(S(90), h - ImGui::GetTextLineHeightWithSpacing() - S(6));
    if (input_multi("##pn", pnotes_, ImVec2(w, eh), ImGuiInputTextFlags_AllowTabInput)) {
        if (pnotes_ != doc->prj.project_notes) { if (!pcp_) { doc->checkpoint("Edit project notes"); pcp_ = true; } doc->prj.project_notes = pnotes_; doc->prj.dirty = true; }
    }
    if (ImGui::IsItemDeactivated() && pcp_) { pcp_ = false; doc->changed(ui::CH_INFO); }
    if (pnotes_.empty() && ImGui::GetActiveID() != gid) {
        ImVec2 ip = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(ip.x + S(8), ip.y + S(6)), th().text_faint, "Description, special handling, customer requirements...");
    }
}

// Layer notes (ModelPoint::notes): kept — they are the operator instructions printed per export layer and
// edited in the layer properties too — but folded away by default; open automatically when any layer has notes.
void NotesView::draw_layer_notes(float w) {
    const GTheme& t = th();
    auto& pts = doc->prj.model.points;
    if (pts.empty()) return;
    int with = 0;
    for (auto& m : pts) with += !m.notes.empty();
    if (!layer_init_) { layer_init_ = true; layer_open_ = with > 0; }
    if (layer_ < 0 || layer_ >= (int)pts.size()) layer_ = std::clamp(current_point(*doc), 0, (int)pts.size() - 1);
    const float isz = ImGui::GetFontSize();
    ImVec2 ap = ImGui::GetCursorScreenPos();
    std::string lbl = "      Layer notes  (" + std::to_string(with) + " of " + std::to_string(pts.size()) + " layers)##ln";
    ImGui::PushFont(g_font_bold);
    if (ImGui::Selectable(lbl.c_str(), false, 0, ImVec2(w, 0))) layer_open_ = !layer_open_;
    ImGui::PopFont();
    tooltip("Per export layer operator instructions (feeders, nozzles, inspection). Printed with that layer.");
    ImVec2 keep = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(ap.x + S(2), ap.y + (ImGui::GetTextLineHeight() - isz) * 0.5f));
    icon_image(layer_open_ ? ui::Icon::ArrowDown : ui::Icon::ArrowRight, isz);
    ImGui::SetCursorScreenPos(keep);
    if (!layer_open_) { ImGui::Dummy(ImVec2(0, S(6))); return; }
    ImGui::SetNextItemWidth(std::min(w, S(260)));
    if (ImGui::BeginCombo("##lnpt", pts[layer_].name.c_str())) {
        for (int i = 0; i < (int)pts.size(); ++i)
            if (ImGui::Selectable((pts[i].name + (pts[i].notes.empty() ? "" : "  \xE2\x80\xA2") + "##" + std::to_string(i)).c_str(), i == layer_)) layer_ = i;
        ImGui::EndCombo();
    }
    ModelPoint& m = pts[layer_];
    ImGuiID gid = ImGui::GetID("##lnotes");
    if (ImGui::GetActiveID() != gid) buf_ = m.notes;
    if (input_multi("##lnotes", buf_, ImVec2(w, S(90)), ImGuiInputTextFlags_AllowTabInput)) {
        if (buf_ != m.notes) { if (!cp_) { doc->checkpoint("Edit layer notes"); cp_ = true; } m.notes = buf_; doc->prj.dirty = true; }
    }
    if (ImGui::IsItemDeactivated() && cp_) { cp_ = false; doc->changed(ui::CH_INFO); }
    if (buf_.empty() && ImGui::GetActiveID() != gid) {
        ImVec2 ip = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(ip.x + S(8), ip.y + S(6)), t.text_faint, "Instructions for operators of this layer...");
    }
    ImGui::Dummy(ImVec2(0, S(6)));
}

// Revision notes: one dated line each (ECN, change, reason). Enter adds; double-click edits; x deletes.
void NotesView::draw_revision_notes(float w, float h) {
    const GTheme& t = th();
    auto& rn = doc->prj.revision_notes;
    const float y0 = ImGui::GetCursorPosY();
    section_title("Revision notes", rn.empty() ? "ECN, change, reason" : (std::to_string(rn.size()) + " note(s)").c_str());
    ImGui::SetNextItemWidth(w);
    if (input_str("##revadd", rev_buf_, "Add a revision note and press Enter...", ImGuiInputTextFlags_EnterReturnsTrue) && !rev_buf_.empty()) {
        doc->checkpoint("Add revision note");
        rn.push_back(U(today_str()) + "  " + rev_buf_);
        doc->changed(ui::CH_INFO);
        rev_buf_.clear();
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::BeginChild("##revs", ImVec2(w, std::max(S(60), h - (ImGui::GetCursorPosY() - y0))), ImGuiChildFlags_Borders, 0);
    if (rn.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "No revision notes. ECN merges add notes here automatically.");
    int del = -1;
    for (int i = (int)rn.size() - 1; i >= 0; --i) {
        ImGui::PushID(i);
        const float rw = ImGui::GetContentRegionAvail().x;
        if (edit_rev_ == i) {
            ImGui::SetNextItemWidth(rw);
            ImGui::SetKeyboardFocusHere();
            if (input_str("##ed", edit_buf_, nullptr, ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (edit_buf_ != rn[i]) { doc->checkpoint("Edit revision note"); rn[i] = edit_buf_; doc->changed(ui::CH_INFO); }
                edit_rev_ = -1;
            } else if (ImGui::IsItemDeactivated()) edit_rev_ = -1;
        } else {
            ImVec2 p = ImGui::GetCursorScreenPos();
            const float rh = ImGui::GetTextLineHeight() + S(6);
            ImGui::InvisibleButton("row", ImVec2(rw, rh));
            const bool hov = ImGui::IsItemHovered();
            if (hov && ImGui::IsMouseDoubleClicked(0)) { edit_rev_ = i; edit_buf_ = rn[i]; }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (hov) dl->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), t.raised, S(4));
            dl->PushClipRect(p, ImVec2(p.x + rw - S(24), p.y + rh), true);
            dl->AddText(ImVec2(p.x + S(4), p.y + S(3)), t.text, rn[i].c_str());
            dl->PopClipRect();
            if (hov) {
                tooltip((rn[i] + "\n\nDouble-click to edit").c_str());
                ImGui::SetCursorScreenPos(ImVec2(p.x + rw - S(22), p.y + S(1)));
                if (icon_button("del", ui::Icon::Delete, "Delete", false, true, rh - S(2))) del = i;
            }
        }
        ImGui::PopID();
    }
    if (del >= 0) { doc->checkpoint("Delete revision note"); rn.erase(rn.begin() + del); doc->changed(ui::CH_INFO); }
    ImGui::EndChild();
}

void NotesView::draw_history(float w, float h) {
    const GTheme& t = th();
    const float y0 = ImGui::GetCursorPosY();
    section_title("Revision history");
    ImGui::SameLine(0, S(6));
    ImGui::TextDisabled("(?)");
    tooltip("A history entry is added on every program export and when you press Checkpoint (not on Save).\n"
            "Each entry records time, author, a summary (counts / layer / file), your comment and, for exports, the file\n"
            "name and its FNV-1a 64 hash. The newest 10 entries also keep a restorable snapshot of the design:\n"
            "placements, packages, part numbers, panel images, board / panel outlines, export layers with their\n"
            "machine settings and layer notes, virtual origin and Project Info.\n"
            "Not in the snapshot: Gerber layers, import sources, board images, project notes, revision notes,\n"
            "production sides and the history itself. Restore is one undoable step and is itself recorded.");
    ImGui::SetNextItemWidth(w - S(130));
    bool enter = input_str("##cpc", cp_comment_, "Checkpoint comment (optional)...", ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, S(6));
    if (button("Checkpoint", ui::Icon::Check, true, ImVec2(S(124), 0)) || enter) {
        const Project& p = doc->prj;
        add_history(*doc, "checkpoint", std::to_string(p.comps.size()) + " placements, " + std::to_string(p.parts.size()) + " part numbers, " +
                    std::to_string(p.packages.size()) + " packages", cp_comment_, "", "");
        cp_comment_.clear();
    }
    tooltip("Record a restorable snapshot of the current design in the revision history.");
    auto& hs = doc->prj.history;
    int restore = -1;
    if (ImGui::BeginTable("##hist", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter,
                          ImVec2(w, std::max(S(120), h - (ImGui::GetCursorPosY() - y0) - S(4))))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Rev", ImGuiTableColumnFlags_WidthFixed, S(40));
        ImGui::TableSetupColumn("When / kind", ImGuiTableColumnFlags_WidthFixed, S(132));
        ImGui::TableSetupColumn("Summary");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(70));
        ImGui::TableHeadersRow();
        for (int i = (int)hs.size() - 1; i >= 0; --i) {
            const RevisionEntry& r = hs[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.rev.c_str());
            ImGui::TableNextColumn();
            std::string tm = r.time.size() >= 16 ? r.time.substr(0, 10) + " " + r.time.substr(11, 5) : r.time;
            ImGui::TextUnformatted(tm.c_str());
            const char* kn = r.kind == "export" ? "Export" : r.kind == "restore" ? "Restore" : "Checkpoint";
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(r.kind == "export" ? t.accent : t.text_dim), "%s", kn);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", r.summary.c_str());
            if (!r.comment.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "%s", r.comment.c_str());
            if (!r.program_file.empty()) { ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "%s", r.program_file.c_str()); tooltip(("FNV-1a 64: " + r.program_hash).c_str()); }
            if (!r.author.empty() && ImGui::IsItemHovered()) tooltip(("Author: " + r.author).c_str());
            ImGui::TableNextColumn();
            if (!r.snapshot.empty()) {
                if (ImGui::SmallButton("Restore")) restore = i;
                tooltip("Bring the design back to this revision (undoable).");
            } else ImGui::TextDisabled("-");
            ImGui::PopID();
        }
        if (hs.empty()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TableNextColumn(); ImGui::TableNextColumn(); ImGui::TextDisabled("No entries yet"); }
        ImGui::EndTable();
    }
    if (restore >= 0) {
        DocPtr d = doc;
        const size_t ix = (size_t)restore;
        defer([d, ix]() {
            if (ix >= d->prj.history.size()) return;
            const RevisionEntry& r = d->prj.history[ix];
            if (!ui::message(app::frame_hwnd(), L"Restore revision",
                             W("Restore " + r.rev + " (" + r.time + ")?\n\nPlacements, packages, part numbers, outlines, export layers and Project Info "
                               "return to that revision. Gerber layers and notes stay as they are. You can undo this."), true)) return;
            std::string err;
            if (!app::restore_checkpoint(*d, ix, err)) ui::warning(app::frame_hwnd(), L"Restore revision", W(err));
            else toast("Restored " + d->prj.history[ix].rev, ToastKind::Success, 3.f);
        });
    }
}

void NotesView::print(HDC dc, const RECT& page, bool) {
    if (!doc) return;
    const int dpx = GetDeviceCaps(dc, LOGPIXELSX), dpy = GetDeviceCaps(dc, LOGPIXELSY);
    const AssemblyInfo& a = doc->prj.info;
    auto nl = [](const std::string& s) { std::wstring o; for (wchar_t c : W(s)) { if (c == L'\n') o += L'\r'; o += c; } return o; };
    std::wstring t;
    auto kv = [&](const wchar_t* k, const std::string& v) { if (!v.empty()) t += std::wstring(k) + L": " + W(v) + L"\r\n"; };
    kv(L"Project", a.name); kv(L"Customer", a.customer); kv(L"Board ID", a.pcb_id); kv(L"BOM revision", a.bom_rev); kv(L"Author", a.originator);
    if (!doc->prj.project_notes.empty()) t += L"\r\nNotes\r\n" + nl(doc->prj.project_notes) + L"\r\n";
    for (auto& m : doc->prj.model.points) if (!m.notes.empty()) t += L"\r\n" + W(m.name) + L" notes\r\n" + nl(m.notes) + L"\r\n";
    if (!doc->prj.revision_notes.empty()) {
        t += L"\r\nRevision notes\r\n";
        for (auto& r : doc->prj.revision_notes) t += L"  \x2022 " + W(r) + L"\r\n";
    }
    std::wstring head = doc->display_name() + L" \x2014 Project Info";
    HFONT fh = CreateFontW(-MulDiv(12, dpy, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT fb = CreateFontW(-MulDiv(10, dpy, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, fh);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    const int mg = dpx / 2;
    RECT r{page.left + mg, page.top + mg, page.right - mg, page.bottom - mg};
    DrawTextW(dc, head.c_str(), -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    r.top += MulDiv(24, dpy, 72);
    SelectObject(dc, fb);
    DrawTextW(dc, t.c_str(), -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
    SelectObject(dc, old);
    DeleteObject(fh);
    DeleteObject(fb);
}

} // namespace

std::unique_ptr<View> make_visual_aid_view() { return std::make_unique<VisualAidView>(); }
std::unique_ptr<View> make_machine_view() { return std::make_unique<MachineView>(); }
std::unique_ptr<View> make_notes_view() { return std::make_unique<NotesView>(); }

} // namespace pcbp::gui

// ===========================================================================
// app::generate_program
// ===========================================================================
namespace pcbp::app {

// R21: board / circuit marks the program actually carries (writer's selector for JUKI; all job marks otherwise)
int written_mark_count(const MachineJob& j) {
    if (j.machine.rfind("juki.", 0) == 0) return (int)juki::select_marks(j).marks.size();
    return (int)j.fiducials.size();
}
// R21 (AUDIT_V20 P1): check generated bytes before they are published. false = not a readable program.
// `errors` = format check errors ("Machine program check: ..." wording, as check_job reports them).
bool check_program_bytes(const std::string& exporter_id, const std::vector<uint8_t>& b, std::vector<std::string>& errors, std::string& fail) {
    errors.clear();
    if (b.empty()) { fail = "the machine interface produced an empty program"; return false; }
    if (exporter_id.rfind("juki.", 0) != 0) return true;   // no reader for other formats: the byte read-back is the check
    std::vector<juki::HlcCheck> ck;
    if (!juki::verify_hlc_bytes(b, ck, fail)) return false;
    for (auto& c : ck) if (c.error) errors.push_back("Machine program check: " + c.msg);
    return true;
}
// errors in the generated bytes that the review did not list -> one more confirmation (never a hard block)
bool confirm_new_byte_errors(HWND owner, const std::vector<ExportIssue>& reviewed, const std::vector<std::string>& errors, const std::string& who) {
    std::vector<ExportIssue> fresh;
    for (auto& e : errors) {
        bool seen = false;
        for (auto& r : reviewed) seen |= r.sev == ExportIssue::Error && r.msg.size() >= e.size() && r.msg.compare(r.msg.size() - e.size(), e.size(), e) == 0;
        if (!seen) fresh.push_back({ExportIssue::Error, "", who + e});
    }
    return fresh.empty() || confirm_generate(owner, L"Generated program check", fresh);
}

bool generate_program(HWND owner, Doc& d, int point, std::wstring* out_path, std::string& err, const std::function<bool()>& after_review) {
    using namespace pcbp::gui;
    err.clear();
    if (point < 0 || point >= (int)d.prj.model.points.size()) { err = "Invalid process point."; return false; }
    ModelPoint& mp = d.prj.model.points[point];
    if (!mp.has_machine) {
        err = "Process point '" + mp.name + "' has no machine interface. Enable it on the Export page.";
        return false;
    }
    Exporter* ex = machine_exporter(mp.machine.exporter_id);
    if (!ex) {
        err = mp.machine.exporter_id.empty() ? "No machine type is selected for process point '" + mp.name + "'."
                                             : "Machine interface '" + mp.machine.exporter_id + "' is not installed.";
        return false;
    }
    // R21 (AUDIT_V20 P0): the one shared preflight + "Generate anyway?" review. Every caller of generate_program
    // (shell Generate command, Export view, plugin exporters) passes here; nothing hard-blocks.
    const ExportEntry entry = ex->info().builtin ? ExportEntry::GenerateCommand : ExportEntry::PluginExport;
    const std::vector<ExportIssue> reviewed = export_preflight(d.prj, ExportRequest{entry, point});
    if (!confirm_generate(owner, L"Generate Program", reviewed)) { err = kCancelled; return false; }
    if (after_review && !after_review()) { err = kCancelled; return false; }
    if (point >= (int)d.prj.model.points.size()) { err = "Invalid process point."; return false; }
    MachineJob job;
    if (!build_machine_job(d.prj, point, job, err)) return false;
    Options o = default_options(ex->info());
    for (auto& kv : mp.machine.options) if (kv.first.rfind("__", 0) != 0) o[kv.first] = kv.second;
    for (auto it = job.options.begin(); it != job.options.end();) it = it->first.rfind("__", 0) == 0 ? job.options.erase(it) : std::next(it);
    for (auto& kv : o) job.options[kv.first] = kv.second;
    job.machine = ex->info().id;

    std::vector<uint8_t> out;
    {
        // step 1 (build job) ran above on the UI thread; step 2 = machine interface on a worker
        bool xok = false;
        std::string xerr;
        const size_t npl = job.placements.size();
        bool fin = run_with_progress_wait("Generate Program", [&](Progress& p) {
            p.set(0.15f, ("Step 2 of 3: " + ex->info().name + " - " + std::to_string(npl) + " placements").c_str());
            try { xok = ex->export_job(job, o, out, xerr); } catch (const std::exception& e) { xerr = e.what(); }
            p.set(0.6f, "Program generated");
        }, ProgressOpts{true, false, 0.2f});
        if (!fin) { err = kCancelled; return false; }
        if (!xok) {
            err = xerr.empty() ? std::string("The machine interface failed to generate the program.") : xerr;
            err = ex->info().name + ": " + err;
            return false;
        }
    }
    // R21 (AUDIT_V20 P1): validate the generated bytes before the destination is touched
    {
        std::vector<std::string> berr; std::string bfail;
        if (!check_program_bytes(job.machine, out, berr, bfail)) {
            err = ex->info().name + ": " + bfail + ". Nothing was written; the previous program file is unchanged.";
            gui::status("Program NOT written: " + bfail, ToastKind::Error);
            return false;
        }
        if (!confirm_new_byte_errors(owner, reviewed, berr, "")) { err = kCancelled; return false; }
    }

    // ---- where to write: the saved folder when it exists on this PC, else ask
    std::wstring file = ui::W(mp.machine.target_file.empty() ? default_program_file(d.prj, point, ex) : mp.machine.target_file);
    std::wstring dir = ui::W(d.prj.model.export_dir);   // R19a: project-wide output folder
    std::wstring reason;
    bool ask = dir.empty();
    if (!dir.empty() && !dir_exists(dir)) {
        if (parent_exists(dir)) {
            // a missing last folder on this PC: offer to create it
            bool create = false, other = false;
            ui::Form f(L"Generate Program", 480);
            f.header(L"Export folder does not exist").icon((int)ui::Icon::Warning);
            f.label(dir);
            f.ok_text(L"Create folder");
            f.extra_button(L"Choose another...", [&other](ui::Form& ff) { other = true; ff.close(false); return false; });
            create = f.run(owner);
            if (!create && !other) { err = kCancelled; return false; }
            if (create) { if (!ensure_dirs(dir, err)) return false; }
            else { ask = true; reason = L"Saved folder does not exist: " + dir; }
        } else {
            ask = true;
            reason = L"Saved folder not found on this PC: " + dir;
        }
    }
    std::wstring path;
    if (!ask) {
        path = join_path(dir, file);
    } else {
        std::wstring start = fallback_export_dir(d);
        bool remember = true;
        ui::Form f(L"Save Machine Program", 560);
        f.header(L"Where should the program be saved?").icon((int)ui::Icon::Save);
        if (!reason.empty()) f.subtitle(reason);
        f.folder("dir", L"Folder", start);
        f.text("file", L"File name", file);
        f.check("remember", L"Use this folder for every export layer of this project", true);
        f.extra_button(L"Copy path", [](ui::Form& ff) { ImGui::SetClipboardText(U(join_path(ff.str("dir"), ff.str("file"))).c_str()); toast("Path copied", ToastKind::Success, 2.f); return false; });   // R12 §18
        f.extra_button(L"Open folder", [](ui::Form& ff) { std::wstring dd = ff.str("dir"); if (dir_exists(dd)) ShellExecuteW(nullptr, L"open", dd.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return false; });
        f.ok_text(L"Save");
        f.validate([](ui::Form& ff, std::wstring& e) {
            std::wstring dd = ff.str("dir"), fn = ff.str("file");
            if (dd.empty()) { e = L"Choose a folder."; return false; }
            if (fn.empty() || fn.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) { e = L"Enter a valid file name."; return false; }
            if (!dir_exists(dd)) {
                std::string ee;
                if (!ensure_dirs(dd, ee)) { e = ui::W(ee); return false; }
            }
            return true;
        });
        if (!f.run(owner)) { err = kCancelled; return false; }
        dir = f.str("dir");
        file = f.str("file");
        remember = f.checked("remember");
        if (file.find(L'.') == std::wstring::npos) file += L"." + ui::W(exporter_ext(ex));
        path = join_path(dir, file);
        settings().dir_programs = dir;
        if (remember) {
            d.checkpoint("Export folder");
            d.prj.model.export_dir = ui::U(dir);
            std::string fu = ui::U(file);
            mp.machine.target_file = fu == default_program_file(d.prj, point, ex) ? std::string() : fu;
            d.changed(ui::CH_MODEL);
        }
    }
    std::string werr, stage;
    bool wok = false;
    run_with_progress_wait("Generate Program", [&](Progress& p) {
        p.set(0.7f, "Step 3 of 3: writing and verifying program file...");
        wok = write_file_verified(ui::U(path), out.data(), out.size(), werr, &stage);   // temp -> read-back -> replace -> read-back
        p.set(1.f, "Done");
    }, ProgressOpts{true, false, 0.2f});
    std::vector<std::string> post_err; std::string post_fail;
    if (wok && stage.empty()) {   // post-write check on the file itself
        std::vector<uint8_t> back; std::string re;
        if (!read_file(ui::U(path), back, re)) { wok = false; stage = "verify"; werr = re; }
        else if (!check_program_bytes(job.machine, back, post_err, post_fail)) { wok = false; stage = "verify"; werr = post_fail; }
    }
    if (!wok) {
        const bool verify_failed = stage == "verify" || stage == "verify-temp";
        DWORD a = GetFileAttributesW(dir.c_str());
        if (verify_failed) err = "The program was NOT exported correctly: " + werr + "\n" + ui::U(path) +
                                 (stage == "verify" ? "\n\nThe file on disk cannot be trusted. Regenerate it, and do not load it on the machine." : "");
        else if (a == INVALID_FILE_ATTRIBUTES) err = "Cannot save the program: the folder\n" + ui::U(dir) + "\ndoes not exist.";
        else if (file_exists(path) && (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_READONLY)) err = "Cannot save the program: " + ui::U(path) + " is read-only.";
        else err = "Cannot save the program to\n" + ui::U(path) + "\n\nThe file may be open in another program (program editor, machine software) or the folder is write-protected.\nThe previous file is unchanged.\n(" + werr + ")";
        if (verify_failed) {   // record the failure, never a success
            ExportStatus es;
            es.path = path; es.bytes = (long long)out.size(); es.readback = false; es.placements = (int)job.placements.size();
            es.fids = written_mark_count(job); es.time = gui::iso_now(); es.hash = gui::fnv64_hex(out.data(), out.size()); es.format = ex->info().name;
            gui::export_status()[{&d, mp.name}] = es;
            add_history(d, "export", mp.name + ": export FAILED verification (" + stage + ")", "", ui::U(file), es.hash);
        }
        gui::status("Program export FAILED: " + ui::U(path), ToastKind::Error);
        toast("Program export failed" + std::string(verify_failed ? " (verification)" : ""), ToastKind::Error, 8.f);
        return false;
    }
    const bool verified = true;
    last_programs()[{&d, mp.name}] = path;
    {
        ExportStatus es;
        es.path = path; es.bytes = (long long)out.size(); es.readback = verified; es.placements = (int)job.placements.size();
        es.fids = written_mark_count(job); es.time = gui::iso_now(); es.hash = gui::fnv64_hex(out.data(), out.size()); es.format = ex->info().name;
        gui::export_status()[{&d, mp.name}] = es;
        add_history(d, "export", mp.name + ": " + std::to_string(job.placements.size()) + " placements, " + std::to_string(es.fids) +
                    " fiducial marks, " + ex->info().name, "", ui::U(file), es.hash);
    }
    if (out_path) *out_path = path;
    std::string st = "Program written (read-back matches and passes the program checks; not yet verified on a machine): " + ui::U(path) + " (" + std::to_string(job.placements.size()) + " placements)";
    if (!job.warnings.empty()) {
        st += " \xE2\x80\x94 " + std::to_string(job.warnings.size()) + " warning(s): " + job.warnings[0];
        for (size_t i = 1; i < job.warnings.size() && i < 3; ++i) st += "; " + job.warnings[i];
    }
    gui::status(st, job.warnings.empty() ? ToastKind::Success : ToastKind::Warning);
    {
        char kb[48]; std::snprintf(kb, sizeof kb, "%.1f KB", out.size() / 1024.0);
        std::wstring fdir = dir;
        std::string msg = "Program written: " + ui::U(file) + " (" + kb + ", " + std::to_string(job.placements.size()) + " placements)";
        if (!job.warnings.empty()) msg += "\n" + std::to_string(job.warnings.size()) + " warning(s): " + job.warnings[0];
        toast(msg, job.warnings.empty() ? ToastKind::Success : ToastKind::Warning, 8.f, "Open folder",
              [fdir] { ShellExecuteW(app::frame_hwnd(), L"open", fdir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
    }
    return true;
}

// ---------------------------------------------------------------------------
// Export all manufacturing sides: "<Job> [Side N].<ext>" + "<Job> [Side N] - placements.csv"
// per side, plus "<Job> - sides report.html/.txt". R21: the shared preflight + one review first (nothing blocks);
// each program's bytes are checked before writing and read back after; a failed check is a failed side.
bool export_all_sides(HWND owner, Doc& d, int link, std::string& err) {
    using namespace pcbp::gui;
    err.clear();
    Project& p = d.prj;
    const MfgSides mfg = link_sides(p, link);   // R18d: the linked pair (upper layer = Side 1)
    if (!mfg.configured) { err = "Link " + std::to_string(link) + " does not have exactly two export layers."; return false; }
    const auto mem = link_members(p, link);
    const auto iss = export_preflight(p, ExportRequest{ExportEntry::ExportAllSides, mem[0]});
    p.mfg.last_validation = validation_summary(validate_sides(p, mfg));
    if (!confirm_generate(owner, L"Generate linked pair", iss)) { err = kCancelled; return false; }   // R18d: never blocks
    // folder: first side's saved folder, else ask once
    std::wstring dir;
    {
        dir = ui::W(p.model.export_dir);   // R19a: project-wide output folder
        if (dir.empty() || !dir_exists(dir)) {
            ui::Form f(L"Export all sides", 560);
            f.header(L"Where should the side programs be saved?").icon((int)ui::Icon::Save);
            f.folder("dir", L"Folder", dir.empty() || !dir_exists(dir) ? fallback_export_dir(d) : dir);
            f.ok_text(L"Export");
            if (!f.run(owner)) { err = kCancelled; return false; }
            dir = f.str("dir");
            std::string e2;
            if (!dir_exists(dir) && !ensure_dirs(dir, e2)) { err = e2; return false; }
            d.checkpoint("Export folder");
            p.model.export_dir = ui::U(dir);
        }
    }
    std::vector<SideExportResult> res;
    bool all_ok = true;
    for (const MfgSide& sd : mfg.sides) {
        SideExportResult r;
        r.side = sd;
        const int pi = side_point_index(p, sd);
        const ModelPoint& mp = p.model.points[pi];
        r.point = mp.name;
        r.orientation = mp.machine.orientation; r.flipped = mp.machine.flipped;
        const Vec2 org = mp.machine.has_origin ? mp.machine.origin : p.virtual_origin;
        r.origin_x = org.x; r.origin_y = org.y;
        Exporter* ex = machine_exporter(mp.machine.exporter_id);
        MachineJob job;
        std::vector<uint8_t> out;
        if (!ex) r.err = "no machine interface";
        else if (build_side_job(p, sd, job, r.err)) {
            r.exporter = ex->info().name;
            Options o = default_options(ex->info());
            for (auto& kv : mp.machine.options) if (kv.first.rfind("__", 0) != 0) o[kv.first] = kv.second;
            for (auto it = job.options.begin(); it != job.options.end();) it = it->first.rfind("__", 0) == 0 ? job.options.erase(it) : std::next(it);
            for (auto& kv : o) job.options[kv.first] = kv.second;
            if (job.options["juki.comment"].empty()) job.options["juki.comment"] = mfg_label(sd);   // HLC Lineinfo comment (real field, 32 chars)
            apply_desc_mode(p, job, job.options["sides.desc_mode"]);
            job.machine = ex->info().id;
            std::string xerr;
            bool ok = false;
            try { ok = ex->export_job(job, o, out, xerr); } catch (const std::exception& e) { xerr = e.what(); }
            std::vector<std::string> berr; std::string bfail;
            if (!ok) r.err = xerr.empty() ? "export failed" : xerr;
            else if (!check_program_bytes(job.machine, out, berr, bfail)) { ok = false; r.err = bfail + " (nothing written; previous file unchanged)"; }
            else if (!confirm_new_byte_errors(owner, iss, berr, mfg_label(sd) + ": ")) { ok = false; r.err = "cancelled at the generated program check"; }
            if (ok) {
                // R18d: each layer keeps its own file name (<Project Name>_<layer name>, or its Job name)
                r.program_file = mp.machine.target_file.empty() ? default_program_file(p, pi, ex) : mp.machine.target_file;
                const size_t dot = r.program_file.rfind('.');
                const std::string base = dot == std::string::npos ? r.program_file : r.program_file.substr(0, dot);
                r.csv_file = base + " - placements.csv";
                std::string we;
                std::string stage;
                if (!write_file_verified(ui::U(join_path(dir, ui::W(r.program_file))), out.data(), out.size(), we, &stage))
                    r.err = (stage == "verify" || stage == "verify-temp" ? "verification FAILED " : "write ") + r.program_file + ": " + we;
                else {
                    std::vector<uint8_t> back; std::string re; std::vector<std::string> pe;
                    if (!read_file(ui::U(join_path(dir, ui::W(r.program_file))), back, re) || !check_program_bytes(job.machine, back, pe, re))
                        r.err = "verification FAILED " + r.program_file + ": " + re;
                }
                if (r.err.empty()) {
                    const std::string c = placements_csv(p, sd, job);
                    if (!write_file(ui::U(join_path(dir, ui::W(r.csv_file))), c.data(), c.size(), we)) r.warnings.push_back("CSV not written: " + we);
                    r.ok = true;
                    last_programs()[{&d, mp.name}] = join_path(dir, ui::W(r.program_file));
                }
                r.placements = (int)job.placements.size(); r.parts = (int)job.parts.size();
                r.fiducials = written_mark_count(job); r.circuits = (int)job.circuits.size();
                for (auto& w : job.warnings) r.warnings.push_back(w);
            }
        }
        all_ok &= r.ok;
        res.push_back(r);
    }
    const std::string rb = sanitize_file_part(job_base_name(p)) + " - sides report";
    std::string we;
    const std::string html = sides_report_html(p, res), txt = sides_report_txt(p, res);   // report keeps the Side 1 / Side 2 run order
    write_file(ui::U(join_path(dir, ui::W(rb + ".html"))), html.data(), html.size(), we);
    write_file(ui::U(join_path(dir, ui::W(rb + ".txt"))), txt.data(), txt.size(), we);
    d.changed(ui::CH_MODEL);
    std::string msg;
    for (auto& r : res) msg += mfg_label(r.side) + ": " + (r.ok ? r.program_file + " (" + std::to_string(r.placements) + " placements)" : "FAILED - " + r.err) + "\n";
    std::wstring fdir = dir;
    toast(msg + "Report: " + rb + ".html", all_ok ? ToastKind::Success : ToastKind::Error, 10.f, "Open folder",
          [fdir] { ShellExecuteW(app::frame_hwnd(), L"open", fdir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
    gui::status(all_ok ? "All sides exported to " + ui::U(dir) : "Some sides FAILED - see report", all_ok ? ToastKind::Success : ToastKind::Error);
    if (!all_ok) err = msg;
    return all_ok;
}

} // namespace pcbp::app

