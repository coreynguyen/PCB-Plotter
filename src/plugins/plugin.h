#pragma once
// ============================================================================
// plugin.h — host-side plugin model (portable).
//
// Built-in importers/exporters implement Importer / Exporter directly.
// External DLL plugins (sdk/pcbp_plugin.h C ABI) are wrapped by adapters
// created in plugin_dll_win.cpp (Windows only) and registered the same way.
// ============================================================================
#include "../core/machine_job.h"
#include "../core/project.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pcbp {

struct OptionDef {
    std::string key, label;
    enum class Type { Bool, Int, Double, String, Enum, Path } type = Type::String;
    std::string def;                       // default as text ("1"/"0" for bools)
    std::vector<std::string> choices;      // Enum
    std::string tooltip;
};
using Options = std::map<std::string, std::string>;

inline std::string opt(const Options& o, const std::string& k, const std::string& def = {}) {
    auto it = o.find(k); return it == o.end() ? def : it->second;
}
inline bool opt_bool(const Options& o, const std::string& k, bool def = false) {
    auto it = o.find(k); if (it == o.end() || it->second.empty()) return def;
    char c = it->second[0]; return c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y';
}
double opt_num(const Options& o, const std::string& k, double def = 0);

enum class PluginKind { Importer, Exporter };
enum class ImportMode { Merge, Project };
enum class ExportInput { MachineJob, Project };

struct PluginInfo {
    std::string id, name, vendor;
    PluginKind kind = PluginKind::Importer;
    std::vector<std::string> extensions;   // lower-case, no dot
    std::string default_ext;
    std::vector<OptionDef> options;
    ImportMode import_mode = ImportMode::Merge;
    ExportInput export_input = ExportInput::MachineJob;
    std::string category;                  // menu grouping: "CAD", "Gerber", "BOM", "Machine", "Report"
    bool builtin = true;
    std::string module_path;               // DLL path for external plugins
};

struct ImportReport {
    std::vector<std::string> messages;
    int components_added = 0, components_updated = 0, packages_added = 0, parts_added = 0, layers_added = 0;
    std::string summary() const;
};

class Importer {
public:
    virtual ~Importer() = default;
    virtual const PluginInfo& info() const = 0;
    // 0..100: how likely `head` (first bytes) is this format; default = extension match
    virtual int sniff(const std::string& path, const std::string& head) const;
    // Merge mode: modify `prj` in place. Project mode: `prj` is a fresh Project to fill.
    virtual bool run(const std::string& path, const Options& opts, Project& prj,
                     ImportReport& rep, std::string& err) = 0;
};

class Exporter {
public:
    virtual ~Exporter() = default;
    virtual const PluginInfo& info() const = 0;
    virtual bool export_job(const MachineJob&, const Options&, std::vector<uint8_t>& out, std::string& err) {
        (void)out; err = "exporter does not accept machine jobs"; return false;
    }
    virtual bool export_project(const Project&, const Options&, std::vector<uint8_t>& out, std::string& err) {
        (void)out; err = "exporter does not accept projects"; return false;
    }
};

class PluginRegistry {
public:
    static PluginRegistry& get();
    void add(std::unique_ptr<Importer> p);
    void add(std::unique_ptr<Exporter> p);
    const std::vector<std::unique_ptr<Importer>>& importers() const { return imp_; }
    const std::vector<std::unique_ptr<Exporter>>& exporters() const { return exp_; }
    Importer* importer(const std::string& id) const;
    Exporter* exporter(const std::string& id) const;
    // best importer for a file (extension + sniff)
    Importer* detect(const std::string& path) const;
    std::vector<std::string> load_log;     // DLL load diagnostics
private:
    std::vector<std::unique_ptr<Importer>> imp_;
    std::vector<std::unique_ptr<Exporter>> exp_;
};

void register_builtin_plugins(PluginRegistry& r);

// Default options for a plugin (from its OptionDef defaults)
Options default_options(const PluginInfo& info);

// MachineJob <-> JSON (C ABI payload for machine exporters)
std::string machine_job_to_json(const MachineJob& job);

} // namespace pcbp
