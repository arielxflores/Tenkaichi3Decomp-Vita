/*
 * The menus' art changed for the PS Vita, applied to a screen's pack of sections as soon as it is unpacked.
 *
 * The options screen (src/menu/menu_x_c.c Option_Init), its controller page:
 *   - section 16, textures 1 and 3 (the rows' lettering, plain and highlighted): "Vibration" becomes "Vita Settings";
 *   - section 14, textures 1 and 3 (the second strip of the two-way pickers' captions): the rows the Vita does not
 *     show (1P, 2P, Simple) and the free ones read L2, R2, L3, Rear, Front and R3 (Touch Buttons), or, as the page
 *     changes, Touch, Clashes, 2nd Pad, Mods, Square and Stick (Vita_OptionCaptions, Option_VitaSetPlates);
 *   - section 21, texture 0: the controller picture of the key assignment page, a PS2 pad, becomes a Vita;
 *   - section 33 (the guide's lines under the screen): lines 24, 39 and 40 (of vibration) say what the new row does.
 * The versus menu (src/menu/menu_g_c.c DuelMenu_Init):
 *   - section 8, textures 0 and 1 (the plates' lettering, plain and highlighted): "1P VS 2P" becomes "Multiplayer".
 * The button icons of the game's text (the boot file's member 8, src/battle/col_c_b.c FontIcon_Init), seen in the
 * skill list, the hints and the tutorials:
 *   - textures 16 and 19, L3 and R3: the PS2's sticks become black plates like L1 and R2, reading L3 and R3;
 *   - textures 26 and 27, L3 and R3 held down: the same plates under the yellow arrow of the held buttons.
 * The pictures are drawn in the screens' own lettering style (port/vita/tools: gen_patches.py made vita_optart.h
 * from the USA disc's files 454, 449 and 5). Each section is changed only when the bytes to replace are those of that
 * disc (FNV-1a of them); otherwise it stays as it is and the log says so.
 */
#include <stdint.h>
#include <string.h>
#include "vita_optart.h"

extern void Vita_Log(const char *fmt, ...);

static uint8_t *pack_at(void *res, int n) {
    uint32_t *w = (uint32_t *)res;
    return (uint8_t *)res + ((w[n] >> 2) << 2);
}

/* The guide's text table: word 0 the count of entries, word n the offset of entry n (line n - 1), each a slot of fixed
   size holding UTF-16 with a byte-order mark. The Wii's lines, which this disc does not use, give their slots. */
typedef struct Slot {
    uint8_t *p;
    uint32_t size;
} Slot;

static Slot slot_of(uint8_t *table, int entry) {
    uint32_t *w = (uint32_t *)table;
    Slot s = {NULL, 0};
    if ((uint32_t)entry < w[0] && w[entry + 1] > w[entry] && table[w[entry]] == 0xFF && table[w[entry] + 1] == 0xFE) {
        s.p = table + w[entry];
        s.size = w[entry + 1] - w[entry];
    }
    return s;
}

static int write_slot(Slot s, const char *text) {
    size_t i, n = strlen(text);
    if (s.p == NULL || (n + 2) * 2 > s.size) {
        return 0;
    }
    memset(s.p + 2, 0, s.size - 2);
    for (i = 0; i < n; i++) {
        s.p[2 + i * 2] = (uint8_t)text[i];
    }
    return 1;
}

/* line entry - 1 reads the slot of entry `into`, with the text */
static int put_line(uint8_t *table, int entry, int into, const char *text) {
    Slot s = slot_of(table, into);
    if ((uint32_t)entry > ((uint32_t *)table)[0] || !write_slot(s, text)) {
        return 0;
    }
    ((uint32_t *)table)[entry] = (uint32_t)(s.p - table);
    return 1;
}

/* lines 39 and 40, written by the controller page as its cursor moves (Vita_OptionGuide) */
static Slot sGuide[2];
static int sGuideNext;

typedef struct ArtSection {
    int section, first, count;
    uint32_t hash;
} ArtSection;

/* the runs of each section written over, when the bytes they replace are the USA disc's; returns the sections done */
static unsigned apply(void *res, const ArtSection *secs, unsigned n, const uint32_t (*runs)[3], const uint8_t *bytes, const char *what) {
    unsigned s, k, done = 0;
    for (s = 0; s < n; s++) {
        uint8_t *sec = pack_at(res, secs[s].section);
        uint32_t h = 2166136261u;
        for (k = 0; k < (unsigned)secs[s].count; k++) {
            const uint8_t *p = sec + runs[secs[s].first + k][0];
            uint32_t i;
            for (i = 0; i < runs[secs[s].first + k][1]; i++) {
                h = (h ^ p[i]) * 16777619u;
            }
        }
        if (h != secs[s].hash) {
            Vita_Log("%s: section %d is not the USA disc's (%08x), left alone\n", what, secs[s].section, (unsigned)h);
            continue;
        }
        for (k = 0; k < (unsigned)secs[s].count; k++) {
            memcpy(sec + runs[secs[s].first + k][0], bytes + runs[secs[s].first + k][2], runs[secs[s].first + k][1]);
        }
        done++;
    }
    return done;
}

