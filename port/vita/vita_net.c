/*
 * Multiplayer: two Vitas playing the game's own two-player versus mode together, over ad hoc (Vita to Vita, no
 * router needed) or over the Wi-Fi network both are on. The design is z3xox's online play of the PC version
 * (port/src/gs/net.c there, "lockstep"), made again for the Vita:
 *
 *   - Both Vitas run the whole game from the same start and stay in step by exchanging the players' buttons: at
 *     every vertical blank each sends its own player's input for the blank `delay` blanks later, and goes on only
 *     once it has the other player's for this blank. Both read the same two pads and compute the same game.
 *     Nothing is predicted and nothing is rewound: a slow line makes both wait.
 *   - The same start: a match is a fresh start of the game on both (the start menu, SELECT held while the game
 *     starts > Multiplayer), with a save of its own that starts empty (ux0:data/bt3/net_saves), everything in it
 *     unlocked by the game's own Save_UnlockAll, the host's random seed, no mods, no added stages or songs, the
 *     USA disc's text and voices, no logos or opening film, and straight into the versus mode's character select.
 *     The start-up runs on each Vita by itself with nothing pressed; the two meet at the character select (both
 *     must arrive there at the same blank).
 *   - What would come out differently on two Vitas is counted the same way on both: a sound stream lasts as many
 *     blanks as its file says (snd_adx.c), not as long as it takes to be heard; the archives' no-wait reads are
 *     made at once (vita_file.c); the game-side switches of bt3.txt and the settings that change the game are
 *     the game's own (Vita_NetSetting).
 *   - Every SYNC_EVERY blanks each sends a number made of the game's state (Port_NetDigest, headless.c); two that
 *     differ go to the log ("the two games no longer agree").
 *   - Each Vita shows its own player's view of the split-screen fight full screen (Port_NetView: btl_cam.c,
 *     btl_scene.c, eft_aa.c), as the PC version does.
 *   - The match ends at the main menu (back from the versus menu), when the other player leaves, or when the line
 *     is lost (20 seconds without a word); the game then starts again as it normally is, with the player's own save.
 *   - From the versus menu (Multiplayer > Ad Hoc: Host / Join) all of it happens in the running game: the system's
 *     dialog is drawn over the menu, the menu's message window says who is waited for (Vita_NetPairBegin / Poll),
 *     and the "fresh start of the game" is the game alone started again inside the program (vita_warm.c), on both
 *     Vitas, with the connection kept. The match's end starts the game once more the same way, as it normally is,
 *     at the versus menu. (The start menu's way, and bt3.txt netwarm=0: the program itself starts again.)
 *
 * The line: ad hoc is the PSP's ad hoc mode as the Vita has it (the system's own dialog connects the two Vitas,
 * then PDP sockets; the order of calls as in Rinnegatamante's vitaQuake), Wi-Fi is UDP on the local network. The host
 * says so by broadcast a few times a second; the one who joins picks it from a list.
 *
 * Testing on one Vita (development build): bt3.txt nettest=<seconds> runs a match with no line, both players'
 * buttons from a fixed script and the game's thread held up at random moments, and writes the game's state every
 * second into ux0:data/bt3/net_test.txt. Two such runs must write the same file: then nothing in the game depends on
 * how fast each Vita happens to be.
 */
#include <psp2/apputil.h>
#include <psp2/appmgr.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/netcheck_dialog.h>
#include <psp2/pspnet_adhoc.h>
#include <psp2/pspnet_adhocctl.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vita_home.h"

extern void Vita_Log(const char *fmt, ...);
extern int psvDebugScreenPrintf(const char *fmt, ...);
extern int Port_PadRead(int socket, unsigned char *data); /* vita_gfx.c: the Vita's own buttons */
extern unsigned long long Port_NetDigest(int strong);     /* headless.c */
extern void Vita_TextScreen(void);                         /* vita_vgl.c */

#define HOME VITA_HOME
#define NET_SAVES HOME "/net_saves"
#define NET_LOG HOME "/net_test.txt"

enum { MAGIC = 0x56335442 /* "BT3V" */, NET_VERSION = 1, NET_PORT = 18630, PAD = 18, RING = 1024, REDUNDANT = 12,
       SYNC_EVERY = 30, SYNC_SLOTS = 16, NAME_LEN = 24, PKT_MAX = 512 };
enum { T_OFFER = 1, T_JOIN, T_WELCOME, T_REFUSE, T_READY, T_ALIVE, T_ARRIVED, T_INPUT, T_BYE };
enum { WHY_VERSION = 1, WHY_DATA = 2, WHY_BUSY = 3, WHY_FLOAT = 4 };
enum { BYE_LEFT = 1, BYE_LOST = 2, BYE_APART = 3, BYE_QUIT = 4, BYE_OVER = 5 /* both came to the match's end */ };
enum { LINK_NONE = 0, LINK_ADHOC = 1, LINK_WIFI = 2 };

static const uint8_t kIdle[PAD] = {0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80};

/* ------------------------------------------------------------------------------------------------- what was chosen */
int gVitaNetRole;     /* the start menu's choice: 0 none, 1 host (player 1), 2 join (player 2) */
int gVitaNetLink;     /* LINK_ADHOC or LINK_WIFI */
/* (the host) the battle settings of the match, the save's rule[0..5] (Duel Time: 0..4 = 60, 90, 180, 240 s, none; COM
   Level; Referee; Transformations, two; Map Setting); -1: the game's own default. From the start menu (the time) or
   from the versus menu's Multiplayer (all six, the host's own settings: Vita_NetRestartInto). */
int gVitaNetRules[6] = {-1, -1, -1, -1, -1, -1};
int gVitaNetDelay = -1; /* bt3.txt netdelay=<blanks> (testing): the input delay the host asks for */
int gVitaNetTest;     /* (development build) bt3.txt nettest=<seconds>: the one-Vita test of the match's sameness */

static int sOn;       /* the game runs as a match (from its start) */
static int sWarm;     /* the match began in the running game (the versus menu): the game was started again inside the
                         program for it (vita_warm.c), and is once more when it ends */
static int sPending;  /* what the game's next start inside the program is: 1 a match begins, 2 a match has ended */
static uint32_t sTickBase; /* the blank the match's own count of blanks starts from (0: the program's start) */
extern int Vita_WarmCan(void);
extern void Vita_WarmRestart(void);
static int sMe;       /* this Vita's player: 0 (the host) or 1 */
static int sConnected;
static uint32_t sSession;
static unsigned sSeed;
static int sDelay = 2;
static char sMyName[NAME_LEN] = "Player", sTheirName[NAME_LEN] = "the other player";
static unsigned long long sMyBuild, sMyData;
static uint32_t sJoinNonce; /* (the one who joins: its greeting's number, answered again if the host asks once more) */

/* ------------------------------------------------------------------------------------------------- small helpers */
static uint64_t now_us(void) {
    return sceKernelGetProcessTimeWide();
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static uint64_t fnv(uint64_t h, const void *data, size_t n) {
    const uint8_t *b = data;
    size_t i;
    for (i = 0; i < n; i++) {
        h = (h ^ b[i]) * 0x100000001B3ull;
    }
    return h;
}

static unsigned pad_now(void) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    return pad.buttons;
}

/* ------------------------------------------------------------------------------------------------- the line */
typedef struct { uint8_t mac[6]; uint16_t port; uint32_t ip; } NetAddr;
static int sLink, sSock = -1, sNetInit, sAdhocInit;
static SceUID sPoolId = -1;
static NetAddr sPeer, sMine;
static char sErr[240];
static char sMyIp[16];

static int same_addr(const NetAddr *a, const NetAddr *b) {
    return sLink == LINK_ADHOC ? memcmp(a->mac, b->mac, 6) == 0 && a->port == b->port : a->ip == b->ip && a->port == b->port;
}

static int net_base(void) {
    SceNetInitParam p;
    void *base = NULL;
    int r;

    if (sNetInit) {
        return 1;
    }
    if ((r = sceSysmoduleLoadModule(SCE_SYSMODULE_NET)) < 0) {
        snprintf(sErr, sizeof(sErr), "the system's network module did not load (%08x)", (unsigned)r);
        return 0;
    }
    sPoolId = sceKernelAllocMemBlock("bt3_net", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 1024 * 1024, NULL);
    if (sPoolId < 0 || sceKernelGetMemBlockBase(sPoolId, &base) < 0) {
        snprintf(sErr, sizeof(sErr), "no memory for the network (%08x)", (unsigned)sPoolId);
        return 0;
    }
    memset(&p, 0, sizeof(p));
    p.memory = base;
    p.size = 1024 * 1024;
    p.flags = 0;
    r = sceNetInit(&p);
    if (r < 0 && (unsigned)r != 0x80410110u /* (already: SCE_NET_ERROR_EBUSY) */) {
        snprintf(sErr, sizeof(sErr), "the network did not start (sceNetInit %08x)", (unsigned)r);
        return 0;
    }
    r = sceNetCtlInit();
    Vita_Log("net: network started (netctl %08x)\n", (unsigned)r);
    sNetInit = 1;
    return 1;
}

/* The minimal display the system's dialog draws on (as VitaSDK's ime sample): two screen buffers, cleared on the
   processor. Made only for the dialog, before the game's renderer exists, and taken down after it. */
#define DLG_W 960
#define DLG_H 544
#define DLG_STRIDE 1024
#define DLG_BYTES ((4 * DLG_STRIDE * DLG_H + 0x3FFFF) & ~0x3FFFF)
static void *sDlgBuf[2];
static SceUID sDlgId[2] = {-1, -1};
static SceGxmSyncObject *sDlgSync[2];
static SceGxmColorSurface sDlgSurf[2];

