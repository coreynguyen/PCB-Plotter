#pragma once
// ============================================================================
// project_io.h — native PCB Plotter project format (*.pcbp, UTF-8 JSON).
// The same JSON schema is the payload of the plugin C ABI (sdk/pcbp_plugin.h).
// ============================================================================
#include "json.h"
#include "project.h"
#include <string>

namespace pcbp {

constexpr int kProjectFormatVersion = 1;

json::Value project_to_json(const Project& p, bool include_layers_content = true);
bool project_from_json(const json::Value& v, Project& p, std::string& err);

json::Value package_to_json(const Package& p);
Package package_from_json(const json::Value& v);
json::Value gprim_to_json(const GPrim& g);
GPrim gprim_from_json(const json::Value& v);

// temp + flush + atomic replace (MoveFileEx REPLACE_EXISTING|WRITE_THROUGH / rename)
bool atomic_write_file(const std::string& path, const void* data, size_t n, std::string& err);
bool save_project(const Project& p, const std::string& path, std::string& err);
bool load_project(const std::string& path, Project& p, std::string& err);

} // namespace pcbp
