// Linux platform services for the Asahi port: paths, logging, system facts.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#define NX_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define NX_PRINTF(a, b)
#endif

enum { NX_LOG_ERROR = 0, NX_LOG_WARN = 1, NX_LOG_INFO = 2, NX_LOG_DEBUG = 3 };

void nx_log_set_level(int level);
int nx_log_level(void);
void nx_log_set_file(const char *path);
NX_PRINTF(2, 3) void nx_log(int level, const char *fmt, ...);
#define NX_LOGE(...) nx_log(NX_LOG_ERROR, __VA_ARGS__)
#define NX_LOGW(...) nx_log(NX_LOG_WARN, __VA_ARGS__)
#define NX_LOGI(...) nx_log(NX_LOG_INFO, __VA_ARGS__)
#define NX_LOGD(...) nx_log(NX_LOG_DEBUG, __VA_ARGS__)

NX_PRINTF(1, 2) _Noreturn void nx_die(const char *fmt, ...);

// XDG Base Directory paths. Each returns a pointer owned by the platform layer
// and valid until the next call; the directory is created on first use.
const char *nx_config_dir(void);
const char *nx_cache_dir(void);
const char *nx_data_dir(void);
const char *nx_save_dir(void);
const char *nx_log_dir(void);
const char *nx_temp_dir(void);

// Resolve a path relative to the application's data directory (read-only game
// profiles and shader tables live under <data>/games/<title id>).
char *nx_data_path(const char *relative);

uint64_t nx_now_ns(void);
uint64_t nx_monotonic_ns(void);

typedef struct {
    char os[128];
    char kernel[128];
    char arch[32];
    char cpu[256];
    unsigned cpus;
    size_t host_page_size;
    size_t guest_page_size;
    uint64_t memory_total_kb;
    uint64_t memory_available_kb;
} NxSysInfo;

void nx_sysinfo_query(NxSysInfo *out);
void nx_sysinfo_print(const NxSysInfo *info);

// Read a whole file into a malloc'd buffer (NUL-terminated). Returns 0 on
// success. On failure returns an errno value and leaves *data NULL.
int nx_read_file(const char *path, void **data, size_t *size);

// Environment helpers, with a stable view for hot paths.
const char *nx_env(const char *name);
int nx_env_int(const char *name, int fallback);
int nx_env_bool(const char *name, int fallback);
