/* SPDX-License-Identifier: MIT
 * asteroids: sets a utilization floor (uclamp.min, 0..1024) on every thread of the given processes, so the CPU
 * frequency governor (WALT) runs them as if at least that busy. "uclamp <min> <pid>..."; prints the threads set.
 * Threads created later inherit it. */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct sched_attr {
    uint32_t size, sched_policy;
    uint64_t sched_flags;
    int32_t sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime, sched_deadline, sched_period;
    uint32_t sched_util_min, sched_util_max;
};
#define SCHED_FLAG_KEEP_POLICY 0x08
#define SCHED_FLAG_KEEP_PARAMS 0x10
#define SCHED_FLAG_UTIL_CLAMP_MIN 0x20

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "uclamp <min 0..1024> <pid>...\n"); return 2; }
    unsigned min = strtoul(argv[1], NULL, 0);
    int failed = 0;
    for (int i = 2; i < argc; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/task", argv[i]);
        DIR *dir = opendir(path);
        if (!dir) { perror(path); failed = 1; continue; }
        int count = 0;
        for (struct dirent *e; (e = readdir(dir));) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            struct sched_attr attr = {.size = sizeof(attr),
                .sched_flags = SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS | SCHED_FLAG_UTIL_CLAMP_MIN,
                .sched_util_min = min};
            if (syscall(SYS_sched_setattr, atoi(e->d_name), &attr, 0)) { perror(e->d_name); failed = 1; }
            else ++count;
        }
        closedir(dir);
        printf("%s: %d threads uclamp.min=%u\n", argv[i], count, min);
    }
    return failed;
}
