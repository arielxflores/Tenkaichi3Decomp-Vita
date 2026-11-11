/*
 * The menus' art changed for the PS Vita, applied to a screen's pack of sections as soon as it is unpacked.
 *
 * The options screen (src/menu/menu_x_c.c Option_Init), its controller page:
 *   - section 16, textures 1 and 3 (the rows' lettering, plain and highlighted): "Vibration" becomes "Vita Settings";
 *     on the sound page, "Change Voice" becomes "Language" (the text's language and the voices: Option_VitaLang);
 *   - section 14, textures 1 and 3 (the second strip of the two-way pickers' captions): the rows the Vita does not
 *     show (1P, 2P, Simple) and the free ones read L2, R2, L3, Rear, Front and R3 (Touch Buttons), or, as the page
 *     changes, Touch, Clashes, 2nd Pad, Mods, Square and Stick, or Espanol, Deutsch, Francais and Italiano for the
 *     Language row, or Cheats, Quick Start, Quick Save, Tech Fixes, Unlock All, Z Counter and Bombardment for Vita
 *     Settings' second page
 *     (Vita_OptionCaptions, Option_VitaSetPlates; English and Japanese are the first strip's own);
 *   - section 21, texture 0: the controller picture of the key assignment page, a PS2 pad, becomes a Vita;
 *   - section 33 (the guide's lines under the screen): lines 24, 39 and 40 (of vibration) say what the new row does,
 *     line 18 (of the voices) what Language does.
 * The versus menu (src/menu/menu_g_c.c DuelMenu_Init):
 *   - section 8, textures 0 and 1 (the plates' lettering, plain and highlighted): "1P VS 2P" becomes "Multiplayer";
 *   - section 8, textures 2 and 3 (the battle types' lettering): while Multiplayer's choice is open, "Two Controllers",
 *     "Ad Hoc: Host" and "Ad Hoc: Join" (Vita_VersusNetPlates), and the message window's lines 5 to 7 (section 21)
 *     say what each does.
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
#ifndef BT3_BT4 /* (BT4's options keep the console's voice picker: its guide line stays) */
        lines += put_line(t, 19, 20, "The language of the menus and\nthe text, and of the voices.");
#endif
        lines += put_line(t, 40, 27, "");
        lines += put_line(t, 41, 23, "");
        sGuide[0] = slot_of(t, 27);
        sGuide[1] = slot_of(t, 23);
        sGuideNext = 0;
    }
    Vita_Log("optart: %u of 3 pictures and %u of 4 lines changed for the Vita\n", done, lines);
}

/* the versus menu as unpacked: its section 8 when the battle types' lettering is the disc's (NULL: not, or not up), its
   message window's text, and the lines 5 to 7 kept while Multiplayer's choice writes over them */
static uint8_t *sVsSec8, *sVsText;
static int sVsNetOn;
static uint8_t sVsLines[3][128];

void Vita_VersusArt(void *res) {
    unsigned done, k;
    const uint8_t *b = kVsNetT;
    uint8_t *sec;
    sVsSec8 = sVsText = NULL;
    sVsNetOn = 0;
    if (res == NULL || ((uint32_t *)res)[0] < 27) {
        Vita_Log("vsart: not the versus menu's pack, left alone\n");
        return;
    }
    done = apply(res, (const ArtSection *)kVsArtSections, sizeof(kVsArtSections) / sizeof(kVsArtSections[0]), kVsArtRuns, kVsArtBytes, "vsart");
    sec = pack_at(res, 8);
    for (k = 0; k < sizeof(kVsNetRuns) / sizeof(kVsNetRuns[0]); k++) {
        if (memcmp(sec + kVsNetRuns[k][0], b, kVsNetRuns[k][1]) != 0) {
            break;
        }
        b += kVsNetRuns[k][1];
    }
    if (k == sizeof(kVsNetRuns) / sizeof(kVsNetRuns[0])) {
        sVsSec8 = sec;
        sVsText = pack_at(res, 21);
    }
    Vita_Log("vsart: %u of 1 picture changed for the Vita; Multiplayer's choice %s\n", done, sVsSec8 != NULL ? "ready" : "not the disc's art: left out");
}

/* (src/menu/menu_g_c.c) Multiplayer's choice can be offered: the screen's art is the disc's */
int Vita_VersusNetReady(void) {
    return sVsSec8 != NULL;
}

/* (src/menu/menu_g_c.c) Multiplayer's choice opens (1: the battle types' plates read Two Controllers, Ad Hoc: Host and Ad
   Hoc: Join, and the message window's lines 5 to 7 say what they do) or closes (0: as the disc has them again). 0 when
   the screen's art is not the disc's: the choice is then not offered. The menus draw from the unpacked pack every
   frame, so the change shows at once. */
