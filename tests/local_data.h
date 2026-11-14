// local_data.h — paths to private real-world test inputs (customer projects, vendor samples).
// They are never committed: keys resolve through tests/local_data.map (gitignored, "key=path" per
// line) or the file named by $PCBP_LOCAL_DATA. An unmapped key yields "" and the test that needs it
// skips, exactly as when the file is absent.
#pragma once
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace pcbp_test {

inline const std::map<std::string, std::string>& local_map() {
    static const std::map<std::string, std::string> m = [] {
        std::map<std::string, std::string> r;
        const char* env = std::getenv("PCBP_LOCAL_DATA");
        for (const std::string p : {std::string(env ? env : ""), std::string("tests/local_data.map"), std::string("../tests/local_data.map")}) {
            if (p.empty()) continue;
            std::ifstream f(p);
            if (!f) continue;
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                const size_t eq = line.find('=');
                if (eq != std::string::npos) r.emplace(line.substr(0, eq), line.substr(eq + 1));
            }
            break;
        }
        return r;
    }();
    return m;
}

// Path for `key`, or "" when not mapped.
inline std::string local(const char* key) {
    auto it = local_map().find(key);
    return it == local_map().end() ? std::string() : it->second;
}

// '|' separated list for `key`.
inline std::vector<std::string> local_list(const char* key) {
    std::vector<std::string> out;
    const std::string s = local(key);
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find('|', a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

} // namespace pcbp_test
