#!/bin/bash
# Cross-compile PCB Plotter with mingw-w64 (verification build; the product build is the VS2026 .sln)
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-build/mingw}
mkdir -p $OUT/obj
CXX=x86_64-w64-mingw32-g++
FLAGS="-std=c++20 -O2 -Wall -Wno-unused-function -Isrc -Isdk -Ithird_party/imgui -DUNICODE -D_UNICODE -DNDEBUG -DIMGUI_DEFINE_MATH_OPERATORS"
SRCS=$(ls src/core/*.cpp src/io/*.cpp src/plugins/*.cpp src/ui/*.cpp src/app/*.cpp src/gui/*.cpp src/platform/*.cpp third_party/imgui/*.cpp)
OBJS=""
JOBS=${JOBS:-3}
LIST=""
for s in $SRCS; do
  o=$OUT/obj/$(echo $s | tr '/' '_' | sed 's/\.cpp$/.o/')
  OBJS="$OBJS $o"
  if [ ! -f $o ] || [ $s -nt $o ] || [ -n "$(find src -name '*.h' -newer $o | head -1)" ]; then LIST="$LIST $s:$o"; fi
done
echo $LIST | tr ' ' '\n' | grep . | xargs -P $JOBS -I{} sh -c 's=$(echo {} | cut -d: -f1); o=$(echo {} | cut -d: -f2); '"$CXX $FLAGS"' -c $s -o $o || { echo "FAILED: $s"; exit 255; }' || exit 1
RES=""
if [ -f res/app.rc ]; then x86_64-w64-mingw32-windres -Ires res/app.rc -O coff -o $OUT/obj/app_res.o && RES=$OUT/obj/app_res.o; fi
$CXX -static -mwindows -municode -o $OUT/PCBPlotter.exe $OBJS $RES \
  -ld2d1 -ldwrite -ldwmapi -luxtheme -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lgdi32 -luser32 -lmsimg32 -lwindowscodecs -luuid -ld3d11 -ldxgi -limm32 -lwinmm -lshlwapi -lwinhttp
echo "built $OUT/PCBPlotter.exe"
# Explorer thumbnail provider pcbp_thumb.dll (in-proc COM, .pcbp/.cpf previews; see specs/WIN_SHELL.md)
TOBJS=""
for s in src/platform/thumb/thumb_render.cpp src/platform/thumb/pcbp_thumb.cpp; do
  o=$OUT/obj/$(echo $s | tr '/' '_' | sed 's/\.cpp$/.o/')
  if [ ! -f $o ] || [ $s -nt $o ] || [ -n "$(find src -name '*.h' -newer $o | head -1)" ]; then $CXX $FLAGS -c $s -o $o; fi
  TOBJS="$TOBJS $o"
done
DOBJS=$(ls $OUT/obj/src_core_*.o $OUT/obj/src_io_*.o $OUT/obj/src_plugins_*.o | grep -v plugin_dll_win)
$CXX -static -shared -s -o $OUT/pcbp_thumb.dll $TOBJS $DOBJS src/platform/thumb/pcbp_thumb.def -lole32 -luuid -lshell32 -lgdi32 -ladvapi32
echo "built $OUT/pcbp_thumb.dll"
