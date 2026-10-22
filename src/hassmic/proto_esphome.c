/*
 * ESPHome native API, server side: Home Assistant's esphome integration connects to us and sees a voice assistant device
 * with a media player.  Message numbers and fields follow esphome/components/api/api.proto.
 *
 *   encryption       Noise like ESPHome's `api: encryption:` without a key in the YAML.  The device starts without a key;
 *                    Home Assistant generates one and sets it (NoiseEncryptionSetKeyRequest over a Noise connection with
 *                    the all-zero PSK, so it cannot be sniffed), then connects with it.  From then on only that key gets
 *                    in.  Deleting the device in Home Assistant sends an empty key, which starts over; so does deleting
 *                    state/api_key.
 *
 *   voice pipeline   VoiceAssistantRequest -> mic audio as VoiceAssistantAudio (API_AUDIO) -> events.  The reply is fetched
 *                    over http like an announcement: without the SPEAKER flag Home Assistant renders TTS in the media
 *                    player's announcement format (48 kHz) instead of streaming 16 kHz over the API connection.
 *   announcements    VoiceAssistantAnnounceRequest with http URLs; Home Assistant transcodes to the WAV format the media
 *                    player entity advertises, we stream it.  Same path for media_player.play_media.
 *   buttons          the event entity "Button": a press or a 1 s hold of the action and volume keys, beside what
 *                    they do on the Echo (core_button_events)
 *   wake word        the event entity "Wake word" (core_wake_events): taken, near miss, answered by another device,
 *                    nothing said after it.  Listed while the feature is on
 *   light ring       a light with RGB, brightness and effects, steps aside while the Echo shows something of its own
 *                    (ring.h); listed where root installed the ring's animations
 *   timers           finished timers ring (core_timer_finished); "Timer ringing" and "Ringing timers" (their names)
 *                    say so until stopped or rung out, for automations; the button "Stop ringing" and the media
 *                    player's stop stop them
 *   do not disturb   a switch; announcements are dropped while it is on
 *   arbitration      "Join arbitration network" switch and "Arbitration peers"; the action "arbitration_key" (Home
 *                    Assistant names it esphome.<node>_arbitration_key) through which other Echos hand over their
 *                    network key, and the HomeassistantActionRequest with which this one hands over its own (arb.c)
 *   bluetooth proxy  LE scanning with raw advertisements, GATT connections to up to 3 devices at a time, pairing (ble.c)
 *   bluetooth speaker  a switch opens the pairing window (a2dp.c).  The other way, playing to a Bluetooth speaker: a
 *              switch searches for one and pairs it, another plays on it, a text sensor says how it is, a number holds its
 *              latency for Sendspin (a2dp.c, btout.c).  A phone connecting is announced by asking Home
 *                    Assistant to run assist_satellite.announce on us (HomeassistantActionRequest, what an ESPHome YAML
 *                    `homeassistant.action` sends).  Home Assistant only runs it with "Allow the device to perform Home
 *                    Assistant actions" ticked in the device's options; otherwise it raises a repair saying so.
 *   debug access     a switch opens adb over Wi-Fi for 30 min (adbwifi.c); only taken over the connection with the key
 *   wifi motion      experimental, off by default: a switch, a motion binary sensor, its sensitivity (wifimotion.c);
 *                    listed only where the Wi-Fi driver can do it
 *   drop in          the action "drop_in" (esphome.<node>_drop_in, target: another Echo's name or node) calls an Echo
 *                    of the network (dropin.c); "Drop In" says idle / calling / ringing / connected, "Drop In with"
 *                    whom, the button "End Drop In" hangs up, the event entity "Drop In result" says what came of each
 *                    call this Echo made (dropin_results).  Listed while the feature is on
 *   volumes          the media player's volume is the music's (Home Assistant's media too); with separate volumes the
 *                    numbers "Assistant volume", "Sounds volume", "Timer volume" and "Call volume" (with Drop In) hold
 *                    the others (main.c), each only while that kind has a volume of its own rather than following the
 *                    music's
 */
#ifdef RELEASE
#define RUNNING_VERSION VERSION               /* a build CI published (Makefile RELEASE) */
#else
#define RUNNING_VERSION VERSION "+" BUILD
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdatomic.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "a2dp.h"
#include "adbwifi.h"
#include "airplay.h"
#include "apmeta.h"
#include "improv.h"
#include "alarms.h"
#include "arb.h"
#include "diag.h"
#include "dropin.h"
#include "settings.h"
#include "ble.h"
#include "ring.h"
#include "board.h"
#include "core.h"
#include "micgain.h"
#include "hash.h"
#include "https.h"
#include "noise.h"
#include "outq.h"
#include "sendspin.h"
#include "taskmgr.h"
#include "threadname.h"
#include "clock.h"
#include "wifimotion.h"
#include "net.h"
#include "netio.h"
#include "statefile.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD                 /* plain C: same code on the PC and on the armv7 build */
#include "../third_party/minimp3.h"

enum {
    HELLO_REQ = 1, HELLO_RESP, CONNECT_REQ, CONNECT_RESP, DISCONNECT_REQ, DISCONNECT_RESP, PING_REQ, PING_RESP,
    DEVICE_INFO_REQ, DEVICE_INFO_RESP, LIST_ENTITIES_REQ, LIST_ENTITIES_DONE = 19, SUBSCRIBE_STATES = 20,
    SUBSCRIBE_HA_ACTIONS = 34, HA_ACTION, GET_TIME_REQ = 36, GET_TIME_RESP, SUBSCRIBE_HA_STATES = 38, HA_STATE_SUB, HA_STATE, LIST_SERVICE = 41, EXECUTE_SERVICE,
    LIST_BINARY_SENSOR = 12, LIST_SENSOR = 16, LIST_SWITCH = 17, LIST_TEXT_SENSOR = 18, SENSOR_STATE = 25, SWITCH_STATE = 26, BINARY_SENSOR_STATE = 21, TEXT_SENSOR_STATE = 27, SWITCH_COMMAND = 33, LIST_NUMBER = 49, NUMBER_STATE, NUMBER_COMMAND,
    LIST_SELECT = 52, SELECT_STATE, SELECT_COMMAND,
    LIST_MEDIA_PLAYER = 63, MEDIA_PLAYER_STATE, MEDIA_PLAYER_COMMAND,
    SUBSCRIBE_VA = 89, VA_REQUEST, VA_RESPONSE, VA_EVENT, VA_AUDIO = 106, VA_TIMER_EVENT = 115,
    VA_ANNOUNCE = 119, VA_ANNOUNCE_FINISHED, VA_CONFIG_REQ, VA_CONFIG_RESP, VA_SET_CONFIG, SET_KEY_REQ = 124, SET_KEY_RESP,
    BLE_SUBSCRIBE = 66, BLE_DEVICE_REQ = 68, BLE_CONNECTION, BLE_SERVICES_REQ, BLE_SERVICES, BLE_SERVICES_DONE, BLE_READ_REQ,
    BLE_READ, BLE_WRITE_REQ, BLE_READ_DESC_REQ, BLE_WRITE_DESC_REQ, BLE_NOTIFY_REQ, BLE_NOTIFY_DATA, BLE_CONN_FREE_REQ,
    BLE_CONN_FREE, BLE_GATT_ERROR, BLE_WRITTEN, BLE_NOTIFY, BLE_PAIRED, BLE_UNPAIRED, BLE_UNSUBSCRIBE, BLE_CACHE_CLEARED,
    BLE_RAW_ADV = 93, BLE_SCANNER_STATE = 126, BLE_SCANNER_SET_MODE = 127, LIST_EVENT = 107, EVENT = 108,
    LIST_TIME = 103, TIME_STATE, TIME_COMMAND, LIST_BUTTON = 61, BUTTON_COMMAND = 62,
    LIST_LIGHT = 15, LIGHT_STATE = 24, LIGHT_COMMAND = 32,
};
enum { COLOR_MODE_RGB = 35 };           /* on/off, brightness and RGB: the light ring's one mode */
/* Proxy features: passive scan, active connections, remote caching (Home Assistant keeps the GATT database and writes the
 * notification descriptors itself), pairing, raw advertisements, scanner state and mode.  Not: cache clearing (there is
 * no cache on the Echo), connection parameters.  Scanner state: idle / running; mode: passive / active.
 * Device requests: connect (0, and the v3 variants 4 and 5, all the same here), disconnect, pair, unpair, clear cache. */
enum { BLE_FEATURES = 1 | 2 | 4 | 8 | 32 | 64, BLE_IDLE = 0, BLE_RUNNING = 2, BLE_PASSIVE = 0, BLE_ACTIVE = 1 };
enum { BLE_REQ_CONNECT = 0, BLE_REQ_DISCONNECT, BLE_REQ_PAIR, BLE_REQ_UNPAIR, BLE_REQ_CLEAR_CACHE = 6 };
enum { EV_ERROR = 0, EV_RUN_START, EV_RUN_END, EV_STT_START, EV_STT_END, EV_INTENT_START, EV_INTENT_END, EV_TTS_START,
       EV_TTS_END, EV_WAKE_START, EV_WAKE_END, EV_VAD_START, EV_VAD_END, EV_TTS_STREAM_START = 98, EV_TTS_STREAM_END = 99, EV_INTENT_PROGRESS = 100 };
enum { FEAT_VOICE = 1, FEAT_SPEAKER = 2, FEAT_API_AUDIO = 4, FEAT_TIMERS = 8, FEAT_ANNOUNCE = 16, FEAT_START_CONVERSATION = 32 };
/* KEY_NOISE, KEY_MULT and KEY_ARB_HANDOFF: retired entities (noise suppression, mic volume multiplier, arbitration
 * handoff), kept so the others keep their keys */
enum { KEY_NOISE = 2, KEY_MIC_LEVEL, KEY_MULT, KEY_MUTE, KEY_WAKE_SOUND, KEY_SENDSPIN_TOKEN, KEY_SOC_TEMP, KEY_CPU_USAGE, KEY_BT_PAIRING,
       KEY_BT_ANNOUNCE, KEY_DND, KEY_EQ_BASS, KEY_EQ_MID, KEY_EQ_TREBLE, KEY_BT_LANG, KEY_ARB_JOIN, KEY_ARB_PEERS, KEY_ARB_SERVICE,
       KEY_SS_UNPAIRED, KEY_DENOISE, KEY_ADB_WIFI, KEY_LUX, KEY_LED_AUTO, KEY_LED_BRIGHTNESS, KEY_SOUND_DETECTION, KEY_SOUND,
       KEY_BT_OUT_SEARCH, KEY_BT_OUT, KEY_BT_OUT_STATUS, KEY_BT_OUT_DELAY, KEY_WIFI_MOTION_ON, KEY_WIFI_MOTION, KEY_WIFI_MOTION_SENS,
       KEY_FIRMWARE, KEY_ARB_HANDOFF, KEY_WEB_URL, KEY_IDENTIFY, KEY_RESET,
       KEY_TIMER_RINGING, KEY_TIMER_NAMES, KEY_DROPIN_SERVICE, KEY_DROPIN_STATE, KEY_DROPIN_PEER, KEY_DROPIN_END, KEY_RESTART,
       KEY_DROPIN_RESULT, KEY_TIMER_STOP, KEY_BUTTON, KEY_LIGHT, KEY_ASSIST_VOLUME, KEY_SOUNDS_VOLUME, KEY_TIMER_VOLUME,
       KEY_CALL_VOLUME, KEY_WAKE, KEY_RING_FILL, KEY_RING_ROTATION, KEY_RING_LEDS, KEY_BT_AAC, KEY_BT_SECURE,
       KEY_ALARM_ON, KEY_ALARM_TIME = KEY_ALARM_ON + ALARM_SLOTS, KEY_ALARM_REPEAT = KEY_ALARM_TIME + ALARM_SLOTS,
       KEY_ALARM_STOP = KEY_ALARM_REPEAT + ALARM_SLOTS, KEY_ALARM_SNOOZE, KEY_ALARM_RINGING, KEY_ALARM_EVENT, KEY_ALARM_NEXT,
       KEY_MEM_USED, KEY_MEM_AVAIL, KEY_LOAD, KEY_SELF_CPU, KEY_SELF_RSS, KEY_TOP, KEY_THREADS, KEY_LAST_KILL, KEY_KILL_SERVICE, KEY_AIRPLAY, KEY_IMPROV, KEY_AP_TITLE, KEY_AP_ARTIST, KEY_AP_ALBUM };
enum { MP_KEY = 1, MP_IDLE = 1, MP_PLAYING = 2, MP_CMD_PLAY = 0, MP_CMD_PAUSE = 1, MP_CMD_STOP = 2, MP_CMD_MUTE = 3, MP_CMD_UNMUTE = 4 };
#define MEDIA_RATE 48000        /* what we ask Home Assistant to transcode announcements and media to: WAV mono s16 */

/* Several API clients at a time, like ESPHome firmware: Home Assistant plus e.g. a debugging client.  Replies go to the
 * client that asked, state changes to every client that subscribed to states, voice assistant traffic to the one
 * client that subscribed to the voice assistant (first come, first served - also like the firmware). */
#define MAX_CLIENTS 4
/* A connection has this long for its first message (after the Noise handshake, if any).  Until then it holds a slot
 * without having said anything, and is the first to go when a new one finds them all taken: four idle connections
 * used to keep Home Assistant out until they were closed. */
#define FIRST_MSG_S 10
/* What waits for a client that does not take it (outq.h): mic audio is left out past OUTQ_BEHIND, and past OUTQ_LIMIT
 * the client is let go.  The most a client is sent at once is the entity list, about 15 KB; mic audio is 33 KB a second. */
#define OUTQ_LIMIT  (512 * 1024)
#define OUTQ_BEHIND (64 * 1024)
/* lock held.  enc: Noise frames (tx is used under the lock, so the nonces go in the order the frames are queued; rx is the reader's own,
 * in serve()); keyed: with the device key; actions: runs Home Assistant actions for us; talked: a message came through; since: when it
 * connected; q: its outgoing queue, owned by its serve() */
static struct { int fd, states, enc, keyed, ble, ble_free, ble_user, actions, ha_states, talked; long long since; struct noise_cs tx, rx; struct outq *q; } clients[MAX_CLIENTS] =
    { { .fd = -1 }, { .fd = -1 }, { .fd = -1 }, { .fd = -1 } };
static int va_fd = -1;                  /* lock held: voice assistant subscriber */
static __thread int reply_fd = -1;      /* the client whose request this thread is handling */
static int cancelled;                   /* lock held: the user cancelled the run; its events are dropped.  2: a new run
                                         * was asked for, whose RUN_START ends that (TCP keeps the order) */
static int tts_expected;                /* lock held: the reply is being fetched or played; RUN_END must not end the pipeline */
static char tts_url[1024];              /* lock held: reply URL of the running pipeline (known from RUN_START with streaming TTS) */
static int announcing, media_playing;   /* lock held */
static void announce_done(void);
static atomic_int media_busy;
static struct media_job *media_cur;    /* lock held: the job holding media_busy, for cancel() */

/* ---------------------------------------------------------------- protobuf */

struct pb { unsigned char *p; size_t n, cap; };

static void pb_raw(struct pb *b, const void *d, size_t n) { if (b->n + n <= b->cap) { memcpy(b->p + b->n, d, n); b->n += n; } }
static void pb_varint(struct pb *b, uint64_t v) { unsigned char c; do { c = v & 0x7f; v >>= 7; if (v) c |= 0x80; pb_raw(b, &c, 1); } while (v); }
static void pb_uint(struct pb *b, int f, uint64_t v) { if (v) { pb_varint(b, f << 3); pb_varint(b, v); } }
static void pb_bytes(struct pb *b, int f, const void *d, size_t n) { if (n) { pb_varint(b, f << 3 | 2); pb_varint(b, n); pb_raw(b, d, n); } }
static void pb_str(struct pb *b, int f, const char *s) { pb_bytes(b, f, s, strlen(s)); }
static void pb_sint(struct pb *b, int f, int32_t v) { pb_uint(b, f, (uint32_t)v << 1 ^ (uint32_t)(v >> 31)); }   /* zigzag */
static void pb_fixed32(struct pb *b, int f, uint32_t v) { pb_varint(b, f << 3 | 5); pb_raw(b, &v, 4); }      /* little endian host */
static void pb_float(struct pb *b, int f, float v) { uint32_t u; memcpy(&u, &v, 4); pb_fixed32(b, f, u); }

struct pbf { unsigned field, wire; uint64_t v; const unsigned char *data; size_t len; };

/* At most 10 bytes, the last one ending it: a longer or cut-off varint is a broken message, not a value */
static int pb_get_varint(const unsigned char **p, const unsigned char *end, uint64_t *v)
{
    *v = 0;
    for (int sh = 0; sh < 70 && *p < end; sh += 7) {
        unsigned char c = *(*p)++;
        *v |= (uint64_t)(c & 0x7f) << sh;
        if (!(c & 0x80)) return 1;
    }
    return 0;
}

static int pb_next(const unsigned char **p, const unsigned char *end, struct pbf *f)
{
    uint64_t k, v;
    if (*p >= end || !pb_get_varint(p, end, &k) || k >> 32 || !(k >> 3)) return 0;   /* field numbers are 1 .. 2^29-1 */
    f->field = k >> 3; f->wire = k & 7; f->v = 0; f->data = NULL; f->len = 0;
    switch (f->wire) {
    case 0: if (!pb_get_varint(p, end, &v)) return 0; f->v = v; break;
    case 1: if (end - *p < 8) return 0; *p += 8; break;
    case 2:
        if (!pb_get_varint(p, end, &v) || (uint64_t)(end - *p) < v) return 0;
        f->data = *p; f->len = v; *p += v;
        break;
    case 5: if (end - *p < 4) return 0; { uint32_t u; memcpy(&u, *p, 4); f->v = u; } *p += 4; break;
    default: return 0;
    }
    return 1;
}

static void pbf_str(const struct pbf *f, char *out, size_t outsz)
{
    size_t n = f->len < outsz - 1 ? f->len : outsz - 1;
    memcpy(out, f->data, n); out[n] = 0;
}

static int client_of(int fd)            /* lock held */
{
    for (int i = 0; i < MAX_CLIENTS; i++) if (fd >= 0 && clients[i].fd == fd) return i;
    return -1;
}

/* Noise frame: 0x01, big-endian length, then a handshake message or ciphertext.  Header and body in one piece: the
 * sockets are TCP_NODELAY, so written apart they went out as two segments (every mic chunk) */
static int put_frame(struct outq *q, const void *data, size_t len)
{
    unsigned char h[3] = { 1, len >> 8, len & 0xff };
    struct iovec iov[2] = { { h, 3 }, { (void *)data, len } };
    return len > 0xffff ? -1 : outq_put(q, iov, 2);
}

/* lock held.  Only queued: the client's writer thread sends it (outq.h), so a client whose link stalls holds up nobody.
 * droppable: mic audio, left out while the client is behind; decided before the frame is encrypted, which spends a nonce */
static void send_out(int fd, unsigned type, const struct pb *b, int droppable)
{
    unsigned char h[12]; struct pb hb = { h, 0, sizeof h }; size_t n = b ? b->n : 0; int c = client_of(fd), rc;
    if (c < 0 || !clients[c].q) return;                 /* pushed out for a newer connection: its reader is ending */
    struct outq *q = clients[c].q;
    if (droppable && outq_behind(q, 3 + 4 + n + NOISE_TAG)) return;
    if (clients[c].enc) {                               /* inside: big-endian type and length, then the message */
        static unsigned char pt[4 + 8192], ct[sizeof pt + NOISE_TAG];
        if (4 + n > sizeof pt) { fprintf(stderr, "esphome: message %u too long (%zu)\n", type, n); return; }
        pt[0] = type >> 8; pt[1] = type; pt[2] = n >> 8; pt[3] = n;
        if (n) memcpy(pt + 4, b->p, n);
        rc = put_frame(q, ct, noise_encrypt(&clients[c].tx, pt, 4 + n, ct));
    } else {
        pb_raw(&hb, "", 1); pb_varint(&hb, n); pb_varint(&hb, type);
        struct iovec iov[2] = { { h, hb.n }, { n ? b->p : NULL, n } };
        rc = outq_put(q, iov, 2);
    }
    if (rc < 0) shutdown(fd, SHUT_RDWR);                /* its reader cleans up */
}
static void send_to(int fd, unsigned type, const struct pb *b) { send_out(fd, type, b, 0); }

