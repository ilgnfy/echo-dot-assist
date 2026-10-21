/* The task manager (taskmgr.c) against a /proc made of files in a temporary directory (HASSMIC_PROC): parsing of
 * /proc/<pid>/stat, the CPU differences between two samples with processes that came, went, were reused or could not
 * be read before, the top five, our threads, the 255-character limit of a text state, and the kill request that root
 * takes (HASSMIC_STATE). */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "taskmgr.h"

static int bad;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) bad = 1; }
static char root[256], state[256];

__attribute__((format(printf, 2, 3))) static void put(const char *rel, const char *fmt, ...)
{
    char path[512], dir[512]; va_list a;
    snprintf(path, sizeof path, "%s/%s", root, rel);
    for (char *s = strchr(strcpy(dir, path) + strlen(root) + 1, '/'); s; s = strchr(s + 1, '/')) { *s = 0; mkdir(dir, 0755); *s = '/'; }
    FILE *f = fopen(path, "w");
    va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fclose(f);
}

/* A /proc/<pid>/stat line as the kernel writes it: utime = ticks, stime 0 */
static void stat_at(const char *rel, int pid, const char *comm, int ppid, unsigned long long ticks, unsigned long long start, long rss)
{
    put(rel, "%d (%s) S %d %d %d 0 -1 4194560 0 0 0 0 %llu 0 0 0 20 0 1 0 %llu 1000000 %ld 18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
        pid, comm, ppid, pid, pid, ticks, start, rss);
}

static void proc(const char *dir, int pid, const char *comm, int ppid, unsigned long long ticks, unsigned long long start, long rss)
{
    char rel[128];
    snprintf(rel, sizeof rel, "%s/%d/stat", dir, pid);
    stat_at(rel, pid, comm, ppid, ticks, start, rss);
}

static void machine(unsigned long long busy, unsigned long long idle, double uptime)
{
    put("stat", "cpu  %llu 0 0 %llu 0 0 0 0 0 0\ncpu0 1 0 0 1 0 0 0 0 0 0\nintr 1\n", busy, idle);
    put("meminfo", "MemTotal:         500000 kB\nMemFree:           50000 kB\nMemAvailable:     200000 kB\nBuffers:            1000 kB\nCached:            90000 kB\n");
    put("loadavg", "1.25 0.80 0.50 2/250 4242\n");
    put("uptime", "%.2f 3000.00\n", uptime);
}

static void parse(void)
{
    struct taskmgr_proc p;
    int ok = !taskmgr_parse_stat("42 (mixer) S 1 42 42 0 -1 4194560 10 0 0 0 150 50 0 0 -2 -20 12 0 777 1000 300 1844674407370", &p);
    check(ok && p.pid == 42 && p.ppid == 1 && p.state == 'S' && p.ticks == 200 && p.start == 777 && p.rss == 300 && !strcmp(p.comm, "mixer"),
          "stat line: pid, ppid, state, utime + stime, starttime, rss (negative priority and nice in between)");
    ok = !taskmgr_parse_stat("7 (a) R 2 (x) S 9 7 7 0 -1 0 0 0 0 0 5 6 0 0 20 0 1 0 88 0 3", &p);
    check(ok && !strcmp(p.comm, "a) R 2 (x") && p.ppid == 9 && p.ticks == 11 && p.start == 88,
          "a name with spaces and parentheses: the fields start after the last \")\"");
    ok = !taskmgr_parse_stat("8 (a-very-long-process-name) S 1 1 1 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 5 0 1", &p);
    check(ok && !strcmp(p.comm, "a-very-long-pro"), "a name longer than the kernel keeps is cut to 15");
    check(taskmgr_parse_stat("9 (x) S 1 2 3", &p) && taskmgr_parse_stat("garbage", &p) && taskmgr_parse_stat("(x) S 1", &p)
          && taskmgr_parse_stat("10 (x)", &p), "cut-off and broken lines refused");
}

