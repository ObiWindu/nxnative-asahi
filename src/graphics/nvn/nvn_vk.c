// NVN replay backend on Vulkan.
//
// The interface layer (dm/gfx/nvn/hle_nvn.c) records NVN commands into the
// game's command memory. This file owns the host-side objects those records
// name and submits the Vulkan work. The first milestone is deliberately the
// core path: pools, textures, samplers, programs, render-target selection,
// clears, buffer copies/fills, events, fences and submission. Draws and
// dispatches are the next block and report their unsupported state instead of
// drawing a wrong picture.
#include "dm.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
#include "nvn_timestamp.h"
#include "nvn_vk_state.h"
#include "nvn_spv_builtin.h"
#include "graphics/vulkan/nx_vk.h"
#include "memory/nx_mem.h"
#include "platform/linux/nx_front.h"
#include "platform/linux/nx_platform.h"
#ifdef NXNATIVE_TEST_SHADERS
#include "spv_smoke.h"
#endif

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

extern int nvn_host_readable(uint64_t address, uint64_t size);
void nvkd_rp_close(void);   // the open render pass, in nvn_vk_draw.inc
uint64_t g_last_window_draw;
extern uint64_t g_hbuf_bytes, g_hbuf_calls, g_hbuf_big;

typedef struct {
    uint64_t lo, hi;
} pool_ent;

typedef struct {
    uint64_t serial;
    tex_t t;
    int live;
    int hnext;                        // next entry in the serial hash bucket (index + 1)
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkFormat fmt;
    VkRenderPass rp;
    VkFramebuffer fb;
    VkImageViewType view_type;
    VkImageAspectFlags aspect;
    uint32_t layers, levels, samples;
    int can_attach;                   // the format supports this image's attachment aspect
    int can_sample;                   // the format supports sampled reads
    VkImageLayout layout;
    int gpu_dirty;                    // last write was ours; otherwise the guest memory is uploaded
    uint64_t wgen, rgen;              // GPU write generation, and the one last ordered for reads
    // The CPU upload is current as of this flush serial: NVN requires the game
    // to flush a range after writing it for the GPU to see it, so a texture is
    // re-uploaded when its storage was flushed since, instead of at every draw
    // (Super Mario Odyssey sampled about 600 textures a frame and uploaded
    // each one again, 45 ms).
    int up_valid;
    uint64_t up_flush;
    size_t up_size;
} tex_ent;

// `layout` is tracked once per NVN texture, so every explicit transition must
// cover the complete Vulkan image. This keeps mip and array subresources in
// the same layout state as the texture-level tracker.
static VkImageSubresourceRange tex_subresources(const tex_ent *e) {
    return (VkImageSubresourceRange){e->aspect, 0, e->levels, 0, e->layers};
}

typedef struct {
    uint64_t serial;
    prog_t p;
    int live;
} prog_ent;

typedef struct {
    int id;
    samp_par p;
    VkSampler vk;
} samp_ent;

typedef struct {
    void (*write)(volatile uint32_t *, uint32_t, uint32_t);
    volatile uint32_t *val;
    uint32_t mode, value;
} post_event;

static NxVk g_vk;
static int g_vk_ready;
// NVN binds programs per stage (BindProgram's mask): a game may bind its
// vertex program and its fragment program separately, as Unity does (The
// Somnium Files, Picross S). Stage 5 is compute, which NVN binds apart from
// the graphics stages. Each slot holds the program that last claimed it.
static const prog_t *g_bound_stage[6];
// NVN state outlives a submission, and the queue frees the snapshots it made
// for one when it completes. The replay keeps its own copies of the objects
// its persistent state points at (render targets and per-stage programs), or
// the next submission would read freed memory.
static tex_t g_rt_color_copy[8], g_rt_depth_copy;
static prog_t g_bound_prog_copy[6];
static uint64_t g_vbuf_addr, g_vbuf_size;
// Recursive: a guest thread registers a texture (g_mu) and looks it up again
// inside the same call; replay lookups take it too, against that registration.
static pthread_mutex_t g_mu;
static pthread_once_t g_mu_once = PTHREAD_ONCE_INIT;
static void g_mu_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_mu, &a);
    pthread_mutexattr_destroy(&a);
}
static void mu_lock(void) { pthread_once(&g_mu_once, g_mu_init); pthread_mutex_lock(&g_mu); }
static void mu_unlock(void) { pthread_mutex_unlock(&g_mu); }

// Registered texture and image ids, looked up on every draw. The guest thread
// registers them while the replay looks them up: the maps take g_mu, and the
// entries are addressed by index so a realloc of their array is safe.
static tex_id_ent *tid_find(int id) {
    mu_lock();
    tex_id_ent *r = NULL;
    if (g_tid_hash)
        for (int i = g_tid_hash[id_hash_bucket(id, g_tid_hash_n)]; i && !r; i = g_tids[i - 1].hnext)
            if (g_tids[i - 1].id == id) r = &g_tids[i - 1];
    mu_unlock();
    return r;
}
static img_id_ent *imgid_find(int id) {
    mu_lock();
    img_id_ent *r = NULL;
    if (g_img_hash)
        for (int i = g_img_hash[id_hash_bucket(id, g_img_hash_n)]; i && !r; i = g_imgids[i - 1].hnext)
            if (g_imgids[i - 1].id == id) r = &g_imgids[i - 1];
    mu_unlock();
    return r;
}

static pool_ent *g_pools; static int g_npools, g_cpools;
static tex_ent *g_texs; static int g_ntex, g_ctex;
// Textures are looked up by serial on every sampled binding, and a game
// initializes (and finalizes) thousands of them: a linear scan over every one
// ever seen cost Super Mario Odyssey 15 ms a frame once it held 20,000
// entries. The hash makes the lookup constant time and the free list reuses a
// finalized texture's entry, whose Vulkan objects are destroyed with the batch
// that may still read them.
static int *g_tex_buckets; static int g_tex_nbuckets;   // index + 1, 0 empty
static int g_tex_free;                                  // first free entry, index + 1
// A finalized texture's Vulkan objects go here from the guest thread, which
// does not hold the replay lock; the next submission drains the list (under
// the lock) so their destruction follows the batch that may still read them.
typedef struct tex_gc { struct tex_gc *next; uint64_t serial; VkImage img; VkDeviceMemory mem; VkImageView view; VkFramebuffer fb; } tex_gc;
static tex_gc *g_tex_gc;
static pthread_mutex_t g_tex_gc_mu = PTHREAD_MUTEX_INITIALIZER;
static prog_ent *g_progs; static int g_nprog;
static samp_ent *g_samps; static int g_nsamp, g_csamp;
static _Atomic uint64_t g_completed;
static post_event g_post[64]; static int g_npost;   // event posts of the batch being recorded
static _Atomic uint64_t g_vk_barriers;
static uint64_t g_last_fence;
static VkCommandBuffer g_cb;
// ---------- Replay submission batching ----------
// NVN submits a command list as one GPU submission. The replay used to end,
// submit and wait every operation (draw, clear, copy, barrier); a Picross S
// scene then took 42 ms a frame, almost all of it driver and fence time
// rather than GPU work. Operations now record into one command buffer, which
// is submitted and waited when the CPU needs the GPU's results (present,
// readback, indirect arguments, storage-buffer copy-back, guest copies) and at
// the end of the NVN submission. Vulkan objects an operation created stay
// alive until that wait, since they are still in use.
typedef struct sub_obj {
    struct sub_obj *next;
    int kind;                 // 0 descriptor pool, 1 framebuffer, 2 image view, 3 buffer view, 4 buffer + memory, 5 upload block, 6 image + memory
    VkDescriptorPool pool; VkFramebuffer fb; VkImageView iv; VkBufferView bv;
    VkBuffer buf; VkDeviceMemory mem; VkImage img;
    void *map; size_t cap;
} sub_obj;
// A submitted batch: its command buffer, its fence, the objects it uses and
// the NVN fence number and event posts it completes with. Batches are waited
// and freed in submission order.
typedef struct sub_batch {
    struct sub_batch *next;
    VkCommandBuffer cb;
    VkFence fence;
    uint64_t fence_no;
    sub_obj *objs;
    post_event *posts;
    int nposts;
} sub_batch;
static sub_batch *g_batches_head, *g_batches_tail;
static uint64_t g_submitted;          // NVN fence number of the last submitted batch
// Commits are logical fence points. Recording continues across them (an NVN
// command list's GPU work is one stream) and a submission covers every commit
// made before it, so a frame's two command lists go to the GPU as one batch:
// each extra submit on this driver blocks until an earlier one completes
// (15.5 ms), which serialized CPU recording and GPU work.
static uint64_t g_logical_fence;
static uint64_t g_sub_t0;             // when the submission being recorded began
// Per-submission replay profiles (NX_VK_PROF=1): where the worker's CPU time
// goes, printed by mtl_sub_commit.
enum { PROF_STAGES, PROF_PIPELINE, PROF_FB, PROF_DESC, PROF_HBUF, PROF_BARRIER, PROF_DRAW, PROF_PRESENT, PROF_TEX, PROF_VIEW, PROF_CMD, PROF_SAMP, PROF_UPLOAD, PROF_GPU, PROF_UPD, PROF_CMD2, PROF_SUBMIT, PROF_FREE, PROF_N };
static int g_sub_batches;
static int g_sub_uploads;
static uint64_t g_prof[PROF_N];
static uint64_t g_op_ns[256]; static int g_op_n[256];   // NX_VK_PROF: per replay op
static int g_sub_draws, g_sub_dispatches;
static sub_obj *g_sub_objs;           // objects of the batch being recorded
// The replay is one stream: the queue worker records a submission, while guest
// threads may read a texture back or present at any time. They share the
// submission's command buffer and its deferred objects, so every replay entry
// point serializes on this lock (the worker holds it for a whole submission).
// Recursive: a present reads its frame back through mtl_read_texels.
static pthread_mutex_t g_replay_mu;
static pthread_once_t g_replay_mu_once = PTHREAD_ONCE_INIT;
static void replay_mu_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_replay_mu, &a);
    pthread_mutexattr_destroy(&a);
}
static void replay_lock(void) { pthread_once(&g_replay_mu_once, replay_mu_init); pthread_mutex_lock(&g_replay_mu); }
static void replay_unlock(void) { pthread_mutex_unlock(&g_replay_mu); }

static sub_obj *sub_obj_new(int kind) {
    sub_obj *o = calloc(1, sizeof *o);
    if (!o) { LOG("[RED] [nvn/vk] out of memory tracking a replay object"); return NULL; }
    o->kind = kind; o->next = g_sub_objs; g_sub_objs = o;
    return o;
}
static void sub_free_pool(VkDescriptorPool p) { sub_obj *o = sub_obj_new(0); if (o) o->pool = p; else vkDestroyDescriptorPool(g_vk.device, p, NULL); }
static void sub_free_fb(VkFramebuffer f) { sub_obj *o = sub_obj_new(1); if (o) o->fb = f; else vkDestroyFramebuffer(g_vk.device, f, NULL); }
static void sub_free_iv(VkImageView v) { sub_obj *o = sub_obj_new(2); if (o) o->iv = v; else vkDestroyImageView(g_vk.device, v, NULL); }
static void sub_free_bv(VkBufferView v) { sub_obj *o = sub_obj_new(3); if (o) o->bv = v; else vkDestroyBufferView(g_vk.device, v, NULL); }
static void sub_free_buffer(VkBuffer b, VkDeviceMemory m) { sub_obj *o = sub_obj_new(4); if (o) { o->buf = b; o->mem = m; } else nx_vk_destroy_buffer(&g_vk, b, m); }
static void sub_free_image(VkImage i, VkDeviceMemory m) { sub_obj *o = sub_obj_new(6); if (o) { o->img = i; o->mem = m; } else nx_vk_destroy_image(&g_vk, i, m); }
// One descriptor pool per batch: a pool for every draw cost more than the
// draw's commands (Picross S spent milliseconds a frame in pool creation).
static VkDescriptorPool g_batch_dpool;
static int g_batch_dpool_sets;
static VkDescriptorPool batch_desc_pool(void) {
    if (g_batch_dpool && g_batch_dpool_sets > 0) { g_batch_dpool_sets--; return g_batch_dpool; }
    VkDescriptorPoolSize ps[8] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2048}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2048},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 2048}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 512}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 512},
    };
    VkDescriptorPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 64; pci.poolSizeCount = 8; pci.pPoolSizes = ps;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(g_vk.device, &pci, NULL, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    sub_free_pool(pool);   // destroyed when the batch completes
    g_batch_dpool = pool; g_batch_dpool_sets = 63;
    return pool;
}

// A pooled upload block goes back to the free list when its batch completes.
void hb_free_push(VkBuffer b, VkDeviceMemory m, void *map, size_t cap);
static void sub_release_upload(VkBuffer b, VkDeviceMemory m, void *map, size_t cap) {
    sub_obj *o = sub_obj_new(5);
    if (o) { o->buf = b; o->mem = m; o->map = map; o->cap = cap; }
    else nx_vk_destroy_buffer(&g_vk, b, m);
}
static void sub_frees_run(sub_obj *objs) {
    while (objs) {
        sub_obj *o = objs; objs = o->next;
        switch (o->kind) {
        case 0: vkDestroyDescriptorPool(g_vk.device, o->pool, NULL); break;
        case 1: vkDestroyFramebuffer(g_vk.device, o->fb, NULL); break;
        case 2: vkDestroyImageView(g_vk.device, o->iv, NULL); break;
        case 3: vkDestroyBufferView(g_vk.device, o->bv, NULL); break;
        case 4: nx_vk_destroy_buffer(&g_vk, o->buf, o->mem); break;
        case 5: hb_free_push(o->buf, o->mem, o->map, o->cap); break;
        case 6: nx_vk_destroy_image(&g_vk, o->img, o->mem); break;
        }
        free(o);
    }
}
// The command buffer the current NVN submission records into, opened on the
// first operation that needs one.
// The view cache (nvn_vk_draw.inc) drops a finalized texture's views.
void nvn_views_drop_serial(uint64_t serial);

