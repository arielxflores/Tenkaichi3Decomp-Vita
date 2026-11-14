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
| `report=1` | The figures every two seconds in the log (also in the release), and a sample of the GS primitives by state every 150 pictures of a fight (that picture drawn the slow way, off the fast lane) |
| `film=<n>` | A small picture of every n-th frame of a fight in the log (`filmstate`, `filmlist`, `filmtrig` choose which) |
| `logsplit=<KB>` | The log in parts of that size (`log-part2-vgl.txt`, ...) |
| `wide=0` | The PS2's 4:3 picture (also in the release) |
| `msaa=0/2/4` | The GPU's multisampling |
| `fightres=640/720/960` | The fights always at that size (default 640 x 368; also in the release). Taken from arielxflores's 1.2 without its dynamic choice of size |
| `fxhalf=0` | The full-screen passes (the outline, the table passes, the glow) in full precision, not half (from arielxflores's 1.2) |
| `shbank=1` | Every shader the run compiles or reads from the card added to `ux0:data/bt3/shbank.bin`; `port/vita/tools/shbank.py` makes them into `port/vita/shbank.bin`, the bank the VPK carries (`app0:shbank.bin`: the shaders read from it, not compiled; from arielxflores's 1.2) |
| `shbank=2` | The bank checked: each shader it does not have named in the log (`vgl: a shader not in the bank`) and compiled |
| `texearly=0` | A new texture waits for the render thread before its pixels are read from GS memory, as before (from arielxflores's 1.2) |
| `asyncread=0`, `vufquick=0` | The game's no-wait reads on the caller's thread, not the reading thread; the vector stand-ins' full check (from arielxflores's 1.2) |
| `pgoname=<name>`, `pgodump=<s>` | A `BT3_PGO=gen` build: its counts' file name and how often they are written ([building.md](building.md)) |
| `touchlr=rear/front/off`, `touchlog=1` | Where L2, R2, L3 and R3 are touched, all four (over the player's Vita Settings → Touch); the touches in the log |
| `soak=1` | With `mode=demo`: duels and three-against-three team battles one after another without end (`port/src/headless.c`), to find what goes wrong after a long time. The sweep is off in a soak unless `sweep=` is given |
| `soakstages=<ids>`, `soakduel=1` | The soak's stages in turn (ids and ranges: `0-34`, `1,6,0x10`) instead of the demo's, and only duels: a survey of the stages' speed, summed up by `port/vita/tools/perfsum.py log-vgl.txt` |
| `exitafter=<s>` | The program ends itself after that many seconds from its start, its log written whole, with the line `vita: exitafter: the test run ends after <s> s` (the runners' end of a test run) |
| `soakab=1` | Each soak battle twice in a row, the same stage and fighters: once with the settings given the value 2 off (side A), once on (side B); A first in one round of the stages, B first in the next. `perfsum.py --ab log-vgl.txt` sets each stage's B against its A |
| `fehelper=0/1/2` | The uploads' numbers (the hashes the texture cache looks images up by) worked out on the fourth core instead of by the list reader: 0 off, 1 on (default; only with the CapUnlocker plugin), 2 on in the soak's B battles |
| `dpsplit=0/1/2` | A kept part's stage strips grouped by the bank of the stage's buffer they start in, so that a group's indices stay within 64K vertices of its bank. Without it a part whose same-state strips lie in banks far apart was refused for the whole fight and walked the long way (the first fight on a stage; the log's "kept parts not kept for another reason: N indices beyond a bank"). 0 off, 1 on (default), 2 on in the soak's B battles |
| `dpframe=0/1/2` | Kept parts matched with FRAME's buffer number as "the picture" (the game draws into its two display buffers in turn): 0 off (every kept part found in another state every other frame), 1 on (default), 2 on in the soak's B battles |
| `gxmcache=0/1/2` | In vitaGL (`vitagl.patch`): the GXM state calls of a draw left out when they set what is set already (program, texture, vertex stream). 0 off, 1 on (default), 2 on in the soak's B battles |
| `dpfast=0/1/2` | Kept parts drawn with what does not change worked out when they were kept: the form that matched last tried first, whether a stage group breaks the batches' order, the triangles and strips counted, the uniform blocks' viewport floats; the blocks not cleared before being written. 0 off, 1 on (default), 2 on in the soak's B battles |
| `fxrun=0/1/2` | The effects as runs: the quad particles (EftGfx_DrawSprite, EftPrim_DrawBillboard: a triangle strip of four corners) decoded as two triangles each the triangle runs' way instead of register by register; the triangle runs' additive triangles into the additive groups (when the alpha they write is not read). 0 off, 1 on (default), 2 on in the soak's B battles |
| `updest=0/1/2`, `upprof=1`, `updestlog=1` | Native textures, step 2: each upload's destination (place, width, format, rectangle) remembers the upload's number, the rectangle's number it left (worked out once for each upload and destination) and the pages' or, for a small one, the blocks' write counters; an upload found the same there is not taken apart again, and a texture key takes its rectangle's number from the destination it lies in (also an 8- or 4-bit texture sent as 32-bit pixels), checked against the blocks every 16th time. 0 off, 1 on (default), 2 on in the soak's B battles. `upprof=1`: every upload and texture lookup timed, by the way it went (a measuring run: the clock reads cost too). `updestlog=1`: the destinations made and the lookups that found none |
| `texbind=0/1/2` | Native textures, step 3: a destination is unbound at the write of anything but its own upload into its blocks, and a texture (TEX0 and its palette's place) remembers the destinations it lies in; the renderer finds its texture by the names of what they hold, without the pages' numbers, checked the long way every 64th time (0 found wrong in ~15,000 checks on the console). 0 off, 1 on (default), 2 on in the soak's B battles; with `updestlog=1` a binding found wrong is logged with whether its contents differed |
| `esdirect=0/1/2` | The seam's call packets of the models' parts done as what they are (constants into VU memory, the program's call and registers, the MARK) instead of read code by code (an experiment: no gain measured on the console). 0 off (default), 1 on, 2 on in the soak's B battles |
| `seavtx=0/1/2` | The surfaces queued in the ordering table (the sea of Kame House, the other stages' water): each corner tested against the clip planes and projected once a frame, not once for each triangle that has it; a triangle with a corner outside is clipped as before. 0 off, 1 on (default), 2 on in the soak's B battles |
| `nativetex=0/1/2`, `nativetexcheck=<n>` | The stage's and the fighters' textures named by the game ([native.md](native.md)): each upload of a texture of the stage's texture file or of a fighter's carries a MARK with the generation of those files, and its number (what the renderer knows a texture by) is the one worked out from all its bytes the first time it came with that generation, instead of being worked out again from samples of its bytes every time (on the fourth core with CapUnlocker); its bytes read again every `nativetexcheck`-th upload (default 128; 0 never), and read every time once found changed. 0 off, 1 on (default), 2 on in the soak's B battles. The log's "textures named by the game" line counts them |
| `native=0/1/2`, `nativecheck=<n>` | The engine seam ([native.md](native.md)): the stage's meshes and animated objects and the models' parts (the fighters) named by the game and kept in a table of their own instead of the cache of calls; read again the long way every `nativecheck`-th call (default 512; 0 never). 0 off, 1 on (default), 2 on in the soak's B battles. The log's "engine seam" line counts them |
| `fxlow=0/1/2`, `fxlowmin=<screens>`, `fxlowdiv=<n>` | The screen-filling additive effects (auras, beams, flashes) drawn into a picture of their own at 1 / `fxlowdiv` of the view's size each way (default 2: a quarter of the pixels), depth-tested in the pixel shader against the display's stored depth, and added to the picture (as TwinsanityEvolved's `TE_GFX_PARTICLE_SCALE`). Only an additive group covering at least `fxlowmin` screens (default 2) is drawn so; the rest as before. 0 off (as before), 1 on (default; off by itself when the console's shader compiler does not take its pixel shaders), 2 on in the soak's B battles. The log's "effects at a lower resolution" line counts them |
| `ccsize=<n>`, `ccevict=0`, `dppick=0` | The model calls' cache: its places (a power of two, 2048 to 16384; default 4096), places of calls not seen lately given to new ones (`ccevict=0`: kept until the fight ends, as before), a kept part's place chosen free (`dppick=0`: whatever its number gives, as before) |
| `dpslots=<n>`, `dpbank=0` | The kept parts' places (a power of two, 1024 to 8192; default 2048), and the stage's kept parts from anywhere in its buffer (`dpbank=0`: only from its first 2 MB, as before) |
| `perf=0` | No processor counters (libperf), to measure the speed as the release has it |
| `safe=<bits>`, `safe2=<bits>` | Parts of the renderer switched off or done the plain way, to find the cause of a fault (`gVglSafe` and `gVglSafe2` in `port/vita/vita_vgl.c`) |
| `dump=1` | A copy of each file the game loads whole, in `ux0:data/bt3/dump/` |
| `drawlog=<a>-<b>`, `drawlogverts=<n>` | Each draw of frames (lists) a..b in the log: its path, state and size; with `drawlogverts`, the vertices of its GS draws of n vertices or more |
| `bislist=<n>`, `bisfrom=<i>`, `bisto=<j>`, `biskind=<mask>`, `bisinv=1` | In frame n, the draws i..j-1 of the kinds in mask (1 GS primitives, 2 the stage's batches, 4 the fighters' batches, 8 / 16 the stage's / fighters' kept parts) left out, or (`bisinv`) only those drawn: with `film`, which draw makes a wrong picture |
| `zlayer=<n>` | Depth steps a stage layer laid on the stage leads by (default 2; 0: none) |

Most of the remaining settings switch one optimisation of the renderer off (`fastlane=0`, `stx=0`, `wtc=0`, ...)
so that its effect can be measured or a picture compared with and without it.

## The log

Each run appends to `log-vgl.txt` (the release: `log.txt`, which becomes `log-prev.txt` at a start once it is over
1 MB). With `report=1` it has, every two seconds: the frame
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
| `port/vita/vita_iso.c` | The disc's files read from its `.iso` as it is (arielxflores's, from his v1.2): the file calls linked with `--wrap`, a path inside the image read from its place in it |
| `port/vita/vita_float.c`, `vita_vu0_inline.h` | The PS2 vector unit's arithmetic |
| `port/vita/srcfix.py` | Rewrites of the game's text at build time: the PS2's rule for shift counts, and its float division by zero (`__port_fdiv` in `port/include/port_compat.h`: no infinity or NaN where the PS2 gives its largest number) |
| `port/vita/vita_text.c`, `vita_optart.c` | What the game's text and the menus' art say of the PS2, said of the Vita (the options' controller page and its Vita Settings, the Versus Menu's "Multiplayer", the L3 / R3 icons) |
| `port/vita/vitagl/` | The vitaGL patch and its build script |
| `port/vita/tools/` | The scripts that made the new art of the menus and the button icons (`vita_optart.h`); `perfsum.py` (the soak's figures by stage); `check_syntax.sh` (the Vita's C checked without VitaSDK); `vita3k_run.sh`, `vita_console_run.sh` (a test run in Vita3K, or on a console over the network: [testing.md](testing.md)); `vita_setup.sh` (the console set up over the network); `ab_run.sh`, `runcmp.py`, `filmpics.py`, `settings/` (a setting's old and new way side by side: [compare.md](compare.md)) |
| `port/vita/sce_sys/` | The LiveArea |
| `port/src/gs/gs_core.c` | The PC port's GS front end, with the Vita's changes (the list read and drawn on threads of their own, the upload store) |
