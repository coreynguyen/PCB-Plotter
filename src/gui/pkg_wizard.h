#pragma once
// R16b: Gerber-assisted package acquisition wizard routing (the tool itself lives in the Gerber tab,
// gerber_workspace.cpp; state in Project::pkg_wizard, core/package_resolve.h).
#include "gapp.h"
#include <map>
#include <string>

namespace pcbp::gui {
// Opens the Gerber tab of d with the wizard on `start_pkg` ("" = resume where it was).
void pkg_wizard_open(const app::DocPtr& d, const std::string& start_pkg);
// Taken by the Gerber workspace of that document on its next frame.
bool pkg_wizard_request_take(const app::Doc* d, std::string& start_pkg);
// R17a: the master package library (package_library.cpp), for auto_resolve_packages().
const std::map<std::string, Package>* master_package_library();
} // namespace pcbp::gui
