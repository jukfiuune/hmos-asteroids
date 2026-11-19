/* SPDX-License-Identifier: MIT
 * asteroids: checks the ADSP's sensor framework (SSC) directly through sheng's backend (sensors/sheng/ssc_backend.c,
 * QMI over QRTR service 400): discovers each sensor kind the backend knows (accel, gyro, mag, ambient light,
 * pressure, hall, proximity), prints its attributes, then reads a few samples (on-change sensors may send none
 * until their value changes). Needs sscrpcd sensorspd running. "ssc_probe [samples]". */
#include "sheng/ssc_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile int got;
static int want = 5;

static void on_sample(const struct SscRawSample *s, void *u)
{
    if (got++ < want)
        printf("SSC_SAMPLE %s n=%u %.3f %.3f %.3f status=%llu ticks=%llu\n", (const char *)u, s->value_count,
            s->values[0], s->values[1], s->values[2], (unsigned long long)s->status,
            (unsigned long long)s->timestamp_ticks);
}

static void on_error(const struct SscBackendError *e, void *u)
{
    printf("SSC_ERROR %s result=%d errno=%d stage=%u qmi=%u/%u sns=%u\n", (const char *)u, e->result, e->system_errno,
        e->stage, e->qmi_result, e->qmi_error, e->sns_result);
}

int main(int argc, char **argv)
{
    if (argc > 1) want = atoi(argv[1]);
    /* "ssc_probe <samples> <kind>": only that kind (its HarmonyOS sensor type number), for 30 s. */
    int only = argc > 2 ? atoi(argv[2]) : 0;
    setbuf(stdout, 0);
    const uint32_t list[] = {SSC_ACCEL, SSC_GYRO, SSC_MAG, SSC_LIGHT, SSC_PRESSURE, SSC_HALL, SSC_PROXIMITY};
    struct { uint32_t kind; const char *name; } kinds[sizeof(list) / sizeof(list[0])];
    for (unsigned i = 0; i < sizeof(list) / sizeof(list[0]); i++) kinds[i].kind = list[i], kinds[i].name = SscKindType(list[i]);
    int fails = 0;
    for (unsigned i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
        if (only && kinds[i].kind != (uint32_t)only) continue;
        SscBackend *b = 0;
        int r = SscBackendCreate(kinds[i].kind, &b);
        if (r) { printf("SSC_CREATE %s %d\n", kinds[i].name, r); fails++; continue; }
        struct SscSensorInfo info = {0};
        r = SscBackendDiscover(b, &info);
        printf("SSC_DISCOVER %s rc=%d name=%s vendor=%s available=%u rates=%u range=%.1f..%.1f\n", kinds[i].name, r,
            info.name, info.vendor, info.available, info.rate_count, info.selected_range.minimum,
            info.selected_range.maximum);
        if (r) { fails++; SscBackendDestroy(b); continue; }
        struct SscBackendConfig cfg = {.requested_rate_hz = 50};
        got = 0;
        r = SscBackendStart(b, &cfg, on_sample, on_error, (void *)kinds[i].name);
        printf("SSC_START %s rc=%d\n", kinds[i].name, r);
        for (int t = 0; !r && got < want && t < (only ? 600 : 60); t++) usleep(50000);
        printf("SSC_GOT %s %d\n", kinds[i].name, got);
        SscBackendStop(b);
        SscBackendDestroy(b);
        if (r || got < want) fails++;
    }
    printf("SSC_RESULT %s\n", fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
