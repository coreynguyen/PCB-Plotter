#!/usr/bin/env python3
# Generates PCBPlotter.sln + PCBPlotter.vcxproj (+ .filters) and sdk/sample_plugin/SamplePlugin.vcxproj
# for Visual Studio 2026 (PlatformToolset v145). Run from the repo root after adding/removing sources.
# R21: also emits a Visual Studio 2022 (PlatformToolset v143) variant next to it:
#   PCBPlotter_vs2022.sln / PCBPlotter_vs2022.vcxproj (+ .filters) / PcbpThumb_vs2022.vcxproj /
#   sdk/sample_plugin/SamplePlugin_vs2022.vcxproj  (VCProjectVersion 17.0, "# Visual Studio Version 17",
#   separate obj\vs2022\ intermediate folder). Nothing in the generated projects is v145-only: C++20
#   (stdcpp20), /utf-8 /Zc:__cplusplus, ConformanceMode, static CRT -- all supported by v143.
#   --only vs2026 | --only vs2022 limits the output to one variant.
import os, uuid, glob, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

APP_GUID = "{6C1E5A3B-8F2D-4B7A-9E41-2D5C7A1B9F01}"
PLUG_GUID = "{6C1E5A3B-8F2D-4B7A-9E41-2D5C7A1B9F02}"
SLN_TYPE = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}"
TOOLSET = "v145"
VCPROJ_VER = "18.0"      # VS2026; VS2022 = 17.0
SLN_VS = "18"            # "# Visual Studio Version N"
SUFFIX = ""              # file-name suffix of the variant
INT_SUB = ""             # obj\<INT_SUB><name>\... (keeps the variants' intermediates apart)

def rel(p): return p.replace("/", "\\")

# Mirror tools/build_mingw.sh exactly (legacy/ and tests/ are not part of the product build).
SRC_DIRS = ["src/core", "src/io", "src/plugins", "src/ui", "src/app", "src/gui", "src/platform", "third_party/imgui"]
srcs = [f for d in SRC_DIRS for f in sorted(glob.glob(d + "/*.cpp"))]
hdrs = [f for d in SRC_DIRS for f in sorted(glob.glob(d + "/*.h"))] + ["sdk/pcbp_plugin.h"]
PLATS = ("x64",)

def config_block(kind, name, defines, extra_link=""):
    out = []
    for cfg in ("Debug", "Release"):
        for plat in PLATS:
            cond = f"'$(Configuration)|$(Platform)'=='{cfg}|{plat}'"
            dbg = cfg == "Debug"
            out.append(f"""  <ItemDefinitionGroup Condition="{cond}">
    <ClCompile>
      <ObjectFileName>$(IntDir)%(RelativeDir)</ObjectFileName>
      <WarningLevel>Level4</WarningLevel>
      <SDLCheck>true</SDLCheck>
      <PreprocessorDefinitions>{'_DEBUG' if dbg else 'NDEBUG'};{defines};UNICODE;_UNICODE;IMGUI_DEFINE_MATH_OPERATORS;NOMINMAX;WIN32_LEAN_AND_MEAN;_CRT_SECURE_NO_WARNINGS;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <ConformanceMode>true</ConformanceMode>
      <LanguageStandard>stdcpp20</LanguageStandard>
      <AdditionalIncludeDirectories>$(ProjectDir)src;$(ProjectDir)sdk;$(ProjectDir)third_party\\imgui;$(ProjectDir)..\\..\\sdk;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <RuntimeLibrary>{'MultiThreadedDebugDLL' if dbg else 'MultiThreaded'}</RuntimeLibrary>
      <MultiProcessorCompilation>true</MultiProcessorCompilation>
      <AdditionalOptions>/utf-8 /Zc:__cplusplus /wd4100 /wd4244 /wd4267 /wd4389 /wd4456 /wd4457 /wd4458 /wd4459 /wd4505 /wd4702 /wd4706 /wd4127 /wd4996 %(AdditionalOptions)</AdditionalOptions>
      <Optimization>{'Disabled' if dbg else 'MaxSpeed'}</Optimization>
      <FunctionLevelLinking>{'false' if dbg else 'true'}</FunctionLevelLinking>
      <IntrinsicFunctions>{'false' if dbg else 'true'}</IntrinsicFunctions>
    </ClCompile>
    <Link>
      <SubSystem>{'Windows' if kind == 'Application' else 'Windows'}</SubSystem>
      <GenerateDebugInformation>true</GenerateDebugInformation>
      <EnableCOMDATFolding>{'false' if dbg else 'true'}</EnableCOMDATFolding>
      <OptimizeReferences>{'false' if dbg else 'true'}</OptimizeReferences>
      <AdditionalDependencies>{extra_link}%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
    <Manifest>
      <EnableDpiAwareness>PerMonitorHighDPIAware</EnableDpiAwareness>
    </Manifest>
  </ItemDefinitionGroup>""")
    return "\n".join(out)

