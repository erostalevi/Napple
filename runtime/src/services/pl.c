/* pl:u: shared system fonts. The fonts are Nintendo system data, so they are NOT shipped: `napple build --dump-fonts <firmware> -k <keys>
 * -o <app-support>/system/fonts` extracts them from the user's own firmware (docs/runtime-services.md). The service lays them out in
 * a 0x1100000-byte shared memory block, each as {u32 0x18029A7F, u32 size ^ 0x49621806, TrueType/OpenType data}; the offsets given to the
 * guest point at the font data itself (libnx and the SDK pass that address straight to their font engines). Without the files the
 * service serves a tiny placeholder TrueType font (one empty glyph) for every type, so the guest never gets a zero-size or null font
 * (that crashed the module init) and text simply renders blank. */
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

/* Minimal valid sfnt: OS/2, cmap (format 4, no mappings), glyf (one empty .notdef), head, hhea, hmtx, loca, maxp, name, post. Big-endian. */
static uint32_t pl_placeholder(uint8_t *out) {
    enum { NT = 10 };
    static const char *const tag[NT] = {"OS/2", "cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp", "name", "post"};
    uint8_t tb[NT][96] = {{0}}; uint32_t len[NT] = {0};
    #define W16(t, o, v) (tb[t][o] = (uint8_t)((v) >> 8), tb[t][(o) + 1] = (uint8_t)(v))
    #define W32(t, o, v) (W16(t, o, (v) >> 16), W16(t, (o) + 2, (v) & 0xffff))
    /* cmap: version 0, 1 subtable (3,1) at 12; format 4 with one segment 0xFFFF->glyph 0 */
    W16(1, 2, 1); W16(1, 4, 3); W16(1, 6, 1); W32(1, 8, 12);
    W16(1, 12, 4); W16(1, 14, 24); W16(1, 18, 2); W16(1, 20, 2); W16(1, 22, 0); W16(1, 24, 0xFFFF); W16(1, 28, 0xFFFF); W16(1, 30, 1); W16(1, 32, 0);
    len[1] = 36;
    len[2] = 0;                                            /* glyf: empty glyph has zero length */
    W32(3, 0, 0x00010000); W32(3, 12, 0x5F0F3CF5); W16(3, 18, 1000); W16(3, 36, 0); W16(3, 38, 0); W16(3, 40, 1000); W16(3, 42, 1000); W16(3, 46, 8); W16(3, 48, 2); /* head: unitsPerEm 1000, short loca (format 0) */
    len[3] = 54;
    W32(4, 0, 0x00010000); W16(4, 4, 800); W16(4, 6, (uint16_t)-200); W16(4, 10, 1000); W16(4, 34, 1); len[4] = 36; /* hhea: 1 hMetric */
    W16(5, 0, 500); len[5] = 4;                            /* hmtx: advance 500, lsb 0 */
    len[6] = 4;                                            /* loca (short): offsets 0, 0 */
    W32(7, 0, 0x00010000); W16(7, 4, 1); len[7] = 32;      /* maxp v1.0: 1 glyph */
    W16(0, 0, 3); W16(0, 2, 500); W16(0, 4, 400); W16(0, 6, 5); W16(0, 62, 0x20); W16(0, 64, 0xFFFF); W16(0, 68, 800); W16(0, 70, (uint16_t)-200); len[0] = 78; /* OS/2 v3 */
    W16(8, 4, 6); len[8] = 6;                 /* name: no records */
    W32(9, 0, 0x00030000); len[9] = 32;                                 /* post v3: no glyph names */
    uint32_t pos = 12 + 16 * NT, off[NT];
    for (int i = 0; i < NT; i++) { off[i] = pos; pos += (len[i] + 3) & ~3u; }
    memset(out, 0, pos);
    out[1] = 1; out[5] = NT; out[7] = 0x80; out[9] = 3; out[11] = 0x20;
    for (int i = 0; i < NT; i++) {
        uint8_t *r = out + 12 + 16 * i; memcpy(r, tag[i], 4);
        r[9] = off[i] >> 16; r[10] = off[i] >> 8; r[11] = off[i]; r[14] = len[i] >> 8; r[15] = len[i];
        memcpy(out + off[i], tb[i], len[i]);
    }
    return pos;
}

static void pl_use_placeholder(uint32_t pos) {
    uint8_t ttf[512]; uint32_t size = pl_placeholder(ttf);
    for (int t = 0; t < NFONT; t++) {   /* one copy per type: the guest treats the six faces as separate blocks */
        uint8_t *dst = (uint8_t *)pl.shm->host + pos;
        uint32_t hdr[2] = {0x18029A7Fu, size ^ 0x49621806u}; memcpy(dst, hdr, 8); memcpy(dst + 8, ttf, size);
        pl.offset[t] = pos + 8; pl.size[t] = size;
        pos = (pos + 8 + size + 0xff) & ~0xffu;
    }
    LOG("pl", "no usable shared fonts: serving a %u-byte blank placeholder font (games show no text). Dump the real ones with napple build --dump-fonts", size);
}

static void pl_load(void) {
    if (pl.ready) return;
    pl.ready = 1;
    pl.shm = ipc_shared_memory_new(PL_SHM_SIZE);
    char dir[1200]; snprintf(dir, sizeof dir, "%s/system/fonts", g_cfg.save_root);
    DIR *d = opendir(dir);
    if (!d) { LOG("pl", "no shared fonts in %s (run: napple build --dump-fonts <firmware.zip> -k <prod.keys> -o \"%s\"): using a blank placeholder font, games show no text", dir, dir); pl_use_placeholder(0); return; }
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
    if (!pl.loaded) pl_use_placeholder(pos);
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
