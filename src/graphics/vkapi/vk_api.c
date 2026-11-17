#define VK_NO_PROTOTYPES
#include "graphics/vkapi/vk_api.h"
#include "horizon/linux_compat.h"
#include "platform/linux/nx_platform.h"
#include "platform/linux/nx_window.h"

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_vi.h>

// ---------------------------------------------------------------------------
// Generated entry table
// ---------------------------------------------------------------------------
typedef struct { const char *name; void *fn; } NxVkEntry;
typedef struct { const char *name; void **slot; } NxVkSlot;
extern const NxVkEntry nx_vk_entries[];
extern const size_t nx_vk_entry_count;
extern const NxVkSlot nx_vk_slots[];
extern const size_t nx_vk_slot_count;

void nx_vk_missing(const char *name) {
    NX_LOGE("Vulkan entry point %s was called before the real driver provided it", name);
    abort();
}

// ---------------------------------------------------------------------------
// Loader and state
// ---------------------------------------------------------------------------
static void *g_loader;
static PFN_vkGetInstanceProcAddr g_gipa;
static PFN_vkGetDeviceProcAddr g_gdpa;
static VkInstance g_instance;
static VkPhysicalDevice g_phys;
static VkDevice g_device;
static VkQueue g_queue;
static uint32_t g_queue_family;
static NxWindow *g_window;
static void (*g_present_hook)(void);
static int g_initialized;
static int g_clamped_queues;
static VkPipelineCache g_pipeline_cache;
static int g_pipeline_cache_refs;
static char g_pipeline_cache_path[1200];
typedef struct vk_capture_module_ref {
    uint64_t handle;
    char key[17];
    struct vk_capture_module_ref *next;
} vk_capture_module_ref;
static vk_capture_module_ref *g_vk_capture_modules;
typedef struct vk_capture_layout_ref {
    uint64_t handle;
    char key[17];
    struct vk_capture_layout_ref *next;
} vk_capture_layout_ref;
static vk_capture_layout_ref *g_vk_capture_set_layouts;
static vk_capture_layout_ref *g_vk_capture_pipeline_layouts;
static pthread_mutex_t g_vk_capture_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_vk_capture_tmp_id;
static char g_vk_capture_title_id[32];
static char g_vk_capture_build_id[64];
static VkPipelineCache pipeline_cache_open(void);
static void pipeline_cache_save(VkPipelineCache cache);
static void pipeline_cache_path(char *out, size_t n);

static void *real_global(const char *name) { return g_gipa ? (void *)g_gipa(NULL, name) : NULL; }
static void *real_instance(const char *name) { return g_gipa ? (void *)g_gipa(g_instance, name) : NULL; }
static void *real_device(const char *name) { return g_gdpa ? (void *)g_gdpa(g_device, name) : NULL; }

#define REAL_G(n) ((PFN_##n)real_global(#n))
#define REAL_I(n) ((PFN_##n)real_instance(#n))
#define REAL_D(n) ((PFN_##n)real_device(#n))

static const char *vk_capture_root(void) {
    const char *p = nx_env("NX_VK_SHADER_CAPTURE_DIR");
    if (!p || !*p) p = nx_env("DM_VK_CAPTURE");
    return p && *p ? p : NULL;
}

static int vk_capture_mkdir(const char *root) {
    if (mkdir(root, 0755) == 0 || errno == EEXIST) {
        struct stat st;
        if (!stat(root, &st) && S_ISDIR(st.st_mode)) return 0;
    }
    NX_LOGE("vulkan shader capture: cannot create directory %s (%s)", root, strerror(errno));
    return -1;
}

static int vk_capture_write_atomic(const char *path, const void *data, size_t size) {
    char tmp[1400];
    unsigned serial = ++g_vk_capture_tmp_id;
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.%ld.%u", path, (long)getpid(), serial);
    if (n < 0 || (size_t)n >= sizeof tmp) return -1;
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, size, f) == size && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = 0;
    if (ok && rename(tmp, path) == 0) return 0;
    remove(tmp);
    return -1;
}

static const char *vk_capture_layout_key(vk_capture_layout_ref *list, uint64_t handle) {
    for (vk_capture_layout_ref *p = list; p; p = p->next) if (p->handle == handle) return p->key;
    return NULL;
}

static void vk_capture_layout_forget(vk_capture_layout_ref **list, uint64_t handle) {
    vk_capture_layout_ref **link = list;
    while (*link) {
        if ((*link)->handle == handle) { vk_capture_layout_ref *old = *link; *link = old->next; free(old); return; }
        link = &(*link)->next;
    }
}

static int vk_capture_layout_store(const char *json, size_t size, char key[17]) {
    const char *root = vk_capture_root();
    if (!root || vk_capture_mkdir(root) != 0) return -1;
    uint8_t digest[32]; char full[65], path[1200];
    nx_sha256(json, size, digest);
    for (unsigned i = 0; i < sizeof digest; i++) snprintf(full + i * 2, 3, "%02x", digest[i]);
    memcpy(key, full, 16); key[16] = 0;
    int n = snprintf(path, sizeof path, "%s/layout-%s.json", root, key);
    if (n < 0 || (size_t)n >= sizeof path) return -1;
    return vk_capture_write_atomic(path, json, size);
}

static void vk_capture_set_layout(const VkDescriptorSetLayoutCreateInfo *ci, VkDescriptorSetLayout layout) {
    const char *root = vk_capture_root();
    if (!root || !ci || !layout || ci->bindingCount > 4096) return;
    VkDescriptorSetLayoutBindingFlagsCreateInfo *binding_flags = NULL;
    for (VkBaseInStructure *p = (VkBaseInStructure *)ci->pNext; p; p = (VkBaseInStructure *)p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) { binding_flags = (void *)p; break; }
    char *json = NULL; size_t size = 0;
    FILE *f = open_memstream(&json, &size); if (!f) return;
    fprintf(f, "{\"schema\":1,\"kind\":\"descriptor_set_layout\",\"flags\":%u,\"bindings\":[", ci->flags);
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
        fprintf(f, "%s{\"binding\":%u,\"descriptor_type\":%u,\"descriptor_count\":%u,\"stage_flags\":%u,\"binding_flags\":%u,\"immutable_sampler_count\":%u}",
                i ? "," : "", b->binding, (unsigned)b->descriptorType, b->descriptorCount, b->stageFlags,
                binding_flags && i < binding_flags->bindingCount ? binding_flags->pBindingFlags[i] : 0,
                b->pImmutableSamplers ? b->descriptorCount : 0);
    }
    fputs("]}\n", f);
    if (fclose(f) != 0) { free(json); return; }
    char key[17];
    pthread_mutex_lock(&g_vk_capture_mu);
    int ok = vk_capture_layout_store(json, size, key);
    if (!ok) {
        uint64_t h = (uint64_t)(uintptr_t)layout;
        vk_capture_layout_forget(&g_vk_capture_set_layouts, h);
        vk_capture_layout_ref *entry = calloc(1, sizeof *entry);
        if (entry) { entry->handle = h; memcpy(entry->key, key, sizeof key); entry->next = g_vk_capture_set_layouts; g_vk_capture_set_layouts = entry; }
    } else NX_LOGE("vulkan shader capture: failed to write descriptor layout metadata");
    pthread_mutex_unlock(&g_vk_capture_mu);
    free(json);
}