static void send_msg(unsigned type, const struct pb *b) { send_to(reply_fd, type, b); }          /* answer to a request */
static void send_va(unsigned type, const struct pb *b) { send_to(va_fd, type, b); }              /* voice assistant traffic */
static void send_state(unsigned type, const struct pb *b)                                        /* entity state: all subscribers */
{
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && clients[i].states) send_to(clients[i].fd, type, b);
}

#define PB(name, size) unsigned char name##_buf[size]; struct pb name = { name##_buf, 0, size }

/* ---------------------------------------------------------------- identity */

static const char *node_name(void) { return core_node_name(); }     /* ESPHome device names are host names */

static const char *mac(void)
{
    static char m[24] = "02:00:00:00:00:01";
    FILE *f = fopen("/sys/class/net/wlan0/address", "r");
    if (f) { if (fscanf(f, "%17s", m) == 1) for (char *c = m; *c; c++) *c = toupper((unsigned char)*c); fclose(f); }
    return m;
}

static const char *mac_plain(void)      /* "aabbccddeeff": mDNS and the Noise server hello */
{
    static char m[24]; size_t j = 0;
    for (const char *c = mac(); *c && j < sizeof m - 1; c++) if (*c != ':') m[j++] = tolower((unsigned char)*c);
    m[j] = 0;
    return m;
}

/* ---------------------------------------------------------------- encryption key (see top) */

#define NOISE_NAME "Noise_NNpsk0_25519_ChaChaPoly_SHA256"
static uint8_t api_key[32]; static int have_key;        /* lock held */

static const char *key_path(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/api_key", d ? d : "/data/local/hassmic/state");
    return p;
}

static void key_load(void)
{
    char t[128] = ""; FILE *f = fopen(key_path(), "r");
    have_key = 0;
    if (!f) return;
    if (fgets(t, sizeof t, f) && b64_decode(t, strcspn(t, "\r\n"), api_key, sizeof api_key) == 32) have_key = 1;
    else fprintf(stderr, "encryption: %s is not a key, ignored\n", key_path());
    fclose(f);
}

static void print_mdns_to(FILE *f)
{
    char ss[512] = "";
    if (core_sendspin_port) sendspin_mdns(core_sendspin_port, ss, sizeof ss);
    fprintf(f, "<?xml version=\"1.0\" standalone='no'?>\n<!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">\n"
            "<service-group>\n  <name>%s</name>\n  <service><type>_esphomelib._tcp</type><port>%d</port>\n"
            "    <txt-record>mac=%s</txt-record><txt-record>friendly_name=%s</txt-record><txt-record>version=2025.5.0</txt-record>\n"
            "    <txt-record>platform=hassmic</txt-record><txt-record>network=wifi</txt-record>\n"
            /* with a key Home Assistant asks for it when the device is added; without, it adds the device and sets one */
            "    <txt-record>%s=" NOISE_NAME "</txt-record>\n  </service>\n%s</service-group>\n",
            node_name(), core_port, mac_plain(), core_name, have_key ? "api_encryption" : "api_encryption_supported", ss);
}

static void print_mdns(void) { key_load(); print_mdns_to(stdout); }

/* The service file main.sh wrote at boot names the encryption state: rewrite it when that changes (avahi notices). */
static void mdns_refresh(void)
{
    const char *path = getenv("HASSMIC_MDNS_FILE"); struct statefile s;
    if (!path) path = "/data/misc/avahi/services/hassmic.service";
    if (access(path, F_OK)) return;
    if (statefile_open(&s, path, 0644)) { fprintf(stderr, "encryption: cannot update %s\n", path); return; }
    fchmod(fileno(s.f), 0644);                      /* avahi reads it: past hassmic's umask */
    print_mdns_to(s.f);
    if (statefile_commit(&s)) fprintf(stderr, "encryption: cannot update %s\n", path);
}

/* lock held.  k: base64 as Home Assistant sends it; empty clears.  Returns 1 on success. */
static int key_set(const unsigned char *k, size_t len)
{
    uint8_t raw[32]; struct statefile s;
    if (!len) {
        if (unlink(key_path()) && errno != ENOENT) { fprintf(stderr, "encryption: cannot remove %s\n", key_path()); return 0; }
        memset(api_key, 0, sizeof api_key); have_key = 0; fprintf(stderr, "encryption: key cleared, plaintext connections accepted again\n");
        mdns_refresh(); return 1;
    }
    char t[64]; if (len >= sizeof t) return 0;
    memcpy(t, k, len); t[len] = 0;
    if (b64_decode(t, len, raw, sizeof raw) != 32) { fprintf(stderr, "encryption: rejected a key that is not 32 bytes\n"); return 0; }
    if (statefile_open(&s, key_path(), 0600)) { fprintf(stderr, "encryption: cannot write %s.tmp\n", key_path()); return 0; }
    fprintf(s.f, "%s\n", t);
    if (statefile_commit(&s)) { fprintf(stderr, "encryption: cannot write %s\n", key_path()); return 0; }
    memcpy(api_key, raw, 32); have_key = 1;
    fprintf(stderr, "encryption: key set, only encrypted connections with it from now on\n");
    mdns_refresh();
    return 1;
}

/* ---------------------------------------------------------------- settings entities
 * Kept in a small file so they survive restarts.  "Mic level" is the speech level the Echo's own gain aims at
 * (micgain.h).  It replaced noise suppression, auto gain and mic volume multiplier (2026-09-29): Home Assistant ignores
 * the audio settings of an ESPHome VoiceAssistantRequest, so they never did anything.  "Noise reduction" puts RNNoise
 * ahead of that gain (micdenoise.h), taking the noise down by up to 6, 9 or 12 dB; it sits in the settings file's first
 * field, which held the old noise suppression level. */

static int have_light;                  /* a light sensor answered at start: auto brightness is listed, illuminance with the feature */

static int listed(int key);
static void send_setting(int key)       /* lock held */
{
    if (!listed(key)) return;
    PB(b, 48);
    pb_fixed32(&b, 1, key);
    switch (key) {
    case KEY_MIC_LEVEL: pb_float(&b, 2, settings_mic_level()); send_state(NUMBER_STATE, &b); break;
    case KEY_MUTE:  pb_uint(&b, 2, core_muted()); send_state(SWITCH_STATE, &b); break;
    case KEY_WAKE_SOUND: pb_uint(&b, 2, core_wake_sound(-1)); send_state(SWITCH_STATE, &b); break;
    case KEY_BT_PAIRING: if (ble_present()) { pb_uint(&b, 2, a2dp_pairing()); send_state(SWITCH_STATE, &b); } break;
    case KEY_BT_ANNOUNCE: if (ble_present()) { pb_uint(&b, 2, core_bt_announce(-1)); send_state(SWITCH_STATE, &b); } break;
    case KEY_BT_AAC: if (ble_present()) { pb_uint(&b, 2, a2dp_aac(-1)); send_state(SWITCH_STATE, &b); } break;
    case KEY_BT_SECURE: if (ble_present()) { pb_uint(&b, 2, ble_secure_only(-1)); send_state(SWITCH_STATE, &b); } break;
    case KEY_DND:   pb_uint(&b, 2, core_dnd(-1)); send_state(SWITCH_STATE, &b); break;
    case KEY_DENOISE: pb_str(&b, 2, denoise_names[core_mic_denoise(-1)]); send_state(SELECT_STATE, &b); break;
    case KEY_BT_LANG: if (ble_present()) { pb_str(&b, 2, settings_bt_lang()->name); send_state(SELECT_STATE, &b); } break;
    case KEY_EQ_BASS: case KEY_EQ_MID: case KEY_EQ_TREBLE: pb_float(&b, 2, core_eq(key - KEY_EQ_BASS)); send_state(NUMBER_STATE, &b); break;
    case KEY_ARB_JOIN: if (arb_running()) { pb_uint(&b, 2, arb_arbitrate(-1)); send_state(SWITCH_STATE, &b); } break;
    case KEY_ARB_PEERS: if (arb_running()) { pb_float(&b, 2, arb_peers()); send_state(SENSOR_STATE, &b); } break;
    case KEY_SS_UNPAIRED: if (core_sendspin_port) { pb_uint(&b, 2, sendspin_unpaired(-1)); send_state(SWITCH_STATE, &b); } break;
    case KEY_ADB_WIFI: pb_uint(&b, 2, adbwifi_open()); send_state(SWITCH_STATE, &b); break;
    case KEY_SOUND_DETECTION: pb_uint(&b, 2, core_sound(-1)); send_state(SWITCH_STATE, &b); break;
    case KEY_IMPROV: if (ble_present()) { pb_uint(&b, 2, improv_enable(-1)); send_state(SWITCH_STATE, &b); } break;
    case KEY_AIRPLAY: if (airplay_present()) { pb_uint(&b, 2, airplay_enable(-1)); send_state(SWITCH_STATE, &b); } break;
    case KEY_WIFI_MOTION_ON: if (wifimotion_present()) { pb_uint(&b, 2, wifimotion_enable(-1)); send_state(SWITCH_STATE, &b); } break;
    case KEY_WIFI_MOTION_SENS: if (wifimotion_present()) { pb_float(&b, 2, wifimotion_sensitivity(-1)); send_state(NUMBER_STATE, &b); } break;
        break;
    case KEY_WIFI_MOTION:               /* unknown while switched off */
        if (wifimotion_present()) { int m = wifimotion_motion(); pb_uint(&b, 2, m > 0); pb_uint(&b, 3, m < 0); send_state(BINARY_SENSOR_STATE, &b); }
        break;
    case KEY_TIMER_RINGING: pb_uint(&b, 2, core_timers_ringing(NULL, 0) > 0); send_state(BINARY_SENSOR_STATE, &b); break;
    case KEY_TIMER_NAMES: { PB(t, 320); char names[288]; core_timers_ringing(names, sizeof names);
        pb_fixed32(&t, 1, key); pb_str(&t, 2, names); send_state(TEXT_SENSOR_STATE, &t); } break;
    case KEY_DROPIN_STATE: case KEY_DROPIN_PEER: { PB(t, 128); char peer[64]; int st = dropin_status(peer, sizeof peer);
        pb_fixed32(&t, 1, key); pb_str(&t, 2, key == KEY_DROPIN_STATE ? dropin_states[st] : peer); send_state(TEXT_SENSOR_STATE, &t); } break;
    case KEY_LIGHT: {
        struct ring_light l; ring_light_get(&l);
        pb_uint(&b, 2, l.on); pb_float(&b, 3, l.brightness); pb_uint(&b, 11, COLOR_MODE_RGB); pb_float(&b, 10, l.color_brightness);
        pb_float(&b, 4, l.r); pb_float(&b, 5, l.g); pb_float(&b, 6, l.b); pb_str(&b, 9, ring_effects[l.effect]);
        send_state(LIGHT_STATE, &b);
    } break;
    case KEY_RING_FILL: { struct ring_light l; ring_light_get(&l); pb_float(&b, 2, l.fill); send_state(NUMBER_STATE, &b); } break;
    case KEY_RING_ROTATION: pb_float(&b, 2, ring_rotation(-1)); send_state(NUMBER_STATE, &b); break;
    case KEY_LED_AUTO: if (have_light) { pb_uint(&b, 2, core_led_auto(-1)); send_state(SWITCH_STATE, &b); } break;
    case KEY_ASSIST_VOLUME: case KEY_SOUNDS_VOLUME: case KEY_TIMER_VOLUME: case KEY_CALL_VOLUME:     /* VOL_* order */
        pb_float(&b, 2, core_kind_volume(key - KEY_ASSIST_VOLUME, -1)); send_state(NUMBER_STATE, &b); break;
    case KEY_LED_BRIGHTNESS: { int v = core_led_brightness(-1); if (v >= 0) { pb_float(&b, 2, v); send_state(NUMBER_STATE, &b); } } break;
    case KEY_BT_OUT_SEARCH: if (ble_present()) { pb_uint(&b, 2, a2dp_out_searching()); send_state(SWITCH_STATE, &b); } break;
    case KEY_BT_OUT: if (ble_present()) { pb_uint(&b, 2, a2dp_out_enabled()); send_state(SWITCH_STATE, &b); } break;
    case KEY_BT_OUT_DELAY: if (ble_present()) { pb_float(&b, 2, a2dp_out_delay(-1)); send_state(NUMBER_STATE, &b); } break;
    case KEY_BT_OUT_STATUS:
        if (ble_present()) { PB(t, 160); char st[120]; a2dp_out_status(st, sizeof st); pb_fixed32(&t, 1, key); pb_str(&t, 2, st); send_state(TEXT_SENSOR_STATE, &t); }
        break;
    case KEY_SOC_TEMP: case KEY_CPU_USAGE: break;                 /* pushed by diag_thread every 30 s */
    case KEY_ALARM_RINGING: pb_uint(&b, 2, alarms_ringing() >= 0); send_state(BINARY_SENSOR_STATE, &b); break;
    case KEY_ALARM_NEXT: {                      /* unknown: no alarm on, or no clock to ring by */
        PB(t, 96); char at[40]; long long n = alarms_next();
        pb_fixed32(&t, 1, key);
        if (n >= 0) { iso_utc(n, at, sizeof at); pb_str(&t, 2, at); } else pb_uint(&t, 3, 1);
        send_state(TEXT_SENSOR_STATE, &t);
    } break;
    case KEY_FIRMWARE: pb_str(&b, 2, RUNNING_VERSION); send_state(TEXT_SENSOR_STATE, &b); break;
    }
}

/* The Sendspin pairing token, so that it can be copied from Home Assistant into Music Assistant.  It is a secret of
 * sorts (whoever has it can pair a server with this player), so the entity is diagnostic and disabled by default:
 * Home Assistant only records it once the user enables it. */
static void send_token_state(void)      /* lock held */
{
    PB(b, 256); char tok[160];
    if (!core_sendspin_port) return;
    sendspin_pairing_token(tok, sizeof tok);
    pb_fixed32(&b, 1, KEY_SENDSPIN_TOKEN); pb_str(&b, 2, tok);
    send_state(TEXT_SENSOR_STATE, &b);
}

/* The settings page as this client reached us (http://<address>:28931): diagnostic, enabled, so the address is on the
 * device page without a click.  The address is the one the subscriber connected to, sent to it only: a debugging
 * client over adb forward would otherwise show Home Assistant 127.0.0.1.  A new address drops every link over the old
 * one (netaddr.c), so the state at subscribe stays true. */
static void send_web_state(void)        /* lock held */
{
    PB(b, 64); struct sockaddr_in a; socklen_t n = sizeof a; char s[INET_ADDRSTRLEN], url[48];
    if (!core_web_port) return;
    if (getsockname(reply_fd, (struct sockaddr *)&a, &n) || a.sin_family != AF_INET || !inet_ntop(AF_INET, &a.sin_addr, s, sizeof s)) return;
    snprintf(url, sizeof url, "http://%s:%d", s, core_web_port);
    pb_fixed32(&b, 1, KEY_WEB_URL); pb_str(&b, 2, url);
    send_msg(TEXT_SENSOR_STATE, &b);
}

static void send_sensor(int key, float v)   /* lock held */
{
    PB(b, 32);
    if (isnan(v)) return;
    pb_fixed32(&b, 1, key); pb_float(&b, 2, v);
    send_state(SENSOR_STATE, &b);
}

/* SoC temperature and CPU usage (diag.c): pushed every 30 s to whoever subscribed to states */
/* Task manager (taskmgr.h): sampled by diag_thread outside the lock, published here */
#define DIAG_TICK_S 10          /* the task manager; temperature and CPU usage every third tick, the time every 360th */
static struct taskmgr_stats tm;         /* lock held: the last sample */
static int tm_have;                     /* lock held */

static void send_text(int key, const char *v)      /* lock held.  "": unknown */
{
    PB(b, TASKMGR_TEXT + 16);
    pb_fixed32(&b, 1, key);
    if (*v) pb_str(&b, 2, v); else pb_uint(&b, 3, 1);
    send_state(TEXT_SENSOR_STATE, &b);
}

static void send_last_kill(void) { char k[TASKMGR_TEXT]; taskmgr_last_kill(k, sizeof k); send_text(KEY_LAST_KILL, k); }   /* lock held */

static void send_task_states(void)      /* lock held */
{
    send_last_kill();
    if (!tm_have) return;
    send_sensor(KEY_MEM_USED, tm.mem_used_pct); send_sensor(KEY_MEM_AVAIL, tm.mem_avail_mb); send_sensor(KEY_SELF_RSS, tm.self_rss_mb);
    if (tm.load1 >= 0) send_sensor(KEY_LOAD, tm.load1);
    if (!tm.cpu) return;                /* the first sample has nothing to take differences to */
    send_sensor(KEY_SELF_CPU, tm.self_cpu_pct); send_text(KEY_TOP, tm.top); send_text(KEY_THREADS, tm.threads);
}

static void kill_changed(void) { pthread_mutex_lock(&core_lock); send_last_kill(); pthread_mutex_unlock(&core_lock); }

/* lock held: the "kill_process" action.  Root does it (taskmgr.h) and decides what may go; a request at all only from
 * Home Assistant with the key: before it is set anyone on the network gets a connection */
static void on_kill(int keyed, int pid, const char *sig)
{
    char why[160];
    if (!keyed) { fprintf(stderr, "kill: refused, the request did not come over the keyed connection\n"); return; }
    if (taskmgr_kill(pid, sig, why, sizeof why)) fprintf(stderr, "kill: %d refused: %s\n", pid, why);
    send_last_kill();
}


static int any_subscriber(void)         /* lock held */
{
    int any = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) any |= clients[i].fd >= 0 && clients[i].states;
    return any;
}

/* SoC temperature and CPU usage (diag.c) every third tick, the task manager's figures every tick */
static void *diag_thread(void *arg)
{
    static int said;
    thread_name("diag");
    (void)arg;
    for (int n = 1;; n++) {
        struct taskmgr_stats s; int got = 0; float t = NAN, c = NAN;
        sleep(DIAG_TICK_S);
        pthread_mutex_lock(&core_lock); int any = any_subscriber(); pthread_mutex_unlock(&core_lock);
        /* only for someone to see it; outside the lock, it reads all of /proc */
        if (any && !(got = !taskmgr_sample(&s)) && !said++) fprintf(stderr, "task manager: cannot read /proc/stat or /proc/meminfo\n");
        if (got && !said++)             /* what watching costs, once; from then on the diag thread's share in "hassmic threads" */
            fprintf(stderr, "task manager: a sample of %d processes (%d unreadable) and %d threads takes %.1f ms, %.1f ms of it CPU\n",
                    s.nproc, s.unreadable, s.nthreads, s.cost_ms, s.cost_cpu_ms);
        if (any && n % 3 == 0) { t = diag_soc_temp(); c = diag_cpu(); }          /* the CPU figure blocks 250 ms: outside the lock */
        pthread_mutex_lock(&core_lock);
        if (got) { tm = s; tm_have = 1; send_task_states(); }
        if (n % 3 == 0 && any_subscriber()) { send_sensor(KEY_SOC_TEMP, t); send_sensor(KEY_CPU_USAGE, c); }
        pthread_mutex_unlock(&core_lock);
    }
    return NULL;
}

