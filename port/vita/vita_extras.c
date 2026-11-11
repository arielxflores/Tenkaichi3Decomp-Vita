/*
 * PS Vita: stages and songs added from outside the disc (z3xox's PC version: port/src/plat_extras.c, plat_stages.c,
 * plat_songs.c and docs/port/adding_stages.md, adding_songs.md there; this is their Vita form).
 *
 *   ux0:data/bt3/stages/   a stage: a .unk file, the stage model BT3 modders release (the file a mod puts in a disc
 *                          stage's place under PCSX2). Each one becomes a stage of its own after the disc's 36, in
 *                          the versus menus' stage grid (one-on-one, team and DP battles, training).
 *   ux0:data/bt3/songs/    a song: an ADX file (the game's own kind) or an MP3, which is made into an ADX beside
 *                          it the first time the game starts with it (vita_mp3.c). Each one is listed in the music
 *                          select after the disc's songs and before "Random".
 *
 * A file dropped in is used; its name in the menus is the file's name without its extension, an underscore read as
 * a blank ("My_Stage.unk" is "My Stage"). maps.txt in the stages folder and songs.txt in the songs folder (lines
 * "file|Name In The Menu") give other names and an order; the files they do not name follow, by name without
 * regard to case. The names are drawn in the game's own lettering (vita_names.c) where the disc's names have every
 * letter, else in the game's plain font.
 *
 * Nothing about them is written to the save (they are added to the menus' lists after the save's unlocks were
 * applied to the disc's): a save stays good for the game without them. With neither folder, nothing changes.
 *
 * The game asks for their files by ids of their own past the end of the second archive (65,201 files): a stage's
 * model is member 70000 + n, a song member 73000 + n (src/battle/battle_load.c PORT_ADDED_STAGE_FILE,
 * port/include/plat_songs.h PORT_BGM_FILE). Vita_ExtraPath answers those for the file layer (vita_file.c).
 */
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <ctype.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat_songs.h"
#include "plat_stages.h"

extern void Vita_Log(const char *fmt, ...);

#define STAGES_DIR "ux0:data/bt3/stages"
#define SONGS_DIR "ux0:data/bt3/songs"
#define STAGE_FILE_INDEX 70000 /* (battle_load.c: PORT_ADDED_STAGE_FILE) */
#define NAME_LEN 128

/* The game's own stage buffer (BTL_STAGE_BUF_SIZE): every stage of the disc fits it. A larger added stage gets a
   buffer of its size from the port's memory (Port_GameBigAlloc), up to this. */
#define STAGE_BUF_GAME 0x6CB800
#define STAGE_BUF_MAX (24u << 20)

char gPortStageNames[PORT_STAGE_MAX][64];
int gPortExtraStageCount;
int gPortExtraStages[PORT_STAGE_MAX];
/* (the PC's test switch BT3_STAGE_REPLACE, which the menus' code reads: nothing on the Vita) */
int gPortStageReplaceCount, gPortReplaceOld[16], gPortReplaceNew[16];

char gPortSongNames[PORT_SONG_MAX][64];
int gPortSongCount;
int gPortSongOffsets[PORT_SONG_MAX];

static char sStageFile[PORT_STAGE_MAX][NAME_LEN];
static int8_t sStageBase[PORT_STAGE_MAX]; /* the disc stage each was made from, -1 not known */
static char sSongFile[PORT_SONG_MAX][NAME_LEN];
static uint32_t sLargest; /* the largest added stage file */

/* ---- the folders ------------------------------------------------------------------------------------------- */

static int name_cmp(const void *a, const void *b) {
    const unsigned char *x = a, *y = b;
    for (; *x != '\0' && tolower(*x) == tolower(*y); x++, y++) {
    }
    return tolower(*x) != tolower(*y) ? tolower(*x) - tolower(*y) : strcmp(a, b);
}

static int ends_with(const char *name, const char *ext) {
    size_t l = strlen(name), el = strlen(ext), k;
    if (l <= el) {
        return 0;
    }
    for (k = 0; k < el && tolower((unsigned char)name[l - el + k]) == ext[k]; k++) {
    }
    return k == el;
}

