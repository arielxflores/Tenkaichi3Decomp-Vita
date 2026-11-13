# CLAUDE.md: working on Tenkaichi3Decomp-Vita

Read this first. It is the whole path from a fresh checkout to a change measured on the console: what the project
is, the rules, building, setting up the console and the emulator (an agent on the developer's computer does all of
it over the network), testing, comparing the old way against the new, and where the work stands. The details are in
the documents it points to.

The repository's documents, comments and commit messages are in English, in the plain style of the existing ones. A
developer's own instructions (the language to answer in, their fork and its pull request, their console) go in a
`CLAUDE.local.md` beside this file, which git ignores.

## 1. What this is

A native PS Vita port of Dragon Ball Z: Budokai Tenkaichi 3 (PS2, USA, SLUS-21678): the game's decompiled C compiled
for the Vita, with a renderer on vitaGL. It is not an emulator. Lineage:

- `z3xox/Tenkaichi3Decomp`: the matching decompilation and its PC port (`src/`, `include/`, `config/` are the
  decompilation's; `port/` the port's);
- `arielxflores/Tenkaichi3Decomp-Vita`: the Vita port (`port/vita/`), branch `vita`, which pull requests from
  forks go to. Pushing to a branch a pull request is open from updates that pull request, which other people see.

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
- **What goes to a pull request**: only what has been through the console. Experimental work stays on a branch of
  its own. Never rewrite the history of a branch a pull request is open from without asking.
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

The developer's console: a PS Vita on HENkaku/Ensō, on which the release is usually installed, so
`ur0:data/libshacccg.suprx` (the shader compiler), the game files and often CapUnlocker are there already. **Ask the
developer its firmware** (Settings > System > System Information) before choosing a way to drive it. Two ways, which
cannot be installed together:

| | vita-agent-use (preferred) | vitacompanion (fallback) |
|---|---|---|
| What | https://github.com/shoui520/vita-agent-use: made for AI agents; JSON CLI; TLS with pairing | https://github.com/devnoname120/vitacompanion: FTP on 1337, commands on 1338 |
| Firmware | **3.65** (its README) | any with taiHEN |
| Gives | install, launch, close, upload / download with hashes, **real screenshots**, input, CPU / FPS / memory samples, crash dumps and error dialogs pushed as they happen, "runs" (deploy, launch, wait for a log line, collect, clean up) | upload / download, launch, quit, reboot, promote, synthetic input |
| Its rules | its own `AGENTS.md` is the law (read it first): `serve` running for the whole session; a person approves the pairing and any change of `tai/` on the console; `config.txt` changed only by its `config plan` / `config apply` (which wants VitaShell 2.02 at `uma0:app/VITASHELL/` as the recovery copy); each overwritten file leaves a hidden full copy no command can free (send `eboot.bin` only when it changed) | ours: `vita_setup.sh` keeps a copy of `config.txt` before each change |
| Our runner | `port/vita/tools/vita_agent_run.py` | `port/vita/tools/vita_console_run.sh` |