// Called with the replay lock held, before a batch is submitted: the texture
// views and images of finalized textures join that batch's free list, so they
// outlive every command that could still read them (the queue is ordered).
static void tex_gc_drain(void) {
    tex_gc *list;
    pthread_mutex_lock(&g_tex_gc_mu);
    list = g_tex_gc; g_tex_gc = NULL;
    pthread_mutex_unlock(&g_tex_gc_mu);
    while (list) {
        tex_gc *g = list; list = g->next;
        nvn_views_drop_serial(g->serial);
        if (g->fb) sub_free_fb(g->fb);
        if (g->view) sub_free_iv(g->view);
        if (g->img) sub_free_image(g->img, g->mem);
        free(g);
    }
}

static int g_sub_ops;   // operations recorded into the current batch
static VkCommandBuffer rp_cb(void) {
    if (!g_cb) { g_cb = nx_vk_oneshot_begin(&g_vk); g_sub_ops = 0; }
    if (g_cb) g_sub_ops++;
    return g_cb;
}
// An operation's last call: the recording continues into the submission batch.
static int rp_done(VkCommandBuffer cb) { (void)cb; return 0; }
// End the open recording and submit it without waiting: the GPU runs it while
// the next submission is recorded. The queue is ordered, so a later submission
// sees its results; only a CPU read, a present or a wait needs completion.
static int sub_submit(void) {
    tex_gc_drain();
    if (!g_cb) return 0;
    uint64_t _ts = nx_monotonic_ns();
    VkCommandBuffer cb = g_cb; g_cb = VK_NULL_HANDLE;
    nvkd_rp_close();   // the batch's last render pass ends before the buffer
    uint64_t _te = nx_monotonic_ns();
    VkResult _er = vkEndCommandBuffer(cb);
    g_prof[PROF_CMD] += nx_monotonic_ns() - _te;
    if (_er != VK_SUCCESS) { vkFreeCommandBuffers(g_vk.device, g_vk.cmd_pool, 1, &cb); sub_frees_run(g_sub_objs); g_sub_objs = NULL; return -1; }
    VkFenceCreateInfo fi = {0};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    sub_batch *b = calloc(1, sizeof *b);
    if (!b || vkCreateFence(g_vk.device, &fi, NULL, &fence) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_vk.device, g_vk.cmd_pool, 1, &cb);
        sub_frees_run(g_sub_objs); g_sub_objs = NULL;
        free(b);
        return -1;
    }
    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    uint64_t _tq = nx_monotonic_ns();
    struct timespec _c0, _c1; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &_c0);
    VkResult r = vkQueueSubmit(g_vk.queue, 1, &si, fence);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &_c1);
    uint64_t _wall = nx_monotonic_ns() - _tq;
    g_prof[PROF_PRESENT] += _wall;   // vkQueueSubmit's own time (see the prof line)
    static _Atomic uint64_t q_wall, q_cpu; q_wall += _wall;
    q_cpu += (uint64_t)(_c1.tv_sec - _c0.tv_sec) * 1000000000ull + (uint64_t)(_c1.tv_nsec - _c0.tv_nsec);
    if (nx_env_bool("NX_VK_PROF", 0)) LOG("[nvn/vk] queue submit: wall %.2f cpu %.2f ms", (double)_wall / 1e6, (double)((uint64_t)(_c1.tv_sec - _c0.tv_sec) * 1000000000ull + (uint64_t)(_c1.tv_nsec - _c0.tv_nsec)) / 1e6);
    if (r != VK_SUCCESS) {
        LOG("[RED] [nvn/vk] vkQueueSubmit failed: %d", (int)r);
        vkDestroyFence(g_vk.device, fence, NULL);
        vkFreeCommandBuffers(g_vk.device, g_vk.cmd_pool, 1, &cb);
        sub_frees_run(g_sub_objs); g_sub_objs = NULL;
        free(b);
        return -1;
    }
    b->cb = cb; b->fence = fence; b->objs = g_sub_objs; g_sub_objs = NULL;
    g_batch_dpool = VK_NULL_HANDLE; g_batch_dpool_sets = 0;
    b->fence_no = g_logical_fence;   // every commit recorded so far completes with it
    g_submitted++;
    g_sub_batches++;
    b->nposts = g_npost;
    if (g_npost) { b->posts = malloc((size_t)g_npost * sizeof *b->posts); if (b->posts) memcpy(b->posts, g_post, (size_t)g_npost * sizeof *b->posts); else b->nposts = 0; }
    g_npost = 0;
    if (g_batches_tail) g_batches_tail->next = b; else g_batches_head = b;
    g_batches_tail = b;
    if (nx_env_bool("NX_VK_DEBUG", 0)) LOG("[nvn/vk] submit: %d ops (fence %llu)", g_sub_ops, (unsigned long long)b->fence_no);
    g_prof[PROF_SUBMIT] += nx_monotonic_ns() - _ts;
    g_sub_ops = 0;
    return 0;
}
// Free a completed batch: its command buffer, fence, objects, and post its
// events (they complete with its fence, as NVN signals them).
static void sub_batch_free(sub_batch *b) {
    uint64_t _tf = nx_monotonic_ns();
    for (int i = 0; i < b->nposts; i++) {
        if (!b->posts[i].val) continue;
        if (b->posts[i].mode == 0) *b->posts[i].val = b->posts[i].value;
        else *b->posts[i].val += b->posts[i].value;
    }
    free(b->posts);
    sub_frees_run(b->objs);
    vkDestroyFence(g_vk.device, b->fence, NULL);
    vkFreeCommandBuffers(g_vk.device, g_vk.cmd_pool, 1, &b->cb);
    free(b);
    g_prof[PROF_FREE] += nx_monotonic_ns() - _tf;
}
// Complete the oldest batch (waiting for its fence).
static void sub_complete_oldest(void) {
    if (!g_batches_head) return;
    sub_batch *b = g_batches_head;
    uint64_t _tg = nx_monotonic_ns();
    vkWaitForFences(g_vk.device, 1, &b->fence, VK_TRUE, UINT64_MAX);
    g_prof[PROF_GPU] += nx_monotonic_ns() - _tg;
    g_batches_head = b->next;
    if (!g_batches_head) g_batches_tail = NULL;
    if (b->fence_no > g_completed) g_completed = b->fence_no;
    sub_batch_free(b);
}
// Complete every batch whose fence has signalled, without waiting.
static void sub_poll(void) {
    while (g_batches_head && vkGetFenceStatus(g_vk.device, g_batches_head->fence) == VK_SUCCESS) sub_complete_oldest();
}
static void sub_wait_all(void) {
    while (g_batches_head) sub_complete_oldest();
}
// Submit what is recorded and wait for the GPU to finish all of it (the CPU is
// about to read its results).
static int sub_flush(void) {
    uint64_t t0 = nx_monotonic_ns();
    int r = sub_submit();
    sub_wait_all();
    if (nx_env_bool("NX_VK_PROF", 0)) {
        LOG("[nvn/vk] mid-frame flush: %d ops, wait done in %.2f ms", g_sub_ops, (double)(nx_monotonic_ns() - t0) / 1e6);
        static int bt; if (bt < 3) { bt++; void dm_backtrace(uint64_t fp); dm_backtrace((uint64_t)__builtin_frame_address(0)); }
    }
    return r;
}

static p_set_rt g_rt;
static int g_draw_warned, g_dispatch_warned;
static int g_rc_skip;
static uint64_t *g_unknown_counter_reports;
static size_t g_unknown_counter_report_count, g_unknown_counter_report_capacity;
static int g_unknown_counter_reports_all;
static int g_counter_warned;
static uint64_t g_present_count;
static _Atomic uint64_t g_stat_draws, g_stat_passes;

static int nvn_counter_report_unknown(uint64_t address) {
    if (g_unknown_counter_reports_all) return 1;
    for (size_t i = 0; i < g_unknown_counter_report_count; i++)
        if (g_unknown_counter_reports[i] == address) return 1;
    return 0;
}

static void nvn_counter_report_mark_unknown(uint64_t address) {
    if (nvn_counter_report_unknown(address)) return;
    if (g_unknown_counter_report_count == g_unknown_counter_report_capacity) {
        size_t cap = g_unknown_counter_report_capacity ? g_unknown_counter_report_capacity * 2 : 64;
        if (cap < g_unknown_counter_report_capacity || cap > SIZE_MAX / sizeof *g_unknown_counter_reports) {
            g_unknown_counter_reports_all = 1;
            return;
        }
        void *p = realloc(g_unknown_counter_reports, cap * sizeof *g_unknown_counter_reports);
        if (!p) { g_unknown_counter_reports_all = 1; return; }
        g_unknown_counter_reports = p;
        g_unknown_counter_report_capacity = cap;
    }
    g_unknown_counter_reports[g_unknown_counter_report_count++] = address;
}

static void nvn_report_counter(const uint8_t *record) {
    uint64_t type = 0, address = 0;
    memcpy(&type, record, sizeof type);
    memcpy(&address, record + sizeof type, sizeof address);
    if (type == NVN_COUNTER_TYPE_TIMESTAMP || type == NVN_COUNTER_TYPE_TIMESTAMP_TOP) {
        if (!address || !nvn_host_readable(address, sizeof(NVNcounterData))) {
            LOG("[RED] [nvn/vk] timestamp counter destination %#llx is not host-readable",
                (unsigned long long)address);
            return;
        }
        // The timestamp is the CPU's guest clock, and a render condition later in
        // this submission must see it: write it now (prior GPU work need not have
        // completed for a time value; the value names when the report was reached).
        NVNcounterData result = {0, nvn_timestamp_from_ns(guest_ns())};
        memcpy((void *)(uintptr_t)address, &result, sizeof result);
        return;
    }
    if (address) nvn_counter_report_mark_unknown(address);
    if (!g_counter_warned) {
        g_counter_warned = 1;
        LOG("[RED] [nvn/vk] non-timestamp counter reports are unsupported; conditional rendering will draw when their result is unknown");
    }
}

static void nvn_render_condition(const p_render_cond *condition) {
    if (condition->mode >= 2) {
        g_rc_skip = condition->mode == 3;
        return;
    }
    uint64_t a = condition->addr, b = condition->addr + 16, x = 0, y = 0;
    int known = a <= UINT64_MAX - 16 && b >= condition->addr &&
        nvn_host_readable(a, sizeof x) && nvn_host_readable(b, sizeof y) &&
        !nvn_counter_report_unknown(a) && !nvn_counter_report_unknown(b);
    if (known) {
        memcpy(&x, (const void *)(uintptr_t)a, sizeof x);
        memcpy(&y, (const void *)(uintptr_t)b, sizeof y);
        g_rc_skip = condition->mode ? x == y : x != y;
    } else {
        // Occlusion and other non-timestamp query results are not generated by
        // this backend. Drawing is conservative and matches the guest's visible
        // output; it only gives up the culling optimization.
        g_rc_skip = 0;
    }
    if (nx_env_bool("NX_VK_DEBUG", 0))
        LOG("[nvn/vk] render condition mode %d at %#llx: %llu / %llu%s%s",
            condition->mode, (unsigned long long)condition->addr,
            (unsigned long long)x, (unsigned long long)y,
            known ? "" : " (unknown, drawn)", g_rc_skip ? " -> not drawn" : "");
}

static void warn_once(int *flag, const char *what) {
    if (!__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED)) LOG("[RED] NVN Vulkan backend: %s is not implemented yet; the command is dropped", what);
}

static const char *pipeline_cache_path(void) {
    static char p[1200];
    const char *e = nx_env("NX_VK_PIPELINE_CACHE");
    if (e && *e) return e;
    snprintf(p, sizeof p, "%s/nxnative/pipeline-cache.bin", nx_cache_dir());
    return p;
}
static void vk_ready(void) {
    if (g_vk_ready) return;
    NxVkConfig cfg = {0};
    cfg.app_name = "nxnative-asahi NVN";
    cfg.app_version = 1;
    cfg.allow_cpu = nx_env_bool("NX_VK_ALLOW_CPU", 0);
    cfg.device_filter = nx_env("NX_VK_DEVICE");
    cfg.window = nx_front_window();
    cfg.enable_swapchain = cfg.window != NULL;
    if (nx_vk_init(&g_vk, &cfg) != 0) die("NVN Vulkan backend: cannot create a Vulkan device");
    nx_vk_pipeline_cache_load(&g_vk, pipeline_cache_path());
    g_vk_ready = 1;
    // g_st is static zero-initialized. Do not clear it here: the guest may have
    // selected its depth mode before the first program forced Vulkan creation.
    g_st.chmask.mask = 0xffffffffu;
    g_st.color.logic_op = NVN_LOGIC_OP_COPY;
    g_st.poly.front = NVN_FRONT_FACE_CCW;
    g_st.ds.df = NVN_DEPTH_FUNC_LEQUAL;
    g_st.dr[0] = 0.0f; g_st.dr[1] = 1.0f;
    LOG("[nvn/vk] device %s, API %u.%u.%u", g_vk.caps.device_name, VK_API_VERSION_MAJOR(g_vk.caps.api_version),
        VK_API_VERSION_MINOR(g_vk.caps.api_version), VK_API_VERSION_PATCH(g_vk.caps.api_version));
}

static tex_ent *tex_find(uint64_t serial);
static tex_ent *tex_ensure(const tex_t *t);
static void nvkd_prefetch(const prog_t *p);
static void tex_dump_now(const tex_t *t, uint32_t index);
int nvn_dpool_base(const void *pool, int tex);
void nvkd_rp_close(void);   // the open render pass, in nvn_vk_draw.inc

#ifdef NXNATIVE_TEST_SHADERS
static VkPipeline g_test_pipe;
static VkPipelineLayout g_test_pl;
static VkRenderPass g_test_rp;
static VkShaderModule g_test_vs, g_test_fs;
static VkFormat g_test_fmt;

