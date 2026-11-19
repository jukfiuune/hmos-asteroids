/* SPDX-License-Identifier: MIT
 * asteroids: Android's RichTap vibrator engine for HarmonyOS. Nothing's Android plays every vibration through AAC's
 * closed libaacvibrator (vendor), which turns its effect library (/odm/etc/aac_richtap.config) into waveforms for the
 * haptic chip's RichTap device (/dev/aac_richtap); LineageOS and EvoX drive it the same way from NullDebris'
 * hardware/nothing/vibrator (nothing-rt_ics). This does what that HAL does, without Android's binder: it runs in /android
 * under Android's linker (agmplay's __libc_init taken over, as for the PAL server and the Bluetooth bridge), loads the
 * library, and serves our vibrator VDI (vibrator_vdi.cpp) on /dev/unix/socket/asteroids.richtap.
 * The HAL's f0 tracking (libics_haptic) reads the driver's sysfs files, which this kernel (like EvoX's) compiles out,
 * so it is left out; the engine has the chip's f0 from the driver.
 * Requests are struct richtap_request (richtap_client.h); each gets an int32_t back: the vibration's length in ms
 * (on, effect), 0 (stop) or a negative error. */
#include "richtap_client.h"

#include <dlfcn.h>
#include <grp.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_DIR "/hmos-socket" /* HarmonyOS's /dev/unix/socket, bound in by start-richtap.sh */
#define MAX_CLIENTS 4
#define DEV_VIBRATOR_GID 3080     /* HarmonyOS's dev_vibrator, which the vibrator service has */

static int (*vibra_init)(uint32_t *device_type);
static int (*vibra_set_amplitude)(uint8_t amplitude);
static void (*looper_start)(void);
static int32_t (*looper_on)(uint32_t ms);
static int32_t (*looper_prebaked_effect)(uint32_t effect_id, int32_t strength);
static bool (*looper_stop)(void);

static int32_t handle(const struct richtap_request *r)
{
    switch (r->op) {
    case RICHTAP_ON:
        vibra_set_amplitude((uint8_t)r->amplitude);
        return looper_on((uint32_t)r->arg);
    case RICHTAP_EFFECT:
        /* As the HAL's perform(): full amplitude for the effect's own shape, or the composition's scale. */
        vibra_set_amplitude((uint8_t)r->amplitude);
        return looper_prebaked_effect((uint32_t)r->arg, RICHTAP_STRENGTH);
    case RICHTAP_STOP:
        return looper_stop() ? 0 : -1;
    default:
        return -22;
    }
}

static int serve(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s/%s", SOCKET_DIR, RICHTAP_SOCKET_NAME);
    unlink(a.sun_path);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, MAX_CLIENTS)) {
        printf("RICHTAP_SOCKET_ERROR=%s\n", a.sun_path);
        return 1;
    }
    if (chown(a.sun_path, 0, DEV_VIBRATOR_GID) || chmod(a.sun_path, 0660))
        printf("RICHTAP_SOCKET_MODE_ERROR\n");
    printf("RICHTAP_READY=1 pid=%d\n", getpid());

    struct pollfd p[1 + MAX_CLIENTS];
    int n = 1;
    p[0] = (struct pollfd){ .fd = fd, .events = POLLIN };
    for (;;) {
        if (poll(p, n, -1) < 0)
            continue;
        for (int i = n - 1; i >= 1; i--) {
            if (!p[i].revents)
                continue;
            struct richtap_request r;
            if (recv(p[i].fd, &r, sizeof(r), 0) != sizeof(r)) {
                close(p[i].fd);
                p[i] = p[--n];
                continue;
            }
            int32_t ret = handle(&r);
            send(p[i].fd, &ret, sizeof(ret), MSG_NOSIGNAL);
        }
        if (p[0].revents & POLLIN) {
            int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0 && n < 1 + MAX_CLIENTS)
                p[n++] = (struct pollfd){ .fd = c, .events = POLLIN };
            else if (c >= 0)
                close(c);
        }
    }
}

static int server_main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    setbuf(stdout, NULL);
    void *lib = dlopen("libaacvibrator.so", RTLD_NOW);
    if (!lib) {
        printf("RICHTAP_DLOPEN_ERROR=%s\n", dlerror());
        return 80;
    }
#define LOAD(var, name) if (!(*(void **)&var = dlsym(lib, name))) { printf("RICHTAP_NO_SYMBOL=%s\n", name); return 81; }
    LOAD(vibra_init, "aac_vibra_init");
    LOAD(vibra_set_amplitude, "aac_vibra_setAmplitude");
    LOAD(looper_start, "_Z22aac_vibra_looper_startv");
    LOAD(looper_on, "_Z19aac_vibra_looper_onj");
    LOAD(looper_prebaked_effect, "_Z32aac_vibra_looper_prebaked_effectji");
    LOAD(looper_stop, "_Z30aac_vibra_looper_stopPerformHev");
#undef LOAD
    uint32_t type = 0;
    int rc = vibra_init(&type);
    printf("RICHTAP_INIT_RC=%d type=%u\n", rc, type);
    if (rc)
        return 82;
    looper_start();
    return serve();
}

__attribute__((noreturn)) void __libc_init(void *args, void (*onexit)(void),
    int (*original_main)(int, char **, char **), const void *structors)
{
    (void)original_main;
    void (*real)(void *, void (*)(void), int (*)(int, char **, char **), const void *) =
        dlsym(RTLD_NEXT, "__libc_init");
    if (real)
        real(args, onexit, server_main, structors);
    _exit(90);
    __builtin_unreachable();
}
