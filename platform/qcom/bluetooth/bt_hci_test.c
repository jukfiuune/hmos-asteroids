/* SPDX-License-Identifier: MIT
 * asteroids: test client for sheng's Bluetooth bridge, run as root beside HarmonyOS: connects to sheng.bt.h4 (which
 * powers the chip and runs Qualcomm's init), then sends HCI Reset, Read Local Version and Read BD_ADDR and prints the
 * events. "bt_hci_test [socket]" (default /hmos-root/dev/unix/socket/sheng.bt.h4). */
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int event(int fd, unsigned char *buf, int timeout_ms)
{
    struct pollfd p = {fd, POLLIN, 0};
    int n = 0, want = 3;
    while (n < want) {
        if (poll(&p, 1, timeout_ms) <= 0) return -1;
        int k = recv(fd, buf + n, want - n, 0);
        if (k <= 0) return -1;
        n += k;
        if (n == 3 && buf[0] == 4) want = 3 + buf[2];
    }
    return n;
}

static void command(int fd, const char *name, const unsigned char *cmd, int len)
{
    unsigned char buf[300];
    if (send(fd, cmd, len, 0) != len) { printf("BT_TEST_%s_SEND_FAILED\n", name); return; }
    for (int tries = 0; tries < 5; tries++) {
        int n = event(fd, buf, 3000);
        if (n < 0) { printf("BT_TEST_%s_TIMEOUT\n", name); return; }
        printf("BT_TEST_%s_EVENT", name);
        for (int i = 0; i < n; i++) printf(" %02x", buf[i]);
        printf("\n");
        if (buf[1] == 0x0e && n >= 7 && buf[4] == cmd[1] && buf[5] == cmd[2]) return;
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/hmos-root/dev/unix/socket/sheng.bt.h4";
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a))) { perror("connect"); return 1; }
    char hello[4];
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 30000) <= 0 || recv(fd, hello, 4, MSG_WAITALL) != 4) { puts("BT_TEST_NO_HELLO"); return 2; }
    printf("BT_TEST_HELLO=%.4s\n", hello);
    if (memcmp(hello, "SBT1", 4)) return 3;
    const unsigned char reset[] = {1, 0x03, 0x0c, 0}, version[] = {1, 0x01, 0x10, 0}, addr[] = {1, 0x09, 0x10, 0};
    command(fd, "RESET", reset, sizeof(reset));
    command(fd, "VERSION", version, sizeof(version));
    command(fd, "BDADDR", addr, sizeof(addr));
    close(fd);
    return 0;
}