static void dlg_vsync(const void *data) {
    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = *(void *const *)data;
    fb.pitch = DLG_STRIDE;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = DLG_W;
    fb.height = DLG_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static int dlg_gxm_up(void) {
    SceGxmInitializeParams p;
    int i, r;

    memset(&p, 0, sizeof(p));
    p.flags = 0;
    p.displayQueueMaxPendingCount = 1;
    p.displayQueueCallback = dlg_vsync;
    p.displayQueueCallbackDataSize = sizeof(void *);
    p.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    if ((r = sceGxmInitialize(&p)) < 0) {
        snprintf(sErr, sizeof(sErr), "the screen for the system's dialog could not be made (%08x)", (unsigned)r);
        return 0;
    }
    for (i = 0; i < 2; i++) {
        sDlgId[i] = sceKernelAllocMemBlock("bt3_dlg", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, DLG_BYTES, NULL);
        if (sDlgId[i] < 0 || sceKernelGetMemBlockBase(sDlgId[i], &sDlgBuf[i]) < 0) {
            snprintf(sErr, sizeof(sErr), "no video memory for the system's dialog (%08x)", (unsigned)sDlgId[i]);
            return 0;
        }
        sceGxmMapMemory(sDlgBuf[i], DLG_BYTES, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
        memset(sDlgBuf[i], 0, 4 * DLG_STRIDE * DLG_H);
        sceGxmColorSurfaceInit(&sDlgSurf[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                               SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, DLG_W, DLG_H, DLG_STRIDE, sDlgBuf[i]);
        sceGxmSyncObjectCreate(&sDlgSync[i]);
    }
    return 1;
}

static void dlg_gxm_down(void) {
    int i;
    sceGxmDisplayQueueFinish();
    for (i = 0; i < 2; i++) {
        if (sDlgSync[i] != NULL) {
            sceGxmSyncObjectDestroy(sDlgSync[i]);
            sDlgSync[i] = NULL;
        }
        if (sDlgId[i] >= 0) {
            sceGxmUnmapMemory(sDlgBuf[i]);
            sceKernelFreeMemBlock(sDlgId[i]);
            sDlgId[i] = -1;
        }
    }
    sceGxmTerminate();
}

/* The system's application services (the user's name; the system's dialogs), once. */
static void app_util(void) {
    static int done;
    if (!done) {
        SceAppUtilInitParam ip;
        SceAppUtilBootParam bp;
        SceCommonDialogConfigParam cp;
        int r;
        memset(&ip, 0, sizeof(ip));
        memset(&bp, 0, sizeof(bp));
        r = sceAppUtilInit(&ip, &bp);
        sceCommonDialogConfigParamInit(&cp);
        sceCommonDialogSetConfigParam(&cp);
        Vita_Log("net: app utility %08x\n", (unsigned)r);
        done = 1;
    }
}

/* The system's ad hoc dialog (PSP ad hoc, as vitaQuake opens it): both Vitas join the same group. 1: connected. */
static int adhoc_dialog(void) {
    SceDisplayFrameBuf text;
    SceNetCheckDialogParam p;
    SceNetAdhocctlGroupName group;
    SceNetCheckDialogResult res;
    int back = 0, front = 1, r, ok = 0;

    app_util();
    memset(&text, 0, sizeof(text));
    text.size = sizeof(text);
    sceDisplayGetFrameBuf(&text, SCE_DISPLAY_SETBUF_NEXTFRAME); /* (the text screen, put back after) */
    if (!dlg_gxm_up()) {
        dlg_gxm_down();
        sceDisplaySetFrameBuf(&text, SCE_DISPLAY_SETBUF_NEXTFRAME);
        return 0;
    }
    sceNetCheckDialogParamInit(&p);
    memset(&group, 0, sizeof(group));
    memcpy(group.data, "BT3VITA", 7);
    p.groupName = &group;
    memcpy(p.npCommunicationId.data, "BT3VITA00", 9);
    p.npCommunicationId.term = '\0';
    p.npCommunicationId.num = 0;
    p.mode = SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CONN;
    p.timeoutUs = 0;
    r = sceNetCheckDialogInit(&p);
    Vita_Log("net: the ad hoc dialog: %08x\n", (unsigned)r);
    if (r < 0) {
        snprintf(sErr, sizeof(sErr), "the system's ad hoc dialog did not open (%08x)", (unsigned)r);
    } else {
        for (;;) {
            SceCommonDialogUpdateParam up;
            int st = sceNetCheckDialogGetStatus();
            if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
                break;
            }
            memset(sDlgBuf[back], 0, 4 * DLG_STRIDE * DLG_H);
            memset(&up, 0, sizeof(up));
            up.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
            up.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
            up.renderTarget.width = DLG_W;
            up.renderTarget.height = DLG_H;
            up.renderTarget.strideInPixels = DLG_STRIDE;
            up.renderTarget.colorSurfaceData = sDlgBuf[back];
            up.renderTarget.depthSurfaceData = NULL;
            up.displaySyncObject = sDlgSync[back];
            sceCommonDialogUpdate(&up);
            sceGxmPadHeartbeat(&sDlgSurf[back], sDlgSync[back]);
            sceGxmDisplayQueueAddEntry(sDlgSync[front], sDlgSync[back], &sDlgBuf[back]);
            front = back;
            back ^= 1;
            sceDisplayWaitVblankStart();
        }
        memset(&res, 0, sizeof(res));
        sceNetCheckDialogGetResult(&res);
        sceNetCheckDialogTerm();
        Vita_Log("net: the ad hoc dialog ended: result %d\n", res.result);
        ok = res.result == SCE_COMMON_DIALOG_RESULT_OK;
        if (!ok) {
            snprintf(sErr, sizeof(sErr), res.result == SCE_COMMON_DIALOG_RESULT_USER_CANCELED ? "the ad hoc connection was cancelled" :
                     "the ad hoc connection was not made (result %d)", res.result);
        }
    }
    dlg_gxm_down();
    sceDisplaySetFrameBuf(&text, SCE_DISPLAY_SETBUF_NEXTFRAME);
    return ok;
}

/* Ad hoc in three steps: the system's ad hoc services started (adhoc_up), the two Vitas connected by the system's
   dialog (adhoc_dialog above, or drawn over the versus menu: pair_*), this Vita's socket (adhoc_socket). */
static int adhoc_up(void) {
    SceNetAdhocctlAdhocId id;
    int r;

    if (sAdhocInit) {
        return 1;
    }
    if ((r = sceSysmoduleLoadModule(SCE_SYSMODULE_PSPNET_ADHOC)) < 0) {
        snprintf(sErr, sizeof(sErr), "the system's ad hoc module did not load (%08x)", (unsigned)r);
        return 0;
    }
    r = sceNetAdhocInit();
    Vita_Log("net: ad hoc %08x\n", (unsigned)r);
    memset(&id, 0, sizeof(id));
    id.type = SCE_NET_ADHOCCTL_ADHOCTYPE_RESERVED;
    memcpy(id.data, "BT3VITA00", SCE_NET_ADHOCCTL_ADHOCID_LEN);
    r = sceNetAdhocctlInit(&id);
    Vita_Log("net: ad hoc control %08x\n", (unsigned)r);
    if (r < 0 && (unsigned)r != SCE_ERROR_NET_ADHOCCTL_ALREADY_INITIALIZED) {
        snprintf(sErr, sizeof(sErr), "ad hoc did not start (%08x)", (unsigned)r);
        return 0;
    }
    sAdhocInit = 1;
    return 1;
}

/* (a match begun in the running game has ended, or was not made: the next one connects anew) */
static void adhoc_down(void) {
    if (sAdhocInit) {
        int a = sceNetAdhocctlTerm(), b = sceNetAdhocTerm();
        Vita_Log("net: ad hoc ended (%08x, %08x)\n", (unsigned)a, (unsigned)b);
        sAdhocInit = 0;
    }
}

static int adhoc_socket(void) {
    SceNetEtherAddr mac;
    int r;

    memset(&mac, 0, sizeof(mac));
    r = sceNetAdhocctlGetEtherAddr(&mac);
    sSock = sceNetAdhocPdpCreate(&mac, NET_PORT, 0x4000, 0);
    Vita_Log("net: ad hoc address %02x:%02x:%02x:%02x:%02x:%02x (%08x), socket %08x\n", mac.data[0], mac.data[1], mac.data[2], mac.data[3],
             mac.data[4], mac.data[5], (unsigned)r, (unsigned)sSock);
    if (sSock < 0) {
        snprintf(sErr, sizeof(sErr), "the ad hoc socket could not be made (%08x)", (unsigned)sSock);
        return 0;
    }
    memset(&sMine, 0, sizeof(sMine));
    memcpy(sMine.mac, mac.data, 6);
    sMine.port = NET_PORT;
    return 1;
}

static int link_open(int kind) {
    int r;

    sErr[0] = '\0';
    sLink = kind;
    if (!net_base()) {
        return 0;
    }
    if (kind == LINK_ADHOC) {
        return adhoc_up() && adhoc_dialog() && adhoc_socket();
    }
    {
        int state = 0, one = 1;
        SceNetCtlInfo info;
        SceNetSockaddrIn a;
        sceNetCtlInetGetState(&state);
        if (state != SCE_NETCTL_STATE_CONNECTED) {
            snprintf(sErr, sizeof(sErr), "this Vita is not connected to a Wi-Fi network (Settings > Network)");
            return 0;
        }
        memset(&info, 0, sizeof(info));
        if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0) {
            snprintf(sMyIp, sizeof(sMyIp), "%s", info.ip_address);
        }
        sSock = sceNetSocket("bt3_net", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP);
        if (sSock < 0) {
            snprintf(sErr, sizeof(sErr), "the network socket could not be made (%08x)", (unsigned)sSock);
            return 0;
        }
        sceNetSetsockopt(sSock, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST, &one, sizeof(one));
        sceNetSetsockopt(sSock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof(one));
        sceNetSetsockopt(sSock, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &one, sizeof(one));
        memset(&a, 0, sizeof(a));
        a.sin_len = sizeof(a);
        a.sin_family = SCE_NET_AF_INET;
        a.sin_port = sceNetHtons(NET_PORT);
        a.sin_addr.s_addr = SCE_NET_INADDR_ANY;
        if ((r = sceNetBind(sSock, (SceNetSockaddr *)&a, sizeof(a))) < 0) {
            snprintf(sErr, sizeof(sErr), "the network port %d could not be taken (%08x)", NET_PORT, (unsigned)r);
            return 0;
        }
        memset(&sMine, 0, sizeof(sMine));
        sceNetInetPton(SCE_NET_AF_INET, sMyIp, &sMine.ip);
        sMine.port = sceNetHtons(NET_PORT);
        Vita_Log("net: Wi-Fi, address %s, socket %08x\n", sMyIp, (unsigned)sSock);
        return 1;
    }
}

static void link_close(void) {
    if (sSock >= 0) {
        if (sLink == LINK_ADHOC) {
            sceNetAdhocPdpDelete(sSock, 0);
        } else {
            sceNetSocketClose(sSock);
        }
        sSock = -1;
    }
}

static void link_send(const NetAddr *to, const void *buf, int n) {
    if (sSock < 0) {
        return;
    }
    if (sLink == LINK_ADHOC) {
        sceNetAdhocPdpSend(sSock, (const SceNetEtherAddr *)to->mac, to->port, buf, n, 0, SCE_NET_ADHOC_F_NONBLOCK);
    } else {
        SceNetSockaddrIn a;
        memset(&a, 0, sizeof(a));
        a.sin_len = sizeof(a);
        a.sin_family = SCE_NET_AF_INET;
        a.sin_port = to->port;
        a.sin_addr.s_addr = to->ip;
        sceNetSendto(sSock, buf, (unsigned)n, SCE_NET_MSG_DONTWAIT, (SceNetSockaddr *)&a, sizeof(a));
    }
}

static void link_broadcast(const void *buf, int n) {
    NetAddr all;
    memset(&all, 0, sizeof(all));
    if (sLink == LINK_ADHOC) {
        memset(all.mac, 0xFF, 6);
        all.port = NET_PORT;
    } else {
        all.ip = SCE_NET_INADDR_BROADCAST;
        all.port = sceNetHtons(NET_PORT);
    }
    link_send(&all, buf, n);
}

/* One packet waiting, or 0. */
static int link_recv(uint8_t *buf, int size, NetAddr *from) {
    memset(from, 0, sizeof(*from));
    if (sSock < 0) {
        return 0;
    }
    if (sLink == LINK_ADHOC) {
        SceNetEtherAddr mac;
        SceUShort16 port = 0;
        int len = size, r = sceNetAdhocPdpRecv(sSock, &mac, &port, buf, &len, 0, SCE_NET_ADHOC_F_NONBLOCK);
        if (r < 0 || len <= 0) {
            return 0;
        }
        memcpy(from->mac, mac.data, 6);
        from->port = port;
        return len;
    } else {
        SceNetSockaddrIn a;
        unsigned int alen = sizeof(a);
        int r = sceNetRecvfrom(sSock, buf, (unsigned)size, SCE_NET_MSG_DONTWAIT, (SceNetSockaddr *)&a, &alen);
        if (r <= 0) {
            return 0;
        }
        from->ip = a.sin_addr.s_addr;
        from->port = a.sin_port;
        return r;
    }
}

static void addr_text(const NetAddr *a, char *out, int size) {
    if (sLink == LINK_ADHOC) {
        snprintf(out, (size_t)size, "%02X:%02X:%02X:%02X:%02X:%02X", a->mac[0], a->mac[1], a->mac[2], a->mac[3], a->mac[4], a->mac[5]);
    } else {
        sceNetInetNtop(SCE_NET_AF_INET, &a->ip, out, (unsigned)size);
    }
}

/* ------------------------------------------------------------------------------------------------- packets */
/* The head of every packet: magic, type, version, the match's number (the host's). */
static int head(uint8_t *p, int type) {
    put32(p, MAGIC);
    p[4] = (uint8_t)type;
    p[5] = NET_VERSION;
    p[6] = p[7] = 0;
    put32(p + 8, sSession);
    return 12;
}

static void name_put(uint8_t *p, const char *name) {
    memset(p, 0, NAME_LEN);
    snprintf((char *)p, NAME_LEN, "%s", name);
}

static void name_get(char *out, const uint8_t *p) {
    memcpy(out, p, NAME_LEN);
    out[NAME_LEN - 1] = '\0';
}

/* What both Vitas must have the same of: the program, and the disc's archives (their tables of contents: every file's
   place and size; the data tables of the program are checked against the USA release at every start, vita_mem.c). */
static unsigned long long build_id(void) {
    static const char kBuild[] = __DATE__ " " __TIME__ " " VITA_GAME_NAME;
    extern int gPortFastFloat, gVitaWide;
    unsigned long long h = fnv(0xCBF29CE484222325ull, kBuild, sizeof(kBuild));
    int f = gPortFastFloat != 0, w = gVitaWide != 0; /* (bt3.txt float= and wide=: the game computes and culls by them) */
    h = fnv(h, &f, sizeof(f));
    return fnv(h, &w, sizeof(w));
}

static unsigned long long data_id(void) {
    extern char gVitaDisc[];
    static const char *const kArc[3] = {"PZS3US0.AFS", "PZS3US1.AFS", "PZS3US2.AFS"};
    unsigned long long h = 0xCBF29CE484222325ull;
    int k;

    for (k = 0; k < 3; k++) {
        char path[400];
        uint32_t hd[2] = {0, 0};
        SceUID fd;
        snprintf(path, sizeof(path), "%s/DATA/%s", gVitaDisc, kArc[k]);
        fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd < 0) {
            h = fnv(h, "none", 4);
            continue;
        }
        if (sceIoRead(fd, hd, 8) == 8 && hd[1] < 200000) {
            uint32_t bytes = hd[1] * 8;
            uint8_t *toc = malloc(bytes ? bytes : 1);
            if (toc != NULL && sceIoRead(fd, toc, bytes) == (int)bytes) {
                h = fnv(h, hd, 8);
                h = fnv(h, toc, bytes);
            }
            free(toc);
        }
        sceIoClose(fd);
    }
    return h;
}

