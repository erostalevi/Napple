/* Unit tests: time core (calendar maths, rules, snapshots) and the time:u tables (time zone rule, snapshot). */
#include "../src/services/time.c"
#include "svc_test.h"

static void chk_cal(int64_t t, int32_t off, int y, int mo, int d, int h, int mi, int sec, int wday, int yday) {
    TimeCalendar c; TimeCalendarInfo i; time_to_calendar(t, off, &c, &i);
    CHECK(c.year == y && c.month == mo && c.day == d && c.hour == h && c.minute == mi && c.second == sec && i.wday == wday && i.yday == yday,
          "posix %lld off %d -> %d-%02d-%02d %02d:%02d:%02d wday %d yday %d (got %d-%02d-%02d %02d:%02d:%02d wday %d yday %d)", (long long)t, off, y, mo, d, h, mi, sec, wday, yday,
          c.year, c.month, c.day, c.hour, c.minute, c.second, i.wday, i.yday);
    int64_t back; CHECK(time_to_posix(&c, off, &back) == 0 && back == t, "round trip %lld", (long long)t);
}

int main(void) {
    chk_cal(0, 0, 1970, 1, 1, 0, 0, 0, 4, 0);                           /* Thursday */
    chk_cal(951782400, 0, 2000, 2, 29, 0, 0, 0, 2, 59);                 /* leap day, Tuesday */
    chk_cal(1700000000, 0, 2023, 11, 14, 22, 13, 20, 2, 317);
    chk_cal(1700000000, 3600, 2023, 11, 14, 23, 13, 20, 2, 317);        /* +01:00 */
    chk_cal(1700000000, -8 * 3600 - 1800, 2023, 11, 14, 13, 43, 20, 2, 317);
    chk_cal(-1, 0, 1969, 12, 31, 23, 59, 59, 3, 364);                   /* before the epoch */
    chk_cal(4102444800ll, 0, 2100, 1, 1, 0, 0, 0, 5, 0);                /* 2100 is not a leap year */
    chk_cal(1798675200ll, 0, 2026, 12, 31, 0, 0, 0, 4, 364);
    CHECK(time_days_in_month(2024, 2) == 29 && time_days_in_month(2100, 2) == 28 && time_days_in_month(2000, 2) == 29 && time_days_in_month(2023, 13) == 0, "days in month");
    TimeCalendar bad = {2023, 2, 29, 0, 0, 0}; int64_t t; CHECK(time_to_posix(&bad, 0, &t) == -1, "29 Feb 2023 rejected");
    bad = (TimeCalendar){2023, 13, 1, 0, 0, 0}; CHECK(time_to_posix(&bad, 0, &t) == -1, "month 13 rejected");
    bad = (TimeCalendar){2023, 1, 1, 24, 0, 0}; CHECK(time_to_posix(&bad, 0, &t) == -1, "hour 24 rejected");

    CHECK(time_location_kind("UTC", "Europe/Berlin") == 1 && time_location_kind("Etc/GMT", NULL) == 1 && time_location_kind("Europe/Berlin", "Europe/Berlin") == 2 &&
          time_location_kind("Asia/Tokyo", "Europe/Berlin") == 0 && time_location_kind("Europe/Berlin", NULL) == 0, "location kinds");
    CHECK(!strcmp(time_zone_from_link("../var/db/timezone/zoneinfo/Europe/Berlin"), "Europe/Berlin") && time_zone_from_link("/etc/foo") == NULL && time_zone_from_link(NULL) == NULL, "zone from link");
    TimeRule r, p; static uint8_t blob[TIME_RULE_SIZE]; time_rule_make(&r, TIME_RULE_HOST, 7, "Europe/Berlin", blob);
    CHECK(time_rule_parse(blob, sizeof blob, &p) == 0 && p.mode == TIME_RULE_HOST && p.offset == 7 && !strcmp(p.name, "Europe/Berlin"), "rule round trip");
    memset(blob, 0, sizeof blob); CHECK(time_rule_parse(blob, sizeof blob, &p) == -1 && time_rule_parse(blob, 4, &p) == -1, "foreign rule rejected");

    TimeSnapshot a, b, a2; memset(&a, 0, sizeof a); a.user_time = 1000; a.type = 1; a.auto_correction = 1; memcpy(a.steady_source, "0123456789abcdef", 16); a.user_cal.year = 2026; a.user_cal.month = 10;
    snprintf(a.location, sizeof a.location, "Europe/Berlin"); a.user_info.gmt_offset = 7200; a.user_info.is_dst = 1; snprintf(a.user_info.zone, 8, "CEST"); a.steady_value = 77;
    uint8_t w[TIME_SNAPSHOT_SIZE]; time_snapshot_write(&a, w); time_snapshot_read(w, &a2);
    CHECK(a2.user_time == 1000 && a2.type == 1 && a2.user_cal.year == 2026 && !strcmp(a2.location, "Europe/Berlin") && a2.user_info.gmt_offset == 7200 && !strcmp(a2.user_info.zone, "CEST") && a2.steady_value == 77, "snapshot round trip");
    CHECK(w[0xcc] == 1 && w[0xcd] == 1 && w[0x50] == 0xea && w[0x51] == 0x07 && w[0x52] == 10, "snapshot field offsets (auto correction +0xcc, type +0xcd, calendar +0x50)");
    b = a; b.user_time = 1090; int64_t span; CHECK(time_snapshot_span(&a, &b, &span) == 0 && span == 90, "span");
    memcpy(b.steady_source, "XXXXXXXXXXXXXXXX", 16); CHECK(time_snapshot_span(&a, &b, &span) == -1, "different steady clocks");

    /* IPC: time zone service */
    IpcObject *st = time_open(); IpcCall c;
    svc_call(st, 3, NULL, 0, &c); IpcObject *tz = c.out_objects[0];
    svc_call(tz, 0, NULL, 0, &c); CHECK(c.out_size == 0x24 && c.out[0] != 0, "GetDeviceLocationName");
    char loc[0x24]; memcpy(loc, c.out, 0x24);
    static uint8_t rulebuf[TIME_RULE_SIZE]; char nm[0x24] = "UTC";
    memset(&c, 0, sizeof c); c.in = (const uint8_t *)nm; c.in_size = 0x24; svc_set_out_buf(&c, rulebuf, sizeof rulebuf); svc_call_c(tz, 4, &c);
    CHECK(c.result == 0 && !time_rule_parse(rulebuf, sizeof rulebuf, &r) && r.mode == TIME_RULE_FIXED, "LoadTimeZoneRule(UTC) gives a fixed rule");
    char unk[0x24] = "Mars/Olympus"; memset(&c, 0, sizeof c); c.in = (const uint8_t *)unk; c.in_size = 0x24; svc_set_out_buf(&c, rulebuf, sizeof rulebuf); svc_call_c(tz, 4, &c);
    CHECK(c.result != 0, "unknown location: error");
    memset(&c, 0, sizeof c); c.in = (const uint8_t *)loc; c.in_size = 0x24; svc_set_out_buf(&c, rulebuf, sizeof rulebuf); svc_call_c(tz, 4, &c);
    CHECK(c.result == 0, "the device's own location loads");
    memset(&c, 0, sizeof c); c.in = (const uint8_t *)nm; c.in_size = 0x24; svc_set_out_buf(&c, rulebuf, sizeof rulebuf); svc_call_c(tz, 4, &c);
    int64_t tq = 1700000000; uint8_t in8[8]; memcpy(in8, &tq, 8);
    memset(&c, 0, sizeof c); c.in = in8; c.in_size = 8; svc_set_in_buf(&c, rulebuf, sizeof rulebuf); svc_call_c(tz, 100, &c);
    TimeCalendar oc; time_cal_read(c.out, &oc);
    CHECK(c.result == 0 && c.out_size == 8 + 0x18 && oc.year == 2023 && oc.month == 11 && oc.day == 14 && oc.hour == 22 && oc.minute == 13 && oc.second == 20, "ToCalendarTime with the UTC rule");
    uint8_t cal8[8]; time_cal_write(cal8, &oc); int64_t back = 0;
    memset(&c, 0, sizeof c); c.in = cal8; c.in_size = 8; svc_set_in_buf(&c, rulebuf, sizeof rulebuf); svc_set_out_buf(&c, &back, 8); svc_call_c(tz, 201, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, int32_t, 0) == 1 && back == 1700000000, "ToPosixTime with the UTC rule");
    TimeCalendar inv = {2023, 2, 30, 0, 0, 0}; time_cal_write(cal8, &inv);
    memset(&c, 0, sizeof c); c.in = cal8; c.in_size = 8; svc_set_in_buf(&c, rulebuf, sizeof rulebuf); svc_set_out_buf(&c, &back, 8); svc_call_c(tz, 201, &c);
    CHECK(c.result != 0, "ToPosixTime rejects 30 Feb");
    memset(&c, 0, sizeof c); c.in = in8; c.in_size = 8; svc_call_c(tz, 100, &c);
    time_cal_read(c.out, &oc); CHECK(oc.hour == 22, "ToCalendarTime without a recognisable rule falls back to UTC");
    svc_call(tz, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) >= 1, "location count");
    static char names[3 * 0x24]; memset(&c, 0, sizeof c); uint32_t zero = 0; c.in = (const uint8_t *)&zero; c.in_size = 4; svc_set_out_buf(&c, names, sizeof names); svc_call_c(tz, 3, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) >= 1 && !strcmp(names, "UTC"), "location list starts with UTC");
    svc_call(tz, 5, NULL, 0, &c); CHECK(c.out_size == 16, "rule version");

    /* IPC: static service flags and snapshots */
    svc_call(st, 100, NULL, 0, &c); CHECK(c.out[0] == 1, "automatic correction defaults to enabled");
    uint8_t off = 0; svc_call(st, 101, &off, 1, &c); svc_call(st, 100, NULL, 0, &c); CHECK(c.out[0] == 0, "automatic correction can be switched off");
    svc_call(st, 200, NULL, 0, &c); CHECK(c.out[0] == 1, "network clock accuracy is sufficient (host time)");
    static uint8_t snapA[TIME_SNAPSHOT_SIZE], snapB[TIME_SNAPSHOT_SIZE]; uint8_t type = 1;
    memset(&c, 0, sizeof c); c.in = &type; c.in_size = 1; svc_set_out_buf(&c, snapA, sizeof snapA); svc_call_c(st, 400, &c);
    TimeSnapshot sn; time_snapshot_read(snapA, &sn);
    CHECK(c.result == 0 && sn.type == 1 && sn.user_cal.year >= 2024 && sn.user_cal.month >= 1 && sn.user_cal.month <= 12 && sn.user_time > 1700000000 && sn.auto_correction == 0 && sn.location[0], "GetClockSnapshot is filled in from the host clock");
    memset(&c, 0, sizeof c); c.in = &type; c.in_size = 1; svc_set_out_buf(&c, snapB, sizeof snapB); svc_call_c(st, 400, &c);
    memset(&c, 0, sizeof c); svc_set_in_buf(&c, snapA, sizeof snapA); svc_set_in_buf(&c, snapB, sizeof snapB); svc_call_c(st, 501, &c);
    CHECK(c.result == 0 && c.out_size == 8 && SVC_OUT(&c, int64_t, 0) >= 0 && SVC_OUT(&c, int64_t, 0) < 5, "CalculateSpanBetween of two close snapshots");
    memset(&c, 0, sizeof c); svc_set_in_buf(&c, snapA, sizeof snapA); svc_set_in_buf(&c, snapB, sizeof snapB); svc_call_c(st, 500, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, int64_t, 0) % 1000000000ll == 0, "difference is in whole seconds expressed in nanoseconds");
    snapB[0x98] ^= 0xff; memset(&c, 0, sizeof c); svc_set_in_buf(&c, snapA, sizeof snapA); svc_set_in_buf(&c, snapB, sizeof snapB); svc_call_c(st, 501, &c);
    CHECK(c.result != 0, "snapshots from different steady clocks are refused");
    memset(&c, 0, sizeof c); svc_call_c(st, 501, &c); CHECK(c.result != 0, "missing snapshot buffers are refused");
    uint8_t ctxin[0x48] = {0}; ctxin[0] = 1; int64_t offs = 1000000; memcpy(ctxin + 8, &offs, 8); memcpy(ctxin + 0x28, &offs, 8);
    int64_t st0 = steady_seconds();   /* the real guest tick counts from host boot, not from 0 as in the Linux mock */
    memset(&c, 0, sizeof c); c.in = ctxin; c.in_size = sizeof ctxin; svc_set_out_buf(&c, snapA, sizeof snapA); svc_call_c(st, 401, &c);
    time_snapshot_read(snapA, &sn); CHECK(c.result == 0 && sn.type == 1 && sn.user_time >= offs + st0 && sn.user_time <= offs + steady_seconds() + 1, "snapshot from a clock context: offset + steady time");
    printf("{\"test\":\"svc_time\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
