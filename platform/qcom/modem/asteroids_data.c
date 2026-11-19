/*
 * asteroids_data: a cellular data call on the modem, without Android's netmgrd.
 *
 * What netmgrd does on this IPA-based SoC, read from Qualcomm's dataipa and datarmnet sources:
 *   1. rmnet_ipa0 up, then its extended ioctls set the ingress and egress data format: that is when the IPA driver
 *      creates the AP's WAN pipes (once per modem lifetime; plain 4-byte QMAP headers), and records mux 1 as
 *      rmnet_data0. Ingress needs AGG_DATA: this DTS receives into pages (qcom,wan-use-skb-page), and without it the
 *      driver keeps per-packet IPA status and parses it from the skb's empty linear head, then skb_pull()s past it:
 *      the first downlink packet hit the BUG in __skb_pull and hung the phone. With it the frames go to rmnet whole.
 *   2. rmnet_data0 on top of it (rtnetlink, kind "rmnet", mux id 1): ingress deaggregation plus this tree's COALESCE
 *      bit (BIT(4) here, not upstream's checksum bit), which picks rmnet's fragment-aware deaggregator for frames
 *      that span page fragments.
 *   3. QMI over QRTR: the Data Port Mapper (DPM) opens the embedded hardware port (interface 1, the IPA pipe pair):
 *      until then WDS refuses to bind any endpoint (internal error). WDS then binds its client to mux 1 on that
 *      endpoint and starts the network on an APN, and reports the address, gateway, DNS and MTU. The call lasts while
 *      the WDS and DPM sockets stay open. (WDA's data format calls are refused on this modem, for every endpoint: the
 *      format comes from the kernel's own IPA handshake.)
 * Several calls run side by side, one process each, on muxes 1, 2... (rmnet_data0, rmnet_data1...): the pipes are set
 * up once, each mux gets its channel and rmnet link. A dual-stack call is two WDS clients bound to the same mux, one
 * started for IPv4 and one for IPv6 (as netmgrd does); if the network grants only one family the call carries that
 * (Vivacom while roaming: 3GPP cause 50, "PDN type IPv4 only allowed", and an IPv6 start on the IPv4 PDN is a policy
 * mismatch, QMI error 79).
 * IP configuration is left to the caller (HarmonyOS's netmanager, or a shell for tests): this prints the settings.
 *
 * Usage: asteroids_data <apn> [4|6|46 [mux [auth user password]]]   auth: 0 none, 1 PAP, 2 CHAP, 3 PAP or CHAP.
 *        Prints IFACE= ADDR= PREFIX= GW= DNS1= DNS2= ADDR6= PREFIX6= GW6= DNS6_1= DNS6_2= MTU= and stays up until
 *        SIGTERM/SIGINT (then stops the call) or the network drops it (exit 2). Logs to stderr.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "libqrtr.h"
/* The NDK's linux/socket.h redefines musl's sockaddr_storage: skip it, keeping the one type netlink.h needs. */
#ifndef _UAPI_LINUX_SOCKET_H
#define _UAPI_LINUX_SOCKET_H
typedef unsigned short __kernel_sa_family_t;
#endif
#include <linux/rtnetlink.h>

#define WDS 0x01
#define WDA 0x1a
#define DPM 0x2f
#define PHYS "rmnet_ipa0"
#define EP_EMBEDDED 4
#define EP_IFACE 1

