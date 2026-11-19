/*
 * vendorGnssAdapter.so: HarmonyOS's GNSS vendor library over the modem's own GNSS engine.
 *
 * locationhub asks HDF for gnss_interface_service, which location_host's GNSS driver provides
 * (drivers_peripheral location/gnss/hdi_service, shipped by the donor unchanged as libgnss_interface_service_2.0).
 * That service dlopens "vendorGnssAdapter.so" and takes its GnssVendorInterface (location_vendor_lib.h): the donor's
 * drives Huawei's own GNSS chip. This one drives the engine in the modem through QMI LOC (QRTR service 16), as
 * Android's location stack does here: register for position, satellite, NMEA and engine-state indications, unlock
 * the engine, standalone mode, then Start/Stop a periodic session. LOC requests are answered twice: a response that
 * only says the request was taken, then an indication with the same id carrying the result; one thread reads the
 * socket and turns indications into the service's callbacks. It also follows the service through a modem restart
 * (the lookup reports it gone and back) and starts the session again.
 *
 * The structures below mirror location_vendor_lib.h (C++ there, plain layouts) as far as they are used. Built twice by
 * build-gnss.sh: the library, and with GNSS_TEST a static asteroids_gnss that runs a session from a shell and prints
 * what arrives: asteroids_gnss [seconds].
 */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "libqrtr.h"

#define EXPORT __attribute__((visibility("default")))

/* --- location_vendor_lib.h --- */

enum {
    LAT_VALID = 1 << 0, LONG_VALID = 1 << 1, ALTITUDE_VALID = 1 << 2, SPEED_VALID = 1 << 3, BEARING_VALID = 1 << 4,
    HORIZONTAL_ACCURACY_VALID = 1 << 5, VERTICAL_ACCURACY_VALID = 1 << 6, SPEED_ACCURACY_VALID = 1 << 7,
    BEARING_ACCURACY_VALID = 1 << 8, TIME_VALID = 1 << 9, TIME_SINCE_BOOT_VALID = 1 << 10,
    TIME_UNCERTAINTY_VALID = 1 << 11,
};
enum { CONST_GPS = 1, CONST_SBAS = 2, CONST_GLONASS = 3, CONST_QZSS = 4, CONST_BEIDOU = 5, CONST_GALILEO = 6,
       CONST_IRNSS = 7 };
enum { STATUS_NAVIGATING_BEGIN = 1, STATUS_NAVIGATING_END = 2, STATUS_POWER_ON = 3, STATUS_POWER_OFF = 4 };
enum { SV_USED_IN_FIX = 1 << 2 };
enum { START_NORMAL = 1 };
enum { REF_TIME = 1 };
#define SATELLITE_NUM_MAXIMUM 128

typedef struct {
    size_t size;
    uint32_t fieldValidity;
    double latitude, longitude, altitude;
    float speed, bearing, horizontalAccuracy, verticalAccuracy, speedAccuracy, bearingAccuracy;
    int64_t timeForFix;     /* ms since 1970 */
    uint64_t timeSinceBoot; /* ns, deep sleep included */
    uint64_t timeUncertainty; /* ns */
} GnssLocation;

typedef struct {
    size_t size;
    int16_t satelliteId;
    uint8_t constellationCategory;
    float cn0, elevation, azimuth, carrierFrequency;
    uint32_t satelliteAdditionalInfo;
} SatelliteStatusInfo;

typedef struct {
    size_t size;
    uint32_t satellitesNum;
    SatelliteStatusInfo satellitesList[SATELLITE_NUM_MAXIMUM];
} GnssSatelliteStatus;

typedef struct {
    size_t size;
    void (*locationUpdate)(GnssLocation *location);
    void (*gnssWorkingStatusUpdate)(uint16_t *status);
    void (*satelliteStatusUpdate)(GnssSatelliteStatus *status);
    void (*nmeaUpdate)(int64_t time, const char *nmea, int length);
    void (*capabilitiesUpdate)(uint32_t capabilities);
    void (*requestRefInfo)(int category);
    void (*requestExtendedEphemeris)(void);
} GnssBasicCallbackIfaces; /* the service's has more after these; only this prefix is read */

