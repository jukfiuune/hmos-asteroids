#!/usr/bin/env python3
"""Adapts sheng's Turnip OHOS support (written against OpenHarmony's Mesa 25.0.1) to Mesa main, after its
src/freedreno hunks are applied: official extension names, VkNativeBufferOHOS (absent from the headers because the
registry marks VK_OHOS_native_buffer disabled), and bind-time layout through tu_image_init (main has no
tu_image_update_layout). Run from the Mesa tree."""
import glob
import re


def edit(path, old, new):
    s = open(path).read()
    assert old in s, (path, old[:70])
    open(path, "w").write(s.replace(old, new, 1))


for path in glob.glob("src/freedreno/vulkan/*.cc") + glob.glob("src/freedreno/vulkan/*.h") + \
        glob.glob("src/vulkan/runtime/*.c"):
    s = open(path).read()
    if "OHOS_NATIVE_BUFFER_BIT_OHOS" in s:
        open(path, "w").write(s.replace("VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS",
                                        "VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS"))

edit("src/freedreno/vulkan/tu_ohos.h", "struct Ace5OHBufferHandle {", """\
/* VK_OHOS_native_buffer is marked disabled in the registry, so vulkan_ohos.h lacks its struct. This is the
 * registry definition (extension 454, offset 1). */
#define VK_STRUCTURE_TYPE_NATIVE_BUFFER_OHOS ((VkStructureType)1000453001)
struct OHBufferHandle;
typedef struct VkNativeBufferOHOS {
   VkStructureType sType;
   const void *pNext;
   struct OHBufferHandle *handle;
} VkNativeBufferOHOS;
struct Ace5OHBufferHandle {""")

edit("src/freedreno/vulkan/tu_ohos.cc",
     "VkResult result=TU_CALLX(device,tu_image_update_layout)(device,image,DRM_FORMAT_MOD_LINEAR,planes);",
     "VkResult result=TU_CALLX(device,tu_image_init)(device,image,&image->ohos_create_info,"
     "DRM_FORMAT_MOD_LINEAR,planes,TU_IMAGE_ID_ASSIGN);")

s = open("src/freedreno/vulkan/tu_image.h").read()
m = re.search(r"struct tu_image\s*\{\n\s*struct vk_image vk;\n", s)
assert m, "tu_image struct"
s = s[:m.end()] + ("\n   /* OHOS native-buffer images are laid out at bind time from this copy (pNext dropped; they are"
                   " linear). */\n   VkImageCreateInfo ohos_create_info;\n") + s[m.end():]
open("src/freedreno/vulkan/tu_image.h", "w").write(s)

edit("src/freedreno/vulkan/tu_image.cc", """\
   if (ohos_native) {
      result = tu_ohos_import_image(device, image, ohos_native, alloc);""", """\
   if (ohos_native ||
       (image->vk.external_handle_types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS)) {
      image->ohos_create_info = *pCreateInfo;
      image->ohos_create_info.pNext = NULL;
   }
   if (ohos_native) {
      result = tu_ohos_import_image(device, image, ohos_native, alloc);""")
print("ported")

# --- Fix-ups found by compiling against main ---------------------------------------------------------------------
# The bind-time layout check: main moved per-image binding into tu_image_bind(), which returns its result.
s = open("src/freedreno/vulkan/tu_image.cc").read()
misplaced = """         if (mem->ohos_import) {
            result = tu_ohos_bind_layout(device, image, &mem->ohos_handle);
            if (result != VK_SUCCESS) {
               if (status) *status->pResult = result;
               return result;
            }
         }
"""
if misplaced in s:
    s = s.replace(misplaced, "", 1)
    s = s.replace("""   assert(mem);
   image->mem = mem;""", """   assert(mem);
   if (mem->ohos_import) {
      result = tu_ohos_bind_layout(device, image, &mem->ohos_handle);
      if (result != VK_SUCCESS)
         return result;
   }
   image->mem = mem;""", 1)
