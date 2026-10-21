/* The alarm clock's arithmetic (alarmtime.c): POSIX TZ strings as Home Assistant sends them, held against glibc's own
 * reading of the same strings, and when alarms ring: fixed cases (midnight, weekdays, both DST changes, the southern
 * hemisphere) and random ones against a brute-force walk over glibc's local time, minute by minute. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "alarmtime.h"

static int bad;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) bad = 1; }

static long long utc(int y, int mo, int d, int h, int mi, int s) { return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s; }

static long glibc_off(long long u)
{
    time_t t = (time_t)u; struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_gmtoff;
}

/* The zones of the world in their shapes: none, whole and half hours, northern and southern DST, negative DST (Dublin),
 * transitions at negative times (Nuuk), half-hour DST (Lord Howe), the J and zero-based day forms */
static const char *const zones[] = {
    "UTC0", "CET-1CEST,M3.5.0,M10.5.0/3", "EST5EDT,M3.2.0,M11.1.0", "AEST-10AEDT,M10.1.0,M4.1.0/3", "<-03>3", "IST-5:30",
    "<+0545>-5:45", "IST-1GMT0,M10.5.0,M3.5.0/1", "<-02>2<-01>,M3.5.0/-1,M10.5.0/0", "NZST-12NZDT,M9.5.0,M4.1.0/3",
    "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", "XXX3YYY,J60/2,J300/2", "XXX3YYY,59,299/1:30", "PST8PDT,M3.2.0/2:00:00,M11.1.0/2:00:00",
};
#define NZONES (int)(sizeof zones / sizeof *zones)

static void against_glibc(void)
{
    char what[160];
    srand(1);
    for (int z = 0; z < NZONES; z++) {
        struct tz tz; int ok = tz_parse(zones[z], &tz) == 0, diff = 0;
        setenv("TZ", zones[z], 1); tzset();
        for (int i = 0; ok && i < 200000; i++) {
            long long u = utc(1971, 1, 1, 0, 0, 0) + (long long)(rand() / (double)RAND_MAX * (utc(2099, 12, 31, 0, 0, 0) - utc(1971, 1, 1, 0, 0, 0)));
            if (i < 2000) u = utc(2026, 3, 29, 0, 0, 0) + (i - 1000) * 37LL * 60;       /* around a change, densely */
            if (i >= 2000 && i < 4000) u = utc(2026, 10, 25, 0, 0, 0) + (i - 3000) * 37LL * 60;
            if (tz_offset(&tz, u) != glibc_off(u)) { if (!diff++) printf("     %s: %lld: %ld, glibc %ld\n", zones[z], u, tz_offset(&tz, u), glibc_off(u)); }
        }
        snprintf(what, sizeof what, "%s: parsed, offset as glibc's at 200000 moments", zones[z]);
        check(ok && !diff, what);
    }
    struct tz tz;
    static const char *const refused[] = { "", "Europe/Berlin", "CET", "CET-1CEST,M3.5.0", "CET-1CEST,M13.5.0,M10.5.0", "CET-1CEST,M3.5.7,M10.5.0",
                                            "CET-1CEST,M3.5.0,M10.5.0/3x", "<-03", "C1", "CET-1:60" };
    int all = 1;
    for (size_t i = 0; i < sizeof refused / sizeof *refused; i++) {
        int r = tz_parse(refused[i], &tz);
        if (r != -1 || tz.std_off || tz.has_dst) { printf("     accepted %s\n", refused[i]); all = 0; }
    }
    check(all, "IANA names, broken and cut-off strings refused, leaving UTC");
    check(tz_parse("EST5EDT", &tz) == 0 && tz.has_dst && tz.dst_off == -4 * 3600 && tz.start.month == 3 && tz.end.month == 11,
          "DST without rules: the US's, an hour ahead");
}

