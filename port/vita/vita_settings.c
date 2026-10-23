/*
 * Hold SELECT (~1 s, without L/R) for Vita system-style settings (sceMsgDialog), like the PS
 * button sheet: language and widescreen. Language goes to lang.txt (restart to reload menus);
 * widescreen to bt3.txt and applies immediately.
 *
 * Do not use psvDebugScreen here — it fights vitaGL mid-game and freezes with a blank display.
 */
#include "vita_settings.h"
#include "vita_eu.h"
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/message_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/power.h>
#include <vitaGL.h>
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
static int sDialogReady;

int Vita_SettingsOpen(void) {
    return sOpen;
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

static void dialog_ensure(void) {
    SceCommonDialogConfigParam cfg;

    if (sDialogReady) {
        return;
    }
    /* Message dialog is part of Common Dialog (no separate sysmodule id on VitaSDK). */
    sceCommonDialogConfigParamInit(&cfg);
    sceCommonDialogSetConfigParam(&cfg);
    sDialogReady = 1;
}

/* Native message sheet; keeps swapping so the dialog paints over the game (vitaGL common-dialog path). */
static int dialog_run(const char *msg, int buttonType, SceMsgDialogButtonsParam *btns) {
    SceMsgDialogParam param;
    SceMsgDialogUserMessageParam user;
    SceMsgDialogResult result;
    int st;

    dialog_ensure();
    sceMsgDialogParamInit(&param);
    memset(&user, 0, sizeof(user));
    user.msg = (const SceChar8 *)msg;
    user.buttonType = buttonType;
    user.buttonParam = btns;
    param.userMsgParam = &user;
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    if (sceMsgDialogInit(&param) < 0) {
        Vita_Log("vita: settings MsgDialog init failed\n");
        return -1;
    }
    for (;;) {
        st = sceMsgDialogGetStatus();
        if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
            break;
        }
        vglSwapBuffers(GL_TRUE);
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    }
    memset(&result, 0, sizeof(result));
    sceMsgDialogGetResult(&result);
    sceMsgDialogTerm();
    return result.buttonId;
}

static void run_overlay(void) {
    int count = VitaEu_LocaleCount();
    int lang = 0, i, wide = gVitaWide != 0;
    int langDirty = 0;
    const char *cur = VitaEu_Locale();
    char msg[480];
    SceMsgDialogButtonsParam buttons;
    int btn;

    for (i = 0; i < count; i++) {
        if (strcmp(VitaEu_LocaleAt(i), cur) == 0) {
            lang = i;
            break;
        }
    }

    sOpen = 1;

    for (;;) {
        snprintf(msg, sizeof(msg),
                 "Vita settings\n\nLanguage: %s\nWidescreen: %s\n\n"
                 "Language needs a restart. Widescreen applies now.",
                 VitaEu_Available() ? VitaEu_LocaleName(lang) : "(no pzs3eu1)",
                 wide ? "On" : "Off (4:3)");
        memset(&buttons, 0, sizeof(buttons));
        buttons.msg1 = "Language";
        buttons.msg2 = "Widescreen";
        buttons.msg3 = "Close";
        btn = dialog_run(msg, SCE_MSG_DIALOG_BUTTON_TYPE_3BUTTONS, &buttons);
        if (btn == SCE_MSG_DIALOG_BUTTON_ID_BUTTON1 && VitaEu_Available()) {
            /* Cycle languages with Prev / Next / Save */
            for (;;) {
                snprintf(msg, sizeof(msg),
                         "Language\n\n%s\n\nRestart the game after Save to reload menus and story text.",
                         VitaEu_LocaleName(lang));
                memset(&buttons, 0, sizeof(buttons));
                buttons.msg1 = "Prev";
                buttons.msg2 = "Next";
                buttons.msg3 = "Save";
                btn = dialog_run(msg, SCE_MSG_DIALOG_BUTTON_TYPE_3BUTTONS, &buttons);
                if (btn == SCE_MSG_DIALOG_BUTTON_ID_BUTTON1) {
                    lang = (lang + count - 1) % count;
                } else if (btn == SCE_MSG_DIALOG_BUTTON_ID_BUTTON2) {
                    lang = (lang + 1) % count;
                } else {
                    /* Save or dismiss */
                    if (btn == SCE_MSG_DIALOG_BUTTON_ID_BUTTON3) {
                        const char *loc = VitaEu_LocaleAt(lang);
                        if (strcmp(loc, VitaEu_Locale()) != 0) {
                            VitaEu_SetLocale(loc);
                            langDirty = 1;
                            Vita_Log("vita: settings language %s (restart to apply)\n", loc);
                        }
                    }
                    break;
                }
            }
        } else if (btn == SCE_MSG_DIALOG_BUTTON_ID_BUTTON2) {
            snprintf(msg, sizeof(msg), "Widescreen (16:9)?\n\nCurrent: %s", wide ? "On" : "Off");
            btn = dialog_run(msg, SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, NULL);
            if (btn == SCE_MSG_DIALOG_BUTTON_ID_YES) {
                wide = 1;
            } else if (btn == SCE_MSG_DIALOG_BUTTON_ID_NO) {
                wide = 0;
            }
            gVitaWide = wide;
            bt3_set("wide", wide ? "1" : "0");
            Vita_Log("vita: settings widescreen %d\n", wide);
        } else {
            /* Close or dialog error */
            break;
        }
    }

    if (langDirty) {
        dialog_run("Language saved.\n\nClose the game and open it again to apply.",
                   SCE_MSG_DIALOG_BUTTON_TYPE_OK, NULL);
    }

    sOpen = 0;
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
