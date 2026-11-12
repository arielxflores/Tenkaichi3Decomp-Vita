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
  own save with everything unlocked (yours is not touched). From the versus menu it all happens in the running
  game: the system's ad hoc dialog over the menu, the wait for the other Vita in the menu's message window, and
  after the match the versus menu again, without the program starting anew. See the README.
- **The other game from the options**: Options → Vita Settings → **Game** (the third page) switches
  between Budokai Tenkaichi 3 and Team BT4's Budokai Tenkaichi 4, as the start menu's Game does; BT4's own
  Vita Settings have the way back.
- **Vita Settings on the Options screen**: its second row (where Screen was) opens Vita Settings at once, in BT3
  and in BT4; the PS2's Screen page is inside it (third page in BT3, second in BT4). The controller page no longer
  has the row. Captions longer than the plates (Quick Start, Quick Save, Tech Fixes, Unlock All, Bombardment) are
  narrowed to fit inside them. BT4's options now show these labels too (Vita Settings, Game, Unlock All, Screen and
  the rest): its disc's lettering is the mod's own, so it has its own set, made from that disc
  (`port/vita/vita_optart_bt4.h`); before, BT4's Vita Settings plates showed the PS2's captions.
- **Vita Settings' page number** ("Page 1/3") at the top right while it is open, on a plate of its own like the
  pickers', in BT3 and BT4.
- **Buttons**: Vita Settings → Buttons (third page; in BT4 the second) gives any of the ten fighting buttons any
  one's action, or none: L1 / R1 / L2 / R2 on the face buttons and the other way round. The default layout stays
  until changed; Square on the page puts it back. The menus keep their buttons.
- **The dragon's screen (Shenron) keeps its pace**: a menu screen that the graphics chip cannot draw at 960 x 544
  in the time the game gives each picture is drawn at the fights' size until it gets light again (`menuauto=0`
  in bt3.txt keeps the menus' size whatever it costs).
- **Unlock All, a switch**: Vita Settings → Unlock All (second page; in BT4's options next to Game) turns on every
  character and transformation, stage, song and item (Evolution Z items, Potaras) and 9,999,999 Zenni, for the save
  in play and each one loaded while it is on. Your save is copied aside first, and turning it off gives the save
  its own unlocks and Zenni back. It replaces the one-way Cheats plate, and keeps the records list.
- **Disc Fusion**: the modes of the two earlier games' discs are open (the Vita has no disc to swap).
- A crash writes `ux0:data/bt3/crash.txt` and closes the game, instead of leaving it frozen (with the kubridge
  plugin, which most setups have).
- The log has a line a minute with the picture rate and the memory in use, for bug reports.
- `bt3.txt` `fightres=720` draws the fights at 720 x 408 instead of 640 x 368 (sharper; about the same speed).
- `bt3.txt` `weather=light` / `weather=off` and `moveblur=0` (from the PC version): the haze of big see-through
  sprites some stages have, and the scenery's blur while the view turns fast, thinned out or left out (lighter work
  for the graphics chip on those stages).

Sound

- Clearer music and voices: the game's 24 kHz streams are brought to the Vita's 48 kHz through a proper filter
  (32 samples around each output sample) instead of straight lines between samples. The treble is no longer dulled
  (it was 1.4 dB down at 6 kHz and 4 dB at 10 kHz) and the rough edge above 12 kHz is gone (70 dB down, from 9 to
  17 dB).
- Sound effects played at a changed pitch are drawn through four samples instead of two (less dull, less rough).
- Sound follows the picture more closely: it goes out in blocks of about a hundredth of a second, half as long as
  before. If that ever leaves the output empty, the game goes back to the longer blocks by itself.

Fixed

- BT4: the fight camera stood still (it kept the fight's first picture's pose, so it did not follow the fighters and
  the right stick could not move it).
- The dragon's screen (Shenron): the dragon balls' white outlines no longer lag behind or sit apart from the balls.

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
