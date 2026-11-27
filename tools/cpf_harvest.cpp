// cpf_harvest - walk a folder of legacy CAM projects (.cpf) and dump every part number with its
// description, package and measured land-pattern / body geometry, for package-profile matching.
//
// usage: cpf_harvest [folder] [out_prefix]
//   default folder     C:\FlexAssign\cpf (recursive)
//   default out_prefix <folder>\..\cpf_harvest
// writes <prefix>_rows.csv   one row per (file, part number)
//        <prefix>_parts.csv  one row per part number, aggregated over all files
//        <prefix>_errors.csv files that failed to open
#include "core/fileio.h"
#include "core/project.h"
#include "io/flexdb.h"
#include "plugins/plugin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace pcbp;
namespace fs = std::filesystem;

static std::string q(const std::string& s) {
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; if (c == '\r' || c == '\n') c = ' '; o += c; }
    return o + "\"";
}
static std::string f3(double v) { char b[32]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
static std::string u8(const fs::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }
static std::string lowr(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

struct Agg {
    std::map<std::string, int> desc, pkg, shape, manf, vend, cust;
    std::set<std::string> files;
    int qty = 0;
    std::vector<double> bl, bw, el, ew, pitch, h;
    int pads = 0;
};
static double median(std::vector<double> v) {
    v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return !(x > 0); }), v.end());
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
template <class M> static std::string top(const M& m, int* n = nullptr) {
    std::string best; int bn = -1;
    for (auto& kv : m) if (kv.second > bn) { bn = kv.second; best = kv.first; }
    if (n) *n = bn;
    return best;
}

