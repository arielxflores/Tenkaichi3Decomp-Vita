/*
 * Hold SELECT (~1 s, without L/R) for a small settings screen: European language and widescreen.
 * Same debug-screen style as the first-run language list. Language changes are written to lang.txt
 * and need a restart to reload menu archives; widescreen applies immediately and is saved to bt3.txt.
 */
#include "vita_settings.h"
#include "vita_eu.h"
#include "debugScreen.h"
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define HOME "ux0:data/bt3"
#define HOLD_US (1000 * 1000) /* 1 second */

extern int gVitaWide;
extern void Vita_Log(const char *fmt, ...);

static int sOpen;
static unsigned long long sSelectDownUs;
static int sSelectWasDown;

int Vita_SettingsOpen(void) {
    return sOpen;
}

static unsigned buttons(void) {
    SceCtrlData pad;

    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    return pad.buttons;
}

static unsigned wait_buttons(unsigned mask) {
    unsigned b;

    while (buttons() & mask) {
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

/* Insert or replace name=value in bt3.txt (other lines kept). */
static void bt3_set(const char *name, const char *value) {
    char path[300], tmp[300], line[256], out[8192];
    FILE *fp;
    size_t n = 0;
    int found = 0;
    size_t nameLen = strlen(name);

    snprintf(path, sizeof(path), "%s/bt3.txt", HOME);
    snprintf(tmp, sizeof(tmp), "%s/bt3.txt.tmp", HOME);
    out[0] = '\0';
    fp = fopen(path, "r");
    if (fp != NULL) {
        while (fgets(line, sizeof(line), fp) != NULL) {
            char *eq = strchr(line, '=');
            if (eq != NULL && (size_t)(eq - line) == nameLen && strncmp(line, name, nameLen) == 0) {
                n += (size_t)snprintf(out + n, sizeof(out) - n, "%s=%s\n", name, value);
                found = 1;
            } else {
                n += (size_t)snprintf(out + n, sizeof(out) - n, "%s", line);
            }
            if (n + 64 >= sizeof(out)) {
                break;
            }
        }
        fclose(fp);
    }
    if (!found) {
        n += (size_t)snprintf(out + n, sizeof(out) - n, "%s=%s\n", name, value);
    }
    fp = fopen(tmp, "w");
    if (fp == NULL) {
        return;
    }
    fputs(out, fp);
    fclose(fp);
    remove(path);
    rename(tmp, path);
}

static void run_overlay(void) {
    int count = VitaEu_LocaleCount();
    int lang = 0, i, wide = gVitaWide != 0;
    int cursor = 0; /* 0 = language, 1 = widescreen */
    const char *cur = VitaEu_Locale();
    int langDirty = 0;

    for (i = 0; i < count; i++) {
        if (strcmp(VitaEu_LocaleAt(i), cur) == 0) {
            lang = i;
            break;
        }
    }

    sOpen = 1;
    psvDebugScreenInit();
    for (;;) {
        psvDebugScreenPrintf("\e[2J\e[H");
        psvDebugScreenPrintf("\e[33mVita settings\e[39m  (hold SELECT to open)\n\n");
        psvDebugScreenPrintf("UP / DOWN: row   LEFT / RIGHT: change\n");
        psvDebugScreenPrintf("CROSS: save & close   CIRCLE: close\n\n");

        psvDebugScreenPrintf(cursor == 0 ? "  \e[32m> Language: %s\e[39m\n" : "    Language: %s\n",
                             VitaEu_Available() ? VitaEu_LocaleName(lang) : "(no pzs3eu1)");
        psvDebugScreenPrintf(cursor == 1 ? "  \e[32m> Widescreen: %s\e[39m\n" : "    Widescreen: %s\n",
                             wide ? "on" : "off (4:3)");
        if (langDirty) {
            psvDebugScreenPrintf("\n\e[33mLanguage saved: restart the game to apply.\e[39m\n");
        }

        switch (wait_buttons(SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS |
                             SCE_CTRL_CIRCLE)) {
            case SCE_CTRL_UP:
                cursor = (cursor + 1) % 2;
                break;
            case SCE_CTRL_DOWN:
                cursor = (cursor + 1) % 2;
                break;
            case SCE_CTRL_LEFT:
                if (cursor == 0 && VitaEu_Available()) {
                    lang = (lang + count - 1) % count;
                } else if (cursor == 1) {
                    wide = !wide;
                }
                break;
            case SCE_CTRL_RIGHT:
                if (cursor == 0 && VitaEu_Available()) {
                    lang = (lang + 1) % count;
                } else if (cursor == 1) {
                    wide = !wide;
                }
                break;
            case SCE_CTRL_CROSS:
                if (VitaEu_Available()) {
                    const char *loc = VitaEu_LocaleAt(lang);
                    if (strcmp(loc, VitaEu_Locale()) != 0) {
                        VitaEu_SetLocale(loc);
                        langDirty = 1;
                        Vita_Log("vita: settings language %s (restart to apply)\n", loc);
                    }
                }
                gVitaWide = wide;
                bt3_set("wide", wide ? "1" : "0");
                Vita_Log("vita: settings widescreen %d\n", wide);
                sOpen = 0;
                psvDebugScreenPrintf("\e[2J\e[H");
                return;
            default: /* CIRCLE */
                sOpen = 0;
                psvDebugScreenPrintf("\e[2J\e[H");
                return;
        }
    }
}

void Vita_SettingsPoll(void) {
    SceCtrlData pad;
    unsigned long long now;
    int selectAlone;

    if (sOpen) {
        return;
    }
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    selectAlone = (pad.buttons & SCE_CTRL_SELECT) != 0 &&
                  (pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) == 0;
    now = sceKernelGetProcessTimeWide();
    if (selectAlone) {
        if (!sSelectWasDown) {
            sSelectDownUs = now;
            sSelectWasDown = 1;
        } else if (now - sSelectDownUs >= HOLD_US) {
            sSelectWasDown = 0;
            sSelectDownUs = 0;
            /* Wait for SELECT release so the first overlay press is clean. */
            while (buttons() & SCE_CTRL_SELECT) {
                sceKernelDelayThread(20000);
            }
            run_overlay();
        }
    } else {
        sSelectWasDown = 0;
        sSelectDownUs = 0;
    }
}
