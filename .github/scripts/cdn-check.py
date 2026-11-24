"""Asks each game's image CDN for one object it served before (.github/cdn-witnesses.json).

Prints one line per witness. When one is gone, looks for where it went (the other host, the
neighbouring builds "iw9_N" / "N", the neighbouring versions in the path) with HEAD requests only,
and writes the findings to cdn-check.md for the workflow to open an issue with. Exit 1 when a
witness is gone."""
import json
import re
import sys
import time
import urllib.error
import urllib.request

UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"
HOSTS = ["cod-assets.cdn.callofduty.com", "cod-assets.cdn.blizzard.com"]


def head(url):
    for wait in (0, 2, 4):
        time.sleep(wait or 0.2)
        try:
            return urllib.request.urlopen(urllib.request.Request(url, method="HEAD", headers={"User-Agent": UA}), timeout=20).status
        except urllib.error.HTTPError as e:
            if e.code not in (429, 500, 502, 503, 504):
                return e.code
        except OSError:
            pass
    return None


def neighbours(base):
    m = re.fullmatch(r"(pc/(?:iw9_)?)(\d+)", base)
    if not m:
        return [base]
    n = int(m.group(2))
    return [f"{m.group(1)}{k}" for k in range(max(0, n - 2), n + 4)]


def versions(path):
    m = re.fullmatch(r"/(\d+)(/.*)", path)
    if not m:
        return [path]
    v = int(m.group(1))
    return [f"/{k}{m.group(2)}" for k in range(max(0, v - 2), v + 4)]


def main(witnesses, report):
    gone = []
    for w in json.load(open(witnesses, encoding="utf-8"))["witnesses"]:
        url = f"http://{w['host']}/{w['base']}{w['path']}"
        status = head(url)
        print(f"{status} {url} ({w['game']})")
        if status == 200:
            continue
        found = None
        for host in [w["host"]] + [h for h in HOSTS if h != w["host"]]:
            for base in neighbours(w["base"]):
                for path in versions(w["path"]):
                    candidate = f"http://{host}/{base}{path}"
                    if candidate != url and head(candidate) == 200:
                        found = candidate
                        break
                if found:
                    break
            if found:
                break
        gone.append((w, url, status, found))
    if not gone:
        return 0
    lines = ["One or more game image CDNs no longer serve a known object.", ""]
    for w, url, status, found in gone:
        lines.append(f"- **{w['game']}**: `{url}` answered {status}.")
        lines.append(f"  - Found at `{found}`: update the fallback in `XSUBCache.cpp` / `GameModernWarfare4.cpp` and `.github/cdn-witnesses.json`."
                     if found else "  - Not found nearby: read the new URL from the game's exe (`http://cod-assets.cdn.`).")
    open(report, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
