/*
 * Memory of the PS Vita build (port/src/plat_mem.c is the PC's).
 *
 * The PC build gives the game memory at the PS2's own addresses. A Vita program cannot choose where its memory
 * is, and it does not have to: the game takes its heap with one malloc and keeps ordinary 32-bit pointers, which
 * is what the Vita has. So here
 *   - the game heap is one block from the system (28.3 MB, the size of the PS2's region), wherever it lands;
 *   - the hardware registers are two arrays (the game's register macros go through PORT_HW, port_compat.h);
 *   - the data tables of the game's two programs are filled in from the user's own files at start
 *     (Vita_LoadGameData), through the table of addresses build.py generates (vita_dat.c).
 * Every address in a Vita program has its top bit set (0x81000000 and up). The PS2's never do, and nothing in the
 * PC builds has ever run that way (they keep everything below 2 GB): game code that treats a pointer as a signed
 * number or keeps a flag in its top bit would show here first. heap=cdram (bt3.txt) asks for the heap in the
 * graphics memory area instead, which lies lower; an experiment for that case, not a fix.
 */
#include <psp2/io/fcntl.h>
#include <psp2/kernel/sysmem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vita_dat.h"

#define HEAP_SIZE (0x02000000u - 0x003BE000u)
#define HEAP_SKIP 0x730u /* the PS2's first address is 0x3BE730: keep its position within a page */

unsigned char gPortHw[0x10000] __attribute__((aligned(16)));   /* stands for 0x10000000..0x1000FFFF */
unsigned char gPortGsReg[0x2000] __attribute__((aligned(16))); /* stands for 0x12000000..0x12001FFF */
volatile uint32_t gPortDataStripped = 1;                       /* the tables come from the disc, always */
uint32_t gVitaHeapBase, gVitaHeapUsed;

static uint32_t sHeapNext, sHeapEnd;
extern void Vita_Fatal(const char *fmt, ...);

void Vita_MapMemory(int cdram) {
    uint32_t size = (HEAP_SIZE + 0x3FFFFu) & ~0x3FFFFu; /* graphics memory comes in 256 KB steps */
    SceUID id = sceKernelAllocMemBlock("bt3_heap", cdram ? SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                       size, NULL);
    void *base = NULL;

    if (id < 0 || sceKernelGetMemBlockBase(id, &base) < 0 || base == NULL) {
        Vita_Fatal("no memory for the game heap (%u MB, %s): error 0x%08X", size >> 20, cdram ? "graphics memory" : "main memory", (unsigned)id);
    }
    memset(base, 0, size);
    gVitaHeapBase = (uint32_t)(uintptr_t)base;
    sHeapNext = gVitaHeapBase + HEAP_SKIP;
    sHeapEnd = gVitaHeapBase + HEAP_SIZE;
}

/* The game's malloc (port_compat.h renames it): a bump allocator over the heap block, 16-byte aligned. */
void *Port_Malloc(uint32_t size) {
    uint32_t p = (sHeapNext + 15) & ~15u;

    if (size > sHeapEnd - p) {
        Vita_Fatal("malloc(0x%X) does not fit in the game heap", size);
    }
    sHeapNext = p + size;
    gVitaHeapUsed = sHeapNext - gVitaHeapBase;
    return (void *)(uintptr_t)p;
}

/* Memory the port hands to the game by address (file handles, the "second processor's" memory): zeroed. The blocks
   are on a list, so that those the game still held can be let go when it starts again inside the running program
   (Vita_MemWarm). The game's thread only. */
typedef struct Low {
    struct Low *prev, *next;
    uint32_t size, pad; /* (16 bytes: the block keeps malloc's alignment) */
} Low;
static Low sLow = {&sLow, &sLow, 0, 0};

void *Port_LowAlloc(size_t size) {
    Low *l = calloc(1, sizeof(Low) + size);

    if (l == NULL) {
        Vita_Fatal("out of memory (%u bytes)", (unsigned)size);
    }
    l->size = (uint32_t)size;
    l->next = &sLow;
    l->prev = sLow.prev;
    sLow.prev->next = l;
    sLow.prev = l;
    return l + 1;
}

void Port_LowFree(void *addr) {
    if (addr != NULL) {
        Low *l = (Low *)addr - 1;
        l->prev->next = l->next;
        l->next->prev = l->prev;
        free(l);
    }
}

/* (vita_warm.c) the game starts again inside the running program: its memory as a start of the program finds it.
   The heap block empty and all zero, the hardware registers' arrays zero, the blocks handed out by address let go.
   Returns how many of those there were. */
