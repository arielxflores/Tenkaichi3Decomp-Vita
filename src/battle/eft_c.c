#include "common.h"
#include "battle/eft_c.h"
#include "battle/battle.h"
#include "battle/btl_cam.h"
#include "battle/btl_pool.h"
#include "battle/btl_scene.h"
#include "sys/common.h"
#include "sys/dma.h"
#include "sys/gfx.h"
#include "sys/gfx_ot.h"

/*
 * Stage-attached effects, 0x136760..0x13A9D0. See include/battle/eft_c.h.
 * Everything in this file only builds draw data: no fighter, battle object or hit record is read or written.
 */

extern void *memset(void *dst, s32 c, u32 n);
extern s32 rand(void);
extern f32 sinf(f32 x);
extern f32 Mathf_Sin(f32 angle);

extern void Vec4_Set(Vec4 *dst, f32 x, f32 y, f32 z, f32 w);
extern void Vec4_Copy(Vec4 *dst, Vec4 *src);
extern void Vec4_Add(Vec4 *dst, Vec4 *a, Vec4 *b);
extern void Vec4_Sub(Vec4 *dst, Vec4 *a, Vec4 *b);
extern void Vec3_Add(Vec4 *dst, Vec4 *a, Vec4 *b);
extern void Vec3_Sub(Vec4 *dst, Vec4 *a, Vec4 *b);
extern void Vec3_Scale(Vec4 *dst, Vec4 *src, f32 s);
extern f32 Vec3_Dot(Vec4 *a, Vec4 *b);
extern void Mtx_MulVec4(Vec4 *dst, Mtx44 *m, Vec4 *v);
extern void Mtx_StoreIdentity(Mtx44 *m);
extern void Mtx_Copy(Mtx44 *dst, Mtx44 *src);  /* inverse of a rigid transform (inferred) */
extern void Mtx_InverseRT(Mtx44 *dst, Mtx44 *src);
extern f32 Vec3_Dist(Vec4 *a, Vec4 *b);         /* distance */
extern f32 Vec3_DistSq(Vec4 *a, Vec4 *b);         /* squared distance */
extern void Vu0Cur_Push(void);                    /* saves the VU0 matrix state */
extern void Vu0Cur_LoadMtx(Mtx44 *m);                /* loads a matrix into VU0 */
extern void Vu0Cur_Pop(void);                    /* restores the VU0 matrix state */
extern void Vu0Cur_ProjectInt(s32 *out, Vec4 *pos);     /* projects to GS screen coordinates */
extern void ClipVtx_Set(EftSurfVtx *out, f32 *pos, f32 *st, f32 *color);

extern void *BtlTask_CreateChildList(void *task, s32 capacity, s32 workSize);
extern void *BtlTaskList_AddTail(void *list, void *cls, void *arg);
extern void BtlTask_DestroyChildList(void *task);
extern s32 BtlStage_IsReady(void);

/* Stage records (stg_b.c). */
extern u8 *BtlStage_GetFxResA2(void);
extern u8 *BtlStage_GetFxResB2(void);
extern u8 *BtlStage_GetFxResC2(void);
extern u32 *BtlStage_GetList90(void);
extern StgWeatherRec *BtlStage_GetList60(void);
extern StgSurfRec *BtlStage_GetList58(void);
extern u8 *BtlStage_GetList80(void);
extern void BtlStage_GetWaterLevel(f32 *out);
extern void StgTint_GetColor0(s32 *out);
extern s32 StgTint_IsOn(s32 arg);

/* Texture tables (0x1AD..0x1AE). */
extern void EftTexSet_Load32(EftTexTbl *tbl, s32 *data);
extern void EftTexSet_Load4(EftTexEntry *tex, s32 *data);
extern void EftTexSet_Load34(EftSurfTexTbl *tbl, s32 *data);
extern void EftTexSet_Keep32(EftTexTbl *tbl, s32 a, s32 b);
extern void EftTexSet_Keep4(EftTexEntry *tex, s32 a, s32 b);
extern u64 EftVram_AddTex(EftSurfSlot *slot, s32 a, s32 b);
extern void GfxClut_InitPacket(EftSurfBuf *buf, s32 id);

/* Sprite draws of the effect core. */
extern void EftGfx_DrawSprite(Vec4 *pos, Vec4 *color, f32 w, f32 h, f32 u0, f32 v0, f32 u1, f32 v1, f32 rot, s32 a,
                          s32 b, u64 tex0);
#ifdef PORT /* PC build: parameters in the definition's order; the macro reorders the calls of this file */
extern void EftPrim_DrawQuadDepth(Vec4 *pos, Vec4 *color, s32 a, s32 b, u64 tex0, f32 w, f32 h, f32 u0, f32 v0, f32 u1, f32 v1, f32 rot);
#define EftPrim_DrawQuadDepth(pos, color, w, h, u0, v0, u1, v1, rot, a, b, tex0) EftPrim_DrawQuadDepth(pos, color, a, b, tex0, w, h, u0, v0, u1, v1, rot)
#else
extern void EftPrim_DrawQuadDepth(Vec4 *pos, Vec4 *color, f32 w, f32 h, f32 u0, f32 v0, f32 u1, f32 v1, f32 rot, s32 a,
                          s32 b, u64 tex0);
#endif

/* The geyser (eft_b.c) and the emitters it starts (eft_g.c smoke, eft_e.c steam). */
extern void EftGeyser_Update(EftTask *task);
extern s32 EftSmoke_Create(EftGeyserSmokeArg *arg);
extern s32 EftSmoke_GetPos(s32 handle);
extern s32 EftSteam_Create(EftGeyserSteamArg *arg);
extern void EftSteam_SetPaused(s32 handle, s32 on);
extern s32 EftSteam_GetWork(s32 handle);

extern void EftGndDust_InitTemplate(void);

/* Clip planes and helpers of the effect core. */
extern void EftGfx_UpdateClipPlanes(void);
extern Vec4 *EftGfx_GetClipPlanes(void);
extern s32 ClipPoly_ClipPlane(EftSurfVtx *poly, Vec4 *plane, s32 count); /* clips in place, returns the new count */
extern void ClipPoly_ProjectCur(s32 (*xyz)[4], Vec4 *st, EftSurfVtx *poly, s32 count); /* projects count vertices */
extern void EftMath_CalcTangentFrame(Mtx44 *out, EftVec *a, EftVec *b);
#ifdef PORT /* PC build: by value, as the definition takes it (the PS2 passes a struct of this size by address) */
extern void EftGfx_LightClutDiffuse(EftSurfClut *dst, EftSurfClut *src, Vec4 light, u8 r, u8 g, u8 b);
#define EftGfx_LightClutDiffuse(dst, src, light, r, g, b) EftGfx_LightClutDiffuse(dst, src, *(Vec4 *)(light), r, g, b)
#else
extern void EftGfx_LightClutDiffuse(EftSurfClut *dst, EftSurfClut *src, Vec4 *light, u8 r, u8 g, u8 b);
#endif
#ifdef PORT /* PC build: by value, as the definition takes it (the PS2 passes a struct of this size by address) */
extern void EftGfx_LightClutSpecular(EftSurfClut *dst, EftSurfClut *src, Mtx44 view, Mtx44 frame, Vec4 light, f32 k);
#define EftGfx_LightClutSpecular(dst, src, view, frame, light, k) EftGfx_LightClutSpecular(dst, src, *(Mtx44 *)(view), *(Mtx44 *)(frame), *(Vec4 *)(light), k)
#else
extern void EftGfx_LightClutSpecular(EftSurfClut *dst, EftSurfClut *src, Mtx44 *view, Mtx44 *frame, Vec4 *light, f32 k);
#endif
extern u64 EftVram_AddImage(EftTexEntry *tex, s32 a, s32 b);
extern u64 EftVram_AddClut(EftTexEntry *tex);

/* eft_d.c */
extern void EftSurf_DrawPolyOtClipped(EftSurfVtx *poly, s32 unused, u64 *tex, Vec4 *fog, s32 zBias);
extern void EftSurf_DrawTriDirect(s32 *xyz0, s32 *xyz1, s32 *xyz2, Vec4 *rgba0, Vec4 *rgba1, Vec4 *rgba2, Vec4 *st0,
                                  Vec4 *st1, Vec4 *st2, s32 a, u64 *tex, Vec4 *fog, s32 b);

/* Stage manager (stg_b.c); only the flag word is used here. */
typedef struct EftStageMgrView {
    /* 0x00 */ s32 unk0[2];
    /* 0x08 */ s32 flags; /* bit 0: the stage is not drawn */
} EftStageMgrView;
extern EftStageMgrView *gBtlStage;

/* Battle resources at gCommonRes + 0x20: only the stage pack. */
typedef struct EftCommonResView {
    /* 0x00 */ u8 unk0[0x24];
    /* 0x24 */ s32 *stage;
} EftCommonResView;
#define STAGE_PACK() (((EftCommonResView *)gCommonRes)->stage)

extern EftTaskClass gEftWeatherPtclClass;
extern EftStageKind gEftStageKinds[10];

static inline u8 EftTask_IsDead(EftTask *task) {
    return task->flags & 1;
}

/* Returns EftSmoke_GetPos of a live geyser's smoke emitter, 0 when the task is not a geyser or has none. */
s32 EftGeyser_GetSmokePos(EftTask *task) {
    s32 fx;

    if (task == NULL) {
        return 0;
    }
    if (task->cls->update != EftGeyser_Update) {
        return 0;
    }
    if (EftTask_IsDead(task)) {
        return 0;
    }
    fx = ((EftGeyserView *)task->work)->smoke;
    if (fx == 0) {
        return 0;
    }
    return EftSmoke_GetPos(fx);
}

/* Returns EftSteam_GetWork of a live geyser's steam emitter, 0 when the task is not a geyser or has none. */
s32 EftGeyser_GetSteamWork(EftTask *task) {
    s32 fx;

    if (task == NULL) {
        return 0;
    }
    if (task->cls->update != EftGeyser_Update) {
        return 0;
    }
    if (EftTask_IsDead(task)) {
        return 0;
    }
    fx = ((EftGeyserView *)task->work)->steam;
    if (fx == 0) {
        return 0;
    }
    return EftSteam_GetWork(fx);
}

/* Starts the geyser's smoke emitter with the colours of the stage's record. Two rand() calls (the
   description's unk38 and unk48). */
#if 0
/* Not matching: 17 of 162 instructions, all in the block that fills `init`. Same instructions and constants,
 * but the stores are scheduled in another order: the original emits them in field order except colorA.y (after
 * colorA.z) and colorB.y (last), this emits the last store of each shared constant first and the others at the
 * end; 2.0 and 0.985 then swap $f4 / $f5. Tried: field order permutations, chained assignments, an inline
 * setter, float-array fields, an initializer list.
 * Second pass (cleanup 2): the order is the first scheduling pass's, and it follows a simple rule that the natural
 * code obeys elsewhere (EftPrim_DrawBillboard's UV stores): among stores that are ready together, those whose
 * source register DIES there go first in source order, then the others in source order. With one register per
 * constant (which is what CSE makes of any spelling tried) that rule cannot give the original order: it would
 * need the x store of each vector (colorA.x, colorB.x, and pos.w / colorA.w) to be the last use of its own value.
 * So in the original the first component of each vector did not share its value with the others at that point.
 * A hill-climb over all orders of the 19 statements (build/scratch_cleanup2_A/search.py) stops at 11 differing
 * instructions, with an order that is not credible as source; the field-order attempt is kept. */
void EftGeyser_StartSmoke(EftTask *task) {
    EftGeyserSmokeArg arg;
    EftGeyserSmokeArg init;
    EftGeyserView *w = task->work;
    u8 *rec;
    f32 r = 0.0f;
    Vec4 *pos = &w->pos;

    init.pos.x = pos->x;
    init.pos.y = pos->y;
    init.pos.z = pos->z;
    init.pos.w = 1.0f;
    init.colorA.x = 32.0f;
    init.colorA.y = 32.0f;
    init.colorA.z = 32.0f;
    init.colorA.w = r;
    init.colorB.x = 192.0f;
    init.colorB.y = 192.0f;
    init.colorB.z = 192.0f;
    init.colorB.w = r;
    init.unk30 = 2.0f;
    init.unk34 = 64.0f;
    init.unk38 = 1.0f;
    init.unk3C = 0.985f;
    init.unk40 = 90;
    init.unk44 = 30;
    init.unk48 = 3;
    arg = init;
    rec = BtlStage_GetFxResB2();
    if (rec != NULL) {
        arg.colorA.x = rec[0x1C];
        arg.colorA.y = rec[0x1D];
        arg.colorA.z = rec[0x1E];
        arg.colorA.w = rec[0x1F];
        arg.colorB.x = rec[0x20];
        arg.colorB.y = rec[0x21];
        arg.colorB.z = rec[0x22];
        arg.colorB.w = rec[0x23];
        r = (f32)rand() / 2147483647.0f * 5.0f - 2.5f;
        arg.unk48 = rand() % 2 + 2;
        arg.unk38 = w->unkB4 / 150.0f + r;
        arg.unk30 = w->unkB0 / 10.0f * 2.0f;
        arg.unk40 = w->unkB4 / 150.0f + 90.0f;
        if (arg.unk38 < 0.0f) {
            arg.unk38 = -arg.unk38;
        }
        if (w->smoke == 0) {
            w->smoke = EftSmoke_Create(&arg);
        }
    }
}
#endif
INCLUDE_ASM("asm/nonmatchings/battle/eft_c", EftGeyser_StartSmoke);

