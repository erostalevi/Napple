/* Unit tests (ipc-depth-95 r10, part local): IBcatService 20300 GetDeliveryCacheStorageUpdateNotifier, 20301 RequestSuspendDeliveryTask,
 * 20400 / 20401 Register / UnregisterSystemApplicationDeliveryTask and 90200 GetDeliveryList, without a delivery cache provider (offline) and over a
 * provider folder in both layouts. */
#include "../src/services/bcat.c"
#include "svc_test.h"
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void put(const char *root, const char *rel, const void *data, size_t n) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
}
static void mk(const char *root, const char *rel) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    mkdir(path, 0755);
}
static IpcObject *bcat_service(void) {
    IpcObject *root = bcat_open(); IpcCall c;
    svc_call(root, 0, NULL, 0, &c);
    return c.out_objects[0];
}
static IpcObject *obj_with_app(IpcObject *bs, unsigned cmd, uint64_t app, uint32_t *result) {
    IpcCall c;
    svc_call(bs, cmd, &app, 8, &c);
    *result = c.result;
    return c.nout_objects ? c.out_objects[0] : NULL;
}
static uint32_t get_event(IpcObject *o) {
    IpcCall c;
    svc_call(o, 0, NULL, 0, &c);
    return c.nout_copy ? c.out_copy[0] : 0;
}
static void sync_now(IpcObject *bs) {
    IpcCall c;
    svc_call(bs, 10100, NULL, 0, &c);
}
static void close_obj(IpcObject *o) {
    if (o->destroy) o->destroy(o);
    free(o);
}

