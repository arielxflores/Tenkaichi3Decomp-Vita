/*
 * PS Vita: the game started again inside the running program.
 *
 * A multiplayer match needs both Vitas' games to be the same from their first moment (vita_net.c). Ending the
 * program and starting it anew gives that, and loses the ad hoc connection with it: the system's dialog, the wait
 * for the other Vita, all of it once more. Here the game alone starts again: the program, its renderer, its sound
 * and the connection stay.
 *
 * The game is one thread that never returns (main > Game_Main, src/main.c) and keeps everything it knows in
 *   - its own variables: the data tables (the PS2 program's .data and .bss, port/data) and the few variables of
 *     its source files, two runs of the program's memory each (build.py puts a marker object before and after
 *     them: gWarmG0d .. gWarmG1d and so on);
 *   - its heap, one block (vita_mem.c);
 *   - the vector unit's registers (gRefVu0, gPortVu0R) and two generators of random numbers;
 *   - what the port keeps for it by handle: sound streams and voices, open files and archives, the memory card's
 *     files, its blank handler.
 * So: a copy of the variables is taken just before the game's first instruction. To start again, the game's thread
 * leaves the game where it stands (a jump back to before main: the game holds no lock of the port's between two of its
 * calls), what the port keeps for it is given back, its variables are copied back, its heap and its stack are made
 * all zero as a new program finds them, and main is called again. The renderer is only waited for: to it the game's
 * second start is more of the same stream of drawing (new textures, new lists), as every change of screen is.
 *
 * What a start of the PROGRAM would also do and this does not: read bt3.txt and the settings again, look for the
 * disc's files, mods, added stages and songs. Those are as they were.
 */
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "port/vu0_a.h"

extern void Vita_Log(const char *fmt, ...);
extern void Vita_Fatal(const char *fmt, ...);

/* the markers (port/vita/build.py compile_all): 16 bytes each, in .data and in .bss */
extern char gWarmG0d[], gWarmG1d[], gWarmG0b[], gWarmG1b[], gWarmD0d[], gWarmD1d[], gWarmD0b[], gWarmD1b[];

extern unsigned long long gPortRandNext; /* plat_sys.c: the game's C library generator */
extern uint32_t gPortVu0R;               /* mathf_pc.c: the vector unit's R register */
extern int gVitaInMenus, gVitaMenuMode, gVitaInMovie, gVitaMenuReady, gVitaMenuMoves; /* vita_touch.c: set by the game's code */
extern unsigned gPortVBlanks;

int gVitaWarmOff;  /* bt3.txt netwarm=0: the versus menu's Multiplayer starts the program again, as before */
int gVitaWarmTest; /* (development build) bt3.txt warmtest=<seconds>: the one-Vita test of a match begun this way */
unsigned gVitaWarmCount; /* how many times the game was started again */

typedef struct Run {
    uint8_t *at;
    uint32_t size;
} Run;
#define RUNS 8
static Run sRun[RUNS];
static int sRunN;
static uint8_t *sKeep;
static uint32_t sKeepSize;
static struct { int inMenus, menuMode, inMovie, menuReady, menuMoves; } sFlags;
static jmp_buf sJmp;
static volatile int sArmed; /* the game's thread is in the game: the jump can be taken */
static SceUID sThread = -1;

static void run_add(void *from, void *to) {
    if ((uint8_t *)to > (uint8_t *)from && sRunN < RUNS) {
        sRun[sRunN].at = from;
        sRun[sRunN].size = (uint32_t)((uint8_t *)to - (uint8_t *)from);
        sKeepSize += sRun[sRunN].size;
        sRunN++;
    }
}

static void keep_take(void) {
    uint32_t at = 0;
    int k;

    run_add(gWarmG0d + 16, gWarmG1d);
    run_add(gWarmG0b + 16, gWarmG1b);
    run_add(gWarmD0d + 16, gWarmD1d);
    run_add(gWarmD0b + 16, gWarmD1b);
    run_add(&gRefVu0, &gRefVu0 + 1);
    sKeep = malloc(sKeepSize);
    if (sKeep == NULL) {
        Vita_Log("warm: no memory for the copy of the game's variables (%u bytes): the game cannot start again by itself\n", (unsigned)sKeepSize);
        return;
    }
    for (k = 0; k < sRunN; k++) {
        memcpy(sKeep + at, sRun[k].at, sRun[k].size);
        at += sRun[k].size;
    }
    sFlags.inMenus = gVitaInMenus;
    sFlags.menuMode = gVitaMenuMode;
    sFlags.inMovie = gVitaInMovie;
    sFlags.menuReady = gVitaMenuReady;
    sFlags.menuMoves = gVitaMenuMoves;
    Vita_Log("warm: the game's variables kept: %u bytes in %d runs\n", (unsigned)sKeepSize, sRunN);
}

