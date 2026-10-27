/* amiibo library + picker (portable): the metadata index behind the amiibo picker panel, and the picker contract the Mac frontend calls.
 *
 * The index is loaded client-side at runtime from a configurable source and never stored in this repository. Supported inputs: the AmiiboAPI HTTP document
 * ({"amiibo":[{"head","tail","name","character","gameSeries","amiiboSeries","type","image",...}]}, https://github.com/N3evin/AmiiboAPI, MIT, metadata only)
 * and the AmiiboAPI repository's database/amiibo.json ({"amiibos": {"0x<16 hex>": {"name": ...}}, "characters", "amiibo_series", "game_series", "types"}, or the bare {"0x<16 hex>": {...}} map). The upstream repository is archived (read-only since 2025-12-31), so a
 * successful fetch is cached locally and the cache is the fallback when offline; the source can be a mirror, a fork or a local folder.
 * Fetching a URL is the frontend's job (URLSession); the library takes the fetched text. */
#pragma once
#include "nfc_source.h"
#include <pthread.h>

/* amiiboapi.com answers 404 since the project was archived: the default is the archived repository's raw database (metadata only, MIT), a configuration default,
 * not bundled data; override with NAPPLE_AMIIBO_DB (URL, file or folder) or the amiibo_database setting. */
#define AMIIBO_DEFAULT_SOURCE "https://raw.githubusercontent.com/N3evin/AmiiboAPI/master/database/amiibo.json"
#define AMIIBO_REPO_IMAGE_FMT "https://raw.githubusercontent.com/N3evin/AmiiboAPI/master/images/icon_%.8s-%.8s.png"

typedef struct {
    char id[17]; /* 16 hex digits: head + tail */
    char name[64], character[48], series[64], game_series[64], type[16], image[200];
} AmiiboInfo;
typedef struct {
    AmiiboInfo *items;
    size_t count, cap;
} AmiiboLib;
typedef struct {
    char source[512]; /* URL (http/https), a local .json file, or a folder holding amiibo.json or database/amiibo.json */
    char cache[512];  /* where a fetched document is cached */
    char store[512];  /* the virtual amiibo store */
    char key[512];    /* key_retail.bin for the optional dump import */
} AmiiboConfig;

/* Defaults under `support_dir` (~/Library/Application Support/Napple): store <support>/system/amiibo, cache <support>/system/amiibo-cache/amiibo.json, key
 * $HOME/.switch/key_retail.bin. NAPPLE_AMIIBO_DB overrides the source, NAPPLE_AMIIBO_KEY the key path, NAPPLE_AMIIBO_STORE the store. */
void amiibo_config_default(AmiiboConfig *c, const char *support_dir);
int amiibo_config_source_is_remote(const AmiiboConfig *c);

void amiibo_lib_init(AmiiboLib *l);
void amiibo_lib_free(AmiiboLib *l);
/* Replace the contents from a document. Returns the entry count (>0) or -1 when the text is malformed or holds no amiibo (the old contents are kept). */
long amiibo_lib_load_text(AmiiboLib *l, const char *text, size_t n);
long amiibo_lib_load_file(AmiiboLib *l, const char *path);
/* Opens a local source: the file itself, or <dir>/amiibo.json, <dir>/database/amiibo.json, <dir>/api/amiibo/index.json. */
long amiibo_lib_load_source(AmiiboLib *l, const char *path);
/* The refresh policy: a freshly fetched document (NULL when the fetch failed or was skipped) is validated, cached and used; otherwise the cache is used; otherwise
 * a local source. Returns 1 fetched, 2 cache, 3 local source, or -1 when nothing could be loaded (the library is then empty). */
int amiibo_lib_refresh(AmiiboLib *l, const AmiiboConfig *c, const char *fetched, size_t n);
const AmiiboInfo *amiibo_lib_find(const AmiiboLib *l, const uint8_t id[8]);
/* Case-insensitive substring match over name, character, series, game series and id; empty query = everything. Fills idx (up to max) in name order and returns the
 * total number of matches. */
size_t amiibo_lib_search(const AmiiboLib *l, const char *query, size_t *idx, size_t max);

/* ---- picker contract (what the frontend calls) ---------------------------------------------------------------------------------- */
typedef struct AmiiboPicker {
    AmiiboConfig cfg;
    AmiiboLib lib;
    NfcTag tag;
    int present;
    uint64_t generation;
    void (*listener)(void *);
    void *lctx;
    pthread_mutex_t lock;
    NfcTagSource source;
    char last_error[600]; /* human-readable reason for the last failed call, naming the exact missing file where that is the cause */
} AmiiboPicker;
enum { AMIIBO_PICK_REUSE = 0, AMIIBO_PICK_NEW = 1 }; /* select: reuse the stored virtual amiibo of this model, or make another one with a new UID */

void amiibo_picker_init(AmiiboPicker *p, const AmiiboConfig *c);
void amiibo_picker_free(AmiiboPicker *p);
const NfcTagSource *amiibo_picker_source(AmiiboPicker *p); /* hand this to the nfp service (nfp_service_set_source) */
/* list / search over the library */
size_t amiibo_picker_count(const AmiiboPicker *p);
const AmiiboInfo *amiibo_picker_get(const AmiiboPicker *p, size_t i);
size_t amiibo_picker_search(const AmiiboPicker *p, const char *query, size_t *idx, size_t max);
/* Places an amiibo on the reader. 0 ok; -1 unknown id (not a 16-digit hex id); -2 the store cannot be written. */
int amiibo_picker_select(AmiiboPicker *p, const uint8_t id[8], int flags);
int amiibo_picker_select_hex(AmiiboPicker *p, const char *id_hex, int flags);
int amiibo_picker_select_uid(AmiiboPicker *p, const uint8_t uid[7]); /* an existing stored virtual amiibo; -1 when missing */
/* Optional real dump: a 540-byte .bin and the user's key file. 0 ok; -1 dump unreadable; -2 key file missing (last_error names it); -3 key file invalid;
 * -4 signature mismatch (wrong key or damaged dump). */
int amiibo_picker_import_dump(AmiiboPicker *p, const char *dump_path);
void amiibo_picker_remove(AmiiboPicker *p);
int amiibo_picker_present(AmiiboPicker *p);

/* Headless scripted selector, NAPPLE_AMIIBO=<what>[,<insert_ms>[,<remove_ms>]]: <what> is a 16-digit model id, a 14-digit stored UID, or a path to a .bin dump.
 * insert_ms counts from the first StartDetection (0 = at once), remove_ms from the insert (0 = never). The nfp service runs it on a timer thread. */
typedef struct {
    char what[512];
    unsigned insert_ms, remove_ms;
    int valid;
} AmiiboScript;
int amiibo_script_parse(const char *spec, AmiiboScript *s);
int amiibo_script_apply(AmiiboPicker *p, const AmiiboScript *s); /* performs the selection; same results as the select calls */

/* ---- service-level API (offline.c) for the frontend ----------------------------------------------------------------------------- */
/* The process-wide picker, created on first use with amiibo_config_default(save_root). The frontend loads its library (amiibo_lib_refresh on
 * picker->lib), drives it from the picker panel, and installs it with nfp_service_set_source(amiibo_picker_source(p)). */
AmiiboPicker *amiibo_service_picker(void);
/* Installs the tag source the nfp / nfc services read (NULL = no reader: ListDevices reports no device). NAPPLE_AMIIBO installs the picker when no source was set. */
void nfp_service_set_source(const NfcTagSource *src);
