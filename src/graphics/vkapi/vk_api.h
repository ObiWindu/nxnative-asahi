// Game-facing Vulkan implementation for Asahi Linux.
//
// Titles that ship a Vulkan loader (Hades, Super Mario 3D All-Stars, the Galaxy
// games) already produce standard Vulkan calls and SPIR-V. On Linux the host can
// therefore expose a real Vulkan device directly instead of translating to
// Metal. This layer intercepts the handful of Switch-specific pieces:
//
//   - VK_NN_vi_surface (the guest's vi native window) -> the SDL3 window surface
//   - surface capabilities/swapchain creation (preTransform INHERIT, extents)
//   - Switch-shaped device properties/features (the guest asked for a Tegra X1)
//   - diagnostics and the frame-presented hook
//
// Every other entry point forwards to the installed Mesa driver through the
// generated assembly stubs, preserving the AArch64 calling convention exactly.
#pragma once

struct NxWindow;
typedef struct NxWindow NxWindow;

void nx_vkapi_set_window(NxWindow *w);
NxWindow *nx_vkapi_window(void);
void nx_vkapi_set_present_hook(void (*fn)(void));
void nx_vkapi_set_game_identity(const char *title_id, const char *main_build_id);
void *nx_vkapi_lookup(const char *name);

// Load the real Vulkan loader and fill the global entry slots. Called lazily by
// the first Vulkan call; exposed for the host's startup diagnostics.
int nx_vkapi_init(void);
const char *nx_vkapi_device_name(void);
