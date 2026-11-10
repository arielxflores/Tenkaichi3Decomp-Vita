/*
 * CRI ADX streams (music, voices, the movies' sound) on PC: the part of CRI's ADXT library the game calls, with the
 * decoding done here and the output through SDL.
 *
 * The game owns a few "players" (ADXT_Create), starts a file on one (ADXT_StartAfs: a file of an archive;
 * ADXT_StartFname: a loose file next to the movies), and controls it with pause, volume and pan. Every ADX file of
 * the game is the plain kind: type 3, 4-bit ADPCM in 18-byte frames, version 4, not encrypted, mono or stereo at
 * 16 / 24 / 48 kHz (measured over all 65,201 files of the third archive and the two movie tracks).
 *
 * Format (big-endian): 0x8000, u16 header size - 4, u8 type, u8 frame size, u8 bits, u8 channels, u32 sample rate,
 * u32 samples, u16 high-pass frequency, u8 version, u8 flags. Version 4 with a loop: flag at 0x24, loop start
 * sample at 0x28, loop end sample at 0x30. A frame is a u16 scale and 32 signed nibbles; frames of the channels
 * alternate. sample = nibble * (scale + 1) + ((c1 * previous + c2 * the one before) >> 12), clamped to 16 bits,
 * with c1, c2 from the high-pass frequency (the usual ADX formula, in adx_coefs).
 *
 * Each player has its own SDL audio stream (SDL converts the rate and mixes the streams); a stream asks for data
 * through a callback on SDL's audio thread, so the player's state is only touched under the stream's lock.
 * Without a window there is no sound and every player reports "stopped", as before this file existed: headless
 * runs (replay validation) must not depend on a sound device.
 */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gs_internal.h"
#ifdef PORT_VITA
#include <psp2/io/fcntl.h>
#include "vita_read.h"
#endif

enum { ADXT_STAT_STOP = 0, ADXT_STAT_PLAYING = 3, ADXT_STAT_PLAYEND = 5 };
#define MAX_PLAYERS 16
#define FRAME 18
#define PAN_AUTO (-128)

typedef struct Player {
    SDL_AudioStream *stream;
    uint8_t *file;          /* the whole ADX file */
    uint32_t size, data;    /* file size; offset of the first frame */
    uint32_t channels, rate, total;
    int loop;
    uint32_t loopStart, loopEnd; /* samples */
    uint32_t pos;           /* next sample */
    int c1, c2;
    int hist[2][2];
    int stat, paused;
    int vol;                /* 0.1 dB, 0 = full, -960 and below = silent */
    int pan[2];             /* per input channel: -15 left .. 15 right, PAN_AUTO */
#ifdef PORT_VITA
    /* the file is read piece by piece on the reading thread (vita_read.c) while it plays: `loaded` bytes of it are
       there; the decoder waits (silence) at a piece not read yet */
    VitaReadJob job;
    volatile uint32_t loaded;
    int own, waiting;
#endif
} Player;

static Player sPlayers[MAX_PLAYERS];
#ifdef PORT_VITA /* the streams' starts and the voices' changes in the log (the fighters' intro lines) */
extern void Vita_Log(const char *fmt, ...);
static unsigned sAdxLogN;
#define ADX_LOG(...) do { if (sAdxLogN < 400) { sAdxLogN++; Vita_Log(__VA_ARGS__); } } while (0)
#else
#define ADX_LOG(...) ((void)0)
#endif
static int sCount;
static SDL_AudioDeviceID sDevice;
static int sMono, sTried;
static unsigned sFeedCalls; /* callbacks from SDL so far (BT3_SND_VERBOSE prints it at a stop) */

extern int Port_FilePath(int ptid, int flid, const char *fname, char *out, int size); /* plat_file.c */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

static int device(void) {
    /* not before the renderer has decided whether there is a window (its first frame) */
    if (!sTried && (GsGpu_Enabled() || getenv("BT3_SOUND") != NULL || gGsFrame > 2)) {
        sTried = 1;
        /* BT3_SOUND=1: sound without a window too (tests, with SDL_AUDIO_DRIVER=dummy for silence) */
        if ((GsGpu_Enabled() || getenv("BT3_SOUND") != NULL) && getenv("BT3_NOSOUND") == NULL && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            sDevice = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
            if (sDevice == 0) {
                fprintf(stderr, "bt3: no sound device: %s\n", SDL_GetError());
            }
            else {
                SDL_ResumeAudioDevice(sDevice);
                fprintf(stderr, "bt3: sound: %s\n", SDL_GetCurrentAudioDriver());
            }
        }
    }
    return sDevice != 0;
}

