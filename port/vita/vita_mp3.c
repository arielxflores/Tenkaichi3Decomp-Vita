/*
 * PS Vita: MP3 files in the songs folder (ux0:data/bt3/songs, vita_extras.c), made into the ADX the game plays.
 *
 * After z3xox's PC version (port/src/gs/song_mp3.c there, before it went over to playing MP3 files as they are):
 * a `name.mp3` is decoded (dr_mp3, port/third_party/dr_libs) and written as `name.adx` beside it, once, on a thread
 * of its own at the lowest priority, so the game does not wait for it: a song made this way joins the music lists
 * the next time a select screen is set up (vita_extras.c, PortSongs_Refresh). On the Vita this takes some seconds
 * a song; a song of the first start after it was put there may not be offered in the very first select screen.
 *
 * Which MP3 is made into an ADX: the ADX written here carries a mark in its header with the MP3's size and time of
 * last change. No `name.adx`: it is made. One with the mark and the same size and time: nothing to do. With the
 * mark and another size or time (the MP3 was replaced): made again. Without the mark (made by hand, with ffmpeg or
 * the PC version's add_song.py): never written over, and the MP3 is left alone. An MP3 that cannot be decoded is
 * written down in mp3-failed.txt in the folder (its size and time) and not tried again until it changes.
 *
 * The ADX: the kind snd_adx.c plays (type 3, 18-byte frames, 4 bits, version 3, no loop points: an added song is
 * played round from its start), one or two channels. At the MP3's rate up to 32 kHz; a higher rate (44.1 or 48 kHz,
 * most MP3 files) is brought down to 32 kHz, through a low-pass filter so nothing above the new limit folds back.
 * The player holds a song whole in memory (8.6 MB for four minutes at 32 kHz in stereo, 11.9 MB at 44.1 kHz); the
 * disc's own music is at 24 kHz.
 */
#define DR_MP3_IMPLEMENTATION
#include "../third_party/dr_libs/dr_mp3.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void Vita_Log(const char *fmt, ...);

#define FRAME 18
#define HEADER 0x60     /* the header's bytes, "(c)CRI" last; the data follows */
#define MARK_AT 0x30    /* the mark, then the MP3's size and time, 8 bytes each */
#define HIGHPASS 500    /* the high-pass frequency of the disc's music and of ffmpeg's encoder */
#define NAME_MAX_ 128
#define MP3_MAX 128
#define RATE_MAX 32000  /* (see the top) */
#define TAPS 24         /* the low-pass filter's length, on each side of the sample */

static volatile int sMade; /* ADX files written by this run of the program */
static const char kMark[8] = {'B', 'T', '3', ':', 'M', 'P', '3', 1};

static void put16(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, v >> 16);
    put16(p + 2, v & 0xFFFF);
}

/* The prediction coefficients, as snd_adx.c's adx_coefs computes them (the same truncation). */
static void coefs(uint32_t rate, int *c1, int *c2) {
    double z = cos(2.0 * 3.14159265358979323846 * (double)HIGHPASS / (double)rate);
    double a = 1.41421356237309504880 - z, b = 1.41421356237309504880 - 1.0;
    double c = (a - sqrt((a + b) * (a - b))) / b;

    *c1 = (int)(c * 8192.0);
    *c2 = (int)(c * c * -4096.0);
}

/* One frame of one channel: 32 samples (`in`, `step` apart) into 18 bytes. h[0], h[1] are the decoder's last two
   samples, updated. Each frame's prediction is made from the samples the decoder will have, so the error does not
   build up through the filter. */
static void encode_frame(const int16_t *in, int step, int c1, int c2, int h[2], uint8_t *out) {
    int i, peak = 0, scale, h1 = h[0], h2 = h[1];

    for (i = 0; i < 32; i++) {
        int s = in[i * step], d = s - ((c1 * h1 + c2 * h2) >> 12);
        if (d < 0) { d = -d; }
        if (d > peak) { peak = d; }
        h2 = h1;
        h1 = s;
    }
    scale = (peak + 6) / 7;
    if (scale < 1) { scale = 1; }
    if (scale > 0x10000) { scale = 0x10000; }
    put16(out, (unsigned)(scale - 1));
    memset(out + 2, 0, 16);
    h1 = h[0];
    h2 = h[1];
    for (i = 0; i < 32; i++) {
        int pred = (c1 * h1 + c2 * h2) >> 12, d = in[i * step] - pred, n, s;
        n = d >= 0 ? (d + scale / 2) / scale : -((-d + scale / 2) / scale);
        if (n > 7) { n = 7; }
        if (n < -8) { n = -8; }
        s = n * scale + pred;
        if (s > 32767) { s = 32767; }
        if (s < -32768) { s = -32768; }
        out[2 + i / 2] |= (uint8_t)((n & 15) << ((i & 1) ? 0 : 4));
        h2 = h1;
        h1 = s;
    }
    h[0] = h1;
    h[1] = h2;
}

