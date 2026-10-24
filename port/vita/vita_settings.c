/*
 * Hold SELECT (~1 s, without L/R) for an Adrenaline-style pause menu.
 *
 * Drawn with vitaGL (same swap path that made MsgDialog visible): a dark sheet and a
 * green list over the game. psvDebugScreen is not used mid-game — it freezes the display.
 *
 * Layout is designed for 960x544 and scaled to the current framebuffer (fights are often
 * 640x368), so the overlay stays on-screen in battle.
 */
#include "vita_settings.h"
#include "vita_eu.h"
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <vitaGL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define HOME "ux0:data/bt3"
#define HOLD_US (1000 * 1000)
#define DESIGN_W 960.0f
#define DESIGN_H 544.0f

enum {
    ROW_LANG = 0,
    ROW_WIDE,
    ROW_HUD,
    ROW_MENU43,
    ROW_UNLOCK, /* early: visible without scrolling past FX toggles */
    ROW_SKIPLOGO,
    ROW_SKIPINTRO,
    ROW_OUTLINE,
    ROW_GLOW,
    ROW_RESUME,
    ROW_COUNT
};

#define VISIBLE_ROWS 8

extern int gVitaWide;
extern int gVitaHudStyle;
extern int gVitaMenu43;
extern int gVitaSkipLogos;
extern int gVitaSkipIntro;
extern int gVitaFxMask;
extern void Vita_Log(const char *fmt, ...);
extern void Vita_RefreshView(void);
extern void Vita_GlSync(void);
extern void Vita_GetScreenSize(int *w, int *h);
extern void Port_SetAspectMilli(int milli);
extern void vglSwapBuffers(GLboolean has_commondialog);

/* Game save unlock (sys/save.c). gSaveData is NULL before Save_Init. */
typedef struct SaveData SaveData;
extern SaveData *gSaveData;
extern void Save_UnlockAll(SaveData *opt);

static int sOpen;
static int sPending; /* hold reached: open at next FrameEnd, not mid-pad-read */
static unsigned long long sSelectDownUs;
static int sSelectWasDown;
static GLuint sFontTex;
static int sFontReady;
static float sScrW = DESIGN_W, sScrH = DESIGN_H;
static float sSx = 1.0f, sSy = 1.0f;

int Vita_SettingsOpen(void) {
    return sOpen || sPending;
}