# VkNativeBufferOHOS isn't a registry-known sType to vk_find_struct's type map; use the raw lookup.
s = s.replace("vk_find_struct_const(pCreateInfo->pNext, NATIVE_BUFFER_OHOS)",
              "(const VkNativeBufferOHOS *)__vk_find_struct((void *)pCreateInfo->pNext, "
              "VK_STRUCTURE_TYPE_NATIVE_BUFFER_OHOS)")
open("src/freedreno/vulkan/tu_image.cc", "w").write(s)

for path in ("src/freedreno/vulkan/tu_ohos.cc", "src/freedreno/vulkan/tu_device.cc"):
    s = open(path).read()
    s = s.replace("vk_find_struct_const(pCreateInfo->pNext, NATIVE_BUFFER_OHOS)",
                  "(const VkNativeBufferOHOS *)__vk_find_struct((void *)pCreateInfo->pNext, "
                  "VK_STRUCTURE_TYPE_NATIVE_BUFFER_OHOS)")
    open(path, "w").write(s)

# OHOS_native_buffer is the disabled extension: not in main's extension table.
edit("src/freedreno/vulkan/tu_device.cc", "      .OHOS_native_buffer = true,\n", "")
# tu_bo_init_dmabuf gained alignment and flags.
edit("src/freedreno/vulkan/tu_device.cc", "result = tu_bo_init_dmabuf(device, &mem->bo, h->size, h->fd);",
     "result = tu_bo_init_dmabuf(device, &mem->bo, h->size, 0, TU_BO_ALLOC_NO_FLAGS, h->fd);")
# A 64-bit flags value printed with %x.
s = open("src/freedreno/vulkan/tu_ohos.cc").read()
s = s.replace("samples=%u flags=%x\\n\",", "samples=%u flags=%llx\\n\",")
s = s.replace("image->vk.samples,image->vk.create_flags);", "image->vk.samples,(unsigned long long)image->vk.create_flags);")
open("src/freedreno/vulkan/tu_ohos.cc", "w").write(s)
print("fixed up")

# --- Enable VK_OHOS_native_buffer ---------------------------------------------------------------------------------
# HarmonyOS's Vulkan swapchain layer looks up vkGetSwapchainGrallocUsageOHOS, vkAcquireImageOHOS and
# vkQueueSignalReleaseImageOHOS. The registry has the extension but marks it disabled, so Khronos' headers and Mesa's
# generated dispatch tables leave it out. Enable it in vk.xml and give vulkan_ohos.h its definitions (from vk.xml).
edit("src/vulkan/registry/vk.xml",
     'name="VK_OHOS_native_buffer" number="589" type="device" platform="ohos" author="HUAWEI" '
     'contact="Weilan Chen @wchen-h" supported="disabled"',
     'name="VK_OHOS_native_buffer" number="589" type="device" platform="ohos" author="HUAWEI" '
     'contact="Weilan Chen @wchen-h" supported="vulkan"')
