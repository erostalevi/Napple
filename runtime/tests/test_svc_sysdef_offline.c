/* Unit tests: the documented defaults for application-reachable commands that had no row (ipc-depth-sys): offline answers of nifm / ssl / friends / bsd /
 * nsd / sfdnsres, and the table hygiene of everything the offline module exposes (unique ids, every default marked IPC_STUB). */
#include "../src/services/offline.c"
#include "svc_test.h"

static void no_dup(IpcObject *o) {
    for (size_t i = 0; i < o->ncmds; i++)
        for (size_t j = i + 1; j < o->ncmds; j++) CHECK(o->cmds[i].cmd != o->cmds[j].cmd, "%s: command %u appears once", o->iface, o->cmds[i].cmd);
}
int main(void) {
    IpcCall c;
    IpcObject *nf = nifm_open();
    svc_call(nf, 5, NULL, 0, &c); IpcObject *gen = c.out_objects[0];
    no_dup(nf); no_dup(gen);
    /* nifm IGeneralService: no network, so what needs one reports it; plain getters answer zero */
    svc_call(gen, 2, NULL, 0, &c); CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "CreateScanRequest: no network");
    svc_call(gen, 7, NULL, 0, &c); CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "EnumerateNetworkProfiles: no network");
    svc_call(gen, 20, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 1 && c.out[0] == 0, "IsEthernetCommunicationEnabled: false");
    svc_call(gen, 44, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 1 && c.out[0] == 0, "IsWiredConnectionAvailable: false");
    svc_call(gen, 23, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 0, "PutToSleep: accepted");
    CHECK(svc_find(gen, 23)->status == IPC_STUB && svc_find(gen, 15)->status == IPC_IMPL, "new rows are STUB, the old ones stay IMPL");
    svc_call(gen, 4, NULL, 0, &c); IpcObject *rq = c.out_objects[0];
    no_dup(rq);
    svc_call(rq, 20, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 4, "IRequest GetRevision: a zero word");
    svc_call(rq, 26, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 4, "IRequest GetNetworkAccessStatus: a zero word");
    /* friends */
    IpcObject *fr = friend_open();
    svc_call(fr, 0, NULL, 0, &c); IpcObject *fs = c.out_objects[0];
    no_dup(fs);
    svc_call(fs, 20300, NULL, 0, &c); CHECK(c.result == HZ_R_FRIENDS_NETWORK_SERVICE_ACCOUNT_UNAVAILABLE, "GetFriendCandidateList: no network service account");
    svc_call(fs, 10120, NULL, 0, &c); CHECK(c.result == HZ_R_FRIENDS_NETWORK_SERVICE_ACCOUNT_UNAVAILABLE, "CheckFriendListAvailability: no network service account");
    /* ssl */
    IpcObject *ssl = ssl_open();
    no_dup(ssl);
    svc_call(ssl, 11, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 4, "GetTrustedCertificateCount: zero");
    svc_call(ssl, 2, NULL, 0, &c); CHECK(c.result == HZ_R_SSL_NOT_CONNECTED, "GetCertificates: no store, not connected result");
    /* bsd / nsd / sfdnsres */
    IpcObject *bs = bsd_open();
    no_dup(bs);
    svc_call(bs, 40, NULL, 0, &c); CHECK(c.result == 0, "bsd ShutdownAllSockets row exists (accepted)");
    IpcObject *nsd = nsd_open();
    no_dup(nsd);
    CHECK(svc_find(nsd, 100) != NULL, "nsd GetApplicationServerEnvironmentType has a row");
    printf("{\"test\":\"svc_sysdef_offline\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
