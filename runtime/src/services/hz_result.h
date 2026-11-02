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
#define HZ_R_SSL_INVALID_ARGUMENT HZ_RESULT(HZ_MOD_SSL, 2)                             /* [unverified] a bad option, size, format or a second client cert */
#define HZ_R_SSL_NOT_FOUND HZ_RESULT(HZ_MOD_SSL, 3)                                    /* [unverified] an unknown CRL id, certificate id or index */

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
/* acc (module 124), switchbrew 'Error codes': 200 NetworkServiceAccountRegistrationRequired (0x1907C), the user has no network service account. That acc and
 * olsc report exactly this code for each network operation of an unlinked user is [unverified]. */
#define HZ_R_ACC_NSA_REGISTRATION_REQUIRED HZ_RESULT(124, 200)
/* ipc-depth-95 r10 (nifm network profiles). No public value is known for these two: they are placed in the nifm module so a title sees a nifm failure;
 * the descriptions are [unverified] placeholders. */
#define HZ_R_NIFM_NETWORK_PROFILE_NOT_FOUND HZ_RESULT(HZ_MOD_NIFM, 3400) /* [unverified] GetNetworkProfile / RemoveNetworkProfile of an unknown uuid */
#define HZ_R_NIFM_NETWORK_PROFILE_FULL HZ_RESULT(HZ_MOD_NIFM, 3401)      /* [unverified] SetNetworkProfile with every profile slot taken */

/* ipc-depth-95 r10 (applet part): am results from switchbrew 'Error codes' (module 128 rows, value and description as listed there), the lr result
 * switchbrew's PrepareForJit note quotes, and the ovln / ec answers napple picked where no public code exists. */
#define HZ_R_AM_TITLE_NOT_FOUND HZ_RESULT(HZ_MOD_APPLET, 37)           /* 0x4A80 "Title-ID not found" (listed for applet launches; [unverified] for applications) */
#define HZ_R_AM_INVALID_PARAMETERS HZ_RESULT(HZ_MOD_APPLET, 506)       /* 0x3F480 "Invalid parameters" */
#define HZ_R_AM_DEBUG_MODE_NOT_ENABLED HZ_RESULT(HZ_MOD_APPLET, 974)   /* 0x79C80 "DebugMode not enabled" */
#define HZ_R_LR_PROGRAM_NOT_FOUND HZ_RESULT(8, 2)                      /* 0x408: what PrepareForJit returns when the jit sysmodule does not exist */
#define HZ_R_OVLN_QUEUE_FULL HZ_RESULT(25, 2)                          /* [unverified] ovln (module 25) Send into a full queue with OverflowOption Error */
#define HZ_R_EC_NETWORK_SERVICE_ACCOUNT_UNAVAILABLE HZ_RESULT(124, 280) /* [unverified] account: the user has no linked Nintendo account */
