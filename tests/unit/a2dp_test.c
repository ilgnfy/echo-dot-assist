/* The Bluetooth speaker's radio side (a2dp.c) against what a hostile or broken device can send: SDP data element
 * lengths, L2CAP MTUs, authentication failures that used to delete link keys, pairing outside the window, audio at a
 * rate the player has no room for, and the ACL credit pool shared with LE.  a2dp.c is included whole, the controller
 * (hci.h) and the rest of hassmic are stubbed; nothing here runs a thread. */
#include <stdarg.h>
#include "../../src/hassmic/a2dp.c"

/* ---------------------------------------------------------------- stubs */
pthread_mutex_t core_lock = PTHREAD_MUTEX_INITIALIZER;
const char *core_name = "Echo Test";
enum state core_state(void) { return IDLE; }
int  core_volume(void) { return 50; }
void core_set_volume(int p) { (void)p; }
void core_music(int s, int on) { (void)s; (void)on; }
void core_bt_device(const char *n, int on) { (void)n; (void)on; }
void core_bt_pairing(int on) { (void)on; }
int  bt_open(unsigned r, unsigned c) { (void)r; (void)c; return 0; }
int  bt_write(const void *d, size_t n) { (void)d; (void)n; return 0; }
long long bt_queued_us(void) { return 0; }
void bt_close(void) {}
int  ble_present(void) { return 0; }
void ble_start(const struct ble_handler *h) { (void)h; }
void btout_start(void) {}
void btout_ready(uint64_t a, const char *n, int b, unsigned m) { (void)a; (void)n; (void)b; (void)m; }
void btout_gone(void) {}
void btout_streaming(int on) { (void)on; }
void btout_route(int on) { (void)on; }
int  btout_idle(void) { return 0; }
int  btout_speaker_volume(void) { return 0; }
void btout_start_failed(void) {}
void btout_absvol(int on, int p) { (void)on; (void)p; }
int  btout_want(void) { return 0; }
size_t btout_packet(unsigned char *b, size_t m) { (void)b; (void)m; return 0; }

static unsigned ops[64]; static int nops; static unsigned char ret[64]; static int writes, *pool;
int  hci_cmd(unsigned op, const void *p, unsigned n) { (void)p; (void)n; if (nops < 64) ops[nops++] = op; return 0; }
const unsigned char *hci_ret(void) { return ret; }
int  hci_write(const void *b, size_t n) { (void)b; (void)n; writes++; return 0; }
void hci_poke(void) {}
int *hci_acl_pool(void) { return pool; }

/* ---------------------------------------------------------------- checks */
static int bad;
static void expect(int ok, const char *fmt, ...)
{
    va_list a; va_start(a, fmt);
    if (!ok) { printf("FAIL "); vprintf(fmt, a); printf("\n"); bad = 1; }
    va_end(a);
}
static int sent(unsigned op) { for (int i = 0; i < nops; i++) if (ops[i] == op) return 1; return 0; }

static void test_de(void)
{
    unsigned t; size_t len;
    /* sequence, 32-bit length 0xffffffff: h + len wraps a 32-bit size_t to 4 and passed */
    unsigned char huge[10] = { 0x37, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0 };
    expect(de(huge, sizeof huge, &t, &len) == 0, "de: 32-bit length past the end accepted");
    unsigned char fits[9] = { 0x37, 0, 0, 0, 4, 1, 2, 3, 4 };
    expect(de(fits, sizeof fits, &t, &len) == 5 && len == 4 && t == 6, "de: 32-bit length that fits refused");
    unsigned char seq8[4] = { 0x35, 3, 0x19, 0x11 };
    expect(de(seq8, 4, &t, &len) == 0, "de: 8-bit length one past the end accepted");
    expect(de(seq8, 5, &t, &len) == 2 && len == 3, "de: 8-bit length that fits refused");
    unsigned char u128[1] = { 0x1c };                       /* 16-byte UUID, no data */
    expect(de(u128, 1, &t, &len) == 0, "de: fixed size past the end accepted");
    expect(de(u128, 0, &t, &len) == 0, "de: empty input accepted");
}

static struct link *mklink(uint64_t a, int handle)
{
    struct link *l = &links[0]; memset(l, 0, sizeof *l);
    l->used = 1; l->addr = a; l->handle = handle;
    return l;
}

static void test_mtu(void)
{
    struct link *l = mklink(0x112233445566, 1); unsigned char d[4] = { 0x01, 0x00, 0x40, 0x00 };   /* SDP, our CID 0x40 */
    conn_request(l, 1, d, 4);
    struct chan *c = chan_by(l, 0x40);
    expect(c != NULL, "l2cap: SDP channel not opened");
    if (!c) return;
    unsigned char cfg[8] = { 0x40, 0x00, 0, 0, 0x01, 2, 10, 0 };   /* MTU 10 */
    config_req_rx(l, 2, cfg, 8);
    expect(c->rmtu == 48, "l2cap: MTU 10 taken as %d, want 48", c->rmtu);
    cfg[6] = 0x00; cfg[7] = 0x02;                           /* MTU 512 */
    config_req_rx(l, 3, cfg, 8);
    expect(c->rmtu == 512, "l2cap: MTU 512 taken as %d", c->rmtu);
    link_gone(l);
}

