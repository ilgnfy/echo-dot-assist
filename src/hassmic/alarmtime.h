/* The alarm clock's arithmetic, without clocks or state (alarms.c has those; tests/unit/alarmtime_test.c checks this):
 * the time zone as Home Assistant hands it over, and when an alarm rings next.
 *
 * Home Assistant answers GetTimeRequest with the epoch and a POSIX TZ string ("CET-1CEST,M3.5.0,M10.5.0/3", the last
 * line of the zone's tzdata file, as ESPHome's time component takes it).  It is parsed here rather than handed to the
 * libc: bionic's tzset() reads TZ from the environment of the whole process (not thread safe, and every other
 * localtime() would follow it), and this way the PC build computes exactly what the Echo does. */
#ifndef ALARMTIME_H
#define ALARMTIME_H

/* A DST transition: POSIX "Mm.w.d", "Jn" or "n", at `time` seconds of local time (may be negative or past 24 h) */
struct tz_rule { char kind; int month, week, wday, day; long time; };          /* kind 'M', 'J' or 'D' (zero-based n) */
/* Offsets in seconds east of UTC (local = UTC + offset; POSIX writes them the other way round) */
struct tz { long std_off, dst_off; int has_dst; struct tz_rule start, end; };

int  tz_parse(const char *posix, struct tz *out);   /* 0, or -1 if it is not a TZ string this understands (out: UTC) */
long tz_offset(const struct tz *tz, long long utc); /* the offset in force at that moment */

long long days_from_civil(long long y, unsigned m, unsigned d);                 /* 1970-01-01 = 0 */
void civil_from_days(long long days, long long *y, unsigned *m, unsigned *d);
void iso_utc(long long utc, char *out, unsigned n);                            /* "2026-10-06T05:30:00+00:00" */

/* An alarm: local time of day, and the weekdays it repeats on (bit 0 Monday .. bit 6 Sunday; 0: once, the next time
 * that time of day comes) */
#define ALARM_DAILY    0x7f
#define ALARM_WEEKDAYS 0x1f
#define ALARM_WEEKENDS 0x60
struct alarm_def { int on, hour, minute, second, days; };

/* The first ring strictly after `after` (UTC seconds), or -1 when it is off.  A time that a DST change skips rings
 * that much later on the clock (02:30 on the night clocks go forward: 03:30); one it repeats rings the first time only. */
long long alarm_next(const struct alarm_def *a, const struct tz *tz, long long after);
#endif
