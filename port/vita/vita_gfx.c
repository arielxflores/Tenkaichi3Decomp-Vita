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

/* ---- the pads: the Vita's buttons and sticks as the game's first controller, and a second controller (a PS TV's
   second DualShock, or a pad paired to a Vita) as the game's second (the libpad2 packet of port/src/gs/gs_input.c:
   two bytes of buttons, active low, then right x / y, left x / y). The handheld has no L2 / R2 / L3 / R3: they are
   SELECT + L / R, or the touch panels (vita_touch.c); a DualShock has them. ---- */
enum { B_SELECT = 0x0001, B_L3 = 0x0002, B_R3 = 0x0004, B_START = 0x0008, B_UP = 0x0010, B_RIGHT = 0x0020, B_DOWN = 0x0040,
       B_LEFT = 0x0080, B_L2 = 0x0100, B_R2 = 0x0200, B_L1 = 0x0400, B_R1 = 0x0800, B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000,
       B_CROSS = 0x4000, B_SQUARE = 0x8000 };

extern int Port_Setting(const char *name, int def);
extern void Port_SettingSave(const char *name, int value);
extern void Port_SettingsWrite(void);

/* Which of the system's controller ports each player reads (after the PS TV support of limeking614-dev's fork).
   Port 0 is the handheld's own buttons (type PHY), or on a PS TV a virtual pad that merges every DualShock (VIRT);
   ports 1..4 are the paired DualShocks. Player 1: the handheld's buttons, or on a PS TV the first DualShock (not the
   merged pad, which would give the second player's presses to the first too). Player 2: the next DualShock, when
   the setting "pad2" (Options > Controller > Vita Settings, on unless set) allows it. Asked of the system about twice
   a second. */
static int sPort[2] = {0, -1};
static unsigned char sPortType[2] = {SCE_CTRL_TYPE_PHY, 0};
static int sPad2 = -1; /* the setting */

int Vita_Pad2Setting(void) {
    if (sPad2 < 0) {
        sPad2 = Port_Setting("pad2", 1) != 0;
    }
    return sPad2;
}

void Vita_Pad2SettingSet(int on) {
    sPad2 = on != 0;
    Port_SettingSave("pad2", sPad2);
    Port_SettingsWrite();
}

static void ports_update(void) {
    static int countdown;
    SceCtrlPortInfo info;
    int k, n = 0, found[2] = {-1, -1};
    unsigned char type[2] = {0, 0};

    if (countdown-- > 0) {
        return;
    }
    countdown = 30;
    memset(&info, 0, sizeof(info));
    if (sceCtrlGetControllerPortInfo(&info) < 0) {
        info.port[0] = SCE_CTRL_TYPE_PHY;
    }
    for (k = 0; k < 5 && n < 2; k++) {
        if (info.port[k] == SCE_CTRL_TYPE_UNPAIRED || info.port[k] == SCE_CTRL_TYPE_VIRT) {
            continue; /* (the PS TV's merged pad: not a player's) */
        }
        found[n] = k;
        type[n++] = info.port[k];
    }
    if (found[0] < 0) { /* (nothing paired that is a player's: the merged pad, or port 0 as before) */
        found[0] = 0;
        type[0] = info.port[0] == SCE_CTRL_TYPE_VIRT ? SCE_CTRL_TYPE_VIRT : SCE_CTRL_TYPE_PHY;
    }
    if (found[0] != sPort[0] || found[1] != sPort[1]) {
        extern void Vita_Log(const char *fmt, ...);
        Vita_Log("pads: player 1 on port %d (type %d), player 2 %s%d (type %d); ports %d %d %d %d %d\n", found[0], type[0],
                 found[1] < 0 ? "none " : "on port ", found[1], type[1], info.port[0], info.port[1], info.port[2], info.port[3], info.port[4]);
    }
    sPort[0] = found[0];
    sPort[1] = found[1];
    sPortType[0] = type[0];
    sPortType[1] = type[1];
}

/* The second controller is there (the game's socket 1: scePad2GetState, port/src/plat_stub.c). */
int Vita_Pad2Present(void) {
    ports_update();
    return sPort[1] >= 0 && Vita_Pad2Setting();
}

/* ---- Clashes with Square (setting clash_square: on unless set to 0; Options > Controller > Vita Settings).
   z3xox's "Clashes: Square counts as a direction" of the PC build (port/src/gs/gs_input.c there), for the Vita,
   whose sticks are not made to be turned round and round. In a clash (two beams or two rushes meeting: actions
   0x130..0x132 and 0xFA..0xFC, the only ones that raise flag 0xE6, the turning-stick prompt) the game counts the
   frames in which a direction is newly pressed (input 51; BtlAct_ClashStruggleHandler, BtlAct_ClashBlowsHandler).
   With the setting on, while this player's fighter is in a clash, Square is taken off the pad and each press of it
   holds the stick in the next of up, right, down, left for as long as it is held: one press, one newly pressed
   direction, as a tap of the d-pad gives. The stick and the d-pad go on working beside it. A press is worth more
   than one (clash_square_x in quarters: 4, 6, 7, 8 or 9, default 9 = 2.25, as upstream): what it is worth beyond
   one is kept as credit, and each whole point owed puts the stick in the next direction for one frame without a
   press; at most one new direction a frame (the game's own limit), credit capped at two points and dropped with
   the clash. The game sees an ordinary pad. */
static int sClashSquare = -1, sClashSquareX = -1;

