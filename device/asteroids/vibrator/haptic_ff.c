/* SPDX-License-Identifier: MIT
 * asteroids: the vibration motor through Linux force feedback (see haptic_ff.h). One constant effect is uploaded
 * once and updated in place for each request (a new length and level), then played. */
#include "haptic_ff.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define HAPTIC_NAME "haptic_rt"

static int16_t g_effect = -1;

int HapticOpen(void)
{
    DIR *dir = opendir("/dev/input");
    struct dirent *e;
    int fd = -1;
    if (dir == NULL)
        return -1;
    while (fd < 0 && (e = readdir(dir)) != NULL) {
        char path[64], name[64] = "";
        int f;
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        f = open(path, O_RDWR | O_CLOEXEC);
        if (f < 0)
            continue;
        if (ioctl(f, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && strcmp(name, HAPTIC_NAME) == 0)
            fd = f;
        else
            close(f);
    }
    closedir(dir);
    if (fd >= 0) {
        /* Full gain: the strength goes in each effect's level. */
        struct input_event gain = { .type = EV_FF, .code = FF_GAIN, .value = 0xFFFF };
        if (write(fd, &gain, sizeof(gain)) < 0) {
            /* not fatal: the driver may ignore gain */
        }
        g_effect = -1;
    }
    return fd;
}

int HapticPlay(int fd, uint32_t ms, uint32_t strength)
{
    struct ff_effect effect;
    struct input_event play = { .type = EV_FF, .value = 1 };
    if (fd < 0 || ms == 0)
        return -1;
    if (ms > 0xFFFF)
        ms = 0xFFFF;
    if (strength == 0 || strength > 100)
        strength = 100;
    memset(&effect, 0, sizeof(effect));
    effect.type = FF_CONSTANT;
    effect.id = g_effect; /* -1 the first time: the driver assigns one; later the same one is updated */
    effect.replay.length = (uint16_t)ms;
    effect.u.constant.level = (int16_t)(0x7FFF * strength / 100);
    if (ioctl(fd, EVIOCSFF, &effect) < 0) {
        if (g_effect < 0)
            return -errno;
        /* the driver lost it (reset): upload anew */
        effect.id = -1;
        if (ioctl(fd, EVIOCSFF, &effect) < 0)
            return -errno;
    }
    g_effect = effect.id;
    play.code = (uint16_t)g_effect;
    return write(fd, &play, sizeof(play)) == sizeof(play) ? 0 : -errno;
}

int HapticStop(int fd)
{
    struct input_event stop = { .type = EV_FF, .value = 0 };
    if (fd < 0 || g_effect < 0)
        return 0;
    stop.code = (uint16_t)g_effect;
    return write(fd, &stop, sizeof(stop)) == sizeof(stop) ? 0 : -errno;
}
