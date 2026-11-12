# Towards a native renderer: the engine seam

State of the work, how to test it and the rules for going on: [status.md](status.md).

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

## Where it stands (2026-11-12)

On by default since the console runs of 2026-11-12 ([status.md](status.md)): `native` (the seam below), `nativetex`
and `updest` (the textures named by the game, and each upload's destination remembering what it left), `dpfast`
(kept parts drawn with what does not change worked out once), `fxrun` (the effects' quad particles read as runs).
Measured and left: the stage's same-state groups as one draw (the copying cost more than the draws), each kept
group remembering its texture (never found the same), the sea of Kame House in a vertex shader (its work is on the
game thread, which has room: 35-40% busy). Next: textures bound by their names without GS memory's emulation in
between (the ~30,000 texture lookups every two seconds), then the calls sent straight to the drawing thread.

## The steps after the first ones (2026-11-12)

Measured on the console with `port/vita/tools/settings/native_next_ab.txt` (each with the value 2: the soak's B
battles have them, its A battles not; [status.md](status.md)). `texbind` and `seavtx` on by default, `esdirect` off
(no gain measured), `dpjoin` removed:

| Setting | What | Where |
|---|---|---|
| `texbind` | Textures bound by their names: the lookups (~30,000 every two seconds) without the pages' numbers; a destination is unbound at another's write into its blocks (`bind_touch`), a texture remembers the destinations it lies in (`sBind1`), the renderer its texture by their names (`sBind2`); checked the long way every 64th time (the log's `wrong`) | `gs_core.c` `Gs_TexNames`, `vita_vgl.c` `texture_get_k` |
| `esdirect` | The seam's call packets of the models' parts done as what they are, not code by code: the list reader's part of the calls sent straight on (what is left of it: the calls are already sent from their kept copies) | `gs_core.c` `es_direct` |
| `seavtx` | The sea's corners tested and projected once a frame each (a mesh's corners numbered once: the same place, texture place and colour), not once for each of the up to six triangles that have them; the per-corner work of a vertex shader, on the game's thread, in the ordering table's order (a shader on the chip would draw the sea apart from the transparent effects the table puts among its triangles) | `src/battle/eft_c.c` `sea_tri` |
| (`dpjoin`, removed) | The kept groups of one state and the same constants whose indices follow one another as one draw, without copying: tried on the console (2026-11-12), never one such pair (groups of the same constants lie apart on the chip) | |

## What `native=1` does now: the engine seam

The game names its static geometry to the renderer at the point where it hands it to VU1:

| What | VU1 program | Where it is named | Its generation changes when |
|---|---|---|---|
| The stage's meshes | 4 (plain groups), 7 (debris) | `src/battle/stgm_a.c` (`Port_EsStg4` / `Port_EsStg7`) | a frame the stage is not drawn in (its load job, or not ready), or another stage model |
| The stage's animated objects | 8 | `src/sys/vu1_packet.c` `Vu1Pkt_CallProg8` | as the stage's |
| The objects' model parts (the fighters) | 0 (lit), 1 (fade pass), 2 (flat: shadow, silhouette) | `src/sys/vu1_packet.c` `Vu1Pkt_CallProg0..2` | an object is bound to its model files (`BtlObjMdl_BindFiles`, `src/sys/gfxm_e_b.c`: the first bind writes the chains) |

Not named: the ground shadows (program 6), whose chains the game builds every frame.

Each named call packet carries, in place of its last NOP, a VIF MARK (`port/src/gs_marker.c`): the seam's flag
(0x8000), the kind (0x4000: a model's), for a model's part the program (0x3000: a part is called with program 0 and
with program 1, which make other draws) and the generation of that kind (12 bits). MARK does nothing on the PS2.

The list reader (`es_find`, `port/src/gs/gs_core.c`) keeps such a call in a table of its own, found by its address
(and program), instead of the cache of calls (`cc_find`), which finds parts by their address alone and has to guess
when to give a place up and when to read a part again to see that it is the same:

- an entry whose generation is not the call's is read again (the memory holds another stage or model now);
- an entry not called for 300 lists may be taken by another call; no other entry is ever given up;
- a kept call is read again the long way only every `nativecheck`-th call (default 512; the cache's every 64th; 0
  never), in case the game writes into a mesh.

The kept copies are the cache's own records, sent the same way (`cc_end`, `cc_replay`, and the drawing thread's kept
parts): only how a call is found and how long it is trusted change. The log's `engine seam` line counts the calls sent
from their copies, read the long way, kept, sent to the cache when the table was full, renewed for a new generation,
places taken over, and the generations begun.

`native=2` with `soakab=1` sets the seam against the cache stage by stage.

## Next steps

Each behind `native`, with the way it replaces kept for comparison, and each measured on a console before the next.

1. **The sea of Kame House** (`EftSurf_DrawMeshes`, `src/battle/eft_c.c`). About 900 triangles a frame are set up on
   the game thread (their reflection, fog and fade worked out with the vector unit) and sent twice as single GS
   triangles, each with its own PRIM, TEX0 and palette: about 1,800 primitives the list reader and the drawing thread
   go through one by one. Natively: the surface's mesh (static data of the stage pack) on the chip once, the per-vertex
   work in a vertex shader, and its two layers as two draws. To be measured first (`perfsum.py --parts` on that stage):
   if the chip is what is slow there, the reflection's pixels are what to cut instead.
2. **The calls sent straight to the drawing thread.** A named call that is kept leaves the list reader only its
   packet's constants to read: a side queue from the game thread to the drawing thread, in the list's order through a
   marker, leaves the reader out of the 3D models.
3. **The textures.** The stage's materials (`StgModel_SetTexture`) and the fighters' textures are uploaded every
   frame, and the renderer finds them by a number worked out from a sample of their bytes (on the fourth core with
   CapUnlocker). Naming them at the seam saves that work, but only if the renderer can never take a texture for one
   that is no longer in that memory: less to gain than it looked, and a risk; after the others.
4. **The 2D and the effects** stay on the GS front end: many small draws whose state is the GS's. The screen-filling
   effects are drawn at a lower resolution already (`fxlow`, outside the seam).
