// Offscreen GPU smoke test: real Vulkan device, real shaders, real draw,
// real readback. It is deliberately headless so it runs on a server or over
// SSH, and it fails on the first API error instead of reporting success.
#include "graphics/vulkan/nx_vk.h"
#include "memory/nx_mem.h"
#include "platform/linux/nx_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define W 256
#define H 256

#include "spv_smoke.h"

#define VKOK(x, what) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { NX_LOGE("%s failed: %d", what, (int)r_); goto fail; } } while (0)

static int pipeline_cache_bad_header_test(NxVk *vk) {
    char path[] = "/tmp/nxasahi-pipeline-header-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    int ok = 1;
    for (int test = 0; test < 4 && ok; test++) {
        VkPipelineCacheHeaderVersionOne h = {0};
        h.headerSize = sizeof h; h.headerVersion = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
        h.vendorID = vk->caps.vendor_id; h.deviceID = vk->caps.device_id;
        memcpy(h.pipelineCacheUUID, vk->caps.props.pipelineCacheUUID, VK_UUID_SIZE);
        if (test == 0) h.pipelineCacheUUID[0] ^= 0xff; // stale driver-cache UUID
        if (test == 1) h.headerSize = 0;              // malformed header extent
        if (test == 2) h.headerVersion++;             // unsupported header version
        if (test == 3) h.vendorID++;                  // different device vendor
        if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0 ||
            write(fd, &h, sizeof h) != (ssize_t)sizeof h) { ok = 0; break; }
        if (vk->pipeline_cache) vkDestroyPipelineCache(vk->device, vk->pipeline_cache, NULL);
        vk->pipeline_cache = VK_NULL_HANDLE;
        if (nx_vk_pipeline_cache_load(vk, path) != 0 || vk->pipeline_cache == VK_NULL_HANDLE) ok = 0;
    }
    close(fd);
    unlink(path);
    return ok ? 0 : -1;
}