/* Starts the geyser's steam emitter with the colour of the stage's record, and pauses it. */
#if 0
/* Not matching: 19 of 126 instructions, the same store-order problem as EftGeyser_StartSmoke: the original stores
 * color.x right after unk10.w and color.y / color.z / unk44 last; this stores color.x with them. The three
 * position loads then alternate $f1 / $f0 the other way round.
 * Second pass: see EftGeyser_StartSmoke. Here the original order is exactly "color.x has a value of its own,
 * color.y / z / w share another": then x and w are both last uses and go first, y / z / unk44 wait. No spelling
 * found gives two values (chained assignments in every grouping, copies of color.x, a brace initialiser: that one
 * also clears the tail and is 19 instructions longer). The memset + unk10.w pair does look like the compiler's
 * own expansion of a mostly-zero `{ 0, 0, 0, 1 }` constructor. The search over statement orders stops at 12. */
void EftGeyser_StartSteam(EftTask *task) {
    EftGeyserSteamArg arg;
    EftGeyserSteamArg init;
    EftGeyserView *w = task->work;
    u8 *rec;
    Vec4 *pos = &w->pos;

    init.pos.x = pos->x;
    init.pos.y = pos->y;
    init.pos.z = pos->z;
    init.pos.w = 1.0f;
    memset(&init.unk10, 0, sizeof(init.unk10));
    init.unk10.w = 1.0f;
    init.color.x = 128.0f;
    init.color.y = 128.0f;
    init.color.z = 128.0f;
    init.color.w = 128.0f;
    init.unk30 = 8.0f;
    init.gravity = 9.8f / 30.0f;
    init.unk38 = 4.8f;
    init.unk3C = 25;
    init.unk40 = 15.0f;
    init.unk44 = 0.5f;
    init.unk48 = 2.0f;
    init.unk4C = 0.5f;
    init.unk50 = 5;
    arg = init;
    rec = BtlStage_GetFxResC2();
    if (rec != NULL) {
        arg.color.x = rec[0x1A];
        arg.color.y = rec[0x1B];
        arg.color.z = rec[0x1C];
        arg.color.w = rec[0x1D];
        arg.unk30 = w->unkB4 / 150.0f + 8.0f;
        arg.unk38 = w->unkB0 / 10.0f * 4.8f;
        arg.unk3C = w->unkB4 / 150.0f + 25.0f;
        if (w->steam == 0) {
            w->steam = EftSteam_Create(&arg);
            EftSteam_SetPaused(w->steam, 1);
        }
    }
}
#endif
INCLUDE_ASM("asm/nonmatchings/battle/eft_c", EftGeyser_StartSteam);

/* Outcode of a clip-space point: which of the six planes +-w it is outside of. */
s32 EftWeather_ClipCode(Vec4 *v) {
    s32 code = 0;

    if (v->x > v->w) {
        code = 1;
    }
    if (v->x < -v->w) {
        code |= 2;
    }
    if (v->y > v->w) {
        code |= 4;
    }
    if (v->y < -v->w) {
        code |= 8;
    }
    if (v->z > v->w) {
        code |= 0x10;
    }
    if (v->z < -v->w) {
        code |= 0x20;
    }
    return code;
}

/* Task init: when the stage's weather record is on, allocates the state and adds the particle task. */
void EftWeather_Init(EftTask *task) {
    s32 *pack = (s32 *)BtlScene_GetStageData();
    StgWeatherRec *rec = BtlStage_GetList60();

    if (rec != NULL && (rec->flags & 1)) {
        gEftWeather = BtlPool_Alloc(BtlPool_GetCurrent(), sizeof(EftWeather));
        memset(gEftWeather, 0, sizeof(EftWeather));
        gEftWeather->angle = 0;
        gEftWeather->color[0] = 0x80;
        gEftWeather->color[1] = 0x80;
        gEftWeather->color[2] = 0x80;
        gEftWeather->color[3] = 0x80;
        gEftWeather->size = rec->size;
        gEftWeather->count = rec->count;
        gEftWeather->wind.x = rec->wind[0];
        gEftWeather->wind.y = rec->wind[1];
        gEftWeather->wind.z = rec->wind[2];
        gEftWeather->flags = rec->ptclFlags;
        EftTexSet_Load32(&gEftWeather->tex, BtlScene_GetPackEntry(pack, 0xB));
        gEftWeather->list = BtlTask_CreateChildList(task, 1, EFT_WEATHER_PTCL_MAX * sizeof(EftWeatherPtcl));
        gEftWeather->enabled = 1;
        BtlTaskList_AddTail(gEftWeather->list, &gEftWeatherPtclClass, NULL);
    }
}

/* Task term: frees the state. */
void EftWeather_Term(void) {
    if (gEftWeather != NULL) {
        BtlPool_Free(BtlPool_GetCurrent(), gEftWeather);
        gEftWeather = NULL;
    }
}

/* Task update: wind of this frame = base wind plus a sine on x and z; the angle advances 0..3 degrees (rand). */
void EftWeather_Update(void) {
    f32 angle;

    if (gEftWeather != NULL) {
        angle = (f32)gEftWeather->angle * 3.14159265f / 180.0f;
        Vec4_Copy(&gEftWeather->windNow, &gEftWeather->wind);
        gEftWeather->windNow.x += sinf(angle) * 0.3f;
        gEftWeather->windNow.z += sinf(angle) * 0.3f;
        gEftWeather->angle = (gEftWeather->angle + (rand() & 3)) % 360;
    }
}

/* Particle task init: scatters the particles. */
void EftWeatherPtcl_Init(EftTask *task) {
    EftWeatherPtcl_Reset(task);
}

/* Particle task term: nothing. */
void EftWeatherPtcl_Term(void) {
}

/* Classifies the camera movement since the last frame: 2 = jumped (more than 15 units), 1 = moving (more than
   4, or still settling), and keeps the per-view settle counter. */
static inline void EftWeather_TrackCam(Vec4 *cam, EftWeatherView *view, s32 second, s32 *moving) {
    f32 dist = Vec3_Dist(cam, &view->camPos[second]);

    if (dist > 15.0f) {
        *moving = 2;
        view->moveCnt[second] = 0;
    } else if (dist > 4.0f) {
        *moving = 1;
        if (view->moveCnt[second] < 15) {
            view->moveCnt[second]++;
        }
    } else if (view->moveCnt[second] != 0) {
        view->moveCnt[second]--;
        *moving = 1;
    }
}

/* Moves the particles of one view. Each particle random-walks its velocity (3 rand()), drifts with the wind,
   and wraps around an 80-unit box centred 55 units in front of the camera. The alpha fades with the distance
   to the camera, after a wrap, while the camera moves, with height, near y = 0 and with the stage tint.
   Camera-dependent and called from the draw callback: purely visual. */
void EftWeather_Move(EftWeatherPtcl *p, EftWeatherView *view, s32 count) {
    Vec4 offs;
    Vec4 pos;
    Vec4 cam;
    Mtx44 m;
    Mtx44 inv;
    s32 tint[4];
    s32 second = BtlScene_IsSecondView();
    s32 moving = 0;
    s32 flags;
    f32 size;
    f32 a;
    f32 y;
    s32 code;
    s32 n;
    s32 i;

    flags = gEftWeather->flags;
    size = gEftWeather->size;
    Vec4_Set(&offs, 0.0f, 0.0f, 55.0f, 0.0f);
    Mtx_StoreIdentity(&inv);
    if (gBtlCamView != NULL) {
        Mtx_Copy(&m, &gBtlCamView->world2view2);
        Mtx_InverseRT(&inv, &m);
    }
    Vec4_Copy(&cam, (Vec4 *)inv.m[3]);
    EftWeather_TrackCam(&cam, view, second, &moving);
    Vec4_Copy(&view->camPos[second], &cam);
    Vec3_Sub((Vec4 *)m.m[3], (Vec4 *)m.m[3], &offs);
    for (i = 0; i < count; i++) {
        Vec4 *q;

        p->vel.x += (f32)((rand() & 0x1FF) - 0xFF) * (1.0f / 255.0f) * 0.02f;
        p->vel.y += (f32)((rand() & 0x1FF) - 0xFF) * (1.0f / 255.0f) * 0.02f;
        p->vel.z += (f32)((rand() & 0x1FF) - 0xFF) * (1.0f / 255.0f) * 0.02f;
        if (p->vel.x < -0.4f) {
            p->vel.x = -0.4f;
        }
        if (p->vel.y < -0.4f) {
            p->vel.y = -0.4f;
        }
        if (p->vel.z < -0.4f) {
            p->vel.z = -0.4f;
        }
        if (p->vel.x > 0.4f) {
            p->vel.x = 0.4f;
        }
        if (p->vel.y > 0.4f) {
            p->vel.y = 0.4f;
        }
        if (p->vel.z > 0.4f) {
            p->vel.z = 0.4f;
        }
        Vec4_Copy(&pos, &p->pos);
        Vec4_Add(&pos, &pos, &gEftWeather->windNow);
        n = 0;
        Vec4_Add(&pos, &pos, &p->vel);
        Mtx_MulVec4(&pos, &m, &pos);
        pos.w = 40.0f;
        q = &pos;
        code = EftWeather_ClipCode(&pos);
        if (code != 0) {
            f32 wrap = -80.0f;

            do {
                p->wrap = 3;
                if (code & 1) {
                    pos.x += wrap;
                }
                if (code & 2) {
                    pos.x -= wrap;
                }
                if (code & 4) {
                    pos.y += wrap;
                }
                if (code & 8) {
                    pos.y -= wrap;
                }
                if (code & 0x10) {
                    pos.z += wrap;
                }
                if (code & 0x20) {
                    pos.z -= wrap;
                }
                code = EftWeather_ClipCode(q);
                if (n > 0x40) {
                    break;
                }
                n++;
            } while (code != 0);
        }
        if (moving == 2) {
            p->wrap = 0;
        }
        Vec3_Add(&pos, &pos, &offs);
        pos.w = 1.0f;
        Mtx_MulVec4(&pos, &inv, &pos);
        y = pos.y;
        if ((flags & 2) && y >= 0.0f) {
            y = 0.3f * -500.0f;
        }
        if (y < -500.0f) {
            y = -500.0f;
        }
        y = 1.0f - y / -500.0f;
        a = Vec3_Dist(&cam, q) * (1.0f / 110.0f);
        if (a > 1.0f) {
            a = 0.0f;
        } else if (a < 0.5f) {
            if (a <= 0.1f) {
                a = 0.0f;
            } else {
                a = (a - 0.1f) * 2.5f;
            }
        } else {
            a = 1.0f - (a - 0.5f) * 2.0f;
        }
        if (p->wrap != 0) {
            a *= (f32)(3 - p->wrap) / 3.0f;
            p->wrap--;
        }
        if (moving == 1 && view->moveCnt[second] != 0) {
            a *= (f32)(30 - view->moveCnt[second]) / 30.0f;
        }
        a *= y;
        if (flags & 1) {
            if (pos.y + (size + 4.0f) > 0.0f) {
                if (pos.y + size < 0.0f) {
                    a *= -((pos.y + size) * 0.25f);
                } else {
                    a = 0.0f;
                }
            }
        }
        if (StgTint_IsOn(0)) {
            s32 r = gEftWeather->color[0];
            s32 g = gEftWeather->color[1];
            s32 b = gEftWeather->color[2];
            f32 max = 100.0f;
            u8 *rec = BtlStage_GetList80();
            f32 k = 1.0f;

            if (rec != NULL) {
                max = rec[7];
            }
            StgTint_GetColor0(tint);
            if (tint[3] != 0) {
                g -= tint[3];
                b -= tint[3];
                r -= tint[3];
                if (r < 0) {
                    r = 0;
                }
                if (g < 0) {
                    g = 0;
                }
                if (b < 0) {
                    b = 0;
                }
                k -= (f32)tint[3] / max;
            }
            Vec4_Set(&p->color, r, g, b, a * 128.0f * k);
        } else {
            Vec4_Set(&p->color, 128.0f, 128.0f, 128.0f, a * 128.0f);
        }
        Vec4_Copy(&p->pos, q);
        p++;
    }
}

