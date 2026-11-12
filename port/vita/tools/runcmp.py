#!/usr/bin/env python3
"""Two test runs of the development build set side by side: A (a setting at 0, the way before) and B (at 1, the new
way). For the comparison workflow of docs/vita/compare.md; the logs come from vita3k_run.sh or vita_console_run.sh
(their log.txt: one run's part of ux0:data/bt3/log-vgl.txt).

    python3 port/vita/tools/runcmp.py A/log.txt B/log.txt [--top 15] [--time]

Three parts, then a verdict:
  1. Health: the lines that say something went wrong (shaders not built, the watchdog, errors) in each run, and the
     figures that must be 0 (fxlow's draws without their shader, the engine seam's calls sent to the cache).
  2. The report's counters (report=1): every line of the two-second report with its numbers taken out is a kind of
     line; for each kind, the mean of each number over the run in A and in B, and the kinds whose numbers differ most.
     Lines of times and speeds are left out unless --time (they mean nothing in Vita3K; on the console use perfsum.py).
  3. The draws (drawlog=a-b in both runs): each draw's state and size (its frame, place, path and texture's number
     left out: they differ from run to run), counted over all the frames logged; how many of A's draws B has too.
     The two runs need not be at the same moment frame for frame: the comparison is over the whole window.

The verdict is a summary for a person or an agent: OK, LOOK (differences worth a look) or BAD (a must not met).
"""
import collections
import re
import statistics
import sys

BAD_LINE = re.compile(r"not compiled|not built|WATCHDOG|ERROR|[Ee]rror|crash|fault|refused|abort", re.I)
TIME_WORDS = re.compile(r"\bms\b|\bus\b|pictures/s|cycles|MHz|speed \d+%|per second|seconds?\b")
NUM = re.compile(r"-?\d+(?:\.\d+)?")
DL = re.compile(r"^dl (\d+) (-?\d+) k(\d+) t(\d+) n(\d+) x(\d+) tex(\d+) (e\d+ b\d+\(\d+,\d+,\d+\) a\d+\(\d+,[0-9a-f]+\) "
                r"z\d+\(\d+,\d+\) m[0-9a-f]+ d\d+ f\d+ sc-?\d+,-?\d+,-?\d+,-?\d+)")
MUSTS = [  # (what, a pattern with one number, the most it may be)
    ("fxlow: draws without their shader", re.compile(r"effects at a lower resolution: .* (\d+) draws without their shader"), 0),
    ("native: calls sent to the cache (table full)", re.compile(r"engine seam .*?(\d+) to the cache \(table full\)"), 0),
]


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def health(lines):
    bad = [l for l in lines if BAD_LINE.search(l) and not l.startswith("dl ")]
    musts = {}
    for what, pat, most in MUSTS:
        vals = [int(m[1]) for m in (pat.search(l) for l in lines) if m]
        musts[what] = (sum(vals), most, len(vals))
    return bad, musts


def counters(lines, with_time):
    kinds = collections.defaultdict(list)
    for l in lines:
        s = l.strip()
        if not (s.startswith("vita:") or s.startswith("vgl:") or s.startswith("gs:")) or s.startswith("vita: Tenkaichi3Decomp"):
            continue
        if not with_time and TIME_WORDS.search(s):
            continue
        nums = [float(x) for x in NUM.findall(s)]
        if not nums:
            continue
        kinds[NUM.sub("#", s)].append(nums)
    out = {}
    for k, rows in kinds.items():
        width = min(len(r) for r in rows)
        out[k] = (len(rows), [statistics.fmean(r[i] for r in rows) for i in range(width)])
    return out


