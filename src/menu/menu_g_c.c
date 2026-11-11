#include "common.h"
#include "menu/menu_g.h"
#include "sys/pad.h"
#include "sys/save.h"

DuelMenu *gDuelMenu = NULL; /* 0x3B38E8 */

#ifdef PORT_VITA
/* The Vita's Multiplayer (the second plate, the console's 1P VS 2P): a choice of how, in the battle type's three plates,
   which read Two Controllers (the console's two-player mode, as before: needs a second controller), Ad Hoc: Host and
   Ad Hoc: Join while it is open (port/vita/vita_optart.c Vita_VersusNetPlates). Hosting or joining starts the game
   again as a match against another Vita nearby (port/vita/vita_net.c), with the host's battle settings. */
extern int Vita_VersusNetReady(void);
extern int Vita_VersusNetPlates(int on);
extern void Vita_NetRestartInto(int role, int link, const int *rules);
static s32 sVitaNet;      /* the choice is open */
static s32 sVitaNetKeep;  /* the battle type's cursor, kept while the choice has its plates */
static s32 sVitaNetGo;    /* 1 host, 2 join: chosen, the screen fades out */
static s32 sVitaNetWait;  /* frames until the game starts again */
#endif

/*
 * DuelMenu, 0x352EC0..0x3562B8: the duel mode's menu (progress mode 38). Two guide characters (Vegeta and
 * Nappa), a top list (1P vs COM, 1P vs 2P, COM vs COM, battle settings), then the battle type (single, team,
 * DP battle), for a DP battle the DP limit, and under "battle settings" a list of five settings plus "restore
 * defaults" with a value picker. The screen's frame loop (DuelMenu_Run, 0x356090) is the last function.
 */

#define DM gDuelMenu

#define DM_SAY(who, n) \
    DM->talker = who; \
    DM->voiceLine = n; \
    Voice_PlayWithSubtitle(DM->subtitles, DUEL_VOICE_BASE, DM->voiceLine)

/* Counts a frame without input; after 3600 the second guide complains. */
void DuelMenu_Idle(void) {
    DM->idle++;
    if (DM->idle == DUEL_IDLE_FRAMES) {
        DM_SAY(1, 8);
        DM->idle = 0;
    }
}

#define DM_VALUE_UV(n) \
    uv.y0 = (n) * 0x20; \
    uv.y1 = uv.y0 + 0x20; \
    uv.x0 = 0; \
    uv.x1 = 0x100

/* Shows or hides the value plates of one setting and gives the visible ones their text. */
void DuelMenu_ShowValues(s32 item, s32 show) {
    MFlashRef ref;
    MFlashUv uv;
    char name[64];
    char sub[64];
    MFlash *flash = &DM->flash[0];
    s32 i;
    s32 j;

    switch (item) {
    case 0:
        for (i = 0; i < 5; i++) {
            sprintf(name, "mc_set_%d_%d", item + 1, i + 1);
            Flash_FindLabel(flash, NULL, name, &ref);
            Flash_ClipSetFlags(flash, &ref, 2, (u8)show);
            if (show) {
                DM_VALUE_UV(i);
                sprintf(sub, "mc_set_%d_off", item + 1);
                Flash_FindLabel(flash, name, sub, &ref);
                Flash_ClipSetUv(flash, &ref, &uv);
                sprintf(sub, "mc_set_%d_on", item + 1);
                Flash_FindLabel(flash, name, sub, &ref);
                Flash_ClipSetUv(flash, &ref, &uv);
            }
        }
        break;
    case 1:
        for (i = 0; i < 5; i++) {
            sprintf(name, "mc_set_%d_%d", item + 1, i + 1);
            Flash_FindLabel(flash, NULL, name, &ref);
            Flash_ClipSetFlags(flash, &ref, 2, (u8)show);
            if (show) {
                DM_VALUE_UV(i);
                sprintf(sub, "mc_set_%d_off", item + 1);
                Flash_FindLabel(flash, name, sub, &ref);
                Flash_ClipSetUv(flash, &ref, &uv);
                sprintf(sub, "mc_set_%d_on", item + 1);
                Flash_FindLabel(flash, name, sub, &ref);
                Flash_ClipSetUv(flash, &ref, &uv);
            }
        }
        break;
    case 2:
        for (i = 0; i < 7; i++) {
            sprintf(name, "mc_set_%d_%d", item + 1, i + 1);
            Flash_FindLabel(flash, NULL, name, &ref);
            Flash_ClipSetFlags(flash, &ref, 2, (u8)show);
            if (show) {
                DM_VALUE_UV(i);
                sprintf(sub, "mc_set_%d_off", item + 1);
                Flash_FindLabel(flash, name, sub, &ref);
                Flash_ClipSetUv(flash, &ref, &uv);
                sprintf(sub, "mc_set_%d_on", item + 1);
                Flash_FindLabel(flash, name, sub, &ref);
                Flash_ClipSetUv(flash, &ref, &uv);
            }
        }
        break;
    case 3:
        Flash_FindLabel(flash, NULL, "set_4_1p2p", &ref);
        Flash_ClipSetFlags(flash, &ref, 2, (u8)show);
        for (i = 0; i < 2; i++) {
            for (j = 0; j < 2; j++) {
                sprintf(name, i ? "mc_set_%db_%d" : "mc_set_%da_%d", item + 1, j + 1);
                Flash_FindLabel(flash, NULL, name, &ref);
                Flash_ClipSetFlags(flash, &ref, 2, (u8)show);
                if (show) {
                    DM_VALUE_UV(j);
                    Flash_FindLabel(flash, name, "mc_set_onoff_off", &ref);
                    Flash_ClipSetUv(flash, &ref, &uv);
                    Flash_FindLabel(flash, name, "mc_set_onoff_on", &ref);
                    Flash_ClipSetUv(flash, &ref, &uv);
                }
            }
        }
        break;
    case 4:
        for (i = 0; i < 2; i++) {
            sprintf(name, "mc_set_%d_%d", item + 1, i + 1);
            Flash_FindLabel(flash, NULL, name, &ref);
            Flash_ClipSetFlags(flash, &ref, 2, (u8)show);
            if (show) {
                DM_VALUE_UV(i);
                Flash_FindLabel(flash, name, "mc_set_onoff_off", &ref);
                Flash_ClipSetUv(flash, &ref, &uv);
                Flash_FindLabel(flash, name, "mc_set_onoff_on", &ref);
                Flash_ClipSetUv(flash, &ref, &uv);
            }
        }
        break;
    }
}

