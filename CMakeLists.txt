# PCB Plotter - CMake build (MinGW-w64 cross / MSYS2 native / Linux-native core + tests).
# The product build is still the VS2026 / VS2022 .sln; this mirrors it for GCC.
#
#   Linux -> Windows:  cmake --preset mingw-cross        && cmake --build --preset mingw-cross
#   MSYS2 (UCRT64):    cmake --preset mingw-native       && cmake --build --preset mingw-native
#   Linux tests:       cmake --preset linux-tests        && cmake --build --preset linux-tests && ctest --preset linux-tests
#
# Source globs mirror tools/gen_vcxproj.py (SRC_DIRS); re-run cmake after adding/removing files.
cmake_minimum_required(VERSION 3.21)
project(PCBPlotter LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(PCBP_BUILD_APP   "Build PCBPlotter.exe + pcbp_thumb.dll (Windows targets only)" ON)
option(PCBP_BUILD_TESTS "Build the portable unit tests (tests/test_*.cpp)" OFF)
option(PCBP_WERROR      "Treat warnings as errors" OFF)
option(PCBP_STATIC      "Static libgcc/libstdc++/winpthread (one self-contained exe)" ON)

# ---------------------------------------------------------------- sources
macro(pcbp_glob var)
  set(${var})
  foreach(d ${ARGN})
    file(GLOB _g CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${d}/*.cpp")
    list(SORT _g)
    list(APPEND ${var} ${_g})
  endforeach()
endmacro()

pcbp_glob(SRC_CORE    src/core src/io src/plugins)        # portable (Linux tests link this)
list(FILTER SRC_CORE EXCLUDE REGEX "/plugin_dll_win\\.cpp$")
pcbp_glob(SRC_GUI     src/ui src/app src/gui src/platform)
pcbp_glob(SRC_IMGUI   third_party/imgui)
set(SRC_PLUGIN_DLL ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/plugin_dll_win.cpp)
set(SRC_THUMB ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/thumb/thumb_render.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/thumb/pcbp_thumb.cpp)

# ---------------------------------------------------------------- common flags
add_library(pcbp_flags INTERFACE)
target_include_directories(pcbp_flags INTERFACE src sdk third_party/imgui)
target_compile_definitions(pcbp_flags INTERFACE IMGUI_DEFINE_MATH_OPERATORS $<$<CONFIG:Release>:NDEBUG>)
if(MSVC)
  target_compile_options(pcbp_flags INTERFACE /utf-8 /Zc:__cplusplus /W4 /permissive-
    /wd4100 /wd4244 /wd4267 /wd4389 /wd4456 /wd4457 /wd4458 /wd4459 /wd4505 /wd4702 /wd4706 /wd4127 /wd4996)
  target_compile_definitions(pcbp_flags INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  if(PCBP_WERROR)
    target_compile_options(pcbp_flags INTERFACE /WX)
  endif()
else()
  target_compile_options(pcbp_flags INTERFACE -Wall -Wno-unused-function)
  if(PCBP_WERROR)
    target_compile_options(pcbp_flags INTERFACE -Werror)
  endif()
endif()
if(WIN32)
  target_compile_definitions(pcbp_flags INTERFACE UNICODE _UNICODE)
endif()

# ---------------------------------------------------------------- portable core
# OBJECT library: linked whole into the exe and the thumbnail DLL (as the .sln does),
# wrapped in a STATIC library for tests so each test pulls only the objects it uses.
add_library(pcbp_core_obj OBJECT ${SRC_CORE})
target_link_libraries(pcbp_core_obj PUBLIC pcbp_flags)
set_target_properties(pcbp_core_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(pcbp_core STATIC $<TARGET_OBJECTS:pcbp_core_obj>)
target_link_libraries(pcbp_core PUBLIC pcbp_flags)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(pcbp_core PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------- Windows app
if(WIN32 AND PCBP_BUILD_APP)
  # same set as tools/build_mingw.sh (ODBC, dbghelp, WinRT PDF are LoadLibrary'd at runtime)
  set(PCBP_WIN_LIBS d2d1 dwrite dwmapi uxtheme comctl32 comdlg32 shell32 ole32 gdi32 user32
      msimg32 windowscodecs uuid d3d11 dxgi imm32 winmm shlwapi winhttp)
  set(RC_SRC)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/res/app.rc)
    enable_language(RC)
    set(RC_SRC res/app.rc)
    if(NOT MSVC)
      set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR}/res")
    endif()
  endif()

  add_executable(PCBPlotter WIN32
    $<TARGET_OBJECTS:pcbp_core_obj> ${SRC_PLUGIN_DLL} ${SRC_GUI} ${SRC_IMGUI} ${RC_SRC})
  target_link_libraries(PCBPlotter PRIVATE pcbp_flags ${PCBP_WIN_LIBS})

  add_library(pcbp_thumb SHARED
    ${SRC_THUMB} $<TARGET_OBJECTS:pcbp_core_obj> src/platform/thumb/pcbp_thumb.def)
  target_link_libraries(pcbp_thumb PRIVATE pcbp_flags ole32 uuid shell32 gdi32 advapi32)
  set_target_properties(pcbp_thumb PROPERTIES PREFIX "" OUTPUT_NAME pcbp_thumb)

  if(MINGW)
    target_link_options(PCBPlotter PRIVATE -municode)
    if(PCBP_STATIC)
      target_link_options(PCBPlotter PRIVATE -static)
      target_link_options(pcbp_thumb PRIVATE -static)
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
      target_link_options(pcbp_thumb PRIVATE -s)
    endif()
  endif()
  set_target_properties(PCBPlotter pcbp_thumb PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# ---------------------------------------------------------------- tests
if(PCBP_BUILD_TESTS)
  enable_testing()
  # several tests write scratch files to <repo>/build/tests (as tests/run_all.sh lays it out)
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build/tests)
  add_library(pcbp_imgui_core STATIC
    third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_widgets.cpp third_party/imgui/imgui_tables.cpp)
  target_link_libraries(pcbp_imgui_core PUBLIC pcbp_flags)

  file(GLOB TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp)
  list(SORT TEST_SRCS)
  foreach(t ${TEST_SRCS})
    get_filename_component(name ${t} NAME_WE)
    file(STRINGS ${t} _win REGEX "#include <windows\\.h>")
    if(_win AND NOT WIN32)
      continue()   # Windows-only test (e.g. test_seh_guard): built by the mingw-cross-tests preset
    endif()
    add_executable(${name} ${t})
    target_link_libraries(${name} PRIVATE pcbp_core)
    if(name STREQUAL "test_files_preview")
      target_link_libraries(${name} PRIVATE pcbp_imgui_core)
    endif()
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
    # several tests read fixtures relative to the repo root (tests/data, docs/demo)
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endforeach()
  if(NOT WIN32 AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_vendor_formats.sh)
    add_test(NAME test_vendor_formats COMMAND bash tests/test_vendor_formats.sh
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endif()
endif()
