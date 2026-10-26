/* time:u / time:s / time:a: clocks from the host. The SDK reads most clock state from a shared memory block
 * (GetSharedMemoryNativeHandle) without IPC, so the block's layout matters:
 *   0x00 steady clock context, 0x38 local system clock context, 0x80 network system clock context, 0xC8 automatic correction flag,
 * each a lock-free pair (u32 counter, two copies of the value; the reader uses copy [counter & 1]). */
#include "../ipc.h"
#include "../napple.h"
#include "../timescale.h"
#include "time_core.h"
#include "hz_result.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const uint8_t clock_source[16] = {0x4e, 0x61, 0x70, 0x70, 0x6c, 0x65, 0x43, 0x6c, 1, 2, 3, 4, 5, 6, 7, 8};

static int64_t steady_seconds(void) { return (int64_t)(runtime_guest_ticks() / 19200000ull); }
/* The wall clock the guest sees advances with the game clock: real time plus the skew the time scale accumulated (0 at 1x). */
static int64_t posix_now(void) { return (int64_t)time(NULL) + timescale_wall_skew_ns() / 1000000000ll; }
static void system_context(uint8_t *p) { /* SystemClockContext, 0x20 */
    int64_t steady = steady_seconds(), off = posix_now() - steady;
    memcpy(p, &off, 8);
    memcpy(p + 8, &steady, 8);
    memcpy(p + 0x10, clock_source, 16);
}

/* nn::time::AdjustableUserSystemClock::GetCurrentTime, the clock Animal Crossing 3.0.3's days follow (SDK 20.5.4): the SDK reads it through a MonotonicSystemClock
 * (sdk+0x4421cc) and, with the shared memory this service gives, the game saw epoch 0 (ToCalendarTime(0)), took the date and time for invalid
 * (System_DateTimeNg event flow) and never left its boot sequence. The host answers with its own clock, as nxnative does. */
static uint32_t h_adjustable_user_clock_now(int64_t *out) {
    if (out) *out = posix_now();
    return 0;
}
static int h_adjustable_user_clock_timespec(int64_t *ts) {
    if (ts) {
        ts[0] = posix_now();
        ts[1] = 0;
    }
    return 0;
}
void time_register_overrides(void) {
    static HostSymbol table[] = {
        {"_ZN2nn4time25AdjustableUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", h_adjustable_user_clock_now},
        {"_ZN2nn4time43GetAdjustableUserSystemClockCurrentTimespecEP8timespec", h_adjustable_user_clock_timespec},
    };
    host_symbols_register(table, sizeof table / sizeof *table);
}

static IpcSharedMemory *shmem;
static IpcSharedMemory *time_shmem(void) {
    if (shmem) return shmem;
    shmem = ipc_shared_memory_new(0x1000);
    uint8_t *m = shmem->host;
    memcpy(m + 0x10, clock_source, 16); /* steady clock context: offset 0, source id */
    system_context(m + 0x40);
    system_context(m + 0x88);
    return shmem;
}

