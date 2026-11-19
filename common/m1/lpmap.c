// SPDX-License-Identifier: MIT
// lpmap: maps logical partitions from super onto device-mapper, for M1's init (busybox has no dmsetup, and the
// HarmonyOS images live in super; see common/m1/super/). It reads Android's LP metadata (liblp's on-disk format, version 10)
// from the given slot's metadata copy and creates one read-only dm-linear device per partition, with its node at
// /dev/mapper/<name>.
//
//   lpmap -l <super>                       lists the partitions and their extents
//   lpmap <super> <suffix> [name...]       maps <name><suffix> for each name (every partition with that suffix if none)
//   lpmap -d <dm name>                     removes a mapping
//
// Exits 1 if any partition asked for is missing or did not map; the others are still mapped.
#include <errno.h>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define LP_RESERVED 4096        // LP_PARTITION_RESERVED_BYTES
#define LP_GEOMETRY_SIZE 4096   // LP_METADATA_GEOMETRY_SIZE; the geometry and its backup follow the reserved bytes
#define LP_GEOMETRY_MAGIC 0x616c4467
#define LP_HEADER_MAGIC 0x414C5030
#define LP_TARGET_LINEAR 0

struct table { uint32_t offset, num, size; };
struct part { char name[37]; uint32_t first_extent, num_extents; };
struct extent { uint64_t sectors, target; uint32_t type, source; };

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static struct part *parts;
static struct extent *extents;
static uint32_t nparts, nextents;

static int load(const char *dev, int slot)
{
    int fd = open(dev, O_RDONLY);
    if (fd < 0) { perror(dev); return -1; }
    uint8_t g[LP_GEOMETRY_SIZE];
    if (pread(fd, g, sizeof(g), LP_RESERVED) != sizeof(g) || u32(g) != LP_GEOMETRY_MAGIC) {
        fprintf(stderr, "%s: no LP geometry\n", dev);
        close(fd);
        return -1;
    }
    uint32_t max = u32(g + 40), slots = u32(g + 44);  // after magic, struct_size and the 32-byte checksum
    if (slot >= (int)slots) slot = 0;
    off_t at = LP_RESERVED + 2 * LP_GEOMETRY_SIZE + (off_t)slot * max;
    uint8_t *m = malloc(max);
    if (!m || pread(fd, m, max, at) != (ssize_t)max || u32(m) != LP_HEADER_MAGIC || (m[4] | m[5] << 8) != 10) {
        fprintf(stderr, "%s: no LP metadata (slot %d)\n", dev, slot);
        close(fd);
        return -1;
    }
    close(fd);
    uint32_t header = u32(m + 8), tables = u32(m + 44);
    if (header + tables > max) { fprintf(stderr, "%s: metadata larger than its slot\n", dev); return -1; }
    const uint8_t *t = m + header;
    struct table pt = { u32(m + 80), u32(m + 84), u32(m + 88) }, et = { u32(m + 92), u32(m + 96), u32(m + 100) };
    if (pt.offset + (uint64_t)pt.num * pt.size > tables || et.offset + (uint64_t)et.num * et.size > tables) {
        fprintf(stderr, "%s: bad table descriptors\n", dev);
        return -1;
    }
    nparts = pt.num;
    nextents = et.num;
    parts = calloc(nparts + 1, sizeof(*parts));
    extents = calloc(nextents + 1, sizeof(*extents));
    for (uint32_t i = 0; i < nparts; i++) {
        const uint8_t *p = t + pt.offset + i * pt.size;
        memcpy(parts[i].name, p, 36);
        parts[i].first_extent = u32(p + 40);
        parts[i].num_extents = u32(p + 44);
    }
    for (uint32_t i = 0; i < nextents; i++) {
        const uint8_t *e = t + et.offset + i * et.size;
        extents[i] = (struct extent){ u64(e), u64(e + 12), u32(e + 8), u32(e + 20) };
    }
    free(m);
    return 0;
}

static void dm_init(struct dm_ioctl *io, size_t size, const char *name)
{
    memset(io, 0, sizeof(*io));
    io->version[0] = DM_VERSION_MAJOR;
    io->data_size = size;
    io->data_start = sizeof(*io);
    snprintf(io->name, sizeof(io->name), "%s", name);
}