/* ---------------------------------------------------------------- light sensor and LED brightness
 * Illuminance as stock reads it (core_lux) and the level the LED ring shows, which follows it while auto brightness is on
 * (core_led_brightness).  Polled every second, stock's rate once its engine has settled.  The sensor's integer lux flicker
 * by one or two, so a reading goes out when it is 10 % (at least 1 lx) off the last one sent, or after a minute for any
 * change.  A fixed LED level whenever it changes; an auto one at 2 steps off the last one sent, or after 5 minutes for
 * any change: the engine moves the ring once its average is half a step off, so in light near a step's edge (dusk) the
 * sensor's flicker took it 11, 12, 11, 12 every few seconds, all of it in HA's history (2026-10-11).  The ring does
 * the same under stock.  Both only with the "Light sensor" feature on (off by default): even damped, an auto level that
 * follows the room is an entry in HA's logbook every few minutes (forum, 2026-10-15).  Off, the level goes out when HA
 * subscribes and when it is set; auto brightness itself works the same. */
static float sent_lux = NAN; static int sent_level = -1; static time_t sent_at, level_at;     /* lock held */

static void *light_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sleep(1);
        float lux = core_lux(); time_t now = time(NULL);
        pthread_mutex_lock(&core_lock);
        int any = 0, level = core_led_brightness(-1);
        for (int i = 0; i < MAX_CLIENTS; i++) any |= clients[i].fd >= 0 && clients[i].states;
        if (any) {                                              /* a new subscriber gets them from send_light_states */
            float d = fabsf(lux - sent_lux);
            int report = settings_on("light_sensor");
            if (report && !isnan(lux) && (isnan(sent_lux) || d >= fmaxf(1, sent_lux / 10) || (d > 0 && now - sent_at >= 60))) {
                send_sensor(KEY_LUX, lux); sent_lux = lux; sent_at = now;
            }
            if (level >= 0 && level != sent_level
                && (!core_led_auto(-1) || (report && (abs(level - sent_level) >= 2 || now - level_at >= 300)))) {
                send_setting(KEY_LED_BRIGHTNESS); sent_level = level; level_at = now;
            }
        }
        pthread_mutex_unlock(&core_lock);
    }
    return NULL;
}

static void send_light_states(void)     /* lock held */
{
    if (listed(KEY_LUX)) { sent_lux = core_lux(); sent_at = time(NULL); send_sensor(KEY_LUX, sent_lux); }
    sent_level = core_led_brightness(-1); level_at = time(NULL);
    send_setting(KEY_LED_AUTO); send_setting(KEY_LED_BRIGHTNESS); send_setting(KEY_LIGHT);
    send_setting(KEY_RING_FILL); send_setting(KEY_RING_ROTATION);
}

static void send_light_entities(void)
{
    /* Home Assistant's light on the ring (ring.h): a palette made beforehand, so the colour shows as the nearest of
     * it; the state says what was asked for */
    if (listed(KEY_LIGHT)) { PB(b, 256); pb_str(&b, 1, "light_ring"); pb_fixed32(&b, 2, KEY_LIGHT); pb_str(&b, 3, "Light ring");
      pb_uint(&b, 12, COLOR_MODE_RGB); pb_uint(&b, 5, 1); pb_uint(&b, 6, 1);
      for (int i = 0; i < ring_neffects; i++) pb_str(&b, 11, ring_effects[i]);
      pb_str(&b, 14, "mdi:circle-slice-8"); send_msg(LIST_LIGHT, &b); }
    /* Its fill (issue #57): a timer's or a washing machine's progress, a battery.  Kept over the light being off, as its
     * colour is.  The rotation is where the fill starts, in LEDs from where the volume's does: a setting (settings.c) */
    if (listed(KEY_RING_FILL)) { PB(b, 160); pb_str(&b, 1, "light_ring_fill"); pb_fixed32(&b, 2, KEY_RING_FILL); pb_str(&b, 3, "Light ring fill");
      pb_str(&b, 5, "mdi:circle-slice-5"); pb_float(&b, 6, 0); pb_float(&b, 7, 100); pb_float(&b, 8, 1);
      pb_str(&b, 11, "%"); pb_uint(&b, 12, 2); send_msg(LIST_NUMBER, &b); }
    if (listed(KEY_RING_ROTATION)) { PB(b, 160); pb_str(&b, 1, "light_ring_rotation"); pb_fixed32(&b, 2, KEY_RING_ROTATION);
      pb_str(&b, 3, "Light ring rotation"); pb_str(&b, 5, "mdi:rotate-right"); pb_float(&b, 6, 0); pb_float(&b, 7, ring_leds() - 1);
      pb_float(&b, 8, 1); pb_uint(&b, 10, 1); pb_str(&b, 11, "LEDs"); pb_uint(&b, 12, 1); send_msg(LIST_NUMBER, &b); }
    /* LED by LED: esphome.<node>_light_ring_leds with colors ["#ff0000", "", ...] from the fill's start on; the light's
     * own colour or effect from Home Assistant takes the ring back to one colour */
    if (listed(KEY_RING_LEDS)) { PB(b, 256); PB(a, 48); pb_str(&b, 1, "light_ring_leds"); pb_fixed32(&b, 2, KEY_RING_LEDS);
      pb_str(&a, 1, "colors"); pb_uint(&a, 2, 7); pb_bytes(&b, 3, a.p, a.n);
      pb_str(&b, 5, "The light ring LED by LED: \"#rrggbb\" each, \"\" for dark, from where its fill starts"); send_msg(LIST_SERVICE, &b); }
    if (listed(KEY_LUX)) { PB(b, 192); pb_str(&b, 1, "illuminance"); pb_fixed32(&b, 2, KEY_LUX); pb_str(&b, 3, "Illuminance"); pb_str(&b, 6, "lx");
          pb_uint(&b, 7, 0); pb_str(&b, 9, "illuminance"); pb_uint(&b, 10, 1); send_msg(LIST_SENSOR, &b); }
    if (have_light) {
        { PB(b, 160); pb_str(&b, 1, "led_auto_brightness"); pb_fixed32(&b, 2, KEY_LED_AUTO); pb_str(&b, 3, "LED auto brightness");
          pb_str(&b, 5, "mdi:brightness-auto"); pb_uint(&b, 8, 1); send_msg(LIST_SWITCH, &b); }
    }
    { PB(b, 160); pb_str(&b, 1, "led_brightness"); pb_fixed32(&b, 2, KEY_LED_BRIGHTNESS); pb_str(&b, 3, "LED brightness");
      pb_str(&b, 5, "mdi:brightness-6"); pb_float(&b, 6, 0); pb_float(&b, 7, 100); pb_float(&b, 8, 1); pb_uint(&b, 10, 1);
      pb_str(&b, 11, "%"); pb_uint(&b, 12, 2); send_msg(LIST_NUMBER, &b); }
    /* separate volumes (main.c): the three beside the media player's, which is the music's; no entity category, controls
     * like the media player's volume */
    static const char *const vol_ids[VOL_KINDS] = { "assistant_volume", "sounds_volume", "timer_volume", "call_volume" };
    static const char *const vol_names[VOL_KINDS] = { "Assistant volume", "Sounds volume", "Timer volume", "Call volume" };
    static const char *const vol_icons[VOL_KINDS] = { "mdi:account-voice", "mdi:bell-ring-outline", "mdi:timer-outline", "mdi:phone-in-talk" };
    for (int i = 0; i < VOL_KINDS; i++) if (listed(KEY_ASSIST_VOLUME + i)) {
        PB(b, 160); pb_str(&b, 1, vol_ids[i]); pb_fixed32(&b, 2, KEY_ASSIST_VOLUME + i); pb_str(&b, 3, vol_names[i]);
        pb_str(&b, 5, vol_icons[i]); pb_float(&b, 6, 0); pb_float(&b, 7, 100); pb_float(&b, 8, 1);
        pb_str(&b, 11, "%"); pb_uint(&b, 12, 2); send_msg(LIST_NUMBER, &b);
    }
}

/* What Home Assistant lists.  Always: media player, identify, factory reset, mute, do not disturb, wake sound, LEDs, buttons, equalizer, firmware, the
 * Sendspin token (a secret: never on the plain-HTTP settings page).  A feature's entities only while it is on
 * (settings.c, switched on the settings page; switching one has Home Assistant read the list again).  Everything else
 * is on the settings page only.  States go out only for what is listed. */
static int listed(int key)
{
    switch (key) {
    case KEY_BT_ANNOUNCE: case KEY_BT_LANG: case KEY_BT_AAC: case KEY_BT_SECURE: return ble_present();
    case KEY_ARB_JOIN: return arb_running();                      /* the switch that joins the rounds: always, the peers follow when on */
    case KEY_WIFI_MOTION_ON: return wifimotion_present();
    case KEY_SENDSPIN_TOKEN: case KEY_SS_UNPAIRED: return core_sendspin_port != 0;
    case KEY_SOUND: return settings_on("sound_detection");
    case KEY_AIRPLAY: return airplay_present();
    case KEY_AP_TITLE: case KEY_AP_ARTIST: case KEY_AP_ALBUM: return airplay_present() && airplay_enable(-1);
    case KEY_IMPROV: return ble_present();
    case KEY_WAKE: return settings_on("wake_events");
    case KEY_BT_PAIRING: return ble_present() && settings_on("bluetooth_audio");
    case KEY_BT_OUT_SEARCH: case KEY_BT_OUT: case KEY_BT_OUT_STATUS: case KEY_BT_OUT_DELAY: return ble_present() && settings_on("bluetooth_speaker");
    case KEY_ARB_PEERS: return arb_running() && settings_on("arbitration");
    case KEY_ARB_SERVICE: return arb_running();     /* the network, which runs either way */
    case KEY_WIFI_MOTION: case KEY_WIFI_MOTION_SENS: return wifimotion_present() && settings_on("wifi_motion");
    case KEY_LED_AUTO: return have_light;
    case KEY_LUX: return have_light && settings_on("light_sensor");
    case KEY_LIGHT: return ring_custom();
    case KEY_RING_FILL: case KEY_RING_ROTATION: case KEY_RING_LEDS: return ring_per_led();
    case KEY_CALL_VOLUME: if (!(dropin_running() && settings_on("drop_in"))) return 0;    /* calls only with Drop In */
        /* fall through */
    case KEY_ASSIST_VOLUME: case KEY_SOUNDS_VOLUME: case KEY_TIMER_VOLUME:       /* one that follows the music has none */
        return settings_on("separate_volumes") && !core_kind_linked(key - KEY_ASSIST_VOLUME, -1);
    case KEY_DROPIN_SERVICE: case KEY_DROPIN_STATE: case KEY_DROPIN_PEER: case KEY_DROPIN_END: case KEY_DROPIN_RESULT:
        return dropin_running() && settings_on("drop_in");
    default: return 1;
    }
}

/* ---------------------------------------------------------------- alarm clock (alarms.h) */

static void send_alarm_states(void)     /* lock held */
{
    for (int i = 0; i < ALARM_SLOTS; i++) {
        struct alarm_def a; int r = -1;
        alarms_get(i, &a);
        { PB(b, 16); pb_fixed32(&b, 1, KEY_ALARM_ON + i); pb_uint(&b, 2, a.on); send_state(SWITCH_STATE, &b); }
        { PB(b, 24); pb_fixed32(&b, 1, KEY_ALARM_TIME + i); pb_uint(&b, 3, a.hour); pb_uint(&b, 4, a.minute); pb_uint(&b, 5, a.second);
          send_state(TIME_STATE, &b); }
        for (int k = 0; k < alarm_nrepeat; k++) if (alarm_repeat_days[k] == a.days) r = k;
        { PB(b, 32); pb_fixed32(&b, 1, KEY_ALARM_REPEAT + i);      /* days the select has no option for (a hand-edited file): unknown */
          if (r >= 0) pb_str(&b, 2, alarm_repeat_names[r]); else pb_uint(&b, 3, 1);
          send_state(SELECT_STATE, &b); }
    }
    send_setting(KEY_ALARM_RINGING); send_setting(KEY_ALARM_NEXT);
}

static void send_alarm_entities(void)
{
    for (int i = 0; i < ALARM_SLOTS; i++) {
        char id[32], name[32];
        { PB(b, 128); snprintf(id, sizeof id, "alarm_%d", i + 1); snprintf(name, sizeof name, "Alarm %d", i + 1);
          pb_str(&b, 1, id); pb_fixed32(&b, 2, KEY_ALARM_ON + i); pb_str(&b, 3, name); pb_str(&b, 5, "mdi:alarm"); send_msg(LIST_SWITCH, &b); }
        { PB(b, 128); snprintf(id, sizeof id, "alarm_%d_time", i + 1); snprintf(name, sizeof name, "Alarm %d time", i + 1);
          pb_str(&b, 1, id); pb_fixed32(&b, 2, KEY_ALARM_TIME + i); pb_str(&b, 3, name); pb_str(&b, 5, "mdi:clock-outline"); send_msg(LIST_TIME, &b); }
        { PB(b, 384); snprintf(id, sizeof id, "alarm_%d_repeat", i + 1); snprintf(name, sizeof name, "Alarm %d repeat", i + 1);
          pb_str(&b, 1, id); pb_fixed32(&b, 2, KEY_ALARM_REPEAT + i); pb_str(&b, 3, name); pb_str(&b, 5, "mdi:calendar-sync");
          for (int k = 0; k < alarm_nrepeat; k++) pb_str(&b, 6, alarm_repeat_names[k]);
          send_msg(LIST_SELECT, &b); }
    }
    { PB(b, 128); pb_str(&b, 1, "stop_alarm"); pb_fixed32(&b, 2, KEY_ALARM_STOP); pb_str(&b, 3, "Stop alarm"); pb_str(&b, 5, "mdi:alarm-off");
      send_msg(LIST_BUTTON, &b); }
    { PB(b, 128); pb_str(&b, 1, "snooze_alarm"); pb_fixed32(&b, 2, KEY_ALARM_SNOOZE); pb_str(&b, 3, "Snooze alarm"); pb_str(&b, 5, "mdi:alarm-snooze");
      send_msg(LIST_BUTTON, &b); }
    { PB(b, 128); pb_str(&b, 1, "alarm_ringing"); pb_fixed32(&b, 2, KEY_ALARM_RINGING); pb_str(&b, 3, "Alarm ringing"); pb_str(&b, 8, "mdi:alarm-bell");
      send_msg(LIST_BINARY_SENSOR, &b); }
    /* which alarm started ringing, for automations ("When Alarm fires with alarm_1"); missed while nobody is connected */
    { PB(b, 192); pb_str(&b, 1, "alarm"); pb_fixed32(&b, 2, KEY_ALARM_EVENT); pb_str(&b, 3, "Alarm"); pb_str(&b, 5, "mdi:alarm");
      for (int i = 0; i < ALARM_SLOTS; i++) { char t[24]; snprintf(t, sizeof t, "alarm_%d", i + 1); pb_str(&b, 9, t); }
      send_msg(LIST_EVENT, &b); }
    { PB(b, 128); pb_str(&b, 1, "next_alarm"); pb_fixed32(&b, 2, KEY_ALARM_NEXT); pb_str(&b, 3, "Next alarm"); pb_str(&b, 5, "mdi:alarm-check");
      pb_str(&b, 8, "timestamp"); send_msg(LIST_TEXT_SENSOR, &b); }
}

/* lock held: the switch, time and repeat commands, and the buttons.  0: not the alarm clock's */
static int on_alarm_command(unsigned type, unsigned key, int on, const char *opt, const unsigned char *p, const unsigned char *end)
{
    struct alarm_def a; struct pbf f; int i;
    if (type == BUTTON_COMMAND && key == KEY_ALARM_STOP) alarms_stop();
    else if (type == BUTTON_COMMAND && key == KEY_ALARM_SNOOZE) { if (!alarms_snooze()) fprintf(stderr, "alarm clock: snooze, but no alarm rings\n"); }
    else if (type == SWITCH_COMMAND && key >= KEY_ALARM_ON && key < KEY_ALARM_ON + ALARM_SLOTS) {
        alarms_get(i = key - KEY_ALARM_ON, &a); a.on = on; alarms_set(i, &a);
    } else if (type == SELECT_COMMAND && key >= KEY_ALARM_REPEAT && key < KEY_ALARM_REPEAT + ALARM_SLOTS) {
        alarms_get(i = key - KEY_ALARM_REPEAT, &a);
        for (int k = 0; k < alarm_nrepeat; k++) if (!strcmp(opt, alarm_repeat_names[k])) { a.days = alarm_repeat_days[k]; alarms_set(i, &a); }
    } else if (type == TIME_COMMAND && key >= KEY_ALARM_TIME && key < KEY_ALARM_TIME + ALARM_SLOTS) {
        alarms_get(i = key - KEY_ALARM_TIME, &a);
        a.hour = a.minute = a.second = 0;                   /* zero values are left out of the message */
        while (pb_next(&p, end, &f)) {
            if (f.field == 2) a.hour = f.v < 24 ? (int)f.v : 23;
            else if (f.field == 3) a.minute = f.v < 60 ? (int)f.v : 59;
            else if (f.field == 4) a.second = f.v < 60 ? (int)f.v : 59;
        }
        alarms_set(i, &a);
    } else return 0;
    send_alarm_states(); send_task_states();
    return 1;
}

static void task_sensor(const char *id, int key, const char *name, const char *icon, const char *unit, int decimals, int disabled)
{
    PB(b, 192);
    pb_str(&b, 1, id); pb_fixed32(&b, 2, key); pb_str(&b, 3, name); pb_str(&b, 5, icon); pb_str(&b, 6, unit);
    pb_uint(&b, 7, decimals); pb_uint(&b, 10, 1); pb_uint(&b, 12, disabled); pb_uint(&b, 13, 2);
    send_msg(LIST_SENSOR, &b);
}

static void task_text(const char *id, int key, const char *name, const char *icon, int disabled)
{
    PB(b, 160);
    pb_str(&b, 1, id); pb_fixed32(&b, 2, key); pb_str(&b, 3, name); pb_str(&b, 5, icon); pb_uint(&b, 6, disabled); pb_uint(&b, 7, 2);
    send_msg(LIST_TEXT_SENSOR, &b);
}

static void send_task_entities(void)
{
    task_sensor("memory_used", KEY_MEM_USED, "Memory used", "mdi:memory", "%", 1, 0);
    task_sensor("memory_available", KEY_MEM_AVAIL, "Memory available", "mdi:memory", "MB", 0, 1);
    task_sensor("load_average", KEY_LOAD, "Load average", "mdi:gauge", "", 2, 1);
    task_sensor("hassmic_cpu", KEY_SELF_CPU, "hassmic CPU", "mdi:chip", "%", 1, 1);
    task_sensor("hassmic_memory", KEY_SELF_RSS, "hassmic memory", "mdi:memory", "MB", 1, 1);
    task_text("top_processes", KEY_TOP, "Top processes", "mdi:format-list-numbered", 1);
    task_text("hassmic_threads", KEY_THREADS, "hassmic threads", "mdi:format-list-bulleted", 1);
    task_text("last_kill", KEY_LAST_KILL, "Last kill", "mdi:close-octagon-outline", 0);
    /* Home Assistant names it esphome.<node>_kill_process and makes every argument required: "signal" may be empty */
    { PB(b, 256); PB(a, 48); pb_str(&b, 1, "kill_process"); pb_fixed32(&b, 2, KEY_KILL_SERVICE);
      pb_str(&a, 1, "pid"); pb_uint(&a, 2, 1); pb_bytes(&b, 3, a.p, a.n);
      a.n = 0; pb_str(&a, 1, "signal"); pb_uint(&a, 2, 3); pb_bytes(&b, 3, a.p, a.n);
      pb_str(&b, 5, "Ends a process: signal term (or empty) or kill; root refuses what keeps the Echo running or locked down");
      send_msg(LIST_SERVICE, &b); }
}

