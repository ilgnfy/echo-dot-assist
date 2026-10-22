/* Outgoing TCP connections by host name, for URLs that Home Assistant or Music Assistant hand us; and who may come in. */
#ifndef NET_H
#define NET_H
#include <stdint.h>
#include <sys/socket.h>
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

/* IPv4 address of a peer (getpeername / recvfrom), host byte order; 0 for anything else */
uint32_t net_peer4(const struct sockaddr *sa, socklen_t len);
void net_ntoa4(uint32_t addr, char out[16]);

/* Who may connect to a listener that has no authentication of its own (Wyoming: hassmic -A).  A comma separated list
 * of IPv4 addresses and subnets, "192.168.1.10,10.0.5.0/24", written strictly: four decimal numbers 0-255 without
 * leading zeros (inet_aton would read "010" as octal), a prefix length 1-32.  Host bits under the prefix are ignored. */
#define NET_ALLOW_MAX 8
struct net_allow { int n; uint32_t net[NET_ALLOW_MAX], mask[NET_ALLOW_MAX]; };
int net_allow_parse(struct net_allow *a, const char *spec);     /* 0, or -1 (and *a empty) for anything else */
int net_allowed(const struct net_allow *a, uint32_t addr);      /* an empty list allows everyone */

/* Back-off per source address, for a listener that serves one connection at a time (the push port): a peer whose
 * connections fail NET_BACKOFF_FAILS times within NET_BACKOFF_MS is refused for NET_BACKOFF_MS from the last of them.
 * One that succeeds starts from zero.  The table is small and forgets the oldest, so it costs nothing to fill. */
#define NET_BACKOFF_N     32
#define NET_BACKOFF_FAILS 3
#define NET_BACKOFF_MS    60000
struct net_backoff { struct { uint32_t addr; int fails; long long first, until; } e[NET_BACKOFF_N]; };
long long net_backoff_left(const struct net_backoff *b, uint32_t addr, long long now);   /* ms it is still refused, 0 */
void net_backoff_result(struct net_backoff *b, uint32_t addr, int ok, long long now);
#endif
