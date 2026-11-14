/*
 * PS Vita: the names of the stages and songs added from outside the disc (vita_extras.c), in the game's own lettering.
 *
 * Every name the disc shows in the versus menus' stage and music select is a picture: sheets of 512 x 256 with four
 * stage names (rows of 64) or eight song names (rows of 32), which the movie's name clips show a row of. The PC
 * version (z3xox's) puts an added one's name together from the letters of those pictures (vita_namefont.c, its
 * namefont.c) and draws it over the picture. Here it is written into the sheets themselves, so the game draws it
 * like its own: into a row that the disc's entries do not show at that moment, as the sheet's own colour numbers
 * (the nearest of its table, the alpha counted most: the disc's pictures were made with that table). The sheets
 * are the game's copy, unpacked from the menu archive each time the select screen is set up; nothing on the
 * memory card or the disc is changed.
 *   - a stage's name goes into the row of "Random" (the ninth sheet's last row), which comes back as it was before
 *     "Random" itself is shown;
 *   - a song's into the row of the music value 20 (a row of "********": the menus drop the values 20 to 23), in
 *     both the plain sheets and the lit ones (the music list open).
 * A name with a character no name of the disc has (J, Q, X, Y, Z, j, q, x, digits, most punctuation), or the
 * sheets of another edition, gives 0: the menu prints it in the game's plain font (src/menu/menu_c_e.c).
 *
 * The sheets' layout (as vita_namefont.c checks it): 8 bits a pixel with a table of 256 32-bit colours, sent to the
 * GS as a 256 x 128 picture of 32 bits a pixel, so every byte lands where the 8-bit format reads it; the table in
 * the GS's order, entries 8..15 and 16..23 of every 32 changed places.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#else /* (a test on the PC) */
typedef int SceUID;
typedef unsigned SceSize;
#define sceKernelCreateThread(n, e, p, s, a, m, o) (-1)
#define sceKernelStartThread(t, n, a) (-1)
#define sceKernelExitDeleteThread(s) ((void)0)
#define sceKernelGetProcessTimeWide() 0ull
#endif
#include "vita_namefont.h"
#include "plat_songs.h"
#include "plat_stages.h"

extern void Vita_Log(const char *fmt, ...);

#define SHEET_W 512
#define SHEET_H 256
#define UP_W (SHEET_W / 2) /* the upload's picture, 32 bits a pixel */
#define UP_H (SHEET_H / 2)

enum { K_STAGE, K_SONG };
/* the rows written to: a stage's in the ninth stage sheet's fourth row ("Random", stage 0x23), a song's in the third
   song sheet's fifth row (music value 20, which no list shows) */
#define STAGE_SHEET 8
#define STAGE_ROW 3
#define SONG_SHEET 2
#define SONG_ROW 4