typedef struct {
    size_t size;
    GnssBasicCallbackIfaces gnssCallback;
} GnssCallbackStruct;

typedef struct {
    size_t size;
    int64_t time; /* ms since 1970 */
    int64_t elapsedRealtime;
    int uncertaintyOfTime;
} GnssRefTime;

typedef struct {
    size_t size;
    int category;
    union {
        GnssRefTime time;
        GnssLocation gnssLocation;
    } u;
} GnssReferenceInfo;

typedef struct {
    size_t size;
    uint32_t startCategory;
    union {
        struct {
            size_t size;
            uint32_t minInterval; /* ms */
            int gnssMode;
        } gnssBasicConfig;
    } u;
} GnssConfigParameter;

typedef struct {
    size_t size;
    int (*enableGnss)(GnssCallbackStruct *callbacks);
    int (*disableGnss)(void);
    int (*startGnss)(uint32_t category);
    int (*stopGnss)(uint32_t category);
    int (*injectsGnssReferenceInfo)(int category, GnssReferenceInfo *info);
    int (*setGnssConfigPara)(GnssConfigParameter *para);
    void (*removeAuxiliaryData)(uint16_t flags);
    int (*injectExtendedEphemeris)(char *data, int length);
    int (*getCachedLocationsSize)(void);
    void (*flushCachedGnssLocations)(void);
    const void *(*getGnssModuleIface)(int iface);
} VendorInterface;

typedef struct {
    size_t size;
    const VendorInterface *(*getGnssInterface)(void);
} GnssVendorDevice;

/* --- logging: hilog in the library, stderr in the test tool --- */

#ifdef GNSS_TEST
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#else
int HiLogPrint(int type, int level, unsigned int domain, const char *tag, const char *fmt, ...);
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    HiLogPrint(3 /* LOG_CORE */, 4 /* LOG_INFO */, 0xD002300, "AsteroidsGnss", "%{public}s", buf);
}
#endif

/* --- QMI LOC --- */

#define LOC_SERVICE 16
#define QMI_REQUEST 0
#define QMI_RESPONSE 2
#define QMI_INDICATION 4
#define LOC_INFORM_CLIENT_REVISION 0x20
#define LOC_REGISTER_EVENTS 0x21
#define LOC_START 0x22
#define LOC_STOP 0x23
#define LOC_POSITION_REPORT 0x24
#define LOC_SV_INFO 0x25
#define LOC_NMEA 0x26
#define LOC_INJECT_TIME_REQUEST 0x28
#define LOC_ENGINE_STATE 0x2B
#define LOC_INJECT_UTC_TIME 0x38
#define LOC_SET_ENGINE_LOCK 0x3A
#define LOC_SET_OPERATION_MODE 0x4A
#define SESSION_ID 1

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static GnssBasicCallbackIfaces g_cb;
static int g_sock = -1, g_up, g_navigating;
static uint32_t g_node, g_port, g_interval = 1000;
static uint16_t g_txn;
static pthread_t g_thread;
/* The satellites the last fix used, as the modem numbers them. */
static uint16_t g_used[64];
static int g_usedCount;

struct tlvbuf {
    uint8_t data[256];
    uint16_t len;
};

static void TlvAdd(struct tlvbuf *t, uint8_t type, const void *value, uint16_t len)
{
    t->data[t->len] = type;
    memcpy(t->data + t->len + 1, &len, 2);
    memcpy(t->data + t->len + 3, value, len);
    t->len += 3 + len;
}

static const uint8_t *TlvGet(const uint8_t *body, size_t size, uint8_t type, uint16_t *len)
{
    for (size_t off = 0; off + 3 <= size;) {
        uint16_t l;
        memcpy(&l, body + off + 1, 2);
        if (off + 3 + l > size)
            break;
        if (body[off] == type) {
            *len = l;
            return body + off + 3;
        }
        off += 3 + l;
    }
    return NULL;
}

