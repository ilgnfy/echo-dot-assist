/* See apmeta.h. */
#include "apmeta.h"
#include "hash.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void apmeta_clear(struct apmeta *m) { memset(m, 0, sizeof *m); }

/* four ASCII characters written as eight hex digits ("6d696e6d" -> "minm"); "" if it is not that */
static void hex4(const char *s, char out[5])
{
    out[0] = 0;
    for (int i = 0; i < 4; i++) {
        char h[3] = { s[2 * i], s[2 * i + 1], 0 }, *e; long v = strtol(h, &e, 16);
        if (*e || v < 0x20 || v > 0x7e) { out[0] = 0; return; }
        out[i] = (char)v;
    }
    out[4] = 0;
}

/* the text between <tag> and </tag> in item (NUL-terminated), or NULL */
static const char *between(const char *item, const char *open, const char *close, size_t *len)
{
    const char *a = strstr(item, open), *b;
    if (!a) return NULL;
    a += strlen(open);
    if (!(b = strstr(a, close))) return NULL;
    *len = (size_t)(b - a);
    return a;
}

/* UTF-8 only, no control characters, cut at a character boundary */
static void keep(char *dst, const unsigned char *src, size_t n, int *changed)
{
    char t[APMETA_TEXT]; size_t o = 0;
    for (size_t i = 0; i < n && o < sizeof t - 1; ) {
        unsigned c = src[i]; size_t k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!k) { i++; continue; }                                       /* not a lead byte: dropped */
        if (i + k > n) break;                                            /* cut short at the end */
        int ok = 1;
        for (size_t j = 1; j < k; j++) if ((src[i + j] & 0xc0) != 0x80) ok = 0;
        if (!ok || (k == 1 && c < 0x20)) { i++; continue; }              /* a stray byte or a control character: dropped */
        if (o + k > sizeof t - 1) break;
        memcpy(t + o, src + i, k); o += k; i += k;
    }
    t[o] = 0;
    if (strcmp(dst, t)) { memcpy(dst, t, o + 1); *changed = 1; }
}

static int item(struct apmeta *m, char *it)
{
    size_t tl, cl, dl; int changed = 0;
    const char *t = between(it, "<type>", "</type>", &tl), *c = between(it, "<code>", "</code>", &cl);
    char type[5], code[5];
    if (!t || !c || tl != 8 || cl != 8) return 0;
    hex4(t, type); hex4(c, code);
    if (!strcmp(type, "ssnc")) {
        /* a new track's bundle begins, or the session ended or was flushed: what was known is not this one's */
        if (!strcmp(code, "mdst") || !strcmp(code, "pend") || !strcmp(code, "pfls") || !strcmp(code, "disc")) {
            if (m->title[0] || m->artist[0] || m->album[0]) { apmeta_clear(m); return 1; }
        }
        return 0;
    }
    if (strcmp(type, "core")) return 0;
    char *field = !strcmp(code, "minm") ? m->title : !strcmp(code, "asar") ? m->artist : !strcmp(code, "asal") ? m->album : NULL;
    if (!field) return 0;
    const char *d = between(it, "<data encoding=\"base64\">", "</data>", &dl);
    if (!d) { keep(field, (const unsigned char *)"", 0, &changed); return changed; }       /* an empty value */
    char b64[1100]; uint8_t raw[800];
    if (dl >= sizeof b64) return 0;
    size_t o = 0;
    for (size_t i = 0; i < dl; i++) if (d[i] != '\n' && d[i] != '\r' && d[i] != ' ') b64[o++] = d[i];
    b64[o] = 0;
    long n = b64_decode(b64, o, raw, sizeof raw);
    if (n < 0) return 0;
    keep(field, raw, (size_t)n, &changed);
    return changed;
}

int apmeta_feed(struct apmeta_parser *p, struct apmeta *m, const char *data, size_t len)
{
    int changed = 0;
    while (len) {
        if (p->skipping) {                                  /* throw away up to "</item>", which may straddle two reads */
            char both[sizeof p->tail + 512]; size_t k = len < 512 ? len : 512, t = strlen(p->tail);
            memcpy(both, p->tail, t); memcpy(both + t, data, k); both[t + k] = 0;
            const char *e = strstr(both, "</item>");
            if (e) { size_t used = (size_t)(e + 7 - both) - t; data += used; len -= used; p->skipping = 0; p->tail[0] = 0; p->n = 0; continue; }
            size_t keepn = t + k < 6 ? t + k : 6;
            memcpy(p->tail, both + t + k - keepn, keepn); p->tail[keepn] = 0;
            data += k; len -= k;
            continue;
        }
        size_t room = sizeof p->acc - 1 - p->n, k = len < room ? len : room;
        memcpy(p->acc + p->n, data, k); p->n += k; p->acc[p->n] = 0;
        char *e = strstr(p->acc, "</item>");
        if (e) {
            size_t used_in_acc = (size_t)(e + 7 - p->acc), before = p->n - k;
            e[7] = 0;                                       /* this item only: a second one may follow in the same read */
            changed |= item(m, p->acc);
            size_t consumed = used_in_acc > before ? used_in_acc - before : 0;
            data += consumed; len -= consumed; p->n = 0; p->acc[0] = 0;
            continue;
        }
        data += k; len -= k;
        if (p->n >= sizeof p->acc - 1) { p->n = 0; p->acc[0] = 0; p->skipping = 1; p->tail[0] = 0; }      /* too big to be a name: cover art */
    }
    return changed;
}
