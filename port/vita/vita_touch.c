/*
 * The PS Vita's touch panels for the game.
 *
 * L2 / R2 / L3 / R3: the Vita has no second pair of shoulder buttons and no stick buttons. They are the rear touch
 * pad's four quarters, as PS4 Remote Play has them (and so where the HORI Remote Play Assist Attachment's triggers
 * and back buttons press it): upper left L2, upper right R2, lower left L3, lower right R3, as seen from the front.
 * The controller page of the options moves each of them, on its own, to a corner of the front screen (the bottom
 * ones L2 / R2, the top ones L3 / R3) or takes it off the panels. (SELECT + L / R give L2 / R2 in any case.)
 *
 * The menus on the front screen. Almost every menu of the game is a Flash movie (src/sys/gfxm_c.c), and its items
 * are sprites with the labels "fl_on_start" (the cursor is on it) and "fl_off_start" (it is not): 451 uses in the
 * menus. Each frame the movies' drawing reports here where those sprites are on the screen (Vita_FlashClipBegin /
 * Vita_FlashQuad / Vita_FlashClipEnd), so that
 *   - a tap on the item the cursor is on presses CROSS (the game's "confirm");
 *   - a tap on another item moves the cursor there: the d-pad is pressed towards it, one step at a time, until the
 *     movie puts that item at "fl_on_start" (the game's own sound and voice play, as with the buttons);
 *   - a swipe presses the d-pad in its direction, once per 56 pixels moved (long lists, the character grid);
 *   - a two-finger tap presses TRIANGLE (back);
 *   - on a screen without such items (a message, "press a button"), a tap presses CROSS; on the title screen START.
 * In a fight the screen does nothing unless a menu with such items is up (the pause and end-of-fight menus).
 * Nothing of the game is changed: the touches become buttons in the controller packet (vita_gfx.c Port_PadRead).
 */
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void Vita_Log(const char *fmt, ...);
extern int gVitaWide;

/* PS2 button bits of the controller packet (vita_gfx.c) */
enum { B_SELECT = 0x0001, B_L3 = 0x0002, B_R3 = 0x0004, B_START = 0x0008, B_UP = 0x0010, B_RIGHT = 0x0020, B_DOWN = 0x0040,
       B_LEFT = 0x0080, B_L2 = 0x0100, B_R2 = 0x0200, B_TRIANGLE = 0x1000, B_CROSS = 0x4000 };

int gVitaTouchLR = -1; /* bt3.txt touchlr=rear / front / off (0 / 1 / 2) for all four; -1: the player's setting (the options' controller page) */
int gVitaTouchLog;     /* bt3.txt touchlog=1: what the touches did, into the log */
int gVitaInMenus;      /* src/menu/menu_a_b.c: 1 while Progress_Main runs (the menus), 0 in a fight */
int gVitaMenuMode;     /* the menus' screen (gProgress->mode): 1 the title */
int gVitaInMovie;      /* src/sys/movie.c: a film is playing (a tap skips it: START) */
/* The player's setting: the controller page's "Touch Buttons" (src/menu/menu_x_c.c), where each of L2, R2, L3 and
   R3 is touched. Kept in a file of its own (a save holds nothing for it), one line per button:
       L2=rear
       R2=front
       L3=off
   A button not in the file is on the rear pad. (The first versions wrote one word, "rear" or "front", for L2 and R2:
   read as that place for all four.) */
#define TOUCH_FILE "ux0:data/bt3/touch.txt"
static const char *const kSpotName[3] = {"rear", "front", "off"};
static const char *const kBtnName[4] = {"L2", "R2", "L3", "R3"};
static const unsigned kBtnBit[4] = {B_L2, B_R2, B_L3, B_R3};
static int sSpot[4], sSpotRead;

static int spot_of(const char *s) {
    int k;
    for (k = 0; k < 3; k++) {
        if (strncmp(s, kSpotName[k], strlen(kSpotName[k])) == 0) {
            return k;
        }
    }
    return -1;
}

static void spots_read(void) {
    FILE *fp = fopen(TOUCH_FILE, "r");
    char line[32];
    int b, v;
    sSpotRead = 1;
    memset(sSpot, 0, sizeof(sSpot));
    if (fp == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        if ((v = spot_of(line)) >= 0) { /* (the old file: one place for all) */
            for (b = 0; b < 4; b++) {
                sSpot[b] = v;
            }
        }
        for (b = 0; b < 4; b++) {
            if (strncmp(line, kBtnName[b], 2) == 0 && line[2] == '=' && (v = spot_of(line + 3)) >= 0) {
                sSpot[b] = v;
            }
        }
    }
    fclose(fp);
}

/* where button b (0 L2, 1 R2, 2 L3, 3 R3) is touched: 0 the rear pad, 1 the front screen's corner, 2 nowhere */
int Vita_TouchGetSpot(int b) {
    if (!sSpotRead) {
        spots_read();
    }
    if (b < 0 || b > 3) {
        return 2;
    }
    return gVitaTouchLR >= 0 ? gVitaTouchLR : sSpot[b];
}