/* The files of `dir` whose name ends in one of `exts` (lower case), sorted by name without regard to case. */
static int scan(const char *dir, const char *const *exts, int nExt, char names[][NAME_LEN], int max) {
    SceUID d = sceIoDopen(dir);
    SceIoDirent e;
    int n = 0, k;

    if (d < 0) {
        return 0;
    }
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0 && n < max) {
        size_t l = strlen(e.d_name);
        if (!SCE_S_ISREG(e.d_stat.st_mode) || l >= NAME_LEN) {
            continue;
        }
        for (k = 0; k < nExt && !ends_with(e.d_name, exts[k]); k++) {
        }
        if (k < nExt) {
            snprintf(names[n++], NAME_LEN, "%s", e.d_name);
        }
    }
    sceIoDclose(d);
    qsort(names, (size_t)n, NAME_LEN, name_cmp);
    return n;
}

/* "my_cool-map.unk" -> "my cool-map" */
static void name_from_file(const char *file, char *name, unsigned size) {
    const char *dot = strrchr(file, '.');
    unsigned i, n = (unsigned)(dot != NULL ? dot - file : (long)strlen(file));

    for (i = 0; i < n && i + 1 < size; i++) {
        name[i] = file[i] == '_' ? ' ' : file[i];
    }
    name[i] = '\0';
}

static char *trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        *--e = '\0';
    }
    return s;
}

static uint32_t file_size(const char *dir, const char *file) {
    char path[300];
    SceIoStat st;
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (sceIoGetstat(path, &st) < 0 || !SCE_S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size >= 0x40000000) {
        return 0;
    }
    return (uint32_t)st.st_size;
}

/* The stage number in a stage model's parameter block (the file's third word: the block's place; the block's word
   at 0x14: its environment record's place; the record's first word: the number, src/battle/stg_a_b.c
   BtlStage_BindFile). A model made from a disc stage keeps that stage's number. -1 when it is not one of them. */
static int stage_base(const char *file, uint32_t size) {
    char path[300];
    uint32_t w[3], env[2];
    int32_t id = -1;
    SceUID fd;
    snprintf(path, sizeof(path), "%s/%s", STAGES_DIR, file);
    if ((fd = sceIoOpen(path, SCE_O_RDONLY, 0)) < 0) {
        return -1;
    }
    if (sceIoPread(fd, w, 12, 0) == 12 && (w[2] & ~3u) + 0x18 <= size &&
        sceIoPread(fd, env, 8, (SceOff)((w[2] & ~3u) + 0x10)) == 8 && env[0] >= 1 && (w[2] & ~3u) + env[1] + 4 <= size) {
        if (sceIoPread(fd, &id, 4, (SceOff)((w[2] & ~3u) + env[1])) != 4) {
            id = -1;
        }
    }
    sceIoClose(fd);
    return id >= 0 && id < PORT_STAGE_FIRST ? (int)id : -1;
}

/* The list file's lines "file|Name", each through add(file, name). */
static void read_list(const char *dir, const char *list, void (*add)(const char *file, const char *name)) {
    char path[300], line[256];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s", dir, list);
    if ((fp = fopen(path, "rb")) == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *bar = strchr(line, '|');
        if (line[0] == '#' || bar == NULL) {
            continue;
        }
        *bar = '\0';
        add(trim(line), trim(bar + 1));
    }
    fclose(fp);
}

/* ---- stages ------------------------------------------------------------------------------------------------ */

static void add_stage(const char *file, const char *name) {
    int i;
    uint32_t size;

    for (i = 0; i < gPortExtraStageCount; i++) {
        if (strcasecmp(sStageFile[i], file) == 0) {
            return; /* (in the list and found in the folder: once) */
        }
    }
    if (gPortExtraStageCount >= PORT_STAGE_MAX) {
        Vita_Log("stages: more than %d added, %s is left out\n", PORT_STAGE_MAX, file);
        return;
    }
    if (strlen(file) >= NAME_LEN || (size = file_size(STAGES_DIR, file)) == 0) {
        Vita_Log("stages: %s is left out: no such file\n", file);
        return;
    }
    if (size > STAGE_BUF_MAX) {
        Vita_Log("stages: %s is left out: %u bytes, more than the %u a stage can have here\n", file, (unsigned)size, STAGE_BUF_MAX);
        return;
    }
    if (size > sLargest) {
        sLargest = size;
    }
    i = gPortExtraStageCount++;
    snprintf(sStageFile[i], NAME_LEN, "%s", file);
    snprintf(gPortStageNames[i], sizeof(gPortStageNames[i]), "%s", name);
    gPortExtraStages[i] = PORT_STAGE_FIRST + i;
    sStageBase[i] = (int8_t)stage_base(file, size);
    Vita_Log("stages: %d \"%s\": %s, %u KB, made from disc stage %d\n", PORT_STAGE_FIRST + i, gPortStageNames[i], file, (unsigned)(size >> 10),
             sStageBase[i]);
}

