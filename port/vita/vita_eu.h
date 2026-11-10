/* The European languages from the player's own European disc (vita_eu.c). */
#ifndef VITA_EU_H
#define VITA_EU_H

#include <psp2/types.h>
#include <stdint.h>

void VitaEu_Init(const char *home);
int VitaEu_Available(void);
int VitaEu_Lang(void);            /* 0 English (the USA disc's files), 1 Spanish, 2 German, 3 French, 4 Italian */
int VitaEu_UsesMenuRemap(void);   /* a European language is in use */
void VitaEu_ChooseAtStart(void);
void VitaEu_NotePartition(int ptid, const char *name);
int VitaEu_Member(int ptid, int flid, SceUID *fd, uint32_t *base, uint32_t *size, int *own);
SceUID VitaEu_Fd(void);
void VitaEu_PatchToc(int ptid, uint32_t *table, uint32_t count);
int VitaEu_OpeningSong(char *out, int size);

#endif
