# The development build

```sh
BT3_VITA_DEV=1 VITA_SAMPLES=~/vitasdk-samples python3 port/vita/build.py
```

builds `port/build/vita_dev/Tenkaichi3Decomp-Vita-dev.vpk`: the same program as the release, with its own title
(BTTD00008, "BT3 (development)") so that it can be installed next to it, and every setting of `bt3.txt` read. It
shares `ux0:data/bt3/` (the disc files, saves and settings) with the release and writes its log to
`ux0:data/bt3/log-vgl.txt`.

Started from its bubble without settings, it shows a start screen: the float self-test of the vector library's
arithmetic (the exact model against the sums computed on the PC, and the hardware model against the exact one),
then CROSS for the game, CIRCLE for the demo fight (computer against computer).

## `ux0:data/bt3/bt3.txt`

One `name=value` per line. The release reads only `disc`, `wide`, `heap` and `report`; the development build reads
all of them. The full list, each with a comment, is `read_settings` in `port/vita/vita_main.c`; the ones most used:

| Setting | Meaning |
|---|---|
| `mode=play` / `mode=demo` | The game from its start, or the demo fight, without the start screen |
| `demoduel=1`, `demostage=<n>`, `demochara1=<id>`, `demochara2=<id>` | The test fight: a duel with intros on that stage, with those fighters |
| `sweep=<s>` | In the demo fight, start again after that many seconds with the next pairing (0: off) |
| `startmode=<n>` | The menus start on that screen (62: the options) |
| `report=1` | The figures every two seconds in the log (also in the release) |
| `film=<n>` | A small picture of every n-th frame of a fight in the log (`filmstate`, `filmlist`, `filmtrig` choose which) |
| `logsplit=<KB>` | The log in parts of that size (`log-part2-vgl.txt`, ...) |
| `wide=0` | The PS2's 4:3 picture (also in the release) |
| `msaa=0/2/4` | The GPU's multisampling |
| `touchlr=rear/front/off`, `touchlog=1` | Where L2, R2, L3 and R3 are touched, all four (over the player's Vita Settings → Touch); the touches in the log |
| `soak=1` | With `mode=demo`: duels and three-against-three team battles one after another without end (`port/src/headless.c`), to find what goes wrong after a long time |
| `perf=0` | No processor counters (libperf), to measure the speed as the release has it |
| `safe=<bits>`, `safe2=<bits>` | Parts of the renderer switched off or done the plain way, to find the cause of a fault (`gVglSafe` and `gVglSafe2` in `port/vita/vita_vgl.c`) |
| `dump=1` | A copy of each file the game loads whole, in `ux0:data/bt3/dump/` |
| `drawlog=<a>-<b>`, `drawlogverts=<n>` | Each draw of frames (lists) a..b in the log: its path, state and size; with `drawlogverts`, the vertices of its GS draws of n vertices or more |
| `bislist=<n>`, `bisfrom=<i>`, `bisto=<j>`, `biskind=<mask>`, `bisinv=1` | In frame n, the draws i..j-1 of the kinds in mask (1 GS primitives, 2 the stage's batches, 4 the fighters' batches, 8 / 16 the stage's / fighters' kept parts) left out, or (`bisinv`) only those drawn: with `film`, which draw makes a wrong picture |
| `zlayer=<n>` | Depth steps a stage layer laid on the stage leads by (default 2; 0: none) |

Most of the remaining settings switch one optimisation of the renderer off (`fastlane=0`, `stx=0`, `wtc=0`, ...)
so that its effect can be measured or a picture compared with and without it.

## The log

Each run appends to `log-vgl.txt` (the release: `log.txt`). With `report=1` it has, every two seconds: the frame
rate, where the time of each thread went (the game, the list reader, the drawer, the render thread), the uploads,
the texture and state caches, and the processor counters per thread when libperf is installed. When no picture has
been made for three seconds, a watchdog writes where every thread is to `hang-vgl.txt`: each thread's state, the
drawing and render threads' place in their work, the lists' two queues, the pictures shown, vitaGL's free memory and
the texture count.

## Where things are

| Path | Contents |
|---|---|
| `port/vita/build.py` | The build |
| `port/vita/vita_main.c` | Start-up, settings, the log, the watchdog, exit |
| `port/vita/vita_vgl.c` | The renderer (vitaGL): shaders, textures, render targets, the native effects, the render thread, pacing |
| `port/vita/vita_gfx.c` | Which frames are drawn, the controllers (a second one on a PS TV), clashes with Square |
| `port/vita/vita_touch.c` | The touch panels: L2 / R2 / L3 / R3 and the menus |
| `port/vita/vita_audio.c`, `sdlshim/` | Sound output, under the PC port's sound code (`port/src/gs/snd_adx.c`, `snd_se.c`) |
| `port/vita/vita_file.c`, `vita_mem.c` | The disc's archives, the mods folder, memory and the game's data tables |
| `port/vita/vita_float.c`, `vita_vu0_inline.h` | The PS2 vector unit's arithmetic |
| `port/vita/srcfix.py` | Rewrites of the game's text at build time: the PS2's rule for shift counts, and its float division by zero (`__port_fdiv` in `port/include/port_compat.h`: no infinity or NaN where the PS2 gives its largest number) |
| `port/vita/vita_text.c`, `vita_optart.c` | What the game's text and the menus' art say of the PS2, said of the Vita (the options' controller page and its Vita Settings, the Versus Menu's "Multiplayer", the L3 / R3 icons) |
| `port/vita/vitagl/` | The vitaGL patch and its build script |
| `port/vita/tools/` | The scripts that made the new art of the menus and the button icons (`vita_optart.h`) |
| `port/vita/sce_sys/` | The LiveArea |
| `port/src/gs/gs_core.c` | The PC port's GS front end, with the Vita's changes (the list read and drawn on threads of their own, the upload store) |
