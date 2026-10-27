/* The system configuration file: round trip, environment precedence, malformed input. */
#include "../src/services/sysconfig_core.c"
#include "svc_test.h"
#include <unistd.h>

int main(void) {
    char dir[] = "/tmp/napple-sysconf-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "temp dir");
    SysConfig c, r;
    CHECK(sysconfig_load(dir, &r) == 1 && !r.amiibo_database[0], "no file: defaults, reported as absent");
    memset(&c, 0, sizeof c);
    snprintf(c.amiibo_database, sizeof c.amiibo_database, "https://example.invalid/a \"quoted\".json");
    snprintf(c.amiibo_key, sizeof c.amiibo_key, "/keys/key_retail.bin");
    snprintf(c.online_provider, sizeof c.online_provider, "/opt/prov\\ider.dylib");
    CHECK(sysconfig_save(dir, &c) == 0, "save");
    CHECK(sysconfig_load(dir, &r) == 0 && !strcmp(r.amiibo_database, c.amiibo_database) && !strcmp(r.amiibo_key, c.amiibo_key) &&
              !strcmp(r.online_provider, c.online_provider) && !r.mii_dir[0] && !r.amiibo_store[0], "round trip, escapes included");
    unsetenv("NAPPLE_AMIIBO_DB"); unsetenv("NAPPLE_AMIIBO_STORE"); unsetenv("NAPPLE_MII_DIR");
    setenv("NAPPLE_AMIIBO_KEY", "/from/env.bin", 1); unsetenv("NAPPLE_ONLINE_PROVIDER");
    CHECK(sysconfig_apply_env(&r) == 2, "two variables set (the environment wins over the file for the third)");
    CHECK(!strcmp(getenv("NAPPLE_AMIIBO_KEY"), "/from/env.bin") && !strcmp(getenv("NAPPLE_ONLINE_PROVIDER"), "/opt/prov\\ider.dylib") && !getenv("NAPPLE_MII_DIR"), "values land in the NAPPLE_* variables");
    char path[1200];
    sysconfig_path(dir, path, sizeof path);
    FILE *f = fopen(path, "w"); fputs("{broken", f); fclose(f);
    CHECK(sysconfig_load(dir, &r) == -1 && !r.amiibo_key[0], "a malformed file is reported and ignored");
    f = fopen(path, "w"); fputs("{\"amiibo_key\": \"/k\", \"future_key\": [1,2], \"mii_dir\": 5}", f); fclose(f);
    CHECK(sysconfig_load(dir, &r) == 0 && !strcmp(r.amiibo_key, "/k") && !r.mii_dir[0], "unknown keys and wrongly typed values are ignored");
    printf("{\"test\":\"svc_sysconfig\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
