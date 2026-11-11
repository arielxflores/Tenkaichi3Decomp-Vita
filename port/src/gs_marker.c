/*
 * Markers for the PC renderer inside the game's display list. The game builds each frame's list first and the
 * renderer executes it afterwards, so "a native effect goes HERE" has to travel in the list: a one-register GIF
 * packet that writes the effect number to GS register 0x7F, which the real GS does not have. The front end
 * (port/src/gs/gs_core.c) hands it to the GPU back end; the software reference ignores it and keeps drawing the
 * PS2 passes. Called from the game's effect functions under `#ifdef PORT`.
 */
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
   engine's own data. The stage's meshes (VU1 programs 4 and 7, src/battle/stgm_a.c) are the stage file's static
   data: the call packet that runs one carries, in place of its last NOP, a VIF MARK with the seam's flag (0x8000)
   and the stage's generation. The list reader (gs_core.c es_find) then keeps that call for the stage's life in a
   table of its own, by its address and the generation, instead of the cache of calls, which finds parts by their
   address alone and has to guess when to give a place up and when to read a part again to see that it is the same.
   The generation changes whenever a stage may have been loaded: a frame in which the stage is not drawn (a load
   job, or the stage not ready: Port_EsStageBusy) or another model. MARK does nothing on the PS2's VIF but set a
   register nobody reads. ---- */
extern void *Dma_Alloc(int size);
extern uint32_t *Vu1Pkt_CallProg7(uint32_t chain, float w);
extern int gVitaAbSide; /* (port/src/headless.c: the soak's A / B side) */
int gVitaNative = 0;    /* bt3.txt native: 0 off, 1 on, 2 in the soak's B battles */
static void *sEsMdl;
static int sEsBusy = 1;
static unsigned sEsGen;
unsigned gVitaEsGens; /* for the log: the stage generations begun */

void Port_EsStageBusy(void) {
    sEsBusy = 1;
}
static uint32_t es_mark(void *mdl) {
    if (mdl != sEsMdl || sEsBusy) {
        sEsMdl = mdl;
        sEsBusy = 0;
        sEsGen = (sEsGen + 1) & 0x7FFF;
        if (sEsGen == 0) {
            sEsGen = 1;
        }
        gVitaEsGens++;
    }
    return 0x07000000u | 0x8000u | sEsGen; /* VIF MARK */
}
static int es_on(void) {
    return gVitaNative == 2 ? gVitaAbSide == 1 : gVitaNative != 0;
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
#endif