static void vk_capture_pipeline_layout(const VkPipelineLayoutCreateInfo *ci, VkPipelineLayout layout) {
    const char *root = vk_capture_root();
    if (!root || !ci || !layout || ci->setLayoutCount > 64 || ci->pushConstantRangeCount > 4096) return;
    char *json = NULL; size_t size = 0;
    FILE *f = open_memstream(&json, &size); if (!f) return;
    fprintf(f, "{\"schema\":1,\"kind\":\"pipeline_layout\",\"flags\":%u,\"set_layouts\":[", ci->flags);
    pthread_mutex_lock(&g_vk_capture_mu);
    for (uint32_t i = 0; i < ci->setLayoutCount; i++) {
        const char *set_key = vk_capture_layout_key(g_vk_capture_set_layouts, (uint64_t)(uintptr_t)ci->pSetLayouts[i]);
        fprintf(f, "%s", i ? "," : "");
        if (set_key) {
            char path[1200]; snprintf(path, sizeof path, "%s/layout-%s.json", root, set_key);
            FILE *sf = fopen(path, "rb");
            if (sf) { fputs("{\"key\":", f); fprintf(f, "\"%s\",\"layout\":", set_key); int ch; while ((ch = fgetc(sf)) != EOF) fputc(ch, f); fclose(sf); fputc('}', f); }
            else fprintf(f, "{\"key\":\"%s\",\"unavailable\":true}", set_key);
        } else fputs("{\"key\":null,\"unavailable\":true}", f);
    }
    pthread_mutex_unlock(&g_vk_capture_mu);
    fputs("],\"push_constant_ranges\":[", f);
    for (uint32_t i = 0; i < ci->pushConstantRangeCount; i++) {
        const VkPushConstantRange *r = &ci->pPushConstantRanges[i];
        fprintf(f, "%s{\"stage_flags\":%u,\"offset\":%u,\"size\":%u}", i ? "," : "", r->stageFlags, r->offset, r->size);
    }
    fputs("]}\n", f);
    if (fclose(f) != 0) { free(json); return; }
    char key[17];
    pthread_mutex_lock(&g_vk_capture_mu);
    int ok = vk_capture_layout_store(json, size, key);
    if (!ok) {
        uint64_t h = (uint64_t)(uintptr_t)layout;
        vk_capture_layout_forget(&g_vk_capture_pipeline_layouts, h);
        vk_capture_layout_ref *entry = calloc(1, sizeof *entry);
        if (entry) { entry->handle = h; memcpy(entry->key, key, sizeof key); entry->next = g_vk_capture_pipeline_layouts; g_vk_capture_pipeline_layouts = entry; }
    } else NX_LOGE("vulkan shader capture: failed to write pipeline layout metadata");
    pthread_mutex_unlock(&g_vk_capture_mu);
    free(json);
}

static void vk_capture_json_string(char *out, size_t cap, const char *s) {
    size_t n = 0;
    if (!cap) return;
    out[n++] = '"';
    for (; s && *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) { n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c); }
        else out[n++] = (char)c;
    }
    if (n + 2 <= cap) { out[n++] = '"'; out[n] = 0; }
    else out[cap - 1] = 0;
}

static void vk_capture_module_write(const VkShaderModuleCreateInfo *ci, VkShaderModule module) {
    const char *root = vk_capture_root();
    if (!root || !ci || !ci->pCode || !ci->codeSize || ci->codeSize > (256u << 20)) return;
    uint8_t digest[32];
    nx_sha256(ci->pCode, ci->codeSize, digest);
    char full[65], key[17];
    for (unsigned i = 0; i < sizeof digest; i++) snprintf(full + i * 2, 3, "%02x", digest[i]);
    memcpy(key, full, 16); key[16] = 0;
    pthread_mutex_lock(&g_vk_capture_mu);
    if (vk_capture_mkdir(root) != 0) { pthread_mutex_unlock(&g_vk_capture_mu); return; }
    char path[1200], side[1200], title[256], build[256], meta[1200];
    snprintf(path, sizeof path, "%s/%s.spv", root, key);
    snprintf(side, sizeof side, "%s/%s.json", root, key);
    const char *env_title = g_vk_capture_title_id[0] ? g_vk_capture_title_id : nx_env("DM_GAME_ID");
    const char *env_build = g_vk_capture_build_id[0] ? g_vk_capture_build_id : nx_env("DM_GAME_BUILD_ID");
    vk_capture_json_string(title, sizeof title, env_title ?: "");
    vk_capture_json_string(build, sizeof build, env_build ?: "");
    int m = snprintf(meta, sizeof meta,
        "{\"schema\":1,\"key\":\"%s\",\"sha256\":\"%s\",\"code_size\":%zu,\"title_id\":%s,\"build_id\":%s}\n",
        key, full, ci->codeSize, title, build);
    int existing = 0;
    FILE *old = fopen(path, "rb");
    if (old) {
        existing = 1;
        if (fseek(old, 0, SEEK_END) != 0 || (size_t)ftell(old) != ci->codeSize || fseek(old, 0, SEEK_SET) != 0) existing = -1;
        else {
            uint8_t *buf = malloc(ci->codeSize);
            if (!buf || fread(buf, 1, ci->codeSize, old) != ci->codeSize || memcmp(buf, ci->pCode, ci->codeSize)) existing = -1;
            free(buf);
        }
        fclose(old);
    }
    int ok = m > 0 && (size_t)m < sizeof meta && existing >= 0;
    if (ok && !existing) ok = vk_capture_write_atomic(path, ci->pCode, ci->codeSize) == 0;
    if (ok) ok = vk_capture_write_atomic(side, meta, (size_t)m) == 0;
    if (!ok) NX_LOGE("vulkan shader capture: failed to write module %s (%zu bytes)", key, ci->codeSize);
    else NX_LOGI("vulkan shader capture: module %s (%zu bytes)", key, ci->codeSize);
    if (ok && module) {
        uint64_t h = (uint64_t)(uintptr_t)module;
        vk_capture_module_ref *entry = g_vk_capture_modules;
        while (entry && entry->handle != h) entry = entry->next;
        if (!entry) { entry = calloc(1, sizeof *entry); if (entry) { entry->next = g_vk_capture_modules; g_vk_capture_modules = entry; } }
        if (entry) { entry->handle = h; memcpy(entry->key, key, sizeof key); }
    }
    pthread_mutex_unlock(&g_vk_capture_mu);
}

static const char *vk_capture_stage_name(VkShaderStageFlagBits stage) {
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT: return "vert";
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return "tesc";
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return "tese";
    case VK_SHADER_STAGE_GEOMETRY_BIT: return "geom";
    case VK_SHADER_STAGE_FRAGMENT_BIT: return "frag";
    case VK_SHADER_STAGE_COMPUTE_BIT: return "comp";
    default: return "other";
    }
}

