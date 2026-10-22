/* AirPlay 2.  shairport-sync does the protocol (pairing, RTSP, the encrypted audio, the clock through nqptp, which root's main.sh
 * runs: UDP 319/320 are privileged); this file runs it as a child while the setting is on, writes its configuration, and
 * plays what it writes to a FIFO (S16, 44.1 kHz, stereo, at the pace of the sender's clock) on the music stream.
 * Its mDNS records are avahi service files in the daemon's directory (mdns_external.c of tools/airplay/patches).
 * Music Assistant bridges plain AirPlay (version 1) to an Echo already; this is the native one, synchronised with
 * other AirPlay 2 speakers by the clock nqptp shares.  No Apple software here; the sender may be any iPhone, Mac or app. */
#define _GNU_SOURCE
#include "airplay.h"
#include "audio.h"
#include "btout.h"
#include "core.h"
#include "statefile.h"
#include "threadname.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RATE 44100
#define CHANNELS 2
#define MIXER_TARGET_US 120000      /* as sendspin.c: little queued in the mixer, so a stop or a duck is heard at once */
#define GAP_MS 1500                 /* no audio this long: the sender paused or left */
/* all in 16384-32767, where lockdown.sh admits inbound TCP and UDP (shairport-sync opens "any port" sockets in the base's range) */
#define RTSP_PORT 28933
#define UDP_BASE 28940
#define UDP_RANGE 40

/* Output latency of the mixer's Music stream after our write (mixer, DAC, amp): sendspin.c measured 83..88 ms with src/tools/latency.c
 * including the capture path, ~70 ms without; HASSMIC_OUTPUT_LATENCY_MS as there. */
#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif
#define OUT_LATENCY_MS 70
#define FIFO_BYTES 4096             /* the pipe's own buffer: 23 ms, so its fill is not a latency of its own (the default 64 KB would be 370 ms) */
static atomic_int enabled, delay_ms, playing, yield_req, conf_total_ms;
static atomic_int child_pid;
static char fifo_path[300], conf_path[300];

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }
static const char *avahi_dir(void) { const char *e = getenv("HASSMIC_AVAHI_DIR"); return e ? e : "/data/misc/avahi/services"; }

/* next to the running binary, or HASSMIC_SHAIRPORT (the PC's tests: a script that writes a tone) */
static const char *binary(void)
{
    static char p[300];
    const char *e = getenv("HASSMIC_SHAIRPORT");
    if (e) return e;
    ssize_t n = readlink("/proc/self/exe", p, sizeof p - 20);
    if (n <= 0) return "";
    p[n] = 0;
    char *s = strrchr(p, '/');
    if (!s) return "";
    strcpy(s + 1, "shairport-sync");
    return p;
}

int airplay_present(void) { return access(binary(), X_OK) == 0; }
int airplay_enable(int set) { if (set >= 0) atomic_store(&enabled, set != 0); return atomic_load(&enabled); }
/* What lies between shairport-sync's write and the sound: the mixer queue kept near its target (writes wait for it to fall under it), the
 * FIFO half full, the output and a Bluetooth speaker's buffer.  Shairport-sync is told to hand audio out that much early, plus the
 * setting's trim; this is what a fixed number measured by ear used to guess. */
static int total_ms(void)
{
    const char *e = getenv("HASSMIC_OUTPUT_LATENCY_MS");
    int out = e ? atoi(e) : OUT_LATENCY_MS;
    return (MIXER_TARGET_US - 10000) / 1000 + FIFO_BYTES / 2 * 1000 / (RATE * 2 * CHANNELS) + out + (int)(btout_latency_us() / 1000) + atomic_load(&delay_ms);
}
int airplay_delay(int set) { if (set != INT_MIN) atomic_store(&delay_ms, set); return atomic_load(&delay_ms); }
int airplay_playing(void) { return atomic_load(&playing); }
void airplay_yield(void) { if (atomic_load(&playing)) atomic_store(&yield_req, 1); }

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

/* shairport-sync's configuration (libconfig): the name people see, ports in the admitted range, the FIFO, and how early it
 * must hand out audio for the speaker's own delay (the mixer queue and the output) */
static int conf_total_ms_set(void) { int t = total_ms(); atomic_store(&conf_total_ms, t); return t; }
static int write_conf(void)
{
    struct statefile f;
    if (statefile_open(&f, conf_path, 0600)) return -1;
    fprintf(f.f,
        "general = {\n  name = \"%s\";\n  port = %d;\n  udp_port_base = %d;\n  udp_port_range = %d;\n  output_backend = \"pipe\";\n"
        "  audio_backend_latency_offset_in_seconds = %.3f;\n  mdns_backend = \"external-avahi\";\n"
        "  ignore_volume_control = \"no\";\n};\n"
        "pipe = {\n  name = \"%s\";\n  output_format = \"S16\";\n  output_rate = %d;\n  output_channels = 2;\n};\n"   /* the backend's own section: from general they are ignored and AirPlay 2 gives S32 at 48 kHz */
        "sessioncontrol = {\n  allow_session_interruption = \"yes\";\n  session_timeout = 60;\n};\n"
        "diagnostics = {\n  log_verbosity = 2;\n};\n",
        core_name, RTSP_PORT, UDP_BASE, UDP_RANGE, -conf_total_ms_set() / 1000.0, fifo_path, RATE);
    return statefile_commit(&f);
}

