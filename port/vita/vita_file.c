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
 * their archive members the same way (port/src/gs/snd_adx.c).
 *
 * Mods, as in the PC build: a file under ux0:data/bt3/mods/ is used instead of the disc's.
 *   mods/pzs3us1/00042.bin       member 42 of PZS3US1.AFS (the folder is the archive's name; any extension, the
 *                                number may have fewer digits: 42.pak works too)
 *   mods/disc/DATA/<NAME>        a file of the disc's DATA folder (a film, or a whole archive: PZS3US1.AFS)
 * The folders are looked through once at start (no probe of the memory card on every read). The setting "mods"
 * (Options > Controller > Vita Settings, on unless set to 0) turns them off without deleting them; it takes effect
 * at the next start. The game's program (SLUS_216.78, BIN/DBZP.BIN) cannot be modded: it is compiled into the port.
 */
#include <ctype.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2048
enum { STAT_STOP = 1, STAT_READING = 2, STAT_READEND = 3, STAT_ERROR = 4 };

typedef struct Partition {
    SceUID fd; /* 0 = not loaded */
    uint32_t count;
    uint32_t *table; /* offset, size per file */
    char name[16];   /* the archive's name in lower case, without ".afs": its folder under mods/ */
    char path[300];  /* where it was opened (to open it again) */
} Partition;

typedef struct PortFile {
    SceUID fd;
    int owned; /* a loose file: closed with the handle */
    int pt;    /* else the archive it is in (its descriptor is the partition's, opened again if it goes bad) */
    uint32_t base, size;
    int32_t sizeSct, posSct, stat;
} PortFile;

static Partition sPart[8];

/* ---- mods ---- */
#define MODS_DIR "ux0:data/bt3/mods"
typedef struct ModFile { uint16_t ptid; uint32_t flid; char name[48]; } ModFile; /* a member (ptid < 8) or a DATA file (ptid 8) */
static ModFile *sMods;
static int sModN, sModCap, sModsOn = -1;
unsigned gVitaModFiles; /* for the log */

static void mod_add(int ptid, uint32_t flid, const char *name) {
    if (sModN == sModCap) {
        ModFile *m = realloc(sMods, (size_t)(sModCap ? sModCap * 2 : 64) * sizeof(ModFile));
        if (m == NULL) {
            return;
        }
        sMods = m;
        sModCap = sModCap ? sModCap * 2 : 64;
    }
    sMods[sModN].ptid = (uint16_t)ptid;
    sMods[sModN].flid = flid;
    snprintf(sMods[sModN].name, sizeof(sMods[0].name), "%s", name);
    sModN++;
}

/* The files of one folder of mods/: members by their number, or (ptid 8) DATA files by their name. */
static void mod_scan(const char *dir, int ptid) {
    SceUID d = sceIoDopen(dir);
    SceIoDirent e;
    int n = 0;
    if (d < 0) {
        return;
    }
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        if (SCE_S_ISDIR(e.d_stat.st_mode) || e.d_name[0] == '.') {
            continue;
        }
        if (ptid < 8) {
            char *end;
            unsigned long v = strtoul(e.d_name, &end, 10);
            if (end != e.d_name && (*end == '\0' || *end == '.')) {
                mod_add(ptid, (uint32_t)v, e.d_name);
                n++;
            }
        } else {
            char up[48];
            size_t i;
            for (i = 0; i + 1 < sizeof(up) && e.d_name[i] != '\0'; i++) {
                up[i] = (char)toupper((unsigned char)e.d_name[i]);
            }
            up[i] = '\0';
            mod_add(8, 0, up);
            n++;
        }
    }
    sceIoDclose(d);
    if (n > 0) {
        extern void Vita_Log(const char *fmt, ...);
        Vita_Log("mods: %d file(s) in %s\n", n, dir);
        gVitaModFiles += (unsigned)n;
    }
}

