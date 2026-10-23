/*
 * European language packs for the Vita port (SLES-54945), matching the PC recomp:
 *   <root>/pzs3eu1/NNNNN.bin   menu / HUD / text archives per language
 *   <root>/disc/DATA/ZS3EUOP.ADX  optional European opening song
 *
 * The game executable remains the USA release. When a European language is chosen, reads of the
 * matching PZS3US1.AFS entries open the .bin files instead (same entry map as port/tools/extract_eu.py).
 */
#include "vita_eu.h"
#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

extern char gVitaDisc[];

#define EU_FIRST 451
#define EU_STRIDE 31
#define HUD_FIRST 5
#define LANGUAGES 5
/* Battle script subtitles (BtlScript LoadText, USA 0x231+story). EU packs are 50-wide banks at
 * 661/711/761/811/861 (en/es/de/fr/it); USA story index is 4 ahead of the EU bank index. */
#define BTL_TEXT_USA0 561
#define BTL_TEXT_COUNT 50
#define BTL_TEXT_SHIFT 4
#define BTL_TEXT_EU_STORIES 45

/* Menu/HUD text packs, plus story-mode saga packs (baseFile+0x10+subMenu → AFS 464..471). */
static const int kUsaFiles[] = {448, 449, 450, 451, 452, 453, 459, 460, 461, 463, 464, 465, 466, 467, 468, 469, 470, 471, 472};
static const char *kLocales[] = {"en", "es", "de", "fr", "it"};
static const char *kNames[] = {"English", "Espanol", "Deutsch", "Francais", "Italiano"};
/* en keeps USA archives; es..it use European banks 1..4 (English bank 0 is unused, as on PC). */
static const int kBanks[] = {-1, 1, 2, 3, 4};
static const int kBtlTextEuBase[] = {661, 711, 761, 811, 861};

static char sRoot[256];
static char sLocale[8] = "en";
static int sAvailable;
static int sUs1Ptid = -1;
static int sHasChoice;

static int file_ok(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    sceIoClose(fd);
    return 1;
}

