#include "platform/linux/nx_window.h"
#include "platform/linux/nx_platform.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include "dm_input.h"   // host input seam (dm/host/dm_input.h)
// The unit tests link the platform layer without the HID service; the app has
// the strong definitions in hle_hid.c.
extern void dm_input_set(uint64_t, int32_t, int32_t, int32_t, int32_t) __attribute__((weak));
extern void dm_pad_set_player(int, uint64_t, int32_t, int32_t, int32_t, int32_t) __attribute__((weak));
extern void dm_pad_players(uint32_t) __attribute__((weak));

// SDL3 ABI subset. The runtime library is a distribution package; the dev
// headers are not required. Keep this list in sync with the SDL3 API version
// the project documents. All types below are the ABI types SDL3 uses.
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Gamepad SDL_Gamepad;
typedef uint32_t SDL_InitFlags;
typedef uint64_t SDL_WindowFlags;
typedef uint64_t SDL_JoystickID;

enum { SDL_INIT_VIDEO = 0x00000020u, SDL_INIT_JOYSTICK = 0x00000200u, SDL_INIT_GAMEPAD = 0x00002000u, SDL_INIT_AUDIO = 0x00000010u };
enum { SDL_WINDOW_VULKAN = 0x10000000ull, SDL_WINDOW_RESIZABLE = 0x00000020ull };

typedef struct {
    void *pfn;
} NxSdlSym;

struct NxWindow {
    void *lib;
    SDL_Window *win;
    int vulkan;
    int should_close;
    char backend[32];
    SDL_Gamepad *pads[8];
    int npads;
};

static void *g_sdl;
static int g_sdl_refs;
static int (*p_SDL_Init)(SDL_InitFlags);
static int (*p_SDL_InitSubSystem)(SDL_InitFlags);
static void (*p_SDL_Quit)(void);
static const char *(*p_SDL_GetError)(void);
static SDL_Window *(*p_SDL_CreateWindow)(const char *, int, int, SDL_WindowFlags);
static void (*p_SDL_DestroyWindow)(SDL_Window *);
static int (*p_SDL_PollEvent)(void *);
static int (*p_SDL_GetWindowSizeInPixels)(SDL_Window *, int *, int *);
static const char *(*p_SDL_GetCurrentVideoDriver)(void);
static int (*p_SDL_Vulkan_GetInstanceExtensions)(SDL_Window *, unsigned *, const char *const **);
static int (*p_SDL_Vulkan_CreateSurface)(SDL_Window *, VkInstance, const VkAllocationCallbacks *, VkSurfaceKHR *);
static int (*p_SDL_SetWindowTitle)(SDL_Window *, const char *);
static int (*p_SDL_SetWindowSize)(SDL_Window *, int, int);
static const uint8_t *(*p_SDL_GetKeyboardState)(int *);
static SDL_JoystickID *(*p_SDL_GetGamepads)(int *);
static void (*p_SDL_free)(void *);
static SDL_Gamepad *(*p_SDL_OpenGamepad)(SDL_JoystickID);
static void (*p_SDL_CloseGamepad)(SDL_Gamepad *);
static int (*p_SDL_GetGamepadButton)(SDL_Gamepad *, int);
static int16_t (*p_SDL_GetGamepadAxis)(SDL_Gamepad *, int);
static const char *(*p_SDL_GetGamepadName)(SDL_Gamepad *);

#define LOAD(n) do { *(void **)(&p_##n) = dlsym(g_sdl, #n); if (!p_##n) { NX_LOGE("SDL3 is missing %s: %s", #n, dlerror()); return -1; } } while (0)

static int sdl_load(void) {
    if (g_sdl) return 0;
    g_sdl = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!g_sdl) g_sdl = dlopen("libSDL3.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_sdl) {
        NX_LOGE("SDL3 runtime not found (%s); install SDL3 (Fedora: sdl3)", dlerror());
        return -1;
    }
    LOAD(SDL_Init); LOAD(SDL_InitSubSystem); LOAD(SDL_Quit); LOAD(SDL_GetError);
    LOAD(SDL_CreateWindow); LOAD(SDL_DestroyWindow); LOAD(SDL_PollEvent);
    LOAD(SDL_GetWindowSizeInPixels); LOAD(SDL_GetCurrentVideoDriver);
    LOAD(SDL_Vulkan_GetInstanceExtensions); LOAD(SDL_Vulkan_CreateSurface);
    LOAD(SDL_SetWindowTitle); LOAD(SDL_SetWindowSize);
    LOAD(SDL_GetKeyboardState); LOAD(SDL_GetGamepads); LOAD(SDL_free);
    LOAD(SDL_OpenGamepad); LOAD(SDL_CloseGamepad); LOAD(SDL_GetGamepadButton);
    LOAD(SDL_GetGamepadAxis); LOAD(SDL_GetGamepadName);
    return 0;
}

