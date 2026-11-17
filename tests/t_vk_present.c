// Swapchain presentation path: a headless VkSurfaceKHR, a source image, and
// nx_vk_present_image (the same blit/present code NVN uses). Skips when the
// installed driver has no VK_EXT_headless_surface.
#include "graphics/vulkan/nx_vk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int has_ext(const char *name) {
    uint32_t n = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &n, NULL) != VK_SUCCESS) return 0;
    VkExtensionProperties *p = calloc(n ? n : 1, sizeof *p);
    vkEnumerateInstanceExtensionProperties(NULL, &n, p);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, name)) found = 1;
    free(p);
    return found;
}
int main(void) {
    if (!has_ext(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)) { printf("SKIP: VK_EXT_headless_surface is not available\n"); return 0; }
    const char *iext[] = {VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    VkApplicationInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "nxnative present test"; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai; ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = iext;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("SKIP: no Vulkan instance\n"); return 0; }
    uint32_t nd = 0;
    vkEnumeratePhysicalDevices(inst, &nd, NULL);
    VkPhysicalDevice *devs = calloc(nd ? nd : 1, sizeof *devs);
    vkEnumeratePhysicalDevices(inst, &nd, devs);
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    uint32_t qf = 0;
    for (uint32_t i = 0; i < nd && !phys; i++) {
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, NULL);
        VkQueueFamilyProperties *q = calloc(nq ? nq : 1, sizeof *q);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, q);
        for (uint32_t k = 0; k < nq; k++) if (q[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) { phys = devs[i]; qf = k; break; }
        free(q);
    }
    free(devs);
    if (!phys) { printf("SKIP: no graphics device\n"); vkDestroyInstance(inst, NULL); return 0; }
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, ep);
    int swap = 0;
    for (uint32_t i = 0; i < ne; i++) if (!strcmp(ep[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) swap = 1;
    free(ep);
    if (!swap) { printf("SKIP: no swapchain extension\n"); vkDestroyInstance(inst, NULL); return 0; }
    VkHeadlessSurfaceCreateInfoEXT hci = {0};
    hci.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateHeadlessSurfaceEXT(inst, &hci, NULL, &surface) != VK_SUCCESS) { printf("SKIP: no headless surface\n"); vkDestroyInstance(inst, NULL); return 0; }
    VkBool32 present = 0;
    vkGetPhysicalDeviceSurfaceSupportKHR(phys, qf, surface, &present);
    if (!present) { printf("SKIP: headless surface cannot present\n"); vkDestroySurfaceKHR(inst, surface, NULL); vkDestroyInstance(inst, NULL); return 0; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qci.queueFamilyIndex = qf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char *dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dext;
    VkDevice dev = VK_NULL_HANDLE;
    if (vkCreateDevice(phys, &dci, NULL, &dev) != VK_SUCCESS) { printf("SKIP: no device\n"); vkDestroySurfaceKHR(inst, surface, NULL); vkDestroyInstance(inst, NULL); return 0; }
    VkQueue queue = VK_NULL_HANDLE; vkGetDeviceQueue(dev, qf, 0, &queue);
    VkCommandPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pci.queueFamilyIndex = qf; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    NxVk vk = {0};
    vk.instance = inst; vk.physical = phys; vk.device = dev; vk.queue = queue; vk.queue_family = qf; vk.present_family = qf; vk.surface = surface; vk.cmd_pool = pool;
    vkGetPhysicalDeviceProperties(phys, &vk.caps.props);
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
    if (nx_vk_create_image(&vk, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, (VkExtent3D){64, 64, 1}, 1, 1, 1,
                           VK_IMAGE_TILING_OPTIMAL, 0, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED, &img, &mem) != 0) { printf("FAIL: source image\n"); return 1; }
    VkCommandBuffer cb = nx_vk_oneshot_begin(&vk);
    VkImageMemoryBarrier b = {0};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img; b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue red = {{1, 0, 0, 1}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, &red, 1, &range);
    nx_vk_oneshot_end(&vk, cb);
    int r = nx_vk_present_image(&vk, img, VK_IMAGE_LAYOUT_GENERAL, 64, 64, NULL, 0);
    if (r == 1) { printf("SKIP: presentation not available\n"); return 0; }
    if (r != 0 || !vk.swapchain || !vk.swap_count) { printf("FAIL: present returned %d, swapchain %p count %u\n", r, (void *)vk.swapchain, vk.swap_count); return 1; }
    printf("presentation: swapchain %ux%u, %u images, present ok\n", vk.swap_extent.width, vk.swap_extent.height, vk.swap_count);
    printf("VK PRESENT TEST PASSED\n");
    return 0;
}