static void send_setting_entities(void)
{
    static const char *const eq_ids[] = { "equalizer_bass", "equalizer_mid", "equalizer_treble" };
    static const char *const eq_labels[] = { "Equalizer bass", "Equalizer mid", "Equalizer treble" };   /* sort together in HA */
    for (int i = 0; i < 3; i++) {       /* like the Alexa app's sliders */
        PB(b, 192); pb_str(&b, 1, eq_ids[i]); pb_fixed32(&b, 2, KEY_EQ_BASS + i); pb_str(&b, 3, eq_labels[i]); pb_str(&b, 5, "mdi:tune-vertical");
        pb_float(&b, 6, -6); pb_float(&b, 7, 6); pb_float(&b, 8, 1); pb_str(&b, 11, "dB"); pb_uint(&b, 12, 2); send_msg(LIST_NUMBER, &b);
    }
    { PB(b, 256); pb_str(&b, 1, "mic_level"); pb_fixed32(&b, 2, KEY_MIC_LEVEL); pb_str(&b, 3, "Mic level"); pb_str(&b, 5, "mdi:microphone-plus");
      pb_float(&b, 6, MICGAIN_LEVEL_MIN); pb_float(&b, 7, MICGAIN_LEVEL_MAX); pb_float(&b, 8, 1); pb_uint(&b, 10, 1); pb_str(&b, 11, "dBFS");
      pb_uint(&b, 12, 2); send_msg(LIST_NUMBER, &b); }
    { PB(b, 128); pb_str(&b, 1, "mute"); pb_fixed32(&b, 2, KEY_MUTE); pb_str(&b, 3, "Mute"); pb_str(&b, 5, "mdi:microphone-off"); send_msg(LIST_SWITCH, &b); }
    { PB(b, 128); pb_str(&b, 1, "do_not_disturb"); pb_fixed32(&b, 2, KEY_DND); pb_str(&b, 3, "Do not disturb"); pb_str(&b, 5, "mdi:minus-circle");
      send_msg(LIST_SWITCH, &b); }
    if (listed(KEY_SENDSPIN_TOKEN)) { PB(b, 192); pb_str(&b, 1, "sendspin_pairing_token"); pb_fixed32(&b, 2, KEY_SENDSPIN_TOKEN); pb_str(&b, 3, "Sendspin pairing token");
      pb_str(&b, 5, "mdi:key-link"); pb_uint(&b, 6, 1); pb_uint(&b, 7, 2); send_msg(LIST_TEXT_SENSOR, &b); }
    /* off (the default): only servers paired with the token above play; on: any Music Assistant on the LAN, unpaired */
    if (listed(KEY_SS_UNPAIRED)) { PB(b, 192); pb_str(&b, 1, "sendspin_unpaired_access"); pb_fixed32(&b, 2, KEY_SS_UNPAIRED);
      pb_str(&b, 3, "Music Assistant without pairing"); pb_str(&b, 5, "mdi:lock-open-variant"); pb_uint(&b, 8, 1); send_msg(LIST_SWITCH, &b); }
    { PB(b, 128); pb_str(&b, 1, "noise_reduction"); pb_fixed32(&b, 2, KEY_DENOISE); pb_str(&b, 3, "Noise reduction"); pb_str(&b, 5, "mdi:waveform");
      for (int i = 0; i < 4; i++) pb_str(&b, 6, denoise_names[i]);
      pb_uint(&b, 8, 1); send_msg(LIST_SELECT, &b); }
    { PB(b, 128); pb_str(&b, 1, "wake_sound"); pb_fixed32(&b, 2, KEY_WAKE_SOUND); pb_str(&b, 3, "Wake sound"); pb_str(&b, 5, "mdi:bell-ring");
      pb_uint(&b, 8, 1); send_msg(LIST_SWITCH, &b); }
    /* AirPlay 2 (airplay.c): the switch; off by default, the receiver and its mDNS records exist only while on */
    if (listed(KEY_IMPROV)) { PB(b, 160); pb_str(&b, 1, "wifi_setup_over_bluetooth"); pb_fixed32(&b, 2, KEY_IMPROV); pb_str(&b, 3, "Wi-Fi setup over Bluetooth");
      pb_str(&b, 5, "mdi:wifi-cog"); pb_uint(&b, 8, 1); send_msg(LIST_SWITCH, &b); }
    if (listed(KEY_AP_TITLE)) {                 /* text sensors: the media player entity of ESPHome carries no title, artist or album */
        static const struct { const char *id, *name, *icon; int key; } np[] = { { "airplay_title", "AirPlay title", "mdi:music-note", KEY_AP_TITLE },
            { "airplay_artist", "AirPlay artist", "mdi:account-music", KEY_AP_ARTIST }, { "airplay_album", "AirPlay album", "mdi:album", KEY_AP_ALBUM } };
        for (int i = 0; i < 3; i++) { PB(b, 160); pb_str(&b, 1, np[i].id); pb_fixed32(&b, 2, np[i].key); pb_str(&b, 3, np[i].name); pb_str(&b, 5, np[i].icon); send_msg(LIST_TEXT_SENSOR, &b); }
    }
    if (listed(KEY_AIRPLAY)) { PB(b, 128); pb_str(&b, 1, "airplay_2"); pb_fixed32(&b, 2, KEY_AIRPLAY); pb_str(&b, 3, "AirPlay 2");
      pb_str(&b, 5, "mdi:apple-airplay"); pb_uint(&b, 8, 1); send_msg(LIST_SWITCH, &b); }
    /* Sound detection (sound.h): the switch (off by default: a second decoder on the mic stream) */
    { PB(b, 128); pb_str(&b, 1, "sound_detection"); pb_fixed32(&b, 2, KEY_SOUND_DETECTION); pb_str(&b, 3, "Sound detection");
      pb_str(&b, 5, "mdi:ear-hearing"); pb_uint(&b, 8, 1); send_msg(LIST_SWITCH, &b); }
    /* and one event entity whose event type names the sound; Home Assistant keeps each with its
     * time, automations trigger on the type. */
    if (listed(KEY_SOUND)) { PB(b, 384); pb_str(&b, 1, "sound"); pb_fixed32(&b, 2, KEY_SOUND); pb_str(&b, 3, "Sound"); pb_str(&b, 5, "mdi:waveform");
      for (int i = 0; i < core_sound_nevents; i++) pb_str(&b, 9, core_sound_events[i]);
      send_msg(LIST_EVENT, &b); }
    /* The buttons (core_button_events): what they do on the Echo stays, Home Assistant hears of them as well.  Event
     * types are what automations trigger on: they stay as they are, a new kind of press is a new type */
    { PB(b, 256); pb_str(&b, 1, "button"); pb_fixed32(&b, 2, KEY_BUTTON); pb_str(&b, 3, "Button"); pb_str(&b, 5, "mdi:gesture-tap-button");
      pb_str(&b, 8, "button");
      for (int i = 0; i < core_button_nevents; i++) pb_str(&b, 9, core_button_events[i]);
      send_msg(LIST_EVENT, &b); }
    if (listed(KEY_WAKE)) { PB(b, 192); pb_str(&b, 1, "wake_word"); pb_fixed32(&b, 2, KEY_WAKE); pb_str(&b, 3, "Wake word"); pb_str(&b, 5, "mdi:account-voice");
      for (int i = 0; i < core_wake_nevents; i++) pb_str(&b, 9, core_wake_events[i]);
      send_msg(LIST_EVENT, &b); }
    if (listed(KEY_BT_PAIRING)) { PB(b, 128); pb_str(&b, 1, "bluetooth_pairing"); pb_fixed32(&b, 2, KEY_BT_PAIRING); pb_str(&b, 3, "Bluetooth phones: pairing");
      pb_str(&b, 5, "mdi:bluetooth-connect"); send_msg(LIST_SWITCH, &b); }
    if (listed(KEY_BT_ANNOUNCE)) { PB(b, 128); pb_str(&b, 1, "bluetooth_announcements"); pb_fixed32(&b, 2, KEY_BT_ANNOUNCE); pb_str(&b, 3, "Bluetooth phones: announcements");
      pb_str(&b, 5, "mdi:bluetooth-audio"); pb_uint(&b, 8, 1); send_msg(LIST_SWITCH, &b); }
    if (listed(KEY_BT_AAC)) { PB(b, 160); pb_str(&b, 1, "bluetooth_aac"); pb_fixed32(&b, 2, KEY_BT_AAC); pb_str(&b, 3, "Bluetooth phones: AAC");
      pb_str(&b, 5, "mdi:music-note"); pb_uint(&b, 8, 1); send_msg(LIST_SWITCH, &b); }
    if (listed(KEY_BT_SECURE)) { PB(b, 192); pb_str(&b, 1, "bluetooth_secure_pairing"); pb_fixed32(&b, 2, KEY_BT_SECURE);
      pb_str(&b, 3, "Bluetooth proxy: secure pairing only"); pb_str(&b, 5, "mdi:shield-lock"); pb_uint(&b, 8, 1); send_msg(LIST_SWITCH, &b); }
    if (listed(KEY_BT_LANG)) { PB(b, 512); pb_str(&b, 1, "bluetooth_announcement_language"); pb_fixed32(&b, 2, KEY_BT_LANG);
      pb_str(&b, 3, "Bluetooth phones: announcement language"); pb_str(&b, 5, "mdi:translate");
      for (int i = 0; i < bt_lang_count; i++) pb_str(&b, 6, bt_langs[i].name);
      pb_uint(&b, 8, 1); send_msg(LIST_SELECT, &b); }
    /* a root shell without a password for the whole network while it is on: closes by itself after 30 min (lockdown.sh) */
    { PB(b, 160); pb_str(&b, 1, "debug_access_adb"); pb_fixed32(&b, 2, KEY_ADB_WIFI); pb_str(&b, 3, "Debug access (adb over Wi-Fi)");
      pb_str(&b, 5, "mdi:console-network"); pb_uint(&b, 8, 1); send_msg(LIST_SWITCH, &b); }
    /* playing to a Bluetooth speaker: the search pairs the nearest one in pairing mode and switches playing on it on */
    if (listed(KEY_BT_OUT)) {
        { PB(b, 160); pb_str(&b, 1, "bluetooth_speaker_search"); pb_fixed32(&b, 2, KEY_BT_OUT_SEARCH); pb_str(&b, 3, "Bluetooth speaker: search");
          pb_str(&b, 5, "mdi:speaker-wireless"); send_msg(LIST_SWITCH, &b); }
        { PB(b, 160); pb_str(&b, 1, "play_on_bluetooth_speaker"); pb_fixed32(&b, 2, KEY_BT_OUT); pb_str(&b, 3, "Bluetooth speaker: play on it");
          pb_str(&b, 5, "mdi:speaker-bluetooth"); send_msg(LIST_SWITCH, &b); }
        { PB(b, 160); pb_str(&b, 1, "bluetooth_speaker"); pb_fixed32(&b, 2, KEY_BT_OUT_STATUS); pb_str(&b, 3, "Bluetooth speaker: status");
          pb_str(&b, 5, "mdi:speaker"); send_msg(LIST_TEXT_SENSOR, &b); }
        /* what the speaker adds to the Echo's own latency, for Sendspin's sync with other players: speakers differ */
        { PB(b, 192); pb_str(&b, 1, "bluetooth_speaker_delay"); pb_fixed32(&b, 2, KEY_BT_OUT_DELAY); pb_str(&b, 3, "Bluetooth speaker: delay");
          pb_str(&b, 5, "mdi:timer-sand"); pb_float(&b, 6, 0); pb_float(&b, 7, 1000); pb_float(&b, 8, 10); pb_uint(&b, 10, 1);
          pb_str(&b, 11, "ms"); pb_uint(&b, 12, 2); send_msg(LIST_NUMBER, &b); }
    }
    if (listed(KEY_ARB_JOIN)) { PB(b, 160); pb_str(&b, 1, "join_arbitration_network"); pb_fixed32(&b, 2, KEY_ARB_JOIN); pb_str(&b, 3, "Join arbitration network");
      pb_str(&b, 5, "mdi:account-group"); pb_uint(&b, 8, 1); send_msg(LIST_SWITCH, &b); }
    if (listed(KEY_ARB_SERVICE)) {
        /* the action other Echos hand their network key through: Home Assistant registers it as
         * esphome.<node>_arbitration_key, after the node name we report, whatever the device is called there */
        { PB(b, 256); PB(a, 48); pb_str(&b, 1, "arbitration_key"); pb_fixed32(&b, 2, KEY_ARB_SERVICE);
          pb_str(&a, 1, "network"); pb_uint(&a, 2, 3); pb_bytes(&b, 3, a.p, a.n);
          a.n = 0; pb_str(&a, 1, "key"); pb_uint(&a, 2, 3); pb_bytes(&b, 3, a.p, a.n);
          pb_str(&b, 5, "Used by other Echos (wake word arbitration), not by people"); send_msg(LIST_SERVICE, &b); }
    }
    if (listed(KEY_ARB_PEERS)) {
        { PB(b, 128); pb_str(&b, 1, "arbitration_peers"); pb_fixed32(&b, 2, KEY_ARB_PEERS); pb_str(&b, 3, "Arbitration peers");
          pb_str(&b, 5, "mdi:access-point-network"); pb_uint(&b, 13, 2); send_msg(LIST_SENSOR, &b); }
    }
    /* Wi-Fi motion (wifimotion.c): experimental, and the names say so; the switch is off by default */
    if (listed(KEY_WIFI_MOTION_ON)) { PB(b, 192); pb_str(&b, 1, "wifi_motion_detection"); pb_fixed32(&b, 2, KEY_WIFI_MOTION_ON);
      pb_str(&b, 3, "Wi-Fi motion detection (experimental)"); pb_str(&b, 5, "mdi:wifi-alert"); pb_uint(&b, 8, 1); send_msg(LIST_SWITCH, &b); }
    if (listed(KEY_WIFI_MOTION)) {
        { PB(b, 192); pb_str(&b, 1, "wifi_motion"); pb_fixed32(&b, 2, KEY_WIFI_MOTION); pb_str(&b, 3, "Wi-Fi motion (experimental)");
          pb_str(&b, 5, "motion"); send_msg(LIST_BINARY_SENSOR, &b); }
        { PB(b, 192); pb_str(&b, 1, "wifi_motion_sensitivity"); pb_fixed32(&b, 2, KEY_WIFI_MOTION_SENS);
          pb_str(&b, 3, "Wi-Fi motion sensitivity (experimental)"); pb_str(&b, 5, "mdi:tune");
          pb_float(&b, 6, WIFIMOTION_SENS_MIN); pb_float(&b, 7, WIFIMOTION_SENS_MAX); pb_float(&b, 8, 1); pb_uint(&b, 10, 1); pb_uint(&b, 12, 2);
          send_msg(LIST_NUMBER, &b); }
    }
    /* Timers: "binary_sensor.<node>_timer_ringing" still on a minute after it came on = nobody heard it, for an
     * automation that announces it elsewhere.  The names (or the length of a timer without one) are a diagnostic beside
     * it, set before the ring goes on so a trigger on it can read them */
    { PB(b, 128); pb_str(&b, 1, "timer_ringing"); pb_fixed32(&b, 2, KEY_TIMER_RINGING); pb_str(&b, 3, "Timer ringing");
      pb_str(&b, 8, "mdi:timer-alert"); send_msg(LIST_BINARY_SENSOR, &b); }
    { PB(b, 128); pb_str(&b, 1, "ringing_timers"); pb_fixed32(&b, 2, KEY_TIMER_NAMES); pb_str(&b, 3, "Ringing timers");
      pb_str(&b, 5, "mdi:timer-outline"); pb_uint(&b, 7, 2); send_msg(LIST_TEXT_SENSOR, &b); }
    /* ends the ring and nothing else (issue #21): the media player's stop does too, but it also stops what the media
     * player plays, so an automation had to start the music again */
    { PB(b, 128); pb_str(&b, 1, "stop_ringing"); pb_fixed32(&b, 2, KEY_TIMER_STOP); pb_str(&b, 3, "Stop ringing");
      pb_str(&b, 5, "mdi:timer-off-outline"); send_msg(LIST_BUTTON, &b); }
    /* Drop In (dropin.c): "Drop In with" first, so a trigger on "Drop In" finds it set */
    if (listed(KEY_DROPIN_SERVICE)) {
        { PB(b, 256); PB(a, 48); pb_str(&b, 1, "drop_in"); pb_fixed32(&b, 2, KEY_DROPIN_SERVICE);
          pb_str(&a, 1, "target"); pb_uint(&a, 2, 3); pb_bytes(&b, 3, a.p, a.n);
          pb_str(&b, 5, "Drop in on another Echo: its name (\"Echo Dot 3 5695c4\", as the device was first named) or node"); send_msg(LIST_SERVICE, &b); }
        { PB(b, 128); pb_str(&b, 1, "drop_in_with"); pb_fixed32(&b, 2, KEY_DROPIN_PEER); pb_str(&b, 3, "Drop In with");
          pb_str(&b, 5, "mdi:account-voice"); send_msg(LIST_TEXT_SENSOR, &b); }
        { PB(b, 128); pb_str(&b, 1, "drop_in"); pb_fixed32(&b, 2, KEY_DROPIN_STATE); pb_str(&b, 3, "Drop In");
          pb_str(&b, 5, "mdi:phone-in-talk"); send_msg(LIST_TEXT_SENSOR, &b); }
        { PB(b, 128); pb_str(&b, 1, "end_drop_in"); pb_fixed32(&b, 2, KEY_DROPIN_END); pb_str(&b, 3, "End Drop In");
          pb_str(&b, 5, "mdi:phone-hangup"); send_msg(LIST_BUTTON, &b); }
        /* What came of each call this Echo made, as soon as it is known: refused (and why), not answered, connected.
         * An event entity: its state is the time of the last one, new for every call however it ended (HA forces each
         * event's time past the last one: event/__init__.py _trigger_event), so the voice blueprint tells this call's
         * from the last one's by the state alone.  "Drop In" cannot: a refusal is calling, then idle as before */
        { PB(b, 512); pb_str(&b, 1, "drop_in_result"); pb_fixed32(&b, 2, KEY_DROPIN_RESULT); pb_str(&b, 3, "Drop In result");
          pb_str(&b, 5, "mdi:phone-check");
          for (int i = 0; i < DROPIN_NRESULTS; i++) pb_str(&b, 9, dropin_results[i]);
          send_msg(LIST_EVENT, &b); }
    }
    if (core_web_port) { PB(b, 128); pb_str(&b, 1, "web_ui_address"); pb_fixed32(&b, 2, KEY_WEB_URL); pb_str(&b, 3, "Web UI address");
      pb_str(&b, 5, "mdi:web"); pb_uint(&b, 7, 2); send_msg(LIST_TEXT_SENSOR, &b); }
    send_alarm_entities();
    send_task_entities();
    /* diagnostics, disabled by default in Home Assistant: SoC temperature and CPU usage, pushed every 30 s */
    { PB(b, 192); pb_str(&b, 1, "soc_temperature"); pb_fixed32(&b, 2, KEY_SOC_TEMP); pb_str(&b, 3, "SoC temperature");
      pb_str(&b, 5, "mdi:thermometer"); pb_str(&b, 6, "\xc2\xb0" "C"); pb_uint(&b, 7, 1); pb_str(&b, 9, "temperature"); pb_uint(&b, 10, 1);
      pb_uint(&b, 12, 1); pb_uint(&b, 13, 2); send_msg(LIST_SENSOR, &b); }
    { PB(b, 192); pb_str(&b, 1, "cpu_usage"); pb_fixed32(&b, 2, KEY_CPU_USAGE); pb_str(&b, 3, "CPU usage");
      pb_str(&b, 5, "mdi:chip"); pb_str(&b, 6, "%"); pb_uint(&b, 10, 1); pb_uint(&b, 12, 1); pb_uint(&b, 13, 2); send_msg(LIST_SENSOR, &b); }
    /* the running build (an update is pushed from the PC: scripts/ota-push.sh, so this is all Home Assistant needs to show) */
    { PB(b, 128); pb_str(&b, 1, "firmware"); pb_fixed32(&b, 2, KEY_FIRMWARE); pb_str(&b, 3, "Firmware");
      pb_str(&b, 5, "mdi:chip"); pb_uint(&b, 7, 2); send_msg(LIST_TEXT_SENSOR, &b); }
    /* which Echo is this: a diagnostic button with HA's "identify" class, as ESPHome's own devices offer */
    { PB(b, 128); pb_str(&b, 1, "identify"); pb_fixed32(&b, 2, KEY_IDENTIFY); pb_str(&b, 3, "Identify");
      pb_uint(&b, 7, 2); pb_str(&b, 8, "identify"); send_msg(LIST_BUTTON, &b); }
    /* ESPHome's factory_reset button: a configuration entity, so no dashboard shows it by itself.  Everything is
     * forgotten, Home Assistant's key and the Wi-Fi networks too (core_reset): the Echo starts its setup again */
    { PB(b, 128); pb_str(&b, 1, "factory_reset"); pb_fixed32(&b, 2, KEY_RESET); pb_str(&b, 3, "Factory reset");
      pb_str(&b, 5, "mdi:restart-alert"); pb_uint(&b, 7, 1); pb_str(&b, 8, "restart"); send_msg(LIST_BUTTON, &b); }
    /* ESPHome's restart button: the whole Echo, as a power cycle (core_reboot) */
    { PB(b, 128); pb_str(&b, 1, "restart"); pb_fixed32(&b, 2, KEY_RESTART); pb_str(&b, 3, "Restart");
      pb_uint(&b, 7, 1); pb_str(&b, 8, "restart"); send_msg(LIST_BUTTON, &b); }
    send_light_entities();
}

