/* time:u / time:s / time:a: clocks from the host. The SDK reads most clock state from a shared memory block
 * (GetSharedMemoryNativeHandle) without IPC, so the block's layout matters:
 *   0x00 steady clock context, 0x38 local system clock context, 0x80 network system clock context, 0xC8 automatic correction flag,
 * each a lock-free pair (u32 counter, two copies of the value; the reader uses copy [counter & 1]). */
#include "../ipc.h"
#include "../timescale.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const uint8_t clock_source[16] = {0x4e, 0x61, 0x70, 0x70, 0x6c, 0x65, 0x43, 0x6c, 1, 2, 3, 4, 5, 6, 7, 8};

static int64_t steady_seconds(void) { return (int64_t)(runtime_guest_ticks() / 19200000ull); }
/* The wall clock the guest sees advances with the game clock: real time plus the skew the time scale accumulated (0 at 1x). */
static int64_t posix_now(void) { return (int64_t)time(NULL) + timescale_wall_skew_ns() / 1000000000ll; }
static void system_context(uint8_t *p) {                       /* SystemClockContext, 0x20 */
    int64_t steady = steady_seconds(), off = posix_now() - steady;
    memcpy(p, &off, 8); memcpy(p + 8, &steady, 8); memcpy(p + 0x10, clock_source, 16);
}

static IpcSharedMemory *shmem;
static IpcSharedMemory *time_shmem(void) {
    if (shmem) return shmem;
    shmem = ipc_shared_memory_new(0x1000);
    uint8_t *m = shmem->host;
    memcpy(m + 0x10, clock_source, 16);                                   /* steady clock context: offset 0, source id */
    system_context(m + 0x40); system_context(m + 0x88);
    return shmem;
}

static void sc_get_current_time(IpcObject *s, IpcCall *c) { (void)s; int64_t t = posix_now(); IPC_OUT(c, t); }
static void sc_get_context(IpcObject *s, IpcCall *c) { (void)s; uint8_t b[0x20]; system_context(b); ipc_out(c, b, sizeof b); }
static void sc_noop(IpcObject *s, IpcCall *c) { (void)s; (void)c; }
static const IpcCmd system_clock_cmds[] = {
    {0, "GetCurrentTime", sc_get_current_time, IPC_IMPL}, {1, "SetCurrentTime", sc_noop, IPC_STUB},
    {2, "GetSystemClockContext", sc_get_context, IPC_IMPL}, {3, "SetSystemClockContext", sc_noop, IPC_STUB},
};
static void steady_now(IpcObject *s, IpcCall *c) { (void)s; int64_t v = steady_seconds(); ipc_out(c, &v, 8); ipc_out(c, clock_source, 16); }
static void steady_zero(IpcObject *s, IpcCall *c) { (void)s; int64_t v = 0; IPC_OUT(c, v); }
static const IpcCmd steady_clock_cmds[] = {
    {0, "GetCurrentTimePoint", steady_now, IPC_IMPL}, {2, "GetTestOffset", steady_zero, IPC_STUB}, {3, "SetTestOffset", sc_noop, IPC_STUB},
    {200, "GetRtcValue", steady_zero, IPC_STUB}, {201, "IsRtcResetDetected", steady_zero, IPC_STUB}, {204, "GetInternalOffset", steady_zero, IPC_STUB},
};

