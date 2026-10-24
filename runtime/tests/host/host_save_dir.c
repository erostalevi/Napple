/* host_info.c's host_save_dir for host tests (host_info.c itself needs CoreFoundation): <save-root>/<titleid>/save. */
#include "napple.h"
#include <stdio.h>
#include <sys/stat.h>
const char *host_save_dir(void) {
    static char dir[1100];
    char a[1024]; mkdir(g_cfg.save_root, 0755);
    snprintf(a, sizeof a, "%s/%s", g_cfg.save_root, g_cfg.title_id); mkdir(a, 0755);
    snprintf(dir, sizeof dir, "%s/save", a); mkdir(dir, 0755);
    return dir;
}
