/*
 * The Budokai Tenkaichi 4 program (build.py BT3_BT4): what the Vita's platform layer adds for BT4decomp's game code
 * (NauTaro's DBZ-BT4-Tag-Team-Decomp, which runs Team BT4's Budokai Tenkaichi 4 B14 Rev2 mod on Tenkaichi3Decomp).
 *
 * The disc: the player's own BT4 B14 Rev2 image, its files as they are in ux0:data/bt3/bt4/ (SLUS_219.78, BIN/,
 * DATA/). BT4 B14 Rev2 comes in two releases, Spanish and English; which one is there is told by the SHA-1 of its three
 * programs (BT4decomp's port/tools/bt4_disc.py), and the game's text follows it (Port_GameProfileEnglish).
 *
 * The archives: PZS4US0..3.AFS are not AFS archives. Their tables are in BIN/DBZ4.BIN: before the name
 * "pzs4us<n>.afs" a 16-byte header {0, 0x11A + 2 * count, count, count}, and at 0x11A from it a 16-bit count of
 * 2048-byte sectors per member; the members follow one another in the archive (BT4decomp's port/setup/native.cpp).
 *
 * Memory: BT4's fighters and stages need more than the PS2's 32 MB; the game asks the port for such blocks
 * (Port_GameBigAlloc: the characters' packs, a stage larger than the game's buffer) and gives them back through
 * Heap_Free (Port_GameBigFree says which blocks are these).
 */
#include <malloc.h>
#include <psp2/io/fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void Vita_Log(const char *fmt, ...);
extern void Vita_Fatal(const char *fmt, ...);

int gPortVariant = 1; /* (src/sys/file.c: the BT4 mod's file numbers) */

/* ---- SHA-1 (FIPS 180-4) ---- */
typedef struct Sha1 {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    unsigned n;
} Sha1;

static uint32_t rol(uint32_t x, int k) { return x << k | x >> (32 - k); }