int main(int argc, char** argv) {
    fs::path root = argc > 1 ? fs::path((const char8_t*)argv[1]) : fs::path(u8"C:\\FlexAssign\\cpf");
    std::string prefix = argc > 2 ? argv[2] : u8(root.parent_path() / "cpf_harvest");

    PluginRegistry reg;
    register_builtin_plugins(reg);
    Importer* imp = reg.importer("circuitcam.cpf");
    if (!imp) { std::fprintf(stderr, "cpf importer missing\n"); return 2; }

    std::vector<fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && lowr(u8(it->path().extension())) == ".cpf") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    std::printf("%zu .cpf files under %s\n", files.size(), u8(root).c_str());
    if (files.empty()) return 1;

    FILE* fr = std::fopen((prefix + "_rows.csv").c_str(), "wb");
    FILE* fe = std::fopen((prefix + "_errors.csv").c_str(), "wb");
    if (!fr || !fe) { std::fprintf(stderr, "cannot write %s_*.csv\n", prefix.c_str()); return 2; }
    std::fprintf(fr, "File,PartNumber,Description,CustPN,VendPN,ManfPN,Polarized,PopType,PartMarking,Package,PackageDesc,Leads,JLead,Radial,Notched,PinMarked,Qty,Pads,Shape,BodyL,BodyW,BodyKnown,"
                     "PadExtL,PadExtW,Pitch,ChipCode,Height,SMT,Packaging,ProgType,ProgTape,ProgPitch,"
                     "MachNozzle,MachTapeW,MachPitch,MachType\n");
    std::fprintf(fe, "File,Error\n");

    std::map<std::string, Agg> agg;
    struct PkRow { std::string name, pins; int files = 0, leads = 0, jl = 0, rad = 0, notch = 0, pm = 0, smt = 1; flexdb::Footprint fp; std::string first_file; };
    std::map<std::string, PkRow> pkrows;   // key: name + pin signature
    auto pin_sig = [](const Package& p) {
        std::string o; char b[128];
        for (auto& d : p.pads) {
            std::snprintf(b, sizeof b, "%s%d:%.3f,%.3f,%.3fx%.3f%s%s", o.empty() ? "" : ";", d.pin, d.pos.x, d.pos.y, d.w, d.h,
                          d.shape == PadShape::Round ? "R" : "", d.drill > 0 ? "T" : "");
            o += b;
            if (d.rot != 0) { std::snprintf(b, sizeof b, "@%.1f", d.rot); o += b; }
        }
        return o;
    };
    int ok = 0, bad = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        const std::string path = u8(files[i]);
        const std::string fname = u8(files[i].filename());
        std::printf("[%zu/%zu] %s", i + 1, files.size(), fname.c_str());
        Project prj;
        ImportReport rep;
        std::string err;
        Options o = default_options(imp->info());
        bool r = false;
        try { r = imp->run(path, o, prj, rep, err); } catch (const std::exception& e) { err = e.what(); }
        if (!r) { ++bad; std::printf("  FAILED %s\n", err.c_str()); std::fprintf(fe, "%s,%s\n", q(path).c_str(), q(err).c_str()); continue; }
        ++ok;
        std::map<std::string, int> qty;
        std::map<std::string, std::map<std::string, int>> cpk;
        for (auto& c : prj.comps) if (!c.number.empty()) { qty[c.number]++; if (!c.package.empty()) cpk[c.number][c.package]++; }
        std::map<std::string, flexdb::Footprint> fpc;
        for (auto& kv : prj.parts) {
            const PartNumber& pn = kv.second;
            std::string pkname = pn.package;
            if (pkname.empty() && cpk.count(kv.first)) pkname = top(cpk[kv.first]);
            const Package* pk = nullptr;
            auto pit = prj.packages.find(pkname);
            if (pit != prj.packages.end()) pk = &pit->second;
            flexdb::Footprint fp;
            if (pk) {
                auto f = fpc.find(pkname);
                if (f == fpc.end()) f = fpc.emplace(pkname, flexdb::measure_footprint(*pk)).first;
                fp = f->second;
            }
            const double h = pn.height > 0 ? pn.height : (pk ? pk->height : 0);
            const int n = qty.count(kv.first) ? qty[kv.first] : 0;
            std::fprintf(fr, "%s,%s,%s,%s,%s,%s,%d,%s,%s,%s,%s,%d,%d,%d,%d,%d,%d,%d,%s,%s,%s,%d,%s,%s,%s,%s,%s,%d,%s,%d,%d,%d,%d,%d,%d,%d\n",
                q(fname).c_str(), q(kv.first).c_str(), q(pn.description).c_str(), q(pn.cust_pn).c_str(), q(pn.vend_pn).c_str(),
                q(pn.manf_pn).c_str(), pn.polarized ? 1 : 0, q(pn.pop_type).c_str(), q(pn.part_marking).c_str(), q(pkname).c_str(),
                q(pk ? pk->description : "").c_str(), pk ? pk->leads : 0, pk && pk->j_lead, pk && pk->radial, pk && pk->notched, pk && pk->pin_marked, n, fp.pads, q(fp.shape).c_str(),
                f3(fp.body_l).c_str(), f3(fp.body_w).c_str(), fp.body_known ? 1 : 0,
                f3(fp.ext_l).c_str(), f3(fp.ext_w).c_str(), f3(fp.pitch).c_str(), q(fp.chip_code).c_str(),
                f3(h).c_str(), pk ? (pk->smt ? 1 : 0) : 1, q(pn.packaging).c_str(),
                pn.prog_type, pn.prog_tape, pn.prog_pitch,
                pk ? pk->mach.nozzle : 0, pk ? pk->mach.tape_w : 0, pk ? pk->mach.pitch : 0, pk ? pk->mach.type_code : 0);
            Agg& a = agg[kv.first];
            if (!pn.description.empty()) a.desc[pn.description]++;
            if (!pn.manf_pn.empty()) a.manf[pn.manf_pn]++;
            if (!pn.vend_pn.empty()) a.vend[pn.vend_pn]++;
            if (!pn.cust_pn.empty()) a.cust[pn.cust_pn]++;
            if (!pkname.empty()) a.pkg[pkname]++;
            if (!fp.shape.empty()) a.shape[fp.shape]++;
            a.files.insert(fname);
            a.qty += n;
            a.bl.push_back(fp.body_l); a.bw.push_back(fp.body_w);
            a.el.push_back(fp.ext_l); a.ew.push_back(fp.ext_w);
            a.pitch.push_back(fp.pitch); a.h.push_back(h);
            a.pads = std::max(a.pads, fp.pads);
        }
        for (auto& pkv : prj.packages) {
            const Package& p = pkv.second;
            std::string sig = pin_sig(p);
            PkRow& pr = pkrows[p.name + "\x1f" + sig];
            if (!pr.files) { pr.name = p.name; pr.pins = sig; pr.leads = p.leads; pr.jl = p.j_lead; pr.rad = p.radial; pr.notch = p.notched;
                             pr.pm = p.pin_marked; pr.smt = p.smt; pr.fp = flexdb::measure_footprint(p); pr.first_file = fname; }
            pr.files++;
        }
        std::printf("  %zu parts, %zu comps\n", prj.parts.size(), prj.comps.size());
        std::fflush(fr);
    }
    std::fclose(fr); std::fclose(fe);

    FILE* fp = std::fopen((prefix + "_parts.csv").c_str(), "wb");
    std::fprintf(fp, "PartNumber,Description,ManfPN,VendPN,CustPN,Package,PackageVotes,PackageVariants,Files,TotalQty,Pads,Shape,"
                     "BodyL,BodyW,PadExtL,PadExtW,Pitch,Height\n");
    for (auto& kv : agg) {
        const Agg& a = kv.second;
        int pv = 0;
        std::string pk = top(a.pkg, &pv);
        std::string variants;
        for (auto& p : a.pkg) if (p.first != pk) variants += (variants.empty() ? "" : "; ") + p.first + " x" + std::to_string(p.second);
        std::fprintf(fp, "%s,%s,%s,%s,%s,%s,%d,%s,%zu,%d,%d,%s,%s,%s,%s,%s,%s,%s\n", q(kv.first).c_str(), q(top(a.desc)).c_str(), q(top(a.manf)).c_str(), q(top(a.vend)).c_str(), q(top(a.cust)).c_str(),
            q(pk).c_str(), pv < 0 ? 0 : pv, q(variants).c_str(), a.files.size(), a.qty, a.pads, q(top(a.shape)).c_str(),
            f3(median(a.bl)).c_str(), f3(median(a.bw)).c_str(), f3(median(a.el)).c_str(), f3(median(a.ew)).c_str(),
            f3(median(a.pitch)).c_str(), f3(median(a.h)).c_str());
    }
    std::fclose(fp);
    FILE* fk = std::fopen((prefix + "_packages.csv").c_str(), "wb");
    std::fprintf(fk, "Package,Files,FirstFile,Leads,Pads,Shape,BodyL,BodyW,PadExtL,PadExtW,Pitch,ChipCode,SMT,JLead,Radial,Notched,PinMarked,"
                     "Pins(pin:x,y,WxH mm; R=round T=thru @rot)\n");
    for (auto& kv : pkrows) {
        const PkRow& p = kv.second;
        std::fprintf(fk, "%s,%d,%s,%d,%d,%s,%s,%s,%s,%s,%s,%s,%d,%d,%d,%d,%d,%s\n", q(p.name).c_str(), p.files, q(p.first_file).c_str(),
            p.leads, p.fp.pads, q(p.fp.shape).c_str(), f3(p.fp.body_l).c_str(), f3(p.fp.body_w).c_str(), f3(p.fp.ext_l).c_str(),
            f3(p.fp.ext_w).c_str(), f3(p.fp.pitch).c_str(), q(p.fp.chip_code).c_str(), p.smt, p.jl, p.rad, p.notch, p.pm, q(p.pins).c_str());
    }
    std::fclose(fk);
    std::printf("\n%d ok, %d failed, %zu distinct part numbers\nwrote %s_rows.csv, %s_parts.csv, %s_errors.csv\n",
                ok, bad, agg.size(), prefix.c_str(), prefix.c_str(), prefix.c_str());
    return 0;
}
