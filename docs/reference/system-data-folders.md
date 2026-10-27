# System data folders

Where Napple keeps data that is **not** part of one game. One root serves every generated game app, Napple Studio and the developer scripts, so a Mii created in one game
exists in all of them and nothing is configured twice.

## The root

`~/Library/Application Support/Napple/` (the "support directory"), or `NAPPLE_SUPPORT_DIR` when set. The runtime resolves it once at boot (`g_cfg.support_dir`, helper
`napple_support_dir()` in `runtime/src/napple.h`). **`--save-root` does not move it**: that option only moves per-game saves (`<save root>/<title id>/`). The launcher, the runtime,
Studio (`SystemData.supportDir`), `tools/prepare_game.sh`, `tools/boot_test.sh` and `tools/golden/golden.py` all resolve the same root; `runtime/tests/test_svc_sysroot.c`
and the Studio test `SystemDataTests` check it. The generated Mac apps are **not sandboxed** (no `com.apple.security.app-sandbox`; a Rust test guards it), because a sandboxed
app could not see this folder, the user's key file or an iCloud Drive Mii folder. iOS apps are sandboxed and use their own container (`runtime/src/main.c`).

| Path under the root | What | Written by | Read by |
|---|---|---|---|
| `system.json` | the one configuration file for the simulators and the online hook (keys below) | Studio (Settings > Mii / Amiibo / Online), the in-game overlay (amiibo tab) | the runtime at boot (`sysconfig_core.c`) |
| `system/mii/MiiDatabase.dat` (+ `.bak`, `.lock`) | the Mii database shared by every game (up to 100 Miis; the six defaults are code) | `mii:u` / `mii:e`, the Mii Maker applet | every game, Studio's Mii pane |
| `system/amiibo/<uid>.json` | virtual amiibo and their game data (application area) | the amiibo picker, `NAPPLE_AMIIBO` | `nfp` / `nfc` |
| `system/amiibo-cache/amiibo.json` | the last fetched amiibo list (offline fallback; metadata only, never committed) | the amiibo list reload | the picker |
| `system/fonts/*.ttf` | the shared system fonts (`pl:u`) | the launcher on start (from the game bundle), `napple build --dump-fonts` | `pl:u` |
| `system/icon/SubstituteUserIcon.jpg` | the default profile picture | `napple build` | `acc` |
| `system/applet-run-<pid>/` | scratch storages of a running Mii Maker child | `applet.c` | the child; removed afterwards |
| `<title id>/` (save root) | per-game saves, `settings.json`, `input.json`, caches | the runtime / overlay | the runtime |
| `keysets/<name>/`, `engine/`, `Builds/`, `library.json`, `logs/` | Studio's own data (keys are owner-only) | Studio | Studio |
| `goldens/` | approved golden frames of the graphics gate (never in the repository) | `tools/golden/golden.py approve` | `golden.py run` |

## `system.json`

A flat JSON object, all keys optional and empty by default. At boot each non-empty key becomes its environment variable **unless that variable is already set** (the
environment always wins, so scripts and tests override the file). Unknown keys are ignored.

| Key | Environment variable | Meaning |
|---|---|---|
| `amiibo_database` | `NAPPLE_AMIIBO_DB` | amiibo list source: URL, a `.json` file, or a folder holding `amiibo.json` / `database/amiibo.json` (default: the AmiiboAPI repository's raw `database/amiibo.json`, configuration only, no data shipped) |
| `amiibo_key` | `NAPPLE_AMIIBO_KEY` | `key_retail.bin` for importing real dumps (default `~/.switch/key_retail.bin`) |
| `amiibo_store` | `NAPPLE_AMIIBO_STORE` | folder of the virtual amiibo (default `system/amiibo`) |
| `online_provider` | `NAPPLE_ONLINE_PROVIDER` | shared library implementing the online provider ABI; empty or unloadable = the offline NullProvider, with one log line |
| `mii_dir` | `NAPPLE_MII_DIR` | folder of `MiiDatabase.dat` (any folder; an iCloud Drive one included) |

See [hardware simulators](../guides/hardware-simulators.md), [network providers](../guides/network-providers.md) and [Studio](../guides/studio.md).
