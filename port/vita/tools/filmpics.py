#!/usr/bin/env python3
"""The small pictures a run puts in its log (bt3.txt film=<n>: every n-th picture of a fight, about 214 x 124, from
vita_vgl.c shot_emit: "shot: <picture> <row> <x> <base64 RGB>" lines, "shot: <picture> end") made into image files.
Works on a console's log; in Vita3K the pictures come out empty (it does not fill in the display's memory).

    python3 port/vita/tools/filmpics.py log.txt OUTDIR                 each picture as OUTDIR/<picture>.ppm (.png with Pillow)
    python3 port/vita/tools/filmpics.py --pair A/log.txt B/log.txt OUTDIR
        A's k-th picture beside B's k-th, OUTDIR/ab_<k>.ppm (.png): for docs/vita/compare.md's look at two runs.
        The two runs' pictures are of the same moments only if the fights went the same way: compare what is drawn,
        not where the fighters stand.
"""
import base64
import os
import re
import sys

SHOT = re.compile(r"^shot: (\d+) (\d+) (\d+) ([A-Za-z0-9+/]+)\s*$")


def read(path):
    pics = {}
    order = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = SHOT.match(line)
            if not m:
                continue
            pic, row, x0, data = int(m[1]), int(m[2]), int(m[3]), m[4]
            px = base64.b64decode(data + "=" * (-len(data) % 4))
            if pic not in pics:
                pics[pic] = {}
                order.append(pic)
            pics[pic].setdefault(row, {})[x0] = px
    out = []
    for pic in order:
        rows = pics[pic]
        h = max(rows) + 1
        w = max(x0 + len(px) // 3 for r in rows.values() for x0, px in r.items())
        img = bytearray(w * h * 3)
        for y, r in rows.items():
            for x0, px in r.items():
                img[(y * w + x0) * 3:(y * w + x0) * 3 + len(px)] = px
        out.append((pic, w, h, bytes(img)))
    return out


def save(path, w, h, rgb):
    try:
        from PIL import Image
        Image.frombytes("RGB", (w, h), rgb).resize((w * 3, h * 3), Image.NEAREST).save(path + ".png")
        return path + ".png"
    except ImportError:
        with open(path + ".ppm", "wb") as f:
            f.write(b"P6 %d %d 255\n" % (w, h) + rgb)
        return path + ".ppm"


def beside(a, b):
    (_, wa, ha, pa), (_, wb, hb, pb) = a, b
    w, h, gap = wa + 4 + wb, max(ha, hb), 4
    img = bytearray(b"\xff\x00\xff" * (w * h))  # (magenta where neither picture is)
    for y in range(ha):
        img[y * w * 3:(y * w + wa) * 3] = pa[y * wa * 3:(y + 1) * wa * 3]
    for y in range(hb):
        o = (y * w + wa + gap) * 3
        img[o:o + wb * 3] = pb[y * wb * 3:(y + 1) * wb * 3]
    return w, h, bytes(img)


def main(argv):
    if len(argv) == 2:
        pics = read(argv[0])
        os.makedirs(argv[1], exist_ok=True)
        for pic, w, h, rgb in pics:
            save(os.path.join(argv[1], "%06d" % pic), w, h, rgb)
        print("%d pictures into %s" % (len(pics), argv[1]))
        return 0
    if len(argv) == 4 and argv[0] == "--pair":
        pa, pb = read(argv[1]), read(argv[2])
        os.makedirs(argv[3], exist_ok=True)
        n = min(len(pa), len(pb))
        for k in range(n):
            w, h, rgb = beside(pa[k], pb[k])
            save(os.path.join(argv[3], "ab_%03d" % k), w, h, rgb)
        print("A %d pictures, B %d: %d pairs into %s (A left, B right)" % (len(pa), len(pb), n, argv[3]))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
