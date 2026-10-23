/* In-game settings (hold SELECT ~1s): native MsgDialog sheet for language and widescreen. */
#ifndef VITA_SETTINGS_H
#define VITA_SETTINGS_H

/* Call each pad read. When SELECT is held alone long enough, opens the system dialog (blocks until closed). */
void Vita_SettingsPoll(void);

/* 1 while the settings dialog is open (pad reads should report no buttons to the game). */
int Vita_SettingsOpen(void);

#endif
