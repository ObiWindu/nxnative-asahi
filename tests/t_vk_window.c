// Windowed presentation through SDL3's offscreen video driver: create a real
// SDL window and Vulkan surface, initialize the device with that window, clear
// a source image, present it and verify that a swapchain was created. This is
// the same path an NVN display texture takes.
#include "platform/linux/nx_window.h"
#include "graphics/vulkan/nx_vk.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    NxWindow *win = nx_window_create("nxnative window test", 320, 240, 1, 0);
    if (!win) { printf("SKIP: no window from the video driver\n"); return 0; }
    NxVk vk;
    NxVkConfig cfg = {0};
    cfg.app_name = "nxnative window test";
    cfg.app_version = 1;
    cfg.window = win;
    cfg.enable_swapchain = 1;
    if (nx_vk_init(&vk, &cfg) != 0) { printf("SKIP: no Vulkan device for the window\n"); nx_window_destroy(win); return 0; }
    if (!nx_vk_has_surface(&vk)) { printf("SKIP: the video driver gave no Vulkan surface\n"); nx_vk_shutdown(&vk); nx_window_destroy(win); return 0; }
    VkBool32 presents = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(vk.physical, vk.present_family, vk.surface, &presents);
    if (!presents) { printf("SKIP: the window's surface cannot present on this driver\n"); nx_vk_shutdown(&vk); nx_window_destroy(win); return 0; }
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
    if (nx_vk_create_image(&vk, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, (VkExtent3D){64, 64, 1}, 1, 1, 1,
                           VK_IMAGE_TILING_OPTIMAL, 0, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED, &img, &mem) != 0) {
        printf("FAIL: source image\n"); nx_vk_shutdown(&vk); nx_window_destroy(win); return 1;
    }
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
    if (r != 0 || !vk.swapchain || !vk.swap_count) {
        printf("FAIL: present returned %d, swapchain %p, %u images\n", r, (void *)vk.swapchain, vk.swap_count);
        return 1;
    }
    printf("window presentation: swapchain %ux%u, %u images\n", vk.swap_extent.width, vk.swap_extent.height, vk.swap_count);
    printf("VK WINDOW TEST PASSED\n");
    nx_vk_shutdown(&vk);
    nx_window_destroy(win);
    return 0;
}
