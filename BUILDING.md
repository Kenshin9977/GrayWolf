# Building GrayWolf

## Requirements

- Visual Studio 2022 (or its Build Tools) with **Desktop development with C++** (toolset v143)
  and a Windows 10/11 SDK.
- [vcpkg](https://github.com/microsoft/vcpkg) in **classic mode**, with the environment variable
  `VCPKG_ROOT` set to its folder: the projects read it (`src/Directory.Build.props`) and the build
  stops with an error when it is not set.
- Git, and Git Bash for `tools/build.sh`.

Dependencies, all with the triplet `x64-windows-static-md` (static libs, dynamic CRT):

```
vcpkg install opus directxtex[dx11] directxtk spdlog casclib libflac libogg miniaudio zlib cli11 miniz libdeflate lz4 xxhash lzo nlohmann-json stb tinygltf "imgui[docking-experimental,dx11-binding,win32-binding]" --triplet x64-windows-static-md --overlay-ports=vcpkg-overlay-ports
```

## Sources

```
git clone https://github.com/Kenshin9977/GrayWolf.git GrayWolf
cd GrayWolf
git submodule update --init --depth 1
```

Do not use `--recursive`: DirectXShaderCompiler is used for its headers only. The OpenAssetTools
submodule's static libraries are built once, by `tools/build-oat.ps1`: the first build of
`GrayWolf.vcxproj` runs it when they are missing (several minutes; it fetches premake). The CI
workflow's "Build the OpenAssetTools libs" step (`.github/workflows/build.yml`) is the same recipe.

## Build

```
tools/build.sh [Dev|Release] [--show-warnings]
```

It builds `src/GrayWolf/GrayWolf.sln` (x64) with MSBuild. `Dev` (the default) has no
whole-program optimisation and rebuilds one file in seconds; `Release` (LTCG) is for a
distributed build. The exe lands in `src/GrayWolf/x64/<Dev|Release>/GrayWolf.exe`, with the
zonekit profiles copied beside it (`zonekit/profiles`): rebuild after changing a table.

At run time, fast files need Oodle beside the exe: `oo2core_8_win64.dll` (Cold War) or
`oo2core_6_win64.dll`, both shipped with the games.

## IW8 layout regression

Run `python tools/check-iw8-soundbank-layout.py --exe src/GrayWolf/x64/Dev/GrayWolf.exe`
after changing the IW8 profile. It builds synthetic soundbanks and checks all eight
`-1`/`-2` variant-array, file-detail and parameter combinations, including relative
and cross-bank name references. It needs Python 3, but no game files or Oodle.
