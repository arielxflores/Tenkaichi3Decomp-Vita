/*
 * PS Vita: the sound streams brought to the audio port's 48 kHz.
 *
 * The game's music and voices are ADX streams at 24 kHz (a few at 48 kHz; songs added from outside the disc at up to
 * 32 kHz), the Vita's audio port takes 48 kHz. Until now the samples in between were put on the straight line
 * between two input samples. That is cheap, but it is a poor filter. Measured with test tones at 24 kHz in: the
 * treble comes out 1.4 dB down at 6 kHz, 3.2 dB at 9 kHz and 4 dB at 10 kHz (the music duller than it is), and each
 * tone's mirror image above 12 kHz is only 17 dB under it at 6 kHz and 9 dB at 10 kHz (a rough edge on cymbals and
 * on the voices' s sounds).
 *
 * Here each output sample is made of the 32 input samples around its moment, weighted with a windowed sinc (a
 * low-pass at the input's upper limit, Kaiser window). The same tones: level within 0.01 dB up to 10 kHz (0.3 dB
 * down at 10.5 kHz, the filter's edge above that), the mirror images 70 dB down or more up to 10.5 kHz. The ratio
 * is taken exactly (48000 / rate = L / M in lowest terms: for 24 kHz two positions between input samples, for
 * 16 and 32 kHz three, for 44.1 kHz 160), one row of weights per position, so no position is rounded and nothing
 * drifts. The weights are 2.14 fixed point, each row summing to one exactly (a steady level stays itself at every
 * position: no ripple at the rows' rate); a sample is 2 x 32 multiply-adds, done eight at a time on the processor's
 * vector unit (whole numbers: the same result as one at a time, which is what the test on a PC runs).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vita_resample.h"
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define RS_NEON 1
#endif

#define RS_TABLES 8
#define RS_MAX_L 640      /* (11.025 kHz) */
#define RS_BETA 7.0       /* the Kaiser window's shape: mirror images about 70 dB down */
#define RS_EDGE 0.97      /* the low-pass's half-way point, as a part of the input's (or output's) upper limit */

static VitaRs sTab[RS_TABLES];
static int sTabN;

static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0, q = x * x * 0.25;
    int k;
    for (k = 1; k < 40; k++) {
        term *= q / ((double)k * (double)k);
        sum += term;
        if (term < sum * 1e-12) {
            break;
        }
    }
    return sum;
}

