/* pl:u: shared system fonts. The fonts are Nintendo system data, so they are NOT shipped: `napple build --dump-fonts <firmware> -k <keys>
 * -o <app-support>/system/fonts` extracts them from the user's own firmware (docs/runtime-services.md). The service lays them out in
 * a 0x1100000-byte shared memory block, each as {u32 0x18029A7F, u32 size ^ 0x49621806, TrueType/OpenType data}; the offsets given to the
 * guest point at the font data itself (libnx and the SDK pass that address straight to their font engines). Without the files the
 * service still answers with zero-size fonts so games start without text instead of failing. */
#include "../ipc.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PL_SHM_SIZE 0x1100000u
#define NFONT 6
/* SharedFontType (libnx PlSharedFontType): 0 standard (Japan/US/EU), 1 Chinese simplified, 2 Chinese simplified extension,
 * 3 Chinese traditional, 4 Korean, 5 Nintendo extension. Matched on the extracted file name. */
static const char *const font_match[NFONT] = {"_std_", "_org_zh-cn", "_ext_zh-cn", "zh-tw", "_ko_", "nintendo_ext_"};

static struct { int ready; IpcSharedMemory *shm; uint32_t offset[NFONT], size[NFONT]; unsigned loaded; } pl;

static void pl_load(void) {
    if (pl.ready) return;
    pl.ready = 1;
    pl.shm = ipc_shared_memory_new(PL_SHM_SIZE);
    char dir[1200]; snprintf(dir, sizeof dir, "%s/system/fonts", g_cfg.save_root);
    DIR *d = opendir(dir);
    if (!d) { LOG("pl", "no shared fonts in %s (run: napple build --dump-fonts <firmware.zip> -k <prod.keys> -o \"%s\"): games get zero-size fonts", dir, dir); return; }
    uint32_t pos = 0;
    for (struct dirent *e; (e = readdir(d));) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".ttf")) continue;
        int t = -1;
        for (int i = 0; i < NFONT; i++) if (strstr(e->d_name, font_match[i])) { t = i; break; }
        if (t < 0 || pl.size[t]) continue;
        char path[1500]; snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        int fd = open(path, O_RDONLY); struct stat st;
        if (fd < 0 || fstat(fd, &st) || st.st_size <= 0) { if (fd >= 0) close(fd); continue; }
        uint32_t size = (uint32_t)st.st_size;
        if ((uint64_t)pos + 8 + size > PL_SHM_SIZE) { close(fd); LOG("pl", "font %s does not fit the shared memory", e->d_name); continue; }
        uint8_t *dst = (uint8_t *)pl.shm->host + pos;
        uint32_t hdr[2] = {0x18029A7Fu, size ^ 0x49621806u}; memcpy(dst, hdr, 8);
        ssize_t r = read(fd, dst + 8, size); close(fd);
        if (r != (ssize_t)size) continue;
        pl.offset[t] = pos + 8; pl.size[t] = size; pl.loaded++;
        pos = (pos + 8 + size + 0xff) & ~0xffu;
        LOGV("pl", "font type %d <- %s (%u bytes at +%#x)", t, e->d_name, size, pl.offset[t]);
    }
    closedir(d);
    LOG("pl", "%u shared fonts loaded from %s", pl.loaded, dir);
    if (!pl.loaded) LOG("pl", "no usable fonts found: games get zero-size fonts");
}

static unsigned font_type(IpcCall *c) { unsigned t = IPC_IN(c, uint32_t, 0); if (t >= NFONT) { c->result = 0x1; return 0; } return t; }
static void pl_request_load(IpcObject *s, IpcCall *c) { (void)s; pl_load(); font_type(c); }
static void pl_load_state(IpcObject *s, IpcCall *c) { (void)s; pl_load(); font_type(c); uint32_t v = 1; IPC_OUT(c, v); }
static void pl_size(IpcObject *s, IpcCall *c) { (void)s; pl_load(); unsigned t = font_type(c); uint32_t v = pl.size[t]; IPC_OUT(c, v); }
static void pl_offset(IpcObject *s, IpcCall *c) { (void)s; pl_load(); unsigned t = font_type(c); uint32_t v = pl.offset[t]; IPC_OUT(c, v); }
static void pl_handle(IpcObject *s, IpcCall *c) { (void)s; pl_load(); ipc_out_copy_handle(c, ipc_shared_memory_handle(pl.shm)); }
/* GetSharedFontInOrderOfPriority(language) -> {loaded, count}, types[], offsets[], sizes[]: the fonts that exist, the language's own
 * font first (the SDK takes the first entry as the primary face). */
static void pl_in_order(IpcObject *s, IpcCall *c) {
    (void)s; pl_load();
    uint64_t lang = IPC_IN(c, uint64_t, 0); char code[9] = {0}; memcpy(code, &lang, 8);
    int first = !strncmp(code, "zh-Hans", 7) || !strncmp(code, "zh-CN", 5) ? 1 : !strncmp(code, "zh-Hant", 7) || !strncmp(code, "zh-TW", 5) ? 3 : !strncmp(code, "ko", 2) ? 4 : 0;
    uint32_t types[NFONT], offs[NFONT], sizes[NFONT]; unsigned n = 0;
    if (pl.size[first]) { types[n] = first; offs[n] = pl.offset[first]; sizes[n] = pl.size[first]; n++; }
    for (int t = 0; t < NFONT; t++) if (t != first && pl.size[t]) { types[n] = t; offs[n] = pl.offset[t]; sizes[n] = pl.size[t]; n++; }
    ipc_write_buf(c, ipc_out_buf(c, 0), types, n * 4ull); ipc_write_buf(c, ipc_out_buf(c, 1), offs, n * 4ull); ipc_write_buf(c, ipc_out_buf(c, 2), sizes, n * 4ull);
    uint32_t loaded = 1, count = n; IPC_OUT(c, loaded); IPC_OUT(c, count);
}
static const IpcCmd pl_cmds[] = {
    {0, "RequestLoad", pl_request_load, IPC_IMPL}, {1, "GetLoadState", pl_load_state, IPC_IMPL}, {2, "GetSize", pl_size, IPC_IMPL},
    {3, "GetSharedMemoryAddressOffset", pl_offset, IPC_IMPL}, {4, "GetSharedMemoryNativeHandle", pl_handle, IPC_IMPL},
    {5, "GetSharedFontInOrderOfPriority", pl_in_order, IPC_IMPL}, {6, "GetSharedFontInOrderOfPriorityForSystem", pl_in_order, IPC_IMPL},
};
static IpcObject *pl_open(void) { return IPC_OBJECT("pl:u", pl_cmds, NULL, NULL); }
void service_register_pl(void) { ipc_register_service("pl:u", pl_open); ipc_register_service("pl:s", pl_open); }