static void upper_copy(char *dst, size_t n, const char *src) {
    size_t i;
    for (i = 0; src[i] != '\0' && i + 1 < n; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static int locale_index(const char *locale) {
    int i;
    if (locale == NULL || locale[0] == '\0') {
        return -1;
    }
    for (i = 0; i < (int)(sizeof(kLocales) / sizeof(kLocales[0])); i++) {
        if (strcmp(locale, kLocales[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static int eu_index_for_usa(int usa, int bank) {
    int k;
    if (bank < 0 || bank >= LANGUAGES) {
        return -1;
    }
    if (usa == HUD_FIRST) {
        return HUD_FIRST + bank;
    }
    for (k = 0; k < (int)(sizeof(kUsaFiles) / sizeof(kUsaFiles[0])); k++) {
        if (kUsaFiles[k] == usa) {
            return EU_FIRST + (usa - kUsaFiles[0]) + bank * EU_STRIDE;
        }
    }
    return -1;
}

/* European battle-dialogue text for locale index (es..it). English keeps the USA file. */
static int eu_btl_text_for_usa(int usa, int localeIndex) {
    int story, euStory;

    if (localeIndex <= 0 || localeIndex >= (int)(sizeof(kLocales) / sizeof(kLocales[0]))) {
        return -1;
    }
    if (usa < BTL_TEXT_USA0 || usa >= BTL_TEXT_USA0 + BTL_TEXT_COUNT) {
        return -1;
    }
    story = usa - BTL_TEXT_USA0;
    if (story < BTL_TEXT_SHIFT) {
        return -1; /* first four USA stories have no matching EU language pack slot */
    }
    euStory = story - BTL_TEXT_SHIFT;
    if (euStory < 0 || euStory >= BTL_TEXT_EU_STORIES) {
        return -1;
    }
    return kBtlTextEuBase[localeIndex] + euStory;
}

void VitaEu_Init(const char *root) {
    char path[300], line[32];
    FILE *fp;
    int i;

    sRoot[0] = '\0';
    sAvailable = 0;
    sHasChoice = 0;
    sUs1Ptid = -1;
    snprintf(sLocale, sizeof(sLocale), "en");
    if (root == NULL || root[0] == '\0') {
        return;
    }
    snprintf(sRoot, sizeof(sRoot), "%s", root);

    /* Presence of the Spanish HUD entry means extract_eu (or the PC setup) was run. */
    snprintf(path, sizeof(path), "%s/pzs3eu1/00006.bin", sRoot);
    sAvailable = file_ok(path);

    snprintf(path, sizeof(path), "%s/lang.txt", sRoot);
    fp = fopen(path, "r");
    if (fp != NULL) {
        if (fgets(line, sizeof(line), fp) != NULL) {
            char *p = line;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            for (i = 0; p[i] != '\0'; i++) {
                if (p[i] == '\n' || p[i] == '\r' || p[i] == ' ' || p[i] == '"' || p[i] == '\'') {
                    p[i] = '\0';
                    break;
                }
                if (p[i] >= 'A' && p[i] <= 'Z') {
                    p[i] = (char)(p[i] - 'A' + 'a');
                }
            }
            if (locale_index(p) >= 0) {
                snprintf(sLocale, sizeof(sLocale), "%s", p);
                sHasChoice = 1;
            }
        }
        fclose(fp);
    }
}

int VitaEu_Available(void) {
    return sAvailable;
}

int VitaEu_UsesMenuRemap(void) {
    int li = locale_index(sLocale);

    return sAvailable && li >= 0 && kBanks[li] >= 0;
}

const char *VitaEu_Locale(void) {
    return sLocale;
}

void VitaEu_SetLocale(const char *locale) {
    char path[300];
    FILE *fp;
    int i = locale_index(locale);

    if (i < 0) {
        return;
    }
    snprintf(sLocale, sizeof(sLocale), "%s", kLocales[i]);
    sHasChoice = 1;
    if (sRoot[0] == '\0') {
        return;
    }
    snprintf(path, sizeof(path), "%s/lang.txt", sRoot);
    fp = fopen(path, "w");
    if (fp != NULL) {
        fprintf(fp, "%s\n", sLocale);
        fclose(fp);
    }
}

int VitaEu_NeedsPrompt(void) {
    return sAvailable && !sHasChoice;
}

int VitaEu_LocaleCount(void) {
    return (int)(sizeof(kLocales) / sizeof(kLocales[0]));
}

const char *VitaEu_LocaleAt(int index) {
    if (index < 0 || index >= VitaEu_LocaleCount()) {
        return "en";
    }
    return kLocales[index];
}

const char *VitaEu_LocaleName(int index) {
    if (index < 0 || index >= VitaEu_LocaleCount()) {
        return kNames[0];
    }
    return kNames[index];
}

void VitaEu_NotePartition(int ptid, const char *fname) {
    char up[64];
    if (fname == NULL) {
        return;
    }
    upper_copy(up, sizeof(up), fname);
    if (strstr(up, "PZS3US1") != NULL) {
        sUs1Ptid = ptid & 7;
    }
}

void VitaEu_PatchTocSizes(int ptid, uint32_t *table, uint32_t count) {
    int bank, li, k, idx;
    char path[320];
    SceUID fd;
    SceOff size;
    extern void Vita_Log(const char *fmt, ...);

    if (!sAvailable || table == NULL || sUs1Ptid < 0 || (ptid & 7) != sUs1Ptid) {
        return;
    }
    li = locale_index(sLocale);
    if (li < 0) {
        return;
    }
    bank = kBanks[li];
    if (bank < 0) {
        return;
    }
    /* HUD + each menu archive the European disc duplicates per language. */
    idx = eu_index_for_usa(HUD_FIRST, bank);
    if (idx >= 0 && (uint32_t)HUD_FIRST < count) {
        snprintf(path, sizeof(path), "%s/pzs3eu1/%05d.bin", sRoot, idx);
        fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd >= 0) {
            size = sceIoLseek(fd, 0, SCE_SEEK_END);
            sceIoClose(fd);
            if (size > 0) {
                Vita_Log("eu: toc %d size %u -> %u (%s)\n", HUD_FIRST, table[HUD_FIRST * 2 + 1], (unsigned)size, path);
                table[HUD_FIRST * 2 + 1] = (uint32_t)size;
            }
        }
    }
    for (k = 0; k < (int)(sizeof(kUsaFiles) / sizeof(kUsaFiles[0])); k++) {
        int usa = kUsaFiles[k];
        idx = eu_index_for_usa(usa, bank);
        if (idx < 0 || (uint32_t)usa >= count) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/pzs3eu1/%05d.bin", sRoot, idx);
        fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd < 0) {
            continue;
        }
        size = sceIoLseek(fd, 0, SCE_SEEK_END);
        sceIoClose(fd);
        if (size > 0) {
            Vita_Log("eu: toc %d size %u -> %u (%s)\n", usa, table[usa * 2 + 1], (unsigned)size, path);
            table[usa * 2 + 1] = (uint32_t)size;
        }
    }
    /* Battle script subtitle packs (USA 561+story → EU language bank). */
    for (k = BTL_TEXT_SHIFT; k < BTL_TEXT_SHIFT + BTL_TEXT_EU_STORIES; k++) {
        int usa = BTL_TEXT_USA0 + k;
        idx = eu_btl_text_for_usa(usa, li);
        if (idx < 0 || (uint32_t)usa >= count) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/pzs3eu1/%05d.bin", sRoot, idx);
        fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd < 0) {
            continue;
        }
        size = sceIoLseek(fd, 0, SCE_SEEK_END);
        sceIoClose(fd);
        if (size > 0) {
            table[usa * 2 + 1] = (uint32_t)size;
        }
    }
}

int VitaEu_OverridePath(int ptid, int flid, char *out, int size) {
    int idx, bank, li;

    if (!sAvailable || out == NULL || size <= 0 || sUs1Ptid < 0 || (ptid & 7) != sUs1Ptid) {
        return 0;
    }
    li = locale_index(sLocale);
    if (li < 0) {
        return 0;
    }
    bank = kBanks[li];
    if (bank >= 0) {
        idx = eu_index_for_usa(flid, bank);
        if (idx >= 0) {
            snprintf(out, (size_t)size, "%s/pzs3eu1/%05d.bin", sRoot, idx);
            if (file_ok(out)) {
                return 1;
            }
        }
    }
    /* Battle dialogue text: es/de/fr/it banks (English stays on the USA disc). */
    idx = eu_btl_text_for_usa(flid, li);
    if (idx < 0) {
        return 0;
    }
    snprintf(out, (size_t)size, "%s/pzs3eu1/%05d.bin", sRoot, idx);
    return file_ok(out);
}

int VitaEu_OpeningAdxPath(char *out, int size) {
    if (!sAvailable || out == NULL || size <= 0) {
        return 0;
    }
    /* Same as PC: once the European disc was added, the opening uses its soundtrack. */
    snprintf(out, (size_t)size, "%s/DATA/ZS3EUOP.ADX", gVitaDisc);
    return file_ok(out);
}