/* time zone: local time = UTC converted with the host's current zone rules. */
static void write_calendar(IpcCall *c, int64_t t, int utc) {
    time_t tt = (time_t)t; struct tm tm;
    if (utc) gmtime_r(&tt, &tm); else localtime_r(&tt, &tm);
    uint8_t cal[8] = {0}; uint16_t year = (uint16_t)(tm.tm_year + 1900); memcpy(cal, &year, 2);
    cal[2] = (uint8_t)(tm.tm_mon + 1); cal[3] = (uint8_t)tm.tm_mday; cal[4] = (uint8_t)tm.tm_hour; cal[5] = (uint8_t)tm.tm_min; cal[6] = (uint8_t)tm.tm_sec;
    uint8_t info[0x18] = {0};
    int32_t wday = tm.tm_wday, yday = tm.tm_yday, off = utc ? 0 : (int32_t)tm.tm_gmtoff; uint32_t dst = utc ? 0 : (uint32_t)(tm.tm_isdst > 0);
    memcpy(info, &wday, 4); memcpy(info + 4, &yday, 4);
    snprintf((char *)info + 8, 8, "%s", utc || !tm.tm_zone ? "UTC" : tm.tm_zone);
    memcpy(info + 16, &dst, 4); memcpy(info + 20, &off, 4);
    ipc_out(c, cal, sizeof cal); ipc_out(c, info, sizeof info);
}
static void tz_location_name(IpcObject *s, IpcCall *c) { (void)s; char n[0x24] = "UTC"; ipc_out(c, n, sizeof n); }
static void tz_to_calendar(IpcObject *s, IpcCall *c) { (void)s; write_calendar(c, IPC_IN(c, int64_t, 0), 0); }
static void tz_to_calendar_utc(IpcObject *s, IpcCall *c) { (void)s; write_calendar(c, IPC_IN(c, int64_t, 0), 1); }
static void tz_to_posix(IpcObject *s, IpcCall *c) {
    (void)s;
    const uint8_t *in = ipc_in(c, 0, 8); struct tm tm = {0};
    uint16_t year; memcpy(&year, in, 2);
    tm.tm_year = year - 1900; tm.tm_mon = in[2] - 1; tm.tm_mday = in[3]; tm.tm_hour = in[4]; tm.tm_min = in[5]; tm.tm_sec = in[6]; tm.tm_isdst = -1;
    int64_t t = mktime(&tm);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b) ipc_write_buf(c, b, &t, 8);
    int32_t n = 1; IPC_OUT(c, n);
}
static const IpcCmd timezone_cmds[] = {
    {0, "GetDeviceLocationName", tz_location_name, IPC_STUB},
    {100, "ToCalendarTime", tz_to_calendar_utc, IPC_IMPL}, {101, "ToCalendarTimeWithMyRule", tz_to_calendar, IPC_IMPL},
    {201, "ToPosixTime", tz_to_posix, IPC_IMPL}, {202, "ToPosixTimeWithMyRule", tz_to_posix, IPC_IMPL},
};

static void st_system_clock(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("ISystemClock", system_clock_cmds, NULL, NULL)); }
static void st_steady_clock(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("ISteadyClock", steady_clock_cmds, NULL, NULL)); }
static void st_timezone(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("ITimeZoneService", timezone_cmds, NULL, NULL)); }
static void st_shared_memory(IpcObject *s, IpcCall *c) { (void)s; ipc_out_copy_handle(c, ipc_shared_memory_handle(time_shmem())); }
static void st_bool_false(IpcObject *s, IpcCall *c) { (void)s; uint8_t v = 0; IPC_OUT(c, v); }
static void st_initial_year(IpcObject *s, IpcCall *c) { (void)s; int32_t y = 2000; IPC_OUT(c, y); }
static const IpcCmd static_cmds[] = {
    {0, "GetStandardUserSystemClock", st_system_clock, IPC_IMPL}, {1, "GetStandardNetworkSystemClock", st_system_clock, IPC_IMPL},
    {2, "GetStandardSteadyClock", st_steady_clock, IPC_IMPL}, {3, "GetTimeZoneService", st_timezone, IPC_IMPL},
    {4, "GetStandardLocalSystemClock", st_system_clock, IPC_IMPL}, {5, "GetEphemeralNetworkSystemClock", st_system_clock, IPC_IMPL},
    {20, "GetSharedMemoryNativeHandle", st_shared_memory, IPC_IMPL},
    {100, "IsStandardUserSystemClockAutomaticCorrectionEnabled", st_bool_false, IPC_STUB},
    {101, "SetStandardUserSystemClockAutomaticCorrectionEnabled", sc_noop, IPC_STUB}, {102, "GetStandardUserSystemClockInitialYear", st_initial_year, IPC_STUB},
    {200, "IsStandardNetworkSystemClockAccuracySufficient", st_bool_false, IPC_STUB},
};
static IpcObject *time_open(void) { return IPC_OBJECT("time:u", static_cmds, NULL, NULL); }
void service_register_time(void) { ipc_register_service("time:u", time_open); ipc_register_service("time:s", time_open); ipc_register_service("time:a", time_open); }
