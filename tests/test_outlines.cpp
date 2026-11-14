// ============================================================================
// test_outlines.cpp — R16 (REVIEW_V15 §3/§12, specs/OUTLINES.md): board outline provenance
// (Inferred / Confirmed / Imported), migration, no auto-resize, export validation. Synthetic data only.
// ============================================================================
#include "../src/core/project.h"
#include "../src/core/project_io.h"
#include "../src/core/machine_job.h"
#include "../src/core/json.h"
#include <cstdio>
#include <string>

using namespace pcbp;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static Project centroid_only() {
    Project p;
    p.model.points.clear();
    ModelPoint mp; mp.name = "Top SMT"; mp.has_machine = true; mp.machine.exporter_id = "juki.h8h"; mp.machine.side = Side::Top;
    p.model.points.push_back(mp);
    auto mk = [&](const char* r, double x, double y, Feature f = Feature::Component) {
        Component c; c.ref = r; c.pos = {x, y}; c.package = "0603"; c.number = "PN1"; c.feature = f; c.assigned_point = f == Feature::Component ? 0 : -1;
        p.add_component(c);
    };
    mk("R1", 10, 10); mk("R2", 30, 20); mk("FID1", 5, 5, Feature::Fiducial);
    Package pk; pk.name = "0603"; pk.pads = {Pad{{-0.8, 0}, 0.8, 0.9}, Pad{{0.8, 0}, 0.8, 0.9}}; p.packages["0603"] = pk;
    return p;
}
static bool has_msg(const std::vector<ExportIssue>& v, const char* s, ExportIssue::Sev sev) {
    for (auto& i : v) if (i.sev == sev && i.msg.find(s) != std::string::npos) return true;
    return false;
}
static Project roundtrip(const Project& p, bool strip_src) {
    std::string t = project_to_json(p).dump();
    if (strip_src) {   // simulate a pre-R16 file
        for (const char* k : {"\"board_outline_src\"", "\"panel_outline_src\""}) {
            size_t a = t.find(k); if (a == std::string::npos) continue;
            size_t e = t.find(',', a); t.erase(a, e - a + 1);
        }
    }
    json::Value v; std::string err; Project r;
    CHECK(json::Value::parse(t, v, err) && project_from_json(v, r, err));
    return r;
}

