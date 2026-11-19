#!/usr/bin/env python3
"""Adapts Linaro's rmtfs (github.com/linux-msm/rmtfs) to this kernel; run in the rmtfs checkout.

Built with -DANDROID (no libudev), rmtfs knows only upstream's /dev/qcom_rmtfs_mem1. This downstream kernel exports
the rmtfs carveout through msm_sharedmem instead, as a UIO device named "rmtfs" (/sys/class/uio/uioN, which
Android's rmt_storage looks up by name the same way): open that, with its address and size from maps/map0.
Also: musl has <endian.h>, not <sys/endian.h>.
"""
s = open("sharedmem.c").read()

old = '''static int rmtfs_mem_open_uio(struct rmtfs_mem *rmem, int client_id)
{
	fprintf(stderr, "uio access is not supported on ANDROID yet\\n");
	return -EINVAL;
}'''

new = r'''/* Downstream kernels: msm_sharedmem's UIO device named "rmtfs". */
static int read_hex_file(const char *path, uint64_t *val)
{
	char buf[64];
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -EINVAL;
	buf[n] = '\0';
	*val = strtoull(buf, NULL, 16);
	return 0;
}

static int rmtfs_mem_open_uio(struct rmtfs_mem *rmem, int client_id)
{
	char path[PATH_MAX];
	char name[32];
	struct dirent *de;
	DIR *dir;
	ssize_t n;
	int fd;

	(void)client_id;
	dir = opendir("/sys/class/uio");
	if (!dir)
		return -ENOENT;
	while ((de = readdir(dir)) != NULL) {
		if (strncmp(de->d_name, "uio", 3))
			continue;
		snprintf(path, sizeof(path), "/sys/class/uio/%s/name", de->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = read(fd, name, sizeof(name) - 1);
		close(fd);
		if (n <= 0)
			continue;
		name[n] = '\0';
		if (strcmp(name, "rmtfs\n") && strcmp(name, "rmtfs"))
			continue;

		snprintf(path, sizeof(path), "/sys/class/uio/%s/maps/map0/addr", de->d_name);
		if (read_hex_file(path, &rmem->address) < 0)
			break;
		snprintf(path, sizeof(path), "/sys/class/uio/%s/maps/map0/size", de->d_name);
		if (read_hex_file(path, &rmem->size) < 0)
			break;
		snprintf(path, sizeof(path), "/dev/%s", de->d_name);
		closedir(dir);

		rmem->fd = open(path, O_RDWR);
		if (rmem->fd < 0) {
			fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
			return -errno;
		}
		rmem->base = mmap(0, rmem->size, PROT_READ|PROT_WRITE, MAP_SHARED, rmem->fd, 0);
		if (rmem->base == MAP_FAILED) {
			fprintf(stderr, "failed to mmap %s: %s\n", path, strerror(errno));
			close(rmem->fd);
			return -errno;
		}
		fprintf(stderr, "using %s (rmtfs, %#" PRIx64 " bytes at %#" PRIx64 ")\n", path, rmem->size, rmem->address);
		return 0;
	}
	closedir(dir);
	return -ENOENT;
}'''

if new not in s:
    assert old in s, "rmtfs_mem_open_uio stub not found"
    s = s.replace(old, new)
s = s.replace("#include <sys/endian.h>", "#include <endian.h>")
open("sharedmem.c", "w").write(s)
