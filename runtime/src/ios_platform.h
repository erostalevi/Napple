/* iOS process shell helpers (ios_platform.m). Only meaningful when NAPPLE_IOS. */
#pragma once
#include "platform.h"
#if NAPPLE_IOS
/* <App.app>/game/<version dir>: $NAPPLE_VERSION, else the first line of game/default. NULL when the bundle has no game. */
const char *ios_default_data_dir(void);
/* <app container>/Documents/Napple: visible in the Files app (UIFileSharingEnabled), so saves can be backed up. */
const char *ios_default_save_root(void);
/* The launcher's job on macOS: copy the .ttf files in game/system/fonts to <save-root>/system/fonts where pl:u reads them. */
void ios_install_fonts(const char *data_dir, const char *save_root);
/* The Mii database folder shared by every Napple game app of this developer: <App Group container>/Napple/mii, the group named by the Info.plist key
 * NappleAppGroup (also listed in the entitlements). NULL when the app has no group (then the database stays in the app's own save root). */
const char *ios_shared_mii_dir(void);
#endif
