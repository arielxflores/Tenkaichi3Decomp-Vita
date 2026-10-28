/*
 * PS Vita: the renderer, a back end for the PC port's GS front end (port/src/gs/gs_core.c) on vitaGL.
 *
 * gs_core.c reads the game's display lists (DMA, VIF, GIF) and keeps the GS's memory; what it draws comes here as
 * GS primitives and as the game's vertex-program batches (gs_vu1.c). This file turns them into draws on the Vita's
 * graphics chip:
 *   - every GS primitive through a few Cg shaders that follow the GS (position, depth and colour as the GS
 *     computes them, S / T / Q divided per pixel, the texture function with 0x80 = 1, alpha test, fog);
 *   - the game's vertex programs (VU1 programs 0, 4 and 6: models, stage, ground shadows) as a vertex shader, with
 *     the stage's and the fighters' vertices kept in the chip's memory while they do not change;
 *   - textures decoded from GS memory and cached by content, palettes on the chip;
 *   - the GS buffers the effects draw into (the ground shadows' page, the glare and glow chain, the blurs) as
 *     framebuffer objects, and the effects the PC renderer draws natively (the outline, the glow, the depth
 *     tint, the intro's cross-fade) as passes of their own;
 *   - the frame's list read on one core and drawn on another, the vitaGL calls on a third (the fourth core with
 *     the CapUnlocker plugin), and the pictures put up in step with the game's clock (at the end of this file).
 * Fights are drawn at 640 x 368 and scaled to the screen, the menus at 960 x 544.
 *
 * Conventions: GS alpha 0x80 is 1.0 (vertex alpha / 128, texture alpha doubled when decoded). A vertex is given
 * to the chip in clip space with w = 1 / Q, so that texture coordinates are interpolated with perspective as the
 * GS does with S, T, Q. GS depth grows towards the viewer: depth = z / zmax with GEQUAL / GREATER.
 * The shaders are compiled at run time (libshacccg.suprx) once and kept in ux0:data/bt3/shcache.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>
#include <zlib.h>
#include <math.h>
#include "../src/gs/gs_internal.h"

extern int gGsMainFbp;  /* gs_sony.c: the buffer this frame is drawn into */
#include <psp2/perf.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

void Vita_Log(const char *fmt, ...);
/* a branch the game's own pictures never take (the log's checks, the film, the tests): laid apart from the
   code around it (-freorder-blocks-and-partition), so that the busy routines keep to fewer cache lines */
#ifndef VITA_RARE
#define VITA_RARE(x) __builtin_expect_with_probability(!!(x), 1, 0.0)
#endif
extern int gGsSplit; /* gs_core.c: the list is read and drawn on two threads */
extern int gVitaPerfOn;
extern void Vita_PerfRead(uint32_t v[5]);
extern void Vita_PerfAccum(uint32_t last[5], uint64_t sum[5]);
extern void Vita_PerfThreadStart(void);
extern uint64_t gVitaPerfWork[5];
#define gVitaGsSkip gVitaGsSkipR /* the render side's copy (vita_gfx.c) */
extern int gVitaGsSkip;
extern void Vita_Log(const char *fmt, ...);

#define GS_W 512.0f
#define GS_H 448.0f
/* The picture on the 960 x 544 screen: all of it (the game projects its 3D for that shape, src/battle/btl_cam.c
   with Port_WideFactor; menus and films are stretched to it), or the console's 4:3 in the middle (wide=0). */
int gVitaWide = 1;
int gVitaPbufMb;  /* bt3.txt pbuf=<MB>: the parameter buffer, 0 vitaGL's default (16 MB) */
/* the size drawn at: 640 x 368, which the Vita's display stretches to its 960 x 544 (a GPU test: the graphics chip
   was still working 40 ms after the last draw of a picture, with the fighters' shader made cheap or not: the
   pixels are what it spends its time on) */
/* In the menus (gPortMenuMode) the screen's own 960 x 544: their 2D and their few models cost the chip
   little, and drawn at the screen's size the text and the art are sharp. The display memory is made for 960 x 544
   once and described again for the size of the moment (vitaGL change, vglSwapResolution); the change is made at a
   picture's end (GsGpu_FrameEnd, scr_switch). bt3.txt menuhi=0: 640 x 368 everywhere, as before. */
#define SCR_W_FIGHT 640
#define SCR_H_FIGHT 368
#define SCR_W_MENU 960
#define SCR_H_MENU 544
static int sScrW = SCR_W_FIGHT, sScrH = SCR_H_FIGHT;
int gVitaMenuHi = 1;
int gVitaSprInset = 1; /* bt3.txt sprinset=0: sprites' coordinates as given (see GsGpu_Draw_impl) */
#define SCR_W sScrW
#define SCR_H sScrH
static int sViewW = SCR_W_FIGHT;
#define VIEW_W sViewW
#define VIEW_H SCR_H
#define VIEW_X ((SCR_W - VIEW_W) / 2)
#define MAX_VERTS 45000
#define MAX_TEX 384
#define TEX_BUCKETS 1024

/* 32 bytes: the vertices of a fight's picture are 1.3 MB written and copied once more for the chip, and on this
   machine the memory traffic is what that costs. The colour as the GS has it (0x80 = 1; the shader scales). */
typedef struct V {
    float x, y, z, w;
    float s, t, q;
    uint8_t c[4];
} V;

typedef struct State { /* everything a batch shares; compared as bytes */
    GLuint tex;
    uint8_t topo, tme, env, linear, wrapS, wrapT;
    uint8_t blend, src, dst, eq;
    uint8_t atest, afunc, aref;
    uint8_t ztest, zfunc, zwrite;
    uint8_t mask;
    uint8_t fba;  /* every pixel written gets alpha 0x80 or more (the masks of the HUD bars) */
    uint8_t sop;  /* what the draw does to bit 7 of the picture's alpha, kept in the stencil buffer: 0 nothing known, 1 clears it, 2 sets it */
    uint8_t date; /* TEST.DATE: 0, or 1 + DATM (drawn only where the alpha already in the picture has bit 7 = DATM) */
    uint8_t qlin; /* a GS triangle whose Q differs corner to corner: its colour across the screen (kVertNP) */
    uint8_t target; /* 0 the picture, 1 the work page (WP_FBP: the ground shadows) */
    uint8_t pad_[2]; /* (no padding unset) */
    int16_t sx, sy, sw, sh;
} State;
/* Whether a draw gives the same picture before or after other such draws: opaque, depth-tested and
   depth-writing, nothing read from the picture or the stencil and all of the colour written (the nearer surface wins
   whatever the order). Only such draws are put together with an earlier draw of their state across draws of other
   states. A blended draw, one with TEST.DATE or one that writes the stencil's copy of alpha bit 7 (FBA) keeps its
   place: drawn together with an earlier one, the stage's see-through groups (the water) were drawn
   before or after each other depending on which of their states came first in the picture, and as the camera moved
   the order flipped: the ground under the water and in the distance flickered. */
extern int gVglSafe2;
static inline int st_commutes(const State *s) {
    return !s->blend && !s->date && s->sop == 0 && s->zwrite && s->ztest && s->zfunc >= 2 && s->mask == 15 && s->target == 0;
}
int gVitaOrder = 1; /* bt3.txt order=0: draws put together across others as before (the test's other half) */
static int sBinCount;
static int sBinLastUnsafe = -1; /* the batch made last (the stage's or the general way's) that is not st_commutes */
/* a strip may go on in batch b (made earlier): it keeps its place among the draws (st_commutes) */
static inline int bin_join_ok(int b, const State *d) {
    return !gVitaOrder || b == sBinCount - 1 || (st_commutes(d) && sBinLastUnsafe < b);
}
static inline void bin_made(int b, const State *d) {
    if (gVitaOrder && !st_commutes(d)) {
        sBinLastUnsafe = b;
    }
}

typedef struct Tex {
    uint64_t tex0, texa;
    uint32_t gen;
    unsigned last;
    GLuint id;
    int16_t pi, pp; /* a texture with a palette: its indices and its palette (below), or -1 */
    uint8_t amin, amax; /* (for the log: its alpha, least and most, as the shader gets it: 0x80 = 1) */
} Tex;
/* a test (below, GsGpu_FrameEnd): the change in effect for the list being drawn: 0 none, 1 the fighters' TEST.DATE
   off, 2 every TEST.DATE off, 3 the fighters' strips the batches' way, 4 every texture made again */
static int sT114Var;
extern int gT116Be, gT116Fe;
static int sFsOn, sFsN, sFsOver; /* (the fighters' draw states of this list are collected for the log) */
static void fs_dump(unsigned list);

static V sVertsMain[MAX_VERTS];
static V *sVerts = sVertsMain;
static const uint16_t *sIdx; /* the batch's triangles as indices into its vertices (the bins), or NULL */
static int sIdxCount; /* (the batch being filled or flushed: the main one, or one of the bins below) */
static int sVertCount;
static State sCur;
static int sMovieDirty;
static void movie_fill(void);
static int sHaveState;
static Tex sTex[MAX_TEX];
static int sTexCount;
static uint16_t sTexBucket[TEX_BUCKETS];
static Tex *sLast;
static unsigned sStCacheGen; /* (declared here for the texture table; below, with the states' cache) */
static struct { uint32_t fbp, gen; } sWork[16]; /* buffers the game drew into that are not on the screen */
static int sWorkCount;
static int sWorkChanged; /* (state_of_: the list above changed: the states remembered may use a texture that is now a work buffer) */
static unsigned sWorkGen;  /* (counts those changes: GsGpu_DsLayer's states remembered) */
unsigned gVglDraws, gVglDropped, gVglNewTex, gVglBatches, gVglFilm;

/* The GS's pixel pipeline as one shader pair (0: could not be built, the fixed-function path below is used):
   texture function with the GS's scale (0x80 = 1, results above 1 cut off), texture alpha, alpha test. */
#define STAGE_BYTES (16u << 20)
extern int gVglSafe;
static void pace_cb(void *fb), pace_swap(int phase);
static int sPaceHold; /* (the GPU profile's window: no picture held back, so that none waits for a buffer: pace_swap) */
static int gVglLean = 1; /* the lean pixel shaders (the colour read in half precision) for everything (an earlier version's GPU profile: 13% of the chip's time less) */
static GLuint sVsM; /* the GS's vertex shader with the colour's alpha scaled twice: the lean modulate variants (+64) need no multiply for the texture's alpha */
static struct { const float *src; uint32_t count, sig, at, seen; } sStageCache[16384];
static uint32_t sStageUsed; /* vertices in the chip's buffer */
#define STAGE_VERTS (STAGE_BYTES / 32)
static GLuint sProg;
static GLint sUniMode, sUniTest, sUniTex;
/* The stage's geometry, kept on the graphics chip (see stage_draw): its own vertex shader, the same pixel shader */
static GLuint sStageProg, sStageVbo;
static GLint sStU[9]; /* uR0..uR3, uK1, uK2, uMode, uTest, uTex */
typedef struct StageBin { float m[24]; uint32_t *ix; int icount; } StageBin;
static const struct Variant *sVarUse; /* the shader pair of the batch being drawn (NULL: the general one) */
static const struct StageBin *sStageDraw; /* the batch being drawn is one of the stage's (else NULL) */
/* The fighters' models, kept on the chip the same way (vertex program 0, layers 0 and 1): the bones and the
   colour of a run of strips are the batch's uniforms (see fighter_draw). */
#define FT_VERTS 65536
static GLuint sFtProg, sFtVbo;
static GLint sFtU[5]; /* uF, uMode, uTest, uTex, uS */
static const float *sFtDraw; /* the batch being drawn is the fighters' (its 20 uniforms; else NULL) */
static struct { const float *src; uint32_t count, sig, at, seen, slot, ioff; } sFtCache[8192];
#define FT_IDX (3 * FT_VERTS)   /* the fighters' strips as triangles, indices kept on the chip beside their vertices */
static GLuint sFtIbo;
static uint8_t *sStageVboPtr, *sFtVboPtr, *sDpIboPtr; /* the chip's buffers, written straight (no vitaGL call: the render thread may be in it) */
static uint16_t *sFtIboPtr;
static uint32_t sFtIdxUsed;
static uint32_t sFtSlotNext; /* places given in the order strips are first written: neighbours differ */
static const float *sFtSlots;
static int sFtSlotsUsed; /* places 0..n-1 hold something (the rest is not sent) */ /* the batch being drawn: its strips' own uniforms (96) */
static uint32_t sFtUsed;
/* The GS has no near or far plane: a model's vertex whose depth is beyond the buffer's range is drawn there, its
   depth held at the range's end. A GPU cuts such a triangle off at the plane instead. bt3.txt zclamp=1: the model and
   stage shaders (and vu_transform) hold a vertex's depth inside the range; a vertex behind the eye (w < 0) is still
   cut. Off by default: no scene was seen to need it (the red planet's flicker was the layers' depth, zl_u). */
int gVitaZClamp = 0;
static char *zclamp_src(const char *src, const char *after, const char *add, GLint *len) {
    const char *at = strstr(src, after);
    size_t n = strlen(src), k, m = strlen(add);
    char *o;
    if (!gVitaZClamp || at == NULL || (o = (char *)malloc(n + m + 1)) == NULL) {
        *len = (GLint)n;
        return NULL;
    }
    k = (size_t)(at - src) + strlen(after);
    memcpy(o, src, k);
    memcpy(o + k, add, m);
    memcpy(o + k + m, src + k, n - k + 1);
    *len = (GLint)(n + m);
    return o;
}
#define ZCLAMP_V "    if (vPos.w > 0.0f) { vPos.z = clamp(vPos.z, -vPos.w, vPos.w); }\n"
static const char kVertFt[] =
    "uniform float4 uF[20];\n" /* what the strips of a batch share */
    "uniform float4 uS[192];\n" /* each strip's own, 16 places of 12: bones 0..9, light, colour */
    "void main(float4 aPos, float4 aNrm, float2 aTex,\n"
    "          float4 out vPos : POSITION, float3 out vTex : TEXCOORD0, float4 out vCol : TEXCOORD1) {\n"
    "    float4 B[12];\n"
    "    int i, j;\n"
    "    for (j = 0; j < 12; j++) { B[j] = float4(0.0f, 0.0f, 0.0f, 0.0f); }\n"
    "    for (i = 0; i < 16; i++) {\n" /* (picked by weights: an index into the uniforms goes wrong for some places, two GPU tests) */
    "        float w = abs(aNrm.w - (float)i) < 0.5f ? 1.0f : 0.0f;\n"
    "        for (j = 0; j < 12; j++) { B[j] += w * uS[i * 12 + j]; }\n"
    "    }\n"
    "    float3 a = aPos.xyz - B[8].xyz;\n"
    "    float3 b = aPos.xyz - B[9].xyz;\n"
    "    float4 pa = B[3] + B[0] * a.x + B[1] * a.y + B[2] * a.z;\n"
    "    float4 pb = B[7] + B[4] * b.x + B[5] * b.y + B[6] * b.z;\n"
    "    float4 p = pb + (pa - pb) * aPos.w;\n"
    "    float4 sv = uF[13] + uF[10] * p.x + uF[11] * p.y + uF[12] * p.z;\n"
    "    if (uF[18].x > 1.5f && sv.w < 0.0f) { sv = -sv; }\n" /* (layer 2, the silhouettes, as vu_transform) */
    "    vPos = sv * uF[16] + sv.w * uF[17];\n"
    "    if (uF[18].x > 1.5f) { vPos.z = clamp(vPos.z, -vPos.w, vPos.w); }\n"
    "    vCol = B[11];\n"
    "    if (uF[18].x > 0.5f) {\n"
    "        float4 na = B[0] * aNrm.x + B[1] * aNrm.y + B[2] * aNrm.z;\n"
    "        float4 nb = B[4] * aNrm.x + B[5] * aNrm.y + B[6] * aNrm.z;\n"
    "        float4 n = nb + (na - nb) * aPos.w;\n"
    "        vTex = float3((dot(B[10].xyz, n.xyz) + B[10].w) * 0.5f + 0.5f, 0.0f, 1.0f);\n"
    "    } else {\n"
    "        vTex = float3(aTex, 1.0f);\n"
    "    }\n"
    "}\n";
static const char kVertStage[] =
    "void main(float4 aPos, float4 aCol, float2 aTex,\n"
    "          uniform float4 uR0, uniform float4 uR1, uniform float4 uR2, uniform float4 uR3, uniform float4 uK1, uniform float4 uK2,\n"
    "          float4 out vPos : POSITION, float3 out vTex : TEXCOORD0, float4 out vCol : TEXCOORD1) {\n"
    "    float4 sv = aPos.x * uR0 + aPos.y * uR1 + aPos.z * uR2 + aPos.w * uR3;\n" /* the vertex program's screen matrix */
    "    vPos = sv * uK1 + sv.w * uK2;\n"                                         /* as vu_transform: GS pixels -> clip space */
    "    vCol = aCol * (255.0f / 128.0f);\n"
    "    vTex = float3(aTex, 1.0f);\n"
    "}\n";
/* The direct path's vertex shaders (see "direct parts" below): a model part's strips with its constants as ONE
   block of uniforms, the same for every strip of the draw. Fighters: 0..9 bones and pivots, 10 light, 11 colour,
   12..15 the screen matrix, 16 scale, 17 offset (GS pixels -> clip space), 18 x: layer (1: the shading layer). */
static const char kVertFtD[] =
    "uniform float4 U[23];\n"
    "void main(float4 aPos, float4 aNrm, float2 aTex,\n"
    "          float4 out vPos : POSITION, float3 out vTex : TEXCOORD0, float4 out vCol : TEXCOORD1) {\n"
    "    float3 a = aPos.xyz - U[8].xyz;\n"
    "    float3 b = aPos.xyz - U[9].xyz;\n"
    "    float4 pa = U[3] + U[0] * a.x + U[1] * a.y + U[2] * a.z;\n"
    "    float4 pb = U[7] + U[4] * b.x + U[5] * b.y + U[6] * b.z;\n"
    "    float4 p = pb + (pa - pb) * aPos.w;\n"
    "    float4 sv = U[15] + U[12] * p.x + U[13] * p.y + U[14] * p.z;\n"
    "    if (U[18].y > 0.5f && sv.w < 0.0f) { sv = -sv; }\n" /* (the silhouettes, layer 2, as kVertFt) */
    "    vPos = sv * U[16] + sv.w * U[17];\n"
    "    if (U[18].y > 0.5f) { vPos.z = clamp(vPos.z, -vPos.w, vPos.w); }\n"
    /* program 0's reject: a triangle with a vertex outside its clip-test volume (VU memory 18..21) is not
       drawn on the PS2 (ADC), where a GPU would clip it and draw the rest. Such a vertex goes behind the eye at the
       origin: every triangle it is part of is clipped to a line (the edges to it end where they begin on the screen).
       Far behind it (z -1e20, w 1), not just behind (z -1, w 0): the chip cuts each edge to it right at its
       other end, so the line left is exactly that edge's ends twice and has no width. With the point just behind, the
       cut ends were a hair off where an edge ran far off the picture, and the line between the triangle's two other
       corners came out dotted (the dotted white line from Bardock's head, his headband's tail) */
    "    if (U[18].z > 0.5f) {\n"
    "        float4 cv = U[22] + U[19] * p.x + U[20] * p.y + U[21] * p.z;\n"
    "        float aw = abs(cv.w);\n"
    "        if (abs(cv.x) > aw || abs(cv.y) > aw || abs(cv.z) > aw) { vPos = float4(0.0f, 0.0f, -1e20f, 1.0f); }\n"
    "    }\n"
    /* a bone that was not finite (bone_fix: replaced, 1 A, 2 B, 3 both): the PS2 takes such a vertex far
       off (no NaN there, the largest number instead) and its clip test drops the triangle; here it went to a corner
       of the picture (the thin line from Bardock's head) */
    "    if (U[18].w > 0.5f) {\n"
    "        float bb = floor(U[18].w * 0.5f + 0.01f);\n"
    "        float ba = U[18].w - 2.0f * bb;\n"
    "        if ((ba > 0.5f && aPos.w > 0.0f) || (bb > 0.5f && aPos.w < 1.0f)) { vPos = float4(0.0f, 0.0f, -1e20f, 1.0f); }\n"
    "    }\n"
    "    vCol = U[11];\n"
    "    if (U[18].x > 0.5f) {\n"
    "        float4 na = U[0] * aNrm.x + U[1] * aNrm.y + U[2] * aNrm.z;\n"
    "        float4 nb = U[4] * aNrm.x + U[5] * aNrm.y + U[6] * aNrm.z;\n"
    "        float4 n = nb + (na - nb) * aPos.w;\n"
    "        vTex = float3((dot(U[10].xyz, n.xyz) + U[10].w) * 0.5f + 0.5f, 0.0f, 1.0f);\n"
    "    } else {\n"
    "        vTex = float3(aTex, 1.0f);\n"
    "    }\n"
    "}\n";
/* the stage: 0..3 the vertex program's screen matrix, 4 scale, 5 offset */
static const char kVertStD[] =
    "uniform float4 U[6];\n"
    "void main(float4 aPos, float4 aCol, float2 aTex,\n"
    "          float4 out vPos : POSITION, float3 out vTex : TEXCOORD0, float4 out vCol : TEXCOORD1) {\n"
    "    float4 sv = aPos.x * U[0] + aPos.y * U[1] + aPos.z * U[2] + aPos.w * U[3];\n"
    "    vPos = sv * U[4] + sv.w * U[5];\n"
    "    vCol = aCol * (255.0f / 128.0f);\n"
    "    vTex = float3(aTex, 1.0f);\n"
    "}\n";
static const char kVert[] =
    "void main(float4 aPos, float4 aCol, float3 aTex,\n"
    "          float4 out vPos : POSITION, float3 out vTex : TEXCOORD0, float4 out vCol : TEXCOORD1) {\n"
    "    vPos = aPos;\n"
    "    vTex = aTex;\n"
    "    vCol = aCol * (255.0f / 128.0f);\n"
    "}\n";
/* The GS's primitives whose texture is read straight from the coordinates (w = 1 / Q): the chip interpolates every
   output with perspective, but the GS takes the colour straight across the screen (Gouraud). So the colour leaves
   multiplied by w (TEXCOORD0's z: w, given as the vertex's Q) and the pixel shader divides by w as interpolated:
   (sum c_i w_i / w_i) / (sum w_i / w_i) over the same perspective weights = the colour straight across the screen.
   (Test 114: the big blended layers drawn over the fighters after them took the colour of their near corners over
   far more of the screen, and covered parts of the fighters.) */
static const char kVertNP[] =
    "void main(float4 aPos, float4 aCol, float3 aTex,\n"
    "          float4 out vPos : POSITION, float3 out vTex : TEXCOORD0, float4 out vCol : TEXCOORD1) {\n"
    "    vPos = aPos;\n"
    "    vTex = aTex;\n"
    "    vCol = aCol * (255.0f / 128.0f) * aTex.z;\n"
    "}\n";
static GLuint sVsNP;  /* (0: not built; the colour then goes with perspective, as before, and w is not given) */
/* as kVert, with the colour's alpha scaled by 255 / 128 once more: the texture's alpha needs that scale
   too in the modulate variants, and the product of the two is the same with the scale on either */
static const char kVertM[] =
    "void main(float4 aPos, float4 aCol, float3 aTex,\n"
    "          float4 out vPos : POSITION, float3 out vTex : TEXCOORD0, float4 out vCol : TEXCOORD1) {\n"
    "    vPos = aPos;\n"
    "    vTex = aTex;\n"
    "    vCol = aCol * float4(255.0f / 128.0f, 255.0f / 128.0f, 255.0f / 128.0f, (255.0f / 128.0f) * (255.0f / 128.0f));\n"
    "}\n";
static const char kFrag[] =
    "uniform sampler2D uTex;\n"
    "uniform float4 uMode;\n" /* x: 0 no texture, 1 modulate, 2 decal with the texture's alpha, 3 decal with the vertex's */
    "uniform float4 uTest;\n" /* x: alpha test function (0 off, else the GS's number + 1), y: reference */
    "float4 main(float3 vTex : TEXCOORD0, float4 vCol : TEXCOORD1) {\n"
    "    float4 c = vCol;\n"
    "    if (uMode.x > 0.5f) {\n"
    "        float4 t = tex2D(uTex, vTex.xy / vTex.z);\n"
    "        t.a = t.a * (255.0f / 128.0f);\n"
    "        if (uMode.x < 1.5f) { c = float4(t.rgb * vCol.rgb, t.a * vCol.a); }\n"
    "        else if (uMode.x < 2.5f) { c = t; }\n"
    "        else { c = float4(t.rgb, vCol.a); }\n"
    "    }\n"
    "    if (uTest.x > 0.5f) {\n"
    "        float d = c.a - uTest.y;\n"
    "        float e = 0.5f / 128.0f;\n"
    "        if (uTest.x < 1.5f) { discard; }\n"                       /* NEVER */
    "        else if (uTest.x < 2.5f) { }\n"                            /* ALWAYS */
    "        else if (uTest.x < 3.5f) { if (d > -e) discard; }\n"       /* LESS */
    "        else if (uTest.x < 4.5f) { if (d > e) discard; }\n"        /* LEQUAL */
    "        else if (uTest.x < 5.5f) { if (abs(d) > e) discard; }\n"   /* EQUAL */
    "        else if (uTest.x < 6.5f) { if (d < -e) discard; }\n"       /* GEQUAL */
    "        else if (uTest.x < 7.5f) { if (d < e) discard; }\n"        /* GREATER */
    "        else { if (abs(d) < e) discard; }\n"                       /* NOTEQUAL */
    "    }\n"
    "    if (uMode.y > 0.5f) { c.a = max(c.a, 1.0f); }\n"
    "    return saturate(c);\n"
    "}\n";

/* The pixel shader in variants (texture function x alpha test): a shader without `discard` keeps the graphics
   chip's hidden-surface removal working (a PowerVR chip shades only what is seen in the end, unless a shader may
   discard pixels), and no variant branches on a uniform per pixel. Built when first needed. */
static GLuint sVsKind[5]; /* the vertex shaders: the GS's (clip-space vertices), the stage's, the fighters', the direct path's fighters' and stage's */
typedef struct Variant { GLuint prog; GLint test, tex, u[6]; } Variant;
/* The texture read straight from the interpolated coordinates (tex2D), not divided by Q in the pixel shader
   (tex2Dproj): the chip fetches such a texture before the shader runs, and a read that depends on arithmetic
   in the shader waits for it (the GPU profile of a fifth of the chip's time in a fight). The GS's
   S, T, Q for the chip's own perspective interpolation: the vertex's w is 1 / Q, its coordinates S / Q. */
static int sNoProj = 1;
static Variant sVar[5][2048]; /* (+ 1024 the colour scaled by its own alpha here, blended with factor one) */  /* (+ 128 a picture drawn here as the texture, its alpha as stored; + 256 and without its alpha, TCC 0) */
static GLuint sFragVar[2048]; /* (f + 8: the texture read straight from the coordinates, not divided by Q: the GPU profile) */
/* the pixel shaders compiled once and kept on the memory card (ux0:data/bt3/shcache, by a hash of their
   source): the shader compiler takes ~0.2 s a program, 140 programs at the start; read back, a few ms */
extern int gVglSafe2;
static uint32_t sShBin[4096];
unsigned gVitaShCache[3]; /* for the log: read from the card, compiled and written, compiled (not written) */
static int sh_cached(GLuint sh, const char *src, GLint len) {
    char path[96];
    uint64_t h = 1469598103934665603ull;
    GLint ok = 0;
    GLsizei bl = 0;
    SceUID fd;
    int i;
    for (i = 0; i < len; i++) {
        h = (h ^ (uint8_t)src[i]) * 1099511628211ull;
    }
    h ^= 172; /* (the format of these files: a new number when it changes) */
    {
        static int first = 1;
        if (first) {
            first = 0;
            Vita_Log("vgl: start: the first pixel shader from the card\n");
        }
    }
    sceIoMkdir("ux0:data/bt3/shcache", 0777);
    snprintf(path, sizeof(path), "ux0:data/bt3/shcache/f%08x%08x.gxp", (unsigned)(h >> 32), (unsigned)h);
    if (!(gVglSafe2 & 32768) && (fd = sceIoOpen(path, SCE_O_RDONLY, 0)) >= 0) {
        int n = sceIoRead(fd, sShBin, sizeof(sShBin));
        sceIoClose(fd);
        if (n > 64 && n < (int)sizeof(sShBin)) {
            glShaderBinary(1, &sh, 0, sShBin, n);
            glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (ok) {
                gVitaShCache[0]++;
                return 1;
            }
        }
    }
    glShaderSource(sh, 1, &src, &len);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        return 0;
    }
    memset(sShBin, 0, 64);
    vglGetShaderBinary(sh, (GLsizei)sizeof(sShBin), &bl, sShBin);
    if (bl > 64 && bl < (GLsizei)sizeof(sShBin) && (fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777)) >= 0) {
        int n = sceIoWrite(fd, sShBin, (SceSize)bl);
        sceIoClose(fd);
        if (n != (int)bl) {
            sceIoRemove(path);
        } else {
            gVitaShCache[1]++;
            return 1;
        }
    }
    gVitaShCache[2]++;
    return 1;
}
static GLuint frag_variant(int f) {
    static const char *const kTex[4] = {
        "",
        "    { half4 t = tex2Dproj(uTex, vTex); t.a = t.a * (half)(255.0f / 128.0f); c = half4(t.rgb * c.rgb, t.a * c.a); }\n",
        "    { half4 t = tex2Dproj(uTex, vTex); t.a = t.a * (half)(255.0f / 128.0f); c = t; }\n",
        "    { half4 t = tex2Dproj(uTex, vTex); c = half4(t.rgb, c.a); }\n"};
    static const char kTest[] =
        "    { float d = (float)c.a - uTest.y; float e = 0.5f / 128.0f;\n"
        "      if (uTest.x < 1.5f) { discard; }\n"
        "      else if (uTest.x < 2.5f) { }\n"
        "      else if (uTest.x < 3.5f) { if (d > -e) discard; }\n"
        "      else if (uTest.x < 4.5f) { if (d > e) discard; }\n"
        "      else if (uTest.x < 5.5f) { if (abs(d) > e) discard; }\n"
        "      else if (uTest.x < 6.5f) { if (d < -e) discard; }\n"
        "      else if (uTest.x < 7.5f) { if (d < e) discard; }\n"
        "      else { if (abs(d) < e) discard; } }\n";
    char src[2048];
    GLint len, ok = 0;
    GLuint fs;
    if (sFragVar[f] != 0) {
        return sFragVar[f];
    }
    {   /* in half precision (the chip's pixel units do twice the work in it); in full precision if that fails */
        static const char *const kTexF[4] = {
            "",
            "    { float4 t = tex2Dproj(uTex, vTex); t.a = t.a * (255.0f / 128.0f); c = float4(t.rgb * c.rgb, t.a * c.a); }\n",
            "    { float4 t = tex2Dproj(uTex, vTex); t.a = t.a * (255.0f / 128.0f); c = t; }\n",
            "    { float4 t = tex2Dproj(uTex, vTex); c = float4(t.rgb, c.a); }\n"};
        int pass;
        for (pass = 0; pass < 2; pass++) {
            ok = 0;
            if (pass == 0 && (gVglSafe & 1048576)) {
                continue;
            }
            static const char *const kTexFb[2][4] = { /* a picture drawn here: alpha as stored (0x80 = 1); [1]: TCC 0 */
                {"", "    { half4 t = tex2Dproj(uTex, vTex); c = half4(t.rgb * c.rgb, t.a * c.a); }\n", "    { c = (half4)tex2Dproj(uTex, vTex); }\n",
                 "    { half4 t = tex2Dproj(uTex, vTex); c = half4(t.rgb, c.a); }\n"},
                {"", "    { half4 t = tex2Dproj(uTex, vTex); c = half4(t.rgb * c.rgb, c.a); }\n", "    { half4 t = tex2Dproj(uTex, vTex); c = half4(t.rgb, c.a); }\n",
                 "    { half4 t = tex2Dproj(uTex, vTex); c = half4(t.rgb, c.a); }\n"}};
            /* (+ 512) a fighter seen through (drawn into the alpha only): the GS's alpha byte, 0xF4..0xFE, does
               not fit the picture's scale (0x80 = 1). Written as 234 + 2 (byte - 0xF4) of 255: even numbers above 128, which
               the usual scale never makes (it makes even ones up to 128 and odd ones above), so the copy of the ids tells
               them apart (kFragTblIds); anything else as usual */
            static const char *const kTexId[4] = {"",
                "    { float4 t = tex2Dproj(uTex, vTex); float a = floor(t.a * 255.0f * (float)c.a + 0.5f);\n"
                "      c = half4(t.rgb * c.rgb, a > 243.5f ? (2.0f * a - 253.75f) * (1.0f / 255.0f) : saturate(a * (1.0f / 128.0f))); }\n",
                "    { float4 t = tex2Dproj(uTex, vTex); float a = floor(t.a * 255.0f + 0.5f);\n"
                "      c = half4(t.rgb, a > 243.5f ? (2.0f * a - 253.75f) * (1.0f / 255.0f) : saturate(a * (1.0f / 128.0f))); }\n", ""};
            const char *texPart = (f & 512) ? kTexId[f & 3] : (f & 128) ? kTexFb[(f & 256) ? 1 : 0][f & 3]
                                : (f & 64) && (f & 3) == 1 ? (pass == 0 ? "    { half4 t = tex2Dproj(uTex, vTex); c = t * c; }\n" : "    { float4 t = tex2Dproj(uTex, vTex); c = t * c; }\n") /* (sVsM: the texture's alpha scale in the colour) */
                                : pass == 0 ? kTex[f & 3] : kTexF[f & 3];
            char texBuf[200];
            if ((f & (128 | 512)) && pass == 1) { /* (full precision: the same with float4) */
                char *q;
                snprintf(texBuf, sizeof(texBuf), "%s", texPart);
                while ((q = strstr(texBuf, "half4")) != NULL) {
                    memcpy(q, "float", 5);
                    memmove(q + 6, q + 5, strlen(q + 5) + 1);
                    q[5] = '4';
                }
                texPart = texBuf;
            }
            len = snprintf(src, sizeof(src), pass == 0 ? "%s%shalf4 main(float3 vTex : TEXCOORD0, float4 vCol : TEXCOORD1) {\n    half4 c = (half4)vCol;\n%s%s%s    return saturate(c);\n}\n"
                                                       : "%s%sfloat4 main(float3 vTex : TEXCOORD0, float4 vCol : TEXCOORD1) {\n    float4 c = vCol;\n%s%s%s    return saturate(c);\n}\n",
                           (f & 3) ? "uniform sampler2D uTex;\n" : "", (f & 4) ? "uniform float4 uTest;\n" : "", texPart, (f & 4) ? kTest : "",
                           /* (as the PC renderer's 9af92af) the GS's factor As / 0x80 goes up to 2, which no blend factor
                              does: where only the source is scaled by its alpha, it is scaled here and blended with factor one */
                           (f & 1024) ? "    c.rgb = c.rgb * c.a;\n" : "");
            if (f & 8) { /* tex2Dproj(uTex, vTex) -> tex2D(uTex, vTex.xy) */
                char *q;
                while ((q = strstr(src, "tex2Dproj(uTex, vTex)")) != NULL) {
                    memcpy(q, "tex2D(uTex, vTex.xy)", 20);
                    memmove(q + 20, q + 21, strlen(q + 21) + 1);
                }
                len = (GLint)strlen(src);
            }
            if (f & 32) { /* the lean ones: the colour read in half precision (the chip's iterators do half the work) */
                char *q;
                if ((q = strstr(src, "float4 vCol : TEXCOORD1")) != NULL) {
                    memmove(q + 4, q + 5, strlen(q + 5) + 1);
                    memcpy(q, "half", 4);
                }
                len = (GLint)strlen(src);
            }
            if ((f & 16) && (f & 32)) { /* the colour (half) divided by w (scaled per triangle: put's sQn) in half precision */
                char *q;
                if ((q = strstr(src, "c = (half4)vCol;")) != NULL) {
                    memmove(q + 27, q + 16, strlen(q + 16) + 1);
                    memcpy(q, "c = vCol / (half)vTex.z;   ", 27);
                } else if ((q = strstr(src, "c = vCol;")) != NULL) { /* (the full-precision try) */
                    memmove(q + 18, q + 9, strlen(q + 9) + 1);
                    memcpy(q, "c = vCol / vTex.z;", 18);
                }
                len = (GLint)strlen(src);
            } else if (f & 16) { /* the colour divided by w as interpolated (kVertNP: GS triangles whose Q differs) */
                char *q;
                if ((q = strstr(src, "c = (half4)vCol;")) != NULL) {
                    memmove(q + 27, q + 16, strlen(q + 16) + 1);
                    memcpy(q, "c = (half4)(vCol / vTex.z);", 27);
                } else if ((q = strstr(src, "c = vCol;")) != NULL) {
                    memmove(q + 18, q + 9, strlen(q + 9) + 1);
                    memcpy(q, "c = vCol / vTex.z;", 18);
                }
                len = (GLint)strlen(src);
            }
            fs = glCreateShader(GL_CG_FRAGMENT_SHADER_EXT);
            ok = sh_cached(fs, src, len);
            if (ok) {
                if (f == 0 || f == 5) {
                    Vita_Log("vgl: pixel shader variant %d in %s precision\n", f, pass == 0 ? "half" : "full");
                }
                {   /* (the program's header for the log: its counts of registers and instructions) */
                    static uint32_t bin[4096];
                    GLsizei bl = 0;
                    int o;
                    memset(bin, 0, 64);
                    vglGetShaderBinary(fs, (GLsizei)sizeof(bin), &bl, bin);
                    for (o = 0; o < 64 && o + 96 <= (int)bl; o++) {
                        if (memcmp((const uint8_t *)bin + o, "GXP", 4) == 0) {
                            uint32_t h[24];
                            memcpy(h, (const uint8_t *)bin + o, sizeof(h));
                            Vita_Log("vgl: pixel shader variant %d (%s): %d bytes; header from 0x24: %x %x %x %x %x %x %x %x %x %x %x %x %x %x\n", f, pass == 0 ? "half" : "full", (int)h[2],
                                     h[9], h[10], h[11], h[12], h[13], h[14], h[15], h[16], h[17], h[18], h[19], h[20], h[21], h[22]);
                            break;
                        }
                    }
                }
                break;
            }
            {
                char log[400];
                log[0] = 0; glGetShaderInfoLog(fs, sizeof(log) - 1, NULL, log); log[sizeof(log) - 1] = 0;
                Vita_Log("vgl: pixel shader variant %d (%s precision) not compiled: %s\n", f, pass == 0 ? "half" : "full", log);
            }
        }
        if (!ok) {
            return 0;
        }
    }
    sFragVar[f] = fs;
    return fs;
}
/* kind 0 the GS's vertices, 1 the stage's, 2 the fighters'; f: texture function (0 none, 1 modulate, 2 decal,
   3 decal with the vertex's alpha) + 4 with an alpha test. NULL if it cannot be built. */
static const Variant *variant(int kind, int f) {
    static const char *const kAttr[5][3] = {{"aPos", "aCol", "aTex"}, {"aPos", "aCol", "aTex"}, {"aPos", "aNrm", "aTex"}, {"aPos", "aNrm", "aTex"}, {"aPos", "aCol", "aTex"}};
    static const char *const kStage[6] = {"uR0", "uR1", "uR2", "uR3", "uK1", "uK2"};
    Variant *v = &sVar[kind][f];
    GLuint fs;
    int k;
    if (v->prog != 0) {
        return v;
    }
    if (v->test == -2 || sVsKind[kind] == 0 || ((f & 64) && (kind != 0 || sVsM == 0 || (f & 16))) || (fs = frag_variant(f)) == 0) {
        v->test = -2;
        return NULL;
    }
    v->prog = glCreateProgram();
    glAttachShader(v->prog, kind == 0 && (f & 16) && sVsNP != 0 ? sVsNP : kind == 0 && (f & 64) ? sVsM : sVsKind[kind]);
    glAttachShader(v->prog, fs);
    for (k = 0; k < 3; k++) {
        glBindAttribLocation(v->prog, (GLuint)k, kAttr[kind][k]);
    }
    glLinkProgram(v->prog);
    v->test = glGetUniformLocation(v->prog, "uTest");
    v->tex = glGetUniformLocation(v->prog, "uTex");
    for (k = 0; k < 6; k++) {
        v->u[k] = kind == 1 ? glGetUniformLocation(v->prog, kStage[k]) : kind == 2 && k == 0 ? glGetUniformLocation(v->prog, "uF") : kind == 2 && k == 1 ? glGetUniformLocation(v->prog, "uS") :
                  kind >= 3 && k == 0 ? glGetUniformLocation(v->prog, "U") : -1;
    }
    return v;
}

static void dp_init(void);
static void ol_init(void); /* the fighters' outline */
static void fx_init(void); /* the effects' passes */
/* the variants drawn with: with a texture, those of the reads straight from the coordinates or those divided by Q */
static int var_used(int ff) {
    return (ff & 16) == 0 && ((ff & 3) == 0 ? (ff & 8) == 0 : ((ff & 8) != 0) == (sNoProj != 0));
}
/* (and the GS's own with the colour across the screen: textured, read straight, kVertNP built) */
static int var_used_k(int kk, int ff) {
    if (ff & 1024) { /* (the GS's own blended with their colour scaled by its alpha) */
        return kk == 0 && !(ff & (128 | 256 | 512)) && var_used_k(kk, ff & 1023);
    }
    if (ff & 512) { /* (the fighters seen through) */
        return (kk == 2 || kk == 3) && !(ff & (4 | 16 | 32 | 64 | 128 | 256)) && ((ff & 3) == 1 || (ff & 3) == 2) && var_used(ff & 15);
    }
    if (ff & 128) { /* (the GS's own, textured from a picture drawn here) */
        int base = ff & 31;
        return kk == 0 && (base & 3) != 0 && !(ff & (32 | 64)) &&
               (var_used(base) || (sVsNP != 0 && (base & 16) && (base & 8) && sNoProj && var_used(base & 15)));
    }
    if (ff & 256) {
        return 0;
    }
    if (ff & 64) { /* (the GS's lean modulate ones with the alpha scale in the colour: sVsM) */
        return kk == 0 && sVsM != 0 && (ff & 32) && (ff & 3) == 1 && !(ff & 16) && var_used(ff & 31);
    }
    if (ff & 32) { /* (the lean ones: as the others, never with the colour divided by w) */
        return !(ff & 16) && var_used(ff & 31);
    }
    return var_used(ff) || (kk == 0 && sVsNP != 0 && (ff & 16) && (ff & 3) != 0 && (ff & 8) && sNoProj && var_used(ff & 15));
}
static void shader_init(void) {
    GLuint vs = glCreateShader(GL_CG_VERTEX_SHADER_EXT), fs = glCreateShader(GL_CG_FRAGMENT_SHADER_EXT), prog;
    const char *src;
    GLint len, ok = 0;
    char log[400];

    sNoProj = !(gVglSafe & 8388608);
    src = kVert; len = (GLint)sizeof(kVert) - 1;
    glShaderSource(vs, 1, &src, &len);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        log[0] = 0; glGetShaderInfoLog(vs, sizeof(log) - 1, NULL, log); log[sizeof(log) - 1] = 0;
        Vita_Log("vgl: vertex shader not compiled: %s\n", log);
        return;
    }
    src = kFrag; len = (GLint)sizeof(kFrag) - 1; ok = 0;
    glShaderSource(fs, 1, &src, &len);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        log[0] = 0; glGetShaderInfoLog(fs, sizeof(log) - 1, NULL, log); log[sizeof(log) - 1] = 0;
        Vita_Log("vgl: fragment shader not compiled: %s\n", log);
        return;
    }
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aCol");
    glBindAttribLocation(prog, 2, "aTex");
    glLinkProgram(prog);
    sUniMode = glGetUniformLocation(prog, "uMode");
    sUniTest = glGetUniformLocation(prog, "uTest");
    sUniTex = glGetUniformLocation(prog, "uTex");
    Vita_Log("vgl: shader pair built (uniforms %d %d %d)\n", (int)sUniMode, (int)sUniTest, (int)sUniTex);
    if (sUniMode < 0 || sUniTest < 0) {
        return;
    }
    sProg = prog;
    sVsKind[0] = vs;
    if (gVglLean && !((unsigned)gVglSafe & 0x80000000u)) { /* (the lean modulate variants' vertex shader) */
        GLuint vsm = glCreateShader(GL_CG_VERTEX_SHADER_EXT);
        src = kVertM; len = (GLint)sizeof(kVertM) - 1; ok = 0;
        glShaderSource(vsm, 1, &src, &len);
        glCompileShader(vsm);
        glGetShaderiv(vsm, GL_COMPILE_STATUS, &ok);
        if (ok) {
            sVsM = vsm;
        } else {
            log[0] = 0; glGetShaderInfoLog(vsm, sizeof(log) - 1, NULL, log); log[sizeof(log) - 1] = 0;
            Vita_Log("vgl: vertex shader with the alpha scaled twice not compiled: %s\n", log);
        }
    }
    if (sNoProj && !(gVglSafe & 1073741824)) { /* (the colour straight across the screen: kVertNP) */
        GLuint vsn = glCreateShader(GL_CG_VERTEX_SHADER_EXT);
        src = kVertNP; len = (GLint)sizeof(kVertNP) - 1; ok = 0;
        glShaderSource(vsn, 1, &src, &len);
        glCompileShader(vsn);
        glGetShaderiv(vsn, GL_COMPILE_STATUS, &ok);
        if (ok) {
            sVsNP = vsn;
        } else {
            log[0] = 0; glGetShaderInfoLog(vsn, sizeof(log) - 1, NULL, log); log[sizeof(log) - 1] = 0;
            Vita_Log("vgl: vertex shader for the colour across the screen not compiled: %s\n", log);
        }
        Vita_Log("vgl: the GS's primitives' colour %s\n", sVsNP != 0 ? "straight across the screen (as the GS)" : "with perspective");
    }
    if (!(gVglSafe & 2048)) {
        GLuint vs2 = glCreateShader(GL_CG_VERTEX_SHADER_EXT), p2;
        static const char *const names[9] = {"uR0", "uR1", "uR2", "uR3", "uK1", "uK2", "uMode", "uTest", "uTex"};
        int k, good = 1;
        char *zc = zclamp_src(kVertStage, "    vPos = sv * uK1 + sv.w * uK2;\n", ZCLAMP_V, &len);
        src = zc != NULL ? zc : kVertStage; ok = 0;
        glShaderSource(vs2, 1, &src, &len);
        free(zc);
        glCompileShader(vs2);
        glGetShaderiv(vs2, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            log[0] = 0; glGetShaderInfoLog(vs2, sizeof(log) - 1, NULL, log); log[sizeof(log) - 1] = 0;
            Vita_Log("vgl: stage vertex shader not compiled: %s\n", log);
            return;
        }
        p2 = glCreateProgram();
        glAttachShader(p2, vs2);
        glAttachShader(p2, fs);
        glBindAttribLocation(p2, 0, "aPos");
        glBindAttribLocation(p2, 1, "aCol");
        glBindAttribLocation(p2, 2, "aTex");
        glLinkProgram(p2);
        for (k = 0; k < 9; k++) {
            sStU[k] = glGetUniformLocation(p2, names[k]);
            if (k < 8 && sStU[k] < 0) {
                good = 0;
            }
        }
        Vita_Log("vgl: stage shader %s\n", good ? "built" : "without its uniforms: not used");
        if (good) {
            glGenBuffers(1, &sStageVbo);
            glBindBuffer(GL_ARRAY_BUFFER, sStageVbo);
            glBufferData(GL_ARRAY_BUFFER, STAGE_BYTES, NULL, GL_STATIC_DRAW);
            sStageVboPtr = (uint8_t *)glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
            glUnmapBuffer(GL_ARRAY_BUFFER);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            sStageProg = p2;
            sVsKind[1] = vs2;
        }
    }
    if (!(gVglSafe & (2048 | 8192))) {
        GLuint vs3 = glCreateShader(GL_CG_VERTEX_SHADER_EXT), p3;
        static const char *const names[5] = {"uF", "uMode", "uTest", "uTex", "uS"};
        int k, good = 1;
        char *zc = zclamp_src(kVertFt, "    vPos = sv * uF[16] + sv.w * uF[17];\n", ZCLAMP_V, &len);
        src = zc != NULL ? zc : kVertFt; ok = 0;
        glShaderSource(vs3, 1, &src, &len);
        free(zc);
        glCompileShader(vs3);
        glGetShaderiv(vs3, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            log[0] = 0; glGetShaderInfoLog(vs3, sizeof(log) - 1, NULL, log); log[sizeof(log) - 1] = 0;
            Vita_Log("vgl: fighter vertex shader not compiled: %s\n", log);
            return;
        }
        p3 = glCreateProgram();
        glAttachShader(p3, vs3);
        glAttachShader(p3, fs);
        glBindAttribLocation(p3, 0, "aPos");
        glBindAttribLocation(p3, 1, "aNrm");
        glBindAttribLocation(p3, 2, "aTex");
        glLinkProgram(p3);
        for (k = 0; k < 5; k++) {
            sFtU[k] = glGetUniformLocation(p3, names[k]);
            if (k != 3 && sFtU[k] < 0) {
                good = 0;
            }
        }
        Vita_Log("vgl: fighter shader %s\n", good ? "built" : "without its uniforms: not used");
        if (good) {
            Vita_Log("vgl: start: the fighters' buffers (the steps of the start, for the rare start that stops here)\n");
            glGenBuffers(1, &sFtVbo);
            glBindBuffer(GL_ARRAY_BUFFER, sFtVbo);
            glBufferData(GL_ARRAY_BUFFER, FT_VERTS * 40, NULL, GL_STATIC_DRAW);
            sFtVboPtr = (uint8_t *)glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
            glUnmapBuffer(GL_ARRAY_BUFFER);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glGenBuffers(1, &sFtIbo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sFtIbo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, FT_IDX * 2, NULL, GL_STATIC_DRAW);
            sFtIboPtr = (uint16_t *)glMapBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_WRITE_ONLY); /* (the chip's address: the same on both sides) */
            glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
            sFtProg = p3;
            sVsKind[2] = vs3;
            Vita_Log("vgl: start: the fighters' buffers made\n");
        }
    }
    if (!(gVglSafe & (32768 | 131072)) && sFtProg != 0 && sStageProg != 0) { /* the direct path's vertex shaders */
        const char *const srcs[2] = {kVertFtD, kVertStD};
        const GLint lens[2] = {(GLint)sizeof(kVertFtD) - 1, (GLint)sizeof(kVertStD) - 1};
        int k;
        for (k = 0; k < 2; k++) {
            GLuint vsd = glCreateShader(GL_CG_VERTEX_SHADER_EXT);
            char *zc = zclamp_src(srcs[k], k == 0 ? "    vPos = sv * U[16] + sv.w * U[17];\n" : "    vPos = sv * U[4] + sv.w * U[5];\n", ZCLAMP_V, &len);
            src = zc != NULL ? zc : srcs[k]; ok = 0;
            (void)lens;
            Vita_Log("vgl: start: the direct %s vertex shader\n", k == 0 ? "fighter" : "stage");
            glShaderSource(vsd, 1, &src, &len);
            glCompileShader(vsd);
            glGetShaderiv(vsd, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                log[0] = 0; glGetShaderInfoLog(vsd, sizeof(log) - 1, NULL, log); log[sizeof(log) - 1] = 0;
                Vita_Log("vgl: direct %s vertex shader not compiled: %s\n", k == 0 ? "fighter" : "stage", log);
                break;
            }
            sVsKind[3 + k] = vsd;
        }
    }
    if (!(gVglSafe & 32768)) { /* every variant now, not in the middle of a fight */
        int kk, ff, built = 0;
        Vita_Log("vgl: start: the shader variants\n");
        for (kk = 0; kk < 5; kk++) {
            for (ff = 0; ff < 2048; ff++) {
                if (var_used_k(kk, ff)) {
                    built += variant(kk, ff) != NULL;
                }
            }
        }
        Vita_Log("vgl: %d shader variants built (textures read %s); pixel shaders: %u from the card, %u compiled and kept, %u compiled\n", built,
                 sNoProj ? "straight from the coordinates" : "divided by Q", gVitaShCache[0], gVitaShCache[1], gVitaShCache[2]);
        dp_init();
        ol_init();
        fx_init();
    }
}

/* ---- the game's vertex programs as ONE vertex shader (the PC back end's shaders/vu0.vert, vu4.vert, vu6.vert in
   Cg): gs_vu1.c hands over a program's batch as it lies in VU memory (48-byte vertices) with its constants, and
   the chip does what the interpreter did per vertex. uC: 0..3 matrix A (columns), 4..7 matrix B, 8 / 9 pivots,
   10..13 the screen matrix, 14 light, 15 / 16 colours, 17 x y: XYOFFSET in pixels, z: depth maximum, w: layer,
   18 second light, 19 x: 0 program 0's family, 1 program 4 (stage), 2 program 6 (ground shadow).
   Positions leave in clip space (the chip clips and interpolates with perspective); colours with 0x80 = 1, as the
   fragment shader above expects. ---- */
#define MAX_VU_VERTS 12288
static const char kVertVu[] =
    "uniform float4 uC[20];\n"
    "void main(float4 aPos, float4 aNrm, float4 aSt,\n"
    "          float4 out vPos : POSITION, float3 out vTex : TEXCOORD0, float4 out vCol : TEXCOORD1) {\n"
    "    float layer = uC[17].w;\n"
    "    float kind = uC[19].x;\n"
    "    float4 s;\n"
    "    float4 col = float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
    "    float2 st = aSt.xy;\n"
    "    if (kind < 0.5f) {\n"
    "        float w = (layer > 3.5f && layer < 4.5f) ? 1.0f : aPos.w;\n"
    "        float3 da = aPos.xyz - uC[8].xyz;\n"
    "        float3 db = aPos.xyz - uC[9].xyz;\n"
    "        float3 pa = uC[0].xyz * da.x + uC[1].xyz * da.y + uC[2].xyz * da.z + uC[3].xyz;\n"
    "        float3 pb = uC[4].xyz * db.x + uC[5].xyz * db.y + uC[6].xyz * db.z + uC[7].xyz;\n"
    "        float3 p = pb + (pa - pb) * w;\n"
    "        s = uC[10] * p.x + uC[11] * p.y + uC[12] * p.z + uC[13];\n"
    "        if (layer > 1.5f && layer < 2.5f && s.w < 0.0f) { s = -s; }\n"
    "        if (layer > 4.5f) {\n"
    "            float4 n1 = float4(aNrm.xyz, 1.0f);\n"
    "            col = uC[16];\n"
    "            st = float2(dot(uC[14], n1) * 0.5f + 0.5f, dot(uC[18], n1) * 0.5f + 0.5f);\n"
    "        } else if (layer > 2.5f) {\n"
    "            col = clamp(floor(float4(aNrm.xyz, layer > 3.5f ? uC[15].w : aNrm.w)), 0.0f, 255.0f);\n"
    "        } else if (layer < 0.5f || layer > 1.5f) {\n"
    "            col = uC[15];\n"
    "        } else {\n"
    "            float3 na = uC[0].xyz * aNrm.x + uC[1].xyz * aNrm.y + uC[2].xyz * aNrm.z;\n"
    "            float3 nb = uC[4].xyz * aNrm.x + uC[5].xyz * aNrm.y + uC[6].xyz * aNrm.z;\n"
    "            float la = dot(uC[14].xyz, na);\n"
    "            float lb = dot(uC[14].xyz, nb);\n"
    "            col = uC[16];\n"
    "            st = float2((lb + (la - lb) * w + uC[14].w) * 0.5f + 0.5f, 0.0f);\n"
    "        }\n"
    "    } else {\n"
    "        float4 p = (kind < 1.5f) ? aPos : float4(aPos.xyz, 1.0f);\n"
    "        s = uC[10] * p.x + uC[11] * p.y + uC[12] * p.z + uC[13] * p.w;\n"
    "        col = clamp(floor(aNrm), 0.0f, 255.0f);\n"
    "        if (kind > 1.5f) {\n"
    "            float4 c = uC[0] * p.x + uC[1] * p.y + uC[2] * p.z + uC[3];\n"
    "            st = (c.xy * uC[8].x + 1.0f) * 0.5f * uC[9].xy;\n"
    "        }\n"
    "    }\n"
    "    float z = 2.0f * s.z / uC[17].z - s.w;\n"
    "    if (layer > 1.5f && layer < 2.5f && kind < 0.5f) { z = clamp(z, -s.w, s.w); }\n"
    "    vPos = float4((s.x - uC[17].x * s.w) / 256.0f - s.w, s.w - (s.y - uC[17].y * s.w) / 224.0f, z, s.w);\n"
    "    vCol = col / 128.0f;\n"
    "    vTex = float3(st, 1.0f);\n"
    "}\n";
static GLuint sProgVu;
static GLint sVuUniMode, sVuUniTest, sVuUniTex, sVuUniC;
static float sVuVerts[MAX_VU_VERTS * 12];
static int sVuCount;
static State sVuState;
static float sVuUni[20][4];
int gVglVuReady;                 /* 1: gs_vu1.c hands the programs' batches over (vita_gfx.c answers its question) */
unsigned gVglVuTris, gVglVuBatches, gVglKeys;
unsigned gVglUs[6], gVglCalls[6]; /* where a drawn picture's time goes: 0 vu_draw, 1 state_of, 2 flush, 3 swap, 4 GsGpu_Draw, 5 GsVu1_Call */
extern unsigned long long sceKernelGetProcessTimeWide(void);
/* Where the time goes: the code says where it is (gProf), a thread looks once a millisecond and counts.
   0 the game, 1 the display list (the rest of it), 2 unpacking vectors, 3 GS packets, 4 uploads, 5 vertex-program
   calls, 6 their strips (vu_draw), 7 batches to the chip, 8 the swap, 9 GS primitives, 10 waiting for the blank. */
volatile unsigned char gProf;
volatile unsigned char gProfGame; /* the game's thread: 0 running, 1 waiting for the blank, 2 waiting for the picture thread */
unsigned gProfHist[2][12], gProfGameHist[3], gProfWorkHist;
extern volatile unsigned char gProfWork;
unsigned gVitaWaitSkips; /* lists not drawn because the game had to wait for the one before */
/* With libperf: one drawn list in sixteen is measured part by part (cycles, instructions, mispredicted branches,
   data cache misses per region of gProf); reading the counters is slow, so only then. */
volatile int gVitaProfMeasure;
void Vita_ProfSwitch(int n);
#define P_IN(n) unsigned char p_keep = gProf; gProf = (n); if (gVitaProfMeasure) { Vita_ProfSwitch(n); }
#define P_OUT gProf = p_keep; if (gVitaProfMeasure) { Vita_ProfSwitch(p_keep); }
static int prof_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        sceKernelDelayThread(1000);
        gProfHist[gVitaGsSkip ? 0 : 1][gProf < 12 ? gProf : 11]++;
        gProfGameHist[gProfGame < 3 ? gProfGame : 0]++;
        gProfWorkHist += gProfWork;
    }
    return 0;
}
extern volatile int gVitaZoneOn;
void Vita_Z(int z);
#define ZM(z) do { if (gVitaZoneOn) { Vita_Z(z); } } while (0)
#define T_IN unsigned long long t_in = sceKernelGetProcessTimeWide()
#define T_OUT(i) (gVglUs[i] += (unsigned)(sceKernelGetProcessTimeWide() - t_in), gVglCalls[i]++)

static void vu_shader_init(void) {
    GLuint vs = glCreateShader(GL_CG_VERTEX_SHADER_EXT), fs = glCreateShader(GL_CG_FRAGMENT_SHADER_EXT), prog;
    const char *src;
    GLint len, ok = 0;
    char log[400];

    {
        char *zc = zclamp_src(kVertVu, "    float z = 2.0f * s.z / uC[17].z - s.w;\n", "    if (s.w > 0.0f) { z = clamp(z, -s.w, s.w); }\n", &len);
        src = zc != NULL ? zc : kVertVu;
        glShaderSource(vs, 1, &src, &len);
        free(zc);
    }
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        log[0] = 0; glGetShaderInfoLog(vs, sizeof(log) - 1, NULL, log); log[sizeof(log) - 1] = 0;
        Vita_Log("vgl: vertex-program shader not compiled: %s\n", log);
        return;
    }
    src = kFrag; len = (GLint)sizeof(kFrag) - 1; ok = 0;
    glShaderSource(fs, 1, &src, &len);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        return;
    }
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aNrm");
    glBindAttribLocation(prog, 2, "aSt");
    glLinkProgram(prog);
    sVuUniMode = glGetUniformLocation(prog, "uMode");
    sVuUniTest = glGetUniformLocation(prog, "uTest");
    sVuUniTex = glGetUniformLocation(prog, "uTex");
    sVuUniC = glGetUniformLocation(prog, "uC");
    Vita_Log("vgl: vertex-program shader built (uniforms %d %d %d %d)\n", (int)sVuUniMode, (int)sVuUniTest, (int)sVuUniTex, (int)sVuUniC);
    if (sVuUniMode <= 0 || sVuUniTest <= 0 || sVuUniC <= 0) {
        return;
    }
    sProgVu = prog;
    gVglVuReady = 1;
}

static void scissor(int x, int y, int w, int h);
static void scissor_of(const struct State *s);
/* The chip's state as last set by flush_impl: a call that would set it to what it already is is left out (each
   costs vitaGL work, 300+ batches a picture). Anything else that touches the state lets go of all of it. */
static struct { int valid; GLuint prog, tex; int texp, sten, blend, depth, mask; } sGlc;
#define GLC_RESET() (sGlc.valid = 0)
static void target_set(int t); /* the picture or the work page bound for what is drawn next */
static volatile int sMidOn;    /* the frame's middle is filmed (wtshot=1), below */
static void ol_init(void);     /* the fighters' outline, below */
static void ol_draw(const uint8_t *p);
static void ids_exec(void);     /* the effects' passes, below */
static void tbl_exec(const uint8_t *p);
#define TGT_IDS 14             /* State.target / target_set: the copy of the ids */
#define TGT_GLOW 12            /* 12, 13: the native glow's two 64 x 64 pictures */
#define TGT_FOG 11             /* the fog byte worked out once from the depth (GfxDepthFog_Draw) */
#define TGT_XF 10              /* the cross-fade's copy of the picture shown last */
static GLuint sFogFbo, sFogTex;
static int sFogOk;             /* (the thread that draws) sFogTex holds the fog byte */
static void fog_exec(const uint8_t *p);
static void xf_exec(const uint8_t *p);  /* the cross-fade, below */
static GLuint sXfFbo, sXfTex, sXfProg;
static GLint sXfU[3];
static int sXfOk;
static GLuint sIdsFbo, sIdsTex;
static GLuint sGlowFbo[2], sGlowTex[2];
static int sGlowSkip;          /* between the glow's markers: its own passes dropped */
static int sOlTail;            /* after the outline's marker: the game's own outline passes
                                  that follow it (gfxm_a.c ObjOutline_Draw: the edge image into the alpha, the darkening
                                  by it) dropped, the outline being drawn natively at the marker */
static unsigned sOlTailN[2];   /* (for the log) those dropped: edge image reads, darkenings */
static int sTblZero;           /* the last table pass's alphas were all 0: the glow after it adds nothing */
static int sSeeThru;           /* a model drawn into the alpha only in this picture (seen through) */
static unsigned sGlowN, sGlowNull; /* (for the log) glows drawn, left out */
static int sMainZ = 1;         /* depth written into the picture since its scene last
                                  stored it (a pass that reads the depth splits the scene only then) */
static void glow_exec(const uint8_t *p);
static void glow_init(void);
static int sIdsOk;             /* the thread that draws: the copy of the ids made in this picture */
static int tbl_intercept(int type, int ctx);
static int sTblOn;
static int sTblState;          /* state_of_: the texture left out (a table pass: its own shader reads the source) */
static GLuint sOlProg;
static GLuint sOlTex[3];       /* (the display buffers as textures, below) */
extern unsigned int gxm_back_buffer_index;
static int sTexXf;             /* the primitive's texture is a picture drawn here: its coordinates moved into it (put) */
static float sTexXfV[4];       /* s' = [0] + s * [1], t' = [2] + t * [3] */
extern unsigned gVitaOl[4];
static void rt_sync(void); /* (the render thread's work all done; see rt_start) */
static void rt_start(void);
static int sRtOn;
static void flush_vu(void) {
    GLC_RESET();
    static const GLenum kFactor[6] = {GL_ZERO, GL_ONE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    const State *s = &sVuState;

    if (sVuCount == 0) {
        return;
    }
    rt_sync();
    target_set(0);
    glUseProgram(sProgVu);
    if (s->tme && s->tex != 0) {
        glBindTexture(GL_TEXTURE_2D, s->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s->linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s->linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, s->wrapS ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, s->wrapT ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        if (sVuUniTex > 0) {
            glUniform1i(sVuUniTex, 0);
        }
        glUniform4f(sVuUniMode, s->env == 1 ? 2.0f : s->env == 2 ? 3.0f : 1.0f, 0.0f, 0.0f, 0.0f);
    } else {
        glUniform4f(sVuUniMode, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    glUniform4f(sVuUniTest, s->atest ? (float)(s->afunc + 1) : 0.0f, (float)s->aref / 128.0f, 0.0f, 0.0f);
    glUniform4fv(sVuUniC, 20, &sVuUni[0][0]);
    if (s->blend) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(kFactor[s->src], kFactor[s->dst], GL_ONE, GL_ZERO);
        glBlendEquationSeparate(s->eq == 1 ? GL_FUNC_REVERSE_SUBTRACT : s->eq == 2 ? GL_FUNC_SUBTRACT : GL_FUNC_ADD, GL_FUNC_ADD);
    } else {
        glDisable(GL_BLEND);
    }
    glDisable(GL_ALPHA_TEST);
    if (s->ztest) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(s->zfunc == 0 ? GL_NEVER : s->zfunc == 1 ? GL_ALWAYS : s->zfunc == 2 ? GL_GEQUAL : GL_GREATER);
        glDepthMask(s->zwrite ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glColorMask(s->mask & 1 ? GL_TRUE : GL_FALSE, s->mask & 2 ? GL_TRUE : GL_FALSE, s->mask & 4 ? GL_TRUE : GL_FALSE, s->mask & 8 ? GL_TRUE : GL_FALSE);
    scissor_of(s);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 48, &sVuVerts[0]);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 48, &sVuVerts[4]);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 48, &sVuVerts[8]);
    glDrawArrays(GL_TRIANGLES, 0, sVuCount);
    gVglVuBatches++;
    gVglVuTris += (unsigned)sVuCount / 3;
    sVuCount = 0;
}

int gVglSafe;            /* to find a fault. 1 no textures, 2 no blending, 4 no alpha test, 8 no scissor, 16 the
                            fixed-function pipeline instead of the shader, 32 every upload written into GS memory
                            at once (gs_core.c), 64 films decoded on the game's thread (plat_movie.c) */
int gVglSafe2;           /* more of the same: 1 a fight's picture handed to the chip whole (not in parts) */
/* A fight's picture handed to the chip in parts: the chip worked on a picture only once it was all drawn
   (21 ms after the swap in the busy stretches, idle before), so a picture was late whenever the drawing thread's
   time and the chip's came to more than the blanks allowed. Now the part drawn so far goes to the chip every few ms
   (vitaGL change: a new scene on the same surfaces, the depth kept), and the chip's work overlaps the drawing. */
extern GLboolean vglSplitScene(void);
extern int vgl_bt3_split_on;
extern unsigned vgl_bt3_splits;
#define SPLIT_MS 7
#define SPLIT_MAX 4
static unsigned long long sSplitAt; /* the next part's end (0: none in this picture) */
static int sSplitN;                  /* parts ended in this picture */
static unsigned sSplitPics;          /* for the log: pictures drawn in parts */
static int sLogFrames = 3; /* (and again whenever sLogMore is set: after the first texture) */
static int sLogMore = 1; /* the first drawn frames are logged batch by batch */
static unsigned sBadVerts;

static SceDisplayFrameBuf sTextFb; /* the text screen that was up before the picture */

/* Back to the text screen (the end of the run, a fatal message). */
void Vita_TextScreen(void) {
    if (sTextFb.base != NULL) {
        extern void Port_GsSync(void);
        Port_GsSync(); /* (the picture thread is not in the middle of a frame) */
        rt_sync();
        glFinish();
        sceKernelDelayThread(100000);
        sceDisplaySetFrameBuf(&sTextFb, SCE_DISPLAY_SETBUF_NEXTFRAME);
        sceKernelDelayThread(50000);
    }
}

/* The scissor only when it changes: vitaGL keeps it as a mask in the stencil buffer and redraws that mask over the
   whole screen for every glScissor (two screen-sized quads). */
static int sScissor[4] = {-1, -1, -1, -1};

static int sCovSample;
unsigned gVglScissorN; /* for the log: scissor changes (vitaGL draws two full-picture passes over the depth buffer's mask bit for each) */
static void run_add(int k, int n, float area);
/* a state's scissor: the picture's in GS pixels (state_of_), turned into the display's at the size of the
   moment; a work target's or the work page's in their own pixels */
static void scissor_of(const State *s) {
    if (s->target == 0) {
        int x0 = s->sx, y0 = s->sy, x1 = s->sw, y1 = s->sh;
        int px = VIEW_X + x0 * VIEW_W / 512;
        int py = (448 - y1) * VIEW_H / 448; /* OpenGL counts from the bottom */
        scissor(px, py, VIEW_X + (x1 * VIEW_W + 511) / 512 - px, ((448 - y0) * VIEW_H + 447) / 448 - py);
    } else {
        scissor(s->sx, s->sy, s->sw, s->sh);
    }
}
static void scissor(int x, int y, int w, int h) {
    if (x != sScissor[0] || y != sScissor[1] || w != sScissor[2] || h != sScissor[3]) {
        glScissor(x, y, w, h);
        sScissor[0] = x; sScissor[1] = y; sScissor[2] = w; sScissor[3] = h;
        gVglScissorN++;
        if (sCovSample && !sRtOn) { /* (the render thread's scissors are not in the sample) */
            run_add(-3, 1, (float)(w * h)); /* (the log's sample: "S" and the area in chip pixels) */
        }
    }
}

/* ---- the work page: the ground shadows. The game draws each fighter from straight above into a
   256 x 256 page of GS memory (0x150, after clearing it) and lays that picture on the stage under the fighter
   (VU1 program 6, textured from block 0x2A00): src/sys/gfxm_e_b.c. That page is a framebuffer object here: GS
   primitives drawn into it go there, and a draw textured from it takes its picture, as long as everything drawn
   into it since it was last cleared was drawn (the glow and the outline use the same page through a 16-bit view or
   from the picture itself: then it is spoiled, and what reads it is left out, as before). In a fight only (the
   display's depth is kept across the switch: vitaGL change, vglBt3ContNext). ---- */
#define WP_FBP 0x150
extern void vglBt3ContNext(void), vglBt3Dirty(void);
/* the page in slots, one per shadow of the game (gfxm_e_b.c: its place in the shadow pool, given by a
   marker before the shadow's passes) and one for anything else: a fighter's silhouette stays in its slot, and on
   every other picture the game draws none and the shadow is laid with the one drawn before (Port_ShadowSil) */
#define WP_SLOTS 6
#define WP_OTHER 5
static GLuint sWpTexK[WP_SLOTS], sWpFboK[WP_SLOTS];
static int sWpSlot = WP_OTHER;  /* the slot the page is now */
static int sWpAgain = -1;       /* a marker said: the shadow of this slot with the silhouette drawn before (none now) */
static int sWpMarked;           /* the game sends the markers (its other draws into the page are not drawn: never read whole) */
static uint8_t sWpSlotOk[WP_SLOTS]; /* a silhouette was drawn whole into the slot (and laid on the ground) */
static int sWpReady;     /* the framebuffer object exists */
static int sWpAllow;     /* state_of_ called for a GS primitive: its target may be the page */
static int sWpValid;     /* the page holds just what was drawn into it since it was cleared */
static float sWpArea;    /* the area of the clear so far */
static int sTgtCur;      /* the target bound (the thread that draws): 0 the picture, 1 + n the page's slot n */
int gVitaWpOff;          /* the page not used for now (the log's pictures without it) */
unsigned gVitaWp[12];    /* for the log: 0 drawn into the page, 1 refused there (page spoiled), 2 clears, 3 reads given the page, 4 refused (not whole), 5 refused (form), 6 switches,
                            7 clears drawn, 8 silhouette strips drawn directly, 9 silhouette strips the other way, 10 silhouettes begun, 11 shadows with the one before */
static void wp_init(void) {
    extern int gVglSafe2;
    if (gVglSafe2 & 32) {
        return; /* (bt3.txt safe2=32: no work page) */
    }
    int k;
    sWpReady = 1;
    for (k = 0; k < WP_SLOTS; k++) {
        glGenTextures(1, &sWpTexK[k]);
        glBindTexture(GL_TEXTURE_2D, sWpTexK[k]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &sWpFboK[k]);
        glBindFramebuffer(GL_FRAMEBUFFER, sWpFboK[k]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sWpTexK[k], 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        sWpReady = sWpReady && sWpTexK[k] != 0 && sWpFboK[k] != 0;
    }
    Vita_Log("vgl: work page for the ground shadows: %s (%d slots; textures %u..%u)\n", sWpReady ? "made" : "NOT made", WP_SLOTS, (unsigned)sWpTexK[0], (unsigned)sWpTexK[WP_SLOTS - 1]);
}
/* ---- Work targets: the effects' work buffers (the glare's and the object glow's chain: the picture
   halved into page 0x150, halved again into 0x170, to 64 x 64 in 0x1F6 and blurred between it and 0x1F8, back up and
   added to the picture; the haze, the blurs) as framebuffer objects, as the PC renderer has them (gs_gpu.c: any page
   drawn into is a target, one drawn into another is an ordinary textured sprite). A target is read when everything
   drawn into it was drawn here and nothing was uploaded over its memory since; the picture itself is read from the
   display buffer (the scene drawing it has ended: the switch to the target ends it). In a fight only. ---- */
#define WT_MAX 12
#define WT_BASE 16 /* State.target: 0 the picture, 1..6 the shadow page's slots, 16 + n the work target n */
typedef struct WorkTarget { uint32_t fbp, fbw; int w, h; GLuint tex, fbo; uint8_t drawn, stale; uint32_t sig; unsigned last; float cov; } WorkTarget;
static WorkTarget sWt[WT_MAX];
static int sWtN;
static unsigned gVitaBoneBad;      /* fighters' bones or vertices that were not finite */
static unsigned sWtRej[WT_MAX][8]; /* primitives into a target refused, by the reason (sRejWhy) */
static int sWtOn;           /* (made at the start: bt3.txt safe2=128 leaves them out) */
int gVitaFxOff;             /* (the log's pictures without the effects: with the work page off) */
extern int gVitaSliver;
int gVitaFxSweep;           /* the effects left out in turn, 60 pictures each, the chip's time logged by which */
int gVitaFxMask;            /* 1 the table passes, 2 the glow, 4 the work targets' effects, 8 the outline */
#define FX_WT_OFF (gVitaFxOff || (gVitaFxMask & 4))
int gVitaClipOff;           /* the fighters' reject of program 0 not done (the log's pictures; bt3.txt safe2=512) */
static unsigned sTblCbp[8][3]; /* table passes by palette block: block, drawn, not */
static void tbl_count(uint32_t cbp, int drawn) {
    int k;
    for (k = 0; k < 8; k++) {
        if (sTblCbp[k][0] == cbp || sTblCbp[k][0] == 0) {
            sTblCbp[k][0] = cbp;
            sTblCbp[k][drawn ? 1 : 2]++;
            return;
        }
    }
}
unsigned gVitaWt[10];       /* for the log: 0 primitives drawn into targets, 1 refused (a pass missed), 2 reads given a target, 3 reads refused (stale or overwritten),
                               4 reads of the picture, 5 table passes, 6 of them from the fog, 7 copies of the ids, 8 table passes refused, 9 targets made */
static uint32_t wt_sig(const WorkTarget *t) { /* (the pages' memory as the uploads left it, see wp_sig) */
    extern uint32_t gGsPageSig[512];
    uint32_t h = 2166136261u, k, pages = (uint32_t)(t->w * t->h * 4 + 8191) / 8192;
    for (k = 0; k < pages; k++) {
        h = (h ^ gGsPageSig[(t->fbp + k) & 511]) * 16777619u;
    }
    return h;
}
static int ok_cacheable_wt(const WorkTarget *t) {
    (void)t;
    return 1;
}
/* (by the page and the width: the same memory is a 256-wide work page for the glow and a 512-wide one for the haze;
   each is a target of its own, not kept in step with the other) */
static WorkTarget *wt_find(uint32_t fbp, uint32_t fbw) {
    int i;
    for (i = 0; i < sWtN; i++) {
        if (sWt[i].fbp == fbp && (fbw == 0 || sWt[i].fbw == fbw)) {
            return &sWt[i];
        }
    }
    return NULL;
}
/* (the drawing thread: made here, used by the thread that draws in order) */
static WorkTarget *wt_get(uint32_t fbp, uint32_t fbw) {
    WorkTarget *t = wt_find(fbp, fbw);
    int w = (int)fbw * 64, h = fbw >= 8 ? 448 : w;
    if (t != NULL) {
        return t;
    }
    if (sWtN == WT_MAX || fbw == 0 || fbw > 8 || (fbw & (fbw - 1)) != 0) {
        return NULL;
    }
    t = &sWt[sWtN];
    memset(t, 0, sizeof(*t));
    rt_sync();
    GLC_RESET();
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    {   /* black with alpha 0, as an empty page: a target is read where nothing was drawn into it yet (the
           pan blur blends the picture into the glow's page, which the native glow no longer draws: a test's black
           blocks were the memory's leftovers) */
        void *zero = calloc((size_t)w * (size_t)h, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
        free(zero);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    sTgtCur = -1; /* (bound again before the next draw) */
    if (t->tex == 0 || t->fbo == 0) {
        Vita_Log("vgl: work target for page %03x NOT made\n", (unsigned)fbp);
        return NULL;
    }
    t->fbp = fbp;
    t->fbw = fbw;
    t->w = w;
    t->h = h;
    sWtN++;
    gVitaWt[9]++;
    Vita_Log("vgl: work target %d: page %03x, %d x %d\n", sWtN - 1, (unsigned)fbp, w, h);
    return t;
}
/* (on the thread that draws, in the order of the draws) */
static void target_set(int t) {
    if (t == sTgtCur) {
        return;
    }
    if (sTgtCur == 0) {
        sMainZ = 0; /* (the picture's scene ends here: its depth is stored) */
    }
    if (t == TGT_IDS) { /* the copy of the ids */
        glBindFramebuffer(GL_FRAMEBUFFER, sIdsFbo);
        glViewport(0, 0, SCR_W, SCR_H);
    } else if (t == TGT_FOG) { /* the fog byte */
        glBindFramebuffer(GL_FRAMEBUFFER, sFogFbo);
        glViewport(0, 0, SCR_W, SCR_H);
    } else if (t == TGT_XF) { /* the cross-fade's copy */
        glBindFramebuffer(GL_FRAMEBUFFER, sXfFbo);
        glViewport(0, 0, SCR_W, SCR_H);
    } else if (t == TGT_GLOW || t == TGT_GLOW + 1) { /* the native glow */
        glBindFramebuffer(GL_FRAMEBUFFER, sGlowFbo[t - TGT_GLOW]);
        glViewport(0, 0, 64, 64);
    } else if (t >= WT_BASE) { /* a work target */
        glBindFramebuffer(GL_FRAMEBUFFER, sWt[(t - WT_BASE) % WT_MAX].fbo);
        glViewport(0, 0, sWt[(t - WT_BASE) % WT_MAX].w, sWt[(t - WT_BASE) % WT_MAX].h);
    } else if (t != 0) { /* (1 + the page's slot) */
        glBindFramebuffer(GL_FRAMEBUFFER, sWpFboK[(t - 1) % WP_SLOTS]);
        glViewport(0, 0, 256, 256);
    } else {
        vglBt3ContNext();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(VIEW_X, 0, VIEW_W, VIEW_H);
    }
    vglBt3Dirty(); /* (the next scene's uniform buffers) */
    GLC_RESET();
    sScissor[0] = sScissor[1] = sScissor[2] = sScissor[3] = -1;
    sTgtCur = t;
    gVitaWp[6]++;
}
/* A clear of the page is held back until something is drawn into the page: the glow clears it and then
   spoils it at once (the picture drawn into it is not drawn here), and each switch to the page and back costs the
   chip a scene. Dropped when the page is spoiled first; drawn (as one sprite when it covers the page) before the
   first draw into it. */
static State sWpClrSt;
static GsVertex sWpClrV;
static float sWpClrZmax;
static float sWpClr[16][4];
static int sWpClrN, sWpClrWhole;
static int wp_is_tex(GLuint t) {
    int k;
    for (k = 0; k < WP_SLOTS; k++) {
        if (t != 0 && t == sWpTexK[k]) {
            return 1;
        }
    }
    return 0;
}
/* gfxm_e_b.c's markers: 0x20 + n the silhouette of shadow n is drawn now (into slot n), 0x28 + n the
   shadow n is laid with the silhouette drawn before, 0x2F the shadows are over */
static void wp_marker(int m) {
    extern int gVitaGsSkip;
    if (!sWpReady || gVitaGsSkip) {
        return; /* (a list not drawn: the slots keep what they hold) */
    }
    sWpMarked = 1;
    if (m >= 0x20 && m < 0x20 + WP_OTHER) {
        sWpSlot = m - 0x20;
        sWpAgain = -1;
        sWpSlotOk[sWpSlot] = 0;
        sWpValid = 0;
        sWpArea = 0.0f;
        sWpClrN = 0; /* (a clear held for another slot is not this one's) */
        sWpClrWhole = 0;
        gVitaWp[10]++;
    } else if (m >= 0x28 && m < 0x28 + WP_OTHER) {
        sWpSlot = WP_OTHER;
        sWpAgain = m - 0x28;
        gVitaWp[11]++;
    } else if (m == 0x2F || m == 0x20 + WP_OTHER) {
        sWpSlot = WP_OTHER;
        sWpAgain = -1;
    }
}
static void wp_spoil(void) {
    sWpValid = 0;
    sWpArea = 0.0f;
    sWpClrN = 0;
    sWpClrWhole = 0;
}
/* the page's GS memory as the uploads left it: a texture read from 0x2A00 is the page's picture only
   while nothing was uploaded there since the last draw into the page (the game streams stage textures through the
   same memory: an earlier version took those for the page and left the ground out) */
static uint32_t sWpSig;
static int sWpSigSet;
static uint32_t wp_sig(void) {
    extern uint32_t gGsPageSig[512];
    uint32_t h = 2166136261u;
    int k;
    for (k = 0; k < 32; k++) {
        h = (h ^ gGsPageSig[(WP_FBP + k) & 511]) * 16777619u;
    }
    return h;
}

static void frame_begin(void) {
    rt_sync();
    {   /* the frame about to be drawn is one the film takes: its middle too (wtshot=1) */
        extern int Vita_InFight(void);
        extern int gVitaFilm, gVitaWtShot;
        extern unsigned gVitaFilmPicsNext(void);
        sMidOn = gVitaWtShot && gVitaFilm > 0 && Vita_InFight() && gVitaFilmPicsNext() % (unsigned)gVitaFilm == 0 ? 1 : 0;
    }
    target_set(0);
    {   /* a fight's picture in parts: decided before the picture's first scene is begun (by the clear) */
        extern int Vita_InBattle(void);
        vgl_bt3_split_on = Vita_InBattle() && !(gVglSafe2 & 1);
        sSplitN = 0;
        sSplitAt = vgl_bt3_split_on ? sceKernelGetProcessTimeWide() + SPLIT_MS * 1000ull : 0;
    }
    GLC_RESET();
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
}

static int vj_thread(SceSize args, void *argp);
static int sVjOn;
int gVitaMsaa; /* bt3.txt msaa=<0, 2, 4> */
int GsGpu_Init(void) {
    memset(&sTextFb, 0, sizeof(sTextFb));
    sTextFb.size = sizeof(sTextFb);
    if (sceDisplayGetFrameBuf(&sTextFb, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0) {
        sTextFb.base = NULL;
    }
    Vita_Log("vita: starting vitaGL\n");
    /* stream vertex buffers (one per batch, 300+ a picture) from vitaGL's per-frame ring, not each its own allocation
       freed by the garbage collector (port/vita/vitagl: vitaGL built with USE_SCRATCH_MEMORY=1) */
    vglSetupScratchMemory(GL_TRUE, GL_TRUE);
    /* larger sceGxm rings: with ~250 draws a frame carrying ~3 KB of uniforms each, the default 2 MB vertex ring and
       128 KB VDM ring can fill, and a full ring makes the draw call wait for the chip */
    vglSetVertexBufferSize(8 * 1024 * 1024);
    vglSetVDMBufferSize(512 * 1024);
    vglSetFragmentBufferSize(2 * 1024 * 1024);
    if (gVitaPbufMb > 0) { /* (bt3.txt pbuf=<MB>) the parameter buffer: the chip's room for a scene's transformed
                              vertices and tile lists, shared by the scenes in flight (full: the chip draws what it has
                              in pieces, storing and loading the picture) */
        extern void vglSetParamBufferSize(uint32_t size);
        vglSetParamBufferSize((uint32_t)gVitaPbufMb * 1024u * 1024u);
        Vita_Log("vgl: parameter buffer %d MB\n", gVitaPbufMb);
    }
    vglWaitVblankStart(GL_FALSE);
    /* the display's render target made for 8 scenes in flight, not 1: a picture is handed to the chip in
       parts (the time splits, the work page's switches, the outline), and with room for one scene the start of each
       part waited for the chip to finish the part before (the render thread: 10 ms a picture with the outline) */
    {
        extern void vglSetupRenderTargetScenesNum(uint8_t size_main, uint8_t size_fbos);
        vglSetupRenderTargetScenesNum(8, 8);
    }
    /* (bt3.txt msaa=2 or 4) the chip's multisampling: polygon edges smoothed (the picture is 640 x 368,
       stretched to the screen: edges crawl as the camera moves). The depth the fog reads is then 2 x 2 (or 1 x 2)
       samples a pixel: fx_init. */
    if (gVitaMsaa != 0) {
        gVitaMenuHi = 0; /* (the menus' size only without multisampling) */
    }
    /* the display memory made for the menus' size when they are drawn at it; the fights' size from the
       first picture on (the boot logos and the films are not the menus) */
    vglInitExtended(0x1000000, gVitaMenuHi ? SCR_W_MENU : SCR_W_FIGHT, gVitaMenuHi ? SCR_H_MENU : SCR_H_FIGHT, 0x1800000,
                    gVitaMsaa == 4 ? SCE_GXM_MULTISAMPLE_4X : gVitaMsaa == 2 ? SCE_GXM_MULTISAMPLE_2X : SCE_GXM_MULTISAMPLE_NONE);
    Vita_Log("vgl: multisampling %s; menus at %s\n", gVitaMsaa == 4 ? "4x" : gVitaMsaa == 2 ? "2x" : "off",
             gVitaMenuHi ? "960 x 544" : "640 x 368");
    vglSetDisplayCallback(pace_cb); /* (the pictures shown in step with the game's clock) */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepthf(0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (gVitaMenuHi) {
        vglSwapResolution(SCR_W_FIGHT, SCR_H_FIGHT); /* (made at the swap below) */
    }
    vglSwapBuffers(GL_FALSE);
    sScrW = SCR_W_FIGHT;
    sScrH = SCR_H_FIGHT;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    sViewW = gVitaWide ? SCR_W : SCR_W * 725 / 960;
    glViewport(VIEW_X, 0, VIEW_W, VIEW_H);
    glDisable(GL_CULL_FACE);
    if (!(gVglSafe & 16)) {
        shader_init();
        gVglVuReady = sProg != 0 && !(gVglSafe & 128); /* gs_vu1.c hands the vertex programs' batches over (vu_draw) */
    }
    Vita_Log("vgl: %s\n", sProg != 0 ? "drawing with the GS shader" : "drawing with the fixed-function pipeline");
    if (sProg != 0) {
        wp_init();
    }
    frame_begin();
    {
        extern SceUID Vita_ThreadOnCore3(const char *name, SceKernelThreadEntry entry, int prio, int stack, int fallbackMask);
        SceUID th = Vita_ThreadOnCore3("bt3_prof", prof_thread, 64, 0x4000, 0x00020000 /* (else the list reader's core, not the game's) */);
        { extern void Vita_WatchThread(int slot, int id, const char *name); Vita_WatchThread(4, th, "sampler"); }
        if (th >= 0) {
            sceKernelStartThread(th, 0, NULL);
        }
    }
    if (gVglVuReady && !(gVglSafe & 512)) { /* the strips' vertices on a thread of their own, read where they lie in the list */
        extern int gVitaLazyUnpack;
        SceUID th = sceKernelCreateThread("bt3_strips", vj_thread, 0x10000100, 0x10000, 0, 0, NULL);
        if (th >= 0 && sceKernelStartThread(th, 0, NULL) >= 0) {
            sVjOn = 1;
        }
        gVitaLazyUnpack = 1;
    }
    rt_start(); /* (last, vitaGL is set up) */
    Vita_Log("vita: vitaGL back end started\n");
    return 1;
}

/* ---- textures: GS memory -> RGBA, cached by TEX0, TEXA and a hash of the pages they occupy ---- */
unsigned long long gVitaTexMakeUs; /* for the log: the time new textures took (read, converted, handed to vitaGL) */
/* New textures a picture may make for blended GS primitives (the effects): past 4 ms of making textures in one
   picture, such a primitive waits for the next picture (it is not drawn now). An attack's first picture asked for
   100 at once (8 ms: a picture late); its effects now come in over a few pictures. Models and opaque
   primitives are never kept waiting. */
static int sTexMayDefer;          /* GsGpu_Draw_impl: the primitive asking is the GS's own */
/* a filmed list's signature, to tell two runs of the same demo fight apart: the GS primitives as the
   list gives them (registers, vertices) with their textures' contents (gen), those refused by reason, the deferred
   ones' TEX0, the stage's strips with their textures' contents, the textures made */
static uint32_t sSigGs, sSigGsN, sSigRej[8], sSigSt, sSigStN, sSigTexMade, sSigGenCur, sSigGsT, sSigStT;
static uint64_t sSigDefT0[6];
static int sSigDefN;
#define SIG_MIX(h, x) ((h) = ((h) ^ (uint32_t)(x)) * 16777619u, (h) ^= (h) >> 13)
extern int gVitaFilm; /* (the signature only with the film on: a test run) */
static int sTexDeferOk, sTexDeferredNow;
static unsigned sTexBudgetFrame;
static unsigned long long sTexBudgetUs;
unsigned gT126Deferred;           /* for the log: primitives kept waiting for their texture */
unsigned long long gT125Us[6];    /* for the log: textures with a palette, by part: numbers of GS memory, indices (without the reading), palette, vitaGL; making room; the most in one picture */
static unsigned long long sT125Pic; static unsigned sT125PicFrame;
unsigned long long gVitaTexReadUs, gVitaTexPx; /* of it: reading GS memory; and the pixels made */
float gVglCov[2];                  /* for the log: GS primitives' area in GS pixels, opaque / blended */
static int sCovSample, sCovN;      /* a picture whose GS primitives are listed by state (the log) */
static struct { uint32_t key; uint64_t t0, alpha, test; State d; int type, n, amin, amax; float area, skip, qr; } sCov[64];
static float sCovQr; /* (the primitive being added: its largest Q over its smallest) */
unsigned gVglSkip[4];              /* for the log: GS primitives left out without alpha, writing only what nothing reads; draws that read the picture's alpha */
float gVglSkipArea[2];             /* (their area in GS pixels) */
static unsigned sAlphaRead;        /* the last picture in which a draw read the picture's alpha (a blend factor, TEST.DATE) */
/* The GPU profile (a window of the fight): each picture is drawn with a part left out, in turn, and the time the
   chip works after the last draw is handed over is measured: what each part costs the chip. -1: off */
static int sProf = -1;
#define PROF_MODES 11
static unsigned long long sProfUs[PROF_MODES];
static unsigned sProfN[PROF_MODES];
static float sProfQlin; /* the picture's GS triangles with the colour across the screen (area) */
int gVitaGpuProf, gVitaGpuProfFrom = 1450, gVitaGpuProfTo = 2150; /* bt3.txt gpuprof=1, gpuproffrom=, gpuprofto= (lists) */
int gVitaQlinOpt = 1; /* bt3.txt qlinopt=0: as an earlier version (every GS triangle whose Q differs: the full-precision division) */
static struct { int16_t k; uint16_t n; float area; } sRun[320]; /* the sampled picture's draws in order: runs of a state (k >= 0), or models (-1 kept parts and fighters, -2 batches) */
static int sRunN;
static void run_add(int k, int n, float area) {
    if (sRunN > 0 && sRun[sRunN - 1].k == k) {
        sRun[sRunN - 1].n = (uint16_t)(sRun[sRunN - 1].n + n);
        sRun[sRunN - 1].area += area;
    } else if (sRunN < 320) {
        sRun[sRunN].k = (int16_t)k;
        sRun[sRunN].n = (uint16_t)n;
        sRun[sRunN].area = area;
        sRunN++;
    }
}
unsigned gVitaBeList[3];           /* for the log (gs_core.c): the longest drawn list (us), lists over 30 ms, over 33 ms */
static uint32_t bucket_of(uint64_t t0, uint64_t texa, uint32_t gen) {
    uint64_t h = (t0 ^ (texa * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)gen << 17)) * 0xD6E8FEB86659FD93ull;
    return (uint32_t)(h >> 40) & (TEX_BUCKETS - 1);
}

static void rebuild_buckets(void) {
    int i;

    memset(sTexBucket, 0, sizeof(sTexBucket));
    for (i = 0; i < sTexCount; i++) {
        uint32_t h = bucket_of(sTex[i].tex0, sTex[i].texa, sTex[i].gen);
        while (sTexBucket[h] != 0) {
            h = (h + 1) & (TEX_BUCKETS - 1);
        }
        sTexBucket[h] = (uint16_t)(i + 1);
    }
    sLast = NULL;
}

extern unsigned long long gVitaWall[6];
/* A number for what GS memory holds where a texture (and its palette) lies: the generations of the 8 KB pages it
   can occupy (gs_core.c bumps a page's when an upload writes into it). A texture's key, and a state made with it,
   stay good while this does, whatever was uploaded elsewhere (the global epoch changes with every upload: 300 a
   picture, and the keys and states were made again after nearly each one). */
extern uint32_t gGsPageSig[512];
extern unsigned gVitaSigCheck[4];
static unsigned sSigN; /* (the page-number checks counted) */
/* the texture's own pages only (its palette's aside) */
static uint32_t tex_sig_t(uint64_t t0) {
    uint32_t tbp = (uint32_t)(t0 & 0x3FFF), tbw = (uint32_t)((t0 >> 14) & 0x3F), psm = (uint32_t)((t0 >> 20) & 0x3F), th = 1u << ((t0 >> 30) & 15);
    uint32_t bits = (psm == 0x1B || psm == 0x24 || psm == 0x2C) ? 32u : (uint32_t)Gs_PsmBits(psm);
    uint32_t rowbytes = (tbw ? tbw : 1) * 64u * bits / 8u, first = tbp / 32, n = (th * rowbytes + 8191u) / 8192u + 1u, k, sig = 0x811C9DC5u;
    if (n > 128) {
        n = 128;
    }
    for (k = 0; k < n; k++) {
        sig = (sig ^ gGsPageSig[(first + k) & 511]) * 16777619u;
    }
    return sig;
}
static uint32_t tex_sig(uint64_t t0) {
    uint32_t psm = (uint32_t)((t0 >> 20) & 0x3F);
    uint32_t bits = (psm == 0x1B || psm == 0x24 || psm == 0x2C) ? 32u : (uint32_t)Gs_PsmBits(psm);
    uint32_t sig = tex_sig_t(t0);
    if (bits <= 8) {
        uint32_t cp = (uint32_t)((t0 >> 37) & 0x3FFF) / 32;
        sig = (sig ^ gGsPageSig[cp & 511]) * 16777619u;
        sig = (sig ^ gGsPageSig[(cp + 1) & 511]) * 16777619u;
    }
    return sig;
}
/* Textures in the chip's swizzled order (Morton: a pixel's row bits in the even places of its number, its column
   bits in the odd ones; a rectangle is squares one after the other): neighbouring pixels in any direction lie close
   together, so a texture read smaller than it is, or at a slant (the stage), hits the chip's texture cache. The
   pixels are put in that order here and handed to vitaGL as they are; the texture is then marked swizzled. */
int gVitaBisect; /* the fight in three parts, the newest changes off part by part (0 all on, 1 two off, 2 all off) */
static uint32_t sSwzX[512], sSwzY[512];
static uint32_t sSwz[512 * 512];
unsigned gVitaSwz;   /* for the log: textures made swizzled */
static void swizzle32(uint32_t *dst, const uint32_t *src, uint32_t w, uint32_t h) {
    uint32_t m = w < h ? w : h, lm = (uint32_t)__builtin_ctz(m), mask = m - 1, x, y;
    if (sSwzY[1] == 0) {
        uint32_t v, b;
        for (v = 0; v < 512; v++) {
            uint32_t e = 0;
            for (b = 0; b < 9; b++) {
                e |= ((v >> b) & 1u) << (2 * b);
            }
            sSwzY[v] = e;      /* the row's bits: the even places */
            sSwzX[v] = e << 1; /* the column's: the odd ones */
        }
    }
    for (y = 0; y < h; y++) {
        const uint32_t *srow = src + y * w;
        uint32_t rowoff = sSwzY[y & mask] + (h > w ? (y >> lm) << (2 * lm) : 0);
        if (w > h) {
            for (x = 0; x < w; x++) {
                dst[rowoff + ((x >> lm) << (2 * lm)) + sSwzX[x & mask]] = srow[x];
            }
        } else {
            for (x = 0; x < w; x++) {
                dst[rowoff + sSwzX[x]] = srow[x];
            }
        }
    }
}
/* ---- Textures with a palette (the GS's 8- and 4-bit formats) as the chip's own: the indices once (one byte a
   pixel), the palette apart (1 KB), the texture the two together. The game changes a fighter's palette alone
   from picture to picture (a fade: a test made 300 whole textures in two seconds for it, 600 ms); a new
   palette is now 256 entries, not a texture of 65,536 pixels read from GS memory and converted. ---- */
extern void *gpu_alloc_mapped_aligned_for_gpu(size_t alignment, size_t size);
extern void vglSetupTexture(GLenum target, void *data, SceGxmTextureFormat format, SceGxmTextureType type, GLsizei width, GLsizei height, GLsizei mip_count);
extern void vglOverloadTexDataPointer(GLenum target, void *data);
extern void vglLazyFree(void *addr);
#define IDX_MAX 512
#define PAL_MAX 1024
static struct { uint64_t key; uint32_t gen; int refs; uint8_t *data; } sIdxT[IDX_MAX];
static struct { uint64_t key, texa; uint32_t gen; int refs; uint32_t *data; } sPalT[PAL_MAX];
static int sPalOn = -1;
unsigned gVitaPal[3]; /* for the log: index textures made, palettes made, textures made of the two */
unsigned gVitaPaints; /* for the log (gs_core.c): sprites the game drew into its palettes' pages */
static int idx_get(uint64_t key, uint32_t gen, uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t n, uint32_t *px) {
    int k, freeK = -1;
    uint32_t w8 = (tw + 7) & ~7u, x, y;
    uint8_t *d;
    for (k = 0; k < IDX_MAX; k++) {
        if (sIdxT[k].data != NULL && sIdxT[k].key == key && sIdxT[k].gen == gen) {
            return k;
        }
        if (sIdxT[k].data == NULL && freeK < 0) {
            freeK = k;
        }
    }
    if (freeK < 0 || (d = (uint8_t *)gpu_alloc_mapped_aligned_for_gpu(16, w8 * th)) == NULL) {
        return -1;
    }
    {   /* The indices put together in memory the processor caches, then copied to the chip's memory in one go (as
           bytes straight from GS memory where the format allows: no 32-bit pass in between) */
        static uint8_t tmp[512 * 512];
        unsigned long long tr = sceKernelGetProcessTimeWide();
        int direct = Gs_VramReadIdx8(tbp, tbw, psm, tw, th, tmp, w8);
        if (!direct) {
            Gs_VramReadRect(tbp, tbw, psm, tw, th, px);
        }
        gVitaTexReadUs += sceKernelGetProcessTimeWide() - tr;
        gVitaTexPx += tw * th;
        for (y = 0; y < th && !direct; y++) {
            const uint32_t *srow = px + y * tw;
            uint8_t *o = tmp + y * w8;
            for (x = 0; x < tw; x++) {
                o[x] = (uint8_t)(srow[x] & (n - 1));
            }
            for (; x < w8; x++) {
                o[x] = 0;
            }
        }
        if (direct && w8 != tw) {
            for (y = 0; y < th; y++) {
                memset(tmp + y * w8 + tw, 0, w8 - tw);
            }
        }
        memcpy(d, tmp, (size_t)w8 * th);
    }
    sIdxT[freeK].key = key;
    sIdxT[freeK].gen = gen;
    sIdxT[freeK].refs = 0;
    sIdxT[freeK].data = d;
    gVitaPal[0]++;
    return freeK;
}
static int pal_get(uint64_t key, uint64_t texa, uint32_t gen, uint32_t cbp, uint32_t cpsm, uint32_t tcc, uint32_t bits) {
    int k, freeK = -1;
    uint32_t i, n = bits == 8 ? 256 : 16, *d;
    for (k = 0; k < PAL_MAX; k++) {
        if (sPalT[k].data != NULL && sPalT[k].key == key && sPalT[k].texa == texa && sPalT[k].gen == gen) {
            return k;
        }
        if (sPalT[k].data == NULL && freeK < 0) {
            freeK = k;
        }
    }
    if (freeK < 0 || (d = (uint32_t *)gpu_alloc_mapped_aligned_for_gpu(64, 256 * 4)) == NULL) {
        return -1;
    }
    {
        uint32_t tmp[256]; /* (put together here, then copied to the chip's memory in one go: idx_get) */
        uint32_t raw[256]; /* the CLUT's rectangle (16 x 16 or 8 x 2) read block by block, not entry by entry */
        Gs_VramReadRect(cbp, 1, cpsm, bits == 8 ? 16 : 8, bits == 8 ? 16 : 2, raw);
        for (i = 0; i < 256; i++) {
            uint32_t c = 0, a;
            if (i < n) {
                uint32_t q = bits == 8 ? (i & 0xE7) | ((i & 8) << 1) | ((i & 0x10) >> 1) : i;
                c = Gs_Expand(raw[q], cpsm); /* (16 wide: entry q at row q / 16, column q % 16; 8 wide: q / 8, q % 8) */
                a = sProg != 0 ? (tcc ? c >> 24 : 0x80) : (tcc ? (c >> 24) * 2 : 255);
                c = (c & 0x00FFFFFFu) | (a > 255 ? 255u : a) << 24;
            }
            tmp[i] = c;
        }
        memcpy(d, tmp, sizeof(tmp));
    }
    sPalT[freeK].key = key;
    sPalT[freeK].texa = texa;
    sPalT[freeK].gen = gen;
    sPalT[freeK].refs = 0;
    sPalT[freeK].data = d;
    gVitaPal[1]++;
    return freeK;
}
/* a texture of the table let go: its own GL texture, and its indices and palette when nothing else uses them */
static void tex_free(Tex *t) {
    rt_sync();
    if (t->pi >= 0) {
        glBindTexture(GL_TEXTURE_2D, t->id);
        vglOverloadTexDataPointer(GL_TEXTURE_2D, NULL); /* (the indices are shared: not vitaGL's to free) */
        glDeleteTextures(1, &t->id);
        sGlc.tex = 0;
        if (--sIdxT[t->pi].refs <= 0) {
            vglLazyFree(sIdxT[t->pi].data);
            sIdxT[t->pi].data = NULL;
        }
        if (t->pp >= 0 && --sPalT[t->pp].refs <= 0) {
            vglLazyFree(sPalT[t->pp].data);
            sPalT[t->pp].data = NULL;
        }
    } else {
        glDeleteTextures(1, &t->id);
    }
}
/* for the log: the textures made most often in a stretch (place, format, size, palette's place) */
static struct { uint64_t key; unsigned n, tw, th; } sTexTop[32];
static void tex_top_add(uint64_t t0, uint32_t tw, uint32_t th) {
    uint64_t key = (t0 & 0x3FFFFFFFFull) | ((t0 >> 37) & 0x3FFF) << 34;
    int k, low = 0;
    for (k = 0; k < 32; k++) {
        if (sTexTop[k].key == key && sTexTop[k].n != 0) {
            sTexTop[k].n++;
            return;
        }
        if (sTexTop[k].n < sTexTop[low].n) {
            low = k;
        }
    }
    sTexTop[low].key = key;
    sTexTop[low].n = 1;
    sTexTop[low].tw = tw;
    sTexTop[low].th = th;
}
void Vita_TexTopLog(void) {
    char line[360];
    int len = 0, j;
    for (j = 0; j < 6; j++) {
        int k, best = -1;
        for (k = 0; k < 32; k++) {
            if (sTexTop[k].n > 1 && (best < 0 || sTexTop[k].n > sTexTop[best].n)) {
                best = k;
            }
        }
        if (best < 0) {
            break;
        }
        len += snprintf(line + len, sizeof(line) - (size_t)len, " %ux at %04x psm %02x %ux%u palette %04x;", sTexTop[best].n, (unsigned)(sTexTop[best].key & 0x3FFF),
                        (unsigned)((sTexTop[best].key >> 20) & 0x3F), sTexTop[best].tw, sTexTop[best].th, (unsigned)((sTexTop[best].key >> 34) & 0x3FFF));
        sTexTop[best].n = 0;
    }
    if (len > 0) {
        Vita_Log("vita:   textures made again and again:%s\n", line);
    }
    memset(sTexTop, 0, sizeof(sTexTop));
}
/* a texture whose image (place, size, format) was drawn in the last pictures with other contents or
   another palette is a layer of the scene, not a new effect: the light on the ground under Namek's lake is a
   picture whose palette the game turns every picture (a new texture each time). Kept waiting by the budget it was
   left out of busy pictures (the intro's most): the ground under the water flickered. Such a texture is made
   whatever the budget; only effects new to the picture wait. */
unsigned gT126Layer; /* for the log: textures made over the budget as layers */
static int tex_layer(uint64_t t0, uint64_t texa) {
    static uint64_t memoK; static unsigned memoFrame; static int memoR;
    uint64_t img = t0 & 0x3FFFFFFFFull, key = img ^ (texa * 0x9E3779B97F4A7C15ull);
    int k, r = 0;
    if (memoFrame == gGsFrame && memoK == key) {
        return memoR;
    }
    for (k = 0; k < sTexCount; k++) {
        if ((sTex[k].tex0 & 0x3FFFFFFFFull) == img && sTex[k].texa == texa && sTex[k].last + 3 >= gGsFrame) {
            r = 1;
            break;
        }
    }
    memoK = key;
    memoFrame = gGsFrame;
    memoR = r;
    gT126Layer += r;
    return r;
}
static GLuint texture_get_k(uint64_t t0raw, uint64_t texa);
static GLuint texture_get(int ctx) {
    return texture_get_k(gGs.tex0[ctx], gGs.texa);
}
/* the texture of TEX0 `t0raw` and TEXA `texa` as GS memory holds it now */
unsigned gVitaClutSub; /* for the log: textures made with the palette of the GS's last CLUT load (their CLD 0, CBP 0) */
unsigned gVitaKeyPart[3]; /* texture keys whose texture's number was kept (its pages as they were), checked, found otherwise */
static GLuint texture_get_k(uint64_t t0raw, uint64_t texa) {
    static uint32_t px[1024 * 1024 / 4]; /* up to 512 x 512; larger textures are cut */
    uint64_t t0 = t0raw & 0x1FFFFFFFFFFFFFFFull;
    {   /* CLD 0 and CBP 0: the palette is the GS's CLUT buffer, as last loaded (gs_core.c) */
        extern volatile uint64_t gGsClutT0;
        uint64_t ct = gGsClutT0;
        if ((t0raw >> 61) == 0 && ((t0 >> 37) & 0x3FFF) == 0 && ct != 0 && Gs_PsmBits((t0 >> 20) & 0x3F) <= 8 && !(gVglSafe2 & 16)) {
            t0 = (t0 & ~0x1FFFFFE000000000ull) | (ct & 0x1FFFFFE000000000ull);
            gVitaClutSub++;
        }
    }
    uint32_t tbp = t0 & 0x3FFF, tbw = (t0 >> 14) & 0x3F, psm = (t0 >> 20) & 0x3F, tw = 1u << ((t0 >> 26) & 15), th = 1u << ((t0 >> 30) & 15);
    uint32_t cbp = (t0 >> 37) & 0x3FFF, cpsm = (t0 >> 51) & 15, tcc = (t0 >> 34) & 1;
    uint32_t bits = (uint32_t)Gs_PsmBits(psm), gen = 0, i, x, y, clut[256];
    /* the keys made last, and the state of GS memory each was made in. Several: a fighter's strips alternate
       between two textures, and with one remembered key every strip made its key again (60 us, 7,000 times a
       picture: most of a drawn fight picture's time) */
    static struct { uint64_t t0, texa; unsigned epoch; uint32_t gen, sig, gi, tsig; int valid; } sKey[256], *ke; /* (256, was 64; gi, tsig) */
    Tex *t;

    if (tw > 512) { tw = 512; }
    if (th > 512) { th = 512; }
    /* `gen` identifies the CONTENT: the ids of the blocks of GS memory the texture and its palette occupy
       (gs_core.c: uploads the game repeats give the same ids without being written again) */
    ke = &sKey[(uint32_t)((t0 ^ t0 >> 29 ^ texa) * 0x9E3779B1u) >> 24];
    if (ke->valid && ke->t0 == t0 && ke->texa == texa && (ke->epoch == gGsEpoch || (gVitaBisect != 2 && ke->sig == tex_sig(t0) && (++sSigN & 3) != 0))) {
        gen = ke->gen;
        ke->epoch = gGsEpoch;
    } else {
        /* (one in four keys the page numbers would keep is made again, to see whether they missed a change) */
        int sigSame = gVitaBisect != 2 && ke->valid && ke->t0 == t0 && ke->texa == texa && ke->epoch != gGsEpoch && ke->sig == tex_sig(t0);
        /* the texture's own number kept while its pages are as they were: only its palette's made again
           (a fighter's palettes go one after another to one place, and each made every key of that place again, the
           texture's blocks and all: 64 blocks of a 128 x 128 picture); one time in eight made all the same, a check */
        uint32_t tsig = tex_sig_t(t0), gi;
        static unsigned tsigN;
        if (gVitaBisect != 2 && ke->valid && ke->t0 == t0 && ke->texa == texa && ke->tsig == tsig && (++tsigN & 7) != 0) {
            gi = ke->gi;
            gVitaKeyPart[0]++;
        } else {
            gi = Gs_RectId(tbp, tbw, psm, 0, 0, tw, th);
            if (gVitaBisect != 2 && ke->valid && ke->t0 == t0 && ke->texa == texa && ke->tsig == tsig) {
                gVitaKeyPart[1]++;
                gVitaKeyPart[2] += gi != ke->gi;
            }
        }
        gen = gi;
        ke->gi = gi;
        ke->tsig = tsig;
        if (bits <= 8) {
            gen = (gen ^ Gs_RectId(cbp, 1, cpsm, 0, 0, bits == 8 ? 16 : 8, bits == 8 ? 16 : 2)) * 16777619u + cbp;
        }
        if (sigSame) {
            gVitaSigCheck[0]++;
            gVitaSigCheck[1] += gen != ke->gen;
        }
        ke->t0 = t0;
        ke->texa = texa;
        ke->epoch = gGsEpoch;
        ke->gen = gen;
        ke->sig = tex_sig(t0);
        ke->valid = 1;
        gVglKeys++;
    }
    sSigGenCur = gen; /* (the signature) */
    if (sLast != NULL && sLast->tex0 == t0 && sLast->texa == texa && sLast->gen == gen) {
        sLast->last = gGsFrame;
        return sLast->id;
    }
    {
        uint32_t h = bucket_of(t0, texa, gen);
        while (sTexBucket[h] != 0) {
            Tex *c = &sTex[sTexBucket[h] - 1];
            if (c->tex0 == t0 && c->texa == texa && c->gen == gen) {
                c->last = gGsFrame;
                sLast = c;
                return c->id;
            }
            h = (h + 1) & (TEX_BUCKETS - 1);
        }
    }
    if (sTexBudgetFrame != gGsFrame) {
        sTexBudgetFrame = gGsFrame;
        sTexBudgetUs = 0;
    }
    if (sTexDeferOk && sTexBudgetUs > 4000 && !(gVglSafe & 536870912) && !tex_layer(t0, texa)) {
        sTexDeferredNow = 1; /* (made in a later picture: above) */
        gT126Deferred++;
        return 0;
    }
    rt_sync(); /* (vitaGL is this thread's again) */
    if (sTexCount == MAX_TEX) { /* full: drop what was not used for a second, or failing that not in this frame */
        unsigned long long tev = sceKernelGetProcessTimeWide();
        unsigned keep = gGsFrame > 30 ? gGsFrame - 30 : 0;
        int pass, n, k;
        for (pass = 0; pass < 2 && sTexCount == MAX_TEX; pass++, keep = gGsFrame) {
            for (n = 0, k = 0; k < sTexCount; k++) {
                if (sTex[k].last >= keep) {
                    sTex[n++] = sTex[k];
                } else {
                    tex_free(&sTex[k]);
                }
            }
            sTexCount = n;
        }
        rebuild_buckets();
        sStCacheGen++; /* (states kept from pictures before refer to the table's places) */
        gT125Us[4] += sceKernelGetProcessTimeWide() - tev;
        if (sTexCount == MAX_TEX) {
            return 0;
        }
    }
    unsigned long long tmake = sceKernelGetProcessTimeWide();
    if (sT125PicFrame != gGsFrame) {
        sT125PicFrame = gGsFrame;
        sT125Pic = 0;
    }
    tex_top_add(t0, tw, th);
    if (sPalOn < 0) {
        sPalOn = !(gVglSafe & 268435456);
        Vita_Log("vgl: textures with a palette %s\n", sPalOn ? "as the chip's own (indices and a palette)" : "converted to colours");
    }
    if (bits <= 8 && sPalOn) {
        uint32_t n = bits == 8 ? 256 : 16;
        unsigned long long ta = sceKernelGetProcessTimeWide(), tb, tc, td, tr0 = gVitaTexReadUs;
        uint32_t gi = Gs_RectId(tbp, tbw, psm, 0, 0, tw, th), gc = Gs_RectId(cbp, 1, cpsm, 0, 0, bits == 8 ? 16 : 8, bits == 8 ? 16 : 2);
        int pi, pp;
        tb = sceKernelGetProcessTimeWide();
        pi = idx_get(t0 & ((1ull << 34) - 1), gi, tbp, tbw, psm, tw, th, n, px);
        tc = sceKernelGetProcessTimeWide();
        pp = pi >= 0 ? pal_get((uint64_t)cbp | (uint64_t)cpsm << 14 | (uint64_t)tcc << 18 | (uint64_t)bits << 19 | (uint64_t)sProg << 27, texa, gc, cbp, cpsm, tcc, bits) : -1;
        td = sceKernelGetProcessTimeWide();
        gT125Us[0] += tb - ta;
        gT125Us[1] += (tc - tb) - (gVitaTexReadUs - tr0);
        gT125Us[2] += td - tc;
        if (pi >= 0 && pp >= 0) {
            uint32_t *cw;
            t = &sTex[sTexCount++];
            t->tex0 = t0;
            t->texa = texa;
            t->gen = gen;
            t->last = gGsFrame;
            t->pi = (int16_t)pi;
            t->pp = (int16_t)pp;
            t->amin = 0; /* (a test's alpha range: no longer looked at, the palette lies in the chip's memory) */
            t->amax = 255;
            sIdxT[pi].refs++;
            sPalT[pp].refs++;
            glGenTextures(1, &t->id);
            sGlc.tex = 0;
            glBindTexture(GL_TEXTURE_2D, t->id);
            vglSetupTexture(GL_TEXTURE_2D, sIdxT[pi].data, SCE_GXM_TEXTURE_FORMAT_P8_ABGR, SCE_GXM_TEXTURE_LINEAR, (GLsizei)tw, (GLsizei)th, 1);
            cw = (uint32_t *)(void *)vglGetGxmTexture(GL_TEXTURE_2D);
            if (cw != NULL) {
                cw[3] = (cw[3] & 0xFC000000u) | ((uint32_t)(uintptr_t)sPalT[pp].data >> 6);
            }
            {
                uint32_t h = bucket_of(t0, texa, gen);
                while (sTexBucket[h] != 0) {
                    h = (h + 1) & (TEX_BUCKETS - 1);
                }
                sTexBucket[h] = (uint16_t)sTexCount;
            }
            sLast = t;
            gVglNewTex++;
            sSigTexMade++;
            gVitaPal[2]++;
            {
                unsigned long long te = sceKernelGetProcessTimeWide();
                gT125Us[3] += te - td;
                gVitaTexMakeUs += te - tmake;
                sTexBudgetUs += te - tmake;
                sT125Pic += te - tmake;
                if (sT125Pic > gT125Us[5]) {
                    gT125Us[5] = sT125Pic;
                }
            }
            return t->id;
        }
    }
    if (bits <= 8) {
        uint32_t n = bits == 8 ? 256 : 16;
        for (i = 0; i < n; i++) {
            uint32_t k = bits == 8 ? (i & 0xE7) | ((i & 8) << 1) | ((i & 0x10) >> 1) : i;
            clut[i] = bits == 8 ? Gs_Expand(Gs_VramRead(cbp, 1, cpsm, k & 15, k >> 4), cpsm) : Gs_Expand(Gs_VramRead(cbp, 1, cpsm, k & 7, k >> 3), cpsm);
        }
    }
    {
        unsigned long long tr = sceKernelGetProcessTimeWide();
        Gs_VramReadRect(tbp, tbw, psm, tw, th, px); /* block by block, then converted in place */
        gVitaTexReadUs += sceKernelGetProcessTimeWide() - tr;
        gVitaTexPx += tw * th;
    }
    if (bits <= 8) { /* the palette with its alpha rule applied once, not per pixel */
        uint32_t n = bits == 8 ? 256 : 16;
        for (i = 0; i < n; i++) {
            uint32_t c = clut[i], a = sProg != 0 ? (tcc ? c >> 24 : 0x80) : (tcc ? (c >> 24) * 2 : 255);
            clut[i] = (c & 0x00FFFFFFu) | (a > 255 ? 255u : a) << 24;
        }
        for (i = 0; i < tw * th; i++) { px[i] = clut[px[i] & (n - 1)]; }
    } else {
        int p32 = (psm & 0x0F) == 0;
        for (i = 0; i < tw * th; i++) {
            uint32_t c = p32 ? px[i] : Gs_Expand(px[i], psm), a;
            if (sProg != 0) {
                a = tcc ? c >> 24 : 0x80; /* as stored: the shader scales it (0x80 = 1) */
            } else {
                a = tcc ? (c >> 24) * 2 : 255; /* TCC 0: the texture has no alpha of its own */
            }
            px[i] = (c & 0x00FFFFFFu) | (a > 255 ? 255u : a) << 24;
        }
    }
    t = &sTex[sTexCount++];
    t->tex0 = t0;
    t->texa = texa;
    t->gen = gen;
    t->last = gGsFrame;
    t->pi = t->pp = -1;
    t->amin = 0; /* (a test's alpha range: no longer worked out) */
    t->amax = 255;
    {
        static unsigned made;
        if (made < 24 || made % 100 == 0) { /* (a line in the log costs more than the texture) */
            Vita_Log("vgl: frame %u: texture %u: %u x %u, format %02x, at %04x; %d cached; free memory: %u KB video, %u KB main\n", gGsFrame, made, tw, th, psm, tbp,
                     sTexCount, (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 10), (unsigned)(vglMemFree(VGL_MEM_RAM) >> 10));
        }
        made++;
    }
    glGenTextures(1, &t->id);
    sGlc.tex = 0; /* (the binding changes here) */
    glBindTexture(GL_TEXTURE_2D, t->id);
    { unsigned long long w0 = sceKernelGetProcessTimeWide();
    if ((gVglSafe & 33554432) && tw >= 16 && tw == th) { /* (on request only: a test still showed fighters wrong in part of the fight) */
        /* (square only, and the chip's power-of-two swizzled kind, which repeats: a test made every texture of 16 x
           16 or more "swizzled, arbitrary size", and parts of the fighters went missing) */
        swizzle32(sSwz, px, tw, th);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)tw, (GLsizei)th, 0, GL_RGBA, GL_UNSIGNED_BYTE, sSwz);
        {
            SceGxmTexture *gt = vglGetGxmTexture(GL_TEXTURE_2D);
            uint32_t *cw = (uint32_t *)(void *)gt;
            SceGxmTexture tmp;
            if (cw != NULL && (cw[1] & 0xE0000000u) == 0x60000000u &&
                sceGxmTextureInitSwizzled(&tmp, (const void *)(uintptr_t)(cw[2] & 0xFFFFFFFCu), sceGxmTextureGetFormat(gt), tw, th, 0) == 0) {
                cw[1] = ((const uint32_t *)(const void *)&tmp)[1]; /* (its kind and size; the filters and the data stay) */
                gVitaSwz++;
            } else { /* (not as expected: the pixels in rows after all) */
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)tw, (GLsizei)th, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            }
        }
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)tw, (GLsizei)th, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
      gVitaWall[3] += sceKernelGetProcessTimeWide() - w0; }
    {
        uint32_t h = bucket_of(t0, texa, gen);
        while (sTexBucket[h] != 0) {
            h = (h + 1) & (TEX_BUCKETS - 1);
        }
        sTexBucket[h] = (uint16_t)sTexCount;
    }
    sLast = t;
    gVglNewTex++;
    sSigTexMade++;
    gVitaTexMakeUs += sceKernelGetProcessTimeWide() - tmake;
    sTexBudgetUs += sceKernelGetProcessTimeWide() - tmake;
    if (sLogMore) { /* the frames in which the first textures are drawn, batch by batch too */
        sLogMore = 0;
        sLogFrames = 4;
    }
    /* (the binding changed; flush() binds its batch's texture every time, and the batch still waiting keeps its own) */
    return t->id;
}

/* ---- batches ---- */
/* wall-clock microseconds the drawing thread spends in calls that can wait (for the log): swap, buffer maps, draw submission, texture creation */
unsigned long long gVitaWall[6];
unsigned long long gVitaWallK[4][2]; /* draw submissions by kind: stage, fighters, other models, GS primitives: microseconds, count */
static inline void *wt_map(GLenum t, GLenum a) { unsigned long long t0 = sceKernelGetProcessTimeWide(); void *r = glMapBuffer(t, a); gVitaWall[1] += sceKernelGetProcessTimeWide() - t0; return r; }
static volatile int gVitaDrawWhere;
/* (the watchdog: vitaGL's free memory as last looked at, once a second, by the drawing thread: the watchdog itself
   calls nothing of vitaGL, which may be what is stuck) */
static volatile unsigned sMemVramK, sMemRamK, sMemSeen;
static inline void wt_swap(void) {
    unsigned long long t0 = sceKernelGetProcessTimeWide();
    rt_sync();
    gVitaDrawWhere = 3;
    pace_swap(0);
    vglSwapBuffers(GL_FALSE);
    pace_swap(1);
    gVitaDrawWhere = 0;
    gVitaWall[0] += sceKernelGetProcessTimeWide() - t0;
    if (++sMemSeen % 60 == 0) {
        sMemVramK = (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 10);
        sMemRamK = (unsigned)(vglMemFree(VGL_MEM_RAM) >> 10);
    }
}
/* What one draw of a batch reads (flush_impl): its state and its data. */
typedef struct FlushIn {
    State s;
    int kind;                 /* 0 vertices of its own (GS primitives; strips worked out here), 1 the stage's, 2 the fighters' */
    int vertCount, idxCount, stageIcount, ftSlotsUsed, prof;
    const V *verts;           /* kind 0 */
    const uint16_t *idx;      /* kinds 0 (or NULL) and 2 */
    const float *stageM;      /* kind 1: 24 floats */
    const uint32_t *stageIx;  /* kind 1 */
    const float *ft, *ftSlots; /* kind 2: 80 floats; 48 per place taken */
    unsigned frame;           /* (the list it belongs to: the draw log and bisection, below) */
} FlushIn;
/* ---- A list's draws one by one (test runs): bt3.txt drawlog=a-b puts each draw of lists a..b into the log (its
   path, state and size), and bislist=n bisfrom=i bisto=j biskind=m leaves out the draws i..j-1 of list n whose
   kind is in m (1 GS primitives, 2 the stage's strips, 4 the fighters' strips, 8 the stage's kept parts, 16 the
   fighters' kept parts): which draw makes a wrong picture. ---- */
int gVitaDrawLogVerts = 1 << 30, gVitaDrawLogFrom = -1, gVitaDrawLogTo = -1, gVitaBisList = -1, gVitaBisFrom, gVitaBisTo, gVitaBisKind = 31, gVitaBisInv;
static unsigned sBisFrame = ~0u;
static int sBisN;
static int bis_draw(unsigned frame, int cat, const State *s, int count, int extra, const float *bb) {
    int idx, skip;
    if (frame != sBisFrame) {
        sBisFrame = frame;
        sBisN = 0;
    }
    idx = sBisN++;
    skip = (int)frame == gVitaBisList && (cat & gVitaBisKind) && (idx >= gVitaBisFrom && idx < gVitaBisTo) != gVitaBisInv;
    if (gVitaBisInv && idx == 0) {
        skip = 0; /* (the list's clear, always) */
    }
    if ((int)frame >= gVitaDrawLogFrom && (int)frame <= gVitaDrawLogTo) {
        char b[80];
        b[0] = 0;
        if (bb != NULL) {
            snprintf(b, sizeof(b), " box %.3f %.3f %.3f %.3f", bb[0], bb[1], bb[2], bb[3]);
        }
        Vita_Log("dl %u %d k%d t%d n%d x%d tex%u e%u b%u(%u,%u,%u) a%u(%u,%02x) z%u(%u,%u) m%x d%u f%u sc%d,%d,%d,%d%s%s\n", frame, idx, cat, (int)s->target, count, extra,
                 (unsigned)s->tex, s->env, s->blend, s->src, s->dst, s->eq, s->atest, s->afunc, s->aref, s->ztest, s->zfunc, s->zwrite, s->mask,
                 s->date, s->fba, s->sx, s->sy, s->sw, s->sh, b, skip ? " SKIP" : "");
    }
    return skip;
}
static void flush_impl(const FlushIn *in);
static void flush_run(const FlushIn *in);
static void rt_put_draw(const FlushIn *in);
static int sRtOn;
/* ---- Additive groups. An effect is often many big blended triangles in two or three states taking
   turns (an aura: its two textures, one or two triangles each, a hundred times a picture): a draw each, and the
   chip loads the new state on every tile each triangle covers (pairing 0, 90 to 120 draws a picture, the
   chip's time from 12 to 35 ms with 20 blended screens). Primitives that only ADD to the picture (Cd + Cs x factor,
   no depth written, no stencil, the alpha not written or never read again) give the same picture in any order (the
   sum, clamped at the end as at each step), so while they come one after another they go into a batch per state
   and are drawn a batch at a time when anything else comes. ---- */
#define AG_SLOTS 8
#define AG_VERTS 4096
static V sAgV[AG_SLOTS][AG_VERTS];
static State sAgSt[AG_SLOTS];
static int sAgCnt[AG_SLOTS], sAgN, sAgCur = -1;
unsigned gVitaAg[3]; /* for the log: groups drawn, their draws, state changes saved */
static void flush_one(void);
static void ag_flush(int mine) { /* mine: the batch being filled is the group's (else the caller's is kept as it is) */
    State keep = sCur;
    V *kv = sVerts;
    int kc = sVertCount, kic = sIdxCount, kh = sHaveState, kfu = sFtSlotsUsed, k, draws = 0;
    const uint16_t *ki = sIdx;
    const struct StageBin *ks = sStageDraw;
    const float *kf = sFtDraw, *kfs = sFtSlots;
    if (mine) {
        sAgCnt[sAgCur] = sVertCount;
    }
    sIdx = NULL;
    sStageDraw = NULL;
    sFtDraw = NULL;
    for (k = 0; k < sAgN; k++) {
        if (sAgCnt[k] > 0) {
            sCur = sAgSt[k];
            sVerts = sAgV[k];
            sVertCount = sAgCnt[k];
            flush_one();
            draws++;
        }
    }
    gVitaAg[0]++;
    gVitaAg[1] += (unsigned)draws;
    sAgN = 0;
    sAgCur = -1;
    if (mine) {
        sVerts = sVertsMain;
        sVertCount = 0;
        sCur = keep;
        sHaveState = 0;
    } else {
        sCur = keep; sVerts = kv; sVertCount = kc; sIdx = ki; sIdxCount = kic; sHaveState = kh;
        sStageDraw = ks; sFtDraw = kf; sFtSlots = kfs; sFtSlotsUsed = kfu;
    }
}
static inline void ag_close(void) {
    if (sAgN > 0) {
        ag_flush(sAgCur >= 0 && sVerts == sAgV[sAgCur]);
    }
}
__attribute__((hot)) static void flush(void) {
    if (sAgN > 0) {
        int mine = sAgCur >= 0 && sVerts == sAgV[sAgCur];
        ag_flush(mine);
        if (mine) {
            return;
        }
    }
    flush_one();
}
__attribute__((hot)) static void flush_one(void) {
    FlushIn in;
    if (sVertCount == 0) {
        return;
    }
    in.s = sCur;
    in.kind = sStageDraw != NULL ? 1 : sFtDraw != NULL ? 2 : 0;
    in.vertCount = sVertCount;
    in.verts = sVerts;
    in.idx = in.kind == 1 ? NULL : sIdx;
    in.idxCount = sIdxCount;
    in.stageM = sStageDraw != NULL ? sStageDraw->m : NULL;
    in.stageIx = sStageDraw != NULL ? sStageDraw->ix : NULL;
    in.stageIcount = sStageDraw != NULL ? sStageDraw->icount : 0;
    in.ft = sFtDraw;
    in.ftSlots = sFtSlots;
    in.ftSlotsUsed = sFtSlotsUsed;
    in.prof = sProf;
    in.frame = gGsFrame;
    sVertCount = 0;
    if (sRtOn) {
        rt_put_draw(&in);
        return;
    }
    flush_run(&in);
}
/* a draw made (by the drawing thread, or the render thread) */
static void flush_run(const FlushIn *in) {
    {   /* (one reading of the clock before and one after, were two each) */
        unsigned long long w0 = sceKernelGetProcessTimeWide();
        if (sSplitAt != 0 && w0 >= sSplitAt) { /* the picture so far to the chip */
            if (vglSplitScene()) {
                sSplitPics += sSplitN == 0;
                GLC_RESET(); /* (the state as vitaGL has it is kept; the port's copy is set again anyway) */
            }
            sSplitAt = ++sSplitN < SPLIT_MAX ? w0 + SPLIT_MS * 1000ull : 0;
        }
        P_IN(7);
        flush_impl(in);
        P_OUT;
        {
            unsigned long long dw = sceKernelGetProcessTimeWide() - w0;
            int kk = in->kind == 1 ? 0 : in->kind == 2 ? 1 : in->idx != NULL ? 2 : 3;
            gVitaWall[2] += dw;
            gVitaWallK[kk][0] += dw;
            gVitaWallK[kk][1]++;
            gVglUs[2] += (unsigned)dw;
            gVglCalls[2]++;
        }
    }
}

/* ---- The render thread: the batches' draws handed to vitaGL on a thread of their own. The drawing
   thread works out the GS's state and the batches, and then spent ~6 ms of a busy picture in vitaGL's calls
   (zones 36..43); now it puts each draw (its state and a copy of all it reads: vertices, indices, uniforms) in a
   ring, and the render thread makes the calls: on the fourth core when the system lets a thread run there (the
   CapUnlocker plugin), else on the game's core (whose thread waits for the blank ~9 ms a picture). Whatever else
   calls vitaGL (a texture made or let go, the end of the picture, the chip's buffers let go) first waits for the
   ring to be empty (rt_sync), so that vitaGL is never in two threads at once. ---- */
#define RT_RING (4u << 20)
#define RT_WRAP 0
#define RT_DRAW 1
#define RT_DP 2
#define RT_OL 3 /* the outline */
#define RT_IDS 4 /* the ids copied out of the picture's alpha */
#define RT_TBL 5 /* a table pass */
#define RT_GLOW 6 /* the native glow */
#define RT_FOG 7 /* the fog byte from the depth */
#define RT_XF 8 /* the intro's cross-fade */
typedef struct RtHdr { uint32_t size, op, pad_[2]; } RtHdr;
static uint8_t *sRtRing;
static volatile uint32_t sRtHead, sRtTail; /* bytes handed over / done, counting on (the ring's place: & (RT_RING - 1)) */
static uint32_t sRtW, sRtPend;             /* (the drawing thread's) written so far; the packet being written */
static volatile int sRtSleep;              /* the render thread waits on sRtSema */
static SceUID sRtSema = -1;
int gVitaRtCore = -1;                      /* for the log: where the render thread runs (-1: none) */
static volatile int sRtWhere;                /* (the watchdog) where the drawing and render threads are: 1 waiting for the render thread, 2 waiting for room, 3 the swap, 4 a picture for the log; 1 a draw, 2 kept parts, 3 asleep */
volatile unsigned gVitaShown; /* pictures the display has taken (pace_cb): the watchdog sees whether the chip goes on */
static uint32_t sDpIboUsed; /* (below: the kept parts' indices) */
void Vita_VglState(char *b, int n) {
    extern void Gs_QueueState(char *b, int n);
    int k;
    k = snprintf(b, (size_t)n, "vita: WATCHDOG:   drawing thread at %d; render thread %s at %d: handed over %u, done %u (written %u), %s\n", gVitaDrawWhere, sRtOn ? "on" : "off",
                 sRtWhere, sRtHead, sRtTail, sRtW, sRtSleep ? "marked asleep" : "not marked asleep");
    if (k > 0 && k < n) {
        k += snprintf(b + k, (size_t)(n - k), "vita: WATCHDOG:   pictures shown %u, drawn %u; textures %d; free %u KB video, %u KB main (%u pictures ago); kept parts %u KB, fighters %u, stage %u\n",
                      gVitaShown, sMemSeen, sTexCount, sMemVramK, sMemRamK, sMemSeen % 60, (unsigned)(sDpIboUsed >> 10), (unsigned)sFtUsed, (unsigned)sStageUsed);
    }
    if (k > 0 && k < n) {
        Gs_QueueState(b + k, n - k);
    }
}
unsigned long long gVitaRt[8];             /* for the log: packets, bytes; waits for the ring to empty, us; waits for room, us; us busy making them; made by the drawing thread itself */
static inline uint32_t a16(uint32_t n) { return (n + 15u) & ~15u; }
static uint8_t *rt_reserve(uint32_t op, uint32_t bytes) {
    uint32_t total = a16(bytes + (uint32_t)sizeof(RtHdr)), off = sRtW & (RT_RING - 1), pad = off + total > RT_RING ? RT_RING - off : 0;
    RtHdr *h;
    if (sRtW + pad + total - sRtTail > RT_RING) {
        unsigned long long t0 = sceKernelGetProcessTimeWide();
        gVitaDrawWhere = 2;
        while (sRtW + pad + total - sRtTail > RT_RING) {
            __asm__ volatile("yield");
        }
        gVitaDrawWhere = 0;
        gVitaRt[4]++;
        gVitaRt[5] += sceKernelGetProcessTimeWide() - t0;
    }
    __sync_synchronize(); /* (what the render thread read there is done with) */
    if (pad != 0) {
        h = (RtHdr *)(void *)(sRtRing + off);
        h->size = pad;
        h->op = RT_WRAP;
        sRtW += pad;
        off = 0;
    }
    h = (RtHdr *)(void *)(sRtRing + off);
    h->size = total;
    h->op = op;
    sRtPend = total;
    gVitaRt[0]++;
    gVitaRt[1] += total;
    return (uint8_t *)(h + 1);
}
/* the render thread woken when it waits and there is work (a system call: not for every draw, rt_commit) */
static uint32_t sRtUnsig;
static void rt_kick(void) {
    __sync_synchronize();
    if (sRtSleep && sRtHead != sRtTail) {
        sRtSleep = 0;
        sceKernelSignalSema(sRtSema, 1);
    }
    sRtUnsig = 0;
}
static void rt_commit(void) {
    sRtW += sRtPend;
    __sync_synchronize();
    sRtHead = sRtW;
    __sync_synchronize();
    if (sRtSleep && (++sRtUnsig >= 4 || sRtPend >= (64u << 10))) {
        rt_kick();
    }
}
static int rt_exec_one(void);
/* the drawing thread: everything handed over is done (vitaGL is this thread's until it hands over again). When the
   render thread has done nothing for 2 ms, this thread makes the rest itself. */
static void rt_sync(void) {
    if (sRtOn && sRtTail != sRtW) {
        unsigned long long t0 = sceKernelGetProcessTimeWide();
        uint32_t lastTail = sRtTail, spins = 0;
        unsigned long long moved = t0;
        gVitaDrawWhere = 1;
        rt_kick();
        while (sRtTail != sRtW) {
            if ((++spins & 63) == 0) {
                unsigned long long now = sceKernelGetProcessTimeWide();
                if (sRtTail != lastTail) {
                    lastTail = sRtTail;
                    moved = now;
                } else if (now - moved > 2000 && rt_exec_one()) {
                    gVitaRt[7]++;
                    lastTail = sRtTail;
                    continue;
                }
            }
            __asm__ volatile("yield");
        }
        gVitaDrawWhere = 0;
        gVitaRt[2]++;
        gVitaRt[3] += sceKernelGetProcessTimeWide() - t0;
    }
    __sync_synchronize();
}
static void rt_put_draw(const FlushIn *in) {
    uint32_t nv = in->kind == 0 ? (uint32_t)in->vertCount * (uint32_t)sizeof(V) : 0;
    uint32_t ni = in->kind != 1 && in->idx != NULL ? (uint32_t)in->idxCount * 2u : 0;
    uint32_t ns = in->kind == 1 ? 96u + (uint32_t)in->stageIcount * 4u : 0;
    uint32_t nf = in->kind == 2 ? 320u + (uint32_t)in->ftSlotsUsed * 192u : 0;
    uint32_t total = a16((uint32_t)sizeof(FlushIn)) + a16(nv) + a16(ni) + a16(ns) + a16(nf);
    uint8_t *p;
    FlushIn *o;
    if (total > RT_RING / 4) { /* (too large for the ring: drawn here, the ring emptied first) */
        rt_sync();
        flush_run(in);
        return;
    }
    p = rt_reserve(RT_DRAW, total);
    o = (FlushIn *)(void *)p;
    *o = *in;
    p += a16((uint32_t)sizeof(FlushIn));
    if (nv != 0) {
        memcpy(p, in->verts, nv);
        o->verts = (const V *)(const void *)p;
        p += a16(nv);
    }
    if (ni != 0) {
        memcpy(p, in->idx, ni);
        o->idx = (const uint16_t *)(const void *)p;
        p += a16(ni);
    }
    if (ns != 0) {
        memcpy(p, in->stageM, 96);
        o->stageM = (const float *)(const void *)p;
        memcpy(p + 96, in->stageIx, ns - 96);
        o->stageIx = (const uint32_t *)(const void *)(p + 96);
        p += a16(ns);
    }
    if (nf != 0) {
        memcpy(p, in->ft, 320);
        o->ft = (const float *)(const void *)p;
        memcpy(p + 320, in->ftSlots, nf - 320);
        o->ftSlots = (const float *)(const void *)(p + 320);
    }
    rt_commit();
}
static void rt_exec_dp(uint8_t *p);
/* The packet at the tail made by whoever holds sRtLock: the render thread, or the drawing thread when the
   render thread has not moved for 2 ms while it waits for it (on the fourth core the system's shell can stop it for
   0.9 s). 1 when a packet was done. */
static volatile int sRtLock;
static int rt_exec_one(void) {
    uint32_t t;
    const RtHdr *h;
    if (__sync_lock_test_and_set(&sRtLock, 1)) {
        return 0;
    }
    t = sRtTail;
    if (t == sRtHead) {
        __sync_lock_release(&sRtLock);
        return 0;
    }
    __sync_synchronize();
    h = (const RtHdr *)(const void *)(sRtRing + (t & (RT_RING - 1)));
    if (h->op != RT_WRAP) {
        unsigned long long t0 = sceKernelGetProcessTimeWide();
        sRtWhere = (int)h->op;
        if (h->op == RT_DRAW) {
            flush_run((const FlushIn *)(const void *)(h + 1));
        } else if (h->op == RT_DP) {
            rt_exec_dp((uint8_t *)(h + 1));
        } else if (h->op == RT_OL) {
            ol_draw((const uint8_t *)(h + 1));
        } else if (h->op == RT_IDS) {
            ids_exec();
        } else if (h->op == RT_TBL) {
            tbl_exec((const uint8_t *)(h + 1));
        } else if (h->op == RT_GLOW) {
            glow_exec((const uint8_t *)(h + 1));
        } else if (h->op == RT_FOG) {
            fog_exec((const uint8_t *)(h + 1));
        } else if (h->op == RT_XF) {
            xf_exec((const uint8_t *)(h + 1));
        }
        sRtWhere = 0;
        gVitaRt[6] += sceKernelGetProcessTimeWide() - t0;
    }
    __sync_synchronize();
    sRtTail = t + h->size;
    __sync_lock_release(&sRtLock);
    return 1;
}
static int rt_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    for (;;) {
        uint32_t t = sRtTail;
        if (t == sRtHead) {
            if (gVitaRtCore == 3) { /* (a core of its own: a moment's look before sleeping; nothing longer, the system's work is there too) */
                int k;
                for (k = 0; k < 1500 && t == sRtHead; k++) {
                    __asm__ volatile("yield");
                }
                if (t != sRtHead) {
                    continue;
                }
            }
            sRtSleep = 1;
            __sync_synchronize();
            if (t == sRtHead) {
                sRtWhere = 3;
                sceKernelWaitSema(sRtSema, 1, NULL);
                sRtWhere = 0;
            }
            sRtSleep = 0;
            continue;
        }
        if (!rt_exec_one()) {
            sceKernelDelayThread(100); /* (the drawing thread is making them: no spinning here) */
        }
    }
    return 0;
}
static void rt_start(void) {
    extern SceUID Vita_ThreadOnCore3(const char *name, SceKernelThreadEntry entry, int prio, int stack, int fallbackMask);
    extern int gVitaCore3;
    SceUID th;
    if ((gVglSafe2 & 2) || sProg == 0) {
        Vita_Log("vgl: the draws made on the drawing thread itself\n");
        return;
    }
    sRtRing = (uint8_t *)memalign(64, RT_RING);
    sRtSema = sceKernelCreateSema("bt3_rt", 0, 0, 1 << 20, NULL);
    /* (a large stack: a shader variant not made beforehand is compiled on this thread) */
    /* on the game's core, above the game's thread: on the fourth core (earlier versions) it stood still for
       0.9 s whenever the system's shell worked there (the test's screen captures: every 13 s), and the picture with
       it. The game's thread waits for the blank 9-15 ms a picture; this one needs 5-9 ms. Settings 8: the fourth core. */
    /* the fourth core again, at the highest priority an application has (on the game's core, an earlier version, the
       game's thread was late in a heavy moment); when the shell stops it there, the drawing thread takes over (rt_sync).
       Settings 8: the game's core. */
    th = -1;
    if (!(gVglSafe2 & 8) && gVitaCore3 == 1) {
        th = Vita_ThreadOnCore3("bt3_render", rt_thread, 64, 2 << 20, 0x00010000);
    }
    if (th < 0 && sRtRing != NULL && sRtSema >= 0) {
        th = sceKernelCreateThread("bt3_render", rt_thread, 159, 2 << 20, 0, 0x00010000, NULL);
    }
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        Vita_Log("vgl: no render thread (%08x): the draws made on the drawing thread itself\n", (unsigned)th);
        return;
    }
    { extern void Vita_WatchThread(int slot, int id, const char *name); Vita_WatchThread(5, th, "render"); }
    gVitaRtCore = !(gVglSafe2 & 8) && gVitaCore3 == 1 ? 3 : 0;
    __sync_synchronize();
    sRtOn = 1;
    Vita_Log("vgl: the draws made on a render thread of their own, on core %d\n", gVitaRtCore);
}
/* The state that is not the program's: stencil, blend, alpha test, depth, colour mask, scissor (sGlc: only what
   changed). sVarUse must be the variant the program was chosen from (or NULL). */
static int sPremul; /* the batch's pixel shader scaled the colour by its alpha: the source factor is one */
static void apply_fixed(const State *s) {
    static const GLenum kFunc[8] = {GL_NEVER, GL_ALWAYS, GL_LESS, GL_LEQUAL, GL_EQUAL, GL_GEQUAL, GL_GREATER, GL_NOTEQUAL};
    static const GLenum kFactor[6] = {GL_ZERO, GL_ONE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    /* Bit 7 of the alpha in the picture, as the GS's destination-alpha test sees it, is kept in the stencil buffer:
       set by what is drawn with FBA (the HUD bars' masks; the stage's ground shadows, each of which may darken a
       pixel only once), cleared or set by flat untextured fills according to their alpha, and tested by TEST.DATE. */
    {
    /* blending with the colour left out (only the alpha written, which takes the source's as it is):
       none, the same pixels for a lighter pixel program (the alpha clears and alpha passes, several screens a picture) */
    int bOn = s->blend && (s->mask & 7) != 0;
    int kSten = s->date | s->sop << 4, kBlend = bOn ? (1 | s->src << 1 | s->dst << 4 | s->eq << 7 | sPremul << 9) : 0;
    if (s->ztest && s->zwrite && sTgtCur == 0) {
        sMainZ = 1; /* */
    }
    int kDepth = s->ztest | s->zfunc << 1 | s->zwrite << 3, kMask = s->mask;
    int v = sGlc.valid && sProg != 0;
    if (v && sGlc.sten == kSten) {
    } else if (!s->date && !s->sop) {
        glDisable(GL_STENCIL_TEST);
    } else {
        glEnable(GL_STENCIL_TEST);
        glStencilMask(1);
        if (s->date) {
            int t = s->date - 1, w = s->sop ? s->sop - 1 : t;
            glStencilFunc(GL_EQUAL, t, 1);
            glStencilOp(GL_KEEP, GL_KEEP, w == t ? GL_KEEP : w > t ? GL_INCR : GL_DECR);
        } else {
            glStencilFunc(GL_ALWAYS, s->sop == 3 ? 0 : s->sop - 1, 1); /* (3: bit7_mixed's first pass, 0 where drawn) */
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        }
    }
    if (v && sGlc.blend == kBlend) {
    } else if (bOn) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(sPremul ? GL_ONE : kFactor[s->src], kFactor[s->dst], GL_ONE, GL_ZERO);
        glBlendEquationSeparate(s->eq == 1 ? GL_FUNC_REVERSE_SUBTRACT : s->eq == 2 ? GL_FUNC_SUBTRACT : GL_FUNC_ADD, GL_FUNC_ADD);
    } else {
        glDisable(GL_BLEND);
    }
    if (sProg != 0) {
        glDisable(GL_ALPHA_TEST);
    } else if (s->atest) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(kFunc[s->afunc], s->aref >= 128 ? 1.0f : (float)s->aref / 128.0f);
    } else {
        glDisable(GL_ALPHA_TEST);
    }
    if (v && sGlc.depth == kDepth) {
    } else if (s->ztest) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(s->zfunc == 0 ? GL_NEVER : s->zfunc == 1 ? GL_ALWAYS : s->zfunc == 2 ? GL_GEQUAL : GL_GREATER);
        glDepthMask(s->zwrite ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    if (!(v && sGlc.mask == kMask)) {
        glColorMask(s->mask & 1 ? GL_TRUE : GL_FALSE, s->mask & 2 ? GL_TRUE : GL_FALSE, s->mask & 4 ? GL_TRUE : GL_FALSE, s->mask & 8 ? GL_TRUE : GL_FALSE);
    }
    if (sProg != 0 && sVarUse != NULL) {
        sGlc.sten = kSten; sGlc.blend = kBlend; sGlc.depth = kDepth; sGlc.mask = kMask;
        sGlc.valid = 1;
    }
    }
    scissor_of(s);
}
/* A stage part laid on what is drawn already (GEQUAL, no depth written: the red planet's surface layers and its
   clouds): on the console its depth and that of the surface under it come out as the same
   whole number of the 24-bit buffer, or the layer's one higher, and it passes. Here the chip works the depths out
   with their fractions, from another part's triangles: in whole triangles of it the layer came out a hair behind
   and was not drawn (the clouds of the red planet near Frieza's ship gone in patches as the camera moved). The layer
   leads by zlayer depth steps (2: the same as the GS primitives' gVitaZLead; 16 already let the planet's far side
   show at its rim), in all three ways the stage is drawn: kept parts, the stage's batches, vu_transform. */
int gVitaZLayer = 2;
static const float *zl_u(const float *u, uint32_t usize, const State *s, int mode) {
    static float t[128];
    if (gVitaZLayer == 0 || mode == 1 || !s->ztest || s->zwrite || s->zfunc != 2 || usize > sizeof(t)) {
        return u;
    }
    memcpy(t, u, usize);
    t[22] += (float)gVitaZLayer * t[18]; /* (U[5].z, the offset of z, by steps of U[4].z: 2 / the buffer's range) */
    return t;
}
static int sVfLast;           /* the pixel shader variant of the draw being made (flush_impl) */
unsigned gVitaBit7Passes;    /* for the log: second passes for bit 7 of the alpha (bit7_mixed) */
static void flush_impl(const FlushIn *in) {
    const State *s = &in->s;
    const GLuint useProg = in->kind == 1 ? sStageProg : in->kind == 2 ? sFtProg : sProg;
    const GLint uMode = in->kind == 1 ? sStU[6] : in->kind == 2 ? sFtU[1] : sUniMode, uTest = in->kind == 1 ? sStU[7] : in->kind == 2 ? sFtU[2] : sUniTest,
                uTex = in->kind == 1 ? sStU[8] : in->kind == 2 ? sFtU[3] : sUniTex;

    if (in->vertCount == 0) {
        return;
    }
    if (in->prof > 0 && ((in->kind == 1 && (in->prof == 5 || in->prof == 7)) || (in->kind == 2 && (in->prof == 6 || in->prof == 7)))) {
        return; /* (the GPU profile: this part left out) */
    }
    if (VITA_RARE(gVitaBisList >= 0 || gVitaDrawLogTo >= 0)) {
        float bb[4] = {1e9f, 1e9f, -1e9f, -1e9f};
        int k, cnt = in->kind == 1 ? in->stageIcount : in->idx != NULL ? in->idxCount : in->vertCount;
        if (in->kind == 0 && (int)in->frame >= gVitaDrawLogFrom && (int)in->frame <= gVitaDrawLogTo) {
            for (k = 0; k < in->vertCount; k++) { /* (clip space over w) */
                float w = in->verts[k].w != 0.0f ? in->verts[k].w : 1.0f, x = in->verts[k].x / w, y = in->verts[k].y / w;
                if (x < bb[0]) { bb[0] = x; } if (y < bb[1]) { bb[1] = y; }
                if (x > bb[2]) { bb[2] = x; } if (y > bb[3]) { bb[3] = y; }
            }
        }
        if (in->kind == 0 && (int)in->frame >= gVitaDrawLogFrom && (int)in->frame <= gVitaDrawLogTo && in->vertCount >= gVitaDrawLogVerts) {
            /* (its vertices and indices too: drawlogverts=n, the draws of n vertices or more) */
            for (k = 0; k < in->vertCount; k++) {
                const V *q = &in->verts[k];
                Vita_Log("dv %u %d %d %.6g %.6g %.6g %.6g %.5g %.5g %.5g %u %u %u %u\n", in->frame, in->frame == sBisFrame ? sBisN : 0, k, q->x, q->y, q->z, q->w, q->s, q->t, q->q,
                         q->c[0], q->c[1], q->c[2], q->c[3]);
            }
            for (k = 0; in->idx != NULL && k < in->idxCount; k += 24) {
                char b[200];
                int j, len = 0;
                for (j = k; j < in->idxCount && j < k + 24; j++) {
                    len += snprintf(b + len, sizeof(b) - (size_t)len, " %u", in->idx[j]);
                }
                Vita_Log("di %u %d%s\n", in->frame, in->frame == sBisFrame ? sBisN : 0, b);
            }
        }
        if (bis_draw(in->frame, in->kind == 1 ? 2 : in->kind == 2 ? 4 : 1, s, cnt, in->kind == 2 ? in->ftSlotsUsed : in->vertCount,
                     in->kind == 0 && bb[0] < 1e8f ? bb : NULL)) {
            return;
        }
    }
    target_set(s->target); /* (the picture or the work page) */
    if (sLogFrames > 0) {
        Vita_Log("vgl: batch %u: %d vertices, %s, texture %u env %u, blend %u (%u,%u,%u), alpha test %u (%u,%u), depth %u (%u,%u), mask %x, scissor %d %d %d %d\n",
                 gVglBatches, in->vertCount, s->topo ? "lines" : "triangles", (unsigned)s->tex, s->env, s->blend, s->src, s->dst, s->eq, s->atest, s->afunc, s->aref,
                 s->ztest, s->zfunc, s->zwrite, s->mask, s->sx, s->sy, s->sw, s->sh);
    }
    if (sProg != 0) {
        /* the pixel shader for this batch: a test that every pixel passes is no test */
        int tst = s->atest && s->afunc != 1 && !(s->afunc == 5 && s->aref == 0);
        int gsb = s->blend && in->kind != 1 && in->kind != 2 && in->idx == NULL; /* (a blended GS primitive: the GPU profile's modes 3 and 4) */
        int fm = s->tme && s->tex != 0 && !(in->prof == 3 && gsb) ? (s->env == 1 ? 2 : s->env == 2 ? 3 : 1) : 0;
        int lin = s->linear;
        int vf = fm | tst << 2 | (sNoProj && fm ? 8 : 0) | (s->qlin && fm && in->kind != 1 && in->kind != 2 ? 16 : 0);
        if (s->pad_[0] && fm && in->kind != 1 && in->kind != 2) { /* a picture drawn here as the texture */
            vf |= 128 | (s->pad_[0] == 2 ? 256 : 0);
        } else if (in->kind == 2 && s->mask == 8 && (fm == 1 || fm == 2) && !(vf & 16) && !tst) {
            vf |= 512; /* a fighter drawn into the alpha only (seen through): its id as the copy of the ids reads it */
        } else if (gVglLean && !(vf & 16)) { /* (in the GPU profile's window too, as played) */
            vf |= 32; /* (the lean pixel shaders: the colour read in half precision) */
            if ((vf & 3) == 1 && in->kind != 1 && in->kind != 2 && sVsM != 0 && !((unsigned)gVglSafe & 0x80000000u)) {
                vf |= 64; /* (and the texture's alpha scale carried in the colour: a multiply less) */
            }
        } else if ((vf & 16) && gVglLean && gVitaQlinOpt && in->prof != 2 && in->kind == 0) {
            vf |= 32; /* the colour divided by w in half precision too (w scaled per triangle: sQn) */
        }
        if (in->kind == 0 && s->blend && s->src == 2 && s->dst <= 1 && !(vf & (128 | 256 | 512)) && !(gVglSafe2 & 16384)) {
            vf |= 1024; /* the colour scaled by its own alpha (up to 2) here, the blend's source factor one */
        }
        sPremul = (vf & 1024) != 0;
        sVfLast = vf;
        const Variant *var = (gVglSafe & 32768) ? NULL : variant(in->kind == 1 ? 1 : in->kind == 2 ? 2 : 0, vf);
        if (var == NULL) {
            sPremul = 0;
        }
        if (var != NULL) {
            int newProg = !sGlc.valid || sGlc.prog != var->prog;
            sVarUse = var;
            if (newProg) {
                glUseProgram(var->prog);
                sGlc.prog = var->prog;
            }
            if (fm) {
                int tp = lin | s->wrapS << 1 | s->wrapT << 2;
                if (!sGlc.valid || sGlc.tex != s->tex) {
                    glBindTexture(GL_TEXTURE_2D, s->tex);
                    sGlc.tex = s->tex;
                    sGlc.texp = -1;
                }
                if (sGlc.texp != tp) {
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, lin ? GL_LINEAR : GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, lin ? GL_LINEAR : GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, s->wrapS ? GL_CLAMP_TO_EDGE : GL_REPEAT);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, s->wrapT ? GL_CLAMP_TO_EDGE : GL_REPEAT);
                    sGlc.texp = tp;
                }
                if (newProg && var->tex >= 0) {
                    glUniform1i(var->tex, 0);
                }
            }
            if (tst && var->test >= 0) {
                glUniform4f(var->test, (float)(s->afunc + 1), (float)s->aref / 128.0f, 0.0f, 0.0f);
            }
        } else {
        sVarUse = NULL;
        GLC_RESET();
        glUseProgram(useProg);
        if (s->tme && s->tex != 0) {
            glBindTexture(GL_TEXTURE_2D, s->tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s->linear ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s->linear ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, s->wrapS ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, s->wrapT ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            if (uTex >= 0) {
                glUniform1i(uTex, 0);
            }
            glUniform4f(uMode, s->env == 1 ? 2.0f : s->env == 2 ? 3.0f : 1.0f, 0.0f, 0.0f, 0.0f);
        } else {
            glUniform4f(uMode, 0.0f, 0.0f, 0.0f, 0.0f);
        }
        glUniform4f(uTest, s->atest ? (float)(s->afunc + 1) : 0.0f, (float)s->aref / 128.0f, 0.0f, 0.0f);
        }
    } else if (s->tme && s->tex != 0) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, s->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s->linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s->linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, s->wrapS ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, s->wrapT ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, s->env == 1 ? GL_REPLACE : s->env == 2 ? GL_DECAL : GL_MODULATE);
    } else {
        glDisable(GL_TEXTURE_2D);
    }
    ZM(36); /* (the parts of a batch's draw, zones 36..39) */
    apply_fixed(s);
    sPremul = 0;
    ZM(37);
    if (sProg != 0) {
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        if (in->kind == 1) { /* the stage's strips: vertices already on the chip, one matrix for the batch */
            const float *m = zl_u(in->stageM, 96, s, 0); /* (a layer laid on the stage leads: zl_u) */
            int k;
            for (k = 0; k < 6; k++) {
                glUniform4fv(sVarUse != NULL ? sVarUse->u[k] : sStU[k], 1, &m[k * 4]);
            }
            glBindBuffer(GL_ARRAY_BUFFER, sStageVbo);
            glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 32, (const void *)0);
            glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 32, (const void *)24);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (const void *)16);
            ZM(38);
            glDrawElements(GL_TRIANGLES, in->stageIcount, GL_UNSIGNED_INT, in->stageIx);
            ZM(39);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            gVglBatches++;
            return;
        }
        if (in->kind == 2) { /* the fighters' strips: vertices on the chip, the bones as uniforms */
            glUniform4fv(sVarUse != NULL ? sVarUse->u[0] : sFtU[0], 20, in->ft);
            glUniform4fv(sVarUse != NULL ? sVarUse->u[1] : sFtU[4], 12 * in->ftSlotsUsed, in->ftSlots); /* (only up to the highest place taken) */
            glBindBuffer(GL_ARRAY_BUFFER, sFtVbo);
            glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 40, (const void *)0);
            glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 40, (const void *)16);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 40, (const void *)32);
            {   /* (for the log: the draw call itself, apart from the state set before it) */
                unsigned long long d0 = sceKernelGetProcessTimeWide();
                ZM(38);
                glDrawElements(GL_TRIANGLES, in->idxCount, GL_UNSIGNED_SHORT, in->idx);
                ZM(39);
                gVitaWall[5] += sceKernelGetProcessTimeWide() - d0;
            }
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            gVglBatches++;
            return;
        }
        if (!(gVglSafe & 256)) { /* one copy of the vertices into a buffer of the chip's (else vitaGL copies per attribute) */
            static GLuint vbo;
            if (vbo == 0) {
                glGenBuffers(1, &vbo);
            }
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizei)((size_t)in->vertCount * sizeof(V)), in->verts, GL_STREAM_DRAW);
            glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(V), (const void *)0);
            glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(V), (const void *)28);
            glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(V), (const void *)16);
            ZM(38);
            if (in->idx != NULL) { glDrawElements(GL_TRIANGLES, in->idxCount, GL_UNSIGNED_SHORT, in->idx); } else { glDrawArrays(s->topo ? GL_LINES : GL_TRIANGLES, 0, in->vertCount); }
            if (VITA_RARE(s->sop == 3)) { /* bit 7 of the alpha into the stencil where it is set (bit7_mixed) */
                const Variant *v2 = variant(0, (sVfLast & ~1024) | 4);
                if (v2 != NULL && v2->test >= 0) {
                    glUseProgram(v2->prog);
                    if ((sVfLast & 3) && v2->tex >= 0) {
                        glUniform1i(v2->tex, 0);
                    }
                    glUniform4f(v2->test, 6.0f, 1.0f, 0.0f, 0.0f); /* GEQUAL 0x80 */
                    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glDisable(GL_BLEND);
                    if (s->ztest) {
                        glDepthMask(GL_FALSE);
                        if (s->zwrite && s->zfunc == 3) {
                            glDepthFunc(GL_GEQUAL); /* (the pixels the first pass drew: their depth is the buffer's now) */
                        }
                    }
                    glEnable(GL_STENCIL_TEST);
                    glStencilMask(1);
                    glStencilFunc(GL_ALWAYS, 1, 1);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                    if (in->idx != NULL) { glDrawElements(GL_TRIANGLES, in->idxCount, GL_UNSIGNED_SHORT, in->idx); } else { glDrawArrays(s->topo ? GL_LINES : GL_TRIANGLES, 0, in->vertCount); }
                    GLC_RESET(); /* (the next draw sets all of its state again) */
                    gVitaBit7Passes++;
                }
            }
            ZM(39);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        } else {
            glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(V), &in->verts[0].x);
            glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(V), &in->verts[0].c[0]);
            glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(V), &in->verts[0].s);
            if (in->idx != NULL) { glDrawElements(GL_TRIANGLES, in->idxCount, GL_UNSIGNED_SHORT, in->idx); } else { glDrawArrays(s->topo ? GL_LINES : GL_TRIANGLES, 0, in->vertCount); }
        }
        if (sLogFrames > 0) {
            Vita_Log("vgl: batch %u given to vitaGL\n", gVglBatches);
        }
        gVglBatches++;
        return;
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(4, GL_FLOAT, sizeof(V), &in->verts[0].x);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(V), &in->verts[0].c[0]);
    if (s->tme && s->tex != 0) {
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, sizeof(V), &in->verts[0].s);
    } else {
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glDrawArrays(s->topo ? GL_LINES : GL_TRIANGLES, 0, in->vertCount);
    if (sLogFrames > 0) {
        Vita_Log("vgl: batch %u given to vitaGL\n", gVglBatches);
    }
    gVglBatches++;
}

static int sPutPage; /* the primitive being put goes into the work page: 256 x 256, not turned over (vitaGL's
                        framebuffer objects count from the bottom, so the page's row 0 is the texture's row 0) */
static float sPutW = 256.0f, sPutH = 256.0f; /* the size of the target being put into (a work target's own) */
static void put_target(int t) {
    sPutPage = t;
    if (t >= WT_BASE) {
        sPutW = (float)sWt[t - WT_BASE].w;
        sPutH = (float)sWt[t - WT_BASE].h;
    } else {
        sPutW = sPutH = 256.0f;
    }
}
/* GS primitives lead by this many depth steps towards the eye. What the game transforms itself (or the
   interpreter) and lays onto scenery drawn by a vertex program on the chip (the light on the ground under Namek's
   lake, a waterfall on its rock) lies in front of it by less than a step: on the console both depths come out as the
   same whole number, or the layer's one higher, and GEQUAL passes everywhere. Here the scenery's depth is worked
   out by the chip with its fractions (and its corners where the console's 1/16 pixel steps are not), so it can come
   out above the layer's whole number, at this pixel and not that one, differently every picture as the camera
   moves: the layer blinked in patches. With the lead the layer wins wherever the two are within those steps (the
   PC back end did the same: GS_Z_LEAD). Not past the nearest depth there is. */
int gVitaZLead = 2;
/* the triangle's smallest Q: w = 1 / Q handed for the colour's division (kVertNP) is scaled by it, so that
   it stays at most 1 and the colour times it fits half precision (the division then in the lean pixel shaders; the
   scale is the same at the three corners and cancels out) */
static float sQn = 1.0f;
/* the depth's divisor as a reciprocal, kept for the buffer's range it was made for (a double: the result as
   the division's but in the last of 53 bits, which the float it is rounded to does not keep) */
static double sZmaxFor = -1.0, sZmaxInv;
static inline __attribute__((always_inline)) void put_i(const GsVertex *v, float x, float y, float s, float t, float q, const GsVertex *c, float zmax) {
    V *o = &sVerts[sVertCount++];
    float w = q > 0.0f ? 1.0f / q : 1.0f;
    double zl = (double)v->z + (double)gVitaZLead, zm = (double)zmax;
    float zn;
    if (zm != sZmaxFor) {
        sZmaxFor = zm;
        sZmaxInv = 1.0 / zm;
    }
    zn = (float)((zl < zm ? zl : (double)v->z) * sZmaxInv);
    if (sPutPage) { /* (as a picture of GS_W x GS_H, which the lines below turn into clip space) */
        x = x * (GS_W / sPutW);
        y = GS_H - y * (GS_H / sPutH);
    }
    if (sTexXf) { /* a picture drawn here as the texture: the coordinates into it */
        s = sTexXfV[0] + s * sTexXfV[1];
        t = sTexXfV[2] + t * sTexXfV[3];
    }

    /* The GS has no near or far plane: a depth beyond the buffer's range is drawn (the text printer sends
       0xFFFFFF0 into a 24-bit buffer). The PC back end's pipeline clamps depth; here it is done by hand, or the
       chip clips the sprite away. */
    if (!(zn > 0.0f)) { zn = 0.0f; }
    if (zn > 1.0f) { zn = 1.0f; }
    if (sProg != 0 && sNoProj) { /* S / Q, T / Q interpolated by the chip with w = 1 / Q: the GS's (S / Q per pixel) */
        o->x = (x / (GS_W * 0.5f) - 1.0f) * w;
        o->y = (1.0f - y * (1.0f / (GS_H * 0.5f))) * w;
        o->z = (zn * 2.0f - 1.0f) * w;
        o->w = w;
        o->c[0] = c->r;
        o->c[1] = c->g;
        o->c[2] = c->b;
        o->c[3] = c->a;
        o->s = s;
        o->t = t;
        o->q = sCur.qlin ? w * sQn : 1.0f; /* (kVertNP: the colour across the screen, where Q differs corner to corner) */
        return;
    }
    if (sProg != 0) { /* as the GS: position, depth and colour straight across the screen; S, T, Q divided per pixel */
        o->x = x / (GS_W * 0.5f) - 1.0f;
        o->y = 1.0f - y * (1.0f / (GS_H * 0.5f));
        o->z = zn * 2.0f - 1.0f;
        o->w = 1.0f;
        o->c[0] = c->r;
        o->c[1] = c->g;
        o->c[2] = c->b;
        o->c[3] = c->a;
        o->s = s * q;
        o->t = t * q;
        o->q = q;
        return;
    }

    o->x = (x / (GS_W * 0.5f) - 1.0f) * w;
    o->y = (1.0f - y * (1.0f / (GS_H * 0.5f))) * w;
    o->z = (zn * 2.0f - 1.0f) * w;
    o->w = w;
    o->c[0] = (uint8_t)(c->r > 127 ? 255 : c->r * 2); /* 0x80 is "the texture's own colour" */
    o->c[1] = (uint8_t)(c->g > 127 ? 255 : c->g * 2);
    o->c[2] = (uint8_t)(c->b > 127 ? 255 : c->b * 2);
    o->c[3] = (uint8_t)(c->a > 127 ? 255 : c->a * 2);
    o->s = s * w;
    o->t = t * w;
}
static __attribute__((noinline)) void put(const GsVertex *v, float x, float y, float s, float t, float q, const GsVertex *c, float zmax) {
    put_i(v, x, y, s, t, q, c, zmax); /* (one copy for the callers; GsGpu_Draw_impl had six of it in line) */
}

/* the TEX1 values the states are made with (filters, mip-map levels, LOD): whether the game asks
   for mip-maps or another minifying filter than the magnifying one anywhere (Planet Namek's flicker) */
static struct { uint64_t v, t0; unsigned n; } sTex1[24];
static void tex1_note(uint64_t tex1, uint64_t t0) {
    uint64_t v = tex1 & 0x00000FFF001803FFull; /* LCM, MXL, MMAG, MMIN, MTBA, L, K */
    int k, low = 0;
    for (k = 0; k < 24; k++) {
        if (sTex1[k].n != 0 && sTex1[k].v == v) {
            sTex1[k].n++;
            return;
        }
        if (sTex1[k].n < sTex1[low].n) {
            low = k;
        }
    }
    sTex1[low].v = v;
    sTex1[low].t0 = t0;
    sTex1[low].n = 1;
}
void Vita_Tex1Log(void) {
    extern unsigned gGsMipWrites;
    extern uint64_t gGsMipTbp[4];
    char line[700];
    int len = 0, j;
    for (j = 0; j < 8; j++) {
        int k, best = -1;
        for (k = 0; k < 24; k++) {
            if (sTex1[k].n > 0 && (best < 0 || sTex1[k].n > sTex1[best].n)) {
                best = k;
            }
        }
        if (best < 0) {
            break;
        }
        {
            uint64_t v = sTex1[best].v, t0 = sTex1[best].t0;
            len += snprintf(line + len, sizeof(line) - (size_t)len, " %ux MXL %u MMAG %u MMIN %u LCM %u L %u K %d (psm %02x %ux%u at %04x);", sTex1[best].n,
                            (unsigned)((v >> 2) & 7), (unsigned)((v >> 5) & 1), (unsigned)((v >> 6) & 7), (unsigned)(v & 1), (unsigned)((v >> 19) & 3),
                            (int)((int32_t)((uint32_t)(v >> 32) << 20) >> 20), (unsigned)((t0 >> 20) & 0x3F), 1u << ((t0 >> 26) & 15), 1u << ((t0 >> 30) & 15),
                            (unsigned)(t0 & 0x3FFF));
        }
        sTex1[best].n = 0;
    }
    Vita_Log("vgl:   TEX1 of the states made:%s | MIPTBP writes %u (last %llx %llx)\n", len ? line : " none", gGsMipWrites,
             (unsigned long long)gGsMipTbp[0], (unsigned long long)gGsMipTbp[2]);
    memset(sTex1, 0, sizeof(sTex1));
    gGsMipWrites = 0;
}
/* The state of the primitive about to be drawn, or 0 when it is not drawn by this back end. */
static int state_of_(int ctx, int type, State *d);
static int sRejWhy;          /* (state_of_: why the last primitive is not drawn) */
unsigned gVitaSigCheck[4];   /* for the log: texture keys whose pages were unchanged, of them made otherwise; the same for states */
unsigned gVitaRej[2][8];     /* for the log: GS primitives / fighter strips not drawn, by that reason */
/* The same registers in the same frame, with GS memory as it was: the same state. (A fighter's strips come in
   two alternating states, 5,000 times a picture.) */
unsigned gVitaStOf[6]; /* state_of: 0 the work targets' way, 1 the page's way, 2 found (memory unchanged), 3 found
                          (texture's pages unchanged), 4 worked out (one in four of those, the check), 5 worked out (not found) */
/* 256 states remembered, was 16: Kame House's pictures have some 250 states worked out each, most of them
   pushed out by others in the same picture and made again (a state worked out is 5,000 to 8,000 cycles of the drawing
   thread). Emptied by a new number (sStCacheGen) instead of clearing it. */
#define ST_CACHE 1024 /* (was 256; a test had 2,500 states in 2 s pushed out by another of the same picture) */
static unsigned sStCacheGen = 1;
unsigned gVitaStOfEvict; /* (the log) states worked out whose place held another state of the same picture */
static int state_of_cached(int ctx, int type, State *d, const uint64_t *r, int wpk);
unsigned gVitaStLast;
unsigned gVitaWtWay[5]; /* the work targets' way: a work target as the texture, the picture, page 0, page 0x70; drawn into a target not kept */
int gVitaStLastOn = 1, gVitaFl6 = 1, gVitaStXOn = 1, gVitaWtCOn = 1;
/* the work targets' way's reads remembered (above in state_of) */
static int work_mark(uint32_t fbp);
typedef struct WtC { uint64_t r[11]; unsigned frame, epoch; int ctx, type, wp, kind, ok; WorkTarget *s; float xf[4]; State st; } WtC;
static WtC sWtC[32];
unsigned gVitaWtC[3]; /* (the log) the work targets' way: states remembered taken, worked out, remembered */ /* (tests: bt3.txt stlast=, fl6=) */
__attribute__((hot)) static int state_of(int ctx, int type, State *d) {
    uint64_t r[11];
    int ok, pageT, wpk;

    r[0] = gGs.prim; r[1] = gGs.tex0[ctx]; r[2] = gGs.test[ctx]; r[3] = gGs.clamp[ctx]; r[4] = gGs.scissor[ctx];
    r[5] = gGs.frame[ctx]; r[6] = gGs.zbuf[ctx]; r[7] = gGs.alpha[ctx]; r[8] = gGs.tex1[ctx]; r[9] = gGs.texa; r[10] = gGs.fba[ctx];
    pageT = sWpReady && (r[5] & 0x1FF) == WP_FBP;
    if (pageT) {
        sWpSig = wp_sig(); /* (the page's memory as the uploads left it, at the last draw into it) */
        sWpSigSet = 1;
    }
    wpk = pageT ? 1 + (sWpAllow | gVitaWpOff << 1 | vgl_bt3_split_on << 2 | sWpSlot << 3) : 0; /* (a state into the page is kept, for the same callers; 154: by slot) */
    sTexXf = 0;
    if (sWtOn && vgl_bt3_split_on) { /* work targets */
        uint32_t tb = (uint32_t)(r[1] & 0x3FFF) / 32;
        WorkTarget *ft = wt_find((uint32_t)(r[5] & 0x1FF), (uint32_t)((r[5] >> 16) & 0x3F));
        wpk |= (sWpAllow | FX_WT_OFF << 1) << 12;
        if ((((r[0] >> 4) & 1) && (r[1] & 0x1F) == 0 && (wt_find(tb, 0) != NULL || tb == (uint32_t)gGsMainFbp || tb == 0 || tb == 0x70)) || (ft != NULL && !ok_cacheable_wt(ft))) {
            /* a work target or the picture as a texture: the coordinates and whether it is whole change by the draw */
            gVitaStOf[0]++;
            gVitaWtWay[(((r[0] >> 4) & 1) && (r[1] & 0x1F) == 0 && (wt_find(tb, 0) != NULL || tb == (uint32_t)gGsMainFbp || tb == 0 || tb == 0x70)) ? (wt_find(tb, 0) != NULL ? 0 : tb == (uint32_t)gGsMainFbp ? 1 : tb == 0 ? 2 : 3) : 4]++;
            /* A read of a work target or of the picture, remembered for the picture: the same registers
               give the same state while what state_of_ looked at is as it was (the target read: drawn into, not stale,
               its memory as its last draw left it; the picture: the same copy of it) */
            uint32_t fbp5 = (uint32_t)(r[5] & 0x1FF);
            WtC *e = &sWtC[((uint32_t)(r[1] ^ r[5] ^ r[0] >> 3 ^ r[7] >> 1 ^ r[4]) * 0x9E3779B1u) >> 27];
            if (gVitaWtCOn && e->ok && e->frame == gGsFrame && e->ctx == ctx && e->type == type && e->wp == wpk && memcmp(r, e->r, sizeof(r)) == 0 &&
                (e->kind == 2 ? e->s->drawn && !e->s->stale && e->s->tex == e->st.tex && (e->epoch == gGsEpoch || wt_sig(e->s) == e->s->sig)
                              : sOlTex[gxm_back_buffer_index % 3] == e->st.tex) &&
                /* the shadows' work page shares its page with the glow's first target: read as the target only
                   where state_of_ would not take it for the work page (outside a shadow's markers) */
                ((r[1] & 0x3FFF) != WP_FBP * 32 || !sWpReady || (sWtOn && sWpMarked && sWpAgain < 0 && sWpSlot == WP_OTHER && !FX_WT_OFF))) {
                if (gGsMainFbp >= 0 && fbp5 != (uint32_t)gGsMainFbp) {
                    work_mark(fbp5); /* (as state_of_: the game drew into that buffer) */
                }
                e->epoch = gGsEpoch;
                sTexXf = 1;
                memcpy(sTexXfV, e->xf, sizeof(e->xf));
                gVitaWt[e->kind == 2 ? 2 : 4]++;
                gVitaWtC[0]++;
                *d = e->st;
                return 1;
            }
            sRejWhy = 0;
            sWorkChanged = 0;
            ok = state_of_(ctx, type, d);
            gVitaWtC[1]++;
            if (ok && sTexXf && d->tme && d->pad_[0] != 0 && gGsMainFbp >= 0) {
                WorkTarget *ws = wt_find(tb, (uint32_t)((r[1] >> 14) & 0x3F));
                int kind = ws != NULL && ws->tex == d->tex ? 2 : sOlTex[gxm_back_buffer_index % 3] == d->tex ? 3 : 0;
                if (kind != 0) {
                    memcpy(e->r, r, sizeof(r));
                    e->frame = gGsFrame; e->epoch = gGsEpoch; e->ctx = ctx; e->type = type; e->wp = wpk; e->kind = kind; e->s = ws; e->ok = 1;
                    memcpy(e->xf, sTexXfV, sizeof(e->xf));
                    e->st = *d;
                    gVitaWtC[2]++;
                }
            }
            if (!ok && sWorkChanged) {
                sStCacheGen++;
            }
            if (!ok && ft != NULL) {
                ft->stale = 1; /* (a pass into it missed: what reads it is left out) */
                ft->cov = 0.0f;
                gVitaWt[1]++;
                sWtRej[(ft - sWt) % WT_MAX][sRejWhy & 7]++;
            }
            if (!ok && pageT) {
                wp_spoil();
                gVitaWp[1]++;
            }
            return ok;
        }
    }
    if (sWpReady && ((((r[0] >> 4) & 1) && (r[1] & 0x3FFF) == WP_FBP * 32 && (sWpAgain >= 0 || (sWpSigSet && wp_sig() == sWpSig))))) {
        /* from the work page: what it gives changes as the page is cleared and spoiled */
        gVitaStOf[1]++;
        sRejWhy = 0;
        sWorkChanged = 0;
        ok = state_of_(ctx, type, d);
        if (!ok && sWorkChanged) {
            sStCacheGen++;
        }
        if (!ok && (r[5] & 0x1FF) == WP_FBP) {
            wp_spoil(); /* (anything drawn into the page and not drawn here) */
            gVitaWp[1]++;
        }
        return ok;
    }
    {   /* the state of the call before, when the registers are its registers: before the table (whose 1024
           places lie in 150 KB, a cache miss or two a look) */
        static struct { uint64_t r[11]; unsigned frame, epoch, gen; int ctx, type, wp, ok; State st; } last;
        if (last.ok && gVitaStLastOn && last.gen == sStCacheGen && last.frame == gGsFrame && last.ctx == ctx && last.type == type && last.wp == wpk &&
            (last.epoch == gGsEpoch || !((r[0] >> 4) & 1)) && memcmp(r, last.r, sizeof(r)) == 0) {
            gVitaStOf[2]++;
            gVitaStLast++;
            *d = last.st;
            return 1;
        }
        ok = state_of_cached(ctx, type, d, r, wpk);
        last.ok = ok;
        if (ok) {
            memcpy(last.r, r, sizeof(r));
            last.frame = gGsFrame; last.epoch = gGsEpoch; last.gen = sStCacheGen; last.ctx = ctx; last.type = type; last.wp = wpk;
            last.st = *d;
        }
        return ok;
    }
}
/* A state kept from one picture to the next. The cache held a state for the picture it was made in only, and
   each picture worked out its ~200 states again (5,000 to 8,000 cycles each). A state drawn into the picture (not a
   work buffer, work page or work target) whose texture, if it has one, is one of the texture table's own is kept on:
   its registers the same (FRAME's buffer number as "the picture", which alternates between two buffers), nothing
   changed in the table since (a texture let go: sStCacheGen), and the GS memory pages of its texture and palette as
   they were (tex_sig). One in eight is worked out again as a check (the log). */
unsigned gVitaStX[3]; /* (the log) states kept from a picture before; worked out as a check; of them found otherwise */
static int state_of_cached(int ctx, int type, State *d, const uint64_t *rin, int wpk) {
    static struct { uint64_t r[11]; unsigned frame, epoch; uint32_t sig; int ctx, type, valid, wp, xok, ti; State st; } cache[ST_CACHE], *c;
    uint64_t r[11];
    uint32_t h;
    int ok, pageT = sWpReady && (rin[5] & 0x1FF) == WP_FBP, mainF = gGsMainFbp >= 0 && (uint32_t)(rin[5] & 0x1FF) == (uint32_t)gGsMainFbp;
    memcpy(r, rin, sizeof(r));
    if (mainF) {
        r[5] = (r[5] & ~0x1FFull) | 0x8000u; /* (FRAME's bits 9-15 are not used: "the picture") */
    }
#define ST_SAME(e) ((e)->valid == (int)sStCacheGen && (e)->ctx == ctx && (e)->type == type && (e)->wp == wpk && memcmp(r, (e)->r, sizeof(r)) == 0)
    h = (uint32_t)(r[1] ^ r[1] >> 29) * 0x9E3779B1u ^ (uint32_t)r[0] * 0x85EBCA6Bu ^ (uint32_t)r[7] ^ (uint32_t)r[2];
    c = &cache[((h ^ h >> 15) * 0x2C1B3C6Du) >> 22];
    {   /* two places for a number: the other one of the pair when this one holds another state */
        typeof(c) c2 = &cache[(unsigned)(c - cache) ^ 1u];
        if (ST_SAME(c2)) {
            c = c2;
        } else if (!ST_SAME(c) && c->valid == (int)sStCacheGen && c->frame == gGsFrame && !(c2->valid == (int)sStCacheGen && c2->frame == gGsFrame)) {
            c = c2; /* (this one holds another state of the picture, the other is free) */
        }
    }
    if (ST_SAME(c) && c->frame == gGsFrame &&
        (c->epoch == gGsEpoch || !((r[0] >> 4) & 1) || (gVitaBisect != 2 && c->sig == tex_sig(r[1]) && (++sSigN & 3) != 0))) { /* (no texture: GS memory plays no part) */
        gVitaStOf[c->epoch == gGsEpoch || !((r[0] >> 4) & 1) ? 2 : 3]++;
        c->epoch = gGsEpoch;
        *d = c->st;
        return 1;
    }
    {
        int sigSame = gVitaBisect != 2 && ST_SAME(c) && c->frame == gGsFrame && c->epoch != gGsEpoch && c->sig == tex_sig(r[1]);
        int xSame = !sigSame && gVitaStXOn && c->xok && ST_SAME(c) && c->frame != gGsFrame && (!c->st.tme || (c->ti >= 0 && c->ti < sTexCount && sTex[c->ti].id == c->st.tex)) &&
                    (!((r[0] >> 4) & 1) || c->sig == tex_sig(r[1]));
        static unsigned xN;
        if (xSame && (++xN & 7) != 0) { /* a state of a picture before */
            gVitaStX[0]++;
            if (c->st.tme) {
                sTex[c->ti].last = gGsFrame; /* (in use: the table keeps it) */
            }
            c->frame = gGsFrame;
            c->epoch = gGsEpoch;
            *d = c->st;
            return 1;
        }
        State keep = c->st;
        sWorkChanged = 0;
        sRejWhy = 0;
        gVitaStOf[sigSame ? 4 : 5]++;
        gVitaStOfEvict += c->valid == (int)sStCacheGen && c->frame == gGsFrame && !sigSame;
        ok = state_of_(ctx, type, d);
        if (sigSame) { /* (one in four states the page numbers would keep, made again) */
            gVitaSigCheck[2]++;
            gVitaSigCheck[3] += !ok || memcmp(&keep, d, sizeof(keep)) != 0;
        }
        if (xSame) {
            gVitaStX[1]++;
            gVitaStX[2] += !ok || memcmp(&keep, d, sizeof(keep)) != 0;
        }
    }
    if (!ok && pageT) {
        wp_spoil(); /* (anything drawn into the page and not drawn here) */
        gVitaWp[1]++;
    }
    if (!ok && sWtOn) {
        WorkTarget *ft = wt_find((uint32_t)(rin[5] & 0x1FF), (uint32_t)((rin[5] >> 16) & 0x3F));
        if (ft != NULL) {
            ft->stale = 1; /* (a pass into a work target missed) */
            ft->cov = 0.0f;
            gVitaWt[1]++;
            sWtRej[(ft - sWt) % WT_MAX][sRejWhy & 7]++;
        }
    }
    if (ok) {
        memcpy(c->r, r, sizeof(c->r));
        c->frame = gGsFrame; c->epoch = gGsEpoch; c->ctx = ctx; c->type = type; c->valid = (int)sStCacheGen; c->wp = wpk;
        c->sig = ((r[0] >> 4) & 1) ? tex_sig(r[1]) : 0;
        c->st = *d;
        /* kept for the pictures after: into the picture, its texture (if any) the table's own */
        c->ti = d->tme && sLast != NULL && sLast->id == d->tex ? (int)(sLast - sTex) : -1;
        c->xok = mainF && d->target == 0 && !sTexXf && d->pad_[0] == 0 && (((r[0] >> 4) & 1) ? d->tme && c->ti >= 0 : !d->tme) &&
                 !(((r[0] >> 4) & 1) && ((r[1] >> 37) & 0x3FFF) == 0 && Gs_PsmBits((uint32_t)((r[1] >> 20) & 0x3F)) <= 8); /* (not one with the GS's last loaded palette) */
    } else if (sWorkChanged || gVitaBisect == 2) {
        sStCacheGen++; /* (a draw into another work buffer, or one drawn into anew: changes what may be used as a texture) */
    }
#undef ST_SAME
    return ok;
}
/* a work buffer the game drew into: remembered with its page's upload count (its slot in sWork, 16 when full) */
static int work_mark(uint32_t fbp) {
    int i;
    for (i = 0; i < sWorkCount && sWork[i].fbp != fbp; i++) {
    }
    if (i == sWorkCount && sWorkCount < 16) {
        sWorkCount++;
        sWorkChanged = 1;
        sWorkGen++;
    }
    if (i < 16 && (sWork[i].fbp != fbp || sWork[i].gen != gGsPageGen[fbp & 511])) {
        sWork[i].fbp = fbp;
        sWork[i].gen = gGsPageGen[fbp & 511];
        sWorkChanged = 1;
        sWorkGen++;
    }
    return i;
}
/* The GS's alpha test can let the pixels that fail it write the picture but not the depth (AFAIL FB_ONLY), or the
   depth only, or the colour only. Those draws were drawn as if there were no test: every pixel wrote the depth.
   bt3.txt afail=1: a draw whose failing pixels keep the depth (FB_ONLY, RGB_ONLY) writes no depth; afail=2: its
   failing pixels not drawn (the alpha test as for KEEP). */
int gVitaAFail;
static unsigned sAfSeen[48];
static int sAfSeenN;
static int state_of_(int ctx, int type, State *d) {
    uint64_t prim = gGs.prim, t0 = gGs.tex0[ctx], test = gGs.test[ctx], cl = gGs.clamp[ctx], sc = gGs.scissor[ctx], fr = gGs.frame[ctx];
    uint32_t fpsm = (uint32_t)((fr >> 24) & 0x3F), fbp = (uint32_t)(fr & 0x1FF), zbp = (uint32_t)(gGs.zbuf[ctx] & 0x1FF), fbmsk = (uint32_t)(fr >> 32);
    uint32_t tpsm = (uint32_t)((t0 >> 20) & 0x3F), tbp = (uint32_t)(t0 & 0x3FFF);
    int tme = (prim >> 4) & 1, abe = (prim >> 6) & 1, i, x0, y0, x1, y1;
    int page = 0, pageTex = 0, pageSlot = WP_OTHER; /* drawn into the work page / textured from it (154: its slot) */
    int afx = 0; /* an alpha test that every pixel fails: 1 the picture only, 2 the depth only, 3 the colour only */
    WorkTarget *wtT = NULL, *wtS = NULL; /* drawn into a work target / textured from one */

    if (Gs_PsmBits(fpsm) == 16 || fbp == zbp) {
        if (fbp != zbp && (gGsMainFbp >= 0 ? fbp != (uint32_t)gGsMainFbp : (fbp != 0 && fbp != 0x70))) {
            /* a work buffer drawn through a 16-bit view (the outline's edge image, GfxPost_ClearWork):
               not drawn here, but the game has written it now, so what reads it next means this buffer and not the
               uploads that share its pages (the effects' textures and palettes at page 0x150). As the PC renderer
               (gs_gpu.c). the outline's later passes read those uploads as their mask, and its darkening
               put blocks of black over the top of the picture (Bardock's stage) */
            work_mark(fbp);
        }
        sRejWhy = 1;
        return 0; /* a 16-bit view of a buffer, or a picture drawn into the depth buffer's memory */
    }
    if (gGsMainFbp >= 0 ? fbp != (uint32_t)gGsMainFbp : (fbp != 0 && fbp != 0x70)) {
        i = work_mark(fbp); /* a work buffer: remember that the game drew into it */
        if (sWpAllow && sWpReady && !gVitaWpOff && vgl_bt3_split_on && fbp == WP_FBP && fpsm == 0 && ((fr >> 16) & 0x3F) == 4 &&
            (sWpSlot != WP_OTHER || !sWpMarked)) { /* (with the game's markers, the page only for the shadows) */
            page = 1; /* a GS primitive into the work page: drawn there */
        } else if (sWpAllow && sWtOn && !FX_WT_OFF && vgl_bt3_split_on && fpsm == 0 && (wtT = wt_get(fbp, (uint32_t)((fr >> 16) & 0x3F))) != NULL) {
            page = 2; /* a GS primitive into a work target */
        } else {
            sRejWhy = 2;
            return 0;
        }
    }
    memset(d, 0, sizeof(*d));
    if (gVglSafe & 1) { tme = 0; }
    if (sTblState) { tme = 0; } /* (a table pass, see tbl_intercept) */
    if (gVglSafe & 2) { abe = 0; }
    if (gVglSafe & 4) { test &= ~1ull; }
    if (gVglSafe & 8) { sc = 0x01BF000001FF0000ull; }
    if (tme) {
        if ((tpsm & 0x30) == 0x30 || tbp / 32 == zbp) {
            sRejWhy = 3;
            return 0; /* the depth buffer as a texture */
        }
        /* PSMT8H / PSMT4HL / PSMT4HH: one byte or half a byte of a 32-bit pixel as an index. The game keeps its
           text fonts that way (uploaded data, decoded like any texture); the same formats read from a buffer the
           game DREW into are the full-screen effects' tricks and are not drawn. */
        if (sWpReady && tbp == WP_FBP * 32 && (sWpAgain >= 0 || (sWpSigSet && wp_sig() == sWpSig)) &&
            !(sWtOn && sWpMarked && sWpAgain < 0 && sWpSlot == WP_OTHER && !FX_WT_OFF)) { /* the work page as a texture: its picture, when whole
                                                                     (158: with the game's markers, only inside a shadow's; outside, the glow's work target) */
            uint32_t tbw = (uint32_t)((t0 >> 14) & 0x3F), twl = (uint32_t)((t0 >> 26) & 15), thl = (uint32_t)((t0 >> 30) & 15);
            if (sWpAgain >= 0 ? (!sWpSlotOk[sWpAgain] || page || !vgl_bt3_split_on || gVitaWpOff) /* (the silhouette drawn before, if there is one) */
                              : (!sWpValid || page || !vgl_bt3_split_on || gVitaWpOff || sWpClrN > 0)) { /* (a clear held back: the page is empty) */
                gVitaWp[4]++;
                sRejWhy = 5;
                return 0;
            }
            /* read as PSMCT24 by the shadow (gfxm_e.c, ObjGs_AddShadowTexEnv): the alpha is TEXA's, 0 where
               the colour is black (AEM) and TA0 = 0x80 elsewhere, which is what the page's own alpha holds (cleared to
               black with alpha 0, the fighter drawn grey with alpha 0x80) */
            int ct24ok = tpsm == 1 && ((gGs.texa >> 15) & 1) && (gGs.texa & 0xFF) == 0x80;
            if ((tpsm != 0 && !ct24ok) || tbw != 4 || twl != 8 || thl != 8) {
                gVitaWp[5]++;
                sRejWhy = 5;
                return 0;
            }
            gVitaWp[3]++;
            pageTex = 1;
            pageSlot = sWpAgain >= 0 ? sWpAgain : sWpSlot;
            if (sWpAgain < 0 && sWpSlot < WP_OTHER) {
                sWpSlotOk[sWpSlot] = 1; /* (the silhouette whole, laid on the ground: kept for the picture after) */
            }
        }
        sTexXf = 0;
        if (!pageTex && sWtOn && vgl_bt3_split_on && !FX_WT_OFF && (t0 & 0x1F) == 0 && tpsm == 0) {
            /* a work target as a texture, or the picture itself read into one */
            uint32_t tbw = (uint32_t)((t0 >> 14) & 0x3F);
            float tw = (float)(1u << ((t0 >> 26) & 15)), th = (float)(1u << ((t0 >> 30) & 15));
            int mainPage = gGsMainFbp >= 0 ? tbp / 32 == (uint32_t)gGsMainFbp : (tbp / 32 == 0 || tbp / 32 == 0x70);
            if ((wtS = wt_find(tbp / 32, tbw)) != NULL) {
                if (!wtS->drawn || wtS->stale || wtS == wtT || wt_sig(wtS) != wtS->sig) {
                    gVitaWt[3]++;
                    sRejWhy = 5;
                    return 0;
                }
                pageTex = 2;
                sTexXf = 1;
                sTexXfV[0] = 0.0f; sTexXfV[1] = tw / (float)wtS->w; sTexXfV[2] = 0.0f; sTexXfV[3] = th / (float)wtS->h;
                gVitaWt[2]++;
            } else if (mainPage && page == 2 && tbw == 8 && sOlTex[gxm_back_buffer_index % 3] != 0) {
                pageTex = 3; /* (the scene drawing the picture has ended with the switch to the target: it is in memory) */
                sTexXf = 1;
                sTexXfV[0] = (float)VIEW_X / (float)SCR_W; sTexXfV[1] = tw / 512.0f * (float)VIEW_W / (float)SCR_W;
                sTexXfV[2] = 0.0f; sTexXfV[3] = th / 448.0f;
                gVitaWt[4]++;
            }
        }
        if (pageTex) {
        } else if ((t0 & 0x1F) == 0 || tpsm == 0x1B || tpsm == 0x24 || tpsm == 0x2C) {
            if (tbp / 32 == 0 || tbp / 32 == 0x70) {
                sRejWhy = 4;
                return 0; /* the picture itself as a texture */
            }
            for (i = 0; i < sWorkCount; i++) {
                if (sWork[i].fbp == tbp / 32 && sWork[i].gen == gGsPageGen[sWork[i].fbp & 511]) {
                    sRejWhy = 5;
                    return 0; /* a buffer the game drew into and nothing was uploaded over since */
                }
            }
        }
        sTexDeferOk = sTexMayDefer && abe && !pageTex;
        sTexDeferredNow = 0;
        d->tex = pageTex == 1 ? sWpTexK[pageSlot] : pageTex == 2 ? wtS->tex : pageTex == 3 ? sOlTex[gxm_back_buffer_index % 3] : texture_get(ctx);
        if (pageTex) { /* a picture drawn here: its alpha as stored (0x80 = 1), not the uploads' (the shader scales those) */
            d->pad_[0] = ((t0 >> 34) & 1) ? 1 : 2;
        }
        sTexDeferOk = 0;
        if (d->tex == 0 && sTexDeferredNow) {
            sTexDeferredNow = 0;
            sRejWhy = 7;
            return 0; /* (its texture is made in the next picture: texture_get_k) */
        }
        d->tme = d->tex != 0;
        d->env = ((t0 >> 35) & 3) == 1 ? (((t0 >> 34) & 1) ? 1 : 2) : 0; /* DECAL: replace, or keep the vertex alpha */
        d->linear = (uint8_t)((gGs.tex1[ctx] >> 5) & 1);
        tex1_note(gGs.tex1[ctx], t0);
        d->wrapS = (cl & 3) != 0;
        d->wrapT = ((cl >> 2) & 3) != 0;
    }
    d->topo = type == 1;
    if (abe) {
        /* ((A - B) * C >> 7) + D as source and destination factors (gs_gpu.c, blend_of) */
        uint64_t al = gGs.alpha[ctx];
        int A = al & 3, B = (al >> 2) & 3, C = (al >> 4) & 3, D = (al >> 6) & 3, k[2];
        int fixOne = ((al >> 32) & 0xFF) >= 0x40;

        for (i = 0; i < 2; i++) {
            int c = (A == i) - (B == i), one = D == i;
            k[i] = c == 0 ? one : c > 0 ? (one ? 5 : 2) : (one ? 4 : 3); /* 0 nothing, 1 one, 2 C, 3 -C, 4 1 - C, 5 1 + C */
        }
        d->blend = 1;
        for (i = 0; i < 2; i++) {
            uint8_t f = k[i] == 0 ? 0 : k[i] == 1 || k[i] == 5 ? 1 :
                        C == 0 ? (k[i] == 4 ? 3 : 2) : C == 1 ? (k[i] == 4 ? 5 : 4) : (k[i] == 4 ? (fixOne ? 0 : 1) : (fixOne ? 1 : 0));
            if (i == 0) { d->src = f; } else { d->dst = f; }
        }
        d->eq = k[0] == 3 ? 1 : k[1] == 3 ? 2 : 0;
    }
    d->zwrite = !((gGs.zbuf[ctx] >> 32) & 1);
    if ((test & 1) && ((test >> 12) & 3) != 0 && ((test >> 1) & 7) != 1) { /* (ALWAYS: no pixel fails) */
        int af = (int)((test >> 12) & 3);
        if (sAfSeenN < 48) { /* (the log: each kind once) */
            unsigned key = (unsigned)(test & 0xFFFFF) | (unsigned)d->zwrite << 20 | (unsigned)page << 21 | (unsigned)abe << 23 | (unsigned)tme << 24 |
                           (unsigned)(((test >> 16) & 7) << 25);
            int j;
            for (j = 0; j < sAfSeenN && sAfSeen[j] != key; j++) {
            }
            if (j == sAfSeenN) {
                sAfSeen[sAfSeenN++] = key;
                Vita_Log("vgl: afail: frame %u test %05x (afail %d func %d ref %02x ztest %d zfunc %d) zwrite %d page %d abe %d tme %d alpha %010llx\n",
                         gGsFrame, (unsigned)(test & 0xFFFFF), af, (int)((test >> 1) & 7), (unsigned)((test >> 4) & 0xFF),
                         (int)((test >> 16) & 1), (int)((test >> 17) & 3), d->zwrite, page, abe, tme, (unsigned long long)(gGs.alpha[ctx] & 0xFFFFFFFFFFull));
            }
        }
        if (gVitaAFail != 0 && ((test >> 1) & 7) == 0) {
            afx = af; /* NEVER: every pixel fails, so it writes just what AFAIL says (below) */
        } else if (gVitaAFail == 1 && af != 2) {
            d->zwrite = 0; /* the pixels that fail keep the depth: those that pass write it too, here none do */
        } else if (gVitaAFail == 1 || gVitaAFail == 2) {
            d->atest = 1; /* the pixels that fail not drawn at all */
            d->afunc = (uint8_t)((test >> 1) & 7);
            d->aref = (uint8_t)((test >> 4) & 0xFF);
        }
    }
    if ((test & 1) && ((test >> 12) & 3) == 0) {
        d->atest = 1;
        d->afunc = (uint8_t)((test >> 1) & 7);
        d->aref = (uint8_t)((test >> 4) & 0xFF);
    }
    if (!(gVglSafe & 64)) {
        d->date = ((test >> 14) & 1) ? (uint8_t)(1 + ((test >> 15) & 1)) : 0;
        d->fba = (uint8_t)(gGs.fba[ctx] & 1);
    }
    d->ztest = (uint8_t)((test >> 16) & 1);
    d->zfunc = (uint8_t)((test >> 17) & 3);
    d->mask = (uint8_t)(((fbmsk & 0xFF) != 0xFF) | ((fbmsk & 0xFF00) != 0xFF00) << 1 | ((fbmsk & 0xFF0000) != 0xFF0000) << 2 |
                        ((fbmsk & 0xFF000000u) != 0xFF000000u) << 3);
    if (afx == 1) {
        d->zwrite = 0;
    } else if (afx == 2) {
        d->mask = 0;
    } else if (afx == 3) {
        d->zwrite = 0;
        d->mask &= 7;
    }
    d->sop = (d->mask & 8) && d->fba ? 2 : 0; /* (a flat fill's own alpha: GsGpu_Draw) */
    x0 = (int)(sc & 0x7FF);
    x1 = (int)((sc >> 16) & 0x7FF) + 1;
    y0 = (int)((sc >> 32) & 0x7FF);
    y1 = (int)((sc >> 48) & 0x7FF) + 1;
    if (page == 2) { /* a work target: its own size */
        int tw2 = wtT->w, th2 = wtT->h;
        if (x1 > tw2) { x1 = tw2; }
        if (y1 > th2) { y1 = th2; }
        if (x1 <= x0 || y1 <= y0 || d->date) {
            sRejWhy = 6;
            return 0;
        }
        d->target = (uint8_t)(WT_BASE + (wtT - sWt));
        d->sx = (int16_t)x0;
        d->sw = (int16_t)(x1 - x0);
        /* a target's row r is the GS's row r (put: row 0 at clip space -1, vitaGL's row 0 of a framebuffer
           object), and so is the scissor's: vitaGL takes glScissor's y as the object's row. It was h - y1 (as for the
           picture, whose rows count from the bottom): a scissor short of the target's height was upside down, and
           the depth blur (stg_c.c StgBlur_Draw: 224 rows of the 256) left the top 32 rows of its two targets as they
           were, black: the dark top of the picture in some stages, the black blocks of the Bardock fight) */
        d->sy = (int16_t)y0;
        d->sh = (int16_t)(y1 - y0);
        d->sop = 0;
        d->fba = 0;
        return 1;
    }
    if (page) { /* the work page: its own pixels, 256 x 256; no alpha bit 7 kept for it */
        if (x1 > 256) { x1 = 256; }
        if (y1 > 256) { y1 = 256; }
        if (x1 <= x0 || y1 <= y0 || d->date) {
            sRejWhy = 6;
            return 0;
        }
        d->target = (uint8_t)(1 + sWpSlot); /* (by slot) */
        d->sop = 0;
        d->fba = 0;
        d->sx = (int16_t)x0;
        d->sw = (int16_t)(x1 - x0);
        d->sy = (int16_t)y0; /* (as a work target's, above) */
        d->sh = (int16_t)(y1 - y0);
        return 1;
    }
    if (x1 > 512) { x1 = 512; }
    if (y1 > 448) { y1 = 448; }
    if (x1 <= x0 || y1 <= y0) {
        sRejWhy = 6;
        return 0;
    }
    /* the picture's scissor kept in GS pixels (x0, y0, x1, y1), turned into the display's pixels when it is
       applied (scissor_of): a kept part's draws (dp_record) outlive a change of the picture's size */
    d->sx = (int16_t)x0;
    d->sy = (int16_t)y0;
    d->sw = (int16_t)x1;
    d->sh = (int16_t)y1;
    return 1;
}

static int sBinCount;
static void flush_bins(void);
static void flush_bins_c(int why);
extern int gDpCapture;        /* (direct parts, below) */
volatile int gVglL2Drawn;     /* a strip of program 2 (fighters' flat shadow / silhouette, layer 2) was drawn: gs_vu1.c sends them all */
unsigned gVglL2[2];           /* for the log: layer-2 strips drawn, dropped */
static int sDpRecBad, sDpItemN;
static int sDsOn;             /* (direct strips, below) */
static void dp_free_pending(void);
/* A drawn fight frame written to the log, state by state, to find out what a wrong-looking part is drawn with:
   the GS registers of every run of primitives and what the renderer made of them. Two frames a fight. */
static int sDumping, sDumpLines;
static void dump_state(const char *what, int ctx, int type, const State *d, float x0, float y0, float x1, float y1, unsigned count) {
    static uint64_t last[9];
    static int lastCtx = -1, lastOk = -1;
    static unsigned run;
    uint64_t r[9];

    r[0] = gGs.prim; r[1] = gGs.tex0[ctx]; r[2] = gGs.test[ctx]; r[3] = gGs.alpha[ctx]; r[4] = gGs.frame[ctx];
    r[5] = gGs.fba[ctx]; r[6] = gGs.zbuf[ctx]; r[7] = gGs.clamp[ctx]; r[8] = gGs.texa;
    if (what[0] == 's') {
        r[1] = 0; /* (strips: a line per state, not per texture) */
    }
    run++;
    if (memcmp(r, last, sizeof(r)) == 0 && ctx == lastCtx && (d != NULL) == lastOk) {
        return;
    }
    memcpy(last, r, sizeof(r));
    lastCtx = ctx;
    lastOk = d != NULL;
    if (sDumpLines++ > 1500) {
        return;
    }
    Vita_Log("dump: (%u before) %s type %d ctx %d n %u at %.0f,%.0f-%.0f,%.0f: PRIM %03x TEX0 %08x%08x TEST %05x ALPHA %02x fix %02x FRAME %03x msk %08x FBA %u ZBUF %03x zmsk %u CLAMP %x TEXA %08x%08x",
             run - 1, what, type, ctx, count, x0, y0, x1, y1, (unsigned)r[0], (unsigned)(r[1] >> 32), (unsigned)r[1], (unsigned)r[2], (unsigned)(r[3] & 0xFF),
             (unsigned)(r[3] >> 32), (unsigned)(r[4] & 0x1FF), (unsigned)(r[4] >> 32), (unsigned)r[5], (unsigned)(r[6] & 0x1FF), (unsigned)(r[6] >> 32) & 1, (unsigned)r[7],
             (unsigned)(r[8] >> 32), (unsigned)r[8]);
    if (d != NULL) {
        Vita_Log(" -> tex %u env %u blend %u (%u,%u,%u) atest %u (%u,%u) z %u (%u,%u) mask %x fba %u date %u sop %u\n", (unsigned)d->tex, d->env, d->blend, d->src, d->dst, d->eq,
                 d->atest, d->afunc, d->aref, d->ztest, d->zfunc, d->zwrite, d->mask, d->fba, d->date, d->sop);
    } else {
        Vita_Log(" -> NOT DRAWN\n");
    }
    run = 0;
}
/* The picture's alpha as the GS would have it, by cells of 32 x 32 GS pixels: the value, or -1 when not known
   (anything but a flat sprite of one alpha over the whole cell makes it not known). */
static int16_t sAm[14][16];
static void am_write(float x0, float y0, float x1, float y1, int val) {
    int cx0, cy0, cx1, cy1, cx, cy;
    if (!(x1 > x0 && y1 > y0)) {
        return;
    }
    x0 = x0 < 0.0f ? 0.0f : x0; y0 = y0 < 0.0f ? 0.0f : y0; x1 = x1 > 512.0f ? 512.0f : x1; y1 = y1 > 448.0f ? 448.0f : y1;
    if (!(x1 > x0 && y1 > y0)) {
        return;
    }
    cx0 = (int)x0 >> 5; cy0 = (int)y0 >> 5; cx1 = ((int)x1 - 1) >> 5; cy1 = ((int)y1 - 1) >> 5;
    if (cx1 > 15) { cx1 = 15; } if (cy1 > 13) { cy1 = 13; }
    if (val < 0) { /* "not known" over the cells: each row filled at once (the sea's triangles, 2000 a picture) */
        if (cx1 >= cx0) {
            for (cy = cy0; cy <= cy1; cy++) {
                memset(&sAm[cy][cx0], 0xFF, (size_t)(cx1 - cx0 + 1) * sizeof(sAm[0][0]));
            }
        }
        return;
    }
    for (cy = cy0; cy <= cy1; cy++) {
        for (cx = cx0; cx <= cx1; cx++) {
            int whole = val >= 0 && x0 <= (float)(cx * 32) && x1 >= (float)(cx * 32 + 32) && y0 <= (float)(cy * 32) && y1 >= (float)(cy * 32 + 32);
            sAm[cy][cx] = (int16_t)(whole ? val : -1);
        }
    }
}
static int am_zero(float x0, float y0, float x1, float y1) {
    int cx0, cy0, cx1, cy1, cx, cy;
    x0 = x0 < 0.0f ? 0.0f : x0; y0 = y0 < 0.0f ? 0.0f : y0; x1 = x1 > 512.0f ? 512.0f : x1; y1 = y1 > 448.0f ? 448.0f : y1;
    if (!(x1 > x0 && y1 > y0)) {
        return 1;
    }
    cx0 = (int)x0 >> 5; cy0 = (int)y0 >> 5; cx1 = ((int)x1 - 1) >> 5; cy1 = ((int)y1 - 1) >> 5;
    if (cx1 > 15) { cx1 = 15; } if (cy1 > 13) { cy1 = 13; }
    for (cy = cy0; cy <= cy1; cy++) {
        for (cx = cx0; cx <= cx1; cx++) {
            if (sAm[cy][cx] != 0) {
                return 0;
            }
        }
    }
    return 1;
}
/* a model's draw that reads the picture's alpha: the alpha is not left out for a while (the models' alpha is not followed) */
/* the picture's alpha, read for real (a blend with the picture's alpha: TEST.DATE is the stencil's,
   see flush) only a few times a picture, early in it, after a full-screen write of it: once a picture has made as
   many such reads as the one before, and the one before had written all of the alpha before its first, what is
   written to the alpha after them is never read (the next picture writes it all again first). A draw that changes
   nothing else is left out then: the stage's dust, 19 screens in 60 triangles, drawn with an alpha of 0 in the
   busiest pictures. */
static unsigned sAlReads, sAlPrevReads;
static int sAlFull, sAlFullBefore, sAlPrevOk;
unsigned gVitaAlDead[2]; /* for the log: primitives and screens left out by it */
static void al_read(void) {
    if (sAlReads++ == 0) {
        sAlFullBefore = sAlFull;
    }
}
static int al_dead(void) {
    return sAlPrevOk && sAlPrevReads != 0 && sAlReads >= sAlPrevReads && !(gVglSafe & 8192);
}
static void al_frame_end(int drawn) {
    if (drawn) {
        sAlPrevReads = sAlReads;
        sAlPrevOk = sAlReads == 0 || sAlFullBefore;
    }
    sAlReads = 0;
    sAlFull = sAlFullBefore = 0;
}
static struct { uint8_t site, src, dst, eq, date, tme, mask, zw; unsigned n; } sAmR[8]; /* (for the log: which model draws read the alpha) */
static unsigned sAmRN;
static void am_model_reads(const State *st, int site) {
    if (st->blend && (st->src >= 4 || st->dst >= 4)) {
        al_read();
    }
    if (st->date || (st->blend && (st->src >= 4 || st->dst >= 4))) {
        unsigned k;
        sAlphaRead = gGsFrame;
        gVglSkip[3]++;
        for (k = 0; k < sAmRN; k++) {
            if (sAmR[k].site == site && sAmR[k].src == st->src && sAmR[k].dst == st->dst && sAmR[k].eq == st->eq && sAmR[k].date == st->date && sAmR[k].tme == st->tme &&
                sAmR[k].mask == st->mask && sAmR[k].zw == st->zwrite) {
                break;
            }
        }
        if (k == sAmRN && sAmRN < 8) {
            sAmR[k].site = (uint8_t)site; sAmR[k].src = st->src; sAmR[k].dst = st->dst; sAmR[k].eq = st->eq; sAmR[k].date = st->date; sAmR[k].tme = st->tme;
            sAmR[k].mask = st->mask; sAmR[k].zw = st->zwrite; sAmR[k].n = 0;
            sAmRN++;
        }
        if (k < 8) {
            sAmR[k].n++;
        }
    }
}
void Vita_AmLog(void) { /* vita_main.c: the model draws that read the picture's alpha, by kind */
    char line[600];
    int len = snprintf(line, sizeof(line), "vgl:   model draws that read the picture's alpha (where (0 batches, 1 kept parts, 2 strips) / blend src dst eq / DATE / textured / mask / depth write: count):");
    unsigned k;
    {   /* vitaGL's fragment programs by blend settings, kept with each program (port's vitaGL change) */
        extern unsigned vgl_bt3_frag_patches, vgl_bt3_frag_hits, vgl_bt3_frag_us, vgl_bt3_layout_hits, vgl_bt3_layout_misses;
        Vita_Log("vgl:   fragment programs: %u made by the shader patcher (%u us), %u times one kept with its program taken instead | vertex layouts: %u found again, "
                 "%u made\n", vgl_bt3_frag_patches, vgl_bt3_frag_us, vgl_bt3_frag_hits, vgl_bt3_layout_hits, vgl_bt3_layout_misses);
        vgl_bt3_frag_patches = vgl_bt3_frag_hits = vgl_bt3_frag_us = vgl_bt3_layout_hits = vgl_bt3_layout_misses = 0;
        Vita_Log("vgl:   pictures handed to the chip in parts: %u, %u parts after the first | textures read with the GS's last loaded palette (CLD 0, CBP 0): %u\n",
                 sSplitPics, vgl_bt3_splits, gVitaClutSub);
        gVitaClutSub = 0;
        sSplitPics = 0;
        vgl_bt3_splits = 0;
        if (gVitaBit7Passes != 0) {
            Vita_Log("vgl:   second passes for alpha bit 7 (primitives whose alpha crosses 0x80): %u\n", gVitaBit7Passes);
            gVitaBit7Passes = 0;
        }
        if (sWpReady) {
            Vita_Log("vgl:   work page (ground shadows): %u GS primitives drawn into it, %u not (the page spoiled) | %u clears, %u of them drawn | reads: %u given the page, "
                     "%u not (spoiled), %u not (another form) | %u switches of target | silhouette strips: %u drawn directly, %u the other way | silhouettes: %u drawn, %u shadows laid with the one before\n",
                     gVitaWp[0], gVitaWp[1], gVitaWp[2], gVitaWp[7], gVitaWp[3], gVitaWp[4], gVitaWp[5], gVitaWp[6], gVitaWp[8], gVitaWp[9], gVitaWp[10], gVitaWp[11]);
            memset(gVitaWp, 0, sizeof(gVitaWp));
        }
        if (sOlProg != 0) {
            Vita_Log("vgl:   outline: %u drawn, %u not (the scene not split), %u pictures of the alpha it reads\n", gVitaOl[0], gVitaOl[1], gVitaOl[2]);
            memset(gVitaOl, 0, sizeof(gVitaOl));
        }
        if (sWtOn || sTblOn) {
            char by[200];
            int bl = 0, k2;
            by[0] = 0;
            for (k2 = 0; k2 < 8 && sTblCbp[k2][0] != 0; k2++) {
                bl += snprintf(by + bl, sizeof(by) - (size_t)bl, " %03x:%u/%u", sTblCbp[k2][0], sTblCbp[k2][1], sTblCbp[k2][2]);
                sTblCbp[k2][1] = sTblCbp[k2][2] = 0;
            }
            Vita_Log("vgl:   glow: %u drawn, %u left out (nothing to add)\n", sGlowN, sGlowNull);
            sGlowN = sGlowNull = 0;
            Vita_Log("vgl:   effects: %u primitives into work targets, %u not (a pass missed) | reads of targets: %u given, %u not (stale) | %u reads of the picture | "
                     "table passes: %u (%u from the fog), %u not drawn, by palette (drawn/not):%s | %u copies of the ids | %u targets made\n",
                     gVitaWt[0], gVitaWt[1], gVitaWt[2], gVitaWt[3], gVitaWt[4], gVitaWt[5], gVitaWt[6], gVitaWt[8], by, gVitaWt[7], gVitaWt[9]);
            memset(gVitaWt, 0, sizeof(gVitaWt) - sizeof(gVitaWt[9]));
        }
        if (gVitaAg[0] != 0) { /* */
            Vita_Log("vgl:   additive groups: %u, drawn in %u draws; %u changes of state saved\n", gVitaAg[0], gVitaAg[1], gVitaAg[2]);
            memset(gVitaAg, 0, sizeof(gVitaAg));
        }
        if (sOlTailN[0] + sOlTailN[1] != 0) { /* */
            Vita_Log("vgl:   the outline's own passes left out after the native one: %u edge image reads, %u darkenings\n", sOlTailN[0], sOlTailN[1]);
            sOlTailN[0] = sOlTailN[1] = 0;
        }
        if (gVitaBoneBad != 0) { /* */
            Vita_Log("vgl:   fighters' bones not finite: %u (uniform blocks and vertices; their triangles dropped)\n", gVitaBoneBad);
            gVitaBoneBad = 0;
        }
        {   /* the primitives into work targets refused, by target and reason */
            char rl[400];
            int rn = 0, k, w;
            rl[0] = 0;
            for (k = 0; k < sWtN && k < WT_MAX; k++) {
                for (w = 0; w < 8; w++) {
                    if (sWtRej[k][w] != 0) {
                        rn += snprintf(rl + rn, sizeof(rl) - (size_t)rn, " page %03x why %d: %u;", (unsigned)sWt[k].fbp, w, sWtRej[k][w]);
                    }
                }
            }
            if (rn > 0) {
                Vita_Log("vgl:   work targets' primitives refused:%s\n", rl);
            }
            memset(sWtRej, 0, sizeof(sWtRej));
        }
        if (gVitaFxSweep) {
            extern unsigned long long gFxGpu[16][2];
            char fl[300];
            int fn = 0, m;
            fl[0] = 0;
            for (m = 0; m < 16; m++) {
                if (gFxGpu[m][1] != 0) {
                    fn += snprintf(fl + fn, sizeof(fl) - (size_t)fn, " %d: %.1f ms (%u);", m, (double)gFxGpu[m][0] / (double)gFxGpu[m][1] / 1000.0, (unsigned)gFxGpu[m][1]);
                }
            }
            Vita_Log("vgl:   fxsweep: the chip's time per picture by the effects left out (1 table passes, 2 glow, 4 work targets, 8 outline), %s:%s\n",
                     gVitaFxSweep == 3 ? "this stretch" : "this fight so far", fl);
            if (gVitaFxSweep == 3) {
                memset(gFxGpu, 0, sizeof(gFxGpu));
            }
        }
        if (sRtOn) {
            Vita_Log("vgl:   render thread (core %d): %llu draws handed over (%llu KB), busy %llu ms; the drawing thread waited for it to finish %llu times (%llu ms in all), "
                     "for room in the ring %llu times (%llu ms); %llu made by the drawing thread itself\n", gVitaRtCore, gVitaRt[0], gVitaRt[1] >> 10, gVitaRt[6] / 1000,
                     gVitaRt[2], gVitaRt[3] / 1000, gVitaRt[4], gVitaRt[5] / 1000, gVitaRt[7]);
            memset(gVitaRt, 0, sizeof(gVitaRt));
        }
    }
    if (sAmRN == 0) {
        return;
    }
    for (k = 0; k < sAmRN; k++) {
        len += snprintf(line + len, sizeof(line) - (size_t)len, " %u/%u %u %u/%u/%u/%x/%u: %u;", sAmR[k].site, sAmR[k].src, sAmR[k].dst, sAmR[k].eq, sAmR[k].date, sAmR[k].tme, sAmR[k].mask,
                        sAmR[k].zw, sAmR[k].n);
    }
    Vita_Log("%s\n", line);
    sAmRN = 0;
}
/* (for the log) the area of a sprite or a triangle in GS pixels, within the picture */
static float prim_area(int type, const GsVertex *v) {
    float area;
    int i;
    if (type == 6) {
        float cx0 = v[0].x < 0.0f ? 0.0f : v[0].x > 512.0f ? 512.0f : v[0].x, cx1 = v[1].x < 0.0f ? 0.0f : v[1].x > 512.0f ? 512.0f : v[1].x;
        float cy0 = v[0].y < 0.0f ? 0.0f : v[0].y > 448.0f ? 448.0f : v[0].y, cy1 = v[1].y < 0.0f ? 0.0f : v[1].y > 448.0f ? 448.0f : v[1].y;
        area = (cx1 - cx0) * (cy1 - cy0);
    } else {
        float bx0 = v[0].x, bx1 = v[0].x, by0 = v[0].y, by1 = v[0].y, box;
        for (i = 1; i < 3; i++) {
            if (v[i].x < bx0) { bx0 = v[i].x; } if (v[i].x > bx1) { bx1 = v[i].x; }
            if (v[i].y < by0) { by0 = v[i].y; } if (v[i].y > by1) { by1 = v[i].y; }
        }
        bx0 = bx0 < 0.0f ? 0.0f : bx0; by0 = by0 < 0.0f ? 0.0f : by0; bx1 = bx1 > 512.0f ? 512.0f : bx1; by1 = by1 > 448.0f ? 448.0f : by1;
        box = bx1 > bx0 && by1 > by0 ? (bx1 - bx0) * (by1 - by0) : 0.0f;
        area = 0.5f * ((v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y));
        area = area < 0.0f ? -area : area;
        if (area > box) { area = box; }
    }
    return area < 0.0f ? -area : area;
}
/* (for the log: a picture now and then, which states the GS primitives' area is drawn with) */
static void cov_add(const State *d, int ctx, int type, float area, float skip, int amin, int amax) {
    uint64_t t0 = gGs.tex0[ctx];
    uint32_t key = (uint32_t)d->tex * 0x9E3779B1u ^ (uint32_t)(d->blend | d->src << 1 | d->dst << 4 | d->eq << 7 | d->tme << 9 | type << 10 | d->ztest << 14 | d->zwrite << 15 |
                                                             d->mask << 16 | d->date << 20 | d->sop << 22 | d->fba << 24) * 0x85EBCA6Bu;
    int k;
    for (k = 0; k < sCovN && sCov[k].key != key; k++) {
    }
    if (k == sCovN && sCovN < 64) {
        sCovN++;
        memset(&sCov[k], 0, sizeof(sCov[k]));
        sCov[k].key = key;
        sCov[k].t0 = t0;
        sCov[k].alpha = gGs.alpha[ctx];
        sCov[k].test = gGs.test[ctx];
        sCov[k].d = *d;
        sCov[k].type = type;
        sCov[k].amin = 255;
    }
    if (k < sCovN) {
        sCov[k].area += area;
        sCov[k].skip += skip;
        sCov[k].n++;
        if (amin < sCov[k].amin) { sCov[k].amin = amin; }
        if (amax > sCov[k].amax) { sCov[k].amax = amax; }
        if (sCovQr > sCov[k].qr) { sCov[k].qr = sCovQr; }
        run_add(k, 1, area);
    }
}
static void wp_emit_clear(void) {
    static const float kWhole[4] = {0.0f, 0.0f, 256.0f, 256.0f};
    int k, n = sWpClrWhole ? 1 : sWpClrN;
    if (sWpClrN == 0) {
        return;
    }
    if (!sHaveState || memcmp(&sWpClrSt, &sCur, sizeof(State)) != 0 || sVertCount + 6 * n > MAX_VERTS) {
        flush();
        sCur = sWpClrSt;
        sHaveState = 1;
    }
    put_target(1);
    sTexXf = 0;
    for (k = 0; k < n; k++) {
        const float *r = sWpClrWhole ? kWhole : sWpClr[k];
        put(&sWpClrV, r[0], r[1], 0.0f, 0.0f, 1.0f, &sWpClrV, sWpClrZmax);
        put(&sWpClrV, r[2], r[1], 0.0f, 0.0f, 1.0f, &sWpClrV, sWpClrZmax);
        put(&sWpClrV, r[0], r[3], 0.0f, 0.0f, 1.0f, &sWpClrV, sWpClrZmax);
        put(&sWpClrV, r[2], r[1], 0.0f, 0.0f, 1.0f, &sWpClrV, sWpClrZmax);
        put(&sWpClrV, r[2], r[3], 0.0f, 0.0f, 1.0f, &sWpClrV, sWpClrZmax);
        put(&sWpClrV, r[0], r[3], 0.0f, 0.0f, 1.0f, &sWpClrV, sWpClrZmax);
    }
    gVitaWp[7]++;
    sWpClrN = 0;
    sWpClrWhole = 0;
}
static int sAnchor;
extern int Port_IsWide(void), Port_AspectMilli(void);
extern int gPortMenuMode; /* port/src/headless.c: the menus are running */
static void GsGpu_Draw_impl(int type, int ctx, const GsVertex *v);
unsigned long long gVitaDrawCyc[8]; /* GS primitives drawn the GS's way (not in a run), lists measured part by part: cycles by kind */
unsigned gVitaDrawCnt[8];
void GsGpu_Draw(int type, int ctx, const GsVertex *v) {
    P_IN(9);
    if (gVitaZoneOn & 1) {
        uint32_t c0 = 0, c1 = 0;
        scePerfArmPmonGetCounterValue(0, 31, &c0);
        GsGpu_Draw_impl(type, ctx, v);
        scePerfArmPmonGetCounterValue(0, 31, &c1);
        gVitaDrawCyc[type & 7] += c1 - c0;
        gVitaDrawCnt[type & 7]++;
    } else {
        GsGpu_Draw_impl(type, ctx, v);
    }
    gVglCalls[4]++;
    P_OUT;
}
/* one axis of a 2D piece: coordinates c0 at position p0 and c1 at p1 (GS pixels); the GS reads the
   texels of the pixels from the nearer position up to one short of the farther: the farther corner's coordinate
   moved back by one pixel's worth (pieces under a pixel across are left alone) */
static inline void spr_far_edge(float p0, float p1, float *c0, float *c1) {
    float dp = p1 - p0, dc;
    if (dp < 1.0f && dp > -1.0f) {
        return;
    }
    dc = (*c1 - *c0) / dp;
    if (dp > 0.0f) {
        *c1 -= dc;
    } else {
        *c0 -= dc;
    }
}
/* A triangle of a 2D piece drawn as a strip (the HUD's pieces: two triangles over a rectangle, the texel
   coordinates (UV) along its sides): the far corners as spr_far_edge. Only a triangle whose corners are corners of
   one rectangle, with U following X alone and V following Y alone, so the two triangles of the piece agree. */
static void tri_far_edge(const GsVertex *v, float *s, float *t) {
    float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y, sA = 0.0f, sB = 0.0f, tA = 0.0f, tB = 0.0f, ds, dt;
    int i, nA = 0, nB = 0, mA = 0, mB = 0;
    for (i = 1; i < 3; i++) {
        if (v[i].x < x0) { x0 = v[i].x; }
        if (v[i].x > x1) { x1 = v[i].x; }
        if (v[i].y < y0) { y0 = v[i].y; }
        if (v[i].y > y1) { y1 = v[i].y; }
    }
    if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) {
        return;
    }
    for (i = 0; i < 3; i++) {
        if (v[i].x == x0) {
            if (nA++ && s[i] != sA) { return; }
            sA = s[i];
        } else if (v[i].x == x1) {
            if (nB++ && s[i] != sB) { return; }
            sB = s[i];
        } else {
            return;
        }
        if (v[i].y == y0) {
            if (mA++ && t[i] != tA) { return; }
            tA = t[i];
        } else if (v[i].y == y1) {
            if (mB++ && t[i] != tB) { return; }
            tB = t[i];
        } else {
            return;
        }
    }
    if (!nA || !nB || !mA || !mB) {
        return;
    }
    ds = (sB - sA) / (x1 - x0);
    dt = (tB - tA) / (y1 - y0);
    for (i = 0; i < 3; i++) {
        if (v[i].x == x1) { s[i] -= ds; }
        if (v[i].y == y1) { t[i] -= dt; }
    }
}
/* The quick way for a GS triangle whose state is the triangle's before it: nothing about the GS's state
   has changed since (gs_core.c gGsStateGen: no register of the state written with another value), nor GS memory
   (the epoch, for a textured one), nor the picture. The state the full way worked out for that one is taken as it
   is, and of the bookkeeping that follows only what can matter for such a state: a state that reads the picture's
   alpha, or one that the full way could leave out as changing nothing, never goes this way. The sea of Kame House
   is ~1500 such triangles a picture, one after another in the ordering table. bt3.txt fastlane=0: off. */
int gVitaFastLane = 1;
unsigned gVitaFlWhy[2][8]; /* GS triangles / sprites the full way not open to the next: not drawn, texture moved, DATE, reads the alpha, writes no colour or depth, into a target, other */
unsigned gVitaFl[3]; /* (the log) GS triangles drawn the quick way, the full way with a state the quick way could take next, the full way */
extern uint32_t gGsStateGen;
static struct { uint32_t gen, frame, epoch; int ctx, ok, type; State d; } sFL; /* (type, 3 or 6: sprites too) */
/* The quick way's states remembered by PRIM and TEX0: while no other register of the state has changed
   (gs_core.c gGsStateGenX) in this picture, nor GS memory for a textured one, a triangle whose PRIM and TEX0 are one
   of these takes its state as it was worked out. The sea's runs of Kame House alternate between three textures (its
   two layers and the shore's), ~120 runs a picture, and each run's first triangle had its state worked out again. */
extern uint32_t gGsStateGenX;
#define FLM 8
static struct { uint64_t prim, tex0; uint32_t genX, frame, epoch; int ctx, ok; State d; } sFLm[FLM];
static unsigned sFLmNext;
unsigned gVitaFlm[2]; /* (the log) states taken from these, put in them */
static void flm_put(int ctx, const State *d) {
    uint64_t prim = gGs.prim, t0 = gGs.tex0[ctx];
    unsigned k;
    if ((t0 >> 61) == 0 && ((t0 >> 37) & 0x3FFF) == 0) {
        return; /* (the palette the GS loaded last: texture_get_k; not remembered) */
    }
    for (k = 0; k < FLM; k++) {
        if (sFLm[k].ok && sFLm[k].prim == prim && sFLm[k].tex0 == t0 && sFLm[k].ctx == ctx) {
            break;
        }
    }
    if (k == FLM) {
        k = sFLmNext++ % FLM;
    }
    sFLm[k].prim = prim;
    sFLm[k].tex0 = t0;
    sFLm[k].genX = gGsStateGenX;
    sFLm[k].frame = gGsFrame;
    sFLm[k].epoch = gGsEpoch;
    sFLm[k].ctx = ctx;
    sFLm[k].ok = 1;
    sFLm[k].d = *d;
    gVitaFlm[1]++;
}
/* 1: sFL holds the remembered state of this PRIM and TEX0 now */
static int flm_take(int ctx) {
    uint64_t prim = gGs.prim, t0 = gGs.tex0[ctx];
    unsigned k;
    for (k = 0; k < FLM; k++) {
        if (sFLm[k].ok && sFLm[k].prim == prim && sFLm[k].tex0 == t0 && sFLm[k].ctx == ctx && sFLm[k].genX == gGsStateGenX &&
            sFLm[k].frame == gGsFrame && (!sFLm[k].d.tme || sFLm[k].epoch == gGsEpoch)) {
            sFL.ok = 1;
            sFL.type = 3;
            sFL.gen = gGsStateGen;
            sFL.frame = gGsFrame;
            sFL.epoch = sFLm[k].epoch;
            sFL.ctx = ctx;
            sFL.d = sFLm[k].d;
            gVitaFlm[0]++;
            return 1;
        }
    }
    return 0;
}
static inline int fl_state_ok(const State *d) {
    return !d->date && !(d->blend && (d->src >= 4 || d->dst >= 4)) && (d->zwrite || (d->mask & 7) != 0) && d->target == 0;
}
/* (the quick way's bookkeeping for a triangle (as the full way does it for such a state): 0 when it is left
   out (blended with its own alpha that its corners give as 0 or 1 of 128) */
static void fl_sig(const State *dp, const GsVertex *v, uint64_t t0, int ctx, int type) { /* (as the full way: the film's signature) */
    const int n = type == 6 ? 2 : 3;
    int k;
    {
        State d = *dp;
        SIG_MIX(sSigGs, type);
        SIG_MIX(sSigGs, (uint32_t)t0);
        SIG_MIX(sSigGs, (uint32_t)(t0 >> 32));
        SIG_MIX(sSigGs, (uint32_t)gGs.alpha[ctx]);
        SIG_MIX(sSigGs, (uint32_t)gGs.test[ctx]);
        SIG_MIX(sSigGsT, d.tme ? sSigGenCur : 0u);
        for (k = 0; k < n; k++) {
            uint32_t w[8];
            memcpy(&w[0], &v[k].x, 4); memcpy(&w[1], &v[k].y, 4); memcpy(&w[2], &v[k].s, 4); memcpy(&w[3], &v[k].t, 4);
            memcpy(&w[4], &v[k].q, 4);
            w[5] = (uint32_t)v[k].z;
            w[6] = (uint32_t)v[k].r | (uint32_t)v[k].g << 8 | (uint32_t)v[k].b << 16 | (uint32_t)v[k].a << 24;
            w[7] = (uint32_t)v[k].u | (uint32_t)v[k].v << 16;
            SIG_MIX(sSigGs, w[0]); SIG_MIX(sSigGs, w[1]); SIG_MIX(sSigGs, w[2]); SIG_MIX(sSigGs, w[3]);
            SIG_MIX(sSigGs, w[4]); SIG_MIX(sSigGs, w[5]); SIG_MIX(sSigGs, w[6]); SIG_MIX(sSigGs, w[7]);
        }
        sSigGsN++;
    }
}
/* A primitive that writes the alpha from its vertices, some of them with bit 7 set and some not: the GS writes bit 7
   pixel by pixel (the alpha as it goes across the triangle), and TEST.DATE later reads it, while the stencil (this back
   end's copy of bit 7) is left as it was under such a triangle. bt3.txt bit7px=1: the stencil set to 0 where the
   triangle is drawn and to 1 again where its alpha is 0x80 or more (a second pass, flush_impl). Off by default: no
   scene was seen to need it (the full-screen grids the stages lay before their DATE layers write an alpha of 0). */
int gVitaBit7Px = 0;
static int bit7_mixed(const State *d) {
    if (!gVitaBit7Px || d->date || d->target != 0) {
        return 0;
    }
    if (d->atest && d->afunc != 1 && !((d->afunc == 5 || d->afunc == 6) && d->aref <= 0x80)) {
        return 0; /* (the second pass's test would not include the draw's own) */
    }
    return 3;
}
static int fl_book(State *dp, int ctx, const GsVertex *v, int gouraud, uint64_t t0, int type) {
    const int n = type == 6 ? 2 : 3, lastOnly = type == 6 || !gouraud; /* (a sprite too, as the full way) */
    State d = *dp;
    float bx0 = v[0].x, by0 = v[0].y, bx1 = v[0].x, by1 = v[0].y;
    int k;
    if (VITA_RARE(gVitaFilm > 0)) {
        fl_sig(&d, v, t0, ctx, type);
    }
    if ((d.mask & 8) && !d.fba && !(gVglSafe & 64) && (!d.tme || !((t0 >> 34) & 1))) { /* (as the full way) */
        int hi = 0;
        for (k = lastOnly ? n - 1 : 0; k < n; k++) {
            hi += v[k].a >= 0x80;
        }
        d.sop = hi == 0 ? 1 : (lastOnly ? hi == 1 : hi == n) ? 2 : bit7_mixed(&d);
    }
    for (k = 1; k < n; k++) {
        if (v[k].x < bx0) { bx0 = v[k].x; } if (v[k].x > bx1) { bx1 = v[k].x; }
        if (v[k].y < by0) { by0 = v[k].y; } if (v[k].y > by1) { by1 = v[k].y; }
    }
    {   /* (the GS's scissor) */
        uint64_t sc = gGs.scissor[ctx];
        float sx0 = (float)(sc & 0x7FF), sx1 = (float)((sc >> 16) & 0x7FF) + 1.0f, sy0 = (float)((sc >> 32) & 0x7FF), sy1 = (float)((sc >> 48) & 0x7FF) + 1.0f;
        if (bx0 < sx0) { bx0 = sx0; } if (by0 < sy0) { by0 = sy0; } if (bx1 > sx1) { bx1 = sx1; } if (by1 > sy1) { by1 = sy1; }
    }
    if (type == 6 && (d.mask & 8) && !d.atest && !d.date && (!d.ztest || d.zfunc == 1) && bx0 <= 0.5f && by0 <= 0.5f && bx1 >= 511.5f && by1 >= 447.5f) {
        sAlFull = 1; /* (as the full way: all of the alpha written) */
    }
    if (d.mask & 8) {
        if (type == 6 && !d.tme && !d.fba && !d.atest && !d.date && (!d.ztest || d.zfunc == 1)) {
            am_write(bx0, by0, bx1, by1, v[1].a); /* (as the full way: a flat sprite's alpha is known) */
        } else {
            am_write(bx0, by0, bx1, by1, -1);
        }
    }
    *dp = d;
    {   /* (as the full way) one blended with its own alpha that its corners give as 0 or 1 (of 128) is left out */
        float area = prim_area(type, v);
        gVglCov[d.blend ? 1 : 0] += area;
        if (d.blend && d.eq == 0 && d.src == 2 && (d.dst == 3 || d.dst == 1) && !d.zwrite && (!d.tme || d.env != 1) && !(gVglSafe & 2097152)) {
            int amax = 0, alphaDead = gGsFrame - sAlphaRead > 60u;
            for (k = lastOnly ? n - 1 : 0; k < n; k++) {
                if (v[k].a > amax) { amax = v[k].a; }
            }
            if (!alphaDead && d.sop == 0 && al_dead()) {
                alphaDead = 2;
            }
            if (amax <= 1 && (!(d.mask & 8) || alphaDead)) {
                gVglSkip[0]++;
                gVglSkipArea[0] += area;
                if (alphaDead == 2 && (d.mask & 8)) {
                    gVitaAlDead[0]++;
                    gVitaAlDead[1] += (unsigned)(area * (100.0f / (512.0f * 448.0f)));
                }
                return 0;
            }
        }
    }
    return 1;
}
static void GsGpu_Draw_impl(int type, int ctx, const GsVertex *v) {
    uint64_t prim = gGs.prim, t0 = gGs.tex0[ctx];
    int fst = (prim >> 8) & 1, gouraud = (prim >> 3) & 1, n = type == 6 ? 2 : type == 3 ? 3 : type == 1 ? 2 : 1, i;
    float tw = (float)(1u << ((t0 >> 26) & 15)), th = (float)(1u << ((t0 >> 30) & 15));
    float zmax = ((gGs.zbuf[ctx] >> 24) & 15) == 0 ? 4294967295.0f : ((gGs.zbuf[ctx] >> 24) & 15) == 1 ? 16777215.0f : 65535.0f;
    float s[3], t[3], q[3], xs[3];
    const GsVertex *flat = &v[n - 1];
    State d;
    int agOk = 0; /* one that only adds to the picture: an additive group's (flush) */

    ZM(10);
    if (gDpCapture) {
        sDpRecBad = 1; /* (a GS primitive inside a part: it is not kept) */
    }
    if (sBinCount != 0 || sDpItemN != 0) {
        flush_bins_c(0); /* the vertex programs' strips collected so far come first */
        am_write(0.0f, 0.0f, 512.0f, 448.0f, -1); /* (models: their alpha is not followed) */
    }
    if (sGlowSkip) {
        return; /* the glow's own passes: drawn natively at its marker */
    }
    if (sOlTail) { /* the outline's own passes after its marker (gfxm_a.c ObjOutline_Draw) */
        int fill = type == 6 && !((prim >> 4) & 1) && ((prim >> 6) & 1) && (gGs.alpha[ctx] & 0xFF) == 0x52;
        if (type != 6 || (sOlTail == 2 && !fill)) {
            sOlTail = 0; /* (anything else: the passes are over) */
        } else if (((prim >> 4) & 1) && t0 == 0xE64222A00ull) {
            sOlTailN[0]++;
            return; /* GfxPost_DrawTex16Texa: the 16-bit edge image (never drawn here) into the picture's alpha */
        } else if (fill) {
            sOlTail = 2; /* (all 16 of its strips, not the first only: a test, 15 drawn) */
            sOlTailN[1]++;
            return; /* GfxPost_FillBlend(0x52): the picture darkened by that alpha (done by the native outline) */
        }
    }
    if (VITA_RARE(gVitaSliver) && type >= 1 && type <= 5) { /* a line or triangle with a corner far off the picture (the
                                                     GS's own origin, 0 0, is at -1792 -1824 here): the thin white line from
                                                     Bardock's head towards the top left? Every kind, the speed lines too */
        extern int Vita_InFight(void);
        static unsigned nF, fightWas;
        int nv = type == 1 || type == 2 ? 2 : 3, k, far = 0;
        if (Vita_InFight() != (int)fightWas) {
            fightWas = (unsigned)Vita_InFight();
            nF = 0; /* (a cap for each fight) */
        }
        for (k = 0; k < nv; k++) {
            far |= v[k].x < -600.0f || v[k].x > 1100.0f || v[k].y < -600.0f || v[k].y > 1050.0f;
        }
        if (far && nF < 120 && Vita_InFight()) {
            nF++;
            Vita_Log("sliver: picture %u far type %d ctx %d PRIM %03x TEX0 %08x%08x ALPHA %02x FRAME %08x TEST %05x: (%.1f,%.1f z %u c %02x%02x%02x%02x) (%.1f,%.1f z %u c %02x%02x%02x%02x) (%.1f,%.1f z %u c %02x%02x%02x%02x)\n",
                     gGsFrame, type, ctx, (unsigned)(gGs.prim & 0x7FF), (unsigned)(gGs.tex0[ctx] >> 32), (unsigned)gGs.tex0[ctx], (unsigned)(gGs.alpha[ctx] & 0xFF),
                     (unsigned)gGs.frame[ctx], (unsigned)(gGs.test[ctx] & 0x7FFFF), v[0].x, v[0].y, (unsigned)v[0].z, v[0].r, v[0].g, v[0].b, v[0].a,
                     v[1].x, v[1].y, (unsigned)v[1].z, v[1].r, v[1].g, v[1].b, v[1].a, nv > 2 ? v[2].x : 0.0f, nv > 2 ? v[2].y : 0.0f, nv > 2 ? (unsigned)v[2].z : 0u,
                     nv > 2 ? v[2].r : 0, nv > 2 ? v[2].g : 0, nv > 2 ? v[2].b : 0, nv > 2 ? v[2].a : 0);
        }
    }
    if (VITA_RARE(gVitaSliver) && type == 3) { /* a long thin GS triangle: an effect's line across the picture? */
        extern int Vita_InFight(void);
        static unsigned nGs;
        float e0 = sqrtf((v[1].x - v[0].x) * (v[1].x - v[0].x) + (v[1].y - v[0].y) * (v[1].y - v[0].y));
        float e1 = sqrtf((v[2].x - v[1].x) * (v[2].x - v[1].x) + (v[2].y - v[1].y) * (v[2].y - v[1].y));
        float e2 = sqrtf((v[0].x - v[2].x) * (v[0].x - v[2].x) + (v[0].y - v[2].y) * (v[0].y - v[2].y));
        float L = e0 > e1 ? (e0 > e2 ? e0 : e2) : (e1 > e2 ? e1 : e2);
        float ar = 0.5f * fabsf((v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y));
        static unsigned fightWas2;
        if (Vita_InFight() != (int)fightWas2) {
            fightWas2 = (unsigned)Vita_InFight();
            nGs = 0; /* (a cap for each fight) */
        }
        if (L > 200.0f && 2.0f * ar / L < 3.0f && nGs < 80 && Vita_InFight() && ((gGs.tex0[ctx] & 0x3FFF) != 0x2B18 || L > 700.0f)) {
            nGs++;
            Vita_Log("sliver: picture %u GS triangle ctx %d PRIM %03x TEX0 %08x%08x ALPHA %02x FRAME %03x: (%.0f,%.0f c %02x%02x%02x%02x) (%.0f,%.0f c %02x%02x%02x%02x) (%.0f,%.0f c %02x%02x%02x%02x)\n",
                     gGsFrame, ctx, (unsigned)(gGs.prim & 0x7FF), (unsigned)(gGs.tex0[ctx] >> 32), (unsigned)gGs.tex0[ctx], (unsigned)(gGs.alpha[ctx] & 0xFF),
                     (unsigned)(gGs.frame[ctx] & 0x1FF), v[0].x, v[0].y, v[0].r, v[0].g, v[0].b, v[0].a, v[1].x, v[1].y, v[1].r, v[1].g, v[1].b, v[1].a,
                     v[2].x, v[2].y, v[2].r, v[2].g, v[2].b, v[2].a);
        }
    }
    if (tbl_intercept(type, ctx)) {
        return; /* a pass about the depth page's spare byte, taken as a whole */
    }
    ZM(11);
    if (type == 0) {
        return; /* points: not used for anything visible so far */
    }
    ZM(13);
    if ((type == 3 || type == 6) &&
        ((sFL.ok && sFL.type == type && sFL.gen == gGsStateGen && sFL.frame == gGsFrame && sFL.ctx == ctx && (!sFL.d.tme || sFL.epoch == gGsEpoch)) ||
         (type == 3 && flm_take(ctx))) &&
        gVitaFastLane && !gVitaSliver && !sDumping && !sCovSample && sProf <= 0 && !(sT114Var & 1) && !(gVglSafe2 & 8192)) {
        /* the quick way (above) */
        d = sFL.d;
        sTexXf = 0;
        gVitaFl[0]++;
        ZM(47);
        if (!fl_book(&d, ctx, v, gouraud, t0, type)) {
            return;
        }
        goto page_draw;
    }
    sTexMayDefer = 1;
    sWpAllow = 1;
    i = state_of(ctx, type, &d);
    sWpAllow = 0;
    sTexMayDefer = 0;
    ZM(47); /* state_of on its own */
    sFL.ok = 0;
    if (i && (type == 3 || (type == 6 && gVitaFl6)) && !sTexXf && fl_state_ok(&d)) { /* the next triangle may take this state (226: a sprite, the next sprite) */
        sFL.ok = 1;
        sFL.type = type;
        sFL.gen = gGsStateGen;
        sFL.frame = gGsFrame;
        sFL.epoch = gGsEpoch;
        sFL.ctx = ctx;
        sFL.d = d;
        gVitaFl[1]++;
        if (type == 3) {
            flm_put(ctx, &d);
        }
    } else {
        gVitaFl[2]++;
        if (type == 3 || type == 6) { /* why not open to the next */
            int w = !i ? 0 : sTexXf ? 1 : d.date ? 2 : (d.blend && (d.src >= 4 || d.dst >= 4)) ? 3 : !(d.zwrite || (d.mask & 7) != 0) ? 4 : d.target != 0 ? 5 : 6;
            gVitaFlWhy[type == 6][w]++;
        }
    }
    if (i && d.target >= WT_BASE) { /* into a work target: drawn as it is */
        WorkTarget *t = &sWt[d.target - WT_BASE];
        if (t->last != gGsFrame) {
            t->cov = 0.0f;
        }
        if (type == 6 && (!d.blend || (d.src == 1 && d.dst == 0 && d.eq == 0)) && d.mask == 15 && t->stale) {
            /* (a sprite that replaces what is there: once they cover the scissor's area, the target holds what is drawn
               here again, after a pass into it was missed) */
            float bx0 = v[0].x < v[1].x ? v[0].x : v[1].x, bx1 = v[0].x < v[1].x ? v[1].x : v[0].x;
            float by0 = v[0].y < v[1].y ? v[0].y : v[1].y, by1 = v[0].y < v[1].y ? v[1].y : v[0].y;
            float sx0 = (float)d.sx, sx1 = (float)(d.sx + d.sw), sy0 = (float)d.sy, sy1 = sy0 + (float)d.sh; /* (rows as the GS's) */
            if (bx0 < sx0) { bx0 = sx0; } if (bx1 > sx1) { bx1 = sx1; } if (by0 < sy0) { by0 = sy0; } if (by1 > sy1) { by1 = sy1; }
            if (bx1 > bx0 && by1 > by0) {
                t->cov += (bx1 - bx0) * (by1 - by0);
            }
            if (t->cov >= 0.97f * (float)d.sw * (float)d.sh) {
                t->stale = 0;
            }
        }
        t->drawn = 1;
        t->last = gGsFrame;
        t->sig = wt_sig(t); /* (its memory as the uploads left it, now) */
        gVitaWt[0]++;
        goto page_draw;
    }
    if (i && d.target) { /* into the work page: drawn as it is (none of the picture's shortcuts) */
        gVitaWp[0]++;
        if (type == 6 && !d.tme && !d.blend && d.mask == 15) { /* a clear: the page whole again once it all is */
            float cx0 = v[0].x < 0.0f ? 0.0f : v[0].x, cx1 = v[1].x > 256.0f ? 256.0f : v[1].x;
            float cy0 = v[0].y < 0.0f ? 0.0f : v[0].y, cy1 = v[1].y > 256.0f ? 256.0f : v[1].y;
            if (cx1 > cx0 && cy1 > cy0) {
                sWpArea += (cx1 - cx0) * (cy1 - cy0);
            }
            if (sWpClrN > 0 && (memcmp(&sWpClrSt, &d, sizeof(d)) != 0 || memcmp(&sWpClrV.r, &v[1].r, 4) != 0 || sWpClrN == 16)) {
                wp_emit_clear(); /* (another clear held back: drawn first) */
            }
            sWpClrSt = d;
            sWpClrV = v[1];
            sWpClrZmax = zmax;
            sWpClr[sWpClrN][0] = v[0].x; sWpClr[sWpClrN][1] = v[0].y; sWpClr[sWpClrN][2] = v[1].x; sWpClr[sWpClrN][3] = v[1].y;
            sWpClrN++;
            if (sWpArea >= 256.0f * 256.0f * 0.98f) {
                sWpValid = 1;
                sWpArea = 0.0f;
                sWpClrWhole = 1;
                gVitaWp[2]++;
            }
            return; /* (held back: wp_emit_clear) */
        }
        wp_emit_clear();
        goto page_draw;
    }
    if (!i) {
        ZM(14);
        gVglDropped++;
        gVitaRej[0][sRejWhy & 7]++;
        sSigRej[sRejWhy & 7]++;
        if ((sRejWhy & 7) == 7 && sSigDefN < 6) {
            sSigDefT0[sSigDefN++] = t0;
        }
        if (VITA_RARE(sDumping)) {
            dump_state("GS", ctx, type, NULL, v[0].x, v[0].y, v[n - 1].x, v[n - 1].y, (unsigned)n);
        }
        return;
    }
    if (VITA_RARE(gVitaFilm > 0)) {   /* the signature: what the list gives, and the texture's contents */
        int k;
        SIG_MIX(sSigGs, type);
        SIG_MIX(sSigGs, (uint32_t)t0);
        SIG_MIX(sSigGs, (uint32_t)(t0 >> 32));
        SIG_MIX(sSigGs, (uint32_t)gGs.alpha[ctx]);
        SIG_MIX(sSigGs, (uint32_t)gGs.test[ctx]);
        SIG_MIX(sSigGsT, d.tme ? sSigGenCur : 0u);
        for (k = 0; k < n; k++) {
            uint32_t w[8];
            memcpy(&w[0], &v[k].x, 4); memcpy(&w[1], &v[k].y, 4); memcpy(&w[2], &v[k].s, 4); memcpy(&w[3], &v[k].t, 4);
            memcpy(&w[4], &v[k].q, 4);
            w[5] = (uint32_t)v[k].z;
            w[6] = (uint32_t)v[k].r | (uint32_t)v[k].g << 8 | (uint32_t)v[k].b << 16 | (uint32_t)v[k].a << 24;
            w[7] = (uint32_t)v[k].u | (uint32_t)v[k].v << 16;
            SIG_MIX(sSigGs, w[0]); SIG_MIX(sSigGs, w[1]); SIG_MIX(sSigGs, w[2]); SIG_MIX(sSigGs, w[3]);
            SIG_MIX(sSigGs, w[4]); SIG_MIX(sSigGs, w[5]); SIG_MIX(sSigGs, w[6]); SIG_MIX(sSigGs, w[7]);
        }
        sSigGsN++;
    }
    if ((d.mask & 8) && !d.fba && !(gVglSafe & 64) && (!d.tme || !((t0 >> 34) & 1))) {
        /* the alpha written is the vertices' own: bit 7 of it, if it is the same for all of them */
        int hi = 0;
        for (i = type == 6 || !gouraud ? n - 1 : 0; i < n; i++) {
            hi += v[i].a >= 0x80;
        }
        d.sop = hi == 0 ? 1 : (type == 6 || !gouraud ? hi == 1 : hi == n) ? 2 : bit7_mixed(&d);
    }
    {   /* A primitive that changes nothing is left out: one that writes no colour and no depth, or only the alpha
           when nothing has read the picture's alpha for a while (the screen shows no alpha; a picture used as a
           texture is not drawn by this back end); one blended with its own alpha, which its vertices give as 0 or 1
           (of 128: the GS's product rounds it away); one blended with the picture's alpha where that is 0 (the
           alpha map below). The game clears and sets the alpha in full-screen passes a few times a picture, for
           an effect that reads it (a darkening by the alpha) that is idle in a fight: 7 screens of filling. */
        int why = 0, k, amin = 255, amax = 0;
        int alphaDead = gGsFrame - sAlphaRead > 60u, reads = d.date != 0 || (d.blend && (d.src >= 4 || d.dst >= 4));
        float area = 0.0f, bx0 = v[0].x, by0 = v[0].y, bx1 = v[0].x, by1 = v[0].y;
        for (k = type == 6 || !gouraud ? n - 1 : 0; k < n; k++) {
            if (v[k].a < amin) { amin = v[k].a; }
            if (v[k].a > amax) { amax = v[k].a; }
        }
        for (k = 1; k < n; k++) {
            if (v[k].x < bx0) { bx0 = v[k].x; } if (v[k].x > bx1) { bx1 = v[k].x; }
            if (v[k].y < by0) { by0 = v[k].y; } if (v[k].y > by1) { by1 = v[k].y; }
        }
        {   /* (the GS's scissor) */
            uint64_t sc = gGs.scissor[ctx];
            float sx0 = (float)(sc & 0x7FF), sx1 = (float)((sc >> 16) & 0x7FF) + 1.0f, sy0 = (float)((sc >> 32) & 0x7FF), sy1 = (float)((sc >> 48) & 0x7FF) + 1.0f;
            if (bx0 < sx0) { bx0 = sx0; } if (by0 < sy0) { by0 = sy0; } if (bx1 > sx1) { bx1 = sx1; } if (by1 > sy1) { by1 = sy1; }
        }
        if (d.blend && (d.src >= 4 || d.dst >= 4)) {
            al_read();
        } else if (type == 6 && (d.mask & 8) && !d.atest && !d.date && (!d.ztest || d.zfunc == 1) && bx0 <= 0.5f && by0 <= 0.5f && bx1 >= 511.5f && by1 >= 447.5f) {
            sAlFull = 1; /* (all of the alpha written) */
        }
        if (!alphaDead && d.sop == 0 && al_dead()) { /* (not one that sets the stencil's copy of alpha bit 7: TEST.DATE reads that) */
            alphaDead = 2;
        }
        if (reads) {
            /* the blend with the picture's alpha 0 where all of it is drawn: no change to the colour? */
            int nil = 0;
            if (!d.date && am_zero(bx0, by0, bx1, by1)) {
                int sf = d.src == 0 || d.src == 4 ? 0 : d.src == 1 || d.src == 5 ? 1 : 2, df = d.dst == 0 || d.dst == 4 ? 0 : d.dst == 1 || d.dst == 5 ? 1 : 2;
                nil = sf == 0 && df == 1 && d.eq != 2;
            }
            if (!nil) {
                sAlphaRead = gGsFrame;
                gVglSkip[2]++;
            } else if (!d.zwrite && (!(d.mask & 8) || alphaDead)) {
                why = 3;
            }
        }
        if (why != 0) {
        } else if (!d.zwrite && (d.mask & 7) == 0 && (!(d.mask & 8) || alphaDead)) {
            why = 2;
        } else if (d.blend && d.eq == 0 && d.src == 2 && (d.dst == 3 || d.dst == 1) && !d.zwrite && (!(d.mask & 8) || alphaDead) && (!d.tme || d.env != 1) && amax <= 1) {
            why = 1;
        }
        if (d.mask & 8) { /* what the GS would leave in the picture's alpha */
            if (type == 6 && !d.tme && !d.fba && !d.atest && !d.date && (!d.ztest || d.zfunc == 1)) {
                am_write(bx0, by0, bx1, by1, v[1].a);
            } else {
                am_write(bx0, by0, bx1, by1, -1);
            }
        }
        if (type == 6 || type == 3) {
            area = prim_area(type, v);
            gVglCov[d.blend ? 1 : 0] += area;
            if (VITA_RARE(sCovSample)) {
                int k2;
                float qlo = 1e30f, qhi = 0.0f;
                for (k2 = 0; k2 < n; k2++) {
                    float qq = v[k2].q < 0.0f ? -v[k2].q : v[k2].q;
                    qlo = qq < qlo ? qq : qlo;
                    qhi = qq > qhi ? qq : qhi;
                }
                sCovQr = type == 3 && d.tme && !((prim >> 8) & 1) && qlo > 0.0f ? qhi / qlo : 0.0f;
                cov_add(&d, ctx, type, area, why != 0 ? area : 0.0f, amin, amax);
            }
        }
        if (why != 0 && !(gVglSafe & 2097152)) {
            gVglSkip[why == 1 ? 0 : 1]++;
            gVglSkipArea[why == 1 ? 0 : 1] += area;
            if (alphaDead == 2 && (d.mask & 8)) {
                gVitaAlDead[0]++;
                gVitaAlDead[1] += (unsigned)(area * (100.0f / (512.0f * 448.0f)));
            }
            return;
        }
        if (VITA_RARE(sProf > 0) && (sProf == 7 || (sProf == 1 && d.blend) || (sProf == 8 && d.blend && type == 3 && area > 0.25f * 512.0f * 448.0f) ||
                          (sProf == 9 && type == 6 && area > 0.9f * 512.0f * 448.0f) || (sProf == 10 && d.target != 0))) {
            return; /* (the GPU profile: these left out) */
        }
        agOk = (gVglSafe2 & 8192) && d.target == 0 /* (off unless asked for, safe2=8192: a test was slower with it, 19 -> 24 ms a picture) */ && d.blend && d.eq == 0 && d.dst == 1 && d.src < 4 && !d.zwrite && !d.date && d.sop == 0 && !d.fba &&
               d.pad_[0] == 0 && !sTexXf && (!(d.mask & 8) || alphaDead) && !sDumping && !sCovSample;
    }
    if (VITA_RARE(sT114Var & 1) && type == 3 && d.blend) {
        return; /* (no blended GS triangles, so that the fighters are seen) */
    }
page_draw:
    if (sVsNP != 0 && sNoProj && d.tme && type == 3 && !fst) {
        /* Q the same at the three corners: w the same, and the chip's interpolation with perspective is the GS's
           straight one. Only where it differs is the colour carried across the screen (a division per pixel). */
        float qa = v[0].q, qb = v[1].q, qc = v[2].q, qm = fabsf(qa);
        qm = fabsf(qb) > qm ? fabsf(qb) : qm;
        qm = fabsf(qc) > qm ? fabsf(qc) : qm;
        d.qlin = fabsf(qa - qb) > qm * 0.02f || fabsf(qa - qc) > qm * 0.02f; /* (2%: the two ways differ by under a part in 200) */
        if (d.qlin && gVitaQlinOpt && sProf != 2 && (!gouraud || (v[0].r == v[1].r && v[0].r == v[2].r && v[0].g == v[1].g && v[0].g == v[2].g &&
                                                                  v[0].b == v[1].b && v[0].b == v[2].b && v[0].a == v[1].a && v[0].a == v[2].a))) {
            d.qlin = 0; /* one colour at all three corners: carried with perspective or across the screen, the same */
        }
        if (d.qlin && VITA_RARE(sProf >= 0)) {
            sProfQlin += prim_area(type, v);
            if (sProf == 4) {
                return; /* (the GPU profile: these left out) */
            }
        }
    }
    if (VITA_RARE(sDumping)) {
        dump_state("GS", ctx, type, &d, v[0].x, v[0].y, v[n - 1].x, v[n - 1].y, (unsigned)n);
    }
    ZM(14);
    if (agOk) { /* into the additive group's batch of its state */
        int k;
        if (sAgN == 0) {
            flush(); /* (the batch before it: drawn first) */
        }
        for (k = 0; k < sAgN && memcmp(&sAgSt[k], &d, sizeof(d)) != 0; k++) {
        }
        if ((k < sAgN && (k == sAgCur ? sVertCount : sAgCnt[k]) + 6 > AG_VERTS) || (k == sAgN && sAgN == AG_SLOTS)) {
            ag_close(); /* (full: the group drawn, a new one begun) */
            k = 0;
        }
        if (k == sAgN) {
            sAgSt[k] = d;
            sAgCnt[k] = 0;
            sAgN++;
        } else if (k != sAgCur) {
            gVitaAg[2]++; /* (back to a state of the group: a draw and a change of state saved) */
        }
        if (k != sAgCur) {
            if (sAgCur >= 0) {
                sAgCnt[sAgCur] = sVertCount;
            }
            sAgCur = k;
            sVerts = sAgV[k];
            sVertCount = sAgCnt[k];
        }
        sCur = d;
        sHaveState = 1;
    } else {
        if (sAgN > 0) {
            ag_close();
        }
        if (!sHaveState || memcmp(&d, &sCur, sizeof(d)) != 0 || sVertCount + 6 > MAX_VERTS) {
            ZM(15);
            flush();
            ZM(30);
            sCur = d;
            sHaveState = 1;
        }
    }
    put_target(d.target);
    for (i = 0; i < n; i++) { /* nothing unreasonable to the chip: a vertex far outside the picture or without a usable Q */
        if (!(v[i].x > -8192.0f && v[i].x < 8192.0f && v[i].y > -8192.0f && v[i].y < 8192.0f) ||
            (!fst && (prim >> 4 & 1) && !(v[i].q > 1e-12f && v[i].q < 1e12f && v[i].s > -1e12f && v[i].s < 1e12f && v[i].t > -1e12f && v[i].t < 1e12f))) {
            sBadVerts++;
            return;
        }
    }
    gVglDraws++;
    for (i = 0; i < n; i++) {
        xs[i] = v[i].x;
    }
    if ((type == 6 || fst) && Port_IsWide() && !gPortMenuMode && !d.target && !sTexXf) { /* (an effect's strips of a
                                                     picture drawn here keep their place, as gs_gpu.c's tex_is_target) */
        /* Widescreen (as the PC back end, gs_gpu.c): the 3D scene is projected for the wide picture by the game;
           2D art is laid out for 4:3 and would come out a third too wide. In a fight each 2D piece is narrowed
           about a fixed point: the left or right edge or the middle for the HUD's panels (the markers), the
           middle of the screen for anything else. Left alone: full-screen fills and fades, and pieces as wide as
           the screen (bands, flashes, speed lines). In the menus everything is stretched as it is. */
        float x0 = xs[0], x1 = xs[0], y0 = v[0].y, y1 = v[0].y, pivot;
        for (i = 1; i < n; i++) {
            if (xs[i] < x0) { x0 = xs[i]; }
            if (xs[i] > x1) { x1 = xs[i]; }
            if (v[i].y < y0) { y0 = v[i].y; }
            if (v[i].y > y1) { y1 = v[i].y; }
        }
        if ((type == 6 && !d.tme && y1 - y0 >= 400.0f) || x1 - x0 >= 480.0f) {
            pivot = -1.0f;
        } else if (sAnchor == 4) {
            float cx = (x0 + x1) * 0.5f;
            pivot = x1 <= 300.0f || cx < 180.0f ? 0.0f : x0 >= 212.0f || cx > 332.0f ? 512.0f : 256.0f;
        } else {
            pivot = sAnchor == 1 ? 0.0f : sAnchor == 2 ? 512.0f : 256.0f;
        }
        if (pivot >= 0.0f) {
            float narrow = 1333.333f / (float)Port_AspectMilli();
            for (i = 0; i < n; i++) {
                xs[i] = pivot + (xs[i] - pivot) * narrow;
            }
        }
    }
    for (i = 0; i < n; i++) {
        if (fst) {
            s[i] = (float)v[i].u / 16.0f / tw;
            t[i] = (float)v[i].v / 16.0f / th;
            q[i] = 1.0f;
        } else if (!d.tme && sNoProj) {
            s[i] = t[i] = 0.0f;
            q[i] = 1.0f; /* (no texture: the colour across the screen as the GS has it, w = 1) */
        } else {
            s[i] = v[i].s;
            t[i] = v[i].t;
            q[i] = v[i].q != 0.0f ? v[i].q : 1.0f;
        }
    }
    if (type == 6) { /* sprite: two corners; colour, depth and Q of the second vertex */
        const GsVertex *a = &v[0], *b = &v[1];
        float s0 = fst ? s[0] : s[0] / q[1], s1 = fst ? s[1] : s[1] / q[1], t0s = fst ? t[0] : t[0] / q[1], t1 = fst ? t[1] : t[1] / q[1];
        if (sTexXf && xs[1] != xs[0] && b->y != a->y) {
            /* (as gs_gpu.c) the GS takes a texel at a whole pixel's position, the chip at its middle: half a
               pixel later. Between pictures drawn here that is not cosmetic: the glow's blur steps sample a texel apart
               on purpose and would move the picture by a texel a step instead of blurring it */
            float ds = 0.5f * (s1 - s0) / (xs[1] - xs[0]), dt = 0.5f * (t1 - t0s) / (b->y - a->y);
            s0 -= ds; s1 -= ds; t0s -= dt; t1 -= dt;
        } else if (gVitaSprInset && d.tme && d.linear && !d.pad_[0] && !sTexXf) {
            /* A filtered sprite of an uploaded picture: the GS takes the texels at its pixels x0 .. x1 - 1
               (the corner at x1 is not drawn), so the farthest texel it reads is one pixel's worth short of the far
               corner's coordinate. The chip's pixels fall between the GS's (640 x 368, the menus' 960 x 544) and
               near the far edge it read up to the corner's coordinate itself: the filter took in the texels next to
               the piece in its sheet (a faint line under a HUD bar whose own texels are clear). The far corner's
               coordinates are given as the GS's last pixel has them. */
            spr_far_edge(a->x, b->x, &s0, &s1);
            spr_far_edge(a->y, b->y, &t0s, &t1);
        }
        {   /* two corners worked out, the other two taken from them: put's x and S depend on x and S alone,
               its y and T on y and T, the rest on the second vertex (Q 1 at all four), so the six are the six put
               would make */
            V *o = &sVerts[sVertCount];
            put_i(b, xs[0], a->y, s0, t0s, 1.0f, b, zmax);
            put_i(b, xs[1], b->y, s1, t1, 1.0f, b, zmax);
            o[4] = o[1];
            o[1] = o[0];
            o[1].x = o[4].x;
            o[1].s = o[4].s;
            o[2] = o[0];
            o[2].y = o[4].y;
            o[2].t = o[4].t;
            o[3] = o[1];
            o[5] = o[2];
            sVertCount += 4;
        }
    } else {
        if (n == 3 && fst && gVitaSprInset && d.tme && d.linear && !d.pad_[0] && !sTexXf) {
            tri_far_edge(v, s, t); /* a 2D piece drawn as triangles (the HUD) */
        }
        sQn = 1.0f;
        if (d.qlin && gVitaQlinOpt && sProf != 2) {
            float qmin = 0.0f;
            for (i = 0; i < n; i++) {
                if (q[i] > 0.0f && (qmin == 0.0f || q[i] < qmin)) {
                    qmin = q[i];
                }
            }
            sQn = qmin > 0.0f && qmin < 1000.0f ? qmin : 1.0f;
        }
        for (i = 0; i < n; i++) {
            /* S, T arrive already divided by the vertex's w (the GS divides by Q per pixel): s / q is the plain
               coordinate, and put() multiplies it by w = 1 / q again for the chip's own perspective division */
            put(&v[i], xs[i], v[i].y, s[i] / q[i], t[i] / q[i], q[i], gouraud ? &v[i] : flat, zmax);
        }
    }
    ZM(18); /* the vertices put in the batch */
}

/* A run of GS triangles, one after another with nothing written between them (gs_core.c tri_run: the
   effects' triangle packets with the same PRIM and TEX0, the sea of Kame House): each the quick way when the state of
   the one before is its own (GsGpu_Draw_impl's above, without its checks for what a run cannot hold), else as any GS
   primitive. */
static int sTblRun; /* (below: tbl_intercept's) */
uint64_t gVitaTriCyc[7]; /* (the log: lists measured part by part) a run's quick triangle's cycles: checks, bookkeeping, Q, batch, checks, vertices; count */
#define TC(i) do { if (tcOn) { uint32_t now_; scePerfArmPmonGetCounterValue(0, 31, &now_); gVitaTriCyc[i] += now_ - tcLast; tcLast = now_; } } while (0)
/* the run's "not known" alpha cells (am_write's -1), gathered by row and laid at once: the same cells as
   the triangles' own am_write calls, before anything that reads them (a triangle the full way, a batch drawn) */
static uint16_t sRunAm[14];
static int sRunAmN;
static void run_am_mark(float x0, float y0, float x1, float y1) {
    int cx0, cy0, cx1, cy1, cy;
    uint16_t m;
    if (!(x1 > x0 && y1 > y0)) {
        return;
    }
    x0 = x0 < 0.0f ? 0.0f : x0; y0 = y0 < 0.0f ? 0.0f : y0; x1 = x1 > 512.0f ? 512.0f : x1; y1 = y1 > 448.0f ? 448.0f : y1;
    if (!(x1 > x0 && y1 > y0)) {
        return;
    }
    cx0 = (int)x0 >> 5; cy0 = (int)y0 >> 5; cx1 = ((int)x1 - 1) >> 5; cy1 = ((int)y1 - 1) >> 5;
    if (cx1 > 15) { cx1 = 15; } if (cy1 > 13) { cy1 = 13; }
    if (cx1 < cx0) {
        return;
    }
    m = (uint16_t)((2u << cx1) - (1u << cx0));
    for (cy = cy0; cy <= cy1; cy++) {
        sRunAm[cy] |= m;
    }
    sRunAmN = 1;
}
static void run_am_lay(void) {
    int cy, cx;
    if (!sRunAmN) {
        return;
    }
    for (cy = 0; cy < 14; cy++) {
        uint16_t m = sRunAm[cy];
        sRunAm[cy] = 0;
        for (cx = 0; m != 0; cx++, m >>= 1) {
            if (m & 1) {
                sAm[cy][cx] = -1;
            }
        }
    }
    sRunAmN = 0;
}
int gVitaRunEst = 0; /* (tests: bt3.txt runest=1; off: no gain measured, a Kame House fight 15,335 k cycles a list against 15,347 k) */
unsigned gVitaRunEstN; /* (the log) runs' first triangles whose state was worked out here */
void GsGpu_DrawTriRun(int ctx, const GsVertex *v, uint32_t ntri) {
    uint32_t k, tcLast = 0;
    int tcOn = (gVitaZoneOn & 1) != 0;
    int lean = 0, gouraud = 0, sopOn = 0, amOn = 0, skipOn = 0, qlinOn = 0, ztype = 0;
    int inBatch = 0, lastSop = -1, lastQlin = -1; /* the triangle before went into the batch drawn now with these */
    float sx0 = 0.0f, sx1 = 0.0f, sy0 = 0.0f, sy1 = 0.0f, zmax = 0.0f;
    uint64_t prim = 0, t0 = 0;
    State d0;
    P_IN(9);
    memset(&d0, 0, sizeof(d0));
    for (k = 0; k < ntri; k++, v += 3) {
        State d;
        float s[3], t[3], q[3], bx0, by0, bx1, by1, area;
        int i;
        if (tcOn) {
            scePerfArmPmonGetCounterValue(0, 31, &tcLast);
        }
        gVglCalls[4]++;
        /* whether the quick way holds is looked at for the run's first triangle and after one drawn the full
           way only: nothing between two quick triangles changes what it depends on (no register is written, no GS
           memory, and only the full way sets the quick way's state or the strips' and parts' counts) */
        if (!lean) {
            prim = gGs.prim;
            t0 = gGs.tex0[ctx];
            if (!((prim >> 8) & 1) && (prim & 7) == 3 && !(sFL.ok && sFL.type == 3 && sFL.gen == gGsStateGen && sFL.frame == gGsFrame && sFL.ctx == ctx &&
                                                          (!sFL.d.tme || sFL.epoch == gGsEpoch)) && !flm_take(ctx) /* */ &&
                gVitaRunEst && gVitaFastLane && !gVitaSliver && !sDumping && !sCovSample && sProf <= 0 && !(sT114Var & 1) && !(gVglSafe2 & 8192) && !gDpCapture &&
                sBinCount == 0 && sDpItemN == 0 && !sGlowSkip && !sOlTail) {
                /* The run's first triangle had its state worked out by drawing it the full way (GsGpu_Draw_impl,
                   ~6,000 cycles): the sea of Kame House's ~240 runs a picture alternate between three textures and other
                   registers change between them. Its state is worked out here as the full way does (state_of, the same
                   flags), and when the quick way can take it, the triangle goes the quick way: what the full way would
                   draw for such a state. */
                State de;
                int okE;
                sTexMayDefer = 1;
                sWpAllow = 1;
                okE = state_of(ctx, 3, &de);
                sWpAllow = 0;
                sTexMayDefer = 0;
                sFL.ok = 0;
                if (okE && !sTexXf && fl_state_ok(&de)) {
                    sFL.ok = 1;
                    sFL.type = 3;
                    sFL.gen = gGsStateGen;
                    sFL.frame = gGsFrame;
                    sFL.epoch = gGsEpoch;
                    sFL.ctx = ctx;
                    sFL.d = de;
                    gVitaFl[1]++;
                    gVitaRunEstN++;
                    flm_put(ctx, &de);
                }
            }
        }
        if (!lean && (((prim >> 8) & 1) || (prim & 7) != 3 || !sFL.ok || sFL.type != 3 || sFL.gen != gGsStateGen || sFL.frame != gGsFrame || sFL.ctx != ctx ||
            (sFL.d.tme && sFL.epoch != gGsEpoch) || !gVitaFastLane || gVitaSliver || sDumping || sCovSample || sProf > 0 || (sT114Var & 1) ||
            (gVglSafe2 & 8192) || gDpCapture || sBinCount != 0 || sDpItemN != 0 || sGlowSkip || sOlTail)) {
            run_am_lay();
            GsGpu_Draw_impl(3, ctx, v);
            lean = 0;
            inBatch = 0;
            continue;
        }
        if (!lean) { /* the run's state, as the full way left it: what is the same for all of its triangles */
            uint64_t sc = gGs.scissor[ctx];
            d0 = sFL.d;
            gouraud = (prim >> 3) & 1;
            sopOn = (d0.mask & 8) && !d0.fba && !(gVglSafe & 64) && (!d0.tme || !((t0 >> 34) & 1));
            amOn = (d0.mask & 8) != 0;
            skipOn = d0.blend && d0.eq == 0 && d0.src == 2 && (d0.dst == 3 || d0.dst == 1) && !d0.zwrite && (!d0.tme || d0.env != 1) && !(gVglSafe & 2097152);
            qlinOn = sVsNP != 0 && sNoProj && d0.tme;
            sx0 = (float)(sc & 0x7FF); sx1 = (float)((sc >> 16) & 0x7FF) + 1.0f; sy0 = (float)((sc >> 32) & 0x7FF); sy1 = (float)((sc >> 48) & 0x7FF) + 1.0f;
            ztype = (int)((gGs.zbuf[ctx] >> 24) & 15);
            zmax = ztype == 0 ? 4294967295.0f : ztype == 1 ? 16777215.0f : 65535.0f;
            lean = 1;
        }
        sTblRun = 0; /* (as tbl_intercept does for any GS primitive but a sprite) */
        d = d0;
        sTexXf = 0;
        gVitaFl[0]++;
        TC(0);
        /* the bookkeeping (fl_book) */
        if (VITA_RARE(gVitaFilm > 0)) {
            fl_sig(&d, v, t0, ctx, 3);
        }
        if (sopOn) {
            int hi = 0;
            for (i = gouraud ? 0 : 2; i < 3; i++) {
                hi += v[i].a >= 0x80;
            }
            d.sop = hi == 0 ? 1 : (!gouraud ? hi == 1 : hi == 3) ? 2 : 0;
        }
        bx0 = bx1 = v[0].x;
        by0 = by1 = v[0].y;
        for (i = 1; i < 3; i++) {
            if (v[i].x < bx0) { bx0 = v[i].x; } if (v[i].x > bx1) { bx1 = v[i].x; }
            if (v[i].y < by0) { by0 = v[i].y; } if (v[i].y > by1) { by1 = v[i].y; }
        }
        area = prim_area(3, v); /* (the box before the scissor, as prim_area has it) */
        if (bx0 < sx0) { bx0 = sx0; } if (by0 < sy0) { by0 = sy0; } if (bx1 > sx1) { bx1 = sx1; } if (by1 > sy1) { by1 = sy1; }
        if (amOn) {
            run_am_mark(bx0, by0, bx1, by1);
        }
        gVglCov[d.blend ? 1 : 0] += area;
        if (skipOn) {
            int amax = 0, alphaDead = gGsFrame - sAlphaRead > 60u;
            for (i = gouraud ? 0 : 2; i < 3; i++) {
                if (v[i].a > amax) { amax = v[i].a; }
            }
            if (amax <= 1) {
                if (!alphaDead && d.sop == 0 && al_dead()) {
                    alphaDead = 2;
                }
                if (!(d.mask & 8) || alphaDead) {
                    gVglSkip[0]++;
                    gVglSkipArea[0] += area;
                    if (alphaDead == 2 && (d.mask & 8)) {
                        gVitaAlDead[0]++;
                        gVitaAlDead[1] += (unsigned)(area * (100.0f / (512.0f * 448.0f)));
                    }
                    continue;
                }
            }
        }
        TC(1);
        if (qlinOn) { /* (as GsGpu_Draw_impl: page_draw) */
            float qa = v[0].q, qb = v[1].q, qc = v[2].q, qm = fabsf(qa);
            qm = fabsf(qb) > qm ? fabsf(qb) : qm;
            qm = fabsf(qc) > qm ? fabsf(qc) : qm;
            d.qlin = fabsf(qa - qb) > qm * 0.02f || fabsf(qa - qc) > qm * 0.02f;
            if (d.qlin && gVitaQlinOpt && sProf != 2 && (!gouraud || (v[0].r == v[1].r && v[0].r == v[2].r && v[0].g == v[1].g && v[0].g == v[2].g &&
                                                                      v[0].b == v[1].b && v[0].b == v[2].b && v[0].a == v[1].a && v[0].a == v[2].a))) {
                d.qlin = 0;
            }
            if (d.qlin && VITA_RARE(sProf >= 0)) {
                sProfQlin += prim_area(3, v);
            }
        }
        TC(2);
        if (sAgN > 0) {
            run_am_lay();
            ag_close();
            inBatch = 0;
        }
        if (inBatch && d.sop == lastSop && d.qlin == lastQlin && sHaveState && sVertCount + 6 <= MAX_VERTS) {
            /* the batch of the triangle before: d is that one's state but for these two, compared */
        } else if (!sHaveState || memcmp(&d, &sCur, sizeof(d)) != 0 || sVertCount + 6 > MAX_VERTS) {
            run_am_lay();
            flush();
            sCur = d;
            sHaveState = 1;
        }
        inBatch = 1;
        lastSop = d.sop;
        lastQlin = d.qlin;
        put_target(d.target);
        TC(3);
        for (i = 0; i < 3; i++) { /* (as GsGpu_Draw_impl) */
            if (!(v[i].x > -8192.0f && v[i].x < 8192.0f && v[i].y > -8192.0f && v[i].y < 8192.0f) ||
                ((prim >> 4 & 1) && !(v[i].q > 1e-12f && v[i].q < 1e12f && v[i].s > -1e12f && v[i].s < 1e12f && v[i].t > -1e12f && v[i].t < 1e12f))) {
                break;
            }
        }
        if (i < 3) {
            sBadVerts++;
            continue;
        }
        gVglDraws++;
        for (i = 0; i < 3; i++) {
            if (!d.tme && sNoProj) {
                s[i] = t[i] = 0.0f;
                q[i] = 1.0f;
            } else {
                s[i] = v[i].s;
                t[i] = v[i].t;
                q[i] = v[i].q != 0.0f ? v[i].q : 1.0f;
            }
        }
        sQn = 1.0f;
        if (d.qlin && gVitaQlinOpt && sProf != 2) {
            float qmin = 0.0f;
            for (i = 0; i < 3; i++) {
                if (q[i] > 0.0f && (qmin == 0.0f || q[i] < qmin)) {
                    qmin = q[i];
                }
            }
            sQn = qmin > 0.0f && qmin < 1000.0f ? qmin : 1.0f;
        }
        TC(4);
        for (i = 0; i < 3; i++) { /* (S and T times 1 / Q, the last bit aside the division's) */
            float rq = 1.0f / q[i];
            put_i(&v[i], v[i].x, v[i].y, s[i] * rq, t[i] * rq, q[i], gouraud ? &v[i] : &v[2], zmax);
        }
        TC(5);
        if (tcOn) {
            gVitaTriCyc[6]++;
        }
    }
    run_am_lay();
    P_OUT;
}
#undef TC

/* A picture of the screen in the log (a third of its size, base64 rows, a few rows per drawn picture so the log's
   buffer keeps up): what a test build shows can be looked at from its log. */
static uint8_t sShot[320 * 184 * 3];
static unsigned sShotDue;
extern void *gxm_color_surfaces_addr[];
extern unsigned int gxm_back_buffer_index;
static int sShotW, sShotH, sShotRow = -1;
static unsigned sShotPic;
static void shot_take(unsigned pic) {
    SceDisplayFrameBuf fb;
    int x, y;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    if (sShotRow >= 0 || sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0 || fb.base == NULL || fb.pitch < fb.width) {
        return;
    }
    sShotW = (int)fb.width / 3;
    sShotH = (int)fb.height / 3;
    if (sShotW > 214) { sShotW = 214; }
    if (sShotH > 124) { sShotH = 124; }
    for (y = 0; y < sShotH; y++) {
        for (x = 0; x < sShotW; x++) {
            const uint8_t *p = (const uint8_t *)fb.base + ((size_t)(y * 3 + 1) * fb.pitch + (size_t)(x * 3 + 1)) * 4;
            uint8_t *o = &sShot[(y * sShotW + x) * 3];
            o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
        }
    }
    sShotRow = 0;
    sShotPic = pic;
    Vita_Log("shot: %u begin %d %d\n", pic, sShotW, sShotH);
}
/* the picture just drawn, half its size, before the swap: the same picture of the game in every run */
static void shot_take_exact(unsigned pic) {
    const uint8_t *fb;
    int x, y;
    if (sShotRow >= 0) {
        return;
    }
    rt_sync();
    glFinish();
    fb = (const uint8_t *)gxm_color_surfaces_addr[gxm_back_buffer_index];
    if (fb == NULL) {
        return;
    }
    sShotW = SCR_W / 2;
    sShotH = SCR_H / 2;
    for (y = 0; y < sShotH; y++) {
        for (x = 0; x < sShotW; x++) {
            const uint8_t *p = fb + ((size_t)(y * 2) * ((SCR_W + 63) & ~63) + (size_t)(x * 2)) * 4;
            uint8_t *o = &sShot[(y * sShotW + x) * 3];
            o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
        }
    }
    sShotRow = 0;
    sShotPic = pic;
    Vita_Log("shot: %u begin %d %d\n", pic, sShotW, sShotH);
}
static void shot_emit(void) {
    static const char k64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int rows;
    for (rows = 0; rows < 6 && sShotRow >= 0 && sShotRow < sShotH; rows++, sShotRow++) {
        int x0;
        for (x0 = 0; x0 < sShotW; x0 += 75) {
            char line[320];
            const uint8_t *p = &sShot[(sShotRow * sShotW + x0) * 3];
            int n = (sShotW - x0 < 75 ? sShotW - x0 : 75) * 3, i, o = 0;
            for (i = 0; i < n; i += 3) {
                uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)(i + 1 < n ? p[i + 1] : 0) << 8 | (i + 2 < n ? p[i + 2] : 0);
                line[o++] = k64[v >> 18]; line[o++] = k64[(v >> 12) & 63]; line[o++] = k64[(v >> 6) & 63]; line[o++] = k64[v & 63];
            }
            line[o] = 0;
            Vita_Log("shot: %u %d %d %s\n", sShotPic, sShotRow, x0, line);
        }
    }
    if (sShotRow >= sShotH && sShotRow >= 0) {
        Vita_Log("shot: %u end\n", sShotPic);
        sShotRow = -1;
    }
}

/* ---- what makes parts of the fighters go missing. Chosen moments of the fight, counted in the game's
   lists (drawn or not, so the same moments in every run), are pictured as drawn; two lists later the same moment is
   drawn with one thing changed (sT114Var) and pictured again, and so on. The pictures wait in memory and go to the
   log packed (zlib), a few rows a picture. The fighters' draw states of each moment's first picture go to the log
   too. ---- */
/* four lists in a row at each moment: as drawn now (the colour across the screen), the colour with
   perspective as in an earlier version, no blended GS triangles, as drawn now again */
static const unsigned kT114[] = {118, 348, 548, 698, 898, 1048, 1198};
static const int kT115Var[4] = {0, 1, 2, 0};
static unsigned sFF;          /* the fight's lists so far */
static unsigned sT114Shot;    /* a picture due for the list being drawn (moment * 10 + change), 0: none */
static int sT114DsKeep = -1;  /* (change 3: sDsOn as it was) */
typedef struct { unsigned id, list, w, h; uint8_t *px; } Shot114;
static Shot114 sSq[64];
static int sSqN;
static z_stream sZs;
static int sZsOn;
static unsigned sZsRow, sZsSeq;
static void t114_shot(unsigned id, unsigned list) {
    const uint8_t *fb;
    uint8_t *o;
    int full = id >= 9000; /* (the pictures of the white shards whole, every pixel) */
    int x, y, w = full ? SCR_W : SCR_W / 2, h = full ? SCR_H : SCR_H / 2, st = full ? 1 : 2;
    if (sSqN == (full ? 8 : 64) || (o = (uint8_t *)malloc((size_t)w * h * 3)) == NULL) {
        Vita_Log("t114: picture %u (list %u) not kept\n", id, list);
        return;
    }
    rt_sync();
    glFinish();
    fb = (const uint8_t *)gxm_color_surfaces_addr[gxm_back_buffer_index];
    if (fb == NULL) {
        free(o);
        return;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint8_t *p = fb + ((size_t)(y * st) * ((SCR_W + 63) & ~63) + (size_t)(x * st)) * 4;
            uint8_t *q = &o[(y * w + x) * 3];
            if (full) { /* (5, 6, 5 bits: packs to half) */
                q[0] = p[0] & 0xF8; q[1] = p[1] & 0xFC; q[2] = p[2] & 0xF8;
            } else {
                q[0] = p[0]; q[1] = p[1]; q[2] = p[2];
            }
        }
    }
    sSq[sSqN].id = id;
    sSq[sSqN].list = list;
    sSq[sSqN].w = (unsigned)w;
    sSq[sSqN].h = (unsigned)h;
    sSq[sSqN].px = o;
    sSqN++;
}
/* a work target's picture for the log, beside it its alpha as grey (the black blocks of
   the Bardock fight: which target holds them, and with what alpha) */
int gVitaWtShot;
static void t114_shot_rgba(unsigned id, unsigned list, const uint8_t *src, int w, int h, int stride, int alpha2x) {
    uint8_t *o;
    int x, y;
    if (src == NULL || w <= 0 || h <= 0 || sSqN >= 60 || (o = (uint8_t *)malloc((size_t)w * 2 * h * 3)) == NULL) {
        return;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint8_t *p = src + ((size_t)y * (size_t)stride + (size_t)x) * 4;
            uint8_t *q = &o[(y * w * 2 + x) * 3], *r = &o[(y * w * 2 + w + x) * 3];
            int a = alpha2x ? (p[3] * 2 > 255 ? 255 : p[3] * 2) : p[3];
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2];
            r[0] = r[1] = r[2] = (uint8_t)a;
        }
    }
    sSq[sSqN].id = id;
    sSq[sSqN].list = list;
    sSq[sSqN].w = (unsigned)w * 2;
    sSq[sSqN].h = (unsigned)h;
    sSq[sSqN].px = o;
    sSqN++;
}
/* pictures of the picture in the middle of a filmed frame: before and after each glow,
   before the outline (the black blocks of the Bardock fight: which pass makes them). Taken by the thread that draws,
   where the picture's scene has just ended; handed to the log's queue at the frame's end. */
static uint8_t *sMidPx[10];
static unsigned sMidId[10];
static volatile int sMidN, sMidOn;
static unsigned sMidBase, sMidK;
static unsigned sFilmPics, sFilmN;
static void mid_shot(int k) {
    const uint8_t *fb;
    uint8_t *o;
    int x, y, w = SCR_W / 2, h = SCR_H / 2;
    if (!sMidOn || sMidN >= 10 || (o = (uint8_t *)malloc((size_t)w * 2 * h * 3)) == NULL) {
        return;
    }
    glFinish();
    fb = (const uint8_t *)gxm_color_surfaces_addr[gxm_back_buffer_index];
    if (fb == NULL) {
        free(o);
        return;
    }
    for (y = 0; y < h; y++) { /* (the picture, and beside it its alpha, 0x80 as white) */
        for (x = 0; x < w; x++) {
            const uint8_t *q = fb + ((size_t)(y * 2) * ((SCR_W + 63) & ~63) + (size_t)(x * 2)) * 4;
            uint8_t *r = &o[(y * w * 2 + x) * 3], *t = &o[(y * w * 2 + w + x) * 3];
            r[0] = q[0]; r[1] = q[1]; r[2] = q[2];
            t[0] = t[1] = t[2] = (uint8_t)(q[3] * 2 > 255 ? 255 : q[3] * 2);
        }
    }
    sMidPx[sMidN] = o;
    sMidId[sMidN] = sMidBase + (unsigned)k * 100u + sMidK++;
    sMidN++;
}
unsigned gVitaFilmPicsNext(void) {
    if (sFilmN >= 12) {
        return 1; /* (only the first twelve) */
    }
    sMidBase = 3000 + sFilmN * 10;
    return sFilmPics + 1;
}
static void mid_flush(unsigned list) { /* (the drawing thread, the render thread idle) */
    int k;
    for (k = 0; k < sMidN; k++) {
        if (sSqN < 60) {
            sSq[sSqN].id = sMidId[k];
            sSq[sSqN].list = list;
            sSq[sSqN].w = SCR_W;
            sSq[sSqN].h = SCR_H / 2;
            sSq[sSqN].px = sMidPx[k];
            sSqN++;
        } else {
            free(sMidPx[k]);
        }
    }
    sMidN = 0;
    sMidK = 0;
}
/* the work targets and the picture's alpha into the log (after t114_shot: the chip has finished) */
static void wt_shots(unsigned base, unsigned list) {
    int k;
    const uint8_t *fb = (const uint8_t *)gxm_color_surfaces_addr[gxm_back_buffer_index];
    if (fb != NULL) { /* the picture's alpha (at half size, as the film): 0x80 is 1.0 */
        uint8_t *o = (uint8_t *)malloc((size_t)(SCR_W / 2) * (SCR_H / 2) * 4);
        if (o != NULL) {
            int x, y;
            for (y = 0; y < SCR_H / 2; y++) {
                for (x = 0; x < SCR_W / 2; x++) {
                    memcpy(&o[(y * (SCR_W / 2) + x) * 4], fb + ((size_t)(y * 2) * ((SCR_W + 63) & ~63) + (size_t)(x * 2)) * 4, 4);
                }
            }
            t114_shot_rgba(base, list, o, SCR_W / 2, SCR_H / 2, SCR_W / 2, 1);
            free(o);
        }
    }
    for (k = 0; k < sWtN; k++) {
        const uint8_t *d;
        glBindTexture(GL_TEXTURE_2D, sWt[k].tex);
        d = (const uint8_t *)vglGetTexDataPointer(GL_TEXTURE_2D);
        Vita_Log("wtshot: %u target %d page %03x %d x %d drawn %d stale %d\n", base + 1 + k, k, (unsigned)sWt[k].fbp, sWt[k].w, sWt[k].h, sWt[k].drawn, sWt[k].stale);
        t114_shot_rgba(base + 1 + k, list, d, sWt[k].w, sWt[k].h, sWt[k].w, 1);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    GLC_RESET();
}

/* ---- The fighters' outline (src/sys/gfxm_a.c ObjOutline_Draw). Every model writes a number of its own
   into the picture's alpha (the stage and the sky are cleared to 0xFF, "nothing"); the PS2 makes the edges of that
   image (a table of the numbers, the same image a pixel over subtracted) and darkens the picture by 0x64 there,
   through passes over the depth buffer's memory and a 16-bit view of the work page that are not drawn here. Here,
   at the game's marker (as the PC renderer, outline.frag): the picture so far goes to memory (the scene is split),
   one full-screen pass reads its alpha straight from the display buffer, finds the edges (here against its four
   neighbours) and subtracts the 0x64; the scene is split again after it, so nothing drawn later changes the alpha
   another part of the pass is still reading. ---- */
/* the five places read are worked out per vertex: the chip fetches a texture read straight from an
   interpolated coordinate before the pixel shader runs, and one worked out in the shader waits for it (156: the
   chip's time per picture up by several ms with the pass) */
static const char kVertOl[] =
    "uniform float4 uV;\n" /* x: the view's left edge, y: its width (in the display buffer's widths); z, w: a pixel */
    "void main(float2 aPos, float4 out vPos : POSITION, float2 out t0 : TEXCOORD0, float2 out t1 : TEXCOORD1,\n"
    "          float2 out t2 : TEXCOORD2, float2 out t3 : TEXCOORD3, float2 out t4 : TEXCOORD4) {\n"
    "    float2 c = float2(uV.x + (aPos.x * 0.5f + 0.5f) * uV.y, 0.5f - aPos.y * 0.5f);\n"
    "    vPos = float4(aPos, 0.0f, 1.0f);\n"
    "    t0 = c;\n"
    "    t1 = c + float2(uV.z, 0.0f);\n"
    "    t2 = c - float2(uV.z, 0.0f);\n"
    "    t3 = c + float2(0.0f, uV.w);\n"
    "    t4 = c - float2(0.0f, uV.w);\n"
    "}\n";
static const char kFragOl[] =
    "uniform sampler2D uTex;\n"
    "uniform float4 uP;\n" /* x: the darkening (0x64) */
    "float tb(float a) {\n" /* ObjOutline_BuildClut: (n << 3) & 0xFF, 0 as 0x80; "nothing" 0 */
    "    float g = floor(a * 128.0f + 0.5f);\n" /* (the GS's alpha: 0x80 is 1.0 here) */
    "    float c = frac(g * 0.03125f) * 256.0f;\n"
    "    return a > 0.998f ? 0.0f : (c < 0.5f ? 128.0f : c);\n"
    "}\n"
    "float4 main(float2 t0 : TEXCOORD0, float2 t1 : TEXCOORD1, float2 t2 : TEXCOORD2, float2 t3 : TEXCOORD3, float2 t4 : TEXCOORD4) : COLOR {\n"
    "    float h = tb(tex2D(uTex, t0).a);\n"
    "    float r = tb(tex2D(uTex, t1).a);\n"
    "    float l = tb(tex2D(uTex, t2).a);\n"
    "    float d = tb(tex2D(uTex, t3).a);\n"
    "    float u = tb(tex2D(uTex, t4).a);\n"
    "    float e = (h > r || h > l || h > d || h > u) ? uP.x : 0.0f;\n"
    "    return float4(e, e, e, 0.0f);\n"
    "}\n";
int gVitaFilm;              /* bt3.txt film=<n> */
static GLuint sOlProg, sOlTex[3];
static GLint sOlU[3];        /* uV, uP, uTex */
int gVitaOlOff;              /* the outline not drawn for now (the log's pictures without it) */
static int sOlShotWant, sOlSplitDone;
unsigned gVitaOl[4];         /* for the log: outlines drawn, not drawn (the scene not split), pictures of the alpha */
static void ol_init(void) {
    GLuint vs, fs;
    GLint len, ok = 0;
    const char *src;
    char log[300];
    extern int gVglSafe2;
    if (gVglSafe2 & 64) {
        Vita_Log("vgl: outline off (safe2=64)\n");
        return;
    }
    vs = glCreateShader(GL_CG_VERTEX_SHADER_EXT);
    src = kVertOl; len = (GLint)sizeof(kVertOl) - 1;
    glShaderSource(vs, 1, &src, &len);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        log[0] = 0; glGetShaderInfoLog(vs, sizeof(log) - 1, NULL, log); log[sizeof(log) - 1] = 0;
        Vita_Log("vgl: outline vertex shader not compiled: %s\n", log);
        return;
    }
    fs = glCreateShader(GL_CG_FRAGMENT_SHADER_EXT);
    src = kFragOl; len = (GLint)sizeof(kFragOl) - 1; ok = 0;
    glShaderSource(fs, 1, &src, &len);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        log[0] = 0; glGetShaderInfoLog(fs, sizeof(log) - 1, NULL, log); log[sizeof(log) - 1] = 0;
        Vita_Log("vgl: outline pixel shader not compiled: %s\n", log);
        return;
    }
    sOlProg = glCreateProgram();
    glAttachShader(sOlProg, vs);
    glAttachShader(sOlProg, fs);
    glBindAttribLocation(sOlProg, 0, "aPos");
    glLinkProgram(sOlProg);
    sOlU[0] = glGetUniformLocation(sOlProg, "uV");
    sOlU[1] = glGetUniformLocation(sOlProg, "uP");
    sOlU[2] = glGetUniformLocation(sOlProg, "uTex");
    Vita_Log("vgl: outline shader built (uniforms %d %d %d)\n", (int)sOlU[0], (int)sOlU[1], (int)sOlU[2]);
    if (sOlU[0] < 0 || sOlU[1] < 0) {
        sOlProg = 0;
    }
}
/* the display buffer being drawn, as a texture (made once per buffer: its memory is the buffer's, never let go) */
static GLuint ol_view(void) {
    unsigned i = gxm_back_buffer_index;
    if (i > 2 || gxm_color_surfaces_addr[i] == NULL) {
        return 0;
    }
    if (sOlTex[i] == 0) {
        glGenTextures(1, &sOlTex[i]);
        glBindTexture(GL_TEXTURE_2D, sOlTex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        vglSetupTexture(GL_TEXTURE_2D, gxm_color_surfaces_addr[i], SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, SCE_GXM_TEXTURE_LINEAR, SCR_W, SCR_H, 1);
        Vita_Log("vgl: outline: display buffer %u as a texture (%u)\n", i, (unsigned)sOlTex[i]);
    }
    return sOlTex[i];
}
/* (the thread that draws, in the draws' order) */
static void ol_draw(const uint8_t *p) {
    static const float kQuad[8] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    GLuint t;
    int fromIds = sIdsOk && sIdsTex != 0; /* the copy of the ids made just before: no split needed */
    (void)p;
    target_set(0);
    if (!fromIds && !vglSplitScene() && !sOlSplitDone) { /* (the picture so far in memory: the alpha is read from there) */
        gVitaOl[1]++;
        return;
    }
    sOlSplitDone = 0;
    if (sMidOn && (fromIds ? vglSplitScene() : 1)) {
        mid_shot(3); /* (the log: the picture before the outline) */
    }
    t = fromIds ? sIdsTex : ol_view();
    if (t == 0) {
        return;
    }
    glUseProgram(sOlProg);
    glBindTexture(GL_TEXTURE_2D, t);
    if (sOlU[2] >= 0) {
        glUniform1i(sOlU[2], 0);
    }
    glUniform4f(sOlU[0], (float)VIEW_X / (float)SCR_W, (float)VIEW_W / (float)SCR_W, 1.0f / (float)SCR_W, 1.0f / (float)SCR_H);
    glUniform4f(sOlU[1], 100.0f / 255.0f, 0.0f, 0.0f, 0.0f);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD); /* the picture minus the darkening */
    glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    scissor(VIEW_X, 0, VIEW_W, VIEW_H);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (!fromIds) {
        vglSplitScene(); /* (what is drawn next, in the same scene, could change the alpha this pass reads elsewhere) */
    }
    GLC_RESET();
    gVitaOl[0]++;
    if (!fromIds) {   /* the outline's two parts count as the picture's time splits (each part costs the chip a store
           and a load of the picture and its depth: 156 had ~6 a picture) */
        unsigned long long now = sceKernelGetProcessTimeWide();
        sSplitN += 2;
        sSplitAt = sSplitAt != 0 && sSplitN < SPLIT_MAX ? now + SPLIT_MS * 1000ull : 0;
    }
}
/* a picture of the alpha as the outline reads it, for the log (the drawing thread, the render thread idle) */
static void ol_alpha_shot(unsigned id, unsigned list) {
    const uint8_t *fb;
    uint8_t *o;
    int x, y;
    extern SceGxmContext *gxm_context;
    if (sSqN == 8 || (o = (uint8_t *)malloc((size_t)SCR_W * SCR_H * 3)) == NULL) {
        return;
    }
    target_set(0);
    sOlSplitDone = vglSplitScene();
    sceGxmFinish(gxm_context);
    fb = (const uint8_t *)gxm_color_surfaces_addr[gxm_back_buffer_index];
    if (fb == NULL) {
        free(o);
        return;
    }
    for (y = 0; y < SCR_H; y++) {
        for (x = 0; x < SCR_W; x++) {
            const uint8_t *q = fb + ((size_t)y * ((SCR_W + 63) & ~63) + (size_t)x) * 4;
            uint8_t *w = &o[(y * SCR_W + x) * 3];
            w[0] = q[3]; /* the alpha as it is */
            w[1] = 0;
            w[2] = 0;
        }
    }
    sSq[sSqN].id = id;
    sSq[sSqN].list = list;
    sSq[sSqN].w = SCR_W;
    sSq[sSqN].h = SCR_H;
    sSq[sSqN].px = o;
    sSqN++;
    gVitaOl[2]++;
}
/* the game's marker (GsGpu_Native 1): the outline here */
static void ol_marker(void) {
    extern int Vita_InBattle(void);
    static unsigned shots;
    if (sOlProg == 0 || gVitaGsSkip || !Vita_InBattle()) {
        return;
    }
    sOlTail = 1; /* the game's own outline passes after this: dropped (GsGpu_Draw) */
    al_read(); /* (the picture's alpha is read here: the writes before it are kept) */
    sAlphaRead = gGsFrame;
    flush();
    flush_bins_c(15); /* (everything before it drawn first, in its order) */
    if (sOlShotWant) {
        sOlShotWant = 0;
        rt_sync();
        ol_alpha_shot(9000 + shots++ * 10 + 3, gGsFrame);
    }
    if (gVitaOlOff || (gVitaFxMask & 8)) {
        return;
    }
    if (sRtOn) {
        rt_reserve(RT_OL, 16);
        rt_commit();
    } else {
        ol_draw(NULL);
    }
}
/* ---- The depth page's spare byte and the passes that draw it through a table (gfxm_b.c
   GfxPost_DrawDepthClut: the depth tint, the alpha that masks the glare and the object glow, the see-through tint).
   The top byte of each depth word holds either a fog value made from the depth (GfxDepthFog_Draw) or a copy of the
   picture's alpha, where each model wrote a number of its own (GfxPost_CopyAlphaToDepth); the game then draws that
   byte over the screen as an 8-bit texture with a 256-colour table. Here, as in the PC renderer (dclut.frag): the
   copy of the ids is a copy of the display buffer's alpha (the scene split there, one full-screen pass into a
   picture of its own), the fog value is worked out from the depth buffer (stored when the scene is split), the
   table comes from GS memory, and the 16 strips of a pass are one full-screen draw with the game's blend and mask.
   The outline reads the same copy (no split of its own any more). ---- */
static const char kVertFx[] =
    "uniform float4 uV;\n" /* x: the left edge, y: the width (in the display buffer's widths), z: 1, or -1 into a picture of ours */
    "void main(float2 aPos, float4 out vPos : POSITION, float2 out t0 : TEXCOORD0) {\n"
    "    vPos = float4(aPos, 0.0f, 1.0f);\n"
    "    t0 = float2(uV.x + (aPos.x * 0.5f + 0.5f) * uV.y, 0.5f - aPos.y * 0.5f * uV.z);\n"
    "}\n";
static const char kFragCopy[] =
    "uniform sampler2D uSrc;\n"
    "float4 main(float2 t0 : TEXCOORD0) : COLOR {\n"
    "    return tex2D(uSrc, t0);\n"
    "}\n";
static const char kFragTblIds[] =
    "uniform sampler2D uSrc;\n"
    "uniform sampler2D uClut;\n"
    "float4 main(float2 t0 : TEXCOORD0) : COLOR {\n"
    "    float a = tex2D(uSrc, t0).a;\n"
    "    float s = floor(a * 255.0f + 0.5f);\n"
    "    float idx = (s > 233.5f && fmod(s, 2.0f) < 0.5f) ? (s + 254.0f) * 0.5f : a > 0.998f ? 255.0f : floor(a * 128.0f + 0.5f);\n" /* (the GS's alpha: 0x80 is 1.0 here;
                                                                     0xF4..0xFE the fighters seen through, as 234 + 2k (kTexId); 0xFF "nothing") */
    "    float4 c = tex2D(uClut, float2((idx + 0.5f) * (1.0f / 256.0f), 0.5f));\n"
    "    return float4(c.rgb, saturate(c.a * (255.0f / 128.0f)));\n"
    "}\n";
static const char kFragTblFog[] =
    "uniform sampler2D uSrc;\n"
    "uniform sampler2D uClut;\n"
    "uniform float4 uP;\n" /* x: the depth buffer's largest value */
    "float4 main(float2 t0 : TEXCOORD0) : COLOR {\n"
    "    float z = tex2D(uSrc, t0).x * uP.x;\n"
    "    float idx = z >= 65448.0f ? 0.0f : clamp(255.0f - floor(z * (1.0f / 256.0f)), 0.0f, 255.0f);\n"
    "    float4 c = tex2D(uClut, float2((idx + 0.5f) * (1.0f / 256.0f), 0.5f));\n"
    "    return float4(c.rgb, saturate(c.a * (255.0f / 128.0f)));\n"
    "}\n";
static const char kFragFogCopy[] = /* the fog byte (dclut.frag's) from the depth, into a picture of its own */
    "uniform sampler2D uSrc;\n"
    "uniform float4 uP;\n" /* x: the depth buffer's largest value */
    "float4 main(float2 t0 : TEXCOORD0) : COLOR {\n"
    "    float z = tex2D(uSrc, t0).x * uP.x;\n"
    "    float idx = z >= 65448.0f ? 0.0f : clamp(255.0f - floor(z * (1.0f / 256.0f)), 0.0f, 255.0f);\n"
    "    return float4(idx, idx, idx, idx) * (1.0f / 255.0f);\n"
    "}\n";
static const char kFragXfade[] =
    "uniform sampler2D uSrc;\n"
    "uniform float4 uP;\n" /* x: the alpha */
    "float4 main(float2 t0 : TEXCOORD0) : COLOR {\n"
    "    return float4(tex2D(uSrc, t0).rgb, uP.x);\n"
    "}\n";
static const char kFragTblByte[] = /* a table pass from that byte */
    "uniform sampler2D uSrc;\n"
    "uniform sampler2D uClut;\n"
    "float4 main(float2 t0 : TEXCOORD0) : COLOR {\n"
    "    float idx = floor(tex2D(uSrc, t0).r * 255.0f + 0.5f);\n"
    "    float4 c = tex2D(uClut, float2((idx + 0.5f) * (1.0f / 256.0f), 0.5f));\n"
    "    return float4(c.rgb, saturate(c.a * (255.0f / 128.0f)));\n"
    "}\n";
static GLuint sDepthTex, sFxProg[5]; /* copy, table from the ids, table from the fog, (163) fog byte, table from it */
static GLint sFxU[5][4];                      /* uV, uSrc, uClut, uP */
static int sDepthByteIsFog;    /* the drawing thread: what the depth page's top byte holds now */
static int sTblRun;            /* the drawing thread: the GS primitive before was a strip of this table pass */
static uint64_t sTblRegs[6];
static struct { uint32_t hash; unsigned last; int used; } sClut[16]; /* (the drawing thread: which table each slot holds) */
static uint32_t sClutPx[16][256]; /* (the slots' entries: a table is reused only when every entry is the same) */
static GLuint sClutTex[16];                                           /* (the thread that draws: the slots' textures) */
typedef struct TblOp { State d; int src, slot, upload; float zmax; uint32_t cbp; uint32_t px[256]; } TblOp;
static GLuint fx_prog(const char *frag, int len, const char *what) {
    static GLuint vs;
    GLuint fs, prog;
    GLint ok = 0, l;
    const char *src;
    char log[300];
    if (vs == 0) {
        vs = glCreateShader(GL_CG_VERTEX_SHADER_EXT);
        src = kVertFx; l = (GLint)sizeof(kVertFx) - 1;
        glShaderSource(vs, 1, &src, &l);
        glCompileShader(vs);
        glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            log[0] = 0; glGetShaderInfoLog(vs, sizeof(log) - 1, NULL, log); log[sizeof(log) - 1] = 0;
            Vita_Log("vgl: effects' vertex shader not compiled: %s\n", log);
            vs = 0;
            return 0;
        }
    }
    fs = glCreateShader(GL_CG_FRAGMENT_SHADER_EXT);
    src = frag; ok = 0;
    glShaderSource(fs, 1, &src, &len);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        log[0] = 0; glGetShaderInfoLog(fs, sizeof(log) - 1, NULL, log); log[sizeof(log) - 1] = 0;
        Vita_Log("vgl: %s pixel shader not compiled: %s\n", what, log);
        return 0;
    }
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glLinkProgram(prog);
    return prog;
}
/* A picture of the display's size with its framebuffer object (made again, at the new size, when that changes). */
static void fx_target(GLuint *tex, GLuint *fbo) {
    if (*fbo != 0) {
        glDeleteFramebuffers(1, fbo);
        *fbo = 0;
    }
    if (*tex != 0) {
        glDeleteTextures(1, tex);
        *tex = 0;
    }
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SCR_W, SCR_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
/* the copies of the picture: the ids, the fog byte, the cross-fade's */
static void fx_targets(void) {
    fx_target(&sIdsTex, &sIdsFbo);
    if (sFxProg[3] && sFxProg[4]) {
        fx_target(&sFogTex, &sFogFbo);
    }
    if (sXfProg != 0) {
        fx_target(&sXfTex, &sXfFbo);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}
static void fx_init(void) {
    extern int gVglSafe2;
    extern void vglTexImageDepthBuffer(GLenum target);
    int i, k;
    for (i = 0; i < 3; i++) { /* the display buffers as textures (their memory: made once, never let go) */
        if (gxm_color_surfaces_addr[i] != NULL && sOlTex[i] == 0) {
            glGenTextures(1, &sOlTex[i]);
            glBindTexture(GL_TEXTURE_2D, sOlTex[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            vglSetupTexture(GL_TEXTURE_2D, gxm_color_surfaces_addr[i], SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, SCE_GXM_TEXTURE_LINEAR, SCR_W, SCR_H, 1);
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    sWtOn = !(gVglSafe2 & 128);
    gVitaClipOff = (gVglSafe2 & 512) != 0;
    if (gVglSafe2 & 256) {
        Vita_Log("vgl: table passes off (safe2=256)\n");
        return;
    }
    sFxProg[0] = fx_prog(kFragCopy, (int)sizeof(kFragCopy) - 1, "copy");
    sFxProg[1] = fx_prog(kFragTblIds, (int)sizeof(kFragTblIds) - 1, "table (ids)");
    sFxProg[2] = fx_prog(kFragTblFog, (int)sizeof(kFragTblFog) - 1, "table (fog)");
    sFxProg[3] = fx_prog(kFragFogCopy, (int)sizeof(kFragFogCopy) - 1, "fog byte");
    sFxProg[4] = fx_prog(kFragTblByte, (int)sizeof(kFragTblByte) - 1, "table (fog byte)");
    for (k = 0; k < 5; k++) {
        sFxU[k][0] = sFxProg[k] ? glGetUniformLocation(sFxProg[k], "uV") : -1;
        sFxU[k][1] = sFxProg[k] ? glGetUniformLocation(sFxProg[k], "uSrc") : -1;
        sFxU[k][2] = sFxProg[k] ? glGetUniformLocation(sFxProg[k], "uClut") : -1;
        sFxU[k][3] = sFxProg[k] ? glGetUniformLocation(sFxProg[k], "uP") : -1;
    }
    sXfProg = fx_prog(kFragXfade, (int)sizeof(kFragXfade) - 1, "cross-fade");
    if (sXfProg != 0) { /* the cross-fade's picture */
        sXfU[0] = glGetUniformLocation(sXfProg, "uV");
        sXfU[1] = glGetUniformLocation(sXfProg, "uSrc");
        sXfU[2] = glGetUniformLocation(sXfProg, "uP");
    }
    fx_targets();
    glGenTextures(1, &sDepthTex);
    glBindTexture(GL_TEXTURE_2D, sDepthTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    vglTexImageDepthBuffer(GL_TEXTURE_2D); /* (the display's depth buffer as a texture: its memory, stored at each scene's end) */
    if (gVitaMsaa == 2 || gVitaMsaa == 4) { /* its samples: 2 x 2 a pixel (4x), 1 x 2 (2x); one of them is read */
        SceGxmTexture *gt = vglGetGxmTexture(GL_TEXTURE_2D);
        if (gt != NULL) {
            void *dd = sceGxmTextureGetData(gt);
            sceGxmTextureInitLinear(gt, dd, SCE_GXM_TEXTURE_FORMAT_DF32M, gVitaMsaa == 4 ? SCR_W * 2 : SCR_W, SCR_H * 2, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    sTblOn = sFxProg[0] && sFxProg[1] && sFxProg[2] && sIdsTex && sIdsFbo && sDepthTex;
    glow_init();
    Vita_Log("vgl: work targets %s; table passes %s (programs %u %u %u, uniforms %d %d %d %d)\n", sWtOn ? "on" : "off", sTblOn ? "on" : "NOT on",
             (unsigned)sFxProg[0], (unsigned)sFxProg[1], (unsigned)sFxProg[2], (int)sFxU[2][0], (int)sFxU[2][1], (int)sFxU[2][2], (int)sFxU[2][3]);
}
/* A linear texture over memory that is not its own (the display's buffers) described again for a new
   size; its filters and edge modes kept. (vglSetupTexture would hand the memory it had to the allocator.) */
static void tex_redescribe(GLuint t, void *data, SceGxmTextureFormat fmt, int w, int h) {
    SceGxmTexture *gt, old;
    glBindTexture(GL_TEXTURE_2D, t);
    gt = vglGetGxmTexture(GL_TEXTURE_2D);
    if (gt == NULL) {
        return;
    }
    old = *gt;
    sceGxmTextureInitLinear(gt, data, fmt, (unsigned)w, (unsigned)h, 0);
    sceGxmTextureSetMinFilter(gt, sceGxmTextureGetMinFilter(&old));
    sceGxmTextureSetMagFilter(gt, sceGxmTextureGetMagFilter(&old));
    sceGxmTextureSetUAddrMode(gt, sceGxmTextureGetUAddrMode(&old));
    sceGxmTextureSetVAddrMode(gt, sceGxmTextureGetVAddrMode(&old));
}
/* (the drawing thread, right after a swap that changed the display's size) everything of the picture's
   size made again for the new one: the view, the display buffers and the depth as textures, the copies of the picture */
unsigned gVitaScrSwitches;
static void scr_resized(int w, int h) {
    int i;
    sScrW = w;
    sScrH = h;
    sViewW = gVitaWide ? SCR_W : SCR_W * 725 / 960;
    for (i = 0; i < 3; i++) {
        if (sOlTex[i] != 0 && gxm_color_surfaces_addr[i] != NULL) {
            tex_redescribe(sOlTex[i], gxm_color_surfaces_addr[i], SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, w, h);
        }
    }
    if (sDepthTex != 0) {
        extern void vglTexImageDepthBuffer(GLenum target);
        glBindTexture(GL_TEXTURE_2D, sDepthTex);
        vglTexImageDepthBuffer(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    if (sIdsTex != 0) {
        fx_targets();
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    sIdsOk = 0;
    sFogOk = 0;
    sXfOk = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    sTgtCur = 0;
    glViewport(VIEW_X, 0, VIEW_W, VIEW_H);
    sScissor[0] = sScissor[1] = sScissor[2] = sScissor[3] = -1;
    GLC_RESET();
    gVitaScrSwitches++;
    Vita_Log("vgl: the picture is drawn at %d x %d from list %u\n", w, h, gGsFrame);
}
/* the picture's swap; the menus' size or the fights' asked for the next picture */
static void swap_sized(void) {
    extern int gPortMenuMode; /* port/src/headless.c: the menus are running */
    int menu = gVitaMenuHi && gPortMenuMode;
    int w = menu ? SCR_W_MENU : SCR_W_FIGHT, h = menu ? SCR_H_MENU : SCR_H_FIGHT;
    int change = w != sScrW || h != sScrH;
    if (change) {
        rt_sync();
        vglSwapResolution(w, h);
    }
    wt_swap();
    if (change) {
        scr_resized(w, h);
    }
}
static void fx_quad(void) {
    static const float kQuad[8] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
/* (the thread that draws) the ids: the picture so far to memory, its alpha copied into a picture of their own */
static void ids_exec(void) {
    GLuint view = sOlTex[gxm_back_buffer_index % 3];
    /* (no split of the picture's scene here: the switch to the copy's own target ends it, and the chip stores it
       before it begins the copy) */
    sIdsOk = 0;
    if (view == 0 || sFxProg[0] == 0) {
        return;
    }
    target_set(TGT_IDS);
    glUseProgram(sFxProg[0]);
    glBindTexture(GL_TEXTURE_2D, view);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (sFxU[0][1] >= 0) {
        glUniform1i(sFxU[0][1], 0);
    }
    glUniform4f(sFxU[0][0], 0.0f, 1.0f, -1.0f, 0.0f); /* (into a picture of ours: turned over, so its rows are the display's) */
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    scissor(0, 0, SCR_W, SCR_H);
    fx_quad();
    GLC_RESET();
    sIdsOk = 1;
    gVitaWt[7]++;
}
/* GfxDepthFog_Draw: the fog byte worked out once from the depth as it is now (the
   switch to its own picture stores the depth), as the PS2 keeps it in the depth page's spare byte; the table passes
   that read it then take this picture, with no split of the picture's scene each */
static void fog_exec(const uint8_t *p) {
    float zmax;
    memcpy(&zmax, p, 4);
    sFogOk = 0;
    if (sFogFbo == 0 || sFxProg[3] == 0) {
        return;
    }
    target_set(TGT_FOG);
    glUseProgram(sFxProg[3]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sDepthTex);
    if (sFxU[3][1] >= 0) {
        glUniform1i(sFxU[3][1], 0);
    }
    glUniform4f(sFxU[3][0], 0.0f, 1.0f, -1.0f, 0.0f); /* (the whole display, turned over into a picture of ours) */
    if (sFxU[3][3] >= 0) {
        glUniform4f(sFxU[3][3], zmax, 0.0f, 0.0f, 0.0f);
    }
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    scissor(0, 0, SCR_W, SCR_H);
    fx_quad();
    GLC_RESET();
    sFogOk = 1;
}
/* ---- The intro's cross-fade (src/battle/stg_b.c ScrXfade): the picture shown last, copied when the game
   asks for a capture, drawn over the frames that follow with the game's alpha (128 down to 0 over a second). ---- */
unsigned gVitaXf[3]; /* for the log: captures, draws, draws left out (nothing captured) */
static void xf_exec(const uint8_t *p) {
    int alpha, capture;
    extern unsigned int gxm_front_buffer_index;
    memcpy(&alpha, p, 4);
    memcpy(&capture, p + 4, 4);
    if (capture) { /* the picture shown last (the frame before this one) into the copy, turned over as ids_exec's */
        GLuint view = sOlTex[gxm_front_buffer_index % 3];
        if (view != 0 && sFxProg[0] != 0 && sXfFbo != 0) {
            target_set(TGT_XF);
            glUseProgram(sFxProg[0]);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, view);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            if (sFxU[0][1] >= 0) {
                glUniform1i(sFxU[0][1], 0);
            }
            glUniform4f(sFxU[0][0], 0.0f, 1.0f, -1.0f, 0.0f);
            glDisable(GL_BLEND);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            scissor(0, 0, SCR_W, SCR_H);
            fx_quad();
            GLC_RESET();
            sXfOk = 1;
            gVitaXf[0]++;
        }
    }
    if (!sXfOk || alpha <= 0) {
        gVitaXf[2]++;
        return;
    }
    target_set(0);
    glUseProgram(sXfProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sXfTex);
    if (sXfU[1] >= 0) {
        glUniform1i(sXfU[1], 0);
    }
    glUniform4f(sXfU[0], (float)VIEW_X / (float)SCR_W, (float)VIEW_W / (float)SCR_W, 1.0f, 0.0f);
    glUniform4f(sXfU[2], alpha >= 128 ? 1.0f : (float)alpha / 128.0f, 0.0f, 0.0f, 0.0f);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    scissor(VIEW_X, 0, VIEW_W, VIEW_H);
    fx_quad();
    GLC_RESET();
    gVitaXf[1]++;
}
/* (GsGpu_Native 0x50) the game's cross-fade draw: arg = alpha | capture count << 8 */
static void xf_marker(uint32_t arg) {
    static uint32_t seen = ~0u;
    uint32_t serial = arg >> 8;
    int op[2];
    if (gVitaGsSkip || sXfProg == 0) {
        return; /* (a list not drawn: the capture is made at the next one drawn) */
    }
    op[0] = (int)(arg & 0xFF);
    op[1] = serial != seen;
    seen = serial;
    flush();
    flush_bins_c(15);
    if (sRtOn) {
        memcpy(rt_reserve(RT_XF, 16), op, sizeof(op));
        rt_commit();
    } else {
        xf_exec((const uint8_t *)op);
    }
}
/* (the thread that draws) a table pass */
static void tbl_exec(const uint8_t *p) {
    const TblOp *op = (const TblOp *)(const void *)p;
    int k = op->src == 0 ? (sFogOk ? 4 : 2) : 1;
    target_set(op->d.target);
    if (op->src == 0 && !sFogOk) {
        extern int gVglSafe2;
        if (sMainZ || !(gVglSafe2 & 2048)) { /* (the depth so far to memory: the fog value is worked out from it; an earlier version:
                                                 with safe2=2048, not when it is there already) */
            vglSplitScene();
            sMainZ = 0;
        }
    } else if (!sIdsOk) {
        return;
    }
    GLC_RESET();
    glUseProgram(sFxProg[k]);
    if (op->upload) { /* (a table not held yet: into its slot's texture, here, in the order of the draws) */
        GLuint *ct = &sClutTex[op->slot & 15];
        if (*ct == 0) {
            glGenTextures(1, ct);
        }
        glBindTexture(GL_TEXTURE_2D, *ct);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, op->px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sClutTex[op->slot & 15]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, op->src == 0 ? (k == 4 ? sFogTex : sDepthTex) : sIdsTex);
    if (sFxU[k][1] >= 0) {
        glUniform1i(sFxU[k][1], 0);
    }
    if (sFxU[k][2] >= 0) {
        glUniform1i(sFxU[k][2], 1);
    }
    glUniform4f(sFxU[k][0], (float)VIEW_X / (float)SCR_W, (float)VIEW_W / (float)SCR_W, 1.0f, 0.0f);
    if (sFxU[k][3] >= 0) {
        glUniform4f(sFxU[k][3], op->zmax, 0.0f, 0.0f, 0.0f);
    }
    {
        State d = op->d;
        d.ztest = 0; /* (the passes test nothing: TEST.ZTST ALWAYS) */
        d.zwrite = 0;
        apply_fixed(&d);
    }
    fx_quad();
    if (sMidOn && op->cbp == 0x3E98 && vglSplitScene()) {
        mid_shot(4); /* (the log: the picture after the pan blur's table, before its copy) */
    }
    GLC_RESET();
}
/* the table at `cbp` in GS memory (the order of a PSMT8 palette's entries, as gs_gpu.c) into `px`: the slot that
   holds it, and whether it has to be loaded into that slot (on the thread that draws, with the pass) */
static int clut_slot(uint32_t cbp, uint32_t *px, int *upload) {
    uint32_t raw[256], hash = 2166136261u, i;
    int k, old = 0;
    Gs_VramReadRect(cbp, 1, 0, 16, 16, raw);
    for (i = 0; i < 256; i++) {
        uint32_t n = (i & 0xE7) | ((i & 8) << 1) | ((i & 0x10) >> 1);
        px[i] = raw[n];
        hash = (hash ^ px[i] ^ (px[i] >> 16)) * 16777619u; /* (the top bits folded in: the alpha is there) */
    }
    /* Found by the hash, taken only if every entry is the same (z3xox's fix in the PC build, 52b675c): by the hash
       alone the object glow's table and the glare's ramp, which differ only in alpha, could be one, and the
       fighters' glow was drawn through the glare's ramp, the whole picture white (teen Gohan's third costume on
       Namek). */
    for (k = 0; k < 16; k++) {
        if (sClut[k].used && sClut[k].hash == hash && memcmp(sClutPx[k], px, 256 * sizeof(uint32_t)) == 0) {
            sClut[k].last = gGsFrame;
            *upload = 0;
            return k;
        }
        if (!sClut[k].used || (sClut[old].used && sClut[k].last < sClut[old].last)) {
            old = k;
        }
    }
    sClut[old].used = 1;
    sClut[old].hash = hash;
    sClut[old].last = gGsFrame;
    memcpy(sClutPx[old], px, 256 * sizeof(uint32_t));
    *upload = 1;
    return old;
}
/* (GsGpu_Draw, the drawing thread) the passes about the depth page's spare byte: 1 when taken here */
static int tbl_intercept(int type, int ctx) {
    uint64_t prim = gGs.prim, t0 = gGs.tex0[ctx], fr = gGs.frame[ctx];
    uint32_t fbp = (uint32_t)(fr & 0x1FF), zbp = (uint32_t)(gGs.zbuf[ctx] & 0x1FF), fpsm = (uint32_t)((fr >> 24) & 0x3F);
    uint32_t tpsm = (uint32_t)((t0 >> 20) & 0x3F), tbp = (uint32_t)(t0 & 0x3FFF), cbp = (uint32_t)((t0 >> 37) & 0x3FFF);
    uint64_t regs[6];
    int run = sTblRun;
    sTblRun = 0;
    if (!sTblOn || !vgl_bt3_split_on || type != 6 || !((prim >> 4) & 1)) {
        return 0;
    }
    if (fbp == zbp && Gs_PsmBits(fpsm) != 16) { /* a pass that fills the spare byte (not drawn: see state_of_) */
        int mainPage = gGsMainFbp >= 0 ? tbp / 32 == (uint32_t)gGsMainFbp : (tbp / 32 == 0 || tbp / 32 == 0x70);
        if (tpsm == 0x1B) {
            sDepthByteIsFog = 1; /* GfxDepthFog_Draw */
            if (!run && sFogFbo != 0 && !(gVglSafe2 & 4096)) { /* its 16 strips: one copy of the fog byte */
                float zmax = ((gGs.zbuf[ctx] >> 24) & 15) == 0 ? 4294967295.0f : ((gGs.zbuf[ctx] >> 24) & 15) == 1 ? 16777215.0f : 65535.0f;
                flush();
                if (sRtOn) {
                    memcpy(rt_reserve(RT_FOG, 16), &zmax, 4);
                    rt_commit();
                } else {
                    fog_exec((const uint8_t *)&zmax);
                }
            }
            sTblRun = 1;
        } else if (tpsm == 0 && mainPage && (tbp & 0x1F) == 0) {
            if ((sDepthByteIsFog || !run) && !(gVitaFxMask & 16)) { /* GfxPost_CopyAlphaToDepth (its 16 strips: one copy) */
                flush();
                if (sRtOn) {
                    rt_reserve(RT_IDS, 16);
                    rt_commit();
                } else {
                    ids_exec();
                }
            }
            sDepthByteIsFog = 0;
            sTblRun = 1;
        }
        return 0;
    }
    if (tpsm != 0x1B || tbp / 32 != zbp) {
        return 0;
    }
    if (cbp == 0x3E8C && sOlProg != 0) {
        return 1; /* the outline's own passes: the outline is drawn at its marker */
    }
    if (gVitaFxOff || (gVitaFxMask & 1)) {
        return 0; /* (the log's pictures without the effects: as before, left out) */
    }
    if (cbp == 0x3E98 && (gVitaFxMask & 32)) {
        return 1; /* (the log's film: the pan blur's pass left out) */
    }
    if (cbp == 0x3E94 && !sSeeThru) {
        return 1; /* the see-through tint with nothing seen through: every entry it would draw is transparent */
    }
    regs[0] = fr; regs[1] = gGs.alpha[ctx]; regs[2] = gGs.test[ctx]; regs[3] = t0; regs[4] = gGs.scissor[ctx]; regs[5] = (uint64_t)sDepthByteIsFog;
    if (run && memcmp(regs, sTblRegs, sizeof(regs)) == 0) {
        sTblRun = 1;
        return 1; /* the next strip of the same pass */
    }
    {
        State d;
        TblOp *op;
        int ok;
        sTblState = 1;
        sWpAllow = 0;
        ok = state_of_(ctx, 6, &d);
        sTblState = 0;
        if (!ok || d.target != 0) {
            WorkTarget *t = wt_find(fbp, (uint32_t)((fr >> 16) & 0x3F));
            if (t != NULL) {
                t->stale = 1;
            }
            gVitaWt[8]++;
            tbl_count(cbp, 0);
            return 1; /* (into anything but the picture: not drawn) */
        }
        flush();
        if (sRtOn) {
            op = (TblOp *)(void *)rt_reserve(RT_TBL, sizeof(TblOp));
        } else {
            static TblOp one;
            op = &one;
        }
        op->d = d;
        op->src = sDepthByteIsFog ? 0 : 1;
        op->slot = clut_slot(cbp, op->px, &op->upload);
        op->cbp = cbp;
        {   /* a table of alphas all 0 (no object glows; no glare): the glow that reads them adds nothing */
            int q;
            for (q = 0; q < 256 && (op->px[q] >> 24) == 0; q++) {
            }
            sTblZero = q == 256;
        }
        op->zmax = ((gGs.zbuf[ctx] >> 24) & 15) == 0 ? 4294967295.0f : ((gGs.zbuf[ctx] >> 24) & 15) == 1 ? 16777215.0f : 65535.0f;
        if (sRtOn) {
            rt_commit();
        } else {
            tbl_exec((const uint8_t *)op);
        }
        memcpy(sTblRegs, regs, sizeof(regs));
        sTblRun = 1;
        gVitaWt[5]++;
        gVitaWt[6] += sDepthByteIsFog != 0;
        tbl_count(cbp, 1);
        if (!sDepthByteIsFog) {
            al_read(); /* (the picture's alpha read: the ids written before are kept) */
            sAlphaRead = gGsFrame;
        }
    }
    return 1;
}
/* ---- The glow, native (gfxm_a.c GfxPost_DrawGlow: the object glow and the sky glare). The game's
   chain halves the picture three times through work pages (each weighted by the picture's alpha, which the table
   pass before set), blurs the 64 x 64 result ten times between two pages, scales it back up tinted with `glow` and
   adds it to the picture: about fourteen scenes a glow on this chip, two glows a picture, and the chip's time went
   from 15 to 47 ms. Here: the picture weighted by its alpha into 64 x 64 (one pass of eight taps), a
   Gaussian of the ten passes' width in two passes, and the add: three scenes. ---- */
static const char kFragGlowDown[] =
    "uniform sampler2D uSrc;\n"
    "uniform float4 uP;\n" /* xy: one display pixel in texture units; zw: unused */
    "uniform float4 uC;\n" /* the tint (the game's colour / 128) */
    "float4 main(float2 t0 : TEXCOORD0) : COLOR {\n"
    "    float3 s = float3(0.0f, 0.0f, 0.0f);\n"
    "    float4 c;\n"
    "    c = tex2D(uSrc, t0 + float2(-3.75f, -1.4f) * uP.xy); s += c.rgb * c.a;\n"
    "    c = tex2D(uSrc, t0 + float2(-1.25f, -1.4f) * uP.xy); s += c.rgb * c.a;\n"
    "    c = tex2D(uSrc, t0 + float2( 1.25f, -1.4f) * uP.xy); s += c.rgb * c.a;\n"
    "    c = tex2D(uSrc, t0 + float2( 3.75f, -1.4f) * uP.xy); s += c.rgb * c.a;\n"
    "    c = tex2D(uSrc, t0 + float2(-3.75f,  1.4f) * uP.xy); s += c.rgb * c.a;\n"
    "    c = tex2D(uSrc, t0 + float2(-1.25f,  1.4f) * uP.xy); s += c.rgb * c.a;\n"
    "    c = tex2D(uSrc, t0 + float2( 1.25f,  1.4f) * uP.xy); s += c.rgb * c.a;\n"
    "    c = tex2D(uSrc, t0 + float2( 3.75f,  1.4f) * uP.xy); s += c.rgb * c.a;\n"
    "    return float4(s * 0.125f * uC.rgb, 1.0f);\n"
    "}\n";
static const char kFragGlowBlur[] =
    "uniform sampler2D uSrc;\n"
    "uniform float4 uP;\n" /* xy: one texel along the blur */
    "float4 main(float2 t0 : TEXCOORD0) : COLOR {\n"
    "    float3 s = tex2D(uSrc, t0).rgb * 0.2432f;\n"
    "    s += (tex2D(uSrc, t0 + uP.xy).rgb + tex2D(uSrc, t0 - uP.xy).rgb) * 0.2048f;\n"
    "    s += (tex2D(uSrc, t0 + 2.0f * uP.xy).rgb + tex2D(uSrc, t0 - 2.0f * uP.xy).rgb) * 0.1221f;\n"
    "    s += (tex2D(uSrc, t0 + 3.0f * uP.xy).rgb + tex2D(uSrc, t0 - 3.0f * uP.xy).rgb) * 0.0516f;\n"
    "    return float4(s, 1.0f);\n"
    "}\n";
static const char kFragGlowAdd[] =
    "uniform sampler2D uSrc;\n"
    "uniform float4 uC;\n"
    "float4 main(float2 t0 : TEXCOORD0) : COLOR {\n"
    "    return float4(tex2D(uSrc, t0).rgb * uC.rgb, 0.0f);\n"
    "}\n";
static GLuint sGlowProg[3];
static GLint sGlowU[3][4]; /* uV, uSrc, uP, uC */
static int sGlowOn;
typedef struct GlowOp { uint32_t color, glow; } GlowOp;
static void glow_init(void) {
    extern int gVglSafe2;
    int k;
    if (gVglSafe2 & 1024) {
        Vita_Log("vgl: native glow off (safe2=1024)\n");
        return;
    }
    sGlowProg[0] = fx_prog(kFragGlowDown, (int)sizeof(kFragGlowDown) - 1, "glow (down)");
    sGlowProg[1] = fx_prog(kFragGlowBlur, (int)sizeof(kFragGlowBlur) - 1, "glow (blur)");
    sGlowProg[2] = fx_prog(kFragGlowAdd, (int)sizeof(kFragGlowAdd) - 1, "glow (add)");
    for (k = 0; k < 3; k++) {
        sGlowU[k][0] = sGlowProg[k] ? glGetUniformLocation(sGlowProg[k], "uV") : -1;
        sGlowU[k][1] = sGlowProg[k] ? glGetUniformLocation(sGlowProg[k], "uSrc") : -1;
        sGlowU[k][2] = sGlowProg[k] ? glGetUniformLocation(sGlowProg[k], "uP") : -1;
        sGlowU[k][3] = sGlowProg[k] ? glGetUniformLocation(sGlowProg[k], "uC") : -1;
    }
    for (k = 0; k < 2; k++) {
        glGenTextures(1, &sGlowTex[k]);
        glBindTexture(GL_TEXTURE_2D, sGlowTex[k]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &sGlowFbo[k]);
        glBindFramebuffer(GL_FRAMEBUFFER, sGlowFbo[k]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sGlowTex[k], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    sTgtCur = -1;
    sGlowOn = sGlowProg[0] && sGlowProg[1] && sGlowProg[2] && sGlowFbo[0] && sGlowFbo[1] && sGlowTex[0] && sGlowTex[1];
    Vita_Log("vgl: native glow %s\n", sGlowOn ? "on" : "NOT on");
}
static void glow_pass(int k, GLuint src, float u0, float u1, float flip) {
    glUseProgram(sGlowProg[k]);
    glBindTexture(GL_TEXTURE_2D, src);
    if (sGlowU[k][1] >= 0) {
        glUniform1i(sGlowU[k][1], 0);
    }
    glUniform4f(sGlowU[k][0], u0, u1, flip, 0.0f);
    fx_quad();
}
/* (the thread that draws) */
static void glow_exec(const uint8_t *p) {
    const GlowOp *op = (const GlowOp *)(const void *)p;
    GLuint view = sOlTex[gxm_back_buffer_index % 3];
    float r = (float)(op->color & 0xFF) / 128.0f, g = (float)((op->color >> 8) & 0xFF) / 128.0f, b = (float)((op->color >> 16) & 0xFF) / 128.0f;
    if (view == 0) {
        return;
    }
    GLC_RESET();
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glActiveTexture(GL_TEXTURE0);
    /* the picture, weighted by its alpha and tinted, into 64 x 64 (the switch ends the picture's scene: in memory) */
    target_set(TGT_GLOW);
    mid_shot(1); /* (the log: the picture before the glow) */
    scissor(0, 0, 64, 64);
    glUseProgram(sGlowProg[0]);
    glBindTexture(GL_TEXTURE_2D, view);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (sGlowU[0][1] >= 0) {
        glUniform1i(sGlowU[0][1], 0);
    }
    glUniform4f(sGlowU[0][0], (float)VIEW_X / (float)SCR_W, (float)VIEW_W / (float)SCR_W, -1.0f, 0.0f);
    glUniform4f(sGlowU[0][2], 1.0f / (float)SCR_W, 1.0f / (float)SCR_H, 0.0f, 0.0f);
    glUniform4f(sGlowU[0][3], r, g, b, 1.0f);
    fx_quad();
    /* the blur: across, then down */
    target_set(TGT_GLOW + 1);
    scissor(0, 0, 64, 64);
    glUseProgram(sGlowProg[1]);
    glUniform4f(sGlowU[1][2], 1.0f / 64.0f, 0.0f, 0.0f, 0.0f);
    glow_pass(1, sGlowTex[0], 0.0f, 1.0f, -1.0f);
    target_set(TGT_GLOW);
    scissor(0, 0, 64, 64);
    glUseProgram(sGlowProg[1]);
    glUniform4f(sGlowU[1][2], 0.0f, 1.0f / 64.0f, 0.0f, 0.0f);
    glow_pass(1, sGlowTex[1], 0.0f, 1.0f, -1.0f);
    /* added to the picture, tinted with `glow` (ALPHA 0x68, FIX 0x80: Cs + Cd; the picture's alpha left as it is) */
    target_set(0);
    scissor(VIEW_X, 0, VIEW_W, VIEW_H);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glUseProgram(sGlowProg[2]);
    glUniform4f(sGlowU[2][3], (float)(op->glow & 0xFF) / 128.0f, (float)((op->glow >> 8) & 0xFF) / 128.0f, (float)((op->glow >> 16) & 0xFF) / 128.0f, 0.0f);
    glow_pass(2, sGlowTex[0], 0.0f, 1.0f, 1.0f);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (sMidOn && vglSplitScene()) {
        mid_shot(2); /* (the log: after it) */
    }
    GLC_RESET();
}
/* (the drawing thread) the glow's markers: 0x40 its colour, 0x41 its tint (the glow begins), 0x42 its end */
static void glow_marker(int m, uint32_t arg) {
    static uint32_t color;
    if (m == 0x40) {
        color = arg;
        return;
    }
    if (m == 0x42) {
        sGlowSkip = 0;
        return;
    }
    if (!sGlowOn || gVitaGsSkip) {
        return; /* (the game's passes, the general way) */
    }
    sGlowSkip = 1;
    if (gVitaFxOff || (gVitaFxMask & 2)) {
        return; /* (the log's pictures without the effects) */
    }
    if (sTblZero || (arg & 0xFFFFFF) == 0) {
        sGlowNull++;
        return; /* the alphas it weights the picture by are all 0, or its tint is black: it would add nothing */
    }
    flush();
    flush_bins_c(15);
    if (sRtOn) {
        GlowOp *op = (GlowOp *)(void *)rt_reserve(RT_GLOW, sizeof(GlowOp));
        op->color = color;
        op->glow = arg;
        rt_commit();
    } else {
        GlowOp one;
        one.color = color;
        one.glow = arg;
        glow_exec((const uint8_t *)&one);
    }
    sGlowN++;
}
static void t114_out(const uint8_t *b, unsigned n) {
    static const char k64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    while (n > 0) {
        char line[320];
        unsigned m = n < 225 ? n : 225, i, o = 0;
        for (i = 0; i < m; i += 3) {
            uint32_t v = (uint32_t)b[i] << 16 | (uint32_t)(i + 1 < m ? b[i + 1] : 0) << 8 | (i + 2 < m ? b[i + 2] : 0);
            line[o++] = k64[v >> 18];
            line[o++] = k64[(v >> 12) & 63];
            line[o++] = i + 1 < m ? k64[(v >> 6) & 63] : '=';
            line[o++] = i + 2 < m ? k64[v & 63] : '=';
        }
        line[o] = 0;
        Vita_Log("shotz: %u %u %s\n", sSq[0].id, sZsSeq++, line);
        b += m;
        n -= m;
    }
}
static void t114_emit(void) {
    static uint8_t out[4096];
    unsigned w, h, rows;
    int fin;
    if (sSqN == 0) {
        return;
    }
    w = sSq[0].w;
    h = sSq[0].h;
    if (!sZsOn) {
        memset(&sZs, 0, sizeof(sZs));
        if (deflateInit(&sZs, sSq[0].w > 320 ? 1 : 6) != Z_OK) {
            return;
        }
        sZsOn = 1;
        sZsRow = 0;
        sZsSeq = 0;
        Vita_Log("shotz: %u begin %u %u list %u\n", sSq[0].id, w, h, sSq[0].list);
    }
    {   /* (more rows at a time while many pictures wait) */
        unsigned step = sSqN > 20 ? h : sSqN > 6 ? 46u : (w > 320 ? 24u : 12u); /* (a whole picture at a time with 20 waiting) */
        rows = h - sZsRow < step ? h - sZsRow : step;
    }
    fin = sZsRow + rows >= h;
    sZs.next_in = sSq[0].px + (size_t)sZsRow * w * 3;
    sZs.avail_in = rows * w * 3;
    sZsRow += rows;
    for (;;) {
        int r;
        sZs.next_out = out;
        sZs.avail_out = sizeof(out);
        r = deflate(&sZs, fin ? Z_FINISH : Z_NO_FLUSH);
        t114_out(out, (unsigned)(sizeof(out) - sZs.avail_out));
        if (r == Z_STREAM_END || (r != Z_OK && r != Z_BUF_ERROR) || (!fin && sZs.avail_in == 0 && sZs.avail_out != 0)) {
            break;
        }
    }
    if (fin) {
        int k;
        deflateEnd(&sZs);
        Vita_Log("shotz: %u end\n", sSq[0].id);
        free(sSq[0].px);
        for (k = 1; k < sSqN; k++) {
            sSq[k - 1] = sSq[k];
        }
        sSqN--;
        sZsOn = 0;
    }
}
static void tex_reset_all(void) {
    int k;
    for (k = 0; k < sTexCount; k++) {
        tex_free(&sTex[k]);
    }
    sTexCount = 0;
    sLast = NULL;
    rebuild_buckets();
    gGsEpoch++; /* (the states and keys remembered refer to them) */
    sStCacheGen++; /* (and the states kept by the pages' numbers) */
}
static unsigned sT116List;
void GsGpu_T116(unsigned var, unsigned shot, unsigned list) {
    ag_close(); /* the additive group before anything else */
    sT114Var = (int)var;
    gT116Be = (int)var;
    sT114Shot = shot;
    sT116List = list;
}
/* the end of a list (drawn or not): the changes end with it */
static void t114_list_end(int drawn) {
    if (sT114Shot != 0 || sT114Var != 0) {
        sT114Var = 0;
        gT116Be = 0;
        sT114Shot = 0;
    }
    (void)drawn;
}
static void t114_list_end_old(int drawn) __attribute__((unused));
static void t114_list_end_old(int drawn) {
    extern int Vita_InFight(void);
    unsigned next, k;
    int v;
    if (!Vita_InFight()) {
        if (sT114Var == 3 && sT114DsKeep >= 0) {
            sDsOn = sT114DsKeep;
        }
        sT114Var = 0;
        sT114Shot = 0;
        sFsOn = 0;
        sFF = 0;
        return;
    }
    sFF++;
    if (sT114Shot != 0) {
        if (!drawn) {
            return; /* (the change waits for a list that is drawn) */
        }
        if (sT114Var == 3 && sT114DsKeep >= 0) {
            sDsOn = sT114DsKeep;
        }
        sT114Var = 0;
        sT114Shot = 0;
    }
    if (gVglSafe & 67108864) {
        return;
    }
    next = sFF + 1;
    for (k = 0; k < sizeof(kT114) / sizeof(kT114[0]); k++) {
        if (next >= kT114[k] && next <= kT114[k] + 3) {
            break;
        }
    }
    if (k == sizeof(kT114) / sizeof(kT114[0])) {
        return;
    }
    v = kT115Var[next - kT114[k]];
    sT114Var = v;
    sT114Shot = kT114[k] * 10 + (next - kT114[k]);
    if (next == kT114[k]) {
        sCovSample = 1; /* (the GS primitives in order: the log's sample) */
        sCovN = 0;
        sRunN = 0;
    }
    if (v == 3) {
        sT114DsKeep = sDsOn;
        sDsOn = 0;
    } else if (v == 4) {
        tex_reset_all();
    }
}
void GsGpu_FrameEnd(void) {
    al_frame_end(!gVitaGsSkip);
    flush();
    flush_bins_c(1);
    rt_sync(); /* the render thread's draws done: what follows is this thread's (the parts kept are let go, the swap) */
    target_set(0); /* (the picture bound again before the swap) */
    if (sWpClrN > 0) { /* a clear of the page held back and never needed: the page was not cleared */
        sWpValid = 0;
        sWpClrN = 0;
        sWpClrWhole = 0;
    }
    sWpSlot = WP_OTHER; /* (the markers of the next picture say again) */
    sWpAgain = -1;
    sIdsOk = 0; /* (the copy of the ids is this picture's) */
    sTblRun = 0;
    sGlowSkip = 0; /* */
    sOlTail = 0; /* */
    sMainZ = 1;
    sSeeThru = 0;
    sTblZero = 0;
    if (gVitaFxSweep == 1 && !gVitaGsSkip) { /* the effects left out in turn, 60 pictures each */
        static const int kMask[6] = {0, 1, 2, 4, 8, 15};
        static unsigned n, k;
        if (++n % 60 == 0) {
            k = (k + 1) % 6;
            gVitaFxMask = kMask[k];
        }
    }
    if (gVitaFxSweep == 3 && !gVitaGsSkip) { /* one picture each in turn: the same moments of the fight for all (the log, each stretch) */
        static const int kMask[6] = {0, 1, 2, 4, 8, 15};
        static unsigned k;
        k = (k + 1) % 6;
        gVitaFxMask = kMask[k];
    }
    am_write(0.0f, 0.0f, 512.0f, 448.0f, -1); /* (the next picture's alpha: not known) */
    dp_free_pending();
    if (!gVitaGsSkip) {
        extern int Vita_InFight(void);
        static unsigned n;
        if (VITA_RARE(sCovSample)) {
            int k, j, best;
            float total = 0.0f;
            for (k = 0; k < sCovN; k++) {
                total += sCov[k].area;
            }
            Vita_Log("vgl: picture %u, GS primitives by state: %d states, %.2f screens in all\n", gGsFrame, sCovN, total / (512.0f * 448.0f));
            for (j = 0; j < 64; j++) {
                for (best = -1, k = 0; k < sCovN; k++) {
                    if (sCov[k].n > 0 && (best < 0 || sCov[k].area > sCov[best].area)) { best = k; }
                }
                if (best < 0) { break; }
                Vita_Log("vgl:   #%d %.2f screens in %d %s: TEX0 %08x%08x tex %u env %u, ALPHA %02x fix %02x blend %u (%u,%u,%u), TEST %05x, depth %u/%u, vertex alpha %d..%d, "
                         "colour mask %x, stencil %u, DATE %u, FBA %u; %.2f screens left out; Q ratio %.1f\n",
                         best, sCov[best].area / (512.0f * 448.0f), sCov[best].n, sCov[best].type == 6 ? "sprites" : "triangles", (unsigned)(sCov[best].t0 >> 32), (unsigned)sCov[best].t0,
                         (unsigned)sCov[best].d.tex, sCov[best].d.env, (unsigned)(sCov[best].alpha & 0xFF), (unsigned)((sCov[best].alpha >> 32) & 0xFF), sCov[best].d.blend,
                         sCov[best].d.src, sCov[best].d.dst, sCov[best].d.eq, (unsigned)sCov[best].test, sCov[best].d.ztest, sCov[best].d.zwrite, sCov[best].amin, sCov[best].amax,
                         sCov[best].d.mask, sCov[best].d.sop, sCov[best].d.date, sCov[best].d.fba, sCov[best].skip / (512.0f * 448.0f), sCov[best].qr);
                sCov[best].n = -sCov[best].n;
            }
            {   /* the draws in order: #state x primitives (screens), M models */
                char line[400];
                int len = 0;
                for (k = 0; k < sRunN; k++) {
                    if (len > 300) {
                        Vita_Log("vgl:   in order: %s\n", line);
                        len = 0;
                    }
                    len += sRun[k].k >= 0 ? snprintf(line + len, sizeof(line) - (size_t)len, " #%dx%u(%.2f)", sRun[k].k, sRun[k].n, sRun[k].area / (512.0f * 448.0f))
                           : sRun[k].k == -3 ? snprintf(line + len, sizeof(line) - (size_t)len, " S%ux(%.0f%%)", sRun[k].n, sRun[k].area / (float)(SCR_W * SCR_H) * 100.0f)
                                             : snprintf(line + len, sizeof(line) - (size_t)len, " %s%u", sRun[k].k == -1 ? "M" : "B", sRun[k].n);
                }
                if (len > 0) {
                    Vita_Log("vgl:   in order: %s\n", line);
                }
            }
            sCovSample = 0;
        }
        {   /* (and the picture after one of more than 45 blended screens, twice a fight: what the busiest are made of) */
            static float covPrev;
            static unsigned heavy;
            float fc = gVglCov[1] >= covPrev ? gVglCov[1] - covPrev : gVglCov[1];
            covPrev = gVglCov[1];
            if (!Vita_InFight()) {
                heavy = 0;
            }
            extern int Vita_InBattle(void);
            if ((Vita_InFight() && (++n % 150 == 40 || (fc > 45.0f * 512.0f * 448.0f && heavy < 2 && !sCovSample && ++heavy))) ||
                (gVitaFilm > 0 && !Vita_InFight() && Vita_InBattle() && ++n % 45 == 20)) { /* (the intros too, every 45 pictures, in a test run with the film) */
                sCovSample = 1;
                sCovN = 0;
                sRunN = 0;
            }
        }
    }
    gGsFbUploads = 0; /* (gs_core.c reports the first upload of a frame only) */
    sAnchor = 0;
    {
        extern int Vita_InFight(void);
        static unsigned fightPics;
        if (sDumping && !gVitaGsSkip) {
            Vita_Log("dump: end of the frame\n");
            sDumping = 0;
        }
        {   /* a fight has ended: the next one may have another stage; the chip's copy of this one is let go */
            static int was;
            extern int Vita_InBattle(void);
            int now = Vita_InBattle();
            if (!gGsSplit && was && !now && (sStageUsed != 0 || sFtUsed != 0)) {
                glFinish();
                memset(sStageCache, 0, sizeof(sStageCache));
                sStageUsed = 0;
                memset(sFtCache, 0, sizeof(sFtCache));
                sFtUsed = 0;
                sFtIdxUsed = 0;
            }
            was = now;
        }
        if (!Vita_InFight()) {
            fightPics = 0;
        } else if ((gVglSafe & 4096) && !gVitaGsSkip && (++fightPics == 12 || fightPics == 90)) { /* (off: writing it stops the game for a moment) */
            sDumping = 1; /* the next frame, if it is drawn (else the one after) */
            sDumpLines = 0;
            Vita_Log("dump: fight picture %u, frame %u\n", fightPics, gGsFrame + 1);
        }
    }
    sHaveState = 0;
    if (!gVitaGsSkip) {
        if (sMovieDirty) {
            movie_fill();
        }
        if (sLogFrames > 0) {
            Vita_Log("vgl: frame %u: swap (%u primitives left out for their coordinates)\n", gGsFrame, sBadVerts);
        }
        {
            T_IN;
            gProf = 8;
            if (gVitaProfMeasure) { Vita_ProfSwitch(8); }
            {   /* The GPU profile: in a window of the fight, every picture, how long the chip still works after the
                   last draw is handed over (waiting for it costs the picture its time; outside the window it is not
                   measured: one picture in 32 waited for came late, in the busy stretches) */
                extern unsigned gVitaGpuWait[2];
                extern int Vita_InFight(void);
                static unsigned pics;
                static float profCov;
                float profFc = gVglCov[1] >= profCov ? gVglCov[1] - profCov : gVglCov[1];
                profCov = gVglCov[1];
                if (sProf >= 0) {
                    unsigned long long g0 = sceKernelGetProcessTimeWide();
                    glFinish();
                    g0 = sceKernelGetProcessTimeWide() - g0;
                    sProfUs[sProf] += g0;
                    sProfN[sProf]++;
                    gVitaGpuWait[0] += (unsigned)g0;
                    gVitaGpuWait[1]++;
                    /* each picture of the window: its mode, the chip's time after the last draw, its blended GS
                       primitives (screens, as the game gave them), of them triangles with the colour across the screen */
                    Vita_Log("gpuprof: list %u mode %d: %u us, blended %.1f screens, perspective-colour triangles %.1f\n", gGsFrame, sProf, (unsigned)g0,
                             profFc / (512.0f * 448.0f), sProfQlin / (512.0f * 448.0f));
                }
                sProfQlin = 0.0f;
                if (Vita_InFight()) {
                    pics++;
                }
                if (sProf >= 0 && (pics % 180 == 0 || !Vita_InFight())) {
                    static const char *const kName[PROF_MODES] = {"all", "no blended GS primitives", "as an earlier version (every perspective GS triangle's colour in full precision)",
                                                                  "blended GS primitives untextured", "no perspective-colour GS triangles", "no stage", "no fighters", "nothing",
                                                                  "no big blended GS triangles (over a quarter screen)", "no full-screen sprites", "nothing into work targets"};
                    char line[900];
                    int k, len = snprintf(line, sizeof(line), "vgl: GPU profile, fight pictures to %u: the chip's work after the last draw, ms (pictures):", pics);
                    for (k = 0; k < PROF_MODES && len > 0 && len < (int)sizeof(line); k++) {
                        len += snprintf(line + len, sizeof(line) - (size_t)len, " %s %.1f (%u)%s", kName[k], sProfN[k] ? (double)sProfUs[k] / sProfN[k] / 1000.0 : 0.0, sProfN[k], k < PROF_MODES - 1 ? "," : "");
                    }
                    Vita_Log("%s\n", line);
                    memset(sProfUs, 0, sizeof(sProfUs));
                    memset(sProfN, 0, sizeof(sProfN));
                }
                {   /* (on request: bt3.txt gpuprof=1, or safe bit 4194304 as before; the window by the game's frame,
                       the same moments of the demo fight however many pictures are drawn) */
                    int on = Vita_InFight() && ((gVglSafe & 4194304) || gVitaGpuProf) && gGsFrame >= (unsigned)gVitaGpuProfFrom && gGsFrame < (unsigned)gVitaGpuProfTo;
                    sProf = on ? (int)(pics % PROF_MODES) : -1;
                    sPaceHold = on;
                }
                if (sProf >= 0) {
                    sCovSample = 0; /* (the pictures of the log's samples are drawn whole) */
                }
            }
            {   /* a fight's picture is shown for two blanks, each one as long as the others (the game draws one every
                   two blanks there); elsewhere a picture is shown at the next blank, as before */
                extern uint32_t vsync_interval;
                extern int Vita_InBattle(void);
                vsync_interval = Vita_InBattle() && !(gVglSafe & 524288) && (gVglSafe & 134217728) ? 2 : 0; /* (else pace_cb puts it up) */
            }
            if (sT114Shot != 0) { /* (a picture for the log: this very one, before it is shown) */
                t114_shot(sT114Shot, sT116List);
                if (sFsOn) {
                    fs_dump(sFF + 1);
                    sFsOn = 0;
                }
            }
            {   /* bt3.txt film=<n>: a small picture for the log every n pictures of a fight (a test run looking
                   for something that flickers) */
                extern int Vita_FilmNow(void);
#define filmPics sFilmPics
#define filmN sFilmN
                extern int gVitaFilmMax;
                if (gVitaFilm > 0 && Vita_FilmNow()) { /* the list's signature */
                    char dl[160];
                    int k, len = 0;
                    dl[0] = 0;
                    for (k = 0; k < sSigDefN && len < 140; k++) {
                        len += snprintf(dl + len, sizeof(dl) - (size_t)len, " %08x%08x", (unsigned)(sSigDefT0[k] >> 32), (unsigned)sSigDefT0[k]);
                    }
                    Vita_Log("sig: list %u: GS primitives %u %08x textures %08x, refused %u %u %u %u %u %u %u %u; models %u %08x textures %08x; textures made %u; deferred:%s\n", gGsFrame,
                             sSigGsN, sSigGs, sSigGsT, sSigRej[0], sSigRej[1], sSigRej[2], sSigRej[3], sSigRej[4], sSigRej[5], sSigRej[6], sSigRej[7], sSigStN, sSigSt,
                             sSigStT, sSigTexMade, dl);
                }
                sSigGs = sSigGsN = sSigSt = sSigStN = sSigTexMade = sSigGsT = sSigStT = 0;
                memset(sSigRej, 0, sizeof(sSigRej));
                sSigDefN = 0;
                if (gVitaFilm > 0 && Vita_FilmNow() && sSqN < 60 && (gVitaFilmMax <= 0 || filmN < (unsigned)gVitaFilmMax) && ++filmPics % (unsigned)gVitaFilm == 0) {
                    t114_shot(1000 + filmN++, gGsFrame);
                    {
                        extern void Vita_FilmTaken(void);
                        Vita_FilmTaken(); /* (a burst's picture, filmtrig=) */
                    }
                    if (gVitaWtShot && filmN <= 12) {
                        wt_shots(2000 + (filmN - 1) * 10, gGsFrame);
                    }
                    mid_flush(gGsFrame);
                    if (gVitaFxSweep == 2) { /* the film's pictures each with other effects left out: which makes what */
                        static const int kSeq[7] = {0, 1, 2, 4, 8, 16, 32};
                        Vita_Log("film: picture %u had effects mask %d\n", 1000 + filmN - 1, gVitaFxMask);
                        gVitaFxMask = kSeq[filmN % 7];
                    }
                }
#undef filmPics
#undef filmN
                if (sMidN > 0) { /* (a filmed frame that was not filmed after all) */
                    rt_sync();
                    for (; sMidN > 0; sMidN--) {
                        free(sMidPx[sMidN - 1]);
                    }
                    sMidK = 0;
                }
                sMidOn = 0;
            }
            {   /* on the game's request (vita_main.c: the sweep, twice a pairing) two pictures for the log, two
                   pictures apart: with the work page (the ground shadows), then without */
                extern volatile int gVitaShotReq;
                extern int Vita_InFight(void);
                static int abStep;
                static unsigned abN;
                if (abStep == 0 && gVitaShotReq != 0 && Vita_InFight()) {
                    gVitaShotReq = 0;
                    t114_shot(9000 + abN * 10 + 1, gGsFrame);
                    gVitaWpOff = 1;
                    gVitaOlOff = 1; /* (the outline too) */
                    gVitaFxOff = 1; /* (and the effects' passes) */
                    gVitaClipOff = 1; /* (and the fighters' reject) */
                    sOlShotWant = 1; /* (and the alpha it reads, in the next picture) */
                    abStep = 1;
                } else if (abStep > 0 && ++abStep == 3) {
                    t114_shot(9000 + abN * 10 + 2, gGsFrame);
                    gVitaWpOff = 0;
                    gVitaOlOff = 0;
                    gVitaFxOff = 0;
                    gVitaClipOff = (gVglSafe2 & 512) != 0;
                    abStep = 0;
                    abN++;
                }
            }
            swap_sized(); /* (the menus' size or the fights') */
            t114_emit();
            gProf = 1;
            if (gVitaProfMeasure) { Vita_ProfSwitch(1); }
            T_OUT(3);
        }
        if (sLogFrames > 0) {
            Vita_Log("vgl: frame %u: swapped\n", gGsFrame);
            sLogFrames--;
        }
        frame_begin();
    }
    t114_list_end(!gVitaGsSkip);
}

/* Not in this first back end: the vertex programs as shaders (the interpreter is forced, vita_gfx.c), the movie
   picture, the native effects. */

static int state_of(int ctx, int type, State *d);
static void flush(void);

/* One strip of a vertex program's batch. kind 0: program 0's family (stride 12 floats), 1: program 4, 2: program 6
   (stride 16: position, -, colour, -). */
/* The strips of the vertex programs are collected in several batches at once, one per state: a fighter's model
   sends every strip twice (texture layer, shading layer), so in the game's order the state would change with
   every strip, and a draw per strip is what costs the time (2,000 draws, 0.6 s a picture). All strips of one
   state go in one draw; the batches are drawn in the order they were begun, so the shading layer still follows
   the texture layer it lies on. Any other primitive (the GS's own: HUD, effects) draws them all first. */
#define BINS 32
#define BINS_V 16 /* batches with vertices of their own (the general way): only the first 16 */
#define BIN_VERTS 24000
#define BIN_IDX 72000
static struct { State st; int count, icount; } sBin[BINS];
/* The stage's batches: indices into the vertices kept on the chip, and the matrix they are drawn with. They take
   their turn with the other batches (sBinStage[b] for bin b). */
static StageBin sBinStage[BINS];
static uint32_t sBinHash[BINS]; /* a number for each batch's state: compared before the state itself */
__attribute__((hot)) static uint32_t state_hash(const State *d) {
    const uint32_t *w = (const uint32_t *)(const void *)d;
    uint32_t h = 2166136261u, i;
    for (i = 0; i < sizeof(State) / 4; i++) {
        h = (h ^ w[i]) * 16777619u;
    }
    return h;
}
static uint8_t sBinIsStage[BINS]; /* 1 the stage's, 2 the fighters' */
static float sBinFt[BINS][80];
static float sBinFtS[BINS][192 * 4]; /* the strips' own uniforms, by place */
static uint32_t sBinFtMask[BINS];
static unsigned sBinFtCG[BINS]; /* the strip constants a fighter batch was made from (its shared uniforms are the same while they are) */   /* places taken */
static uint32_t sBinFtMH[BINS]; /* a number for the batch's matrices (looked at before comparing them) */
static uint32_t sBinFtH[BINS]; /* a number for the batch's uniforms */
#define STAGE_IDX (1 << 18)
static uint32_t sStageIdx[STAGE_IDX];
static int sStageIdxUsed;
static V sBinVerts[BINS_V][BIN_VERTS];
static uint16_t sBinIdx[BINS][BIN_IDX];
static int sBinCount;
static unsigned sBinGen; /* counts the times the bins were drawn */
int gVglSamePos;      /* gs_vu1.c: the strip that comes now is the one before with another layer */

static void vj_join(void);
static void flush_bins(void);
unsigned gVitaFlushWhy[20][2];
unsigned gVitaFtMiss[4]; /* a fighter strip found no batch: no batch with its state / its screen matrix / its place free / (room) */ /* for the log: batch flushes by the place that asked (call order in this file): times, batches drawn */
static void flush_bins_c(int why) { gVitaFlushWhy[why][0]++; gVitaFlushWhy[why][1] += (unsigned)sBinCount; flush_bins(); }
/* The fighter batch with the most strips is drawn now and its place given back (the others stay): when the batches
   run out, drawing all of them made ten times as many small draws (fighter batches have no order among themselves,
   being drawn with the depth test, as all batches already are drawn in no particular order). -1: none to give. */
static int flush_one_ft(void) {
    ag_close(); /* the additive group before anything else */
    State keep = sCur;
    int keepHave = sHaveState, keepCount = sVertCount, b, best = -1, most = -1;
    for (b = 0; b < sBinCount; b++) {
        if (sBinIsStage[b] == 3) {
            return b; /* (one already free) */
        }
        if (sBinIsStage[b] == 2 && sBin[b].icount > most) {
            most = sBin[b].icount;
            best = b;
        }
    }
    if (best < 0) {
        return -1;
    }
    gVitaFlushWhy[13][0]++;
    gVitaFlushWhy[13][1]++;
    sCur = sBin[best].st;
    sFtDraw = sBinFt[best];
    sFtSlots = sBinFtS[best];
    sFtSlotsUsed = 32 - __builtin_clz(sBinFtMask[best] | 1);
    sVerts = sVertsMain;
    sVertCount = 1;
    sIdx = sBinIdx[best];
    sIdxCount = sBin[best].icount;
    flush();
    sFtDraw = NULL;
    sIdx = NULL;
    sVerts = sVertsMain;
    sCur = keep;
    sHaveState = keepHave;
    sVertCount = keepCount;
    sBin[best].icount = 0;
    sBinFtMask[best] = 0;
    sBinIsStage[best] = 3; /* free */
    sBinGen++; /* (what pointed at it is looked up again) */
    return best;
}
static void dp_draw_all(void);
static int sDpItemN;
static void flush_bins(void) {
    ag_close(); /* the additive group before anything else */
    State keep = sCur;
    int keepHave = sHaveState, keepCount = sVertCount, b;

    dp_draw_all(); /* the parts drawn directly came before what is in the batches (or no later than it) */
    if (sBinCount == 0) {
        return;
    }
    if (VITA_RARE(sCovSample)) {
        run_add(-2, sBinCount, 0.0f);
    }
    vj_join(); /* (the worker has filled them) */
    for (b = 0; b < sBinCount; b++) {
        if (sBin[b].icount == 0) {
            continue;
        }
        sCur = sBin[b].st;
        am_model_reads(&sCur, 0);
        if (sBinIsStage[b] == 2) {
            sFtDraw = sBinFt[b];
            sFtSlots = sBinFtS[b];
            sFtSlotsUsed = 32 - __builtin_clz(sBinFtMask[b] | 1);
            sVerts = sVertsMain; /* (not read: the fighters' vertices are on the chip) */
            sVertCount = 1;
            sIdx = sBinIdx[b];
            sIdxCount = sBin[b].icount;
            flush();
            sFtDraw = NULL;
            continue;
        }
        if (sBinIsStage[b]) {
            sStageDraw = &sBinStage[b];
            sVertCount = 1; /* (flush wants something to do) */
            flush();
            sStageDraw = NULL;
            continue;
        }
        sVerts = sBinVerts[b];
        sVertCount = sBin[b].count;
        sIdx = sBinIdx[b];
        sIdxCount = sBin[b].icount;
        flush();
    }
    sStageIdxUsed = 0;
    sBinCount = 0;
    sBinLastUnsafe = -1;
    sBinGen++;
    sIdx = NULL;
    sVerts = sVertsMain;
    sCur = keep;
    sHaveState = keepHave;
    sVertCount = keepCount;
    if (sRtOn) {
        rt_kick(); /* (the batches' draws handed over) */
    }
}

#include <arm_neon.h>
/* A strip's vertices are worked out on a thread of its own (the third processor core): the render thread only
   decides where they go (which batch, which places in it) and goes on reading the list. */
typedef struct VJob {
    const float *v;         /* the strip's vertices: in the display list (they stay), or a copy */
    const float (*u)[4];    /* a copy of the constants */
    V *out;
    uint16_t *ix;
    const V *same;          /* the strip before with the same positions, or NULL */
    float zl;               /* depth steps the stage's layer leads by (zl_u), in clip space per w */
    int base;
    uint16_t count;
    uint8_t kind, layer;
    uint8_t page;           /* into the work page: its 256 x 256, not turned over (put() does the same) */
} VJob;

static void vu_transform(const VJob *j) {
    /* What the vertex program does per vertex (the PC back end's vu0.vert / vu4.vert / vu6.vert), on the processor:
       the result is an ordinary vertex in clip space, so every strip of a frame that shares its texture and
       blending goes to the chip in ONE draw. Four floats at a time (NEON). Each vertex is written once, straight
       into its batch; the strip's triangles are indices (a triangle that is not drawn: three times the same). */
    uint8_t bad[1024];
    const float (*u)[4] = j->u;
    const float *vertices = j->v;
    const V *same = j->same;
    const int kind = j->kind, layer = j->layer, base = j->base;
    const uint32_t count = j->count, stride = kind == 2 ? 16 : 12;
    uint32_t i;
    V *o = j->out;
    uint16_t *ix = j->ix;
    const float32x4_t u0 = vld1q_f32(u[0]), u1 = vld1q_f32(u[1]), u2 = vld1q_f32(u[2]), u3 = vld1q_f32(u[3]);
    const float32x4_t u4 = vld1q_f32(u[4]), u5 = vld1q_f32(u[5]), u6 = vld1q_f32(u[6]), u7 = vld1q_f32(u[7]);
    const float32x4_t u8 = vld1q_f32(u[8]), u9 = vld1q_f32(u[9]);
    const float32x4_t u10 = vld1q_f32(u[10]), u11 = vld1q_f32(u[11]), u12 = vld1q_f32(u[12]), u13 = vld1q_f32(u[13]);
    const float k1[4] = {j->page ? 1.0f / 128.0f : 1.0f / 256.0f, j->page ? 1.0f / 128.0f : -1.0f / 224.0f, 2.0f / u[17][2], 0.0f};
    const float k2[4] = {j->page ? -u[17][0] / 128.0f - 1.0f : -u[17][0] / 256.0f - 1.0f, j->page ? -u[17][1] / 128.0f - 1.0f : 1.0f + u[17][1] / 224.0f, -1.0f, 1.0f};
    const float32x4_t K1 = vld1q_f32(k1), K2 = vld1q_f32(k2), big = vdupq_n_f32(1e30f);
    const float32x4_t zero = vdupq_n_f32(0.0f), c255 = vdupq_n_f32(255.0f);
#define PACK(c) vget_lane_u32(vreinterpret_u32_u8(vmovn_u16(vcombine_u16(vmovn_u32(vcvtq_u32_f32(vminq_f32(vmaxq_f32((c), zero), c255))), vdup_n_u16(0)))), 0)
    const uint32_t col15 = PACK(vld1q_f32(u[15])), col16 = PACK(vld1q_f32(u[16]));
    const int clip = kind == 0 && u[19][1] > 0.5f && same == NULL;


    for (i = 0; i < count; i++, o++) {
        const float *v = &vertices[i * stride];
        float32x4_t pos = vld1q_f32(v), sv, out;
        uint32_t col;
        float ss = v[8], st = v[9], sw;
        uint32x4_t okv;
        uint32x2_t ok2;

        bad[i] = 0;
        if (kind == 0) {
            float w = layer == 4 ? 1.0f : v[3];
            if (same == NULL) {
                float32x4_t a = vsubq_f32(pos, u8), bb = vsubq_f32(pos, u9), pa, pb, p;

                pa = vmlaq_n_f32(u3, u0, vgetq_lane_f32(a, 0));
                pa = vmlaq_n_f32(pa, u1, vgetq_lane_f32(a, 1));
                pa = vmlaq_n_f32(pa, u2, vgetq_lane_f32(a, 2));
                pb = vmlaq_n_f32(u7, u4, vgetq_lane_f32(bb, 0));
                pb = vmlaq_n_f32(pb, u5, vgetq_lane_f32(bb, 1));
                pb = vmlaq_n_f32(pb, u6, vgetq_lane_f32(bb, 2));
                p = vmlaq_n_f32(pb, vsubq_f32(pa, pb), w);
                if (clip) { /* programs 0 and 1: a triangle with a vertex outside the clip-test volume is not drawn */
                    float32x4_t cv = vmlaq_n_f32(vld1q_f32(u[23]), vld1q_f32(u[20]), vgetq_lane_f32(p, 0));
                    float cw;
                    cv = vmlaq_n_f32(cv, vld1q_f32(u[21]), vgetq_lane_f32(p, 1));
                    cv = vmlaq_n_f32(cv, vld1q_f32(u[22]), vgetq_lane_f32(p, 2));
                    cw = fabsf(vgetq_lane_f32(cv, 3));
                    if (fabsf(vgetq_lane_f32(cv, 0)) > cw || fabsf(vgetq_lane_f32(cv, 1)) > cw || fabsf(vgetq_lane_f32(cv, 2)) > cw) {
                        bad[i] |= 4; /* (and the position made unusable: the shading layer that takes it over drops it too) */
                    }
                }
                sv = vmlaq_n_f32(u13, u10, vgetq_lane_f32(p, 0));
                sv = vmlaq_n_f32(sv, u11, vgetq_lane_f32(p, 1));
                sv = vmlaq_n_f32(sv, u12, vgetq_lane_f32(p, 2));
                if (layer == 2 && vgetq_lane_f32(sv, 3) < 0.0f) {
                    sv = vnegq_f32(sv);
                }
            } else {
                sv = zero;
            }
            if (layer == 5) {
                col = col16;
                ss = (u[14][0] * v[4] + u[14][1] * v[5] + u[14][2] * v[6] + u[14][3]) * 0.5f + 0.5f;
                st = (u[18][0] * v[4] + u[18][1] * v[5] + u[18][2] * v[6] + u[18][3]) * 0.5f + 0.5f;
            } else if (layer >= 3) {
                float32x4_t c = vld1q_f32(&v[4]);
                if (layer == 4) {
                    c = vsetq_lane_f32(u[15][3], c, 3);
                }
                col = PACK(c);
            } else if (layer != 1) {
                col = col15;
            } else {
                float32x4_t na, nb, n;
                na = vmulq_n_f32(u0, v[4]);
                na = vmlaq_n_f32(na, u1, v[5]);
                na = vmlaq_n_f32(na, u2, v[6]);
                nb = vmulq_n_f32(u4, v[4]);
                nb = vmlaq_n_f32(nb, u5, v[5]);
                nb = vmlaq_n_f32(nb, u6, v[6]);
                n = vmlaq_n_f32(nb, vsubq_f32(na, nb), w);
                col = col16;
                ss = (u[14][0] * vgetq_lane_f32(n, 0) + u[14][1] * vgetq_lane_f32(n, 1) + u[14][2] * vgetq_lane_f32(n, 2) + u[14][3]) * 0.5f + 0.5f;
                st = 0.0f;
            }
            if (layer == 4) {
                uint32_t flag;
                memcpy(&flag, &v[3], 4);
                bad[i] = (flag & 0xFFFF) != 0; /* debris: a flagged vertex does not complete a triangle */
            }
        } else {
            float32x4_t c = vld1q_f32(kind == 1 ? &v[4] : &v[8]);

            sv = vmulq_n_f32(u13, kind == 1 ? v[3] : 1.0f);
            sv = vmlaq_n_f32(sv, u10, v[0]);
            sv = vmlaq_n_f32(sv, u11, v[1]);
            sv = vmlaq_n_f32(sv, u12, v[2]);
            col = PACK(c);
            if (kind == 2) {
                uint32_t flag;
                memcpy(&flag, &v[3], 4);
                bad[i] = (flag & 0xFFFF) != 0;
                ss = ((u[0][0] * v[0] + u[1][0] * v[1] + u[2][0] * v[2] + u[3][0]) * u[8][0] + 1.0f) * 0.5f * u[9][0];
                st = ((u[0][1] * v[0] + u[1][1] * v[1] + u[2][1] * v[2] + u[3][1]) * u[8][0] + 1.0f) * 0.5f * u[9][1];
            }
        }
        sw = vgetq_lane_f32(sv, 3);
        out = same != NULL ? vld1q_f32(&same[i].x) : vmlaq_n_f32(vmulq_f32(sv, K1), K2, sw); /* x, y, z as the chip wants them; w */
        {   /* a position that is not a number (a bone that was not finite): dropped as the reject does */
            uint32x4_t eq = vceqq_f32(out, out);
            uint32x2_t m2 = vand_u32(vget_low_u32(eq), vget_high_u32(eq));
            if ((vget_lane_u32(m2, 0) & vget_lane_u32(m2, 1)) == 0) {
                bad[i] |= 4;
                gVitaBoneBad++;
            }
        }
        if (bad[i] & 4) {
            out = vdupq_n_f32(2e30f);
        }
        if (j->zl != 0.0f && same == NULL) { /* a stage layer laid on the stage: it leads (zl_u) */
            out = vsetq_lane_f32(vgetq_lane_f32(out, 2) + j->zl * sw, out, 2);
        }
        if ((kind == 0 && layer == 2) || (gVitaZClamp && sw > 0.0f && same == NULL)) {
            /* the shadow camera's depth is not kept in range: kept, not clipped. And (bt3.txt zclamp=1) any vertex's:
               the GS holds a depth past its buffer's range at the range's end, where the chip would cut the triangle */
            float z = vgetq_lane_f32(out, 2);
            out = vsetq_lane_f32(z < -sw ? -sw : z > sw ? sw : z, out, 2);
        }
        vst1q_f32(&o->x, out);
        memcpy(o->c, &col, 4);
        o->s = ss;
        o->t = st;
        o->q = 1.0f;
        okv = vcaltq_f32(out, big); /* not a number the chip should see: none of its triangles */
        ok2 = vand_u32(vget_low_u32(okv), vget_high_u32(okv));
        if ((vget_lane_u32(ok2, 0) & vget_lane_u32(ok2, 1)) == 0) {
            bad[i] |= 2;
        }
    }
    for (i = 0; i + 2 < count; i++, ix += 3) { /* strip -> triangles */
        if ((bad[i + 2] & 1) || ((bad[i] | bad[i + 1] | bad[i + 2]) & 2)) {
            ix[0] = ix[1] = ix[2] = (uint16_t)base;
            continue;
        }
        ix[0] = (uint16_t)(base + (int)i);
        ix[1] = (uint16_t)(base + (int)i + 1);
        ix[2] = (uint16_t)(base + (int)i + 2);
    }
}

#define VJ_MAX 4096
/* The constants and (when they do not stay in the list) the vertices travel in a small ring that is used over
   and over: on this machine it is memory that has never been in the cache that costs, and a fight's strips are
   3 MB a picture. A piece is free again when the strip after the one it was made for is done. */
#define VJ_ARENA (256u << 10)
static VJob sVj[VJ_MAX];
static unsigned sVjFrom[VJ_MAX];           /* per job: the ring position its data begins at */
static volatile unsigned sVjHead, sVjTail; /* the worker takes at head, the render thread adds at tail */
static volatile unsigned sVjFree;          /* ring positions below this are free (the worker moves it) */
static volatile unsigned sVjClaim;         /* the next job to be taken (by the worker, or by the render thread while it waits) */
static volatile unsigned sVjDone[VJ_MAX];  /* per job: its number + 1 once done */
static volatile int sVjLock;               /* moving sVjHead / sVjFree */
static uint8_t sVjArena[VJ_ARENA + 64] __attribute__((aligned(16)));
static unsigned sVjUsed, sVjGen;           /* sVjUsed: the ring position to write at (counts on and on) */
static const void *sVjLastU;
static unsigned sVjLastUAt;
static int sVjOn;
static int sSameU;                /* the constants handed over now are those of the strip before (its second layer) */
unsigned gVitaUpSkipped;          /* gs_core.c: uploads not looked at in frames that are not drawn */
volatile unsigned char gProfWork; /* 1 while the worker has a strip in hand */
unsigned gVitaVj[4];              /* for the log: strips given to the worker, read in place, copied; times waited for it */

/* Take the next job and do it; 0 when there is none. Both threads take jobs in order; a job is finished out of
   order at worst, so the head (and the free part of the ring) only moves over a row of finished jobs. A job that
   reuses the positions of the job before waits for that one. */
__attribute__((hot)) static int vj_help(void) {
    unsigned c = sVjClaim, i;
    const VJob *j;

    if (c == sVjTail || !__sync_bool_compare_and_swap(&sVjClaim, c, c + 1)) {
        return c != sVjTail;
    }
    j = &sVj[c & (VJ_MAX - 1)];
    if (j->same != NULL) {
        while (sVjHead != c && sVjDone[(c - 1) & (VJ_MAX - 1)] != c) {
        }
        __sync_synchronize();
    }
    vu_transform(j);
    __sync_synchronize();
    sVjDone[c & (VJ_MAX - 1)] = c + 1;
    while (__sync_lock_test_and_set(&sVjLock, 1)) {
    }
    for (i = sVjHead; i != sVjTail && sVjDone[i & (VJ_MAX - 1)] == i + 1; i++) {
        sVjFree = sVjFrom[i & (VJ_MAX - 1)];
    }
    __sync_synchronize();
    sVjHead = i;
    __sync_lock_release(&sVjLock);
    return 1;
}

static int vj_thread(SceSize args, void *argp) {
    unsigned idle = 0;

    uint32_t last[5];
    int working = 0;

    (void)args; (void)argp;
    Vita_PerfThreadStart();
    for (;;) {
        if (sVjClaim != sVjTail) {
            if (!working && gVitaPerfOn) {
                Vita_PerfRead(last);
            }
            working = 1;
            gProfWork = 1;
            vj_help();
            idle = 0;
        } else {
            gProfWork = 0;
            if (working && gVitaPerfOn) {
                Vita_PerfAccum(last, gVitaPerfWork); /* (only the time it had work: its waiting is not counted) */
            }
            working = 0;
            if (++idle > 4000) { /* nothing for a while: sleep in between (a 3D part of a list starts 0.1 ms late at worst) */
                sceKernelDelayThread(idle > 40000 ? 1000 : 100);
            }
        }
    }
    return 0;
}

/* Everything given to the worker is done (before the batches are drawn, and before the list ends): the render
   thread does jobs itself meanwhile, instead of waiting. */
static void vj_join(void) {
    if (!sVjOn) {
        return;
    }
    if (sVjHead != sVjTail) {
        gVitaVj[3]++;
    }
    while (vj_help()) {
    }
    while (sVjHead != sVjTail) {
    }
    __sync_synchronize();
}

/* `size` bytes in a row in the ring (waits for the worker if it is full). */
static uint8_t *vj_room(unsigned size) {
    unsigned spins = 0;

    size = (size + 15) & ~15u;
    if ((sVjUsed & (VJ_ARENA - 1)) + size > VJ_ARENA) {
        sVjUsed = (sVjUsed + VJ_ARENA) & ~(VJ_ARENA - 1); /* (not over the end) */
    }
    while (sVjUsed + size - sVjFree > VJ_ARENA) {
        if (sVjHead == sVjTail) { /* nothing in work: all of it is free */
            sVjFree = sVjUsed;
            sVjLastU = NULL;
            sVjGen++;
            break;
        }
        if (!vj_help() && ++spins > 2000) {
            sceKernelDelayThread(50);
        }
    }
    sVjUsed += size;
    return &sVjArena[(sVjUsed - size) & (VJ_ARENA - 1)];
}

/* ---- The stage on the graphics chip ----
   The stage's strips (vertex program 4) come from the stage's model, which lies in the game's memory and does not
   change: the same vertices from the same place every frame, only the camera moves. The first time a strip is seen
   its vertices are written into a buffer of the chip's (as they are: position, colour, texture coordinates); from
   then on the strip costs its indices, and the chip does the vertex program's arithmetic. A strip whose data lies
   in the frame's own list (rewritten every frame) is drawn the other way. */
unsigned gVitaFighter[4];   /* for the log: fighter strips (layers 0, 1), of them outside the list, constant changes, vertices */
unsigned gVitaStage[3];     /* for the log: strips drawn this way, strips written to the chip, vertices kept */

__attribute__((hot)) static uint32_t stage_sig(const float *v, uint32_t count) {
    uint32_t h = count * 0x9E3779B1u, w, i, k;
    const uint32_t idx[3] = {0, count / 2, count - 1};
    for (k = 0; k < 3; k++) {
        for (i = 0; i < 12; i++) {
            memcpy(&w, &v[idx[k] * 12 + i], 4);
            h = (h ^ w) * 0x85EBCA6Bu;
            h ^= h >> 13;
        }
    }
    return h;
}

__attribute__((hot)) static int stage_draw(const State *d, const float *v, uint32_t count, const float (*u)[4]) {
    extern int Port_DmaInList(uint32_t a), Port_InSnap(const void *p);
    uint32_t h, sig, at, i, need = (count - 2) * 3;
    int b, k;
    float m[24];

    if (Port_DmaInList((uint32_t)(uintptr_t)v) || Port_InSnap(v) || count > 1024) {
        return 0;
    }
    h = ((uint32_t)(uintptr_t)v * 0x9E3779B1u ^ count) >> 18;
    for (k = 0; k < 8; k++, h = (h + 1) & 16383) {
        if (sStageCache[h].src == NULL || (sStageCache[h].src == v && sStageCache[h].count == count)) {
            break;
        }
    }
    if (k < 8 && sStageCache[h].src != NULL && gGsFrame - sStageCache[h].seen < 32u) {
        sig = sStageCache[h].sig; /* (looked at lately: the vertices are not read again) */
    } else {
        sig = stage_sig(v, count);
        if (k < 8 && sStageCache[h].src != NULL && sStageCache[h].sig == sig) {
            sStageCache[h].seen = gGsFrame;
        }
    }
    if (k == 8 || sStageCache[h].src == NULL || sStageCache[h].sig != sig) {
        uint8_t *map;
        if (k == 8) {
            h = ((uint32_t)(uintptr_t)v * 0x9E3779B1u ^ count) >> 18; /* (crowded: the first place is taken over) */
        }
        if (sStageUsed + count > STAGE_VERTS) {
            return 0; /* full: the rest of the stage is drawn the other way */
        }
        map = sStageVboPtr;
        if (map == NULL) {
            return 0;
        }
        for (i = 0; i < count; i++) {
            const float *s = &v[i * 12];
            float o[6];
            uint8_t c[4];
            int j;
            memcpy(o, s, 16);          /* x y z w */
            o[4] = s[8];               /* s t */
            o[5] = s[9];
            for (j = 0; j < 4; j++) { /* the colour as the strip path makes it: 0..255, cut, 0x80 = 1 */
                float f = s[4 + j];
                c[j] = (uint8_t)(f < 0.0f ? 0 : f > 255.0f ? 255 : (int)f);
            }
            memcpy(map + (size_t)(sStageUsed + i) * 32, o, 24);
            memcpy(map + (size_t)(sStageUsed + i) * 32 + 24, c, 4);
        }
        sStageCache[h].src = v;
        sStageCache[h].count = count;
        sStageCache[h].sig = sig;
        sStageCache[h].at = sStageUsed;
        sStageCache[h].seen = gGsFrame;
        sStageUsed += count;
        gVitaStage[1]++;
        gVitaStage[2] = sStageUsed;
    }
    ZM(13);
    at = sStageCache[h].at;
    if (VITA_RARE(gVitaFilm > 0)) { /* (the signature) */
        uint32_t w;
        SIG_MIX(sSigSt, (uint32_t)(uintptr_t)v);
        SIG_MIX(sSigSt, count);
        SIG_MIX(sSigStT, d->tme ? sSigGenCur : 0u);
        SIG_MIX(sSigSt, (uint32_t)d->blend | (uint32_t)d->src << 4 | (uint32_t)d->dst << 8 | (uint32_t)d->date << 12 | (uint32_t)d->sop << 16 | (uint32_t)d->zfunc << 20 | (uint32_t)d->zwrite << 24);
        memcpy(&w, &u[10][0], 4); SIG_MIX(sSigSt, w);
        memcpy(&w, &u[11][1], 4); SIG_MIX(sSigSt, w);
        memcpy(&w, &u[13][3], 4); SIG_MIX(sSigSt, w);
        sSigStN++;
    }
    memcpy(&m[0], u[10], 64);
    m[16] = 1.0f / 256.0f; m[17] = -1.0f / 224.0f; m[18] = 2.0f / u[17][2]; m[19] = 0.0f;
    m[20] = -u[17][0] / 256.0f - 1.0f; m[21] = 1.0f + u[17][1] / 224.0f; m[22] = -1.0f; m[23] = 1.0f;
    uint32_t dh = state_hash(d);
    for (b = 0; b < sBinCount && !(sBinIsStage[b] == 1 && sBinHash[b] == dh && memcmp(&sBin[b].st, d, sizeof(*d)) == 0 && memcmp(sBinStage[b].m, m, sizeof(m)) == 0 &&
                                   sBinStage[b].ix + sBinStage[b].icount == &sStageIdx[sStageIdxUsed]); b++) {
    }
    if (b < sBinCount && !bin_join_ok(b, d)) {
        b = sBinCount; /* its place among the draws kept */
    }
    if (sStageIdxUsed + (int)need > STAGE_IDX) {
        flush_bins_c(2);
        b = sBinCount;
    }
    if (b == sBinCount) {
        if (sBinCount == BINS) {
            flush_bins_c(3);
        }
        b = sBinCount++;
        bin_made(b, d);
        sBin[b].st = *d;
        sBin[b].count = 0;
        sBin[b].icount = 1; /* (not empty: flush_bins skips empty ones) */
        sBinIsStage[b] = 1;
        sBinHash[b] = dh;
        memcpy(sBinStage[b].m, m, sizeof(m));
        sBinStage[b].ix = &sStageIdx[sStageIdxUsed];
        sBinStage[b].icount = 0;
    }
    ZM(14);
    for (i = 0; i + 2 < count; i++) {
        uint32_t *ix = &sStageIdx[sStageIdxUsed];
        ix[0] = at + i;
        ix[1] = at + i + 1;
        ix[2] = at + i + 2;
        sStageIdxUsed += 3;
    }
    sBinStage[b].icount += (int)need;
    gVitaStage[0]++;
    ZM(15);
    return 1;
}

unsigned gVitaFt[3];
unsigned gVitaGpuWait[2]; /* for the log: microseconds the chip still worked at the swap, times measured */ /* for the log: fighter strips drawn from the chip, strips written to it, vertices kept */
__attribute__((hot)) static int fighter_draw(const State *d, int layer, const float *v, uint32_t count, const float (*u)[4]) {
    if (1) { return 0; } /* (the fighters' shader now wants each strip's place in its vertices: only the planned path, chip_emit, gives it) */
    extern int Port_DmaInList(uint32_t a), Port_InSnap(const void *p);
    extern int Vita_InFight(void);
    static struct { unsigned gen; int b; } lastF[2];
    uint32_t h, sig, at, i, need = (count - 2) * 3, dh, uh;
    int b, k;
    float m[80];

    if (sFtProg == 0 || !Vita_InFight() || Port_DmaInList((uint32_t)(uintptr_t)v) || Port_InSnap(v) || count > 1024) {
        return 0;
    }
    h = ((uint32_t)(uintptr_t)v * 0x9E3779B1u ^ count) >> 19;
    for (k = 0; k < 8; k++, h = (h + 1) & 8191) {
        if (sFtCache[h].src == NULL || (sFtCache[h].src == v && sFtCache[h].count == count)) {
            break;
        }
    }
    /* the model's vertices are read again (a few of them, to see they are still the same) only every 32 frames:
       reading them is what costs (memory that is not in the cache) */
    sig = 0;
    if (k < 8 && sFtCache[h].src != NULL && gGsFrame - sFtCache[h].seen < 32u) {
        sig = sFtCache[h].sig;
    } else {
        sig = stage_sig(v, count);
        if (k < 8 && sFtCache[h].src != NULL && sFtCache[h].sig == sig) {
            sFtCache[h].seen = gGsFrame;
        }
    }
    if (k == 8 || sFtCache[h].src == NULL || sFtCache[h].sig != sig) {
        uint8_t *map;
        if (k == 8) {
            h = ((uint32_t)(uintptr_t)v * 0x9E3779B1u ^ count) >> 19;
        }
        if (sFtUsed + count > FT_VERTS) {
            return 0;
        }
        glBindBuffer(GL_ARRAY_BUFFER, sFtVbo);
        map = (uint8_t *)wt_map(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
        if (map == NULL) {
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            return 0;
        }
        for (i = 0; i < count; i++) {
            memcpy(map + (size_t)(sFtUsed + i) * 40, &v[i * 12], 40);
        }
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        sFtCache[h].src = v;
        sFtCache[h].count = count;
        sFtCache[h].sig = sig;
        sFtCache[h].at = sFtUsed;
        sFtCache[h].seen = gGsFrame;
        sFtUsed += count;
        gVitaFt[1]++;
        gVitaFt[2] = sFtUsed;
    }
    at = sFtCache[h].at;
    ZM(7);
    {   /* the uniforms as a number: the batch of the strip before (same bones, same layer) is found without
           building and comparing them */
        static uint32_t base;
        if (!sSameU) {
            const uint32_t *w = (const uint32_t *)(const void *)u;
            base = 2166136261u;
            for (i = 0; i < 18 * 4; i++) {
                base = (base ^ w[i]) * 16777619u;
            }
        }
        uh = base ^ (uint32_t)layer * 0x9E3779B1u;
    }
    dh = state_hash(d);
    b = lastF[layer].b;
    if (!(lastF[layer].gen == sBinGen && b < sBinCount && sBinIsStage[b] == 2 && sBinFtH[b] == uh && sBinHash[b] == dh &&
          sBin[b].icount + (int)need <= BIN_IDX && memcmp(&sBin[b].st, d, sizeof(*d)) == 0)) {
        memcpy(&m[0], u[0], 14 * 16);
        memcpy(&m[56], u[14], 16);
        for (k = 0; k < 4; k++) {
            float c = u[layer == 0 ? 15 : 16][k];
            m[60 + k] = (c < 0.0f ? 0.0f : c > 255.0f ? 255.0f : (float)(int)c) / 128.0f;
        }
        m[64] = 1.0f / 256.0f; m[65] = -1.0f / 224.0f; m[66] = 2.0f / u[17][2]; m[67] = 0.0f;
        m[68] = -u[17][0] / 256.0f - 1.0f; m[69] = 1.0f + u[17][1] / 224.0f; m[70] = -1.0f; m[71] = 1.0f;
        m[72] = (float)layer; m[73] = m[74] = m[75] = 0.0f;
        m[76] = m[77] = m[78] = m[79] = 0.0f;
        for (b = 0; b < sBinCount && !(sBinIsStage[b] == 2 && sBinFtH[b] == uh && sBinHash[b] == dh && memcmp(&sBin[b].st, d, sizeof(*d)) == 0 &&
                                       sBin[b].icount + (int)need <= BIN_IDX && memcmp(sBinFt[b], m, sizeof(m)) == 0); b++) {
        }
        if (b == sBinCount) {
            if (sBinCount == BINS) {
                flush_bins_c(4);
            }
            b = sBinCount++;
            sBin[b].st = *d;
            sBin[b].count = 0;
            sBin[b].icount = 0;
            sBinIsStage[b] = 2;
            sBinHash[b] = dh;
            sBinFtH[b] = uh;
            memcpy(sBinFt[b], m, sizeof(m));
        }
        lastF[layer].b = b;
        lastF[layer].gen = sBinGen;
    }
    ZM(8);
    {
        uint16_t *ix = &sBinIdx[b][sBin[b].icount];
        for (i = 0; i + 2 < count; i++, ix += 3) {
            ix[0] = (uint16_t)(at + i);
            ix[1] = (uint16_t)(at + i + 1);
            ix[2] = (uint16_t)(at + i + 2);
        }
    }
    sBin[b].icount += (int)need;
    gVitaFt[0]++;
    ZM(9);
    return 1;
}


/* ---- With the list split over two threads (gs_core.c), the bookkeeping of the models kept on the chip is done on
   the front end's thread, which has time to spare: which place in the chip's buffer a strip has (or gets, with
   its vertices to be written there), its triangles as indices, and a number for its uniforms. The back end only
   writes what is new and appends the indices to a batch. The caches belong to the front end then. ---- */
typedef struct ChipPlan { uint32_t mode, at, count, uh, upload, slot, ioff; } ChipPlan; /* mode 1 fighter, 2 stage; ioff: the fighter strip's indices in sFtIbo */
extern int gGsSplit;
const void *gVglPlan; /* gs_core.c sets it for the strip being drawn (the back end) */

/* The front end: a fight has ended (the stage and fighters will be different): 1 if the caches were let go, and
   the back end must wait for the chip before anything is written over (a record in the queue says so). */
unsigned gVitaChipGen = 1;
unsigned gVglConstGen; /* counts the times the back end took new strip constants */
/* Also in a fight, when the fighters' copies have nearly filled their buffers: each fighter brought in (a team
   battle's next member, a transformation, a fusion) adds its model, and the ones gone stay. Full, the new ones were
   drawn the slow way, every frame: a team battle fell to 10-12 pictures a second from the third change of fighter.
   Everything is let go then and made again from the next picture (a moment's stutter); not more than once in ten
   seconds, so that two models too large for the buffers together do not make it every picture. */
#define DP_IBO_BYTES (4u << 20)
int Vita_ChipResetDue(void) {
    extern int Vita_InBattle(void);
    static int was;
    static unsigned lastFull;
    int now = Vita_InBattle(), r = 0, full = 0;
    if (now && (sFtUsed > FT_VERTS - FT_VERTS / 8 || sFtIdxUsed > FT_IDX - FT_IDX / 8 || sDpIboUsed > DP_IBO_BYTES - DP_IBO_BYTES / 8) &&
        (lastFull == 0 || gGsFrame - lastFull > 600)) {
        full = 1;
        lastFull = gGsFrame;
        Vita_Log("vgl: frame %u: the chip's copies nearly full (fighters %u of %u vertices, %u of %u indices; kept parts %u KB): let go\n",
                 gGsFrame, (unsigned)sFtUsed, (unsigned)FT_VERTS, (unsigned)sFtIdxUsed, (unsigned)FT_IDX, (unsigned)(sDpIboUsed >> 10));
    }
    if (((was && !now) || full) && (sStageUsed != 0 || sFtUsed != 0)) {
        memset(sStageCache, 0, sizeof(sStageCache));
        sStageUsed = 0;
        memset(sFtCache, 0, sizeof(sFtCache));
        sFtUsed = 0;
        sFtIdxUsed = 0;
        r = 1;
        gVitaChipGen++; /* (gs_core.c: the replayed calls refer to the buffers let go) */
    }
    was = now;
    return r;
}

/* ---- what makes parts of the fighters go missing. At chosen moments of the fight (counted in the
   game's lists) six drawn lists in a row are pictured, all without the blended GS triangles (the effects over the
   fighters), each with one more thing changed: 2 no calls replayed (every model call read the long way), 4 the
   fighters not on the chip (their vertices worked out by the processor, drawn in batches), 8 the kept parts walked
   (not drawn from their kept form), 16 the fighters' vertices looked at every time. The list reader decides, as it
   reads the list, and tells the drawing thread in the list's records (GR_T116). ---- */
/* (one picture at each of the moments two tests showed parts missing, without the blended GS
   triangles so that the fighters are seen) */
static const unsigned kT116[] = {0}; /* (none: 0 is never a fight list) */
static const unsigned kT116Seq[1] = {1};
int gT116Fe;                 /* the list being read: its changes */
int gT116Be;                 /* the list being drawn: its changes */
unsigned gT116Stale[2];      /* change 16: fighter strips whose vertices had changed unseen, strips looked at */
static unsigned sFeFF;       /* the fight's lists read */
static int sFeMoment = -1, sFeSeq;
static unsigned sFeMarkN;    /* pictures asked for by the game's thread so far */
void Vita_T116FeList(int drawn, int mark, unsigned *var, unsigned *shot, unsigned *list) {
    extern int Vita_InFight(void);
    unsigned k;
    *var = *shot = *list = 0;
    gT116Fe = 0;
    if (!Vita_InFight() || (gVglSafe & 67108864)) {
        sFeFF = 0;
        sFeMoment = -1;
        return;
    }
    sFeFF++;
    if (mark != 0 && drawn) { /* (the white shards; port_fast.c marks the list when the speed lines draw) */
        *shot = 9000 + sFeMarkN++ * 10 + (unsigned)mark;
        *list = sFeFF;
        return;
    }
    for (k = 0; sFeMoment < 0 && k < sizeof(kT116) / sizeof(kT116[0]); k++) {
        if (sFeFF == kT116[k]) {
            sFeMoment = (int)k;
            sFeSeq = 0;
        }
    }
    if (sFeMoment < 0 || !drawn) {
        return; /* (a list not drawn takes no change: the next drawn one does) */
    }
    *var = kT116Seq[sFeSeq];
    *shot = kT116[sFeMoment] * 10 + (unsigned)sFeSeq;
    *list = sFeFF;
    gT116Fe = (int)*var;
    gT116Stale[0] = gT116Stale[1] = 0;
    if (++sFeSeq == (int)(sizeof(kT116Seq) / sizeof(kT116Seq[0]))) {
        sFeMoment = -1;
    }
}
void Vita_T116FeListEnd(void) {
    if (gT116Fe & 16) {
        Vita_Log("t116: list %u: fighter strips whose vertices had changed unseen: %u of %u\n", sFeFF, gT116Stale[0], gT116Stale[1]);
    }
    gT116Fe = 0;
}

/* The front end: can this strip be drawn from the chip's buffers? Fills `p` and returns the bytes it needs
   behind it (vertices to write, then the indices), or -1. */
int Vita_ChipPlan(int which, int layer, const float *v, uint32_t count, const float *consts, int stable, ChipPlan *p) {
    extern int Port_DmaInList(uint32_t a), Port_InSnap(const void *pp);
    extern int Vita_InBattle(void);
    uint32_t h, sig, need = (count - 2) * 3;
    int k;
    if (!stable || count < 3 || count > 1024 || Port_DmaInList((uint32_t)(uintptr_t)v) || Port_InSnap(v)) {
        return -1;
    }
    if (which == 0 && layer <= 2 && sFtProg != 0 && Vita_InBattle() && !(gT116Fe & 4)) { /* (the fighters not on the chip; layer 2, the silhouettes, too) */
        h = ((uint32_t)(uintptr_t)v * 0x9E3779B1u ^ count) >> 19;
        for (k = 0; k < 8; k++, h = (h + 1) & 8191) {
            if (sFtCache[h].src == NULL || (sFtCache[h].src == v && sFtCache[h].count == count)) {
                break;
            }
        }
        if (gT116Fe & 16) { /* (the vertices looked at every time; those changed unseen counted) */
            sig = stage_sig(v, count);
            gT116Stale[1]++;
            if (k < 8 && sFtCache[h].src != NULL && gGsFrame - sFtCache[h].seen < 32u && sFtCache[h].sig != sig) {
                gT116Stale[0]++;
            }
            if (k < 8 && sFtCache[h].src != NULL && sFtCache[h].sig == sig) {
                sFtCache[h].seen = gGsFrame;
            }
        } else if (k < 8 && sFtCache[h].src != NULL && gGsFrame - sFtCache[h].seen < 32u) {
            sig = sFtCache[h].sig; /* (of 15000 strips a run looked at again, none had changed) */
        } else {
            sig = stage_sig(v, count);
            if (k < 8 && sFtCache[h].src != NULL && sFtCache[h].sig == sig) {
                sFtCache[h].seen = gGsFrame;
            }
        }
        p->upload = 0;
        if (k == 8 || sFtCache[h].src == NULL || sFtCache[h].sig != sig) {
            if (k == 8) {
                h = ((uint32_t)(uintptr_t)v * 0x9E3779B1u ^ count) >> 19;
            }
            if (sFtUsed + count > FT_VERTS || sFtIdxUsed + need > FT_IDX) {
                return -1;
            }
            sFtCache[h].src = v; sFtCache[h].count = count; sFtCache[h].sig = sig; sFtCache[h].at = sFtUsed; sFtCache[h].seen = gGsFrame;
            sFtCache[h].slot = sFtSlotNext++ & 15;
            sFtCache[h].ioff = sFtIdxUsed;
            sFtUsed += count;
            sFtIdxUsed += need;
            p->upload = 1;
        }
        if (!sDsOn) { /* (the batches' way only) */
            const uint32_t *w = (const uint32_t *)(const void *)consts;
            uint32_t uh = 2166136261u ^ (uint32_t)layer * 0x9E3779B1u, i;
            for (i = 0; i < 96; i++) {
                uh = (uh ^ w[i]) * 16777619u;
            }
            p->uh = uh;
        } else {
            p->uh = 0;
        }
        p->mode = 1;
        p->slot = sFtCache[h].slot;
        p->ioff = sFtCache[h].ioff;
        p->at = sFtCache[h].at;
        p->count = count;
        return (int)((p->upload ? count * 40 : 0) + ((need * 2 + 3) & ~3u));
    }
    if (which == 4 && sStageProg != 0) {
        h = ((uint32_t)(uintptr_t)v * 0x9E3779B1u ^ count) >> 18;
        for (k = 0; k < 8; k++, h = (h + 1) & 16383) {
            if (sStageCache[h].src == NULL || (sStageCache[h].src == v && sStageCache[h].count == count)) {
                break;
            }
        }
        if (k < 8 && sStageCache[h].src != NULL && gGsFrame - sStageCache[h].seen < 32u) {
            sig = sStageCache[h].sig;
        } else {
            sig = stage_sig(v, count);
            if (k < 8 && sStageCache[h].src != NULL && sStageCache[h].sig == sig) {
                sStageCache[h].seen = gGsFrame;
            }
        }
        p->upload = 0;
        if (k == 8 || sStageCache[h].src == NULL || sStageCache[h].sig != sig) {
            if (k == 8) {
                h = ((uint32_t)(uintptr_t)v * 0x9E3779B1u ^ count) >> 18;
            }
            if (sStageUsed + count > STAGE_VERTS) {
                return -1;
            }
            sStageCache[h].src = v; sStageCache[h].count = count; sStageCache[h].sig = sig; sStageCache[h].at = sStageUsed; sStageCache[h].seen = gGsFrame;
            sStageUsed += count;
            p->upload = 1;
        }
        p->mode = 2;
        p->at = sStageCache[h].at;
        p->count = count;
        p->uh = 0;
        return (int)((p->upload ? count * 32 : 0) + need * 4);
    }
    return -1;
}

/* The front end: what the plan needs behind it (the vertices as the chip keeps them, the indices). */
void Vita_ChipFill(const ChipPlan *p, const float *v, uint8_t *dst) {
    uint32_t i, count = p->count;
    if (p->mode == 1) {
        if (p->upload) {
            float sl = (float)p->slot;
            for (i = 0; i < count; i++) {
                memcpy(dst + i * 40, &v[i * 12], 40);
                memcpy(dst + i * 40 + 28, &sl, 4); /* (the normal's 4th word: unused; the strip's place instead) */
            }
            dst += count * 40;
        }
        {
            uint16_t *ix = (uint16_t *)(void *)dst;
            for (i = 0; i + 2 < count; i++, ix += 3) {
                ix[0] = (uint16_t)(p->at + i); ix[1] = (uint16_t)(p->at + i + 1); ix[2] = (uint16_t)(p->at + i + 2);
            }
        }
        return;
    }
    if (p->upload) {
        for (i = 0; i < count; i++) {
            const float *sv = &v[i * 12];
            float o[6];
            uint8_t c[4];
            int j;
            memcpy(o, sv, 16);
            o[4] = sv[8];
            o[5] = sv[9];
            for (j = 0; j < 4; j++) {
                float f = sv[4 + j];
                c[j] = (uint8_t)(f < 0.0f ? 0 : f > 255.0f ? 255 : (int)f);
            }
            memcpy(dst + i * 32, o, 24);
            memcpy(dst + i * 32 + 24, c, 4);
        }
        dst += count * 32;
    }
    {
        uint32_t *ix = (uint32_t *)(void *)dst;
        for (i = 0; i + 2 < count; i++, ix += 3) {
            ix[0] = p->at + i; ix[1] = p->at + i + 1; ix[2] = p->at + i + 2;
        }
    }
}

/* The back end: a fighter strip planned with upload (its first sight): its vertices into the chip's buffer and its
   indices (behind them in the record) into the chip's index buffer at the places the plan gave them. */
static void ft_upload(const ChipPlan *p) {
    const uint8_t *src = (const uint8_t *)p + 32;
    uint8_t *map;
    map = sFtVboPtr;
    if (map != NULL) {
        memcpy(map + (size_t)p->at * 40, src, p->count * 40);
    }
    if (sFtIboPtr != NULL) {
        memcpy(sFtIboPtr + p->ioff, src + p->count * 40, (p->count - 2) * 3 * 2);
    }
    gVitaFt[1]++;
    gVitaFt[2] = p->at + p->count;
}

/* The back end: draw a planned strip (its state `d`, constants `u`). */
static void chip_emit(const State *d, int layer, const float (*u)[4], const ChipPlan *p) {
    const uint8_t *extra = (const uint8_t *)p + 32; /* (the plan takes 32 bytes in the record) */
    uint32_t need = (p->count - 2) * 3, dh = state_hash(d);
    int b, k;
    if (p->mode == 1) {
        static struct { unsigned gen; int b; } lastF[3];
        const int L3 = layer == 1 ? 1 : layer == 2 ? 2 : 0; /* layer 2: the silhouettes for the ground shadows (program 2) */
        uint32_t uh = p->uh, mh;
        float m[80];
        {   /* (the GS's offsets and depth range come from the back end's registers) */
            const uint32_t *w = (const uint32_t *)(const void *)u[17];
            uh = (uh ^ w[0]) * 16777619u; uh = (uh ^ w[1]) * 16777619u; uh = (uh ^ w[2]) * 16777619u;
        }
        if (p->upload) {
            ft_upload(p);
            extra += p->count * 40;
        }
        ZM(25);
        {   /* strips that share the batch's uniforms (screen matrix, offsets, layer) go in one batch, up to 8 of them,
               each in the place its vertices name (given when they were first written), with its own bones there */
            /* the strip's own uniforms and the batch's shared ones come from the constants alone (and the GS offsets):
               a part's strips share them, so they are worked out once per part and layer (gVglConstGen: gs_core.c) */
            static struct { unsigned gen; int valid, target; uint32_t u17[3]; float m[80], sb[48]; uint32_t ch; } memo[3];
            float sb[48];
            uint32_t bit = 1u << (p->slot & 15), ch;
            const uint32_t *u17 = (const uint32_t *)(const void *)u[17];
            int fresh = 0;
            if (!(memo[L3].valid && memo[L3].gen == gVglConstGen && memo[L3].target == d->target && memo[L3].u17[0] == u17[0] && memo[L3].u17[1] == u17[1] && memo[L3].u17[2] == u17[2])) {
                float *mm = memo[L3].m, *sbm = memo[L3].sb;
                uint32_t h = 2166136261u;
                memcpy(sbm, u[0], 10 * 16);
                memcpy(&sbm[40], u[14], 16);
                for (k = 0; k < 4; k++) {
                    float c = u[layer == 1 ? 16 : 15][k];
                    sbm[44 + k] = (c < 0.0f ? 0.0f : c > 255.0f ? 255.0f : (float)(int)c) / 128.0f;
                }
                memset(mm, 0, sizeof(memo[0].m));
                memcpy(&mm[40], u[10], 4 * 16);
                if (d->target) { /* into the work page: its 256 x 256, not turned over (as put() and vu_transform) */
                    mm[64] = 1.0f / 128.0f; mm[65] = 1.0f / 128.0f; mm[66] = 2.0f / u[17][2]; mm[67] = 0.0f;
                    mm[68] = -u[17][0] / 128.0f - 1.0f; mm[69] = -u[17][1] / 128.0f - 1.0f; mm[70] = -1.0f; mm[71] = 1.0f;
                } else {
                    mm[64] = 1.0f / 256.0f; mm[65] = -1.0f / 224.0f; mm[66] = 2.0f / u[17][2]; mm[67] = 0.0f;
                    mm[68] = -u[17][0] / 256.0f - 1.0f; mm[69] = 1.0f + u[17][1] / 224.0f; mm[70] = -1.0f; mm[71] = 1.0f;
                }
                mm[72] = (float)layer;
                {
                    const uint32_t *mw = (const uint32_t *)(const void *)mm;
                    for (k = 40; k < 73; k++) { h = (h ^ mw[k]) * 16777619u; }
                }
                memo[L3].ch = h;
                memo[L3].gen = gVglConstGen;
                memcpy(memo[L3].u17, u17, 12);
                memo[L3].target = d->target;
                memo[L3].valid = 1;
                fresh = 1;
            }
            (void)fresh;
            ch = memo[L3].ch;
            memcpy(m, memo[L3].m, sizeof(m));
            memcpy(sb, memo[L3].sb, sizeof(sb));
            b = lastF[L3].b;
            if (!(lastF[L3].gen == sBinGen && b < sBinCount && sBinIsStage[b] == 2 && sBinFtMH[b] == ch && sBinHash[b] == dh && (!(sBinFtMask[b] & bit) || memcmp(&sBinFtS[b][(p->slot & 15) * 48], sb, sizeof(sb)) == 0) &&
                  sBin[b].icount + (int)need <= BIN_IDX && memcmp(&sBin[b].st, d, sizeof(*d)) == 0 &&
                  (sBinFtCG[b] == gVglConstGen * 4u + (unsigned)L3 || memcmp(sBinFt[b], m, sizeof(m)) == 0))) {
                for (b = 0; b < sBinCount && !(sBinIsStage[b] == 2 && sBinFtMH[b] == ch && sBinHash[b] == dh && (!(sBinFtMask[b] & bit) || memcmp(&sBinFtS[b][(p->slot & 15) * 48], sb, sizeof(sb)) == 0) &&
                                               sBin[b].icount + (int)need <= BIN_IDX && memcmp(&sBin[b].st, d, sizeof(*d)) == 0 && memcmp(sBinFt[b], m, sizeof(m)) == 0); b++) {
                }
                if (b == sBinCount) { /* (for the log: why no batch took it) */
                    int q, st = 0, cm = 0, sl = 0;
                    for (q = 0; q < sBinCount; q++) {
                        if (sBinIsStage[q] != 2 || sBinHash[q] != dh || memcmp(&sBin[q].st, d, sizeof(*d)) != 0) { continue; }
                        st = 1;
                        if (sBinFtMH[q] != ch || memcmp(sBinFt[q], m, sizeof(m)) != 0) { continue; }
                        cm = 1;
                        if ((sBinFtMask[q] & bit) && memcmp(&sBinFtS[q][(p->slot & 15) * 48], sb, sizeof(sb)) != 0) { continue; }
                        sl = 1;
                    }
                    gVitaFtMiss[!st ? 0 : !cm ? 1 : !sl ? 2 : 3]++;
                }
                if (b == sBinCount) {
                    b = -1;
                    if (sBinCount == BINS) {
                        b = flush_one_ft();
                        if (b < 0) {
                            flush_bins_c(5);
                        }
                    }
                    if (b < 0) {
                        b = sBinCount++;
                    }
                    sBin[b].st = *d;
                    sBin[b].count = 0;
                    sBin[b].icount = 0;
                    sBinIsStage[b] = 2;
                    sBinHash[b] = dh;
                    sBinFtH[b] = uh;
                    sBinFtMH[b] = ch;
                    sBinFtMask[b] = 0;
                    sBinFtCG[b] = gVglConstGen * 4u + (unsigned)L3; /* (the constants it was made from) */
                    memcpy(sBinFt[b], m, sizeof(m));
                }
                lastF[L3].b = b;
                lastF[L3].gen = sBinGen;
            }
            sBinFtMask[b] |= bit;
            memcpy(&sBinFtS[b][(p->slot & 15) * 48], sb, sizeof(sb));
        }
        ZM(26);
        memcpy(&sBinIdx[b][sBin[b].icount], extra, need * 2);
        sBin[b].icount += (int)need;
        gVitaFt[0]++;
        return;
    }
    {
        float m[24];
        if (p->upload) {
            uint8_t *map;
            map = sStageVboPtr;
            if (map != NULL) {
                memcpy(map + (size_t)p->at * 32, extra, p->count * 32);
            }
            extra += p->count * 32;
            gVitaStage[1]++;
            gVitaStage[2] = p->at + p->count;
        }
        ZM(27);
        memcpy(&m[0], u[10], 64);
        m[16] = 1.0f / 256.0f; m[17] = -1.0f / 224.0f; m[18] = 2.0f / u[17][2]; m[19] = 0.0f;
        m[20] = -u[17][0] / 256.0f - 1.0f; m[21] = 1.0f + u[17][1] / 224.0f; m[22] = -1.0f; m[23] = 1.0f;
        {   /* only the stage batch that took the last indices can go on (its indices must stay in one run): that one alone is looked at */
            static struct { unsigned gen; int b; } lastS = {~0u, 0};
            b = lastS.b;
            if (!(lastS.gen == sBinGen && b < sBinCount && sBinIsStage[b] == 1 && sBinHash[b] == dh &&
                  sBinStage[b].ix + sBinStage[b].icount == &sStageIdx[sStageIdxUsed] &&
                  memcmp(sBinStage[b].m, m, sizeof(m)) == 0 && memcmp(&sBin[b].st, d, sizeof(*d)) == 0 && bin_join_ok(b, d))) {
                b = sBinCount;
            }
            if (sStageIdxUsed + (int)need > STAGE_IDX) {
                flush_bins_c(6);
                b = sBinCount;
            }
            if (b == sBinCount) {
                if (sBinCount == BINS) {
                    flush_bins_c(7);
                }
                b = sBinCount++;
                bin_made(b, d);
                sBin[b].st = *d;
                sBin[b].count = 0;
                sBin[b].icount = 1;
                sBinIsStage[b] = 1;
                sBinHash[b] = dh;
                memcpy(sBinStage[b].m, m, sizeof(m));
                sBinStage[b].ix = &sStageIdx[sStageIdxUsed];
                sBinStage[b].icount = 0;
            }
            lastS.b = b;
            lastS.gen = sBinGen;
        }
        if (0) {
        if (sStageIdxUsed + (int)need > STAGE_IDX) {
            flush_bins_c(8);
            b = sBinCount;
        }
        if (b == sBinCount) {
            if (sBinCount == BINS) {
                flush_bins_c(9);
            }
            b = sBinCount++;
            sBin[b].st = *d;
            sBin[b].count = 0;
            sBin[b].icount = 1;
            sBinIsStage[b] = 1;
            sBinHash[b] = dh;
            memcpy(sBinStage[b].m, m, sizeof(m));
            sBinStage[b].ix = &sStageIdx[sStageIdxUsed];
            sBinStage[b].icount = 0;
        }
        }
        ZM(28);
        memcpy(&sStageIdx[sStageIdxUsed], extra, need * 4);
        sStageIdxUsed += (int)need;
        sBinStage[b].icount += (int)need;
        gVitaStage[0]++;
    }
}

/* ---- direct parts. A model part the front end keeps (gs_core.c: a DMA call sent again as one GR_PART record)
   was walked here strip by strip every frame: its GS state, its constants, a batch found for each strip, its
   indices copied (half of this thread's time in a fight). Now, when gs_core.c sees the part's GS state on entry
   as it was the first time, the part is drawn from what that first walk made of it: its strips in groups by
   state, the groups' indices kept on the chip, and per group one draw with the part's constants as uniforms.
   gs_core.c follows the GS registers (what the part reads, what it leaves); this side records the strips while
   the part is walked the long way (gDpCapture), makes the groups, and draws them at the batches' turn. ---- */
extern GLboolean vglFastPrepare(GLenum type);
extern void vglFastDrawPtr(GLenum type, const void *idx, uint32_t count, const void *vunif, uint32_t size);
extern void *vglAllocFromScratch(size_t size);
extern void vglFastDraw(GLenum type, uint32_t offset, uint32_t count, const void *vunif, uint32_t size);
extern uint32_t vglFastVertUniformSize(GLuint prog);
extern int vglFastUniformOffset(GLuint prog, GLint location);
#define DP_IBO_BYTES (4u << 20)
static GLuint sDpIbo;             /* the groups' indices (16 bits), appended; emptied only with the chip's buffers */
static uint32_t sDpIboUsed;
static uint8_t *sDpIboShadow;     /* the same indices in memory the processor reads fast (draws merged: below) */
unsigned gVitaDpMerge[2];         /* for the log: the stage's draws merged into the one before (same state, same constants), merged draws made */
static int sDpOn, sDsOn;
static uint32_t sDpUSize[2];      /* bytes of a uniform block: fighters, stage */
int gDpCapture;                   /* a part is walked the long way to be kept (gs_core.c) */
unsigned gVitaDp[8];              /* for the log: parts drawn directly, groups drawn, prepared states, kept, refused, entry state differed, walked, index KB */
typedef struct DpRec { State d; uint64_t t0, texa; float u17[3]; const ChipPlan *p; uint8_t layer; } DpRec;
#define DP_REC 768
static DpRec sDpRec[DP_REC];
static int sDpRecN, sDpRecBad;
typedef struct DpGroup { State st; uint64_t t0, texa; uint32_t ioff, icount; uint16_t strips, ub; uint8_t mode, layer; const uint16_t *iptr; } DpGroup;
typedef struct DpUb { float u17[3]; uint8_t mode, layer; } DpUb;
#define DP_UB 8
typedef struct DpBlob { int ngroups, nub; DpUb ub[DP_UB]; DpGroup g[1]; } DpBlob;
typedef struct DpItem { const DpGroup *g; const float *u; GLuint tex; int next; } DpItem;
#define DP_ITEMS 2048
static DpItem sDpItem[DP_ITEMS];
static int sDpItemN;
#define DP_UFLOATS (1 << 17)
static float sDpU[DP_UFLOATS] __attribute__((aligned(16)));
static int sDpUN;
static void *sDpFreeList[4096];
#define DP_IH 16384
static struct { uint32_t h1, h2, count, off; } sDpIh[DP_IH]; /* the groups' indices on the chip, by content */
static int sDpIhN;
unsigned gVitaDpShared;      /* for the log: groups whose indices were on the chip already */
unsigned long long gVitaDpUs; /* for the log: the direct draws' time on this thread */
static int sDpFreeN;

static void dp_init(void) {
    int f, k, ok = sVsKind[3] != 0 && sVsKind[4] != 0 && !(gVglSafe & 131072);
    for (k = 0; k < 2 && ok; k++) {
        for (f = 0; f < 16 && ok; f++) {
            const Variant *v;
            if (!var_used(f)) {
                continue;
            }
            v = variant(3 + k, f);
            uint32_t sz;
            if (v == NULL || v->u[0] < 0 || vglFastUniformOffset(v->prog, v->u[0]) != 0) {
                Vita_Log("vgl: direct parts: variant %d/%d: uniform block not as expected (%d)\n", 3 + k, f, v != NULL ? vglFastUniformOffset(v->prog, v->u[0]) : -9);
                ok = 0;
                break;
            }
            sz = vglFastVertUniformSize(v->prog);
            if (sz < (k == 0 ? 23u * 16u : 6u * 16u) || sz > 512 || (sDpUSize[k] != 0 && sDpUSize[k] != sz)) {
                Vita_Log("vgl: direct parts: variant %d/%d: uniform block of %u bytes\n", 3 + k, f, (unsigned)sz);
                ok = 0;
                break;
            }
            sDpUSize[k] = sz;
        }
    }
    if (ok) {
        glGenBuffers(1, &sDpIbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sDpIbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, DP_IBO_BYTES, NULL, GL_STATIC_DRAW);
        sDpIboPtr = (uint8_t *)glMapBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_WRITE_ONLY);
        glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        sDpOn = sDpIbo != 0 && sDpIboPtr != NULL;
        if (gVglSafe & 16777216) {
            sDpIboShadow = (uint8_t *)malloc(DP_IBO_BYTES);
        }
    }
    sDsOn = sDpOn && !(gVglSafe & 262144);
    Vita_Log("vgl: direct parts %s, direct strips %s (uniform blocks %u and %u bytes)\n", sDpOn ? "on" : "off", sDsOn ? "on" : "off", (unsigned)sDpUSize[0], (unsigned)sDpUSize[1]);
}

int GsGpu_DpOn(void) {
    return sDpOn && sDpIboUsed + 131072 < DP_IBO_BYTES;
}
void GsGpu_DpCapBegin(void) {
    ag_close(); /* the additive group before anything else */
    sDpRecN = 0;
    sDpRecBad = 0;
    gDpCapture = 1;
}
void GsGpu_DpCapAbort(void) {
    gDpCapture = 0;
}
/* a strip of the part being walked: drawn with `d` from the chip's buffers as planned (vu_draw_impl) */
static void dp_record(const State *d, int layer, const float (*u)[4], const ChipPlan *p, int ctx) {
    DpRec *r;
    if (sDpRecN == DP_REC || p->upload || (p->mode != 1 && p->mode != 2) || p->count < 3 || sDumping) {
        sDpRecBad = 1;
        return;
    }
    r = &sDpRec[sDpRecN++];
    r->d = *d;
    r->t0 = gGs.tex0[ctx];
    r->texa = gGs.texa;
    r->u17[0] = u[17][0];
    r->u17[1] = u[17][1];
    r->u17[2] = u[17][2];
    r->p = p;
    r->layer = (uint8_t)layer;
}
/* The walk is over: the part's strips in groups, their indices on the chip. NULL: not kept. */
void *GsGpu_DpCapEnd(void) {
    typedef struct { State st; uint64_t t0, texa; float u17[3]; uint32_t mode, layer; } Key;
    static Key key[64];
    static uint8_t gi[DP_REC];
    static uint32_t gcount[64], gstrips[64];
    Key k;
    DpBlob *b;
    int i, j, ng = 0;
    uint32_t total = 0, at;
    uint8_t *map;
    uint16_t *tmp;

    gDpCapture = 0;
    if (sDpRecBad || sDpRecN == 0) {
        return NULL;
    }
    for (i = 0; i < sDpRecN; i++) {
        const DpRec *r = &sDpRec[i];
        memset(&k, 0, sizeof(k));
        k.st = r->d;
        k.st.tex = 0;
        if (r->d.tme) {
            k.t0 = r->t0 & 0x1FFFFFFFFFFFFFFFull;
            k.texa = r->texa;
        }
        memcpy(k.u17, r->u17, sizeof(k.u17));
        k.mode = r->p->mode;
        k.layer = r->layer;
        for (j = 0; j < ng && memcmp(&key[j], &k, sizeof(k)) != 0; j++) {
        }
        if (j == ng) {
            if (ng == 64) {
                return NULL;
            }
            key[ng] = k;
            gcount[ng] = 0;
            gstrips[ng] = 0;
            ng++;
        }
        gi[i] = (uint8_t)j;
        gcount[j] += (r->p->count - 2) * 3;
        gstrips[j]++;
        if (r->p->mode == 2) { /* (the stage's indices are 32 bits wide: kept as 16 when they fit) */
            const uint32_t *ix = (const uint32_t *)(const void *)((const uint8_t *)r->p + 32);
            uint32_t n = (r->p->count - 2) * 3, q;
            for (q = 0; q < n; q++) {
                if (ix[q] > 0xFFFFu) {
                    return NULL;
                }
            }
        }
    }
    for (j = 0; j < ng; j++) {
        total += gcount[j];
    }
    if (sDpIboUsed + total * 2 + ng * 16 > DP_IBO_BYTES) {
        return NULL;
    }
    b = (DpBlob *)malloc(sizeof(DpBlob) + (size_t)ng * sizeof(DpGroup));
    tmp = (uint16_t *)malloc(total * 2 + 16);
    if (b == NULL || tmp == NULL) {
        free(b);
        free(tmp);
        return NULL;
    }
    memset(b, 0, sizeof(DpBlob) + (size_t)ng * sizeof(DpGroup));
    b->ngroups = ng;
    for (j = 0, at = 0; j < ng; j++) {
        DpGroup *g = &b->g[j];
        int u;
        g->st = key[j].st;
        g->t0 = key[j].t0;
        g->texa = key[j].texa;
        g->mode = (uint8_t)key[j].mode;
        g->layer = (uint8_t)key[j].layer;
        g->ioff = at; /* (in `tmp` for now) */
        g->icount = 0;
        g->strips = (uint16_t)gstrips[j];
        at += gcount[j];
        for (u = 0; u < b->nub; u++) { /* the uniform blocks the part needs: one per kind, layer and offsets */
            if (b->ub[u].mode == g->mode && b->ub[u].layer == g->layer && memcmp(b->ub[u].u17, key[j].u17, sizeof(key[j].u17)) == 0) {
                break;
            }
        }
        if (u == b->nub) {
            if (u == DP_UB) {
                free(b);
                free(tmp);
                return NULL;
            }
            memcpy(b->ub[u].u17, key[j].u17, sizeof(key[j].u17));
            b->ub[u].mode = g->mode;
            b->ub[u].layer = g->layer;
            b->nub++;
        }
        g->ub = (uint16_t)u;
    }
    for (i = 0; i < sDpRecN; i++) { /* the strips' indices, group by group in the strips' order */
        const DpRec *r = &sDpRec[i];
        DpGroup *g = &b->g[gi[i]];
        uint32_t n = (r->p->count - 2) * 3, q;
        uint16_t *dst = tmp + g->ioff + g->icount;
        if (r->p->mode == 1) {
            memcpy(dst, (const uint8_t *)r->p + 32, n * 2);
        } else {
            const uint32_t *ix = (const uint32_t *)(const void *)((const uint8_t *)r->p + 32);
            for (q = 0; q < n; q++) {
                dst[q] = (uint16_t)ix[q];
            }
        }
        g->icount += n;
    }
    /* each group's indices on the chip: where the same indices already are (another form of the part), else new */
    map = NULL;
    for (j = 0; j < ng; j++) {
        DpGroup *g = &b->g[j];
        const uint16_t *src = tmp + g->ioff;
        uint32_t h1 = 2166136261u, h2 = 0x9E3779B9u ^ g->icount, q, slot;
        for (q = 0; q < g->icount; q++) {
            h1 = (h1 ^ src[q]) * 16777619u;
            h2 = (h2 + src[q]) * 0x85EBCA6Bu ^ (h2 >> 13);
        }
        slot = h1 & (DP_IH - 1);
        while (sDpIh[slot].count != 0 && !(sDpIh[slot].h1 == h1 && sDpIh[slot].h2 == h2 && sDpIh[slot].count == g->icount)) {
            slot = (slot + 1) & (DP_IH - 1);
        }
        if (sDpIh[slot].count != 0) {
            g->ioff = sDpIh[slot].off;
            gVitaDpShared++;
            continue;
        }
        if (map == NULL) {
            map = sDpIboPtr;
            if (map == NULL) {
                free(b);
                free(tmp);
                return NULL;
            }
        }
        memcpy(map + sDpIboUsed, src, g->icount * 2);
        if (sDpIboShadow != NULL) {
            memcpy(sDpIboShadow + sDpIboUsed, src, g->icount * 2);
        }
        g->ioff = sDpIboUsed;
        sDpIboUsed += (g->icount * 2 + 15) & ~15u;
        if (sDpIhN < DP_IH / 2) {
            sDpIh[slot].h1 = h1;
            sDpIh[slot].h2 = h2;
            sDpIh[slot].count = g->icount;
            sDpIh[slot].off = g->ioff;
            sDpIhN++;
        }
    }
    free(tmp);
    gVitaDp[7] = sDpIboUsed >> 10;
    return b;
}

/* gs_core.c lets go of a kept part: freed after the frame's draws (its groups may be waiting to be drawn) */
void GsGpu_DpFree(void *blob) {
    if (blob == NULL) {
        return;
    }
    if (sDpFreeN == 4096) {
        free(sDpFreeList[0]); /* (not reached: 4096 in one frame) */
        memmove(sDpFreeList, sDpFreeList + 1, 4095 * sizeof(void *));
        sDpFreeN--;
    }
    sDpFreeList[sDpFreeN++] = blob;
}
static void dp_free_pending(void) {
    int i;
    for (i = 0; i < sDpFreeN; i++) {
        free(sDpFreeList[i]);
    }
    sDpFreeN = 0;
}

/* bones A (0..15, pivot 32..34) and B (16..31, pivot 36..38) of a fighter's uniform block that are not
   finite: replaced by the identity (and a pivot of 0) so that nothing else turns NaN; the flags (1 A, 2 B) for the
   shader, which drops the vertices that use such a bone */
static float bone_fix(float *o) {
    int m, k, bad = 0;
    for (m = 0; m < 2; m++) {
        uint32_t acc = 0;
        for (k = 0; k < 16; k++) {
            uint32_t b;
            memcpy(&b, &o[m * 16 + k], 4);
            acc |= ((b & 0x7F800000u) == 0x7F800000u);
        }
        for (k = 0; k < 3; k++) {
            uint32_t b;
            memcpy(&b, &o[32 + m * 4 + k], 4);
            acc |= ((b & 0x7F800000u) == 0x7F800000u);
        }
        if (acc) {
            bad |= 1 << m;
            memset(&o[m * 16], 0, 64);
            o[m * 16 + 0] = o[m * 16 + 5] = o[m * 16 + 10] = o[m * 16 + 15] = 1.0f;
            o[32 + m * 4] = o[33 + m * 4] = o[34 + m * 4] = 0.0f;
        }
    }
    if (bad) {
        gVitaBoneBad++;
    }
    return (float)bad;
}
/* a part's uniform block: the fighters' (vertex program 0, layer 0 or 1) or the stage's (program 4) */
static void dp_ublock(float *o, const float *c, const DpUb *ub, uint32_t floats) {
    const float *u17 = ub->u17;
    float *t;
    memset(o, 0, floats * 4);
    if (ub->mode == 1) {
        int k;
        memcpy(o, c, 160);                                  /* 0..9: bones and pivots (VU memory 0..9) */
        o[75] = bone_fix(o);                                /* */
        o[40] = c[40]; o[41] = c[44]; o[42] = c[48]; o[43] = c[52]; /* 10: the light */
        for (k = 0; k < 4; k++) {                           /* 11: the layer's colour (layer 2: program 2's, where layer 0's is) */
            float v = c[(ub->layer == 0 || ub->layer == 2 ? 88 : 92) + k];
            o[44 + k] = (v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : (float)(int)v) / 128.0f;
        }
        memcpy(&o[48], &c[56], 64);                         /* 12..15: the screen matrix */
        t = &o[64];
        if (ub->layer == 2) { /* a silhouette, into the work page (256 x 256; vitaGL's framebuffer objects stand upside down) */
            o[73] = 1.0f;
            t[0] = 1.0f / 128.0f; t[1] = 1.0f / 128.0f; t[2] = 2.0f / u17[2]; t[3] = 0.0f;
            t[4] = -u17[0] / 128.0f - 1.0f; t[5] = -u17[1] / 128.0f - 1.0f; t[6] = -1.0f; t[7] = 1.0f;
            return;
        }
        o[72] = (float)ub->layer;
        if (!gVitaClipOff) { /* program 0's clip-test matrix: its reject (kVertFtD) */
            o[74] = 1.0f;
            memcpy(&o[76], &c[72], 64);
        }
    } else {
        memcpy(o, c, 64);                                   /* 0..3: the screen matrix */
        t = &o[16];
    }
    t[0] = 1.0f / 256.0f; t[1] = -1.0f / 224.0f; t[2] = 2.0f / u17[2]; t[3] = 0.0f;
    t[4] = -u17[0] / 256.0f - 1.0f; t[5] = 1.0f + u17[1] / 224.0f; t[6] = -1.0f; t[7] = 1.0f;
}

static void dp_draw_all(void);
static void shade_log(const char *where, uint64_t t0, int ctx, const float *c); /* */
static int sVu0Have; /* GsGpu_DrawVu0: its constants are those of the strip before (a part drawn directly ends that) */
/* A kept part, its GS state as when it was kept: its groups wait for the batches' turn, with the constants of now
   (`c`: VU memory 0..23) and the textures GS memory holds now. */
static void bone_check(const float *c, const char *where); /* (the log; below) */
void GsGpu_DpDraw(const void *blob, const float *c) {
    ag_close(); /* the additive group before anything else */
    const DpBlob *b = (const DpBlob *)blob;
    const float *ub[DP_UB];
    int i;
    ZM(32); /* (the parts of a kept part, zones 32..35) */
    if (VITA_RARE(gVitaSliver) && b->nub > 0 && b->ub[0].mode == 1) {
        bone_check(c, "kept part");
    }
    if (sDpItemN + b->ngroups > DP_ITEMS || sDpUN + b->nub * 128 > DP_UFLOATS) {
        dp_draw_all();
    }
    if (gVitaOrder && sBinCount > 0) { /* the batches waiting came first: drawn first when the order matters */
        int unsafe = sBinLastUnsafe >= 0;
        for (i = 0; i < b->ngroups && !unsafe; i++) {
            unsafe = b->g[i].mode != 1 && !st_commutes(&b->g[i].st);
        }
        if (unsafe) {
            flush_bins_c(16);
        }
    }
    flush(); /* GS primitives that came before */
    ZM(33);
    for (i = 0; i < b->nub; i++) {
        uint32_t floats = sDpUSize[b->ub[i].mode == 1 ? 0 : 1] / 4;
        float *o = &sDpU[sDpUN];
        sDpUN += (int)((floats + 3) & ~3u);
        dp_ublock(o, c, &b->ub[i], floats);
        ub[i] = o;
    }
    ZM(34);
    for (i = 0; i < b->ngroups; i++) {
        const DpGroup *g = &b->g[i];
        DpItem *it = &sDpItem[sDpItemN++];
        if (g->mode == 1 && g->layer == 1) {
            shade_log("kept", g->t0, -1, c);
        }
        am_model_reads(&g->st, 1);
        sSeeThru |= g->st.mask == 8; /* */
        it->g = g;
        it->u = ub[g->ub];
        it->tex = g->st.tme ? texture_get_k(g->t0, g->texa) : 0;
        if (VITA_RARE(gVitaFilm > 0)) {
            SIG_MIX(sSigSt, (uint32_t)g->t0); /* (the signature: the kept parts' groups, their textures' contents) */
            SIG_MIX(sSigSt, (uint32_t)(g->t0 >> 32));
            SIG_MIX(sSigStT, g->st.tme ? sSigGenCur : 0u);
            SIG_MIX(sSigSt, (uint32_t)g->icount | (uint32_t)g->mode << 24);
            SIG_MIX(sSigSt, (uint32_t)g->st.blend | (uint32_t)g->st.src << 4 | (uint32_t)g->st.dst << 8 | (uint32_t)g->st.date << 12 | (uint32_t)g->st.sop << 16);
            sSigStN++;
        }
        gVglVuTris += g->icount / 3;
        gVglVuBatches += g->strips;
        if (g->mode == 1) {
            gVitaFt[0] += g->strips;
        } else {
            gVitaStage[0] += g->strips;
        }
    }
    sVu0Have = 0;
    gVitaDp[0]++;
    ZM(35);
}

/* ---- direct strips: the fighters' strips do not come in kept parts (the game sends each fighter's pieces
   straight from the list, no DMA call), so each strip came here and found a batch, a place for its bones, a copy of
   its indices (half of this thread's time). Now the strips of one set of constants (one piece of a fighter: its
   bones) are drawn as one draw per layer: the strips' indices go straight into memory the chip reads this frame,
   and the piece's bones are the draw's uniform block, as with the kept parts. ---- */
#define DS_OPEN 8
#define DS_CAP 3072 /* indices an open draw takes before another is begun */
static struct { State st; int layer; DpGroup *g; const float *u; } sDsOpen[DS_OPEN];
static int sDsOpenN;
static unsigned sDsGen = ~0u;           /* the constants the open draws were begun with (gVglConstGen) */
static float *sDsU[3];                  /* their uniform blocks, layer by layer (or NULL; 2: the silhouettes) */
static uint32_t sDsU17[3][3];
static DpGroup sDsGroup[DP_ITEMS];
static int sDsGroupN;
unsigned gVitaDs[4];                    /* for the log: strips drawn this way, draws begun, runs of constants, strips that went the batches' way */
static void ds_close(void) {
    sDsOpenN = 0;
    sDsGen = ~0u;
    sDsU[0] = sDsU[1] = sDsU[2] = NULL;
}
static void ds_ublock(float *o, const float (*u)[4], int layer, uint32_t floats) {
    int k;
    memset(o, 0, floats * 4);
    memcpy(o, u[0], 160);                                   /* 0..9: bones and pivots */
    o[75] = bone_fix(o);                                    /* */
    memcpy(&o[40], u[14], 16);                              /* 10: the light */
    for (k = 0; k < 4; k++) {                               /* 11: the layer's colour */
        float v = u[layer == 1 ? 16 : 15][k];
        o[44 + k] = (v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : (float)(int)v) / 128.0f;
    }
    memcpy(&o[48], u[10], 64);                              /* 12..15: the screen matrix */
    if (layer == 2) { /* a silhouette, into the work page: as dp_ublock */
        o[64] = 1.0f / 128.0f; o[65] = 1.0f / 128.0f; o[66] = 2.0f / u[17][2]; o[67] = 0.0f;
        o[68] = -u[17][0] / 128.0f - 1.0f; o[69] = -u[17][1] / 128.0f - 1.0f; o[70] = -1.0f; o[71] = 1.0f;
        o[73] = 1.0f;
        return;
    }
    o[64] = 1.0f / 256.0f; o[65] = -1.0f / 224.0f; o[66] = 2.0f / u[17][2]; o[67] = 0.0f;
    o[68] = -u[17][0] / 256.0f - 1.0f; o[69] = 1.0f + u[17][1] / 224.0f; o[70] = -1.0f; o[71] = 1.0f;
    o[72] = (float)layer;
    if (u[19][1] > 0.5f) { /* program 0's reject, as dp_ublock */
        o[74] = 1.0f;
        memcpy(&o[76], u[20], 64);
    }
}
/* A planned fighter strip (vertices on the chip): 1 when taken this way, 0 for the batches */
static int ds_strip_x(const State *d, int layer, const float *u17, const float (*u)[4], const float *c, const ChipPlan *p);
static int ds_strip(const State *d, int layer, const float (*u)[4], const ChipPlan *p) {
    return ds_strip_x(d, layer, u[17], u, NULL, p);
}
static int ds_strip_x(const State *d, int layer, const float *u17, const float (*u)[4], const float *c, const ChipPlan *p) {
    const uint16_t *ix;
    uint32_t need = (p->count - 2) * 3, floats = sDpUSize[0] / 4;
    int L = layer == 2 ? 2 : layer & 1, i;
    if (!sDsOn || layer > 2 || sFtIboPtr == NULL || (layer == 2 && !d->target)) { /* (layer 2: the silhouettes into the work page only) */
        return 0;
    }
    if (sDsGen != gVglConstGen) { /* other constants: another piece */
        ds_close();
        sDsGen = gVglConstGen;
        gVitaDs[2]++;
    }
    if (sDsU[L] == NULL || memcmp(sDsU17[L], u17, 12) != 0) {
        float *o;
        if (sDpUN + 128 > DP_UFLOATS) {
            dp_draw_all();
            sDsGen = gVglConstGen;
        }
        o = &sDpU[sDpUN];
        sDpUN += (int)((floats + 3) & ~3u);
        if (u != NULL) {
            ds_ublock(o, u, L, floats);
        } else {
            DpUb ub;
            memcpy(ub.u17, u17, 12);
            ub.mode = 1;
            ub.layer = (uint8_t)L;
            dp_ublock(o, c, &ub, floats);
        }
        if (sDsU[L] != NULL) { /* (other offsets: the open draws of this layer are closed) */
            int n = 0;
            for (i = 0; i < sDsOpenN; i++) {
                if (sDsOpen[i].layer != L) {
                    sDsOpen[n++] = sDsOpen[i];
                }
            }
            sDsOpenN = n;
        }
        sDsU[L] = o;
        memcpy(sDsU17[L], u17, 12);
    }
    ix = sFtIboPtr + p->ioff;
    for (i = 0; i < sDsOpenN; i++) {
        if (sDsOpen[i].layer == L && sDsOpen[i].u == sDsU[L] && memcmp(&sDsOpen[i].st, d, sizeof(*d)) == 0) {
            break;
        }
    }
    if (i < sDsOpenN && sDsOpen[i].g->iptr + sDsOpen[i].g->icount != ix) {
        sDsOpen[i] = sDsOpen[--sDsOpenN]; /* (its indices are elsewhere on the chip: another draw is begun) */
        i = sDsOpenN;
    }
    if (i == sDsOpenN) {
        DpGroup *g;
        DpItem *it;
        if (L != 2) {
            am_model_reads(d, 2);
        }
        if (sDpItemN == DP_ITEMS || sDsGroupN == DP_ITEMS) {
            dp_draw_all(); /* (the uniform blocks went with it: this strip goes the batches' way) */
            return 0;
        }
        if (sDsOpenN == DS_OPEN) {
            sDsOpen[0] = sDsOpen[--sDsOpenN];
        }
        g = &sDsGroup[sDsGroupN++];
        memset(g, 0, sizeof(*g));
        g->st = *d;
        g->mode = 1;
        g->layer = (uint8_t)L;
        g->iptr = ix;
        it = &sDpItem[sDpItemN++];
        it->g = g;
        it->u = sDsU[L];
        it->tex = d->tex;
        it->next = -1;
        i = sDsOpenN++;
        sDsOpen[i].st = *d;
        sDsOpen[i].layer = L;
        sDsOpen[i].g = g;
        sDsOpen[i].u = sDsU[L];
        gVitaDs[1]++;
    }
    sDsOpen[i].g->icount += need;
    sDsOpen[i].g->strips++;
    gVitaDs[0]++;
    return 1;
}

/* gs_core.c GR_VU2 (program 0's two layers in one record): a layer of a planned fighter strip straight to the
   direct draws (the GS registers are set already): its state, its offsets, its indices. 0: the general way. */
/* the fighters' draw states of a picture, for the log (the GS registers they come from, how many strips,
   the textures' and the layer colour's alpha) */
static struct { uint64_t test, alpha, frame, zbuf, t0, clamp; uint32_t fba, layer, ctx, n, tris, ntex, okn; GLuint lastTex; int tamin, tamax, camin, camax; State d; } sFs[48];
static void fs_add(int layer, int ctx, const State *d, int ok, const float *c, uint32_t count) {
    uint64_t t0m = gGs.tex0[ctx] & ~((1ull << 20) - 1) & ~(0xFFull << 26) & ~(0x3FFFull << 37); /* (no addresses or sizes: the formats, CLUT use) */
    int k, ca = c != NULL ? (int)c[(layer == 0 ? 88 : 92) + 3] : -1;
    const Tex *t = NULL;
    for (k = 0; k < sFsN; k++) {
        if (sFs[k].layer == (uint32_t)layer && sFs[k].ctx == (uint32_t)ctx && sFs[k].test == gGs.test[ctx] && sFs[k].alpha == gGs.alpha[ctx] &&
            sFs[k].frame == gGs.frame[ctx] && sFs[k].zbuf == gGs.zbuf[ctx] && sFs[k].t0 == t0m && sFs[k].fba == (uint32_t)(gGs.fba[ctx] & 1)) {
            break;
        }
    }
    if (k == sFsN) {
        if (sFsN == 48) {
            sFsOver++;
            return;
        }
        memset(&sFs[k], 0, sizeof(sFs[k]));
        sFs[k].layer = (uint32_t)layer; sFs[k].ctx = (uint32_t)ctx; sFs[k].test = gGs.test[ctx]; sFs[k].alpha = gGs.alpha[ctx];
        sFs[k].frame = gGs.frame[ctx]; sFs[k].zbuf = gGs.zbuf[ctx]; sFs[k].t0 = t0m; sFs[k].fba = (uint32_t)(gGs.fba[ctx] & 1); sFs[k].clamp = gGs.clamp[ctx];
        sFs[k].tamin = 999; sFs[k].tamax = -1; sFs[k].camin = 999; sFs[k].camax = -999;
        sFsN++;
    }
    sFs[k].n++;
    sFs[k].tris += count - 2;
    if (!ok) {
        return;
    }
    sFs[k].okn++;
    sFs[k].d = *d;
    if (ca < sFs[k].camin) { sFs[k].camin = ca; }
    if (ca > sFs[k].camax) { sFs[k].camax = ca; }
    if (d->tex != 0 && d->tex != sFs[k].lastTex) {
        int j;
        sFs[k].lastTex = d->tex;
        sFs[k].ntex++;
        for (j = 0; j < sTexCount; j++) {
            if (sTex[j].id == d->tex) {
                t = &sTex[j];
                break;
            }
        }
        if (t != NULL) {
            if (t->amin < sFs[k].tamin) { sFs[k].tamin = t->amin; }
            if (t->amax > sFs[k].tamax) { sFs[k].tamax = t->amax; }
        }
    }
}
static void fs_dump(unsigned list) {
    int k;
    for (k = 0; k < sFsN; k++) {
        const State *d = &sFs[k].d;
        Vita_Log("t114: list %u: fighters #%d layer %u ctx %u: %u strips (%u drawn), %u triangles | TEST %05x ALPHA %02x fix %02x FRAME %03x psm %02x msk %08x FBA %u ZBUF %03x zmsk %u CLAMP %x\n",
                 list, k, sFs[k].layer, sFs[k].ctx, sFs[k].n, sFs[k].okn, sFs[k].tris, (unsigned)sFs[k].test, (unsigned)(sFs[k].alpha & 0xFF), (unsigned)((sFs[k].alpha >> 32) & 0xFF),
                 (unsigned)(sFs[k].frame & 0x1FF), (unsigned)((sFs[k].frame >> 24) & 0x3F), (unsigned)(sFs[k].frame >> 32), sFs[k].fba, (unsigned)(sFs[k].zbuf & 0x1FF), (unsigned)((sFs[k].zbuf >> 32) & 1),
                 (unsigned)sFs[k].clamp & 0xF);
        Vita_Log("t114:   #%d TEX0 psm %02x tcc %u tfx %u cpsm %x csm %u csa %u cld %u | %u textures, alpha %d..%d, layer colour alpha %d..%d | tex %u env %u blend %u (%u,%u,%u) atest %u (%u,%u) z %u (%u,%u) mask %x fba %u date %u sop %u\n",
                 k, (unsigned)((sFs[k].t0 >> 20) & 0x3F), (unsigned)((sFs[k].t0 >> 34) & 1), (unsigned)((sFs[k].t0 >> 35) & 3), (unsigned)((sFs[k].t0 >> 51) & 15),
                 (unsigned)((sFs[k].t0 >> 55) & 1), (unsigned)((sFs[k].t0 >> 56) & 31), (unsigned)((sFs[k].t0 >> 61) & 7), sFs[k].ntex, sFs[k].tamin, sFs[k].tamax, sFs[k].camin, sFs[k].camax,
                 (unsigned)d->tex, d->env, d->blend, d->src, d->dst, d->eq, d->atest, d->afunc, d->aref, d->ztest, d->zfunc, d->zwrite, d->mask, d->fba, d->date, d->sop);
    }
    if (sFsOver) {
        Vita_Log("t114: list %u: %d more fighter strips in states not listed\n", list, sFsOver);
    }
    sFsN = 0;
    sFsOver = 0;
}
unsigned gT121Fs[8];  /* for the log: fighter layers' states taken from the strip before, or not: why */
unsigned gT121Ep[8];  /* for the log: GS memory changes (epoch) by where: paint, large upload not by block, small upload, large, (small: in pieces, tiny, not remembered) */
unsigned gT121Sm[4];  /* for the log: small uploads not found unchanged: first sight, other place or size, other data, GS memory changed */
void Vita_T121Log(void) {
    Vita_Log("vita:   fighter layers' states: %u as the strip before; worked out: %u first, %u new picture, %u GS memory changed, %u buffers drawn into, %u other context, %u other texture, %u other registers\n",
             gT121Fs[0], gT121Fs[1], gT121Fs[2], gT121Fs[3], gT121Fs[4], gT121Fs[5], gT121Fs[6], gT121Fs[7]);
    Vita_Log("vita:   GS memory changes: %u paints, %u large uploads not by block, %u small uploads (%u in pieces, %u tiny, %u other), %u large | small uploads written: %u first sight, %u other place, %u other data, %u memory changed\n",
             gT121Ep[0], gT121Ep[1], gT121Ep[2], gT121Ep[4], gT121Ep[5], gT121Ep[6], gT121Ep[3], gT121Sm[0], gT121Sm[1], gT121Sm[2], gT121Sm[3]);
    Vita_Log("vita:   textures with a palette, ms by part: GS memory's numbers %u, indices %u (and reading), palette %u, vitaGL %u; making room %u; most in one picture %u us; %u effect primitives waited a picture for theirs; %u layers' textures made over the budget\n",
             (unsigned)(gT125Us[0] / 1000), (unsigned)(gT125Us[1] / 1000), (unsigned)(gT125Us[2] / 1000), (unsigned)(gT125Us[3] / 1000), (unsigned)(gT125Us[4] / 1000), (unsigned)gT125Us[5], gT126Deferred, gT126Layer);
    gT126Deferred = 0;
    gT126Layer = 0;
    memset(gT125Us, 0, sizeof(gT125Us));
    {
        extern unsigned gVitaFeHash[3], gVitaFeMiss[4];
        Vita_Log("vita:   upload numbers: %u worked out ahead by the list reader, %u of them taken; %u worked out by the drawing thread (none there %u, another list's %u, "
                 "another upload's %u, another length %u)\n", gVitaFeHash[0], gVitaFeHash[1], gVitaFeHash[2], gVitaFeMiss[0], gVitaFeMiss[1], gVitaFeMiss[2], gVitaFeMiss[3]);
        memset(gVitaFeHash, 0, sizeof(gVitaFeHash));
        memset(gVitaFeMiss, 0, sizeof(gVitaFeMiss));
    }
    {
        extern int gVitaUpCheck, gVitaUpQuick;
        extern unsigned gVitaUpMiss[4], gVitaUpDyn[2], gVitaUpDyn2[3], gVitaCcLeak, gVitaDirChk[2];
        if (gVitaUpCheck) {
            Vita_Log("vita:   uploads checked (upquick %d): numbers from samples %u, %u of them wrong; the list reader's %u, %u of them wrong | large uploads %u, %u from a place whose bytes changed"
                     " | uploads from a copy %u, read in place %u, of them changed since %u | kept copies never freed %u | GS packets read in place %u, of them changed since %u\n",
                     gVitaUpQuick, gVitaUpMiss[0], gVitaUpMiss[1], gVitaUpMiss[2], gVitaUpMiss[3], gVitaUpDyn[0], gVitaUpDyn[1], gVitaUpDyn2[0], gVitaUpDyn2[1], gVitaUpDyn2[2], gVitaCcLeak, gVitaDirChk[0], gVitaDirChk[1]);
            memset(gVitaDirChk, 0, sizeof(gVitaDirChk));
            memset(gVitaUpMiss, 0, sizeof(gVitaUpMiss));
            memset(gVitaUpDyn, 0, sizeof(gVitaUpDyn));
            memset(gVitaUpDyn2, 0, sizeof(gVitaUpDyn2));
        }
    }
    memset(gT121Fs, 0, sizeof(gT121Fs));
    memset(gT121Ep, 0, sizeof(gT121Ep));
    memset(gT121Sm, 0, sizeof(gT121Sm));
}
/* (for the log: bt3.txt sliver=1) the triangles of a fighter's strip that reach far off the screen: the
   strip's vertices (as the chip has them) through kVertFtD's arithmetic on the processor. A thin line from a fighter's
   head across the sky in the Bardock / Frieza fight is looked for this way. */
int gVitaSliver;
static unsigned sSliverN;
/* a fighter piece's two bone matrices (VU 0..3, 4..7) with a value that is not finite or a
   row far from unit length: a secondary-motion bone (hair, tails, cloth) gone wrong would stretch the piece */
static void bone_check(const float *c, const char *where) {
    extern int Vita_InFight(void);
    static unsigned n, fightWas;
    int m, r, k, bad = 0;
    if (Vita_InFight() != (int)fightWas) {
        fightWas = (unsigned)Vita_InFight();
        n = 0;
    }
    if (n >= 40) {
        return;
    }
    for (m = 0; m < 2; m++) {
        for (r = 0; r < 3; r++) {
            float l = 0.0f;
            for (k = 0; k < 4; k++) {
                float v = c[m * 16 + r * 4 + k];
                if (!(v == v) || v > 1e6f || v < -1e6f) {
                    bad |= 1;
                }
            }
            for (k = 0; k < 3; k++) {
                l += c[m * 16 + r * 4 + k] * c[m * 16 + r * 4 + k];
            }
            if (l > 400.0f || l < 0.0025f) {
                bad |= 2;
            }
        }
        for (k = 0; k < 4; k++) {
            float v = c[m * 16 + 12 + k];
            if (!(v == v) || v > 1e5f || v < -1e5f) {
                bad |= 4;
            }
        }
    }
    if (bad) {
        n++;
        Vita_Log("sliver: picture %u bones %s (%d): A %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g; B %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g; pivots %g %g %g / %g %g %g\n",
                 gGsFrame, where, bad, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15],
                 c[16], c[17], c[18], c[19], c[20], c[21], c[22], c[23], c[24], c[25], c[26], c[27], c[28], c[29], c[30], c[31], c[32], c[33], c[34], c[36], c[37], c[38]);
    }
}
static void sliver_check(int layer, const float *c, const ChipPlan *p, const float *u17) {
    const uint8_t *vb = sFtVboPtr;
    float sx[1024], sy[1024], sw[1024];
    uint8_t rej[1024];
    uint32_t i, n = p->count;
    {   /* a cap for each fight */
        extern int Vita_InFight(void);
        static int fightWas;
        if (Vita_InFight() != fightWas) {
            fightWas = Vita_InFight();
            sSliverN = 0;
        }
    }
    if (layer == 0) {
        bone_check(c, "strip");
    }
    if (vb == NULL || n > 1024) { /* (past the cap of the long thin ones, still the remnants below) */
        return;
    }
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(const void *)(vb + (size_t)(p->at + i) * 40);
        float a[3], b[3], pa[4], pb[4], q[4], sv[4], cv[4];
        int k;
        for (k = 0; k < 3; k++) {
            a[k] = v[k] - c[32 + k];
            b[k] = v[k] - c[36 + k];
        }
        for (k = 0; k < 4; k++) {
            pa[k] = c[12 + k] + c[0 + k] * a[0] + c[4 + k] * a[1] + c[8 + k] * a[2];
            pb[k] = c[28 + k] + c[16 + k] * b[0] + c[20 + k] * b[1] + c[24 + k] * b[2];
            q[k] = pb[k] + (pa[k] - pb[k]) * v[3];
        }
        for (k = 0; k < 4; k++) {
            sv[k] = c[68 + k] + c[56 + k] * q[0] + c[60 + k] * q[1] + c[64 + k] * q[2];
            cv[k] = c[84 + k] + c[72 + k] * q[0] + c[76 + k] * q[1] + c[80 + k] * q[2];
        }
        sw[i] = sv[3];
        sx[i] = sv[3] != 0.0f ? sv[0] / sv[3] - u17[0] : 1e9f;
        sy[i] = sv[3] != 0.0f ? sv[1] / sv[3] - u17[1] : 1e9f;
        rej[i] = fabsf(cv[0]) > fabsf(cv[3]) || fabsf(cv[1]) > fabsf(cv[3]) || fabsf(cv[2]) > fabsf(cv[3]);
        if (fabsf(sx[i]) < 3.0f && fabsf(sy[i]) < 3.0f && !rej[i] && sSliverN < 40) { /* a fighter's vertex at the picture's corner */
            Vita_Log("sliver: picture %u layer %d strip of %u at %u: vertex %u at the corner (%.1f,%.1f w %g): position %g %g %g weight %g\n",
                     gGsFrame, layer, n, p->at, i, sx[i], sy[i], sw[i], v[0], v[1], v[2], v[3]);
            sSliverN++;
        }
    }
    for (i = 0; i + 2 < n; i++) {
        uint32_t j, in = 0;
        float e[3], L = 0.0f, area, hgt;
        if (rej[i] | rej[i + 1] | rej[i + 2]) {
            /* a rejected triangle whose two other corners are far apart: the line its remnant made */
            static unsigned remN;
            static int remFight;
            extern int Vita_InFight(void);
            if (Vita_InFight() != remFight) {
                remFight = Vita_InFight();
                remN = 0;
            }
            if (rej[i] + rej[i + 1] + rej[i + 2] == 1 && remN < 20) {
                uint32_t a0 = rej[i] ? i + 1 : i, a1 = rej[i + 2] ? i + 1 : i + 2;
                float el = sqrtf((sx[a0] - sx[a1]) * (sx[a0] - sx[a1]) + (sy[a0] - sy[a1]) * (sy[a0] - sy[a1]));
                if (el > 150.0f && sw[a0] > 0.0f && sw[a1] > 0.0f) {
                    remN++;
                    Vita_Log("sliver: picture %u layer %d strip of %u at %u, triangle %u rejected (corner %u): the other two (%.0f,%.0f w %.3g) (%.0f,%.0f w %.3g), %.0f apart\n",
                             gGsFrame, layer, n, p->at, i, rej[i] ? i : rej[i + 1] ? i + 1 : i + 2, sx[a0], sy[a0], sw[a0], sx[a1], sy[a1], sw[a1], el);
                }
            }
            continue;
        }
        for (j = i; j < i + 3; j++) {
            in += sw[j] > 0.0f && sx[j] > -64.0f && sx[j] < 576.0f && sy[j] > -64.0f && sy[j] < 512.0f;
        }
        for (j = 0; j < 3; j++) { /* (a long, thin triangle: its longest side over 200 pixels, under 4 pixels across) */
            uint32_t a0 = i + j, a1 = i + (j + 1) % 3;
            e[j] = sqrtf((sx[a0] - sx[a1]) * (sx[a0] - sx[a1]) + (sy[a0] - sy[a1]) * (sy[a0] - sy[a1]));
            if (e[j] > L) {
                L = e[j];
            }
        }
        area = 0.5f * fabsf((sx[i + 1] - sx[i]) * (sy[i + 2] - sy[i]) - (sx[i + 2] - sx[i]) * (sy[i + 1] - sy[i]));
        hgt = L > 0.0f ? 2.0f * area / L : 0.0f;
        if (sSliverN < 40 && in > 0 && area > 0.2f && (L > 120.0f && hgt < 3.0f || !(sw[i] > 0.0f && sw[i + 1] > 0.0f && sw[i + 2] > 0.0f))) {
            const float *v0 = (const float *)(const void *)(vb + (size_t)(p->at + i) * 40);
            Vita_Log("sliver: picture %u layer %d strip of %u at %u, triangle %u: screen (%.0f,%.0f w %.3g) (%.0f,%.0f w %.3g) (%.0f,%.0f w %.3g)\n",
                     gGsFrame, layer, n, p->at, i, sx[i], sy[i], sw[i], sx[i + 1], sy[i + 1], sw[i + 1], sx[i + 2], sy[i + 2], sw[i + 2]);
            for (j = 0; j < 3; j++) {
                const float *v = v0 + j * 10;
                Vita_Log("sliver:   vertex %u: position %g %g %g weight %g | normal %g %g %g | st %g %g\n", i + j, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[8], v[9]);
            }
            Vita_Log("sliver:   pivots A %g %g %g B %g %g %g | A row0 %g %g %g %g | B row0 %g %g %g %g | clip w row %g %g %g %g\n",
                     c[32], c[33], c[34], c[36], c[37], c[38], c[0], c[1], c[2], c[3], c[16], c[17], c[18], c[19], c[75], c[79], c[83], c[87]);
            sSliverN++;
            return;
        }
    }
}
/* the fighters' shading layer: its ramp (Frieza "too dark"), once for each texture */
extern uint32_t Gs_TexelOf(uint64_t t0, int u, int v);
static void shade_log(const char *where, uint64_t t0, int ctx, const float *c) {
    extern int gVitaShadeLog;
    if (!gVitaShadeLog) {
        return; /* (bt3.txt shadelog=1) */
    }
    static uint64_t seen[32];
    static int nSeen;
    uint32_t tw = 1u << ((t0 >> 26) & 15), th = 1u << ((t0 >> 30) & 15), u;
    char line[420];
    int j, len;
    for (j = 0; j < nSeen && seen[j] != t0; j++) {
    }
    if (j < nSeen || nSeen == 32) {
        return;
    }
    seen[nSeen++] = t0;
    len = snprintf(line, sizeof(line), "vita: shading layer (%s, b206): TEX0 %08x%08x %ux%u", where, (unsigned)(t0 >> 32), (unsigned)t0, tw, th);
    if (ctx >= 0) {
        len += snprintf(line + len, sizeof(line) - (unsigned)len, " CLAMP %llx TEX1 %llx ALPHA %llx PRIM %03x", (unsigned long long)gGs.clamp[ctx],
                        (unsigned long long)gGs.tex1[ctx], (unsigned long long)gGs.alpha[ctx], (unsigned)(gGs.prim & 0x7FF));
    }
    if (c != NULL) {
        len += snprintf(line + len, sizeof(line) - (unsigned)len, " | light %.3f %.3f %.3f %.3f | colours %.0f %.0f %.0f %.0f / %.0f %.0f %.0f %.0f", c[40], c[44], c[48], c[52],
                        c[88], c[89], c[90], c[91], c[92], c[93], c[94], c[95]);
    }
    Vita_Log("%s\n", line);
    {   /* the ramp as read (row 0), 32 places along it, and the column at u 0 */
        char l2[480];
        int len2 = snprintf(l2, sizeof(l2), "vita:   ramp row 0 (b207):");
        for (u = 0; u < 32 && len2 > 0 && len2 < (int)sizeof(l2) - 12; u++) {
            len2 += snprintf(l2 + len2, sizeof(l2) - (unsigned)len2, " %06x", (unsigned)Gs_TexelOf(t0, (int)(u * (tw - 1u) / 31u), 0) & 0xFFFFFF);
        }
        Vita_Log("%s\n", l2);
        len2 = snprintf(l2, sizeof(l2), "vita:   ramp column 0 (b207):");
        for (u = 0; u < 16 && len2 > 0 && len2 < (int)sizeof(l2) - 12; u++) {
            len2 += snprintf(l2 + len2, sizeof(l2) - (unsigned)len2, " %06x", (unsigned)Gs_TexelOf(t0, 0, (int)(u * (th - 1u) / 15u)) & 0xFFFFFF);
        }
        Vita_Log("%s\n", l2);
    }
}
int GsGpu_DsLayer(int layer, int ctx, uint32_t count, const float *c, const void *plan) {
    ag_close(); /* the additive group before anything else */
    if (layer == 1) {
        shade_log("direct", gGs.tex0[ctx], ctx, c);
    }
    const ChipPlan *p = (const ChipPlan *)plan;
    State d;
    float u17[3];
    if (!sDsOn || gDpCapture || sDumping || p->mode != 1 || count < 3 || count > 1024) {
        return 0;
    }
    ZM(4);
    {   /* A fighter's strips come one after another with the same registers, layer by layer: the state of the strip
           before is this one's while the registers, GS memory (the epoch) and the buffers drawn into are as they
           were. (state_of's own memory hashes and checks the pages: 600 cycles a layer, 4,000 layers a picture.) */
        /* four states a layer, not one: a fighter's strips take turns between textures (the body's, the
           face's), and each turn worked its state out again (13,800 in two seconds of Kame House, "other texture") */
        static struct { uint64_t r[11]; unsigned frame, epoch, wgen; int ctx, valid; State d; } lastK[2][4], *lp;
        static unsigned lastNext[2];
        uint64_t r[11];
        int L = layer & 1, ok, k;
        r[0] = gGs.prim; r[1] = gGs.tex0[ctx]; r[2] = gGs.test[ctx]; r[3] = gGs.clamp[ctx]; r[4] = gGs.scissor[ctx];
        r[5] = gGs.frame[ctx]; r[6] = gGs.zbuf[ctx]; r[7] = gGs.alpha[ctx]; r[8] = gGs.tex1[ctx]; r[9] = gGs.texa; r[10] = gGs.fba[ctx];
        for (k = 0; k < 4; k++) {
            lp = &lastK[L][k];
            if (lp->valid && lp->frame == gGsFrame && lp->epoch == gGsEpoch && lp->wgen == sWorkGen && lp->ctx == ctx && memcmp(lp->r, r, sizeof(r)) == 0) {
                break;
            }
        }
        if (k < 4) {
            d = lp->d;
            ok = 1;
            gT121Fs[0]++;
        } else {
            for (k = 0; k < 4 && lastK[L][k].r[1] != r[1]; k++) {
            }
            lp = &lastK[L][k < 4 ? k : (int)(lastNext[L]++ & 3)]; /* (this texture's place, or the next in turn) */
            gT121Fs[!lp->valid ? 1 : lp->frame != gGsFrame ? 2 : lp->epoch != gGsEpoch ? 3 : lp->wgen != sWorkGen ? 4 : lp->ctx != ctx ? 5 :
                    lp->r[1] != r[1] ? 6 : 7]++;
            ok = state_of(ctx, 3, &d);
            lp->valid = ok && !wp_is_tex(d.tex); /* (the work page's picture is not kept) */
            if (ok) {
                memcpy(lp->r, r, sizeof(r));
                lp->frame = gGsFrame;
                lp->epoch = gGsEpoch;
                lp->wgen = sWorkGen;
                lp->ctx = ctx;
                lp->d = d;
            }
        }
        if (!ok) {
            goto rejected;
        }
    }
    if (0) {
    rejected:
        if (sFsOn) {
            fs_add(layer, ctx, NULL, 0, c, count);
        }
        gVitaRej[1][sRejWhy & 7]++;
        if (p->upload && layer == 0) { /* not drawn, but the vertices planned for the chip are written */
            ft_upload(p);
        }
        sVu0Have = 0;
        return 1;
    }
    if (sFsOn) {
        fs_add(layer, ctx, &d, 1, c, count);
    }
    sSeeThru |= d.mask == 8; /* */
    ZM(5);
    flush(); /* GS primitives that came before */
    if (p->upload && layer == 0) {
        ft_upload(p);
    }
    u17[0] = (float)(gGs.xyoffset[ctx] & 0xFFFF) / 16.0f;
    u17[1] = (float)((gGs.xyoffset[ctx] >> 32) & 0xFFFF) / 16.0f;
    u17[2] = ((gGs.zbuf[ctx] >> 24) & 15) == 0 ? 4294967295.0f : ((gGs.zbuf[ctx] >> 24) & 15) == 1 ? 16777215.0f : 65535.0f;
    sVu0Have = 0; /* (GsGpu_DrawVu0's constants are not these) */
    if (!ds_strip_x(&d, layer, u17, NULL, c, p)) {
        return 0;
    }
    if (VITA_RARE(gVitaSliver) && layer <= 1) { /* (the shading layer too) */
        sliver_check(layer, c, p, u17);
    }
    gVglVuTris += count - 2;
    gVglVuBatches++;
    gVitaFt[0]++;
    ZM(29);
    return 1;
}

/* The game's side (gfxm_e_b.c ObjShadow_BeginRender, on the game's thread): is the silhouette of shadow
   `slot` drawn now (1), or is the shadow laid with the one drawn before (0)? The silhouette is a whole second pass
   over the fighter (1,000 strips a picture with two big ones); in mode 2 each is drawn every other picture, the two
   fighters in turn, and laid every picture where the fighter is now (only its pose a picture old). Never twice in a
   row with the one before, and not when the one before is older than a picture and a half (a fight's start, a
   slow stretch). */
int gVitaSilMode = 2;
static unsigned long long sSilAt[WP_OTHER];
static unsigned sSilTick, sSilOrder;
int Port_ShadowSil(int slot) {
    unsigned n = sSilOrder++;
    unsigned long long now = sceKernelGetProcessTimeWide();
    if (slot < 0 || slot >= WP_OTHER) {
        return 1;
    }
    if (gVitaSilMode == 2 && sSilAt[slot] != 0 && now - sSilAt[slot] < 50000ull && ((sSilTick + n) & 1)) {
        sSilAt[slot] = 0; /* (the next is drawn whatever comes) */
        return 0;
    }
    sSilAt[slot] = now;
    return 1;
}
void Port_ShadowSilEnd(void) { /* (ObjShadow_ResetEnv: the shadows of this picture are over) */
    sSilTick++;
    sSilOrder = 0;
}

/* gs_core.c GR_VU: a planned strip of program 2 (a fighter's silhouette, layer 2) into the work page,
   straight to the direct draws as the fighters' own strips (GsGpu_DsLayer): its state as the strip before's while
   the registers are, the page's clear first. Were batches of their own, and half of them the general way (1,000
   strips a picture in the heaviest pairing). 0: the general way. */
int GsGpu_DsSil(int ctx, uint32_t count, const float *c, const void *plan) {
    ag_close(); /* the additive group before anything else */
    static struct { uint64_t r[11]; unsigned frame, epoch, wgen; int ctx, valid, off, slot; State d; } last;
    const ChipPlan *p = (const ChipPlan *)plan;
    State d;
    float u17[3];
    uint64_t r[11];
    if (!sDsOn || gDpCapture || sDumping || !sWpReady || p == NULL || p->mode != 1 || count < 3 || count > 1024) {
        return 0;
    }
    ZM(4);
    r[0] = gGs.prim; r[1] = gGs.tex0[ctx]; r[2] = gGs.test[ctx]; r[3] = gGs.clamp[ctx]; r[4] = gGs.scissor[ctx];
    r[5] = gGs.frame[ctx]; r[6] = gGs.zbuf[ctx]; r[7] = gGs.alpha[ctx]; r[8] = gGs.tex1[ctx]; r[9] = gGs.texa; r[10] = gGs.fba[ctx];
    if (last.valid && last.frame == gGsFrame && last.epoch == gGsEpoch && last.wgen == sWorkGen && last.ctx == ctx && last.off == gVitaWpOff &&
        last.slot == sWpSlot && memcmp(last.r, r, sizeof(r)) == 0) {
        d = last.d;
    } else {
        int ok;
        last.valid = 0;
        if ((r[5] & 0x1FF) != WP_FBP) {
            return 0; /* (not into the page: the flat shadow, the general way) */
        }
        sWpAllow = 1;
        ok = state_of(ctx, 3, &d);
        sWpAllow = 0;
        if (!ok || !d.target || d.tme) {
            return 0; /* (refused, or not into the page: the general way, which counts it) */
        }
        memcpy(last.r, r, sizeof(r));
        last.frame = gGsFrame;
        last.epoch = gGsEpoch;
        last.wgen = sWorkGen;
        last.ctx = ctx;
        last.off = gVitaWpOff;
        last.slot = sWpSlot;
        last.d = d;
        last.valid = 1;
    }
    wp_emit_clear(); /* (the page's clear held back: drawn first) */
    ZM(5);
    flush(); /* GS primitives that came before */
    if (p->upload) {
        ft_upload(p);
    }
    u17[0] = (float)(gGs.xyoffset[ctx] & 0xFFFF) / 16.0f;
    u17[1] = (float)((gGs.xyoffset[ctx] >> 32) & 0xFFFF) / 16.0f;
    u17[2] = ((gGs.zbuf[ctx] >> 24) & 15) == 0 ? 4294967295.0f : ((gGs.zbuf[ctx] >> 24) & 15) == 1 ? 16777215.0f : 65535.0f;
    sVu0Have = 0; /* (GsGpu_DrawVu0's constants are not these) */
    if (!ds_strip_x(&d, 2, u17, NULL, c, p)) {
        gVitaWp[9]++;
        return 0;
    }
    gVglL2[0]++;
    gVglL2Drawn = 1;
    gVitaWp[8]++;
    gVglVuTris += count - 2;
    gVglVuBatches++;
    ZM(29);
    return 1;
}

/* The waiting groups are drawn: the texture layers and the stage first, then the shading layers (each lies on
   the texture layer of its part), each in runs of the same state. */
/* the kept parts' draws, from the items given (the drawing thread's own, or the render thread's copy) */
unsigned gVitaOrderRuns[2]; /* for the log: runs begun anew to keep the order, draws put with an earlier run */
static void dp_draw_items(DpItem *items, int n, int prof, unsigned frame) {
    static struct { State st; uint32_t h; int mode, first, last; } bk[512];
    static int16_t bhead[512], bnext[512]; /* (the runs found by their number, not one after the other) */
    int lastUnsafe = -1; /* the run begun last that is not st_commutes */
    /* pass 2: the silhouettes, into the work page; first when the page is the target now (its clear was
       the draw before), so that the target changes once */
    static const int kOrder[2][3] = {{0, 1, 2}, {2, 0, 1}};
    int pass, i, nb, k, page = 0;
    for (i = 0; i < n && !page; i++) {
        page = items[i].g->layer == 2;
    }
    for (k = 0; k < 3; k++) {
        pass = kOrder[page && sTgtCur != 0][k];
        if (pass == 2 && !page) {
            continue;
        }
        nb = 0;
        lastUnsafe = -1;
        memset(bhead, 0xFF, sizeof(bhead));
        for (i = 0; i < n; i++) {
            DpItem *it = &items[i];
            State st;
            uint32_t h, hs;
            int b, safe;
            if ((it->g->layer == 1 ? 1 : it->g->layer == 2 ? 2 : 0) != pass) {
                continue;
            }
            st = it->g->st;
            st.tex = it->tex;
            st.tme = st.tme && it->tex != 0;
            h = state_hash(&st) ^ (uint32_t)it->g->mode * 0x9E3779B1u;
            hs = (h * 0x2545F491u) >> 23;
            for (b = bhead[hs]; b >= 0 && !(bk[b].h == h && bk[b].mode == it->g->mode && memcmp(&bk[b].st, &st, sizeof(st)) == 0); b = bnext[b]) {
            }
            safe = st_commutes(&st) || !gVitaOrder || it->g->mode == 1; /* (the fighters' parts: together as before) */
            if (b >= 0 && it->g->mode != 1 && !(safe ? lastUnsafe < b : b == nb - 1)) {
                b = -1; /* its place kept: a run of its own */
                gVitaOrderRuns[0]++;
            } else if (b >= 0 && b != nb - 1) {
                gVitaOrderRuns[1]++;
            }
            if (b < 0) {
                if (nb == 512) {
                    b = 511; /* (not reached; drawn with that run's state at worst) */
                } else {
                    b = nb++;
                    if (!safe) {
                        lastUnsafe = b;
                    }
                    bk[b].st = st;
                    bk[b].h = h;
                    bk[b].mode = it->g->mode;
                    bk[b].first = i;
                    bk[b].last = i;
                    bnext[b] = bhead[hs];
                    bhead[hs] = (int16_t)b;
                    it->next = -1;
                    continue;
                }
            }
            items[bk[b].last].next = i;
            bk[b].last = i;
            it->next = -1;
        }
        if (nb > 0 && pass != 2) {
            target_set(0);
        }
        ZM(40); /* (the parts of these draws, zones 40..43) */
        for (i = 0; i < nb; i++) {
            const State *s = &bk[i].st;
            int mode = bk[i].mode, kind = mode == 1 ? 3 : 4, q;
            int tst = s->atest && s->afunc != 1 && !(s->afunc == 5 && s->aref == 0);
            int fm = s->tme && s->tex != 0 ? (s->env == 1 ? 2 : s->env == 2 ? 3 : 1) : 0;
            if (prof == 7 || (prof == 5 && mode != 1) || (prof == 6 && mode == 1)) {
                continue; /* (the GPU profile: this part left out) */
            }
            if (pass == 2) {
                target_set(s->target); /* the silhouettes: the page's slot */
            }
            /* a fighter drawn into the alpha only (seen through, GfxAlphaKey): the texture's alpha as it is
               (the GS's byte, 0xF4..0xFE), which the picture's alpha cannot hold in its usual scale (0x80 = 1) */
            int raw = mode == 1 && s->mask == 8 && (fm == 1 || fm == 2) && !tst;
            const Variant *var = variant(kind, fm | tst << 2 | (sNoProj && fm ? 8 : 0) | (raw ? 512 : gVglLean ? 32 : 0));
            uint32_t usize = sDpUSize[mode == 1 ? 0 : 1];
            int newProg;
            if (var == NULL) {
                continue;
            }
            newProg = !sGlc.valid || sGlc.prog != var->prog;
            sVarUse = var;
            if (newProg) {
                glUseProgram(var->prog);
                sGlc.prog = var->prog;
            }
            if (fm) {
                int tp = s->linear | s->wrapS << 1 | s->wrapT << 2;
                if (!sGlc.valid || sGlc.tex != s->tex) {
                    glBindTexture(GL_TEXTURE_2D, s->tex);
                    sGlc.tex = s->tex;
                    sGlc.texp = -1;
                }
                if (sGlc.texp != tp) {
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s->linear ? GL_LINEAR : GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s->linear ? GL_LINEAR : GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, s->wrapS ? GL_CLAMP_TO_EDGE : GL_REPEAT);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, s->wrapT ? GL_CLAMP_TO_EDGE : GL_REPEAT);
                    sGlc.texp = tp;
                }
                if (newProg && var->tex >= 0) {
                    glUniform1i(var->tex, 0);
                }
            }
            if (tst && var->test >= 0) {
                glUniform4f(var->test, (float)(s->afunc + 1), (float)s->aref / 128.0f, 0.0f, 0.0f);
            }
            apply_fixed(s);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glEnableVertexAttribArray(2);
            if (mode == 1) {
                glBindBuffer(GL_ARRAY_BUFFER, sFtVbo);
                glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 40, (const void *)0);
                glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 40, (const void *)16);
                glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 40, (const void *)32);
            } else {
                glBindBuffer(GL_ARRAY_BUFFER, sStageVbo);
                glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 32, (const void *)0);
                glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 32, (const void *)24);
                glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (const void *)16);
            }
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sDpIbo);
            {
                unsigned long long d0 = sceKernelGetProcessTimeWide();
                ZM(41);
                if (vglFastPrepare(GL_UNSIGNED_SHORT)) {
                    ZM(42);
                    /* The stage's groups of one state with the same constants (parts placed in the world as they
                       are: the same screen matrix) as ONE draw: their indices one after the other in this frame's
                       memory. A draw costs the processor ~1,300 cycles, a group's indices ~ a hundred to copy. */
                    static int pend[64];
                    int np = 0, merge = mode != 1 && sDpIboShadow != NULL && (gVglSafe & 16777216); /* (on request: a test, the copying cost more than the draws) */
                    uint32_t pcount = 0;
                    for (q = bk[i].first;; q = items[q].next) {
                        const DpGroup *g = q >= 0 ? items[q].g : NULL;
                        if (g != NULL && g->icount == 0) {
                            continue;
                        }
                        if (g != NULL && VITA_RARE(gVitaBisList >= 0 || gVitaDrawLogTo >= 0) &&
                            bis_draw(frame, mode == 1 ? 16 : 8, s, (int)g->icount, (int)g->ioff | g->layer << 24, NULL)) {
                            continue;
                        }
                        if (g != NULL && merge && g->iptr == NULL && np > 0 && np < 64 && pcount + g->icount <= 196608 &&
                            (items[pend[0]].u == items[q].u || memcmp(items[pend[0]].u, items[q].u, usize) == 0)) {
                            pend[np++] = q;
                            pcount += g->icount;
                            gVitaDpMerge[0]++;
                            continue;
                        }
                        if (np == 1) { /* what waits: drawn */
                            vglFastDraw(GL_UNSIGNED_SHORT, items[pend[0]].g->ioff, items[pend[0]].g->icount, zl_u(items[pend[0]].u, usize, s, mode), usize);
                            gVitaDp[1]++;
                        } else if (np > 1) {
                            uint16_t *ix = (uint16_t *)vglAllocFromScratch(pcount * 2);
                            int k;
                            if (ix != NULL) {
                                uint16_t *o = ix;
                                for (k = 0; k < np; k++) {
                                    const DpGroup *pg = items[pend[k]].g;
                                    memcpy(o, sDpIboShadow + pg->ioff, pg->icount * 2);
                                    o += pg->icount;
                                }
                                vglFastDrawPtr(GL_UNSIGNED_SHORT, ix, pcount, zl_u(items[pend[0]].u, usize, s, mode), usize);
                                gVitaDp[1]++;
                                gVitaDpMerge[1]++;
                            } else {
                                for (k = 0; k < np; k++) {
                                    vglFastDraw(GL_UNSIGNED_SHORT, items[pend[k]].g->ioff, items[pend[k]].g->icount, zl_u(items[pend[k]].u, usize, s, mode), usize);
                                    gVitaDp[1]++;
                                }
                            }
                        }
                        np = 0;
                        pcount = 0;
                        if (g == NULL) {
                            break;
                        }
                        if (merge && g->iptr == NULL) {
                            pend[np++] = q;
                            pcount = g->icount;
                            continue;
                        }
                        if (g->iptr != NULL) {
                            vglFastDrawPtr(GL_UNSIGNED_SHORT, g->iptr, g->icount, zl_u(items[q].u, usize, s, mode), usize);
                        } else {
                            vglFastDraw(GL_UNSIGNED_SHORT, g->ioff, g->icount, zl_u(items[q].u, usize, s, mode), usize);
                        }
                        gVitaDp[1]++;
                    }
                }
                gVitaWall[5] += sceKernelGetProcessTimeWide() - d0;
                ZM(43);
            }
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
            gVitaDp[2]++;
            gVglBatches++;
        }
    }
}
/* the render thread's copy of the items, their groups and their uniform blocks */
typedef struct RtDp { int n, prof; uint32_t uf, frame; } RtDp;
static int rt_put_dp(void) {
    uint32_t n = (uint32_t)sDpItemN, uf = (uint32_t)sDpUN, i;
    uint32_t oi = a16((uint32_t)sizeof(RtDp)), og = oi + a16(n * (uint32_t)sizeof(DpItem)), ou = og + a16(n * (uint32_t)sizeof(DpGroup)), total = ou + uf * 4u;
    uint8_t *p;
    RtDp *d;
    DpItem *it;
    DpGroup *g;
    float *u;
    if (total > RT_RING / 4) {
        return 0;
    }
    p = rt_reserve(RT_DP, total);
    d = (RtDp *)(void *)p;
    it = (DpItem *)(void *)(p + oi);
    g = (DpGroup *)(void *)(p + og);
    u = (float *)(void *)(p + ou);
    d->n = (int)n;
    d->prof = sProf;
    d->frame = gGsFrame;
    d->uf = uf;
    memcpy(u, sDpU, uf * 4u);
    for (i = 0; i < n; i++) {
        g[i] = *sDpItem[i].g;
        it[i] = sDpItem[i];
        it[i].g = &g[i];
        it[i].u = u + (sDpItem[i].u - sDpU);
    }
    rt_commit();
    rt_kick();
    return 1;
}
static void rt_exec_dp(uint8_t *p) {
    const RtDp *d = (const RtDp *)(const void *)p;
    uint32_t n = (uint32_t)d->n;
    dp_draw_items((DpItem *)(void *)(p + a16((uint32_t)sizeof(RtDp))), d->n, d->prof, d->frame);
    (void)n;
}
static void dp_draw_all(void) {
    ag_close(); /* the additive group before anything else */
    State keep = sCur;
    int keepHave = sHaveState, keepCount = sVertCount;
    unsigned long long t0;
    ds_close();
    if (sDpItemN == 0) {
        return;
    }
    if (VITA_RARE(sCovSample)) {
        run_add(-1, sDpItemN, 0.0f);
    }
    t0 = sceKernelGetProcessTimeWide();
    if (!(sRtOn && rt_put_dp())) {
        if (sRtOn) {
            rt_sync(); /* (too large for the ring: drawn here, the ring emptied first) */
        }
        dp_draw_items(sDpItem, sDpItemN, sProf, gGsFrame);
    }
    sDpItemN = 0;
    sDpUN = 0;
    sDsGroupN = 0;
    gVitaDpUs += sceKernelGetProcessTimeWide() - t0;
    sCur = keep;
    sHaveState = keepHave;
    sVertCount = keepCount;
}

/* The chip's buffers are let go (a fight ended): what was kept refers to them (gs_core.c has dropped its parts) */
void GsGpu_DpReset(void) {
    ag_close(); /* the additive group before anything else */
    flush_bins_c(14);
    rt_sync();
    glFinish();
    memset(sWpSlotOk, 0, sizeof(sWpSlotOk)); /* (another fight, other fighters in the slots) */
    dp_free_pending();
    sDpIboUsed = 0;
    memset(sDpIh, 0, sizeof(sDpIh));
    sDpIhN = 0;
}

static void vu_draw_impl(int kind, int layer, int ctx, const float *vertices, uint32_t count, const float (*u)[4]);
__attribute__((hot)) static void vu_draw(int kind, int layer, int ctx, const float *vertices, uint32_t count, const float (*u)[4]) {
    P_IN(6);
    vu_draw_impl(kind, layer, ctx, vertices, count, u);
    gVglCalls[0]++;
    P_OUT;
}
__attribute__((hot)) static void vu_draw_impl(int kind, int layer, int ctx, const float *vertices, uint32_t count, const float (*u)[4]) {
    static struct { const float *vertices, *jv; uint32_t count; unsigned gen, vjgen, jvAt; const V *out; int layer; } last;
    extern int gVglVertsStable; /* gs_vu1.c: these vertices lie in the display list and stay there until it ends */
    const V *same = NULL;
    State d;
    uint32_t stride = kind == 2 ? 16 : 12;
    int b, need = (int)(count - 2) * 3, base, stable = gVglVertsStable, samePos = gVglSamePos;
    uint32_t dh;
    VJob job;

    gVglVertsStable = 0;
    gVglSamePos = 0;
    ZM(4);
    sWpAllow = kind == 0 && layer == 2; /* the fighters' silhouettes into the work page (program 2); planned too */
    b = count >= 3 && count <= 1024 && state_of(ctx, 3, &d);
    sWpAllow = 0;
    if (b && d.target) {
        wp_emit_clear(); /* the page's clear held back: drawn first */
    }
    if (!b) {
        if (layer == 2) {
            gVglL2[1]++;
        }
        if (gDpCapture) {
            sDpRecBad = 1;
        }
        if (gVglPlan != NULL && ((const ChipPlan *)gVglPlan)->upload && ((const ChipPlan *)gVglPlan)->mode == 1) {
            ft_upload((const ChipPlan *)gVglPlan); /* not drawn, but the vertices the front end placed are written */
        } else if (gVglPlan != NULL && ((const ChipPlan *)gVglPlan)->upload) { /* not drawn, but the vertices the front end placed are written */
            const ChipPlan *p = (const ChipPlan *)gVglPlan;
            uint8_t *map;
            map = p->mode == 1 ? sFtVboPtr : sStageVboPtr;
            if (map != NULL) {
                uint32_t sz = p->mode == 1 ? 40u : 32u;
                memcpy(map + (size_t)p->at * sz, (const uint8_t *)p + 32, p->count * sz);
            }
        }
        gVglPlan = NULL;
        if (VITA_RARE(sDumping)) {
            dump_state(kind == 0 ? "strip0" : kind == 1 ? "strip4" : "strip6", ctx, 100 + layer, NULL, 0, 0, 0, 0, count);
        }
        return;
    }
    if (VITA_RARE(sDumping)) {
        dump_state(kind == 0 ? "strip0" : kind == 1 ? "strip4" : "strip6", ctx, 100 + layer, &d, 0, 0, 0, 0, count);
    }
    sSeeThru |= kind == 0 && d.mask == 8; /* */
    if (layer == 2 && kind == 0) {
        gVglL2[0]++;
        gVglL2Drawn = 1;
    }
    ZM(5);
    flush(); /* GS primitives that came before */
    ZM(6);
    if (gVglSafe & 512) {
        flush_bins_c(10); /* (to find a fault: one batch at a time) */
    }
    if (gVglPlan != NULL) { /* (the list is split: planned by the front end) */
        const ChipPlan *p = (const ChipPlan *)gVglPlan;
        gVglPlan = NULL;
        if (gDpCapture) {
            if (layer == 2) {
                sDpRecBad = 1; /* the silhouettes go the batches' way (chip_emit), not into kept parts */
            } else {
                dp_record(&d, layer, u, p, ctx);
            }
        }
        ZM(29);
        if (p->mode == 1 && !gDpCapture && sDsOn && !sDumping) {
            if (p->upload) { /* (written to the chip first, as chip_emit does) */
                ft_upload(p);
            }
            if (ds_strip(&d, layer, u, p)) {
                gVglVuTris += count - 2;
                gVglVuBatches++;
                gVitaFt[0]++;
                last.vertices = NULL;
                return;
            }
            gVitaDs[3]++;
        }
        chip_emit(&d, layer, u, p);
        gVglVuTris += count - 2;
        gVglVuBatches++;
        last.vertices = NULL;
        return;
    }
    if (gDpCapture) {
        sDpRecBad = 1; /* (a strip not planned: the part is not kept) */
    }
    if (gGsSplit) {
        goto not_stage; /* (the caches are the front end's: a strip it did not plan is drawn the general way) */
    }
    if (kind == 0 && layer <= 1) {
        int done;
        { P_IN(10); done = fighter_draw(&d, layer, vertices, count, u); P_OUT; }
        if (!done) {
            goto not_fighter;
        }
        gVglVuTris += count - 2;
        gVglVuBatches++;
        last.vertices = NULL;
        return;
    }
not_fighter:
    if (kind == 1 && stable && sStageProg != 0) {
        int done;
        { P_IN(10); done = stage_draw(&d, vertices, count, u); P_OUT; }
        if (!done) {
            goto not_stage;
        }
        gVglVuTris += count - 2;
        gVglVuBatches++;
        last.vertices = NULL;
        return;
    }
not_stage:
    dh = state_hash(&d);
    for (b = sBinCount - 1; b >= 0 && (sBinIsStage[b] || sBinHash[b] != dh || memcmp(&d, &sBin[b].st, sizeof(d)) != 0 || sBin[b].count + (int)count > BIN_VERTS || sBin[b].icount + need > BIN_IDX); b--) {
    } /* (the latest such batch first) */
    if (b < 0 || !bin_join_ok(b, &d)) {
        b = sBinCount;
    }
    if (b == sBinCount) {
        if (sBinCount >= BINS_V) {
            flush_bins_c(11);
        }
        b = sBinCount++;
        bin_made(b, &d);
        sBin[b].st = d;
        sBin[b].count = 0;
        sBin[b].icount = 0;
        sBinIsStage[b] = 0;
        sBinHash[b] = dh;
    }
    base = sBin[b].count;
    /* a fighter's strip comes twice (texture layer, shading layer): the positions are worked out once */
    if (samePos && kind == 0 && last.vertices == vertices && last.count == count && last.gen == sBinGen && layer != 2 && layer != 4 &&
        last.layer != 2 && last.layer != 4) {
        same = last.out;
    }
    job.kind = (uint8_t)kind;
    job.layer = (uint8_t)layer;
    job.zl = kind == 1 && gVitaZLayer != 0 && d.ztest && !d.zwrite && d.zfunc == 2 ? (float)gVitaZLayer * 2.0f / u[17][2] : 0.0f;
    job.page = d.target;
    job.count = (uint16_t)count;
    job.out = &sBinVerts[b][base];
    job.ix = &sBinIdx[b][sBin[b].icount];
    job.base = base;
    job.same = same;
    job.v = vertices;
    job.u = u;
    sBin[b].count = base + (int)count;
    sBin[b].icount += need;
    gVglVuTris += count - 2;
    gVglVuBatches++;
    if (!sVjOn) {
        vu_transform(&job);
    } else {
        uint32_t vbytes = count * stride * 4;
        unsigned from;
        int lastLive = last.vjgen == sVjGen;

        while (sVjTail - sVjHead >= VJ_MAX - 1) {
            vj_help();
        }
        /* the constants: the copy made for the strip before, if they are the same (a fighter's two layers) */
        if (sVjLastU == NULL || !lastLive || (!sSameU && memcmp(sVjLastU, u, 384) != 0)) {
            unsigned g0 = sVjGen;
            uint8_t *m = vj_room(384);
            if (g0 != sVjGen) {
                lastLive = 0;
            }
            memcpy(m, u, 384);
            sVjLastU = m;
            sVjLastUAt = sVjUsed - 384;
        }
        job.u = (const float (*)[4])sVjLastU;
        from = sVjLastUAt;
        if (stable) {
            gVitaVj[1]++;
        } else if (same != NULL && lastLive && last.jv != NULL && last.jvAt - sVjFree < VJ_ARENA) {
            job.v = last.jv; /* (the copy made for the layer before) */
            if ((int)(last.jvAt - from) < 0) {
                from = last.jvAt;
            }
        } else if (vbytes + 384 > VJ_ARENA / 2) {
            vj_join(); /* (never: a strip is 64 KB at most) */
            vu_transform(&job);
            goto done;
        } else {
            unsigned g0 = sVjGen;
            uint8_t *m = vj_room(vbytes);
            if (g0 != sVjGen) { /* the ring was emptied meanwhile: the constants are gone with it */
                uint8_t *k = vj_room(384);
                memcpy(k, u, 384);
                sVjLastU = k;
                sVjLastUAt = sVjUsed - 384;
                job.u = (const float (*)[4])sVjLastU;
                m = vj_room(vbytes);
                from = sVjLastUAt;
            }
            memcpy(m, vertices, vbytes);
            job.v = (const float *)(const void *)m;
            last.jvAt = sVjUsed - ((vbytes + 15) & ~15u);
            gVitaVj[2]++;
        }
        sVjFrom[sVjTail & (VJ_MAX - 1)] = from;
        sVj[sVjTail & (VJ_MAX - 1)] = job;
        __sync_synchronize();
        sVjTail = sVjTail + 1;
        gVitaVj[0]++;
    }
done:
    last.vertices = kind == 0 ? vertices : NULL;
    last.jv = job.v;
    last.vjgen = sVjGen;
    last.count = count;
    last.gen = sBinGen;
    last.out = job.out;
    last.layer = layer;
}

__attribute__((hot)) static void vu_misc(float (*u)[4], int ctx, float layer, float kind) {
    u[17][0] = (float)(gGs.xyoffset[ctx] & 0xFFFF) / 16.0f;
    u[17][1] = (float)((gGs.xyoffset[ctx] >> 32) & 0xFFFF) / 16.0f;
    u[17][2] = ((gGs.zbuf[ctx] >> 24) & 15) == 0 ? 4294967295.0f : ((gGs.zbuf[ctx] >> 24) & 15) == 1 ? 16777215.0f : 65535.0f;
    u[17][3] = layer;
    u[19][0] = kind;
}

__attribute__((hot)) void GsGpu_DrawVu0(int layer, int ctx, const float *vertices, uint32_t count, const float *consts) {
    if (layer == 1) {
        shade_log("strip", gGs.tex0[ctx], ctx, consts);
    }
    ag_close(); /* the additive group before anything else */
    static float u[24][4]; /* (20..23 program 0's clip-test matrix, u[19][1] set when it is there) */
#define have sVu0Have

    ZM(17);
    /* a fighter's strip comes twice, texture layer and shading layer, with nothing changed in between
       (gs_vu1.c says so): the constants are not put together and compared a second time */
    sSameU = have && gVglSamePos;
    if (!sSameU) {
        memset(u, 0, sizeof(u));
        memcpy(u[0], &consts[0], 64);
        memcpy(u[4], &consts[16], 64);
        memcpy(u[8], &consts[32], 16);
        memcpy(u[9], &consts[36], 16);
        memcpy(u[10], &consts[56], 64);
        u[14][0] = consts[40]; u[14][1] = consts[44]; u[14][2] = consts[48]; u[14][3] = consts[52];
        memcpy(u[15], &consts[88], 16);
        memcpy(u[16], &consts[92], 16);
        u[18][0] = consts[41]; u[18][1] = consts[45]; u[18][2] = consts[49]; u[18][3] = consts[53];
        vu_misc(u, ctx, 0.0f, 0.0f);
        if ((layer == 0 || layer == 1 || layer == 5) && !gVitaClipOff) { /* programs 0 and 1: their reject (VU 18..21) */
            memcpy(u[20], &consts[72], 64);
            u[19][1] = 1.0f;
        }
    }
    have = 1;
#undef have
    vu_draw(0, layer, ctx, vertices, count, u);
    sSameU = 0;
}

__attribute__((hot)) void GsGpu_DrawVu4(int ctx, const float *vertices, uint32_t count, const float *consts) {
    ag_close(); /* the additive group before anything else */
    float u[24][4];

    memset(u, 0, sizeof(u));
    memcpy(u[10], &consts[0], 64);
    vu_misc(u, ctx, 0.0f, 1.0f);
    vu_draw(1, 0, ctx, vertices, count, u);
}

__attribute__((hot)) void GsGpu_DrawVu6(int ctx, const float *vertices, uint32_t count, const float *consts) {
    ag_close(); /* the additive group before anything else */
    float u[24][4];

    memset(u, 0, sizeof(u));
    memcpy(u[0], &consts[0x0C * 4], 64);
    u[8][0] = consts[0x10 * 4];
    u[9][0] = 1.0f;
    u[9][1] = 1.0f;
    memcpy(u[10], &consts[0], 64);
    vu_misc(u, ctx, 0.0f, 2.0f);
    vu_draw(2, 0, ctx, vertices, count, u);
}
/* Markers the HUD code puts into the list (PC port): which side of the screen the 2D pieces that follow belong to. */
static int sAnchor; /* 0 none, 1 left, 2 right, 3 centre, 4 by position */
void GsGpu_Native(int effect) {
    ag_close(); /* the additive group before anything else */
    if (effect == 1) {
        ol_marker(); /* the outline */
        return;
    }
    if (effect >= 0x20 && effect <= 0x2F) {
        wp_marker(effect); /* the ground shadows' slots */
        return;
    }
    if (effect >= 0x40 && effect <= 0x42) { /* the glow (gfxm_a.c GfxPost_DrawGlow) */
        extern uint32_t gGsMarkerArg;
        glow_marker(effect, gGsMarkerArg);
        return;
    }
    if (effect == 0x50) { /* the intro's cross-fade */
        extern uint32_t gGsMarkerArg;
        xf_marker(gGsMarkerArg);
        return;
    }
    if (effect >= 0x10 && effect <= 0x14) {
        sAnchor = effect == 0x13 ? 0 : effect == 0x14 ? 4 : effect - 0x0F;
    }
}

/* A picture uploaded straight into a display buffer (512 x 448 at page 0, or 448 lines further down): the films.
   gs_core.c calls this at the first such upload of a frame, at its place among the frame's draws (after the clear,
   before a fade) and BEFORE the pixels are in GS memory. A film's picture comes here straight from the decoder
   (plat_movie.c: the upload itself is not even carried out): it goes into the texture and is drawn. Anything else
   uploaded that way is read back from GS memory at the end of the frame and seen from the next frame on. */
static GLuint sMovieTex;
static int sMovieDirty; /* read back at the end of the frame: 1 the first buffer, 2 the second */
static void flush(void);
extern const uint8_t *Vita_MovieFrame(void);
extern int Vita_MoviePlaying(void);

static void movie_put(const void *rgba) {
    rt_sync();
    GLC_RESET(); /* 512 x 448, R G B A: new memory for the texture every time, so a
                                             picture the chip is still drawing from is never written into */
    glBindTexture(GL_TEXTURE_2D, sMovieTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 448, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

void GsGpu_FbUpload(int second) {
    ag_close(); /* the additive group before anything else */
    rt_sync();
    GLC_RESET();
    static const float kPos[8] = {-1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f};
    static const float kUv[8] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    const uint8_t *picture;

    if (gVitaGsSkip) {
        return;
    }
    flush_bins_c(12);
    flush();
    rt_sync();
    GLC_RESET();
    sHaveState = 0;
    if (sMovieTex == 0) {
        static uint32_t black[512 * 448];
        glGenTextures(1, &sMovieTex);
        movie_put(black);
        Vita_Log("vgl: frame %u: first picture uploaded into a display buffer\n", gGsFrame);
    }
    picture = Vita_MovieFrame();
    if (picture != NULL) {
        movie_put(picture);
        gVglFilm++;
    } else if (!Vita_MoviePlaying()) {
        sMovieDirty = second ? 2 : 1;
    }
    target_set(0);
    glUseProgram(0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, sMovieTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_DEPTH_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    scissor(VIEW_X, 0, VIEW_W, VIEW_H);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glVertexPointer(2, GL_FLOAT, 0, kPos);
    glTexCoordPointer(2, GL_FLOAT, 0, kUv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void movie_fill(void) {
    static uint32_t px[512 * 448];
    uint32_t bp = sMovieDirty == 2 ? 0x70u * 32u : 0u, x, y;

    for (y = 0; y < 448; y++) {
        for (x = 0; x < 512; x++) {
            px[y * 512 + x] = Gs_VramRead(bp, 8, 0, x, y) | 0xFF000000u;
        }
    }
    movie_put(px);
    sMovieDirty = 0;
}

/* Where the time goes (the log's figures): the display list with everything this back end does, and the movie
   decoder. build.py links the two entry points through these. */
#include <psp2/kernel/processmgr.h>
unsigned gVitaUsGs, gVitaUsMovie;
extern void __real_Port_GsVif1Chain(uint32_t tadr, int tte);
extern int __real_sceMpegGetPicture(void *mp, void *rgb, int blocks);
unsigned gVitaUnpackSkipped;
unsigned gVitaUnpack[2];  /* vectors unpacked into VU memory: by the short way, by the general loop */
unsigned gVitaList[2][2]; /* display lists of frames not drawn / drawn: how many, microseconds */
void __wrap_Port_GsVif1Chain(uint32_t tadr, int tte) {
    __real_Port_GsVif1Chain(tadr, tte);
}
/* src/sys/dma.c: the third display-list buffer (the game's heap is the console's size; this is the Vita's own memory) */
uint32_t Port_DmaSpare(uint32_t size) {
    static uint8_t buf[1u << 20] __attribute__((aligned(64)));
    return size <= sizeof(buf) ? (uint32_t)(uintptr_t)buf : 0;
}
/* libperf (vita_perf.c): what the picture thread counted per list, drawn or not, and the strip thread's work */
extern int gVitaPerfOn;
extern void Vita_PerfRead(uint32_t v[5]);
extern void Vita_PerfAccum(uint32_t last[5], uint64_t sum[5]);
extern void Vita_PerfThreadStart(void);
uint64_t gVitaPerfList[2][5], gVitaPerfWork[5];
unsigned gVitaPerfLists[2];
static uint32_t sPerfListLast[5];
uint64_t gVitaRegion[12][5]; /* per region: cycles, instructions, mispredicted branches, data cache misses, switches */
unsigned gVitaRegionLists;
static int sRegCur;
static uint32_t sRegLast[4];
static void reg_read(uint32_t v[4]) {
    scePerfArmPmonGetCounterValue(0, 31, &v[0]);
    scePerfArmPmonGetCounterValue(0, 2, &v[1]);
    scePerfArmPmonGetCounterValue(0, 3, &v[2]);
    scePerfArmPmonGetCounterValue(0, 0, &v[3]);
}
int gGrThreadId; /* gs_core.c: the back end's thread (with the list split, only it is measured) */
void Vita_ProfSwitch(int n) {
    uint32_t now[4];
    int k;
    if (gGrThreadId != 0 && sceKernelGetThreadId() != gGrThreadId) {
        return;
    }
    reg_read(now);
    for (k = 0; k < 4; k++) {
        gVitaRegion[sRegCur][k] += (uint32_t)(now[k] - sRegLast[k]);
    }
    gVitaRegion[sRegCur][4]++;
    reg_read(sRegLast); /* (the reading itself is left out as far as it can be) */
    sRegCur = n < 12 ? n : 11;
}
/* Zones: finer than the parts, for one measured list in 32 (no part switches then): Vita_Z(z) adds the cycles
   since the mark before to zone z. */
volatile int gVitaZoneOn;   /* 1: the drawing thread's list is measured now, 2: the list reader's */
uint64_t gVitaZone[48];
unsigned gVitaZoneN[48], gVitaZoneLists;
uint64_t gVitaZoneF[48];    /* the list reader's zones (its own marks) */
unsigned gVitaZoneNF[48], gVitaZoneListsF;
static uint32_t sZLast, sZLastF;
int gVitaZoneThread; /* (not used) */
int gFeThreadId;     /* gs_core.c: the list reader's thread */
void Vita_Z(int z) {
    uint32_t now;
    int tid = sceKernelGetThreadId();
    if (tid == gGrThreadId) {
        if (!(gVitaZoneOn & 1)) {
            return;
        }
        scePerfArmPmonGetCounterValue(0, 31, &now);
        gVitaZone[z] += now - sZLast;
        gVitaZoneN[z]++;
        sZLast = now;
    } else if (tid == gFeThreadId) {
        if (!(gVitaZoneOn & 2)) {
            return;
        }
        scePerfArmPmonGetCounterValue(0, 31, &now);
        gVitaZoneF[z] += now - sZLastF;
        gVitaZoneNF[z]++;
        sZLastF = now;
    }
}
/* the first drawn lists of a fight, each measured zone by zone on both threads: the slowest of them
   for the log (the first picture of a fight took 140 ms) */
#define WARM_LISTS 8
uint64_t gVitaWarmZ[2][48];      /* the slowest one's zones: the drawing thread's, the list reader's */
unsigned gVitaWarmUs[2], gVitaWarmN[2]; /* its time on each thread (us); lists measured */
static uint64_t sWarmSave[2][48], sWarmCur[2][48];
static unsigned sWarmSaveN[2][48];
static unsigned long long sWarmT0[2];
static int sWarmLeft[2], sWarmOn[2], sWarmWas[2];
static void warm_begin(int th) { /* th 0: the drawing thread (gVitaZone), 1: the list reader (gVitaZoneF) */
    extern int Vita_InBattle(void);
    int now = Vita_InBattle(), k;
    uint64_t *z = th == 0 ? gVitaZone : gVitaZoneF;
    unsigned *zn = th == 0 ? gVitaZoneN : gVitaZoneNF;
    if (now && !sWarmWas[th]) {
        sWarmLeft[th] = WARM_LISTS;
    }
    sWarmWas[th] = now;
    sWarmOn[th] = 0;
    if (sWarmLeft[th] <= 0 || !gVitaPerfOn) {
        return;
    }
    sWarmLeft[th]--;
    sWarmOn[th] = (gVitaZoneOn & (th == 0 ? 1 : 2)) ? 2 : 1; /* (2: the regular sampling measures this one too) */
    for (k = 0; k < 48; k++) {
        sWarmSave[th][k] = z[k];
        sWarmSaveN[th][k] = zn[k];
    }
    if (sWarmOn[th] == 1) {
        scePerfArmPmonGetCounterValue(0, 31, th == 0 ? &sZLast : &sZLastF);
        __sync_fetch_and_or(&gVitaZoneOn, th == 0 ? 1 : 2);
    }
    sWarmT0[th] = sceKernelGetProcessTimeWide();
}
static void warm_end(int th) {
    uint64_t *z = th == 0 ? gVitaZone : gVitaZoneF;
    unsigned *zn = th == 0 ? gVitaZoneN : gVitaZoneNF;
    unsigned us;
    int k;
    if (!sWarmOn[th]) {
        return;
    }
    Vita_Z(0);
    us = (unsigned)(sceKernelGetProcessTimeWide() - sWarmT0[th]);
    for (k = 0; k < 48; k++) {
        sWarmCur[th][k] = z[k] >= sWarmSave[th][k] ? z[k] - sWarmSave[th][k] : z[k]; /* (the log's reset in between) */
    }
    gVitaWarmN[th]++;
    if (us > gVitaWarmUs[th]) {
        gVitaWarmUs[th] = us;
        memcpy(gVitaWarmZ[th], sWarmCur[th], sizeof(sWarmCur[th]));
    }
    if (sWarmOn[th] == 1) { /* (not one of the regular samples: as if it had not been measured) */
        __sync_fetch_and_and(&gVitaZoneOn, th == 0 ? ~1 : ~2);
        for (k = 0; k < 48; k++) {
            z[k] = sWarmSave[th][k];
            zn[k] = sWarmSaveN[th][k];
        }
    }
    sWarmOn[th] = 0;
}
/* gs_core.c, the list reader, around each list it reads: one drawn list in 32 is measured zone by zone */
void Vita_FeListBegin(int drawn) {
    static unsigned n;
    if (drawn) {
        warm_begin(1);
    }
    if (!drawn || !gVitaPerfOn || ++n % 32 != 7 || sWarmOn[1]) {
        return;
    }
    {
        int k;
        scePerfArmPmonGetCounterValue(0, 31, &sZLastF);
        __sync_fetch_and_or(&gVitaZoneOn, 2);
        for (k = 0; k < 64; k++) {
            Vita_Z(31); /* (what a mark costs) */
        }
        Vita_Z(0);
    }
}
void Vita_FeListEnd(void) {
    warm_end(1);
    if (gVitaZoneOn & 2) {
        Vita_Z(0);
        __sync_fetch_and_and(&gVitaZoneOn, ~2);
        gVitaZoneListsF++;
    }
}
void Vita_PerfListBegin(void) {
    static int started;
    if (!gVitaPerfOn) {
        return;
    }
    if (!started) {
        started = 1;
        Vita_PerfThreadStart();
    }
    Vita_PerfRead(sPerfListLast);
    if (!gVitaGsSkip) {
        warm_begin(0);
    }
    {
        static unsigned drawn;
        if (!gVitaGsSkip && drawn % 32 == 15 && !sWarmOn[0]) {
            int k;
            drawn++;
            scePerfArmPmonGetCounterValue(0, 31, &sZLast);
            __sync_fetch_and_or(&gVitaZoneOn, 1);
            for (k = 0; k < 64; k++) {
                Vita_Z(31); /* (what a mark costs) */
            }
            Vita_Z(0);
        } else if (!gVitaGsSkip && ++drawn % 16 == 0) {
            int k;
            sRegCur = 11; /* (what measuring costs: 64 switches with nothing in between) */
            reg_read(sRegLast);
            for (k = 0; k < 64; k++) {
                Vita_ProfSwitch(11);
            }
            Vita_ProfSwitch(1);
            gVitaProfMeasure = 1;
        }
    }
}
void Vita_PerfListEnd(void) {
    warm_end(0);
    if (gVitaZoneOn & 1) {
        Vita_Z(0);
        __sync_fetch_and_and(&gVitaZoneOn, ~1);
        gVitaZoneLists++;
    }
    if (gVitaProfMeasure) {
        Vita_ProfSwitch(0);
        gVitaProfMeasure = 0;
        gVitaRegionLists++;
    }
    if (gVitaPerfOn) {
        Vita_PerfAccum(sPerfListLast, gVitaPerfList[!gVitaGsSkip]);
        gVitaPerfLists[!gVitaGsSkip]++;
    }
}
/* ---- The pictures shown in step with the game's clock ----
   The game's thread waits for the console's blanks (vita_main.c); a fight's frame takes two of them. Its list is
   read on one core and drawn on another while the game goes on, and the picture used to be put up at the first
   blank after the chip finished it: a list that took a little longer than most put its picture up a blank late
   (shown for three blanks, the one before it), and the next one on time again (shown for one). Now each picture
   is put up a fixed number of blanks (D) after the tick of its frame: one that is ready early waits in the display
   queue (three buffers: one shown, one waiting, one drawn into), and one that is a little late no longer moves the
   pictures around it. D is measured at the start of each fight (what nine pictures in ten of the first 60 needed,
   and one blank more), raised when a picture misses its blank by one or two (a longer miss is a stall, not the
   pace), and lowered by one after 60 pictures that all had three blanks to spare (not less than the
   pictures are put up at when it is set, and two to spare are kept).
   At most 5 (with 7 the pictures waited so long that the chip waited for a buffer to draw into, and
   every picture was ready 5 blanks after its tick instead of 3). */
volatile unsigned gVitaGridVb;          /* vita_main.c: the console's blank of the game's latest tick */
unsigned long long gVitaTickT[64];      /* vita_main.c: the time of each tick, by its blank */
volatile int gVitaGameLate;             /* vita_main.c: how late the game was for that tick (us) */
typedef struct PaceRec {
    unsigned seq, vb, target, cbVc, idleUs, idle0, L, D, cov, newTex, gpu, hold, texUs, fxm;
    unsigned long long tTick, tSub, tFe0, tFe1, tBe0, tSw0, tSw1, tBe1, tCb, tPrevRet, tPrevSet;
} PaceRec;
static PaceRec sPR[32];
static struct { void *addr; unsigned seq; int paced; volatile int valid; } sPb[4];
static unsigned sBeSeq, sPaceLast, sPaceN, sPaceLH[16], sPaceD, sPaceRun, sPaceRun3;
static unsigned long long sCbRet, sCbSet, sDoneLast;
static int sPaceOn = -1;
unsigned gPace[8];                      /* for the log: pictures paced, late, blanks late, lowered, waited (us), measured, not paced */
unsigned gPaceHist[2][10];              /* for the log: blanks from the tick to ready; blanks to spare */
unsigned long long gPaceGpu[7][3];      /* for the log: the chip's time per picture where it can be told, by the blended screens: sum, count, most */
unsigned long long gFxGpu[16][2];       /* the same by the effects left out (gVitaFxMask): sum, count */
static PaceRec sPaceMiss[6];
static unsigned sPaceMissN;
static PaceRec sPaceWorst; /* (for the log: the drawn list of the stretch the drawing thread took longest over) */

void Vita_PaceFe(unsigned seq, unsigned vb, unsigned long long tsub, int end) { /* gs_core.c, the list reader */
    PaceRec *r = &sPR[seq & 31];
    unsigned long long t = sceKernelGetProcessTimeWide();
    if (!end) {
        memset(r, 0, sizeof(*r));
        r->seq = seq;
        r->vb = vb;
        r->tSub = tsub;
        r->tTick = tsub != 0 ? gVitaTickT[vb & 63] : 0;
        r->tFe0 = t;
    } else if (r->seq == seq) {
        r->tFe1 = t;
    }
}
void Vita_PaceBe(unsigned seq, unsigned vb, int end) { /* gs_core.c, the drawing thread */
    extern unsigned gGrIdleUs;
    PaceRec *r = &sPR[seq & 31];
    (void)vb;
    if (r->seq != seq) {
        return;
    }
    if (!end) {
        r->tBe0 = sceKernelGetProcessTimeWide();
        r->idle0 = gGrIdleUs;
        sBeSeq = seq;
    } else {
        r->tBe1 = sceKernelGetProcessTimeWide();
        r->idleUs = gGrIdleUs >= r->idle0 ? gGrIdleUs - r->idle0 : gGrIdleUs;
        if (r->tSw0 != 0 && r->tBe1 - r->tBe0 > sPaceWorst.tBe1 - sPaceWorst.tBe0) {
            sPaceWorst = *r;
        }
    }
}
static void pace_swap(int phase) { /* the drawing thread, around the swap */
    extern void *gxm_color_surfaces_addr[];
    extern unsigned int gxm_back_buffer_index;
    extern int Vita_InBattle(void);
    PaceRec *r = &sPR[sBeSeq & 31];
    int mine = r->seq == sBeSeq;
    if (phase == 0) {
        static float covLast;
        static unsigned texLast;
        unsigned i = gxm_back_buffer_index & 3;
        float cov = gVglCov[1] >= covLast ? gVglCov[1] - covLast : gVglCov[1];
        unsigned nt = gVglNewTex >= texLast ? gVglNewTex - texLast : gVglNewTex;
        covLast = gVglCov[1];
        texLast = gVglNewTex;
        if (mine) {
            r->tSw0 = sceKernelGetProcessTimeWide();
            r->cov = (unsigned)(cov * (100.0f / (512.0f * 448.0f)));
            r->newTex = nt;
            r->hold = sPaceHold;
            r->texUs = sTexBudgetFrame == gGsFrame ? (unsigned)sTexBudgetUs : 0;
            r->fxm = (unsigned)gVitaFxMask;
        }
        sPb[i].valid = 0;
        __sync_synchronize();
        sPb[i].addr = gxm_color_surfaces_addr[gxm_back_buffer_index];
        sPb[i].seq = sBeSeq;
        sPb[i].paced = mine && r->tSub != 0 && Vita_InBattle() && !sPaceHold;
        __sync_synchronize();
        sPb[i].valid = 1;
    } else if (mine) {
        r->tSw1 = sceKernelGetProcessTimeWide();
    }
}
/* vitaGL's display queue callback (the chip has finished the picture; vitaGL puts it up at the next blank when
   this returns) */
static void pace_cb(void *fb) {
    extern int Vita_InBattle(void);
    gVitaShown++;
    unsigned long long t = sceKernelGetProcessTimeWide();
    extern unsigned Vita_Vcount(void);
    unsigned vc = Vita_Vcount(), T, seq = 0;
    int i, k = -1, paced = 0, missed = 0, prompt = t > sCbRet + 300; /* (prompt: not held back by the picture before: the chip finished it just now) */
    PaceRec *r = NULL;

    if (sPaceOn < 0) {
        sPaceOn = !(gVglSafe & 134217728);
    }
    for (i = 0; i < 4; i++) {
        if (sPb[i].valid && sPb[i].addr == fb) {
            k = i;
            break;
        }
    }
    if (k >= 0) {
        seq = sPb[k].seq;
        paced = sPb[k].paced;
        sPb[k].valid = 0;
        r = &sPR[seq & 31];
        if (r->seq != seq) {
            r = NULL;
            paced = 0;
        } else {
            r->tCb = t;
            r->cbVc = vc;
            r->tPrevRet = sCbRet;
            r->tPrevSet = sCbSet;
            if (prompt && r->tSw0 != 0 && t > r->tSw0) { /* the chip's time for it: from the end of its scene or the end of the one before */
                unsigned long long st = sDoneLast > r->tSw0 ? sDoneLast : r->tSw0, g = t - st;
                unsigned bin = r->cov < 2000 ? 0 : r->cov < 2500 ? 1 : r->cov < 3000 ? 2 : r->cov < 3500 ? 3 : r->cov < 4000 ? 4 : r->cov < 4500 ? 5 : 6;
                r->gpu = (unsigned)g;
                if (gVitaFxSweep && !r->hold) {
                    unsigned m = r->fxm & 15;
                    gFxGpu[m][0] += g;
                    gFxGpu[m][1]++;
                }
                if (!r->hold) {
                    gPaceGpu[bin][0] += g;
                    gPaceGpu[bin][1]++;
                    if (g > gPaceGpu[bin][2]) {
                        gPaceGpu[bin][2] = g;
                    }
                }
            }
        }
    }
    sDoneLast = prompt ? t : 0;
    if (!Vita_InBattle()) {
        sPaceN = 0; /* (measured again in the next fight) */
        sPaceD = 0;
        sPaceRun = sPaceRun3 = 0;
        memset(sPaceLH, 0, sizeof(sPaceLH));
    }
    T = vc + 1;
    if (!sPaceOn) {
        paced = 0;
    } else if (!paced) {
        gPace[6]++;
        if (Vita_InBattle() && (int)(sPaceLast + 2 - T) > 0) {
            T = sPaceLast + 2;
        }
    } else {
        unsigned L = vc + 1 - r->vb;
        gPace[0]++;
        gPaceHist[0][L > 9 ? 9 : L]++;
        r->L = L;
        if (sPaceN < 60 || sPaceD == 0) { /* measuring: shown as before (at least two blanks each) */
            gPace[5]++;
            if ((int)(sPaceLast + 2 - T) > 0) {
                T = sPaceLast + 2;
            }
            if (L <= 12) { /* (a stall is not the pace) */
                sPaceLH[L > 15 ? 15 : L]++;
                sPaceN++;
            }
            if (sPaceN == 60) {
                unsigned j, c = 0, p = 2;
                for (j = 0; j < 16; j++) {
                    c += sPaceLH[j];
                    if (c >= 54) {
                        p = j;
                        break;
                    }
                }
                p++; /* (a blank more than nine in ten needed: the busy stretches come later) */
                {   /* (and not less than the pictures are put up at now: a lower D would show the next one a blank
                       early, for one blank (a test at 18 s); a higher one is lowered below if it is not needed) */
                    unsigned now = T - r->vb;
                    p = now > p ? now : p;
                }
                sPaceD = p < 2 ? 2 : p > 5 ? 5 : p;
                sPaceRun = sPaceRun3 = 0;
            }
        } else {
            T = r->vb + sPaceD;
            if ((int)(T - (sPaceLast + 1)) < 0) {
                T = sPaceLast + 1; /* (never two pictures for one blank) */
            }
            if ((int)(vc + 1 - T) > 0) { /* late: up at the next blank, and D raised for the next ones */
                unsigned x = vc + 1 - T;
                gPace[1]++;
                gPace[2] += x;
                r->D = sPaceD;
                if (x <= 2) {
                    sPaceD = sPaceD + x > 5 ? 5 : sPaceD + x; /* (more and the chip would wait for a buffer to draw into) */
                }
                missed = 1;
                T = vc + 1;
                sPaceRun = sPaceRun3 = 0;
            } else {
                unsigned spare = T - (vc + 1);
                gPaceHist[1][spare > 9 ? 9 : spare]++;
                sPaceRun3 = spare >= 3 ? sPaceRun3 + 1 : 0;
                sPaceRun = spare >= 2 ? sPaceRun + 1 : 0;
                if (sPaceRun3 >= 60 && sPaceD > 2) { /* (three to spare for 2 s: one blank less, a picture shown for one blank; two to spare are kept) */
                    sPaceD--;
                    sPaceRun = sPaceRun3 = 0;
                    gPace[3]++;
                }
            }
        }
    }
    if (sPaceOn && (int)(vc + 1 - T) < 0) { /* put up for blank T: after the one before it has begun */
        unsigned long long w0 = sceKernelGetProcessTimeWide();
        int guard = 0;
        while ((int)(Vita_Vcount() - (T - 1)) < 0) {
            if (++guard > 8) { /* (a blank out of reach is not waited for: the next one, and the pace measured anew) */
                T = Vita_Vcount() + 1;
                sPaceN = 0;
                sPaceD = 0;
                break;
            }
            sceDisplayWaitVblankStart();
        }
        gPace[4] += (unsigned)(sceKernelGetProcessTimeWide() - w0);
    }
    sPaceLast = T;
    if (r != NULL) {
        r->target = T;
        if (r->D == 0) {
            r->D = sPaceD;
        }
        if (missed && sPaceMissN < 6) {
            sPaceMiss[sPaceMissN++] = *r;
        }
    }
    sCbRet = sCbSet = sceKernelGetProcessTimeWide();
}
void Vita_PaceLog(void) { /* vita_main.c, with the log's other lines */
    static const char *const kBin[7] = {"under 20", "20-25", "25-30", "30-35", "35-40", "40-45", "45+"};
    unsigned j, k;
    char line[600];
    int len;
    if (gPace[0] + gPace[6] == 0) {
        return;
    }
    Vita_Log("vita:   pictures in step with the game's clock: %u blanks after the tick now; %u paced (%u of them measuring), %u late by %u blanks in all, %u lowered, %u not paced; waited %u ms\n",
             sPaceD, gPace[0], gPace[5], gPace[1], gPace[2], gPace[3], gPace[6], gPace[4] / 1000);
    for (k = 0; k < 2; k++) {
        len = snprintf(line, sizeof(line), k == 0 ? "vita:   blanks from the tick to the picture ready:" : "vita:   blanks to spare:");
        for (j = 0; j < 10; j++) {
            len += snprintf(line + len, sizeof(line) - (size_t)len, " %u%s", gPaceHist[k][j], j == 9 ? "+" : "");
        }
        Vita_Log("%s\n", line);
    }
    len = snprintf(line, sizeof(line), "vita:   the chip's time per picture (where it can be told), by the GS primitives' blended screens:");
    for (j = 0; j < 7; j++) {
        if (gPaceGpu[j][1] != 0) {
            len += snprintf(line + len, sizeof(line) - (size_t)len, " %s: %.1f ms, at most %.1f (%u);", kBin[j], (double)gPaceGpu[j][0] / gPaceGpu[j][1] / 1000.0,
                            (double)gPaceGpu[j][2] / 1000.0, (unsigned)gPaceGpu[j][1]);
        }
    }
    Vita_Log("%s\n", line);
    for (j = 0; j < sPaceMissN; j++) {
        const PaceRec *r = &sPaceMiss[j];
        unsigned long long b = r->tTick;
#define PMS(x) ((x) != 0 && b != 0 ? (double)((long long)((x) - b)) / 1000.0 : -1.0)
        Vita_Log("vita:   late picture, list %u: ready %u blanks after its tick, wanted at %u (up at %d) | game gave the list %.1f ms after the tick | reader %.1f..%.1f | "
                 "drawer %.1f..%.1f (waited for the reader %u us) | swap %.1f..%.1f | chip done %.1f (the picture before put up %.1f); the chip's time %.1f ms, "
                 "%u.%02u blended screens, %u new textures\n",
                 r->seq, r->L, r->D, (int)(r->target - r->vb), PMS(r->tSub), PMS(r->tFe0), PMS(r->tFe1), PMS(r->tBe0), PMS(r->tBe1), r->idleUs, PMS(r->tSw0), PMS(r->tSw1),
                 PMS(r->tCb), PMS(r->tPrevSet), r->gpu / 1000.0, r->cov / 100, r->cov % 100, r->newTex);
#undef PMS
    }
    if (sPaceWorst.seq != 0) {
        const PaceRec *r = &sPaceWorst;
        Vita_Log("vita:   longest drawn list, %u: the drawing thread %.1f ms on it (waited for the reader %.1f ms, new textures %.1f ms, %u of them; swap %.1f ms), the reader %.1f ms; "
                 "%u.%02u blended screens\n", r->seq, (double)(r->tBe1 - r->tBe0) / 1000.0, r->idleUs / 1000.0, r->texUs / 1000.0, r->newTex,
                 r->tSw1 > r->tSw0 ? (double)(r->tSw1 - r->tSw0) / 1000.0 : 0.0, r->tFe1 > r->tFe0 ? (double)(r->tFe1 - r->tFe0) / 1000.0 : 0.0, r->cov / 100, r->cov % 100);
        memset(&sPaceWorst, 0, sizeof(sPaceWorst));
    }
    sPaceMissN = 0;
    memset(gPace, 0, sizeof(gPace));
    memset(gPaceHist, 0, sizeof(gPaceHist));
    memset(gPaceGpu, 0, sizeof(gPaceGpu));
}
void Vita_ListDone(unsigned us) { /* gs_core.c, after every list (on the thread that ran it) */
    int drawn = !gVitaGsSkip;
    gVitaUsGs += us;
    gVitaList[drawn][0]++;
    gVitaList[drawn][1] += us;
}
int __wrap_sceMpegGetPicture(void *mp, void *rgb, int blocks) {
    unsigned long long t = sceKernelGetProcessTimeWide();
    int r = __real_sceMpegGetPicture(mp, rgb, blocks);
    gVitaUsMovie += (unsigned)(sceKernelGetProcessTimeWide() - t);
    return r;
}
unsigned gGpuNewTex, gGpuNewTexPixels, gGpuNewPipes;
unsigned long long gGpuTexNs, gGpuPipeNs, gGpuEndNs;

extern void __real_GsVu1_Call(int addr);
__attribute__((hot)) void __wrap_GsVu1_Call(int addr) {
    P_IN(5);
    ZM(1);
    __real_GsVu1_Call(addr);
    ZM(12);
    gVglCalls[5]++;
    P_OUT;
}