static int test_pipeline(VkFormat fmt, VkRenderPass *rp_out) {
    if (g_test_pipe && g_test_fmt == fmt) { *rp_out = g_test_rp; return 1; }
    if (!g_test_vs) g_test_vs = nx_vk_shader_module(&g_vk, k_vs_spv, sizeof k_vs_spv);
    if (!g_test_fs) g_test_fs = nx_vk_shader_module(&g_vk, k_fs_spv, sizeof k_fs_spv);
    if (!g_test_vs || !g_test_fs) return 0;
    VkAttachmentDescription at = {0};
    at.format = fmt; at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_GENERAL; at.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkSubpassDependency dep[2] = {{0}};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT; dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    VkRenderPassCreateInfo rci = {0};
    rci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rci.attachmentCount = 1; rci.pAttachments = &at;
    rci.subpassCount = 1; rci.pSubpasses = &sub;
    rci.dependencyCount = 2; rci.pDependencies = dep;
    if (vkCreateRenderPass(g_vk.device, &rci, NULL, &g_test_rp) != VK_SUCCESS) return 0;
    VkPipelineShaderStageCreateInfo st[2] = {{0}};
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g_test_vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g_test_fs; st[1].pName = "main";
    VkVertexInputBindingDescription vb = {0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &va;
    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = {0, 0, 64, 64, 0, 1};
    VkRect2D sc = {{0, 0}, {64, 64}};
    VkPipelineViewportStateCreateInfo vps = {0};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {0}; cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs = {0};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    VkPipelineLayoutCreateInfo plci = {0};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (vkCreatePipelineLayout(g_vk.device, &plci, NULL, &g_test_pl) != VK_SUCCESS) return 0;
    VkGraphicsPipelineCreateInfo gp = {0};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vps;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cbs;
    gp.layout = g_test_pl; gp.renderPass = g_test_rp; gp.subpass = 0;
    if (vkCreateGraphicsPipelines(g_vk.device, VK_NULL_HANDLE, 1, &gp, NULL, &g_test_pipe) != VK_SUCCESS) return 0;
    g_test_fmt = fmt; *rp_out = g_test_rp;
    return 1;
}

static void copy_image_to_guest(const tex_t *t, tex_ent *e, VkCommandBuffer cb) {
    uint32_t w = t->d.width ? t->d.width : 1, h = t->d.height ? t->d.height : 1;
    VkImageMemoryBarrier b = {0};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = e->layout; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = e->img; b.subresourceRange = tex_subresources(e);
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    size_t row = (size_t)w * 4, bytes = row * h;
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory bmem = VK_NULL_HANDLE; void *map = NULL;
    if (bytes && nx_vk_create_buffer(&g_vk, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buf, &bmem, &map) == 0) {
        VkBufferImageCopy r = {0}; r.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.imageExtent = (VkExtent3D){w, h, 1};
        vkCmdCopyImageToBuffer(cb, e->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
        VkBufferMemoryBarrier bb = {0};
        bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = buf; bb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bb, 0, NULL);
        VkImageMemoryBarrier back = b;
        back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; back.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; back.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &back);
        nx_vk_oneshot_end(&g_vk, cb);
        if (map && t->mem) {
            size_t dst_row = t->L.layer_size ? (size_t)(t->L.layer_size / (t->d.height ? t->d.height : 1)) : row;
            if (dst_row < row) row = dst_row;
            for (uint32_t y = 0; y < h; y++) memcpy(t->mem + (size_t)y * dst_row, (uint8_t *)map + (size_t)y * (size_t)w * 4, row);
        }
        vkUnmapMemory(g_vk.device, bmem); nx_vk_destroy_buffer(&g_vk, buf, bmem);
    } else nx_vk_oneshot_end(&g_vk, cb);
    e->layout = VK_IMAGE_LAYOUT_GENERAL;
}

static void render_triangle(const tex_t *t) {
    sub_flush();   // the test path waits for its own readback: prior clears land first
    tex_ent *e = tex_ensure(t);
    if (!e) return;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (!test_pipeline(e->fmt, &rp)) return;
    if (!e->fb) {
        VkFramebufferCreateInfo fci = {0};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = rp; fci.attachmentCount = 1; fci.pAttachments = &e->view;
        fci.width = t->d.width ? t->d.width : 1; fci.height = t->d.height ? t->d.height : 1; fci.layers = 1;
        vkCreateFramebuffer(g_vk.device, &fci, NULL, &e->fb);
    }
    VkBuffer vbuf = VK_NULL_HANDLE; VkDeviceMemory vmem = VK_NULL_HANDLE; void *vmap = NULL;
    if (!g_vbuf_addr || g_vbuf_size < 24 || nx_vk_create_buffer(&g_vk, g_vbuf_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vbuf, &vmem, &vmap) != 0) return;
    memcpy(vmap, (void *)(uintptr_t)g_vbuf_addr, g_vbuf_size);
    VkCommandBuffer cb = nx_vk_oneshot_begin(&g_vk);
    if (!cb) { nx_vk_destroy_buffer(&g_vk, vbuf, vmem); return; }
    VkClearValue clr = {0}; clr.color = (VkClearColorValue){{0, 0, 1, 1}};
    VkRenderPassBeginInfo rbi = {0};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = rp; rbi.framebuffer = e->fb; rbi.renderArea = (VkRect2D){{0, 0}, {t->d.width, t->d.height}}; rbi.clearValueCount = 1; rbi.pClearValues = &clr;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_test_pipe);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &off);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    copy_image_to_guest(t, e, cb);
    e->layout = VK_IMAGE_LAYOUT_GENERAL; e->gpu_dirty = 1; e->wgen++;
    nx_vk_destroy_buffer(&g_vk, vbuf, vmem);
}
#endif


static VkFormat vkfmt(int f);   // the full NVN format map lives with the draw/replay block

// A format with no alpha channel reads alpha as 1, as NVN's X channel and the
// Metal host's no-alpha formats do. Vulkan substitutes the RGBA format of the
// same size, so a sampled view maps A to ONE (an attachment keeps the identity
// view: a render target writes the guest's spare channel as it is).
static int nvn_fmt_no_alpha(int f) {
    switch (f) {
    case NVN_FORMAT_RGBX8: case NVN_FORMAT_RGBX8SN: case NVN_FORMAT_RGBX8UI: case NVN_FORMAT_RGBX8I:
    case NVN_FORMAT_RGBX8_SRGB:
    case NVN_FORMAT_RGBX16F: case NVN_FORMAT_RGBX16: case NVN_FORMAT_RGBX16SN:
    case NVN_FORMAT_RGBX16UI: case NVN_FORMAT_RGBX16I:
    case NVN_FORMAT_RGBX32F: case NVN_FORMAT_RGBX32UI: case NVN_FORMAT_RGBX32I:
    case NVN_FORMAT_RGB8: case NVN_FORMAT_RGB8SN: case NVN_FORMAT_RGB8UI: case NVN_FORMAT_RGB8I:
    case NVN_FORMAT_RGB16F: case NVN_FORMAT_RGB16: case NVN_FORMAT_RGB16SN:
    case NVN_FORMAT_RGB16UI: case NVN_FORMAT_RGB16I:
    case NVN_FORMAT_RGB32F: case NVN_FORMAT_RGB32UI: case NVN_FORMAT_RGB32I:
    case NVN_FORMAT_RGB5: case NVN_FORMAT_BGR5:
    case NVN_FORMAT_BGRX8: case NVN_FORMAT_BGRX8_SRGB:
    case NVN_FORMAT_RGB_DXT1: case NVN_FORMAT_RGB_DXT1_SRGB:
        return 1;
    default:
        return 0;
    }
}

static tex_ent *tex_ensure_impl(const tex_t *t);
static tex_ent *tex_ensure(const tex_t *t) {
    uint64_t t0 = nx_monotonic_ns();
    tex_ent *r = tex_ensure_impl(t);
    g_prof[PROF_TEX] += nx_monotonic_ns() - t0;
    return r;
}
static tex_ent *tex_ensure_impl(const tex_t *t) {
    if (!t || !t->serial) return NULL;
    mu_lock();
    tex_ent *e = tex_find(t->serial);
    if (!e) { mtl_tex_init(t); e = tex_find(t->serial); }   // g_mu is recursive
    mu_unlock();
    if (!e) return NULL;
    if (e->img) return e;
    e->fmt = vkfmt(t->d.format);
    if (e->fmt == VK_FORMAT_UNDEFINED) {
        LOG("[RED] [nvn/vk] texture format %d is not mapped (serial %llu target %d %ux%ux%u flags %#x pool %p off %#llx)",
            t->d.format, (unsigned long long)t->serial, t->d.target, t->d.width, t->d.height, t->d.depth,
            t->d.flags, (void *)t->pool, (unsigned long long)t->off);
        static int traced;
        if (!traced++) dm_backtrace((uint64_t)__builtin_frame_address(0));
        return NULL;
    }
    uint32_t layers = 1;
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    if (t->d.target == NVN_TEXTURE_TARGET_2D_ARRAY) {
        layers = t->d.depth ? t->d.depth : 1;
        view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    } else if (t->d.target == NVN_TEXTURE_TARGET_CUBEMAP) {
        layers = t->d.depth ? t->d.depth : 6;
        view_type = VK_IMAGE_VIEW_TYPE_CUBE;
    } else if (t->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) {
        layers = (t->d.depth ? t->d.depth : 1) * 6;
        view_type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    } else if (t->d.target == NVN_TEXTURE_TARGET_3D) {
        layers = 1;
        view_type = VK_IMAGE_VIEW_TYPE_3D;
    } else if (t->d.depth > 1) {
        static int warned;
        if (!warned) { warned = 1; LOG("[RED] [nvn/vk] unsupported texture target %d depth %u", t->d.target, t->d.depth); }
        return NULL;
    }
    uint32_t samples = t->d.samples ? t->d.samples : 1;
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8 && samples != 16) {
        LOG("[RED] [nvn/vk] unsupported sample count %u", samples); return NULL;
    }
    uint32_t w = t->d.width ? t->d.width : 1, h = t->d.height ? t->d.height : 1;
    uint32_t levels = t->d.levels ? t->d.levels : 1;
    int depth_fmt = t->d.format == NVN_FORMAT_DEPTH16 || t->d.format == NVN_FORMAT_DEPTH24 ||
                    t->d.format == NVN_FORMAT_DEPTH32F || t->d.format == NVN_FORMAT_DEPTH24_STENCIL8 ||
                    t->d.format == NVN_FORMAT_DEPTH32F_STENCIL8 || t->d.format == NVN_FORMAT_STENCIL8;
    // The image takes exactly the uses its format has. Asking for a use the
    // format lacks (a colour attachment on a block-compressed or sampled-only
    // format, a storage image on a packed one) makes image creation invalid,
    // and a format with no sampled read cannot be replayed at all.
    VkFormatProperties fp = {0};
    vkGetPhysicalDeviceFormatProperties(g_vk.physical, e->fmt, &fp);
    uint32_t f = fp.optimalTilingFeatures;
    uint32_t need = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                    (depth_fmt ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    if ((f & need) != need) {
        static int warned[8]; static int nwarned;
        int known = 0;
        for (int i = 0; i < nwarned; i++) if (warned[i] == t->d.format) known = 1;
        if (!known && nwarned < (int)(sizeof warned / sizeof *warned)) {
            warned[nwarned++] = t->d.format;
            LOG("[RED] [nvn/vk] texture format %d (Vulkan format %d) lacks the transfer and %s support this replay needs on this device; its textures are dropped",
                t->d.format, (int)e->fmt, depth_fmt ? "depth/stencil attachment" : "sampled read");
        }
        return NULL;
    }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    int can_attach = 0;
    if (depth_fmt) {
        if (f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; can_attach = 1; }
    } else if (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) { usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; can_attach = 1; }
    if (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    VkExtent3D extent = {w, h, is3d && t->d.depth ? t->d.depth : 1};
    // NVN views reinterpret a texture's format (an sRGB twin of a linear
    // texture, R32UI over the same storage, depth as R32F). Vulkan only allows
    // a view whose format differs from the image's with MUTABLE_FORMAT, and a
    // view registered after the image exists cannot add it then.
    VkImageCreateFlags image_flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (is3d && g_vk.caps.api_version >= VK_API_VERSION_1_1 && g_vk.caps.image_2d_view_of_3d)
        image_flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    if (nx_vk_create_image(&g_vk, is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, e->fmt, extent, levels, layers, samples, VK_IMAGE_TILING_OPTIMAL, image_flags,
                           usage, VK_IMAGE_LAYOUT_UNDEFINED, &e->img, &e->mem) != 0) return NULL;
    VkImageAspectFlags aspect = depth_fmt ? ((t->d.format == NVN_FORMAT_DEPTH24_STENCIL8 || t->d.format == NVN_FORMAT_DEPTH32F_STENCIL8 || t->d.format == NVN_FORMAT_STENCIL8) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    if (t->d.format == NVN_FORMAT_STENCIL8) aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
    VkImageViewCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = e->img; vi.viewType = view_type; vi.format = e->fmt;
    vi.subresourceRange = (VkImageSubresourceRange){aspect, 0, levels, 0, layers};
    if (vkCreateImageView(g_vk.device, &vi, NULL, &e->view) != VK_SUCCESS) { nx_vk_destroy_image(&g_vk, e->img, e->mem); e->img = VK_NULL_HANDLE; e->mem = VK_NULL_HANDLE; return NULL; }
    e->view_type = view_type; e->aspect = aspect; e->layers = layers; e->levels = levels; e->samples = samples;
    e->can_attach = can_attach;
    e->can_sample = (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    e->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    e->gpu_dirty = 0;
    return e;
}

static void clear_color_record(const p_clear_color *c);

static void pool_find(uint64_t addr, uint64_t len, pool_ent **out) {
    mu_lock();
    *out = NULL;
    for (int i = 0; i < g_npools; i++) if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) { *out = &g_pools[i]; break; }
    mu_unlock();
}

static int tex_bucket(uint64_t serial, int nbuckets) {
    serial ^= serial >> 33; serial *= 0xff51afd7ed558ccdull;
    serial ^= serial >> 33; serial *= 0xc4ceb9fe1a85ec53ull;
    serial ^= serial >> 33;
    return (int)(serial & (uint64_t)(nbuckets - 1));
}
static void tex_hash_rebuild(int nbuckets) {
    int *b = calloc((size_t)nbuckets, sizeof *b);
    if (!b) return;
    free(g_tex_buckets);
    g_tex_buckets = b; g_tex_nbuckets = nbuckets;
    for (int i = 0; i < g_ntex; i++) if (g_texs[i].live) {
        int h = tex_bucket(g_texs[i].serial, nbuckets);
        g_texs[i].hnext = b[h]; b[h] = i + 1;
    }
}
static tex_ent *tex_find_locked(uint64_t serial) {
    if (!g_tex_buckets || !serial) return NULL;
    for (int i = g_tex_buckets[tex_bucket(serial, g_tex_nbuckets)]; i; i = g_texs[i - 1].hnext)
        if (g_texs[i - 1].serial == serial && g_texs[i - 1].live) return &g_texs[i - 1];
    return NULL;
}
static tex_ent *tex_find(uint64_t serial) {
    mu_lock();
    tex_ent *e = tex_find_locked(serial);
    mu_unlock();
    return e;
}
// Grow before the new entry is marked live: a rebuild walks the live entries,
// and inserting a live entry into a table that already holds it links it to
// itself (the chain never ends).
static void tex_hash_ensure(void) {
    if (!g_tex_buckets) tex_hash_rebuild(64);
    else if (g_ntex > g_tex_nbuckets * 2) tex_hash_rebuild(g_tex_nbuckets * 2);
}
static void tex_hash_insert(tex_ent *e) {
    if (!g_tex_buckets) return;
    int h = tex_bucket(e->serial, g_tex_nbuckets);
    e->hnext = g_tex_buckets[h];
    g_tex_buckets[h] = (int)(e - g_texs) + 1;
}
static void tex_hash_remove(tex_ent *e) {
    if (!g_tex_buckets) return;
    int *pp = &g_tex_buckets[tex_bucket(e->serial, g_tex_nbuckets)];
    while (*pp && &g_texs[*pp - 1] != e) pp = &g_texs[*pp - 1].hnext;
    if (*pp) *pp = e->hnext;
}
void nvn_views_drop_serial(uint64_t serial);   // the view cache, in nvn_vk_draw.inc
static void tex_gc_drain(void);
static prog_ent *prog_find(uint64_t serial) {
    mu_lock();
    prog_ent *r = NULL;
    for (int i = 0; i < g_nprog; i++) if (g_progs[i].serial == serial && g_progs[i].live) { r = &g_progs[i]; break; }
    mu_unlock();
    return r;
}

void mtl_init(void) { vk_ready(); }
void mtl_prepare(void) { vk_ready(); }

void mtl_pool_add(const void *mem, uint64_t size) {
    if (!mem || !size) return;
    mu_lock();
    pool_ent e = {(uint64_t)(uintptr_t)mem, (uint64_t)(uintptr_t)mem + size};
    if (g_npools == g_cpools) { g_cpools = g_cpools ? g_cpools * 2 : 16; g_pools = realloc(g_pools, (size_t)g_cpools * sizeof *g_pools); }
    g_pools[g_npools++] = e;
    mu_unlock();
}
void mtl_pool_addressed(const void *mem, uint64_t size) { mtl_pool_add(mem, size); }
void mtl_pool_del(const void *mem) {
    mu_lock();
    for (int i = 0; i < g_npools; i++) if (g_pools[i].lo == (uint64_t)(uintptr_t)mem) { g_pools[i] = g_pools[--g_npools]; break; }
    mu_unlock();
}
void mtl_pool_replace(const void *const *del, int ndel, const void *const *add, const uint64_t *size, int nadd) {
    for (int i = 0; i < ndel; i++) mtl_pool_del(del[i]);
    for (int i = 0; i < nadd; i++) mtl_pool_add(add[i], size[i]);
}
void mtl_alias_drop_pool(const void *pool) { (void)pool; }
void mtl_sp_phys_gone(const void *mem, uint64_t size) { (void)mem; (void)size; }
void dm_gfx_addr_note(uint64_t a, char *note, size_t n) {
    pool_ent *p = NULL; pool_find(a, 1, &p);
    if (note && n) snprintf(note, n, "%s%#llx", p ? "in a memory pool at " : "not in a pool", (unsigned long long)(p ? a - p->lo : a));
}
int mtl_addr_known(uint64_t addr, uint64_t len) { pool_ent *p; pool_find(addr, len, &p); return p != NULL; }
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    pool_ent *p; pool_find(addr, len, &p);
    if (!p) return 0;
    if (buf) *buf = (void *)(uintptr_t)addr;
    if (off) *off = 0;
    return 1;
}
int mtl_gpu_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off, uint64_t *gpu) {
    if (!mtl_addr(addr, len, buf, off)) return 0;
    if (gpu) *gpu = addr;
    return 1;
}