void Vita_TouchSetSpot(int b, int spot) {
    FILE *fp;
    int k;
    if (!sSpotRead) {
        spots_read();
    }
    if (b < 0 || b > 3 || spot < 0 || spot > 2 || sSpot[b] == spot) {
        return;
    }
    sSpot[b] = spot;
    fp = fopen(TOUCH_FILE, "w");
    if (fp != NULL) {
        for (k = 0; k < 4; k++) {
            fprintf(fp, "%s=%s\n", kBtnName[k], kSpotName[sSpot[k]]);
        }
        fclose(fp);
    }
    Vita_Log("touch: %s now %s\n", kBtnName[b], spot == 0 ? "on the rear pad" : spot == 1 ? "on a front corner" : "not touched");
}

/* ---- the selectable sprites, as the movies draw them ---- */

/* FlashClip (include/sys/gfxm_c.h): name[0x40] at 0, its timeline (FlashTl) at 0xCC: frames (+0x08), frame (+0x0C),
   state (+0x15) */
#define CLIP_TL 0xCC
#define TL_FRAMES(c) (*(const uint8_t **)((const uint8_t *)(c) + CLIP_TL + 0x08))
#define TL_FRAME(c) (*(const uint32_t *)((const uint8_t *)(c) + CLIP_TL + 0x0C))
#define TL_STATE(c) (*((const uint8_t *)(c) + CLIP_TL + 0x15))

typedef struct Hit {
    const void *clip, *flash;
    float x0, y0, x1, y1; /* PS2 frame pixels (512 x 448) */
} Hit;
#define HIT_MAX 160
static Hit sHitA[HIT_MAX], sHitB[HIT_MAX];
static Hit *sCur = sHitA, *sLast = sHitB;
static int sCurN, sLastN;
static int sStack[24], sStackN, sStackSkip; /* entries of sCur being drawn (-1: a clip that is not an item) */

/* Whether a sprite's timeline has the label "fl_on_start": cached by its frame list (the movie's data). */
#define SEL_SLOTS 1024
static struct { const uint8_t *frames; int8_t sel; } sSel[SEL_SLOTS];

static int frames_has(const uint8_t *p, const char *label) {
    int n;
    for (n = 0; n < 4096; n++) {
        const char *name = (const char *)p;
        size_t len = strlen(name);
        uint16_t size;
        if (strcmp(name, label) == 0) {
            return 1;
        }
        p += len + 1;
        memcpy(&size, p, 2);
        if (size == 0) {
            return 0;
        }
        p += 2 + size;
    }
    return 0;
}

static int selectable(const void *clip) {
    const uint8_t *frames = TL_FRAMES(clip);
    unsigned h;
    int i;
    if (frames == NULL) {
        return 0;
    }
    h = ((unsigned)(uintptr_t)frames >> 2) * 2654435761u >> 22; /* 10 bits */
    for (i = 0; i < 8; i++) {
        unsigned s = (h + i) & (SEL_SLOTS - 1);
        if (sSel[s].frames == frames) {
            return sSel[s].sel;
        }
        if (sSel[s].frames == NULL) {
            sSel[s].frames = frames;
            sSel[s].sel = (int8_t)frames_has(frames, "fl_on_start");
            return sSel[s].sel;
        }
    }
    return frames_has(frames, "fl_on_start");
}

/* The label of the frame the sprite shows (the last named frame up to it). */
static const char *clip_label(const void *clip) {
    const uint8_t *p = TL_FRAMES(clip);
    uint32_t upto = TL_FRAME(clip), i;
    uint8_t st = TL_STATE(clip);
    const char *last = "";
    if (p == NULL) {
        return last;
    }
    if ((st & 1) && !(st & 4) && upto > 0) { /* playing: `frame` is the next one */
        upto--;
    }
    for (i = 0; i <= upto && i < 4096; i++) {
        const char *name = (const char *)p;
        size_t len = strlen(name);
        uint16_t size;
        if (len > 0) {
            last = name;
        }
        p += len + 1;
        memcpy(&size, p, 2);
        if (size == 0) {
            break;
        }
        p += 2 + size;
    }
    return last;
}

enum { ST_OTHER, ST_ON, ST_OFF, ST_OK };
static int clip_state(const void *clip) {
    const char *l = clip_label(clip);
    if (strncmp(l, "fl_on", 5) == 0) {
        return ST_ON;
    }
    if (strncmp(l, "fl_off", 6) == 0) {
        return ST_OFF;
    }
    if (strncmp(l, "fl_ok", 5) == 0) {
        return ST_OK;
    }
    return ST_OTHER;
}

/* src/sys/gfxm_c.c FlashSlot_Draw: a visible sprite instance's children are about to be drawn / have been. */
void Vita_FlashClipBegin(const void *clip, const void *flash) {
    if (sStackN >= (int)(sizeof(sStack) / sizeof(sStack[0]))) {
        sStackSkip++;
        return;
    }
    if (sCurN < HIT_MAX && selectable(clip)) {
        Hit *h = &sCur[sCurN];
        h->clip = clip;
        h->flash = flash;
        h->x0 = h->y0 = 1e9f;
        h->x1 = h->y1 = -1e9f;
        sStack[sStackN++] = sCurN++;
    } else {
        sStack[sStackN++] = -1;
    }
}

void Vita_FlashClipEnd(void) {
    if (sStackSkip > 0) {
        sStackSkip--;
        return;
    }
    if (sStackN > 0) {
        int i = sStack[--sStackN];
        if (i >= 0 && i == sCurN - 1 && sCur[i].x1 < sCur[i].x0) {
            sCurN--; /* nothing of it was drawn */
        }
    }
}

