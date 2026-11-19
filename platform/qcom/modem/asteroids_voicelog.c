/*
 * asteroids_voicelog: logs the modem's voice call events, with the end reason AT+CEER doesn't give.
 *
 * A diagnostic: it registers with the modem's voice service (QMI service 9) over QRTR for call, supplementary service,
 * handover, speech codec and DTMF notifications and prints each ALL_CALL_STATUS_IND: call id, state, type, direction,
 * mode, the end reason when a call ends (TLV 0x14, QMI voice's CALL_END_REASON numbering) and the network's
 * diagnostics (0x17); any other indication with its TLVs in hex. Numbers are not printed. It only listens.
 * Usage: asteroids_voicelog (runs until killed; logs to stdout).
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "libqrtr.h"

#define VOICE_SERVICE 9
#define MSG_INDICATION_REGISTER 0x03
#define MSG_ALL_CALL_STATUS_IND 0x2e

static const char *const states[] = { "?", "origination", "incoming", "conversation", "cc in progress", "alerting",
                                      "hold", "waiting", "disconnecting", "end", "setup" };
static const char *const modes[] = { "no srv", "cdma", "gsm", "umts", "lte", "tds", "unknown", "wlan", "nr5g" };

static int sock;
static uint32_t node, port;

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

#define NAME(names, i) ((unsigned int)(i) < sizeof(names) / sizeof(*names) ? names[i] : "?")

static int find_voice(void)
{
    char buf[256];
    struct sockaddr_qrtr sq;
    socklen_t sl;
    struct qrtr_packet pkt;

    if (qrtr_new_lookup(sock, VOICE_SERVICE, 0, 0) < 0)
        return -1;
    for (int i = 0; i < 100; i++) {
        if (qrtr_poll(sock, 100) <= 0)
            continue;
        sl = sizeof(sq);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&sq, &sl);
        if (n < 0 || qrtr_decode(&pkt, buf, n, &sq) < 0 || pkt.type != QRTR_TYPE_NEW_SERVER)
            continue;
        if (pkt.service == VOICE_SERVICE) {
            node = pkt.node;
            port = pkt.port;
            return 0;
        }
    }
    return -1;
}

static const uint8_t *tlv(const uint8_t *body, size_t size, uint8_t type, uint16_t *len)
{
    for (size_t off = 0; off + 3 <= size;) {
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

static void call_status(const uint8_t *body, size_t size)
{
    const uint8_t *p;
    uint16_t len;
    char types[64] = "";

    /* Every TLV type present, so a reason in an unexpected place shows up. */
    for (size_t off = 0; off + 3 <= size;) {
        uint16_t l;
        char t[8];

        memcpy(&l, body + off + 1, 2);
        snprintf(t, sizeof(t), " %02x", body[off]);
        if (strlen(types) < sizeof(types) - sizeof(t))
            strcat(types, t);
        off += 3 + l;
    }
    say("call status (TLVs%s)", types);
    p = tlv(body, size, 0x01, &len);
    if (p && len >= 1)
        for (int i = 0, n = p[0]; i < n && 1 + (i + 1) * 7 <= len; i++) {
            const uint8_t *c = p + 1 + i * 7;
            say("  call %u: %s, type %u, %s, mode %s", c[0], NAME(states, c[1]), c[2], c[3] == 1 ? "MO" : "MT",
                NAME(modes, c[4]));
        }
    p = tlv(body, size, 0x14, &len);
    if (p && len >= 1)
        for (int i = 0, n = p[0]; i < n && 1 + (i + 1) * 3 <= len; i++) {
            uint16_t reason;

            memcpy(&reason, p + 1 + i * 3 + 1, 2);
            say("  call %u end reason %u (0x%x)", p[1 + i * 3], reason, reason);
        }
    p = tlv(body, size, 0x17, &len);
    if (p && len >= 1) {
        char hex[200] = "";
        for (int i = 0; i < len && i < 60; i++)
            snprintf(hex + strlen(hex), sizeof(hex) - strlen(hex), " %02x", p[i]);
        say("  diagnostics:%s", hex);
    }
}

/* Any other indication: its TLVs in hex (the first bytes of each). */
static void other(uint16_t mid, const uint8_t *body, size_t size)
{
    char hex[400];
    int used = snprintf(hex, sizeof(hex), "indication 0x%02x:", mid);
    for (size_t off = 0; off + 3 <= size && used < (int)sizeof(hex) - 40;) {
        uint16_t l;

        memcpy(&l, body + off + 1, 2);
        if (off + 3 + l > size)
            break;
        used += snprintf(hex + used, sizeof(hex) - used, " [%02x]", body[off]);
        for (int i = 0; i < l && i < 16 && used < (int)sizeof(hex) - 8; i++)
            used += snprintf(hex + used, sizeof(hex) - used, "%02x", body[off + 3 + i]);
        off += 3 + l;
    }
    say("%s", hex);
}

int main(void)
{
    uint8_t buf[4096];
    /* Indication register: DTMF (0x10), supplementary service (0x12), call (0x13), handover (0x14) and speech codec
     * (0x15) notifications on. */
    uint8_t req[] = { QMI_REQUEST, 1, 0, MSG_INDICATION_REGISTER, 0, 20, 0, 0x10, 1, 0, 1, 0x12, 1, 0, 1, 0x13, 1, 0, 1,
                      0x14, 1, 0, 1, 0x15, 1, 0, 1 };

    sock = qrtr_open(0);
    if (sock < 0 || find_voice() < 0) {
        say("no voice service");
        return 1;
    }
    say("voice service at %u:%u", node, port);
    qrtr_sendto(sock, node, port, req, sizeof(req));
    for (;;) {
        uint32_t n_node, n_port;
        uint16_t mid, mlen;
        int n;

        if (qrtr_poll(sock, -1) <= 0)
            continue;
        n = qrtr_recvfrom(sock, buf, sizeof(buf), &n_node, &n_port);
        if (n < 7 || n_node != node || n_port != port)
            continue;
        memcpy(&mid, buf + 3, 2);
        memcpy(&mlen, buf + 5, 2);
        if (7 + mlen > n)
            continue;
        if (buf[0] == QMI_RESPONSE && mid == MSG_INDICATION_REGISTER) {
            uint16_t l, r[2] = { 1, 0 };
            const uint8_t *res = tlv(buf + 7, mlen, 0x02, &l);
            if (res && l >= 4)
                memcpy(r, res, 4);
            say("call events: %s (%u)", r[0] ? "failed" : "registered", r[1]);
        } else if (buf[0] == QMI_INDICATION && mid == MSG_ALL_CALL_STATUS_IND) {
            call_status(buf + 7, mlen);
        } else if (buf[0] == QMI_INDICATION) {
            other(mid, buf + 7, mlen);
        }
    }
}