static int map(int ctl, const char *super, const struct part *p)
{
    struct stat st;
    if (stat(super, &st) < 0) { perror(super); return -1; }
    if (p->num_extents == 0 || p->first_extent + p->num_extents > nextents) {
        fprintf(stderr, "%s: no extents\n", p->name);
        return -1;
    }
    size_t size = sizeof(struct dm_ioctl) + p->num_extents * (sizeof(struct dm_target_spec) + 64);
    struct dm_ioctl *io = calloc(1, size);
    dm_init(io, sizeof(*io), p->name);
    if (ioctl(ctl, DM_DEV_CREATE, io) < 0) {
        fprintf(stderr, "%s: create: %s\n", p->name, strerror(errno));
        free(io);
        return -1;
    }
    dev_t dev = io->dev;
    dm_init(io, size, p->name);
    io->flags = DM_READONLY_FLAG;
    io->target_count = p->num_extents;
    uint8_t *at = (uint8_t *)io + sizeof(*io);
    uint64_t start = 0;
    for (uint32_t i = 0; i < p->num_extents; i++) {
        const struct extent *e = &extents[p->first_extent + i];
        struct dm_target_spec *s = (struct dm_target_spec *)at;
        if (e->type != LP_TARGET_LINEAR || e->source != 0) {
            fprintf(stderr, "%s: extent %u isn't linear on super\n", p->name, i);
            free(io);
            return -1;
        }
        s->sector_start = start;
        s->length = e->sectors;
        snprintf(s->target_type, sizeof(s->target_type), "linear");
        char *params = (char *)(s + 1);
        snprintf(params, 64, "%u:%u %llu", major(st.st_rdev), minor(st.st_rdev), (unsigned long long)e->target);
        s->next = sizeof(*s) + 64;
        at += s->next;
        start += e->sectors;
    }
    if (ioctl(ctl, DM_TABLE_LOAD, io) < 0) {
        fprintf(stderr, "%s: table: %s\n", p->name, strerror(errno));
        free(io);
        return -1;
    }
    dm_init(io, sizeof(*io), p->name);  // resume: makes the loaded table live
    if (ioctl(ctl, DM_DEV_SUSPEND, io) < 0) {
        fprintf(stderr, "%s: resume: %s\n", p->name, strerror(errno));
        free(io);
        return -1;
    }
    free(io);
    char node[96];
    mkdir("/dev/mapper", 0755);
    snprintf(node, sizeof(node), "/dev/mapper/%s", p->name);
    unlink(node);
    if (mknod(node, S_IFBLK | 0600, dev) < 0) { perror(node); return -1; }
    printf("%s: %llu sectors in %u extent(s), dm %u:%u\n", node, (unsigned long long)start, p->num_extents,
           major(dev), minor(dev));
    return 0;
}

static int control(void)
{
    int ctl = open("/dev/mapper/control", O_RDWR);
    if (ctl < 0) perror("/dev/mapper/control");
    return ctl;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "-l")) {
        if (load(argv[2], 0)) return 1;
        for (uint32_t i = 0; i < nparts; i++) {
            printf("%s", parts[i].name);
            for (uint32_t j = 0; j < parts[i].num_extents && parts[i].first_extent + j < nextents; j++) {
                const struct extent *e = &extents[parts[i].first_extent + j];
                printf(" %llu+%llu", (unsigned long long)e->target, (unsigned long long)e->sectors);
            }
            printf("\n");
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "-d")) {
        int ctl = control();
        if (ctl < 0) return 1;
        struct dm_ioctl io;
        dm_init(&io, sizeof(io), argv[2]);
        if (ioctl(ctl, DM_DEV_REMOVE, &io) < 0) { fprintf(stderr, "%s: %s\n", argv[2], strerror(errno)); return 1; }
        char node[160];
        snprintf(node, sizeof(node), "/dev/mapper/%s", argv[2]);
        unlink(node);
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: lpmap -l <super> | lpmap <super> <suffix> [name...] | lpmap -d <dm name>\n");
        return 2;
    }
    const char *super = argv[1], *suffix = argv[2];
    // Android's slot numbering: metadata copy 0 for _a, 1 for _b.
    if (load(super, !strcmp(suffix, "_b"))) return 1;
    int ctl = control();
    if (ctl < 0) return 1;
    int rc = 0;
    if (argc == 3) {
        size_t n = strlen(suffix);
        for (uint32_t i = 0; i < nparts; i++) {
            size_t l = strlen(parts[i].name);
            if (l > n && !strcmp(parts[i].name + l - n, suffix) && parts[i].num_extents && map(ctl, super, &parts[i]))
                rc = 1;
        }
        return rc;
    }
    for (int a = 3; a < argc; a++) {
        char want[64];
        snprintf(want, sizeof(want), "%s%s", argv[a], suffix);
        uint32_t i;
        for (i = 0; i < nparts && strcmp(parts[i].name, want); i++)
            ;
        if (i == nparts) {
            fprintf(stderr, "%s: not in super\n", want);
            rc = 1;
        } else if (map(ctl, super, &parts[i])) {
            rc = 1;
        }
    }
    return rc;
}
