/* propd: Android's properties for an Android root that has no Android init: the property area bionic's readers map
 * (/dev/__properties__; libbinder clients, for one, wait for servicemanager.ready) and the property service its setters
 * talk to (/dev/socket/property_service; hwservicemanager won't run if it can't set hwservicemanager.disabled).
 * It writes a property_info that puts every property in one context, has bionic create that context's area
 * (__system_property_area_init, as init does) and fills it from build.prop-style files and key=value arguments, a later
 * value replacing an earlier one as in init's import order. Then it serves setprop: ro. properties are set once, ctl.
 * requests (start and stop services) are refused, the rest change.
 * Built as a preload for agmplay, like the other helpers, so it takes its arguments from PROPD, space-separated:
 *   env PROPD="/system/build.prop /vendor/build.prop servicemanager.ready=true" LD_PRELOAD=.../libpropd.so \
 *       chroot <root> linker64 /vendor/bin/agmplay -h */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct prop_info prop_info;
int __system_property_area_init(void);
int __system_property_add(const char *name, unsigned int namelen, const char *value, unsigned int valuelen);
int __system_property_update(prop_info *pi, const char *value, unsigned int len);
const prop_info *__system_property_find(const char *name);

#define DIR "/dev/__properties__"
#define CONTEXT "u:object_r:default_prop:s0"

/* libpropertyinfoparser's layout (system/core/property_service): header, the context and type string tables, and a
 * trie whose root carries the context and type every property falls back to. All offsets are from the start. */
static int write_property_info(void) {
    static uint8_t b[512];
    uint32_t *w = (uint32_t *)b, o = 0;
#define PUT(v) (w[o / 4] = (v), o += 4)
    o = 24;                                        /* header, filled in below */
    uint32_t root_name = o; memcpy(b + o, "root", 5); o += 8;
    uint32_t ctx_str = o; memcpy(b + o, CONTEXT, sizeof CONTEXT); o += (sizeof CONTEXT + 3) & ~3u;
    uint32_t type_str = o; memcpy(b + o, "string", 7); o += 8;
    uint32_t contexts = o; PUT(1); PUT(ctx_str);
    uint32_t types = o; PUT(1); PUT(type_str);
    uint32_t entry = o; PUT(root_name); PUT(4); PUT(0); PUT(0);
    uint32_t root = o; PUT(entry); PUT(0); PUT(0); PUT(0); PUT(0); PUT(0); PUT(0);
    w[0] = 1; w[1] = 1; w[2] = o; w[3] = contexts; w[4] = types; w[5] = root;
    int fd = open(DIR "/property_info", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0444);
    if (fd < 0 || write(fd, b, o) != (ssize_t)o) return -1;
    return close(fd);
}

/* 0 or one of bionic's PROP_ERROR_ codes. `service` is a setprop, which may not change an ro. property. */
static int set(const char *name, size_t nl, const char *value, size_t vl, int service) {
    char key[256];
    int ro = !strncmp(name, "ro.", 3);
    if (!nl || nl >= sizeof key) return 0x10;                                    /* PROP_ERROR_INVALID_NAME */
    if (vl >= 92 && !ro) return 0x14;                                            /* PROP_ERROR_INVALID_VALUE */
    memcpy(key, name, nl); key[nl] = 0;
    const prop_info *pi = __system_property_find(key);
    if (pi && ro && service) return 0x0b;                                        /* PROP_ERROR_READ_ONLY_PROPERTY */
    if (service && !strncmp(key, "ctl.", 4)) return 0x18;                        /* PROP_ERROR_HANDLE_CONTROL_MESSAGE */
    return (pi ? __system_property_update((prop_info *)pi, value, vl) : __system_property_add(key, nl, value, vl))
        ? 0x18 : 0;
}

static int readn(int fd, void *b, size_t n) {
    for (size_t o = 0; o < n;) {
        ssize_t r = read(fd, (char *)b + o, n - o);
        if (r <= 0) return -1;
        o += r;
    }
    return 0;
}

static int readstr(int fd, char *b, size_t cap, size_t *len) {
    uint32_t n;
    if (readn(fd, &n, 4) || n >= cap || readn(fd, b, n)) return -1;
    b[n] = 0;
    *len = n;
    return 0;
}

/* bionic's protocols (system_property_set.cpp): version 2 is the command, the name and the value as length-prefixed
 * strings, answered with the result; version 1 is a fixed 128-byte message with no answer. */
static void serve(void) {
    struct sockaddr_un a = {.sun_family = AF_UNIX, .sun_path = "/dev/socket/property_service"};
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    unlink(a.sun_path);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || chmod(a.sun_path, 0666) || listen(s, 16)) {
        perror("propd: property_service");
        _exit(1);
    }
    fprintf(stderr, "propd: serving\n");
    for (;;) {
        int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) continue;
        struct timeval tv = {2, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        uint32_t cmd;
        static char name[4096], value[4096];
        size_t nl, vl;
        if (!readn(c, &cmd, 4) && cmd == 0x00020001 && !readstr(c, name, sizeof name, &nl) &&
            !readstr(c, value, sizeof value, &vl)) {
            uint32_t r = set(name, nl, value, vl, 1);
            fprintf(stderr, "propd: %s=%s%s\n", name, value, r ? " refused" : "");
            if (write(c, &r, 4) != 4) fprintf(stderr, "propd: no answer sent\n");
        } else if (cmd == 1 && !readn(c, name, 32) && !readn(c, value, 92)) {
            name[31] = value[91] = 0;
            int r = set(name, strlen(name), value, strlen(value), 1);
            fprintf(stderr, "propd: %s=%s%s\n", name, value, r ? " refused" : "");
        }
        close(c);
    }
}

static void line(char *l) {
    l[strcspn(l, "\r\n")] = 0;
    while (*l == ' ' || *l == '\t') l++;
    char *eq = strchr(l, '=');
    if (*l == '#' || !eq || !strncmp(l, "import ", 7)) return;
    char *k = l, *ke = eq, *v = eq + 1;
    while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
    while (*v == ' ' || *v == '\t') v++;
    set(k, ke - k, v, strlen(v), 0);
}

__attribute__((constructor)) static void propd(void) {
    mkdir(DIR, 0755);
    if (write_property_info()) { perror("propd: " DIR "/property_info"); _exit(1); }
    if (__system_property_area_init()) { fprintf(stderr, "propd: area init failed\n"); _exit(1); }
    static char args[4096];
    snprintf(args, sizeof args, "ro.property_service.version=2 %s", getenv("PROPD") ? getenv("PROPD") : "");
    for (char *a = strtok(args, " "); a; a = strtok(NULL, " ")) {
        FILE *f = strchr(a, '=') ? NULL : fopen(a, "re");
        if (f) {
            char l[1024];
            while (fgets(l, sizeof l, f)) line(l);
            fclose(f);
        } else if (strchr(a, '=')) {
            line(a);
        } else {
            perror(a);
        }
    }
    serve();
}