void mtl_tex_init(const tex_t *t) {
    if (!t || !t->serial) return;
    mu_lock();
    tex_ent *e = tex_find(t->serial);
    if (!e) {
        if (g_tex_free) {
            int i = g_tex_free - 1; e = &g_texs[i]; g_tex_free = e->hnext;
            memset(e, 0, sizeof *e);
        } else {
            if (g_ntex == g_ctex) { g_ctex = g_ctex ? g_ctex * 2 : 64; g_texs = realloc(g_texs, (size_t)g_ctex * sizeof *g_texs); }
            e = &g_texs[g_ntex++]; memset(e, 0, sizeof *e);
        }
        e->serial = t->serial;
        tex_hash_ensure();
        e->live = 1;
        tex_hash_insert(e);
    } else {
        e->t = *t; e->live = 1;
    }
    e->t = *t; e->live = 1;
    mu_unlock();
}
void mtl_tex_release(tex_t *t) {
    if (!t) return;
    replay_lock();   // the replay may be reading this entry right now
    mu_lock();
    tex_ent *e = tex_find(t->serial);
    if (e) {
        tex_gc *g = calloc(1, sizeof *g);
        e->live = 0;
        tex_hash_remove(e);
        if (g) {
            g->serial = e->serial; g->img = e->img; g->mem = e->mem; g->view = e->view; g->fb = e->fb;
            pthread_mutex_lock(&g_tex_gc_mu);
            g->next = g_tex_gc; g_tex_gc = g;
            pthread_mutex_unlock(&g_tex_gc_mu);
        } else if (e->img) nx_vk_destroy_image(&g_vk, e->img, e->mem);
        e->fb = VK_NULL_HANDLE; e->view = VK_NULL_HANDLE; e->img = VK_NULL_HANDLE; e->mem = VK_NULL_HANDLE;
        e->rp = VK_NULL_HANDLE; e->layout = VK_IMAGE_LAYOUT_UNDEFINED; e->gpu_dirty = 0;
        e->serial = 0;
        e->hnext = g_tex_free; g_tex_free = (int)(e - g_texs) + 1;
    }
    mu_unlock();
    replay_unlock();
}
void mtl_tex_pin(uint64_t serial) { (void)serial; }
void mtl_tex_unpin(uint64_t serial) { (void)serial; }
static void tex_dirty_set(const tex_t *t, int gpu) {
    if (!t) return;
    mu_lock();
    tex_ent *e = tex_find(t->serial);
    if (e) e->gpu_dirty = gpu;
    mu_unlock();
}
void mtl_tex_cpu_written(const tex_t *t) { tex_dirty_set(t, 0); }
void mtl_tex_linear_cpu_written(const tex_t *t) { tex_dirty_set(t, 0); }
void mtl_tex_gpu_written(const tex_t *t) { tex_dirty_set(t, 1); }
int mtl_tex_gpu_owned(uint64_t serial) { (void)serial; return 0; }
int mtl_tex_cpu_rewritten(const tex_t *t) { (void)t; return 0; }

void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (!t) return;
    mu_lock();
    tex_id_ent *e = tid_find(id);
    if (!e) {
        // A fixed, generous table: reallocating it would move entries the
        // replay holds pointers to (a title's first frame registers thousands
        // of textures while the replay resolves them).
        if (!g_tids) { g_ctid = 65536; g_tids = calloc((size_t)g_ctid, sizeof *g_tids); }
        if (g_ntid == g_ctid) {
            static int warned;
            if (!warned++) LOG("[RED] [nvn/vk] more than %d registered texture ids; the table no longer grows", g_ctid);
            mu_unlock(); return;
        }
        tid_hash_ensure();
        e = &g_tids[g_ntid]; memset(e, 0, sizeof *e); e->id = id;
        tid_hash_put(g_ntid); g_ntid++;
    }
    e->t = *t; e->gone = 0;
    if (v) { e->v = *v; e->has_view = 1; }
    mu_unlock();
    mtl_tex_init(t);
}
void mtl_reg_tex_gone(int id) {
    mu_lock();
    tex_id_ent *e = tid_find(id); if (e) e->gone = 1;
    mu_unlock();
}
void mtl_reg_image(int id, const tex_t *t, const view_t *v) {
    if (!t) return;
    mu_lock();
    img_id_ent *e = imgid_find(id);
    if (!e) {
        if (!g_imgids) { g_cimg = 4096; g_imgids = calloc((size_t)g_cimg, sizeof *g_imgids); }
        if (g_nimg == g_cimg) {
            static int warned;
            if (!warned++) LOG("[RED] [nvn/vk] more than %d registered image ids; the table no longer grows", g_cimg);
            mu_unlock(); return;
        }
        img_hash_ensure();
        e = &g_imgids[g_nimg]; memset(e, 0, sizeof *e); e->id = id;
        img_hash_put(g_nimg); g_nimg++;
    }
    e->t = *t; e->gone = 0;
    if (v) { e->v = *v; e->has_view = 1; }
    mu_unlock();
    mtl_tex_init(t);
}
void mtl_reg_samp(int id, const samp_par *p) {
    mu_lock();
    if (g_nsamp == g_csamp) { g_csamp = g_csamp ? g_csamp * 2 : 64; g_samps = realloc(g_samps, (size_t)g_csamp * sizeof *g_samps); }
    g_samps[g_nsamp++] = (samp_ent){id, *p, VK_NULL_HANDLE};
    mu_unlock();
}
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_set_samp_pool_n(int n) { (void)n; }