/* ---- tiny 8x8 ASCII font (32..127), 1 bpp packed rows ---- */
static const unsigned char kFont8[96][8] = {
    {0,0,0,0,0,0,0,0}, {0x18,0x3C,0x3C,0x18,0x18,0,0x18,0}, {0x36,0x36,0x24,0,0,0,0,0},
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0}, {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0}, {0,0x63,0x33,0x18,0x0C,0x66,0x63,0},
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0}, {0x0C,0x0C,0x06,0,0,0,0,0}, {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0},
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0}, {0,0x66,0x3C,0xFF,0x3C,0x66,0,0}, {0,0x0C,0x0C,0x3F,0x0C,0x0C,0,0},
    {0,0,0,0,0,0x0C,0x0C,0x06}, {0,0,0,0x3F,0,0,0,0}, {0,0,0,0,0,0x0C,0x0C,0}, {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0},
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0}, {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0}, {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0},
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0}, {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0}, {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0},
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0}, {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0}, {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0},
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0}, {0,0x0C,0x0C,0,0,0x0C,0x0C,0}, {0,0x0C,0x0C,0,0,0x0C,0x0C,0x06},
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0}, {0,0,0x3F,0,0x3F,0,0,0}, {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0},
    {0x1E,0x33,0x30,0x18,0x0C,0,0x0C,0}, {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0}, {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0},
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0}, {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0}, {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0},
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0}, {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0}, {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0},
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0}, {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0}, {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0},
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0}, {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0}, {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0},
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0}, {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0}, {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0},
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0}, {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0}, {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0},
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0}, {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0}, {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0},
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0}, {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0}, {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0},
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0}, {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0}, {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0},
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0}, {0x08,0x1C,0x36,0x63,0,0,0,0}, {0,0,0,0,0,0,0,0xFF},
    {0x0C,0x0C,0x18,0,0,0,0,0}, {0,0,0x1E,0x30,0x3E,0x33,0x6E,0}, {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0},
    {0,0,0x1E,0x33,0x03,0x33,0x1E,0}, {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0}, {0,0,0x1E,0x33,0x3f,0x03,0x1E,0},
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0}, {0,0,0x6E,0x33,0x33,0x3E,0x30,0x1F}, {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0},
    {0x0C,0,0x0E,0x0C,0x0C,0x0C,0x1E,0}, {0x30,0,0x30,0x30,0x30,0x33,0x33,0x1E}, {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0},
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0}, {0,0,0x33,0x7F,0x7F,0x6B,0x63,0}, {0,0,0x1F,0x33,0x33,0x33,0x33,0},
    {0,0,0x1E,0x33,0x33,0x33,0x1E,0}, {0,0,0x3B,0x66,0x66,0x3E,0x06,0x0F}, {0,0,0x6E,0x33,0x33,0x3E,0x30,0x78},
    {0,0,0x3B,0x6E,0x66,0x06,0x0F,0}, {0,0,0x3E,0x03,0x1E,0x30,0x1F,0}, {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0},
    {0,0,0x33,0x33,0x33,0x33,0x6E,0}, {0,0,0x33,0x33,0x33,0x1E,0x0C,0}, {0,0,0x63,0x6B,0x7F,0x7F,0x36,0},
    {0,0,0x63,0x36,0x1C,0x36,0x63,0}, {0,0,0x33,0x33,0x33,0x3E,0x30,0x1F}, {0,0,0x3F,0x19,0x0C,0x26,0x3F,0},
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0}, {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0}, {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0},
    {0x6E,0x3B,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0},
};