s = open("src/freedreno/vulkan/tu_ohos.h").read()
start = s.index("/* VK_OHOS_native_buffer is marked disabled")
s = s[:start] + s[s.index("struct Ace5OHBufferHandle {"):]
open("src/freedreno/vulkan/tu_ohos.h", "w").write(s)
s = open("include/vulkan/vulkan_ohos.h").read()
block = """
/* VK_OHOS_native_buffer: in the registry but marked disabled, so Khronos leaves it out of this header. Added for
 * HarmonyOS's swapchain layer (asteroids port); definitions from vk.xml. */
#define VK_OHOS_native_buffer 1
struct OHBufferHandle;
#define VK_OHOS_NATIVE_BUFFER_SPEC_VERSION 1
#define VK_OHOS_NATIVE_BUFFER_EXTENSION_NAME "VK_OHOS_native_buffer"
#define VK_STRUCTURE_TYPE_NATIVE_BUFFER_OHOS ((VkStructureType)1000453001)
#define VK_STRUCTURE_TYPE_SWAPCHAIN_IMAGE_CREATE_INFO_OHOS ((VkStructureType)1000453002)
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENTATION_PROPERTIES_OHOS ((VkStructureType)1000453003)

typedef enum VkSwapchainImageUsageFlagBitsOHOS {
    VK_SWAPCHAIN_IMAGE_USAGE_SHARED_BIT_OHOS = 0x00000001,
    VK_SWAPCHAIN_IMAGE_USAGE_FLAG_BITS_MAX_ENUM_OHOS = 0x7FFFFFFF
} VkSwapchainImageUsageFlagBitsOHOS;
typedef VkFlags VkSwapchainImageUsageFlagsOHOS;
typedef struct VkNativeBufferOHOS {
    VkStructureType            sType;
    const void*                pNext;
    struct OHBufferHandle*     handle;
} VkNativeBufferOHOS;

typedef struct VkSwapchainImageCreateInfoOHOS {
    VkStructureType                   sType;
    const void*                       pNext;
    VkSwapchainImageUsageFlagsOHOS    usage;
} VkSwapchainImageCreateInfoOHOS;

typedef struct VkPhysicalDevicePresentationPropertiesOHOS {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           sharedImage;
} VkPhysicalDevicePresentationPropertiesOHOS;

typedef VkResult (VKAPI_PTR *PFN_vkGetSwapchainGrallocUsageOHOS)(VkDevice device, VkFormat format, VkImageUsageFlags imageUsage, uint64_t* grallocUsage);
typedef VkResult (VKAPI_PTR *PFN_vkAcquireImageOHOS)(VkDevice device, VkImage image, int32_t nativeFenceFd, VkSemaphore semaphore, VkFence fence);
typedef VkResult (VKAPI_PTR *PFN_vkQueueSignalReleaseImageOHOS)(VkQueue queue, uint32_t waitSemaphoreCount, const VkSemaphore* pWaitSemaphores, VkImage image, int32_t* pNativeFenceFd);

#ifndef VK_NO_PROTOTYPES
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainGrallocUsageOHOS(VkDevice device, VkFormat format, VkImageUsageFlags imageUsage, uint64_t* grallocUsage);
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireImageOHOS(VkDevice device, VkImage image, int32_t nativeFenceFd, VkSemaphore semaphore, VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSignalReleaseImageOHOS(VkQueue queue, uint32_t waitSemaphoreCount, const VkSemaphore* pWaitSemaphores, VkImage image, int32_t* pNativeFenceFd);
#endif
"""
tail = s.rindex("#ifdef __cplusplus")
s = s[:tail] + block + "\n" + s[tail:]
open("include/vulkan/vulkan_ohos.h", "w").write(s)
# Expose it from Turnip again now that the extension table knows it.
edit("src/freedreno/vulkan/tu_device.cc", "      .OHOS_external_memory = true,\n",
     "      .OHOS_external_memory = true,\n      .OHOS_native_buffer = true,\n")
print("VK_OHOS_native_buffer enabled")

# --- Prototypes for sheng's observed implementations (its logging wrappers in tu_device.cc call them) ------------
edit("src/freedreno/vulkan/tu_ohos.h", "struct tu_device;\n", """struct tu_device;
VKAPI_ATTR VkResult VKAPI_CALL sheng_observed_tu_AllocateMemory(VkDevice, const VkMemoryAllocateInfo *,
   const VkAllocationCallbacks *, VkDeviceMemory *);
VKAPI_ATTR VkResult VKAPI_CALL sheng_observed_tu_CreateImage(VkDevice, const VkImageCreateInfo *,
   const VkAllocationCallbacks *, VkImage *);
VKAPI_ATTR VkResult VKAPI_CALL sheng_observed_tu_GetNativeBufferPropertiesOHOS(VkDevice, const OH_NativeBuffer *,
   VkNativeBufferPropertiesOHOS *);
""")
print("prototypes added")