/* msm_rmnet.h */
#define RMNET_IOCTL_EXTENDED 0x89FD
#define RMNET_IOCTL_ADD_MUX_CHANNEL 0x0005
#define RMNET_IOCTL_SET_EGRESS_DATA_FORMAT 0x0006
#define RMNET_IOCTL_SET_INGRESS_DATA_FORMAT 0x0007
#define RMNET_IOCTL_GET_EP_PAIR 0x0010
#define RMNET_IOCTL_INGRESS_FORMAT_AGG_DATA (1 << 5)
struct rmnet_ioctl_extended {
    uint32_t extended_ioctl;
    union {
        uint32_t data;
        struct {
            uint32_t mux_id;
            uint8_t vchannel_name[16];
        } rmnet_mux_val;
        struct {
            uint32_t data;
            uint32_t agg_size;
            uint32_t agg_count;
        } ingress_format;
        struct {
            uint32_t consumer_pipe_num; /* the uplink pipe (APPS_WAN_PROD) */
            uint32_t producer_pipe_num; /* the downlink pipe (APPS_WAN_CONS) */
        } ipa_ep_pair;
    } u;
};
/* IFLA_RMNET_MUX_ID, IFLA_RMNET_FLAGS and RMNET_FLAGS_INGRESS_DEAGGREGATION (bit 0) from linux/if_link.h match
 * datarmnet's (rmnet_config.c); its higher flag bits don't. */

static volatile sig_atomic_t stopping;
static uint8_t mux = 1;  /* this call's mux and interface, rmnet_data<mux - 1> */
static char iface[IFNAMSIZ] = "rmnet_data0";

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fputs("asteroids_data: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* --- kernel side --- */

static int ifup(int fd, const char *name)
{
    struct ifreq ifr = { 0 };

    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0)
        return -errno;
    if (ifr.ifr_flags & IFF_UP)
        return 0;
    ifr.ifr_flags |= IFF_UP;
    return ioctl(fd, SIOCSIFFLAGS, &ifr) < 0 ? -errno : 0;
}

static int rmnet_ext(int fd, struct rmnet_ioctl_extended *ext)
{
    struct ifreq ifr = { 0 };

    snprintf(ifr.ifr_name, IFNAMSIZ, PHYS);
    ifr.ifr_data = (void *)ext;
    return ioctl(fd, RMNET_IOCTL_EXTENDED, &ifr) < 0 ? -errno : 0;
}

/* The pipes are set up once per modem lifetime, and each mux's channel once: markers per rmnet_ipa0 ifindex (it gets a
 * new one when the modem restarts). */
static int setup_pipes(int fd)
{
    char marker[64], mux_marker[64];
    struct rmnet_ioctl_extended ext;
    unsigned int index = if_nametoindex(PHYS);
    int rc;

    if (!index) {
        say("no %s", PHYS);
        return -ENODEV;
    }
    snprintf(marker, sizeof(marker), "/dev/.asteroids-ipa-pipes-%u", index);
    snprintf(mux_marker, sizeof(mux_marker), "/dev/.asteroids-ipa-mux-%u-%u", index, mux);
    rc = ifup(fd, PHYS);
    if (rc) {
        say("%s up: %s", PHYS, strerror(-rc));
        return rc;
    }
    if (access(marker, F_OK) == 0)
        goto channel;
    memset(&ext, 0, sizeof(ext));
    ext.extended_ioctl = RMNET_IOCTL_SET_INGRESS_DATA_FORMAT;
    ext.u.ingress_format.data = RMNET_IOCTL_INGRESS_FORMAT_AGG_DATA;
    ext.u.ingress_format.agg_size = 16384;
    rc = rmnet_ext(fd, &ext);
    say("ingress data format: %d", rc);
    if (rc)
        return rc;
    memset(&ext, 0, sizeof(ext));
    ext.extended_ioctl = RMNET_IOCTL_SET_EGRESS_DATA_FORMAT;
    say("egress data format: %d (an error can still mean the main pipe is up; see dmesg)", rmnet_ext(fd, &ext));
    close(open(marker, O_CREAT | O_WRONLY | O_CLOEXEC, 0600));
channel:
    if (access(mux_marker, F_OK) == 0)
        return 0;
    memset(&ext, 0, sizeof(ext));
    ext.extended_ioctl = RMNET_IOCTL_ADD_MUX_CHANNEL;
    ext.u.rmnet_mux_val.mux_id = mux;
    memcpy(ext.u.rmnet_mux_val.vchannel_name, iface, strlen(iface) + 1);
    say("mux channel %u: %d", mux, rmnet_ext(fd, &ext));
    close(open(mux_marker, O_CREAT | O_WRONLY | O_CLOEXEC, 0600));
    return 0;
}

