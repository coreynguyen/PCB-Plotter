#!/usr/bin/env bash
set -euo pipefail
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/test.cpp" <<'CPP'
#include "plugins/builtin_plugins.cpp"
#include <cassert>
#include <iostream>
#include <fstream>
#include <chrono>

int main() {
    using namespace pcbp;
    MachineJob job;
    job.program_name = "Demo";
    JobPlacement p;
    p.ref = "R1"; p.part = "PN"; p.package = "0603";
    p.x = 25.4; p.y = -12.7; p.rot = 90;
    job.placements = {p, p};
    job.placements.back().skip = true;
    job.fiducials = {{"FID1", 0, 0}, {"FID2", 25.4, 0}};
    std::vector<uint8_t> out;
    std::string err;
    auto run = [&](VendorFormat f, const Options& o = {}) {
        out.clear();
        assert(export_vendor(job, o, f, out, err));
        return std::string(out.begin(), out.end());
    };
    assert(run(VendorFormat::Janets) == "R1,25.400,-12.700,90,PN\r\n");
    assert(run(VendorFormat::Janets, {{"units","inch"}, {"angle","CW"}}) ==
           "R1,1.0000,-0.5000,270,PN\r\n");
    assert(run(VendorFormat::Samsung).find("\"R1\"\t1.000\t-0.500\t0\t90\tNONE\t0\t0\t0\t0\t1\t0\t\"PN\"\t\"\"\t\"\"\r\n") != std::string::npos);
    assert(run(VendorFormat::Directline) == "B Demo\r\nB Ref1 = FID1\r\nB Ref2 = FID2\r\nU  0.1\r\nC1       0       0   10000       0\r\nM1   10000   -5000  90 PN\r\nM2R1\r\nE\r\n");
    assert(run(VendorFormat::TPSys).find("F8 25400 -12700 90000 0 N N PN\r\nF9 R1\r\n") != std::string::npos);
    assert(run(VendorFormat::Flat).find("R1\tPN\t0603\t25.4000\t-12.7000\t90\tTOP\t\t\r\n") != std::string::npos);
    assert(run(VendorFormat::Dp, {{"units","inch"}, {"target","Multroniks"}}).find(" #C PN ; R1     ; 90.000; 1.000; -0.500\r\n") != std::string::npos);
    assert(run(VendorFormat::Ftip).find("R1= ( PN,0603,,,25.40,-12.70,90,TOP)\r\n") != std::string::npos);
    assert(run(VendorFormat::Quadra).find("#R1#,1000,-500,90,#PXY#,#0603#,#TOP#,1,T,") == 0);
    assert(run(VendorFormat::Dima).find("[FileID 1.00]") == 0);
    assert(run(VendorFormat::Dima).find("|  25.400| -12.700|  90.00|") != std::string::npos);
    job.placements[0].smt = false;
    assert(run(VendorFormat::Tdk).find("N:10 X:2540 Y:-1270 P:PN M:1 C:90") != std::string::npos);
    out.clear();
    job.placements[0].smt = true;
    assert(!export_vendor(job, {}, VendorFormat::Tdk, out, err) && out.empty());
    out.clear();
    assert(!export_vendor(job, {}, VendorFormat::Blakell, out, err) && out.empty());
    job.placements[0].has_ordered_pin_pair = true;
    job.placements[0].pin1 = {0, 0};
    job.placements[0].pin2 = {2.54, 0};
    assert(run(VendorFormat::Blakell) == "PN\tR1\t0\t0\t100\t0\t100\t1\t0603\r\n");
    out.clear();
    job.placements[0].rot = 23;
    assert(!export_vendor(job, {}, VendorFormat::Samsung, out, err) && out.empty());
    job.placements[1].skip = false;
    assert(!export_vendor(job, {}, VendorFormat::Janets, out, err) && out.empty());
    job.placements[1].skip = true;
    job.fiducials.pop_back();
    assert(!export_vendor(job, {}, VendorFormat::Directline, out, err) && out.empty());
    Options cmp = {{"cmp.part", "PN"}, {"cmp.C01", "A"}, {"cmp.C02", "B"},
                   {"cmp.C03", "1 2 3"}, {"cmp.C05", "C"}, {"cmp.C07", "D"}};
    out.clear();
    assert(export_mydata_cmp(job, cmp, out, err));
    assert(std::string(out.begin(), out.end()) ==
           "#\r\nC00 PN\r\nC01 A\r\nC02 B\r\nC03 1 2 3\r\nC05 C\r\nC07 D\r\n");
    out.clear();
    cmp["cmp.C03"] = "1 2";
    assert(!export_mydata_cmp(job, cmp, out, err) && out.empty());
    job.placements[1].part = "PN2";
    job.placements[1].skip = false;
    out.clear();
    assert(export_mydata_cmp_batch(job, {{"cmp.rows", "PN;A;B;1 2 3;C;D|PN2;A;B;4 5 6;C;D"}}, out, err));
    assert(std::string(out.begin(), out.end()).find("#\r\nC00 PN2\r\n") != std::string::npos);
    out.clear();
    assert(!export_mydata_cmp_batch(job, {{"cmp.rows", "PN;A;B;1 2 3;C;D|PN;A;B;1 2 3;C;D"}}, out, err) && out.empty());
    job.placements[0].rot = 90;
    job.placements[1].ref = "R2";
    out.clear();
    assert(export_universal_hsp(job, {}, out, err));
    assert(std::string(out.begin(), out.end()).find("025.40,-12.70,090.00,101,") != std::string::npos);
    out.clear();
    job.placements[0].rot = 45;
    assert(!export_universal_hsp(job, {}, out, err) && out.empty());
    job.board_w = 100; job.board_h = 80;
    job.fiducials = {{"F1", 0, 0}, {"F2", 90, 70}};
    for (auto& p : job.placements) {
        p.has_scan_pair = true; p.scan1 = {1, 2}; p.scan2 = {3, 4};
    }
    out.clear();
    assert(export_mascot(job, {}, out, err));
    assert(std::string(out.begin(), out.end()).find("MAS Assembly File,1.6,") == 0);
    assert(std::string(out.begin(), out.end()).find(",60,,0,1000,2000,3000,4000,") != std::string::npos);
    out.clear();
    job.placements[0].has_scan_pair = false;
    assert(!export_mascot(job, {}, out, err) && out.empty());
    const std::string gen_path = "/tmp/pcbp_vendor_gen_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    { std::ofstream gen(gen_path); gen << "# Unknown item \"PN2\"\n"; }
    out.clear();
    assert(export_mydata_cmp_batch(job, {{"cmp.gen_file", gen_path},
        {"cmp.rows", "PN;A;B;1 2 3;C;D|PN2;A;B;4 5 6;C;D"}}, out, err));
    assert(std::string(out.begin(), out.end()).find("C00 PN\r") == std::string::npos);
    std::remove(gen_path.c_str());
    { std::ofstream uni(gen_path); uni << "PN\tR\t4\tA\n"; }
    out.clear();
    assert(export_universal_radial(job, {{"uni.path", gen_path}}, out, err));
    assert(std::string(out.begin(), out.end()).find("0003>     1000    -0500") != std::string::npos);
    std::remove(gen_path.c_str());
    { std::ofstream uni(gen_path); uni << "PN\tV\t4\tC\nPN2\tV\t2\tD\n"; }
    job.placements[0].has_scan_pair = true;
    job.placements[0].scan1 = {0, 0}; job.placements[0].scan2 = {2.54, 0};
    job.placements[1].scan1 = {0, 0}; job.placements[1].scan2 = {0, 2.54};
    out.clear();
    assert(export_universal_radial(job, {{"uni.path", gen_path}}, out, err, true, true));
    const std::string vcd(out.begin(), out.end());
    assert(vcd.find("0003> +01000 -00500 +00100 +00000 +00004 +00000 +00000 C I") != std::string::npos);
    assert(vcd.find("0004> +00000 +00000 +00000 +00000 +00000 +00000 +00000 A Z") != std::string::npos);
    assert(vcd.find("0005> +00500 +01000 +00100 +00000 +00002 +00000 +00000 D I") != std::string::npos);
    std::remove(gen_path.c_str());
    job.placements[0].part = "PN<&";
    out.clear();
    assert(export_juki_iss(job, {{"iss.transport", "RTOL"}}, out, err));
    const std::string iss(out.begin(), out.end());
    assert(iss.find("<headPlacement total=\"2\"") != std::string::npos);
    assert(iss.find("<componentName>PN&lt;&amp;</componentName>") != std::string::npos);
    assert(iss.find("<transferDirection>RTOL</transferDirection>") != std::string::npos);
    out.clear();
    job.placements[0].circuit = 2;
    assert(!export_juki_iss(job, {}, out, err) && out.empty());
    std::cout << "44 serializer checks passed\n";
}
CPP
g++ -std=c++20 -O0 -ffunction-sections -fdata-sections -Isrc -Isdk "$tmp/test.cpp" \
    src/plugins/plugin_registry.cpp src/io/io_util.cpp src/core/project.cpp src/core/fileio.cpp \
    -Wl,--gc-sections -o "$tmp/test"
"$tmp/test"
