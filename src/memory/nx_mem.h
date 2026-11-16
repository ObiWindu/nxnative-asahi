// Guest memory with 4 KiB guest pages on a 16 KiB (or any) host page.
//
// Horizon's page size is 4096. Asahi's kernel page size is 16384. The CPU and
// the GPU see guest addresses directly, so the host mapping must be contiguous
// and at the guest address. Protection is the only thing that cannot be
// expressed at 4 KiB granularity: the kernel protects whole host pages. This
// module keeps a 4 KiB permission/dirty table and derives host mprotect calls
// from it. When guest pages inside one host page disagree, the host page gets
// the union of their permissions; the caller can query how many host pages are
// approximate (the loader keeps code and data apart at 16 KiB boundaries).
#pragma once
#include <stddef.h>
#include <stdint.h>

#define NX_GUEST_PAGE 4096u

enum {
    NX_PAGE_READ    = 1u << 0,
    NX_PAGE_WRITE   = 1u << 1,
    NX_PAGE_EXEC    = 1u << 2,
    NX_PAGE_MAPPED  = 1u << 3,
    NX_PAGE_DIRTY   = 1u << 4,
    NX_PAGE_TRACKED = 1u << 5,
};

typedef struct NxMemRange {
    uint8_t *base;
    size_t size;
    size_t guest_pages;
    size_t host_pages;
    uint8_t *perm;       // one byte per 4 KiB guest page
    uint8_t *host_prot;  // cached PROT_* per host page
    int fd;              // memfd, or -1 for anonymous
    int owns_fd;
} NxMemRange;

typedef struct NxMemAlias {
    int fd;
    size_t size;
} NxMemAlias;

size_t nx_host_page_size(void);
size_t nx_guest_page_size(void);
int nx_page_shift(void);
size_t nx_host_pages_for(size_t bytes);
size_t nx_guest_pages_for(size_t bytes);
int nx_mem_page_is_host_aligned(void);

// Anonymous guest memory at an arbitrary host address (identity-mapped: the
// returned base is the guest address). size is rounded up to the host page.
NxMemRange *nx_mem_create(size_t size);
NxMemRange *nx_mem_create_at(uintptr_t addr, size_t size);
void nx_mem_destroy(NxMemRange *r);

// memfd-backed memory that can be mapped at several guest addresses (Horizon's
// aliases, virtual memory pools, GPU-visible shared memory).
NxMemAlias *nx_mem_alias_create(size_t size);
void *nx_mem_alias_map(NxMemAlias *a, uintptr_t addr, size_t size, int prot);
void nx_mem_alias_unmap(void *p, size_t size);
void nx_mem_alias_destroy(NxMemAlias *a);

// Protect a 4 KiB-granular range. prot is a bitmask of NX_PAGE_READ,
// NX_PAGE_WRITE, NX_PAGE_EXEC, or 0 to unmap. Returns 0, or an errno value.
int nx_mem_protect(NxMemRange *r, uint64_t addr, size_t len, int prot);

// Change protection of one host page's worth of guest pages and return the
// number of guest pages in that host page; used by the dirty tracker.
int nx_mem_dirty_track(NxMemRange *r, uint64_t addr, size_t len);
int nx_mem_dirty_commit(NxMemRange *r, uint64_t fault_addr, uint64_t *first_dirty, size_t *dirty_len);
int nx_mem_dirty_clear(NxMemRange *r, uint64_t addr, size_t len);
int nx_mem_dirty_take(NxMemRange *r, uint64_t *first_dirty, size_t *dirty_len);

// Permission queries for diagnostics/tests.
int nx_mem_perm(const NxMemRange *r, uint64_t addr);
int nx_mem_effective_host_prot(const NxMemRange *r, uint64_t addr);
int nx_mem_approximate_pages(const NxMemRange *r); // host pages whose 4 guest pages disagree
int nx_mem_validate(const NxMemRange *r);

// Instruction cache maintenance for generated/rewritten code.
void nx_icache_flush(void *addr, size_t len);
int nx_mem_make_executable(void *addr, size_t len);

// Report the capability matrix at startup.
void nx_mem_report(void);