static const uint8_t kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t kCol32[8][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}, {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
                                     {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47}, {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
static const uint8_t kCol8First[4][16] = {{0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54}, {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
                                          {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23}, {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31}};

static uint16_t *sInv;   /* GS word of the sheet -> its pixel in the upload's picture (y * 256 + x) */
static uint32_t sEntry[3]; /* the three name-sheet files' first texture entries (the stage select set up last) */
static int sOn;          /* the sheets are usable (and there is something to name) */
static int sHeld[2];     /* the added one whose name is in the row now, -1 none (the disc's own) */
static uint8_t sOrig[SHEET_W * 64]; /* the stage row's own colour numbers, to put back */

/* (the game's 32-bit pointers are the Vita's own) */
#define PTR(a) ((uint8_t *)(uintptr_t)(uint32_t)(a))
static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* The byte of the 8-bit sheet pixel (x, y) in the upload's pixels. */
static uint32_t byte_at(int x, int y) {
    int col = (y & 15) >> 2, sx = (col & 1) ? ((x & 15) ^ 4) : (x & 15);
    uint32_t a = (((uint32_t)((y >> 1) & ~0x1F) * 4 + ((x >> 2) & ~0x1F) + kBlock32[(y >> 4) & 3][(x >> 4) & 7]) << 8) + kCol8First[y & 3][sx] + col * 64;
    return (uint32_t)sInv[a >> 2] * 4 + (a & 3);
}

static int inv_make(void) {
    int x, y;
    if (sInv != NULL) {
        return 1;
    }
    if ((sInv = malloc(UP_W * UP_H * sizeof(uint16_t))) == NULL) {
        return 0;
    }
    for (y = 0; y < UP_H; y++) { /* the upload: 32-bit pixels, a buffer 256 wide (as vita_namefont.c's sheet_decode) */
        for (x = 0; x < UP_W; x++) {
            uint32_t a = (((uint32_t)(y & ~0x1F) * 4 + ((x >> 1) & ~0x1F) + kBlock32[(y >> 3) & 3][(x >> 3) & 7]) << 6) + kCol32[y & 7][x & 7];
            sInv[a] = (uint16_t)(y * UP_W + x);
        }
    }
    return 1;
}

/* A sheet's pixels and colour table (sheet `i` of the file whose first entry is `e`). */
static uint8_t *sheet_pix(uint32_t e, int i) {
    return PTR(rd32(PTR(e) + i * 0x40 + 0x38)) + 0x60;
}
static const uint8_t *sheet_clut(uint32_t e, int i) {
    return PTR(rd32(PTR(e) + i * 0x40 + 0x3C)) + 0x60;
}

/* Whether sheet `i` of file `e` is one of the kind written to (as vita_namefont.c's sheet_decode checks a sheet: 512 x
   256, 8 bits with a table of 32-bit colours, sent as a 256 x 128 picture of 32 bits). Every screen that hands its
   sheets over is checked: nothing is written into pictures of another kind. */
static int sheet_ok(uint32_t e, int i) {
    const uint8_t *p = PTR(e) + i * 0x40, *pix, *clut;
    uint64_t tex0;
    if (e == 0) {
        return 0;
    }
    memcpy(&tex0, p + 0x30, 8);
    pix = PTR(rd32(p + 0x38));
    clut = PTR(rd32(p + 0x3C));
    return pix != NULL && clut != NULL && rd32(p + 0x08) == 0x60 + SHEET_W * SHEET_H + 0x20 && rd32(p + 0x0C) == 0x60 + 0x400 + 0x20 &&
           ((tex0 >> 20) & 0x3F) == 0x13 && ((tex0 >> 14) & 0x3F) == 8 && ((tex0 >> 26) & 0xF) == 9 && ((tex0 >> 30) & 0xF) == 8 &&
           ((tex0 >> 51) & 0xF) == 0 && ((tex0 >> 55) & 1) == 0 && (rd32(p + 0x18) & 0x3F) == 4 && ((rd32(p + 0x18) >> 8) & 0x3F) == 0 &&
           rd32(pix + 0x30) == UP_W && rd32(pix + 0x34) == UP_H && rd32(clut + 0x30) == 16 && rd32(clut + 0x34) == 16;
}

static void row_read(uint32_t e, int sheet, int y0, int h, uint8_t *out) {
    uint8_t *pix = sheet_pix(e, sheet);
    int x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < SHEET_W; x++) {
            out[y * SHEET_W + x] = pix[byte_at(x, y0 + y)];
        }
    }
}

static void row_write(uint32_t e, int sheet, int y0, int h, const uint8_t *in) {
    uint8_t *pix = sheet_pix(e, sheet);
    int x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < SHEET_W; x++) {
            pix[byte_at(x, y0 + y)] = in[y * SHEET_W + x];
        }
    }
}

/* The name `text` in style `style` as the colour numbers of sheet `sheet` of file `e`, SHEET_W x h (h: the style's
   row). NULL when it cannot be put together (a letter the disc has not). */
