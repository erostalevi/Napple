/* Horizon result codes and BSD errno values used by the services (portable: no macOS or runtime dependency).
 * A result is module | description << 9. Module numbers and descriptions below are written down from the public error-code
 * documentation as the author remembers it; the ones marked [unverified] were not checked against a console or a title and are the
 * first thing to compare if a game reacts to them. */
#pragma once
#include <stdint.h>

#define HZ_RESULT(module, desc) ((uint32_t)(module) | ((uint32_t)(desc) << 9))
#define HZ_MODULE(r) ((uint32_t)(r) & 0x1ff)
#define HZ_DESC(r) (((uint32_t)(r) >> 9) & 0x1fff)

enum {
    HZ_MOD_NCM = 5,
    HZ_MOD_NS = 16,
    HZ_MOD_SETTINGS = 105,
    HZ_MOD_NIFM = 110,
    HZ_MOD_TIME = 116,
    HZ_MOD_NSD = 117,
    HZ_MOD_FRIENDS = 121,
    HZ_MOD_BCAT = 122,
    HZ_MOD_SSL = 123,
    HZ_MOD_APPLET = 128,
    HZ_MOD_MII = 148,
    HZ_MOD_HID = 202,
    HZ_MOD_LDN = 203,
    HZ_MOD_BTM = 143,
    HZ_MOD_AUDIO = 153,
    HZ_MOD_AUDIO_IN = HZ_MOD_AUDIO,
    HZ_MOD_IRSENSOR = 205,
    HZ_MOD_CAPTURE = 206,
    HZ_MOD_PSM = 133, /* [unverified] */
    HZ_MOD_LBL = 131, /* [unverified] */
    HZ_MOD_APM = 127, /* [unverified] */
};

/* Results the offline answers use. */
#define HZ_R_OK 0u
#define HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED HZ_RESULT(HZ_MOD_NIFM, 1111) /* [unverified] what GetResult reports when no connection can be made */
#define HZ_R_NIFM_PENDING_CONNECTION HZ_RESULT(HZ_MOD_NIFM, 111)
#define HZ_R_NCM_CONTENT_META_NOT_FOUND HZ_RESULT(HZ_MOD_NCM, 250)                     /* [unverified] */
#define HZ_R_NCM_CONTENT_NOT_FOUND HZ_RESULT(HZ_MOD_NCM, 310)                          /* [unverified] */
#define HZ_R_NS_APPLICATION_NOT_FOUND HZ_RESULT(HZ_MOD_NS, 400)                        /* [unverified] */
#define HZ_R_BCAT_NOT_FOUND HZ_RESULT(HZ_MOD_BCAT, 20)                                 /* [unverified] */
#define HZ_R_CAPTURE_NOT_FOUND HZ_RESULT(HZ_MOD_CAPTURE, 5)                            /* [unverified] */
#define HZ_R_CAPTURE_INVALID_ARGUMENT HZ_RESULT(HZ_MOD_CAPTURE, 2)                          /* [unverified] */
#define HZ_R_CAPTURE_SCREENSHOT_NOT_PERMITTED HZ_RESULT(HZ_MOD_CAPTURE, 1052)          /* [unverified] */
#define HZ_R_LDN_NETWORK_NOT_AVAILABLE HZ_RESULT(HZ_MOD_LDN, 22)                       /* [unverified] */
#define HZ_R_IRSENSOR_NOT_CONNECTED HZ_RESULT(HZ_MOD_IRSENSOR, 3)                      /* [unverified] */
#define HZ_R_AUDREC_BUFFER_COUNT_LIMIT HZ_RESULT(HZ_MOD_AUDIO, 8)                      /* [unverified] AppendFinalOutputRecorderBuffer past the queue limit */
#define HZ_R_AUDREC_BUFFER_NOT_FOUND HZ_RESULT(HZ_MOD_AUDIO, 8)                        /* [unverified] a tag that is not queued */
#define HZ_R_HID_NOT_CONNECTED HZ_RESULT(HZ_MOD_HID, 7)                                /* [unverified] */
#define HZ_R_FRIENDS_NETWORK_SERVICE_ACCOUNT_UNAVAILABLE HZ_RESULT(HZ_MOD_FRIENDS, 15) /* [unverified] */
#define HZ_R_SSL_NOT_CONNECTED HZ_RESULT(HZ_MOD_SSL, 205)                              /* [unverified] */

/* BSD errno values as the Horizon socket service reports them (FreeBSD numbering). */
enum {
    HZ_EPERM = 1,
    HZ_ENOENT = 2,
    HZ_EINTR = 4,
    HZ_EIO = 5,
    HZ_EBADF = 9,
    HZ_ENOMEM = 12,
    HZ_EACCES = 13,
    HZ_EFAULT = 14,
    HZ_EINVAL = 22,
    HZ_EMFILE = 24,
    HZ_EPIPE = 32,
    HZ_EAGAIN = 35,
    HZ_EINPROGRESS = 36,
    HZ_EALREADY = 37,
    HZ_ENOTSOCK = 38,
    HZ_EDESTADDRREQ = 39,
    HZ_EMSGSIZE = 40,
    HZ_EPROTOTYPE = 41,
    HZ_ENOPROTOOPT = 42,
    HZ_EPROTONOSUPPORT = 43,
    HZ_EOPNOTSUPP = 45,
    HZ_EAFNOSUPPORT = 47,
    HZ_EADDRINUSE = 48,
    HZ_EADDRNOTAVAIL = 49,
    HZ_ENETDOWN = 50,
    HZ_ENETUNREACH = 51,
    HZ_ECONNABORTED = 53,
    HZ_ECONNRESET = 54,
    HZ_ENOBUFS = 55,
    HZ_EISCONN = 56,
    HZ_ENOTCONN = 57,
    HZ_ETIMEDOUT = 60,
    HZ_ECONNREFUSED = 61,
    HZ_EHOSTUNREACH = 65,
};
/* getaddrinfo / netdb error numbers (FreeBSD numbering) */
enum { HZ_EAI_AGAIN = 2, HZ_EAI_FAIL = 4, HZ_EAI_NONAME = 8 };
enum { HZ_NETDB_HOST_NOT_FOUND = 1, HZ_NETDB_TRY_AGAIN = 2, HZ_NETDB_NO_RECOVERY = 3, HZ_NETDB_NO_DATA = 4 };
