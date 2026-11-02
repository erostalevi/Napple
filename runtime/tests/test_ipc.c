/* Unit tests: handle table and HIPC/CMIF framing. ipc.c is included so the static parse / respond / dispatch are testable without a guest. */
#include "../src/ipc.c"
#include "check.h"

/* ---- a test service ---- */
static int g_noop_calls;
static void echo_u32(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = IPC_IN(c, uint32_t, 0); v += 1; IPC_OUT(c, v); }
static void out_u8_u64(IpcObject *s, IpcCall *c) { (void)s; uint8_t a = 7; uint64_t b = 0x1122334455667788ull; IPC_OUT(c, a); IPC_OUT(c, b); }
static void fail_cmd(IpcObject *s, IpcCall *c) { (void)s; c->result = 0x1234; }
static void noop(IpcObject *s, IpcCall *c) { (void)s; (void)c; g_noop_calls++; }
static void out_handle(IpcObject *s, IpcCall *c) { (void)s; ipc_out_copy_handle(c, 0xabc); }
static void sub_open(IpcObject *s, IpcCall *c);
static const IpcCmd sub_cmds[] = {{0, "Echo", echo_u32, IPC_IMPL}};
static void sub_open(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("ISub", sub_cmds, NULL, NULL)); }
static const IpcCmd root_cmds[] = {{0, "Echo", echo_u32, IPC_IMPL}, {1, "Out", out_u8_u64, IPC_IMPL}, {2, "Fail", fail_cmd, IPC_IMPL}, {3, "Noop", noop, IPC_STUB},
                                   {4, "Handle", out_handle, IPC_IMPL}, {5, "OpenSub", sub_open, IPC_IMPL}};

/* Builds a plain HIPC request: header, optional special header, raw data section aligned to 16 with the CMIF header. */
static unsigned build(uint32_t *m, unsigned type, unsigned cmd, const void *args, unsigned nargs, int domain_obj, unsigned ncopy, const uint32_t *copies) {
    memset(m, 0, 0x100);
    unsigned special = ncopy != 0, domain = domain_obj >= 0;
    uint32_t *p = m + 2;
    if (special) { *p++ = ncopy << 1; for (unsigned i = 0; i < ncopy; i++) *p++ = copies[i]; }
    uint8_t *payload = (uint8_t *)m + ((((uint8_t *)p - (uint8_t *)m) + 15) & ~15);
    uint8_t *w = payload;
    if (domain) { uint8_t dh[16] = {1, 0}; uint16_t sz = (uint16_t)(16 + nargs); memcpy(dh + 2, &sz, 2); uint32_t oid = (uint32_t)domain_obj; memcpy(dh + 4, &oid, 4); memcpy(w, dh, 16); w += 16; }
    uint32_t h[4] = {CMIF_IN_MAGIC, 0, cmd, 0}; memcpy(w, h, 16); w += 16;
    if (nargs) memcpy(w, args, nargs);
    w += nargs;
    unsigned words = (unsigned)((w - (uint8_t *)p + 3) / 4);
    m[0] = type; m[1] = (words & 0x3ff) | (special ? 1u << 31 : 0);
    return words;
}
static const uint8_t *response_payload(const uint32_t *m, int domain, uint32_t *result) {
    unsigned special = m[1] >> 31; const uint32_t *p = m + 2;
    if (special) { unsigned nc = (p[0] >> 1) & 0xf, nm = (p[0] >> 5) & 0xf; p += 1 + nc + nm; }
    const uint8_t *payload = (const uint8_t *)m + ((((const uint8_t *)p - (const uint8_t *)m) + 15) & ~15);
    if (domain) payload += 16;
    uint32_t h[4]; memcpy(h, payload, 16);
    if (h[0] != CMIF_OUT_MAGIC) return NULL;
    *result = h[2]; return payload + 16;
}

