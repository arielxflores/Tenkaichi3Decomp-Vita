# port/vita

The PS Vita version of the port. Everything in this folder exists only for the Vita; the game's code and the rest
of `port/` are shared with the PC builds, and most of the Vita's changes to them are inside `#ifdef PORT_VITA`.

- Building: [docs/vita/building.md](../../docs/vita/building.md)
- The development build, its settings and a map of the files here: [docs/vita/development.md](../../docs/vita/development.md)
- Installing and playing: the [README](../../README.md)

How the Vita differs from the PC builds, in short:

| Difference | What the port does |
|---|---|
| 32-bit ARM, 4-byte pointers | The PS2's own pointer size: built like the PC port's 32-bit variant, none of the 64-bit pointer rewriting |
| Plain `char` unsigned, small enums in the ARM EABI | `-fsigned-char -fno-short-enums -fsigned-bitfields` |
| gcc for ARM evaluates call arguments left to right; the PS2 compiler right to left | The game's preprocessed text is rewritten as in the 64-bit PC build (`port/tools/ptr32.py`) |
| A shift by 32 or more gives 0 on ARM; the PS2 uses the low 5 bits of the count | Every shift by a variable count gets the PS2's rule written out (`srcfix.py`) |
| The game's code runs on the float unit, which has infinities and NaNs; the PS2's has neither (x / 0 is its largest number) | Every float division of the game's text goes through `__port_fdiv` (`srcfix.py`), the maths library hands back the largest number, and the two loops known to run for ever on such a number are guarded |
| A program cannot choose its addresses | The game heap is one block from the system, the hardware registers are arrays (`PORT_HW`), and the data tables are found through a generated table of addresses (`vita_mem.c`) |
| 68,611 loose files are impractical on a memory card | The disc's three AFS archives are read as they are (`vita_file.c`) |
| No SDL GPU back end on the Vita | A renderer on vitaGL (`vita_vgl.c`) behind the PC's GS front end, and the dozen SDL3 audio calls the PC's sound code uses, on the Vita's audio output (`sdlshim/`, `vita_audio.c`) |
| One controller (two on a PS TV), no L2 / R2 / L3 / R3, touch panels | `vita_gfx.c` and `vita_touch.c`: L2 / R2 / L3 / R3 on the rear pad's quarters (or the front screen's corners), the menus by touch; a second DualShock as player 2 |