static const char *const kVsNetLine[3] = {"Two players on this Vita,\nwith a second controller.",
                                          "Host a match for a Vita nearby\n(ad hoc: no router needed).",
                                          "Join a match that a Vita nearby\nhosts (ad hoc)."};

int Vita_VersusNetPlates(int on) {
    const uint8_t *b;
    unsigned k;
    if (sVsSec8 == NULL) {
        return 0;
    }
    on = on != 0;
    if (on == sVsNetOn) {
        return 1;
    }
    b = on ? kVsNetN : kVsNetT;
    for (k = 0; k < sizeof(kVsNetRuns) / sizeof(kVsNetRuns[0]); k++) {
        memcpy(sVsSec8 + kVsNetRuns[k][0], b, kVsNetRuns[k][1]);
        b += kVsNetRuns[k][1];
    }
    for (k = 0; k < 3; k++) { /* (lines 5, 6, 7: the table's entries 6, 7, 8) */
        Slot sl = slot_of(sVsText, (int)k + 6);
        if (sl.p == NULL || sl.size > sizeof(sVsLines[k])) {
            continue;
        }
        if (on) {
            memcpy(sVsLines[k], sl.p, sl.size);
            write_slot(sl, kVsNetLine[k]);
        } else {
            memcpy(sl.p, sVsLines[k], sl.size);
        }
    }
    sVsNetOn = on;
    return 1;
}

/* (src/menu/menu_g_c.c) Multiplayer's choice waits for the other Vita (port/vita/vita_net.c): what it says goes
   into the message window, written over one of the two lines that are not the plate's under the cursor (`keep`: 5, 6
   or 7), in turn, so that the window takes each as a new line. Cut to what the line's slot holds; letters the game's
   lettering may not have (a Vita's user name) as "?". Returns the line to show, -1: none. */
int Vita_VersusNetStatus(const char *text, int keep) {
    static int turn;
    char t[64];
    Slot sl;
    size_t i, max;
    int line;
    if (sVsText == NULL || !sVsNetOn) {
        return -1;
    }
    turn ^= 1;
    line = 5 + ((keep - 5 + 1 + turn) % 3 + 3) % 3;
    sl = slot_of(sVsText, line + 1);
    if (sl.p == NULL || sl.size < 8) {
        return -1;
    }
    max = sl.size / 2 - 2;
    if (max > sizeof(t) - 1) {
        max = sizeof(t) - 1;
    }
    for (i = 0; i < max && text[i] != '\0'; i++) {
        unsigned char c = (unsigned char)text[i];
        t[i] = c == '\n' || (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    t[i] = '\0';
    write_slot(sl, t);
    return line;
}

/* the choice's own three lines again, after Vita_VersusNetStatus */
void Vita_VersusNetHelp(void) {
    int k;
    if (sVsText == NULL || !sVsNetOn) {
        return;
    }
    for (k = 0; k < 3; k++) {
        write_slot(slot_of(sVsText, k + 6), kVsNetLine[k]);
    }
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
   Rear, Front, R3) or of the page above them (set 1: Touch, Clashes, 2nd Pad, Mods, Square, Stick), or the European
   languages of the Language row (set 2: Espanol, Deutsch, Francais, Italiano in rows 0, 1, 4, 5), or Vita Settings'
   second page (set 3: Cheats, Quick Start, Quick Save, Tech Fixes in rows 0, 1, 4, 5, the cheat's Unlock All in 6),
   or the Tech Fixes' two (set 4: Z Counter, Bombardment in rows 0, 1), or Vita Settings' third page (set 5: Game in
   row 0, its BT3 and BT4 in rows 6 and 7). Rows 2 and 3 (Normal, Custom: the control
   type's picker) are the same in every set. The menus upload a picture each time they draw it, so the change shows
   at once. */
void Vita_OptionCaptions(int set) {
    static const uint8_t *const kSet[6] = {kOptCapT, kOptCapV, kOptCapL, kOptCapX, kOptCapY, kOptCapG};
    const uint8_t *b = kSet[set >= 0 && set < 6 ? set : 0];
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

/* 1: the caption sets can be put in (the options screen is up and its art is the USA disc's) */
int Vita_OptionCaptionsOk(void) {
    return sCapSec != NULL;
}

/* the options screen is closed: its pack is let go */
void Vita_OptionArtEnd(void) {
    sCapSec = NULL;
}
