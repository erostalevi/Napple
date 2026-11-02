/* Mac-only risk checks on the real titles (ipc-risk-mac-checks, round 11). Three rows that the unit tests could only fake:
 *  1. applet ExecuteProgram(RestartProgram) through the REAL host_relaunch (execv of this very binary): the new process gets the new argv, the user
 *     channel file and the previous program index.
 *  2. fsp-srv permission bits from each title's real main.npdm (shared title data, read-only): the title's own save and RomFS open, mounts the bits
 *     do not grant (game card, save transfer, content storage, rights id) are PermissionDenied.
 *  3. acc with no linked Napple profile (the NullProvider): every network-account operation answers 0x1907C.
 * Needs the titles built under NAPPLE_WORK (default /Users/neto/NappleWork/games); a missing title is reported as SKIP, not a failure.
 * applet.c, account.c and fs.c are #included, so this is one translation unit. */
/* applet.c and fs.c both define an IStorage: rename the applet one */
#define storage_destroy applet_storage_destroy
#define storage_cmds applet_storage_cmds
#include "../src/services/applet.c"
#undef storage_destroy
#undef storage_cmds
#include "../src/services/account.c"
#include "../src/services/fs.c"
#include "svc_test.h"
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>

#define GOLDEN_DIR "../tools/golden/risk"
static int g_skipped;

