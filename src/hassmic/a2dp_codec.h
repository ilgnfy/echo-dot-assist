/* Bluetooth speaker codecs (a2dp_codecs.c): what a2dp.c offers per stream endpoint, and the decoders behind them. */
#ifndef A2DP_CODEC_H
#define A2DP_CODEC_H
#include <stddef.h>
#include <stdint.h>

enum { A2DP_SBC = 0x00, A2DP_AAC = 0x02, A2DP_VENDOR = 0xff };

/* where decoded audio goes: frames of ch channels, s16 interleaved */
typedef void a2dp_pcm_fn(const int16_t *pcm, unsigned frames, unsigned ch, unsigned rate);

struct a2dp_codec {
    const char *name;
    unsigned type;                                  /* A2DP codec type */
    const unsigned char *caps; unsigned ncaps;      /* codec information element after media type and codec type */
    int  (*usable)(void);                           /* the decoder exists on this device */
    int  (*check)(const unsigned char *cfg, unsigned n);    /* 1: a configuration within caps */
    int  (*open)(const unsigned char *cfg, unsigned n);     /* 0: decoder ready */
    void (*packet)(const unsigned char *p, size_t n, a2dp_pcm_fn *out);   /* one L2CAP media packet */
    void (*close)(void);
};

extern const struct a2dp_codec a2dp_codecs[];      /* in order of preference; the source still picks */
extern const int a2dp_ncodecs;

/* AAC: is this LATM AudioMuxElement's configuration AAC LC at rate with up to ch channels (or one seen before)?
 * *cfg_ok carries "a good configuration was seen" from packet to packet; 0 at the start of a stream */
int a2dp_latm_check(const unsigned char *p, size_t n, unsigned rate, unsigned ch, int *cfg_ok);
#endif