/* Particle task update: refreshes the weather textures. */
void EftWeatherPtcl_Update(void) {
    EftTexSet_Keep32(&gEftWeather->tex, 1, 0);
}

/* Particle task post-update: nothing. */
void EftWeatherPtcl_PostUpdate(void) {
}

/* Particle task reset: random position within +-51 units of the camera, random texture (4 rand() each). */
void EftWeatherPtcl_Reset(EftTask *task) {
    EftWeatherPtcl *p = task->work;
    s32 i;

    for (i = EFT_WEATHER_PTCL_MAX - 1; i >= 0; i--) {
        p->pos.x = (f32)((rand() & 0x3FF) - 0x1FF) * 0.1f;
        p->pos.y = (f32)((rand() & 0x3FF) - 0x1FF) * 0.1f;
        p->pos.z = (f32)((rand() & 0x3FF) - 0x1FF) * 0.1f;
        p->pos.w = 1.0f;
        p->vel.x = 0.0f;
        p->vel.y = 0.0f;
        p->vel.z = 0.0f;
        p->vel.w = 0.0f;
        Vec4_Set(&p->color, 128.0f, 128.0f, 128.0f, 0.0f);
        p->wrap = 0;
        p->tex = rand() % gEftWeather->tex.count;
        p++;
    }
    gEftWeather->enabled = 1;
}

/* Draws count particles as sprites of size * 16. */
void EftWeather_DrawPtcls(EftWeatherPtcl *p, s32 count) {
    EftTexTbl *tex = &gEftWeather->tex;
    s32 i;

    Vu0Cur_Push();
    Vu0Cur_LoadMtx(&gBtlCamView->world2screen);
    for (i = 0; i < count; i++) {
        EftGfx_DrawSprite(&p->pos, &p->color, gEftWeather->size * 16.0f, gEftWeather->size * 16.0f, 0.0f, 0.0f, 1.0f,
                      1.0f, 0.0f, 0, 0, (p->tex + tex->tex)->tex0);
        p++;
    }
    Vu0Cur_Pop();
}

/* Particle task draw: moves this view's particles (unless paused), then draws them. In split-screen each
   view owns half of the particles. */
void EftWeatherPtcl_Draw(EftTask *task) {
    s32 second = BtlScene_IsSecondView();
    EftWeatherPtcl *p = task->work;
    s32 count = gEftWeather->count;

    if (gEftWeather->enabled) {
        if (!(Battle_GetWork()->flags & 0x100)) {
            if (Battle_IsSplitScreen()) {
                count /= 2;
                if (!second) {
                    EftWeather_Move(p, &gEftWeather->view, count);
                } else {
                    p += EFT_WEATHER_PTCL_MAX / 2;
                    EftWeather_Move(p, &gEftWeather->view, count);
                }
            } else {
                EftWeather_Move(task->work, &gEftWeather->view, count);
            }
        }
        if (!(gBtlStage->flags & 1)) {
            if (EftStage_IsDrawOn()) {
                EftWeather_DrawPtcls(p, count);
            }
        }
    }
}

/* Sets the number of particles. */
void EftWeather_SetCount(s32 count) {
    if (gEftWeather != NULL) {
        gEftWeather->count = count;
    }
}

/* Returns the number of particles. */
s32 EftWeather_GetCount(void) {
    if (gEftWeather != NULL) {
        return gEftWeather->count;
    }
    return 0;
}

/* Sets the particle flags. */
void EftWeather_SetFlags(s32 flags) {
    if (gEftWeather != NULL) {
        gEftWeather->flags = flags;
    }
}

/* Returns the particle flags. */
s32 EftWeather_GetFlags(void) {
    if (gEftWeather != NULL) {
        return gEftWeather->flags;
    }
    return 0;
}

/* Returns the base wind vector, NULL without weather. */
Vec4 *EftWeather_GetWindPtr(void) {
    if (gEftWeather == NULL) {
        return NULL;
    }
    return &gEftWeather->wind;
}

/* Sets the particle size. */
void EftWeather_SetSize(f32 size) {
    if (gEftWeather != NULL) {
        gEftWeather->size = size;
    }
}

/* Returns the particle size. */
f32 EftWeather_GetSize(void) {
    if (gEftWeather != NULL) {
        return gEftWeather->size;
    }
    return 0.0f;
}

/* Sets the weather colour. */
void EftWeather_SetColor(u8 r, u8 g, u8 b) {
    if (gEftWeather != NULL) {
        gEftWeather->color[0] = r;
        gEftWeather->color[1] = g;
        gEftWeather->color[2] = b;
    }
}

/* Returns the weather colour, alpha 0x80. */
void EftWeather_GetColor(s32 *out) {
    if (gEftWeather != NULL) {
        out[0] = gEftWeather->color[0];
        out[1] = gEftWeather->color[1];
        out[2] = gEftWeather->color[2];
        out[3] = 0x80;
    }
}

/* Sets the base wind. */
void EftWeather_SetWind(f32 x, f32 y, f32 z) {
    if (gEftWeather != NULL) {
        gEftWeather->wind.x = x;
        gEftWeather->wind.y = y;
        gEftWeather->wind.z = z;
    }
}

/* Returns the base wind. */
void EftWeather_GetWind(Vec4 *out) {
    if (gEftWeather != NULL) {
        Vec4_Set(out, gEftWeather->wind.x, gEftWeather->wind.y, gEftWeather->wind.z, 1.0f);
    }
}

/* Same as EftWeather_SetCount. */
void EftWeather_SetCount2(s32 count) {
    if (gEftWeather != NULL) {
        gEftWeather->count = count;
    }
}

/* Same as EftWeather_GetCount. */
s32 EftWeather_GetCount2(void) {
    if (gEftWeather != NULL) {
        return gEftWeather->count;
    }
    return 0;
}

/* Same as EftWeather_SetFlags. */
void EftWeather_SetFlags2(s32 flags) {
    if (gEftWeather != NULL) {
        gEftWeather->flags = flags;
    }
}

/* Same as EftWeather_GetFlags. */
s32 EftWeather_GetFlags2(void) {
    if (gEftWeather != NULL) {
        return gEftWeather->flags;
    }
    return 0;
}

/* Turns the particles on or off. */
void EftWeather_SetEnabled(s32 on) {
    if (gEftWeather != NULL) {
        gEftWeather->enabled = on;
    }
}

/* Destroys and recreates layer 0 of the effect scene (all stage effects). */
void EftStage_Recreate(void) {
    BtlScene_FreeLayer0();
    BtlScene_CreateLayer0();
    EftGndDust_InitTemplate();
}

/* Resets every effect task and frees the stage effect tasks. */
void EftStage_ResetAll(void) {
    BtlScene_Reset(0);
    EftStage_FreeKinds();
}

/* Returns 1 when the stage effects are to be drawn. */
s32 EftStage_IsDrawOn(void) {
    s32 ret = 0;

    if (gEftStage != NULL) {
        ret = (gEftStage->flags >> 1) & 1;
    }
    return ret;
}

/* Turns drawing of the stage effects on or off. */
void EftStage_SetDrawOn(s32 on) {
    if (gEftStage != NULL) {
        if (on) {
            gEftStage->flags |= EFT_STAGE_FLAG_DRAW;
        } else {
            gEftStage->flags &= ~EFT_STAGE_FLAG_DRAW;
        }
    }
}

/* Layer-0 task init: state from pool slot 2, one child task per stage effect kind, drawing on. */
void EftStage_Init(EftTask *task) {
    BtlPool_SetCurrent(2);
    gEftStage = BtlPool_Alloc(BtlPool_GetCurrent(), sizeof(EftStage));
    memset(gEftStage, 0, sizeof(EftStage));
    gEftStage->task = task;
    EftStage_CreateKinds();
    EftStage_SetDrawOn(1);
}

/* Layer-0 task term: frees the state and empties pool slot 2. */
void EftStage_Term(void) {
    BtlPool_SetCurrent(2);
    BtlPool_Free(BtlPool_GetCurrent(), gEftStage);
    gEftStage = NULL;
    BtlPool_Reset(2);
}

/* Layer-0 task update: nothing. */
void EftStage_Update(void) {
}

/* Destroys the child tasks of the layer. */
void EftStage_FreeKinds(void) {
    if (gEftStage->list != NULL) {
        BtlPool_SetCurrent(2);
        BtlTask_DestroyChildList(gEftStage->task);
        gEftStage->list = NULL;
    }
}

/* Creates the child list and one task for every kind the stage has data for; loads the sprite. */
void EftStage_CreateKinds(void) {
    EftStageKind *kind;
    s32 i;

    if (gEftStage->list == NULL) {
        gEftStage->list = BtlTask_CreateChildList(gEftStage->task, 10, 0);
        kind = gEftStageKinds;
        for (i = 9; i >= 0; i--) {
            if (kind->cls != NULL) {
                if (EftStage_HasKind(kind->kind)) {
                    BtlTaskList_AddTail(gEftStage->list, kind->cls, NULL);
                }
            }
            kind++;
        }
        EftStage_LoadSprite();
    }
}

/* Returns 1 when the loaded stage has the data a stage effect kind needs (-1: always). */
s32 EftStage_HasKind(s32 kind) {
    s32 *pack = STAGE_PACK();
    s32 ret = 0;

    if (kind == -1) {
        return 1;
    }
    switch (kind) {
    case 0:
        if (BtlScene_GetPackEntrySize(pack, 7) > 0) {
            ret = 1;
        }
        break;
    case 2:
        if (BtlScene_GetPackEntrySize(pack, 0xB) > 0) {
            ret = 1;
        }
        break;
    case 1:
        if (BtlScene_GetPackEntrySize(pack, 0xD) > 0) {
            ret = 1;
        }
        break;
    case 3:
        if (BtlScene_GetPackEntrySize(pack, 0xE) > 0) {
            ret = 1;
        }
        break;
    case 4:
        if (BtlStage_GetFxResB2() != NULL) {
            ret = 1;
        }
        break;
    case 5:
        if (BtlStage_GetFxResC2() != NULL) {
            ret = 1;
        }
        break;
    case 6:
        if (BtlStage_GetFxResA2() != NULL) {
            ret = 1;
        }
        break;
    case 7:
        if (BtlStage_GetList90() != NULL) {
            if (*BtlStage_GetList90() & 1) {
                if (BtlScene_GetPackEntrySize(pack, 0x12) > 0) {
                    ret = 1;
                }
            }
        }
        break;
    case 8:
        if (BtlScene_GetPackEntrySize(pack, 0x15) > 0) {
            if (BtlScene_GetPackEntrySize(pack, 0x16) > 0) {
                ret = BtlScene_GetPackEntrySize(pack, 0x17) > 0;
            }
        }
        break;
    }
    return ret;
}

/* Nothing. */
void EftStage_Nop(void) {
}

/* Brightness factor from the stage tint: 1 - alpha / 100, kept within 0.5..1. */
f32 EftStage_GetTintScale(void) {
    s32 color[4];
    f32 scale = 1.0f;

    StgTint_GetColor0(color);
    scale -= (f32)color[3] / 100.0f;
    if (scale > 1.0f) {
        scale = 1.0f;
    }
    if (scale < 0.5f) {
        scale = 0.5f;
    }
    return scale;
}

/* Loads the stage's single sprite texture (pack entry 0x14) when it exists. Declared with a result it never
   sets: the original does not turn the last call into a tail call. */
s32 EftStage_LoadSprite(void) {
    s32 *pack = STAGE_PACK();

    if (BtlStage_IsReady()) {
        if (BtlScene_GetPackEntrySize(pack, 0x14) > 0) {
            gEftStage->flags |= EFT_STAGE_FLAG_SUN;
        } else {
            gEftStage->flags &= ~EFT_STAGE_FLAG_SUN;
        }
        if (gEftStage->flags & EFT_STAGE_FLAG_SUN) {
            EftTexSet_Load4(&gEftStage->sprite, BtlScene_GetPackEntry(pack, 0x14));
        }
    }
}

/* Refreshes the sprite texture. */
void EftStage_UpdateSprite(void) {
    if (BtlStage_IsReady() && (gEftStage->flags & EFT_STAGE_FLAG_SUN)) {
        EftTexSet_Keep4(&gEftStage->sprite, 1, 0);
    }
}

