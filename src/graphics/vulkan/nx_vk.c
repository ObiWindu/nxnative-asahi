#include "graphics/vulkan/nx_vk.h"
#include "platform/linux/nx_platform.h"
#include "platform/linux/nx_window.h"

#include <stdlib.h>
#include <string.h>

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                               VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)type; (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) NX_LOGE("vulkan: %s", data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) NX_LOGW("vulkan: %s", data->pMessage);
    else NX_LOGD("vulkan: %s", data->pMessage);
    return VK_FALSE;
}

static int has_layer(const char *name) {
    uint32_t n = 0;
    if (vkEnumerateInstanceLayerProperties(&n, NULL) != VK_SUCCESS) return 0;
    VkLayerProperties *p = calloc(n ? n : 1, sizeof *p);
    if (!p) return 0;
    int found = 0;
    if (vkEnumerateInstanceLayerProperties(&n, p) == VK_SUCCESS)
        for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].layerName, name)) { found = 1; break; }
    free(p);
    return found;
}

static int has_instance_ext(const char *name) {
    uint32_t n = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &n, NULL) != VK_SUCCESS) return 0;
    VkExtensionProperties *p = calloc(n ? n : 1, sizeof *p);
    if (!p) return 0;
    int found = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &n, p) == VK_SUCCESS)
        for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, name)) { found = 1; break; }
    free(p);
    return found;
}

static int has_device_ext(VkPhysicalDevice dev, const char *name) {
    uint32_t n = 0;
    if (vkEnumerateDeviceExtensionProperties(dev, NULL, &n, NULL) != VK_SUCCESS) return 0;
    VkExtensionProperties *p = calloc(n ? n : 1, sizeof *p);
    if (!p) return 0;
    int found = 0;
    if (vkEnumerateDeviceExtensionProperties(dev, NULL, &n, p) == VK_SUCCESS)
        for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, name)) { found = 1; break; }
    free(p);
    return found;
}