int Port_ClashSquare(void) {
    if (sClashSquare < 0) {
        sClashSquare = Port_Setting("clash_square", 1) != 0;
    }
    return sClashSquare;
}

void Port_ClashSquareSet(int on) {
    sClashSquare = on != 0;
    Port_SettingSave("clash_square", sClashSquare);
    Port_SettingsWrite();
}

static int clash_square_x(void) {
    if (sClashSquareX < 0) {
        int x = Port_Setting("clash_square_x", 9);
        sClashSquareX = x == 4 || x == 6 || x == 7 || x == 8 || x == 9 ? x : 9;
    }
    return sClashSquareX;
}

static void clash_square(int socket, unsigned *btn, unsigned char *lx, unsigned char *ly) {
    extern void *BtlChar_FindByObjId(int objId); /* the game: the fighter of a side, NULL outside a fight */
    extern int BtlAct_GetCurrent(void *chr);   /* its action */
    extern int gVitaInMenus;
    static int dir[2], was[2], credit[2];
    int in = 0, down = (*btn & B_SQUARE) != 0, extra = 0;
    void *chr;

    if (!Port_ClashSquare()) {
        return;
    }
    if (!gVitaInMenus && (chr = BtlChar_FindByObjId(socket)) != NULL) {
        int action = BtlAct_GetCurrent(chr);
        in = (action >= 0x130 && action <= 0x132) || action == 0xFA || action == 0xFB || action == 0xFC;
    }
    if (!in) {
        was[socket] = down; /* (a Square held into the clash is not a press in it) */
        credit[socket] = 0;
        return;
    }
    if (down && !was[socket]) {
        dir[socket] = (dir[socket] + 1) & 3;
        credit[socket] += clash_square_x() - 4;
        if (credit[socket] > 8) {
            credit[socket] = 8;
        }
    } else if (credit[socket] >= 4) { /* a whole point owed: the next direction for this frame, without a press */
        dir[socket] = (dir[socket] + 1) & 3;
        credit[socket] -= 4;
        extra = 1;
    }
    was[socket] = down;
    *btn &= ~(unsigned)B_SQUARE;
    if (down || extra) {
        switch (dir[socket]) {
        case 0: *ly = 0x00; break; /* up */
        case 1: *lx = 0xFF; break; /* right */
        case 2: *ly = 0xFF; break; /* down */
        default: *lx = 0x00; break; /* left */
        }
    }
}

int Port_PadRead(int socket, unsigned char *data) {
    static const struct { unsigned vita, ps2; } map[] = {
        {SCE_CTRL_START, B_START}, {SCE_CTRL_UP, B_UP}, {SCE_CTRL_RIGHT, B_RIGHT}, {SCE_CTRL_DOWN, B_DOWN}, {SCE_CTRL_LEFT, B_LEFT},
        {SCE_CTRL_TRIANGLE, B_TRIANGLE}, {SCE_CTRL_CIRCLE, B_CIRCLE}, {SCE_CTRL_CROSS, B_CROSS}, {SCE_CTRL_SQUARE, B_SQUARE}};
    SceCtrlData pad;
    unsigned btn = 0, i;
    int ds;

    if (socket < 0 || socket > 1) {
        return 0;
    }
    ports_update();
    if (socket == 1 && !Vita_Pad2Present()) {
        return 0;
    }
    memset(&pad, 0, sizeof(pad));
    pad.lx = pad.ly = pad.rx = pad.ry = 0x80;
    ds = sPortType[socket] == SCE_CTRL_TYPE_DS3 || sPortType[socket] == SCE_CTRL_TYPE_DS4;
    if (ds) { /* a DualShock: L1 / R1 and L2 / R2 / L3 / R3 of its own */
        sceCtrlPeekBufferPositiveExt2(sPort[socket], &pad, 1);
    } else { /* the handheld's buttons (L / R are its triggers' bits), or the PS TV's merged pad as before */
        sceCtrlPeekBufferPositive(0, &pad, 1);
    }
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (pad.buttons & map[i].vita) {
            btn |= map[i].ps2;
        }
    }
    if (ds) {
        if (pad.buttons & SCE_CTRL_L1) { btn |= B_L1; }
        if (pad.buttons & SCE_CTRL_R1) { btn |= B_R1; }
        if (pad.buttons & SCE_CTRL_L2) { btn |= B_L2; }
        if (pad.buttons & SCE_CTRL_R2) { btn |= B_R2; }
        if (pad.buttons & SCE_CTRL_L3) { btn |= B_L3; }
        if (pad.buttons & SCE_CTRL_R3) { btn |= B_R3; }
        if (pad.buttons & SCE_CTRL_SELECT) { btn |= B_SELECT; }
    } else if (pad.buttons & SCE_CTRL_SELECT) { /* SELECT + L / R = L2 / R2; SELECT alone = SELECT */
        if (pad.buttons & SCE_CTRL_LTRIGGER) { btn |= B_L2; }
        if (pad.buttons & SCE_CTRL_RTRIGGER) { btn |= B_R2; }
        if (!(pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) { btn |= B_SELECT; }
    } else {
        if (pad.buttons & SCE_CTRL_LTRIGGER) { btn |= B_L1; }
        if (pad.buttons & SCE_CTRL_RTRIGGER) { btn |= B_R1; }
    }
    if (socket == 0) { /* the touch panels: L2 / R2 on the rear pad or the front corners, the menus on the screen (vita_touch.c) */
        extern unsigned Vita_TouchPad(void);
        btn |= Vita_TouchPad();
    }
    clash_square(socket, &btn, &pad.lx, &pad.ly);
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

