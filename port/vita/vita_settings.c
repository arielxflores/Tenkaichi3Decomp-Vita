/*
 * Hold SELECT (~1 s, without L/R) for an Adrenaline-style pause menu over the game:
 * dark full-screen sheet, list with cursor, CROSS confirm / CIRCLE back.
 *
 * Uses psvDebugScreen while the game is frozen inside PadRead (no vitaGL swaps), so it
 * does not fight the renderer like an in-frame debug overlay would.
 */
#include "vita_settings.h"
#include "vita_eu.h"
#include "debugScreen.h"
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/display.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define HOME "ux0:data/bt3"
#define HOLD_US (1000 * 1000)

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

/* Adrenaline-like dark sheet: title + list. cursor rows: 0 language, 1 widescreen, 2 resume. */
static void draw_menu(int cursor, int lang, int wide, int langDirty, int editingLang) {
    int count = VitaEu_LocaleCount();
    int i;

    psvDebugScreenPrintf("\e[2J\e[H");
    psvDebugScreenPrintf("\n\n");
    psvDebugScreenPrintf("          \e[32m******************************\e[39m\n");
    psvDebugScreenPrintf("          \e[32m*       BT3 SETTINGS        *\e[39m\n");
    psvDebugScreenPrintf("          \e[32m******************************\e[39m\n\n");

    if (editingLang) {
        psvDebugScreenPrintf("          Language\n\n");
        for (i = 0; i < count; i++) {
            if (!VitaEu_Available() && i != 0) {
                continue;
            }
            psvDebugScreenPrintf(i == lang ? "          \e[32m> %s\e[39m\n" : "            %s\n", VitaEu_LocaleName(i));
        }
        psvDebugScreenPrintf("\n          \e[33mUP/DOWN  CROSS: save  CIRCLE: back\e[39m\n");
        if (langDirty) {
            psvDebugScreenPrintf("\n          \e[33mSaved — restart to apply language.\e[39m\n");
        }
        return;
    }

    psvDebugScreenPrintf(cursor == 0 ? "          \e[32m> Language: %s\e[39m\n" : "            Language: %s\n",
                         VitaEu_Available() ? VitaEu_LocaleName(lang) : "(no pzs3eu1)");
    psvDebugScreenPrintf(cursor == 1 ? "          \e[32m> Widescreen: %s\e[39m\n" : "            Widescreen: %s\n",
                         wide ? "On" : "Off");
    psvDebugScreenPrintf(cursor == 2 ? "          \e[32m> Resume game\e[39m\n" : "            Resume game\n");
    psvDebugScreenPrintf("\n          \e[33mUP/DOWN  CROSS: OK  CIRCLE: close\e[39m\n");
    if (langDirty) {
        psvDebugScreenPrintf("\n          \e[33mLanguage saved — restart the game.\e[39m\n");
    }
}

static void run_overlay(void) {
    int count = VitaEu_LocaleCount();
    int lang = 0, i, wide = gVitaWide != 0;
    int cursor = 0;
    int langDirty = 0;
    int editingLang = 0;
    const char *cur = VitaEu_Locale();
    unsigned btn;

    for (i = 0; i < count; i++) {
        if (strcmp(VitaEu_LocaleAt(i), cur) == 0) {
            lang = i;
            break;
        }
    }

    sOpen = 1;
    psvDebugScreenInit();

    for (;;) {
        draw_menu(cursor, lang, wide, langDirty, editingLang);
        btn = wait_buttons(SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS |
                           SCE_CTRL_CIRCLE | SCE_CTRL_SELECT);

        if (editingLang) {
            switch (btn) {
                case SCE_CTRL_UP:
                case SCE_CTRL_LEFT:
                    if (VitaEu_Available()) {
                        lang = (lang + count - 1) % count;
                    }
                    break;
                case SCE_CTRL_DOWN:
                case SCE_CTRL_RIGHT:
                    if (VitaEu_Available()) {
                        lang = (lang + 1) % count;
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
                    editingLang = 0;
                    break;
                default: /* CIRCLE / SELECT */
                    editingLang = 0;
                    break;
            }
            continue;
        }

        switch (btn) {
            case SCE_CTRL_UP:
                cursor = (cursor + 2) % 3;
                break;
            case SCE_CTRL_DOWN:
                cursor = (cursor + 1) % 3;
                break;
            case SCE_CTRL_LEFT:
            case SCE_CTRL_RIGHT:
                if (cursor == 1) {
                    wide = !wide;
                    gVitaWide = wide;
                    bt3_set("wide", wide ? "1" : "0");
                    Vita_Log("vita: settings widescreen %d\n", wide);
                } else if (cursor == 0 && VitaEu_Available()) {
                    editingLang = 1;
                }
                break;
            case SCE_CTRL_CROSS:
                if (cursor == 0) {
                    editingLang = 1;
                } else if (cursor == 1) {
                    wide = !wide;
                    gVitaWide = wide;
                    bt3_set("wide", wide ? "1" : "0");
                    Vita_Log("vita: settings widescreen %d\n", wide);
                } else {
                    goto done;
                }
                break;
            default: /* CIRCLE / SELECT */
                goto done;
        }
    }

done:
    sOpen = 0;
    psvDebugScreenPrintf("\e[2J\e[H");
    /* Give the display back; next game frame restores vitaGL. */
    sceDisplayWaitVblankStart();
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
            while ((pad.buttons & SCE_CTRL_SELECT) != 0) {
                sceKernelDelayThread(20000);
                memset(&pad, 0, sizeof(pad));
                sceCtrlPeekBufferPositive(0, &pad, 1);
            }
            run_overlay();
        }
    } else {
        sSelectWasDown = 0;
        sSelectDownUs = 0;
    }
}
