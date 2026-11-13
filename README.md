# Tenkaichi3Decomp for the PS Vita

A native PS Vita port of **Dragon Ball Z: Budokai Tenkaichi 3** (PlayStation 2, USA release, SLUS-21678), based
on z3xox's [Tenkaichi3Decomp](https://github.com/z3xox/Tenkaichi3Decomp), the PC port built on the
BT3-Decompiled decompilation. The game's own code is compiled for the Vita, with a renderer for the Vita's GPU
(vitaGL), sound, touch and controls written for the handheld. It is not an emulator.

This repository contains **no game data**. The game's files are copied from your own disc of the USA release to
the memory card, see [Game files](#game-files).

This is an unofficial port. It is not affiliated with the Tenkaichi3Decomp project; please report problems with
the Vita version here, not upstream.

<p align="center">
  <img src="docs/screenshots/title.jpg" width="49%">
  <img src="docs/screenshots/fight1.jpg" width="49%">
  <img src="docs/screenshots/fight2.jpg" width="49%">
  <img src="docs/screenshots/controller.jpg" width="49%">
</p>

## What works

- The game from its opening film through the menus to the fights, saving and loading included. The modes are the
  PS2 game's own code.
- The PS2 version's screen effects: the fighters' outlines, ground shadows, glow, blur and the depth tint.
- Music, voices and sound effects; the opening and ending films.
- Fights are drawn for the Vita's 16:9 screen without stretching; the menus are drawn at the screen's full
  resolution.
- The touch screen works in the menus, and the rear touch pad gives the L2, R2, L3 and R3 buttons the Vita lacks
  (grip attachments made for PS4 Remote Play work with it).

Fights run at the PS2 version's 30 fps. A few stages with heavy effects drop below that at times, most noticeably
Kame House (its reflective sea), where it can go down to about 26 fps. Fights are drawn at 720 x 408 while the
graphics chip has the time for it and at 640 x 368 while it has not (the dynamic resolution of Killzone: Mercenary),
so they are as sharp as the moment allows without losing pictures.

Two players: on a PS TV (or a Vita with a second controller paired), the second DualShock is player 2: the Versus
Menu's Multiplayer → Two Controllers is the PS2's two-controller mode. Two Vitas can also play the versus mode against
each other, ad hoc (Multiplayer → Ad Hoc: Host / Join) or over the same Wi-Fi network (experimental): see
[Multiplayer](#multiplayer-two-vitas-experimental).

What changed in each version: [CHANGELOG.md](CHANGELOG.md).

## Installation

Requirements:

- A PS Vita running HENkaku / Ensō
- Recommended: the [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker) kernel plugin, which lets the game use
  the Vita's fourth CPU core. Without it the game runs, but heavy scenes are slower.
- About 3 GB free on `ux0:` for the game files

The game's shaders come already compiled in the package, so `libshacccg.suprx` is not needed (if it is in
`ur0:data/`, it is used only for a shader the package lacks).

Steps:

1. Install `Tenkaichi3Decomp-Vita.vpk` from the [releases page](https://github.com/arielxflores/Tenkaichi3Decomp-Vita/releases)
   with VitaShell.
2. Copy your disc image (`.iso`) to `ux0:data/bt3/` as described below.
3. Start **Dragon Ball Z: BT3** from the LiveArea.

## Game files

You need a disc image (`.iso`) of your own copy of the USA release, SLUS-21678. Other releases (PAL, Japanese) are
not supported. Copy the image as it is to `ux0:data/bt3/` with VitaShell (USB or FTP), under any name ending in
`.iso`:

```
ux0:data/bt3/Dragon Ball Z Budokai Tenkaichi 3.iso
```

The game reads its files from the image. Or copy only `SLUS_216.78` and the `BIN` and `DATA` folders from the
image (with [7-Zip](https://www.7-zip.org/); the rest of the disc is not needed), so that the memory card has:

```
ux0:data/bt3/disc/SLUS_216.78
ux0:data/bt3/disc/BIN/DBZP.BIN
ux0:data/bt3/disc/DATA/PZS3US0.AFS
ux0:data/bt3/disc/DATA/PZS3US1.AFS
ux0:data/bt3/disc/DATA/PZS3US2.AFS
ux0:data/bt3/disc/DATA/ZS3USOP.PSS        (and the other files of the DATA folder)
```

- **Windows:** open the `.iso` with [7-Zip](https://www.7-zip.org/), select `SLUS_216.78`, `BIN` and `DATA` and
  extract them into a folder named `disc`.
- **Linux / macOS:** `7z x -odisc your-image.iso SLUS_216.78 BIN DATA`

Then copy the `disc` folder to `ux0:data/bt3/`. Either way, the game checks at start that the files are the
unmodified USA release and tells you if they are not.

### Languages

**Options → Sound → Language** sets the language of the text and of the voices together:

| Language | Text (menus, HUD, battle subtitles) | Voices |
|---|---|---|
| English | English | English |
| Japanese | English (no release has other text) | Japanese |
| Español, Deutsch, Français, Italiano | Spanish, German, French, Italian | English or Japanese, chosen next |

English and Japanese come with the USA disc. Spanish, German, French and Italian need your own copy of the European
release (SLES-54945); the European disc has only English and Japanese voices, so those are the voices it offers.
Copy its disc image (`.iso`) to `ux0:data/bt3/eu/` (or beside the USA one in `ux0:data/bt3/`), or only
`DATA/PZS3EU1.AFS` from it to

```
ux0:data/bt3/eu/PZS3EU1.AFS
```

In Spanish the Options screen is the European disc's too, with the Vita's rows and Vita Settings in Spanish (Ajustes
Vita, Idioma, Control, Ajustes del control, and the guide lines), and so are the start menu and the versus menu's
Multiplayer. The Spanish text says "control" and "controles" where the discs say "mando" and "mandos".
In German, French and Italian the Options screen stays in English.

With it, the game asks for the language at its next start; the [start menu](#start-menu) has the list too. A new
language shows from the next screen (the options screen itself stays in English, with the Vita's rows). The folder
`ux0:data/bt3/pzs3eu1/` written by the PC version's `extract_eu.py` is read too.

## Controls

The **Normal** control type of the Vita version:

| Vita | Action |
|---|---|
| Left stick | Move |
| Square | Fight |
| Triangle | Ki Blast |
| Cross | Dash |
| Circle | Guard |
| L | Ki Charge |
| R | Ascend |
| Rear touch pad, upper left quarter (L2) | Blaster Wave |
| Rear touch pad, upper right quarter (R2) | Descend |
| Rear touch pad, lower left / lower right quarter | L3 / R3 |
| START | Pause |

On a PS TV, or with a DualShock paired, the controller's own L2, R2, L3 and R3 buttons are used.

- The rear touch pad's quarters are those of PS4 Remote Play, so the triggers and rear buttons of a grip attachment
  for it, such as HORI's Remote Play Assist Attachment, work as L2, R2, L3 and R3.
- **Options → Vita Settings** (the Options screen's first row; the rows are Vita Settings, Controls, Sound, Save/Load) has ten settings on three
  pages (Down from the lower row turns the page):
  - **Touch**: for each of L2, R2, L3 and R3, where it is touched: its quarter of the rear pad, a corner of the
    front screen (L2 / R2 the bottom corners, L3 / R3 the top ones), or nowhere (Off). A finger resting on the rear
    pad's lower half stops counting as L3 / R3 after a second, so the hands holding the Vita cannot hold R3 (which
    stops the fighter from moving); lift it and touch again to press it. SELECT + L / R also give L2 / R2.
  - **Clashes**: **Square** (the default) wins beam struggles and rush clashes by pressing Square again and again,
    instead of turning the left stick; the prompt shows Square. **Stick** turns the stick, as on the PS2 (the d-pad
    works too).
  - **2nd Pad**: a second controller plays as player 2 (on by default).
  - **Mods**: the files in `ux0:data/bt3/mods/` are used (on by default; see [Mods](#mods)).
  - **Unlock All** (ON / OFF): every character and transformation, every stage, every song, every item (the
    Evolution Z items and the Potaras among them) and 9,999,999 Zenni, by the game's own unlock of everything (left
    in by its developers and never used by the game; the PC version's Cheats tab). Your records list is kept.
    **ON** (CROSS twice) first copies your save as it is to `ux0:data/bt3/unlock_backup.bin`, then unlocks
    everything; every save loaded while it is on gets it too. **OFF** (CROSS twice) gives your save its own unlocks
    and Zenni back from that copy (progress since then in other things, such as Dragon History, stays). The change
    is written to your save as you leave the options. BT4's Vita Settings have the same switch (on their second
    page, with its own copy in `ux0:data/bt3/bt4/`).
  - **Quick Start**: the game starts at its title screen, without the logos, the opening film and the copyright
    page (off by default).
  - **Quick Save**: saving and loading without the memory card window and its waits; a question to answer or an
    error still shows the window (on by default; the PC version's quick saving).
  - **Tech Fixes**: the community's fixes of the fighting mechanics, each on or off (both off by default), from the
    next fight and never in a replay: **Z Counter** keeps a timed Z Counter that the game loses when your movement
    changes your hitbox in the frame the attack arrives; **Bombardment** stops body contact from breaking a chain of
    smash Ki blasts with dashes between them. From the community's "BT3 PS2 Port Gameplay Source" through the PC
    version.
  - **Game**: **BT3** or **BT4**, the package's other game ([Budokai Tenkaichi 4](#budokai-tenkaichi-4-experimental),
    once its files are in place). Press CROSS twice on the other game: this one closes and the other starts, and
    starts each time from then on, until changed back (BT4's Vita Settings have the same plate). Options changed and
    not saved yet are lost with it.
  - **Screen**: the PS2's Screen page (camera type, screen shake, screen position), which Vita Settings took the place
    of on the Options screen; back from it leads to the Options screen. (In BT4: on the second page, with the tag
    team's HUD.)
- **Options → Controls → Controller Settings → Custom** (the PS2's Controller → Control Type) assigns the actions to other buttons, as on the PS2, and on the
  Vita every one of the eight buttons can be changed, L1, L2, R1 and R2 too (Up / Down go round each column; the
  PS2's page reached only the face buttons), each to any of the eight actions: hold Cross and press Left / Right. So
  the shoulder buttons' actions can go on the face buttons and the other way round; the button that had the action
  takes the other one. The pause
  menu's skill list shows each command with the buttons of the control type in use, and L3 / R3 as buttons like L1,
  not as the PS2's sticks.
- In the menus, tap an item to move to it and tap it again to choose it; swipe to scroll, and tap with two
  fingers to go back. A tap skips a film.
- Holding START + SELECT for two seconds closes the game (press START on the screen that follows).

### Start menu

Hold SELECT while the game starts (from the LiveArea until the text screen shows) for:

- **Game**: Budokai Tenkaichi 3 or [Budokai Tenkaichi 4](#budokai-tenkaichi-4-experimental). The choice is kept: BT4
  then starts each time, until you change it here again.
- **Language**: with the European archive (see [Languages](#languages)).
- **Unlock everything (cheat)**: the game's own unlock, which the game never uses: every character, stage, song and
  item, and 4,850,000 Zenni, given to the save the game loads, once (its records list is emptied). It stays once
  the game saves, and cannot be taken back: copy `ux0:data/bt3/saves/` first if you want to keep the save as it is.
  The game's own switch, which can be taken back, is Vita Settings → Unlock All.
- **Multiplayer**: a match against another Vita (experimental), see [Multiplayer](#multiplayer-two-vitas-experimental).

## Budokai Tenkaichi 4 (experimental)

The package also carries **Team BT4's Budokai Tenkaichi 4 B14 Rev2**, through
[NauTaro's BT4decomp](https://github.com/NauTaro/DBZ-BT4-Tag-Team-Decomp), the mod's game running on Tenkaichi3Decomp,
built for the Vita with this port. You need your own disc image of BT4 B14 Rev2 (its English or Spanish release, as
published by Team BT4). Copy the image as it is to `ux0:data/bt3/bt4/` (any name ending in `.iso`), or
`SLUS_219.78` and the `BIN` and `DATA` folders from it, so that the memory card has:

```
ux0:data/bt3/bt4/SLUS_219.78
ux0:data/bt3/bt4/BIN/DBZ4.BIN
ux0:data/bt3/bt4/BIN/DBZP.BIN
ux0:data/bt3/bt4/DATA/PZS4US0.AFS     (to PZS4US3.AFS, and the other files of the DATA folder: about 5.9 GB)
```

Then choose it in the game, **Options → Vita Settings → Game → BT4** (on the third page: Down from the lower row,
twice), or hold SELECT while the game starts and choose **Game** in the [start menu](#start-menu). The
way back is the same plate in BT4's own Vita Settings, or the start menu. BT4 keeps its own saves,
settings and log in `ux0:data/bt3/bt4/`.

**BT4 in Spanish**: Team BT4's Spanish release has the game's text and voices in Spanish. To keep both releases, put
the English one in `ux0:data/bt3/bt4/` as above and the Spanish one in `ux0:data/bt3/bt4/es/` (its `.iso`, or its
`SLUS_219.78`, `BIN` and `DATA`). Then choose in BT4's **Options → Vita Settings → Language** (second page):
**English** or **Español**, CROSS twice, and the game starts again in it. The options' Vita rows, Vita Settings'
guide lines and the versus menu's Multiplayer are in Spanish too. With only one of the two releases on the memory
card, BT4 starts in that one. If the game does not get going in the language just chosen, the next start goes back
to the one before.

The PC version's online play, Tag Team battles and its other PC features are
not part of the Vita's. Should BT4 crash or freeze before its first pictures, the next start says so and starts BT3
instead (`ux0:data/bt3/bt4/log.txt` and `crash.txt` tell what happened: please attach them to a report).

## Mods

Files in `ux0:data/bt3/mods/` are used instead of the disc's, laid out as for the PC version of Tenkaichi3Decomp:

```
ux0:data/bt3/mods/pzs3us1/00042.bin      file 42 of DATA/PZS3US1.AFS (pzs3us0, pzs3us2 for the other archives)
ux0:data/bt3/mods/disc/DATA/<NAME>       a file of the disc's DATA folder, a whole .AFS archive included
```

The number of an archive's file may also be written without leading zeros and with another extension
(`42.pak`). A mod can also have a folder of its own (a pack), laid out the same way inside, or holding the `DATA`
folder of a modded disc as it is:

```
ux0:data/bt3/mods/<Pack>/pzs3us1/00042.bin
ux0:data/bt3/mods/<Pack>/DATA/PZS3US1.AFS
```

Packs are used in the order of their names, after the loose files (the first file found for a slot is the one
used). A pack whose name starts with `_` is skipped. The folders are read when the game starts; **Vita Settings →
Mods** shows what is in use and turns it all off without deleting anything (from the next start).

Mods that change only the disc's data files work. Mods that change the game's program cannot: the program is
compiled into the port from the unmodified USA release, and those mods' data files are made for their own program.
Budokai Tenkaichi 4 is the exception, as a program of its own (see above). A pack holding a changed `SLUS_216.78` or `BIN/DBZP.BIN` is left
out, with a message when the game starts. A modded disc's image (`.iso`) is not read as a pack: copy its files
into a pack instead.

## Stages and songs from outside the disc

New stages and songs can be added to the versus menus (one-on-one, team and DP battles, and training), as in the
PC version:

```
ux0:data/bt3/stages/My_Stage.unk       a stage: the .unk stage model BT3 modders release (the file such a mod
                                       puts in a disc stage's place under PCSX2)
ux0:data/bt3/songs/My_Song.mp3         a song: an MP3, or an ADX (the game's own kind)
```

Each stage gets a place of its own in the stage grid after the disc's 36, and each song a place in the music list
before "Random"; "Random" draws from them too. Their names are the files' names, an underscore read as a blank
(`My_Stage.unk` is "My Stage"), drawn in the game's own lettering, cut from the disc's names: the letters J, Q, X,
Y, Z, j, q, x, digits and most punctuation are not in it, and a name with one of them is written in the game's
plain font instead. To give other names or an order, write `maps.txt` in the stages folder and `songs.txt` in the
songs folder, a line `file|Name In The Menu` each; the files they do not name follow, by name. Up to 26 stages and
40 songs are used.

- An added stage shows the picture and the icon of the disc stage it was made from, and uses that stage's sounds.
  A stage larger than the game's stage memory (7.1 MB) is given more memory, up to 24 MB.
- An MP3 is made into an ADX beside it (`My_Song.adx`) the first time the game starts with it, in the background
  (some seconds a song); it is in the list from the next time a select screen opens. Songs above 32 kHz are made
  at 32 kHz, and they play round and round like the disc's music. An MP3 that cannot be read is listed in
  `mp3-failed.txt` in the songs folder. Deleting the MP3 later keeps the song (its ADX).
- Nothing about them goes into the save, so a save stays good without them; the folders are read when the game
  starts.

## Multiplayer (two Vitas, experimental)

**Experimental**: matches between two Vitas have not been tested on two real Vitas yet. They may stop with a
desync or a lost connection; please report what happens, with `ux0:data/bt3/log.txt` from both Vitas. Online play
over the internet (as the PC version's lobbies) is not there yet.

Two Vitas can play the versus mode against each other: **ad hoc** (the two Vitas near each other, no router needed)
or over the **Wi-Fi** network both are connected to. Both need this same version of the game and the files of the
same disc.

1. In the game: Versus → **Multiplayer**, then **Ad Hoc: Host** on one Vita and **Ad Hoc: Join** on the other
   (**Two Controllers** is the console's own two-player mode, for a PS TV or a paired controller). It all happens
   there, in the versus menu: the system's own ad hoc dialog connects the two Vitas over the menu, then the menu's
   message window says who is waited for (Triangle gives up). The host's Battle Settings are the match's; the one
   who joins takes the host nearby.
2. When the two have found each other the match loads (a few seconds: the game begins anew inside the running
   program, so that both Vitas' games are exactly the same) and opens on the versus mode's character select, where
   each player chooses with their own Vita. The host is player 1 and chooses the stage.
3. Going back out of the character select ends the match: you are in the versus menu again, with your own save.

The other way, which also has **Wi-Fi**: hold SELECT while the game starts and choose **Multiplayer** in the start
menu (Host a match / Join a match, ad hoc or Wi-Fi; the one who joins picks the host from a list). A match begun
there ends with the program starting again.

- A match has a save of its own (`ux0:data/bt3/net_saves/`) with everything unlocked; your own save is not touched.
  The two games have to be exactly the same, so mods, the stages and songs added from outside the disc, the European
  languages and the Tech Fixes are left out of a match, and it starts without the logos and the opening film.
- Each Vita shows its own player's view of the fight, full screen.
- Hosting from the versus menu takes your Battle Settings (time, COM level, referee, transformations, map) into
  the match; from the start menu, the time limit can be set there (Multiplayer → Time limit when hosting).
- The match ends when a player goes back out of the character select, when the other player leaves or when the
  connection is lost. While the game waits for the other Vita, holding SELECT + START leaves the match.
- What you had not saved when the match began (options changed in that session, say) is not kept: the game loads
  your save again after the match.
- `netwarm=0` in `ux0:data/bt3/bt3.txt` makes the versus menu start the whole program again for a match, as the
  start menu's way does (should the way without it give trouble). The menus are drawn at the screen's full size
  for the system's dialog to show over them; where they are not, the program starts again by itself.
- The two games stay in step by sending each other the buttons pressed, as the PC version's online play does: when
  the connection is slow, both wait for it. If the two games ever stop agreeing (a desync), `log.txt` says so.

Multiplayer is new: when something goes wrong, `ux0:data/bt3/log.txt` from both Vitas helps.

## Troubleshooting

- **"The game's files were not found"**: the disc image (`.iso`) must be in `ux0:data/bt3/` or
  `ux0:data/bt3/disc/`, or `SLUS_216.78` and the `BIN` and `DATA` folders in `ux0:data/bt3/disc/` (a folder named
  `disk`, or one more folder level in between, is found too). The message lists what it found instead. An image
  of another release (PAL, Japanese) or of a modded disc is not used.
- **The fights' picture is smaller than the menus' (it fills only part of the screen)**: the fights are drawn at
  640 x 368 (720 x 408 with `fightres=720`) and the Vita's display scales them to the screen. The Sharpscale plugin in
  a scaling mode other than Original (Integer, for one) shows them unscaled instead: set it back to Original.
- **The game stops** (the picture freezes, the music plays on): the game writes `ux0:data/bt3/hang.txt` when its
  picture stops for a few seconds. Attaching it and `log.txt` to a bug report helps a lot.
- **The game closes by itself**: it crashed; `ux0:data/bt3/crash.txt` says where (with the kubridge plugin
  installed, as with most homebrew setups). Attach it and `log.txt` to the bug report.

## Notes

- Saves (a virtual memory card) and settings are kept in `ux0:data/bt3/`: `saves/`, `bt3_settings.txt` (Clashes,
  2nd Pad, Mods) and `touch.txt` (Touch); a multiplayer match's own save is `net_saves/`. `log.txt` there describes the last runs, which helps with bug reports.
- The first start takes a little longer: the shaders are compiled and kept in `ux0:data/bt3/shcache/`.
- The port runs the Vita at its standard maximum clocks (CPU 444 MHz, GPU 222 MHz); no overclocking plugin is
  used or needed.
- Optional settings go in `ux0:data/bt3/bt3.txt`, one per line: `wide=0` shows the PS2's original 4:3 picture,
  `disc=<folder>` reads the game files from another folder, `fightres=640` or `fightres=720` draws the fights always
  at 640 x 368 or 720 x 408 instead of choosing between the two as they go (`fightres=960`, the screen's own
  resolution, is slower), `lang=0` to
  `4` sets the language (English, Spanish, German, French, Italian), `asyncread=0` reads the game's files on its
  own thread as before. `weather=light` or `weather=off` thins out or leaves out the haze some stages have drifting
  over them (Desert and Rocky Area at noon, Rocky Area in the evening, Kami's Lookout; a little on Glacier, Muscle
  Tower and the Time Chamber: big see-through sprites over the whole picture, the heaviest thing on those stages
  for the graphics chip), and `moveblur=0` the blur of the scenery while the view turns fast (both from the PC
  version). For Budokai Tenkaichi 4 these two go in `ux0:data/bt3/bt4/bt3.txt`.

## Building

See [docs/vita/building.md](docs/vita/building.md). The build needs VitaSDK, vitaGL with this port's changes
(`port/vita/vitagl`), Python 3 and libclang; no game files are needed to build. A development build with test
options is described in [docs/vita/development.md](docs/vita/development.md).

The rest of the repository is Tenkaichi3Decomp as of commit
[`dd6b20c`](https://github.com/z3xox/Tenkaichi3Decomp/commit/dd6b20ce13dc01871983a8176196f5cb772fa7a8), with the
changes the Vita needed in the shared code, most of them inside `#ifdef PORT_VITA`. Its own README is
[docs/port/README-PC.md](docs/port/README-PC.md). The PC build is not maintained here: for the PC version, use
[upstream](https://github.com/z3xox/Tenkaichi3Decomp).

## Credits

- [z3xox](https://github.com/z3xox) and the contributors of
  [Tenkaichi3Decomp](https://github.com/z3xox/Tenkaichi3Decomp), the PC port this one is built on, and the
  authors of the BT3-Decompiled decompilation it uses; z3xox's later fixes are ported here (the seam models,
  the story mode's saga list, the palette tables) along with the PC version's Square option for clashes, its
  developer cheat (Unlock All), Quick Start, Quick Save, Fight Again, and the stages and songs added from outside
  the disc with their names in the game's lettering (with valenvivaldi's MP3 conversion), and its online play,
  whose design (the two games kept in step by their buttons) the Vitas' multiplayer follows; the community's Z
  Counter and Bombardment fixes come from there too
- [NauTaro](https://github.com/NauTaro) for [BT4decomp](https://github.com/NauTaro/DBZ-BT4-Tag-Team-Decomp), whose
  game code the Budokai Tenkaichi 4 program is built from, and Team BT4 for Budokai Tenkaichi 4
- [Matteo Santoro](https://github.com/Matt-code-d) for the European languages and the Dragon World Tour fix
  (Yamcha's invitation), and [limeking614-dev](https://github.com/limeking614-dev) for the PS TV controller
  support, from their forks
- Spike, the developer of Dragon Ball Z: Budokai Tenkaichi 3
- [Rinnegatamante](https://github.com/Rinnegatamante) for vitaGL and vitaShaRK, and for vitaQuake, whose ad hoc
  multiplayer showed how the Vita connects ad hoc; [bythos14](https://github.com/bythos14) for SceShaccCgExt
- [GrapheneCt](https://github.com/GrapheneCt) for CapUnlocker
- The [VitaSDK](https://vitasdk.org/) team
- The third-party code and data used by the port are listed in [THIRD_PARTY.md](THIRD_PARTY.md).

## License

The Vita port's code (`port/vita/` and the changes for the Vita in the rest of the tree) is under the
[GNU Affero General Public License, version 3](LICENSE), the license of Tenkaichi3Decomp's port code since
October 2026. The port code it is based on, from before that change, came under the MIT license; its notice is
kept in [LICENSE-MIT](LICENSE-MIT). `port/vita/vitagl/vitagl.patch` modifies vitaGL and is under vitaGL's license
(LGPL-3.0).

The game's code in `src/` and `include/` comes from the decompilation, and the LiveArea images use the game's
artwork: they belong to the game's rights holders. This project is not affiliated with or endorsed by them, and
distributes none of the game's data. You need your own legally obtained copy of the game to use it.