/* Prepares the value picker for the setting under the cursor: which rule(s), how many values, their text. */
void DuelMenu_SetupValue(void) {
    switch (DM->sel[DUEL_LEVEL_SETTING]) {
    case 0:
        DM->rule = 0;
        DM->valueCount = 5;
        DM->column = 0;
        DM->value[0] = gSaveData->rule[DM->rule];
        DM->value[1] = 0;
        DM->tex[47] = MTEX(DM->setRes, 1);
        DM->tex[48] = NULL;
        break;
    case 1:
        DM->rule = 1;
        DM->valueCount = 5;
        DM->column = 0;
        DM->value[0] = gSaveData->rule[DM->rule];
        DM->value[1] = 0;
        DM->tex[47] = MTEX(DM->setRes, 1);
        DM->tex[48] = NULL;
        break;
    case 2:
        DM->rule = 2;
        DM->valueCount = 7;
        DM->column = 0;
        DM->value[0] = gSaveData->rule[DM->rule];
        DM->value[1] = 0;
        DM->tex[47] = MTEX(DM->setRes, 1);
        DM->tex[48] = NULL;
        break;
    case 3:
        DM->rule = 3;
        DM->valueCount = 2;
        DM->column = 0;
        DM->value[0] = gSaveData->rule[DM->rule];
        DM->value[1] = gSaveData->rule[DM->rule + 1];
        DM->tex[47] = NULL;
        DM->tex[48] = MTEX(DM->setRes, 2);
        break;
    case 4:
        DM->rule = 5;
        DM->valueCount = 2;
        DM->column = 0;
        DM->value[0] = gSaveData->rule[DM->rule];
        DM->value[1] = 0;
        DM->tex[47] = NULL;
        DM->tex[48] = MTEX(DM->setRes, 2);
        break;
    }
}

#define DM_RES(n) \
    res = (MTexRes *)MPACK_AT(DM->res, n); \
    Res_RelocateOffsets(&res, res, res)

/* Loads and unpacks the screen (section `section` of archive 5), builds its movie and windows, and takes the
   three cursors left in gProgress by the last visit. */
void DuelMenu_Init(s32 section) {
    MTexRes *res = NULL;
    s32 i;

    DM = Heap_Alloc(0x194, 0x20, 0, 2);
    memset(DM, 0, 0x194);
    DM->pack = (u32 *)MPACK_AT(gMenuArc5, section);
    DM->res = Sprite_Unpack(DM->pack, NULL, NULL);
#ifdef PORT_VITA
    {
        extern void Vita_VersusArt(void *res); /* port/vita/vita_optart.c: the second plate reads "Multiplayer" */
        Vita_VersusArt(DM->res);
        sVitaNet = 0; /* (Multiplayer's choice closed, its plates the disc's again: Vita_VersusArt) */
        sVitaNetGo = 0;
    }
#endif
    DM_RES(1);
    DM->bg = res;
    DM_RES(2);
    DM->tex[0] = MTEX(res, 0);
    DM->tex[1] = MTEX(res, 1);
    DM->tex[8] = MTEX(res, 2);
    DM_RES(3);
    DM->tex[38] = MTEX(res, 0);
    DM->tex[39] = MTEX(res, 1);
    DM->tex[40] = MTEX(res, 3);
    DM_RES(4);
    DM->tex[35] = MTEX(res, 0);
    DM->tex[36] = MTEX(res, 1);
    DM->tex[37] = MTEX(res, 3);
    DM_RES(5);
    DM->tex[2] = MTEX(res, 0);
    DM->tex[3] = MTEX(res, 1);
    DM->tex[4] = MTEX(res, 2);
    DM->tex[5] = MTEX(res, 3);
    DM->tex[6] = MTEX(res, 4);
    DM->tex[7] = MTEX(res, 5);
    DM_RES(6);
    DM->tex[12] = MTEX(res, 0);
    DM->tex[15] = MTEX(res, 1);
    DM->tex[14] = MTEX(res, 2);
    DM_RES(7);
    DM->tex[10] = MTEX(res, 1);
    DM->tex[17] = MTEX(res, 2);
    DM_RES(8);
    DM->tex[13] = MTEX(res, 0);
    DM->tex[16] = MTEX(res, 1);
    DM->tex[9] = MTEX(res, 2);
    DM->tex[11] = MTEX(res, 3);
    DM_RES(9);
    DM->tex[41] = MTEX(res, 0);
    DM_RES(10);
    DM->tex[42] = MTEX(res, 0);
    DM->tex[47] = MTEX(res, 1);
    DM->tex[48] = MTEX(res, 2);
    DM->setRes = res;
    DM_RES(11);
    DM->tex[46] = MTEX(res, 0);
    DM->tex[45] = MTEX(res, 1);
    DM_RES(12);
    DM->tex[43] = MTEX(res, 0);
    DM->tex[18] = MTEX(res, 1);
    DM->tex[19] = MTEX(res, 2);
    DM_RES(13);
    DM->tex[20] = MTEX(res, 0);
    DM->tex[21] = MTEX(res, 1);
    DM_RES(14);
    DM->tex[34] = MTEX(res, 0);
    DM->tex[44] = MTEX(res, 1);
    DM_RES(15);
    DM->tex[22] = MTEX(res, 0);
    DM->tex[23] = MTEX(res, 1);
    DM_RES(16);
    DM->tex[24] = MTEX(res, 0);
    DM->tex[25] = MTEX(res, 1);
    DM_RES(17);
    DM->tex[32] = MTEX(res, 0);
    DM->tex[33] = MTEX(res, 1);
    DM->tex[28] = MTEX(res, 2);
    DM->tex[29] = MTEX(res, 3);
    DM->tex[30] = MTEX(res, 4);
    DM->tex[31] = MTEX(res, 5);
    DM_RES(18);
    DM->tex[26] = MTEX(res, 0);
    DM->tex[27] = MTEX(res, 1);
    Flash_Create(&DM->flash[0], MPACK_AT(DM->res, 19), DM->tex);
    Flash_Play(&DM->flash[0], 1);
    DM_RES(24);
    IconWin_Init(MPACK_AT(DM->res, 23), res);
    IconWin_Open();
    DM->msgText = MPACK_AT(DM->res, 21);
    DM->subtitles = MPACK_AT(DM->res, 26);
    MsgWin_Init(MPACK_AT(DM->res, 22), DM->msgText, 1, 0);
    MsgWin_Open();
    DM->setText = MPACK_AT(DM->res, 27);
    Dialog_Init(MPACK_AT(DM->res, 25), DM->setText, 0);
    Dialog_SetLayout(0);
    for (i = 0; i < 2; i++) {
        DM->blink[i] = Rand_Range(0x20);
    }
    DM->voiceLine = -1;
    DM->sel[DUEL_LEVEL_TOP] = DUEL_PROG->versus;
    DM->sel[DUEL_LEVEL_TYPE] = DUEL_PROG->battleType;
    DM->sel[DUEL_LEVEL_DP] = DUEL_PROG->dpLimit;
}