/* What tells one state of a file from another: its size and its time of last change. 0: no such file. */
static int file_id(const char *path, uint64_t *size, uint64_t *mtime) {
    SceIoStat st;
    const SceDateTime *t = &st.st_mtime;
    if (sceIoGetstat(path, &st) < 0 || !SCE_S_ISREG(st.st_mode)) {
        return 0;
    }
    *size = (uint64_t)st.st_size;
    *mtime = (uint64_t)t->year << 40 | (uint64_t)t->month << 32 | (uint64_t)t->day << 24 | (uint64_t)t->hour << 16 |
             (uint64_t)t->minute << 8 | (uint64_t)t->second;
    return 1;
}

enum { ADX_NONE, ADX_CURRENT, ADX_STALE, ADX_FOREIGN };

static int adx_state(const char *adx, uint64_t size, uint64_t mtime) {
    SceIoStat st;
    uint8_t h[HEADER];
    FILE *fp;
    size_t got;
    int k;

    if (sceIoGetstat(adx, &st) < 0) {
        return ADX_NONE;
    }
    fp = fopen(adx, "rb");
    got = fp != NULL ? fread(h, 1, sizeof(h), fp) : 0;
    if (fp != NULL) {
        fclose(fp);
    }
    if (got < sizeof(h) || h[0] != 0x80 || h[1] != 0 || ((unsigned)h[2] << 8 | h[3]) != HEADER - 4 || memcmp(h + MARK_AT, kMark, 8) != 0) {
        return ADX_FOREIGN;
    }
    for (k = 0; k < 8; k++) {
        if (h[MARK_AT + 8 + k] != (uint8_t)(size >> (56 - 8 * k)) || h[MARK_AT + 16 + k] != (uint8_t)(mtime >> (56 - 8 * k))) {
            return ADX_STALE;
        }
    }
    return ADX_CURRENT;
}

static void header(uint8_t *head, unsigned channels, uint32_t rate, uint32_t frames, uint64_t size, uint64_t mtime) {
    memset(head, 0, HEADER);
    memcpy(head + MARK_AT, kMark, 8);
    put32(head + MARK_AT + 8, (uint32_t)(size >> 32));
    put32(head + MARK_AT + 12, (uint32_t)size);
    put32(head + MARK_AT + 16, (uint32_t)(mtime >> 32));
    put32(head + MARK_AT + 20, (uint32_t)mtime);
    put16(head, 0x8000);
    put16(head + 2, HEADER - 4);
    head[4] = 3;
    head[5] = FRAME;
    head[6] = 4;
    head[7] = (uint8_t)channels;
    put32(head + 8, rate);
    put32(head + 12, frames);
    put16(head + 16, HIGHPASS);
    head[18] = 3; /* version 3: no loop points */
    memcpy(head + HEADER - 6, "(c)CRI", 6);
}

/* ---- bringing the rate down ------------------------------------------------------------------------------- */

/* A windowed-sinc low-pass at the new rate's limit, read at fractional places: output k is the input at k * in / out
   through the filter, its weights from a table of PHASES places between two input samples. The input comes in
   pieces; what the next outputs still need of it stays. */
#define PHASES 256
#define CAP 8192
typedef struct Rs {
    double step;       /* input samples per output sample */
    double at;         /* the next output's place, in input samples from buf[0] */
    int channels, have;
    float w[PHASES + 1][2 * TAPS]; /* weights of the samples at c - TAPS + 1 .. c + TAPS for the place c + phase / PHASES */
    float buf[2][CAP];
} Rs;

static void rs_init(Rs *r, int channels, double step) {
    float cut = (float)(0.95 / step); /* (a little under the new limit) */
    int p, k;
    memset(r, 0, sizeof(*r));
    r->channels = channels;
    r->step = step;
    for (p = 0; p <= PHASES; p++) {
        for (k = 0; k < 2 * TAPS; k++) {
            float x = (float)p / PHASES - (float)(k - TAPS + 1), a = x * cut, win;
            win = fabsf(x) >= TAPS ? 0.0f : 0.5f + 0.5f * cosf(3.14159265f * x / TAPS); /* (Hann) */
            r->w[p][k] = (fabsf(a) < 1e-6f ? 1.0f : sinf(3.14159265f * a) / (3.14159265f * a)) * cut * win;
        }
    }
}

