#include "memory/nx_mem.h"
#include "platform/linux/nx_platform.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001u
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static size_t g_host_page;
static size_t g_guest_page = NX_GUEST_PAGE;
static int g_page_shift = 12;

static int is_pow2(size_t v) { return v && !(v & (v - 1)); }

size_t nx_host_page_size(void) {
    if (!g_host_page) {
        long p = sysconf(_SC_PAGESIZE);
        /* Never assume 4096: Asahi is 16384, and a future kernel could differ. */
        if (p <= 0 || !is_pow2((size_t)p) || (size_t)p < 4096) {
            nx_die("invalid host page size %ld from sysconf(_SC_PAGESIZE)", p);
        }
        g_host_page = (size_t)p;
    }
    return g_host_page;
}

size_t nx_guest_page_size(void) { return g_guest_page; }
int nx_page_shift(void) { return g_page_shift; }
size_t nx_host_pages_for(size_t bytes) { return (bytes + nx_host_page_size() - 1) / nx_host_page_size(); }
size_t nx_guest_pages_for(size_t bytes) { return (bytes + g_guest_page - 1) / g_guest_page; }
int nx_mem_page_is_host_aligned(void) { return nx_host_page_size() >= g_guest_page && (nx_host_page_size() % g_guest_page) == 0; }

static uint8_t prot_to_perm(int prot) {
    uint8_t p = NX_PAGE_MAPPED;
    if (prot & PROT_READ) p |= NX_PAGE_READ;
    if (prot & PROT_WRITE) p |= NX_PAGE_WRITE;
    if (prot & PROT_EXEC) p |= NX_PAGE_EXEC;
    return p;
}

static int perm_to_prot(uint8_t p) {
    int prot = 0;
    if (!(p & NX_PAGE_MAPPED)) return PROT_NONE;
    if (p & NX_PAGE_READ) prot |= PROT_READ;
    if (p & NX_PAGE_WRITE) prot |= PROT_WRITE;
    if (p & NX_PAGE_EXEC) prot |= PROT_EXEC;
    return prot;
}

static int perm_matches(uint8_t a, uint8_t b) { return (a & (NX_PAGE_READ | NX_PAGE_WRITE | NX_PAGE_EXEC)) == (b & (NX_PAGE_READ | NX_PAGE_WRITE | NX_PAGE_EXEC)); }

static int recompute_host_page(NxMemRange *r, size_t hp, int *approximate) {
    size_t hp_size = nx_host_page_size();
    size_t first = hp * hp_size / g_guest_page;
    size_t per = hp_size / g_guest_page; /* guaranteed integral: host >= guest and power of two */
    if (per > r->guest_pages - first) per = r->guest_pages - first;
    uint8_t u = 0;
    int mapped = 0;
    uint8_t first_perm = 0;
    for (size_t i = 0; i < per; i++) {
        uint8_t p = r->perm[first + i];
        if (!(p & NX_PAGE_MAPPED)) continue;
        if (!mapped) { first_perm = p; mapped = 1; }
        u |= p & (NX_PAGE_READ | NX_PAGE_WRITE | NX_PAGE_EXEC);
        if ((p & (NX_PAGE_READ | NX_PAGE_WRITE | NX_PAGE_EXEC)) != (first_perm & (NX_PAGE_READ | NX_PAGE_WRITE | NX_PAGE_EXEC))) {
            if (approximate) *approximate += 1;
        }
    }
    if (!mapped) {
        u = 0;
    } else {
        u |= NX_PAGE_MAPPED;
    }
    int prot = perm_to_prot(u);
    /* W^X is enforced by Fedora's SELinux in some configurations. A host page
       that genuinely needs both is requested as RWX; if the kernel refuses it,
       report the failure rather than silently dropping execute or write. */
    if (prot == (PROT_READ | PROT_WRITE | PROT_EXEC)) {
        void *addr = r->base + hp * hp_size;
        if (mprotect(addr, hp_size, prot) != 0 && errno == EACCES) {
            nx_log(NX_LOG_ERROR, "host page %p needs RWX for mixed 4 KiB guest permissions and the kernel refused it: %s", addr, strerror(errno));
            return EACCES;
        }
        r->host_prot[hp] = (uint8_t)prot;
        return 0;
    }
    if (mprotect(r->base + hp * hp_size, hp_size, prot) != 0) return errno;
    r->host_prot[hp] = (uint8_t)prot;
    return 0;
}

