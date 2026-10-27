/*
 * Graphics Synthesizer, reference implementation in software.
 *
 * The game's drawing code runs unchanged and produces what it produced on the PS2: one DMA chain per frame on the
 * VIF1 channel, carrying GIF packets (register writes, primitives, texture uploads) for the GS and data for the
 * VU1 vertex programs. This file interprets the chain and draws the GS part into an ordinary RGBA image:
 *     DMA chain  ->  VIF1 commands  ->  GIF packets  ->  GS registers and vertices  ->  rasteriser.
 * It is the readable, dependency-free reference: screenshots for tests, and the specification by example for the
 * GPU renderer, which will take over from the "GS registers and vertices" stage.
 *
 * Not here yet: the VU1 vertex programs (3D models: their data is skipped), reading the frame buffer back as a
 * texture, mip-mapping, fog, dithering, bilinear filtering.
 * GS memory is emulated with its real layout (4 MB, pages / blocks / columns per pixel format): the game uploads
 * 4-bit and 8-bit textures as 32-bit images whose bytes are already arranged for the indexed formats, and it
 * draws into memory that it later uses as a texture, so uploads, textures, palettes and frame buffers must all
 * address the same memory the same way.
 *
 * Compiled with the host's float unit (this is not simulation code). BT3_GS=1 (or any value) enables the
 * software rasteriser; BT3_GS=gpu sends the primitives to the GPU back end (gs_gpu.c) and opens a window.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef PORT_VITA /* the render side's own copy of "this frame is not drawn" (set when a list is handed over) */
#define gVitaGsSkip gVitaGsSkipR
#endif
#include <pthread.h>
#include <string.h>
#include <time.h>
#include "gs_internal.h"

#define SURF_W 1024
#define SURF_H 1024

/* ------------------------------------------------------------------------------------------------ GS state */

typedef struct Target {
    uint32_t fbp;
    uint32_t *z;    /* depth, one word per pixel (kept outside GS memory) */
    unsigned drawn; /* pixels written since the last frame end */
} Target;

GsState gGs;
uint32_t gGsPageGen[512];
uint32_t gGsPageSig[512]; /* (the renderer's: bumped with gGsPageGen, and also by the uploads that do not count for gGsPageGen) */
unsigned gGsFbUploads; /* bit 0 / 1: pixels were uploaded into display buffer 0 / 0x70 since the back end last looked */
unsigned gGsFrame;
#define gs gGs
#define Vertex GsVertex
#define sFrame gGsFrame
static int sGpu; /* 1: the GPU back end draws (BT3_GS=gpu), 0: the software rasteriser below */

static Target sTargets[16];
static int sTargetCount;
static unsigned sMissingTex;
static unsigned sPix[5]; /* per frame: outside scissor, failed alpha test, failed depth test, colour written, depth written */
static int sDrawFrom;       /* BT3_GS_FROM=<frame>: draw only from this frame on (the reference is slow) */
static int sProbeX = -1, sProbeY = -1;
static int sDumpFrame = -1; /* BT3_GS_DUMP=<frame>: list that frame's primitives */
static unsigned sStat[8]; /* per frame: DMA tags, VIF codes, DIRECT quadwords, GIF tags, register writes, vertices, image quadwords, unknown VIF */

/* ---- GS local memory: 4 MB. A buffer pointer counts 256-byte blocks, a buffer width counts 64 pixels. Where a
   pixel lives depends on its format: pages of 32 blocks, blocks in a format-specific order inside the page,
   "columns" inside the block. The tables are the hardware's; the two indexed ones are generated from their first
   column (odd columns exchange the two halves of every group of eight). ---- */