static void sc_get_current_time(IpcObject *s, IpcCall *c) {
    (void)s;
    int64_t t = posix_now();
    IPC_OUT(c, t);
}
static void sc_get_context(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t b[0x20];
    system_context(b);
    ipc_out(c, b, sizeof b);
}
static void sc_noop(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static const IpcCmd system_clock_cmds[] = {
    {0, "GetCurrentTime", sc_get_current_time, IPC_IMPL},
    {1, "SetCurrentTime", sc_noop, IPC_STUB},
    {2, "GetSystemClockContext", sc_get_context, IPC_IMPL},
    {3, "SetSystemClockContext", sc_noop, IPC_STUB},
};
static void steady_now(IpcObject *s, IpcCall *c) {
    (void)s;
    int64_t v = steady_seconds();
    ipc_out(c, &v, 8);
    ipc_out(c, clock_source, 16);
}
static void steady_zero(IpcObject *s, IpcCall *c) {
    (void)s;
    int64_t v = 0;
    IPC_OUT(c, v);
}
static const IpcCmd steady_clock_cmds[] = {
    {0, "GetCurrentTimePoint", steady_now, IPC_IMPL}, {2, "GetTestOffset", steady_zero, IPC_STUB},        {3, "SetTestOffset", sc_noop, IPC_STUB},
    {200, "GetRtcValue", steady_zero, IPC_STUB},      {201, "IsRtcResetDetected", steady_zero, IPC_STUB}, {204, "GetInternalOffset", steady_zero, IPC_STUB},
};

/* time zone. The device zone is the host's IANA zone when /etc/localtime points into a zoneinfo directory, else UTC. A TimeZoneRule is an
 * opaque blob to the guest; ours (time_core.h) says "fixed offset" (UTC family) or "host" (the host's rules, including DST, through localtime_r). */
static const char *device_zone(void) {
    static char name[0x24];
    static int done;
    if (!done) {
        done = 1;
        char link[256];
        ssize_t n = readlink("/etc/localtime", link, sizeof link - 1);
        const char *z = NULL;
        if (n > 0) {
            link[n] = 0;
            z = time_zone_from_link(link);
        }
        snprintf(name, sizeof name, "%s", z ? z : "UTC");
    }
    return name;
}
/* Calendar for `t` under the host's rules (utc: no zone). */
static void host_calendar(int64_t t, int utc, TimeCalendar *cal, TimeCalendarInfo *info) {
    time_t tt = (time_t)t;
    struct tm tm;
    if (utc) gmtime_r(&tt, &tm);
    else localtime_r(&tt, &tm);
    memset(info, 0, sizeof *info);
    cal->year = (int16_t)(tm.tm_year + 1900);
    cal->month = (int8_t)(tm.tm_mon + 1);
    cal->day = (int8_t)tm.tm_mday;
    cal->hour = (int8_t)tm.tm_hour;
    cal->minute = (int8_t)tm.tm_min;
    cal->second = (int8_t)tm.tm_sec;
    info->wday = tm.tm_wday;
    info->yday = tm.tm_yday;
    snprintf(info->zone, sizeof info->zone, "%s", utc || !tm.tm_zone ? "UTC" : tm.tm_zone);
    info->is_dst = utc ? 0 : tm.tm_isdst > 0;
    info->gmt_offset = utc ? 0 : (int32_t)tm.tm_gmtoff;
}
/* Calendar under a rule: NULL or a fixed rule use time_core, a host rule uses the host's. */
static void calendar_under(int64_t t, const TimeRule *rule, TimeCalendar *cal, TimeCalendarInfo *info) {
    if (rule && rule->mode == TIME_RULE_HOST) {
        host_calendar(t, 0, cal, info);
        return;
    }
    time_to_calendar(t, rule ? rule->offset : 0, cal, info);
    snprintf(info->zone, sizeof info->zone, "UTC");
}
static void put_calendar(IpcCall *c, const TimeCalendar *cal, const TimeCalendarInfo *info) {
    uint8_t cb[8], ib[0x18];
    time_cal_write(cb, cal);
    time_info_write(ib, info);
    ipc_out(c, cb, sizeof cb);
    ipc_out(c, ib, sizeof ib);
}
static int rule_from_call(IpcCall *c, TimeRule *r) { /* the rule travels as the call's first input buffer */
    const IpcBuf *b = ipc_in_buf(c, 0);
    const void *p = b ? ipc_buf_ptr(b) : NULL;
    return time_rule_parse(p, b ? (size_t)b->size : 0, r);
}
static void tz_location_name(IpcObject *s, IpcCall *c) {
    (void)s;
    char n[0x24] = {0};
    snprintf(n, sizeof n, "%s", device_zone());
    ipc_out(c, n, sizeof n);
}
static int location_total(void) { return strcmp(device_zone(), "UTC") ? 2 : 1; }
static void tz_location_count(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t n = location_total();
    IPC_OUT(c, n);
}
static void tz_location_list(IpcObject *s, IpcCall *c) { /* 3(s32 index) -> names from `index`, count written */
    (void)s;
    int32_t idx = IPC_IN(c, int32_t, 0);
    if (idx < 0) idx = 0;
    const IpcBuf *b = ipc_out_buf(c, 0);
    const char *names[2] = {"UTC", device_zone()};
    int written = 0;
    if (b && idx < location_total()) {
        int fit = (int)(b->size / 0x24), avail = location_total() - idx;
        written = fit < avail ? fit : avail;
        uint8_t *tmp = calloc(written ? (size_t)written : 1, 0x24);
        for (int i = 0; i < written && tmp; i++) snprintf((char *)tmp + (size_t)i * 0x24, 0x24, "%s", names[idx + i]);
        if (tmp && written) ipc_write_buf(c, b, tmp, (uint64_t)written * 0x24);
        free(tmp);
    }
    int32_t cnt = written;
    IPC_OUT(c, cnt);
}
static void tz_load_rule(IpcObject *s, IpcCall *c) { /* 4(LocationName) -> rule in the output buffer */
    (void)s;
    char name[0x25] = {0};
    memcpy(name, ipc_in(c, 0, 0x24), 0x24);
    int kind = time_location_kind(name, device_zone());
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!kind || !b || b->size < TIME_RULE_SIZE) {
        c->result = HZ_RESULT(HZ_MOD_TIME, 200);
        return;
    } /* [unverified] time zone not found */
    TimeRule r;
    static uint8_t blob[TIME_RULE_SIZE];
    time_rule_make(&r, kind == 1 ? TIME_RULE_FIXED : TIME_RULE_HOST, 0, name, blob);
    ipc_write_buf(c, b, blob, sizeof blob);
}
static void tz_rule_version(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v[16] = {'N', 'A', 'P', 'L', 'T', 'Z', '0', '1'};
    ipc_out(c, v, sizeof v);
}
static void tz_to_calendar_my_rule(IpcObject *s, IpcCall *c) { /* 101: the host's rules */
    (void)s;
    TimeCalendar cal;
    TimeCalendarInfo info;
    host_calendar(IPC_IN(c, int64_t, 0), 0, &cal, &info);
    put_calendar(c, &cal, &info);
}
static void tz_to_calendar(IpcObject *s, IpcCall *c) { /* 100: the rule the guest loaded; none we recognise -> UTC */
    (void)s;
    TimeRule r;
    int have = !rule_from_call(c, &r);
    TimeCalendar cal;
    TimeCalendarInfo info;
    {
        static int n;
        if (getenv("NAPPLE_TIME_LOG") && n++ < 5) LOG("time", "ToCalendarTime(%lld) rule %s", (long long)IPC_IN(c, int64_t, 0), have ? "loaded" : "none");
    }
    calendar_under(IPC_IN(c, int64_t, 0), have ? &r : NULL, &cal, &info);
    put_calendar(c, &cal, &info);
}
static void posix_out(IpcCall *c, int64_t t, int ok) {
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!ok) {
        c->result = HZ_RESULT(HZ_MOD_TIME, 102);
        return;
    } /* [unverified] invalid calendar */
    if (b) ipc_write_buf(c, b, &t, 8);
    int32_t n = 1;
    IPC_OUT(c, n);
}
static int64_t host_mktime(const TimeCalendar *cal) {
    struct tm tm = {0};
    tm.tm_year = cal->year - 1900;
    tm.tm_mon = cal->month - 1;
    tm.tm_mday = cal->day;
    tm.tm_hour = cal->hour;
    tm.tm_min = cal->minute;
    tm.tm_sec = cal->second;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}