/* Feeds `n` interleaved input frames, writes output frames (interleaved) to out, returns how many. */
static int rs_run(Rs *r, const int16_t *in, int n, int16_t *out, int outMax) {
    int i, ch, made = 0;
    for (i = 0; i < n; i++) {
        if (r->have == CAP) { /* (full: the samples no output needs any more go) */
            int base = (int)floor(r->at) - TAPS + 1;
            if (base > 0) {
                for (ch = 0; ch < r->channels; ch++) {
                    memmove(r->buf[ch], r->buf[ch] + base, (size_t)(r->have - base) * sizeof(float));
                }
                r->have -= base;
                r->at -= base;
            } else {
                break; /* (cannot happen: an output is made for every step of input) */
            }
        }
        for (ch = 0; ch < r->channels; ch++) {
            r->buf[ch][r->have] = in[i * r->channels + ch];
        }
        r->have++;
        while (made < outMax && r->at + TAPS < r->have) { /* (the filter's right half is in) */
            int c = (int)floor(r->at), k, k0 = c - TAPS + 1;
            const float *w = r->w[(int)((r->at - c) * PHASES + 0.5)];
            for (ch = 0; ch < r->channels; ch++) {
                const float *b = r->buf[ch];
                float s = 0.0f;
                for (k = k0 < 0 ? -k0 : 0; k < 2 * TAPS; k++) {
                    s += b[k0 + k] * w[k];
                }
                s = s > 32767.0f ? 32767.0f : s < -32768.0f ? -32768.0f : s;
                out[made * r->channels + ch] = (int16_t)lrintf(s);
            }
            made++;
            r->at += r->step;
        }
    }
    return made;
}

/* ---- one file --------------------------------------------------------------------------------------------- */

#define CHUNK (32 * 256) /* sample frames decoded at a time */

