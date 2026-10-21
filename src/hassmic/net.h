/* Outgoing TCP connections by host name, for URLs that Home Assistant or Music Assistant hand us. */
#ifndef NET_H
#define NET_H
#include <sys/types.h>

/* Connected socket or -1.  Every address the name resolves to is tried in turn, so an unreachable one (a global IPv6
 * address behind the egress lock, say) does not hide a working one.  Names ending in ".local" are looked up by mDNS: the
 * system resolver does not do that.  timeout_s applies to each connect() and stays set as send and receive timeout. */
int net_connect(const char *host, const char *port, int timeout_s);

/* socket() as the group that the egress lock lets out to any address (runas -r); for sockets others open for us (libcurl) */
int net_socket(int family, int type, int protocol);

/* IPv4 address of a ".local" name by one-shot mDNS query (RFC 6762 5.1), in network byte order; 0 if nobody answered.
 * Only answers from port 5353 on a local subnet that repeat the query's id count; kept for their TTL (at most 5 min). */
unsigned mdns_resolve4(const char *name);

long long net_mono_ms(void);
/* deadline: net_mono_ms() value after which the read gives up (-1, ETIMEDOUT); 0 = none.
 * read_full_by: bytes read (fewer at end of stream) or -1.  read_until: reads up to and including delim, never past
 * it (what follows stays for the caller), NUL terminated; its length, 0 at end of stream, -1 on error or overflow. */
ssize_t net_read_full_by(int fd, void *buf, size_t n, long long deadline);
ssize_t net_read_until(int fd, char *buf, size_t cap, const char *delim, long long deadline);
#endif
