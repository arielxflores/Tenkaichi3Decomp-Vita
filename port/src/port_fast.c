/*
 * Faster stand-ins for a few game functions, and the timing of the battle's task lists (Vita build).
 * Compiled like the game's own code (its calling convention), linked with --wrap (port/vita/build.py: FAST_WRAPS).
 *
 * StgFrustum_TestBox, StgFrustum_TestPart (src/battle/stg_a.c): the stage's culling, "drawing only" (which parts of
 * the stage are drawn; nothing of the fight depends on it). The original does each matrix product and division
 * through the vector library (a call per vector operation); here the same arithmetic is plain float code. The
 * stage of the demo's pairing 0 spent 4.5 ms of each frame in the cull.
 *
 * BtlTaskList_Update, BtlTaskList_Draw (src/battle/eft_ae.c): the effects' task lists, as the game's (the same
 * order, the same removal of dead tasks), with the time of each task's update and draw callback added up by the
 * callback, for the log: which effects cost the game's thread its time.
 *
 * Bpe_Decode (src/sys/bpe.c): the game's unpacker (every menu picture and loading screen, the character select's
 * portraits, the motions the fighters start during a fight), byte for byte the same output: whole-int arithmetic
 * instead of the 16-bit the game's code has, and each pair code's expansion kept, once made, for the rest of its
 * block (a pair's bytes copied instead of unwound through the stack again).
 */
#include <stdint.h>
#include <string.h>
#include "common.h"
#include "sys/math3d.h"
#include "battle/stg_a.h"
#include "battle/eft_ae.h"
#include "sys/list.h"
#include "sys/heap.h"
#include "battle/btl_obj.h"
extern s32 BtlStage_GetAmbient(s32 *rgba);
extern s32 BtlStage_GetId(void);

extern unsigned int sceKernelGetProcessTimeLow(void);
extern int snprintf(char *s, unsigned n, const char *fmt, ...);
extern void Vita_Log(const char *fmt, ...);
extern int Vita_InBattle(void);

/* ---- the stage's culling ---- */
static unsigned sCullBox, sCullPart;

/* out = row0 * v.x + row1 * v.y + row2 * v.z + row3 * v.w (Mtx_MulVec4) */
static inline void mul4(float *o, const Mtx44 *m, float x, float y, float z, float w) {
    o[0] = ((m->m[0][0] * x + m->m[1][0] * y) + m->m[2][0] * z) + m->m[3][0] * w;
    o[1] = ((m->m[0][1] * x + m->m[1][1] * y) + m->m[2][1] * z) + m->m[3][1] * w;
    o[2] = ((m->m[0][2] * x + m->m[1][2] * y) + m->m[2][2] * z) + m->m[3][2] * w;
    o[3] = ((m->m[0][3] * x + m->m[1][3] * y) + m->m[2][3] * z) + m->m[3][3] * w;
}

/* Frustum test of the cube (x, y, z) +- r: 0 inside, 1 crossing, 2 outside (as StgFrustum_TestBox). */
s32 __wrap_StgFrustum_TestBox(StgFrustum *fr, f32 x, f32 y, f32 z, f32 r) {
    u32 any = 0, all = 0x3F;
    int i, j, k;
    sCullBox++;
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 2; j++) {
            for (k = 0; k < 2; k++) {
                float v[4], p[4], inv;
                u32 code = 0;
                mul4(v, &fr->view, i == 0 ? x - r : x + r, j == 0 ? y - r : y + r, k == 0 ? z - r : z + r, 1.0f);
                mul4(p, &fr->proj, v[0], v[1], v[2], 1.0f);
                inv = 1.0f / p[3];
                p[0] *= inv;
                p[1] *= inv;
                p[2] *= inv;
                code |= p[0] < -1.0f ? 1 : 0;
                code |= p[0] > 1.0f ? 2 : 0;
                code |= p[1] < -1.0f ? 4 : 0;
                code |= p[1] > 1.0f ? 8 : 0;
                code |= p[2] < 0.0f ? 0x10 : 0;
                code |= p[2] > 1.0f ? 0x20 : 0;
                all &= code;
                any |= code;
            }
        }
    }
    if (any == 0) {
        return 0;
    }
    return all != 0 ? 2 : 1;
}

