/*
 * asteroids_uim: activates the SIM cards' subscriptions, the job Qualcomm's qcrild does on Android.
 *
 * The modem finds a card and its applications by itself, but it doesn't bind a USIM (or SIM) application to a
 * subscription until the AP asks: until then its AT handler answers +CPIN? with "SIM busy" (CME ERROR 14) and
 * registers nowhere. This client talks to the modem's UIM service (QMI service 11) over QRTR: it reads the card status,
 * and for each card whose GSM/UMTS application isn't provisioned, sends CHANGE_PROVISIONING_SESSION: slot 1's
 * application becomes the primary subscription (SUB1), slot 2's the secondary (SUB2). It then stays registered for
 * card status changes, so a SIM inserted later is provisioned too, and follows the service: when the modem restarts,
 * its UIM service comes back on a new port (the lookup reports it) and the cards are provisioned again.
 *
 * Message layouts are QMI UIM's, as libqmi documents them (GET_CARD_STATUS 0x2F, REGISTER_EVENTS 0x2E,
 * STATUS_CHANGE_IND 0x32, CHANGE_PROVISIONING_SESSION 0x38). Encoded by hand: there are only four.
 * Run by start-modem.sh after the modem starts; logs to stdout. "asteroids_uim status" prints the status once;
 * "asteroids_uim powerup <slot>" power-cycles a card slot (QMI UIM POWER_DOWN/POWER_UP).
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "libqrtr.h"

#define UIM_SERVICE 11
#define MSG_REGISTER_EVENTS 0x2e
#define MSG_GET_CARD_STATUS 0x2f
#define MSG_STATUS_CHANGE_IND 0x32
#define MSG_CHANGE_PROVISIONING 0x38
#define MSG_POWER_DOWN 0x30
#define MSG_POWER_UP 0x31
#define TLV_RESULT 0x02
#define TLV_CARD_STATUS 0x10
#define NO_INDEX 0xffff
#define MAX_CARDS 2
#define MAX_APPS 8

enum { CARD_ABSENT, CARD_PRESENT, CARD_ERROR };
enum { APP_SIM = 1, APP_USIM = 2 };
/* Session types: primary and secondary GSM/UMTS provisioning. */
static const uint8_t session_for_slot[MAX_CARDS] = { 0, 2 };
static const char *const card_states[] = { "absent", "present", "error" };
static const char *const app_types[] = { "unknown", "SIM", "USIM", "RUIM", "CSIM", "ISIM" };
static const char *const app_states[] = { "unknown", "detected", "PIN required", "PUK required",
                                          "personalization", "PIN blocked", "illegal", "ready" };

struct app {
    uint8_t type, state, aid_len, aid[32];
};

struct card {
    uint8_t state, error, napps;
    struct app apps[MAX_APPS];
};

struct status {
    uint16_t gw_index[MAX_CARDS];  /* primary and secondary GW sessions: card << 8 | app, or NO_INDEX */
    uint8_t ncards;
    struct card cards[MAX_CARDS];
};

static int sock;
static uint32_t node, port;
static uint16_t txn;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    struct timespec ts;
    va_list ap;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("%5ld.%03ld ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static const char *name(const char *const *names, size_t n, unsigned int i)
{
    return i < n ? names[i] : "?";
}
#define NAME(names, i) name(names, sizeof(names) / sizeof(*names), i)

/* Finds the UIM service: the lookup answers with NEW_SERVER packets, then an empty one. */
static int find_uim(void)
{
    char buf[256];
    struct sockaddr_qrtr sq;
    socklen_t sl;
    struct qrtr_packet pkt;
    int i;

    if (qrtr_new_lookup(sock, UIM_SERVICE, 0, 0) < 0)
        return -1;
    for (i = 0; i < 100; i++) {
        if (qrtr_poll(sock, 100) <= 0)
            continue;
        sl = sizeof(sq);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&sq, &sl);
        if (n < 0 || qrtr_decode(&pkt, buf, n, &sq) < 0 || pkt.type != QRTR_TYPE_NEW_SERVER)
            continue;
        if (pkt.service == UIM_SERVICE) {
            node = pkt.node;
            port = pkt.port;
            return 0;
        }
    }
    return -1;
}

static int send_msg(uint16_t id, const uint8_t *tlvs, uint16_t len)
{
    uint8_t buf[256];

    buf[0] = QMI_REQUEST;
    txn++;
    memcpy(buf + 1, &txn, 2);
    memcpy(buf + 3, &id, 2);
    memcpy(buf + 5, &len, 2);
    memcpy(buf + 7, tlvs, len);
    return qrtr_sendto(sock, node, port, buf, 7 + len);
}

/* Finds a TLV in a QMI message body; returns its value and sets *len. */
static const uint8_t *tlv(const uint8_t *body, size_t size, uint8_t type, uint16_t *len)
{
    size_t off = 0;

    while (off + 3 <= size) {
        uint16_t l;

        memcpy(&l, body + off + 1, 2);
        if (off + 3 + l > size)
            break;
        if (body[off] == type) {
            *len = l;
            return body + off + 3;
        }
        off += 3 + l;
    }
    return NULL;
}