/* Draws the sprite, 16 units wide, at a world position. */
void EftStage_DrawSprite(Vec4 *pos) {
    if (BtlStage_IsReady() && (gEftStage->flags & EFT_STAGE_FLAG_SUN)) {
        EftVec color = { 128.0f, 128.0f, 128.0f, 128.0f };
        f32 size = 16.0f;

        Vu0Cur_Push();
        Vu0Cur_LoadMtx(&gBtlCamView->world2screen);
        EftPrim_DrawQuadDepth(pos, (Vec4 *)&color, size, size, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0, 0, gEftStage->sprite.tex0);
        Vu0Cur_Pop();
    }
}

/* Surface task init: locates the surface file in the stage pack and, when a mesh uses the reflection, sets up
   the palette renderer. */
void EftSurf_Init(void) {
    s32 *pack = STAGE_PACK();
    EftSurfParam *param = (EftSurfParam *)((u8 *)pack + ((u32)pack[0xD] >> 2 << 2));
    s32 size = pack[0xE] - pack[0xD];
    EftSurfRt *rt;
    EftSurfFile *file;
    EftSurfMesh *mesh;
    StgSurfRec *rec;
    s32 count;
    s32 i;

    gEftSurf = BtlPool_Alloc(BtlPool_GetCurrent(), sizeof(EftSurf));
    memset(gEftSurf, 0, sizeof(EftSurf));
    gEftSurf->param = param;
    gEftSurf->file = (EftSurfFile *)(param + 1);
    gEftSurf->u.s.size = size;
    rt = &gEftSurf->rt;
    EftSurf_Relocate(gEftSurf->file);
    file = gEftSurf->file;
    count = file->meshCount;
    mesh = file->meshes;
    for (i = 0; i < count; i++, mesh++) {
        if (mesh->flags & EFT_SURF_REFLECT) {
            if (!(rt->flags & EFT_SURF_RT_READY)) {
                EftTexSet_Load34(&rt->tex, (s32 *)((u8 *)pack + ((u32)pack[0xA] >> 2 << 2)));
                GfxClut_InitPacket(&rt->buf[0], 0x3E80);
                GfxClut_InitPacket(&rt->buf[1], 0x3E84);
                GfxClut_InitPacket(&rt->buf[2], 0x3E88);
                rt->animCount = rt->tex.count - 4;
                rt->texA = rt->tex.count - 4;
                rt->texB = rt->tex.count - 2;
                rt->param = *gEftSurf->param;
                rec = BtlStage_GetList58();
                if (rec != NULL) {
                    rt->param.colorA[0] = rec->colorA[0];
                    rt->param.colorA[1] = rec->colorA[1];
                    rt->param.colorA[2] = rec->colorA[2];
                    rt->param.colorA[3] = rec->colorA[3];
                    rt->param.colorB[0] = rec->colorB[0];
                    rt->param.colorB[1] = rec->colorB[1];
                    rt->param.colorB[2] = rec->colorB[2];
                    rt->param.colorB[3] = rec->colorB[3];
                    rt->param.lightDir.x = rec->lightDir[0];
                    rt->param.lightDir.y = rec->lightDir[1];
                    rt->param.lightDir.z = rec->lightDir[2];
                    rt->param.lightDir.w = 1.0f;
                    rt->param.unk30 = rec->unkC;
                    rt->param.animSpeed = rec->animSpeed;
                    rt->param.fadeDist = rec->fadeDist;
                }
                if (rt->param.fadeDist == 0.0f) {
                    rt->param.fadeDist = 1000.0f;
                }
                rt->texTbl = rt->tex.tex;
                rt->flags |= EFT_SURF_RT_READY | EFT_SURF_RT_ON;
            }
        }
    }
}

/* Surface task term: turns the file's pointers back into offsets and frees the state. */
void EftSurf_Term(void) {
    EftSurf_Unrelocate(gEftSurf->file);
    BtlPool_Free(BtlPool_GetCurrent(), gEftSurf);
    gEftSurf = NULL;
}

/* Surface task update: scrolls the meshes (unless paused) and refreshes the textures. */
void EftSurf_Update(void) {
    if (BtlStage_IsReady()) {
        if (!(Battle_GetWork()->flags & 0x100)) {
            EftSurf_Animate(gEftSurf->file);
        }
        EftSurf_UpdateTextures(gEftSurf->file);
    }
}

/* Surface task draw. */
void EftSurf_Draw(void) {
#ifdef PORT_VITA
    extern int gVitaSeaOff;
    if (gVitaSeaOff) {
        return; /* (a test's A/B only: bt3.txt seaoff=1, the stage's surfaces not drawn) */
    }
#endif
    if (!(gBtlStage->flags & 1) && BtlStage_IsReady() && EftStage_IsDrawOn()) {
        EftSurf_DrawMeshes();
    }
}

/* Surface task post-update: nothing. */
void EftSurf_PostUpdate(void) {
}

/* Surface task reset: nothing. */
void EftSurf_Reset(void) {
}

/* Turns the file offsets of a surface file into pointers. A file that is not version 4 is emptied. */
void EftSurf_Relocate(EftSurfFile *file) {
    EftSurfTex *tex;
    EftSurfMesh *mesh;
    s32 i;

    file->unkC += (s32)file;
    file->texs = (EftSurfTex *)((s32)file->texs + (s32)file);
    file->meshes = (EftSurfMesh *)((s32)file->meshes + (s32)file);
    if (file->version != 4) {
        file->meshCount = 0;
        file->texCount = 0;
        return;
    }
    tex = file->texs;
    for (i = 0; i < file->texCount; i++) {
        tex->ptr0 = tex->ofs0 + (s32)file;
        tex->ptr4 = tex->ofs4 + (s32)file;
        tex++;
    }
    mesh = file->meshes;
    for (i = 0; i < file->meshCount; i++) {
        mesh->tris = (EftSurfTri *)((s32)mesh->tris + (s32)file);
        mesh++;
    }
}

/* Turns the pointers of a surface file back into offsets. (The texture pointers come out negated: file - ofs.) */
void EftSurf_Unrelocate(EftSurfFile *file) {
    EftSurfTex *tex;
    EftSurfMesh *mesh;
    s32 i;

    tex = file->texs;
    for (i = 0; i < file->texCount; i++) {
        tex->ptr0 = (s32)file - tex->ofs0;
        tex->ptr4 = (s32)file - tex->ofs4;
        tex++;
    }
    mesh = file->meshes;
    for (i = 0; i < file->meshCount; i++) {
        mesh->tris = (EftSurfTri *)((s32)mesh->tris - (s32)file);
        mesh++;
    }
    file->unkC -= (s32)file;
    file->texs = (EftSurfTex *)((s32)file->texs - (s32)file);
    file->meshes = (EftSurfMesh *)((s32)file->meshes - (s32)file);
    if (file->version != 4) {
        file->meshCount = 0;
        file->texCount = 0;
    }
}

/* Gets this frame's TEX0 of every texture of the file; allows the palettes to be built again. */
void EftSurf_UpdateTextures(EftSurfFile *file) {
    EftSurfRt *rt = &gEftSurf->rt;
    EftSurfTex *tex;
    s32 i;

    tex = file->texs;
    for (i = 0; i < file->texCount; i++) {
        gEftSurf->u.slot[i + 1].def = tex;
        gEftSurf->u.slot[i + 1].unk0 = tex->unk30;
        tex->tex0 = EftVram_AddTex(&gEftSurf->u.slot[i + 1], 1, 0);
        tex++;
    }
    if (rt->flags & EFT_SURF_RT_ON) {
        rt->flags &= ~EFT_SURF_RT_BUILT;
    }
}

/* Advances the texture scroll of every mesh by its per-frame step. */
void EftSurf_Animate(EftSurfFile *file) {
    EftSurfRt *rt = &gEftSurf->rt;
    s32 count = file->meshCount;
    EftSurfMesh *mesh = file->meshes;
    s32 i;

    rt->flags &= ~EFT_SURF_RT_STEPPED;
    for (i = 0; i < count; i++, mesh++) {
        if (mesh->flags & EFT_SURF_PULSE) {
            mesh->v += mesh->dv;
            if (mesh->dv == 0.0f) {
                mesh->dv = -0.001f;
                mesh->v = 1.0f;
            }
            if (mesh->v > 1.0f) {
                mesh->v = 1.0f;
                mesh->dv = -mesh->dv;
            }
            if (mesh->v < 0.95f) {
                mesh->v = 0.95f;
                mesh->dv = -mesh->dv;
            }
            mesh->u += mesh->du;
            if (mesh->u < 0.0f) {
                mesh->u = 0.0f;
            }
            if (mesh->u > 6.2831853f) {
                mesh->u = -6.2831853f;
            }
        } else if (mesh->flags & EFT_SURF_WAVE) {
            mesh->v += mesh->dv;
            mesh->u += mesh->du;
            if (mesh->v > 1.0f) {
                mesh->v -= 1.0f;
            }
            if (mesh->u > 6.2831853f) {
                mesh->u = -6.2831853f;
            }
        } else if (mesh->flags & EFT_SURF_REFLECT) {
            if (!(rt->flags & EFT_SURF_RT_STEPPED)) {
                EftSurf_StepAnim();
                rt->flags |= EFT_SURF_RT_STEPPED;
            }
        } else {
            mesh->u += mesh->du;
            mesh->v += mesh->dv;
            if (mesh->u > 1.0f) {
                mesh->u -= 1.0f;
            }
            if (mesh->v > 1.0f) {
                mesh->v -= 1.0f;
            }
        }
    }
}

/* Projects the eight corners of a box. Returns 1 when one corner is on screen or the corners lie on several
   sides of it, 0 when the box is certainly outside the view. */
s32 EftSurf_IsBoxVisible(Vec4 *min, Vec4 *max) {
    Vec4 corner[8];
    s32 sc[4];
    s32 count[5];
    s32 all = 0;
    s32 i;
    s16 code;

    memset(count, 0, sizeof(count));
    Vec4_Set(&corner[0], min->x, min->y, min->z, 1.0f);
    Vec4_Set(&corner[1], max->x, min->y, min->z, 1.0f);
    Vec4_Set(&corner[2], min->x, max->y, min->z, 1.0f);
    Vec4_Set(&corner[3], max->x, max->y, min->z, 1.0f);
    Vec4_Set(&corner[4], min->x, min->y, max->z, 1.0f);
    Vec4_Set(&corner[5], max->x, min->y, max->z, 1.0f);
    Vec4_Set(&corner[6], min->x, max->y, max->z, 1.0f);
    Vec4_Set(&corner[7], max->x, max->y, max->z, 1.0f);
    for (i = 0; i < 8; i++) {
        Vu0Cur_ProjectInt(sc, &corner[i]);
        sc[0] -= 0x700;
        sc[1] -= 0x720;
        if (sc[2] < 0) {
            count[4]++;
            code = 0x10;
        } else {
            code = 0x20;
            if (sc[0] < gBtlCamView->scissorX0) {
                count[0]++;
                code = 0x21;
            }
            if (gBtlCamView->scissorX1 + 1 < sc[0]) {
                count[1]++;
                code |= 2;
            }
            if (sc[1] < gBtlCamView->scissorY0) {
                count[2]++;
                code |= 4;
            }
            if (gBtlCamView->scissorY1 + 1 < sc[1]) {
                count[3]++;
                code |= 8;
            }
        }
        all |= code;
        if ((code & ~0x20) == 0) {
            return 1;
        }
    }
    if ((all & 1) + ((all & 2) >> 1) + ((all & 4) >> 2) + ((all & 8) >> 3) >= 2) {
        return 1;
    }
    return (all & 0x30) == 0x30;
}

/* Returns an entry of the reflection renderer's texture table. */
static inline EftTexEntry *EftSurfRt_GetTex(EftSurfRt *rt, s32 idx) {
    return &rt->texTbl[idx];
}

#ifdef PORT_VITA
/* (port, Vita) 1 when the three corners of a surface triangle all lie clearly outside one of the five clip planes:
   ClipPoly_ClipPlane leaves nothing of such a triangle, whichever clipping path it takes. Worked out in the console's
   own float arithmetic it is the same answer except for corners within a hair of the plane, which are left to the
   original path (the margin). On Namek about half of the 700 surface triangles a frame are wholly off the view, and
   each went through its corners' set-up and up to five clipping calls first. */