/* What the port's other sound code mixes into: the opened device, or 0. */
SDL_AudioDeviceID Port_AudioDevice(void) {
    return device() ? sDevice : 0;
}

static void adx_coefs(uint32_t highpass, uint32_t rate, int *c1, int *c2) {
    double z = cos(2.0 * 3.14159265358979323846 * (double)highpass / (double)rate);
    double a = 1.41421356237309504880 - z, b = 1.41421356237309504880 - 1.0;
    double c = (a - sqrt((a + b) * (a - b))) / b;

    /* 12 fractional bits, truncated toward zero (as vgmstream does; ffmpeg rounds instead, which changes a
       coefficient by at most one unit: its output differed from the floor variant by up to 91 of 32768 over three
       seconds of music, and by nothing on a short clip). Which rounding CRI's own decoder uses was not checked. */
    *c1 = (int)(c * 8192.0);
    *c2 = (int)(c * c * -4096.0);
}

/* Decodes up to `frames` 32-sample frames at p->pos (a multiple of 32) into interleaved stereo; returns samples. */
static int decode(Player *p, int16_t *out, int frames) {
    int n = 0, f, ch, i;
    float gl[2], gr[2];

    for (ch = 0; ch < 2; ch++) { /* pan per input channel */
        int pan = p->pan[ch] != PAN_AUTO ? p->pan[ch] : p->channels == 1 ? 0 : ch == 0 ? -15 : 15;
        if (sMono) { pan = 0; }
        gl[ch] = pan <= 0 ? 1.0f : (float)(15 - pan) / 15.0f;
        gr[ch] = pan >= 0 ? 1.0f : (float)(15 + pan) / 15.0f;
    }
    for (f = 0; f < frames; f++) {
        uint32_t end = p->loop ? p->loopEnd : p->total, index = p->pos / 32;
        int16_t pcm[2][32];
        const uint8_t *src = p->file + p->data + (size_t)index * FRAME * p->channels;
        int count;

        if (p->pos >= end) {
            if (!p->loop) {
                break;
            }
            p->pos = p->loopStart & ~31u; /* loop starts are frame-aligned by the encoder */
            continue;
        }
#ifdef PORT_VITA
        if (src + FRAME * p->channels > p->file + __atomic_load_n(&p->loaded, __ATOMIC_ACQUIRE) &&
            src + FRAME * p->channels <= p->file + p->size) {
            p->waiting = 1; /* (not read yet) */
            break;
        }
#endif
        if (src + FRAME * p->channels > p->file + p->size) {
            p->loop = 0;
            p->total = p->pos;
            break;
        }
        for (ch = 0; ch < (int)p->channels; ch++, src += FRAME) {
            int scale = (int)be16(src) + 1, h1 = p->hist[ch][0], h2 = p->hist[ch][1];
            for (i = 0; i < 32; i++) {
                int nib = (src[2 + i / 2] >> ((i & 1) ? 0 : 4)) & 15, s;
                if (nib & 8) { nib -= 16; }
                s = nib * scale + ((p->c1 * h1 + p->c2 * h2) >> 12);
                if (s > 32767) { s = 32767; }
                if (s < -32768) { s = -32768; }
                pcm[ch][i] = (int16_t)s;
                h2 = h1;
                h1 = s;
            }
            p->hist[ch][0] = h1;
            p->hist[ch][1] = h2;
        }
        count = (int)(end - p->pos < 32 ? end - p->pos : 32);
        for (i = (int)(p->pos & 31); i < count; i++) {
            float l, r;
            if (p->channels == 1) {
                l = (float)pcm[0][i] * gl[0];
                r = (float)pcm[0][i] * gr[0];
            } else {
                l = (float)pcm[0][i] * gl[0] + (float)pcm[1][i] * gl[1];
                r = (float)pcm[0][i] * gr[0] + (float)pcm[1][i] * gr[1];
            }
            out[n * 2] = (int16_t)(l > 32767.0f ? 32767.0f : l < -32768.0f ? -32768.0f : l);
            out[n * 2 + 1] = (int16_t)(r > 32767.0f ? 32767.0f : r < -32768.0f ? -32768.0f : r);
            n++;
        }
        p->pos = (p->pos & ~31u) + 32;
    }
    return n;
}

