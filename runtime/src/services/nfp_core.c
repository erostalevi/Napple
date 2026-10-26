#include "nfp_core.h"
#include <string.h>
#include <time.h>

static AmiiboDate default_today(void) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    return (AmiiboDate){(uint16_t)(tm.tm_year + 1900), (uint8_t)(tm.tm_mon + 1), (uint8_t)tm.tm_mday};
}
void nfp_device_init(NfpDevice *d) {
    memset(d, 0, sizeof *d);
    d->state = NFP_DS_INIT;
    d->today = default_today;
}
void nfp_set_source(NfpDevice *d, const NfcTagSource *src) {
    d->have_src = src != NULL;
    if (src) d->src = *src;
    d->gen_valid = 0;
}
uint32_t nfp_take_events(NfpDevice *d) {
    uint32_t e = d->events;
    d->events = 0;
    return e;
}

/* the tag on the reader changed (or was seen for the first time) */
void nfp_poll(NfpDevice *d) {
    if (!d->have_src || !d->src.poll) return;
    NfcTag t;
    uint64_t gen = 0;
    int present = d->src.poll(d->src.ctx, &t, &gen);
    if (d->gen_valid && gen == d->seen_gen) return;
    d->gen_valid = 1;
    d->seen_gen = gen;
    int had = d->tag_present;
    d->tag_present = present;
    if (present) d->tag = t;
    if (had && (d->state == NFP_DS_TAG_FOUND || d->state == NFP_DS_MOUNTED)) { /* removed, or replaced by another tag: the old one is gone either way */
        d->state = NFP_DS_TAG_REMOVED;
        d->mounted = d->dirty = d->area_open = 0;
        d->events |= NFP_EV_DEACTIVATE;
        return;
    }
    if (present && d->state == NFP_DS_SEARCHING) {
        d->state = NFP_DS_TAG_FOUND;
        d->events |= NFP_EV_ACTIVATE;
    }
}

uint32_t nfp_initialize(NfpDevice *d, uint64_t app_id) {
    if (d->initialized) return NFP_R_WRONG_DEVICE_STATE;
    d->initialized = 1;
    d->state = d->have_src ? NFP_DS_INIT : NFP_DS_UNAVAILABLE;
    d->app_id = app_id;
    d->gen_valid = 0;
    d->tag_present = d->mounted = d->dirty = d->area_open = 0;
    return 0;
}
uint32_t nfp_finalize(NfpDevice *d) {
    if (!d->initialized) return NFP_R_WRONG_DEVICE_STATE;
    d->initialized = 0;
    d->state = NFP_DS_FINALIZED;
    d->mounted = d->dirty = d->area_open = 0;
    return 0;
}
int nfp_state(const NfpDevice *d) { return d->initialized; }
NfpDeviceState nfp_device_state(NfpDevice *d) {
    nfp_poll(d);
    return d->state;
}
static uint32_t need_init(const NfpDevice *d) { return d->initialized ? 0 : NFP_R_WRONG_DEVICE_STATE; }
static uint32_t wrong_state(const NfpDevice *d) { return d->state == NFP_DS_TAG_REMOVED ? NFP_R_TAG_REMOVED : NFP_R_WRONG_DEVICE_STATE; }