static int parse_status(const uint8_t *p, uint16_t len, struct status *s)
{
    const uint8_t *end = p + len;
    int c, a;

#define NEED(n) do { if (p + (n) > end) return -1; } while (0)
    memset(s, 0, sizeof(*s));
    NEED(9);
    memcpy(&s->gw_index[0], p, 2);       /* GW primary */
    memcpy(&s->gw_index[1], p + 4, 2);   /* GW secondary (the 1x ones are skipped) */
    s->ncards = p[8];
    p += 9;
    for (c = 0; c < s->ncards; c++) {
        struct card *card = &s->cards[c < MAX_CARDS ? c : MAX_CARDS - 1];

        NEED(6);
        card->state = p[0];
        card->error = p[4];
        card->napps = p[5];
        p += 6;
        for (a = 0; a < card->napps; a++) {
            struct app *app = &card->apps[a < MAX_APPS ? a : MAX_APPS - 1];

            NEED(7);
            app->type = p[0];
            app->state = p[1];
            app->aid_len = p[6] < sizeof(app->aid) ? p[6] : sizeof(app->aid);
            NEED(7 + p[6] + 7);
            memcpy(app->aid, p + 7, app->aid_len);
            p += 7 + p[6] + 7;  /* AID, then upin-replaces-pin1 and the PIN1/PIN2 states and retries */
        }
    }
    if (s->ncards > MAX_CARDS)
        s->ncards = MAX_CARDS;
    for (c = 0; c < s->ncards; c++)
        if (s->cards[c].napps > MAX_APPS)
            s->cards[c].napps = MAX_APPS;
    return 0;
#undef NEED
}

static void print_status(const struct status *s)
{
    int c, a;

    for (c = 0; c < s->ncards; c++) {
        const struct card *card = &s->cards[c];

        say("slot %d: card %s%s", c + 1, NAME(card_states, card->state),
            card->state == CARD_ERROR ? (card->error == 0 ? " (power down)" : "") : "");
        for (a = 0; a < card->napps; a++)
            say("  app %d: %s, %s, aid %u bytes", a, NAME(app_types, card->apps[a].type),
                NAME(app_states, card->apps[a].state), card->apps[a].aid_len);
    }
    say("subscriptions: primary %04x, secondary %04x", s->gw_index[0], s->gw_index[1]);
}

/* Waits for the response to message id, handing indications to on_ind on the way. */
static void (*on_ind)(const uint8_t *body, size_t size);

static int wait_response(uint16_t id, uint8_t *body, size_t *size)
{
    uint8_t buf[4096];
    int i;

    for (i = 0; i < 50; i++) {
        uint32_t n_node, n_port;
        uint16_t mid, mlen;

        if (qrtr_poll(sock, 100) <= 0)
            continue;
        int n = qrtr_recvfrom(sock, buf, sizeof(buf), &n_node, &n_port);
        if (n < 7 || n_node != node || n_port != port)
            continue;
        memcpy(&mid, buf + 3, 2);
        memcpy(&mlen, buf + 5, 2);
        if (7 + mlen > n)
            continue;
        if (buf[0] == QMI_INDICATION && on_ind) {
            if (mid == MSG_STATUS_CHANGE_IND)
                on_ind(buf + 7, mlen);
            continue;
        }
        if (buf[0] != QMI_RESPONSE || mid != id)
            continue;
        memcpy(body, buf + 7, mlen);
        *size = mlen;
        return 0;
    }
    return -1;
}

/* Sends a request and returns the QMI error (0 on success, -1 on no answer). */
static int request(uint16_t id, const uint8_t *tlvs, uint16_t len, uint8_t *body, size_t *size)
{
    const uint8_t *r;
    uint16_t rl, result, error;

    if (send_msg(id, tlvs, len) < 0 || wait_response(id, body, size) < 0)
        return -1;
    r = tlv(body, *size, TLV_RESULT, &rl);
    if (!r || rl < 4)
        return -1;
    memcpy(&result, r, 2);
    memcpy(&error, r + 2, 2);
    return result ? error : 0;
}

static void provision(const struct status *s)
{
    int c, a;

    for (c = 0; c < s->ncards; c++) {
        const struct card *card = &s->cards[c];
        uint8_t req[64], body[512];
        size_t size;
        int err, n = 0;

        if (card->state != CARD_PRESENT || s->gw_index[c] != NO_INDEX)
            continue;
        for (a = 0; a < card->napps; a++)
            if (card->apps[a].type == APP_USIM)
                break;
        if (a == card->napps)
            for (a = 0; a < card->napps; a++)
                if (card->apps[a].type == APP_SIM)
                    break;
        if (a == card->napps) {
            say("slot %d: no SIM or USIM application", c + 1);
            continue;
        }
        /* Session change: session type, activate. */
        req[n++] = 0x01; req[n++] = 2; req[n++] = 0; req[n++] = session_for_slot[c]; req[n++] = 1;
        /* Application: slot (1-based), AID. */
        req[n++] = 0x10; req[n++] = 2 + card->apps[a].aid_len; req[n++] = 0;
        req[n++] = c + 1;
        req[n++] = card->apps[a].aid_len;
        memcpy(req + n, card->apps[a].aid, card->apps[a].aid_len);
        n += card->apps[a].aid_len;
        err = request(MSG_CHANGE_PROVISIONING, req, n, body, &size);
        say("slot %d: activating %s as SUB%d: %s (%d)", c + 1, NAME(app_types, card->apps[a].type), c + 1,
            err ? "failed" : "ok", err);
    }
}

