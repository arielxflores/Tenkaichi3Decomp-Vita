/*
 * Fast stand-ins for the busiest routines of the vector library (src/port/vu0_a.c, vu0_b.c), Vita build.
 *
 * Why: the library mirrors the PS2's vector unit instruction by instruction, every float operation through a checked
 * primitive (port/vita/vita_vu0_inline.h) and, in the first half, through a model of the unit's registers; Mtx_Mul
 * alone is 12 KB of code. The fight of the demo's pairing 0 calls the library 20,000 to 27,000 times a frame,
 * on the game's thread, which needs all of its 33 ms.
 *
 * What: the same operations in the same order and association, as plain float code. The game's thread runs with
 * the float unit set to round toward zero, flush to zero, default NaN (Vita_FloatMode), which is what the checked
 * primitives rely on as well: for finite operands that do not overflow, their result IS the float unit's result.
 * What the primitives add is only for operands or results with an exponent of 255 (infinity / NaN on the Vita,
 * ordinary numbers on the PS2) and for overflow (clamped to +/-max on the PS2). So each stand-in computes into
 * locals, and when an output is not finite (which is the case whenever an operand had an exponent of 255 or a step
 * overflowed: neither a sum nor a product can make an infinity or a NaN finite again) it stores nothing and calls
 * the library's own routine. The square roots: the library's is the exact root truncated (an integer model of the
 * unit's), which is the float unit's square root rounded toward zero. Division: as the primitive, a divisor with
 * an exponent of 0 gives +/-max.
 * The routines of the first half also leave their registers (gRefVu0: vf4..vf11, ACC, the current matrix vf16..19)
 * as the library does, since a few routines of the second half store a register component they never set.
 *
 * Checked at the first call on the game's thread (in round-toward-zero mode): each stand-in against the library's
 * routine on 600 sets of random operands (outputs and registers bit for bit); a stand-in that differs once is not
 * used (the log says which), and the time per call of both is measured for the log. gVglSafe 16384: none used.
 *
 * Linked with --wrap through the call counting of port/vita/vita_vucount.c (its trampolines go to VuFast_<name>
 * for these); compiled like the game's code (port/vita/build.py: kind 'ee').
 */
#include <stdint.h>
#include <string.h>
#include "port/vu0_a.h"

extern unsigned long long sceKernelGetProcessTimeWide(void);
extern int snprintf(char *s, unsigned n, const char *fmt, ...);
extern void Vita_Log(const char *fmt, ...);
extern int gVglSafe;

typedef struct { float x, y, z, w; } V4;
typedef struct { V4 r[4]; } M4;

#define VF(n) (*(V4 *)&gRefVu0.vf[n])
#define ACC (*(V4 *)&gRefVu0.acc)

/* the library's routines (--wrap: the real ones) */
extern void __real_Vec4_Add(V4 *o, const V4 *a, const V4 *b), __real_Vec4_Sub(V4 *o, const V4 *a, const V4 *b);
extern void __real_Vec3_Add(V4 *o, const V4 *a, const V4 *b), __real_Vec3_Sub(V4 *o, const V4 *a, const V4 *b);
extern void __real_Vec4_Mul(V4 *o, const V4 *a, const V4 *b), __real_Vec3_Cross(V4 *o, const V4 *a, const V4 *b);
extern void __real_Vec4_Scale(V4 *o, const V4 *v, float s), __real_Vec3_Scale(V4 *o, const V4 *v, float s);
extern void __real_Mtx_MulVec4(V4 *o, const M4 *m, const V4 *v), __real_Mtx_MulVec3(V4 *o, const M4 *m, const V4 *v);
extern float __real_Vec3_LengthSq(const V4 *v), __real_Vec3_Length(const V4 *v), __real_Vec3_Dot(const V4 *a, const V4 *b);
extern float __real_Vec3_DistSq(const V4 *a, const V4 *b), __real_Vec3_Dist(const V4 *a, const V4 *b);
extern void __real_Vec3_Normalize(V4 *o, const V4 *v);
extern void __real_Mtx_Mul(M4 *d, M4 *a, M4 *b), __real_Vu0Cur_MulMtx(M4 *m), __real_Vu0Cur_MulMtxRev(M4 *m);
extern void __real_Vu0Cur_MulVec4(V4 *o, V4 *v), __real_Vu0Cur_MulVec3(V4 *o, V4 *v);

enum {
    F_Vec4_Add, F_Vec4_Sub, F_Vec3_Add, F_Vec3_Sub, F_Vec4_Mul, F_Vec3_Cross, F_Vec4_Scale, F_Vec3_Scale, F_Mtx_MulVec4,
    F_Mtx_MulVec3, F_Vec3_LengthSq, F_Vec3_Length, F_Vec3_Dot, F_Vec3_DistSq, F_Vec3_Dist, F_Vec3_Normalize, F_Mtx_Mul,
    F_Vu0Cur_MulMtx, F_Vu0Cur_MulMtxRev, F_Vu0Cur_MulVec4, F_Vu0Cur_MulVec3, F_COUNT
};
static const char *const kName[F_COUNT] = {
    "Vec4_Add", "Vec4_Sub", "Vec3_Add", "Vec3_Sub", "Vec4_Mul", "Vec3_Cross", "Vec4_Scale", "Vec3_Scale", "Mtx_MulVec4",
    "Mtx_MulVec3", "Vec3_LengthSq", "Vec3_Length", "Vec3_Dot", "Vec3_DistSq", "Vec3_Dist", "Vec3_Normalize", "Mtx_Mul",
    "Vu0Cur_MulMtx", "Vu0Cur_MulMtxRev", "Vu0Cur_MulVec4", "Vu0Cur_MulVec3"};
static unsigned char sOn[F_COUNT]; /* 1: the stand-in is used */
static int sTested;
unsigned gVuFastFall; /* (for the log) calls handed to the library's routine for a result that was not finite */
static int vuf_test(void);
#define GATE(k) (__builtin_expect(!sOn[k], 0) && !(sTested ? 0 : vuf_test(), sOn[k]))

/* (x * 0 is 0 for a finite x, NaN for an infinity or a NaN; the sum is NaN when any of them was) */
static inline int bad4(float a, float b, float c, float d) {
    float t = (a * 0.0f + b * 0.0f) + (c * 0.0f + d * 0.0f);
    return t != t;
}
static inline int bad1(float a) {
    float t = a * 0.0f;
    return t != t;
}
static inline float sqrt_rz(float a) { return __builtin_sqrtf(__builtin_fabsf(a)); }
/* vdiv: a divisor with an exponent of 0 gives +/-max (the sign of the exclusive-or), a dividend with one +/-0 */
static inline float div_vu(float a, float b) {
    union { float f; uint32_t u; } x, y, r;
    x.f = a;
    y.f = b;
    if ((y.u & 0x7F800000u) == 0) {
        r.u = ((x.u ^ y.u) & 0x80000000u) | 0x7F7FFFFFu;
        return r.f;
    }
    if ((x.u & 0x7F800000u) == 0) {
        r.u = (x.u ^ y.u) & 0x80000000u;
        return r.f;
    }
    return a / b;
}
/* ((r0 * v.x + r1 * v.y) + r2 * v.z) + r3 * v.w per component; acc: the sum before the last term */
static inline V4 apply(const V4 *r, V4 v, V4 *acc) {
    V4 a, o;
    a.x = r[0].x * v.x; a.y = r[0].y * v.x; a.z = r[0].z * v.x; a.w = r[0].w * v.x;
    a.x = a.x + r[1].x * v.y; a.y = a.y + r[1].y * v.y; a.z = a.z + r[1].z * v.y; a.w = a.w + r[1].w * v.y;
    a.x = a.x + r[2].x * v.z; a.y = a.y + r[2].y * v.z; a.z = a.z + r[2].z * v.z; a.w = a.w + r[2].w * v.z;
    o.x = a.x + r[3].x * v.w; o.y = a.y + r[3].y * v.w; o.z = a.z + r[3].z * v.w; o.w = a.w + r[3].w * v.w;
    if (acc != 0) {
        *acc = a;
    }
    return o;
}
static inline float dot3(V4 a, V4 b) {
    float tx = a.x * b.x, ty = a.y * b.y, tz = a.z * b.z;
    return (tx + ty) + VF(3).x * tz;
}