/* src/sys/gfxm_c.c FlashDraw_Quad: a quad that is drawn (its four corners, screen pixels). */
void Vita_FlashQuad(const float *pt, unsigned flags, int alpha) {
    int s, k;
    if ((flags & 0x08) || alpha < 0x20) { /* (glows are added on: not the item's shape) */
        return;
    }
    for (s = 0; s < sStackN; s++) {
        int i = sStack[s];
        Hit *h;
        if (i < 0) {
            continue;
        }
        h = &sCur[i];
        for (k = 0; k < 4; k++) {
            float x = pt[k * 2], y = pt[k * 2 + 1];
            x = x < 0.0f ? 0.0f : x > 512.0f ? 512.0f : x; /* (the part on the screen) */
            y = y < 0.0f ? 0.0f : y > 448.0f ? 448.0f : y;
            if (x < h->x0) h->x0 = x;
            if (x > h->x1) h->x1 = x;
            if (y < h->y0) h->y0 = y;
            if (y > h->y1) h->y1 = y;
        }
    }
}

/* src/sys/gfxm_c.c Flash_Destroy: its sprites are gone. */
void Vita_FlashGone(const void *flash) {
    int i, n;
    for (i = n = 0; i < sLastN; i++) {
        if (sLast[i].flash != flash) {
            sLast[n++] = sLast[i];
        }
    }
    sLastN = n;
    for (i = n = 0; i < sCurN; i++) { /* (none is being drawn now) */
        if (sCur[i].flash != flash) {
            sCur[n++] = sCur[i];
        }
    }
    sCurN = n;
    sStackN = sStackSkip = 0;
    memset(sSel, 0, sizeof(sSel)); /* its frame lists may be freed */
}

/* once a frame (the pad's read): what was drawn since the last read is the screen */
static void hits_swap(void) {
    Hit *t = sLast;
    sLast = sCur;
    sLastN = sCurN;
    sCur = t;
    sCurN = 0;
    sStackN = sStackSkip = 0;
}

/* name without its trailing digits (mc_menu_plate_3 -> mc_menu_plate_) */
static size_t family_len(const char *name) {
    size_t n = strlen(name);
    while (n > 0 && name[n - 1] >= '0' && name[n - 1] <= '9') {
        n--;
    }
    return n;
}

static int same_family(const void *a, const void *b) {
    const char *na = (const char *)a, *nb = (const char *)b;
    size_t la = family_len(na), lb = family_len(nb);
    return la == lb && strncmp(na, nb, la) == 0;
}

static Hit *hit_find(const void *clip) {
    int i;
    for (i = 0; i < sLastN; i++) {
        if (sLast[i].clip == clip) {
            return &sLast[i];
        }
    }
    return NULL;
}

/* the item under a point: of the boxes that hold it, the one it is most central in (the boxes of a list's rows
   overlap: they take in their pictures) */
static Hit *hit_at(float x, float y) {
    Hit *best = NULL;
    float score = 1e30f;
    int i;
    for (i = 0; i < sLastN; i++) {
        Hit *h = &sLast[i];
        if (x >= h->x0 && x <= h->x1 && y >= h->y0 && y <= h->y1) {
            float w = h->x1 - h->x0 + 1.0f, hh = h->y1 - h->y0 + 1.0f;
            float dx = x - (h->x0 + h->x1) * 0.5f, dy = y - (h->y0 + h->y1) * 0.5f;
            float sc = (dx < 0 ? -dx : dx) / w + (dy < 0 ? -dy : dy) / hh;
            if (sc < score) {
                score = sc;
                best = h;
            }
        }
    }
    return best;
}

/* the item of the target's kind the cursor is on */
static Hit *hit_on(const Hit *target) {
    Hit *fam = NULL, *any = NULL;
    float d = 1e30f;
    int i;
    for (i = 0; i < sLastN; i++) {
        Hit *h = &sLast[i];
        if (h->flash != target->flash || h == target || clip_state(h->clip) != ST_ON) {
            continue;
        }
        if (same_family(h->clip, target->clip)) {
            if (fam == NULL || (h->x1 - h->x0) * (h->y1 - h->y0) < (fam->x1 - fam->x0) * (fam->y1 - fam->y0)) {
                fam = h;
            }
        } else {
            float dx = (h->x0 + h->x1 - target->x0 - target->x1) * 0.5f, dy = (h->y0 + h->y1 - target->y0 - target->y1) * 0.5f;
            if (dx * dx + dy * dy < d) {
                d = dx * dx + dy * dy;
                any = h;
            }
        }
    }
    return fam != NULL ? fam : any;
}

static void log_items(const char *why) {
    int i;
    Vita_Log("touch: %s: %d items on the screen (menus %d, screen %d)\n", why, sLastN, gVitaInMenus, gVitaMenuMode);
    for (i = 0; i < sLastN && i < 48; i++) {
        const Hit *h = &sLast[i];
        Vita_Log("touch:   %-28.28s %4d,%4d - %4d,%4d  %s\n", (const char *)h->clip, (int)h->x0, (int)h->y0, (int)h->x1, (int)h->y1, clip_label(h->clip));
    }
}

/* ---- buttons pressed for the touches ---- */

