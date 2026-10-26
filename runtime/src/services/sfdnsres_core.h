/* sfdnsres core: resolver error strings (portable). Text follows the FreeBSD netdb strings the Horizon resolver returns. */
#pragma once
const char *sfdns_hstrerror(unsigned netdb_error);  /* GetHostStringErrorRequest */
const char *sfdns_gai_strerror(unsigned gai_error); /* GetGaiStringErrorRequest */

/* ---- serialised results (the resolver client library's wire format; field order and big-endian numbers are from the author's memory of the
 * public description, unverified on a console) ------------------------------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>
#include "online_provider.h"
#define SFDNS_ADDRINFO_MAGIC 0xBEEFCAFEu
/* addrinfo list: per entry u32 magic, s32 flags, s32 family, s32 socktype, s32 protocol, u32 addrlen, sockaddr (sin: u16 family, u16 port,
 * u32 address, 8 zero bytes; sin6: u16 family, u16 port, u32 flow, 16 bytes, u32 scope), canonical name with its NUL; the list ends with a u32 0.
 * Returns the bytes written, or 0 when `cap` is too small (the caller reports EAI_OVERFLOW). socktype 0 yields one entry per address and kind
 * the way getaddrinfo does for an empty hint: stream, then datagram. */
size_t sfdns_serialize_addrinfo(const OnlineResolved *r, int flags, int socktype, int protocol, uint8_t *out, size_t cap);
/* Reads the first entry of a serialised hints buffer. Returns 0, or -1 for a missing or malformed buffer (the hints then default to all zero). */
int sfdns_parse_hints(const uint8_t *buf, size_t n, int *flags, int *family, int *socktype, int *protocol);
/* hostent: name NUL, u32 alias count (0), u16 addrtype, u16 addrlen, u32 address count, the addresses. 0 when `cap` is too small. */
size_t sfdns_serialize_hostent(const char *name, const OnlineResolved *r, uint8_t *out, size_t cap);
/* Numeric service ("80") to a port; 0 when the string is empty or not a number. */
uint16_t sfdns_port_from_service(const char *service);
/* Provider error (HZ_EAI_*) to the netdb error of the hostent calls: AGAIN -> TRY_AGAIN, NONAME -> HOST_NOT_FOUND, anything else NO_RECOVERY. */
unsigned sfdns_eai_to_netdb(int eai);