/* ---- each stand-in: computes, and stores only when every output is finite (returns 0 otherwise) ---- */
static int fx_Vec4_Add(V4 *o, const V4 *a, const V4 *b) {
    V4 va = *a, vb = *b;
    va.x = va.x + vb.x; va.y = va.y + vb.y; va.z = va.z + vb.z; va.w = va.w + vb.w;
    if (bad4(va.x, va.y, va.z, va.w)) { return 0; }
    *o = va;
    return 1;
}
static int fx_Vec4_Sub(V4 *o, const V4 *a, const V4 *b) {
    V4 va = *a, vb = *b;
    va.x = va.x - vb.x; va.y = va.y - vb.y; va.z = va.z - vb.z; va.w = va.w - vb.w;
    if (bad4(va.x, va.y, va.z, va.w)) { return 0; }
    *o = va;
    return 1;
}
static int fx_Vec3_Add(V4 *o, const V4 *a, const V4 *b) { /* out.w = a.w */
    V4 va = *a, vb = *b;
    va.x = va.x + vb.x; va.y = va.y + vb.y; va.z = va.z + vb.z;
    if (bad4(va.x, va.y, va.z, 0.0f)) { return 0; }
    *o = va;
    return 1;
}
static int fx_Vec3_Sub(V4 *o, const V4 *a, const V4 *b) {
    V4 va = *a, vb = *b;
    va.x = va.x - vb.x; va.y = va.y - vb.y; va.z = va.z - vb.z;
    if (bad4(va.x, va.y, va.z, 0.0f)) { return 0; }
    *o = va;
    return 1;
}
static int fx_Vec4_Mul(V4 *o, const V4 *a, const V4 *b) {
    V4 va = *a, vb = *b;
    va.x = va.x * vb.x; va.y = va.y * vb.y; va.z = va.z * vb.z; va.w = va.w * vb.w;
    if (bad4(va.x, va.y, va.z, va.w)) { return 0; }
    *o = va;
    return 1;
}
static int fx_Vec3_Cross(V4 *o, const V4 *a, const V4 *b) { /* out.w = b.w - b.w */
    V4 va = *a, vb = *b, r;
    r.x = va.y * vb.z - vb.y * va.z;
    r.y = va.z * vb.x - vb.z * va.x;
    r.z = va.x * vb.y - vb.x * va.y;
    r.w = vb.w - vb.w;
    if (bad4(r.x, r.y, r.z, r.w)) { return 0; }
    *o = r;
    return 1;
}
static int fx_Vec4_Scale(V4 *o, const V4 *v, float s) {
    V4 va = *v;
    va.x = va.x * s; va.y = va.y * s; va.z = va.z * s; va.w = va.w * s;
    if (bad4(va.x, va.y, va.z, va.w)) { return 0; }
    *o = va;
    return 1;
}
static int fx_Vec3_Scale(V4 *o, const V4 *v, float s) {
    V4 va = *v;
    va.x = va.x * s; va.y = va.y * s; va.z = va.z * s;
    if (bad4(va.x, va.y, va.z, 0.0f)) { return 0; }
    *o = va;
    return 1;
}
static int fx_Mtx_MulVec4(V4 *o, const M4 *m, const V4 *v) {
    V4 rows[4], p;
    V4 in = *v;
    rows[0] = m->r[0]; rows[1] = m->r[1]; rows[2] = m->r[2]; rows[3] = m->r[3];
    p = apply(rows, in, 0);
    if (bad4(p.x, p.y, p.z, p.w)) { return 0; }
    *o = p;
    return 1;
}
static int fx_Mtx_MulVec3(V4 *o, const M4 *m, const V4 *v) {
    V4 rows[4], p;
    V4 in = *v;
    rows[0] = m->r[0]; rows[1] = m->r[1]; rows[2] = m->r[2]; rows[3] = m->r[3];
    p = apply(rows, in, 0);
    if (bad4(p.x, p.y, p.z, p.w)) { return 0; } /* (w too: the library computes it, with the same clamping) */
    p.w = in.w;
    *o = p;
    return 1;
}
static int fx_Vec3_LengthSq(float *r, const V4 *v) {
    V4 in = *v;
    float d = dot3(in, in);
    if (bad1(d)) { return 0; }
    *r = d;
    return 1;
}
static int fx_Vec3_Length(float *r, const V4 *v) {
    V4 in = *v;
    float d = dot3(in, in);
    if (bad1(d)) { return 0; }
    *r = sqrt_rz(d);
    return 1;
}
static int fx_Vec3_Dot(float *r, const V4 *a, const V4 *b) {
    float d = dot3(*a, *b);
    if (bad1(d)) { return 0; }
    *r = d;
    return 1;
}
static int fx_Vec3_DistSq(float *r, const V4 *a, const V4 *b) {
    V4 d;
    float s;
    d.x = a->x - b->x; d.y = a->y - b->y; d.z = a->z - b->z; d.w = a->w - b->w;
    s = dot3(d, d);
    if (bad4(s, d.w, 0.0f, 0.0f)) { return 0; }
    *r = s;
    return 1;
}
static int fx_Vec3_Dist(float *r, const V4 *a, const V4 *b) {
    V4 d;
    float s;
    d.x = a->x - b->x; d.y = a->y - b->y; d.z = a->z - b->z; d.w = a->w - b->w;
    s = dot3(d, d);
    if (bad4(s, d.w, 0.0f, 0.0f)) { return 0; }
    *r = sqrt_rz(s);
    return 1;
}
static int fx_Vec3_Normalize(V4 *o, const V4 *v) {
    V4 in = *v, r;
    float d = dot3(in, in), len, q;
    if (bad1(d)) { return 0; }
    len = VF(0).x + sqrt_rz(d);
    q = div_vu(VF(0).w, len);
    r.w = VF(0).w - VF(0).w;
    r.x = in.x * q;
    r.y = in.y * q;
    r.z = in.z * q;
    if (bad4(r.x, r.y, r.z, q)) { return 0; }
    *o = r;
    return 1;
}
/* the first half's: registers as the library leaves them */
static int fx_Mtx_Mul(M4 *dst, M4 *a, M4 *b) {
    V4 A[4], B[4], R[4], acc;
    int i;
    for (i = 0; i < 4; i++) {
        A[i] = a->r[i];
        B[i] = b->r[i];
    }
    for (i = 0; i < 4; i++) {
        R[i] = apply(A, B[i], &acc);
    }
    if (bad4(R[0].x, R[0].y, R[0].z, R[0].w) | bad4(R[1].x, R[1].y, R[1].z, R[1].w) | bad4(R[2].x, R[2].y, R[2].z, R[2].w) |
        bad4(R[3].x, R[3].y, R[3].z, R[3].w)) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        VF(4 + i) = A[i];
        VF(8 + i) = R[i];
        dst->r[i] = R[i];
    }
    ACC = acc;
    return 1;
}
static int fx_Vu0Cur_MulMtx(M4 *m) { /* vf4..7 = m, vf8..11 = the current matrix before, vf16..19 = the product */
    V4 Mr[4], C[4], R[4], acc;
    int i;
    for (i = 0; i < 4; i++) {
        Mr[i] = m->r[i];
        C[i] = VF(16 + i);
    }
    for (i = 0; i < 4; i++) {
        R[i] = apply(C, Mr[i], &acc);
    }
    if (bad4(R[0].x, R[0].y, R[0].z, R[0].w) | bad4(R[1].x, R[1].y, R[1].z, R[1].w) | bad4(R[2].x, R[2].y, R[2].z, R[2].w) |
        bad4(R[3].x, R[3].y, R[3].z, R[3].w)) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        VF(4 + i) = Mr[i];
        VF(8 + i) = C[i];
        VF(16 + i) = R[i];
    }
    ACC = acc;
    return 1;
}
static int fx_Vu0Cur_MulMtxRev(M4 *m) { /* vf4..7 = m, vf16..19 = m * each row of the current matrix */
    V4 Mr[4], R[4], acc;
    int i;
    for (i = 0; i < 4; i++) {
        Mr[i] = m->r[i];
    }
    for (i = 0; i < 4; i++) {
        R[i] = apply(Mr, VF(16 + i), &acc);
    }
    if (bad4(R[0].x, R[0].y, R[0].z, R[0].w) | bad4(R[1].x, R[1].y, R[1].z, R[1].w) | bad4(R[2].x, R[2].y, R[2].z, R[2].w) |
        bad4(R[3].x, R[3].y, R[3].z, R[3].w)) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        VF(4 + i) = Mr[i];
        VF(16 + i) = R[i];
    }
    ACC = acc;
    return 1;
}
static int fx_Vu0Cur_MulVec4(V4 *o, V4 *v) { /* vf4 = the product */
    V4 in = *v, p, acc;
    p = apply(&VF(16), in, &acc);
    if (bad4(p.x, p.y, p.z, p.w)) { return 0; }
    VF(4) = p;
    ACC = acc;
    *o = p;
    return 1;
}
static int fx_Vu0Cur_MulVec3(V4 *o, V4 *v) { /* vf4 = the product's x, y, z and v.w */
    V4 in = *v, p, acc;
    p = apply(&VF(16), in, &acc);
    if (bad4(p.x, p.y, p.z, p.w)) { return 0; }
    p.w = in.w;
    VF(4) = p;
    ACC = acc;
    *o = p;
    return 1;
}

/* ---- the polygon clipper and the projections the stage's surfaces use (the sea of Kame House: ~1000
   triangles a frame, 12 ms of the game's thread in the library's routines). The library's (src/port/vu0_b.c) as plain
   float code: the same operations, order and association; registers as the library leaves them (ClipPoly_ClipPlane:
   vf6.x the last distance; ClipPoly_ProjectCur: vf13.xyz the last vertex's S, T, Q; their .w stale as there). All
   inputs are read and every output is worked out before anything is stored, so a call that meets a number the
   float unit cannot take as the PS2 does goes to the library's routine untouched. ---- */
