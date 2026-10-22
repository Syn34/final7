"""Builds src/art.bin (the 725 pictures final7 shows) from Card Overview v2.1 by @mergy_.

    pip install dnfile
    python tools/build_art.py "Forbidden Memories - Card Overview v2.1.exe"

art.bin layout:  "FMA1", u32 count, count x (u16 item, u32 offset, u32 length), PNG data.
Items 1-722 are card ids; 723 = Dragon (type), 724 = Equip (type), 725 = Thunder (type).
"""
import json
import os
import struct
import sys

import dnfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPECIAL = {"#Dragon": 723, "#Equip": 724, "#Thunder": 725}


def read_resources(raw):
    p = 0

    def i32():
        nonlocal p
        v = struct.unpack_from("<i", raw, p)[0]
        p += 4
        return v

    def r7():
        nonlocal p
        v = s = 0
        while True:
            b = raw[p]
            p += 1
            v |= (b & 0x7F) << s
            s += 7
            if b < 0x80:
                return v

    assert i32() == -0x41103532  # 0xBEEFCACE
    i32()
    skip = i32()
    p += skip
    i32()
    nres, ntypes = i32(), i32()
    for _ in range(ntypes):
        n = r7()
        p += n
    while p % 8:
        p += 1
    p += 4 * nres
    pos = [i32() for _ in range(nres)]
    data_sec = i32()
    names_sec = p
    entries = []
    for ps in pos:
        p = names_sec + ps
        n = r7()
        name = raw[p:p + n].decode("utf-16le")
        p += n
        entries.append((name, data_sec + i32()))
    entries.sort(key=lambda e: e[1])
    out = {}
    for k, (name, off) in enumerate(entries):
        end = entries[k + 1][1] if k + 1 < len(entries) else len(raw)
        blob = raw[off:end]
        i = blob.find(b"\x89PNG")
        j = blob.find(b"IEND", i)
        if i >= 0 and j > 0:
            out[name] = blob[i:j + 8]
    return out


def main(exe):
    pe = dnfile.dnPE(exe)
    res = [r for r in pe.net.resources if str(r.name).endswith("Resources.resources")][0]
    images = read_resources(bytes(res.data._data))
    cards = json.load(open(os.path.join(ROOT, "tools", "cards.json"), encoding="utf-8"))
    by_name = {c["name"].lower(): c["id"] for c in cards}
    items = []
    for name, png in images.items():
        item = SPECIAL.get(name) or by_name.get(name.lower())
        if item:
            items.append((item, png))
    items.sort()
    header = b"FMA1" + struct.pack("<I", len(items))
    table, data = b"", b""
    base = len(header) + 10 * len(items)
    for item, png in items:
        table += struct.pack("<HII", item, base + len(data), len(png))
        data += png
    with open(os.path.join(ROOT, "src", "art.bin"), "wb") as fh:
        fh.write(header + table + data)
    print(len(items), "pictures written")


if __name__ == "__main__":
    main(sys.argv[1])