static uint32_t sVram[1 << 20];
static const uint8_t kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t kBlock16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const uint8_t kBlock16S[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27}, {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
static const uint8_t kCol32[8][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}, {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
                                     {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47}, {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
static const uint8_t kCol16[8][16] = {
    {0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27}, {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31},
    {32, 34, 40, 42, 48, 50, 56, 58, 33, 35, 41, 43, 49, 51, 57, 59}, {36, 38, 44, 46, 52, 54, 60, 62, 37, 39, 45, 47, 53, 55, 61, 63},
    {64, 66, 72, 74, 80, 82, 88, 90, 65, 67, 73, 75, 81, 83, 89, 91}, {68, 70, 76, 78, 84, 86, 92, 94, 69, 71, 77, 79, 85, 87, 93, 95},
    {96, 98, 104, 106, 112, 114, 120, 122, 97, 99, 105, 107, 113, 115, 121, 123}, {100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127}};
static const uint8_t kCol8First[4][16] = {{0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54}, {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
                                          {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23}, {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31}};
static const uint8_t kCol4First[4][32] = {
    {0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110},
    {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
    {65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47},
    {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63}};
static uint8_t sCol8[16][16];
static uint16_t sCol4[16][32];

static void tables_init(void) {
    int y, x;
    for (y = 0; y < 16; y++) {
        int col = y >> 2;
        for (x = 0; x < 16; x++) {
            int sx = (col & 1) ? (x ^ 4) : x;
            sCol8[y][x] = (uint8_t)(kCol8First[y & 3][sx] + col * 64);
        }
        for (x = 0; x < 32; x++) {
            int sx = (col & 1) ? (x ^ 4) : x;
            sCol4[y][x] = (uint16_t)(kCol4First[y & 3][sx] + col * 128);
        }
    }
}

#ifdef PORT_VITA
/* ---- PS Vita: uploads by the block.
   A 256-byte block holds 8 x 8 pixels of 32 bits, 16 x 8 of 16, 16 x 16 of 8 or 32 x 16 of 4. An upload whose
   rectangle is made of whole blocks (nearly every texture) is a permutation of its bytes inside each block, which
   costs a fraction of one address computation per pixel. ---- */
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

/* The pixels of one block in the layout of format psm. */
static void psm_block(uint32_t psm, uint32_t *w, uint32_t *h) {
    if ((psm & 0x30) == 0x30) {
        psm &= 0x0F;
    }
    switch (psm) {
    case 0x02: case 0x0A: *w = 16; *h = 8; break;
    case 0x13: *w = 16; *h = 16; break;
    case 0x14: *w = 32; *h = 16; break;
    default: *w = 8; *h = 8; break; /* 32 bits and the formats inside a 32-bit pixel */
    }
}

/* Writes block b whole from rows of source pixels `stride` bytes apart (bits: 32, 16, 8, 4). */
static void block_write(uint32_t b, int bits, const uint8_t *src, uint32_t stride) {
    uint32_t *d = &sVram[(b & 0x3FFF) * 64], x, y;
    uint8_t *d8 = (uint8_t *)d;
    uint16_t *d16 = (uint16_t *)d;

    switch (bits) {
    case 32:
        for (y = 0; y < 8; y++, src += stride) {
            for (x = 0; x < 8; x++) {
                memcpy(&d[kCol32[y][x]], src + x * 4, 4);
            }
        }
        break;
    case 16:
        for (y = 0; y < 8; y++, src += stride) {
            for (x = 0; x < 16; x++) {
                d16[kCol16[y][x]] = (uint16_t)(src[x * 2] | src[x * 2 + 1] << 8);
            }
        }
        break;
    case 8:
        for (y = 0; y < 16; y++, src += stride) {
            for (x = 0; x < 16; x++) {
                d8[sCol8[y][x]] = src[x];
            }
        }
        break;
    default:
        memset(d, 0, 256);
        for (y = 0; y < 16; y++, src += stride) {
            for (x = 0; x < 32; x++) {
                uint32_t v = (src[x >> 1] >> ((x & 1) * 4)) & 15, idx = sCol4[y][x];
                d8[idx >> 1] |= (uint8_t)(v << ((idx & 1) * 4));
            }
        }
        break;
    }
}

uint64_t gGsMipTbp[4];   /* MIPTBP1_1, MIPTBP1_2, MIPTBP2_1, MIPTBP2_2 as last written */
unsigned gGsMipWrites;
unsigned gVitaUsUpload;  /* per log interval: time in the larger uploads, */
unsigned gVitaUp[5];     /* pixels written one by one, written by the block, not written (below), calls, film blocks */
#endif

#ifdef PORT_VITA
/* ---- PS Vita with the GPU renderer: uploads that are not carried out.
   The game sends every texture again in every frame, several of them to the same place one after the other, and
   in the menus that is most of a frame's work. The renderer does not need the pixels in GS memory to recognise a
   texture it has already made, only to make a new one. So an upload of whole blocks is taken note of instead of
   written:
     - its bytes are kept, once, under a 64-bit number computed from them (sUp: found again by that number when
       the game sends the same bytes the next time, which costs reading them once);
     - every block it covers remembers where its bytes are (sBlk[].up, off) and gets an id made from that number
       and the block's place in the upload. The same upload gives the same ids every time.
   The renderer's texture key is made of the ids of the blocks a texture occupies (Gs_RectId) and so does not
   change while the game keeps sending the same data. Whoever reads or changes single pixels of a block
   (vram_rw: a new texture being decoded, a palette, an upload that is not made of whole blocks) has the block
   written first (blk_real). A block written pixel by pixel takes its id from its content when asked (dirty).
   gGsEpoch counts every change, so that a key need not be made twice for the same state. ---- */
typedef struct Up { uint64_t hash; uint32_t len, seen; uint8_t *data; } Up;
typedef struct Blk { uint32_t id, off; uint16_t up, stride; uint8_t bits, dirty; } Blk;
#ifndef UP_MAX
#define UP_MAX 3072
#define UP_BYTES_MAX (16u << 20)
#endif
#define UP_INDEX 8192                 /* hash table over sUp: entry + 1, open addressing */
static Up sUp[UP_MAX];
static uint16_t sUpIndex[UP_INDEX], sUpFree[UP_MAX];
static uint32_t sUpFreeCount, sUpBytes;
static Blk sBlk[16384];
static int sLazy;                     /* 0: every upload is written at once */
unsigned gGsEpoch;
unsigned gVitaUpStore[3];             /* for the log: uploads kept, their bytes, sweeps */

static void blk_real(uint32_t b) {
    Blk *k = &sBlk[b];
    uint32_t up = k->up;

    if (up != 0) {
        k->up = 0;
        block_write(b, k->bits, sUp[up - 1].data + k->off, k->stride);
    }
}

static uint32_t blk_id(uint32_t b) {
    Blk *k = &sBlk[b];

    if (k->dirty) {
        const uint32_t *w = &sVram[b * 64];
        uint32_t h = 2166136261u, i;
        for (i = 0; i < 64; i++) {
            h = (h ^ w[i]) * 16777619u;
            h ^= h >> 15;
        }
        k->id = h;
        k->dirty = 0;
    }
    return k->id;
}

/* The number of an upload's bytes (len is a multiple of 8 for whole blocks; the rest is taken byte by byte).
   Every step is one-to-one on the 64 bits, so two uploads that differ in one place never get the same number. */
static uint64_t up_hash(const uint8_t *p, uint32_t len) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ len, w;
    uint32_t i;

    for (i = 0; i + 8 <= len; i += 8) {
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B1u;
        h ^= h >> 32;
    }
    for (; i < len; i++) {
        h = (h ^ p[i]) * 0x9E3779B1u;
        h ^= h >> 32;
    }
    return h;
}

static void up_index_add(uint32_t e) {
    uint32_t h = (uint32_t)(sUp[e].hash >> 20) & (UP_INDEX - 1);
    while (sUpIndex[h] != 0) {
        h = (h + 1) & (UP_INDEX - 1);
    }
    sUpIndex[h] = (uint16_t)(e + 1);
}

/* Lets go of the uploads the game has not sent since frame `keep`; blocks that still wait for one of them are
   written first. */
static void up_sweep(uint32_t keep) {
    static uint8_t dead[UP_MAX];
    uint32_t e, b, n = 0;

    for (e = 0; e < UP_MAX; e++) {
        dead[e] = sUp[e].data != NULL && sUp[e].seen < keep;
        n += dead[e];
    }
    if (n == 0) {
        return;
    }
    for (b = 0; b < 16384; b++) {
        if (sBlk[b].up != 0 && dead[sBlk[b].up - 1]) {
            blk_real(b);
        }
    }
    memset(sUpIndex, 0, sizeof(sUpIndex));
    for (e = 0; e < UP_MAX; e++) {
        if (dead[e]) {
            sUpBytes -= sUp[e].len;
            free(sUp[e].data);
            sUp[e].data = NULL;
            sUpFree[sUpFreeCount++] = (uint16_t)e;
        } else if (sUp[e].data != NULL) {
            up_index_add(e);
        }
    }
    gVitaUpStore[2]++;
}

/* The entry that holds these bytes (a new one with a copy of them if need be), or -1: no room. */
/* The number of a large upload without reading all of it every frame: the main menu sends 3 MB of 32-bit pictures
   a frame, and reading them is most of that frame's time. The data the game sends from the same address, with the
   same length and the same bytes at every 252nd place, is taken for what it was the last time; every eighth frame
   it is read whole again, so a change the samples miss shows a few frames late at the worst. */
#ifndef UP_QUICK
#define UP_QUICK 1
#endif
/* the numbers worked out lately, and the re-reading budget of this frame: one set per thread that works them out
   (the drawing thread; the list reader ahead of it, below) */
extern int gGsSplit;
typedef struct UpQ { struct { uint32_t key, len, frame; const uint8_t *src; uint64_t hash; } q[1024]; uint32_t budgetFrame, budget; } UpQ;
/* bt3.txt upquick=0: every large upload's number from all of its bytes (no samples); upcheck=1: the
   number a sample found unchanged is checked against all of the bytes, and an upload whose bytes changed where the
   samples did not look is counted and logged (a texture the renderer would keep showing as it was) */
int gVitaUpQuick = 1, gVitaUpCheck;
unsigned gVitaUpDyn2[3]; /* (upcheck=1, the log) uploads from a copy (the queue), read in place, of them whose bytes changed */
unsigned gVitaUpMiss[4]; /* for the log: samples' numbers checked, of them wrong; the list reader's numbers checked, of them wrong */
static void up_miss_note(int kind, const uint8_t *src, uint32_t len, uint32_t age) {
    extern void Vita_Log(const char *fmt, ...);
    static unsigned lines;
    if (lines < 200) {
        lines++;
        Vita_Log("gs: upload of %u bytes from %p (to %04x, psm %02x, %u x %u at %u,%u) changed %s; list %u%s\n", len, (const void *)src,
                 (unsigned)((gs.bitbltbuf >> 32) & 0x3FFF), (unsigned)((gs.bitbltbuf >> 56) & 0x3F), (unsigned)gs.tw, (unsigned)gs.th,
                 (unsigned)gs.tx, (unsigned)gs.ty, kind == 0 ? "where its samples did not look" : "after the list reader worked out its number",
                 sFrame, kind == 0 ? (age < 8 ? " (sampled lately)" : " (re-reading put off)") : "");
    }
}
static UpQ sUpQBe;
static uint64_t sUpFull; /* the bytes' own number, worked out first */
unsigned gVitaUpDyn[2];  /* for the log: large uploads looked at, of them from a place whose bytes changed since its last upload */
static void up_dyn_note(const uint8_t *src, uint32_t len, uint64_t full) {
    extern void Vita_Log(const char *fmt, ...);
    static struct { const uint8_t *src; uint32_t len, frame; uint64_t full; } t[2048];
    static unsigned lines;
    uint32_t h = ((uint32_t)(uintptr_t)src * 0x9E3779B1u ^ len) >> 21;
    gVitaUpDyn[0]++;
    if (t[h].src == src && t[h].len == len && t[h].full != full) {
        gVitaUpDyn[1]++;
        if (lines < 150) {
            lines++;
            Vita_Log("gs: the %u bytes at %p (to %04x, psm %02x, %u x %u) are not as they were in list %u; list %u\n", len, (const void *)src,
                     (unsigned)((gs.bitbltbuf >> 32) & 0x3FFF), (unsigned)((gs.bitbltbuf >> 56) & 0x3F), (unsigned)gs.tw, (unsigned)gs.th, t[h].frame, sFrame);
        }
    }
    t[h].src = src;
    t[h].len = len;
    t[h].full = full;
    t[h].frame = sFrame;
}
static uint64_t up_hash_quick_q(UpQ *qs, const uint8_t *src, uint32_t len) {
    uint32_t k = (uint32_t)(uintptr_t)src * 0x9E3779B1u ^ len, i, w;
    __typeof__(&qs->q[0]) e;

    if (gVitaUpCheck && qs == &sUpQBe && len >= 4096) {
        sUpFull = up_hash(src, len);
        up_dyn_note(src, len, sUpFull);
    }
    if (!UP_QUICK || !gVitaUpQuick || len < 4096) { /* (it is the reading that costs: every byte comes from memory the game wrote, not from the cache) */
        return up_hash(src, len);
    }
    {
    /* (a large upload sampled more thinly: the samples cost a fresh cache line each, 40 cycles and more; what they
       miss, the whole is read for within a second or so: below) */
    uint32_t step = len > 65536 ? 1020 : 252;
    for (i = 0; i + 4 <= len; i += step) {
        __builtin_prefetch(src + i + step * 6); /* (each sample is a fresh cache line: several in flight at once) */
        memcpy(&w, src + i, 4);
        k = (k ^ w) * 0x85EBCA6Bu;
        k ^= k >> 15;
    }
    }
    e = &qs->q[(k >> 10) & 1023];
    if (e->key == k && e->src == src && e->len == len) {
        /* The samples are as they were: the whole is read again now and then, to be sure. Not all in the same
           frame (the textures first seen together came due together, every eighth frame: 10 MB read in one
           frame, a picture late): at most 384 KB of such re-reading a frame, the rest later, none later
           than 4 seconds. */
        uint32_t age = sFrame - e->frame;
        if (qs->budgetFrame != sFrame) {
            qs->budgetFrame = sFrame;
            qs->budget = 0;
        }
        if (age < 8 || (age < 120 && qs->budget + len > 393216)) {
            if (gVitaUpCheck && qs == &sUpQBe) {
                gVitaUpMiss[0]++;
                if (sUpFull != e->hash) {
                    gVitaUpMiss[1]++;
                    up_miss_note(0, src, len, age);
                }
            }
            return e->hash;
        }
        qs->budget += len;
    }
    e->key = k; e->src = src; e->len = len; e->frame = sFrame;
    e->hash = up_hash(src, len);
    return e->hash;
}
/* The uploads' numbers worked out ahead by the list reader (its core has time to spare, the drawing thread's has
   not: 1 to 2 million cycles a picture in the busy stretches of a fight). It reads the GIF data it hands
   over by reference, finds the image transfers in it and leaves their numbers here, marked with the list; the
   drawing thread takes a number when the place, the length and the list agree (a seqlock: the reader may be at the
   next list already), and works it out itself otherwise. */
typedef struct FeHash { volatile uint32_t seq; const uint8_t *p; uint32_t len; uint64_t hash; } FeHash;
static FeHash sFeHash[4096];
/* the slot of an upload's data: by all of its address. With the low bits alone (address / 16 mod 2048)
   every texture 32 KB apart from another took the same slot, and the textures of the effects lie at such addresses:
   in the busiest stretches the drawing thread worked out 1,400 to 2,500 numbers itself.
   And four slots for each place, one for each of four lists in turn: the list reader is a list ahead of
   the drawing thread when that one is slow, and the same texture sent from the same place in the next list took the
   slot before the drawing thread had read it (nearly all of what it still worked out was "another list's") */
static inline FeHash *fe_slot_seq(const uint8_t *d, uint32_t seq) {
    return &sFeHash[(((uint32_t)(uintptr_t)d * 0x9E3779B1u) >> 22 << 2) | (seq & 3)];
}
static UpQ sUpQFe;
static uint32_t sFeListSeq = 1;   /* the list reader: the list it reads (numbered from 1) */
static uint32_t sBeListSeq;       /* the drawing thread: the list it draws */
static uint32_t sFeGifLeft;       /* the list reader: quadwords of the GIF packet's data still to come */
unsigned gVitaFeMiss[4];
unsigned gVitaFeHash[3];          /* for the log: numbers worked out ahead, taken, worked out by the drawing thread */
static void fe_hash_gif(const uint8_t *p, uint32_t qwc, int keep) {
    uint32_t q = 0;
    if (sFeGifLeft >= qwc) {
        sFeGifLeft -= qwc;
        return;
    }
    q = sFeGifLeft;
    sFeGifLeft = 0;
    while (q < qwc) {
        uint64_t tag[2];
        uint32_t nloop, flg, nreg, dq;
        memcpy(tag, p + (size_t)q * 16, 16);
        nloop = (uint32_t)(tag[0] & 0x7FFF);
        flg = (uint32_t)((tag[0] >> 58) & 3);
        nreg = (uint32_t)((tag[0] >> 60) & 15);
        nreg = nreg ? nreg : 16;
        dq = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        q++;
        if (keep && flg >= 2 && nloop >= 128 && q + dq <= qwc) {
            const uint8_t *d = p + (size_t)q * 16;
            FeHash *e = fe_slot_seq(d, sFeListSeq);
            uint64_t h = up_hash_quick_q(&sUpQFe, d, nloop * 16);
            e->seq = 0;
            __sync_synchronize();
            e->p = d;
            e->len = nloop * 16;
            e->hash = h;
            __sync_synchronize();
            e->seq = sFeListSeq;
            gVitaFeHash[0]++;
        }
        if (q + dq > qwc) {
            sFeGifLeft = q + dq - qwc;
            return;
        }
        q += dq;
    }
}
/* the drawing thread: an upload's number (the list reader's if it has it) */
static uint64_t up_hash_quick(const uint8_t *src, uint32_t len) {
    if (gGsSplit && len >= 2048) {
        FeHash *e = fe_slot_seq(src, sBeListSeq);
        uint32_t s1 = e->seq, s2;
        const uint8_t *ep;
        uint32_t el;
        uint64_t h;
        __sync_synchronize();
        ep = e->p; el = e->len; h = e->hash;
        __sync_synchronize();
        s2 = e->seq;
        if (s1 != 0 && s1 == s2 && s1 == sBeListSeq && ep == src && el == len) {
            gVitaFeHash[1]++;
            if (gVitaUpCheck) { /* the bytes as the drawing thread finds them now */
                gVitaUpMiss[2]++;
                if (up_hash(src, len) != h) {
                    gVitaUpMiss[3]++;
                    up_miss_note(1, src, len, 0);
                }
            }
            return h;
        }
        gVitaFeHash[2]++;
        {   /* (for the log: why not: no number there, another list's, another upload's, another length) */
            extern unsigned gVitaFeMiss[4];
            gVitaFeMiss[s1 == 0 ? 0 : (s1 != s2 || s1 != sBeListSeq) ? 1 : ep != src ? 2 : 3]++;
        }
    }
    return up_hash_quick_q(&sUpQBe, src, len);
}

static int up_get(uint64_t hash, uint32_t len, const uint8_t *src) {
    static int ready;
    uint32_t h, e;
    int pass;

    if (!ready) {
        for (e = 0; e < UP_MAX; e++) {
            sUpFree[e] = (uint16_t)(UP_MAX - 1 - e);
        }
        sUpFreeCount = UP_MAX;
        ready = 1;
    }
    for (h = (uint32_t)(hash >> 20) & (UP_INDEX - 1); sUpIndex[h] != 0; h = (h + 1) & (UP_INDEX - 1)) {
        Up *u = &sUp[sUpIndex[h] - 1];
        if (u->hash == hash && u->len == len) {
            u->seen = sFrame;
            return (int)sUpIndex[h] - 1;
        }
    }
    for (pass = 0; pass < 2 && (sUpFreeCount == 0 || sUpBytes + len > UP_BYTES_MAX); pass++) {
        up_sweep(pass == 0 ? (sFrame > 120 ? sFrame - 120 : 0) : sFrame); /* two seconds unused; then all but this frame's */
    }
    if (sUpFreeCount == 0 || sUpBytes + len > UP_BYTES_MAX) {
        return -1;
    }
    e = sUpFree[sUpFreeCount - 1];
    sUp[e].data = malloc(len);
    if (sUp[e].data == NULL) {
        return -1;
    }
    sUpFreeCount--;
    memcpy(sUp[e].data, src, len);
    sUp[e].hash = hash;
    sUp[e].len = len;
    sUp[e].seen = sFrame;
    sUpBytes += len;
    up_index_add(e);
    gVitaUpStore[0] = UP_MAX - sUpFreeCount;
    gVitaUpStore[1] = sUpBytes;
    return (int)e;
}
/* Which bits of the 32-bit pixels of a block were written since: a texture in the top bits (PSMT8H,
   PSMT4HL, PSMT4HH) or the 24 below (PSMCT24) shares its blocks with another one in the other bits, and every
   upload of one of them changed the blocks' contents, so the other one, sent again unchanged, was taken for changed
   and written again pixel by pixel (the fighters' intros: 20,000 to 26,000 such pixels a picture). An
   upload of such a format is now found unchanged when nothing wrote ITS bits of those blocks since. */
static uint32_t sPlaneGen[16384][3]; /* per block: writes to bits 0..23, 24..27, 28..31 */
int gVitaPlanes = 1;                 /* (bt3.txt planes=0: as before, the whole contents compared) */
static inline int psm_planes(uint32_t psm) {
    if ((psm & 0x30) == 0x30) {
        psm &= 0x0F;
    }
    switch (psm) {
    case 0x01: return 1;
    case 0x1B: return 6;
    case 0x24: return 2;
    case 0x2C: return 4;
    default: return 7;
    }
}
static inline void plane_bump(uint32_t b, int planes) {
    b &= 0x3FFF;
    sPlaneGen[b][0] += (uint32_t)(planes & 1);
    sPlaneGen[b][1] += (uint32_t)(planes >> 1 & 1);
    sPlaneGen[b][2] += (uint32_t)(planes >> 2 & 1);
}
#define VRAM_TOUCH(n) do { uint32_t b_ = (n) & 0x3FFF; if (sBlk[b_].up != 0) { blk_real(b_); } if (write) { sBlk[b_].dirty = 1; plane_bump(b_, psm_planes(psm)); } } while (0)
#else
#define VRAM_TOUCH(n) ((void)0)
#endif

/* Reads (write = 0) or writes the pixel (x, y) of a buffer at bp, width bw, format psm. Value: the stored bits.
   (write = 2: which 256-byte block the pixel is in.) */
static uint32_t vram_rw(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, int write, uint32_t v) {
    uint32_t a, *w;

    uint32_t zx = 0; /* the depth formats (0x30..0x3A) are the colour formats with the blocks of a page in another order */

    if (bw == 0) {
        bw = 1;
    }
    if ((psm & 0x30) == 0x30) {
        zx = 0x18;
        psm &= 0x0F;
    }
    switch (psm) {
    case 0x02: case 0x0A: /* 16 bits */
        a = ((bp + ((y >> 1) & ~0x1Fu) * bw + ((x >> 1) & ~0x1Fu) +
              ((psm == 0x0A ? kBlock16S : kBlock16)[(y >> 3) & 7][(x >> 4) & 3] ^ zx)) << 7) + kCol16[y & 7][x & 15];
        if (write == 2) { return (a >> 7) & 0x3FFF; }
        VRAM_TOUCH(a >> 7);
        w = &sVram[(a >> 1) & 0xFFFFF];
        if (write) { *w = (*w & ~(0xFFFFu << ((a & 1) * 16))) | (v & 0xFFFF) << ((a & 1) * 16); return 0; }
        return (*w >> ((a & 1) * 16)) & 0xFFFF;
    case 0x13: /* 8 bits */
        a = ((bp + ((y >> 1) & ~0x1Fu) * (bw >> 1) + ((x >> 2) & ~0x1Fu) + kBlock32[(y >> 4) & 3][(x >> 4) & 7]) << 8) + sCol8[y & 15][x & 15];
        if (write == 2) { return (a >> 8) & 0x3FFF; }
        VRAM_TOUCH(a >> 8);
        w = &sVram[(a >> 2) & 0xFFFFF];
        if (write) { *w = (*w & ~(0xFFu << ((a & 3) * 8))) | (v & 0xFF) << ((a & 3) * 8); return 0; }
        return (*w >> ((a & 3) * 8)) & 0xFF;
    case 0x14: /* 4 bits */
        a = ((bp + ((y >> 2) & ~0x1Fu) * (bw >> 1) + ((x >> 2) & ~0x1Fu) + kBlock16[(y >> 4) & 7][(x >> 5) & 3]) << 9) + sCol4[y & 15][x & 31];
        if (write == 2) { return (a >> 9) & 0x3FFF; }
        VRAM_TOUCH(a >> 9);
        w = &sVram[(a >> 3) & 0xFFFFF];
        if (write) { *w = (*w & ~(0xFu << ((a & 7) * 4))) | (v & 0xF) << ((a & 7) * 4); return 0; }
        return (*w >> ((a & 7) * 4)) & 0xF;
    default: /* 32 bits, and the formats that live inside a 32-bit pixel */
        a = ((bp + (y & ~0x1Fu) * bw + ((x >> 1) & ~0x1Fu) + (kBlock32[(y >> 3) & 3][(x >> 3) & 7] ^ zx)) << 6) + kCol32[y & 7][x & 7];
        if (write == 2) { return (a >> 6) & 0x3FFF; }
        VRAM_TOUCH(a >> 6);
        w = &sVram[a & 0xFFFFF];
        switch (psm) {
        case 0x01: if (write) { *w = (*w & 0xFF000000u) | (v & 0xFFFFFF); return 0; } return *w & 0xFFFFFF;
        case 0x1B: if (write) { *w = (*w & 0x00FFFFFFu) | v << 24; return 0; } return *w >> 24;
        case 0x24: if (write) { *w = (*w & 0xF0FFFFFFu) | (v & 15) << 24; return 0; } return (*w >> 24) & 15;
        case 0x2C: if (write) { *w = (*w & 0x0FFFFFFFu) | (v & 15) << 28; return 0; } return *w >> 28;
        default: if (write) { *w = v; return 0; } return *w;
        }
    }
}

uint32_t Gs_VramRead(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y) {
    return vram_rw(bp, bw, psm, x, y, 0, 0);
}

/* A hash of one 8 KB page of GS memory, recomputed only after an upload touched the page. The GPU back end keys
   its texture cache on these, so a texture the game uploads again unchanged (it streams the stage's textures to
   one address many times per frame) is recognised instead of decoded again. */
#ifdef PORT_VITA
/* The pixels (stored bits) of the rectangle (0, 0, w, h) of buffer (bp, bw, psm) into out (w per row), a block
   at a time: one address computation and one residency check per block instead of per pixel. */
/* An 8- or 4-bit texture's indices as bytes, rows `stride` apart (the renderer's palette textures: no 32-bit pass
   in between). 0: not such a format (Gs_VramReadRect then). */
int Gs_VramReadIdx8(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t w, uint32_t h, uint8_t *out, uint32_t stride) {
    uint32_t gw, gh, bx, by, x, y, write = 0;
    if (psm != 0x13 && psm != 0x14) {
        return 0;
    }
    psm_block(psm, &gw, &gh);
    for (by = 0; by < h; by += gh) {
        uint32_t yh = h - by < gh ? h - by : gh;
        for (bx = 0; bx < w; bx += gw) {
            uint32_t xw = w - bx < gw ? w - bx : gw;
            uint32_t b = vram_rw(bp, bw, psm, bx, by, 2, 0) & 0x3FFF;
            const uint8_t *s8 = (const uint8_t *)&sVram[b * 64];
            VRAM_TOUCH(b);
            (void)write;
            for (y = 0; y < yh; y++) {
                uint8_t *o = &out[(by + y) * stride + bx];
                if (psm == 0x13) {
                    for (x = 0; x < xw; x++) { o[x] = s8[sCol8[y][x]]; }
                } else {
                    for (x = 0; x < xw; x++) { uint32_t n = sCol4[y][x]; o[x] = (uint8_t)((s8[n >> 1] >> ((n & 1) * 4)) & 15); }
                }
            }
        }
    }
    return 1;
}
void Gs_VramReadRect(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t w, uint32_t h, uint32_t *out) {
    uint32_t gw, gh, bx, by, x, y, p = psm, write = 0;

    if ((p & 0x30) == 0x30) { p &= 0x0F; }
    psm_block(psm, &gw, &gh);
    for (by = 0; by < h; by += gh) {
        uint32_t yh = h - by < gh ? h - by : gh;
        for (bx = 0; bx < w; bx += gw) {
            uint32_t xw = w - bx < gw ? w - bx : gw;
            uint32_t b = vram_rw(bp, bw, psm, bx, by, 2, 0) & 0x3FFF;
            const uint32_t *bw32 = &sVram[b * 64];
            VRAM_TOUCH(b);
            (void)write;
            for (y = 0; y < yh; y++) {
                uint32_t *o = &out[(by + y) * w + bx];
                switch (p) {
                case 0x02: case 0x0A: {
                    const uint16_t *s = (const uint16_t *)bw32;
                    for (x = 0; x < xw; x++) { o[x] = s[kCol16[y][x]]; }
                    break;
                }
                case 0x13: {
                    const uint8_t *s = (const uint8_t *)bw32;
                    for (x = 0; x < xw; x++) { o[x] = s[sCol8[y][x]]; }
                    break;
                }
                case 0x14: {
                    const uint8_t *s = (const uint8_t *)bw32;
                    for (x = 0; x < xw; x++) { uint32_t n = sCol4[y][x]; o[x] = (s[n >> 1] >> ((n & 1) * 4)) & 15; }
                    break;
                }
                case 0x01: for (x = 0; x < xw; x++) { o[x] = bw32[kCol32[y][x]] & 0xFFFFFF; } break;
                case 0x1B: for (x = 0; x < xw; x++) { o[x] = bw32[kCol32[y][x]] >> 24; } break;
                case 0x24: for (x = 0; x < xw; x++) { o[x] = (bw32[kCol32[y][x]] >> 24) & 15; } break;
                case 0x2C: for (x = 0; x < xw; x++) { o[x] = bw32[kCol32[y][x]] >> 28; } break;
                default: for (x = 0; x < xw; x++) { o[x] = bw32[kCol32[y][x]]; } break;
                }
            }
        }
    }
}

/* A number for what the rectangle (x0, y0, w, h) of the buffer (bp, bw, psm) holds, block by block: the renderer's
   texture key. The same number: the same pixels (as far as 32 bits can tell). */
uint32_t Gs_RectId(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h) {
    uint32_t gw, gh, x, y, g = 2166136261u;

    psm_block(psm, &gw, &gh);
    for (y = y0 & ~(gh - 1); y < y0 + h; y += gh) {
        for (x = x0 & ~(gw - 1); x < x0 + w; x += gw) {
            g = (g ^ blk_id(vram_rw(bp, bw, psm, x, y, 2, 0))) * 16777619u;
            g ^= g >> 13;
        }
    }
    return g;
}
/* the number an upload's memory is remembered by: for a format in part of the 32-bit pixels, the writes
   to its own bits of the rectangle's blocks (sPlaneGen); else what the rectangle holds (Gs_RectId) */
static uint32_t up_rect_id(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h) {
    int planes = psm_planes(psm), k;
    uint32_t gw, gh, x, y, g = 2166136261u ^ (uint32_t)planes;
    if (planes == 7 || !gVitaPlanes) {
        return Gs_RectId(bp, bw, psm, x0, y0, w, h);
    }
    psm_block(psm, &gw, &gh);
    for (y = y0 & ~(gh - 1); y < y0 + h; y += gh) {
        for (x = x0 & ~(gw - 1); x < x0 + w; x += gw) {
            uint32_t b = vram_rw(bp, bw, psm, x, y, 2, 0) & 0x3FFF;
            for (k = 0; k < 3; k++) {
                if (planes >> k & 1) {
                    g = (g ^ sPlaneGen[b][k] ^ b << 18) * 16777619u;
                    g ^= g >> 13;
                }
            }
        }
    }
    return g;
}
/* the large uploads written pixel by pixel: format, size, place, and why they were written */
static struct { uint32_t key, n, px, why[4]; } sUpTop[16];
static void up_top_add(uint32_t psm, uint32_t tx, uint32_t ty, uint32_t tw, uint32_t th, int why) {
    uint32_t key = psm | (tw & 0x3FF) << 6 | (th & 0x3FF) << 16 | ((tx | ty) & 7 ? 1u << 26 : 0) | (tx & 1 ? 1u << 27 : 0);
    int k, low = 0;
    for (k = 0; k < 16; k++) {
        if (sUpTop[k].n != 0 && sUpTop[k].key == key) {
            break;
        }
        if (sUpTop[k].n < sUpTop[low].n) {
            low = k;
        }
    }
    if (k == 16) {
        k = low;
        memset(&sUpTop[k], 0, sizeof(sUpTop[k]));
        sUpTop[k].key = key;
    }
    sUpTop[k].n++;
    sUpTop[k].px += tw * th;
    sUpTop[k].why[why & 3]++;
}
void Vita_UpLog(void) {
    extern void Vita_Log(const char *fmt, ...);
    char line[600];
    int len = 0, j;
    for (j = 0; j < 6; j++) {
        int k, best = -1;
        for (k = 0; k < 16; k++) {
            if (sUpTop[k].n > 0 && (best < 0 || sUpTop[k].px > sUpTop[best].px)) {
                best = k;
            }
        }
        if (best < 0) {
            break;
        }
        len += snprintf(line + len, sizeof(line) - (size_t)len, " psm %02x %ux%u%s: %u (%u k px; first %u, other data %u, other place %u, memory changed %u);",
                        sUpTop[best].key & 0x3F, (sUpTop[best].key >> 6) & 0x3FF, (sUpTop[best].key >> 16) & 0x3FF, (sUpTop[best].key >> 26 & 1) ? " off the grid" : "",
                        sUpTop[best].n, sUpTop[best].px / 1000, sUpTop[best].why[0], sUpTop[best].why[1], sUpTop[best].why[2], sUpTop[best].why[3]);
        sUpTop[best].n = 0;
    }
    Vita_Log("vita:   large uploads written pixel by pixel (planes %d):%s\n", gVitaPlanes, len ? line : " none");
    memset(sUpTop, 0, sizeof(sUpTop));
}
#endif

uint32_t Gs_PageHash(uint32_t page) {
    static uint32_t hash[512], seen[512];
    static uint8_t valid[512];
    uint32_t h = 2166136261u, i;
    const uint32_t *w;

    page &= 511;
    if (valid[page] && seen[page] == gGsPageGen[page]) {
        return hash[page];
    }
#ifdef PORT_VITA
    for (i = page * 32; i < page * 32 + 32; i++) { /* (the page's blocks have to be there to be read) */
        if (sBlk[i].up != 0) {
            blk_real(i);
        }
    }
#endif
    w = &sVram[page * 2048];
    for (i = 0; i < 2048; i++) {
        h = (h ^ w[i]) * 16777619u;
        h ^= h >> 15;
    }
    hash[page] = h;
    seen[page] = gGsPageGen[page];
    valid[page] = 1;
    return h;
}

static Target *target_get(uint32_t fbp) {
    int i;
    for (i = 0; i < sTargetCount; i++) {
        if (sTargets[i].fbp == fbp) {
            return &sTargets[i];
        }
    }
    if (sTargetCount == 16) {
        return &sTargets[0];
    }
    sTargets[sTargetCount].fbp = fbp;
    sTargets[sTargetCount].z = calloc(SURF_W * SURF_H, 4);
    return &sTargets[sTargetCount++];
}

/* ------------------------------------------------------------------------------------------------ textures */

int Gs_PsmBits(uint32_t psm) {
    if ((psm & 0x30) == 0x30) {
        psm &= 0x0F; /* a depth buffer read as a texture: 32, 24 or 16 bits like the colour formats */
    }
    switch (psm) {
    case 0x00: return 32;
    case 0x01: return 24;
    case 0x02: case 0x0A: return 16;
    case 0x13: case 0x1B: return 8;
    case 0x14: case 0x24: case 0x2C: return 4;
    default: return 32;
    }
}

/* A stored colour of format psm as R, G, B, A (TEXA supplies alpha where the format has none or one bit). */
uint32_t Gs_Expand(uint32_t c, uint32_t psm) {
    uint32_t ta0 = gs.texa & 0xFF, ta1 = (gs.texa >> 32) & 0xFF, aem = (gs.texa >> 15) & 1;

    if ((psm & 0x30) == 0x30) {
        psm &= 0x0F;
    }

    if (psm == 0x00) {
        return c;
    }
    if (psm == 0x01) {
        c &= 0xFFFFFF;
        return c | ((aem && c == 0 ? 0 : ta0) << 24);
    }
    {
        uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
        uint32_t a = (c & 0x8000) ? ta1 : (aem && (c & 0x7FFF) == 0 ? 0 : ta0);
        return r | g << 8 | b << 16 | a << 24;
    }
}

static uint32_t texel(int ctx, int u, int v) {
    uint64_t t0 = gs.tex0[ctx], cl = gs.clamp[ctx];
    uint32_t tbp = t0 & 0x3FFF, tbw = (t0 >> 14) & 0x3F, psm = (t0 >> 20) & 0x3F, tw = 1u << ((t0 >> 26) & 15), th = 1u << ((t0 >> 30) & 15);
    uint32_t cbp = (t0 >> 37) & 0x3FFF, cpsm = (t0 >> 51) & 15, csa = (t0 >> 56) & 31;
    uint32_t wms = cl & 3, wmt = (cl >> 2) & 3;
    int minu = (cl >> 4) & 0x3FF, maxu = (cl >> 14) & 0x3FF, minv = (cl >> 24) & 0x3FF, maxv = (cl >> 34) & 0x3FF;
    uint32_t c;

    if (wms == 0) { u &= (int)tw - 1; } else if (wms == 1) { u = u < 0 ? 0 : u >= (int)tw ? (int)tw - 1 : u; }
    else if (wms == 2) { u = u < minu ? minu : u > maxu ? maxu : u; } else { u = (u & minu) | maxu; }
    if (wmt == 0) { v &= (int)th - 1; } else if (wmt == 1) { v = v < 0 ? 0 : v >= (int)th ? (int)th - 1 : v; }
    else if (wmt == 2) { v = v < minv ? minv : v > maxv ? maxv : v; } else { v = (v & minv) | maxv; }
    c = vram_rw(tbp, tbw, psm, (uint32_t)u, (uint32_t)v, 0, 0);
    if (Gs_PsmBits(psm) > 8) {
        return Gs_Expand(c, psm);
    }
    /* Palette (CSM1): a 16-colour palette is an 8 x 2 block at CBP, a 256-colour one 16 x 16 read in the GS's order.
       CSA only moves a 16-colour palette inside the GS's internal buffer, not in memory. */
    (void)csa;
    if (Gs_PsmBits(psm) == 8) {
        c = (c & 0xE7) | ((c & 8) << 1) | ((c & 0x10) >> 1);
        return Gs_Expand(vram_rw(cbp, 1, cpsm, c & 15, c >> 4, 0, 0), cpsm);
    }
    return Gs_Expand(vram_rw(cbp, 1, cpsm, c & 7, c >> 3, 0, 0), cpsm);
}

/* a texel of any texture in GS memory, as texel() reads it (no wrapping) */
uint32_t Gs_TexelOf(uint64_t t0, int u, int v) {
    uint32_t tbp = t0 & 0x3FFF, tbw = (t0 >> 14) & 0x3F, psm = (t0 >> 20) & 0x3F;
    uint32_t cbp = (t0 >> 37) & 0x3FFF, cpsm = (t0 >> 51) & 15, c;
    c = vram_rw(tbp, tbw, psm, (uint32_t)u, (uint32_t)v, 0, 0);
    if (Gs_PsmBits(psm) > 8) {
        return Gs_Expand(c, psm);
    }
    if (Gs_PsmBits(psm) == 8) {
        c = (c & 0xE7) | ((c & 8) << 1) | ((c & 0x10) >> 1);
        return Gs_Expand(vram_rw(cbp, 1, cpsm, c & 15, c >> 4, 0, 0), cpsm);
    }
    return Gs_Expand(vram_rw(cbp, 1, cpsm, c & 7, c >> 3, 0, 0), cpsm);
}

/* ------------------------------------------------------------------------------------------------ pixels */

static void pixel(Target *sf, int ctx, int x, int y, uint32_t z, uint32_t src) {
    uint64_t test = gs.test[ctx], al = gs.alpha[ctx], zb = gs.zbuf[ctx], pr = gs.prim;
    uint64_t sc = gs.scissor[ctx], fr = gs.frame[ctx];
    uint32_t fbp = (fr & 0x1FF) << 5, fbw = (fr >> 16) & 0x3F, fpsm = (fr >> 24) & 0x3F;
    uint32_t d, sr, sg, sb, sa, out[3], zbp, zpsm, zmax;
    int i, write_rgb = 1, write_z = !((zb >> 32) & 1);

    /* BT3_GS_PROBE=x,y: in the BT3_GS_DUMP frame, report every primitive that reaches this pixel */
    if (sDumpFrame == (int)sFrame && x == sProbeX && y == sProbeY) {
        uint64_t t0 = gs.tex0[ctx];
        fprintf(stderr, "  probe: prim %llx z %08x src %08x | tex %04x psm %02x %ux%u tcc %u tfx %u cbp %04x | alpha %llx test %llx frame %llx zbuf %llx clamp %llx | depth there %08x\n",
                (unsigned long long)gs.prim, z, src, (unsigned)(t0 & 0x3FFF), (unsigned)((t0 >> 20) & 0x3F), 1u << ((t0 >> 26) & 15),
                1u << ((t0 >> 30) & 15), (unsigned)((t0 >> 34) & 1), (unsigned)((t0 >> 35) & 3), (unsigned)((t0 >> 37) & 0x3FFF),
                (unsigned long long)gs.alpha[ctx], (unsigned long long)gs.test[ctx], (unsigned long long)gs.frame[ctx],
                (unsigned long long)gs.zbuf[ctx], (unsigned long long)gs.clamp[ctx],
                vram_rw((uint32_t)(gs.zbuf[ctx] & 0x1FF) << 5, (uint32_t)((gs.frame[ctx] >> 16) & 0x3F), 0x30 | (uint32_t)((gs.zbuf[ctx] >> 24) & 15), (uint32_t)x, (uint32_t)y, 0, 0));
    }

    if (x < (int)(sc & 0x7FF) || x > (int)((sc >> 16) & 0x7FF) || y < (int)((sc >> 32) & 0x7FF) || y > (int)((sc >> 48) & 0x7FF) ||
        x < 0 || y < 0 || x >= SURF_W || y >= SURF_H) {
        sPix[0]++;
        return;
    }
    sa = src >> 24;
    if (test & 1) { /* alpha test */
        uint32_t ref = (test >> 4) & 0xFF;
        int pass;
        switch ((test >> 1) & 7) {
        case 0: pass = 0; break;
        case 1: pass = 1; break;
        case 2: pass = sa < ref; break;
        case 3: pass = sa <= ref; break;
        case 4: pass = sa == ref; break;
        case 5: pass = sa >= ref; break;
        case 6: pass = sa > ref; break;
        default: pass = sa != ref; break;
        }
        if (!pass) {
            switch ((test >> 12) & 3) {
            case 0: sPix[1]++; return;             /* keep */
            case 1: write_z = 0; break;            /* frame buffer only */
            case 2: write_rgb = 0; break;          /* z only */
            default: write_z = 0; break;           /* RGB only */
            }
        }
    }
    /* The depth buffer lives in GS memory like everything else (the game clears it by drawing into it as a frame
       buffer and reads it as a texture): ZBUF.ZBP, the frame's width, format 0x30 + ZBUF.PSM. */
    zbp = (uint32_t)(zb & 0x1FF) << 5;
    zpsm = 0x30 | (uint32_t)((zb >> 24) & 15);
    zmax = Gs_PsmBits(zpsm) == 32 ? 0xFFFFFFFFu : Gs_PsmBits(zpsm) == 24 ? 0xFFFFFFu : 0xFFFFu;
    if (z > zmax) {
        z = zmax;
    }
    if ((test >> 16) & 1) { /* depth test */
        uint32_t zd = vram_rw(zbp, fbw, zpsm, (uint32_t)x, (uint32_t)y, 0, 0);
        switch ((test >> 17) & 3) {
        case 0: sPix[2]++; return;
        case 1: break;
        case 2: if (z < zd) { sPix[2]++; return; } break;
        default: if (z <= zd) { sPix[2]++; return; } break;
        }
    }
    d = vram_rw(fbp, fbw, fpsm, (uint32_t)x, (uint32_t)y, 0, 0);
    if (Gs_PsmBits(fpsm) == 16) {
        d = (d & 31) << 3 | ((d >> 5) & 31) << 11 | ((d >> 10) & 31) << 19 | ((d & 0x8000) ? 0x80000000u : 0);
    } else if (fpsm == 1) {
        d |= 0x80000000u;
    }
    sr = src & 0xFF; sg = (src >> 8) & 0xFF; sb = (src >> 16) & 0xFF;
    out[0] = sr; out[1] = sg; out[2] = sb;
    if ((pr >> 6) & 1) { /* blending: ((A - B) * C >> 7) + D */
        uint32_t s[3] = {sr, sg, sb};
        uint32_t c = ((al >> 4) & 3) == 0 ? sa : ((al >> 4) & 3) == 1 ? d >> 24 : (uint32_t)((al >> 32) & 0xFF);
        for (i = 0; i < 3; i++) {
            int dc = (int)((d >> (8 * i)) & 0xFF);
            int a = (al & 3) == 0 ? (int)s[i] : (al & 3) == 1 ? dc : 0;
            int b = ((al >> 2) & 3) == 0 ? (int)s[i] : ((al >> 2) & 3) == 1 ? dc : 0;
            int dd = ((al >> 6) & 3) == 0 ? (int)s[i] : ((al >> 6) & 3) == 1 ? dc : 0;
            int v = (((a - b) * (int)c) >> 7) + dd;
            out[i] = v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v;
        }
    }
    if (write_rgb) {
        /* FRAME.FBMSK: bits that are set are NOT written. The game depends on it (it moves single channels
           between the depth buffer, the frame's alpha byte and work buffers, sometimes through a 16-bit view
           of 32-bit memory). The mask is given for 32-bit pixels and narrows with the format. */
        uint32_t c = out[0] | out[1] << 8 | out[2] << 16 | sa << 24, m = (uint32_t)(fr >> 32), old;
        if (Gs_PsmBits(fpsm) == 16) {
            c = out[0] >> 3 | (out[1] >> 3) << 5 | (out[2] >> 3) << 10 | (sa & 0x80) << 8;
            m = ((m >> 3) & 0x1F) | ((m >> 6) & 0x3E0) | ((m >> 9) & 0x7C00) | ((m >> 16) & 0x8000);
        }
        if (m != 0) {
            old = vram_rw(fbp, fbw, fpsm, (uint32_t)x, (uint32_t)y, 0, 0);
            c = (old & m) | (c & ~m);
        }
        vram_rw(fbp, fbw, fpsm, (uint32_t)x, (uint32_t)y, 1, c);
        sf->drawn++;
        sPix[3]++;
    }
    if (write_z) {
        vram_rw(zbp, fbw, zpsm, (uint32_t)x, (uint32_t)y, 1, z);
    }
}

/* Vertex colour and texture combined (TEX0.TFX / TCC). */
static uint32_t shade(int ctx, uint32_t vc, float s, float t, float q, int u, int v) {
    uint64_t pr = gs.prim, t0 = gs.tex0[ctx];
    uint32_t tc, c[4], out = 0, tfx = (t0 >> 35) & 3, tcc = (t0 >> 34) & 1;
    int i;

    if (!((pr >> 4) & 1)) {
        return vc;
    }
    if ((pr >> 8) & 1) {
        tc = texel(ctx, u >> 4, v >> 4);
    } else {
        float w = q != 0.0f ? 1.0f / q : 1.0f;
        tc = texel(ctx, (int)(s * w * (float)(1u << ((t0 >> 26) & 15))), (int)(t * w * (float)(1u << ((t0 >> 30) & 15))));
    }
    for (i = 0; i < 3; i++) {
        uint32_t cv = (vc >> (8 * i)) & 0xFF, ct = (tc >> (8 * i)) & 0xFF, av = vc >> 24;
        c[i] = tfx == 0 ? (ct * cv) >> 7 : tfx == 1 ? ct : ((ct * cv) >> 7) + av;
        if (c[i] > 255) { c[i] = 255; }
        out |= c[i] << (8 * i);
    }
    {
        uint32_t av = vc >> 24, at = tc >> 24;
        c[3] = !tcc ? av : tfx == 0 ? (at * av) >> 7 : tfx == 2 ? at + av : at;
        if (c[3] > 255) { c[3] = 255; }
    }
    return out | c[3] << 24;
}

/* ------------------------------------------------------------------------------------------------ primitives */

static uint32_t vcol(const Vertex *v) {
    return v->r | v->g << 8 | v->b << 16 | (uint32_t)v->a << 24;
}

static void draw_sprite(Target *sf, int ctx, const Vertex *a, const Vertex *b) {
    float x0 = a->x, x1 = b->x, y0 = a->y, y1 = b->y;
    int xa, xb, ya, yb, x, y;
    uint32_t col = vcol(b);

    if (x0 > x1) { const Vertex *t = a; a = b; b = t; x0 = a->x; x1 = b->x; }
    xa = (int)(x0 + 0.9375f); xb = (int)(x1 + 0.9375f);
    if (y0 <= y1) { ya = (int)(y0 + 0.9375f); yb = (int)(y1 + 0.9375f); } else { ya = (int)(y1 + 0.9375f); yb = (int)(y0 + 0.9375f); }
    for (y = ya; y < yb; y++) {
        float fy = y1 != y0 ? ((float)y - y0) / (y1 - y0) : 0.0f;
        for (x = xa; x < xb; x++) {
            float fx = x1 != x0 ? ((float)x - x0) / (x1 - x0) : 0.0f;
            pixel(sf, ctx, x, y, b->z, shade(ctx, col, a->s + (b->s - a->s) * fx, a->t + (b->t - a->t) * fy, b->q,
                                             (int)((float)a->u + (float)(b->u - a->u) * fx), (int)((float)a->v + (float)(b->v - a->v) * fy)));
        }
    }
}

static float edge(const Vertex *a, const Vertex *b, float x, float y) {
    return (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
}

static void draw_triangle(Target *sf, int ctx, const Vertex *a, const Vertex *b, const Vertex *c) {
    float area = edge(a, b, c->x, c->y);
    int gouraud = (gs.prim >> 3) & 1;
    float minx = a->x, maxx = a->x, miny = a->y, maxy = a->y;
    int x, y;

    if (area == 0.0f) {
        return;
    }
    if (b->x < minx) { minx = b->x; } if (c->x < minx) { minx = c->x; }
    if (b->x > maxx) { maxx = b->x; } if (c->x > maxx) { maxx = c->x; }
    if (b->y < miny) { miny = b->y; } if (c->y < miny) { miny = c->y; }
    if (b->y > maxy) { maxy = b->y; } if (c->y > maxy) { maxy = c->y; }
    if (minx < 0) { minx = 0; } if (miny < 0) { miny = 0; }
    if (maxx > SURF_W - 1) { maxx = SURF_W - 1; } if (maxy > SURF_H - 1) { maxy = SURF_H - 1; }
    for (y = (int)miny; y <= (int)maxy; y++) {
        for (x = (int)minx; x <= (int)maxx; x++) {
            /* weights in double precision and results rounded, not truncated: with float weights that sum to just
               under 1 a flat triangle's depth came out one unit low on some pixels, and the fighters' black outline
               hull (drawn at almost the same depth as the body) showed through as speckles */
            double w0 = (double)edge(b, c, (float)x, (float)y) / (double)area, w1 = (double)edge(c, a, (float)x, (float)y) / (double)area, w2 = 1.0 - w0 - w1;
            uint32_t col;

            if (w0 < 0.0 || w1 < 0.0 || w2 < -1e-9) {
                continue;
            }
            if (gouraud) {
                col = (uint32_t)(a->r * w0 + b->r * w1 + c->r * w2 + 0.5) | (uint32_t)(a->g * w0 + b->g * w1 + c->g * w2 + 0.5) << 8 |
                      (uint32_t)(a->b * w0 + b->b * w1 + c->b * w2 + 0.5) << 16 | (uint32_t)(a->a * w0 + b->a * w1 + c->a * w2 + 0.5) << 24;
            } else {
                col = vcol(c);
            }
            pixel(sf, ctx, x, y, (uint32_t)((double)a->z * w0 + (double)b->z * w1 + (double)c->z * w2 + 0.5),
                  shade(ctx, col, (float)(a->s * w0 + b->s * w1 + c->s * w2), (float)(a->t * w0 + b->t * w1 + c->t * w2),
                        (float)(a->q * w0 + b->q * w1 + c->q * w2), (int)(a->u * w0 + b->u * w1 + c->u * w2),
                        (int)(a->v * w0 + b->v * w1 + c->v * w2)));
        }
    }
}

static void draw_line(Target *sf, int ctx, const Vertex *a, const Vertex *b) {
    float dx = b->x - a->x, dy = b->y - a->y;
    int n = (int)((dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy)), i;

    for (i = 0; i <= n; i++) {
        float f = n ? (float)i / (float)n : 0.0f;
        pixel(sf, ctx, (int)(a->x + dx * f), (int)(a->y + dy * f), b->z, vcol(b));
    }
}

#ifdef PORT_VITA
/* Direct parts (see GR_PART below): while a kept part is walked to be kept as groups, which GS registers it reads
   before writing them (they must be the same next time) and which it writes (set to what they were after it).
   The registers as fields of gs: 0 prim (the effective one), 1 PRIM, 2 RGBAQ, 3 ST, 4 UV, 5 TEXA, 6 PRMODECONT,
   7 PRMODE, 8/9 TEX0, 10/11 TEX1, 12/13 CLAMP, 14/15 XYOFFSET, 16/17 SCISSOR, 18/19 ALPHA, 20/21 TEST, 22/23 FRAME,
   24/25 ZBUF, 26/27 FBA, 28..31 BITBLTBUF, TRXPOS, TRXREG, TRXDIR; 32 Q, 33 the vertex queue (emptied by PRIM). */
static int sDpCap, sDpBad;
static uint64_t sDpCmp, sDpWr;
#define DPF(i) (1ull << (i))
#define DP_STRIP_READS (DPF(0) | DPF(5) | 0x0FFFFF00ull) /* what a strip's state is made of: prim, TEXA, fields 8..27 */
static inline void dp_rd(uint64_t m) { sDpCmp |= m & ~sDpWr; }
/* a strip's state is made of its own context's fields only (vita_vgl.c state_of reads [ctx]): with both
   contexts' fields compared, a fighter's part drawn in the first context missed its kept form whenever the second
   context's TEXO had changed in between (the ground shadows', the effects': field 9, 15,000 times a 2 s) */
static inline uint64_t dp_strip_reads(int ctx) {
    uint64_t m = DPF(0) | DPF(5);
    int b;
    if (ctx < 0 || ctx > 1) {
        return DP_STRIP_READS;
    }
    for (b = 8; b <= 26; b += 2) {
        m |= DPF(b + ctx);
    }
    return m;
}
static inline void dp_wr(uint64_t m) { sDpWr |= m; }
static void dp_note(uint32_t addr) {
    switch (addr) {
    case 0x00: dp_rd(DPF(6) | DPF(7)); dp_wr(DPF(1) | DPF(0) | DPF(33)); break;
    case 0x01: dp_wr(DPF(2) | DPF(32)); break;
    case 0x02: dp_wr(DPF(3)); break;
    case 0x03: dp_wr(DPF(4)); break;
    case 0x06: case 0x07: dp_wr(DPF(8 + addr - 0x06)); break;
    case 0x08: case 0x09: dp_wr(DPF(12 + addr - 0x08)); break;
    case 0x14: case 0x15: dp_wr(DPF(10 + addr - 0x14)); break;
    case 0x16: case 0x17: dp_rd(DPF(8 + addr - 0x16)); dp_wr(DPF(8 + addr - 0x16)); break; /* TEX2: part of TEX0 */
    case 0x18: case 0x19: dp_wr(DPF(14 + addr - 0x18)); break;
    case 0x1A: dp_rd(DPF(1) | DPF(7)); dp_wr(DPF(6) | DPF(0)); break;
    case 0x1B: dp_rd(DPF(1) | DPF(6)); dp_wr(DPF(7) | DPF(0)); break;
    case 0x3B: dp_wr(DPF(5)); break;
    case 0x40: case 0x41: dp_wr(DPF(16 + addr - 0x40)); break;
    case 0x42: case 0x43: dp_wr(DPF(18 + addr - 0x42)); break;
    case 0x47: case 0x48: dp_wr(DPF(20 + addr - 0x47)); break;
    case 0x4A: case 0x4B: dp_wr(DPF(26 + addr - 0x4A)); break;
    case 0x4C: case 0x4D: dp_wr(DPF(22 + addr - 0x4C)); break;
    case 0x4E: case 0x4F: dp_wr(DPF(24 + addr - 0x4E)); break;
    case 0x50: case 0x51: case 0x52: dp_wr(DPF(28 + addr - 0x50)); break;
    case 0x04: case 0x05: case 0x0C: case 0x0D: case 0x53: case 0x7F: sDpBad = 1; break; /* vertices, a transfer, a marker */
    default: break;
    }
}
#endif

/* The seam's exact positions (z3xox's 5a2a000 and 3eca58d in the PC build). A model's seam (the strip joining two of
   its parts: the back of Videl's shirt) is projected on the CPU and sent as plain GS triangles, rounded to the GS's
   1/16 pixel (src/port/vu0_a.c, the seam routine); the parts beside it come out of the vertex programs at full
   precision. Their shared edges did not meet: the black outline behind showed through, a dashed line down the seam.
   The routine leaves here each vertex's position before the rounding, under the rounded one; a GS vertex with that
   rounded position (and depth) is drawn at it, and a triangle of three such corners a little larger (overlapping the
   parts beside it closes the pixels still open where one edge has a corner the other has not). Looked at only once
   a seam has been made (sSeamAny): the vertices of everything else go by as before. */
#define SEAM_SLOTS 16384
static struct { volatile uint64_t key; volatile float fx, fy; } sSeam[SEAM_SLOTS];
static volatile int sSeamAny;
static uint8_t sVtxSeam[3]; /* which of gs.vtx are a seam's */

static uint64_t seam_key(uint32_t x, uint32_t y, uint32_t z) {
    return ((uint64_t)(z & 0xFFFFFF) << 32 | (uint64_t)(y & 0xFFFF) << 16 | (x & 0xFFFF)) | 1ull << 63; /* (never 0) */
}

void Port_SeamExact(uint32_t x, uint32_t y, uint32_t z, uint32_t fxBits, uint32_t fyBits) {
    uint64_t key = seam_key(x, y, z >> 4); /* (the seam goes to the GS as XYZF2: its depth is the word's bits 4..27) */
    unsigned slot = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 50) & (SEAM_SLOTS - 1);
    float fx, fy;
    memcpy(&fx, &fxBits, 4);
    memcpy(&fy, &fyBits, 4);
    __atomic_store_n(&sSeam[slot].key, 0, __ATOMIC_RELEASE); /* (a reader in between finds no key) */
    sSeam[slot].fx = fx;
    sSeam[slot].fy = fy;
    __atomic_store_n(&sSeam[slot].key, key, __ATOMIC_RELEASE);
    sSeamAny = 1;
}

static int seam_exact(uint32_t x, uint32_t y, uint32_t z, float *fx, float *fy) {
    uint64_t key = seam_key(x, y, z);
    unsigned slot = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 50) & (SEAM_SLOTS - 1);
    if (__atomic_load_n(&sSeam[slot].key, __ATOMIC_ACQUIRE) != key) {
        return 0;
    }
    *fx = sSeam[slot].fx;
    *fy = sSeam[slot].fy;
    return __atomic_load_n(&sSeam[slot].key, __ATOMIC_ACQUIRE) == key;
}

/* a seam triangle, each corner moved about half a screen pixel away from its middle (the Vita's picture is 1.5 / 1.2
   times the GS's: 0.4 of a GS pixel) */
static void seam_widen(Vertex *t) {
    float cx = (t[0].x + t[1].x + t[2].x) / 3.0f, cy = (t[0].y + t[1].y + t[2].y) / 3.0f;
    int i;
    for (i = 0; i < 3; i++) {
        float dx = t[i].x - cx, dy = t[i].y - cy, len = __builtin_sqrtf(dx * dx + dy * dy);
        if (len > 1e-4f) {
            t[i].x += dx / len * 0.4f;
            t[i].y += dy / len * 0.4f;
        }
    }
}

/* A vertex arrives (XYZ2 / XYZF2 with kick = 1, XYZ3 / XYZF3 with kick = 0). */
/* (out of vertex(), see there) */
static void vertex_debug(int *kickp, int type, int ctx) {
    int kick = *kickp;
    {   /* BT3_GS_STOP=<n>: in the frame named by BT3_GS_FROM, draw only the first n primitives (to find what covers what) */
        static int stop = -1;
        static unsigned count, frame = 0xFFFFFFFFu;
        if (stop < 0) { stop = getenv("BT3_GS_STOP") != NULL ? atoi(getenv("BT3_GS_STOP")) : 0; }
        if (frame != sFrame) { frame = sFrame; count = 0; }
        /* counted in complete primitives, the unit of the BT3_GS_DUMP listing */
        if (kick && gs.vcount >= (type == 6 || type == 1 || type == 2 ? 2 : type == 0 ? 1 : 3)) { count++; }
        if (stop > 0 && (int)sFrame == sDrawFrom && count > (unsigned)stop) { kick = 0; }
    }
    if ((int)sFrame < sDrawFrom) {
        kick = 0; /* BT3_GS_FROM: uploads and state are processed, nothing is drawn yet */
    }
    if (sDumpFrame == (int)sFrame && kick && gs.vcount >= (type == 6 || type == 1 || type == 2 ? 2 : type == 0 ? 1 : 3)) {
        const Vertex *p0 = &gs.vtx[0], *p1 = &gs.vtx[gs.vcount - 1];
        fprintf(stderr, "  prim %d ctx %d fbp %03x abe %d tme %d fst %d | (%.1f,%.1f)-(%.1f,%.1f) z %08x uv (%d,%d)-(%d,%d) st (%.3f,%.3f) q %.3f "
                        "rgba %02x%02x%02x%02x | tbp %04x psm %02x tw %d th %d cbp %04x | xyoff %d,%d scis %d..%d,%d..%d alpha %llx test %llx frame %llx zbuf %llx\n",
                type, ctx, (unsigned)(gs.frame[ctx] & 0x1FF), (int)((gs.prim >> 6) & 1), (int)((gs.prim >> 4) & 1), (int)((gs.prim >> 8) & 1),
                p0->x, p0->y, p1->x, p1->y, p1->z, p0->u >> 4, p0->v >> 4, p1->u >> 4, p1->v >> 4, p1->s, p1->t, p1->q, p1->r, p1->g, p1->b, p1->a,
                (unsigned)(gs.tex0[ctx] & 0x3FFF), (unsigned)((gs.tex0[ctx] >> 20) & 0x3F), 1 << ((gs.tex0[ctx] >> 26) & 15),
                1 << ((gs.tex0[ctx] >> 30) & 15), (unsigned)((gs.tex0[ctx] >> 37) & 0x3FFF),
                (int)(gs.xyoffset[ctx] & 0xFFFF) >> 4, (int)((gs.xyoffset[ctx] >> 32) & 0xFFFF) >> 4,
                (int)(gs.scissor[ctx] & 0x7FF), (int)((gs.scissor[ctx] >> 16) & 0x7FF), (int)((gs.scissor[ctx] >> 32) & 0x7FF),
                (int)((gs.scissor[ctx] >> 48) & 0x7FF), (unsigned long long)gs.alpha[ctx], (unsigned long long)gs.test[ctx],
                (unsigned long long)gs.frame[ctx], (unsigned long long)gs.zbuf[ctx]);
        if ((gs.prim >> 4) & 1) {
            uint32_t tx = texel(ctx, 8, 8), cb = (uint32_t)((gs.tex0[ctx] >> 37) & 0x3FFF), cp = (uint32_t)((gs.tex0[ctx] >> 51) & 15);
            fprintf(stderr, "    tex: tbw %u cpsm %x csm %u csa %u tcc %u tfx %u texa %llx | texel(8,8) %08x | clut[0..3] %08x %08x %08x %08x\n",
                    (unsigned)((gs.tex0[ctx] >> 14) & 0x3F), cp, (unsigned)((gs.tex0[ctx] >> 55) & 1), (unsigned)((gs.tex0[ctx] >> 56) & 31),
                    (unsigned)((gs.tex0[ctx] >> 34) & 1), (unsigned)((gs.tex0[ctx] >> 35) & 3), (unsigned long long)gs.texa, tx,
                    vram_rw(cb, 1, cp, 0, 0, 0, 0), vram_rw(cb, 1, cp, 1, 0, 0, 0), vram_rw(cb, 1, cp, 2, 0, 0, 0), vram_rw(cb, 1, cp, 3, 0, 0, 0));
        }
    }
    /* BT3_GS_ZDUMP=<frame>: when that frame first draws through the depth tint's table (CLUT 0x3E64), write the
       depth page (512 x 448 words: depth in the low 24 bits, the byte the effects index on top) to zdump.bin */
    static int zdump = -2; /* (read once: getenv for every sprite was a cost) */
    if (zdump == -2) { zdump = getenv("BT3_GS_ZDUMP") != NULL ? atoi(getenv("BT3_GS_ZDUMP")) : -1; }
    if (kick && type == 6 && gs.vcount == 2 && zdump >= 0 && (int)sFrame == zdump &&
        ((gs.tex0[ctx] >> 37) & 0x3FFF) == 0x3E64) {
        static int done;
        if (!done) {
            FILE *f = fopen("zdump.bin", "wb");
            uint32_t x, y;
            done = 1;
            for (y = 0; y < 448; y++) {
                for (x = 0; x < 512; x++) {
                    uint32_t w = Gs_VramRead(0xE0 * 32, 8, 0, x, y);
                    fwrite(&w, 4, 1, f);
                }
            }
            fclose(f);
        }
    }
    *kickp = kick;
}

static int sVtxDbg = -1; /* (the PC build's environment switches for every vertex: vertex_debug) */
static void vertex(uint32_t x, uint32_t y, uint32_t z, int kick) {
    int seamHit = 0;
    int ctx = (gs.prim >> 9) & 1, type = gs.prim & 7;
    Target *sf;
#ifdef PORT_VITA
    if (sDpCap) {
        sDpBad = 1;
    }
#endif
    Vertex v;

    sStat[5]++;
#ifdef PORT_VITA /* a frame that is read but not drawn (port/vita/vita_gfx.c) */
    {
        extern int gVitaGsSkip;
        if (gVitaGsSkip) {
            kick = 0;
        }
    }
#endif
    v.x = ((float)(int)x - (float)(gs.xyoffset[ctx] & 0xFFFF)) / 16.0f;
    v.y = ((float)(int)y - (float)((gs.xyoffset[ctx] >> 32) & 0xFFFF)) / 16.0f;
    v.z = z;
    if (__builtin_expect(sSeamAny, 0)) { /* a seam's vertex: where it was before the rounding (Port_SeamExact) */
        float fx, fy;
        seamHit = sGpu && seam_exact(x, y, z, &fx, &fy);
        if (seamHit) {
            v.x = fx - (float)(gs.xyoffset[ctx] & 0xFFFF) / 16.0f;
            v.y = fy - (float)((gs.xyoffset[ctx] >> 32) & 0xFFFF) / 16.0f;
        }
    }
    v.r = gs.rgbaq & 0xFF; v.g = (gs.rgbaq >> 8) & 0xFF; v.b = (gs.rgbaq >> 16) & 0xFF; v.a = (gs.rgbaq >> 24) & 0xFF;
    memcpy(&v.s, &gs.st, 4);
    { uint32_t tt = (uint32_t)(gs.st >> 32); memcpy(&v.t, &tt, 4); }
    v.q = gs.q;
    v.u = gs.uv & 0x3FFF; v.v = (gs.uv >> 16) & 0x3FFF;
    if (gs.vcount < 3) {
        sVtxSeam[gs.vcount] = (uint8_t)seamHit;
        gs.vtx[gs.vcount++] = v;
    } else {
        sVtxSeam[0] = sVtxSeam[1];
        sVtxSeam[1] = sVtxSeam[2];
        sVtxSeam[2] = (uint8_t)seamHit;
        gs.vtx[0] = gs.vtx[1]; gs.vtx[1] = gs.vtx[2]; gs.vtx[2] = v;
    }
    gs.strip++;
    /* the software renderer's target: looked up only where it is drawn into (the GPU back end draws
       elsewhere; a linear search for every vertex of the effects was a cost on the drawing thread) */
    sf = sGpu ? NULL : target_get(gs.frame[ctx] & 0x1FF);
    {   /* the tools below (environment switches of the PC build) looked at for every vertex only when
           one of them is set: a few dozen instructions a vertex on the drawing thread otherwise */
        if (sVtxDbg < 0) {
            sVtxDbg = getenv("BT3_GS_STOP") != NULL || getenv("BT3_GS_ZDUMP") != NULL || sDrawFrom > 0 || sDumpFrame >= 0;
        }
        if (__builtin_expect(sVtxDbg, 0)) {
            vertex_debug(&kick, type, ctx);
        }
    }
    switch (type) {
    case 0: if (kick) { if (sGpu) { GsGpu_Draw(0, ctx, &v); } else { pixel(sf, ctx, (int)v.x, (int)v.y, v.z, vcol(&v)); } } gs.vcount = 0; break;
    case 1: if (gs.vcount == 2) { if (kick) { if (sGpu) { GsGpu_Draw(1, ctx, gs.vtx); } else { draw_line(sf, ctx, &gs.vtx[0], &gs.vtx[1]); } } gs.vcount = 0; } break;
    case 2: if (gs.vcount == 2) { if (kick) { if (sGpu) { GsGpu_Draw(1, ctx, gs.vtx); } else { draw_line(sf, ctx, &gs.vtx[0], &gs.vtx[1]); } } gs.vtx[0] = gs.vtx[1]; gs.vcount = 1; } break;
    case 3: if (gs.vcount == 3) { if (kick) { if (sGpu && sVtxSeam[0] && sVtxSeam[1] && sVtxSeam[2]) { Vertex t[3] = {gs.vtx[0], gs.vtx[1], gs.vtx[2]}; seam_widen(t); GsGpu_Draw(3, ctx, t); } else if (sGpu) { GsGpu_Draw(3, ctx, gs.vtx); } else { draw_triangle(sf, ctx, &gs.vtx[0], &gs.vtx[1], &gs.vtx[2]); } } gs.vcount = 0; } break;
    case 4: if (gs.vcount == 3 && kick) { if (sGpu) { GsGpu_Draw(3, ctx, gs.vtx); } else { draw_triangle(sf, ctx, &gs.vtx[0], &gs.vtx[1], &gs.vtx[2]); } } break;
    case 5:
        if (gs.vcount == 3) {
            if (kick) { if (sGpu) { GsGpu_Draw(3, ctx, gs.vtx); } else { draw_triangle(sf, ctx, &gs.vtx[0], &gs.vtx[1], &gs.vtx[2]); } }
            gs.vtx[1] = gs.vtx[2]; /* fan: keep the first vertex */
            gs.vcount = 2;
        }
        break;
    case 6:
        if (gs.vcount == 2) {
            /* A rectangle drawn into TEXTURE memory (a page the game uploads to, addressed as a 64-pixel-wide
               buffer): the game paints over its fighters' palettes this way, to give them all one alpha before the
               see-through pass. That has to happen in GS memory, where the GPU back end reads palettes from. */
            uint32_t fb = (uint32_t)(gs.frame[ctx] & 0x1FF);
            if (kick && sGpu && ((gs.frame[ctx] >> 16) & 0x3F) == 1 && gGsPageGen[fb] != 0) {
                draw_sprite(target_get(fb), ctx, &gs.vtx[0], &gs.vtx[1]);
                gGsPageGen[fb]++;
                gGsPageGen[(fb + 1) & 511]++;
                gGsPageSig[fb]++;
                gGsPageSig[(fb + 1) & 511]++;
#ifdef PORT_VITA
                gGsEpoch++; /* (the renderer's keys and states remembered by the epoch) */
                { extern unsigned gT121Ep[8]; gT121Ep[0]++; }
                { extern unsigned gVitaPaints; gVitaPaints++; }
#endif
            } else if (kick) {
                if (sGpu) { GsGpu_Draw(6, ctx, gs.vtx); } else { draw_sprite(sf, ctx, &gs.vtx[0], &gs.vtx[1]); }
            }
            gs.vcount = 0;
        }
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ registers */

#ifdef PORT_VITA
extern volatile unsigned char gProf; /* vita_vgl.c: where the time goes */
extern volatile int gVitaProfMeasure;
extern void Vita_ProfSwitch(int n);
#define VPROF(n) ((gProf = (n)), (gVitaProfMeasure ? Vita_ProfSwitch(n) : (void)0))
#else
#define VPROF(n) ((void)0)
#endif
static void transfer_begin(void) {
    if (sDumpFrame == (int)sFrame && (gs.trxdir & 3) != 0) {
        fprintf(stderr, "  transfer dir %u: sbp %04x sbw %u spsm %02x -> dbp %04x dbw %u dpsm %02x, from %u,%u to %u,%u size %ux%u\n",
                (unsigned)(gs.trxdir & 3), (unsigned)(gs.bitbltbuf & 0x3FFF), (unsigned)((gs.bitbltbuf >> 16) & 0x3F),
                (unsigned)((gs.bitbltbuf >> 24) & 0x3F), (unsigned)((gs.bitbltbuf >> 32) & 0x3FFF), (unsigned)((gs.bitbltbuf >> 48) & 0x3F),
                (unsigned)((gs.bitbltbuf >> 56) & 0x3F), (unsigned)(gs.trxpos & 0x7FF), (unsigned)((gs.trxpos >> 16) & 0x7FF),
                (unsigned)((gs.trxpos >> 32) & 0x7FF), (unsigned)((gs.trxpos >> 48) & 0x7FF), (unsigned)(gs.trxreg & 0xFFF),
                (unsigned)((gs.trxreg >> 32) & 0xFFF));
    }
    gs.transfer = (gs.trxdir & 3) == 0; /* only host -> GS */
    gs.tx = (gs.trxpos >> 32) & 0x7FF;
    gs.ty = (gs.trxpos >> 48) & 0x7FF;
    gs.tw = gs.trxreg & 0xFFF;
    gs.th = (gs.trxreg >> 32) & 0xFFF;
    gs.tpos = 0;
    if (VITA_RARE(sDumpFrame == (int)sFrame)) {
        fprintf(stderr, "  upload dbp %04x dbw %u psm %02x at %u,%u size %ux%u\n", (unsigned)((gs.bitbltbuf >> 32) & 0x3FFF),
                (unsigned)((gs.bitbltbuf >> 48) & 0x3F), (unsigned)((gs.bitbltbuf >> 56) & 0x3F), gs.tx, gs.ty, gs.tw, gs.th);
    }
}

#ifdef PORT_VITA
#ifndef UP_BY_BLOCK
#define UP_BY_BLOCK 1 /* (the test of this code against the pixel-by-pixel reference switches it) */
#endif
#ifndef UP_LAZY_MIN
#define UP_LAZY_MIN 2048 /* bytes: a palette or a single small block is written faster than it is kept account of */
#endif
/* Whole block rows of an upload: the rectangle (x0, y0, w, h) of the buffer, made of whole blocks, from `src`. */
static void up_piece(uint32_t bp, uint32_t bw, uint32_t psm, int bits, uint32_t gw, uint32_t gh, uint32_t x0, uint32_t y0,
                     uint32_t w, uint32_t h, const uint8_t *src) {
    uint32_t stride = w * (uint32_t)bits / 8, bx, by;
    if (sLazy && stride < 65536 && stride * h >= UP_LAZY_MIN) { /* taken note of, not written (see sUp) */
        uint32_t len = stride * h, n = 0;
        uint64_t hash = up_hash_quick(src, len);
        int e = up_get(hash, len, src);

        if (e >= 0) {
            uint32_t lo = (uint32_t)hash ^ (uint32_t)bits << 24 ^ stride, hi = (uint32_t)(hash >> 32);
            for (by = 0; by < h; by += gh) {
                for (bx = 0; bx < w; bx += gw, n++) {
                    Blk *k = &sBlk[vram_rw(bp, bw, psm, x0 + bx, y0 + by, 2, 0)];
                    uint32_t id = (lo + n * 0x9E3779B1u) * 0x85EBCA6Bu;
                    id = (id ^ id >> 15 ^ hi) * 0xC2B2AE35u;
                    k->id = id ^ id >> 13;
                    k->off = by * stride + bx * (uint32_t)bits / 8;
                    k->up = (uint16_t)(e + 1);
                    k->stride = (uint16_t)stride;
                    k->bits = (uint8_t)bits;
                    k->dirty = 0;
                    plane_bump((uint32_t)(k - sBlk), 7);
                }
            }
            gVitaUp[2] += w * h;
            return;
        }
    }
    for (by = 0; by < h; by += gh) {
        for (bx = 0; bx < w; bx += gw) {
            uint32_t b = vram_rw(bp, bw, psm, x0 + bx, y0 + by, 2, 0);
            sBlk[b].up = 0; /* (whatever waited for this block is overwritten whole) */
            sBlk[b].dirty = 1;
            plane_bump(b, 7);
            block_write(b, bits, src + by * stride + bx * (uint32_t)bits / 8, stride);
        }
    }
    gVitaUp[1] += w * h;
}

/* The pixels `from` .. `to` of the data at `p` to where the upload has got to (gs.tpos), a run of a row
   inside one block at a time: the block's place is worked out once per run (8 to 32 pixels) and the pixels go
   straight to their places in it. Pixel by pixel (vram_rw for each, below) it was some 350 cycles a pixel: the
   fighters' intros send 35,000 such pixels a picture (textures in the top bits of 32-bit pixels, or not on the
   block grid), 30 ms of the drawing thread's 33 (Planet Namek). */
#ifndef UP_RUNS
#define UP_RUNS 1
#endif
static inline uint32_t up_src(const uint8_t *p, uint32_t bits, uint32_t i) {
    switch (bits) {
    case 32: return p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    case 24: return p[i * 3] | p[i * 3 + 1] << 8 | p[i * 3 + 2] << 16;
    case 16: return p[i * 2] | p[i * 2 + 1] << 8;
    case 8: return p[i];
    default: return (p[i / 2] >> ((i & 1) * 4)) & 15;
    }
}
static void up_pixels(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t bits, const uint8_t *p, uint32_t from, uint32_t to) {
    uint32_t px, py, c, i;

    if (from >= to) {
        return;
    }
    px = gs.tpos % gs.tw; /* (no divide instruction on this processor: the position is counted along) */
    py = gs.tpos / gs.tw;
    gVitaUp[0] += to - from;
    if (UP_RUNS) {
        uint32_t base = (dpsm & 0x30) == 0x30 ? dpsm & 0x0F : dpsm, gw, gh;
        psm_block(dpsm, &gw, &gh);
        for (i = from; i < to;) {
            uint32_t gx = gs.tx + px, gy = gs.ty + py, run = gw - (gx & (gw - 1)), k, yy = gy & (gh - 1), b;
            uint32_t *d;
            if (run > gs.tw - px) {
                run = gs.tw - px;
            }
            if (run > to - i) {
                run = to - i;
            }
            b = vram_rw(dbp, dbw, dpsm, gx, gy, 2, 0) & 0x3FFF;
            if (sBlk[b].up != 0) {
                blk_real(b);
            }
            sBlk[b].dirty = 1;
            plane_bump(b, psm_planes(dpsm));
            d = &sVram[b * 64];
            switch (base) {
            case 0x02: case 0x0A:
                for (k = 0; k < run; k++) { ((uint16_t *)d)[kCol16[yy][(gx + k) & 15]] = (uint16_t)up_src(p, bits, i + k); }
                break;
            case 0x13:
                for (k = 0; k < run; k++) { ((uint8_t *)d)[sCol8[yy][(gx + k) & 15]] = (uint8_t)up_src(p, bits, i + k); }
                break;
            case 0x14:
                for (k = 0; k < run; k++) {
                    uint32_t n = sCol4[yy][(gx + k) & 31];
                    uint8_t *q = &((uint8_t *)d)[n >> 1];
                    *q = (uint8_t)((*q & ~(0xFu << ((n & 1) * 4))) | (up_src(p, bits, i + k) & 15) << ((n & 1) * 4));
                }
                break;
            case 0x01:
                for (k = 0; k < run; k++) { uint32_t *w = &d[kCol32[yy][(gx + k) & 7]]; *w = (*w & 0xFF000000u) | (up_src(p, bits, i + k) & 0xFFFFFF); }
                break;
            case 0x1B:
                for (k = 0; k < run; k++) { uint32_t *w = &d[kCol32[yy][(gx + k) & 7]]; *w = (*w & 0x00FFFFFFu) | up_src(p, bits, i + k) << 24; }
                break;
            case 0x24:
                for (k = 0; k < run; k++) { uint32_t *w = &d[kCol32[yy][(gx + k) & 7]]; *w = (*w & 0xF0FFFFFFu) | (up_src(p, bits, i + k) & 15) << 24; }
                break;
            case 0x2C:
                for (k = 0; k < run; k++) { uint32_t *w = &d[kCol32[yy][(gx + k) & 7]]; *w = (*w & 0x0FFFFFFFu) | (up_src(p, bits, i + k) & 15) << 28; }
                break;
            default:
                for (k = 0; k < run; k++) { d[kCol32[yy][(gx + k) & 7]] = up_src(p, bits, i + k); }
                break;
            }
            i += run;
            gs.tpos += run;
            px += run;
            if (px == gs.tw) {
                px = 0;
                py++;
            }
        }
        (void)c;
        return;
    }
    for (i = from; i < to; i++, gs.tpos++) {
        switch (bits) {
        case 32: c = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24; break;
        case 24: c = p[i * 3] | p[i * 3 + 1] << 8 | p[i * 3 + 2] << 16; break;
        case 16: c = p[i * 2] | p[i * 2 + 1] << 8; break;
        case 8: c = p[i]; break;
        default: c = (p[i / 2] >> ((i & 1) * 4)) & 15; break;
        }
        vram_rw(dbp, dbw, dpsm, gs.tx + px, gs.ty + py, 1, c);
        if (++px == gs.tw) {
            px = 0;
            py++;
        }
    }
}

extern volatile int gVitaZoneOn; /* (vita_vgl.c: the zones of a measured list) */
extern void Vita_Z(int z);
static void transfer_data_(const uint8_t *p, uint32_t bytes);
static void transfer_data(const uint8_t *p, uint32_t bytes) {
    unsigned long long t;

    if (bytes < 2048) { /* (palettes, the film's blocks: reading the clock would cost as much as they do) */
        transfer_data_(p, bytes);
        return;
    }
    t = sceKernelGetProcessTimeWide();
    transfer_data_(p, bytes);
    gVitaUsUpload += (unsigned)(sceKernelGetProcessTimeWide() - t);
}
#define transfer_data transfer_data_
#endif
static void transfer_data(const uint8_t *p, uint32_t bytes) {
    uint32_t dbp = (gs.bitbltbuf >> 32) & 0x3FFF, dbw = (gs.bitbltbuf >> 48) & 0x3F, dpsm = (gs.bitbltbuf >> 56) & 0x3F;
    uint32_t bits, total, i;
#ifdef PORT_VITA
    void *sm = NULL; /* (a small upload remembered: below) */
    int smallUp = 0;  /* (the log's zones: a small upload) */
#endif

    if (!gs.transfer || gs.tw == 0) {
        return;
    }
#ifdef PORT_VITA
    if (gVitaUpCheck && gs.tpos == 0 && bytes >= 64) { /* an upload read in place whose bytes are not what they were the last time */
        extern int Port_InGr(const void *p);
        extern void Vita_Log(const char *fmt, ...);
        static struct { const uint8_t *p; uint32_t bytes, frame; uint64_t h; } t[4096];
        static unsigned lines;
        uint32_t k = ((uint32_t)(uintptr_t)p * 0x9E3779B1u ^ bytes) >> 20;
        if (Port_InGr(p)) {
            gVitaUpDyn2[0]++;
        } else {
            uint64_t h = up_hash(p, bytes);
            gVitaUpDyn2[1]++;
            if (t[k].p == p && t[k].bytes == bytes && t[k].h != h) {
                gVitaUpDyn2[2]++;
                if (lines < 120) {
                    lines++;
                    Vita_Log("gs: upload read in place, %u bytes at %p to %04x (psm %02x, %u x %u), not as in list %u; list %u\n", bytes, (const void *)p, (unsigned)dbp,
                             (unsigned)dpsm, (unsigned)gs.tw, (unsigned)gs.th, t[k].frame, sFrame);
                }
            }
            t[k].p = p;
            t[k].bytes = bytes;
            t[k].h = h;
            t[k].frame = sFrame;
        }
    }
#endif
    bits = (uint32_t)Gs_PsmBits(dpsm);
    total = gs.tw * gs.th;
    /* A picture uploaded straight into a display buffer (the movies: page 0, 512 wide, the second buffer 448 lines
       down): the GPU back end has to copy it from GS memory into that buffer's texture at the end of the frame. */
    if (dbp == 0 && dbw == 8 && bits == 32) {
        unsigned bit = gs.ty < 448 ? 1u : 2u;
        if (!(gGsFbUploads & bit) && sGpu) {
            GsGpu_FbUpload(bit == 2); /* its place among the frame's draws: after the clear, before a fade */
        }
        gGsFbUploads |= bit;
#ifdef PORT_VITA
        {   /* the film's picture, which the renderer has straight from the decoder (plat_movie.c): not written */
            extern int Vita_MovieDirect(const void *p);
            if (sGpu && Vita_MovieDirect(p)) {
                uint32_t n = bytes / 4;
                gs.tpos = n < total - gs.tpos ? gs.tpos + n : total;
                gVitaUp[4]++;
                return;
            }
        }
#endif
    }
#ifdef PORT_VITA
    {
        uint32_t gw, gh, n = (bytes * 8 + bits - 1) / bits, done = 0; /* (rounded up for 24 bits, as below) */

        if (gs.tpos >= total) {
            return;
        }
        if (n > total - gs.tpos) {
            n = total - gs.tpos;
        }
        /* A frame that is not drawn: an upload that was carried out in one of the last frames from the same place
           in the game's memory to the same place in GS memory is not looked at again (the game sends every texture
           every frame; reading 250 KB of them to find them unchanged was a third of such a frame). The next frame
           that is drawn looks at all of them as before. */
        if (sLazy && gs.tpos == 0 && n == total && bytes >= 256) {
            extern int gVitaGsSkip;
            extern unsigned gVitaUpSkipped;
            static struct { const uint8_t *p; uint64_t bb; uint32_t pos, size, bytes; unsigned frame; } seen[1024], *e;
            uint32_t pos = gs.tx | gs.ty << 16, size = gs.tw | gs.th << 16;

            e = &seen[(uint32_t)(((uint32_t)(uintptr_t)p ^ (uint32_t)(gs.bitbltbuf >> 32) * 0x85EBCA6Bu ^ pos * 31u) * 0x9E3779B1u) >> 22];
            if (e->p == p && e->bb == gs.bitbltbuf && e->pos == pos && e->size == size && e->bytes == bytes) {
                if (gVitaGsSkip && sFrame - e->frame <= 8) {
                    gs.tpos = total;
                    gVitaUpSkipped++;
                    return;
                }
            } else {
                e->p = p; e->bb = gs.bitbltbuf; e->pos = pos; e->size = size; e->bytes = bytes;
            }
            if (!gVitaGsSkip) {
                e->frame = sFrame;
            } else if (e->frame == 0 || sFrame - e->frame > 8) {
                e->frame = sFrame - 9; /* (carried out now, in a frame not drawn: looked at again the next time) */
            }
        }
        gVitaUp[3]++;
        psm_block(dpsm, &gw, &gh);
        /* A small upload (a palette: 64 bytes or 1 KB, before each piece of a fighter, every frame) the same as the
           last one to the same place, which GS memory still holds: nothing to write. Written, it counted as a change
           of GS memory (the epoch, the pages' numbers), and the drawing state and texture of every strip after it
           were worked out anew (600 cycles a strip, 4,000 strips a picture). Compared byte for byte. */
        /* (256 places, was 64: a test had 80 small uploads a list "seen for the first time", the others
           having pushed them out) */
        static struct { uint64_t bb; uint32_t pos, size, bytes, rect; int valid; uint32_t miss; uint8_t data[1024]; } smemo[256];
        if (sLazy && gs.tpos == 0 && n == total && bytes <= 1024 && bytes >= 16) {
            uint32_t pos = gs.tx | gs.ty << 16, size = gs.tw | gs.th << 16;
            smallUp = 1;
            struct { uint64_t bb; uint32_t pos, size, bytes, rect; int valid; uint32_t miss; uint8_t data[1024]; } *m =
                (void *)&smemo[((((uint32_t)(gs.bitbltbuf >> 32) * 0x9E3779B1u) ^ pos * 0x85EBCA6Bu) * 0x27D4EB2Fu) >> 24];
            if (m->valid && m->bb == gs.bitbltbuf && m->pos == pos && m->size == size && m->bytes == bytes && memcmp(m->data, p, bytes) == 0 &&
                up_rect_id(dbp, dbw, dpsm, gs.tx, gs.ty, gs.tw, gs.th) == m->rect) {
                uint32_t rowbytes = (dbw ? dbw : 1) * 64 * bits / 8, first = dbp / 32 + (gs.ty * rowbytes) / 8192;
                uint32_t last = dbp / 32 + ((gs.ty + gs.th) * rowbytes + 8191) / 8192, pg;
                gs.tpos = total;
                gVitaUp[2] += total;
                for (pg = first; pg <= last; pg++) { /* (as the large uploads found unchanged: the page holds uploaded data again) */
                    gGsPageGen[pg & 511]++;
                }
                return;
            }
            { extern unsigned gT121Sm[4]; gT121Sm[!m->valid ? 0 : (m->bb != gs.bitbltbuf || m->pos != pos || m->size != size || m->bytes != bytes) ? 1 : memcmp(m->data, p, bytes) != 0 ? 2 : 3]++; }
            /* a place whose data is another at every upload (a fighter's palettes, one place for all of its
               pieces: 150 a picture) is not remembered, but one time in eight: no copy, no comparing, and no number
               for its rectangle (2,500 cycles an upload, and none of them ever found unchanged) */
            m->miss = m->bb == gs.bitbltbuf && m->pos == pos && m->size == size && m->bytes == bytes ? m->miss + 1 : 0;
            m->valid = 0; /* (until written: its rectangle's number is taken after) */
            m->bb = gs.bitbltbuf; m->pos = pos; m->size = size; m->bytes = bytes;
            if (m->miss < 4 || (m->miss & 7) == 0) {
                memcpy(m->data, p, bytes);
                sm = (void *)m;
            }
        }
        if (gVitaZoneOn) { Vita_Z(44); } /* (the small ones' memory looked at) */
        /* An upload that cannot go by the block (a format inside a 32-bit pixel, 24 bits, a place off the block
           grid) and comes whole: the scenes before a fight send 150,000 such pixels every frame, 50 ms of one
           pixel after the other. If the same data went to the same place the last time and that place still holds
           what it held after it, there is nothing to write. */
        /* (now for uploads that go by the block too: the game sends every texture every drawn frame, and taking
           each one apart block by block to find it unchanged was 2 million cycles a picture) */
        int byBlock = (dpsm == 0x00 || dpsm == 0x02 || dpsm == 0x0A || dpsm == 0x13 || dpsm == 0x14) && !((gs.tx | gs.tw) & (gw - 1)) && !(gs.ty & (gh - 1)) &&
                      n >= gs.tw * gh && UP_BY_BLOCK;
        static struct { uint64_t bb, hash; uint32_t x, y, w, h, rect; int valid; } memo[256];
        /* the slot of this upload: it was declared static with the table, so it kept the last large upload's
           slot from one call to the next, and every upload after it that does not go by the block took the large
           uploads' way: the palettes (64 bytes, 130 a picture on Kame House) were never found unchanged and each was
           written again, 5,500 cycles, and counted as a change of GS memory */
        void *mm = NULL;
        int memoWhy = 0;
        if (sLazy && gs.tpos == 0 && n == total && bytes >= 2048) {
            static struct { uint64_t bb, hash; uint32_t x, y, w, h, rect; int valid; } *m;
            extern volatile int gVitaZoneOn;
            extern void Vita_Z(int z);
            uint64_t hash;
            if (gVitaZoneOn) { Vita_Z(7); }
            hash = up_hash_quick(p, (total * bits + 7) / 8);
            if (gVitaZoneOn) { Vita_Z(16); } /* (the upload's bytes looked at) */

            m = (void *)&memo[(uint32_t)((hash ^ gs.bitbltbuf >> 32 ^ gs.tx * 31u ^ gs.ty * 131u) * 0x9E3779B1u) >> 24];
            memoWhy = !m->valid ? 0 : (m->bb != gs.bitbltbuf || m->x != gs.tx || m->y != gs.ty || m->w != gs.tw || m->h != gs.th) ? 2 : m->hash != hash ? 1 : 3;
            if (memoWhy == 3 && up_rect_id(dbp, dbw, dpsm, gs.tx, gs.ty, gs.tw, gs.th) == m->rect) {
                gs.tpos = total;
                gVitaUp[2] += total;
                {   /* Nothing to write, but an upload all the same: a page the game DREW into (a work buffer, on the
                       chip, not in GS memory) holds the texture again after it. Without this the renderer took the
                       page for the drawn-into buffer still and left out what has that texture (parts of a fighter,
                       now and then). The texture keys follow gGsPageSig and are not made again for it. */
                    uint32_t rowbytes = (dbw ? dbw : 1) * 64 * bits / 8, first = dbp / 32 + (gs.ty * rowbytes) / 8192;
                    uint32_t last = dbp / 32 + ((gs.ty + gs.th) * rowbytes + 8191) / 8192, pg;
                    for (pg = first; pg <= last; pg++) {
                        gGsPageGen[pg & 511]++;
                    }
                }
                if (gVitaZoneOn) { Vita_Z(12); } /* (its place in GS memory found holding it still) */
                return;
            }
            m->bb = gs.bitbltbuf; m->hash = hash; m->x = gs.tx; m->y = gs.ty; m->w = gs.tw; m->h = gs.th;
            m->valid = 0; /* (until written: its rectangle's number is taken after) */
            mm = (void *)m;
        }
        if (gVitaZoneOn) { Vita_Z(45); } /* (the large ones' memory looked at) */
        if (mm != NULL && !byBlock) {
            struct { uint64_t bb, hash; uint32_t x, y, w, h, rect; int valid; } *m = (void *)mm;
            up_top_add(dpsm, gs.tx, gs.ty, gs.tw, gs.th, memoWhy);
            up_pixels(dbp, dbw, dpsm, bits, p, 0, n);
            gGsEpoch++;
            { extern unsigned gT121Ep[8]; gT121Ep[1]++; }
            m->rect = up_rect_id(dbp, dbw, dpsm, gs.tx, gs.ty, gs.tw, gs.th);
            m->valid = 1;
            {   /* (the pages written: the renderer's texture keys follow them; gGsPageGen is not bumped here, as before:
                   gs_core.c's own tests on it stay as they were) */
                uint32_t rowbytes = (dbw ? dbw : 1) * 64 * bits / 8, first = dbp / 32 + (gs.ty * rowbytes) / 8192;
                uint32_t last = dbp / 32 + ((gs.ty + gs.th) * rowbytes + 8191) / 8192;
                for (i = first; i <= last; i++) {
                    gGsPageSig[i & 511]++;
                }
            }
            if (gVitaZoneOn) { Vita_Z(46); }
            return;
        }
        /* the whole block rows in this piece of the data by the block; what is in front of them and behind them
           (data that arrives in pieces, a height that is not a multiple of a block's) pixel by pixel */
        if ((dpsm == 0x00 || dpsm == 0x02 || dpsm == 0x0A || dpsm == 0x13 || dpsm == 0x14) && !((gs.tx | gs.tw) & (gw - 1)) &&
            !(gs.ty & (gh - 1)) && n >= gs.tw * gh && UP_BY_BLOCK) {
            uint32_t rowpix = gs.tw * gh, head = gs.tpos % rowpix, row, rows;

            head = head != 0 ? rowpix - head : 0; /* pixels up to the next block row */
            if (head < n) {
                row = (gs.tpos + head) / gs.tw;
                rows = (n - head) / rowpix;
                if (rows > (gs.th - row) / gh) {
                    rows = (gs.th - row) / gh;
                }
                if (rows > 0) {
                    up_pixels(dbp, dbw, dpsm, bits, p, 0, head);
                    up_piece(dbp, dbw, dpsm, (int)bits, gw, gh, gs.tx, gs.ty + row, gs.tw, rows * gh, p + (size_t)head * bits / 8);
                    gs.tpos += rows * rowpix;
                    done = head + rows * rowpix;
                }
            }
        }
        up_pixels(dbp, dbw, dpsm, bits, p, done, n);
        if (gVitaZoneOn && smallUp) { Vita_Z(40); } /* (a small upload's pixels written) */
        gGsEpoch++;
        { extern unsigned gT121Ep[8]; gT121Ep[bytes < 2048 ? 2 : 3]++; if (bytes < 2048 && sm == NULL) { gT121Ep[gs.tpos != 0 || n != total ? 4 : bytes < 16 ? 5 : 6]++; } }
        if (mm != NULL) {
            struct { uint64_t bb, hash; uint32_t x, y, w, h, rect; int valid; } *m = (void *)mm;
            m->rect = up_rect_id(dbp, dbw, dpsm, gs.tx, gs.ty, gs.tw, gs.th);
            m->valid = 1;
        }
    }
#else
    for (i = 0; i * bits < bytes * 8 && gs.tpos < total; gs.tpos++, i++) {
        uint32_t x = gs.tx + gs.tpos % gs.tw, y = gs.ty + gs.tpos / gs.tw, c;

        switch (bits) {
        case 32: c = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24; break;
        case 24: c = p[i * 3] | p[i * 3 + 1] << 8 | p[i * 3 + 2] << 16; break;
        case 16: c = p[i * 2] | p[i * 2 + 1] << 8; break;
        case 8: c = p[i]; break;
        default: c = (p[i / 2] >> ((i & 1) * 4)) & 15; break;
        }
        vram_rw(dbp, dbw, dpsm, x, y, 1, c);
    }
#endif
    /* which pages changed: every page the rectangle can touch (pages are 8 KB = 32 blocks) */
    {
        uint32_t rowbytes = (dbw ? dbw : 1) * 64 * bits / 8, first = dbp / 32 + (gs.ty * rowbytes) / 8192;
        uint32_t last = dbp / 32 + ((gs.ty + gs.th) * rowbytes + 8191) / 8192;
        for (i = first; i <= last; i++) {
            gGsPageGen[i & 511]++;
            gGsPageSig[i & 511]++;
        }
    }
#ifdef PORT_VITA
    if (gVitaZoneOn && smallUp) { Vita_Z(41); } /* (a small upload's pages) */
    if (sm != NULL && gs.tpos >= total) { /* (the small upload above, written whole: what GS memory holds after it) */
        struct { uint64_t bb; uint32_t pos, size, bytes, rect; int valid; uint32_t miss; uint8_t data[1024]; } *m = sm;
        m->rect = up_rect_id(dbp, dbw, dpsm, gs.tx, gs.ty, gs.tw, gs.th);
        m->valid = 1;
    }
    if (gVitaZoneOn) { Vita_Z(smallUp ? 42 : 46); } /* (written; 216: a small one's rectangle's number, 42) */
#endif
}
#undef transfer_data

/* PRMODECONT.AC = 1: the attributes (shading, texturing, fogging, blending, ...) are PRIM's own; AC = 0: they come
   from PRMODE and PRIM only gives the primitive type. The game switches to PRMODE for its shadow passes. */
__attribute__((hot)) static void prim_update(void) {
    gs.prim = (gs.prmodecont & 1) ? gs.primRaw : (gs.primRaw & 7) | (gs.prmode & 0x7F8);
}

/* The GS's CLUT buffer: a texture with a palette reads its colours from the buffer, which a TEX0 or TEX2
   write with CLD set fills from GS memory at its CBP. A draw whose TEX0 leaves CLD 0 uses what was loaded last,
   whatever its own CBP says (the effects' speed lines: CBP 0, the picture's own memory). Followed here: the TEX0
   fields of the last load (vita_vgl.c reads the palette from there when a draw's CLD is 0 and its CBP 0). */
volatile uint64_t gGsClutT0;  /* CBP, CPSM, CSM, CSA of the last CLUT load (0: none yet) */
unsigned gGsClutLoads;
static uint32_t sClutCbp0 = 0xFFFFFFFFu, sClutCbp1 = 0xFFFFFFFFu;
static inline void clut_note(uint64_t v) {
    uint32_t cld = (uint32_t)(v >> 61) & 7, cbp = (uint32_t)(v >> 37) & 0x3FFF;
    int load = 0;
    switch (cld) {
    case 1: load = 1; break;
    case 2: load = 1; sClutCbp0 = cbp; break;
    case 3: load = 1; sClutCbp1 = cbp; break;
    case 4: if (cbp != sClutCbp0) { load = 1; sClutCbp0 = cbp; } break;
    case 5: if (cbp != sClutCbp1) { load = 1; sClutCbp1 = cbp; } break;
    default: break;
    }
    if (load) {
        gGsClutT0 = (v & 0x1FFFFFE000000000ull) | 1; /* (bit 0: set, so that a load at CBP 0 is told from none) */
        gGsClutLoads++;
    }
}
uint32_t gGsMarkerArg; /* (Vita) the upper word of the port's marker (gs_marker.c, Port_GsMarkerArg) */
/* A count of the GS's state changing: every write of a register that is not a vertex's own (RGBAQ,
   ST, UV, the XYZs, FOG) with a value other than the register holds, and every write of TEXFLUSH, the transfer
   registers and the port's marker. vita_vgl.c draws a GS triangle the quick way when nothing has changed since the
   triangle before it (its state as that one's). */
uint32_t gGsStateGen;
uint32_t gGsStateGenX; /* the same count without PRIM and TEX0_1/2: the sea's runs take turns between a few TEX0s */
static uint64_t sRegShadow[128];
__attribute__((hot)) static void reg_write(uint32_t addr, uint64_t d) {
    sStat[4]++;
#ifdef PORT_VITA
    if (sDpCap) {
        dp_note(addr);
    }
    if ((addr - 1u) > 4u && addr != 0x0A && addr != 0x0C && addr != 0x0D) {
        if (addr == 0x3F || addr >= 0x50 || sRegShadow[addr] != d) {
            sRegShadow[addr & 127] = d;
            gGsStateGen++;
            gGsStateGenX += addr != 0x00 && addr != 0x06 && addr != 0x07;
        }
    }
#endif
    if (VITA_RARE(sDumpFrame == (int)sFrame) && (addr == 0x4C || addr == 0x4D || addr == 0x18 || addr == 0x19 || addr == 0x40 || addr == 0x41 ||
                                       addr == 0x4E || addr == 0x4F || addr == 0x1A || addr == 0x1B)) {
        fprintf(stderr, "  reg %02x = %llx\n", addr, (unsigned long long)d);
    }
    switch (addr) {
    case 0x00: gs.primRaw = d; prim_update(); gs.vcount = 0; gs.strip = 0; break;
    case 0x01: gs.rgbaq = d; { uint32_t q = (uint32_t)(d >> 32); memcpy(&gs.q, &q, 4); } break;
    case 0x02: gs.st = d; break;
    case 0x03: gs.uv = d; break;
    case 0x04: vertex(d & 0xFFFF, (d >> 16) & 0xFFFF, (uint32_t)((d >> 32) & 0xFFFFFF), 1); break;
    case 0x05: vertex(d & 0xFFFF, (d >> 16) & 0xFFFF, (uint32_t)(d >> 32), 1); break;
    case 0x06: case 0x07: gs.tex0[addr - 6] = d; if (d >> 61) { clut_note(d); } break;
    case 0x08: case 0x09: gs.clamp[addr - 8] = d; break;
    case 0x0C: vertex(d & 0xFFFF, (d >> 16) & 0xFFFF, (uint32_t)((d >> 32) & 0xFFFFFF), 0); break;
    case 0x0D: vertex(d & 0xFFFF, (d >> 16) & 0xFFFF, (uint32_t)(d >> 32), 0); break;
    case 0x14: case 0x15: gs.tex1[addr - 0x14] = d; break;
#ifdef PORT_VITA
    case 0x34: case 0x35: case 0x36: case 0x37: gGsMipTbp[addr - 0x34] = d; gGsMipWrites++; break; /* MIPTBP1/2 */
#endif
    case 0x16: case 0x17: gs.tex0[addr - 0x16] = (gs.tex0[addr - 0x16] & ~0x1FFFFFE003F00000ull) | (d & 0x1FFFFFE003F00000ull); if (d >> 61) { clut_note(d); } break; /* TEX2: PSM and CLUT fields */
    case 0x18: case 0x19: gs.xyoffset[addr - 0x18] = d; break;
    case 0x1A: gs.prmodecont = d; prim_update(); break;
    case 0x1B: gs.prmode = d; prim_update(); break;
    case 0x3B: gs.texa = d; break;
    case 0x40: case 0x41: gs.scissor[addr - 0x40] = d; break;
    case 0x42: case 0x43: gs.alpha[addr - 0x42] = d; break;
    case 0x47: case 0x48: gs.test[addr - 0x47] = d; break;
    case 0x4A: case 0x4B: gs.fba[addr - 0x4A] = d & 1; break;
    case 0x4C: case 0x4D: gs.frame[addr - 0x4C] = d; break;
    case 0x4E: case 0x4F: gs.zbuf[addr - 0x4E] = d; break;
    case 0x50: gs.bitbltbuf = d; break;
    case 0x51: gs.trxpos = d; break;
    case 0x52: gs.trxreg = d; break;
    case 0x53: gs.trxdir = d; transfer_begin(); break;
    case 0x7F: if (sGpu) { gGsMarkerArg = (uint32_t)(d >> 32); GsGpu_Native((int)d); } break; /* not a GS register: the port's marker (gs_marker.c) */
    default: break;
    }
}

__attribute__((hot)) void Gs_RegWrite(uint32_t addr, uint64_t d) {
    reg_write(addr, d);
}

int GsGpu_Enabled(void) {
    return sGpu;
}

/* ------------------------------------------------------------------------------------------------ GIF */

/* One run of GIF data (whole quadwords). */
#ifdef PORT_VITA
extern volatile int gVitaZoneOn;
extern void Vita_Z(int z);
#define GZM2(z) do { if (gVitaZoneOn) { Vita_Z(z); } } while (0)
#else
#define GZM2(z) ((void)0)
#endif
#ifdef PORT_VITA
int gVitaTriRun = 1;   /* bt3.txt trirun=0: those packets register by register, as before */
extern volatile int gVitaZoneOn;
extern int scePerfArmPmonGetCounterValue(int thid, unsigned int counter, unsigned int *value);
unsigned long long gVitaTriRunCyc[2]; /* cycles reading the runs' packets into vertices, triangles */
unsigned long long gVitaGifCyc[2]; /* lists measured part by part: cycles in the triangle runs, in the images */
unsigned gVitaGifCnt[2];
unsigned gVitaGifN[6]; /* GIF tags PACKED, REGLIST, IMAGE; A+D writes; REGLIST registers outside the runs; PACKED registers */
unsigned gVitaTriRunN[2]; /* (the log) runs, triangles in them */
/* n packets of 7 quadwords at q (tag, register list, PRIM, TEX0, 3 x (RGBAQ, ST, XYZF2), NOP), the same PRIM and TEX0 */
static void tri_run(const uint64_t *q, uint32_t n) {
    extern int gVitaGsSkip;
    extern void GsGpu_DrawTriRun(int ctx, const GsVertex *v, uint32_t ntri);
    static GsVertex vb[3 * 64];
    uint32_t i, j, k, texreg = (uint32_t)((q[1] >> 4) & 0xF);
    int ctx, type;
    float ox, oy;
    gVitaTriRunN[0]++;
    gVitaTriRunN[1] += n;
    sStat[3] += n;
    reg_write(0, q[2]);
    reg_write(texreg, q[3]);
    ctx = (int)((gs.prim >> 9) & 1);
    type = (int)(gs.prim & 7);
    if (type != 3) { /* (another kind of primitive: the GS's own way, vertex by vertex) */
        for (i = 0; i < n; i++, q += 14) {
            if (i > 0) {
                reg_write(0, q[2]);
                reg_write(texreg, q[3]);
            }
            for (k = 0; k < 3; k++) {
                reg_write(1, q[4 + 3 * k]);
                reg_write(2, q[5 + 3 * k]);
                reg_write(4, q[6 + 3 * k]);
            }
        }
        return;
    }
    ox = (float)(gs.xyoffset[ctx] & 0xFFFF);
    oy = (float)((gs.xyoffset[ctx] >> 32) & 0xFFFF);
    for (i = 0; i < n; i += 64) {
        uint32_t m = n - i < 64 ? n - i : 64, c0 = 0, c1;
        if (gVitaZoneOn & 1) {
            scePerfArmPmonGetCounterValue(0, 31, &c0);
        }
        for (j = 0; j < m; j++) {
            const uint64_t *pk = q + 14 * (i + j);
            GsVertex *v = &vb[3 * j];
            if (i + j + 3 < n) { /* the packet three ahead on its way into the cache */
                const char *ahead = (const char *)(pk + 14 * 3);
                __builtin_prefetch(ahead);
                __builtin_prefetch(ahead + 32);
                __builtin_prefetch(ahead + 64);
                __builtin_prefetch(ahead + 96);
            }
            if (i + j > 0) { /* PRIM and TEX0 again, with the values they hold: the vertex queue emptied, the palette loaded again */
                sStat[4] += 2;
                if (pk[3] >> 61) {
                    clut_note(pk[3]);
                }
            }
            sStat[4] += 9;
            sStat[5] += 3;
            for (k = 0; k < 3; k++) {
                uint64_t c = pk[4 + 3 * k], st = pk[5 + 3 * k], xyz = pk[6 + 3 * k];
                uint32_t qq = (uint32_t)(c >> 32), tt = (uint32_t)(st >> 32), ss = (uint32_t)st;
                v[k].x = ((float)(int)(uint32_t)(xyz & 0xFFFF) - ox) / 16.0f;
                v[k].y = ((float)(int)(uint32_t)((xyz >> 16) & 0xFFFF) - oy) / 16.0f;
                v[k].z = (uint32_t)((xyz >> 32) & 0xFFFFFF);
                v[k].r = (uint8_t)c; v[k].g = (uint8_t)(c >> 8); v[k].b = (uint8_t)(c >> 16); v[k].a = (uint8_t)(c >> 24);
                memcpy(&v[k].s, &ss, 4);
                memcpy(&v[k].t, &tt, 4);
                memcpy(&v[k].q, &qq, 4);
                v[k].u = (int)(gs.uv & 0x3FFF);
                v[k].v = (int)((gs.uv >> 16) & 0x3FFF);
            }
        }
        if (gVitaZoneOn & 1) {
            scePerfArmPmonGetCounterValue(0, 31, &c1);
            gVitaTriRunCyc[0] += c1 - c0;
            gVitaTriRunCyc[1] += m;
        }
        if (!gVitaGsSkip) {
            GsGpu_DrawTriRun(ctx, vb, m);
        }
    }
    q += 14 * (n - 1); /* (the registers as the last packet leaves them) */
    gs.rgbaq = q[10];
    { uint32_t qq = (uint32_t)(q[10] >> 32); memcpy(&gs.q, &qq, 4); }
    gs.st = q[11];
    gs.vtx[0] = vb[3 * ((n - 1) % 64)];
    gs.vtx[1] = vb[3 * ((n - 1) % 64) + 1];
    gs.vtx[2] = vb[3 * ((n - 1) % 64) + 2];
    gs.vcount = 0;
    gs.strip = 3;
}
#endif
static uint64_t sGifTagLo, sGifTagHi; /* current tag: persists across runs */
static uint32_t sGifLoops, sGifReg;    /* loops left, register index within the loop */
__attribute__((hot)) void Gs_Gif(const uint8_t *p, uint32_t qwc) {
#define tag_lo sGifTagLo
#define tag_hi sGifTagHi
#define loops sGifLoops
#define reg sGifReg
    const uint64_t *q = (const uint64_t *)p;
    const uint8_t *pf = p + 256; /* the next cache line of the data on its way: a line ahead every 32 bytes read */

    if (qwc > 16) {
        __builtin_prefetch(p + 64);
        __builtin_prefetch(p + 96);
        __builtin_prefetch(p + 128);
        __builtin_prefetch(p + 160);
        __builtin_prefetch(p + 192);
        __builtin_prefetch(p + 224);
    }
    while (qwc != 0) {
        uint32_t flg, nreg;

        if ((const uint8_t *)q + 256 >= pf) {
            if (pf < (const uint8_t *)q + 128) { /* (after a run of packets read at once) */
                pf = (const uint8_t *)q + 256;
            }
            __builtin_prefetch(pf);
            pf += 32;
        }

#ifdef PORT_VITA
        /* A run of the effects' triangle packets as the ordering table sends them (src/battle/eft_c.c,
           eft_d.c: one GIF tag "REGLIST, 12 registers": PRIM, TEX0, three times RGBAQ, ST, XYZF2, and a NOP), all with
           the same PRIM and TEX0: the registers written as the GIF would write them, and each triangle drawn, without the
           register-by-register walk (the sea of Kame House: 1,400 of them a picture). */
        if (loops == 0 && qwc >= 7 && sGpu && !sDpCap && sVtxDbg == 0 && gVitaTriRun &&
            (q[0] == 0xC400000000008001ull || q[0] == 0xC400000000000001ull) && (q[1] == 0xF42142142160ull || q[1] == 0xF42142142170ull)) {
            uint32_t n = 1, maxn = qwc / 7;
            while (n < maxn && q[14 * n] == q[0] && q[14 * n + 1] == q[1] && q[14 * n + 2] == q[2] && q[14 * n + 3] == q[3]) {
                n++;
            }
            if (gVitaZoneOn & 1) { /* */
                unsigned c0 = 0, c1 = 0;
                scePerfArmPmonGetCounterValue(0, 31, &c0);
                tri_run(q, n);
                scePerfArmPmonGetCounterValue(0, 31, &c1);
                gVitaGifCyc[0] += c1 - c0;
                gVitaGifCnt[0] += n;
            } else {
                tri_run(q, n);
            }
            q += 14 * n;
            qwc -= 7 * n;
            continue;
        }
#endif
        if (loops == 0) {
            sStat[3]++;
            tag_lo = q[0];
            tag_hi = q[1];
            q += 2;
            qwc--;
            loops = tag_lo & 0x7FFF;
            reg = 0;
#ifdef PORT_VITA
            gVitaGifN[(tag_lo >> 58) & 3 ? (((tag_lo >> 58) & 3) == 1 ? 1 : 2) : 0]++;
#endif
            if (loops != 0 && ((tag_lo >> 46) & 1) && ((tag_lo >> 58) & 3) == 0) {
                reg_write(0, (tag_lo >> 47) & 0x7FF);
            }
            continue;
        }
        flg = (tag_lo >> 58) & 3;
        nreg = (tag_lo >> 60) & 15;
        if (nreg == 0) {
            nreg = 16;
        }
        if (flg == 0) { /* PACKED: one quadword per register */
            uint32_t r = (tag_hi >> (4 * reg)) & 15;
            uint64_t lo = q[0], hi = q[1];

            switch (r) {
            case 0x0: reg_write(0, lo & 0x7FF); break;
            case 0x1:
#ifdef PORT_VITA
                if (sDpCap) { dp_rd(DPF(32)); }
#endif
                reg_write(1, (lo & 0xFF) | ((lo >> 32) & 0xFF) << 8 | (hi & 0xFF) << 16 | ((hi >> 32) & 0xFF) << 24 |
                                   (uint64_t)*(uint32_t *)&gs.q << 32); break;
            case 0x2:
#ifdef PORT_VITA
                if (sDpCap) { dp_wr(DPF(3) | DPF(32)); }
#endif
                gs.st = lo; { uint32_t qq = (uint32_t)hi; memcpy(&gs.q, &qq, 4); } break;
            case 0x3: reg_write(3, (lo & 0x3FFF) | ((lo >> 32) & 0x3FFF) << 16); break;
            case 0x4: vertex(lo & 0xFFFF, (lo >> 32) & 0xFFFF, (uint32_t)((hi >> 4) & 0xFFFFFF), !((hi >> 47) & 1)); break;
            case 0x5: vertex(lo & 0xFFFF, (lo >> 32) & 0xFFFF, (uint32_t)hi, !((hi >> 47) & 1)); break;
            case 0xC: vertex(lo & 0xFFFF, (lo >> 32) & 0xFFFF, (uint32_t)((hi >> 4) & 0xFFFFFF), 0); break;
            case 0xD: vertex(lo & 0xFFFF, (lo >> 32) & 0xFFFF, (uint32_t)hi, 0); break;
            case 0xE: GZM2(8); reg_write((uint32_t)(hi & 0xFF), lo); GZM2(9); gVitaGifN[3]++; break;
            case 0xF: break;
            default: reg_write(r, lo); break;
            }
            q += 2;
            qwc--;
            if (++reg == nreg) {
                reg = 0;
                loops--;
            }
        } else if (flg == 1) { /* REGLIST: 64 bits per register */
            int half;
            for (half = 0; half < 2 && loops != 0; half++) {
                uint32_t r = (tag_hi >> (4 * reg)) & 15;
                if (r != 0xE && r != 0xF) {
                    reg_write(r, q[half]);
                }
                gVitaGifN[4]++;
                if (++reg == nreg) {
                    reg = 0;
                    loops--;
                }
            }
            q += 2;
            qwc--;
        } else { /* IMAGE */
            uint32_t n = loops < qwc ? loops : qwc;
#ifdef PORT_VITA
            if (sDpCap) {
                sDpBad = 1;
            }
#endif
            sStat[6] += n;
            VPROF(4);
            GZM2(8);
            if (gVitaZoneOn & 1) { /* */
                unsigned c0 = 0, c1 = 0;
                scePerfArmPmonGetCounterValue(0, 31, &c0);
                transfer_data((const uint8_t *)q, n * 16);
                scePerfArmPmonGetCounterValue(0, 31, &c1);
                gVitaGifCyc[1] += c1 - c0;
                gVitaGifCnt[1]++;
            } else {
                transfer_data((const uint8_t *)q, n * 16);
            }
            GZM2(7);
            VPROF(3);
            q += 2 * n;
            qwc -= n;
            loops -= n;
        }
    }
#undef tag_lo
#undef tag_hi
#undef loops
#undef reg
}

/* ------------------------------------------------------------------------------------------------ front / back */
#ifdef PORT_VITA
/* The list is read on one thread (DMA tags, VIF commands, VU1 programs: the "front end") and drawn on another (GS
   registers, GS memory, the graphics chip: the "back end"), with a queue of GS work between them, as the PS2's
   own GIF sits between VU1 and the GS. Each side then has a processor core to itself. The front end keeps its own
   copies of the flags both sides use; the back end owns the GS state (gs, GS memory, gVitaGsSkipR, gGsMainFbp). */
int gVitaGsSkipF;                       /* the front end's "this frame is not drawn" */
int gVglVertsStable, gVglVertsStableF;  /* vita_vgl.c reads the first; gs_vu1.c writes the second */
int gVglSamePosF;
int gGsSplit;                           /* the back end runs on a thread of its own */
#define GR_SIZE (8u << 20)
typedef struct GRec { uint16_t type, flags; uint32_t size; uint32_t a, b, c, e; uint64_t d; } GRec; /* 32 bytes */
enum { GR_WRAP, GR_GIFP, GR_GIFC, GR_REG, GR_VU, GR_SKIP, GR_FBP, GR_FRAMEEND, GR_LISTBEGIN, GR_LISTEND, GR_INIT, GR_GIFCH, GR_CHIPRESET, GR_PART, GR_VU2, GR_T116 };
typedef struct Vu2 { uint64_t texv[2]; uint32_t texa[2], prim[2], ctx[2]; uint32_t pad[2]; } Vu2; /* GR_VU2: program 0's two layers (48 bytes) */
static uint8_t sGr[GR_SIZE] __attribute__((aligned(64)));
static volatile unsigned sGrHead, sGrTail; /* bytes taken by the back end / put by the front end (count on and on) */
static volatile int sGrCurType = -1; /* the record the back end is doing (the watchdog) */
unsigned gGrWaits;                         /* for the log: times the front end found the queue full */

int Port_InGr(const void *p) {
    return (const uint8_t *)p >= sGr && (const uint8_t *)p < sGr + GR_SIZE;
}

unsigned long long gGrRoomUs; /* for the log: the front end's time waiting for room in the queue */
unsigned long long gVitaFeUs[2][2]; /* for the log: lists not drawn / drawn: how many, the front end's microseconds */
static unsigned sGrHeadSeen; /* the front end's last look at sGrHead (another core writes it: looked at again only when needed) */
static void gr_room(unsigned need) {
    unsigned spins = 0;
    unsigned long long t0;
    if (sGrTail + need - sGrHeadSeen <= GR_SIZE) {
        return;
    }
    sGrHeadSeen = sGrHead;
    if (sGrTail + need - sGrHeadSeen <= GR_SIZE) {
        return;
    }
    gGrWaits++;
    t0 = sceKernelGetProcessTimeWide();
    while (sGrTail + need - sGrHead > GR_SIZE) {
        if (++spins > 500) {
            sceKernelDelayThread(50);
        }
    }
    sGrHeadSeen = sGrHead;
    gGrRoomUs += sceKernelGetProcessTimeWide() - t0;
}

/* Register writes put off (GsFE_RegDefer): sent as records before the next record of any kind. */
static struct { uint32_t addr; uint64_t d; } sRegDef[4];
static int sRegDefN;
static void reg_def_flush(void);
/* A record with `payload` bytes behind it, in one piece. */
static GRec *gr_alloc(uint32_t payload) {
    uint32_t need = (32 + payload + 31) & ~31u;
    if (sRegDefN != 0) {
        reg_def_flush();
    }
    for (;;) {
        uint32_t pos = sGrTail % GR_SIZE;
        if (pos + need > GR_SIZE) {
            GRec *w = (GRec *)(void *)&sGr[pos];
            gr_room(GR_SIZE - pos);
            w->type = GR_WRAP;
            w->size = GR_SIZE - pos;
            __sync_synchronize();
            sGrTail += GR_SIZE - pos;
            continue;
        }
        gr_room(need);
        {
            GRec *r = (GRec *)(void *)&sGr[pos];
            r->size = need;
            r->flags = 0;
            return r;
        }
    }
}

static void gr_put(GRec *r) {
    __sync_synchronize();
    sGrTail += r->size;
}

static void gr_simple(int type, uint32_t a, uint32_t b, uint32_t c) {
    GRec *r = gr_alloc(0);
    r->type = (uint16_t)type;
    r->a = a;
    r->b = b;
    r->c = c;
    gr_put(r);
}

static int gr_idle(void) {
    return sGrHead == sGrTail;
}

static void gr_drain(void) {
    if (sRegDefN != 0) {
        reg_def_flush();
    }
    while (!gr_idle()) {
        sceKernelDelayThread(100);
    }
}

void GsFE_RegWrite(uint32_t addr, uint64_t d) {
    GRec *r;
    if (!gGsSplit) {
        Gs_RegWrite(addr, d);
        return;
    }
    r = gr_alloc(0);
    r->type = GR_REG;
    r->a = addr;
    r->d = d;
    gr_put(r);
}
/* A register write whose value matters only if something comes after it before the same register is written
   again (gs_vu1.c: the shadow-page batches that are not sent, 800 a picture, each setting TEX0 and PRIM): the
   last value of each register is sent, before whatever record comes next. */
void GsFE_RegDefer(uint32_t addr, uint64_t d) {
    int i;
    if (!gGsSplit) {
        Gs_RegWrite(addr, d);
        return;
    }
    for (i = 0; i < sRegDefN; i++) {
        if (sRegDef[i].addr == addr) {
            sRegDef[i].d = d;
            return;
        }
    }
    if (sRegDefN == 4) {
        reg_def_flush();
    }
    sRegDef[sRegDefN].addr = addr;
    sRegDef[sRegDefN].d = d;
    sRegDefN++;
}
static void reg_def_flush(void) {
    int i, n = sRegDefN;
    sRegDefN = 0; /* (gr_alloc below: nothing put off any more) */
    for (i = 0; i < n; i++) {
        GRec *r = gr_alloc(0);
        r->type = GR_REG;
        r->a = sRegDef[i].addr;
        r->d = sRegDef[i].d;
        gr_put(r);
    }
}

/* as GsFE_Gif, and the image transfers in the copy given their numbers before the drawing thread sees it */
static void GsFE_GifHashed(const uint8_t *p, uint32_t qwc) {
    GRec *r = gr_alloc(qwc * 16);
    r->type = GR_GIFC;
    r->b = qwc;
    memcpy(r + 1, p, qwc * 16);
    fe_hash_gif((const uint8_t *)(r + 1), qwc, 1);
    gr_put(r);
}
void GsFE_Gif(const uint8_t *p, uint32_t qwc) { /* (VU memory or a packet put together: copied) */
    GRec *r;
    if (!gGsSplit) {
        Gs_Gif(p, qwc);
        return;
    }
    r = gr_alloc(qwc * 16);
    r->type = GR_GIFC;
    r->b = qwc;
    memcpy(r + 1, p, qwc * 16);
    gr_put(r);
}

typedef struct ChipPlan { uint32_t mode, at, count, uh, upload, slot, ioff; } ChipPlan; /* (as vita_vgl.c has it) */
extern int Vita_ChipResetDue(void);
extern int Vita_ChipPlan(int which, int layer, const float *v, uint32_t count, const float *consts, int stable, ChipPlan *p);
extern void Vita_ChipFill(const ChipPlan *p, const float *v, uint8_t *dst);
static int sCcCap;         /* (the call cache, below: the call being recorded) */
static uint32_t sChainSp;  /* (run_chain: calls deep) */
unsigned gVitaFtWhere[4];  /* for the log: fighter strips in a call being recorded / another call / no call; planned */
static float sFeLast[96]; /* fe_vu: the constants the back end holds (sent only when they change) */
static int sFeHave;
static void fe_vu(int which, int layer, int ctx, const float *v, uint32_t count, const float *consts, uint32_t stride) {
    extern int Port_DmaInList(uint32_t a);
    extern int Port_InSnapArena(const void *p);
    int stable = gVglVertsStableF && !(gGsSplit && (Port_DmaInList((uint32_t)(uintptr_t)v) || Port_InSnapArena(v))), same = gVglSamePosF; /* (in the game's list buffer: copied, see GR_LISTEND) */
    gVglVertsStableF = 0;
    gVglSamePosF = 0;
    if (!gGsSplit) {
        extern int gVglSamePos;
        if (ctx < 0) {
            ctx = (int)((gs.prim >> 9) & 1);
        }
        gVglVertsStable = stable;
        gVglSamePos = same;
        if (which == 0) { GsGpu_DrawVu0(layer, ctx, v, count, consts); }
        else if (which == 4) { GsGpu_DrawVu4(ctx, v, count, consts); }
        else { GsGpu_DrawVu6(ctx, v, count, consts); }
        return;
    }
    {
        /* the constants travel only when they changed (a fighter's run of strips shares its bones): the back end
           keeps the last ones. That is most of what the queue carried, and memory another core wrote is slow to read */
        float *last = sFeLast;
#define have sFeHave
        /* the models kept on the chip: planned here (vita_vgl.c), the back end only appends */
        ChipPlan plan;
        int extra, noix;
        int keep = have && memcmp(last, consts, 384) == 0;
        uint32_t vb = stable ? 0 : count * stride * 4, cb = keep ? 0 : 384, pb;
        GRec *r;
        if (Vita_ChipResetDue()) {
            gr_simple(GR_CHIPRESET, 0, 0, 0);
        }
        { extern volatile int gVitaZoneOn; extern void Vita_Z(int z); if (gVitaZoneOn) { Vita_Z(7); } }
        extra = Vita_ChipPlan(which, layer, v, count, consts, stable, &plan);
        if (which == 0) { /* (for the log: a fighter strip in a call being recorded / in another call / in no call) */
            extern unsigned gVitaFtWhere[4];
            gVitaFtWhere[sCcCap >= 0 ? 0 : sChainSp > 0 ? 1 : 2]++;
            gVitaFtWhere[3] += extra >= 0;
        }
        { extern volatile int gVitaZoneOn; extern void Vita_Z(int z); if (gVitaZoneOn) { Vita_Z(8); } }
        /* a silhouette's strip (program 2, layer 2) already on the chip: its indices are there too and the
           back end draws it from them (GsGpu_DsSil), so they are not written into the record (flag 32) */
        noix = extra >= 0 && which == 0 && layer == 2 && plan.mode == 1 && !plan.upload;
        pb = noix ? 32 : extra >= 0 ? 32 + (uint32_t)extra : 0;
        r = gr_alloc(cb + vb + pb);
        if (!keep) {
            memcpy(last, consts, 384);
            have = 1;
        }
        r->type = GR_VU;
        r->flags = (uint16_t)(stable | same << 1 | (!stable) << 2 | keep << 3 | (pb != 0) << 4 | noix << 5);
        r->a = (uint32_t)which;
        r->b = (uint32_t)layer;
        r->c = (uint32_t)ctx;
        r->e = count;
        r->d = (uint64_t)(uintptr_t)v;
        if (cb != 0) {
            memcpy(r + 1, consts, 384);
        }
        if (vb != 0) {
            memcpy((uint8_t *)(r + 1) + cb, v, vb);
        }
        if (pb != 0) {
            memcpy((uint8_t *)(r + 1) + cb + vb, &plan, sizeof(plan));
            if (!noix) {
                Vita_ChipFill(&plan, v, (uint8_t *)(r + 1) + cb + vb + 32);
            }
        }
        gr_put(r);
        { extern volatile int gVitaZoneOn; extern void Vita_Z(int z); if (gVitaZoneOn) { Vita_Z(9); } }
    }
}
#undef have
/* Program 0's batch with both its layers as ONE record (gs_vu1.c hle_program0): the two layers' TEX0 writes and
   PRIMs, the constants when they changed, the strip's plan and indices once. Was six records and two plans. The
   back end does what the six did (GR_VU2). 0: not this way (the six records then). */
int GsFE_DrawVu0Pair(const uint8_t *h, const float *v, uint32_t count, const float *consts, int stableIn) {
    extern int Port_DmaInList(uint32_t a);
    extern int Port_InSnapArena(const void *p);
    extern volatile int gVitaZoneOn;
    extern void Vita_Z(int z);
    uint64_t tag[2], ad[2][2], pt[2][2];
    ChipPlan plan;
    int extra, keep, k;
    uint32_t cb;
    GRec *r;
    Vu2 *x;
    if (!gGsSplit || sCcCap >= 0 || !stableIn || Port_DmaInList((uint32_t)(uintptr_t)v) || Port_InSnapArena(v)) {
        return 0;
    }
    memcpy(tag, h, 16);
    if (!((tag[0] & 0x7FFF) == 1 && !((tag[0] >> 46) & 1) && ((tag[0] >> 58) & 3) == 0 && (tag[0] >> 60) == 1 && (tag[1] & 0xF) == 0xE)) {
        return 0; /* (the head is not one A+D register: the GIF parser's way) */
    }
    memcpy(ad, h + 16, 32);
    memcpy(pt, h + 48, 32);
    if (Vita_ChipResetDue()) {
        gr_simple(GR_CHIPRESET, 0, 0, 0);
    }
    extra = Vita_ChipPlan(0, 0, v, count, consts, 1, &plan);
    if (extra < 0) {
        return 0;
    }
    if (gVitaZoneOn) { Vita_Z(8); }
    gVitaFtWhere[2] += 2;
    gVitaFtWhere[3] += 2;
    keep = sFeHave && memcmp(sFeLast, consts, 384) == 0;
    cb = keep ? 0 : 384;
    if (!plan.upload) {
        extra = 0; /* (the strip's indices are on the chip already, vita_vgl.c sFtIbo: not sent) */
    }
    r = gr_alloc(48 + cb + 32 + (uint32_t)extra);
    if (!keep) {
        memcpy(sFeLast, consts, 384);
        sFeHave = 1;
    }
    r->type = GR_VU2;
    r->flags = (uint16_t)(1 | keep << 3 | 1 << 4 | (!plan.upload) << 5);
    r->e = count;
    r->d = (uint64_t)(uintptr_t)v;
    x = (Vu2 *)(void *)(r + 1);
    for (k = 0; k < 2; k++) {
        x->texv[k] = ad[k][0];
        x->texa[k] = (uint32_t)(ad[k][1] & 0xFF);
        x->prim[k] = (uint32_t)((pt[k][0] >> 47) & 0x7FF);
        x->ctx[k] = (uint32_t)((pt[k][0] >> 56) & 1);
    }
    if (cb != 0) {
        memcpy((uint8_t *)(r + 1) + 48, consts, 384);
    }
    memcpy((uint8_t *)(r + 1) + 48 + cb, &plan, sizeof(plan));
    if (plan.upload) {
        Vita_ChipFill(&plan, v, (uint8_t *)(r + 1) + 48 + cb + 32);
    }
    gr_put(r);
    if (gVitaZoneOn) { Vita_Z(25); }
    return 1;
}
void GsFE_DrawVu0(int layer, int ctx, const float *v, uint32_t count, const float *consts) { fe_vu(0, layer, ctx, v, count, consts, 12); }
void GsFE_DrawVu4(int ctx, const float *v, uint32_t count, const float *consts) { fe_vu(4, 0, ctx, v, count, consts, 12); }

/* ---- replayed calls: a model part's vertex chain (a DMA "call" to static data), read once, sent again from a copy
   The front end's work for a part was the same every frame: walk the chain, unpack its vertices into VU memory,
   run the VU program's stand-in, plan each strip on the chip. Only the constants (the bones) change. So the records
   one call made are kept, without their constants, and later calls to the same address send the kept records with
   the constants of now (VU memory 0..23, unpacked by the call packet itself). A call is kept only when it made
   nothing but register writes, GIF packets and strips already on the chip, with constants from VU memory 0; it is
   read the long way again now and then, and dropped if anything in it changed (a part the game writes into). */
extern const uint8_t *GsVu1_Consts(void);
extern unsigned gVitaChipGen;
#define CC_N 2048
typedef struct CallCache { uint32_t addr, gen, bytes, at, uses, serial; uint8_t state; uint8_t *data; } CallCache; /* state: 0 new, 1 read once, 2 kept, 3 never */
static uint32_t sCcSerial; /* numbers the kept copies (the back end keeps its own form of each: see dp_part) */
static CallCache sCC[CC_N];
static int sCcCap = -1;          /* the entry being read the long way (and recorded), -1: none */
static unsigned sCcStart, sCcDepth;
unsigned gVitaCc[4];              /* for the log: calls replayed, read the long way, kept, dropped */

static void cc_retire(uint8_t *p);
static CallCache *cc_find(uint32_t addr) {
    uint32_t h = (addr * 0x9E3779B1u) >> 21, k;
    for (k = 0; k < 16; k++, h = (h + 1) & (CC_N - 1)) {
        if (sCC[h].addr == addr || sCC[h].addr == 0) {
            if (sCC[h].addr == 0) { memset(&sCC[h], 0, sizeof(sCC[h])); sCC[h].addr = addr; }
            if (sCC[h].gen != gVitaChipGen) { /* (the chip's buffers were let go: what was kept refers to them) */
                cc_retire(sCC[h].data); sCC[h].data = NULL; sCC[h].state = 0; sCC[h].gen = gVitaChipGen;
            }
            return &sCC[h];
        }
    }
    return NULL;
}

/* The call to e->addr ended: keep what it made, if it can be kept. */
/* A kept copy let go is freed once the back end has finished the list it was let go in: GR_PART
   records sent before point at it. It was freed six lists later as the list READER counts them, and the reader can
   be further ahead than that of the drawing thread (a drawn list that takes long, the lists after it read and left
   out meanwhile; lists the game waited for in the intro): the drawing thread then walked freed
   memory (and those of tests 187 and 182 at the fight's start) */
static volatile uint32_t sBeListDone; /* the drawing thread: the last list it finished (GR_LISTEND) */
#define CC_OLD 4096
static struct { uint8_t *p; unsigned list; } sCcOld[CC_OLD];
static unsigned sCcOldN, sCcLists;
unsigned gVitaCcLeak; /* (for the log: copies never freed, the table being full) */
static void cc_retire(uint8_t *p) { /* (the back end may still be walking it: freed once it is through that list) */
    unsigned i, n = 0, done = sBeListDone;
    __sync_synchronize();
    for (i = 0; i < sCcOldN; i++) {
        if ((int)(done - sCcOld[i].list) >= 1) { free(sCcOld[i].p); } else { sCcOld[n++] = sCcOld[i]; }
    }
    sCcOldN = n;
    if (p == NULL) { return; }
    if (sCcOldN == CC_OLD) { /* (full: the oldest is left, not freed: freeing it could be what the back end walks) */
        memmove(sCcOld, sCcOld + 1, (CC_OLD - 1) * sizeof(sCcOld[0]));
        sCcOldN--;
        gVitaCcLeak++;
    }
    sCcOld[sCcOldN].p = p;
    sCcOld[sCcOldN].list = sFeListSeq;
    sCcOldN++;
}
unsigned gVitaCcWhy[9]; /* for the log: calls not kept: too big, wrapped, nothing in them, a strip with its vertices copied, a strip not planned, constants not VU memory 0, another record; GIF data copied in; VU memory 0..31 changed by the call */
unsigned gVitaCcMax;     /* the largest call recorded (bytes of records) */
static uint8_t sCcK0[512]; /* VU memory 0..31 when the call being recorded began */
static void cc_end(CallCache *e) {
    unsigned end, pos = sCcStart, uploads = 0, out = 0, extra = 0;
    const uint8_t *k = GsVu1_Consts();
    uint8_t *buf;
    if (sRegDefN != 0) {
        reg_def_flush(); /* (what the call put off belongs to it) */
    }
    end = sGrTail;
    int ok = 1;
    sCcCap = -1;
    if (memcmp(sCcK0, k, 512) != 0) {
        /* The call wrote the constants itself (a fighter's piece unpacks its own bones): sent again from its copy,
           it would be drawn with the constants of before the call (the piece before it: a test, parts of the
           fighters drawn elsewhere, so missing), and what comes after the call would not see the constants it
           left. Read the long way every time. */
        gVitaCcWhy[8]++;
        e->state = 3;
        return;
    }
    if (end - pos > gVitaCcMax) {
        gVitaCcMax = end - pos;
    }
    if (end - pos == 0 || end - pos > 256 * 1024) {
        /* nothing, or a whole model in one call: never kept, so that the calls inside it are (each part) */
        gVitaCcWhy[end - pos == 0 ? 2 : 0]++;
        e->state = 3;
        return;
    }
    if ((pos % GR_SIZE) + (end - pos) > GR_SIZE) {
        gVitaCcWhy[1]++; /* (wrapped round the queue: another time) */
        return;
    }
    {   /* GIF data the call refers to (in the model, not the list) is kept as a copy: room for it */
        unsigned q = pos;
        while (q != end) {
            const GRec *r = (const GRec *)(const void *)&sGr[q % GR_SIZE];
            if (r->type == GR_GIFP) {
                extra += 32 + r->b * 16;
            }
            q += r->size;
        }
    }
    buf = (uint8_t *)malloc(end - pos + extra);
    if (buf == NULL) { return; }
    while (pos != end && ok) {
        const GRec *r = (const GRec *)(const void *)&sGr[pos % GR_SIZE];
        uint32_t sz = r->size;
        if (r->type == GR_REG || r->type == GR_GIFC) {
            memcpy(buf + out, r, sz);
            out += sz;
        } else if (r->type == GR_GIFP && r->b <= 4096) {
            GRec *o = (GRec *)(void *)(buf + out);
            uint32_t rec = (32 + r->b * 16 + 31) & ~31u;
            memset(o, 0, 32);
            o->type = GR_GIFC;
            o->size = rec;
            o->b = r->b;
            memcpy(o + 1, (const void *)(uintptr_t)r->a, r->b * 16);
            out += rec;
            gVitaCcWhy[7]++;
        } else if (r->type == GR_VU && (r->flags & 1) && !(r->flags & 4) && (r->flags & 16) && !(r->flags & 32)) {
            uint32_t cb = (r->flags & 8) ? 0 : 384;
            const uint32_t *plan = (const uint32_t *)(const void *)((const uint8_t *)(r + 1) + cb);
            uint32_t mode = plan[0], cnt = plan[2], upload = plan[4], ib, rec;
            if (cb != 0 && memcmp(r + 1, k, 384) != 0) { ok = 0; gVitaCcWhy[5]++; break; } /* (constants not from VU memory 0) */
            uploads += upload != 0;
            ib = mode == 1 ? (((cnt - 2) * 3 * 2 + 3) & ~3u) : (cnt - 2) * 3 * 4;
            rec = (32 + 32 + ib + 31) & ~31u;
            {
                GRec *o = (GRec *)(void *)(buf + out);
                uint32_t *op;
                memcpy(o, r, 32);
                o->size = rec;
                o->flags = (uint16_t)((r->flags & ~8u) | 8u); /* (stored without constants: they are put back when sent) */
                op = (uint32_t *)(void *)(o + 1);
                memcpy(op, plan, 32);
                op[4] = 0; /* (the vertices are on the chip now) */
                memcpy((uint8_t *)op + 32, (const uint8_t *)plan + 32 + (upload ? cnt * (mode == 1 ? 40u : 32u) : 0), ib);
            }
            out += rec;
        } else {
            ok = 0;
            gVitaCcWhy[r->type != GR_VU ? 6 : (r->flags & 4) || !(r->flags & 1) ? 3 : 4]++;
        }
        pos += sz;
    }
    if (!ok) {
        free(buf);
        e->state = 3;
        return;
    }
    gVitaCc[1]++;
    if (uploads != 0 || e->state == 0) { /* (first sight, or written to the chip just now: kept from the next sight on) */
        free(buf);
        if (e->state == 2) { gVitaCc[3]++; cc_retire(e->data); e->data = NULL; }
        e->state = 1;
        return;
    }
    if (e->data != NULL && e->bytes == out && memcmp(e->data, buf, out) == 0) {
        free(buf); /* (the same as kept: the kept copy stays, the back end may be walking it) */
    } else {
        cc_retire(e->data);
        e->data = buf;
        e->serial = ++sCcSerial;
    }
    e->bytes = out;
    e->state = 2;
    e->uses = 0;
    gVitaCc[2]++;
}

/* Send what the call to e->addr made, with the constants of now: one record (the back end walks the kept ones). */
static void cc_replay(CallCache *e) {
    const uint8_t *k = GsVu1_Consts();
    GRec *o = gr_alloc(384);
    o->type = GR_PART;
    o->a = (uint32_t)(uintptr_t)e->data;
    o->b = e->bytes;
    o->c = e->serial;
    memcpy(o + 1, k, 384);
    gr_put(o);
    sFeHave = 0; /* (the back end's constants are these now, not fe_vu's last ones) */
    gVitaCc[0]++;
}
void GsFE_DrawVu6(int ctx, const float *v, uint32_t count, const float *consts) { fe_vu(6, 0, ctx, v, count, consts, 16); }

/* ---- direct parts, the back end's side: a kept part (GR_PART, numbered by its copy) is walked the long way once
   while vita_vgl.c records its strips (GsGpu_DpCap*), and the GS fields it read before writing them and those it
   wrote are noted (dp_note). Later, when the fields it reads hold what they held then, vita_vgl.c draws its groups
   (GsGpu_DpDraw) and the fields it writes are set to what they were after it: nothing is walked. Two forms per
   part (a field that alternates frame by frame, the buffer drawn into); a part that keeps changing is let be. */
extern int GsGpu_DpOn(void);
extern void GsGpu_DpCapBegin(void), GsGpu_DpCapAbort(void), GsGpu_DpFree(void *blob), GsGpu_DpDraw(const void *blob, const float *c), GsGpu_DpReset(void);
extern void *GsGpu_DpCapEnd(void);
extern unsigned gVitaDp[8];
unsigned gVitaDpMiss[34]; /* for the log: kept parts found in another state, by the first GS field that differed */
#define DP_NF 34
#define DP_N 1024
/* what a kept part's match compares comes first and packed (n fields: their numbers, their values): the
   drawing thread looked it up in a 1 MB table, field by field across 272 bytes, 2,400 cycles a part */
typedef struct DpForm { void *gpu; uint32_t hits; uint8_t n, idx[DP_NF + 1]; uint64_t val[DP_NF]; uint64_t cmp, wr; uint64_t entry[DP_NF], exit[DP_NF]; } DpForm;
/* four forms a part, was two: the buffer drawn into alternates (FRAME_1, field 22) and, while the fighters'
   ground shadows are drawn, the second context's texture (field 9) too; with two forms those parts were walked the
   long way three times in four (15,000 a 2 s in the shadows' stretches of a test, the drawing thread 36-45 ms a
   list) and soon refused for good as changing */
#define DP_FORMS 4
typedef struct DpSlot { uint32_t serial, refused, made, hits, seen; DpForm f[DP_FORMS]; } DpSlot;
static DpSlot sDp[DP_N];
static int sDpOn = -1;
static uint64_t sDpEntry[DP_NF];
_Static_assert(offsetof(GsState, trxdir) == 31 * 8 && offsetof(GsState, prim) == 0, "the GS fields followed lie one after another");

static void dp_snap(uint64_t *v) {
    uint32_t qq;
    memcpy(v, &gs.prim, 32 * 8);
    memcpy(&qq, &gs.q, 4);
    v[32] = qq;
    v[33] = (uint64_t)(uint32_t)gs.vcount | (uint64_t)(uint32_t)gs.strip << 32;
}
static uint64_t dp_field(int i) {
    if (i < 32) {
        return (&gs.prim)[i];
    }
    if (i == 32) {
        uint32_t qq;
        memcpy(&qq, &gs.q, 4);
        return qq;
    }
    return (uint64_t)(uint32_t)gs.vcount | (uint64_t)(uint32_t)gs.strip << 32;
}
static int dp_match(const DpForm *f) {
    int k, n = f->n;
    for (k = 0; k < n; k++) {
        if (dp_field(f->idx[k]) != f->val[k]) {
            return 0;
        }
    }
    return 1;
}
static void dp_apply(const DpForm *f) {
    uint64_t m = f->wr;
    while (m != 0) {
        int i = __builtin_ctzll(m);
        if (i < 32) {
            (&gs.prim)[i] = f->exit[i];
        } else if (i == 32) {
            uint32_t qq = (uint32_t)f->exit[32];
            memcpy(&gs.q, &qq, 4);
        } else {
            gs.vcount = (int)(uint32_t)f->exit[33];
            gs.strip = (int)(uint32_t)(f->exit[33] >> 32);
        }
        m &= m - 1;
    }
}
static void dp_clear(void) {
    int i, k;
    for (i = 0; i < DP_N; i++) {
        for (k = 0; k < DP_FORMS; k++) {
            GsGpu_DpFree(sDp[i].f[k].gpu);
        }
    }
    memset(sDp, 0, sizeof(sDp));
}
/* The part numbered `serial` drawn from its kept form: 1, or 0 (walk it). */
extern int gVitaGsSkip;
extern int gT116Be;
static int dp_part(uint32_t serial, const float *consts) {
    DpSlot *s;
    int k;
    if (sDpOn < 0) {
        extern int gVglSafe;
        sDpOn = !(gVglSafe & 131072) && getenv("BT3_NO_DP") == NULL;
    }
    if (!sDpOn || serial == 0 || gVitaGsSkip || sGifLoops != 0 || (gT116Be & 8)) { /* (kept parts walked) */
        return 0;
    }
    s = &sDp[serial & (DP_N - 1)];
    if (s->serial != serial) {
        return 0;
    }
    for (k = 0; k < DP_FORMS; k++) {
        DpForm *f = &s->f[k];
        if (f->gpu != NULL && dp_match(f)) {
            GsGpu_DpDraw(f->gpu, consts);
            dp_apply(f);
            f->hits++;
            s->hits++;
            s->seen = sFrame;
            return 1;
        }
    }
    if (s->f[0].gpu != NULL) {
        static unsigned logged;
        gVitaDp[5]++; /* (kept, but the GS state differs) */
        for (k = 0; k < DP_FORMS; k++) { /* (for the log: the first field that differs, form by form) */
            uint64_t m = s->f[k].gpu != NULL ? s->f[k].cmp : 0;
            while (m != 0) {
                int i = __builtin_ctzll(m);
                if (dp_field(i) != s->f[k].entry[i]) {
                    gVitaDpMiss[i]++;
                    break;
                }
                m &= m - 1;
            }
        }
        if (logged < 24) {
            extern void Vita_Log(const char *fmt, ...);
            uint64_t m = s->f[0].cmp;
            logged++;
            while (m != 0) {
                int i = __builtin_ctzll(m);
                if (dp_field(i) != s->f[0].entry[i]) {
                    Vita_Log("gs: part %u found in another state: field %d is %08x%08x, was %08x%08x\n", (unsigned)serial, i, (unsigned)(dp_field(i) >> 32),
                             (unsigned)dp_field(i), (unsigned)(s->f[0].entry[i] >> 32), (unsigned)s->f[0].entry[i]);
                    break;
                }
                m &= m - 1;
            }
        }
    }
    return 0;
}
/* Before the part is walked: is it to be recorded? */
static int dp_cap_begin(uint32_t serial) {
    DpSlot *s;
    gVitaDp[6]++;
    if (!sDpOn || serial == 0 || gVitaGsSkip || sGifLoops != 0 || !GsGpu_DpOn() || (gT116Be & 8)) {
        return 0;
    }
    s = &sDp[serial & (DP_N - 1)];
    if (s->serial != serial && s->serial != 0 && sFrame - s->seen < 16) {
        return 0; /* (the place is another part's, in use) */
    }
    if (s->serial != serial) {
        int k2;
        for (k2 = 0; k2 < DP_FORMS; k2++) {
            GsGpu_DpFree(s->f[k2].gpu);
        }
        memset(s, 0, sizeof(*s));
        s->serial = serial;
    }
    if (s->refused) {
        return 0;
    }
    s->seen = sFrame;
    dp_snap(sDpEntry);
    sDpCmp = sDpWr = 0;
    sDpBad = 0;
    sDpCap = 1;
    GsGpu_DpCapBegin();
    return 1;
}
static void dp_cap_end(uint32_t serial) {
    DpSlot *s = &sDp[serial & (DP_N - 1)];
    DpForm *f;
    void *gpu = NULL;
    sDpCap = 0;
    if (!sDpBad && sGifLoops == 0) {
        gpu = GsGpu_DpCapEnd();
    } else {
        GsGpu_DpCapAbort();
    }
    if (s->serial != serial) {
        GsGpu_DpFree(gpu);
        return;
    }
    if (gpu == NULL) {
        s->refused = 1;
        gVitaDp[4]++;
        return;
    }
    {   /* an empty form, or the one used least lately */
        int k2;
        f = &s->f[0];
        for (k2 = 0; k2 < DP_FORMS; k2++) {
            if (s->f[k2].gpu == NULL) {
                f = &s->f[k2];
                break;
            }
            if (s->f[k2].hits < f->hits) {
                f = &s->f[k2];
            }
        }
    }
    GsGpu_DpFree(f->gpu);
    memcpy(f->entry, sDpEntry, sizeof(sDpEntry));
    dp_snap(f->exit);
    f->cmp = sDpCmp;
    f->wr = sDpWr;
    {   /* (the fields compared, packed: dp_match) */
        uint64_t m = sDpCmp;
        f->n = 0;
        while (m != 0) {
            int i = __builtin_ctzll(m);
            f->idx[f->n] = (uint8_t)i;
            f->val[f->n] = sDpEntry[i];
            f->n++;
            m &= m - 1;
        }
    }
    f->gpu = gpu;
    f->hits = 0;
    {   /* (the forms not taken now age: the ones that stay are the ones in use) */
        int k2;
        for (k2 = 0; k2 < DP_FORMS; k2++) {
            s->f[k2].hits /= 2;
        }
    }
    gVitaDp[3]++;
    {
        static unsigned logged;
        if (logged < 12 || (logged < 40 && s->made > 0)) {
            extern void Vita_Log(const char *fmt, ...);
            logged++;
            Vita_Log("gs: part %u kept as groups (form %d, kept %u times before): reads %08x%08x first, writes %08x%08x\n", (unsigned)serial, (int)(f - s->f),
                     (unsigned)s->made, (unsigned)(f->cmp >> 32), (unsigned)f->cmp, (unsigned)(f->wr >> 32), (unsigned)f->wr);
        }
    }
    if (++s->made > 4 * DP_FORMS && s->made * 16 > s->hits) { /* kept again and again: its state keeps changing */
        s->refused = 1;
    }
}
#endif

/* ------------------------------------------------------------------------------------------------ VIF1 */

/* One run of VIF data (32-bit words): GIF data (DIRECT) goes to the GS, everything else to VU1 (gs_vu1.c).
   A command's data may continue in the next run (the command word often sits in the DMA tag, its data behind):
   DIRECT data is passed on as it comes, the data of the other commands is collected first. */
__attribute__((hot)) static void vif(const uint32_t *w, uint32_t count) {
    static uint32_t direct;                    /* quadwords of GIF data still to come */
    static uint32_t pend, need, have, buf[2048]; /* a command waiting for `need` words of data */
    uint32_t i = 0;

    while (i < count) {
        uint32_t code, cmd, num, imm;

        if (direct != 0) {
            uint32_t qwc = (count - i) / 4 < direct ? (count - i) / 4 : direct;
            if (qwc == 0) {
                break; /* misaligned run: drop it */
            }
            sStat[2] += qwc;
            VPROF(3);
#ifdef PORT_VITA
            if (gGsSplit) { /* in the game's list buffer or a copy of a piece of it: copied (the game writes that again
                                once the reader is through the list); elsewhere (textures, models): referred to, as before */
                extern int Port_DmaInList(uint32_t a), Port_InSnapArena(const void *p);
                GZM2(21);
                if (Port_DmaInList((uint32_t)(uintptr_t)&w[i]) || Port_InSnapArena(&w[i])) {
                    /* (copied: the drawing thread reads the copy. An earlier version: the image transfers' numbers worked out
                       here on the copy too, where the drawing thread will look for them; they were the drawing
                       thread's, 54 a picture at 43 us each in the busiest fight of a test) */
                    GsFE_GifHashed((const uint8_t *)&w[i], qwc);
                } else {
                    fe_hash_gif((const uint8_t *)&w[i], qwc, 1); /* (the image transfers' numbers worked out here: up_hash_quick) */
                    gr_simple(GR_GIFP, (uint32_t)(uintptr_t)&w[i], qwc, 0);
                }
                GZM2(23);
            } else
#endif
            Gs_Gif((const uint8_t *)&w[i], qwc);
            VPROF(1);
            i += qwc * 4;
            direct -= qwc;
            continue;
        }
        if (need != 0) {
            uint32_t n = count - i < need - have ? count - i : need - have;
            if (have + n <= 2048) {
                memcpy(&buf[have], &w[i], n * 4);
            }
            have += n;
            i += n;
            if (have == need) {
                uint32_t c = (pend >> 24) & 0x7F, pn = (pend >> 16) & 0xFF;
                if (need <= 2048) {
                    if (c == 0x20) { GsVu1_SetMask(buf[0]); }
                    else if (c == 0x30) { GsVu1_SetRow(buf); }
                    else if (c == 0x31) { GsVu1_SetCol(buf); }
                    else if (c == 0x4A) { GsVu1_Program(pend & 0xFFFF, buf, need); }
                    else {
#ifdef PORT_VITA
                        extern int gVitaUnpackVolatile; /* gs_vu1.c: this data is gone after the call */
                        gVitaUnpackVolatile = 1;
#endif
                        VPROF(2);
                        GZM2(21);
                        GsVu1_Unpack(c, pn, pend & 0xFFFF, buf);
                        GZM2(22);
                        VPROF(1);
#ifdef PORT_VITA
                        gVitaUnpackVolatile = 0;
#endif
                    }
                }
                need = have = 0;
            }
            continue;
        }
        code = w[i++];
        cmd = (code >> 24) & 0x7F;
        num = (code >> 16) & 0xFF;
        imm = code & 0xFFFF;
        sStat[1]++;
        pend = code;
        if (sDumpFrame == (int)sFrame && cmd != 0 && cmd != 0x50) {
            fprintf(stderr, "  vif %02x num %3u imm %04x\n", cmd, num, imm);
        }
        switch (cmd) {
        case 0x01: GsVu1_SetCycle(imm & 0xFF, (imm >> 8) & 0xFF); break;
        case 0x02: GsVu1_SetOffset(imm); break;
        case 0x03: GsVu1_SetBase(imm); break;
        case 0x04: GsVu1_SetItop(imm); break;
        case 0x05: GsVu1_SetMode(imm); break;
        case 0x14: case 0x15: GsVu1_Call((int)imm); break;
        case 0x17: GsVu1_Call(-1); break;
        case 0x20: need = 1; break;
        case 0x30: case 0x31: need = 4; break;
        case 0x4A: need = (num ? num : 256) * 2; break;
        case 0x50: case 0x51: direct = imm ? imm : 65536; break;
        default:
            if (cmd >= 0x60) {
                need = GsVu1_UnpackWords(cmd, num);
#ifdef PORT_VITA
                /* the data is nearly always all here: unpacked from where it lies, not copied first. And in a frame
                   that is not drawn, a batch's own data (addressed from TOPS, read only by the program call that
                   follows it, which is not made) is not unpacked at all */
                if (need != 0 && need <= count - i) {
                    extern unsigned gVitaUnpackSkipped;
                    if (gVitaGsSkipF && (imm & 0x8000)) {
                        gVitaUnpackSkipped += num ? num : 256;
                    } else {
                        VPROF(2);
                        GZM2(21);
                        GsVu1_Unpack(cmd, num, imm, &w[i]);
                        GZM2(22);
                        VPROF(1);
                    }
                    i += need;
                    need = 0;
                }
#endif
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ DMA and frames */

static void screenshot(void) {
    static int every = -1;
    static uint64_t shown; /* FRAME register of the buffer that received the most pixels */
    Target *best = NULL;
    char name[64];
    FILE *fp;
    int i, x, y;

    if (every < 0) {
        every = getenv("BT3_SHOT") != NULL ? atoi(getenv("BT3_SHOT")) : 0;
    }
    {
        extern int gGsMainFbp; /* the frame's own buffer (sceGsSwapDBuff), else the one that received most pixels */
        for (i = 0; i < sTargetCount; i++) {
            if (gGsMainFbp >= 0 ? sTargets[i].fbp == (uint32_t)gGsMainFbp : (best == NULL || sTargets[i].drawn > best->drawn)) {
                best = &sTargets[i];
            }
        }
    }
    if (every > 0 && best != NULL && !sGpu && sFrame % (unsigned)every == 0) {
        uint64_t fr = (gs.frame[0] & 0x1FF) == best->fbp ? gs.frame[0] : (gs.frame[1] & 0x1FF) == best->fbp ? gs.frame[1] : shown;
        uint32_t fbw = (fr >> 16) & 0x3F, fpsm = (fr >> 24) & 0x3F;

        shown = fr;
        snprintf(name, sizeof(name), "port/build/shots/frame_%05u.ppm", sFrame);
        fp = fopen(name, "wb");
        if (fp != NULL) {
            fprintf(fp, "P6\n640 448\n255\n");
            for (y = 0; y < 448; y++) {
                for (x = 0; x < 640; x++) {
                    uint32_t c = x < (int)(fbw ? fbw : 10) * 64 ? vram_rw(best->fbp << 5, fbw ? fbw : 10, fpsm, (uint32_t)x, (uint32_t)y, 0, 0) : 0;
                    if (Gs_PsmBits(fpsm) == 16) {
                        c = (c & 31) << 3 | ((c >> 5) & 31) << 11 | ((c >> 10) & 31) << 19;
                    }
                    fwrite(&c, 1, 3, fp);
                }
            }
            fclose(fp);
        }
    }
#ifdef PORT_VITA /* the finished frame to the Vita's screen (port/vita/vita_gfx.c) */
    {
        extern void Vita_GsPresent(uint32_t (*read)(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y), uint32_t bp, uint32_t fbw, uint32_t psm);
        uint32_t fbp = best != NULL ? best->fbp : 0;
        uint64_t fr = (gs.frame[0] & 0x1FF) == fbp ? gs.frame[0] : (gs.frame[1] & 0x1FF) == fbp ? gs.frame[1] : shown;
        uint32_t fbw = (fr >> 16) & 0x3F;

        shown = fr;
        Vita_GsPresent(Gs_VramRead, fbp << 5, fbw ? fbw : 8, (fr >> 24) & 0x3F);
    }
#endif
    for (i = 0; i < sTargetCount; i++) {
        sTargets[i].drawn = 0;
    }
    sFrame++;
}

/* ---- The render thread (GPU back end only).
   On the PS2 the frame's list is executed by DMA while the CPU already runs the next frame's game logic; the game
   is built for that (two list buffers, and it waits for the previous list before sending the next one). The PC
   build does the same: the game thread hands a finished list to this thread and goes on; anything else that
   touches the GS (uploads on the GIF channel, the per-frame clear) first waits until the thread is idle, which
   is the game's own sceGsSyncPath point. The window and all drawing belong to this thread. ---- */
static pthread_t sThread;
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sWake = PTHREAD_COND_INITIALIZER, sDone = PTHREAD_COND_INITIALIZER;
static int sThreaded;
static int sJob;            /* 0 none, 1 start the GPU back end, 2 run a list */
static uint32_t sJobTadr;
static int sJobTte, sInitResult;
static void run_chain(uint32_t tadr, int tte);

static uint32_t sJobQwc;
#ifdef PORT_VITA
static int sAsync, sFresh;
static uint32_t sSnapCountA[3];
static void snap_make(uint32_t tadr, int arena);
#endif
static int sWaited;         /* the game had to wait for a list (Vita: the next one is not drawn) */
static void gif_channel(uint32_t addr, uint32_t qwc, int chain);
#ifdef PORT_VITA
extern volatile unsigned char gProfGame; /* vita_vgl.c: 2 while the game waits for this thread */
#endif

#ifdef PORT_VITA
/* The Vita's render thread takes its work from a queue, in the order the game gave it: lists, and the small GS
   packets between them (the frame's clear and draw environment). The game keeps THREE list buffers there
   (src/sys/dma.c), so two lists may be unfinished while it writes the third: a list that takes longer than a frame
   to draw does not stop the game, as long as the lists after it are quick (they are not drawn). */
typedef struct RJob {
    int kind;            /* 1 start the back end, 2 a list, 3 a GIF transfer read in place, 4 a copied GIF packet */
    uint32_t a, qwc;
    int c, skip, fbp, arena;
    int setskip;         /* kinds 3 and 4: -1, or "the frame this belongs to is not drawn" */
    unsigned vb;         /* kind 2: the console's blank of the game's tick before it (vita_main.c), for the picture's showing */
    unsigned long long tsub; /* kind 2: when the game gave it */
    int mark;            /* kind 2: a picture of it for the log, asked for by the game's thread */
    uint32_t data[32 * 4];
} RJob;
volatile int gVitaShotMark; /* port_fast.c: the list being made is to be pictured (1 tinted, 2 as drawn) */
#define RQ 8
static RJob sQ[RQ];
static unsigned sQHead, sQCount;
static int sLists;       /* lists in the queue or being drawn */
static int sDrawnLists;  /* those of them that are drawn (the others only read: a few ms each) */
static unsigned long long sDrawStart; /* when the drawn list in hand was begun (0: none is) */
static unsigned sDrawAvg = 60000;     /* what a drawn list takes, in microseconds (a running average) */
static int sSnapCur;     /* the copies that belong to the list being drawn */
extern int gGsMainFbp;

int sceKernelChangeThreadCpuAffinityMask(SceUID thid, int mask);
static void *render_thread(void *arg) {
    (void)arg;
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x00020000); /* (a core of its own: the second) */
    { extern void Vita_PerfThreadStart(void); Vita_PerfThreadStart(); } /* (the zones measure the list reader now: its counters started too) */
    {
        extern int gFeThreadId;
        extern void Vita_WatchThread(int slot, int id, const char *name);
        gFeThreadId = sceKernelGetThreadId();
        Vita_WatchThread(1, gFeThreadId, "list reader");
    }
    pthread_mutex_lock(&sLock);
    for (;;) {
        RJob *job;

        while (sQCount == 0) {
            pthread_cond_wait(&sWake, &sLock);
        }
        job = &sQ[sQHead]; /* (stays where it is: the game's thread only adds behind it) */
        pthread_mutex_unlock(&sLock);
        if (gGsSplit) { /* the front end: the list is read here, the GS work goes to the back end's queue */
            if (job->kind == 1) {
                gr_simple(GR_INIT, 0, 0, 0);
            } else if (job->kind == 2) {
                extern unsigned long long gVitaFeUs[2][2];
                unsigned long long f0 = sceKernelGetProcessTimeWide();
                extern void Vita_FeListBegin(int drawn), Vita_FeListEnd(void);
                Vita_FeListBegin(!job->skip);
                gVitaGsSkipF = job->skip;
                gr_simple(GR_SKIP, (uint32_t)job->skip, 0, 0);
                sFeListSeq++;
                sFeGifLeft = 0;
                {
                    extern void Vita_PaceFe(unsigned seq, unsigned vb, unsigned long long tsub, int end);
                    Vita_PaceFe(sFeListSeq, job->vb, job->tsub, 0);
                }
                gr_simple(GR_LISTBEGIN, (uint32_t)!job->skip, sFeListSeq, job->vb);
                {   /* (the changes for this list, decided as it is read; the drawing thread is told) */
                    extern void Vita_T116FeList(int drawn, int mark, unsigned *var, unsigned *shot, unsigned *list);
                    unsigned tv, ts, tl;
                    Vita_T116FeList(!job->skip, job->mark, &tv, &ts, &tl);
                    if (ts != 0) {
                        gr_simple(GR_T116, tv, ts, tl);
                    }
                }
                sSnapCur = job->arena;
                run_chain(job->a, job->c);
                { extern void Vita_PaceFe(unsigned seq, unsigned vb, unsigned long long tsub, int end); Vita_PaceFe(sFeListSeq, job->vb, 0, 1); }
                { extern void Vita_T116FeListEnd(void); Vita_T116FeListEnd(); }
                gr_simple(GR_LISTEND, (uint32_t)!job->skip, 0, 0);
                Vita_FeListEnd();
                gVitaFeUs[!job->skip][0]++;
                gVitaFeUs[!job->skip][1] += sceKernelGetProcessTimeWide() - f0;
                pthread_mutex_lock(&sLock);
                sCcLists++;
                cc_retire(NULL);
                sLists--; /* the reader is through the list: nothing the back end does reads the game's list buffer now */
                pthread_cond_broadcast(&sDone);
                pthread_mutex_unlock(&sLock);
            } else {
                if (job->setskip >= 0) {
                    gVitaGsSkipF = job->setskip;
                    gr_simple(GR_SKIP, (uint32_t)job->setskip, 0, 0);
                }
                if (job->kind == 3) {
                    gr_simple(GR_GIFCH, job->a, job->qwc, (uint32_t)job->c);
                } else {
                    if (job->fbp >= 0) {
                        gr_simple(GR_FBP, (uint32_t)job->fbp, 0, 0);
                    }
                    GsFE_Gif((const uint8_t *)job->data, job->qwc);
                }
            }
            pthread_mutex_lock(&sLock);
            sQHead = (sQHead + 1) % RQ;
            sQCount--;
            pthread_cond_broadcast(&sDone);
            continue;
        }
        if (job->kind == 1) {
            sInitResult = GsGpu_Init();
        } else if (job->kind == 2) {
            extern int gVitaGsSkip;
            gVitaGsSkip = gVitaGsSkipF = job->skip;
            sSnapCur = job->arena;
            if (!job->skip) {
                unsigned long long t0 = sceKernelGetProcessTimeWide();
                unsigned took;
                pthread_mutex_lock(&sLock);
                sDrawStart = t0;
                pthread_mutex_unlock(&sLock);
                run_chain(job->a, job->c);
                took = (unsigned)(sceKernelGetProcessTimeWide() - t0);
                pthread_mutex_lock(&sLock);
                sDrawStart = 0;
                sDrawAvg = (sDrawAvg * 3 + (took > 200000 ? 200000 : took)) / 4;
                pthread_mutex_unlock(&sLock);
            } else {
                run_chain(job->a, job->c);
            }
        } else if (job->kind == 3) {
            if (job->setskip >= 0) {
                extern int gVitaGsSkip;
                gVitaGsSkip = gVitaGsSkipF = job->setskip;
            }
            gif_channel(job->a, job->qwc, job->c);
        } else {
            if (job->setskip >= 0) {
                extern int gVitaGsSkip;
                gVitaGsSkip = gVitaGsSkipF = job->setskip;
            }
            if (job->fbp >= 0) {
                gGsMainFbp = job->fbp;
            }
            Gs_Gif((const uint8_t *)job->data, job->qwc);
        }
        pthread_mutex_lock(&sLock);
        if (job->kind == 2) {
            sLists--;
            sDrawnLists -= !job->skip;
        }
        sQHead = (sQHead + 1) % RQ;
        sQCount--;
        pthread_cond_broadcast(&sDone);
    }
    return NULL;
}

static unsigned long long sGrIdleFrom;
static int sGrInList;
unsigned gGrIdleUs; /* for the log */
static void frame_tail(void);
/* The back end: carries out the front end's queue in order. */
extern volatile int gVitaZoneOn;
extern void Vita_Z(int z);
#define GZM(z) do { if (gVitaZoneOn) { Vita_Z(z); } } while (0)
static float sGrConsts[96] __attribute__((aligned(16))); /* the strips' constants as the back end holds them */
int gVitaShadeLog; /* bt3.txt shadelog=1: the fighters' shading layers in the log (an earlier version's) */
unsigned long long gVitaGrCyc[16]; /* the drawing thread's cycles by record, lists measured part by part */
unsigned gVitaGrN[16];
static void gr_exec(const GRec *r) {
    GZM(19); /* (between records: the ring, the loop) */
    switch (r->type) {
    case GR_GIFP: Gs_Gif((const uint8_t *)(uintptr_t)r->a, r->b); GZM(20); break;
    case GR_GIFC: Gs_Gif((const uint8_t *)(r + 1), r->b); GZM(20); break;
    case GR_REG: reg_write(r->a, r->d); GZM(21); break;
    case GR_VU: {
        extern int gVglSamePos;
        float *consts = sGrConsts;
        if (sDpCap) {
            dp_rd(dp_strip_reads((int)r->c < 0 ? (int)((gs.prim >> 9) & 1) : (int)r->c));
        }
        uint32_t cb = (r->flags & 8) ? 0 : 384;
        const float *v = (r->flags & 4) ? (const float *)(const void *)((const uint8_t *)(r + 1) + cb) : (const float *)(uintptr_t)r->d;
        if (cb != 0) {
            memcpy(consts, r + 1, 384);
            { extern unsigned gVglConstGen; gVglConstGen++; }
        }
        GZM(22);
        int ctx = (int)r->c < 0 ? (int)((gs.prim >> 9) & 1) : (int)r->c;
        extern const void *gVglPlan;
        gVglVertsStable = r->flags & 1;
        gVglSamePos = (r->flags >> 1) & 1;
        gVglPlan = (r->flags & 16) ? (const void *)((const uint8_t *)(r + 1) + cb + ((r->flags & 4) ? r->e * (r->a == 6 ? 64u : 48u) : 0)) : NULL;
        if (r->a == 0 && r->b == 2 && gVglPlan != NULL && !sDpCap) { /* a fighter's silhouette into the work page: the direct draws */
            extern int GsGpu_DsSil(int ctx, uint32_t count, const float *c, const void *plan);
            if (GsGpu_DsSil(ctx, r->e, consts, gVglPlan)) {
                gVglPlan = NULL;
                gVglVertsStable = 0;
                gVglSamePos = 0;
                GZM(23);
                break;
            }
        }
        if (r->flags & 32) { /* (the general way wants the indices behind the plan: written out, as GR_VU2 does) */
            static uint8_t full[32 + 1024 * 3 * 2] __attribute__((aligned(8)));
            const ChipPlan *pp = (const ChipPlan *)gVglPlan;
            uint16_t *ix = (uint16_t *)(void *)(full + 32);
            uint32_t i;
            memcpy(full, gVglPlan, 32);
            for (i = 0; i + 2 < pp->count && i < 1022; i++, ix += 3) {
                ix[0] = (uint16_t)(pp->at + i); ix[1] = (uint16_t)(pp->at + i + 1); ix[2] = (uint16_t)(pp->at + i + 2);
            }
            gVglPlan = full;
        }
        if (r->a == 0) { GsGpu_DrawVu0((int)r->b, ctx, v, r->e, consts); }
        else if (r->a == 4) { GsGpu_DrawVu4(ctx, v, r->e, consts); }
        else { GsGpu_DrawVu6(ctx, v, r->e, consts); }
        GZM(23); /* (the rest of the draw: after the last mark inside it) */
        gVglPlan = NULL;
        break;
    }
    case GR_VU2: { /* program 0's two layers (GsFE_DrawVu0Pair): TEX0, PRIM and the strip, twice, with one plan */
        extern int gVglSamePos;
        extern const void *gVglPlan;
        const Vu2 *x = (const Vu2 *)(const void *)(r + 1);
        uint32_t cb = (r->flags & 8) ? 0 : 384;
        const uint8_t *plan = (const uint8_t *)(r + 1) + 48 + cb;
        static uint8_t full[32 + 1024 * 3 * 2] __attribute__((aligned(8))); /* (the plan with its indices, for the general way) */
        int haveFull = !(r->flags & 32);
        const float *v = (const float *)(uintptr_t)r->d;
        int k;
        if (cb != 0) {
            memcpy(sGrConsts, (const uint8_t *)(r + 1) + 48, 384);
            { extern unsigned gVglConstGen; gVglConstGen++; }
        }
        GZM(22);
        for (k = 0; k < 2; k++) {
            extern int GsGpu_DsLayer(int layer, int ctx, uint32_t count, const float *c, const void *plan);
            reg_write(x->texa[k], x->texv[k]);
            reg_write(0, x->prim[k]);
            if (k == 1 && gVitaShadeLog) { /* the fighters' shading layer: its ramp, once for each texture (Frieza "too dark"; 219: bt3.txt shadelog=1) */
                static uint64_t seen[24];
                static int nSeen;
                int ctx1 = (int)x->ctx[k] < 0 ? (int)((gs.prim >> 9) & 1) : (int)x->ctx[k], j;
                uint64_t t0 = gs.tex0[ctx1];
                for (j = 0; j < nSeen && seen[j] != t0; j++) {
                }
                if (j == nSeen && nSeen < 24) {
                    extern void Vita_Log(const char *fmt, ...);
                    const float *c = (const float *)(const void *)sGrConsts;
                    uint32_t tw = 1u << ((t0 >> 26) & 15), th = 1u << ((t0 >> 30) & 15), u;
                    char line[400];
                    int len;
                    seen[nSeen++] = t0;
                    len = snprintf(line, sizeof(line), "vita: shading layer (b206): TEX0 %08x%08x %ux%u CLAMP %llx TEX1 %llx ALPHA %llx PRIM %03x | light %.3f %.3f %.3f %.3f | colours %.0f %.0f %.0f %.0f / %.0f %.0f %.0f %.0f | ramp:",
                                   (unsigned)(t0 >> 32), (unsigned)t0, tw, th, (unsigned long long)gs.clamp[ctx1], (unsigned long long)gs.tex1[ctx1],
                                   (unsigned long long)gs.alpha[ctx1], (unsigned)(gs.prim & 0x7FF), c[40], c[44], c[48], c[52], c[88], c[89], c[90], c[91], c[92], c[93], c[94], c[95]);
                    for (u = 0; u < 16 && len > 0 && len < (int)sizeof(line) - 12; u++) {
                        len += snprintf(line + len, sizeof(line) - (unsigned)len, " %06x", (unsigned)(texel(ctx1, (int)(u * tw / 16u), (int)(th / 2u)) & 0xFFFFFF));
                    }
                    Vita_Log("%s\n", line);
                }
            }
            if (sDpCap) {
                dp_rd(dp_strip_reads((int)x->ctx[k] < 0 ? (int)((gs.prim >> 9) & 1) : (int)x->ctx[k]));
            } else if (GsGpu_DsLayer(k, (int)x->ctx[k], r->e, sGrConsts, plan)) {
                continue; /* (straight to the direct draws) */
            }
            if (!haveFull) { /* the general way wants the indices behind the plan: written out (a strip's are simple) */
                const ChipPlan *pp = (const ChipPlan *)(const void *)plan;
                uint16_t *ix = (uint16_t *)(void *)(full + 32);
                uint32_t i;
                memcpy(full, plan, 32);
                for (i = 0; i + 2 < pp->count && i < 1022; i++, ix += 3) {
                    ix[0] = (uint16_t)(pp->at + i); ix[1] = (uint16_t)(pp->at + i + 1); ix[2] = (uint16_t)(pp->at + i + 2);
                }
                plan = full;
                haveFull = 1;
            }
            gVglVertsStable = 1;
            gVglSamePos = k == 1;
            gVglPlan = plan;
            GsGpu_DrawVu0(k, (int)x->ctx[k], v, r->e, sGrConsts);
            gVglPlan = NULL;
        }
        GZM(23);
        break;
    }
    case GR_PART: { /* a model part as one record: its constants once, then the strips the front end kept (gs_core.c cc_replay) */
        const uint8_t *p = (const uint8_t *)(uintptr_t)r->a, *q = p + r->b;
        int cap;
        {   /* (its kept form fetched while the constants are copied) */
            const uint8_t *sl = (const uint8_t *)&sDp[r->c & (DP_N - 1)];
            const DpSlot *ds = (const DpSlot *)(const void *)sl;
            int fk;
            __builtin_prefetch(sl);
            for (fk = 0; fk < DP_FORMS; fk++) { /* (each form's numbers and the first of its values) */
                __builtin_prefetch(&ds->f[fk]);
                __builtin_prefetch((const uint8_t *)&ds->f[fk] + 32);
                __builtin_prefetch(&ds->f[fk].val[4]);
            }
        }
        memcpy(sGrConsts, r + 1, 384);
        { extern unsigned gVglConstGen; gVglConstGen++; }
        if (dp_part(r->c, sGrConsts)) {
            GZM(1);
            break; /* drawn from its kept form */
        }
        cap = dp_cap_begin(r->c);
        while (p < q) {
            const GRec *s = (const GRec *)(const void *)p;
            gr_exec(s);
            p += s->size;
        }
        if (cap) {
            dp_cap_end(r->c);
        }
        break;
    }
    case GR_T116: { extern void GsGpu_T116(unsigned var, unsigned shot, unsigned list); GsGpu_T116(r->a, r->b, r->c); break; }
    case GR_CHIPRESET: { /* (the chip's copies of the last fight are written over next) */
        GsGpu_DpReset(); /* (draws what waits, waits for the chip) */
        dp_clear();
        break;
    }
    case GR_SKIP: { extern int gVitaGsSkip; gVitaGsSkip = (int)r->a; break; }
    case GR_FBP: gGsMainFbp = (int)r->a; break;
    case GR_FRAMEEND: frame_tail(); GZM(2); break;
    case GR_LISTBEGIN:
        sGrInList = (int)r->a;
        sBeListSeq = r->b; /* (the list reader's number for it: its upload numbers, up_hash_quick) */
        { extern void Vita_PaceBe(unsigned seq, unsigned vb, int end); Vita_PaceBe(r->b, r->c, 0); }
        {
            extern void Vita_PerfListBegin(void);
            Vita_PerfListBegin(); /* (the back end is what is measured with the list split) */
        }
        if (r->a) {
            pthread_mutex_lock(&sLock);
            sDrawStart = sceKernelGetProcessTimeWide();
            pthread_mutex_unlock(&sLock);
        }
        break;
    case GR_LISTEND: {
        extern void Vita_ListDone(unsigned us);
        static unsigned long long last;
        extern void Vita_PerfListEnd(void);
        unsigned long long now;
        sGrInList = 0;
        { extern void Vita_PaceBe(unsigned seq, unsigned vb, int end); Vita_PaceBe(sBeListSeq, 0, 1); }
        Vita_PerfListEnd();
        now = sceKernelGetProcessTimeWide();
        pthread_mutex_lock(&sLock);
        if (r->a && sDrawStart != 0) {
            extern unsigned gVitaBeList[3];
            unsigned took = (unsigned)(now - sDrawStart);
            sDrawAvg = (sDrawAvg * 3 + (took > 200000 ? 200000 : took)) / 4;
            if (took > gVitaBeList[0]) {
                gVitaBeList[0] = took; /* (for the log: the longest drawn list, and those over 30 / 33 ms) */
            }
            gVitaBeList[1] += took > 30000;
            gVitaBeList[2] += took > 33000;
        }
        sDrawStart = 0;
        sDrawnLists -= (int)r->a; /* (the list itself was counted off by the reader: the game waits only for that) */
        sBeListDone = sBeListSeq; /* (the kept copies let go before it may be freed: cc_retire) */
        pthread_cond_broadcast(&sDone);
        pthread_mutex_unlock(&sLock);
        Vita_ListDone(last != 0 ? (unsigned)(now - last) : 0);
        last = now;
        GZM(3);
        break;
    }
    case GR_INIT: sInitResult = GsGpu_Init(); break;
    case GR_GIFCH: gif_channel(r->a, r->b, (int)r->c); break;
    default: break;
    }
}

static void *gr_thread(void *arg) {
    unsigned idle = 0;
    (void)arg;
    {
        extern void Vita_PerfThreadStart(void);
        extern int gGrThreadId;
        Vita_PerfThreadStart();
        gGrThreadId = sceKernelGetThreadId();
        sceKernelChangeThreadCpuAffinityMask(gGrThreadId, 0x00040000); /* (a core of its own: the third) */
        { extern void Vita_WatchThread(int slot, int id, const char *name); Vita_WatchThread(2, gGrThreadId, "drawing"); }
    }
    for (;;) {
        unsigned h = sGrHead;
        if (h == sGrTail) {
            if (idle == 0) {
                sGrIdleFrom = sceKernelGetProcessTimeWide();
            }
            if (++idle > 2000) {
                extern unsigned long long gVitaWall[6];
                unsigned long long w0 = sceKernelGetProcessTimeWide();
                sceKernelDelayThread(idle > 20000 ? 200 : 50);
                gVitaWall[4] += sceKernelGetProcessTimeWide() - w0;
            }
            continue;
        }
        if (idle != 0) {
            GZM(24); /* (waiting for the front end) */
        }
        if (idle != 0 && sGrInList) {
            gGrIdleUs += (unsigned)(sceKernelGetProcessTimeWide() - sGrIdleFrom); /* (waited for the front end within a drawn list) */
        }
        idle = 0;
        __sync_synchronize();
        {   /* everything the front end has put up to now, its position given back now and then (the two cores did not
               pass the queue's two positions back and forth for every record) */
            unsigned t = sGrTail, n = 0;
            __sync_synchronize();
            while (h != t) {
                const GRec *r = (const GRec *)(const void *)&sGr[h % GR_SIZE];
                unsigned size = r->size;
                sGrCurType = r->type; /* (the watchdog) */
                __builtin_prefetch(&sGr[(h + size) % GR_SIZE]);
                __builtin_prefetch(&sGr[(h + size + 64) % GR_SIZE]);
                if (gVitaZoneOn & 1) {
                    extern int scePerfArmPmonGetCounterValue(int thid, unsigned int counter, unsigned int *value);
                    unsigned c0 = 0, c1 = 0, ty = r->type & 15;
                    scePerfArmPmonGetCounterValue(0, 31, &c0);
                    gr_exec(r);
                    scePerfArmPmonGetCounterValue(0, 31, &c1);
                    gVitaGrCyc[ty] += c1 - c0;
                    gVitaGrN[ty]++;
                } else {
                    gr_exec(r);
                }
                h += size;
                if (++n % 32 == 0) {
                    __sync_synchronize();
                    sGrHead = h;
                }
            }
            __sync_synchronize();
            sGrHead = h;
        }
    }
    return NULL;
}

/* For the watchdog (port/vita/vita_main.c): where the lists' two queues are. Reads only. */
void Gs_QueueState(char *b, int n) {
    snprintf(b, (size_t)n, "vita: WATCHDOG:   lists queued %u, unfinished %d (drawn %d); back end queue %u of %u bytes taken, record %d being done; frame %u\n",
             sQCount, sLists, sDrawnLists, sGrHead, sGrTail, sGrCurType, sFrame);
}

/* Blocks until the render thread has nothing to do. */
static void render_wait(void) {
    if (gGsSplit) {
        pthread_mutex_lock(&sLock);
        while (sQCount != 0) {
            gProfGame = 2;
            pthread_cond_wait(&sDone, &sLock);
        }
        pthread_mutex_unlock(&sLock);
        gr_drain();
        gProfGame = 0;
        return;
    }
    pthread_mutex_lock(&sLock);
    while (sQCount != 0) {
        gProfGame = 2;
        pthread_cond_wait(&sDone, &sLock);
    }
    gProfGame = 0;
    pthread_mutex_unlock(&sLock);
}

/* Blocks until at most `most` lists are unfinished; returns how many are. */
static int render_wait_lists(int most) {
    int n;

    pthread_mutex_lock(&sLock);
    while (sLists > most) {
        gProfGame = 2;
        pthread_cond_wait(&sDone, &sLock);
    }
    gProfGame = 0;
    n = sLists;
    pthread_mutex_unlock(&sLock);
    return n;
}

/* Blocks until at most `most` drawn lists are unfinished by the DRAWING thread (render_wait_lists counts
   the list reader's progress, which the game's waiting in the intro (warm=, introwait=) went by: the drawing thread
   could fall lists behind, its pictures shown late, and the kept copies it walked freed under it) */
static void render_wait_drawn(int most) {
    pthread_mutex_lock(&sLock);
    while (sDrawnLists > most) {
        gProfGame = 2;
        pthread_cond_wait(&sDone, &sLock);
    }
    gProfGame = 0;
    pthread_mutex_unlock(&sLock);
}

/* Is a list that is drawn still so far from finished that the frame coming now should not be drawn as well? (One
   that is nearly done does not count: the list after it waits those few milliseconds in the queue, and every other
   frame can be drawn instead of every third.) */
static int render_drawing(void) {
    int busy;

    pthread_mutex_lock(&sLock);
    if (gGsSplit) {
        /* with the list split the two halves work on two lists at once (the reader on the next, the drawer on
           this one): one drawn list in flight does not stop the next from being drawn, two do */
        /* the game no longer waits for the drawer (a GPU test: it waits for the reader only), so one list may be read
           while the one before is drawn: the next is skipped only when two are in flight (a GPU test tried this too, but
           then every texture was copied into the queue, which made every list slow) */
        busy = sDrawnLists > 1;
    } else if (sDrawnLists == 0) {
        busy = 0;
    } else if (sDrawnLists > 1 || sDrawStart == 0) {
        busy = 1;
    } else {
        unsigned long long gone = sceKernelGetProcessTimeWide() - sDrawStart;
        busy = gone + 12000 < sDrawAvg;
    }
    pthread_mutex_unlock(&sLock);
    return busy;
}

static void render_put(const RJob *job) {
    pthread_mutex_lock(&sLock);
    while (sQCount == RQ) {
        pthread_cond_wait(&sDone, &sLock);
    }
    sQ[(sQHead + sQCount) % RQ] = *job;
    sQCount++;
    if (job->kind == 2) {
        sLists++;
        sDrawnLists += !job->skip;
    }
    pthread_cond_signal(&sWake);
    pthread_mutex_unlock(&sLock);
}

static void render_post(int kind, uint32_t a, int c) {
    static RJob job; /* (the game's thread only) */
    job.kind = kind;
    job.a = a;
    job.c = c;
    job.setskip = -1;
    render_put(&job);
}
#else
static void *render_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&sLock);
    for (;;) {
        while (sJob == 0) {
            pthread_cond_wait(&sWake, &sLock);
        }
        if (sJob == 1) {
            sInitResult = GsGpu_Init();
        } else {
            uint32_t tadr = sJobTadr, qwc = sJobQwc;
            int tte = sJobTte, job = sJob;
            pthread_mutex_unlock(&sLock);
            if (job == 2) {
                run_chain(tadr, tte);
            } else {
                gif_channel(tadr, qwc, tte);
            }
            pthread_mutex_lock(&sLock);
        }
        sJob = 0;
        pthread_cond_broadcast(&sDone);
    }
    return NULL;
}

/* Blocks until the render thread has nothing to do. */
static void render_wait(void) {
    pthread_mutex_lock(&sLock);
    if (sJob != 0) {
        sWaited = 1;
#ifdef PORT_VITA
        gProfGame = 2;
#endif
    }
    while (sJob != 0) {
        pthread_cond_wait(&sDone, &sLock);
    }
#ifdef PORT_VITA
    gProfGame = 0;
#endif
    pthread_mutex_unlock(&sLock);
}

static void render_post(int job, uint32_t tadr, int tte) {
    pthread_mutex_lock(&sLock);
    sJob = job;
    sJobTadr = tadr;
    sJobTte = tte;
    pthread_cond_signal(&sWake);
    pthread_mutex_unlock(&sLock);
}

#endif

/* The render thread has finished what it was given (anything outside this file that changes what a list reads). */
void Port_GsSync(void) {
    if (sThreaded && !pthread_equal(pthread_self(), sThread)) {
        render_wait();
    }
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Which renderer: BT3_GS = "gpu" (the window), "1" (the software reference), "none" (nothing is drawn: tests).
   Without the variable a release program opens its window, so that it can be started by a double click; a
   developer's build draws nothing, as the checks expect. */
static const char *gs_mode(void) {
    extern volatile uint32_t gPortDataStripped; /* plat_mem.c: 1 in a release program */
    const char *m = getenv("BT3_GS");

    if (m == NULL) {
        return gPortDataStripped == 1 ? "gpu" : NULL;
    }
    return strcmp(m, "none") == 0 || strcmp(m, "off") == 0 || strcmp(m, "0") == 0 ? NULL : m;
}

static int gs_on(void) {
    static int on = -1;

    if (on < 0) {
        on = gs_mode() != NULL;
        tables_init();
#ifdef PORT_VITA
        {
            uint32_t b;
            for (b = 0; b < 16384; b++) {
                sBlk[b].dirty = 1; /* (ids from the content until an upload says otherwise) */
            }
        }
#endif
        gs.prmodecont = 1; /* the GS starts with the attributes in PRIM */
        sDrawFrom = getenv("BT3_GS_FROM") != NULL ? atoi(getenv("BT3_GS_FROM")) : 0;
        sDumpFrame = getenv("BT3_GS_DUMP") != NULL ? atoi(getenv("BT3_GS_DUMP")) : -1;
        if (getenv("BT3_GS_PROBE") != NULL) {
            sscanf(getenv("BT3_GS_PROBE"), "%d,%d", &sProbeX, &sProbeY);
        }
        if (on && strcmp(gs_mode(), "gpu") == 0) {
            /* BT3_GS_THREAD=1: the back end and the list run on the render thread (experimental: the game was seen
               to change data a list still refers to, which shows as a jumping camera). Default: this thread. */
            sThreaded = getenv("BT3_GS_THREAD") != NULL;
            if (sThreaded) {
#ifdef PORT_VITA
                {   /* (room for the shader compiler, which runs on this thread) */
                    pthread_attr_t at;
                    pthread_attr_init(&at);
                    pthread_attr_setstacksize(&at, 2u << 20);
                    pthread_create(&sThread, &at, render_thread, NULL);
                    {
                        extern int gVglSafe;
                        gGsSplit = !(gVglSafe & (512 | 16384)) && getenv("BT3_GS_NOSPLIT") == NULL;
                        if (gGsSplit) {
                            static pthread_t back;
                            pthread_create(&back, &at, gr_thread, NULL); /* (the shader compiler runs there) */
                        }
                    }
                }
#else
                pthread_create(&sThread, NULL, render_thread, NULL);
#endif
                render_post(1, 0, 0);
                render_wait();
                sGpu = sInitResult;
            } else {
                sGpu = GsGpu_Init();
            }
#ifdef PORT_VITA
            {
                extern int gVglSafe; /* vita_vgl.c; 32: every upload written at once, as before */
                sLazy = sGpu && !(gVglSafe & 32);
            }
#endif
        }
    }
    return on;
}

/* A transfer on the GIF channel itself (the game uses it for texture uploads outside the frame's list):
   `qwc` quadwords of GIF data at `addr`, or with chain = 1 a source chain of them starting at `addr`. */
void Port_GsGifChannel(uint32_t addr, uint32_t qwc, int chain) {

    if (!gs_on()) {
        return;
    }
    if (sThreaded) {
        render_wait(); /* the GS is one device: this transfer goes after the list that is being drawn */
#ifdef PORT_VITA /* and on the render thread, which is the only one that talks to the graphics chip */
        {
            static RJob job;
            job.kind = 3;
            job.a = addr;
            job.qwc = qwc;
            job.c = chain;
            job.setskip = -1;
            render_put(&job);
            render_wait();
        }
        return;
#endif
    }
    gif_channel(addr, qwc, chain);
}

#ifdef PORT_VITA
/* sceGsSwapDBuff (gs_sony.c): the frame's draw environment and clear, and which buffer the frame is drawn into.
   In its place in the queue, not at once: the lists before it are still to be drawn with the buffer before. */
int gVitaTriple = 1; /* three list buffers (SQUARE on the start screen: two, the picture one frame behind at most) */
int gVitaDmaFlush; /* src/sys/dma.c: the list that comes now is the frame's own (the game does not wait for it) */
static int sFreshPending;
static int sSwapSkip = -1; /* what the swap decided for the frame whose list comes next */
void Port_GsSwap(int fbp, uint32_t addr, uint32_t qwc) {
    extern int Vita_AsyncOk(void), Vita_SkipWanted(int async);
    extern int gVitaGsSkip;
    extern unsigned gVitaWaitSkips;
    static RJob job;

    if (!gs_on()) {
        return;
    }
    if (!sThreaded) {
        gVitaGsSkip = sSwapSkip = Vita_SkipWanted(0);
        gGsMainFbp = fbp;
        gif_channel(addr, qwc, 0);
        return;
    }
    if (qwc > 32 || !Vita_AsyncOk()) {
        render_wait();
        gGsMainFbp = fbp;
        job.kind = 3;
        job.a = addr;
        job.qwc = qwc;
        job.c = 0;
        job.setskip = sSwapSkip = Vita_SkipWanted(0);
        render_put(&job);
        render_wait();
        return;
    }
    if (sFreshPending) { /* the list before referred to something new: finished first, as on the console */
        sFreshPending = 0;
        render_wait();
    }
    /* is the frame drawn? Not if the game is late, and not while a list before it is still unfinished */
    sSwapSkip = Vita_SkipWanted(1);
    if (!sSwapSkip && render_drawing() > 0) {
        extern int Vita_WarmUp(void);
        extern unsigned gVitaWarmWaits;
        extern int Vita_FilmWait(void);
        if (Vita_WarmUp() || Vita_FilmWait()) { /* the stage's first seconds: the game waits for the picture, none is left out
                                                   (and the lists a test run films, filmwait=1) */
            render_wait_drawn(1); /* (the older of the two drawn lists done by the drawing thread: this one drawn after it) */
            gVitaWarmWaits++;
        } else {
            sSwapSkip = 1;
            gVitaWaitSkips++;
        }
    }
    job.kind = 4;
    job.fbp = fbp;
    job.qwc = qwc;
    job.setskip = sSwapSkip;
    memcpy(job.data, (const void *)(uintptr_t)addr, qwc * 16);
    render_put(&job);
}
#endif

static void gif_channel(uint32_t addr, uint32_t qwc, int chain) {
    int guard;

    if (!chain) {
        Gs_Gif((const uint8_t *)(uintptr_t)addr, qwc);
        return;
    }
    for (guard = 0; guard < 100000; guard++) {
        const uint32_t *tag = (const uint32_t *)(uintptr_t)addr;
        uint32_t n = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, ref = tag[1];

        if (id == 0 || id == 3 || id == 4) {
            Gs_Gif((const uint8_t *)(uintptr_t)ref, n);
            addr += 16;
        } else {
            Gs_Gif((const uint8_t *)(tag + 4), n);
            addr = id == 2 ? ref : addr + 16 + n * 16;
        }
        if (id == 0 || id == 7) {
            break;
        }
    }
}

/* Carries out a source-chain transfer on VIF1 that starts at `tadr` (PS2 address = host address). */
void Port_GsVif1Chain(uint32_t tadr, int tte) {
    static uint64_t next, start, sum, worst;
    static unsigned n, over;
    uint64_t t, tChain;

    if (!gs_on()) {
        return;
    }
    if (!sGpu) {
        run_chain(tadr, tte);
        return;
    }
    tChain = now_ns();
#ifdef PORT_VITA
    {   /* The frame's own list (Dma_Flush) is drawn while the game goes on, unless a film plays. It is not drawn
           at all if the game is late for its vertical blanks, or if a list before it is still unfinished. Any
           other list is finished when this returns, as the game expects. */
        extern int Vita_SkipWanted(int async), Vita_AsyncOk(void);
        extern int gVitaGsSkip;
        extern unsigned gVitaWaitSkips;
        static RJob job;
        static int arena;

        int own = gVitaDmaFlush;

        sAsync = sThreaded && gVitaDmaFlush && Vita_AsyncOk();
        gVitaDmaFlush = 0;
        if (!sThreaded) {
            gVitaGsSkip = sSwapSkip >= 0 ? sSwapSkip : Vita_SkipWanted(0);
            sSnapCur = 0;
            sSnapCountA[0] = 0;
            run_chain(tadr, tte);
        } else if (!sAsync) {
            render_wait();
            job.kind = 2;
            job.a = tadr;
            job.c = tte;
            job.skip = sSwapSkip >= 0 ? sSwapSkip : Vita_SkipWanted(0);
            job.arena = 0;
            job.vb = 0;
            job.tsub = 0; /* (a list the game waits for: its picture is shown when it is ready) */
            { extern volatile int gVitaShotMark; job.mark = __sync_lock_test_and_set(&gVitaShotMark, 0); }
            sSnapCountA[0] = 0;
            render_put(&job);
            render_wait();
        } else {
            render_wait_lists(gVitaTriple ? 1 : 0); /* the buffer the game writes next is free again (three list buffers, three copies: with the list split, two lists may be unfinished, one with each half) */
            job.kind = 2;
            job.a = tadr;
            job.c = tte;
            if (sSwapSkip >= 0) {
                job.skip = sSwapSkip; /* (as the frame's clear was told) */
            } else {
                job.skip = Vita_SkipWanted(1);
                if (render_drawing() > 0 && !job.skip) {
                    extern int Vita_WarmUp(void);
                    extern unsigned gVitaWarmWaits;
                    extern int Vita_FilmWait(void);
                    if (Vita_WarmUp() || Vita_FilmWait()) { /* (as Port_GsSwap) */
                        render_wait_drawn(1); /* (the older of the two drawn lists done by the drawing thread: this one drawn after it) */
                        gVitaWarmWaits++;
                    } else {
                        job.skip = 1;
                        gVitaWaitSkips++;
                    }
                }
            }
            arena = (arena + 1) % 3;
            job.arena = arena;
            {
                extern volatile unsigned gVitaGridVb;
                job.vb = gVitaGridVb;
                job.tsub = sceKernelGetProcessTimeWide();
            }
            { extern volatile int gVitaShotMark; job.mark = __sync_lock_test_and_set(&gVitaShotMark, 0); }
            snap_make(tadr, arena);
            render_put(&job);
            if (sFresh) {
                extern unsigned gVitaFresh;
                gVitaFresh++; /* (counted only: waiting for such a list cost the game a tenth of its time and cured nothing) */
            }
        }
        if (own) {
            sSwapSkip = -1; /* (the next frame's swap decides anew) */
        }
    }
#else
    if (sThreaded) {
        render_wait(); /* the previous list must be finished before this one starts (the PS2's sceGsSyncPath) */
    } else {
        run_chain(tadr, tte);
    }
#endif
    /* The frame's work ends here. Time it (BT3_GS_VERBOSE), then keep 30 frames per second. */
    t = now_ns();
    if (start != 0) {
        extern unsigned long long gPortSleptNs;
        uint64_t work = t - start - (uint64_t)gPortSleptNs; /* without the waiting for the vertical blank */
        gPortSleptNs = 0;
        {   /* a frame over budget: where the time went and what was created in it */
            extern unsigned gGpuNewTex, gGpuNewTexPixels, gGpuNewPipes;
            extern uint64_t gGpuTexNs, gGpuPipeNs, gGpuEndNs;
            if (work > 33366700ull && getenv("BT3_GS_VERBOSE") != NULL) {
                fprintf(stderr, "slow: frame %u: %.1f ms = game %.1f ms + display list %.1f ms; %u new textures (%u pixels), %u new pipelines\n",
                        sFrame, (double)work / 1e6, (double)(tChain - start) / 1e6, (double)(t - tChain) / 1e6, gGpuNewTex, gGpuNewTexPixels,
                        gGpuNewPipes);
                fprintf(stderr, "slow:   of the display list time: texture decoding %.1f ms, pipeline creation %.1f ms, submitting the frame %.1f ms\n",
                        (double)gGpuTexNs / 1e6, (double)gGpuPipeNs / 1e6, (double)gGpuEndNs / 1e6);
            }
            gGpuNewTex = gGpuNewTexPixels = gGpuNewPipes = 0;
            gGpuTexNs = gGpuPipeNs = gGpuEndNs = 0;
        }
        sum += work;
        if (work > worst) { worst = work; }
        if (work > 33366700ull) { over++; }
        if (++n == 60) {
            if (getenv("BT3_GS_VERBOSE") != NULL) {
                fprintf(stderr, "time: frame %u: work per frame: average %.1f ms, worst %.1f ms, %u of 60 over the 33.4 ms budget\n",
                        sFrame, (double)sum / 60e6, (double)worst / 1e6, over);
            }
            n = 0; sum = 0; worst = 0; over = 0;
        }
    }
    /* pacing is done at the vertical blank (Port_VBlank in plat_stub.c): 60 per second, the game waits for one
       per menu frame and two per battle frame */
    (void)next;
    start = now_ns();
#ifndef PORT_VITA
    if (sThreaded) {
        render_post(2, tadr, tte);
    }
#endif
}

#ifdef PORT_VITA
/* A list that is drawn while the game computes the next frame: the small pieces of data it REFERS to in the
   game's own memory (matrices, constants: the camera was seen to jump on PC without this) are copied when the
   list is handed over. The list's own two buffers and the large data (textures, models) are read in place. */
/* The pixels of an image of up to 256 quadwords (4 KB: the palettes, 1 KB each, before each piece of a
   fighter and each texture of the stage; small textures): the drawing thread read those in place, a list or more after the game handed it over, when the game
   may already have written the next frame's into them (a palette the game changes from frame to frame showed the
   next frame's colours: the sparkle of the water's ground differed from run to run of the same demo fight, test
   186-189). And the copies are found by their tag (a hash), not in order: a call replayed from its kept form is not
   read, and its pieces left every copy after it unused (read in place). */
#define SNAP_QWC 32 /* pieces of the list's own level (matrices, constants) up to this; inside a call (a model's
                          chain: its vertices, the same every frame) none: the chip keeps those by their place */
#define SNAP_IMG_QWC 256 /* an image's pixels (a palette, a small texture: the REF behind a GIF IMAGE tag) up to this, anywhere;
                            and GS packets the game sends from its own memory (a REF whose data opens with VIF DIRECT) */
#define SNAP_MAX 4096
#define SNAP_WORDS ((1024u << 10) / 4)
#define SNAP_HASH 8192
static struct { const uint32_t *tag; uint32_t off; } sSnapA[3][SNAP_MAX]; /* one set per list that may be unfinished */
static uint32_t sSnapDataA[3][SNAP_WORDS];
static uint16_t sSnapH[3][SNAP_HASH]; /* entry + 1 of sSnapA by the tag's address (open addressing) */
static inline uint32_t snap_h(const uint32_t *tag) { return ((uint32_t)(uintptr_t)tag * 0x9E3779B1u) >> 19; }
static uint32_t sSnapNext;
unsigned gVitaSnap[2]; /* for the log: pieces copied, bytes */
uint32_t gPortDmaBufs[3]; /* src/sys/dma.c: the game's three list buffers */
int Port_DmaInList(uint32_t a) {
    int i;
    for (i = 0; i < 3; i++) {
        if (gPortDmaBufs[i] != 0 && a - gPortDmaBufs[i] < 0x100000u) {
            return 1;
        }
    }
    return 0;
}
int Port_InSnapArena(const void *p) { /* (the copies of the list's pieces: used again three lists later) */
    return (const uint8_t *)p >= (const uint8_t *)sSnapDataA && (const uint8_t *)p < (const uint8_t *)sSnapDataA + sizeof(sSnapDataA);
}
int Port_InSnap(const void *p) { /* (or in the back end's queue: a copy that is gone soon after) */
    return ((const uint8_t *)p >= (const uint8_t *)sSnapDataA && (const uint8_t *)p < (const uint8_t *)sSnapDataA + sizeof(sSnapDataA)) || Port_InGr(p);
}
/* Large data a list refers to is read in place, later, by the render thread. What the game sends every frame from
   the same place (textures, models) stays where it is; what it sends ONCE may be gone a frame later (a stage's
   textures while a fight is loaded: the game only waits for the list of the frame before). A list that refers to
   something not seen in the last frames is therefore finished before the game's next frame goes on. */
static struct { uint32_t addr, qwc, seen; } sRefSeen[8192];
static uint32_t sRefList;  /* counts the lists handed over */
static int sFresh;         /* the list made last refers to something new */
unsigned gVitaFresh;       /* for the log: such lists */
unsigned gVitaRef[5][2]; /* referred pieces by size (up to 32, 128, 512, 2048 quadwords, more): how many, quadwords */

/* (upcheck) GS packets of the list's own level read in place (not copied): their bytes at the handover,
   compared when the list reader comes to them (whether the game wrote the next frame's into them first) */
#define SNAP_CHK 1024
static struct { const uint32_t *tag; const uint32_t *w; uint32_t qwc, h; } sSnapChk[3][SNAP_CHK];
static uint32_t sSnapChkN[3];
unsigned gVitaDirChk[2]; /* for the log: such packets read, of them changed */
static uint32_t snap_chk_hash(const uint32_t *w, uint32_t qwc) {
    uint32_t h = 2166136261u, i;
    for (i = 0; i < qwc * 4; i++) {
        h = (h ^ w[i]) * 16777619u;
    }
    return h;
}
static void snap_chk_add(int arena, const uint32_t *tag, const uint32_t *w, uint32_t qwc) {
    if (sSnapChkN[arena] < SNAP_CHK) {
        uint32_t n = sSnapChkN[arena]++;
        sSnapChk[arena][n].tag = tag;
        sSnapChk[arena][n].w = w;
        sSnapChk[arena][n].qwc = qwc;
        sSnapChk[arena][n].h = snap_chk_hash(w, qwc);
    }
}
static void snap_chk_read(int arena, const uint32_t *tag, const uint32_t *data) {
    uint32_t i;
    for (i = 0; i < sSnapChkN[arena]; i++) {
        if (sSnapChk[arena][i].tag == tag && sSnapChk[arena][i].w == data) {
            gVitaDirChk[0]++;
            gVitaDirChk[1] += snap_chk_hash(data, sSnapChk[arena][i].qwc) != sSnapChk[arena][i].h;
            sSnapChk[arena][i].w = NULL; /* (once) */
            return;
        }
    }
}
/* The images (palettes, small textures) copied at the handover are, most of them, the same every frame:
   the stage's and the fighters' palettes, 220 a list, 16 MB in 2 s (earlier versions: the copying and its traffic
   cost the fight pictures). What the drawing thread must not read in place is pixels the game writes anew from frame
   to frame (an animated palette: the water's light). So each image's bytes are summed at the handover and compared
   with the sum the same place had the last time: the same, read in place (the game writes those bytes again, the
   same); changed, copied. bt3.txt snapsame=0: every image copied, as earlier versions. */
int gVitaSnapSame = 1;
unsigned gVitaSnapSame2[2]; /* for the log: images read in place as unchanged, images copied */
static struct { uint32_t addr, qwc, h; } sImgSeen[4096];
static uint32_t img_sum(const uint32_t *w, uint32_t words) {
    uint32_t a = 0x9E3779B1u, b = 0x85EBCA6Bu, i;
    for (i = 0; i + 4 <= words; i += 4) {
        a = (a ^ w[i]) * 0x01000193u + w[i + 1];
        b = (b ^ w[i + 2]) * 0x01000193u + w[i + 3];
    }
    return a ^ (b * 0x27D4EB2Fu);
}
static int snap_img_changed(uint32_t addr, uint32_t qwc) {
    uint32_t h = img_sum((const uint32_t *)(uintptr_t)addr, qwc * 4), k = ((addr >> 4) * 0x9E3779B1u) >> 20, t;
    for (t = 0; t < 8; t++, k = (k + 1) & 4095) {
        if (sImgSeen[k].addr == addr || sImgSeen[k].addr == 0) {
            break;
        }
    }
    if (t == 8) { /* (full around here: in place of the first) */
        k = ((addr >> 4) * 0x9E3779B1u) >> 20;
    }
    if (sImgSeen[k].addr == addr && sImgSeen[k].qwc == qwc && sImgSeen[k].h == h) {
        return 0;
    }
    sImgSeen[k].addr = addr;
    sImgSeen[k].qwc = qwc;
    sImgSeen[k].h = h;
    return 1;
}
static void snap_make(uint32_t tadr, int arena) {
    uint32_t stack[2], sp = 0, sSnapCount = 0, sSnapUsed = 0;
    uint32_t *sSnapData = sSnapDataA[arena];
    int guard;

    memset(sSnapH[arena], 0, sizeof(sSnapH[arena]));
    sSnapChkN[arena] = 0;

    sRefList++;
    sFresh = 0;

    const uint32_t *prevTag = NULL;
    uint32_t prevId = 7, prevQwc = 0;
    for (guard = 0; guard < 100000; guard++) {
        const uint32_t *tag = (const uint32_t *)(uintptr_t)tadr;
        uint32_t qwc = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, addr = tag[1], spAt = sp;
        int end = 0, image = 0;

        if (id == 3 && prevId == 1 && prevQwc != 0 && prevTag != NULL) { /* a REF behind a CNT that ends with a GIF IMAGE tag of its size: pixels */
            const uint32_t *g = prevTag + 4 + (prevQwc - 1) * 4;
            image = ((g[1] >> 26) & 3) == 2 && (g[0] & 0x7FFF) == qwc;
        }
        if (!image && sp == 0 && (id == 0 || id == 3 || id == 4) && qwc >= 2 && qwc <= SNAP_IMG_QWC) { /* (the list's own level only: a model's
                                                                                                    chain sends its GS packets the same every frame, 16 MB copied in 2 s) */
            /* a REF whose data opens with VIF DIRECT: GS packets the game builds in its own memory (a palette
               with its GIF tags, the effects' CLUT blocks: those at 2a0b / 2a0f were written anew every frame in
               two buffers each). Copied only when they carry an image (a GIF IMAGE
               tag): the others were most of a test's 16 MB per 2 s (the effects' primitives, 1 KB a piece, 220 a
               list) and the fight lost pictures to the copying; whether those change in place is checked (upcheck) */
            const uint32_t *w = (const uint32_t *)(uintptr_t)addr;
            int k, gifAt = -1;
            uint32_t dq = 0;
            if (((tag[3] >> 24) & 0x7E) == 0x50) {
                gifAt = 0; /* (the tag's own VIF code is DIRECT: the data is GIF packets from its first word) */
                dq = tag[3] & 0xFFFF;
            } else if (((tag[2] >> 24) & 0x7E) == 0x50) {
                gifAt = 0;
                dq = tag[2] & 0xFFFF;
            }
            for (k = 0; k < 4 && gifAt < 0; k++) {
                uint32_t c = (w[k] >> 24) & 0x7F;
                if (c == 0x50 || c == 0x51) { /* DIRECT, DIRECTHL: its data from the next quadword */
                    gifAt = (k + 4) & ~3;
                    dq = w[k] & 0xFFFF;
                    break;
                }
                if (c != 0x00 && c != 0x10 && c != 0x11 && c != 0x13) { /* (anything but NOP and the FLUSHes before it: not GS packets) */
                    break;
                }
            }
            if (gifAt >= 0) {
                uint32_t q = (uint32_t)gifAt / 4, endq = qwc;
                int tags;
                if (dq != 0 && q + dq < endq) {
                    endq = q + dq;
                }
                for (tags = 0; tags < 64 && q < endq; tags++) { /* the GIF tags: any with FLG IMAGE and pixels */
                    const uint32_t *g = w + q * 4;
                    uint32_t nloop = g[0] & 0x7FFF, flg = (g[1] >> 26) & 3, nreg = (g[1] >> 28) & 15;
                    if (nreg == 0) {
                        nreg = 16;
                    }
                    if (flg >= 2) {
                        if (nloop != 0) {
                            image = 1;
                            break;
                        }
                        q += 1;
                    } else if (flg == 1) {
                        q += 1 + (nloop * nreg + 1) / 2;
                    } else {
                        q += 1 + nloop * nreg;
                    }
                }
                if (!image && gVitaUpCheck) {
                    snap_chk_add(arena, tag, w, qwc);
                }
            }
        }
        prevTag = tag;
        prevId = id;
        prevQwc = qwc;
        switch (id) {
        case 0: end = 1; break;
        case 1: tadr += 16 + qwc * 16; break;
        case 2: tadr = addr; break;
        case 3: case 4: tadr += 16; break;
        case 5: if (sp < 2) { stack[sp++] = tadr + 16 + qwc * 16; } tadr = addr; break;
        case 6: if (sp > 0) { tadr = stack[--sp]; } else { end = 1; } break;
        default: end = 1; break;
        }
        if ((id == 0 || id == 3 || id == 4) && qwc > 512) { /* (the large pieces: textures, models; small ones come and go every frame) */
            uint32_t h = (addr ^ addr >> 13 ^ qwc * 0x9E3779B1u) * 0x85EBCA6Bu >> 19, tries;
            for (tries = 0; tries < 4 && !(sRefSeen[(h + tries) & 8191].addr == addr && sRefSeen[(h + tries) & 8191].qwc == qwc); tries++) {
            }
            if (tries == 4) { /* not there: in place of the one seen longest ago */
                uint32_t old = 0, t2;
                for (t2 = 1; t2 < 4; t2++) {
                    if (sRefList - sRefSeen[(h + t2) & 8191].seen > sRefList - sRefSeen[(h + old) & 8191].seen) { old = t2; }
                }
                tries = old;
                sRefSeen[(h + tries) & 8191].addr = addr;
                sRefSeen[(h + tries) & 8191].qwc = qwc;
                sFresh = 1;
            } else if (sRefList - sRefSeen[(h + tries) & 8191].seen > 8) {
                sFresh = 1;
            }
            sRefSeen[(h + tries) & 8191].seen = sRefList;
        }
        if ((id == 0 || id == 3 || id == 4) && qwc != 0) {
            int k = qwc <= 32 ? 0 : qwc <= 128 ? 1 : qwc <= 512 ? 2 : qwc <= 2048 ? 3 : 4;
            gVitaRef[k][0]++;
            gVitaRef[k][1] += qwc;
        }
        if (image && qwc <= SNAP_IMG_QWC && !(qwc <= SNAP_QWC && spAt == 0) && (id == 0 || id == 3 || id == 4) && gVitaSnapSame && !snap_img_changed(addr, qwc)) {
            image = 0; /* pixels the same as when this place was last handed over: read in place */
            gVitaSnapSame2[0]++;
        } else if (image) {
            gVitaSnapSame2[1]++;
        }
        if ((id == 0 || id == 3 || id == 4) && qwc != 0 && ((qwc <= SNAP_QWC && spAt == 0) || (image && qwc <= SNAP_IMG_QWC)) && sSnapCount < SNAP_MAX &&
            sSnapUsed + qwc * 4 <= SNAP_WORDS) {
            uint32_t hh = snap_h(tag), t2;
            memcpy(&sSnapData[sSnapUsed], (const void *)(uintptr_t)addr, qwc * 16);
            sSnapA[arena][sSnapCount].tag = tag;
            sSnapA[arena][sSnapCount].off = sSnapUsed;
            for (t2 = 0; t2 < 64 && sSnapH[arena][(hh + t2) & (SNAP_HASH - 1)] != 0; t2++) {
                if (sSnapA[arena][sSnapH[arena][(hh + t2) & (SNAP_HASH - 1)] - 1].tag == tag) {
                    break; /* (a tag seen again, in a call made twice: its first copy serves, the data is the same) */
                }
            }
            if (t2 < 64 && sSnapH[arena][(hh + t2) & (SNAP_HASH - 1)] == 0) {
                sSnapH[arena][(hh + t2) & (SNAP_HASH - 1)] = (uint16_t)(sSnapCount + 1);
            }
            sSnapCount++;
            sSnapUsed += qwc * 4;
        }
        if (end) {
            break;
        }
    }
    sSnapCountA[arena] = sSnapCount;
    gVitaSnap[0] += sSnapCount;
    gVitaSnap[1] += sSnapUsed * 4;
}

static void run_chain_(uint32_t tadr, int tte);
static void run_chain(uint32_t tadr, int tte) {
    extern void Vita_ListDone(unsigned us);
    extern volatile unsigned char gProf;
    unsigned long long t = sceKernelGetProcessTimeWide();

    sSnapNext = 0;
    gProf = 1;
    {
        extern void Vita_PerfListBegin(void), Vita_PerfListEnd(void);
        if (!gGsSplit) {
            Vita_PerfListBegin();
        }
        run_chain_(tadr, tte);
        if (!gGsSplit) {
            Vita_PerfListEnd();
        }
    }
    gProf = 0;
    if (!gGsSplit) {
        Vita_ListDone((unsigned)(sceKernelGetProcessTimeWide() - t));
    }
}
#define run_chain run_chain_
#endif
/* The back end's end of a frame. */
static void frame_tail(void) {
    memset(sPix, 0, sizeof(sPix));
    memset(sStat, 0, sizeof(sStat));
    if (sGpu) {
        GsGpu_FrameEnd();
    }
    screenshot();
    if (sMissingTex != 0 && getenv("BT3_GS_VERBOSE") != NULL) {
        fprintf(stderr, "gs: frame %u: %u texel reads from textures that were never uploaded\n", sFrame, sMissingTex);
    }
    sMissingTex = 0;
}

/* Executes one frame's list: the DMA chain at `tadr` on VIF1, then the end-of-frame work of the back end. */
static void run_chain(uint32_t tadr, int tte) {
    uint32_t stack[2], sp = 0;
    int guard;
#ifdef PORT_VITA
    static int ccOn = -1;
    if (ccOn < 0) {
        extern int gVglSafe;
        ccOn = !(gVglSafe & 65536) && getenv("BT3_NO_CC") == NULL; /* (bt3.txt safe=65536: every call read the long way) */
    }
#endif

    for (guard = 0; guard < 100000; guard++) {
        const uint32_t *tag = (const uint32_t *)(uintptr_t)tadr;
        uint32_t qwc = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, addr = tag[1]; /* a host pointer: all 32 bits */
        const uint32_t *data;
        int end = 0;

        switch (id) {
        case 0: data = (const uint32_t *)(uintptr_t)addr; end = 1; break;                 /* REFE */
        case 1: data = tag + 4; tadr += 16 + qwc * 16; break;                             /* CNT */
        case 2: data = tag + 4; tadr = addr; break;                                       /* NEXT */
        case 3: case 4: data = (const uint32_t *)(uintptr_t)addr; tadr += 16; break;      /* REF, REFS */
        case 5: data = tag + 4; if (sp < 2) { stack[sp++] = tadr + 16 + qwc * 16; } tadr = addr; break; /* CALL */
        case 6: data = tag + 4; if (sp > 0) { tadr = stack[--sp]; } else { end = 1; } break;          /* RET */
        default: data = tag + 4; end = 1; break;                                          /* END */
        }
#ifdef PORT_VITA
        sChainSp = sp;
        if (id == 6 && sCcCap >= 0 && sp == sCcDepth) { /* (the recorded call returns: after its RET's own data) */
            if (tte) { vif(tag + 2, 2); }
            vif(tag + 4, qwc * 4);
            cc_end(&sCC[sCcCap]);
            continue;
        }
        if (sSnapCountA[sSnapCur] != 0 && (id == 0 || id == 3 || id == 4)) { /* (by the tag, not in order) */
            uint32_t hh = snap_h(tag), t2;
            for (t2 = 0; t2 < 64; t2++) {
                uint32_t ix = sSnapH[sSnapCur][(hh + t2) & (SNAP_HASH - 1)];
                if (ix == 0) {
                    break;
                }
                if (sSnapA[sSnapCur][ix - 1].tag == tag) {
                    data = &sSnapDataA[sSnapCur][sSnapA[sSnapCur][ix - 1].off]; /* the copy made when the list was handed over */
                    sSnapNext++;
                    break;
                }
            }
        }
        if (sSnapChkN[sSnapCur] != 0 && (id == 0 || id == 3 || id == 4) && sp == 0) {
            snap_chk_read(sSnapCur, tag, data);
        }
#endif
        if (VITA_RARE(sDumpFrame == (int)sFrame)) {
            fprintf(stderr, "  dma tag at %08x: id %u qwc %u addr %08x | %08x %08x (depth %u)\n", (unsigned)(uintptr_t)tag, id, qwc, addr, tag[2], tag[3], sp);
        }
        sStat[0]++;
        if (tte) {
            vif(tag + 2, 2);
        }
        vif(data, qwc * 4);
#ifdef PORT_VITA
        GZM2(20);
#endif
#ifdef PORT_VITA
        if (id == 5 && ccOn && gGsSplit && !gVitaGsSkipF && sp > 0 && sCcCap < 0) {
            extern int Vita_InBattle(void), Vita_ChipResetDue(void);
            extern int gT116Fe;
            CallCache *e = Vita_InBattle() && !(gT116Fe & 2) ? cc_find(addr) : NULL; /* (no calls replayed) */
            if (Vita_ChipResetDue()) {
                gr_simple(GR_CHIPRESET, 0, 0, 0);
                e = NULL;
            }
            if (e != NULL && e->state == 2 && e->gen == gVitaChipGen && ++e->uses % 64 != 0) {
                GZM2(20);
                cc_replay(e);
                GZM2(24);
                tadr = stack[--sp]; /* (the chain itself is not read) */
            } else if (e != NULL && e->state != 3) {
                if (sRegDefN != 0) {
                    reg_def_flush(); /* (what was put off before the call is not the call's) */
                }
                sCcCap = (int)(e - sCC); /* read the long way, and recorded */
                sCcStart = sGrTail;
                sCcDepth = sp - 1;
                memcpy(sCcK0, GsVu1_Consts(), 512); /* (the constants as the call found them: cc_end) */
            }
        }
#endif
        if (end) {
            break;
        }
    }
#ifdef PORT_VITA
    sCcCap = -1; /* (a call the list left without returning is not kept) */
#endif
    if (getenv("BT3_GS_VERBOSE") != NULL && (sFrame < 4 || sFrame % 60 == 0)) {
        fprintf(stderr, "gs: frame %u: %u DMA tags, %u VIF codes, %u DIRECT qwords, %u GIF tags, %u register writes, %u vertices, "
                        "%u image qwords, %d targets\n", sFrame, sStat[0], sStat[1], sStat[2], sStat[3], sStat[4], sStat[5],
                sStat[6], sTargetCount);
    }
    if (VITA_RARE(sDumpFrame == (int)sFrame)) {
        static const int pt[5][2] = {{360, 330}, {362, 331}, {100, 100}, {400, 250}, {364, 300}};
        int k;
        for (k = 0; k < 5; k++) {
            fprintf(stderr, "  end of frame at (%d,%d): frame %08x, depth memory %08x\n", pt[k][0], pt[k][1],
                    vram_rw(0, 8, 0, (uint32_t)pt[k][0], (uint32_t)pt[k][1], 0, 0), vram_rw(0x1C00, 8, 0x30, (uint32_t)pt[k][0], (uint32_t)pt[k][1], 0, 0));
        }
    }
    if (VITA_RARE(sDumpFrame == (int)sFrame)) {
        fprintf(stderr, "  pixels: %u outside scissor, %u failed alpha test, %u failed depth test, %u written\n", sPix[0], sPix[1], sPix[2], sPix[3]);
    }
    GsVu1_FrameEnd();
#ifdef PORT_VITA
    if (gGsSplit) {
        gr_simple(GR_FRAMEEND, 0, 0, 0);
        return;
    }
#endif
    frame_tail();
}
#ifdef PORT_VITA
#undef run_chain
#endif