static void vk_capture_stage(const VkPipelineShaderStageCreateInfo *stage, VkPipelineLayout pipeline_layout) {
    const char *root = vk_capture_root();
    if (!root || !stage || !stage->module || !stage->pName) return;
    char key[17] = {0};
    uint64_t h = (uint64_t)(uintptr_t)stage->module;
    pthread_mutex_lock(&g_vk_capture_mu);
    for (vk_capture_module_ref *p = g_vk_capture_modules; p; p = p->next) if (p->handle == h) { memcpy(key, p->key, sizeof key); break; }
    if (!key[0]) {
        NX_LOGW("vulkan shader capture: pipeline references module %#llx that was not captured", (unsigned long long)h);
        pthread_mutex_unlock(&g_vk_capture_mu); return;
    }
    const char *layout_key = pipeline_layout ? vk_capture_layout_key(g_vk_capture_pipeline_layouts, (uint64_t)(uintptr_t)pipeline_layout) : NULL;
    char entry[512]; vk_capture_json_string(entry, sizeof entry, stage->pName);
    const VkSpecializationInfo *sp = stage->pSpecializationInfo;
    size_t ne = sp ? sp->mapEntryCount : 0, nd = sp ? sp->dataSize : 0;
    if (ne > 4096 || nd > (1u << 20) || (ne && (!sp->pMapEntries || !sp->pData)) || (nd && !sp->pData)) {
        NX_LOGW("vulkan shader capture: ignoring oversized specialization metadata for %s", key);
        pthread_mutex_unlock(&g_vk_capture_mu); return;
    }
    size_t cap = 1100 + strlen(entry) + ne * 112 + nd * 2;
    char *json = malloc(cap);
    if (!json) { pthread_mutex_unlock(&g_vk_capture_mu); return; }
    char layout_value[20] = "null";
    if (layout_key) snprintf(layout_value, sizeof layout_value, "\"%s\"", layout_key);
    size_t at = (size_t)snprintf(json, cap,
        "{\"schema\":1,\"key\":\"%s\",\"stage\":\"%s\",\"stage_flags\":%u,\"entry_point\":%s,\"pipeline_layout_key\":%s,\"specialization_entries\":[",
        key, vk_capture_stage_name(stage->stage), (unsigned)stage->stage, entry, layout_value);
    for (size_t i = 0; i < ne && at < cap; i++) {
        const VkSpecializationMapEntry *e = &sp->pMapEntries[i];
        at += (size_t)snprintf(json + at, cap - at, "%s{\"constant_id\":%u,\"offset\":%u,\"size\":%zu}",
                               i ? "," : "", e->constantID, e->offset, e->size);
    }
    if (at < cap) at += (size_t)snprintf(json + at, cap - at, "],\"specialization_data_hex\":\"");
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < nd && at + 2 < cap; i++) { uint8_t b = ((const uint8_t *)sp->pData)[i]; json[at++] = hex[b >> 4]; json[at++] = hex[b & 15]; }
    if (at + 3 < cap) { json[at++] = '"'; json[at++] = '}'; json[at++] = '\n'; json[at] = 0; }
    if (at >= cap) {
        NX_LOGE("vulkan shader capture: stage metadata overflow for %s", key);
        free(json); pthread_mutex_unlock(&g_vk_capture_mu); return;
    }
    uint8_t sig[32]; nx_sha256(json, at, sig);
    char use_key[17], path[1200];
    for (unsigned i = 0; i < 8; i++) snprintf(use_key + i * 2, 3, "%02x", sig[i]);
    snprintf(path, sizeof path, "%s/%s.use-%s.json", root, key, use_key);
    if (vk_capture_write_atomic(path, json, at) != 0)
        NX_LOGE("vulkan shader capture: failed to write stage metadata for %s", key);
    free(json);
    pthread_mutex_unlock(&g_vk_capture_mu);
}

static void bind_global_slots(void) {
    for (size_t i = 0; i < nx_vk_slot_count; i++) {
        void *p = real_global(nx_vk_slots[i].name);
        if (p && !*nx_vk_slots[i].slot) *nx_vk_slots[i].slot = p;
    }
}
static void bind_instance_slots(void) {
    for (size_t i = 0; i < nx_vk_slot_count; i++) {
        void *p = real_instance(nx_vk_slots[i].name);
        if (p) *nx_vk_slots[i].slot = p;
    }
}
static void bind_device_slots(void) {
    for (size_t i = 0; i < nx_vk_slot_count; i++) {
        void *p = real_device(nx_vk_slots[i].name);
        if (p) *nx_vk_slots[i].slot = p;
    }
}

int nx_vkapi_init(void) {
    if (g_initialized) return 0;
    g_loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_loader) g_loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_loader) {
        NX_LOGE("Vulkan loader not found (%s); install vulkan-loader", dlerror());
        return -1;
    }
    g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_loader, "vkGetInstanceProcAddr");
    g_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(g_loader, "vkGetDeviceProcAddr");
    if (!g_gipa) { NX_LOGE("Vulkan loader has no vkGetInstanceProcAddr"); return -1; }
    bind_global_slots();
    g_initialized = 1;
    return 0;
}

void nx_vkapi_set_window(NxWindow *w) { g_window = w; }
NxWindow *nx_vkapi_window(void) { return g_window; }
void nx_vkapi_set_present_hook(void (*fn)(void)) { g_present_hook = fn; }
void nx_vkapi_set_game_identity(const char *title_id, const char *main_build_id) {
    snprintf(g_vk_capture_title_id, sizeof g_vk_capture_title_id, "%s", title_id ? title_id : "");
    snprintf(g_vk_capture_build_id, sizeof g_vk_capture_build_id, "%s", main_build_id ? main_build_id : "");
}
const char *nx_vkapi_device_name(void) { return g_phys ? "selected" : "none"; }

void *nx_vkapi_lookup(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < nx_vk_entry_count; i++) if (!strcmp(nx_vk_entries[i].name, name)) return nx_vk_entries[i].fn;
    return NULL;
}

// ---------------------------------------------------------------------------
// Instance extension helpers
// ---------------------------------------------------------------------------
static int instance_ext_available(const char *name) {
    PFN_vkEnumerateInstanceExtensionProperties fn = REAL_G(vkEnumerateInstanceExtensionProperties);
    if (!fn) return 0;
    uint32_t n = 0;
    if (fn(NULL, &n, NULL) != VK_SUCCESS) return 0;
    VkExtensionProperties *p = calloc(n ? n : 1, sizeof *p);
    if (!p) return 0;
    int found = 0;
    if (fn(NULL, &n, p) == VK_SUCCESS) for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, name)) { found = 1; break; }
    free(p);
    return found;
}

static int layer_available(const char *name) {
    PFN_vkEnumerateInstanceLayerProperties fn = REAL_G(vkEnumerateInstanceLayerProperties);
    if (!fn) return 0;
    uint32_t n = 0;
    if (fn(&n, NULL) != VK_SUCCESS) return 0;
    VkLayerProperties *p = calloc(n ? n : 1, sizeof *p);
    if (!p) return 0;
    int found = 0;
    if (fn(&n, p) == VK_SUCCESS) for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].layerName, name)) { found = 1; break; }
    free(p);
    return found;
}