/* (t = v + plane * radius; the dot of t with the plane, three components) */
static inline float plane_dot(const float *v, const Vec4 *pl, float rad) {
    float tx = v[0] + pl->x * rad, ty = v[1] + pl->y * rad, tz = v[2] + pl->z * rad;
    return (tx * pl->x + ty * pl->y) + tz * pl->z;
}

/* Frustum test of a part's bounding sphere: 0 inside, 1 crossing a side plane, 2 outside (as StgFrustum_TestPart). */
s32 __wrap_StgFrustum_TestPart(StgPart *part, StgFrustum *fr) {
    float cx = part->x, cy = part->y, cz = part->z, v[4], p[4], dot, inv;
    StgNode *node = part->node;
    s32 in = 1, cross = 0;
    sCullPart++;
    if (node != NULL && node->keyCount != 0) {
        StgKey *key = node->keys;
        cx += key->x;
        cy += key->y;
        cz += key->z;
    }
    mul4(v, &fr->view, cx, cy, cz, 1.0f);
    v[3] = 1.0f;
    if (plane_dot(v, &fr->plane[4], part->radius) < 0.0f) {
        in = 0;
    } else {
        mul4(p, &fr->proj, v[0], v[1], v[2], 1.0f);
        inv = 1.0f / p[3];
        p[0] *= inv;
        p[1] *= inv;
        if (-1.0f <= p[0] && p[0] <= 1.0f && -1.0f <= p[1] && p[1] <= 1.0f) {
            return in ? cross : 2;
        }
        if (-1.0f <= p[0] && p[0] <= 1.0f) {
            if (p[1] < -1.0f) {
                dot = plane_dot(v, &fr->plane[3], part->radius);
                if (dot < 0.0f) { in = 0; } else { cross = 1; }
            } else if (p[1] > 1.0f) {
                dot = plane_dot(v, &fr->plane[2], part->radius);
                if (dot < 0.0f) { in = 0; } else { cross = 1; }
            }
        } else {
            dot = 0.0f;
            if (p[0] < -1.0f) {
                dot = plane_dot(v, &fr->plane[1], part->radius);
                if (dot < 0.0f) { in = 0; } else { cross = 1; }
            } else if (p[0] > 1.0f) {
                dot = plane_dot(v, &fr->plane[0], part->radius);
                if (dot < 0.0f) { in = 0; } else { cross = 1; }
            }
            if (cross == 1) {
                if (p[1] < -1.0f) {
                    dot = plane_dot(v, &fr->plane[3], part->radius);
                    if (dot < 0.0f) { in = 0; }
                } else if (p[1] > 1.0f) {
                    dot = plane_dot(v, &fr->plane[2], part->radius);
                    if (dot < 0.0f) { in = 0; }
                }
            }
        }
    }
    return in ? cross : 2;
}

/* ---- the effects' task lists, timed by callback ---- */
extern BtlTask *gBtlTaskCur;
extern void BtlTask_Kill(BtlTask *task);
typedef struct { void *fn; unsigned us, n; } TaskT;
#define TASK_T 64
static TaskT sTaskT[2][TASK_T]; /* 0 update, 1 draw */
static unsigned sTaskFrames;

static void task_add(int k, void *fn, unsigned us) {
    unsigned h = ((unsigned)(uintptr_t)fn >> 2) % TASK_T, i;
    for (i = 0; i < TASK_T; i++) {
        TaskT *t = &sTaskT[k][(h + i) % TASK_T];
        if (t->fn == fn || t->fn == NULL) {
            t->fn = fn;
            t->us += us;
            t->n++;
            return;
        }
    }
}

static void task_update(BtlTaskList *list) {
    BtlTask *task = list->head;
    BtlTask *next;

    while (task != NULL) {
        if (task->cls->update != NULL) {
            unsigned t0 = sceKernelGetProcessTimeLow();
            gBtlTaskCur = task;
            task->cls->update(task);
            task_add(0, (void *)task->cls->update, sceKernelGetProcessTimeLow() - t0);
            if (task->cls->draw != NULL) {
                BTL_TASK_BITS(task)->updated = 1;
            }
        }
        next = task->next;
        if (BTL_TASK_BITS(task)->dead) {
            BtlTask_Kill(task);
        } else if (task->child != NULL) {
            task_update(task->child);
        }
        task = next;
    }
}
void __wrap_BtlTaskList_Update(BtlTaskList *list) {
    sTaskFrames += Vita_InBattle() ? 1 : 0;
    task_update(list);
}

