/* caps:u and caps:su (nn::capsrv): screenshots and the album. Offline and album-less: saving a screenshot succeeds and returns an album
 * entry without a file; listing the album returns no files; loading, sizing or deleting a file reports "not found". Command ids are from
 * the author's memory of the public documentation and are unverified. Unlisted commands fail loudly (generic unknown-command policy). */
#include "svc_common.h"
#include "caps_core.h"
#include "hz_result.h"
#include "host_info.h"
#include <time.h>

static CapsState caps;

static void entry_out(IpcCall *c) {
    time_t t = time(NULL); struct tm tm; gmtime_r(&t, &tm);
    uint8_t e[CAPS_ENTRY_SIZE];
    caps_make_entry(&caps, (uint16_t)(tm.tm_year + 1900), (uint8_t)(tm.tm_mon + 1), (uint8_t)tm.tm_mday, (uint8_t)tm.tm_hour, (uint8_t)tm.tm_min, (uint8_t)tm.tm_sec, e);
    ipc_out(c, e, sizeof e);
}
/* A title that disabled screenshots (ISelfController::SetScreenShotPermission) gets the refusal a console gives. */
static void su_save_screenshot(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!applet_screenshot_permitted()) { c->result = HZ_R_CAPTURE_SCREENSHOT_NOT_PERMITTED; return; }
    entry_out(c);
}
static void not_found(IpcObject *s, IpcCall *c) { (void)s; c->result = HZ_R_CAPTURE_NOT_FOUND; }
static void list_empty(IpcObject *s, IpcCall *c) { (void)s; int64_t n = 0; IPC_OUT(c, n); }
static void set_shim_version(IpcObject *s, IpcCall *c) { (void)s; caps.shim_version = IPC_IN(c, uint64_t, 0); }
static void precheck(IpcObject *s, IpcCall *c) { (void)s; (void)c; }

static const IpcCmd su_cmds[] = {
    {32, "SaveScreenShot", su_save_screenshot, IPC_STUB}, {33, "SaveScreenShotEx0", su_save_screenshot, IPC_STUB},
    {201, "SaveScreenShot", su_save_screenshot, IPC_STUB}, {202, "SaveEditedScreenShot", su_save_screenshot, IPC_STUB},
    {203, "SaveScreenShotEx0", su_save_screenshot, IPC_STUB}, {204, "SaveEditedScreenShotEx0", su_save_screenshot, IPC_STUB},
    {205, "SaveScreenShotEx1", su_save_screenshot, IPC_STUB}, {206, "SaveScreenShotEx2", su_save_screenshot, IPC_STUB},
    {210, "SaveScreenShotEx2", su_save_screenshot, IPC_STUB},
};
static IpcObject *su_open(void) { return IPC_OBJECT("caps:su", su_cmds, NULL, NULL); }

static const IpcCmd session_cmds[] = {
    {1, "GetAlbumFileList", list_empty, IPC_STUB}, {2, "LoadAlbumFile", not_found, IPC_STUB}, {3, "GetAlbumFileSize", not_found, IPC_STUB},
    {4, "DeleteAlbumFile", not_found, IPC_STUB}, {5, "LoadAlbumFileThumbnail", not_found, IPC_STUB},
};
static void caps_open_session(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IAlbumAccessorApplicationSession", session_cmds, NULL, NULL)); }
static const IpcCmd u_cmds[] = {
    {32, "SetShimLibraryVersion", set_shim_version, IPC_STUB},
    {102, "GetAlbumFileList0AafeAruidDeprecated", list_empty, IPC_STUB}, {103, "DeleteAlbumFileByAruid", not_found, IPC_STUB},
    {104, "GetAlbumFileSizeByAruid", not_found, IPC_STUB}, {105, "DeleteAlbumFileByAruidForDebug", not_found, IPC_STUB},
    {110, "LoadAlbumScreenShotImageByAruid", not_found, IPC_STUB}, {120, "LoadAlbumScreenShotThumbnailImageByAruid", not_found, IPC_STUB},
    {130, "PrecheckToCreateContentsByAruid", precheck, IPC_STUB},
    {140, "GetAlbumFileList1AafeAruidDeprecated", list_empty, IPC_STUB}, {141, "GetAlbumFileList2AafeUidAruidDeprecated", list_empty, IPC_STUB},
    {142, "GetAlbumFileList3AaeAruid", list_empty, IPC_STUB}, {143, "GetAlbumFileList4AaeUidAruid", list_empty, IPC_STUB},
    {60002, "OpenAccessorSessionForApplication", caps_open_session, IPC_STUB},
};
static IpcObject *u_open(void) { return IPC_OBJECT("caps:u", u_cmds, NULL, NULL); }
void service_register_caps(void) { ipc_register_service("caps:su", su_open); ipc_register_service("caps:u", u_open); }