NxWindow *nx_window_create(const char *title, int width, int height, int vulkan, int resizable) {
    if (sdl_load() != 0) return NULL;
    if (!g_sdl_refs) {
        if (!p_SDL_Init(SDL_INIT_VIDEO)) {
            NX_LOGE("SDL_Init(VIDEO) failed: %s", p_SDL_GetError());
            return NULL;
        }
        if (!p_SDL_InitSubSystem(SDL_INIT_GAMEPAD))
            NX_LOGW("SDL gamepad subsystem unavailable: %s", p_SDL_GetError());
        g_sdl_refs = 1;
    }
    SDL_WindowFlags flags = (vulkan ? SDL_WINDOW_VULKAN : 0) | (resizable ? SDL_WINDOW_RESIZABLE : 0);
    SDL_Window *win = p_SDL_CreateWindow(title, width, height, flags);
    if (!win) {
        NX_LOGE("SDL_CreateWindow(%dx%d) failed: %s", width, height, p_SDL_GetError());
        return NULL;
    }
    NxWindow *w = calloc(1, sizeof *w);
    if (!w) { p_SDL_DestroyWindow(win); return NULL; }
    w->lib = g_sdl;
    w->win = win;
    w->vulkan = vulkan;
    const char *drv = p_SDL_GetCurrentVideoDriver();
    snprintf(w->backend, sizeof w->backend, "%s", drv ? drv : "unknown");
    NX_LOGI("window: %dx%d, backend %s, Vulkan %s", width, height, w->backend, vulkan ? "yes" : "no");
    return w;
}

void nx_window_destroy(NxWindow *w) {
    if (!w) return;
    if (w->win) p_SDL_DestroyWindow(w->win);
    free(w);
}

// SDL3 scancodes are stable across platforms. The mapping is documented in
// docs/TROUBLESHOOTING.md and can be changed without touching the HID service.
static uint64_t keyboard_buttons(const uint8_t *k) {
    uint64_t b = 0;
    if (!k) return 0;
    if (k[44] || k[29]) b |= DM_BTN_A;        // Space / Z
    if (k[27]) b |= DM_BTN_B;                 // X
    if (k[6]) b |= DM_BTN_X;                  // C
    if (k[25]) b |= DM_BTN_Y;                 // V
    if (k[20]) b |= DM_BTN_L;                 // Q
    if (k[8]) b |= DM_BTN_R;                  // E
    if (k[30]) b |= DM_BTN_ZL;                // 1
    if (k[31]) b |= DM_BTN_ZR;                // 2
    if (k[40]) b |= DM_BTN_PLUS;              // Return
    if (k[42]) b |= DM_BTN_MINUS;             // Backspace
    if (k[82]) b |= DM_BTN_UP;                // Up
    if (k[81]) b |= DM_BTN_DOWN;              // Down
    if (k[80]) b |= DM_BTN_LEFT;              // Left
    if (k[79]) b |= DM_BTN_RIGHT;             // Right
    if (k[22]) b |= DM_BTN_LS;                // S
    if (k[15]) b |= DM_BTN_RS;                // L
    return b;
}