static void version_str(uint32_t v, char *out, size_t n) {
    snprintf(out, n, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

int nx_vk_init(NxVk *vk, const NxVkConfig *cfg) {
    memset(vk, 0, sizeof *vk);
    vk->pipeline_cache = VK_NULL_HANDLE;
    if (!cfg) return -1;
    vk->validation = cfg->enable_validation;

    const char *exts[16];
    uint32_t next = 0;
    if (has_instance_ext(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        exts[next++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    int debug_ext = 0;
    if (cfg->enable_validation && has_instance_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        exts[next++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        debug_ext = 1;
    }
    for (uint32_t i = 0; i < cfg->instance_extension_count && next < 15; i++)
        if (cfg->instance_extensions[i] && has_instance_ext(cfg->instance_extensions[i])) exts[next++] = cfg->instance_extensions[i];
    // SDL's offscreen driver creates a headless surface; enabling the extension
    // when the loader has it also gives CI a real VkSurfaceKHR to present to.
    if (next < 15 && has_instance_ext(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME))
        exts[next++] = VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME;
    if (cfg->window) {
        unsigned nw = 0; const char *const *wn = NULL;
        if (nx_window_vulkan_extensions((NxWindow *)cfg->window, &nw, &wn) == 0) {
            for (unsigned i = 0; i < nw && next < 16; i++)
                if (wn[i] && has_instance_ext(wn[i])) exts[next++] = wn[i];
        } else NX_LOGW("window does not provide Vulkan instance extensions; presentation is disabled");
    }

    const char *layers[1];
    uint32_t nlayers = 0;
    if (cfg->enable_validation) {
        if (has_layer("VK_LAYER_KHRONOS_validation")) layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
        else NX_LOGW("Vulkan validation requested but VK_LAYER_KHRONOS_validation is not installed; continuing without it");
    }

    VkApplicationInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = cfg->app_name ? cfg->app_name : "nxnative-asahi";
    ai.applicationVersion = cfg->app_version ? cfg->app_version : 1;
    ai.pEngineName = "nxnative";
    ai.engineVersion = 1;
    ai.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = next;
    ici.ppEnabledExtensionNames = exts;
    ici.enabledLayerCount = nlayers;
    ici.ppEnabledLayerNames = layers;
    VkResult r = vkCreateInstance(&ici, NULL, &vk->instance);
    if (r != VK_SUCCESS) {
        NX_LOGE("vkCreateInstance failed: %d", (int)r);
        return -1;
    }
    vk->window = cfg->window;
    if (cfg->window) {
        vk->surface = nx_window_vulkan_surface((NxWindow *)cfg->window, vk->instance);
        if (vk->surface == VK_NULL_HANDLE) NX_LOGW("no Vulkan surface from the window; presentation is disabled");
    }
    if (debug_ext) {
        VkDebugUtilsMessengerCreateInfoEXT dci = {0};
        dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = debug_cb;
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(vk->instance, "vkCreateDebugUtilsMessengerEXT");
        if (create) create(vk->instance, &dci, NULL, &vk->debug_messenger);
    }

    uint32_t ndev = 0;
    r = vkEnumeratePhysicalDevices(vk->instance, &ndev, NULL);
    if (r != VK_SUCCESS || !ndev) {
        NX_LOGE("no Vulkan physical devices (vkEnumeratePhysicalDevices %d, count %u)", (int)r, ndev);
        nx_vk_shutdown(vk);
        return -1;
    }
    VkPhysicalDevice *devs = calloc(ndev, sizeof *devs);
    if (!devs) { nx_vk_shutdown(vk); return -1; }
    vkEnumeratePhysicalDevices(vk->instance, &ndev, devs);
    int chosen = -1, fallback = -1;
    for (uint32_t i = 0; i < ndev; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        int cpu = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        if (cpu && !cfg->allow_cpu && fallback < 0) { fallback = (int)i; continue; }
        if (cfg->device_filter && !strstr(p.deviceName, cfg->device_filter)) continue;
        if (cpu && !cfg->allow_cpu) continue;
        chosen = (int)i;
        break;
    }
    if (chosen < 0) chosen = fallback;
    if (chosen < 0 && !cfg->allow_cpu) {
        NX_LOGE("no hardware Vulkan device found (set NX_VK_ALLOW_CPU=1 for llvmpipe diagnostics only)");
        free(devs); nx_vk_shutdown(vk); return -1;
    }
    if (chosen < 0) chosen = 0;
    vk->physical = devs[chosen];
    free(devs);
    VkPhysicalDeviceProperties2 p2 = {0};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    VkPhysicalDeviceDriverProperties dp = {0};
    dp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    p2.pNext = &dp;
    vkGetPhysicalDeviceProperties2(vk->physical, &p2);
    vk->caps.props = p2.properties;
    vkGetPhysicalDeviceFeatures(vk->physical, &vk->caps.features);
    vkGetPhysicalDeviceMemoryProperties(vk->physical, &vk->caps.memory);
    vk->caps.api_version = vk->caps.props.apiVersion;
    vk->caps.driver_version = vk->caps.props.driverVersion;
    vk->caps.vendor_id = vk->caps.props.vendorID;
    vk->caps.device_id = vk->caps.props.deviceID;
    vk->caps.device_type = vk->caps.props.deviceType;
    snprintf(vk->caps.device_name, sizeof vk->caps.device_name, "%s", vk->caps.props.deviceName);
    snprintf(vk->caps.driver_name, sizeof vk->caps.driver_name, "%s", dp.driverName);
    snprintf(vk->caps.driver_info, sizeof vk->caps.driver_info, "%s", dp.driverInfo);
    vk->caps.is_cpu = vk->caps.device_type == VK_PHYSICAL_DEVICE_TYPE_CPU;

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical, &nq, NULL);
    VkQueueFamilyProperties *q = calloc(nq ? nq : 1, sizeof *q);
    if (!q) { nx_vk_shutdown(vk); return -1; }
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical, &nq, q);
    int qf = -1;
    for (uint32_t i = 0; i < nq; i++) {
        if (!(q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 present = VK_TRUE;
        if (vk->surface && vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical, i, vk->surface, &present) != VK_SUCCESS) present = VK_FALSE;
        if (qf < 0 && present) { qf = (int)i; vk->caps.queue_flags = q[i].queueFlags; vk->caps.queue_count = q[i].queueCount; }
        if (present && (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (q[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) { qf = (int)i; vk->caps.queue_flags = q[i].queueFlags; vk->caps.queue_count = q[i].queueCount; break; }
    }
    if (qf < 0) for (uint32_t i = 0; i < nq; i++) {
        if (!(q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        qf = (int)i; vk->caps.queue_flags = q[i].queueFlags; vk->caps.queue_count = q[i].queueCount; break;
    }
    free(q);
    if (qf < 0) {
        NX_LOGE("the selected device has no graphics queue family");
        nx_vk_shutdown(vk);
        return -1;
    }
    vk->queue_family = (uint32_t)qf;
    vk->caps.queue_family = (uint32_t)qf;
    vk->present_family = (uint32_t)qf;

    uint32_t next_ext = 0;
    const char *dexts[16];
    static const char *const wanted[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME,
        VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
        VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    };
    for (size_t i = 0; i < sizeof wanted / sizeof *wanted && next_ext < 15; i++) {
        if (i == 0 && !cfg->enable_swapchain && !vk->surface) continue;
        if (has_device_ext(vk->physical, wanted[i])) dexts[next_ext++] = wanted[i];
    }

    VkPhysicalDeviceFeatures enabled = vk->caps.features;
    /* Leave these off unless a caller asks: they do not affect correctness of
       the core path and some drivers accept them with expensive emulation. */
    enabled.robustBufferAccess = VK_FALSE;
    enabled.sparseBinding = VK_FALSE;
    enabled.sparseResidencyBuffer = VK_FALSE;
    enabled.sparseResidencyImage2D = VK_FALSE;
    enabled.sparseResidencyImage3D = VK_FALSE;
    enabled.sparseResidency2Samples = VK_FALSE;
    enabled.sparseResidency4Samples = VK_FALSE;
    enabled.sparseResidency8Samples = VK_FALSE;
    enabled.sparseResidency16Samples = VK_FALSE;
    enabled.sparseResidencyAliased = VK_FALSE;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = vk->queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &enabled;
    dci.enabledExtensionCount = next_ext;
    dci.ppEnabledExtensionNames = dexts;
    r = vkCreateDevice(vk->physical, &dci, NULL, &vk->device);
    if (r != VK_SUCCESS) {
        NX_LOGE("vkCreateDevice failed: %d", (int)r);
        nx_vk_shutdown(vk);
        return -1;
    }
    vkGetDeviceQueue(vk->device, vk->queue_family, 0, &vk->queue);

    VkCommandPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = vk->queue_family;
    if (vkCreateCommandPool(vk->device, &pci, NULL, &vk->cmd_pool) != VK_SUCCESS) {
        NX_LOGE("vkCreateCommandPool failed");
        nx_vk_shutdown(vk);
        return -1;
    }

    vk->caps.n_extensions = 0;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(vk->physical, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    if (ep) {
        vkEnumerateDeviceExtensionProperties(vk->physical, NULL, &ne, ep);
        vk->caps.extensions = calloc(ne ? ne : 1, sizeof *vk->caps.extensions);
        if (vk->caps.extensions) {
            for (uint32_t i = 0; i < ne; i++) vk->caps.extensions[i] = strdup(ep[i].extensionName);
            vk->caps.n_extensions = ne;
        }
        free(ep);
    }
    NX_LOGI("Vulkan device: %s (%s %s), API %u.%u.%u, queue family %u", vk->caps.device_name, vk->caps.driver_name,
            vk->caps.driver_info, VK_API_VERSION_MAJOR(vk->caps.api_version), VK_API_VERSION_MINOR(vk->caps.api_version),
            VK_API_VERSION_PATCH(vk->caps.api_version), vk->queue_family);
    return 0;
}

void nx_vk_shutdown(NxVk *vk) {
    if (!vk) return;
    if (vk->device) {
        vkDeviceWaitIdle(vk->device);
        for (uint32_t i = 0; i < vk->swap_count; i++) if (vk->swap_views[i]) vkDestroyImageView(vk->device, vk->swap_views[i], NULL);
        vk->swap_count = 0;
        if (vk->swapchain) vkDestroySwapchainKHR(vk->device, vk->swapchain, NULL);
        vk->swapchain = VK_NULL_HANDLE;
        if (vk->pipeline_cache) vkDestroyPipelineCache(vk->device, vk->pipeline_cache, NULL);
        if (vk->cmd_pool) vkDestroyCommandPool(vk->device, vk->cmd_pool, NULL);
        vkDestroyDevice(vk->device, NULL);
    }
    if (vk->debug_messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(vk->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(vk->instance, vk->debug_messenger, NULL);
    }
    for (uint32_t i = 0; i < vk->caps.n_extensions; i++) free(vk->caps.extensions[i]);
    free(vk->caps.extensions);
    if (vk->surface) vkDestroySurfaceKHR(vk->instance, vk->surface, NULL);
    if (vk->instance) vkDestroyInstance(vk->instance, NULL);
    memset(vk, 0, sizeof *vk);
}

int nx_vk_supports_extension(const NxVk *vk, const char *name) {
    if (!vk || !name) return 0;
    for (uint32_t i = 0; i < vk->caps.n_extensions; i++) if (!strcmp(vk->caps.extensions[i], name)) return 1;
    return 0;
}

static const char *mem_flags(VkMemoryPropertyFlags f, char *buf, size_t n) {
    buf[0] = 0;
    if (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) strncat(buf, "DEVICE_LOCAL ", n - strlen(buf) - 1);
    if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) strncat(buf, "HOST_VISIBLE ", n - strlen(buf) - 1);
    if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) strncat(buf, "HOST_COHERENT ", n - strlen(buf) - 1);
    if (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) strncat(buf, "HOST_CACHED ", n - strlen(buf) - 1);
    if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) strncat(buf, "LAZILY_ALLOCATED ", n - strlen(buf) - 1);
    return buf;
}

void nx_vk_report(const NxVk *vk, FILE *out) {
    if (!vk || !out) return;
    char v[32];
    version_str(vk->caps.api_version, v, sizeof v);
    fprintf(out, "Vulkan API: %s\n", v);
    fprintf(out, "GPU: %s\n", vk->caps.device_name);
    fprintf(out, "Vendor: 0x%04x  Device: 0x%04x  Type: %d\n", vk->caps.vendor_id, vk->caps.device_id, (int)vk->caps.device_type);
    version_str(vk->caps.driver_version, v, sizeof v);
    fprintf(out, "Driver: %s %s (0x%s)\n", vk->caps.driver_name, vk->caps.driver_info, v);
    fprintf(out, "Queue family: %u, flags 0x%x, queues %u\n", vk->caps.queue_family, vk->caps.queue_flags, vk->caps.queue_count);
    fprintf(out, "Memory heaps: %u\n", vk->caps.memory.memoryHeapCount);
    for (uint32_t i = 0; i < vk->caps.memory.memoryHeapCount; i++)
        fprintf(out, "  heap %u: %.1f MiB, flags 0x%x\n", i,
                vk->caps.memory.memoryHeaps[i].size / 1048576.0, vk->caps.memory.memoryHeaps[i].flags);
    fprintf(out, "Memory types: %u\n", vk->caps.memory.memoryTypeCount);
    for (uint32_t i = 0; i < vk->caps.memory.memoryTypeCount; i++) {
        char f[160];
        fprintf(out, "  type %u: heap %u, %s\n", i, vk->caps.memory.memoryTypes[i].heapIndex,
                mem_flags(vk->caps.memory.memoryTypes[i].propertyFlags, f, sizeof f));
    }
    fprintf(out, "Extensions: %u\n", vk->caps.n_extensions);
    for (uint32_t i = 0; i < vk->caps.n_extensions; i++) fprintf(out, "  %s\n", vk->caps.extensions[i]);
    fprintf(out, "Limits: maxImage2D %ux%u, maxImage3D %u, maxBuffer %llu, maxMemoryAllocation %llu\n",
            vk->caps.props.limits.maxImageDimension2D, vk->caps.props.limits.maxImageDimension2D,
            vk->caps.props.limits.maxImageDimension3D, (unsigned long long)vk->caps.props.limits.maxStorageBufferRange,
            (unsigned long long)vk->caps.props.limits.maxMemoryAllocationCount);
    fprintf(out, "Compute: workgroup %ux%ux%u, shared %u bytes\n",
            vk->caps.props.limits.maxComputeWorkGroupSize[0], vk->caps.props.limits.maxComputeWorkGroupSize[1],
            vk->caps.props.limits.maxComputeWorkGroupSize[2], vk->caps.props.limits.maxComputeSharedMemorySize);
    fprintf(out, "Features: geometry %d, tessellation %d, dualSrcBlend %d, fragmentStores %d, vertexStores %d, samplerAnisotropy %d\n",
            vk->caps.features.geometryShader, vk->caps.features.tessellationShader, vk->caps.features.dualSrcBlend,
            vk->caps.features.fragmentStoresAndAtomics, vk->caps.features.vertexPipelineStoresAndAtomics,
            vk->caps.features.samplerAnisotropy);
    static const VkFormat fmts[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
                                    VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM,
                                    VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK,
                                    VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_R16G16B16A16_SFLOAT};
    fprintf(out, "Formats:\n");
    for (size_t i = 0; i < sizeof fmts / sizeof *fmts; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(vk->physical, fmts[i], &fp);
        fprintf(out, "  %d: optimal 0x%x linear 0x%x buffer 0x%x\n", (int)fmts[i], fp.optimalTilingFeatures, fp.linearTilingFeatures, fp.bufferFeatures);
    }
}

int nx_vk_memory_type(const NxVk *vk, uint32_t bits, VkMemoryPropertyFlags want, uint32_t *out) {
    for (uint32_t i = 0; i < vk->caps.memory.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        if ((vk->caps.memory.memoryTypes[i].propertyFlags & want) == want) { *out = i; return 0; }
    }
    return -1;
}

int nx_vk_create_buffer(NxVk *vk, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
                        VkBuffer *buf, VkDeviceMemory *mem, void **mapped) {
    *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; if (mapped) *mapped = NULL;
    VkBufferCreateInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(vk->device, &bi, NULL, buf);
    if (r != VK_SUCCESS) { NX_LOGE("vkCreateBuffer(%llu) failed: %d", (unsigned long long)size, (int)r); return -1; }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk->device, *buf, &req);
    uint32_t mt = 0;
    if (nx_vk_memory_type(vk, req.memoryTypeBits, want, &mt) != 0) {
        NX_LOGE("no memory type for buffer %llu (bits 0x%x want 0x%x)", (unsigned long long)size, req.memoryTypeBits, want);
        vkDestroyBuffer(vk->device, *buf, NULL); *buf = VK_NULL_HANDLE; return -1;
    }
    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mt;
    r = vkAllocateMemory(vk->device, &ai, NULL, mem);
    if (r != VK_SUCCESS) { NX_LOGE("vkAllocateMemory(%llu) failed: %d", (unsigned long long)req.size, (int)r); vkDestroyBuffer(vk->device, *buf, NULL); *buf = VK_NULL_HANDLE; return -1; }
    if (vkBindBufferMemory(vk->device, *buf, *mem, 0) != VK_SUCCESS) {
        NX_LOGE("vkBindBufferMemory failed");
        vkFreeMemory(vk->device, *mem, NULL); vkDestroyBuffer(vk->device, *buf, NULL);
        *mem = VK_NULL_HANDLE; *buf = VK_NULL_HANDLE; return -1;
    }
    if (mapped && (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        if (vkMapMemory(vk->device, *mem, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) { *mapped = NULL; return -1; }
    }
    return 0;
}

void nx_vk_destroy_buffer(NxVk *vk, VkBuffer buf, VkDeviceMemory mem) {
    if (buf) vkDestroyBuffer(vk->device, buf, NULL);
    if (mem) vkFreeMemory(vk->device, mem, NULL);
}

int nx_vk_create_image(NxVk *vk, VkImageType type, VkFormat fmt, VkExtent3D extent, uint32_t mips, uint32_t layers,
                       uint32_t samples, VkImageTiling tiling, VkImageUsageFlags usage, VkImageLayout initial,
                       VkImage *img, VkDeviceMemory *mem) {
    *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
    VkImageCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = type;
    ci.format = fmt;
    ci.extent = extent;
    ci.mipLevels = mips;
    ci.arrayLayers = layers;
    ci.samples = (VkSampleCountFlagBits)samples;
    ci.tiling = tiling;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = initial;
    VkResult r = vkCreateImage(vk->device, &ci, NULL, img);
    if (r != VK_SUCCESS) { NX_LOGE("vkCreateImage(%dx%dx%d fmt %d) failed: %d", extent.width, extent.height, extent.depth, (int)fmt, (int)r); return -1; }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, *img, &req);
    uint32_t mt = 0;
    VkMemoryPropertyFlags want = tiling == VK_IMAGE_TILING_LINEAR ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (nx_vk_memory_type(vk, req.memoryTypeBits, want, &mt) != 0 &&
        nx_vk_memory_type(vk, req.memoryTypeBits, 0, &mt) != 0) {
        NX_LOGE("no memory type for image (bits 0x%x)", req.memoryTypeBits);
        vkDestroyImage(vk->device, *img, NULL); *img = VK_NULL_HANDLE; return -1;
    }
    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mt;
    r = vkAllocateMemory(vk->device, &ai, NULL, mem);
    if (r != VK_SUCCESS) { NX_LOGE("image vkAllocateMemory(%llu) failed: %d", (unsigned long long)req.size, (int)r); vkDestroyImage(vk->device, *img, NULL); *img = VK_NULL_HANDLE; return -1; }
    if (vkBindImageMemory(vk->device, *img, *mem, 0) != VK_SUCCESS) {
        NX_LOGE("vkBindImageMemory failed");
        vkFreeMemory(vk->device, *mem, NULL); vkDestroyImage(vk->device, *img, NULL);
        *mem = VK_NULL_HANDLE; *img = VK_NULL_HANDLE; return -1;
    }
    return 0;
}

void nx_vk_destroy_image(NxVk *vk, VkImage img, VkDeviceMemory mem) {
    if (img) vkDestroyImage(vk->device, img, NULL);
    if (mem) vkFreeMemory(vk->device, mem, NULL);
}

VkShaderModule nx_vk_shader_module(NxVk *vk, const uint32_t *spirv, size_t bytes) {
    VkShaderModuleCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = spirv;
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(vk->device, &ci, NULL, &m);
    if (r != VK_SUCCESS) { NX_LOGE("vkCreateShaderModule(%zu bytes) failed: %d", bytes, (int)r); return VK_NULL_HANDLE; }
    return m;
}

VkCommandBuffer nx_vk_oneshot_begin(NxVk *vk) {
    VkCommandBufferAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = vk->cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vk->device, &ai, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cb); return VK_NULL_HANDLE; }
    return cb;
}

int nx_vk_oneshot_end(NxVk *vk, VkCommandBuffer cb) {
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return -1;
    VkFenceCreateInfo fi = {0};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(vk->device, &fi, NULL, &fence) != VK_SUCCESS) return -1;
    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkResult r = vkQueueSubmit(vk->queue, 1, &si, fence);
    if (r == VK_SUCCESS) r = vkWaitForFences(vk->device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(vk->device, fence, NULL);
    vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cb);
    return r == VK_SUCCESS ? 0 : -1;
}

int nx_vk_wait_idle(NxVk *vk) { return vkDeviceWaitIdle(vk->device) == VK_SUCCESS ? 0 : -1; }

VkFormat nx_vk_pick_depth_format(const NxVk *vk) {
    static const VkFormat f[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
    for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(vk->physical, f[i], &p);
        if ((p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
            (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return f[i];
    }
    return VK_FORMAT_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Pipeline cache persistence
// ---------------------------------------------------------------------------
int nx_vk_pipeline_cache_load(NxVk *vk, const char *path) {
    void *data = NULL;
    size_t size = 0;
    VkPipelineCacheCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (path && !nx_read_file(path, &data, &size) && size > 32) {
        VkPipelineCacheHeaderVersionOne h;
        if (size >= sizeof h) {
            memcpy(&h, data, sizeof h);
            if (h.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                h.vendorID == vk->caps.vendor_id && h.deviceID == vk->caps.device_id &&
                h.pipelineCacheUUID[0]) {
                ci.initialDataSize = size;
                ci.pInitialData = data;
                NX_LOGI("pipeline cache: loading %zu bytes from %s", size, path);
            } else {
                NX_LOGW("pipeline cache %s is for another GPU/driver; rebuilding", path);
            }
        }
        free(data);
    }
    VkResult r = vkCreatePipelineCache(vk->device, &ci, NULL, &vk->pipeline_cache);
    if (r != VK_SUCCESS) { NX_LOGE("vkCreatePipelineCache failed: %d", (int)r); vk->pipeline_cache = VK_NULL_HANDLE; return -1; }
    return 0;
}

int nx_vk_pipeline_cache_save(NxVk *vk, const char *path) {
    if (!vk->pipeline_cache || !path) return -1;
    size_t size = 0;
    if (vkGetPipelineCacheData(vk->device, vk->pipeline_cache, &size, NULL) != VK_SUCCESS || !size) return -1;
    void *data = malloc(size);
    if (!data) return -1;
    if (vkGetPipelineCacheData(vk->device, vk->pipeline_cache, &size, data) != VK_SUCCESS) { free(data); return -1; }
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(data); return -1; }
    size_t wrote = fwrite(data, 1, size, f);
    fclose(f); free(data);
    if (wrote != size) { remove(tmp); return -1; }
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    NX_LOGI("pipeline cache: saved %zu bytes to %s", size, path);
    return 0;
}

// ---------------------------------------------------------------------------
// Presentation through the platform window's swapchain
// ---------------------------------------------------------------------------
int nx_vk_has_surface(const NxVk *vk) { return vk && vk->surface != VK_NULL_HANDLE; }

static int swap_create(NxVk *vk) {
    if (!vk->surface) return 1;
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->physical, vk->surface, &caps) != VK_SUCCESS) return -1;
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical, vk->surface, &nf, NULL);
    VkSurfaceFormatKHR *fmts = calloc(nf ? nf : 1, sizeof *fmts);
    if (!fmts) return -1;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical, vk->surface, &nf, fmts);
    static const VkFormat want[] = {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
                                    VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    VkSurfaceFormatKHR chosen = nf ? fmts[0] : (VkSurfaceFormatKHR){VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (size_t w = 0; w < sizeof want / sizeof *want; w++)
        for (uint32_t i = 0; i < nf; i++)
            if (fmts[i].format == want[w] && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = fmts[i]; w = sizeof want / sizeof *want; break; }
    free(fmts);
    uint32_t npm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk->physical, vk->surface, &npm, NULL);
    VkPresentModeKHR *pms = calloc(npm ? npm : 1, sizeof *pms);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (pms) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(vk->physical, vk->surface, &npm, pms);
        int has_fifo = 0;
        for (uint32_t i = 0; i < npm; i++) if (pms[i] == VK_PRESENT_MODE_FIFO_KHR) has_fifo = 1;
        if (!has_fifo && npm) mode = pms[0];
        free(pms);
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int w = 0, h = 0;
        NxWindow *win = (NxWindow *)vk->window;
        if (win) nx_window_size(win, &w, &h);
        if (w > 0 && h > 0) { extent.width = (uint32_t)w; extent.height = (uint32_t)h; }
        else { extent.width = 1280; extent.height = 720; }
        if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }
    if (!extent.width || !extent.height) return 1;   // minimized
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!(caps.supportedUsageFlags & usage)) usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkSwapchainCreateInfoKHR ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = vk->surface;
    ci.minImageCount = count;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = usage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = vk->swapchain;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult scr = vkCreateSwapchainKHR(vk->device, &ci, NULL, &sc);
    if (scr != VK_SUCCESS) { NX_LOGE("vkCreateSwapchainKHR(%ux%u, format %d, %u images) failed: %d", extent.width, extent.height, (int)chosen.format, count, (int)scr); return -1; }
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(vk->device, sc, &n, NULL);
    if (!n || n > 8) { vkDestroySwapchainKHR(vk->device, sc, NULL); return -1; }
    VkImage imgs[8];
    vkGetSwapchainImagesKHR(vk->device, sc, &n, imgs);
    for (uint32_t i = 0; i < vk->swap_count; i++) if (vk->swap_views[i]) vkDestroyImageView(vk->device, vk->swap_views[i], NULL);
    if (vk->swapchain) vkDestroySwapchainKHR(vk->device, vk->swapchain, NULL);
    vk->swapchain = sc;
    vk->swap_format = chosen.format;
    vk->swap_color_space = chosen.colorSpace;
    vk->swap_extent = extent;
    vk->swap_count = n;
    for (uint32_t i = 0; i < n; i++) {
        vk->swap_images[i] = imgs[i];
        VkImageViewCreateInfo vi = {0};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = imgs[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = chosen.format;
        vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(vk->device, &vi, NULL, &vk->swap_views[i]) != VK_SUCCESS) vk->swap_views[i] = VK_NULL_HANDLE;
    }
    vk->swap_dirty = 0;
    NX_LOGI("swapchain: %ux%u, format %d, %u images, mode %d", extent.width, extent.height, (int)chosen.format, n, (int)mode);
    return 0;
}

int nx_vk_present_image(NxVk *vk, VkImage src, VkImageLayout src_layout, uint32_t width, uint32_t height, const int crop[4], int flip_y) {
    if (!vk || !vk->surface || !src) return 1;
    if (!vk->swapchain || vk->swap_dirty) {
        int r = swap_create(vk);
        if (r) return r;
    }
    VkSemaphoreCreateInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore acquire = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
    VkFenceCreateInfo fi = {0};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateSemaphore(vk->device, &si, NULL, &acquire) != VK_SUCCESS ||
        vkCreateSemaphore(vk->device, &si, NULL, &done) != VK_SUCCESS ||
        vkCreateFence(vk->device, &fi, NULL, &fence) != VK_SUCCESS) {
        if (acquire) vkDestroySemaphore(vk->device, acquire, NULL);
        if (done) vkDestroySemaphore(vk->device, done, NULL);
        return -1;
    }
    uint32_t index = 0;
    VkResult ar = vkAcquireNextImageKHR(vk->device, vk->swapchain, UINT64_MAX, acquire, VK_NULL_HANDLE, &index);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
        vk->swap_dirty = 1;
        if (swap_create(vk) != 0) { vkDestroySemaphore(vk->device, acquire, NULL); vkDestroySemaphore(vk->device, done, NULL); vkDestroyFence(vk->device, fence, NULL); return 1; }
        ar = vkAcquireNextImageKHR(vk->device, vk->swapchain, UINT64_MAX, acquire, VK_NULL_HANDLE, &index);
    }
    if (ar != VK_SUCCESS) {
        vkDestroySemaphore(vk->device, acquire, NULL); vkDestroySemaphore(vk->device, done, NULL); vkDestroyFence(vk->device, fence, NULL);
        return ar == VK_ERROR_SURFACE_LOST_KHR ? 1 : -1;
    }
    uint32_t sx = 0, sy = 0, sw = width, sh = height;
    if (crop && crop[2] > 0 && crop[3] > 0) {
        int x = crop[0] < 0 ? 0 : crop[0], y = crop[1] < 0 ? 0 : crop[1];
        if ((uint32_t)x < width) sx = (uint32_t)x;
        if ((uint32_t)y < height) sy = (uint32_t)y;
        if (sx + (uint32_t)crop[2] <= width) sw = (uint32_t)crop[2];
        if (sy + (uint32_t)crop[3] <= height) sh = (uint32_t)crop[3];
    }
    if (sw > width - sx) sw = width - sx;
    if (sh > height - sy) sh = height - sy;
    if (!sw || !sh) {
        vkDestroySemaphore(vk->device, acquire, NULL); vkDestroySemaphore(vk->device, done, NULL); vkDestroyFence(vk->device, fence, NULL);
        return -1;
    }
    VkCommandBufferAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = vk->cmd_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vk->device, &ai, &cb) != VK_SUCCESS) return -1;
    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier sb = {0};
    sb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    sb.oldLayout = src_layout;
    sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    sb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; sb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sb.image = src;
    sb.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    sb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    sb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &sb);
    VkImageMemoryBarrier db = {0};
    db.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    db.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    db.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    db.image = vk->swap_images[index];
    db.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    db.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &db);
    float scale_x = (float)vk->swap_extent.width / (float)sw, scale_y = (float)vk->swap_extent.height / (float)sh;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    if (nx_env_bool("NX_VK_STRETCH", 0)) { scale_x = (float)vk->swap_extent.width / sw; scale_y = (float)vk->swap_extent.height / sh; }
    int32_t dw = (int32_t)(sw * scale + 0.5f), dh = (int32_t)(sh * scale + 0.5f);
    if (dw > (int32_t)vk->swap_extent.width) dw = (int32_t)vk->swap_extent.width;
    if (dh > (int32_t)vk->swap_extent.height) dh = (int32_t)vk->swap_extent.height;
    int32_t dx = ((int32_t)vk->swap_extent.width - dw) / 2, dy = ((int32_t)vk->swap_extent.height - dh) / 2;
    VkImageBlit region = {0};
    region.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[0] = (VkOffset3D){(int32_t)sx, (int32_t)sy, 0};
    region.srcOffsets[1] = (VkOffset3D){(int32_t)(sx + sw), (int32_t)(sy + sh), 1};
    region.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[0] = (VkOffset3D){dx, flip_y ? dy + dh : dy, 0};
    region.dstOffsets[1] = (VkOffset3D){dx + dw, flip_y ? dy : dy + dh, 1};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk->swap_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
    VkImageMemoryBarrier pb = db;
    pb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    pb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pb.dstAccessMask = 0;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &pb);
    if (src_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        VkImageMemoryBarrier rb = sb;
        rb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        rb.newLayout = src_layout;
        rb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        rb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &rb);
    }
    vkEndCommandBuffer(cb);
    VkSubmitInfo sub = {0};
    sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    sub.waitSemaphoreCount = 1; sub.pWaitSemaphores = &acquire; sub.pWaitDstStageMask = &wait;
    sub.commandBufferCount = 1; sub.pCommandBuffers = &cb;
    sub.signalSemaphoreCount = 1; sub.pSignalSemaphores = &done;
    VkResult r = vkQueueSubmit(vk->queue, 1, &sub, fence);
    if (r == VK_SUCCESS) r = vkWaitForFences(vk->device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (r == VK_SUCCESS) {
        VkPresentInfoKHR pi = {0};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &done;
        pi.swapchainCount = 1; pi.pSwapchains = &vk->swapchain; pi.pImageIndices = &index;
        VkResult pr = vkQueuePresentKHR(vk->queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) vk->swap_dirty = 1;
    }
    vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cb);
    vkDestroySemaphore(vk->device, acquire, NULL);
    vkDestroySemaphore(vk->device, done, NULL);
    vkDestroyFence(vk->device, fence, NULL);
    return r == VK_SUCCESS ? 0 : -1;
}