static void sampling(void)
{
    struct taskmgr_stats s; char want[TASKMGR_TEXT], what[600];
    long page = sysconf(_SC_PAGESIZE), hz = sysconf(_SC_CLK_TCK);
    long mb10 = 10L * 1048576 / page;                   /* pages in 10 MB */
    /* first sample: uptime 1000 s */
    machine(10000, 30000, 1000);
    proc(".", 1, "init", 0, 500, 1, mb10 / 10);
    proc(".", 100, "mixer", 1, 1000, 300, mb10 * 2);
    proc(".", 200, "busy", 1, 2000, 400, mb10);
    proc(".", 300, "gone", 1, 50, 500, mb10);
    proc(".", 500, "hassmic", 1, 400, 600, mb10);
    proc(".", 700, "reused", 1, 9000, 700, mb10);
    put("800/status", "Name: secret\n");             /* a directory whose stat we may not read */
    stat_at("self/stat", 500, "hassmic", 1, 400, 600, mb10);
    put("self/statm", "5000 %ld 100 10 0 1000 0\n", mb10);
    proc("self/task", 1001, "capture", 1, 100, 600, mb10);
    proc("self/task", 1002, "mic sender", 1, 10, 600, mb10);
    proc("self/task", 1003, "diag", 1, 5, 600, mb10);
    check(!taskmgr_sample(&s) && !s.cpu && s.nproc == 6 && s.unreadable == 1 && s.nthreads == 3,
          "first sample: 6 processes, 1 unreadable, 3 threads, no CPU figures yet");
    snprintf(what, sizeof what, "memory 60.0 %% used, 195 MB available, load 1.25, hassmic 10.0 MB: %.1f %.0f %.2f %.1f",
             s.mem_used_pct, s.mem_avail_mb, s.load1, s.self_rss_mb);
    check(s.mem_used_pct > 59.99f && s.mem_used_pct < 60.01f && (int)(s.mem_avail_mb + 0.5f) == 195 && s.load1 == 1.25f
          && s.self_rss_mb > 9.99f && s.self_rss_mb < 10.01f, what);
    check(s.cost_ms >= 0 && s.cost_cpu_ms >= 0, "the sample's cost is measured");

    /* second: 1000 jiffies later.  300 went, 400 came after the first sample, 700 is a new process under an old pid,
     * 900 was there all along but not readable then (no difference to show), 800 still unreadable */
    machine(10600, 30400, 1010);
    proc(".", 1, "init", 0, 505, 1, mb10 / 10);
    proc(".", 100, "mixer", 1, 1100, 300, mb10 * 2);
    proc(".", 200, "busy", 1, 2300, 400, mb10);
    char rel[300]; snprintf(rel, sizeof rel, "%s/300/stat", root); unlink(rel);
    proc(".", 400, "new", 1, 50, 1000 * hz + 50, mb10);
    proc(".", 500, "hassmic", 1, 430, 600, mb10);
    proc(".", 700, "reused", 1, 20, 1000 * hz + 70, mb10);
    proc(".", 900, "hidden", 1, 99999, 200, mb10);
    stat_at("self/stat", 500, "hassmic", 1, 430, 600, mb10);
    proc("self/task", 1001, "capture", 1, 180, 600, mb10);
    proc("self/task", 1002, "mic sender", 1, 20, 600, mb10);
    proc("self/task", 1003, "diag", 1, 5, 600, mb10);
    check(!taskmgr_sample(&s) && s.cpu && s.nproc == 7, "second sample: CPU figures");
    snprintf(want, sizeof want, "busy 200 30.0%% 10MB, mixer 100 10.0%% 20MB, new 400 5.0%% 10MB, hassmic 500 3.0%% 10MB, reused 700 2.0%% 10MB");
    snprintf(what, sizeof what, "top five: gone, unknown history and the sixth left out: \"%s\"", s.top);
    check(!strcmp(s.top, want), what);
    snprintf(what, sizeof what, "our threads, busiest first, idle ones left out: \"%s\"", s.threads);
    check(!strcmp(s.threads, "3 threads: capture 8.0%, mic sender 1.0%"), what);
    check(s.self_cpu_pct > 2.99f && s.self_cpu_pct < 3.01f, "hassmic's own CPU: 3.0 %");

    /* third: nothing moved for our threads; then many long names: every list stays within 255 characters, whole entries */
    machine(10700, 30800, 1015);
    proc("self/task", 1001, "capture", 1, 180, 600, mb10);
    proc("self/task", 1002, "mic sender", 1, 20, 600, mb10);
    check(!taskmgr_sample(&s) && !strcmp(s.threads, "3 threads, none busy"), "no thread busy: said so");
    for (int i = 0; i < 60; i++) {
        char n[16]; snprintf(n, sizeof n, "long-name-%05d", i);
        proc("self/task", 2000 + i, n, 1, 0, 600, mb10);
        proc(".", 2000 + i, n, 1, 0, 600, 999999);
    }
    machine(10800, 31000, 1020);
    taskmgr_sample(&s);
    for (int i = 0; i < 60; i++) {
        char n[16]; snprintf(n, sizeof n, "long-name-%05d", i);
        proc("self/task", 2000 + i, n, 1, 1000 + i, 600, mb10);
        proc(".", 2000 + i, n, 1, 1000 + i, 600, 99999999);
    }
    machine(30800, 101000, 1030);
    taskmgr_sample(&s);
    size_t lt = strlen(s.threads), lp = strlen(s.top);
    snprintf(what, sizeof what, "63 busy threads: %zu characters, ends with a whole entry: \"...%s\"", lt, s.threads + (lt > 30 ? lt - 30 : 0));
    check(lt <= 255 && lt > 200 && !strncmp(s.threads, "63 threads: long-name-00059 ", 28) && s.threads[lt - 1] == '%', what);
    snprintf(what, sizeof what, "top five with long names and big ones: %zu characters: \"%s\"", lp, s.top);
    check(lp <= 255 && !strncmp(s.top, "long-name-00059 2059 ", 21) && strstr(s.top, "long-name-00055"), what);
}

