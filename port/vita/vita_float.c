/*
 * Float arithmetic helpers on the PS Vita.
 *
 * The game's own code uses the processor's float unit directly. What still goes through here:
 *   - the vector library (src/port/vu0_a.c, vu0_b.c): its add, multiply and divide follow the PS2 vector unit's
 *     truncating arithmetic (vita_vu0_inline.h has the same primitives inline);
 *   - the compiler's run-time helpers referenced by the objects built with the PS2 compiler's rules (build.py
 *     renames them to the PortF_ / PortD_ names defined here), and a few maths functions.
 *
 * Two models of the PS2's float arithmetic are kept:
 *   exact: port/src/softfloat_ps2.c, the PS2 float model in integer code;
 *   hardware: the same model on the Vita's float unit, set to round toward zero with denormals as zero
 *      (Vita_FloatMode), which is the model's truncation; around that: the PS2 adder's cut of the smaller operand
 *      is done on the bit patterns first, the FPU's division switches to round-to-nearest for its one instruction,
 *      a result with the largest exponent becomes the PS2's largest number, and operands that look like infinity
 *      or NaN go to the exact routine. It gives the same bits as the exact model.
 * The hardware model is compiled in; the exact routines stay for the operands handed back to them and for the
 * self-test (Vita_FloatSelfTest, on the development build's start screen), which compares the two on the real
 * float unit and the exact model with the sums computed on the PC (port/vita/floattest.c).
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

int gPortFastFloat;
#define FAST 1         /* the hardware model in every call (the self-test calls the exact routines itself) */
#define N(i) ((void)0) /* (a place to count the calls by kind) */

#define SIGN 0x80000000u
#define FMAX 0x7F7FFFFFu
#define EXP(x) (((x) >> 23) & 0xFF)

/* The exact model's routines (port/src/softfloat_ps2.c; renamed by build.py, see RENAME_DEFS there). */
extern uint32_t PortX_addsf3(uint32_t a, uint32_t b), PortX_subsf3(uint32_t a, uint32_t b);
extern uint32_t PortX_mulsf3(uint32_t a, uint32_t b), PortX_divsf3(uint32_t a, uint32_t b);
extern int32_t PortX_fixsfsi(uint32_t a);
extern uint32_t PortX_fixunssfsi(uint32_t a), PortX_floatsisf(int32_t v), PortX_floatunsisf(uint32_t v);
extern uint32_t PortX_VuAdd(uint32_t a, uint32_t b), PortX_VuMul(uint32_t a, uint32_t b), PortX_VuDiv(uint32_t a, uint32_t b);

static inline float f(uint32_t u) { float x; memcpy(&x, &u, 4); return x; }
static inline uint32_t u(float x) { uint32_t v; memcpy(&v, &x, 4); return v; }
static inline double d(uint64_t v) { double x; memcpy(&x, &v, 8); return x; }
static inline uint64_t q(double x) { uint64_t v; memcpy(&v, &x, 8); return v; }
#ifndef __arm__
uint32_t Host_DivNearest(uint32_t a, uint32_t b); /* fastcmp.c: the PC stand-in for the three instructions below */
#endif

/* The float unit's control register for the calling thread: fast = round toward zero, flush to zero, default NaN. */
void Vita_FloatMode(int fast) {
#ifdef __arm__
    uint32_t fpscr;

    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr &= ~((3u << 22) | (1u << 24) | (1u << 25));
    if (fast) {
        fpscr |= (3u << 22) | (1u << 24) | (1u << 25);
    }
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
#else
    (void)fast; /* the PC program that computes the expected sums (floattest.c) only runs the exact model */
#endif
}

/* No infinity and no NaN on the PS2: such a result is the largest number of its sign. */
static inline uint32_t out(uint32_t r) {
    return EXP(r) == 255 ? (r & SIGN) | FMAX : r;
}

/* An operand with the largest exponent (an infinity or NaN pattern; a number to the PS2): the exact routines. */
#define SPECIAL(a, b) __builtin_expect((((a) & 0x7F800000u) == 0x7F800000u) | (((b) & 0x7F800000u) == 0x7F800000u), 0)

/* The FPU's add.s: the smaller operand loses its low bits first (softfloat_ps2.c, fpu_add), then a truncating add. */
static inline uint32_t fast_fpu_add(uint32_t a, uint32_t b) {
    int32_t dd = (int32_t)EXP(a) - (int32_t)EXP(b);

    if (SPECIAL(a, b)) {
        return PortX_addsf3(a, b);
    }
    if (dd > 0) {
        b &= dd >= 25 ? SIGN : 0xFFFFFFFFu << (dd - 1);
    } else if (dd < 0) {
        a &= dd <= -25 ? SIGN : 0xFFFFFFFFu << (-dd - 1);
    }
    return out(u(f(a) + f(b)));
}

