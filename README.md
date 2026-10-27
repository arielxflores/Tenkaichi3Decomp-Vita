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
Kame House (its reflective sea), where it can go down to about 26 fps.

Two players: on a PS TV (or a Vita with a second controller paired), the second DualShock is player 2, and the
Versus Menu's "Multiplayer" entry is the PS2's two-controller mode. Ad-hoc and online play between two Vitas are not
implemented yet.

What changed in each version: [CHANGELOG.md](CHANGELOG.md).

## Installation

Requirements:

- A PS Vita running HENkaku / Ensō
- `libshacccg.suprx` extracted to `ur0:data/` (see
  [this guide](https://samilops2.gitbook.io/vita-troubleshooting-guide/shader-compiler/extract-libshacccg.suprx))
- Recommended: the [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker) kernel plugin, which lets the game use
  the Vita's fourth CPU core. Without it the game runs, but heavy scenes are slower.
- About 3 GB free on `ux0:` for the game files

Steps:

1. Install `Tenkaichi3Decomp-Vita.vpk` from the [releases page](https://github.com/arielxflores/Tenkaichi3Decomp-Vita/releases)
   with VitaShell.
2. Copy the game files to `ux0:data/bt3/disc/` as described below.
3. Start **Dragon Ball Z: BT3** from the LiveArea.

## Game files

You need a disc image (`.iso`) of your own copy of the USA release, SLUS-21678. Other releases (PAL, Japanese) are
not supported. From the image, copy `SLUS_216.78` and the `BIN` and `DATA` folders, so that the memory card has:

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

Then copy the `disc` folder to `ux0:data/bt3/` with VitaShell (USB or FTP). The game checks at start that the
files are the unmodified USA release and tells you if they are not.

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
- **Options → Controller → Vita Settings** has four settings:
  - **Touch**: for each of L2, R2, L3 and R3, where it is touched: its quarter of the rear pad, a corner of the
    front screen (L2 / R2 the bottom corners, L3 / R3 the top ones), or nowhere (Off). A finger resting on the rear
    pad's lower half stops counting as L3 / R3 after a second, so the hands holding the Vita cannot hold R3 (which
    stops the fighter from moving); lift it and touch again to press it. SELECT + L / R also give L2 / R2.
  - **Clashes**: **Square** (the default) wins beam struggles and rush clashes by pressing Square again and again,
    instead of turning the left stick; the prompt shows Square. **Stick** turns the stick, as on the PS2 (the d-pad
    works too).
  - **2nd Pad**: a second controller plays as player 2 (on by default).
  - **Mods**: the files in `ux0:data/bt3/mods/` are used (on by default; see [Mods](#mods)).
- **Options → Controller → Control Type → Custom** assigns the actions to other buttons, as on the PS2. The pause
  menu's skill list shows each command with the buttons of the control type in use, and L3 / R3 as buttons like L1,
  not as the PS2's sticks.
- In the menus, tap an item to move to it and tap it again to choose it; swipe to scroll, and tap with two
  fingers to go back. A tap skips a film.
- Holding START + SELECT for two seconds closes the game (press START on the screen that follows).

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

Mods that change only the disc's data files work. Mods that change the game's program, like Team BT4's
Budokai Tenkaichi 4, cannot: the program is compiled into the port from the unmodified USA release, and those
mods' data files are made for their own program. A pack holding a changed `SLUS_216.78` or `BIN/DBZP.BIN` is left
out, with a message when the game starts. A disc image (`.iso`) is not read: copy a modded disc's files into a
pack instead.

## Troubleshooting

- **"The game's files were not found"**: `SLUS_216.78` and the `BIN` and `DATA` folders must be in
  `ux0:data/bt3/disc/` (a folder named `disk`, or one more folder level in between, is found too). An `.iso` cannot
  be read as it is: extract those three items from it with 7-Zip.
- **The picture fills only part of the screen**: the Sharpscale plugin is set to a resolution other than
  "Original"; set it back to Original.
- **The game stops** (the picture freezes, the music plays on): the game writes `ux0:data/bt3/hang.txt` when its
  picture stops for a few seconds. Attaching it and `log.txt` to a bug report helps a lot.

## Notes

- Saves (a virtual memory card) and settings are kept in `ux0:data/bt3/`: `saves/`, `bt3_settings.txt` (Clashes,
  2nd Pad, Mods) and `touch.txt` (Touch). `log.txt` there describes the last runs, which helps with bug reports.
- The first start takes a little longer: the shaders are compiled and kept in `ux0:data/bt3/shcache/`.
- The port runs the Vita at its standard maximum clocks (CPU 444 MHz, GPU 222 MHz); no overclocking plugin is
  used or needed.
- Optional settings go in `ux0:data/bt3/bt3.txt`, one per line: `wide=0` shows the PS2's original 4:3 picture,
  `disc=<folder>` reads the game files from another folder.

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
  the story mode's saga list, the palette tables) along with the PC version's Square option for clashes
- [Matteo Santoro](https://github.com/Matt-code-d) for the Dragon World Tour fix (Yamcha's invitation) and
  [limeking614-dev](https://github.com/limeking614-dev) for the PS TV controller support, from their forks
- Spike, the developer of Dragon Ball Z: Budokai Tenkaichi 3
- [Rinnegatamante](https://github.com/Rinnegatamante) for vitaGL and vitaShaRK, and [bythos14](https://github.com/bythos14)
  for SceShaccCgExt
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
