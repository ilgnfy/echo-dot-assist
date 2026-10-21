/* Task manager: where the Echo's CPU and memory go, for Home Assistant's diagnostic entities (proto_esphome.c), and
 * killing a process, which root does for us (scripts/system/main.sh, kill_watch). */
#ifndef TASKMGR_H
#define TASKMGR_H
#include <stddef.h>

#define TASKMGR_TEXT 256        /* an ESPHome text sensor's state: 255 characters at most */
#define TASKMGR_TOP 5           /* entries in "Top processes" */

struct taskmgr_stats {
    int cpu;                    /* the CPU figures below are there: an earlier sample to take the difference to */
    /* CPU shares are of the whole machine (all cores = 100 %), as the "CPU usage" sensor's */
    float mem_used_pct, mem_avail_mb, load1, self_cpu_pct, self_rss_mb;
    int nproc, unreadable, nthreads;    /* processes read; ones whose /proc/<pid>/stat we may not read; our threads */
    double cost_ms, cost_cpu_ms;        /* what taking this sample cost: wall clock, CPU time of the calling thread */
    char top[TASKMGR_TEXT];             /* "name pid cpu% rssMB, ...": the TASKMGR_TOP busiest since the last sample */
    char threads[TASKMGR_TEXT];         /* "N threads: name cpu%, ...": ours, busiest first, as many as fit */
};

/* One thread only (the diag thread): reads /proc, or HASSMIC_PROC (tests), and keeps this sample for the next one's
 * differences.  Processes that came or went in between are handled; unreadable ones are skipped and counted.
 * 0, or -1 when /proc/stat or /proc/meminfo could not be read. */
int taskmgr_sample(struct taskmgr_stats *out);

/* One line of /proc/<pid>/stat.  Exposed for tests/unit/taskmgr_test.c.  0, or -1 when it is not one. */
struct taskmgr_proc {
    int pid, ppid; char state;
    unsigned long long ticks, start;    /* utime + stime; starttime (field 22): with the pid, which process this is */
    long rss;                           /* pages */
    char comm[16];
};
int taskmgr_parse_stat(const char *line, struct taskmgr_proc *p);

/* Killing.  hassmic runs unprivileged and may not signal other users' processes, so it leaves a request in
 * state/kill-request (pid and start time, which defeats a reused pid; the signal) for root's watcher, which applies its
 * own policy and answers in state/kill-result.  changed(): taskmgr_last_kill() has news; called without any lock, from
 * a thread of ours, never from inside taskmgr_kill(). */
void taskmgr_start(void (*changed)(void));
int  taskmgr_kill(int pid, const char *sig, char *why, size_t n);  /* any thread.  sig: "term" (also "") or "kill".
                                                                       0: asked; -1: refused here, why says it */
void taskmgr_last_kill(char *out, size_t n);                        /* any thread; "" before the first */
#endif