/* The FPU's div.s rounds to nearest (the vector unit's division truncates like everything else). */
static inline uint32_t fast_fpu_div(uint32_t a, uint32_t b) {
    uint32_t r;

    if (SPECIAL(a, b)) {
        return PortX_divsf3(a, b);
    }
    if (EXP(b) == 0) {
        return ((a ^ b) & SIGN) | FMAX; /* x / 0 and 0 / 0 */
    }
    if (EXP(a) == 0) {
        return (a ^ b) & SIGN;
    }
#ifdef __arm__
    {
        uint32_t fpscr;
        float x = f(a), y = f(b), z;
        __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
        __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr & ~(3u << 22)));
        __asm__ volatile("vdiv.f32 %0, %1, %2" : "=t"(z) : "t"(x), "t"(y));
        __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
        r = u(z);
    }
#else
    r = Host_DivNearest(a, b);
#endif
    return out(r);
}

static inline uint32_t fast_vu_div(uint32_t a, uint32_t b) {
    if (SPECIAL(a, b)) {
        return PortX_VuDiv(a, b);
    }
    if (EXP(b) == 0) {
        return ((a ^ b) & SIGN) | FMAX;
    }
    if (EXP(a) == 0) {
        return (a ^ b) & SIGN;
    }
    return out(u(f(a) / f(b)));
}

/* ---- single precision: what the compiler calls for the game's float operations ---- */
uint32_t PortF_add(uint32_t a, uint32_t b) { N(0); return FAST ? fast_fpu_add(a, b) : PortX_addsf3(a, b); }
uint32_t PortF_sub(uint32_t a, uint32_t b) { N(0); return FAST ? fast_fpu_add(a, b ^ SIGN) : PortX_subsf3(a, b); }
uint32_t PortF_rsub(uint32_t a, uint32_t b) { return PortF_sub(b, a); }
uint32_t PortF_mul(uint32_t a, uint32_t b) {
    N(1);
    return FAST ? (SPECIAL(a, b) ? PortX_mulsf3(a, b) : out(u(f(a) * f(b)))) : PortX_mulsf3(a, b);
}
uint32_t PortF_div(uint32_t a, uint32_t b) { N(2); return FAST ? fast_fpu_div(a, b) : PortX_divsf3(a, b); }

/* Comparisons: every pattern is a number, compared as sign and magnitude with +0 == -0 (RefVu0_LtBits). Integer
   code, the same in both models. */
static inline int32_t key(uint32_t a) {
    return (a & SIGN) ? -(int32_t)(a & 0x7FFFFFFF) : (int32_t)(a & 0x7FFFFFFF);
}
int PortF_cmpeq(uint32_t a, uint32_t b) { N(3); return key(a) == key(b); }
int PortF_cmplt(uint32_t a, uint32_t b) { N(3); return key(a) < key(b); }
int PortF_cmple(uint32_t a, uint32_t b) { N(3); return key(a) <= key(b); }
int PortF_cmpgt(uint32_t a, uint32_t b) { N(3); return key(a) > key(b); }
int PortF_cmpge(uint32_t a, uint32_t b) { N(3); return key(a) >= key(b); }
int PortF_cmpun(uint32_t a, uint32_t b) { (void)a; (void)b; return 0; }

/* Conversions. Float -> integer is short integer code in the exact model already. Integer -> float is a
   normalising loop there; with the control register set, the float unit's conversion truncates the same way. */
int32_t PortF_f2iz(uint32_t a) { N(4); return PortX_fixsfsi(a); }
uint32_t PortF_i2f(int32_t v) { N(5); return FAST ? u((float)v) : PortX_floatsisf(v); }
uint32_t PortF_f2uiz(uint32_t a) { N(4); return PortX_fixunssfsi(a); }  /* the PS2 compiler's own sequence */
uint32_t PortF_ui2f(uint32_t v) { N(5); return PortX_floatunsisf(v); }
int64_t PortF_f2lz(uint32_t a) { return (int64_t)f(a); }
uint64_t PortF_f2ulz(uint32_t a) { return (uint64_t)f(a); }
uint32_t PortF_l2f(int64_t v) { return u((float)v); }
uint32_t PortF_ul2f(uint64_t v) { return u((float)v); }

