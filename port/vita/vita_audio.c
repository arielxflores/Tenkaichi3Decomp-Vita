/*
 * PS Vita: sound output. The PC port's sound code (snd_adx.c: music and voices from ADX streams; snd_se.c: the
 * game's sound effects) asks SDL3 for "audio streams" that SDL pulls data from through a callback and mixes.
 * This file is that part of SDL on the Vita's audio port: one thread that, every block of samples at 48 kHz, asks
 * each stream for what it needs, brings it to 48 kHz, applies the stream's gain, adds the streams up and hands the
 * block to sceAudioOut. Interface: sdlshim/SDL3/SDL.h.
 *
 * To 48 kHz: a stream at another rate (the music and the voices are 24 kHz) goes through vita_resample.c's 32-point
 * filter, which keeps the treble and leaves no mirror images (the straight line between two samples that stood here
 * before lost 4 dB at 10 kHz and left the images 9 dB under the sound). A stream at 48 kHz (the sound effects' mix) is
 * added as it is. A rate the filter has no table for is still put on straight lines.
 *
 * The block is 512 samples (10.7 ms), not 1024 as before: a sound effect is heard a block sooner on average (it
 * starts with the block being mixed when the game asks for it, and a block waits behind the one that plays). Should
 * the thread come too late for that (the port found empty when the next block is ready, four times within ten
 * seconds), it goes back to 1024 for the rest of the run and the log says so.
 *
 * One lock for everything (the streams are few and a block takes well under a millisecond to mix); it can be
 * taken again by the thread that holds it, as the callers do.
 */
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "vita_resample.h"

#define OUT_RATE 48000
#define GRAIN_MAX 1024
#define GRAIN_SHORT 512
#define STREAMS 24
#define FIFO 8192 /* frames (stereo) a stream can hold */
#define LIN_FRAMES (GRAIN_MAX * 2 + VITA_RS_TAPS) /* input frames a block can take (a rate of up to 96 kHz) */

struct SDL_AudioStream {
    int used, bound, freq, channels;
    float gain;
    SDL_AudioStreamCallback cb;
    void *user;
    int16_t fifo[FIFO * 2];
    unsigned head;   /* frames */
    int count;
    uint32_t frac;   /* (straight lines) position between fifo[head] and the next frame, 16.16 */
    const VitaRs *rs; /* the filter's table for the stream's rate, or NULL (48 kHz, or straight lines) */
    int phase;       /* (the filter) position between two input frames, in the table's steps */
};

static struct SDL_AudioStream sStreams[STREAMS];
static SceUID sMutex = -1, sThread = -1;
static volatile int sOwner = -1, sDepth;
static int sPort = -1;
unsigned gVitaAudioBlocks, gVitaAudioStarved;
extern void Vita_Log(const char *fmt, ...);

static void lock(void) {
    int me = sceKernelGetThreadId();
    if (sOwner == me) {
        sDepth++;
        return;
    }
    sceKernelLockMutex(sMutex, 1, NULL);
    sOwner = me;
    sDepth = 1;
}

static void unlock(void) {
    if (--sDepth == 0) {
        sOwner = -1;
        sceKernelUnlockMutex(sMutex, 1);
    }
}

/* The frames the filter reads before a stream's first one: silence, so that the first sample is heard at its moment. */
static void preroll(struct SDL_AudioStream *s) {
    s->head = 0;
    s->count = 0;
    s->frac = 0;
    s->phase = 0;
    if (s->rs != NULL) {
        memset(s->fifo, 0, sizeof(int16_t) * 2 * VITA_RS_PAST);
        s->count = VITA_RS_PAST;
    }
}

static int sGrain = GRAIN_SHORT;
unsigned gVitaAudioLate; /* (for the log) blocks that found the port empty */