static int TlvCopy(const uint8_t *body, size_t size, uint8_t type, void *out, uint16_t want)
{
    uint16_t len;
    const uint8_t *p = TlvGet(body, size, type, &len);
    if (p == NULL || len < want)
        return 0;
    memcpy(out, p, want);
    return 1;
}

/* Sends a request; its outcome comes back as an indication, which the reader logs. Called with g_lock held. */
static void LocSend(uint16_t id, const struct tlvbuf *t)
{
    uint8_t buf[300];
    if (!g_up)
        return;
    buf[0] = QMI_REQUEST;
    g_txn++;
    memcpy(buf + 1, &g_txn, 2);
    memcpy(buf + 3, &id, 2);
    memcpy(buf + 5, &t->len, 2);
    memcpy(buf + 7, t->data, t->len);
    if (qrtr_sendto(g_sock, g_node, g_port, buf, 7 + t->len) < 0)
        say("LOC request 0x%02x: %s", id, strerror(errno));
}

static void LocU32(uint16_t id, uint32_t value)
{
    struct tlvbuf t = { .len = 0 };
    TlvAdd(&t, 0x01, &value, 4);
    LocSend(id, &t);
}

/* A periodic session: fixes at the interval, high accuracy, final reports only. */
static void LocStart(void)
{
    struct tlvbuf t = { .len = 0 };
    uint8_t session = SESSION_ID;
    uint32_t periodic = 1, high = 3, intermediate = 2;
    TlvAdd(&t, 0x01, &session, 1);
    TlvAdd(&t, 0x10, &periodic, 4);
    TlvAdd(&t, 0x11, &high, 4);
    TlvAdd(&t, 0x12, &intermediate, 4);
    TlvAdd(&t, 0x13, &g_interval, 4);
    LocSend(LOC_START, &t);
    say("session started (every %u ms)", g_interval);
}

static void LocStop(void)
{
    struct tlvbuf t = { .len = 0 };
    uint8_t session = SESSION_ID;
    TlvAdd(&t, 0x01, &session, 1);
    LocSend(LOC_STOP, &t);
    say("session stopped");
}

static void InjectTime(int64_t utcMs, uint32_t uncertaintyMs)
{
    struct tlvbuf t = { .len = 0 };
    uint64_t utc = (uint64_t)utcMs;
    TlvAdd(&t, 0x01, &utc, 8);
    TlvAdd(&t, 0x02, &uncertaintyMs, 4);
    LocSend(LOC_INJECT_UTC_TIME, &t);
}

/* The service (back): what to report, no engine lock, standalone (no assistance server here). Set NMEA Types is
 * NOT_SUPPORTED on this modem (newer Qualcomm stacks build NMEA on the AP), so NMEA may never come. */
static void LocSetup(void)
{
    struct tlvbuf t = { .len = 0 };
    /* position, satellites, NMEA, time requests, engine state, fix session state */
    uint64_t mask = 1 << 0 | 1 << 1 | 1 << 2 | 1 << 4 | 1 << 7 | 1 << 8;
    uint32_t clientType = 1;
    LocU32(LOC_INFORM_CLIENT_REVISION, 80);
    /* This modem refuses the registration without a client type: INVALID_ARGUMENT for any mask, and Start then fails
     * with GENERAL_ERROR. (Its client string TLV is shorter than libqmi's: ARGUMENT_TOO_LONG; not sent.) */
    TlvAdd(&t, 0x01, &mask, 8);
    TlvAdd(&t, 0x11, &clientType, 4);
    LocSend(LOC_REGISTER_EVENTS, &t);
    LocU32(LOC_SET_ENGINE_LOCK, 1 /* none */);
    LocU32(LOC_SET_OPERATION_MODE, 4 /* standalone */);
    if (g_navigating)
        LocStart();
}

