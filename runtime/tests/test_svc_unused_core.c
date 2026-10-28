/* Unit tests for core functions nothing else called: mii_db_set_storage, mii_new_create_id, time_info_read, bsd_lookup, irs_core_access_done, nfp_build_* , ngc_words_clear, amiibo_format. */
#include "../src/services/amiibo_core.h"
#include "../src/services/irs_core.h"
#include "../src/services/mii_core.h"
#include "../src/services/nfp_core.h"
#include "../src/services/ngc_core.h"
#include "../src/services/offline_core.h"
#include "../src/services/time_core.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

/* an in-memory MiiStorage that records what the database does to it */
typedef struct { uint8_t buf[0x10000]; size_t n; int have, writes, reads, locks, unlocks, destroys; uint64_t stamp; } MemStore;
static int ms_read(void *c, int which, uint8_t *buf, size_t cap, size_t *n) {
    MemStore *m = c; m->reads++;
    if (which != 0 || !m->have) return 1;
    if (m->n > cap) return 2;
    memcpy(buf, m->buf, m->n); *n = m->n; return 0;
}
static int ms_write(void *c, const uint8_t *buf, size_t n) { MemStore *m = c; if (n > sizeof m->buf) return 1; memcpy(m->buf, buf, n); m->n = n; m->have = 1; m->writes++; m->stamp++; return 0; }
static void ms_lock(void *c) { ((MemStore *)c)->locks++; }
static void ms_unlock(void *c) { ((MemStore *)c)->unlocks++; }
static uint64_t ms_stamp(void *c) { return ((MemStore *)c)->stamp; }
static int ms_destroy(void *c) { MemStore *m = c; m->have = 0; m->n = 0; m->destroys++; return 0; }

static int g_grants;
static int src_request(void *ctx, void (*done)(void *, int), void *cb) { (void)ctx; (void)done; (void)cb; g_grants++; return IR_ACCESS_PENDING; }