/* ---- the white shards over a fast-moving fighter: pictures for the log. When the speed lines (the
   white trails behind a fighter moving fast, eft_m.c) are drawn, the list is marked (gs_core.c carries the mark to
   the drawing thread, which pictures it whole, every pixel); in the first of two lists in a row the speed lines'
   own triangles are tinted (the trails magenta, the streaks green), the second is as drawn. ---- */
extern void EftSpdLine_Draw(void);
extern void *gEftSpdLine;
extern volatile int gVitaShotMark;
extern int Vita_InFight(void);
static int sTint, sShotNext;
static unsigned sShotN, sShotLast;
#define SHOT_EVENTS 0 /* (off; an earlier version took them) */
static int sInSpd;
static void shot_check(void) {
    const s32 *cnt = gEftSpdLine != NULL ? (const s32 *)((const u8 *)gEftSpdLine + 0x1860) : NULL; /* trails, streaks */
    unsigned now = sceKernelGetProcessTimeLow();
    sTint = 0;
    if (sShotNext) { /* the list after the tinted one: as drawn */
        sShotNext = 0;
        gVitaShotMark = 2;
        return;
    }
    if (cnt == NULL || cnt[0] + cnt[1] <= 0 || sShotN >= SHOT_EVENTS || (sShotN > 0 && now - sShotLast < 2500000u) || !Vita_InFight()) {
        return;
    }
    sShotLast = now;
    Vita_Log("shards: picture %u: %d speed-line trails, %d streaks (tinted: trails magenta, streaks green; the next list as drawn)\n",
             9000 + sShotN * 10 + 1, (int)cnt[0], (int)cnt[1]);
    sShotN++;
    sShotNext = 1;
    sTint = 1;
    gVitaShotMark = 1;
}
void __real_EftGfx_DrawPolyAvgZFront(void *verts, s32 a1, s32 a2, s32 a3, s32 front, s32 flip, u64 tex, s32 zofs);
void __wrap_EftGfx_DrawPolyAvgZFront(void *verts, s32 a1, s32 a2, s32 a3, s32 front, s32 flip, u64 tex, s32 zofs) {
    if (sTint) {
        float *v = (float *)verts; /* three clip vertices of 0x30 bytes: position, texture, colour (0..255) */
        int k;
        for (k = 0; k < 3; k++) {
            float *c = v + k * 12 + 8;
            if (c[0] > 128.0f) {
                c[1] = 0.0f;
            } else {
                c[1] = 255.0f;
            }
        }
    }
    __real_EftGfx_DrawPolyAvgZFront(verts, a1, a2, a3, front, flip, tex, zofs);
}

static void task_draw(BtlTaskList *list) {
    BtlTask *task = list->head;
    BtlTask *next;

    while (task != NULL) {
        if (task->cls->draw != NULL && BTL_TASK_BITS(task)->updated) {
            unsigned t0 = sceKernelGetProcessTimeLow();
            int spd = (void *)task->cls->draw == (void *)EftSpdLine_Draw;
            if (spd) {
                shot_check();
            }
            sInSpd = spd;
            task->cls->draw(task);
            sInSpd = 0;
            sTint = 0;
            task_add(1, (void *)task->cls->draw, sceKernelGetProcessTimeLow() - t0);
        }
        next = task->next;
        if (!BTL_TASK_BITS(task)->dead && task->child != NULL) {
            task_draw(task->child);
        }
        task = next;
    }
}
void __wrap_BtlTaskList_Draw(BtlTaskList *list) {
    task_draw(list);
}

/* vita_main.c, with the log's other lines: the culls, and the effects' callbacks that took the most time (us per
   frame; the callbacks' addresses at run time, and BtlTaskList_Draw's, to find them in port/build/.../bt3.map) */