static struct rtattr *rta_put(struct nlmsghdr *n, unsigned short type, const void *data, unsigned short len)
{
    struct rtattr *a = (struct rtattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));

    a->rta_type = type;
    a->rta_len = RTA_LENGTH(len);
    if (len)
        memcpy(RTA_DATA(a), data, len);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(a->rta_len);
    return a;
}

static void rta_end(struct nlmsghdr *n, struct rtattr *nest)
{
    nest->rta_len = (char *)n + n->nlmsg_len - (char *)nest;
}

/* rmnet_data<n> over rmnet_ipa0 on its mux, ingress deaggregation through the fragment-aware path. */
static int create_link(void)
{
    struct {
        struct nlmsghdr n;
        struct ifinfomsg i;
        char attrs[256];
    } req = { 0 };
    /* Deaggregation, and datarmnet's COALESCE (its BIT(4); rmnet_private.h), not upstream's checksum bit. */
    const uint32_t f = RMNET_FLAGS_INGRESS_DEAGGREGATION | 1u << 4;
    struct { uint32_t flags, mask; } flags = { f, f };
    uint32_t link = if_nametoindex(PHYS);
    uint16_t mux_id = mux;
    struct rtattr *info, *data;
    char buf[1024];
    int fd, rc = -EIO;

    if (if_nametoindex(iface))
        return 0;
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_type = RTM_NEWLINK;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    req.i.ifi_family = AF_UNSPEC;
    rta_put(&req.n, IFLA_IFNAME, iface, strlen(iface) + 1);
    rta_put(&req.n, IFLA_LINK, &link, sizeof(link));
    info = rta_put(&req.n, IFLA_LINKINFO, NULL, 0);
    rta_put(&req.n, IFLA_INFO_KIND, "rmnet", sizeof("rmnet"));
    data = rta_put(&req.n, IFLA_INFO_DATA, NULL, 0);
    rta_put(&req.n, IFLA_RMNET_MUX_ID, &mux_id, sizeof(mux_id));
    rta_put(&req.n, IFLA_RMNET_FLAGS, &flags, sizeof(flags));
    rta_end(&req.n, data);
    rta_end(&req.n, info);
    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -errno;
    if (send(fd, &req, req.n.nlmsg_len, 0) >= 0 && recv(fd, buf, sizeof(buf), 0) > 0) {
        struct nlmsghdr *h = (struct nlmsghdr *)buf;

        rc = h->nlmsg_type == NLMSG_ERROR ? ((struct nlmsgerr *)NLMSG_DATA(h))->error : 0;
    }
    close(fd);
    return rc;
}

/* --- QMI --- */

struct client {
    int sock;
    uint32_t node, port;
    uint16_t txn;
};

static int qmi_open(struct client *c, unsigned int service)
{
    char buf[256];
    struct sockaddr_qrtr sq;
    socklen_t sl;
    struct qrtr_packet pkt;

    memset(c, 0, sizeof(*c));
    c->sock = qrtr_open(0);
    if (c->sock < 0 || qrtr_new_lookup(c->sock, service, 0, 0) < 0)
        return -1;
    for (int i = 0; i < 100; i++) {
        if (qrtr_poll(c->sock, 100) <= 0)
            continue;
        sl = sizeof(sq);
        ssize_t n = recvfrom(c->sock, buf, sizeof(buf), 0, (struct sockaddr *)&sq, &sl);
        if (n < 0 || qrtr_decode(&pkt, buf, n, &sq) < 0 || pkt.type != QRTR_TYPE_NEW_SERVER)
            continue;
        if (pkt.service == service) {
            c->node = pkt.node;
            c->port = pkt.port;
            return 0;
        }
    }
    return -1;
}

