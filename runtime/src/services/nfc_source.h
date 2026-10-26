/* NfcTagSource: where the nfp / nfc services get their tag from (portable). The Mac frontend's amiibo picker panel, the headless scripted selector and
 * the tests all implement or drive one. A tag is a UID plus either a virtual amiibo (the primary form) or a raw 540-byte NTAG215 image (a real dump that
 * could not be decrypted without the user's key: the services then see a tag that is not a readable amiibo). */
#pragma once
#include "amiibo_core.h"
#include <stdint.h>

typedef struct {
    uint8_t uid[7];
    int is_amiibo; /* 1: `amiibo` is valid; 0: only the UID (and optionally the image) is known */
    Amiibo amiibo;
    int has_image;
    uint8_t image[AMIIBO_TAG_SIZE]; /* raw NTAG215 memory, when the source has it (mifare / pass-through reads) */
} NfcTag;

typedef struct {
    void *ctx;
    const char *name;
    /* 1 and fills *out when a tag is on the reader, else 0. *generation changes whenever a tag is placed or removed. */
    int (*poll)(void *ctx, NfcTag *out, uint64_t *generation);
    /* Persists the tag's amiibo after a Flush. 0 ok, nonzero failure. May be NULL (read-only source). */
    int (*store)(void *ctx, const NfcTag *tag);
    /* The source calls cb(cbctx) whenever a tag is placed or removed, on any thread. May be NULL. */
    void (*set_listener)(void *ctx, void (*cb)(void *), void *cbctx);
} NfcTagSource;
