/*
 * The game's text on the PS Vita: what the menus and the save dialogs say of the PS2 is said of the Vita.
 * "Checking memory card (PS2)... Do not remove memory card (PS2), controller, reset, or switch off the console." and
 * the dialog titles "MEMORY CARD slot 1" are in every screen's dialog table (each screen has its own copy), so they
 * are changed where all text goes: the font's print commands and its measuring (src/battle/col_c.c Font_NewCmd,
 * Font_MeasureWidthEx, Font_CountLinesEx call Vita_TextFix). A string that names no PS2 thing is returned as it is;
 * one that does is rewritten once into a pool and the rewritten copy is used from then on.
 */
#include <stdint.h>
#include <string.h>

extern void Vita_Log(const char *fmt, ...);

typedef struct Rule {
    const char *from, *to;
} Rule;

/* in order: the whole sentences first, then the left-overs */
static const Rule kRules[] = {
    {"Do not remove memory card (PS2),\ncontroller, reset, or switch off the console.", "Do not remove the memory card\nor switch off the system."},
    {"Do not remove memory card (PS2), \ncontroller, reset, or switch off the console.", "Do not remove the memory card\nor switch off the system."},
    {"Do not remove memory card (PS2), \nreset, or switch off the console.", "Do not remove the memory card\nor switch off the system."},
    {"Do not remove memory card (PS2),\nreset, or switch off the console.", "Do not remove the memory card\nor switch off the system."},
    {"Checking memory card (PS2)...", "Checking the memory card..."},
    {"MEMORY CARD slot 1 \nand MEMORY CARD slot 2", "Memory card"},
    {"MEMORY CARD slot 1", "Memory card"},
    {"MEMORY CARD slot 2", "Memory card"},
    {"Formatting memory card (PS2). ", "Formatting the memory card. "},
    {"Format memory card (PS2)?", "Format the memory card?"},
    {"\n(PS2) ", "\n"},
    {" (PS2)", ""},
    {"(PS2) ", ""},
    {"(PS2)", ""},
    {"controller, reset, or switch off the console", "or switch off the system"},
    {"reset, or switch off the console", "or switch off the system"},
    {"the console", "the system"},
    {"PlayStation\xae" "2", "PS Vita"},
    {"PlayStation 2", "PS Vita"},
    /* a two-player battle without a second controller (a PS TV's second DualShock, or VitaControl's): PZS3US1.AFS
       member 4's "No DUALSHOCK(R)2 analog controller connected. \nVerify that a DUALSHOCK(R)2 analog controller
       \nis properly inserted into controller port 2." (and port 1, and both: "No DUALSHOCK(R)2 analog controllers\nare
       connected to either controller port 1 or 2. \nVerify that DUALSHOCK(R)2 analog controllers\nare properly
       inserted into controller ports 1 and 2.") */
    {"is properly inserted into controller port 2", "is connected as the second controller"},
    {"is properly inserted into controller port 1", "is connected as the first controller"},
    {"are connected to either controller port 1 or 2", "are connected"},
    {"are properly inserted into controller ports 1 and 2", "are connected as the first and second ones"},
    {"DUALSHOCK\xae" "2 analog controller", "controller"},
    {"DUALSHOCK\xae" "2 analog Controller", "controller"},
    {"DUALSHOCK 2 analog controller", "controller"},
    {"DUALSHOCK\xae" "2", "controller"},
    {"DUALSHOCK 2", "controller"},
};

static uint16_t ch_at(const void *s, int wide, int i) {
    return wide ? ((const uint16_t *)s)[i] : ((const uint8_t *)s)[i];
}

static int str_len(const void *s, int wide) {
    int n = 0;
    while (n < 4096 && ch_at(s, wide, n) != 0) {
        n++;
    }
    return n;
}

static int match_at(const void *s, int wide, int i, int len, const char *pat) {
    int k;
    for (k = 0; pat[k] != 0; k++) {
        if (i + k >= len || ch_at(s, wide, i + k) != (uint8_t)pat[k]) {
            return 0;
        }
    }
    return 1;
}

/* quick test: does the string name anything of the PS2 at all */
static int needs_fix(const void *s, int wide, int len) {
    int i;
    for (i = 0; i + 2 < len; i++) {
        uint16_t c = ch_at(s, wide, i);
        if ((c == 'P' && (match_at(s, wide, i, len, "PS2") || match_at(s, wide, i, len, "PlayStation"))) ||
            (c == 'M' && match_at(s, wide, i, len, "MEMORY CARD")) || (c == 'c' && match_at(s, wide, i, len, "console")) ||
            (c == 'D' && match_at(s, wide, i, len, "DUALSHOCK"))) {
            return 1;
        }
    }
    return 0;
}

#define POOL_BYTES (96 * 1024)
#define CACHE 384
static uint8_t sPool[POOL_BYTES] __attribute__((aligned(4)));
static unsigned sPoolUsed;
static struct {
    const void *src;
    uint32_t hash;
    void *out;
} sCache[CACHE];
static int sCacheN;