/* Frees the screen. */
void DuelMenu_Term(void) {
    s32 i;

    Dialog_Term();
    MsgWin_Term();
    IconWin_Term();
    for (i = 0; i < DUELMENU_FLASH_NUM; i++) {
        Flash_Destroy(&DM->flash[i]);
    }
    if (DM->res != NULL) {
        Heap_Free(DM->res);
        DM->res = NULL;
    }
    if (DM != NULL) {
        Heap_Free(DM);
        DM = NULL;
    }
}

/* The rules as a member of a structure inside the save block: the offset split of the loop in DuelMenu_Draw
   (0xC20 + 0x14) needs a nested member; where the inner structure really starts is not known. */
typedef struct DuelSave {
    /* 0x0000 */ u8 unk0[0xC28];
    /* 0x0C28 */ struct {
        u64 stageBits;
        u32 bgmBits;
        s32 rule[6];
    } s;
} DuelSave;

#define DM_SAVE ((DuelSave *)gSaveData)

#define DM_ROW_UV(n, w) \
    uv.y0 = (n) * 0x20; \
    uv.y1 = uv.y0 + 0x20; \
    uv.x0 = 0; \
    uv.x1 = w

#define DM_SET_UV(parent, clip) \
    Flash_FindLabel(flash, parent, clip, &ref); \
    Flash_ClipSetUv(flash, &ref, &uv)

/* The clip of guide i. The expression is written out at both uses: with a local variable the second call's
   argument set-up does not match. */
#define DM_GUIDE(i) ((i) ? "mc_guide_nappa" : "mc_guide_bejita")

/* Draws the background, the guides (blinking, the talker's mouth moving), every plate's text strip, the
   current values of the five settings, the scrolling cloud and smoke layers, the movie and the two windows. */
