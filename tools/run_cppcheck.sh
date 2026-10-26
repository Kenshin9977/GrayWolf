#!/bin/bash
# tools/run_cppcheck.sh - opt-in cppcheck static analysis over src/GrayWolf and src/Core.
#
# NOT part of the normal build - this is a separate, deliberately-invoked pass. tools/build.sh's
# ~4s Dev incremental rebuild is a protected property of the iterative workflow;
# nothing here runs automatically as part of it.
#
# Usage:
#   tools/run_cppcheck.sh [output-file]
#
# Defaults to writing the full report to tools/cppcheck-report.log (relative to repo root, git-
# ignored like every other *.log in this project - it's a local run artifact, not tracked) and
# also prints a one-line summary + per-rule-id counts to stdout. A full run over both trees takes
# ~5 minutes on this machine (210 translation units, -j8) - that's expected for an opt-in tool,
# same tradeoff as MSVC /analyze's one-time slow run; it is never wired into a normal build.
#
# --enable=warning,performance,portability only (not style/all - too noisy for a first pass).
#
# Excludes third-party code two ways: -i skips fpng (the one library kept in the repo) as a
# translation unit; tools/cppcheck-suppressions.txt covers the vcpkg headers GLTFExport.cpp
# compiles with their *_IMPLEMENTATION macro.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

CPPCHECK="cppcheck"
if ! command -v cppcheck >/dev/null 2>&1; then
  if [ -f "/c/Program Files/Cppcheck/cppcheck.exe" ]; then
    CPPCHECK="/c/Program Files/Cppcheck/cppcheck.exe"
  else
    echo "cppcheck not found on PATH or at the default install location." >&2
    echo "Install with: winget install --id Cppcheck.Cppcheck -e" >&2
    exit 1
  fi
fi

OUT="${1:-tools/cppcheck-report.log}"

VENDORED=(
  src/Core/Core/fpng.cpp
  src/Core/Core/fpng.h
)

IGNORE_ARGS=()
for f in "${VENDORED[@]}"; do
  IGNORE_ARGS+=(-i "$f")
done

echo "Running cppcheck (this can take several minutes)..." >&2

"$CPPCHECK" \
  --enable=warning,performance,portability \
  --std=c++17 \
  --platform=win64 \
  --library=windows --library=mfc \
  --suppressions-list="$REPO_ROOT/tools/cppcheck-suppressions.txt" \
  -j 8 \
  "${IGNORE_ARGS[@]}" \
  src/GrayWolf/GrayWolf src/Core/Core \
  > "$OUT" 2>&1 || true

echo "Report written to $OUT"
echo ""
echo "Findings by rule id:"
grep -oE "\[[a-zA-Z0-9_]+\]$" "$OUT" | sort | uniq -c | sort -rn