def project(name, guid, kind, sources, headers, rc, defines, extra_link, out_name, filters_root=True):
    cfgs = []
    for cfg in ("Debug", "Release"):
        for plat in PLATS:
            cfgs.append(f"""    <ProjectConfiguration Include="{cfg}|{plat}">
      <Configuration>{cfg}</Configuration>
      <Platform>{plat}</Platform>
    </ProjectConfiguration>""")
    props = []
    for cfg in ("Debug", "Release"):
        for plat in PLATS:
            cond = f"'$(Configuration)|$(Platform)'=='{cfg}|{plat}'"
            props.append(f"""  <PropertyGroup Condition="{cond}" Label="Configuration">
    <ConfigurationType>{kind}</ConfigurationType>
    <UseDebugLibraries>{'true' if cfg == 'Debug' else 'false'}</UseDebugLibraries>
    <PlatformToolset>{TOOLSET}</PlatformToolset>
    <WholeProgramOptimization>{'false' if cfg == 'Debug' else 'true'}</WholeProgramOptimization>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>""")
    outs = []
    for cfg in ("Debug", "Release"):
        for plat in PLATS:
            cond = f"'$(Configuration)|$(Platform)'=='{cfg}|{plat}'"
            outs.append(f"""  <PropertyGroup Condition="{cond}">
    <OutDir>$(SolutionDir)bin\\$(Platform)\\$(Configuration)\\{'plugins\\' if kind == 'DynamicLibrary' else ''}</OutDir>
    <IntDir>$(SolutionDir)obj\\{INT_SUB}{name}\\$(Platform)\\$(Configuration)\\</IntDir>
    <TargetName>{out_name}</TargetName>
    <GenerateManifest>false</GenerateManifest>
  </PropertyGroup>""")
    cl = "\n".join(f'    <ClCompile Include="{rel(s)}" />' for s in sources)
    hh = "\n".join(f'    <ClInclude Include="{rel(h)}" />' for h in headers)
    rcs = f'  <ItemGroup>\n    <ResourceCompile Include="{rel(rc)}" />\n  </ItemGroup>\n  <ItemGroup>\n    <None Include="res\\app.manifest" />\n    <Image Include="res\\app.ico" />\n' + "".join(f'    <Image Include="res\\icons\\{os.path.basename(i)}" />\n' for i in sorted(glob.glob("res/icons/*.ico"))) + '  </ItemGroup>' if rc else ""
    return f"""<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
{chr(10).join(cfgs)}
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <VCProjectVersion>{VCPROJ_VER}</VCProjectVersion>
    <Keyword>Win32Proj</Keyword>
    <ProjectGuid>{guid}</ProjectGuid>
    <RootNamespace>{name}</RootNamespace>
    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
{chr(10).join(props)}
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
  <ImportGroup Label="PropertySheets">
    <Import Project="$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props" Condition="exists('$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props')" Label="LocalAppDataPlatform" />
  </ImportGroup>
{chr(10).join(outs)}
{config_block(kind, name, defines, extra_link)}
  <ItemGroup>
{cl}
  </ItemGroup>
  <ItemGroup>
{hh}
  </ItemGroup>
{rcs}
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />
</Project>
"""

