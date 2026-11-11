/*
 * The reading thread: the memory card's reads off the game's thread. A song is read piece by piece while it plays
 * (snd_adx.c: the whole file at its start held the game for 0.1 to 0.5 s at each change of music), and the archives'
 * no-wait reads (vita_file.c ADXF_ReadNw) are what their name says, so that a screen goes on drawing while its
 * pictures load (the menus' portraits, the loading screens). bt3.txt asyncread=0: the reads on the caller's thread,
 * as before. One thread, the jobs in order; a job's `done` may ask for its next piece.
 */
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include <stddef.h>
#include "vita_read.h"

extern int Port_Setting(const char *name, int def);
extern void Vita_Log(const char *fmt, ...);

int gVitaAsyncRead = -1; /* bt3.txt asyncread=0 / 1 (vita_main.c), -1 not given */
static SceUID sRdMutex = -1, sRdSema = -1;
static int sRdOn = -1;
static VitaReadJob *sRdHead, *sRdTail;
unsigned gVitaRdJobs, gVitaRdBytes; /* (for the log) */

static int rd_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        VitaReadJob *j;
        int got, again;
        sceKernelWaitSema(sRdSema, 1, NULL);
        sceKernelLockMutex(sRdMutex, 1, NULL);
        j = sRdHead;
        if (j != NULL) {
            sRdHead = j->next;
            if (sRdHead == NULL) {
                sRdTail = NULL;
            }
            j->next = NULL;
            __atomic_store_n(&j->state, 2, __ATOMIC_RELEASE);
        }
        sceKernelUnlockMutex(sRdMutex, 1);
        if (j == NULL) {
            continue;
        }
        got = __atomic_load_n(&j->cancel, __ATOMIC_ACQUIRE) ? 0 : j->len != 0 ? sceIoPread(j->fd, j->buf, j->len, j->off) : 0;
        gVitaRdJobs++;
        gVitaRdBytes += got > 0 ? (unsigned)got : 0u;
        again = !__atomic_load_n(&j->cancel, __ATOMIC_ACQUIRE) && j->done != NULL ? j->done(j, got) : 0;
        sceKernelLockMutex(sRdMutex, 1, NULL);
        if (again && !__atomic_load_n(&j->cancel, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&j->state, 1, __ATOMIC_RELEASE);
            if (sRdTail != NULL) {
                sRdTail->next = j;
            } else {
                sRdHead = j;
            }
            sRdTail = j;
            sceKernelUnlockMutex(sRdMutex, 1);
            sceKernelSignalSema(sRdSema, 1);
        } else {
            __atomic_store_n(&j->state, 0, __ATOMIC_RELEASE);
            sceKernelUnlockMutex(sRdMutex, 1);
        }
    }
    return 0;
}

int Vita_ReadOn(void) {
    if (sRdOn < 0) {
        SceUID th;
        sRdOn = 0;
        if ((gVitaAsyncRead >= 0 ? gVitaAsyncRead : Port_Setting("asyncread", 1)) == 0) {
            Vita_Log("vita: reads on the caller's thread (asyncread=0)\n");
            return 0;
        }
        sRdMutex = sceKernelCreateMutex("bt3_rdq", 0, 0, NULL);
        sRdSema = sceKernelCreateSema("bt3_rdq", 0, 0, 1 << 20, NULL);
        /* above the game's thread (160), below the sound's (100): it mostly waits for the card; on the first two
           cores (the third's drawing thread is the one short of time) */
        th = sRdMutex >= 0 && sRdSema >= 0 ? sceKernelCreateThread("bt3_read", rd_thread, 140, 0x4000, 0, 0x00030000, NULL) : -1;
        if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
            Vita_Log("vita: no reading thread (%08x): reads on the caller's thread\n", (unsigned)th);
            return 0;
        }
        sRdOn = 1;
    }
    return sRdOn;
}

int Vita_ReadStart(VitaReadJob *j) {
    if (!Vita_ReadOn()) {
        return 0;
    }
    sceKernelLockMutex(sRdMutex, 1, NULL);
    __atomic_store_n(&j->cancel, 0, __ATOMIC_RELEASE);
    j->next = NULL;
    __atomic_store_n(&j->state, 1, __ATOMIC_RELEASE);
    if (sRdTail != NULL) {
        sRdTail->next = j;
    } else {
        sRdHead = j;
    }
    sRdTail = j;
    sceKernelUnlockMutex(sRdMutex, 1);
    sceKernelSignalSema(sRdSema, 1);
    return 1;
}

void Vita_ReadCancel(VitaReadJob *j) {
    if (sRdOn <= 0 || Vita_ReadIdle(j)) {
        return;
    }
    sceKernelLockMutex(sRdMutex, 1, NULL);
    __atomic_store_n(&j->cancel, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&j->state, __ATOMIC_ACQUIRE) == 1) { /* still queued: out of the queue (the thread's count of jobs stays one higher: it finds none) */
        VitaReadJob **q = &sRdHead, *prev = NULL;
        while (*q != NULL && *q != j) {
            prev = *q;
            q = &(*q)->next;
        }
        if (*q == j) {
            *q = j->next;
            if (sRdTail == j) {
                sRdTail = prev;
            }
            j->next = NULL;
        }
        __atomic_store_n(&j->state, 0, __ATOMIC_RELEASE);
    }
    while (__atomic_load_n(&j->state, __ATOMIC_ACQUIRE) == 2) { /* being read: until it is done */
        sceKernelUnlockMutex(sRdMutex, 1);
        sceKernelDelayThread(200);
        sceKernelLockMutex(sRdMutex, 1, NULL);
    }
    sceKernelUnlockMutex(sRdMutex, 1);
}

