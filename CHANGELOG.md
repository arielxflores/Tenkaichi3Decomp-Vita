# Changes (PS Vita port)

## 1.2.6

New

- **Multiplayer over the internet** (experimental): two Vitas play the versus mode from anywhere, by the host's
  address. Versus → Multiplayer now offers **Online** (first), **Ad Hoc** and **Two Controllers**; Online has **Online: Host**,
  which shows the host's **code** in the message window (eight letters and digits, `2420-7E1Y`: the host's internet
  address written shorter, with a check letter against typos; no server), and **Online: Join**, where the other
  player types it with the system's keyboard (the last one is offered again). All of it in the menu, as ad hoc is; the start
  menu (SELECT a few times as the game starts) → Multiplayer → Host / Join a match (internet) too. The game asks the
  host's router to let the match in (UPnP), finds the host's address as the internet sees it (STUN), says when the
  router cannot be passed (a second router, the provider's own) and what to do, and sets the input delay from the
  measured time there and back. No server and no account. See the README's Multiplayer → Over the internet.
- **Multiplayer: what was lost is sent again from what the other Vita says it has.** Each packet carried the last
  12 inputs only, enough for the short input delays of ad hoc and Wi-Fi; with the longer delays of the internet a
  stretch of lost packets could leave both Vitas waiting until the connection was given up. Each Vita now says what
  it has of the other's inputs, and the rest is sent again from there.

## 1.2.5

Fixed

- **The start menu with SELECT.** SELECT was looked at in one moment as the game started, and a Vita can pass on a
  button held since the LiveArea late: holding it often did nothing (pressing it a few times worked). Now SELECT
  pressed or held at any moment from the game's start until its start menu's turn comes (and for a second after)
  opens the start menu, however long the start takes. The game's messages now say "press SELECT a few times as the
  game starts".
- **"out of memory (4294947232 bytes)"** now says what it means: one of the disc's files is damaged (its copy differs
  from the disc, though its size may not), and to copy the game's files again or try another memory card.
- **With the PSVshell plugin installed** the fights are drawn at 720 x 408 always. PSVshell's hook of the display
  held back the moment the game reads the graphics chip's time from, so the game took the chip for much slower than
  it was and changed the fights' size again and again (hundreds of times an hour): a PS TV's picture broke up, and a
  Vita froze after an hour of play. `fightres=auto` in `bt3.txt` has the sizes chosen as they go again.

New

- **Team battles from the versus menu**: Versus → Multiplayer → **Ad Hoc: Host**, L / R choose one fighter each or
  teams (the line in the message window says which). The start menu's Multiplayer has the same choice.

## 1.2.4

Fixed

- **Fusion from the touch panels.** Since 1.2 a touch of R3 (the rear pad's lower right quarter, or its front
  corner) was always a short press, so that a hand resting on the rear pad could not hold R3 and stop the fighter;
  but the game's fusion is R3 held with left, up or right, which could no longer be done. Now R3 touched while left,
  up or right is held stays held as long as the direction is (at most a second and a half): hold the direction, touch
  R3 and keep both. A touch without a direction is still a short press (transformations).
- **BT3 no longer takes the files in `ux0:data/bt3/bt4/` for its disc.** Files there with BT3's program name
  (BT3's own files put there, or an older BT4 release made on BT3's files) were used as BT3's disc, and the game
  stopped at once with "out of memory (4294947232 bytes)". The "files were not found" message now says where BT3's
  files go and which BT4 release the package runs.
- **A data file that was not copied whole is named at the start.** The game checks the disc's `DATA/*.AFS` files
  against their own index: one cut short (a copy or an unpacking that did not finish) is named with its size and the
  size it needs, instead of the game crashing when it reaches the missing part.

New

- **Budokai Tenkaichi 4 found**: the first start that finds BT4's files says how to switch to it (Options → Vita
  Settings → Game, or SELECT held while the game starts). Once.
- **uma0**: the disc's files (its image, or `SLUS_216.78`, `BIN` and `DATA`) and BT4's are also found in
  `uma0:data/bt3/` (`disc/`, `bt4/`), for an SD2Vita mounted as uma0 beside a memory card. Saves, settings and mods
  stay in `ux0:data/bt3/`.