typedef struct { V4 pos, uv, col; } CV;
extern int __real_ClipPoly_ClipPlane(CV *poly, const V4 *plane, int n);
extern void __real_ClipPoly_ProjectCur(int32_t (*scr)[4], V4 *stq, const CV *poly, int n);
extern void __real_Vu0Cur_ProjectInt(int32_t *out, const V4 *v);
static inline uint32_t fbits(float f) {
    union { float f; uint32_t u; } x;
    x.f = f;
    return x.u;
}
/* vftoi0 / vftoi4 of a finite number: scaled exactly, truncated toward zero, saturated. In single
   precision: times 1 or 16 is exact (an overflow gives the largest float in this mode, which saturates the same way),
   and the processor's conversion truncates toward zero and saturates (was: by way of a double, twice as slow). */
static inline int32_t toi_vu(float f, float scale) {
    float d = f * scale, r;
    int32_t i;
    __asm__("vcvt.s32.f32 %0, %1" : "=t"(r) : "t"(d));
    memcpy(&i, &r, 4);
    return i;
}
/* ClipPlane_EdgeParam then ClipVtx_Lerp(o, next, cur, t): o = next * t + cur * (1 - t) */
static inline int edge_lerp(CV *o, const V4 *pl, const CV *cur, const CV *next, float dist) {
    float dx = cur->pos.x - next->pos.x, dy = cur->pos.y - next->pos.y, dz = cur->pos.z - next->pos.z;
    float tx = pl->x * dx, ty = pl->y * dy, tz = pl->z * dz;
    float dot = (tx + ty) + VF(3).x * tz;
    float t = div_vu(dist, dot), u = VF(0).w - t;
    const V4 *a, *b;
    V4 *r;
    int k, bad = bad4(dot, t, u, 0.0f);
    for (k = 0; k < 3; k++) {
        a = k == 0 ? &next->pos : k == 1 ? &next->uv : &next->col;
        b = k == 0 ? &cur->pos : k == 1 ? &cur->uv : &cur->col;
        r = k == 0 ? &o->pos : k == 1 ? &o->uv : &o->col;
        r->x = a->x * t + b->x * u;
        r->y = a->y * t + b->y * u;
        r->z = a->z * t + b->z * u;
        r->w = a->w * t + b->w * u;
        bad |= bad4(r->x, r->y, r->z, r->w);
    }
    return !bad;
}
static int fx_ClipPoly_ClipPlane(int *ret, CV *poly, const V4 *plane, int n) {
    CV buf[16];
    float d[16];
    int out[16], i, j, k, nOut = 0;
    V4 pl;
    if (n == 0) {
        *ret = 0;
        return 1;
    }
    if (n < 0 || n > 9) {
        return 0;
    }
    pl = *plane;
    for (i = 0; i < n; i++) { /* ClipPlane_DistArray: ((n.x p.x + n.y p.y) + 1 * n.z p.z) + 1 * n.w */
        float tx = pl.x * poly[i].pos.x, ty = pl.y * poly[i].pos.y, tz = pl.z * poly[i].pos.z;
        float acc = tx + ty;
        uint32_t b;
        acc = acc + VF(3).x * tz;
        d[i] = acc + VF(3).x * pl.w;
        if (bad1(d[i])) {
            return 0;
        }
        b = fbits(d[i]);
        out[i] = (b >> 31) != 0 && (b & 0x7FFFFFFFu) != 0; /* (c.lt.s against 0 on the bits: -0 is not below) */
        nOut += out[i];
    }
    if (nOut == 0 || nOut == n) {
        VF(6).x = d[n - 1];
        *ret = nOut == 0 ? n : 0;
        return 1;
    }
    k = 0;
    for (i = 0, j = 1; i < n; i++, j++) {
        const CV *cur, *next;
        if (j >= n) {
            j = 0;
        }
        cur = &poly[i];
        next = &poly[j];
        if (!out[i]) {
            buf[k++] = *cur;
            if (out[j] && !edge_lerp(&buf[k++], &pl, cur, next, d[i])) {
                return 0;
            }
        } else if (!out[j]) {
            if (!edge_lerp(&buf[k++], &pl, cur, next, d[i])) {
                return 0;
            }
        }
    }
    for (i = 0; i < k; i++) {
        poly[i] = buf[i];
    }
    VF(6).x = d[n - 1];
    *ret = k;
    return 1;
}
static int fx_ClipPoly_ProjectCur(int32_t (*scr)[4], V4 *stq, const CV *poly, int n) {
    V4 R[4], S[16];
    int32_t I[16][4];
    float sw = VF(13).w;
    int i;
    if (n <= 0 || n > 16) {
        return 0;
    }
    R[0] = VF(16); R[1] = VF(17); R[2] = VF(18); R[3] = VF(19);
    for (i = 0; i < n; i++) {
        V4 p = apply(R, poly[i].pos, 0);
        float q;
        if (bad4(p.x, p.y, p.z, p.w)) {
            return 0;
        }
        q = div_vu(VF(0).w, p.w);
        p.x = p.x * q; p.y = p.y * q; p.z = p.z * q;
        S[i].x = poly[i].uv.x * q; S[i].y = poly[i].uv.y * q; S[i].z = VF(1).z * q; S[i].w = sw;
        if (bad4(p.x, p.y, p.z, q) | bad4(S[i].x, S[i].y, S[i].z, 0.0f)) {
            return 0;
        }
        I[i][0] = toi_vu(p.x, 16.0f); I[i][1] = toi_vu(p.y, 16.0f); I[i][2] = toi_vu(p.z, 1.0f); I[i][3] = toi_vu(p.w, 1.0f);
    }
    for (i = 0; i < n; i++) {
        stq[i] = S[i];
        scr[i][0] = I[i][0]; scr[i][1] = I[i][1]; scr[i][2] = I[i][2]; scr[i][3] = I[i][3];
    }
    VF(13).x = S[n - 1].x; VF(13).y = S[n - 1].y; VF(13).z = S[n - 1].z;
    return 1;
}
static int fx_Vu0Cur_ProjectInt(int32_t *o, const V4 *v) {
    V4 R[4], p;
    float q;
    R[0] = VF(16); R[1] = VF(17); R[2] = VF(18); R[3] = VF(19);
    p = apply(R, *v, 0);
    if (bad4(p.x, p.y, p.z, p.w)) {
        return 0;
    }
    q = div_vu(VF(0).w, p.w);
    p.x = p.x * q; p.y = p.y * q; p.z = p.z * q;
    if (bad4(p.x, p.y, p.z, q)) {
        return 0;
    }
    o[0] = toi_vu(p.x, 1.0f); o[1] = toi_vu(p.y, 1.0f); o[2] = toi_vu(p.z, 1.0f); o[3] = toi_vu(p.w, 1.0f);
    return 1;
}
enum { G_ClipPlane, G_ProjectCur, G_ProjectInt, G_COUNT };
static const char *const kName2[G_COUNT] = {"ClipPoly_ClipPlane", "ClipPoly_ProjectCur", "Vu0Cur_ProjectInt"};
static unsigned char sOn2[G_COUNT];
#define GATE2(k) (__builtin_expect(!sOn2[k], 0) && !(sTested ? 0 : vuf_test(), sOn2[k]))

/* ---- what the game calls (through vita_vucount.c's trampolines) ---- */
#define FALL(call) do { gVuFastFall++; call; } while (0)
int VuFast_ClipPoly_ClipPlane(CV *poly, const V4 *plane, int n) {
    int r;
    if (GATE2(G_ClipPlane) || !fx_ClipPoly_ClipPlane(&r, poly, plane, n)) {
        FALL(r = __real_ClipPoly_ClipPlane(poly, plane, n));
    }
    return r;
}
void VuFast_ClipPoly_ProjectCur(int32_t (*scr)[4], V4 *stq, const CV *poly, int n) {
    if (GATE2(G_ProjectCur) || !fx_ClipPoly_ProjectCur(scr, stq, poly, n)) {
        FALL(__real_ClipPoly_ProjectCur(scr, stq, poly, n));
    }
}
void VuFast_Vu0Cur_ProjectInt(int32_t *o, const V4 *v) {
    if (GATE2(G_ProjectInt) || !fx_Vu0Cur_ProjectInt(o, v)) {
        FALL(__real_Vu0Cur_ProjectInt(o, v));
    }
}
/* ---- the rotations: Mtx_RotateX/Y/Z, Vu0Cur_RotateX/Y/Z and the sine and cosine they begin with. The
   library's are 14 KB of code each (every vector instruction of the unit modelled in line), 250 calls a fight frame:
   the game's thread spent ~110,000 instruction-cache misses a frame, most of them streaming those. The same
   operations in the same order, as plain float code; the registers left as the library leaves them (vu0_a.c
   Ref_Vu0_SinCos: vf4 = (u2, the sign extension of u, $t0's upper words, u), vf5 the coefficients, vf6 = (t9, t7, t5,
   t3), vf7.x / .w, vf12, Q, $t0; the rotation's two axes in vf8 / vf9, ACC the last row's sum before its w term). ---- */
