/*
 * A crash's report, without the USB debug link: kubridge's exception handlers (the kernel plugin most homebrew
 * setups carry; libkubridge_stub_weak: without it nothing is registered and the log says so).
 *
 * On a data abort, a prefetch abort or an undefined instruction in any of the game's threads the handler writes
 * <home>/crash.txt (ux0:data/bt3/crash.txt): the kind, the registers, the program counter and the link register as
 * offsets into the program (the unstripped ELF names the function), the fault address, and the words on the stack
 * that point into the program (the call chain, nearest first). Then the process ends: on a Vita in development mode
 * (retail-cp) a crashed game otherwise stays frozen and blocks the next launch, and a test run sees the game gone
 * instead of waiting it out.
 */
#include <kubridge.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void Vita_Log(const char *fmt, ...);

static uint32_t sTextBase, sTextSize;
static char sPath[96];
static char sOut[4096];
static int sLen;
static volatile int sIn;

static void put(const char *fmt, ...) {
    va_list ap;
    int n;
    if (sLen >= (int)sizeof(sOut) - 1) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(sOut + sLen, sizeof(sOut) - (size_t)sLen, fmt, ap);
    va_end(ap);
    if (n > 0) {
        sLen += n;
        if (sLen > (int)sizeof(sOut) - 1) {
            sLen = (int)sizeof(sOut) - 1;
        }
    }
}

static int in_text(uint32_t a) {
    return sTextSize != 0 && a >= sTextBase && a < sTextBase + sTextSize;
}

static void handler(KuKernelExceptionContext *c) {
    static const char *const kKind[3] = {"data abort", "prefetch abort", "undefined instruction"};
    const uint32_t *r = &c->r0;
    char name[32];
    SceKernelThreadInfo ti;
    SceUID fd;
    int i, n = 0;

    if (__atomic_exchange_n(&sIn, 1, __ATOMIC_ACQ_REL)) { /* (a second thread's crash while the first is written) */
        for (;;) {
            sceKernelDelayThread(1000000);
        }
    }
    memset(&ti, 0, sizeof(ti));
    ti.size = sizeof(ti);
    name[0] = '\0';
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) >= 0) {
        snprintf(name, sizeof(name), "%s", ti.name);
    }
    sLen = 0;
    put("crash: %s in thread \"%s\"\n", c->exceptionType < 3 ? kKind[c->exceptionType] : "exception", name);
    put("crash: pc %08x (program +%x)  lr %08x (program +%x)\n", (unsigned)c->pc, in_text(c->pc) ? (unsigned)(c->pc - sTextBase) : 0u,
        (unsigned)c->lr, in_text(c->lr) ? (unsigned)(c->lr - sTextBase) : 0u);
    put("crash: fault address %08x, status %08x, spsr %08x\n", (unsigned)c->FAR, (unsigned)c->FSR, (unsigned)c->SPSR);
    for (i = 0; i < 13; i++) {
        put("crash: r%-2d %08x%s", i, (unsigned)r[i], i % 4 == 3 || i == 12 ? "\n" : " ");
    }
    put("crash: sp %08x; the program's text %08x + %x\n", (unsigned)c->sp, (unsigned)sTextBase, (unsigned)sTextSize);
    put("crash: on the stack, return addresses into the program (offsets), nearest first:");
    if ((c->sp & 3) == 0 && ti.stack != NULL && c->sp >= (uint32_t)ti.stack && c->sp < (uint32_t)ti.stack + (uint32_t)ti.stackSize) {
        const uint32_t *p = (const uint32_t *)c->sp, *end = (const uint32_t *)((uint32_t)ti.stack + (uint32_t)ti.stackSize);
        for (; p < end && n < 48; p++) {
            if (in_text(*p) && (*p & 1)) { /* (Thumb return addresses) */
                put(" +%x", (unsigned)(*p - 1 - sTextBase));
                n++;
            }
        }
    }
    put("\n");
    fd = sceIoOpen(sPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, sOut, (SceSize)sLen);
        sceIoClose(fd);
    }
    sceKernelExitProcess(0);
}

/* At the start (vita_main.c): the handlers, if kubridge is there. `home`: where crash.txt goes. */
void Vita_CrashInit(const char *home) {
    SceKernelModuleInfo mi;
    SceUID mod = sceKernelGetModuleIdByAddr((void *)&Vita_CrashInit);
    int r, k;

    snprintf(sPath, sizeof(sPath), "%s/crash.txt", home);
    memset(&mi, 0, sizeof(mi));
    mi.size = sizeof(mi);
    if (mod >= 0 && sceKernelGetModuleInfo(mod, &mi) >= 0) {
        sTextBase = (uint32_t)mi.segments[0].vaddr;
        sTextSize = (uint32_t)mi.segments[0].memsz;
    }
    for (k = 0; k < 3; k++) {
        r = kuKernelRegisterExceptionHandler((SceUInt32)k, handler, NULL, NULL);
        if (r < 0) {
            Vita_Log("vita: no crash report (kubridge's exception handlers: 0x%08X)\n", (unsigned)r);
            return;
        }
    }
    Vita_Log("vita: a crash is reported in %s (the program's text at %08X, %u KB)\n", sPath, (unsigned)sTextBase, (unsigned)(sTextSize >> 10));
}
