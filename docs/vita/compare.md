# Comparing the new renderer with the one before

Each change of the renderer is behind a `bt3.txt` setting: **A** is the setting at 0, the way that was there before;
**B** is the new way (1, or 2 for the soak's B battles). Both are in the same build (the branch with all the work), so
nothing else differs between them. A change is compared in two stages, the emulator first and the console second, and
its result is written down before its default changes or it goes into a pull request.

| Stage | Where | The question | Tools |
|---|---|---|---|
| 1. Does B draw what A draws? | Vita3K (or the console) | Correctness: the same draws, no errors, no crash, its own counters right | `ab_run.sh`, `runcmp.py` |
| 2. Is B faster, and does it look right? | The console | Speed, the fourth core, the pictures as the console makes them | `vita_console_run.sh`, `perfsum.py --ab`, `filmpics.py` |
| 3. Write it down, decide | | Default on or off; into the pull request or not | [status.md](status.md) |

Setting up Vita3K and the console for this once: [testing.md](testing.md). The settings under test now: `fxlow`
(the effects at a lower resolution, on by default already) and `native` (the engine seam, off).

## Stage 1: does B draw what A draws (Vita3K)

```sh
BT3_VITA_DEV=1 python3 port/vita/build.py                       # the development package
VITA3K=... VITA3K_PREF=... port/vita/tools/ab_run.sh --switch native
```

`ab_run.sh` runs the same fixed fight twice (`port/vita/tools/settings/fight_ab.txt`: the demo's first pairing on stage
4, a duel, no sweep to another pairing, the report every two seconds, every draw of lists 1800-1802 in the log), once
with `native=0` and once with `native=1`, and sets the two logs side by side with `runcmp.py`; everything goes to
`port/build/runs/ab-native-<date>/` (`A/`, `B/`, `report.txt`). Another stage: a copy of `fight_ab.txt` with another
`demostage=`, given with `--base`. The same on the console instead of Vita3K: `--where console` (and `VITA_IP`).

`report.txt` has three parts and a verdict (OK, LOOK or BAD):

1. **Health**: the lines that say something went wrong in each run (shaders not built, the watchdog, errors), and the
   figures that must stay 0 (fxlow's draws without their shader; the seam's calls sent to the cache because its table
   was full). Any of them in B and not in A: BAD.
2. **The report's counters**: each kind of report line with its numbers' means in A and B; the kinds only one run
   has, and the numbers that differ by 10% or more. The times and speeds are left out (they mean nothing in Vita3K).
3. **The draws** (`drawlog=`): each draw's state and size counted over the logged frames, and how many of them both
   runs have. The two runs need not be at the same moment frame for frame: it is the whole window that is compared.

What to expect, setting by setting:

| Setting | Health | Counters | Draws | Then |
|---|---|---|---|---|
| `native` | nothing new in B | B has the "engine seam" line: most calls "sent from their copies", "to the cache" 0 (the cache's own "model calls" line counts the seam's calls too) | ~100% the same (a named call sends the cache's own records) | OK expected: anything else is a bug of the seam |
| `fxlow` | B: "their pixel shaders built" | B has the "effects at a lower resolution" line, "draws without their shader" 0 | LOOK expected: B gathers the additive effects of one state into one draw (the additive groups) and draws a large group into target 9 (`t9`, the smaller picture; the picture added back afterwards is not a logged draw), so "more in A" lists many small blended draws at `t0` and "more in B" fewer, larger ones at `t9` or `t0`, with the same blend (`b1(src,1,0)`) | its pictures only on the console (Vita3K does not read the depth back as the console does) |

Also, once per setting: **a long run** in Vita3K (`soak_ab.txt` without its `soakab=1` line, the setting at 1, 30
minutes or more): no `hang.txt`, no crash, the seam's table never full.

## Stage 2: is B faster, and does it look right (the console)

**The speed.** One run interleaves A and B battle by battle (the same stage, the same fighters), so that the
console's temperature and the memory card's state weigh the same on both:

```sh
cp port/vita/tools/settings/soak_ab.txt /tmp/soak_native.txt && echo "native=2" >> /tmp/soak_native.txt
VITA_IP=... port/vita/tools/vita_console_run.sh --settings /tmp/soak_native.txt --seconds 5400 --eboot
python3 port/vita/tools/perfsum.py --ab port/build/runs/<date>/log.txt          # (also in perfsum_ab.txt)
python3 port/vita/tools/perfsum.py --parts port/build/runs/<date>/log.txt       # where the drawing thread's time goes
```

What counts: the mean pictures a second of B against A over the stages, each stage's B against its A (none slower by
more than about half a picture a second), the lows, and in `--parts` the parts the change was made for (the seam: "kept
walked", "program calls", the list reader's time; fxlow: the render thread's and the chip's time in effect-heavy
stretches). Run it with CapUnlocker on and, once, off (the fourth core changes where the time goes).

**The pictures.** Two short runs with small pictures in the log (`film=` reads the display's memory: on the console
only):

```sh
cp port/vita/tools/settings/fight_ab.txt /tmp/fight_film.txt && echo "film=30" >> /tmp/fight_film.txt
VITA_IP=... port/vita/tools/ab_run.sh --switch fxlow --where console --base /tmp/fight_film.txt --seconds 180
python3 port/vita/tools/filmpics.py --pair port/build/runs/ab-fxlow-<date>/A/log.txt port/build/runs/ab-fxlow-<date>/B/log.txt /tmp/pics
```

`/tmp/pics/ab_<k>.png`: A's k-th picture on the left, B's on the right. The fights need not go the same way frame for
frame: look at how things are drawn (for `fxlow`: an aura or a beam in front of a fighter and behind one, its edge
along the fighter; for `native`: the stage and the fighters whole, nothing missing or out of place), not at where
the fighters stand. A person looks at them, or an agent that can read images.

## Stage 3: write it down, decide

A line in [status.md](status.md)'s "Results" table for every stage run: the date, the build (the commit), where, the
setting, the verdict or the figures, and the run's folder name. Then:

- **Default on** only when stage 1 is OK (or LOOK explained) and stage 2 shows B no slower and its pictures right.
- **Into the pull request** to `arielxflores/Tenkaichi3Decomp-Vita` only what has been through stage 2 on a console.
- A failure: the run's folder, the verdict, and what was seen go into status.md; the setting stays off.

## Who does what

| Who | Can do |
|---|---|
| An agent in the cloud (like the one that wrote this) | Change the code, `check_syntax.sh`, read and compare logs and pictures someone uploads (`runcmp.py`, `perfsum.py`, `filmpics.py`), write the results down. Not: build (no VitaSDK), run Vita3K or reach the console |
| An agent on the developer's computer (Claude Code there) | All of the above, and build, stage 1 in Vita3K, stage 2 on the console over the network |
| The developer | The one-time set-up (Vita3K, vitacompanion, the game files), keeping the console on and charging, and the last look at the pictures |