static void on_setting(unsigned type, const unsigned char *p, const unsigned char *end)
{
    struct pbf f; unsigned key = 0; float num = 0; int on = 0; char opt[32] = ""; const unsigned char *msg0 = p;
    while (pb_next(&p, end, &f)) {
        if (f.field == 1) key = f.v;
        else if (f.field == 2 && f.wire == 5) { uint32_t u = f.v; memcpy(&num, &u, 4); }
        else if (f.field == 2 && f.wire == 0) on = f.v != 0;
        else if (f.field == 2 && f.data) pbf_str(&f, opt, sizeof opt);
    }
    if (on_alarm_command(type, key, on, opt, msg0, end)) return;
    if (type == SELECT_COMMAND && key == KEY_DENOISE) { for (int i = 0; i < 4; i++) if (!strcmp(opt, denoise_names[i])) core_mic_denoise(i); }
    else if (type == SELECT_COMMAND && key == KEY_BT_LANG) { for (int i = 0; i < bt_lang_count; i++) if (!strcmp(opt, bt_langs[i].name)) settings_bt_lang_index(i); }
    else if (type == NUMBER_COMMAND && key == KEY_MIC_LEVEL) {
        char v[16], e[80]; int l = num < MICGAIN_LEVEL_MIN ? MICGAIN_LEVEL_MIN : num > MICGAIN_LEVEL_MAX ? MICGAIN_LEVEL_MAX : (int)lroundf(num);
        snprintf(v, sizeof v, "%d", l); settings_set("mic_level", v, e, sizeof e); return;
    }
    else if (type == SWITCH_COMMAND && key == KEY_MUTE) { core_soft_mute(on); settings_save(); send_setting(KEY_MUTE); return; }
    else if (type == SWITCH_COMMAND && key == KEY_WAKE_SOUND) core_wake_sound(on);
    else if (type == SWITCH_COMMAND && key == KEY_BT_ANNOUNCE) core_bt_announce(on);
    else if (type == SWITCH_COMMAND && (key == KEY_BT_AAC || key == KEY_BT_SECURE) && ble_present()) {
        /* the weaker way (AAC on: the firmware's decoder takes what phones send; secure pairing only off: legacy pairing again)
         * only over the keyed connection: before Home Assistant has set the key anyone on the network could connect and flip it */
        int c = client_of(reply_fd), weaker = key == KEY_BT_AAC ? on : !on;
        if (!weaker || (c >= 0 && clients[c].keyed)) { char e[80]; settings_set(key == KEY_BT_AAC ? "bluetooth_aac" : "bluetooth_secure_pairing", on ? "on" : "off", e, sizeof e); }
        else fprintf(stderr, "bluetooth: %s refused, the request did not come over the keyed connection\n", key == KEY_BT_AAC ? "AAC on" : "secure pairing off");
        send_setting(key); return;
    }
    else if (type == SWITCH_COMMAND && key == KEY_DND) core_dnd(on);
    else if (type == SWITCH_COMMAND && key == KEY_BT_PAIRING) { a2dp_pair(on); return; }     /* its state follows through bt_changed */
    else if (type == SWITCH_COMMAND && key == KEY_BT_OUT_SEARCH && ble_present()) { a2dp_out_search(on); return; }   /* a2dp.c keeps them */
    else if (type == SWITCH_COMMAND && key == KEY_BT_OUT && ble_present()) { a2dp_out_enable(on); send_setting(key); return; }
    else if (type == NUMBER_COMMAND && key == KEY_BT_OUT_DELAY && ble_present()) { a2dp_out_delay(num < 0 ? 0 : (int)lroundf(num)); send_setting(key); return; }
    else if (type == SWITCH_COMMAND && key == KEY_ARB_JOIN && arb_running()) { char e[80]; settings_set("arbitration", on ? "on" : "off", e, sizeof e); send_setting(key); return; }   /* a feature: the list follows */
    else if (type == SWITCH_COMMAND && key == KEY_SS_UNPAIRED && core_sendspin_port) { sendspin_unpaired(on); send_setting(key); return; }  /* sendspin.c keeps it */
    else if (type == SWITCH_COMMAND && key == KEY_ADB_WIFI) {
        /* Before Home Assistant has set the key anyone on the network gets a connection: that must not be a way to
         * a root shell.  Closing it is always fine. */
        int c = client_of(reply_fd);
        if (!on || (c >= 0 && clients[c].keyed)) adbwifi_ask(on);
        else fprintf(stderr, "adb over Wi-Fi: refused, the request did not come over the keyed connection\n");
        send_setting(key); return;
    }
    else if (type == SWITCH_COMMAND && key == KEY_IMPROV && ble_present()) { char e[80]; settings_set("wifi_setup_bluetooth", on ? "on" : "off", e, sizeof e); send_setting(key); return; }
    else if (type == SWITCH_COMMAND && key == KEY_AIRPLAY && airplay_present()) { char e[80]; settings_set("airplay", on ? "on" : "off", e, sizeof e); send_setting(key); return; }
    else if (type == SWITCH_COMMAND && key == KEY_SOUND_DETECTION) { char e[80]; settings_set("sound_detection", on ? "on" : "off", e, sizeof e); send_setting(key); return; }
    else if (type == SWITCH_COMMAND && key == KEY_WIFI_MOTION_ON && wifimotion_present()) {
        char e[80]; settings_set("wifi_motion", on ? "on" : "off", e, sizeof e); send_setting(key); return;       /* a feature: the list follows */
    }
    else if (type == NUMBER_COMMAND && key == KEY_WIFI_MOTION_SENS && wifimotion_present()) {
        wifimotion_sensitivity((int)lroundf(num)); settings_save(); send_setting(key); return;
    }
    else if (type == SWITCH_COMMAND && key == KEY_LED_AUTO && have_light) {
        core_led_auto(on); settings_save(); send_setting(key); return;              /* the level follows through light_thread */
    }
    else if (type == NUMBER_COMMAND && key == KEY_LED_BRIGHTNESS) {                  /* a level of its own: auto off, or it moves */
        core_led_brightness(num < 0 ? 0 : (int)lroundf(num)); settings_save(); send_setting(KEY_LED_AUTO); send_setting(key); return;
    }
    else if (type == NUMBER_COMMAND && key >= KEY_ASSIST_VOLUME && key <= KEY_CALL_VOLUME && listed(key)) {   /* saved, shown: settings.c */
        static const char *const names[] = { "assistant_volume", "sounds_volume", "timer_volume", "call_volume" };
        char v[8], e[80]; snprintf(v, sizeof v, "%d", num < 0 ? 0 : num > 100 ? 100 : (int)lroundf(num));
        settings_set(names[key - KEY_ASSIST_VOLUME], v, e, sizeof e); return;
    }
    else if (type == NUMBER_COMMAND && key == KEY_RING_FILL && listed(key)) {
        struct ring_light l; ring_light_get(&l); l.fill = num < 0 ? 0 : num > 100 ? 100 : (int)lroundf(num);
        ring_light_set(&l, 0); send_setting(key); return;
    }
    else if (type == NUMBER_COMMAND && key == KEY_RING_ROTATION && listed(key)) {              /* saved, shown: settings.c */
        char v[8], e[80]; snprintf(v, sizeof v, "%d", num < 0 ? 0 : (int)lroundf(num));
        settings_set("ring_rotation", v, e, sizeof e); return;
    }
    else if (type == NUMBER_COMMAND && key >= KEY_EQ_BASS && key <= KEY_EQ_TREBLE) {           /* the mixer keeps it, not our file */
        core_set_eq(key - KEY_EQ_BASS, (int)lroundf(num)); send_setting(key); return;
    }
    else return;
    fprintf(stderr, "settings: mic_level=%d noise_reduction=%s mute=%d wake_sound=%d bt_announce=%d dnd=%d bt_lang=%s\n", settings_mic_level(),
            denoise_names[core_mic_denoise(-1)], core_soft_mute(-1), core_wake_sound(-1), core_bt_announce(-1), core_dnd(-1), settings_bt_lang()->code);
    settings_save(); send_setting(key);
}

/* The action light_ring_leds: "#rrggbb" (or without "#") per LED from the fill's start, anything else dark.  The light
 * on with them; an empty list is the light's own colour again */
static void ring_leds_from(char colors[][16], int n)        /* lock held */
{
    struct ring_light l; ring_light_get(&l);
    for (int i = 0; i < n; i++) {
        const char *c = colors[i] + (colors[i][0] == '#');
        memset(l.led_rgb[i], 0, 3);
        if (strlen(c) == 6 && strspn(c, "0123456789abcdefABCDEF") == 6) {
            unsigned long v = strtoul(c, NULL, 16);
            l.led_rgb[i][0] = (unsigned char)(v >> 16); l.led_rgb[i][1] = (unsigned char)(v >> 8); l.led_rgb[i][2] = (unsigned char)v;
        }
        else if (*c && strcasecmp(c, "off") && strcasecmp(c, "black")) fprintf(stderr, "light ring: \"%s\" is no colour (#rrggbb), dark\n", colors[i]);
    }
    l.nleds = n; l.on = 1;
    ring_light_set(&l, 0);
    send_setting(KEY_LIGHT);
}

/* Home Assistant's light: what the command has, over what it was (ESPHome sends only what changed).  A flash shows
 * blinking for its length and leaves the light as it was */
static void on_light(const unsigned char *p, const unsigned char *end)      /* lock held */
{
    struct pbf f; struct ring_light l; unsigned key = 0, flash = 0; int has[32] = { 0 }; float v[32] = { 0 }; char eff[32] = "";
    ring_light_get(&l);
    while (pb_next(&p, end, &f)) {
        if (f.field >= 32) continue;
        if (f.field == 1) key = (unsigned)f.v;
        else if (f.wire == 5) { uint32_t u = (uint32_t)f.v; memcpy(&v[f.field], &u, 4); }
        else if (f.field == 19 && f.data) pbf_str(&f, eff, sizeof eff);
        else if (f.field == 17) flash = (unsigned)f.v;
        else if (f.field == 3) v[3] = f.v != 0;
        else has[f.field] = f.v != 0;
    }
    if (key != KEY_LIGHT || !listed(KEY_LIGHT)) return;
    if (has[2]) l.on = v[3] != 0;
    if (has[4]) l.brightness = v[5];
    if (has[20]) l.color_brightness = v[21];
    if (has[6]) { l.r = v[7]; l.g = v[8]; l.b = v[9]; l.nleds = 0; }      /* one colour again */
    if (has[18]) {
        l.nleds = 0;
        if (!strcasecmp(eff, "off")) l.effect = 0;        /* Home Assistant's own name for none */
        for (int i = 0; i < ring_neffects; i++) if (!strcasecmp(eff, ring_effects[i])) l.effect = i;
    }
    ring_light_set(&l, has[16] ? flash : 0);
    send_setting(KEY_LIGHT);
}

/* ---------------------------------------------------------------- media over http (announcements, play_media) */

struct media_job {
    char url[2][1024]; int announce, start_conversation, media;     /* media: play_media, at the main volume */
    int fd, aborted;                    /* lock held.  aborted: its run was cancelled; it plays nothing and touches no state */
    /* the job's own buffers and decoder, not static ones: cancel() lets go of a job whose thread may still be reading,
     * and the next run's reply starts its own thread at once */
    unsigned char buf[32768]; mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME]; mp3dec_t dec;
};

static int job_aborted(struct media_job *job)
{
    pthread_mutex_lock(&core_lock); int a = job->aborted; pthread_mutex_unlock(&core_lock);
    return a;
}

static void send_mp_state(void)         /* lock held */
{
    PB(b, 32);
    pb_fixed32(&b, 1, MP_KEY); pb_uint(&b, 2, media_playing || core_music_active() ? MP_PLAYING : MP_IDLE); pb_float(&b, 3, core_volume() / 100.0f);
    send_state(MEDIA_PLAYER_STATE, &b);
}

/* A music source began or ended playing: the entity is "playing" while one does, so an automation or a voice command ("stop the music" finds the media
 * player that is playing) sees it, and its pause and stop reach it (on_mp_command) */
static void send_ap_meta(void)          /* lock held: what AirPlay plays, as three text sensors (the media player entity has no title) */
{
    char t[APMETA_TEXT], a[APMETA_TEXT], l[APMETA_TEXT];
    if (!listed(KEY_AP_TITLE)) return;
    airplay_meta(t, a, l);
    send_text(KEY_AP_TITLE, t); send_text(KEY_AP_ARTIST, a); send_text(KEY_AP_ALBUM, l);
}

void esphome_media_changed(void) { send_mp_state(); send_ap_meta(); }

static int http_get_host(const char *host, const char *port, const char *path)
{
    char req[1400], line[1024]; int v6 = strchr(host, ':') != NULL;
    int fd = net_connect(host, port, 4);
    if (fd < 0) return -1;
    struct timeval tv = { 15, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);     /* TTS may still be rendering */
    /* HTTP/1.0: no chunked transfer encoding, the body ends with the connection */
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s%s%s:%s\r\nUser-Agent: hassmic/" VERSION "\r\n\r\n",
                     path, v6 ? "[" : "", host, v6 ? "]" : "", port);
    if (write_all(fd, req, n) < 0) { close(fd); return -1; }
    for (int first = 1;; first = 0) {                   /* header lines; the body stays in the socket */
        ssize_t i = net_read_until(fd, line, sizeof line, "\n", 0);
        line[i > 0 ? strcspn(line, "\r\n") : 0] = 0;
        if (first && !strstr(line, " 200")) { fprintf(stderr, "media: http://%s:%s%s -> %s\n", host, port, path, line); close(fd); return -1; }
        if (i <= 0) { close(fd); return -1; }           /* cut off, or a header line longer than we take */
        if (i <= 2) return fd;                          /* "\r\n": end of headers */
    }
}

static int http_get(const char *url)    /* returns a socket positioned at the body, or -1 */
{
    char host[256], port[8] = "80"; const char *p = url, *path;
    if (!strncasecmp(p, "https://", 8)) return https_open(url);     /* Home Assistant serving HTTPS itself (https.c) */
    if (strncmp(p, "http://", 7)) { fprintf(stderr, "media: only http:// and https:// are supported: %s\n", url); return -1; }
    p += 7; path = strchr(p, '/');
    snprintf(host, sizeof host, "%.*s", (int)(path ? path - p : (long)strlen(p)), p);
    char *colon = strrchr(host, ':'), *close_br = strrchr(host, ']');
    if (colon && close_br > colon) colon = NULL;                            /* "[fe80::1]": the colons are the address */
    if (colon) { *colon = 0; snprintf(port, sizeof port, "%s", colon + 1); }
    if (host[0] == '[' && (close_br = strchr(host, ']'))) { *close_br = 0; memmove(host, host + 1, strlen(host)); }
    return http_get_host(host, port, path ? path : "/");
}

static int tts_begin(struct media_job *job, unsigned rate, unsigned ch)    /* -1: cancelled, the reply must not play */
{
    pthread_mutex_lock(&core_lock);
    int ok = !job->aborted;
    if (ok) core_tts_begin(rate, ch, job->media);
    pthread_mutex_unlock(&core_lock);
    return ok ? 0 : -1;
}

static int feed(struct media_job *job, const void *pcm, size_t len)     /* queue with back pressure; -1: cancelled */
{
    if (job_aborted(job)) return -1;    /* the queue may hold the next run's reply by now */
    core_tts_data(pcm, len);
    while (core_tts_queued() > 256 * 1024 && !core_tts_flushing()) { if (job_aborted(job)) return -1; usleep(50000); }
    return 0;
}

static int play_wav(struct media_job *job, int fd, const unsigned char *head, size_t have)
{
    unsigned char ck[8], fmt[16], *buf = job->buf; unsigned rate = 0, ch = 0, bits = 0; const size_t cap = sizeof job->buf;
    (void)head; (void)have;                             /* the 12 byte RIFF/WAVE header, already consumed */
    for (;;) {
        if (read_full(fd, ck, 8) != 8) return -1;
        uint32_t len; memcpy(&len, ck + 4, 4);
        if (!memcmp(ck, "data", 4)) break;
        if (!memcmp(ck, "fmt ", 4) && len >= 16) {
            if (read_full(fd, fmt, 16) != 16) return -1;
            ch = fmt[2] | fmt[3] << 8; memcpy(&rate, fmt + 4, 4); bits = fmt[14] | fmt[15] << 8; len -= 16;
        }
        for (len += len & 1; len; ) { ssize_t r = read(fd, buf, len < cap ? len : cap); if (r <= 0) return -1; len -= r; }
    }
    /* what the mixer takes (MixerOpenPlay): Home Assistant sends 48 kHz mono here, anything outside is not ours */
    if (bits != 16 || rate < 8000 || rate > 48000 || ch < 1 || ch > 2) { fprintf(stderr, "media: unsupported WAV (%u Hz, %u ch, %u bit)\n", rate, ch, bits); return -1; }
    fprintf(stderr, "media: WAV %u Hz x%u\n", rate, ch);
    if (tts_begin(job, rate, ch) < 0) return -1;
    size_t odd = 0;
    for (ssize_t r; !core_tts_flushing() && (r = read(fd, buf + odd, cap - odd)) > 0; ) {
        size_t n = odd + r, use = n - n % (2 * ch);                 /* whole frames only */
        if (feed(job, buf, use) < 0) break;
        odd = n - use; memmove(buf, buf + use, odd);
    }
    return 0;
}

/* Home Assistant sends TTS announcements as they come from the TTS engine, usually MP3 (it only asks for WAV once a
 * voice pipeline has run), so that has to play too. */
static int play_mp3(struct media_job *job, int fd, const unsigned char *head, size_t have)
{
    unsigned char *in = job->buf; mp3d_sample_t *pcm = job->pcm; const size_t cap = sizeof job->buf;
    mp3dec_frame_info_t info; size_t n = have; int began = 0, eof = 0;
    memcpy(in, head, have);
    mp3dec_init(&job->dec);
    while (!core_tts_flushing()) {
        while (!eof && n < cap) { ssize_t r = read(fd, in + n, cap - n); if (r <= 0) eof = 1; else n += r; }
        if (!n) break;
        int samples = mp3dec_decode_frame(&job->dec, in, n, pcm, &info);
        if (!info.frame_bytes) { if (eof) break; n = 0; continue; }     /* no sync in the whole buffer */
        if (samples > 0) {
            if (!began) {
                fprintf(stderr, "media: MP3 %d Hz x%d\n", info.hz, info.channels);
                if (tts_begin(job, info.hz, info.channels) < 0) break;
                began = 1;
            }
            if (feed(job, pcm, (size_t)samples * info.channels * 2) < 0) break;
        }
        n -= info.frame_bytes; memmove(in, in + info.frame_bytes, n);
    }
    if (!began) fprintf(stderr, "media: no MP3 audio found\n");
    return began ? 0 : -1;
}