static int mods_on(void) {
    if (sModsOn < 0) {
        extern int Port_Setting(const char *name, int def);
        extern void Vita_Log(const char *fmt, ...);
        sModsOn = Port_Setting("mods", 1) != 0;
        if (sModsOn) {
            mod_scan(MODS_DIR "/disc/DATA", 8);
        } else {
            Vita_Log("mods: off (setting)\n");
        }
    }
    return sModsOn;
}

/* the mods/ path of member `flid` of partition `ptid`, or of DATA file `name` (upper case); 0: none */
static int mod_path(int ptid, uint32_t flid, const char *name, char *out, size_t size) {
    int i;
    if (!mods_on() || sModN == 0) {
        return 0;
    }
    for (i = 0; i < sModN; i++) {
        if (ptid < 8 && sMods[i].ptid == ptid && sMods[i].flid == flid) {
            snprintf(out, size, MODS_DIR "/%s/%s", sPart[ptid].name, sMods[i].name);
            return 1;
        }
        if (ptid == 8 && sMods[i].ptid == 8 && strcmp(sMods[i].name, name) == 0) {
            snprintf(out, size, MODS_DIR "/disc/DATA/%s", sMods[i].name);
            return 1;
        }
    }
    return 0;
}
char gPortMoviePath[512]; /* host path of the movie file opened last */
extern char gVitaDisc[];
extern void *Port_LowAlloc(size_t size);
extern void Port_LowFree(void *addr);
extern void Vita_Fatal(const char *fmt, ...);
unsigned gVitaFileOpens, gVitaFileBytes; /* for the status screen */
unsigned gVitaFileReads, gVitaFileReadsGame;       /* for the log: reads, and those of the game's thread, and their time */
unsigned long long gVitaFileUs, gVitaFileUsGame;
int gVitaGameThreadId;

/* <disc>/DATA/<NAME>: the name in upper case, without a directory or a ";1"; mods/disc/DATA/<NAME> when there is one. */
static int mod_path(int ptid, uint32_t flid, const char *name, char *out, size_t size);
static void data_path(const char *fname, char *out, size_t size) {
    const char *base = fname, *p;
    char name[48];
    size_t n = 0;

    for (p = fname; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            base = p + 1;
        }
    }
    for (p = base; *p != '\0' && *p != ';' && n + 1 < sizeof(name); p++) {
        name[n++] = (char)toupper((unsigned char)*p);
    }
    name[n] = '\0';
    if (mod_path(8, 0, name, out, size)) {
        extern void Vita_Log(const char *fmt, ...);
        Vita_Log("mods: %s instead of the disc's\n", out);
        return;
    }
    snprintf(out, size, "%s/DATA/%s", gVitaDisc, name);
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
    snprintf(pt->path, sizeof(pt->path), "%s", path);
    {   /* its name, for its folder of mods */
        const char *b = strrchr(path, '/') != NULL ? strrchr(path, '/') + 1 : path;
        size_t i;
        for (i = 0; i + 1 < sizeof(pt->name) && b[i] != '\0' && b[i] != '.'; i++) {
            pt->name[i] = (char)tolower((unsigned char)b[i]);
        }
        pt->name[i] = '\0';
        if (mods_on()) {
            char dir[96];
            snprintf(dir, sizeof(dir), MODS_DIR "/%s", pt->name);
            mod_scan(dir, ptid & 7);
        }
    }
    if (sceIoPread(pt->fd, head, 8, 0) != 8 || memcmp(head, "AFS\0", 4) != 0 || head[1] == 0 || head[1] > 0x100000) {
        Vita_Fatal("not an AFS archive:\n  %s", path);
    }
    pt->count = head[1];
    pt->table = malloc((size_t)pt->count * 8);
    if (pt->table == NULL || sceIoPread(pt->fd, pt->table, pt->count * 8, 8) != (int)(pt->count * 8)) {
        Vita_Fatal("cannot read the file table of\n  %s", path);
    }
    return 0;
}