# --- OHOS imports don't count against the heap budget -------------------------------------------------------------
# An imported native buffer belongs to the allocator, not to us. render_service re-imports the same few window
# buffers again and again (it doesn't free the earlier imports), so charging each import to heap.used fills the
# budget after about a thousand frames and every later allocation fails with VK_ERROR_OUT_OF_DEVICE_MEMORY; the
# screen then stops updating. Charge and refund only memory Turnip allocates itself.
edit("src/freedreno/vulkan/tu_device.cc", """   if (result == VK_SUCCESS && !mem->lazy) {
      result = tu_add_to_heap(device, mem->bo);""", """   if (result == VK_SUCCESS && !mem->lazy && !mem->ohos_import) {
      result = tu_add_to_heap(device, mem->bo);
   }
   if (result == VK_SUCCESS && !mem->lazy) {""")
edit("src/freedreno/vulkan/tu_device.cc", """   if (mem->bo) {
      p_atomic_add(&device->physical_device->heap.used, -mem->bo->size);""", """   if (mem->bo) {
      if (!mem->ohos_import)
         p_atomic_add(&device->physical_device->heap.used, -mem->bo->size);""")
print("OHOS imports outside the heap budget")

# --- One KGSL mapping per dma-buf ----------------------------------------------------------------------------------
# KGSL hands out a new object (GPU mapping, page list, held fd) for every import, even of a buffer that is already
# imported. render_service imports the same screen-sized window buffers again and again without freeing the earlier
# imports (thousands of mappings of 4 buffers after half an hour, ~70 more each time an app opens), so kernel memory
# and GPU address space grow without bound. Share one tu_bo per dma-buf (by inode, as the MSM backend shares one per
# GEM handle): a re-import takes a reference, and the mapping goes when the last one is freed. The OHOS import
# counters say whether render_service frees at all.
edit("src/freedreno/vulkan/tu_knl_kgsl.cc", """static void
kgsl_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   assert(bo->gem_handle);

   if (!p_atomic_dec_zero(&bo->refcnt))
      return;
""", """static void
kgsl_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   assert(bo->gem_handle);

   if (bo->unique_id >> 63) {
      std::lock_guard<std::mutex> guard(dmabuf_bos_lock);
      if (!p_atomic_dec_zero(&bo->refcnt))
         return;
      auto it = dmabuf_bos.find({dev, ino_t(bo->unique_id & ~(1ULL << 63))});
      if (it != dmabuf_bos.end() && it->second == bo)
         dmabuf_bos.erase(it);
   } else if (!p_atomic_dec_zero(&bo->refcnt)) {
      return;
   }
""")
edit("src/freedreno/vulkan/tu_knl_kgsl.cc", """   int ret;

   req.flags |= kgsl_align_flag(align);

   ret = safe_ioctl(dev->physical_device->local_fd,
                    IOCTL_KGSL_GPUOBJ_IMPORT, &req);""", """   int ret;

   struct stat shared;
   const bool known = fstat(fd, &shared) == 0;
   if (known) {
      std::lock_guard<std::mutex> guard(dmabuf_bos_lock);
      auto it = dmabuf_bos.find({dev, shared.st_ino});
      if (it != dmabuf_bos.end() && it->second->size >= size) {
         p_atomic_inc(&it->second->refcnt);
         *out_bo = it->second;
         return VK_SUCCESS;
      }
   }

   req.flags |= kgsl_align_flag(align);

   ret = safe_ioctl(dev->physical_device->local_fd,
                    IOCTL_KGSL_GPUOBJ_IMPORT, &req);""")
