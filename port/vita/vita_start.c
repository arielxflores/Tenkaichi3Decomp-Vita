/*
 * The start menu, on the text screen before the game's first picture: SELECT held while the game starts.
 *
 *   Language        Spanish, German, French or Italian from the player's European disc (vita_eu.c; only when its
 *                   archive is there). At the first start that finds the archive the list shows by itself.
 *   Unlock all      a cheat: the game's own unlock (Save_UnlockAll, a routine the game has but never calls): every
 *                   character, stage, song and item, and 4,850,000 Zenni, given to the save the game loads now. The
 *                   game keeps it when it saves next.
 *   Start           (CIRCLE or START too)
 */
#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>

#include "vita_eu.h"

extern void Vita_Log(const char *fmt, ...);
extern int psvDebugScreenPrintf(const char *fmt, ...);

int gVitaCheatUnlock; /* the save the game loads next is given everything (src/menu/menu_i_b.c BootCard_OnFlowDone) */

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

void Vita_StartMenu(void) {
    enum { IT_LANG, IT_UNLOCK, IT_START };
    int items[3], n = 0, sel = 0, i;
    unsigned p;

    if (VitaEu_NeedsChoice()) {
        VitaEu_ChooseLanguage();
    }
    if (!(pad_now() & SCE_CTRL_SELECT)) {
        return;
    }
    Vita_Log("vita: start menu (SELECT held)\n");
    if (VitaEu_Available()) {
        items[n++] = IT_LANG;
    }
    items[n++] = IT_UNLOCK;
    items[n++] = IT_START;
    for (;;) {
        psvDebugScreenPrintf("\e[H\e[2J\n  Budokai Tenkaichi 3: start menu\n\n");
        for (i = 0; i < n; i++) {
            psvDebugScreenPrintf("  %s ", i == sel ? "\e[33m>" : " ");
            if (items[i] == IT_LANG) {
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
                                 "  Zenni, given to the save the game loads now. It stays\n"
                                 "  once the game saves (it cannot be taken back).\n");
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
    while (pad_now() & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_START)) { /* (not a press in the game) */
        sceKernelDelayThread(16000);
    }
}