/* ---- the vector unit's arithmetic, used by the vector library (src/port/vu0_a.c with REF_VU0_EXTERN_ARITH) ---- */
uint32_t RefVu0_AddBits(uint32_t a, uint32_t b) {
    N(6);
    return FAST && !SPECIAL(a, b) ? out(u(f(a) + f(b))) : PortX_VuAdd(a, b);
}
uint32_t RefVu0_MulBits(uint32_t a, uint32_t b) {
    N(6);
    return FAST && !SPECIAL(a, b) ? out(u(f(a) * f(b))) : PortX_VuMul(a, b);
}
uint32_t RefVu0_DivBits(uint32_t a, uint32_t b) { N(6); return FAST ? fast_vu_div(a, b) : PortX_VuDiv(a, b); }

/* ---- double precision: software IEEE arithmetic on the PS2 as well, so the float unit's is the same. Rare in the
   game. (In the fast model the thread rounds toward zero; nothing of the simulation is known to depend on it.) ---- */
uint64_t PortD_add(uint64_t a, uint64_t b) { N(7); return q(d(a) + d(b)); }
uint64_t PortD_sub(uint64_t a, uint64_t b) { N(7); return q(d(a) - d(b)); }
uint64_t PortD_rsub(uint64_t a, uint64_t b) { return q(d(b) - d(a)); }
uint64_t PortD_mul(uint64_t a, uint64_t b) { N(7); return q(d(a) * d(b)); }
uint64_t PortD_div(uint64_t a, uint64_t b) { N(7); return q(d(a) / d(b)); }
int PortD_cmpeq(uint64_t a, uint64_t b) { return d(a) == d(b); }
int PortD_cmplt(uint64_t a, uint64_t b) { return d(a) < d(b); }
int PortD_cmple(uint64_t a, uint64_t b) { return d(a) <= d(b); }
int PortD_cmpgt(uint64_t a, uint64_t b) { return d(a) > d(b); }
int PortD_cmpge(uint64_t a, uint64_t b) { return d(a) >= d(b); }
int PortD_cmpun(uint64_t a, uint64_t b) { return d(a) != d(a) || d(b) != d(b); }
uint64_t PortD_f2d(uint32_t a) { N(7); return q((double)f(a)); }
uint32_t PortD_d2f(uint64_t a) { N(7); return u((float)d(a)); }
int32_t PortD_d2iz(uint64_t a) { return (int32_t)d(a); }
uint32_t PortD_d2uiz(uint64_t a) { return (uint32_t)d(a); }
int64_t PortD_d2lz(uint64_t a) { return (int64_t)d(a); }
uint64_t PortD_d2ulz(uint64_t a) { return (uint64_t)d(a); }
uint64_t PortD_i2d(int32_t v) { return q((double)v); }
uint64_t PortD_ui2d(uint32_t v) { return q((double)v); }
uint64_t PortD_l2d(int64_t v) { return q((double)v); }
uint64_t PortD_ul2d(uint64_t v) { return q((double)v); }

/* ---- the C maths functions that are not built from the PS2's library sources (port/src/plat_libm.c on PC):
   not called by the simulation. ---- */
/* (no NaN and no infinity handed back to the game: the PS2's largest number of that sign, as its float unit gives) */
static float ps2(float r) {
    uint32_t v = u(r);
    return EXP(v) == 255 ? f((v & 0x7FFFFFu) != 0 ? FMAX : (v & SIGN) | FMAX) : r;
}
float Port_ceilf(float a) { return ps2(ceilf(a)); }
float Port_expf(float a) { return ps2(expf(a)); }
float Port_logf(float a) { return ps2(logf(a)); }
float Port_fmodf(float a, float b) { return ps2(fmodf(a, b)); }
/* pow with double arguments: one caller in the game (the replay menu's digits, src/menu/menu_za_d.c). */
uint64_t PortD_pow(uint64_t a, uint64_t b) { return q(pow(d(a), d(b))); }

/* ---- self-test and speed ----
   The same operand stream on every machine (a 64-bit linear generator): operands with exponents near 1.0, so that
   rounding and cancellation happen, and every 16th pair as raw bit patterns (huge, tiny, "infinity" patterns).
   kExpected holds the sums of the exact model's results as computed on the PC by port/vita/floattest.c from the
   same sources; a difference on the Vita means the ARM build of the model does not compute what the PC's does. */
