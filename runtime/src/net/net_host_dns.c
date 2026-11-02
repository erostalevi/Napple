/* Host provider, sfdnsres: names resolve through the Mac's own resolver (getaddrinfo / getnameinfo), so /etc/hosts, the configured DNS servers and
 * any VPN or split-DNS setup apply exactly as for other host programs. Nothing is rewritten: a name the guest builds from a template it never filled
 * in (nsd answers names unchanged) simply does not resolve. */
#include "net_host.h"
#include "hz_result.h"
#include "offline_core.h"
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>

/* Darwin's EAI_* are FreeBSD's numbers (EAGAIN 2, BADFLAGS 3, FAIL 4, FAMILY 5, MEMORY 6, NONAME 8, SERVICE 9, SOCKTYPE 10, SYSTEM 11, OVERFLOW 14),
 * except the obsolete EAI_NODATA (7), which FreeBSD folded into EAI_NONAME. */
int nh_eai(int e) {
    switch (e) {
        case 0: return 0;
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
        case EAI_NODATA: return HZ_EAI_NONAME;
#endif
        case EAI_AGAIN: return HZ_EAI_AGAIN;
        case EAI_FAIL: return HZ_EAI_FAIL;
        case EAI_NONAME: return HZ_EAI_NONAME;
        default: return e > 0 && e <= 14 ? e : HZ_EAI_FAIL;
    }
}

int nh_dns_resolve(void *ctx, const char *host, const char *service, int family, OnlineResolved *out) {
    (void)ctx;
    memset(out, 0, sizeof *out);
    if ((!host || !*host) && (!service || !*service)) return HZ_EAI_NONAME;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = family == BSD_AF_INET ? AF_INET : family == BSD_AF_INET6 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM; /* one entry per address (the guest's own hints only filter the same list) */
    hints.ai_flags = host && *host ? AI_CANONNAME : AI_PASSIVE;
    int tok;
    nh_core_away(&tok); /* the resolver may wait on the network */
    int rc = getaddrinfo(host && *host ? host : NULL, service && *service ? service : NULL, &hints, &res);
    nh_core_back(&tok);
    if (rc) return nh_eai(rc);
    for (struct addrinfo *ai = res; ai && out->count < ONLINE_MAX_ADDRS; ai = ai->ai_next) {
        OnlineAddr a;
        nh_addr_from_host(ai->ai_addr, &a);
        if (!a.family) continue;
        int dup = 0;
        for (uint32_t i = 0; i < out->count && !dup; i++) dup = !memcmp(&out->addrs[i], &a, sizeof a);
        if (!dup) out->addrs[out->count++] = a;
        if (ai->ai_canonname && !out->canonname[0]) {
            strncpy(out->canonname, ai->ai_canonname, sizeof out->canonname - 1);
        }
    }
    freeaddrinfo(res);
    return out->count ? 0 : HZ_EAI_NONAME;
}

int nh_dns_reverse(void *ctx, const OnlineAddr *a, char *host, size_t n) {
    (void)ctx;
    struct sockaddr_storage ss;
    socklen_t len;
    if (!n || nh_addr_to_host(a, &ss, &len)) return HZ_EAI_FAIL;
    int tok;
    nh_core_away(&tok);
    int rc = getnameinfo((struct sockaddr *)&ss, len, host, (socklen_t)n, NULL, 0, NI_NAMEREQD);
    nh_core_back(&tok);
    return nh_eai(rc);
}