int main(void) {
    /* ---- handle table ---- */
    int a, b;
    uint32_t h1 = handle_new(OBJ_MISC, &a), h2 = handle_new(OBJ_SESSION, &b);
    CHECK(h1 && h2 && h1 != h2, "distinct handles");
    CHECK(handle_get(h1, OBJ_MISC) == &a && handle_get(h1, OBJ_SESSION) == NULL, "type checked lookup");
    CHECK(handle_type(h2) == OBJ_SESSION && handle_type(0) == OBJ_FREE && handle_type(0x7fffffff) == OBJ_FREE, "types, invalid handles");
    CHECK((h1 >> 30) == 0, "bit 30 stays clear (mutex tag waiter bit)");
    ObjType t; CHECK(handle_close(h1, &t) && t == OBJ_MISC, "close returns the object type");
    CHECK(handle_get(h1, OBJ_MISC) == NULL && handle_close(h1, NULL) == NULL, "closed handle is invalid");
    uint32_t h3 = handle_new(OBJ_MISC, &a);   /* the slot is reused, the handle value is not */
    CHECK((h3 & 0x7fff) == (h1 & 0x7fff) && h3 != h1 && handle_get(h1, OBJ_MISC) == NULL, "stale handle stays invalid after slot reuse");
    handle_close(h2, NULL); handle_close(h3, NULL);
    uint32_t many[4200]; unsigned n = 0; for (; n < 4200; n++) { many[n] = handle_new(OBJ_MISC, &a); if (!many[n]) break; }
    CHECK(n > 4000 && n < 4200, "table is bounded (%u handles)", n);
    for (unsigned i = 0; i < n; i++) handle_close(many[i], NULL);
    CHECK(handle_new(OBJ_MISC, &a) != 0, "handles available again after closing");

    /* ---- plain session ---- */
    IpcObject *root = IPC_OBJECT("IRoot", root_cmds, NULL, NULL);
    IpcSession *s = session_new(root);
    uint32_t m[64]; uint32_t res; const uint8_t *out;
    uint32_t arg = 41; build(m, T_REQUEST, 0, &arg, 4, -1, 0, NULL);
    dispatch(s, m); out = response_payload(m, 0, &res);
    uint32_t got; CHECK(out && res == 0, "echo succeeds"); memcpy(&got, out, 4); CHECK(got == 42, "echo returns arg+1 (%u)", got);
    build(m, T_REQUEST, 1, NULL, 0, -1, 0, NULL); dispatch(s, m); out = response_payload(m, 0, &res);
    uint64_t b64; CHECK(out && res == 0 && out[0] == 7, "u8 out"); memcpy(&b64, out + 8, 8); CHECK(b64 == 0x1122334455667788ull, "u64 out is 8-aligned");
    build(m, T_REQUEST, 2, NULL, 0, -1, 0, NULL); dispatch(s, m); out = response_payload(m, 0, &res); CHECK(out && res == 0x1234, "result code propagates");
    build(m, T_REQUEST, 3, NULL, 0, -1, 0, NULL); dispatch(s, m); CHECK(g_noop_calls == 1, "stub command ran");
    build(m, T_REQUEST_CTX, 3, NULL, 0, -1, 0, NULL); dispatch(s, m); CHECK(g_noop_calls == 2, "RequestWithContext framed like Request");
    setenv("NAPPLE_IPC_LENIENT", "1", 1);
    build(m, T_REQUEST, 99, NULL, 0, -1, 0, NULL); dispatch(s, m); out = response_payload(m, 0, &res); CHECK(out && res == IPC_R_UNKNOWN_COMMAND, "unknown command: error result in lenient mode");
    build(m, T_REQUEST, 4, NULL, 0, -1, 0, NULL); dispatch(s, m);
    CHECK((m[1] >> 31) == 1 && ((m[2] >> 1) & 0xf) == 1 && m[3] == 0xabc, "copy handle returned in the special header");
    build(m, T_CONTROL, 3, NULL, 0, -1, 0, NULL); dispatch(s, m); out = response_payload(m, 0, &res); uint16_t pb; memcpy(&pb, out, 2); CHECK(out && res == 0 && pb == POINTER_BUFFER_SIZE, "control: pointer buffer size");
    /* a sub object comes back as a move handle on a plain session */
    build(m, T_REQUEST, 5, NULL, 0, -1, 0, NULL); dispatch(s, m);
    CHECK((m[1] >> 31) == 1 && ((m[2] >> 5) & 0xf) == 1, "sub-object returned as a move handle");
    uint32_t sub_h = m[3]; IpcSession *ss = handle_get(sub_h, OBJ_SESSION); CHECK(ss && ss->object && !strcmp(ss->object->iface, "ISub"), "move handle is a session of the sub-object");
    if (ss) { arg = 9; build(m, T_REQUEST, 0, &arg, 4, -1, 0, NULL); dispatch(ss, m); out = response_payload(m, 0, &res); memcpy(&got, out, 4); CHECK(got == 10, "sub-object command runs"); }

    /* ---- TIPC: type = 16 + command, raw arguments right after the header, result word first in the reply ---- */
    { uint32_t t[64] = {0}; t[0] = T_TIPC_BASE + 0; t[1] = 1; t[2] = 20;               /* echo_u32(20), no CMIF header */
      dispatch(s, t); CHECK((t[1] & 0x3ff) == 2 && t[2] == 0 && t[3] == 21, "tipc request: result word then output data (%u words, res %#x, out %u)", t[1] & 0x3ff, t[2], t[3]);
      memset(t, 0, sizeof t); t[0] = T_TIPC_BASE + 2; t[1] = 0; dispatch(s, t);
      CHECK((t[1] & 0x3ff) == 1 && t[2] == 0x1234, "tipc result code propagates");
      memset(t, 0, sizeof t); t[0] = T_TIPC_BASE + 4; dispatch(s, t);
      CHECK((t[1] >> 31) == 1 && ((t[2] >> 1) & 0xf) == 1 && t[3] == 0xabc && t[4] == 0, "tipc copy handle in the special header");
      memset(t, 0, sizeof t); t[0] = T_TIPC_BASE + 5; dispatch(s, t);
      CHECK((t[1] >> 31) == 1 && ((t[2] >> 5) & 0xf) == 1, "tipc sub-object returned as a move handle");
      memset(t, 0, sizeof t); t[0] = T_TIPC_BASE + 99; dispatch(s, t); CHECK(t[2] == IPC_R_UNKNOWN_COMMAND, "tipc unknown command: error result in lenient mode");
      memset(t, 0, sizeof t); t[0] = T_TIPC_CLOSE; dispatch(s, t); }

    /* ---- sm over TIPC: a failed GetServiceHandle keeps the success layout (the SDK reads the move handle at word 3 and the result at word 4) ---- */
    { IpcSession *sm = session_new(sm_open()); uint32_t t[64] = {0}; t[0] = T_TIPC_BASE + 1; t[1] = 2; memcpy(&t[2], "nosuchsv", 8); dispatch(sm, t);
      CHECK((t[1] >> 31) == 1 && ((t[2] >> 5) & 0xf) == 1 && t[3] == 0 && t[4] == IPC_R_NOT_FOUND(21, 5), "tipc sm error reply: move handle 0 at word 3, result at word 4 (%#x %#x %#x)", t[2], t[3], t[4]); }

    /* ---- domain ---- */
    build(m, T_CONTROL, 0, NULL, 0, -1, 0, NULL); dispatch(s, m); out = response_payload(m, 0, &res);
    uint32_t root_id; memcpy(&root_id, out, 4); CHECK(res == 0 && root_id == 1 && s->is_domain, "ConvertCurrentObjectToDomain gives object id 1");
    arg = 100; build(m, T_REQUEST, 0, &arg, 4, 1, 0, NULL); dispatch(s, m); out = response_payload(m, 1, &res); memcpy(&got, out, 4); CHECK(res == 0 && got == 101, "domain request on object 1");
    build(m, T_REQUEST, 5, NULL, 0, 1, 0, NULL); dispatch(s, m); out = response_payload(m, 1, &res);
    uint32_t new_id; const uint8_t *ids = out + 0; (void)ids; memcpy(&new_id, out, 4);
    CHECK(res == 0 && s->dom->objects[2] != NULL && !strcmp(s->dom->objects[2]->iface, "ISub"), "sub-object registered as domain object 2 (id %u)", new_id);
    arg = 5; build(m, T_REQUEST, 0, &arg, 4, 2, 0, NULL); dispatch(s, m); out = response_payload(m, 1, &res); memcpy(&got, out, 4); CHECK(res == 0 && got == 6, "request addressed to domain object 2");
    { uint8_t close_msg[0x100] = {0}; uint32_t *cm = (uint32_t *)close_msg; uint8_t dh[16] = {2, 0}; uint32_t oid = 2; memcpy(dh + 4, &oid, 4);
      cm[0] = T_REQUEST; cm[1] = 8; memcpy(close_msg + 16, dh, 16); dispatch(s, cm); CHECK(s->dom->objects[2] == NULL, "domain close message releases the object"); }

    /* ---- domain id reuse: a title that makes short-lived objects (MK8D's acc IProfile every 200 ms) must not exhaust the table ---- */
    { IpcDomain *d = s->dom; uint32_t seen_max = 0; unsigned live = 0, failed0 = g_failed;
      /* find the first free id from the current state, then churn: open + close one object at a time far past DOMAIN_MAX */
      for (unsigned i = 0; i < 5 * DOMAIN_MAX; i++) {
          build(m, T_REQUEST, 5, NULL, 0, 1, 0, NULL); dispatch(s, m); out = response_payload(m, 1, &res);
          uint32_t id; memcpy(&id, out, 4);
          CHECK(res == 0 && id >= 2 && id <= DOMAIN_MAX && d->objects[id] && !strcmp(d->objects[id]->iface, "ISub"), "churn %u: new id %u valid", i, id);
          if (id > seen_max) seen_max = id;
          uint8_t cl[0x100] = {0}; uint32_t *cm = (uint32_t *)cl; uint8_t dh[16] = {2, 0}; memcpy(dh + 4, &id, 4);
          cm[0] = T_REQUEST; cm[1] = 8; memcpy(cl + 16, dh, 16); dispatch(s, cm);
          CHECK(d->objects[id] == NULL, "churn %u: id %u released", i, id);
          if (g_failed != failed0) break;
      }
      CHECK(seen_max <= DOMAIN_MAX, "ids stay within the table over %u create/close cycles", 5 * DOMAIN_MAX);
      for (unsigned i = 1; i <= DOMAIN_MAX; i++) if (d->objects[i]) live++;
      CHECK(live == 1, "only the root remains open after churn (%u)", live);

      /* ids stay stable while open: hold 3 objects, churn around them, they keep their id and object */
      uint32_t held[3]; IpcObject *held_obj[3];
      for (unsigned i = 0; i < 3; i++) { build(m, T_REQUEST, 5, NULL, 0, 1, 0, NULL); dispatch(s, m); out = response_payload(m, 1, &held[i]), memcpy(&held[i], out, 4); held_obj[i] = d->objects[held[i]]; }
      CHECK(held[0] != held[1] && held[1] != held[2] && held[0] != held[2], "three open objects have distinct ids");
      for (unsigned i = 0; i < 3 * DOMAIN_MAX; i++) {
          pthread_mutex_lock(&d->lock); unsigned id = domain_alloc_locked(d, held_obj[0]); pthread_mutex_unlock(&d->lock); /* any object works as a placeholder */
          object_ref(held_obj[0]);
          CHECK(id != held[0] && id != held[1] && id != held[2], "a fresh id never collides with an open one");
          pthread_mutex_lock(&d->lock); IpcObject *o = domain_release_locked(d, id); pthread_mutex_unlock(&d->lock); object_unref(o);
          if (g_failed != failed0) break;
      }
      CHECK(d->objects[held[0]] == held_obj[0] && d->objects[held[1]] == held_obj[1] && d->objects[held[2]] == held_obj[2], "held ids keep their objects");

      /* exhaustion only when DOMAIN_MAX objects are open at once */
      IpcObject *ph = IPC_OBJECT("IPh", sub_cmds, NULL, NULL); unsigned got_ids = 0; uint32_t first_free[DOMAIN_MAX + 1];
      pthread_mutex_lock(&d->lock);
      for (;;) { object_ref(ph); unsigned id = domain_alloc_locked(d, ph); if (!id) { object_unref(ph); break; } first_free[got_ids++] = id; }
      pthread_mutex_unlock(&d->lock);
      unsigned open_now = 0; for (unsigned i = 1; i <= DOMAIN_MAX; i++) if (d->objects[i]) open_now++;
      CHECK(open_now == DOMAIN_MAX, "allocation fails only once all %d ids are open (%u)", DOMAIN_MAX, open_now);
      pthread_mutex_lock(&d->lock); IpcObject *rel = domain_release_locked(d, first_free[got_ids / 2]); unsigned again = domain_alloc_locked(d, rel); pthread_mutex_unlock(&d->lock);
      CHECK(again == first_free[got_ids / 2], "one close frees one slot: the next create gets it back (%u)", again);

      /* closing an unknown / out of range / never-opened id is an error reply, not a crash */
      uint32_t bad_ids[] = {0, DOMAIN_MAX + 1, 0xffffffffu, 100000};
      for (unsigned i = 0; i < 4; i++) {
          uint8_t cl[0x100] = {0}; uint32_t *cm = (uint32_t *)cl; uint8_t dh[16] = {2, 0}; memcpy(dh + 4, &bad_ids[i], 4);
          cm[0] = T_REQUEST; cm[1] = 8; memcpy(cl + 16, dh, 16); dispatch(s, cm); out = response_payload(cm, 1, &res);
          CHECK(out && res == IPC_R_DOMAIN_OBJECT_NOT_FOUND, "close of unknown id %#x answers an error result (%#x)", bad_ids[i], res);
      }
      /* a stale id closed twice: the second close errors and does not touch another object (the id is not handed out again before the never-used ids) */
      d->next_id = 3; d->free_count = 0; d->free_head = 0; /* fresh view of the allocator: ids 1,2 in use */
      for (unsigned i = 3; i <= DOMAIN_MAX; i++) d->objects[i] = NULL;
      build(m, T_REQUEST, 5, NULL, 0, 1, 0, NULL); dispatch(s, m); uint32_t stale; out = response_payload(m, 1, &res); memcpy(&stale, out, 4);
      uint8_t cl[0x100] = {0}; uint32_t *cm = (uint32_t *)cl; uint8_t dh[16] = {2, 0}; memcpy(dh + 4, &stale, 4); cm[0] = T_REQUEST; cm[1] = 8; memcpy(cl + 16, dh, 16);
      dispatch(s, cm); build(m, T_REQUEST, 5, NULL, 0, 1, 0, NULL); dispatch(s, m); uint32_t fresh; out = response_payload(m, 1, &res); memcpy(&fresh, out, 4);
      CHECK(fresh != stale && d->objects[fresh], "the next create does not take the just-closed id (%u vs %u)", fresh, stale);
      memcpy(dh + 4, &stale, 4); memset(cl, 0, sizeof cl); cm[0] = T_REQUEST; cm[1] = 8; memcpy(cl + 16, dh, 16); dispatch(s, cm); out = response_payload(cm, 1, &res);
      CHECK(res == IPC_R_DOMAIN_OBJECT_NOT_FOUND && d->objects[fresh] != NULL, "second close of the stale id errors and leaves the new object %u open", fresh);
    }
    printf("{\"test\":\"ipc\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
