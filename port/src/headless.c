/*
 * Headless harness: stands in for the menu overlay. The game's own main loop is
 *     for (;;) { Overlay_Load(0); Progress_Main(0); Battle_Main(0); }
 * and Progress_Main is the menu's entry point: it returns when a battle has been set up. Here it sets one up
 * without any menu:
 *     BT3_REPLAY=<file>   a replay save file (0x1AC00 bytes: 0x38 header + the replay block)
 *     otherwise           the game's own attract-demo battle (CPU against CPU, one of nine fixed pairings)
 * and the second time it is called (the battle is over) it ends the program.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void Battle_ClearWork(void);
extern int BattleReplay_Load(void *buf, int size);
extern void Demo_SetupBattle(void);

#define REPLAY_FILE_SIZE 0x1AC00
#define REPLAY_HEADER 0x38
#define REPLAY_BLOCK 0x1ABA8

static int sBattles;
static int sFromMenu;

/* Linked with --wrap=Progress_Main: the game's call in Game_Main comes here. BT3_REPLAY or BT3_DEMO: the battle is
   set up without menus (below); otherwise the real menus run (the overlay's Progress_Main, src/menu/menu_a_b.c). */
extern int __real_Progress_Main(int arg);
int gPortMenuMode;

int __wrap_Progress_Main(int arg) {
    const char *path = getenv("BT3_REPLAY");

    if (path == NULL && getenv("BT3_DEMO") == NULL) {
        {
            int r;
            gPortMenuMode = 1; /* the renderer treats all 2D as one centred 4:3 page while the menus run */
            r = __real_Progress_Main(arg);
            gPortMenuMode = 0;
            sFromMenu = 1; /* a battle set up by the real menus follows: the traces apply to it too */
            return r;
        }
    }
#ifdef PORT_VITA
    if (getenv("BT3_SOAK") != NULL && path == NULL) { /* (Vita, the test runs' soak=1) one battle after another, never the end */
        extern int Port_SoakNext(int first);
        return Port_SoakNext(sBattles++ == 0);
    }
#endif
    if (sBattles++ > 0) {
        printf("bt3: battle finished\n");
        exit(0);
    }
#ifdef PORT_VITA
    {   /* The third archive (music, the fighters' voices, streamed sound effects) is opened by the first-run
           sequence of the menus (src/menu/menu_i_c.c FirstRun_Main), which a test fight leaves out: its music and the
           intros' lines were silent ("archive 2 file ... not found"). Opened here as the menus would. */
        extern int File_LoadPartitionNw(int pt);
        extern int File_WaitPartition(int pt);
        File_LoadPartitionNw(2);
        File_WaitPartition(2);
    }
#endif
    if (path != NULL) {
        static uint8_t buf[REPLAY_FILE_SIZE] __attribute__((aligned(16)));
        FILE *fp = fopen(path, "rb");

        if (fp == NULL || fread(buf, 1, sizeof(buf), fp) != sizeof(buf)) {
            fprintf(stderr, "bt3: cannot read replay %s (0x%X bytes expected)\n", path, REPLAY_FILE_SIZE);
            exit(2);
        }
        fclose(fp);
        Battle_ClearWork();
        if (!BattleReplay_Load(buf + REPLAY_HEADER, REPLAY_BLOCK)) {
            fprintf(stderr, "bt3: %s is not a replay this version accepts\n", path);
            exit(2);
        }
        printf("bt3: replay %s\n", path);
    } else if (getenv("BT3_DEMO_PICK") != NULL && atoi(getenv("BT3_DEMO_PICK")) >= 0 && atoi(getenv("BT3_DEMO_PICK")) <= 8) {
        /* one of the attract demo's nine pairings, chosen (Demo_SetupBattle in src/battle/view_b_c.c draws it at
           random, and the game's random numbers start the same every time: always the same pairing) */
        static const int kPair[9][3] = {{0xA, 0x93, 0x1D}, {0x2A, 0x6C, 6}, {0x19, 0x5C, 2}, {0x27, 0x60, 1}, {0x17, 0x6F, 1},
                                        {0x49, 0x5B, 0x10}, {0, 0x1E, 0x19}, {0x32, 0x72, 5}, {5, 0x25, 1}};
        extern unsigned Rand_Range(unsigned n);
        extern void BattleSetup_SetRule(int screenMode, int mode, int bgm, int timeLimit, int announcer, int stage, int unk10);
        extern void BattleSetup_SetSide(int sideNo, int control, int pad, int memberCount, int unk1FC, int unk200, int lead, void *bits);
        extern void BattleSetup_SetMember(int sideNo, int idx, int chara, int costume, int variant, int cpuLevel, float health, void *items);
        extern void BattleSetup_Finish(void);
        extern int CpuLevel_FromSetting(unsigned setting);
        int pick = atoi(getenv("BT3_DEMO_PICK")), i;
        int bgm = (int)Rand_Range(9) + 8;
        /* BT3_DEMO_STAGE=<id>: on that stage instead; BT3_DEMO_CHARA1 / 2=<id>: those fighters;
           BT3_DEMO_DUEL=1: as the Duel menu's COM against COM (mode 0: the fighters' intros, the announcer) */
        int stage = getenv("BT3_DEMO_STAGE") != NULL ? atoi(getenv("BT3_DEMO_STAGE")) : kPair[pick][2];
        int chara[2] = {getenv("BT3_DEMO_CHARA1") != NULL ? atoi(getenv("BT3_DEMO_CHARA1")) : kPair[pick][0],
                        getenv("BT3_DEMO_CHARA2") != NULL ? atoi(getenv("BT3_DEMO_CHARA2")) : kPair[pick][1]};
        int duel = getenv("BT3_DEMO_DUEL") != NULL && atoi(getenv("BT3_DEMO_DUEL")) != 0;

        Battle_ClearWork();
        if (duel) {
            BattleSetup_SetRule(0, 0, bgm, 0, 1, stage, 1);
        } else {
            BattleSetup_SetRule(0, 7, bgm, 5, 4, stage, 0);
        }
        BattleSetup_SetSide(0, 2, 0, 1, 1, 1, 0, NULL);
        BattleSetup_SetSide(1, 2, 1, 1, 1, 1, 0, NULL);
        for (i = 0; i < 2; i++) {
            BattleSetup_SetMember(i, 0, chara[i], 0, 0, CpuLevel_FromSetting(2), 100.0f, NULL);
        }
        BattleSetup_Finish();
        printf("bt3: %s battle, pairing %d (characters %d and %d, stage %d)\n", duel ? "COM against COM duel" : "attract-demo", pick, chara[0], chara[1], stage);
    } else {
        Demo_SetupBattle();
        printf("bt3: attract-demo battle\n");
    }
    fflush(stdout);
    return 0;
}

