/*
 * The start menu, on the text screen before the game's first picture: SELECT held while the game starts.
 *
 *   Game            Budokai Tenkaichi 3, or Team BT4's Budokai Tenkaichi 4 B14 Rev2 (BT4decomp's game code, built
 *                   for the Vita as app0:bt4.bin: build.py BT3_BT4) when the package carries it and the player's BT4
 *                   files are in ux0:data/bt3/bt4/. The choice is kept (ux0:data/bt3/start.txt): BT4 then starts
 *                   straight away each time, until it is changed here again.
 *   Language        Spanish, German, French or Italian from the player's European disc (vita_eu.c; only when its
 *                   archive is there). At the first start that finds the archive the list shows by itself.
 *   Unlock all      a cheat: the game's own unlock (Save_UnlockAll, a routine the game has but never calls): every
 *                   character, stage, song and item, and 4,850,000 Zenni, given to the save the game loads now. The
 *                   game keeps it when it saves next.
 *   Multiplayer     (BT3) a match against another Vita, ad hoc or over the same Wi-Fi network: host or join
 *                   (vita_net.c, after the disc's files are found)
 *   Start           (CIRCLE or START too)
 *
 * BT4 that does not get going (it crashed or froze before its first pictures: ux0:data/bt3/bt4/starting.txt is
 * still there at the next start) is turned off with a message, so the package cannot get stuck starting it.
 */
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>

#include "vita_eu.h"
#include "vita_home.h"

extern void Vita_Log(const char *fmt, ...);
extern int psvDebugScreenPrintf(const char *fmt, ...);
extern int sceAppMgrLoadExec(const char *appPath, char *const argv[], void *optParam);

#define START_FILE "ux0:data/bt3/start.txt"         /* "bt4": the package starts BT4 */
#define BT4_STARTING "ux0:data/bt3/bt4/starting.txt" /* BT4 was started and has not drawn yet */
#define BT4_PROGRAM "app0:bt4.bin"

int gVitaCheatUnlock;     /* the save the game loads next is given everything (src/menu/menu_i_b.c BootCard_OnFlowDone) */
int gVitaStartMenuForce;  /* (development build, startmenu=1 in bt3.txt) the menu as if SELECT were held: the test runs' */

/* The text screens in Spanish when the game is (the European disc's Spanish, vita_eu.c; BT4's Spanish release): the
   text screen's font has no accents, so the words go without them. */
static int sEs;
#define ES(english, spanish) (sEs ? (spanish) : (english))

static void es_update(void) {
#ifdef BT3_BT4
    extern int Port_Setting(const char *name, int def);
    sEs = Port_Setting("bt4lang", 0) == 1;
#else
    sEs = VitaEu_Lang() == 1;
#endif
}

static unsigned pad_now(void) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    return pad.buttons;
}

/* waits for a new press of one of `mask`; returns the buttons newly pressed */
static unsigned press(unsigned mask) {
    unsigned prev = pad_now(), now;
    for (;;) {
        sceKernelDelayThread(16000);
        now = pad_now();
        if ((now & ~prev) & mask) {
            return now & ~prev;
        }
        prev = now;
    }
}

static int select_held(void) {
    return (pad_now() & SCE_CTRL_SELECT) != 0 || gVitaStartMenuForce;
}

static int exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static void write_text(const char *path, const char *text) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, text, strlen(text));
        sceIoClose(fd);
    }
}

