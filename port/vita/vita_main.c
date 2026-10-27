/*
 * PS Vita: start-up, settings, the log, and the ways out.
 *
 * The game's main() runs on a thread of its own with a large stack (the PS2 code keeps big buffers in locals; the
 * PC builds give it 16 MB). main is wrapped by the linker (--wrap=main), as in the 64-bit PC builds, and so is
 * exit: the port ends the program in several places, and an error is shown on a text screen until START.
 *
 * Files, all under ux0:data/bt3/ :
 *   disc/              the files of the player's own disc (SLUS_216.78, BIN/DBZP.BIN, DATA/...), as they are on it
 *   saves/             the memory card (port/src/plat_mc.c), bt3_settings.txt the port's settings
 *   bt3.txt            optional, one name=value per line. The released program reads:
 *                        disc=<folder>     another place for the disc files
 *                        wide=0            the console's 4:3 picture in the middle of the screen
 *                        heap=cdram        the game heap in graphics memory (see vita_mem.c)
 *                        report=1          speed figures every two seconds into the log
 *                      the development build (build.py, BT3_VITA_DEV=1) reads every setting of read_settings
 *                      below: test fights, renderer switches, statistics; see docs/vita/development.md.
 *   log.txt            written by every run (log-vgl.txt by the development build), kept across runs
 */
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "debugScreen.h"

#define HOME "ux0:data/bt3"
#ifdef PORT_VITA_SFX /* the development build has its own log, so that its runs and the release's can be kept side by side */
#define SFX PORT_VITA_SFX
#else
#define SFX ".txt"
#endif
#define GAME_STACK 0x00800000

unsigned int _newlib_heap_size_user = 80 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 0x00100000;

char gVitaDisc[256] = HOME "/disc";
static FILE *sLog;
static int sHeapCdram, sFloatChoice = -1; /* -1: ask */
static unsigned long long sStart, sBattleStart, sLastDraw;
static unsigned sBattleStartVb, sLastVb, sLastState = 0xFFFFFFFFu;
static volatile int sEnded;
static int sMode = -1; /* 0 the menus (play), 1 the demo fight; -1: ask */
int gVitaReport = 1;   /* the numbers every two seconds into the log (the released program: bt3.txt report=1) */
static const int kDemoPick = -1; /* the demo's pairing (0..8), or -1: each run the next one (ux0:data/bt3/demo_next.txt) */
/* the sweep: in the demo fight, after this many seconds of fighting the program starts itself again
   (the next pairing: demo_next.txt), so that one test run sees several pairings; 0: off (bt3.txt: sweep=<s>) */
static int sSweepSec = 32;
volatile int gVitaShotReq; /* the drawing thread takes two pictures for the log (vita_vgl.c) */
extern int gVitaGsFixedSkip;
extern unsigned gVitaGsDrawn, gVitaGsLastDrawMs, gVitaGsSkipNow;

extern unsigned gPortVBlanks;
static unsigned long long now_us(void);
static void watch_log(void);
static void logf_(const char *fmt, ...) __attribute__((cold));
static int sPerfOff; /* bt3.txt perf=0 */
extern void Vita_TextScreen(void);
extern unsigned gVglDraws, gVglDropped, gVglNewTex, gVglBatches, gVglFilm, gVitaUsGs, gVitaUsMovie, gVitaUsUpload, gVitaUp[5], gVitaUpStore[3];
extern void Vita_Pace(int lateUs);
extern unsigned gVglVuTris, gVglVuBatches;
extern int Vita_MovieStats(unsigned stat[4], unsigned *decodeUs);
static unsigned sLateSum;
void Vita_Log(const char *fmt, ...) {
    char msg[1400]; /* (was 400, which cut the long lines and their ends) */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    logf_("%s", msg);
}
extern int gPortFastFloat;
extern uint32_t gVitaHeapBase, gVitaHeapUsed;
extern unsigned gVitaFileOpens, gVitaFileBytes, gPortVBlanks;
extern void Vita_FloatMode(int fast);
extern int Vita_FloatSelfTest(unsigned long long (*now)(void), unsigned nsPerOp[2][7], unsigned diffPermille[7], const char **names);
extern void Vita_MapMemory(int cdram);
extern void Vita_LoadGameData(const char *disc);
extern int __real_main(int argc, char **argv);

/* The fight's state, read the way port/src/headless.c reads it (integer interfaces only: this file is compiled
   for the Vita's float ABI, the game's is software float). */
extern struct { int state; } *gBtlSeq;
/* The list may be drawn while the game goes on: not while a film plays (its pictures come from the game's thread). */
int Vita_InFight(void) { return gBtlSeq != NULL && gBtlSeq->state == 3; }
/* the battle from its stage intro to its end (states 0..6): the same stage and fighters the whole time, drawn the fast
   way (kept parts, fighters on the chip) and shown every two blanks */
int Vita_InBattle(void) { return gBtlSeq != NULL && gBtlSeq->state >= 0 && gBtlSeq->state <= 6; }
/* bt3.txt filmstate=<n>: the film's pictures in that state of the fight (3, the fight, unless told: 0 the
   fighters' intros) */
int gVitaFilmState = 3;
/* The stage's first seconds: the renderer meets the stage's textures, its geometry and the models' parts
   for the first time and makes them (a picture of 133 ms, a dozen of 40 to 90). In these blanks the game
   waits for a picture still being drawn instead of leaving its own out: the intro plays without jumps, a moment later.
   (bt3.txt warm=<blanks>, 0: off.) */
int gVitaWarmVb = 300;
static unsigned sState0Vb, sVbNow;
/* bt3.txt introwait=1 (the default): the same for the whole of the intros (states 0 to 2: the stage's
   flight, the fighters' entrances): a picture that takes long holds the game up a little instead of the next
   picture being left out, which showed as a skip in the stage's intro. Not in the fight. */
int gVitaIntroWait = 1;
int gVitaSeaOff; /* bt3.txt seaoff=1: the stage's surfaces (eft_c.c EftSurf_Draw) not drawn */
int gVitaSurfCull = 1; /* eft_c.c EftSurf_DrawMeshes: surface triangles wholly outside a clip plane not prepared */
int Vita_WarmUp(void) {
    if (gBtlSeq == NULL) {
        return 0;
    }
    if (gVitaIntroWait && gBtlSeq->state >= 0 && gBtlSeq->state <= 2) {
        return 1;
    }
    return gVitaWarmVb > 0 && gBtlSeq->state == 0 && sVbNow - sState0Vb < (unsigned)gVitaWarmVb;
}
unsigned gVitaWarmWaits;
int gVitaFilmMax; /* bt3.txt filmmax=<n>: at most n pictures (the log's size: 75 of them made 18 MB, too much to fetch) */
/* bt3.txt filmtrig=<n>: the film only in bursts of n pictures in a row, each from a skill's name coming up
   in the fight (HudCombo_SetText: Wild Sense, Dragon Fist...), the moments the flicker of the ground under the water
   was seen in */
int gVitaFilmTrig;
static volatile unsigned sLogLost; /* (the log, below) */
static volatile int sFilmBurst;
static unsigned sFilmTrigs;
void Vita_FilmTrigger(int side, int text) {
    if (gVitaFilmTrig > 0 && Vita_InFight() && sFilmBurst <= 0) {
        sFilmBurst = gVitaFilmTrig;
        sFilmTrigs++;
        logf_("film: burst %u from a skill's name (side %d, entry %d) at blank %u (log bytes lost so far %u)\n", sFilmTrigs, side, text, gPortVBlanks, sLogLost);
    }
}
/* bt3.txt filmlist=<a>-<b>: the film in the lists a..b of the fight (the demo fight is the same in every
   run: the same lists show the same moment), every one of them drawn with filmwait=1 (the game waits for the
   picture instead of leaving lists out), so that the pictures follow one another */
int gVitaFilmFrom, gVitaFilmTo, gVitaFilmWait, gVitaFilmFrom2, gVitaFilmTo2;
extern unsigned gGsFrame;
int Vita_FilmWait(void) {
    return gVitaFilmWait && gVitaFilmTo > 0 && gGsFrame + 40u >= (unsigned)gVitaFilmFrom && gGsFrame <= (unsigned)gVitaFilmTo;
}
static int film_window(void) {
    return gVitaFilmTo > 0 && gGsFrame + 2u >= (unsigned)gVitaFilmFrom && gGsFrame <= (unsigned)gVitaFilmTo;
}
int Vita_FilmNow(void) {
    if (gVitaFilmTo > 0) {
        return (gGsFrame >= (unsigned)gVitaFilmFrom && gGsFrame <= (unsigned)gVitaFilmTo) ||
               (gVitaFilmTo2 > 0 && gGsFrame >= (unsigned)gVitaFilmFrom2 && gGsFrame <= (unsigned)gVitaFilmTo2);
    }
    if (gVitaFilmTrig > 0) {
        return sFilmBurst > 0;
    }
    return gBtlSeq != NULL && gBtlSeq->state == gVitaFilmState;
}
/* the game's side of a filmed moment: the stage's effects of the frame being made (a line each frame of a
   burst; the pictures follow a frame or two later) */
static int sTintMed = -1, sTintOn, sTintAdd, sCamY16, sWaterY16;
void Vita_FilmTintNote(int medium, int enabled, int additive, int camY16, int waterY16) {
    sTintMed = medium;
    sTintOn = enabled;
    sTintAdd = additive;
    sCamY16 = camY16;
    sWaterY16 = waterY16;
}
void Vita_FilmFxNote(unsigned bits, unsigned flagsHi, unsigned flagsLo) {
    static unsigned lastHi, lastLo, lastBits;
    static int lastMed = -2;
    if ((gVitaFilmTrig > 0 && sFilmBurst > 0) || film_window()) {
        logf_("film: frame at blank %u (list drawn last %u): stage features %x, battle flags %08x %08x (glare and fog %s), depth tint medium %d (%s%s), camera y %d.%02d, water %d.%02d\n",
              gPortVBlanks, gGsFrame, bits, flagsHi, flagsLo, (flagsHi & 0x04000000u) ? "left out" : "drawn", sTintMed, sTintOn ? "on" : "off",
              sTintAdd ? ", adds" : "", sCamY16 / 16, (sCamY16 < 0 ? -sCamY16 : sCamY16) % 16 * 100 / 16, sWaterY16 / 16,
              (sWaterY16 < 0 ? -sWaterY16 : sWaterY16) % 16 * 100 / 16);
    } else if (gVitaFilmTrig > 0 && Vita_InFight() && (((flagsHi ^ lastHi) & 0x04000000u) || bits != lastBits || sTintMed != lastMed)) {
        logf_("film: at blank %u: stage features %x, battle flags %08x %08x, depth tint medium %d (%s), camera y %d, water %d (sixteenths)\n",
              gPortVBlanks, bits, flagsHi, flagsLo, sTintMed, sTintOn ? "on" : "off", sCamY16, sWaterY16);
    }
    lastHi = flagsHi;
    lastLo = flagsLo;
    lastBits = bits;
    lastMed = sTintMed;
}
void Vita_FilmTaken(void) {
    if (sFilmBurst > 0) {
        sFilmBurst--;
    }
}
extern int Vita_MoviePlaying(void);
int Vita_AsyncOk(void) { return (gBtlSeq != NULL && gBtlSeq->state >= 2) || !Vita_MoviePlaying(); }
extern int *BtlSeq_GetClock(void);
extern int BtlCharApi_GetHp(int objId);

static unsigned long long now_us(void) {
    return sceKernelGetProcessTimeWide();
}

/* The log: written into memory by whoever logs, and to the file by a thread of its own on the second core, ten
   times a second (a line written straight to the memory card took the game's thread 1-3 ms: the 120 lines every
   two seconds were a fifth of its time). log_sync writes what waits at once (fatal messages, the end). */
