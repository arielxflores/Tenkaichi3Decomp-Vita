# Towards a native renderer: the engine seam

The Vita's renderer is the PC port's GS front end with a back end on vitaGL: the game builds the PS2's display list
(DMA, VIF, GIF) every frame, the list reader (`port/src/gs/gs_core.c`) reads it on a core of its own, and the drawing
thread (`port/vita/vita_vgl.c`) turns what it finds into draws. For the 3D models it already draws natively in
most respects: the VU1 programs 0, 4 and 6 are vertex shaders, the models' vertices stay on the chip ("kept parts"),
and the outline, the glow and the depth tint are passes of their own. What is not native is how the renderer learns
**what** is being drawn: it finds it again in the list, frame after frame, by the address of each model call (the
cache of calls, `cc_find`), and it has to guess when a call it kept is still the same (read again every 64th call) and
when to give its place to another (the causes of the slow detailed stages fixed in November 2026).

The native ports of other PS2 games draw from the engine's own data instead, at the point where the game hands its
geometry to VU1 (the "engine seam"), and keep reading the GIF stream for 2D only:

| Port | How it draws |
|---|---|
| BT3-Recomp (z3xox, this game) | "Native Vulkan (engine seam)": host meshes and GPU vertex programs where the game hands geometry to VU1, native post-processing, GIF only for the 2D layer; the GS emulation (paraLLEl-GS) kept beside it for comparison |
| Celosia (Shadow of the Colossus, Vita) | A native renderer that reads the models' data; the GS emulation kept as a reference (F9) |
| OpenGOAL (Jak) | The DMA chain split into buckets, one C++ renderer per bucket, level geometry converted offline |
| rac1-native-game (Ratchet & Clank) | "Not drawn by emulating the PS2's graphics chip": the renderer reads what the game decided to draw |
| TwinsanityEvolved (Crash Twinsanity, Vita) | The decompilation's platform layer is the seam; meshes, skins and particles drawn by the engine |

## The first step (bt3.txt `native=1`)

The stage's meshes are named by the game. `StgModel_Draw` (`src/battle/stgm_a.c`, under `#ifdef PORT_VITA`) calls
`Port_EsStg4` / `Port_EsStg7` (`port/src/gs_marker.c`) for the plain groups (VU1 program 4) and the debris
(program 7). They send the same packets as `Vu1Pkt_CallProg4` / `7`, with a VIF MARK in place of the packet's last
NOP: the seam's flag (0x8000) and the stage's generation. The generation changes whenever a stage may have been
loaded (a frame in which the stage is not drawn: its load job, or the stage not ready; or another model).

The list reader (`es_find`) keeps such a call in a table of its own for the stage's life, by its address and the
generation: no place is given up to another call, no part is found again by the cache's guess, and the mesh is read
the long way only every `nativecheck`-th call (default 512, against the cache's 64; 0 never), in case the game writes
into a mesh. Everything else (the fighters, the effects, the 2D) goes the way it went. The log's
`engine seam` line counts the meshes sent from their copies, read the long way, kept, sent to the cache when the
table was full, the tables let go and the generations.

`native=2` with `soakab=1` sets the seam against the cache stage by stage.

## Next steps

In the order of what they are likely to save; each behind a setting, with the way it replaces kept for comparison.

1. **The stage's textures.** `StgModel_SetTexture` uploads the material's texture (`TexFile_UploadOne`) every time a
   material is drawn, every frame, and the renderer finds the texture again by a number worked out from the uploaded
   bytes (the fourth core's helper). Named at the seam by (texture file, index, generation), the number is known
   without reading the bytes.
2. **The fighters** (programs 0, 1, 2). The same seam in `gfxm_d_c.c` (the parts, their bones and light), with a
   generation of the fighter's model (transformations and tag changes load another model into the same memory).
   The constants (bones) come in the call packet already.
3. **The calls sent straight to the drawing thread.** Once a mesh is named and kept, the list reader has nothing to
   read but the call packet's constants: a side queue from the game thread to the drawing thread, in the list's order
   through a marker, leaves the reader out for the 3D models.
4. **Instanced draws.** Stages repeat meshes (railings, rocks, trees); TwinsanityEvolved draws a repeated mesh as one
   instanced draw on the Vita.
5. **The 2D and the effects** stay on the GS front end: they are many small draws whose state is the GS's.
