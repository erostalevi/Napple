#include "sfdnsres_core.h"

const char *sfdns_hstrerror(unsigned e) {
    static const char *const t[] = {"Resolver Error 0 (no error)", "Unknown host", "Host name lookup failure", "Unknown server error", "No address associated with name"};
    return e < sizeof t / sizeof t[0] ? t[e] : "Unknown resolver error";
}
const char *sfdns_gai_strerror(unsigned e) {
    static const char *const t[] = {
        "Success", "Address family for hostname not supported", "Temporary failure in name resolution", "Invalid value for ai_flags",
        "Non-recoverable failure in name resolution", "ai_family not supported", "Memory allocation failure", "No address associated with hostname",
        "hostname nor servname provided, or not known", "servname not supported for ai_socktype", "ai_socktype not supported", "System error returned in errno",
        "Invalid value for hints", "Resolved protocol is unknown", "Argument buffer overflow",
    };
    return e < sizeof t / sizeof t[0] ? t[e] : "Unknown error";
}
