/* The Bluetooth speaker's codec glue (a2dp_codecs.c): a 1 kHz stereo tone encoded the way sources send it (aptX raw,
 * aptX HD and Opus in RTP, packets as PipeWire builds them), decoded through a2dp_codecs[], must come out as the same
 * tone at the same level.  AAC needs the firmware's FFmpeg 4 and is tested on the device. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "a2dp_codec.h"
#include "../third_party/freeaptx.h"

typedef struct OpusEncoder OpusEncoder;
OpusEncoder *opus_encoder_create(int32_t Fs, int channels, int application, int *error);
int opus_encode(OpusEncoder *st, const int16_t *pcm, int frame_size, unsigned char *data, int32_t max_data_bytes);

#define RATE 48000
#define SECONDS 2
static int16_t out[RATE * SECONDS * 2 + 8192]; static size_t nout; static unsigned out_rate, out_ch;

static void sink(const int16_t *pcm, unsigned frames, unsigned ch, unsigned rate)
{
    out_rate = rate; out_ch = ch;
    for (unsigned i = 0; i < frames && nout < RATE * SECONDS + 4096; i++, nout++) { out[2 * nout] = pcm[i * ch]; out[2 * nout + 1] = pcm[i * ch + ch - 1]; }
}

static const struct a2dp_codec *find(const char *name) { for (int i = 0; i < a2dp_ncodecs; i++) if (!strcmp(a2dp_codecs[i].name, name)) return &a2dp_codecs[i]; return NULL; }
static double tone(int i) { return 12000 * sin(2 * M_PI * 1000 * i / RATE); }

static int verdict(const char *name)
{
    double e = 0, p; size_t skip = 4800;                    /* codec delay and start-up */
    for (size_t i = skip; i < nout; i++) e += (double)out[2 * i] * out[2 * i];
    double rms = nout > skip ? sqrt(e / (nout - skip)) : 0, want = 12000 / sqrt(2);
    /* 1 kHz: correlate with sine and cosine */
    double si = 0, co = 0; for (size_t i = skip; i < nout; i++) { si += out[2 * i] * sin(2 * M_PI * 1000 * i / RATE); co += out[2 * i] * cos(2 * M_PI * 1000 * i / RATE); }
    p = nout > skip ? sqrt(si * si + co * co) * 2 / (nout - skip) : 0;
    int ok = out_rate == RATE && nout >= RATE * SECONDS * 9 / 10 && fabs(20 * log10(rms / want)) < 1 && p / 12000 > 0.9;
    printf("%s %-8s %zu frames at %u Hz, level %+.2f dB, 1 kHz part %.3f\n", ok ? "ok  " : "FAIL", name, nout, out_rate, 20 * log10(rms / want), p / 12000);
    return !ok;
}

static void rtp(unsigned char *p, int seq) { memset(p, 0, 12); p[0] = 0x80; p[1] = 96; p[2] = seq >> 8; p[3] = seq; }

static int test_aptx(int hd)
{
    const struct a2dp_codec *c = find(hd ? "aptX HD" : "aptX"); unsigned char cfg[11];
    memcpy(cfg, c->caps, c->ncaps); cfg[6] = 0x12;                     /* 48 kHz stereo */
    if (!c->check(cfg, c->ncaps) || c->open(cfg, c->ncaps)) { printf("FAIL %s config\n", c->name); return 1; }
    struct aptx_context *enc = aptx_init(hd); nout = 0;
    for (int i = 0, seq = 0; i < RATE * SECONDS; i += 4 * 64, seq++) {     /* 64 aptX samples (256 frames) per packet */
        unsigned char in[256 * 6], pkt[12 + 64 * 6]; size_t w = 0, h = hd ? 12 : 0;
        for (int k = 0; k < 256; k++) for (int ch = 0; ch < 2; ch++) {
            int v = (int)tone(i + k) * 256; unsigned char *b = in + 6 * k + 3 * ch; b[0] = v; b[1] = v >> 8; b[2] = v >> 16;
        }
        aptx_encode(enc, in, sizeof in, pkt + h, sizeof pkt - h, &w);
        if (hd) rtp(pkt, seq);
        c->packet(pkt, h + w, sink);
    }
    aptx_finish(enc); c->close();
    return verdict(c->name);
}

static int test_opus(void)
{
    const struct a2dp_codec *c = find("Opus"); unsigned char cfg[7]; int err = 0;
    memcpy(cfg, c->caps, 7); cfg[6] = 0x80 | 0x10 | 0x02;             /* 48 kHz, 20 ms, stereo */
    if (!c->check(cfg, 7) || c->open(cfg, 7)) { printf("FAIL Opus config\n"); return 1; }
    OpusEncoder *enc = opus_encoder_create(RATE, 2, 2049, &err); nout = 0;     /* OPUS_APPLICATION_AUDIO */
    for (int i = 0, seq = 0; i < RATE * SECONDS; i += 960, seq++) {
        int16_t in[960 * 2]; unsigned char pkt[13 + 1500];
        for (int k = 0; k < 960; k++) in[2 * k] = in[2 * k + 1] = (int16_t)tone(i + k);
        int n = opus_encode(enc, in, 960, pkt + 13, 1500);
        rtp(pkt, seq); pkt[12] = 1;                                     /* one frame */
        if (n > 0) c->packet(pkt, 13 + n, sink);
    }
    c->close();
    return verdict(c->name);
}