static void remove_records(void)
{
    char p[320];
    snprintf(p, sizeof p, "%s/airplay-raop.service", avahi_dir()); unlink(p);
    snprintf(p, sizeof p, "%s/airplay-ap2.service", avahi_dir()); unlink(p);
}

static void child_start(void)
{
    if (write_conf()) { fprintf(stderr, "airplay: cannot write %s\n", conf_path); return; }
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() == 1) _exit(1);
        setenv("HASSMIC_AVAHI_DIR", avahi_dir(), 1);
        execl(binary(), "shairport-sync", "-c", conf_path, (char *)NULL);
        fprintf(stderr, "airplay: cannot run %s: %s\n", binary(), strerror(errno));
        _exit(127);
    }
    atomic_store(&child_pid, pid);
    fprintf(stderr, "airplay: shairport-sync started as \"%s\"\n", core_name);
}

static void child_stop(const char *why)
{
    pid_t pid = atomic_exchange(&child_pid, 0);
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 30; i++) { if (waitpid(pid, NULL, WNOHANG) == pid) goto gone; usleep(100000); }
    kill(pid, SIGKILL); waitpid(pid, NULL, 0);
gone:
    remove_records();
    fprintf(stderr, "airplay: shairport-sync stopped (%s)\n", why);
}

static void *supervisor(void *arg)
{
    long long next = 0; int fast = 0;
    (void)arg;
    thread_name("airplay");
    remove_records();                                   /* left by a kill -9 */
    for (;;) {
        int want = atomic_load(&enabled) && airplay_present();
        pid_t pid = atomic_load(&child_pid);
        if (atomic_exchange(&yield_req, 0) && pid > 0) { child_stop("another source plays"); next = now_ms() + 1000; pid = 0; }
        if (!want && pid > 0) { child_stop("switched off"); pid = 0; }
        if (pid > 0 && !atomic_load(&playing) && total_ms() != atomic_load(&conf_total_ms)) { child_stop("delay changed"); next = now_ms(); pid = 0; }
        if (pid > 0) {
            int st;
            if (waitpid(pid, &st, WNOHANG) == pid) {
                atomic_store(&child_pid, 0); remove_records();
                fprintf(stderr, "airplay: shairport-sync exited (%d), again in %d s\n", WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st), fast < 5 ? 2 : 30);
                next = now_ms() + (fast++ < 5 ? 2000 : 30000);
            } else if (now_ms() > next + 60000) fast = 0;
        } else if (want && now_ms() >= next) { child_start(); if (atomic_load(&child_pid)) next = now_ms(); else next = now_ms() + 30000; }
        sleep(1);
    }
    return NULL;
}

/* The sender's audio.  The FIFO is ours (mode 0600, opened for reading before the child opens it for writing); shairport-sync
 * writes while a sender plays and nothing while it is paused, so a gap is the end. */
static void *reader(void *arg)
{
    static char buf[4096];
    size_t have = 0;                        /* bytes of a frame the last read ended in the middle of: dropping them would shift every sample after */
    int fd = -1, open_music = 0, sized = 0;
    (void)arg;
    thread_name("airplay-audio");
    for (;;) {
        if (fd < 0 && (fd = open(fifo_path, O_RDONLY | O_NONBLOCK)) < 0) { sleep(2); continue; }
        if (fd >= 0 && !sized) sized = fcntl(fd, F_SETPIPE_SZ, FIFO_BYTES) >= 0;
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, GAP_MS);
        if (r > 0 && (p.revents & POLLIN)) {
            ssize_t n = read(fd, buf + have, sizeof buf - have);
            if (n > 0 && atomic_load(&enabled)) {
                size_t total = have + (size_t)n, whole = total - total % (2 * CHANNELS);
                have = total - whole;
                n = (ssize_t)whole;
                if (!open_music) {
                    if (music_open(RATE, CHANNELS)) { usleep(200000); continue; }
                    open_music = 1; atomic_store(&playing, 1); core_music(MUSIC_AIRPLAY, 1);
                    fprintf(stderr, "airplay: playing\n");
                }
                while (music_queued_us() > MIXER_TARGET_US && !atomic_load(&yield_req)) usleep(5000);
                if (!atomic_load(&yield_req) && n > 0) music_write(buf, (size_t)n);
                if (have) memmove(buf, buf + n, have);
            } else if (n == 0) usleep(100000);          /* the writer left: poll reports HUP at once while no writer is there */
            continue;
        }
        if (r > 0 && (p.revents & (POLLHUP | POLLERR))) { usleep(200000); continue; }       /* no writer yet */
        if (open_music) {                                                   /* a gap */
            music_close(); open_music = 0; have = 0; atomic_store(&playing, 0); core_music(MUSIC_AIRPLAY, 0);
            fprintf(stderr, "airplay: stopped\n");
        }
    }
    return NULL;
}

void airplay_init(void)
{
    pthread_t t;
    snprintf(fifo_path, sizeof fifo_path, "%s/airplay.pipe", state_dir());
    snprintf(conf_path, sizeof conf_path, "%s/airplay.conf", state_dir());
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0600)) { fprintf(stderr, "airplay: cannot make %s: %s\n", fifo_path, strerror(errno)); return; }
    if (!pthread_create(&t, NULL, reader, NULL)) pthread_detach(t);
    if (!pthread_create(&t, NULL, supervisor, NULL)) pthread_detach(t);
}
