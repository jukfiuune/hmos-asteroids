/*
 * wowlan: sets the Wi-Fi device's wake-on-WLAN triggers over nl80211, as "iw phy phy0 wowlan enable any" does.
 *
 * Without triggers the kernel disconnects Wi-Fi at every suspend (cfg80211 leaves all networks), and qcacld's teardown
 * raises its vdev_stop wakeup source, which aborts the suspend: with Wi-Fi on the phone never slept. Android's Wi-Fi
 * HAL sets triggers; here after-boot.sh runs this once the device exists. They belong to the wiphy and last until the
 * driver goes. Tries "any" (wake on anything the firmware passes up), then disconnect + magic packet.
 * Usage: wowlan [phy index] (default 0). Plain generic netlink, no libnl.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
/* The NDK's linux/socket.h redefines musl's sockaddr_storage: skip it, keeping the one type netlink.h needs. */
#define _UAPI_LINUX_SOCKET_H
typedef unsigned short __kernel_sa_family_t;
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>

struct msg {
    struct nlmsghdr n;
    struct genlmsghdr g;
    char attrs[256];
};

static int sock;
static unsigned int seq;

static struct nlattr *put(struct nlmsghdr *n, unsigned short type, const void *data, unsigned short len)
{
    struct nlattr *a = (struct nlattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));

    a->nla_type = type;
    a->nla_len = NLA_HDRLEN + len;
    if (len)
        memcpy((char *)a + NLA_HDRLEN, data, len);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + NLA_ALIGN(a->nla_len);
    return a;
}

static void start(struct msg *m, unsigned short family, unsigned char cmd)
{
    memset(m, 0, sizeof(*m));
    m->n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    m->n.nlmsg_type = family;
    m->n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    m->n.nlmsg_seq = ++seq;
    m->g.cmd = cmd;
    m->g.version = 1;
}

/* Sends m and waits for its acknowledgement; returns 0 or -errno. The family lookup's answer arrives before the ack
 * and is handed to *family. */
static int request(struct msg *m, unsigned short *family)
{
    char buf[8192];

    if (send(sock, m, m->n.nlmsg_len, 0) < 0)
        return -errno;
    for (;;) {
        int len = recv(sock, buf, sizeof(buf), 0);

        if (len < 0)
            return -errno;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned int)len); h = NLMSG_NEXT(h, len)) {
            if (h->nlmsg_seq != m->n.nlmsg_seq)
                continue;
            if (h->nlmsg_type == NLMSG_ERROR)
                return ((struct nlmsgerr *)NLMSG_DATA(h))->error;
            if (family && h->nlmsg_type == GENL_ID_CTRL) {
                struct nlattr *a = (struct nlattr *)((char *)NLMSG_DATA(h) + GENL_HDRLEN);
                int left = h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);

                for (; left >= NLA_HDRLEN && a->nla_len >= NLA_HDRLEN && a->nla_len <= left;
                     left -= NLA_ALIGN(a->nla_len), a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len)))
                    if ((a->nla_type & NLA_TYPE_MASK) == CTRL_ATTR_FAMILY_ID)
                        memcpy(family, (char *)a + NLA_HDRLEN, sizeof(*family));
            }
        }
    }
}

static int set_triggers(unsigned short family, unsigned int phy, const unsigned short *triggers, int count)
{
    struct msg m;
    struct nlattr *nest;

    start(&m, family, NL80211_CMD_SET_WOWLAN);
    put(&m.n, NL80211_ATTR_WIPHY, &phy, sizeof(phy));
    nest = put(&m.n, NL80211_ATTR_WOWLAN_TRIGGERS | NLA_F_NESTED, NULL, 0);
    for (int i = 0; i < count; i++)
        put(&m.n, triggers[i], NULL, 0);  /* flags */
    nest->nla_len = (char *)&m.n + m.n.nlmsg_len - (char *)nest;
    return request(&m, NULL);
}

int main(int argc, char **argv)
{
    static const unsigned short any[] = { NL80211_WOWLAN_TRIG_ANY };
    static const unsigned short fallback[] = { NL80211_WOWLAN_TRIG_DISCONNECT, NL80211_WOWLAN_TRIG_MAGIC_PKT };
    unsigned int phy = argc > 1 ? (unsigned int)atoi(argv[1]) : 0;
    unsigned short family = 0;
    struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
    struct msg m;
    int rc;

    sock = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("wowlan: netlink: %s\n", strerror(errno));
        return 1;
    }
    start(&m, GENL_ID_CTRL, CTRL_CMD_GETFAMILY);
    put(&m.n, CTRL_ATTR_FAMILY_NAME, "nl80211", sizeof("nl80211"));
    rc = request(&m, &family);
    if (rc || !family) {
        printf("wowlan: no nl80211 (%d)\n", rc);
        return 1;
    }
    rc = set_triggers(family, phy, any, 1);
    if (!rc) {
        printf("wowlan: phy%u wakes on any\n", phy);
        return 0;
    }
    printf("wowlan: phy%u \"any\": %s; trying disconnect + magic packet\n", phy, strerror(-rc));
    rc = set_triggers(family, phy, fallback, 2);
    printf("wowlan: phy%u disconnect + magic packet: %s\n", phy, rc ? strerror(-rc) : "set");
    return rc ? 1 : 0;
}
