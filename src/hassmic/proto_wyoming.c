/* Wyoming satellite protocol: Home Assistant connects to us (wyoming integration). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include "board.h"
#include "core.h"
#include "outq.h"
#include "wyoming.h"

#define TTS_RATE 22050      /* assumed when audio-start carries no rate */

/* What waits for the client (outq.h): mic audio is left out past OUTQ_BEHIND (2 s of it), past OUTQ_LIMIT the client
 * is let go.  It is sent nothing big but audio. */
#define OUTQ_LIMIT  (512 * 1024)
#define OUTQ_BEHIND (64 * 1024)

static int client = -1;                 /* lock held */
static unsigned long long client_seq;   /* lock held: when it was accepted (core_conn_seq) */
static struct outq *client_q;           /* lock held: its outgoing queue, owned by its serve() */

/* lock held.  Only queued: the client's writer thread sends it, so a stalled link holds up nobody.  droppable: mic audio,
 * left out while the client is behind */
static void send_out(const char *type, const char *data, const void *payload, size_t len, int droppable)
{
    struct iovec iov[3]; char head[256];
    if (client < 0 || !client_q || (droppable && outq_behind(client_q, 256 + len))) return;
    if (outq_put(client_q, iov, wy_event_iov(iov, head, type, data, payload, len)) < 0)
        shutdown(client, SHUT_RDWR);        /* reader thread notices and cleans up */
}
static void send_event(const char *type, const char *data, const void *payload, size_t len) { send_out(type, data, payload, len, 0); }

static void start(void)
{
    char data[256];
    snprintf(data, sizeof data,
             "{\"start_stage\":\"%s\",\"end_stage\":\"tts\",\"restart_on_end\":false%s}",
             core_local_wake ? "asr" : "wake", core_local_wake ? ",\"wake_word_name\":\"alexa\"" : "");
    send_event("run-pipeline", data, NULL, 0);
}