static int convert(const char *mp3, const char *adx, uint64_t size, uint64_t mtime, const char **why, int *source) {
    static int16_t pcm[CHUNK * 2], res[CHUNK * 2 + 64], pend[32 * 2];
    static Rs rs; /* (at the bottom of the file's own memory: some 120 KB) */
    drmp3 *dec = malloc(sizeof(*dec));
    char tmp[300];
    uint8_t head[HEADER], buf[FRAME * 2];
    int ch, channels, c1, c2, h[2][2] = {{0, 0}, {0, 0}}, last = 0, npend = 0, down;
    uint64_t frames = 0;
    uint32_t rate;
    FILE *fp;

    *source = 1;
    if (dec == NULL || !drmp3_init_file(dec, mp3, NULL)) {
        *why = "not an MP3 that can be read";
        free(dec);
        return -1;
    }
    channels = (int)dec->channels;
    if (channels < 1 || channels > 2 || dec->sampleRate == 0) {
        *why = channels > 2 ? "more than two channels" : "not an MP3 that can be read";
        drmp3_uninit(dec);
        free(dec);
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s.part", adx); /* (written under another name, given its own when whole) */
    if ((fp = fopen(tmp, "wb")) == NULL) {
        *source = 0;
        *why = "the songs folder cannot be written to";
        drmp3_uninit(dec);
        free(dec);
        return -1;
    }
    rate = dec->sampleRate;
    down = rate > RATE_MAX;
    if (down) {
        rs_init(&rs, channels, (double)rate / RATE_MAX);
        rate = RATE_MAX;
    }
    header(head, (unsigned)channels, rate, 0, size, mtime);
    fwrite(head, 1, sizeof(head), fp);
    coefs(rate, &c1, &c2);
    *why = NULL;
    while (!last && *why == NULL) {
        uint64_t got = 0;
        int n, k;
        const int16_t *src;
        while (got < CHUNK) {
            uint64_t m = drmp3_read_pcm_frames_s16(dec, CHUNK - got, pcm + got * channels);
            if (m == 0) {
                break;
            }
            got += m;
        }
        last = got < CHUNK;
        if (down) {
            n = rs_run(&rs, pcm, (int)got, res, CHUNK + 32);
            src = res;
        } else {
            n = (int)got;
            src = pcm;
        }
        /* whole ADX frames of 32; what is left waits for the next piece (and the end is filled with silence) */
        for (k = 0; k < n; ) {
            int take = 32 - npend < n - k ? 32 - npend : n - k;
            memcpy(pend + npend * channels, src + k * channels, (size_t)take * channels * sizeof(int16_t));
            npend += take;
            k += take;
            if (npend == 32) {
                for (ch = 0; ch < channels; ch++) {
                    encode_frame(pend + ch, channels, c1, c2, h[ch], buf + ch * FRAME);
                }
                fwrite(buf, 1, (size_t)FRAME * channels, fp);
                npend = 0;
                frames += 32;
            }
        }
        if (last && npend > 0) {
            memset(pend + npend * channels, 0, (size_t)(32 - npend) * channels * sizeof(int16_t));
            for (ch = 0; ch < channels; ch++) {
                encode_frame(pend + ch, channels, c1, c2, h[ch], buf + ch * FRAME);
            }
            fwrite(buf, 1, (size_t)FRAME * channels, fp);
            frames += npend;
            npend = 0;
        }
        if (frames > 0xFFFFFFF0u) {
            *why = "too long";
        } else if (ferror(fp)) {
            *why = "the songs folder cannot be written to (full?)";
            *source = 0;
        }
    }
    drmp3_uninit(dec);
    free(dec);
    if (*why == NULL && frames == 0) {
        *why = "not an MP3 that can be read";
    }
    if (*why != NULL) {
        fclose(fp);
        sceIoRemove(tmp);
        return -1;
    }
    header(head, (unsigned)channels, rate, (uint32_t)frames, size, mtime);
    fseek(fp, 0, SEEK_SET);
    fwrite(head, 1, sizeof(head), fp);
    *source = 0;
    if (ferror(fp) | fclose(fp)) {
        *why = "the songs folder cannot be written to (full?)";
        sceIoRemove(tmp);
        return -1;
    }
    sceIoRemove(adx); /* (a stale one of ours: rename does not replace) */
    if (sceIoRename(tmp, adx) < 0) {
        *why = "the songs folder cannot be written to";
        sceIoRemove(tmp);
        return -1;
    }
    return 0;
}

/* ---- the MP3 files that failed ---------------------------------------------------------------------------- */

typedef struct Failed {
    uint64_t size, mtime;
    char name[NAME_MAX_], why[64];
    int seen;
} Failed;

static Failed sFailed[MP3_MAX];
static int sFailedCount;

static void failed_load(const char *dir) {
    char path[300], line[400];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/mp3-failed.txt", dir);
    sFailedCount = 0;
    fp = fopen(path, "rb");
    while (fp != NULL && sFailedCount < MP3_MAX && fgets(line, sizeof(line), fp) != NULL) {
        Failed *f = &sFailed[sFailedCount];
        char *e, *name, *bar = NULL, *q;
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) {
            line[--l] = '\0';
        }
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }
        f->size = strtoull(line, &e, 10);
        if (e == line || *e != ' ') {
            continue;
        }
        name = e + 1;
        f->mtime = strtoull(name, &e, 10);
        if (e == name || *e != ' ') {
            continue;
        }
        name = e + 1;
        for (q = name; (q = strstr(q, " | ")) != NULL; q += 3) {
            bar = q;
        }
        if (bar == NULL || bar == name || (size_t)(bar - name) >= sizeof(f->name)) {
            continue;
        }
        snprintf(f->name, sizeof(f->name), "%.*s", (int)(bar - name), name);
        snprintf(f->why, sizeof(f->why), "%s", bar + 3);
        f->seen = 0;
        sFailedCount++;
    }
    if (fp != NULL) {
        fclose(fp);
    }
}

static void failed_save(const char *dir) {
    char path[300];
    FILE *fp = NULL;
    int i;

    snprintf(path, sizeof(path), "%s/mp3-failed.txt", dir);
    for (i = 0; i < sFailedCount; i++) {
        const Failed *f = &sFailed[i];
        if (!f->seen) {
            continue;
        }
        if (fp == NULL) {
            if ((fp = fopen(path, "wb")) == NULL) {
                return;
            }
            fputs("# MP3 files the game could not make into songs (written by the game). A file is tried again when it\n"
                  "# changes, or when its line here is deleted. <size> <time of last change> <file> | <why>\n", fp);
        }
        fprintf(fp, "%" PRIu64 " %" PRIu64 " %s | %s\n", f->size, f->mtime, f->name, f->why);
    }
    if (fp != NULL) {
        fclose(fp);
    } else {
        sceIoRemove(path);
    }
}

static Failed *failed_find(const char *name) {
    int i;
    for (i = 0; i < sFailedCount; i++) {
        if (strcmp(sFailed[i].name, name) == 0) {
            return &sFailed[i];
        }
    }
    return NULL;
}

/* ---- the folder ------------------------------------------------------------------------------------------- */

static int ends_with(const char *s, const char *ext) {
    size_t l = strlen(s), e = strlen(ext), k;
    if (l <= e) {
        return 0;
    }
    for (k = 0; k < e && tolower((unsigned char)s[l - e + k]) == ext[k]; k++) {
    }
    return k == e;
}