edit("src/freedreno/vulkan/tu_knl_kgsl.cc", """#include "tu_rmv.h"
""", """#include "tu_rmv.h"

#include <map>
#include <mutex>
#include <sys/stat.h>

/* dma-buf imports by (device, inode); see kgsl_bo_init_dmabuf. */
static std::mutex dmabuf_bos_lock;
static std::map<std::pair<const tu_device *, ino_t>, tu_bo *> dmabuf_bos;
""")
edit("src/freedreno/vulkan/tu_knl_kgsl.cc", """      bo->unique_id = st.st_ino | (1ULL << 63);

   tu_dump_bo_init(dev, bo);
""", """      bo->unique_id = st.st_ino | (1ULL << 63);
   if (known) {
      std::lock_guard<std::mutex> guard(dmabuf_bos_lock);
      dmabuf_bos[{dev, shared.st_ino}] = bo;
   }

   tu_dump_bo_init(dev, bo);
""")
edit("src/freedreno/vulkan/tu_device.cc", """   if (mem->bo) {
      if (!mem->ohos_import)""", """   if (mem->ohos_import) {
      static unsigned frees;
      unsigned n = p_atomic_inc_return(&frees);
      if (n % 64 == 0)
         fprintf(stderr, "SHENG_OHOS_FREE n=%u\\n", n);
   }
   if (mem->bo) {
      if (!mem->ohos_import)""")
edit("src/freedreno/vulkan/tu_device.cc", """         mem->ohos_handle = *h;
         mem->ohos_import = true;""", """         mem->ohos_handle = *h;
         mem->ohos_import = true;
         static unsigned imports;
         unsigned n = p_atomic_inc_return(&imports);
         if (n % 64 == 0)
            fprintf(stderr, "SHENG_OHOS_IMPORT n=%u\\n", n);""")
print("one KGSL mapping per dma-buf")

# --- Release imports whose buffer is gone --------------------------------------------------------------------------
# render_service never frees the memory of most native-buffer imports (vkFreeMemory runs a handful of times in
# thousands of imports), so every buffer it ever drew stays mapped and pinned, about 65 MB more each time an app opens
# and closes. A buffer's dma-buf records how many references it has (fdinfo "count"); once only this import's own fd
# and KGSL's reference are left, the buffer's owners and render_service itself have let go of it, and nothing can draw
# it again. Every 16 imports (at most four times a second: the scan runs inside render_service's frame), such memory
# objects drop their BO (the GPU mapping and the pages); a later vkFreeMemory of one finds no BO and only frees the
# object.
edit("src/freedreno/vulkan/tu_device.cc", """   VkResult result = sheng_observed_tu_AllocateMemory(handle, info, alloc, memory);
   static unsigned calls = 0, failures = 0;""", """   VkResult result = sheng_observed_tu_AllocateMemory(handle, info, alloc, memory);
   if (result == VK_SUCCESS && vk_find_struct_const(info->pNext, IMPORT_NATIVE_BUFFER_INFO_OHOS)) {
      std::lock_guard<std::mutex> guard(ohos_imports_lock);
      ohos_imports[tu_device_memory_from_handle(*memory)] = device;
      static uint64_t last_scan;
      uint64_t now = os_time_get_nano();
      if (++ohos_imports_since_scan >= 16 && now - last_scan >= 250000000) {
         ohos_imports_since_scan = 0;
         last_scan = now;
         unsigned released = tu_ohos_release_orphans();
         static unsigned total, scans;
         if (released || ++scans % 64 == 0)
            fprintf(stderr, "SHENG_OHOS_RELEASE released=%u total=%u live=%zu scan_us=%llu\\n", released,
                    total += released, ohos_imports.size(),
                    (unsigned long long)((os_time_get_nano() - now) / 1000));
      }
   }
   static unsigned calls = 0, failures = 0;""")