static void poll_input(NxWindow *w) {
    const uint8_t *k = p_SDL_GetKeyboardState(NULL);
    uint64_t kb = keyboard_buttons(k);
    int lx = 0, ly = 0, rx = 0, ry = 0;
    if (k) {
        lx = (k[7] ? DM_STICK_MAX : 0) - (k[4] ? DM_STICK_MAX : 0);    // D / A
        ly = (k[26] ? DM_STICK_MAX : 0) - (k[22] ? DM_STICK_MAX : 0);  // W / S
        rx = (k[15] ? DM_STICK_MAX : 0) - (k[13] ? DM_STICK_MAX : 0);  // L / J
        ry = (k[12] ? DM_STICK_MAX : 0) - (k[14] ? DM_STICK_MAX : 0);  // I / K
        if (k[9]) kb |= DM_BTN_LS;    // F
        if (k[10]) kb |= DM_BTN_RS;   // G
    }
    if (dm_input_set) dm_input_set(kb, lx, ly, rx, ry);

    int n = 0;
    SDL_JoystickID *ids = p_SDL_GetGamepads(&n);
    if (n == 0 && w->npads) {
        for (int i = 0; i < w->npads; i++) if (w->pads[i]) p_SDL_CloseGamepad(w->pads[i]);
        w->npads = 0;
    }
    if (n > 0 && w->npads == 0) {
        SDL_Gamepad *g = p_SDL_OpenGamepad(ids[0]);
        if (g) {
            w->pads[w->npads++] = g;
            NX_LOGI("gamepad connected: %s", p_SDL_GetGamepadName(g));
        }
    }
    if (ids) p_SDL_free(ids);
    if (w->npads > 0) {
        SDL_Gamepad *g = w->pads[0];
        uint64_t b = 0;
        if (p_SDL_GetGamepadButton(g, 0)) b |= DM_BTN_B;
        if (p_SDL_GetGamepadButton(g, 1)) b |= DM_BTN_A;
        if (p_SDL_GetGamepadButton(g, 2)) b |= DM_BTN_Y;
        if (p_SDL_GetGamepadButton(g, 3)) b |= DM_BTN_X;
        if (p_SDL_GetGamepadButton(g, 4)) b |= DM_BTN_MINUS;
        if (p_SDL_GetGamepadButton(g, 6)) b |= DM_BTN_PLUS;
        if (p_SDL_GetGamepadButton(g, 7)) b |= DM_BTN_LS;
        if (p_SDL_GetGamepadButton(g, 8)) b |= DM_BTN_RS;
        if (p_SDL_GetGamepadButton(g, 9)) b |= DM_BTN_L;
        if (p_SDL_GetGamepadButton(g, 10)) b |= DM_BTN_R;
        if (p_SDL_GetGamepadButton(g, 11)) b |= DM_BTN_UP;
        if (p_SDL_GetGamepadButton(g, 12)) b |= DM_BTN_DOWN;
        if (p_SDL_GetGamepadButton(g, 13)) b |= DM_BTN_LEFT;
        if (p_SDL_GetGamepadButton(g, 14)) b |= DM_BTN_RIGHT;
        int ax = p_SDL_GetGamepadAxis(g, 0), ay = -p_SDL_GetGamepadAxis(g, 1);
        int bx = p_SDL_GetGamepadAxis(g, 2), by = -p_SDL_GetGamepadAxis(g, 3);
        if (p_SDL_GetGamepadAxis(g, 4) > 8000) b |= DM_BTN_ZL;
        if (p_SDL_GetGamepadAxis(g, 5) > 8000) b |= DM_BTN_ZR;
        if (dm_pad_set_player) dm_pad_set_player(0, b, ax, ay, bx, by);
        if (dm_pad_players) dm_pad_players(1);
    } else if (dm_pad_players) {
        dm_pad_players(0);
    }
}

int nx_window_poll(NxWindow *w) {
    if (!w) return 0;
    unsigned char ev[256];
    while (p_SDL_PollEvent(ev)) {
        uint32_t type;
        memcpy(&type, ev, sizeof type);
        if (type == 0x100u) w->should_close = 1;          // SDL_EVENT_QUIT
        else if (type == 0x101u) w->should_close = 1;     // SDL_EVENT_WINDOW_CLOSE_REQUESTED
    }
    poll_input(w);
    return !w->should_close;
}

void nx_window_resize(NxWindow *w, int width, int height) {
    if (w && w->win && width > 0 && height > 0) p_SDL_SetWindowSize(w->win, width, height);
}

void nx_window_size(NxWindow *w, int *width, int *height) {
    int a = 0, b = 0;
    if (w && w->win) p_SDL_GetWindowSizeInPixels(w->win, &a, &b);
    if (width) *width = a;
    if (height) *height = b;
}

void nx_window_set_title(NxWindow *w, const char *title) {
    if (w && w->win && title) p_SDL_SetWindowTitle(w->win, title);
}

VkSurfaceKHR nx_window_vulkan_surface(NxWindow *w, VkInstance instance) {
    if (!w || !w->win || !w->vulkan) return VK_NULL_HANDLE;
    VkSurfaceKHR s = VK_NULL_HANDLE;
    if (!p_SDL_Vulkan_CreateSurface(w->win, instance, NULL, &s)) {
        NX_LOGE("SDL_Vulkan_CreateSurface failed: %s", p_SDL_GetError());
        return VK_NULL_HANDLE;
    }
    return s;
}

int nx_window_vulkan_extensions(NxWindow *w, unsigned *count, const char *const **names) {
    if (!w || !w->win) return -1;
    if (!p_SDL_Vulkan_GetInstanceExtensions(w->win, count, names)) {
        NX_LOGE("SDL_Vulkan_GetInstanceExtensions failed: %s", p_SDL_GetError());
        return -1;
    }
    return 0;
}

void *nx_window_native_handle(NxWindow *w) { return w ? w->win : NULL; }
const char *nx_window_backend(NxWindow *w) { return w ? w->backend : "none"; }
