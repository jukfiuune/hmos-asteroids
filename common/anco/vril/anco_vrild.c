// The host end of the Android container's virtual RIL. The guest's rild (libvirtual_ril.so) talks to the host over
// the virt_ril UDS socket; on Huawei phones the host end is libril_msgtransfer.z.so inside the RIL host, in front of
// the Balong modem RIL. Here the phone's RIL stays as it is: this process loads libril_msgtransfer.z.so on its own,
// answers the parameters it reads (the virtual modem switch on, our RIL behind it), and gives it libanco_vril.z.so
// as the modem RIL (VRIL_Init, Android's RIL_Init), which answers the guest from HarmonyOS's state.
// The guest's mobile data is the phone's own connection, shared: libril_msgtransfer answers the guest's data calls
// from the calls the RIL host made through it, so while the phone has one this process activates it once through
// libril_msgtransfer's host side, and our RIL answers with the phone's call. The guest's traffic then reaches the host
// over the container's cellular link (host ancormnet_data0, guest rmnet_data0), which the network broker sets up.
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "hril.h"
#include "hril_request.h"

#define VRIL_LIB "/system/lib64/libanco_vril.z.so"
// libril_msgtransfer.z.so's own dependencies are vendor and chipset libraries.
#define LIB_PATH "/vendor/lib64:/system/lib64:/system/lib64/chipset-pub-sdk:/system/lib64/platformsdk:/system/lib64/chipset-sdk"
#define MT_LIB "/vendor/lib64/libril_msgtransfer.z.so"
#define STATE_PATH "/data/asteroids/modem-state"

static FILE *g_log;