- **Multiplayer team battles**: hosting from the start menu, the host chooses one fighter each or teams (the versus
  mode's team select: up to five fighters each, switching and fusion).

## 1.2.3

Faster (manumastro's work, pull request #6)

- **Detailed stages stay fast.** Three causes, all in the copies of the game's model calls the port keeps: their
  cache filled up over a session and was never emptied (a stage played after others ran without it), the kept parts'
  places were too few and shared at random, and only the first 2 MB of a stage's vertices could be kept, so a fight
  began fast and slowed down as more of the stage came into view. The first fight on a stage is as fast as the next
  ones, and kept parts are no longer drawn the long way every other frame. On the Vita, in a duel at 720 x 408: stage
  4 at 29.5 pictures a second (1.2.1: 26.2), stage 27 at 29.5 (24.8), Kame House at 29.8 (28.8), against the
  game's 30.
- **The first steps of a native renderer** (the "engine seam"): the game names the stage's meshes and animated
  objects, the fighters' model parts and their textures to the renderer, which no longer has to find them again in
  the PS2's display list every frame; the effects' particles are read as runs, and the sea's corners are worked out
  once a frame. Room for the busy moments, and for a Vita without the fourth core.
- **Screen-filling effects at a lower resolution** (auras, beams, flashes covering at least two screens' worth): drawn
  at half the size each way and added to the picture, as TwinsanityEvolved does with its particles.
- With the CapUnlocker plugin, the fourth CPU core also works out the image uploads' numbers, and vitaGL leaves out
  graphics-chip state calls that change nothing.
- `log.txt` is kept under 1 MB: a longer one becomes `log-prev.txt` at the next start.

Fixed

- BT4: the map select shows the two fighters' portraits, faded toward the middle as on the PS2 (the PC version hides
  them, for its widened menus; on the Vita a depth test threw them away).
- **A disc image that is not all there is no longer used.** An `.iso` whose copy did not finish (its size looks
  right, but past some point its reads come back empty), or one cut at 4 GB by a FAT32 memory card, was used anyway:
  the sounds from its missing part were silent, and a fight that needed a fighter from it closed the game (BT4, as a
  player reported). The game now checks that an image can be read to its end and says at the start how far it can
  be read, and what to do.
- **A disc image one folder down is found.** An archive unpacked into `ux0:data/bt3/` or `ux0:data/bt3/bt4/` puts
  the image in a folder of its own (Team BT4's release: `PS2_NTSC/`); the game, BT4 and the start menu now look one
  folder down too (as manumastro's fork does).

## 1.2.2

Fixed

- **The game closing right at its start on some Vitas** (1.2 and 1.2.1: "An error has occurred", C2-12828-1, with
  nothing in the log after its first lines): the crash report the game set up through the kubridge plugin's exception
  handlers closed it with some versions of kubridge. The released program no longer sets it up (`crashreport=1` in
  `ux0:data/bt3/bt3.txt` does); the console's own crash dump still says where a crash happened.

## 1.2.1

Fixed

- Songs from music mods (the anime songs images, among others) played once and stopped instead of looping: their
  loop point is kept in an older version of the ADX header, which the port did not read. They loop through the
  fight now, as the disc's own songs do.
- BT4's disc image (or its unpacked files) was not found unless it was in `ux0:data/bt3/bt4/`. `ux0:bt3/bt4/`,
  `ux0:data/bt4/`, `ux0:bt4/` and BT3's own folder `ux0:data/bt3/` are looked in too now. The README notes that
  BT4's image, about 6 GB, does not fit on a FAT32 card as one file: copy the unpacked files there instead.
- BT4: the Outfit Select window was empty (the costume's name was drawn by the PC version's text overlay, which the
  Vita does not have). It shows the costume's number between the arrows (1/4) and its name under them, in the
  game's own lettering, centred in the window and narrowed to fit it.

For crash reports

- The log (`ux0:data/bt3/log.txt`) now names each step of the start and lists the plugins in taiHEN's `config.txt`,
  and until the game starts it is written line by line, so a start that crashes leaves a log that ends at the step
  it crashed in (some players' Vitas close 1.2 right at the start, with nothing in the log to say why).

## 1.2

New

- **The disc image as it is**: copy your `.iso` to `ux0:data/bt3/` and the game reads its files from it; no need to
  unpack it with 7-Zip any more (the unpacked files still work). The same for BT4 and the European disc.
- **Languages**: Options → Language/Sound → Language (the console's Sound page and its Change Voice row; Language is
  now the page's first row, and its picker shows its page, Page 1/3, at the top right) sets the text and the voices together:
  English, Japanese (voices), and with your own European disc (SLES-54945: its image or its `DATA/PZS3EU1.AFS` in
  `ux0:data/bt3/eu/`) Spanish, German, French and Italian menus, HUD and battle subtitles, with English or Japanese
  voices. Based on Matteo Santoro's work.
- **Budokai Tenkaichi 4** (experimental): Team BT4's B14 Rev2 through NauTaro's BT4decomp, from your own BT4 disc's
  image or files in `ux0:data/bt3/bt4/`; chosen in the start menu. See the README.
- **Sharper fights**: fights are drawn at 720 x 408 whenever the graphics chip has the time for it and drop to
  640 x 368 the moment a heavy scene needs it (Killzone: Mercenary's dynamic resolution); `fightres=640` in
  `bt3.txt` keeps them at 640 x 368.
- **Faster effects**: the outline, the glow and the colour-table passes do their arithmetic in half precision, as
  the game's own pixel shaders already did.
- **Shaders shipped ready-made** (the offline shader bank of Grarak's MH3 port): the package carries all of the
  game's shaders already compiled by the Vita's own compiler, so the game no longer compiles any at start, and
  `libshacccg.suprx` is no longer needed.
- **BT4 in Spanish**: with Team BT4's Spanish release in `ux0:data/bt3/bt4/es/` beside the English one, BT4's
  **Options → Vita Settings → Language** switches between them (the game starts again in the one chosen): the
  game's text and voices, and the port's own rows, guide lines and Multiplayer plates, in Spanish.
- **BT3's Options in Spanish**: with the European disc and Español chosen, the Options screen is the European
  disc's Spanish one, with Vita Settings and the Vita's rows in Spanish; the start menu and the versus menu's
  Multiplayer too. Both games' Spanish says "control" / "controles" for the discs' "mando" / "mandos" (the Options
  row and title Control, Ajustes del control, Control 2, Dos Controles, and the game's own text).
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
- **Multiplayer between two Vitas** (experimental: not yet tested on two real Vitas): the versus mode against another Vita, ad hoc (no router) or over the same
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
- **The Options screen's rows** are Vita Settings, Controls, Sound, Save/Load (BT3 and BT4); in BT3 the Sound row
  reads Language/Sound (Idioma/Sonido) and its page opens on Language.
- **Controls**: the Options screen's Controller row and its page's title read Controls, the page's Control Type row
  reads Controller Settings, in the disc's own lettering (BT3 and BT4), and Dende's lines for the two trade
  places to match.
- **Controller Settings → Custom reaches L1, L2, R1 and R2**: the PS2's page let only the four face buttons be chosen to
  change; now Up / Down go round each column, so every button can take any of the eight actions (the shoulder
  buttons' actions on the face buttons and the other way round), in BT3 and BT4. The default layout stays until
  changed.
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
- `bt3.txt` `fightres=720` keeps the fights at 720 x 408 whatever the scene (`fightres=640` at 640 x 368; by
  default the size follows the graphics chip's time, see Sharper fights).
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

- BT4: crashes in fights. The game's code counts on a call's arguments being worked out right to left, as the PS2's
  compiler did; BT3's build rewrites its calls so (port/tools/ptr32.py r2l), BT4's build did not (its sources lie in
  their own tree), so a ki blast's hit read its fighter before it was known, among others. BT4's sources now get the
  same rewrite, and its shifts and divisions the PS2's rules (port/vita/srcfix.py). The ki blast's record is also
  fetched in its own statement now (BT3 and BT4).
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
