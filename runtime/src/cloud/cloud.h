/* Cloud saves in the runtime (Napple Account, end-to-end encrypted; docs/guides/cloud-saves.md). The sync engine is Rust
 * (crates/napple-cloud, linked as target/release/libnapple_cloud.a); this is the glue the runtime calls. Builds without runtime/src/cloud
 * (make CLOUD=0, or tools/build_ios.sh until it lists this directory) get the weak no-op definitions in main.c and services/fs.c. */
#pragma once

/* main.c, after the title id is known and before services_init (before the game can open a save): pulls the newest cloud save of each
 * linked local user into <save root>/<title>/save and starts the background push thread. Signed out = nothing happens. */
void cloud_runtime_start(void);
/* services/fs.c, after a journaled save root committed new data: debounced background push. Never blocks the calling (game) thread. */
void cloud_on_commit(const char *root);
/* services/fs.c fs_report_uncommitted (both clean exits: nnMain returning and the window/time limit): push what is pending, bounded wait. */
void cloud_on_exit(void);
