/* asteroids: registers the donor's unmodified power HDI (libpower_interface_service_1.3, hosted on the phone by
 * power_host, which we don't start) as power_interface_service, with its original Binder stub from libpower_driver.
 * powermgr suspends the device through it: on SLEEP it calls StartSuspend, and the implementation writes the
 * wakeup count and "mem" to /sys/power/state, and running locks to /sys/power/wake_lock, as Android's
 * system_suspend does. Built like sheng's battery publisher (battery/native_battery_hdi_publisher_v131.cpp).
 * "native_power_hdi_publisher --serve" (the asteroids_power_hdi service) logs to /data/local/tmp/asteroids-power-hdi.txt.
 */
#include "power/v1_3/ipower_interface.h"
#include "object_collector.h"
#include "iservmgr_hdi.h"
#include "hdf_remote_service.h"
#include "hdf_device_class.h"
#include "message_parcel.h"
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
namespace P = OHOS::HDI::Power::V1_3;
static volatile sig_atomic_t stop;
static void finish(int) { stop = 1; }
#define LOAD(var, name) do { *reinterpret_cast<void **>(&(var)) = dlsym(driver, name); \
    if (!(var)) { printf("POWER_HDI_SYMBOL_MISSING=%s\n", name); return 3; } } while (0)
int main(int argc, char **argv)
{
    setbuf(stdout, nullptr);
    bool serve = argc == 2 && !strcmp(argv[1], "--serve");
    if (argc > 1 && !serve) return 1;
    if (serve) {
        int fd = open("/data/local/tmp/asteroids-power-hdi.txt", O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0) return 2;
        if (fd > 2) close(fd);
    }
    signal(SIGTERM, finish); signal(SIGINT, finish); signal(SIGALRM, finish);
    if (!serve) alarm(25);
    auto registryCheck = OHOS::HDI::ServiceManager::V1_0::IServiceManager::Get();
    if (!registryCheck) return 2;
    if (registryCheck->GetService("power_interface_service")) {
        puts("POWER_HDI_ALREADY_REGISTERED=1"); return 2;
    }
    // The driver library carries the Binder stubs (registered with the object collector when it loads).
    void *driver = dlopen("/vendor/lib64/libpower_driver.z.so", RTLD_NOW | RTLD_GLOBAL);
    void *factory = dlopen("/vendor/lib64/libpower_interface_service_1.3.z.so", RTLD_NOW | RTLD_GLOBAL);
    if (!driver || !factory) { printf("POWER_HDI_LOAD_ERROR=%s\n", dlerror()); return 3; }
    auto create = reinterpret_cast<P::IPowerInterface *(*)()>(dlsym(factory, "PowerInterfaceImplGetInstance"));
    if (!create) return 3;
    OHOS::sptr<P::IPowerInterface> implementation = create();
    if (!implementation) return 4;
    auto stub = OHOS::HDI::ObjectCollector::GetInstance().GetOrNewObject(implementation,
        P::IPowerInterface::GetDescriptor());
    if (!stub) { puts("POWER_HDI_ORIGINAL_STUB_NULL"); return 5; }
    HdfRemoteService *(*getRemote)(int32_t);
    bool (*setDesc)(HdfRemoteService *, const char *);
    bool (*writeToken)(HdfRemoteService *, HdfSBuf *);
    HdfSBuf *(*obtain)(HdfSbufType); void (*recycle)(HdfSBuf *);
    bool (*str)(HdfSBuf *, const char *); bool (*u16)(HdfSBuf *, uint16_t); bool (*u32)(HdfSBuf *, uint32_t);
    int32_t (*toParcel)(HdfSBuf *, OHOS::MessageParcel **);
    LOAD(getRemote, "HdfRemoteServiceGet"); LOAD(setDesc, "HdfRemoteServiceSetInterfaceDesc");
    LOAD(writeToken, "HdfRemoteServiceWriteInterfaceToken"); LOAD(obtain, "HdfSbufTypedObtain");
    LOAD(recycle, "HdfSbufRecycle"); LOAD(str, "HdfSbufWriteString");
    LOAD(u16, "HdfSbufWriteUint16"); LOAD(u32, "HdfSbufWriteUint32"); LOAD(toParcel, "SbufToParcel");
    auto registry = getRemote(5100);
    if (!registry || !setDesc(registry, "HDI.IServiceManager.V1_0")) return 6;
    auto data = obtain(SBUF_IPC), reply = obtain(SBUF_IPC);
    if (!data || !reply) return 6;
    OHOS::MessageParcel *parcel = nullptr;
    bool valid = writeToken(registry, data) && str(data, "power_interface_service") &&
        u16(data, DEVICE_CLASS_DEFAULT) && u32(data, 0) && toParcel(data, &parcel) == 0 &&
        parcel->WriteRemoteObject(stub) && str(data, "") && str(data, "ohos.hdi.power.v1_3.IPowerInterface");
    int rc = valid ? registry->dispatcher->Dispatch(registry, 1, data, reply) : -1;
    recycle(data); recycle(reply);
    printf("POWER_HDI_PUBLISH_RC=%d uid=%d pid=%d\n", rc, getuid(), getpid());
    if (rc) return 7;
    while (!stop) sleep(1);
    data = obtain(SBUF_IPC); reply = obtain(SBUF_IPC);
    if (data && reply && writeToken(registry, data) && str(data, "power_interface_service"))
        printf("POWER_HDI_UNPUBLISH_RC=%d\n", registry->dispatcher->Dispatch(registry, 8, data, reply));
    _exit(0);
}
