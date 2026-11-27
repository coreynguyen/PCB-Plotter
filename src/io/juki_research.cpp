#include "juki_research.h"

#include "io/jet_mdb.h"
#include "io/juki_research_rules.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <cctype>

namespace pcbp::juki {

namespace {
std::string up(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
}  // namespace


// ------------------------------------------------------------------------------------------------ ResearchDb
bool ResearchDb::open(const std::string& path, std::string& err) {
    jet::Database jdb;   // the read-only reader (resolves memo / long values)
    if (!jdb.open(path, err)) return false;
    ids_.clear();
    ext_.clear();
    name_of_.clear();
    ext_of_.clear();
    tabs_.clear();
    for (const std::string& tn : jdb.tables()) {
        if (tn.rfind("MSys", 0) == 0) continue;
        jet::Table t;
        if (!jdb.read_table(tn, t, err)) return false;
        const int k = t.col("CompoID");   // part tables; the others are reference tables (lookup())
        Tab& T = tabs_[up(tn)];
        std::vector<int> numeric, texts;
        for (size_t c = 0; c < t.cols.size(); ++c) {
            auto ty = t.cols[c].type;
            if (ty == jet::ColType::Text || ty == jet::ColType::Memo) {
                T.scols[up(t.cols[c].name)] = (int)texts.size();
                texts.push_back((int)c);
                continue;
            }
            if (ty == jet::ColType::OLE || ty == jet::ColType::Binary || ty == jet::ColType::GUID || ty == jet::ColType::DateTime)
                continue;
            T.cols[up(t.cols[c].name)] = (int)numeric.size();
            numeric.push_back((int)c);
        }
        for (const auto& row : t.rows) {
            const long id = k >= 0 && !row[(size_t)k].null ? (long)row[(size_t)k].as_int() : 0;
            Row R;
            R.v.assign(numeric.size(), 0);
            R.null.assign(numeric.size(), 0);
            for (size_t j = 0; j < numeric.size(); ++j) {
                const jet::Value& x = row[(size_t)numeric[j]];
                const auto ty = t.cols[(size_t)numeric[j]].type;
                if (ty == jet::ColType::Bool) { R.v[j] = x.as_bool() ? 1 : 0; continue; }   // never NULL in Jet
                if (x.null) { R.null[j] = 1; continue; }
                R.v[j] = ty == jet::ColType::Float || ty == jet::ColType::Double ? x.d : (double)x.as_int();
            }
            R.s.resize(texts.size());
            R.snull.assign(texts.size(), 0);
            for (size_t j = 0; j < texts.size(); ++j) {
                const jet::Value& x = row[(size_t)texts[j]];
                if (x.null) R.snull[j] = 1;
                else R.s[j] = x.as_string();
            }
            if (k >= 0) T.rows[id].push_back(std::move(R));
            else T.list.push_back(std::move(R));
        }
        if (up(tn) == "COMPO") {
            const int n = t.col("CompoName"), x = t.col("ExtCompoName");
            for (const auto& row : t.rows) {
                if (n < 0 || k < 0) continue;
                const long id = (long)row[(size_t)k].as_int();
                const std::string nm = row[(size_t)n].as_string();
                ids_[up(nm)] = id;
                name_of_[id] = nm;
                if (x >= 0 && !row[(size_t)x].null) {
                    const std::string e = row[(size_t)x].as_string();
                    if (!e.empty()) { ext_of_[id] = e; ext_.emplace(up(e), id); }
                }
            }
        }
    }
    if (ids_.empty()) { err = "no Compo table / parts"; return false; }
    return true;
}

long ResearchDb::id_of(const std::string& name) const {
    auto it = ids_.find(up(name));
    return it == ids_.end() ? 0 : it->second;
}

long ResearchDb::find_part(const std::string& part, std::string* machine_name) const {
    long id = id_of(part);
    if (!id) {
        auto it = ext_.find(up(part));
        if (it != ext_.end()) id = it->second;
    }
    if (!id && part.size() > 20) {
        // the 20-character machine name, unless that name belongs to a different (long) part number
        const long t = id_of(part.substr(0, 20));
        auto e = ext_of_.find(t);
        if (t && (e == ext_of_.end() || up(e->second) == up(part))) id = t;
    }
    if (id && machine_name) {
        auto n = name_of_.find(id);
        *machine_name = n == name_of_.end() ? part : n->second;
    }
    return id;
}

std::vector<std::string> ResearchDb::names() const {
    std::vector<std::string> v;
    v.reserve(ids_.size());
    for (const auto& kv : ids_) v.push_back(kv.first);
    return v;
}

bool ResearchDb::value(long id, const std::string& table, const std::string& column, double& v) const {
    return value_n(id, table, 0, column, v);
}

size_t ResearchDb::rows(long id, const std::string& table) const {
    auto t = tabs_.find(up(table));
    if (t == tabs_.end()) return 0;
    auto r = t->second.rows.find(id);
    return r == t->second.rows.end() ? 0 : r->second.size();
}

bool ResearchDb::value_n(long id, const std::string& table, size_t n, const std::string& column, double& v) const {
    auto t = tabs_.find(up(table));
    if (t == tabs_.end()) return false;
    auto c = t->second.cols.find(up(column));
    if (c == t->second.cols.end()) return false;
    auto r = t->second.rows.find(id);
    if (r == t->second.rows.end() || n >= r->second.size() || r->second[n].null[(size_t)c->second]) return false;
    v = r->second[n].v[(size_t)c->second];
    return true;
}

bool ResearchDb::lookup_text(const std::string& table, const std::string& key_column, long key, const std::string& column,
                             std::string& v) const {
    auto t = tabs_.find(up(table));
    if (t == tabs_.end()) return false;
    auto kc = t->second.cols.find(up(key_column));
    auto c = t->second.scols.find(up(column));
    if (kc == t->second.cols.end() || c == t->second.scols.end()) return false;
    for (const Row& r : t->second.list) {
        if (r.null[(size_t)kc->second] || std::lround(r.v[(size_t)kc->second]) != key) continue;
        if (r.snull[(size_t)c->second]) return false;
        v = r.s[(size_t)c->second];
        return true;
    }
    return false;
}

bool ResearchDb::text_n(long id, const std::string& table, size_t n, const std::string& column, std::string& v) const {
    auto t = tabs_.find(up(table));
    if (t == tabs_.end()) return false;
    auto c = t->second.scols.find(up(column));
    if (c == t->second.scols.end()) return false;
    auto r = t->second.rows.find(id);
    if (r == t->second.rows.end() || n >= r->second.size() || r->second[n].snull[(size_t)c->second]) return false;
    v = r->second[n].s[(size_t)c->second];
    return true;
}

std::vector<size_t> ResearchDb::ref_rows(const std::string& table, const std::string& key_column, long key) const {
    std::vector<size_t> out;
    auto t = tabs_.find(up(table));
    if (t == tabs_.end()) return out;
    auto kc = t->second.cols.find(up(key_column));
    if (kc == t->second.cols.end()) return out;
    for (size_t i = 0; i < t->second.list.size(); ++i) {
        const Row& r = t->second.list[i];
        if (!r.null[(size_t)kc->second] && std::lround(r.v[(size_t)kc->second]) == key) out.push_back(i);
    }
    return out;
}

bool ResearchDb::ref_value(const std::string& table, size_t row, const std::string& column, double& v) const {
    auto t = tabs_.find(up(table));
    if (t == tabs_.end() || row >= t->second.list.size()) return false;
    auto c = t->second.cols.find(up(column));
    if (c == t->second.cols.end() || t->second.list[row].null[(size_t)c->second]) return false;
    v = t->second.list[row].v[(size_t)c->second];
    return true;
}

bool ResearchDb::ref_text(const std::string& table, size_t row, const std::string& column, std::string& v) const {
    auto t = tabs_.find(up(table));
    if (t == tabs_.end() || row >= t->second.list.size()) return false;
    auto c = t->second.scols.find(up(column));
    if (c == t->second.scols.end() || t->second.list[row].snull[(size_t)c->second]) return false;
    v = t->second.list[row].s[(size_t)c->second];
    return true;
}

bool ResearchDb::text(long id, const std::string& table, const std::string& column, std::string& v) const {
    auto t = tabs_.find(up(table));
    if (t == tabs_.end()) return false;
    auto c = t->second.scols.find(up(column));
    if (c == t->second.scols.end()) return false;
    auto r = t->second.rows.find(id);
    if (r == t->second.rows.end() || r->second.empty() || r->second[0].snull[(size_t)c->second]) return false;
    v = r->second[0].s[(size_t)c->second];
    return true;
}

int ResearchDb::load_machine_folder(const std::string& folder) {
    const char* names[3] = {"SubCompo.ini", "SubCompoFx1r.ini", "SubCompoFx2.ini"};
    int n = 0;
    for (int k = 0; k < 3; ++k) {
        accel_[k] = rules::SubCompo();
        if (folder.empty()) continue;
        // case-insensitive file name match (the machine folder may come from a Windows share)
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(folder), ec)) {
            if (up(e.path().filename().string()) != up(names[k])) continue;
            if (accel_[k].load(e.path().string())) ++n;
            break;
        }
    }
    return n;
}