/* Brute force over glibc's local time: the first time the clock passes the alarm's time on an allowed day, from two
 * days before `after` on (so that a repeated hour's second pass is known as the second), minute by minute; inside a
 * skipped hour, as long after the jump as the time lay past where the clock jumped from (the time it lands on exists:
 * Nuuk goes from 22:59 straight to 00:00) */
static long long brute(const struct alarm_def *a, long long after)
{
    long long secs = a->hour * 3600 + a->minute * 60 + a->second, seen[16]; int nseen = 0;
    long long u0 = (after / 60 - 2 * 1440) * 60;
    for (long long u = u0 + 60; u < after + 10 * 86400LL; u += 60) {
        long long l1 = u - 60 + glibc_off(u - 60), l2 = u + glibc_off(u);
        for (long long day = l1 / 86400 - 1; day <= l2 / 86400; day++) {
            long long t = day * 86400 + secs; int dup = 0;
            if (!(l1 < t && t <= l2)) continue;
            if (a->days && !(a->days >> ((int)((day % 7 + 7 + 4) % 7) + 6) % 7 & 1)) continue;
            for (int i = 0; i < nseen; i++) dup |= seen[i] == day;
            if (dup) continue;
            if (nseen < 16) seen[nseen++] = day;
            long long hit = t == l2 ? u : u - 60 + (t - l1);       /* where the clock lands after a jump exists */
            if (hit > after) return hit;
        }
    }
    return -1;
}

static void random_alarms(void)
{
    char what[160];
    srand(2);
    for (int z = 0; z < NZONES; z++) {
        struct tz tz; tz_parse(zones[z], &tz);
        setenv("TZ", zones[z], 1); tzset();
        int diff = 0;
        for (int i = 0; i < 150; i++) {
            /* half of them near this year's changes, where it matters */
            long long base = i % 3 == 0 ? utc(2026, 3, 27, 0, 0, 0) : i % 3 == 1 ? utc(2026, 10, 23, 0, 0, 0) : utc(2026, 1, 1, 0, 0, 0);
            long long after = base + rand() % (i % 3 == 2 ? 365 * 86400 : 5 * 86400);
            struct alarm_def a = { 1, rand() % 24, rand() % 60, 0, rand() % 4 == 0 ? 0 : rand() % 128 };
            if (i % 5 == 0) { a.hour = 2; a.minute = 30; }                            /* inside many zones' changes */
            if (i % 7 == 0) { a.hour = 0; a.minute = rand() % 2 * 30; }
            if (a.days == 0 && i % 2) a.days = 0;
            if (!a.days && rand() % 2) a.days = ALARM_DAILY;
            long long want = brute(&a, after), got = alarm_next(&a, &tz, after);
            if (got != want) { if (!diff++) printf("     %s: after %lld, %02d:%02d days %02x: %lld, brute force %lld\n", zones[z], after, a.hour, a.minute, a.days, got, want); }
        }
        snprintf(what, sizeof what, "%s: 150 random alarms ring when a minute-by-minute walk over glibc's clock says", zones[z]);
        check(!diff, what);
    }
}