static int play_url(struct media_job *job, int fd)
{
    unsigned char h[12];
    if (read_full(fd, h, 12) != 12) { fprintf(stderr, "media: empty response\n"); return -1; }
    if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) return play_wav(job, fd, h, 12);
    if (!memcmp(h, "fLaC", 4) || !memcmp(h, "OggS", 4)) { fprintf(stderr, "media: FLAC/Ogg is not supported, only WAV and MP3\n"); return -1; }
    return play_mp3(job, fd, h, 12);
}

static void *media_thread(void *arg)
{
    struct media_job *job = arg; int ok = 0, began = 0;
    for (int i = 0; i < 2; i++) {
        if (!job->url[i][0] || core_tts_flushing()) continue;
        int fd = http_get(job->url[i]);
        if (fd < 0) continue;
        pthread_mutex_lock(&core_lock); job->fd = fd; int gone = job->aborted; pthread_mutex_unlock(&core_lock);
        if (!gone && play_url(job, fd) == 0) { began = 1; if (i == 1) ok = 1; }
        pthread_mutex_lock(&core_lock); job->fd = -1; pthread_mutex_unlock(&core_lock);
        close(fd);
    }
    pthread_mutex_lock(&core_lock);
    if (job->aborted) {                 /* cancel() has let go of it: a new run and its reply may be under way already */
        pthread_mutex_unlock(&core_lock);
        free(job);
        return NULL;
    }
    if (media_cur == job) media_cur = NULL;
    if (job->announce) {
        if (job->start_conversation && ok) core_restart_after();
        if (!began) { PB(b, 8); pb_uint(&b, 1, 0); send_va(VA_ANNOUNCE_FINISHED, &b); announcing = 0; announce_done(); }
    }
    if (!began) { media_playing = 0; send_mp_state(); core_pipeline_finish(); atomic_store(&media_busy, 0); }
    pthread_mutex_unlock(&core_lock);
    if (began) core_tts_end();          /* played() below finishes the job once the audio is out */
    free(job);
    return NULL;
}

/* lock held.  url0 = optional chime, url1 = the media.  media: music, at the music volume (main.c); else speech, at the
 * replies' (separate volumes) */
static void media_start(const char *url0, const char *url1, int announce, int start_conversation, int is_reply, int media)
{
    struct media_job *job = calloc(1, sizeof *job); pthread_t t;
    if (!job) return;
    if (atomic_exchange(&media_busy, 1)) {              /* one at a time: refuse, the running one finishes first */
        fprintf(stderr, "media: busy, dropped %s\n", url1);
        if (announce) { PB(b, 8); pb_uint(&b, 1, 0); send_va(VA_ANNOUNCE_FINISHED, &b); }
        free(job); return;
    }
    if (!is_reply && (core_state() == LISTENING || core_state() == THINKING)) core_pipeline_finish();
    snprintf(job->url[0], sizeof job->url[0], "%s", url0); snprintf(job->url[1], sizeof job->url[1], "%s", url1);
    job->announce = announce; job->start_conversation = start_conversation; job->media = media; job->fd = -1;
    media_cur = job;
    announcing = announce; media_playing = 1; send_mp_state();
    fprintf(stderr, "media: %s%s\n", announce ? "announce " : "", url1);
    pthread_create(&t, NULL, media_thread, job); pthread_detach(t);
}

/* ---------------------------------------------------------------- bluetooth proxy
 * Home Assistant subscribes to raw advertisements and parses them itself.  The radio scans only while someone is
 * subscribed.  Active scanning (scan requests, so names from scan responses) is the default, like ESPHome's; Home
 * Assistant can switch the mode (not kept across restarts, it sets it again).
 * Connections: Home Assistant asks for a free slot (connections free), connects, fetches the services once and caches
 * them, then reads, writes, subscribes and pairs.  Answers go to every client that uses the proxy (subscribed, or sent a
 * request).  When the last one leaves,
 * its connections are closed, as ESPHome does. */

static int ble_mode = BLE_ACTIVE;       /* lock held */

static int ble_subscribers(void)        /* lock held */
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) n += clients[i].fd >= 0 && clients[i].ble;
    return n;
}

static void ble_update(void) { ble_scan(ble_subscribers() > 0, ble_mode == BLE_ACTIVE); }      /* lock held */

static void send_ble(unsigned type, const struct pb *b)                                        /* lock held */
{
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && clients[i].ble) send_to(clients[i].fd, type, b);
}

static void send_scanner_state(void)    /* lock held */
{
    PB(b, 16);
    pb_uint(&b, 1, ble_scanning() ? BLE_RUNNING : BLE_IDLE); pb_uint(&b, 2, ble_mode); pb_uint(&b, 3, ble_mode);
    send_ble(BLE_SCANNER_STATE, &b);
}

static void ble_changed(void) { pthread_mutex_lock(&core_lock); send_scanner_state(); pthread_mutex_unlock(&core_lock); }

static void send_gatt(unsigned type, const struct pb *b)                                      /* lock held */
{
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && (clients[i].ble || clients[i].ble_free || clients[i].ble_user)) send_to(clients[i].fd, type, b);
}

static void send_slots(int to_asker)    /* lock held */
{
    PB(b, 64); uint64_t a[BLE_MAX_CONN]; int n = ble_connections(a);
    pb_uint(&b, 1, BLE_MAX_CONN - n); pb_uint(&b, 2, BLE_MAX_CONN);
    if (n) { PB(al, 40); for (int i = 0; i < n; i++) pb_varint(&al, a[i]); pb_bytes(&b, 3, al.p, al.n); }    /* packed */
    if (to_asker) { send_msg(BLE_CONN_FREE, &b); return; }
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && clients[i].ble_free) send_to(clients[i].fd, BLE_CONN_FREE, &b);
}

static void ble_slots(void) { pthread_mutex_lock(&core_lock); send_slots(0); pthread_mutex_unlock(&core_lock); }

static void ble_connection(uint64_t addr, int connected, unsigned mtu, int error)
{
    PB(b, 32);
    pb_uint(&b, 1, addr); pb_uint(&b, 2, connected); pb_uint(&b, 3, mtu); pb_uint(&b, 4, error);
    pthread_mutex_lock(&core_lock); send_gatt(BLE_CONNECTION, &b); pthread_mutex_unlock(&core_lock);
}

static void pb_uuid(struct pb *b, int f, const uint64_t u[2])  /* repeated uint64, packed */
{
    PB(v, 20); pb_varint(&v, u[0]); pb_varint(&v, u[1]); pb_bytes(b, f, v.p, v.n);
}

/* one message per service, like ESPHome: a big database would not fit into one */
static void ble_db(uint64_t addr, const struct ble_db *db)
{
    static unsigned char buf[8000], sb[7900], cb[1200], db_[64];
    pthread_mutex_lock(&core_lock);
    for (int s = 0; s < db->nsvc; s++) {
        const struct ble_svc *sv = &db->svc[s]; struct pb m = { buf, 0, sizeof buf }, sm = { sb, 0, sizeof sb };
        pb_uuid(&sm, 1, sv->uuid); pb_uint(&sm, 2, sv->start);
        for (int c = sv->first; c < sv->first + sv->n; c++) {
            const struct ble_chr *ch = &db->chr[c]; struct pb cm = { cb, 0, sizeof cb };
            pb_uuid(&cm, 1, ch->uuid); pb_uint(&cm, 2, ch->handle); pb_uint(&cm, 3, ch->props);
            for (int d = ch->first; d < ch->first + ch->n; d++) {
                struct pb dm = { db_, 0, sizeof db_ };
                pb_uuid(&dm, 1, db->dsc[d].uuid); pb_uint(&dm, 2, db->dsc[d].handle);
                pb_bytes(&cm, 4, dm.p, dm.n);
            }
            pb_bytes(&sm, 3, cm.p, cm.n);
        }
        if (sm.n >= sm.cap - 64) fprintf(stderr, "bluetooth: service %d of %012llx too big, truncated\n", s, (unsigned long long)addr);
        pb_uint(&m, 1, addr); pb_bytes(&m, 2, sm.p, sm.n);
        send_gatt(BLE_SERVICES, &m);
    }
    { PB(b, 16); pb_uint(&b, 1, addr); send_gatt(BLE_SERVICES_DONE, &b); }
    pthread_mutex_unlock(&core_lock);
}

static void ble_data(unsigned type, uint64_t addr, unsigned handle, const void *data, size_t len, int with_data)
{
    PB(b, 600);
    pb_uint(&b, 1, addr); pb_uint(&b, 2, handle); if (with_data) pb_bytes(&b, 3, data, len);
    pthread_mutex_lock(&core_lock); send_gatt(type, &b); pthread_mutex_unlock(&core_lock);
}

static void ble_read_cb(uint64_t a, unsigned h, const void *d, size_t n) { ble_data(BLE_READ, a, h, d, n, 1); }
static void ble_notify_cb(uint64_t a, unsigned h, const void *d, size_t n) { ble_data(BLE_NOTIFY_DATA, a, h, d, n, 1); }
static void ble_written_cb(uint64_t a, unsigned h) { ble_data(BLE_WRITTEN, a, h, NULL, 0, 0); }
static void ble_error_cb(uint64_t addr, unsigned handle, int error)
{
    PB(b, 32);
    pb_uint(&b, 1, addr); pb_uint(&b, 2, handle); pb_uint(&b, 3, error);
    pthread_mutex_lock(&core_lock); send_gatt(BLE_GATT_ERROR, &b); pthread_mutex_unlock(&core_lock);
}

static void on_ble_request(unsigned type, const unsigned char *p, const unsigned char *end)   /* lock held */
{
    struct pbf f; uint64_t addr = 0; unsigned kind = 0, handle = 0, addr_type = 0, flag = 0; const unsigned char *data = NULL; size_t len = 0;
    while (pb_next(&p, end, &f)) switch (f.field) {
        case 1: addr = f.v; break;
        case 2: if (type == BLE_DEVICE_REQ) kind = f.v; else handle = f.v; break;
        case 3: if (type == BLE_WRITE_DESC_REQ && f.data) { data = f.data; len = f.len; } else flag = f.v != 0; break;
        case 4: if (type == BLE_DEVICE_REQ) addr_type = f.v; else if (f.data) { data = f.data; len = f.len; } break;
    }
    switch (type) {
    case BLE_DEVICE_REQ:
        if (kind == BLE_REQ_DISCONNECT) ble_disconnect(addr);
        else if (kind == BLE_REQ_PAIR) ble_pair(addr);
        else if (kind == BLE_REQ_UNPAIR) ble_unpair(addr);
        else if (kind == BLE_REQ_CLEAR_CACHE) { PB(b, 24); pb_uint(&b, 1, addr); pb_uint(&b, 2, 1); send_msg(BLE_CACHE_CLEARED, &b); }
        else ble_connect(addr, addr_type);
        break;
    case BLE_SERVICES_REQ:   ble_services(addr); break;
    case BLE_READ_REQ: case BLE_READ_DESC_REQ: ble_read(addr, handle); break;
    case BLE_WRITE_REQ:      ble_write(addr, handle, data, len, flag); break;
    case BLE_WRITE_DESC_REQ: ble_write(addr, handle, data, len, 1); break;
    case BLE_NOTIFY_REQ: { PB(b, 24); pb_uint(&b, 1, addr); pb_uint(&b, 2, handle); send_msg(BLE_NOTIFY, &b); } break;     /* see BLE_FEATURES */
    }
}


static void ble_adverts(const struct ble_adv *a, int n)
{
    PB(b, BLE_BATCH * 64); PB(one, 64);
    for (int i = 0; i < n; i++) {
        one.n = 0;
        pb_uint(&one, 1, a[i].addr); pb_sint(&one, 2, a[i].rssi); pb_uint(&one, 3, a[i].addr_type); pb_bytes(&one, 4, a[i].data, a[i].len);
        pb_bytes(&b, 1, one.p, one.n);
    }
    pthread_mutex_lock(&core_lock);
    send_ble(BLE_RAW_ADV, &b);
    pthread_mutex_unlock(&core_lock);
}

static void ble_pair_result(unsigned type, uint64_t addr, int ok, int error)
{
    PB(b, 32);
    pb_uint(&b, 1, addr); pb_uint(&b, 2, ok); pb_uint(&b, 3, error);
    pthread_mutex_lock(&core_lock); send_gatt(type, &b); pthread_mutex_unlock(&core_lock);
}
static void ble_paired_cb(uint64_t a, int ok, int e) { ble_pair_result(BLE_PAIRED, a, ok, e); }
static void ble_unpaired_cb(uint64_t a, int ok, int e) { ble_pair_result(BLE_UNPAIRED, a, ok, e); }

/* Bluetooth speaker: the pairing window opened, ran out or ended with a paired device */
static void bt_changed(void)
{
    pthread_mutex_lock(&core_lock);
    send_setting(KEY_BT_PAIRING); send_setting(KEY_BT_OUT_SEARCH); send_setting(KEY_BT_OUT); send_setting(KEY_BT_OUT_STATUS);
    pthread_mutex_unlock(&core_lock);
}
static void adb_changed(void) { pthread_mutex_lock(&core_lock); send_setting(KEY_ADB_WIFI); pthread_mutex_unlock(&core_lock); }
static void wifi_changed(void) { pthread_mutex_lock(&core_lock); send_setting(KEY_WIFI_MOTION); pthread_mutex_unlock(&core_lock); }

static const struct ble_handler ble_handler = {
    .adverts = ble_adverts, .scan_changed = ble_changed, .slots_changed = ble_slots, .connection = ble_connection, .services = ble_db,
    .read = ble_read_cb, .written = ble_written_cb, .notify = ble_notify_cb, .error = ble_error_cb,
    .paired = ble_paired_cb, .unpaired = ble_unpaired_cb,
};

/* ---------------------------------------------------------------- core callbacks (lock held) */

static void pb_map(struct pb *b, int f, const char *k, const char *v)      /* HomeassistantServiceMap */
{
    PB(e, 400); pb_str(&e, 1, k); pb_str(&e, 2, v);
    pb_bytes(b, f, e.p, e.n);
}

/* The satellite entity is ours by MAC: Home Assistant keys the device on it, entity ids can be renamed.  The name
 * the phone chose goes in plain data, never into the template.
 * Home Assistant refuses an announcement while the satellite still plays one (SatelliteBusyError: a phone
 * reconnecting during "Disconnected from ..."), and still for a moment after our "finished", until its announce call
 * has unwound.  So one request at a time, latest wins: the next goes 1 s after the previous announcement ended, or
 * 15 s after its request if none came (Home Assistant dropped it), and not at all once 30 s old. */
static char bt_next[160];               /* lock held: waiting to be asked */
static long long bt_next_ms, bt_asked_ms, bt_free_ms;   /* lock held: queued at; last request out, no end yet (0: none); not before */

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void bt_ask(const char *msg)
{
    PB(b, 1024); char tpl[400], m[24]; size_t j = 0; int n = 0;
    for (const char *c = mac(); *c && j < sizeof m - 1; c++) m[j++] = tolower((unsigned char)*c);
    m[j] = 0;
    snprintf(tpl, sizeof tpl, "{%% for e in integration_entities('esphome') if e.startswith('assist_satellite.') and "
             "('mac', '%s') in (device_attr(e, 'connections') or []) %%}{{ e }}{%% endfor %%}", m);
    pb_str(&b, 1, "assist_satellite.announce");
    pb_map(&b, 2, "message", msg);
    pb_map(&b, 3, "entity_id", tpl);
    pb_map(&b, 3, "preannounce", "{{ false }}");    /* the chime already played here.  Plain data arrives as strings, and
                                                       the announce schema takes a real bool only: a template renders one */
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && clients[i].actions) { send_to(clients[i].fd, HA_ACTION, &b); n++; }
    if (n) bt_asked_ms = now_ms();
    fprintf(stderr, "bluetooth: \"%s\" %s\n", msg, n ? "asked of Home Assistant" : "not spoken: no client takes actions");
}

static void bt_try(void)                /* lock held */
{
    long long now = now_ms();
    if (!bt_next[0] || announcing || now < bt_free_ms || (bt_asked_ms && now - bt_asked_ms < 15000)) return;
    if (now - bt_next_ms < 30000) bt_ask(bt_next);
    bt_next[0] = 0;
}

static void *bt_thread(void *arg)
{
    (void)arg;
    for (;;) { usleep(200000); pthread_mutex_lock(&core_lock); bt_try(); pthread_mutex_unlock(&core_lock); }
    return NULL;
}

static void bt_device(const char *name, int on)
{
    static int started;
    if (!started) { pthread_t t; started = !pthread_create(&t, NULL, bt_thread, NULL); if (started) pthread_detach(t); }
    const struct bt_lang *l = settings_bt_lang();
    if (*name) snprintf(bt_next, sizeof bt_next, on ? l->on : l->off, name);
    else snprintf(bt_next, sizeof bt_next, "%s", on ? l->on_any : l->off_any);
    bt_next_ms = now_ms();
    bt_try();
    if (bt_next[0]) fprintf(stderr, "bluetooth: \"%s\" waits for the announcement before\n", bt_next);
}

static void announce_done(void) { bt_asked_ms = 0; bt_free_ms = now_ms() + 1000; }

static void start(void)
{
    PB(b, 96);
    tts_expected = 0; tts_url[0] = 0; if (cancelled) cancelled = 2;
    pb_uint(&b, 1, 1);                                  /* start */
    pb_uint(&b, 3, core_local_wake ? 1 : 3);            /* flags: USE_VAD, plus USE_WAKE_WORD when detection is remote */
    /* audio settings: neutral.  Home Assistant ignores them today; the gain is applied here (micgain.h), and absent they
     * would read as volume multiplier 0 should it ever start using them */
    { PB(as, 8); pb_float(&as, 3, 1.0f); pb_varint(&b, 4 << 3 | 2); pb_varint(&b, as.n); pb_raw(&b, as.p, as.n); }
    /* the wake word heard: Home Assistant's duplicate check keys on it, and it runs the assistant of the select showing
     * it ("Wake word 2" -> "Assistant 2") */
    if (core_local_wake && *core_wake_phrase()) pb_str(&b, 5, core_wake_phrase());
    send_va(VA_REQUEST, &b);
}

static void audio(const void *pcm, size_t len)
{
    static unsigned char buf[4096 + 16];
    for (size_t off = 0; off < len; off += 4096) {
        struct pb b = { buf, 0, sizeof buf };
        pb_bytes(&b, 1, (const char *)pcm + off, len - off < 4096 ? len - off : 4096);
        send_out(va_fd, VA_AUDIO, &b, 1);              /* a client that falls behind loses audio, never a state */
    }
}

static void stop(void)
{
    PB(a, 8); pb_uint(&a, 2, 1); send_va(VA_AUDIO, &a);        /* end of audio */
    send_va(VA_REQUEST, NULL);                                 /* start = false */
}

static void cancel(void)
{
    send_va(VA_REQUEST, NULL);                                 /* start = false: abort */
    cancelled = 1;
    /* A streamed reply can be on its way already (tool calls can follow its first words).  The fetch may be waiting on
     * Home Assistant: cut its socket, and let go of it here, or the job would hold media_busy until it notices and
     * refuse the next run's reply (the gap is enough when that comes at once: seen in CI) */
    if (tts_expected && media_cur) {
        media_cur->aborted = 1;
        if (media_cur->fd >= 0) shutdown(media_cur->fd, SHUT_RDWR);
        media_cur = NULL;                                     /* the thread frees it; the slot is free now, not when it notices */
        media_playing = 0; send_mp_state();
        atomic_store(&media_busy, 0);
    }
}