static int add_name(const char **list, uint32_t *n, uint32_t cap, const char *name) {
    if (!name || !*name) return 0;
    for (uint32_t i = 0; i < *n; i++) if (!strcmp(list[i], name)) return 0;
    if (*n >= cap) return 0;
    list[(*n)++] = name;
    return 1;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL h_vkEnumerateInstanceVersion(uint32_t *version) {
    if (version) *version = VK_API_VERSION_1_1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL h_vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *props) {
    if (nx_vkapi_init() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateInstanceLayerProperties fn = REAL_G(vkEnumerateInstanceLayerProperties);
    return fn ? fn(count, props) : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL h_vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *props) {
    if (nx_vkapi_init() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer) {
        PFN_vkEnumerateInstanceExtensionProperties fn = REAL_G(vkEnumerateInstanceExtensionProperties);
        return fn ? fn(layer, count, props) : VK_ERROR_LAYER_NOT_PRESENT;
    }
    PFN_vkEnumerateInstanceExtensionProperties fn = REAL_G(vkEnumerateInstanceExtensionProperties);
    uint32_t real_n = 0;
    if (fn) fn(NULL, &real_n, NULL);
    VkExtensionProperties *real = calloc(real_n ? real_n : 1, sizeof *real);
    if (fn && real) fn(NULL, &real_n, real);
    int have_vi = 0;
    for (uint32_t i = 0; i < real_n; i++) if (!strcmp(real[i].extensionName, VK_NN_VI_SURFACE_EXTENSION_NAME)) have_vi = 1;
    uint32_t total = real_n + (have_vi ? 0 : 1);
    if (!props) {
        *count = total;
        free(real);
        return VK_SUCCESS;
    }
    uint32_t want = *count, k = 0;
    for (uint32_t i = 0; i < real_n && k < want; i++) props[k++] = real[i];
    if (!have_vi && k < want) {
        memset(&props[k], 0, sizeof props[k]);
        snprintf(props[k].extensionName, sizeof props[k].extensionName, "%s", VK_NN_VI_SURFACE_EXTENSION_NAME);
        props[k].specVersion = VK_NN_VI_SURFACE_SPEC_VERSION;
        k++;
    }
    free(real);
    *count = k;
    return k < total ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL h_vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkInstance *out) {
    if (nx_vkapi_init() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    if (!ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
    const char *exts[32];
    uint32_t next = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount && next < 32; i++) {
        const char *name = ci->ppEnabledExtensionNames[i];
        if (!strcmp(name, VK_NN_VI_SURFACE_EXTENSION_NAME)) continue;   // ours, not the real driver's
        if (!instance_ext_available(name)) { NX_LOGW("vulkan: guest requested unavailable instance extension %s; dropping it", name); continue; }
        add_name(exts, &next, 32, name);
    }
    if (instance_ext_available(VK_KHR_SURFACE_EXTENSION_NAME)) add_name(exts, &next, 32, VK_KHR_SURFACE_EXTENSION_NAME);
    if (g_window) {
        unsigned nw = 0;
        const char *const *we = NULL;
        if (nx_window_vulkan_extensions(g_window, &nw, &we) == 0)
            for (unsigned i = 0; i < nw && next < 32; i++) if (instance_ext_available(we[i])) add_name(exts, &next, 32, we[i]);
    }
    if (instance_ext_available(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        add_name(exts, &next, 32, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

    const char *layers[8];
    uint32_t nlayers = 0;
    for (uint32_t i = 0; i < ci->enabledLayerCount && nlayers < 8; i++)
        if (layer_available(ci->ppEnabledLayerNames[i])) layers[nlayers++] = ci->ppEnabledLayerNames[i];
        else NX_LOGW("vulkan: guest requested unavailable instance layer %s; dropping it", ci->ppEnabledLayerNames[i]);

    VkApplicationInfo app = {0};
    const VkApplicationInfo *appp = ci->pApplicationInfo;
    if (!appp) {
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "nxnative guest";
        app.apiVersion = VK_API_VERSION_1_1;
        appp = &app;
    } else {
        app = *appp;
        if (app.apiVersion > VK_API_VERSION_1_1) app.apiVersion = VK_API_VERSION_1_1;
        appp = &app;
    }
    VkInstanceCreateInfo ci2 = *ci;
    ci2.pApplicationInfo = appp;
    ci2.enabledExtensionCount = next;
    ci2.ppEnabledExtensionNames = exts;
    ci2.enabledLayerCount = nlayers;
    ci2.ppEnabledLayerNames = layers;

    PFN_vkCreateInstance fn = REAL_G(vkCreateInstance);
    if (!fn) { NX_LOGE("vulkan: real vkCreateInstance missing"); return VK_ERROR_INITIALIZATION_FAILED; }
    VkResult r = fn(&ci2, alloc, out);
    if (r != VK_SUCCESS) { NX_LOGE("vkCreateInstance (real) failed: %d", (int)r); return r; }
    g_instance = *out;
    bind_instance_slots();
    NX_LOGI("vulkan: instance created (guest extensions %u, host extensions %u, layers %u)", ci->enabledExtensionCount, next, nlayers);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL h_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc) {
    PFN_vkDestroyInstance fn = REAL_I(vkDestroyInstance);
    if (fn && instance) fn(instance, alloc);
    if (instance == g_instance) { g_instance = VK_NULL_HANDLE; g_phys = VK_NULL_HANDLE; }
}

VKAPI_ATTR VkResult VKAPI_CALL h_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *count, VkPhysicalDevice *devs) {
    PFN_vkEnumeratePhysicalDevices fn = (PFN_vkEnumeratePhysicalDevices)real_instance("vkEnumeratePhysicalDevices");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t n = 0;
    VkResult r = fn(instance, &n, NULL);
    if (r != VK_SUCCESS || !n) return r == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : r;
    VkPhysicalDevice *all = calloc(n, sizeof *all);
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = fn(instance, &n, all);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) { free(all); return r; }
    int chosen = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        PFN_vkGetPhysicalDeviceProperties propfn = REAL_I(vkGetPhysicalDeviceProperties);
        if (propfn) propfn(all[i], &p); else memset(&p, 0, sizeof p);
        if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) { chosen = (int)i; break; }
    }
    g_phys = all[chosen];
    free(all);
    if (!devs) { *count = 1; return VK_SUCCESS; }
    if (*count < 1) { *count = 1; return VK_INCOMPLETE; }
    devs[0] = g_phys;
    *count = 1;
    return VK_SUCCESS;
}

// Tegra X1 properties, as the Switch driver reports them. The real device is
// more capable; the guest's engine sizing and workarounds expect this shape.
static void switch_props(VkPhysicalDeviceProperties *p) {
    memset(p, 0, sizeof *p);
    p->apiVersion = VK_API_VERSION_1_1;
    p->driverVersion = VK_MAKE_VERSION(1, 0, 0);
    p->vendorID = 0x10de;
    p->deviceID = 0x92ba;
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    snprintf(p->deviceName, sizeof p->deviceName, "%s", "NVIDIA Tegra X1 (Asahi Vulkan)");
    VkPhysicalDeviceLimits *l = &p->limits;
    l->maxImageDimension1D = l->maxImageDimension2D = l->maxImageDimensionCube = 16384;
    l->maxImageDimension3D = 2048; l->maxImageArrayLayers = 2048;
    l->maxTexelBufferElements = 1u << 27; l->maxUniformBufferRange = 65536; l->maxStorageBufferRange = 1u << 30;
    l->maxPushConstantsSize = 256; l->maxMemoryAllocationCount = 4096; l->maxSamplerAllocationCount = 4000;
    l->bufferImageGranularity = 1024; l->maxBoundDescriptorSets = 8;
    l->maxPerStageDescriptorSamplers = 16; l->maxPerStageDescriptorUniformBuffers = 15;
    l->maxPerStageDescriptorStorageBuffers = 16; l->maxPerStageDescriptorSampledImages = 31;
    l->maxPerStageDescriptorStorageImages = 8; l->maxPerStageDescriptorInputAttachments = 8;
    l->maxPerStageResources = 128;
    l->maxDescriptorSetSamplers = 4000; l->maxDescriptorSetUniformBuffers = 90;
    l->maxDescriptorSetUniformBuffersDynamic = 15; l->maxDescriptorSetStorageBuffers = 96;
    l->maxDescriptorSetStorageBuffersDynamic = 16; l->maxDescriptorSetSampledImages = 4096;
    l->maxDescriptorSetStorageImages = 4096; l->maxDescriptorSetInputAttachments = 8;
    l->maxVertexInputAttributes = 31; l->maxVertexInputBindings = 14;
    l->maxVertexInputAttributeOffset = 2047; l->maxVertexInputBindingStride = 2048;
    l->maxVertexOutputComponents = 128; l->maxFragmentInputComponents = 128;
    l->maxFragmentOutputAttachments = 8; l->maxFragmentDualSrcAttachments = 1;
    l->maxFragmentCombinedOutputResources = 16;
    l->maxComputeSharedMemorySize = 32768;
    l->maxComputeWorkGroupCount[0] = l->maxComputeWorkGroupCount[1] = l->maxComputeWorkGroupCount[2] = 65535;
    l->maxComputeWorkGroupInvocations = 1024;
    l->maxComputeWorkGroupSize[0] = l->maxComputeWorkGroupSize[1] = 1024;
    l->maxComputeWorkGroupSize[2] = 64;
    l->subPixelPrecisionBits = 8; l->subTexelPrecisionBits = 8; l->mipmapPrecisionBits = 8;
    l->maxDrawIndexedIndexValue = 0xffffffffu; l->maxDrawIndirectCount = 0xffffffffu;
    l->maxSamplerLodBias = 15; l->maxSamplerAnisotropy = 16; l->maxViewports = 1;
    l->maxViewportDimensions[0] = l->maxViewportDimensions[1] = 16384;
    l->viewportBoundsRange[0] = -32768; l->viewportBoundsRange[1] = 32767;
    l->viewportSubPixelBits = 8;
    l->minMemoryMapAlignment = 64; l->minTexelBufferOffsetAlignment = 256;
    l->minUniformBufferOffsetAlignment = 256; l->minStorageBufferOffsetAlignment = 256;
    l->minTexelOffset = -8; l->maxTexelOffset = 7;
    l->minTexelGatherOffset = -32; l->maxTexelGatherOffset = 31;
    l->minInterpolationOffset = -0.5f; l->maxInterpolationOffset = 0.4375f;
    l->subPixelInterpolationOffsetBits = 4;
    l->maxFramebufferWidth = l->maxFramebufferHeight = 16384; l->maxFramebufferLayers = 2048;
    l->framebufferColorSampleCounts = l->framebufferDepthSampleCounts = l->framebufferStencilSampleCounts = 0x7;
    l->framebufferNoAttachmentsSampleCounts = 0x7; l->maxColorAttachments = 8;
    l->sampledImageColorSampleCounts = l->sampledImageIntegerSampleCounts = 0x7;
    l->sampledImageDepthSampleCounts = l->sampledImageStencilSampleCounts = 0x7;
    l->storageImageSampleCounts = 1; l->maxSampleMaskWords = 1;
    l->timestampComputeAndGraphics = 1; l->timestampPeriod = 1;
    l->maxClipDistances = 8; l->maxCullDistances = 0; l->maxCombinedClipAndCullDistances = 8;
    l->discreteQueuePriorities = 2; l->pointSizeRange[0] = 1; l->pointSizeRange[1] = 511;
    l->lineWidthRange[0] = l->lineWidthRange[1] = 1; l->pointSizeGranularity = 0.0625f;
    l->lineWidthGranularity = 0; l->strictLines = 1; l->standardSampleLocations = 1;
    l->optimalBufferCopyOffsetAlignment = 256; l->optimalBufferCopyRowPitchAlignment = 256;
    l->nonCoherentAtomSize = 64;
}

static void switch_features(VkPhysicalDeviceFeatures *f) {
    memset(f, 0, sizeof *f);
    f->robustBufferAccess = f->fullDrawIndexUint32 = f->imageCubeArray = f->independentBlend = 1;
    f->sampleRateShading = f->dualSrcBlend = f->drawIndirectFirstInstance = f->depthClamp = 1;
    f->depthBiasClamp = f->fillModeNonSolid = f->largePoints = f->samplerAnisotropy = 1;
    f->textureCompressionBC = f->textureCompressionASTC_LDR = f->occlusionQueryPrecise = 1;
    f->fragmentStoresAndAtomics = f->shaderImageGatherExtended = f->shaderClipDistance = 1;
    f->shaderResourceMinLod = 1;
}

VKAPI_ATTR void VKAPI_CALL h_vkGetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p) {
    (void)pd; switch_props(p);
}
VKAPI_ATTR void VKAPI_CALL h_vkGetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) {
    PFN_vkGetPhysicalDeviceProperties2 fn = REAL_I(vkGetPhysicalDeviceProperties2);
    if (fn) fn(pd, p);   // fills the pNext chain with the real driver's answers
    switch_props(&p->properties);
}
VKAPI_ATTR void VKAPI_CALL h_vkGetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) {
    (void)pd; switch_features(f);
}
VKAPI_ATTR void VKAPI_CALL h_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) {
    PFN_vkGetPhysicalDeviceFeatures2 fn = REAL_I(vkGetPhysicalDeviceFeatures2);
    if (fn) fn(pd, f);   // fills the pNext chain with the real driver's answers
    switch_features(&f->features);
}
VKAPI_ATTR void VKAPI_CALL h_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *m) {
    PFN_vkGetPhysicalDeviceMemoryProperties fn = REAL_I(vkGetPhysicalDeviceMemoryProperties);
    if (fn) fn(pd, m); else memset(m, 0, sizeof *m);
}
VKAPI_ATTR void VKAPI_CALL h_vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *m) {
    PFN_vkGetPhysicalDeviceMemoryProperties2 fn = REAL_I(vkGetPhysicalDeviceMemoryProperties2);
    if (fn) fn(pd, m); else memset(&m->memoryProperties, 0, sizeof m->memoryProperties);
}
VKAPI_ATTR void VKAPI_CALL h_vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties *p) {
    PFN_vkGetPhysicalDeviceQueueFamilyProperties fn = REAL_I(vkGetPhysicalDeviceQueueFamilyProperties);
    if (fn) fn(pd, n, p); else *n = 0;
}
VKAPI_ATTR void VKAPI_CALL h_vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties2 *p) {
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 fn = REAL_I(vkGetPhysicalDeviceQueueFamilyProperties2);
    if (fn) fn(pd, n, p); else *n = 0;
}
VKAPI_ATTR void VKAPI_CALL h_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice pd, VkFormat fmt, VkFormatProperties *p) {
    PFN_vkGetPhysicalDeviceFormatProperties fn = REAL_I(vkGetPhysicalDeviceFormatProperties);
    if (fn) fn(pd, fmt, p); else memset(p, 0, sizeof *p);
}
VKAPI_ATTR void VKAPI_CALL h_vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat fmt, VkFormatProperties2 *p) {
    PFN_vkGetPhysicalDeviceFormatProperties2 fn = REAL_I(vkGetPhysicalDeviceFormatProperties2);
    if (fn) fn(pd, fmt, p); else memset(&p->formatProperties, 0, sizeof p->formatProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL h_vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice pd, VkFormat fmt, VkImageType t, VkImageTiling ti,
                                                                         VkImageUsageFlags u, VkImageCreateFlags f, VkImageFormatProperties *p) {
    PFN_vkGetPhysicalDeviceImageFormatProperties fn = REAL_I(vkGetPhysicalDeviceImageFormatProperties);
    return fn ? fn(pd, fmt, t, ti, u, f, p) : VK_ERROR_FORMAT_NOT_SUPPORTED;
}
VKAPI_ATTR VkResult VKAPI_CALL h_vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *ci, VkImageFormatProperties2 *p) {
    PFN_vkGetPhysicalDeviceImageFormatProperties2 fn = REAL_I(vkGetPhysicalDeviceImageFormatProperties2);
    return fn ? fn(pd, ci, p) : VK_ERROR_FORMAT_NOT_SUPPORTED;
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
static int device_ext_available(const char *name) {
    if (!g_phys) return 0;
    uint32_t n = 0;
    PFN_vkEnumerateDeviceExtensionProperties fn = REAL_I(vkEnumerateDeviceExtensionProperties);
    if (!fn || fn(g_phys, NULL, &n, NULL) != VK_SUCCESS) return 0;
    VkExtensionProperties *p = calloc(n ? n : 1, sizeof *p);
    if (!p) return 0;
    int found = 0;
    if (fn(g_phys, NULL, &n, p) == VK_SUCCESS) for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, name)) { found = 1; break; }
    free(p);
    return found;
}

