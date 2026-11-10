/*
 * The European languages (Spanish, German, French, Italian) from the player's own European disc (SLES-54945),
 * after Matteo Santoro's work for the PC and the Vita (github.com/Matt-code-d): the USA program stays, and the
 * members of PZS3US1.AFS that hold the menus' lettering, the battle HUD and the battle subtitles are read from the
 * European archive instead, from the chosen language's copy.
 *
 *   ux0:data/bt3/eu/PZS3EU1.AFS     the European disc's DATA/PZS3EU1.AFS, as it is
 *   ux0:data/bt3/eu/ZS3EUOP.ADX     (optional) the European opening's song
 *   ux0:data/bt3/pzs3eu1/NNNNN.bin  (also read) the members on their own, as port/tools/extract_eu.py of the PC
 *                                   build writes them
 *
 * The European archive holds the menus five times, once per language (English, Spanish, German, French, Italian):
 * USA member 448 + k is European member 451 + k + 31 * language; the battle HUD (USA 5) is members 5..9; the
 * battle subtitles (USA 560 + story) are 50-wide banks at 661 / 711 / 761 / 811 / 861, whose slots 0..44 are USA
 * stories 5.. and whose slots 45..49 hold the next language's stories 0..4. English keeps the USA disc's files.
 *
 * The language: bt3.txt lang=0..4 (English .. Italian), chosen on a screen at the first start that finds the
 * archive; holding SELECT while the game starts shows the screen again.
 */
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vita_eu.h"

extern void Vita_Log(const char *fmt, ...);
extern int Port_Setting(const char *name, int def);
extern void Port_SettingSave(const char *name, int value);
extern void Port_SettingsWrite(void);
extern int psvDebugScreenPrintf(const char *fmt, ...);

#define EU_FIRST 451
#define EU_STRIDE 31
#define HUD_USA 5
#define LANGS 5
#define BTL_FLID0 560   /* the battle subtitles: BtlScript's text file 0x231 + story is member 560 + story */
#define BTL_COUNT 50
#define BTL_SHIFT 5     /* USA stories 0..4 sit in the previous bank's slots 45..49 */
#define BTL_MAIN 45

/* The USA menu archives with lettering or text (454..458: Dragon World Tour's brackets, 464..471: the sagas). */
static const uint16_t kUsaMenu[] = {448, 449, 450, 451, 452, 453, 454, 455, 456, 457, 458, 459,
                                    460, 461, 463, 464, 465, 466, 467, 468, 469, 470, 471, 472};
static const char *const kNames[LANGS] = {"English", "Espanol", "Deutsch", "Francais", "Italiano"};
static const uint16_t kBtlBase[LANGS] = {661, 711, 761, 811, 861};

static char sHome[128];
static SceUID sFd = -1;       /* the European archive, or -1 */
static uint32_t sCount;
static uint32_t *sToc;        /* its (offset, size) per member */
static int sDir;              /* the members as files under pzs3eu1/ instead */
static int sLang;             /* 0 English .. 4 Italian */
static int sUs1 = -1;         /* PZS3US1's partition */

static int file_ok(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    sceIoClose(fd);
    return 1;
}

/* the European member for USA member `flid` in the chosen language, or -1 */
static int eu_member(int flid) {
    int k, story;

    if (sLang <= 0 || sLang >= LANGS) {
        return -1;
    }
    if (flid == HUD_USA) {
        return HUD_USA + sLang;
    }
    for (k = 0; k < (int)(sizeof(kUsaMenu) / sizeof(kUsaMenu[0])); k++) {
        if (kUsaMenu[k] == flid) {
            return EU_FIRST + (flid - kUsaMenu[0]) + sLang * EU_STRIDE;
        }
    }
    if (flid >= BTL_FLID0 && flid < BTL_FLID0 + BTL_COUNT) {
        story = flid - BTL_FLID0;
        if (story < BTL_SHIFT) {
            return kBtlBase[sLang - 1] + BTL_MAIN + story;
        }
        story -= BTL_SHIFT;
        return story < BTL_MAIN ? kBtlBase[sLang] + story : -1;
    }
    return -1;
}

static int open_archive(const char *path) {
    uint32_t head[2];
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);

    if (fd < 0) {
        return 0;
    }
    if (sceIoPread(fd, head, 8, 0) != 8 || memcmp(head, "AFS\0", 4) != 0 || head[1] < 1000 || head[1] > 0x10000) {
        Vita_Log("eu: %s is not the European disc's PZS3EU1.AFS\n", path);
        sceIoClose(fd);
        return 0;
    }
    sToc = malloc((size_t)head[1] * 8);
    if (sToc == NULL || sceIoPread(fd, sToc, head[1] * 8, 8) != (int)(head[1] * 8)) {
        free(sToc);
        sToc = NULL;
        sceIoClose(fd);
        return 0;
    }
    sFd = fd;
    sCount = head[1];
    return 1;
}