#define QMAX 8
static unsigned sQ[QMAX];
static int sQN;
static unsigned sHeld;   /* the synthetic button now down */
static int sPhase;       /* frames left of: pressing (>0 with sHeld), then released */
static int sGap;

static void press(unsigned b) {
    if (sQN < QMAX) {
        sQ[sQN++] = b;
    }
}

/* one frame of the presses: 2 frames down, 3 up */
static int sFast; /* (a fling is scrolling: one frame down, one up, so that the list can go as fast as the finger) */
int gVitaMenuReady = 1; /* src/menu/menu_a.c: the main menu's ring takes a press now (its scroll has played) */
static int ring_wait(void); /* (the main menu: a press waits until the ring takes it) */

static int sFlingGap = 1; /* frames up between a fling's presses: few at its start, more as it slows */
static unsigned press_tick(void) {
    if (sHeld) {
        if (--sPhase <= 0) {
            sHeld = 0;
            sGap = sFast ? sFlingGap : 3;
        }
        return sHeld;
    }
    if (sGap > 0) {
        sGap--;
        return 0;
    }
    if (sQN > 0 && !ring_wait()) {
        sHeld = sQ[0];
        memmove(sQ, sQ + 1, (size_t)(sQN - 1) * sizeof(sQ[0]));
        sQN--;
        sPhase = 2; /* (two frames down for a fling's presses too; an earlier version's one frame was not taken) */
        return sHeld;
    }
    return 0;
}

static int press_idle(void) {
    return !sHeld && sGap == 0 && sQN == 0;
}

/* The main menu's ring of modes scrolls with the finger: it follows a drag one entry per entry's height,
   with the list (the finger up: the entries move up, the cursor goes down), and a quick swipe flings it: it goes
   on at the speed the finger left the screen, slowing down as it goes (faster swipe, faster and longer); a touch
   stops it. */
#define FLING_STEP 46.0f   /* screen pixels per entry (the main menu's rows) */
static struct {
    float v;      /* the speed the finger left with, pixels per frame, + = the list going up (the cursor down) */
    float carry;  /* */
    int left, total, dir; /* the fling's presses still to come, of how many, which button */
} sFling;
static float sHistY[6];
static int sHistN;

static int fling_screen(void) {
    return gVitaInMenus && gVitaMenuMode == 4;
}

static int ring_wait(void) {
    return fling_screen() && !gVitaMenuReady;
}

/* A fling is a number of steps worked out when the finger leaves, as far as the list would glide at that
   speed slowing down by 7% a frame, one press after the other as the ring takes them, closer together at the start
   than at the end. (Build 183 made the steps as the speed ran down; while the ring played its scroll they were lost,
   and a fast fling went five entries.) */
static void fling_start(float v) {
    float dist = (v < 0.0f ? -v : v) * 1.1f / (1.0f - 0.93f);
    int n = (int)(dist / FLING_STEP + 0.5f);
    if (n > 22) {
        n = 22; /* (twice round the ring at most) */
    }
    sFling.v = v;
    sFling.left = sFling.total = n;
    sFling.dir = v > 0.0f ? B_DOWN : B_UP;
}

static void fling_stop(void) {
    sFling.v = 0.0f;
    sFling.left = 0;
}

int gVitaMenuMoves; /* src/menu/menu_a.c: the main menu's cursor moves (for the log) */
static void fling_tick(void) {
    static int movesAt = -1;
    if (sFling.left <= 0) {
        if (!sHeld && sQN == 0) {
            if (sFast && gVitaTouchLog && movesAt >= 0) {
                Vita_Log("touch: fling done: the menu moved %d times since it began\n", gVitaMenuMoves - movesAt);
            }
            sFast = 0;
            movesAt = -1;
        }
        return;
    }
    if (movesAt < 0) {
        movesAt = gVitaMenuMoves;
    }
    if (sQN == 0 && !sHeld && sGap == 0) {
        int done = sFling.total - sFling.left;
        press((unsigned)sFling.dir);
        sFling.left--;
        sFast = 1;
        sFlingGap = 1 + done * 7 / (sFling.total > 0 ? sFling.total : 1);
    }
}

/* moving the cursor to a tapped item */
static struct {
    const void *clip, *flash;
    const void *lastOn;
    int steps, stuck, lastAxis, settle;
    int budget; /* list mode: presses left (-1: not a numbered list) */
} sSeek;

/* the number at the end of an item's name (mc_menu_plate_4 -> 4), or -1 */
static int name_index(const void *clip) {
    const char *n = (const char *)clip;
    size_t len = strlen(n), k = len;
    int v = 0, m = 1;
    while (k > 0 && n[k - 1] >= '0' && n[k - 1] <= '9') {
        k--;
    }
    if (k == len || len - k > 4) {
        return -1;
    }
    for (; k < len; k++) {
        v = v * 10 + (n[k] - '0');
    }
    (void)m;
    return v;
}

/* A numbered list (the target's kind): along which axis its numbers grow on the screen, and which way. Measured on
   the items the cursor is not on (the lit one's box takes in its glow and pictures). 1 vertical, 0 horizontal, -1
   cannot tell. */