VKAPI_ATTR VkResult VKAPI_CALL h_vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *out) {
    if (!ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
    g_phys = pd;
    const char *exts[32];
    uint32_t next = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount && next < 32; i++) {
        const char *name = ci->ppEnabledExtensionNames[i];
        if (!strcmp(name, VK_NN_VI_SURFACE_EXTENSION_NAME)) continue;
        if (!device_ext_available(name)) { NX_LOGW("vulkan: guest requested unavailable device extension %s; dropping it", name); continue; }
        add_name(exts, &next, 32, name);
    }
    if (device_ext_available(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) add_name(exts, &next, 32, VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    VkDeviceQueueCreateInfo qci[8];
    uint32_t nqci = ci->queueCreateInfoCount > 8 ? 8 : ci->queueCreateInfoCount;
    for (uint32_t i = 0; i < nqci; i++) {
        qci[i] = ci->pQueueCreateInfos[i];
        if (qci[i].queueCount > 1) {
            if (!g_clamped_queues) NX_LOGW("vulkan: guest requested %u queues in family %u; the host exposes one serial queue", qci[i].queueCount, qci[i].queueFamilyIndex);
            qci[i].queueCount = 1;
        }
    }
    VkDeviceCreateInfo ci2 = *ci;
    ci2.queueCreateInfoCount = nqci;
    ci2.pQueueCreateInfos = qci;
    ci2.enabledExtensionCount = next;
    ci2.ppEnabledExtensionNames = exts;
    PFN_vkCreateDevice fn = REAL_I(vkCreateDevice);
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = fn(pd, &ci2, alloc, out);
    if (r != VK_SUCCESS) {
        NX_LOGE("vkCreateDevice (real) failed: %d (extensions %u)", (int)r, next);
        return r;
    }
    g_device = *out;
    g_queue_family = nqci ? qci[0].queueFamilyIndex : 0;
    bind_device_slots();
    PFN_vkGetDeviceQueue getq = REAL_D(vkGetDeviceQueue);
    if (getq) getq(g_device, g_queue_family, 0, &g_queue);
    pipeline_cache_path(g_pipeline_cache_path, sizeof g_pipeline_cache_path);
    g_pipeline_cache = pipeline_cache_open();
    g_pipeline_cache_refs = 0;
    NX_LOGI("vulkan: device created (host extensions %u, queue family %u)", next, g_queue_family);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL h_vkDestroyDevice(VkDevice d, const VkAllocationCallbacks *alloc) {
    if (d == g_device && g_pipeline_cache) {
        pipeline_cache_save(g_pipeline_cache);
        PFN_vkDestroyPipelineCache dc = REAL_D(vkDestroyPipelineCache);
        if (dc) dc(d, g_pipeline_cache, alloc);
        g_pipeline_cache = VK_NULL_HANDLE;
        g_pipeline_cache_refs = 0;
    }
    PFN_vkDestroyDevice fn = REAL_D(vkDestroyDevice);
    if (fn && d) fn(d, alloc);
    if (d == g_device) {
        pthread_mutex_lock(&g_vk_capture_mu);
        while (g_vk_capture_modules) { vk_capture_module_ref *next = g_vk_capture_modules->next; free(g_vk_capture_modules); g_vk_capture_modules = next; }
        while (g_vk_capture_set_layouts) { vk_capture_layout_ref *next = g_vk_capture_set_layouts->next; free(g_vk_capture_set_layouts); g_vk_capture_set_layouts = next; }
        while (g_vk_capture_pipeline_layouts) { vk_capture_layout_ref *next = g_vk_capture_pipeline_layouts->next; free(g_vk_capture_pipeline_layouts); g_vk_capture_pipeline_layouts = next; }
        pthread_mutex_unlock(&g_vk_capture_mu);
        g_device = VK_NULL_HANDLE; g_queue = VK_NULL_HANDLE;
    }
}

VKAPI_ATTR void VKAPI_CALL h_vkGetDeviceQueue(VkDevice d, uint32_t family, uint32_t index, VkQueue *q) {
    PFN_vkGetDeviceQueue fn = REAL_D(vkGetDeviceQueue);
    if (!fn) { *q = VK_NULL_HANDLE; return; }
    if (index > 0) {
        if (!g_clamped_queues) { g_clamped_queues = 1; NX_LOGW("vulkan: guest asked for queue %u; returning the single host queue", index); }
        index = 0;
    }
    fn(d, family, index, q);
}
VKAPI_ATTR void VKAPI_CALL h_vkGetDeviceQueue2(VkDevice d, const VkDeviceQueueInfo2 *info, VkQueue *q) {
    if (!info) { *q = VK_NULL_HANDLE; return; }
    VkDeviceQueueInfo2 copy = *info;
    if (copy.queueIndex > 0) copy.queueIndex = 0;
    PFN_vkGetDeviceQueue2 fn = REAL_D(vkGetDeviceQueue2);
    if (fn) fn(d, &copy, q); else h_vkGetDeviceQueue(d, copy.queueFamilyIndex, 0, q);
}

// ---------------------------------------------------------------------------
// Persistent pipeline cache
// ---------------------------------------------------------------------------
static void pipeline_cache_path(char *out, size_t n) {
    VkPhysicalDeviceProperties p;
    PFN_vkGetPhysicalDeviceProperties fn = REAL_I(vkGetPhysicalDeviceProperties);
    if (fn) fn(g_phys, &p); else memset(&p, 0, sizeof p);
    char id[VK_UUID_SIZE * 2 + 1];
    for (unsigned i = 0; i < VK_UUID_SIZE; i++) snprintf(id + i * 2, 3, "%02x", p.pipelineCacheUUID[i]);
    snprintf(out, n, "%s/vk-pipeline-%s-%u.%u.bin", nx_cache_dir(), id,
             VK_API_VERSION_MAJOR(p.driverVersion), VK_API_VERSION_MINOR(p.driverVersion));
}

static VkPipelineCache pipeline_cache_open(void) {
    VkPipelineCache cache = VK_NULL_HANDLE;
    void *data = NULL;
    size_t size = 0;
    if (g_pipeline_cache_path[0] && !nx_read_file(g_pipeline_cache_path, &data, &size) && size > sizeof(VkPipelineCacheHeaderVersionOne)) {
        VkPipelineCacheHeaderVersionOne h;
        memcpy(&h, data, sizeof h);
        VkPhysicalDeviceProperties p;
        PFN_vkGetPhysicalDeviceProperties fn = REAL_I(vkGetPhysicalDeviceProperties);
        if (fn) fn(g_phys, &p); else memset(&p, 0, sizeof p);
        if (h.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && h.vendorID == p.vendorID &&
            h.deviceID == p.deviceID && !memcmp(h.pipelineCacheUUID, p.pipelineCacheUUID, VK_UUID_SIZE)) {
            NX_LOGI("vulkan: pipeline cache %zu bytes from %s", size, g_pipeline_cache_path);
        } else {
            NX_LOGW("vulkan: pipeline cache %s is for another GPU/driver; rebuilding", g_pipeline_cache_path);
            free(data); data = NULL; size = 0;
        }
    }
    VkPipelineCacheCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = size;
    ci.pInitialData = data;
    PFN_vkCreatePipelineCache fn = REAL_D(vkCreatePipelineCache);
    if (!fn || fn(g_device, &ci, NULL, &cache) != VK_SUCCESS) {
        ci.initialDataSize = 0; ci.pInitialData = NULL;
        if (fn) fn(g_device, &ci, NULL, &cache);
    }
    free(data);
    return cache;
}

static void pipeline_cache_save(VkPipelineCache cache) {
    if (!cache || !g_pipeline_cache_path[0]) return;
    PFN_vkGetPipelineCacheData get = REAL_D(vkGetPipelineCacheData);
    if (!get) return;
    size_t size = 0;
    if (get(g_device, cache, &size, NULL) != VK_SUCCESS || !size || size > (64u << 20)) return;
    void *data = malloc(size);
    if (!data) return;
    if (get(g_device, cache, &size, data) != VK_SUCCESS) { free(data); return; }
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_pipeline_cache_path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        size_t w = fwrite(data, 1, size, f);
        fclose(f);
        if (w == size && rename(tmp, g_pipeline_cache_path) == 0) NX_LOGI("vulkan: pipeline cache saved (%zu bytes)", size);
        else remove(tmp);
    }
    free(data);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL h_vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    if (nx_vkapi_init() != 0) return NULL;
    void *p = nx_vkapi_lookup(name);
    if (p) return (PFN_vkVoidFunction)p;
    if (instance) return g_gipa(instance, name);
    return g_gipa ? g_gipa(NULL, name) : NULL;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL h_vkGetDeviceProcAddr(VkDevice device, const char *name) {
    void *p = nx_vkapi_lookup(name);
    if (p) return (PFN_vkVoidFunction)p;
    return (g_gdpa && device) ? g_gdpa(device, name) : NULL;
}

// ---------------------------------------------------------------------------
// Switch VI surface and swapchain
// ---------------------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL h_vkCreateViSurfaceNN(VkInstance instance, const VkViSurfaceCreateInfoNN *ci, const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
    (void)instance; (void)ci; (void)alloc;
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    if (!g_window) {
        NX_LOGE("vulkan: vkCreateViSurfaceNN without a front-end window (headless test?)");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkSurfaceKHR s = nx_window_vulkan_surface(g_window, instance);
    if (!s) return VK_ERROR_SURFACE_LOST_KHR;
    *out = s;
    NX_LOGI("vulkan: VI surface created on the %s window", nx_window_backend(g_window));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL h_vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *alloc) {
    PFN_vkDestroySurfaceKHR fn = REAL_I(vkDestroySurfaceKHR);
    if (fn && surface) fn(instance, surface, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL h_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice pd, uint32_t family, VkSurfaceKHR s, VkBool32 *ok) {
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR fn = REAL_I(vkGetPhysicalDeviceSurfaceSupportKHR);
    return fn ? fn(pd, family, s, ok) : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL h_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c) {
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR fn = REAL_I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = fn(pd, s, c);
    if (r != VK_SUCCESS) return r;
    c->supportedTransforms |= VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR;
    if (c->currentExtent.width == 0xffffffffu) {
        int w = 0, h = 0;
        nx_window_size(g_window, &w, &h);
        if (w > 0 && h > 0) c->currentExtent = (VkExtent2D){(uint32_t)w, (uint32_t)h};
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL h_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkSurfaceFormatKHR *f) {
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR fn = REAL_I(vkGetPhysicalDeviceSurfaceFormatsKHR);
    return fn ? fn(pd, s, n, f) : VK_ERROR_INITIALIZATION_FAILED;
}
VKAPI_ATTR VkResult VKAPI_CALL h_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkPresentModeKHR *m) {
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR fn = REAL_I(vkGetPhysicalDeviceSurfacePresentModesKHR);
    return fn ? fn(pd, s, n, m) : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL h_vkCreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
    if (!ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
    VkSwapchainCreateInfoKHR ci2 = *ci;
    if (ci2.preTransform & VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR) ci2.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    if (ci2.compositeAlpha == VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ci2.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (g_window && ci2.imageExtent.width && ci2.imageExtent.height)
        nx_window_resize(g_window, (int)ci2.imageExtent.width, (int)ci2.imageExtent.height);
    VkSurfaceCapabilitiesKHR caps;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR capsfn = REAL_I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    if (capsfn && capsfn(g_phys, ci2.surface, &caps) == VK_SUCCESS) {
        if (ci2.imageExtent.width < caps.minImageExtent.width || ci2.imageExtent.height < caps.minImageExtent.height ||
            ci2.imageExtent.width > caps.maxImageExtent.width || ci2.imageExtent.height > caps.maxImageExtent.height) {
            VkExtent2D e = caps.currentExtent.width != 0xffffffffu ? caps.currentExtent : ci2.imageExtent;
            NX_LOGW("vulkan: swapchain extent %ux%u outside [%ux%u .. %ux%u], using %ux%u", ci2.imageExtent.width, ci2.imageExtent.height,
                    caps.minImageExtent.width, caps.minImageExtent.height, caps.maxImageExtent.width, caps.maxImageExtent.height, e.width, e.height);
            ci2.imageExtent = e;
        }
        if (ci2.minImageCount < caps.minImageCount) ci2.minImageCount = caps.minImageCount;
        if (caps.maxImageCount && ci2.minImageCount > caps.maxImageCount) ci2.minImageCount = caps.maxImageCount;
        if (ci2.imageUsage & ~caps.supportedUsageFlags) {
            NX_LOGW("vulkan: swapchain usage 0x%x masked to surface capabilities 0x%x", ci2.imageUsage, ci2.imageUsage & caps.supportedUsageFlags);
            ci2.imageUsage &= caps.supportedUsageFlags;
        }
    }
    PFN_vkCreateSwapchainKHR fn = REAL_D(vkCreateSwapchainKHR);
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = fn(d, &ci2, alloc, out);
    if (r != VK_SUCCESS) NX_LOGE("vkCreateSwapchainKHR (real) failed: %d (%ux%u fmt %d)", (int)r, ci2.imageExtent.width, ci2.imageExtent.height, (int)ci2.imageFormat);
    else NX_LOGI("vulkan: swapchain %ux%u, %u images", ci2.imageExtent.width, ci2.imageExtent.height, ci2.minImageCount);
    return r;
}

VKAPI_ATTR void VKAPI_CALL h_vkDestroySwapchainKHR(VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks *alloc) {
    PFN_vkDestroySwapchainKHR fn = REAL_D(vkDestroySwapchainKHR);
    if (fn && sc) fn(d, sc, alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL h_vkGetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR sc, uint32_t *n, VkImage *imgs) {
    PFN_vkGetSwapchainImagesKHR fn = REAL_D(vkGetSwapchainImagesKHR);
    return fn ? fn(d, sc, n, imgs) : VK_ERROR_INITIALIZATION_FAILED;
}
VKAPI_ATTR VkResult VKAPI_CALL h_vkAcquireNextImageKHR(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *idx) {
    PFN_vkAcquireNextImageKHR fn = REAL_D(vkAcquireNextImageKHR);
    return fn ? fn(d, sc, timeout, sem, fence, idx) : VK_ERROR_INITIALIZATION_FAILED;
}
VKAPI_ATTR VkResult VKAPI_CALL h_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) {
    PFN_vkQueuePresentKHR fn = REAL_D(vkQueuePresentKHR);
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = fn(q, pi);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        if (g_present_hook) g_present_hook();
    } else if (r != VK_ERROR_OUT_OF_DATE_KHR) {
        NX_LOGW("vkQueuePresentKHR: %d", (int)r);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Debug report (the guest may call it without checking)
// ---------------------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL h_vkCreateDebugReportCallbackEXT(VkInstance instance, const VkDebugReportCallbackCreateInfoEXT *ci, const VkAllocationCallbacks *alloc, VkDebugReportCallbackEXT *out) {
    PFN_vkCreateDebugReportCallbackEXT fn = (PFN_vkCreateDebugReportCallbackEXT)real_instance("vkCreateDebugReportCallbackEXT");
    if (fn) return fn(instance, ci, alloc, out);
    static uint64_t next;
    *out = (VkDebugReportCallbackEXT)(uintptr_t)(0x1000 + ++next);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL h_vkDestroyDebugReportCallbackEXT(VkInstance instance, VkDebugReportCallbackEXT cb, const VkAllocationCallbacks *alloc) {
    if ((uintptr_t)cb >= 0x1000) return;
    PFN_vkDestroyDebugReportCallbackEXT fn = (PFN_vkDestroyDebugReportCallbackEXT)real_instance("vkDestroyDebugReportCallbackEXT");
    if (fn) fn(instance, cb, alloc);
}
VKAPI_ATTR void VKAPI_CALL h_vkDebugReportMessageEXT(VkInstance instance, VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT type,
                                                     uint64_t object, size_t location, int32_t code, const char *layer, const char *message) {
    PFN_vkDebugReportMessageEXT fn = (PFN_vkDebugReportMessageEXT)real_instance("vkDebugReportMessageEXT");
    if (fn) { fn(instance, flags, type, object, location, code, layer, message); return; }
    NX_LOGD("vulkan debug report: %s: %s", layer ? layer : "-", message ? message : "-");
}

// ---------------------------------------------------------------------------
// Shaders, pipelines, caches
// ---------------------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL h_vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *alloc, VkShaderModule *out) {
    if (!ci || !ci->pCode || ci->codeSize < 20 || (ci->codeSize & 3)) {
        NX_LOGE("vkCreateShaderModule: invalid SPIR-V size %zu", ci ? ci->codeSize : 0);
        return VK_ERROR_INVALID_SHADER_NV;
    }
    if (ci->pCode[0] != 0x07230203u) NX_LOGE("vkCreateShaderModule: bad SPIR-V magic 0x%08x", ci->pCode[0]);
    PFN_vkCreateShaderModule fn = REAL_D(vkCreateShaderModule);
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = fn(d, ci, alloc, out);
    if (r != VK_SUCCESS) NX_LOGE("vkCreateShaderModule: real driver rejected %zu bytes: %d", ci->codeSize, (int)r);
    else if (out) vk_capture_module_write(ci, *out);
    return r;
}
VKAPI_ATTR void VKAPI_CALL h_vkDestroyShaderModule(VkDevice d, VkShaderModule module, const VkAllocationCallbacks *alloc) {
    PFN_vkDestroyShaderModule fn = REAL_D(vkDestroyShaderModule);
    if (!fn) return;
    pthread_mutex_lock(&g_vk_capture_mu);
    uint64_t h = (uint64_t)(uintptr_t)module;
    vk_capture_module_ref **link = &g_vk_capture_modules;
    while (*link) {
        if ((*link)->handle == h) { vk_capture_module_ref *old = *link; *link = old->next; free(old); break; }
        link = &(*link)->next;
    }
    pthread_mutex_unlock(&g_vk_capture_mu);
    fn(d, module, alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL h_vkCreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDescriptorSetLayout *out) {
    PFN_vkCreateDescriptorSetLayout fn = REAL_D(vkCreateDescriptorSetLayout);
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = fn(d, ci, alloc, out);
    if (r == VK_SUCCESS && out) vk_capture_set_layout(ci, *out);
    return r;
}
VKAPI_ATTR void VKAPI_CALL h_vkDestroyDescriptorSetLayout(VkDevice d, VkDescriptorSetLayout layout, const VkAllocationCallbacks *alloc) {
    PFN_vkDestroyDescriptorSetLayout fn = REAL_D(vkDestroyDescriptorSetLayout);
    if (!fn) return;
    pthread_mutex_lock(&g_vk_capture_mu);
    vk_capture_layout_forget(&g_vk_capture_set_layouts, (uint64_t)(uintptr_t)layout);
    pthread_mutex_unlock(&g_vk_capture_mu);
    fn(d, layout, alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL h_vkCreatePipelineLayout(VkDevice d, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *alloc, VkPipelineLayout *out) {
    PFN_vkCreatePipelineLayout fn = REAL_D(vkCreatePipelineLayout);
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = fn(d, ci, alloc, out);
    if (r == VK_SUCCESS && out) vk_capture_pipeline_layout(ci, *out);
    return r;
}
VKAPI_ATTR void VKAPI_CALL h_vkDestroyPipelineLayout(VkDevice d, VkPipelineLayout layout, const VkAllocationCallbacks *alloc) {
    PFN_vkDestroyPipelineLayout fn = REAL_D(vkDestroyPipelineLayout);
    if (!fn) return;
    pthread_mutex_lock(&g_vk_capture_mu);
    vk_capture_layout_forget(&g_vk_capture_pipeline_layouts, (uint64_t)(uintptr_t)layout);
    pthread_mutex_unlock(&g_vk_capture_mu);
    fn(d, layout, alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL h_vkCreateGraphicsPipelines(VkDevice d, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *alloc, VkPipeline *out) {
    PFN_vkCreateGraphicsPipelines fn = REAL_D(vkCreateGraphicsPipelines);
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    if (cache == VK_NULL_HANDLE) cache = g_pipeline_cache;
    if (ci) for (uint32_t i = 0; i < count; i++)
        for (uint32_t k = 0; k < ci[i].stageCount; k++) vk_capture_stage(&ci[i].pStages[k], ci[i].layout);
    VkResult r = fn(d, cache, count, ci, alloc, out);
    if (r != VK_SUCCESS) NX_LOGE("vkCreateGraphicsPipelines: %u pipeline(s) failed: %d", count, (int)r);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL h_vkCreateComputePipelines(VkDevice d, VkPipelineCache cache, uint32_t count, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *alloc, VkPipeline *out) {
    PFN_vkCreateComputePipelines fn = REAL_D(vkCreateComputePipelines);
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    if (cache == VK_NULL_HANDLE) cache = g_pipeline_cache;
    if (ci) for (uint32_t i = 0; i < count; i++) vk_capture_stage(&ci[i].stage, ci[i].layout);
    VkResult r = fn(d, cache, count, ci, alloc, out);
    if (r != VK_SUCCESS) NX_LOGE("vkCreateComputePipelines: %u pipeline(s) failed: %d", count, (int)r);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL h_vkCreatePipelineCache(VkDevice d, const VkPipelineCacheCreateInfo *ci, const VkAllocationCallbacks *alloc, VkPipelineCache *out) {
    (void)d; (void)alloc;
    if (ci && ci->initialDataSize) NX_LOGI("vulkan: ignoring %zu bytes of the guest's binary pipeline cache (NVIDIA cache)", ci->initialDataSize);
    if (!g_pipeline_cache) {
        g_pipeline_cache = pipeline_cache_open();
        if (!g_pipeline_cache) return VK_ERROR_INITIALIZATION_FAILED;
    }
    g_pipeline_cache_refs++;
    *out = g_pipeline_cache;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL h_vkDestroyPipelineCache(VkDevice d, VkPipelineCache c, const VkAllocationCallbacks *alloc) {
    if (c == g_pipeline_cache) {
        if (g_pipeline_cache_refs > 0) g_pipeline_cache_refs--;
        if (g_pipeline_cache_refs == 0) {
            pipeline_cache_save(c);
            PFN_vkDestroyPipelineCache fn = REAL_D(vkDestroyPipelineCache);
            if (fn) fn(d, c, alloc);
            g_pipeline_cache = VK_NULL_HANDLE;
        }
        return;
    }
    PFN_vkDestroyPipelineCache fn = REAL_D(vkDestroyPipelineCache);
    if (fn && c) fn(d, c, alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL h_vkGetPipelineCacheData(VkDevice d, VkPipelineCache c, size_t *size, void *data) {
    PFN_vkGetPipelineCacheData fn = REAL_D(vkGetPipelineCacheData);
    return fn ? fn(d, c, size, data) : VK_ERROR_INITIALIZATION_FAILED;
}
VKAPI_ATTR VkResult VKAPI_CALL h_vkMergePipelineCaches(VkDevice d, VkPipelineCache dst, uint32_t n, const VkPipelineCache *src) {
    PFN_vkMergePipelineCaches fn = REAL_D(vkMergePipelineCaches);
    return fn ? fn(d, dst, n, src) : VK_ERROR_INITIALIZATION_FAILED;
}