static char sLogBuf[1024 * 1024]; /* (was 256 KB; a filmed list's picture can go out whole) */
static volatile unsigned sLogHead, sLogTail, sLogLost;
static unsigned sLogSplit, sLogBytes; /* bt3.txt logsplit=<KB> */
static int sLogPart = 1;
static volatile int sLogLock, sLogDrainLock, sLogThreaded;
static void log_put(const char *p, unsigned n) {
    while (__sync_lock_test_and_set(&sLogLock, 1)) {
    }
    if (sLogHead + n - sLogTail <= sizeof(sLogBuf)) {
        unsigned at = sLogHead % sizeof(sLogBuf), first = n < sizeof(sLogBuf) - at ? n : sizeof(sLogBuf) - at;
        memcpy(sLogBuf + at, p, first);
        memcpy(sLogBuf, p + first, n - first);
        __sync_synchronize();
        sLogHead += n;
    } else {
        sLogLost += n;
    }
    __sync_lock_release(&sLogLock);
}
static void log_sync(void) {
    unsigned h, t;
    while (__sync_lock_test_and_set(&sLogDrainLock, 1)) {
        sceKernelDelayThread(100);
    }
    h = sLogHead;
    __sync_synchronize();
    t = sLogTail;
    if (sLog != NULL && t != h) {
        while (t != h) {
            unsigned at = t % sizeof(sLogBuf), n = h - t < sizeof(sLogBuf) - at ? h - t : sizeof(sLogBuf) - at;
            fwrite(sLogBuf + at, 1, n, sLog);
            t += n;
            sLogBytes += n;
        }
        fflush(sLog);
        if (sLogSplit != 0 && sLogBytes >= sLogSplit && sLogPart < 9) { /* the next part */
            char path[64];
            FILE *f;
            snprintf(path, sizeof(path), HOME "/log-part%d" SFX, sLogPart + 1);
            if ((f = fopen(path, "w")) != NULL) {
                fclose(sLog);
                sLog = f;
                sLogPart++;
                sLogBytes = 0;
            }
        }
    }
    sLogTail = h;
    __sync_lock_release(&sLogDrainLock);
}
static int log_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    for (;;) {
        sceKernelDelayThread(100000);
        log_sync();
    }
    return 0;
}
static void logf_(const char *fmt, ...) {
    va_list ap;

    if (sLog != NULL) {
        if (sLogThreaded) {
            char line[1400];
            int n;
            va_start(ap, fmt);
            n = vsnprintf(line, sizeof(line), fmt, ap);
            va_end(ap);
            if (n > (int)sizeof(line) - 1) {
                n = (int)sizeof(line) - 1;
                line[n - 1] = '\n'; /* (a cut line still ends its line) */
            }
            if (n > 0) {
                log_put(line, (unsigned)n);
            }
            return;
        }
        va_start(ap, fmt);
        vfprintf(sLog, fmt, ap);
        va_end(ap);
        fflush(sLog);
    }
}

static unsigned buttons(void) {
    SceCtrlData pad;

    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    return pad.buttons;
}

static unsigned wait_buttons(unsigned mask) {
    unsigned b;

    while (buttons() & mask) { /* released first: the press that brought us here does not count */
        sceKernelDelayThread(20000);
    }
    for (;;) {
        b = buttons() & mask;
        if (b != 0) {
            return b;
        }
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        sceKernelDelayThread(20000);
    }
}

static void quit(void) {
    log_sync();
    if (sLog != NULL) {
        fflush(sLog);
    }
    fflush(stdout);
    fflush(stderr);
    sceKernelExitProcess(0);
    for (;;) {
        sceKernelDelayThread(1000000);
    }
}

/* A message the program cannot go on after: red on the screen, in the log, then START ends the program. */
void Vita_Fatal(const char *fmt, ...) {
    char msg[600];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    sEnded = 1;
    logf_("FATAL: %s\n", msg);
    log_sync();
    Vita_TextScreen();
    psvDebugScreenPrintf("\n\e[31m%s\e[39m\n\nPress START to quit.\n", msg);
    wait_buttons(SCE_CTRL_START);
    quit();
}

/* A function of the PS2's libraries that the Vita build has no stand-in for (build.py generates one caller of this
   per missing name, port/build/gen/vita_<variant>/stubs.c). None of them is reached in the game. */
void Port_Missing(const char *name) {
    Vita_Fatal("not available on the Vita: %s", name);
}

static void summary(int final) {
    unsigned long long t = now_us();
    unsigned vb = gPortVBlanks - sBattleStartVb;
    unsigned long long us = t - sBattleStart;

    if (sBattleStart != 0 && us > 0 && vb > 0) {
        unsigned perSec10 = (unsigned)((unsigned long long)vb * 10000000ull / us);
        unsigned percent = perSec10 * 1000u / 5994u;
        char line[200];

        snprintf(line, sizeof(line), "fight so far: %u vertical blanks in %u.%u s = %u.%u per second = %u%% of the console's speed",
                 vb, (unsigned)(us / 1000000), (unsigned)(us / 100000 % 10), perSec10 / 10, perSec10 % 10, percent);
        psvDebugScreenPrintf("%s\e[K\n", line);
        if (final) {
            logf_("vita: %s\n", line);
        }
    }
}

/* Called by Port_VBlank (port/src/plat_stub.c) at every vertical blank of the game: twice a second it redraws
   the status. The game does not wait for anything in this build, so blanks per second IS the game's speed. */