static uint8_t *name_indices(int style, const char *text, uint32_t e, int sheet) {
    const uint8_t *clut = sheet_clut(e, sheet);
    static struct { uint32_t c; uint8_t v, set; } cache[4096];
    int w, h, x, y, k, i;
    uint8_t *rgba = NameFont_Compose(style, text, &w, &h), *out;
    int pr[256], pg[256], pb[256], pa[256];

    if (rgba == NULL) {
        return NULL;
    }
    if ((out = malloc((size_t)SHEET_W * h)) == NULL) {
        free(rgba);
        return NULL;
    }
    for (k = 0; k < 256; k++) { /* the table's colours, the colour scaled by the alpha (0x80 = opaque) */
        const uint8_t *c = clut + k * 4;
        pa[k] = c[3] > 0x80 ? 0x80 : c[3];
        pr[k] = c[0] * pa[k];
        pg[k] = c[1] * pa[k];
        pb[k] = c[2] * pa[k];
    }
    memset(cache, 0, sizeof(cache));
    for (y = 0; y < h; y++) {
        for (x = 0; x < SHEET_W; x++) {
            /* a name wider than the row (a long one) is pressed into it: the pixels it covers, averaged */
            int x0 = x * w / SHEET_W, x1 = (x + 1) * w / SHEET_W, n = 0, sr = 0, sg = 0, sb = 0, sa = 0;
            uint32_t c;
            int best = 0, bestD = 0x7FFFFFFF;
            if (x1 <= x0) {
                x1 = x0 + 1;
            }
            for (i = x0; i < x1; i++) {
                const uint8_t *p = rgba + ((size_t)y * w + i) * 4;
                sr += p[0] * p[3];
                sg += p[1] * p[3];
                sb += p[2] * p[3];
                sa += p[3];
                n++;
            }
            {   /* straight colour and the PS2's alpha (0x80 = opaque) */
                int a = sa / n, r = sa ? sr / sa : 0, g = sa ? sg / sa : 0, b = sa ? sb / sa : 0;
                a = (a * 128 + 127) / 255;
                c = (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24;
                k = (int)((c * 0x9E3779B1u) >> 20);
                if (cache[k].set && cache[k].c == c) {
                    out[y * SHEET_W + x] = cache[k].v;
                    continue;
                }
                for (i = 0; i < 256; i++) { /* the nearest of the table: colour scaled by alpha, and the alpha itself */
                    int dr = r * a - pr[i], dg = g * a - pg[i], db = b * a - pb[i], da = a - pa[i];
                    int d = (dr / 16) * (dr / 16) + (dg / 16) * (dg / 16) + (db / 16) * (db / 16) + da * da * 256;
                    if (d < bestD) {
                        bestD = d;
                        best = i;
                    }
                }
            }
            /* the stored byte: the table's own order (entries 8..15 and 16..23 of every 32 changed places) */
            out[y * SHEET_W + x] = (uint8_t)((best & 0xE7) | ((best & 8) << 1) | ((best & 0x10) >> 1));
            cache[k].c = c;
            cache[k].v = out[y * SHEET_W + x];
            cache[k].set = 1;
        }
    }
    free(rgba);
    return out;
}

/* The names put together, once each (the sheets are the same every time the screen is set up). */
static uint8_t *sStageIdx[PORT_STAGE_MAX];
static uint8_t *sSongIdx[PORT_SONG_MAX][2];
static uint8_t sFailed[2][PORT_SONG_MAX];

/* A number for the colour tables of the rows written to: another (the menus of another language, vita_eu.c) and the
   names are put together again for it. */
static uint32_t tables_sum(void) {
    uint32_t h = 2166136261u, k;
    const uint8_t *t[3];
    t[0] = sheet_clut(sEntry[0], STAGE_SHEET);
    t[1] = sheet_clut(sEntry[1], SONG_SHEET);
    t[2] = sEntry[2] != 0 ? sheet_clut(sEntry[2], SONG_SHEET) : t[1];
    for (k = 0; k < 3 * 1024; k++) {
        h = (h ^ t[k >> 10][k & 1023]) * 16777619u;
    }
    return h;
}

/* Cutting the letters takes a while (on the PC 35 ms, here some 20 times that): the sheets are copied on the game's
   thread as the select screen is set up (vita_namefont.c, NameFont_TakeSheets: the game's own memory, which goes
   when the screen goes) and the letters cut from the copies on a thread of their own while the characters are
   chosen. Until it is done an added name is drawn in the font. */
static volatile int sCut; /* 0 not started, 1 cutting, 2 done */
static int cut_thread(SceSize args, void *argp) {
    unsigned long long t = sceKernelGetProcessTimeWide();
    (void)args;
    (void)argp;
    NameFont_Ready();
    Vita_Log("names: the disc's letters cut in %u ms (%s)\n", (unsigned)((sceKernelGetProcessTimeWide() - t) / 1000),
             NameFont_Has(NF_STAGE, 'a') ? "the lettering is used" : "not usable: the names in the game's font");
    __atomic_store_n(&sCut, 2, __ATOMIC_RELEASE);
    sceKernelExitDeleteThread(0);
    return 0;
}
static void cut_start(int stage, int song, int songLit) {
    SceUID th;
    NameFont_TakeSheets(stage, song, songLit);
    sCut = 1;
    th = sceKernelCreateThread("bt3_names", cut_thread, 190, 0x10000, 0, 0x00060000 /* the second and third cores */, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        cut_thread(0, NULL); /* (now, then) */
    }
}
static int cut_done(void) {
    return __atomic_load_n(&sCut, __ATOMIC_ACQUIRE) == 2 && NameFont_Ready();
}

/* The menu set up (src/menu/menu_c_e.c CharSel_Init, menu_e_b.c TeamSel_Init): its three name-sheet files. */
void Port_NameFontSheets(int stage, int song, int songLit) {
    static int first = 1;
    static uint32_t tables;
    uint32_t now;
    int k;
    sEntry[0] = (uint32_t)stage;
    sEntry[1] = (uint32_t)song;
    sEntry[2] = (uint32_t)songLit;
    sHeld[0] = sHeld[1] = -1;
    sOn = 0;
    PortSongs_Refresh(); /* (songs made from an MP3 since: the screen lists them a moment later) */
    if (gPortExtraStageCount == 0 && gPortSongCount == 0) {
        return; /* (nothing to name: the sheets are not even looked at) */
    }
    if (!sheet_ok(sEntry[0], STAGE_SHEET) || !sheet_ok(sEntry[1], SONG_SHEET) || !inv_make()) {
        return;
    }
    if (!sheet_ok(sEntry[2], SONG_SHEET)) {
        sEntry[2] = 0; /* (the lit song names: without them the plain ones only) */
    }
    if (first) {
        first = 0;
        cut_start(stage, song, songLit);
    }
    if ((now = tables_sum()) != tables) {
        tables = now;
        for (k = 0; k < PORT_STAGE_MAX; k++) {
            free(sStageIdx[k]);
            sStageIdx[k] = NULL;
        }
        for (k = 0; k < PORT_SONG_MAX; k++) {
            free(sSongIdx[k][0]);
            free(sSongIdx[k][1]);
            sSongIdx[k][0] = sSongIdx[k][1] = NULL;
        }
        memset(sFailed, 0, sizeof(sFailed));
    }
    row_read(sEntry[0], STAGE_SHEET, STAGE_ROW * 64, 64, sOrig);
    sOn = 1;
}

int Port_NameRow(int kind, int idx, int *sheet, int *row) {
    if (!sOn || !cut_done()) {
        return 0;
    }
    if (kind == K_STAGE) {
        if (idx < 0 || idx >= gPortExtraStageCount || sFailed[K_STAGE][idx]) {
            return 0;
        }
        if (sHeld[K_STAGE] != idx) {
            if (sStageIdx[idx] == NULL && (sStageIdx[idx] = name_indices(NF_STAGE, gPortStageNames[idx], sEntry[0], STAGE_SHEET)) == NULL) {
                sFailed[K_STAGE][idx] = 1;
                Vita_Log("stages: the name \"%s\" has a letter the disc's names do not: in the game's plain font\n", gPortStageNames[idx]);
                return 0;
            }
            row_write(sEntry[0], STAGE_SHEET, STAGE_ROW * 64, 64, sStageIdx[idx]);
            sHeld[K_STAGE] = idx;
        }
        *sheet = STAGE_SHEET;
        *row = STAGE_ROW;
        return 1;
    }
    if (idx < 0 || idx >= gPortSongCount || sFailed[K_SONG][idx] || sEntry[2] == 0) {
        return 0;
    }
    if (sHeld[K_SONG] != idx) {
        int k;
        for (k = 0; k < 2; k++) {
            if (sSongIdx[idx][k] == NULL &&
                (sSongIdx[idx][k] = name_indices(k ? NF_SONG_LIT : NF_SONG, gPortSongNames[idx], sEntry[1 + k], SONG_SHEET)) == NULL) {
                sFailed[K_SONG][idx] = 1;
                Vita_Log("songs: the name \"%s\" has a letter the disc's names do not: in the game's plain font\n", gPortSongNames[idx]);
                return 0;
            }
        }
        for (k = 0; k < 2; k++) {
            row_write(sEntry[1 + k], SONG_SHEET, SONG_ROW * 32, 32, sSongIdx[idx][k]);
        }
        sHeld[K_SONG] = idx;
    }
    *sheet = SONG_SHEET;
    *row = SONG_ROW;
    return 1;
}

void Port_NameRowDisc(int kind, int id) {
    if (sOn && kind == K_STAGE && id == STAGE_SHEET * 4 + STAGE_ROW && sHeld[K_STAGE] >= 0) {
        row_write(sEntry[0], STAGE_SHEET, STAGE_ROW * 64, 64, sOrig); /* "Random" itself */
        sHeld[K_STAGE] = -1;
    }
    /* (the song row is one no list shows) */
}