/* ------------------------------------------------------------------------------------------------- the start menu's part */
static void my_name(void) {
    char n[SCE_SYSTEM_PARAM_USERNAME_MAXSIZE];
    app_util();
    memset(n, 0, sizeof(n));
    if (sceAppUtilSystemParamGetString(SCE_SYSTEM_PARAM_ID_USERNAME, (SceChar8 *)n, sizeof(n)) >= 0 && n[0] != '\0') {
        snprintf(sMyName, sizeof(sMyName), "%s", n);
    }
}

static const char *time_name(int t) {
    return t == 0 ? "60 s" : t == 1 ? "90 s" : t == 2 ? "180 s" : t == 3 ? "240 s" : t == 4 ? "none" : "240 s (the game's own)";
}

static const char *refusal(int why) {
    return why == WHY_VERSION ? "the two Vitas have different versions of the game (or their bt3.txt float= / wide= differ)" :
           why == WHY_DATA ? "the two Vitas' disc files are not the same (another dump of the disc?)" :
           why == WHY_FLOAT ? "the two Vitas' bt3.txt float= settings differ" :
           why == WHY_BUSY ? "the host is already playing with someone" : "the host said no";
}

static int cancel_pressed(void) {
    static unsigned prev;
    unsigned now = pad_now(), pressed = now & ~prev;
    prev = now;
    return (pressed & SCE_CTRL_CIRCLE) != 0;
}

/* The host: tells the network a few times a second, takes the first Vita that asks and fits. 1: a player joined. */
static int host_wait(void) {
    uint8_t pkt[PKT_MAX], in[PKT_MAX];
    uint64_t lastOffer = 0, lastWelcome = 0, since = now_us();
    uint32_t nonce = 0;
    int joined = 0, lines = 0, r;
    NetAddr from;

    sSession = (uint32_t)(now_us() ^ (now_us() >> 21)) | 1u;
    sSeed = (unsigned)(now_us() * 2654435761u) | 1u;
    sDelay = gVitaNetDelay >= 1 && gVitaNetDelay <= 8 ? gVitaNetDelay : sLink == LINK_WIFI ? 3 : 2;
    psvDebugScreenPrintf("\e[H\e[2J\n  Multiplayer: hosting a match (%s)\n\n  Waiting for another Vita to join ...\n\n"
                         "  On the other Vita: hold SELECT while the game starts,\n  Multiplayer > Join a match (%s).\n\n"
                         "  You are player 1 (%s). Time limit: %s.\n\n  CIRCLE: cancel.\n",
                         sLink == LINK_ADHOC ? "ad hoc" : "Wi-Fi", sLink == LINK_ADHOC ? "ad hoc" : "Wi-Fi", sMyName, time_name(gVitaNetRules[0]));
    if (sLink == LINK_WIFI) {
        psvDebugScreenPrintf("  (this Vita's address: %s)\n", sMyIp);
    }
    for (;;) {
        uint64_t t = now_us();
        int n;
        if (cancel_pressed()) {
            return 0;
        }
        if (!joined && t - lastOffer > 250000) {
            int k = head(pkt, T_OFFER);
            put64(pkt + k, sMyBuild);
            put64(pkt + k + 8, sMyData);
            name_put(pkt + k + 16, sMyName);
            link_broadcast(pkt, k + 16 + NAME_LEN);
            lastOffer = t;
        }
        if (joined && t - lastWelcome > 100000) {
            int k;
            memset(pkt, 0, 64);
            k = head(pkt, T_WELCOME);
            put32(pkt + k, nonce);
            put32(pkt + k + 4, sSeed);
            pkt[k + 8] = (uint8_t)sDelay;
            for (r = 0; r < 6; r++) {
                pkt[k + 12 + r] = (uint8_t)(int8_t)gVitaNetRules[r];
            }
            name_put(pkt + k + 20, sMyName);
            link_send(&sPeer, pkt, k + 20 + NAME_LEN);
            lastWelcome = t;
            if (t - since > 30000000) { /* (the other Vita went quiet half-way) */
                joined = 0;
                psvDebugScreenPrintf("\n  The other Vita stopped answering; waiting again.\n");
            }
        }
        while ((n = link_recv(in, sizeof(in), &from)) > 0) {
            int type;
            if (n < 12 || get32(in) != MAGIC) {
                continue;
            }
            type = in[4];
            if (type == T_JOIN && n >= 12 + 16 + NAME_LEN + 4 && get32(in + 8) == sSession) {
                uint32_t why = 0;
                char name[NAME_LEN];
                name_get(name, in + 28);
                if (in[5] != NET_VERSION || get64(in + 12) != sMyBuild) {
                    why = WHY_VERSION;
                } else if (get64(in + 20) != sMyData) {
                    why = WHY_DATA;
                } else if (joined && !same_addr(&from, &sPeer)) {
                    why = WHY_BUSY;
                }
                if (why != 0) {
                    int k = head(pkt, T_REFUSE);
                    put32(pkt + k, get32(in + 28 + NAME_LEN));
                    put32(pkt + k + 4, why);
                    link_send(&from, pkt, k + 8);
                    if (lines++ < 6) {
                        psvDebugScreenPrintf("\n  \e[33m%s could not join: %s.\e[39m\n", name, refusal((int)why));
                    }
                    continue;
                }
                if (!joined) {
                    char where[40];
                    sPeer = from;
                    nonce = get32(in + 28 + NAME_LEN);
                    snprintf(sTheirName, sizeof(sTheirName), "%s", name);
                    joined = 1;
                    since = t;
                    lastWelcome = 0;
                    addr_text(&from, where, sizeof(where));
                    Vita_Log("net: %s (%s) asks to join\n", name, where);
                    psvDebugScreenPrintf("\n  %s is joining ...\n", name);
                }
            } else if (type == T_READY && joined && same_addr(&from, &sPeer) && get32(in + 8) == sSession && n >= 16 && get32(in + 12) == nonce) {
                Vita_Log("net: %s joined: match %08x, seed %08x, input delay %d\n", sTheirName, (unsigned)sSession, sSeed, sDelay);
                return 1;
            }
        }
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND); /* (no sleep while waiting) */
        sceKernelDelayThread(10000);
    }
}