static unsigned long long sStallEndT; /* (the game's thread: when it last came out of a blank's wait, and the console's count then) */
static unsigned sStallEndVc;
void Vita_VBlank(unsigned vblanks) {
    unsigned long long t;

    {   /* with a picture the vertical blank is the clock, as with a window on PC: one every 1 / 59.94 s, on a grid
           that stays where it is. A frame that took too long is made up for by the waits after it being shorter
           (up to two blanks' time; beyond that the grid moves along with the game): a battle frame and a film's
           frame wait for two blanks, and the second wait must not add a whole blank to a frame that was only late
           for the first. This is the only pacing on the Vita (Port_VBlank in plat_stub.c leaves its own out). */
        /* the grid is the console's own blanks: the game waits for the blank of its tick itself, so the
           ticks and the pictures shown at them (vita_vgl.c) never drift apart. "late": how long after that blank
           the game came, worked out from the last blank it waited for. */
        static unsigned nextVc, vbAt;
        static unsigned long long vbT;
        static int have;
        unsigned long long now = now_us();
        unsigned vc = (unsigned)sceDisplayGetVcount();
        long long late;
        {   /* the game's thread stopped between two of its blanks without running: the whole program
               held up from outside (1.3 s of a fight at 64 s, with no thread of it busy) */
            static unsigned long long endRun;
            static unsigned stalls;
            SceKernelThreadInfo ti;
            memset(&ti, 0, sizeof(ti));
            ti.size = sizeof(ti);
            if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) >= 0) {
                if (sStallEndT != 0 && now - sStallEndT > 60000 && (now - sStallEndT) - (ti.runClocks - endRun) > 40000 && stalls < 40) {
                    stalls++;
                    logf_("vita: the game's thread held up at %u ms: %u ms between two of its blanks, of them %u ms running; the console's blanks meanwhile %u\n",
                          (unsigned)((now - sStart) / 1000), (unsigned)((now - sStallEndT) / 1000), (unsigned)((ti.runClocks - endRun) / 1000), vc - sStallEndVc);
                }
                endRun = ti.runClocks;
            }
        }

        if (!have) {
            nextVc = vc;
            vbAt = vc;
            vbT = now;
            have = 1;
        }
        if ((int)(vc - nextVc) > 2) { /* (more than two blanks late: the grid moves along with the game) */
            nextVc = vc - 2;
        }
        if ((int)(vc - nextVc) < 0) {
            extern volatile unsigned char gProfGame;
            gProfGame = 1;
            while ((int)((unsigned)sceDisplayGetVcount() - nextVc) < 0) {
                sceDisplayWaitVblankStart();
            }
            gProfGame = 0;
            vbT = now_us();
            vbAt = (unsigned)sceDisplayGetVcount();
            late = -1;
        } else {
            late = (long long)now - ((long long)vbT + (long long)(int)(nextVc - vbAt) * 16683);
            if (late < 0) {
                late = 0;
            }
        }
        {
            extern volatile unsigned gVitaGridVb;
            extern unsigned long long gVitaTickT[64];
            extern volatile int gVitaGameLate;
            gVitaTickT[nextVc & 63] = (unsigned long long)((long long)vbT + (long long)(int)(nextVc - vbAt) * 16683);
            gVitaGameLate = late > 0 ? (int)late : 0;
            __sync_synchronize();
            gVitaGridVb = nextVc;
        }
        nextVc++;
        sStallEndT = now_us();
        sStallEndVc = (unsigned)sceDisplayGetVcount();
        Vita_Pace((int)late); /* (vita_gfx.c: is the next frame drawn?) */
        sLateSum += late > 0 ? (unsigned)late : 0;
    }
    t = now_us();
    unsigned state = gBtlSeq != NULL ? (unsigned)gBtlSeq->state : 0xFFFFFFFEu;

    sVbNow = vblanks;
    if (state != sLastState) {
        logf_("vita: vertical blank %u, %u ms: fight sequence state %d\n", vblanks, (unsigned)((t - sStart) / 1000), (int)state);
        if (state == 0) {
            sState0Vb = vblanks; /* (the warm-up's blanks count from here) */
        }
        if (sBattleStart == 0 && gBtlSeq != NULL && gBtlSeq->state >= 2) {
            sBattleStart = t;
            sBattleStartVb = vblanks;
        }
        sLastState = state;
    }
    {   /* the sweep (sSweepSec) */
        static unsigned long long sFightT;
        static int sSweepDone, sShots;
        if (sSweepSec > 0 && sMode == 1 && !sSweepDone) {
            if (sFightT == 0 && state == 3) {
                sFightT = t;
            }
            if (sFightT != 0 && sShots < 2 && t - sFightT > (sShots == 0 ? 10000000ull : 22000000ull)) { /* pictures for the log */
                extern volatile int gVitaShotReq;
                sShots++;
                gVitaShotReq = 1;
                logf_("vita: sweep: pictures asked for (pairing %s, %d s into the fight): with the ground shadows, then without\n",
                      getenv("BT3_DEMO_PICK") != NULL ? getenv("BT3_DEMO_PICK") : "?", sShots == 1 ? 10 : 22);
            }
            if (sFightT != 0 && t - sFightT > (unsigned long long)sSweepSec * 1000000ull) {
                extern int sceAppMgrLoadExec(const char *appPath, char *const argv[], void *optParam);
                int r;
                sSweepDone = 1;
                logf_("vita: sweep: pairing %s has fought %d s; the program starts again for the next pairing\n", getenv("BT3_DEMO_PICK") != NULL ? getenv("BT3_DEMO_PICK") : "?", sSweepSec);
                log_sync();
                if (sLog != NULL) {
                    fflush(sLog);
                }
                fflush(stdout);
                fflush(stderr);
                r = sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
                logf_("vita: sweep: starting again failed (%08x); this pairing goes on\n", (unsigned)r);
            }
        }
    }
    if (t - sLastDraw < 500000) {
        return;
    }
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    static int sClockFix;
    if (sClockFix < 8 && (scePowerGetGpuClockFrequency() < 222 || scePowerGetArmClockFrequency() < 444 || scePowerGetBusClockFrequency() < 222)) {
        /* the clocks put back when the system lowered them (after a suspend, say): the console's own top speeds
           (eight times at most: a system that will not give them is not asked forever) */
        sClockFix++;
        logf_("vita: clocks found lowered (processor %d, bus %d, graphics chip %d MHz): set again\n", scePowerGetArmClockFrequency(),
              scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency());
        scePowerSetArmClockFrequency(444);
        scePowerSetBusClockFrequency(222);
        scePowerSetGpuClockFrequency(222);
        scePowerSetGpuXbarClockFrequency(166);
    }
    {
        static unsigned n, vb0, drawn0;
        static unsigned long long t0;
#ifdef PORT_VITA_RELEASE
        if (++n % 4 == 0 && gVitaReport) { /* (the released program: only with bt3.txt report=1) */
#else
        if (++n % 4 == 0) {
#endif
            if (t0 != 0) {
                unsigned vb = vblanks - vb0, pics = gVitaGsDrawn - drawn0, perPic = pics != 0 ? pics : 1, film[4], filmUs;
                struct mallinfo mi = mallinfo();
                if (vb == 0) {
                    vb = 1;
                }
                logf_("vita: %u s: speed %u%%, %u.%u pictures/s, late %u us/blank | per picture: %u prims (%u dropped), %u batches | %u new textures |"
                      " per blank: list %u us, uploads %u us in %u calls: %u px one by one, %u by block, %u kept | store: %u uploads, %u KB, %u sweeps |"
                      " heap %u KB\n",
                      (unsigned)((t - sStart) / 1000000), (unsigned)((unsigned long long)(vblanks - vb0) * 1000000ull * 1000ull / 5994ull * 10ull / (t - t0)),
                      (unsigned)((unsigned long long)pics * 10000000ull / (t - t0)) / 10, (unsigned)((unsigned long long)pics * 10000000ull / (t - t0)) % 10,
                      sLateSum / vb, gVglDraws / perPic, gVglDropped / perPic, gVglBatches / perPic, gVglNewTex, gVitaUsGs / vb, gVitaUsUpload / vb,
                      gVitaUp[3] / vb, gVitaUp[0] / vb, gVitaUp[1] / vb, gVitaUp[2] / vb, gVitaUpStore[0], gVitaUpStore[1] >> 10, gVitaUpStore[2],
                      (unsigned)mi.uordblks >> 10);
                {   /* the list the drawing thread is on: windows of a fixed range of lists compared build to build */
                    extern unsigned gGsFrame;
                    logf_("vita:   list now %u\n", gGsFrame);
                }
                {
                    extern unsigned gVitaAudioBlocks, gVitaAudioStarved;
                    static unsigned ab;
                    if (gVitaAudioBlocks != ab) {
                        logf_("vita:   sound: %u blocks played in this time, %u times a stream ran dry\n", gVitaAudioBlocks - ab, gVitaAudioStarved);
                        ab = gVitaAudioBlocks;
                        gVitaAudioStarved = 0;
                    }
                }
                {
                    extern unsigned gVitaUnpack[2], gVitaList[2][2];
                    logf_("vita:   lists: %u not drawn, %u us each; %u drawn, %u us each | vectors unpacked per blank: %u the short way, %u the long way\n",
                          gVitaList[0][0], gVitaList[0][1] / (gVitaList[0][0] ? gVitaList[0][0] : 1), gVitaList[1][0],
                          gVitaList[1][1] / (gVitaList[1][0] ? gVitaList[1][0] : 1), gVitaUnpack[0] / vb, gVitaUnpack[1] / vb);
                    {
                        extern unsigned gVglUs[6], gVglCalls[6];
                        logf_("vita:   per picture, us (calls): vertex-program calls %u (%u), of them strips %u (%u); state %u (%u); GS primitives %u (%u); to the chip %u (%u); swap %u (%u)\n",
                              gVglUs[5] / perPic, gVglCalls[5] / perPic, gVglUs[0] / perPic, gVglCalls[0] / perPic, gVglUs[1] / perPic, gVglCalls[1] / perPic,
                              gVglUs[4] / perPic, gVglCalls[4] / perPic, gVglUs[2] / perPic, gVglCalls[2] / perPic, gVglUs[3] / perPic, gVglCalls[3] / perPic);
                        memset(gVglUs, 0, sizeof(gVglUs));
                        memset(gVglCalls, 0, sizeof(gVglCalls));
                    }
                    {
                        extern unsigned gProfHist[2][12], gProfGameHist[3], gVitaUnpackSkipped, gVitaWaitSkips, gVitaSnap[2];
                        static const char *const kName[10] = {"idle", "list", "unpack", "GS packets", "uploads", "program calls", "strips", "to the chip", "swap", "GS primitives"};
                        char line[400];
                        int n = 0, k;
                        for (k = 0; k < 10; k++) {
                            n += snprintf(line + n, sizeof(line) - (size_t)n, "%s %u/%u%s", kName[k], gProfHist[0][k], gProfHist[1][k], k < 9 ? ", " : "");
                        }
                        logf_("vita:   picture thread, ms in frames not drawn / drawn: %s | %u vectors per blank not unpacked\n", line, gVitaUnpackSkipped / vb);
                        extern unsigned gVitaSnapSame2[2];
                        logf_("vita:   game thread, ms: running %u, waiting for the blank %u, waiting for the picture thread %u | %u lists not drawn for that wait | %u pieces (%u bytes) copied for the lists"
                              " | images read in place as unchanged %u, copied %u\n",
                              gProfGameHist[0], gProfGameHist[1], gProfGameHist[2], gVitaWaitSkips, gVitaSnap[0], gVitaSnap[1], gVitaSnapSame2[0], gVitaSnapSame2[1]);
                        gVitaSnapSame2[0] = gVitaSnapSame2[1] = 0;
                        if (gVitaWarmWaits) {
                            logf_("vita:   the intro: the game waited for a picture %u times instead of leaving one out\n", gVitaWarmWaits);
                            gVitaWarmWaits = 0;
                        }
                        watch_log();
                        {
                            extern unsigned gVitaFileReads, gVitaFileReadsGame;
                            extern unsigned long long gVitaFileUs, gVitaFileUsGame;
                            logf_("vita:   file reads: %u taking %u ms (on the game's thread: %u taking %u ms)\n", gVitaFileReads, (unsigned)(gVitaFileUs / 1000), gVitaFileReadsGame, (unsigned)(gVitaFileUsGame / 1000));
                            gVitaFileReads = gVitaFileReadsGame = 0;
                            gVitaFileUs = gVitaFileUsGame = 0;
                        }
                        {
                            extern unsigned gProfWorkHist, gVitaVj[4], gVitaLz[3];
                            logf_("vita:   strip thread: busy %u ms; per picture: %u strips given to it (%u read in the list, %u copied), waited for %u times | vectors per blank left in the list %u, copied after all %u\n",
                                  gProfWorkHist, gVitaVj[0] / perPic, gVitaVj[1] / perPic, gVitaVj[2] / perPic, gVitaVj[3] / perPic, gVitaLz[0] / vb, gVitaLz[1] / vb);
                            gProfWorkHist = 0;
                            memset(gVitaVj, 0, sizeof(gVitaVj));
                            memset(gVitaLz, 0, sizeof(gVitaLz));
                        }
                        {
                            extern unsigned gVitaRef[5][2];
                            unsigned nl = gVitaList[0][0] + gVitaList[1][0];
                            nl = nl ? nl : 1;
                            logf_("vita:   data the lists refer to, per list, pieces (KB): up to 32 quadwords %u (%u), 128: %u (%u), 512: %u (%u), 2048: %u (%u), more: %u (%u)\n",
                                  gVitaRef[0][0] / nl, gVitaRef[0][1] / 64 / nl, gVitaRef[1][0] / nl, gVitaRef[1][1] / 64 / nl, gVitaRef[2][0] / nl, gVitaRef[2][1] / 64 / nl,
                                  gVitaRef[3][0] / nl, gVitaRef[3][1] / 64 / nl, gVitaRef[4][0] / nl, gVitaRef[4][1] / 64 / nl);
                            {
                                extern unsigned gVitaFresh;
                                { extern unsigned gVitaStage[3]; logf_("vita:   stage on the graphics chip: %u strips per picture drawn from it, %u strips written to it, %u vertices kept there\n", gVitaStage[0] / perPic, gVitaStage[1], gVitaStage[2]); gVitaStage[0] = gVitaStage[1] = 0; }
                                { extern unsigned gGrIdleUs; logf_("vita:   drawing thread waited for the list reader %u ms in drawn lists\n", gGrIdleUs / 1000); gGrIdleUs = 0; }
                                { extern unsigned long long gVitaTexMakeUs; extern unsigned gVitaBeList[3];
                                  extern unsigned long long gVitaTexReadUs, gVitaTexPx; extern float gVglCov[2]; extern uint32_t vgl_flip_hist[5];
                                  extern unsigned gVitaSwz, gVitaPal[3], gVitaPaints, gVitaSigCheck[4];
                                  logf_("vita:   page-number check: texture keys %u with pages unchanged, %u of them made otherwise; states %u, %u otherwise\n", gVitaSigCheck[0], gVitaSigCheck[1], gVitaSigCheck[2], gVitaSigCheck[3]);
                                  memset(gVitaSigCheck, 0, sizeof(gVitaSigCheck));
                                  logf_("vita:   sprites drawn into palette pages (GS memory): %u\n", gVitaPaints);
                                  gVitaPaints = 0;
                                  logf_("vita:   drawing thread: longest drawn list %u us, %u over 30 ms, %u over 33 ms | new textures took %u ms (reading GS memory %u ms, %u k pixels, %u swizzled; %u with a palette: %u index sets, %u palettes made)\n", gVitaBeList[0], gVitaBeList[1], gVitaBeList[2],
                                        (unsigned)(gVitaTexMakeUs / 1000), (unsigned)(gVitaTexReadUs / 1000), (unsigned)(gVitaTexPx / 1000), gVitaSwz, gVitaPal[2], gVitaPal[0], gVitaPal[1]);
                                  gVitaSwz = 0; gVitaPal[0] = gVitaPal[1] = gVitaPal[2] = 0;
                                  { extern void Vita_TexTopLog(void); Vita_TexTopLog(); }
                                  { extern void Vita_T121Log(void); Vita_T121Log(); }
                                  { extern void Vita_PaceLog(void); Vita_PaceLog(); }
                                  { extern void Vita_AmLog(void); Vita_AmLog(); }
                                  { extern void Vita_FastLog(void); Vita_FastLog(); }
                                  logf_("vita:   GS primitives cover %u.%02u screens opaque, %u.%02u blended per picture | pictures on the screen for 0 / 1 / 2 / 3 / 4+ blanks: %u %u %u %u %u\n",
                                        (unsigned)(gVglCov[0] / perPic / (512.0f * 448.0f)), (unsigned)(gVglCov[0] / perPic / (512.0f * 448.0f) * 100.0f) % 100,
                                        (unsigned)(gVglCov[1] / perPic / (512.0f * 448.0f)), (unsigned)(gVglCov[1] / perPic / (512.0f * 448.0f) * 100.0f) % 100,
                                        vgl_flip_hist[0], vgl_flip_hist[1], vgl_flip_hist[2], vgl_flip_hist[3], vgl_flip_hist[4]);
                                  { extern unsigned gVitaRej[2][8];
                                    logf_("vita:   not drawn (16-bit or depth target / work buffer target / depth as texture / picture as texture / drawn-into buffer as texture / no scissor / other): GS primitives %u %u %u %u %u %u %u, fighter strips %u %u %u %u %u %u %u\n",
                                          gVitaRej[0][1], gVitaRej[0][2], gVitaRej[0][3], gVitaRej[0][4], gVitaRej[0][5], gVitaRej[0][6], gVitaRej[0][0] + gVitaRej[0][7],
                                          gVitaRej[1][1], gVitaRej[1][2], gVitaRej[1][3], gVitaRej[1][4], gVitaRej[1][5], gVitaRej[1][6], gVitaRej[1][0] + gVitaRej[1][7]);
                                    memset(gVitaRej, 0, sizeof(gVitaRej)); }
                                  { extern unsigned gVglSkip[4], gVglScissorN; extern float gVglSkipArea[2];
                                    logf_("vita:   GS primitives left out as they change nothing: %u without alpha (%u.%02u screens per picture), %u writing only the alpha nothing reads or blended with an alpha of 0 (%u.%02u screens per picture); %u read the picture's alpha (%u model draws) | %u scissor changes per picture\n",
                                          gVglSkip[0], (unsigned)(gVglSkipArea[0] / perPic / (512.0f * 448.0f)), (unsigned)(gVglSkipArea[0] / perPic / (512.0f * 448.0f) * 100.0f) % 100,
                                          gVglSkip[1], (unsigned)(gVglSkipArea[1] / perPic / (512.0f * 448.0f)), (unsigned)(gVglSkipArea[1] / perPic / (512.0f * 448.0f) * 100.0f) % 100, gVglSkip[2], gVglSkip[3], gVglScissorN / perPic);
                                    memset(gVglSkip, 0, sizeof(gVglSkip)); gVglSkipArea[0] = gVglSkipArea[1] = 0.0f; gVglScissorN = 0;
                                    { extern unsigned gVitaAlDead[2]; logf_("vita:   of them after the picture's last read of its alpha: %u, %u.%02u screens per picture\n", gVitaAlDead[0], gVitaAlDead[1] / perPic / 100, gVitaAlDead[1] / perPic % 100); gVitaAlDead[0] = gVitaAlDead[1] = 0; } }
                                  memset(gVitaBeList, 0, sizeof(gVitaBeList)); gVitaTexMakeUs = gVitaTexReadUs = gVitaTexPx = 0; gVglCov[0] = gVglCov[1] = 0.0f; memset(vgl_flip_hist, 0, sizeof(vgl_flip_hist)); }
                                { extern unsigned long long gVitaFeUs[2][2], gGrRoomUs; logf_("vita:   list reader: %u us per drawn list, %u per list not drawn; %u ms waiting for room in the drawing thread's queue\n", (unsigned)(gVitaFeUs[1][1] / (gVitaFeUs[1][0] ? gVitaFeUs[1][0] : 1)), (unsigned)(gVitaFeUs[0][1] / (gVitaFeUs[0][0] ? gVitaFeUs[0][0] : 1)), (unsigned)(gGrRoomUs / 1000)); memset(gVitaFeUs, 0, sizeof(gVitaFeUs)); gGrRoomUs = 0; }
                                { extern unsigned gVitaGpuWait[2]; if (gVitaGpuWait[1]) { logf_("vita:   graphics chip still busy at the swap: %u us on average (%u times)\n", gVitaGpuWait[0] / gVitaGpuWait[1], gVitaGpuWait[1]); } gVitaGpuWait[0] = gVitaGpuWait[1] = 0; }
                                { extern unsigned gVitaFt[3]; logf_("vita:   fighters on the graphics chip: %u strips per picture drawn from it, %u strips written to it, %u vertices kept there\n", gVitaFt[0] / perPic, gVitaFt[1], gVitaFt[2]); gVitaFt[0] = gVitaFt[1] = 0; }
                            { extern unsigned gVitaUpSkipped; logf_("vita:   %u uploads not looked at in frames that are not drawn\n", gVitaUpSkipped); gVitaUpSkipped = 0; }
                            logf_("vita:   %u lists referred to something new and were finished before the game went on\n", gVitaFresh);
                                gVitaFresh = 0;
                            }
                            memset(gVitaRef, 0, sizeof(gVitaRef));
                        }
                        {
                            extern int gVitaPerfOn;
                            extern void Vita_PerfAccum(uint32_t last[5], uint64_t sum[5]);
                            extern uint64_t gVitaPerfList[2][5], gVitaPerfWork[5];
                            extern unsigned gVitaPerfLists[2];
                            static uint32_t gameLast[5];
                            static uint64_t game[5];
                            if (gVitaPerfOn) {
                                const uint64_t *t[4] = {game, gVitaPerfList[0], gVitaPerfList[1], gVitaPerfWork};
                                static const char *const nm[4] = {"game thread, per frame", "picture thread, per list not drawn", "picture thread, per list drawn", "strip thread, per drawn picture"};
                                unsigned div[4], k;
                                memset(game, 0, sizeof(game));
                                Vita_PerfAccum(gameLast, game);
                                div[0] = gVitaPerfLists[0] + gVitaPerfLists[1];
                                div[1] = gVitaPerfLists[0];
                                div[2] = gVitaPerfLists[1];
                                div[3] = gVitaPerfLists[1];
                                for (k = 0; k < 4; k++) {
                                    const uint64_t *c = t[k];
                                    unsigned d = div[k] ? div[k] : 1;
                                    unsigned ki = (unsigned)(c[3] / 1000) ? (unsigned)(c[3] / 1000) : 1;
                                    logf_("vita:   counters, %s: %u k cycles, %u k instructions (%u.%02u per cycle), data cache misses %u, instruction cache misses %u, mispredicted branches %u (per 1000 instructions: %u, %u, %u)\n",
                                          nm[k], (unsigned)(c[0] / d / 1000), (unsigned)(c[3] / d / 1000), (unsigned)(c[0] ? c[3] * 100 / c[0] / 100 : 0), (unsigned)(c[0] ? c[3] * 100 / c[0] % 100 : 0),
                                          (unsigned)(c[1] / d), (unsigned)(c[2] / d), (unsigned)(c[4] / d), (unsigned)(c[1] / ki), (unsigned)(c[2] / ki), (unsigned)(c[4] / ki));
                                }
                                {
                                    extern uint64_t gVitaRegion[12][5];
                                    extern unsigned gVitaRegionLists;
                                    static const char *const rn[11] = {"(end)", "list", "unpack", "GS packets", "uploads", "program calls", "strips", "to the chip", "swap", "GS primitives", "kept on chip"};
                                    if (gVitaRegionLists) {
                                        unsigned r, nl = gVitaRegionLists;
                                        for (r = 1; r < 11; r++) {
                                            const uint64_t *c = gVitaRegion[r];
                                            logf_("vita:   counters, drawn list by part: %-13s %6u k cycles, %6u k instructions, %5u mispredicted, %5u data misses, %5u times\n", rn[r],
                                                  (unsigned)(c[0] / nl / 1000), (unsigned)(c[1] / nl / 1000), (unsigned)(c[2] / nl), (unsigned)(c[3] / nl), (unsigned)(c[4] / nl));
                                        }
                                        logf_("vita:   counters, one switch of part costs %u cycles, %u instructions (included in the lines above)\n",
                                              (unsigned)(gVitaRegion[11][0] / (gVitaRegion[11][4] ? gVitaRegion[11][4] : 1)), (unsigned)(gVitaRegion[11][1] / (gVitaRegion[11][4] ? gVitaRegion[11][4] : 1)));
                                        memset(gVitaRegion, 0, sizeof(gVitaRegion));
                                        gVitaRegionLists = 0;
                                    }
                                }
                                {
                                    extern uint64_t gVitaZone[48];
                                    extern unsigned gVitaZoneN[48], gVitaZoneLists;
                                    static const char *const zn[31] = {"rest of the list", "kept part drawn directly", "frame end (draws, swap)",
                                        "list end", "consts built, to strip", "strip: state", "strip: flush", "GIF: image upload",
                                        "GIF: up to a register", "GIF: A+D register write", "GS prim: parse to draw", "GS prim: strips flushed", "upload: found unchanged", "GS prim: to state",
                                        "GS prim: state", "GS prim: to flush", "upload: bytes looked at", "fighter: to DrawVu0", "GS prim: vertices (b205)", "record: ring/loop", "record: GIF", "record: REG", "record: VU consts", "record: draw tail", "back end waiting", "chip: fighter upload", "chip: fighter batch", "chip: stage upload", "chip: stage batch", "draw: to chip_emit", "GS prim: flush (draw call)"};
                                    if (gVitaZoneLists) {
                                        unsigned z, nl = gVitaZoneLists;
                                        for (z = 0; z < 31; z++) {
                                            if (gVitaZoneN[z]) {
                                                logf_("vita:   zone %2u %-26s %7u k cycles per drawn list, %6u marks, %4u cycles each\n", z, zn[z],
                                                      (unsigned)(gVitaZone[z] / nl / 1000), gVitaZoneN[z] / nl, (unsigned)(gVitaZone[z] / gVitaZoneN[z]));
                                            }
                                        }
                                        {   /* the parts of a kept part's draw and of a batch's (zones 32..) */
                                            static const char *const zx[16] = {"kept part: matched", "kept part: GS prims before", "kept part: uniform blocks",
                                                "kept part: groups, textures", "flush: program, texture", "flush: fixed state", "flush: vertices to the chip",
                                                "flush: the draw call", "upload: small, pixels (b216)", "upload: small, pages (b216)", "upload: small, rect (b216)",
                                                "kept parts drawn: draws", "upload: small, compared", "upload: large, compared", "upload: written", "GS prim: state_of (b205)"};
                                            for (z = 32; z < 48; z++) {
                                                if (gVitaZoneN[z]) {
                                                    logf_("vita:   zone %2u %-26s %7u k cycles per drawn list, %6u marks, %4u cycles each\n", z, zx[z - 32],
                                                          (unsigned)(gVitaZone[z] / nl / 1000), gVitaZoneN[z] / nl, (unsigned)(gVitaZone[z] / gVitaZoneN[z]));
                                                }
                                            }
                                        }
                                        logf_("vita:   zone mark costs %u cycles\n", (unsigned)(gVitaZone[31] / (gVitaZoneN[31] ? gVitaZoneN[31] : 1)));
                                        {   /* */
                                            extern unsigned gVitaStOf[6];
                                            extern unsigned gVitaFl[3];
                                            {
                                                extern unsigned long long gVitaGrCyc[16];
                                                extern unsigned gVitaGrN[16];
                                                extern unsigned gVitaZoneLists;
                                                static const char *const gn[16] = {"wrap", "GIF in place", "GIF copied", "register", "strip", "skip", "FBP", "frame end", "list begin",
                                                                                     "list end", "init", "GIF channel", "chip reset", "kept part", "strip pair", "t116"};
                                                char line[700];
                                                int len = 0, t;
                                                unsigned nl = gVitaZoneLists ? gVitaZoneLists : 1;
                                                for (t = 0; t < 16; t++) {
                                                    if (gVitaGrN[t] != 0 && len < (int)sizeof(line) - 60) {
                                                        len += snprintf(line + len, sizeof(line) - (unsigned)len, " %s %llu k (%u)", gn[t], gVitaGrCyc[t] / nl / 1000, gVitaGrN[t] / nl);
                                                    }
                                                }
                                                logf_("vita:   drawing thread by record, cycles per measured list (records):%s\n", line);
                                                {
                                                    extern unsigned long long gVitaGifCyc[2], gVitaDrawCyc[8];
                                                    extern unsigned gVitaGifCnt[2], gVitaDrawCnt[8];
                                                    logf_("vita:   of the GIF records, per measured list: triangle runs %llu k (%u triangles), images %llu k (%u); GS primitives the GS's way: "
                                                          "sprites %llu k (%u), triangles %llu k (%u), strips %llu k (%u), fans %llu k (%u), lines %llu k (%u)\n",
                                                          gVitaGifCyc[0] / nl / 1000, gVitaGifCnt[0] / nl, gVitaGifCyc[1] / nl / 1000, gVitaGifCnt[1] / nl,
                                                          gVitaDrawCyc[6] / nl / 1000, gVitaDrawCnt[6] / nl, gVitaDrawCyc[3] / nl / 1000, gVitaDrawCnt[3] / nl,
                                                          gVitaDrawCyc[4] / nl / 1000, gVitaDrawCnt[4] / nl, gVitaDrawCyc[5] / nl / 1000, gVitaDrawCnt[5] / nl,
                                                          gVitaDrawCyc[1] / nl / 1000, gVitaDrawCnt[1] / nl);
                                                    memset(gVitaGifCyc, 0, sizeof(gVitaGifCyc));
                                                    memset(gVitaGifCnt, 0, sizeof(gVitaGifCnt));
                                                    memset(gVitaDrawCyc, 0, sizeof(gVitaDrawCyc));
                                                    memset(gVitaDrawCnt, 0, sizeof(gVitaDrawCnt));
                                                }
                                                memset(gVitaGrCyc, 0, sizeof(gVitaGrCyc));
                                                memset(gVitaGrN, 0, sizeof(gVitaGrN));
                                            }
                                            {
                                                extern unsigned gVitaKeyPart[3], gVitaFlm[2];
                                                extern unsigned gVitaStLast;
                                                logf_("vita:   texture keys: %u with the texture's number kept, %u checked, %u of them found otherwise; quick states by PRIM and TEX0: %u taken, %u put; states as the call before's %u\n",
                                                      gVitaKeyPart[0], gVitaKeyPart[1], gVitaKeyPart[2], gVitaFlm[0], gVitaFlm[1], gVitaStLast);
                                                gVitaStLast = 0;
                                                memset(gVitaKeyPart, 0, sizeof(gVitaKeyPart));
                                                memset(gVitaFlm, 0, sizeof(gVitaFlm));
                                            }
                                            extern unsigned gVitaStOfEvict;
                                            logf_("vita:   GS primitives' states: %u the work targets' way, %u the page's way, %u found, %u found (pages unchanged), %u worked out as a check, %u worked out (%u of them pushed out by another of the picture)\n",
                                                  gVitaStOf[0], gVitaStOf[1], gVitaStOf[2], gVitaStOf[3], gVitaStOf[4], gVitaStOf[5], gVitaStOfEvict);
                                            gVitaStOfEvict = 0;
            {
                extern unsigned gVitaStX[3];
                logf_("vita:   states kept from a picture before: %u, worked out again as a check %u, %u of them found otherwise\n", gVitaStX[0], gVitaStX[1], gVitaStX[2]);
                memset(gVitaStX, 0, sizeof(gVitaStX));
                {
                    extern unsigned gVitaWtC[3];
                    extern unsigned gVitaRunEstN;
                    logf_("vita:   the work targets' way: %u states remembered taken, %u worked out, %u remembered; runs' first triangles the quick way %u\n",
                          gVitaWtC[0], gVitaWtC[1], gVitaWtC[2], gVitaRunEstN);
                    gVitaRunEstN = 0;
                    memset(gVitaWtC, 0, sizeof(gVitaWtC));
                }
            }
                                            extern unsigned gVitaTriRunN[2];
                                            logf_("vita:   GS triangles the quick way %u; the full way %u (its state open to the next), %u; triangle packets read as runs: %u runs, %u triangles\n",
                                                  gVitaFl[0], gVitaFl[1], gVitaFl[2], gVitaTriRunN[0], gVitaTriRunN[1]);
                                            gVitaTriRunN[0] = gVitaTriRunN[1] = 0;
                                            {
                                                extern unsigned gVitaFlWhy[2][8], gVitaWtWay[5];
                                                logf_("vita:   the full way not open to the next (not drawn, texture moved, DATE, reads alpha, no colour or depth, into a target, other): triangles %u %u %u %u %u %u %u, sprites %u %u %u %u %u %u %u; "
                                                      "the work targets' way: target as texture %u, the picture %u, page 0 %u, page 70 %u, into a target %u\n",
                                                      gVitaFlWhy[0][0], gVitaFlWhy[0][1], gVitaFlWhy[0][2], gVitaFlWhy[0][3], gVitaFlWhy[0][4], gVitaFlWhy[0][5], gVitaFlWhy[0][6],
                                                      gVitaFlWhy[1][0], gVitaFlWhy[1][1], gVitaFlWhy[1][2], gVitaFlWhy[1][3], gVitaFlWhy[1][4], gVitaFlWhy[1][5], gVitaFlWhy[1][6],
                                                      gVitaWtWay[0], gVitaWtWay[1], gVitaWtWay[2], gVitaWtWay[3], gVitaWtWay[4]);
                                                memset(gVitaFlWhy, 0, sizeof(gVitaFlWhy));
                                                memset(gVitaWtWay, 0, sizeof(gVitaWtWay));
                                            }
                                            {
                                                extern uint64_t gVitaTriCyc[7];
                                                unsigned long long nt = gVitaTriCyc[6] ? gVitaTriCyc[6] : 1;
                                                logf_("vita:   a run's quick triangle, cycles (%llu measured): checks %llu, bookkeeping %llu, Q %llu, batch %llu, checks %llu, vertices %llu\n",
                                                      (unsigned long long)gVitaTriCyc[6], (unsigned long long)(gVitaTriCyc[0] / nt), (unsigned long long)(gVitaTriCyc[1] / nt),
                                                      (unsigned long long)(gVitaTriCyc[2] / nt), (unsigned long long)(gVitaTriCyc[3] / nt), (unsigned long long)(gVitaTriCyc[4] / nt),
                                                      (unsigned long long)(gVitaTriCyc[5] / nt));
                                                memset(gVitaTriCyc, 0, sizeof(gVitaTriCyc));
                                                {
                                                    extern unsigned long long gVitaTriRunCyc[2];
                                                    extern unsigned gVitaGifN[6];
                                                    logf_("vita:   runs read into vertices: %llu cycles a triangle (%llu measured); GIF tags: %u packed, %u register lists, %u images; %u A+D writes, %u register-list registers\n",
                                                          gVitaTriRunCyc[1] ? gVitaTriRunCyc[0] / gVitaTriRunCyc[1] : 0ull, gVitaTriRunCyc[1], gVitaGifN[0], gVitaGifN[1], gVitaGifN[2], gVitaGifN[3], gVitaGifN[4]);
                                                    memset(gVitaTriRunCyc, 0, sizeof(gVitaTriRunCyc));
                                                    memset(gVitaGifN, 0, sizeof(gVitaGifN));
                                                }
                                            }
                                            memset(gVitaStOf, 0, sizeof(gVitaStOf));
                                            memset(gVitaFl, 0, sizeof(gVitaFl));
                                        }
                                        memset(gVitaZone, 0, sizeof(gVitaZone));
                                        memset(gVitaZoneN, 0, sizeof(gVitaZoneN));
                                        gVitaZoneLists = 0;
                                    }
                                }
                                {   /* the list reader's zones */
                                    extern uint64_t gVitaZoneF[48];
                                    extern unsigned gVitaZoneNF[48], gVitaZoneListsF;
                                    static const char *const fn[31] = {"rest of the list", "up to a program call", "program call: buffers", "program 0: its batch read",
                                        "", "", "", "strip: up to the plan", "strip: the plan", "strip: the record", "", "", "program call: the rest", "", "", "",
                                        "program 0: a layer's registers", "", "program 4: its batch read", "", "DMA: the rest", "VIF commands", "unpack", "DIRECT data",
                                        "kept calls sent", "program 0: both layers sent", "", "", "", "", ""};
                                    if (gVitaZoneListsF) {
                                        unsigned z, nl = gVitaZoneListsF;
                                        for (z = 0; z < 31; z++) {
                                            if (gVitaZoneNF[z]) {
                                                logf_("vita:   list reader zone %2u %-28s %7u k cycles per drawn list, %6u marks, %4u cycles each\n", z, fn[z],
                                                      (unsigned)(gVitaZoneF[z] / nl / 1000), gVitaZoneNF[z] / nl, (unsigned)(gVitaZoneF[z] / gVitaZoneNF[z]));
                                            }
                                        }
                                        logf_("vita:   list reader zone mark costs %u cycles\n", (unsigned)(gVitaZoneF[31] / (gVitaZoneNF[31] ? gVitaZoneNF[31] : 1)));
                                        memset(gVitaZoneF, 0, sizeof(gVitaZoneF));
                                        memset(gVitaZoneNF, 0, sizeof(gVitaZoneNF));
                                        gVitaZoneListsF = 0;
                                    }
                                    {   /* the slowest of the fight's first drawn lists, zone by zone */
                                        extern uint64_t gVitaWarmZ[2][48];
                                        extern unsigned gVitaWarmUs[2], gVitaWarmN[2];
                                        static const char *const zn2[31] = {"rest of the list", "kept part drawn directly", "frame end (draws, swap)",
                                            "list end", "consts built, to strip", "strip: state", "strip: flush", "GIF: image upload",
                                            "GIF: up to a register", "GIF: A+D register write", "GS prim: parse to draw", "GS prim: strips flushed", "upload: found unchanged", "GS prim: to state",
                                            "GS prim: state", "GS prim: to flush", "upload: bytes looked at", "fighter: to DrawVu0", "GS prim: vertices (b205)", "record: ring/loop", "record: GIF", "record: REG", "record: VU consts", "record: draw tail", "back end waiting", "chip: fighter upload", "chip: fighter batch", "chip: stage upload", "chip: stage batch", "draw: to chip_emit", "GS prim: flush (draw call)"};
                                        unsigned th, z;
                                        for (th = 0; th < 2; th++) {
                                            if (gVitaWarmN[th] == 0) {
                                                continue;
                                            }
                                            {
                                                char wl[900];
                                                int wn = snprintf(wl, sizeof(wl), "vita:   the fight's first drawn lists (%u): the slowest took the %s %u us; its zones, ms:", gVitaWarmN[th],
                                                                  th == 0 ? "drawing thread" : "list reader", gVitaWarmUs[th]);
                                                for (z = 0; z < 31 && wn > 0 && wn < (int)sizeof(wl) - 80; z++) {
                                                    if (gVitaWarmZ[th][z] >= 222000u) { /* (0.5 ms and more) */
                                                        wn += snprintf(wl + wn, sizeof(wl) - (size_t)wn, " %u %s %u.%u;", z, th == 0 ? zn2[z] : fn[z][0] ? fn[z] : "?",
                                                                       (unsigned)(gVitaWarmZ[th][z] / 444000u), (unsigned)(gVitaWarmZ[th][z] / 44400u % 10));
                                                    }
                                                }
                                                logf_("%s\n", wl);
                                            }
                                            gVitaWarmN[th] = 0;
                                            gVitaWarmUs[th] = 0;
                                            memset(gVitaWarmZ[th], 0, sizeof(gVitaWarmZ[th]));
                                        }
                                    }
                                }
                                {
                                    extern unsigned long long gVitaWall[6];
                                    logf_("vita:   drawing thread, wall ms in this stretch: swap %u, buffer maps %u, draw calls %u, new textures %u, asleep waiting for work %u; fighter glDrawElements alone %u\n",
                                          (unsigned)(gVitaWall[0] / 1000), (unsigned)(gVitaWall[1] / 1000), (unsigned)(gVitaWall[2] / 1000), (unsigned)(gVitaWall[3] / 1000), (unsigned)(gVitaWall[4] / 1000), (unsigned)(gVitaWall[5] / 1000));
                                    {
                                        extern unsigned long long gVitaWallK[4][2];
                                        logf_("vita:   draw calls by kind (ms, calls): stage %u/%u, fighters %u/%u, other models %u/%u, GS primitives %u/%u\n",
                                              (unsigned)(gVitaWallK[0][0] / 1000), (unsigned)gVitaWallK[0][1], (unsigned)(gVitaWallK[1][0] / 1000), (unsigned)gVitaWallK[1][1],
                                              (unsigned)(gVitaWallK[2][0] / 1000), (unsigned)gVitaWallK[2][1], (unsigned)(gVitaWallK[3][0] / 1000), (unsigned)gVitaWallK[3][1]);
                                        memset(gVitaWallK, 0, sizeof(gVitaWallK));
                                        {
                                            extern unsigned gVitaFlushWhy[20][2];
                                            int w;
                                            logf_("vita:   batch flushes (times/batches) by cause: GS prim %u/%u, frame end %u/%u, fighter bins full %u/%u, stage %u/%u+%u/%u, general bins full %u/%u, other",
                                                  gVitaFlushWhy[0][0], gVitaFlushWhy[0][1], gVitaFlushWhy[1][0], gVitaFlushWhy[1][1], gVitaFlushWhy[5][0], gVitaFlushWhy[5][1],
                                                  gVitaFlushWhy[6][0], gVitaFlushWhy[6][1], gVitaFlushWhy[7][0], gVitaFlushWhy[7][1], gVitaFlushWhy[11][0], gVitaFlushWhy[11][1]);
                                            for (w = 0; w < 20; w++) { if (w != 0 && w != 1 && w != 5 && w != 6 && w != 7 && w != 11 && gVitaFlushWhy[w][0]) { logf_(" [%d] %u/%u", w, gVitaFlushWhy[w][0], gVitaFlushWhy[w][1]); } }
                                            logf_("\n");
                                            memset(gVitaFlushWhy, 0, sizeof(gVitaFlushWhy));
                                            { extern unsigned gVitaOrderRuns[2]; logf_("vita:   kept parts' draws: %u runs begun anew to keep their order, %u put with an earlier run of their state\n", gVitaOrderRuns[0], gVitaOrderRuns[1]); gVitaOrderRuns[0] = gVitaOrderRuns[1] = 0; }
                                            { extern void Vita_Tex1Log(void); Vita_Tex1Log(); }
                                            { extern void Vita_UpLog(void); Vita_UpLog(); }
                                            { extern unsigned gVitaXf[3]; if (gVitaXf[0] | gVitaXf[1] | gVitaXf[2]) { logf_("vita:   the intro's cross-fade: %u pictures copied, %u fade draws, %u left out\n", gVitaXf[0], gVitaXf[1], gVitaXf[2]); memset(gVitaXf, 0, sizeof(gVitaXf)); } }
                                            { extern unsigned gVitaCc[4]; logf_("vita:   model calls: %u sent from copies, %u read the long way and recorded, %u kept, %u dropped (changed)\n", gVitaCc[0], gVitaCc[1], gVitaCc[2], gVitaCc[3]); memset(gVitaCc, 0, sizeof(gVitaCc)); }
                                            { extern unsigned gVitaDp[8], gVitaDpMerge[2]; logf_("vita:   model parts drawn directly: %u (%u draws in %u states), %u parts walked (%u kept now, %u refused, %u found in another state), %u KB of indices on the chip; the stage's draws: %u merged into %u\n", gVitaDp[0], gVitaDp[1], gVitaDp[2], gVitaDp[6], gVitaDp[3], gVitaDp[4], gVitaDp[5], gVitaDp[7], gVitaDpMerge[0], gVitaDpMerge[1]); gVitaDpMerge[0] = gVitaDpMerge[1] = 0; { unsigned kb = gVitaDp[7]; memset(gVitaDp, 0, sizeof(gVitaDp)); gVitaDp[7] = kb; } }
                                            { extern unsigned gVitaCcWhy[9], gVitaCcMax, gVitaFtWhere[4], gVitaDpMiss[34], gVitaDpShared, gVitaList[2][2]; extern unsigned long long gVitaDpUs;
                                              unsigned perList = gVitaList[1][0] ? gVitaList[1][0] : 1, best[3] = {0, 0, 0}, bi[3] = {0, 0, 0}, i2, j2;
                                              for (i2 = 0; i2 < 34; i2++) { for (j2 = 0; j2 < 3; j2++) { if (gVitaDpMiss[i2] > best[j2]) { unsigned k2; for (k2 = 2; k2 > j2; k2--) { best[k2] = best[k2 - 1]; bi[k2] = bi[k2 - 1]; } best[j2] = gVitaDpMiss[i2]; bi[j2] = i2; break; } } }
                                              logf_("vita:   calls not kept: %u too big (largest %u KB), %u wrapped, %u empty, %u strips copied, %u strips not planned, %u other constants, %u other records, %u writing the constants; %u GIF pieces copied in\n",
                                                    gVitaCcWhy[0], gVitaCcMax >> 10, gVitaCcWhy[1], gVitaCcWhy[2], gVitaCcWhy[3], gVitaCcWhy[4], gVitaCcWhy[5], gVitaCcWhy[6], gVitaCcWhy[8], gVitaCcWhy[7]);
                                              logf_("vita:   fighter strips read by the list reader: %u in a call being recorded, %u in another call, %u in no call (%u planned on the chip)\n", gVitaFtWhere[0], gVitaFtWhere[1], gVitaFtWhere[2], gVitaFtWhere[3]);
                                              logf_("vita:   kept parts in another state, by field: %u x %u, %u x %u, %u x %u | %u groups found on the chip already | direct draws %u us per drawn list\n",
                                                    bi[0], best[0], bi[1], best[1], bi[2], best[2], gVitaDpShared, (unsigned)(gVitaDpUs / perList));
                                              { extern unsigned gVitaDs[4], gVglL2[2]; extern volatile int gVglL2Drawn; logf_("vita:   fighter strips drawn directly: %u in %u draws (%u pieces); %u went the batches' way | shadow-page strips (program 2): %u drawn, %u dropped%s\n", gVitaDs[0], gVitaDs[1], gVitaDs[2], gVitaDs[3], gVglL2[0], gVglL2[1], gVglL2Drawn ? "" : ", the others not sent"); memset(gVitaDs, 0, sizeof(gVitaDs)); gVglL2[0] = gVglL2[1] = 0; }
                                              memset(gVitaCcWhy, 0, sizeof(gVitaCcWhy)); gVitaCcMax = 0; memset(gVitaFtWhere, 0, sizeof(gVitaFtWhere)); memset(gVitaDpMiss, 0, sizeof(gVitaDpMiss)); gVitaDpShared = 0; gVitaDpUs = 0; }
                                            { extern unsigned gVitaFtMiss[4]; logf_("vita:   fighter strips that started a batch: other state %u, other screen matrix %u, place taken %u, full %u\n", gVitaFtMiss[0], gVitaFtMiss[1], gVitaFtMiss[2], gVitaFtMiss[3]); memset(gVitaFtMiss, 0, sizeof(gVitaFtMiss)); }
                                        }
                                    }
                                    {
                                        extern uint64_t vgl_dbg_time[8];
                                        logf_("vita:   inside vitaGL's glDrawElements (ms): shader setup %u (of it: fragment program %u, uniforms %u in %u cached-layout draws), indices %u, sceGxmDraw %u, in %u calls\n",
                                              (unsigned)(vgl_dbg_time[0] / 1000), (unsigned)(vgl_dbg_time[4] / 1000), (unsigned)(vgl_dbg_time[5] / 1000), (unsigned)vgl_dbg_time[6],
                                              (unsigned)(vgl_dbg_time[1] / 1000), (unsigned)(vgl_dbg_time[2] / 1000), (unsigned)vgl_dbg_time[3]);
                                        memset(vgl_dbg_time, 0, sizeof(vgl_dbg_time));
                                    }
                                    memset(gVitaWall, 0, sizeof(gVitaWall));
                                }
                                memset(gVitaPerfList, 0, 2 * 5 * sizeof(uint64_t));
                                memset(gVitaPerfWork, 0, 5 * sizeof(uint64_t));
                                gVitaPerfLists[0] = gVitaPerfLists[1] = 0;
                            }
                        }
                        memset(gProfHist, 0, sizeof(gProfHist));
                        memset(gProfGameHist, 0, sizeof(gProfGameHist));
                        gVitaUnpackSkipped = 0;
                        gVitaWaitSkips = 0;
                        gVitaSnap[0] = gVitaSnap[1] = 0;
                    }
                    memset(gVitaList, 0, sizeof(gVitaList));
                    gVitaUnpack[0] = gVitaUnpack[1] = 0;
                }
                if (gBtlSeq != NULL && gBtlSeq->state >= 2) {
                    extern unsigned gVglKeys;
                    logf_("vita:   fight: sequence %d, battle clock %d ticks, health %d / %d; %u texture keys made per picture\n", gBtlSeq->state,
                          BtlSeq_GetClock()[0], BtlCharApi_GetHp(0), BtlCharApi_GetHp(1), gVglKeys / perPic);
                    gVglKeys = 0;
                }
                if (gVglVuBatches != 0) {
                    logf_("vita:   vertex programs without the interpreter: %u triangles in %u strips per picture\n", gVglVuTris / perPic, gVglVuBatches / perPic);
                    gVglVuTris = gVglVuBatches = 0;
                }
                if (Vita_MovieStats(film, &filmUs)) {
                    logf_("vita:   film: %u pictures on screen in this time (%u film blocks/blank not uploaded); since its start %u shown, %u decoded too late,"
                          " %u left out, %u asked for early; %u ms per picture; %u us/blank on the game's thread\n",
                          gVglFilm, gVitaUp[4] / vb, film[0], film[1], film[2], film[3], filmUs / 1000, gVitaUsMovie / vb);
                }
                gVitaUsGs = gVitaUsMovie = gVitaUsUpload = 0;
                memset(gVitaUp, 0, sizeof(gVitaUp));
                gVglFilm = 0;
                sLateSum = 0;
            }
            gVglDraws = gVglDropped = gVglBatches = gVglNewTex = 0;
            t0 = t; vb0 = vblanks; drawn0 = gVitaGsDrawn;
        }
    }
    if ((buttons() & (SCE_CTRL_START | SCE_CTRL_SELECT | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) == (SCE_CTRL_START | SCE_CTRL_SELECT)) {
        static int held;
        if (++held >= 3) { /* a second and a half: both are buttons of the game too */
            exit(9);
        }
    }
    sLastDraw = t;
    sLastVb = vblanks;
}

/* exit(), from anywhere in the port or the game: keep the screen until it has been read. */
void __wrap_exit(int code) {
    if (!sEnded) {
        sEnded = 1;
        fflush(stdout);
        fflush(stderr);
        Vita_TextScreen();
        psvDebugScreenPrintf("\n\e[32mThe program ended with code %d%s.\e[39m\n", code,
                             code == 0 ? " (the fight ran to its end)" : code == 9 ? " (stopped by you)" : "");
        logf_("vita: exit(%d) at vertical blank %u\n", code, gPortVBlanks);
        if (gBtlSeq != NULL && gBtlSeq->state >= 2) {
            psvDebugScreenPrintf("last state: sequence %d, battle clock %d ticks, health %d / %d\n", gBtlSeq->state, BtlSeq_GetClock()[0],
                                 BtlCharApi_GetHp(0), BtlCharApi_GetHp(1));
            logf_("vita: last state: sequence %d, battle clock %d ticks, health %d / %d\n", gBtlSeq->state, BtlSeq_GetClock()[0],
                  BtlCharApi_GetHp(0), BtlCharApi_GetHp(1));
        }
        summary(1);
        psvDebugScreenPrintf("\nThe log is " HOME "/log" SFX ". Press START to quit.\n");
        log_sync();
        while (__sync_lock_test_and_set(&sLogDrainLock, 1)) {
            sceKernelDelayThread(100);
        }
        sLogThreaded = 0;
        if (sLog != NULL) { /* closed now, so that the files can be fetched while this screen is still up */
            fclose(sLog);
            sLog = NULL;
        }
        __sync_lock_release(&sLogDrainLock);
        fclose(stdout);
        fclose(stderr);
        wait_buttons(SCE_CTRL_START);
    }
    quit();
    for (;;) {
    }
}

/* bt3.txt: name=value lines; '#' starts a comment. */
static void read_settings(void) {
    FILE *fp = fopen(HOME "/bt3.txt", "r");
    char line[400];

    if (fp == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *eq = strchr(line, '='), *end;

        if (line[0] == '#' || eq == NULL) {
            continue;
        }
        *eq++ = '\0';
        end = eq + strlen(eq);
        while (end > eq && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) {
            *--end = '\0';
        }
#ifdef PORT_VITA_RELEASE
        /* the released program: only the player's settings. The test runs leave theirs in the same file
           (mode=demo, film=...): a game started from its bubble goes to its title screen, not into a test fight */
        if (strcmp(line, "disc") != 0 && strcmp(line, "heap") != 0 && strcmp(line, "wide") != 0 && strcmp(line, "report") != 0) {
            continue;
        }
#endif
        if (strcmp(line, "disc") == 0) {
            snprintf(gVitaDisc, sizeof(gVitaDisc), "%s", eq);
        } else if (strcmp(line, "float") == 0) {
            sFloatChoice = strcmp(eq, "fast") == 0;
        } else if (strcmp(line, "heap") == 0) {
            sHeapCdram = strcmp(eq, "cdram") == 0;
        } else if (strcmp(line, "skip") == 0) {
            gVitaGsFixedSkip = atoi(eq);
        } else if (strcmp(line, "mode") == 0) {
            sMode = strcmp(eq, "demo") == 0;
        } else if (strcmp(line, "sweep") == 0) {
            sSweepSec = atoi(eq);
        } else if (strcmp(line, "wide") == 0) {
            extern int gVitaWide;
            gVitaWide = atoi(eq) != 0; /* wide=0: the console's 4:3 picture with bars at the sides */
        } else if (strcmp(line, "msaa") == 0) { /* the chip's multisampling: 0, 2 or 4 */
            extern int gVitaMsaa;
            gVitaMsaa = atoi(eq);
        } else if (strcmp(line, "menuhi") == 0) { /* 0: the menus at 640 x 368 as the fights, as before */
            extern int gVitaMenuHi;
            gVitaMenuHi = atoi(eq) != 0;
        } else if (strcmp(line, "fadepass") == 0) { /* VU1 program 1, the close-up shine: 0 not drawn, 2 its shine layer twice */
            extern int gPortFadePass;
            gPortFadePass = atoi(eq);
        } else if (strcmp(line, "perf") == 0) { /* 0: no processor counters (libperf), as the released program: its speed */
            sPerfOff = atoi(eq) == 0;
        } else if (strcmp(line, "fl6") == 0) { /* 0: sprites never take the state of the sprite before (as an earlier version) */
            extern int gVitaFl6;
            gVitaFl6 = atoi(eq) != 0;
        } else if (strcmp(line, "runest") == 0) { /* 0: a run's first triangle the full way when its state is not known */
            extern int gVitaRunEst;
            gVitaRunEst = atoi(eq) != 0;
        } else if (strcmp(line, "wtc") == 0) { /* 0: the work targets' reads not remembered */
            extern int gVitaWtCOn;
            gVitaWtCOn = atoi(eq) != 0;
        } else if (strcmp(line, "stx") == 0) { /* 0: no states kept from one picture to the next */
            extern int gVitaStXOn;
            gVitaStXOn = atoi(eq) != 0;
        } else if (strcmp(line, "stlast") == 0) { /* 0: no state of the call before (as an earlier version) */
            extern int gVitaStLastOn;
            gVitaStLastOn = atoi(eq) != 0;
        } else if (strcmp(line, "shadelog") == 0) { /* 1: the fighters' shading layers in the log (an earlier version's) */
            extern int gVitaShadeLog;
            gVitaShadeLog = atoi(eq) != 0;
        } else if (strcmp(line, "trirun") == 0) { /* 0: the effects' triangle packets register by register (gs_core.c tri_run) */
            extern int gVitaTriRun;
            gVitaTriRun = atoi(eq) != 0;
        } else if (strcmp(line, "searun") == 0) { /* 0: the ordering table sent in its own order (src/sys/gfx_ot.c Ot_Send) */
            extern int gVitaSeaRun;
            gVitaSeaRun = atoi(eq) != 0;
        } else if (strcmp(line, "seaoff") == 0) { /* 1: the stage's surfaces not drawn (what they cost) */
            gVitaSeaOff = atoi(eq) != 0;
        } else if (strcmp(line, "fastlane") == 0) { /* 0: every GS triangle the full way (vita_vgl.c) */
            extern int gVitaFastLane;
            gVitaFastLane = atoi(eq) != 0;
        } else if (strcmp(line, "sprinset") == 0) { /* 0: filtered sprites' coordinates as given (the lines under the HUD) */
            extern int gVitaSprInset;
            gVitaSprInset = atoi(eq) != 0;
        } else if (strcmp(line, "warm") == 0) { /* blanks from the stage's first picture in which the game waits for the pictures (0: off) */
            gVitaWarmVb = atoi(eq);
        } else if (strcmp(line, "gpuprof") == 0) { /* the GPU profile in a window of the fight (lists gpuproffrom..gpuprofto) */
            extern int gVitaGpuProf;
            gVitaGpuProf = atoi(eq);
        } else if (strcmp(line, "gpuproffrom") == 0) {
            extern int gVitaGpuProfFrom;
            gVitaGpuProfFrom = atoi(eq);
        } else if (strcmp(line, "gpuprofto") == 0) {
            extern int gVitaGpuProfTo;
            gVitaGpuProfTo = atoi(eq);
        } else if (strcmp(line, "surfcull") == 0) { /* 0: every water-surface triangle prepared and clipped, as before */
            extern int gVitaSurfCull;
            gVitaSurfCull = atoi(eq);
        } else if (strcmp(line, "snapsame") == 0) { /* 0: every image the lists refer to copied at the handover, as earlier versions */
            extern int gVitaSnapSame;
            gVitaSnapSame = atoi(eq);
        } else if (strcmp(line, "qlinopt") == 0) { /* 0: every perspective GS triangle's colour divided in full precision, as before */
            extern int gVitaQlinOpt;
            gVitaQlinOpt = atoi(eq);
        } else if (strcmp(line, "zlead") == 0) { /* depth steps a GS primitive leads by (0: none, as before) */
            extern int gVitaZLead;
            gVitaZLead = atoi(eq);
        } else if (strcmp(line, "introwait") == 0) { /* 0: only the first blanks of the intro wait (warm=) */
            gVitaIntroWait = atoi(eq);
        } else if (strcmp(line, "planes") == 0) { /* 0: an upload into part of the 32-bit pixels compared by all of them, as before */
            extern int gVitaPlanes;
            gVitaPlanes = atoi(eq);
        } else if (strcmp(line, "order") == 0) { /* 0: the kept parts' draws of one state put together across others, as before */
            extern int gVitaOrder;
            gVitaOrder = atoi(eq);
        } else if (strcmp(line, "upquick") == 0) { /* 0: every large upload's number from all of its bytes */
            extern int gVitaUpQuick;
            gVitaUpQuick = atoi(eq);
        } else if (strcmp(line, "upcheck") == 0) { /* 1: uploads' numbers checked against all of their bytes (the log) */
            extern int gVitaUpCheck;
            gVitaUpCheck = atoi(eq);
        } else if (strcmp(line, "filmlist") == 0) { /* the film in lists a..b: filmlist=a-b (and c..d after a comma) */
            char *dash = strchr(eq, '-'), *comma = strchr(eq, ',');
            gVitaFilmFrom = atoi(eq);
            gVitaFilmTo = dash != NULL ? atoi(dash + 1) : gVitaFilmFrom;
            if (comma != NULL) {
                char *dash2 = strchr(comma + 1, '-');
                gVitaFilmFrom2 = atoi(comma + 1);
                gVitaFilmTo2 = dash2 != NULL ? atoi(dash2 + 1) : gVitaFilmFrom2;
            }
        } else if (strcmp(line, "filmwait") == 0) { /* 1: the game waits for the pictures around the filmed lists */
            gVitaFilmWait = atoi(eq);
        } else if (strcmp(line, "filmtrig") == 0) { /* the film in bursts of n pictures from each skill's name */
            gVitaFilmTrig = atoi(eq);
        } else if (strcmp(line, "logsplit") == 0) { /* the log in parts of n KB (log-part2-vgl.txt...): a part a fetch can take */
            sLogSplit = (unsigned)atoi(eq) * 1024u;
        } else if (strcmp(line, "filmmax") == 0) {
            gVitaFilmMax = atoi(eq);
        } else if (strcmp(line, "filmstate") == 0) { /* the film in that state of the fight (0: the intros) */
            gVitaFilmState = atoi(eq);
        } else if (strcmp(line, "film") == 0) { /* a small picture for the log every <n> pictures of a fight (test runs) */
            extern int gVitaFilm;
            gVitaFilm = atoi(eq);
        } else if (strcmp(line, "shadows") == 0) { /* the ground shadows' silhouettes: 1 every picture (as the console), 2 each fighter's every other picture */
            extern int gVitaSilMode;
            gVitaSilMode = atoi(eq);
        } else if (strcmp(line, "pbuf") == 0) { /* the graphics chip's parameter buffer, MB (vitaGL's default 16) */
            extern int gVitaPbufMb;
            gVitaPbufMb = atoi(eq);
        } else if (strcmp(line, "safe") == 0) { /* vita_vgl.c's gVglSafe, for a test run's A/B (268435456: textures with a palette converted to colours) */
            extern int gVglSafe;
            gVglSafe = (int)strtol(eq, NULL, 0);
        } else if (strcmp(line, "safe2") == 0) { /* vita_vgl.c's gVglSafe2: 1 a picture handed to the chip whole, 2 no render thread, 8 the render thread on the fourth core */
            extern int gVglSafe2;
            gVglSafe2 = (int)strtol(eq, NULL, 0);
        } else if (strcmp(line, "fxsweep") == 0) { /* the effects left out in turn: their cost in the log */
            extern int gVitaFxSweep;
            gVitaFxSweep = atoi(eq);
        } else if (strcmp(line, "wtshot") == 0) { /* with each film picture, the effects' work targets and the picture's alpha */
            extern int gVitaWtShot;
            gVitaWtShot = atoi(eq);
        } else if (strcmp(line, "sliver") == 0) { /* fighters' triangles far off the screen, into the log */
            extern int gVitaSliver;
            gVitaSliver = atoi(eq);
        } else if (strcmp(line, "report") == 0) {
            gVitaReport = atoi(eq) != 0;
        } else if (strcmp(line, "touchlr") == 0) { /* L2 / R2: rear (the rear pad), front (the screen's bottom corners), off */
            extern int gVitaTouchLR;
            gVitaTouchLR = strcmp(eq, "rear") == 0 ? 0 : strcmp(eq, "front") == 0 ? 1 : strcmp(eq, "off") == 0 ? 2 : -1;
        } else if (strcmp(line, "touchlog") == 0) { /* what the touches do, into the log */
            extern int gVitaTouchLog;
            gVitaTouchLog = atoi(eq);
        } else if (strcmp(line, "dump") == 0) { /* a copy of each file the game loads whole: dump/<id>.bin */
            extern int gVitaDump;
            gVitaDump = atoi(eq);
        } else if (strcmp(line, "demostage") == 0 || strcmp(line, "demochara1") == 0 || strcmp(line, "demochara2") == 0 ||
                   strcmp(line, "demoduel") == 0) { /* the test fight: stage, fighters, a duel with intros */
            char env[32];
            snprintf(env, sizeof(env), "BT3_DEMO_%s", line + 4);
            for (char *c = env; *c; c++) {
                *c = (char)toupper((unsigned char)*c);
            }
            setenv(env, eq, 1);
        } else if (strcmp(line, "startmode") == 0) { /* the menus start on this screen (62 the options) */
            extern int gVitaStartMode;
            gVitaStartMode = atoi(eq);
        } else if (strcmp(line, "replay") == 0) {
            setenv("BT3_REPLAY", eq, 1);
        } else if (strncmp(line, "BT3_", 4) == 0) {
            setenv(line, eq, 1);
        }
        logf_("vita: setting %s=%s\n", line, eq);
    }
    fclose(fp);
}

static void start_screen(void) {
#ifdef PORT_VITA_RELEASE
    /* The released program: no start screen. The game from its start (logos, title, menus). */
    sFloatChoice = 1;
    if (sMode < 0) {
        sMode = 0;
    }
#else
    unsigned ns[2][7], diff[7];
    const char *names[7];
    int bad, i;

    psvDebugScreenPrintf("\e[33mTenkaichi3Decomp, PS Vita development build\e[39m (" __DATE__ ")\n\n");
    /* The vector library's arithmetic (vita_float.c): the exact model's sums against the PC's, and the hardware
       model against the exact one. The game's own float code uses the processor's float unit directly. */
    psvDebugScreenPrintf("Float self-test, please wait...\n");
    bad = Vita_FloatSelfTest(now_us, ns, diff, names);
    psvDebugScreenPrintf("\n  operation         exact model   hardware model   results that differ\n");
    logf_("vita: float self-test: %d of 7 exact sums differ from the PC's\n", bad);
    for (i = 0; i < 7; i++) {
        psvDebugScreenPrintf("  %-16s %7u ns     %7u ns        %u per 1000\n", names[i], ns[0][i], ns[1][i], diff[i]);
        logf_("vita:   %-16s exact %u ns, hardware %u ns, %u per 1000 differ\n", names[i], ns[0][i], ns[1][i], diff[i]);
    }
    psvDebugScreenPrintf(bad == 0 ? "\n\e[32mExact model: the same results as on the PC.\e[39m\n"
                                  : "\n\e[31mExact model: some sums differ from the PC's.\e[39m\n");
    sFloatChoice = 1;
#endif
    if (sMode < 0) {
        psvDebugScreenPrintf("\nCROSS: the game from its start\nCIRCLE: the demo fight (computer against computer)\nSTART: quit\n\n"
                             "Hold START + SELECT for two seconds to stop. The log is " HOME "/log" SFX ".\n");
        switch (wait_buttons(SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_START)) {
            case SCE_CTRL_CROSS: sMode = 0; break;
            case SCE_CTRL_CIRCLE: sMode = 1; break;
            default: quit();
        }
    }
    if (sMode == 1) {
        setenv("BT3_DEMO", "1", 1);
        if (getenv("BT3_DEMO_PICK") == NULL && kDemoPick >= 0) { /* (one pairing, the heavy one, while it is worked on) */
            char b[8];
            snprintf(b, sizeof(b), "%d", kDemoPick);
            setenv("BT3_DEMO_PICK", b, 1);
        }
        if (getenv("BT3_DEMO_PICK") == NULL) { /* the demo's nine pairings in turn, one per run */
            FILE *fp = fopen(HOME "/demo_next.txt", "r");
            int n = 0;
            char b[8];
            if (fp != NULL) {
                if (fscanf(fp, "%d", &n) != 1) {
                    n = 0;
                }
                fclose(fp);
            }
            if (n < 0 || n > 8) {
                n = 0;
            }
            snprintf(b, sizeof(b), "%d", n);
            setenv("BT3_DEMO_PICK", b, 1);
            fp = fopen(HOME "/demo_next.txt", "w");
            if (fp != NULL) {
                fprintf(fp, "%d\n", (n + 1) % 9);
                fclose(fp);
            }
        }
        logf_("vita: demo pairing %s of 0..8 (one per run, in turn: %s/demo_next.txt)\n", getenv("BT3_DEMO_PICK"), HOME);
    }
    gPortFastFloat = sFloatChoice;
    psvDebugScreenPrintf("\e[2J\e[H");
}

/* The fourth core is the system's; an application's threads may run there only with the CapUnlocker
   kernel plugin (taiHEN, *KERNEL). A helper thread is made there when the system lets it, else on `fallbackMask`. */
int gVitaCore3 = -1; /* 1: the fourth core takes our threads, 0: it does not, -1: not tried yet */
SceUID Vita_ThreadOnCore3(const char *name, SceKernelThreadEntry entry, int prio, int stack, int fallbackMask) {
    SceUID th = -1;
    if (gVitaCore3 != 0) {
        th = sceKernelCreateThread(name, entry, prio, stack, 0, 0x00080000, NULL);
        if (gVitaCore3 < 0) {
            gVitaCore3 = th >= 0;
            logf_("vita: the fourth core %s (%08x)\n", th >= 0 ? "takes our helper threads (the CapUnlocker plugin is in)" :
                  "is the system's alone (no CapUnlocker plugin): the helper threads share the first three", th >= 0 ? 0u : (unsigned)th);
        }
    }
    if (th < 0) {
        th = sceKernelCreateThread(name, entry, prio, stack, 0, fallbackMask, NULL);
    }
    return th;
}

/* The threads that matter, as the system sees them (for the log): processor time, where they ran, how often
   something else took their core. */
static struct { int id; const char *name; unsigned long long run; unsigned pre, ipre; int chg; } sWatch[8];
void Vita_WatchThread(int slot, int id, const char *name) {
    if (slot >= 0 && slot < 8) {
        sWatch[slot].id = id;
        sWatch[slot].name = name;
    }
}
static void watch_log(void) {
    char line[1300];
    int n = 0, i;
    for (i = 0; i < 8; i++) {
        SceKernelThreadInfo ti;
        if (sWatch[i].id <= 0) {
            continue;
        }
        memset(&ti, 0, sizeof(ti));
        ti.size = sizeof(ti);
        if (sceKernelGetThreadInfo(sWatch[i].id, &ti) < 0) {
            continue;
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%s%s %u ms on core %d (mask %x, priority %d), %u times put aside, %u by interrupts, %d moves", n ? "; " : "",
                      sWatch[i].name, (unsigned)((ti.runClocks - sWatch[i].run) / 1000), (int)ti.lastExecutedCpuId, (unsigned)ti.currentCpuAffinityMask, (int)ti.currentPriority,
                      ti.threadPreemptCount - sWatch[i].pre, ti.intrPreemptCount - sWatch[i].ipre, (int)ti.changeCpuCount - sWatch[i].chg);
        sWatch[i].run = ti.runClocks;
        sWatch[i].pre = ti.threadPreemptCount;
        sWatch[i].ipre = ti.intrPreemptCount;
        sWatch[i].chg = (int)ti.changeCpuCount;
        if (n > (int)sizeof(line) - 200) {
            break;
        }
    }
    if (n > 0) {
        logf_("vita:   threads: %s\n", line);
    }
}

/* The watchdog: when the drawing thread has made no picture for three seconds, where every thread is
   (its state, what it waits on) and where the drawing and render threads are in their work, written to a file of its
   own straight away (not through the log thread, which may be what is stuck) and to the log. */
static int watchdog_thread(SceSize args, void *argp) {
    extern unsigned gGsFrame;
    extern void Vita_VglState(char *b, int n);
    unsigned last = 0, still = 0, dumps = 0;
    (void)args;
    (void)argp;
    for (;;) {
        sceKernelDelayThread(500000);
        if (gGsFrame != last) {
            last = gGsFrame;
            still = 0;
            continue;
        }
        if (last == 0 || !sLogThreaded || ++still % 6 != 0 || dumps >= 4) { /* (not before the first picture, nor at the end) */
            continue;
        }
        dumps++;
        {
            static char out[3000];
            int n = 0, i;
            SceUID fd;
            n += snprintf(out + n, sizeof(out) - (size_t)n, "vita: WATCHDOG: no picture for %u s (picture %u); log %u written, %u kept, %u lost\n", still / 2, gGsFrame,
                          sLogTail, sLogHead, sLogLost);
            Vita_VglState(out + n, (int)sizeof(out) - n);
            n += (int)strlen(out + n);
            for (i = 0; i < 8 && n < (int)sizeof(out) - 300; i++) {
                SceKernelThreadInfo ti;
                if (sWatch[i].id <= 0) {
                    continue;
                }
                memset(&ti, 0, sizeof(ti));
                ti.size = sizeof(ti);
                if (sceKernelGetThreadInfo(sWatch[i].id, &ti) < 0) {
                    continue;
                }
                n += snprintf(out + n, sizeof(out) - (size_t)n, "vita: WATCHDOG:   %s: status %x, priority %d, core %d (last %d), waits for %x (object %x), run %u ms\n", sWatch[i].name,
                              (unsigned)ti.status, (int)ti.currentPriority, (int)ti.currentCpuId, (int)ti.lastExecutedCpuId, (unsigned)ti.waitType, (unsigned)ti.waitId,
                              (unsigned)(ti.runClocks / 1000));
            }
            fd = sceIoOpen(HOME "/hang" SFX, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
            if (fd >= 0) {
                sceIoWrite(fd, out, (SceSize)n);
                sceIoClose(fd);
            }
            logf_("%s", out);
        }
    }
    return 0;
}

static int game_thread(SceSize args, void *argp) {
    static char *argv[] = {"bt3", NULL};

    (void)args;
    (void)argp;
    Vita_FloatMode(gPortFastFloat); /* the float unit's mode belongs to the thread */
    {
        extern int gVitaGameThreadId;
        gVitaGameThreadId = sceKernelGetThreadId();
    }
    {
        extern void Vita_PerfThreadStart(void);
        Vita_PerfThreadStart();
    }
    exit(__real_main(1, argv));
    return 0;
}

int __wrap_main(int argc, char **argv) {
    SceUID th;

    (void)argc;
    (void)argv;
    scePowerSetArmClockFrequency(444); /* the console's own top speed: no overclocking */
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE); /* a DualShock's sticks through the Ext2 reads (PS TV, a second pad) */
    psvDebugScreenInit();
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(HOME, 0777);
    sLog = fopen(HOME "/log" SFX, "a");
    logf_("\n======== new run ========\n");
    {   /* the parts of a log split before (logsplit=): this run's own, if any, are made anew */
        int k;
        for (k = 2; k <= 9; k++) {
            char path[64];
            snprintf(path, sizeof(path), HOME "/log-part%d" SFX, k);
            sceIoRemove(path);
        }
    }
#ifdef PORT_VITA_RELEASE
    gVitaReport = 0;
    logf_("vita: Dragon Ball Z Budokai Tenkaichi 3 (Tenkaichi3Decomp) for the PS Vita, built " __DATE__ " " __TIME__ ", processor clock %d MHz\n", scePowerGetArmClockFrequency());
#else
    logf_("vita: Tenkaichi3Decomp development build of " __DATE__ " " __TIME__ ", processor clock %d MHz\n", scePowerGetArmClockFrequency());
#endif
    logf_("vita: clocks as set: processor %d MHz, bus %d MHz, graphics chip %d MHz, its crossbar %d MHz\n", scePowerGetArmClockFrequency(),
          scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
    {
        SceUID lt = Vita_ThreadOnCore3("bt3_log", log_thread, 180, 0x4000, 0x00020000 /* else the second core */);
        if (lt >= 0 && sceKernelStartThread(lt, 0, NULL) >= 0) {
            sLogThreaded = 1;
            Vita_WatchThread(6, lt, "log");
        }
    }
    {   /* the watchdog: any core, above everything of ours */
        SceUID wd = sceKernelCreateThread("bt3_watchdog", watchdog_thread, 64, 0x4000, 0, 0, NULL);
        if (wd >= 0) {
            sceKernelStartThread(wd, 0, NULL);
        }
    }
    if (freopen(HOME "/out" SFX, "w", stdout) != NULL) { /* what the port's own code prints */
        setvbuf(stdout, NULL, _IOLBF, 0);
    }
    if (freopen(HOME "/err" SFX, "w", stderr) != NULL) {
        /* a line at a time, not a write per piece of each line: the memory card's writes are slow */
        setvbuf(stderr, NULL, _IOLBF, 1024);
    }
    setenv("BT3_DATA", HOME, 1);
    setenv("BT3_SAVES", HOME "/saves", 1);
    setenv("BT3_SETTINGS", HOME "/bt3_settings.txt", 1);
#ifndef PORT_VITA_RELEASE
    setenv("BT3_TRACE", "600", 1); /* the fight's state every 600 blanks into out.txt: comparable with a PC run */
#endif
    read_settings();
    start_screen();
#ifndef PORT_VITA_RELEASE /* (the released program shows nothing until the game's first picture; an error still shows) */
    psvDebugScreenPrintf("Reading the game's data tables from %s ...\n", gVitaDisc);
#endif
    Vita_LoadGameData(gVitaDisc);
    Vita_MapMemory(sHeapCdram);
    logf_("vita: %s floats, game heap at 0x%08X, disc folder %s, %s\n", gPortFastFloat ? "hardware" : "exact", (unsigned)gVitaHeapBase, gVitaDisc,
          getenv("BT3_REPLAY") != NULL ? getenv("BT3_REPLAY") : "attract-demo fight");
    sStart = now_us();
    {
        extern int Vita_PerfInit(void);
#ifdef PORT_VITA_RELEASE
        int r = gVitaReport ? Vita_PerfInit() : 0;
#else
        int r = sPerfOff ? 0 : Vita_PerfInit();
#endif
        logf_(r == 1 ? "vita: processor counters on (libperf)\n" : "vita: processor counters off (libperf kernel plugin not loaded: %08x)\n", r);
    }
    th = sceKernelCreateThread("bt3_game", game_thread, 0x10000100, GAME_STACK, 0, 0x00010000 /* the first core, to itself */, NULL);
    Vita_WatchThread(0, th, "game");
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        Vita_Fatal("cannot start the game's thread (error 0x%08X)", (unsigned)th);
    }
    sceKernelWaitThreadEnd(th, NULL, NULL);
    for (;;) { /* the game's thread ends the program itself (exit); nothing to do here */
        sceKernelDelayThread(1000000);
    }
    return 0;
}