void DuelMenu_Draw(void) {
    MFlashRef ref;
    MFlashUv uv;
    char name[64];
    char sub[64];
    char smoke[64];
    s32 i;
    s32 j;
    s32 rule;
    MFlash *flash;

    Sprite_DrawPicture(DM->bg, 0, 0, 0x80);
    flash = &DM->flash[0];
    for (i = 0; i < 2; i++) {
        Flash_FindLabel(flash, DM_GUIDE(i), i ? "mc_guide_nappa_eye" : "mc_guide_bejita_eye", &ref);
        FlashAnim_Blink(flash, &ref, &DM->blink[i], 0);
        Flash_FindLabel(flash, DM_GUIDE(i), i ? "mc_guide_nappa_mouth" : "mc_guide_bejita_mouth", &ref);
        if (DM->talker == i) {
            FlashAnim_Talk(flash, &ref, &DM->talk[i], 0);
        } else {
            FlashAnim_ShowNext2(flash, &ref, 0);
        }
    }
    rule = 0;
    for (i = 0; i < 4; i++) {
        DM_ROW_UV(i, 0x200);
        sprintf(name, "mc_menu_plate_%d", i + 1);
        if (i == 1) {
            Flash_FindLabel(flash, NULL, name, &ref);
#ifdef PORT_VITA /* (Multiplayer leads to ad hoc too: open without a second controller) */
            Flash_ClipSetColor(flash, &ref, (DM->flags & DUELMENU_NO_PAD2) && !Vita_VersusNetReady() ? 0.4f : 1.0f);
#else
            Flash_ClipSetColor(flash, &ref, (DM->flags & DUELMENU_NO_PAD2) ? 0.4f : 1.0f);
#endif
        }
        DM_SET_UV(name, "mc_menu_text_off");
        DM_SET_UV(name, "mc_menu_text_on");
    }
    for (i = 0; i < 3; i++) {
        DM_ROW_UV(i, 0x200);
        sprintf(name, "mc_mode_%d", i + 1);
#ifdef PORT_VITA
        if (i == 0) { /* (Multiplayer's choice: Two Controllers greyed without a second controller) */
            Flash_FindLabel(flash, NULL, name, &ref);
            Flash_ClipSetColor(flash, &ref, sVitaNet && (DM->flags & DUELMENU_NO_PAD2) ? 0.4f : 1.0f);
        }
#endif
        DM_SET_UV(name, "mc_mode_text_off");
        DM_SET_UV(name, "mc_mode_text_on");
    }
    uv.y1 = 0x10;
    uv.x1 = 0x40;
    uv.y0 = 0;
    uv.x0 = 0;
    Flash_FindLabel(flash, NULL, "mc_menu_plate_anime", &ref);
    FlashAnim_Sheet(flash, &ref, &DM->iconTimer, &DM->iconFrame, &uv, 1, 4, 4);
    for (i = 0; i < 3; i++) {
        DM_ROW_UV(i, 0x100);
        sprintf(name, "mc_dp_%d", i + 1);
        DM_SET_UV(name, "mc_dp_value_off");
        DM_SET_UV(name, "mc_dp_value_on");
    }
    for (i = 0; i < 6; i++) {
        DM_ROW_UV(i, 0x100);
        sprintf(name, "mc_battle_set_text_%d", i + 1);
        DM_SET_UV(name, "mc_battle_set_text_off");
        DM_SET_UV(name, "mc_battle_set_text_on");
    }
    for (i = 0; i < 5; i++) {
        switch (i) {
        case 0:
            rule = 0;
            break;
        case 1:
            rule = 1;
            break;
        case 2:
            rule = 2;
            break;
        case 3:
            rule = 3;
            break;
        case 4:
            rule = 5;
            break;
        }
        switch (i) {
        case 0:
        case 1:
        case 2:
            DM_ROW_UV(gSaveData->rule[rule], 0x100);
            sprintf(name, "mc_set_%d", i + 1);
            sprintf(sub, "mc_set_%d_off", i + 1);
            DM_SET_UV(name, sub);
            sprintf(sub, "mc_set_%d_on", i + 1);
            DM_SET_UV(name, sub);
            break;
        case 4:
            DM_ROW_UV(gSaveData->rule[rule], 0x100);
            sprintf(name, "mc_set_%d", i + 1);
            DM_SET_UV(name, "mc_set_onoff_off");
            DM_SET_UV(name, "mc_set_onoff_on");
            break;
        case 3:
            for (j = 0; j < 2; j++) {
                DM_ROW_UV(DM_SAVE->s.rule[rule + j], 0x80);
                sprintf(name, j ? "mc_set_%dbs" : "mc_set_%das", i + 1);
                sprintf(sub, j ? "mc_set_%dbs_off" : "mc_set_%das_off", i + 1);
                DM_SET_UV(name, sub);
                sprintf(sub, j ? "mc_set_%dbs_on" : "mc_set_%das_on", i + 1);
                DM_SET_UV(name, sub);
            }
            break;
        }
    }
    for (i = 0; i < 5; i++) {
        DuelMenu_ShowValues(i, i == DM->sel[DUEL_LEVEL_VALUE]);
    }
    uv.y0 = 0;
    uv.y1 = 0x100;
    uv.x0 = 0;
    uv.x1 = 0x200;
    Flash_FindLabel(flash, NULL, "mc_kumo", &ref);
    FlashAnim_Scroll(flash, &ref, &uv, &DM->cloud, NULL, -0.28444445f, 0.0f);
    {
        f32 speed[6] = { -1.2190476f, 1.4222222f, 1.4222222f, 1.7066667f, 1.4222222f, 1.4222222f };

        uv.y1 = 0x100;
        uv.x1 = 0x200;
        uv.y0 = 0;
        uv.x0 = 0;
        for (i = 0; i < 2; i++) {
            sprintf(smoke, "mc_kemuri_%d", i + 1);
            Flash_FindLabel(flash, NULL, smoke, &ref);
            FlashAnim_Scroll(flash, &ref, &uv, &DM->smoke[i], NULL, speed[i], 0.0f);
        }
        uv.y1 = 0x200;
        uv.x1 = 0x100;
        uv.y0 = 0;
        uv.x0 = 0;
        for (i = 2; i < 6; i++) {
            sprintf(smoke, "mc_kemuri_%d", i + 1);
            Flash_FindLabel(flash, NULL, smoke, &ref);
            FlashAnim_Scroll(flash, &ref, &uv, NULL, &DM->smoke[i], 0.0f, speed[i]);
        }
    }
    for (i = 0; i < DUELMENU_FLASH_NUM; i++) {
        Flash_Draw(&DM->flash[i]);
    }
    IconWin_Draw();
    MsgWin_Draw(0, 0, DM->voiceLine);
}

/* Sends the plate under the cursor of a level to a label. */
void DuelMenu_ClipGoto(s32 level, char *label) {
    MFlashRef ref;
    char name[64];
    MFlash *flash = &DM->flash[0];
    s32 i;

    switch (level) {
    case DUEL_LEVEL_TOP:
        sprintf(name, "mc_menu_plate_%d", DM->sel[level] + 1);
        break;
    case DUEL_LEVEL_TYPE:
        sprintf(name, "mc_mode_%d", DM->sel[level] + 1);
        break;
    case DUEL_LEVEL_DP:
        sprintf(name, "mc_dp_%d", DM->sel[level] + 1);
        break;
    case DUEL_LEVEL_SETTING:
        sprintf(name, "mc_battle_set_text_%d", DM->sel[level] + 1);
        Flash_FindLabel(flash, NULL, name, &ref);
        Flash_ClipGotoLabel(flash, &ref, label);
        switch (DM->sel[level]) {
        case 3:
            for (i = 0; i < 2; i++) {
                sprintf(name, i ? "mc_set_%dbs" : "mc_set_%das", DM->sel[level] + 1);
                Flash_FindLabel(flash, NULL, name, &ref);
                Flash_ClipGotoLabel(flash, &ref, label);
            }
            break;
        case 5:
            break;
        default:
            sprintf(name, "mc_set_%d", DM->sel[level] + 1);
            Flash_FindLabel(flash, NULL, name, &ref);
            Flash_ClipGotoLabel(flash, &ref, label);
            break;
        }
        return;
    case DUEL_LEVEL_VALUE:
        switch (DM->sel[level]) {
        case 3:
            sprintf(name, DM->column ? "mc_set_%db_%d" : "mc_set_%da_%d", DM->sel[level] + 1,
                    DM->value[DM->column] + 1);
            break;
        case 5:
            break;
        default:
            sprintf(name, "mc_set_%d_%d", DM->sel[level] + 1, DM->value[0] + 1);
            break;
        }
        break;
    }
    Flash_FindLabel(flash, NULL, name, &ref);
    Flash_ClipGotoLabel(flash, &ref, label);
}

