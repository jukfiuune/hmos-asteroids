/* socketexec: starts an Android daemon in an Android root with the listening sockets init would have made for it (its
 * rc file's socket lines): it chroots into <root>, each NAME:TYPE becomes /dev/socket/NAME (mode 0666) and
 * ANDROID_SOCKET_NAME=<fd> in the environment, and it execs the command. android_get_control_socket wants the socket
 * bound at that very path, so it is bound inside the root. TYPE is stream, dgram or seqpacket. Static OHOS binary, run
 * from M1's shell.
 *   socketexec /camroot tombstoned_crash:seqpacket ... -- /apex/com.android.runtime/bin/linker64 /system/bin/tombstoned */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2 || chroot(argv[1]) || chdir("/")) return perror("chroot"), 1;
    int i = 2;
    for (; i < argc && strcmp(argv[i], "--"); i++) {
        char name[64], type[16], env[96];
        if (sscanf(argv[i], "%63[^:]:%15s", name, type) != 2) return fprintf(stderr, "bad socket %s\n", argv[i]), 1;
        int t = !strcmp(type, "stream") ? SOCK_STREAM : !strcmp(type, "dgram") ? SOCK_DGRAM : SOCK_SEQPACKET;
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        snprintf(a.sun_path, sizeof a.sun_path, "/dev/socket/%s", name);
        int s = socket(AF_UNIX, t, 0);
        unlink(a.sun_path);
        if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || chmod(a.sun_path, 0666) ||
            (t != SOCK_DGRAM && listen(s, 8)))
            return perror(a.sun_path), 1;
        snprintf(env, sizeof env, "ANDROID_SOCKET_%s=%d", name, s);
        putenv(strdup(env));
    }
    if (i + 1 >= argc) return fprintf(stderr, "usage: socketexec <root> NAME:TYPE... -- command\n"), 1;
    execvp(argv[i + 1], argv + i + 1);
    perror(argv[i + 1]);
    return 1;
}
