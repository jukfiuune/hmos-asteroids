/* Taps the touchscreen from outside HarmonyOS: writes a one-finger touch-down and lift-off into the touchscreen's own
 * evdev node, which the kernel delivers to every reader of that device as if the panel had reported it. Coordinates
 * are in panel pixels and scaled to the device's ABS_MT ranges.
 *
 *     tap <x> <y> [hold ms]          e.g. tap 540 1800
 *     tap swipe <x0> <y0> <x1> <y1> [ms]  a swipe, 300 ms unless given (a point every 8 ms)
 *     tap key <code> [hold ms]       press a key on the device that has it, e.g. tap key 116 (power)
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define PANEL_W 1080
#define PANEL_H 2392

static int fd;
static struct input_absinfo ax, ay;

static void emit(int type, int code, int value)
{
    struct input_event e = {.type = type, .code = code, .value = value};
    if (write(fd, &e, sizeof(e)) != sizeof(e))
        perror("write");
}

static void at(int x, int y)
{
    emit(EV_ABS, ABS_MT_POSITION_X, ax.minimum + (long)x * (ax.maximum - ax.minimum) / (PANEL_W - 1));
    emit(EV_ABS, ABS_MT_POSITION_Y, ay.minimum + (long)y * (ay.maximum - ay.minimum) / (PANEL_H - 1));
    emit(EV_SYN, SYN_REPORT, 0);
}

static void sleep_ms(int ms)
{
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

static int open_touchscreen(void)
{
    char path[32], name[64];
    for (int i = 0; i < 32; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int f = open(path, O_RDWR | O_CLOEXEC);
        if (f < 0)
            continue;
        if (ioctl(f, EVIOCGABS(ABS_MT_POSITION_X), &ax) == 0 && ioctl(f, EVIOCGABS(ABS_MT_POSITION_Y), &ay) == 0 &&
            ax.maximum > 0 && ay.maximum > 0) {
            if (ioctl(f, EVIOCGNAME(sizeof(name)), name) < 0)
                strcpy(name, "?");
            fprintf(stderr, "tap: %s (%s) x 0..%d y 0..%d\n", path, name, ax.maximum, ay.maximum);
            return f;
        }
        close(f);
    }
    return -1;
}

/* The first input device that reports the key. */
static int open_key_device(int code)
{
    char path[32], name[64];
    unsigned char keys[KEY_MAX / 8 + 1];
    for (int i = 0; i < 32; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int f = open(path, O_RDWR | O_CLOEXEC);
        if (f < 0)
            continue;
        memset(keys, 0, sizeof(keys));
        if (ioctl(f, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 && (keys[code / 8] >> (code % 8)) & 1) {
            if (ioctl(f, EVIOCGNAME(sizeof(name)), name) < 0)
                strcpy(name, "?");
            fprintf(stderr, "tap: %s (%s) key %d\n", path, name, code);
            return f;
        }
        close(f);
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "key")) {
        int code = atoi(argv[2]);
        if (code <= 0 || code > KEY_MAX || (fd = open_key_device(code)) < 0) {
            fputs("tap: no device with that key\n", stderr);
            return 1;
        }
        emit(EV_KEY, code, 1);
        emit(EV_SYN, SYN_REPORT, 0);
        sleep_ms(argc > 3 ? atoi(argv[3]) : 80);
        emit(EV_KEY, code, 0);
        emit(EV_SYN, SYN_REPORT, 0);
        return 0;
    }
    int swipe = (argc == 6 || argc == 7) && !strcmp(argv[1], "swipe");
    if (!swipe && argc != 3 && argc != 4) {
        fputs("usage: tap <x> <y> [hold ms] | tap swipe <x0> <y0> <x1> <y1> [ms] | tap key <code> [hold ms]\n", stderr);
        return 2;
    }
    fd = open_touchscreen();
    if (fd < 0) {
        fputs("tap: no touchscreen\n", stderr);
        return 1;
    }
    int x0 = atoi(argv[swipe + 1]), y0 = atoi(argv[swipe + 2]);
    emit(EV_ABS, ABS_MT_SLOT, 0);
    emit(EV_ABS, ABS_MT_TRACKING_ID, 4242);
    emit(EV_KEY, BTN_TOUCH, 1);
    emit(EV_ABS, ABS_MT_TOUCH_MAJOR, 6);
    at(x0, y0);
    if (swipe) {
        int x1 = atoi(argv[4]), y1 = atoi(argv[5]);
        int steps = (argc == 7 ? atoi(argv[6]) : 300) / 8;
        if (steps < 1)
            steps = 1;
        for (int i = 1; i <= steps; i++) {
            sleep_ms(8);
            at(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
        }
    } else {
        sleep_ms(argc == 4 ? atoi(argv[3]) : 60);
    }
    emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(EV_KEY, BTN_TOUCH, 0);
    emit(EV_SYN, SYN_REPORT, 0);
    return 0;
}