struct tlvbuf {
    uint8_t data[512];
    uint16_t len;
};

static void tlv_add(struct tlvbuf *t, uint8_t type, const void *value, uint16_t len)
{
    t->data[t->len] = type;
    memcpy(t->data + t->len + 1, &len, 2);
    memcpy(t->data + t->len + 3, value, len);
    t->len += 3 + len;
}

static const uint8_t *tlv_get(const uint8_t *body, size_t size, uint8_t type, uint16_t *len)
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

/* Sends a request and waits for its response (indications on the way are dropped). Returns the QMI error, or -1 on
 * no answer; the response body goes to body/size. */
static int qmi_call(struct client *c, uint16_t id, const struct tlvbuf *t, uint8_t *body, size_t *size, int timeout_ms)
{
    uint8_t buf[2048];
    const uint8_t *r;
    uint16_t rl, result, error;

    buf[0] = QMI_REQUEST;
    c->txn++;
    memcpy(buf + 1, &c->txn, 2);
    memcpy(buf + 3, &id, 2);
    memcpy(buf + 5, &t->len, 2);
    memcpy(buf + 7, t->data, t->len);
    if (qrtr_sendto(c->sock, c->node, c->port, buf, 7 + t->len) < 0)
        return -1;
    for (int waited = 0; waited < timeout_ms; waited += 100) {
        uint32_t node, port;
        uint16_t mid, mlen, txn;

        if (qrtr_poll(c->sock, 100) <= 0)
            continue;
        int n = qrtr_recvfrom(c->sock, buf, sizeof(buf), &node, &port);
        if (n < 7 || node != c->node || port != c->port || buf[0] != QMI_RESPONSE)
            continue;
        memcpy(&txn, buf + 1, 2);
        memcpy(&mid, buf + 3, 2);
        memcpy(&mlen, buf + 5, 2);
        if (txn != c->txn || mid != id || 7 + mlen > n)
            continue;
        memcpy(body, buf + 7, mlen);
        *size = mlen;
        r = tlv_get(body, mlen, 0x02, &rl);
        if (!r || rl < 4)
            return -1;
        memcpy(&result, r, 2);
        memcpy(&error, r + 2, 2);
        return result ? error : 0;
    }
    return -1;
}

static int u32_tlv(const uint8_t *body, size_t size, uint8_t type, uint32_t *out)
{
    uint16_t len;
    const uint8_t *p = tlv_get(body, size, type, &len);

    if (!p || len < 4)
        return 0;
    memcpy(out, p, 4);
    return 1;
}

static void ipv4(uint32_t v, char *out)
{
    struct in_addr a = { htonl(v) };

    inet_ntop(AF_INET, &a, out, INET_ADDRSTRLEN);
}

static void ipv6(const uint8_t *v, char *out)
{
    inet_ntop(AF_INET6, v, out, INET6_ADDRSTRLEN);
}

static void stop(int sig)
{
    (void)sig;
    stopping = 1;
}

/* One family of a call: its own WDS client, bound to this call's mux. */
struct call {
    struct client wds;
    uint8_t family;
    uint32_t handle;
    int up;
};

static const struct { uint32_t type, iface; } ep = { EP_EMBEDDED, EP_IFACE };