static void tz_to_posix_my_rule(IpcObject *s, IpcCall *c) { /* 202: the host's rules */
    (void)s;
    TimeCalendar cal;
    time_cal_read(ipc_in(c, 0, 8), &cal);
    int64_t probe;
    int ok = !time_to_posix(&cal, 0, &probe); /* range-check with the pure maths */
    posix_out(c, ok ? host_mktime(&cal) : 0, ok);
}
static void tz_to_posix(IpcObject *s, IpcCall *c) { /* 201: the guest's loaded rule */
    TimeCalendar cal;
    time_cal_read(ipc_in(c, 0, 8), &cal);
    TimeRule r;
    int have = !rule_from_call(c, &r);
    if (have && r.mode == TIME_RULE_HOST) {
        tz_to_posix_my_rule(s, c);
        return;
    }
    int64_t t = 0;
    int ok = !time_to_posix(&cal, have ? r.offset : 0, &t);
    posix_out(c, t, ok);
}
static const IpcCmd timezone_cmds[] = {
    {0, "GetDeviceLocationName", tz_location_name, IPC_IMPL},
    {2, "GetTotalLocationNameCount", tz_location_count, IPC_IMPL},
    {3, "LoadLocationNameList", tz_location_list, IPC_IMPL},
    {4, "LoadTimeZoneRule", tz_load_rule, IPC_IMPL},
    {5, "GetTimeZoneRuleVersion", tz_rule_version, IPC_IMPL},
    {100, "ToCalendarTime", tz_to_calendar, IPC_IMPL},
    {101, "ToCalendarTimeWithMyRule", tz_to_calendar_my_rule, IPC_IMPL},
    {201, "ToPosixTime", tz_to_posix, IPC_IMPL},
    {202, "ToPosixTimeWithMyRule", tz_to_posix_my_rule, IPC_IMPL},
};