static int audio_thread(SceSize args, void *argp) {
    static int16_t out[GRAIN_MAX * 2];
    static int32_t mix[GRAIN_MAX * 2];
    static int16_t lin[LIN_FRAMES * 2];
    unsigned blocks = 0, windowStart = 0, windowLate = 0;
    int i, k;

    (void)args; (void)argp;
    for (;;) {
        const int grain = sGrain;
        memset(mix, 0, sizeof(int32_t) * 2 * (size_t)grain);
        lock();
        for (k = 0; k < STREAMS; k++) {
            struct SDL_AudioStream *s = &sStreams[k];
            int need, g;

            if (!s->used || !s->bound || s->freq <= 0) {
                continue;
            }
            g = (int)(s->gain * 256.0f);
            if (s->rs != NULL && (need = VitaRs_Need(s->rs, s->phase, grain)) <= LIN_FRAMES) {
                /* through the filter: the frames it reads laid out in a row (the store goes round), silence
                   behind the last one when the stream has no more (its end, or data that is late) */
                int have, first, used;
                if (s->count < need && s->cb != NULL) {
                    s->cb(s->user, s, (need - s->count) * 2 * s->channels, need * 2 * s->channels);
                }
                if (s->count <= VITA_RS_PAST) {
                    continue; /* (nothing but the silence in front) */
                }
                have = s->count < need ? s->count : need;
                first = FIFO - (int)(s->head & (FIFO - 1));
                if (first > have) {
                    first = have;
                }
                memcpy(lin, &s->fifo[(s->head & (FIFO - 1)) * 2], sizeof(int16_t) * 2 * (size_t)first);
                memcpy(lin + first * 2, s->fifo, sizeof(int16_t) * 2 * (size_t)(have - first));
                if (have < need) {
                    memset(lin + have * 2, 0, sizeof(int16_t) * 2 * (size_t)(need - have));
                    gVitaAudioStarved++;
                }
                used = VitaRs_Mix(s->rs, lin, &s->phase, mix, grain, g);
                if (used >= s->count) {
                    preroll(s); /* (played out: the next data starts afresh) */
                } else {
                    s->head += (unsigned)used;
                    s->count -= used;
                }
            } else if (s->freq == OUT_RATE) { /* as it is */
                int n;
                if (s->count < grain && s->cb != NULL) {
                    s->cb(s->user, s, (grain - s->count) * 2 * s->channels, grain * 2 * s->channels);
                }
                n = s->count < grain ? s->count : grain;
                for (i = 0; i < n; i++) {
                    const int16_t *a = &s->fifo[((s->head + (unsigned)i) & (FIFO - 1)) * 2];
                    mix[i * 2] += (a[0] * g) >> 8;
                    mix[i * 2 + 1] += (a[1] * g) >> 8;
                }
                s->head += (unsigned)n;
                s->count -= n;
                if (n < grain && n > 0) {
                    gVitaAudioStarved++;
                }
            } else { /* (a rate without a table) on the straight line between two frames */
                uint32_t step = (uint32_t)(((uint64_t)s->freq << 16) / OUT_RATE);
                need = (int)(((uint64_t)step * (unsigned)grain + s->frac) >> 16) + 2;
                if (s->count < need && s->cb != NULL) {
                    s->cb(s->user, s, (need - s->count) * 2 * s->channels, need * 2 * s->channels);
                }
                if (s->count < 2) {
                    continue;
                }
                for (i = 0; i < grain; i++) {
                    const int16_t *a, *b;
                    int f = (int)(s->frac >> 8) & 0xFF;
                    if (s->count < 2) {
                        gVitaAudioStarved++;
                        break;
                    }
                    a = &s->fifo[(s->head & (FIFO - 1)) * 2];
                    b = &s->fifo[((s->head + 1) & (FIFO - 1)) * 2];
                    mix[i * 2] += ((a[0] * (256 - f) + b[0] * f) >> 8) * g >> 8;
                    mix[i * 2 + 1] += ((a[1] * (256 - f) + b[1] * f) >> 8) * g >> 8;
                    s->frac += step;
                    s->head += s->frac >> 16;
                    s->count -= (int)(s->frac >> 16);
                    s->frac &= 0xFFFF;
                }
                if (s->count < 0) {
                    s->count = 0;
                }
            }
        }
        unlock();
        for (i = 0; i < grain * 2; i++) {
            int32_t v = mix[i];
            out[i] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
        gVitaAudioBlocks++;
        blocks++;
        if (grain == GRAIN_SHORT && blocks > 200) {
            /* the short block: is the thread in time for it? The port still has samples of the block before when
               this one is ready; none left means it ran empty meanwhile (heard as a click). Four of those within
               ten seconds: the long block from here on. */
            if (sceAudioOutGetRestSample(sPort) == 0) {
                gVitaAudioLate++;
                windowLate++;
            }
            if (blocks - windowStart >= 10u * OUT_RATE / GRAIN_SHORT) {
                windowStart = blocks;
                windowLate = 0;
            } else if (windowLate >= 4) {
                if (sceAudioOutSetConfig(sPort, GRAIN_MAX, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO) >= 0) {
                    sGrain = GRAIN_MAX;
                    Vita_Log("vita: sound: the port ran empty %u times within ten seconds with blocks of %d samples: blocks of %d from here on\n", windowLate,
                             GRAIN_SHORT, GRAIN_MAX);
                }
                windowLate = 0;
                windowStart = blocks;
            }
        }
        sceAudioOutOutput(sPort, out); /* returns when the block has been taken: this is the thread's clock */
    }
    return 0;
}

bool SDL_InitSubSystem(uint32_t flags) { (void)flags; return true; }
const char *SDL_GetCurrentAudioDriver(void) { return "PS Vita audio port, 48 kHz"; }
const char *SDL_GetError(void) { return "PS Vita audio"; }
bool SDL_ResumeAudioDevice(SDL_AudioDeviceID dev) { (void)dev; return true; }

SDL_AudioDeviceID SDL_OpenAudioDevice(SDL_AudioDeviceID id, const SDL_AudioSpec *spec) {
    (void)id; (void)spec;
    if (sThread >= 0) {
        return 1;
    }
    sMutex = sceKernelCreateMutex("bt3_audio", 0, 0, NULL);
    sPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, sGrain, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (sMutex < 0 || sPort < 0) {
        Vita_Log("vita: sound: no audio port (mutex %08x, port %08x)\n", (unsigned)sMutex, (unsigned)sPort);
        return 0;
    }
    {   /* on the fourth core when the system lets it (vita_main.c), else the first */
        extern SceUID Vita_ThreadOnCore3(const char *name, SceKernelThreadEntry entry, int prio, int stack, int fallbackMask);
        (void)Vita_ThreadOnCore3; /* (back on the first core; on the fourth it stopped with the system's shell) */
        sThread = sceKernelCreateThread("bt3_audio", audio_thread, 100, 0x40000, 0, 0x00010000, NULL);
    }
    /* above the game's thread (160): a late block is heard.
        On the first core, the game's: the drawing thread's core is the one that is short of time now (test
        135: the game's thread takes 14-24 ms of its 33, the sound about 1 ms a picture) */
    { extern void Vita_WatchThread(int slot, int id, const char *name); Vita_WatchThread(3, sThread, "sound"); }
    if (sThread < 0 || sceKernelStartThread(sThread, 0, NULL) < 0) {
        Vita_Log("vita: sound: no audio thread (%08x)\n", (unsigned)sThread);
        sThread = -1;
        return 0;
    }
    Vita_Log("vita: sound: audio port open, 48 kHz stereo, blocks of %d samples\n", sGrain);
    return 1;
}

SDL_AudioStream *SDL_CreateAudioStream(const SDL_AudioSpec *src, const SDL_AudioSpec *dst) {
    int k;

    (void)dst;
    if (sMutex < 0 || src == NULL || src->channels < 1 || src->channels > 2) {
        return NULL;
    }
    lock();
    for (k = 0; k < STREAMS && sStreams[k].used; k++) {
    }
    if (k < STREAMS) {
        memset(&sStreams[k], 0, sizeof(sStreams[k]));
        sStreams[k].freq = src->freq;
        sStreams[k].channels = src->channels;
        sStreams[k].gain = 1.0f;
        sStreams[k].rs = VitaRs_Get(src->freq);
        preroll(&sStreams[k]);
        sStreams[k].used = 1;
    }
    unlock();
    return k < STREAMS ? &sStreams[k] : NULL;
}

bool SDL_SetAudioStreamGetCallback(SDL_AudioStream *s, SDL_AudioStreamCallback cb, void *userdata) {
    lock();
    s->user = userdata;
    s->cb = cb;
    unlock();
    return true;
}

bool SDL_BindAudioStream(SDL_AudioDeviceID dev, SDL_AudioStream *s) {
    (void)dev;
    s->bound = 1;
    return true;
}

/* (from a stream's callback, or under SDL_LockAudioStream) */
bool SDL_PutAudioStreamData(SDL_AudioStream *s, const void *buf, int len) {
    const int16_t *p = buf;
    int frames = len / (2 * s->channels), i;

    for (i = 0; i < frames && s->count < FIFO; i++) {
        int16_t *o = &s->fifo[((s->head + (unsigned)s->count) & (FIFO - 1)) * 2];
        if (s->channels == 2) {
            o[0] = p[i * 2];
            o[1] = p[i * 2 + 1];
        } else {
            o[0] = o[1] = p[i];
        }
        s->count++;
    }
    return true;
}

bool SDL_SetAudioStreamGain(SDL_AudioStream *s, float gain) { s->gain = gain; return true; }
bool SDL_LockAudioStream(SDL_AudioStream *s) { (void)s; lock(); return true; }
bool SDL_UnlockAudioStream(SDL_AudioStream *s) { (void)s; unlock(); return true; }

bool SDL_ClearAudioStream(SDL_AudioStream *s) {
    lock();
    preroll(s);
    unlock();
    return true;
}

void SDL_DestroyAudioStream(SDL_AudioStream *s) {
    lock();
    s->used = 0;
    s->bound = 0;
    s->cb = NULL;
    unlock();
}
