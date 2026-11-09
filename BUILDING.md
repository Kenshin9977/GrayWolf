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
vcpkg install opus directxtex[dx11] directxtk spdlog casclib libflac libogg miniaudio zlib cli11 miniz libdeflate lz4 xxhash lzo nlohmann-json stb tinygltf wolff "imgui[docking-experimental,dx11-binding,win32-binding]" --triplet x64-windows-static-md --overlay-ports=vcpkg-overlay-ports
```

`wolff` (the fast file reader, its own repository: github.com/Kenshin9977/wolff) is fetched by the
overlay port at the commit it pins. To build from a local checkout instead, set `WOLFF_SOURCE_DIR`
and `VCPKG_KEEP_ENV_VARS=WOLFF_SOURCE_DIR` (`vcpkg-overlay-ports/wolff/portfile.cmake`).

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
zonekit profiles copied beside it (`zonekit/profiles`). The profiles are Wolff's (`profiles/` in
its repository): the build takes them from `WOLFF` (a wolff checkout) when set, else from a
`../wolff` checkout beside GrayWolf, else from the installed package (`share/wolff/profiles`).
Rebuild after changing a table.

Wolff finds the games' decryption keys itself (Black Ops II's and Call of Duty Online's in their
exe, WWII's downloaded once from Scobalula's s2_ff_killer) and caches them in
`%LOCALAPPDATA%\wolff\keys.json`.

At run time, fast files need Oodle beside the exe: `oo2core_8_win64.dll` (Cold War) or
`oo2core_6_win64.dll`, both shipped with the games.