void VitaEu_Init(const char *home) {
    char path[200];

    snprintf(sHome, sizeof(sHome), "%s", home);
    snprintf(path, sizeof(path), "%s/eu/PZS3EU1.AFS", home);
    if (!open_archive(path)) {
        snprintf(path, sizeof(path), "%s/pzs3eu1/00006.bin", home);
        sDir = file_ok(path);
    }
    sLang = Port_Setting("lang", -1);
    if (VitaEu_Available()) {
        Vita_Log("eu: the European languages from %s (%u members); language %d\n", sDir ? "pzs3eu1/" : "eu/PZS3EU1.AFS",
                 (unsigned)sCount, sLang);
    }
    if (sLang < 0 || sLang >= LANGS) {
        sLang = 0;
    }
}

int VitaEu_Available(void) {
    return sFd >= 0 || sDir;
}

int VitaEu_Lang(void) {
    return VitaEu_Available() ? sLang : 0;
}

int VitaEu_UsesMenuRemap(void) {
    return VitaEu_Lang() > 0;
}

static unsigned pad_now(void) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    return pad.buttons;
}

/* At the start (vita_main.c, on the text screen): the language, the first time the archive is found or with SELECT
   held. Up / down and CROSS. */
void VitaEu_ChooseAtStart(void) {
    unsigned prev, now;
    int i, sel, first;

    if (!VitaEu_Available()) {
        return;
    }
    first = Port_Setting("lang", -1) < 0;
    if (!first && !(pad_now() & SCE_CTRL_SELECT)) {
        return;
    }
    sel = sLang;
    prev = pad_now();
    for (;;) {
        psvDebugScreenPrintf("\e[H\e[2J\n  Budokai Tenkaichi 3: the game's language\n\n");
        for (i = 0; i < LANGS; i++) {
            psvDebugScreenPrintf("  %s %s\e[39m\n", i == sel ? "\e[33m>" : " ", kNames[i]);
        }
        psvDebugScreenPrintf("\n  Up / down, then CROSS.\n\n  (Hold SELECT while the game starts to choose again.)\n");
        for (;;) {
            sceKernelDelayThread(16000);
            now = pad_now();
            if ((now & ~prev) & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_CROSS)) {
                break;
            }
            prev = now;
        }
        if ((now & ~prev) & SCE_CTRL_CROSS) {
            break;
        }
        sel = (sel + ((now & ~prev) & SCE_CTRL_UP ? LANGS - 1 : 1)) % LANGS;
        prev = now;
    }
    sLang = sel;
    Port_SettingSave("lang", sLang);
    Port_SettingsWrite();
    Vita_Log("eu: language %d (%s) chosen\n", sLang, kNames[sLang]);
    psvDebugScreenPrintf("\e[H\e[2J");
    while (pad_now() & SCE_CTRL_CROSS) { /* (not a press in the game) */
        sceKernelDelayThread(16000);
    }
}

void VitaEu_NotePartition(int ptid, const char *name) {
    if (strcmp(name, "pzs3us1") == 0) {
        sUs1 = ptid & 7;
    }
}

/* Member `flid` of partition `ptid` in the chosen language: 1 and its descriptor (not to be closed: *own 0) and
   place in it, or a file of pzs3eu1/ opened (*own 1); 0 when the USA disc's is used. */
int VitaEu_Member(int ptid, int flid, SceUID *fd, uint32_t *base, uint32_t *size, int *own) {
    int m;

    if (!VitaEu_Available() || (ptid & 7) != sUs1 || (m = eu_member(flid)) < 0) {
        return 0;
    }
    if (sFd >= 0) {
        if ((uint32_t)m >= sCount || sToc[m * 2 + 1] == 0) {
            return 0;
        }
        *fd = sFd;
        *base = sToc[m * 2];
        *size = sToc[m * 2 + 1];
        *own = 0;
        return 1;
    } else {
        char path[200];
        SceUID f;
        SceOff n;
        snprintf(path, sizeof(path), "%s/pzs3eu1/%05d.bin", sHome, m);
        if ((f = sceIoOpen(path, SCE_O_RDONLY, 0)) < 0) {
            return 0;
        }
        n = sceIoLseek(f, 0, SCE_SEEK_END);
        if (n <= 0) {
            sceIoClose(f);
            return 0;
        }
        *fd = f;
        *base = 0;
        *size = (uint32_t)n;
        *own = 1;
        return 1;
    }
}

SceUID VitaEu_Fd(void) {
    return sFd;
}

/* The archive's member sizes as the European ones: the game sizes some buffers from the table (the files are a
   little larger in some languages). */
void VitaEu_PatchToc(int ptid, uint32_t *table, uint32_t count) {
    uint32_t flid, base, size;
    SceUID fd;
    int own, n = 0;

    for (flid = 0; flid < count && flid < BTL_FLID0 + BTL_COUNT; flid++) {
        if (VitaEu_Member(ptid, (int)flid, &fd, &base, &size, &own)) {
            if (own) {
                sceIoClose(fd);
            }
            table[flid * 2 + 1] = size;
            n++;
        }
    }
    if (n != 0) {
        Vita_Log("eu: %d members of the archive in %s\n", n, kNames[sLang]);
    }
}

/* The European opening's song instead of the USA one (ZS3USOP.ADX), when it is there and a European language is
   in use. */
int VitaEu_OpeningSong(char *out, int size) {
    if (VitaEu_Lang() <= 0) {
        return 0;
    }
    snprintf(out, (size_t)size, "%s/eu/ZS3EUOP.ADX", sHome);
    return file_ok(out);
}
