# Changes (PS Vita port)

## 1.2 (in progress)

New

- **Languages**: Spanish, German, French and Italian menus, HUD and battle subtitles, from your own European disc
  (SLES-54945): copy its `DATA/PZS3EU1.AFS` to `ux0:data/bt3/eu/`. The language is chosen at the first start that
  finds it. Based on Matteo Santoro's work.
- **Start menu**: hold SELECT while the game starts for the language and an **Unlock everything** cheat (the
  game's own unlock: all characters, stages, music and items, and 4,850,000 Zenni, given to the save it loads).
- **Disc Fusion**: the modes of the two earlier games' discs are open (the Vita has no disc to swap).
- A crash writes `ux0:data/bt3/crash.txt` and closes the game, instead of leaving it frozen (with the kubridge
  plugin, which most setups have).
- The log has a line a minute with the picture rate and the memory in use, for bug reports.
- `bt3.txt` `fightres=720` draws the fights at 720 x 408 instead of 640 x 368 (sharper; about the same speed).

Fixed

- A second controller paired with the VitaControl plugin moved player 1 as well as player 2.
- L3 / R3 on the touch panels (a corner of the screen, or the rear pad): a touch is a quick press, so
  transformations happen (the game ignores an R3 held longer than about a fifth of a second).
- The large lettering (the title screen's, the menus' headings) no longer looks cut through.
- No freeze of about a second when the opening film ends or is skipped.

Faster

- Music changes no longer stop the game for a moment: songs are read while they play. The screens that load as
  they draw (the menus' portraits, the loading screens) keep drawing while their files are read.
- The game's file unpacker (menus, loading screens, fighters' motions) is 2 to 5 times faster.
- The drawing thread (the limit in a fight) does less: texture lookups, draw state comparisons, the C library's
  thread lookups and memory comparisons.
- New textures are read while the drawing thread draws, and the opening film no longer reads 2 MB it does not use.

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
