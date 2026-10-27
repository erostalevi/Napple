/* One system data root: Mii database, fonts and the applet work dir follow g_cfg.support_dir, never a per-run --save-root. */
#include "../src/services/mii.c"
#include "svc_test.h"
#include <unistd.h>
#ifdef __linux__
const char *host_user_name(void) { return "Tester"; }
#endif

int main(void) {
    char sup[] = "/tmp/napple-sysroot-XXXXXX", saves[] = "/tmp/napple-sysroot-saves-XXXXXX";
    CHECK(mkdtemp(sup) && mkdtemp(saves), "temp roots");
    unsetenv("NAPPLE_MII_DIR");
    g_cfg.save_root = saves;
    CHECK(napple_support_dir() == saves, "without a support dir the legacy save root is used (tests, iOS)");
    g_cfg.support_dir = sup;
    CHECK(napple_support_dir() == sup, "support dir wins over --save-root");
    IpcObject *root = mii_open();
    IpcCall c;
    svc_call(root, 0, "\0\0\0\0", 4, &c);
    IpcObject *db = c.out_objects[0];
    uint32_t rnd[3] = {0, MII_GENDER_FEMALE, 0};
    svc_call(db, 6, rnd, 12, &c); /* BuildRandom touches the database, which opens its file */
    uint32_t src = 1;
    svc_call(db, 2, &src, 4, &c);
    char path[1500];
    snprintf(path, sizeof path, "%s/system/mii/MiiDatabase.dat", sup);
    CHECK(access(path, F_OK) == 0, "MiiDatabase.dat lives under <support>/system/mii");
    snprintf(path, sizeof path, "%s/system", saves);
    CHECK(access(path, F_OK) != 0, "nothing system-level under the per-run save root");
    printf("{\"test\":\"svc_sysroot\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