uint32_t nfp_start_detection(NfpDevice *d) {
    uint32_t r = need_init(d);
    if (r) return r;
    if (d->state == NFP_DS_UNAVAILABLE) return NFP_R_NFC_DISABLED;
    nfp_poll(d);
    if (d->state != NFP_DS_INIT && d->state != NFP_DS_TAG_REMOVED) return NFP_R_WRONG_DEVICE_STATE;
    d->state = NFP_DS_SEARCHING;
    d->mounted = d->dirty = d->area_open = 0;
    if (d->tag_present) {
        d->state = NFP_DS_TAG_FOUND;
        d->events |= NFP_EV_ACTIVATE;
    }
    return 0;
}
uint32_t nfp_stop_detection(NfpDevice *d) {
    uint32_t r = need_init(d);
    if (r) return r;
    if (d->state == NFP_DS_UNAVAILABLE) return NFP_R_NFC_DISABLED;
    nfp_poll(d);
    if (d->state == NFP_DS_INIT) return NFP_R_WRONG_DEVICE_STATE;
    d->state = NFP_DS_INIT;
    d->mounted = d->dirty = d->area_open = 0; /* unflushed changes are dropped */
    return 0;
}
uint32_t nfp_mount(NfpDevice *d, uint32_t model_type, uint32_t target) {
    (void)model_type;
    (void)target;
    uint32_t r = need_init(d);
    if (r) return r;
    nfp_poll(d);
    if (d->state != NFP_DS_TAG_FOUND) return wrong_state(d);
    if (!d->tag.is_amiibo) return NFP_R_NOT_AN_AMIIBO;
    d->m = d->tag.amiibo;
    d->mounted = 1;
    d->dirty = d->area_open = 0;
    d->state = NFP_DS_MOUNTED;
    return 0;
}
uint32_t nfp_unmount(NfpDevice *d) {
    uint32_t r = need_init(d);
    if (r) return r;
    nfp_poll(d);
    if (d->state != NFP_DS_MOUNTED) return wrong_state(d);
    d->mounted = d->dirty = d->area_open = 0;
    d->state = NFP_DS_TAG_FOUND;
    return 0;
}
uint32_t nfp_flush(NfpDevice *d) {
    uint32_t r = need_init(d);
    if (r) return r;
    nfp_poll(d);
    if (d->state != NFP_DS_MOUNTED) return wrong_state(d);
    if (!d->dirty) return 0;
    Amiibo n = d->m;
    n.write_counter++;
    n.write_date = d->today();
    NfcTag t = d->tag;
    t.amiibo = n;
    if (d->src.store && d->src.store(d->src.ctx, &t)) return NFP_R_UNABLE_TO_ACCESS_BACKUP;
    d->m = n;
    d->tag = t;
    d->dirty = 0;
    return 0;
}
uint32_t nfp_restore(NfpDevice *d) {
    uint32_t r = need_init(d);
    if (r) return r;
    nfp_poll(d);
    if (d->state != NFP_DS_TAG_FOUND && d->state != NFP_DS_MOUNTED) return wrong_state(d);
    /* the source is the backup: re-read the persisted amiibo and drop what was not flushed */
    NfcTag t;
    uint64_t gen;
    if (d->src.poll && d->src.poll(d->src.ctx, &t, &gen) == 1) {
        d->tag = t;
        if (d->mounted && t.is_amiibo) d->m = t.amiibo;
    }
    d->dirty = 0;
    return 0;
}

/* ---- structures -------------------------------------------------------------------------------------------------------------------- */
static void put16(uint8_t *p, unsigned v) { memcpy(p, &(uint16_t){(uint16_t)v}, 2); } /* little endian host: the structures are little endian */
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
void nfp_build_tag_info(const NfcTag *t, uint8_t o[NFP_TAG_INFO_SIZE]) {
    memset(o, 0, NFP_TAG_INFO_SIZE);
    memcpy(o, t->uid, 7);
    o[10] = 7;          /* uuid_length */
    put32(o + 0x20, 1); /* protocol: type A */
    put32(o + 0x24, 2); /* tag type: type 2 (NTAG215) */
}
void nfp_build_common_info(const Amiibo *a, uint8_t o[NFP_COMMON_INFO_SIZE]) {
    memset(o, 0, NFP_COMMON_INFO_SIZE);
    put16(o, a->write_date.year);
    o[2] = a->write_date.month;
    o[3] = a->write_date.day;
    put16(o + 4, a->write_counter);
    put16(o + 6, a->version);
    put32(o + 8, AMIIBO_APP_AREA_SIZE);
}
void nfp_build_model_info(const Amiibo *a, uint8_t o[NFP_MODEL_INFO_SIZE]) {
    memset(o, 0, NFP_MODEL_INFO_SIZE);
    memcpy(o, a->id, 8);
}
void nfp_build_register_info(const Amiibo *a, uint8_t o[NFP_REGISTER_INFO_SIZE]) {
    memset(o, 0, NFP_REGISTER_INFO_SIZE);
    memcpy(o, a->owner_mii, AMIIBO_MII_CHARINFO);
    put16(o + 0x58, a->register_date.year);
    o[0x5a] = a->register_date.month;
    o[0x5b] = a->register_date.day;
    memcpy(o + 0x5c, a->nickname, sizeof a->nickname < 0x29 ? sizeof a->nickname : 0x29);
    o[0x85] = a->font_region;
}