static void audio(const void *pcm, size_t len)
{
    char data[96]; struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(data, sizeof data, "{\"rate\":16000,\"width\":2,\"channels\":1,\"timestamp\":%lld}",
             (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    send_out("audio-chunk", data, pcm, len, 1);
}

static void played(void) { send_event("played", NULL, NULL, 0); }

static void send_info(void)
{
    char data[1024];
    snprintf(data, sizeof data,
             "{\"asr\":[],\"tts\":[],\"handle\":[],\"intent\":[],\"wake\":[],\"mic\":[],\"snd\":[],"
             "\"satellite\":{\"name\":\"%s\",\"attribution\":{\"name\":\"hassmic\",\"url\":\"\"},"
             "\"installed\":true,\"description\":\"%s with Amazon audio front end\",\"version\":\"" VERSION "\","
             "\"area\":null,\"has_vad\":false,\"active_wake_words\":[%s],\"max_active_wake_words\":1,"
             "\"supports_trigger\":true}}",
             core_name, board.model, core_local_wake ? "\"alexa\"" : "");
    send_event("info", data, NULL, 0);
}

/* lock held: the client began a reply (audio-start) it has not ended yet */
static int stream_open;

/* A connection that goes away mid reply (replaced by a newer one, or dropped) never sends its audio-stop: without one
 * the mixer stream stays open and the wake word keeps the lower threshold it has while something plays.  Lock held. */
static void end_open_stream(void)
{
    if (!stream_open) return;
    core_tts_flush(); core_tts_end(); stream_open = 0;
}

static void handle(int fd, const struct wy_event *ev)
{
    const char *t = ev->type;
    pthread_mutex_lock(&core_lock);
    if (client != fd) { pthread_mutex_unlock(&core_lock); return; }     /* replaced by a newer connection: it is that one's */
    if (!strcmp(t, "audio-chunk")) { pthread_mutex_unlock(&core_lock); core_tts_data(ev->payload, ev->payload_len); return; }

    if (!strcmp(t, "ping")) send_event("pong", NULL, NULL, 0);
    else if (!strcmp(t, "describe")) send_info();
    else if (!strcmp(t, "run-satellite")) { fprintf(stderr, "satellite: running\n"); core_link(1, 1); }
    else if (!strcmp(t, "pause-satellite")) core_link(1, 0);
    else if (!strcmp(t, "detection")) core_set_state(LISTENING);
    else if (!strcmp(t, "voice-stopped") || !strcmp(t, "transcript")) {
        if (!strcmp(t, "transcript")) {
            char text[256] = "";
            wy_json_str(strchr(ev->json, '\n') ? strchr(ev->json, '\n') : ev->json, "text", text, sizeof text);
            fprintf(stderr, "transcript: %s\n", text);
            core_mic_off();
        }
        if (core_state() == LISTENING) core_set_state(THINKING);
    } else if (!strcmp(t, "audio-start")) {
        long rate = TTS_RATE, ch = 1, width = 2;
        wy_json_int(ev->json, "rate", &rate); wy_json_int(ev->json, "channels", &ch); wy_json_int(ev->json, "width", &width);
        if (width != 2) fprintf(stderr, "play: unsupported sample width %ld, expect noise\n", width);
        /* straight into MixerOpenPlay: the mixer takes what TTS engines send, 8-48 kHz mono or stereo, and nothing
         * else needs to reach it.  Refused, the chunks are dropped (no stream open) and audio-stop ends the reply. */
        if (rate < 8000 || rate > 48000 || ch < 1 || ch > 2) fprintf(stderr, "play: refused audio-start with %ld Hz x%ld\n", rate, ch);
        else { core_tts_begin(rate, ch, 0); stream_open = 1; }
    } else if (!strcmp(t, "audio-stop")) { core_tts_end(); stream_open = 0; }
    else if (!strcmp(t, "error")) {
        char code[64] = "";
        fprintf(stderr, "server error: %s\n", ev->json);
        wy_json_str(ev->json, "code", code, sizeof code);
        if (!strcmp(code, "stt-no-text-recognized")) core_no_speech(); else core_error();     /* nothing said: quietly */
    }
    pthread_mutex_unlock(&core_lock);
}

/* One client at a time, and the newest wins: Home Assistant reconnects after a restart or a network change, often
 * before the old connection is known to be dead, and anything else that connects (a scanner, a stray idle socket)
 * must not keep it out.  Served one per thread, so the new connection can push the old one out. */
static void serve(int fd)
{
    struct wy_reader rd; struct wy_event *ev = malloc(sizeof *ev); struct outq *q;
    if (!ev || wy_reader_init(&rd, fd) < 0) { free(ev); return; }
    /* The claim comes first and the queue (a thread) after it: a client that connects right behind a probe that closes at once must not be
     * pushed out by the probe's serve() being the slower of the two to get here */
    unsigned long long seq = core_conn_seq(fd);
    pthread_mutex_lock(&core_lock);
    if (client >= 0 && client_seq > seq) {              /* a connection accepted after this one has been served first: it is the newest */
        pthread_mutex_unlock(&core_lock);
        wy_reader_free(&rd); free(ev);
        return;
    }
    if (client >= 0) { fprintf(stderr, "client replaced by a new connection\n"); shutdown(client, SHUT_RDWR); end_open_stream(); }
    client = fd; client_seq = seq; client_q = NULL; core_link(1, 0);
    pthread_mutex_unlock(&core_lock);
    q = outq_open(fd, OUTQ_LIMIT, OUTQ_BEHIND);
    pthread_mutex_lock(&core_lock);
    if (q && client == fd) client_q = q;
    else if (!q && client == fd) { client = -1; core_link(0, 0); }       /* cannot serve it: no queue */
    pthread_mutex_unlock(&core_lock);
    if (!q) { wy_reader_free(&rd); free(ev); return; }
    fprintf(stderr, "client connected\n");

    while (wy_read(&rd, ev) > 0) handle(fd, ev);

    pthread_mutex_lock(&core_lock);
    int was = client == fd;
    if (was) { client = -1; client_q = NULL; end_open_stream(); core_link(0, 0); }
    pthread_mutex_unlock(&core_lock);
    outq_close(q);                          /* nothing queues for it any more: replaced, or gone just now */
    wy_reader_free(&rd); free(ev);
    if (was) fprintf(stderr, "client disconnected\n");
}

static void print_mdns(void)
{
    printf("<?xml version=\"1.0\" standalone='no'?>\n<!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">\n"
           "<service-group>\n  <name replace-wildcards=\"yes\">%s %%h</name>\n"
           "  <service><type>_wyoming._tcp</type><port>%d</port></service>\n</service-group>\n", core_name, core_port);
}

const struct proto proto_wyoming = { "wyoming", 16700, 1, serve, start, audio, NULL, NULL, played, NULL, NULL, print_mdns, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