static int start_bt4_chosen(void) {
    char b[8] = {0};
    SceUID fd = sceIoOpen(START_FILE, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    return strncmp(b, "bt4", 3) == 0;
}

#ifndef BT3_BT4
/* BT4 was started and has not drawn yet: "1" in starting.txt (a "0" there, as the test runs leave it, is not) */
static int bt4_was_starting(void) {
    char b[4] = {0};
    SceUID fd = sceIoOpen(BT4_STARTING, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    return b[0] == '1';
}
#endif

static void release_buttons(void) {
    while (pad_now() & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_START | SCE_CTRL_SELECT)) { /* (not a press in the game) */
        sceKernelDelayThread(16000);
    }
}

#ifndef BT3_BT4
/* BT4 in the package and its files in place: 1; else 0, and `why` says what is missing. Looked for once a run (the
   images in each place are opened to look inside; the Game row's guide asks each time it shows). */
static int bt4_ready(const char **why) {
    static int known = -1;
    static const char *knownWhy;
    if (known >= 0) {
        *why = knownWhy;
        return known;
    }
    if (!exists(BT4_PROGRAM)) {
        *why = ES("this package has no BT4 program", "este paquete no tiene el programa de BT4");
        return 0;
    }
    if (!(exists("ux0:data/bt3/bt4/SLUS_219.78") && exists("ux0:data/bt3/bt4/BIN/DBZ4.BIN")) &&
        !(exists("ux0:data/bt3/bt4/es/SLUS_219.78") && exists("ux0:data/bt3/bt4/es/BIN/DBZ4.BIN"))) { /* (es: the Spanish release) */
        extern int Vita_IsoFind(const char *dir, const char *file, int keep, char *out, int size, int *seen);
        char iso[300];
        static const char *const kAlso[] = VITA_BT4_ALSO; /* (where players put them besides: the BT4 program looks there too) */
        unsigned k;
        int found = Vita_IsoFind("ux0:data/bt3/bt4", "BIN/DBZ4.BIN", 0, iso, sizeof(iso), NULL) ||
                    Vita_IsoFind("ux0:data/bt3/bt4/es", "BIN/DBZ4.BIN", 0, iso, sizeof(iso), NULL); /* (or its .iso: vita_iso.c) */
        for (k = 0; !found && k < sizeof(kAlso) / sizeof(kAlso[0]); k++) {
            char slus[64], bin[64];
            snprintf(slus, sizeof(slus), "%s/SLUS_219.78", kAlso[k]);
            snprintf(bin, sizeof(bin), "%s/BIN/DBZ4.BIN", kAlso[k]);
            found = (exists(slus) && exists(bin)) || Vita_IsoFind(kAlso[k], "BIN/DBZ4.BIN", 0, iso, sizeof(iso), NULL);
            if (found) {
                Vita_Log("vita: BT4's files are in %s (not ux0:data/bt3/bt4)\n", kAlso[k]);
            }
        }
        if (!found) {
            *why = ES("copy your BT4 B14 Rev2 disc's .iso (or its SLUS_219.78,\n  BIN and DATA) to ux0:data/bt3/bt4/",
                      "copia el .iso de tu disco BT4 B14 Rev2 (o su SLUS_219.78,\n  BIN y DATA) en ux0:data/bt3/bt4/");
            knownWhy = *why;
            return known = 0;
        }
    }
    *why = knownWhy = NULL;
    return known = 1;
}

static void start_bt4(void) {
    int r;
    write_text(BT4_STARTING, "1");
    Vita_Log("vita: starting Budokai Tenkaichi 4 (%s)\n", BT4_PROGRAM);
    psvDebugScreenPrintf(ES("\e[H\e[2J\n  Starting Budokai Tenkaichi 4 ...\n", "\e[H\e[2J\n  Iniciando Budokai Tenkaichi 4 ...\n"));
    r = sceAppMgrLoadExec(BT4_PROGRAM, NULL, NULL);
    sceIoRemove(BT4_STARTING);
    Vita_Log("vita: BT4 could not be started (%08x)\n", (unsigned)r);
    psvDebugScreenPrintf(ES("\n  \e[31mBT4 could not be started (%08x).\e[39m Press CROSS.\n",
                            "\n  \e[31mNo se pudo iniciar BT4 (%08x).\e[39m Pulsa X.\n"), (unsigned)r);
    press(SCE_CTRL_CROSS);
    psvDebugScreenPrintf("\e[H\e[2J");
}
#else
/* BT4's language (Vita Settings > Language, src/menu/menu_x_c.c): its English or its Spanish release (vita_main.c
   find_disc: the English one in ux0:data/bt3/bt4, the Spanish one in ux0:data/bt3/bt4/es). The choice is kept in
   bt3_settings.txt (bt4lang= 0 English, 1 Spanish) only once the game got going in it: until then it is tried, from
   lang_try.txt ("n1": the next start tries Spanish; "s1": this start is trying it). A try that did not get going is
   dropped at the next start, which is in the language kept. */
#define BT4_LANG_TRY "ux0:data/bt3/bt4/lang_try.txt"
extern int Port_Setting(const char *name, int def);
extern void Port_SettingSave(const char *name, int value);
extern void Port_SettingsWrite(void);

static int read_try(char *state) {
    char b[4] = {0};
    SceUID fd = sceIoOpen(BT4_LANG_TRY, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }
    sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    *state = b[0];
    return b[1] == '1' ? 1 : b[1] == '0' ? 0 : -1;
}

/* (vita_main.c find_disc) the language to start in: 0 English, 1 Spanish (-1: none chosen yet) */
int Vita_Bt4LangStart(void) {
    char state = 0;
    int lang = read_try(&state);
    if (lang >= 0 && state == 'n') {
        write_text(BT4_LANG_TRY, lang ? "s1" : "s0");
        Vita_Log("vita: BT4 in %s, as chosen in the options (kept once the game gets going)\n", lang ? "Spanish" : "English");
        return lang;
    }
    if (lang >= 0) {
        sceIoRemove(BT4_LANG_TRY);
        Vita_Log("vita: BT4 in %s did not get going the last time: the language kept instead\n", lang ? "Spanish" : "English");
    }
    return Port_Setting("bt4lang", -1);
}

/* (Vita Settings > Language) the game started again in `lang`; returns only when it could not be */
void Vita_Bt4LangSwitch(int lang) {
    int r;
    write_text(BT4_LANG_TRY, lang ? "n1" : "n0");
    write_text(BT4_STARTING, "1");
    Vita_Log("vita: the options chose BT4 in %s: the game starts again\n", lang ? "Spanish" : "English");
    sceKernelDelayThread(100000);
    r = sceAppMgrLoadExec(BT4_PROGRAM, NULL, NULL);
    sceIoRemove(BT4_LANG_TRY);
    sceIoRemove(BT4_STARTING);
    Vita_Log("vita: BT4 could not be started again (%08x)\n", (unsigned)r);
}

/* (BT4's program) its first pictures are up: it got going (the BT3 program's guard, above); a language being tried
   is kept */
void Vita_Bt4Started(void) {
    char state = 0;
    int lang = read_try(&state);
    sceIoRemove(BT4_STARTING);
    if (lang >= 0 && state == 's') {
        extern int Vita_Spanish(void);
        sceIoRemove(BT4_LANG_TRY);
        if (lang == Vita_Spanish()) {
            Port_SettingSave("bt4lang", lang);
            Port_SettingsWrite();
            Vita_Log("vita: BT4 got going in %s: kept\n", lang ? "Spanish" : "English");
        } else { /* (that release is not there: the other one started) */
            Vita_Log("vita: BT4 was to start in %s, but that release is not there: not kept\n", lang ? "Spanish" : "English");
        }
    }
}
#endif

/* The other game of the package from the game's own options (Vita Settings > Game, src/menu/menu_x_c.c), as the start
   menu's Game does it: the choice is kept, and the other program started in this one's place.
   Vita_GameSwitchCheck: 0 it can be started, 1 the package has no BT4 program, 2 the player's BT4 files are not there.
   Vita_GameSwitch returns only when the other program could not be started (the choice is then taken back). */
/* Unlock All (the options' Vita Settings, src/menu/menu_x_c.c): the save as it was before it was turned on, kept in a
   file of its own so that turning it off can give the save its own unlocks back. Each game its own (their saves
   differ). */
#ifdef BT3_BT4
#define UNLOCK_BACKUP "ux0:data/bt3/bt4/unlock_backup.bin"
#else
#define UNLOCK_BACKUP "ux0:data/bt3/unlock_backup.bin"
#endif

/* 1: kept (now, or it was already: the first copy is the one before everything was unlocked) */
int Vita_UnlockBackupWrite(const void *save, int size) {
    SceUID fd;
    int n;
    if (exists(UNLOCK_BACKUP)) {
        return 1;
    }
    fd = sceIoOpen(UNLOCK_BACKUP ".new", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        Vita_Log("vita: unlock all: the save's copy could not be made (%08x)\n", (unsigned)fd);
        return 0;
    }
    n = sceIoWrite(fd, save, (SceSize)size);
    sceIoClose(fd);
    if (n != size || sceIoRename(UNLOCK_BACKUP ".new", UNLOCK_BACKUP) < 0) {
        sceIoRemove(UNLOCK_BACKUP ".new");
        Vita_Log("vita: unlock all: the save's copy could not be written\n");
        return 0;
    }
    Vita_Log("vita: unlock all: the save as it was kept in %s\n", UNLOCK_BACKUP);
    return 1;
}

/* 1: the copy was read into `save` */
int Vita_UnlockBackupRead(void *save, int size) {
    SceUID fd = sceIoOpen(UNLOCK_BACKUP, SCE_O_RDONLY, 0);
    int n;
    if (fd < 0) {
        return 0;
    }
    n = sceIoRead(fd, save, (SceSize)size);
    sceIoClose(fd);
    return n == size;
}

void Vita_UnlockBackupDrop(void) {
    sceIoRemove(UNLOCK_BACKUP);
}

int Vita_UnlockBackupThere(void) {
    return exists(UNLOCK_BACKUP);
}

int Vita_GameSwitchCheck(void) {
#ifndef BT3_BT4
    const char *why;
    if (!exists(BT4_PROGRAM)) {
        return 1;
    }
    return bt4_ready(&why) ? 0 : 2;
#else
    return 0;
#endif
}

void Vita_GameSwitch(void) {
    int r;
#ifndef BT3_BT4
    write_text(START_FILE, "bt4");
    write_text(BT4_STARTING, "1");
    Vita_Log("vita: the options chose Budokai Tenkaichi 4 (%s)\n", BT4_PROGRAM);
    sceKernelDelayThread(100000);
    r = sceAppMgrLoadExec(BT4_PROGRAM, NULL, NULL);
    sceIoRemove(BT4_STARTING);
    write_text(START_FILE, "bt3");
    Vita_Log("vita: BT4 could not be started (%08x)\n", (unsigned)r);
#else
    write_text(START_FILE, "bt3");
    Vita_Log("vita: the options chose Budokai Tenkaichi 3\n");
    sceKernelDelayThread(100000);
    r = sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
    write_text(START_FILE, "bt4");
    Vita_Log("vita: BT3 could not be started (%08x)\n", (unsigned)r);
#endif
}

#ifndef BT3_BT4
/* Multiplayer: host or join, ad hoc or Wi-Fi; the time limit the host gives the match. 1: a choice was made (the
   game then starts as a match: vita_net.c). */
static int net_menu(void) {
    extern int gVitaNetRole, gVitaNetLink, gVitaNetRules[6];
    static const char *const kTimeEn[6] = {"the game's (240 s)", "60 seconds", "90 seconds", "180 seconds", "240 seconds", "none"};
    static const char *const kTimeEs[6] = {"el del juego (240 s)", "60 segundos", "90 segundos", "180 segundos", "240 segundos", "sin limite"};
    const char *const *kTime = sEs ? kTimeEs : kTimeEn;
    int sel = 0, time = gVitaNetRules[0] + 1;
    unsigned p;

    for (;;) {
        static const char *const kItemEn[6] = {"Host a match (ad hoc)", "Join a match (ad hoc)", "Host a match (Wi-Fi, the same network)",
                                               "Join a match (Wi-Fi, the same network)", "Time limit when hosting: ", "Back"};
        static const char *const kItemEs[6] = {"Crear una partida (ad hoc)", "Unirse a una partida (ad hoc)", "Crear una partida (Wi-Fi, la misma red)",
                                               "Unirse a una partida (Wi-Fi, la misma red)", "Tiempo limite al crearla: ", "Volver"};
        const char *const *kItem = sEs ? kItemEs : kItemEn;
        int i;
        psvDebugScreenPrintf(ES("\e[H\e[2J\n  Multiplayer: a match against another Vita\n\n", "\e[H\e[2J\n  Multijugador: una partida contra otra Vita\n\n"));
        for (i = 0; i < 6; i++) {
            psvDebugScreenPrintf("  %s %s%s\e[39m\n", i == sel ? "\e[33m>" : " ", kItem[i], i == 4 ? kTime[time] : "");
        }
        psvDebugScreenPrintf(ES("\n  Both Vitas need this same version of the game and the\n"
                                "  same disc files. Ad hoc: the two Vitas near each other,\n"
                                "  no router (the system asks to connect). Wi-Fi: both\n"
                                "  connected to the same network (Settings > Network).\n\n"
                                "  The match is the versus mode, player 1 (the host)\n"
                                "  against player 2, everything unlocked, with a save of\n"
                                "  its own; your save is not touched. It ends at the\n"
                                "  main menu; then the game starts again as usual.\n\n"
                                "  Up / down, CROSS to choose; CIRCLE: back.\n",
                                "\n  Las dos Vitas necesitan esta misma version del juego y\n"
                                "  los mismos archivos del disco. Ad hoc: las dos Vitas\n"
                                "  cerca, sin router (el sistema pide conectar). Wi-Fi:\n"
                                "  las dos en la misma red (Ajustes > Red).\n\n"
                                "  La partida es el modo versus, jugador 1 (quien la crea)\n"
                                "  contra jugador 2, todo desbloqueado, con una partida\n"
                                "  guardada propia; la tuya no se toca. Termina en el\n"
                                "  menu principal; luego el juego empieza como siempre.\n\n"
                                "  Arriba / abajo, X para elegir; CIRCULO: volver.\n"));
        p = press(SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
        if (p & SCE_CTRL_CIRCLE) {
            return 0;
        }
        if (p & SCE_CTRL_UP) {
            sel = (sel + 5) % 6;
        } else if (p & SCE_CTRL_DOWN) {
            sel = (sel + 1) % 6;
        } else if (sel == 4 && (p & (SCE_CTRL_CROSS | SCE_CTRL_RIGHT))) {
            time = (time + 1) % 6;
        } else if (sel == 4 && (p & SCE_CTRL_LEFT)) {
            time = (time + 5) % 6;
        } else if ((p & SCE_CTRL_CROSS) && sel == 5) {
            return 0;
        } else if ((p & SCE_CTRL_CROSS) && sel < 4) {
            gVitaNetRole = (sel & 1) ? 2 : 1;
            gVitaNetLink = sel < 2 ? 1 : 2;
            gVitaNetRules[0] = time - 1;
            Vita_Log("vita: start menu: multiplayer, %s, %s\n", gVitaNetRole == 1 ? "host" : "join", gVitaNetLink == 1 ? "ad hoc" : "Wi-Fi");
            return 1;
        }
    }
}
#endif

void Vita_StartMenu(void) {
    enum { IT_GAME, IT_LANG, IT_UNLOCK, IT_NET, IT_START };
    int items[5], n = 0, sel = 0, i;
    unsigned p;
#ifndef BT3_BT4
    const char *why = NULL;
    int bt4;

    es_update();

    {   /* started again for a multiplayer match by the versus menu (vita_net.c): no menu, no BT4 */
        extern int Vita_NetStartFile(void);
        if (Vita_NetStartFile()) {
            return;
        }
    }
    bt4 = bt4_ready(&why);
    {   /* a test of the menus (bt3.txt startmode=, the test runner's): BT3's, whichever game the player chose (that
           choice, start.txt, left as it is) */
        extern int gVitaStartMode;
        if (gVitaStartMode != 0 && start_bt4_chosen()) {
            Vita_Log("vita: startmode=%d (a test): Budokai Tenkaichi 3, though BT4 is chosen\n", gVitaStartMode);
            bt4 = 0;
            goto chosen;
        }
    }
    if (start_bt4_chosen()) {
        if (bt4_was_starting()) { /* (the last start of BT4 did not get going) */
            sceIoRemove(BT4_STARTING);
            write_text(START_FILE, "bt3");
            Vita_Log("vita: BT4 did not get going the last time: Budokai Tenkaichi 3 instead\n");
            psvDebugScreenPrintf(ES("\n  \e[33mBudokai Tenkaichi 4 did not get going the last time\e[39m\n"
                                    "  (see ux0:data/bt3/bt4/log.txt and crash.txt).\n\n"
                                    "  Budokai Tenkaichi 3 starts instead. Hold SELECT while the\n"
                                    "  game starts to choose BT4 again.\n\n  Press CROSS.\n",
                                    "\n  \e[33mBudokai Tenkaichi 4 no llego a arrancar la ultima vez\e[39m\n"
                                    "  (mira ux0:data/bt3/bt4/log.txt y crash.txt).\n\n"
                                    "  En su lugar empieza Budokai Tenkaichi 3. Manten SELECT\n"
                                    "  al iniciar el juego para elegir BT4 otra vez.\n\n  Pulsa X.\n"));
            press(SCE_CTRL_CROSS);
            psvDebugScreenPrintf("\e[H\e[2J");
        } else if (!select_held()) {
            if (bt4) {
                start_bt4();
            } else {
                Vita_Log("vita: BT4 was chosen, but %s\n", why);
            }
        }
    }
chosen:
#endif
    if (VitaEu_NeedsChoice()) {
        VitaEu_ChooseLanguage();
    }
    es_update();
    if (!select_held()) {
        return;
    }
    Vita_Log("vita: start menu (SELECT held)\n");
#ifndef BT3_BT4
    if (exists(BT4_PROGRAM)) {
        items[n++] = IT_GAME;
    }
#else
    items[n++] = IT_GAME;
#endif
    if (VitaEu_Available()) {
        items[n++] = IT_LANG;
    }
    items[n++] = IT_UNLOCK;
#ifndef BT3_BT4
    items[n++] = IT_NET;
#endif
    items[n++] = IT_START;
    for (;;) {
        psvDebugScreenPrintf(ES("\e[H\e[2J\n  %s: start menu\n\n", "\e[H\e[2J\n  %s: menu de inicio\n\n"), VITA_GAME_NAME);
        for (i = 0; i < n; i++) {
            psvDebugScreenPrintf("  %s ", i == sel ? "\e[33m>" : " ");
            if (items[i] == IT_GAME) {
#ifndef BT3_BT4
                psvDebugScreenPrintf(ES("Game: Budokai Tenkaichi 3 (choose for BT4)", "Juego: Budokai Tenkaichi 3 (elige para BT4)"));
#else
                psvDebugScreenPrintf(ES("Game: Budokai Tenkaichi 4 (choose for BT3)", "Juego: Budokai Tenkaichi 4 (elige para BT3)"));
#endif
            } else if (items[i] == IT_LANG) {
                psvDebugScreenPrintf(ES("Language: %s", "Idioma: %s"), VitaEu_LangName(VitaEu_Lang()));
            } else if (items[i] == IT_UNLOCK) {
                psvDebugScreenPrintf(ES("Unlock everything (cheat): %s", "Desbloquear todo (truco): %s"), gVitaCheatUnlock ? ES("ON", "SI") : ES("OFF", "NO"));
            } else if (items[i] == IT_NET) {
                psvDebugScreenPrintf(ES("Multiplayer: play against another Vita", "Multijugador: juega contra otra Vita"));
            } else {
                psvDebugScreenPrintf(ES("Start the game", "Empezar el juego"));
            }
            psvDebugScreenPrintf("\e[39m\n");
        }
        if (items[sel] == IT_UNLOCK) {
            psvDebugScreenPrintf(ES("\n  Every character, stage, song and item, and 4,850,000\n"
                                    "  Zenni, given to the save the game loads now (its\n"
                                    "  records list is emptied). It stays once the game saves\n"
                                    "  (it cannot be taken back). Also in the game: Options >\n"
                                    "  Controller > Vita Settings > Cheats.\n",
                                    "\n  Todos los personajes, escenarios, canciones y objetos, y\n"
                                    "  4.850.000 Zenni, para la partida que el juego carga ahora\n"
                                    "  (su lista de records se vacia). Se queda cuando el juego\n"
                                    "  guarda (no se puede deshacer). En el juego tambien:\n"
                                    "  Opciones > Ajustes Vita > Desbloquear.\n"));
        } else if (items[sel] == IT_NET) {
            psvDebugScreenPrintf(ES("\n  A match of the versus mode against a friend's Vita:\n"
                                    "  ad hoc (the two Vitas near each other) or over the\n"
                                    "  same Wi-Fi network. One hosts, the other joins.\n",
                                    "\n  Una partida del modo versus contra la Vita de un amigo:\n"
                                    "  ad hoc (las dos Vitas cerca) o por la misma red Wi-Fi.\n"
                                    "  Uno la crea y el otro se une.\n"));
        } else if (items[sel] == IT_GAME) {
#ifndef BT3_BT4
            psvDebugScreenPrintf(ES("\n  Team BT4's Budokai Tenkaichi 4 B14 Rev2 (NauTaro's\n"
                                    "  BT4decomp), from your own BT4 disc's .iso or files in\n"
                                    "  ux0:data/bt3/bt4/. It then starts each time, until\n"
                                    "  changed here again.%s%s\n",
                                    "\n  Budokai Tenkaichi 4 B14 Rev2 de Team BT4 (BT4decomp de\n"
                                    "  NauTaro), del .iso o los archivos de tu propio disco de\n"
                                    "  BT4 en ux0:data/bt3/bt4/. Luego empieza siempre, hasta\n"
                                    "  que lo cambies aqui.%s%s\n"),
                                 why != NULL ? ES("\n\n  \e[33mNot now: ", "\n\n  \e[33mAhora no: ") : "", why != NULL ? why : "");
            if (why != NULL) {
                psvDebugScreenPrintf("\e[39m");
            }
#else
            psvDebugScreenPrintf(ES("\n  Back to Budokai Tenkaichi 3: it then starts each time,\n"
                                    "  until BT4 is chosen again.\n",
                                    "\n  Volver a Budokai Tenkaichi 3: luego empieza siempre,\n"
                                    "  hasta que se elija BT4 otra vez.\n"));
#endif
        }
        psvDebugScreenPrintf(ES("\n  Up / down, CROSS to choose; CIRCLE or START: start.\n",
                                "\n  Arriba / abajo, X para elegir; CIRCULO o START: empezar.\n"));
        p = press(SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_START);
        if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_START)) {
            break;
        }
        if (p & SCE_CTRL_UP) {
            sel = (sel + n - 1) % n;
        } else if (p & SCE_CTRL_DOWN) {
            sel = (sel + 1) % n;
        } else if (items[sel] == IT_GAME) {
#ifndef BT3_BT4
            if (bt4) {
                write_text(START_FILE, "bt4");
                release_buttons();
                start_bt4();
            }
#else
            int r;
            write_text(START_FILE, "bt3");
            Vita_Log("vita: back to Budokai Tenkaichi 3\n");
            release_buttons();
            r = sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
            Vita_Log("vita: BT3 could not be started (%08x)\n", (unsigned)r);
#endif
        } else if (items[sel] == IT_LANG) {
            VitaEu_ChooseLanguage();
            es_update();
        } else if (items[sel] == IT_UNLOCK) {
            gVitaCheatUnlock ^= 1;
#ifndef BT3_BT4
        } else if (items[sel] == IT_NET) {
            if (net_menu()) {
                break;
            }
#endif
        } else {
            break;
        }
    }
    Vita_Log("vita: start menu left (unlock everything %s)\n", gVitaCheatUnlock ? "on" : "off");
    psvDebugScreenPrintf("\e[H\e[2J");
    release_buttons();
}