extern void __real_Mtx_RotateX(M4 *d, M4 *s, float a), __real_Mtx_RotateY(M4 *d, M4 *s, float a), __real_Mtx_RotateZ(M4 *d, M4 *s, float a);
extern void __real_Vu0Cur_RotateX(float a), __real_Vu0Cur_RotateY(float a), __real_Vu0Cur_RotateZ(float a);
typedef struct { V4 v4, v5, v6, v7, v12; float q; uint32_t t0lo, t0hi; } SinCosR;
static inline float uf(uint32_t u) { union { uint32_t u; float f; } x; x.u = u; return x.f; }
static int fx_sincos(float angle, SinCosR *o) {
    static const uint32_t coef[4] = {0x362E9C14u, 0xB94FB21Fu, 0x3C08873Eu, 0xBE2AAAA4u};
    const V4 v0 = VF(0);
    V4 r6, r12 = VF(12), r7 = VF(7), r5;
    float u, u2, c, q;
    uint32_t ub;
    int neg;
    if (bad1(angle) | bad4(v0.x, v0.y, v0.z, v0.w) | bad4(r12.y, r12.z, r12.w, 0.0f)) {
        return 0;
    }
    neg = angle < 0.0f;
    u = neg ? uf(0x3FC90FDBu) + angle : uf(0x3FC90FDBu) - angle;
    ub = fbits(u);
    r5.x = uf(coef[0]); r5.y = uf(coef[1]); r5.z = uf(coef[2]); r5.w = uf(coef[3]);
    r12.x = v0.x + u;                                       /* vaddx.x vf12, vf0, vf4x */
    u2 = u * u;                                             /* vmul.x vf4, vf4, vf4 */
    r12.y = r12.y * v0.x; r12.z = r12.z * v0.x; r12.w = r12.w * v0.x; /* vmulx.yzw vf12, vf12, vf0x */
    r6.x = r5.x * u; r6.y = r5.y * u; r6.z = r5.z * u; r6.w = r5.w * u;     /* vmulw.xyzw vf6, vf5, vf4w */
    r6.x = r6.x * u2; r6.y = r6.y * u2; r6.z = r6.z * u2; r6.w = r6.w * u2; /* vmulx.xyzw vf6, vf6, vf4x */
    r6.x = r6.x * u2; r6.y = r6.y * u2; r6.z = r6.z * u2;  /* vmulx.xyz */
    r12.x = r12.x + r6.w;                                   /* vaddw.x */
    r6.x = r6.x * u2; r6.y = r6.y * u2;                     /* vmulx.xy */
    r12.x = r12.x + r6.z;                                   /* vaddz.x */
    r6.x = r6.x * u2;                                       /* vmulx.x */
    r12.x = r12.x + r6.y;                                   /* vaddy.x */
    r12.x = r12.x + r6.x;                                   /* vaddx.x */
    c = r12.x;
    r12.x = v0.x + c; r12.y = v0.y + c;                     /* vaddx.xy vf12, vf0, vf12x */
    r7.x = r12.x * r12.x;                                   /* vmul.x vf7, vf12, vf12 */
    r7.w = v0.w - r7.x;                                     /* vsubx.w vf7, vf0, vf7x */
    q = sqrt_rz(r7.w);                                      /* vsqrt Q, vf7w */
    r7.x = v0.x + q;                                        /* vaddq.x vf7, vf0, Q */
    r12.x = neg ? v0.x - r7.x : v0.x + r7.x;
    if (bad4(r12.x, r12.y, r12.z, r12.w) | bad4(r6.x, r6.y, r6.z, r6.w) | bad4(r7.x, r7.w, u2, q)) {
        return 0;
    }
    o->v4.x = u2;
    { union { uint32_t u[4]; V4 v; } t; t.v = o->v4; t.u[1] = (ub & 0x80000000u) ? 0xFFFFFFFFu : 0; t.u[2] = gRefVu0.gpr[0].u[2]; t.u[3] = ub; o->v4 = t.v; }
    o->v5 = r5; o->v6 = r6; o->v7 = r7; o->v12 = r12; o->q = q;
    o->t0lo = ub; o->t0hi = (ub & 0x80000000u) ? 0xFFFFFFFFu : 0;
    return 1;
}
static inline void sincos_commit(const SinCosR *o, int regs) {
    union { float f; uint32_t u; } q;
    q.f = o->q;
    gRefVu0.gpr[0].u[0] = o->t0lo; /* ($t0) */
    gRefVu0.gpr[0].u[1] = o->t0hi;
    gRefVu0.q = q.u;
    VF(12) = o->v12;
    if (regs) { /* (the current matrix's rotations: these stay; a matrix's are overwritten by its rows) */
        VF(4) = o->v4;
        VF(5) = o->v5;
        VF(6) = o->v6;
        VF(7).x = o->v7.x;
        VF(7).w = o->v7.w;
    }
}
/* axis 0 X, 1 Y, 2 Z: the two axes (vf8, vf9) and the rotation's four rows of factors */
static inline void rot_axes(int axis, const V4 *r12, V4 *A, V4 *B, V4 R[4]) {
    const V4 v0 = VF(0);
    float s = r12->x, c = r12->y;
    if (axis == 2) {
        A->x = v0.x + c; A->y = v0.y + s; A->z = v0.z - v0.z; A->w = v0.w - v0.w;
        B->x = v0.x - s; B->y = v0.y + c; B->z = v0.z - v0.z; B->w = v0.w - v0.w;
        R[0] = *A; R[1] = *B; R[2] = VF(1); R[3] = v0;
    } else if (axis == 0) {
        A->x = v0.x - v0.x; A->y = v0.y + c; A->z = v0.z + s; A->w = v0.w - v0.w;
        B->x = v0.x - v0.x; B->y = v0.y - s; B->z = v0.z + c; B->w = v0.w - v0.w;
        R[0] = VF(3); R[1] = *A; R[2] = *B; R[3] = v0;
    } else {
        A->x = v0.x + c; A->y = v0.y - v0.y; A->z = v0.z - s; A->w = v0.w - v0.w;
        B->x = v0.x + s; B->y = v0.y - v0.y; B->z = v0.z + c; B->w = v0.w - v0.w;
        R[0] = *A; R[1] = VF(2); R[2] = *B; R[3] = v0;
    }
}
static int fx_Mtx_Rotate(int axis, M4 *d, const M4 *src, float angle) {
    SinCosR sc;
    V4 A, B, R[4], in[4], out[4], acc;
    int i;
    if (!fx_sincos(angle, &sc)) {
        return 0;
    }
    in[0] = src->r[0]; in[1] = src->r[1]; in[2] = src->r[2]; in[3] = src->r[3];
    rot_axes(axis, &sc.v12, &A, &B, R);
    if (bad4(VF(1).x, VF(1).y, VF(1).z, VF(1).w) | bad4(VF(2).x, VF(2).y, VF(2).z, VF(2).w) | bad4(VF(3).x, VF(3).y, VF(3).z, VF(3).w)) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        out[i] = apply(R, in[i], &acc);
        if (bad4(out[i].x, out[i].y, out[i].z, out[i].w) | bad4(acc.x, acc.y, acc.z, acc.w)) {
            return 0;
        }
    }
    sincos_commit(&sc, 0);
    VF(4) = out[0]; VF(5) = out[1]; VF(6) = out[2]; VF(7) = out[3];
    VF(8) = A; VF(9) = B;
    ACC = acc;
    d->r[0] = out[0]; d->r[1] = out[1]; d->r[2] = out[2]; d->r[3] = out[3];
    return 1;
}
static int fx_Vu0Cur_Rotate(int axis, float angle) {
    SinCosR sc;
    V4 A, B, R[4], out[4], acc;
    int i;
    if (!fx_sincos(angle, &sc)) {
        return 0;
    }
    rot_axes(axis, &sc.v12, &A, &B, R);
    if (bad4(VF(1).x, VF(1).y, VF(1).z, VF(1).w) | bad4(VF(2).x, VF(2).y, VF(2).z, VF(2).w) | bad4(VF(3).x, VF(3).y, VF(3).z, VF(3).w)) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        out[i] = apply(R, VF(16 + i), &acc);
        if (bad4(out[i].x, out[i].y, out[i].z, out[i].w) | bad4(acc.x, acc.y, acc.z, acc.w)) {
            return 0;
        }
    }
    sincos_commit(&sc, 1);
    VF(8) = A; VF(9) = B;
    VF(16) = out[0]; VF(17) = out[1]; VF(18) = out[2]; VF(19) = out[3];
    ACC = acc;
    return 1;
}
enum { R_MtxX, R_MtxY, R_MtxZ, R_CurX, R_CurY, R_CurZ, R_COUNT };
static const char *const kName3[R_COUNT] = {"Mtx_RotateX", "Mtx_RotateY", "Mtx_RotateZ", "Vu0Cur_RotateX", "Vu0Cur_RotateY", "Vu0Cur_RotateZ"};
static unsigned char sOn3[R_COUNT];
#define GATE3(k) (__builtin_expect(!sOn3[k], 0) && !(sTested ? 0 : vuf_test(), sOn3[k]))
void VuFast_Mtx_RotateX(M4 *d, M4 *s, float a) { if (GATE3(R_MtxX) || !fx_Mtx_Rotate(0, d, s, a)) { FALL(__real_Mtx_RotateX(d, s, a)); } }
void VuFast_Mtx_RotateY(M4 *d, M4 *s, float a) { if (GATE3(R_MtxY) || !fx_Mtx_Rotate(1, d, s, a)) { FALL(__real_Mtx_RotateY(d, s, a)); } }
void VuFast_Mtx_RotateZ(M4 *d, M4 *s, float a) { if (GATE3(R_MtxZ) || !fx_Mtx_Rotate(2, d, s, a)) { FALL(__real_Mtx_RotateZ(d, s, a)); } }
void VuFast_Vu0Cur_RotateX(float a) { if (GATE3(R_CurX) || !fx_Vu0Cur_Rotate(0, a)) { FALL(__real_Vu0Cur_RotateX(a)); } }
void VuFast_Vu0Cur_RotateY(float a) { if (GATE3(R_CurY) || !fx_Vu0Cur_Rotate(1, a)) { FALL(__real_Vu0Cur_RotateY(a)); } }
void VuFast_Vu0Cur_RotateZ(float a) { if (GATE3(R_CurZ) || !fx_Vu0Cur_Rotate(2, a)) { FALL(__real_Vu0Cur_RotateZ(a)); } }
/* the library's compositions of them (vu0_a.c: its own calls stay inside it), by way of the stand-ins */
extern void __real_Mtx_RotateZXY(M4 *d, M4 *s, V4 *a), __real_Mtx_RotateXYZ(M4 *d, M4 *s, V4 *a), __real_Vu0Cur_RotateZXY(V4 *a), __real_Vu0Cur_RotateXYZ(V4 *a);
void VuFast_Mtx_RotateZXY(M4 *d, M4 *s, V4 *a) { VuFast_Mtx_RotateZ(d, s, a->z); VuFast_Mtx_RotateX(d, d, a->x); VuFast_Mtx_RotateY(d, d, a->y); }
void VuFast_Mtx_RotateXYZ(M4 *d, M4 *s, V4 *a) { VuFast_Mtx_RotateX(d, s, a->x); VuFast_Mtx_RotateY(d, d, a->y); VuFast_Mtx_RotateZ(d, d, a->z); }
void VuFast_Vu0Cur_RotateZXY(V4 *a) { VuFast_Vu0Cur_RotateZ(a->z); VuFast_Vu0Cur_RotateX(a->x); VuFast_Vu0Cur_RotateY(a->y); }
void VuFast_Vu0Cur_RotateXYZ(V4 *a) { VuFast_Vu0Cur_RotateX(a->x); VuFast_Vu0Cur_RotateY(a->y); VuFast_Vu0Cur_RotateZ(a->z); }
void VuFast_Vec4_Add(V4 *o, const V4 *a, const V4 *b) { if (GATE(F_Vec4_Add) || !fx_Vec4_Add(o, a, b)) { FALL(__real_Vec4_Add(o, a, b)); } }
void VuFast_Vec4_Sub(V4 *o, const V4 *a, const V4 *b) { if (GATE(F_Vec4_Sub) || !fx_Vec4_Sub(o, a, b)) { FALL(__real_Vec4_Sub(o, a, b)); } }
void VuFast_Vec3_Add(V4 *o, const V4 *a, const V4 *b) { if (GATE(F_Vec3_Add) || !fx_Vec3_Add(o, a, b)) { FALL(__real_Vec3_Add(o, a, b)); } }
void VuFast_Vec3_Sub(V4 *o, const V4 *a, const V4 *b) { if (GATE(F_Vec3_Sub) || !fx_Vec3_Sub(o, a, b)) { FALL(__real_Vec3_Sub(o, a, b)); } }
void VuFast_Vec4_Mul(V4 *o, const V4 *a, const V4 *b) { if (GATE(F_Vec4_Mul) || !fx_Vec4_Mul(o, a, b)) { FALL(__real_Vec4_Mul(o, a, b)); } }
void VuFast_Vec3_Cross(V4 *o, const V4 *a, const V4 *b) { if (GATE(F_Vec3_Cross) || !fx_Vec3_Cross(o, a, b)) { FALL(__real_Vec3_Cross(o, a, b)); } }
void VuFast_Vec4_Scale(V4 *o, const V4 *v, float s) { if (GATE(F_Vec4_Scale) || !fx_Vec4_Scale(o, v, s)) { FALL(__real_Vec4_Scale(o, v, s)); } }
void VuFast_Vec3_Scale(V4 *o, const V4 *v, float s) { if (GATE(F_Vec3_Scale) || !fx_Vec3_Scale(o, v, s)) { FALL(__real_Vec3_Scale(o, v, s)); } }
void VuFast_Mtx_MulVec4(V4 *o, const M4 *m, const V4 *v) { if (GATE(F_Mtx_MulVec4) || !fx_Mtx_MulVec4(o, m, v)) { FALL(__real_Mtx_MulVec4(o, m, v)); } }
void VuFast_Mtx_MulVec3(V4 *o, const M4 *m, const V4 *v) { if (GATE(F_Mtx_MulVec3) || !fx_Mtx_MulVec3(o, m, v)) { FALL(__real_Mtx_MulVec3(o, m, v)); } }
float VuFast_Vec3_LengthSq(const V4 *v) { float r; if (GATE(F_Vec3_LengthSq) || !fx_Vec3_LengthSq(&r, v)) { FALL(r = __real_Vec3_LengthSq(v)); } return r; }
float VuFast_Vec3_Length(const V4 *v) { float r; if (GATE(F_Vec3_Length) || !fx_Vec3_Length(&r, v)) { FALL(r = __real_Vec3_Length(v)); } return r; }
float VuFast_Vec3_Dot(const V4 *a, const V4 *b) { float r; if (GATE(F_Vec3_Dot) || !fx_Vec3_Dot(&r, a, b)) { FALL(r = __real_Vec3_Dot(a, b)); } return r; }
float VuFast_Vec3_DistSq(const V4 *a, const V4 *b) { float r; if (GATE(F_Vec3_DistSq) || !fx_Vec3_DistSq(&r, a, b)) { FALL(r = __real_Vec3_DistSq(a, b)); } return r; }
float VuFast_Vec3_Dist(const V4 *a, const V4 *b) { float r; if (GATE(F_Vec3_Dist) || !fx_Vec3_Dist(&r, a, b)) { FALL(r = __real_Vec3_Dist(a, b)); } return r; }
void VuFast_Vec3_Normalize(V4 *o, const V4 *v) { if (GATE(F_Vec3_Normalize) || !fx_Vec3_Normalize(o, v)) { FALL(__real_Vec3_Normalize(o, v)); } }
void VuFast_Mtx_Mul(M4 *d, M4 *a, M4 *b) { if (GATE(F_Mtx_Mul) || !fx_Mtx_Mul(d, a, b)) { FALL(__real_Mtx_Mul(d, a, b)); } }
void VuFast_Vu0Cur_MulMtx(M4 *m) { if (GATE(F_Vu0Cur_MulMtx) || !fx_Vu0Cur_MulMtx(m)) { FALL(__real_Vu0Cur_MulMtx(m)); } }
void VuFast_Vu0Cur_MulMtxRev(M4 *m) { if (GATE(F_Vu0Cur_MulMtxRev) || !fx_Vu0Cur_MulMtxRev(m)) { FALL(__real_Vu0Cur_MulMtxRev(m)); } }
void VuFast_Vu0Cur_MulVec4(V4 *o, V4 *v) { if (GATE(F_Vu0Cur_MulVec4) || !fx_Vu0Cur_MulVec4(o, v)) { FALL(__real_Vu0Cur_MulVec4(o, v)); } }
void VuFast_Vu0Cur_MulVec3(V4 *o, V4 *v) { if (GATE(F_Vu0Cur_MulVec3) || !fx_Vu0Cur_MulVec3(o, v)) { FALL(__real_Vu0Cur_MulVec3(o, v)); } }

