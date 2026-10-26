#include "sfdnsres_core.h"

const char *sfdns_hstrerror(unsigned e) {
    static const char *const t[] = {"Resolver Error 0 (no error)", "Unknown host", "Host name lookup failure", "Unknown server error",
                                    "No address associated with name"};
    return e < sizeof t / sizeof t[0] ? t[e] : "Unknown resolver error";
}
const char *sfdns_gai_strerror(unsigned e) {
    static const char *const t[] = {
        "Success",
        "Address family for hostname not supported",
        "Temporary failure in name resolution",
        "Invalid value for ai_flags",
        "Non-recoverable failure in name resolution",
        "ai_family not supported",
        "Memory allocation failure",
        "No address associated with hostname",
        "hostname nor servname provided, or not known",
        "servname not supported for ai_socktype",
        "ai_socktype not supported",
        "System error returned in errno",
        "Invalid value for hints",
        "Resolved protocol is unknown",
        "Argument buffer overflow",
    };
    return e < sizeof t / sizeof t[0] ? t[e] : "Unknown error";
}

#include <string.h>
static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

size_t sfdns_serialize_addrinfo(const OnlineResolved *r, int flags, int socktype, int protocol, uint8_t *out, size_t cap) {
    size_t o = 0;
    unsigned kinds = socktype ? 1 : 2;
    for (uint32_t i = 0; i < r->count && i < ONLINE_MAX_ADDRS; i++)
        for (unsigned k = 0; k < kinds; k++) {
            const OnlineAddr *a = &r->addrs[i];
            int st = socktype ? socktype : (k == 0 ? 1 : 2), pr = protocol ? protocol : (st == 1 ? 6 : 17);
            int v6 = a->family == 28;
            size_t alen = v6 ? 28 : 16, cn = (i == 0 && k == 0) ? strlen(r->canonname) : 0;
            size_t need = 24 + alen + cn + 1;
            if (o + need + 4 > cap) return 0;
            be32(out + o, SFDNS_ADDRINFO_MAGIC);
            be32(out + o + 4, (uint32_t)flags);
            be32(out + o + 8, v6 ? 28u : 2u);
            be32(out + o + 12, (uint32_t)st);
            be32(out + o + 16, (uint32_t)pr);
            be32(out + o + 20, (uint32_t)alen);
            uint8_t *sa = out + o + 24;
            memset(sa, 0, alen);
            be16(sa, v6 ? 28 : 2);
            be16(sa + 2, a->port);
            if (v6) memcpy(sa + 8, a->addr, 16);
            else memcpy(sa + 4, a->addr, 4);
            memcpy(sa + alen, r->canonname, cn);
            sa[alen + cn] = 0;
            o += need;
        }
    if (o + 4 > cap) return 0;
    be32(out + o, 0);
    return o + 4;
}
int sfdns_parse_hints(const uint8_t *buf, size_t n, int *flags, int *family, int *socktype, int *protocol) {
    *flags = *family = *socktype = *protocol = 0;
    if (!buf || n < 20 || rd32(buf) != SFDNS_ADDRINFO_MAGIC) return -1;
    *flags = (int)rd32(buf + 4);
    *family = (int)rd32(buf + 8);
    *socktype = (int)rd32(buf + 12);
    *protocol = (int)rd32(buf + 16);
    return 0;
}
size_t sfdns_serialize_hostent(const char *name, const OnlineResolved *r, uint8_t *out, size_t cap) {
    size_t nl = strlen(name) + 1;
    uint32_t cnt = r->count > ONLINE_MAX_ADDRS ? ONLINE_MAX_ADDRS : r->count;
    int v6 = cnt && r->addrs[0].family == 28;
    size_t al = v6 ? 16 : 4, need = nl + 4 + 2 + 2 + 4 + cnt * al;
    if (need > cap) return 0;
    size_t o = 0;
    memcpy(out, name, nl);
    o += nl;
    be32(out + o, 0);
    o += 4;
    be16(out + o, v6 ? 28 : 2);
    be16(out + o + 2, (uint16_t)al);
    be32(out + o + 4, cnt);
    o += 8;
    for (uint32_t i = 0; i < cnt; i++, o += al) memcpy(out + o, r->addrs[i].addr, al);
    return o;
}
uint16_t sfdns_port_from_service(const char *s) {
    if (!s || !*s) return 0;
    unsigned v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (unsigned)(*s - '0');
        if (v > 65535) return 0;
    }
    return (uint16_t)v;
}
unsigned sfdns_eai_to_netdb(int e) {
    switch (e) {
        case 0: return 0;
        case 2: return 2;  /* EAI_AGAIN -> TRY_AGAIN */
        case 8: return 1;  /* EAI_NONAME -> HOST_NOT_FOUND */
        default: return 3; /* NO_RECOVERY */
    }
}
