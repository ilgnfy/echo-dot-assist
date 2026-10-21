/* Every thread of ours names itself as it starts, so that the task manager (taskmgr.c, "hassmic threads" in Home
 * Assistant), scripts/top.sh and top -H tell them apart; unnamed they all show the process's "hassmic".  The kernel keeps
 * 15 characters and cuts the rest.  Not the main thread: its name is the process's, which pidof matches (main.sh). */
#ifndef THREADNAME_H
#define THREADNAME_H
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
static inline void thread_name(const char *name) { prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0); }

/* Threads a library starts itself cannot call thread_name: libpryon's decoder runs the wake word model in a worker of
 * its own, created inside PryonDecoder_NewSpotterAudioDecoder, and that thread did 16-20 % of all CPU on the Dot 3 as
 * an anonymous "hassmic" (task manager, 2026-10-06; debuggerd -b: only libpryon frames).  So the caller takes the
 * process's threads before such a call (thread_tids) and names those that came up during it (thread_name_new): a new
 * thread inherits its creator's name, ours rename themselves at once, so one still carrying the caller's name is the
 * library's.  A sibling's name is written through /proc/self/task/<tid>/comm (allowed within the thread group). */
#define THREAD_TIDS_MAX 64
struct thread_tids { int n; int tid[THREAD_TIDS_MAX]; };

static inline void thread_tids(struct thread_tids *s)
{
    DIR *d = opendir("/proc/self/task"); struct dirent *e;
    s->n = 0;
    if (!d) return;
    while ((e = readdir(d)) && s->n < THREAD_TIDS_MAX) if (e->d_name[0] >= '0' && e->d_name[0] <= '9') s->tid[s->n++] = atoi(e->d_name);
    closedir(d);
}

static inline int thread_comm(int tid, char *out, size_t n)
{
    char p[48]; int f, r;
    snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
    if ((f = open(p, O_RDONLY)) < 0) return -1;
    r = read(f, out, n - 1); close(f);
    if (r <= 0) return -1;
    out[r] = 0; out[strcspn(out, "\n")] = 0;
    return 0;
}

static inline void thread_name_new(const struct thread_tids *before, const char *name)
{
    struct thread_tids now; char mine[24], theirs[24], p[48];
    if (thread_comm((int)syscall(SYS_gettid), mine, sizeof mine)) return;
    thread_tids(&now);
    for (int i = 0; i < now.n; i++) {
        int j, f;
        for (j = 0; j < before->n && before->tid[j] != now.tid[i]; j++) ;
        if (j < before->n || thread_comm(now.tid[i], theirs, sizeof theirs) || strcmp(theirs, mine)) continue;
        snprintf(p, sizeof p, "/proc/self/task/%d/comm", now.tid[i]);
        if ((f = open(p, O_WRONLY)) >= 0) { if (write(f, name, strlen(name)) < 0) { /* named or not, it runs */ } close(f); }
    }
}
#endif
