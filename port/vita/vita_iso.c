/*
 * The game's files straight from the player's disc image: an .iso (a 1:1 copy of the PS2 DVD, as PCSX2 plays it) in
 * the data folder, ux0:data/bt3/ (or its disc/ or disk/; the BT4 port's ux0:data/bt4/), under any name, instead of
 * the program, BIN and DATA unpacked from it. vita_main.c's find_disc mounts it when no unpacked files are found.
 *
 * arielxflores's reader, from his Vita port's v1.2 (arielxflores/Tenkaichi3Decomp-Vita, 8484872), taken as it is;
 * only its comments on where the image goes are this fork's.
 *
 * Every file call of the program comes through here (build.py links sceIoOpen, sceIoPread, sceIoRead, sceIoLseek,
 * sceIoGetstat, sceIoDopen and their companions with --wrap, the C library's own included): a path inside a
 * mounted image, <image>/DATA/PZS3US1.AFS, opens the image itself (a handle of its own each time), and that handle's
 * reads are moved by the file's place in the image and kept to its size. Paths that are not in an image, and handles
 * that are not of one, go to the system as they are: with no image mounted that is one load and a compare more per
 * call. Images are mounted at the start, before the game's threads.
 *
 * An image is ISO 9660 (the PS2's DVDs; the UDF beside it is not needed): its folders are read once at mounting
 * (a PS2 disc has a few dozen files); each file is one run of 2048-byte sectors. Names are matched without case and
 * without the ";1" of the disc. Files on an image are read-only: opening one to write is refused (as the disc).
 */
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern void Vita_Log(const char *fmt, ...);

#define ISO_SECTOR 2048
#define ISO_MOUNTS 3     /* the game's disc (his port also mounts the European one) */
#define ISO_OPEN 64      /* handles of an image's files open at once */
#define ISO_DIRS 8       /* folders of an image being listed at once */
#define ISO_ENTRIES 4096 /* files and folders of one image, at most */

#define E_NOENT ((int)0x80010002)
#define E_NOTDIR ((int)0x80010014)
#define E_ISDIR ((int)0x80010015)
#define E_INVAL ((int)0x80010016)
#define E_MFILE ((int)0x80010018)
#define E_ROFS ((int)0x8001001E)

SceUID __real_sceIoOpen(const char *file, int flags, SceMode mode);
int __real_sceIoClose(SceUID fd);
SceSSize __real_sceIoRead(SceUID fd, void *buf, SceSize nbyte);
int __real_sceIoPread(SceUID fd, void *data, SceSize size, SceOff offset);
SceOff __real_sceIoLseek(SceUID fd, SceOff offset, int whence);
int __real_sceIoLseek32(SceUID fd, int offset, int whence);
int __real_sceIoGetstat(const char *file, SceIoStat *stat);
int __real_sceIoGetstatByFd(SceUID fd, SceIoStat *stat);
SceUID __real_sceIoDopen(const char *dirname);
int __real_sceIoDread(SceUID fd, SceIoDirent *dir);
int __real_sceIoDclose(SceUID fd);

typedef struct IsoEntry {
    char name[40];  /* upper case, without the ";1" */
    int16_t parent; /* its folder (an index), -1 the image's top */
    uint8_t dir;
    uint32_t lba, size;
} IsoEntry;

typedef struct IsoMount {
    char path[256]; /* the image's own path: its files are <path>/<folder>/<name> */
    int len;
    IsoEntry *ent;
    int n;
} IsoMount;

typedef struct IsoOpen {
    SceUID fd; /* 0: a free slot; -1 being filled (set last, so a reader never sees a half-made one) */
    SceOff base, size, pos;
} IsoOpen;

typedef struct IsoDir {
    SceUID fd; /* a real folder's handle (the image's own folder), so the number is the system's and unique */
    int mount, parent, next;
} IsoDir;

static IsoMount sMount[ISO_MOUNTS];
static int sMountN;
static IsoOpen sOpen[ISO_OPEN];
static int sOpenN; /* slots in use: none, and a handle is not looked up at all */
static IsoDir sDir[ISO_DIRS];
static int sDirN;

/* A free slot taken (its fd -1 meanwhile, which no handle matches), without a lock: a thread spinning on a lock
   held by a lower priority one on the same core would never let it go on. */
