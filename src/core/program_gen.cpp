// ============================================================================
// program_gen.cpp — see program_gen.h. Logic moved verbatim from
// gui/output_views.cpp (generate_program / export_all_sides, R18d/R19a/R21);
// the GUI now calls these and keeps only its dialogs and status bookkeeping.
// ============================================================================
#include "program_gen.h"
#include "fileio.h"
#include "../io/juki_writer.h"
#include <cstring>
#include <exception>

namespace pcbp::progen {

std::string sanitize_filename(std::string s) {
    for (char& c : s) if (std::strchr("<>:\"/\\|?*", c) || (unsigned char)c < 32) c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? std::string("program") : s;
}

// R18d: no machine type = no extension (was a silent ".txt" placeholder)
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

std::string program_file_for(const Project& p, int point, const Exporter* e) {
    if (point >= 0 && point < (int)p.model.points.size() && !p.model.points[point].machine.target_file.empty())
        return p.model.points[point].machine.target_file;
    return default_program_file(p, point, e);
}

bool is_machine_exporter(const Exporter* e) { return e && e->info().export_input == ExportInput::MachineJob; }

Exporter* machine_exporter(PluginRegistry& reg, const std::string& id) {
    if (id.empty()) return nullptr;
    Exporter* e = reg.exporter(id);
    return is_machine_exporter(e) ? e : nullptr;
}
Exporter* machine_exporter(const std::string& id) { return machine_exporter(PluginRegistry::get(), id); }

Options merge_options(const Exporter& ex, const MachineSetup& ms, MachineJob& job) {
    Options o = default_options(ex.info());
    for (auto& kv : ms.options) if (kv.first.rfind("__", 0) != 0) o[kv.first] = kv.second;
    for (auto it = job.options.begin(); it != job.options.end();) it = it->first.rfind("__", 0) == 0 ? job.options.erase(it) : std::next(it);
    for (auto& kv : o) job.options[kv.first] = kv.second;
    return o;
}

int written_mark_count(const MachineJob& j) {
    if (j.machine.rfind("juki.", 0) == 0) return (int)juki::select_marks(j).marks.size();
    return (int)j.fiducials.size();
}

bool check_program_bytes(const std::string& exporter_id, const std::vector<uint8_t>& b, std::vector<std::string>& errors, std::string& fail) {
    errors.clear();
    fail.clear();
    if (b.empty()) { fail = "the machine interface produced an empty program"; return false; }
    if (exporter_id.rfind("juki.", 0) != 0) return true;   // no reader for other formats: the byte read-back is the check
    std::vector<juki::HlcCheck> ck;
    if (!juki::verify_hlc_bytes(b, ck, fail)) return false;
    for (auto& c : ck) if (c.error) errors.push_back("Machine program check: " + c.msg);
    return true;
}

std::vector<std::string> fresh_byte_errors(const std::vector<ExportIssue>& reviewed, const std::vector<std::string>& errors) {
    std::vector<std::string> fresh;
    for (auto& e : errors) {
        bool seen = false;
        for (auto& r : reviewed)
            seen |= r.sev == ExportIssue::Error && r.msg.size() >= e.size() && r.msg.compare(r.msg.size() - e.size(), e.size(), e) == 0;
        if (!seen) fresh.push_back(e);
    }
    return fresh;
}

namespace {
PluginRegistry& registry_of(PluginRegistry* reg) { return reg ? *reg : PluginRegistry::get(); }

// exporter -> bytes -> byte check (shared tail of generate_point / generate_side)
bool run_exporter(Exporter& ex, const Options& o, Generated& out, std::string& err) {
    std::string xerr;
    bool ok = false;
    try { ok = ex.export_job(out.job, o, out.bytes, xerr); } catch (const std::exception& e) { xerr = e.what(); }
    if (!ok) {
        err = ex.info().name + ": " + (xerr.empty() ? std::string("the machine interface failed to generate the program.") : xerr);
        return false;
    }
    std::string fail;
    if (!check_program_bytes(out.job.machine, out.bytes, out.byte_errors, fail)) {
        err = ex.info().name + ": " + fail + ". Nothing was written; the previous program file is unchanged.";
        return false;
    }
    return true;
}
} // namespace

bool generate_point(const Project& p, int point, Generated& out, std::string& err, PluginRegistry* reg) {
    out = Generated{};
    err.clear();
    if (point < 0 || point >= (int)p.model.points.size()) { err = "Invalid process point."; return false; }
    const ModelPoint& mp = p.model.points[point];
    if (!mp.has_machine) { err = "Process point '" + mp.name + "' has no machine interface. Enable it on the Export page."; return false; }
    Exporter* ex = machine_exporter(registry_of(reg), mp.machine.exporter_id);
    if (!ex) {
        err = mp.machine.exporter_id.empty() ? "No machine type is selected for process point '" + mp.name + "'."
                                             : "Machine interface '" + mp.machine.exporter_id + "' is not installed.";
        return false;
    }
    out.point = point;
    out.exporter_id = ex->info().id;
    out.exporter_name = ex->info().name;
    out.file = program_file_for(p, point, ex);
    if (!build_machine_job(p, point, out.job, err)) { if (err.empty()) err = "Nothing to program."; return false; }
    const Options o = merge_options(*ex, mp.machine, out.job);
    out.job.machine = ex->info().id;
    return run_exporter(*ex, o, out, err);
}

bool generate_side(const Project& p, const MfgSide& s, Generated& out, std::string& err, PluginRegistry* reg) {
    out = Generated{};
    err.clear();
    const int pi = side_point_index(p, s);
    if (pi < 0) { err = mfg_label(s) + ": export layer '" + s.point + "' is missing"; return false; }
    const ModelPoint& mp = p.model.points[pi];
    Exporter* ex = machine_exporter(registry_of(reg), mp.machine.exporter_id);
    if (!ex) { err = "no machine interface"; return false; }
    out.point = pi;
    out.exporter_id = ex->info().id;
    out.exporter_name = ex->info().name;
    out.file = program_file_for(p, pi, ex);   // R18d: each layer keeps its own file name
    if (!build_side_job(p, s, out.job, err)) { if (err.empty()) err = "Nothing to program."; return false; }
    const Options o = merge_options(*ex, mp.machine, out.job);
    if (out.job.options["juki.comment"].empty()) out.job.options["juki.comment"] = mfg_label(s);   // HLC Lineinfo comment (32 chars)
    apply_desc_mode(p, out.job, out.job.options["sides.desc_mode"]);
    out.job.machine = ex->info().id;
    return run_exporter(*ex, o, out, err);
}

std::string join_path(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    if (dir.back() == '\\' || dir.back() == '/') return dir + file;
#ifdef _WIN32
    return dir + "\\" + file;
#else
    return dir + (dir.find('\\') != std::string::npos && dir.find('/') == std::string::npos ? "\\" : "/") + file;
#endif
}

bool write_program(const std::string& path_in, const Generated& g, WriteResult& r) {
    const std::string path = path_in;   // path_in may alias r.path
    r = WriteResult{};
    r.path = path;
    if (!write_file_verified(path, g.bytes.data(), g.bytes.size(), r.err, &r.stage)) {   // temp -> read-back -> replace -> read-back
        if (r.stage.empty()) r.stage = "write";
        return false;
    }
    std::vector<uint8_t> back;
    std::string re, fail;
    if (!read_file(path, back, re)) { r.stage = "verify"; r.err = re; return false; }
    if (!check_program_bytes(g.job.machine, back, r.post_errors, fail)) { r.stage = "verify"; r.err = fail; return false; }
    r.stage.clear();
    r.ok = true;
    return true;
}

bool export_point(const Project& p, int point, const std::string& dir, PointExport& out, bool dry_run, PluginRegistry* reg) {
    out = PointExport{};
    Exporter* ex = point >= 0 && point < (int)p.model.points.size() ? machine_exporter(registry_of(reg), p.model.points[point].machine.exporter_id) : nullptr;
    const ExportEntry entry = ex && !ex->info().builtin ? ExportEntry::PluginExport : ExportEntry::GenerateCommand;
    out.preflight = export_preflight(p, ExportRequest{entry, point});   // reported, never enforced
    if (!generate_point(p, point, out.gen, out.err, reg)) return false;
    const std::string d = dir.empty() ? p.model.export_dir : dir;
    if (d.empty()) { out.err = "No export folder: pass one or set the project's export folder."; return false; }
    out.write.path = join_path(d, out.gen.file);
    if (dry_run) { out.ok = true; return true; }
    if (!write_program(out.write.path, out.gen, out.write)) {
        out.err = (out.write.verify_failed() ? "The program was NOT exported correctly: " : "Cannot save the program: ") + out.write.err + " (" + out.write.path + ")";
        return false;
    }
    out.ok = true;
    return true;
}

bool export_link(const Project& p, int link, const std::string& dir, LinkExport& out, std::string& err, bool dry_run,
                 const ByteErrorGate& gate, PluginRegistry* reg) {
    out = LinkExport{};
    err.clear();
    const MfgSides mfg = link_sides(p, link);   // R18d: the linked pair (upper layer = Side 1)
    if (!mfg.configured) { err = "Link " + std::to_string(link) + " does not have exactly two export layers."; return false; }
    const auto mem = link_members(p, link);
    out.preflight = export_preflight(p, ExportRequest{ExportEntry::ExportAllSides, mem.empty() ? -1 : mem[0]});
    out.dir = dir.empty() ? p.model.export_dir : dir;
    if (out.dir.empty()) { err = "No export folder: pass one or set the project's export folder."; return false; }
    bool all_ok = true;
    for (const MfgSide& sd : mfg.sides) {
        SideExportResult r;
        r.side = sd;
        Generated g;
        const int pi = side_point_index(p, sd);
        if (pi >= 0) {
            const ModelPoint& mp = p.model.points[pi];
            r.point = mp.name;
            r.orientation = mp.machine.orientation; r.flipped = mp.machine.flipped;
            const Vec2 org = mp.machine.has_origin ? mp.machine.origin : p.virtual_origin;
            r.origin_x = org.x; r.origin_y = org.y;
        }
        if (generate_side(p, sd, g, r.err, reg)) {
            r.exporter = g.exporter_name;
            r.program_file = g.file;
            const size_t dot = r.program_file.rfind('.');
            r.csv_file = (dot == std::string::npos ? r.program_file : r.program_file.substr(0, dot)) + " - placements.csv";
            const auto fresh = fresh_byte_errors(out.preflight, g.byte_errors);
            bool write = true;
            if (!fresh.empty()) {
                if (gate && !gate(sd, fresh)) { write = false; r.err = "cancelled at the generated program check"; }
                else for (auto& e : fresh) r.warnings.push_back(mfg_label(sd) + ": " + e);
            }
            if (write) {
                if (!dry_run) {
                    WriteResult w;
                    if (!write_program(join_path(out.dir, r.program_file), g, w))
                        r.err = (w.verify_failed() ? "verification FAILED " : "write ") + r.program_file + ": " + w.err;
                    else {
                        const std::string c = placements_csv(p, sd, g.job);
                        std::string we;
                        if (!write_file(join_path(out.dir, r.csv_file), c.data(), c.size(), we)) r.warnings.push_back("CSV not written: " + we);
                    }
                }
                r.ok = r.err.empty();
            }
            r.placements = (int)g.job.placements.size(); r.parts = (int)g.job.parts.size();
            r.fiducials = written_mark_count(g.job); r.circuits = (int)g.job.circuits.size();
            for (auto& w : g.job.warnings) r.warnings.push_back(w);
        }
        all_ok &= r.ok;
        out.sides.push_back(r);
        out.gens.push_back(std::move(g));
    }
    if (!dry_run) {
        const std::string rb = sanitize_file_part(job_base_name(p)) + " - sides report";
        const std::string html = sides_report_html(p, out.sides), txt = sides_report_txt(p, out.sides);   // Side 1 / Side 2 run order
        std::string we;
        out.report_html = join_path(out.dir, rb + ".html");
        out.report_txt = join_path(out.dir, rb + ".txt");
        if (!write_file(out.report_html, html.data(), html.size(), we)) out.report_html.clear();
        if (!write_file(out.report_txt, txt.data(), txt.size(), we)) out.report_txt.clear();
    }
    out.all_ok = all_ok;
    if (!all_ok)
        for (auto& r : out.sides) if (!r.ok) err += mfg_label(r.side) + ": FAILED - " + r.err + "\n";
    return all_ok;
}

} // namespace pcbp::progen