static NxMemRange *range_new(void *base, size_t size, int fd, int owns_fd) {
    size_t hp_size = nx_host_page_size();
    if (size % hp_size) size = (size + hp_size - 1) & ~(hp_size - 1);
    NxMemRange *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->base = base;
    r->size = size;
    r->guest_pages = size / g_guest_page;
    r->host_pages = size / hp_size;
    r->fd = fd;
    r->owns_fd = owns_fd;
    r->perm = malloc(r->guest_pages);
    r->host_prot = malloc(r->host_pages);
    if (!r->perm || !r->host_prot) { free(r->perm); free(r->host_prot); free(r); return NULL; }
    memset(r->perm, NX_PAGE_MAPPED | NX_PAGE_READ | NX_PAGE_WRITE, r->guest_pages);
    memset(r->host_prot, PROT_READ | PROT_WRITE, r->host_pages);
    return r;
}

NxMemRange *nx_mem_create(size_t size) {
    size_t hp = nx_host_page_size();
    size_t n = (size + hp - 1) & ~(hp - 1);
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { nx_log(NX_LOG_ERROR, "mmap(%zu) failed: %s", n, strerror(errno)); return NULL; }
    NxMemRange *r = range_new(p, n, -1, 0);
    if (!r) munmap(p, n);
    return r;
}

NxMemRange *nx_mem_create_at(uintptr_t addr, size_t size) {
    size_t hp = nx_host_page_size();
    size_t n = (size + hp - 1) & ~(hp - 1);
    void *want = (void *)addr;
    void *p = mmap(want, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) { nx_log(NX_LOG_ERROR, "mmap at %p (%zu) failed: %s", want, n, strerror(errno)); return NULL; }
    NxMemRange *r = range_new(p, n, -1, 0);
    if (!r) munmap(p, n);
    return r;
}

void nx_mem_destroy(NxMemRange *r) {
    if (!r) return;
    munmap(r->base, r->size);
    if (r->owns_fd && r->fd >= 0) close(r->fd);
    free(r->perm); free(r->host_prot); free(r);
}

NxMemAlias *nx_mem_alias_create(size_t size) {
    size_t hp = nx_host_page_size();
    if (!size) { errno = EINVAL; return NULL; }
    if (size > SIZE_MAX - (hp - 1)) { errno = EOVERFLOW; return NULL; }
    size = (size + hp - 1) & ~(hp - 1);
    int fd = (int)syscall(SYS_memfd_create, "nxnative-guest", MFD_CLOEXEC);
    if (fd < 0) { nx_log(NX_LOG_ERROR, "memfd_create failed: %s", strerror(errno)); return NULL; }
    if (ftruncate(fd, (off_t)size) != 0) { nx_log(NX_LOG_ERROR, "ftruncate(%zu) failed: %s", size, strerror(errno)); close(fd); return NULL; }
    NxMemAlias *a = calloc(1, sizeof *a);
    if (!a) { close(fd); return NULL; }
    a->fd = fd; a->size = size;
    return a;
}

void *nx_mem_alias_map(NxMemAlias *a, uintptr_t addr, size_t size, int prot) {
    return nx_mem_alias_map_at(a, addr, size, 0, prot);
}

