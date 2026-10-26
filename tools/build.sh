#!/bin/bash
# Wrapper around MSBuild that cuts pre-existing unrelated-file warning noise, which every build
# this session scrolled past regardless of what was actually touched. Errors always show; the
# final linker output line always shows. Warnings are suppressed by default (this project has a
# known, accepted backlog of pre-existing warnings unrelated to any one round's own work) unless
# --show-warnings is passed.
#
# Usage:
#   tools/build.sh [Dev|Release|DevAsan] [--show-warnings]
#
# DevAsan is Dev with AddressSanitizer (src/Directory.Build.props, GwAsan): x64/DevAsan/GrayWolf.exe
# beside its clang_rt.asan_dynamic-x86_64.dll, never overwriting the Dev build.
#
# Defaults to Dev (WholeProgramOptimization=false, ~4s true incremental rebuild after a 1-line
# touch, vs. Release's flat 26-46s full LTCG codegen every time regardless of what changed -
# measured 2026-08-29). Use Release only for a final build.

set -euo pipefail

CONFIG="Dev"
SHOW_WARNINGS=0
for arg in "$@"; do
  case "$arg" in
    Dev|Release|DevAsan) CONFIG="$arg" ;;
    --show-warnings) SHOW_WARNINGS=1 ;;
    *) echo "Unknown argument: $arg" >&2; exit 1 ;;
  esac
done

MSBUILD="/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe"
if [ ! -f "$MSBUILD" ]; then
  MSBUILD="/c/Program Files/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe"
fi
if [ ! -f "$MSBUILD" ]; then
  # any other VS 2022 edition (the CI runner's Enterprise): ask vswhere
  VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
  MSBUILD="$("$VSWHERE" -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild/**/Bin/MSBuild.exe' | head -n 1 | tr -d '\r')"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/../src/GrayWolf"

EXTRA=()
if [ "$CONFIG" = "DevAsan" ]; then
  CONFIG="Dev"
  EXTRA=(-p:GwAsan=true)
fi

if [ "$SHOW_WARNINGS" -eq 1 ]; then
  "$MSBUILD" GrayWolf.sln -p:Configuration="$CONFIG" -p:Platform=x64 "${EXTRA[@]}" -m -v:m -nologo
else
  # Drop every "warning C####" line. Our own ZoneWalkers.* work has produced zero warnings every
  # time it's been checked this session (verified repeatedly via --show-warnings during real
  # rounds) - the backlog is entirely pre-existing and unrelated, so there's no exception needed.
  # the build's own exit status, not grep's (which fails when no line is left)
  set +e +o pipefail
  "$MSBUILD" GrayWolf.sln -p:Configuration="$CONFIG" -p:Platform=x64 "${EXTRA[@]}" -m -v:m -nologo \
    | grep -viE ": warning "
  exit "${PIPESTATUS[0]}"
fi