/* Tracks whether a second controller is present, starts the line of the current top item once the greeting
   has ended, and steps the movie. */
void DuelMenu_Update(void) {
    s32 i;

    if (DM->flags & DUELMENU_NO_PAD2) {
        if (gPad[1].status != 0xFF) {
            DM->flags ^= DUELMENU_NO_PAD2;
        }
    } else {
        if (gPad[1].status == 0xFF) {
            DM->flags |= DUELMENU_NO_PAD2;
        }
    }
    if ((DM->flags & DUELMENU_GREETED) && DM->voiceLine == 0) {
        if (Voice_GetStat(0) == 5) {
            DM_SAY(0, DM->sel[DUEL_LEVEL_TOP] + 1);
        }
    }
    for (i = 0; i < DUELMENU_FLASH_NUM; i++) {
        Flash_Advance(&DM->flash[i]);
    }
#ifdef PORT_VITA
    if (sVitaNetGo != 0 && --sVitaNetWait <= 0) { /* (Ad Hoc: Host / Join, faded out: the game starts again for the match) */
        s32 role = sVitaNetGo;
        sVitaNetGo = 0;
        Vita_NetRestartInto(role, 1, (const int *)gSaveData->rule);
    }
#endif
}

#define DM_LEVEL_GOTO(label) DuelMenu_ClipGoto(DM->level, label)
#define DM_CUR DM->sel[DM->level]

#ifdef PORT_VITA
/* The message window says what the plate under the cursor does (lines 5 to 7, written over while the choice is
   open), without a voice. */
static void DuelMenu_VitaNetSay(void) {
    Voice_StopWithLip();
    DM->voiceLine = DM->sel[DUEL_LEVEL_TYPE] + 5;
}

/* Multiplayer chosen on the top list: its choice opens in the battle type's plates. */
static void DuelMenu_VitaNetOpen(void) {
    DM_LEVEL_GOTO("fl_ok");
    Flash_GotoLabel(&DM->flash[0], "fl_mode_2", 1);
    DM->level = DUEL_LEVEL_TYPE;
    sVitaNetKeep = DM->sel[DUEL_LEVEL_TYPE];
    Vita_VersusNetPlates(1);
    sVitaNet = 1;
    DM->sel[DUEL_LEVEL_TYPE] = (DM->flags & DUELMENU_NO_PAD2) ? 1 : 0;
    DuelMenu_VitaNetSay();
    DM_LEVEL_GOTO("fl_on_start");
    Snd_PlaySe(1, 1);
}

/* The choice's plates as the battle types again (the cursor where the battle type was). */
static void DuelMenu_VitaNetClose(void) {
    DM_LEVEL_GOTO("fl_off_start");
    Vita_VersusNetPlates(0);
    sVitaNet = 0;
    DM->sel[DUEL_LEVEL_TYPE] = sVitaNetKeep;
}

static void DuelMenu_VitaNetInput(void) {
    if (sVitaNetGo != 0) {
        return; /* (fading out) */
    }
    if (gPad[0].gameRepeat & (PADG_UP | PADG_DOWN)) {
        DM->idle = 0;
        DM_LEVEL_GOTO("fl_off_start");
        DM_CUR += (gPad[0].gameRepeat & PADG_UP) ? 2 : 1;
        DM_CUR %= 3;
        DM_LEVEL_GOTO("fl_on_start");
        DuelMenu_VitaNetSay();
        Snd_PlaySe(1, 0);
    } else if (gPad[0].gamePressed & PADG_CROSS) {
        DM->idle = 0;
        if (DM_CUR == 0) {
            if (DM->flags & DUELMENU_NO_PAD2) {
                Snd_PlaySe(1, 7); /* (no second controller) */
                return;
            }
            DuelMenu_VitaNetClose(); /* the console's two-player mode: on to the battle type, as before */
            DM_LEVEL_GOTO("fl_on_start");
            DM_SAY(0, DM->sel[DUEL_LEVEL_TYPE] + 5);
            Snd_PlaySe(1, 1);
        } else {
            DM_LEVEL_GOTO("fl_ok");
            Snd_PlaySe(1, 1);
            ColorFade_StartOut(0, 0, 0, 0x14);
            sVitaNetGo = DM_CUR; /* 1 host, 2 join */
            sVitaNetWait = 0x12; /* (the fade's 20 frames end the screen: the game starts again just before) */
        }
    } else if (gPad[0].gamePressed & PADG_TRIANGLE) {
        char name[64];

        DM->idle = 0;
        DuelMenu_VitaNetClose();
        DM->level = DUEL_LEVEL_TOP;
        DM_LEVEL_GOTO("fl_on_start");
        sprintf(name, "fl_mode_%d_cansel", DM_CUR + 1);
        Flash_GotoLabel(&DM->flash[0], name, 1);
        DM_SAY(0, DM->sel[DUEL_LEVEL_TOP] + 1);
        Snd_PlaySe(1, 2);
    } else {
        DuelMenu_Idle();
    }
}
#endif

