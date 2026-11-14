/* Two copies of this program are two Vitas: the real port/vita/vita_net.c over UDP on this machine.
   t_net <role 1|2> <sessions> <loss percent> <log file>
   Pairing as the versus menu does it (Vita_NetPairBegin / Poll / Start), the "game started again" as a jump,
   the game a loop of blanks that folds the pads it is given into a number. Both logs must be the same. */
#define _GNU_SOURCE
#include "../../vita_net.c"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int sRole, sLoss, sPort;
static FILE *sOut;
static jmp_buf sJb;
static unsigned long long sState;
static unsigned sRng = 12345;
unsigned gPortVBlanks;
volatile int gVitaDialogOn, gVitaNetWaiting;
int gPortFastFloat, gVitaWide = 1, gVitaGameFxOff, gVitaCheatUnlock, gVitaStartMode, gVitaPad2Fake, gVitaVufQuick, gVitaInMenus = 1;
char gVitaDisc[300] = "nowhere";

static unsigned rnd(void) { sRng ^= sRng << 13; sRng ^= sRng >> 17; sRng ^= sRng << 5; return sRng; }

void Vita_Log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); fprintf(stderr, "[%d] ", sRole); vfprintf(stderr, fmt, ap); va_end(ap); }
int psvDebugScreenPrintf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); fprintf(stderr, "[%d screen] ", sRole); vfprintf(stderr, fmt, ap); va_end(ap); return 0; }
void Vita_TextScreen(void) {}
int Vita_DialogCan(void) { return 1; }
static int sClassic;
int Vita_WarmCan(void) { return !sClassic; }
void Vita_WarmRestart(void) { Vita_NetWarm(gPortVBlanks); longjmp(sJb, 1); }
int VitaEu_MatchEnglish(void) { return 3; }
void VitaEu_MatchRestore(int lang) { if (lang != 3) { fprintf(stderr, "language not restored\n"); exit(3); } }
void Save_UnlockAll(void *o) { (void)o; }
void Port_Srand(unsigned s) { (void)s; }
int Port_Rand(void) { return 4; }
void Rand_SeedByArray(unsigned *k, int n) { (void)k; (void)n; }
unsigned long long Port_NetDigest(int strong) { (void)strong; return sState; }
int Port_PadRead(int socket, unsigned char *data) {
    unsigned r = rnd();
    (void)socket;
    memset(data, 0, 18);
    data[0] = (unsigned char)r; data[1] = (unsigned char)(r >> 8);
    data[2] = data[3] = 0x80; data[4] = (unsigned char)(r >> 16); data[5] = (unsigned char)(r >> 24);
    return 1;
}

/* ---- the system ---- */
SceUInt64 sceKernelGetProcessTimeWide(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (SceUInt64)ts.tv_sec * 1000000ull + (SceUInt64)(ts.tv_nsec / 1000); }
int sceKernelDelayThread(SceUInt us) { usleep(us); return 0; }
int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad, int count) { static unsigned n; (void)port; (void)count; pad->buttons = (++n / 5) & 1 ? SCE_CTRL_CROSS : 0; return 1; }
static int sDlg;
SceInt32 sceNetCheckDialogInit(SceNetCheckDialogParam *p) { (void)p; sDlg = 40; return 0; }
SceCommonDialogStatus sceNetCheckDialogGetStatus(void) { return --sDlg <= 0 ? SCE_COMMON_DIALOG_STATUS_FINISHED : SCE_COMMON_DIALOG_STATUS_RUNNING; }
SceInt32 sceNetCheckDialogGetResult(SceNetCheckDialogResult *r) { r->result = SCE_COMMON_DIALOG_RESULT_OK; return 0; }
/* ad hoc: UDP on this machine; a Vita's address is its port */
static int sFd = -1;
int sceNetAdhocctlGetEtherAddr(SceNetEtherAddr *a) { memset(a, 0, sizeof(*a)); a->data[0] = 2; a->data[4] = (uint8_t)(sPort >> 8); a->data[5] = (uint8_t)sPort; return 0; }
int sceNetAdhocPdpCreate(const SceNetEtherAddr *s, SceUShort16 port, unsigned int size, int flag) {
    struct sockaddr_in a;
    (void)s; (void)port; (void)size; (void)flag;
    sFd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)sPort); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sFd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); exit(2); }
    fcntl(sFd, F_SETFL, O_NONBLOCK);
    return 7;
}
int sceNetAdhocPdpDelete(int id, int flag) { (void)id; (void)flag; if (sFd >= 0) { close(sFd); sFd = -1; } return 0; }
int sceNetAdhocPdpSend(int id, const SceNetEtherAddr *d, SceUShort16 port, const void *data, int len, unsigned int timeout, int flag) {
    int k, p0 = 41001, p1 = 41002;
    (void)id; (void)port; (void)timeout; (void)flag;
    if (sFd < 0 || (int)(rnd() % 100) < sLoss) { return 0; }
    if (d->data[0] != 0xFF) { p0 = p1 = (d->data[4] << 8) | d->data[5]; }
    for (k = p0; k <= p1; k++) {
        struct sockaddr_in a;
        if (k == sPort) { continue; }
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_port = htons((uint16_t)k); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(sFd, data, (size_t)len, 0, (struct sockaddr *)&a, sizeof(a));
    }
    return 0;
}
int sceNetAdhocPdpRecv(int id, SceNetEtherAddr *s, SceUShort16 *port, void *buf, int *len, unsigned int timeout, int flag) {
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    int n;
    (void)id; (void)timeout; (void)flag;
    if (sFd < 0) { return -1; }
    n = (int)recvfrom(sFd, buf, (size_t)*len, 0, (struct sockaddr *)&a, &al);
    if (n <= 0) { return -1; }
    memset(s, 0, sizeof(*s)); s->data[0] = 2; s->data[4] = (uint8_t)(ntohs(a.sin_port) >> 8); s->data[5] = (uint8_t)ntohs(a.sin_port);
    *port = NET_PORT;
    *len = n;
    return 0;
}

