/*
 * PS Vita: the pace of the pictures and the controller.
 *
 * Vita_GsPresent / Vita_Pace decide which of the game's frames are drawn: all of them, unless the game runs late
 * for its vertical blanks (then up to five in a row are left out, so that the game itself keeps the console's
 * speed). Port_PadRead gives the Vita's buttons and sticks to the game as its first controller, and
 * Vita_GsGetenv answers the PC renderer's environment switches (gs_core.c and gs_vu1.c are compiled with getenv
 * renamed to it).
 */
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int gVitaGsSkip;                 /* 1: do not draw the frame being read */
int gVitaGsSkipR;                /* the same for the list in hand (GPU build: set when the list is handed over) */

int gVitaGsFixedSkip = -1;       /* skip=<n> of bt3.txt, or -1: by the time drawing takes */
unsigned gVitaGsDrawn, gVitaGsLastDrawMs, gVitaGsSkipNow;

/* Called by gs_core.c at the end of every frame with the buffer the game shows. A frame is left undrawn only when the game is behind its vertical blanks.
   Vita_GsPresent (the end of every frame's list) counts; Vita_Pace (every vertical blank the game waits for, from
   vita_main.c, with the time the game was late for it: the last wait before a frame is the one that counts)
   decides about the frame to come. At most five frames in a row are left out. */
static int sSkipRun;

void Vita_GsPresent(uint32_t (*read)(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y), uint32_t bp, uint32_t fbw, uint32_t psm) {
    static unsigned long long last;
    unsigned long long t = sceKernelGetProcessTimeWide();

    (void)read; (void)bp; (void)fbw; (void)psm;
    if (!gVitaGsSkipR) {
        gVitaGsDrawn++;
        gVitaGsLastDrawMs = last != 0 ? (unsigned)((t - last) / 1000) : 0;
        sSkipRun = 0;
    } else {
        sSkipRun++;
    }
    last = t;
}

static int sLateUs, sSkipRunSeen;
/* async: the list is drawn on the render thread while the game goes on (a short wait for it is not "late") */
/* (drawn on another processor core, a picture costs the game nothing: whether one is drawn is decided by whether
   the render thread is free, in gs_core.c, not by the game being late) */
int Vita_SkipWanted(int async) { return async && gVitaGsFixedSkip < 0 ? 0 : gVitaGsSkip; }

void Vita_Pace(int lateUs) {
    sLateUs = lateUs;
    sSkipRunSeen = sSkipRun;
    extern int gVglSafe;
    /* (with the pictures put up in step with the game's clock, vita_vgl.c, a list given a little late is still shown
       on time: only a game more than 10 ms late leaves one out) */
    gVitaGsSkip = gVitaGsFixedSkip >= 0 ? sSkipRun < gVitaGsFixedSkip : lateUs > ((gVglSafe & 134217728) ? 4000 : 10000) && sSkipRun < 5;
    gVitaGsSkipNow = (unsigned)sSkipRun;
}

/* ---- the pad: the Vita's buttons and sticks as the game's first controller (the libpad2 packet of
   port/src/gs/gs_input.c: two bytes of buttons, active low, then right x / y, left x / y). The Vita has no L2 / R2:
   they are SELECT + L / R, or the touch panels (vita_touch.c). ---- */
enum { B_SELECT = 0x0001, B_START = 0x0008, B_UP = 0x0010, B_RIGHT = 0x0020, B_DOWN = 0x0040, B_LEFT = 0x0080, B_L2 = 0x0100, B_R2 = 0x0200,
       B_L1 = 0x0400, B_R1 = 0x0800, B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000, B_CROSS = 0x4000, B_SQUARE = 0x8000 };

int Port_PadRead(int socket, unsigned char *data) {
    static const struct { unsigned vita, ps2; } map[] = {
        {SCE_CTRL_START, B_START}, {SCE_CTRL_UP, B_UP}, {SCE_CTRL_RIGHT, B_RIGHT}, {SCE_CTRL_DOWN, B_DOWN}, {SCE_CTRL_LEFT, B_LEFT},
        {SCE_CTRL_TRIANGLE, B_TRIANGLE}, {SCE_CTRL_CIRCLE, B_CIRCLE}, {SCE_CTRL_CROSS, B_CROSS}, {SCE_CTRL_SQUARE, B_SQUARE}};
    SceCtrlData pad;
    unsigned btn = 0, i;
    extern void Vita_SettingsPoll(void);
    extern int Vita_SettingsOpen(void);

    if (socket != 0) {
        return 0;
    }
    Vita_SettingsPoll();
    if (Vita_SettingsOpen()) {
        memset(data, 0, 18);
        data[0] = data[1] = 0xFF; /* all buttons released (active low) */
        data[2] = data[3] = data[4] = data[5] = 0x80;
        return 18;
    }
    memset(&pad, 0, sizeof(pad));
    pad.lx = pad.ly = pad.rx = pad.ry = 0x80;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (pad.buttons & map[i].vita) {
            btn |= map[i].ps2;
        }
    }
    if (pad.buttons & SCE_CTRL_SELECT) { /* SELECT + L / R = L2 / R2; SELECT alone = SELECT */
        if (pad.buttons & SCE_CTRL_LTRIGGER) { btn |= B_L2; }
        if (pad.buttons & SCE_CTRL_RTRIGGER) { btn |= B_R2; }
        if (!(pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) { btn |= B_SELECT; }
    } else {
        if (pad.buttons & SCE_CTRL_LTRIGGER) { btn |= B_L1; }
        if (pad.buttons & SCE_CTRL_RTRIGGER) { btn |= B_R1; }
    }
    { /* the touch panels: L2 / R2 on the rear pad or the front corners, the menus on the screen (vita_touch.c) */
        extern unsigned Vita_TouchPad(void);
        btn |= Vita_TouchPad();
    }
    memset(data, 0, 18);
    data[0] = (unsigned char)(~btn & 0xFF);
    data[1] = (unsigned char)(~(btn >> 8) & 0xFF);
    data[2] = pad.rx; data[3] = pad.ry; data[4] = pad.lx; data[5] = pad.ly;
    return 18;
}

/* ---- the renderer's switches: gs_core.c and gs_vu1.c ask the environment in their innermost loops (developer
   switches of the PC build). Here every such question is answered at once: the GPU renderer is on
   (BT3_GS=gpu), the PC's developer switches are off. build.py compiles the two files with getenv renamed to this. ---- */
char *Vita_GsGetenv(const char *name) {
    if (name[4] == 'V' && strcmp(name, "BT3_VU_INTERP") == 0) { /* the interpreter unless vita_vgl.c has its shader */
        extern int gVglVuReady;
        return gVglVuReady ? NULL : "1";
    }
    if (name[4] == 'N' && strcmp(name, "BT3_NOSOUND") == 0) {
        extern int gVglSafe;
        return (gVglSafe & 1024) ? "1" : NULL;
    }
    if (name[4] == 'S' && strcmp(name, "BT3_SND_VERBOSE") == 0) {
        /* (off) every sound effect and stream start was printed into err-vgl.txt, unbuffered, while the
           sound's lock was held: a few writes to the memory card per effect, which held up the sound thread and the
           game's thread (Snd_Update up to 211 ms a frame, the picture rate down to 3 a second) */
        return NULL;
    }
    if (name[4] == 'G' && strcmp(name, "BT3_GS_THREAD") == 0) { /* lists on a thread of their own (TRIANGLE: not) */
        extern int gVglSafe;
        return (gVglSafe & 512) ? NULL : "1";
    }
    return strcmp(name, "BT3_GS") == 0 ? "gpu" : NULL;
}

