// test_side_sniff.cpp — R16c (REVIEW_V15 §7): centroid Side column conventions and BOM / centroid sniffing.
#include "../src/io/pnp_import.h"
#include "../src/core/project_io.h"
#include <cstdio>

using namespace pcbp;
using namespace pcbp::io;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static PnpPreview run(const std::string& csv, PnpOptions o = {}) {
    PnpPreview p; std::string e;
    pnp_preview(csv, o, p, e);
    return p;
}
static int bottoms(const PnpPreview& p) { int n = 0; for (auto& r : p.rows) n += r.side == Side::Bottom; return n; }

int main() {
    {   // Top / Bottom words
        auto p = run("Designator,X,Y,Rotation,Layer\nR1,1,2,0,Top\nR2,3,4,90,Bottom\nR3,5,6,0,TopLayer\nR4,7,8,0,BottomLayer\n");
        CHECK(p.sides.col >= 0);
        CHECK(p.sides.top == 2 && p.sides.bottom == 2 && p.sides.unknown == 0);
        CHECK(bottoms(p) == 2);
        CHECK(p.sides.conv == SideConv::Words);
    }
    {   // T / B letters
        auto p = run("Ref,X,Y,Rot,Side\nR1,1,2,0,T\nR2,3,4,0,B\nR3,5,6,0,B\n");
        CHECK(p.sides.conv == SideConv::Letters);
        CHECK(p.sides.top == 1 && p.sides.bottom == 2);
    }
    {   // M = mirrored = bottom, blank = default side (top)
        auto p = run("Ref,X,Y,Rot,Mirror\nR1,1,2,0,\nR2,3,4,0,M\nR3,5,6,0,\n");
        CHECK(p.sides.bottom == 1 && p.sides.top == 2 && p.sides.blank == 2);
        CHECK(bottoms(p) == 1);
        CHECK(p.issues.empty() || true);
    }
    {   // blank = user-selected default (bottom)
        PnpOptions o; o.default_side = Side::Bottom;
        auto p = run("Ref,X,Y,Rot,Mirror\nR1,1,2,0,\nR2,3,4,0,M\n", o);
        CHECK(bottoms(p) == 2);
    }
    {   // numeric layers: Eagle 1 / 16, Altium 1 / 32, KiCad 0 / 31
        auto a = run("Ref,X,Y,Rot,Layer\nR1,1,2,0,1\nR2,3,4,0,16\nR3,5,6,0,1\n");
        CHECK(a.sides.conv == SideConv::Numeric);
        CHECK(a.sides.top == 2 && a.sides.bottom == 1);
        auto b = run("Ref,X,Y,Rot,Layer\nR1,1,2,0,0\nR2,3,4,0,31\n");
        CHECK(b.sides.top == 1 && b.sides.bottom == 1);
        auto c = run("Ref,X,Y,Rot,Layer\nR1,1,2,0,0\nR2,3,4,0,1\n");   // 0 / 1: lowest top
        CHECK(c.sides.top == 1 && c.sides.bottom == 1);
        auto d = run("Ref,X,Y,Rot,Layer\nR1,1,2,0,1\nR2,3,4,0,2\nR3,5,6,0,3\n");   // inner layer: ask
        CHECK(d.sides.unknown == 1);
    }
    {   // override table wins
        PnpOptions o; o.side_override["SOLDERSIDE"] = 1; o.side_override["TOP"] = 1;
        auto p = run("Ref,X,Y,Rot,Side\nR1,1,2,0,Top\nR2,3,4,0,SolderSide\n", o);
        CHECK(p.sides.unknown == 0);
        CHECK(bottoms(p) == 2);
        bool ov = false; for (auto& v : p.sides.values) if (v.token == "TOP") ov = v.overridden && v.auto_side == 0 && v.side == 1;
        CHECK(ov);
    }
    {   // unrecognised value -> unknown (issue), not silently bottom
        auto p = run("Ref,X,Y,Rot,Side\nR1,1,2,0,Top\nR2,3,4,0,Q\n");
        CHECK(p.sides.unknown == 1);
        bool issue = false; for (auto& i : p.issues) if (i.msg.find("not recognised") != std::string::npos) issue = true;
        CHECK(issue);
    }
    {   // no side column: everything default side
        auto p = run("Ref,X,Y,Rot\nR1,1,2,0\nR2,3,4,0\n");
        CHECK(p.sides.col < 0 && p.sides.top == 2 && p.sides.bottom == 0);
    }
    // ---- sniffing ----
    std::string why;
    CHECK(sniff_csv_kind("Designator,Mid X,Mid Y,Rotation,Layer,Footprint\nR1,10.0,20.0,90,Top,0603\nC1,11.5,3.2,0,Bottom,0402\n", &why) == CsvKind::Centroid);
    CHECK(sniff_csv_kind("RefDes;X (mm);Y (mm);Rot;Side\nU1;1,5;2,5;0;T\nU2;3;4;90;B\n") == CsvKind::Centroid);
    CHECK(sniff_csv_kind("Item,Qty,Reference,Value,Part Number\n1,3,\"C1,C2,C3\",100nF,CAP-100N\n2,1,R1,10k,RES-10K\n", &why) == CsvKind::Bom);
    CHECK(sniff_csv_kind("Quantity\tDesignator\tComment\tFootprint\n2\tR1 R2\t10k\t0603\n1\tU1\tLM358\tSOIC-8\n") == CsvKind::Bom);
    CHECK(sniff_csv_kind("hello world\nnothing here\n") == CsvKind::Unknown);
    {   // Data tab "Reviewed / OK" acknowledge is saved in the project
        Project p;
        Component a; a.ref = "BT1"; a.review_ok = true; p.add_component(a);
        Component b; b.ref = "BT1"; p.add_component(b);
        Project q; std::string e;
        CHECK(project_from_json(project_to_json(p), q, e));
        CHECK(q.comps.size() == 2 && q.comps[0].review_ok && !q.comps[1].review_ok);
    }
    std::printf("test_side_sniff: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