edit("src/freedreno/vulkan/tu_device.cc", """static void
_tu_destroy_memory(struct tu_device *device,
                  struct tu_device_memory *mem)
{""", """/* Live native-buffer imports, for tu_ohos_release_orphans. */
static std::mutex ohos_imports_lock;
static std::map<tu_device_memory *, tu_device *> ohos_imports;
static unsigned ohos_imports_since_scan;

/* References to the dma-buf behind fd (fdinfo "count"), or -1. */
static long
tu_dmabuf_refs(int fd)
{
   char path[48], line[96];
   long count = -1;
   snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
   FILE *f = fopen(path, "re");
   while (f && fgets(line, sizeof(line), f))
      if (sscanf(line, "count: %ld", &count) == 1)
         break;
   if (f)
      fclose(f);
   return count;
}

/* Drops the BO of each import whose dma-buf only this import's fd and KGSL still hold. Called with
 * ohos_imports_lock held. Re-imports share a BO, so thousands of imports have only a couple of hundred BOs: each BO's
 * count is read once (reading fdinfo for every import took tens of milliseconds inside render_service's frame). */
static unsigned
tu_ohos_release_orphans(void)
{
   unsigned released = 0;
   std::map<tu_bo *, long> bo_refs;
   for (auto it = ohos_imports.begin(); it != ohos_imports.end();) {
      tu_device_memory *mem = it->first;
      long refs = -1;
      if (mem->bo && mem->bo->shared_fd >= 0) {
         auto known = bo_refs.find(mem->bo);
         refs = known != bo_refs.end() ? known->second : (bo_refs[mem->bo] = tu_dmabuf_refs(mem->bo->shared_fd));
      }
      if (refs > 0 && refs <= 2) {
         tu_bo_finish(it->second, mem->bo);
         mem->bo = NULL;
         it = ohos_imports.erase(it);
         released++;
      } else {
         ++it;
      }
   }
   return released;
}

static void
_tu_destroy_memory(struct tu_device *device,
                  struct tu_device_memory *mem)
{
   if (mem->ohos_import) {
      /* After this, a concurrent release no longer sees mem; if one already ran, mem->bo is NULL. */
      std::lock_guard<std::mutex> guard(ohos_imports_lock);
      ohos_imports.erase(mem);
   }""")
s = open("src/freedreno/vulkan/tu_device.cc").read()
if "#include <map>" not in s:
    open("src/freedreno/vulkan/tu_device.cc", "w").write("#include <map>\n#include <mutex>\n" + s)
edit("src/freedreno/vulkan/tu_device.cc", """   /* Lazy memory may or may not have a BO at any given time, so it must
    * not depend on mem->bo. Use the memory object itself for the id and
    * the requested size, so the alloc and free reports always match.
    */
   const uint64_t mem_obj_id =
      mem->lazy ? reinterpret_cast<uintptr_t>(mem) : mem->bo->unique_id;
   const VkDeviceSize size = mem->lazy ? mem->vk.size : mem->bo->size;""", """   /* Lazy memory may or may not have a BO at any given time, and a released
    * native-buffer import has none, so neither may depend on mem->bo. Use the
    * memory object itself for the id and the requested size, so the alloc and
    * free reports always match.
    */
   const uint64_t mem_obj_id =
      mem->lazy || !mem->bo ? reinterpret_cast<uintptr_t>(mem) : mem->bo->unique_id;
   const VkDeviceSize size = mem->lazy || !mem->bo ? mem->vk.size : mem->bo->size;""")
print("orphaned imports released")

# NV21 (OHOS format 25, YCRCB_420_SP): the layout of NV12 (24) with Cr before Cb in the chroma plane. Gallery decodes
# photos into it, and render_service's import failed (VK_ERROR_INVALID_EXTERNAL_HANDLE), so the opened photo
# vanished. Same plane layout as 24, imported as G8_B8R8_2PLANE_420 with the conversion's R and B swapped.
edit("src/freedreno/vulkan/tu_ohos.cc", """ case 24: return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM; // NV12, linear Y then interleaved UV""",
     """ case 24: return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM; // NV12, linear Y then interleaved UV
 case 25: return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM; // NV21: interleaved VU, swapped in the conversion""")
