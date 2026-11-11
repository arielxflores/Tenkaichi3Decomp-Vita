# Changes (PS Vita port)

## 1.2 (in progress)

New

- **The disc image as it is**: copy your `.iso` to `ux0:data/bt3/` and the game reads its files from it; no need to
  unpack it with 7-Zip any more (the unpacked files still work). The same for BT4 and the European disc.
- **Languages**: Options → Sound → Language (the console's Change Voice row) sets the text and the voices together:
  English, Japanese (voices), and with your own European disc (SLES-54945: its image or its `DATA/PZS3EU1.AFS` in
  `ux0:data/bt3/eu/`) Spanish, German, French and Italian menus, HUD and battle subtitles, with English or Japanese
  voices. Based on Matteo Santoro's work.
- **Budokai Tenkaichi 4** (experimental): Team BT4's B14 Rev2 through NauTaro's BT4decomp, from your own BT4 disc's
  image or files in `ux0:data/bt3/bt4/`; chosen in the start menu. See the README.
- **Vita Settings, a second page** (from the PC version, z3xox's): **Cheats → Unlock All** in the game (every
  character, stage, song and item, and the most Zenni: the game's own debug unlock), **Quick Start** (no logos,
  opening film or copyright page), **Quick Save** (saving and loading without the memory card window, on by
  default) and **Tech Fixes** (the community's Z Counter and Bombardment fixes, off by default).
- **Dragon History: Fight Again** in the pause menu, to start a story fight over (from the PC version).
- **Stages and songs from outside the disc** (from the PC version): `.unk` stage models in `ux0:data/bt3/stages/`
  and MP3 or ADX songs in `ux0:data/bt3/songs/` join the versus menus' stage grid and music list, their names in
  the game's own lettering. See the README.
- **Start menu**: hold SELECT while the game starts for the game (BT3 or BT4), the language and an **Unlock
  everything** cheat (the game's own unlock: all characters, stages, music and items, and 4,850,000 Zenni, given to
  the save it loads).
- **Multiplayer between two Vitas**: the versus mode against another Vita, ad hoc (no router) or over the same
  Wi-Fi network: Versus → Multiplayer → Ad Hoc: Host / Ad Hoc: Join (the host's Battle Settings go with it), or the
  start menu (ad hoc and Wi-Fi). One hosts, the other joins. Both games are kept in step by the buttons
  pressed, as in the PC version's online play; each Vita shows its own player's view of the fight. A match has its
  own save with everything unlocked (yours is not touched). See the README.
- **Disc Fusion**: the modes of the two earlier games' discs are open (the Vita has no disc to swap).
- A crash writes `ux0:data/bt3/crash.txt` and closes the game, instead of leaving it frozen (with the kubridge
  plugin, which most setups have).
- The log has a line a minute with the picture rate and the memory in use, for bug reports.
- `bt3.txt` `fightres=720` draws the fights at 720 x 408 instead of 640 x 368 (sharper; about the same speed).
- `bt3.txt` `weather=light` / `weather=off` and `moveblur=0` (from the PC version): the haze of big see-through
  sprites some stages have, and the scenery's blur while the view turns fast, thinned out or left out (lighter work
  for the graphics chip on those stages).

Fixed

- A second controller paired with the VitaControl plugin moved player 1 as well as player 2.
- Budokai Tenkaichi 4: in a split-screen fight (two controllers), a moment the game shows as one full-screen view
  without a camera of its own could leave player 2's fighter (and their blinding effect) out.
- L3 / R3 on the touch panels (a corner of the screen, or the rear pad): a touch is a quick press, so
  transformations happen (the game ignores an R3 held longer than about a fifth of a second).
- The large lettering (the title screen's, the menus' headings) no longer looks cut through.
- No freeze of about a second when the opening film ends or is skipped.
- Graphics memory no longer leaks when it runs short (a part of a texture with a palette was left behind): a likely
  cause of the slowdown and the missing effects after many fights in one session. The log's line a minute shows
  the texture tables and the free memory, for reports of long sessions.
- A two-player battle without a second controller says so in the Vita's words, not the PS2's ("No DUALSHOCK 2
  analog controller ... controller port 2").

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
