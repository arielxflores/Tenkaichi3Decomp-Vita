# Testing the Vita build: what an agent (or a person) can run, and where

Four levels, from what runs anywhere to what needs the console in the hand. To set a setting's old way against its
new one with them: [compare.md](compare.md). Each change of the renderer goes through
as many of them as the place it is made in allows; what was run is said in the commit message and in
[status.md](status.md).

| Level | What it shows | Needs | A cloud agent | An agent on the developer's computer |
|---|---|---|---|---|
| 1. Syntax check | The code compiles for the Vita (names, types) | clang, git | yes | yes |
| 2. Vita3K | The program starts, runs a test fight, draws; the log's counters; no crash | VitaSDK, Vita3K, the game files | no | yes |
| 3. The console by network | All of it, at the console's speed, with the fourth core | A Vita on the same network with vitacompanion | no | yes |
| 4. The console by hand | What a person sees and feels: pictures, controls | A Vita | no | (asks the person) |

A cloud session has neither the toolchain (VitaSDK's releases are not reachable) nor the game files (never in the
repository) nor the developer's network: it stops at level 1 and asks for the rest.

## 1. The syntax check

```sh
port/vita/tools/check_syntax.sh                      # the renderer's files and the game files the port changes
port/vita/tools/check_syntax.sh FILE.c ...
WARN=1 port/vita/tools/check_syntax.sh FILE.c        # with -Wall -Wextra
```

Clones the SDK's headers, vitaGL at the port's revision with `vitagl.patch`, vitaShaRK and the samples into
`port/build/syntax_sdk` once, then compiles each file with the host's clang for armv7-a up to syntax and types. It
does not link and runs nothing: a Cg shader (compiled on the console at run time), a GXM fault or a wrong picture is
not seen.

## 2. Vita3K