int ADXF_GetPtStat(int ptid) {
    return sPart[ptid & 7].fd > 0 ? STAT_READEND : STAT_STOP;
}

void *ADXF_OpenAfs(int ptid, int flid) {
    Partition *pt = &sPart[ptid & 7];
    PortFile *f;

    if (pt->fd <= 0 || flid < 0 || (uint32_t)flid >= pt->count) {
        fprintf(stderr, "bt3: file not found: partition %d, index %d\n", ptid, flid);
        return NULL;
    }
    {   /* a mod of this member: its own file */
        char path[160];
        if (mod_path(ptid & 7, (uint32_t)flid, NULL, path, sizeof(path))) {
            SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
            if (fd >= 0) {
                SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
                f = Port_LowAlloc(sizeof(PortFile));
                f->fd = fd;
                f->owned = 1;
                f->base = 0;
                f->size = (uint32_t)size;
                f->sizeSct = (int32_t)((size + SECTOR - 1) / SECTOR);
                f->posSct = 0;
                f->stat = STAT_STOP;
                gVitaFileOpens++;
                return f;
            }
        }
    }
    f = Port_LowAlloc(sizeof(PortFile));
    f->fd = pt->fd;
    f->owned = 0;
    f->pt = ptid & 7;
    f->posSct = 0;
    f->base = pt->table[flid * 2];
    f->size = pt->table[flid * 2 + 1];
    f->sizeSct = (int32_t)((f->size + SECTOR - 1) / SECTOR);
    f->stat = STAT_STOP;
    gVitaFileOpens++;
    return f;
}

/* A file outside the archives, named relative to the disc's data directory. */
void *ADXF_Open(char *fname, void *atr) {
    char path[300];
    PortFile *f;
    SceUID fd;
    SceOff size;
    size_t n;

    (void)atr;
    data_path(fname, path, sizeof(path));
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

/* An archive's descriptor that has gone bad (a read failed): opened again. A memory card adapter's driver can let the
   descriptors of a program go after the system has put it aside (the PS button, sleep); the reads came back empty and
   the game took zeros for its files. 1 when it was opened again. */
static int pt_reopen(Partition *pt, int err) {
    extern void Vita_Log(const char *fmt, ...);
    SceUID fd;
    if (pt->path[0] == '\0' || (fd = sceIoOpen(pt->path, SCE_O_RDONLY, 0)) < 0) {
        Vita_Log("vita: reading %s failed (0x%08X), and it cannot be opened again\n", pt->path, (unsigned)err);
        return 0;
    }
    Vita_Log("vita: reading %s failed (0x%08X): opened again\n", pt->path, (unsigned)err);
    sceIoClose(pt->fd);
    pt->fd = fd;
    return 1;
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
        if (!f->owned) {
            f->fd = sPart[f->pt].fd; /* (the archive's, which may have been opened again) */
        }
        got = have != 0 ? sceIoPread(f->fd, buf, have, (SceOff)f->base + pos) : 0;
        if (got != (int)have && have != 0 && !f->owned && pt_reopen(&sPart[f->pt], got)) {
            f->fd = sPart[f->pt].fd;
            got = sceIoPread(f->fd, buf, have, (SceOff)f->base + pos);
        }
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
        char mod[160];
        if (pt->fd <= 0 || flid < 0 || (uint32_t)flid >= pt->count) {
            return NULL;
        }
        if (mod_path(ptid & 7, (uint32_t)flid, NULL, mod, sizeof(mod))) {
            return Vita_LoadWhole(mod, size); /* (its own file) */
        }
        n = pt->table[flid * 2 + 1];
        p = malloc(n ? n : 1);
        if (p != NULL && n != 0 && sceIoPread(pt->fd, p, n, pt->table[flid * 2]) != (int)n && !(pt_reopen(pt, -1) &&
                                                                                       sceIoPread(pt->fd, p, n, pt->table[flid * 2]) == (int)n)) {
            free(p);
            return NULL;
        }
        if (p == NULL) {
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
