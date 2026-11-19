// SPDX-License-Identifier: MIT
// asteroids: registers the donor's vibrator service (VibratorIfService, /vendor/lib64/libvibrator_interface_service_2.0.z.so)
// as vibrator_interface_service with the HDF service manager, as motion_hdi_publisher.cpp does for the motion service.
// Huawei runs it in an HDF host this port doesn't start. The service loads the vibrator VDI, ours (vibrator_vdi.cpp);
// the stub is the donor's (libvibrator_stub_2.0.z.so). Stays up until SIGTERM.
#include "vibrator/v2_0/ivibrator_interface.h"
#include "object_collector.h"
#include "iservmgr_hdi.h"
#include "hdf_remote_service.h"
#include "hdf_device_class.h"
#include "message_parcel.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <dlfcn.h>
#include <semaphore.h>
#include <unistd.h>

using namespace OHOS;
namespace Vibrator = OHOS::HDI::Vibrator::V2_0;

namespace {
constexpr char SERVICE_NAME[] = "vibrator_interface_service";
constexpr char SERVICE_DESCRIPTOR[] = "ohos.hdi.vibrator.v2_0.IVibratorInterface";
sem_t stopWake;

void StopSignal(int)
{
    int saved = errno;
    sem_post(&stopWake);
    errno = saved;
}

// The service manager's registration call, through libhdf_ipc_adapter's C API (as sheng's sensor publisher).
struct RegistryApi {
    HdfRemoteService *(*getRemote)(int32_t) = nullptr;
    bool (*setDesc)(HdfRemoteService *, const char *) = nullptr;
    bool (*writeToken)(HdfRemoteService *, HdfSBuf *) = nullptr;
    HdfSBuf *(*obtain)(HdfSbufType) = nullptr;
    void (*recycle)(HdfSBuf *) = nullptr;
    bool (*writeString)(HdfSBuf *, const char *) = nullptr;
    bool (*writeU16)(HdfSBuf *, uint16_t) = nullptr;
    bool (*writeU32)(HdfSBuf *, uint32_t) = nullptr;
    int32_t (*toParcel)(HdfSBuf *, MessageParcel **) = nullptr;

    bool Load(void *library)
    {
#define LOAD_MEMBER(member, symbol) \
        *reinterpret_cast<void **>(&(member)) = dlsym(library, symbol); \
        if ((member) == nullptr) return false
        LOAD_MEMBER(getRemote, "HdfRemoteServiceGet");
        LOAD_MEMBER(setDesc, "HdfRemoteServiceSetInterfaceDesc");
        LOAD_MEMBER(writeToken, "HdfRemoteServiceWriteInterfaceToken");
        LOAD_MEMBER(obtain, "HdfSbufTypedObtain");
        LOAD_MEMBER(recycle, "HdfSbufRecycle");
        LOAD_MEMBER(writeString, "HdfSbufWriteString");
        LOAD_MEMBER(writeU16, "HdfSbufWriteUint16");
        LOAD_MEMBER(writeU32, "HdfSbufWriteUint32");
        LOAD_MEMBER(toParcel, "SbufToParcel");
#undef LOAD_MEMBER
        return true;
    }

    int32_t Call(HdfRemoteService *registry, int code, const sptr<IRemoteObject> &stub)
    {
        HdfSBuf *data = obtain(SBUF_IPC);
        HdfSBuf *reply = obtain(SBUF_IPC);
        MessageParcel *parcel = nullptr;
        bool valid = data != nullptr && reply != nullptr && writeToken(registry, data) &&
            writeString(data, SERVICE_NAME);
        if (valid && stub != nullptr) {  // add: class, info, the stub, an empty info string, the descriptor
            valid = writeU16(data, DEVICE_CLASS_DEFAULT) && writeU32(data, 0) && toParcel(data, &parcel) == 0 &&
                parcel != nullptr && parcel->WriteRemoteObject(stub) && writeString(data, "") &&
                writeString(data, SERVICE_DESCRIPTOR);
        }
        int32_t result = valid ? registry->dispatcher->Dispatch(registry, code, data, reply) : HDF_FAILURE;
        if (data != nullptr) recycle(data);
        if (reply != nullptr) recycle(reply);
        return result;
    }
};
}  // namespace

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (sem_init(&stopWake, 0, 0) != 0) return 12;
    struct sigaction action{};
    action.sa_handler = StopSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);

    auto serviceManager = HDI::ServiceManager::V1_0::IServiceManager::Get();
    if (serviceManager == nullptr) {
        puts("VIBRATOR_PUBLISH_REGISTRY_AVAILABLE=0");
        return 1;
    }
    if (serviceManager->GetService(SERVICE_NAME) != nullptr) {
        puts("VIBRATOR_PUBLISH_ALREADY_REGISTERED=1");
        return 2;
    }
    // The stubs register their factories with the object collector when loaded.
    void *stubs = dlopen("/vendor/lib64/libvibrator_stub_2.0.z.so", RTLD_NOW | RTLD_GLOBAL);
    void *ipc = dlopen("libhdf_ipc_adapter.z.so", RTLD_NOW | RTLD_GLOBAL);
    void *service = dlopen("/vendor/lib64/libvibrator_interface_service_2.0.z.so", RTLD_NOW | RTLD_LOCAL);
    std::printf("VIBRATOR_PUBLISH_LOAD stubs=%d ipc=%d service=%d\n", stubs != nullptr, ipc != nullptr,
        service != nullptr);
    if (stubs == nullptr || ipc == nullptr || service == nullptr) {
        std::printf("VIBRATOR_PUBLISH_DLERROR %s\n", dlerror());
        return 3;
    }
    using Factory = Vibrator::IVibratorInterface *(*)();
    auto create = reinterpret_cast<Factory>(dlsym(service, "VibratorInterfaceImplGetInstance"));
    RegistryApi api;
    if (create == nullptr || !api.Load(ipc)) {
        puts("VIBRATOR_PUBLISH_SYMBOLS_READY=0");
        return 4;
    }
    sptr<Vibrator::IVibratorInterface> implementation = create();
    if (implementation == nullptr) return 5;
    auto stub = HDI::ObjectCollector::GetInstance().GetOrNewObject(implementation,
        Vibrator::IVibratorInterface::GetDescriptor());
    if (stub == nullptr) return 6;
    auto registry = api.getRemote(5100);
    if (registry == nullptr || !api.setDesc(registry, "HDI.IServiceManager.V1_0")) return 7;
    int32_t published = api.Call(registry, 1, stub);
    std::printf("VIBRATOR_PUBLISH_RESULT rc=%d uid=%u\n", published, getuid());
    if (published != HDF_SUCCESS) return 8;
    while (sem_wait(&stopWake) != 0 && errno == EINTR) {}
    int32_t removed = api.Call(registry, 8, nullptr);
    std::printf("VIBRATOR_PUBLISH_CLEANUP unpublish=%d\n", removed);
    // IPC threads may still hold the service: keep its code mapped until the process exits.
    _exit(removed == HDF_SUCCESS ? 0 : 9);
}