static int name_cmp(const void *a, const void *b) {
    const unsigned char *x = a, *y = b;
    for (; *x != '\0' && tolower(*x) == tolower(*y); x++, y++) {
    }
    return tolower(*x) != tolower(*y) ? tolower(*x) - tolower(*y) : strcmp(a, b);
}

static char sDir[256];

static void convert_folder(const char *dir) {
    static char names[MP3_MAX][NAME_MAX_];
    SceUID d = sceIoDopen(dir);
    SceIoDirent e;
    int n = 0, k, dirty = 0;

    if (d < 0) {
        return;
    }
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        if (ends_with(e.d_name, ".adx.part")) { /* (left by a run that was closed while it made one) */
            char path[300];
            snprintf(path, sizeof(path), "%s/%s", dir, e.d_name);
            sceIoRemove(path);
            continue;
        }
        if (!ends_with(e.d_name, ".mp3") || !SCE_S_ISREG(e.d_stat.st_mode)) {
            continue;
        }
        if (strlen(e.d_name) >= NAME_MAX_ - 1) {
            Vita_Log("songs: %s left out: the name is too long\n", e.d_name);
        } else if (n < MP3_MAX) {
            snprintf(names[n++], NAME_MAX_, "%s", e.d_name);
        }
    }
    sceIoDclose(d);
    if (n == 0) {
        return;
    }
    qsort(names, (size_t)n, NAME_MAX_, name_cmp);
    failed_load(dir);
    for (k = 0; k < n; k++) {
        const char *name = names[k], *why = "";
        char mp3[300], adx[300];
        uint64_t size, mtime;
        int source = 0, state;
        unsigned long long t0;
        Failed *f;

        snprintf(mp3, sizeof(mp3), "%s/%s", dir, name);
        snprintf(adx, sizeof(adx), "%s/%.*s.adx", dir, (int)(strlen(name) - 4), name);
        if (!file_id(mp3, &size, &mtime)) {
            continue;
        }
        state = adx_state(adx, size, mtime);
        if (state == ADX_CURRENT) {
            continue;
        }
        if (state == ADX_FOREIGN) {
            Vita_Log("songs: %s is not made into a song: the %.*s.adx beside it was not made from it, and is kept\n", name,
                     (int)(strlen(name) - 4), name);
            continue;
        }
        f = failed_find(name);
        if (f != NULL && f->size == size && f->mtime == mtime) {
            f->seen = 1; /* failed as it is now: not tried again */
            continue;
        }
        if (f != NULL) {
            dirty = 1;
        }
        t0 = sceKernelGetProcessTimeWide();
        if (convert(mp3, adx, size, mtime, &why, &source) == 0) {
            Vita_Log("songs: %s made into a song in %u s\n", name, (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000000));
            __atomic_add_fetch(&sMade, 1, __ATOMIC_RELEASE);
            continue;
        }
        Vita_Log("songs: %s left out: %s\n", name, why);
        if (source) {
            if (f == NULL && sFailedCount < MP3_MAX) {
                f = &sFailed[sFailedCount++];
                snprintf(f->name, sizeof(f->name), "%s", name);
            }
            if (f != NULL) {
                f->size = size;
                f->mtime = mtime;
                snprintf(f->why, sizeof(f->why), "%s", why);
                f->seen = 1;
                dirty = 1;
            }
        }
    }
    for (k = 0; k < sFailedCount; k++) {
        dirty |= !sFailed[k].seen;
    }
    if (dirty) {
        failed_save(dir);
    }
}

static int mp3_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    convert_folder(sDir);
    sceKernelExitDeleteThread(0);
    return 0;
}

/* At start (vita_extras.c): the MP3 files of `dir` that need it, made into songs on a thread of their own. */
void Vita_Mp3Start(const char *dir) {
    SceUID th;
    snprintf(sDir, sizeof(sDir), "%s", dir);
    /* the lowest priority there is, on the second and third cores: it runs when the list reader and the drawing
       thread wait (a large stack: the decoder keeps its frame on it) */
    th = sceKernelCreateThread("bt3_mp3", mp3_thread, 191, 0x40000, 0, 0x00060000, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        Vita_Log("songs: no thread for the MP3 files (0x%08X): they are not made into songs\n", (unsigned)th);
    }
}

/* How many songs were made since the start: the song list is read again when it has grown. */
int Vita_Mp3Made(void) {
    return __atomic_load_n(&sMade, __ATOMIC_ACQUIRE);
}