static int pipeline_cache_corruption_test(NxVk *vk) {
    char path[] = "/tmp/nxasahi-pipeline-data-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    close(fd);
    int ok = nx_vk_pipeline_cache_save(vk, path) == 0;
    // Simulate an interrupted write. The loader must reject this before the
    // opaque bytes reach the driver, then create a fresh empty cache.
    fd = ok ? open(path, O_WRONLY) : -1;
    if (fd < 0 || ftruncate(fd, 8) != 0) ok = 0;
    if (fd >= 0) close(fd);
    if (vk->pipeline_cache) vkDestroyPipelineCache(vk->device, vk->pipeline_cache, NULL);
    vk->pipeline_cache = VK_NULL_HANDLE;
    if (ok) ok = nx_vk_pipeline_cache_load(vk, path) == 0 && vk->pipeline_cache != VK_NULL_HANDLE;
    // Persist and reload an implementation-produced cache as a valid roundtrip.
    if (ok) ok = nx_vk_pipeline_cache_save(vk, path) == 0;
    if (vk->pipeline_cache) vkDestroyPipelineCache(vk->device, vk->pipeline_cache, NULL);
    vk->pipeline_cache = VK_NULL_HANDLE;
    if (ok) ok = nx_vk_pipeline_cache_load(vk, path) == 0 && vk->pipeline_cache != VK_NULL_HANDLE;
    unlink(path);
    return ok ? 0 : -1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    NxSysInfo si;
    nx_sysinfo_query(&si);
    nx_sysinfo_print(&si);
    nx_mem_report();

    NxVk vk;
    NxVkConfig cfg = {0};
    cfg.app_name = "nxnative-asahi vk smoke";
    cfg.app_version = 1;
    cfg.enable_validation = nx_env_bool("NX_VK_VALIDATION", 0);
    cfg.allow_cpu = nx_env_bool("NX_VK_ALLOW_CPU", 0);
    cfg.device_filter = nx_env("NX_VK_DEVICE");
    if (nx_vk_init(&vk, &cfg) != 0) return 1;
    nx_vk_report(&vk, stdout);
    if (pipeline_cache_bad_header_test(&vk) != 0) {
        NX_LOGE("pipeline cache did not recover from a stale driver UUID");
        nx_vk_shutdown(&vk);
        return 1;
    }

    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory img_mem = VK_NULL_HANDLE;
    VkBuffer vbuf = VK_NULL_HANDLE, rbuf = VK_NULL_HANDLE;
    VkDeviceMemory vmem = VK_NULL_HANDLE, rmem = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    int rc = 1;

    if (nx_vk_create_image(&vk, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, (VkExtent3D){W, H, 1}, 1, 1, 1,
                           VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED, &img, &img_mem) != 0) goto fail;
    if (nx_vk_create_buffer(&vk, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &rbuf, &rmem, NULL) != 0) goto fail;
    float verts[6] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f};
    void *vmap = NULL;
    if (nx_vk_create_buffer(&vk, sizeof verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &vbuf, &vmem, &vmap) != 0) goto fail;
    memcpy(vmap, verts, sizeof verts);

    VkAttachmentDescription att = {0};
    att.format = VK_FORMAT_R8G8B8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep[2] = {{0}};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].srcAccessMask = 0; dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rci = {0};
    rci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rci.attachmentCount = 1; rci.pAttachments = &att;
    rci.subpassCount = 1; rci.pSubpasses = &sub;
    rci.dependencyCount = 2; rci.pDependencies = dep;
    VKOK(vkCreateRenderPass(vk.device, &rci, NULL, &rp), "vkCreateRenderPass");

    VkImageViewCreateInfo vci = {0};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    VKOK(vkCreateImageView(vk.device, &vci, NULL, &view), "vkCreateImageView");
    VkFramebufferCreateInfo fci = {0};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = rp; fci.attachmentCount = 1; fci.pAttachments = &view;
    fci.width = W; fci.height = H; fci.layers = 1;
    VKOK(vkCreateFramebuffer(vk.device, &fci, NULL, &fb), "vkCreateFramebuffer");

    vs = nx_vk_shader_module(&vk, k_vs_spv, sizeof k_vs_spv);
    fs = nx_vk_shader_module(&vk, k_fs_spv, sizeof k_fs_spv);
    if (!vs || !fs) goto fail;

    VkPipelineShaderStageCreateInfo stages[2] = {{0}};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";
    VkVertexInputBindingDescription vb = {0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &va;
    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = {0, 0, W, H, 0, 1};
    VkRect2D sc = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps = {0};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs = {0};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    VkPipelineLayoutCreateInfo plci = {0};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VKOK(vkCreatePipelineLayout(vk.device, &plci, NULL, &pl), "vkCreatePipelineLayout");
    VkGraphicsPipelineCreateInfo gp = {0};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pColorBlendState = &cbs;
    gp.layout = pl; gp.renderPass = rp; gp.subpass = 0;
    VKOK(vkCreateGraphicsPipelines(vk.device, vk.pipeline_cache, 1, &gp, NULL, &pipeline), "vkCreateGraphicsPipelines");
    if (pipeline_cache_corruption_test(&vk) != 0) {
        NX_LOGE("pipeline cache did not recover from a truncated file");
        goto fail;
    }

    VkCommandBufferAllocateInfo cai = {0};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = vk.cmd_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VKOK(vkAllocateCommandBuffers(vk.device, &cai, &cb), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo cbi = {0};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKOK(vkBeginCommandBuffer(cb, &cbi), "vkBeginCommandBuffer");
    VkClearValue clear = {0};
    clear.color = (VkClearColorValue){{0.0f, 0.0f, 1.0f, 1.0f}};
    VkRenderPassBeginInfo rbi = {0};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = rp; rbi.framebuffer = fb; rbi.renderArea = sc; rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &off);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    VkBufferImageCopy br = {0};
    br.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    br.imageExtent = (VkExtent3D){W, H, 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &br);
    VkBufferMemoryBarrier bmb = {0};
    bmb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bmb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bmb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb.buffer = rbuf; bmb.offset = 0; bmb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bmb, 0, NULL);
    VKOK(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
    VkFenceCreateInfo fci2 = {0};
    fci2.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VKOK(vkCreateFence(vk.device, &fci2, NULL, &fence), "vkCreateFence");
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cb;
    VKOK(vkQueueSubmit(vk.queue, 1, &submit, fence), "vkQueueSubmit");
    VKOK(vkWaitForFences(vk.device, 1, &fence, VK_TRUE, 2000000000ull), "vkWaitForFences");

    void *rmap = NULL;
    VKOK(vkMapMemory(vk.device, rmem, 0, VK_WHOLE_SIZE, 0, &rmap), "vkMapMemory");
    const uint8_t *px = rmap;
    const uint8_t *center = px + (H / 2 * W + W / 2) * 4;
    const uint8_t *corner = px + (8 * W + 8) * 4;
    printf("center RGBA: %u %u %u %u\ncorner RGBA: %u %u %u %u\n", center[0], center[1], center[2], center[3], corner[0], corner[1], corner[2], corner[3]);
    if (center[0] > 240 && center[1] < 16 && center[2] < 16 && center[3] > 240 &&
        corner[0] < 16 && corner[1] < 16 && corner[2] > 240 && corner[3] > 240) {
        printf("Vulkan offscreen smoke: PASS\n");
        rc = 0;
    } else {
        printf("Vulkan offscreen smoke: FAIL (unexpected pixels)\n");
    }
    vkUnmapMemory(vk.device, rmem);
fail:
    if (fence) vkDestroyFence(vk.device, fence, NULL);
    if (cb) vkFreeCommandBuffers(vk.device, vk.cmd_pool, 1, &cb);
    if (pipeline) vkDestroyPipeline(vk.device, pipeline, NULL);
    if (pl) vkDestroyPipelineLayout(vk.device, pl, NULL);
    if (vs) vkDestroyShaderModule(vk.device, vs, NULL);
    if (fs) vkDestroyShaderModule(vk.device, fs, NULL);
    if (fb) vkDestroyFramebuffer(vk.device, fb, NULL);
    if (rp) vkDestroyRenderPass(vk.device, rp, NULL);
    if (view) vkDestroyImageView(vk.device, view, NULL);
    nx_vk_destroy_buffer(&vk, vbuf, vmem);
    nx_vk_destroy_buffer(&vk, rbuf, rmem);
    nx_vk_destroy_image(&vk, img, img_mem);
    nx_vk_shutdown(&vk);
    return rc;
}
