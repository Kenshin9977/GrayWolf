#!/usr/bin/env python3
"""Exercise IW8 soundbank block placement with synthetic zones, without game data."""

import argparse
import itertools
import os
from pathlib import Path
import struct
import subprocess
import tempfile


INLINE = 0xFFFFFFFFFFFFFFFE
SHARED = 0xFFFFFFFFFFFFFFFF


def put64(buf, at, value):
    struct.pack_into("<Q", buf, at, value)


def sound_hash(name):
    value = 5381
    for byte in name.lower().encode("ascii"):
        value = (value * 65599 + byte) & 0xFFFFFFFF
    return value


def fixture(variants, detail, metadata):
    # Two zone-level soundbanks, no script strings. The asset list occupies the
    # first 32 bytes of block 5; headers are temporary, names have byte alignment.
    zone = bytearray(64)
    struct.pack_into("<I", zone, 16, 2)
    put64(zone, 24, INLINE)
    for at in (32, 48):
        struct.pack_into("<I", zone, at, 21)  # soundbank
        put64(zone, at + 8, INLINE)
    bank = len(zone)
    zone.extend(bytes(512))
    put64(zone, bank, INLINE)
    struct.pack_into("<I", zone, bank + 32, 3)
    put64(zone, bank + 40, INLINE)
    zone.extend(b"bank_inline\0")
    lists = len(zone)
    zone.extend(bytes(96))
    for i, name in enumerate(("first_alias", "second_alias", "second_alias")):
        put64(zone, lists + 32 * i, INLINE if i < 2 else (0x15 << 32) | 409)
        struct.pack_into("<I", zone, lists + 32 * i + 8, sound_hash(name))
    put64(zone, lists + 16, variants)
    struct.pack_into("<I", zone, lists + 24, 1)
    zone.extend(b"first_alias\0")
    variant = len(zone)
    zone.extend(bytes(32))
    put64(zone, variant, INLINE)
    struct.pack_into("<I", zone, variant + 8, sound_hash("sound_shot"))
    put64(zone, variant + 16, detail)
    put64(zone, variant + 24, metadata)
    zone.extend(b"sound_shot\0")
    file_info = len(zone)
    zone.extend(bytes(40))
    put64(zone, file_info, INLINE)
    zone.extend(b"file_detail\0")
    params = len(zone)
    zone.extend(bytes(176))
    second_name = len(zone)
    zone.extend(b"second_alias\0")

    other = len(zone)
    zone.extend(bytes(512))
    put64(zone, other, INLINE)
    struct.pack_into("<I", zone, other + 32, 1)
    put64(zone, other + 40, INLINE)
    other_name = len(zone)
    zone.extend(b"bank_references\0")
    absolute_field = len(zone)
    zone.extend(bytes(32))
    put64(zone, absolute_field, (5 << 32) | 441)
    struct.pack_into("<I", zone, absolute_field + 8, sound_hash("second_alias"))

    # Independent expected virtual offsets: list start align8(32 + 12) = 48;
    # 3 lists end at 144, first_alias ends at 156, variant [160,192),
    # sound_shot ends at 203, file [208,248), file_detail ends at 260,
    # parameters [264,440), second_alias [440,453). Markers must not change them.
    expected = {variant: 160, file_info: 208, params: 264,
                second_name: 440, other_name: 453}
    return zone, expected, second_name, lists + 64, absolute_field


def read_walk(path):
    data = path.read_bytes()
    at = 0

    def take(fmt):
        nonlocal at
        result = struct.unpack_from(fmt, data, at)
        at += struct.calcsize(fmt)
        return result

    def text():
        nonlocal at
        size, = take("<I")
        result = data[at:at + size].decode("utf-8")
        at += size
        return result

    assert take("<4sII") == (b"BO4W", 2, 1), "unexpected walk format"
    text()  # input path
    assert text() == "walked to the end", "incomplete synthetic walk"
    assert take("<B") == (0,), "zone parse failed"
    owners, = take("<I")
    for _ in range(owners):
        text()
    count, = take("<Q")
    sizes = take(f"<{count}q")
    take(f"<{count}H")
    memory = take(f"<{count}q")
    positions = take(f"<{count}q")
    assert at == len(data), "trailing walk data"
    return list(zip(positions, memory, sizes))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--profile", type=Path,
                        default=Path(__file__).resolve().parents[1] / "zonekit/profiles/iw8")
    args = parser.parse_args()
    exe, profile = args.exe.resolve(), args.profile.resolve()
    with tempfile.TemporaryDirectory(prefix="graywolf-iw8-layout-") as tmp:
        tmp = Path(tmp)
        for markers in itertools.product((INLINE, SHARED), repeat=3):
            label = "/".join("-1" if m == SHARED else "-2" for m in markers)
            zone, expected, second_name, relative, absolute = fixture(*markers)
            raw, walk, paths = tmp / "synthetic.zone", tmp / "zone.walk", tmp / "zones.txt"
            raw.write_bytes(zone)
            paths.write_text(str(raw) + "\n", encoding="utf-8")
            result = subprocess.run(
                [str(exe), "zone-walk", str(paths), "--profile", str(profile),
                 "--layout", "--out", str(walk), "--threads", "1"],
                cwd=tmp, env={**os.environ, "GRAYWOLF_NO_UPDATE": "1"},
                capture_output=True, timeout=30,
                creationflags=0x08000000 if os.name == "nt" else 0,
            )
            assert result.returncode == 0, f"{label}: zone-walk failed ({result.returncode})"
            reads = read_walk(walk)
            memory = {p: m for p, m, _ in reads}
            for p, offset in expected.items():
                assert memory.get(p) == (5 << 40) | offset, (
                    f"{label}: read at {p} placed at {memory.get(p)}, expected block 5 + {offset}")
            # One alias refers within its bank, the next bank refers back globally.
            # Both must select the same exact bytes, rather than another valid string.
            for field, base in ((relative, 32), (absolute, 0)):
                value, = struct.unpack_from("<Q", zone, field)
                offset = (value & 0xFFFFFFFF) - 1 + base
                targets = [p + offset - (m & 0xFFFFFFFFFF) for p, m, n in reads
                           if m >= 0 and m >> 40 == 5
                           and (m & 0xFFFFFFFFFF) <= offset < (m & 0xFFFFFFFFFF) + abs(n)]
                assert targets == [second_name], f"{label}: alias reference resolved to {targets}"
            print(f"PASS variants/detail/metadata {label}: placements and both references")


if __name__ == "__main__":
    main()