The steps (`VITA_IP` = the console's address; `port/vita/tools/vita_setup.sh` works over FTP: VitaShell's server at
first, vitacompanion's after; the developer is needed for a few seconds at the marked steps):

1. **The developer**: a fixed address for the console on the network; VitaShell open, SELECT pressed (its FTP server,
   port 1337); tell the agent the address and the firmware.
2. `VITA_IP=... port/vita/tools/vita_setup.sh check`: what is there (the FTP server, vitacompanion, the plugins in
   `config.txt`, the shader compiler, the disc files, BTTD00010 / BTTD00008).
3. **On 3.65, vita-agent-use**: build its plugin (its README: VitaSDK, CMake, the Mbed TLS 3.6.7 source), then
   `vita_setup.sh agentuse vita_agent_kernel.skprx vita_agent_loader.suprx vita_agent_shell.suprx` (the three into
   `ur0:tai/`, its two lines into `config.txt`, vitacompanion's lines taken out). **The developer** restarts the
   console, runs `python3 client/vita_agent.py bootstrap` (a terminal wizard: it asks the address and a folder for its
   config) and `session pair`, and taps OK on the console. Then `export VITA_AGENT_CONFIG_DIR=...` (bootstrap prints
   it), `VITA_AGENT=<vita-agent-use checkout>/vita-agent`, and `vita-agent serve` in a long-lived background process
   (`server status`: `connected`). Recommended there too: the NoSleep and NoLockScreen plugins.
   **Otherwise, vitacompanion**: download its release's `vitacompanion.suprx` and `vitacompanion_kernel.skprx`, then
   `vita_setup.sh companion vitacompanion.suprx vitacompanion_kernel.skprx`; **the developer** restarts the console
   once; from then on `vita_setup.sh check` shows it answering.
4. **The development build** (section 3) installed: with vita-agent-use, upload the `.vpk` and `app install` it (its
   AGENTS.md); with vitacompanion, `vita_setup.sh install-dev` (its `promote`; else the developer installs the `.vpk`
   with VitaShell once). Later builds need only the program: the runners send `eboot.bin` (`--eboot`).
5. **CapUnlocker** (the fourth core; https://github.com/GrapheneCt/CapUnlocker), for the run "without the fourth
   core" only: with vitacompanion, `vita_setup.sh capunlocker off|on` (its line in `config.txt`, then a reboot through
   vitacompanion); with vita-agent-use, through its guarded `config plan` / `config apply` and the developer's
   approval. Leave it on otherwise.

Tell the developer before every reboot and every change of `config.txt`. If after a reboot the console does not answer
within four minutes, stop and tell the developer: the old `config.txt` is in `port/build/vita_backup/`
(`config.first.txt` is the console's own, from before any change), and the console may need VitaShell (or starting
without plugins, see the HENkaku / Ensō guides) to put it back.

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
| `port/vita/tools/vita_agent_run.py --settings F --seconds N [--eboot] [--shots K]` | The same through vita-agent-use, as one of its runs ending at the game's own `exitafter` line; with screenshots (`shots/`), the console's CPU / FPS / memory samples and crash events (`events.jsonl`) |
| `port/vita/tools/vita3k_run.sh --settings F --seconds N` | The same in Vita3K |
| `port/vita/tools/ab_run.sh --switch S [--where vita3k\|console\|agent]` | A (S=0) and B (S=1) on the same fixed fight, then `runcmp.py`; verdict OK / LOOK / BAD |
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

**Profiling** (where the time goes, beyond `perfsum.py --parts`):

- **CPU, per thread**: the port already reads the Cortex-A9's counters through
  [libperf](https://github.com/shoui520/libperf) (`port/vita/vita_perf.c`: cycles, instructions, cache misses per
  thread in the report) when its kernel plugin `libperf.skprx` is in `config.txt`'s `*KERNEL` and its user module
  `libperf.suprx` is in the app's folder (`ux0:app/BTTD00008/libperf.suprx`; it is not in the package). Retail 3.65.
- **GPU**: [psp2-cex-gpues4-prof](https://github.com/shoui520/psp2-cex-gpues4-prof): experimental GPU profiling on a
  retail console, per pass, draw and shader, with a vitaGL adapter; a kernel plugin `psp2_gpuprof.skprx` and a
  library linked into the game, started before `vglInit`. Not in the port yet: the next step for questions like
  "is Kame House slow on the processor or on the graphics chip" (a development-build option, off by default).
- **The whole system, Razor**: [psp2-tooling](https://github.com/Grarak/psp2-tooling) (Grarak): Sony's DevKit tools
  on a retail console over USB: DTrace and whole-system CPU sampling with call stacks, Razor GPU metrics and frame
  captures, hardware breakpoints. It needs **decrypted DevKit 3.65 modules that the developer must provide** (the
  agent never looks for or downloads Sony's modules), was tested only on a PCH-2000 on 3.65 (not on a PCH-1000), and
  puts the console in development mode (its USB becomes the debug port). The heavy option: only if the lighter ones do
  not answer the question, and only with the developer's go-ahead.

When something breaks: the console's crash dump `ux0:data/psp2core-*.psp2dmp` with
https://github.com/xyzz/vita-parse-core and `port/build/vita_dev/bt3.unstripped.elf`; the port's watchdog
`ux0:data/bt3/hang-vgl.txt`; the log's start-up lines (shaders not built); `film=`, `drawlog=`, `bislist=` to find a
wrong draw (`docs/vita/development.md`).

## 7. Where the work stands

`docs/vita/status.md` (kept up to date: update it with every step) has the switches and their defaults, what has been
verified, and the Results table. In short (2026-11-13):

- Everything is in one pull request to `arielxflores/Tenkaichi3Decomp-Vita` (base `vita`): the detailed stages'
  fixes, the fourth core's helper, vitaGL's state calls, `fxlow` (the screen-filling additive effects drawn at half
  resolution and added back), the first steps of a native renderer (`native`, `nativetex`, `updest`, `dpfast`,
  `fxrun`, `texbind`, `seavtx`: the game names its static geometry and its textures to the renderer, which keeps
  them without guessing), and the test tools. All on by default except `esdirect` (no gain measured).
- Run on a console (PCH-1004, 3.65, CapUnlocker on, the development build): each switch A against B, then everything
  on by default for 10 minutes (no crash, no hang, 29.3-29.6 fps on every stage).
- **Not run yet**, the first things to do with a console: a run without the fourth core (CapUnlocker off), the
  release build, and stage 1 in Vita3K. Then the next steps in `docs/vita/native.md`.

## 8. Documents

| Document | About |
|---|---|
| `README.md`, `CHANGELOG.md` | For players: installing, controls, mods; what changed |
| `port/vita/README.md` | The Vita port's differences from the PC port; links |
| `docs/vita/building.md` | Building, vitaGL's patch |
| `docs/vita/development.md` | The development build, every `bt3.txt` setting, the log, a map of the files |
| `docs/vita/status.md` | The work in progress, its state, the Results table, the rules |
| `docs/vita/native.md` | The native renderer: the engine seam, what is named, the next steps |
| `docs/vita/testing.md` | The levels of testing, Vita3K and the console set up and run, profiling |
| `docs/vita/compare.md` | The old way against the new: the three stages, what to expect |
| `docs/port/README.md` | The PC port's notes (rules for `src/`) |
| `docs/systems/*.md`, `docs/README.md` | What the game's code does, system by system |
