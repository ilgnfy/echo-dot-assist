/* shairport-sync's metadata pipe (docs/airplay.md): a stream of
 *   <item><type>636f7265</type><code>6d696e6d</code><length>5</length>
 *   <data encoding="base64">
 *   VGl0bGU=</data></item>
 * with type and code four ASCII characters in hex ("core" + "minm" = the title, "asar" artist, "asal" album; "ssnc" + "mdst"/"pend"...: the
 * session).  Cover art arrives as one big item ("PICT"): not asked for from shairport-sync, and thrown away here if one comes. */
#ifndef APMETA_H
#define APMETA_H
#include <stddef.h>

#define APMETA_TEXT 160                  /* bytes of a title, artist or album kept (UTF-8, cut at a character) */

struct apmeta { char title[APMETA_TEXT], artist[APMETA_TEXT], album[APMETA_TEXT]; };

struct apmeta_parser {
    char acc[2048];                      /* the item being read */
    size_t n;
    int skipping;                        /* an item too big to keep (cover art): thrown away up to its end */
    char tail[8];                        /* the last bytes while skipping: "</item>" may straddle two reads */
};

/* data: what the pipe gave; m is updated in place.  1 if a field changed */
int apmeta_feed(struct apmeta_parser *p, struct apmeta *m, const char *data, size_t len);
void apmeta_clear(struct apmeta *m);
#endif