/* ClockSnapshot (400/401) and the calculations on snapshots (500/501). The "user" and "network" clocks are the same host clock: the host's time
 * is NTP-accurate, so automatic correction and network accuracy are reported as enabled / sufficient. */
static int auto_correction = 1;
static void build_snapshot(uint8_t type, const uint8_t *user_ctx, const uint8_t *net_ctx, uint8_t out[TIME_SNAPSHOT_SIZE]) {
    TimeSnapshot sn;
    memset(&sn, 0, sizeof sn);
    int64_t steady = steady_seconds();
    if (user_ctx) memcpy(sn.user_ctx, user_ctx, 0x20);
    else system_context(sn.user_ctx);
    if (net_ctx) memcpy(sn.network_ctx, net_ctx, 0x20);
    else system_context(sn.network_ctx);
    int64_t uoff, noff;
    memcpy(&uoff, sn.user_ctx, 8);
    memcpy(&noff, sn.network_ctx, 8);
    sn.user_time = uoff + steady;
    sn.network_time = noff + steady;
    host_calendar(sn.user_time, 0, &sn.user_cal, &sn.user_info);
    host_calendar(sn.network_time, 0, &sn.network_cal, &sn.network_info);
    sn.steady_value = steady;
    memcpy(sn.steady_source, clock_source, 16);
    snprintf(sn.location, sizeof sn.location, "%s", device_zone());
    sn.auto_correction = (uint8_t)auto_correction;
    sn.type = type;
    time_snapshot_write(&sn, out);
}
static void st_get_snapshot(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t out[TIME_SNAPSHOT_SIZE];
    build_snapshot(IPC_IN(c, uint8_t, 0), NULL, NULL, out);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b) ipc_write_buf(c, b, out, sizeof out);
}
static void st_get_snapshot_ctx(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t out[TIME_SNAPSHOT_SIZE];
    build_snapshot(IPC_IN(c, uint8_t, 0), ipc_in(c, 8, 0x20), ipc_in(c, 0x28, 0x20), out);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b) ipc_write_buf(c, b, out, sizeof out);
}
static int two_snapshots(IpcCall *c, TimeSnapshot *a, TimeSnapshot *b) {
    const IpcBuf *ba = ipc_in_buf(c, 0), *bb = ipc_in_buf(c, 1);
    const uint8_t *pa = ba ? ipc_buf_ptr(ba) : NULL, *pb = bb ? ipc_buf_ptr(bb) : NULL;
    if (!pa || !pb || ba->size < TIME_SNAPSHOT_SIZE || bb->size < TIME_SNAPSHOT_SIZE) return -1;
    time_snapshot_read(pa, a);
    time_snapshot_read(pb, b);
    return 0;
}
static void st_span_between(IpcObject *s, IpcCall *c) {
    (void)s;
    TimeSnapshot a, b;
    int64_t span;
    if (two_snapshots(c, &a, &b) || time_snapshot_span(&a, &b, &span)) {
        c->result = HZ_RESULT(HZ_MOD_TIME, 200);
        return;
    } /* [unverified] snapshots from different clocks */
    IPC_OUT(c, span);
}
static void st_user_clock_difference(IpcObject *s, IpcCall *c) {
    (void)s;
    TimeSnapshot a, b;
    int64_t span;
    if (two_snapshots(c, &a, &b) || time_snapshot_span(&a, &b, &span)) {
        c->result = HZ_RESULT(HZ_MOD_TIME, 200);
        return;
    }
    int64_t ns = span * 1000000000ll;
    IPC_OUT(c, ns);
}
/* CalculateMonotonicSystemClockBaseTimePoint(SystemClockContext) -> s64: context.offset + now - context.steady_time_point, or a clock mismatch error. */
static void st_monotonic_base(IpcObject *s, IpcCall *c) {
    (void)s;
    const uint8_t *ctx = ipc_in(c, 0, 0x20);
    if (!ctx) {
        c->result = HZ_RESULT(HZ_MOD_TIME, 200);
        return;
    }
    int64_t off, tp;
    memcpy(&off, ctx, 8);
    memcpy(&tp, ctx + 8, 8);
    if (memcmp(ctx + 0x10, clock_source, 16)) {
        c->result = HZ_RESULT(HZ_MOD_TIME, 200);
        return;
    } /* [unverified] ResultClockMismatch */
    int64_t base = off + steady_seconds() - tp;
    IPC_OUT(c, base);
}
static void st_auto_correction_get(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = (uint8_t)auto_correction;
    IPC_OUT(c, v);
}
static void st_auto_correction_set(IpcObject *s, IpcCall *c) {
    (void)s;
    auto_correction = IPC_IN(c, uint8_t, 0) != 0;
}