[Vita3K](https://vita3k.org) runs the development package on a computer. It is how other native ports with a Vita
build test their pictures without the console: TwinsanityEvolved runs its package in Vita3K with a script
(`tools/vita/vita3k_run.sh`), replays recorded input deterministically and compares a hash of every draw between two
builds; Celosia measures and profiles in Vita3K and keeps settings for where Vita3K differs from the console.

What Vita3K cannot tell (from TwinsanityEvolved's and Celosia's notes): **the console's speed** (its timing is not the
console's), the float unit's last bits (its rounding modes are applied differently), **the fourth core**
(CapUnlocker), and some GPU memory behaviour (Vita3K caches colour surfaces by address: memory freed and reused as a
texture can show the old picture). Use it for "does it run, does it draw the same, do the counters look right"; settle
speed and last-bit differences on the console.

### What to expect of this port in Vita3K (not tried yet)

Nobody has run this port in Vita3K yet. From what the port does and what the other ports report:

- **Likely to work**: the program itself (ARM code, threads, files, the game heap), vitaGL (TwinsanityEvolved and
  Celosia run on it in Vita3K), the runtime shader compiler (TwinsanityEvolved compiles its shaders in Vita3K with the
  same `libshacccg.suprx` and gets the same bytes as the console: it ships the result as the console's cache), the
  display buffers read as textures (Vita3K finds a colour surface again by its address).
- **Likely to differ from the console**: what reads the **depth buffer as a texture** (`vglTexImageDepthBuffer`: the
  fog byte and the table passes, the depth tint, and `fxlow`'s depth test). Vita3K keeps surfaces on the host's GPU
  and its read-back of a picture into memory is empty (TwinsanityEvolved): a wrong picture there in Vita3K is not
  proof of a bug on the console. The kept parts written straight into the chip's buffers should be seen (Vita3K reads
  vertex memory at each draw), but that is to be checked.
- **Not there**: the fourth core (the program sees no CapUnlocker and runs its fallbacks), the processor counters
  (`perf=0` if libperf's calls fail), the console's speed.

### What the log is good for in Vita3K

The log (`log-vgl.txt`) is written by the program itself, so what it counts is as good in Vita3K as on the console
whenever it counts the program's own decisions:

- **Good**: the renderer's counters every two seconds (`report=1`): the engine seam's line (calls sent from their
  copies, read the long way, table full), the effects at a lower resolution (groups drawn smaller, draws without their
  shader), the cache of calls, the kept parts, the textures made, the shaders built or refused at start-up; `drawlog=`
  (each draw of the chosen frames: its path, state and size); errors, the watchdog's `hang-vgl.txt`, crashes. To tell
  whether a change draws the same things: the same test with the setting at 0 and at 1, and these lines (and
  `drawlog=` around one moment) side by side.
- **Not good**: every time and speed figure (pictures a second, the threads' milliseconds, `perfsum.py`), the fourth
  core's lines, the processor counters.
- **Pictures**: `film=` copies the display's memory from the processor, which Vita3K does not fill in (its read-back
  is empty, as TwinsanityEvolved found): use Vita3K's own screenshots, or its `-C` (it saves the colour surfaces).

### Once

1. Build the emulator or take a build from [vita3k.org](https://vita3k.org); install the two firmware packages
   (`--firmware PATH.PUP` on the command line, or the menu).
2. Find its storage folder (Vita3K's settings: "Emulated System Storage Folder"; by default
   `~/.local/share/Vita3K/Vita3K` on Linux, `~/Library/Application Support/Vita3K/Vita3K` on macOS). Below, `FS` is
   its `fs` folder.
3. The game files into `FS/ux0/data/bt3/disc/` as on the console ([the README](../../README.md#game-files)).
4. The runtime shader compiler `libshacccg.suprx` into `FS/ur0/data/` (extracted from a console as for the release).
   Without it, only the shaders already in `ux0:data/bt3/shcache` work: copying a console's `shcache` folder into
   `FS/ux0/data/bt3/` is another way.
5. VitaSDK and the development package: `BT3_VITA_DEV=1 python3 port/vita/build.py` ([building.md](building.md)).

### A run

```sh
VITA3K=/path/to/Vita3K VITA3K_PREF=/path/to/storage port/vita/tools/vita3k_run.sh --settings test.txt --seconds 300
```

installs `port/build/vita_dev/Tenkaichi3Decomp-Vita-dev.vpk`, puts `test.txt` in `ux0:data/bt3/bt3.txt` (the one
there is saved and put back), runs Vita3K's plain window in console mode (`--frontend sdl -z`), stops it after the
given seconds (or at once if Vita3K's own log grows past 50 MB, an emulated fault in a loop), and leaves in
`port/build/runs/vita3k-<date>/` this run's part of the log (`log.txt`), `hang.txt` if the watchdog wrote one, the tail
of Vita3K's log, and `perfsum.txt` if there were soak battles.

Vita3K's options used or useful (its `--help`, `vita3k/config/src/config.cpp`): a `.vpk` as the first argument
installs and runs it; `-r TITLEID` runs an installed one; `--frontend sdl` a window that runs the app and quits with
it; `-z` console mode; `-l 0..6` the log level; `-B Vulkan|OpenGL`; `--firmware`; `-d TITLEID` deletes an installed
app.

## 3. The console by network: vitacompanion

[vitacompanion](https://github.com/devnoname120/vitacompanion) is a plugin for homebrew development: an FTP server
on port 1337 and a command server on 1338 (`launch TITLEID`, `quit TITLEID|all`, `screen on|off`, `reboot`, `wait`,
and synthetic buttons, sticks and touches: `press cross; wait 100ms; release cross`). With it a script on a computer
of the same network sends a new program, starts it, stops it and fetches the log, with nobody holding the console.

### Once

1. Copy `vitacompanion.suprx` and `vitacompanion_kernel.skprx` to `ur0:tai/` and add them to `ur0:tai/config.txt`:

   ```
   *KERNEL
   ur0:tai/vitacompanion_kernel.skprx

   *main
   ur0:tai/vitacompanion.suprx
   ```

   then reboot. `echo version | nc VITA_IP 1338` answers when it works.
2. Install the development package once with VitaShell (title BTTD00008, next to the release).
3. Give the console a fixed address on the network, and keep it charging (a long soak).

### A run

```sh
VITA_IP=192.168.1.50 port/vita/tools/vita_console_run.sh --settings test.txt --seconds 900 --eboot
```

closes the game if it runs, sends `port/build/vita_dev/eboot.bin` over the installed one (`--eboot`: only the program,
no new package needed), puts `test.txt` in `ux0:data/bt3/bt3.txt` (the console's is saved and put back), starts the
game, waits, closes it, and fetches into `port/build/runs/<date>/`: this run's part of `log-vgl.txt` (`log.txt`),
`hang.txt` if any, the list of crash dumps in `ux0:data`, and `perfsum.txt` / `perfsum_ab.txt` (with `soakab=1`).

### When it crashes

The console writes a core dump to `ux0:data/psp2core-*.psp2dmp` and shows error C2-12828-1. The build keeps the
program with its symbols (`port/build/vita_dev/bt3.unstripped.elf`):
[vita-parse-core](https://github.com/xyzz/vita-parse-core) turns the dump into the place of the fault and the call
stack (`python main.py core.psp2dmp bt3.unstripped.elf`; an old Python 2 tool, check its README). The port's own
watchdog writes `ux0:data/bt3/hang-vgl.txt` when no picture has been made for three seconds.

## Test settings (`bt3.txt`)

The development build reads every setting of [development.md](development.md). The ones a test run uses:

```
mode=demo          # the demo fight, computer against computer, without the start screen
report=1           # the figures every two seconds in the log
soak=1             # battle after battle (headless.c)
soakduel=1         # duels only: the same work on every stage
soakstages=0-24    # the stages in turn
soakab=1           # each battle twice: the settings of value 2 off (side A), then on (side B)
fxlow=2            # (for example) the setting under test, on in the B battles only
```

A run of all 25 stages with `soakab=1` takes about 50 battles: give it the time (`--seconds`). A short check that the
program starts, fights and draws: `mode=demo` and `report=1`, 120 seconds.

## What this port does not have yet

- **An end of the run from inside**: a setting such as `exitafter=<seconds>` (the program closes itself, its log
  written), so that a run ends cleanly instead of being stopped from outside.
- **A hash of the draws** per picture in the log (TwinsanityEvolved's `TE_GFX_DRAWHASH`): with it two runs of the
  same fight compare picture by picture, in Vita3K, without looking at them. Whether two runs of the soak fight the
  same fight frame for frame (the computer's moves, the game's random numbers) is not known yet: to be checked first. `film=` (small pictures in the log) is what there is now.
- **Recorded input**: the soak's fights need none; a test of the menus or of a player's moves would need the pads
  recorded (or vitacompanion's synthetic buttons on the console).

## Sources

- Vita3K's command line: `vita3k/config/src/config.cpp` in [Vita3K](https://github.com/Vita3K/Vita3K) (2026-11-11)
- TwinsanityEvolved's Vita3K runner and notes: [tools/vita/vita3k_run.sh, README](https://github.com/menaman123/TwinsanityEvolved)
- Celosia's Vita3K settings and measurements: [README](https://github.com/HenryKun55/celosia)
- [vitacompanion](https://github.com/devnoname120/vitacompanion) (FTP, commands, synthetic input)
- [vita-parse-core](https://github.com/xyzz/vita-parse-core) (core dumps)
