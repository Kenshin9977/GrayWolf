#!/usr/bin/env python3
"""tools/run_clang_tidy.py - opt-in clang-tidy static analysis over GrayWolf source files.

Not part of the normal build. Drives clang-tidy.exe directly against a compile_commands.json it
writes itself, never through MSBuild: a RunCodeAnalysisOnce + SelectedFiles build recompiles
whatever else MSBuild thinks is stale and aborts on any broken file before the selected file is
analysed, and it shares x64/Dev with concurrent `tools/build.sh` runs. The command line is
safety_gate.py's (the project's /std:c++20, vcpkg's includes before DirectXShaderCompiler's,
whose miniz.h has no zip API; Core's files with Core's own flags), so both parse alike.

Known limitation: a translation unit that includes resource.h (UTF-16 LE with BOM, e.g. through
MainWindow.h or the *Settings.h headers) fails to parse under clang; such files are reported as
clang-tidy reports them, not skipped by guess.

Checks/config come from .clang-tidy at the repo root.

Usage:
    python tools/run_clang_tidy.py                  # representative sample (default file list)
    python tools/run_clang_tidy.py file1.cpp ...     # files relative to src/GrayWolf/GrayWolf
                                                     # (Games/T8/DiskGame.cpp) or to the repo
    python tools/run_clang_tidy.py --all             # every .cpp of GrayWolf (Games/, AssetGrid/,
                                                     # PreviewPost/ included) and Core (slow)
"""

import json
import os
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from safety_gate import CORE, GW, SKIP, compile_command  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
OUT_FILE = REPO_ROOT / "tools" / "clang-tidy-report.log"

DEFAULT_FILES = [
    "CoDXModelTranslator.cpp",
    "CoDXAnimTranslator.cpp",
    "AssetRegistry.cpp",
]

CLANG_TIDY_CANDIDATES = [
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-tidy.exe",
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\bin\clang-tidy.exe",
    r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-tidy.exe",
]


def find_clang_tidy() -> str:
    for c in CLANG_TIDY_CANDIDATES:
        if os.path.isfile(c):
            return c
    # Fall back to PATH (e.g. `pip install clang-tidy`).
    return "clang-tidy"


def resolve(name: str) -> Path:
    """A file named relative to src/GrayWolf/GrayWolf, else to the repo, else as given."""
    for base in (GW, REPO_ROOT):
        p = base / name
        if p.exists():
            return p.resolve()
    return Path(name).resolve()


def main() -> int:
    args = sys.argv[1:]
    if args == ["--all"]:
        files = [p for root in (GW, CORE) for p in sorted(root.rglob("*.cpp")) if not SKIP.search(p.as_posix())]
    elif args:
        files = [resolve(a) for a in args]
    else:
        files = [resolve(f) for f in DEFAULT_FILES]

    vcpkg_root = os.environ.get("VCPKG_ROOT", "")
    if not vcpkg_root:
        print("run_clang_tidy: VCPKG_ROOT is not set (your vcpkg folder; BUILDING.md)", file=sys.stderr)
        return 2

    db_dir = REPO_ROOT / "tools" / "_clang_tidy_db"
    db_dir.mkdir(exist_ok=True)
    entries = []
    for f in files:
        cmd, directory = compile_command(f, vcpkg_root)
        entries.append({"directory": str(directory), "command": cmd, "file": str(f)})
    (db_dir / "compile_commands.json").write_text(json.dumps(entries, indent=2), encoding="utf-8")

    clang_tidy = find_clang_tidy()
    cmd = [clang_tidy, f"-p={db_dir}", "--config-file", str(REPO_ROOT / ".clang-tidy")] + [str(f) for f in files]

    print(f"Running: {clang_tidy} against {len(files)} file(s)...", file=sys.stderr)
    result = subprocess.run(cmd, capture_output=True, text=True)
    OUT_FILE.write_text(result.stdout + result.stderr, encoding="utf-8")

    print(f"Report written to tools/{OUT_FILE.name}", file=sys.stderr)
    print("", file=sys.stderr)
    print("Findings by check:", file=sys.stderr)
    counts = Counter()
    for line in (result.stdout + result.stderr).splitlines():
        m = re.search(r"\[([a-zA-Z0-9._,-]+)\]\s*$", line)
        if m:
            for check in m.group(1).split(","):
                counts[check] += 1
    for check, n in counts.most_common(30):
        print(f"  {n:5d} {check}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