/* AAC's in-band configuration (LATM StreamMuxConfig), bit by bit as Android's and PipeWire's FDK encoder writes it:
 * useSameStreamMux 0, audioMuxVersion 0, allStreamsSameTimeFraming 1, numSubFrames 0, numProgram 0, numLayer 0, then
 * the AudioSpecificConfig: object type 5 bits, rate index 4, channels 4, and what FFmpeg reads after it */
static size_t latm(unsigned char *p, int version, unsigned aot, unsigned sfi, unsigned ch)
{
    unsigned long long v = 0; int n = 0;
#define PUT(bits, k) (v = v << (k) | (bits), n += (k))
    PUT(0, 1); PUT(version, 1);
    if (version) { PUT(0, 1); PUT(0, 2); PUT(0xff, 8); }    /* audioMuxVersionA, taraBufferFullness (1 byte) */
    PUT(1, 1); PUT(0, 6); PUT(0, 4); PUT(0, 3);
    if (version) { PUT(0, 2); PUT(2, 8); }                  /* ascLen */
    PUT(aot, 5); PUT(sfi, 4); PUT(ch, 4); PUT(0, 3);
#undef PUT
    size_t bytes = (n + 7) / 8; v <<= 8 * bytes - n;
    for (size_t i = 0; i < bytes; i++) p[i] = v >> 8 * (bytes - 1 - i);
    memset(p + bytes, 0x55, 8);                             /* the frame itself: not looked at */
    return bytes + 8;
}

static int test_latm(void)
{
    unsigned char p[32]; int ok = 1, cfg = 0; size_t n;
    struct { int version; unsigned aot, sfi, ch, rate, nch; int want; const char *what; } t[] = {
        { 0, 2, 4, 2, 44100, 2, 1, "AAC LC 44.1 kHz stereo" },
        { 0, 2, 3, 2, 48000, 2, 1, "AAC LC 48 kHz stereo" },
        { 1, 2, 3, 1, 48000, 1, 1, "audioMuxVersion 1, mono" },
        { 0, 2, 3, 2, 44100, 2, 0, "48 kHz where 44.1 was negotiated" },
        { 0, 2, 0, 2, 48000, 2, 0, "96 kHz" },
        { 0, 2, 15, 2, 48000, 2, 0, "explicit rate" },
        { 0, 5, 3, 2, 48000, 2, 0, "SBR object type" },
        { 0, 2, 3, 0, 48000, 2, 0, "channels from a program config element" },
        { 0, 2, 3, 6, 48000, 2, 0, "5.1" },
        { 0, 2, 3, 2, 48000, 1, 0, "stereo where mono was negotiated" },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
        cfg = 0; n = latm(p, t[i].version, t[i].aot, t[i].sfi, t[i].ch);
        int got = a2dp_latm_check(p, n, t[i].rate, t[i].nch, &cfg);
        if (got != t[i].want) { printf("FAIL AAC config %s: %s\n", t[i].what, got ? "accepted" : "refused"); ok = 0; }
    }
    unsigned char same[4] = { 0x80, 0x55, 0x55, 0x55 };    /* useSameStreamMux */
    cfg = 0;
    if (a2dp_latm_check(same, 4, 48000, 2, &cfg)) { printf("FAIL AAC: reused configuration without one seen\n"); ok = 0; }
    n = latm(p, 0, 2, 3, 2);
    a2dp_latm_check(p, n, 48000, 2, &cfg);
    if (!a2dp_latm_check(same, 4, 48000, 2, &cfg)) { printf("FAIL AAC: reused good configuration refused\n"); ok = 0; }
    n = latm(p, 0, 2, 0, 2);                                /* a bad one in between: the old one counts no more */
    a2dp_latm_check(p, n, 48000, 2, &cfg);
    if (a2dp_latm_check(same, 4, 48000, 2, &cfg)) { printf("FAIL AAC: reused configuration after a bad one\n"); ok = 0; }
    if (a2dp_latm_check(p, 1, 48000, 2, &cfg)) { printf("FAIL AAC: cut configuration accepted\n"); ok = 0; }
    printf("%s AAC     in-band configuration checks\n", ok ? "ok  " : "FAIL");
    return !ok;
}

int main(void) { return test_aptx(0) | test_aptx(1) | test_opus() | test_latm(); }
