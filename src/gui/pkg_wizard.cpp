// ============================================================================
// pkg_wizard.cpp — R16b: routes "Build from Gerber" / "Package Acquisition Wizard" to the Gerber tab.
// ============================================================================
#include "pkg_wizard.h"
#include "../core/package_resolve.h"

namespace pcbp::gui {
using namespace pcbp::app;
namespace {
std::weak_ptr<Doc> g_req;
std::string g_start;
}

void pkg_wizard_open(const DocPtr& d, const std::string& start_pkg) {
    if (!d) return;
    if (d->prj.layers.empty()) {
        status("Build from Gerber needs Gerber layers (paste or copper): import the Gerbers first (File > Import Gerber).", ToastKind::Warning);
        return;
    }
    if (unresolved_packages(d->prj).empty()) { status("Every package used by a placement has graphics.", ToastKind::Info); return; }
    g_req = d;
    g_start = start_pkg;
    activate_view(d, ViewKind::Gerber);
    request_redraw(3);
}

bool pkg_wizard_request_take(const Doc* d, std::string& start_pkg) {
    auto p = g_req.lock();
    if (!p || p.get() != d) return false;
    g_req.reset();
    start_pkg = g_start;
    g_start.clear();
    return true;
}
} // namespace pcbp::gui
