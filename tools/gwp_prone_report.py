"""Reads the "GWP ..." lines GrayWolf's prone diagnostic (--bo3-usermap-debug prone) prints in Black Ops III
and sums them up: success rate by run, by surface, by ground slope, by spawn class, and what stands in the
way of the failed attempts.

    py -3.11 tools/gwp_prone_report.py <console_mp.log or a copy of the console> [--run ID] [--csv out.csv]

Any text holding the lines works (console_mp.log, a console pasted to a file): a line is found by its
"GWP run=" anywhere in it, and a line seen twice (the console and the on-screen echo) is counted once.
"""
import argparse
import collections
import csv
import re
import sys

LINE = re.compile(r"GWP (run=\S+ seq=\d+ .*)$")


def parse(path):
    seen = set()
    records = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = LINE.search(raw.rstrip("\r\n"))
            if not m:
                continue
            kv = {}
            for tok in m.group(1).split(" "):
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    # the colour codes a console may keep (^7)
                    kv[k] = re.sub(r"\^\d", "", v)
            key = (kv.get("run"), kv.get("seq"))
            if key in seen:
                continue
            seen.add(key)
            records.append(kv)
    return records


def rate(ok, n):
    return f"{ok}/{n} ({100.0 * ok / n:.1f} %)" if n else "0/0"


def dist(word):
    """'37:metal:e12' -> 37.0, '64' -> 64.0"""
    try:
        return float(word.split(":")[0])
    except (ValueError, AttributeError):
        return None


