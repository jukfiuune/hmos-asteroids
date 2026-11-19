/* SPDX-License-Identifier: MIT
 * asteroids: vibrates from a shell, for checks, through the RichTap server as the vibrator VDI does (HarmonyOS's
 * /dev/unix/socket, so run it in the HarmonyOS root):
 *   asteroids_vibrate <ms> | click | tick [amplitude 1..255] */
#include "richtap_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <ms> | click | tick [amplitude 1..255]\n", argv[0]);
        return 2;
    }
    int32_t amplitude = argc > 2 ? (int32_t)strtol(argv[2], NULL, 0) : 255;
    int fd = RichtapConnect();
    if (fd < 0) {
        fprintf(stderr, "no RichTap server at %s\n", RICHTAP_SOCKET_PATH);
        return 1;
    }
    int32_t ret;
    if (strcmp(argv[1], "click") == 0 || strcmp(argv[1], "tick") == 0)
        ret = RichtapRequest(fd, RICHTAP_EFFECT,
            strcmp(argv[1], "click") == 0 ? RICHTAP_EFFECT_CLICK : RICHTAP_EFFECT_TICK, amplitude);
    else
        ret = RichtapRequest(fd, RICHTAP_ON, (int32_t)strtol(argv[1], NULL, 0), amplitude);
    printf("%s at %d: %d\n", argv[1], amplitude, ret);
    close(fd);
    return ret < 0 ? 1 : 0;
}
