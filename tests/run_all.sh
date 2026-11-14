#!/bin/bash
# Builds and runs the portable unit tests on Linux (needs g++ >= 12; test_jet also needs mdbtools as oracle).
set -e
cd "$(dirname "$0")/.."
B=build/tests; mkdir -p $B
CXX="g++ -std=c++20 -O1 -Isrc -Isdk"
SHEET="$(ls src/core/sheet*.cpp src/core/formula*.cpp | tr '\n' ' ')"
CORE="src/core/project.cpp src/core/board_image.cpp src/core/project_io.cpp src/core/json.cpp src/core/fileio.cpp src/core/machine_job.cpp src/core/plot_align.cpp src/core/arrays.cpp src/core/machine_assign.cpp"  # R24b machine_assign (machine_job uses it)
$CXX tests/test_jet.cpp src/io/jet_mdb.cpp src/core/fileio.cpp -o $B/test_jet
$CXX tests/test_gerber.cpp src/io/gerber.cpp src/io/digitize.cpp $CORE -o $B/test_gerber
$CXX tests/test_importers.cpp src/io/text_import.cpp src/io/text_table.cpp src/io/centroid_import.cpp src/io/bom_import.cpp src/io/gencad.cpp src/io/ipc356.cpp src/io/io_util.cpp $CORE -o $B/test_importers
$CXX tests/test_text_import.cpp src/io/text_import.cpp src/io/pnp_import.cpp src/io/bom_import2.cpp src/io/m1_bom.cpp src/io/bom_source.cpp src/io/xlsx.cpp src/io/ods.cpp src/io/deflate.cpp src/io/archive.cpp src/io/inflate.cpp src/io/xml_lite.cpp $SHEET src/io/centroid_import.cpp src/io/bom_import.cpp src/io/text_table.cpp src/io/io_util.cpp $CORE -o $B/test_text_import
$CXX -O2 tests/test_refexpand.cpp src/io/text_import.cpp src/io/io_util.cpp src/core/json.cpp src/core/fileio.cpp -o $B/test_refexpand
$CXX tests/test_placement_check.cpp src/core/placement_check.cpp src/core/package_edit.cpp src/core/project.cpp -o $B/test_placement_check
$CXX tests/test_panelize.cpp src/core/panelize.cpp src/core/project.cpp -o $B/test_panelize
$CXX tests/test_arrays.cpp src/core/panelize.cpp $CORE -o $B/test_arrays   # R22 REVIEW_V21 §1-§3: linked instances, per-instance outline checks, units, delete array, no generated outlines
$CXX tests/test_plot_align.cpp src/core/panelize.cpp src/io/juki_writer.cpp $CORE -o $B/test_plot_align
$CXX tests/test_juki.cpp src/io/juki_writer.cpp $CORE -o $B/test_juki
$CXX tests/test_cc_archive.cpp src/io/cc_archive.cpp src/io/inflate.cpp $CORE -o $B/test_cc_archive
if [ -f tests/test_plugins.cpp ]; then
  $CXX tests/test_plugins.cpp src/plugins/builtin_plugins.cpp src/plugins/juki_import_plugin.cpp src/plugins/cad_formats_v2.cpp src/plugins/viewmate_plugin.cpp src/plugins/plugin_registry.cpp $(ls src/io/*.cpp) $SHEET $CORE -o $B/test_plugins
fi
if [ -f tests/test_cpf_real.cpp ]; then
  $CXX tests/test_cpf_real.cpp src/core/placement_check.cpp src/core/package_edit.cpp src/plugins/builtin_plugins.cpp src/plugins/juki_import_plugin.cpp src/plugins/cad_formats_v2.cpp src/plugins/viewmate_plugin.cpp src/plugins/plugin_registry.cpp $(ls src/io/*.cpp) $SHEET $CORE -o $B/test_cpf_real
fi
$CXX tests/test_juki_import.cpp src/plugins/builtin_plugins.cpp src/plugins/juki_import_plugin.cpp src/plugins/cad_formats_v2.cpp src/plugins/viewmate_plugin.cpp src/plugins/plugin_registry.cpp $(ls src/io/*.cpp) $SHEET $CORE -o $B/test_juki_import
$CXX tests/test_pnp_program.cpp src/io/pnp_program.cpp src/io/juki_writer.cpp $CORE -o $B/test_pnp_program
$CXX tests/test_cad_v2.cpp src/plugins/builtin_plugins.cpp src/plugins/juki_import_plugin.cpp src/plugins/cad_formats_v2.cpp src/plugins/viewmate_plugin.cpp src/plugins/plugin_registry.cpp $(ls src/io/*.cpp) $SHEET $CORE -o $B/test_cad_v2
$CXX tests/test_flexdb.cpp src/plugins/builtin_plugins.cpp src/plugins/juki_import_plugin.cpp src/plugins/cad_formats_v2.cpp src/plugins/viewmate_plugin.cpp src/plugins/plugin_registry.cpp $(ls src/io/*.cpp) $SHEET $CORE -o $B/test_flexdb
$CXX tests/test_origin.cpp src/plugins/builtin_plugins.cpp src/plugins/juki_import_plugin.cpp src/plugins/cad_formats_v2.cpp src/plugins/viewmate_plugin.cpp src/plugins/plugin_registry.cpp $(ls src/io/*.cpp) $SHEET $CORE -o $B/test_origin
$CXX tests/test_sheet.cpp $SHEET src/io/xlsx.cpp src/io/deflate.cpp src/io/archive.cpp src/io/inflate.cpp src/io/xml_lite.cpp src/io/text_import.cpp src/io/text_table.cpp src/io/io_util.cpp src/core/fileio.cpp src/core/json.cpp -o $B/test_sheet
$CXX -O2 tests/test_sheet_engine.cpp $SHEET src/io/xlsx.cpp src/io/ods.cpp src/io/deflate.cpp src/io/archive.cpp src/io/inflate.cpp src/io/xml_lite.cpp src/io/text_import.cpp src/io/text_table.cpp src/io/io_util.cpp src/core/fileio.cpp src/core/json.cpp -o $B/test_sheet_engine
$CXX tests/test_content_detect.cpp src/core/content_detect.cpp src/io/archive.cpp src/io/inflate.cpp src/io/deflate.cpp src/io/gerber.cpp src/io/juki_import.cpp src/io/juki_writer.cpp $CORE -o $B/test_content_detect
PLUG="src/plugins/builtin_plugins.cpp src/plugins/juki_import_plugin.cpp src/plugins/cad_formats_v2.cpp src/plugins/viewmate_plugin.cpp src/plugins/plugin_registry.cpp"
$CXX tests/test_viewmate_bin.cpp $PLUG $(ls src/io/*.cpp) $SHEET $CORE -o $B/test_viewmate_bin
$CXX tests/test_migrate.cpp src/core/migrate.cpp $PLUG $(ls src/io/*.cpp) $SHEET $CORE -o $B/test_migrate
$CXX tests/test_package_edit.cpp src/core/package_edit.cpp src/core/pkg_library.cpp $PLUG $(ls src/io/*.cpp) $SHEET $CORE -o $B/test_package_edit
$CXX tests/test_sides.cpp src/core/sides.cpp src/core/placement_check.cpp src/core/package_edit.cpp $CORE -o $B/test_sides
$CXX tests/test_angles.cpp src/core/group_ops.cpp $PLUG $(ls src/io/*.cpp) $SHEET $CORE -o $B/test_angles
$CXX tests/test_odb.cpp $PLUG $(ls src/io/*.cpp) $SHEET $CORE -o $B/test_odb
$CXX tests/test_mail.cpp src/io/mime.cpp src/io/mail_eml.cpp src/io/mail_msg.cpp src/io/rtf.cpp src/io/html_rich.cpp src/io/docx.cpp src/io/archive.cpp src/io/inflate.cpp src/io/xml_lite.cpp src/io/io_util.cpp -o $B/test_mail
$CXX tests/test_view_cache.cpp -o $B/test_view_cache
$CXX tests/test_autosave.cpp src/core/autosave.cpp $CORE -o $B/test_autosave   # R12 recovery snapshots / atomic save
$CXX tests/test_allegro_brd.cpp $PLUG $(ls src/io/*.cpp) $SHEET $CORE -o $B/test_allegro_brd   # R14b Allegro binary .brd
$CXX tests/test_m1_bom.cpp src/io/m1_bom.cpp src/io/text_import.cpp src/io/bom_import2.cpp src/io/bom_source.cpp src/io/xlsx.cpp src/io/ods.cpp src/io/deflate.cpp src/io/archive.cpp src/io/inflate.cpp src/io/xml_lite.cpp $SHEET src/io/bom_import.cpp src/io/text_table.cpp src/io/io_util.cpp $CORE -o $B/test_m1_bom   # R14d M1 ERP BOM profile
$CXX tests/test_bom_review.cpp src/io/bom_review.cpp src/io/m1_bom.cpp src/io/text_import.cpp src/io/bom_import2.cpp src/io/bom_source.cpp src/io/xlsx.cpp src/io/ods.cpp src/io/deflate.cpp src/io/archive.cpp src/io/inflate.cpp src/io/xml_lite.cpp $SHEET src/io/bom_import.cpp src/io/text_table.cpp src/io/io_util.cpp $CORE -o $B/test_bom_review   # R14d BOM review report
$CXX tests/test_polarity.cpp src/core/package_edit.cpp src/core/pkg_library.cpp $CORE -o $B/test_polarity   # R15 polarity marker / body protection
$CXX tests/test_r18_import.cpp src/core/package_edit.cpp src/core/pkg_library.cpp $PLUG $(ls src/io/*.cpp) $SHEET $CORE -o $B/test_r18_import   # R18c SMT/TH from drills, polarity classes
$CXX tests/test_transform.cpp src/core/transform.cpp $CORE -o $B/test_transform   # R16c unified Transform
$CXX tests/test_side_sniff.cpp src/io/pnp_import.cpp src/io/text_import.cpp src/io/centroid_import.cpp src/io/text_table.cpp src/io/io_util.cpp $CORE -o $B/test_side_sniff   # R16c centroid sides + BOM/centroid sniff
$CXX tests/test_pkg_resolve.cpp src/core/package_resolve.cpp src/core/package_edit.cpp $CORE -o $B/test_pkg_resolve   # R16b centroid -> package pipeline / wizard / tape angle
$CXX tests/test_outlines.cpp src/io/juki_writer.cpp $CORE -o $B/test_outlines   # R16a outline provenance / export review
$CXX tests/test_e2e_centroid.cpp src/core/package_resolve.cpp src/core/package_edit.cpp $(ls src/io/*.cpp) $SHEET $CORE -o $B/test_e2e_centroid   # R17a REVIEW_V16 §14 end-to-end (needs cwd = repo root)
$CXX tests/test_job_archive.cpp src/core/job_archive.cpp src/core/content_detect.cpp src/io/archive.cpp src/io/inflate.cpp src/io/deflate.cpp src/io/gerber.cpp src/io/juki_import.cpp src/io/juki_writer.cpp $CORE -o $B/test_job_archive   # R19b job archive (Files tab)
$CXX -Ithird_party/imgui tests/test_files_preview.cpp src/io/sheet_preview.cpp src/io/jet_mdb.cpp src/io/docx.cpp src/io/html_rich.cpp src/io/xlsx.cpp src/io/ods.cpp src/io/deflate.cpp src/io/archive.cpp src/io/inflate.cpp src/io/xml_lite.cpp src/io/text_import.cpp src/io/text_table.cpp src/io/io_util.cpp src/core/job_archive.cpp src/core/content_detect.cpp src/io/gerber.cpp src/io/juki_import.cpp src/io/juki_writer.cpp third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_widgets.cpp third_party/imgui/imgui_tables.cpp $SHEET $CORE -o $B/test_files_preview   # R20 Files tab: docx / sheet preview / open-dialog loop guard
$CXX tests/test_export_preflight.cpp src/core/export_preflight.cpp src/core/sides.cpp src/core/placement_check.cpp src/core/package_edit.cpp src/io/juki_writer.cpp $CORE -o $B/test_export_preflight   # R21 AUDIT_V20: shared preflight, side marks, numbered marks, verified write
$CXX tests/test_gerber_match.cpp src/io/gerber_match.cpp src/io/gerber.cpp -o $B/test_gerber_match   # R22b REVIEW_V21 §6/§8/§9/§10: selection model, diagonal pitch, 0/90/180/270 pattern search, outline exactness
$CXX tests/test_job_folder.cpp src/core/job_archive.cpp src/core/aoi_align.cpp src/io/program_view.cpp src/core/content_detect.cpp src/io/archive.cpp src/io/inflate.cpp src/io/deflate.cpp src/io/gerber.cpp src/io/juki_import.cpp src/io/juki_writer.cpp $CORE -o $B/test_job_folder   # R23b job folder sidecars / ISS view / AOI alignment
$CXX tests/test_r23_preview.cpp src/io/sheet_preview.cpp src/io/jet_mdb.cpp src/io/xlsx.cpp src/io/ods.cpp src/io/deflate.cpp src/io/archive.cpp src/io/inflate.cpp src/io/xml_lite.cpp src/io/text_import.cpp src/io/text_table.cpp src/io/io_util.cpp src/core/fileio.cpp src/core/json.cpp $SHEET -o $B/test_r23_preview   # R23a Jet (.mdb/.cpf) sheet preview, copy TSV, find
$CXX tests/test_machine_assign.cpp src/io/juki_writer.cpp $CORE -o $B/test_machine_assign   # R24b library-free machine data assignment
$CXX tests/test_r24a.cpp src/core/placement_check.cpp src/core/package_edit.cpp $PLUG $(ls src/io/*.cpp) $SHEET $CORE -o $B/test_r24a   # R24a user v23: fiducial removal, body dims / CAD height, linked-layer fiducials
for t in $B/test_*; do echo "== $t"; $t | tail -3; done
echo "== test_vendor_formats"; bash tests/test_vendor_formats.sh | tail -1
if [ -f tests/check_devirt.sh ]; then echo "== check_devirt"; bash tests/check_devirt.sh || true; fi   # R23a v22 PDF crash class (weak __cxa_pure_virtual = call 0)
