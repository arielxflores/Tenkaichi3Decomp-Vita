# The renderer work in progress (handoff)

The whole path for an agent, from a checkout to a change measured on the console: [CLAUDE.md](../../CLAUDE.md).

Written 2026-11-12 so that another person or agent can take the work up without the conversation that produced it.
Update it when the state changes; delete it once everything here has been run on a console and merged.

## Where the work is

| Branch | What it holds |
|---|---|
| `main` (this fork) | The head of the pull request to `arielxflores/Tenkaichi3Decomp-Vita` (base `vita`, PR #6): the detailed stages' fixes, the fourth core's helper, vitaGL's state calls, the test tools, and the effects at a lower resolution (`fxlow`) |
| `claude/fork-review-native-system-h3wioa` | `main` and, on top of it, the engine seam (`native`): the first steps of a native renderer. Not in the pull request |

The commits, oldest first:

| Commit | What | Switch | Default |
|---|---|---|---|
| `5e043db` | The screen-filling additive effects at a lower resolution | `fxlow`, `fxlowmin`, `fxlowdiv` | (see the next) |
| `bc53450` | `fxlow` on by default, as the other speed-ups; off by itself if its shaders are not built | `fxlow=0` goes back | on |
| `effc5ad` | The engine seam: the stage's meshes (VU1 programs 4, 7) named by the game | `native`, `nativecheck` | off |
| `b935734` | The seam also for the fighters' parts (0, 1, 2) and the stage's animated objects (8) | `native` | off |

The switches are read from `ux0:data/bt3/bt3.txt` by the **development build** only (the release reads `disc`,
`heap`, `wide`, `report`): see [development.md](development.md).

## State of verification

**None of these four commits has been run on a console or an emulator yet.** What was checked:

- every file they touch compiles, up to syntax and types, for the Vita's processor against the SDK's and vitaGL's
  headers: `port/vita/tools/check_syntax.sh` (no VitaSDK needed; no link, nothing run; [testing.md](testing.md));
- by reading: the order of the draws (the effects' groups are drawn at their place; a named call is the cache's call,
  found another way), and that every change to the game's text is under `#ifdef PORT_VITA` (the PS2 matching build
  and the PC port see none of it).

Not checked, and only a console (or Vita3K, below) can show it:

- that the Cg shaders of `fxlow` (`kVertZ`, the variants + 2048 built in `frag_variant`) are taken by the console's
  shader compiler (if not, `fxlow` turns itself off: the log says so);
- that the effects' depth test reads the depth the right way up and at the right scale (`kFragZTest`: the depth
  texture's rows from the top, `uZ.x` / `uZ.y` the view's place in the display);
- that `native=1` draws the same pictures and is faster; that no named call is changed by the game while it is kept
  (`nativecheck` reads each again every 512th call).

## How to test on the console

Build the development package (`BT3_VITA_DEV=1`, [building.md](building.md)), install it next to the release.

