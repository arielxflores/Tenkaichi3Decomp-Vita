# The renderer work in progress (handoff)

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
   transformation and a change of fighter. The line `engine seam (native=1): ...`: most calls "sent from their
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
   For the sea of Kame House (the next step of [native.md](native.md)) run `perfsum.py --parts` on that stage alone.

When a picture is wrong: `film=` takes pictures into the log, `drawlog=` and `bislist=` find the draw
([development.md](development.md)); each change has its switch to tell whether it is the cause.

## Testing without holding the console

[testing.md](testing.md): the four levels (the syntax check; Vita3K; the console driven over the network with
vitacompanion; by hand), what each shows, how to set each up once, and the two runners:
`port/vita/tools/vita3k_run.sh` and `port/vita/tools/vita_console_run.sh` (a run with a given `bt3.txt`, the log
fetched, `perfsum.py` over it). Both were written from the tools' documentation and tried only against stand-ins of
Vita3K and of the console (not on the real ones yet).

## Rules for whoever goes on

- Every change behind a `bt3.txt` switch, `0` the old way, `1` the new, `2` the new in the soak's B battles only
  (`gVitaAbSide == 1`), and a line in [development.md](development.md)'s table; counters for the log's report.
- Changes to the game's text (`src/`, `include/`) only under `#ifdef PORT_VITA`, written so that the PS2 build is
  unchanged (`docs/port/README.md`: the matching build must stay byte-identical).
- Before a commit: `port/vita/tools/check_syntax.sh` clean (and `WARN=1` on the lines changed).
- In the commit message and in [native.md](native.md) / this file: what was checked and how, and what was not.
- The changelog's "Unreleased" section for what players see; default-off development switches under "New".
