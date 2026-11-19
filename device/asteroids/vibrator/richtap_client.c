/* SPDX-License-Identifier: MIT
 * asteroids: a client of the RichTap server (see richtap_client.h). */
#include "richtap_client.h"

#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int RichtapConnect(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", RICHTAP_SOCKET_PATH);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

int32_t RichtapRequest(int fd, int32_t op, int32_t arg, int32_t amplitude)
{
    struct richtap_request r = { op, arg, amplitude };
    int32_t ret;
    if (fd < 0)
        return -EPIPE;
    if (send(fd, &r, sizeof(r), MSG_NOSIGNAL) != sizeof(r) || recv(fd, &ret, sizeof(ret), 0) != sizeof(ret))
        return -EPIPE;
    return ret;
}
