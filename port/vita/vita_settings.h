/* In-game settings (hold SELECT ~1s): Adrenaline-style GL pause menu
 * (language, widescreen, HUD/menus layout, skip boot, FX, unlock). */
#ifndef VITA_SETTINGS_H
#define VITA_SETTINGS_H

/* Call each pad read: only arms a pending open (does not draw / swap). */
void Vita_SettingsPoll(void);

/* Call once per frame after the game's swap (GsGpu_FrameEnd): runs the overlay if pending. */
void Vita_SettingsPresent(void);

/* 1 while the settings menu is open or about to open (pad reads report no buttons). */
int Vita_SettingsOpen(void);

#endif
