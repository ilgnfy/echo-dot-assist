/*
 * Receiving end of scripts/ota-push.sh.  Runs inside hassmic, i.e. unprivileged and network-facing, so it decides nothing:
 * it checks the bundle's signature (to refuse junk before it touches the disk), stores bundle + signature in state/ota/
 * and drops a "request" file.  The root-side loop in main.sh verifies again with the tool and key from the read-only
 * system partition, installs, and writes "result", which is relayed to the pusher.
 *
 *   -> "HMOTA-PUSH1 <bundle bytes>\n"  <64 byte signature>  <bundle>
 *   <- one line: "OK <version>" | "FAILED <why>"
 *
 * The same key opens adb over Wi-Fi (adbwifi.c) without Home Assistant: the way back in when the Echo is not adopted,
 * has lost its key, or runs Wyoming, and its USB wires are gone.  Challenge and response, so that a recorded exchange
 * cannot be played again; what is signed starts differently from every bundle ("HMOTA1\n"), so neither passes for the
 * other.
 *   -> "HMOTA-ADB1\n"
 *   <- "NONCE <64 hex digits>\n"
 *   -> <64 byte signature over "HMOTA-ADB1\n" + the 32 nonce bytes>
 *   <- one line: "OK ..." once root's firewall watcher has opened it | "FAILED <why>"
 *
 * An update that passes its self test (main.c) becomes the factory copy on the system partition, the one the Echo falls
 * back to: ota_healthy() tells root's installer loop, which checks that it is the installed update and that the running
 * hassmic is its binary, then writes it (scripts/system/sysinstall.sh).
 *
 * This port takes the owner's key only (update.pub): there are no updates fetched from the internet.
 */
#include "ota.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../third_party/monocypher.h"
#include "adbwifi.h"
#include "net.h"
#include "netio.h"
#include "statefile.h"

#define MAX_BUNDLE (16u << 20)
#define HEAD_MS    10000        /* the request line */
#define SIGN_MS    20000        /* then the signature over the nonce: ota-push.sh signs in well under a second */
#define BODY_MS    30000        /* signature and bundle: this, plus a second per MIN_RATE bytes it announced */
#define MIN_RATE   (64u << 10)  /* bytes/s; Wi-Fi to the Echo does megabytes per second.  16 MiB: 4 min 46 s at most */
static int port;

static pthread_mutex_t handoff_lock = PTHREAD_MUTEX_INITIALIZER;     /* one bundle in state/ota/ at a time */

static const char *pub_path(void) { const char *e = getenv("HASSMIC_UPDATE_PUB"); return e ? e : "/system/hassmic/update.pub"; }
/* main.sh names the release key of the copy that runs (an update may bring a new one), else the factory copy's */
static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }

static void reply(int fd, const char *line) { char b[300]; int n = snprintf(b, sizeof b, "%s\n", line); write_all(fd, b, n); fprintf(stderr, "update: %s\n", line); }

static int store(const char *dir, const char *name, const void *data, size_t len)
{
    char path[300]; snprintf(path, sizeof path, "%s/%s", dir, name);
    return statefile_write(path, 0666, data, len);
}

static int load_key(const char *path, uint8_t pk[32])
{
    FILE *f = fopen(path, "rb");
    int ok = f && fread(pk, 1, 32, f) == 32;
    if (f) fclose(f);
    return ok;
}

/* 0 if the peer signs "<line>\n" + a fresh nonce with the update key; otherwise it has been answered already. */
static int challenge(int fd, const uint8_t pk[32], const char *line, long long by)
{
    uint8_t msg[160], sig[64]; char hex[80]; size_t ll = strlen(line); int r = open("/dev/urandom", O_RDONLY);
    memcpy(msg, line, ll); msg[ll++] = '\n';
    if (r < 0 || read(r, msg + ll, 32) != 32) { if (r >= 0) close(r); reply(fd, "FAILED no randomness"); return -1; }
    close(r);
    int n = sprintf(hex, "NONCE ");
    for (int i = 0; i < 32; i++) n += sprintf(hex + n, "%02x", msg[ll + i]);
    hex[n++] = '\n';
    if (write_all(fd, hex, n) || net_read_full_by(fd, sig, 64, by) != 64) { reply(fd, "FAILED no signature"); return -1; }
    if (crypto_eddsa_check(sig, pk, msg, ll + 32)) { reply(fd, "FAILED signature does not verify against this device's update key"); return -1; }
    return 0;
}