static int list_axis(const Hit *t, int *sign) {
    float si = 0, sx = 0, sy = 0, sii = 0, six = 0, siy = 0, n = 0, cx, cy;
    int i;
    for (i = 0; i < sLastN; i++) {
        const Hit *h = &sLast[i];
        int k;
        if (h->flash != t->flash || !same_family(h->clip, t->clip) || clip_state(h->clip) == ST_ON || (k = name_index(h->clip)) < 0) {
            continue;
        }
        cx = (h->x0 + h->x1) * 0.5f;
        cy = (h->y0 + h->y1) * 0.5f;
        si += k; sx += cx; sy += cy; sii += (float)k * k; six += k * cx; siy += k * cy; n += 1;
    }
    if (n < 2 || sii * n - si * si <= 0) {
        return -1;
    }
    cx = six * n - si * sx; /* (covariances, times n squared) */
    cy = siy * n - si * sy;
    if ((cy < 0 ? -cy : cy) >= (cx < 0 ? -cx : cx)) {
        *sign = cy >= 0 ? 1 : -1;
        return 1;
    }
    *sign = cx >= 0 ? 1 : -1;
    return 0;
}

static void seek_tick(void) {
    Hit *t, *on;
    float dx, dy, ox, oy, w, h;
    int axis, sign, ti, oi;
    if (sSeek.clip == NULL || !press_idle()) {
        return;
    }
    t = hit_find(sSeek.clip);
    if (sSeek.settle > 0 && t != NULL) { /* (the movie answers the press a few frames later: until the cursor moves) */
        Hit *now = hit_on(t);
        if ((now == NULL || now->clip == sSeek.lastOn) && clip_state(t->clip) != ST_ON) {
            sSeek.settle--;
            return;
        }
        sSeek.settle = 0;
    }
    if (t == NULL) {
        if (gVitaTouchLog) Vita_Log("touch: the item went away; stopped after %d steps\n", sSeek.steps);
        sSeek.clip = NULL;
        return;
    }
    if (clip_state(t->clip) == ST_ON) {
        if (gVitaTouchLog) Vita_Log("touch: cursor on %s after %d steps\n", (const char *)t->clip, sSeek.steps);
        sSeek.clip = NULL;
        return;
    }
    on = hit_on(t);
    if (on == NULL) {
        if (gVitaTouchLog) Vita_Log("touch: no item of %s's kind has the cursor; stopped after %d steps\n", (const char *)t->clip, sSeek.steps);
        sSeek.clip = NULL;
        return;
    }
    /* a numbered list: as many presses as the numbers differ, the way the numbers grow. (A list that scrolls under a
       cursor at its end, as the main menu's ring, brings the tapped entry under the cursor the same way.) */
    ti = name_index(t->clip);
    oi = name_index(on->clip);
    if (same_family(t->clip, on->clip) && ti >= 0 && oi >= 0 && ti != oi) {
        int delta = ti - oi, dir;
        axis = list_axis(t, &sign);
        if (axis < 0) { /* two items: from their top left corners (the lit one's box grows right and down) */
            float ddx = t->x0 - on->x0, ddy = t->y0 - on->y0;
            axis = (ddy < 0 ? -ddy : ddy) >= (ddx < 0 ? -ddx : ddx);
            sign = ((axis ? ddy : ddx) >= 0) == (delta > 0) ? 1 : -1;
        }
        if (sSeek.steps == 0) {
            sSeek.budget = delta < 0 ? -delta : delta;
        }
        if (sSeek.budget <= 0) {
            if (gVitaTouchLog) Vita_Log("touch: list: %d presses made for %s\n", sSeek.steps, (const char *)t->clip);
            sSeek.clip = NULL;
            return;
        }
        dir = delta * sign > 0;
        press(axis ? (dir ? B_DOWN : B_UP) : (dir ? B_RIGHT : B_LEFT));
        sSeek.budget--;
        sSeek.lastOn = on->clip;
        sSeek.steps++;
        sSeek.settle = 14;
        return;
    }
    sSeek.stuck = sSeek.steps > 0 && on->clip == sSeek.lastOn ? sSeek.stuck + 1 : 0;
    if (sSeek.stuck >= 2 || sSeek.steps >= 30) {
        if (gVitaTouchLog) Vita_Log("touch: the cursor does not get to %s (%s); stopped after %d steps\n", (const char *)t->clip, sSeek.stuck >= 2 ? "it stays" : "too far", sSeek.steps);
        sSeek.clip = NULL;
        return;
    }
    dx = (t->x0 + t->x1 - on->x0 - on->x1) * 0.5f;
    dy = (t->y0 + t->y1 - on->y0 - on->y1) * 0.5f;
    /* items side by side or one above the other: the axis they share; otherwise the longer way */
    ox = (t->x1 < on->x1 ? t->x1 : on->x1) - (t->x0 > on->x0 ? t->x0 : on->x0);
    oy = (t->y1 < on->y1 ? t->y1 : on->y1) - (t->y0 > on->y0 ? t->y0 : on->y0);
    w = (t->x1 - t->x0) < (on->x1 - on->x0) ? (t->x1 - t->x0) : (on->x1 - on->x0);
    h = (t->y1 - t->y0) < (on->y1 - on->y0) ? (t->y1 - t->y0) : (on->y1 - on->y0);
    if (ox > w * 0.5f && oy <= h * 0.5f) {
        axis = 1;
    } else if (oy > h * 0.5f && ox <= w * 0.5f) {
        axis = 0;
    } else {
        axis = (dy < 0 ? -dy : dy) >= (dx < 0 ? -dx : dx) ? 1 : 0;
    }
    if (sSeek.stuck == 1) {
        axis = !sSeek.lastAxis; /* that way the cursor did not move: the other */
    }
    if ((axis ? dy : dx) == 0.0f) {
        axis = !axis;
    }
    press(axis ? (dy < 0 ? B_UP : B_DOWN) : (dx < 0 ? B_LEFT : B_RIGHT));
    sSeek.lastAxis = axis;
    sSeek.lastOn = on->clip;
    sSeek.steps++;
    sSeek.settle = 14;
}