int main(void) {
    { /* mii_new_create_id: RFC 4122 version 4, different every time */
        uint8_t a[16], b[16]; mii_new_create_id(a); mii_new_create_id(b);
        CHECK((a[6] >> 4) == 4 && (a[8] >> 6) == 2 && (b[6] >> 4) == 4 && (b[8] >> 6) == 2, "UUID version 4, variant 1");
        CHECK(memcmp(a, b, 16) != 0, "two ids differ");
        uint8_t z[16] = {0}; CHECK(memcmp(a, z, 16) != 0, "not all zero");
    }
    { /* mii_db_set_storage: a provider is loaded from, written to under its lock, and re-read after a restart; NULL means memory only */
        static MemStore ms; MiiStorage st = {&ms, ms_read, ms_write, ms_lock, ms_unlock, ms_stamp, ms_destroy};
        mii_db_set_storage(&st); mii_db_format();
        CHECK(ms.writes >= 1 && ms.locks >= 1 && ms.locks == ms.unlocks, "format wrote the provider under its lock");
        MiiCharInfo ci; mii_build_random(0, 0, 0, &ci); MiiStoreData sd; mii_store_from_char_info(&ci, &sd);
        int w0 = ms.writes; CHECK(mii_db_add_or_replace(&sd) >= 0 && ms.writes > w0 && mii_db_count() == 1, "adding a Mii rewrites the provider");
        mii_db_set_storage(&st);
        CHECK(mii_db_count() == 1 && mii_db_find(sd.create_id) == 0, "the database reloads from the provider (a restart)");
        mii_db_set_storage(NULL); mii_db_set_storage(&st);
        CHECK(mii_db_find(sd.create_id) == 0, "the provider's contents survive a detour through memory-only");
        CHECK(mii_db_destroy_file() == 0 && ms.destroys == 1 && !ms.have, "destroy goes to the provider");
        mii_db_set_storage(NULL);
    }
    { /* time_info_read is the inverse of time_info_write */
        TimeCalendarInfo i = {3, 200, "JST", 1, 32400}, o; uint8_t w[0x18]; memset(&o, 0x77, sizeof o);
        time_info_write(w, &i); time_info_read(w, &o);
        CHECK(o.wday == 3 && o.yday == 200 && !strcmp(o.zone, "JST") && o.is_dst == 1 && o.gmt_offset == 32400, "calendar info round-trips");
        uint8_t raw[0x18] = {0}; raw[0] = 6; raw[4] = 0x6c; raw[5] = 1; memcpy(raw + 8, "UTC", 4); raw[20] = 0xf0; raw[21] = 0xf1; raw[22] = 0xff; raw[23] = 0xff;
        time_info_read(raw, &o); CHECK(o.wday == 6 && o.yday == 364 && !strcmp(o.zone, "UTC") && o.is_dst == 0 && o.gmt_offset == -3600, "wire layout: wday, yday, zone[8], dst, signed offset");
    }
    { /* bsd_lookup: only open descriptors resolve */
        BsdTable t; bsd_init(&t); int err = 0;
        CHECK(bsd_lookup(&t, 0) == NULL && bsd_lookup(&t, -1) == NULL && bsd_lookup(&t, BSD_MAX_FDS) == NULL, "empty table, out-of-range ids");
        int fd = bsd_socket(&t, BSD_AF_INET, BSD_SOCK_STREAM, 0, &err);
        CHECK(fd >= 0 && bsd_lookup(&t, fd) != NULL && bsd_lookup(&t, fd) == &t.fds[fd], "an open socket resolves");
        CHECK(bsd_close(&t, fd, &err) == 0 && bsd_lookup(&t, fd) == NULL, "closed: not found");
    }
    { /* irs_core_access_done: the asynchronous answer of the source's permission prompt */
        IrsCore c; irs_core_init(&c); IrFrameSource s; memset(&s, 0, sizeof s); s.request_access = src_request;
        irs_core_set_source(&c, &s);
        CHECK(c.access == IR_ACCESS_PENDING, "access starts pending");
        irs_core_access_done(&c, 1); CHECK(c.access == IR_ACCESS_GRANTED, "granted");
        irs_core_access_done(&c, 0); CHECK(c.access == IR_ACCESS_DENIED, "denied");
        irs_core_access_done(&c, 5); CHECK(c.access == IR_ACCESS_GRANTED, "any nonzero is a grant");
        irs_core_free(&c);
    }
    { /* ngc_words_clear */
        static NgcWords w; unsigned n = ngc_words_load_text(&w, "bad\n#c\nworse\n", 13);
        uint16_t t[] = {'a', 'B', 'a', 'D'};
        CHECK(n == 2 && w.count == 2 && ngc_check(&w, t, 4) == 1, "two words loaded, one match");
        uint32_t ver = w.version; ngc_words_clear(&w);
        CHECK(w.count == 0 && ngc_check(&w, t, 4) == 0 && ngc_mask(&w, t, 4) == 0 && t[1] == 'B', "cleared: nothing matches or masks");
        (void)ver;
        CHECK(ngc_words_load_text(&w, "ad\n", 3) == 1 && ngc_check(&w, t, 4) == 1, "a cleared list loads again");
    }
    { /* amiibo_format + nfp_build_* */
        static Amiibo a; uint8_t id[8] = {1, 2, 3, 4, 5, 6, 7, 2}; amiibo_new(&a, id, 42);
        uint8_t mii[AMIIBO_MII_CHARINFO]; for (unsigned i = 0; i < sizeof mii; i++) mii[i] = (uint8_t)(i + 1);
        amiibo_set_register(&a, "Link", mii, (AmiiboDate){2026, 10, 27});
        CHECK(amiibo_area_create(&a, 0x1234, 0x0100aabbull, "xy", 2, 0) == 0, "area created");
        a.write_date = (AmiiboDate){2026, 9, 30}; a.write_counter = 7; a.version = 3; a.font_region = 2;
        uint8_t o[0x100];
        nfp_build_model_info(&a, o); CHECK(!memcmp(o, id, 8) && o[8] == 0 && o[0x3f] == 0, "model info: the 8-byte id first, rest zero");
        nfp_build_common_info(&a, o);
        CHECK(o[0] == (2026 & 0xff) && o[1] == (2026 >> 8) && o[2] == 9 && o[3] == 30 && o[4] == 7 && o[6] == 3 && o[8] == AMIIBO_APP_AREA_SIZE && o[9] == 0, "common info: write date, counter, version, app area size");
        nfp_build_register_info(&a, o);
        CHECK(!memcmp(o, mii, sizeof mii) && o[0x58] == (2026 & 0xff) && o[0x5a] == 10 && o[0x5b] == 27 && !strcmp((char *)o + 0x5c, "Link") && o[0x85] == 2, "register info: Mii, register date, nickname, font region");
        NfcTag tag; memset(&tag, 0, sizeof tag); for (int i = 0; i < 7; i++) tag.uid[i] = (uint8_t)(0x10 + i);
        uint8_t ti[NFP_TAG_INFO_SIZE]; memset(ti, 0xff, sizeof ti); nfp_build_tag_info(&tag, ti);
        CHECK(ti[0] == 0x10 && ti[6] == 0x16 && ti[10] == 7 && ti[0x20] == 1 && ti[0x24] == 2 && ti[0x57] == 0, "tag info: uid, length 7, type A, type 2, zero padded");
        amiibo_format(&a);
        CHECK(!a.registered && a.nickname[0] == 0 && !a.has_app_area && a.register_date.year == 0 && !memcmp(a.id, id, 8) && a.uid[0] == 0x04, "format: unregistered, no area, id and uid kept");
        uint8_t z[AMIIBO_MII_CHARINFO] = {0}; CHECK(!memcmp(a.owner_mii, z, sizeof z), "owner Mii cleared");
        nfp_build_register_info(&a, o); CHECK(o[0x5c] == 0 && o[0x58] == 0, "register info of a formatted amiibo is empty");
    }
    printf("{\"test\":\"svc_unused_core\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