def filters(sources, headers, rc):
    dirs = sorted({os.path.dirname(rel(p)) for p in sources + headers})
    fl = []
    for d in dirs:
        fl.append(f'    <Filter Include="{d}">\n      <UniqueIdentifier>{{{uuid.uuid5(uuid.NAMESPACE_URL, d)}}}</UniqueIdentifier>\n    </Filter>')
    items = []
    for s in sources:
        items.append(f'    <ClCompile Include="{rel(s)}">\n      <Filter>{os.path.dirname(rel(s))}</Filter>\n    </ClCompile>')
    for h in headers:
        items.append(f'    <ClInclude Include="{rel(h)}">\n      <Filter>{os.path.dirname(rel(h))}</Filter>\n    </ClInclude>')
    if rc:
        fl.append(f'    <Filter Include="res">\n      <UniqueIdentifier>{{{uuid.uuid5(uuid.NAMESPACE_URL, "res")}}}</UniqueIdentifier>\n    </Filter>')
        items.append(f'    <ResourceCompile Include="{rel(rc)}">\n      <Filter>res</Filter>\n    </ResourceCompile>')
    return f"""<?xml version="1.0" encoding="utf-8"?>
<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup>
{chr(10).join(fl)}
  </ItemGroup>
  <ItemGroup>
{chr(10).join(items)}
  </ItemGroup>
</Project>
"""

