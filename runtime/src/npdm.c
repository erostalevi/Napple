/* main.npdm: process metadata the kernel and the service manager honour (thread priority/core of the main thread, the list of
 * svcs the process may use, the services it may open). A game's main.npdm sits in its data directory. */
#include "napple.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Npdm g_npdm;

static uint32_t u32(const uint8_t *b, size_t o) { uint32_t v; memcpy(&v, b + o, 4); return v; }
static uint64_t u64(const uint8_t *b, size_t o) { uint64_t v; memcpy(&v, b + o, 8); return v; }

int npdm_load(const char *path) {
    memset(&g_npdm, 0, sizeof g_npdm);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t buf[0x4000];
    size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
    if (n < 0x80 || memcmp(buf, "META", 4)) return 0;
    uint32_t aci_off = u32(buf, 0x70), aci_len = u32(buf, 0x74);
    if ((uint64_t)aci_off + aci_len > n || aci_len < 0x40 || memcmp(buf + aci_off, "ACI0", 4)) return 0;
    const uint8_t *aci = buf + aci_off;
    memcpy(g_npdm.name, buf + 0x20, 15);
    g_npdm.is_64bit = buf[0xC] & 1; g_npdm.address_space = (buf[0xC] >> 1) & 7;
    g_npdm.main_priority = buf[0xE]; g_npdm.main_core = buf[0xF];
    g_npdm.system_resource_size = u32(buf, 0x14); g_npdm.version = u32(buf, 0x18); g_npdm.main_stack_size = u32(buf, 0x1C);
    g_npdm.title_id = u64(aci, 0x10);
    uint32_t fs_off = u32(aci, 0x20), sac_off = u32(aci, 0x28), sac_len = u32(aci, 0x2C), kac_off = u32(aci, 0x30), kac_len = u32(aci, 0x34);
    if ((uint64_t)fs_off + 12 <= aci_len) g_npdm.fs_perms = u64(aci, fs_off + 4);
    if ((uint64_t)sac_off + sac_len <= aci_len) {
        for (uint32_t i = 0; i < sac_len;) {
            uint8_t c = aci[sac_off + i]; unsigned len = (c & 7) + 1;
            if (i + 1 + len > sac_len || g_npdm.nservices == 64) break;
            memcpy(g_npdm.services[g_npdm.nservices], aci + sac_off + i + 1, len);
            g_npdm.service_host[g_npdm.nservices++] = c & 0x80 ? 1 : 0;
            i += 1 + len;
        }
    }
    if ((uint64_t)kac_off + kac_len <= aci_len) {
        for (uint32_t i = 0; i + 4 <= kac_len; i += 4) {
            uint32_t d = u32(aci, kac_off + i);
            if (d == 0xFFFFFFFFu) continue;
            unsigned ones = (unsigned)__builtin_ctz(~d);
            if (ones == 4) {
                unsigned idx = d >> 29; uint32_t mask = (d >> 5) & 0xFFFFFF;
                for (unsigned b = 0; b < 24; b++) if ((mask >> b & 1) && idx * 24 + b < 192) {
                    unsigned s = idx * 24 + b; g_npdm.svc_mask[s / 32] |= 1u << (s % 32); g_npdm.nsvc_allowed++;
                }
            } else if (ones == 16) g_npdm.handle_table_size = (uint16_t)((d >> 17) & 0x3FF);
        }
    }
    g_npdm.valid = 1;
    return 1;
}

int npdm_service_allowed(const char *name) {
    if (!g_npdm.valid) return 1;
    for (unsigned i = 0; i < g_npdm.nservices; i++) {
        const char *s = g_npdm.services[i];
        size_t n = strnlen(s, 12);
        /* names with a trailing '*' (prefix grants) are allowed by prefix */
        if (n && s[n - 1] == '*' ? !strncmp(name, s, n - 1) : !strncmp(name, s, 12)) return 1;
    }
    return 0;
}
