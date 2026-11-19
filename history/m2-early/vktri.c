// vktri: renders a spinning triangle with Vulkan (Turnip on KGSL) into an offscreen image, then either writes the
// first frame as a PPM or, with --drm, copies every frame into a DRM dumb buffer on the panel. Tests the GPU on its
// own, and GPU frames on the display, before HarmonyOS's composer is involved.
//     vktri out.ppm                     one 1080x2392 frame
//     vktri --drm [seconds]             frames on the panel, with the frame rate printed every second
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "tri_vert.h"
#include "tri_frag.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "%s failed: %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static VkPhysicalDeviceMemoryProperties memprops;

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    fprintf(stderr, "no memory type for %x\n", want);
    exit(1);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// The panel: first connected connector, preferred mode, one dumb buffer scanned out. Returns its mapping.
static uint32_t *drm_setup(uint32_t *w, uint32_t *h, uint32_t *pitch)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("/dev/dri/card0"); exit(1); }
    drmSetMaster(fd);
    drmModeRes *res = drmModeGetResources(fd);
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 &&
            (!conn || c->connector_type == DRM_MODE_CONNECTOR_DSI))
            conn = c;
    }
    if (!conn) { fprintf(stderr, "no connected connector\n"); exit(1); }
    drmModeModeInfo mode = conn->modes[0];
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) mode = conn->modes[i];
    uint32_t crtc = 0;
    for (int e = 0; !crtc && e < conn->count_encoders; e++) {
        drmModeEncoder *en = drmModeGetEncoder(fd, conn->encoders[e]);
        for (int i = 0; en && i < res->count_crtcs; i++)
            if (en->possible_crtcs & (1u << i)) { crtc = res->crtcs[i]; break; }
    }
    struct drm_mode_create_dumb cd = { .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32 };
    uint32_t fb;
    struct drm_mode_map_dumb md;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) ||
        drmModeAddFB(fd, cd.width, cd.height, 24, 32, cd.pitch, cd.handle, &fb) ||
        (md = (struct drm_mode_map_dumb){ .handle = cd.handle }, drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md))) {
        perror("dumb buffer");
        exit(1);
    }
    uint32_t *px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    memset(px, 0, cd.size);
    if (drmModeSetCrtc(fd, crtc, fb, 0, 0, &conn->connector_id, 1, &mode)) { perror("SetCrtc"); exit(1); }
    *w = cd.width; *h = cd.height; *pitch = cd.pitch;
    printf("panel %ux%u@%u, crtc %u\n", mode.hdisplay, mode.vdisplay, mode.vrefresh, crtc);
    return px;
}