static void menu(int left) {
    static const int kRules[6] = {2, 1, 0, 1, 1, 0};
    const char *line = NULL;
    unsigned gen = 0, seen = 99999;
    int r, k;
    if (left <= 0) { fprintf(sOut, "done\n"); fclose(sOut); exit(0); }
    if (gVitaStartMode != 0 && gVitaStartMode != 38) { fprintf(stderr, "start mode %d\n", gVitaStartMode); exit(3); }
    for (k = 0; k < 100 + (int)(rnd() % 200); k++) { gPortVBlanks++; usleep(1000); }
    if (!Vita_NetPairBegin(sRole, kRules)) { fprintf(stderr, "[%d] pairing did not begin: %s\n", sRole, sErr); exit(2); }
    while ((r = Vita_NetPairPoll(&line, &gen)) == 0) {
        if (gen != seen) { seen = gen; fprintf(stderr, "[%d menu] %s\n", sRole, line); }
        gPortVBlanks++;
        usleep(2000);
    }
    fprintf(stderr, "[%d menu] %s (%d)\n", sRole, line, r);
    if (r < 0) { exit(2); }
    for (k = 0; k < 18; k++) { gPortVBlanks++; usleep(2000); }
    Vita_NetPairStart();
}

static void match(int n) {
    unsigned t;
    sState = 0xCBF29CE484222325ull;
    fprintf(sOut, "match %d rules %d %d %d %d %d %d me %d\n", n, gVitaNetRules[0], gVitaNetRules[1], gVitaNetRules[2], gVitaNetRules[3], gVitaNetRules[4], gVitaNetRules[5], 0);
    for (t = 1;; t++) {
        unsigned char p[2][18];
        gPortVBlanks++;
        Port_NetBeginTick(gPortVBlanks);
        if (Port_NetTick() != t) { fprintf(stderr, "[%d] tick %u is not %u\n", sRole, Port_NetTick(), t); exit(3); }
        if (t == 150) { Port_NetArrived(); }
        Port_NetInput(0, p[0]);
        Port_NetInput(1, p[1]);
        sState = fnv(sState, p, sizeof(p));
        if (t % 60 == 0) { fprintf(sOut, "%u %016llx\n", t, sState); }
        if (t == 150 + 1500 + (unsigned)n * 300) { Port_NetLeave(); }
        if (t < 150 && sRole == 2) { usleep(4000); } /* (a slower start-up on one Vita) */
        if (rnd() % 8 == 0) { usleep(rnd() % 6000); } else { usleep(300); }
    }
}

int main(int argc, char **argv) {
    static volatile int sLeft, sN;
    sRole = atoi(argv[1]);
    sLeft = atoi(argv[2]);
    sLoss = atoi(argv[3]);
    sOut = fopen(argv[4], "w");
    sPort = 41000 + sRole;
    sRng = (unsigned)time(NULL) * 2654435761u + (unsigned)sRole * 977u;
    setenv("BT3_SAVES", "mine", 1);
    if (getenv("WIDE") != NULL) { gVitaWide = atoi(getenv("WIDE")); }
    sClassic = sRole > 10;
    if (sRole > 10) { /* the start menu's way: found before the game, the match from the program's start */
        sRole -= 10;
        sPort = 41000 + sRole;
        gVitaNetRole = sRole;
        gVitaNetLink = 1;
        Vita_NetConnect();
        match(0);
    }
    for (;;) {
        if (setjmp(sJb) != 0) {
            if (!Port_NetActive()) { /* a match has ended */
                if (strcmp(getenv("BT3_SAVES"), "mine") != 0) { fprintf(stderr, "save folder not restored\n"); exit(3); }
                sLeft--;
            } else if (strcmp(getenv("BT3_SAVES"), "mine") == 0) { fprintf(stderr, "save folder not the match's\n"); exit(3); }
        }
        if (Port_NetActive()) { match(sN++); } else { menu(sLeft); }
    }
}
