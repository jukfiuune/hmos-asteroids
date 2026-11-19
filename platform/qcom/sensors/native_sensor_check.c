/* SPDX-License-Identifier: MIT
 * asteroids: sensors through HarmonyOS's public sensor API, as an app would: lists what the sensors service offers,
 * then reads the accelerometer, gyro, magnetometer, ambient light and proximity for a moment each. "native_sensor_check [log] [samples]"; run as the init
 * service asteroids_sensor_check (a root shell has no token, so subscribing is refused with 201). */
#include <sensors/oh_sensor.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile int got;
static int want = 3;

static void on_event(Sensor_Event *e)
{
    Sensor_Type t;
    float *d = 0;
    uint32_t n = 0;
    OH_SensorEvent_GetType(e, &t);
    OH_SensorEvent_GetData(e, &d, &n);
    if (got++ < want && d && n >= 1)
        printf("SENSOR_EVENT type=%d n=%u %.3f %.3f %.3f\n", t, n, d[0], n > 1 ? d[1] : 0, n > 2 ? d[2] : 0);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    if (argc > 2) want = atoi(argv[2]);
    setbuf(stdout, 0);
    uint32_t count = 0;
    Sensor_Result r = OH_Sensor_GetInfos(0, &count);
    printf("SENSOR_COUNT rc=%d count=%u\n", r, count);
    Sensor_Info **infos = count ? OH_Sensor_CreateInfos(count) : 0;
    if (infos && OH_Sensor_GetInfos(infos, &count) == SENSOR_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            char name[64] = {0}, vendor[64] = {0};
            uint32_t nl = sizeof(name), vl = sizeof(vendor);
            Sensor_Type t = 0;
            float res = 0;
            OH_SensorInfo_GetName(infos[i], name, &nl);
            OH_SensorInfo_GetVendorName(infos[i], vendor, &vl);
            OH_SensorInfo_GetType(infos[i], &t);
            OH_SensorInfo_GetResolution(infos[i], &res);
            printf("SENSOR_INFO type=%d name=%s vendor=%s resolution=%g\n", t, name, vendor, res);
        }
    }
    int fails = 0;
    const Sensor_Type types[] = {SENSOR_TYPE_ACCELEROMETER, SENSOR_TYPE_GYROSCOPE, SENSOR_TYPE_MAGNETIC_FIELD,
        SENSOR_TYPE_AMBIENT_LIGHT, SENSOR_TYPE_PROXIMITY};
    for (int i = 0; i < (int)(sizeof(types) / sizeof(types[0])); i++) {
        Sensor_SubscriptionId *id = OH_Sensor_CreateSubscriptionId();
        Sensor_SubscriptionAttribute *at = OH_Sensor_CreateSubscriptionAttribute();
        Sensor_Subscriber *sub = OH_Sensor_CreateSubscriber();
        OH_SensorSubscriptionId_SetType(id, types[i]);
        OH_SensorSubscriptionAttribute_SetSamplingInterval(at, 20000000);
        OH_SensorSubscriber_SetCallback(sub, on_event);
        got = 0;
        r = OH_Sensor_Subscribe(id, at, sub);
        printf("SENSOR_SUBSCRIBE type=%d rc=%d\n", types[i], r);
        for (int t = 0; !r && got < want && t < 40; t++) usleep(50000);
        OH_Sensor_Unsubscribe(id, sub);
        if (r || got < want) fails++;
        OH_Sensor_DestroySubscriber(sub);
        OH_Sensor_DestroySubscriptionAttribute(at);
        OH_Sensor_DestroySubscriptionId(id);
    }
    printf("SENSOR_RESULT %s\n", fails ? "FAIL" : "OK");
    return fails;
}
