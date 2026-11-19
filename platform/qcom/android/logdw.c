/* logdw: stands in for Android's logd for the processes in the Android root: listens on the datagram socket liblog
 * writes to (<dir>/logdw) and prints each message as "<time> <tid> <priority> <tag>: <message>". Text logs only
 * (binary event logs are skipped).   logdw <socket dir> [minimum priority, 2 verbose .. 7 fatal]
 * liblog's datagram: android_log_header_t (id u8, tid u16, sec u32, nsec u32, packed), then priority u8, tag, NUL,
 * message, NUL. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: logdw <socket dir> [min priority]\n"); return 2; }
    int min = argc > 2 ? atoi(argv[2]) : 3;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s/logdw", argv[1]);
    unlink(a.sun_path);
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("logdw"); return 1; }
    chmod(a.sun_path, 0666);
    setvbuf(stdout, NULL, _IOLBF, 0);
    static char b[65536];
    const char *p = "??VDIWEFS";
    for (;;) {
        ssize_t n = recv(s, b, sizeof(b) - 1, 0);
        if (n < 13) continue;
        b[n] = 0;
        if (b[0] == 2 || b[0] == 3) continue;  /* events, security */
        uint16_t tid; uint32_t sec, nsec;
        memcpy(&tid, b + 1, 2); memcpy(&sec, b + 3, 4); memcpy(&nsec, b + 7, 4);
        int prio = b[11];
        if (prio < min) continue;
        const char *tag = b + 12, *msg = tag + strlen(tag) + 1;
        if (msg >= b + n) msg = "";
        printf("%u.%03u %u %c %s: %s\n", sec % 100000, nsec / 1000000, tid, p[prio > 8 ? 0 : prio], tag, msg);
    }
}
