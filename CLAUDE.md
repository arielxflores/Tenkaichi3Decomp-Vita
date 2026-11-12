# CLAUDE.md: working on Tenkaichi3Decomp-Vita

Read this first. It is the whole path from a fresh checkout to a change measured on the console: what the project
is, the rules, building, setting up the console and the emulator (an agent on the developer's computer does all of
it over the network), testing, comparing the old way against the new, and where the work stands. The details are in
the documents it points to.

**Talk to the developer in Italian.** The repository's documents, comments and commit messages are in English, in the
plain style of the existing ones.

## 1. What this is

A native PS Vita port of Dragon Ball Z: Budokai Tenkaichi 3 (PS2, USA, SLUS-21678): the game's decompiled C compiled
for the Vita, with a renderer on vitaGL. It is not an emulator. Lineage:

- `z3xox/Tenkaichi3Decomp`: the matching decompilation and its PC port (`src/`, `include/`, `config/` are the
  decompilation's; `port/` the port's);
- `arielxflores/Tenkaichi3Decomp-Vita`: the Vita port (`port/vita/`), branch `vita`;
- `manumastro/Tenkaichi3Decomp-Vita`: this fork. Its `main` is the head of **pull request #6** to
  `arielxflores/Tenkaichi3Decomp-Vita` (base `vita`): pushing to `main` updates that pull request, which other people
  see. The renderer work in progress is on `claude/fork-review-native-system-h3wioa`, on top of `main`.

How the renderer works, in one line: the game builds the PS2's display list every frame; the list reader
(`port/src/gs/gs_core.c`) reads it on its own core; the drawing thread (`port/vita/vita_vgl.c`) turns it into vitaGL
draws, handed to a render thread. Overview: `port/vita/README.md`; the 3D path and the native renderer's plan:
`docs/vita/native.md`.

## 2. Rules

- **No game data in the repository, ever** (no disc files, no dumps of them, no saves). The game files live on the
  console (`ux0:data/bt3/disc/`) and in Vita3K's storage, never under the checkout. Files fetched from the console go
  under `port/build/` (ignored by git).
- **The game's text** (`src/`, `include/`): change it only under `#ifdef PORT_VITA` (or `#ifdef PORT`), so that the
  PS2 build stays byte-identical (`docs/port/README.md`). Everything Vita-only goes in `port/vita/` or under
  `PORT_VITA`.
- **Every renderer change behind a `bt3.txt` setting**: `0` the way before, `1` the new way, `2` the new way in the
  soak's B battles only (`gVitaAbSide == 1`); a row in `docs/vita/development.md`'s table; counters in the report.
  Default off until it has been compared on a console (section 6); then on, like the other speed-ups.
- **Before a commit**: `port/vita/tools/check_syntax.sh` clean on the files changed (and `WARN=1` on the lines
  changed); with VitaSDK, the build too. In the commit message: what was checked and how, and what was not.
- **What goes to `main`** (the pull request): only what has been through the console. Experimental work stays on the
  work branch. Never rewrite `main`'s history without asking.
- **The console is the developer's**: never delete or overwrite their saves (`ux0:data/bt3/saves/`), the disc files,
  or the release (title BTTD00010); the development build is BTTD00008. Keep a copy before changing any file there
  (the scripts do). Tell the developer before a reboot or a change of `ur0:tai/config.txt`.
- **One run at a time** on the console and in Vita3K.

## 3. Building

Linux or WSL. Steps and variables: `docs/vita/building.md`. In short:

```sh
# once: VitaSDK (https://vitasdk.org) with its packages (vdpm: zlib, math-neon, vitaShaRK, SceShaccCgExt, taihen),
# libclang for Python (apt install libclang-dev python3-clang, or pip install libclang),
# the samples (git clone https://github.com/vitasdk/samples ~/vitasdk-samples), and vitaGL with the port's patch:
export VITASDK=/usr/local/vitasdk PATH=$VITASDK/bin:$PATH
port/vita/vitagl/build_vitagl.sh

# the development build (title BTTD00008, "BT3 (development)", reads every setting of bt3.txt, logs to log-vgl.txt):
BT3_VITA_DEV=1 VITA_SAMPLES=~/vitasdk-samples python3 port/vita/build.py
#   -> port/build/vita_dev/Tenkaichi3Decomp-Vita-dev.vpk, eboot.bin, bt3.unstripped.elf (for crash dumps)
# the release (BTTD00010, reads only disc, wide, heap, report):
VITA_SAMPLES=~/vitasdk-samples python3 port/vita/build.py
```

