/* Marks the running A/B slot as successfully booted, as Android's Qualcomm boot control HAL does after boot, so the
 * bootloader stops spending the slot's retries (and, once they run out, falling back to fastboot).
 *
 *     slot_success          report the slot's state
 *     slot_success mark     also set the slot's "successful" bit
 *
 * ABL keeps the slot state in the attribute flags of the boot_<slot> partition entry (attribute byte 6, bits 48-55):
 * priority (2 bits), active, retries left (3 bits), successful, unbootable. Only the successful bit of the running
 * slot's boot entry changes, in both the primary and the backup table. Every header and entry-array CRC is checked
 * before anything is written, both tables must agree on the entry, and the result is read back. Nothing is written
 * when the bit is already set.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FLAG_ACTIVE 0x04
#define FLAG_SUCCESSFUL 0x40
#define FLAG_UNBOOTABLE 0x80

struct gpt_header {
    char signature[8];
    uint32_t revision, header_size, header_crc, reserved;
    uint64_t current_lba, backup_lba, first_usable, last_usable;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count, entry_size, entries_crc;
} __attribute__((packed));

struct gpt_table {
    uint8_t header_block[8192];
    struct gpt_header *header;
    uint8_t *entries;
    size_t entries_bytes;
    uint64_t lba;
};

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffff;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = crc & 1 ? (crc >> 1) ^ 0xedb88320 : crc >> 1;
    }
    return ~crc;
}

static int slot_suffix(char out[3])
{
    FILE *f = fopen("/proc/bootconfig", "re");
    char line[256];
    int found = 0;
    while (f && fgets(line, sizeof(line), f))
        if (sscanf(line, "androidboot.slot_suffix = \"%2[_ab]\"", out) == 1 && strlen(out) == 2)
            found = 1;
    if (f)
        fclose(f);
    return found ? 0 : -1;
}

/* The disk that holds the partition named name: its sysfs uevent says PARTNAME=<name>. */
static int find_disk(const char *name, char *disk, size_t size)
{
    DIR *d = opendir("/sys/class/block");
    struct dirent *e;
    char path[512], line[128], want[80];
    int found = -1;
    snprintf(want, sizeof(want), "PARTNAME=%s\n", name);
    while (d && (e = readdir(d))) {
        snprintf(path, sizeof(path), "/sys/class/block/%s/uevent", e->d_name);
        FILE *f = fopen(path, "re");
        while (f && fgets(line, sizeof(line), f))
            if (!strcmp(line, want)) {
                /* sde40 -> sde */
                size_t n = strcspn(e->d_name, "0123456789");
                snprintf(disk, size, "/dev/block/%.*s", (int)n, e->d_name);
                found++;
            }
        if (f)
            fclose(f);
    }
    if (d)
        closedir(d);
    return found == 0 ? 0 : -1; /* exactly one match */
}

static int read_table(int fd, unsigned bs, uint64_t lba, struct gpt_table *t)
{
    t->lba = lba;
    if (pread(fd, t->header_block, bs, lba * bs) != (ssize_t)bs)
        return -1;
    t->header = (struct gpt_header *)t->header_block;
    struct gpt_header *h = t->header;
    if (memcmp(h->signature, "EFI PART", 8) || h->header_size < 92 || h->header_size > bs || h->current_lba != lba ||
        h->entry_size != 128 || h->entry_count == 0 || h->entry_count > 1024)
        return -2;
    uint32_t want = h->header_crc;
    h->header_crc = 0;
    uint32_t got = crc32(t->header_block, h->header_size);
    h->header_crc = want;
    if (got != want)
        return -3;
    t->entries_bytes = (size_t)h->entry_count * h->entry_size;
    t->entries = malloc(t->entries_bytes);
    if (!t->entries || pread(fd, t->entries, t->entries_bytes, h->entries_lba * bs) != (ssize_t)t->entries_bytes)
        return -4;
    if (crc32(t->entries, t->entries_bytes) != h->entries_crc)
        return -5;
    return 0;
}

static int find_entry(const struct gpt_table *t, const char *name)
{
    int found = -1;
    for (uint32_t i = 0; i < t->header->entry_count; i++) {
        const uint8_t *e = t->entries + (size_t)i * 128;
        char ascii[37] = {0};
        for (int k = 0; k < 36; k++) {
            uint16_t c = e[56 + 2 * k] | e[57 + 2 * k] << 8;
            if (!c)
                break;
            ascii[k] = c < 128 ? (char)c : '?';
        }
        if (!strcmp(ascii, name)) {
            if (found >= 0)
                return -2;
            found = (int)i;
        }
    }
    return found;
}

