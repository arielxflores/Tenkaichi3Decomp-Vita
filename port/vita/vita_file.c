/*
 * The game's file access on the PS Vita (port/src/plat_file.c is the PC's).
 *
 * The game reads everything through CRI's ADXF library: three AFS archives ("partitions" 0..2), files addressed by
 * (partition, index), read in 2048-byte sectors. The PC build unpacks the archives into 68,611 loose files first;
 * copying that many files to a memory card takes hours, so here the archives are read as they are on the disc:
 * <disc>/DATA/PZS3US0.AFS, PZS3US1.AFS, PZS3US2.AFS, next to the other files of the disc's DATA folder.
 * <disc> is the folder gVitaDisc (vita_main.c: ux0:data/bt3/disc, or disc=... in bt3.txt).
 *
 * An AFS archive: "AFS\0", the number of files, then (offset, size) per file; the data follows, each file on a
 * 2048-byte boundary. Reads go through sceIoPread, so the position is not shared state. The sound streams read
 * their archive members the same way (port/src/gs/snd_adx.c). Not supported: the PC build's mods folder.
 *
 * European languages (optional): when ux0:data/bt3/pzs3eu1/ holds the entries from SLES-54945 (same as the PC
 * recomp), selected PZS3US1 members are read from those .bin files instead (vita_eu.c).
 */
#include <ctype.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vita_eu.h"

#define SECTOR 2048
enum { STAT_STOP = 1, STAT_READING = 2, STAT_READEND = 3, STAT_ERROR = 4 };

typedef struct Partition {
    SceUID fd; /* 0 = not loaded */
    uint32_t count;
    uint32_t *table; /* offset, size per file */
} Partition;

typedef struct PortFile {
    SceUID fd;
    int owned; /* a loose file: closed with the handle */
    uint32_t base, size;
    int32_t sizeSct, posSct, stat;
} PortFile;

static Partition sPart[8];
char gPortMoviePath[512]; /* host path of the movie file opened last */
extern char gVitaDisc[];
extern void *Port_LowAlloc(size_t size);
extern void Port_LowFree(void *addr);
extern void Vita_Fatal(const char *fmt, ...);
unsigned gVitaFileOpens, gVitaFileBytes; /* for the status screen */
unsigned gVitaFileReads, gVitaFileReadsGame;       /* for the log: reads, and those of the game's thread, and their time */
unsigned long long gVitaFileUs, gVitaFileUsGame;
int gVitaGameThreadId;

/* <disc>/DATA/<NAME>: the name in upper case, without a directory or a ";1". */
static void data_path(const char *fname, char *out, size_t size) {
    const char *base = fname, *p;
    size_t n;

    for (p = fname; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            base = p + 1;
        }
    }
    n = (size_t)snprintf(out, size, "%s/DATA/", gVitaDisc);
    for (p = base; *p != '\0' && *p != ';' && n + 1 < size; p++) {
        out[n++] = (char)toupper((unsigned char)*p);
    }
    out[n] = '\0';
}

