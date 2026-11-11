/*
 * The start menu, on the text screen before the game's first picture: SELECT held while the game starts.
 *
 *   Game            Budokai Tenkaichi 3, or Team BT4's Budokai Tenkaichi 4 B14 Rev2 (BT4decomp's game code, built
 *                   for the Vita as app0:bt4.bin: build.py BT3_BT4) when the package carries it and the player's BT4
 *                   files are in ux0:data/bt3/bt4/. The choice is kept (ux0:data/bt3/start.txt): BT4 then starts
 *                   straight away each time, until it is changed here again.
 *   Language        Spanish, German, French or Italian from the player's European disc (vita_eu.c; only when its
 *                   archive is there). At the first start that finds the archive the list shows by itself.
 *   Unlock all      a cheat: the game's own unlock (Save_UnlockAll, a routine the game has but never calls): every
 *                   character, stage, song and item, and 4,850,000 Zenni, given to the save the game loads now. The
 *                   game keeps it when it saves next.
 *   Start           (CIRCLE or START too)
 *
 * BT4 that does not get going (it crashed or froze before its first pictures: ux0:data/bt3/bt4/starting.txt is
 * still there at the next start) is turned off with a message, so the package cannot get stuck starting it.
 */
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>

#include "vita_eu.h"
#include "vita_home.h"

extern void Vita_Log(const char *fmt, ...);
extern int psvDebugScreenPrintf(const char *fmt, ...);
extern int sceAppMgrLoadExec(const char *appPath, char *const argv[], void *optParam);

#define START_FILE "ux0:data/bt3/start.txt"         /* "bt4": the package starts BT4 */
#define BT4_STARTING "ux0:data/bt3/bt4/starting.txt" /* BT4 was started and has not drawn yet */
#define BT4_PROGRAM "app0:bt4.bin"

int gVitaCheatUnlock;     /* the save the game loads next is given everything (src/menu/menu_i_b.c BootCard_OnFlowDone) */
int gVitaStartMenuForce;  /* (development build, startmenu=1 in bt3.txt) the menu as if SELECT were held: the test runs' */

static unsigned pad_now(void) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    return pad.buttons;
}

/* waits for a new press of one of `mask`; returns the buttons newly pressed */
static unsigned press(unsigned mask) {
    unsigned prev = pad_now(), now;
    for (;;) {
        sceKernelDelayThread(16000);
        now = pad_now();
        if ((now & ~prev) & mask) {
            return now & ~prev;
        }
        prev = now;
    }
}

static int select_held(void) {
    return (pad_now() & SCE_CTRL_SELECT) != 0 || gVitaStartMenuForce;
}

static int exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static void write_text(const char *path, const char *text) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, text, strlen(text));
        sceIoClose(fd);
    }
}