/* ---- 1. the execv restart ---------------------------------------------------------------------------------------------------------------------- */
static IpcObject *sub(IpcObject *proxy, unsigned cmd) { IpcCall c; svc_call(proxy, cmd, NULL, 0, &c); return c.nout_objects ? c.out_objects[0] : NULL; }
static IpcObject *app_functions(void) {
    IpcObject *oe = oe_open();
    IpcCall c;
    svc_call(oe, 0, NULL, 0, &c);
    return c.nout_objects ? sub(c.out_objects[0], 20) : NULL;
}
static IpcObject *storage_with(const char *text) {
    IpcObject *st = storage_new(strlen(text));
    memcpy(((Storage *)st->state)->data, text, strlen(text));
    return st;
}
/* The process the restart started: report what it was handed through the file named by RISK_RESULT. */
static int restarted_process(int argc, char **argv) {
    char text[64] = "", *e = getenv("RISK_RESULT");
    g_program_id = 0x0100AAAA00010000ull;
    IpcObject *af = app_functions();
    uint32_t kind = 1;
    IpcCall c;
    svc_call(af, 1, &kind, 4, &c);
    if (c.result == 0 && c.nout_objects) {
        Storage *st = c.out_objects[0]->state;
        snprintf(text, sizeof text, "%.*s", (int)(st->size < 63 ? st->size : 63), (const char *)st->data);
    }
    svc_call(af, 123, NULL, 0, &c);
    int prev = c.result ? -99 : SVC_OUT(&c, int32_t, 0);
    FILE *f = e ? fopen(e, "w") : NULL;
    if (!f) return 2;
    fprintf(f, "argc=%d argv1=%s channel=%s prev=%d env_channel_left=%d\n", argc, argc > 1 ? argv[1] : "", text, prev, getenv(ENV_USER_CHANNEL) != NULL);
    fclose(f);
    return 0;
}
static void t_restart(char **argv0) {
    char dir[256], res[300];
    snprintf(dir, sizeof dir, "%s/risk_restart.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    CHECK(mkdtemp(dir) != NULL, "temp dir");
    snprintf(res, sizeof res, "%s/result.txt", dir);
    pid_t pid = fork();
    if (pid == 0) {
        setenv("RISK_PHASE", "restarted", 1);
        setenv("RISK_RESULT", res, 1);
        setenv("NAPPLE_USER_CHANNEL_DIR", dir, 1);
        static char *nargv[] = {NULL, (char *)"--program-arg=restarted", NULL};
        nargv[0] = argv0[0];
        *_NSGetArgv() = nargv;
        *_NSGetArgc() = 2;
        g_program_id = 0x0100AAAA00010000ull;
        IpcObject *af = app_functions();
        IpcCall c;
        memset(&c, 0, sizeof c);
        c.in_objects[0] = storage_with("carried");
        c.nin_objects = 1;
        svc_call_c(af, 122, &c); /* UnpopToUserChannel */
        struct { uint32_t kind, pad; uint64_t value; } ep = {2, 0, 0};
        svc_call(af, 120, &ep, sizeof ep, &c); /* RestartProgram: execv does not return */
        _exit(100 + (int)c.result % 100);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "the restarted process ran to its end (status %d)", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    char line[300] = "";
    FILE *f = fopen(res, "r");
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    CHECK(strstr(line, "argc=2 argv1=--program-arg=restarted ") == line, "execv keeps the program arguments (%s)", line);
    CHECK(strstr(line, "channel=carried ") != NULL, "the new process pops the storage the old one left (%s)", line);
    CHECK(strstr(line, "prev=0 ") != NULL, "GetPreviousProgramIndex = 0 after the restart (%s)", line);
    CHECK(strstr(line, "env_channel_left=0") != NULL, "the hand-over variable is consumed (%s)", line);
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) printf("cleanup failed\n");
}

/* ---- 2. fs permission bits from the real npdm --------------------------------------------------------------------------------------------------- */
static IpcCall call(IpcObject *o, unsigned cmd, const void *in, uint32_t n) { IpcCall c; svc_call(o, cmd, in, n, &c); return c; }
static void drop(IpcObject *o) { if (o->destroy) o->destroy(o); free(o); }
static void check_build(const char *title, const char *build_dir, const char *saves) {
    char npdm[600];
    snprintf(npdm, sizeof npdm, "%s/main.npdm", build_dir);
    if (access(npdm, R_OK)) { printf("SKIP %s: no main.npdm\n", build_dir); g_skipped++; return; }
    CHECK(npdm_load(npdm) && g_npdm.valid, "%s: main.npdm loads", title);
    uint64_t perms = g_npdm.fs_perms;
    printf("  %s %s: title id %016llx fs_perms %016llx\n", title, strrchr(build_dir, '/') + 1, (unsigned long long)g_npdm.title_id, (unsigned long long)perms);
    g_cfg.data_dir = build_dir;
    g_cfg.save_root = saves;
    snprintf(g_cfg.title_id, sizeof g_cfg.title_id, "%s", title);
    g_program_id = strtoull(title, NULL, 16);
    IpcObject *srv = fsp_open();
    /* the title's own saves */
    uint8_t in[0x48] = {0}, attr[0x40] = {0};
    memcpy(attr, &g_program_id, 8);
    attr[0x20] = SAVE_DEVICE;
    in[0] = 1;
    memcpy(in + 8, attr, 0x40);
    IpcCall sv = call(srv, 51, in, sizeof in);
    CHECK(sv.result == 0 && sv.nout_objects == 1, "%s: OpenSaveDataFileSystem (device save) opens", title);
    if (sv.nout_objects) drop(sv.out_objects[0]);
    attr[0x20] = SAVE_ACCOUNT;
    memcpy(attr + 8, g_user_id, 16);
    memcpy(in + 8, attr, 0x40);
    sv = call(srv, 51, in, sizeof in);
    CHECK(sv.result == 0 && sv.nout_objects == 1, "%s: OpenSaveDataFileSystem (account save) opens", title);
    if (sv.nout_objects) drop(sv.out_objects[0]);
    /* the RomFS */
    IpcCall rf = call(srv, 200, NULL, 0);
    CHECK(rf.result == 0 && rf.nout_objects == 1, "%s: OpenDataStorageByCurrentProcess opens the RomFS (result %x)", title, rf.result);
    if (rf.nout_objects) {
        IpcCall sz = call(rf.out_objects[0], 4, NULL, 0);
        CHECK(sz.result == 0 && SVC_OUT(&sz, int64_t, 0) > 0, "%s: ...with a non-empty size", title);
        drop(rf.out_objects[0]);
    }
    /* mounts the real bits do not grant: none of them is held by an application unless the npdm says so */
    static const struct { unsigned cmd; uint64_t mask; const char *name; } forbidden[] = {
        {81, FS_PERM_TRANSFER_MANAGER, "OpenSaveDataTransferManager"}, {82, FS_PERM_TRANSFER_MANAGER_V2, "OpenSaveDataTransferManagerVersion2"},
        {86, FS_PERM_SAVE_MOVER, "OpenSaveDataMover"}, {87, FS_PERM_TRANSFER_FOR_REPAIR, "OpenSaveDataTransferManagerForRepair"},
    };
    unsigned denied = 0;
    for (unsigned i = 0; i < sizeof forbidden / sizeof *forbidden; i++) {
        int held = (perms & forbidden[i].mask & ~FS_PERM_DEBUG_BITS) != 0;
        uint32_t r = call(srv, forbidden[i].cmd, NULL, 0).result;
        CHECK(held ? r != R_FS_PERMISSION_DENIED : r == R_FS_PERMISSION_DENIED, "%s: %s %s (npdm bits %016llx, result %x)", title, forbidden[i].name,
              held ? "is allowed by the npdm" : "is PermissionDenied", (unsigned long long)perms, r);
        denied += r == R_FS_PERMISSION_DENIED;
    }
    uint32_t gc[2] = {0, 1};
    int gc_held = (perms & FS_PERM_MOUNT_GAME_CARD & ~FS_PERM_DEBUG_BITS) != 0;
    uint32_t r31 = call(srv, 31, gc, sizeof gc).result;
    CHECK(gc_held ? r31 == R_FS_GAME_CARD_NOT_INSERTED : r31 == R_FS_PERMISSION_DENIED, "%s: OpenGameCardFileSystem follows the GameCard bit (result %x)", title, r31);
    uint32_t cs = 1;
    int cm_held = (perms & FS_PERM_MOUNT_CONTENT_STORAGE & ~FS_PERM_DEBUG_BITS) != 0;
    uint32_t r110 = call(srv, 110, &cs, 4).result;
    CHECK(cm_held ? r110 != R_FS_PERMISSION_DENIED : r110 == R_FS_PERMISSION_DENIED, "%s: OpenContentStorageFileSystem follows the ContentManager bit (result %x)", title, r110);
    CHECK(denied + (!gc_held) + (!cm_held) >= 1, "%s: at least one forbidden mount is refused", title);
    drop(srv);
}
static void t_titles(void) {
    const char *root = getenv("NAPPLE_WORK") ? getenv("NAPPLE_WORK") : "/Users/neto/NappleWork/games";
    char saves[256];
    snprintf(saves, sizeof saves, "%s/risk_saves.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    CHECK(mkdtemp(saves) != NULL, "temp save root");
    static const char *titles[] = {"0100000000010000", "0100152000022000", "01006BB00C6F0000", "01006F8002326000"};
    unsigned built = 0;
    for (unsigned t = 0; t < 4; t++) {
        char tdir[300];
        snprintf(tdir, sizeof tdir, "%s/%s", root, titles[t]);
        DIR *d = opendir(tdir);
        if (!d) { printf("SKIP %s: not built under %s\n", titles[t], root); g_skipped++; continue; }
        struct dirent *e;
        while ((e = readdir(d))) {
            char bd[600];
            if (e->d_name[0] == '.' || strlen(e->d_name) != 16) continue;
            snprintf(bd, sizeof bd, "%s/%s", tdir, e->d_name);
            if (strcmp(e->d_name, "applets") && strcmp(e->d_name, "sysdata")) { static char keep[8][600]; static unsigned nk; char *k = keep[nk++ % 8]; snprintf(k, 600, "%s", bd); check_build(titles[t], k, saves); built++; }
        }
        closedir(d);
    }
    printf("  %u builds checked\n", built);
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", saves);
    if (system(cmd)) printf("cleanup failed\n");
}

/* ---- 3. acc with no linked Napple profile ------------------------------------------------------------------------------------------------------- */
static uint32_t async_result(IpcObject *o, unsigned cmd, const void *in, uint32_t n) {
    IpcCall c;
    svc_call(o, cmd, in, n, &c);
    if (c.result || c.nout_objects != 1) return 0xffffffffu;
    IpcCall d;
    svc_call(c.out_objects[0], 2, NULL, 0, &d);
    CHECK(d.result == 0 && d.out[0] == 1, "acc %u: the context is done at once", cmd);
    return ((AccAsync *)c.out_objects[0]->state)->result;
}
static void t_unlinked_acc(void) {
    online_provider_install(NULL); /* the NullProvider: no ONLINE_CAP_ACCOUNT, no linked Napple profile */
    IpcObject *u = acc_open();
    IpcCall c;
    svc_call(u, 101, g_user_id, 16, &c);
    CHECK(c.result == 0 && c.nout_objects == 1, "101 GetBaasAccountManagerForApplication");
    IpcObject *m = c.nout_objects ? c.out_objects[0] : NULL;
    CHECK(HZ_R_ACC_NSA_REGISTRATION_REQUIRED == 0x1907C, "the constant is 0x1907C");
    CHECK(async_result(u, 103, g_user_id, 16) == 0x1907C, "103 CheckNetworkServiceAvailabilityAsync(uid): 0x1907C");
    CHECK(async_result(u, 103, NULL, 0) == 0x1907C, "103 without a uid: 0x1907C");
    if (m) {
        CHECK(async_result(m, 170, NULL, 0) == 0x1907C, "170 LoadNetworkServiceLicenseKindAsync: 0x1907C");
        svc_call(m, 1, NULL, 0, &c);
        CHECK(c.result == 0 && SVC_OUT(&c, uint64_t, 0) == 0x4e61707070304e53ULL, "GetAccountId: the fixed local id, not a network account");
    }
    CHECK(acc_nsa_result(g_user_id) == 0x1907C, "acc_nsa_result for the local user: 0x1907C");
    uint8_t other[16] = {9};
    CHECK(acc_nsa_result(other) == 0x1907C, "...and for any other uid");
}

int main(int argc, char **argv) {
    if (getenv("RISK_PHASE")) return restarted_process(argc, argv);
    alarm(120);
    printf("risk_title: restart\n");
    t_restart(argv);
    printf("risk_title: fs permission bits on the real npdm files\n");
    t_titles();
    printf("risk_title: unlinked acc\n");
    t_unlinked_acc();
    printf("{\"test\":\"risk_title\",\"checked\":%d,\"failed\":%d,\"skipped\":%d}\n", g_checked, g_failed, g_skipped);
    return g_failed != 0;
}