void *nx_mem_alias_map_at(NxMemAlias *a, uintptr_t addr, size_t size, size_t backing_offset, int prot) {
    size_t hp = nx_host_page_size();
    if (!a || !size || (backing_offset & (hp - 1)) || backing_offset > a->size || size > a->size - backing_offset) {
        errno = EINVAL;
        return NULL;
    }
    if (addr && (addr & (hp - 1))) { errno = EINVAL; return NULL; }
    void *p = mmap((void *)addr, size, prot, MAP_SHARED | (addr ? MAP_FIXED_NOREPLACE : 0), a->fd, (off_t)backing_offset);
    if (p == MAP_FAILED) { nx_log(NX_LOG_ERROR, "alias mmap at %p (%zu) failed: %s", (void *)addr, size, strerror(errno)); return NULL; }
    return p;
}

void nx_mem_alias_unmap(void *p, size_t size) { if (p) munmap(p, size); }
void nx_mem_alias_destroy(NxMemAlias *a) { if (!a) return; close(a->fd); free(a); }

int nx_mem_protect(NxMemRange *r, uint64_t addr, size_t len, int prot) {
    if (!r || !len) return EINVAL;
    if (addr < (uint64_t)(uintptr_t)r->base) return EINVAL;
    size_t off = (size_t)(addr - (uintptr_t)r->base);
    if (off >= r->size) return EINVAL;
    if (len > r->size - off) len = r->size - off;
    size_t first = off / g_guest_page;
    size_t pages = (off % g_guest_page + len + g_guest_page - 1) / g_guest_page;
    uint8_t p = prot_to_perm(prot);
    for (size_t i = 0; i < pages && first + i < r->guest_pages; i++) r->perm[first + i] = p;
    size_t hp_size = nx_host_page_size();
    size_t h0 = (off) / hp_size;
    size_t h1 = (off + len - 1) / hp_size;
    int approximate = 0, rc = 0;
    for (size_t h = h0; h <= h1 && h < r->host_pages; h++) {
        int a = 0;
        int e = recompute_host_page(r, h, &a);
        if (e && !rc) rc = e;
        approximate += a;
    }
    if (approximate) NX_LOGD("guest protection is approximate across %d host page(s) at %p", approximate, (void *)r->base);
    return rc;
}

int nx_mem_dirty_track(NxMemRange *r, uint64_t addr, size_t len) {
    if (!r) return EINVAL;
    size_t off = (size_t)(addr - (uintptr_t)r->base);
    if (off >= r->size) return EINVAL;
    if (len > r->size - off) len = r->size - off;
    size_t hp = nx_host_page_size();
    size_t h0 = off / hp, h1 = (off + len - 1) / hp;
    for (size_t h = h0; h <= h1 && h < r->host_pages; h++) {
        if (mprotect(r->base + h * hp, hp, PROT_READ) != 0) return errno;
        r->host_prot[h] = PROT_READ;
        size_t first = h * hp / g_guest_page, per = hp / g_guest_page;
        if (first + per > r->guest_pages) per = r->guest_pages - first;
        for (size_t i = 0; i < per; i++) r->perm[first + i] |= NX_PAGE_TRACKED;
    }
    return 0;
}

int nx_mem_dirty_commit(NxMemRange *r, uint64_t fault_addr, uint64_t *first_dirty, size_t *dirty_len) {
    if (!r) return EINVAL;
    size_t hp = nx_host_page_size();
    size_t off = (size_t)(fault_addr - (uintptr_t)r->base);
    if (off >= r->size) return EINVAL;
    size_t h = off / hp;
    if (mprotect(r->base + h * hp, hp, PROT_READ | PROT_WRITE) != 0) return errno;
    r->host_prot[h] = PROT_READ | PROT_WRITE;
    size_t first = h * hp / g_guest_page, per = hp / g_guest_page;
    if (first + per > r->guest_pages) per = r->guest_pages - first;
    for (size_t i = 0; i < per; i++) { r->perm[first + i] |= NX_PAGE_DIRTY | NX_PAGE_TRACKED; }
    if (first_dirty) *first_dirty = (uint64_t)(uintptr_t)(r->base + first * g_guest_page);
    if (dirty_len) *dirty_len = per * g_guest_page;
    return 0;
}

