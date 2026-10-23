/* In-game settings (hold SELECT ~1s): Adrenaline-style pause menu for language and widescreen. */
#ifndef VITA_SETTINGS_H
#define VITA_SETTINGS_H

/* Call each pad read. When SELECT is held alone long enough, opens the menu (blocks until closed). */
void Vita_SettingsPoll(void);

/* 1 while the settings menu is open (pad reads should report no buttons to the game). */
int Vita_SettingsOpen(void);

#endif