static int gcd(int a, int b) {
    while (b != 0) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

const VitaRs *VitaRs_Get(int freq) {
    int k, g, L, M, p, j;
    int16_t *coef;
    double fc, i0b;

    if (freq <= 0 || freq == VITA_RS_OUT) {
        return NULL;
    }
    for (k = 0; k < sTabN; k++) {
        if (sTab[k].freq == freq) {
            return &sTab[k];
        }
    }
    g = gcd(freq, VITA_RS_OUT);
    L = VITA_RS_OUT / g;
    M = freq / g;
    if (sTabN >= RS_TABLES || L > RS_MAX_L) {
        return NULL;
    }
    coef = malloc((size_t)L * VITA_RS_TAPS * sizeof(int16_t));
    if (coef == NULL) {
        return NULL;
    }
    /* the low-pass, in cycles per input sample: the lower of the two rates' upper limits */
    fc = 0.5 * RS_EDGE * (freq > VITA_RS_OUT ? (double)VITA_RS_OUT / (double)freq : 1.0);
    i0b = bessel_i0(RS_BETA);
    for (p = 0; p < L; p++) {
        double w[VITA_RS_TAPS], sum = 0.0;
        int q[VITA_RS_TAPS], total = 0, big = 0;
        for (j = 0; j < VITA_RS_TAPS; j++) {
            /* the output sample is p / L after input frame VITA_RS_PAST of the window: this frame's distance from it */
            double t = (double)(j - VITA_RS_PAST) - (double)p / (double)L;
            double u = t / (double)(VITA_RS_TAPS / 2); /* -1 .. 1 across the window */
            double win = u <= -1.0 || u >= 1.0 ? 0.0 : bessel_i0(RS_BETA * sqrt(1.0 - u * u)) / i0b;
            double x = 2.0 * 3.14159265358979323846 * fc * t;
            w[j] = (t > -1e-9 && t < 1e-9 ? 1.0 : sin(x) / x) * 2.0 * fc * win;
            sum += w[j];
        }
        for (j = 0; j < VITA_RS_TAPS; j++) {
            double v = w[j] / sum * 16384.0;
            q[j] = (int)(v < 0.0 ? v - 0.5 : v + 0.5);
            total += q[j];
            if (abs(q[j]) > abs(q[big])) {
                big = j;
            }
        }
        q[big] += 16384 - total; /* (the row sums to one exactly: the rounding's rest goes to its largest weight) */
        for (j = 0; j < VITA_RS_TAPS; j++) {
            coef[p * VITA_RS_TAPS + j] = (int16_t)q[j];
        }
    }
    sTab[sTabN].freq = freq;
    sTab[sTabN].L = L;
    sTab[sTabN].M = M;
    sTab[sTabN].stepFrames = M / L;
    sTab[sTabN].stepPhase = M % L;
    sTab[sTabN].coef = coef;
    return &sTab[sTabN++];
}

int VitaRs_Need(const VitaRs *rs, int phase, int out) {
    if (out <= 0) {
        return 0;
    }
    return (int)(((long long)phase + (long long)(out - 1) * rs->M) / rs->L) + VITA_RS_TAPS;
}

int VitaRs_Can(const VitaRs *rs, int phase, int have) {
    long long room;
    if (have < VITA_RS_TAPS) {
        return 0;
    }
    /* the last sample's window starts at frame (phase + (n - 1) M) / L, which has to be at most have - TAPS */
    room = (long long)(have - VITA_RS_TAPS + 1) * rs->L - 1 - phase;
    return room < 0 ? 0 : (int)(room / rs->M) + 1;
}

int VitaRs_Mix(const VitaRs *rs, const int16_t *in, int *phase, int32_t *mix, int out, int gain) {
    int ph = *phase, pos = 0, i;
    const int L = rs->L, sf = rs->stepFrames, sp = rs->stepPhase;

    for (i = 0; i < out; i++) {
        const int16_t *x = in + pos * 2, *c = rs->coef + ph * VITA_RS_TAPS;
        int32_t l, r;
#ifdef RS_NEON
        int32x4_t al = vdupq_n_s32(0), ar = vdupq_n_s32(0);
        int j;
        for (j = 0; j < VITA_RS_TAPS; j += 8) {
            int16x8x2_t v = vld2q_s16(x + j * 2); /* eight frames: the left ones, the right ones */
            int16x8_t w = vld1q_s16(c + j);
            al = vmlal_s16(al, vget_low_s16(v.val[0]), vget_low_s16(w));
            al = vmlal_s16(al, vget_high_s16(v.val[0]), vget_high_s16(w));
            ar = vmlal_s16(ar, vget_low_s16(v.val[1]), vget_low_s16(w));
            ar = vmlal_s16(ar, vget_high_s16(v.val[1]), vget_high_s16(w));
        }
        {
            int32x2_t sl = vadd_s32(vget_low_s32(al), vget_high_s32(al)), sr = vadd_s32(vget_low_s32(ar), vget_high_s32(ar));
            l = vget_lane_s32(vpadd_s32(sl, sl), 0);
            r = vget_lane_s32(vpadd_s32(sr, sr), 0);
        }
#else
        int j;
        l = r = 0;
        for (j = 0; j < VITA_RS_TAPS; j++) {
            l += (int32_t)x[j * 2] * c[j];
            r += (int32_t)x[j * 2 + 1] * c[j];
        }
#endif
        mix[i * 2] += (((l + 8192) >> 14) * gain) >> 8;
        mix[i * 2 + 1] += (((r + 8192) >> 14) * gain) >> 8;
        pos += sf;
        ph += sp;
        if (ph >= L) {
            ph -= L;
            pos++;
        }
    }
    *phase = ph;
    return pos;
}
