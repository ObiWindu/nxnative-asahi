// Linux front end. This first stage is deliberately headless: it runs the
// guest on the calling thread and reports a missing window honestly. The SDL3
// window/input/audio layer plugs into the same interface next.
#include "hle.h"
#include "front.h"
#include "platform/linux/nx_platform.h"
#include "platform/linux/nx_window.h"
#include "graphics/vkapi/vk_api.h"
#include "dm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void (*front_progress_hook)(const char *title, long done, long total) = NULL;

static NxWindow *g_window;
static NxWindow *g_run_window;
static _Atomic int g_guest_done;
static void *guest_thread(void *arg) {
    void (*guest)(void) = arg;
    guest();
    atomic_store_explicit(&g_guest_done, 1, memory_order_release);
    return NULL;
}

void front_run(void (*guest)(void)) {
    /* The Vulkan interception layer needs the surface before the guest creates
       its VI surface. A desktop session gets a real SDL3 window; a headless
       session (SSH, tests) runs the guest on the calling thread. */
    g_run_window = nx_window_create(DM_GAME_NAME, 1280, 720, 1, 1);
    if (!g_run_window) {
        NX_LOGW("frontend: no window; the guest runs headless (Vulkan titles will not present)");
        guest();
        return;
    }
    nx_vkapi_set_window(g_run_window);
    nx_vkapi_set_present_hook(dm_guest_frame_tick);
    g_window = g_run_window;
    atomic_store(&g_guest_done, 0);
    pthread_t th;
    if (pthread_create(&th, NULL, guest_thread, (void *)guest) != 0) {
        NX_LOGE("frontend: cannot start the guest thread; running it on the main thread");
        nx_window_destroy(g_run_window); g_run_window = NULL;
        guest();
        return;
    }
    while (!atomic_load_explicit(&g_guest_done, memory_order_acquire) && nx_window_poll(g_run_window)) {
        struct timespec ts = {0, 4000000};
        nanosleep(&ts, NULL);
    }
    if (atomic_load_explicit(&g_guest_done, memory_order_acquire)) {
        pthread_join(th, NULL);
        NX_LOGI("frontend: guest returned; shutting the host down");
        nx_window_destroy(g_run_window); g_run_window = NULL;
        dm_host_shutdown("guest returned");
    } else {
        NX_LOGI("frontend: window closed; shutting the host down");
        nx_window_destroy(g_run_window); g_run_window = NULL;
        dm_host_shutdown("window closed");
    }
}

void *front_metal_layer(void) { return NULL; }
int front_screen_max_fps(void) { return 60; }
int front_display_pixels(int *w, int *h) {
    if (!g_run_window) return 0;
    int a = 0, b = 0;
    nx_window_size(g_run_window, &a, &b);
    if (a <= 0 || b <= 0) return 0;
    if (w) *w = a;
    if (h) *h = b;
    return 1;
}
int front_screen_variable_refresh(void) { return 0; }
void front_screen_match(int fps) { (void)fps; }
void front_picture_shape(int w, int h) { (void)w; (void)h; }
void front_progress(const char *title, long done, long total) {
    if (title) NX_LOGI("prepare: %s (%ld/%ld)", title, done, total);
}
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap;
    NX_LOGW("software keyboard: no window front end yet");
    return -1;
}
int front_ctrl_support(int min, int max) {
    (void)min; (void)max;
    NX_LOGW("controller applet: no window front end yet");
    return -1;
}
void front_save_failed(int err) { NX_LOGE("save write failed: errno %d", err); }
void front_gpu_lost(void) { NX_LOGE("GPU lost; the graphics device stopped responding"); }

int front_web_open(const char *dir, const char *path, const char *callback, int hidden, const dm_web_opts *opts) {
    (void)dir; (void)path; (void)callback; (void)hidden; (void)opts;
    NX_LOGW("web applet: no web engine on this build");
    return -1;
}
void front_web_show(void) {}
void front_web_send(const char *msg, size_t n) { (void)msg; (void)n; }
void front_web_close(void) {}
void front_web_shot(const char *path) { (void)path; }
void front_program_handoff(int index, const char *name) { NX_LOGI("program handoff: %d (%s)", index, name ? name : "?"); }

// Graphics hooks used by the service layer before the NVN backend is linked.
// They are weak so the real backend replaces them when it is built.
__attribute__((weak)) void mtl_present_layer(uint32_t w, uint32_t h) { (void)w; (void)h; }
__attribute__((weak)) void mtl_present_progress(uint64_t *queued, uint64_t *done) { if (queued) *queued = 0; if (done) *done = 0; }
__attribute__((weak)) double mtl_presented_time(uint64_t n) { (void)n; return -1; }
__attribute__((weak)) uint64_t dm_nvn_frame_count(void) { return 0; }
__attribute__((weak)) int mtl_pso_prebuild_tool(void) { return 0; }

// A window is created by the interactive launcher; expose the same object to
// the Vulkan interception layer when it exists.
NxWindow *nx_front_window(void) { return g_window; }
void nx_front_set_window(NxWindow *w) { g_window = w; }
