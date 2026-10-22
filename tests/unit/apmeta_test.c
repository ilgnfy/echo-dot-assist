/* apmeta.c: shairport-sync's metadata pipe, in any chunking, with a cover art item in the middle. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apmeta.h"

static int fail;
#define CHECK(c, ...) do { if (!(c)) { fail = 1; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *T = "<item><type>636f7265</type><code>6d696e6d</code><length>12</length>\n<data encoding=\"base64\">\nQmxhY2sgSG9sZSBTdW4=</data></item>\n"
                       "<item><type>636f7265</type><code>61736172</code><length>10</length>\n<data encoding=\"base64\">\nU291bmRnYXJkZW4=</data></item>\n"
                       "<item><type>636f7265</type><code>6173616c</code><length>5</length>\n<data encoding=\"base64\">\nU3VwZXJ1bmtub3du</data></item>\n";

static int feed_in(struct apmeta_parser *p, struct apmeta *m, const char *s, size_t len, size_t chunk)
{
    int ch = 0;
    for (size_t i = 0; i < len; i += chunk) ch |= apmeta_feed(p, m, s + i, len - i < chunk ? len - i : chunk);
    return ch;
}

int main(void)
{
    static struct apmeta_parser p; struct apmeta m;
    for (size_t chunk = 1; chunk <= 4096; chunk = chunk < 8 ? chunk + 1 : chunk * 7) {
        memset(&p, 0, sizeof p); apmeta_clear(&m);
        int ch = feed_in(&p, &m, T, strlen(T), chunk);
        CHECK(ch && !strcmp(m.title, "Black Hole Sun") && !strcmp(m.artist, "Soundgarden") && !strcmp(m.album, "Superunknown"),
              "chunks of %zu: \"%s\" / \"%s\" / \"%s\"", chunk, m.title, m.artist, m.album);
    }
    /* cover art between the items: 60 KB of base64, thrown away, the next item still read */
    size_t big = 60000; char *s = malloc(big + 400);
    memcpy(s, "<item><type>73736e63</type><code>50494354</code><length>45000</length>\n<data encoding=\"base64\">\n", 96);
    size_t o = strlen(s); memset(s + o, 'A', big); o += big; memcpy(s + o, "</data></item>\n", 16); o += 15;
    for (size_t chunk = 1; chunk <= 700; chunk *= 9) {
        memset(&p, 0, sizeof p); apmeta_clear(&m);
        feed_in(&p, &m, T, strlen(T), 100); feed_in(&p, &m, s, o, chunk);
        const char *next = "<item><type>636f7265</type><code>6d696e6d</code><length>3</length>\n<data encoding=\"base64\">\nT25l</data></item>\n";
        feed_in(&p, &m, next, strlen(next), chunk);
        CHECK(!strcmp(m.title, "One") && !strcmp(m.artist, "Soundgarden"), "cover art skipped (chunks of %zu), the item after it read: \"%s\"", chunk, m.title);
    }
    free(s);
    /* a new bundle (mdst) and the end of the session (pend) forget the track */
    memset(&p, 0, sizeof p); apmeta_clear(&m); feed_in(&p, &m, T, strlen(T), 64);
    const char *pend = "<item><type>73736e63</type><code>70656e64</code><length>0</length></item>\n";
    int ch = feed_in(&p, &m, pend, strlen(pend), 7);
    CHECK(ch && !m.title[0] && !m.artist[0] && !m.album[0], "the end of the session clears the track");
    /* text is made safe: control characters and broken UTF-8 dropped */
    memset(&p, 0, sizeof p); apmeta_clear(&m);
    const char *bad = "<item><type>636f7265</type><code>6d696e6d</code><length>6</length>\n<data encoding=\"base64\">\nYQFi/2M=</data></item>\n";
    feed_in(&p, &m, bad, strlen(bad), 5);
    CHECK(!strcmp(m.title, "abc"), "control and broken bytes dropped: \"%s\"", m.title);
    /* junk between items, a long title cut at a character */
    memset(&p, 0, sizeof p); apmeta_clear(&m);
    feed_in(&p, &m, "garbage\n\n<item>no tags</item>", 28, 3);
    CHECK(!m.title[0], "junk is ignored");
    printf(fail ? "FAILED\n" : "all good\n");
    return fail;
}