static int start_bt4_chosen(void) {
    char b[8] = {0};
    SceUID fd = sceIoOpen(START_FILE, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    return strncmp(b, "bt4", 3) == 0;
}

#ifndef BT3_BT4
/* BT4 was started and has not drawn yet: "1" in starting.txt (a "0" there, as the test runs leave it, is not) */
static int bt4_was_starting(void) {
    char b[4] = {0};
    SceUID fd = sceIoOpen(BT4_STARTING, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    return b[0] == '1';
}
#endif

static void release_buttons(void) {
    while (pad_now() & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_START | SCE_CTRL_SELECT)) { /* (not a press in the game) */
        sceKernelDelayThread(16000);
    }
}

#ifndef BT3_BT4
/* BT4 in the package and its files in place: 1; else 0, and `why` says what is missing */
static int bt4_ready(const char **why) {
    if (!exists(BT4_PROGRAM)) {
        *why = "this package has no BT4 program";
        return 0;
    }
    if (!(exists("ux0:data/bt3/bt4/SLUS_219.78") && exists("ux0:data/bt3/bt4/BIN/DBZ4.BIN"))) {
        extern int Vita_IsoFind(const char *dir, const char *file, int keep, char *out, int size, int *seen);
        char iso[300];
        if (!Vita_IsoFind("ux0:data/bt3/bt4", "BIN/DBZ4.BIN", 0, iso, sizeof(iso), NULL)) { /* (or its .iso: vita_iso.c) */
            *why = "copy your BT4 B14 Rev2 disc's .iso (or its SLUS_219.78,\n  BIN and DATA) to ux0:data/bt3/bt4/";
            return 0;
        }
    }
    *why = NULL;
    return 1;
}

static void start_bt4(void) {
    int r;
    write_text(BT4_STARTING, "1");
    Vita_Log("vita: starting Budokai Tenkaichi 4 (%s)\n", BT4_PROGRAM);
    psvDebugScreenPrintf("\e[H\e[2J\n  Starting Budokai Tenkaichi 4 ...\n");
    r = sceAppMgrLoadExec(BT4_PROGRAM, NULL, NULL);
    sceIoRemove(BT4_STARTING);
    Vita_Log("vita: BT4 could not be started (%08x)\n", (unsigned)r);
    psvDebugScreenPrintf("\n  \e[31mBT4 could not be started (%08x).\e[39m Press CROSS.\n", (unsigned)r);
    press(SCE_CTRL_CROSS);
    psvDebugScreenPrintf("\e[H\e[2J");
}
#else
/* (BT4's program) its first pictures are up: it got going (the BT3 program's guard, above) */
void Vita_Bt4Started(void) {
    sceIoRemove(BT4_STARTING);
}
#endif

void Vita_StartMenu(void) {
    enum { IT_GAME, IT_LANG, IT_UNLOCK, IT_START };
    int items[4], n = 0, sel = 0, i;
    unsigned p;
#ifndef BT3_BT4
    const char *why = NULL;
    int bt4 = bt4_ready(&why);

    if (start_bt4_chosen()) {
        if (bt4_was_starting()) { /* (the last start of BT4 did not get going) */
            sceIoRemove(BT4_STARTING);
            write_text(START_FILE, "bt3");
            Vita_Log("vita: BT4 did not get going the last time: Budokai Tenkaichi 3 instead\n");
            psvDebugScreenPrintf("\n  \e[33mBudokai Tenkaichi 4 did not get going the last time\e[39m\n"
                                 "  (see ux0:data/bt3/bt4/log.txt and crash.txt).\n\n"
                                 "  Budokai Tenkaichi 3 starts instead. Hold SELECT while the\n"
                                 "  game starts to choose BT4 again.\n\n  Press CROSS.\n");
            press(SCE_CTRL_CROSS);
            psvDebugScreenPrintf("\e[H\e[2J");
        } else if (!select_held()) {
            if (bt4) {
                start_bt4();
            } else {
                Vita_Log("vita: BT4 was chosen, but %s\n", why);
            }
        }
    }
#endif
    if (VitaEu_NeedsChoice()) {
        VitaEu_ChooseLanguage();
    }
    if (!select_held()) {
        return;
    }
    Vita_Log("vita: start menu (SELECT held)\n");
#ifndef BT3_BT4
    if (exists(BT4_PROGRAM)) {
        items[n++] = IT_GAME;
    }
#else
    items[n++] = IT_GAME;
#endif
    if (VitaEu_Available()) {
        items[n++] = IT_LANG;
    }
    items[n++] = IT_UNLOCK;
    items[n++] = IT_START;
    for (;;) {
        psvDebugScreenPrintf("\e[H\e[2J\n  %s: start menu\n\n", VITA_GAME_NAME);
        for (i = 0; i < n; i++) {
            psvDebugScreenPrintf("  %s ", i == sel ? "\e[33m>" : " ");
            if (items[i] == IT_GAME) {
#ifndef BT3_BT4
                psvDebugScreenPrintf("Game: Budokai Tenkaichi 3 (choose for BT4)");
#else
                psvDebugScreenPrintf("Game: Budokai Tenkaichi 4 (choose for BT3)");
#endif
            } else if (items[i] == IT_LANG) {
                psvDebugScreenPrintf("Language: %s", VitaEu_LangName(VitaEu_Lang()));
            } else if (items[i] == IT_UNLOCK) {
                psvDebugScreenPrintf("Unlock everything (cheat): %s", gVitaCheatUnlock ? "ON" : "OFF");
            } else {
                psvDebugScreenPrintf("Start the game");
            }
            psvDebugScreenPrintf("\e[39m\n");
        }
        if (items[sel] == IT_UNLOCK) {
            psvDebugScreenPrintf("\n  Every character, stage, song and item, and 4,850,000\n"
                                 "  Zenni, given to the save the game loads now (its\n"
                                 "  records list is emptied). It stays once the game saves\n"
                                 "  (it cannot be taken back). Also in the game: Options >\n"
                                 "  Controller > Vita Settings > Cheats.\n");
        } else if (items[sel] == IT_GAME) {
#ifndef BT3_BT4
            psvDebugScreenPrintf("\n  Team BT4's Budokai Tenkaichi 4 B14 Rev2 (NauTaro's\n"
                                 "  BT4decomp), from your own BT4 disc's .iso or files in\n"
                                 "  ux0:data/bt3/bt4/. It then starts each time, until\n"
                                 "  changed here again.%s%s\n", why != NULL ? "\n\n  \e[33mNot now: " : "", why != NULL ? why : "");
            if (why != NULL) {
                psvDebugScreenPrintf("\e[39m");
            }
#else
            psvDebugScreenPrintf("\n  Back to Budokai Tenkaichi 3: it then starts each time,\n"
                                 "  until BT4 is chosen again.\n");
#endif
        }
        psvDebugScreenPrintf("\n  Up / down, CROSS to choose; CIRCLE or START: start.\n");
        p = press(SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_START);
        if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_START)) {
            break;
        }
        if (p & SCE_CTRL_UP) {
            sel = (sel + n - 1) % n;
        } else if (p & SCE_CTRL_DOWN) {
            sel = (sel + 1) % n;
        } else if (items[sel] == IT_GAME) {
#ifndef BT3_BT4
            if (bt4) {
                write_text(START_FILE, "bt4");
                release_buttons();
                start_bt4();
            }
#else
            int r;
            write_text(START_FILE, "bt3");
            Vita_Log("vita: back to Budokai Tenkaichi 3\n");
            release_buttons();
            r = sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
            Vita_Log("vita: BT3 could not be started (%08x)\n", (unsigned)r);
#endif
        } else if (items[sel] == IT_LANG) {
            VitaEu_ChooseLanguage();
        } else if (items[sel] == IT_UNLOCK) {
            gVitaCheatUnlock ^= 1;
        } else {
            break;
        }
    }
    Vita_Log("vita: start menu left (unlock everything %s)\n", gVitaCheatUnlock ? "on" : "off");
    psvDebugScreenPrintf("\e[H\e[2J");
    release_buttons();
}
