/* System configuration (portable): the ONE file where the hardware simulators, the online provider and the Mii folder are configured for every game.
 * <support>/system.json (~/Library/Application Support/Napple/system.json) is written by Napple Studio and by the in-game overlay and read by the runtime at boot;
 * each non-empty key becomes its environment variable unless the variable is already set (the environment always wins, so scripts and tests can override).
 *   amiibo_database -> NAPPLE_AMIIBO_DB     URL, .json file or folder of the amiibo list
 *   amiibo_key      -> NAPPLE_AMIIBO_KEY    key_retail.bin
 *   amiibo_store    -> NAPPLE_AMIIBO_STORE  folder of the virtual amiibo (default <support>/system/amiibo)
 *   online_provider -> NAPPLE_ONLINE_PROVIDER  provider module; empty = the NullProvider (offline)
 *   mii_dir         -> NAPPLE_MII_DIR       folder of MiiDatabase.dat (default <support>/system/mii)
 * Unknown keys are ignored (and dropped when the runtime saves). */
#pragma once
#include <stddef.h>

typedef struct {
    char amiibo_database[512], amiibo_key[512], amiibo_store[512], online_provider[512], mii_dir[512];
} SysConfig;

int sysconfig_path(const char *support, char *out, size_t cap);
/* 0 = read; 1 = no file (all empty); -1 = unreadable or malformed (all empty). */
int sysconfig_load(const char *support, SysConfig *c);
int sysconfig_save(const char *support, const SysConfig *c); /* atomic (temp + rename); 0 or -1 */
/* setenv(overwrite = 0) for every non-empty key; returns how many variables were set. */
int sysconfig_apply_env(const SysConfig *c);
