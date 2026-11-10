/*
 * The C library's per-thread lookups the short way (as Grarak's mh3-wii port, runtime/vita/fast_tls.c).
 *
 * VitaSDK's newlib finds a thread's struct _reent (errno, malloc's and stdio's state) through
 * __getreent -> __getreent_for_thread(0), which saves eight registers and takes 136 bytes of stack even when the
 * thread's reent is already made; pthread-embedded's pthread_getspecific / pthread_self (every condition wait,
 * every pthread_testcancel) goes pte_osTlsGetValue -> vitasdk_get_tls_data -> __getreent_for_thread the same way.
 * Both read the thread's TLS slot 0x89 (newlib's threading.c), which holds the reent; the keys' list
 * (pthread-embedded's vita_osal.c: nodes {next, key, value}) starts in the word 8 bytes before it.
 *
 * The wraps (build.py: --wrap=__getreent --wrap=pte_osTlsGetValue) read the same slot and the same list, and leave a
 * thread whose reent is not made yet to the SDK's own functions (they make it). Nothing is cached or kept: the SDK
 * still makes and frees everything. The layouts are VitaSDK's private ones, checked against its libc.a and
 * libpthread.a (libc_a-threading.o, vita_osal.o): see them again when the SDK changes.
 */
#include <psp2/kernel/threadmgr.h>
#include <reent.h>
#include <stddef.h>
#include <string.h>

enum { NEWLIB_REENT_TLS_KEY = 0x89 };

typedef struct TlsKeyNode {
    struct TlsKeyNode *next;
    unsigned int key;
    void *value;
} TlsKeyNode;

_Static_assert(sizeof(void *) == 4, "the SDK's TLS layout is a 32-bit one");
_Static_assert(offsetof(TlsKeyNode, key) == 4 && offsetof(TlsKeyNode, value) == 8 && sizeof(TlsKeyNode) == 12,
               "pthread-embedded's key node");

extern struct _reent *__real___getreent(void);
extern void *__real_pte_osTlsGetValue(unsigned int key);

static inline struct _reent *reent_now(void) {
    return *(struct _reent **)sceKernelGetTLSAddr(NEWLIB_REENT_TLS_KEY);
}

struct _reent *__wrap___getreent(void) {
    struct _reent *r = reent_now();
    return r != NULL ? r : __real___getreent();
}

void *__wrap_pte_osTlsGetValue(unsigned int key) {
    struct _reent *r = reent_now();
    TlsKeyNode *n;
    if (r == NULL) {
        return __real_pte_osTlsGetValue(key);
    }
    memcpy(&n, (const char *)r - 2 * sizeof(void *), sizeof(n)); /* (vitasdk_get_tls_data: reent - 8, its first word) */
    for (; n != NULL; n = n->next) {
        if (n->key == key) {
            return n->value;
        }
    }
    return NULL;
}