static int64_t NowMs(clockid_t clock)
{
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Reports arrive every second while navigating: a line when the value changes, else at most every 30 s. */
static int Worth(int64_t *last, int *lastValue, int value)
{
    int64_t now = NowMs(CLOCK_BOOTTIME);
    if (value == *lastValue && now - *last < 30000)
        return 0;
    *last = now;
    *lastValue = value;
    return 1;
}

static GnssBasicCallbackIfaces Callbacks(void)
{
    pthread_mutex_lock(&g_lock);
    GnssBasicCallbackIfaces cb = g_cb;
    pthread_mutex_unlock(&g_lock);
    return cb;
}

static void ReportStatus(uint16_t status)
{
    GnssBasicCallbackIfaces cb = Callbacks();
    if (cb.gnssWorkingStatusUpdate != NULL)
        cb.gnssWorkingStatusUpdate(&status);
}

static void OnPosition(const uint8_t *b, size_t n)
{
    GnssLocation loc = { .size = sizeof(GnssLocation) };
    uint32_t status = 1;
    uint64_t utc;
    float f;
    uint16_t len;
    const uint8_t *p;
    if (!TlvCopy(b, n, 0x01, &status, 4) || status != 0)
        return; /* in progress or failed: only fixes are reported */
    if (!TlvCopy(b, n, 0x10, &loc.latitude, 8) || !TlvCopy(b, n, 0x11, &loc.longitude, 8))
        return;
    loc.fieldValidity = LAT_VALID | LONG_VALID;
    if (TlvCopy(b, n, 0x1A, &f, 4)) /* above the WGS 84 ellipsoid */
        loc.altitude = f, loc.fieldValidity |= ALTITUDE_VALID;
    if (TlvCopy(b, n, 0x18, &loc.speed, 4))
        loc.fieldValidity |= SPEED_VALID;
    if (TlvCopy(b, n, 0x20, &loc.bearing, 4))
        loc.fieldValidity |= BEARING_VALID;
    if (TlvCopy(b, n, 0x12, &loc.horizontalAccuracy, 4))
        loc.fieldValidity |= HORIZONTAL_ACCURACY_VALID;
    if (TlvCopy(b, n, 0x1C, &loc.verticalAccuracy, 4))
        loc.fieldValidity |= VERTICAL_ACCURACY_VALID;
    if (TlvCopy(b, n, 0x19, &loc.speedAccuracy, 4))
        loc.fieldValidity |= SPEED_ACCURACY_VALID;
    if (TlvCopy(b, n, 0x21, &loc.bearingAccuracy, 4))
        loc.fieldValidity |= BEARING_ACCURACY_VALID;
    loc.timeForFix = TlvCopy(b, n, 0x25, &utc, 8) ? (int64_t)utc : NowMs(CLOCK_REALTIME);
    loc.timeSinceBoot = (uint64_t)NowMs(CLOCK_BOOTTIME) * 1000000;
    loc.fieldValidity |= TIME_VALID | TIME_SINCE_BOOT_VALID;
    if (TlvCopy(b, n, 0x28, &f, 4)) /* ms */
        loc.timeUncertainty = (uint64_t)(f * 1e6f), loc.fieldValidity |= TIME_UNCERTAINTY_VALID;
    pthread_mutex_lock(&g_lock);
    g_usedCount = 0;
    if ((p = TlvGet(b, n, 0x2C, &len)) != NULL && len >= 1)
        for (int i = 0; i < p[0] && 1 + 2 * i + 2 <= len && g_usedCount < 64; i++)
            memcpy(&g_used[g_usedCount++], p + 1 + 2 * i, 2);
    pthread_mutex_unlock(&g_lock);
    static int64_t lastFixLog;
    static int lastFixUsed = -1;
    if (Worth(&lastFixLog, &lastFixUsed, g_usedCount))
        say("fix %.6f %.6f +-%.0f m, %d satellites", loc.latitude, loc.longitude, loc.horizontalAccuracy, g_usedCount);
    GnssBasicCallbackIfaces cb = Callbacks();
    if (cb.locationUpdate != NULL)
        cb.locationUpdate(&loc);
}

/* The modem's satellite numbers to the constellation's own (as Android's Qualcomm HAL does): GLONASS 65-96,
 * BeiDou 201-263, Galileo 301-336 and NavIC 401-414 are offset; GPS, SBAS and QZSS stand as they are. */
static void SatelliteId(uint32_t system, uint16_t id, int16_t *outId, uint8_t *outConst)
{
    switch (system) {
    case 1: *outConst = CONST_GPS; break;
    case 2: *outConst = CONST_GALILEO; break;
    case 3: *outConst = CONST_SBAS; break;
    case 4: case 6: *outConst = CONST_BEIDOU; break;
    case 5: *outConst = CONST_GLONASS; break;
    case 7: *outConst = CONST_QZSS; break;
    case 8: *outConst = CONST_IRNSS; break;
    default: *outConst = 0; break;
    }
    if (id >= 65 && id <= 96)
        id -= 64;
    else if (id >= 201 && id <= 263)
        id -= 200;
    else if (id >= 301 && id <= 336)
        id -= 300;
    else if (id >= 401 && id <= 414)
        id -= 400;
    *outId = (int16_t)id;
}

static void OnSatellites(const uint8_t *b, size_t n)
{
    static GnssSatelliteStatus st;
    uint16_t len;
    const uint8_t *p = TlvGet(b, n, 0x10, &len);
    int tracked = 0;
    static int64_t lastLog;
    static int lastInView = -1;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    if (p == NULL || len < 1)
        len = 0; /* none in view (the engine searching): reported as an empty list */
    pthread_mutex_lock(&g_lock);
    /* valid u32, system u32, id u16, health u8, status u32, navigation data u8, elevation, azimuth, SNR (floats) */
    for (int i = 0; len > 0 && i < p[0] && 1 + 28 * (i + 1) <= len && st.satellitesNum < SATELLITE_NUM_MAXIMUM; i++) {
        const uint8_t *e = p + 1 + 28 * i;
        uint32_t valid, system, status;
        uint16_t id;
        SatelliteStatusInfo *s = &st.satellitesList[st.satellitesNum];
        memcpy(&valid, e, 4);
        memcpy(&system, e + 4, 4);
        memcpy(&id, e + 8, 2);
        memcpy(&status, e + 11, 4);
        memcpy(&s->elevation, e + 16, 4);
        memcpy(&s->azimuth, e + 20, 4);
        memcpy(&s->cn0, e + 24, 4);
        if (!(valid & 2))
            continue;
        s->size = sizeof(*s);
        SatelliteId(system, id, &s->satelliteId, &s->constellationCategory);
        for (int k = 0; k < g_usedCount; k++)
            if (g_used[k] == id)
                s->satelliteAdditionalInfo |= SV_USED_IN_FIX;
        tracked += status == 2;
        st.satellitesNum++;
    }
    pthread_mutex_unlock(&g_lock);
    if (Worth(&lastLog, &lastInView, (int)st.satellitesNum * 100 + tracked))
        say("satellites: %u in view, %d tracked", st.satellitesNum, tracked);
    GnssBasicCallbackIfaces cb = Callbacks();
    if (cb.satelliteStatusUpdate != NULL)
        cb.satelliteStatusUpdate(&st);
}

static void OnNmea(const uint8_t *b, size_t n)
{
    uint16_t len;
    const uint8_t *p = TlvGet(b, n, 0x01, &len);
    char nmea[512];
    if (p == NULL)
        return;
    if (len >= sizeof(nmea))
        len = sizeof(nmea) - 1;
    memcpy(nmea, p, len);
    nmea[len] = '\0';
    GnssBasicCallbackIfaces cb = Callbacks();
#ifdef GNSS_TEST
    fputs(nmea, stdout);
    if (len == 0 || nmea[strlen(nmea) - 1] != '\n')
        fputc('\n', stdout);
    fflush(stdout);
#endif
    if (cb.nmeaUpdate != NULL)
        cb.nmeaUpdate(NowMs(CLOCK_REALTIME), nmea, (int)strlen(nmea));
}

static void OnIndication(uint16_t id, const uint8_t *b, size_t n)
{
    uint32_t u32;
    int32_t state;
    switch (id) {
    case LOC_POSITION_REPORT:
        OnPosition(b, n);
        break;
    case LOC_SV_INFO:
        OnSatellites(b, n);
        break;
    case LOC_NMEA:
        OnNmea(b, n);
        break;
    case LOC_ENGINE_STATE:
        if (TlvCopy(b, n, 0x01, &state, 4)) {
            say("engine %s", state == 1 ? "on" : "off");
            ReportStatus(state == 1 ? STATUS_POWER_ON : STATUS_POWER_OFF);
        }
        break;
    case LOC_INJECT_TIME_REQUEST:
        /* The system clock (network or NTP time) as a starting point. */
        pthread_mutex_lock(&g_lock);
        InjectTime(NowMs(CLOCK_REALTIME), 1000);
        pthread_mutex_unlock(&g_lock);
        say("time injected on the engine's request");
        break;
    default:
        /* A request's outcome: status 0 is success. */
        if (TlvCopy(b, n, 0x01, &u32, 4) && u32 != 0)
            say("LOC 0x%02x: status %u", id, u32);
        break;
    }
}

static void *Reader(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t buf[4096];
        struct sockaddr_qrtr sq;
        socklen_t sl = sizeof(sq);
        struct qrtr_packet pkt;
        struct pollfd pfd = { g_sock, POLLIN, 0 };
        uint16_t id, len, result[2];
        const uint8_t *m;
        if (poll(&pfd, 1, -1) <= 0)
            continue;
        ssize_t n = recvfrom(g_sock, buf, sizeof(buf), 0, (struct sockaddr *)&sq, &sl);
        if (n < 0 || qrtr_decode(&pkt, buf, n, &sq) < 0)
            continue;
        if (pkt.type == QRTR_TYPE_NEW_SERVER && pkt.service == LOC_SERVICE) {
            pthread_mutex_lock(&g_lock);
            g_node = pkt.node;
            g_port = pkt.port;
            g_up = 1;
            say("LOC service at %u:%u", g_node, g_port);
            LocSetup();
            pthread_mutex_unlock(&g_lock);
            continue;
        }
        if (pkt.type == QRTR_TYPE_DEL_SERVER && pkt.service == LOC_SERVICE) {
            pthread_mutex_lock(&g_lock);
            g_up = 0;
            pthread_mutex_unlock(&g_lock);
            say("LOC service gone (modem restart?)");
            continue;
        }
        if (pkt.type != QRTR_TYPE_DATA || pkt.node != g_node || pkt.port != g_port || pkt.data_len < 7)
            continue;
        m = pkt.data;
        memcpy(&id, m + 3, 2);
        memcpy(&len, m + 5, 2);
        if (7 + (size_t)len > pkt.data_len)
            continue;
#ifdef GNSS_TEST
        if (getenv("GNSS_RAW")) { /* every message from the service, for debugging */
            uint16_t txn;
            memcpy(&txn, m + 1, 2);
            fprintf(stderr, "< type %u txn %u id 0x%02x:", m[0], txn, id);
            for (int i = 0; i < len && i < 64; i++)
                fprintf(stderr, " %02x", m[7 + i]);
            fputc('\n', stderr);
        }
#endif
        if (m[0] == QMI_INDICATION)
            OnIndication(id, m + 7, len);
        else if (m[0] == QMI_RESPONSE && TlvCopy(m + 7, len, 0x02, result, 4) && result[0] != 0)
            say("LOC request 0x%02x: QMI error %u", id, result[1]);
    }
    return NULL;
}

