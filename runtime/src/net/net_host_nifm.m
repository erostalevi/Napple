/* Host provider, nifm: the console's network interface mirrors the Mac's real connectivity. The primary IPv4 service (SystemConfiguration's dynamic
 * store: State:/Network/Global/IPv4 and /DNS) gives the interface, router and DNS servers; getifaddrs gives its address and mask. The guest sees a wired-
 * style connection (no SSID, full strength) whenever the Mac has a primary IPv4 interface with an address, and "disconnected" otherwise. Answers are
 * cached for a second (titles poll the connection status every frame). */
#include "net_host.h"
#include "hz_result.h"
#include <SystemConfiguration/SystemConfiguration.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>

void log_line(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static uint32_t ip_of(CFStringRef s) {
    char buf[64];
    struct in_addr a;
    if (!s || CFGetTypeID(s) != CFStringGetTypeID() || !CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return 0;
    return inet_pton(AF_INET, buf, &a) == 1 ? ntohl(a.s_addr) : 0;
}

static void query(OnlineNetInfo *o) {
    memset(o, 0, sizeof *o);
    SCDynamicStoreRef st = SCDynamicStoreCreate(NULL, CFSTR("napple-net-host"), NULL, NULL);
    if (!st) return;
    char ifname[IFNAMSIZ] = {0};
    CFDictionaryRef g4 = SCDynamicStoreCopyValue(st, CFSTR("State:/Network/Global/IPv4"));
    if (g4 && CFGetTypeID(g4) == CFDictionaryGetTypeID()) {
        CFStringRef pi = CFDictionaryGetValue(g4, CFSTR("PrimaryInterface"));
        if (pi && CFGetTypeID(pi) == CFStringGetTypeID()) CFStringGetCString(pi, ifname, sizeof ifname, kCFStringEncodingUTF8);
        o->gateway = ip_of(CFDictionaryGetValue(g4, CFSTR("Router")));
    }
    if (g4) CFRelease(g4);
    CFDictionaryRef dns = SCDynamicStoreCopyValue(st, CFSTR("State:/Network/Global/DNS"));
    if (dns && CFGetTypeID(dns) == CFDictionaryGetTypeID()) {
        CFArrayRef servers = CFDictionaryGetValue(dns, CFSTR("ServerAddresses"));
        if (servers && CFGetTypeID(servers) == CFArrayGetTypeID()) {
            for (CFIndex i = 0; i < CFArrayGetCount(servers) && !o->dns2; i++) {
                uint32_t a = ip_of(CFArrayGetValueAtIndex(servers, i)); /* IPv6 servers are skipped: nifm reports IPv4 */
                if (!a) continue;
                if (!o->dns1) o->dns1 = a;
                else o->dns2 = a;
            }
        }
    }
    if (dns) CFRelease(dns);
    CFRelease(st);
    if (!ifname[0]) return;
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || strcmp(i->ifa_name, ifname) || !(i->ifa_flags & IFF_UP)) continue;
        o->ip = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        if (i->ifa_netmask) o->mask = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        break;
    }
    freeifaddrs(ifs);
    if (o->ip) {
        o->connected = 1;
        o->kind = 2; /* wired: there is no SSID or signal to report */
        o->strength = 3;
    }
}

static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static OnlineNetInfo cache;
static int64_t cache_ms = -1;
static int last_connected = -1;
int nh_net_info(void *ctx, OnlineNetInfo *out) {
    (void)ctx;
    pthread_mutex_lock(&cache_lock);
    int64_t now = nh_now_ms();
    if (cache_ms < 0 || now - cache_ms >= 1000) {
        query(&cache);
        cache_ms = now;
        if (cache.connected != last_connected) {
            last_connected = cache.connected;
            if (cache.connected)
                log_line("net", "host provider: nifm connected, ip %u.%u.%u.%u", cache.ip >> 24, cache.ip >> 16 & 255, cache.ip >> 8 & 255, cache.ip & 255);
            else log_line("net", "host provider: nifm reports no connection (the Mac has no primary IPv4 interface)");
        }
    }
    *out = cache;
    pthread_mutex_unlock(&cache_lock);
    return 0;
}
uint32_t nh_net_connect(void *ctx) {
    OnlineNetInfo ni;
    nh_net_info(ctx, &ni);
    return ni.connected ? 0 : HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED; /* no interface to bring up: the same answer as a console without a network */
}
void nh_net_release(void *ctx) { (void)ctx; }