/* ---- the check at the first call ---- */
static uint32_t sSeed = 0x2545F491u;
static uint32_t rnd(void) {
    sSeed ^= sSeed << 13;
    sSeed ^= sSeed >> 17;
    sSeed ^= sSeed << 5;
    return sSeed;
}
/* operands as a game has them: mostly ordinary magnitudes, some zeros and near-equal pairs, now and then a huge or
   tiny one (and, rarely, an exponent of 255, which must go to the library) */
static float rfloat(void) {
    union { float f; uint32_t u; } x;
    uint32_t k = rnd() & 255;
    if (k < 12) {
        x.u = ((k & 1) ? 0x80000000u : 0) | ((k & 2) ? (rnd() & 0x7FFFFFu) : 0); /* (zeros, and numbers too small for the unit) */
    } else if (k < 14) {
        x.u = (rnd() & 0x807FFFFFu) | 0x7F800000u;
    } else if (k < 20) {
        x.u = (rnd() & 0x807FFFFFu) | ((uint32_t)(1 + (rnd() % 40)) << 23);
    } else if (k < 26) {
        x.u = (rnd() & 0x807FFFFFu) | ((uint32_t)(200 + (rnd() % 54)) << 23);
    } else {
        x.u = (rnd() & 0x807FFFFFu) | ((uint32_t)(112 + (rnd() % 30)) << 23);
    }
    return x.f;
}
static void rvec(V4 *v) { v->x = rfloat(); v->y = rfloat(); v->z = rfloat(); v->w = (rnd() & 3) == 0 ? 1.0f : rfloat(); }
static void rmtx(M4 *m) {
    int i;
    for (i = 0; i < 4; i++) {
        rvec(&m->r[i]);
    }
}
/* the registers a stand-in may change, and the outputs */
typedef struct { RefVec4 vf[16]; RefVec4 acc; uint8_t out[64]; } Snap;
static void snap(Snap *s, const void *out, unsigned n) {
    memcpy(s->vf, &gRefVu0.vf[4], sizeof(s->vf));
    memcpy(&s->acc, &gRefVu0.acc, sizeof(s->acc));
    memset(s->out, 0, sizeof(s->out));
    memcpy(s->out, out, n);
}
static int same(const Snap *a, const Snap *b) { return memcmp(a, b, sizeof(*a)) == 0; }

