#!/bin/bash
# tools/run_analyze.sh - opt-in MSVC /analyze (C++ Core Guidelines checker) run over GrayWolf.
#
# NOT part of the normal build - tools/build.sh's ~4s Dev incremental rebuild is a deliberately
# protected property of the iterative workflow. This script is a separate,
# explicitly-invoked pass; nothing here is baked into Directory.Build.props or any .vcxproj, so
# a normal `tools/build.sh` run is completely unaffected.
#
# Forces a full :Rebuild of the GrayWolf project (and pulls in its Core/OatInterop project
# references) so every translation unit actually gets recompiled and therefore actually analyzed
# - RunCodeAnalysisOnce only runs analysis on files MSBuild actually recompiles, so an incremental
# no-op build would silently analyze nothing. That means this is genuinely slow (multiple minutes
# for the whole project, verified 2026-08-31) - expected and fine for an opt-in one-shot tool,
# never run automatically.
#
# Usage:
#   tools/run_analyze.sh [Dev|Release] [output-file]
#
# Uses the Microsoft Native Recommended C++ Core Guidelines ruleset (CppCoreCheckRules.ruleset,
# ships with the VS2022 "C++ Core Features and guidelines checkers" component - already present
# on this machine, verified via EspXEngine.dll/CppCoreCheck.dll under VC/Tools/MSVC/*/bin). Findings
# print inline in the build log as warning C6xxx (legacy /analyze) and C26xxx (Core Guidelines)
# codes; MSBuild also drops one <File>.nativecodeanalysis.xml per analyzed translation unit next
# to its object file, which is the machine-readable form of the same findings.
#
# Builds into an ISOLATED x64/<Config>Analyze intermediate/output dir, never x64/<Config> - the
# analysis-flagged command line (/D CODE_ANALYSIS, /analyze, ...) differs from a normal build's,
# so if this ran against the same x64/Dev a concurrent `tools/build.sh Dev` uses, it would mark
# every object file "stale" for whichever one runs next, turning their next Dev build from a ~4s
# incremental rebuild into a full recompile - directly observed happening once during this script's
# own verification (2026-08-31) before this isolation was added.
#
# KNOWN PRE-EXISTING ISSUE (confirmed, not fixed, unrelated to this script or to analysis flags):
# src/GrayWolf/GrayWolf/FastFile.cpp:688 calls a thirdparty-style `s20_crypt(Key,
# S20_KEYLEN_256, ...)` Salsa20 API that was never wired up - this project's own Salsa20.h only
# exposes `Salsa20::CryptInPlace`/`SelfTest`. Confirmed via `git blame` to predate this session
# (2026-08-04) and confirmed NOT specific to analysis flags - a forced clean `tools/build.sh Dev`
# hits the identical C2065/C3861 errors. It was masked by a stale cached object until something
# forced a full recompile of that file; not fixed here (out of scope for static-analysis setup,
# and correctness-sensitive - it feeds real decryption). This means a full rebuild - this script's
# or anyone else's - currently cannot complete the final Link step; the analysis pass itself still
# runs and produces real findings for every other translation unit before that failure is hit.
set -euo pipefail

CONFIG="${1:-Dev}"
OUT="${2:-tools/msvc-analyze-report.log}"  # git-ignored like every other *.log - local run artifact
ANALYZE_DIR="x64/${CONFIG}Analyze"

MSBUILD="/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe"
if [ ! -f "$MSBUILD" ]; then
  MSBUILD="/c/Program Files/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT/src/GrayWolf"

echo "Running MSVC /analyze (Core Guidelines ruleset) - this is a full rebuild, expect several minutes..." >&2

"$MSBUILD" GrayWolf.sln -t:GrayWolf:Rebuild \
  -p:Configuration="$CONFIG" -p:Platform=x64 \
  -p:RunCodeAnalysisOnce=true \
  -p:EnableCppCoreCheck=true \
  -p:CodeAnalysisRuleSet=CppCoreCheckRules.ruleset \
  -p:IntDir="$ANALYZE_DIR\\" -p:OutDir="$ANALYZE_DIR\\" \
  -m -v:m -nologo \
  > "$REPO_ROOT/$OUT" 2>&1 || true

echo "Report written to $OUT" >&2
echo "" >&2
echo "Findings by warning code:" >&2
grep -oE "warning C[0-9]{4,5}" "$REPO_ROOT/$OUT" | sort | uniq -c | sort -rn | head -30 >&2
