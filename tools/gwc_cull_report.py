"""Reads the "GWC ..." lines of GrayWolf's culling diagnostic (--bo3-usermap-debug cull|both) and sums them up:
the eye at each stance change or test, the marks (models seen / hidden) with the eye and the test in force,
each eye-height sweep's change heights, the twins, the static models listed in view.

    py -3.11 tools/gwc_cull_report.py <console_mp.log or a copy of the console> [--near]

A line is found by its "GWC run=" anywhere in it; a line seen twice (console and on-screen echo) counts once.
"""
import argparse
import collections
import re
import sys

LINE = re.compile(r"GWC (run=\S+ seq=\d+ .*)$")


def parse(path):
    seen, recs = set(), []
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = LINE.search(raw.rstrip("\r\n"))
            if not m:
                continue
            kv = {}
            for tok in m.group(1).split(" "):
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    kv[k] = re.sub(r"\^\d", "", v)
            key = (kv.get("run"), kv.get("seq"))
            if key in seen:
                continue
            seen.add(key)
            recs.append(kv)
    return recs


def show(r, keys):
    return " ".join(f"{k}={r[k]}" for k in keys if k in r)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--near", action="store_true", help="also every static model listed in view")
    a = ap.parse_args()
    recs = parse(a.log)
    if not recs:
        sys.exit(f"no GWC line in {a.log}: was the map built with --bo3-usermap-debug cull|both, and the console logged (+set logfile 2)?")
    server = [r for r in recs if r.get("run") != "c"]
    client = [r for r in recs if r.get("run") == "c"]
    print(f"{len(server)} server records, {len(client)} client records" +
          ("" if client else " (none: the client script did not run or its PrintLn does not reach the log)"))

    print("\n== automatic runs (F+Shift): the summary line, then each answer")
    for r in server:
        if r.get("ev") == "auto_summary":
            print("  " + " ".join(f"{k}={v}" for k, v in r.items() if k not in ("run", "seq", "t", "ev")))
    for r in server:
        if r.get("ev") == "answer":
            print("    " + show(r, ["step", "answer", "stance"]))
    print("\n== keys pressed")
    keys = collections.Counter((r.get("key"), r.get("action")) for r in server if r.get("ev") == "key")
    print("  " + (", ".join(f"{k} {a} x{n}" for (k, a), n in keys.items()) or "none logged"))

    print("\n== eye (each stance change, test, command)")
    for r in server:
        if r.get("ev") == "eye":
            print("  " + show(r, ["why", "stance", "eyeh", "eye", "yaw", "pitch", "over", "under", "test", "twins", "mark", "lodbiasrigid"]))

    print("\n== marks (F+Reload or during a sweep: the models seen or hidden from here)")
    for r in server:
        if r.get("ev") == "mark":
            print("  " + show(r, ["src", "seen", "change", "stance", "eyeh", "eye", "h", "z", "feet", "yaw", "test", "twins"]))

    print("\n== sweeps (script camera over the feet): the heights where the user marked a change")
    cur = None
    for r in server:
        if r.get("ev") == "sweep" and r.get("state") == "start":
            cur = {"start": r, "marks": [], "last": None}
        elif r.get("ev") == "step" and cur:
            cur["last"] = r.get("h")
        elif r.get("ev") == "mark" and r.get("src") == "sweep" and cur:
            cur["marks"].append(r)
        elif r.get("ev") == "sweep" and r.get("state") == "end" and cur:
            s = cur["start"]
            print(f"  feet={s.get('feet')} yaw={s.get('yaw')} from={s.get('from')} to={s.get('to')} step={s.get('step')} "
                  f"stopped at h={r.get('last_h')}")
            if not cur["marks"]:
                print("    no mark")
            for m in cur["marks"]:
                print(f"    change {m.get('change', '?')} at h={m.get('h')} (z={m.get('z')})")
            cur = None

    print("\n== tests (F+Tactical) and twins (F+Jump)")
    for r in server:
        if r.get("ev") in ("test", "twins"):
            print(f"  {r.get('ev')}: " + show(r, ["test", "what", "lodbiasrigid", "modellodbias", "mode", "count", "eye"]))
    for r in client:
        if r.get("ev") == "umbra":
            print("  client applied: " + show(r, ["test", "cam"]))

    print("\n== static models in view (client, FindStaticModelIndex at each placed model's origin)")
    lists = []
    items = []
    for r in client:
        if r.get("ev") == "near":
            items.append(r)
        elif r.get("ev") == "nearsum":
            lists.append((r, items))
            items = []
    for s, its in lists:
        kinds = collections.Counter(re.sub(r"^[a-z0-9]+_mp_[a-z0-9]+_", "", i.get("name", "")) for i in its)
        print(f"  {show(s, ['why', 'cam', 'yaw', 'r', 'in_view', 'suspects_in_view', 'listed', 'no_smodel'])}  most: " + ", ".join(f"{k} {n}" for k, n in kinds.most_common(6)))
        if a.near:
            for i in its:
                print("    " + show(i, ["kind", "i", "name", "pos", "d", "az", "pitch", "smodel"]))
    for r in client:
        if r.get("ev") == "blink":
            print("  blink: " + show(r, ["part", "smodels"]))


if __name__ == "__main__":
    main()
