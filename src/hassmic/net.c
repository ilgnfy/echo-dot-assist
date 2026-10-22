#ifndef _GNU_SOURCE
#define _GNU_SOURCE                     /* getresgid on glibc */
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/fsuid.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "net.h"

/* ---------------------------------------------------------------- mDNS */

static int put_name(uint8_t *p, size_t cap, const char *name)
{
    size_t o = 0;
    while (*name) {
        const char *dot = strchr(name, '.'); size_t l = dot ? (size_t)(dot - name) : strlen(name);
        if (!l || l > 63 || o + l + 2 > cap) return -1;
        p[o++] = l; memcpy(p + o, name, l); o += l;
        name += l + (dot ? 1 : 0);
    }
    p[o++] = 0;
    return o;
}

/* Name at *off (compression pointers followed) as dotted text; *off moves past it.  -1 on a malformed packet. */
static int get_name(const uint8_t *m, size_t len, size_t *off, char *out, size_t outsz)
{
    size_t o = *off, n = 0; int jumps = 0, moved = 0;
    for (;;) {
        if (o >= len) return -1;
        uint8_t l = m[o];
        if ((l & 0xc0) == 0xc0) {
            if (o + 1 >= len || ++jumps > 16) return -1;
            if (!moved) { *off = o + 2; moved = 1; }
            o = (l & 0x3f) << 8 | m[o + 1];
            continue;
        }
        if (l & 0xc0) return -1;
        if (!l) { if (!moved) *off = o + 1; break; }
        if (o + 1 + l > len || n + l + 2 > outsz) return -1;
        if (n) out[n++] = '.';
        memcpy(out + n, m + o + 1, l); n += l; o += 1 + l;
    }
    out[n] = 0;
    return 0;
}

static unsigned parse_answer(const uint8_t *m, size_t len, const char *name, unsigned id, unsigned *ttl)
{
    char rn[256]; size_t off = 12;
    if (len < 12 || !(m[2] & 0x80) || (unsigned)(m[0] << 8 | m[1]) != id) return 0;   /* responses to our query only */
    unsigned qd = m[4] << 8 | m[5], rr = (m[6] << 8 | m[7]) + (m[8] << 8 | m[9]) + (m[10] << 8 | m[11]);
    for (unsigned i = 0; i < qd; i++) { if (get_name(m, len, &off, rn, sizeof rn)) return 0; off += 4; }
    for (unsigned i = 0; i < rr; i++) {
        if (get_name(m, len, &off, rn, sizeof rn) || off + 10 > len) return 0;
        unsigned type = m[off] << 8 | m[off + 1], cls = (m[off + 2] << 8 | m[off + 3]) & 0x7fff, rdlen = m[off + 8] << 8 | m[off + 9];
        off += 10;
        if (off + rdlen > len) return 0;
        if (type == 1 && cls == 1 && rdlen == 4 && !strcasecmp(rn, name)) {
            unsigned a; memcpy(&a, m + off, 4);
            *ttl = (unsigned)m[off - 6] << 24 | m[off - 5] << 16 | m[off - 4] << 8 | m[off - 3];
            return a;
        }
        off += rdlen;
    }
    return 0;
}

/* Sent from a port other than 5353, so responders answer by unicast to it (RFC 6762 5.1, 6.7) and the avahi-daemon that
 * owns 5353 on the Echo is not in the way.  Not an ephemeral port: those start at 32768, and the stock firewall admits
 * inbound UDP on 16384-32767 only.  The query goes out on every IPv4 interface: multicast has no route to follow. */
/* Answers are kept for their TTL (at most CACHE_MAX_S): every announcement and media fetch from Home Assistant's
 * "homeassistant.local" URL looked it up again, up to 1.75 s each when the first query went unanswered. */
#define CACHE_N     4
#define CACHE_MAX_S 300
static struct { char name[256]; unsigned addr; long long until; } cache[CACHE_N];
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

static long long mono_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec; }

