#!/usr/bin/env python3
"""tools/safety_gate.py - pointer-safety gate on changed lines (opt-in; not wired into the hook yet).

Two halves, both reported only on the lines a diff adds or changes, so existing debt never blocks:
  - clang-tidy with tools/safety-gate.clang-tidy (owning raw pointers, malloc, make_unique/shared,
    mutable globals, pointer arithmetic, dangling handles, use after move), through -line-filter;
  - clang-query with tools/safety-gate.query (what clang-tidy has no check for: static raw pointers,
    non-const static members, smart pointers built from .get(), dropped release(), .get() stored in
    a member or a static, static callbacks capturing this).
Per-path exemptions: tools/safety-gate.json. In place: // NOLINT(<check>): <why> (clang-tidy checks
only; a gate-* finding is fixed, or the path exempted in safety-gate.json with its reason).

Usage:
    python tools/safety_gate.py                  # staged changes (what a pre-commit hook would run)
    python tools/safety_gate.py --diff master    # the branch's changes since master (merge base)
    python tools/safety_gate.py --files a.cpp b.h --all-lines   # whole files: the debt they hold
    options: --jobs N, --no-query, --tidy <clang-tidy.exe>, --report <file>
Exit code 1 when a finding is left, 0 otherwise, 2 on a setup error.
"""

import fnmatch
import json
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GW = REPO / "src" / "GrayWolf" / "GrayWolf"
CORE = REPO / "src" / "Core" / "Core"
LLVM = Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin")
SOURCES = ("src/GrayWolf/GrayWolf/", "src/Core/Core/")
SKIP = re.compile(r"(/ThirdParty/|/x64/|/_Template/|/fpng\.|/tiny_gltf|/stb_|/json\.hpp)", re.I)
DIAG = re.compile(r"^(.*?):(\d+):(\d+): (?:warning|error): (.*) \[([\w.,-]+)\]\s*$")
BIND = re.compile(r'^(.*)\((\d+),(\d+)\): note: "(gate-[\w-]+)" binds here')


def git(*args):
    return subprocess.run(["git", *args], cwd=REPO, capture_output=True, text=True, encoding="utf-8",
                          errors="replace").stdout


def changed_lines(diff_args):
    """{repo-relative posix path: [[first, last], ...]} of the lines the diff adds or changes."""
    out = git("diff", "-U0", "--no-color", "--diff-filter=ACMR", *diff_args, "--", *SOURCES)
    ranges = defaultdict(list)
    current = None
    for line in out.splitlines():
        if line.startswith("+++ "):
            current = line[6:] if line.startswith("+++ b/") else None
        elif line.startswith("@@") and current:
            m = re.match(r"@@ -\S+ \+(\d+)(?:,(\d+))? @@", line)
            first, count = int(m.group(1)), int(m.group(2) or 1)
            if count:
                ranges[current].append([first, first + count - 1])
    return {f: r for f, r in ranges.items() if not SKIP.search("/" + f)}


def units_for(rel):
    """The .cpp files whose analysis covers rel: itself, or for a header its same-stem .cpp, else the
    first two .cpp that include it."""
    p = REPO / rel
    if p.suffix == ".cpp":
        return [p]
    same = p.with_suffix(".cpp")
    if same.exists():
        return [same]
    found = []
    inc = re.compile(r'#\s*include\s*["<]([^">]*[/\\])?' + re.escape(p.name) + r'[">]')
    for root in (GW, CORE):
        for c in sorted(root.rglob("*.cpp")):
            if SKIP.search(c.as_posix()):
                continue
            if inc.search(c.read_text(encoding="utf-8", errors="replace")):
                found.append(c)
                if len(found) == 2:
                    return found
    return found


def compile_command(src, vcpkg):
    inst = Path(vcpkg) / "installed" / "x64-windows-static-md" / "include"
    common = ("/nologo /W3 /Od /D CODE_ANALYSIS /D _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS /D FLAC__NO_DLL "
              "/EHa /MD /GS /fp:precise /std:c++20 /Gd /TP --target=amd64-pc-windows-msvc /utf-8 ")
    if CORE in src.parents:
        return (f'"CL.exe" /c /I"{CORE}" /I"{inst}" /I"{inst / "opus"}" '
                "/D WIN32 /D _CRT_NO_VA_START_VALIDATION /D _DEBUG /D _LIB " + common + f'"{src}"'), CORE
    # vcpkg before DirectXShaderCompiler: DXC ships a miniz.h of its own without the zip API
    # the compiled shader headers of PreviewPost: whichever build made them (none before a first build)
    shaders = next(iter(sorted(GW.glob("x64/*/PreviewPostShaders"))), GW / "x64" / "Dev" / "PreviewPostShaders")
    return (f'"CL.exe" /c /I"{GW}" /I"{GW.parent / "OatInterop"}" /I"{CORE}" /I"{inst}" '
            f'/I"{REPO / "src" / "External" / "DirectXShaderCompiler" / "include"}" /I"{shaders}" '
            "/D _UNICODE /D UNICODE /D CASCLIB_NO_AUTO_LINK_LIBRARY /D CASCLIB_UNICODE "
            "/D CASCLIB_DETECT_UNICODE_MISMATCHES " + common + f'"{src}"'), GW