/* runs function k on the operands, the library's way (lib != 0) or the stand-in's (falling back as the game's
   calls do); out is what it writes */
typedef struct { M4 m, n; V4 a, b; float s; } Args;
static void run(int k, int lib, Args *g, void *out) {
    V4 *o = (V4 *)out;
    float *f = (float *)out;
    switch (k) {
    case F_Vec4_Add: if (lib || !fx_Vec4_Add(o, &g->a, &g->b)) { __real_Vec4_Add(o, &g->a, &g->b); } break;
    case F_Vec4_Sub: if (lib || !fx_Vec4_Sub(o, &g->a, &g->b)) { __real_Vec4_Sub(o, &g->a, &g->b); } break;
    case F_Vec3_Add: if (lib || !fx_Vec3_Add(o, &g->a, &g->b)) { __real_Vec3_Add(o, &g->a, &g->b); } break;
    case F_Vec3_Sub: if (lib || !fx_Vec3_Sub(o, &g->a, &g->b)) { __real_Vec3_Sub(o, &g->a, &g->b); } break;
    case F_Vec4_Mul: if (lib || !fx_Vec4_Mul(o, &g->a, &g->b)) { __real_Vec4_Mul(o, &g->a, &g->b); } break;
    case F_Vec3_Cross: if (lib || !fx_Vec3_Cross(o, &g->a, &g->b)) { __real_Vec3_Cross(o, &g->a, &g->b); } break;
    case F_Vec4_Scale: if (lib || !fx_Vec4_Scale(o, &g->a, g->s)) { __real_Vec4_Scale(o, &g->a, g->s); } break;
    case F_Vec3_Scale: if (lib || !fx_Vec3_Scale(o, &g->a, g->s)) { __real_Vec3_Scale(o, &g->a, g->s); } break;
    case F_Mtx_MulVec4: if (lib || !fx_Mtx_MulVec4(o, &g->m, &g->a)) { __real_Mtx_MulVec4(o, &g->m, &g->a); } break;
    case F_Mtx_MulVec3: if (lib || !fx_Mtx_MulVec3(o, &g->m, &g->a)) { __real_Mtx_MulVec3(o, &g->m, &g->a); } break;
    case F_Vec3_LengthSq: if (lib || !fx_Vec3_LengthSq(f, &g->a)) { *f = __real_Vec3_LengthSq(&g->a); } break;
    case F_Vec3_Length: if (lib || !fx_Vec3_Length(f, &g->a)) { *f = __real_Vec3_Length(&g->a); } break;
    case F_Vec3_Dot: if (lib || !fx_Vec3_Dot(f, &g->a, &g->b)) { *f = __real_Vec3_Dot(&g->a, &g->b); } break;
    case F_Vec3_DistSq: if (lib || !fx_Vec3_DistSq(f, &g->a, &g->b)) { *f = __real_Vec3_DistSq(&g->a, &g->b); } break;
    case F_Vec3_Dist: if (lib || !fx_Vec3_Dist(f, &g->a, &g->b)) { *f = __real_Vec3_Dist(&g->a, &g->b); } break;
    case F_Vec3_Normalize: if (lib || !fx_Vec3_Normalize(o, &g->a)) { __real_Vec3_Normalize(o, &g->a); } break;
    case F_Mtx_Mul: if (lib || !fx_Mtx_Mul((M4 *)out, &g->m, &g->n)) { __real_Mtx_Mul((M4 *)out, &g->m, &g->n); } break;
    case F_Vu0Cur_MulMtx: if (lib || !fx_Vu0Cur_MulMtx(&g->m)) { __real_Vu0Cur_MulMtx(&g->m); } break;
    case F_Vu0Cur_MulMtxRev: if (lib || !fx_Vu0Cur_MulMtxRev(&g->m)) { __real_Vu0Cur_MulMtxRev(&g->m); } break;
    case F_Vu0Cur_MulVec4: if (lib || !fx_Vu0Cur_MulVec4(o, &g->a)) { __real_Vu0Cur_MulVec4(o, &g->a); } break;
    case F_Vu0Cur_MulVec3: if (lib || !fx_Vu0Cur_MulVec3(o, &g->a)) { __real_Vu0Cur_MulVec3(o, &g->a); } break;
    }
}
static unsigned out_size(int k) {
    return k == F_Mtx_Mul ? 64 : (k >= F_Vec3_LengthSq && k <= F_Vec3_Dist) ? 4 : (k == F_Vu0Cur_MulMtx || k == F_Vu0Cur_MulMtxRev) ? 0 : 16;
}

/* The released program's check is short (a tenth of the operand sets, no timing): the full one took about a
   second of the game's thread when the title screen comes up (the opening film's end, or its skip, froze that
   long). bt3.txt vufquick=0 / 1 chooses; a development build does the full one by default. */
#ifdef PORT_VITA_RELEASE
#define VUF_QUICK 1
#else
#define VUF_QUICK 0
#endif
extern int Port_Setting(const char *name, int def);