int Port_FilePath(int ptid, int flid, const char *fname, char *out, int size) {
    SceUID fd;

    (void)ptid;
    (void)flid;
    if (fname == NULL) { /* a member of an archive: a name for Vita_LoadWhole below, not a path */
        Partition *pt = &sPart[ptid & 7];
        if (pt->fd <= 0 || flid < 0 || (uint32_t)flid >= pt->count) {
            return 0;
        }
        snprintf(out, (size_t)size, "afs:%d:%d", ptid & 7, flid);
        return 1;
    }
    data_path(fname, out, (size_t)size);
    /* Movie BGM goes through ADXT_StartFname → here, not ADXF_Open; swap in ZS3EUOP when present. */
    {
        char eu[320], up[64];
        const char *base = fname, *p;
        size_t i;
        for (p = fname; *p != '\0'; p++) {
            if (*p == '/' || *p == '\\' || *p == ':') {
                base = p + 1;
            }
        }
        for (i = 0; base[i] != '\0' && base[i] != ';' && i + 1 < sizeof(up); i++) {
            up[i] = (char)toupper((unsigned char)base[i]);
        }
        up[i] = '\0';
        if (strcmp(up, "ZS3USOP.ADX") == 0 && VitaEu_OpeningAdxPath(eu, (int)sizeof(eu))) {
            snprintf(out, (size_t)size, "%s", eu);
        }
    }
    fd = sceIoOpen(out, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    sceIoClose(fd);
    return 1;
}

int ADXF_LoadPartitionNw(int ptid, char *fname, void *dir, void *ptinfo) {
    Partition *pt = &sPart[ptid & 7];
    char path[300];
    uint32_t head[2];

    (void)dir;
    (void)ptinfo;
    if (pt->fd > 0) {
        return 0;
    }
    data_path(fname, path, sizeof(path));
    pt->fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (pt->fd < 0) {
        Vita_Fatal("archive not found:\n  %s\n(error 0x%08X)", path, (unsigned)pt->fd);
    }
    if (sceIoPread(pt->fd, head, 8, 0) != 8 || memcmp(head, "AFS\0", 4) != 0 || head[1] == 0 || head[1] > 0x100000) {
        Vita_Fatal("not an AFS archive:\n  %s", path);
    }
    pt->count = head[1];
    pt->table = malloc((size_t)pt->count * 8);
    if (pt->table == NULL || sceIoPread(pt->fd, pt->table, pt->count * 8, 8) != (int)(pt->count * 8)) {
        Vita_Fatal("cannot read the file table of\n  %s", path);
    }
    VitaEu_NotePartition(ptid, fname);
    VitaEu_PatchTocSizes(ptid, pt->table, pt->count);
    return 0;
}

int ADXF_GetPtStat(int ptid) {
    return sPart[ptid & 7].fd > 0 ? STAT_READEND : STAT_STOP;
}

void *ADXF_OpenAfs(int ptid, int flid) {
    Partition *pt = &sPart[ptid & 7];
    PortFile *f;
    char eu[320];
    uint32_t usaSize;

    if (pt->fd <= 0 || flid < 0 || (uint32_t)flid >= pt->count) {
        fprintf(stderr, "bt3: file not found: partition %d, index %d\n", ptid, flid);
        return NULL;
    }
    usaSize = pt->table[flid * 2 + 1];
    f = Port_LowAlloc(sizeof(PortFile));
    memset(f, 0, sizeof(*f));
    if (VitaEu_OverridePath(ptid, flid, eu, (int)sizeof(eu))) {
        SceUID fd = sceIoOpen(eu, SCE_O_RDONLY, 0);
        SceOff size;
        if (fd < 0) {
            Port_LowFree(f);
            fprintf(stderr, "bt3: EU file not found: %s\n", eu);
            return NULL;
        }
        size = sceIoLseek(fd, 0, SCE_SEEK_END);
        if (size <= 0) {
            sceIoClose(fd);
            Port_LowFree(f);
            return NULL;
        }
        /*
         * European archives are often a few KB larger than the USA AFS table entry. Callers that still
         * size buffers from the table would overflow; keep the TOC in sync and log the swap.
         */
        f->fd = fd;
        f->owned = 1;
        f->base = 0;
        f->size = (uint32_t)size;
        pt->table[flid * 2 + 1] = f->size;
        {
            extern void Vita_Log(const char *fmt, ...);
            Vita_Log("eu: afs:%d:%d -> %s (%u bytes, was %u)\n", ptid, flid, eu, f->size, usaSize);
        }
    } else {
        f->fd = pt->fd;
        f->base = pt->table[flid * 2];
        f->size = usaSize;
    }
    f->sizeSct = (int32_t)((f->size + SECTOR - 1) / SECTOR);
    f->stat = STAT_STOP;
    gVitaFileOpens++;
    return f;
}

/* A file outside the archives, named relative to the disc's data directory. */
void *ADXF_Open(char *fname, void *atr) {
    char path[300], eu[320], up[64];
    PortFile *f;
    SceUID fd;
    SceOff size;
    size_t n;
    const char *base;
    const char *p;

    (void)atr;
    data_path(fname, path, sizeof(path));
    /* European opening soundtrack (optional), same rule as the PC recomp. */
    for (base = fname, p = fname; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            base = p + 1;
        }
    }
    {
        size_t i;
        for (i = 0; base[i] != '\0' && base[i] != ';' && i + 1 < sizeof(up); i++) {
            up[i] = (char)toupper((unsigned char)base[i]);
        }
        up[i] = '\0';
    }
    if (strcmp(up, "ZS3USOP.ADX") == 0 && VitaEu_OpeningAdxPath(eu, (int)sizeof(eu))) {
        snprintf(path, sizeof(path), "%s", eu);
    }
    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "bt3: file not found: %s\n", path);
        return NULL;
    }
    n = strlen(path);
    if (n > 4 && strcmp(&path[n - 4], ".PSS") == 0) { /* a movie: its decoder (plat_movie.c) needs the path */
        snprintf(gPortMoviePath, sizeof(gPortMoviePath), "%s", path);
    }
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    f = Port_LowAlloc(sizeof(PortFile));
    f->fd = fd;
    f->owned = 1;
    f->size = (uint32_t)size;
    f->sizeSct = (int32_t)((size + SECTOR - 1) / SECTOR);
    f->stat = STAT_STOP;
    gVitaFileOpens++;
    return f;
}

void ADXF_Close(void *h) {
    PortFile *f = h;

    if (f != NULL) {
        if (f->owned) {
            sceIoClose(f->fd);
        }
        Port_LowFree(f);
    }
}