def ground(word):
    """'-0.4:0.999:concrete' -> (dz, nz, surf); none/solid -> (None, None, word)"""
    parts = (word or "u").split(":")
    if len(parts) < 3:
        return None, None, parts[0]
    try:
        return float(parts[0]), float(parts[1]), parts[2]
    except ValueError:
        return None, None, word


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--run", help="one run id only (default: every run)")
    ap.add_argument("--csv", help="also every attempt as a CSV (point, yaw, ok, ground, nearest obstacle...)")
    a = ap.parse_args()

    recs = parse(a.log)
    if a.run:
        recs = [r for r in recs if r.get("run") == a.run]
    if not recs:
        sys.exit(f"no GWP line in {a.log}: was the map run with the diagnostic, and the console logged (+set logfile 2)?")

    by_ev = collections.defaultdict(list)
    for r in recs:
        by_ev[r.get("ev")].append(r)
    print(f"{len(recs)} records, runs: {', '.join(sorted({r['run'] for r in recs}))}")
    for e in by_ev["env"]:
        print("env:", " ".join(f"{k}={v}" for k, v in e.items() if k not in ("run", "seq", "t", "ev")))
    for e in by_ev["keys"]:
        print("trace fields:", " ".join(f"{k}={v}" for k, v in e.items() if k not in ("run", "seq", "t", "ev")))
    for e in by_ev["spawn"]:
        print("spawn:", " ".join(f"{k}={v}" for k, v in e.items() if k not in ("run", "seq", "t", "ev")))

    # the point measures, by (run, point index)
    grounds = {(r["run"], r.get("i")): r for r in by_ev["ground"]}
    dirs = collections.defaultdict(dict)
    for r in by_ev["dir"]:
        dirs[(r["run"], r.get("i"))][int(r.get("d", 0))] = r

    # the runs: started, and how far each got (a run stopped by hand ends before its last point)
    for r in by_ev["run"]:
        ends = [s for s in by_ev["summary"] if s["run"] == r["run"] and int(s.get("seq", 0)) > int(r.get("seq", 0))]
        end = min(ends, key=lambda s: int(s["seq"])) if ends else None
        last = [t for t in by_ev["try"] if t["run"] == r["run"] and int(t["seq"]) > int(r["seq"]) and (end is None or int(t["seq"]) < int(end["seq"]))]
        upto = max((int(t["i"]) for t in last if t.get("i", "").isdigit()), default=None)
        planned = r.get("to") if r.get("mode") == "auto" else "grid"
        print(f"run started seq={r.get('seq')} mode={r.get('mode')} points {r.get('from', '-')}..{planned}: "
              f"{len(last)} attempts, last point {upto}, {'ended' if end else 'no summary (log cut)'}"
              f"{' (stopped early)' if end and r.get('mode') == 'auto' and upto is not None and upto + 1 < int(r.get('to', 0)) else ''}")

    # attempts that say nothing about prone: the player fell during it (fell=1; older logs: moved
    # more than 8 units), or stood on a floor the ground trace from above does not see (a floor
    # facing down, the player under it: the probe's pz=none)
    def excluded(t):
        if t.get("fell") == "1" or ("fell" not in t and float(t.get("moved", "0") or 0) > 8):
            return "fell"
        if grounds.get((t["run"], t.get("i")), {}).get("pz") == "none":
            return "floor_unseen_from_above"
        return ""

    all_tries = by_ev["try"]
    why = collections.Counter(excluded(t) for t in all_tries)
    tries = [t for t in all_tries if not excluded(t)]
    print(f"\n== prone attempts logged: {len(all_tries)}, left out: "
          + (", ".join(f"{k} {v}" for k, v in why.items() if k) or "none")
          + (f" (of them prone 'ok': {sum(t.get('ok') == '1' for t in all_tries if excluded(t))})" if len(tries) < len(all_tries) else ""))
    for k, v in collections.Counter(r.get("why") for r in by_ev["skip"]).items():
        print(f"points skipped in game, {k}: {v}")
    print(f"\n== prone attempts on a floor, no fall: {len(tries)}")
    if tries:
        ok = sum(t.get("ok") == "1" for t in tries)
        crouch = sum(t.get("stc") == "crouch" for t in tries)
        allow = sum(t.get("ok_allow") == "1" for t in tries)
        onground = sum(t.get("og") == "1" for t in tries)
        print(f"prone ok        {rate(ok, len(tries))}")
        print(f"crouch ok       {rate(crouch, len(tries))}   (crouch works where prone does not: a prone-only check)")
        print(f"ok after AllowProne(true) {allow}   (> 0: a script had turned prone off)")
        print(f"on ground before {rate(onground, len(tries))}")
        moved = [float(t["moved"]) for t in tries if t.get("moved") not in (None, "u")]
        if moved:
            print(f"moved during the attempt: max {max(moved):.1f}, > 2 units {sum(m > 2 for m in moved)}")
        inputs = sum(float(t.get("input", "0") or 0) > 0.1 for t in tries if t.get("input") not in (None, "u"))
        if inputs:
            print(f"attempts with the sticks/keys held: {inputs} (a moving player spoils an attempt)")

        def table(title, keyf):
            groups = collections.defaultdict(lambda: [0, 0])
            for t in tries:
                k = keyf(t)
                groups[k][0] += 1
                groups[k][1] += t.get("ok") == "1"
            print(f"\n-- by {title}")
            for k, (n, o) in sorted(groups.items(), key=lambda x: -x[1][0]):
                print(f"  {str(k):40} {rate(o, n)}")

        table("source", lambda t: t.get("src", "u").split(":")[0])
        table("spawn class", lambda t: t.get("src", "u").split(":", 1)[-1])
        table("yaw", lambda t: t.get("yaw"))
        table("ground surface (player mask)", lambda t: t.get("surf"))

        def g(t, key):
            return grounds.get((t["run"], t.get("i")), {}).get(key, "u")

        table("ground entity (player mask)", lambda t: re.sub(r"^\d+:", "", g(t, "pent")))
        table("ground normal z", lambda t: g(t, "pnz")[:4])
        table("drawn floor (bullet) minus clip floor, units", lambda t: _bucket(g(t, "bz"), g(t, "pz")))
        table("ceiling (units above feet, 94 = open)", lambda t: _ceil(g(t, "ceil")))
        table("triggers touched", lambda t: g(t, "touch").split("/")[0])

        # the room around the point along the body: least clear distance ahead and behind at 2..28 units up
        def axis(t):
            d = dirs.get((t["run"], t.get("i")), {})
            yaw = int(float(t.get("yaw", 0)))
            base = int(float(d[0]["yaw"])) if 0 in d else 0
            ahead = ((yaw - base) // 45) % 8
            behind = (ahead + 4) % 8
            least = None
            for k in (ahead, behind):
                for h in ("c2", "c8", "c14", "c20", "c28"):
                    v = dist(d.get(k, {}).get(h))
                    if v is not None and (least is None or v < least):
                        least = v
            return least

        def axis_bucket(t):
            v = axis(t)
            if v is None:
                return "u"
            return "clear 64" if v >= 63.5 else f"blocked < {int(v // 16 + 1) * 16}"

        table("room along the body (least of ahead/behind, heights 2-28)", axis_bucket)

        # behind alone: the legs go there when proning (a wall nearer than about 32 units refuses it,
        # as in any Call of Duty)
        def behind_bucket(t):
            d = dirs.get((t["run"], t.get("i")), {})
            yaw = int(float(t.get("yaw", 0)))
            base = int(float(d[0]["yaw"])) if 0 in d else 0
            k = (((yaw - base) // 45) + 4) % 8
            vals = [dist(d.get(k, {}).get(h)) for h in ("c8", "c14", "c20", "c28")]
            vals = [v for v in vals if v is not None]
            if not vals:
                return "u"
            v = min(vals)
            return "clear 64" if v >= 63.5 else f"blocked < {int(v // 16 + 1) * 16}"

        table("room behind the feet (heights 8-28; 2 left out: kerbs and steps)", behind_bucket)

        def gaps(t):
            d = dirs.get((t["run"], t.get("i")), {})
            yaw = int(float(t.get("yaw", 0)))
            base = int(float(d[0]["yaw"])) if 0 in d else 0
            ahead = ((yaw - base) // 45) % 8
            out = []
            for k in (ahead, (ahead + 4) % 8):
                for key in ("g16", "g32", "g48"):
                    dz, nz, s = ground(d.get(k, {}).get(key))
                    if dz is None:
                        out.append(s)
                    elif abs(dz) > 12:
                        out.append("step")
            return ",".join(sorted(set(out))) or "flat"

        table("ground along the body (16/32/48 ahead and behind)", gaps)

        fails = [t for t in tries if t.get("ok") != "1"]
        print(f"\n-- first failed attempts ({len(fails)} in all)")
        for t in fails[:15]:
            gr = grounds.get((t["run"], t.get("i")), {})
            print(f"  i={t.get('i')} {t.get('src')} yaw={t.get('yaw')} pos={t.get('pos')} stp={t.get('stp')} surf={t.get('surf')} "
                  f"pent={gr.get('pent')} pnz={gr.get('pnz')} bz={gr.get('bz')} touch={gr.get('touch')}")

        if a.csv:
            with open(a.csv, "w", newline="", encoding="utf-8") as f:
                w = csv.writer(f)
                w.writerow(["run", "i", "src", "yaw", "ok", "ok_allow", "stc", "stp", "og", "pos", "surf", "pnz", "pent", "pz", "bz",
                            "bsurf", "ceil", "touch", "least_clear_along_body", "ground_along_body"])
                for t in tries:
                    gr = grounds.get((t["run"], t.get("i")), {})
                    w.writerow([t["run"], t.get("i"), t.get("src"), t.get("yaw"), t.get("ok"), t.get("ok_allow"), t.get("stc"),
                                t.get("stp"), t.get("og"), t.get("pos"), t.get("surf"), gr.get("pnz"), gr.get("pent"), gr.get("pz"),
                                gr.get("bz"), gr.get("bsurf"), gr.get("ceil"), gr.get("touch"), axis(t), gaps(t)])
            print(f"\nCSV: {a.csv}")

    for r in by_ev["stancebtn"]:
        print("stance button:", " ".join(f"{k}={v}" for k, v in r.items() if k not in ("run", "seq", "t", "ev")))
    for r in by_ev["skip"][:10]:
        print("skipped:", r.get("i"), r.get("pos"), r.get("why"))
    for r in by_ev["summary"]:
        print("in-game summary:", " ".join(f"{k}={v}" for k, v in r.items() if k not in ("run", "seq", "t", "ev")))


def _bucket(bz, pz):
    try:
        d = float(bz) - float(pz)
    except ValueError:
        return f"bullet {bz} / clip {pz}"
    if abs(d) < 0.5:
        return "same (|d| < 0.5)"
    return f"{'+' if d > 0 else '-'}{min(int(abs(d) // 2 + 1) * 2, 32)}"


def _ceil(c):
    try:
        v = float(c)
    except ValueError:
        return c
    return "open" if v >= 93.5 else f"< {int(v // 16 + 1) * 16}"


if __name__ == "__main__":
    main()