int main() {
    {   // centroid only: no frame -> Inferred, export blocked
        Project p = centroid_only();
        CHECK(p.board_frame.empty() && p.board_outline_source() == OutlineSource::Inferred && !p.board_outline_confirmed());
        // R17b (REVIEW_V16 §4): advisory Warning, not a blocker
        auto v = validate_for_export(p, 0);
        CHECK(has_msg(v, "Board outline unconfirmed", ExportIssue::Warning) && !has_msg(v, "Board outline unconfirmed", ExportIssue::Error));
        CHECK(!has_errors(v));
        // a machine setup that demands measured geometry still blocks
        p.model.points[0].machine.options["require_confirmed_outline"] = "1";
        CHECK(has_msg(validate_for_export(p, 0), "Board outline unconfirmed", ExportIssue::Error));
        p.model.points[0].machine.options.erase("require_confirmed_outline");
    }
    {   // R17b: acknowledge without the editor (Use as-is / Don't warn), outline itself untouched
        Project p = centroid_only();
        p.infer_circuit_outline();
        const BBox b0 = p.circuit_frame_bbox();
        CHECK(!p.inferred_outline_acknowledged());
        p.acknowledge_inferred_outline(false);   // Use as-is
        CHECK(p.inferred_outline_acknowledged() && p.circuit_outline_source() == OutlineSource::Inferred);
        BBox b1 = p.circuit_frame_bbox();
        CHECK(b1.x0 == b0.x0 && b1.x1 == b0.x1 && b1.y0 == b0.y0 && b1.y1 == b0.y1);
        auto v = validate_for_export(p, 0);
        CHECK(has_msg(v, "used as-is (acknowledged)", ExportIssue::Info) && !has_msg(v, "Board outline unconfirmed", ExportIssue::Warning));
        // persisted
        Project r = roundtrip(p, false);
        CHECK(r.inferred_outline_acknowledged() && !r.outline_warn_off);
        // the inferred dimensions change (re-inference after an import) -> warns again
        p.comps[1].pos = {80, 50};
        p.infer_circuit_outline();
        CHECK(!p.inferred_outline_acknowledged());
        CHECK(has_msg(validate_for_export(p, 0), "Board outline unconfirmed", ExportIssue::Warning));
        // Don't warn for this project: any inferred size
        p.acknowledge_inferred_outline(true);
        p.comps[1].pos = {90, 60};
        p.infer_circuit_outline();
        CHECK(p.inferred_outline_acknowledged());
        CHECK(has_msg(validate_for_export(p, 0), "warning off for this project", ExportIssue::Info));
        CHECK(roundtrip(p, false).outline_warn_off);
        // confirming later (Export / validation) clears the issue entirely
        p.confirm_outlines();
        v = validate_for_export(p, 0);
        CHECK(!has_msg(v, "Board outline", ExportIssue::Info) && !has_msg(v, "Board outline", ExportIssue::Warning));
        // genuine missing geometry stays blocking: no outline, no placements
        Project e = centroid_only(); e.comps.clear();
        CHECK(has_msg(validate_for_export(e, 0), "Board outline missing", ExportIssue::Error));
    }
    {   // R17b (REVIEW_V16 §3): deleting placements resizes nothing except an Inferred outline
        for (OutlineSource src : {OutlineSource::Confirmed, OutlineSource::Imported}) {
            Project p = centroid_only(); p.set_circuit_frame_rect(BBox(-2, -3, 70, 45)); p.board_outline_src = src;
            p.set_panel_frame_rect(BBox(-10, -10, 90, 60)); p.panel_outline_src = src;
            p.comps.erase(p.comps.begin());   // delete placements
            p.comps.erase(p.comps.begin());
            p.infer_circuit_outline();        // what an import / refresh would call
            const BBox c = p.circuit_frame_bbox(), q = p.panel_frame_bbox();
            CHECK(c.x0 == -2 && c.y0 == -3 && c.x1 == 70 && c.y1 == 45 && p.circuit_outline_source() == src);
            CHECK(q.x0 == -10 && q.x1 == 90 && p.board_outline_source() == src);
            CHECK(p.board_bbox().x1 == 70);
        }
        {   // frozen Inferred outline: a delete alone does not move it; only an explicit refresh does
            Project p = centroid_only(); p.infer_circuit_outline();
            const BBox b0 = p.circuit_frame_bbox();
            p.comps.erase(p.comps.begin() + 1);
            CHECK(p.circuit_frame_bbox().x1 == b0.x1 && p.circuit_outline_source() == OutlineSource::Inferred);
            p.infer_circuit_outline();
            CHECK(p.circuit_frame_bbox().x1 < b0.x1 && p.circuit_outline_source() == OutlineSource::Inferred);
        }
        {   // no frame at all: the live extent is shown and reported as Inferred
            Project p = centroid_only();
            const double w0 = p.board_bbox().w();
            p.comps.erase(p.comps.begin() + 1);
            CHECK(p.board_bbox().w() < w0 && p.board_outline_source() == OutlineSource::Inferred);
        }
    }
    {   // frozen inferred outline follows imports, but never once confirmed
        Project p = centroid_only();
        p.infer_circuit_outline();
        CHECK(p.circuit_outline_source() == OutlineSource::Inferred && p.circuit_frame_is_placement_extent());
        BBox b0 = p.circuit_frame_bbox();
        p.comps[1].pos = {60, 40};
        p.infer_circuit_outline();   // refresh (import path): inferred may follow
        CHECK(p.circuit_frame_bbox().x1 > b0.x1);
        p.confirm_outlines();
        CHECK(p.board_outline_source() == OutlineSource::Confirmed);
        BBox b1 = p.circuit_frame_bbox();
        p.comps.erase(p.comps.begin() + 1);   // delete / move placements
        p.comps[0].pos = {100, 100};
        p.infer_circuit_outline();
        BBox b2 = p.circuit_frame_bbox();
        CHECK(b2.x0 == b1.x0 && b2.x1 == b1.x1 && b2.y0 == b1.y0 && b2.y1 == b1.y1);
        CHECK(!has_msg(validate_for_export(p, 0), "Board outline unconfirmed", ExportIssue::Error) && !has_msg(validate_for_export(p, 0), "Board outline unconfirmed", ExportIssue::Warning));
        // imported outlines are untouched as well
        Project q = centroid_only(); q.set_circuit_frame_rect(BBox(0, 0, 50, 40)); q.board_outline_src = OutlineSource::Imported;
        q.infer_circuit_outline();
        CHECK(q.circuit_frame_bbox().x1 == 50 && q.board_outline_source() == OutlineSource::Imported);
    }
    {   // persistence + migration
        Project p = centroid_only(); p.infer_circuit_outline();
        CHECK(roundtrip(p, false).board_outline_src == OutlineSource::Inferred);
        p.board_outline_src = OutlineSource::Imported;
        CHECK(roundtrip(p, false).board_outline_src == OutlineSource::Imported);
        // old file, outline == placement extents (+2 mm) -> Inferred
        p.board_outline_src = OutlineSource::Confirmed;
        CHECK(roundtrip(p, true).board_outline_src == OutlineSource::Inferred);
        // old file, any other outline -> Confirmed
        p.set_circuit_frame_rect(BBox(0, 0, 50, 40));
        CHECK(roundtrip(p, true).board_outline_src == OutlineSource::Confirmed);
        // non-rect (KiCad-like) outline stays Confirmed
        p.board_frame = {{{0, 0}, {50, 0}, {50, 30}, {40, 40}, {0, 40}, {0, 0}}};
        CHECK(roundtrip(p, true).board_outline_src == OutlineSource::Confirmed);
        // panel frame decides the board size
        Project r = centroid_only(); r.set_panel_frame_rect(BBox(-5, -5, 80, 60));
        CHECK(r.board_outline_confirmed());
        r.panel_outline_src = OutlineSource::Inferred;
        CHECK(!r.board_outline_confirmed());
    }
    {   // §12 export review items
        Project p = centroid_only(); p.set_circuit_frame_rect(BBox(0, 0, 50, 40));
        auto v = validate_for_export(p, 0);
        CHECK(!has_errors(v));
        // unresolved package (placeholder without graphics)
        p.packages["SOT23"] = Package{}; p.packages["SOT23"].name = "SOT23"; p.comps[1].package = "SOT23";
        v = validate_for_export(p, 0);
        CHECK(has_msg(v, "unresolved (no graphics)", ExportIssue::Warning) && has_msg(v, "1 unresolved package(s)", ExportIssue::Warning));
        // top / bottom separation
        p.comps[0].side = Side::Bottom;
        v = validate_for_export(p, 0);
        CHECK(has_msg(v, "Top/bottom separation", ExportIssue::Error));
        p.comps[0].side = Side::Top;
        // transform consistency
        p.model.points[0].machine.orientation = 45;
        CHECK(has_msg(validate_for_export(p, 0), "Transform: board rotation", ExportIssue::Error));
        p.model.points[0].machine.orientation = 0;
        p.model.points[0].machine.side = Side::Bottom;
        for (auto& c : p.comps) c.side = Side::Bottom;
        v = validate_for_export(p, 0);
        CHECK(has_msg(v, "bottom-side program without board flip", ExportIssue::Warning) && has_msg(v, "Transform: the model point", ExportIssue::Warning));
    }
    std::printf("test_outlines: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
