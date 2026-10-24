/* time core: calendar maths, time-zone rules and ClockSnapshot (portable; no libc time zone, no host clock). */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { int16_t year; int8_t month, day, hour, minute, second; } TimeCalendar;              /* nn::time::CalendarTime (8 bytes on the wire) */
typedef struct { int32_t wday, yday; char zone[8]; int32_t is_dst, gmt_offset; } TimeCalendarInfo;   /* CalendarAdditionalInfo, 0x18 on the wire */

/* Wire forms of CalendarTime (8 bytes) and CalendarAdditionalInfo (0x18 bytes). */
void time_cal_write(uint8_t out[8], const TimeCalendar *c);
void time_cal_read(const uint8_t in[8], TimeCalendar *c);
void time_info_write(uint8_t out[0x18], const TimeCalendarInfo *i);
void time_info_read(const uint8_t in[0x18], TimeCalendarInfo *i);

/* Seconds since 1970-01-01 UTC <-> civil date, with a fixed offset east of UTC in seconds. */
void time_to_calendar(int64_t posix, int32_t offset, TimeCalendar *cal, TimeCalendarInfo *info);
/* Inverse; returns 0 and sets *posix, or -1 when the fields are out of range (month 1..12, day 1..days in month, hour < 24, ...). */
int time_to_posix(const TimeCalendar *cal, int32_t offset, int64_t *posix);
int time_days_in_month(int year, int month);

/* TimeZoneRule (0x4000 bytes on the wire). The guest treats it as opaque; this host's rules are tiny: a magic, a mode and an offset. */
#define TIME_RULE_SIZE 0x4000u
enum { TIME_RULE_FIXED = 0, TIME_RULE_HOST = 1 };
typedef struct { char magic[8]; int32_t mode, offset; char name[0x24]; } TimeRule;
void time_rule_make(TimeRule *r, int mode, int32_t offset, const char *name, uint8_t out[TIME_RULE_SIZE]);
int time_rule_parse(const void *buf, size_t size, TimeRule *out);          /* 0 ok, -1 not one of ours */
/* Classifies a location name: 1 UTC family (fixed offset 0), 2 the host's zone (`host_name`), 0 unknown. */
int time_location_kind(const char *name, const char *host_name);
/* Extracts the IANA name from the target of /etc/localtime ("../var/db/timezone/zoneinfo/Europe/Berlin" -> "Europe/Berlin"); NULL if absent. */
const char *time_zone_from_link(const char *link_target);

/* ClockSnapshot, 0xD0 bytes. */
#define TIME_SNAPSHOT_SIZE 0xD0u
typedef struct {
    uint8_t user_ctx[0x20], network_ctx[0x20];       /* SystemClockContext: s64 offset, steady time point {s64 value, u8 source[16]} */
    int64_t user_time, network_time;
    TimeCalendar user_cal, network_cal; TimeCalendarInfo user_info, network_info;
    int64_t steady_value; uint8_t steady_source[16];
    char location[0x24]; uint8_t auto_correction, type;
} TimeSnapshot;
void time_snapshot_write(const TimeSnapshot *s, uint8_t out[TIME_SNAPSHOT_SIZE]);
void time_snapshot_read(const uint8_t in[TIME_SNAPSHOT_SIZE], TimeSnapshot *s);
/* Seconds from a to b by user time; 0 and *out set, or -1 when the two snapshots come from different steady clocks. */
int time_snapshot_span(const TimeSnapshot *a, const TimeSnapshot *b, int64_t *out);
