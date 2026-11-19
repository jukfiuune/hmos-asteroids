/* The hotspot's country code: wifi_manager sets one on the AP interface before it starts hostapd, and gives up on the
 * hotspot if that fails. The donor's vendor HAL (libwifi_hal_hw) sends it as a vendor command of Huawei's Wi-Fi chip,
 * which qcacld doesn't know, so it always failed. Once wifi_host has loaded that HAL, this makes its
 * SetCountryCodeCommand (the wifiSetCountryCode of its function table) return success at once, leaving the regulatory
 * domain to the driver (qcacld's own, from its board data). The code the framework asks for is its default, not the
 * phone's location (CN with a Bulgarian SIM), so applying it would be wrong anyway.
 * The function is patched in memory, after checking its first instructions are the donor build's (sha256 5350feef...
 * of libwifi_hal_hw.z.so); wrapping the HAL's dlsym lookup instead broke wifi_host, whose linker namespaces resolve
 * dlsym by its caller. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#define HAL "/vendor/lib64/libwifi_hal_hw.z.so"
#define SET_COUNTRY_CODE 0x7fe0  /* SetCountryCodeCommand */
static const uint32_t expected[2] = {0xd000008f, 0xf94759ef};  /* adrp x15, ...; ldr x15, [x15, #3760] */
static const uint32_t patch[2] = {0x52800000, 0xd65f03c0};     /* mov w0, #0; ret */

static void note(const char *what)
{
    int fd = open("/data/local/tmp/wifi-country-code.txt", O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return;
    (void)write(fd, what, strlen(what));
    close(fd);
}

static uintptr_t hal_base(void)
{
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (!base && fgets(line, sizeof(line), f)) {
        unsigned long start, offset;
        if (strstr(line, HAL) && sscanf(line, "%lx-%*lx %*s %lx", &start, &offset) == 2 && offset == 0) base = start;
    }
    fclose(f);
    return base;
}

static void *patcher(void *arg)
{
    (void)arg;
    uintptr_t base = 0;
    for (int i = 0; i < 600 && !(base = hal_base()); i++) usleep(100000);  /* the chip service loads it at start */
    if (!base) return NULL;
    uint32_t *code = (uint32_t *)(base + SET_COUNTRY_CODE);
    if (memcmp(code, expected, sizeof(expected))) {
        note("country code: not the expected libwifi_hal_hw, left alone\n");
        return NULL;
    }
    long page = sysconf(_SC_PAGESIZE);
    void *p = (void *)((uintptr_t)code & ~(uintptr_t)(page - 1));
    if (mprotect(p, (size_t)page, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        note("country code: mprotect failed\n");
        return NULL;
    }
    memcpy(code, patch, sizeof(patch));
    __builtin___clear_cache((char *)code, (char *)code + sizeof(patch));
    mprotect(p, (size_t)page, PROT_READ | PROT_EXEC);
    note("country code: left to the driver\n");
    return NULL;
}

__attribute__((constructor)) static void start(void)
{
    pthread_t t;
    if (!pthread_create(&t, NULL, patcher, NULL)) pthread_detach(t);
}
