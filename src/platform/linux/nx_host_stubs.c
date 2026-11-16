// Host-side holes filled while the corresponding Asahi subsystem is staged.
// These are weak so the real NVN/audio/front-end implementations replace them
// without touching the service code. None reports a capability it does not
// have: diagnostics are no-ops, and the shutdown path really exits.
#include "dm.h"
#include "front.h"
#include "platform/linux/nx_platform.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__thread _Atomic int dm_host_work __attribute__((weak));

__attribute__((weak)) void dm_diag_samplers_start(void) {}
__attribute__((weak)) void dm_cheats_init(void) { NX_LOGI("cheats: not available on this build"); }
__attribute__((weak)) void probe_install(void) {}
__attribute__((weak)) void dm_ue_malloc_install(void) {}
__attribute__((weak)) int dm_ue_heap_owns(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }

__attribute__((weak)) void dm_audio_suspend(int on) { (void)on; }
__attribute__((weak)) void dm_gfx_suspend(int on) { (void)on; }
__attribute__((weak)) void dm_gfx_quit(int wait_ms) { (void)wait_ms; }
__attribute__((weak)) void dm_gfx_quit_begin(void) {}
__attribute__((weak)) void dm_gfx_enter(void) {}
__attribute__((weak)) void dm_gfx_leave(void) {}
__attribute__((weak)) void mtl_present_screen(uint32_t w, uint32_t h) { (void)w; (void)h; }
__attribute__((weak)) int nvn_fps_high_now(void) { return 0; }

__attribute__((weak)) void dm_gfx_shot_at(long k, const char *path) { (void)k; (void)path; }
__attribute__((weak)) void dm_gfx_texlist(int target) { (void)target; }
__attribute__((weak)) void dm_gfx_restop(void) {}
__attribute__((weak)) void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; }
__attribute__((weak)) void dm_gfx_bufsnap(const char *key, uint64_t a, size_t n, const char *path) { (void)key; (void)a; (void)n; (void)path; }
__attribute__((weak)) void dm_gfx_texdump_next(const char *path) { (void)path; }
__attribute__((weak)) void dm_gfx_passdump_next(long n) { (void)n; }
__attribute__((weak)) void dm_gfx_addr_note(uint64_t a, char *note, size_t n) { (void)a; if (n) note[0] = 0; }
__attribute__((weak)) void probe_atrace(uint64_t lo, uint64_t n) { (void)lo; (void)n; }

void dm_host_shutdown_then(const char *why, void (*then)(void *), void *arg) {
    NX_LOGI("host shutdown: %s", why ? why : "requested");
    dm_gfx_quit_begin();
    dm_gfx_quit(0);
    if (then) then(arg);
    _exit(0);
}

void dm_host_shutdown(const char *why) { dm_host_shutdown_then(why, NULL, NULL); }

static void term_handler(int sig) { (void)sig; dm_host_shutdown("SIGTERM"); }

void dm_term_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = term_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
}

// NVN diagnostics: no instruction/cycle counter and no cheat VM in this build.
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; if (buf && nbytes) memset(buf, 0, nbytes); return 0; }
void dm_cheats_present(void) {}
void *objc_autoreleasePoolPush(void) { return NULL; }
void objc_autoreleasePoolPop(void *p) { (void)p; }

// Camera/chat blank native images: zero-filled host memory, a real blank image.
static pthread_mutex_t g_blank_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { void *p; size_t n; } g_blank[32];
uint32_t dm_native_image_blank(uint32_t w, uint32_t h) {
    size_t n = (size_t)w * h * 4;
    if (!n) return 0;
    void *p = calloc(1, n);
    if (!p) return 0;
    pthread_mutex_lock(&g_blank_mu);
    for (int i = 0; i < 32; i++) if (!g_blank[i].p) { g_blank[i].p = p; g_blank[i].n = n; pthread_mutex_unlock(&g_blank_mu); return 0x80000001u + (uint32_t)i; }
    pthread_mutex_unlock(&g_blank_mu);
    free(p);
    LOG("[RED] native images: more than 32 blank images");
    return 0;
}
void dm_native_image_blank_free(uint32_t handle) {
    if (!(handle >> 31)) return;
    unsigned k = (handle & 0x7fffffffu) - 1;
    if (k >= 32) return;
    pthread_mutex_lock(&g_blank_mu);
    free(g_blank[k].p); g_blank[k].p = NULL; g_blank[k].n = 0;
    pthread_mutex_unlock(&g_blank_mu);
}