def rel_of(path):
    try:
        return Path(os.path.normpath(path)).resolve().relative_to(REPO).as_posix()
    except ValueError:
        return None


def main():
    args = sys.argv[1:]

    def opt(name, default=None):
        return args[args.index(name) + 1] if name in args else default

    all_lines = "--all-lines" in args
    if "--files" in args:
        names = [a for a in args[args.index("--files") + 1:] if not a.startswith("--")]
        names = [n for n in names if n not in (opt("--jobs"), opt("--tidy"), opt("--report"))]
        files = {Path(n).resolve().relative_to(REPO).as_posix(): [[1, 10**9]] for n in names}
        all_lines = True
    elif "--diff" in args:
        files = changed_lines([opt("--diff") + "...HEAD"])
    else:
        files = changed_lines(["--cached"])
    if not files:
        print("safety-gate: no C/C++ change under src/GrayWolf or src/Core.")
        return 0

    vcpkg = os.environ.get("VCPKG_ROOT", "")
    if not vcpkg:
        print("safety-gate: VCPKG_ROOT is not set (BUILDING.md)", file=sys.stderr)
        return 2
    tidy = opt("--tidy", str(LLVM / "clang-tidy.exe"))
    query = str(LLVM / "clang-query.exe")
    exempt = json.loads((REPO / "tools" / "safety-gate.json").read_text(encoding="utf-8"))["exempt"]

    units = sorted({u for f in files for u in units_for(f)})
    if not units:
        print("safety-gate: no translation unit covers the changed files.")
        return 0
    db = Path(tempfile.gettempdir()) / "graywolf_safety_gate"
    db.mkdir(exist_ok=True)
    entries = []
    for u in units:
        cmd, d = compile_command(u, vcpkg)
        entries.append({"directory": str(d), "command": cmd, "file": str(u)})
    (db / "compile_commands.json").write_text(json.dumps(entries, indent=1), encoding="utf-8")
    line_filter = json.dumps([{"name": str(REPO / f).replace("/", os.sep), "lines": r} for f, r in files.items()])

    def run(entry):
        outs = []
        cmd = [tidy, f"-p={db}", f"--config-file={REPO / 'tools' / 'safety-gate.clang-tidy'}", "--quiet",
               "--extra-arg=-Wdangling", "--extra-arg=-Wdangling-gsl", "--extra-arg=-Wreturn-stack-address",
               entry["file"]]
        if not all_lines:
            cmd.insert(3, f"--line-filter={line_filter}")
        r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
        outs.append(("tidy", r.stdout + r.stderr))
        if "--no-query" not in args:
            r = subprocess.run([query, f"-p={db}", "-f", str(REPO / "tools" / "safety-gate.query"), entry["file"]],
                               capture_output=True, text=True, encoding="utf-8", errors="replace")
            outs.append(("query", r.stdout + r.stderr))
        return entry["file"], outs

    findings = {}
    parse_errors = []
    with ThreadPoolExecutor(int(opt("--jobs", "8"))) as ex:
        for unit, outs in ex.map(run, entries):
            for kind, text in outs:
                lines = text.splitlines()
                for i, line in enumerate(lines):
                    m = DIAG.match(line) if kind == "tidy" else BIND.match(line)
                    if not m:
                        continue
                    if kind == "tidy":
                        path, ln, msg, checks = m.group(1), int(m.group(2)), m.group(4), m.group(5).split(",")
                    else:
                        path, ln = m.group(1), int(m.group(2))
                        checks = [m.group(4)]
                        msg = lines[i + 1].split("|", 1)[1].strip() if i + 1 < len(lines) and "|" in lines[i + 1] else ""
                    rel = rel_of(path)
                    if rel is None:
                        continue
                    if "clang-diagnostic-error" in checks:
                        parse_errors.append(f"{rel}:{ln}: {msg} (in {Path(unit).name})")
                        continue
                    if rel not in files or not any(a <= ln <= b for a, b in files[rel]):
                        continue
                    for c in checks:
                        if c == "-warnings-as-errors" or any(fnmatch.fnmatch(rel, g) for g in exempt.get(c, [])):
                            continue
                        findings[(rel, ln, c)] = msg

    report = []
    for (rel, ln, c), msg in sorted(findings.items()):
        report.append(f"{rel}:{ln}: [{c}] {msg}")
    counts = Counter(c for _, _, c in findings)
    summary = [f"  {n:5d} {c}" for c, n in counts.most_common()]
    text = "\n".join(report + ([""] + ["safety-gate findings by check:"] + summary if report else []))
    if parse_errors:
        text += "\n\nclang could not parse part of a unit (findings past the error may be missing):\n  " + \
                "\n  ".join(sorted(set(parse_errors))[:10])
    if opt("--report"):
        Path(opt("--report")).write_text(text + "\n", encoding="utf-8")
    print(text if text.strip() else "safety-gate: no finding on the changed lines.")
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
