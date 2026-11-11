#!/usr/bin/env python3
"""The speed of each stage from a soak's log (the development build's ux0:data/bt3/log-vgl.txt).

A survey of the stages, in bt3.txt:

    mode=demo
    soak=1
    soakduel=1
    soakstages=0-34

fights a duel on each stage in turn and writes the figures every two seconds (report=1, on in the development build).
This sums them up by stage, from the log's "soak: battle <n>, a duel on stage <id>" lines:

    python3 port/vita/tools/perfsum.py log-vgl.txt

Columns: samples (two-second stretches), pictures a second (mean and lowest), the game's speed, the game thread's ms
running / waiting for the blank / waiting for the picture thread in each stretch, the list reader's us per drawn list,
the screens the GS primitives cover per picture (opaque + blended), the render thread's busy ms in each stretch.
"""
import re
import statistics
import sys

PATTERNS = {
    "speed": (re.compile(r"vita: \d+ s: speed (\d+)%, ([\d.]+) pictures/s"), lambda m: {"speed": int(m[1]), "fps": float(m[2])}),
    "game": (re.compile(r"game thread, ms: running (\d+), waiting for the blank (\d+), waiting for the picture thread (\d+)"),
             lambda m: {"run": int(m[1]), "blank": int(m[2]), "wait": int(m[3])}),
    "list": (re.compile(r"list reader: (\d+) us per drawn list"), lambda m: {"list": int(m[1])}),
    "cover": (re.compile(r"GS primitives cover ([\d.]+) screens opaque, ([\d.]+) blended"), lambda m: {"cover": float(m[1]) + float(m[2])}),
    "render": (re.compile(r"render thread \(core \d\): \d+ draws handed over \(\d+ KB\), busy (\d+) ms"), lambda m: {"render": int(m[1])}),
}
COLUMNS = ["fps", "speed", "run", "blank", "wait", "list", "cover", "render"]


def read(path):
    stages, stage = {}, None
    battle = re.compile(r"soak: battle \d+, (?:a duel|three against three) on stage (\d+)")
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = battle.match(line)
            if m:
                stage = int(m[1])
                continue
            if line.startswith("vita: Tenkaichi3Decomp"):  # (a new run: nothing counts until its first battle)
                stage = None
                continue
            if stage is None:
                continue
            values = stages.setdefault(stage, {k: [] for k in COLUMNS})
            for regex, take in PATTERNS.values():
                m = regex.search(line)
                if m:
                    for k, v in take(m).items():
                        values[k].append(v)
    return stages


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    stages = read(sys.argv[1])
    if not stages:
        sys.exit("no soak battles in that log (bt3.txt: mode=demo, soak=1)")
    mean = lambda v: f"{statistics.mean(v):.1f}" if v else "-"
    low = lambda v: f"{min(v):.1f}" if v else "-"
    print(f"{'stage':>5} {'samples':>7} {'fps':>5} {'low':>5} {'speed%':>6} {'run ms':>6} {'blank ms':>8} {'wait ms':>7}"
          f" {'list us':>7} {'screens':>7} {'render ms':>9}")
    for s in sorted(stages, key=lambda k: statistics.mean(stages[k]["fps"]) if stages[k]["fps"] else 99):
        v = stages[s]
        print(f"{s:>5} {len(v['fps']):>7} {mean(v['fps']):>5} {low(v['fps']):>5} {mean(v['speed']):>6} {mean(v['run']):>6}"
              f" {mean(v['blank']):>8} {mean(v['wait']):>7} {mean(v['list']):>7} {mean(v['cover']):>7} {mean(v['render']):>9}")


if __name__ == "__main__":
    main()
