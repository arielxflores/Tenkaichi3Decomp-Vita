/*
 * Forced into every game source of the PC build (-include port_compat.h), in front of the game's own headers.
 * The game sources under src/ and include/ are shared with the matching decompilation and are not edited for the
 * port unless there is no other way; differences between the PS2 compiler and a host compiler are bridged here.
 */
#ifndef PORT_COMPAT_H
#define PORT_COMPAT_H

#define PORT 1

/* The game's types: on the PS2 `long` is 64 bits. Take over include/types.h. */
#define TYPES_H
typedef signed char s8;
typedef unsigned char u8;
typedef short s16;
typedef unsigned short u16;
typedef int s32;
typedef unsigned int u32;
typedef long long s64;
typedef unsigned long long u64;
typedef float f32;

/* Functions the matching build takes from assembly: the port supplies them elsewhere (see docs/port/). */
#define INCLUDE_ASM_H
#define INCLUDE_ASM(FOLDER, NAME)
#define INCLUDE_RODATA(FOLDER, NAME)
#define RODATA_ALIGN16()
#define LIT4_WORD(NAME, HEX)
#define ASM_STUB_BEGIN() extern int asm_stub_begin_
#define ASM_STUB_END() extern int asm_stub_end_

/* PS Vita: the PS2's hardware-register addresses (0x1000xxxx: timers, DMA, VIF, GIF; 0x1200xxxx: the GS) cannot be
   mapped there, so the register macros of the game go through PORT_HW() to two blocks of ordinary memory
   (port/vita/vita_mem.c). On PC the registers are plain memory at the PS2's own addresses (port/src/plat_mem.c). */
#ifdef PORT_VITA
extern unsigned char gPortHw[], gPortGsReg[];
#define PORT_HW(a) ((a) >= 0x12000000u ? (unsigned)gPortGsReg + ((a) - 0x12000000u) : (unsigned)gPortHw + ((a) - 0x10000000u))
#endif

/* C library calls of the game that must not go to the host's versions (see port/src/plat_mem.c). */
#define malloc Port_Malloc

#ifdef PORT_VITA
/* PS Vita: the game's float division (port/vita/srcfix.py divs() writes every `a / b` of floats in the game's text
   as __port_fdiv(a, b), and `a /= b` as __port_fdiv_to(&(a), b)). The game's code runs on the Vita's float unit,
   where x / 0 is an infinity and 0 / 0 a NaN; the PS2's gives the largest number, of the sign of the two operands
   (a divisor that is a denormal is a zero there). Neither exists on the PS2, and the game's loops are not made for
   them: a NaN or an infinity that reached a fighter's height, an angle being wrapped or a path's length made a loop
   that never ends (the game stood still, the music played on). */
static inline __attribute__((always_inline)) float __port_fdiv(float a, float b) {
    union { float f; unsigned u; } x, y;
    y.f = b;
    if (__builtin_expect((y.u & 0x7F800000u) == 0, 0)) {
        x.f = a;
        x.u = ((x.u ^ y.u) & 0x80000000u) | 0x7F7FFFFFu;
        return x.f;
    }
    return a / b;
}
static inline __attribute__((always_inline)) float __port_fdiv_to(float *p, float b) {
    return *p = __port_fdiv(*p, b);
}
#endif

#endif