bool ResearchDb::lookup(const std::string& table, const std::string& key_column, long key, const std::string& column,
                        double& v) const {
    auto t = tabs_.find(up(table));
    if (t == tabs_.end()) return false;
    auto kc = t->second.cols.find(up(key_column)), c = t->second.cols.find(up(column));
    if (kc == t->second.cols.end() || c == t->second.cols.end()) return false;
    for (const Row& r : t->second.list) {
        if (r.null[(size_t)kc->second] || std::lround(r.v[(size_t)kc->second]) != key) continue;
        if (r.null[(size_t)c->second]) return false;
        v = r.v[(size_t)c->second];
        return true;
    }
    return false;
}



bool research_part(const ResearchDb& db, const std::string& name, ResearchResult& out, std::string& err) {
    const long id = db.id_of(name);
    if (!id) { out = ResearchResult(); err = name + ": not in the component database"; return false; }
    return research_rows(db, id, PartRows(), out, err);
}


bool research_rows(const ResearchDb& db, long id, const PartRows& over, ResearchResult& out, std::string& err) {
    err.clear();
    // a column of `over` replaces the template's where the template has that table's row (no template: `over` is
    // the part)
    auto row_of = [&](const char* t) { return id ? db.rows(id, t) > 0 : over.has_table(t); };
    auto num = [&](const char* t, const char* c, double& v) {
        bool null = false;
        if (row_of(t) && over.get(t, c, v, null)) return !null;
        return id && db.value(id, t, c, v);
    };
    RecordSource s;
    s.get = [&](const char* t, const char* c, long& v) {
        double d = 0.0;
        if (!num(t, c, d)) return false;
        v = std::lround(d);
        return true;
    };
    s.real = [&](const char* t, const char* c, double& v) { return num(t, c, v); };
    s.text = [&](const char* t, const char* c, std::string& v) {
        if (row_of(t) && over.get_text(t, c, v)) return true;
        return id && db.text(id, t, c, v);
    };
    s.lookup = [&](const char* t, const char* kc, long key, const char* c, long& v) {
        double d = 0.0;
        if (!db.lookup(t, kc, key, c, d)) return false;
        v = std::lround(d);
        return true;
    };
    s.ltext = [&](const char* t, const char* kc, long key, const char* c, std::string& v) { return db.lookup_text(t, kc, key, c, v); };
    s.count = [&](const char* t) { return id ? db.rows(id, t) : (size_t)0; };
    s.row = [&](const char* t, size_t n, const char* c, long& v) {
        double d = 0.0;
        if (!id || !db.value_n(id, t, n, c, d)) return false;
        v = std::lround(d);
        return true;
    };
    s.edb.part_text = [&](const char* t, size_t n, const char* c, std::string& v) { return id && db.text_n(id, t, n, c, v); };
    s.edb.ref_rows = [&](const char* t, const char* kc, long key) { return db.ref_rows(t, kc, key); };
    s.edb.ref_val = [&](const char* t, size_t n, const char* c, long& v) {
        double d = 0.0;
        if (!db.ref_value(t, n, c, d)) return false;
        v = std::lround(d);
        return true;
    };
    s.edb.ref_text = [&](const char* t, size_t n, const char* c, std::string& v) { return db.ref_text(t, n, c, v); };
    s.accel = db.accel_tables();
    return research_with(s, out);
}

}  // namespace pcbp::juki