int Vita_MemWarm(void) {
    int n = 0;
    while (sLow.next != &sLow) {
        Port_LowFree(sLow.next + 1);
        n++;
    }
    memset((void *)(uintptr_t)gVitaHeapBase, 0, HEAP_SIZE);
    sHeapNext = gVitaHeapBase + HEAP_SKIP;
    gVitaHeapUsed = 0;
    memset(gPortHw, 0, sizeof(gPortHw));
    memset(gPortGsReg, 0, sizeof(gPortGsReg));
    return n;
}

static uint8_t *read_file(const char *path, size_t *size) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    uint8_t *buf;
    SceOff n;

    if (fd < 0) {
        return NULL;
    }
    n = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    buf = n > 0 && n < 0x4000000 ? malloc((size_t)n) : NULL;
    if (buf == NULL || sceIoRead(fd, buf, (SceSize)n) != (int)n) {
        sceIoClose(fd);
        free(buf);
        return NULL;
    }
    sceIoClose(fd);
    *size = (size_t)n;
    return buf;
}

/* Copies the values of the game's data tables in from <disc>/SLUS_216.78 (its loaded sections as one image from
   0x100000) and <disc>/BIN/DBZP.BIN, and checks them against the checksum of port/data: the same step as
   Port_LoadGameData of the PC build, with the destinations as addresses the Vita's loader has already placed. */
#include "vita_home.h"
#ifdef BT3_BT4
extern void Vita_Bt4CheckProgram(const char *disc, const uint8_t *elf, size_t size);
#endif
#define ROM_BASE 0x100000u
#define ROM_END 0x2FF180u

void Vita_LoadGameData(const char *disc) {
    char path[300];
    uint8_t *elf, *src[2] = {NULL, NULL};
    size_t elfSize = 0, srcSize[2] = {0, 0};
    uint64_t hash = 0xCBF29CE484222325ull;
    uint32_t i, k;

    snprintf(path, sizeof(path), "%s/" VITA_PROGRAM, disc);
    elf = read_file(path, &elfSize);
    if (elf == NULL || elfSize < 0x34 || memcmp(elf, "\177ELF", 4) != 0) {
        Vita_Fatal("the game's program was not found:\n  %s\nCopy the files of your own disc there (see the README).", path);
    }
    {
        uint32_t shoff, sh[6];
        uint16_t shentsize, shnum;
        memcpy(&shoff, elf + 0x20, 4);
        memcpy(&shentsize, elf + 0x2E, 2);
        memcpy(&shnum, elf + 0x30, 2);
        srcSize[0] = ROM_END - ROM_BASE;
        src[0] = calloc(1, srcSize[0]);
        if (src[0] == NULL) {
            Vita_Fatal("out of memory");
        }
        for (i = 0; i < shnum && (size_t)shoff + (size_t)(i + 1) * shentsize <= elfSize; i++) {
            memcpy(sh, elf + shoff + (size_t)i * shentsize, sizeof(sh)); /* name, type, flags, addr, offset, size */
            if ((sh[2] & 2) && sh[1] != 8 && sh[5] != 0 && sh[3] >= ROM_BASE && sh[3] + sh[5] <= ROM_END &&
                (size_t)sh[4] + sh[5] <= elfSize) {
                memcpy(src[0] + (sh[3] - ROM_BASE), elf + sh[4], sh[5]);
            }
        }
        free(elf);
#ifdef BT3_BT4
        Vita_Bt4CheckProgram(disc, src[0], srcSize[0]); /* (which of the two releases of BT4 B14 Rev2: vita_bt4.c) */
#endif
    }
    snprintf(path, sizeof(path), "%s/BIN/DBZP.BIN", disc);
    src[1] = read_file(path, &srcSize[1]);
    if (src[1] == NULL) {
        Vita_Fatal("the game's menu program was not found:\n  %s", path);
    }
    for (i = 0; i < gPortDatCount; i++) {
        const PortDatRun *r = &gPortDatRuns[i];
        const uint8_t *from;

        if (r->source > 1 || (size_t)r->offset + r->length > srcSize[r->source]) {
            Vita_Fatal("the list of the game's data does not fit the files in\n  %s", disc);
        }
        from = src[r->source] + r->offset;
        memcpy(r->dst, from, r->length);
        for (k = 0; k < r->length; k++) {
            hash = (hash ^ from[k]) * 0x100000001B3ull;
        }
    }
    free(src[0]);
    free(src[1]);
#ifndef BT3_BT4 /* (BT4: its programs were checked whole above) */
    if (hash != gPortDatChecksum) {
        Vita_Fatal("SLUS_216.78 / BIN/DBZP.BIN in\n  %s\nare not the unmodified USA release (SLUS-21678).", disc);
    }
#endif
}

/* The program's end address on the PS2: a table of the PS2's C library points at it; nothing reads it here
   (src/sys/heap.c takes the number itself in this build). */
unsigned char D_3BE71C[16];