static int take_slot(SceUID *fd) {
    SceUID free_ = 0;
    return __atomic_compare_exchange_n(fd, &free_, -1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* --- reading an image's folders --- */

typedef struct Scan {
    SceUID fd;
    SceOff imageSize;
    IsoEntry *ent;
    int n, bad;
} Scan;

static void scan_dir(Scan *s, uint32_t lba, uint32_t size, int parent, int depth) {
    uint8_t *b;
    uint32_t pos = 0;
    int first = s->n, i, last;

    if (depth > 8 || size == 0 || size > 0x100000 || (SceOff)lba * ISO_SECTOR + size > s->imageSize) {
        s->bad = 1;
        return;
    }
    b = malloc(size);
    if (b == NULL || __real_sceIoPread(s->fd, b, size, (SceOff)lba * ISO_SECTOR) != (int)size) {
        free(b);
        s->bad = 1;
        return;
    }
    while (pos + 34 <= size) {
        uint32_t len = b[pos], nameLen;
        const uint8_t *r = b + pos;
        IsoEntry *e;
        uint32_t k;

        if (len == 0) { /* (the rest of this sector is empty: the next one) */
            pos = (pos / ISO_SECTOR + 1) * ISO_SECTOR;
            continue;
        }
        if (len < 34 || pos + len > size) {
            break;
        }
        pos += len;
        nameLen = r[32];
        if (33 + nameLen > len || nameLen == 0 || (nameLen == 1 && r[33] <= 1)) { /* (".", "..") */
            continue;
        }
        if (s->n >= ISO_ENTRIES || nameLen >= sizeof(e->name) || (r[25] & 0x80)) { /* (a file in several runs: not on these discs) */
            continue;
        }
        e = &s->ent[s->n];
        memset(e, 0, sizeof(*e));
        for (k = 0; k < nameLen && r[33 + k] != ';'; k++) {
            char c = (char)r[33 + k];
            e->name[k] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
        }
        if (k > 1 && e->name[k - 1] == '.') { /* ("NAME." for a name without an extension) */
            e->name[k - 1] = '\0';
        }
        e->parent = (int16_t)parent;
        e->dir = (r[25] & 2) != 0;
        e->lba = le32(r + 2);
        e->size = le32(r + 10);
        if (!e->dir && (SceOff)e->lba * ISO_SECTOR + e->size > s->imageSize) {
            continue; /* (beyond the image's end: a copy cut short) */
        }
        s->n++;
    }
    free(b);
    last = s->n;
    for (i = first; i < last && !s->bad; i++) {
        if (s->ent[i].dir) {
            scan_dir(s, s->ent[i].lba, s->ent[i].size, i, depth + 1);
        }
    }
}

/* the image's folders, or 0 (not an ISO 9660 image) */
static int scan_image(const char *path, IsoEntry **ent) {
    uint8_t pvd[ISO_SECTOR];
    Scan s;

    memset(&s, 0, sizeof(s));
    *ent = NULL;
    s.fd = __real_sceIoOpen(path, SCE_O_RDONLY, 0);
    if (s.fd < 0) {
        return 0;
    }
    s.imageSize = __real_sceIoLseek(s.fd, 0, SCE_SEEK_END);
    if (s.imageSize < 17 * ISO_SECTOR || __real_sceIoPread(s.fd, pvd, ISO_SECTOR, 16 * ISO_SECTOR) != ISO_SECTOR ||
        pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0 || pvd[156] < 34) {
        __real_sceIoClose(s.fd);
        return 0;
    }
    s.ent = malloc(sizeof(IsoEntry) * ISO_ENTRIES);
    if (s.ent != NULL) {
        scan_dir(&s, le32(pvd + 156 + 2), le32(pvd + 156 + 10), -1, 0);
    }
    __real_sceIoClose(s.fd);
    if (s.ent == NULL || s.bad || s.n == 0) {
        free(s.ent);
        return 0;
    }
    *ent = realloc(s.ent, sizeof(IsoEntry) * (size_t)s.n);
    if (*ent == NULL) {
        *ent = s.ent;
    }
    return s.n;
}

/* --- paths --- */

/* A path in one spelling: "ux0:/data//bt3/" and "ux0:data/bt3" are the same folder (the C library's fopen hands
   the system the first form). 0: too long to be one of ours. */
static int canon(char *dst, const char *src, size_t size) {
    size_t n = 0;
    const char *colon = strchr(src, ':'), *slash = strchr(src, '/');

    if (colon != NULL && (slash == NULL || colon < slash)) { /* the device, then no '/' */
        while (src <= colon) {
            if (n + 1 >= size) {
                return 0;
            }
            dst[n++] = *src++;
        }
        while (*src == '/') {
            src++;
        }
    }
    for (; *src != '\0'; src++) {
        if (*src == '/' && (n == 0 || dst[n - 1] == '/' || dst[n - 1] == ':')) {
            if (n == 0) {
                dst[n++] = '/'; /* (a path from the top: kept) */
            }
            continue;
        }
        if (n + 1 >= size) {
            return 0;
        }
        dst[n++] = *src;
    }
    while (n > 1 && dst[n - 1] == '/') {
        n--;
    }
    dst[n] = '\0';
    return 1;
}

/* A path in a mounted image: 1, with its mount and entry (-1 the image's top, -2 not there); else 0. */
static int lookup(const char *path_, int *mount, int *index) {
    int m, n = __atomic_load_n(&sMountN, __ATOMIC_ACQUIRE);
    char path[512];

    if (n == 0 || path_ == NULL || !canon(path, path_, sizeof(path))) {
        return 0;
    }
    for (m = 0; m < n; m++) {
        const IsoMount *im = &sMount[m];
        const char *p = path + im->len;
        int cur = -1;

        if (strncasecmp(path, im->path, (size_t)im->len) != 0 || (*p != '\0' && *p != '/')) {
            continue;
        }
        *mount = m;
        for (;;) {
            const char *seg;
            size_t len, cmp;
            int i, hit = -2;

            while (*p == '/') {
                p++;
            }
            if (*p == '\0') {
                break;
            }
            seg = p;
            while (*p != '\0' && *p != '/') {
                p++;
            }
            len = (size_t)(p - seg);
            cmp = len;
            for (i = 0; i < (int)len; i++) { /* ("NAME;1": the version left out) */
                if (seg[i] == ';') {
                    cmp = (size_t)i;
                    break;
                }
            }
            if (cur >= 0 && !im->ent[cur].dir) {
                *index = -2;
                return 1;
            }
            for (i = 0; i < im->n; i++) {
                if (im->ent[i].parent == cur && strncasecmp(im->ent[i].name, seg, cmp) == 0 && im->ent[i].name[cmp] == '\0') {
                    hit = i;
                    break;
                }
            }
            if (hit < 0) {
                *index = -2;
                return 1;
            }
            cur = hit;
        }
        *index = cur;
        return 1;
    }
    return 0;
}

static void fill_stat(SceIoStat *st, int dir, SceOff size) {
    st->st_mode = dir ? (SCE_S_IFDIR | SCE_S_IRUSR | SCE_S_IXUSR) : (SCE_S_IFREG | SCE_S_IRUSR);
    st->st_attr = dir ? SCE_SO_IFDIR : SCE_SO_IFREG;
    st->st_size = dir ? 0 : size;
}

/* --- handles --- */

static IsoOpen *open_find(SceUID fd) {
    int i;
    if (__atomic_load_n(&sOpenN, __ATOMIC_ACQUIRE) == 0 || fd <= 0) {
        return NULL;
    }
    for (i = 0; i < ISO_OPEN; i++) {
        if (__atomic_load_n(&sOpen[i].fd, __ATOMIC_ACQUIRE) == fd) {
            return &sOpen[i];
        }
    }
    return NULL;
}

static IsoDir *dir_find(SceUID fd) {
    int i;
    if (__atomic_load_n(&sDirN, __ATOMIC_ACQUIRE) == 0 || fd <= 0) {
        return NULL;
    }
    for (i = 0; i < ISO_DIRS; i++) {
        if (__atomic_load_n(&sDir[i].fd, __ATOMIC_ACQUIRE) == fd) {
            return &sDir[i];
        }
    }
    return NULL;
}

SceUID __wrap_sceIoOpen(const char *file, int flags, SceMode mode) {
    int m, i, k;
    const IsoEntry *e;
    SceUID fd;

    if (!lookup(file, &m, &i)) {
        return __real_sceIoOpen(file, flags, mode);
    }
    if (i == -2) {
        return (flags & SCE_O_WRONLY) ? E_ROFS : E_NOENT;
    }
    if (i == -1 || sMount[m].ent[i].dir) {
        return E_ISDIR;
    }
    if (flags & SCE_O_WRONLY) {
        return E_ROFS;
    }
    e = &sMount[m].ent[i];
    fd = __real_sceIoOpen(sMount[m].path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return fd;
    }
    for (k = 0; k < ISO_OPEN && !take_slot(&sOpen[k].fd); k++) {
    }
    if (k < ISO_OPEN) {
        sOpen[k].base = (SceOff)e->lba * ISO_SECTOR;
        sOpen[k].size = e->size;
        sOpen[k].pos = 0;
        __atomic_add_fetch(&sOpenN, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&sOpen[k].fd, fd, __ATOMIC_RELEASE);
    }
    if (k == ISO_OPEN) {
        __real_sceIoClose(fd);
        return E_MFILE;
    }
    return fd;
}

int __wrap_sceIoClose(SceUID fd) {
    IsoOpen *o = open_find(fd);
    if (o != NULL) {
        __atomic_store_n(&o->fd, 0, __ATOMIC_RELEASE);
        __atomic_sub_fetch(&sOpenN, 1, __ATOMIC_RELEASE);
    }
    return __real_sceIoClose(fd);
}

int __wrap_sceIoPread(SceUID fd, void *data, SceSize size, SceOff offset) {
    IsoOpen *o = open_find(fd);
    if (o == NULL) {
        return __real_sceIoPread(fd, data, size, offset);
    }
    if (offset < 0) {
        return E_INVAL;
    }
    if (offset >= o->size) {
        return 0;
    }
    if ((SceOff)size > o->size - offset) {
        size = (SceSize)(o->size - offset);
    }
    return __real_sceIoPread(fd, data, size, o->base + offset);
}

SceSSize __wrap_sceIoRead(SceUID fd, void *buf, SceSize nbyte) {
    IsoOpen *o = open_find(fd);
    int got;
    if (o == NULL) {
        return __real_sceIoRead(fd, buf, nbyte);
    }
    if (o->pos >= o->size) {
        return 0;
    }
    if ((SceOff)nbyte > o->size - o->pos) {
        nbyte = (SceSize)(o->size - o->pos);
    }
    got = __real_sceIoPread(fd, buf, nbyte, o->base + o->pos);
    if (got > 0) {
        o->pos += got;
    }
    return got;
}

static SceOff seek(IsoOpen *o, SceOff offset, int whence) {
    SceOff at = whence == SCE_SEEK_SET ? offset : whence == SCE_SEEK_CUR ? o->pos + offset : whence == SCE_SEEK_END ? o->size + offset : -1;
    if (at < 0) {
        return E_INVAL;
    }
    o->pos = at;
    return at;
}

SceOff __wrap_sceIoLseek(SceUID fd, SceOff offset, int whence) {
    IsoOpen *o = open_find(fd);
    return o == NULL ? __real_sceIoLseek(fd, offset, whence) : seek(o, offset, whence);
}

int __wrap_sceIoLseek32(SceUID fd, int offset, int whence) {
    IsoOpen *o = open_find(fd);
    SceOff at;
    if (o == NULL) {
        return __real_sceIoLseek32(fd, offset, whence);
    }
    at = seek(o, offset, whence);
    return at > 0x7FFFFFFF ? E_INVAL : (int)at;
}

int __wrap_sceIoGetstat(const char *file, SceIoStat *stat) {
    int m, i, r;
    if (!lookup(file, &m, &i)) {
        return __real_sceIoGetstat(file, stat);
    }
    if (i == -2) {
        return E_NOENT;
    }
    r = __real_sceIoGetstat(sMount[m].path, stat); /* (the image's own times) */
    if (r < 0) {
        memset(stat, 0, sizeof(*stat));
    }
    fill_stat(stat, i < 0 || sMount[m].ent[i].dir, i < 0 ? 0 : sMount[m].ent[i].size);
    return 0;
}

int __wrap_sceIoGetstatByFd(SceUID fd, SceIoStat *stat) {
    IsoOpen *o = open_find(fd);
    int r = __real_sceIoGetstatByFd(fd, stat);
    if (o != NULL && r >= 0) {
        fill_stat(stat, 0, o->size);
    }
    return r;
}

SceUID __wrap_sceIoDopen(const char *dirname) {
    int m, i, k;
    char up[256];
    char *slash;
    SceUID fd;

    if (!lookup(dirname, &m, &i)) {
        return __real_sceIoDopen(dirname);
    }
    if (i == -2) {
        return E_NOENT;
    }
    if (i >= 0 && !sMount[m].ent[i].dir) {
        return E_NOTDIR;
    }
    snprintf(up, sizeof(up), "%s", sMount[m].path); /* (the folder that holds the image: a real handle for the number) */
    slash = strrchr(up, '/');
    if (slash != NULL && slash[-1] != ':') {
        *slash = '\0';
    } else if (slash != NULL) {
        slash[1] = '\0';
    }
    fd = __real_sceIoDopen(up);
    if (fd < 0) {
        return fd;
    }
    for (k = 0; k < ISO_DIRS && !take_slot(&sDir[k].fd); k++) {
    }
    if (k < ISO_DIRS) {
        sDir[k].mount = m;
        sDir[k].parent = i;
        sDir[k].next = 0;
        __atomic_add_fetch(&sDirN, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&sDir[k].fd, fd, __ATOMIC_RELEASE);
    }
    if (k == ISO_DIRS) {
        __real_sceIoDclose(fd);
        return E_MFILE;
    }
    return fd;
}

int __wrap_sceIoDread(SceUID fd, SceIoDirent *dir) {
    IsoDir *d = dir_find(fd);
    const IsoMount *im;
    const IsoEntry *e;

    if (d == NULL) {
        return __real_sceIoDread(fd, dir);
    }
    im = &sMount[d->mount];
    while (d->next < im->n && im->ent[d->next].parent != d->parent) {
        d->next++;
    }
    if (d->next >= im->n) {
        return 0;
    }
    e = &im->ent[d->next++];
    memset(dir, 0, sizeof(*dir));
    fill_stat(&dir->d_stat, e->dir, e->size);
    snprintf(dir->d_name, sizeof(dir->d_name), "%s", e->name);
    return 1;
}

int __wrap_sceIoDclose(SceUID fd) {
    IsoDir *d = dir_find(fd);
    if (d != NULL) {
        __atomic_store_n(&d->fd, 0, __ATOMIC_RELEASE);
        __atomic_sub_fetch(&sDirN, 1, __ATOMIC_RELEASE);
    }
    return __real_sceIoDclose(fd);
}

/* --- mounting --- */

/* The image at `path` mounted (its files are then <path>/...): 1; 0 not an ISO 9660 image (or no room). Mounting an
   image already mounted: 1. */
int Vita_IsoMount(const char *path_) {
    IsoEntry *ent;
    IsoMount *im;
    int n, m;

    char path[sizeof(im->path)];

    if (!canon(path, path_, sizeof(path))) {
        return 0;
    }
    for (m = 0; m < sMountN; m++) {
        if (strcasecmp(sMount[m].path, path) == 0) {
            return 1;
        }
    }
    if (sMountN >= ISO_MOUNTS || (n = scan_image(path, &ent)) == 0) {
        return 0;
    }
    im = &sMount[sMountN];
    snprintf(im->path, sizeof(im->path), "%s", path);
    im->len = (int)strlen(im->path);
    im->ent = ent;
    im->n = n;
    __atomic_store_n(&sMountN, sMountN + 1, __ATOMIC_RELEASE);
    Vita_Log("iso: %s mounted (%d files and folders)\n", im->path, n);
    return 1;
}

/* (only the last one, before any of its files was opened: an image looked into and not wanted) */
static void unmount_last(void) {
    IsoMount *im = &sMount[sMountN - 1];
    __atomic_store_n(&sMountN, sMountN - 1, __ATOMIC_RELEASE);
    free(im->ent);
    memset(im, 0, sizeof(*im));
}

int Vita_IsoIsMounted(const char *path) {
    int m, i;
    return lookup(path, &m, &i) && i == -1;
}

/* The first .iso in folder `dir` that has `file` (as "SLUS_216.78" or "DATA/PZS3EU1.AFS"): 1, its path in `out`,
   and it stays mounted when `keep`; else 0, and `seen` (when not NULL) counts the .iso files looked into. */
int Vita_IsoFind(const char *dir, const char *file, int keep, char *out, int size, int *seen) {
    SceUID d = __real_sceIoDopen(dir);
    SceIoDirent e;
    int found = 0;

    if (seen != NULL) {
        *seen = 0;
    }
    if (d < 0) {
        return 0;
    }
    memset(&e, 0, sizeof(e));
    while (!found && __real_sceIoDread(d, &e) > 0) {
        size_t l = strlen(e.d_name);
        char path[300], inner[400];
        int was = sMountN, m, i;

        if (SCE_S_ISDIR(e.d_stat.st_mode) || l < 5 || strcasecmp(e.d_name + l - 4, ".iso") != 0 || e.d_name[0] == '.') {
            continue;
        }
        if (seen != NULL) {
            (*seen)++;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, e.d_name);
        if (!Vita_IsoMount(path)) {
            Vita_Log("iso: %s is not a disc image that can be read (ISO 9660)\n", path);
            continue;
        }
        snprintf(inner, sizeof(inner), "%s/%s", path, file);
        if (lookup(inner, &m, &i) && i >= 0 && !sMount[m].ent[i].dir) {
            found = 1;
            snprintf(out, (size_t)size, "%s", path);
            if (!keep && sMountN > was) {
                unmount_last();
            }
        } else {
            Vita_Log("iso: %s has no %s\n", path, file);
            if (sMountN > was) {
                unmount_last();
            }
        }
    }
    __real_sceIoDclose(d);
    return found;
}