/* The one who joins: the hosts heard are listed; CROSS joins the one chosen. 1: in a match. */
typedef struct { NetAddr addr; uint32_t session; char name[NAME_LEN]; int fits; uint64_t seen; } Offer;

static int join_wait(void) {
    Offer list[4];
    uint8_t pkt[PKT_MAX], in[PKT_MAX];
    int count = 0, sel = 0, asking = -1, redraw = 1, k;
    uint64_t lastAsk = 0, askSince = 0;
    uint32_t nonce = (uint32_t)now_us() | 1u;
    unsigned prev = pad_now();
    NetAddr from;
    char note[200] = "";

    memset(list, 0, sizeof(list));
    for (;;) {
        uint64_t t = now_us();
        unsigned now = pad_now(), pressed = now & ~prev;
        int n;
        prev = now;
        for (k = 0; k < count; k++) { /* (a host not heard for five seconds has gone) */
            if (t - list[k].seen > 5000000 && k != asking) {
                list[k] = list[--count];
                if (asking == count) {
                    asking = k; /* (the last one took this place) */
                }
                sel = sel >= count ? (count > 0 ? count - 1 : 0) : sel;
                redraw = 1;
                k--;
            }
        }
        if (redraw) {
            psvDebugScreenPrintf("\e[H\e[2J\n  Multiplayer: joining a match (%s)\n\n", sLink == LINK_ADHOC ? "ad hoc" : "Wi-Fi");
            if (count == 0) {
                psvDebugScreenPrintf("  Looking for a Vita that hosts a match ...\n\n  On the other Vita: hold SELECT while the game starts,\n"
                                     "  Multiplayer > Host a match (%s).\n", sLink == LINK_ADHOC ? "ad hoc" : "Wi-Fi");
            } else {
                psvDebugScreenPrintf("  Matches found:\n\n");
                for (k = 0; k < count; k++) {
                    psvDebugScreenPrintf("  %s %s%s\e[39m\n", k == sel ? "\e[33m>" : " ", list[k].name, list[k].fits ? "" : " (another version: cannot join)");
                }
                psvDebugScreenPrintf("\n  CROSS: join.");
            }
            psvDebugScreenPrintf("\n  CIRCLE: cancel.\n");
            if (note[0] != '\0') {
                psvDebugScreenPrintf("\n  \e[33m%s\e[39m\n", note);
            }
            if (asking >= 0) {
                psvDebugScreenPrintf("\n  Joining %s ...\n", list[asking].name);
            }
            redraw = 0;
        }
        if (pressed & SCE_CTRL_CIRCLE) {
            return 0;
        }
        if (asking < 0 && (pressed & SCE_CTRL_UP) && count > 0) {
            sel = (sel + count - 1) % count;
            redraw = 1;
        } else if (asking < 0 && (pressed & SCE_CTRL_DOWN) && count > 0) {
            sel = (sel + 1) % count;
            redraw = 1;
        } else if (asking < 0 && (pressed & SCE_CTRL_CROSS) && count > 0 && list[sel].fits) {
            asking = sel;
            askSince = t;
            lastAsk = 0;
            note[0] = '\0';
            redraw = 1;
        }
        if (asking >= 0 && t - lastAsk > 200000) {
            sSession = list[asking].session;
            k = head(pkt, T_JOIN);
            put64(pkt + k, sMyBuild);
            put64(pkt + k + 8, sMyData);
            name_put(pkt + k + 16, sMyName);
            put32(pkt + k + 16 + NAME_LEN, nonce);
            link_send(&list[asking].addr, pkt, k + 16 + NAME_LEN + 4);
            lastAsk = t;
            if (t - askSince > 10000000) {
                snprintf(note, sizeof(note), "%s did not answer.", list[asking].name);
                asking = -1;
                redraw = 1;
            }
        }
        while ((n = link_recv(in, sizeof(in), &from)) > 0) {
            int type;
            if (n < 12 || get32(in) != MAGIC) {
                continue;
            }
            type = in[4];
            if (type == T_OFFER && n >= 12 + 16 + NAME_LEN) {
                int at = -1;
                for (k = 0; k < count; k++) {
                    if (same_addr(&list[k].addr, &from)) {
                        at = k;
                    }
                }
                if (at < 0 && count < 4) {
                    at = count++;
                    redraw = 1;
                }
                if (at >= 0) {
                    int fits = in[5] == NET_VERSION && get64(in + 12) == sMyBuild && get64(in + 20) == sMyData;
                    if (list[at].session != get32(in + 8) || list[at].fits != fits) {
                        redraw = 1;
                    }
                    list[at].addr = from;
                    list[at].session = get32(in + 8);
                    list[at].fits = fits;
                    list[at].seen = t;
                    name_get(list[at].name, in + 28);
                }
            } else if (asking >= 0 && same_addr(&from, &list[asking].addr) && get32(in + 8) == list[asking].session && n >= 16 &&
                       get32(in + 12) == nonce) {
                if (type == T_REFUSE && n >= 20) {
                    snprintf(note, sizeof(note), "%s: %s.", list[asking].name, refusal((int)get32(in + 16)));
                    asking = -1;
                    redraw = 1;
                } else if (type == T_WELCOME && n >= 12 + 20 + NAME_LEN) {
                    sPeer = from;
                    sSeed = get32(in + 16);
                    sDelay = in[20] >= 1 && in[20] <= 8 ? in[20] : 2;
                    for (k = 0; k < 6; k++) {
                        gVitaNetRules[k] = (int8_t)in[24 + k];
                    }
                    name_get(sTheirName, in + 32);
                    k = head(pkt, T_READY);
                    put32(pkt + k, nonce);
                    link_send(&sPeer, pkt, k + 4);
                    link_send(&sPeer, pkt, k + 4);
                    sJoinNonce = nonce;
                    Vita_Log("net: joined %s: match %08x, seed %08x, input delay %d, battle settings %d %d %d %d %d %d\n", sTheirName, (unsigned)sSession,
                             sSeed, sDelay, gVitaNetRules[0], gVitaNetRules[1], gVitaNetRules[2], gVitaNetRules[3], gVitaNetRules[4], gVitaNetRules[5]);
                    return 1;
                }
            }
        }
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND); /* (no sleep while waiting) */
        sceKernelDelayThread(10000);
    }
}

static void restart(void) {
    int r;
    Vita_Log("net: the game starts again\n");
    sceKernelDelayThread(100000);
    r = sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
    Vita_Log("net: the game could not be started again (%08x)\n", (unsigned)r);
    sceKernelExitProcess(0);
}

static void wait_cross(void) {
    unsigned prev = pad_now();
    for (;;) {
        unsigned now = pad_now();
        if ((now & ~prev) & SCE_CTRL_CROSS) {
            break;
        }
        prev = now;
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        sceKernelDelayThread(16000);
    }
}

static void session_saves(void) {
    static const char *const kFiles[] = {"BASLUS-21678DBZT3", "icon.sys", "dbzsm.ico"};
    char path[200];
    int i;
    sceIoMkdir(NET_SAVES, 0777);
    for (i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), NET_SAVES "/card1/BASLUS-21678DBZT3/%s", kFiles[i]);
        sceIoRemove(path);
    }
    setenv("BT3_SAVES", NET_SAVES, 1);
}

/* ------------------------------------------------------------------------------------------------- from the game's menus */
#define NET_START HOME "/net_start.txt"

/* (src/menu/menu_g_c.c, the versus menu's Multiplayer > Ad Hoc: Host / Join) a match is a fresh start of the game: the
   choice and the host's battle settings are written down, and the game starts again (vita_start.c Vita_NetStartFile
   takes them, before anything else). */
void Vita_NetRestartInto(int role, int link, const int *rules) {
    char text[96];
    SceUID fd;
    int k, n;

    n = snprintf(text, sizeof(text), "net %d %d", role, link);
    for (k = 0; k < 6; k++) {
        n += snprintf(text + n, sizeof(text) - (size_t)n, " %d", rules != NULL ? rules[k] : -1);
    }
    n += snprintf(text + n, sizeof(text) - (size_t)n, "\n");
    fd = sceIoOpen(NET_START, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, text, (SceSize)n);
        sceIoClose(fd);
    }
    Vita_Log("net: the versus menu chose %s (%s): the game starts again for the match\n", role == 1 ? "host" : "join", link == LINK_ADHOC ? "ad hoc" : "Wi-Fi");
    restart();
}

/* (vita_start.c, first thing) the game was started again for a match by its menus: 1, and the choice taken (the file is
   gone again: the next start is an ordinary one, whatever happens) */
int Vita_NetStartFile(void) {
    char text[96];
    int r[6], role = 0, link = 0, n, k;
    SceUID fd = sceIoOpen(NET_START, SCE_O_RDONLY, 0);

    if (fd < 0) {
        return 0;
    }
    memset(text, 0, sizeof(text));
    sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    sceIoRemove(NET_START);
    n = sscanf(text, "net %d %d %d %d %d %d %d %d", &role, &link, &r[0], &r[1], &r[2], &r[3], &r[4], &r[5]);
    if (n < 2 || (role != 1 && role != 2) || (link != LINK_ADHOC && link != LINK_WIFI)) {
        Vita_Log("net: %s is not a match's start (%s), left alone\n", NET_START, text);
        return 0;
    }
    gVitaNetRole = role;
    gVitaNetLink = link;
    for (k = 0; k < 6; k++) {
        gVitaNetRules[k] = k + 2 < n ? r[k] : -1;
    }
    Vita_Log("net: started again for a match from the versus menu: %s, %s\n", role == 1 ? "host" : "join", link == LINK_ADHOC ? "ad hoc" : "Wi-Fi");
    return 1;
}

/* What would make the two games differ, put as the game has it: the test switches of bt3.txt (a replay, the demo
   fight, a starting screen, the game-side effect switches), the start menu's cheat, a European language, the vector
   stand-ins' check. Then the match's own save folder, empty. */
static struct { int kept, fxOff, cheat, pad2Fake, lang, hadSaves; char saves[200]; } sWas;