def draws(lines):
    keys = collections.Counter()
    frames = collections.Counter()
    for l in lines:
        m = DL.match(l)
        if m:
            frame, _idx, _k, t, n, x, tex, rest = m.groups()
            keys[(t, n, x, tex != "0", rest)] += 1
            frames[frame] += 1
    return keys, frames


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="Two test runs side by side (docs/vita/compare.md)")
    ap.add_argument("a", help="A's log.txt (the setting at 0)")
    ap.add_argument("b", help="B's log.txt (the setting at 1)")
    ap.add_argument("--top", type=int, default=15, help="lines shown in each list")
    ap.add_argument("--time", action="store_true", help="the report's times and speeds too")
    opt = ap.parse_args(argv)
    top = opt.top
    args = [opt.a, opt.b]
    argv = ["--time"] if opt.time else []
    la, lb = read(args[0]), read(args[1])
    verdict, notes = "OK", []

    print("== 1. Health")
    for name, lines in (("A", la), ("B", lb)):
        bad, musts = health(lines)
        print(f"  {name}: {len(lines)} lines; {len(bad)} that say something went wrong")
        for l in bad[:8]:
            print(f"     {l[:160]}")
        for what, (total, most, n) in musts.items():
            if n:
                ok = total <= most
                print(f"     {what}: {total} over {n} reports{'' if ok else '   <-- must be ' + str(most)}")
                if not ok and name == "B":
                    verdict = "BAD"
                    notes.append(f"B: {what} = {total}")
        if bad and name == "B":
            nb_a = len(health(la)[0])
            if len(bad) > nb_a:
                verdict = "BAD" if verdict == "BAD" else "LOOK"
                notes.append(f"B has {len(bad)} lines that say something went wrong, A {nb_a}")

    print("\n== 2. The report's counters (means over the run)" + ("" if "--time" in argv else "; times and speeds left out (--time)"))
    ca, cb = counters(la, "--time" in argv), counters(lb, "--time" in argv)
    only_a = [k for k in ca if k not in cb]
    only_b = [k for k in cb if k not in ca]
    for name, only in (("only in A", only_a), ("only in B", only_b)):
        if only:
            print(f"  {name}:")
            for k in only[:top]:
                print(f"     {k[:150]}")
    diffs = []
    for k in ca.keys() & cb.keys():
        (na, va), (nb, vb) = ca[k], cb[k]
        for i, (x, y) in enumerate(zip(va, vb)):
            if x != y and max(abs(x), abs(y)) >= 2:
                rel = abs(x - y) / max(abs(x), abs(y))
                if rel >= 0.10:
                    diffs.append((rel, k, i, x, y))
    diffs.sort(reverse=True)
    print(f"  kinds of line in both: {len(ca.keys() & cb.keys())}; numbers that differ by 10% or more: {len(diffs)}")
    for rel, k, i, x, y in diffs[:top]:
        print(f"     #{i + 1} {x:.1f} -> {y:.1f} ({rel * 100:.0f}%)  {k[:120]}")

    print("\n== 3. The draws (drawlog=)")
    da, fa = draws(la)
    db, fb = draws(lb)
    if not da or not db:
        print("  no draws logged in " + ("A and B" if not da and not db else "A" if not da else "B") + " (bt3.txt: drawlog=a-b)")
    else:
        ta, tb = sum(da.values()), sum(db.values())
        common = sum((da & db).values())
        pa, pb = ta / len(fa), tb / len(fb)
        share = common / max(ta, tb)
        print(f"  A: {ta} draws in {len(fa)} frames ({pa:.0f} a frame);  B: {tb} draws in {len(fb)} frames ({pb:.0f} a frame)")
        print(f"  the same state and size in both: {common} ({share * 100:.1f}% of the larger)")
        for name, extra in (("more in A", da - db), ("more in B", db - da)):
            if extra:
                print(f"  {name}:")
                for key, c in extra.most_common(min(top, 8)):
                    t, n, x, tex, rest = key
                    print(f"     {c:5d} x  t{t} n{n} x{x} {'tex ' if tex else ''}{rest}")
        if share < 0.95 or abs(pa - pb) > 0.05 * max(pa, pb):
            verdict = "BAD" if verdict == "BAD" else "LOOK"
            notes.append(f"draws: {share * 100:.1f}% the same, {pa:.0f} against {pb:.0f} a frame")

    print(f"\n== Verdict: {verdict}" + ("" if not notes else ": " + "; ".join(notes)))
    return 0 if verdict == "OK" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