static int changes;
static void changed(void) { changes++; }

static void killing(void)
{
    char why[160], last[TASKMGR_TEXT], path[300], buf[128] = "", what[400]; struct stat st;
    taskmgr_start(changed);
    taskmgr_last_kill(last, sizeof last);
    check(!last[0], "no kill yet: Last kill empty");
    check(taskmgr_kill(100, "hup", why, sizeof why) && strstr(why, "term or kill"), "a signal other than term or kill refused");
    taskmgr_last_kill(last, sizeof last);
    check(!strncmp(last, "FAILED signal must be", 21), "... and Last kill says so");
    check(taskmgr_kill(4242, "", why, sizeof why) && strstr(why, "no process 4242"), "a pid that is not there refused");
    snprintf(path, sizeof path, "%s/kill-request", state);
    check(access(path, F_OK) != 0, "... and nothing asked of root");
    int r = taskmgr_kill(200, "", why, sizeof why);
    FILE *f = fopen(path, "r"); if (f) { if (!fgets(buf, sizeof buf, f)) buf[0] = 0; fclose(f); }
    snprintf(what, sizeof what, "request for pid 200 with its start time, TERM by default, mode 600: \"%s\"", strtok(buf, "\n") ? buf : "");
    check(!r && !strcmp(buf, "200 400 TERM") && !stat(path, &st) && (st.st_mode & 0777) == 0600, what);
    taskmgr_last_kill(last, sizeof last);
    check(!strcmp(last, "asked: TERM to busy (200)"), "Last kill: asked");
    check(taskmgr_kill(100, "KILL", why, sizeof why) && strstr(why, "under way"), "a second kill while the first is under way refused");
    /* root's answer */
    snprintf(path, sizeof path, "%s/kill-result", state);
    f = fopen(path, "w"); fputs("OK busy (200) ended on TERM\x01\n", f); fclose(f);
    for (int i = 0; i < 40 && !changes; i++) usleep(50000);
    taskmgr_last_kill(last, sizeof last);
    snprintf(what, sizeof what, "root's answer taken and gone, control characters out: \"%s\"", last);
    check(changes == 1 && !strcmp(last, "OK busy (200) ended on TERM?") && access(path, F_OK) != 0, what);
    check(!taskmgr_kill(100, "sigkill", why, sizeof why), "the next kill is taken once the first is answered");
}

int main(void)
{
    snprintf(root, sizeof root, "/tmp/taskmgr_test.XXXXXX");
    snprintf(state, sizeof state, "/tmp/taskmgr_state.XXXXXX");
    if (!mkdtemp(root) || !mkdtemp(state)) { perror("mkdtemp"); return 1; }
    setenv("HASSMIC_PROC", root, 1); setenv("HASSMIC_STATE", state, 1);
    parse();
    sampling();
    killing();
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf %s %s", root, state);
    if (system(cmd)) return 1;
    printf(bad ? "FAILED\n" : "all good\n");
    return bad;
}