static s32 EftSurf_PortOutside(EftSurfTri *tri, Vec4 *pl) {
    s32 p;
    s32 k;

    for (p = 0; p < 5; p++, pl++) {
        for (k = 0; k < 3; k++) {
            f32 a = pl->x * tri->v[k].pos[0], b = pl->y * tri->v[k].pos[1], c = pl->z * tri->v[k].pos[2];
            f32 d = a + b + c + pl->w;
            f32 m = (a < 0.0f ? -a : a) + (b < 0.0f ? -b : b) + (c < 0.0f ? -c : c) + (pl->w < 0.0f ? -pl->w : pl->w);
            if (!(d < -1e-4f * m)) {
                break; /* (this corner is not clearly outside this plane) */
            }
        }
        if (k == 3) {
            return 1;
        }
    }
    return 0;
}
#endif

#ifdef PORT_VITA
/* (port, Vita) 1 when the three corners all lie clearly inside the five clip planes: ClipPoly_ClipPlane would hand
   the triangle back unchanged five times (no corner outside), so the calls are left out. */
static s32 EftSurf_PortInside(EftSurfVtx *tri, Vec4 *pl) {
    s32 p;
    s32 k;
    extern int gVitaSurfCull;

    if (!gVitaSurfCull) {
        return 0;
    }
    for (p = 0; p < 5; p++, pl++) {
        for (k = 0; k < 3; k++) {
            f32 a = pl->x * tri[k].pos[0], b = pl->y * tri[k].pos[1], c = pl->z * tri[k].pos[2];
            f32 d = a + b + c + pl->w * tri[k].pos[3];
            f32 m = (a < 0.0f ? -a : a) + (b < 0.0f ? -b : b) + (c < 0.0f ? -c : c) + (pl->w < 0.0f ? -pl->w : pl->w);
            if (!(d > 1e-4f * m) || tri[k].pos[3] != 1.0f) {
                return 0;
            }
        }
    }
    return 1;
}
#endif

#ifdef PORT_VITA
/* where the surfaces' frame goes: [0] all, [1] the triangles' set-up, [2] their draws;
   counts: [0] frames, [1] triangles looked at, [2] left out (off the view), [3] clipped draws, [4] unclipped draws,
   [5] other kinds */
unsigned long long gVitaSurfUs[3];
unsigned gVitaSurfN[10];
/* the processor's cycles in the surfaces, one frame in eight: [0] the triangles' set-up, [1] their
   draw routines, of them [2] clipping, [3] projecting, [4] queueing; [5] triangles, [6] frames, [7] all */
unsigned long long gVitaSurfCyc[8];
int gVitaSurfMeas;
extern int gVitaPerfOn;
extern int scePerfArmPmonGetCounterValue(int thid, unsigned int counter, unsigned int *value);
unsigned Vita_SurfCyc(void) {
    unsigned v = 0;
    scePerfArmPmonGetCounterValue(0, 31, &v);
    return v;
}
extern unsigned int sceKernelGetProcessTimeLow(void);
#define SURF_T() sceKernelGetProcessTimeLow()
/* The vector library's routines the triangles' set-up calls three times a triangle, here in line: the same
   operations in the same order as the library's fast stand-ins (port/src/port_vufast.c, checked bit for bit against
   the library), in the float unit's mode of the game's thread; a result that is not finite goes to the library's
   routine as the stand-in does. The copies are copies. (~1000 triangles a frame on Kame House.) */
extern struct EftVuHead { f32 vf0[4], vf1[4], vf2[4], vf3[4]; } gRefVu0; /* (the vector unit's registers as the library keeps them: port/vu0_a.h) */
static inline s32 EftV_Bad(f32 a) {
    f32 t = a * 0.0f;
    return t != t;
}
static inline f32 EftV_DistSq(f32 *a, f32 *b) {
    f32 dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2], dw = a[3] - b[3];
    f32 tx = dx * dx, ty = dy * dy, tz = dz * dz;
    f32 s = (tx + ty) + gRefVu0.vf3[0] * tz;
    if (EftV_Bad(s) | EftV_Bad(dw)) {
        return Vec3_DistSq((Vec4 *)a, (Vec4 *)b);
    }
    return s;
}
static inline void EftV_Scale3(f32 *o, f32 s) {
    f32 x = o[0] * s, y = o[1] * s, z = o[2] * s;
    if (EftV_Bad(x) | EftV_Bad(y) | EftV_Bad(z)) {
        Vec3_Scale((Vec4 *)o, (Vec4 *)o, s);
        return;
    }
    o[0] = x;
    o[1] = y;
    o[2] = z;
}
/* ---- bt3.txt seavtx (the surfaces' vertices worked out once): the surfaces queued in the ordering table (Kame House's
   sea and the other stages' water: ~600 triangles a frame, ~8 ms of the game's thread there) are triangles of their
   own, each with its three corners: a corner shared by six triangles was tested against the five clip planes and
   projected six times. A mesh's corners are now numbered once (the same place, texture place and colour: the same
   corner), and in a frame each corner is tested and projected once; a triangle whose three corners lie inside the
   planes is queued from them, as EftSurf_DrawPolyOtClipped / EftSurf_DrawTriOtClipped would queue it unclipped. A
   triangle with a corner outside goes their way, clipped. 0 off (default), 1 on, 2 in the soak's B battles. ---- */
int gVitaSeaVtx = 0;
unsigned gVitaSeaVtxN[4]; /* for the log: triangles queued from the corners worked out once, corners worked out, corners found worked out, meshes numbered */
typedef struct SeaV { s32 scr[4]; Vec4 st; u32 stamp; s32 inside; s32 pad[2]; } __attribute__((aligned(16))) SeaV;
typedef struct SeaMesh { EftSurfMesh *mesh; struct EftSurfTri *tris; s32 triCount, nv; u16 *idx; SeaV *v; void *raw; } SeaMesh;
#define SEA_MESH_N 256
static SeaMesh sSeaMesh[SEA_MESH_N];
static void *sSeaFile;
static u32 sSeaStamp;
extern void EftSurf_PortReflectTri(s32 *s0, s32 *s1, s32 *s2, Vec4 *c0, Vec4 *c1, Vec4 *c2, Vec4 *st0, Vec4 *st1, Vec4 *st2, u64 *tex, Vec4 *fog, s32 zBias);
static void EftSurf_PortPlainTri(s32 *s0, s32 *s1, s32 *s2, f32 *c0, f32 *c1, f32 *c2, Vec4 *st0, Vec4 *st1, Vec4 *st2, s32 layer, u64 tex0, s32 zBias);
static void sea_free(void) {
    s32 k;
    for (k = 0; k < SEA_MESH_N; k++) {
        if (sSeaMesh[k].mesh != NULL) {
            __builtin_free(sSeaMesh[k].idx);
            __builtin_free(sSeaMesh[k].raw);
        }
    }
    __builtin_memset(sSeaMesh, 0, sizeof(sSeaMesh));
}
static u32 sea_hash(const u32 *w) {
    u32 h = 2166136261u;
    s32 k;
    for (k = 0; k < 12; k++) {
        h = (h ^ w[k]) * 16777619u;
    }
    return h ^ h >> 15;
}
/* the mesh's corners numbered: NULL when it is not one to take this way (or no memory) */
static SeaMesh *sea_mesh(EftSurfMesh *m, void *file) {
    extern int gVitaAbSide, gVitaSurfCull;
    u32 h, n, P, i;
    SeaMesh *e = NULL;
    u16 *ht;
    s32 *first;
    if (!(gVitaSeaVtx == 2 ? gVitaAbSide == 1 : gVitaSeaVtx != 0) || !gVitaSurfCull) {
        return NULL;
    }
    if (file != sSeaFile) {
        sea_free();
        sSeaFile = file;
    }
    if (m->triCount <= 0 || m->triCount > 20000 || (m->flags & (EFT_SURF_CLIP | EFT_SURF_WAVE))) {
        return NULL;
    }
    h = ((u32)(unsigned long)m * 0x9E3779B1u) >> 24;
    for (i = 0; i < SEA_MESH_N; i++, h = (h + 1) & (SEA_MESH_N - 1)) {
        SeaMesh *c = &sSeaMesh[h];
        if (c->mesh == m && c->tris == m->tris && c->triCount == m->triCount) {
            return c->v != NULL ? c : NULL;
        }
        if (c->mesh == NULL) {
            e = c;
            break;
        }
    }
    if (e == NULL) {
        return NULL; /* (full) */
    }
    e->mesh = m;
    e->tris = m->tris;
    e->triCount = m->triCount;
    n = (u32)m->triCount * 3;
    for (P = 1; P < 2 * n; P <<= 1) {
    }
    e->idx = (u16 *)__builtin_malloc(n * sizeof(u16));
    ht = (u16 *)__builtin_malloc(P * sizeof(u16));
    first = (s32 *)__builtin_malloc(n * sizeof(s32));
    if (e->idx == NULL || ht == NULL || first == NULL) {
        __builtin_free(ht);
        __builtin_free(first);
        return NULL; /* (e->v NULL: never this way) */
    }
    __builtin_memset(ht, 0, P * sizeof(u16));
    e->nv = 0;
    for (i = 0; i < n; i++) {
        const EftSurfVtx *vx = &m->tris[i / 3].v[i % 3];
        u32 k = sea_hash((const u32 *)vx) & (P - 1);
        while (ht[k] != 0 && __builtin_memcmp(&m->tris[first[ht[k] - 1] / 3].v[first[ht[k] - 1] % 3], vx, sizeof(EftSurfVtx)) != 0) {
            k = (k + 1) & (P - 1);
        }
        if (ht[k] == 0) {
            first[e->nv] = (s32)i;
            ht[k] = (u16)(e->nv + 1);
            e->nv++;
        }
        e->idx[i] = (u16)(ht[k] - 1);
    }
    __builtin_free(ht);
    __builtin_free(first);
    e->raw = __builtin_malloc((u32)e->nv * sizeof(SeaV) + 16);
    if (e->raw == NULL) {
        return NULL;
    }
    e->v = (SeaV *)(((unsigned long)e->raw + 15) & ~15ul);
    __builtin_memset(e->v, 0, (u32)e->nv * sizeof(SeaV));
    gVitaSeaVtxN[3]++;
    {
        extern void Vita_Log(const char *fmt, ...);
        static s32 lines;
        if (lines < 40) {
            lines++;
            Vita_Log("vita: surfaces (seavtx): a mesh of %d triangles: %d corners of %u\n", m->triCount, e->nv, n);
        }
    }
    return e;
}
/* (as EftSurf_PortInside, one corner) */
static inline s32 sea_inside(const f32 *pos, const Vec4 *pl) {
    s32 p;
    if (pos[3] != 1.0f) {
        return 0;
    }
    for (p = 0; p < 5; p++, pl++) {
        f32 a = pl->x * pos[0], b = pl->y * pos[1], c = pl->z * pos[2];
        f32 d = a + b + c + pl->w * pos[3];
        f32 m = (a < 0.0f ? -a : a) + (b < 0.0f ? -b : b) + (c < 0.0f ? -c : c) + (pl->w < 0.0f ? -pl->w : pl->w);
        if (!(d > 1e-4f * m)) {
            return 0;
        }
    }
    return 1;
}
/* triangle j of the mesh (its corners prepared in out) queued from its corners worked out once: 1; 0: a corner
   outside the planes (the caller's way, clipped) */
static s32 sea_tri(SeaMesh *sm, s32 j, EftSurfVtx *out, Vec4 *pl, s32 reflect, u64 *tex, f32 (*fog)[4], s32 layer, u64 tex0, s32 zBias) {
    SeaV *v[3];
    s32 k;
    for (k = 0; k < 3; k++) {
        SeaV *e = &sm->v[sm->idx[j * 3 + k]];
        if (e->stamp != sSeaStamp) {
            e->stamp = sSeaStamp;
            e->inside = sea_inside(out[k].pos, pl);
            if (e->inside) {
                ClipPoly_ProjectCur((s32 (*)[4])e->scr, &e->st, &out[k], 1);
            }
            gVitaSeaVtxN[1]++;
        } else {
            gVitaSeaVtxN[2]++;
        }
        if (!e->inside) {
            return 0;
        }
        v[k] = e;
    }
    gVitaSeaVtxN[0]++;
    if (reflect) {
        EftSurf_PortReflectTri(v[0]->scr, v[1]->scr, v[2]->scr, (Vec4 *)out[0].color, (Vec4 *)out[1].color, (Vec4 *)out[2].color, &v[0]->st, &v[1]->st, &v[2]->st,
                               tex, (Vec4 *)fog, zBias);
    } else {
        EftSurf_PortPlainTri(v[0]->scr, v[1]->scr, v[2]->scr, out[0].color, out[1].color, out[2].color, &v[0]->st, &v[1]->st, &v[2]->st, layer, tex0, zBias);
    }
    return 1;
}
#endif
/* Draws every mesh of the surface file: scrolls and tints the vertices of each triangle, then sends it through
   the draw routine its flags select. */
