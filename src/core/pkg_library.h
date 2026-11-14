#pragma once
// ============================================================================
// pkg_library.h — master package library (shared across projects).
//
// A project carries its own LOCAL packages (Project::packages). The master
// library is a separate persistent file (*.pcbplib, UTF-8 JSON) that local
// packages are sent to and pulled from. CircuitCAM libraries (.cpf/.mdb) can
// be opened read-only as additional sources (loaded by the GUI via the
// importer plugin, then wrapped with from_packages()).
// ============================================================================
#include "project.h"
#include <map>
#include <functional>
#include <string>
#include <vector>

namespace pcbp {

struct PackageLibrary {
    std::string path;                       // UTF-8 file path ("" = unsaved)
    std::string name;                       // display name
    std::map<std::string, Package> packages;
    bool read_only = false;                 // CircuitCAM sources
    uint64_t gen = 1;                       // bumps on every change

    bool load(const std::string& path, std::string& err);   // *.pcbplib (missing file = empty library)
    bool save(std::string& err) const;
    void touch() { ++gen; }
};

// geometry + flags equality (name / process / confirmed ignored), 1e-4 mm tolerance
bool package_same(const Package& a, const Package& b);
// canonical text used by package_same (exposed for hashing / caching)
std::string package_canon(const Package& p);
// R18c multi-select: apply ONE field edit `f` to each named package (f must only touch the field being edited).
// Packages f leaves unchanged are skipped. Returns the names actually changed. No undo checkpoint here.
std::vector<std::string> pkg_edit_many(Project& prj, const std::vector<std::string>& pkgs, const std::function<void(Package&)>& f);


enum class SyncState : uint8_t { LocalOnly, Same, Differs, LibraryOnly };
SyncState sync_state(const std::map<std::string, Package>& local, const std::map<std::string, Package>& lib, const std::string& name, bool from_local);

enum class ConflictAction : uint8_t { Ask, Overwrite, Skip, KeepBoth };

std::string unique_package_name(const std::map<std::string, Package>& m, const std::string& base, const char* suffix = "_");

struct TransferStats { int added = 0, replaced = 0, skipped = 0, renamed = 0, identical = 0; };

// Copy `names` from src into dst. `resolve(name)` is called for every name that
// exists in dst with different content and returns the action (never Ask).
// Returns false when resolve() asked to abort (returns Ask).
template <class Resolve>
bool transfer_packages(const std::map<std::string, Package>& src, std::map<std::string, Package>& dst,
                       const std::vector<std::string>& names, Resolve&& resolve, TransferStats& st,
                       std::vector<std::string>* renamed_to = nullptr) {
    for (auto& n : names) {
        auto s = src.find(n);
        if (s == src.end()) continue;
        auto d = dst.find(n);
        if (d == dst.end()) { dst[n] = s->second; dst[n].name = n; ++st.added; continue; }
        if (package_same(d->second, s->second) && d->second.description == s->second.description) { ++st.identical; continue; }
        ConflictAction a = resolve(n);
        if (a == ConflictAction::Ask) return false;
        if (a == ConflictAction::Skip) { ++st.skipped; continue; }
        if (a == ConflictAction::Overwrite) { d->second = s->second; d->second.name = n; ++st.replaced; continue; }
        Package p = s->second;
        p.name = unique_package_name(dst, n);
        if (renamed_to) renamed_to->push_back(p.name);
        dst[p.name] = p;
        ++st.renamed;
    }
    return true;
}

} // namespace pcbp