static int vuf_test(void) {
    uint32_t fpscr;
    RefVec4 keep[32], keepAcc;
    char line[900];
    int len, k, i, bad = 0, quick, div;
    unsigned long long tq = sceKernelGetProcessTimeWide();
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    if (((fpscr >> 22) & 3) != 3) {
        return 0; /* not yet in the game's mode (round toward zero): the library's routines meanwhile */
    }
    sTested = 1;
    quick = Port_Setting("vufquick", VUF_QUICK) != 0;
    div = quick ? 10 : 1;
    if (gVglSafe & 16384) {
        Vita_Log("vita: the vector library's fast stand-ins are off (gVglSafe 16384)\n");
        return 0;
    }
    memcpy(keep, gRefVu0.vf, sizeof(keep));
    memcpy(&keepAcc, &gRefVu0.acc, sizeof(keepAcc));
    len = snprintf(line, sizeof(line), "vita: the vector library's fast stand-ins, ns per call (the library's / the stand-in's):");
    for (k = 0; k < F_COUNT; k++) {
        int diff = 0;
        unsigned long long t0, t1, t2;
        Args g;
        uint8_t outA[64], outB[64];
        /* (the current matrix: random, the same for both) */
        for (i = 0; i < 600 / div && !diff; i++) {
            Snap sa, sb;
            M4 cur;
            Args h;
            rmtx(&g.m); rmtx(&g.n); rvec(&g.a); rvec(&g.b); g.s = rfloat();
            if ((i & 7) == 1) { g.b = g.a; g.b.x = g.a.x * 1.0000001f; } /* (near-equal operands) */
            rmtx(&cur);
            memcpy(&gRefVu0.vf[4], keep + 4, 12 * sizeof(RefVec4)); /* (scratch registers: as found) */
            memcpy(&gRefVu0.vf[16], &cur, sizeof(cur));
            memcpy(outA, &g, 64); memcpy(outB, &g, 64); /* (outputs start with the same bytes) */
            h = g;
            run(k, 1, &g, outA);
            snap(&sa, outA, out_size(k));
            memcpy(&gRefVu0.vf[4], keep + 4, 12 * sizeof(RefVec4));
            memcpy(&gRefVu0.vf[16], &cur, sizeof(cur));
            run(k, 0, &h, outB);
            snap(&sb, outB, out_size(k));
            if (!same(&sa, &sb)) {
                diff = i + 1;
            }
        }
        if (!diff) {
            sOn[k] = 1;
        } else {
            bad++;
        }
        /* the time per call: the same ordinary operands 2000 times each way */
        do {
            rmtx(&g.m); rmtx(&g.n); rvec(&g.a); rvec(&g.b); g.s = rfloat();
            g.a.x = 1.5f; g.a.y = -2.25f; g.a.z = 0.75f; g.a.w = 1.0f; g.b = g.a; g.b.y = 3.0f; g.s = 0.5f;
            for (i = 0; i < 16; i++) { ((float *)&g.m)[i] = (float)(i - 7) * 0.375f; ((float *)&g.n)[i] = (float)(5 - i) * 0.125f; }
        } while (0);
        memcpy(&gRefVu0.vf[16], &g.n, sizeof(M4));
        t0 = t1 = t2 = 0;
        if (!quick) {
            t0 = sceKernelGetProcessTimeWide();
            for (i = 0; i < 2000; i++) { run(k, 1, &g, outA); if (k == F_Vu0Cur_MulMtx || k == F_Vu0Cur_MulMtxRev) { memcpy(&gRefVu0.vf[16], &g.n, sizeof(M4)); } }
            t1 = sceKernelGetProcessTimeWide();
            for (i = 0; i < 2000; i++) { run(k, 0, &g, outB); if (k == F_Vu0Cur_MulMtx || k == F_Vu0Cur_MulMtxRev) { memcpy(&gRefVu0.vf[16], &g.n, sizeof(M4)); } }
            t2 = sceKernelGetProcessTimeWide();
        }
        if (len > 0 && len < (int)sizeof(line) - 60) {
            len += snprintf(line + len, sizeof(line) - (unsigned)len, " %s %u/%u%s;", kName[k], (unsigned)((t1 - t0) / 2), (unsigned)((t2 - t1) / 2),
                            diff ? " DIFFERS, not used" : "");
        }
        if (diff) {
            Vita_Log("vita: the vector library's %s: the stand-in differs from the library's routine at operand set %d; not used\n", kName[k], diff);
        }
    }
    Vita_Log("%s\n", line);
    {   /* the clipper and the projections: polygons of 3..6 corners, the outputs and the registers compared */
        len = snprintf(line, sizeof(line), "vita:   the clipper's stand-ins, ns per call (the library's / the stand-in's):");
        for (k = 0; k < G_COUNT; k++) {
            int diff = 0, nTests = 3000 / div, kind;
            unsigned long long t0, t1, t2;
            unsigned cut = 0, fell = 0;
            for (i = 0; i < nTests && !diff; i++) {
                CV pa[16], pb[16];
                V4 pl, sa[16], sb[16];
                int32_t ia[16][4], ib[16][4];
                int n = 3 + (int)(rnd() % 4), j, ra = 0, rb = 0;
                Snap xa, xb;
                M4 cur;
                V4 r13;
                kind = (int)(rnd() & 3); /* (0: anything; else ordinary numbers, a plane through the polygon's middle) */
                for (j = 0; j < 16; j++) {
                    rvec(&pa[j].pos); rvec(&pa[j].uv); rvec(&pa[j].col);
                    if (kind != 0) {
                        pa[j].pos.x = (float)((int)(rnd() % 4001) - 2000) * 0.37f;
                        pa[j].pos.y = (float)((int)(rnd() % 4001) - 2000) * 0.29f;
                        pa[j].pos.z = (float)((int)(rnd() % 4001) - 2000) * 0.53f;
                        pa[j].pos.w = 1.0f;
                        pa[j].uv.x = (float)(rnd() % 1000) * 0.001f; pa[j].uv.y = (float)(rnd() % 1000) * 0.001f;
                        pa[j].uv.z = 1.0f; pa[j].uv.w = 1.0f;
                        pa[j].col.x = (float)(rnd() % 256); pa[j].col.y = (float)(rnd() % 256); pa[j].col.z = (float)(rnd() % 256); pa[j].col.w = (float)(rnd() % 129);
                    }
                }
                rvec(&pl);
                if (kind != 0) {
                    pl.x = (float)((int)(rnd() % 2001) - 1000) * 0.001f; pl.y = (float)((int)(rnd() % 2001) - 1000) * 0.001f;
                    pl.z = (float)((int)(rnd() % 2001) - 1000) * 0.001f;
                    pl.w = -(pl.x * pa[0].pos.x + pl.y * pa[0].pos.y + pl.z * pa[0].pos.z) * (float)(rnd() % 3) * 0.5f;
                }
                rmtx(&cur);
                if (kind != 0) { /* (a projection with w well away from 0 for most corners) */
                    int a, b2;
                    for (a = 0; a < 4; a++) {
                        for (b2 = 0; b2 < 4; b2++) {
                            ((float *)&cur)[a * 4 + b2] = (float)((int)(rnd() % 2001) - 1000) * 0.002f;
                        }
                    }
                    cur.r[3].w = 500.0f + (float)(rnd() % 5000);
                }
                rvec(&r13);
                memcpy(pb, pa, sizeof(pa));
                memset(ia, 0x5A, sizeof(ia)); memset(ib, 0x5A, sizeof(ib)); memset(sa, 0x5A, sizeof(sa)); memset(sb, 0x5A, sizeof(sb));
                memcpy(&gRefVu0.vf[4], keep + 4, 12 * sizeof(RefVec4));
                memcpy(&gRefVu0.vf[16], &cur, sizeof(cur));
                memcpy(&gRefVu0.vf[13], &r13, sizeof(r13));
                if (k == G_ClipPlane) { ra = __real_ClipPoly_ClipPlane(pa, &pl, n); }
                else if (k == G_ProjectCur) { __real_ClipPoly_ProjectCur(ia, sa, pa, n); }
                else { __real_Vu0Cur_ProjectInt(ia[0], &pa[0].pos); }
                snap(&xa, &ra, 4);
                memcpy(&gRefVu0.vf[4], keep + 4, 12 * sizeof(RefVec4));
                memcpy(&gRefVu0.vf[16], &cur, sizeof(cur));
                memcpy(&gRefVu0.vf[13], &r13, sizeof(r13));
                if (k == G_ClipPlane) { if (!fx_ClipPoly_ClipPlane(&rb, pb, &pl, n)) { fell++; rb = __real_ClipPoly_ClipPlane(pb, &pl, n); } }
                else if (k == G_ProjectCur) { if (!fx_ClipPoly_ProjectCur(ib, sb, pb, n)) { fell++; __real_ClipPoly_ProjectCur(ib, sb, pb, n); } }
                else { if (!fx_Vu0Cur_ProjectInt(ib[0], &pb[0].pos)) { fell++; __real_Vu0Cur_ProjectInt(ib[0], &pb[0].pos); } }
                snap(&xb, &rb, 4);
                cut += k == G_ClipPlane && ra != 0 && ra != n;
                if (!same(&xa, &xb) || memcmp(pa, pb, sizeof(pa)) != 0 || memcmp(ia, ib, sizeof(ia)) != 0 || memcmp(sa, sb, sizeof(sa)) != 0) {
                    diff = i + 1;
                }
            }
            if (!diff) {
                sOn2[k] = 1;
            } else {
                bad++;
                Vita_Log("vita: the vector library's %s: the stand-in differs from the library's routine at operand set %d; not used\n", kName2[k], diff);
            }
            t0 = t1 = t2 = 0;
            if (!quick) {   /* the time per call: an ordinary triangle cut by its plane, 2000 times each way */
                CV p0[16], pw[16];
                V4 pl = {0.3f, -0.4f, 0.5f, 10.0f}, st[16];
                int32_t sc[16][4];
                memset(p0, 0, sizeof(p0));
                p0[0].pos.x = 100.0f; p0[0].pos.y = 20.0f; p0[0].pos.z = 30.0f; p0[0].pos.w = 1.0f;
                p0[1].pos.x = -80.0f; p0[1].pos.y = 40.0f; p0[1].pos.z = -50.0f; p0[1].pos.w = 1.0f;
                p0[2].pos.x = 10.0f; p0[2].pos.y = -60.0f; p0[2].pos.z = 70.0f; p0[2].pos.w = 1.0f;
                for (i = 0; i < 16; i++) { ((float *)&gRefVu0.vf[16])[i] = (float)(i - 7) * 0.375f; }
                ((float *)&gRefVu0.vf[19])[3] = 900.0f;
                t0 = sceKernelGetProcessTimeWide();
                for (i = 0; i < 2000; i++) {
                    memcpy(pw, p0, 3 * sizeof(CV));
                    if (k == G_ClipPlane) { (void)__real_ClipPoly_ClipPlane(pw, &pl, 3); }
                    else if (k == G_ProjectCur) { __real_ClipPoly_ProjectCur(sc, st, pw, 3); }
                    else { __real_Vu0Cur_ProjectInt(sc[0], &pw[0].pos); }
                }
                t1 = sceKernelGetProcessTimeWide();
                for (i = 0; i < 2000; i++) {
                    int r;
                    memcpy(pw, p0, 3 * sizeof(CV));
                    if (k == G_ClipPlane) { if (!fx_ClipPoly_ClipPlane(&r, pw, &pl, 3)) { (void)__real_ClipPoly_ClipPlane(pw, &pl, 3); } }
                    else if (k == G_ProjectCur) { if (!fx_ClipPoly_ProjectCur(sc, st, pw, 3)) { __real_ClipPoly_ProjectCur(sc, st, pw, 3); } }
                    else { if (!fx_Vu0Cur_ProjectInt(sc[0], &pw[0].pos)) { __real_Vu0Cur_ProjectInt(sc[0], &pw[0].pos); } }
                }
                t2 = sceKernelGetProcessTimeWide();
            }
            if (len > 0 && len < (int)sizeof(line) - 90) {
                len += snprintf(line + len, sizeof(line) - (unsigned)len, " %s %u/%u (%d tests, %u cut, %u to the library)%s;", kName2[k], (unsigned)((t1 - t0) / 2),
                                (unsigned)((t2 - t1) / 2), nTests, cut, fell, diff ? " DIFFERS, not used" : "");
            }
        }
        Vita_Log("%s\n", line);
    }
    {   /* the rotations: angles mostly within a half turn (as the game's), some anything; the matrix, the
           registers (vf4..vf19, ACC, Q, $t0) compared */
        RefVec4 keepGpr = gRefVu0.gpr[0];
        uint32_t keepQ = gRefVu0.q;
        len = snprintf(line, sizeof(line), "vita:   the rotations' stand-ins, ns per call (the library's / the stand-in's):");
        for (k = 0; k < R_COUNT; k++) {
            int diff = 0, nTests = 2000 / div, axis = k % 3, cur = k >= 3;
            unsigned long long t0, t1, t2;
            for (i = 0; i < nTests && !diff; i++) {
                M4 src, da, db, cm;
                V4 r12, r7;
                RefVec4 g8;
                float ang = (rnd() & 3) ? ((float)((int)(rnd() % 20001) - 10000) * 0.000314159f) : rfloat();
                Snap xa, xb;
                uint32_t qa, qb;
                RefVec4 ga, gb;
                rmtx(&src); rmtx(&cm); rvec(&r12); rvec(&r7);
                if (rnd() & 1) { /* (ordinary numbers) */
                    int j;
                    for (j = 0; j < 16; j++) {
                        ((float *)&src)[j] = (float)((int)(rnd() % 2001) - 1000) * 0.01f;
                        ((float *)&cm)[j] = (float)((int)(rnd() % 2001) - 1000) * 0.01f;
                    }
                    r12.z = (float)((int)(rnd() % 21) - 10); r12.w = -(float)(rnd() % 7);
                }
                g8.u[0] = rnd(); g8.u[1] = rnd(); g8.u[2] = rnd(); g8.u[3] = rnd();
                memset(&da, 0x5A, sizeof(da)); memset(&db, 0x5A, sizeof(db));
                memcpy(&gRefVu0.vf[4], keep + 4, 12 * sizeof(RefVec4));
                memcpy(&gRefVu0.vf[12], &r12, 16); memcpy(&gRefVu0.vf[7], &r7, 16);
                memcpy(&gRefVu0.vf[16], &cm, sizeof(cm));
                gRefVu0.gpr[0] = g8;
                if (!cur) { M4 sc = src; (axis == 0 ? __real_Mtx_RotateX : axis == 1 ? __real_Mtx_RotateY : __real_Mtx_RotateZ)(&da, &sc, ang); }
                else { (axis == 0 ? __real_Vu0Cur_RotateX : axis == 1 ? __real_Vu0Cur_RotateY : __real_Vu0Cur_RotateZ)(ang); }
                snap(&xa, &da, 64); qa = gRefVu0.q; ga = gRefVu0.gpr[0];
                memcpy(&gRefVu0.vf[4], keep + 4, 12 * sizeof(RefVec4));
                memcpy(&gRefVu0.vf[12], &r12, 16); memcpy(&gRefVu0.vf[7], &r7, 16);
                memcpy(&gRefVu0.vf[16], &cm, sizeof(cm));
                gRefVu0.gpr[0] = g8;
                if (!cur) { M4 sc = src; if (!fx_Mtx_Rotate(axis, &db, &sc, ang)) { (axis == 0 ? __real_Mtx_RotateX : axis == 1 ? __real_Mtx_RotateY : __real_Mtx_RotateZ)(&db, &sc, ang); } }
                else { if (!fx_Vu0Cur_Rotate(axis, ang)) { (axis == 0 ? __real_Vu0Cur_RotateX : axis == 1 ? __real_Vu0Cur_RotateY : __real_Vu0Cur_RotateZ)(ang); } }
                snap(&xb, &db, 64); qb = gRefVu0.q; gb = gRefVu0.gpr[0];
                if (!same(&xa, &xb) || qa != qb || memcmp(&ga, &gb, 16) != 0) {
                    diff = i + 1;
                    Vita_Log("vita: rotation %s differs (angle %08x): q %08x/%08x t0 %08x %08x/%08x %08x\n", kName3[k], fbits(ang), qa, qb, ga.u[0], ga.u[1], gb.u[0], gb.u[1]);
                    {
                        int r2, c2;
                        for (r2 = 0; r2 < 16; r2++) {
                            for (c2 = 0; c2 < 4; c2++) {
                                if (xa.vf[r2].u[c2] != xb.vf[r2].u[c2]) {
                                    Vita_Log("vita:   vf%d.%c %08x / %08x\n", r2 + 4, "xyzw"[c2], xa.vf[r2].u[c2], xb.vf[r2].u[c2]);
                                }
                            }
                        }
                        for (c2 = 0; c2 < 4; c2++) {
                            if (xa.acc.u[c2] != xb.acc.u[c2]) {
                                Vita_Log("vita:   acc.%c %08x / %08x\n", "xyzw"[c2], xa.acc.u[c2], xb.acc.u[c2]);
                            }
                        }
                        if (memcmp(xa.out, xb.out, 64) != 0) {
                            Vita_Log("vita:   the matrix differs\n");
                        }
                    }
                }
            }
            if (!diff) {
                sOn3[k] = 1;
            } else {
                bad++;
            }
            t0 = t1 = t2 = 0;
            if (!quick) {
                M4 src, d;
                for (i = 0; i < 16; i++) { ((float *)&src)[i] = (float)(i - 7) * 0.375f; }
                t0 = sceKernelGetProcessTimeWide();
                for (i = 0; i < 2000; i++) {
                    if (!cur) { (axis == 0 ? __real_Mtx_RotateX : axis == 1 ? __real_Mtx_RotateY : __real_Mtx_RotateZ)(&d, &src, 0.3f); }
                    else { memcpy(&gRefVu0.vf[16], &src, sizeof(src)); (axis == 0 ? __real_Vu0Cur_RotateX : axis == 1 ? __real_Vu0Cur_RotateY : __real_Vu0Cur_RotateZ)(0.3f); }
                }
                t1 = sceKernelGetProcessTimeWide();
                for (i = 0; i < 2000; i++) {
                    if (!cur) { (void)fx_Mtx_Rotate(axis, &d, &src, 0.3f); }
                    else { memcpy(&gRefVu0.vf[16], &src, sizeof(src)); (void)fx_Vu0Cur_Rotate(axis, 0.3f); }
                }
                t2 = sceKernelGetProcessTimeWide();
            }
            if (len > 0 && len < (int)sizeof(line) - 60) {
                len += snprintf(line + len, sizeof(line) - (unsigned)len, " %s %u/%u%s;", kName3[k], (unsigned)((t1 - t0) / 2), (unsigned)((t2 - t1) / 2), diff ? " DIFFERS, not used" : "");
            }
        }
        Vita_Log("%s\n", line);
        gRefVu0.gpr[0] = keepGpr;
        gRefVu0.q = keepQ;
    }
    memcpy(gRefVu0.vf, keep, sizeof(keep)); /* (the registers as they were: the current matrix too) */
    memcpy(&gRefVu0.acc, &keepAcc, sizeof(keepAcc));
    Vita_Log("vita:   %d of %d stand-ins used; the check took %u ms%s\n", F_COUNT + G_COUNT + R_COUNT - bad, F_COUNT + G_COUNT + R_COUNT,
             (unsigned)((sceKernelGetProcessTimeWide() - tq) / 1000), quick ? " (the short one: no times per call)" : "");
    if (!quick) {   /* what a reading of the process clock costs: the drawing thread reads it a few hundred times a
           picture for the log */
        unsigned long long c0 = sceKernelGetProcessTimeWide(), c1;
        for (i = 0; i < 1000; i++) {
            (void)sceKernelGetProcessTimeWide();
        }
        c1 = sceKernelGetProcessTimeWide();
        Vita_Log("vita:   a reading of the process clock takes %u ns\n", (unsigned)(c1 - c0));
    }
    return 0;
}
