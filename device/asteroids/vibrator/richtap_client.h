/* SPDX-License-Identifier: MIT
 * asteroids: the RichTap server's requests (richtap_server.c), shared with its clients (vibrator_vdi.cpp,
 * asteroids_vibrate.c). */
#ifndef ASTEROIDS_RICHTAP_CLIENT_H
#define ASTEROIDS_RICHTAP_CLIENT_H

#include <stdint.h>

#define RICHTAP_SOCKET_NAME "asteroids.richtap"
#define RICHTAP_SOCKET_PATH "/dev/unix/socket/" RICHTAP_SOCKET_NAME /* as HarmonyOS sees it */

/* The engine's own effects, as nothing-rt_ics plays them: Android's click, double click, thud, pop and heavy click
 * are the click, its texture tick the tick; both at strength 100, softer only through the amplitude. */
#define RICHTAP_EFFECT_CLICK 12296
#define RICHTAP_EFFECT_TICK 12297
#define RICHTAP_STRENGTH 100

enum {
    RICHTAP_ON = 1,     /* vibrate arg ms at amplitude */
    RICHTAP_EFFECT = 2, /* play effect arg at amplitude */
    RICHTAP_STOP = 3,
};

struct richtap_request {
    int32_t op;
    int32_t arg;
    int32_t amplitude; /* 1..255 */
};

#ifdef __cplusplus
extern "C" {
#endif

/* A connection to the server: the fd, or -1. */
int RichtapConnect(void);
/* Sends a request and waits for its answer: the vibration's length in ms, 0, or a negative error (-EPIPE when the
 * server is gone; connect again). */
int32_t RichtapRequest(int fd, int32_t op, int32_t arg, int32_t amplitude);

#ifdef __cplusplus
}
#endif

#endif