/* SDL's audio thread wants `additional` more bytes from this player (the stream is locked while this runs). */
static void SDLCALL feed(void *user, SDL_AudioStream *stream, int additional, int total) {
    Player *p = user;
    int16_t buf[32 * 2 * 16];

    sFeedCalls++;

    (void)total;
    while (additional > 0 && p->stat == ADXT_STAT_PLAYING && !p->paused) {
        int n;
#ifdef PORT_VITA
        p->waiting = 0;
#endif
        n = decode(p, buf, 16);
        if (n == 0) {
#ifdef PORT_VITA
            if (p->waiting) {
                break; /* (the next piece is still being read: silence this time) */
            }
#endif
            p->stat = ADXT_STAT_PLAYEND;
            break;
        }
        SDL_PutAudioStreamData(stream, buf, n * 4);
        additional -= n * 4;
    }
}

int gPortMusicPercent = 100, gPortSePercent = 100; /* the settings overlay's volumes */

static void apply_volume(Player *p) {
    if (p->stream != NULL) {
        SDL_SetAudioStreamGain(p->stream, p->vol <= -960 ? 0.0f : (float)pow(10.0, (double)p->vol / 200.0) * (float)gPortMusicPercent / 100.0f);
    }
}

static void stop(Player *p) {
    if (p->stream != NULL && getenv("BT3_SND_VERBOSE") != NULL) {
        fprintf(stderr, "adx: stop player %d at %.1f s, status %d, volume %d; %u callbacks so far\n", (int)(p - sPlayers), (double)p->pos / (double)p->rate, p->stat, p->vol, sFeedCalls);
    }
    if (p->stream != NULL && p - sPlayers >= 4) {
        ADX_LOG("adx: player %d stopped at %.1f s of %.1f, status %d (frame %u)\n", (int)(p - sPlayers), (double)p->pos / (double)p->rate, (double)p->total / (double)p->rate, p->stat, gGsFrame);
    }
    if (p->stream != NULL) {
        SDL_DestroyAudioStream(p->stream); /* waits for a running callback */
        p->stream = NULL;
    }
#ifdef PORT_VITA
    Vita_ReadCancel(&p->job); /* (the reading thread done with it) */
    if (p->own) {
        sceIoClose(p->job.fd);
        p->own = 0;
    }
#endif
    free(p->file);
    p->file = NULL;
    p->stat = ADXT_STAT_STOP;
}

#ifdef PORT_VITA
#define ADX_PIECE 0x10000 /* 64 KB: about a second of a stereo song at 48 kHz */
/* (on the reading thread) a piece is in: the next one, until the file's end; a failed read ends the song there */
static int adx_piece(VitaReadJob *j, int got) {
    Player *p = j->user;
    uint32_t at = (uint32_t)((uint8_t *)j->buf - p->file);
    if (got != (int)j->len) {
        p->size = at; /* (the decoder ends where the file stops) */
        return 0;
    }
    __atomic_store_n(&p->loaded, at + (uint32_t)got, __ATOMIC_RELEASE);
    at += (uint32_t)got;
    if (at >= p->size) {
        return 0;
    }
    j->off += got;
    j->buf = p->file + at;
    j->len = p->size - at < ADX_PIECE ? p->size - at : ADX_PIECE;
    return 1;
}
#endif