extern void BtlTaskList_Draw(BtlTaskList *list);
void Vita_FastLog(void) {
    unsigned f = sTaskFrames ? sTaskFrames : 1, k, j;
    if (Vita_InBattle()) { /* the stage's colours and the fighters' (Frieza "too dark" on Kame House: the data, or the port?) */
        s32 amb[4] = {0, 0, 0, 0};
        u8 *lc = (u8 *)BtlStage_GetLightColors();
        Vec4 dir;
        ListNode *nd;
        char line[600];
        int len;
        BtlStage_GetAmbient(amb);
        BtlStage_GetLightVecB(&dir);
        len = snprintf(line, sizeof(line), "vita:   stage %d colours: ambient %d %d %d; light %.3f %.3f %.3f %.3f",
                       (int)BtlStage_GetId(), (int)amb[0], (int)amb[1], (int)amb[2], dir.x, dir.y, dir.z, dir.w);
        if (lc != NULL) {
            for (k = 0; k < 16 && len < 560; k++) {
                len += snprintf(line + len, sizeof(line) - (unsigned)len, "%s%02x", k == 0 ? "; light table " : (k & 3) == 0 ? " " : "", lc[k]);
            }
        }
        for (nd = (ListNode *)List_GetHead(BtlObj_GetUsedList()); nd != NULL && len < 520; nd = (ListNode *)List_GetNext(nd)) {
            BtlObj *o = &((BtlObjNode *)nd)->obj;
            if (o->type == BTL_OBJ_TYPE_CHARA && o->state.view != NULL) {
                len += snprintf(line + len, sizeof(line) - (unsigned)len, "; fighter flags %08x colour %.0f %.0f %.0f %.0f", (unsigned)o->state.view->flags,
                                o->state.view->color.x, o->state.view->color.y, o->state.view->color.z, o->state.view->color.w);
            }
        }
        Vita_Log("%s\n", line);
    }
    Vita_Log("vita:   stage culls per frame: %u boxes, %u parts\n", sCullBox / f, sCullPart / f);
    sCullBox = sCullPart = 0;
    {   /* the stage's surfaces (src/battle/eft_c.c EftSurf_DrawMeshes) */
        extern unsigned long long gVitaSurfUs[3];
        extern unsigned gVitaSurfN[10];
        unsigned sf = gVitaSurfN[0] ? gVitaSurfN[0] : 1;
        if (gVitaSurfN[0] != 0) {
            Vita_Log("vita:   surfaces per frame (%u frames): %u us; %u triangles looked at, %u left out; drawn: %u reflecting clipped, %u reflecting, %u clipped, "
                     "%u reflecting in the ordering table, %u in the ordering table\n",
                     gVitaSurfN[0], (unsigned)(gVitaSurfUs[0] / sf), gVitaSurfN[1] / sf, gVitaSurfN[2] / sf,
                     gVitaSurfN[3] / sf, gVitaSurfN[4] / sf, gVitaSurfN[5] / sf, gVitaSurfN[6] / sf, gVitaSurfN[7] / sf);
            {
                extern unsigned gVitaOtRun[6];
                Vita_Log("vita:   reflecting surfaces: %u flat meshes, %u not flat; the ordering table's sea packets per frame: %u, %u runs put together "
                         "(%u packets in them), %u runs left as they were (palette not loaded by each), %u barriers (%u of them another surface's)\n",
                         gVitaSurfN[8], gVitaSurfN[9], gVitaOtRun[0] / sf, gVitaOtRun[1] / sf, gVitaOtRun[2] / sf, gVitaOtRun[3] / sf, gVitaOtRun[4] / sf, gVitaOtRun[5] / sf);
                for (k = 0; k < 6; k++) {
                    gVitaOtRun[k] = 0;
                }
            }
        }
        {
            extern unsigned long long gVitaSurfCyc[8];
            unsigned long long nt = gVitaSurfCyc[5] ? gVitaSurfCyc[5] : 1, nf = gVitaSurfCyc[6] ? gVitaSurfCyc[6] : 1;
            if (gVitaSurfCyc[6] != 0) {
                Vita_Log("vita:   surfaces' cycles (%llu frames measured, %llu triangles): %llu k a frame; a triangle: set-up %llu, draw routine %llu (clipping %llu, projecting %llu, queueing %llu)\n",
                         gVitaSurfCyc[6], gVitaSurfCyc[5], gVitaSurfCyc[7] / nf / 1000, gVitaSurfCyc[0] / nt, gVitaSurfCyc[1] / nt, gVitaSurfCyc[2] / nt, gVitaSurfCyc[3] / nt,
                         gVitaSurfCyc[4] / nt);
            }
            for (k = 0; k < 8; k++) {
                gVitaSurfCyc[k] = 0;
            }
        }
        gVitaSurfUs[0] = gVitaSurfUs[1] = gVitaSurfUs[2] = 0;
        for (k = 0; k < 10; k++) {
            gVitaSurfN[k] = 0;
        }
    }
    for (k = 0; k < 2; k++) {
        char line[900];
        int len = 0;
        len += snprintf(line + len, sizeof(line) - (unsigned)len, "vita:   effect tasks' %s callbacks, us per frame (%u frames; BtlTaskList_Draw at %08x):", k == 0 ? "update" : "draw", sTaskFrames,
                        (unsigned)(uintptr_t)&BtlTaskList_Draw);
        for (j = 0; j < 14; j++) { /* (the largest, one by one) */
            unsigned i, best = TASK_T, bu = 0;
            for (i = 0; i < TASK_T; i++) {
                if (sTaskT[k][i].fn != NULL && sTaskT[k][i].us >= bu && sTaskT[k][i].n != 0) {
                    best = i;
                    bu = sTaskT[k][i].us;
                }
            }
            if (best == TASK_T || bu / f < 30) {
                break;
            }
            len += snprintf(line + len, sizeof(line) - (unsigned)len, " %08x %u (%u);", (unsigned)(uintptr_t)sTaskT[k][best].fn, bu / f, sTaskT[k][best].n / f);
            sTaskT[k][best].n = 0;
        }
        Vita_Log("%s\n", line);
        for (j = 0; j < TASK_T; j++) {
            sTaskT[k][j].us = 0;
            sTaskT[k][j].n = 0;
        }
    }
    sTaskFrames = 0;
}

