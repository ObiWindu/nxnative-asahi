// Native window abstraction. On Linux this is SDL3 (Wayland first, X11 fallback)
// loaded at run time, so the build does not need SDL development headers and the
// application still starts with a clear diagnostic when SDL3 is absent.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct NxWindow NxWindow;

NxWindow *nx_window_create(const char *title, int width, int height, int vulkan, int resizable);
void nx_window_destroy(NxWindow *w);
int nx_window_poll(NxWindow *w);              // 1 = keep running, 0 = close requested
void nx_window_size(NxWindow *w, int *width, int *height);
void nx_window_resize(NxWindow *w, int width, int height);
void nx_window_set_title(NxWindow *w, const char *title);
VkSurfaceKHR nx_window_vulkan_surface(NxWindow *w, VkInstance instance);
int nx_window_vulkan_extensions(NxWindow *w, unsigned *count, const char *const **names);
void *nx_window_native_handle(NxWindow *w);   // SDL_Window*, for diagnostics
const char *nx_window_backend(NxWindow *w);   // "wayland", "x11", "dummy", ...
