// ============================================================================
// plugin_dll_win.cpp — loads external plugin DLLs (sdk/pcbp_plugin.h C ABI)
// and wraps every descriptor in an Importer / Exporter adapter.
//
// Data crosses the boundary as UTF-8 JSON:
//   importer -> project JSON (project_from_json; MERGE results are merged into
//               the open project: components by ref, packages / parts by name,
//               layers appended, board frame replaced when present)
//   exporter <- machine_job_to_json(job) or project_to_json(project)
// Options are passed as a JSON object (bool -> true/false, int/double -> number,
// everything else -> string).
//
// Every call into a plugin is guarded with SEH under MSVC so that an access
// violation inside a plugin is reported instead of terminating the app.
// DLLs are never unloaded (descriptors and callbacks live for the process).
// ============================================================================
#include "../app/app.h"
#include "../core/fileio.h"
#include "../core/json.h"
#include "../core/project_io.h"
#include "pcbp_plugin.h"
#include "plugin.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cctype>

namespace pcbp::app {
namespace {

// ---------------------------------------------------------------------------
// guarded calls (no C++ objects with destructors inside __try frames)
// ---------------------------------------------------------------------------
#ifdef _MSC_VER
#define PCBP_TRY __try
#define PCBP_EXCEPT(flag) __except (EXCEPTION_EXECUTE_HANDLER) { *(flag) = 1; }
#else
#define PCBP_TRY
#define PCBP_EXCEPT(flag) (void)(flag);
#endif

int g_enumerate(pcbp_plugin_enumerate_fn fn, const pcbp_plugin_desc** list, uint32_t* count, int* crashed) {
    int r = 0;
    PCBP_TRY { r = fn(PCBP_PLUGIN_API_VERSION, list, count); }
    PCBP_EXCEPT(crashed)
    return r;
}

int g_sniff(const pcbp_plugin_desc* d, const char* path, const uint8_t* head, size_t n, int* crashed) {
    int r = 0;
    PCBP_TRY { r = d->sniff(path, head, n); }
    PCBP_EXCEPT(crashed)
    return r;
}

int g_import(const pcbp_plugin_desc* d, const pcbp_host* h, const char* path, const uint8_t* data, size_t n,
             const char* opts, char** out, char** err, int* crashed) {
    int r = 0;
    PCBP_TRY { r = d->import_file(h, path, data, n, opts, out, err); }
    PCBP_EXCEPT(crashed)
    return r;
}

int g_export(const pcbp_plugin_desc* d, const pcbp_host* h, const char* in, const char* opts,
             uint8_t** out, size_t* size, char** err, int* crashed) {
    int r = 0;
    PCBP_TRY { r = d->export_file(h, in, opts, out, size, err); }
    PCBP_EXCEPT(crashed)
    return r;
}

void g_free(const pcbp_plugin_desc* d, void* p) {
    if (!p || !d->free_mem) return;      // no free_mem: leak rather than free across CRTs
    int crashed = 0;
    PCBP_TRY { d->free_mem(p); }
    PCBP_EXCEPT(&crashed)
}

// ---------------------------------------------------------------------------
// host callbacks
// ---------------------------------------------------------------------------
struct HostCtx {
    std::vector<std::string>* messages = nullptr;
    std::string prefix;
};

void PCBP_CALL host_log(void* ctx, int level, const char* msg) {
    auto* h = static_cast<HostCtx*>(ctx);
    if (!h || !msg) return;
    std::string m = level == PCBP_LOG_ERROR ? "error: " : level == PCBP_LOG_WARN ? "warning: " : "";
    m += msg;
    if (h->messages) h->messages->push_back(m);
    OutputDebugStringW(utf8_to_wide("[" + h->prefix + "] " + m + "\n").c_str());
}

void PCBP_CALL host_progress(void* ctx, double fraction, const char* msg) {
    auto* h = static_cast<HostCtx*>(ctx);
    std::wstring s = utf8_to_wide(h ? h->prefix : std::string());
    if (msg && *msg) s += L": " + utf8_to_wide(msg);
    if (fraction >= 0 && fraction <= 1) s += L" (" + std::to_wstring((int)(fraction * 100 + 0.5)) + L"%)";
    set_status(s);
}

pcbp_host make_host(HostCtx& ctx) {
    pcbp_host h{};
    h.api_version = PCBP_PLUGIN_API_VERSION;
    h.ctx = &ctx;
    h.log = host_log;
    h.progress = host_progress;
    return h;
}

std::string take_str(const pcbp_plugin_desc* d, char*& p) {
    std::string s = p ? std::string(p) : std::string();
    g_free(d, p);
    p = nullptr;
    return s;
}

// ---------------------------------------------------------------------------
// descriptor -> PluginInfo
// ---------------------------------------------------------------------------
std::vector<std::string> parse_filter(const char* f) {
    std::vector<std::string> out;
    if (!f) return out;
    std::string cur;
    auto flush = [&] {
        std::string e = cur;
        const size_t dot = e.find_last_of('.');
        if (dot != std::string::npos) e = e.substr(dot + 1);
        for (auto& c : e) c = (char)tolower((unsigned char)c);
        if (!e.empty() && e != "*" && std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
        cur.clear();
    };
    for (const char* p = f; *p; ++p) {
        if (*p == ';' || *p == ',' || *p == ' ') flush();
        else cur += *p;
    }
    flush();
    return out;
}

std::string json_scalar_text(const json::Value& v) {
    switch (v.type()) {
    case json::Type::Bool: return v.b() ? "1" : "0";
    case json::Type::Number: {
        std::string s = v.dump();
        return s;
    }
    case json::Type::String: return v.str();
    default: return {};
    }
}

std::vector<OptionDef> parse_options(const char* schema, std::vector<std::string>& log) {
    std::vector<OptionDef> out;
    if (!schema || !*schema) return out;
    json::Value v;
    std::string err;
    if (!json::Value::parse(schema, v, err) || !v.is_array()) {
        log.push_back("  options_schema_json ignored: " + (err.empty() ? std::string("not an array") : err));
        return out;
    }
    for (auto& o : v.items()) {
        if (!o.is_object() || o["key"].str().empty()) continue;
        OptionDef d;
        d.key = o["key"].str();
        d.label = o["label"].str(d.key);
        d.tooltip = o["tooltip"].str();
        const std::string t = o["type"].str("string");
        d.type = t == "bool" ? OptionDef::Type::Bool : t == "int" ? OptionDef::Type::Int
               : (t == "double" || t == "number") ? OptionDef::Type::Double : t == "enum" ? OptionDef::Type::Enum
               : t == "path" ? OptionDef::Type::Path : OptionDef::Type::String;
        for (auto& c : o["choices"].items()) d.choices.push_back(json_scalar_text(c));
        d.def = json_scalar_text(o["default"]);
        if (d.type == OptionDef::Type::Bool && d.def.empty()) d.def = "0";
        if (d.type == OptionDef::Type::Enum && d.def.empty() && !d.choices.empty()) d.def = d.choices[0];
        out.push_back(std::move(d));
    }
    return out;
}

std::string options_json(const PluginInfo& info, const Options& o) {
    json::Value v = json::Value::object();
    Options full = default_options(info);
    for (auto& kv : o) full[kv.first] = kv.second;
    for (auto& kv : full) {
        const OptionDef* d = nullptr;
        for (auto& x : info.options) if (x.key == kv.first) d = &x;
        if (d && d->type == OptionDef::Type::Bool) v.set(kv.first, opt_bool(full, kv.first));
        else if (d && (d->type == OptionDef::Type::Int || d->type == OptionDef::Type::Double) && !kv.second.empty())
            v.set(kv.first, opt_num(full, kv.first));
        else v.set(kv.first, kv.second);
    }
    return v.dump();
}

// Merge a (partial) imported project into `dst`.
void merge_project(const Project& src, const json::Value& raw, Project& dst, ImportReport& rep) {
    for (auto& kv : src.packages) {
        if (!dst.packages.count(kv.first)) ++rep.packages_added;
        dst.packages[kv.first] = kv.second;
    }
    for (auto& kv : src.parts) {
        if (!dst.parts.count(kv.first)) ++rep.parts_added;
        dst.parts[kv.first] = kv.second;
    }
    for (const Component& c : src.comps) {
        Component* ex = c.ref.empty() ? nullptr : dst.find_ref(c.ref);
        if (ex) {
            const uint32_t id = ex->id;
            const int pt = ex->assigned_point;
            *ex = c;
            ex->id = id;
            if (c.assigned_point < 0) ex->assigned_point = pt;
            ++rep.components_updated;
        } else {
            Component n = c;
            n.id = 0;
            dst.add_component(n);
            ++rep.components_added;
        }
    }
    for (auto& l : src.layers) { dst.layers.push_back(l); ++rep.layers_added; }
    if (!src.board_frame.empty()) { dst.board_frame = src.board_frame; dst.board_outline_src = OutlineSource::Imported; }
    if (!src.panel_frame.empty()) { dst.panel_frame = src.panel_frame; dst.panel_outline_src = OutlineSource::Imported; }
    if (raw.has("assembly")) {
        auto keep = [](std::string& d, const std::string& s) { if (!s.empty()) d = s; };
        const AssemblyInfo& a = src.info;
        AssemblyInfo& b = dst.info;
        keep(b.name, a.name); keep(b.number, a.number); keep(b.description, a.description);
        keep(b.originator, a.originator); keep(b.bom_number, a.bom_number); keep(b.assembly_rev, a.assembly_rev);
        keep(b.bom_rev, a.bom_rev); keep(b.manf_rev, a.manf_rev); keep(b.standard, a.standard);
        keep(b.customer, a.customer); keep(b.url, a.url);
    }
    if (raw.has("origin")) dst.virtual_origin = src.virtual_origin;
    dst.dirty = true;
}

// ---------------------------------------------------------------------------
// adapters
// ---------------------------------------------------------------------------
class DllImporter final : public Importer {
public:
    DllImporter(const pcbp_plugin_desc* d, PluginInfo info) : d_(d), info_(std::move(info)) {}
    const PluginInfo& info() const override { return info_; }

    int sniff(const std::string& path, const std::string& head) const override {
        if (!d_->sniff) return Importer::sniff(path, head);
        int crashed = 0;
        int r = g_sniff(d_, path.c_str(), (const uint8_t*)head.data(), head.size(), &crashed);
        if (crashed) return 0;
        return std::clamp(r, 0, 100);
    }

    bool run(const std::string& path, const Options& opts, Project& prj, ImportReport& rep,
             std::string& err) override {
        std::vector<uint8_t> data;
        if (!read_file(path, data, err)) return false;
        HostCtx ctx{&rep.messages, info_.id};
        pcbp_host host = make_host(ctx);
        const std::string oj = options_json(info_, opts);
        char* out = nullptr;
        char* perr = nullptr;
        int crashed = 0;
        const int ok = g_import(d_, &host, path.c_str(), data.data(), data.size(), oj.c_str(), &out, &perr, &crashed);
        if (crashed) {
            err = "plugin '" + info_.id + "' crashed during import (" + info_.module_path + ")";
            return false;
        }
        const std::string js = take_str(d_, out);
        const std::string es = take_str(d_, perr);
        if (!ok) { err = es.empty() ? "plugin '" + info_.id + "' failed" : es; return false; }
        json::Value v;
        if (!json::Value::parse(js, v, err)) { err = "plugin returned invalid JSON: " + err; return false; }
        if (!v.is_object()) { err = "plugin returned no JSON object"; return false; }
        if (!v.has("format")) v.set("format", "PCBPlotter.Project");
        if (!v.has("version")) v.set("version", kProjectFormatVersion);
        if (info_.import_mode == ImportMode::Project) {
            if (!project_from_json(v, prj, err)) return false;
            rep.components_added = (int)prj.comps.size();
            rep.packages_added = (int)prj.packages.size();
            rep.parts_added = (int)prj.parts.size();
            rep.layers_added = (int)prj.layers.size();
            prj.dirty = true;
            return true;
        }
        Project tmp;
        if (!project_from_json(v, tmp, err)) return false;
        merge_project(tmp, v, prj, rep);
        return true;
    }

private:
    const pcbp_plugin_desc* d_;
    PluginInfo info_;
};

class DllExporter final : public Exporter {
public:
    DllExporter(const pcbp_plugin_desc* d, PluginInfo info) : d_(d), info_(std::move(info)) {}
    const PluginInfo& info() const override { return info_; }

    bool export_job(const MachineJob& job, const Options& o, std::vector<uint8_t>& out, std::string& err) override {
        if (info_.export_input != ExportInput::MachineJob) return Exporter::export_job(job, o, out, err);
        return call(machine_job_to_json(job), o, out, err);
    }
    bool export_project(const Project& p, const Options& o, std::vector<uint8_t>& out, std::string& err) override {
        if (info_.export_input != ExportInput::Project) return Exporter::export_project(p, o, out, err);
        return call(project_to_json(p).dump(), o, out, err);
    }

private:
    bool call(const std::string& input, const Options& o, std::vector<uint8_t>& out, std::string& err) {
        std::vector<std::string> msgs;
        HostCtx ctx{&msgs, info_.id};
        pcbp_host host = make_host(ctx);
        const std::string oj = options_json(info_, o);
        uint8_t* data = nullptr;
        size_t size = 0;
        char* perr = nullptr;
        int crashed = 0;
        const int ok = g_export(d_, &host, input.c_str(), oj.c_str(), &data, &size, &perr, &crashed);
        if (crashed) {
            err = "plugin '" + info_.id + "' crashed during export (" + info_.module_path + ")";
            return false;
        }
        const std::string es = take_str(d_, perr);
        if (!ok) {
            g_free(d_, data);
            err = es.empty() ? "plugin '" + info_.id + "' failed" : es;
            for (auto& m : msgs) err += "\n" + m;
            return false;
        }
        out.assign(data, data + (data ? size : 0));
        g_free(d_, data);
        return true;
    }

    const pcbp_plugin_desc* d_;
    PluginInfo info_;
};

bool is_disabled(const std::string& id) {
    for (auto& d : settings().disabled_plugins) if (d == id) return true;
    return false;
}

void load_one(PluginRegistry& r, const std::wstring& file) {
    const std::string path = wide_to_utf8(file);
    const std::string name = path.substr(path.find_last_of("\\/") + 1);
    // Don't let a DLL's dependencies pop up "missing DLL" error boxes.
    const UINT old_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    HMODULE h = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    SetErrorMode(old_mode);
    if (!h) {
        r.load_log.push_back(name + ": LoadLibrary failed (error " + std::to_string(GetLastError()) + ")");
        return;
    }
    auto fn = reinterpret_cast<pcbp_plugin_enumerate_fn>(reinterpret_cast<void*>(GetProcAddress(h, "pcbp_plugin_enumerate")));
    if (!fn) {
        r.load_log.push_back(name + ": not a PCB Plotter plugin (no pcbp_plugin_enumerate export)");
        FreeLibrary(h);
        return;
    }
    const pcbp_plugin_desc* list = nullptr;
    uint32_t count = 0;
    int crashed = 0;
    const int ok = g_enumerate(fn, &list, &count, &crashed);
    if (crashed || !ok || !list) {
        r.load_log.push_back(name + (crashed ? ": crashed in pcbp_plugin_enumerate" : ": pcbp_plugin_enumerate returned 0"));
        if (!crashed) FreeLibrary(h);       // a crashed module may hold broken state; keep it mapped
        return;
    }
    if (count > 256) { r.load_log.push_back(name + ": implausible descriptor count " + std::to_string(count)); return; }
    int added = 0;
    std::string ids;
    // Descriptors form an array whose stride is the plugin's sizeof(desc)
    // (struct_size of the first entry; newer SDKs may append fields).
    const size_t stride = std::max<size_t>(list->struct_size, sizeof(pcbp_plugin_desc));
    for (uint32_t i = 0; i < count; ++i) {
        const pcbp_plugin_desc* d =
            reinterpret_cast<const pcbp_plugin_desc*>(reinterpret_cast<const uint8_t*>(list) + i * stride);
        std::vector<std::string> log;
        auto reject = [&](const std::string& why) {
            r.load_log.push_back(name + " [" + std::to_string(i) + "]: " + why);
        };
        if (d->struct_size < sizeof(pcbp_plugin_desc)) { reject("struct_size " + std::to_string(d->struct_size) + " too small"); continue; }
        if (d->api_version != PCBP_PLUGIN_API_VERSION) { reject("unsupported api_version " + std::to_string(d->api_version)); continue; }
        if (!d->id || !*d->id) { reject("missing id"); continue; }
        const std::string id = d->id;
        if (is_disabled(id)) { r.load_log.push_back(name + ": " + id + " disabled in settings"); continue; }
        if ((d->kind == PCBP_KIND_IMPORTER && r.importer(id) && r.importer(id)->info().builtin) ||
            (d->kind == PCBP_KIND_EXPORTER && r.exporter(id) && r.exporter(id)->info().builtin)) {
            reject("id '" + id + "' collides with a built-in plugin; use a vendor prefix");
            continue;
        }
        PluginInfo info;
        info.id = id;
        info.name = d->name && *d->name ? d->name : id;
        info.vendor = d->vendor ? d->vendor : "";
        info.extensions = parse_filter(d->file_filter);
        info.default_ext = d->default_ext ? d->default_ext : (info.extensions.empty() ? "" : info.extensions[0]);
        for (auto& c : info.default_ext) c = (char)tolower((unsigned char)c);
        if (!info.default_ext.empty() && info.default_ext[0] == '.') info.default_ext.erase(0, 1);
        info.options = parse_options(d->options_schema_json, log);
        info.builtin = false;
        info.module_path = path;
        if (d->kind == PCBP_KIND_IMPORTER) {
            if (!d->import_file) { reject(id + ": importer without import_file"); continue; }
            info.kind = PluginKind::Importer;
            info.import_mode = d->import_mode == PCBP_IMPORT_PROJECT ? ImportMode::Project : ImportMode::Merge;
            info.category = info.import_mode == ImportMode::Project ? "Project" : "Plugin";
            r.add(std::make_unique<DllImporter>(d, std::move(info)));
        } else if (d->kind == PCBP_KIND_EXPORTER) {
            if (!d->export_file) { reject(id + ": exporter without export_file"); continue; }
            if (d->export_input != PCBP_EXPORT_MACHINE_JOB && d->export_input != PCBP_EXPORT_PROJECT) {
                reject(id + ": unknown export_input " + std::to_string(d->export_input));
                continue;
            }
            info.kind = PluginKind::Exporter;
            info.export_input = d->export_input == PCBP_EXPORT_MACHINE_JOB ? ExportInput::MachineJob : ExportInput::Project;
            info.category = info.export_input == ExportInput::MachineJob ? "Machine" : "Plugin";
            r.add(std::make_unique<DllExporter>(d, std::move(info)));
        } else {
            reject(id + ": unknown kind " + std::to_string(d->kind));
            continue;
        }
        if (!d->free_mem) log.push_back("  " + id + ": no free_mem; returned buffers will leak");
        for (auto& l : log) r.load_log.push_back(name + l);
        ids += (added++ ? ", " : "") + id;
    }
    r.load_log.push_back(name + ": " + std::to_string(added) + " of " + std::to_string(count) + " plugins loaded" +
                         (ids.empty() ? "" : " (" + ids + ")"));
    // The module stays loaded for the process lifetime (descriptors point into it).
}

} // namespace

void load_plugins_dir(PluginRegistry& r, const std::wstring& dir) {
    if (dir.empty()) return;
    std::wstring base = dir;
    if (base.back() != L'\\' && base.back() != L'/') base += L'\\';
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((base + L"*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND)
            r.load_log.push_back("plugins dir " + wide_to_utf8(dir) + ": error " + std::to_string(e));
        return;
    }
    std::vector<std::wstring> files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(base + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(files.begin(), files.end());
    for (auto& f : files) load_one(r, f);
}

} // namespace pcbp::app
