/*
 * Profile-guided optimisation (build.py BT3_PGO): a build made with BT3_PGO=gen counts how often each branch of the
 * program is taken while it plays, and the counts, fetched from the Vita, make the next build (BT3_PGO=use) lay the
 * code out and inline for the paths the game really takes (the drawing thread's and the game thread's hot loops).
 *
 * VitaSDK's libgcov cannot write files itself; gcc puts a record per object into the section gcov_info
 * (-fprofile-info-section) and this file writes each one out with libgcov's __gcov_info_to_gcda, as
 * ux0:data/bt3/pgo/run-<n>/<object>.gcda: at exit, before a test sweep starts the program again, and every five
 * minutes (each writing has all the counts of the run so far: the last one of a run is the run). The counts are
 * plain (not atomic): the threads that share code lose a few, which the use build's -fprofile-correction takes.
 * port/vita/pgo_merge.sh adds up the folders of several runs (gcov-tool) into port/vita/pgo/<rel|dev>/.
 * Not a gen build: nothing (the calls cost nothing).
 */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void Vita_Log(const char *fmt, ...);

#ifdef BT3_PGO_GEN
struct gcov_info;
extern void __gcov_info_to_gcda(const struct gcov_info *info, void (*filename_fn)(const char *, void *),
                                void (*dump_fn)(const void *, unsigned, void *), void *(*allocate_fn)(unsigned, void *), void *arg);
extern const struct gcov_info *const __start_gcov_info[];
extern const struct gcov_info *const __stop_gcov_info[];

#include "vita_home.h"
#define PGO_ROOT VITA_HOME "/pgo"
static char sRun[64];

typedef struct Out {
    SceUID fd;
    unsigned bytes;
    int failed;
} Out;

static void pgo_name(const char *name, void *arg) {
    Out *o = arg;
    const char *b = strrchr(name, '/');
    char path[200];
    snprintf(path, sizeof(path), "%s/%s", sRun, b != NULL ? b + 1 : name);
    o->fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    o->failed |= o->fd < 0;
}

static void pgo_write(const void *data, unsigned n, void *arg) {
    Out *o = arg;
    if (o->fd >= 0 && sceIoWrite(o->fd, data, n) != (int)n) {
        o->failed = 1;
    }
    o->bytes += n;
}

static void *pgo_alloc(unsigned n, void *arg) {
    (void)arg;
    return malloc(n); /* (only for value profiles, which these builds do not make) */
}

void Vita_PgoInit(void) {
    SceIoStat st;
    int n;
    sceIoMkdir(PGO_ROOT, 0777);
    for (n = 1; n < 1000; n++) { /* the first run-<n> not there yet */
        snprintf(sRun, sizeof(sRun), PGO_ROOT "/run-%03d", n);
        if (sceIoGetstat(sRun, &st) < 0) {
            break;
        }
    }
    sceIoMkdir(sRun, 0777);
    Vita_Log("vita: profile counts (BT3_PGO=gen) of %d objects go to %s\n", (int)(__stop_gcov_info - __start_gcov_info), sRun);
}

void Vita_PgoDump(const char *why) {
    static volatile int busy;
    const struct gcov_info *const *p;
    Out o;
    unsigned files = 0, bytes = 0, bad = 0;
    if (sRun[0] == '\0' || __atomic_exchange_n(&busy, 1, __ATOMIC_ACQ_REL)) {
        return;
    }
    for (p = __start_gcov_info; p < __stop_gcov_info; p++) {
        if (*p == NULL) {
            continue;
        }
        memset(&o, 0, sizeof(o));
        o.fd = -1;
        __gcov_info_to_gcda(*p, pgo_name, pgo_write, pgo_alloc, &o);
        if (o.fd >= 0) {
            sceIoClose(o.fd);
        }
        files++;
        bytes += o.bytes;
        bad += o.failed != 0;
    }
    Vita_Log("vita: profile counts written (%s): %u files, %u KB, %u failed, in %s\n", why, files, bytes >> 10, bad, sRun);
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}
#else
void Vita_PgoInit(void) {}
void Vita_PgoDump(const char *why) { (void)why; }
#endif