static void played(void)
{
    if (announcing) { PB(b, 8); pb_uint(&b, 1, 1); send_va(VA_ANNOUNCE_FINISHED, &b); announcing = 0; announce_done(); }
    if (media_playing) { media_playing = 0; send_mp_state(); }
    atomic_store(&media_busy, 0);
    tts_expected = 0;
}

static void volume_changed(int percent) { (void)percent; send_mp_state(); }
static void mute_changed(int muted) { (void)muted; send_setting(KEY_MUTE); settings_save(); }

/* ---------------------------------------------------------------- requests from Home Assistant */

static void wake_event(const char *event);
static void on_event(const unsigned char *p, const unsigned char *end)
{
    struct pbf f, g; unsigned type = 0; char key[48], val[1024], text[512] = "", code[48] = ""; int cont = 0, stream_now = 0;
    while (pb_next(&p, end, &f)) {
        if (f.field == 1) type = f.v;
        else if (f.field == 2 && f.wire == 2) {
            const unsigned char *q = f.data; key[0] = val[0] = 0;
            while (pb_next(&q, f.data + f.len, &g)) { if (g.field == 1 && g.data) pbf_str(&g, key, sizeof key); else if (g.field == 2 && g.data) pbf_str(&g, val, sizeof val); }
            if (!strcmp(key, "text") || !strcmp(key, "message") || !strcmp(key, "code")) snprintf(text + strlen(text), sizeof text - strlen(text), "%s ", val);
            if (!strcmp(key, "code")) snprintf(code, sizeof code, "%s", val);
            if (!strcmp(key, "continue_conversation") && !strcmp(val, "1")) cont = 1;
            if (!strcmp(key, "tts_start_streaming") && !strcmp(val, "1")) stream_now = 1;
            if (!strcmp(key, "url")) snprintf(tts_url, sizeof tts_url, "%s", val);
        }
    }
    /* Home Assistant may still report the aborted run (what was on the way: intent, TTS, end).  An error after a new
     * start is the new run's (one refused before it starts has no RUN_START) */
    if (cancelled == 1 || (cancelled == 2 && type != EV_RUN_START && type != EV_ERROR)) return;
    if (type == EV_RUN_START || type == EV_ERROR) cancelled = 0;
    switch (type) {
    case EV_WAKE_END:   core_remote_wake(); break;          /* the wake word was heard in Home Assistant (-w remote) */
    case EV_STT_START:  core_set_state(LISTENING); break;
    case EV_VAD_END:    core_mic_off(); if (core_state() == LISTENING) core_set_state(THINKING); break;
    case EV_STT_END:    fprintf(stderr, "transcript: %s\n", text);
        if (core_transcript(text)) break;                   /* "stop" to a ring or music: the run is cancelled */
        core_mic_off(); if (core_state() == LISTENING) core_set_state(THINKING); break;
    case EV_INTENT_END: if (cont) core_restart_after(); break;
    case EV_TTS_START:  fprintf(stderr, "reply: %s\n", text); break;
    case EV_INTENT_PROGRESS: if (!stream_now) break;    /* streaming TTS: the URL from RUN_START can be fetched already */
        /* fall through */
    case EV_TTS_END:
        if (!tts_expected && tts_url[0]) { tts_expected = 1; core_mic_off(); media_start("", tts_url, 1, 0, 1, 0); }
        break;
    case EV_RUN_END:    if (!tts_expected && core_state() != SPEAKING) core_pipeline_finish(); break;
    case EV_ERROR:
        tts_expected = 0;
        /* Another satellite reported the same wake word first (Home Assistant takes the first per phrase in 2 s): not
         * this Echo's turn, and nothing went wrong, so no error ring */
        if (!strcmp(code, "duplicate_wake_up_detected")) {
            fprintf(stderr, "pipeline: another satellite answers\n"); wake_event("answered_elsewhere"); core_pipeline_finish(); break;
        }
        /* nothing said: no failure to show (core_no_speech) */
        if (!strcmp(code, "stt-no-text-recognized")) { fprintf(stderr, "pipeline: nothing said\n"); core_no_speech(); break; }
        fprintf(stderr, "pipeline error: %s\n", text); core_error(); break;
    }
}

static void on_timer(const unsigned char *p, const unsigned char *end)
{
    struct pbf f; unsigned type = 0, total = 0; char tname[64] = "", id[40] = "";
    while (pb_next(&p, end, &f)) {
        if (f.field == 1) type = f.v; else if (f.field == 2 && f.data) pbf_str(&f, id, sizeof id);
        else if (f.field == 3 && f.data) pbf_str(&f, tname, sizeof tname); else if (f.field == 4) total = f.v;
    }
    static const char *const names[] = { "started", "updated", "cancelled", "finished" };
    fprintf(stderr, "timer %s: %s (%u s)\n", type < 4 ? names[type] : "?", tname, total);
    if (type == 3) core_timer_finished(id, tname, total); else if (type == 2) core_timer_cancelled(id);
}

static void on_announce(const unsigned char *p, const unsigned char *end)
{
    struct pbf f; static char media[1024], pre[1024]; int conv = 0;
    media[0] = pre[0] = 0;
    while (pb_next(&p, end, &f)) {
        if (f.field == 1 && f.data) pbf_str(&f, media, sizeof media);
        else if (f.field == 3 && f.data) pbf_str(&f, pre, sizeof pre);
        else if (f.field == 4) conv = f.v != 0;
    }
    /* Do not disturb drops announcements (reported as not played), but not the one we asked for ourselves: a Bluetooth
     * connection message answers something the user just did, as stock's does. */
    if (core_dnd(-1) && !bt_asked_ms) { fprintf(stderr, "media: do not disturb, dropped announcement %s\n", media); PB(b, 8); pb_uint(&b, 1, 0); send_va(VA_ANNOUNCE_FINISHED, &b); return; }
    media_start(pre, media, 1, conv, 0, 0);
}

static void on_mp_command(const unsigned char *p, const unsigned char *end)
{
    struct pbf f; static char url[1024]; int has_cmd = 0, cmd = 0, has_vol = 0, has_url = 0, announcement = 0; float vol = 0;
    url[0] = 0;
    while (pb_next(&p, end, &f)) switch (f.field) {
        case 2: has_cmd = f.v != 0; break;
        case 3: cmd = f.v; break;
        case 4: has_vol = f.v != 0; break;
        case 5: { uint32_t u = f.v; memcpy(&vol, &u, 4); } break;
        case 6: has_url = f.v != 0; break;
        case 7: if (f.data) pbf_str(&f, url, sizeof url); break;
        case 9: announcement = f.v != 0; break;        /* tts.speak and play_media's announce: speech, not music */
    }
    if (has_vol) core_set_volume((int)(vol * 100 + 0.5f));
    /* the one media player for everything the Echo plays: pause and stop pause the music (Sendspin, Bluetooth; AirPlay's sender is dropped, it has no
     * pause to ask for), play resumes what that paused */
    if (has_cmd && (cmd == MP_CMD_STOP || cmd == MP_CMD_PAUSE)) core_music_pause();
    if (has_cmd && cmd == MP_CMD_PLAY && !has_url) core_music_resume();
    if (has_cmd && cmd == MP_CMD_STOP && media_playing) core_tts_flush();
    if (has_cmd && cmd == MP_CMD_STOP) core_alarm_stop("media player stop");     /* as "Stop ringing" does, for automations */
    if (has_url && url[0]) media_start("", url, 0, 0, 0, !announcement);
    if (has_cmd && !has_url) send_mp_state();
}

static void send_device_info(void)
{
    PB(b, 512);
    pb_str(&b, 2, node_name()); pb_str(&b, 3, mac()); pb_str(&b, 4, "2025.5.0"); pb_str(&b, 5, BUILD_TIME);
    /* With a project name (8) Home Assistant ignores model (6) and manufacturer (12): it shows the project name's two
     * halves ("hassmic", "echo-dot-2") and project version (9) as firmware.  Match hassmic Echos by entities, not these */
    pb_str(&b, 6, board.model); pb_str(&b, 8, board.project); pb_str(&b, 9, VERSION);
    pb_str(&b, 12, "Amazon"); pb_str(&b, 13, core_name);
    if (core_web_port) pb_uint(&b, 10, core_web_port); /* webserver_port: HA links the device page to http://<host>:<it> */
    if (ble_present()) { pb_uint(&b, 11, 5); pb_uint(&b, 15, BLE_FEATURES); pb_str(&b, 18, ble_mac()); }    /* 11: legacy "active connections" */
    pb_uint(&b, 17, FEAT_VOICE | FEAT_API_AUDIO | FEAT_TIMERS | FEAT_ANNOUNCE | FEAT_START_CONVERSATION);      /* no SPEAKER: see top */
    pb_uint(&b, 19, 1);                                 /* api_encryption_supported */
    pb_uint(&b, 26, !have_key);                         /* api_encryption_provisionable: Home Assistant sets the key */
    send_msg(DEVICE_INFO_RESP, &b);
}

static void send_entities(void)
{
    PB(b, 256); PB(fmt, 48);
    pb_str(&b, 1, "media_player"); pb_fixed32(&b, 2, MP_KEY); pb_str(&b, 3, "Media Player");
    pb_uint(&b, 8, 1);                                  /* supports_pause: Home Assistant's "pause the music" needs it; for AirPlay it drops the sender */
    for (int purpose = 0; purpose < 2; purpose++) {     /* 0 = default, 1 = announcement: Home Assistant transcodes to this */
        fmt.n = 0;
        pb_str(&fmt, 1, "wav"); pb_uint(&fmt, 2, MEDIA_RATE); pb_uint(&fmt, 3, 1); pb_uint(&fmt, 4, purpose); pb_uint(&fmt, 5, 2);
        pb_varint(&b, 9 << 3 | 2); pb_varint(&b, fmt.n); pb_raw(&b, fmt.p, fmt.n);      /* purpose 0 encodes as absent: fine */
    }
    send_msg(LIST_MEDIA_PLAYER, &b);
    send_setting_entities();
    send_msg(LIST_ENTITIES_DONE, NULL);
}

/* Every installed model (core.h); Home Assistant shows them in the satellite's wake word selects ("Wake word", "Wake
 * word 2"), and up to max_active_wake_words of them listen at once */
static void send_va_config(void)
{
    PB(b, 3072);
    if (core_local_wake) {
        const struct core_wake_word *w; int n = core_wake_words(&w), act[CORE_WAKE_ACTIVE_MAX], k = n ? core_wake_active(act) : 0;
        for (int i = 0; i < n; i++) {
            PB(e, 192); pb_str(&e, 1, w[i].id); pb_str(&e, 2, w[i].name); pb_str(&e, 3, w[i].lang);
            pb_varint(&b, 1 << 3 | 2); pb_varint(&b, e.n); pb_raw(&b, e.p, e.n);
        }
        for (int i = 0; i < k; i++) pb_str(&b, 2, w[act[i]].id);
        pb_uint(&b, 3, core_wake_max() > 1 ? core_wake_max() : 1);
    }
    send_msg(VA_CONFIG_RESP, &b);
}

/* Home Assistant's selects: the ids of the ones picked, in the order they were picked (not by select).  None ("No wake
 * word" in both): the Echo keeps what it has, it has no way to listen for none */
static void on_va_set_config(const unsigned char *p, const unsigned char *end)
{
    struct pbf f; char id[64]; const struct core_wake_word *w; int n = core_wake_words(&w), pick[CORE_WAKE_ACTIVE_MAX], k = 0;
    while (pb_next(&p, end, &f)) {
        if (f.field != 1 || !f.data) continue;
        pbf_str(&f, id, sizeof id);
        int i = 0;
        while (i < n && strcmp(w[i].id, id)) i++;
        if (i == n) fprintf(stderr, "wake word: Home Assistant picked unknown \"%s\", ignored\n", id);
        else if (k < CORE_WAKE_ACTIVE_MAX) pick[k++] = i;
        else fprintf(stderr, "wake word: Home Assistant picked more than %d, \"%s\" ignored\n", CORE_WAKE_ACTIVE_MAX, id);
    }
    if (k) core_wake_set(pick, k);
}

/* returns 0 to close the connection */
/* lock held: every setting's state, to a client that subscribed (and after the web page changed one) */
static void send_all_settings(void)
{
    send_setting(KEY_MIC_LEVEL); send_setting(KEY_DENOISE); send_setting(KEY_MUTE); send_setting(KEY_WAKE_SOUND); send_setting(KEY_BT_PAIRING); send_setting(KEY_BT_ANNOUNCE); send_setting(KEY_DND); send_setting(KEY_SOUND_DETECTION); send_setting(KEY_AIRPLAY);
        send_setting(KEY_BT_LANG); send_setting(KEY_BT_AAC); send_setting(KEY_BT_SECURE); send_setting(KEY_IMPROV);
        for (int k = KEY_EQ_BASS; k <= KEY_EQ_TREBLE; k++) send_setting(k);
        for (int k = KEY_ARB_JOIN; k <= KEY_ARB_PEERS; k++) send_setting(k);
        send_setting(KEY_SS_UNPAIRED); send_setting(KEY_ADB_WIFI);
        send_setting(KEY_BT_OUT_SEARCH); send_setting(KEY_BT_OUT); send_setting(KEY_BT_OUT_STATUS); send_setting(KEY_BT_OUT_DELAY);
        for (int k = KEY_WIFI_MOTION_ON; k <= KEY_WIFI_MOTION_SENS; k++) send_setting(k);
        send_setting(KEY_FIRMWARE);
        send_setting(KEY_TIMER_NAMES); send_setting(KEY_TIMER_RINGING);
        send_setting(KEY_DROPIN_PEER); send_setting(KEY_DROPIN_STATE);
        send_setting(KEY_ASSIST_VOLUME); send_setting(KEY_SOUNDS_VOLUME); send_setting(KEY_TIMER_VOLUME); send_setting(KEY_CALL_VOLUME);
        send_light_states();
    send_alarm_states();
}

static void settings_changed(void) { send_all_settings(); }      /* lock held: a setting changed (web page, settings.c) */

/* lock held: a feature went on or off.  ESPHome has no message for a changed entity list: Home Assistant reads it at
 * each connect, and removes what is no longer there (registry included).  So the links close; it comes back in seconds. */
static void entities_changed(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0) shutdown(clients[i].fd, SHUT_RDWR);
    fprintf(stderr, "entities changed: links closed, Home Assistant lists them again\n");
}

/* lock held.  The time, from Home Assistant (clock.c): asked on each link with the device key once it subscribes, and
 * again on its pings every 6 h (it pings every 20 s or so).  Only that link: the clock decides which certificates
 * curl takes, so nobody else on the network may move it */
#define TIME_ASK_MS (6LL * 3600 * 1000)
static long long time_asked;
static void ask_time(int c, int force)
{
    static int told;
    if (c < 0 || !clients[c].keyed) {
        if (!have_key && !told++) fprintf(stderr, "clock: not asking Home Assistant for the time: it has not given this Echo an encryption key\n");
        return;
    }
    if (!force && time_asked && now_ms() - time_asked < TIME_ASK_MS) return;
    time_asked = now_ms();
    send_msg(GET_TIME_REQ, NULL);
}

