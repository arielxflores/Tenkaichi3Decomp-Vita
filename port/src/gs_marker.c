/*
 * Markers for the PC renderer inside the game's display list. The game builds each frame's list first and the
 * renderer executes it afterwards, so "a native effect goes HERE" has to travel in the list: a one-register GIF
 * packet that writes the effect number to GS register 0x7F, which the real GS does not have. The front end
 * (port/src/gs/gs_core.c) hands it to the GPU back end; the software reference ignores it and keeps drawing the
 * PS2 passes. Called from the game's effect functions under `#ifdef PORT`.
 */
#include <stddef.h>
#include <stdint.h>

extern void Dma_AddData(void *src, int size);

void Port_GsMarker(int effect) {
    uint32_t pkt[12] = {
        0x10000000u | 2, 0, 0x10000000u, 0x50000000u | 2, /* DMA CNT, 2 quadwords; VIF FLUSHE, DIRECT 2 */
        0x8000u | 1, 0x10000000u, 0xE, 0,                  /* GIF tag: 1 loop, EOP, one register: A+D */
        (uint32_t)effect, 0, 0x7F, 0,                      /* value, register number */
    };

    Dma_AddData(pkt, sizeof(pkt));
}

/* (Vita) A marker with a 32-bit value of its own (in the register's upper word): gs_core.c hands it over in
   gGsMarkerArg before the marker itself. */
void Port_GsMarkerArg(int effect, unsigned arg) {
    uint32_t pkt[12] = {
        0x10000000u | 2, 0, 0x10000000u, 0x50000000u | 2,
        0x8000u | 1, 0x10000000u, 0xE, 0,
        (uint32_t)effect, (uint32_t)arg, 0x7F, 0,
    };

    Dma_AddData(pkt, sizeof(pkt));
}

#ifdef PORT_VITA
/* ---- The engine seam (bt3.txt native=1; docs/vita/native.md): the first step of a renderer that draws from the
   engine's own data. The meshes of the stage (VU1 programs 4 and 7, src/battle/stgm_a.c; 8, the animated objects,
   src/sys/vu1_packet.c Vu1Node_Draw) and the parts of the objects' models (programs 0, 1, 2, the fighters,
   src/sys/vu1_packet.c Vu1Pkt_CallProg0..2) are static data: the call packet that runs one carries, in place of its
   last NOP, a VIF MARK: the seam's flag (0x8000), the kind (0x4000: a model's, else the stage's) and the generation of
   that kind in its low 12 bits (for a model's, 0x3000 the program, 0, 1 or 2: a part is called with program 0 and with
   program 1, the fade pass, and makes other draws with each). The list reader (gs_core.c es_find) then keeps that call in a table of its own, by its address and the
   generation, instead of the cache of calls, which finds parts by their address alone and has to guess when to give a
   place up and when to read a part again to see that it is the same.
   The stage's generation changes whenever a stage may have been loaded: a frame in which the stage is not drawn (a
   load job, or the stage not ready: Port_EsStageBusy) or another model. The models' whenever an object is bound to
   its model files (src/sys/gfxm_e_b.c BtlObjMdl_BindFiles: the first bind writes the model's chains). The ground
   shadows (program 6) are not named: their chains are built every frame. MARK does nothing on the PS2's VIF but set
   a register nobody reads. ---- */
extern void *Dma_Alloc(int size);
extern uint32_t *Vu1Pkt_CallProg7(uint32_t chain, float w);
extern int gVitaAbSide; /* (port/src/headless.c: the soak's A / B side) */
int gVitaNative = 1;    /* bt3.txt native: 0 off, 1 on (default), 2 in the soak's B battles */
static void *sEsMdl;
static int sEsBusy = 1;
static unsigned sEsGen, sEsGenM = 1;
/* ---- The textures named by the game (bt3.txt nativetex): an upload of a texture of the stage's texture file or of a
   fighter's carries, in its BITBLTBUF packet's VIF NOP, a MARK: the seam's flag (0x8000), 0x3000 (no named call has
   it: a model part's program is 0, 1 or 2) and the generation of the stage's textures, or with 0x4000 that of the
   models (sEsGenM). The list reader (gs_core.c texid_hash) then takes the upload's number from what it worked out
   the first time that place and length came with that generation, instead of reading the bytes again, and reads
   them again every gVitaTexCheck-th time to see that they are the same. The stage's textures' generation changes
   whenever the stage may have been loaded (Port_EsStageBusy) or its texture file is another. ---- */