def generate():
    global srcs
    # mirrors the mingw link line
    LIBS = "d3d11.lib;dxgi.lib;d2d1.lib;dwrite.lib;windowscodecs.lib;imm32.lib;dwmapi.lib;uxtheme.lib;comctl32.lib;comdlg32.lib;shell32.lib;shlwapi.lib;ole32.lib;uuid.lib;gdi32.lib;user32.lib;msimg32.lib;advapi32.lib;winmm.lib;"
    with open(f"PCBPlotter{SUFFIX}.vcxproj", "w", newline="\r\n") as f:
        f.write(project("PCBPlotter", APP_GUID, "Application", srcs, hdrs, "res/app.rc", "PCBP_APP", LIBS, "PCBPlotter"))
    with open(f"PCBPlotter{SUFFIX}.vcxproj.filters", "w", newline="\r\n") as f:
        f.write(filters(srcs, hdrs, "res/app.rc"))

    plug_src = sorted(glob.glob("sdk/sample_plugin/*.cpp"))
    have_plugin = bool(plug_src)
    if have_plugin:
        ps = [os.path.relpath(p, "sdk/sample_plugin") for p in plug_src]
        with open(f"sdk/sample_plugin/SamplePlugin{SUFFIX}.vcxproj", "w", newline="\r\n") as f:
            f.write(project("SamplePlugin", PLUG_GUID, "DynamicLibrary", ps, ["..\\pcbp_plugin.h"], None, "PCBP_PLUGIN", "", "sample_plugin"))

    # Explorer thumbnail provider (specs/WIN_SHELL.md): portable core/io/plugins + src/platform/thumb, exports via .def
    THUMB_GUID = "{6C1E5A3B-8F2D-4B7A-9E41-2D5C7A1B9F03}"
    thumb_src = sorted(glob.glob("src/platform/thumb/*.cpp")) + [f for d in ("src/core", "src/io", "src/plugins") for f in sorted(glob.glob(d + "/*.cpp")) if not f.endswith("plugin_dll_win.cpp")]
    thumb_hdr = sorted(glob.glob("src/platform/thumb/*.h"))
    have_thumb = bool(glob.glob("src/platform/thumb/pcbp_thumb.cpp"))
    if have_thumb:
        t = project("PcbpThumb", THUMB_GUID, "DynamicLibrary", thumb_src, thumb_hdr, None, "PCBP_THUMB", "ole32.lib;uuid.lib;shell32.lib;gdi32.lib;advapi32.lib;", "pcbp_thumb")
        t = t.replace("$(Configuration)\\plugins\\</OutDir>", "$(Configuration)\\</OutDir>")
        t = t.replace("    </Link>", "      <ModuleDefinitionFile>src\\platform\\thumb\\pcbp_thumb.def</ModuleDefinitionFile>\n    </Link>")
        with open(f"PcbpThumb{SUFFIX}.vcxproj", "w", newline="\r\n") as f:
            f.write(t)

    sln = ["", "Microsoft Visual Studio Solution File, Format Version 12.00", f"# Visual Studio Version {SLN_VS}",
           f"VisualStudioVersion = {SLN_VS}.0.0.0", "MinimumVisualStudioVersion = 10.0.40219.1",
           f'Project("{SLN_TYPE}") = "PCBPlotter", "PCBPlotter{SUFFIX}.vcxproj", "{APP_GUID}"', "EndProject"]
    if have_plugin:
        sln += [f'Project("{SLN_TYPE}") = "SamplePlugin", "sdk\\sample_plugin\\SamplePlugin{SUFFIX}.vcxproj", "{PLUG_GUID}"',
                f"\tProjectSection(ProjectDependencies) = postProject", "\tEndProjectSection", "EndProject"]
    if have_thumb:
        sln += [f'Project("{SLN_TYPE}") = "PcbpThumb", "PcbpThumb{SUFFIX}.vcxproj", "{THUMB_GUID}"', "EndProject"]
    sln += ["Global", "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution"]
    for c in ("Debug", "Release"):
        for p in ("x64",):
            sln.append(f"\t\t{c}|{p} = {c}|{p}")
    sln.append("\tEndGlobalSection")
    sln.append("\tGlobalSection(ProjectConfigurationPlatforms) = postSolution")
    for g in [APP_GUID] + ([PLUG_GUID] if have_plugin else []) + ([THUMB_GUID] if have_thumb else []):
        for c in ("Debug", "Release"):
            for p, vp in (("x64", "x64"),):
                sln.append(f"\t\t{g}.{c}|{p}.ActiveCfg = {c}|{vp}")
                sln.append(f"\t\t{g}.{c}|{p}.Build.0 = {c}|{vp}")
    sln += ["\tEndGlobalSection", "\tGlobalSection(SolutionProperties) = preSolution", "\t\tHideSolutionNode = FALSE",
            "\tEndGlobalSection", "EndGlobal", ""]
    with open(f"PCBPlotter{SUFFIX}.sln", "w", newline="\r\n", encoding="utf-8-sig") as f:
        f.write("\n".join(sln))
    print(f"generated {TOOLSET} (PCBPlotter{SUFFIX}.sln): {len(srcs)} sources, {len(hdrs)} headers, plugin={have_plugin}")


VARIANTS = {
    "vs2026": dict(TOOLSET="v145", VCPROJ_VER="18.0", SLN_VS="18", SUFFIX="", INT_SUB=""),
    "vs2022": dict(TOOLSET="v143", VCPROJ_VER="17.0", SLN_VS="17", SUFFIX="_vs2022", INT_SUB="vs2022\\"),
}
only = None
if "--only" in sys.argv:
    i = sys.argv.index("--only")
    only = sys.argv[i + 1] if i + 1 < len(sys.argv) else None
    if only not in VARIANTS: sys.exit("usage: gen_vcxproj.py [--only vs2026|vs2022]")
for key, v in VARIANTS.items():
    if only and key != only: continue
    globals().update(v)
    generate()