static void sha1_block(Sha1 *s, const uint8_t *p) {
    uint32_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], t;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(Sha1 *s) {
    static const uint32_t h0[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    memcpy(s->h, h0, sizeof(h0));
    s->len = 0;
    s->n = 0;
}

static void sha1_add(Sha1 *s, const uint8_t *p, size_t n) {
    s->len += n;
    while (n > 0) {
        unsigned k = 64 - s->n < n ? 64 - s->n : (unsigned)n;
        if (s->n == 0 && n >= 64) {
            sha1_block(s, p);
            p += 64;
            n -= 64;
            continue;
        }
        memcpy(s->buf + s->n, p, k);
        s->n += k;
        p += k;
        n -= k;
        if (s->n == 64) {
            sha1_block(s, s->buf);
            s->n = 0;
        }
    }
}

static void sha1_hex(Sha1 *s, char out[41]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, z = 0, l[8];
    int i;
    sha1_add(s, &pad, 1);
    while (s->n != 56) {
        sha1_add(s, &z, 1);
    }
    for (i = 0; i < 8; i++) {
        l[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha1_add(s, l, 8);
    for (i = 0; i < 5; i++) {
        snprintf(out + i * 8, 9, "%08x", (unsigned)s->h[i]);
    }
}

void Vita_Sha1(const void *data, size_t n, char out[41]) {
    Sha1 s;
    sha1_init(&s);
    sha1_add(&s, data, n);
    sha1_hex(&s, out);
}

/* ---- the disc ---- */
static int sEnglish = 1;
static uint8_t *sIndex;   /* BIN/DBZ4.BIN */
static size_t sIndexSize;
static char sDisc[256];

static uint8_t *load(const char *path, size_t *size) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceOff n;
    uint8_t *p;
    if (fd < 0) {
        return NULL;
    }
    n = sceIoLseek(fd, 0, SCE_SEEK_END);
    p = n > 0 && n < (32 << 20) ? malloc((size_t)n) : NULL;
    if (p != NULL && sceIoPread(fd, p, (SceSize)n, 0) != (int)n) {
        free(p);
        p = NULL;
    }
    sceIoClose(fd);
    *size = (size_t)n;
    return p;
}

/* vita_mem.c Vita_LoadGameData, with SLUS_219.78 read: which release (Spanish or English) the three programs are.
   `image`: SLUS_219.78's loaded sections as one image from 0x100000 to 0x2FF180, as BT4decomp hashes it. */
void Vita_Bt4CheckProgram(const char *disc, const uint8_t *image, size_t size) {
    /* SLUS_219.78 (its image), BIN/DBZP.BIN, BIN/DBZ4.BIN of each release (BT4decomp port/tools/bt4_disc.py) */
    static const char *const kProfiles[2][3] = {
        {"2ae9c636de479c01d2b4e9906c25b7ca807e80d9", "de91913f40bd675d70ee748aa16a42a96dff5919", "989ee93cbd31508717bc245793c1e89c50109e0f"},
        {"a4776fbadd2d266f42cf559a3530c2e6454d01a3", "1d9f8da80c200bb50e7df3210fff30827e5dae9c", "145837274421c39ae7eedc3fa1b697d4212541c2"}};
    char sum[3][41], path[300];
    uint8_t *menu;
    size_t menuSize = 0;
    int p;

    snprintf(sDisc, sizeof(sDisc), "%s", disc);
    Vita_Sha1(image, size, sum[0]);
    snprintf(path, sizeof(path), "%s/BIN/DBZP.BIN", disc);
    menu = load(path, &menuSize);
    if (menu == NULL) {
        Vita_Fatal("BT4's menu program was not found:\n  %s", path);
    }
    Vita_Sha1(menu, menuSize, sum[1]);
    free(menu);
    snprintf(path, sizeof(path), "%s/BIN/DBZ4.BIN", disc);
    sIndex = load(path, &sIndexSize);
    if (sIndex == NULL) {
        Vita_Fatal("BT4's archive index was not found:\n  %s", path);
    }
    Vita_Sha1(sIndex, sIndexSize, sum[2]);
    for (p = 0; p < 2; p++) {
        if (strcmp(sum[0], kProfiles[p][0]) == 0 && strcmp(sum[1], kProfiles[p][1]) == 0 && strcmp(sum[2], kProfiles[p][2]) == 0) {
            sEnglish = p;
            Vita_Log("bt4: Budokai Tenkaichi 4 B14 Rev2, the %s release\n", p ? "English" : "Spanish");
            return;
        }
    }
    Vita_Log("bt4: SLUS_219.78 %s, DBZP.BIN %s, DBZ4.BIN %s\n", sum[0], sum[1], sum[2]);
    Vita_Fatal("The files in\n  %s\nare not those of Budokai Tenkaichi 4 B14 Rev2\n(its English or Spanish release, unmodified).", disc);
}

/* vita_file.c: the table of archive "pzs4us<n>" (offset, size per member; malloc), or NULL when `name` is not one */
uint32_t *Vita_Bt4Table(const char *name, uint32_t *count, uint32_t fileSize) {
    char key[16];
    size_t at, i;
    uint32_t head[4], *t, off = 0;
    if (strncmp(name, "pzs4us", 6) != 0 || name[6] < '0' || name[6] > '3' || name[7] != '\0' || sIndex == NULL) {
        return NULL;
    }
    snprintf(key, sizeof(key), "%s.afs", name);
    for (at = 16; at + strlen(key) + 1 <= sIndexSize; at++) {
        if (memcmp(sIndex + at, key, strlen(key) + 1) == 0) {
            break;
        }
    }
    if (at + strlen(key) + 1 > sIndexSize) {
        Vita_Fatal("BT4's archive index has no table for %s", key);
    }
    at -= 16;
    memcpy(head, sIndex + at, 16);
    if (head[0] != 0 || head[2] != head[3] || head[1] != 0x11A + head[2] * 2 || at + head[1] > sIndexSize || head[2] == 0) {
        Vita_Fatal("BT4's archive index: the table of %s is not as expected", key);
    }
    *count = head[2];
    t = malloc((size_t)head[2] * 8);
    if (t == NULL) {
        Vita_Fatal("out of memory");
    }
    for (i = 0; i < head[2]; i++) {
        uint16_t sectors;
        memcpy(&sectors, sIndex + at + 0x11A + i * 2, 2);
        t[i * 2] = off;
        t[i * 2 + 1] = (uint32_t)sectors * 2048u;
        if (off + t[i * 2 + 1] > fileSize) { /* (the last member: the archive ends inside its last sector) */
            t[i * 2 + 1] = off < fileSize ? fileSize - off : 0;
        }
        off += (uint32_t)sectors * 2048u;
    }
    Vita_Log("bt4: %s: %u members, %u MB (from DBZ4.BIN)\n", key, (unsigned)head[2], (unsigned)(off >> 20));
    return t;
}

/* ---- the game's profile (BT4decomp port/src/gs/game_profile.c): one release installed, its language ---- */
int Port_GameProfileEnglish(void) { return sEnglish; }
int Port_GameProfileLanguage(void) { return sEnglish; }
int Port_GameProfilePortuguese(void) { return 0; }
int Port_GameProfileAvailable(int language) { return language == sEnglish; }
int Port_GameProfilePrompt(void) { return 0; }
int Port_GameProfileChoose(int english) { (void)english; return 0; }
int Port_GameProfileMenuActive(void) { return 0; }
void Port_GameProfileMenuSet(int active, int choice, int alpha) { (void)active; (void)choice; (void)alpha; }

/* ---- memory beyond the game's heap ---- */
#define BIG_SLOTS 2048 /* open addressing over the blocks' addresses */
static uintptr_t sBig[BIG_SLOTS];
static unsigned sBigCount, sBigTotal; /* (blocks in use; bytes ever given, for the log) */

static unsigned big_slot(uintptr_t a) { return (unsigned)((a >> 6) * 2654435761u) & (BIG_SLOTS - 1); }

unsigned Port_GameBigAlloc(int size) {
    void *p;
    unsigned k;
    if (size <= 0 || sBigCount >= BIG_SLOTS * 3 / 4) {
        Vita_Log("bt4: a large block of %d bytes refused (%u blocks in use)\n", size, sBigCount);
        return 0;
    }
    p = memalign(64, (size_t)size);
    if (p == NULL) {
        Vita_Log("bt4: out of memory for a large block of %d bytes (%u blocks in use)\n", size, sBigCount);
        return 0;
    }
    for (k = big_slot((uintptr_t)p); sBig[k] != 0 && sBig[k] != (uintptr_t)-1; k = (k + 1) & (BIG_SLOTS - 1)) {
    }
    sBig[k] = (uintptr_t)p;
    sBigCount++;
    sBigTotal += (unsigned)size;
    return (unsigned)(uintptr_t)p;
}

int Port_GameBigFree(unsigned addr) {
    unsigned k;
    if (addr == 0 || sBigCount == 0) {
        return 0;
    }
    for (k = big_slot(addr); sBig[k] != 0; k = (k + 1) & (BIG_SLOTS - 1)) {
        if (sBig[k] == addr) {
            sBig[k] = (uintptr_t)-1; /* (a tombstone: the probe goes on past it) */
            sBigCount--;
            free((void *)(uintptr_t)addr);
            return 1;
        }
    }
    return 0;
}