edit("src/freedreno/vulkan/tu_ohos.cc", """ bool yuv=h->format==24 || h->format==28 || h->format==29;
 if(yuv && ((h->width | h->height)&1))return false;""", """ bool yuv=h->format==24 || h->format==25 || h->format==28 || h->format==29;
 if(yuv && ((h->width | h->height)&1))return false;""")
edit("src/freedreno/vulkan/tu_ohos.cc", """ if(h->format==24){planes[1].offset""", """ if(h->format==24 || h->format==25){planes[1].offset""")
edit("src/freedreno/vulkan/tu_ohos.cc", """  const bool yuv=h->format==24 || h->format==28 || h->format==29;""",
     """  const bool yuv=h->format==24 || h->format==25 || h->format==28 || h->format==29;""")
edit("src/freedreno/vulkan/tu_ohos.cc", """  f->samplerYcbcrConversionComponents={VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
  f->suggestedYcbcrModel=yuv?""", """  f->samplerYcbcrConversionComponents={VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
  if(h->format==25)f->samplerYcbcrConversionComponents={VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_IDENTITY};
  f->suggestedYcbcrModel=yuv?""")
edit("src/vulkan/runtime/vk_ycbcr_conversion.c",
     """      if ((ohos_ext_info->externalFormat != 24 && ohos_ext_info->externalFormat != 28 && ohos_ext_info->externalFormat != 29) || pCreateInfo->format != VK_FORMAT_UNDEFINED) {""",
     """      if ((ohos_ext_info->externalFormat != 24 && ohos_ext_info->externalFormat != 25 && ohos_ext_info->externalFormat != 28 && ohos_ext_info->externalFormat != 29) || pCreateInfo->format != VK_FORMAT_UNDEFINED) {""")
edit("src/vulkan/runtime/vk_ycbcr_conversion.c",
     """      state->format = ohos_ext_info->externalFormat == 24 ? VK_FORMAT_G8_B8R8_2PLANE_420_UNORM : VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
      for (unsigned i = 0; i < 4; ++i)
         state->mapping[i] = VK_COMPONENT_SWIZZLE_IDENTITY;""",
     """      const bool two_plane = ohos_ext_info->externalFormat == 24 || ohos_ext_info->externalFormat == 25;
      state->format = two_plane ? VK_FORMAT_G8_B8R8_2PLANE_420_UNORM : VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
      for (unsigned i = 0; i < 4; ++i)
         state->mapping[i] = VK_COMPONENT_SWIZZLE_IDENTITY;
      if (ohos_ext_info->externalFormat == 25) {  /* NV21: Cr where NV12 has Cb */
         state->mapping[0] = VK_COMPONENT_SWIZZLE_B;
         state->mapping[2] = VK_COMPONENT_SWIZZLE_R;
      }""")
edit("src/freedreno/vulkan/tu_image.cc",
     """      if ((external->externalFormat != 24 && external->externalFormat != 28 && external->externalFormat != 29) || pCreateInfo->format != VK_FORMAT_UNDEFINED ||""",
     """      if ((external->externalFormat != 24 && external->externalFormat != 25 && external->externalFormat != 28 && external->externalFormat != 29) || pCreateInfo->format != VK_FORMAT_UNDEFINED ||""")
edit("src/freedreno/vulkan/tu_image.cc",
     """      resolved_info.format = external->externalFormat == 24 ? VK_FORMAT_G8_B8R8_2PLANE_420_UNORM : VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;""",
     """      resolved_info.format = external->externalFormat == 24 || external->externalFormat == 25 ?
         VK_FORMAT_G8_B8R8_2PLANE_420_UNORM : VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;""")
print("NV21 import")
