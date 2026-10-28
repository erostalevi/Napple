/* Mods at run time (napple build --mods): per-frame cheats and the merged mod config, both read from <data-dir>/mods/ (cheats.txt, config.json).
 * The RomFS overlay is separate (romfs_map.c, served through fs.c). NAPPLE_NO_MODS=1 turns all of it off. */
#pragma once
#include <stdint.h>

struct napple_settings;
/* Reads the files once (idempotent, thread-safe); safe to call without a mods directory. */
void mods_runtime_init(void);
/* Runs every cheat once. Called from the display vsync thread: a frame-synchronous point, never from guest code. */
void mods_cheats_frame(void);
/* Overrides s with the merged config.json (renderScale, aspect, fpsMode). The overrides are for the process only: the caller must pass a copy and never save it.
 * Returns 1 when something was changed. */
int mods_config_apply_settings(struct napple_settings *s);
/* fixes: {"name": true|false} of the merged config; dflt when the mod set does not mention it. */
int mods_fix_enabled(const char *name, int dflt);
/* Test hook: the main module's memory (the runtime asks module_by_name("main")). */
void mods_runtime_set_main(uint8_t *base, uint64_t size, uint64_t writable_from);