static void start(Player *p, const char *path) {
    SDL_AudioSpec spec;
    FILE *fp;
    long size;
    uint32_t header, highpass;

    stop(p);
    if (!device()) {
        return;
    }
#ifdef PORT_VITA /* the archives are read as they are on the disc: no file of its own to open (vita_file.c) */
    {   /* the first piece now (the header and about a second of sound), the rest on the reading thread */
        SceUID fd;
        uint32_t base, first;
        int own;
        (void)fp; (void)size;
        p->size = 0;
        p->file = NULL;
        if (Vita_FileSpan(path, &fd, &base, &p->size, &own)) {
            first = p->size < ADX_PIECE ? p->size : ADX_PIECE;
            p->file = malloc(p->size ? p->size : 1);
            if (p->file != NULL && sceIoPread(fd, p->file, first, base) != (int)first) {
                free(p->file);
                p->file = NULL;
            }
        }
        if (p->file == NULL) {
            if (p->size != 0 && own) {
                sceIoClose(fd);
            }
            fprintf(stderr, "bt3: sound file not found: %s\n", path);
            ADX_LOG("adx: player %d: %s not read\n", (int)(p - sPlayers), path);
            return;
        }
        p->loaded = first;
        memset(&p->job, 0, sizeof(p->job));
        p->job.fd = fd;
        p->own = own;
        if (first < p->size) {
            p->job.off = (SceOff)base + first;
            p->job.buf = p->file + first;
            p->job.len = p->size - first < ADX_PIECE ? p->size - first : ADX_PIECE;
            p->job.done = adx_piece;
            p->job.user = p;
            if (!Vita_ReadStart(&p->job)) { /* (no reading thread: the rest now) */
                uint32_t rest = p->size - first;
                if (sceIoPread(fd, p->file + first, rest, (SceOff)base + first) == (int)rest) {
                    p->loaded = p->size;
                } else {
                    p->size = first;
                }
            }
        }
        if (own && p->job.done == NULL) { /* (all read) */
            sceIoClose(fd);
            p->own = 0;
        }
    }
#else
    fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "bt3: sound file not found: %s\n", path);
        return;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    p->file = malloc((size_t)size);
    p->size = (uint32_t)fread(p->file, 1, (size_t)size, fp);
    fclose(fp);
#endif
    if (p->size < 0x40 || be16(p->file) != 0x8000 || p->file[4] != 3 || p->file[5] != FRAME || p->file[6] != 4 ||
        p->file[7] < 1 || p->file[7] > 2 || (p->file[0x13] & 8)) {
        fprintf(stderr, "bt3: %s is not an ADX file of the kind the game uses\n", path);
        ADX_LOG("adx: player %d: %s is not an ADX file of the kind the game uses (%u bytes: %02x %02x %02x %02x %02x %02x %02x %02x)\n", (int)(p - sPlayers), path, p->size,
                p->size > 0 ? p->file[0] : 0, p->size > 1 ? p->file[1] : 0, p->size > 4 ? p->file[4] : 0, p->size > 5 ? p->file[5] : 0,
                p->size > 6 ? p->file[6] : 0, p->size > 7 ? p->file[7] : 0, p->size > 0x12 ? p->file[0x12] : 0, p->size > 0x13 ? p->file[0x13] : 0);
        free(p->file);
        p->file = NULL;
        return;
    }
    header = be16(p->file + 2) + 4;
    p->data = header;
    p->channels = p->file[7];
    p->rate = be32(p->file + 8);
    p->total = be32(p->file + 12);
    highpass = be16(p->file + 16);
    p->loop = 0;
    if (p->file[0x12] == 4 && header >= 0x38 && be32(p->file + 0x24) != 0) {
        p->loopStart = be32(p->file + 0x28);
        p->loopEnd = be32(p->file + 0x30);
        p->loop = p->loopEnd > p->loopStart && p->loopEnd <= p->total;
    }
    adx_coefs(highpass, p->rate, &p->c1, &p->c2);
    memset(p->hist, 0, sizeof(p->hist));
    p->pos = 0;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = (int)p->rate;
    p->stream = SDL_CreateAudioStream(&spec, NULL);
    if (p->stream == NULL) {
        fprintf(stderr, "bt3: sound stream: %s\n", SDL_GetError());
        ADX_LOG("adx: player %d: no stream for %s\n", (int)(p - sPlayers), path);
        return;
    }
    ADX_LOG("adx: player %d plays %s: %u ch, %u Hz, %.1f s, loop %d, volume %d, paused %d (frame %u)\n", (int)(p - sPlayers), path, p->channels, p->rate,
            (double)p->total / (double)p->rate, p->loop, p->vol, p->paused, gGsFrame);
    if (getenv("BT3_SND_VERBOSE") != NULL) {
        fprintf(stderr, "adx: start %s: %u ch, %u Hz, %.1f s, loop %d, volume %d (0.1 dB), paused %d\n", path, p->channels, p->rate,
                (double)p->total / (double)p->rate, p->loop, p->vol, p->paused);
    }
    p->stat = ADXT_STAT_PLAYING;
    apply_volume(p);
    SDL_SetAudioStreamGetCallback(p->stream, feed, p);
    if (!SDL_BindAudioStream(sDevice, p->stream)) {
        fprintf(stderr, "bt3: sound stream: %s\n", SDL_GetError());
    }
}

