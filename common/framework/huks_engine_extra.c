/* Entry points the donor's HUKS HDI imports from libhuks_ca_hdi.z.so that upstream's engine doesn't have: Huawei's
 * key encapsulation (KEM) extensions. They answer "not supported", as a TEE without those algorithms would. */
#include "hks_type.h"

int32_t HuksHdiEncapsulate(void)
{
    return HKS_ERROR_NOT_SUPPORTED;
}

int32_t HuksHdiDecapsulate(void)
{
    return HKS_ERROR_NOT_SUPPORTED;
}

/* huks_service's file store (hks_storage.c, built in for the RKC root-key files) fills buffers through the service's
 * HDI access layer; here the engine is the HDI, so it goes straight to it. */
int32_t HuksHdiGenerateRandom(const struct HksParamSet *paramSet, struct HksBlob *random);

int32_t HuksAccessGenerateRandom(const struct HksParamSet *paramSet, struct HksBlob *random)
{
    return HuksHdiGenerateRandom(paramSet, random);
}