/* --- the vendor interface --- */

static int EnableGnss(GnssCallbackStruct *callbacks)
{
    int rc = 0;
    pthread_mutex_lock(&g_lock);
    if (callbacks != NULL)
        g_cb = callbacks->gnssCallback;
    /* The socket and its reader stay for the process's life: the service calls in here under its own lock, which
     * the reader's callbacks take too, so the reader is never waited for. */
    if (g_sock < 0) {
        g_sock = qrtr_open(0);
        if (g_sock < 0 || qrtr_new_lookup(g_sock, LOC_SERVICE, 0, 0) < 0 ||
            pthread_create(&g_thread, NULL, Reader, NULL) != 0) {
            say("no QRTR: %s", strerror(errno));
            if (g_sock >= 0)
                close(g_sock);
            g_sock = -1;
            rc = -1;
        } else {
            pthread_detach(g_thread);
        }
    }
    pthread_mutex_unlock(&g_lock);
    say("enabled");
    return rc;
}

static int DisableGnss(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_navigating)
        LocStop();
    g_navigating = 0;
    memset(&g_cb, 0, sizeof(g_cb));
    pthread_mutex_unlock(&g_lock);
    say("disabled");
    return 0;
}

static int StartGnss(uint32_t category)
{
    if (category != START_NORMAL)
        return -1; /* no batching (GNSS cache) */
    pthread_mutex_lock(&g_lock);
    g_navigating = 1;
    LocStart();
    pthread_mutex_unlock(&g_lock);
    ReportStatus(STATUS_NAVIGATING_BEGIN);
    return 0;
}