/* ------------------------------------------------------------------------------- the library's functions */

void ADXT_Init(void) {}

void *ADXT_Create(int maxnch, void *work, int worksize) {
    Player *p = &sPlayers[sCount < MAX_PLAYERS ? sCount++ : MAX_PLAYERS - 1];

    (void)maxnch; (void)work; (void)worksize;
    memset(p, 0, sizeof(*p));
    p->pan[0] = p->pan[1] = PAN_AUTO;
    return p;
}

void ADXT_SetReloadSct(void *adxt, int nsct) { (void)adxt; (void)nsct; }
void ADXT_SetSvrFreq(void *adxt, int freq) { (void)adxt; (void)freq; }

void ADXT_StartAfs(void *adxt, int patid, int fid) {
    char path[512];

    if (adxt != NULL && Port_FilePath(patid, fid, NULL, path, sizeof(path))) {
        start(adxt, path);
    } else {
        ADX_LOG("adx: player %d: archive %d file %d not found\n", adxt != NULL ? (int)((Player *)adxt - sPlayers) : -1, patid, fid);
    }
}

void ADXT_StartFname(void *adxt, char *fname) {
    char path[512];

    if (adxt != NULL && Port_FilePath(0, 0, fname, path, sizeof(path))) {
        start(adxt, path);
    }
}

void ADXT_Stop(void *adxt) {
    if (adxt != NULL) {
        stop(adxt);
    }
}

void ADXT_Pause(void *adxt, int sw) {
    Player *p = adxt;

    if (p == NULL) {
        return;
    }
    if (getenv("BT3_SND_VERBOSE") != NULL && p->paused != (sw != 0)) {
        fprintf(stderr, "adx: player %d pause %d\n", (int)(p - sPlayers), sw);
    }
    if (p - sPlayers >= 4 && p->paused != (sw != 0)) {
        ADX_LOG("adx: player %d pause %d (frame %u)\n", (int)(p - sPlayers), sw, gGsFrame);
    }
    if (p->stream != NULL) {
        SDL_LockAudioStream(p->stream);
        p->paused = sw != 0;
        if (p->paused) {
            SDL_ClearAudioStream(p->stream); /* what is queued would still be heard */
        }
        SDL_UnlockAudioStream(p->stream);
    } else {
        p->paused = sw != 0;
    }
}

void ADXT_SetOutVol(void *adxt, int vol) {
    Player *p = adxt;

    if (p != NULL) {
        if (getenv("BT3_SND_VERBOSE") != NULL && p->vol != vol) {
            fprintf(stderr, "adx: player %d volume %d\n", (int)(p - sPlayers), vol);
        }
        if (p - sPlayers >= 4 && p->vol != vol) {
            ADX_LOG("adx: player %d volume %d (frame %u)\n", (int)(p - sPlayers), vol, gGsFrame);
        }
        p->vol = vol;
        apply_volume(p);
    }
}

int ADXT_GetOutVol(void *adxt) {
    return adxt != NULL ? ((Player *)adxt)->vol : 0;
}

void ADXT_SetOutPan(void *adxt, int chan, int pan) {
    Player *p = adxt;

    if (p != NULL && chan >= 0 && chan < 2) {
        if (p->stream != NULL) { SDL_LockAudioStream(p->stream); }
        p->pan[chan] = pan;
        if (p->stream != NULL) { SDL_UnlockAudioStream(p->stream); }
    }
}

void ADXT_SetOutputMono(int flag) {
    sMono = flag != 0;
}

int ADXT_GetStat(void *adxt) {
    return adxt != NULL ? ((Player *)adxt)->stat : ADXT_STAT_STOP;
}

/* A CRI call the movie player makes (0x277A70 in the PS2 executable, not named): asked with the movie's sound
   player before it starts a disc read ahead, which it only does on 0. Taken as "is the stream reading from the
   disc right now"; here a stream never is (the whole file is read when it starts). */
int func_00277A70(void *adxt) {
    (void)adxt;
    return 0;
}

/* The music volume setting changed: the streams playing now take it over. */
void Port_AudioRefresh(void) {
    int i;
    for (i = 0; i < MAX_PLAYERS; i++) {
        apply_volume(&sPlayers[i]);
    }
}
