/* Unit tests: the nfp / nfc device state machine (nfp_core.c) and the nfp:user / nfc:user / nfc:mf:u services over a picker tag source. Synthetic amiibo only. */
#include "../src/services/offline.c"
#include "svc_test.h"
#include <sys/stat.h>
#include <unistd.h>

static uint8_t ID_A[8] = {0x01, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x02}, ID_B[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x01, 0x02};
static AmiiboDate fixed_today(void) { return (AmiiboDate){2026, 10, 26}; }
static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static NfcTag *rt_ptr;
static int rt_present(void *c, NfcTag *o, uint64_t *g) { (void)c; *o = *rt_ptr; *g = 1; return 1; }
static void core_tests(const char *root) {
    AmiiboConfig cfg; amiibo_config_default(&cfg, root); snprintf(cfg.store, sizeof cfg.store, "%s/core-store", root);
    AmiiboPicker pk; amiibo_picker_init(&pk, &cfg);
    NfpDevice d; nfp_device_init(&d); d.today = fixed_today;
    uint8_t buf[0x100]; int ex;
    CHECK(nfp_start_detection(&d) == NFP_R_WRONG_DEVICE_STATE && nfp_mount(&d, 0, 0) == NFP_R_WRONG_DEVICE_STATE && nfp_get_tag_info(&d, buf) == NFP_R_WRONG_DEVICE_STATE, "before Initialize every call fails with the wrong-state result");
    CHECK(nfp_state(&d) == 0 && nfp_initialize(&d, 0x0100000000001000ull) == 0 && nfp_state(&d) == 1 && nfp_initialize(&d, 1) == NFP_R_WRONG_DEVICE_STATE, "Initialize once");
    CHECK(nfp_device_state(&d) == NFP_DS_UNAVAILABLE && nfp_start_detection(&d) == NFP_R_NFC_DISABLED, "no reader: Unavailable, detection reports NFC disabled");
    nfp_finalize(&d); nfp_device_init(&d); d.today = fixed_today; nfp_set_source(&d, amiibo_picker_source(&pk)); nfp_initialize(&d, 0x0100000000001000ull);
    CHECK(nfp_device_state(&d) == NFP_DS_INIT, "with a reader: Initialized");
    CHECK(nfp_stop_detection(&d) == NFP_R_WRONG_DEVICE_STATE, "StopDetection when not detecting");
    CHECK(nfp_start_detection(&d) == 0 && nfp_device_state(&d) == NFP_DS_SEARCHING && nfp_take_events(&d) == 0, "StartDetection: searching, no event yet");
    CHECK(nfp_start_detection(&d) == NFP_R_WRONG_DEVICE_STATE, "StartDetection while searching");
    CHECK(nfp_get_tag_info(&d, buf) == NFP_R_WRONG_DEVICE_STATE && nfp_mount(&d, 0, 0) == NFP_R_WRONG_DEVICE_STATE, "no tag yet: tag calls fail");
    amiibo_picker_select(&pk, ID_A, AMIIBO_PICK_REUSE); nfp_poll(&d);
    CHECK(d.state == NFP_DS_TAG_FOUND && nfp_take_events(&d) == NFP_EV_ACTIVATE, "a tag arrives: found, activate event");
    CHECK(nfp_get_tag_info(&d, buf) == 0 && !memcmp(buf, d.tag.uid, 7) && buf[10] == 7 && u32(buf + 0x20) == 1 && u32(buf + 0x24) == 2 && !buf[0x30] , "TagInfo: UID, length 7, type A, type 2");
    CHECK(nfp_get_common_info(&d, buf) == NFP_R_WRONG_DEVICE_STATE, "CommonInfo needs the tag mounted");
    CHECK(nfp_mount(&d, 0, 3) == 0 && d.state == NFP_DS_MOUNTED, "Mount");
    CHECK(nfp_get_model_info(&d, buf) == 0 && !memcmp(buf, ID_A, 8) && !buf[8], "ModelInfo carries the model id");
    CHECK(nfp_get_common_info(&d, buf) == 0 && u16(buf + 4) == 0 && u32(buf + 8) == 216, "CommonInfo: write counter 0, application area size 216");
    CHECK(nfp_get_register_info(&d, buf) == NFP_R_REGISTRATION_NOT_INITIALIZED, "RegisterInfo before registration");
    uint8_t mii[AMIIBO_MII_CHARINFO]; for (int i = 0; i < 0x58; i++) mii[i] = (uint8_t)(i ^ 0x5a);
    CHECK(nfp_set_nickname_owner(&d, "Hero", mii) == 0 && nfp_get_register_info(&d, buf) == 0 && !memcmp(buf, mii, 0x58) && u16(buf + 0x58) == 2026 && buf[0x5a] == 10 && buf[0x5b] == 26 && !strcmp((char *)buf + 0x5c, "Hero"), "SetNicknameAndOwner then RegisterInfo: Mii, date, nickname");
    CHECK(nfp_area_exists(&d, &ex) == 0 && ex == 0, "no application area yet");
    uint32_t sz = 0; CHECK(nfp_area_open(&d, 0x1234) == NFP_R_APP_AREA_NOT_INITIALIZED && nfp_area_size(&d, &sz) == NFP_R_APP_AREA_NOT_INITIALIZED, "Open / size without an area");
    uint8_t save[64]; for (int i = 0; i < 64; i++) save[i] = (uint8_t)(i * 3);
    CHECK(nfp_area_create(&d, 0x1234, save, 300) == NFP_R_WRONG_APP_AREA_SIZE, "Create with more than 216 bytes");
    CHECK(nfp_area_create(&d, 0x1234, save, 64) == 0 && nfp_area_create(&d, 0x1234, save, 64) == NFP_R_APP_AREA_EXISTS, "Create once");
    uint8_t rd[216]; size_t n = 0; CHECK(nfp_area_read(&d, rd, sizeof rd, &n) == 0 && n == 216 && !memcmp(rd, save, 64) && rd[64] == 0, "the created area is open and reads back, padded");
    CHECK(nfp_area_size(&d, &sz) == 0 && sz == 216, "area size");
    uint8_t upd[8] = {9, 9, 9, 9, 9, 9, 9, 9}; CHECK(nfp_area_write(&d, upd, 8) == 0 && nfp_area_read(&d, rd, 16, &n) == 0 && n == 16 && rd[0] == 9 && rd[8] == 8 * 3, "write updates the head, the rest stays");
    CHECK(nfp_area_write(&d, upd, 217) == NFP_R_WRONG_APP_AREA_SIZE, "oversize write");
    CHECK(nfp_unmount(&d) == 0 && d.state == NFP_DS_TAG_FOUND && nfp_area_read(&d, rd, 8, &n) == NFP_R_WRONG_DEVICE_STATE, "Unmount drops back to tag found");
    CHECK(nfp_mount(&d, 0, 3) == 0 && nfp_get_register_info(&d, buf) == NFP_R_REGISTRATION_NOT_INITIALIZED && nfp_area_exists(&d, &ex) == 0 && ex == 0, "unflushed changes were dropped by Unmount");
    nfp_set_nickname_owner(&d, "Hero", mii); nfp_area_create(&d, 0x1234, save, 64);
    CHECK(nfp_flush(&d) == 0, "Flush"); { Amiibo st; CHECK(amiibo_store_load(cfg.store, d.tag.uid, &st) == 0 && st.registered && st.has_app_area && st.write_counter == 1 && st.write_date.year == 2026 && st.write_date.day == 26 && !memcmp(st.app_data, save, 64), "the store holds the flushed amiibo: registered, area, write counter 1, today"); }
    CHECK(nfp_area_open(&d, 0x9999) == NFP_R_WRONG_APP_AREA_ID, "Open with the wrong access id");
    nfp_unmount(&d); nfp_mount(&d, 0, 3); CHECK(nfp_area_read(&d, rd, 8, &n) == NFP_R_APP_AREA_NOT_INITIALIZED, "after a fresh Mount the area is closed until opened");
    CHECK(nfp_area_open(&d, 0x1234) == 0 && nfp_area_read(&d, rd, 8, &n) == 0 && rd[1] == 3, "Open with the right access id");
    CHECK(nfp_flush(&d) == 0 && nfp_get_common_info(&d, buf) == 0 && u16(buf + 4) == 1, "Flush without changes writes nothing (counter stays 1)");
    nfp_area_write(&d, upd, 8); CHECK(nfp_flush(&d) == 0 && nfp_get_common_info(&d, buf) == 0 && u16(buf + 4) == 2, "a change and a Flush bump the counter");
    nfp_area_write(&d, save, 8); /* not flushed */
    amiibo_picker_remove(&pk); nfp_poll(&d);
    CHECK(d.state == NFP_DS_TAG_REMOVED && nfp_take_events(&d) == NFP_EV_DEACTIVATE, "removal: tag removed, deactivate event");
    CHECK(nfp_get_tag_info(&d, buf) == NFP_R_TAG_REMOVED && nfp_mount(&d, 0, 3) == NFP_R_TAG_REMOVED && nfp_area_read(&d, rd, 8, &n) == NFP_R_TAG_REMOVED && nfp_flush(&d) == NFP_R_TAG_REMOVED, "after removal the calls report TagRemoved");
    CHECK(nfp_stop_detection(&d) == 0 && d.state == NFP_DS_INIT, "StopDetection after removal");
    amiibo_picker_select(&pk, ID_A, AMIIBO_PICK_REUSE); nfp_poll(&d); CHECK(d.state == NFP_DS_INIT && nfp_take_events(&d) == 0, "a tag placed while not detecting raises nothing");
    CHECK(nfp_start_detection(&d) == 0 && d.state == NFP_DS_TAG_FOUND && nfp_take_events(&d) == NFP_EV_ACTIVATE, "StartDetection finds the tag already on the reader");
    nfp_mount(&d, 0, 3); nfp_area_open(&d, 0x1234); nfp_area_read(&d, rd, 8, &n); CHECK(rd[0] == 9, "the unflushed write from before the removal did not reach the tag");
    amiibo_picker_remove(&pk); nfp_poll(&d); nfp_take_events(&d);
    CHECK(nfp_start_detection(&d) == 0 && d.state == NFP_DS_SEARCHING, "StartDetection is allowed again after removal (no stop needed)");
    amiibo_picker_select(&pk, ID_B, AMIIBO_PICK_REUSE); nfp_poll(&d); CHECK(d.state == NFP_DS_TAG_FOUND, "another amiibo is found"); nfp_take_events(&d);
    nfp_mount(&d, 0, 3); CHECK(nfp_get_model_info(&d, buf) == 0 && !memcmp(buf, ID_B, 8), "mounting the second amiibo shows its model");
    amiibo_picker_select(&pk, ID_A, AMIIBO_PICK_REUSE); nfp_poll(&d); CHECK(d.state == NFP_DS_TAG_REMOVED && (nfp_take_events(&d) & NFP_EV_DEACTIVATE), "swapping the tag on a mounted reader counts as a removal");
    nfp_start_detection(&d); nfp_mount(&d, 0, 3);
    CHECK(nfp_format(&d) == 0 && nfp_get_register_info(&d, buf) == NFP_R_REGISTRATION_NOT_INITIALIZED && nfp_area_exists(&d, &ex) == 0 && ex == 0, "Format clears the registration and the area");
    CHECK(nfp_restore(&d) == 0 && nfp_get_register_info(&d, buf) == 0, "Restore re-reads what was last flushed (the registered state)");
    nfp_area_delete(&d); nfp_area_open(&d, 0); nfp_flush(&d);
    nfp_area_create(&d, 7, "ab", 2); CHECK(nfp_area_recreate(&d, 8, "xy", 2) == 0 && nfp_area_open(&d, 8) == 0 && nfp_area_open(&d, 7) == NFP_R_WRONG_APP_AREA_ID, "Recreate replaces the area and its access id");
    CHECK(nfp_area_delete(&d) == 0 && nfp_area_exists(&d, &ex) == 0 && ex == 0 && nfp_area_delete(&d) == NFP_R_APP_AREA_NOT_INITIALIZED, "Delete");
    /* a tag that is not an amiibo */
    nfp_stop_detection(&d); { NfcTagSource raw; static NfcTag rt; memset(&rt, 0, sizeof rt); rt.uid[0] = 4; rt.has_image = 1; for (int i = 0; i < AMIIBO_TAG_SIZE; i++) rt.image[i] = (uint8_t)i;
      rt_ptr = &rt; raw = (NfcTagSource){.ctx = NULL, .name = "raw", .poll = rt_present};
      NfpDevice rd2; nfp_device_init(&rd2); nfp_set_source(&rd2, &raw); nfp_initialize(&rd2, 1); nfp_start_detection(&rd2);
      CHECK(rd2.state == NFP_DS_TAG_FOUND && nfp_get_tag_info(&rd2, buf) == 0, "a plain tag is found and has TagInfo"); CHECK(nfp_mount(&rd2, 0, 3) == NFP_R_NOT_AN_AMIIBO, "but it cannot be mounted as an amiibo");
      uint8_t out[64]; uint8_t rdc[2] = {0x30, 4}; CHECK(nfc_passthrough(&rd2, rdc, 2, out, sizeof out) == 16 && out[0] == 16 && out[15] == 31, "pass-through READ returns the tag image pages");
      uint8_t fr[3] = {0x3a, 2, 3}; CHECK(nfc_passthrough(&rd2, fr, 3, out, sizeof out) == 8 && out[0] == 8, "FAST_READ");
      uint8_t gv = 0x60; CHECK(nfc_passthrough(&rd2, &gv, 1, out, sizeof out) == 8 && out[1] == 4 && out[6] == 0x11, "GET_VERSION reports an NTAG215");
      uint8_t bad = 0xa2; CHECK(nfc_passthrough(&rd2, &bad, 1, out, sizeof out) == -1, "WRITE is not supported");
      nfp_finalize(&rd2); }
    { /* a virtual amiibo without an image synthesises the UID pages and the capability container */
      NfpDevice vd; nfp_device_init(&vd); nfp_set_source(&vd, amiibo_picker_source(&pk)); nfp_initialize(&vd, 1); amiibo_picker_select(&pk, ID_A, AMIIBO_PICK_REUSE); nfp_start_detection(&vd);
      uint8_t out[16], rd0[2] = {0x30, 0}; CHECK(nfc_passthrough(&vd, rd0, 2, out, 16) == 16 && out[0] == vd.tag.uid[0] && out[3] == (0x88 ^ vd.tag.uid[0] ^ vd.tag.uid[1] ^ vd.tag.uid[2]) && out[12] == 0xe1 && out[13] == 0x10 && out[14] == 0x3e, "READ of pages 0..3 on a virtual tag: UID, check bytes, capability container"); }
    amiibo_picker_free(&pk);
}

