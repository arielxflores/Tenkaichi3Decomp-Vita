/*
 * Profile-guided optimisation (build.py BT3_PGO): a build made with BT3_PGO=gen counts how often each branch of the
 * program is taken while it plays, and the counts, fetched from the Vita, make the next build (BT3_PGO=use) lay the
 * code out and inline for the paths the game really takes (the drawing thread's and the game thread's hot loops).
 *
 * VitaSDK's libgcov cannot write files itself; gcc puts a record per object into the section gcov_info
 * (-fprofile-info-section) and this file turns each one into its .gcda with libgcov's __gcov_info_to_gcda, all of
 * them into one file, ux0:data/bt3/pgo/<name>.pgo ("BT3PGO1\n", then per object its .gcda's name and its bytes, each
 * after a 32-bit length): <name> is run-<n> (the first not there yet), or bt3.txt's pgoname= (the test runs fetch it
 * by that name). It is written at exit, before a test sweep starts the program again, and every five minutes
 * (pgodump=<seconds> for another interval); each writing has all the counts of the run so far, so the last one is
 * the run. The counts are plain (not atomic): the threads that share code lose a few, which the use build's
 * -fprofile-correction takes. port/vita/pgo_merge.sh adds up such files (gcov-tool) into port/vita/pgo/<rel|dev>/.
 * Not a gen build: nothing (the calls cost nothing).
 */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void Vita_Log(const char *fmt, ...);

char gVitaPgoName[32]; /* bt3.txt pgoname= (vita_main.c) */
int gVitaPgoEvery = 300; /* bt3.txt pgodump=<seconds> */

#ifdef BT3_PGO_GEN
struct gcov_info;
extern void __gcov_info_to_gcda(const struct gcov_info *info, void (*filename_fn)(const char *, void *),
                                void (*dump_fn)(const void *, unsigned, void *), void *(*allocate_fn)(unsigned, void *), void *arg);
extern const struct gcov_info *const __start_gcov_info[];
extern const struct gcov_info *const __stop_gcov_info[];

#include "vita_home.h"
#define PGO_ROOT VITA_HOME "/pgo"
static char sPath[96];

typedef struct Out {
    char name[96];
    unsigned char *buf;
    unsigned len, cap;
    int failed;
} Out;

static void pgo_name(const char *name, void *arg) {
    Out *o = arg;
    const char *b = strrchr(name, '/');
    snprintf(o->name, sizeof(o->name), "%s", b != NULL ? b + 1 : name);
}

static void pgo_write(const void *data, unsigned n, void *arg) {
    Out *o = arg;
    if (o->len + n > o->cap) {
        unsigned cap = (o->len + n) * 2 + 4096;
        unsigned char *b = realloc(o->buf, cap);
        if (b == NULL) {
            o->failed = 1;
            return;
        }
        o->buf = b;
        o->cap = cap;
    }
    memcpy(o->buf + o->len, data, n);
    o->len += n;
}

static void *pgo_alloc(unsigned n, void *arg) {
    (void)arg;
    return malloc(n); /* (only for value profiles, which these builds do not make) */
}

void Vita_PgoInit(void) {
    SceIoStat st;
    int n;
    sceIoMkdir(PGO_ROOT, 0777);
    if (gVitaPgoName[0] != '\0') {
        snprintf(sPath, sizeof(sPath), PGO_ROOT "/%s.pgo", gVitaPgoName);
    } else {
        for (n = 1; n < 1000; n++) { /* the first run-<n> not there yet */
            snprintf(sPath, sizeof(sPath), PGO_ROOT "/run-%03d.pgo", n);
            if (sceIoGetstat(sPath, &st) < 0) {
                break;
            }
        }
    }
    Vita_Log("vita: profile counts (BT3_PGO=gen) of %d objects go to %s, every %d s\n", (int)(__stop_gcov_info - __start_gcov_info),
             sPath, gVitaPgoEvery);
}

void Vita_PgoDump(const char *why) {
    static volatile int busy;
    const struct gcov_info *const *p;
    Out o;
    unsigned files = 0, bytes = 0, bad = 0;
    char tmp[104];
    SceUID fd;
    if (sPath[0] == '\0' || __atomic_exchange_n(&busy, 1, __ATOMIC_ACQ_REL)) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", sPath); /* (written whole, then put in place: a run cut short keeps the last one) */
    fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        Vita_Log("vita: profile counts not written (%s): %s could not be made (%08x)\n", why, tmp, (unsigned)fd);
        __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
        return;
    }
    bad |= sceIoWrite(fd, "BT3PGO1\n", 8) != 8;
    memset(&o, 0, sizeof(o));
    for (p = __start_gcov_info; p < __stop_gcov_info; p++) {
        uint32_t len[2];
        if (*p == NULL) {
            continue;
        }
        o.name[0] = '\0';
        o.len = 0;
        o.failed = 0;
        __gcov_info_to_gcda(*p, pgo_name, pgo_write, pgo_alloc, &o);
        if (o.failed || o.name[0] == '\0') {
            bad++;
            continue;
        }
        len[0] = (uint32_t)strlen(o.name);
        len[1] = o.len;
        bad += sceIoWrite(fd, &len[0], 4) != 4 || sceIoWrite(fd, o.name, len[0]) != (int)len[0] || sceIoWrite(fd, &len[1], 4) != 4 ||
               sceIoWrite(fd, o.buf, o.len) != (int)o.len;
        files++;
        bytes += o.len;
    }
    free(o.buf);
    sceIoClose(fd);
    sceIoRemove(sPath);
    if (sceIoRename(tmp, sPath) < 0) {
        bad++;
    }
    Vita_Log("vita: profile counts written (%s): %u objects, %u KB, %u failed, in %s\n", why, files, bytes >> 10, bad, sPath);
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

/* (every half second, vita_main.c's watch thread) */
void Vita_PgoTick(void) {
    static unsigned halves;
    if (gVitaPgoEvery > 0 && ++halves % (unsigned)(gVitaPgoEvery * 2) == 0) {
        Vita_PgoDump("timer");
    }
}
#else
void Vita_PgoInit(void) {}
void Vita_PgoDump(const char *why) { (void)why; }
void Vita_PgoTick(void) {}
#endif