static unsigned cache_get(const char *name)
{
    unsigned a = 0; long long now = mono_s();
    pthread_mutex_lock(&cache_lock);
    for (int i = 0; i < CACHE_N && !a; i++) if (cache[i].until > now && !strcasecmp(cache[i].name, name)) a = cache[i].addr;
    pthread_mutex_unlock(&cache_lock);
    return a;
}

static void cache_put(const char *name, unsigned addr, unsigned ttl)      /* addr 0: forget the name */
{
    int slot = 0; long long now = mono_s();
    pthread_mutex_lock(&cache_lock);
    for (int i = 0; i < CACHE_N; i++) {
        if (!strcasecmp(cache[i].name, name)) { slot = i; break; }
        if (cache[i].until < cache[slot].until) slot = i;
    }
    snprintf(cache[slot].name, sizeof cache[slot].name, "%s", name);
    cache[slot].addr = addr; cache[slot].until = addr ? now + (ttl < CACHE_MAX_S ? ttl : CACHE_MAX_S) : 0;
    pthread_mutex_unlock(&cache_lock);
}

unsigned mdns_resolve4(const char *host)
{
    char name[256]; uint8_t q[300], r[1500]; unsigned addr = 0, life = 0, id = 0; int f;
    struct { uint32_t net, mask; } links[8]; int nlinks = 0;
    snprintf(name, sizeof name, "%s", host);
    size_t nl = strlen(name); if (nl && name[nl - 1] == '.') name[nl - 1] = 0;
    if ((addr = cache_get(name))) return addr;
    /* A fresh id per query: a unicast answer repeats it (RFC 6762 6.7), so one sent blind does not count */
    if ((f = open("/dev/urandom", O_RDONLY)) >= 0) { if (read(f, &id, 2) != 2) id = 0; close(f); }
    id = (id ^ (unsigned)getpid() ^ (unsigned)mono_s()) & 0xffff;
    memset(q, 0, 12); q[0] = id >> 8; q[1] = id;                               /* id; flags 0: standard query */
    q[5] = 1;                                                                   /* one question */
    int l = put_name(q + 12, sizeof q - 16, name);
    if (l < 0) return 0;
    size_t qlen = 12 + l;
    q[qlen++] = 0; q[qlen++] = 1;                                               /* type A */
    q[qlen++] = 0x80; q[qlen++] = 1;                                            /* class IN, unicast response wanted */
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), bound = 0;
    if (s < 0) return 0;
    struct sockaddr_in me = { .sin_family = AF_INET };
    for (int i = 0; i < 20 && !bound; i++) {
        me.sin_port = htons(16384 + ((unsigned)getpid() * 7919u + i * 104729u) % 16384);
        bound = !bind(s, (struct sockaddr *)&me, sizeof me);
    }
    if (!bound) { close(s); return 0; }
    unsigned char ttl = 255; setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(5353) };
    inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
    static const int wait_ms[] = { 250, 500, 1000 };
    for (int round = 0; round < 3 && !addr; round++) {
        struct ifaddrs *ifs = NULL; int sent = 0;
        if (!getifaddrs(&ifs)) {
            for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
                if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) ||
                    !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_MULTICAST)) continue;
                struct in_addr via = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
                if (round == 0 && nlinks < 8 && i->ifa_netmask) {
                    uint32_t mask = ((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr;
                    links[nlinks].net = via.s_addr & mask; links[nlinks++].mask = mask;
                }
                setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &via, sizeof via);
                if (sendto(s, q, qlen, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)qlen) sent++;
            }
            freeifaddrs(ifs);
        }
        if (!sent) sendto(s, q, qlen, 0, (struct sockaddr *)&to, sizeof to);
        struct pollfd pfd = { .fd = s, .events = POLLIN };
        while (!addr && poll(&pfd, 1, wait_ms[round]) > 0) {
            struct sockaddr_in from; socklen_t fl = sizeof from; int onlink = !nlinks;
            ssize_t n = recvfrom(s, r, sizeof r, 0, (struct sockaddr *)&from, &fl);
            /* mDNS answers come from port 5353 and from the link itself (RFC 6762 6, 11): anything else is not one */
            if (n <= 0 || fl < (socklen_t)sizeof from || from.sin_port != htons(5353)) continue;
            for (int i = 0; i < nlinks; i++) onlink |= (from.sin_addr.s_addr & links[i].mask) == links[i].net;
            if (onlink) addr = parse_answer(r, n, name, id, &life);
        }
    }
    close(s);
    if (addr && life) cache_put(name, addr, life);
    return addr;
}

