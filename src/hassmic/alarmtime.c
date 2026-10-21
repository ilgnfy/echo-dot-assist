/* The alarm clock's arithmetic: see alarmtime.h. */
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "alarmtime.h"

#define DAY 86400LL

static long long floordiv(long long a, long long b) { return a / b - (a % b != 0 && (a < 0) != (b < 0)); }

/* Howard Hinnant's algorithms: proleptic Gregorian, any year */
long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civil_from_days(long long z, long long *y, unsigned *m, unsigned *d)
{
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (long long)yoe + era * 400 + (*m <= 2);
}

static int weekday(long long days) { return (int)((days % 7 + 7 + 4) % 7); }    /* 0 Sunday; 1970-01-01 was a Thursday */
static int leap(long long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

void iso_utc(long long utc, char *out, unsigned n)
{
    long long y; unsigned m, d; long long s = utc - floordiv(utc, DAY) * DAY;
    civil_from_days(floordiv(utc, DAY), &y, &m, &d);
    snprintf(out, n, "%04lld-%02u-%02uT%02lld:%02lld:%02lld+00:00", y, m, d, s / 3600, s / 60 % 60, s % 60);
}

/* ---------------------------------------------------------------- POSIX TZ */

/* "CET", or "<-03>" / "<+0545>" (tzdata quotes the names that are numbers) */
static const char *tz_name(const char *p)
{
    const char *s = p;
    if (*p == '<') {
        for (p++; *p && *p != '>'; p++) if (!isalnum((unsigned char)*p) && *p != '+' && *p != '-') return NULL;
        return *p == '>' && p - s > 1 ? p + 1 : NULL;
    }
    while (isalpha((unsigned char)*p)) p++;
    return p - s >= 3 ? p : NULL;
}

static const char *num(const char *p, long *v, long max)
{
    if (!isdigit((unsigned char)*p)) return NULL;
    for (*v = 0; isdigit((unsigned char)*p); p++) if ((*v = *v * 10 + (*p - '0')) > max) return NULL;
    return p;
}

/* [+-]hh[:mm[:ss]]; transition times may run from -167 to +167 hours (RFC 8536's extension, which tzdata uses) */
static const char *hms(const char *p, long *out)
{
    long h, m = 0, s = 0; int neg = *p == '-';
    if (*p == '+' || *p == '-') p++;
    if (!(p = num(p, &h, 167))) return NULL;
    if (*p == ':' && (!(p = num(p + 1, &m, 59)) || (*p == ':' && !(p = num(p + 1, &s, 59))))) return NULL;
    *out = (neg ? -1 : 1) * (h * 3600 + m * 60 + s);
    return p;
}

static const char *rule(const char *p, struct tz_rule *r)
{
    long v, w, d;
    memset(r, 0, sizeof *r); r->time = 7200;                        /* 02:00 unless it says */
    if (*p == 'M') {
        if (!(p = num(p + 1, &v, 12)) || v < 1 || *p != '.' || !(p = num(p + 1, &w, 5)) || w < 1 || *p != '.' || !(p = num(p + 1, &d, 6)))
            return NULL;
        r->kind = 'M'; r->month = (int)v; r->week = (int)w; r->wday = (int)d;
    } else if (*p == 'J') {
        if (!(p = num(p + 1, &v, 365)) || v < 1) return NULL;
        r->kind = 'J'; r->day = (int)v;
    } else {
        if (!(p = num(p, &v, 365))) return NULL;
        r->kind = 'D'; r->day = (int)v;
    }
    if (*p == '/' && !(p = hms(p + 1, &r->time))) return NULL;
    return p;
}

int tz_parse(const char *s, struct tz *z)
{
    const char *p = s; long off;
    memset(z, 0, sizeof *z);
    if (!s || !(p = tz_name(p)) || !(p = hms(p, &off))) return -1;
    z->std_off = -off;
    if (!*p) return 0;
    struct tz t = *z;
    if (!(p = tz_name(p))) goto bad;
    t.has_dst = 1; t.dst_off = t.std_off + 3600;                  /* DST is an hour ahead unless it says */
    if (*p && *p != ',') { if (!(p = hms(p, &off))) goto bad; t.dst_off = -off; }
    if (!*p) {                                                      /* no rules: POSIX leaves them to the system; the US's */
        t.start = (struct tz_rule){ 'M', 3, 2, 0, 0, 7200 }; t.end = (struct tz_rule){ 'M', 11, 1, 0, 0, 7200 };
    } else if (*p != ',' || !(p = rule(p + 1, &t.start)) || *p != ',' || !(p = rule(p + 1, &t.end)) || *p) goto bad;
    *z = t;
    return 0;
bad:
    memset(z, 0, sizeof *z);
    return -1;
}

/* the local date (days since 1970) a rule falls on in year y */
static long long rule_day(const struct tz_rule *r, long long y)
{
    long long jan1 = days_from_civil(y, 1, 1);
    if (r->kind == 'J') return jan1 + r->day - 1 + (leap(y) && r->day >= 60);     /* Jn never counts 29 February */
    if (r->kind == 'D') return jan1 + r->day;
    long long first = days_from_civil(y, (unsigned)r->month, 1);
    long long next = r->month == 12 ? days_from_civil(y + 1, 1, 1) : days_from_civil(y, (unsigned)r->month + 1, 1);
    long long d = first + (r->wday - weekday(first) + 7) % 7 + (r->week - 1) * 7LL;
    while (d >= next) d -= 7;                                       /* week 5: the last one */
    return d;
}

long tz_offset(const struct tz *z, long long utc)
{
    if (!z->has_dst) return z->std_off;
    long long y; unsigned m, d;
    civil_from_days(floordiv(utc + z->std_off, DAY), &y, &m, &d);
    /* the start is given in standard time, the end in daylight time */
    long long s = rule_day(&z->start, y) * DAY + z->start.time - z->std_off;
    long long e = rule_day(&z->end, y) * DAY + z->end.time - z->dst_off;
    int dst = s < e ? utc >= s && utc < e : !(utc >= e && utc < s);  /* southern hemisphere: DST spans the new year */
    return dst ? z->dst_off : z->std_off;
}

/* ---------------------------------------------------------------- alarms */

long long alarm_next(const struct alarm_def *a, const struct tz *z, long long after)
{
    if (!a->on) return -1;
    long secs = a->hour * 3600L + a->minute * 60L + a->second;
    long offs[2] = { z->std_off, z->dst_off }; int n = z->has_dst ? 2 : 1;
    long lo = n == 2 && offs[1] < offs[0] ? offs[1] : offs[0];
    long long today = floordiv(after + tz_offset(z, after), DAY);
    /* from yesterday on (the local date of `after` is one of two near a change), a week and a bit */
    for (long long day = today - 1; day <= today + 8; day++) {
        if (a->days && !(a->days >> (weekday(day) + 6) % 7 & 1)) continue;
        long long wall = day * DAY + secs, at = LLONG_MAX;
        for (int i = 0; i < n; i++) {                       /* the local time is the offset's that is in force then */
            long long u = wall - offs[i];
            if (tz_offset(z, u) == offs[i] && u < at) at = u;   /* repeated hour: the first of the two */
        }
        if (at == LLONG_MAX) at = wall - lo;                /* skipped hour: as if the clock had not gone forward */
        if (at > after) return at;
    }
    return -1;
}