static void match_setup(void) {
    extern int gVitaGameFxOff, gVitaCheatUnlock, gVitaStartMode, gVitaPad2Fake, gVitaVufQuick;
    extern int VitaEu_MatchEnglish(void);
    int keep = !sWas.kept;
    if (keep) { /* (a match begun in the running game: as it was, for after the match) */
        const char *sv = getenv("BT3_SAVES");
        sWas.kept = 1;
        sWas.fxOff = gVitaGameFxOff;
        sWas.cheat = gVitaCheatUnlock;
        sWas.pad2Fake = gVitaPad2Fake;
        sWas.hadSaves = sv != NULL;
        snprintf(sWas.saves, sizeof(sWas.saves), "%s", sv != NULL ? sv : "");
    }
    unsetenv("BT3_REPLAY");
    unsetenv("BT3_DEMO");
    unsetenv("BT3_DEMO_PICK");
    unsetenv("BT3_SOAK");
    gVitaGameFxOff = 0;
    gVitaCheatUnlock = 0;
    gVitaStartMode = 0;
    gVitaPad2Fake = 0;
    gVitaVufQuick = -1;
    {
        int lang = VitaEu_MatchEnglish();
        if (keep) {
            sWas.lang = lang;
        }
    }
    session_saves();
}

/* (a match begun in the running game has ended) the player's own settings and save folder again */
static void match_undo(void) {
    extern int gVitaGameFxOff, gVitaCheatUnlock, gVitaPad2Fake;
    extern void VitaEu_MatchRestore(int lang);
    if (!sWas.kept) {
        return;
    }
    sWas.kept = 0;
    gVitaGameFxOff = sWas.fxOff;
    gVitaCheatUnlock = sWas.cheat;
    gVitaPad2Fake = sWas.pad2Fake;
    VitaEu_MatchRestore(sWas.lang);
    if (sWas.hadSaves) {
        setenv("BT3_SAVES", sWas.saves, 1);
    } else {
        unsetenv("BT3_SAVES");
    }
}

/* (vita_main.c, after the disc's files were found and before the game starts) the start menu chose Multiplayer:
   the line, the other Vita, and the match set up. Comes back only with a match; else the game starts again. */
void Vita_NetConnect(void) {
    extern int gPortFastFloat;
    int ok;

    if (gVitaNetTest > 0) { /* (the one-Vita test: no line) */
        sOn = 1;
        sMe = 0;
        sSeed = 0x5EED1234;
        sDelay = 2;
        match_setup();
        Vita_Log("net: the match test (nettest=%d): no line, both players from the script\n", gVitaNetTest);
        sceIoRemove(NET_LOG);
        return;
    }
    if (gVitaNetRole == 0) {
        return;
    }
    gPortFastFloat = gPortFastFloat != 0; /* (a part of build_id: both must compute alike) */
    sMyBuild = build_id();
    psvDebugScreenPrintf("\e[H\e[2J\n  Multiplayer: checking this Vita's game files ...\n");
    sMyData = data_id();
    psvDebugScreenPrintf("\n  Multiplayer: starting the %s connection ...\n", gVitaNetLink == LINK_ADHOC ? "ad hoc" : "Wi-Fi");
    ok = link_open(gVitaNetLink);
    if (ok) {
        my_name();
        ok = gVitaNetRole == 1 ? host_wait() : join_wait();
        if (!ok) {
            snprintf(sErr, sizeof(sErr), "the match was cancelled");
        }
    }
    if (!ok) {
        Vita_Log("net: no match: %s\n", sErr);
        link_close();
        psvDebugScreenPrintf("\e[H\e[2J\n  Multiplayer: %s.\n\n  Press CROSS: the game starts again.\n", sErr);
        wait_cross();
        restart();
    }
    sOn = 1;
    sConnected = 1;
    sMe = gVitaNetRole == 1 ? 0 : 1;
    match_setup();
    psvDebugScreenPrintf("\e[H\e[2J\n  Multiplayer: %s vs %s. The match begins at the character\n  select (both players choose with their own Vita).\n\n"
                         "  Going back to the main menu ends the match.\n", sMe == 0 ? sMyName : sTheirName, sMe == 0 ? sTheirName : sMyName);
}

/* ------------------------------------------------------------------------------------------------- from the versus menu */
/* The same finding of the other Vita as host_wait and join_wait above, a step at a time: called once a frame by the
   versus menu while its Multiplayer choice waits (src/menu/menu_g_c.c), which shows `line` in its message window. The
   system's ad hoc dialog is drawn over the menu's picture by the drawing thread (vita_vgl.c gVitaDialogOn). The one
   who joins takes the first host heard that fits (two Vitas next to each other: there is one). */
enum { PAIR_OFF, PAIR_DIALOG, PAIR_DIALOG_END, PAIR_HOST, PAIR_SEEK, PAIR_ASK, PAIR_DONE, PAIR_FAIL };
static struct {
    int state, role, count, joined;
    uint64_t lastSend, since;
    uint32_t nonce, hostSession, refused;
    NetAddr host;
    char hostName[NAME_LEN];
    char line[80];
    unsigned lineGen;
} sPair;
extern volatile int gVitaDialogOn;
extern int Vita_DialogCan(void);

