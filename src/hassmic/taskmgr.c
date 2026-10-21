/*
 * Task manager (taskmgr.h).  A sample reads /proc/stat, /proc/meminfo, /proc/loadavg, /proc/uptime, every
 * /proc/<pid>/stat and our own /proc/self/task/<tid>/stat: about 200 small files on an Echo.  CPU use is the difference
 * of each process's utime + stime to the sample before, against the difference of the machine's total jiffies, so all
 * cores are 100 %.  A pid alone does not say which process it is (they are reused), the pid with its start time does.
 * Into static buffers swapped from one sample to the next: the sampler runs every 10 s and should not churn the heap.
 *
 * hassmic runs as an unprivileged user under SELinux: another user's /proc/<pid>/stat may be unreadable.  Such
 * processes are left out quietly and counted (stats.unreadable), and cannot be killed through us either.
 *
 * Killing: see taskmgr.h and scripts/system/main.sh (kill_watch), which decides what may be killed.
 */
#include "taskmgr.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "threadname.h"

#define PROC_MAX 512            /* processes kept per sample; an Echo Dot 3 runs about 200, kernel threads included */
#define THREAD_MAX 128          /* our threads; about 30 with everything on */
#define KILL_ANSWER_S 20        /* root looks every 2 s, and gives a process 3 s for TERM and 1 more for KILL */

/* ---------------------------------------------------------------- reading /proc */

static const char *proc_root(void) { const char *p = getenv("HASSMIC_PROC"); return p && *p ? p : "/proc"; }

/* A whole small file into buf, NUL-terminated: its length, or -1.  No stdio: no buffer allocated per file. */
static long read_small(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC); long len = 0;
    if (fd < 0) return -1;
    for (ssize_t r; len < (long)n - 1 && (r = read(fd, buf + len, n - 1 - len)) != 0; ) {
        if (r < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        len += r;
    }
    close(fd);
    buf[len] = 0;
    return len;
}

/* The command name is in parentheses and may hold anything, spaces and ")" included: the fields start after the last
 * ")".  Then: state, ppid, then (counting from 1 over the whole line) utime 14, stime 15, starttime 22, rss 24. */
int taskmgr_parse_stat(const char *line, struct taskmgr_proc *p)
{
    const char *l = strchr(line, '('), *r = strrchr(line, ')'), *q;
    unsigned long long f[21]; char *end;
    if (!l || !r || r < l || !isdigit((unsigned char)line[0])) return -1;
    p->pid = atoi(line);
    size_t n = (size_t)(r - l - 1) < sizeof p->comm - 1 ? (size_t)(r - l - 1) : sizeof p->comm - 1;
    memcpy(p->comm, l + 1, n); p->comm[n] = 0;
    for (q = r + 1; *q == ' '; q++) ;
    if (!*q || q[1] != ' ') return -1;
    p->state = *q++;
    for (int k = 0; k < 21; k++) {                      /* ppid .. rss; tpgid and priority may be negative: not used */
        f[k] = strtoull(q, &end, 10);
        if (end == q) return -1;
        q = end;
    }
    p->ppid = (int)f[0]; p->ticks = f[10] + f[11]; p->start = f[18]; p->rss = (long)f[20];
    return 0;
}

/* The machine's jiffies so far (user .. steal of the "cpu" line), as the "CPU usage" sensor counts them */
static int read_total(unsigned long long *total)
{
    char buf[512], path[300]; unsigned long long v[8] = { 0 };
    snprintf(path, sizeof path, "%s/stat", proc_root());
    if (read_small(path, buf, sizeof buf) < 0) return -1;
    if (sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 4) return -1;
    *total = 0;
    for (int i = 0; i < 8; i++) *total += v[i];
    return 0;
}

/* MemAvailable is the kernel's own estimate of what can be had without swapping (3.14 on); before it, free + caches */
static int read_mem(float *used_pct, float *avail_mb)
{
    char buf[4096], path[300]; unsigned long long total = 0, avail = 0, free_ = 0, buffers = 0, cached = 0; int have_avail = 0;
    snprintf(path, sizeof path, "%s/meminfo", proc_root());
    if (read_small(path, buf, sizeof buf) < 0) return -1;
    for (char *l = buf; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
        unsigned long long kb;
        if (sscanf(l, "MemTotal: %llu", &kb) == 1) total = kb;
        else if (sscanf(l, "MemAvailable: %llu", &kb) == 1) { avail = kb; have_avail = 1; }
        else if (sscanf(l, "MemFree: %llu", &kb) == 1) free_ = kb;
        else if (sscanf(l, "Buffers: %llu", &kb) == 1) buffers = kb;
        else if (sscanf(l, "Cached: %llu", &kb) == 1) cached = kb;
    }
    if (!total) return -1;
    if (!have_avail) avail = free_ + buffers + cached;
    if (avail > total) avail = total;
    *used_pct = 100.0f * (float)(total - avail) / (float)total;
    *avail_mb = (float)avail / 1024.0f;
    return 0;
}

