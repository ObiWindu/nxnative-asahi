#include "platform/linux/nx_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_log_level = NX_LOG_INFO;
static FILE *g_log_file;
static char g_log_path[1024];

void nx_log_set_level(int level) { g_log_level = level; }
int nx_log_level(void) { return g_log_level; }

void nx_log_set_file(const char *path) {
    if (!path || !*path) return;
    pthread_mutex_lock(&g_log_mu);
    if (g_log_file) { fclose(g_log_file); g_log_file = NULL; }
    snprintf(g_log_path, sizeof g_log_path, "%s", path);
    g_log_file = fopen(path, "a");
    pthread_mutex_unlock(&g_log_mu);
    if (!g_log_file) fprintf(stderr, "[dm] cannot open log file %s: %s\n", path, strerror(errno));
}

static const char *level_name(int level) {
    switch (level) {
    case NX_LOG_ERROR: return "E";
    case NX_LOG_WARN: return "W";
    case NX_LOG_DEBUG: return "D";
    default: return "I";
    }
}

void nx_log(int level, const char *fmt, ...) {
    if (level > g_log_level) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_log_mu);
    fprintf(stderr, "[dm %5ld.%03ld %s] %s\n", (long)ts.tv_sec, ts.tv_nsec / 1000000, level_name(level), msg);
    if (g_log_file) {
        fprintf(g_log_file, "[dm %5ld.%03ld %s] %s\n", (long)ts.tv_sec, ts.tv_nsec / 1000000, level_name(level), msg);
        fflush(g_log_file);
    }
    pthread_mutex_unlock(&g_log_mu);
}

_Noreturn void nx_die(const char *fmt, ...) {
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[dm] FATAL %s\n", msg);
    pthread_mutex_lock(&g_log_mu);
    if (g_log_file) { fprintf(g_log_file, "[dm] FATAL %s\n", msg); fflush(g_log_file); }
    pthread_mutex_unlock(&g_log_mu);
    abort();
}

static char g_config[1024], g_cache[1024], g_data[1024], g_save[1024], g_logs[1024], g_temp[1024];
static int g_paths_ready;

static const char *home_dir(void) {
    const char *h = getenv("HOME");
    return h && *h ? h : "/tmp";
}

static void mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0700) && errno != EEXIST) { /* a read-only XDG dir is not fatal here */ }
        *p = '/';
    }
    if (mkdir(tmp, 0700) && errno != EEXIST) { }
}

static void path_init(void) {
    if (g_paths_ready) return;
    const char *xdg;
    if ((xdg = getenv("XDG_CONFIG_HOME")) && *xdg) snprintf(g_config, sizeof g_config, "%s/nxnative-asahi", xdg);
    else snprintf(g_config, sizeof g_config, "%s/.config/nxnative-asahi", home_dir());
    if ((xdg = getenv("XDG_CACHE_HOME")) && *xdg) snprintf(g_cache, sizeof g_cache, "%s/nxnative-asahi", xdg);
    else snprintf(g_cache, sizeof g_cache, "%s/.cache/nxnative-asahi", home_dir());
    if ((xdg = getenv("XDG_DATA_HOME")) && *xdg) snprintf(g_data, sizeof g_data, "%s/nxnative-asahi", xdg);
    else snprintf(g_data, sizeof g_data, "%s/.local/share/nxnative-asahi", home_dir());
    snprintf(g_save, sizeof g_save, "%s/saves", g_data);
    snprintf(g_logs, sizeof g_logs, "%s/logs", g_cache);
    const char *tmp = getenv("XDG_RUNTIME_DIR");
    if (tmp && *tmp) snprintf(g_temp, sizeof g_temp, "%s/nxnative-asahi", tmp);
    else snprintf(g_temp, sizeof g_temp, "/tmp/nxnative-asahi-%u", (unsigned)getuid());
    mkdir_p(g_config); mkdir_p(g_cache); mkdir_p(g_data); mkdir_p(g_save); mkdir_p(g_logs); mkdir_p(g_temp);
    g_paths_ready = 1;
}

const char *nx_config_dir(void) { path_init(); return g_config; }
const char *nx_cache_dir(void)  { path_init(); return g_cache; }
const char *nx_data_dir(void)   { path_init(); return g_data; }
const char *nx_save_dir(void)   { path_init(); return g_save; }
const char *nx_log_dir(void)    { path_init(); return g_logs; }
const char *nx_temp_dir(void)   { path_init(); return g_temp; }

char *nx_data_path(const char *relative) {
    path_init();
    size_t n = strlen(g_data) + strlen(relative) + 2;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s/%s", g_data, relative);
    return p;
}