/* ---- the panels ---- */

static SceTouchPanelInfo sFrontInfo, sBackInfo;
static int sReady;

static void touch_init(void) {
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &sFrontInfo) < 0 || sFrontInfo.maxDispX <= sFrontInfo.minDispX) {
        memset(&sFrontInfo, 0, sizeof(sFrontInfo));
        sFrontInfo.maxDispX = 1919;
        sFrontInfo.maxDispY = 1087;
    }
    if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &sBackInfo) < 0 || sBackInfo.maxAaX <= sBackInfo.minAaX) {
        memset(&sBackInfo, 0, sizeof(sBackInfo));
        sBackInfo.minAaY = 108;
        sBackInfo.maxAaX = 1919;
        sBackInfo.maxAaY = 889;
    }
    Vita_Log("touch: front %d,%d - %d,%d, rear %d,%d - %d,%d; L2 %s, R2 %s, L3 %s, R3 %s%s\n", sFrontInfo.minDispX, sFrontInfo.minDispY,
             sFrontInfo.maxDispX, sFrontInfo.maxDispY, sBackInfo.minAaX, sBackInfo.minAaY, sBackInfo.maxAaX, sBackInfo.maxAaY,
             kSpotName[Vita_TouchGetSpot(0)], kSpotName[Vita_TouchGetSpot(1)], kSpotName[Vita_TouchGetSpot(2)], kSpotName[Vita_TouchGetSpot(3)],
             gVitaTouchLR >= 0 ? " (bt3.txt touchlr)" : "");
    sReady = 1;
}

/* front panel units -> screen pixels (960 x 544) */
static void to_screen(const SceTouchReport *r, float *x, float *y) {
    *x = (float)(r->x - sFrontInfo.minDispX) * 960.0f / (float)(sFrontInfo.maxDispX - sFrontInfo.minDispX + 1);
    *y = (float)(r->y - sFrontInfo.minDispY) * 544.0f / (float)(sFrontInfo.maxDispY - sFrontInfo.minDispY + 1);
}

/* screen pixels -> the PS2 frame's (512 x 448), as vita_vgl.c shows it (the whole screen, or 4:3 in the middle) */
static void to_frame(float sx, float sy, float *x, float *y) {
    float w = gVitaWide ? 960.0f : 725.0f, left = (960.0f - w) * 0.5f;
    *x = (sx - left) * 512.0f / w;
    *y = sy * 448.0f / 544.0f;
}

/* the front corners: the button a contact that begins there holds (0: not a corner) */
static unsigned corner_btn(float sx, float sy) {
    int top = sy < 170.0f, bottom = sy >= 544.0f - 170.0f;
    if (!top && !bottom) {
        return 0;
    }
    if (sx < 190.0f) {
        return top ? B_L3 : B_L2;
    }
    if (sx >= 960.0f - 190.0f) {
        return top ? B_R3 : B_R2;
    }
    return 0;
}

/* the gesture of the fingers that are not on a corner */
static struct {
    int active, id, fingers, swiped;
    float x0, y0, ax, ay, x, y; /* start, anchor of the next swipe step, now (screen pixels) */
    unsigned frames;
} sG;
static int sCornerId[8], sCornerN; /* the contacts that began on a corner (front L2 / R2 / L3 / R3) */
static unsigned sCornerBtn[8];

static void tap(float sx, float sy) {
    float fx, fy;
    Hit *h;
    to_frame(sx, sy, &fx, &fy);
    h = hit_at(fx, fy);
    if (gVitaTouchLog) {
        Vita_Log("touch: tap at %d,%d (frame %d,%d): %s\n", (int)sx, (int)sy, (int)fx, (int)fy, h != NULL ? (const char *)h->clip : "no item");
        log_items("the tap's screen");
    }
    if (h != NULL) {
        int st = clip_state(h->clip);
        sSeek.clip = NULL;
        if (gVitaMenuMode == 1 && strstr((const char *)h->clip, "start") != NULL) {
            press(B_START); /* the title's "Press START button" */
        } else if (st == ST_ON) {
            press(B_CROSS);
        } else if (st == ST_OFF || st == ST_OTHER) {
            memset(&sSeek, 0, sizeof(sSeek));
            sSeek.clip = h->clip;
            sSeek.flash = h->flash;
        }
        return;
    }
    if (gVitaInMovie) {
        press(B_START); /* skips the film, as START does */
        return;
    }
    if (!gVitaInMenus) {
        return; /* a fight: the screen is no button */
    }
    if (gVitaMenuMode == 1) {
        press(B_START); /* the title screen */
    } else if (sLastN == 0) {
        press(B_CROSS); /* a screen without items: a message, a prompt */
    }
}

