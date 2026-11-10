/* The reading thread (vita_file.c): reads of the memory card off the game's thread, for the songs (snd_adx.c) and
   the archives' no-wait reads (ADXF_ReadNw). */
#ifndef VITA_READ_H
#define VITA_READ_H

#include <psp2/types.h>
#include <stdint.h>

typedef struct VitaReadJob VitaReadJob;
struct VitaReadJob {
    SceUID fd;
    SceOff off;
    void *buf;
    uint32_t len;
    /* on the reading thread after the read (`got`: sceIoPread's result); 1: read again, with the fields as it left
       them (the next piece) */
    int (*done)(VitaReadJob *j, int got);
    void *user;
    volatile int state; /* 0 idle (or finished), 1 queued, 2 being read */
    volatile int cancel;
    VitaReadJob *next;
};

/* 1: the reading thread is there (bt3.txt asyncread, the first call starts it) */
int Vita_ReadOn(void);
/* 1: queued; 0: no reading thread (the caller reads itself) */
int Vita_ReadStart(VitaReadJob *j);
/* returns once the job is neither queued nor being read */
void Vita_ReadCancel(VitaReadJob *j);
static inline int Vita_ReadIdle(const VitaReadJob *j) { return __atomic_load_n(&j->state, __ATOMIC_ACQUIRE) == 0; }

/* where the bytes of a file are: a path, or an archive member's name from Port_FilePath ("afs:p:f"); *own: fd is
   the caller's to close. 0: not found */
int Vita_FileSpan(const char *path, SceUID *fd, uint32_t *base, uint32_t *size, int *own);

#endif