static int handle(unsigned type, const unsigned char *p, size_t len)
{
    const unsigned char *end = p + len; struct pbf f; int keep = 1;
    pthread_mutex_lock(&core_lock);
    int c = client_of(reply_fd);
    if (c < 0) { pthread_mutex_unlock(&core_lock); return 0; }          /* pushed out for a newer connection (serve) */
    if (have_key && !clients[c].keyed && type != DISCONNECT_REQ) {
        /* got in before the key was set (key_set() closes those; this catches what was on its way): the key counts
         * from here on, for every message, TTS audio included */
        fprintf(stderr, "encryption: closing a connection without the key\n");
        pthread_mutex_unlock(&core_lock);
        return 0;
    }
    if (type == VA_AUDIO) {                             /* TTS stream: queued outside the lock, from the voice assistant client only */
        int ours = reply_fd == va_fd;
        pthread_mutex_unlock(&core_lock);
        if (ours) while (pb_next(&p, end, &f)) if (f.field == 1 && f.data) core_tts_data(f.data, f.len);
        return 1;
    }
    switch (type) {
    case HELLO_REQ: { PB(b, 128); pb_uint(&b, 1, 1); pb_uint(&b, 2, 10); pb_str(&b, 3, "hassmic " VERSION); pb_str(&b, 4, node_name()); send_msg(HELLO_RESP, &b); } break;
    case CONNECT_REQ:      send_msg(CONNECT_RESP, NULL); break;                 /* no password */
    case DISCONNECT_REQ:   send_msg(DISCONNECT_RESP, NULL); keep = 0; break;
    case PING_REQ:         send_msg(PING_RESP, NULL); ask_time(c, 0); break;
    case DEVICE_INFO_REQ:  send_device_info(); break;
    case LIST_ENTITIES_REQ: send_entities(); break;
    case SUBSCRIBE_STATES:
        for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd == reply_fd) clients[i].states = 1;
        send_mp_state(); send_ap_meta(); send_all_settings(); send_token_state(); send_web_state();
        ask_time(c, 1); break;
    case GET_TIME_RESP: {
        uint32_t epoch = 0; char tz[128] = "";
        while (pb_next(&p, end, &f)) {
            if (f.field == 1 && f.wire == 5) epoch = (uint32_t)f.v;
            else if (f.field == 2 && f.wire == 2) pbf_str(&f, tz, sizeof tz);
        }
        if (c >= 0 && clients[c].keyed && epoch) {
            clock_from_ha(epoch); clock_zone_from_ha(tz);
            if (epoch >= 1704067200) { alarms_time(epoch, tz); send_setting(KEY_ALARM_NEXT); }     /* the alarm clock keeps its own offset to the boot clock */
        }
    } break;
    case SELECT_COMMAND: case NUMBER_COMMAND: case SWITCH_COMMAND: case TIME_COMMAND: on_setting(type, p, end); break;
    case LIGHT_COMMAND: on_light(p, end); break;
    case BUTTON_COMMAND: { struct pbf f;
        while (pb_next(&p, end, &f)) if (f.field == 1 && f.v == KEY_IDENTIFY) core_identify(); else if (f.field == 1 && f.v == KEY_RESET) core_reset("Home Assistant's button");
                                       else if (f.field == 1 && f.v == KEY_RESTART) core_reboot("Home Assistant's button");
            else if (f.field == 1 && f.v == KEY_TIMER_STOP) core_alarm_stop("Home Assistant's button");
            else if (f.field == 1 && (f.v == KEY_ALARM_STOP || f.v == KEY_ALARM_SNOOZE)) on_alarm_command(BUTTON_COMMAND, (unsigned)f.v, 0, "", p, end);
            else if (f.field == 1 && f.v == KEY_DROPIN_END && listed(KEY_DROPIN_END)) dropin_hangup("Home Assistant's button");
        break; }
    case SUBSCRIBE_HA_ACTIONS: if (c >= 0) clients[c].actions = 1; break;
    case SUBSCRIBE_HA_STATES: if (c >= 0) clients[c].ha_states = 1; break;
    case HA_STATE: {
        /* a state we asked for (arb_request): only from Home Assistant itself, the client holding the device key */
        char ent[128] = "", st[300] = "", attr[64] = "";
        while (pb_next(&p, end, &f)) if (f.data) {
            if (f.field == 1) pbf_str(&f, ent, sizeof ent);
            else if (f.field == 2) pbf_str(&f, st, sizeof st);
            else if (f.field == 3) pbf_str(&f, attr, sizeof attr);
        }
        if (attr[0] || !arb_running() || c < 0 || !clients[c].keyed) break;
        arb_ha_state(ent, st);
    } break;
    case EXECUTE_SERVICE: {
        unsigned key = 0; int argn = 0, nc = 0; char args[2][256] = { "", "" }, colors[RING_LEDS_MAX][16]; struct pbf g;
        while (pb_next(&p, end, &f)) {
            if (f.field == 1) key = (unsigned)f.v;
            else if (f.field == 2 && f.data && argn < 2) {
                const unsigned char *q = f.data;
                while (pb_next(&q, f.data + f.len, &g)) {
                    if (g.field == 4 && g.data) pbf_str(&g, args[argn], sizeof args[argn]);
                    else if (g.field == 9 && g.data && nc < RING_LEDS_MAX) pbf_str(&g, colors[nc++], sizeof colors[0]);   /* string array */
                }
                argn++;
            }
        }
        if (key == KEY_RING_LEDS && listed(KEY_RING_LEDS)) { ring_leds_from(colors, nc); break; }
        if (key == KEY_KILL_SERVICE) { on_kill(c >= 0 && clients[c].keyed, atoi(args[0]), args[1]); break; }
        /* Drop In from Home Assistant (an automation, the voice blueprint): it may open another Echo's microphone, so
         * only over the keyed link too */
        if (key == KEY_DROPIN_SERVICE && listed(KEY_DROPIN_SERVICE)) {
            char e[120];
            if (c < 0 || !clients[c].keyed) fprintf(stderr, "drop in: refused, the action did not come over the keyed connection\n");
            else if (dropin_call(args[0], e, sizeof e)) fprintf(stderr, "drop in: %s: %s\n", args[0], e);
            break;
        }
        /* the network key only from Home Assistant itself: a client holding the device key (without one, anyone on the
         * network could connect and hand this Echo a network) */
        if (key != KEY_ARB_SERVICE || !arb_running()) break;
        if (c < 0 || !clients[c].keyed) { fprintf(stderr, "arbitration: key refused, it did not come over the keyed connection\n"); break; }
        arb_key(args[0], args[1]);
    } break;
    case SUBSCRIBE_VA: {
        int sub = 0;
        while (pb_next(&p, end, &f)) if (f.field == 1) sub = f.v != 0;
        if (sub && va_fd >= 0 && va_fd != reply_fd) { fprintf(stderr, "voice assistant: a second client tried to subscribe, ignored\n"); break; }
        if (!sub && va_fd != reply_fd) break;
        fprintf(stderr, "voice assistant: %s\n", sub ? "subscribed" : "unsubscribed");
        va_fd = sub ? reply_fd : -1;
        if (!sub) tts_expected = 0;
        core_link(1, sub);
    } break;
    case VA_RESPONSE:
        while (pb_next(&p, end, &f)) if (f.field == 2 && f.v) { fprintf(stderr, "pipeline refused\n"); core_error(); }
        break;
    case VA_EVENT:         on_event(p, end); break;
    case VA_TIMER_EVENT:   on_timer(p, end); break;
    case VA_ANNOUNCE:      on_announce(p, end); break;
    case VA_CONFIG_REQ:    send_va_config(); break;
    case VA_SET_CONFIG:    if (core_local_wake) on_va_set_config(p, end); break;
    case MEDIA_PLAYER_COMMAND: on_mp_command(p, end); break;
    case BLE_SUBSCRIBE: case BLE_UNSUBSCRIBE:
        if (c < 0 || !ble_present()) break;
        clients[c].ble = type == BLE_SUBSCRIBE;         /* flags ignored: raw advertisements are all we send */
        ble_update(); if (type == BLE_SUBSCRIBE) send_scanner_state();
        break;
    case BLE_SCANNER_SET_MODE:
        ble_mode = BLE_PASSIVE;                         /* the zero value: an empty message */
        while (pb_next(&p, end, &f)) if (f.field == 1 && f.v == BLE_ACTIVE) ble_mode = BLE_ACTIVE;
        ble_update(); send_scanner_state();
        break;
    case BLE_CONN_FREE_REQ: if (c >= 0 && ble_present()) { clients[c].ble_free = 1; send_slots(1); } break;
    case BLE_DEVICE_REQ: case BLE_SERVICES_REQ: case BLE_READ_REQ: case BLE_READ_DESC_REQ: case BLE_WRITE_REQ:
    case BLE_WRITE_DESC_REQ: case BLE_NOTIFY_REQ:
        if (ble_present() && c >= 0) { clients[c].ble_user = 1; on_ble_request(type, p, end); }
        break;
    case SET_KEY_REQ: {
        /* Without a key: from an encrypted (zero-PSK) connection only, so the key never crosses the network readable.
         * With one: only from a connection that has it (Home Assistant rotating or clearing it). */
        const unsigned char *k = NULL; size_t kl = 0; int ok = 0;
        while (pb_next(&p, end, &f)) if (f.field == 1 && f.data) { k = f.data; kl = f.len; }
        if (c >= 0 && (have_key ? clients[c].keyed : clients[c].enc)) {
            ok = key_set(k, kl);
            if (ok && have_key) {
                /* Everyone else who got in without it goes now, not on their next request: until then they were still
                 * sent every state (the Sendspin pairing token among them) */
                clients[c].keyed = 1;
                for (int i = 0; i < MAX_CLIENTS; i++) if (i != c && clients[i].fd >= 0 && !clients[i].keyed) {
                    shutdown(clients[i].fd, SHUT_RDWR); clients[i].states = clients[i].actions = 0;
                    fprintf(stderr, "encryption: closing a connection without the key\n");
                }
            }
        }
        else fprintf(stderr, "encryption: key change refused (%s connection)\n", have_key ? "unkeyed" : "plaintext");
        PB(b, 8); pb_uint(&b, 1, ok); send_msg(SET_KEY_RESP, &b);
    } break;
    }
    pthread_mutex_unlock(&core_lock);
    return keep;
}

static int read_varint(int fd, uint32_t *out)
{
    uint32_t v = 0; unsigned char c;
    for (int sh = 0; sh < 35; sh += 7) { if (read_full(fd, &c, 1) != 1) return -1; v |= (uint32_t)(c & 0x7f) << sh; if (!(c & 0x80)) { *out = v; return 0; } }
    return -1;
}

/* Body of a Noise frame whose 0x01 has been read.  buf holds 64 KiB, the most a frame can carry.  -1 on error. */
static long read_frame(int fd, unsigned char *buf)
{
    unsigned char h[2];
    if (read_full(fd, h, 2) != 2) return -1;
    size_t len = (size_t)h[0] << 8 | h[1];
    return len && read_full(fd, buf, len) != (ssize_t)len ? -1 : (long)len;
}

/* Client hello (ignored), handshake message 1, our server hello and message 2.  With a key only that key gets through;
 * without one only the all-zero PSK, which is what Home Assistant uses to set the key.  0 = encrypted from here on. */
static int noise_handshake(int fd, int slot, struct outq *q, unsigned char *buf, struct noise_cs *rx)
{
    static const uint8_t zero[32]; uint8_t psk[32], payload[64], out[32 + NOISE_TAG + 1]; unsigned char pre;
    struct noise_hs hs; struct noise_cs tx; char hello[160]; int keyed; long n;
    if (read_frame(fd, buf) < 0 || read_full(fd, &pre, 1) != 1 || pre != 1 || (n = read_frame(fd, buf)) < 1 || buf[0] != 0) return -1;
    pthread_mutex_lock(&core_lock);
    keyed = have_key; memcpy(psk, have_key ? api_key : zero, 32);
    pthread_mutex_unlock(&core_lock);
    size_t hl = 1 + snprintf(hello + 1, sizeof hello - 1, "%s%c%s", node_name(), 0, mac_plain()) + 1;
    hello[0] = 1;                                       /* chosen protocol: Noise */
    if (put_frame(q, hello, hl) < 0) return -1;
    noise_nn_responder_init(&hs, "NoiseAPIInit\0\0", 14);
    if (n - 1 > 32 + NOISE_TAG + (long)sizeof payload || noise_nn_read_msg1(&hs, psk, buf + 1, n - 1, payload) < 0) {
        static const char fail[] = "\x01Handshake MAC failure";           /* what the client turns into "invalid key" */
        put_frame(q, fail, sizeof fail - 1);
        fprintf(stderr, "encryption: handshake with the wrong key (%s)\n", keyed ? "not the device key" : "no key set yet, expected the zero PSK");
        memset(psk, 0, sizeof psk);
        return -1;
    }
    memset(psk, 0, sizeof psk);
    out[0] = 0;                                         /* handshake OK */
    size_t ml = noise_nn_write_msg2(&hs, NULL, 0, out + 1, &tx, rx);
    pthread_mutex_lock(&core_lock);
    int rc = -1;
    if (clients[slot].fd == fd) {                       /* not pushed out meanwhile */
        clients[slot].tx = tx; clients[slot].enc = 1; clients[slot].keyed = keyed;
        rc = put_frame(q, out, ml + 1);                 /* under the lock: no state may overtake it */
    }
    pthread_mutex_unlock(&core_lock);
    memset(&tx, 0, sizeof tx);
    return rc;
}

static void clear_slot(int i)           /* lock held */
{
    clients[i].fd = -1;
    clients[i].states = clients[i].enc = clients[i].keyed = clients[i].ble = clients[i].ble_free = clients[i].ble_user = clients[i].actions = clients[i].ha_states = clients[i].talked = 0;
    memset(&clients[i].tx, 0, sizeof clients[i].tx); memset(&clients[i].rx, 0, sizeof clients[i].rx);
    clients[i].q = NULL;
}

/* lock held.  A slot for fd, or -1; *others: connections besides it.  Connections still silent after FIRST_MSG_S are let
 * go here, and with every slot taken the longest silent one makes room; failing that the oldest without the key that
 * is not the voice assistant's (no key set yet: anyone may connect and talk, so talking alone earns nothing).  The one
 * pushed out finds its slot gone (serve, handle) and ends. */
static int take_slot(int fd, int *others)
{
    long long now = net_mono_ms(); int slot = -1, v = -1, n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && !clients[i].talked && now - clients[i].since > FIRST_MSG_S * 1000LL) {
        fprintf(stderr, "client let go: nothing said in %d s\n", FIRST_MSG_S);
        shutdown(clients[i].fd, SHUT_RDWR); clear_slot(i);
    }
    for (int i = 0; i < MAX_CLIENTS; i++) { if (clients[i].fd < 0 && slot < 0) slot = i; n += clients[i].fd >= 0; }
    if (slot < 0) {
        for (int i = 0; i < MAX_CLIENTS; i++) if (!clients[i].talked && (v < 0 || clients[i].since < clients[v].since)) v = i;
        if (v < 0) for (int i = 0; i < MAX_CLIENTS; i++)
            if (!clients[i].keyed && clients[i].fd != va_fd && (v < 0 || clients[i].since < clients[v].since)) v = i;
        if (v >= 0) {
            fprintf(stderr, "client pushed out for a new connection (%s)\n", clients[v].talked ? "it has no key" : "it has said nothing");
            shutdown(clients[v].fd, SHUT_RDWR); clear_slot(v); slot = v; n--;
            ble_update();
        }
    }
    if (slot >= 0) { clear_slot(slot); clients[slot].fd = fd; clients[slot].since = now; if (!n) core_link(1, 0); }
    *others = n;
    return slot;
}

static void serve(int fd)
{
    unsigned char *buf = malloc(1 << 16), *pt = malloc(1 << 16), pre; uint32_t len, type, cap = 1 << 16; int slot = -1, n = 0, enc = 0;
    struct noise_cs rx;                 /* this reader's own: a slot can pass to a new connection while it still runs */
    struct outq *q = buf && pt ? outq_open(fd, OUTQ_LIMIT, OUTQ_BEHIND) : NULL;
    if (!q) { free(buf); free(pt); return; }
    pthread_mutex_lock(&core_lock);
    { static int loaded; if (!loaded) { loaded = 1; have_light = !isnan(core_lux()); settings_load(); key_load();
                                       pthread_t t;
                                       pthread_create(&t, NULL, light_thread, NULL); pthread_detach(t);
                                       adbwifi_start(adb_changed); wifimotion_start(wifi_changed); taskmgr_start(kill_changed); { pthread_t dt; if (!pthread_create(&dt, NULL, diag_thread, NULL)) pthread_detach(dt); }
                                       if (core_bluetooth(-1)) { ble_start(&ble_handler); a2dp_start(bt_changed); } } }
    slot = take_slot(fd, &n);
    if (slot >= 0) clients[slot].q = q;
    int keyed = have_key;
    pthread_mutex_unlock(&core_lock);
    if (slot < 0) { fprintf(stderr, "client refused: %d connections already\n", MAX_CLIENTS); outq_close(q); free(buf); free(pt); return; }
    reply_fd = fd;
    struct timeval tv = { FIRST_MSG_S, 0 };             /* until the first message: also covers the handshake */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    if (read_full(fd, &pre, 1) != 1) goto done;
    if (pre == 1) {
        if (noise_handshake(fd, slot, q, buf, &rx)) goto done;
        enc = 1;
    } else if (keyed) {                                 /* the client sees 0x01 and reports "requires encryption" */
        fprintf(stderr, "encryption: plaintext connection refused, the device has a key\n");
        put_frame(q, "", 0);
        goto done;
    }
    fprintf(stderr, "client connected (%d, %s)\n", n + 1, enc ? (keyed ? "encrypted" : "zero-PSK, to set the key") : "plaintext");

    for (int first = 1;; first = 0) {
        const unsigned char *msg = buf;
        if (enc) {
            long fl, pl;
            if (read_full(fd, &pre, 1) != 1 || pre != 1 || (fl = read_frame(fd, buf)) < 0) break;
            if ((pl = noise_decrypt(&rx, buf, fl, pt)) < 4) { fprintf(stderr, "encryption: bad frame, closing\n"); break; }
            type = pt[0] << 8 | pt[1]; len = pt[2] << 8 | pt[3];
            if (len != (uint32_t)pl - 4) break;
            msg = pt + 4;
        } else {
            if (!first && read_full(fd, &pre, 1) != 1) break;
            if (pre != 0) break;
            if (read_varint(fd, &len) || read_varint(fd, &type) || len > (1 << 20)) break;
            if (len > cap) { unsigned char *nb = realloc(buf, len); if (!nb) break; buf = nb; cap = len; msg = buf; }
            if (len && read_full(fd, buf, len) != (ssize_t)len) break;
        }
        if (!handle(type, msg, len)) break;
        if (first) {                                    /* it talks: from here on only keepalive judges an idle link */
            pthread_mutex_lock(&core_lock); if (clients[slot].fd == fd) clients[slot].talked = 1; pthread_mutex_unlock(&core_lock);
            tv.tv_sec = 0; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        }
    }

done:

    pthread_mutex_lock(&core_lock);
    if (clients[slot].fd == fd) clear_slot(slot);       /* else it was pushed out, and the slot is someone else's now */
    n = 0;
    ble_update();
    if (!ble_subscribers()) {                           /* nobody left to use them */
        int users = 0; uint64_t a[BLE_MAX_CONN];
        for (int i = 0; i < MAX_CLIENTS; i++) users += clients[i].fd >= 0 && (clients[i].ble_free || clients[i].ble_user);
        if (!users) for (int i = 0, k = ble_connections(a); i < k; i++) ble_disconnect(a[i]);
    }
    for (int i = 0; i < MAX_CLIENTS; i++) n += clients[i].fd >= 0;
    if (va_fd == fd) { va_fd = -1; tts_expected = 0; core_link(n > 0, 0); }      /* the voice assistant's client left: pipelines end */
    else if (!n) core_link(0, 0);
    pthread_mutex_unlock(&core_lock);
    outq_close(q);                                      /* out of the slot: nothing queues for it any more */
    free(buf); free(pt);
    fprintf(stderr, "client disconnected (%d left)\n", n);
}

/* lock held: another Echo's "arbitration_key" action, run by Home Assistant (needs "Allow the device to perform Home
 * Assistant actions" on this one, or Home Assistant raises a repair instead) */
static int arb_send(const char *node, const char *network, const char *key)
{
    PB(b, 512); char svc[96]; int n = 0;
    snprintf(svc, sizeof svc, "esphome.%s_arbitration_key", node);
    for (char *c = svc; *c; c++) if (*c == '-') *c = '_';
    pb_str(&b, 1, svc); pb_map(&b, 2, "network", network); pb_map(&b, 2, "key", key);
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && clients[i].actions) { send_to(clients[i].fd, HA_ACTION, &b); n++; }
    return n ? 0 : -1;
}

static void alarms_changed(int rang)    /* lock held */
{
    send_alarm_states();
    if (rang >= 0) { PB(b, 32); char t[24]; snprintf(t, sizeof t, "alarm_%d", rang + 1); pb_fixed32(&b, 1, KEY_ALARM_EVENT); pb_str(&b, 2, t); send_state(EVENT, &b); }
}

static void arb_changed(void) { for (int k = KEY_ARB_JOIN; k <= KEY_ARB_PEERS; k++) send_setting(k); }

/* lock held: our arbitration tag as scanned (arb.c).  An event, which HA fires for any device it adopted, "perform
 * actions" or not (esphome manager: is_event before the permission check); only to Home Assistant itself */
static int arb_scan(const char *tag_id)
{
    PB(b, 160); int n = 0;
    pb_str(&b, 1, "esphome.tag_scanned"); pb_map(&b, 2, "tag_id", tag_id); pb_uint(&b, 5, 1);
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && clients[i].actions && clients[i].keyed) { send_to(clients[i].fd, HA_ACTION, &b); n++; }
    return n ? 0 : -1;
}

/* lock held: ask Home Assistant once for an entity's state (needs no permission: HA answers any device) */
static int arb_request(const char *entity)
{
    PB(b, 160); int n = 0;
    pb_str(&b, 1, entity); pb_uint(&b, 3, 1);           /* once: no lasting subscription to pile up in HA */
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && clients[i].ha_states && clients[i].keyed) { send_to(clients[i].fd, HA_STATE_SUB, &b); n++; }
    return n ? 0 : -1;
}

static void sound(const char *event)    /* lock held */
{
    PB(b, 64); pb_fixed32(&b, 1, KEY_SOUND); pb_str(&b, 2, event);
    if (listed(KEY_SOUND)) send_state(EVENT, &b);
    fprintf(stderr, "sound: %s\n", event);
}

static void button(const char *event)   /* lock held */
{
    PB(b, 64); pb_fixed32(&b, 1, KEY_BUTTON); pb_str(&b, 2, event);
    send_state(EVENT, &b);
}

static void wake_event(const char *event)       /* lock held */
{
    PB(b, 64); pb_fixed32(&b, 1, KEY_WAKE); pb_str(&b, 2, event);
    if (listed(KEY_WAKE)) { send_state(EVENT, &b); fprintf(stderr, "wake event: %s\n", event); }
}

static void timers_changed(void) { send_setting(KEY_TIMER_NAMES); send_setting(KEY_TIMER_RINGING); }   /* lock held: names first */
/* lock held: whom first.  Each state as dropin.c's loop passed it, not the one it is in now: a refusal that came at once
 * still shows calling, then idle */
static void dropin_changed(int st, const char *peer)
{
    PB(t, 128);
    pb_fixed32(&t, 1, KEY_DROPIN_PEER); pb_str(&t, 2, peer);
    if (listed(KEY_DROPIN_PEER)) send_state(TEXT_SENSOR_STATE, &t);
    t.n = 0; pb_fixed32(&t, 1, KEY_DROPIN_STATE); pb_str(&t, 2, dropin_states[st]);
    if (listed(KEY_DROPIN_STATE)) send_state(TEXT_SENSOR_STATE, &t);
}

static void dropin_result(const char *result)   /* lock held */
{
    PB(b, 64); pb_fixed32(&b, 1, KEY_DROPIN_RESULT); pb_str(&b, 2, result);
    if (listed(KEY_DROPIN_RESULT)) send_state(EVENT, &b);
}

const struct proto proto_esphome = { "esphome", 26053, 1, serve, start, audio, stop, cancel, played, volume_changed, mute_changed, print_mdns, bt_device,
                                     arb_send, arb_changed, sound, arb_request, settings_changed, entities_changed, arb_scan,
                                     timers_changed, dropin_changed, dropin_result, button, wake_event, alarms_changed };
