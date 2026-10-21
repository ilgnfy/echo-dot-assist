/* The alarm clock: alarms that ring on the Echo itself, also while Home Assistant or the network is down (alarms.c).
 *
 * The Echo has no clock of its own to go by once it is locked down (no NTP, egress firewalled, and whether its RTC
 * keeps the time over a reboot is not known), so the wall clock is Home Assistant's: asked for over the ESPHome API
 * when it connects and every hour, and kept as an offset to the kernel's boot clock, which runs on through Home
 * Assistant going away.  hassmic restarting (an update, a crash) keeps it: the offset is saved with the kernel's boot id.
 * After a reboot there is none until Home Assistant has answered, and until then no alarm rings, rather than at a wrong
 * time.  Everything below takes core_lock's protection: called with it held, except alarms_init. */
#ifndef ALARMS_H
#define ALARMS_H
#include "alarmtime.h"

#define ALARM_SLOTS     3
#define ALARM_RING_S    600         /* rings for at most 10 minutes, like stock */
#define ALARM_SNOOZE_S  540         /* stock's 9 minutes */
#define ALARM_LATE_S    600         /* an alarm the clock came too late for (Home Assistant answering after a reboot)
                                     * still rings if it is at most this late; later, it is skipped */

/* the repeat select's options: the days they stand for (alarm_def.days) */
extern const char *const alarm_repeat_names[];
extern const int alarm_repeat_days[];
extern const int alarm_nrepeat;

void alarms_init(void);                         /* main() before any thread: the state files */
void *alarm_thread(void *arg);                  /* rings them, twice a second; tells the client (proto->alarms_changed) */
void alarms_time(long long epoch, const char *tz);  /* Home Assistant's clock: UTC seconds and its POSIX TZ string */
int  alarms_clock(void);                        /* 1: the wall clock is known */
void alarms_get(int slot, struct alarm_def *out);
void alarms_set(int slot, const struct alarm_def *a);  /* kept in state/alarms; occurrences before now never ring */
int  alarms_ringing(void);                      /* the slot whose alarm rings, -1: none */
long long alarms_next(void);                    /* UTC seconds of the next ring (a snoozed one too); -1: none, or no clock */
void alarms_stop(void);                         /* stops a ringing alarm (a timer's too) and a snoozed one */
int  alarms_snooze(void);                       /* a ringing alarm goes quiet and rings again in 9 minutes; 0: none rang */
#endif
