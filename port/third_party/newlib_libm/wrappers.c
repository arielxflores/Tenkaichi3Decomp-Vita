/* acosf, asinf, atan2f, powf, sqrtf as the PS2 executable has them: plain jumps to the __ieee754 routines
   (newlib's wf_*.c built with _IEEE_LIBM). */
#include "fdlibm.h"
#ifdef PORT_VITA
/* (Vita, hardware floats) the PS2 has no NaN and no infinity: where these routines make one (acos or asin of a number
   a hair beyond 1, a power of a negative number, an overflow) its float unit gives the largest number instead,
   which the game's callers survive. A NaN that went on into the game's numbers could stop it for good. */
static float ps2(float r) {
    __uint32_t u;
    GET_FLOAT_WORD(u, r);
    if ((u & 0x7f800000) == 0x7f800000) {
        u = (u & 0x007fffff) != 0 ? 0x7f7fffff : (u & 0x80000000) | 0x7f7fffff; /* NaN: + (0 / 0 there); infinity: its sign */
        SET_FLOAT_WORD(r, u);
    }
    return r;
}
float acosf(float x) { return ps2(__ieee754_acosf(x)); }
float asinf(float x) { return ps2(__ieee754_asinf(x)); }
float atan2f(float y, float x) { return ps2(__ieee754_atan2f(y, x)); }
float powf(float x, float y) { return ps2(__ieee754_powf(x, y)); }
float sqrtf(float x) { return ps2(__ieee754_sqrtf(x)); }
#else
float acosf(float x) { return __ieee754_acosf(x); }
float asinf(float x) { return __ieee754_asinf(x); }
float atan2f(float y, float x) { return __ieee754_atan2f(y, x); }
float powf(float x, float y) { return __ieee754_powf(x, y); }
float sqrtf(float x) { return __ieee754_sqrtf(x); }
#endif
