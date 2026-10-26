/* nfp / nfc core: the amiibo device state machine (portable). One NfpDevice per service connection, over an NfcTagSource.
 *
 * Games talk to nfp:user and get already-decoded structures (TagInfo, CommonInfo, ModelInfo, RegisterInfo, application area); this core produces them from a
 * virtual amiibo (amiibo_core.h). Command semantics, device states and result descriptions follow the author's memory of the public documentation
 * (unverified on a console or a title). Changes to a mounted amiibo are held in RAM until Flush and are lost when the tag is removed first. */
#pragma once
#include "hz_result.h"
#include "nfc_source.h"

#define NFP_MODULE 115
#define NFP_R(desc) HZ_RESULT(NFP_MODULE, desc)
#define NFP_R_DEVICE_NOT_FOUND NFP_R(64)
#define NFP_R_INVALID_ARGUMENT NFP_R(65)
#define NFP_R_WRONG_APP_AREA_SIZE NFP_R(68)
#define NFP_R_WRONG_DEVICE_STATE NFP_R(73)
#define NFP_R_NFC_DISABLED NFP_R(80)
#define NFP_R_TAG_REMOVED NFP_R(97)
#define NFP_R_REGISTRATION_NOT_INITIALIZED NFP_R(120)
#define NFP_R_APP_AREA_NOT_INITIALIZED NFP_R(128)
#define NFP_R_CORRUPTED_DATA NFP_R(144)
#define NFP_R_WRONG_APP_AREA_ID NFP_R(152)
#define NFP_R_APP_AREA_EXISTS NFP_R(168)
#define NFP_R_NOT_AN_AMIIBO NFP_R(178)
#define NFP_R_UNABLE_TO_ACCESS_BACKUP NFP_R(200)

typedef enum {
    NFP_DS_INIT = 0,
    NFP_DS_SEARCHING = 1,
    NFP_DS_TAG_FOUND = 2,
    NFP_DS_TAG_REMOVED = 3,
    NFP_DS_MOUNTED = 4,
    NFP_DS_UNAVAILABLE = 5,
    NFP_DS_FINALIZED = 6
} NfpDeviceState;
enum { NFP_EV_ACTIVATE = 1, NFP_EV_DEACTIVATE = 2 }; /* events the glue must signal after a call */
#define NFP_DEVICE_HANDLE 0x0000000100000000ull      /* the one reader; ListDevices reports it and every call checks it */

typedef struct {
    int initialized;
    NfpDeviceState state;
    NfcTagSource src;
    int have_src;
    uint64_t seen_gen;
    int gen_valid;
    NfcTag tag;
    int tag_present;
    Amiibo m; /* mounted working copy */
    int mounted, dirty, area_open;
    uint64_t app_id; /* title id used when this connection creates an application area */
    uint32_t events;
    AmiiboDate (*today)(void);
} NfpDevice;

void nfp_device_init(NfpDevice *d);
void nfp_set_source(NfpDevice *d, const NfcTagSource *src); /* NULL = no reader: the device reports Unavailable */
uint32_t nfp_take_events(NfpDevice *d);
/* Reads the source and applies tag changes to the state. Call from the source's listener and before any query. */
void nfp_poll(NfpDevice *d);

uint32_t nfp_initialize(NfpDevice *d, uint64_t app_id);
uint32_t nfp_finalize(NfpDevice *d);
int nfp_state(const NfpDevice *d);             /* 0 not initialised, 1 initialised (GetState) */
NfpDeviceState nfp_device_state(NfpDevice *d); /* polls first */
uint32_t nfp_start_detection(NfpDevice *d);
uint32_t nfp_stop_detection(NfpDevice *d);
uint32_t nfp_mount(NfpDevice *d, uint32_t model_type, uint32_t target);
uint32_t nfp_unmount(NfpDevice *d);
uint32_t nfp_flush(NfpDevice *d);
uint32_t nfp_restore(NfpDevice *d);

#define NFP_TAG_INFO_SIZE 0x58
#define NFP_COMMON_INFO_SIZE 0x40
#define NFP_MODEL_INFO_SIZE 0x40
#define NFP_REGISTER_INFO_SIZE 0x100
uint32_t nfp_get_tag_info(NfpDevice *d, uint8_t out[NFP_TAG_INFO_SIZE]);       /* tag found or mounted */
uint32_t nfp_get_common_info(NfpDevice *d, uint8_t out[NFP_COMMON_INFO_SIZE]); /* mounted */
uint32_t nfp_get_model_info(NfpDevice *d, uint8_t out[NFP_MODEL_INFO_SIZE]);   /* mounted */
uint32_t nfp_get_register_info(NfpDevice *d, uint8_t out[NFP_REGISTER_INFO_SIZE]);

uint32_t nfp_area_exists(NfpDevice *d, int *exists);
uint32_t nfp_area_open(NfpDevice *d, uint32_t access_id);
uint32_t nfp_area_size(NfpDevice *d, uint32_t *size);
uint32_t nfp_area_read(NfpDevice *d, void *out, size_t cap, size_t *n);                 /* needs the area open; reads min(cap, 216) bytes */
uint32_t nfp_area_write(NfpDevice *d, const void *data, size_t n);                      /* needs the area open */
uint32_t nfp_area_create(NfpDevice *d, uint32_t access_id, const void *data, size_t n); /* fails when one exists; the new area is open */
uint32_t nfp_area_recreate(NfpDevice *d, uint32_t access_id, const void *data, size_t n);
uint32_t nfp_area_delete(NfpDevice *d);
/* Owner registration (the Amiibo Settings applet's job on a console; the firmware applet host calls these). */
uint32_t nfp_set_nickname_owner(NfpDevice *d, const char *nick_utf8, const uint8_t mii[AMIIBO_MII_CHARINFO]);
uint32_t nfp_format(NfpDevice *d);

/* Type 2 pass-through (nfc:user SendCommandByPassThrough / mifare-style reads) over the tag memory. Supports GET_VERSION (0x60), READ (0x30) and FAST_READ (0x3A).
 * Returns the response length, or -1 for an unsupported command / a state without a tag. */
long nfc_passthrough(NfpDevice *d, const uint8_t *cmd, size_t n, uint8_t *out, size_t cap);
/* Structure builders (exposed for tests). */
void nfp_build_tag_info(const NfcTag *t, uint8_t out[NFP_TAG_INFO_SIZE]);
void nfp_build_common_info(const Amiibo *a, uint8_t out[NFP_COMMON_INFO_SIZE]);
void nfp_build_model_info(const Amiibo *a, uint8_t out[NFP_MODEL_INFO_SIZE]);
void nfp_build_register_info(const Amiibo *a, uint8_t out[NFP_REGISTER_INFO_SIZE]);