static int StopGnss(uint32_t category)
{
    if (category != START_NORMAL)
        return -1;
    pthread_mutex_lock(&g_lock);
    g_navigating = 0;
    LocStop();
    pthread_mutex_unlock(&g_lock);
    ReportStatus(STATUS_NAVIGATING_END);
    return 0;
}

static int InjectReferenceInfo(int category, GnssReferenceInfo *info)
{
    if (info == NULL || category != REF_TIME)
        return 0;
    pthread_mutex_lock(&g_lock);
    InjectTime(info->u.time.time, info->u.time.uncertaintyOfTime > 0 ? (uint32_t)info->u.time.uncertaintyOfTime : 1000);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int SetConfigPara(GnssConfigParameter *para)
{
    if (para != NULL && para->startCategory == START_NORMAL && para->u.gnssBasicConfig.minInterval >= 100) {
        pthread_mutex_lock(&g_lock);
        g_interval = para->u.gnssBasicConfig.minInterval;
        pthread_mutex_unlock(&g_lock);
    }
    return 0;
}

static void RemoveAuxiliaryData(uint16_t flags)
{
    (void)flags;
}

static int InjectExtendedEphemeris(char *data, int length)
{
    (void)data;
    (void)length;
    return 0;
}

static int GetCachedLocationsSize(void)
{
    return 0;
}

static void FlushCachedLocations(void)
{
}

/* No AGNSS, geofencing, network-initiated or measurement modules. */
static const void *GetModuleIface(int iface)
{
    (void)iface;
    return NULL;
}

static const VendorInterface g_interface = {
    sizeof(VendorInterface), EnableGnss, DisableGnss, StartGnss, StopGnss, InjectReferenceInfo, SetConfigPara,
    RemoveAuxiliaryData, InjectExtendedEphemeris, GetCachedLocationsSize, FlushCachedLocations, GetModuleIface,
};

static const VendorInterface *GetGnssInterface(void)
{
    return &g_interface;
}

EXPORT const GnssVendorDevice GnssVendorInterface = { sizeof(GnssVendorDevice), GetGnssInterface };

#ifdef GNSS_TEST
static void PrintStatus(uint16_t *status)
{
    printf("# working status %u\n", *status);
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 60;
    GnssCallbackStruct cb = { .size = sizeof(cb) };
    cb.gnssCallback.gnssWorkingStatusUpdate = PrintStatus;
    if (EnableGnss(&cb) < 0)
        return 1;
    sleep(1);
    StartGnss(START_NORMAL);
    sleep(seconds > 0 ? seconds : 60);
    StopGnss(START_NORMAL);
    sleep(1);
    DisableGnss();
    return 0;
}
#endif
