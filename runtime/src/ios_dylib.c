#include "ios_dylib.h"
#include <stdio.h>
#include <string.h>

int ios_dylib_path(char *out, size_t cap, const char *images_dir, const char *file) {
    /* images_dir = <app>/game/<version>/modules or .../nro */
    char dir[4096];
    if (strlen(images_dir) >= sizeof dir) return -1;
    strcpy(dir, images_dir);
    char *slash = strrchr(dir, '/');
    if (!slash) return -1;
    int nro = !strcmp(slash + 1, "nro");
    *slash = 0; /* dir = <app>/game/<version> */
    char *ver = strrchr(dir, '/');
    if (!ver) return -1;
    const char *vname = ver + 1;
    char app_game[4096];
    snprintf(app_game, sizeof app_game, "%.*s", (int)(ver - dir), dir); /* <app>/game */
    char *g = strrchr(app_game, '/');
    if (!g) return -1;
    *g = 0; /* <app> */
    int n = snprintf(out, cap, "%s/Frameworks/%s.%s%s.dylib", app_game, vname, nro ? "nro." : "", file);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

typedef struct {
    uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
} MachHeader;
typedef struct {
    uint32_t cmd, cmdsize;
    char segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    uint32_t maxprot, initprot, nsects, flags;
} Seg64;

static const Seg64 *find_seg(const void *hdr, const char *name) {
    const MachHeader *h = hdr;
    const uint8_t *p = (const uint8_t *)hdr + sizeof *h;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        uint32_t cmd, size;
        memcpy(&cmd, p, 4);
        memcpy(&size, p + 4, 4);
        if (size < 8) return NULL;
        if (cmd == 0x19) {
            const Seg64 *s = (const Seg64 *)p;
            if (!strncmp(s->segname, name, 16)) return s;
        }
        p += size;
    }
    return NULL;
}

static uint64_t up(uint64_t v) { return (v + 0x3fff) & ~0x3fffULL; }

const char *ios_dylib_check(const void *hdr, uint64_t text_end, uint64_t data_off, uint64_t key_off, uint64_t patch_off, uint64_t file_size) {
    const MachHeader *h = hdr;
    if (h->magic != 0xfeedfacf || h->cputype != 0x0100000c || h->filetype != 6) return "not an arm64 Mach-O dylib";
    const Seg64 *text = find_seg(hdr, "__TEXT"), *data = find_seg(hdr, "__DATA"), *patch = find_seg(hdr, "__TEXT_EXEC");
    if (!text || !data || !patch) return "missing __TEXT, __DATA or __TEXT_EXEC segment";
    uint64_t base = text->vmaddr, rx = up(text_end), ro_end = data_off & ~0x3fffULL;
    if (text->initprot != 5 || text->vmsize < IOS_DYLIB_HEADER_PAGE + rx) return "__TEXT does not cover the module text";
    if (data->initprot != 3 || data->vmaddr - base != IOS_DYLIB_HEADER_PAGE + ro_end || data->vmaddr - base + data->vmsize != IOS_DYLIB_HEADER_PAGE + patch_off)
        return "__DATA does not span data..key page";
    if (key_off < ro_end || key_off + 0x4000 > patch_off) return "key page outside __DATA";
    if (patch->initprot != 5 || patch->vmaddr - base != IOS_DYLIB_HEADER_PAGE + patch_off || patch->vmsize != up(file_size) - patch_off)
        return "__TEXT_EXEC does not match the patch area";
    return NULL;
}