/* Pad 0 on the five levels of the menu. *result is cleared when the screen is left with cancel. */
void DuelMenu_Input(s32 *result) {
    if (!(DM->flash[0].flags & MFLASH_PAD)) {
        return;
    }
    if (!(DM->flags & DUELMENU_STARTED)) {
        DuelMenu_ClipGoto(DUEL_LEVEL_TOP, "fl_on_start");
        DM->flags |= DUELMENU_STARTED;
    }
#ifdef PORT_VITA
    if (DM->level == DUEL_LEVEL_TYPE && sVitaNet) {
        DuelMenu_VitaNetInput();
        return;
    }
#endif
    switch (DM->level) {
    case DUEL_LEVEL_TOP:
        if (gPad[0].gameRepeat & PADG_UP) {
            DM->idle = 0;
            DM_LEVEL_GOTO("fl_off_start");
            DM_CUR--;
#ifdef PORT_VITA
            if ((DM->flags & DUELMENU_NO_PAD2) && !Vita_VersusNetReady()) {
#else
            if (DM->flags & DUELMENU_NO_PAD2) {
#endif
                if (DM_CUR == 1) {
                    DM_CUR = 0;
                }
            }
            if (DM_CUR < 0) {
                DM_CUR = 3;
            }
            DM_LEVEL_GOTO("fl_on_start");
            DM_SAY(0, DM->sel[DUEL_LEVEL_TOP] + 1);
            Snd_PlaySe(1, 0);
        } else if (gPad[0].gameRepeat & PADG_DOWN) {
            DM->idle = 0;
            DM_LEVEL_GOTO("fl_off_start");
            DM_CUR++;
#ifdef PORT_VITA
            if ((DM->flags & DUELMENU_NO_PAD2) && !Vita_VersusNetReady()) {
#else
            if (DM->flags & DUELMENU_NO_PAD2) {
#endif
                if (DM_CUR == 1) {
                    DM_CUR = 2;
                }
            }
            if (DM_CUR >= 4) {
                DM_CUR = 0;
            }
            DM_LEVEL_GOTO("fl_on_start");
            DM_SAY(0, DM->sel[DUEL_LEVEL_TOP] + 1);
            Snd_PlaySe(1, 0);
        } else if (gPad[0].gamePressed & PADG_CROSS) {
            DM->idle = 0;
            switch (DM_CUR) {
            case 3: {
                char name[64];

                DM_LEVEL_GOTO("fl_ok");
                sprintf(name, "fl_mode_%d", DM_CUR + 1);
                Flash_GotoLabel(&DM->flash[0], name, 1);
                DM->level = DUEL_LEVEL_SETTING;
                IconWin_Close();
                MsgWin_SetText(DM->setText);
                DM->voiceLine = DM->sel[DUEL_LEVEL_SETTING] + 5;
                Voice_StopWithLip();
                DM_LEVEL_GOTO("fl_on_start");
                Snd_PlaySe(1, 1);
                break;
            }
            case 1:
#ifdef PORT_VITA
                if (Vita_VersusNetReady()) { /* (two controllers, or ad hoc: the choice) */
                    DuelMenu_VitaNetOpen();
                    break;
                }
#endif
                if (DM->flags & DUELMENU_NO_PAD2) {
                    Snd_PlaySe(1, 7);
                    break;
                }
            default: {
                char name[64];

                DM_LEVEL_GOTO("fl_ok");
                sprintf(name, "fl_mode_%d", DM_CUR + 1);
                Flash_GotoLabel(&DM->flash[0], name, 1);
                DM->level = DUEL_LEVEL_TYPE;
                DM_SAY(0, DM->sel[DUEL_LEVEL_TYPE] + 5);
                DM_LEVEL_GOTO("fl_on_start");
                Snd_PlaySe(1, 1);
                break;
            }
            }
        } else if (gPad[0].gamePressed & PADG_TRIANGLE) {
            DM->idle = 0;
            ColorFade_StartOut(0, 0, 0, 0x14);
            *result = 0;
            Flash_GotoLabel(&DM->flash[0], "fl_exit", 1);
            Snd_PlaySe(1, 2);
        } else {
            DuelMenu_Idle();
        }
        break;
    case DUEL_LEVEL_TYPE:
        if (gPad[0].gameRepeat & PADG_UP) {
            DM->idle = 0;
            DM_LEVEL_GOTO("fl_off_start");
            DM_CUR--;
            if (DM_CUR < 0) {
                DM_CUR = 2;
            }
            DM_LEVEL_GOTO("fl_on_start");
            DM_SAY(0, DM->sel[DUEL_LEVEL_TYPE] + 5);
            Snd_PlaySe(1, 0);
        } else if (gPad[0].gameRepeat & PADG_DOWN) {
            DM->idle = 0;
            DM_LEVEL_GOTO("fl_off_start");
            DM_CUR++;
            if (DM_CUR >= 3) {
                DM_CUR = 0;
            }
            DM_LEVEL_GOTO("fl_on_start");
            DM_SAY(0, DM->sel[DUEL_LEVEL_TYPE] + 5);
            Snd_PlaySe(1, 0);
        } else if (gPad[0].gamePressed & PADG_CROSS) {
            char name[64];

            DM->idle = 0;
            if (DM->sel[DUEL_LEVEL_TOP] == 1 && (DM->flags & DUELMENU_NO_PAD2)) {
                Snd_PlaySe(1, 7);
                break;
            }
            DM_LEVEL_GOTO("fl_ok");
            if (DM_CUR == 2) {
                DM->level = DUEL_LEVEL_DP;
                DM_LEVEL_GOTO("fl_on_start");
                sprintf(name, "fl_dp_%d", DM->sel[DUEL_LEVEL_TOP] + 1);
                Flash_GotoLabel(&DM->flash[0], name, 1);
            } else {
                DM->startState = 1;
            }
            Snd_PlaySe(1, 1);
        } else if (gPad[0].gamePressed & PADG_TRIANGLE) {
            char name[64];

            DM->idle = 0;
            DM_LEVEL_GOTO("fl_off_start");
            DM->level = DUEL_LEVEL_TOP;
            DM_LEVEL_GOTO("fl_on_start");
            sprintf(name, "fl_mode_%d_cansel", DM_CUR + 1);
            Flash_GotoLabel(&DM->flash[0], name, 1);
            DM_SAY(0, DM->sel[DUEL_LEVEL_TOP] + 1);
            Snd_PlaySe(1, 2);
        } else {
            DuelMenu_Idle();
        }
        break;
    case DUEL_LEVEL_DP:
        if (gPad[0].gameRepeat & PADG_UP) {
            DM->idle = 0;
            DM_LEVEL_GOTO("fl_off_start");
            DM_CUR--;
            if (DM_CUR < 0) {
                DM_CUR = 2;
            }
            DM_LEVEL_GOTO("fl_on_start");
            Snd_PlaySe(1, 0);
        } else if (gPad[0].gameRepeat & PADG_DOWN) {
            DM->idle = 0;
            DM_LEVEL_GOTO("fl_off_start");
            DM_CUR++;
            if (DM_CUR >= 3) {
                DM_CUR = 0;
            }
            DM_LEVEL_GOTO("fl_on_start");
            Snd_PlaySe(1, 0);
        } else if (gPad[0].gamePressed & PADG_CROSS) {
            DM->idle = 0;
            if (DM->sel[DUEL_LEVEL_TOP] == 1 && (DM->flags & DUELMENU_NO_PAD2)) {
                Snd_PlaySe(1, 7);
                break;
            }
            DM_LEVEL_GOTO("fl_ok");
            DM->startState = 1;
            Snd_PlaySe(1, 1);
        } else if (gPad[0].gamePressed & PADG_TRIANGLE) {
            char name[64];

            DM->idle = 0;
            DM_LEVEL_GOTO("fl_off_start");
            DM->level = DUEL_LEVEL_TYPE;
            DM_LEVEL_GOTO("fl_on_start");
            sprintf(name, "fl_dp_%d_cansel", DM->sel[DUEL_LEVEL_TOP] + 1);
            Flash_GotoLabel(&DM->flash[0], name, 1);
            Snd_PlaySe(1, 2);
        } else {
            DuelMenu_Idle();
        }
        break;
    case DUEL_LEVEL_SETTING:
        if (gPad[0].gameRepeat & PADG_UP) {
            DM->idle = 0;
            DM_LEVEL_GOTO("fl_off_start");
            DM_CUR--;
            if (DM_CUR < 0) {
                DM_CUR = 5;
            }
            DM_LEVEL_GOTO("fl_on_start");
            DM->voiceLine = DM->sel[DUEL_LEVEL_SETTING] + 5;
            Snd_PlaySe(1, 0);
        } else if (gPad[0].gameRepeat & PADG_DOWN) {
            DM->idle = 0;
            DM_LEVEL_GOTO("fl_off_start");
            DM_CUR++;
            if (DM_CUR >= 6) {
                DM_CUR = 0;
            }
            DM_LEVEL_GOTO("fl_on_start");
            DM->voiceLine = DM->sel[DUEL_LEVEL_SETTING] + 5;
            Snd_PlaySe(1, 0);
        } else if (gPad[0].gamePressed & PADG_CROSS) {
            DM->idle = 0;
            DM_LEVEL_GOTO("fl_ok");
            if (DM_CUR == 5) {
                DM->reset.state = 1;
            } else {
                DuelMenu_SetupValue();
                DM->level = DUEL_LEVEL_VALUE;
                DM_CUR = DM->sel[DUEL_LEVEL_SETTING];
                DM_LEVEL_GOTO("fl_on_start");
                if (DM_CUR == 3) {
                    DM->column ^= 1;
                    DM_LEVEL_GOTO("fl_ok");
                    DM->column ^= 1;
                }
                Flash_GotoLabel(&DM->flash[0], "fl_value", 1);
            }
            Snd_PlaySe(1, 1);
        } else if (gPad[0].gamePressed & PADG_TRIANGLE) {
            char name[64];

            DM->idle = 0;
            DM_LEVEL_GOTO("fl_off_start");
            DM->level = DUEL_LEVEL_TOP;
            DM_LEVEL_GOTO("fl_on_start");
            sprintf(name, "fl_mode_%d_cansel", DM_CUR + 1);
            Flash_GotoLabel(&DM->flash[0], name, 1);
            IconWin_Open();
            MsgWin_SetText(DM->msgText);
            DM_SAY(0, DM->sel[DUEL_LEVEL_TOP] + 1);
            Snd_PlaySe(1, 2);
        }
        break;
    case DUEL_LEVEL_VALUE:
        if (gPad[0].gameRepeat & PADG_UP) {
            DM->idle = 0;
            DM_LEVEL_GOTO("fl_off_start");
            DM->value[DM->column]--;
            if (DM->value[DM->column] < 0) {
                DM->value[DM->column] = DM->valueCount - 1;
            }
            DM_LEVEL_GOTO("fl_on_start");
            Snd_PlaySe(1, 0);
        } else if (gPad[0].gameRepeat & PADG_DOWN) {
            DM->idle = 0;
            DM_LEVEL_GOTO("fl_off_start");
            DM->value[DM->column]++;
            if (DM->value[DM->column] >= DM->valueCount) {
                DM->value[DM->column] = 0;
            }
            DM_LEVEL_GOTO("fl_on_start");
            Snd_PlaySe(1, 0);
        } else if (gPad[0].gameRepeat & (PADG_LEFT | PADG_RIGHT)) {
            DM->idle = 0;
            if (DM->sel[DUEL_LEVEL_SETTING] == 3) {
                DM_LEVEL_GOTO("fl_ok");
                DM->column ^= 1;
                DM_LEVEL_GOTO("fl_on_start");
                Snd_PlaySe(1, 0);
            }
        } else if (gPad[0].gamePressed & PADG_CROSS) {
            DM->idle = 0;
            DM_LEVEL_GOTO("fl_off_start");
            if (DM_CUR == 3) {
                DM->column ^= 1;
                DM_LEVEL_GOTO("fl_off_start");
                DM->column ^= 1;
                gSaveData->rule[DM->rule + 1] = DM->value[1];
            }
            gSaveData->rule[DM->rule] = DM->value[0];
            DM->level = DUEL_LEVEL_SETTING;
            DM_LEVEL_GOTO("fl_on_start");
            Flash_GotoLabel(&DM->flash[0], "fl_value_cansel", 1);
            Snd_PlaySe(1, 1);
        } else if (gPad[0].gamePressed & PADG_TRIANGLE) {
            DM->idle = 0;
            DM_LEVEL_GOTO("fl_off_start");
            if (DM_CUR == 3) {
                DM->column ^= 1;
                DM_LEVEL_GOTO("fl_off_start");
                DM->column ^= 1;
            }
            DM->level = DUEL_LEVEL_SETTING;
            DM_LEVEL_GOTO("fl_on_start");
            Flash_GotoLabel(&DM->flash[0], "fl_value_cansel", 1);
            Snd_PlaySe(1, 2);
        }
        break;
    }
}

/* After the last choice: a guide says a line, then (when it ends or confirm is pressed) the screen leaves. */
void DuelMenu_UpdateStart(void) {
    if (DM->startState == 0) {
        return;
    }
    switch (DM->startState) {
    case 1:
        switch (DM->level) {
        case DUEL_LEVEL_TYPE:
            switch (DM->sel[DUEL_LEVEL_TYPE]) {
            case 0:
                DM_SAY(0, 9);
                break;
            case 1:
                DM_SAY(1, 10);
                break;
            }
            break;
        case DUEL_LEVEL_DP:
            DM_SAY(0, 11);
            break;
        }
        DM->startState++;
        break;
    case 2:
        if (Voice_GetStat(0) == 5) {
            DM->startState++;
        } else if (gPad[0].gamePressed & PADG_CROSS) {
            DM->startState++;
            Snd_PlaySe(1, 1);
        }
        break;
    case 3:
        DM->flags |= DUELMENU_CHOSEN;
        DM->flags |= DUELMENU_LEAVING;
        DM->timer = 15;
        DM->startState = 0;
        break;
    }
}

/* The "restore the default settings?" question: on yes Save_ResetRules, then the dialog closes. */
void DuelMenu_UpdateReset(void) {
    DuelReset *r = &DM->reset;

    if (r->state == 0) {
        return;
    }
    switch (r->state) {
    case 1:
        r->state = 2;
        Dialog_SetLayout(0);
        Dialog_Start(0);
        Dialog_SetCursor(1);
        r->answer = 0;
    case 2:
        Dialog_SetChoices(1);
        Dialog_Start(0);
        r->msg = 1;
        if (r->answer > 0) {
            r->timer--;
            if (r->timer < 0) {
                Save_ResetRules();
                r->timer = 0x78;
                r->state = 3;
            }
        } else if (r->answer < 0) {
            Dialog_SetCursor(1);
            r->timer = 0x78;
            r->state = 3;
        } else {
            r->answer = Dialog_Input(1);
            r->timer = 12;
        }
        break;
    case 3:
        Dialog_SetChoices(0);
        DuelMenu_ClipGoto(DM->level, "fl_on_start");
        Dialog_Start(1);
        Dialog_Input(0);
        r->state = 4;
        break;
    case 4:
        Dialog_Input(0);
        if (Dialog_IsClosed()) {
            r->state = 0;
        }
        break;
    }
    Dialog_SetMsg(r->msg);
    Dialog_Draw(1);
}

/*
 * 0x356090..0x3562B8: the frame loop of the duel menu (modes 38..41), the last function of the object (it was
 * in the next chunk's file menu_h.c until the merge). DuelMenu_UpdateStart and DuelMenu_UpdateReset are called
 * where a second update and a second draw would be.
 */

/* Runs the screen until its fade out is over; returns what the input handler set (1 if it set nothing). */
s32 DuelMenu_Run(s32 section) {
    s32 result = 1;

    DuelMenu_Init(section);
    ColorFade_StartIn(0, 0, 0, 0x14);
    while (1) {
        Gfx_BeginFrame();
        Pad_Update();
        Snd_Update();
        ColorFade_Update();
        if (!(gProgress->flags & MPROG_FREEZE)) {
            DuelMenu_Update();
            DuelMenu_UpdateStart();
        }
        DuelMenu_Draw();
        DuelMenu_UpdateReset();
        ColorFade_Draw();
        Gfx_EndFrame(1);
        Dma_Flush();
        File_Stub264D90();
        if (gProgress->flags & MPROG_FREEZE) {
            continue;
        }
        if (ColorFade_IsInDone()) {
            if (!(gDuelMenu->flags & DUELMENU_GREETED) && (gDuelMenu->flash[0].flags & MFLASH_PAD)) {
                gDuelMenu->flags |= DUELMENU_GREETED;
                gDuelMenu->talker = 0;
                gDuelMenu->voiceLine = 0;
                Voice_PlayWithSubtitle(gDuelMenu->subtitles, DUEL_VOICE_BASE, gDuelMenu->voiceLine);
            }
        }
        if (ColorFade_IsFadingOut()) {
            Voice_FadeOutStep(0);
            Bgm_FadeOutStep();
            continue;
        }
        if (ColorFade_IsOutDone()) {
            break;
        }
        if (gDuelMenu->flags & DUELMENU_LEAVING) {
            if (--gDuelMenu->timer == -1) {
                ColorFade_StartOut(0, 0, 0, 0x14);
                DUEL_PROG->versus = gDuelMenu->sel[0];
                DUEL_PROG->battleType = gDuelMenu->sel[1];
                DUEL_PROG->dpLimit = gDuelMenu->sel[2];
            }
        } else if (gDuelMenu->startState == 0 && gDuelMenu->reset.state == 0) {
            DuelMenu_Input(&result);
        }
    }
    DuelMenu_Term();
    Dma_ResetBuffers();
    return result;
}
