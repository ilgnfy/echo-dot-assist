/* net.c: the -A allowlist (written strictly, matched by prefix) and the push port's back-off per source address. */
#include <stdio.h>
#include <string.h>
#include "net.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } else printf("ok   %s\n", what); } while (0)

static uint32_t ip(int a, int b, int c, int d) { return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | (uint32_t)d; }

int main(void)
{
    struct net_allow a;
    CHECK(!net_allow_parse(&a, "192.168.1.10") && a.n == 1 && net_allowed(&a, ip(192, 168, 1, 10)) && !net_allowed(&a, ip(192, 168, 1, 11)),
          "one address: that one only");
    CHECK(!net_allow_parse(&a, "192.168.1.10,10.0.5.77/24") && a.n == 2 && net_allowed(&a, ip(10, 0, 5, 1)) && net_allowed(&a, ip(10, 0, 5, 255))
          && !net_allowed(&a, ip(10, 0, 6, 1)) && net_allowed(&a, ip(192, 168, 1, 10)), "a list with a subnet, host bits ignored");
    CHECK(!net_allow_parse(&a, "0.0.0.0/1") && net_allowed(&a, ip(127, 0, 0, 1)) && !net_allowed(&a, ip(128, 0, 0, 1)), "/1");
    CHECK(!net_allow_parse(&a, "255.255.255.255/32") && net_allowed(&a, ip(255, 255, 255, 255)), "/32 and 255");
    static const char *bad[] = { "", "1.2.3", "1.2.3.4.5", "1.2.3.4/0", "1.2.3.4/33", "1.2.3.4/", "010.1.1.1", "1.2.3.04", "256.1.1.1",
                                 "1.2.3.4,", ",1.2.3.4", "1.2.3.4 ", " 1.2.3.4", "1.2.3.4;5.6.7.8", "a.b.c.d", "1.2.3.4/08", "1..2.3",
                                 "1.2.3.4,1.2.3.4,1.2.3.4,1.2.3.4,1.2.3.4,1.2.3.4,1.2.3.4,1.2.3.4,1.2.3.4", "1234.1.1.1", "1.2.3.4/1/2" };
    int all = 1;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        a.n = 5;
        if (!net_allow_parse(&a, bad[i]) || a.n) { printf("     accepted: [%s]\n", bad[i]); all = 0; }
    }
    CHECK(all, "anything else is refused, and leaves the list empty");
    struct net_allow none = { 0 };
    CHECK(net_allowed(&none, ip(1, 2, 3, 4)), "no list: everyone");

    char s[16]; net_ntoa4(ip(192, 168, 1, 7), s);
    CHECK(!strcmp(s, "192.168.1.7"), "dotted quad");

    static struct net_backoff b;
    uint32_t x = ip(10, 0, 0, 9), owner = ip(10, 0, 0, 2); long long t = 1000;
    net_backoff_result(&b, x, 0, t); net_backoff_result(&b, x, 0, t + 1000);
    CHECK(!net_backoff_left(&b, x, t + 1500), "two failures: still served");
    net_backoff_result(&b, x, 0, t + 2000);
    CHECK(net_backoff_left(&b, x, t + 2001) == NET_BACKOFF_MS - 1 && !net_backoff_left(&b, owner, t + 2001), "the third within a minute: refused for a minute, nobody else");
    CHECK(!net_backoff_left(&b, x, t + 2000 + NET_BACKOFF_MS), "a minute later: served again");
    net_backoff_result(&b, x, 0, t + 2000 + NET_BACKOFF_MS);
    CHECK(!net_backoff_left(&b, x, t + 2001 + NET_BACKOFF_MS), "... and counted from zero");
    net_backoff_result(&b, owner, 0, t); net_backoff_result(&b, owner, 0, t + 10); net_backoff_result(&b, owner, 1, t + 20);
    net_backoff_result(&b, owner, 0, t + 30);
    CHECK(!net_backoff_left(&b, owner, t + 40), "a connection that proves the key starts from zero");
    net_backoff_result(&b, owner, 0, t + 40 + NET_BACKOFF_MS); net_backoff_result(&b, owner, 0, t + 50 + NET_BACKOFF_MS);
    CHECK(!net_backoff_left(&b, owner, t + 60 + NET_BACKOFF_MS), "failures more than a minute apart do not add up");
    t += 10 * NET_BACKOFF_MS;
    for (int i = 0; i < 3 * NET_BACKOFF_N; i++) net_backoff_result(&b, ip(172, 16, 0, i), 0, t + 100 + i);
    net_backoff_result(&b, x, 0, t + 1000); net_backoff_result(&b, x, 0, t + 1001); net_backoff_result(&b, x, 0, t + 1002);
    CHECK(net_backoff_left(&b, x, t + 1003) > 0, "a full table still keeps the newest");

    printf(fails ? "FAILED\n" : "all good\n");
    return fails != 0;
}