static void mdns_forget(const char *host)
{
    char name[256]; snprintf(name, sizeof name, "%s", host);
    size_t nl = strlen(name); if (nl && name[nl - 1] == '.') name[nl - 1] = 0;
    if (cache_get(name)) cache_put(name, 0, 0);
}

/* ---------------------------------------------------------------- TCP */

/* A socket owned by the real group, if runas -r gave us one: the egress lock lets that group reach any address, while
 * the effective group stays what the mixer wants.  The filesystem group is per thread and switched back at once. */
int net_socket(int family, int type, int protocol)
{
    gid_t r, e, s; int fd;
    type |= SOCK_CLOEXEC;                       /* not into the tools hassmic starts (netio.h) */
    if (getresgid(&r, &e, &s) || r == e) return socket(family, type, protocol);
    setfsgid(r);
    fd = socket(family, type, protocol);
    setfsgid(e);
    return fd;
}

int net_connect(const char *host, const char *port, int timeout_s)
{
    struct addrinfo hints = { 0 }, *ai = NULL, local = { 0 }, *list = NULL; struct sockaddr_in sin = { 0 };
    size_t hl = strlen(host);
    if (hl > 6 && !strcasecmp(host + hl - 6, ".local")) {
        unsigned a = mdns_resolve4(host);
        if (a) {
            sin.sin_family = AF_INET; sin.sin_port = htons(atoi(port)); sin.sin_addr.s_addr = a;
            local.ai_family = AF_INET; local.ai_addr = (struct sockaddr *)&sin; local.ai_addrlen = sizeof sin;
            list = &local;
        }
    }
    hints.ai_socktype = SOCK_STREAM;
    if (!list) {                                        /* also for ".local" that nobody answered: some DNS servers serve it */
        if (getaddrinfo(host, port, &hints, &ai)) { fprintf(stderr, "net: cannot resolve %s\n", host); return -1; }
        list = ai;
    }
    int fd = -1;
    for (struct addrinfo *p = list; p && fd < 0; p = p->ai_next) {
        struct timeval tv = { timeout_s, 0 };           /* connect() obeys the send timeout */
        if ((fd = net_socket(p->ai_family, SOCK_STREAM, 0)) < 0) continue;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, p->ai_addr, p->ai_addrlen)) { close(fd); fd = -1; }
    }
    if (ai) freeaddrinfo(ai);
    if (fd < 0 && list == &local) mdns_forget(host);   /* moved, or a stale answer: ask again next time */
    if (fd < 0) fprintf(stderr, "net: cannot connect to %s:%s (not a local address? see lockdown.sh)\n", host, port);
    return fd;
}

long long net_mono_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Until fd is readable or the deadline (0: none, the socket's own receive timeout applies) has passed */
static int wait_by(int fd, long long deadline)
{
    for (;;) {
        if (!deadline) return 0;
        long long left = deadline - net_mono_ms();
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, left > 60000 ? 60000 : (int)left);
        if (r > 0) return 0;
        if (r < 0 && errno != EINTR) return -1;
    }
}

ssize_t net_read_full_by(int fd, void *buf, size_t n, long long deadline)
{
    char *p = buf; size_t got = 0;
    while (got < n) {
        if (wait_by(fd, deadline)) return -1;
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        got += r;
    }
    return got;
}

/* Header lines used to come one read() per byte.  Peeking takes what is there in one call and consumes exactly up to
 * the delimiter, so whatever follows it (a body, WebSocket frames, a signature) stays in the socket for the caller. */