/* section 14 of the options screen as unpacked and patched (NULL: not the USA disc's, or not up), the captions in it */
static uint8_t *sCapSec;
static int sCapSet;

void Vita_OptionArt(void *res) {
    unsigned done, lines = 0, k;
    sCapSec = NULL;
    if (res == NULL || ((uint32_t *)res)[0] < 38) {
        Vita_Log("optart: not the options screen's pack, left alone\n");
        return;
    }
    done = apply(res, (const ArtSection *)kOptArtSections, sizeof(kOptArtSections) / sizeof(kOptArtSections[0]), kOptArtRuns, kOptArtBytes, "optart");
    {   /* the captions of Touch Buttons in place (as patched): the other set can take turns with them */
        uint8_t *sec = pack_at(res, 14);
        const uint8_t *b = kOptCapT;
        int same = 1;
        for (k = 0; k < sizeof(kOptCapRuns) / sizeof(kOptCapRuns[0]) && same; k++) {
            same = memcmp(sec + kOptCapRuns[k][0], b, kOptCapRuns[k][1]) == 0;
            b += kOptCapRuns[k][1];
        }
        if (same) {
            sCapSec = sec;
            sCapSet = 0;
        }
    }
    /* the guide's lines (entry = line + 1), into slots of the Wii's lines: 24 the row's, 39 and 40 written as the
       page's cursor moves */
    {
        uint8_t *t = pack_at(res, 33);
        lines += put_line(t, 25, 28, "Touch buttons, clashes, a\nsecond controller and mods.");
        lines += put_line(t, 40, 27, "");
        lines += put_line(t, 41, 23, "");
        sGuide[0] = slot_of(t, 27);
        sGuide[1] = slot_of(t, 23);
        sGuideNext = 0;
    }
    Vita_Log("optart: %u of 3 pictures and %u of 3 lines changed for the Vita\n", done, lines);
}

void Vita_VersusArt(void *res) {
    unsigned done;
    if (res == NULL || ((uint32_t *)res)[0] < 27) {
        Vita_Log("vsart: not the versus menu's pack, left alone\n");
        return;
    }
    done = apply(res, (const ArtSection *)kVsArtSections, sizeof(kVsArtSections) / sizeof(kVsArtSections[0]), kVsArtRuns, kVsArtBytes, "vsart");
    Vita_Log("vsart: %u of 1 picture changed for the Vita\n", done);
}

void Vita_IconArt(void *boot) {
    unsigned done;
    if (boot == NULL || ((uint32_t *)boot)[0] < 9) {
        Vita_Log("iconart: not the boot file, left alone\n");
        return;
    }
    done = apply(boot, (const ArtSection *)kIconArtSections, sizeof(kIconArtSections) / sizeof(kIconArtSections[0]), kIconArtRuns, kIconArtBytes, "iconart");
    Vita_Log("iconart: %u of 1 icon file changed for the Vita (L3 / R3)\n", done);
}

/* The guide line for the controller page's "Touch Buttons" (src/menu/menu_x_c.c): the text into line 39 or 40, in
   turn, so that the message window takes it as a new line; returns the line (24, the row's, when the slots are not
   there). */
int Vita_OptionGuide(const char *text) {
    int k = sGuideNext;
    if (!write_slot(sGuide[k], text)) {
        return 24;
    }
    sGuideNext ^= 1;
    return 39 + k;
}

/* Vita Settings (src/menu/menu_x_c.c): the second caption strip with the captions of Touch Buttons (set 0: L2, R2, L3,
   Rear, Front, R3) or of the page above them (set 1: Touch, Clashes, 2nd Pad, Mods, Square, Stick). The menus upload
   a picture each time they draw it, so the change shows at once. */
void Vita_OptionCaptions(int set) {
    const uint8_t *b = set ? kOptCapV : kOptCapT;
    unsigned k;
    if (sCapSec == NULL || set == sCapSet) {
        return;
    }
    for (k = 0; k < sizeof(kOptCapRuns) / sizeof(kOptCapRuns[0]); k++) {
        memcpy(sCapSec + kOptCapRuns[k][0], b, kOptCapRuns[k][1]);
        b += kOptCapRuns[k][1];
    }
    sCapSet = set;
}

/* the options screen is closed: its pack is let go */
void Vita_OptionArtEnd(void) {
    sCapSec = NULL;
}