int gVitaNativeTex = 1; /* bt3.txt nativetex: 0 off, 1 on (default), 2 in the soak's B battles */
static void *sTexFile;
static int sTexBusy = 1, sTexScope;
static unsigned sTexGen;
static int tex_on(void) {
    return gVitaNativeTex == 2 ? gVitaAbSide == 1 : gVitaNativeTex != 0;
}
/* (src/battle/stgm_a.c) the uploads of the stage's texture file `file` begin / end */
void Port_EsTexStage(void *file) {
    if (sTexBusy || file != sTexFile) {
        sTexBusy = 0;
        sTexFile = file;
        sTexGen = (sTexGen + 1) & 0xFFF;
        if (sTexGen == 0) {
            sTexGen = 1;
        }
    }
    sTexScope = 1;
}
void Port_EsTexStageEnd(void) {
    sTexScope = 0;
}
/* (src/sys/gfxm_b_b.c) the VIF word for the BITBLTBUF packet of an upload: the MARK inside the stage's scope, else 0 (NOP) */
uint32_t Port_EsTexWord(void) {
    return sTexScope && tex_on() ? 0x07000000u | 0x8000u | 0x3000u | sTexGen : 0;
}
/* (src/sys/gfxm_d_c.c) the same for a fighter's textures */
uint32_t Port_EsTexModelWord(void) {
    return tex_on() ? 0x07000000u | 0x8000u | 0x4000u | 0x3000u | sEsGenM : 0;
}
unsigned gVitaEsGens[2]; /* for the log: the stage's generations begun, the models' */

void Port_EsStageBusy(void) {
    sEsBusy = 1;
    sTexBusy = 1;
}
void Port_EsModelBound(void) {
    sEsGenM = (sEsGenM + 1) & 0xFFF;
    if (sEsGenM == 0) {
        sEsGenM = 1;
    }
    gVitaEsGens[1]++;
}
static int es_on(void) {
    return gVitaNative == 2 ? gVitaAbSide == 1 : gVitaNative != 0;
}
/* the stage's MARK (mdl: its model, NULL: the one of the stage's meshes drawn before in this picture) */
static uint32_t es_mark(void *mdl) {
    if ((mdl != NULL && mdl != sEsMdl) || sEsBusy) {
        sEsMdl = mdl != NULL ? mdl : sEsMdl;
        sEsBusy = 0;
        sEsGen = (sEsGen + 1) & 0xFFF;
        if (sEsGen == 0) {
            sEsGen = 1;
        }
        gVitaEsGens[0]++;
    }
    return 0x07000000u | 0x8000u | sEsGen; /* VIF MARK */
}
/* Vu1Pkt_CallProg4's packet with the seam's MARK in its data, after MSCALF / BASE / OFFSET (read before the call) */
void Port_EsStg4(void *mdl, uint32_t chain) {
    uint32_t *p = (uint32_t *)Dma_Alloc(0x20);
    p[0] = 0x50000000u | 1;        /* DMA CALL, one quadword */
    p[1] = chain;                  /* (the pointer itself, as VU1_ADDR_MASK under PORT) */
    p[2] = 0x10000000u;            /* FLUSHE */
    p[3] = 0;                      /* NOP */
    p[4] = 0x15000000u;            /* MSCALF 0 */
    p[5] = 0x03000000u | 0x8D;     /* BASE */
    p[6] = 0x02000000u | 0x1B4;    /* OFFSET */
    p[7] = es_on() ? es_mark(mdl) : 0;
}
/* Vu1Pkt_CallProg7's: its last word (a NOP after BASE / OFFSET) */
void Port_EsStg7(void *mdl, uint32_t chain, float w) {
    uint32_t *p = Vu1Pkt_CallProg7(chain, w);
    if (es_on()) {
        p[0x3B] = es_mark(mdl);
    }
}
/* (src/sys/vu1_packet.c) the last word of a call packet that is a NOP: the seam's MARK of a stage's animated object
   (prog -1: program 8) or of a model's part (prog 0, 1, 2) */
void Port_EsMarkCall(uint32_t *last, int prog) {
    if (es_on()) {
        *last = prog < 0 ? es_mark(NULL) : 0x07000000u | 0x8000u | 0x4000u | (uint32_t)(prog & 3) << 12 | sEsGenM;
    }
}
#endif
