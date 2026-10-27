/* napple-launcher: Contents/MacOS/<launcher> of a built Napple game bundle.
 *
 * Runs with no arguments (Finder / `open`): finds the game data next to itself and execs the sibling `napple-runtime`:
 *     Contents/MacOS/napple-runtime <Contents/Resources/game/<version dir>> [extra args]
 * Version dir = $NAPPLE_VERSION, else the first line of Contents/Resources/game/default (the newest embedded version).
 * Extra arguments are passed through (`--headless --seconds 20` for boot tests); Finder's `-psn_*` argument is dropped.
 *
 * Shared system fonts: the runtime's pl:u looks in <save-root>/system/fonts (default ~/Library/Application Support/Napple).
 * On start the launcher installs the bundle's Resources/game/system/fonts/<name>.ttf there when missing or of a different size
 * (clone when possible). No game data is ever written outside the bundle except those fonts.
 * No dependencies beyond libSystem; keep it tiny and free of game knowledge. */
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATHLEN 4096

static int fail(const char *msg, const char *arg) {
    fprintf(stderr, "napple-launcher: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    return 1;
}

static int safe_component(const char *s) {
    return s[0] && !strchr(s, '/') && strcmp(s, ".") && strcmp(s, "..");
}

static void mkdir_p(const char *path) {
    char tmp[PATHLEN];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

static void install_fonts(const char *game, const char *save_root) {
    char src_dir[PATHLEN], dst_dir[PATHLEN];
    snprintf(src_dir, sizeof src_dir, "%s/system/fonts", game);
    snprintf(dst_dir, sizeof dst_dir, "%s/system/fonts", save_root);
    DIR *d = opendir(src_dir);
    if (!d) return;
    int made = 0;
    for (struct dirent *e; (e = readdir(d));) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".ttf") || !safe_component(e->d_name)) continue;
        char s[PATHLEN], t[PATHLEN], tmp[PATHLEN];
        snprintf(s, sizeof s, "%s/%s", src_dir, e->d_name);
        snprintf(t, sizeof t, "%s/%s", dst_dir, e->d_name);
        struct stat a, b;
        if (stat(s, &a) || !S_ISREG(a.st_mode)) continue;
        if (!stat(t, &b) && b.st_size == a.st_size) continue;
        if (!made) { mkdir_p(dst_dir); made = 1; }
        snprintf(tmp, sizeof tmp, "%s.%d.tmp", t, (int)getpid());
        if (copyfile(s, tmp, NULL, COPYFILE_DATA | COPYFILE_CLONE) == 0) rename(tmp, t); else unlink(tmp);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    char self[PATHLEN], real[PATHLEN];
    uint32_t sz = sizeof self;
    if (_NSGetExecutablePath(self, &sz) || !realpath(self, real)) return fail("cannot resolve the executable path", NULL);
    char *slash = strrchr(real, '/');
    if (!slash) return fail("bad executable path", real);
    *slash = 0; /* real = .../Contents/MacOS */

    char runtime[PATHLEN], game[PATHLEN], contents[PATHLEN];
    snprintf(runtime, sizeof runtime, "%s/napple-runtime", real);
    snprintf(contents, sizeof contents, "%s/..", real);
    snprintf(game, sizeof game, "%s/../Resources/game", real);

    char version[256] = {0};
    const char *env = getenv("NAPPLE_VERSION");
    if (env && *env) snprintf(version, sizeof version, "%s", env);
    else {
        char dpath[PATHLEN];
        snprintf(dpath, sizeof dpath, "%s/default", game);
        FILE *f = fopen(dpath, "r");
        if (!f) return fail("missing Resources/game/default (is this a Napple game bundle?)", dpath);
        if (!fgets(version, sizeof version, f)) version[0] = 0;
        fclose(f);
        version[strcspn(version, "\r\n")] = 0;
    }
    if (!safe_component(version)) return fail("invalid version directory name", version);

    char data[PATHLEN];
    snprintf(data, sizeof data, "%s/%s", game, version);
    struct stat st;
    char manifest[PATHLEN];
    snprintf(manifest, sizeof manifest, "%s/manifest.json", data);
    if (stat(manifest, &st)) return fail("game data directory not found", data);

    const char *save_root = NULL;
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--save-root")) save_root = argv[i + 1];
    /* System data (fonts, Mii, amiibo) has one root for every game: NAPPLE_SUPPORT_DIR or the default, never --save-root. */
    char root[PATHLEN];
    const char *support = getenv("NAPPLE_SUPPORT_DIR");
    if (!support || !*support) {
        const char *home = getenv("HOME");
        snprintf(root, sizeof root, "%s/Library/Application Support/Napple", home ? home : ".");
        support = root;
    }
    (void)save_root;
    install_fonts(game, support);

    setenv("NAPPLE_BUNDLE_DIR", contents, 1);
    char **args = calloc((size_t)argc + 3, sizeof *args);
    if (!args) return 1;
    int n = 0;
    args[n++] = runtime;
    args[n++] = data;
    for (int i = 1; i < argc; i++) if (strncmp(argv[i], "-psn_", 5)) args[n++] = argv[i];
    args[n] = NULL;
    execv(runtime, args);
    return fail("cannot execute the runtime", strerror(errno));
}
