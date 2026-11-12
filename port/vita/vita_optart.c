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
#ifdef BT3_BT4
#include "vita_optart_bt4.h"   /* (the BT4 B14 Rev2 disc's lettering is the mod's own: patches made from it) */
#include "vita_optart_bt4es.h" /* (and those of its Spanish release, in Spanish) */
#define VITA_ART_ES
#else
#include "vita_optart.h"    /* (the USA disc's) */
#include "vita_optart_es.h" /* (the European disc's Spanish menus: the options screen in Spanish, vita_eu.c) */
#define VITA_ART_ES
#endif

extern void Vita_Log(const char *fmt, ...);
extern const char *Vita_Tr(const char *english);

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

/* the text (UTF-8: the Spanish letters, which the Spanish discs' lettering has) into the slot as UTF-16 */
static int write_slot(Slot s, const char *text) {
    const unsigned char *t = (const unsigned char *)text;
    size_t i, n = 0;
    if (s.p == NULL) {
        return 0;
    }
    for (i = 0; t[i] != '\0'; i++) {
        n += (t[i] & 0xC0) != 0x80;
    }
    if ((n + 2) * 2 > s.size) {
        return 0;
    }
    memset(s.p + 2, 0, s.size - 2);
    for (i = 0, n = 0; t[i] != '\0'; n++) {
        unsigned c = t[i++];
        if (c >= 0xC0 && (t[i] & 0xC0) == 0x80) { /* (two bytes: up to U+07FF) */
            c = (c & 0x1F) << 6 | (t[i++] & 0x3F);
        }
        s.p[2 + n * 2] = (uint8_t)c;
        s.p[3 + n * 2] = (uint8_t)(c >> 8);
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

/* The art of one disc's screens: the options screen's changes and caption sets, the versus menu's. BT4 has two, its
   English and its Spanish release (the one whose bytes the screen has is used: Vita_Spanish, vita_lang.c, follows the
   release that was started). */
typedef struct ArtSet {
    const ArtSection *opt;
    unsigned optN;
    const uint32_t (*optRuns)[3];
    const uint8_t *optBytes;
    const uint32_t (*capRuns)[2];
    unsigned capN;
    const uint8_t *cap[7];
    const ArtSection *vs;
    unsigned vsN;
    const uint32_t (*vsRuns)[3];
    const uint8_t *vsBytes;
    const uint32_t (*netRuns)[2];
    unsigned netCount;
    const uint8_t *netT, *netOn;
} ArtSet;
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define ART_SET(S) {(const ArtSection *)kOptArtSections##S, COUNT(kOptArtSections##S), kOptArtRuns##S, kOptArtBytes##S, kOptCapRuns##S, \
                    COUNT(kOptCapRuns##S), {kOptCapT##S, kOptCapV##S, kOptCapL##S, kOptCapX##S, kOptCapY##S, kOptCapG##S, kOptCapH##S}, \
                    (const ArtSection *)kVsArtSections##S, COUNT(kVsArtSections##S), kVsArtRuns##S, kVsArtBytes##S, kVsNetRuns##S, \
                    COUNT(kVsNetRuns##S), kVsNetT##S, kVsNetN##S}
static const ArtSet kArtSets[] = {
    ART_SET(),
#ifdef VITA_ART_ES
    ART_SET(Es),
#endif
};
static const ArtSet *sOpt = &kArtSets[0], *sVs = &kArtSets[0];

/* the sections of `secs` whose bytes to replace are there as the set has them */
static unsigned matching(void *res, const ArtSection *secs, unsigned n, const uint32_t (*runs)[3]);

/* the runs of each section written over, when the bytes they replace are the USA disc's; returns the sections done */
static uint32_t section_hash(void *res, const ArtSection *sec, const uint32_t (*runs)[3]) {
    uint8_t *p0 = pack_at(res, sec->section);
    uint32_t h = 2166136261u, i;
    int k;
    for (k = 0; k < sec->count; k++) {
        const uint8_t *p = p0 + runs[sec->first + k][0];
        for (i = 0; i < runs[sec->first + k][1]; i++) {
            h = (h ^ p[i]) * 16777619u;
        }
    }
    return h;
}

static unsigned matching(void *res, const ArtSection *secs, unsigned n, const uint32_t (*runs)[3]) {
    unsigned s, m = 0;
    for (s = 0; s < n; s++) {
        m += secs[s].count > 0 && section_hash(res, &secs[s], runs) == secs[s].hash;
    }
    return m;
}

/* the runs of each section written over, when the bytes they replace are the disc's; returns the sections done */
static unsigned apply(void *res, const ArtSection *secs, unsigned n, const uint32_t (*runs)[3], const uint8_t *bytes, const char *what) {
    unsigned s, k, done = 0;
    for (s = 0; s < n; s++) {
        uint32_t h;
        uint8_t *sec;
        if (secs[s].count == 0) { /* (a screen this disc needs no change of) */
            continue;
        }
        sec = pack_at(res, secs[s].section);
        h = section_hash(res, &secs[s], runs);
        if (h != secs[s].hash) {
            Vita_Log("%s: section %d is not the disc's (%08x), left alone\n", what, secs[s].section, (unsigned)h);
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
static int sOptUp; /* the options screen is up (its art: sOpt) */

void Vita_OptionArt(void *res) {
    unsigned done, lines = 0, k;
    sCapSec = NULL;
    if (res == NULL || ((uint32_t *)res)[0] < 38) {
        Vita_Log("optart: not the options screen's pack, left alone\n");
        return;
    }
    {   /* which disc's screen it is: the set whose bytes it has */
        unsigned best = 0, m;
        sOpt = &kArtSets[0];
        for (k = 0; k < COUNT(kArtSets); k++) {
            if ((m = matching(res, kArtSets[k].opt, kArtSets[k].optN, kArtSets[k].optRuns)) > best) {
                best = m;
                sOpt = &kArtSets[k];
            }
        }
    }
    sOptUp = 1;
    done = apply(res, sOpt->opt, sOpt->optN, sOpt->optRuns, sOpt->optBytes, "optart");
    {   /* the captions of Touch Buttons in place (as patched): the other set can take turns with them */
        uint8_t *sec = pack_at(res, 14);
        const uint8_t *b = sOpt->cap[0];
        int same = 1;
        for (k = 0; k < sOpt->capN && same; k++) {
            same = memcmp(sec + sOpt->capRuns[k][0], b, sOpt->capRuns[k][1]) == 0;
            b += sOpt->capRuns[k][1];
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
#ifndef BT3_BT4
        lines += put_line(t, 25, 28, sOpt != kArtSets ? "T\303\241ctil, choques, Desbloquear, la\npantalla, BT4 y m\303\241s."
                                                      : "Touch, clashes, Unlock All, the\nscreen, BT4 and more.");
#else
        lines += put_line(t, 25, 28, Vita_Tr("Touch buttons, clashes, the\nscreen, the language and more."));
#endif
#ifndef BT3_BT4 /* (BT4's options keep the console's voice picker: its guide line stays) */
        lines += put_line(t, 19, 20, sOpt != kArtSets ? "El idioma de los men\303\272s, del texto\ny de las voces."
                                                      : "The language of the menus and\nthe text, and of the voices.");
#endif
        lines += put_line(t, 40, 27, "");
        lines += put_line(t, 41, 23, "");
        sGuide[0] = slot_of(t, 27);
        sGuide[1] = slot_of(t, 23);
        sGuideNext = 0;
    }
    Vita_Log("optart: %u of %u pictures and %u of 4 lines changed for the Vita (%s disc)\n", done, sOpt->optN, lines,
             sOpt == &kArtSets[0] ? "the English" : "the Spanish");
}

/* the versus menu as unpacked: its section 8 when the battle types' lettering is the disc's (NULL: not, or not up), its
   message window's text, and the lines 5 to 7 kept while Multiplayer's choice writes over them */
static uint8_t *sVsSec8, *sVsText;
static int sVsNetOn;
static uint8_t sVsLines[3][256];

/* 1: section 8 of the versus menu has the battle types' lettering as the set has it */
static int vs_net_there(uint8_t *sec, const ArtSet *set) {
    const uint8_t *b = set->netT;
    unsigned k;
    for (k = 0; k < set->netCount; k++) {
        if (memcmp(sec + set->netRuns[k][0], b, set->netRuns[k][1]) != 0) {
            return 0;
        }
        b += set->netRuns[k][1];
    }
    return 1;
}

void Vita_VersusArt(void *res) {
    unsigned done, k;
    uint8_t *sec;
    sVsSec8 = sVsText = NULL;
    sVsNetOn = 0;
    if (res == NULL || ((uint32_t *)res)[0] < 27) {
        Vita_Log("vsart: not the versus menu's pack, left alone\n");
        return;
    }
    sec = pack_at(res, 8);
    sVs = &kArtSets[0];
    for (k = 0; k < COUNT(kArtSets); k++) { /* (which disc's: the set whose battle types' lettering it has) */
        if (vs_net_there(sec, &kArtSets[k])) {
            sVs = &kArtSets[k];
            break;
        }
    }
    done = apply(res, sVs->vs, sVs->vsN, sVs->vsRuns, sVs->vsBytes, "vsart");
    if (vs_net_there(sec, sVs)) {
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
    b = on ? sVs->netOn : sVs->netT;
    for (k = 0; k < sVs->netCount; k++) {
        memcpy(sVsSec8 + sVs->netRuns[k][0], b, sVs->netRuns[k][1]);
        b += sVs->netRuns[k][1];
    }
    for (k = 0; k < 3; k++) { /* (lines 5, 6, 7: the table's entries 6, 7, 8) */
        Slot sl = slot_of(sVsText, (int)k + 6);
        if (sl.p == NULL || sl.size > sizeof(sVsLines[k])) {
            continue;
        }
        if (on) {
            memcpy(sVsLines[k], sl.p, sl.size);
            write_slot(sl, Vita_Tr(kVsNetLine[k]));
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
        unsigned char c = (unsigned char)text[i], d = (unsigned char)text[i + 1];
        if ((c == 0xC2 || c == 0xC3) && (d & 0xC0) == 0x80 && i + 1 < max) { /* (a Spanish letter: Latin-1) */
            t[i] = (char)c;
            t[i + 1] = (char)d;
            i++;
            continue;
        }
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
        write_slot(slot_of(sVsText, k + 6), Vita_Tr(kVsNetLine[k]));
    }
}

void Vita_IconArt(void *boot) {
    unsigned done;
    if (boot == NULL || ((uint32_t *)boot)[0] < 9) {
        Vita_Log("iconart: not the boot file, left alone\n");
        return;
    }
    done = apply(boot, (const ArtSection *)kIconArtSections, COUNT(kIconArtSections), kIconArtRuns, kIconArtBytes, "iconart");
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
   second page (set 3: Unlock All, Quick Start, Quick Save, Tech Fixes in rows 0, 1, 4, 5), or the Tech Fixes' two
   (set 4: Z Counter, Bombardment in rows 0, 1), or Vita Settings' third page (set 5: Game, Screen in rows 0, 1, Game's
   BT3 and BT4 in rows 6 and 7), or BT4's second page (set 6: Game, Unlock All, BT3, BT4 the same way). Captions
   longer than the disc's are narrowed to fit the plates (gen_patches.py). Rows 2 and 3 (Normal, Custom: the control
   type's picker) are the same in every set. The menus upload a picture each time they draw it, so the change shows
   at once. */
void Vita_OptionCaptions(int set) {
    const uint8_t *b = sOpt->cap[set >= 0 && set < 7 ? set : 0];
    unsigned k;
    if (sCapSec == NULL || set == sCapSet) {
        return;
    }
    for (k = 0; k < sOpt->capN; k++) {
        memcpy(sCapSec + sOpt->capRuns[k][0], b, sOpt->capRuns[k][1]);
        b += sOpt->capRuns[k][1];
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
    sOptUp = 0;
}

/* (vita_lang.c) the language of the options screen that is up: 0 English, 1 Spanish; -1 none is up */
int Vita_OptionArtLang(void) {
    return sOptUp ? (int)(sOpt - kArtSets) : -1;
}