static void status_changed(const uint8_t *body, size_t size)
{
    struct status s;
    const uint8_t *p;
    uint16_t len;

    p = tlv(body, size, TLV_CARD_STATUS, &len);
    if (!p || parse_status(p, len, &s) < 0) {
        say("status change: unreadable");
        return;
    }
    say("card status changed");
    print_status(&s);
    provision(&s);
}

static int get_status(struct status *s)
{
    uint8_t body[1024];
    size_t size;
    const uint8_t *p;
    uint16_t len;
    int err = request(MSG_GET_CARD_STATUS, NULL, 0, body, &size);

    if (err) {
        say("card status: error %d", err);
        return -1;
    }
    p = tlv(body, size, TLV_CARD_STATUS, &len);
    if (!p || parse_status(p, len, s) < 0) {
        say("card status: unreadable");
        return -1;
    }
    return 0;
}

/* Registers for card status events, then provisions what the cards have. */
static void attach(void)
{
    uint8_t req[7] = { 0x01, 4, 0, 1, 0, 0, 0 };  /* event mask: card status */
    uint8_t body[512];
    struct status s;
    size_t size;
    int i;

    say("UIM service at %u:%u", node, port);
    on_ind = status_changed;
    i = request(MSG_REGISTER_EVENTS, req, sizeof(req), body, &size);
    say("card status events: %s (%d)", i ? "failed" : "registered", i);
    if (get_status(&s) < 0)
        return;
    print_status(&s);
    provision(&s);
}

int main(int argc, char **argv)
{
    int once = argc > 1 && (!strcmp(argv[1], "status") || !strcmp(argv[1], "powerup"));
    struct status s;
    int i;

    sock = qrtr_open(0);
    if (sock < 0) {
        say("qrtr: %s", strerror(errno));
        return 1;
    }
    /* The modem's services appear a few seconds after it starts. */
    for (i = 0; find_uim() < 0; i++) {
        if (once || i == 60) {
            say("no UIM service");
            return 1;
        }
        sleep(1);
    }
    /* "asteroids_uim powerup <slot>": powers a card slot down and up, so the modem reads the card again. */
    if (argc > 2 && !strcmp(argv[1], "powerup")) {
        uint8_t slot = (uint8_t)atoi(argv[2]), body[256];
        size_t size;
        uint8_t req[4] = { 0x01, 1, 0, slot };
        say("slot %u power down: %d", slot, request(MSG_POWER_DOWN, req, sizeof(req), body, &size));
        sleep(1);
        say("slot %u power up: %d", slot, request(MSG_POWER_UP, req, sizeof(req), body, &size));
        return 0;
    }
    if (once) {
        say("UIM service at %u:%u", node, port);
        if (get_status(&s) < 0)
            return 1;
        print_status(&s);
        return 0;
    }
    attach();
    for (int gone = 0;;) {
        uint8_t buf[4096];
        struct sockaddr_qrtr sq;
        socklen_t sl = sizeof(sq);
        struct qrtr_packet pkt;
        const uint8_t *m;
        uint16_t mid, mlen;
        ssize_t n;

        if (qrtr_poll(sock, -1) <= 0)  /* the socket is non-blocking */
            continue;
        n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&sq, &sl);
        if (n < 0 && errno != EINTR && errno != EAGAIN) {
            say("qrtr: %s", strerror(errno));
            return 1;
        }
        if (n < 0 || qrtr_decode(&pkt, buf, n, &sq) < 0)
            continue;
        /* The lookup stays open: the service going away and coming back (a modem restart). */
        if (pkt.type == QRTR_TYPE_DEL_SERVER && pkt.service == UIM_SERVICE && pkt.node == node && pkt.port == port) {
            say("UIM service gone (modem restart?)");
            gone = 1;
            continue;
        }
        /* It comes back on the same node and port (the modem allocates them the same way each start). */
        if (pkt.type == QRTR_TYPE_NEW_SERVER && pkt.service == UIM_SERVICE &&
            (gone || pkt.node != node || pkt.port != port)) {
            gone = 0;
            node = pkt.node;
            port = pkt.port;
            attach();
            continue;
        }
        if (pkt.type != QRTR_TYPE_DATA || pkt.node != node || pkt.port != port || pkt.data_len < 7)
            continue;
        m = pkt.data;
        memcpy(&mid, m + 3, 2);
        memcpy(&mlen, m + 5, 2);
        if (m[0] == QMI_INDICATION && mid == MSG_STATUS_CHANGE_IND && 7 + mlen <= pkt.data_len)
            status_changed(m + 7, mlen);
    }
}