ssize_t net_read_until(int fd, char *buf, size_t cap, const char *delim, long long deadline)
{
    size_t have = 0, dl = strlen(delim);
    if (cap < 2 || !dl) return -1;
    for (;;) {
        if (wait_by(fd, deadline)) return -1;
        ssize_t r = recv(fd, buf + have, cap - 1 - have, MSG_PEEK);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return have ? -1 : 0;
        size_t take = r, from = have >= dl ? have - dl + 1 : 0, end = have + r;
        for (size_t i = from; i + dl <= end; i++) if (!memcmp(buf + i, delim, dl)) { take = i + dl - have; break; }
        if (net_read_full_by(fd, buf + have, take, 0) != (ssize_t)take) return -1;     /* peeked: there already */
        have += take; buf[have] = 0;
        if (have >= dl && !memcmp(buf + have - dl, delim, dl)) return have;
        if (have >= cap - 1) return -1;                 /* too long for the caller's buffer */
    }
}

/* ---------------------------------------------------------------- who may connect */

uint32_t net_peer4(const struct sockaddr *sa, socklen_t len)
{
    if (!sa || len < (socklen_t)sizeof(struct sockaddr_in) || sa->sa_family != AF_INET) return 0;
    return ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr);
}

void net_ntoa4(uint32_t a, char out[16]) { snprintf(out, 16, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255); }

/* A decimal number of 1-3 digits without a leading zero, up to max; *s moves past it.  -1 if there is none. */
static int number(const char **s, int max)
{
    const char *p = *s; int v = 0, n = 0;
    while (*p >= '0' && *p <= '9' && n < 3) { v = v * 10 + (*p++ - '0'); n++; }
    if (!n || (n > 1 && **s == '0') || (*p >= '0' && *p <= '9') || v > max) return -1;
    *s = p;
    return v;
}

int net_allow_parse(struct net_allow *a, const char *s)
{
    a->n = 0;
    for (;;) {
        uint32_t addr = 0; int len = 32, v;
        for (int i = 0; i < 4; i++) {
            if ((i && *s++ != '.') || (v = number(&s, 255)) < 0) { a->n = 0; return -1; }
            addr = addr << 8 | (uint32_t)v;
        }
        if (*s == '/' && (s++, (len = number(&s, 32)) < 1)) { a->n = 0; return -1; }
        if (a->n == NET_ALLOW_MAX) { a->n = 0; return -1; }
        a->mask[a->n] = len == 32 ? 0xffffffffu : ~(0xffffffffu >> len);
        a->net[a->n] = addr & a->mask[a->n]; a->n++;
        if (!*s) return 0;
        if (*s++ != ',') { a->n = 0; return -1; }
    }
}

int net_allowed(const struct net_allow *a, uint32_t addr)
{
    if (!a->n) return 1;
    for (int i = 0; i < a->n; i++) if ((addr & a->mask[i]) == a->net[i]) return 1;
    return 0;
}

long long net_backoff_left(const struct net_backoff *b, uint32_t addr, long long now)
{
    for (int i = 0; i < NET_BACKOFF_N; i++)
        if (b->e[i].first && b->e[i].addr == addr) return b->e[i].until > now ? b->e[i].until - now : 0;
    return 0;
}

void net_backoff_result(struct net_backoff *b, uint32_t addr, int ok, long long now)
{
    int slot = -1, old = 0;
    for (int i = 0; i < NET_BACKOFF_N && slot < 0; i++) {
        if (b->e[i].first && b->e[i].addr == addr) slot = i;
        else if (b->e[i].first < b->e[old].first) old = i;
    }
    if (slot < 0) {
        if (ok) return;
        slot = old; memset(&b->e[slot], 0, sizeof b->e[slot]); b->e[slot].addr = addr; b->e[slot].first = now;
    }
    if (ok) { memset(&b->e[slot], 0, sizeof b->e[slot]); return; }
    /* a count older than the window, or a refusal that has run out, starts again */
    if (now - b->e[slot].first >= NET_BACKOFF_MS || (b->e[slot].until && b->e[slot].until <= now)) {
        b->e[slot].fails = 0; b->e[slot].until = 0; b->e[slot].first = now;
    }
    if (++b->e[slot].fails >= NET_BACKOFF_FAILS) b->e[slot].until = now + NET_BACKOFF_MS;
}
