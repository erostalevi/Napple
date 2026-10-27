# Hardware simulators: NFC / amiibo and the IR camera

Features that need hardware get a simulator instead of being dropped. Everything portable is in `runtime/src/services/*_core.c` and tested on Linux; the Mac frontend
wires the user interface and the camera to small C interfaces (the "Mac wiring list" below).

## NFC / amiibo (`nfp:user`, `nfc:user`, `nfc:mf:u`)

Games talk to the nfp service, which returns already-decoded structures (TagInfo, CommonInfo, ModelInfo, RegisterInfo, application area). So the primary design is a
**virtual amiibo** (like emuiibo): its 8-byte model id, a generated UID, nickname and owner, register date and a locally persisted application area. No dump and no key are
needed.

- **Store**: `<support>/system/amiibo/<uid>.json` (`<support>` = `~/Library/Application Support/Napple`, see [system data folders](../reference/system-data-folders.md)), one small JSON per virtual amiibo, shared by every game (so a game's amiibo save persists across runs and across titles).
  Writes go through a temporary file and a rename. Format: `id` (16 hex), `uid` (14 hex), `nickname`, `owner_mii` (base64 CharInfo), `registered`, `register_date`, `write_date`,
  `write_counter`, `version`, `font_region`, `app_area` {`access_id`, `app_id`, `write_counter`, `data` (base64, 216 bytes)}. Only `id` is required in a hand-written file.
- **State machine** (`nfp_core.c`): Initialize, ListDevices, StartDetection, StopDetection, Mount, Unmount, Flush, Restore, GetTagInfo, GetCommonInfo, GetModelInfo,
  GetRegisterInfo, GetNpadId, the three events, and the application area calls (Open, Get, Set, Create, Recreate, GetSize; Delete, Exists, SetNicknameAndOwner and Format in the
  core for the firmware applet host). Device states Initialized, Searching, TagFound, TagRemoved, Mounted, Unavailable, Finalized. Changes live in RAM until Flush and are lost if
  the tag is removed first. Result codes (module 115) are in `nfp_core.h` and unverified.
- **Tag source** (`nfc_source.h`): `NfcTagSource` = `poll` (tag present + generation), `store` (persist after Flush), `set_listener` (called when a tag is placed or removed, which
  signals the activate/deactivate events). Without a source the services behave exactly as before: no device, state Unavailable.
- **Metadata library** (`amiibo_lib_core.h`): the index behind the picker (names, series, character, type, image URLs, 16-hex ids), loaded client-side from a configurable source:
  the [AmiiboAPI](https://github.com/N3evin/AmiiboAPI) repository's `database/amiibo.json` (the default, raw GitHub URL; the old `amiiboapi.com` endpoint answers 404), the API document shape, a URL, a local file or a mirror folder. The C loader reads both document shapes itself (the repository's `amiibos` map is joined with its `characters`, `amiibo_series`, `game_series` and `types` tables, and icon URLs are derived), so the frontend passes the fetched text through unchanged.
  AmiiboAPI is MIT licensed, metadata only, and its repository was archived read-only on 2025-12-31, so a successful fetch is cached
  (`<support>/system/amiibo-cache/amiibo.json`) and the cache is the fallback when offline. The repository ships only the schema, the loader, the config key and the default
  source URL; **never any amiibo data**. Source: `NAPPLE_AMIIBO_DB` (URL, file or folder).
- **Picker contract** (`AmiiboPicker`): `amiibo_picker_count/get/search` (list and search), `amiibo_picker_select(_hex/_uid)` (reuse the stored virtual amiibo of that model, or
  `AMIIBO_PICK_NEW` for another with a new UID), `amiibo_picker_remove`, `amiibo_picker_import_dump`, `amiibo_picker_source` (hand to `nfp_service_set_source`). `amiibo_service_picker()`
  returns the process-wide picker.
- **Headless**: `NAPPLE_AMIIBO=<what>[,<insert_ms>[,<remove_ms>]]` where `<what>` is a model id, a stored UID or a dump path. It installs the picker as the reader and places the amiibo
  at the first StartDetection (after `insert_ms`), removing it `remove_ms` later.
- **Optional real dumps**: a 540-byte NTAG215 dump plus the user's retail key file (`key_retail.bin`, 160 bytes, supplied like `prod.keys`; default `~/.switch/key_retail.bin`,
  override `NAPPLE_AMIIBO_KEY`) becomes a virtual amiibo. Without the key file the picker reports the exact missing path. Keys and dumps are never shipped; the tests generate a
  test-only key. The crypto (SHA-256, HMAC, AES-128-CTR, the key derivation) is checked against published vectors and by round trip; **compatibility with real dumps is unverified**.
- `nfc:user` also answers `SendCommandByPassThrough` for the Type 2 commands READ, FAST_READ and GET_VERSION (UID pages and capability container synthesised for virtual tags).
  `nfc:mf:u` Read/Write (Mifare Classic) report failure: an amiibo tag is not a Mifare card.

## IR camera (`irs`)

`irs_core.c` simulates the Joy-Con IR sensor over an `IrFrameSource`: 320x240 8-bit near-IR intensity frames.

- **Processors**: moment (window split into 8x6 blocks: average intensity and centroid per block, preprocess none/binarize/cutoff, negative image), clustering (8-connected blobs above an
  intensity threshold, pixel-count limits, window, up to 16 clusters largest first, with average, centroid and bounds), image transfer (origin format 320x240..20x15 by box averaging,
  trimming window, written into the game's transfer memory and returned by `GetImageTransferProcessorState`). Tera plugin, pointing/Dpd, IR LED and extended transfer are accepted and
  produce nothing while a source exists.
- **Shared memory**: one 0x800-byte slot per controller id; each state is guarded by a seqlock (`seq` odd while writing). A pump thread (about 60 Hz) runs only while a processor runs.
  Layout and config structs are from memory and unverified (`irs_core.h`).
- **Access**: the source's `request_access` is called when a game starts its first processor, never before; denied = "sensor not connected", pending = processors run on dark frames
  until the answer. With no source every processor reports the sensor as not connected (the previous behaviour).
- **Sources**: the frontend's webcam (see below), or `NAPPLE_IR_SOURCE=blob:x,y,r[,dx,dy]` / `dark` / `flat:v` for headless runs and tests.
- **Webcam adapter**: `irs_adapter_rgb_to_ir` converts an RGB(A)/BGR(A) camera frame to near-IR-like grayscale (luminance, centred 4:3 crop, box resample, optional mirror, percentile contrast
  stretch, optional threshold or binarize).

## Mac wiring (done: `frontend/src/simulators.m`, 2026-10-26 studio-redo)

Both simulators live in the game's own overlay (F1 or the controller Home button), on the **Game/System** tab (amiibo and IR camera sections); the persistent settings (list source, key file, store) are also in Napple Studio > Settings > Amiibo.

1. **amiibo tab**: search field + list over `amiibo_service_picker()` (`amiibo_picker_search/get`), *Place* (`amiibo_picker_select_hex`, reuse), *Place new copy*
   (`AMIIBO_PICK_NEW`), *Remove* (`amiibo_picker_remove`), *Reload list* (the cache / local source first via `amiibo_lib_refresh(..., NULL, 0)`, then an `NSURLSession`
   fetch of `cfg.source` handed to `amiibo_lib_refresh`), *Import dump…* (`amiibo_picker_import_dump`, `last_error` shown). The picker is installed with
   `nfp_service_set_source(amiibo_picker_source(p))` the first time the tab is opened, so a game that never opens it keeps the old "no reader" behaviour.
   Default source: `https://raw.githubusercontent.com/N3evin/AmiiboAPI/master/database/amiibo.json` (the `amiiboapi.com` endpoint is offline since the archive); the
   loader accepts both the API shape and the repository database shape (`amiibos` + `characters`/`amiibo_series`/`game_series`/`types` tables).
2. **Camera tab**: status (`AVCaptureDevice.authorizationStatus`), *Allow webcam now* / *Open Privacy Settings*, *Mirror the image*. The `IrFrameSource` (`g_cam_source`)
   is installed once at window creation (`fe_simulators_install`, called before `fe_build_overlay`): `request_access` maps the AVFoundation status to `IR_ACCESS_*`
   and answers a pending prompt through `done`; `start`/`stop` run an `AVCaptureSession` (640x480 BGRA) whose frames go through `irs_adapter_rgb_to_ir`; `get_frame`
   hands the newest converted frame to the pump. The prompt appears only when a game starts an IR processor.
3. **Packaging**: the game app's `Info.plist` carries `NSCameraUsageDescription` and `NSMicrophoneUsageDescription` (`NAPPLE_AUDIN_SOURCE=mic`), and the ad-hoc signature carries
   the `com.apple.security.device.camera` and `com.apple.security.device.audio-input` entitlements (`bundle.rs`: `mac_entitlements()`). There is deliberately **no App Sandbox**
   entitlement (a Rust test guards it): the shared system folders and the user's key file must stay reachable. Studio's own entitlements file carries the same two.
4. **Linking**: the frontend is a static library linked into the runtime, so the runtime symbols above are weak references in `simulators.m` (`__attribute__((weak))`);
   the frontend's own test binaries link with `-undefined dynamic_lookup` and the tabs then say the feature is unavailable. The frontend's `.m` files compile with
   `-fmodules` so `AVFoundation` is auto-linked without touching the runtime's link line.
5. **Settings**: the list source, the key file and the store folder are fields of the overlay's amiibo section and of Studio's Settings > Amiibo; both write
   `<support>/system.json` (`amiibo_database`, `amiibo_key`, `amiibo_store`), which the runtime turns into `NAPPLE_AMIIBO_DB` / `_KEY` / `_STORE` at boot (the environment wins).
   The reader is installed lazily (first Place / Import, or `NAPPLE_AMIIBO`), never at boot.

Scripted check: `NAPPLE_FE_OVERLAY=3` (Game/System; 0 Audio/Video, 1 FPS/Speed, 2 Controls) opens the overlay on that tab at start; `NAPPLE_FE_VIEWSHOT=<png>:<seconds>` renders it to a PNG.