int main(void) {
    const uint64_t app = 0x0100000000010000ull;
    bcat_set_provider(NULL);
    IpcObject *bs = bcat_service(); IpcCall c; uint32_t r;
    const unsigned real[] = {20300, 20301, 20400, 20401, 90200}, kept[] = {20410, 20500, 30210, 30300, 90202, 90300, 90301};
    for (unsigned i = 0; i < 5; i++) CHECK(svc_find(bs, real[i]) && svc_find(bs, real[i])->status == IPC_IMPL, "IBcatService %u is real", real[i]);
    for (unsigned i = 0; i < 7; i++) CHECK(svc_find(bs, kept[i]) && svc_find(bs, kept[i])->status == IPC_IMPL, "IBcatService %u is real (task timers, push log, usage: r11 ipc-bcat-npns-local)", kept[i]);
    CHECK(bcat_storage_fingerprint(NULL) == 0, "no provider: fingerprint 0");

    /* ---- offline (no provider) ---- */
    IpcObject *nt = obj_with_app(bs, 20300, app, &r);
    CHECK(r == 0 && nt && !strcmp(nt->iface, "INotifierService"), "20300 returns an INotifierService");
    uint32_t ev = get_event(nt); CHECK(ev && !event_is_signaled(ev), "...its event starts clear");
    sync_now(bs); CHECK(!event_is_signaled(ev), "offline a sync changes nothing: the event stays clear");
    { uint8_t list[0x100]; memset(&c, 0, sizeof c); c.in = (const uint8_t *)&app; c.in_size = 8; svc_set_out_buf(&c, list, sizeof list); svc_call_c(bs, 90200, &c);
      CHECK(c.result == HZ_R_BCAT_NOT_FOUND, "90200 GetDeliveryList offline: no list"); }
    IpcObject *su = obj_with_app(bs, 20301, app, &r);
    CHECK(r == 0 && su && !strcmp(su->iface, "IDeliveryTaskSuspensionService") && bcat_suspend_count(app) == 1, "20301 returns a suspension; the task is suspended");
    CHECK(event_is_signaled(get_event(su)), "...its event is signalled: the suspension is in effect at once");
    IpcObject *su2 = obj_with_app(bs, 20301, app, &r); CHECK(r == 0 && bcat_suspend_count(app) == 2, "a second suspension of the same application counts");
    CHECK(bcat_suspend_count(app + 1) == 0, "...other applications are not suspended");
    close_obj(su); CHECK(bcat_suspend_count(app) == 1, "closing one suspension: still suspended");
    close_obj(su2); CHECK(bcat_suspend_count(app) == 0, "closing the last: resumed");
    CHECK(!bcat_sysapp_registered(app), "no system-application task at first");
    svc_call(bs, 20400, &app, 8, &c); CHECK(c.result == 0 && bcat_sysapp_registered(app), "20400 registers the application's task");
    svc_call(bs, 20400, &app, 8, &c); CHECK(c.result == 0 && g_nsysapp_tasks == 1, "...registering twice keeps one");
    uint64_t other = app + 0x1000; svc_call(bs, 20400, &other, 8, &c); CHECK(bcat_sysapp_registered(other) && bcat_sysapp_registered(app), "a second application");
    svc_call(bs, 20401, &app, 8, &c); CHECK(c.result == 0 && !bcat_sysapp_registered(app) && bcat_sysapp_registered(other), "20401 unregisters only that one");
    svc_call(bs, 20401, &app, 8, &c); CHECK(c.result == 0, "unregistering an id that is not registered succeeds");
    svc_call(bs, 20401, &other, 8, &c);

    /* ---- a provider folder (plain layout) ---- */
    char root[] = "/tmp/napple-bcat-r10-XXXXXX"; if (!mkdtemp(root)) return 1;
    mk(root, "news"); put(root, "news/a.bin", "hello", 5);
    BcatProvider prov = bcat_dir_provider(root); bcat_set_provider(&prov);
    uint64_t fp0 = bcat_storage_fingerprint(&prov); CHECK(fp0 != 0 && fp0 == bcat_storage_fingerprint(&prov), "fingerprint of a cache: non-zero, stable");
    IpcObject *n2 = obj_with_app(bs, 20300, app, &r); uint32_t ev2 = get_event(n2);
    sync_now(bs); CHECK(!event_is_signaled(ev2), "a sync over unchanged data: no update");
    CHECK(event_is_signaled(ev), "...but the notifier opened before the cache existed sees it appear");
    event_clear_handle(ev);
    put(root, "news/b.bin", "world!", 6);
    CHECK(bcat_storage_fingerprint(&prov) != fp0, "a new file changes the fingerprint");
    CHECK(!event_is_signaled(ev2), "...but nothing fires before a sync");
    sync_now(bs); CHECK(event_is_signaled(ev2) && event_is_signaled(ev), "a sync that finds new data fires every notifier");
    event_clear_handle(ev2); event_clear_handle(ev);
    sync_now(bs); CHECK(!event_is_signaled(ev2), "the next sync, nothing new: no event");
    put(root, "news/b.bin", "WORLD!", 6); sync_now(bs); CHECK(event_is_signaled(ev2), "changed content (same size, new digest) is an update");
    IpcObject *n3 = obj_with_app(bs, 20300, app, &r);
    mk(root, "extra"); put(root, "extra/x.txt", "x", 1);
    svc_call(bs, 20100, &app, 8, &c); CHECK(c.nout_objects == 1, "RequestSyncDeliveryCacheWithApplicationId still returns its progress");
    uint32_t ev3 = get_event(n3); CHECK(event_is_signaled(ev3), "a change seen before the notifier's first GetEvent is reported on it");
    CHECK(!event_is_signaled(get_event(n3)), "...once");
    close_obj(n3); close_obj(n2); close_obj(nt);
    CHECK(!g_notifiers[0] && !g_notifiers[1] && !g_notifiers[2], "closed notifiers leave the list");
    { uint8_t list[0x40]; memset(&c, 0, sizeof c); c.in = (const uint8_t *)&app; c.in_size = 8; svc_set_out_buf(&c, list, sizeof list); svc_call_c(bs, 90200, &c);
      CHECK(c.result == HZ_R_BCAT_NOT_FOUND, "90200 with a cache but no list.msgpack: not found"); }
    const uint8_t msgpack[] = {0x81, 0xa4, 't', 'o', 'p', 'i', 0xa3, 'a', 'b', 'c'};
    put(root, "list.msgpack", msgpack, sizeof msgpack);
    CHECK(bcat_storage_fingerprint(&prov) == bcat_storage_fingerprint(&prov), "list.msgpack is not a directory of the cache");
    { uint8_t list[0x40]; memset(list, 0, sizeof list); memset(&c, 0, sizeof c); c.in = (const uint8_t *)&app; c.in_size = 8; svc_set_out_buf(&c, list, sizeof list); svc_call_c(bs, 90200, &c);
      CHECK(c.result == 0 && c.out_size == 8 && SVC_OUT(&c, uint64_t, 0) == sizeof msgpack && !memcmp(list, msgpack, sizeof msgpack), "90200 GetDeliveryList returns the list and its size"); }
    { uint8_t small[4]; memset(&c, 0, sizeof c); c.in = (const uint8_t *)&app; c.in_size = 8; svc_set_out_buf(&c, small, sizeof small); svc_call_c(bs, 90200, &c);
      CHECK(c.result == 0 && SVC_OUT(&c, uint64_t, 0) == 4 && !memcmp(small, msgpack, 4), "...a short buffer gets what fits"); }
    { memset(&c, 0, sizeof c); c.in = (const uint8_t *)&app; c.in_size = 8; svc_call_c(bs, 90200, &c);
      CHECK(c.result == 0 && SVC_OUT(&c, uint64_t, 0) == 0, "...no buffer: nothing written"); }

    /* ---- the console's storage layout ---- */
    char sroot[] = "/tmp/napple-bcat-r10s-XXXXXX"; if (!mkdtemp(sroot)) return 1;
    { uint8_t dm[4 + BCAT_DIR_META_ENTRY] = {1}; memcpy(dm + 4, "dl", 3); put(sroot, "directories.meta", dm, sizeof dm);
      mk(sroot, "directories"); mk(sroot, "directories/dl"); mk(sroot, "directories/dl/files");
      uint8_t fm[4 + BCAT_FILE_META_ENTRY] = {1}; memcpy(fm + 4, "f.bin", 6); int64_t sz = 3; memcpy(fm + 4 + 0x28, &sz, 8); fm[4 + 0x30] = 0x11;
      put(sroot, "directories/dl/files.meta", fm, sizeof fm); put(sroot, "directories/dl/files/f.bin", "abc", 3);
      put(sroot, "list.msgpack", msgpack, 5); }
    BcatProvider sp = bcat_auto_provider(sroot); bcat_set_provider(&sp);
    IpcObject *n4 = obj_with_app(bs, 20300, app, &r); uint32_t ev4 = get_event(n4);
    { uint8_t list[0x40]; memset(&c, 0, sizeof c); c.in = (const uint8_t *)&app; c.in_size = 8; svc_set_out_buf(&c, list, sizeof list); svc_call_c(bs, 90200, &c);
      CHECK(c.result == 0 && SVC_OUT(&c, uint64_t, 0) == 5 && !memcmp(list, msgpack, 5), "storage layout: GetDeliveryList reads its list.msgpack"); }
    sync_now(bs); CHECK(!event_is_signaled(ev4), "storage layout: unchanged");
    { uint8_t fm[4 + BCAT_FILE_META_ENTRY] = {1}; memcpy(fm + 4, "f.bin", 6); int64_t sz = 3; memcpy(fm + 4 + 0x28, &sz, 8); fm[4 + 0x30] = 0x22; /* a new digest */
      put(sroot, "directories/dl/files.meta", fm, sizeof fm); }
    sync_now(bs); CHECK(event_is_signaled(ev4), "storage layout: a new digest in files.meta is an update");
    close_obj(n4);
    bcat_set_provider(NULL);

    printf("{\"test\":\"svc_local_r10_bcat\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
