// Native Vulkan device layer for Asahi Linux.
//
// Everything here is queried from the installed loader/driver at runtime. There
// are no device-name checks, no assumed extension set and no 4 KiB page
// assumptions. The device is the Mesa Honeykrisp Vulkan device on Asahi, but the
// same code runs on llvmpipe for headless tests.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_vi.h>

typedef struct {
    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    char driver_name[VK_MAX_DRIVER_NAME_SIZE];
    char driver_info[VK_MAX_DRIVER_INFO_SIZE];
    uint32_t api_version;
    uint32_t driver_version;
    uint32_t vendor_id;
    uint32_t device_id;
    VkPhysicalDeviceType device_type;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memory;
    VkPhysicalDeviceFeatures features;
    VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing;
    uint32_t queue_family;
    uint32_t queue_count;
    VkQueueFlags queue_flags;
    uint32_t n_extensions;
    char **extensions;
    int is_cpu;
} NxVkCaps;

typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool cmd_pool;
    VkDebugUtilsMessengerEXT debug_messenger;
    VkPipelineCache pipeline_cache;
    NxVkCaps caps;
    int validation;
    // Presentation (optional): a VkSurfaceKHR from the platform window.
    VkSurfaceKHR surface;
    void *window;                  // NxWindow* the surface belongs to (for extent queries)
    uint32_t present_family;
    VkSwapchainKHR swapchain;
    VkFormat swap_format;
    VkColorSpaceKHR swap_color_space;
    VkExtent2D swap_extent;
    uint32_t swap_count;
    VkImage swap_images[8];
    VkImageView swap_views[8];
    int swap_dirty;
} NxVk;

typedef struct {
    const char *app_name;
    uint32_t app_version;
    int enable_validation;
    int allow_cpu;                 // 0: prefer a real GPU and only fall back if none exists
    const char *device_filter;     // optional substring match on VkPhysicalDeviceProperties.deviceName
    const char *const *instance_extensions;
    uint32_t instance_extension_count;
    int enable_swapchain;
    void *window;                  // NxWindow*; when set, a Vulkan surface and swapchain are created
} NxVkConfig;

int nx_vk_init(NxVk *vk, const NxVkConfig *cfg);
void nx_vk_shutdown(NxVk *vk);

// Print the capability matrix a bug report needs.
void nx_vk_report(const NxVk *vk, FILE *out);

// Memory helpers.
int nx_vk_memory_type(const NxVk *vk, uint32_t type_bits, VkMemoryPropertyFlags want, uint32_t *out);
int nx_vk_create_buffer(NxVk *vk, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
                        VkBuffer *buf, VkDeviceMemory *mem, void **mapped);
void nx_vk_destroy_buffer(NxVk *vk, VkBuffer buf, VkDeviceMemory mem);
int nx_vk_create_image(NxVk *vk, VkImageType type, VkFormat fmt, VkExtent3D extent, uint32_t mips, uint32_t layers,
                       uint32_t samples, VkImageTiling tiling, VkImageUsageFlags usage, VkImageLayout initial,
                       VkImage *img, VkDeviceMemory *mem);
void nx_vk_destroy_image(NxVk *vk, VkImage img, VkDeviceMemory mem);

VkShaderModule nx_vk_shader_module(NxVk *vk, const uint32_t *spirv, size_t bytes);

// One-shot command recording. nx_vk_oneshot_begin returns a command buffer in
// the recording state; nx_vk_oneshot_end ends, submits on the device queue and
// waits for completion.
VkCommandBuffer nx_vk_oneshot_begin(NxVk *vk);
int nx_vk_oneshot_end(NxVk *vk, VkCommandBuffer cb);

// Timeline helpers.
int nx_vk_wait_idle(NxVk *vk);

// Presentation: blit an image into the window's swapchain and present it. The
// image is left in `src_layout`. Returns 0 on success, 1 when there is no
// surface/swapchain (headless), -1 on a Vulkan failure.
int nx_vk_present_image(NxVk *vk, VkImage src, VkImageLayout src_layout, uint32_t width, uint32_t height,
                        const int crop[4], int flip_y);
int nx_vk_has_surface(const NxVk *vk);

// Capability checks used before creating pipelines.
int nx_vk_supports_extension(const NxVk *vk, const char *name);
VkFormat nx_vk_pick_depth_format(const NxVk *vk);

// Pipeline cache persistence (also available from nx_vk_cache.c).
int nx_vk_pipeline_cache_load(NxVk *vk, const char *path);
int nx_vk_pipeline_cache_save(NxVk *vk, const char *path);