static void pair_say(const char *fmt, ...) {
    char text[sizeof(sPair.line)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (strcmp(text, sPair.line) != 0) {
        snprintf(sPair.line, sizeof(sPair.line), "%s", text);
        sPair.lineGen++;
    }
}

static void pair_fail(const char *fmt, ...) {
    char text[sizeof(sPair.line)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    snprintf(sPair.line, sizeof(sPair.line), "%s", text);
    sPair.lineGen++;
    sPair.state = PAIR_FAIL;
    Vita_Log("net: versus menu: no match: %s (%s)\n", text, sErr);
    link_close();
    adhoc_down();
}

/* 1: the versus menu can host or join without the program starting again (the game can be started again inside it,
   the display has the size the system's dialog draws at, no match is on) */
int Vita_NetPairCan(void) {
    return Vita_WarmCan() && Vita_DialogCan() && !sOn && gVitaNetTest <= 0;
}

/* role 1 host (with the battle settings `rules`, the save's six), 2 join. 1: begun (Vita_NetPairPoll from now on);
   0: it cannot be done from the running game (the caller starts the program again for it, as before). */
int Vita_NetPairBegin(int role, const int *rules) {
    extern int gPortFastFloat;
    SceNetCheckDialogParam p;
    static SceNetAdhocctlGroupName group;
    int k, r;

    if (!Vita_NetPairCan() || sPair.state != PAIR_OFF) {
        return 0;
    }
    memset(&sPair, 0, sizeof(sPair));
    sPair.role = role;
    sErr[0] = '\0';
    sLink = LINK_ADHOC;
    gPortFastFloat = gPortFastFloat != 0;
    sMyBuild = build_id();
    if (sMyData == 0) {
        sMyData = data_id();
    }
    for (k = 0; k < 6; k++) {
        gVitaNetRules[k] = role == 1 && rules != NULL ? rules[k] : -1;
    }
    if (!net_base() || !adhoc_up()) {
        Vita_Log("net: versus menu: %s\n", sErr);
        adhoc_down();
        return 0;
    }
    app_util();
    sceNetCheckDialogParamInit(&p);
    memset(&group, 0, sizeof(group));
    memcpy(group.data, "BT3VITA", 7);
    p.groupName = &group;
    memcpy(p.npCommunicationId.data, "BT3VITA00", 9);
    p.npCommunicationId.term = '\0';
    p.npCommunicationId.num = 0;
    p.mode = SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CONN;
    p.timeoutUs = 0;
    r = sceNetCheckDialogInit(&p);
    Vita_Log("net: versus menu: %s, the ad hoc dialog over the menu: %08x\n", role == 1 ? "host" : "join", (unsigned)r);
    if (r < 0) {
        adhoc_down();
        return 0;
    }
    gVitaDialogOn = 1;
    sPair.state = PAIR_DIALOG;
    pair_say("Connecting (ad hoc) ...");
    return 1;
}

/* One step. 0: going on; 1: the other Vita is found and the match agreed (Vita_NetPairStart next); -1: it failed or
   was refused (*line says why; over). *line: what to show, *gen: counts its changes. */
int Vita_NetPairPoll(const char **line, unsigned *gen) {
    uint8_t pkt[PKT_MAX], in[PKT_MAX];
    uint64_t t = now_us();
    NetAddr from;
    int n, k;

    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    switch (sPair.state) {
    case PAIR_DIALOG:
        if (sceNetCheckDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED) {
            gVitaDialogOn = 0; /* (the drawing thread hands it no more pictures: a few frames, then it is closed) */
            sPair.count = 4;
            sPair.state = PAIR_DIALOG_END;
        }
        break;
    case PAIR_DIALOG_END:
        if (--sPair.count <= 0) {
            SceNetCheckDialogResult res;
            memset(&res, 0, sizeof(res));
            sceNetCheckDialogGetResult(&res);
            sceNetCheckDialogTerm();
            Vita_Log("net: versus menu: the ad hoc dialog ended: result %d\n", res.result);
            if (res.result != SCE_COMMON_DIALOG_RESULT_OK) {
                pair_fail(res.result == SCE_COMMON_DIALOG_RESULT_USER_CANCELED ? "The ad hoc connection was\ncancelled." : "The ad hoc connection was\nnot made.");
            } else if (!adhoc_socket()) {
                pair_fail("The ad hoc connection could\nnot be used.");
            } else {
                my_name();
                if (sPair.role == 1) {
                    sSession = (uint32_t)(now_us() ^ (now_us() >> 21)) | 1u;
                    sSeed = (unsigned)(now_us() * 2654435761u) | 1u;
                    sDelay = gVitaNetDelay >= 1 && gVitaNetDelay <= 8 ? gVitaNetDelay : 2;
                    sPair.state = PAIR_HOST;
                    pair_say("Waiting for a player to join ...\n(Triangle: cancel)");
                } else {
                    sPair.nonce = (uint32_t)now_us() | 1u;
                    sPair.state = PAIR_SEEK;
                    pair_say("Looking for a match nearby ...\n(Triangle: cancel)");
                }
            }
        }
        break;
    case PAIR_HOST:
        if (!sPair.joined && t - sPair.lastSend > 250000) {
            k = head(pkt, T_OFFER);
            put64(pkt + k, sMyBuild);
            put64(pkt + k + 8, sMyData);
            name_put(pkt + k + 16, sMyName);
            link_broadcast(pkt, k + 16 + NAME_LEN);
            sPair.lastSend = t;
        }
        if (sPair.joined && t - sPair.lastSend > 100000) {
            memset(pkt, 0, 64);
            k = head(pkt, T_WELCOME);
            put32(pkt + k, sPair.nonce);
            put32(pkt + k + 4, sSeed);
            pkt[k + 8] = (uint8_t)sDelay;
            for (n = 0; n < 6; n++) {
                pkt[k + 12 + n] = (uint8_t)(int8_t)gVitaNetRules[n];
            }
            name_put(pkt + k + 20, sMyName);
            link_send(&sPeer, pkt, k + 20 + NAME_LEN);
            sPair.lastSend = t;
            if (t - sPair.since > 30000000) { /* (the other Vita went quiet half-way) */
                sPair.joined = 0;
                pair_say("Waiting for a player to join ...\n(Triangle: cancel)");
            }
        }
        while ((n = link_recv(in, sizeof(in), &from)) > 0) {
            int type;
            if (n < 12 || get32(in) != MAGIC) {
                continue;
            }
            type = in[4];
            if (type == T_JOIN && n >= 12 + 16 + NAME_LEN + 4 && get32(in + 8) == sSession) {
                uint32_t why = 0;
                char name[NAME_LEN];
                name_get(name, in + 28);
                if (in[5] != NET_VERSION || get64(in + 12) != sMyBuild) {
                    why = WHY_VERSION;
                } else if (get64(in + 20) != sMyData) {
                    why = WHY_DATA;
                } else if (sPair.joined && !same_addr(&from, &sPeer)) {
                    why = WHY_BUSY;
                }
                if (why != 0) {
                    k = head(pkt, T_REFUSE);
                    put32(pkt + k, get32(in + 28 + NAME_LEN));
                    put32(pkt + k + 4, why);
                    link_send(&from, pkt, k + 8);
                    if (why != WHY_BUSY && sPair.refused != get32(in + 28 + NAME_LEN)) {
                        sPair.refused = get32(in + 28 + NAME_LEN);
                        Vita_Log("net: versus menu: %s could not join: %s\n", name, refusal((int)why));
                    }
                    continue;
                }
                if (!sPair.joined) {
                    sPeer = from;
                    sPair.nonce = get32(in + 28 + NAME_LEN);
                    snprintf(sTheirName, sizeof(sTheirName), "%s", name);
                    sPair.joined = 1;
                    sPair.since = t;
                    sPair.lastSend = 0;
                    Vita_Log("net: versus menu: %s asks to join\n", name);
                    pair_say("%.23s is joining ...", name);
                }
            } else if (type == T_READY && sPair.joined && same_addr(&from, &sPeer) && get32(in + 8) == sSession && n >= 16 && get32(in + 12) == sPair.nonce) {
                Vita_Log("net: versus menu: %s joined: match %08x, seed %08x, input delay %d\n", sTheirName, (unsigned)sSession, sSeed, sDelay);
                gVitaNetRole = 1;
                gVitaNetLink = LINK_ADHOC;
                sPair.state = PAIR_DONE;
                pair_say("%.23s joined.\nThe match begins ...", sTheirName);
                break;
            }
        }
        break;
    case PAIR_SEEK:
    case PAIR_ASK:
        if (sPair.state == PAIR_ASK && t - sPair.lastSend > 200000) {
            sSession = sPair.hostSession;
            k = head(pkt, T_JOIN);
            put64(pkt + k, sMyBuild);
            put64(pkt + k + 8, sMyData);
            name_put(pkt + k + 16, sMyName);
            put32(pkt + k + 16 + NAME_LEN, sPair.nonce);
            link_send(&sPair.host, pkt, k + 16 + NAME_LEN + 4);
            sPair.lastSend = t;
            if (t - sPair.since > 10000000) {
                pair_say("%.23s did not answer.\nLooking again ...", sPair.hostName);
                sPair.state = PAIR_SEEK;
            }
        }
        while ((n = link_recv(in, sizeof(in), &from)) > 0) {
            int type;
            if (n < 12 || get32(in) != MAGIC) {
                continue;
            }
            type = in[4];
            if (type == T_OFFER && n >= 12 + 16 + NAME_LEN && sPair.state == PAIR_SEEK) {
                char name[NAME_LEN];
                name_get(name, in + 28);
                if (!(in[5] == NET_VERSION && get64(in + 12) == sMyBuild)) {
                    pair_say("%.23s has another version\nof the game: cannot join.", name);
                } else if (get64(in + 20) != sMyData) {
                    pair_say("%.23s has other disc files:\ncannot join.", name);
                } else if (get32(in + 8) != sPair.refused) {
                    sPair.host = from;
                    sPair.hostSession = get32(in + 8);
                    snprintf(sPair.hostName, sizeof(sPair.hostName), "%s", name);
                    sPair.since = t;
                    sPair.lastSend = 0;
                    sPair.state = PAIR_ASK;
                    pair_say("Joining %.23s ...", name);
                }
            } else if (sPair.state == PAIR_ASK && same_addr(&from, &sPair.host) && get32(in + 8) == sPair.hostSession && n >= 16 &&
                       get32(in + 12) == sPair.nonce) {
                if (type == T_REFUSE && n >= 20) {
                    int why = (int)get32(in + 16);
                    Vita_Log("net: versus menu: %s: %s\n", sPair.hostName, refusal(why));
                    sPair.refused = sPair.hostSession; /* (not asked again) */
                    sPair.state = PAIR_SEEK;
                    pair_say(why == WHY_BUSY ? "%.23s is already playing\nwith someone." : why == WHY_DATA ? "%.23s has other disc files:\ncannot join." :
                             "%.23s has another version\nof the game: cannot join.", sPair.hostName);
                } else if (type == T_WELCOME && n >= 12 + 20 + NAME_LEN) {
                    sPeer = from;
                    sSeed = get32(in + 16);
                    sDelay = in[20] >= 1 && in[20] <= 8 ? in[20] : 2;
                    for (k = 0; k < 6; k++) {
                        gVitaNetRules[k] = (int8_t)in[24 + k];
                    }
                    name_get(sTheirName, in + 32);
                    k = head(pkt, T_READY);
                    put32(pkt + k, sPair.nonce);
                    link_send(&sPeer, pkt, k + 4);
                    link_send(&sPeer, pkt, k + 4);
                    sJoinNonce = sPair.nonce;
                    gVitaNetRole = 2;
                    gVitaNetLink = LINK_ADHOC;
                    Vita_Log("net: versus menu: joined %s: match %08x, seed %08x, input delay %d, battle settings %d %d %d %d %d %d\n", sTheirName,
                             (unsigned)sSession, sSeed, sDelay, gVitaNetRules[0], gVitaNetRules[1], gVitaNetRules[2], gVitaNetRules[3], gVitaNetRules[4],
                             gVitaNetRules[5]);
                    sPair.state = PAIR_DONE;
                    pair_say("Joined %.23s.\nThe match begins ...", sTheirName);
                    break;
                }
            }
        }
        break;
    default:
        break;
    }
    *line = sPair.line;
    *gen = sPair.lineGen;
    return sPair.state == PAIR_DONE ? 1 : sPair.state == PAIR_FAIL ? -1 : 0;
}

/* 1 while the system's dialog is up (the menu takes no buttons then: they are the dialog's) */
int Vita_NetPairDialog(void) {
    return sPair.state == PAIR_DIALOG || sPair.state == PAIR_DIALOG_END;
}

/* Given up by the player (or the menu is left): the line closed. Not while the system's dialog is up. */
void Vita_NetPairCancel(void) {
    if (sPair.state == PAIR_OFF || Vita_NetPairDialog()) {
        return;
    }
    if (sPair.state != PAIR_FAIL) {
        Vita_Log("net: versus menu: given up\n");
        link_close();
        adhoc_down();
    }
    sPair.state = PAIR_OFF;
}

/* The match agreed (Vita_NetPairPoll gave 1), the menu faded out: the game starts again inside the program as the
   match (vita_warm.c > Vita_NetWarm), on both Vitas. Does not return. */
void Vita_NetPairStart(void) {
    sPair.state = PAIR_OFF;
    sPending = 1;
    Vita_WarmRestart();
    sPending = 0; /* (it could not: the program starts again, as before) */
    link_close();
    adhoc_down();
    Vita_NetRestartInto(gVitaNetRole, LINK_ADHOC, gVitaNetRules);
}

/* ------------------------------------------------------------------------------------------------- in the game */
int Port_NetActive(void) {
    return sOn;
}

int Port_NetSession(void) {
    return sOn;
}

int Port_NetMe(void) {
    return sMe;
}

/* (menu_c_e.c, the PC version's lobbies: only a lobby's owner leaves the character select) no lobbies here: -1 */
int Port_NetLobbyOwnerSide(void) {
    return -1;
}

/* The local player's view full screen in a two-player battle (btl_cam.c, btl_scene.c, eft_aa.c); -1 otherwise. */
int Port_NetView(void) {
    return sOn ? sMe : -1;
}

/* The game's settings that would make the two games differ: the game's own in a match (plat_settings.c). 1 and
   *value when `name` is one of them. */
int Vita_NetSetting(const char *name, int def, int *value) {
    if (!sOn) {
        return 0;
    }
    if (strcmp(name, "quick_start") == 0) {
        *value = 1; /* (no logos, opening film or copyright page: the same on both, and a quicker start) */
        return 1;
    }
    if (strcmp(name, "save_quick") == 0 || strcmp(name, "mods") == 0 || strcmp(name, "tech_zcounter2") == 0 ||
        strcmp(name, "tech_bombardment") == 0) {
        *value = 0;
        return 1;
    }
    if (strcmp(name, "vufquick") == 0 || strcmp(name, "asyncread") == 0) {
        *value = def;
        return 1;
    }
    return 0;
}

typedef struct { uint32_t tick; uint64_t value; } SyncNote;
static SyncNote sNoteMine[SYNC_SLOTS], sNoteTheirs[SYNC_SLOTS];
static uint32_t sNoteOutTick;
static uint64_t sNoteOutValue;
static uint32_t sApartAt;
static unsigned sApartCount, sCompared;

static uint8_t sIn[2][RING][PAD];
static uint32_t sHave[2];
static uint32_t sTick;
static int sArrivedFlag, sMet;
static uint32_t sMyArrival, sTheirArrival;
static uint64_t sHeardUs, sStartUs;
static int sEnding, sShownWait;
static unsigned sWaits;
static uint64_t sWaitUs;
static FILE *sTestLog;

static void note_mine(uint32_t tick) {
    if (tick % SYNC_EVERY == 0) {
        SyncNote *m = &sNoteMine[tick / SYNC_EVERY % SYNC_SLOTS];
        m->tick = tick;
        m->value = Port_NetDigest(0);
        sNoteOutTick = tick;
        sNoteOutValue = m->value;
    }
}

static void note_check(void) {
    int k;
    for (k = 0; k < SYNC_SLOTS; k++) {
        SyncNote *o = &sNoteTheirs[k], *m = &sNoteMine[k];
        if (o->tick != 0 && m->tick == o->tick) {
            sCompared++;
            if (m->value != o->value) {
                sApartCount++;
                if (sApartAt == 0) {
                    sApartAt = o->tick;
                    Vita_Log("net: the two games differ at blank %u (this Vita %016llx, the other %016llx)\n", (unsigned)o->tick,
                             (unsigned long long)m->value, (unsigned long long)o->value);
                }
            }
            o->tick = 0;
        }
    }
}

static uint32_t sNoteTaken; /* (the newest one taken: each comes with every packet until the next) */

static void note_take(uint32_t tick, uint64_t value) {
    if (tick != 0 && tick > sNoteTaken) {
        sNoteTaken = tick;
        SyncNote *o = &sNoteTheirs[tick / SYNC_EVERY % SYNC_SLOTS];
        o->tick = tick;
        o->value = value;
    }
}

static void send_alive(void) {
    uint8_t pkt[64];
    int k = head(pkt, T_ALIVE);
    put32(pkt + k, sTick);
    put32(pkt + k + 4, sNoteOutTick);
    put64(pkt + k + 8, sNoteOutValue);
    link_send(&sPeer, pkt, k + 16);
}

static void send_arrived(uint32_t tick, int answer) {
    uint8_t pkt[32];
    int k = head(pkt, T_ARRIVED);
    put32(pkt + k, tick);
    put32(pkt + k + 4, (uint32_t)answer);
    link_send(&sPeer, pkt, k + 8);
}

static void send_inputs(void) {
    uint8_t pkt[32 + REDUNDANT * PAD];
    uint32_t n = sHave[sMe] < REDUNDANT ? sHave[sMe] : REDUNDANT, base = sHave[sMe] - n, i;
    int k = head(pkt, T_INPUT);
    put32(pkt + k, base);
    put32(pkt + k + 4, n);
    put32(pkt + k + 8, sNoteOutTick);
    put64(pkt + k + 12, sNoteOutValue);
    k += 20;
    for (i = 0; i < n; i++) {
        memcpy(pkt + k + i * PAD, sIn[sMe][(base + i) % RING], PAD);
    }
    link_send(&sPeer, pkt, k + (int)(n * PAD));
}

static void send_bye(int why) {
    uint8_t pkt[32];
    int k = head(pkt, T_BYE), i;
    put32(pkt + k, (uint32_t)why);
    for (i = 0; i < 4; i++) {
        link_send(&sPeer, pkt, k + 4);
    }
}

/* The program ends in a match (START + SELECT held, an error: vita_main.c __wrap_exit): the other Vita is told at
   once, instead of finding out from the silence. */
void Vita_NetExit(void) {
    if (sOn && sConnected && !sEnding) {
        sEnding = 1;
        send_bye(BYE_QUIT);
        Vita_Log("net: the program ends in the match: the other Vita is told\n");
    }
}

/* The match is over: the other Vita is told, the reason is shown, and the game starts again as it normally is. */
static void ended(const char *why, int bye) {
    extern volatile int gVitaNetWaiting;
    if (sEnding) {
        return;
    }
    sEnding = 1;
    gVitaNetWaiting = 1;
    Vita_Log("net: the match has ended at blank %u: %s (%u comparisons with the other Vita, %u differed%s)\n", (unsigned)sTick, why, sCompared,
             sApartCount, sApartAt != 0 ? "" : "");
    if (sApartAt != 0) {
        Vita_Log("net: the two games differed from blank %u on\n", (unsigned)sApartAt);
    }
    if (sConnected && bye != 0) {
        send_bye(bye);
    }
    link_close();
    if (sTestLog != NULL) {
        fprintf(sTestLog, "end at blank %u\n", (unsigned)sTick);
        fclose(sTestLog);
        sTestLog = NULL;
    }
    if (sWarm && Vita_WarmCan()) {
        /* begun in the running game (the versus menu): back there, the game alone started again (vita_warm.c). Said
           on the screen only when it was not this player's own doing or the match's ordinary end. */
        if (bye != BYE_OVER) {
            Vita_TextScreen();
            psvDebugScreenPrintf("\e[H\e[2J\n  Multiplayer: %s\n%s\n  Press CROSS: back to the versus menu, with your own save.\n", why,
                                 sApartAt != 0 ? "\n  (The two Vitas' games had stopped agreeing: see log.txt.)\n" : "");
            wait_cross();
        }
        gVitaNetWaiting = 0;
        sPending = 2;
        Vita_WarmRestart(); /* (does not return) */
    }
    Vita_TextScreen();
    psvDebugScreenPrintf("\e[H\e[2J\n  Multiplayer: %s\n%s\n  Press CROSS: the game starts again with your own save.\n", why,
                         sApartAt != 0 ? "\n  (The two Vitas' games had stopped agreeing: see log.txt.)\n" : "");
    wait_cross();
    restart();
}

/* A match's own counts and flags, as the program's start has them. */
static void session_reset(void) {
    memset(sNoteMine, 0, sizeof(sNoteMine));
    memset(sNoteTheirs, 0, sizeof(sNoteTheirs));
    sNoteOutTick = 0;
    sNoteOutValue = 0;
    sNoteTaken = 0;
    sApartAt = 0;
    sApartCount = sCompared = 0;
    memset(sIn, 0, sizeof(sIn));
    sHave[0] = sHave[1] = 0;
    sTick = 0;
    sArrivedFlag = sMet = 0;
    sMyArrival = sTheirArrival = 0;
    sHeardUs = sStartUs = 0;
    sEnding = sShownWait = 0;
    sWaits = 0;
    sWaitUs = 0;
}

/* (vita_warm.c) the game is being started again inside the program, at blank `blank` of the program's count: a match
   begins with this start (the versus menu's pairing, or the development build's warmtest=), or one has ended. */
void Vita_NetWarm(unsigned blank) {
    extern int gVitaStartMode;
    if (sPending == 1) {
        session_reset();
        sTickBase = blank;
        sOn = 1;
        sWarm = 1;
        sConnected = gVitaNetTest <= 0;
        sMe = gVitaNetTest > 0 ? 0 : gVitaNetRole == 1 ? 0 : 1;
        match_setup();
        if (gVitaNetTest > 0) {
            sceIoRemove(NET_LOG);
        }
        Vita_Log("net: the game starts again as a match: player %d, the match's blanks counted from %u\n", sMe + 1, (unsigned)blank);
    } else if (sOn || sPending == 2) {
        int test = gVitaNetTest > 0;
        sOn = 0;
        sWarm = 0;
        sConnected = 0;
        sTickBase = 0;
        gVitaNetTest = 0;
        gVitaNetRole = 0;
        link_close();
        adhoc_down();
        match_undo();
        session_reset();
        if (!test) {
            gVitaStartMode = 38; /* (back where the match was begun: the versus menu) */
        }
        Vita_Log("net: the game starts again as it normally is\n");
    }
    sPending = 0;
}

/* (vita_warm.c, the development build's warmtest=<seconds>) the one-Vita test of a match, begun in the running game */
void Vita_NetTestBegin(int seconds) {
    gVitaNetTest = seconds;
    sSeed = 0x5EED1234;
    sDelay = 2;
    sPending = 1;
}

/* Nothing has come from the other Vita for longer than `us`. (The clock read here, after the last receive: a time
   taken before it lies before the moment of a packet it brought, and the difference goes round.) */
static int quiet_for(uint64_t us) {
    uint64_t now = now_us();
    return now > sHeardUs && now - sHeardUs > us;
}

/* What came from the other Vita. 1 when anything did. */
static int receive(void) {
    uint8_t in[PKT_MAX];
    NetAddr from;
    int n, any = 0;

    if (gVitaNetTest > 0) {
        return 0;
    }
    while ((n = link_recv(in, sizeof(in), &from)) > 0) {
        int type;
        if (n < 12 || get32(in) != MAGIC || !same_addr(&from, &sPeer)) {
            continue;
        }
        type = in[4];
        if (type == T_WELCOME && sMe == 1) { /* (the host did not hear the answer: once more) */
            uint8_t pkt[32];
            int k = head(pkt, T_READY);
            put32(pkt + k, sJoinNonce);
            link_send(&sPeer, pkt, k + 4);
            sHeardUs = now_us();
            continue;
        }
        if (get32(in + 8) != sSession) {
            continue;
        }
        any = 1;
        sHeardUs = now_us();
        switch (type) {
        case T_ALIVE:
            if (n >= 28) {
                note_take(get32(in + 16), get64(in + 20));
            }
            break;
        case T_ARRIVED:
            if (n >= 20) {
                if (sTheirArrival == 0) {
                    sTheirArrival = get32(in + 12);
                }
                if (sMet && get32(in + 16) == 0) {
                    send_arrived(sMyArrival, 1);
                }
            }
            break;
        case T_INPUT:
            if (n >= 32) {
                uint32_t base = get32(in + 12), cnt = get32(in + 16), i, other = (uint32_t)!sMe;
                note_take(get32(in + 20), get64(in + 24));
                if (cnt > REDUNDANT || n < 32 + (int)(cnt * PAD) || !sMet) {
                    break;
                }
                for (i = 0; i < cnt; i++) {
                    uint32_t t = base + i;
                    if (t == sHave[other] && t < sHave[sMe] + RING / 2) {
                        memcpy(sIn[other][t % RING], in + 32 + i * PAD, PAD);
                        sHave[other] = t + 1;
                    }
                }
            }
            break;
        case T_BYE:
            if (n >= 16 && get32(in + 12) == BYE_OVER && sWarm) {
                ended("the match is over.", BYE_OVER); /* (the other Vita came to the match's end a moment earlier) */
            }
            ended(n >= 16 && get32(in + 12) == BYE_APART ? "the two games no longer agreed (see log.txt): the match has ended." :
                  "the other player left the match.", 0);
            break;
        default:
            break;
        }
    }
    return any;
}

/* A wait for the other Vita: after two seconds it is said on the screen; SELECT + START held leaves the match. */
static void wait_screen(uint64_t since, const char *what) {
    uint64_t t = now_us();
    if (!sShownWait && t - since > 2000000) {
        sShownWait = 1;
        Vita_TextScreen();
        psvDebugScreenPrintf("\e[H\e[2J\n  Multiplayer: %s %s ...\n\n  (hold SELECT + START to leave the match)\n", what, sTheirName);
    }
    if ((pad_now() & (SCE_CTRL_SELECT | SCE_CTRL_START)) == (SCE_CTRL_SELECT | SCE_CTRL_START)) {
        ended("you left the match.", BYE_LEFT);
    }
}

/* The script of the one-Vita test: both players' buttons, the same in every run. In the menus only CROSS and the
   d-pad (the characters, the stage, the battle again); in a fight anything but START and SELECT. */
static void script_pad(int player, uint32_t tick, uint8_t *out) {
    extern int gVitaInMenus;
    uint32_t x = tick * 2654435761u ^ (uint32_t)(player + 1) * 0x9E3779B9u, btn = 0;
    x ^= x >> 15;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    memcpy(out, kIdle, PAD);
    if (gVitaInMenus) {
        uint32_t phase = (tick + (uint32_t)player * 17u) % 48u;
        if (phase < 4) {
            btn = 0x4000; /* CROSS */
        } else if (phase >= 20 && phase < 23 && (x & 3) == 0) {
            /* up or down (never left or right: on the character select's item sets those open the item panel, which
               only Triangle leaves, and Triangle could also leave the select itself) */
            btn = (x & 4) ? 0x0010 : 0x0040;
        }
    } else {
        static const uint32_t kPick[12] = {0x8000, 0x4000, 0x1000, 0x2000, 0x0400, 0x0800, 0x0010, 0x0020, 0x0040, 0x0080, 0x8800, 0x4400};
        uint32_t span = tick / 6u, y = span * 2246822519u ^ (uint32_t)(player + 1) * 3266489917u;
        y ^= y >> 16;
        btn = (y & 7) < 5 ? kPick[(y >> 3) % 12u] : 0;
        if ((y >> 8) & 1) {
            out[4] = (uint8_t)(y >> 9);  /* the left stick */
            out[5] = (uint8_t)(y >> 17);
        }
    }
    out[0] = (uint8_t)(~btn & 0xFF);
    out[1] = (uint8_t)(~(btn >> 8) & 0xFF);
}

/* (vita_gfx.c Port_PadRead) the clashes' Square: which fighter the local Vita's buttons are for */
int Vita_NetPadSide(int socket) {
    return sOn ? sMe : socket;
}

/* The match's first blank after the start-up (the character select is next): both must have arrived at the
   same blank, or their games are not the same. */
static void meet(uint32_t tick) {
    extern volatile int gVitaNetWaiting;
    uint64_t t0 = now_us(), last = 0;
    uint32_t k, t;

    sMet = 1;
    gVitaNetWaiting = 1;
    sMyArrival = tick;
    if (gVitaNetTest > 0) {
        sTheirArrival = tick;
    }
    while (sTheirArrival == 0) {
        uint64_t now = now_us();
        if (last == 0 || now - last > 50000) {
            send_arrived(tick, 0);
            last = now;
        }
        if (!receive()) {
            sceKernelDelayThread(500);
        }
        wait_screen(t0, "waiting at the character select for");
        if (quiet_for(60000000ull)) {
            ended("the other Vita did not come to the character select (a minute): the match has ended.", BYE_LOST);
        }
    }
    gVitaNetWaiting = 0;
    if (gVitaNetTest <= 0) {
        send_arrived(tick, 0);
    }
    /* the first `delay` blanks from here have nothing pressed on both (the inputs made now take effect then) */
    for (k = 0; k < 2; k++) {
        for (t = sHave[k]; t < tick + (uint32_t)sDelay; t++) {
            memcpy(sIn[k][t % RING], kIdle, PAD);
        }
        sHave[k] = tick + (uint32_t)sDelay;
    }
    Vita_Log("net: both at the character select at blank %u (%u ms after the game started); the other Vita's blank %u\n", (unsigned)tick,
             (unsigned)((now_us() - sStartUs) / 1000), (unsigned)sTheirArrival);
    if (sTheirArrival != tick) {
        ended("the two games came to the character select at different moments (they are not the same game): the match has ended.", BYE_APART);
    }
}

/* (menu_a_b.c) the start-up is over: the character select comes next */
void Port_NetArrived(void) {
    if (sOn && !sArrivedFlag) {
        sArrivedFlag = 1;
        Vita_Log("net: the start-up is over at blank %u\n", (unsigned)sTick);
    }
}

/* (plat_stub.c Port_VBlank) a new vertical blank begins. In the start-up: the line kept open, nothing waited for. After
   it: this player's input made now takes effect `delay` blanks on; then the other player's for THIS blank is waited for. */
void Port_NetBeginTick(unsigned tick) {
    uint64_t t0;
    static uint64_t lastAlive;

    if (!sOn) {
        return;
    }
    tick -= sTickBase; /* (a match begun in the running game counts its blanks from its own start) */
    sTick = tick;
    if (sStartUs == 0) {
        sStartUs = sHeardUs = now_us();
        if (gVitaNetTest > 0) {
            sTestLog = fopen(NET_LOG, "w");
        }
    }
    if (gVitaNetTest > 0) {
        /* the test: the game's thread held up now and then, by a draw of its own that differs from run to run */
        static uint32_t r;
        if (r == 0) {
            r = (uint32_t)now_us() | 1u;
        }
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        if ((r & 15) == 0) {
            sceKernelDelayThread((r >> 8) % 40000u);
        }
        if (tick % 60 == 0 && sTestLog != NULL) {
            fprintf(sTestLog, "%u %016llx\n", tick, Port_NetDigest(1));
            fflush(sTestLog); /* (the test runs' closing of the game keeps what was written) */
        }
        if (tick >= (unsigned)gVitaNetTest * 60u) {
            if (sWarm) { /* (warmtest=: the record is complete; the game goes on as it normally is) */
                Vita_Log("net: the test begun in the running game is over at blank %u\n", tick);
                ended("the test is over.", BYE_OVER);
            }
            ended("the test is over: compare ux0:data/bt3/net_test.txt with another run's.", 0);
        }
    }
    if (!sMet && !sArrivedFlag) {
        uint64_t now = now_us();
        note_mine(tick); /* (the start-up is compared too) */
        receive();
        note_check();
        if (gVitaNetTest <= 0 && now - lastAlive > 250000) {
            send_alive();
            lastAlive = now;
        }
        if (gVitaNetTest <= 0 && quiet_for(90000000ull)) {
            ended("nothing came from the other Vita for a minute and a half: the match has ended.", BYE_LOST);
        }
        return;
    }
    if (!sMet) {
        meet(tick);
    }
    {   /* this player's input, for the blank `delay` on */
        uint32_t at = tick + (uint32_t)sDelay;
        if (at >= sHave[sMe]) {
            uint8_t mine[PAD];
            if (gVitaNetTest > 0) {
                script_pad(sMe, at, mine);
            } else if (!Port_PadRead(0, mine)) {
                memcpy(mine, kIdle, PAD);
            }
            memcpy(sIn[sMe][at % RING], mine, PAD);
            sHave[sMe] = at + 1;
        }
    }
    if (gVitaNetTest > 0) {
        uint32_t other = (uint32_t)!sMe;
        while (sHave[other] <= tick + (uint32_t)sDelay) {
            script_pad((int)other, sHave[other], sIn[other][sHave[other] % RING]);
            sHave[other]++;
        }
    }
    send_inputs();
    receive();
    t0 = now_us();
    if (sHave[!sMe] <= tick) {
        extern volatile int gVitaNetWaiting;
        uint64_t last = t0;
        sWaits++;
        gVitaNetWaiting = 1;
        while (sHave[!sMe] <= tick) {
            uint64_t now = now_us();
            if (now - last > 4000) {
                send_inputs(); /* (the other may be waiting for a packet that was lost) */
                last = now;
            }
            if (!receive()) {
                sceKernelDelayThread(200);
            }
            wait_screen(t0, "waiting for");
            if (quiet_for(20000000ull)) {
                ended("the connection to the other Vita was lost.", BYE_LOST);
            }
        }
        gVitaNetWaiting = 0;
        sWaitUs += now_us() - t0;
        if (sShownWait) {
            sShownWait = 0; /* (the game's next picture takes the screen back) */
        }
    }
    note_mine(tick);
    note_check();
    if (tick % 3600 == 0) {
        Vita_Log("net: blank %u: %u waits for the other Vita in the last minute, %u ms in all; %u comparisons, %u differed\n", tick, sWaits,
                 (unsigned)(sWaitUs / 1000), sCompared, sApartCount);
        sWaits = 0;
        sWaitUs = 0;
    }
}

/* (plat_stub.c scePad2Read) what the game reads for a pad in this blank: from the exchange, for both players */
void Port_NetInput(int player, unsigned char *data) {
    if (player >= 0 && player < 2 && sMet && sTick < sHave[player]) {
        memcpy(data, sIn[player][sTick % RING], PAD);
    } else {
        memcpy(data, kIdle, PAD);
    }
}

unsigned Port_NetTick(void) {
    return sTick;
}

/* (menu_a_b.c Progress_Main, before the first run) the match's start: the versus mode's character select, one
   player against the other, a single battle, everything unlocked, the host's seed and time limit. */
void Vita_NetEnter(void *progress, void *save) {
    extern void Save_UnlockAll(void *opt);
    extern void Port_Srand(unsigned seed);
    extern int Port_Rand(void);
    extern void Rand_SeedByArray(unsigned *key, int keyLen);
    unsigned key[4];
    int k;

    if (!sOn || progress == NULL || save == NULL) {
        return;
    }
    *(int *)((char *)progress + 0x18) = 39;  /* mode: the character select */
    *(int *)((char *)progress + 0x620) = 1;  /* 1P vs 2P */
    *(int *)((char *)progress + 0x624) = 0;  /* a single battle */
    *(int *)((char *)progress + 0x630) = 0;
    Save_UnlockAll(save);
    for (k = 0; k < 6; k++) { /* (the host's battle settings: rule[k] of the save) */
        if (gVitaNetRules[k] >= 0 && gVitaNetRules[k] <= 8) {
            *(int *)((char *)save + 0xC34 + 4 * k) = gVitaNetRules[k];
        }
    }
    Port_Srand(sSeed);
    for (k = 0; k < 4; k++) {
        key[k] = (unsigned)Port_Rand();
    }
    Rand_SeedByArray(key, 4);
    Vita_Log("net: the match begins: player %d, seed %08x, input delay %d, battle settings %d %d %d %d %d %d\n", sMe + 1, sSeed, sDelay,
             gVitaNetRules[0], gVitaNetRules[1], gVitaNetRules[2], gVitaNetRules[3], gVitaNetRules[4], gVitaNetRules[5]);
}

/* (menu_a.c) back at the main menu: the match is over */
void Port_NetLeave(void) {
    if (sOn) {
        ended("the match is over.", BYE_OVER);
    }
}
