#include "memory/nx_mem.h"
#include "platform/linux/nx_platform.h"

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
                              else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static NxMemRange *g_dirty_range;
static volatile sig_atomic_t g_dirty_faults;
static sigjmp_buf g_dirty_jmp;

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)uc;
    if (!g_dirty_range) return;
    uint64_t first = 0;
    size_t len = 0;
    if (nx_mem_dirty_commit(g_dirty_range, (uint64_t)(uintptr_t)si->si_addr, &first, &len) == 0) {
        g_dirty_faults++;
        siglongjmp(g_dirty_jmp, 1);
    }
}

static void install_fault_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL) || sigaction(SIGBUS, &sa, NULL)) { perror("sigaction"); exit(2); }
}

static int child_write_faults(void *addr, int expect_fault) {
    pid_t p = fork();
    if (p < 0) { perror("fork"); return -1; }
    if (!p) {
        *(volatile uint8_t *)addr = 0x5a;
        _exit(expect_fault ? 1 : 0);   // expected a fault; reaching here means it did not
    }
    int st = 0;
    waitpid(p, &st, 0);
    int faulted = WIFSIGNALED(st) && (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS);
    if (expect_fault) return faulted ? 0 : -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static void test_basic(void) {
    size_t hp = nx_host_page_size(), gp = nx_guest_page_size();
    CHECK(hp >= gp && (hp % gp) == 0, "host page %zu is a multiple of guest page %zu", hp, gp);
    CHECK(nx_mem_page_is_host_aligned(), "host/guest page geometry is representable");
    NxMemRange *r = nx_mem_create(64 * 1024);
    CHECK(r && nx_mem_validate(r), "64 KiB guest range created and validated");
    if (!r) return;
    CHECK(nx_mem_perm(r, (uint64_t)(uintptr_t)r->base + 0x2000) & NX_PAGE_WRITE, "fresh guest page is writable");
    int rc = nx_mem_protect(r, (uint64_t)(uintptr_t)r->base + 0x4000, 0x1000, NX_PAGE_READ);
    CHECK(rc == 0, "protect one 4 KiB guest page read-only (rc %d)", rc);
    CHECK(!(nx_mem_perm(r, (uint64_t)(uintptr_t)r->base + 0x4000) & NX_PAGE_WRITE), "guest table says the page is read-only");
    /* The 16 KiB host page holds one read-only and three read-write 4 KiB
       guest pages. The kernel cannot express that split, so the page keeps the
       union (RW); the approximation is counted below. */
    CHECK(nx_mem_effective_host_prot(r, (uint64_t)(uintptr_t)r->base + 0x4000) == (PROT_READ | PROT_WRITE),
          "host page keeps the union of its four guest pages (RW)");
    int approx = nx_mem_approximate_pages(r);
    CHECK(approx >= 1, "mixed host page reported as approximate (%d page(s))", approx);
    nx_mem_destroy(r);
}

static void test_faults(void) {
    size_t hp = nx_host_page_size();
    NxMemRange *r = nx_mem_create(hp * 2);
    if (!r) { CHECK(0, "fault range allocation"); return; }
    uint8_t *base = r->base;
    int rc = nx_mem_protect(r, (uint64_t)(uintptr_t)base + hp, hp, 0);
    CHECK(rc == 0, "unmap one host page of guest memory (rc %d)", rc);
    CHECK(child_write_faults(base + hp, 1) == 0, "write to a PROT_NONE host page faults");
    CHECK(child_write_faults(base, 0) == 0, "write to the neighbouring page still succeeds");
    nx_mem_destroy(r);
}

static void test_alias(void) {
    size_t hp = nx_host_page_size();
    NxMemAlias *a = nx_mem_alias_create(hp);
    CHECK(a != NULL, "memfd alias created");
    if (!a) return;
    uint8_t *p1 = nx_mem_alias_map(a, 0, hp, PROT_READ | PROT_WRITE);
    uint8_t *p2 = nx_mem_alias_map(a, 0, hp, PROT_READ | PROT_WRITE);
    CHECK(p1 && p2 && p1 != p2, "same memfd mapped at two guest addresses (%p, %p)", (void *)p1, (void *)p2);
    if (p1 && p2) {
        memset(p1, 0x3c, hp);
        CHECK(p2[0] == 0x3c && p2[hp - 1] == 0x3c, "write through one alias is visible through the other");
        p2[0] = 0xa5;
        CHECK(p1[0] == 0xa5, "write through the second alias is visible through the first");
        nx_mem_alias_unmap(p1, hp);
        nx_mem_alias_unmap(p2, hp);
    }
    nx_mem_alias_destroy(a);
}

static void test_dirty(void) {
    size_t hp = nx_host_page_size();
    NxMemRange *r = nx_mem_create(hp);
    if (!r) { CHECK(0, "dirty range allocation"); return; }
    install_fault_handler();
    g_dirty_range = r;
    int rc = nx_mem_dirty_track(r, (uint64_t)(uintptr_t)r->base, hp);
    CHECK(rc == 0, "track host page for dirty writes (rc %d)", rc);
    g_dirty_faults = 0;
    if (!sigsetjmp(g_dirty_jmp, 1)) {
        volatile uint8_t *q = r->base;
        q[0x2000] = 1;   /* first write faults, the handler unprotects the host page */
    }
    CHECK(g_dirty_faults == 1, "write fault was handled exactly once (%d)", (int)g_dirty_faults);
    r->base[0x0] = 3; r->base[0x3000] = 2;   /* the host page is writable again after the fault */
    int dirty = 0;
    for (size_t i = 0; i < r->guest_pages; i++) if (nx_mem_perm(r, (uint64_t)(uintptr_t)(r->base + i * nx_guest_page_size())) & NX_PAGE_DIRTY) dirty++;
    CHECK(dirty == (int)(hp / nx_guest_page_size()), "all %zu guest pages in the host page are marked dirty (%d)", hp / nx_guest_page_size(), dirty);
    uint64_t first = 0; size_t len = 0;
    int any = nx_mem_dirty_take(r, &first, &len);
    CHECK(any && len >= nx_guest_page_size(), "dirty span can be taken (%zu bytes)", len);
    g_dirty_range = NULL;
    nx_mem_destroy(r);
}

static void test_exec(void) {
#if defined(__aarch64__)
    size_t hp = nx_host_page_size();
    NxMemRange *r = nx_mem_create(hp);
    if (!r) { CHECK(0, "exec range allocation"); return; }
    uint32_t code[2] = {0x52800540u, 0xd65f03c0u};   // mov w0, #42; ret
    memcpy(r->base, code, sizeof code);
    int rc = nx_mem_make_executable(r->base, hp);
    CHECK(rc == 0, "write-to-execute transition for generated code (rc %d)", rc);
    if (!rc) {
        int (*fn)(void) = (int (*)(void))(void *)r->base;
        int v = fn();
        CHECK(v == 42, "generated AArch64 code executed (returned %d)", v);
    }
    nx_mem_destroy(r);
#else
    printf("skip: generated-code execution is AArch64-only on this host\n");
#endif
}

int main(void) {
    NxSysInfo si;
    nx_sysinfo_query(&si);
    nx_sysinfo_print(&si);
    nx_mem_report();
    test_basic();
    test_faults();
    test_alias();
    test_dirty();
    test_exec();
    printf("\n%s (%d failure%s)\n", g_fail ? "MEMORY TESTS FAILED" : "MEMORY TESTS PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