/* Binds a WDS client to the mux and starts the network on the APN for one IP family. Returns the QMI error. */
static int call_start(struct call *c, const char *apn, uint8_t auth, const char *user, const char *password)
{
    struct tlvbuf t;
    uint8_t body[1024];
    size_t size;
    int rc;

    if (qmi_open(&c->wds, WDS) < 0)
        return -1;
    t.len = 0;
    tlv_add(&t, 0x01, &c->family, 1);
    say("IPv%u: client family: %d", c->family, qmi_call(&c->wds, 0x4D, &t, body, &size, 3000));
    t.len = 0;
    tlv_add(&t, 0x10, &ep, sizeof(ep));
    tlv_add(&t, 0x11, &mux, 1);
    rc = qmi_call(&c->wds, 0xA2, &t, body, &size, 3000);
    say("IPv%u: bind mux %u: %d", c->family, mux, rc);
    if (rc)
        return rc;
    t.len = 0;
    tlv_add(&t, 0x14, apn, strlen(apn));
    if (auth) {
        tlv_add(&t, 0x16, &auth, 1);
        tlv_add(&t, 0x17, user, strlen(user));
        tlv_add(&t, 0x18, password, strlen(password));
    }
    tlv_add(&t, 0x19, &c->family, 1);
    rc = qmi_call(&c->wds, 0x20, &t, body, &size, 60000);
    if (rc) {
        uint16_t len, reason = 0;
        const uint8_t *p = tlv_get(body, size, 0x11, &len);
        int16_t verbose = 0;
        uint16_t vtype = 0;
        if (p && len >= 4) {
            memcpy(&vtype, p, 2);
            memcpy(&verbose, p + 2, 2);
        }
        p = tlv_get(body, size, 0x10, &len);
        if (p && len >= 2)
            memcpy(&reason, p, 2);
        say("IPv%u: start network on \"%s\": error %d, end reason %u, verbose %u/%d", c->family, apn, rc, reason, vtype,
            verbose);
        return rc;
    }
    u32_tlv(body, size, 0x01, &c->handle);
    c->up = 1;
    say("IPv%u: connected on \"%s\" (handle 0x%x)", c->family, apn, c->handle);
    return 0;
}

/* The call's settings for its family: address, gateway, DNS, MTU (current settings: DNS, IP address, gateway, MTU,
 * IP family). */
static void call_settings(struct call *c, char *addr, int *prefix, char *gw, char *dns1, char *dns2, uint32_t *mtu)
{
    struct tlvbuf t;
    uint8_t body[1024];
    size_t size;
    uint32_t mask = 0xA310, v;
    uint16_t len;
    const uint8_t *p;
    int rc;

    t.len = 0;
    tlv_add(&t, 0x10, &mask, 4);
    rc = qmi_call(&c->wds, 0x2D, &t, body, &size, 3000);
    if (rc) {
        say("IPv%u: current settings: %d", c->family, rc);
        return;
    }
    if (c->family == 4) {
        if (u32_tlv(body, size, 0x1E, &v))
            ipv4(v, addr);
        if (u32_tlv(body, size, 0x20, &v))
            ipv4(v, gw);
        if (u32_tlv(body, size, 0x21, &v))
            *prefix = __builtin_popcount(v);
        if (u32_tlv(body, size, 0x15, &v))
            ipv4(v, dns1);
        if (u32_tlv(body, size, 0x16, &v))
            ipv4(v, dns2);
    } else {
        if ((p = tlv_get(body, size, 0x25, &len)) && len >= 17) {
            ipv6(p, addr);
            *prefix = p[16];
        }
        if ((p = tlv_get(body, size, 0x26, &len)) && len >= 16)
            ipv6(p, gw);
        if ((p = tlv_get(body, size, 0x27, &len)) && len >= 16)
            ipv6(p, dns1);
        if ((p = tlv_get(body, size, 0x28, &len)) && len >= 16)
            ipv6(p, dns2);
    }
    if (u32_tlv(body, size, 0x29, &v) && v && (!*mtu || v < *mtu))
        *mtu = v;
}

static void call_stop(struct call *c)
{
    struct tlvbuf t;
    uint8_t body[256];
    size_t size;

    if (!c->up)
        return;
    t.len = 0;
    tlv_add(&t, 0x01, &c->handle, 4);
    say("IPv%u: stop network: %d", c->family, qmi_call(&c->wds, 0x21, &t, body, &size, 3000));
    c->up = 0;
}