void PortStages_Init(void) {
    static int done;
    static char found[PORT_STAGE_MAX][NAME_LEN];
    static const char *const kExt[] = {".unk"};
    int n, k;

    if (done) {
        return;
    }
    done = 1;
    read_list(STAGES_DIR, "maps.txt", add_stage);
    n = scan(STAGES_DIR, kExt, 1, found, PORT_STAGE_MAX);
    for (k = 0; k < n; k++) {
        char name[64];
        name_from_file(found[k], name, sizeof(name));
        add_stage(found[k], name);
    }
}

int PortStages_Count(void) {
    return gPortExtraStageCount;
}

const char *PortStages_Name(int index) {
    return index >= 0 && index < gPortExtraStageCount ? gPortStageNames[index] : "";
}

int Port_AddedStageBase(int stage) {
    int i = stage - PORT_STAGE_FIRST;
    return i >= 0 && i < gPortExtraStageCount && sStageBase[i] >= 0 ? sStageBase[i] : 0;
}

int Port_AddedStageLook(int stage) {
    int i = stage - PORT_STAGE_FIRST;
    return i >= 0 && i < gPortExtraStageCount && sStageBase[i] >= 0 ? sStageBase[i] : 0x23;
}

/* The buffer a battle loads its stages into: the game's own size unless an added stage is larger. */
int Port_StageBufSize(void) {
    uint32_t size = sLargest > STAGE_BUF_GAME ? sLargest : STAGE_BUF_GAME;
    return (int)((size + 0x7FFu) & ~0x7FFu); /* whole sectors, as the reads are */
}

/* A buffer larger than the game's own: not from its heap (28 MB, which a battle's fighters need too) but from the
   port's memory. Heap_Free gives it back here (src/sys/heap.c). */
static void *sBig[4];
unsigned Port_GameBigAlloc(int size) {
    int k;
    for (k = 0; k < 4; k++) {
        if (sBig[k] == NULL) {
            void *p = memalign(64, (size_t)size);
            if (p == NULL) {
                Vita_Log("stages: no memory for a stage buffer of %d KB\n", size >> 10);
                return 0;
            }
            sBig[k] = p;
            return (unsigned)(uintptr_t)p;
        }
    }
    return 0;
}

int Port_GameBigFree(unsigned addr) {
    int k;
    if (addr == 0) {
        return 0;
    }
    for (k = 0; k < 4; k++) {
        if (sBig[k] != NULL && (unsigned)(uintptr_t)sBig[k] == addr) {
            free(sBig[k]);
            sBig[k] = NULL;
            return 1;
        }
    }
    return 0;
}

/* (the PC's test switches BT3_TEST_STAGE, BT3_TEST_SCREEN, BT3_TEST_BGM: the Vita's tests choose through bt3.txt) */
int Port_TestStage(void) { return -1; }
int Port_TestScreen(void) { return -1; }
int Port_TestBgm(void) { return -1; }

/* ---- songs ------------------------------------------------------------------------------------------------- */

static void add_song(const char *file, const char *name) {
    int i;

    for (i = 0; i < gPortSongCount; i++) {
        if (strcasecmp(sSongFile[i], file) == 0) {
            return;
        }
    }
    if (gPortSongCount >= PORT_SONG_MAX) {
        Vita_Log("songs: more than %d added, %s is left out\n", PORT_SONG_MAX, file);
        return;
    }
    if (strlen(file) >= NAME_LEN || !ends_with(file, ".adx") || file_size(SONGS_DIR, file) < 0x40) {
        Vita_Log("songs: %s is left out: not an ADX file in the folder\n", file);
        return;
    }
    i = gPortSongCount++;
    snprintf(sSongFile[i], NAME_LEN, "%s", file);
    snprintf(gPortSongNames[i], sizeof(gPortSongNames[i]), "%s", name);
    gPortSongOffsets[i] = PORT_SONG_FIRST_OFFSET + i;
    Vita_Log("songs: %d \"%s\": %s\n", i, gPortSongNames[i], file);
}