int main(int argc, char **argv)
{
    int drm = argc > 1 && !strcmp(argv[1], "--drm");
    int secs = drm && argc > 2 ? atoi(argv[2]) : 10;
    uint32_t W = 1080, H = 2392, pitch = 0;
    uint32_t *scan = drm ? drm_setup(&W, &H, &pitch) : NULL;

    VkInstance inst;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1,
                              .pApplicationName = "vktri" };
    CHECK(vkCreateInstance(&(VkInstanceCreateInfo){ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                                    .pApplicationInfo = &app }, NULL, &inst));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 ? VK_ERROR_INITIALIZATION_FAILED : VK_SUCCESS);
    if (!n) { fprintf(stderr, "no Vulkan device\n"); return 1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s (driver %u.%u.%u)\n", props.deviceName, VK_VERSION_MAJOR(props.driverVersion),
           VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));
    vkGetPhysicalDeviceMemoryProperties(pd, &memprops);

    float prio = 1.0f;
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &(VkDeviceCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio } },
        NULL, &dev));
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    // Color target (BGRA, so the bytes match DRM's XRGB8888) and a host-visible buffer to read it back into.
    VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkImage img;
    CHECK(vkCreateImage(dev, &(VkImageCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT }, NULL, &img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img, &mr);
    VkDeviceMemory imem;
    CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) }, NULL, &imem));
    CHECK(vkBindImageMemory(dev, img, imem, 0));
    VkImageView view;
    CHECK(vkCreateImageView(dev, &(VkImageViewCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }, NULL, &view));

    VkDeviceSize bsize = (VkDeviceSize)W * H * 4;
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bsize, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT }, NULL, &buf));
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkDeviceMemory bmem;
    CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) }, NULL, &bmem));
    CHECK(vkBindBufferMemory(dev, buf, bmem, 0));
    uint8_t *host;
    CHECK(vkMapMemory(dev, bmem, 0, bsize, 0, (void **)&host));

    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &(VkRenderPassCreateInfo){ .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &(VkAttachmentDescription){ .format = fmt,
            .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL },
        .subpassCount = 1, .pSubpasses = &(VkSubpassDescription){
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
            .pColorAttachments = &(VkAttachmentReference){ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } } },
        NULL, &rp));
    VkFramebuffer fbo;
    CHECK(vkCreateFramebuffer(dev, &(VkFramebufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = rp, .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1 },
        NULL, &fbo));

    VkShaderModule vs, fs;
    CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(tri_vert), .pCode = tri_vert }, NULL, &vs));
    CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(tri_frag), .pCode = tri_frag }, NULL, &fs));
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
        .pPushConstantRanges = &(VkPushConstantRange){ VK_SHADER_STAGE_VERTEX_BIT, 0, 8 } }, NULL, &layout));
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fs, .pName = "main" } };
    VkPipeline pipe;
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &(VkGraphicsPipelineCreateInfo){
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO },
        .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST },
        .pViewportState = &(VkPipelineViewportStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
            .pViewports = &(VkViewport){ 0, 0, W, H, 0, 1 }, .scissorCount = 1,
            .pScissors = &(VkRect2D){ { 0, 0 }, { W, H } } },
        .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 },
        .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT },
        .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
            .pAttachments = &(VkPipelineColorBlendAttachmentState){ .colorWriteMask = 0xf } },
        .layout = layout, .renderPass = rp }, NULL, &pipe));

    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT }, NULL, &pool));
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }, &cb));
    VkFence fence;
    CHECK(vkCreateFence(dev, &(VkFenceCreateInfo){ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }, NULL, &fence));

    double t0 = now(), last = t0;
    int frames = 0, total = 0;
    do {
        float pc[2] = { (float)(now() - t0), (float)W / H };
        CHECK(vkResetCommandBuffer(cb, 0));
        CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }));
        vkCmdBeginRenderPass(cb, &(VkRenderPassBeginInfo){ .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = rp, .framebuffer = fbo, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1,
            .pClearValues = &(VkClearValue){ .color = { .float32 = { 0.05f, 0.05f, 0.12f, 1 } } } },
            VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &(VkBufferImageCopy){
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } });
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
            &(VkMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT }, 0, NULL, 0,
            NULL);
        CHECK(vkEndCommandBuffer(cb));
        CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cb }, fence));
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
        CHECK(vkResetFences(dev, 1, &fence));

        if (!drm) {
            FILE *f = fopen(argv[1] ? argv[1] : "vktri.ppm", "wb");
            fprintf(f, "P6\n%u %u\n255\n", W, H);
            for (uint32_t i = 0; i < W * H; i++) {
                uint8_t rgb[3] = { host[i * 4 + 2], host[i * 4 + 1], host[i * 4] };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
            printf("wrote %s\n", argv[1] ? argv[1] : "vktri.ppm");
            break;
        }
        for (uint32_t y = 0; y < H; y++)
            memcpy((uint8_t *)scan + (size_t)y * pitch, host + (size_t)y * W * 4, W * 4);
        frames++, total++;
        if (now() - last >= 1) {
            printf("%.1f fps\n", frames / (now() - last));
            fflush(stdout);
            frames = 0;
            last = now();
        }
    } while (now() - t0 < secs);
    if (drm) printf("%d frames in %.1f s\n", total, now() - t0);
    return 0;
}