// Read one mip (a region of it, one or more slices) back into the guest's
// linear buffer in the texture's own format: a block-compressed format returns
// its blocks and a multisampled colour target is resolved first. Region offsets
// and sizes are in texels; the destination holds one block row per `row` bytes.
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    static int logged;
    if (nx_env_bool("NX_VK_PROF", 0) && logged < 6) { logged++; LOG("[nvn/vk] readback serial %llx %ux%u", (unsigned long long)(t ? t->serial : 0), t ? t->d.width : 0, t ? t->d.height : 0); void dm_backtrace(uint64_t fp); dm_backtrace((uint64_t)__builtin_frame_address(0)); }
    replay_lock();
    sub_flush();   // the CPU reads what the GPU has written
    if (!t || !dst) { replay_unlock(); return; }
    tex_ent *e = tex_ensure(t);
    if (!e) { replay_unlock(); return; }
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    uint32_t lw = t->d.width >> level, lh = t->d.height >> level;
    uint32_t lz = is3d ? (t->d.depth >> level) : 1;
    if (!lw) lw = 1;
    if (!lh) lh = 1;
    if (!lz) lz = 1;
    if (level >= e->levels || (!is3d && layer0 >= e->layers)) {
        warn_once(&g_draw_warned, "texture readback of an unsupported subresource");
        { replay_unlock(); return; }
    }
    // A region's z offset and depth select slices of a 3D texture or layers of
    // an array, as NVN's CPU read does.
    uint32_t z0 = r && r->zoffset > 0 ? (uint32_t)r->zoffset : 0;
    uint32_t nz = r && r->depth > 0 ? (uint32_t)r->depth : 1;
    if (is3d) {
        if (z0 >= lz) { replay_unlock(); return; }
        if (nz > lz - z0) nz = lz - z0;
    } else {
        uint32_t first = layer0 + z0;
        if (first >= e->layers) { replay_unlock(); return; }
        if (nz > e->layers - first) nz = e->layers - first;
    }
    int bw = 0, bh = 0;
    int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (bpb <= 0 || bw <= 0 || bh <= 0) {
        warn_once(&g_draw_warned, "texture readback of a format without a block layout");
        { replay_unlock(); return; }
    }
    uint32_t blocks_x = (lw + (uint32_t)bw - 1) / (uint32_t)bw;
    uint32_t blocks_y = (lh + (uint32_t)bh - 1) / (uint32_t)bh;
    if (blocks_x > SIZE_MAX / (size_t)bpb) { replay_unlock(); return; }
    size_t src_row = (size_t)blocks_x * (size_t)bpb;
    if (blocks_y > SIZE_MAX / src_row) { replay_unlock(); return; }
    size_t slice_bytes = src_row * blocks_y;
    if (slice_bytes > SIZE_MAX / nz) { replay_unlock(); return; }
    // Each slice's buffer offset must be a multiple of 4 and of the texel block.
    size_t slice_stride = slice_bytes;
    size_t align = bpb >= 4 ? (size_t)bpb : 4;
    if (slice_stride % align) slice_stride += align - slice_stride % align;
    if (slice_stride > SIZE_MAX / nz) { replay_unlock(); return; }
    size_t bytes = slice_stride * nz;
    // The requested rectangle, in texels, clamped to this level.
    uint32_t x0 = r && r->xoffset > 0 ? (uint32_t)r->xoffset : 0;
    uint32_t y0 = r && r->yoffset > 0 ? (uint32_t)r->yoffset : 0;
    uint32_t cw = r && r->width > 0 ? (uint32_t)r->width : lw;
    uint32_t ch = r && r->height > 0 ? (uint32_t)r->height : lh;
    if (x0 > lw) x0 = lw;
    if (y0 > lh) y0 = lh;
    if (cw > lw - x0) cw = lw - x0;
    if (ch > lh - y0) ch = lh - y0;
    if (!cw || !ch) { replay_unlock(); return; }
    size_t xb = (size_t)(x0 / (uint32_t)bw) * (size_t)bpb;
    uint32_t yb = y0 / (uint32_t)bh;
    size_t wb = (size_t)((cw + (uint32_t)bw - 1) / (uint32_t)bw) * (size_t)bpb;
    uint32_t hb = (ch + (uint32_t)bh - 1) / (uint32_t)bh;

    // A multisampled colour target has no guest-readable samples: resolve it
    // into a single-sample image of the same format first. Depth and stencil
    // samples have no core resolve command, so those keep the red line.
    VkImage rimg = VK_NULL_HANDLE;
    VkDeviceMemory rmem = VK_NULL_HANDLE;
    int resolve = e->samples > 1;
    if (resolve) {
        if (e->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
            warn_once(&g_draw_warned, "readback of a multisampled depth or stencil texture");
            { replay_unlock(); return; }
        }
        VkFormatProperties fp = {0};
        vkGetPhysicalDeviceFormatProperties(g_vk.physical, e->fmt, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
            warn_once(&g_draw_warned, "readback of a multisampled texture of a format without resolve support");
            { replay_unlock(); return; }
        }
        if (nx_vk_create_image(&g_vk, VK_IMAGE_TYPE_2D, e->fmt, (VkExtent3D){lw, lh, 1}, 1, nz, 1,
                               VK_IMAGE_TILING_OPTIMAL, 0,
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_LAYOUT_UNDEFINED, &rimg, &rmem) != 0) {
            warn_once(&g_draw_warned, "readback of a multisampled texture could not allocate its resolve target");
            { replay_unlock(); return; }
        }
    }
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void *map = NULL;
    if (nx_vk_create_buffer(&g_vk, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buf, &mem, &map) != 0) {
        if (rimg) nx_vk_destroy_image(&g_vk, rimg, rmem);
        { replay_unlock(); return; }
    }
    VkCommandBuffer cb = nx_vk_oneshot_begin(&g_vk);
    if (!cb) {
        nx_vk_destroy_buffer(&g_vk, buf, mem);
        if (rimg) nx_vk_destroy_image(&g_vk, rimg, rmem);
        { replay_unlock(); return; }
    }
    VkImageSubresourceRange whole = tex_subresources(e);
    VkImageMemoryBarrier b = {0};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = e->layout; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = e->img; b.subresourceRange = whole;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    for (uint32_t z = 0; z < nz; z++) {
        VkImage copy_img = e->img;
        VkImageLayout copy_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkImageSubresourceLayers sub = {e->aspect, level, is3d ? 0 : layer0 + z0 + z, 1};
        int32_t oz = is3d ? (int32_t)(z0 + z) : 0;
        if (resolve) {
            if (z == 0) {
                VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, nz};
                VkImageMemoryBarrier tb = {0};
                tb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                tb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; tb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                tb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; tb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                tb.image = rimg; tb.subresourceRange = rr;
                tb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &tb);
                VkImageResolve res = {0};
                res.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, level, layer0 + z0, nz};
                res.srcOffset = (VkOffset3D){0, 0, 0};
                res.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, nz};
                res.dstOffset = (VkOffset3D){0, 0, 0};
                res.extent = (VkExtent3D){lw, lh, 1};
                vkCmdResolveImage(cb, e->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rimg,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &res);
                VkImageMemoryBarrier rb = tb;
                rb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; rb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                rb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; rb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &rb);
            }
            copy_img = rimg;
            sub = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, z, 1};
            oz = 0;
        }
        VkBufferImageCopy cp = {0};
        cp.bufferOffset = (VkDeviceSize)z * slice_stride;
        cp.imageSubresource = sub;
        cp.imageOffset = (VkOffset3D){0, 0, oz};
        cp.imageExtent = (VkExtent3D){lw, lh, 1};
        vkCmdCopyImageToBuffer(cb, copy_img, copy_layout, buf, 1, &cp);
    }
    VkBufferMemoryBarrier bb = {0};
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = buf; bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bb, 0, NULL);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    nx_vk_oneshot_end(&g_vk, cb);
    e->layout = VK_IMAGE_LAYOUT_GENERAL;
    if (map) {
        for (uint32_t z = 0; z < nz; z++) {
            const uint8_t *zsrc = (const uint8_t *)map + (size_t)z * slice_stride;
            uint8_t *zdst = dst + (size_t)z * (img ? img : row * hb);
            size_t n = wb;
            if (n > row) n = row;
            for (uint32_t y = 0; y < hb; y++)
                memcpy(zdst + (size_t)y * row, zsrc + (size_t)(yb + y) * src_row + xb, n);
        }
    }
    nx_vk_destroy_buffer(&g_vk, buf, mem);
    if (rimg) nx_vk_destroy_image(&g_vk, rimg, rmem);
    replay_unlock();
}

void mtl_prog_release(prog_t *p) {
    if (!p) return;
    mu_lock();
    prog_ent *e = prog_find(p->serial); if (e) e->live = 0;
    mu_unlock();
}
void mtl_prog_unpin(prog_t *p) { (void)p; }
void mtl_prog_snapshot(prog_t *copy, const prog_t *source) { if (copy && source) *copy = *source; }
void mtl_prog_prefetch(const prog_t *p) { nvkd_prefetch(p); }
void mtl_prog_forget(const prog_t *p) {
    if (!p) return;
    mu_lock();
    prog_ent *e = prog_find(p->serial); if (e) e->live = 0;
    mu_unlock();
}

void mtl_sub_begin(void) {
    replay_lock();
    vk_ready();
    // Recording continues into the open batch: consecutive NVN command lists
    // are one GPU stream until something needs the GPU's results (present,
    // readback, a wait) or the batch grows too large.
    if (g_sub_ops > 4000) sub_submit();
    g_sub_t0 = nx_monotonic_ns();
    g_npost = 0;
    // Render-target and pipeline state are NVN command-buffer state and persist
    // across submissions; only a recorded SetRenderTargets changes g_rt.
}
void mtl_sub_ready(uint64_t guest_ns) { (void)guest_ns; }
void mtl_queue_activate(uint64_t serial) { (void)serial; }
void mtl_queue_forget(uint64_t serial) { (void)serial; }
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    static int logged;
    if (nx_env_bool("NX_VK_PROF", 0) && logged < 40) { logged++; LOG("[nvn/vk] sub copy %#llx -> %#llx (%llu bytes)", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n); }
    sub_flush(); if (src && dst && n) memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n);
}
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) {
    static int logged;
    if (nx_env_bool("NX_VK_PROF", 0) && logged < 40) { logged++; LOG("[nvn/vk] sub fill %#llx (%llu bytes) = %#x", (unsigned long long)dst, (unsigned long long)n, value); }
    sub_flush(); if (dst && n) memset((void *)(uintptr_t)dst, value, n);
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) {
    if (g_npost < (int)(sizeof g_post / sizeof *g_post)) { g_post[g_npost].val = val; g_post[g_npost].mode = mode; g_post[g_npost].value = v; g_npost++; }
}
void mtl_sub_counter(uint64_t addr, uint32_t type) { (void)addr; (void)type; }
uint64_t mtl_sub_commit(void) {
    int ops = g_sub_ops;
    // An event the command list signals must reach the GPU before the guest
    // waits for it: a guest thread can wait on one without a present or a
    // fence wait in between (Super Mario Odyssey's loader did, and the waited
    // event never fired because the batch was still open).
    if (g_npost > 0) sub_submit();
    // The logical commit point; the work joins the next submission, which
    // covers every commit made before it.
    uint64_t f = ++g_logical_fence;
    if (nx_env_bool("NX_VK_PROF", 0) && g_sub_t0)
        LOG("[nvn/vk] submission: %d ops, recorded in %.2f ms", ops, (double)(nx_monotonic_ns() - g_sub_t0) / 1e6);
    if (nx_env_bool("NX_VK_PROF", 0) && g_sub_t0) {
        LOG("[nvn/vk] prof: draws %d dispatch %d batches %d | gpu wait %.2f | stages %.2f pipeline %.2f fb %.2f desc %.2f hbuf %.2f tex %.2f view %.2f barriers %.2f cmd %.2f ms",
            g_sub_draws, g_sub_dispatches, g_sub_batches, g_prof[PROF_GPU] / 1e6,
            g_prof[PROF_STAGES] / 1e6, g_prof[PROF_PIPELINE] / 1e6, g_prof[PROF_FB] / 1e6, g_prof[PROF_DESC] / 1e6,
            g_prof[PROF_HBUF] / 1e6, g_prof[PROF_TEX] / 1e6, g_prof[PROF_VIEW] / 1e6,
            g_prof[PROF_BARRIER] / 1e6, g_prof[PROF_CMD] / 1e6);
        LOG("[nvn/vk] prof: sampler %.2f upload %.2f update %.2f tail %.2f submit %.2f free %.2f ms (%d uploads), textures %d samplers %d", g_prof[PROF_SAMP] / 1e6, g_prof[PROF_UPLOAD] / 1e6,
            g_prof[PROF_UPD] / 1e6, g_prof[PROF_CMD2] / 1e6, g_prof[PROF_SUBMIT] / 1e6, g_prof[PROF_FREE] / 1e6, g_sub_uploads, g_ntex, g_nsamp);
        {
            int best[6]; for (int k = 0; k < 6; k++) best[k] = -1;
            for (int op = 0; op < 256; op++) if (g_op_n[op]) {
                for (int k = 0; k < 6; k++) if (best[k] < 0 || g_op_ns[op] > g_op_ns[best[k]]) {
                    for (int m = 5; m > k; m--) best[m] = best[m - 1];
                    best[k] = op; break;
                }
            }
            char line[256]; size_t used = 0; uint64_t total = 0; int calls = 0;
            for (int op = 0; op < 256; op++) { total += g_op_ns[op]; calls += g_op_n[op]; }
            for (int k = 0; k < 6 && best[k] >= 0; k++) {
                int o = best[k];
                used += (size_t)snprintf(line + used, sizeof line - used, " op%d %dx %.2f", o, g_op_n[o], g_op_ns[o] / 1e6);
            }
            LOG("[nvn/vk] prof: op time %.2f ms in %d calls:%s | end %.2f queue %.2f", total / 1e6, calls, line, g_prof[PROF_CMD] / 1e6, g_prof[PROF_PRESENT] / 1e6);
            memset(g_op_n, 0, sizeof g_op_n); memset(g_op_ns, 0, sizeof g_op_ns);
        }
        LOG("[nvn/vk] prof: staging %llu calls, %llu KB, %llu large", (unsigned long long)g_hbuf_calls, (unsigned long long)(g_hbuf_bytes / 1024), (unsigned long long)g_hbuf_big);
        g_hbuf_calls = g_hbuf_bytes = g_hbuf_big = 0;
        g_sub_uploads = 0;
        g_sub_draws = g_sub_dispatches = g_sub_batches = 0;
        memset(g_prof, 0, sizeof g_prof);
    }
    g_last_fence = f;
    replay_unlock();
    return f;
}
uint64_t mtl_completed_fence(void) {
    replay_lock();
    sub_poll();
    uint64_t c = g_completed;
    replay_unlock();
    return c;
}
uint64_t mtl_last_fence(void) { return g_last_fence; }

int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    uint64_t _tw = nx_monotonic_ns();
    replay_lock();
    sub_submit(); (void)timeout_ns;
    while (g_batches_head && g_completed < fence) sub_complete_oldest();
    if (fence > g_completed) { nx_vk_wait_idle(&g_vk); sub_wait_all(); }
    int done = fence <= g_completed;
    replay_unlock();
    if (nx_env_bool("NX_VK_PROF", 0)) LOG("[nvn/vk] wait fence %llu: %.2f ms%s", (unsigned long long)fence, (double)(nx_monotonic_ns() - _tw) / 1e6, done ? "" : " (not done)");
    return done;
}
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { return mtl_wait(fence, timeout_ns); }
void mtl_finish(void) {
    if (!g_vk_ready) return;
    replay_lock();
    sub_flush();
    nx_vk_wait_idle(&g_vk);
    static uint64_t last_save;
    uint64_t now = nx_monotonic_ns();
    if (!last_save || now - last_save > 10000000000ull) {   // at most once per 10 s
        nx_vk_pipeline_cache_save(&g_vk, pipeline_cache_path());
        last_save = now;
    }
    g_completed = g_last_fence;
    replay_unlock();
}
void mtl_drain(void) { mtl_finish(); }
void mtl_arch_save(int wait_ms, int ending) { (void)wait_ms; (void)ending; }
void mtl_warm_stop(void) {}

// Copy the guest's texture storage into its VkImage. Linear textures are taken
// row by row; block-linear ones are deswizzled on the CPU first (the GPU
// deswizzler is a later optimisation; correctness comes first).
static int upload_guest_texture_impl(const tex_t *t, tex_ent *e);
static int upload_guest_texture(const tex_t *t, tex_ent *e) {
    uint64_t _tu = nx_monotonic_ns();
    g_sub_uploads++;
    int _r = upload_guest_texture_impl(t, e);
    g_prof[PROF_UPLOAD] += nx_monotonic_ns() - _tu;
    return _r;
}
uint64_t nvn_flush_stamp(const void *p, uint64_t n);   // hle_nvn.c: the newest flush over a range
// Whether the GPU has written [addr, addr + len): a render target or storage
// image with its dirty flag, or a storage buffer bound for writing this batch.
// A CPU read of such a range must wait for the GPU; one of plain guest data
// need not (indirect arguments and multi-draw lists are built by the CPU).
int nvn_range_gpu_written(uint64_t addr, uint64_t len) {
    if (!addr || !len) return 1;
    int hit = 0;
    mu_lock();
    for (int i = 0; i < g_ntex && !hit; i++) {
        tex_ent *e = &g_texs[i];
        if (!e->live || !e->gpu_dirty || !e->t.mem) continue;
        uint64_t lo = (uint64_t)(uintptr_t)e->t.mem, hi = lo + e->t.L.size;
        hit = addr < hi && lo < addr + len;
    }
    mu_unlock();
    for (int st = 0; st < 6 && !hit; st++)
        for (int sl = 0; sl < 16; sl++) {
            uint64_t lo = g_st.ssbo[st][sl].addr, n = g_st.ssbo[st][sl].size;
            if (lo && n && addr < lo + n && lo < addr + len) { hit = 1; break; }
        }
    return hit;
}