static float read_load1(void)
{
    char buf[128], path[300]; float l = -1;
    snprintf(path, sizeof path, "%s/loadavg", proc_root());
    if (read_small(path, buf, sizeof buf) < 0 || sscanf(buf, "%f", &l) != 1) return -1;
    return l;
}

/* Seconds since boot in clock ticks: what starttime counts in */
static unsigned long long read_uptime_ticks(long hz)
{
    char buf[128], path[300]; double up = 0;
    snprintf(path, sizeof path, "%s/uptime", proc_root());
    if (read_small(path, buf, sizeof buf) < 0 || sscanf(buf, "%lf", &up) != 1) return 0;
    return (unsigned long long)(up * hz);
}

/* ---------------------------------------------------------------- samples and differences */

struct ent {
    struct taskmgr_proc p;
    unsigned long long d;       /* ticks since the previous sample */
    int known;                  /* d is right: the process was in the previous sample, or started after it */
};
struct set {
    struct ent *e; int n, cap;
    unsigned long long uptime;  /* clock ticks since boot when it was taken */
    int valid;
};
static struct ent proc_buf[2][PROC_MAX], thread_buf[2][THREAD_MAX];
static struct set procs[2] = { { proc_buf[0], 0, PROC_MAX, 0, 0 }, { proc_buf[1], 0, PROC_MAX, 0, 0 } };
static struct set threads[2] = { { thread_buf[0], 0, THREAD_MAX, 0, 0 }, { thread_buf[1], 0, THREAD_MAX, 0, 0 } };
static int cur;                                 /* index of the newest in procs[] and threads[] */
static unsigned long long last_total, self_ticks;
static int have_last;

static int by_pid(const void *a, const void *b)
{
    const struct ent *x = a, *y = b;
    return (x->p.pid > y->p.pid) - (x->p.pid < y->p.pid);
}

/* Every <number>/stat under dir into s, sorted by pid.  *unreadable: entries that were there but could not be read */
static void read_dir(const char *dir, struct set *s, int *unreadable)
{
    char path[320], buf[1024]; DIR *d = opendir(dir); struct dirent *de;
    s->n = 0;
    if (!d) return;
    while ((de = readdir(d)) && s->n < s->cap) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        snprintf(path, sizeof path, "%s/%s/stat", dir, de->d_name);
        /* gone in between, or not ours to read (SELinux, hidepid) */
        if (read_small(path, buf, sizeof buf) < 0 || taskmgr_parse_stat(buf, &s->e[s->n].p)) { if (unreadable) ++*unreadable; continue; }
        s->n++;
    }
    closedir(d);
    qsort(s->e, s->n, sizeof *s->e, by_pid);
}

/* Each entry's ticks since prev.  Not in prev: started after it (all its ticks are new), or there all along and not read
 * then; the latter has no difference to show and stays out of the ranking. */
static void diff(struct set *now, const struct set *prev)
{
    for (int i = 0; i < now->n; i++) {
        struct ent *e = &now->e[i], key = { .p.pid = e->p.pid };
        const struct ent *o = prev->valid ? bsearch(&key, prev->e, prev->n, sizeof *prev->e, by_pid) : NULL;
        e->known = 0; e->d = 0;
        if (o && o->p.start == e->p.start) { e->known = 1; e->d = e->p.ticks >= o->p.ticks ? e->p.ticks - o->p.ticks : 0; }
        else if (prev->valid && e->p.start >= prev->uptime) { e->known = 1; e->d = e->p.ticks; }
    }
}

/* Index of the busiest known entry not taken yet (more memory breaks a tie), or -1 */
static int busiest(const struct set *s, const unsigned char *taken)
{
    int b = -1;
    for (int i = 0; i < s->n; i++) {
        const struct ent *e = &s->e[i];
        if (taken[i] || !e->known) continue;
        if (b < 0 || e->d > s->e[b].d || (e->d == s->e[b].d && e->p.rss > s->e[b].p.rss)) b = i;
    }
    return b;
}

/* Appends one entry if the whole of it fits: a cut entry would read as a different name or number */
static void append(char *out, size_t n, const char *s)
{
    size_t have = strlen(out), add = strlen(s);
    if (have + add < n) memcpy(out + have, s, add + 1);
}