/* A packet service status indication saying this client's call ended. */
static int call_ended(struct call *c)
{
    uint8_t buf[1024];
    uint32_t node, port;
    uint16_t mid, mlen, len;
    const uint8_t *p;
    int n = qrtr_recvfrom(c->wds.sock, buf, sizeof(buf), &node, &port);

    if (n < 7 || buf[0] != QMI_INDICATION)
        return 0;
    memcpy(&mid, buf + 3, 2);
    memcpy(&mlen, buf + 5, 2);
    if (mid != 0x22 || 7 + mlen > n)
        return 0;
    p = tlv_get(buf + 7, mlen, 0x01, &len);
    if (!p || len < 1 || p[0] != 1)
        return 0;
    {
        uint16_t reason = 0, vtype = 0;
        int16_t verbose = 0;
        if ((p = tlv_get(buf + 7, mlen, 0x10, &len)) && len >= 2)
            memcpy(&reason, p, 2);
        if ((p = tlv_get(buf + 7, mlen, 0x11, &len)) && len >= 4) {
            memcpy(&vtype, p, 2);
            memcpy(&verbose, p + 2, 2);
        }
        say("IPv%u: call ended: reason %u, verbose %u/%d", c->family, reason, vtype, verbose);
    }
    return 1;
}

int main(int argc, char **argv)
{
    struct client wda, wds, dpm;
    struct call calls[2] = { { .family = 4 }, { .family = 6 } };
    struct tlvbuf t;
    uint8_t body[1024];
    size_t size;
    uint8_t auth = 0;
    const char *families = argc > 2 ? argv[2] : "4", *user = "", *password = "";
    char addr[INET6_ADDRSTRLEN] = "", gw[INET6_ADDRSTRLEN] = "", dns1[INET6_ADDRSTRLEN] = "",
         dns2[INET6_ADDRSTRLEN] = "", addr6[INET6_ADDRSTRLEN] = "", gw6[INET6_ADDRSTRLEN] = "",
         dns6_1[INET6_ADDRSTRLEN] = "", dns6_2[INET6_ADDRSTRLEN] = "";
    uint32_t mtu = 0;
    int prefix = 32, prefix6 = 64, rc, fd, up = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: asteroids_data <apn> [4|6|46 [mux [auth user password]]] | --probe | --probe-bind | --profiles\n");
        return 1;
    }
    if (argc > 3 && atoi(argv[3]) >= 1 && atoi(argv[3]) <= 8)
        mux = (uint8_t)atoi(argv[3]);
    if (argc > 6) {
        auth = (uint8_t)atoi(argv[4]);
        user = argv[5];
        password = argv[6];
    }
    snprintf(iface, sizeof(iface), "rmnet_data%u", mux - 1);
    /* Which data endpoint the modem knows: WDA Get Data Format (changes nothing) on each type and interface. */
    if (!strcmp(argv[1], "--probe")) {
        if (qmi_open(&wda, WDA) < 0)
            return 1;
        t.len = 0;
        say("no endpoint: %d", qmi_call(&wda, 0x21, &t, body, &size, 2000));
        for (uint32_t type = 0; type <= 6; type++)
            for (uint32_t iface = 0; iface <= 9; iface++) {
                struct { uint32_t type, iface; } e = { type, iface };
                uint32_t ll = 0, ul = 0, dl = 0;
                t.len = 0;
                tlv_add(&t, 0x10, &e, sizeof(e));
                rc = qmi_call(&wda, 0x21, &t, body, &size, 2000);
                if (rc)
                    continue;
                u32_tlv(body, size, 0x11, &ll);
                u32_tlv(body, size, 0x12, &ul);
                u32_tlv(body, size, 0x13, &dl);
                say("endpoint %u/%u: link layer %u, uplink %u, downlink %u", type, iface, ll, ul, dl);
            }
        return 0;
    }
    /* The modem's 3GPP profiles, every setting as a raw TLV (PDP type is 0x11: 0 IPv4, 2 IPv6, 3 IPv4v6; APN 0x14). */
    if (!strcmp(argv[1], "--profiles")) {
        uint8_t type = 0;
        uint16_t len;
        const uint8_t *p;
        if (qmi_open(&wds, WDS) < 0)
            return 1;
        t.len = 0;
        tlv_add(&t, 0x10, &type, 1);
        rc = qmi_call(&wds, 0x2A, &t, body, &size, 3000);
        p = tlv_get(body, size, 0x01, &len);
        if (rc || !p || len < 1) {
            say("profile list: %d", rc);
            return 1;
        }
        uint8_t list[256], count = p[0];
        size_t off = 1, n = 0;
        for (int i = 0; i < count && off + 3 <= len; i++) {
            list[n++] = p[off + 1];
            off += 3 + p[off + 2];
        }
        for (size_t i = 0; i < n; i++) {
            uint8_t id[2] = { 0, list[i] };
            t.len = 0;
            tlv_add(&t, 0x01, id, 2);
            rc = qmi_call(&wds, 0x2B, &t, body, &size, 3000);
            printf("profile %u: %d\n", list[i], rc);
            for (size_t o = 0; !rc && o + 3 <= size;) {
                uint16_t l;
                memcpy(&l, body + o + 1, 2);
                if (o + 3 + l > size)
                    break;
                if (body[o] != 0x02) {
                    printf("  0x%02x:", body[o]);
                    for (uint16_t k = 0; k < l && k < 48; k++)
                        printf(" %02x", body[o + 3 + k]);
                    if (body[o] == 0x14 || body[o] == 0x10)
                        printf("  \"%.*s\"", l, (const char *)body + o + 3);
                    printf("\n");
                }
                o += 3 + l;
            }
        }
        return 0;
    }
    /* Which endpoint WDS binds a mux to (each try on a fresh client; a bind lasts only as long as its client). */
    if (!strcmp(argv[1], "--probe-bind")) {
        for (uint32_t type = 0; type <= 6; type++)
            for (uint32_t iface = 0; iface <= 9; iface++) {
                struct { uint32_t type, iface; } e = { type, iface };
                if (qmi_open(&wds, WDS) < 0)
                    return 1;
                t.len = 0;
                tlv_add(&t, 0x10, &e, sizeof(e));
                tlv_add(&t, 0x11, &mux, 1);
                rc = qmi_call(&wds, 0xA2, &t, body, &size, 2000);
                close(wds.sock);
                if (rc != 48)
                    say("bind endpoint %u/%u: %d", type, iface, rc);
            }
        return 0;
    }
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || setup_pipes(fd) < 0)
        return 1;
    rc = create_link();
    say("%s: %s", iface, rc ? strerror(-rc) : "ready");
    if (rc)
        return 1;

    if (qmi_open(&wda, WDA) < 0 || qmi_open(&dpm, DPM) < 0) {
        say("no WDA/DPM service");
        return 1;
    }
    {
        struct rmnet_ioctl_extended ext = { .extended_ioctl = RMNET_IOCTL_GET_EP_PAIR };
        struct __attribute__((packed)) {
            uint8_t count;
            uint32_t type, iface, rx, tx;
        } port = { 1, EP_EMBEDDED, EP_IFACE, 0, 0 };
        rc = rmnet_ext(fd, &ext);
        say("IPA pipes: uplink %u, downlink %u (%d)", ext.u.ipa_ep_pair.consumer_pipe_num,
            ext.u.ipa_ep_pair.producer_pipe_num, rc);
        if (rc)
            return 1;
        port.rx = ext.u.ipa_ep_pair.consumer_pipe_num;
        port.tx = ext.u.ipa_ep_pair.producer_pipe_num;
        t.len = 0;
        tlv_add(&t, 0x11, &port, sizeof(port));
        rc = qmi_call(&dpm, 0x20, &t, body, &size, 3000);
        say("DPM open port %u/%u (pipes %u, %u): %d", port.type, port.iface, port.rx, port.tx, rc);
        if (rc) {
            port.rx = ext.u.ipa_ep_pair.producer_pipe_num;
            port.tx = ext.u.ipa_ep_pair.consumer_pipe_num;
            t.len = 0;
            tlv_add(&t, 0x11, &port, sizeof(port));
            rc = qmi_call(&dpm, 0x20, &t, body, &size, 3000);
            say("DPM open port %u/%u (pipes %u, %u): %d", port.type, port.iface, port.rx, port.tx, rc);
        }
    }
    /* What the modem has now, for the log (link layer, uplink/downlink aggregation protocols). */
    t.len = 0;
    tlv_add(&t, 0x10, &ep, sizeof(ep));
    rc = qmi_call(&wda, 0x21, &t, body, &size, 3000);
    if (!rc) {
        uint32_t ll = 0, ul = 0, dl = 0;
        u32_tlv(body, size, 0x11, &ll);
        u32_tlv(body, size, 0x12, &ul);
        u32_tlv(body, size, 0x13, &dl);
        say("modem data format: link layer %u, uplink aggregation %u, downlink aggregation %u", ll, ul, dl);
    } else {
        say("get data format: %d", rc);
    }
    {
        uint32_t raw_ip = 2, qmap = 5, dgrams = 32, dlsize = 32768;
        t.len = 0;
        tlv_add(&t, 0x11, &raw_ip, 4);
        tlv_add(&t, 0x12, &qmap, 4);
        tlv_add(&t, 0x13, &qmap, 4);
        tlv_add(&t, 0x15, &dgrams, 4);
        tlv_add(&t, 0x16, &dlsize, 4);
        tlv_add(&t, 0x17, &ep, sizeof(ep));
        rc = qmi_call(&wda, 0x20, &t, body, &size, 3000);
        say("set data format (raw IP, QMAP): %d", rc);
    }
    for (int i = 0; i < 2; i++)
        if (strchr(families, '0' + calls[i].family) && !call_start(&calls[i], argv[1], auth, user, password))
            up++;
    if (!up)
        return 1;
    if (calls[0].up)
        call_settings(&calls[0], addr, &prefix, gw, dns1, dns2, &mtu);
    if (calls[1].up)
        call_settings(&calls[1], addr6, &prefix6, gw6, dns6_1, dns6_2, &mtu);
    printf("IFACE=%s\nADDR=%s\nPREFIX=%d\nGW=%s\nDNS1=%s\nDNS2=%s\nADDR6=%s\nPREFIX6=%d\nGW6=%s\nDNS6_1=%s\n"
           "DNS6_2=%s\nMTU=%u\n", iface, addr, prefix, gw, dns1, dns2, addr6, prefix6, gw6, dns6_1, dns6_2,
           mtu ? mtu : 1500);
    fflush(stdout);
    ifup(fd, iface);

    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    rc = 0;
    while (!stopping && !rc) {
        struct pollfd p[2];
        int n = 0, which[2];

        for (int i = 0; i < 2; i++)
            if (calls[i].up) {
                p[n].fd = calls[i].wds.sock;
                p[n].events = POLLIN;
                which[n++] = i;
            }
        if (poll(p, n, 1000) <= 0)
            continue;
        for (int i = 0; i < n; i++)
            if ((p[i].revents & POLLIN) && call_ended(&calls[which[i]])) {
                say("the network ended the IPv%u call", calls[which[i]].family);
                calls[which[i]].up = 0;
                rc = 2;
            }
    }
    for (int i = 0; i < 2; i++)
        call_stop(&calls[i]);
    return rc;
}