Without VitaSDK (a cloud session cannot download it): `port/vita/tools/check_syntax.sh` compiles the Vita's C with
the host's clang against the SDK's and vitaGL's headers, up to syntax and types (no link, nothing run).

## 4. The console, set up over the network (once)

The console runs HENkaku/Ensō; the developer plays the release on it, so `ur0:data/libshacccg.suprx` (the shader
compiler), the game files and usually CapUnlocker are there already. The agent sets up the rest over FTP with
`port/vita/tools/vita_setup.sh` (`VITA_IP` = the console's address); the developer is needed twice, for a few seconds.

1. **The developer**: give the console a fixed address on the network, start VitaShell, press SELECT (its FTP
   server, port 1337), say the address.
2. `VITA_IP=... port/vita/tools/vita_setup.sh check`: what is there (the FTP server, vitacompanion, `config.txt`'s
   plugins, the shader compiler, the disc files, BTTD00010 / BTTD00008).
3. **vitacompanion** (https://github.com/devnoname120/vitacompanion: FTP on 1337 and commands on 1338: `launch`,
   `quit`, `reboot`, `promote`, synthetic buttons): download its release's `vitacompanion.suprx` and
   `vitacompanion_kernel.skprx`, then `vita_setup.sh companion vitacompanion.suprx vitacompanion_kernel.skprx`: both
   into `ur0:tai/`, their two lines into `ur0:tai/config.txt` (the console's copy kept in `port/build/vita_backup/`).
4. **The developer**: restart the console once (vitacompanion is not running yet to do it). From now on nobody needs
   to touch it: `vita_setup.sh check` shows vitacompanion answering.
5. **The development build**: build it (section 3), then `vita_setup.sh install-dev`: the package unpacked on the
   computer, sent to `ux0:data/bt3_devpkg` and installed with vitacompanion's `promote` (if that fails: the `.vpk`
   installed with VitaShell by the developer, once). Later builds need only the program: the runners send
   `eboot.bin` (`--eboot`).
6. **CapUnlocker** (the fourth core; https://github.com/GrapheneCt/CapUnlocker): `vita_setup.sh capunlocker off|on`
   comments or uncomments its line in `config.txt` and reboots through vitacompanion, then waits for it to answer.
   Only for the run "without the fourth core"; leave it on otherwise.

If after a reboot vitacompanion does not answer within four minutes, stop and tell the developer: the old
`config.txt` is in `port/build/vita_backup/` (`config.first.txt` is the console's own, from before any change), and
the console may need VitaShell (or starting without plugins, see the HENkaku / Ensō guides) to put it back.

## 5. Vita3K (on the computer, once)

Vita3K (https://vita3k.org) runs the development package without the console: for "does it start, does it draw the
same", not for speed (its timing is not the console's, and it has no fourth core). Details and what its log is good
for: `docs/vita/testing.md`.

1. Vita3K and its two firmware packages (vita3k.org's quick start; `--firmware FILE.PUP` on the command line).
2. Its storage folder (its settings, "Emulated System Storage Folder"; Linux `~/.local/share/Vita3K/Vita3K`): `FS` is
   its `fs/` folder. Copy from the console over FTP (vitacompanion), into `port/build/` first if a script needs it,
   then into Vita3K: the game files `ux0:data/bt3/disc/` -> `FS/ux0/data/bt3/disc/` (about 3 GB), the shader compiler
   `ur0:data/libshacccg.suprx` -> `FS/ur0/data/`, and, to save compiling, `ux0:data/bt3/shcache/` ->
   `FS/ux0/data/bt3/shcache/`. These are the developer's files: they never go into the repository.
3. `VITA3K=<its program> VITA3K_PREF=<its storage folder>` for the runners.

## 6. Testing and comparing

The four levels (the syntax check, Vita3K, the console over the network, by hand): `docs/vita/testing.md`. The runs:

| Script | What it does |
|---|---|
| `port/vita/tools/vita_console_run.sh --settings F --seconds N [--eboot]` | One run on the console: the program and the settings sent, the game launched, waited for, closed; the log, `hang.txt`, the crash dumps' list fetched into `port/build/runs/<date>/`; `perfsum.py` over it |
| `port/vita/tools/vita3k_run.sh --settings F --seconds N` | The same in Vita3K |
| `port/vita/tools/ab_run.sh --switch S [--where vita3k\|console]` | A (S=0) and B (S=1) on the same fixed fight, then `runcmp.py`; verdict OK / LOOK / BAD |
| `port/vita/tools/runcmp.py A/log.txt B/log.txt` | Two logs side by side: health, the report's counters, the draws (`drawlog=`) |
| `port/vita/tools/perfsum.py [--ab\|--parts] log.txt` | The speed by stage; B against A; where the drawing thread's time goes |
| `port/vita/tools/filmpics.py [--pair A B] ...` | The `film=` pictures of a console's log as images, or A's beside B's |

Ready-made settings: `port/vita/tools/settings/fight_ab.txt` (one fixed fight with `drawlog`),
`soak_ab.txt` (the 25 stages with A and B interleaved). All of `bt3.txt`'s settings: `docs/vita/development.md`.

**The workflow for a renderer change** (`docs/vita/compare.md`):

1. Vita3K: `ab_run.sh --switch S`. Does B draw what A draws? Expected for `native`: OK; for `fxlow`: LOOK (its
   effects move into the smaller picture), its depth only checkable on the console.
2. The console: the speed with `soak_ab.txt` + `S=2` (`vita_console_run.sh`, then `perfsum.py --ab` and `--parts`;
   with CapUnlocker on, and once off); the pictures with `fight_ab.txt` + `film=30` (`ab_run.sh --where console`,
   then `filmpics.py --pair`), looked at by the developer or by reading the images.
3. A line in `docs/vita/status.md`'s Results table (date, commit, where, setting, stage, verdict or figures, run
   folder). A setting goes on by default, or into the pull request, only after stage 2 on the console.

When something breaks: the console's crash dump `ux0:data/psp2core-*.psp2dmp` with
https://github.com/xyzz/vita-parse-core and `port/build/vita_dev/bt3.unstripped.elf`; the port's watchdog
`ux0:data/bt3/hang-vgl.txt`; the log's start-up lines (shaders not built); `film=`, `drawlog=`, `bislist=` to find a
wrong draw (`docs/vita/development.md`).

## 7. Where the work stands

`docs/vita/status.md` (kept up to date: update it with every step) has the commits, their settings and defaults, what
has been verified (as of writing: the code checked, nothing run yet on a console or in Vita3K), and the Results
table. In short:

- `fxlow` (on `main`, on by default): the screen-filling additive effects drawn at half resolution and added back
  (TwinsanityEvolved's idea); turns itself off if its shaders are not built.
- `native` (work branch, off): the engine seam, the first steps of a native renderer: the game names its static
  geometry (the stage's meshes and animated objects, the fighters' parts) to the renderer, which keeps it without
  guessing. Next steps in `docs/vita/native.md`: the sea of Kame House, the calls sent straight to the drawing
  thread, the textures.
- **The first thing to do** with a console: section 4, then a two-minute run (`mode=demo`, `report=1`) to see the
  runners work, then stage 1 and 2 for `fxlow` and `native`, and write the results down.

## 8. Documents

| Document | About |
|---|---|
| `README.md`, `CHANGELOG.md` | For players: installing, controls, mods; what changed |
| `port/vita/README.md` | The Vita port's differences from the PC port; links |
| `docs/vita/building.md` | Building, vitaGL's patch |
| `docs/vita/development.md` | The development build, every `bt3.txt` setting, the log, a map of the files |
| `docs/vita/status.md` | The work in progress, its state, the Results table, the rules |
| `docs/vita/native.md` | The native renderer: the engine seam, what is named, the next steps |
| `docs/vita/testing.md` | The levels of testing, Vita3K and the console set up and run |
| `docs/vita/compare.md` | The old way against the new: the three stages, what to expect |
| `docs/port/README.md` | The PC port's notes (rules for `src/`) |
| `docs/systems/*.md`, `docs/README.md` | What the game's code does, system by system |