#ifdef PORT_VITA
/* The test runs' soak (bt3.txt soak=1, the development build): battles without end, to find what goes wrong after a
   long time. In turn a COM against COM duel and a team battle of three against three with a quarter of the health,
   whose fighters are knocked out and replaced quickly (a new model loaded each time), over the attract demo's
   characters, those with a seam (Videl, the Great Saiyaman, Hercule and others) and the demo's stages. */
int Port_SoakNext(int first) {
    static const int kChara[] = {0xA, 0x93, 0x38, 0x2A, 0x6C, 0x37, 0x19, 0x5C, 0x39, 0x27, 0x60, 0x6E, 0x17, 0x6F, 0x86,
                                 0x49, 0x5B, 0, 0x1E, 0x32, 0x72, 5, 0x25};
    static const int kStage[] = {0x1D, 6, 2, 1, 0x10, 0x19, 5};
    static int n;
    extern unsigned Rand_Range(unsigned n);
    extern void BattleSetup_SetRule(int screenMode, int mode, int bgm, int timeLimit, int announcer, int stage, int unk10);
    extern void BattleSetup_SetSide(int sideNo, int control, int pad, int memberCount, int unk1FC, int unk200, int lead, void *bits);
    extern void BattleSetup_SetMember(int sideNo, int idx, int chara, int costume, int variant, int cpuLevel, float health, void *items);
    extern void BattleSetup_Finish(void);
    extern int CpuLevel_FromSetting(unsigned setting);
    extern void Vita_Log(const char *fmt, ...);
    const int nc = (int)(sizeof(kChara) / sizeof(kChara[0]));
    int team = n & 1, members = team ? 3 : 1, stage = kStage[n % (int)(sizeof(kStage) / sizeof(kStage[0]))], i, k;

    if (first) {
        extern int File_LoadPartitionNw(int pt);
        extern int File_WaitPartition(int pt);
        File_LoadPartitionNw(2);
        File_WaitPartition(2);
    }
    Battle_ClearWork();
    BattleSetup_SetRule(0, 0, (int)Rand_Range(9) + 8, 0, 1, stage, 1);
    for (i = 0; i < 2; i++) {
        BattleSetup_SetSide(i, 2, i, members, 1, 1, 0, NULL);
        for (k = 0; k < members; k++) {
            BattleSetup_SetMember(i, k, kChara[(n * 5 + i * 3 + k * 7) % nc], 0, 0, CpuLevel_FromSetting(2), team ? 25.0f : 40.0f, NULL);
        }
    }
    BattleSetup_Finish();
    Vita_Log("soak: battle %d, %s on stage %d: %d against %d\n", n, team ? "three against three" : "a duel", stage,
             kChara[(n * 5) % nc], kChara[(n * 5 + 3) % nc]);
    n++;
    return 0;
}
#endif