1. **It starts and draws.** No settings: start a fight. The log `ux0:data/bt3/log-vgl.txt` must have
   `vgl: the effects at a lower resolution: their pixel shaders built` (else `... are off: their pixel shaders (N)
   were not built`, and the shader compiler's message above it).
2. **The effects (fxlow).** In a fight with auras and beams: the effects in front of and behind the fighters as with
   `fxlow=0` (nothing drawn through a fighter or the ground), no square edges beyond a two-pixel step along a fighter
   inside a beam. Every two seconds (`report=1`) the line
   `effects at a lower resolution: N groups drawn smaller (S screens), M drawn on the picture, D draws without their shader`:
   N > 0 in effect-heavy moments, **D must stay 0**.
3. **The seam (native).** `native=1`: the stage, its animated objects and the fighters as with `native=0`, through a
   transformation and a change of fighter. The line `engine seam (native): ...`: most calls "sent from their
   copies", **"to the cache (table full)" 0**, "of an old generation" only after loads and transformations.
4. **The speed, A/B.** In `bt3.txt`:

   ```
   mode=demo
   soak=1
   soakduel=1
   soakstages=0-24
   soakab=1
   fxlow=2
   ```

   then the same with `native=2` instead of `fxlow=2`. `python3 port/vita/tools/perfsum.py --ab log-vgl.txt` sets each
   stage's B battles (the setting on) against its A battles (off); `--parts` shows where the drawing thread's time goes.
   For the sea of Kame House (stage 12; the next step of [native.md](native.md)) run `perfsum.py --parts` on that stage alone.

When a picture is wrong: `film=` takes pictures into the log, `drawlog=` and `bislist=` find the draw
([development.md](development.md)); each change has its switch to tell whether it is the cause.

## Testing without holding the console

[testing.md](testing.md): the four levels (the syntax check; Vita3K; the console driven over the network with
vitacompanion; by hand), what each shows, how to set each up once, and the two runners:
`port/vita/tools/vita3k_run.sh` and `port/vita/tools/vita_console_run.sh` (a run with a given `bt3.txt`, the log
fetched, `perfsum.py` over it). Both were written from the tools' documentation and tried only against stand-ins of
Vita3K and of the console (not on the real ones yet).

## Comparing A and B, and the results

The workflow: [compare.md](compare.md) (stage 1 in Vita3K: does the new way draw what the old one draws; stage 2 on
the console: is it faster and does it look right; stage 3: write it down here, then decide).

### Results

One line per stage run (the newest first). The run's folder is `port/build/runs/...` on the computer that ran it.

| Date | Build (commit) | Where | Setting | Stage | Verdict / figures | Run |
|---|---|---|---|---|---|---|
| 2026-11-12 | `3f8b443` (dev) | Console, vita-agent-use | `texbind=1 esdirect=1 seavtx=1 updestlog=1`, soak, stages 12, 22, 4, 210 s | 2 (counters, pictures) | texbind after the fix (a block written pixel by pixel had kept its last owner: a palette sharing its block stayed bound; small destinations now checked by their blocks' writes too): **0 wrong** in ~15,000 checks, 20,000-38,000 textures / 2 s found by their names; no crash; pictures right. Verdict: `texbind`, `seavtx` on by default, `esdirect` off | runs/next2 |
| 2026-11-12 | `99dfa15` (dev) | Console, vita-agent-use | `upprof=1 texbind=2 esdirect=2 seavtx=2 dpjoin=2`, soakab, stages 12, 4, 22, 420 s | 1-2 (counters, cost, pictures) | fps at the cap on both sides (B-A +0.6, 0.0, -0.2). Texture lookups (upprof, ms / 2 s, A then B): 157 -> 118 (4), 226 -> 186 (12), 129 -> 114 (22); drawing thread 890 -> 779 (4), 840 -> 818 (12), 734 -> 757 (22). texbind **0.8-1.5% of its checks wrong** (fixed: the line above). seavtx on Kame House: 63% of the corners' work found done, the surfaces 4.9 -> 4.5 ms a frame. dpjoin: never a pair (removed). esdirect: 6,500 call packets / 2 s, the list reader -34, +21, +87 ms / 2 s (no gain seen) | runs/next1 |
| 2026-11-12 | all on by default (dev) | Console, vita-agent-use | the defaults (native, nativetex, updest, dpfast, fxrun, fxlow, dpsplit on), soak with duels and three against three (changes of fighter), stages 4, 12, 20, 22, 23, 13, 10 minutes | 2 (health, speed, pictures) | 10 battles, no crash, no hang; named textures found changed 0; texture keys from a destination wrong 0 (289 reports); the seam's table never full. fps 29.3-29.6 on every stage (lows 22.2-25.3); the list reader 5.4-8.5 ms a drawn list. 24 screenshots right. Verdict: on by default | `runs/final` |
| 2026-11-12 | dpfast, fxrun (dev) | Console, vita-agent-use | `native=1 nativetex=1 updest=1 dpfast=2 fxrun=2`, soakab, stages 4, 12 | 1-2 (cost, pictures) | Drawing thread 765 -> 700 and 820 -> 763 ms / 2 s; GS primitives on Kame House 236 -> 206, GS packets 127 -> 102; ~1,000 particles / 2 s decoded as runs. A first run (all1) showed the stage's polygons drawn huge: dpfast's stage uniform block cleared three rows of the screen matrix (fixed before fast1; pictures right since). Tried and taken out: each kept group remembering its texture (never found the same: the game writes the stage's materials into one place and a fighter's palettes into one); the triangle runs into the additive groups take none in a fight (they write the alpha the fighters' outline reads, as the full way finds too) | `runs/fast1` |
| 2026-11-12 | updest (dev) | Console, vita-agent-use | `native=1 nativetex=1 upprof=1 updest=2`, soakab, stages 4, 12 | 1-2 (counters, cost, pictures) | upprof (every upload and texture lookup timed, ms / 2 s, A then B): stage 4 uploads by the block 233 -> 175-184, texture lookups 209 -> 126-130, small uploads written 62 -> 70-76, all 551 -> 386-432; stage 12 all 453 -> 433. 9,000-10,600 texture keys / 2 s from a destination, 0 wrong in ~2,400 checks (every 16th). fps the same; pictures right. The rest of the lookups' cost is their number (~30,000 / 2 s: every kept group's and strip's state) | `runs/dest2` |
| 2026-11-12 | nativetex (dev) | Console, vita-agent-use | `native=1 nativetex=2`, soakab, stages 4, 12 | 1-2 (counters, speed, pictures) | ~6,000 uploads / 2 s numbered from their names, 0 found changed in the checks (every 128th), 120 named at a fight's start. The fourth core's helper 383 -> 28, 181 -> 52 ms / 2 s; the drawing thread's uploads (its own clock) 2156 -> 1827, 1505 -> 1345 us a blank; the list reader's GS packets +50 ms / 2 s (the first readings and the checks: it has the room). fps the same. Pictures right (6 B screenshots). Default off for now | `runs/tex1` |
| 2026-11-12 | `a5c4005` (dev) | Console, vita-agent-use | `native=2`, soakab, stages 4, 20, 12, two rounds (A/B, then B/A) | 2 (speed) | The gain over main (fxlow, dpsplit on both sides): fps the same (29.2 / 29.2, at the 30 cap); drawing thread 828 -> 560, 891 -> 687, 778 -> 594 ms / 2 s (-23 to -32%); list reader 314 -> 106, 430 -> 151, 555 -> 212 (-62 to -66%); the seam's table never full (131 reports). With native=1 the drawing thread's time is GS primitives 100-275, uploads 125-180, GS packets 100-180, kept parts drawn 115-200 ms / 2 s; the render thread 830 ms / 2 s on Kame House (20,700 draws) | `runs/gain-native` |
| 2026-11-12 | `513c136` (dev) | Console, vita-agent-use | `fxlow=2`, soakab, stage 12 (Kame House) | 2 (pictures) | Kame House is stage **12** (the island, the pink house; 22 is a canyon with a river, 23 a plain with a lake, 13 Namek at night, 16 space, 15 rocky mountains in rain). Splashes, auras, a ki blast over the sea drawn right with fxlow; an aura cut straight where it meets the sea is there with fxlow=0 too (the game's depth test) | `runs/kame-fx` |
| 2026-11-12 | `f5dccab` (dev) | Console, vita-agent-use | `native=1 dpsplit=1`, one fight each, stages 12, 15 | 2 (speed, parts) | Kame House 29.5 fps (the November 2026 benchmark before the detailed stages' fixes: 24.9). The sea costs the game thread ~8.5 ms a frame (972 triangles looked at, ~900 drawn through the ordering table), the drawing thread's GS primitives ~230 ms / 2 s, the render thread 826 ms / 2 s (20,800 draws): no thread is full, so native.md's step 1 (the sea) would give processor time, not pictures a second | `runs/kame` |
| 2026-11-12 | `513c136` + dpsplit (dev) | Console, vita-agent-use | `native=1 dpsplit=1`, one fight each, stages 20, 13, 16 | 2 (counters) | "not kept for another reason" 0 (before: "indices beyond a bank" in every report of a first fight); kept parts walked 1-20 ms / 2 s in first fights (stage 20: 20 against 272-467); the seam's line: 9,000-25,000 calls / 2 s sent from their copies, "to the cache (table full)" 0. dpsplit on by default | `runs/check-split` |
| 2026-11-12 | `513c136` (dev) | Console, vita-agent-use | `native=2`, soakab, stages 20, 23 | 2 (speed, pictures) | fps B-A: 20: -0.4 (29.4 / 29.0), 23: +1.1 (28.1 / 29.2), all +0.3, near the 30 cap. List reader 500 -> 229 ms / 2 s (20) and 469 -> 189 (23); drawing thread 770 -> 649 and 877 -> 756. Pictures (8 B screenshots): stage, animated objects and fighters whole. The seam's own line was not printed (inside the processor counters' block; fixed in f5dccab). Verdict: works, B no slower; default still off until a fight with a transformation is looked at | `runs/ab-native` |
| 2026-11-12 | `8d19e3f` (dev) | Console (PCH-1004, 3.65, CapUnlocker) through vita-agent-use | `fxlow=2`, soakab, stages 4, 20, 22 | 2 (speed, pictures) | Shaders built; "draws without their shader" 0 in all 110 reports; B drew 1-3 groups smaller per 2 s (3-9 screens). fps B-A: 4: -0.4, 20: -1.0, 22: +0.4, all -0.3, within the order of the battles: the first fight on stage 20 in each round (A in round 1, B in round 2) walked its kept parts 270-470 ms / 2 s against ~30 in the second (`513c136` logs why). Render thread on 22: 778 -> 665 ms / 2 s, draws handed over 17,200 -> 13,800. Pictures (8 B screenshots: blasts, dust, auras): drawn right, in front of and behind the fighters. Run stopped by hand at ~9 of 10 minutes (11 fights). Verdict: stays on | `runs/ab-fxlow` |

## Rules for whoever goes on

- Every change behind a `bt3.txt` switch, `0` the old way, `1` the new, `2` the new in the soak's B battles only
  (`gVitaAbSide == 1`), and a line in [development.md](development.md)'s table; counters for the log's report.
- Changes to the game's text (`src/`, `include/`) only under `#ifdef PORT_VITA`, written so that the PS2 build is
  unchanged (`docs/port/README.md`: the matching build must stay byte-identical).
- Before a commit: `port/vita/tools/check_syntax.sh` clean (and `WARN=1` on the lines changed).
- In the commit message and in [native.md](native.md) / this file: what was checked and how, and what was not.
- The changelog's "Unreleased" section for what players see; default-off development switches under "New".