static void format_top(char *out, size_t n, const struct set *s, double dtotal, long page)
{
    unsigned char taken[PROC_MAX] = { 0 }; char one[96];
    out[0] = 0;
    for (int k = 0; k < TASKMGR_TOP; k++) {
        int i = busiest(s, taken);
        if (i < 0) break;
        taken[i] = 1;
        const struct ent *e = &s->e[i];
        snprintf(one, sizeof one, "%s%s %d %.1f%% %ldMB", k ? ", " : "", e->p.comm, e->p.pid, 100.0 * e->d / dtotal,
                 (long)((e->p.rss * (long long)page + (1 << 19)) >> 20));
        append(out, n, one);
    }
}

static void format_threads(char *out, size_t n, const struct set *s, double dtotal)
{
    unsigned char taken[THREAD_MAX] = { 0 }; char one[64]; int listed = 0;
    snprintf(out, n, "%d threads", s->n);
    for (;;) {
        int i = busiest(s, taken);
        if (i < 0 || !s->e[i].d) break;                 /* the idle ones would only crowd out the next sample's */
        taken[i] = 1;
        snprintf(one, sizeof one, "%s%s %.1f%%", listed++ ? ", " : ": ", s->e[i].p.comm, 100.0 * s->e[i].d / dtotal);
        append(out, n, one);
    }
    if (!listed) append(out, n, ", none busy");
}

static double ms_of(clockid_t c)
{
    struct timespec t; clock_gettime(c, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int taskmgr_sample(struct taskmgr_stats *out)
{
    double t0 = ms_of(CLOCK_MONOTONIC), c0 = ms_of(CLOCK_THREAD_CPUTIME_ID);
    long hz = sysconf(_SC_CLK_TCK), page = sysconf(_SC_PAGESIZE);
    char dir[300], buf[1024]; unsigned long long total; struct taskmgr_proc me;
    memset(out, 0, sizeof *out);
    if (hz <= 0) hz = 100;
    if (page <= 0) page = 4096;
    if (read_total(&total) || read_mem(&out->mem_used_pct, &out->mem_avail_mb)) return -1;
    out->load1 = read_load1();
    int prev = cur; cur ^= 1;
    struct set *p = &procs[cur], *t = &threads[cur];
    p->uptime = t->uptime = read_uptime_ticks(hz);
    read_dir(proc_root(), p, &out->unreadable);
    snprintf(dir, sizeof dir, "%s/self/task", proc_root());
    read_dir(dir, t, NULL);
    out->nproc = p->n; out->nthreads = t->n;
    snprintf(dir, sizeof dir, "%s/self/statm", proc_root());
    { unsigned long size, res; if (read_small(dir, buf, sizeof buf) > 0 && sscanf(buf, "%lu %lu", &size, &res) == 2) out->self_rss_mb = (float)res * page / 1048576.0f; }
    snprintf(dir, sizeof dir, "%s/self/stat", proc_root());
    int me_ok = read_small(dir, buf, sizeof buf) > 0 && !taskmgr_parse_stat(buf, &me);
    if (have_last && total > last_total) {
        double dtotal = (double)(total - last_total);
        diff(p, &procs[prev]); diff(t, &threads[prev]);
        format_top(out->top, sizeof out->top, p, dtotal, page);
        format_threads(out->threads, sizeof out->threads, t, dtotal);
        out->self_cpu_pct = me_ok && me.ticks >= self_ticks ? (float)(100.0 * (me.ticks - self_ticks) / dtotal) : 0;
        out->cpu = 1;
    }
    p->valid = t->valid = 1;
    last_total = total; have_last = 1;
    if (me_ok) self_ticks = me.ticks;
    out->cost_ms = ms_of(CLOCK_MONOTONIC) - t0; out->cost_cpu_ms = ms_of(CLOCK_THREAD_CPUTIME_ID) - c0;
    return 0;
}

/* ---------------------------------------------------------------- kill (taskmgr.h) */

static pthread_mutex_t kill_lock = PTHREAD_MUTEX_INITIALIZER;
static char last_kill[TASKMGR_TEXT];           /* kill_lock */
static int kill_pending;                        /* kill_lock: a request is out, its waiter runs */
static void (*on_change)(void);

static const char *state_file(char *p, size_t n, const char *name)
{
    const char *e = getenv("HASSMIC_STATE");
    snprintf(p, n, "%s/%s", e ? e : "/data/local/hassmic/state", name);
    return p;
}

static void set_last(const char *s)
{
    pthread_mutex_lock(&kill_lock); snprintf(last_kill, sizeof last_kill, "%s", s); pthread_mutex_unlock(&kill_lock);
}

void taskmgr_last_kill(char *out, size_t n)
{
    pthread_mutex_lock(&kill_lock); snprintf(out, n, "%s", last_kill); pthread_mutex_unlock(&kill_lock);
}

/* Root's answer, printable ASCII on one line (it names the process root saw, which anyone can name as they like), and
 * gone once read.  0 when there was one. */
static int take_result(char *out, size_t n)
{
    char path[320], buf[TASKMGR_TEXT]; size_t j = 0;
    if (read_small(state_file(path, sizeof path, "kill-result"), buf, sizeof buf) < 0) return -1;
    unlink(path);
    for (const char *c = buf; *c && *c != '\n' && j < n - 1; c++) out[j++] = *c >= 0x20 && *c < 0x7f ? *c : '?';
    out[j] = 0;
    return 0;
}

static void *kill_waiter(void *arg)
{
    char res[TASKMGR_TEXT], path[320];
    (void)arg;
    thread_name("kill wait");
    for (int i = 0; i < KILL_ANSWER_S * 4; i++) {
        usleep(250000);
        if (take_result(res, sizeof res)) continue;
        fprintf(stderr, "kill: %s\n", res);
        set_last(res);
        goto done;
    }
    unlink(state_file(path, sizeof path, "kill-request"));
    fprintf(stderr, "kill: no answer from root within %d s (is the hassmic_fw service running?), request withdrawn\n", KILL_ANSWER_S);
    set_last("FAILED no answer from root (is the hassmic_fw service running?)");
done:
    pthread_mutex_lock(&kill_lock); kill_pending = 0; pthread_mutex_unlock(&kill_lock);
    if (on_change) on_change();
    return NULL;
}

/* "term" (the default, also ""), "kill", with or without SIG, any case, or the numbers */
static const char *sig_name(const char *s)
{
    if (!s || !*s) return "TERM";
    if (!strncasecmp(s, "sig", 3)) s += 3;
    if (!strcasecmp(s, "term") || !strcmp(s, "15")) return "TERM";
    if (!strcasecmp(s, "kill") || !strcmp(s, "9")) return "KILL";
    return NULL;
}

/* why says it to the caller (the log), "Last kill" to Home Assistant.  -1 */
__attribute__((format(printf, 3, 4))) static int refuse(char *why, size_t n, const char *fmt, ...)
{
    char s[TASKMGR_TEXT]; va_list a;
    va_start(a, fmt); vsnprintf(why, n, fmt, a); va_end(a);
    snprintf(s, sizeof s, "FAILED %.*s", (int)sizeof s - 8, why);
    set_last(s);
    return -1;
}

/* the request in one go, under a temporary name: root never reads half of one */
static int write_request(const char *path, const char *line)
{
    char tmp[340]; size_t len = strlen(line); int fd, ok;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)) < 0) return -1;
    ok = write(fd, line, len) == (ssize_t)len;
    if (close(fd)) ok = 0;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

