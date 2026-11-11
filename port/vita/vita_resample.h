/* PS Vita: the sound streams brought to the audio port's 48 kHz (port/vita/vita_resample.c, used by vita_audio.c). */
#ifndef VITA_RESAMPLE_H
#define VITA_RESAMPLE_H
#include <stdint.h>

#define VITA_RS_OUT 48000
#define VITA_RS_TAPS 32                       /* input frames each output sample is made of */
#define VITA_RS_PAST (VITA_RS_TAPS / 2 - 1)   /* of them before the sample's own moment (the rest at and after it) */

typedef struct VitaRs {
    int freq;       /* the input rate */
    int L, M;       /* 48000 / freq = L / M in lowest terms: L positions between two input frames, M of them a step */
    int stepFrames, stepPhase; /* M / L and M % L */
    const int16_t *coef;       /* L rows of VITA_RS_TAPS weights, 2.14 */
} VitaRs;

/* The table for an input rate (made once and kept); NULL for 48 kHz itself and for a rate with too many positions
   (not one of the usual ones): those are played as before. Not safe against itself: call under the sound's lock. */
const VitaRs *VitaRs_Get(int freq);

/* Input frames the next `out` output samples read, counted from the oldest frame of the first one's window. */
int VitaRs_Need(const VitaRs *rs, int phase, int out);

/* How many output samples `have` input frames are enough for (0 when not even one). */
int VitaRs_Can(const VitaRs *rs, int phase, int have);

/* `out` samples of 48 kHz added into mix (stereo, 32 bits) from `in` (stereo frames, 16 bits; in[0], in[1] the oldest
   frame of the first sample's window; VitaRs_Need frames must be there), each times gain / 256. *phase is the
   position between two input frames, carried from call to call. Returns the input frames now behind the window. */
int VitaRs_Mix(const VitaRs *rs, const int16_t *in, int *phase, int32_t *mix, int out, int gain);

#endif