static int write_table(int fd, unsigned bs, struct gpt_table *t)
{
    struct gpt_header *h = t->header;
    h->entries_crc = crc32(t->entries, t->entries_bytes);
    h->header_crc = 0;
    h->header_crc = crc32(t->header_block, h->header_size);
    if (pwrite(fd, t->entries, t->entries_bytes, h->entries_lba * bs) != (ssize_t)t->entries_bytes)
        return -1;
    if (pwrite(fd, t->header_block, bs, t->lba * bs) != (ssize_t)bs)
        return -1;
    return fsync(fd);
}

static void describe(const char *what, uint8_t f)
{
    printf("%s flags=%#04x priority=%u active=%u retries=%u successful=%u unbootable=%u\n", what, f, f & 3,
           !!(f & FLAG_ACTIVE), (f >> 3) & 7, !!(f & FLAG_SUCCESSFUL), !!(f & FLAG_UNBOOTABLE));
}

int main(int argc, char **argv)
{
    int mark = argc > 1 && !strcmp(argv[1], "mark");
    char slot[3], name[16], disk[64], sysfs[128];
    if (slot_suffix(slot)) {
        puts("SLOT_SUCCESS no androidboot.slot_suffix");
        return 10;
    }
    snprintf(name, sizeof(name), "boot%s", slot);
    if (find_disk(name, disk, sizeof(disk))) {
        printf("SLOT_SUCCESS no single disk holds %s\n", name);
        return 11;
    }
    unsigned bs = 0;
    snprintf(sysfs, sizeof(sysfs), "/sys/class/block/%s/queue/logical_block_size", disk + 11);
    FILE *f = fopen(sysfs, "re");
    if (!f || fscanf(f, "%u", &bs) != 1 || (bs != 512 && bs != 4096)) {
        printf("SLOT_SUCCESS bad block size for %s\n", disk);
        return 12;
    }
    fclose(f);
    int fd = open(disk, (mark ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_SYNC);
    if (fd < 0 && errno == ENOENT) {
        /* Before the handoff M1's mdev names it /dev/sde; HarmonyOS's ueventd adds /dev/block/sde. */
        memmove(disk + 5, disk + 11, strlen(disk + 11) + 1);
        fd = open(disk, (mark ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_SYNC);
    }
    if (fd < 0) {
        printf("SLOT_SUCCESS open %s: %s\n", disk, strerror(errno));
        return 13;
    }
    struct gpt_table primary, backup;
    int rc = read_table(fd, bs, 1, &primary);
    if (rc) {
        printf("SLOT_SUCCESS primary table of %s invalid (%d)\n", disk, rc);
        return 14;
    }
    rc = read_table(fd, bs, primary.header->backup_lba, &backup);
    if (rc) {
        printf("SLOT_SUCCESS backup table of %s invalid (%d)\n", disk, rc);
        return 15;
    }
    int ip = find_entry(&primary, name), ib = find_entry(&backup, name);
    if (ip < 0 || ib != ip || primary.entries_bytes != backup.entries_bytes ||
        memcmp(primary.entries, backup.entries, primary.entries_bytes)) {
        printf("SLOT_SUCCESS %s missing, or the primary and backup tables differ (%d, %d)\n", name, ip, ib);
        return 16;
    }
    uint8_t *fp = primary.entries + ip * 128 + 54, *fb = backup.entries + ib * 128 + 54;
    printf("SLOT_SUCCESS %s on %s entry %d:\n", name, disk, ip);
    describe("  before", *fp);
    if (!mark)
        return 0;
    if (!(*fp & FLAG_ACTIVE) || (*fp & FLAG_UNBOOTABLE)) {
        puts("SLOT_SUCCESS the running slot is not active and bootable; nothing written");
        return 17;
    }
    if (*fp & FLAG_SUCCESSFUL) {
        puts("SLOT_SUCCESS already marked; nothing written");
        return 0;
    }
    *fp |= FLAG_SUCCESSFUL;
    *fb |= FLAG_SUCCESSFUL;
    /* Backup first: if the primary write is cut short, ABL still finds a valid table. */
    if (write_table(fd, bs, &backup) || write_table(fd, bs, &primary)) {
        printf("SLOT_SUCCESS write failed: %s\n", strerror(errno));
        return 18;
    }
    struct gpt_table check_p, check_b;
    if (read_table(fd, bs, 1, &check_p) || read_table(fd, bs, primary.header->backup_lba, &check_b) ||
        find_entry(&check_p, name) != ip || !(check_p.entries[ip * 128 + 54] & FLAG_SUCCESSFUL) ||
        memcmp(check_p.entries, check_b.entries, check_p.entries_bytes)) {
        puts("SLOT_SUCCESS read-back check failed");
        return 19;
    }
    describe("  after ", check_p.entries[ip * 128 + 54]);
    close(fd);
    return 0;
}