static void fixed_cases(void)
{
    struct tz cet, syd, utc0, sp; char s[40];
    tz_parse("CET-1CEST,M3.5.0,M10.5.0/3", &cet); tz_parse("AEST-10AEDT,M10.1.0,M4.1.0/3", &syd); tz_parse("UTC0", &utc0);
    tz_parse("<-03>3", &sp);
    struct alarm_def a = { 1, 0, 0, 10, 0 };
    /* 2026-10-05 is a Monday */
    check(alarm_next(&a, &utc0, utc(2026, 10, 5, 23, 59, 30)) == utc(2026, 10, 6, 0, 0, 10), "00:00:10 once, at 23:59:30: just after midnight");
    check(alarm_next(&a, &utc0, utc(2026, 10, 6, 0, 0, 10)) == utc(2026, 10, 7, 0, 0, 10), "strictly after: at its own second, the next day's");
    a = (struct alarm_def){ 1, 7, 0, 0, ALARM_WEEKDAYS };
    check(alarm_next(&a, &cet, utc(2026, 10, 9, 6, 0, 0)) == utc(2026, 10, 12, 5, 0, 0), "weekdays 07:00, Friday 08:00 CEST: Monday 07:00");
    check(alarm_next(&a, &cet, utc(2026, 10, 9, 4, 59, 59)) == utc(2026, 10, 9, 5, 0, 0), "weekdays 07:00, Friday 06:59:59 CEST: in a second");
    a.days = ALARM_WEEKENDS;
    check(alarm_next(&a, &cet, utc(2026, 10, 5, 12, 0, 0)) == utc(2026, 10, 10, 5, 0, 0), "weekends 07:00, Monday: Saturday");
    a.days = 1 << 2;                                                                     /* Wednesday */
    check(alarm_next(&a, &cet, utc(2026, 10, 7, 5, 0, 0)) == utc(2026, 10, 14, 5, 0, 0), "Wednesdays 07:00, at Wednesday's ring: a week on");
    a.days = ALARM_DAILY;
    check(alarm_next(&a, &cet, utc(2026, 3, 28, 7, 0, 0)) == utc(2026, 3, 29, 5, 0, 0), "daily 07:00 over spring forward: 07:00 CEST = 05:00 UTC");
    check(alarm_next(&a, &cet, utc(2026, 10, 24, 7, 0, 0)) == utc(2026, 10, 25, 6, 0, 0), "daily 07:00 over fall back: 07:00 CET = 06:00 UTC");
    a = (struct alarm_def){ 1, 2, 30, 0, ALARM_DAILY };
    check(alarm_next(&a, &cet, utc(2026, 3, 28, 12, 0, 0)) == utc(2026, 3, 29, 1, 30, 0), "02:30 on the night it is skipped: 03:30 CEST");
    check(alarm_next(&a, &cet, utc(2026, 3, 29, 1, 30, 0)) == utc(2026, 3, 30, 0, 30, 0), "and 02:30 CEST the night after");
    check(alarm_next(&a, &cet, utc(2026, 10, 24, 12, 0, 0)) == utc(2026, 10, 25, 0, 30, 0), "02:30 on the night it comes twice: the first, CEST");
    check(alarm_next(&a, &cet, utc(2026, 10, 25, 0, 30, 0)) == utc(2026, 10, 26, 1, 30, 0), "not the second (CET): the next night's");
    a = (struct alarm_def){ 1, 6, 30, 0, 0 };
    check(alarm_next(&a, &syd, utc(2026, 12, 31, 12, 0, 0)) == utc(2026, 12, 31, 19, 30, 0), "Sydney in summer (DST over new year): 06:30 AEDT = 19:30 UTC");
    check(alarm_next(&a, &syd, utc(2026, 7, 1, 0, 0, 0)) == utc(2026, 7, 1, 20, 30, 0), "Sydney in winter: 06:30 AEST = 20:30 UTC");
    check(alarm_next(&a, &sp, utc(2026, 7, 1, 10, 0, 0)) == utc(2026, 7, 2, 9, 30, 0), "<-03>3: 06:30 = 09:30 UTC");
    a.on = 0;
    check(alarm_next(&a, &cet, utc(2026, 7, 1, 10, 0, 0)) == -1, "off: never");
    iso_utc(utc(2026, 3, 29, 1, 30, 5), s, sizeof s);
    check(!strcmp(s, "2026-03-29T01:30:05+00:00"), s);
    long long y; unsigned m, d; int round = 1;
    for (long long day = -800000; day < 800000; day += 97) { civil_from_days(day, &y, &m, &d); round &= days_from_civil(y, m, d) == day; }
    check(round, "dates round-trip over ±2000 years");
}

int main(void)
{
    against_glibc();
    fixed_cases();
    random_alarms();
    return bad;
}
