/* In-game settings overlay (hold SELECT ~1s): language and widescreen. */
#ifndef VITA_SETTINGS_H
#define VITA_SETTINGS_H

/* Call each pad read. When SELECT is held alone long enough, opens the overlay (blocks until closed). */
void Vita_SettingsPoll(void);

/* 1 while the overlay is open (pad reads should report no buttons to the game). */
int Vita_SettingsOpen(void);

#endif