void EftSurf_DrawMeshes(void) {
    EftSurfVtx out[9];
    EftFVec diff;
    EftFVec cam;
    u64 tex[2];
    f32 dist[3];
    EftFVec fog[3];
    EftFVec light;
    f32 height;
    EftSurf *w = gEftSurf;
    EftSurfRt *rt = &w->rt;
    f32 distSq = rt->param.fadeDist * rt->param.fadeDist;
    f32 invDist = 1.0f / distSq;
    EftSurfTex *texs = w->file->texs;
    EftSurfMesh *mesh = w->file->meshes;
    s32 meshCount = w->file->meshCount;
    f32 bright;
    s32 i;
    s32 j;
    u64 tex0;
    s32 zBias;
    s32 near;
    s32 layer;

#ifdef PORT_VITA
    unsigned surfT0 = SURF_T();
    unsigned surfC0, surfC1 = 0, surfC2;
    gVitaSurfN[0]++;
    gVitaSurfMeas = gVitaPerfOn && (gVitaSurfN[0] & 7) == 0;
    if (++sSeaStamp == 0) { /* (seavtx: the corners worked out in this call) */
        sSeaStamp = 1;
    }
    surfC0 = gVitaSurfMeas ? Vita_SurfCyc() : 0;
#endif
    memset(tex, 0, sizeof(tex));
    EftGfx_UpdateClipPlanes();
    gEftSurf->u.s.planes = EftGfx_GetClipPlanes();

    Vec4_Copy((Vec4 *)cam, &gBtlCamView->pos);
    BtlStage_GetWaterLevel(&height);
    cam[1] = height;
    bright = EftStage_GetTintScale();
    EftSurf_BeginDraw(0, 0);
    Vec3_Scale((Vec4 *)light, (Vec4 *)w->rt.param.colorA, bright);
    light[3] = rt->param.colorA[3] * (1.0f / 128.0f);
    if (rt->texTbl != NULL) {
        tex[0] = EftSurfRt_GetTex(rt, rt->texA + rt->flip)->tex0;
        tex[1] = EftSurfRt_GetTex(rt, rt->texB + rt->flip)->tex0;
    }
    Vu0Cur_Push();
    Vu0Cur_LoadMtx(&gBtlCamView->world2screen);
#ifdef PORT_VITA
    {   /* the surfaces' meshes, once a fight, 20 s in */
        extern void Vita_Log(const char *fmt, ...);
        static s32 frames;
        static EftSurf *seen;
        if (seen != w) {
            seen = w;
            frames = 0;
        }
        if (++frames == 1200) {
            EftSurfMesh *m = mesh;
            s32 mi;
            Vita_Log("vita: surfaces: %d meshes\n", meshCount);
            for (mi = 0; mi < meshCount; mi++, m++) {
                f32 y0 = m->triCount > 0 ? m->tris[0].v[0].pos[1] : 0.0f;
                s32 t, k2, flat = 1;
                for (t = 0; t < m->triCount; t++) {
                    for (k2 = 0; k2 < 3; k2++) {
                        if (m->tris[t].v[k2].pos[1] != y0) {
                            flat = 0;
                        }
                    }
                }
                Vita_Log("vita:   mesh %d: flags %03x, %d triangles, texture %d, z bias %d, y %.2f..%.2f, %s (first corner y %.2f)\n", mi, m->flags, m->triCount, m->tex,
                         m->zBias, m->boxMin.y, m->boxMax.y, flat ? "flat" : "not flat", y0);
            }
        }
    }
#endif
    for (i = 0; i < meshCount; i++, mesh++) {
        s32 flags;
        f32 u;
        f32 fade;
        f32 v;
        EftSurfTri *tri;
        s32 triCount;

        tex0 = texs[mesh->tex].tex0;
        flags = mesh->flags;
        u = mesh->u;
        fade = 1.0f;
        v = mesh->v;
        zBias = mesh->zBias;
        tri = mesh->tris;
        triCount = mesh->triCount;
        if (flags & EFT_SURF_HIDDEN) {
            continue;
        }
        if (!EftSurf_IsBoxVisible(&mesh->boxMin, &mesh->boxMax)) {
            continue;
        }
        if (flags & EFT_SURF_NEAR_ONLY) {
            f32 d;

            Vec4_Sub((Vec4 *)diff, &mesh->center, (Vec4 *)cam);
            d = Vec3_Dot((Vec4 *)diff, (Vec4 *)diff);
            if (d > 160000.0f) {
                continue;
            }
            if (d > 40000.0f) {
                fade -= (d - 40000.0f) / 120000.0f;
            }
        }
        layer = 1;
        if (!(flags & EFT_SURF_NO_DEPTH_TEST)) {
            layer = (flags & EFT_SURF_LAYER2) ? 2 : 0;
        }
#ifdef PORT_VITA
        {   /* a reflecting surface queued in the ordering table whose corners all lie in one horizontal
               plane: its triangles never cover each other, so Ot_Send may draw its two passes one after the other
               (src/sys/gfx_ot.c); its packets are marked (eft_d.c EftSurf_QueueTri) */
            extern int gVitaSeaMark, gVitaSeaRun;
            gVitaSeaMark = 0;
            if (gVitaSeaRun && (flags & (EFT_SURF_CLIP | EFT_SURF_REFLECT)) == EFT_SURF_REFLECT && triCount > 0) {
                f32 y0 = tri[0].v[0].pos[1];
                s32 t, k2, flat = 1;
                for (t = 0; t < triCount && flat; t++) {
                    for (k2 = 0; k2 < 3; k2++) {
                        if (tri[t].v[k2].pos[1] != y0) {
                            flat = 0;
                        }
                    }
                }
                gVitaSeaMark = flat;
                gVitaSurfN[flat ? 8 : 9]++;
            }
        }
#endif
#ifdef PORT_VITA
        SeaMesh *sv = (flags & EFT_SURF_CLIP) ? NULL : sea_mesh(mesh, w->file);
#endif
        for (j = 0; j < triCount; j++, tri++) {
            s32 k;

#ifdef PORT_VITA
            gVitaSurfN[1]++;
            if (gVitaSurfMeas) {
                surfC1 = Vita_SurfCyc();
            }
#endif
#ifdef PORT_VITA
            dist[0] = EftV_DistSq(tri->v[0].pos, cam);
            dist[1] = EftV_DistSq(tri->v[1].pos, cam);
            dist[2] = EftV_DistSq(tri->v[2].pos, cam);
#else
            dist[0] = Vec3_DistSq((Vec4 *)tri->v[0].pos, (Vec4 *)cam);
            dist[1] = Vec3_DistSq((Vec4 *)tri->v[1].pos, (Vec4 *)cam);
            dist[2] = Vec3_DistSq((Vec4 *)tri->v[2].pos, (Vec4 *)cam);
#endif
#ifdef PORT_VITA
            {   /* (port, Vita) a triangle the clipping path below would leave nothing of: not prepared at all. The
                   reflecting surface's far triangles (drawn unclipped) go on as before. */
                extern int gVitaSurfCull;
                s32 clipped = 1;
                if ((flags & (EFT_SURF_CLIP | EFT_SURF_REFLECT)) == (EFT_SURF_CLIP | EFT_SURF_REFLECT)) {
                    for (k = 0; k < 3; k++) {
                        if ((dist[k] > distSq ? distSq : dist[k]) > 202500.0f) {
                            clipped = 0; /* (not near: EftSurf_DrawReflectTri, no clipping) */
                        }
                    }
                }
                if (gVitaSurfCull && clipped && EftSurf_PortOutside(tri, gEftSurf->u.s.planes)) {
                    gVitaSurfN[2]++;
                    continue;
                }
            }
#endif
            near = 1;
            for (k = 0; k < 3; k++) {
#ifdef PORT_VITA
                __builtin_memcpy(out[k].pos, tri->v[k].pos, 16);
                __builtin_memcpy(out[k].st, tri->v[k].st, 16);
                __builtin_memcpy(out[k].color, tri->v[k].color, 16);
#else
                ClipVtx_Set(&out[k], tri->v[k].pos, tri->v[k].st, tri->v[k].color);
#endif
                out[k].pos[3] = 1.0f;
                out[k].st[2] = 1.0f;
                out[k].st[3] = 1.0f;
                if (flags & EFT_SURF_WAVE) {
                    if (out[k].st[1] < 0.5f) {
                        out[k].st[0] += Mathf_Sin(u) * 0.05f;
                    } else {
                        out[k].st[0] -= Mathf_Sin(u) * 0.05f;
                    }
                    out[k].st[1] += v;
                } else if (flags & EFT_SURF_REFLECT) {
#ifdef PORT_VITA
                    __builtin_memcpy(out[k].color, light, 16);
#else
                    Vec4_Copy((Vec4 *)out[k].color, (Vec4 *)light);
#endif
                    out[k].color[3] *= tri->v[k].color[3];
                    if (dist[k] > distSq) {
                        dist[k] = distSq;
                    }
                    if (dist[k] > 202500.0f) {
                        near = 0;
                    }
                    fog[k][3] = 1.0f - dist[k] * invDist;
                    if (fog[k][3] > 1.0f) {
                        fog[k][3] = 1.0f;
                    } else if (fog[k][3] < 0.0f) {
                        fog[k][3] = 0.0f;
                    }
                } else {
                    out[k].st[0] += u;
                    out[k].st[1] += v;
                    out[k].color[3] *= fade;
                }
#ifdef PORT_VITA
                EftV_Scale3(out[k].color, bright);
#else
                Vec3_Scale((Vec4 *)out[k].color, (Vec4 *)out[k].color, bright);
#endif
            }
#ifdef PORT_VITA
            /* (by the way it is drawn: 3 reflecting and clipped, 4 reflecting, 5 clipped (DrawTri*), 6 the
               ordering table's reflecting (DrawPolyOtClipped), 7 the ordering table's (DrawTriOtClipped); no clock reads
               per triangle: one takes a microsecond) */
            gVitaSurfN[(flags & (EFT_SURF_CLIP | EFT_SURF_REFLECT)) == (EFT_SURF_CLIP | EFT_SURF_REFLECT) ? (near ? 3 : 4)
                       : (flags & EFT_SURF_CLIP) ? 5 : (flags & EFT_SURF_REFLECT) ? 6 : 7]++;
            if (gVitaSurfMeas) {
                surfC2 = Vita_SurfCyc();
                gVitaSurfCyc[0] += surfC2 - surfC1;
                surfC1 = surfC2;
            }
#endif
            if ((flags & (EFT_SURF_CLIP | EFT_SURF_REFLECT)) == (EFT_SURF_CLIP | EFT_SURF_REFLECT)) {
                if (near) {
                    EftSurf_DrawReflectTriClipped(out, tex, (Vec4 *)fog);
                } else {
                    EftSurf_DrawReflectTri(out, tex, (Vec4 *)fog);
                }
            } else if (flags & EFT_SURF_CLIP) {
                if (near) {
                    EftSurf_DrawTriClipped(out, tex0);
                } else {
                    EftSurf_DrawTri(out, tex0);
                }
            } else if (flags & EFT_SURF_REFLECT) {
#ifdef PORT_VITA
                if (sv == NULL || !sea_tri(sv, j, out, gEftSurf->u.s.planes, 1, tex, fog, 0, 0, zBias))
#endif
                EftSurf_DrawPolyOtClipped(out, 0, tex, (Vec4 *)fog, zBias);
            } else {
#ifdef PORT_VITA
                if (sv == NULL || !sea_tri(sv, j, out, gEftSurf->u.s.planes, 0, NULL, NULL, layer, tex0, zBias))
#endif
                EftSurf_DrawTriOtClipped(out, layer, 1, 0, tex0, zBias);
            }
#ifdef PORT_VITA
            if (gVitaSurfMeas) {
                gVitaSurfCyc[1] += Vita_SurfCyc() - surfC1;
                gVitaSurfCyc[5]++;
            }
#endif
        }
    }
    EftSurf_EndDraw(0);
    Vu0Cur_Pop();
#ifdef PORT_VITA
    {
        extern int gVitaSeaMark;
        gVitaSeaMark = 0;
    }
    gVitaSurfUs[0] += SURF_T() - surfT0;
    if (gVitaSurfMeas) {
        gVitaSurfCyc[7] += Vita_SurfCyc() - surfC0;
        gVitaSurfCyc[6]++;
        gVitaSurfMeas = 0;
    }
#endif
}

#define EFT_F2RGBA(c) \
    ((u64)(s32)(c)->x | ((u64)(s32)(c)->y << 8) | ((u64)(s32)(c)->z << 16) | ((u64)(s32)(c)->w << 24))

