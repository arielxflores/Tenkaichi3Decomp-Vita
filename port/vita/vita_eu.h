/* European languages from the player's SLES-54945 disc, same data layout as the PC recomp
 * (pzs3eu1/*.bin next to disc/). The USA program stays; selected language archives are read from
 * those files instead of PZS3US1.AFS. */
#ifndef VITA_EU_H
#define VITA_EU_H

void VitaEu_Init(const char *root); /* e.g. ux0:data/bt3 */
int VitaEu_Available(void);
const char *VitaEu_Locale(void); /* "en", "es", "de", "fr", "it" */
void VitaEu_SetLocale(const char *locale); /* persists to <root>/lang.txt */
int VitaEu_NeedsPrompt(void); /* EU data present and no lang chosen yet */
int VitaEu_LocaleCount(void);
const char *VitaEu_LocaleAt(int index); /* display order: en es de fr it */
const char *VitaEu_LocaleName(int index); /* English, Español, ... */

void VitaEu_NotePartition(int ptid, const char *fname);
/* 1 and fills out with a host path when this AFS member is replaced for the current language. */
int VitaEu_OverridePath(int ptid, int flid, char *out, int size);
/* Prefer the European opening soundtrack when it was extracted (ZS3EUOP.ADX). */
int VitaEu_OpeningAdxPath(char *out, int size);

#endif
