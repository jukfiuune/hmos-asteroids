// SPDX-License-Identifier: MIT
// asteroids: the entry point the donor's PIN driver calls (PinAuthInterfaceImplGetInstance), with the setup
// OpenHarmony's own driver does first. OpenHarmony's PIN state is global: the PIN database and the three executors'
// Ed25519 key pairs, set up by PinAuth::Init() from its driver's Init. We keep Huawei's driver, which doesn't call it,
// so without this every executor answered "key pair not init!" and none registered with the authentication framework.
// build-pinauth.sh renames OpenHarmony's entry point to OpenHarmonyPinAuthInterfaceImplGetInstance; this one calls Init once,
// then that.
#include "pin_auth.h"
#include "v3_0/ipin_auth_interface.h"
#include "iam_logger.h"

#include <mutex>

#undef LOG_TAG
#define LOG_TAG "PIN_AUTH_HDI"

using OHOS::HDI::PinAuth::V3_0::IPinAuthInterface;

extern "C" IPinAuthInterface *OpenHarmonyPinAuthInterfaceImplGetInstance(void);

extern "C" __attribute__((visibility("default"))) IPinAuthInterface *PinAuthInterfaceImplGetInstance(void)
{
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, [] {
        OHOS::UserIam::PinAuth::PinAuth pinAuth;
        ready = pinAuth.Init() == 0;
        IAM_LOGI("PIN backend init %{public}s", ready ? "done" : "failed");
    });
    return ready ? OpenHarmonyPinAuthInterfaceImplGetInstance() : nullptr;
}