uint32_t nfp_get_tag_info(NfpDevice *d, uint8_t out[NFP_TAG_INFO_SIZE]) {
    uint32_t r = need_init(d);
    if (r) return r;
    nfp_poll(d);
    if (d->state != NFP_DS_TAG_FOUND && d->state != NFP_DS_MOUNTED) return wrong_state(d);
    nfp_build_tag_info(&d->tag, out);
    return 0;
}
static uint32_t need_mounted(NfpDevice *d) {
    uint32_t r = need_init(d);
    if (r) return r;
    nfp_poll(d);
    return d->state == NFP_DS_MOUNTED ? 0 : wrong_state(d);
}
uint32_t nfp_get_common_info(NfpDevice *d, uint8_t out[NFP_COMMON_INFO_SIZE]) {
    uint32_t r = need_mounted(d);
    if (!r) nfp_build_common_info(&d->m, out);
    return r;
}
uint32_t nfp_get_model_info(NfpDevice *d, uint8_t out[NFP_MODEL_INFO_SIZE]) {
    uint32_t r = need_mounted(d);
    if (!r) nfp_build_model_info(&d->m, out);
    return r;
}
uint32_t nfp_get_register_info(NfpDevice *d, uint8_t out[NFP_REGISTER_INFO_SIZE]) {
    uint32_t r = need_mounted(d);
    if (r) return r;
    if (!d->m.registered) return NFP_R_REGISTRATION_NOT_INITIALIZED;
    nfp_build_register_info(&d->m, out);
    return 0;
}

uint32_t nfp_area_exists(NfpDevice *d, int *exists) {
    uint32_t r = need_mounted(d);
    if (!r) *exists = d->m.has_app_area;
    return r;
}
uint32_t nfp_area_open(NfpDevice *d, uint32_t access_id) {
    uint32_t r = need_mounted(d);
    if (r) return r;
    if (!d->m.has_app_area) return NFP_R_APP_AREA_NOT_INITIALIZED;
    if (d->m.app_access_id != access_id) return NFP_R_WRONG_APP_AREA_ID;
    d->area_open = 1;
    return 0;
}
uint32_t nfp_area_size(NfpDevice *d, uint32_t *size) {
    uint32_t r = need_mounted(d);
    if (r) return r;
    if (!d->m.has_app_area) return NFP_R_APP_AREA_NOT_INITIALIZED;
    *size = AMIIBO_APP_AREA_SIZE;
    return 0;
}
uint32_t nfp_area_read(NfpDevice *d, void *out, size_t cap, size_t *n) {
    uint32_t r = need_mounted(d);
    if (r) return r;
    if (!d->m.has_app_area || !d->area_open) return NFP_R_APP_AREA_NOT_INITIALIZED;
    *n = cap < AMIIBO_APP_AREA_SIZE ? cap : AMIIBO_APP_AREA_SIZE;
    memcpy(out, d->m.app_data, *n);
    return 0;
}
uint32_t nfp_area_write(NfpDevice *d, const void *data, size_t n) {
    uint32_t r = need_mounted(d);
    if (r) return r;
    if (!d->m.has_app_area || !d->area_open) return NFP_R_APP_AREA_NOT_INITIALIZED;
    if (n > AMIIBO_APP_AREA_SIZE) return NFP_R_WRONG_APP_AREA_SIZE;
    memcpy(d->m.app_data, data, n);
    d->m.app_write_counter++;
    d->dirty = 1;
    return 0;
}
uint32_t nfp_area_create(NfpDevice *d, uint32_t access_id, const void *data, size_t n) {
    uint32_t r = need_mounted(d);
    if (r) return r;
    int c = amiibo_area_create(&d->m, access_id, d->app_id, data, n, 0);
    if (c == -1) return NFP_R_APP_AREA_EXISTS;
    if (c == -2) return NFP_R_WRONG_APP_AREA_SIZE;
    d->area_open = 1;
    d->dirty = 1;
    return 0;
}
uint32_t nfp_area_recreate(NfpDevice *d, uint32_t access_id, const void *data, size_t n) {
    uint32_t r = need_mounted(d);
    if (r) return r;
    if (n > AMIIBO_APP_AREA_SIZE) return NFP_R_WRONG_APP_AREA_SIZE;
    amiibo_area_delete(&d->m);
    amiibo_area_create(&d->m, access_id, d->app_id, data, n, 0);
    d->area_open = 1;
    d->dirty = 1;
    return 0;
}
uint32_t nfp_area_delete(NfpDevice *d) {
    uint32_t r = need_mounted(d);
    if (r) return r;
    if (!d->m.has_app_area) return NFP_R_APP_AREA_NOT_INITIALIZED;
    amiibo_area_delete(&d->m);
    d->area_open = 0;
    d->dirty = 1;
    return 0;
}
uint32_t nfp_set_nickname_owner(NfpDevice *d, const char *nick, const uint8_t mii[AMIIBO_MII_CHARINFO]) {
    uint32_t r = need_mounted(d);
    if (r) return r;
    amiibo_set_register(&d->m, nick, mii, d->today());
    d->dirty = 1;
    return 0;
}
uint32_t nfp_format(NfpDevice *d) {
    uint32_t r = need_mounted(d);
    if (r) return r;
    amiibo_format(&d->m);
    d->area_open = 0;
    d->dirty = 1;
    return 0;
}