int ADXF_GetFsizeSct(void *h) {
    return ((PortFile *)h)->sizeSct;
}

/* Reads nsct sectors at the current position; the part of the last sector behind the end of the file is zero. */
int ADXF_ReadNw(void *h, int nsct, void *buf) {
    PortFile *f = h;
    uint32_t pos = (uint32_t)f->posSct * SECTOR, want, have;
    int got;

    if (nsct > f->sizeSct - f->posSct) {
        nsct = f->sizeSct - f->posSct;
    }
    if (nsct <= 0) {
        f->stat = STAT_READEND;
        return 0;
    }
    want = (uint32_t)nsct * SECTOR;
    have = pos < f->size ? f->size - pos : 0;
    if (have > want) {
        have = want;
    }
    {
        unsigned long long t0 = sceKernelGetProcessTimeWide(), dt;
        got = have != 0 ? sceIoPread(f->fd, buf, have, (SceOff)f->base + pos) : 0;
        dt = sceKernelGetProcessTimeWide() - t0;
        gVitaFileReads++;
        gVitaFileUs += dt;
        if (sceKernelGetThreadId() == gVitaGameThreadId) {
            gVitaFileReadsGame++;
            gVitaFileUsGame += dt;
        }
    }
    if (got < 0) {
        fprintf(stderr, "bt3: read error 0x%08X\n", (unsigned)got);
        got = 0;
    }
    memset((uint8_t *)buf + got, 0, want - (uint32_t)got);
    f->posSct += nsct;
    f->stat = STAT_READEND;
    gVitaFileBytes += (unsigned)got;
    return nsct;
}

int ADXF_Stop(void *h) {
    ((PortFile *)h)->stat = STAT_STOP;
    return 0;
}

int ADXF_GetStat(void *h) {
    return ((PortFile *)h)->stat;
}

int ADXF_Tell(void *h) {
    return ((PortFile *)h)->posSct;
}

/* CRI set-up and servicing: nothing to do without a drive or a second thread. */
void ADXPS2_ExecVint(int mode) { (void)mode; }
void ADXPS2_SetupDvdFs(void *param) { (void)param; }
void ADXPS2_LoadFcacheDvd(void *param) { (void)param; }
void ADXPS2_SetupThrd(void *param, int unk) { (void)param; (void)unk; }
void ADXM_ExecMain(void) {}
extern void Port_VBlank(void);
void ADXM_WaitVsync(void) { Port_VBlank(); }
void ADXERR_EntryErrFunc(void (*func)(void *obj, char *msg), void *obj) { (void)func; (void)obj; }

/* The drive: a PS2 DVD is always present and idle (sceCdGetDiskTypeSafe 0x14 = PS2 DVD, sceCdStatus 10 = paused). */
int sceCdGetDiskTypeSafe(void) { return 0x14; }
int sceCdStatus(void) { return 10; }

/* A whole file into memory (malloc): a path, or a name Port_FilePath gave for an archive member. The sound
   streams (port/src/gs/snd_adx.c) decode from memory. NULL when it cannot be read. */
void *Vita_LoadWhole(const char *path, uint32_t *size) {
    int ptid, flid;
    uint8_t *p;

    if (sscanf(path, "afs:%d:%d", &ptid, &flid) == 2) {
        Partition *pt = &sPart[ptid & 7];
        uint32_t n;
        char eu[320];
        if (pt->fd <= 0 || flid < 0 || (uint32_t)flid >= pt->count) {
            return NULL;
        }
        if (VitaEu_OverridePath(ptid, flid, eu, (int)sizeof(eu))) {
            SceUID fd = sceIoOpen(eu, SCE_O_RDONLY, 0);
            SceOff len;
            if (fd < 0) {
                return NULL;
            }
            len = sceIoLseek(fd, 0, SCE_SEEK_END);
            n = (uint32_t)len;
            p = malloc(n ? n : 1);
            if (p == NULL || sceIoPread(fd, p, n, 0) != (int)n) {
                free(p);
                sceIoClose(fd);
                return NULL;
            }
            sceIoClose(fd);
            *size = n;
            return p;
        }
        n = pt->table[flid * 2 + 1];
        p = malloc(n ? n : 1);
        if (p == NULL || sceIoPread(pt->fd, p, n, pt->table[flid * 2]) != (int)n) {
            free(p);
            return NULL;
        }
        *size = n;
        return p;
    } else {
        SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        SceOff n;
        if (fd < 0) {
            return NULL;
        }
        n = sceIoLseek(fd, 0, SCE_SEEK_END);
        p = n > 0 && n < 0x4000000 ? malloc((size_t)n) : NULL;
        if (p == NULL || sceIoPread(fd, p, (SceSize)n, 0) != (int)n) {
            free(p);
            sceIoClose(fd);
            return NULL;
        }
        sceIoClose(fd);
        *size = (uint32_t)n;
        return p;
    }
}