static uint32_t ev_state(uint32_t h) { return event_is_signaled(h); }
static void service_tests(const char *root) {
    AmiiboConfig cfg; amiibo_config_default(&cfg, root); snprintf(cfg.store, sizeof cfg.store, "%s/svc-store", root);
    IpcCall c; IpcObject *nfp = nfp_open(), *user;
    svc_call(nfp, 0, NULL, 0, &c); user = c.out_objects[0];
    /* no reader installed: the old stub answers */
    svc_call(user, 0, NULL, 0, &c); CHECK(c.result == 0, "Initialize");
    svc_call(user, 19, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 1, "GetState: initialized");
    svc_call(user, 2, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, int32_t, 0) == 0, "no reader: ListDevices reports no device");
    svc_call(user, 20, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == NFP_DS_UNAVAILABLE, "no reader: GetDeviceState Unavailable");
    svc_call(user, 3, NULL, 0, &c); CHECK(c.result == 0, "no reader: StartDetection accepted (as the stub did)"); svc_call(user, 5, NULL, 0, &c); CHECK(c.result == 0, "no reader: Mount accepted");
    svc_call(user, 23, NULL, 0, &c); CHECK(c.result == 0 && c.nout_copy == 1, "AttachAvailabilityChangeEvent hands out an event");
    svc_call(user, 1, NULL, 0, &c);
    /* install the picker */
    AmiiboPicker pk; amiibo_picker_init(&pk, &cfg); nfp_service_set_source(amiibo_picker_source(&pk));
    svc_call(nfp, 0, NULL, 0, &c); user = c.out_objects[0];
    svc_call(user, 19, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "GetState before Initialize: not initialized");
    svc_call(user, 0, NULL, 0, &c); svc_call(user, 0, NULL, 0, &c); CHECK(c.result == 0, "Initialize twice is accepted");
    uint8_t hbuf[16]; memset(&c, 0, sizeof c); svc_set_out_buf(&c, hbuf, sizeof hbuf); svc_call_c(user, 2, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 1, "ListDevices: one reader"); uint64_t handle; memcpy(&handle, hbuf, 8); CHECK(handle == NFP_DEVICE_HANDLE, "device handle written to the output buffer");
    svc_call(user, 17, NULL, 0, &c); uint32_t act = c.out_copy[0]; svc_call(user, 18, NULL, 0, &c); uint32_t deact = c.out_copy[0];
    CHECK(!ev_state(act) && !ev_state(deact), "events start clear");
    svc_call(user, 20, &handle, 8, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == NFP_DS_INIT, "GetDeviceState: initialized");
    uint64_t bad = 77; svc_call(user, 3, &bad, 8, &c); CHECK(c.result == NFP_R_DEVICE_NOT_FOUND, "a wrong device handle");
    svc_call(user, 3, &handle, 8, &c); CHECK(c.result == 0, "StartDetection"); svc_call(user, 20, &handle, 8, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == NFP_DS_SEARCHING, "searching");
    amiibo_picker_select(&pk, ID_A, AMIIBO_PICK_REUSE);
    CHECK(ev_state(act) && !ev_state(deact), "placing an amiibo signals the activate event (through the picker's listener)");
    uint8_t info[0x100]; memset(&c, 0, sizeof c); svc_set_out_buf(&c, info, sizeof info); c.in = (const uint8_t *)&handle; c.in_size = 8; svc_call_c(user, 13, &c); CHECK(c.result == 0 && info[10] == 7 && u32(info + 0x24) == 2, "GetTagInfo through the output buffer");
    { uint8_t mount[16]; memcpy(mount, &handle, 8); uint32_t mt[2] = {0, 3}; memcpy(mount + 8, mt, 8); svc_call(user, 5, mount, 16, &c); CHECK(c.result == 0, "Mount(handle, model type, target)"); }
    memset(info, 0, sizeof info); memset(&c, 0, sizeof c); svc_set_out_buf(&c, info, sizeof info); c.in = (const uint8_t *)&handle; c.in_size = 8; svc_call_c(user, 16, &c); CHECK(c.result == 0 && !memcmp(info, ID_A, 8), "GetModelInfo");
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, info, sizeof info); c.in = (const uint8_t *)&handle; c.in_size = 8; svc_call_c(user, 14, &c); CHECK(c.result == NFP_R_REGISTRATION_NOT_INITIALIZED, "GetRegisterInfo: unregistered");
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, info, sizeof info); c.in = (const uint8_t *)&handle; c.in_size = 8; svc_call_c(user, 15, &c); CHECK(c.result == 0 && u32(info + 8) == 216, "GetCommonInfo");
    { uint8_t oa[12]; memcpy(oa, &handle, 8); uint32_t id = 0x00abcdef; memcpy(oa + 8, &id, 4); svc_call(user, 7, oa, 12, &c); CHECK(c.result == NFP_R_APP_AREA_NOT_INITIALIZED, "OpenApplicationArea on a fresh amiibo");
      uint8_t data[32]; for (int i = 0; i < 32; i++) data[i] = (uint8_t)(i + 1); memset(&c, 0, sizeof c); svc_set_in_buf(&c, data, sizeof data); c.in = oa; c.in_size = 12; svc_call_c(user, 12, &c); CHECK(c.result == 0, "CreateApplicationArea");
      memset(&c, 0, sizeof c); c.in = oa; c.in_size = 12; svc_call_c(user, 12, &c); CHECK(c.result == NFP_R_APP_AREA_EXISTS, "Create again");
      svc_call(user, 22, &handle, 8, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 216, "GetApplicationAreaSize");
      uint8_t got[64] = {0}; memset(&c, 0, sizeof c); svc_set_out_buf(&c, got, sizeof got); c.in = (const uint8_t *)&handle; c.in_size = 8; svc_call_c(user, 8, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 64 && got[0] == 1 && got[31] == 32 && got[32] == 0, "GetApplicationArea fills the buffer (64 of 216 bytes)");
      uint8_t more[4] = {0xde, 0xad, 0xbe, 0xef}; memset(&c, 0, sizeof c); svc_set_in_buf(&c, more, 4); c.in = (const uint8_t *)&handle; c.in_size = 8; svc_call_c(user, 9, &c); CHECK(c.result == 0, "SetApplicationArea");
      svc_call(user, 10, &handle, 8, &c); CHECK(c.result == 0, "Flush"); Amiibo st; NfcTag cur; uint64_t g; amiibo_picker_source(&pk)->poll(&pk, &cur, &g); CHECK(amiibo_store_load(cfg.store, cur.uid, &st) == 0 && st.has_app_area && st.app_access_id == 0xabcdef && st.app_data[0] == 0xde && st.app_data[4] == 5, "the store has the area written through the service"); }
    amiibo_picker_remove(&pk); CHECK(ev_state(deact), "removing the amiibo signals the deactivate event");
    svc_call(user, 20, &handle, 8, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == NFP_DS_TAG_REMOVED, "device state: tag removed"); svc_call(user, 10, &handle, 8, &c); CHECK(c.result == NFP_R_TAG_REMOVED, "Flush after removal");
    svc_call(user, 21, &handle, 8, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 0, "GetNpadId");
    svc_call(user, 4, &handle, 8, &c); CHECK(c.result == 0, "StopDetection");
    /* nfc:user */
    { IpcObject *nfcr = nfc_open(); svc_call(nfcr, 0, NULL, 0, &c); IpcObject *nfc = c.out_objects[0];
      svc_call(nfc, 0, NULL, 0, &c); svc_call(nfc, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 1, "nfc GetState"); svc_call(nfc, 3, NULL, 0, &c); CHECK(c.out[0] == 1, "IsNfcEnabled with a reader");
      memset(&c, 0, sizeof c); svc_set_out_buf(&c, hbuf, sizeof hbuf); svc_call_c(nfc, 100, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 1, "nfc ListDevices"); svc_call(nfc, 107, NULL, 0, &c); uint32_t nact = c.out_copy[0];
      svc_call(nfc, 104, &handle, 8, &c); CHECK(c.result == 0, "nfc StartDetection"); amiibo_picker_select(&pk, ID_B, AMIIBO_PICK_REUSE); CHECK(ev_state(nact), "the nfc connection gets its own activate event");
      memset(&c, 0, sizeof c); svc_set_out_buf(&c, info, sizeof info); c.in = (const uint8_t *)&handle; c.in_size = 8; svc_call_c(nfc, 106, &c); CHECK(c.result == 0 && info[10] == 7, "nfc GetTagInfo");
      uint8_t cmd[2] = {0x30, 0}, resp[32]; memset(&c, 0, sizeof c); svc_set_in_buf(&c, cmd, 2); svc_set_out_buf(&c, resp, sizeof resp); c.in = (const uint8_t *)&handle; c.in_size = 8; svc_call_c(nfc, 400, &c);
      CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 16 && resp[12] == 0xe1, "SendCommandByPassThrough READ");
      uint8_t wr[2] = {0xa2, 0}; memset(&c, 0, sizeof c); svc_set_in_buf(&c, wr, 2); svc_set_out_buf(&c, resp, sizeof resp); c.in = (const uint8_t *)&handle; c.in_size = 8; svc_call_c(nfc, 400, &c); CHECK(c.result != 0, "an unsupported pass-through command fails");
      svc_call(nfc, 105, &handle, 8, &c);
      IpcObject *mf = mifare_open(); svc_call(mf, 0, NULL, 0, &c); IpcObject *mfu = c.out_objects[0]; svc_call(mfu, 0, NULL, 0, &c); svc_call(mfu, 5, NULL, 0, &c); CHECK(c.result != 0, "Mifare Read cannot be served by an NTAG215 amiibo tag");
      svc_call(mfu, 10, NULL, 0, &c); CHECK(c.result == 0, "mifare GetState"); }
    /* scripted selector: NAPPLE_AMIIBO */
    amiibo_picker_remove(&pk); nfp_service_set_source(NULL);
    svc_call(nfp, 0, NULL, 0, &c); IpcObject *u3 = c.out_objects[0]; svc_call(u3, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "source removed: no device again");
    amiibo_picker_free(&pk);
}
static void script_test(const char *root) {
    char store[300]; snprintf(store, sizeof store, "%s/script-store", root); setenv("NAPPLE_AMIIBO_STORE", store, 1); setenv("NAPPLE_AMIIBO", "0000000000090102", 1);
    nfp_env_checked = 0; nfp_have_src = 0; nfp_script_started = 0;
    IpcCall c; IpcObject *nfp = nfp_open(); svc_call(nfp, 0, NULL, 0, &c); IpcObject *user = c.out_objects[0];
    svc_call(user, 0, NULL, 0, &c); uint8_t hb[8]; memset(&c, 0, sizeof c); svc_set_out_buf(&c, hb, 8); svc_call_c(user, 2, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 1, "NAPPLE_AMIIBO installs the picker as the reader");
    uint64_t handle = NFP_DEVICE_HANDLE; svc_call(user, 17, NULL, 0, &c); uint32_t act = c.out_copy[0];
    svc_call(user, 3, &handle, 8, &c); CHECK(c.result == 0, "StartDetection runs the script (insert at once)"); CHECK(ev_state(act), "the scripted amiibo raises the activate event");
    svc_call(user, 20, &handle, 8, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == NFP_DS_TAG_FOUND, "tag found with no frontend involved");
    uint8_t info[0x100]; { uint8_t mount[16] = {0}; memcpy(mount, &handle, 8); svc_call(user, 5, mount, 16, &c); }
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, info, sizeof info); c.in = (const uint8_t *)&handle; c.in_size = 8; svc_call_c(user, 16, &c); CHECK(c.result == 0 && !memcmp(info, ID_B, 8), "the scripted model id is what the game sees");
    /* delayed insert and remove on the timer thread */
    nfp_service_set_source(NULL); amiibo_picker_remove(&nfp_picker); setenv("NAPPLE_AMIIBO", "0101000000040002,60,80", 1); nfp_env_checked = 0; nfp_have_src = 0; nfp_script_started = 0;
    svc_call(nfp, 0, NULL, 0, &c); IpcObject *u2 = c.out_objects[0]; svc_call(u2, 0, NULL, 0, &c); svc_call(u2, 17, NULL, 0, &c); uint32_t act2 = c.out_copy[0]; svc_call(u2, 18, NULL, 0, &c); uint32_t deact2 = c.out_copy[0];
    svc_call(u2, 3, &handle, 8, &c); CHECK(!ev_state(act2), "delayed script: nothing yet right after StartDetection");
    int got = 0; for (int i = 0; i < 100 && !got; i++) { usleep(10000); got = ev_state(act2); } CHECK(got, "the timer thread places the amiibo");
    got = 0; for (int i = 0; i < 100 && !got; i++) { usleep(10000); got = ev_state(deact2); } CHECK(got, "and removes it after remove_ms");
    unsetenv("NAPPLE_AMIIBO"); unsetenv("NAPPLE_AMIIBO_STORE");
}

int main(void) {
    char tmpl[] = "/tmp/napple-nfp-test-XXXXXX"; char *root = mkdtemp(tmpl); CHECK(root != NULL, "temp dir");
    core_tests(root); service_tests(root); script_test(root);
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf %s", root); (void)!system(cmd);
    printf("{\"test\":\"svc_nfp\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