/* songs.txt may name an MP3: its ADX is the song (vita_mp3.c makes it) */
static void add_listed_song(const char *file, const char *name) {
    char adx[NAME_LEN];
    size_t l = strlen(file);
    if (ends_with(file, ".mp3") && l < NAME_LEN) {
        snprintf(adx, sizeof(adx), "%.*s.adx", (int)(l - 4), file);
        file = adx;
    }
    add_song(file, name);
}

/* Reads the folder and adds the songs not in the list yet, at its end (a song keeps its place: the game holds the
   place's number while a fight with that music is set up). */
static void songs_collect(void) {
    static char found[PORT_SONG_MAX * 2][NAME_LEN];
    static const char *const kExt[] = {".adx"};
    int n, k;
    read_list(SONGS_DIR, "songs.txt", add_listed_song);
    n = scan(SONGS_DIR, kExt, 1, found, PORT_SONG_MAX * 2);
    for (k = 0; k < n; k++) {
        char name[64];
        name_from_file(found[k], name, sizeof(name));
        add_song(found[k], name);
    }
}

void PortSongs_Init(void) {
    static int done;
    extern void Vita_Mp3Start(const char *dir);
    if (done) {
        return;
    }
    done = 1;
    songs_collect();
    Vita_Mp3Start(SONGS_DIR); /* (MP3 files without their ADX yet: made in the background, vita_mp3.c) */
}

/* The select screens call this as they set up their music list: songs made from an MP3 since join it. */
void PortSongs_Refresh(void) {
    extern int Vita_Mp3Made(void);
    static int seen;
    int made = Vita_Mp3Made();
    if (made != seen) {
        seen = made;
        songs_collect();
    }
}

int PortSongs_Count(void) {
    return gPortSongCount;
}

int PortSongs_Offset(int index) {
    return index >= 0 && index < gPortSongCount ? gPortSongOffsets[index] : 0;
}

const char *PortSongs_Name(int index) {
    return index >= 0 && index < gPortSongCount ? gPortSongNames[index] : "";
}

/* Whether `path` is an added song's file: played round and round like the disc's music, though it has no loop
   points of its own (port/src/gs/snd_adx.c). */
int PortSongs_IsFile(const char *path) {
    size_t dl = strlen(SONGS_DIR);
    int i;
    if (strncmp(path, SONGS_DIR "/", dl + 1) != 0) {
        return 0;
    }
    for (i = 0; i < gPortSongCount; i++) {
        if (strcmp(path + dl + 1, sSongFile[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---- the file layer ---------------------------------------------------------------------------------------- */

/* The file of member `flid` of partition `ptid` when it is an added stage's or song's (into out). 0: not one. */
int Vita_ExtraPath(int ptid, int flid, char *out, int size) {
    if ((ptid & 7) != 2) {
        return 0;
    }
    if (flid >= STAGE_FILE_INDEX && flid < STAGE_FILE_INDEX + gPortExtraStageCount) {
        snprintf(out, (size_t)size, STAGES_DIR "/%s", sStageFile[flid - STAGE_FILE_INDEX]);
        return 1;
    }
    if (flid >= PORT_SONG_FILE_INDEX && flid < PORT_SONG_FILE_INDEX + gPortSongCount) {
        snprintf(out, (size_t)size, SONGS_DIR "/%s", sSongFile[flid - PORT_SONG_FILE_INDEX]);
        return 1;
    }
    return 0;
}

/* At start (vita_main.c), after the settings: both folders read. */
void Vita_ExtrasInit(void) {
    PortStages_Init();
    PortSongs_Init();
    if (gPortExtraStageCount > 0 || gPortSongCount > 0) {
        Vita_Log("added from outside the disc: %d stages (%s), %d songs (%s)\n", gPortExtraStageCount, STAGES_DIR, gPortSongCount, SONGS_DIR);
    }
}
