# Embeds the Blender import scripts' bodies into the exe as raw string literals under MSVC's 16 KB
# limit: map_import.py into src/GrayWolf/GrayWolf/MapImportScript.cpp, model_import.py into
# ModelImportScript.cpp, each with materials.py (the materials built from the .materials.json
# sidecar, shared by both) in place of its "# ---- materials.py" line.
# Run after editing any of them: python tools/mapimport/embed.py
import os

HERE = os.path.dirname(os.path.abspath(__file__))
CPP = os.path.join(HERE, "..", "..", "src", "GrayWolf", "GrayWolf")
MARK = "// ---- the script body"
SHARED = open(os.path.join(HERE, "materials.py"), encoding="utf-8").read()


def embed(script, cpp):
    body = open(os.path.join(HERE, script), encoding="utf-8").read()
    # the part a script runs on its own (its parameters, for testing) stays out
    body = body.split("# ---- body\n", 1)[1]
    if "# ---- materials.py\n" not in body:
        raise SystemExit("%s has no '# ---- materials.py' line" % script)
    body = body.replace("# ---- materials.py\n", SHARED, 1)
    if ("/" + "*") in body or (")PY" + '"') in body:
        raise SystemExit("%s holds a sequence a C++ raw literal or the repo hook refuses" % script)
    chunks, cur = [], ""
    for line in body.splitlines(keepends=True):
        if len(cur) + len(line) > 12000:
            chunks.append(cur)
            cur = ""
        cur += line
    chunks.append(cur)
    out = os.path.join(CPP, cpp)
    head = open(out, encoding="utf-8").read().split(MARK)[0]
    lits = "\n".join('R"PY(' + c + ')PY"' for c in chunks)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(head + MARK + " (tools/mapimport/%s, embedded by embed.py: edit it there)\n"
                "const char* const Body =\n" % script + lits + ";\n}\n}\n")
    print("%d bytes in %d literals -> %s" % (len(body), len(chunks), os.path.normpath(out)))


embed("map_import.py", "MapImportScript.cpp")
embed("model_import.py", "ModelImportScript.cpp")
