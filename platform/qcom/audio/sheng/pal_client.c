/* SPDX-License-Identifier: MIT */
#include "pal_client.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>

/* One counter for every request in the process, so replies on a connection are always in sequence order. */
static uint32_t sequence;

int sheng_pal_connect(void)
{
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
    if (fd<0) return -errno;
    struct sockaddr_un a={.sun_family=AF_UNIX};
    memcpy(a.sun_path+1,SHENG_PAL_SOCKET,sizeof(SHENG_PAL_SOCKET)-1);
    socklen_t n=offsetof(struct sockaddr_un,sun_path)+sizeof(SHENG_PAL_SOCKET);
    struct timeval timeout={2,0};
    if (setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)) ||
        setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) ||
        connect(fd,(void *)&a,n)) {
        int rc=-errno; close(fd); return rc;
    }
    return fd;
}

/* asteroids: *broken is set when the link itself failed (send or receive error, a timeout, a reply that doesn't
 * fit): the caller can no longer tell what the server did with the request, so it must drop the connection (the
 * server then closes the stream) and open a new one. A reply that arrives after its request timed out sits in the
 * socket ahead of the next one's; older replies are skipped, so one late reply doesn't put every later request out of
 * step (it did: audio stayed silent until a reboot). */
int sheng_pal_call(int fd, uint32_t op, uint32_t arg0, uint32_t arg1,
    const void *pcm, uint32_t bytes, struct sheng_pal_reply *reply, int *broken)
{
    int dummy;
    if (!broken) broken=&dummy;
    *broken=0;
    if (!reply || bytes>SHENG_PAL_MAX_DATA || (bytes && !pcm)) return -EINVAL;
    uint32_t seq=__atomic_add_fetch(&sequence,1,__ATOMIC_RELAXED);
    struct sheng_pal_request h={SHENG_PAL_MAGIC,op,bytes,seq,arg0,arg1};
    struct iovec iov[2]={{&h,sizeof(h)},{(void *)pcm,bytes}};
    struct msghdr msg={.msg_iov=iov,.msg_iovlen=bytes ? 2 : 1};
    ssize_t n=sendmsg(fd,&msg,MSG_NOSIGNAL);
    if (n<0) { *broken=1; return -errno; }
    if ((size_t)n!=sizeof(h)+bytes) { *broken=1; return -EIO; }
    for (;;) {
        n=recv(fd,reply,sizeof(*reply),MSG_TRUNC);
        if (n<0) { *broken=1; return -errno; }
        if (n==0) { *broken=1; return -ECONNRESET; }
        if (n!=sizeof(*reply) || reply->magic!=SHENG_PAL_MAGIC) { *broken=1; return -EPROTO; }
        if ((int32_t)(reply->sequence-seq)<0) continue;
        if (reply->sequence!=seq || reply->bytes>bytes) { *broken=1; return -EPROTO; }
        return reply->rc;
    }
}

int sheng_pal_request(int fd, uint32_t op, uint32_t arg0, uint32_t arg1,
    const void *pcm, uint32_t bytes, struct sheng_pal_reply *reply)
{
    return sheng_pal_call(fd,op,arg0,arg1,pcm,bytes,reply,0);
}

/* 采集只接收后端返回的真实 PCM，不合成静音或补齐短读 */
int sheng_pal_capture_read(int fd, void *pcm, uint32_t capacity,
    struct sheng_pal_reply *reply)
{
    if (!pcm || !reply || !capacity || capacity>SHENG_PAL_CAPTURE_CHUNK ||
        (capacity&3)) return -EINVAL;
    memset(reply,0,sizeof(*reply));
    uint32_t seq=__atomic_add_fetch(&sequence,1,__ATOMIC_RELAXED);
    struct sheng_pal_request request={SHENG_PAL_MAGIC,SHENG_PAL_CAPTURE_READ,0,seq,capacity,0};
    ssize_t n=send(fd,&request,sizeof(request),MSG_NOSIGNAL);
    if (n<0) return -errno;
    if ((size_t)n!=sizeof(request)) return -EIO;
    struct { struct sheng_pal_reply reply; unsigned char pcm[SHENG_PAL_CAPTURE_CHUNK]; } packet;
    do n=recv(fd,&packet,sizeof(packet),MSG_TRUNC);
    while (n>=(ssize_t)sizeof(packet.reply) && packet.reply.magic==SHENG_PAL_MAGIC &&
        (int32_t)(packet.reply.sequence-seq)<0);  /* a late reply to an earlier request */
    if (n<0) return -errno;
    if ((size_t)n<sizeof(packet.reply) || packet.reply.magic!=SHENG_PAL_MAGIC ||
        packet.reply.sequence!=seq || packet.reply.bytes>capacity || (packet.reply.bytes&3) ||
        (size_t)n!=sizeof(packet.reply)+packet.reply.bytes ||
        (packet.reply.rc && packet.reply.bytes) ||
        (!packet.reply.rc && !packet.reply.bytes)) return -EPROTO;
    *reply=packet.reply;
    if (reply->bytes) memcpy(pcm,packet.pcm,reply->bytes);
    return reply->rc;
}