// Whether the guest storage was flushed since the image's last upload. A
// texture the GPU wrote (a render target) holds the GPU's content and is not
// uploaded.
static int tex_stale(const tex_ent *e, const tex_t *t) {
    if (e->gpu_dirty) return 0;
    if (!e->up_valid || !t->mem || e->up_size != t->L.size) return 1;
    return nvn_flush_stamp(t->mem, t->L.size) != e->up_flush;
}

static int upload_guest_texture_impl(const tex_t *t, tex_ent *e) {
    if (!t->mem) return -1;
    // The flush serial is taken before the storage is read: a flush after this
    // point is newer than the stamp and uploads the content again next time.
    uint64_t up_flush = nvn_flush_stamp(t->mem, t->L.size);
    e->up_valid = 0;
    if (e->samples > 1) { LOG("[RED] [nvn/vk] cannot upload CPU data into a %u-sample texture", e->samples); return -1; }
    uint32_t w = t->d.width ? t->d.width : 1, h = t->d.height ? t->d.height : 1;
    int bw = 0, bh = 0;
    int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (bpb <= 0 || bw <= 0 || bh <= 0) {
        LOG("[RED] [nvn/vk] texture format %d has no block layout", t->d.format);
        return -1;
    }
    uint32_t levels = t->d.levels ? t->d.levels : 1;
    uint32_t layers = e->layers ? e->layers : 1;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    uint32_t depth = is3d && t->d.depth ? t->d.depth : 1;
    // Stage every level and layer tightly: block rows for block-compressed formats, pixel
    // rows otherwise. The staging buffer holds all levels one after another.
    size_t total = 0;
    VkBufferImageCopy copies[256]; memset(copies, 0, sizeof copies);
    if ((size_t)levels * layers > 256) return -1;
    uint32_t nc = 0;
    for (uint32_t l = 0; l < levels; l++) for (uint32_t layer = 0; layer < layers; layer++) {
        uint32_t lw = w >> l, lh = h >> l, ld = depth >> l;
        if (!lw) lw = 1;
        if (!lh) lh = 1;
        if (!ld) ld = 1;
        uint32_t bx = (lw + (uint32_t)bw - 1) / (uint32_t)bw;
        uint32_t by = (lh + (uint32_t)bh - 1) / (uint32_t)bh;
        size_t row = (size_t)bx * (size_t)bpb;
        copies[nc].bufferOffset = total;
        copies[nc].imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1};
        copies[nc].imageExtent = (VkExtent3D){lw, lh, ld};
        total += row * by * ld;
        nc++;
    }
    if (!total) return -1;
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void *map = NULL;
    if (nx_vk_create_buffer(&g_vk, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buf, &mem, &map) != 0) return -1;
    uint8_t *tmp = NULL;
    nc = 0;
    for (uint32_t l = 0; l < levels; l++) for (uint32_t layer = 0; layer < layers; layer++) {
        uint32_t lw = w >> l, lh = h >> l, ld = depth >> l;
        if (!lw) lw = 1;
        if (!lh) lh = 1;
        if (!ld) ld = 1;
        uint32_t bx = (lw + (uint32_t)bw - 1) / (uint32_t)bw;
        uint32_t by = (lh + (uint32_t)bh - 1) / (uint32_t)bh;
        size_t row = (size_t)bx * (size_t)bpb;
        uint8_t *dst = (uint8_t *)map + copies[nc].bufferOffset;
        if (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) {
            size_t pitch = l ? row : (t->L.layer_size ? (size_t)(t->L.layer_size / by) : row);
            if (pitch < row) pitch = row;
            const uint8_t *src = t->mem + (size_t)layer * t->L.layer_size + nvn_tex_level_offset(&t->d, l);
            for (uint32_t y = 0; y < by * ld; y++) memcpy(dst + (size_t)y * row, src + (size_t)y * pitch, row);
        } else {
            size_t n = row * by * ld;
            tmp = malloc(n);
            if (!tmp) { nx_vk_destroy_buffer(&g_vk, buf, mem); return -1; }
            if (nvn_deswizzle(&t->d, l, layer, t->mem, tmp, row) != 0) {
                free(tmp); nx_vk_destroy_buffer(&g_vk, buf, mem);
                LOG("[RED] [nvn/vk] cannot deswizzle format %d level %u", t->d.format, l);
                return -1;
            }
            memcpy(dst, tmp, n);
            free(tmp); tmp = NULL;
        }
        nc++;
    }
    // Inside a submission the upload is recorded after the commands already in
    // the batch (whose layout transitions it must follow); on its own it is a
    // submission of its own that the caller waits for (readback, present).
    int batched = g_cb != VK_NULL_HANDLE;
    if (batched) nvkd_rp_close();   // a transfer cannot record inside a render pass
    VkCommandBuffer cb = batched ? rp_cb() : nx_vk_oneshot_begin(&g_vk);
    if (!cb) { vkUnmapMemory(g_vk.device, mem); nx_vk_destroy_buffer(&g_vk, buf, mem); return -1; }
    VkImageMemoryBarrier b = {0};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = e->layout; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = e->img;
    b.subresourceRange = tex_subresources(e);
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkCmdCopyBufferToImage(cb, buf, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nc, copies);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    if (batched) {
        vkUnmapMemory(g_vk.device, mem);
        sub_free_buffer(buf, mem);   // the batch still reads it
    } else {
        nx_vk_oneshot_end(&g_vk, cb);
        vkUnmapMemory(g_vk.device, mem);
        nx_vk_destroy_buffer(&g_vk, buf, mem);
    }
    e->layout = VK_IMAGE_LAYOUT_GENERAL;
    e->up_valid = 1; e->up_flush = up_flush; e->up_size = (size_t)t->L.size;
    return 0;
}

#include "nvn_vk_draw.inc"

static int g_present_warned;
// NX_VK_PRESENT_DUMP=<dir>[:<n>]: write the first n presented frames (default
// 3) as PPM files, for headless bring-up where no window surface exists. The
// image is read through mtl_read_texels in the texture's own format; only
// 8-bit RGBA/BGRA targets are written, and a lower-left window origin flips
// the rows so the file reads like the screen.
static void tex_write_ppm(const tex_t *t, uint32_t index, const char *dir, const char *prefix) {
    if (t->d.target != NVN_TEXTURE_TARGET_2D || t->d.levels > 1) return;
    int bw = 0, bh = 0, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint32_t w = t->d.width, h = t->d.height;
    int packed10 = t->d.format == NVN_FORMAT_RGB10A2 || t->d.format == NVN_FORMAT_RGB10A2SN;
    int r11g11b10 = t->d.format == NVN_FORMAT_R11G11B10F;
    if ((bpb != 4 && !packed10 && !r11g11b10) || bw != 1 || bh != 1 || !w || !h) {
        static int warned;
        if (!warned++) LOG("[nvn/vk] present dump: format %#x is not an 8-bit colour format; frames are not written", t->d.format);
        return;
    }
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return;
    NVNcopyRegion all = {0, 0, 0, (int)w, (int)h, 1};
    mtl_read_texels(t, 0, 0, &all, px, (size_t)w * 4, (size_t)w * h * 4);
    char path[600];
    snprintf(path, sizeof path, "%s/%s_%08x.ppm", dir, prefix, index);
    FILE *f = fopen(path, "wb");
    if (!f) { free(px); return; }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    int bgra = t->d.format == NVN_FORMAT_BGRA8 || t->d.format == NVN_FORMAT_BGRA8_SRGB;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t)(g_origin_ll ? h - 1 - y : y) * w * 4;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = row + (size_t)x * 4;
            uint8_t rgb[3];
            if (packed10) {
                uint32_t v; memcpy(&v, p, 4);
                rgb[0] = (uint8_t)((v & 0x3ff) >> 2);
                rgb[1] = (uint8_t)(((v >> 10) & 0x3ff) >> 2);
                rgb[2] = (uint8_t)(((v >> 20) & 0x3ff) >> 2);
            } else if (r11g11b10) {
                uint32_t v; memcpy(&v, p, 4);
                float f[3];
                for (int c = 0; c < 3; c++) {
                    uint32_t bits = c == 0 ? (v & 0x7ff) : c == 1 ? ((v >> 11) & 0x7ff) : ((v >> 22) & 0x3ff);
                    int e = (int)(bits >> (c == 2 ? 5 : 6));
                    uint32_t m = bits & ((1u << (c == 2 ? 5 : 6)) - 1);
                    if (e == 0) f[c] = (float)m / (float)(1u << (c == 2 ? 5 : 6)) * powf(2.0f, -14.0f);
                    else if (e == (c == 2 ? 31 : 31)) f[c] = 65504.0f;
                    else f[c] = (1.0f + (float)m / (float)(1u << (c == 2 ? 5 : 6))) * powf(2.0f, (float)e - 15.0f);
                }
                for (int c = 0; c < 3; c++) {
                    float x = f[c] / (1.0f + f[c]);   // tonemap for the dump
                    x = powf(x < 0 ? 0 : x > 1 ? 1 : x, 1.0f / 2.2f);
                    rgb[c] = (uint8_t)(x * 255.0f + 0.5f);
                }
            } else {
                rgb[0] = bgra ? p[2] : p[0]; rgb[1] = p[1]; rgb[2] = bgra ? p[0] : p[2];
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    free(px);
}

static void present_dump(const tex_t *t, uint32_t index) {
    static int on = -1, limit = 3, every = 1;
    static char dir[512];
    if (on < 0) {
        const char *e = getenv("NX_VK_PRESENT_DUMP");
        on = e && *e != 0;
        if (on) {
            snprintf(dir, sizeof dir, "%s", e);
            char *c = strchr(dir, ':');
            if (c) {
                *c = 0;
                char *e = strchr(c + 1, ':');
                limit = atoi(c + 1);
                if (e) every = atoi(e + 1);
            }
        }
    }
    if (!on || !limit || every < 1 || (int)index > limit || index % (uint32_t)every) return;
    tex_write_ppm(t, index, dir, "frame");
}

// A dump of one texture by its serial, asked for from the draw block: the
// presented frame's intermediate targets are transient and finalized before
// present, so they are read while the pass that drew them is still recorded.
static void tex_dump_now(const tex_t *t, uint32_t index) {
    const char *e = nx_env("NX_VK_TEXDUMP");
    if (!e || !*e) return;
    const char *dir = strchr(e, ':');   // <size>:<dir>
    if (!dir || !dir[1]) return;
    tex_write_ppm(t, index, dir + 1, "tex");
}

static uint64_t mtl_present_locked(const tex_t *t, double period_s, const int crop[4], uint64_t *copy) {
    (void)period_s;
    if (copy) *copy = 0;
    g_present_count++;
    if (!t) return g_present_count;
    tex_ent *e = tex_ensure(t);
    if (!e) return g_present_count;
    uint64_t p0 = nx_monotonic_ns();
    if (nx_env("NX_VK_DRAWAT")) LOG("[nvn/vk] present %llu of serial %llx %ux%u (last window draw %llx)", (unsigned long long)g_present_count,
                                    (unsigned long long)t->serial, t->d.width, t->d.height, (unsigned long long)g_last_window_draw);
    // Submit what is recorded and go on: the present and the next frame's
    // recording follow it on the same queue, so the CPU does not wait for the
    // GPU here (NVN presents asynchronously; waiting serialized every frame on
    // the GPU and cost a frame's worth of overlap).
    sub_submit();
    if (nx_env_bool("NX_VK_GPU_TIME", 0)) {
        uint64_t g0 = nx_monotonic_ns();
        sub_wait_all();
        LOG("[nvn/vk] frame GPU time %.2f ms", (double)(nx_monotonic_ns() - g0) / 1e6);
    }

    uint64_t p1 = nx_monotonic_ns();
    if (tex_stale(e, t) && upload_guest_texture(t, e) != 0) return g_present_count;
    if (nx_env_bool("NX_VK_DEBUG", 0))
        LOG("[nvn/vk] present: serial %llu %ux%u fmt %d level %u layer %u", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, t->d.levels, t->d.target);
    present_dump(t, g_present_count);
    uint64_t p2 = nx_monotonic_ns();
    int r = nx_vk_present_image(&g_vk, e->img, e->layout, t->d.width ? t->d.width : 1, t->d.height ? t->d.height : 1, crop, g_origin_ll);
    if (nx_env_bool("NX_VK_PROF", 0))
        LOG("[nvn/vk] present: gpu wait %.2f ms, present %.2f ms", (double)(p1 - p0) / 1e6, (double)(nx_monotonic_ns() - p2) / 1e6);
    if (r == 1) {
        if (!g_present_warned) { g_present_warned = 1; LOG("[nvn/vk] present: no window surface; frames are rendered headless"); }
        return g_present_count;
    }
    if (r != 0) LOG("[RED] [nvn/vk] present failed (Vulkan result %d)", r);
    return g_present_count;
}
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4], uint64_t *copy) {
    replay_lock();
    uint64_t r = mtl_present_locked(t, period_s, crop, copy);
    replay_unlock();
    return r;
}
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
void mtl_present_wait_texture(uint64_t ticket, uint64_t copy) { (void)ticket; (void)copy; }
void mtl_present_flush(void) {}
void mtl_present_test_stall(uint64_t ns) { (void)ns; }
int mtl_display_hz(void) {
    if (!g_vk_ready || g_vk.surface == VK_NULL_HANDLE) return 0;
    return 60;   // the SDL3 window's refresh is not exposed by SDL3; FIFO presents pace at the real rate
}
int mtl_has_window(void) { return g_vk.surface != VK_NULL_HANDLE; }

