// ============================================================================
// align_tool.cpp — Board > Align CAD to Gerber (R13, reworked in R14).
//
// R14 (specs/PANELIZE.md §1): alignment is a Gerber-tab tool ("Align Plot to Gerber") so the operator
// works on the real Gerber rendering: pick the reference component from the plot (overlaid), box its
// pads on a Gerber layer, choose rotation (0/90/180/270/any, mirror) and direction (move the Gerbers
// to the plot, or the plot to the Gerbers), Apply = one undo step. The Gerber-layer-to-Gerber-layer
// alignment stays a separate Gerber-tab tool (Align Layer).
// This file only routes the menu / toolbar command to the Gerber tab: the request is picked up by the
// Gerber workspace of that document on its next frame.
// ============================================================================
#include "gapp.h"
#include "../app/commands.h"
#include <cstdlib>

namespace pcbp::gui {
using namespace pcbp::app;

namespace {
std::weak_ptr<Doc> g_req;
}

// taken by GerberWorkspace::draw (gerber_workspace.cpp)
bool plot_align_request_take(const Doc* d) {
    auto p = g_req.lock();
    if (!p || p.get() != d) return false;
    g_req.reset();
    return true;
}

void align_cad_gerber_open(const DocPtr& d) {
    if (!d) return;
    if (d->prj.layers.empty()) {
        status("Align Plot to Gerber needs Gerber layers: import the panel Gerbers first (File > Import Gerber).", ToastKind::Warning);
        return;
    }
    g_req = d;
    activate_view(d, ViewKind::Gerber);
    request_redraw(3);
}

void align_cad_gerber_draw() {   // the tool lives in the Gerber tab now
    // R14 wine verification: PCBP_TEST_R14 switches to the Gerber tab once (the workspace runs the script)
    static int n = -1;
    if (n == -1) n = (getenv("PCBP_TEST_R14") || getenv("PCBP_TEST_R17B")) ? 0 : -2;   // R17b hook too
    if (n >= 0 && ++n == 30) { post_command(app::ID_WIN_GERBER); n = -2; }
}

// Board / Circuit Outline dialog, "Measure on the Gerber": the Gerber tab of that document opens its
// outline tool (box an outline graphic, or click two snapped corners).
namespace { const Doc* g_out_doc = nullptr; bool g_out_board = false; }
void gerber_outline_tool_request(const Doc* d, bool board) { g_out_doc = d; g_out_board = board; request_redraw(3); }
bool gerber_outline_tool_take(const Doc* d, bool& board) {
    if (!d || d != g_out_doc) return false;
    board = g_out_board; g_out_doc = nullptr;
    return true;
}   // the tool lives in the Gerber tab now

} // namespace pcbp::gui