/* ---- the unpacker ---- */
#define BPE_EBUF 8192 /* a block's kept expansions */
#define BPE_EMAX 48   /* the longest kept (longer ones are unwound each time, as the game does) */
__attribute__((optimize("no-tree-loop-distribute-patterns"))) /* (the short copies stay loops, not calls) */
void *__wrap_Bpe_Decode(void *src, void *dst, s32 *rawSize) {
    static u8 ident[256];
    static s32 identReady;
    u8 ebuf[BPE_EBUF];
    const u8 *in, *start;
    u8 *out;
    s32 size, packSize;
    void *buf;
    if (!identReady) {
        s32 k;
        for (k = 0; k < 256; k++) {
            ident[k] = (u8)k;
        }
        identReady = 1;
    }
    memcpy(&size, src, 4);
    memcpy(&packSize, (const u8 *)src + 4, 4);
    start = in = (const u8 *)src + 8;
    if (rawSize != NULL) {
        *rawSize = size;
    }
    buf = dst != NULL ? dst : Heap_Alloc(size, 0x20, 0, HEAP_ANY);
    out = (u8 *)buf;
    while (in - start != packSize) {
        u8 left[256], right[256];
        u16 elen[256], eoff[256];
        unsigned epos = 0, bs;
        s32 c = 0, count = *in++, i;
        memcpy(left, ident, 256);
        for (;;) { /* the pair table, as the game reads it */
            if (count > 127) {
                c += count - 127;
                count = 0;
            }
            if (c == 256) {
                break;
            }
            for (i = 0; i <= count; i++, c++) {
                left[c] = *in++;
                if (c != left[c]) {
                    right[c] = *in++;
                }
            }
            if (c == 256) {
                break;
            }
            count = *in++;
        }
        bs = ((unsigned)in[0] << 8) + in[1];
        in += 2;
        memset(elen, 0, sizeof(elen));
        while (bs-- != 0) {
            unsigned x = *in++;
            if (left[x] == x) {
                *out++ = (u8)x;
            } else if (elen[x] != 0) {
                const u8 *e = ebuf + eoff[x];
                unsigned n = elen[x], k;
                for (k = 0; k < n; k++) {
                    out[k] = e[k];
                }
                out += n;
            } else {
                u8 stack[256];
                u8 *o0 = out;
                unsigned sp = 0, y = x, n;
                for (;;) {
                    if (left[y] == y) {
                        *out++ = (u8)y;
                        if (sp == 0) {
                            break;
                        }
                        y = stack[--sp];
                    } else {
                        stack[sp++ & 255] = right[y];
                        y = left[y];
                    }
                }
                n = (unsigned)(out - o0);
                if (n <= BPE_EMAX && epos + n <= BPE_EBUF) {
                    memcpy(ebuf + epos, o0, n);
                    eoff[x] = (u16)epos;
                    elen[x] = (u16)n;
                    epos += n;
                }
            }
        }
    }
    return buf;
}
