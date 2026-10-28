# Changes (PS Vita port)

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

- On the space stage near Frieza's ship (the red planet) and on Namek (the ground under the water), parts of the
  clouds or the water are sometimes missing for a frame while the fighters move, which looks like flickering
  patches. Being worked on.
- Not reproduced yet: a flicker on the mission select screen and the title screen's text looking sliced. A short
  video of either helps a lot.
- Ad-hoc and online play between two Vitas are not implemented yet.