void *Vita_TextFix(void *str, int kind) {
    int wide = kind == 0, len, i, n, c;
    uint32_t h = 2166136261u;
    uint16_t buf[1024];
    void *out;
    if (str == NULL || (kind != 0 && kind != 1)) {
        return str;
    }
    len = str_len(str, wide);
    if (len < 3 || !needs_fix(str, wide, len)) {
        return str;
    }
    for (i = 0; i < len; i++) {
        h = (h ^ ch_at(str, wide, i)) * 16777619u;
    }
    for (c = 0; c < sCacheN; c++) {
        if (sCache[c].src == str && sCache[c].hash == h) {
            return sCache[c].out;
        }
    }
    /* rewrite: at each position the first rule that matches */
    n = 0;
    for (i = 0; i < len && n < 1000;) {
        int r, done = 0;
        for (r = 0; r < (int)(sizeof(kRules) / sizeof(kRules[0])); r++) {
            if (match_at(str, wide, i, len, kRules[r].from)) {
                const char *t = kRules[r].to;
                while (*t != 0 && n < 1000) {
                    buf[n++] = (uint8_t)*t++;
                }
                i += (int)strlen(kRules[r].from);
                done = 1;
                break;
            }
        }
        if (!done) {
            buf[n++] = ch_at(str, wide, i++);
        }
    }
    buf[n++] = 0;
    if (sCacheN >= CACHE || sPoolUsed + (unsigned)n * 2 + 4 > POOL_BYTES) {
        return str; /* (full: as it was) */
    }
    out = sPool + sPoolUsed;
    if (wide) {
        memcpy(out, buf, (size_t)n * 2);
        sPoolUsed += ((unsigned)n * 2 + 3) & ~3u;
    } else {
        for (i = 0; i < n; i++) {
            ((uint8_t *)out)[i] = (uint8_t)buf[i];
        }
        sPoolUsed += ((unsigned)n + 3) & ~3u;
    }
    sCache[sCacheN].src = str;
    sCache[sCacheN].hash = h;
    sCache[sCacheN].out = out;
    sCacheN++;
    return out;
}

/* The skill list of the pause menu (src/battle/btl_seq.c BtlText_DrawList): its command lines are written for the
   PS2's Normal control type, with the buttons as icon names inside <PAD=...> tags (src/battle/col_c_b.c): the
   face buttons as one character each, which an animated variant follows with a second one (+ held, * pressed
   repeatedly), the shoulder buttons as two (full-width L1, L2, R1, R2). Each button is given here as the one that
   does the same action on that controller now: the Vita's Normal type has Ki Charge on L (the PS2's L2) and
   Blaster Wave on L2, the rear touch pad (the PS2's L1); a Custom type has its own. */
extern const int32_t *Save_PortKeys(int32_t pad, int32_t *tmp);

enum { HOLD = 0x2642, MASH = 0xFF0A };
static const uint16_t kBtnName[8][2] = { /* circle, cross, square, triangle, L1, L2, R1, R2 */
    {0x25CB, 0}, {0x00D7, 0}, {0x25A1, 0}, {0x25B3, 0}, {0xFF2C, 0xFF11}, {0xFF2C, 0xFF12}, {0xFF32, 0xFF11}, {0xFF32, 0xFF12}};
static const int32_t kConsoleKeys[8] = {2, 1, 0, 3, 4, 5, 6, 7}; /* the PS2's Normal: the action of each button */

static int btn_at(const uint16_t *s) {
    int b;
    for (b = 0; b < 8; b++) {
        if (s[0] == kBtnName[b][0] && (kBtnName[b][1] == 0 || s[1] == kBtnName[b][1])) {
            return b;
        }
    }
    return -1;
}

uint16_t *Vita_PadLine(uint16_t *line, int32_t pad) {
    static uint16_t sRing[16][128];
    static unsigned sNext;
    int32_t tmp[8];
    const int32_t *keys = Save_PortKeys(pad, tmp);
    int to[8], b, k, n = 0, inTag = 0, changed = 0;
    uint16_t *out = sRing[sNext++ & 15];

    for (b = 0; b < 8; b++) { /* the PS2's button b -> the button that has its action now */
        to[b] = b;
        for (k = 0; k < 8; k++) {
            if (keys[k] == kConsoleKeys[b]) {
                to[b] = k;
            }
        }
        changed |= to[b] != b;
    }
    if (!changed) {
        return line;
    }
    while (*line != 0 && *line != '\n' && *line != '\r' && n < 120) {
        if (!inTag && line[0] == '<' && line[1] == 'P' && line[2] == 'A' && line[3] == 'D') {
            inTag = 1;
        } else if (inTag && *line == '>') {
            inTag = 0;
        } else if (inTag && (b = btn_at(line)) >= 0) {
            int t = to[b], len = kBtnName[b][1] != 0 ? 2 : 1;
            uint16_t suffix = b < 4 && (line[1] == HOLD || line[1] == MASH) ? line[1] : 0;
            line += len + (suffix != 0);
            out[n++] = kBtnName[t][0];
            if (kBtnName[t][1] != 0) {
                out[n++] = kBtnName[t][1];
            }
            if (suffix != 0 && t < 4 && (suffix == MASH || t == 2 || t == 3)) { /* (held: only square and triangle have it) */
                out[n++] = suffix;
            }
            continue;
        }
        out[n++] = *line++;
    }
    out[n] = 0;
    return out;
}