/* ---- type 2 pass-through ---------------------------------------------------------------------------------------------------------- */
static void page_read(const NfcTag *t, unsigned page, uint8_t out[4]) {
    memset(out, 0, 4);
    if (page >= 135) return;
    if (t->has_image && page * 4 + 4 <= AMIIBO_TAG_SIZE) {
        memcpy(out, t->image + page * 4, 4);
        return;
    }
    switch (page) { /* a tag without an image: the UID pages and the capability container of an NTAG215 */
        case 0: out[0] = t->uid[0], out[1] = t->uid[1], out[2] = t->uid[2], out[3] = 0x88 ^ t->uid[0] ^ t->uid[1] ^ t->uid[2]; break;
        case 1: memcpy(out, t->uid + 3, 4); break;
        case 2: out[0] = t->uid[3] ^ t->uid[4] ^ t->uid[5] ^ t->uid[6]; break;
        case 3: out[0] = 0xe1, out[1] = 0x10, out[2] = 0x3e; break;
        default: break;
    }
}
long nfc_passthrough(NfpDevice *d, const uint8_t *cmd, size_t n, uint8_t *out, size_t cap) {
    nfp_poll(d);
    if (!d->initialized || (d->state != NFP_DS_TAG_FOUND && d->state != NFP_DS_MOUNTED) || n < 1) return -1;
    if (cmd[0] == 0x60) {
        static const uint8_t ver[8] = {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03};
        if (cap < 8) return -1;
        memcpy(out, ver, 8);
        return 8;
    }
    if (cmd[0] == 0x30 && n >= 2) {
        if (cap < 16) return -1;
        for (unsigned i = 0; i < 4; i++) page_read(&d->tag, (cmd[1] + i) % 135, out + 4 * i);
        return 16;
    }
    if (cmd[0] == 0x3a && n >= 3 && cmd[2] >= cmd[1]) {
        size_t pages = (size_t)cmd[2] - cmd[1] + 1;
        if (pages * 4 > cap) return -1;
        for (size_t i = 0; i < pages; i++) page_read(&d->tag, cmd[1] + (unsigned)i, out + 4 * i);
        return (long)(pages * 4);
    }
    return -1;
}