void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec(const uint8_t *p) {
    if (!p) return;
    int prof = nx_env_bool("NX_VK_PROF", 0);
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        uint64_t t0 = prof ? nx_monotonic_ns() : 0;
        switch (h.op) {
        case OP_END: return;
        case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
        case OP_CALL: { uint64_t t; memcpy(&t, a, 8); if (g_npost >= 0) { mtl_exec((const uint8_t *)(uintptr_t)t); } break; }
        case OP_SIGNAL_EVENT: { volatile uint32_t *v; uint32_t m, x; memcpy(&v, a, 8); memcpy(&m, a + 8, 4); memcpy(&x, a + 12, 4); mtl_sub_event(v, m, x); break; }
        case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s_, d, n); break; }
        case OP_CLEAR_BUF: { uint64_t d; uint32_t v; memcpy(&d, a, 8); memcpy(&v, a + 8, 4); mtl_sub_fill(d, *(uint64_t *)(a + 16), v); break; }
        case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); if (d && n) memcpy((void *)(uintptr_t)d, a + 16, n); break; }
        case OP_SET_RT: {
            p_set_rt rt; memcpy(&rt, a, sizeof rt);
            for (int i = 0; i < rt.n && i < 8; i++) if (rt.color[i]) {
                memcpy(&g_rt_color_copy[i], rt.color[i], sizeof g_rt_color_copy[i]);
                rt.color[i] = &g_rt_color_copy[i];
            }
            if (rt.depth) { memcpy(&g_rt_depth_copy, rt.depth, sizeof g_rt_depth_copy); rt.depth = &g_rt_depth_copy; }
            g_rt = rt;
            break;
        }
        case OP_REPORT_COUNTER: nvn_report_counter(a); break;
        case OP_SET_TEX_POOL: g_st.tex_base = nvn_dpool_base(((const p_set_pool *)a)->pool, 1); break;
        case OP_SET_SAMP_POOL: g_st.samp_base = nvn_dpool_base(((const p_set_pool *)a)->pool, 0); break;
        case OP_BIND_PROGRAM: {
            const p_bind_program *b = (const p_bind_program *)a;
            uint32_t mask = (uint32_t)b->stages;
            for (int st = 0; st < 6; st++) if (mask & (1u << st)) {
                if (!b->prog) { g_bound_stage[st] = NULL; continue; }
                memcpy(&g_bound_prog_copy[st], b->prog, sizeof g_bound_prog_copy[st]);
                g_bound_stage[st] = &g_bound_prog_copy[st];
            }
            break;
        }
        case OP_BIND_VATTR: { const p_bind_vattr *b = (const p_bind_vattr *)a; if (b->n > 0 && b->first >= 0 && b->first + b->n <= 32) { memcpy(&g_st.attr[b->first], b->st, (size_t)b->n * sizeof b->st[0]); g_st.nattr = b->first + b->n; } break; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = (const p_bind_vstream *)a; if (b->n > 0 && b->first >= 0 && b->first + b->n <= 16) { memcpy(&g_st.stream[b->first], b->st, (size_t)b->n * sizeof b->st[0]); g_st.nstream = b->first + b->n; } break; }
        case OP_BIND_VBUF: { p_bind_vbuf tmp; const p_bind_vbuf *b = vbuf_rec(h.op, a, &tmp); if (b->n > 0 && b->first >= 0 && b->first + b->n <= 16) { memcpy(&g_st.vbuf[b->first], b->r, (size_t)b->n * sizeof b->r[0]); g_vbuf_addr = b->r[0].address; g_vbuf_size = b->r[0].size; } break; }
        case OP_BIND_VBUF1: { p_bind_vbuf tmp; const p_bind_vbuf *b = vbuf_rec(h.op, a, &tmp); if (b->n > 0 && b->first >= 0 && b->first + b->n <= 16) { memcpy(&g_st.vbuf[b->first], b->r, (size_t)b->n * sizeof b->r[0]); g_vbuf_addr = b->r[0].address; g_vbuf_size = b->r[0].size; } break; }
        case OP_BIND_UBO: case OP_BIND_UBO_SMALL: { p_bind_buf tmp; const p_bind_buf *b = bind_buf_rec(h.op, a, &tmp); if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) { g_st.ubo[b->stage][b->index] = *b; g_st.ubo_set[b->stage] |= (uint16_t)(1u << b->index); } break; }
        case OP_BIND_SSBO: { const p_bind_buf *b = (const p_bind_buf *)a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) g_st.ssbo[b->stage][b->index] = *b; break; }
        case OP_BIND_TEX: case OP_BIND_TEX1: { p_bind_handles tmp; const p_bind_handles *b = bind_rec(h.op, a, &tmp); if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->n > 0 && b->first + b->n <= 256) memcpy(&g_st.tex[b->stage][b->first], b->h, (size_t)b->n * 8); break; }
        case OP_BIND_SAMP: { const p_bind_handles *b = (const p_bind_handles *)a; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->n > 0 && b->first + b->n <= 128) memcpy(&g_st.samp[b->stage][b->first], b->h, (size_t)b->n * 8); break; }
        case OP_BIND_IMG: { const p_bind_handles *b = (const p_bind_handles *)a; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->n > 0 && b->first + b->n <= 32) memcpy(&g_st.img[b->stage][b->first], b->h, (size_t)b->n * 8); break; }
        case OP_VIEWPORTS: { const p_viewports *b = (const p_viewports *)a; for (int i = 0; i < b->n && b->first + i < 16; i++) memcpy(g_st.vp[b->first + i], b->v[i], 16); g_st.has_vp = b->n > 0; break; }
        case OP_VP_SWIZZLE: {
            const p_vp_swizzle *b = (const p_vp_swizzle *)a;
            if (b->first < 0 || b->n < 0 || b->first > 16 || b->n > 16 - b->first) {
                LOG("[RED] [nvn/vk] viewport swizzle range %d + %d is out of bounds", b->first, b->n); break;
            }
            memcpy(&g_st.vp_sw[b->first], b->sw, 4 * (size_t)b->n);
            break;
        }
        case OP_LINE_WIDTH: g_st.line_width = ((const p_line_width *)a)->w; break;
        case OP_SCISSOR: { const p_scissor *b = (const p_scissor *)a; g_st.sc_x = b->x; g_st.sc_y = b->y; g_st.sc_w = b->w; g_st.sc_h = b->h; g_st.has_scissor = 1; break; }
        case OP_DEPTH_RANGE: { const p_depth_range *b = (const p_depth_range *)a; g_st.dr[0] = b->n; g_st.dr[1] = b->f; break; }
        case OP_DEPTH_CLAMP: g_st.depth_clamp = ((const p_depth_clamp *)a)->on; break;
        case OP_PATCH_SIZE: g_st.patch_size = ((const p_patch_size *)a)->n; break;
        case OP_RAST_DISCARD: g_st.rast_discard = ((const p_rast_discard *)a)->on; break;
        case OP_STENCIL: {
            const p_stencil *b = (const p_stencil *)a;
            for (int f = 0; f < 2; f++) if (b->face & (1 << f)) {
                if (b->which == 0) g_st.st_vmask[f] = (uint32_t)b->value_mask;
                else if (b->which == 1) g_st.st_wmask[f] = (uint32_t)b->mask;
                else g_st.st_ref[f] = (uint32_t)b->ref;
            }
            break;
        }
        case OP_PRIM_RESTART: g_st.restart = *(const p_prim_restart *)a; break;
        case OP_POLY_OFFSET: { const p_poly_offset *b = (const p_poly_offset *)a; g_st.po = *b; g_st.has_po = 1; break; }
        case OP_BLEND_COLOR: memcpy(g_st.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
        case OP_ALPHA_REF: g_st.alpha_ref = ((const p_alpha_ref *)a)->ref; break;
        case OP_PUSH_CONST: { const p_push_const *b = (const p_push_const *)a; if (b->size <= DMSH_PUSH_SIZE * DMSH_PUSH_BLOCKS && b->offset + b->size <= DMSH_PUSH_SIZE * DMSH_PUSH_BLOCKS) memcpy(g_st.push[b->compute ? 1 : 0] + b->offset, b->data, b->size); break; }
        case OP_BIND_STATE: {
            const p_bind_state *b = (const p_bind_state *)a;
            switch (b->kind) {
            case 0: if (h.size - 8 >= 4 + sizeof(blend_st)) memcpy(&g_st.blend, b->bytes, sizeof g_st.blend); break;
            case 1: if (h.size - 8 >= 4 + sizeof(color_st)) memcpy(&g_st.color, b->bytes, sizeof g_st.color); break;
            case 2: if (h.size - 8 >= 4 + sizeof(chmask_st)) memcpy(&g_st.chmask, b->bytes, sizeof g_st.chmask); break;
            case 3: if (h.size - 8 >= 4 + sizeof(ms_st)) memcpy(&g_st.ms, b->bytes, sizeof g_st.ms); break;
            case 4: if (h.size - 8 >= 4 + sizeof(poly_st)) memcpy(&g_st.poly, b->bytes, sizeof g_st.poly); break;
            case 5: if (h.size - 8 >= 4 + sizeof(ds_st)) memcpy(&g_st.ds, b->bytes, sizeof g_st.ds); break;
            }
            break;
        }
        case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_submitted + 1; s->fenced = 1; break; }
        case OP_CLEAR_COLOR: {
            const p_clear_color *c = (const p_clear_color *)a;
            if (!g_rc_skip && c->index >= 0 && c->index < g_rt.n && g_rt.color[c->index]) {
                clear_color_record(c);
            }
            break;
        }
        case OP_COPY_B2T: copy_b2t((const p_copy_b2t *)a, h.size); break;
        case OP_COPY_T2B: copy_t2b((const p_copy_b2t *)a, h.size); break;
        case OP_COPY_T2T: copy_t2t((const p_copy_t2t *)a); break;
        case OP_CLEAR_TEX: clear_tex_record((const p_clear_tex *)a); break;
        case OP_CLEAR_DS: {
            if (g_rc_skip) break;
            const p_clear_ds *c = (const p_clear_ds *)a;
            if (!g_rt.depth) break;
            tex_ent *e = tex_ensure(g_rt.depth);
            if (!e) break;
            VkImageAspectFlags clear_aspects = 0;
            if (c->depth_mask && (e->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
                clear_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
            uint32_t stencil_mask = (uint32_t)c->stencil_mask & 0xff;
            if (stencil_mask && !(e->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
                static int warned_depth_only_stencil_clear;
                if (!warned_depth_only_stencil_clear++)
                    LOG("[nvn/vk] NVN stencil clear on a depth-only attachment has no stencil plane; applying any requested depth clear");
                stencil_mask = 0;
            }
            int partial_stencil = stencil_mask && stencil_mask != 0xff;
            if (stencil_mask) clear_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
            if (!clear_aspects) break;
            uint32_t mip = g_rt.has_dview && g_rt.dview.set_levels ? g_rt.dview.base_level : 0;
            uint32_t base_layer = g_rt.has_dview && g_rt.dview.set_layers ? g_rt.dview.min_layer : 0;
            uint32_t layer_count = g_rt.has_dview && g_rt.dview.set_layers ? g_rt.dview.nlayers : e->layers;
            if (mip >= e->levels || base_layer >= e->layers || !layer_count || layer_count > e->layers - base_layer) {
                LOG("[RED] [nvn/vk] depth/stencil clear attachment view is out of range"); break;
            }
            uint32_t width = (uint32_t)g_rt.depth->d.width >> mip;
            uint32_t height = (uint32_t)g_rt.depth->d.height >> mip;
            if (!width) width = 1;
            if (!height) height = 1;
            int64_t x0 = g_st.has_scissor ? g_st.sc_x : 0;
            int64_t y0 = g_st.has_scissor ? g_st.sc_y : 0;
            int64_t x1 = g_st.has_scissor ? x0 + g_st.sc_w : width;
            int64_t y1 = g_st.has_scissor ? y0 + g_st.sc_h : height;
            if (g_st.has_scissor && (g_st.sc_w < 0 || g_st.sc_h < 0)) {
                LOG("[RED] [nvn/vk] depth/stencil clear has a negative scissor extent"); break;
            }
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > width) x1 = width;
            if (y1 > height) y1 = height;
            if (x1 <= x0 || y1 <= y0) break;
            VkCommandBuffer cb = rp_cb();
            if (!cb) break;
            nvkd_rp_close();   // a clear records outside any open render pass
            float depth = c->depth;
            if (!(depth >= 0.0f)) depth = 0.0f;
            else if (depth > 1.0f) depth = 1.0f;
            VkClearDepthStencilValue v = {depth, (uint32_t)c->stencil & 0xff};
            VkImageSubresourceRange range = {clear_aspects, mip, 1, base_layer, layer_count};
            int is_3d = g_rt.depth->d.target == NVN_TEXTURE_TARGET_3D;
            uint32_t slice_count = is_3d ? (uint32_t)g_rt.depth->d.depth >> mip : 1;
            if (!slice_count) slice_count = 1;
            int rectangular_clear = x0 != 0 || y0 != 0 || x1 != width || y1 != height;
            // Honeykrisp's transfer clear path can call a missing format
            // clear callback for depth/stencil images. Attachment clears have
            // the same NVN semantics for 2D, array and cube targets, and also
            // preserve the exact rectangle when a scissor is active. Keep the
            // transfer path for full 3D clears, which do not need a 2D view.
            // A partial stencil-bit mask needs the triangle: a Vulkan attachment
            // clear writes all eight bits. It also needs a 2D view for 3D
            // targets, like a scissored clear.
            int attachment_clear = partial_stencil || !is_3d || rectangular_clear;
            if (attachment_clear) {
                if (!e->can_attach) {
                    static int warned;
                    if (!warned++) LOG("[RED] [nvn/vk] depth/stencil clear target format %d cannot be a render target on this device", g_rt.depth->d.format);
                    rp_done(cb); break;
                }
                if (is_3d && !g_vk.caps.image_2d_view_of_3d) {
                    rp_done(cb);
                    LOG("[RED] [nvn/vk] partial depth/stencil 3D clear requires image2DViewOf3D"); break;
                }
                VkRenderPass rp = rp_get(0, NULL, e->fmt, e->samples);
                if (!rp) {
                    rp_done(cb);
                    LOG("[RED] [nvn/vk] could not create depth/stencil clear render pass"); break;
                }
                VkPipeline tri = VK_NULL_HANDLE;
                if (partial_stencil) {
                    clr_key k = {0};
                    k.depth = e->fmt; k.samples = e->samples; k.stencil_mask = stencil_mask;
                    k.has_depth = (clear_aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
                    k.depth_write = k.has_depth;
                    tri = clear_pipeline(&k, rp);
                    if (!tri) {
                        rp_done(cb);
                        LOG("[RED] [nvn/vk] partial stencil clear has no clear-triangle pipeline"); break;
                    }
                }
                VkImageMemoryBarrier b = {0};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.oldLayout = e->layout; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = e->img; b.subresourceRange = range;
                b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                     0, 0, NULL, 0, NULL, 1, &b);
                uint32_t slices = is_3d ? slice_count : (partial_stencil ? layer_count : 1);
                int clear_ok = 1;
                for (uint32_t slice = 0; slice < slices; slice++) {
                    // A partial stencil mask is drawn as a triangle, which
                    // reaches only layer 0 of a layered framebuffer: use one
                    // single-layer pass per array layer.
                    uint32_t vlayers = is_3d ? 1 : (partial_stencil ? 1 : layer_count);
                    uint32_t vbase = is_3d ? slice : (partial_stencil ? base_layer + slice : base_layer);
                    VkImageViewCreateInfo vi = {0};
                    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = e->img;
                    vi.viewType = vlayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
                    vi.format = e->fmt;
                    vi.subresourceRange = (VkImageSubresourceRange){clear_aspects, mip, 1,
                        vbase, vlayers};
                    VkImageView iv = VK_NULL_HANDLE;
                    if (vkCreateImageView(g_vk.device, &vi, NULL, &iv) != VK_SUCCESS) {
                        LOG("[RED] [nvn/vk] could not create depth/stencil clear image view");
                        clear_ok = 0; break;
                    }
                    VkFramebuffer fb = VK_NULL_HANDLE;
                    VkFramebufferCreateInfo fi = {0};
                    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fi.renderPass = rp;
                    fi.attachmentCount = 1; fi.pAttachments = &iv; fi.width = width; fi.height = height;
                    fi.layers = vlayers;
                    if (vkCreateFramebuffer(g_vk.device, &fi, NULL, &fb) != VK_SUCCESS) {
                        sub_free_iv(iv);
                        LOG("[RED] [nvn/vk] could not create depth/stencil clear framebuffer");
                        clear_ok = 0; break;
                    }
                    VkRenderPassBeginInfo bi = {0};
                    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; bi.renderPass = rp; bi.framebuffer = fb;
                    bi.renderArea = (VkRect2D){{0, 0}, {width, height}};
                    atomic_fetch_add_explicit(&g_stat_passes, 1, memory_order_relaxed);
                    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
                    if (partial_stencil) {
                        const uint32_t bits[4] = {0};
                        clear_tri_draw(cb, tri, width, height, (int32_t)x0, (int32_t)y0,
                                       (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), bits, depth,
                                       (uint32_t)c->stencil & 0xff);
                    } else {
                        VkClearAttachment ca = {0}; ca.aspectMask = clear_aspects; ca.clearValue.depthStencil = v;
                        VkClearRect cr = {{{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}},
                            0, vlayers};
                        vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
                    }
                    vkCmdEndRenderPass(cb);
                    sub_free_fb(fb); sub_free_iv(iv);
                }
                if (!clear_ok) { rp_done(cb); break; }
                e->layout = VK_IMAGE_LAYOUT_GENERAL;
            } else {
                VkImageMemoryBarrier b = {0};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.oldLayout = e->layout; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = e->img; b.subresourceRange = range;
                b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, NULL, 0, NULL, 1, &b);
                vkCmdClearDepthStencilImage(cb, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
                b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                     0, 0, NULL, 0, NULL, 1, &b);
                e->layout = VK_IMAGE_LAYOUT_GENERAL;
            }
            rp_done(cb);
            e->gpu_dirty = 1; e->wgen++;
            break;
        }
        case OP_BARRIER: {
            int32_t bits = 0;
            if (h.size >= sizeof(op_hdr) + sizeof bits) memcpy(&bits, a, sizeof bits);
            nvkd_barrier(bits);
            break;
        }
        case OP_DRAW: {
            p_draw dt; const p_draw *d = draw_rec(a, h.size, &dt);
            if (g_rc_skip) break;
#ifdef NXNATIVE_TEST_SHADERS
            if (nx_env_bool("NX_VK_TEST_SHADERS", 0) && d->count > 0 && g_rt.n > 0 && g_rt.color[0] && g_vbuf_addr) {
                render_triangle(g_rt.color[0]);
                break;
            }
#endif
            if (nx_env_bool("NX_VK_DEBUG", 0)) LOG("[nvn/vk] OP_DRAW prim %d first %d count %d instances %d vertex program %p rt %d", d->prim, d->first, d->count, d->instances, (void *)g_bound_stage[0], g_rt.n);
            if (g_bound_stage[0]) nvkd_draw_common(g_bound_stage, 0, d, NULL);
            else warn_once(&g_draw_warned, "NVN draw without a bound vertex program");
            break;
        }
        case OP_DRAW_IDX: {
            if (g_rc_skip) break;
            p_draw_idx dt; const p_draw_idx *d = draw_idx_rec(a, h.size, &dt);
            if (g_bound_stage[0]) nvkd_draw_common(g_bound_stage, 1, NULL, d);
            else warn_once(&g_draw_warned, "NVN indexed draw without a bound vertex program");
            break;
        }
        case OP_DRAW_TEX: if (!g_rc_skip) draw_tex_record((const p_draw_tex *)a); break;
        case OP_MULTI_DRAW: if (!g_rc_skip) nvkd_multi_draw((const p_multi_draw *)a); break;
        case OP_TF_BIND: case OP_TF_STATE: warn_once(&g_draw_warned, "NVN transform feedback"); break;
        case OP_WAIT_EVENT: case OP_WAIT_SYNC: warn_once(&g_draw_warned, "NVN wait on an event or sync"); break;
        case OP_RENDER_COND: nvn_render_condition((const p_render_cond *)a); break;
        case OP_DLSS: warn_once(&g_draw_warned, "NVN DLSS evaluation"); break;
        case OP_DISPATCH: {
            const p_dispatch *d = (const p_dispatch *)a;
            if (g_bound_stage[5]) nvkd_dispatch(d);
            else warn_once(&g_dispatch_warned, "NVN compute dispatch without a bound program");
            break;
        }
        default: break;
        }
        if (prof) { g_op_ns[h.op] += nx_monotonic_ns() - t0; g_op_n[h.op]++; }
        p += h.size;
    }
}

