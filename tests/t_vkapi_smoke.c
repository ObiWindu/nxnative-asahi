// Synthetic Vulkan client for the interception layer. It uses the same entry
// table the guest uses, creates a real instance/device on Honeykrisp, clears an
// offscreen image, copies it back and verifies the pixels. No window or surface
// is required, so this runs headless on a server.
#include "graphics/vkapi/vk_api.h"
#include "platform/linux/nx_platform.h"
#include "spv_smoke.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_vi.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define VKF(n) ((PFN_##n)nx_vkapi_lookup(#n))

int main(void) {
    char capture_dir[] = "/tmp/nx-vk-capture-XXXXXX";
    CHECK(mkdtemp(capture_dir) != NULL, "shader capture directory created");
    setenv("NX_VK_SHADER_CAPTURE_DIR", capture_dir, 1);
    if (nx_vkapi_init() != 0) return 1;
    nx_vkapi_set_game_identity("0100000000000000", "00112233445566778899aabbccddeeff");

    PFN_vkEnumerateInstanceExtensionProperties enum_inst_ext = VKF(vkEnumerateInstanceExtensionProperties);
    CHECK(enum_inst_ext != NULL, "vkEnumerateInstanceExtensionProperties is in the guest entry table");
    if (enum_inst_ext) {
        uint32_t n = 0;
        VkResult r = enum_inst_ext(NULL, &n, NULL);
        VkExtensionProperties *p = calloc(n ? n : 1, sizeof *p);
        int found = 0;
        if (r == VK_SUCCESS && p) { enum_inst_ext(NULL, &n, p); for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, VK_NN_VI_SURFACE_EXTENSION_NAME)) found = 1; }
        free(p);
        CHECK(r == VK_SUCCESS && found, "VK_NN_vi_surface is advertised to the guest");
    }

    PFN_vkCreateInstance create_instance = VKF(vkCreateInstance);
    CHECK(create_instance != NULL, "vkCreateInstance is in the guest entry table");
    if (!create_instance) return 1;
    VkApplicationInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "vkapi-smoke";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = create_instance(&ici, NULL, &inst);
    CHECK(r == VK_SUCCESS && inst, "intercepted vkCreateInstance creates a real instance (r=%d)", (int)r);
    if (r != VK_SUCCESS) return 1;

    PFN_vkEnumeratePhysicalDevices enum_dev = VKF(vkEnumeratePhysicalDevices);
    PFN_vkGetPhysicalDeviceProperties props = VKF(vkGetPhysicalDeviceProperties);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qprops = VKF(vkGetPhysicalDeviceQueueFamilyProperties);
    uint32_t nd = 0;
    enum_dev(inst, &nd, NULL);
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    enum_dev(inst, &nd, &phys);
    CHECK(nd == 1 && phys, "one Switch-shaped physical device is exposed");
    VkPhysicalDeviceProperties pp;
    props(phys, &pp);
    CHECK(pp.apiVersion == VK_API_VERSION_1_1, "guest sees Vulkan 1.1");
    CHECK(strstr(pp.deviceName, "Tegra") != NULL, "guest sees a Tegra device (%s)", pp.deviceName);
    printf("device: %s, vendor 0x%04x, device 0x%04x\n", pp.deviceName, pp.vendorID, pp.deviceID);

    uint32_t nq = 0;
    qprops(phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq ? nq : 1, sizeof *qf);
    qprops(phys, &nq, qf);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
    CHECK(family != UINT32_MAX, "a graphics queue family exists (%u families)", nq);
    free(qf);
    if (family == UINT32_MAX) return 1;

    PFN_vkCreateDevice create_dev = VKF(vkCreateDevice);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    r = create_dev(phys, &dci, NULL, &dev);
    CHECK(r == VK_SUCCESS && dev, "intercepted vkCreateDevice creates a real device (r=%d)", (int)r);
    if (r != VK_SUCCESS) return 1;

    PFN_vkGetDeviceQueue getq = VKF(vkGetDeviceQueue);
    VkQueue queue = VK_NULL_HANDLE;
    getq(dev, family, 0, &queue);
    CHECK(queue != VK_NULL_HANDLE, "device queue returned");

    PFN_vkCreateShaderModule create_shader = VKF(vkCreateShaderModule);
    PFN_vkDestroyShaderModule destroy_shader = VKF(vkDestroyShaderModule);
    VkShaderModuleCreateInfo smci = {0}; smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = sizeof k_vs_spv; smci.pCode = k_vs_spv;
    VkShaderModule shader = VK_NULL_HANDLE;
    CHECK(create_shader && create_shader(dev, &smci, NULL, &shader) == VK_SUCCESS && shader,
          "shader module created through the interception layer");
    int captured = 0, capture_metadata = 0;
    DIR *capture = opendir(capture_dir);
    if (capture) {
        struct dirent *de;
        while ((de = readdir(capture))) {
            size_t len = strlen(de->d_name);
            if (len == 20 && !strcmp(de->d_name + 16, ".spv")) {
                char path[512]; snprintf(path, sizeof path, "%s/%s", capture_dir, de->d_name);
                FILE *f = fopen(path, "rb"); uint32_t words[sizeof k_vs_spv / sizeof k_vs_spv[0]];
                if (f && fread(words, 1, sizeof words, f) == sizeof words && !memcmp(words, k_vs_spv, sizeof words)) captured = 1;
                if (f) fclose(f);
                snprintf(path, sizeof path, "%s/%.*s.json", capture_dir, 16, de->d_name);
                f = fopen(path, "rb");
                if (f) { char meta[512] = {0}; size_t n = fread(meta, 1, sizeof meta - 1, f); fclose(f); capture_metadata = n && strstr(meta, "\"code_size\"") && strstr(meta, "\"sha256\"") && strstr(meta, "0100000000000000") && strstr(meta, "00112233445566778899aabbccddeeff"); }
            }
        }
        closedir(capture);
    }
    CHECK(captured, "capture wrote byte-identical, content-addressed SPIR-V");
    CHECK(capture_metadata, "capture wrote size, full digest, title ID and main build ID metadata");
    VkShaderModule fragment_shader = VK_NULL_HANDLE;
    smci.codeSize = sizeof k_fs_spv; smci.pCode = k_fs_spv;
    CHECK(create_shader && create_shader(dev, &smci, NULL, &fragment_shader) == VK_SUCCESS && fragment_shader,
          "fragment module created for stage metadata capture");
    PFN_vkCreateRenderPass create_rp = VKF(vkCreateRenderPass);
    PFN_vkCreateDescriptorSetLayout create_dsl = VKF(vkCreateDescriptorSetLayout);
    PFN_vkDestroyDescriptorSetLayout destroy_dsl = VKF(vkDestroyDescriptorSetLayout);
    PFN_vkCreatePipelineLayout create_pl = VKF(vkCreatePipelineLayout);
    PFN_vkCreateGraphicsPipelines create_pipelines = VKF(vkCreateGraphicsPipelines);
    PFN_vkDestroyPipeline destroy_pipeline = VKF(vkDestroyPipeline);
    PFN_vkDestroyPipelineLayout destroy_pl = VKF(vkDestroyPipelineLayout);
    PFN_vkDestroyRenderPass destroy_rp = VKF(vkDestroyRenderPass);
    VkAttachmentDescription att = {0}; att.format = VK_FORMAT_R8G8B8A8_UNORM; att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpci = {0}; rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1; rpci.pAttachments = &att; rpci.subpassCount = 1; rpci.pSubpasses = &sub;
    VkRenderPass rp = VK_NULL_HANDLE;
    CHECK(create_rp && create_rp(dev, &rpci, NULL, &rp) == VK_SUCCESS && rp, "render pass created for stage capture pipeline");
    VkDescriptorSetLayoutBinding dsl_binding = {0};
    dsl_binding.binding = 3; dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    dsl_binding.descriptorCount = 2; dsl_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo dslci = {0}; dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 1; dslci.pBindings = &dsl_binding;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    CHECK(create_dsl && create_dsl(dev, &dslci, NULL, &dsl) == VK_SUCCESS && dsl,
          "descriptor set layout created for binding metadata capture");
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 16, 32};
    VkPipelineLayoutCreateInfo plci = {0}; plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    CHECK(create_pl && create_pl(dev, &plci, NULL, &pl) == VK_SUCCESS && pl, "pipeline layout created for stage capture pipeline");
    VkPipelineShaderStageCreateInfo stages[2] = {{0}};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = shader; stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment_shader; stages[1].pName = "main";
    VkVertexInputBindingDescription vbind = {0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vattr = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi = {0}; vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vbind;
    vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &vattr;
    VkPipelineInputAssemblyStateCreateInfo ia = {0}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = {0, 0, 32, 32, 0, 1}; VkRect2D sc = {{0, 0}, {32, 32}};
    VkPipelineViewportStateCreateInfo vps = {0}; vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs = {0}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {0}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {0}; cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs = {0}; cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gp = {0}; gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = stages; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cbs; gp.layout = pl; gp.renderPass = rp;
    VkPipeline pipeline = VK_NULL_HANDLE;
    CHECK(create_pipelines && create_pipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipeline) == VK_SUCCESS && pipeline,
          "graphics pipeline created through the interception layer");
    int captured_vertex_stage = 0, captured_fragment_stage = 0, captured_layout = 0;
    capture = opendir(capture_dir);
    if (capture) {
        struct dirent *de;
        while ((de = readdir(capture))) {
            if (!strstr(de->d_name, ".use-") || !strstr(de->d_name, ".json")) continue;
            char path[512], meta[2048] = {0}; snprintf(path, sizeof path, "%s/%s", capture_dir, de->d_name);
            FILE *f = fopen(path, "rb"); if (!f) continue;
            size_t n = fread(meta, 1, sizeof meta - 1, f); fclose(f);
            if (n && strstr(meta, "\"entry_point\":\"main\"")) {
                if (strstr(meta, "\"pipeline_layout_key\":\"")) {
                    char *key = strstr(meta, "\"pipeline_layout_key\":\"");
                    if (key) {
                        key += strlen("\"pipeline_layout_key\":\"");
                        char layout_path[512], layout_json[2048] = {0};
                        snprintf(layout_path, sizeof layout_path, "%s/layout-%.*s.json", capture_dir, 16, key);
                        FILE *lf = fopen(layout_path, "rb");
                        if (lf) { size_t ln = fread(layout_json, 1, sizeof layout_json - 1, lf); fclose(lf);
                            captured_layout = ln && strstr(layout_json, "descriptor_set_layout") && strstr(layout_json, "\"binding\":3") &&
                                              strstr(layout_json, "\"descriptor_count\":2") && strstr(layout_json, "\"push_constant_ranges\":[{\"stage_flags\":1,\"offset\":16,\"size\":32}"); }
                    }
                }
                if (strstr(meta, "\"stage\":\"vert\"")) captured_vertex_stage = 1;
                if (strstr(meta, "\"stage\":\"frag\"")) captured_fragment_stage = 1;
            }
        }
        closedir(capture);
    }
    CHECK(captured_vertex_stage && captured_fragment_stage,
          "capture recorded stage and entry-point metadata for both pipeline shaders");
    CHECK(captured_layout, "capture recorded descriptor bindings and push-constant ranges with the pipeline stages");
    if (pipeline && destroy_pipeline) destroy_pipeline(dev, pipeline, NULL);
    if (pl && destroy_pl) destroy_pl(dev, pl, NULL);
    if (dsl && destroy_dsl) destroy_dsl(dev, dsl, NULL);
    if (rp && destroy_rp) destroy_rp(dev, rp, NULL);
    if (destroy_shader && shader) destroy_shader(dev, shader, NULL);
    if (destroy_shader && fragment_shader) destroy_shader(dev, fragment_shader, NULL);

    PFN_vkGetPhysicalDeviceMemoryProperties memprops = VKF(vkGetPhysicalDeviceMemoryProperties);
    VkPhysicalDeviceMemoryProperties mp;
    memprops(phys, &mp);
    CHECK(mp.memoryTypeCount > 0 && mp.memoryHeapCount > 0, "memory types/heaps reported (%u/%u)", mp.memoryTypeCount, mp.memoryHeapCount);

    // Offscreen image an image clear can write and a copy can read.
    PFN_vkCreateImage create_image = VKF(vkCreateImage);
    PFN_vkGetImageMemoryRequirements img_req = VKF(vkGetImageMemoryRequirements);
    PFN_vkAllocateMemory alloc_mem = VKF(vkAllocateMemory);
    PFN_vkBindImageMemory bind_img = VKF(vkBindImageMemory);
    PFN_vkCreateBuffer create_buf = VKF(vkCreateBuffer);
    PFN_vkGetBufferMemoryRequirements buf_req = VKF(vkGetBufferMemoryRequirements);
    PFN_vkBindBufferMemory bind_buf = VKF(vkBindBufferMemory);
    const uint32_t W = 64, H = 64;
    VkImageCreateInfo ici2 = {0};
    ici2.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici2.imageType = VK_IMAGE_TYPE_2D; ici2.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici2.extent = (VkExtent3D){W, H, 1}; ici2.mipLevels = 1; ici2.arrayLayers = 1;
    ici2.samples = VK_SAMPLE_COUNT_1_BIT; ici2.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici2.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici2.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    CHECK(create_image(dev, &ici2, NULL, &img) == VK_SUCCESS && img, "image created through the interception layer");
    VkMemoryRequirements ir;
    img_req(dev, img, &ir);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((ir.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = i; break; }
    CHECK(mt != UINT32_MAX, "image memory type found (%u types)", mp.memoryTypeCount);
    VkMemoryAllocateInfo mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = ir.size; mai.memoryTypeIndex = mt;
    VkDeviceMemory imem = VK_NULL_HANDLE;
    CHECK(alloc_mem(dev, &mai, NULL, &imem) == VK_SUCCESS && imem, "image memory allocated (%llu bytes)", (unsigned long long)ir.size);
    CHECK(bind_img(dev, img, imem, 0) == VK_SUCCESS, "image memory bound");

    VkBufferCreateInfo bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = W * H * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    CHECK(create_buf(dev, &bci, NULL, &buf) == VK_SUCCESS && buf, "readback buffer created");
    VkMemoryRequirements br;
    buf_req(dev, buf, &br);
    uint32_t bmt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((br.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { bmt = i; break; }
    CHECK(bmt != UINT32_MAX, "host-visible readback memory type found");
    VkMemoryAllocateInfo bmai = {0};
    bmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bmai.allocationSize = br.size; bmai.memoryTypeIndex = bmt;
    VkDeviceMemory bmem = VK_NULL_HANDLE;
    CHECK(alloc_mem(dev, &bmai, NULL, &bmem) == VK_SUCCESS && bmem, "readback memory allocated");
    CHECK(bind_buf(dev, buf, bmem, 0) == VK_SUCCESS, "readback memory bound");

    PFN_vkCreateCommandPool create_pool = VKF(vkCreateCommandPool);
    PFN_vkAllocateCommandBuffers alloc_cb = VKF(vkAllocateCommandBuffers);
    PFN_vkBeginCommandBuffer begin = VKF(vkBeginCommandBuffer);
    PFN_vkCmdPipelineBarrier barrier = VKF(vkCmdPipelineBarrier);
    PFN_vkCmdClearColorImage clear = VKF(vkCmdClearColorImage);
    PFN_vkCmdCopyImageToBuffer copy = VKF(vkCmdCopyImageToBuffer);
    PFN_vkEndCommandBuffer end = VKF(vkEndCommandBuffer);
    PFN_vkCreateFence create_fence = VKF(vkCreateFence);
    PFN_vkQueueSubmit submit = VKF(vkQueueSubmit);
    PFN_vkWaitForFences wait = VKF(vkWaitForFences);
    VkCommandPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; pci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(create_pool(dev, &pci, NULL, &pool) == VK_SUCCESS && pool, "command pool created");
    VkCommandBufferAllocateInfo cai = {0};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    CHECK(alloc_cb(dev, &cai, &cb) == VK_SUCCESS && cb, "command buffer allocated");
    VkCommandBufferBeginInfo cbi = {0};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(begin(cb, &cbi) == VK_SUCCESS, "command buffer began");
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier ib = {0};
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img; ib.subresourceRange = range;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkClearColorValue color = {{1.0f, 0.0f, 0.0f, 1.0f}};
    clear(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    VkImageMemoryBarrier ib2 = ib;
    ib2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ib2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib2);
    VkBufferImageCopy bic = {0};
    bic.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = (VkExtent3D){W, H, 1};
    copy(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
    VkBufferMemoryBarrier bb = {0};
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = buf; bb.offset = 0; bb.size = VK_WHOLE_SIZE;
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bb, 0, NULL);
    CHECK(end(cb) == VK_SUCCESS, "command buffer ended");
    VkFenceCreateInfo fci = {0};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    CHECK(create_fence(dev, &fci, NULL, &fence) == VK_SUCCESS && fence, "fence created");
    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    CHECK(submit(queue, 1, &si, fence) == VK_SUCCESS, "queue submit succeeded");
    CHECK(wait(dev, 1, &fence, VK_TRUE, 2000000000ull) == VK_SUCCESS, "GPU completed the clear/copy");

    PFN_vkMapMemory map = VKF(vkMapMemory);
    PFN_vkUnmapMemory unmap = VKF(vkUnmapMemory);
    void *p = NULL;
    CHECK(map(dev, bmem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS && p, "readback memory mapped");
    if (p) {
        const uint8_t *px = p;
        const uint8_t *c = px + (H / 2 * W + W / 2) * 4;
        printf("center RGBA: %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        CHECK(c[0] > 240 && c[1] < 16 && c[2] < 16 && c[3] > 240, "clear produced the expected red pixel");
        unmap(dev, bmem);
    }

    PFN_vkDestroyImage destroy_image = VKF(vkDestroyImage);
    PFN_vkDestroyBuffer destroy_buf = VKF(vkDestroyBuffer);
    PFN_vkFreeMemory free_mem = VKF(vkFreeMemory);
    PFN_vkDestroyCommandPool destroy_pool = VKF(vkDestroyCommandPool);
    PFN_vkDestroyFence destroy_fence = VKF(vkDestroyFence);
    PFN_vkDestroyDevice destroy_dev = VKF(vkDestroyDevice);
    PFN_vkDestroyInstance destroy_inst = VKF(vkDestroyInstance);
    destroy_fence(dev, fence, NULL);
    destroy_pool(dev, pool, NULL);
    destroy_buf(dev, buf, NULL);
    destroy_image(dev, img, NULL);
    free_mem(dev, bmem, NULL); free_mem(dev, imem, NULL);
    destroy_dev(dev, NULL);
    destroy_inst(inst, NULL);

    printf("\n%s (%d failure%s)\n", g_fail ? "VKAPI TESTS FAILED" : "VKAPI TESTS PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