enum { TEST_OPS = 7, TEST_N = 200000 };
static const char *const kOpName[TEST_OPS] = {"add", "sub", "mul", "div", "f2i", "i2f", "vu add/mul/div"};
#ifdef FLOATTEST_HOST
static const uint32_t kExpected[TEST_OPS];
#else
#include "vita_float_expected.h" /* written by build.py from floattest.c's output */
#endif

static uint64_t sRng;
static uint32_t rnd(void) {
    sRng = sRng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(sRng >> 32);
}
static uint32_t operand(int raw) {
    uint32_t r = rnd();
    return raw ? r : (r & 0x807FFFFFu) | ((115u + ((r >> 23) & 31)) << 23);
}

static uint32_t run_op(int op, uint32_t a, uint32_t b) {
    if (!gPortFastFloat) { /* the exact model, called directly (the entry points below it may be fixed to the other) */
        switch (op) {
            case -1: return a ^ b;
            case 0: return PortX_addsf3(a, b);
            case 1: return PortX_subsf3(a, b);
            case 2: return PortX_mulsf3(a, b);
            case 3: return PortX_divsf3(a, b);
            case 4: return (uint32_t)PortX_fixsfsi(a) + PortX_fixunssfsi(b);
            case 5: return PortX_floatsisf((int32_t)a) ^ PortX_floatunsisf(b);
            default: return PortX_VuAdd(a, b) + PortX_VuMul(a, b) * 3 + PortX_VuDiv(a, b) * 5;
        }
    }
    switch (op) {
        case -1: return a ^ b; /* the loop alone, for the timing */
        case 0: return PortF_add(a, b);
        case 1: return PortF_sub(a, b);
        case 2: return PortF_mul(a, b);
        case 3: return PortF_div(a, b);
        case 4: return (uint32_t)PortF_f2iz(a) + PortF_f2uiz(b);
        case 5: return PortF_i2f((int32_t)a) ^ PortF_ui2f(b);
        default: return RefVu0_AddBits(a, b) + RefVu0_MulBits(a, b) * 3 + RefVu0_DivBits(a, b) * 5;
    }
}

uint32_t Vita_FloatSum(int op) {
    uint32_t sum = 0;
    int i;

    sRng = 0x1234567 + (uint64_t)op;
    for (i = 0; i < TEST_N; i++) {
        uint32_t a = operand((i & 15) == 15), b = operand((i & 15) == 15);
        sum = (sum << 1 | sum >> 31) ^ run_op(op, a, b);
    }
    return sum;
}

/* Fills report lines; returns the number of operations whose exact sum differs from the PC's (0 = good).
   nsPerOp[model][op]: nanoseconds per loop round minus the empty loop's (ops 4 to 6 make two or three calls per
   round); diffPermille[op]: how often the fast model's result differs from the exact one, in results per
   thousand. now() gives microseconds. */
int Vita_FloatSelfTest(unsigned long long (*now)(void), unsigned nsPerOp[2][TEST_OPS], unsigned diffPermille[TEST_OPS], const char **names) {
    int op, i, bad = 0, wasFast = gPortFastFloat;
    unsigned long long t0 = now(), empty;

    Vita_FloatSum(-1);
    empty = now() - t0;
    for (op = 0; op < TEST_OPS; op++) {
        unsigned long long t;
        unsigned diff = 0;

        names[op] = kOpName[op];
        gPortFastFloat = 0;
        Vita_FloatMode(0);
        t0 = now();
        if (Vita_FloatSum(op) != kExpected[op]) {
            bad++;
        }
        t = now() - t0;
        nsPerOp[0][op] = (unsigned)((t > empty ? t - empty : 0) * 1000ull / TEST_N);
        gPortFastFloat = 1;
        Vita_FloatMode(1);
        t0 = now();
        Vita_FloatSum(op);
        t = now() - t0;
        nsPerOp[1][op] = (unsigned)((t > empty ? t - empty : 0) * 1000ull / TEST_N);
        sRng = 0x1234567 + (uint64_t)op;
        for (i = 0; i < TEST_N / 10; i++) {
            uint32_t a = operand((i & 15) == 15), b = operand((i & 15) == 15), x, y;
            gPortFastFloat = 0;
            Vita_FloatMode(0);
            x = run_op(op, a, b);
            gPortFastFloat = 1;
            Vita_FloatMode(1);
            y = run_op(op, a, b);
            diff += x != y;
        }
        diffPermille[op] = diff * 1000u / (TEST_N / 10);
    }
    gPortFastFloat = wasFast;
    Vita_FloatMode(wasFast);
    return bad;
}
