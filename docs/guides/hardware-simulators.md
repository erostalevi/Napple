# Hardware simulators: NFC / amiibo and the IR camera

Features that need hardware get a simulator instead of being dropped. Everything portable is in `runtime/src/services/*_core.c` and tested on Linux; the Mac frontend
wires the user interface and the camera to small C interfaces (the "Mac wiring list" below).

## NFC / amiibo (`nfp:user`, `nfc:user`, `nfc:mf:u`)

Games talk to the nfp service, which returns already-decoded structures (TagInfo, CommonInfo, ModelInfo, RegisterInfo, application area). So the primary design is a
**virtual amiibo** (like emuiibo): its 8-byte model id, a generated UID, nickname and owner, register date and a locally persisted application area. No dump and no key are
needed.

- **Store**: `<save-root>/system/amiibo/<uid>.json`, one small JSON per virtual amiibo, shared by every game (so a game's amiibo save persists across runs and across titles).
  Writes go through a temporary file and a rename. Format: `id` (16 hex), `uid` (14 hex), `nickname`, `owner_mii` (base64 CharInfo), `registered`, `register_date`, `write_date`,
  `write_counter`, `version`, `font_region`, `app_area` {`access_id`, `app_id`, `write_counter`, `data` (base64, 216 bytes)}. Only `id` is required in a hand-written file.
- **State machine** (`nfp_core.c`): Initialize, ListDevices, StartDetection, StopDetection, Mount, Unmount, Flush, Restore, GetTagInfo, GetCommonInfo, GetModelInfo,
  GetRegisterInfo, GetNpadId, the three events, and the application area calls (Open, Get, Set, Create, Recreate, GetSize; Delete, Exists, SetNicknameAndOwner and Format in the
  core for the firmware applet host). Device states Initialized, Searching, TagFound, TagRemoved, Mounted, Unavailable, Finalized. Changes live in RAM until Flush and are lost if
  the tag is removed first. Result codes (module 115) are in `nfp_core.h` and unverified.
- **Tag source** (`nfc_source.h`): `NfcTagSource` = `poll` (tag present + generation), `store` (persist after Flush), `set_listener` (called when a tag is placed or removed, which
  signals the activate/deactivate events). Without a source the services behave exactly as before: no device, state Unavailable.
- **Metadata library** (`amiibo_lib_core.h`): the index behind the picker (names, series, character, type, image URLs, 16-hex ids), loaded client-side from a configurable source:
  the [AmiiboAPI](https://github.com/N3evin/AmiiboAPI) HTTP document (`https://amiiboapi.com/api/amiibo/`) or its `database/amiibo.json`, a URL, a local file or a mirror folder.
  AmiiboAPI is MIT licensed, metadata only, and its repository was archived read-only on 2025-12-31, so a successful fetch is cached
  (`<save-root>/system/amiibo-cache/amiibo.json`) and the cache is the fallback when offline. The repository ships only the schema, the loader, the config key and the default
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

## Mac wiring list (for the later Mac step; nothing here is wired yet)

1. **Tag picker panel**: build the panel on `amiibo_service_picker()`: load the library (fetch `cfg.source` with URLSession and pass the text to `amiibo_lib_refresh`, which caches it),
   list/search, call `amiibo_picker_select` / `amiibo_picker_remove`, offer `amiibo_picker_import_dump` (show `last_error`), then `nfp_service_set_source(amiibo_picker_source(p))` once at start-up
   when amiibo support should be offered.
2. **Webcam adapter**: an `IrFrameSource` whose `get_frame` runs `irs_adapter_rgb_to_ir` on the latest AVCaptureSession frame; install with `irs_service_set_source`. Start the capture session in
   `start()` and stop it in `stop()`.
3. **Permission prompts**: `request_access` triggers `AVCaptureDevice.requestAccess(for: .video)` and calls the `done` callback with the answer; return `IR_ACCESS_GRANTED` when already authorised.
   Packaging (not edited here): add `NSCameraUsageDescription` to Info.plist (for example "Some games use the IR camera of a Joy-Con; Napple can use your webcam to simulate it") and
   the `com.apple.security.device.camera` entitlement.
4. **Settings**: `amiibo_database` (source URL/folder) and the key file path map to `NAPPLE_AMIIBO_DB` and `NAPPLE_AMIIBO_KEY`.