static void font_ensure(void) {
    unsigned char rgba[96 * 8 * 8 * 4];
    int c, y, x, i = 0;

    if (sFontReady) {
        return;
    }
    for (c = 0; c < 96; c++) {
        for (y = 0; y < 8; y++) {
            unsigned char row = kFont8[c][y];
            for (x = 0; x < 8; x++) {
                int on = (row >> x) & 1;
                rgba[i++] = on ? 255 : 0;
                rgba[i++] = on ? 255 : 0;
                rgba[i++] = on ? 255 : 0;
                rgba[i++] = on ? 255 : 0;
            }
        }
    }
    glGenTextures(1, &sFontTex);
    glBindTexture(GL_TEXTURE_2D, sFontTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 96 * 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    sFontReady = 1;
}

static void bt3_set(const char *name, const char *value) {
    char path[300], tmp[300], line[256], out[8192];
    FILE *fp;
    size_t n = 0;
    int found = 0;
    size_t nameLen = strlen(name);

    snprintf(path, sizeof(path), "%s/bt3.txt", HOME);
    snprintf(tmp, sizeof(tmp), "%s/bt3.txt.tmp", HOME);
    out[0] = '\0';
    fp = fopen(path, "r");
    if (fp != NULL) {
        while (fgets(line, sizeof(line), fp) != NULL) {
            char *eq = strchr(line, '=');
            if (eq != NULL && (size_t)(eq - line) == nameLen && strncmp(line, name, nameLen) == 0) {
                n += (size_t)snprintf(out + n, sizeof(out) - n, "%s=%s\n", name, value);
                found = 1;
            } else {
                n += (size_t)snprintf(out + n, sizeof(out) - n, "%s", line);
            }
            if (n + 64 >= sizeof(out)) {
                break;
            }
        }
        fclose(fp);
    }
    if (!found) {
        n += (size_t)snprintf(out + n, sizeof(out) - n, "%s=%s\n", name, value);
    }
    fp = fopen(tmp, "w");
    if (fp == NULL) {
        return;
    }
    fputs(out, fp);
    fclose(fp);
    remove(path);
    rename(tmp, path);
}

static void bt3_set_int(const char *name, int value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    bt3_set(name, buf);
}

static void apply_wide(int wide) {
    gVitaWide = wide != 0;
    Port_SetAspectMilli(gVitaWide ? 1765 : 1333);
    Vita_RefreshView();
    bt3_set_int("wide", gVitaWide);
}

static void apply_fx_bit(int bit, int enabled) {
    if (enabled) {
        gVitaFxMask &= ~bit;
    } else {
        gVitaFxMask |= bit;
    }
    bt3_set_int("fxmask", gVitaFxMask);
}

static void sync_screen(void) {
    int w = (int)DESIGN_W, h = (int)DESIGN_H;

    Vita_GetScreenSize(&w, &h);
    if (w < 160) {
        w = (int)DESIGN_W;
    }
    if (h < 120) {
        h = (int)DESIGN_H;
    }
    sScrW = (float)w;
    sScrH = (float)h;
    sSx = sScrW / DESIGN_W;
    sSy = sScrH / DESIGN_H;
}

/* Design pixels → NDC using the live framebuffer size. */
static void px_to_ndc(float x, float y, float *ox, float *oy) {
    *ox = (x * sSx / sScrW) * 2.0f - 1.0f;
    *oy = 1.0f - (y * sSy / sScrH) * 2.0f;
}

static void draw_quad(float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    float v[8], ax, ay, bx, by, cx, cy, dx, dy;

    px_to_ndc(x0, y0, &ax, &ay);
    px_to_ndc(x1, y0, &bx, &by);
    px_to_ndc(x0, y1, &cx, &cy);
    px_to_ndc(x1, y1, &dx, &dy);
    v[0] = ax; v[1] = ay;
    v[2] = bx; v[3] = by;
    v[4] = cx; v[5] = cy;
    v[6] = dx; v[7] = dy;

    glDisable(GL_TEXTURE_2D);
    glColor4f(r, g, b, a);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, v);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void draw_text(float x, float y, float scale, float r, float g, float b, const char *s) {
    float cw = 8.0f * scale, ch = 8.0f * scale;
    float atlasH = 96.0f * 8.0f;

    font_ensure();
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, sFontTex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(r, g, b, 1.0f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    while (*s) {
        int chn = (unsigned char)*s++;
        float v[8], t[8], ax, ay, bx, by, cx, cy, dx, dy, u0, u1, v0, v1;
        int row;

        if (chn < 32 || chn > 127) {
            chn = '?';
        }
        row = chn - 32;
        u0 = 0.0f;
        u1 = 1.0f;
        v0 = (float)(row * 8) / atlasH;
        v1 = (float)(row * 8 + 8) / atlasH;
        px_to_ndc(x, y, &ax, &ay);
        px_to_ndc(x + cw, y, &bx, &by);
        px_to_ndc(x, y + ch, &cx, &cy);
        px_to_ndc(x + cw, y + ch, &dx, &dy);
        v[0] = ax; v[1] = ay;
        v[2] = bx; v[3] = by;
        v[4] = cx; v[5] = cy;
        v[6] = dx; v[7] = dy;
        t[0] = u0; t[1] = v0;
        t[2] = u1; t[3] = v0;
        t[4] = u0; t[5] = v1;
        t[6] = u1; t[7] = v1;
        glVertexPointer(2, GL_FLOAT, 0, v);
        glTexCoordPointer(2, GL_FLOAT, 0, t);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        x += cw;
    }
}

static unsigned buttons(void) {
    SceCtrlData pad;

    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    return pad.buttons;
}

static void row_label(int row, int lang, char *out, size_t outSz) {
    switch (row) {
    case ROW_LANG:
        snprintf(out, outSz, "Language: %s", VitaEu_Available() ? VitaEu_LocaleName(lang) : "(no pzs3eu1)");
        break;
    case ROW_WIDE:
        snprintf(out, outSz, "Widescreen: %s", gVitaWide ? "On" : "Off");
        break;
    case ROW_HUD:
        snprintf(out, outSz, "HUD style: %s", gVitaHudStyle ? "Centre" : "Edges");
        break;
    case ROW_MENU43:
        snprintf(out, outSz, "Menus in 4:3: %s", gVitaMenu43 ? "On" : "Off");
        break;
    case ROW_UNLOCK:
        snprintf(out, outSz, "Unlock everything...");
        break;
    case ROW_SKIPLOGO:
        snprintf(out, outSz, "Skip logos: %s", gVitaSkipLogos ? "On" : "Off");
        break;
    case ROW_SKIPINTRO:
        snprintf(out, outSz, "Skip intro: %s", gVitaSkipIntro ? "On" : "Off");
        break;
    case ROW_OUTLINE:
        snprintf(out, outSz, "Outline: %s", (gVitaFxMask & 8) ? "Off" : "On");
        break;
    case ROW_GLOW:
        snprintf(out, outSz, "Glow: %s", (gVitaFxMask & 2) ? "Off" : "On");
        break;
    case ROW_RESUME:
        snprintf(out, outSz, "Resume game");
        break;
    default:
        out[0] = '\0';
        break;
    }
}

static void draw_menu(int cursor, int scroll, int lang, int langDirty, int editingLang, int confirmUnlock) {
    int count = VitaEu_LocaleCount();
    int i;
    float px = 80.0f, py = 28.0f, pw = 800.0f, ph = 488.0f;
    float rowStep = 40.0f;
    char line[96];

    sync_screen();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, (GLsizei)sScrW, (GLsizei)sScrH);
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    draw_quad(0, 0, DESIGN_W, DESIGN_H, 0.0f, 0.0f, 0.0f, 0.55f);
    draw_quad(px, py, px + pw, py + ph, 0.05f, 0.08f, 0.05f, 0.92f);
    draw_quad(px, py, px + pw, py + 32.0f, 0.0f, 0.35f, 0.12f, 1.0f);
    draw_text(px + 220.0f, py + 8.0f, 2.0f, 0.6f, 1.0f, 0.6f, "BT3 SETTINGS");

    if (confirmUnlock) {
        draw_text(px + 24.0f, py + 70.0f, 2.0f, 1.0f, 0.9f, 0.4f, "Unlock everything?");
        draw_text(px + 24.0f, py + 110.0f, 1.5f, 0.85f, 0.85f, 0.7f, "All characters, stages, items.");
        draw_text(px + 24.0f, py + 138.0f, 1.5f, 0.85f, 0.85f, 0.7f, "Save to card afterwards.");
        draw_text(px + 24.0f, py + 190.0f, 2.0f, cursor == 0 ? 0.5f : 0.9f, 1.0f, cursor == 0 ? 0.5f : 0.9f,
                  cursor == 0 ? "> Yes, unlock" : "  Yes, unlock");
        draw_text(px + 24.0f, py + 230.0f, 2.0f, cursor == 1 ? 0.5f : 0.9f, 1.0f, cursor == 1 ? 0.5f : 0.9f,
                  cursor == 1 ? "> Cancel" : "  Cancel");
        draw_text(px + 24.0f, py + ph - 36.0f, 1.5f, 0.7f, 0.85f, 0.5f, "UP/DOWN  X:OK  O:back");
        return;
    }

    if (editingLang) {
        draw_text(px + 24.0f, py + 50.0f, 2.0f, 0.8f, 0.9f, 0.8f, "Language");
        for (i = 0; i < count; i++) {
            float yy = py + 90.0f + (float)i * 28.0f;
            int sel = (i == lang);
            if (sel) {
                draw_quad(px + 16.0f, yy - 4.0f, px + pw - 16.0f, yy + 22.0f, 0.0f, 0.45f, 0.15f, 1.0f);
            }
            snprintf(line, sizeof(line), "%s %s", sel ? ">" : " ", VitaEu_LocaleName(i));
            draw_text(px + 32.0f, yy, 2.0f, sel ? 0.5f : 0.85f, 1.0f, sel ? 0.5f : 0.85f, line);
        }
        draw_text(px + 24.0f, py + ph - 36.0f, 1.5f, 0.7f, 0.85f, 0.5f, "UP/DOWN  X:save  O:back");
        return;
    }

    for (i = 0; i < VISIBLE_ROWS; i++) {
        int row = scroll + i;
        float yy = py + 48.0f + (float)i * rowStep;
        int sel;
        char label[80];

        if (row >= ROW_COUNT) {
            break;
        }
        sel = (row == cursor);
        row_label(row, lang, label, sizeof(label));
        if (sel) {
            draw_quad(px + 16.0f, yy - 4.0f, px + pw - 16.0f, yy + 28.0f, 0.0f, 0.45f, 0.15f, 1.0f);
        }
        snprintf(line, sizeof(line), "%s %s", sel ? ">" : " ", label);
        draw_text(px + 32.0f, yy, 1.75f, sel ? 0.5f : 0.9f, 1.0f, sel ? 0.5f : 0.9f, line);
    }
    if (scroll > 0) {
        draw_text(px + pw - 60.0f, py + 40.0f, 1.5f, 0.5f, 0.8f, 0.5f, "^");
    }
    if (scroll + VISIBLE_ROWS < ROW_COUNT) {
        draw_text(px + pw - 60.0f, py + ph - 64.0f, 1.5f, 0.5f, 0.8f, 0.5f, "v");
    }
    draw_text(px + 24.0f, py + ph - 44.0f, 1.5f, 0.7f, 0.85f, 0.5f, "UP/DOWN  L/R or X:change  O:close");
    if (langDirty) {
        draw_text(px + 24.0f, py + ph - 24.0f, 1.5f, 1.0f, 0.85f, 0.2f, "Restart to apply language");
    }
}

static void toggle_row(int cursor) {
    switch (cursor) {
    case ROW_WIDE:
        apply_wide(!gVitaWide);
        Vita_Log("vita: settings widescreen %d\n", gVitaWide);
        break;
    case ROW_HUD:
        gVitaHudStyle = !gVitaHudStyle;
        bt3_set_int("hudstyle", gVitaHudStyle);
        Vita_Log("vita: settings hudstyle %d\n", gVitaHudStyle);
        break;
    case ROW_MENU43:
        gVitaMenu43 = !gVitaMenu43;
        bt3_set_int("menu43", gVitaMenu43);
        Vita_RefreshView();
        Vita_Log("vita: settings menu43 %d\n", gVitaMenu43);
        break;
    case ROW_SKIPLOGO:
        gVitaSkipLogos = !gVitaSkipLogos;
        bt3_set_int("skiplogo", gVitaSkipLogos);
        Vita_Log("vita: settings skiplogo %d\n", gVitaSkipLogos);
        break;
    case ROW_SKIPINTRO:
        gVitaSkipIntro = !gVitaSkipIntro;
        bt3_set_int("skipintro", gVitaSkipIntro);
        Vita_Log("vita: settings skipintro %d\n", gVitaSkipIntro);
        break;
    case ROW_OUTLINE:
        apply_fx_bit(8, (gVitaFxMask & 8) != 0); /* currently off → turn on */
        Vita_Log("vita: settings outline %s\n", (gVitaFxMask & 8) ? "off" : "on");
        break;
    case ROW_GLOW:
        apply_fx_bit(2, (gVitaFxMask & 2) != 0);
        Vita_Log("vita: settings glow %s\n", (gVitaFxMask & 2) ? "off" : "on");
        break;
    default:
        break;
    }
}

static void run_overlay(void) {
    int count = VitaEu_LocaleCount();
    int lang = 0, i;
    int cursor = 0, scroll = 0;
    int langDirty = 0;
    int editingLang = 0;
    int confirmUnlock = 0;
    int unlockCursor = 1; /* default Cancel */
    const char *cur = VitaEu_Locale();
    unsigned prev = 0, now, pressed;

    for (i = 0; i < count; i++) {
        if (strcmp(VitaEu_LocaleAt(i), cur) == 0) {
            lang = i;
            break;
        }
    }

    sOpen = 1;
    sPending = 0;
    Vita_GlSync(); /* vitaGL must be free (not mid-frame / render-thread) */
    sync_screen();
    Vita_Log("vita: settings menu open (%dx%d)\n", (int)sScrW, (int)sScrH);
    prev = buttons();

    for (;;) {
        now = buttons();
        pressed = now & ~prev;
        prev = now;

        if (confirmUnlock) {
            if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) {
                unlockCursor ^= 1;
            } else if (pressed & SCE_CTRL_CROSS) {
                if (unlockCursor == 0) {
                    if (gSaveData != NULL) {
                        Save_UnlockAll(gSaveData);
                        Vita_Log("vita: settings UnlockAll applied\n");
                    } else {
                        Vita_Log("vita: settings UnlockAll skipped (no save)\n");
                    }
                }
                confirmUnlock = 0;
            } else if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) {
                confirmUnlock = 0;
            }
            draw_menu(unlockCursor, 0, lang, langDirty, 0, 1);
            vglSwapBuffers(GL_FALSE);
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
            sceKernelDelayThread(16000);
            continue;
        }

        if (editingLang) {
            if (pressed & (SCE_CTRL_UP | SCE_CTRL_LEFT)) {
                if (VitaEu_Available()) {
                    lang = (lang + count - 1) % count;
                }
            } else if (pressed & (SCE_CTRL_DOWN | SCE_CTRL_RIGHT)) {
                if (VitaEu_Available()) {
                    lang = (lang + 1) % count;
                }
            } else if (pressed & SCE_CTRL_CROSS) {
                if (VitaEu_Available()) {
                    const char *loc = VitaEu_LocaleAt(lang);
                    if (strcmp(loc, VitaEu_Locale()) != 0) {
                        VitaEu_SetLocale(loc);
                        langDirty = 1;
                        Vita_Log("vita: settings language %s (restart to apply)\n", loc);
                    }
                }
                editingLang = 0;
            } else if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) {
                editingLang = 0;
            }
        } else {
            if (pressed & SCE_CTRL_UP) {
                cursor = (cursor + ROW_COUNT - 1) % ROW_COUNT;
            } else if (pressed & SCE_CTRL_DOWN) {
                cursor = (cursor + 1) % ROW_COUNT;
            } else if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) {
                if (cursor == ROW_LANG) {
                    editingLang = 1;
                } else if (cursor != ROW_UNLOCK && cursor != ROW_RESUME) {
                    toggle_row(cursor);
                }
            } else if (pressed & SCE_CTRL_CROSS) {
                if (cursor == ROW_LANG) {
                    editingLang = 1;
                } else if (cursor == ROW_UNLOCK) {
                    confirmUnlock = 1;
                    unlockCursor = 1;
                } else if (cursor == ROW_RESUME) {
                    break;
                } else {
                    toggle_row(cursor);
                }
            } else if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) {
                break;
            }
            if (cursor < scroll) {
                scroll = cursor;
            } else if (cursor >= scroll + VISIBLE_ROWS) {
                scroll = cursor - VISIBLE_ROWS + 1;
            }
        }

        draw_menu(cursor, scroll, lang, langDirty, editingLang, 0);
        vglSwapBuffers(GL_FALSE);
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        sceKernelDelayThread(16000);
    }

    sOpen = 0;
    Vita_RefreshView(); /* restore fight/menu viewport after full-screen overlay */
    Vita_Log("vita: settings menu close\n");
}

void Vita_SettingsPoll(void) {
    SceCtrlData pad;
    unsigned long long now;
    int selectAlone;

    if (sOpen || sPending) {
        return;
    }
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    selectAlone = (pad.buttons & SCE_CTRL_SELECT) != 0 &&
                  (pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) == 0;
    now = sceKernelGetProcessTimeWide();
    if (selectAlone) {
        if (!sSelectWasDown) {
            sSelectDownUs = now;
            sSelectWasDown = 1;
        } else if (now - sSelectDownUs >= HOLD_US) {
            /* Arm only: Present() runs the overlay after the frame swap (safe for menus). */
            sSelectWasDown = 0;
            sSelectDownUs = 0;
            sPending = 1;
            Vita_Log("vita: settings menu pending\n");
        }
    } else {
        sSelectWasDown = 0;
        sSelectDownUs = 0;
    }
}

void Vita_SettingsPresent(void) {
    if (!sPending || sOpen) {
        return;
    }
    run_overlay();
}