/* Root's answer in DIR/NAME (the installer loop looks every 2 s), waited for up to SECS. */
static void answer(const char *dir, const char *name, int secs, char *res, size_t cap)
{
    char path[300]; FILE *f; snprintf(path, sizeof path, "%s/%s", dir, name);
    for (int t = 0; t < secs * 2; t++) {
        if ((f = fopen(path, "r"))) {
            res[0] = 0; if (fgets(res, cap, f)) res[strcspn(res, "\n")] = 0;
            fclose(f);
            if (!res[0]) snprintf(res, cap, "FAILED empty result");
            return;
        }
        usleep(500000);
    }
    snprintf(res, cap, "FAILED the installer did not answer (is the hassmic_fw service running?)");
}

static void relay(int fd, const char *dir, const char *name, int secs)
{
    char res[256]; answer(dir, name, secs, res, sizeof res); reply(fd, res);
}

/* Bundle and signature into state/ota/ with a request for root's installer; 0 if stored. */
static int stage(const uint8_t *b, size_t len, const uint8_t sig[64], char *dir, size_t dircap)
{
    char path[300];
    snprintf(dir, dircap, "%s/ota", state_dir()); mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/result", dir); unlink(path);
    return store(dir, "bundle", b, len) || store(dir, "bundle.sig", sig, 64) || store(dir, "request", "1\n", 2) ? -1 : 0;
}

void ota_healthy(void)
{
    char dir[280];
    snprintf(dir, sizeof dir, "%s/ota", state_dir()); mkdir(dir, 0700);
    if (store(dir, "healthy", "1\n", 2)) fprintf(stderr, "self test: cannot tell the installer\n");
}

static void adb_open(int fd, const uint8_t pk[32], long long by)
{
    if (challenge(fd, pk, "HMOTA-ADB1", by)) return;
    fprintf(stderr, "update: adb over Wi-Fi asked for with the update key\n");
    adbwifi_ask(1);
    for (int t = 0; t < 30; t++) {                      /* the firewall watcher looks every 5 s */
        if (adbwifi_granted()) { reply(fd, "OK adb over Wi-Fi open for 30 min"); return; }
        usleep(500000);
    }
    reply(fd, "FAILED the firewall service did not answer (is hassmic_fw running?)");
}

static void handle(int fd)
{
    char line[128], dir[280]; unsigned long len = 0; uint8_t sig[64], pk[32], *b;
    long long t0 = net_mono_ms();
    /* One connection at a time, so each gets an end it cannot push out: a per-read timeout let a peer sending a byte
     * every 29 s hold the port, and with it the adb way back in, for ever */
    if (net_read_until(fd, line, sizeof line, "\n", t0 + HEAD_MS) <= 0) { reply(fd, "FAILED bad request"); return; }
    line[strcspn(line, "\n")] = 0;
    int adb = !strcmp(line, "HMOTA-ADB1");
    if (!adb && (sscanf(line, "HMOTA-PUSH1 %lu", &len) != 1 || !len || len > MAX_BUNDLE)) { reply(fd, "FAILED bad request"); return; }
    if (!load_key(pub_path(), pk)) { reply(fd, "FAILED this device has no update key (install-system.sh installs it)"); return; }
    if (adb) { adb_open(fd, pk, t0 + HEAD_MS + SIGN_MS); return; }
    if (!(b = malloc(len))) { reply(fd, "FAILED out of memory"); return; }
    long long by = net_mono_ms() + BODY_MS + (long long)(len / MIN_RATE) * 1000;
    if (net_read_full_by(fd, sig, 64, by) != 64 || net_read_full_by(fd, b, len, by) != (ssize_t)len) {
        reply(fd, "FAILED upload incomplete or too slow"); free(b); return;
    }
    if (crypto_eddsa_check(sig, pk, b, len)) { reply(fd, "FAILED signature does not verify against this device's update key"); free(b); return; }

    pthread_mutex_lock(&handoff_lock);
    if (stage(b, len, sig, dir, sizeof dir)) reply(fd, "FAILED cannot store the bundle");
    else { fprintf(stderr, "update: %lu bytes received, signature good, handed to the installer\n", len); relay(fd, dir, "result", 60); }
    pthread_mutex_unlock(&handoff_lock);
    free(b);
}

static void *listener(void *arg)
{
    int ls = net_listen(port);
    (void)arg;
    if (ls < 0) { perror("update: listen"); return NULL; }
    fprintf(stderr, "update: push port %d\n", port);
    for (;;) { int c = net_accept(ls); if (c < 0) break; handle(c); close(c); }      /* one at a time */
    return NULL;
}

int ota_start(int p)
{
    pthread_t t;
    port = p;
    return pthread_create(&t, NULL, listener, NULL) ? -1 : (pthread_detach(t), 0);
}