int nx_mem_dirty_clear(NxMemRange *r, uint64_t addr, size_t len) {
    if (!r) return EINVAL;
    size_t off = (size_t)(addr - (uintptr_t)r->base);
    if (off >= r->size) return EINVAL;
    if (len > r->size - off) len = r->size - off;
    size_t first = off / g_guest_page, pages = (off % g_guest_page + len + g_guest_page - 1) / g_guest_page;
    for (size_t i = 0; i < pages && first + i < r->guest_pages; i++) r->perm[first + i] &= (uint8_t)~NX_PAGE_DIRTY;
    return 0;
}

int nx_mem_dirty_take(NxMemRange *r, uint64_t *first_dirty, size_t *dirty_len) {
    if (!r || !first_dirty || !dirty_len) return EINVAL;
    size_t i = 0;
    while (i < r->guest_pages && !(r->perm[i] & NX_PAGE_DIRTY)) i++;
    if (i == r->guest_pages) { *first_dirty = 0; *dirty_len = 0; return 0; }
    size_t j = i;
    while (j < r->guest_pages && (r->perm[j] & NX_PAGE_DIRTY)) j++;
    *first_dirty = (uint64_t)(uintptr_t)(r->base + i * g_guest_page);
    *dirty_len = (j - i) * g_guest_page;
    for (size_t k = i; k < j; k++) r->perm[k] &= (uint8_t)~NX_PAGE_DIRTY;
    return 1;
}

int nx_mem_perm(const NxMemRange *r, uint64_t addr) {
    if (!r) return 0;
    size_t off = (size_t)(addr - (uintptr_t)r->base);
    if (off >= r->size) return 0;
    return r->perm[off / g_guest_page];
}

int nx_mem_effective_host_prot(const NxMemRange *r, uint64_t addr) {
    if (!r) return 0;
    size_t hp = nx_host_page_size();
    size_t off = (size_t)(addr - (uintptr_t)r->base);
    if (off >= r->size) return 0;
    return r->host_prot[off / hp];
}

int nx_mem_approximate_pages(const NxMemRange *r) {
    if (!r || !nx_mem_page_is_host_aligned()) return (int)r->host_pages;
    int n = 0;
    size_t per = nx_host_page_size() / g_guest_page;
    for (size_t h = 0; h < r->host_pages; h++) {
        size_t first = h * per;
        uint8_t want = 0;
        int have = 0;
        for (size_t i = 0; i < per && first + i < r->guest_pages; i++) {
            uint8_t p = r->perm[first + i];
            if (!(p & NX_PAGE_MAPPED)) { if (have) { n++; break; } continue; }
            if (!have) { want = p; have = 1; }
            else if (!perm_matches(want, p)) { n++; break; }
        }
    }
    return n;
}

int nx_mem_validate(const NxMemRange *r) {
    if (!r) return 0;
    if ((uintptr_t)r->base % nx_host_page_size()) return 0;
    if (r->size % nx_host_page_size()) return 0;
    if (r->guest_pages * g_guest_page != r->size) return 0;
    if (r->host_pages * nx_host_page_size() != r->size) return 0;
    return 1;
}

void nx_icache_flush(void *addr, size_t len) { __builtin___clear_cache((char *)addr, (char *)addr + len); }

int nx_mem_make_executable(void *addr, size_t len) {
    if (mprotect(addr, len, PROT_READ | PROT_EXEC) != 0) return errno;
    nx_icache_flush(addr, len);
    return 0;
}

void nx_mem_report(void) {
    size_t hp = nx_host_page_size(), gp = nx_guest_page_size();
    NX_LOGI("memory model: host page %zu, guest page %zu, host/guest ratio %zu", hp, gp, hp / gp);
    NX_LOGI("memory model: 4 KiB guest permissions are enforced at %zu KiB host granularity; mixed pages are reported", hp / 1024);
    NX_LOGI("memory model: aliases use memfd_create + MAP_SHARED; JIT uses mprotect W^X transitions");
}