/* Overlay_Load reads the menu overlay to its PS2 address: there is nothing to read on PC (size 0). */
int sceOpen(char *name, int flags) { (void)name; (void)flags; return 3; }
int sceLseek(int fd, int offset, int whence) { (void)fd; (void)offset; (void)whence; return 0; }
int sceRead(int fd, void *buf, int size) { (void)fd; (void)buf; return size; }
int sceClose(int fd) { (void)fd; return 0; }

/*
 * BT3_TRACE=<n>: every n vertical blanks, print the state of the fight (sequence state, battle clock words, health
 * and position of the two fighters). Read-only; called from the vertical blank (plat_stub.c).
 */
/* A pointer variable of the game: 4 bytes wide in the 64-bit build too (port/tools/ptr32.py does this for the game's
   own files; a declaration written in a port file has to say it itself). */
#if defined(__x86_64__) && defined(__clang__)
#undef __ptr32 /* the Windows headers of mingw define these two away */
#undef __uptr
#define GAME_PTR *__ptr32 __uptr
#else
#define GAME_PTR *
#endif
extern struct { int state; } GAME_PTR gBtlSeq;
extern int *BtlSeq_GetClock(void);
extern int BtlCharApi_GetHp(int objId);
extern void BtlCharApi_GetPos(int objId, float *out);