static void st_system_clock(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("ISystemClock", system_clock_cmds, NULL, NULL));
}
static void st_steady_clock(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("ISteadyClock", steady_clock_cmds, NULL, NULL));
}
static void st_timezone(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("ITimeZoneService", timezone_cmds, NULL, NULL));
}
static void st_shared_memory(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_copy_handle(c, ipc_shared_memory_handle(time_shmem()));
}
static void svc_true(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 1;
    IPC_OUT(c, v);
}
static void st_initial_year(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t y = 2000;
    IPC_OUT(c, y);
}
static const IpcCmd static_cmds[] = {
    {0, "GetStandardUserSystemClock", st_system_clock, IPC_IMPL},
    {1, "GetStandardNetworkSystemClock", st_system_clock, IPC_IMPL},
    {2, "GetStandardSteadyClock", st_steady_clock, IPC_IMPL},
    {3, "GetTimeZoneService", st_timezone, IPC_IMPL},
    {4, "GetStandardLocalSystemClock", st_system_clock, IPC_IMPL},
    {5, "GetEphemeralNetworkSystemClock", st_system_clock, IPC_IMPL},
    {20, "GetSharedMemoryNativeHandle", st_shared_memory, IPC_IMPL},
    {100, "IsStandardUserSystemClockAutomaticCorrectionEnabled", st_auto_correction_get, IPC_IMPL},
    {101, "SetStandardUserSystemClockAutomaticCorrectionEnabled", st_auto_correction_set, IPC_IMPL},
    {102, "GetStandardUserSystemClockInitialYear", st_initial_year, IPC_STUB},
    {200, "IsStandardNetworkSystemClockAccuracySufficient", svc_true, IPC_IMPL},
    {300, "CalculateMonotonicSystemClockBaseTimePoint", st_monotonic_base, IPC_IMPL},
    {400, "GetClockSnapshot", st_get_snapshot, IPC_IMPL},
    {401, "GetClockSnapshotFromSystemClockContext", st_get_snapshot_ctx, IPC_IMPL},
    {500, "CalculateStandardUserSystemClockDifferenceByUser", st_user_clock_difference, IPC_IMPL},
    {501, "CalculateSpanBetween", st_span_between, IPC_IMPL},
};
static IpcObject *time_open(void) { return IPC_OBJECT("time:u", static_cmds, NULL, NULL); }
void service_register_time(void) {
    ipc_register_service("time:u", time_open);
    ipc_register_service("time:s", time_open);
    ipc_register_service("time:a", time_open);
}