uint64_t nx_now_ns(void) { return nx_monotonic_ns(); }
uint64_t nx_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void read_first_line(const char *path, char *out, size_t n, const char *prefix) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (prefix && strncmp(line, prefix, strlen(prefix))) continue;
        const char *p = prefix ? line + strlen(prefix) : line;
        while (*p == ' ' || *p == '\t' || *p == '"') p++;
        snprintf(out, n, "%s", p);
        size_t l = strlen(out);
        while (l && (out[l - 1] == '\n' || out[l - 1] == '\r' || out[l - 1] == '"')) out[--l] = 0;
        break;
    }
    fclose(f);
}

void nx_sysinfo_query(NxSysInfo *out) {
    memset(out, 0, sizeof *out);
    struct utsname u;
    if (!uname(&u)) {
        snprintf(out->os, sizeof out->os, "%s %s", u.sysname, u.release);
        snprintf(out->kernel, sizeof out->kernel, "%s", u.release);
        snprintf(out->arch, sizeof out->arch, "%s", u.machine);
    }
    char pretty[192] = "";
    read_first_line("/etc/os-release", pretty, sizeof pretty, "PRETTY_NAME=");
    if (pretty[0]) snprintf(out->os, sizeof out->os, "%s", pretty);
    else read_first_line("/etc/os-release", out->os, sizeof out->os, "NAME=");
    read_first_line("/proc/cpuinfo", out->cpu, sizeof out->cpu, "model name");
    if (!out->cpu[0]) read_first_line("/proc/cpuinfo", out->cpu, sizeof out->cpu, "Processor");
    if (!out->cpu[0]) snprintf(out->cpu, sizeof out->cpu, "Apple Silicon (aarch64)");
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    out->cpus = cpus > 0 ? (unsigned)cpus : 1;
    long pg = sysconf(_SC_PAGESIZE);
    out->host_page_size = pg > 0 ? (size_t)pg : 0;
    out->guest_page_size = 4096;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            unsigned long long v = 0;
            if (sscanf(line, "MemTotal: %llu kB", &v) == 1) out->memory_total_kb = v;
            else if (sscanf(line, "MemAvailable: %llu kB", &v) == 1) out->memory_available_kb = v;
        }
        fclose(f);
    }
}

void nx_sysinfo_print(const NxSysInfo *info) {
    NX_LOGI("system: %s", info->os);
    NX_LOGI("kernel: %s, machine: %s, CPUs: %u", info->kernel, info->arch, info->cpus);
    NX_LOGI("CPU: %s", info->cpu);
    NX_LOGI("pages: host %zu bytes, guest %zu bytes (ratio %zu)", info->host_page_size, info->guest_page_size,
            info->guest_page_size ? info->host_page_size / info->guest_page_size : 0);
    if (info->memory_total_kb)
        NX_LOGI("memory: %.1f GiB total, %.1f GiB available", info->memory_total_kb / 1048576.0, info->memory_available_kb / 1048576.0);
    if (info->host_page_size != 16384)
        NX_LOGW("host page size is %zu; Asahi's supported configuration is 16384", info->host_page_size);
    if (info->guest_page_size > info->host_page_size)
        NX_LOGE("guest page size exceeds host page size; the memory model requires host >= guest");
}

int nx_read_file(const char *path, void **data, size_t *size) {
    *data = NULL; *size = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno;
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 0) { int e = errno; close(fd); return e ? e : EIO; }
    size_t n = (size_t)st.st_size;
    char *p = malloc(n + 1);
    if (!p) { close(fd); return ENOMEM; }
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, p + off, n - off);
        if (r < 0) { if (errno == EINTR) continue; int e = errno; free(p); close(fd); return e; }
        if (!r) break;
        off += (size_t)r;
    }
    close(fd);
    p[off] = 0;
    *data = p; *size = off;
    return 0;
}

const char *nx_env(const char *name) { return getenv(name); }
int nx_env_int(const char *name, int fallback) {
    const char *e = getenv(name);
    if (!e || !*e) return fallback;
    char *end = NULL;
    long v = strtol(e, &end, 0);
    return end && *end == 0 ? (int)v : fallback;
}
int nx_env_bool(const char *name, int fallback) {
    const char *e = getenv(name);
    if (!e || !*e) return fallback;
    if (!strcmp(e, "1") || !strcasecmp(e, "on") || !strcasecmp(e, "yes") || !strcasecmp(e, "true")) return 1;
    if (!strcmp(e, "0") || !strcasecmp(e, "off") || !strcasecmp(e, "no") || !strcasecmp(e, "false")) return 0;
    return atoi(e) != 0;
}