void Port_Trace(unsigned vblanks) {
    static int every = -1;
    float p[2][4] __attribute__((aligned(16)));
    unsigned u[2][3];
    int *clock;
    int i;

    if (every < 0) {
        every = getenv("BT3_TRACE") != NULL ? atoi(getenv("BT3_TRACE")) : 0;
    }
    /* BT3_AT=<tick>[,<tick>...]: when the battle clock first shows one of these tick counts, print the fight state
       and write the heap to port/build/heap_<tick>.bin (to compare with console save states made at those ticks). */
#ifndef PORT_VITA /* the dumps below write memory at the PS2's addresses, where the PC builds have the heap */
    if ((sBattles != 0 || sFromMenu) && gBtlSeq != NULL && gBtlSeq->state >= 2 && getenv("BT3_AT") != NULL) {
        static unsigned last = 0xFFFFFFFFu;
        unsigned now = (unsigned)BtlSeq_GetClock()[0];

        if (now != last) {
            const char *t = getenv("BT3_AT");

            last = now;
            while (*t != '\0') {
                if ((unsigned)strtoul(t, (char **)&t, 0) == now) {
                    char name[64];
                    FILE *fp;

                    snprintf(name, sizeof(name), "port/build/heap_%u.bin", now);
                    fp = fopen(name, "wb"); /* a developer's dump: skipped where port/build does not exist */
                    if (fp != NULL) {
                        fwrite((void *)0x3BE730, 1, 0x1EFB014 - 0x3BE730, fp);
                        fclose(fp);
                    }
#ifndef _WIN32 /* the linker symbols of an ELF program */
                    {   /* the game's global variables too: [__data_start, _end), base address first */
                        extern char __data_start[], _end[];
                        uint32_t base = (uint32_t)(uintptr_t)__data_start;

                        snprintf(name, sizeof(name), "port/build/glob_%u.bin", now);
                        fp = fopen(name, "wb");
                        if (fp != NULL) {
                            fwrite(&base, 4, 1, fp);
                            fwrite(__data_start, 1, (size_t)(_end - __data_start), fp);
                            fclose(fp);
                        }
                        snprintf(name, sizeof(name), "port/build/heap_%u.bin", now);
                    }
#endif
                    printf("bt3: tick %u (vblank %u, seq %d): hp %d %d; %s\n", now, vblanks, gBtlSeq->state,
                           BtlCharApi_GetHp(0), BtlCharApi_GetHp(1), name);
                    fflush(stdout);
                }
                if (*t == ',') {
                    t++;
                }
            }
        }
    }
    /* BT3_DUMP=<file>: 600 vertical blanks after the battle sequence reaches its last state, write the game heap
       (PS2 addresses 0x3BE730..0x1EFB014) to the file and stop: to be compared with a console memory dump. */
    if ((sBattles != 0 || sFromMenu) && gBtlSeq != NULL && gBtlSeq->state == 6 && getenv("BT3_DUMP") != NULL) {
        static unsigned since;

        if (since == 0) {
            since = vblanks;
        } else if (vblanks - since == 600) {
            FILE *fp = fopen(getenv("BT3_DUMP"), "wb");

            if (fp != NULL) {
                fwrite((void *)0x3BE730, 1, 0x1EFB014 - 0x3BE730, fp);
                fclose(fp);
            }
            clock = BtlSeq_GetClock();
            printf("bt3: end of battle: gBtlSeq %p clock %08x %08x %08x %08x hp %d %d; heap dumped\n", (void *)gBtlSeq,
                   clock[0], clock[1], clock[2], clock[3], BtlCharApi_GetHp(0), BtlCharApi_GetHp(1));
            exit(0);
        }
    }
#endif
    if ((sBattles != 0 || sFromMenu) && gBtlSeq != NULL && getenv("BT3_TRACE_SEQ") != NULL) { /* when the battle sequence changes state */
        static int last = -1;
        if (gBtlSeq->state != last) {
            printf("seq: state %d begins at vertical blank %u\n", gBtlSeq->state, vblanks);
            fflush(stdout);
            last = gBtlSeq->state;
        }
    }
    if (every <= 0 || vblanks % (unsigned)every != 0 || sBattles == 0 || gBtlSeq == NULL || gBtlSeq->state < 2) {
        return;
    }
    clock = BtlSeq_GetClock();
    for (i = 0; i < 2; i++) {
        BtlCharApi_GetPos(i, p[i]);
        memcpy(u[i], p[i], 12);
    }
    /* BT3_CAMCHECK=<vblank>: test the pad fighter's line of sight (camera eye -> camera target) against EVERY
       triangle of the stage collision mesh, and say what the game's own sweep answers for the same segment. */
    if (getenv("BT3_CAMCHECK") != NULL && vblanks == (unsigned)atoi(getenv("BT3_CAMCHECK"))) {
        extern void *BtlChar_FindByObjId(int objId);
        extern struct { int nodeCount, unk4, unk8, nodeOfs, polyOfs, vtxOfs; } *gStgColMesh;
        extern void *ColMesh_GetPoly(void *mesh, int idx);
        extern void ColMesh_GetPolyVerts(void *mesh, void *poly, float *v0, float *v1, float *v2);
        extern int BtlCam_TraceStage(float *out, float *from, float *to, float *frac, int *hitObj);
        char *chr = BtlChar_FindByObjId(0);
        float *eye = (float *)(chr + 0x420), *tgt = (float *)(chr + 0x450);
        float v0[4] __attribute__((aligned(16))), v1[4] __attribute__((aligned(16))), v2[4] __attribute__((aligned(16)));
        float out[4] __attribute__((aligned(16))), frac = -1.0f;
        int n = gStgColMesh->unk4, hits = 0, obj = -1, k, r; /* unk4: taken as the polygon count */
        double d[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};

        printf("camcheck: vblank %u, eye (%.2f %.2f %.2f) target (%.2f %.2f %.2f), %d polygons (counts in the header: %d, %d)\n", vblanks,
               (double)eye[0], (double)eye[1], (double)eye[2], (double)tgt[0], (double)tgt[1], (double)tgt[2], n, gStgColMesh->unk4,
               gStgColMesh->unk8);
        for (k = 0; k < n; k++) {
            double e1[3], e2[3], pv[3], tv[3], qv[3], det, u, v, t;
            int j;
            ColMesh_GetPolyVerts(gStgColMesh, ColMesh_GetPoly(gStgColMesh, k), v0, v1, v2);
            for (j = 0; j < 3; j++) { e1[j] = v1[j] - v0[j]; e2[j] = v2[j] - v0[j]; tv[j] = eye[j] - v0[j]; }
            pv[0] = d[1] * e2[2] - d[2] * e2[1]; pv[1] = d[2] * e2[0] - d[0] * e2[2]; pv[2] = d[0] * e2[1] - d[1] * e2[0];
            det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
            if (det > -1e-9 && det < 1e-9) { continue; }
            u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) / det;
            qv[0] = tv[1] * e1[2] - tv[2] * e1[1]; qv[1] = tv[2] * e1[0] - tv[0] * e1[2]; qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
            v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) / det;
            t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) / det;
            if (u >= 0 && v >= 0 && u + v <= 1 && t >= 0 && t <= 1) {
                unsigned flags;
                memcpy(&flags, ColMesh_GetPoly(gStgColMesh, k), 4);
                if (hits++ < 6) {
                    printf("camcheck:   polygon %d crosses the line of sight at %.3f of the way (first word %08x)\n", k, t, flags);
                }
            }
        }
        r = BtlCam_TraceStage(out, eye, tgt, &frac, &obj);
        printf("camcheck: %d polygons cross the line of sight; the game's sweep (eye -> target) returns %d, fraction %.3f, zone %d\n", hits, r,
               (double)frac, obj);
        r = BtlCam_TraceStage(out, tgt, eye, &frac, &obj);
        printf("camcheck: the game's sweep the other way (target -> eye) returns %d, fraction %.3f, zone %d\n", r, (double)frac, obj);
        fflush(stdout);
    }
    {   /* the pad fighter's camera: eye position (fighter block + 0x420) against the ground height under the fighter */
        extern float BtlCharApi_GetGroundY(int objId);
        extern void *BtlChar_FindByObjId(int objId);
        float *chr = BtlChar_FindByObjId(0);
        if (chr != NULL && getenv("BT3_TRACE_CAM") != NULL) {
            float *eye = (float *)((char *)chr + 0x420), g = BtlCharApi_GetGroundY(0);
            unsigned ue[3], ug;
            memcpy(ue, eye, 12);
            memcpy(&ug, &g, 4);
            printf("cam tick %u eye %08x %08x %08x ground %08x\n", (unsigned)clock[0], ue[0], ue[1], ue[2], ug);
        }
    }
    printf("vb %7u seq %d clock %08x %08x %08x %08x | hp %6d %6d | p0 %08x %08x %08x | p1 %08x %08x %08x\n", vblanks,
           gBtlSeq->state, clock[0], clock[1], clock[2], clock[3], BtlCharApi_GetHp(0), BtlCharApi_GetHp(1),
           u[0][0], u[0][1], u[0][2], u[1][0], u[1][1], u[1][2]);
    fflush(stdout);
}
