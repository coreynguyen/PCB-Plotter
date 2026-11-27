// flex_fill - build a fresh FlexLine component database from a decisions CSV.
//
// usage: flex_fill <blank.mdb> <profile library .flx> <decisions.csv> <out.mdb> [--all]
//   decisions.csv columns: Name, Description, ExtName, Source, ProfileID, L, W, H, TapeTypeID, Pitch, Nozzle
//   Rows with Source=manual are skipped unless --all (they are already taught in your own database).
// For each part: add it, copy the chosen library profile (all parameter tables), then overwrite
// Compo Width/Length/Height (mm -> 1e-4 mm, Width = long side), Feeder TapeTypeID/Pitch,
// Laser.GX4NozzleNo and LaserFx3.NozzleNo. PkgTypeID / PkgType keep the chosen profile.
#include "io/flexdb.h"
#include "io/jet_write.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace pcbp;

static std::vector<std::vector<std::string>> read_csv(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), {});
    std::vector<std::vector<std::string>> rows(1);
    std::string cell;
    bool q = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (q) {
            if (c == '"') { if (i + 1 < s.size() && s[i + 1] == '"') { cell += '"'; ++i; } else q = false; }
            else cell += c;
        } else if (c == '"') q = true;
        else if (c == ',') { rows.back().push_back(cell); cell.clear(); }
        else if (c == '\n') { rows.back().push_back(cell); cell.clear(); rows.emplace_back(); }
        else if (c != '\r') cell += c;
    }
    if (!cell.empty() || !rows.back().empty()) rows.back().push_back(cell);
    if (rows.back().empty()) rows.pop_back();
    return rows;
}

static bool set_col(flexdb::ComponentDB& db, const char* table, int cid, const char* col, int64_t v, std::string& note) {
    jet::ETable* t = db.table(table);
    if (!t) return false;
    int k = t->col("CompoID"), c = t->col(col);
    if (k < 0 || c < 0) return false;
    bool any = false;
    for (size_t r = 0; r < t->rows.size(); ++r)
        if (t->get_i(r, k) == cid) { t->set_i(r, c, v); any = true; }
    if (!any) note += std::string(" no ") + table + " row;";
    return any;
}

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: flex_fill <blank.mdb> <library.flx> <decisions.csv> <out.mdb> [--all]\n"); return 2; }
    const bool all = argc > 5 && std::string(argv[5]) == "--all";
    std::string err;
    flexdb::Library lib;
    if (!lib.open(argv[2], err)) { std::fprintf(stderr, "library: %s\n", err.c_str()); return 1; }
    flexdb::ComponentDB db;
    if (!db.create_from_template(argv[1], err)) { std::fprintf(stderr, "template: %s\n", err.c_str()); return 1; }
    auto rows = read_csv(argv[3]);
    if (rows.empty()) { std::fprintf(stderr, "empty csv\n"); return 1; }
    std::map<std::string, int> h;
    for (size_t i = 0; i < rows[0].size(); ++i) h[rows[0][i]] = (int)i;
    auto get = [&](const std::vector<std::string>& r, const char* k) { auto it = h.find(k); return it == h.end() || it->second >= (int)r.size() ? std::string() : r[it->second]; };
    auto num = [](const std::string& s) { try { return s.empty() ? 0.0 : std::stod(s); } catch (...) { return 0.0; } };

    int added = 0, skipped = 0, failed = 0;
    for (size_t i = 1; i < rows.size(); ++i) {
        const auto& r = rows[i];
        std::string name = get(r, "Name");
        if (name.empty()) continue;
        if (!all && get(r, "Source") == "manual") { ++skipped; continue; }
        int cid = db.add_part(name, get(r, "Description"), get(r, "ExtName"), err);
        if (!cid) { std::printf("FAIL %s: %s\n", name.c_str(), err.c_str()); ++failed; continue; }
        std::string note;
        int pid = (int)num(get(r, "ProfileID"));
        if (pid && !db.apply_profile(cid, lib, pid, 0, false, err)) { note += " profile " + std::to_string(pid) + ": " + err + ";"; pid = 0; }
        double L = num(get(r, "L")), W = num(get(r, "W")), H = num(get(r, "H"));
        if (L > 0) {
            set_col(db, "Compo", cid, "Width", std::lround(std::max(L, W) * 1e4), note);
            set_col(db, "Compo", cid, "Length", std::lround(std::min(L, W) * 1e4), note);
            if (H > 0) set_col(db, "Compo", cid, "Height", std::lround(H * 1e4), note);
        }
        if (pid) {
            if (int t = (int)num(get(r, "TapeTypeID"))) set_col(db, "Feeder", cid, "TapeTypeID", t, note);
            if (int p = (int)num(get(r, "Pitch"))) set_col(db, "Feeder", cid, "Pitch", p, note);
            if (int n = (int)num(get(r, "Nozzle"))) { set_col(db, "Laser", cid, "GX4NozzleNo", n, note); set_col(db, "LaserFx3", cid, "NozzleNo", n, note); }
        } else note += " untaught (no profile);";
        ++added;
        if (!note.empty()) std::printf("%-20s%s\n", name.c_str(), note.c_str());
    }
    if (!db.save(argv[4], err)) { std::fprintf(stderr, "save: %s\n", err.c_str()); return 1; }
    std::printf("added %d, skipped %d (manual), failed %d -> %s\n", added, skipped, failed, argv[4]);
    return failed ? 3 : 0;
}