/* Sends one textured, Gouraud-shaded, alpha-blended triangle: a REGLIST packet of PRIM, TEX0 and three
   RGBAQ / ST / XYZ2 groups. */
void EftSurf_PutTri(s32 *xyz0, s32 *xyz1, s32 *xyz2, Vec4 *st0, Vec4 *st1, Vec4 *st2, Vec4 *rgba0, Vec4 *rgba1,
                    Vec4 *rgba2, u64 tex0) {
    u64 *p;

    p = Dma_BeginDirect();
    p[0] = GIF_TAG_EX(1, 1, GIF_FLG_REGLIST, 12);
    p[1] = 0xF52152152160;
    p += 2;
    p[0] = 0x5B;
    p[1] = tex0;
    p += 2;
    ((u32 *)p)[0] = EFT_F2RGBA(rgba0);
    ((u32 *)p)[1] = *(u32 *)&st0->z;
    ((u32 *)p)[2] = *(u32 *)&st0->x;
    ((u32 *)p)[3] = *(u32 *)&st0->y;
    p += 2;
    ((u32 *)p)[0] = xyz0[0] | (xyz0[1] << 16);
    ((u32 *)p)[1] = xyz0[2];
    ((u32 *)p)[2] = EFT_F2RGBA(rgba1);
    ((u32 *)p)[3] = *(u32 *)&st1->z;
    p += 2;
    ((u32 *)p)[0] = *(u32 *)&st1->x;
    ((u32 *)p)[1] = *(u32 *)&st1->y;
    ((u32 *)p)[2] = xyz1[0] | (xyz1[1] << 16);
    ((u32 *)p)[3] = xyz1[2];
    p += 2;
    ((u32 *)p)[0] = EFT_F2RGBA(rgba2);
    ((u32 *)p)[1] = *(u32 *)&st2->z;
    ((u32 *)p)[2] = *(u32 *)&st2->x;
    ((u32 *)p)[3] = *(u32 *)&st2->y;
    p += 2;
    ((u32 *)p)[0] = xyz2[0] | (xyz2[1] << 16);
    ((u32 *)p)[1] = xyz2[2];
    p[1] = 0;
    p += 2;
    Dma_EndDirect(p);
}

/* GS state for the surfaces, both contexts: blend mode, linear filtering, depth test (always when
   noDepthTest), no depth writes; context 2 draws into the other colour buffer. */
void EftSurf_BeginDraw(s32 blend, s32 noDepthTest) {
    u64 *p;
    s32 ztst = 3;

    if (noDepthTest) {
        ztst = 1;
    }
    p = Dma_BeginDirect();
    p[0] = GIF_TAG(14, 1, 1);
    p[1] = GIF_REG_AD;
    p += 2;
    if (blend == 0) {
        p[0] = 0x44;
        p[1] = GS_ALPHA_1;
        p += 2;
    } else if (blend == 1) {
        p[0] = 0x48;
        p[1] = GS_ALPHA_1;
        p += 2;
    } else if (blend == 2) {
        p[0] = 0x42;
        p[1] = GS_ALPHA_1;
        p += 2;
    }
    p[0] = 0x60;
    p[1] = GS_TEX1_1;
    p += 2;
    p[0] = ((u64)ztst << 17) | 0x10000;
    p[1] = GS_TEST_1;
    p += 2;
    p[0] = 0;
    p[1] = GS_CLAMP_1;
    p += 2;
    p[0] = GS_SET_ZBUF(0xE0, GS_PSMZ24, 1);
    p[1] = GS_ZBUF_1;
    p += 2;
    p[0] = 1;
    p[1] = GS_COLCLAMP;
    p += 2;
    p[0] = 0x48;
    p[1] = GS_ALPHA_2;
    p += 2;
    p[0] = 0x60;
    p[1] = 0x15;
    p += 2;
    p[0] = ((u64)ztst << 17) | 0x10000;
    p[1] = GS_TEST_2;
    p += 2;
    p[0] = 0;
    p[1] = GS_CLAMP_2;
    p += 2;
    p[0] = GS_SET_ZBUF(0xE0, GS_PSMZ24, 1);
    p[1] = GS_ZBUF_2;
    p += 2;
    p[0] = !(gGfx.frame & 1) ? 0x80070 : 0x80000;
    p[1] = GS_FRAME_2;
    p += 2;
    p[0] = GS_SET_XYOFFSET(0x7000, 0x7200);
    p[1] = GS_XYOFFSET_2;
    p += 2;
    p[0] = GS_SET_SCISSOR(0, 0x1FF, 0, 0x1BF);
    p[1] = GS_SCISSOR_2;
    p += 2;
    Dma_EndDirect(p);
}

/* Restores the GS state after the surfaces: normal blend, depth test and depth writes; texture clamping
   unless arg is set. */
void EftSurf_EndDraw(s32 arg) {
    u64 *p;
    s32 clamp = arg == 0;

    p = Dma_BeginDirect();
    p[0] = GIF_TAG(11, 1, 1);
    p[1] = GIF_REG_AD;
    p += 2;
    p[0] = 0x44;
    p[1] = GS_ALPHA_1;
    p += 2;
    p[0] = 0x60;
    p[1] = GS_TEX1_1;
    p += 2;
    p[0] = 0x50000;
    p[1] = GS_TEST_1;
    p += 2;
    p[0] = GS_SET_CLAMP(clamp, clamp, 0, 0, 0, 0);
    p[1] = GS_CLAMP_1;
    p += 2;
    p[0] = GS_SET_ZBUF(0xE0, GS_PSMZ24, 0);
    p[1] = GS_ZBUF_1;
    p += 2;
    p[0] = 1;
    p[1] = GS_COLCLAMP;
    p += 2;
    p[0] = 0x44;
    p[1] = GS_ALPHA_2;
    p += 2;
    p[0] = 0x60;
    p[1] = 0x15;
    p += 2;
    p[0] = 0x50000;
    p[1] = GS_TEST_2;
    p += 2;
    p[0] = GS_SET_CLAMP(clamp, clamp, 0, 0, 0, 0);
    p[1] = GS_CLAMP_2;
    p += 2;
    p[0] = GS_SET_ZBUF(0xE0, GS_PSMZ24, 0);
    p[1] = GS_ZBUF_2;
    p += 2;
    Dma_EndDirect(p);
}

/* Returns 1 when a projected vertex is behind the camera or outside the screen of the view being drawn
   (GS coordinates * 16: x within 272 of 2048, or the half of it of a split-screen view; y within 240). */
static inline s32 EftSurf_IsOffScreen(s32 *xyz) {
    s32 xMin = 0x6F0;
    s32 xMax = 0x910;
    s32 yMax = 0x8F0;
    s32 yMin = 0x710;

    if (!BtlScene_IsSingleView()) {
        if (!BtlScene_IsSecondView()) {
            xMax = 0x810;
        } else {
            xMin = 0x7F0;
        }
    }
    if (xyz[2] <= 0) {
        return 1;
    }
    if (xyz[0] >= xMax << 4) {
        return 1;
    }
    if (xyz[0] <= xMin << 4) {
        return 1;
    }
    if (xyz[1] >= yMax << 4) {
        return 1;
    }
    if (xyz[1] <= yMin << 4) {
        return 1;
    }
    return 0;
}

/* Draws a triangle as it is, unless its three vertices are all off screen. */
void EftSurf_DrawTri(EftSurfVtx *tri, u64 tex0) {
    s32 xyz[3][4];
    Vec4 st[3];

    ClipPoly_ProjectCur(xyz, st, tri, 3);
    if (EftSurf_IsOffScreen(xyz[0]) && EftSurf_IsOffScreen(xyz[1]) && EftSurf_IsOffScreen(xyz[2])) {
        return;
    }
    EftSurf_PutTri(xyz[0], xyz[1], xyz[2], &st[0], &st[1], &st[2], (Vec4 *)tri[0].color, (Vec4 *)tri[1].color, (Vec4 *)tri[2].color,
                   tex0);
}

/* Clips a triangle against the five planes and draws the resulting polygon as a fan. */
void EftSurf_DrawTriClipped(EftSurfVtx *tri, u64 tex0) {
#ifdef PORT
    /* The triangle is clipped in place and can grow to eight corners, but callers hand over room for three. On
       the PS2 the extra corners land in the caller's other locals; on PC they reach the return address (the
       crash in EftRibbon_DrawStrip). The work is done on a copy with room. */
    EftSurfVtx portPoly[16];
    __builtin_memcpy(portPoly, tri, 3 * sizeof(EftSurfVtx));
    tri = portPoly;
#endif
    EftScreenPos xyz[9];
    Vec4 st[9];
    Vec4 *plane = gEftSurf->u.s.planes;
    s32 count = 3;
    s32 i;

#ifdef PORT_VITA
    if (!EftSurf_PortInside(tri, plane))
#endif
    for (i = 4; i >= 0; i--) {
        count = ClipPoly_ClipPlane(tri, plane, count);
        plane++;
    }
    if (count != 0) {
        ClipPoly_ProjectCur((s32 (*)[4])xyz, st, tri, count);
        for (i = 2; i < count; i++) {
            if (xyz[0].z > 0xFFFFFF) {
                xyz[0].z = 0xFFFFFF;
            }
            if (xyz[i - 1].z > 0xFFFFFF) {
                xyz[i - 1].z = 0xFFFFFF;
            }
            if (xyz[i].z > 0xFFFFFF) {
                xyz[i].z = 0xFFFFFF;
            }
            EftSurf_PutTri(&xyz[0].x, &xyz[i - 1].x, &xyz[i].x, &st[0], &st[i - 1], &st[i], (Vec4 *)tri[0].color,
                           (Vec4 *)tri[i - 1].color, (Vec4 *)tri[i].color, tex0);
        }
    }
}

/* Draws a triangle of the reflecting surface as it is, unless its three vertices are all off screen. */
void EftSurf_DrawReflectTri(EftSurfVtx *tri, u64 *tex, Vec4 *fog) {
    s32 xyz[3][4];
    Vec4 st[3];

    ClipPoly_ProjectCur(xyz, st, tri, 3);
    if (EftSurf_IsOffScreen(xyz[0]) && EftSurf_IsOffScreen(xyz[1]) && EftSurf_IsOffScreen(xyz[2])) {
        return;
    }
    EftSurf_DrawTriDirect(xyz[0], xyz[1], xyz[2], (Vec4 *)tri[0].color, (Vec4 *)tri[1].color, (Vec4 *)tri[2].color, &st[0], &st[1],
                          &st[2], 0, tex, fog, 2);
}

/* Clips a triangle of the reflecting surface against the five planes and draws the fan. */
void EftSurf_DrawReflectTriClipped(EftSurfVtx *tri, u64 *tex, Vec4 *fog) {
#ifdef PORT
    /* The triangle is clipped in place and can grow to eight corners, but callers hand over room for three. On
       the PS2 the extra corners land in the caller's other locals; on PC they reach the return address (the
       crash in EftRibbon_DrawStrip). The work is done on a copy with room. */
    EftSurfVtx portPoly[16];
    __builtin_memcpy(portPoly, tri, 3 * sizeof(EftSurfVtx));
    tri = portPoly;
#endif
    s32 xyz[9][4];
    Vec4 st[9];
    Vec4 *plane = gEftSurf->u.s.planes;
    s32 count = 3;
    s32 i;

#ifdef PORT_VITA
    if (!EftSurf_PortInside(tri, plane))
#endif
    for (i = 4; i >= 0; i--) {
        count = ClipPoly_ClipPlane(tri, plane, count);
        plane++;
    }
    if (count != 0) {
        ClipPoly_ProjectCur(xyz, st, tri, count);
        for (i = 2; i < count; i++) {
            EftSurf_DrawTriDirect(xyz[0], xyz[i - 1], xyz[i], (Vec4 *)tri[0].color, (Vec4 *)tri[i - 1].color,
                                  (Vec4 *)tri[i].color,                                   &st[0], &st[i - 1], &st[i], 0, tex, fog, 2);
        }
    }
}

/* GS vertex register with fog (XYZF2), and the triangle packet this file queues in the ordering table. */
typedef struct EftSurfXyzf {
    u64 x : 16;
    u64 y : 16;
    u64 z : 24;
    u64 f : 8;
} EftSurfXyzf;

typedef struct EftSurfRgbaq {
    u8 r, g, b, a;
    f32 q;
} EftSurfRgbaq;

typedef struct EftSurfSt {
    f32 s, t;
} EftSurfSt;

