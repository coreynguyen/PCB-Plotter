#pragma once
// ============================================================================
// xml_lite.h — tiny non-validating XML DOM parser (pcbp::io), portable.
//
// Handles the prolog, comments, processing instructions, DOCTYPE (skipped,
// incl. internal subset), CDATA, the five predefined entities and numeric
// character references (emitted as UTF-8). Namespaces are not resolved;
// `local()` strips a "prefix:". Nesting depth is capped (default 512).
// Enough for IPC-2581 and Eagle .brd/.sch.
// ============================================================================
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pcbp::io {

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> children;
    std::string text;                       // concatenated character data

    std::string_view local() const;         // name without namespace prefix
    // Attribute lookup (case-sensitive; falls back to case-insensitive).
    const std::string* find_attr(std::string_view k) const;
    std::string attr(std::string_view k, const std::string& def = {}) const;
    double attr_num(std::string_view k, double def = 0) const;
    bool has_attr(std::string_view k) const { return find_attr(k) != nullptr; }
    // First child by local name (case-insensitive), nullptr if none.
    const XmlNode* child(std::string_view lname) const;
    std::vector<const XmlNode*> kids(std::string_view lname) const;
    // Depth-first search for first descendant with local name.
    const XmlNode* find(std::string_view lname) const;
    void find_all(std::string_view lname, std::vector<const XmlNode*>& out) const;
};

bool xml_parse(std::string_view text, XmlNode& root, std::string& err, int max_depth = 512);

} // namespace pcbp::io