static void auth_failed(struct link *l, unsigned status)
{
    unsigned char e[5] = { EV_AUTH_COMPLETE, 3, status, (unsigned char)l->handle, 0 };
    a2dp_event(e, sizeof e);
}

static void test_keys(void)
{
    char dir[] = "/tmp/a2dp_testXXXXXX";
    if (!mkdtemp(dir)) { printf("FAIL no temp dir\n"); bad = 1; return; }
    setenv("HASSMIC_STATE", dir, 1);
    uint64_t a = 0xa1a2a3a4a5a6;
    keys[0].addr = a; keys[0].type = 4; nkeys = 1;

    /* outside the pairing window: the device says it has no key, ours stays */
    struct link *l = mklink(a, 7); l->pend_until = ms() + 15000;
    auth_failed(l, 0x06);
    expect(key_for(a) != NULL, "auth 0x06 outside pairing deleted the key");
    auth_failed(l, 0x05);
    expect(key_for(a) != NULL, "auth 0x05 outside pairing deleted the key");

    /* the speaker's address: no Just Works for it merely because no key is there */
    out.addr = 0xb1b2b3b4b5b6; out.on = 1; out.pairing = 0;
    expect(!out_may_pair(out.addr), "speaker without key may pair outside a search");
    unsigned char io[8] = { EV_IO_CAP_REQUEST, 6 }; for (int i = 0; i < 6; i++) io[2 + i] = out.addr >> 8 * i;
    npend = 0; a2dp_event(io, sizeof io);
    expect(npend == 1 && pend[0].op == OP_IO_CAP_NEG, "speaker's IO capability request not refused");
    out.pairing = 1; out.pair_until = ms() + 1000;
    expect(out_may_pair(out.addr), "speaker found by the search may not pair");
    out.pairing = 0; out.addr = 0;

    /* in the pairing window the user wants it paired: the stale key goes and we authenticate again */
    atomic_store(&pairing_on, 1);
    l->pend_until = ms() + 15000; l->auth_sent = 1;
    auth_failed(l, 0x06);
    expect(key_for(a) == NULL && !l->auth_sent && l->pend_until, "auth 0x06 in the pairing window: no fresh pairing");
    atomic_store(&pairing_on, 0);

    /* commands for a link that went are not sent to its handle afterwards */
    npend = 0; { unsigned char d[3] = { 7, 0, 0x13 }; later(OP_DISCONNECT, d, 3); }
    link_gone(l);
    nops = 0; a2dp_upkeep();
    expect(!sent(OP_DISCONNECT), "disconnect sent for a link already gone");
    char p[300]; snprintf(p, sizeof p, "%s/bt_keys", dir); unlink(p); rmdir(dir);
}

static void test_ring(void)
{
    int16_t pcm[2 * 960] = { 0 };
    r_rate = 0; r_count = 0;
    ring_push(pcm, 960, 2, 96000);                          /* AAC with implicit SBR, as FFmpeg would put it out */
    expect(r_count == 0 && r_rate == 0, "96 kHz audio reached the player");
    ring_push(pcm, 960, 2, 48000);
    expect(r_count == 960 && r_rate == 48000, "48 kHz audio refused");
    r_count = 0;
}

static void test_pool(void)
{
    int shared = 0;
    memset(ret, 0, sizeof ret); ret[0] = 0xfd; ret[1] = 0x03; ret[3] = 8;     /* BR/EDR: 8 x 1021 */
    pool = &shared; shared = 8;                             /* ble.c filled it from the same Read Buffer Size */
    a2dp_setup();
    expect(credits == &shared, "shared buffers: a2dp keeps a count of its own");
    struct link *l = mklink(0x010203040506, 9); unsigned char x[10] = { 0 };
    for (int i = 0; i < 3; i++) l2_send(l, 0x40, x, sizeof x);
    expect(shared == 5, "shared pool at %d after 3 packets, want 5", shared);
    a2dp_completed(9, 3);
    expect(shared == 8, "shared pool at %d after completion, want 8", shared);
    link_gone(l);
    pool = NULL; a2dp_setup();
    expect(credits == &own_credits && own_credits == 8, "separate buffers: own count not set");
}

int main(void)
{
    test_de(); test_mtu(); test_keys(); test_ring(); test_pool();
    if (!bad) printf("a2dp: SDP lengths, L2CAP MTU, link keys, pairing window, player rate, shared ACL pool ok\n");
    return bad;
}
