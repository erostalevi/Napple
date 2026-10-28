/* Unit tests: the offline services' state-backed rows (mm clocks, nifm request parameters, friend/nsd/prepo/ssl/nfc state) and the portable state core. */
#include "../src/services/offline.c"
#include "svc_test.h"

static void u32call(IpcObject *o, unsigned cmd, uint32_t a, uint32_t b, uint32_t d, IpcCall *c) { uint32_t in[3] = {a, b, d}; svc_call(o, cmd, in, sizeof in, c); }

int main(void) {
    /* ---- offline_state_core: mm */
    MmTable t; mm_init(&t);
    uint32_t r1 = mm_request_open(&t, 3), r2 = mm_request_open(&t, 4);
    CHECK(r1 == 1 && r2 == 2, "request ids count up from 1");
    mm_request_set(&t, r1, 100, 200); mm_request_set(&t, r2, 7, 8);
    CHECK(mm_request_get(&t, r1) == 100 && mm_request_get(&t, r2) == 7, "Get reads back what Set stored");
    mm_request_close(&t, r1); CHECK(mm_request_get(&t, r1) == 0 && mm_request_open(&t, 5) == r1, "closed ids are reused, and read 0");
    mm_request_set(&t, 99, 1, 1); mm_request_close(&t, 0); CHECK(mm_request_get(&t, 0) == 0, "unknown ids are ignored");
    for (int i = 0; i < MM_MAX_REQUESTS; i++) mm_request_open(&t, 1);
    CHECK(mm_request_open(&t, 1) == 0, "a full table answers 0");
    mm_module_open(&t, 2); mm_module_set(&t, 2, 55, 66); CHECK(mm_module_get(&t, 2) == 55 && mm_module_get(&t, 3) == 0 && mm_module_get(&t, 99) == 0, "legacy by-module state");
    mm_module_close(&t, 2); CHECK(mm_module_get(&t, 2) == 0, "legacy close");
    /* prepo store */
    PrepoStore ps; prepo_init(&ps); CHECK(ps.agreement_check == 1 && ps.bytes == 0, "defaults");
    prepo_account(&ps, 100); prepo_account(&ps, 50); CHECK(ps.bytes == 150 && ps.count == 2, "reports accounted");
    prepo_account(&ps, PREPO_STORAGE_LIMIT); CHECK(ps.bytes == 150 && ps.count == 2, "past the limit a report is dropped");
    prepo_clear(&ps); CHECK(ps.bytes == 0 && ps.count == 0, "clear");

    IpcCall c;
    /* ---- mm:u over IPC */
    IpcObject *mm = mm_open(); mm_init(&mm_tab);
    u32call(mm, 4, 7, 0, 0, &c); uint32_t id = SVC_OUT(&c, uint32_t, 0); CHECK(id == 1, "Initialize returns request 1");
    u32call(mm, 6, id, 300, 400, &c); u32call(mm, 7, id, 0, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 300, "SetAndWait then Get");
    u32call(mm, 5, id, 0, 0, &c); u32call(mm, 7, id, 0, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "Finalize drops the request");
    u32call(mm, 0, 3, 0, 0, &c); u32call(mm, 2, 3, 9, 9, &c); u32call(mm, 3, 3, 0, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 9, "legacy by-module calls");
    u32call(mm, 1, 3, 0, 0, &c); u32call(mm, 3, 3, 0, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "legacy Finalize");

    /* ---- nifm request parameters and general service */
    IpcObject *nf = nifm_open(); svc_call(nf, 5, NULL, 0, &c); IpcObject *gen = c.out_objects[0];
    svc_call(gen, 1, NULL, 0, &c); uint32_t cid1 = SVC_OUT(&c, uint32_t, 0); CHECK(c.out_size == 16 && cid1 != 0, "GetClientId: 16 bytes, non-zero");
    svc_call(nf, 5, NULL, 0, &c); IpcObject *gen2 = c.out_objects[0]; svc_call(gen2, 1, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) != cid1, "client ids differ per service object");
    uint8_t ex[16] = {1, 2, 3}; svc_call(gen, 26, ex, 16, &c); CHECK(c.result == 0 && !memcmp(((NifmGen *)gen->state)->exclusive, ex, 16), "SetExclusiveClient stored");
    uint8_t ip[0x40]; memset(ip, 0xcc, sizeof ip); memset(&c, 0, sizeof c); svc_set_out_buf(&c, ip, 0x20); svc_call_c(gen, 27, &c); CHECK(ip[0] == 0 && ip[0x1f] == 0 && ip[0x20] == 0xcc, "GetDefaultIpSetting: zeroed, bounded");
    svc_call(gen, 4, NULL, 0, &c); IpcObject *rq = c.out_objects[0]; NifmReq *q = rq->state;
    uint32_t v32 = 0x11; svc_call(rq, 6, &v32, 4, &c); svc_call(rq, 8, &v32, 4, &c); v32 = 0x22; svc_call(rq, 15, &v32, 4, &c);
    CHECK(q->requirement_preset == 0x11 && q->priority == 0x11 && q->raw_priority == 0x22, "preset, priority and raw priority stored");
    uint8_t one = 1;
    svc_call(rq, 10, &one, 1, &c); svc_call(rq, 11, &one, 1, &c); svc_call(rq, 12, &one, 1, &c); svc_call(rq, 13, &one, 1, &c); svc_call(rq, 14, &one, 1, &c);
    CHECK(q->rejectable && q->confirmation_option && q->persistent && q->instant && q->sustainable, "flag setters stored");
    uint8_t req[0x10] = {5, 6, 7}; svc_call(rq, 5, req, sizeof req, &c); CHECK(!memcmp(q->requirement, req, sizeof req), "SetRequirement stored");
    uint8_t prof[16] = {9, 8, 7}; svc_call(rq, 9, prof, 16, &c); CHECK(!memcmp(q->profile_id, prof, 16), "SetNetworkProfileId stored");
    svc_call(rq, 10, NULL, 0, &c); CHECK(c.result == 0 && q->rejectable == 0, "short setter request is harmless");

    /* ---- friend service */
    IpcObject *fr = friend_open(); svc_call(fr, 0, NULL, 0, &c); IpcObject *fs = c.out_objects[0];
    svc_call(fs, 10600, NULL, 0, &c); CHECK(*(uint8_t *)fs->state == 1, "DeclareOpenOnlinePlaySession");
    svc_call(fs, 10601, NULL, 0, &c); CHECK(*(uint8_t *)fs->state == 0, "DeclareCloseOnlinePlaySession");
    svc_call(fs, 10400, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "no blocked users");
    svc_call(fs, 0, NULL, 0, &c); CHECK(c.nout_copy == 1 && !event_is_signaled(c.out_copy[0]), "completion event");
    svc_call(fr, 1, NULL, 0, &c); IpcObject *nt = c.out_objects[0];
    svc_call(nt, 2, NULL, 0, &c); CHECK(c.result == friend_pop_notification_result(), "empty notification queue");
    svc_call(nt, 1, NULL, 0, &c); CHECK(c.result == 0, "Clear");

    /* ---- bsd */
    IpcObject *bs = bsd_open();
    svc_call(bs, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "RegisterClient answers 0 (SMO aborts on any other value)"); svc_call(bs, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "RegisterClient answers 0 again");
    svc_call(bs, 1, NULL, 0, &c); CHECK(c.result == 0 && bsd_monitored == 1, "StartMonitoring");
    uint8_t stats[0x50]; memset(stats, 0xcc, sizeof stats); memset(&c, 0, sizeof c); svc_set_out_buf(&c, stats, sizeof stats); svc_call_c(bs, 29, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 0 && SVC_OUT(&c, int32_t, 4) == 0 && stats[0] == 0 && stats[0x3f] == 0 && stats[0x40] == 0xcc, "GetResourceStatistics: ok, zeroed block");

    /* ---- nsd */
    IpcObject *ns = nsd_open(); uint8_t blob[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    memset(&c, 0, sizeof c); svc_set_in_buf(&c, blob, 8); svc_call_c(ns, 14, &c); CHECK(nsd_import_len == 8 && nsd_import[7] == 8, "ImportSettings stored");
    svc_call(ns, 13, NULL, 0, &c); CHECK(nsd_import_len == 0, "DeleteSettings");
    uint8_t nas[0x108]; memset(nas, 0xcc, sizeof nas); memset(&c, 0, sizeof c); svc_set_out_buf(&c, nas, sizeof nas); svc_call_c(ns, 30, &c); CHECK(nas[0] == 0 && nas[0x107] == 0, "GetNasServiceSetting: empty");
    char name[8]; memset(name, 0, sizeof name); memset(&c, 0, sizeof c); svc_set_out_buf(&c, name, sizeof name); svc_call_c(ns, 10, &c); CHECK(!strcmp(name, "lp1"), "environment name");

    /* ---- prepo */
    IpcObject *pr = prepo_open(); pthread_mutex_lock(&prepo_lock); prepo_init(prepo_st()); pthread_mutex_unlock(&prepo_lock);
    svc_call(pr, 40100, NULL, 0, &c); CHECK(c.out[0] == 1, "agreement check on by default");
    uint8_t zero = 0; svc_call(pr, 40101, &zero, 1, &c); svc_call(pr, 40100, NULL, 0, &c); CHECK(c.out[0] == 0, "SetUserAgreementCheckEnabled");
    uint8_t rep[0x40] = {0}; memset(&c, 0, sizeof c); svc_set_in_buf(&c, rep, sizeof rep); svc_call_c(pr, 10100, &c); svc_call_c(pr, 20100, &c);
    svc_call(pr, 90200, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 0x80 && SVC_OUT(&c, uint64_t, 8) == PREPO_STORAGE_LIMIT, "GetStorageUsage counts both reports");
    svc_call(pr, 90100, NULL, 0, &c); svc_call(pr, 90200, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 0, "ClearStorage");
    svc_call(pr, 10300, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "nothing is pending transmission");

    /* ---- ssl */
    IpcObject *sl = ssl_open(); ssl_contexts = 0;
    svc_call(sl, 0, NULL, 0, &c); IpcObject *cx = c.out_objects[0]; svc_call(sl, 0, NULL, 0, &c); IpcObject *cx2 = c.out_objects[0];
    svc_call(sl, 1, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 2, "GetContextCount = 2");
    u32call(cx, 0, 3, 77, 0, &c); u32call(cx, 1, 3, 0, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 77, "SetOption/GetOption");
    u32call(cx2, 1, 3, 0, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "options are per context");
    u32call(cx, 0, 99, 1, 0, &c); CHECK(c.result == 0, "out-of-range option ignored");
    svc_call(cx, 3, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "no connections yet");
    svc_call(cx, 2, NULL, 0, &c); IpcObject *cn = c.out_objects[0]; svc_call(cx, 3, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 1, "CreateConnection counts");
    svc_call(cx, 4, NULL, 0, &c); uint64_t p1 = SVC_OUT(&c, uint64_t, 0); svc_call(cx, 5, NULL, 0, &c); uint64_t p2 = SVC_OUT(&c, uint64_t, 0); CHECK(p1 && p2 && p1 != p2, "PKI imports get distinct ids");
    svc_call(cx, 6, &p1, 8, &c); CHECK(c.result == 0 && ssl_x(cx)->npki == 1 && ssl_x(cx)->pki[0] == p2, "RemovePki drops one id");
    svc_call(cn, 21, NULL, 0, &c); CHECK(c.result == 0, "FlushSessionCache");
    ssl_conn_destroy(cn); svc_call(cx, 3, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "destroying a connection uncounts it");
    uint32_t ver = 5; svc_call(sl, 5, &ver, 4, &c); CHECK(ssl_interface_version == 5, "SetInterfaceVersion");

    /* ---- nfc */
    IpcObject *nfc = nfc_open(); svc_call(nfc, 0, NULL, 0, &c); IpcObject *nu = c.out_objects[0];
    svc_call(nu, 401, NULL, 0, &c); CHECK(((NfpSvc *)nu->state)->keep_session == 1, "KeepPassThroughSession");
    svc_call(nu, 402, NULL, 0, &c); CHECK(((NfpSvc *)nu->state)->keep_session == 0, "ReleasePassThroughSession");
    /* nfc:sys / nfp:sys (firmware NFC applets) answer CreateSystemInterface with the simulated interfaces */
    { IpcObject *sys = nfc_sys_open(); svc_call(sys, 0, NULL, 0, &c); CHECK(c.result == 0 && c.nout_objects == 1 && c.out_objects[0]->state != NULL, "nfc:sys CreateSystemInterface returns an INfc object");
      /* the Mii Maker's first call on it: a pass-through with an unselected handle, answered empty instead of DeviceNotFound (fatal to the applet) */
      IpcObject *ns = c.out_objects[0]; uint8_t probe[32] = {0}; svc_call(ns, 400, probe, sizeof probe, &c);
      CHECK(c.result == 0, "nfc:sys pass-through before ListDevices succeeds (got %#x)", c.result);
      svc_call(nu, 400, probe, sizeof probe, &c); CHECK(c.result == NFP_R_DEVICE_NOT_FOUND, "nfc:user pass-through with an unknown handle is DeviceNotFound");
      IpcObject *psys = nfp_sys_open(); svc_call(psys, 0, NULL, 0, &c); CHECK(c.result == 0 && c.nout_objects == 1, "nfp:sys CreateSystemInterface returns an INfpUser object"); }
    printf("{\"test\":\"svc_offline_depth\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
