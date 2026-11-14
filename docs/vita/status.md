# The renderer work in progress (handoff)

The whole path for an agent, from a checkout to a change measured on the console: [CLAUDE.md](../../CLAUDE.md).

Written 2026-11-12 so that another person or agent can take the work up without the conversation that produced it;
brought up to date 2026-11-13. Update it when the state changes; delete it once everything here has been merged.

## Where the work is

Everything is in one pull request to `arielxflores/Tenkaichi3Decomp-Vita` (base `vita`, PR #6): the detailed stages'
fixes, the fourth core's helper, vitaGL's state calls, the effects at a lower resolution, the first steps of a native
renderer, and the test tools. No work is held back on another branch.

The renderer's switches, in the order they came, with their defaults (each read from `ux0:data/bt3/bt3.txt` by the
**development build** only; the release reads `disc`, `heap`, `wide`, `report` and runs with the defaults; every
switch is in [development.md](development.md)'s table):

| Switch | What | Default | Run on the console (the Results below) |
|---|---|---|---|
| `ccevict`, `ccsize`, `dppick`, `dpslots`, `dpbank`, `dpframe` | The detailed stages' fixes: the cache of calls emptied, the kept parts' places, the stage buffer's banks, FRAME as "the picture" | on | The 25 stages' survey (the pull request's description) |
| `fehelper` | The uploads' numbers on the fourth core (CapUnlocker) | on | idem |
| `gxmcache` | vitaGL's GXM state calls left out when they change nothing | on | idem |
| `fxlow` | The screen-filling additive effects at a lower resolution | on | `runs/ab-fxlow`, `runs/kame-fx` |
| `dpsplit` | Kept parts' stage strips grouped by their bank too | on | `runs/check-split` |
| `native` | The engine seam: the stage's meshes and animated objects, the fighters' parts named by the game | on | `runs/ab-native`, `runs/gain-native` |
| `nativetex` | The stage's and the fighters' textures named by the game | on | `runs/tex1` |
| `updest` | Each upload's destination remembers what it left there | on | `runs/dest2` |
| `dpfast`, `fxrun` | Kept parts drawn with what does not change worked out once; the effects' particles as runs | on | `runs/fast1` |
| `texbind` | Textures bound by the names of what their uploads left | on | `runs/next1`, `runs/next2` |
| `seavtx` | The sea's corners tested and projected once a frame each | on | `runs/next1`, `runs/next2` |
| `esdirect` | The seam's call packets done as what they are (an experiment: no gain measured) | off | `runs/next1` |

Removed after a console run: `dpjoin` (never a pair). Also on by default, with all of the above: `runs/final` (10
minutes, duels and three against three, six stages).

## State of verification

Run on a console (PCH-1004, firmware 3.65, CapUnlocker on, the development build, driven over the network through
vita-agent-use): every switch above, A against B and then with everything on by default; the figures and verdicts are
in the Results table. No crash, no hang, no texture found changed, pictures right in the screenshots.

Every file the pull request touches compiles, up to syntax and types, for the Vita's processor
(`port/vita/tools/check_syntax.sh`), and every change to the game's text is under `#ifdef PORT_VITA` or behind a macro
that is the original call without it (the PS2 matching build and the PC port see none of it).

Not run yet:

- **without the fourth core** (CapUnlocker off): `fehelper` then does nothing, and the drawing thread works out the
  uploads' numbers itself; the room the native renderer gives is meant for this case;
- **the release build** (BTTD00010): the same code with the defaults, without the development build's switches and
  log;
- **Vita3K**: stage 1 of [compare.md](compare.md) was skipped, the console runs went straight to stage 2;
- the PS2 matching build and the PC port, for byte identity (the guards were checked by reading).

## How to test on the console

Build the development package (`BT3_VITA_DEV=1`, [building.md](building.md)), install it next to the release.

1. **It starts and draws.** No settings: start a fight. The log `ux0:data/bt3/log-vgl.txt` must have
   `vgl: the effects at a lower resolution: their pixel shaders built` (else `... are off: their pixel shaders (N)
   were not built`, and the shader compiler's message above it).
2. **The counters.** Every two seconds (`report=1`): the effects' line, **"draws without their shader" 0**; the
   engine seam's line, **"to the cache (table full)" 0**; the named textures' line, **found changed 0**; texbind's
   `wrong` 0.
3. **A switch against the old way, A/B.** In `bt3.txt` (or `port/vita/tools/settings/soak_ab.txt`,
   `native_next_ab.txt`):

   ```
   mode=demo
   soak=1
   soakduel=1
   soakstages=0-24
   soakab=1
   texbind=2
   ```

   (any switch with the value 2: its B battles have it, its A battles not). `python3 port/vita/tools/perfsum.py --ab
   log-vgl.txt` sets each stage's B battles against its A battles; `--parts` shows where the drawing thread's time
   goes.
4. **Without the fourth core**: the same with CapUnlocker off ([CLAUDE.md](../../CLAUDE.md) section 4, step 5).

When a picture is wrong: `film=` takes pictures into the log, `drawlog=` and `bislist=` find the draw
([development.md](development.md)); each change has its switch to tell whether it is the cause.

## Testing without holding the console

[testing.md](testing.md): the four levels (the syntax check; Vita3K; the console driven over the network; by hand),
what each shows, how to set each up once, and the runners. `port/vita/tools/vita_agent_run.py` (vita-agent-use) ran
every console run in the Results table. `port/vita/tools/vita_console_run.sh` (vitacompanion) and
`port/vita/tools/vita3k_run.sh` were written from the tools' documentation and tried only against stand-ins of the
console and of Vita3K (not on the real ones yet).

## Comparing A and B, and the results

The workflow: [compare.md](compare.md) (stage 1 in Vita3K: does the new way draw what the old one draws; stage 2 on
the console: is it faster and does it look right; stage 3: write it down here, then decide).

### Results

One line per stage run (the newest first). The run's folder is `port/build/runs/...` on the computer that ran it.

| Date | Build (commit) | Where | Setting | Stage | Verdict / figures | Run |
|---|---|---|---|---|---|---|
| 2026-11-13 | `e894c7a` (release, BTTD00011, built with `docker_build.sh`) | Console, by hand (the developer) | the defaults | 2 (play) | The release without the log's samples of the picture's draws (`report=1` brings them back) and its log kept under 1 MB (`log-prev.txt`): played, works. Verdict: in the pull request | (by hand) |
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