// Diagnostics: real values where the backend has them, zero/empty otherwise.
uint32_t mtl_present_qos(void) { return 0; }
int mtl_fps_target(void) {
    const char *e = nx_env("DM_FPS");
    if (e) { int v = atoi(e); if (v == 30 || v == 60) return v; }
    return 60;
}
double mtl_gpu_ms(uint64_t f) { (void)f; return 0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; if (t0) *t0 = 0; if (t1) *t1 = 0; return 0; }
void mtl_set_frame(uint64_t f) { (void)f; }
void mtl_set_depth_zero(int on) {
    if (g_st.depth_zero != (on != 0) && g_npvk > 0)
        LOG("[RED] [nvn/vk] the depth mode changed after programs were loaded; the new mode applies to programs loaded from now on");
    g_st.depth_zero = on != 0;
}
void mtl_set_origin_lower_left(int on) { g_origin_ll = on != 0; }
// A queue's raster flip is captured when it is initialized; it can differ from
// the device's current window origin, so it must not be folded into the latter
// (the same distinction as the Metal host's queue_t.origin_ll).
void mtl_set_queue_origin_lower_left(int on) { g_queue_origin_ll = on != 0; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
void mtl_ab_gpu(uint64_t f, double gpu_ms, double frame_ms) { (void)f; (void)gpu_ms; (void)frame_ms; }
// Cumulative replays, as the Metal host reports them: the periodic frame
// report subtracts the previous sample.
void mtl_stats(uint64_t *passes, uint64_t *draws) {
    if (passes) *passes = atomic_load_explicit(&g_stat_passes, memory_order_relaxed);
    if (draws) *draws = atomic_load_explicit(&g_stat_draws, memory_order_relaxed);
}
void mtl_args_stats(uint64_t *enc, uint64_t *reuse, uint64_t *patch) { if (enc) *enc = 0; if (reuse) *reuse = 0; if (patch) *patch = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { if (call) *call = 0; if (skip) *skip = 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw) { if (load) *load = 0; if (store) *store = 0; if (full_rw) *full_rw = 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
void mtl_vbuf_stats(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d, uint64_t *e, uint64_t *f) { if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; if (d) *d = 0; if (e) *e = 0; if (f) *f = 0; }
void mtl_pkg_stats(uint64_t *a, uint64_t *b) { if (a) *a = 0; if (b) *b = 0; }
unsigned mtl_cpu_tex_count(void) { return 0; }
uint64_t mtl_pkg_edge_skips(void) { return 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { if (lib) *lib = 0; if (pso) *pso = 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { if (lib_ns) *lib_ns = 0; if (pso_ns) *pso_ns = 0; }
void mtl_res_stats(char *out, size_t n) { if (out && n) out[0] = 0; }
void mtl_barrier_stats(uint64_t *skip, uint64_t *full) {
    if (skip) *skip = 0;
    if (full) *full = atomic_load_explicit(&g_vk_barriers, memory_order_relaxed);
}
void mtl_dispatch_barrier_stats(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d) { if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; if (d) *d = 0; }
uint64_t mtl_cut_count(void) { return 0; }
uint64_t mtl_copy_cut_count(void) { return 0; }
uint64_t mtl_blit_splits(void) { return 0; }
void mtl_ubd_stats(uint64_t *a, uint64_t *b) { if (a) *a = 0; if (b) *b = 0; }
void mtl_ubp_stats(uint64_t *a, uint64_t *b, uint64_t *c) { if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; }
void mtl_ubp_why(uint64_t w[6]) { for (int i = 0; i < 6; i++) w[i] = 0; }
void mtl_lc_stats(uint64_t *a, uint64_t *b) { if (a) *a = 0; if (b) *b = 0; }
void mtl_sm_stats(uint64_t *a, uint64_t *b, uint64_t *c) { if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; }
void mtl_cshoist_stats(uint64_t *a, uint64_t *b, uint64_t c[8]) { if (a) *a = 0; if (b) *b = 0; for (int i = 0; i < 8; i++) c[i] = 0; }
void mtl_geohoist_stats(uint64_t *a, uint64_t *b, uint64_t c[6]) { if (a) *a = 0; if (b) *b = 0; for (int i = 0; i < 6; i++) c[i] = 0; }
void mtl_geomesh_stats(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d, uint64_t e[5]) { if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; if (d) *d = 0; for (int i = 0; i < 5; i++) e[i] = 0; }
uint64_t mtl_geocomp_stats(char *out, size_t n) { if (out && n) out[0] = 0; return 0; }
void mtl_fb_stats(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d) { if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; if (d) *d = 0; }
void mtl_pool_stats(uint64_t s[6]) { for (int i = 0; i < 6; i++) s[i] = 0; }
void mtl_gsb_stats(uint64_t *a, uint64_t *b) { if (a) *a = 0; if (b) *b = 0; }
void mtl_tf_stats(uint64_t v[4]) { for (int i = 0; i < 4; i++) v[i] = 0; }
void mtl_hybrid_stats(uint64_t *a, uint64_t *b, uint64_t *c) { if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; }
void mtl_mem_stat(unsigned long long frame, unsigned long long mb) { (void)frame; (void)mb; }
void mtl_pso_line(char *out, size_t n) { if (out && n) out[0] = 0; }
int mtl_probe_ramp_on(void) { return 0; }
int mtl_warm_wait(int timeout_ms) { (void)timeout_ms; return 0; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
int mtl_link_wait_ticks(int ticks, uint64_t timeout_ns) { (void)ticks; (void)timeout_ns; return 0; }
uint64_t mtl_link_drain_take_ns(void) { return 0; }
int mtl_link_rate(void) { return 60; }
void mtl_link_mode_set(int mode) { (void)mode; }
double mtl_presented_time(uint64_t n) { (void)n; return -1; }
int mtl_link_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms) { (void)n; if (target) *target = 0; if (lead_ms) *lead_ms = 0; if (ticks) *ticks = 0; if (gap_ms) *gap_ms = 0; return 0; }
int mtl_link_stalled(void) { return 0; }
int mtl_snap_on(void) { return 0; }
uint64_t mtl_snap_acquire_wait(void) { return 0; }
uint64_t mtl_present_screen_wait(double period) { (void)period; return 0; }
void mtl_present_recorded(uint64_t frame) { (void)frame; }
int mtl_link_wait_vsync(uint64_t *seen, uint64_t timeout_ns) { (void)timeout_ns; if (seen) *seen = 0; return 0; }
void mtl_alloc_stats(uint64_t *a, uint64_t *b, int *c) { if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; }
void xv_test_counts(int *dc, int *viol) { if (dc) *dc = 0; if (viol) *viol = 0; }
uint64_t mtl_mtl_unused(void) { return 0; }

// Capture hooks are diagnostics; the capture writer is not part of this build.
void nvn_cap_submit(int n, const uint64_t *handles) { (void)n; (void)handles; }
void nvn_cap_present(const tex_t *t, const int crop[4]) { (void)t; (void)crop; }
void nvn_cap_queue(uint64_t serial, int waits) { (void)serial; (void)waits; }

uint64_t mach_absolute_time(void) { return nx_monotonic_ns(); }
