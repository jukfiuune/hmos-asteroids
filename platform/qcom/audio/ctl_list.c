/* SPDX-License-Identifier: MIT
 * asteroids: lists an ALSA card's mixer controls with their current values (no tinymix beside HarmonyOS):
 * "ctl_list [card]" prints "numid name = values" for each control. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sound/asound.h>

int main(int argc, char **argv)
{
    char path[32];
    snprintf(path, sizeof(path), "/dev/snd/controlC%d", argc > 1 ? atoi(argv[1]) : 0);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return 1; }
    struct snd_ctl_elem_list list = {0};
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list)) { perror("list"); return 1; }
    list.space = list.count;
    list.pids = calloc(list.count, sizeof(*list.pids));
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list)) { perror("list"); return 1; }
    for (unsigned i = 0; i < list.used; i++) {
        struct snd_ctl_elem_info info = {.id = list.pids[i]};
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info)) continue;
        printf("%u %s =", info.id.numid, info.id.name);
        struct snd_ctl_elem_value v = {.id = info.id};
        if ((info.access & SNDRV_CTL_ELEM_ACCESS_READ) && !ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v)) {
            unsigned n = info.count > 8 ? 8 : info.count;
            for (unsigned j = 0; j < n; j++) {
                if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
                    struct snd_ctl_elem_info e = info;
                    e.value.enumerated.item = v.value.enumerated.item[j];
                    printf(" %s", ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &e) ? "?" : e.value.enumerated.name);
                } else if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER || info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN)
                    printf(" %ld", v.value.integer.value[j]);
            }
        }
        printf("\n");
    }
    return 0;
}