/* The thread's stack below here made zero: what the game's functions find in variables they do not set is then the
   same after every start, and on every Vita. (The lowest page is left as the system made it.) */
static __attribute__((noinline)) void stack_wipe(void) {
    SceKernelThreadInfo ti;
    uintptr_t here = (uintptr_t)__builtin_frame_address(0), lo, hi;

    memset(&ti, 0, sizeof(ti));
    ti.size = sizeof(ti);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) < 0 || ti.stack == NULL) {
        return;
    }
    lo = (uintptr_t)ti.stack;
    hi = lo + (uintptr_t)ti.stackSize;
    if (here <= lo + 0x3000 || here >= hi) {
        return; /* (not where the stack is said to be: left alone) */
    }
    memset((void *)(lo + 0x1000), 0, here - 0x1000 - (lo + 0x1000)); /* (memset's own frame lies above what it clears) */
    if (gVitaWarmCount == 0) {
        Vita_Log("warm: the game's stack: %u KB from %08x, the game begins %u KB under its top\n", (unsigned)(ti.stackSize >> 10), (unsigned)lo,
                 (unsigned)((hi - here) >> 10));
    }
}

/* What the port keeps for the game given back, the game's memory as a new program finds it. */
static void warm_reset(void) {
    extern void Port_GsSync(void);
    extern void Port_AdxWarm(void), Port_SeWarm(void), Vita_FileWarm(void), Port_McWarm(void), Port_StubWarm(void), Port_HeadlessWarm(void);
    extern int Vita_MemWarm(void);
    extern void Vita_NetWarm(unsigned blank);
    unsigned long long t0 = sceKernelGetProcessTimeWide();
    uint32_t at = 0;
    int k, low;

    Port_GsSync();      /* (no list of the game that ended is still being drawn) */
    Port_AdxWarm();     /* the streams (their files' reads with them) */
    Port_SeWarm();
    Vita_FileWarm();
    Port_McWarm();
    Port_StubWarm();
    Port_HeadlessWarm();
    /* (the variables first: the other threads look at the game's state through them, gBtlSeq for one, and must not
       be led into the heap while it is cleared) */
    for (k = 0; k < sRunN; k++) {
        memcpy(sRun[k].at, sKeep + at, sRun[k].size);
        at += sRun[k].size;
    }
    low = Vita_MemWarm();
    gPortRandNext = 1;
    gPortVu0R = 0x3F800000u;
    gVitaInMenus = sFlags.inMenus;
    gVitaMenuMode = sFlags.menuMode;
    gVitaInMovie = sFlags.inMovie;
    gVitaMenuReady = sFlags.menuReady;
    gVitaMenuMoves = sFlags.menuMoves;
    gVitaWarmCount++;
    Vita_NetWarm(gPortVBlanks); /* (a match begins or has ended with this start: vita_net.c) */
    Vita_Log("warm: the game starts again (the %u. time) at blank %u: %d blocks of its let go, %u ms\n", gVitaWarmCount, gPortVBlanks, low,
             (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
}

/* (vita_main.c, the game's thread) runs the game; again each time Vita_WarmRestart is called. Does not return. */
void Vita_WarmGame(void (*game)(void)) {
    sThread = sceKernelGetThreadId();
    keep_take();
    for (;;) {
        if (setjmp(sJmp) == 0) {
            stack_wipe();
            sArmed = sKeep != NULL;
            game();
        }
        sArmed = 0;
        warm_reset();
    }
}

/* 1: the game can be started again by Vita_WarmRestart (the copy of its variables was made; not turned off) */
int Vita_WarmCan(void) {
    return sArmed && !gVitaWarmOff;
}

/* The game starts again, now. From the game's thread, between two of the game's own calls into the port (a menu's
   update, the blank's wait). Does not return when it can be done; returns when it cannot. */
void Vita_WarmRestart(void) {
    if (sArmed && sceKernelGetThreadId() == sThread) {
        sArmed = 0;
        longjmp(sJmp, 1);
    }
    Vita_Log("warm: the game cannot be started again from here\n");
}

/* (plat_stub.c Port_VBlank, every blank) the development build's test: a match's test begun from the running game,
   fifteen seconds in (vita_net.c writes its record as nettest= does: the two must be the same). */
void Vita_WarmPoll(void) {
    if (gVitaWarmTest > 0 && gVitaWarmCount == 0 && gPortVBlanks == 900 && sArmed) {
        extern void Vita_NetTestBegin(int seconds);
        Vita_Log("warm: warmtest=%d: the match's test begins from the running game\n", gVitaWarmTest);
        Vita_NetTestBegin(gVitaWarmTest);
        Vita_WarmRestart();
    }
}