/* DMA tag + REGLIST GIF tag (12 registers): PRIM, TEX0, three (RGBAQ, ST, XYZF2), NOP. */
typedef struct EftSurfTriPkt {
    /* 0x00 */ u32 dmaTag; /* 0x20000007: NEXT, 7 quadwords */
    /* 0x04 */ struct EftSurfTriPkt *next;
    /* 0x08 */ u32 vif0;   /* 0x10000000 */
    /* 0x0C */ u32 vif1;   /* 0x50000007: DIRECT, 7 quadwords */
    /* 0x10 */ u64 gifTag; /* 0xC400000000008001: NLOOP 1, EOP, REGLIST, 12 registers */
    /* 0x18 */ u64 regs;
    /* 0x20 */ u64 prim;
    /* 0x28 */ u64 tex0;
    /* 0x30 */ EftSurfRgbaq rgbaq0;
    /* 0x38 */ EftSurfSt st0;
    /* 0x40 */ EftSurfXyzf xyz0;
    /* 0x48 */ EftSurfRgbaq rgbaq1;
    /* 0x50 */ EftSurfSt st1;
    /* 0x58 */ EftSurfXyzf xyz1;
    /* 0x60 */ EftSurfRgbaq rgbaq2;
    /* 0x68 */ EftSurfSt st2;
    /* 0x70 */ EftSurfXyzf xyz2;
    /* 0x78 */ u64 nop;
} EftSurfTriPkt; /* size 0x80 */

/* Queues one gouraud textured triangle in the ordering table at depth slot z (clamped to 0..0xFFF). Skipped
   when its three alphas are below 0.1. A negative layer means layer 0 without blending. */
static inline void EftSurf_QueueTri(EftScreenPos *v0, EftScreenPos *v1, EftScreenPos *v2, f32 *c0, f32 *c1, f32 *c2,
                                    Vec4 *uv0, Vec4 *uv1, Vec4 *uv2, s32 layer, s32 z, u64 tex0) {
    s32 abe = 1;
    s32 l;
    EftSurfTriPkt *p;
    OtEntry *e;

    if (c0[3] < 0.1f && c1[3] < 0.1f && c2[3] < 0.1f) {
        return;
    }
    if (layer < 0) {
        layer = 0;
        abe = 0;
    }
    p = (EftSurfTriPkt *)gOtCur;
    gOtCur = (u32 *)(p + 1);
    p->prim = ((u64)abe << 6) | 0x1B;
    p->dmaTag = 0x20000007;
    p->vif0 = 0x10000000;
    p->vif1 = 0x50000007;
    p->gifTag = 0xC400000000008001;
    p->regs = 0xF42142142160;
    p->next = NULL;
    p->nop = 0;
    p->rgbaq0.r = c0[0];
    p->rgbaq0.g = c0[1];
    p->rgbaq0.b = c0[2];
    p->rgbaq0.a = c0[3];
    p->rgbaq0.q = uv0->z;
    p->rgbaq1.r = c1[0];
    p->rgbaq1.g = c1[1];
    p->rgbaq1.b = c1[2];
    p->rgbaq1.a = c1[3];
    p->rgbaq1.q = uv1->z;
    p->rgbaq2.r = c2[0];
    p->rgbaq2.g = c2[1];
    p->rgbaq2.b = c2[2];
    p->rgbaq2.a = c2[3];
    l = layer;
    if (l >= 2) {
        l -= 2;
    }
    p->rgbaq2.q = uv2->z;
    p->tex0 = tex0;
    p->st0.s = uv0->x;
    p->st0.t = uv0->y;
    p->st1.s = uv1->x;
    p->st1.t = uv1->y;
    p->st2.s = uv2->x;
    p->st2.t = uv2->y;
    p->xyz0.x = v0->x;
    p->xyz0.y = v0->y;
    p->xyz0.z = v0->z;
    p->xyz0.f = 0xFF;
    p->xyz1.x = v1->x;
    p->xyz1.y = v1->y;
    p->xyz1.z = v1->z;
    p->xyz1.f = 0xFF;
    p->xyz2.x = v2->x;
    p->xyz2.y = v2->y;
    p->xyz2.z = v2->z;
    p->xyz2.f = 0xFF;
#ifdef PORT_VITA
    p->vif0 = 0x5EB00000u | (u32)l; /* (the log: a surface's ordering-table triangle; the word is not sent) */
#endif
    if (z < 0) {
        e = &gOtZ[0].layer[l];
    } else if (z >= 0x1000) {
        e = &gOtZ[0xFFF].layer[l];
    } else {
        e = &gOtZ[z].layer[l];
    }
    e->tail->next = (OtPrim *)p;
    e->tail = (OtPrim *)p;
}

/* Clips a triangle against the five planes and queues the resulting fan in the ordering table, at the depth of
   the first vertex (mirrored when flipZ) less zBias. */
void EftSurf_DrawTriOtClipped(EftSurfVtx *tri, s32 layer, s32 unused, s32 flipZ, u64 tex0, s32 zBias) {
#ifdef PORT
    /* The triangle is clipped in place and can grow to eight corners, but callers hand over room for three. On
       the PS2 the extra corners land in the caller's other locals; on PC they reach the return address (the
       crash in EftRibbon_DrawStrip). The work is done on a copy with room. */
    EftSurfVtx portPoly[16];
    __builtin_memcpy(portPoly, tri, 3 * sizeof(EftSurfVtx));
    tri = portPoly;
#endif
    EftScreenPos xyz[9];
    Vec4 st[9];
    Vec4 *plane = gEftSurf->u.s.planes;
    s32 count = 3;
    s32 i;
#ifdef PORT_VITA
    unsigned c0 = gVitaSurfMeas ? Vita_SurfCyc() : 0, c1;
#endif

#ifdef PORT_VITA
    if (!EftSurf_PortInside(tri, plane))
#endif
    for (i = 4; i >= 0; i--) {
        count = ClipPoly_ClipPlane(tri, plane, count);
        plane++;
    }
#ifdef PORT_VITA
    if (gVitaSurfMeas) {
        c1 = Vita_SurfCyc();
        gVitaSurfCyc[2] += c1 - c0;
        c0 = c1;
    }
#endif
    if (count != 0) {
        ClipPoly_ProjectCur((s32 (*)[4])xyz, st, tri, count);
#ifdef PORT_VITA
        if (gVitaSurfMeas) {
            c1 = Vita_SurfCyc();
            gVitaSurfCyc[3] += c1 - c0;
            c0 = c1;
        }
#endif
        for (i = 2; i < count; i++) {
            s32 z = xyz[0].z >> 8;

            if (flipZ) {
                z = 0x1000 - z;
            }
            if (xyz[0].z > 0xFFFFFF) {
                xyz[0].z = 0xFFFFFF;
            }
            if (xyz[i - 1].z > 0xFFFFFF) {
                xyz[i - 1].z = 0xFFFFFF;
            }
            if (xyz[i].z > 0xFFFFFF) {
                xyz[i].z = 0xFFFFFF;
            }
            EftSurf_QueueTri(&xyz[0], &xyz[i - 1], &xyz[i], tri[0].color, tri[i - 1].color, tri[i].color, &st[0],
                             &st[i - 1], &st[i], layer, z - zBias, tex0);
        }
#ifdef PORT_VITA
        if (gVitaSurfMeas) {
            gVitaSurfCyc[4] += Vita_SurfCyc() - c0;
        }
#endif
    }
}

#ifdef PORT_VITA
/* (seavtx) EftSurf_DrawTriOtClipped's queueing of a triangle that is not clipped, from its corners projected */
static void EftSurf_PortPlainTri(s32 *s0, s32 *s1, s32 *s2, f32 *c0, f32 *c1, f32 *c2, Vec4 *st0, Vec4 *st1, Vec4 *st2, s32 layer, u64 tex0, s32 zBias) {
    EftScreenPos xyz[3];
    s32 z = s0[2] >> 8;
    __builtin_memcpy(&xyz[0], s0, 16);
    __builtin_memcpy(&xyz[1], s1, 16);
    __builtin_memcpy(&xyz[2], s2, 16);
    if (xyz[0].z > 0xFFFFFF) {
        xyz[0].z = 0xFFFFFF;
    }
    if (xyz[1].z > 0xFFFFFF) {
        xyz[1].z = 0xFFFFFF;
    }
    if (xyz[2].z > 0xFFFFFF) {
        xyz[2].z = 0xFFFFFF;
    }
    EftSurf_QueueTri(&xyz[0], &xyz[1], &xyz[2], c0, c1, c2, st0, st1, st2, layer, z - zBias, tex0);
}
#endif
/* Called once per frame before the views are drawn: builds the two palettes of the reflecting surface,
   alternating between two pairs of textures. */
void EftSurf_BuildPalettes(void) {
    EftSurfRt *rt;

    if (gEftSurf != NULL) {
        rt = &gEftSurf->rt;
        if (rt->flags & EFT_SURF_RT_ON) {
            if (!(rt->flags & EFT_SURF_RT_BUILT)) {
                rt->flip = (rt->flip + 1) & 1;
                EftSurf_RenderPalettes();
                rt->flags |= EFT_SURF_RT_BUILT;
            }
        }
    }
}

/* Once per update: light direction in the surface's frame, and the normal map animation advances by
   animSpeed / animCount of a frame. */
void EftSurf_StepAnim(void) {
    EftSurf *w = gEftSurf;
    EftSurfRt *rt = &w->rt;
    Mtx44 *frame = &w->rt.frame;
    f32 step = (f32)rt->param.animSpeed / (f32)rt->animCount;
    EftFVec a[3] = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 10.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 10.0f, 1.0f } };
    EftFVec b[3] = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 10.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 10.0f, 0.0f, 1.0f } };
    EftFVec dir;
    EftFVec tmp;

    tmp[0] = rt->param.lightDir.x;
    tmp[1] = rt->param.lightDir.y;
    tmp[2] = rt->param.lightDir.z;
    tmp[3] = 1.0f;
    *(EftVec *)dir = *(EftVec *)tmp;
    EftMath_CalcTangentFrame(frame, (EftVec *)a, (EftVec *)b);
    rt->light.x = Vec3_Dot((Vec4 *)dir, (Vec4 *)frame->m[0]);
    rt->light.y = Vec3_Dot((Vec4 *)dir, (Vec4 *)w->rt.frame.m[1]);
    rt->light.z = Vec3_Dot((Vec4 *)dir, (Vec4 *)w->rt.frame.m[2]);
    rt->light.w = 1.0f;
    rt->animAcc += step;
    if (rt->animAcc >= 1.0f) {
        rt->animAcc = 0.0f;
        rt->animFrame++;
    }
    if (rt->animFrame > rt->animCount - 1) {
        rt->animFrame = 0;
    }
}

/* Lights the 256 palette entries of the current normal map frame: one palette from the light direction, one
   from the view (identity with a fixed tilt in split-screen), and gives them to the two result textures. */
void EftSurf_RenderPalettes(void) {
    Mtx44 view;
    EftSurfRt *rt;
    EftTexEntry *src;
    EftTexEntry *dst;
    EftTexEntry *dst2;

    if (gEftSurf == NULL) {
        return;
    }
    rt = &gEftSurf->rt;
    if (!(rt->flags & EFT_SURF_RT_ON)) {
        return;
    }
    src = EftSurfRt_GetTex(rt, rt->animFrame);
    *rt->buf[0].clut = src->def->data->clut;
    memset(rt->buf[1].clut, 0, sizeof(EftSurfClut));
    EftGfx_LightClutDiffuse(rt->buf[1].clut, rt->buf[0].clut, &rt->light, (u32)rt->param.colorB[0], (u32)rt->param.colorB[1],
                  (u32)rt->param.colorB[2]);
    memset(rt->buf[2].clut, 0, sizeof(EftSurfClut));
    Mtx_StoreIdentity(&view);
    if (!Battle_IsSplitScreen()) {
        Mtx_Copy(&view, &gBtlCamView->world2view2);
    } else {
        view.m[1][2] = 0.5f;
    }
    EftGfx_LightClutSpecular(rt->buf[2].clut, rt->buf[0].clut, &view, &rt->frame, &rt->light, rt->param.unk30);
    src->tex0 = EftVram_AddImage(src, 1, 0);
    dst = EftSurfRt_GetTex(rt, rt->texA + rt->flip);
    dst->def->data->clut = *rt->buf[1].clut;
    dst->tex0 = (src->tex0 & ~(0x3FFFULL << 37)) | ((u64)EftVram_AddClut(dst) << 37);
    dst2 = EftSurfRt_GetTex(rt, rt->texB + rt->flip);
    dst2->def->data->clut = *rt->buf[2].clut;
    dst2->tex0 = (src->tex0 & ~(0x3FFFULL << 37)) | ((u64)EftVram_AddClut(dst2) << 37);
}