unsigned Vita_TouchPad(void) {
    SceTouchData front, back;
    unsigned btn = 0, r;
    unsigned rear_btns = 0, front_btns = 0; /* the buttons on the rear pad, and on the front corners */
    int i, nav;
    float sx, sy;

    if (!sReady) {
        touch_init();
    }
    hits_swap();
    memset(&front, 0, sizeof(front));
    memset(&back, 0, sizeof(back));
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &front, 1);
    for (i = 0; i < 4; i++) {
        int spot = Vita_TouchGetSpot(i);
        if (spot == 0) {
            rear_btns |= kBtnBit[i];
        } else if (spot == 1) {
            front_btns |= kBtnBit[i];
        }
    }
    if (rear_btns != 0) { /* (each quarter its button, or nothing: the others do not grow into it) */
        /* A finger resting on the lower half (L3 / R3) is let go after a second: the hands holding the Vita touch
           the rear pad there, and R3 held stops the fighter from moving and dashing (BtlInput_TestAction 1 and 3),
           which players took for a dead stick. The game wants L3 / R3 pressed and released (a transformation: R3
           let go within 13 of its frames) or held with a direction for 12 frames, never held for a second. L2 / R2
           (charging, dashing) are held as long as the finger is there. A touch counts again once it is lifted. */
        static struct { uint8_t id, used; unsigned long long t; } held[SCE_TOUCH_MAX_REPORT];
        unsigned long long now = sceKernelGetProcessTimeWide();
        int k;
        sceTouchPeek(SCE_TOUCH_PORT_BACK, &back, 1);
        for (k = 0; k < SCE_TOUCH_MAX_REPORT; k++) {
            int on = 0;
            for (i = 0; i < (int)back.reportNum && i < SCE_TOUCH_MAX_REPORT; i++) {
                on |= held[k].used && back.report[i].id == held[k].id;
            }
            if (!on) {
                held[k].used = 0; /* (lifted) */
            }
        }
        for (i = 0; i < (int)back.reportNum && i < SCE_TOUCH_MAX_REPORT; i++) {
            int left = back.report[i].x < (sBackInfo.minAaX + sBackInfo.maxAaX) / 2, upper = back.report[i].y < (sBackInfo.minAaY + sBackInfo.maxAaY) / 2;
            unsigned long long since = now;
            for (k = 0; k < SCE_TOUCH_MAX_REPORT; k++) {
                if (held[k].used && held[k].id == back.report[i].id) {
                    since = held[k].t;
                    break;
                }
            }
            if (k == SCE_TOUCH_MAX_REPORT) { /* a new touch: from now */
                for (k = 0; k < SCE_TOUCH_MAX_REPORT && held[k].used; k++) {
                }
                if (k < SCE_TOUCH_MAX_REPORT) {
                    held[k].used = 1;
                    held[k].id = back.report[i].id;
                    held[k].t = now;
                }
            }
            if (!upper && now - since > 1000000ull) {
                continue; /* resting there */
            }
            btn |= (upper ? (left ? B_L2 : B_R2) : (left ? B_L3 : B_R3)) & rear_btns;
        }
    }
    /* the front: corners (when they are L2 / R2 / L3 / R3), then one gesture */
    nav = gVitaInMenus || sLastN > 0 || gVitaInMovie;
    {
        int still[8], n = 0, k, primary = -1, fingers = 0;
        for (k = 0; k < sCornerN; k++) {
            still[k] = 0;
        }
        for (i = 0; i < (int)front.reportNum && i < SCE_TOUCH_MAX_REPORT; i++) {
            const SceTouchReport *t = &front.report[i];
            int known = -1;
            unsigned cb;
            for (k = 0; k < sCornerN; k++) {
                if (sCornerId[k] == t->id) {
                    known = k;
                }
            }
            to_screen(t, &sx, &sy);
            if (known >= 0) {
                still[known] = 1;
                btn |= sCornerBtn[known];
                continue;
            }
            if (front_btns != 0 && !(sG.active && sG.id == t->id) && sCornerN < 8 && ((cb = corner_btn(sx, sy)) & front_btns) != 0) {
                sCornerId[sCornerN] = t->id;
                sCornerBtn[sCornerN] = cb;
                still[sCornerN++] = 1;
                btn |= cb;
                continue;
            }
            fingers++;
            if (sG.active && sG.id == t->id) {
                primary = i;
            } else if (!sG.active && primary < 0) {
                primary = i;
            }
        }
        for (k = 0; k < sCornerN; k++) { /* corner contacts lifted */
            if (!still[k]) {
                sCornerId[k] = sCornerId[sCornerN - 1];
                sCornerBtn[k] = sCornerBtn[sCornerN - 1];
                still[k] = still[sCornerN - 1];
                sCornerN--;
                k--;
            }
        }
        (void)n;
        if (primary >= 0) {
            to_screen(&front.report[primary], &sx, &sy);
            if (!sG.active) {
                memset(&sG, 0, sizeof(sG));
                sG.active = 1;
                sG.id = front.report[primary].id;
                sG.x0 = sG.ax = sx;
                sG.y0 = sG.ay = sy;
            }
            sG.x = sx;
            sG.y = sy;
            sG.frames++;
            if (fingers > sG.fingers) {
                sG.fingers = fingers;
            }
            if (sG.frames == 1 && sFling.left > 0) { /* a touch stops a fling (and is no tap) */
                fling_stop();
                sQN = 0;
                sG.swiped = 1;
            }
            if (sHistN < 6) {
                sHistY[sHistN++] = sy;
            } else {
                memmove(sHistY, sHistY + 1, 5 * sizeof(sHistY[0]));
                sHistY[5] = sy;
            }
            if (nav && sG.fingers < 2 && fling_screen()) { /* the main menu: the list follows the finger */
                float dy = sy - sG.ay;
                if (dy >= FLING_STEP || dy <= -FLING_STEP || (!sG.swiped && (dy >= 24.0f || dy <= -24.0f))) {
                    float step = sG.swiped ? FLING_STEP : 24.0f;
                    press(dy < 0 ? B_DOWN : B_UP);
                    sG.ay += dy < 0 ? -step : step;
                    if (!sG.swiped && gVitaTouchLog) Vita_Log("touch: drag on the main menu from %d,%d\n", (int)sG.x0, (int)sG.y0);
                    sG.swiped = 1;
                    sSeek.clip = NULL;
                }
            } else if (nav && sG.fingers < 2) { /* swipe steps */
                float dx = sx - sG.ax, dy = sy - sG.ay, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                float step = sG.swiped ? 56.0f : 48.0f;
                if (adx >= step || ady >= step) {
                    if (ady >= adx) {
                        press(dy < 0 ? B_UP : B_DOWN);
                        sG.ay += dy < 0 ? -step : step;
                        sG.ax = sx;
                    } else {
                        press(dx < 0 ? B_LEFT : B_RIGHT);
                        sG.ax += dx < 0 ? -step : step;
                        sG.ay = sy;
                    }
                    if (!sG.swiped && gVitaTouchLog) Vita_Log("touch: swipe from %d,%d\n", (int)sG.x0, (int)sG.y0);
                    sG.swiped = 1;
                    sSeek.clip = NULL;
                }
            }
        } else if (sG.active) { /* lifted */
            float mx = sG.x - sG.x0, my = sG.y - sG.y0;
            if (sG.swiped && fling_screen() && sHistN >= 2) { /* the speed the finger left with: a fling */
                float v = (sHistY[sHistN - 1] - sHistY[0]) / (float)(sHistN - 1);
                if (v >= 10.0f || v <= -10.0f) { /* (10, was 5: a drag let go of slowly is no fling) */
                    float fv = -v; /* (finger up = the list up = + ) */
                    if (fv > 90.0f) fv = 90.0f;
                    if (fv < -90.0f) fv = -90.0f;
                    fling_start(fv);
                    if (gVitaTouchLog) Vita_Log("touch: fling at %d px a frame: %d entries\n", (int)fv, sFling.left);
                }
            }
            sHistN = 0;
            if (!sG.swiped && sG.frames < 45 && mx * mx + my * my < 30.0f * 30.0f) {
                if (sG.fingers >= 2) {
                    if (nav) {
                        if (gVitaTouchLog) Vita_Log("touch: two-finger tap: back\n");
                        sSeek.clip = NULL;
                        press(B_TRIANGLE);
                    }
                } else {
                    tap(sG.x0, sG.y0);
                }
            }
            sG.active = 0;
        }
    }
    if (!fling_screen()) {
        fling_stop();
    }
    fling_tick();
    seek_tick();
    r = press_tick();
    if (gVitaTouchLog) {
        static unsigned sLogged;
        unsigned lr = btn & (B_L2 | B_R2 | B_L3 | B_R3);
        if (lr != sLogged) {
            Vita_Log("touch: L2 %s, R2 %s, L3 %s, R3 %s (%s)\n", lr & B_L2 ? "down" : "up", lr & B_R2 ? "down" : "up", lr & B_L3 ? "down" : "up",
                     lr & B_R3 ? "down" : "up", rear_btns ? (front_btns ? "rear pad and front corners" : "rear pad") : front_btns ? "front corners" : "no touch");
            sLogged = lr;
        }
        static unsigned sPrevR;
        if (r != 0 && sPrevR == 0) {
            Vita_Log("touch: pressed %s%s\n", r == B_CROSS ? "CROSS" : r == B_TRIANGLE ? "TRIANGLE" : r == B_START ? "START" : r == B_UP ? "UP" : r == B_DOWN ? "DOWN" : r == B_LEFT ? "LEFT" : "RIGHT", sFast ? " (fling)" : "");
        }
        sPrevR = r;
    }
    return btn | r;
}

/* ---- for the test runs ---- */

int gVitaDump;      /* bt3.txt dump=1: src/sys/file.c File_LoadSyncEx copies each file it loads to dump/<id>.bin */
int gVitaStartMode; /* bt3.txt startmode=<n>: the menus begin on screen n (src/menu/menu_a_b.c) */

void Vita_DumpFile(int id, void *data, int size) {
    char path[96];
    FILE *fp;
    sceIoMkdir("ux0:data/bt3/dump", 0777);
    snprintf(path, sizeof(path), "ux0:data/bt3/dump/%d.bin", id);
    fp = fopen(path, "rb");
    if (fp != NULL) { /* (once) */
        fclose(fp);
        return;
    }
    fp = fopen(path, "wb");
    if (fp != NULL) {
        fwrite(data, 1, (size_t)size, fp);
        fclose(fp);
        Vita_Log("dump: file %d, %d bytes\n", id, size);
    }
}
