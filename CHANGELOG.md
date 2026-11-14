# Changes (PS Vita port)

## v1.6-optimized (2026-11-14)

New

- **The disc image as it is** (arielxflores's reader, from his v1.2): copy your `.iso` to `ux0:data/bt3/`, under
  any name, and the game reads its files from it; no need to unpack it with 7-Zip any more (the unpacked files still
  work, and are used first when both are there).

## v1.5-optimized (2026-11-14)

Faster

- **arielxflores's 1.2 speed-ups**, taken into this fork (his own code, its commits cherry-picked with `-x`), all
  but the dynamic fight resolution:
  - shaders shipped ready-made (the offline shader bank of Grarak's MH3 port): every shader, the vertex and effect
    ones too, goes through the cache, and the VPK carries them already compiled (`port/vita/shbank.bin`, 87
    programs, the same for BT3 and BT4, made on a Vita with `shbank=1` and checked with `shbank=2`: none missing
    in the menus or the fights), so `libshacccg.suprx` is no longer needed;
  - the outline, the glow and the colour-table passes in half precision (`fxhalf=0` goes back);
  - songs read while they play (no stop when the music changes), and the game's no-wait reads on a reading thread;
  - the game's file unpacker 2 to 5 times faster, byte for byte the same output;
  - new textures read from GS memory while the render thread draws (`texearly=0` goes back); the drawing thread's
    texture keys and state comparisons in line; the C library's thread lookups the short way and a faster memcmp;
  - the opening film no longer reads 2 MB it does not use, and no freeze when it ends or is skipped;
  - palette textures' indices no longer leak in graphics memory (slowdowns after many fights); a line a minute in
    the log with the picture rate, the memory and the texture tables;
  - profile-guided builds (`BT3_PGO=gen` / `use`, [docs/vita/building.md](docs/vita/building.md));
  - `fightres=720` / `960` in `bt3.txt` draws the fights at that size always (640 x 368 by default).
- **Detailed stages stay fast**: in a survey of 25 stages (a duel on each, stages 0 to 24), the slowest went from
  16.7-18.7 pictures a second to 26-29, the average from 25.2 to 28.8, and stages at 28 or more from 7 to 21 of 25.
  Three causes, all in the copies of the game's model calls the port keeps: their cache filled up over a session and
  was never emptied (a stage played after others ran without it); the kept parts' places were too few and shared at
  random; and only the first 2 MB of a stage's vertices could be kept, so a fight began fast and slowed down as more
  of the stage came into view.
- **The first fight on a stage is as fast as the next ones**: some of the stage's kept parts were refused for the
  whole first fight on a stage and drawn the long way (up to a quarter of the drawing thread's time; stage 20 at 27.9
  pictures a second against 29.9 in the next fight there). Development build: `dpsplit=0` goes back.
- Kept parts are no longer drawn again the long way every other frame (the game draws into its two display buffers
  in turn).
- With the CapUnlocker plugin, the fourth CPU core works out the image uploads' numbers the list reader worked out
  before, and vitaGL leaves out GPU state calls that change nothing.
- The screen-filling effects (auras, beams, flashes) are drawn at half the resolution each way and added to the
  picture, as TwinsanityEvolved does with its particles: the graphics chip shades a quarter of their pixels. Only
  effects covering at least two screens' worth are drawn so; the edge of a fighter inside a beam can show a step of
  two pixels. Development build: `fxlow=0` goes back.

- **The first steps of a native renderer** (the "engine seam", as BT3-Recomp's and Celosia's; `docs/vita/native.md`):
  the game names what it draws to the renderer instead of the renderer finding it again in the PS2 display list
  every frame. The stage's meshes and animated objects and the fighters' model parts are kept by their names; the
  stage's and the fighters' textures are known by their names and each upload's destination remembers what it left
  there; the kept parts are drawn with what does not change worked out once; the effects' particles are read as
  runs. On three heavy stages the list reader works a third of what it did and the drawing thread about two thirds:
  the pictures a second were at the 30 cap already, what this gives is room in the busy moments and for a console
  without the fourth core. The textures are then found by the names of what their uploads left (a quarter less time
  in the texture lookups), and the sea's corners are worked out once a frame each instead of once for each triangle
  that has them. Development build: `native=0`, `nativetex=0`, `updest=0`, `dpfast=0`, `fxrun=0`, `texbind=0`,
  `seavtx=0` go back.

- The log no longer gets a sample of the picture's draws every five seconds of a fight (that picture was drawn the
  slow way), and `log.txt` is kept under 1 MB: a longer one becomes `log-prev.txt` at the next start.

New

- Options → Controller → Vita Settings says whether the game has the fourth CPU core (CapUnlocker on or off).

## 1.1

New

- **Two players on a PS TV**: a second DualShock plays as player 2 (Versus Menu → Multiplayer). On by default;
  Options → Controller → Vita Settings → 2nd Pad turns it off. A DualShock's own L2, R2, L3 and R3 buttons work.
- **Clashes with Square**: beam struggles and rush clashes are won by pressing Square again and again instead of
  turning the left stick, and the prompt shows Square. Vita Settings → Clashes → Stick goes back to the stick.
- **Vita Settings** (Options → Controller): where L2, R2, L3 and R3 are on the touch panels (rear pad, the
  screen's corners, or off), Clashes, 2nd Pad and Mods.
- **Mods**: files in `ux0:data/bt3/mods/` replace the disc's (a single file of an archive, a whole archive, or a
  mod's own folder holding a modded disc's `DATA` folder). Vita Settings → Mods shows what is in use and turns them
  off. A mod that changes the game's program, like Budokai Tenkaichi 4, is left out with a message instead of
  crashing the game. See the README.

Fixed

- The game no longer freezes after about 18 minutes of play (the picture stopped while the music went on).
- The space stage near Frieza's ship: parts of the red planet's clouds no longer vanish for a frame while the fighters
  move (it looked like flickering patches).
- The "ObjSeam" crash with Videl, the Great Saiyaman, Hercule and other characters (Majin Buu saga, Galactic
  Crisis, Makyo Star, some special attacks).
- Freezes from invalid maths results the PS2 never produces.
- Story mode: a won battle that opens a new saga always lists it.
- Dragon World Tour: no crash while Yamcha's invitation is pending.
- Long fights: no giant triangles or white lines when the graphics memory filled up; no wrong colours from mixed-up
  palettes.
- Coming back from the LiveArea or sleep: the game's speed is restored, and its files are reopened if the system
  closed them (glitches, slowdowns and crashes after resuming).
- A finger resting on the rear touch pad no longer holds L3 or R3.
- Setup: the game's files are found in more folder layouts, and the error lists what was found.
- When the picture stops, the game writes `ux0:data/bt3/hang.txt` for bug reports.

Known issues

- Not reproduced yet: on Namek, the ground under the water flickering while the fighters move; a flicker on the
  mission select screen; the title screen's text looking sliced. A short video of any of them (and where in the
  fight it happens) helps a lot.
- Ad-hoc and online play between two Vitas are not implemented yet.