int taskmgr_kill(int pid, const char *sig, char *why, size_t n)
{
    char path[320], buf[1024], line[96], s[TASKMGR_TEXT]; struct taskmgr_proc p; pthread_t t;
    const char *sn = sig_name(sig);
    if (!sn) return refuse(why, n, "signal must be term or kill (pid %d)", pid);
    if (pid < 1) return refuse(why, n, "no process %d", pid);
    snprintf(path, sizeof path, "%s/%d/stat", proc_root(), pid);
    /* root checks the start time against the process it finds under that pid: a request for one that ended meanwhile
     * cannot hit whatever got its number */
    if (read_small(path, buf, sizeof buf) < 0 || taskmgr_parse_stat(buf, &p) || p.pid != pid)
        return refuse(why, n, "no process %d, or not one hassmic may read", pid);
    pthread_mutex_lock(&kill_lock);
    int busy = kill_pending; kill_pending = 1;
    pthread_mutex_unlock(&kill_lock);
    if (busy) { snprintf(why, n, "another kill is still under way"); return -1; }   /* its answer stays the last word */
    unlink(state_file(path, sizeof path, "kill-result"));
    snprintf(line, sizeof line, "%d %llu %s\n", pid, p.start, sn);
    if (write_request(state_file(path, sizeof path, "kill-request"), line)) {
        pthread_mutex_lock(&kill_lock); kill_pending = 0; pthread_mutex_unlock(&kill_lock);
        return refuse(why, n, "cannot write the request for %d", pid);
    }
    snprintf(s, sizeof s, "asked: %s to %s (%d)", sn, p.comm, pid);
    set_last(s);
    fprintf(stderr, "kill: %s\n", s);
    if (pthread_create(&t, NULL, kill_waiter, NULL)) {
        unlink(path);
        pthread_mutex_lock(&kill_lock); kill_pending = 0; pthread_mutex_unlock(&kill_lock);
        return refuse(why, n, "no thread to wait for the answer for %d", pid);
    }
    pthread_detach(t);
    return 0;
}

/* A result nobody took: the kill was of hassmic itself, and root answered after it went */
void taskmgr_start(void (*changed)(void))
{
    char res[TASKMGR_TEXT];
    on_change = changed;
    if (take_result(res, sizeof res)) return;
    fprintf(stderr, "kill: %s (answered before this start)\n", res);
    set_last(res);
}
