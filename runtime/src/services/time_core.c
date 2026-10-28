#include "time_core.h"
#include <string.h>

/* Howard Hinnant's civil-date algorithms (public domain). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    *y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y += *m <= 2;
}
static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int time_days_in_month(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m < 1 || m > 12 ? 0 : d[m - 1] + (m == 2 && leap(y));
}

void time_to_calendar(int64_t posix, int32_t offset, TimeCalendar *cal, TimeCalendarInfo *info) {
    int64_t t = posix + offset, days = t / 86400, rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    int64_t y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    cal->year = (int16_t)y;
    cal->month = (int8_t)m;
    cal->day = (int8_t)d;
    cal->hour = (int8_t)(rem / 3600);
    cal->minute = (int8_t)(rem % 3600 / 60);
    cal->second = (int8_t)(rem % 60);
    if (!info) return;
    memset(info, 0, sizeof *info);
    int64_t w = (days + 4) % 7;
    if (w < 0) w += 7; /* 1970-01-01 was a Thursday (4) */
    info->wday = (int32_t)w;
    info->yday = (int32_t)(days - days_from_civil(y, 1, 1));
    info->gmt_offset = offset;
}
int time_to_posix(const TimeCalendar *c, int32_t offset, int64_t *posix) {
    if (c->month < 1 || c->month > 12 || c->day < 1 || c->day > time_days_in_month(c->year, c->month) || c->hour < 0 || c->hour > 23 || c->minute < 0 ||
        c->minute > 59 || c->second < 0 || c->second > 59)
        return -1;
    *posix = days_from_civil(c->year, (unsigned)c->month, (unsigned)c->day) * 86400 + c->hour * 3600 + c->minute * 60 + c->second - offset;
    return 0;
}

void time_rule_make(TimeRule *r, int mode, int32_t offset, const char *name, uint8_t out[TIME_RULE_SIZE]) {
    memset(r, 0, sizeof *r);
    memcpy(r->magic, "NAPLTZ01", 8);
    r->mode = mode;
    r->offset = offset;
    strncpy(r->name, name, sizeof r->name - 1);
    memset(out, 0, TIME_RULE_SIZE);
    memcpy(out, r, sizeof *r);
}
int time_rule_parse(const void *buf, size_t size, TimeRule *out) {
    if (!buf || size < sizeof *out) return -1;
    memcpy(out, buf, sizeof *out);
    return memcmp(out->magic, "NAPLTZ01", 8) ? -1 : 0;
}
int time_location_kind(const char *n, const char *host) {
    static const char *const utc[] = {"UTC", "GMT", "Etc/UTC", "Etc/GMT", "Etc/Universal", "Etc/Zulu", "Universal", "Zulu"};
    for (unsigned i = 0; i < sizeof utc / sizeof utc[0]; i++)
        if (!strcmp(n, utc[i])) return 1;
    return host && *host && !strcmp(n, host) ? 2 : 0;
}
const char *time_zone_from_link(const char *t) {
    const char *p = t ? strstr(t, "zoneinfo/") : NULL;
    return p && p[9] ? p + 9 : NULL;
}

void time_cal_write(uint8_t o[8], const TimeCalendar *c) {
    memset(o, 0, 8);
    memcpy(o, &c->year, 2);
    o[2] = (uint8_t)c->month;
    o[3] = (uint8_t)c->day;
    o[4] = (uint8_t)c->hour;
    o[5] = (uint8_t)c->minute;
    o[6] = (uint8_t)c->second;
}
void time_cal_read(const uint8_t i[8], TimeCalendar *c) {
    memcpy(&c->year, i, 2);
    c->month = (int8_t)i[2];
    c->day = (int8_t)i[3];
    c->hour = (int8_t)i[4];
    c->minute = (int8_t)i[5];
    c->second = (int8_t)i[6];
}
void time_info_write(uint8_t o[0x18], const TimeCalendarInfo *n) {
    memset(o, 0, 0x18);
    memcpy(o, &n->wday, 4);
    memcpy(o + 4, &n->yday, 4);
    memcpy(o + 8, n->zone, 8);
    memcpy(o + 16, &n->is_dst, 4);
    memcpy(o + 20, &n->gmt_offset, 4);
}
void time_info_read(const uint8_t i[0x18], TimeCalendarInfo *n) {
    memcpy(&n->wday, i, 4);
    memcpy(&n->yday, i + 4, 4);
    memcpy(n->zone, i + 8, 8);
    memcpy(&n->is_dst, i + 16, 4);
    memcpy(&n->gmt_offset, i + 20, 4);
}
void time_snapshot_write(const TimeSnapshot *s, uint8_t o[TIME_SNAPSHOT_SIZE]) {
    memset(o, 0, TIME_SNAPSHOT_SIZE);
    memcpy(o, s->user_ctx, 0x20);
    memcpy(o + 0x20, s->network_ctx, 0x20);
    memcpy(o + 0x40, &s->user_time, 8);
    memcpy(o + 0x48, &s->network_time, 8);
    time_cal_write(o + 0x50, &s->user_cal);
    time_cal_write(o + 0x58, &s->network_cal);
    time_info_write(o + 0x60, &s->user_info);
    time_info_write(o + 0x78, &s->network_info);
    memcpy(o + 0x90, &s->steady_value, 8);
    memcpy(o + 0x98, s->steady_source, 16);
    memcpy(o + 0xa8, s->location, 0x24);
    o[0xcc] = s->auto_correction;
    o[0xcd] = s->type;
}
void time_snapshot_read(const uint8_t i[TIME_SNAPSHOT_SIZE], TimeSnapshot *s) {
    memset(s, 0, sizeof *s);
    memcpy(s->user_ctx, i, 0x20);
    memcpy(s->network_ctx, i + 0x20, 0x20);
    memcpy(&s->user_time, i + 0x40, 8);
    memcpy(&s->network_time, i + 0x48, 8);
    time_cal_read(i + 0x50, &s->user_cal);
    time_cal_read(i + 0x58, &s->network_cal);
    time_info_read(i + 0x60, &s->user_info);
    time_info_read(i + 0x78, &s->network_info);
    memcpy(&s->steady_value, i + 0x90, 8);
    memcpy(s->steady_source, i + 0x98, 16);
    memcpy(s->location, i + 0xa8, 0x24);
    s->auto_correction = i[0xcc];
    s->type = i[0xcd];
}
int time_snapshot_span(const TimeSnapshot *a, const TimeSnapshot *b, int64_t *out) {
    if (memcmp(a->steady_source, b->steady_source, 16)) return -1;
    *out = (int64_t)((uint64_t)b->user_time - (uint64_t)a->user_time); /* the snapshots hold guest values: wrap instead of signed overflow */
    return 0;
}
