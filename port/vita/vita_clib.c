/*
 * memcmp, faster (build.py: --wrap=memcmp). VitaSDK's is newlib's small one: a word at a time with a loop's worth of
 * bookkeeping per word, and on the drawing thread it was 6% of the samples in a profile (Kame House):
 * the renderer compares its states (36 bytes), matrices (64) and vertex programs' constants (384) to find what is
 * unchanged. This compares 16 bytes per pass with one test; when a pass differs, the bytes of that pass are compared
 * one by one for the order, so the result is memcmp's to the bit (the first differing byte, as unsigned chars).
 * Pointers at different offsets from a word go a byte at a time, as newlib's do.
 *
 * (No loop here is one the compiler turns into a call of memcmp: it does that for copies and fills only.)
 */
#include <stddef.h>
#include <stdint.h>

#pragma GCC optimize("O2", "no-tree-vectorize", "no-unroll-loops") /* (vectorized, the byte loop made it save d8-d15 on every call) */

typedef uint32_t __attribute__((may_alias)) Word;

int __wrap_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a, *q = (const uint8_t *)b;
    if (n >= 8 && (((uintptr_t)p ^ (uintptr_t)q) & 3) == 0) {
        while (((uintptr_t)p & 3) != 0) { /* (both at the same offset from a word: up to it) */
            if (*p != *q) {
                return (int)*p - (int)*q;
            }
            p++;
            q++;
            n--;
        }
        while (n >= 16) {
            const Word *x = (const Word *)(const void *)p, *y = (const Word *)(const void *)q;
            if (((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3])) != 0) {
                break; /* (the difference is in these 16: found below) */
            }
            p += 16;
            q += 16;
            n -= 16;
        }
        while (n >= 4 && *(const Word *)(const void *)p == *(const Word *)(const void *)q) {
            p += 4;
            q += 4;
            n -= 4;
        }
    }
    for (; n != 0; n--, p++, q++) {
        if (*p != *q) {
            return (int)*p - (int)*q;
        }
    }
    return 0;
}