void vril_log(const char *fmt, ...)
{
    if (!g_log) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(g_log, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// The parameters libril_msgtransfer.z.so reads, as on a phone with the virtual modem on; the rest are the real ones.
static const char *const g_props[][2] = {
    { "const.booster.virtual_modem_switch", "true" },
    { "const.product.devicetype", "phone" },
    { "const.telephony.slotCount", "2" },
    { "const.telephony.vsimModemCount", "0" },
    { "ril.lril.path", VRIL_LIB },
    { "ril.lril.args", "" },
    { "persist.distributedmodem.multidevice.enable", "false" },
    // The guest's cellular link has fixed addresses (172.17.0.2/24, the host at .1): put them in the guest's call.
    { "persist.ril.need_modify_anco_link_addr", "true" },
};

typedef int (*PropertyGetFn)(const char *, char *, const char *);

int property_get(const char *key, char *value, const char *def)
{
    if (strcmp(key, "ril.lril.path") == 0 && getenv("ANCO_VRIL_LIB")) {
        snprintf(value, 92, "%s", getenv("ANCO_VRIL_LIB"));
        vril_log("prop %s -> %s (env)", key, value);
        return (int)strlen(value);
    }
    for (size_t i = 0; i < sizeof(g_props) / sizeof(g_props[0]); i++) {
        if (strcmp(key, g_props[i][0]) == 0) {
            snprintf(value, 92, "%s", g_props[i][1]);
            vril_log("prop %s -> %s (ours)", key, value);
            return (int)strlen(value);
        }
    }
    static PropertyGetFn real;
    if (!real) real = (PropertyGetFn)dlsym(RTLD_NEXT, "property_get");
    int n = real ? real(key, value, def) : -1;
    if (n < 0) {
        snprintf(value, 92, "%s", def ? def : "");
        n = (int)strlen(value);
    }
    vril_log("prop %s -> %s", key, value);
    return n;
}

// The RIL host's report callbacks (OpenHarmony's HRilReport, Huawei's with eight entries): the phone's telephony
// isn't behind this instance, so reports are only logged. The last one is the timer libril_msgtransfer uses.
typedef void (*TimerFn)(uint8_t *param);
struct TimerArg {
    TimerFn fn;
    uint8_t *param;
    struct timeval tv;
};

static void *TimerThread(void *p)
{
    struct TimerArg a = *(struct TimerArg *)p;
    free(p);
    if (a.tv.tv_sec || a.tv.tv_usec) usleep((useconds_t)(a.tv.tv_sec * 1000000 + a.tv.tv_usec));
    a.fn(a.param);
    return NULL;
}

static void OnTimer(TimerFn fn, uint8_t *param, const struct timeval *tv)
{
    struct TimerArg *a = calloc(1, sizeof(*a));
    if (!a) return;
    a->fn = fn;
    a->param = param;
    if (tv) a->tv = *tv;
    pthread_t t;
    if (pthread_create(&t, NULL, TimerThread, a) == 0) pthread_detach(t);
}

#define REPORT(name) \
    static void name(int32_t slot, void *a, void *b, void *c) { vril_log("host report " #name " slot %d", slot); }
REPORT(OnCall)
REPORT(OnData)
REPORT(OnModem)
REPORT(OnNetwork)
REPORT(OnSim)
REPORT(OnSms)
REPORT(OnExt)

static void *const g_report[8] = {
    (void *)OnCall, (void *)OnData, (void *)OnModem, (void *)OnNetwork,
    (void *)OnSim, (void *)OnSms, (void *)OnExt, (void *)OnTimer,
};

// The APN of slot 1's call that is up (asteroids_atd's "data=<cid>,<apn>,<^DHCP answer>"), or "".
static void DataApn(char *apn, size_t size)
{
    apn[0] = '\0';
    FILE *f = fopen(STATE_PATH, "r");
    if (f == NULL) return;
    char line[600];
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "data=%*d,%99[^,]", apn) == 1) break;
    fclose(f);
}

// The routing table with a default route through dev (prefix match), -1 if none.
static int DefaultTable(const char *dev)
{
    FILE *p = popen("/system/bin/ip route show table all", "r");
    if (p == NULL) return -1;
    char line[256];
    int table = -1;
    while (table < 0 && fgets(line, sizeof(line), p)) {
        char d[32];
        int t;
        const char *dv = strstr(line, " dev ");
        const char *tb = strstr(line, " table ");
        if (strncmp(line, "default", 7) || dv == NULL || tb == NULL || sscanf(dv, " dev %31s", d) != 1 ||
            sscanf(tb, " table %d", &t) != 1)
            continue;
        if (!strncmp(d, dev, strlen(dev))) table = t;
    }
    pclose(p);
    return table;
}

static void *DataThread(void *p)
{
    const HRilOps *ops = p;
    int serial = 0, active = 0;
    char apn[100];
    for (;;) {
        DataApn(apn, sizeof(apn));
        HRilDataInfo info = { .cid = 1, .rat = 14, .userName = "", .password = "", .apn = apn, .type = "IP",
            .roamingType = "IP" };
        ReqDataInfo req = { .serial = ++serial, .slotId = 0 };
        // On Wi-Fi the guest is on its Wi-Fi network, over the same link: the call isn't shared then.
        int wifi = DefaultTable("wlan0") >= 0;
        if (wifi) apn[0] = '\0';
        if (apn[0] && !active && ops->dataOps && ops->dataOps->ActivatePdpContext) {
            vril_log("phone's data call up (%s): activating it for the guest", apn);
            req.request = HREQ_DATA_ACTIVATE_PDP_CONTEXT;
            ops->dataOps->ActivatePdpContext(&req, &info);
            active = 1;
        } else if (!apn[0] && active && ops->dataOps && ops->dataOps->DeactivatePdpContext) {
            vril_log(wifi ? "phone on Wi-Fi: data call no longer shared" : "phone's data call down");
            req.request = HREQ_DATA_DEACTIVATE_PDP_CONTEXT;
            ops->dataOps->DeactivatePdpContext(&req, &info);
            active = 0;
        }
        sleep(5);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (getenv("ANCO_VRILD") == NULL) {
        setenv("ANCO_VRILD", "1", 1);
        setenv("LD_LIBRARY_PATH", LIB_PATH, 1);
        execv("/proc/self/exe", argv);
    }
    g_log = fopen(argc > 1 ? argv[1] : "/data/local/tmp/anco-vril.txt", "a");
    vril_log("start");
    void *mt = dlopen(MT_LIB, RTLD_NOW | RTLD_GLOBAL);
    if (!mt) {
        vril_log("dlopen %s: %s", MT_LIB, dlerror());
        return 1;
    }
    const void *(*init)(const void *) = (const void *(*)(const void *))dlsym(mt, "RilInitOps");
    if (!init) {
        vril_log("no RilInitOps");
        return 1;
    }
    const HRilOps *ops = (const HRilOps *)init(g_report);
    vril_log("RilInitOps -> %p", (const void *)ops);
    pthread_t t;
    if (ops != NULL && pthread_create(&t, NULL, DataThread, (void *)ops) == 0) pthread_detach(t);
    for (;;) pause();
}
