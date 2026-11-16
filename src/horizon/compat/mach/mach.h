#pragma once
// Small Mach compatibility surface for the host sources. Only the calls that
// have an exact Linux equivalent are provided: safe reads/writes of the current
// address space. Thread-state inspection is implemented in hle_thread.c under
// __linux__ with /proc and signal-safe context access, not emulated here.
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

typedef int kern_return_t;
typedef uint64_t mach_port_t;
typedef uintptr_t vm_address_t;
typedef uintptr_t mach_vm_address_t;
typedef size_t vm_size_t;
typedef size_t mach_vm_size_t;
typedef int vm_prot_t;
typedef int vm_inherit_t;
typedef int boolean_t;

#define KERN_SUCCESS 0
#define KERN_INVALID_ARGUMENT 4
#define KERN_FAILURE 5
#define MACH_PORT_NULL 0
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_PROT_ALL 7

static inline mach_port_t mach_task_self(void) { return (mach_port_t)getpid(); }
static inline mach_port_t mach_thread_self(void) { return 0; }

static inline kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t n, vm_address_t out, vm_size_t *got) {
    (void)task;
    struct iovec local = {(void *)out, n};
    struct iovec remote = {(void *)addr, n};
    ssize_t r = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (r < 0) { if (got) *got = 0; return KERN_INVALID_ARGUMENT; }
    if (got) *got = (vm_size_t)r;
    return r == (ssize_t)n ? KERN_SUCCESS : KERN_INVALID_ARGUMENT;
}

static inline kern_return_t vm_write(mach_port_t task, vm_address_t addr, vm_address_t data, vm_size_t n) {
    (void)task;
    struct iovec local = {(void *)data, n};
    struct iovec remote = {(void *)addr, n};
    ssize_t r = process_vm_writev(getpid(), &local, 1, &remote, 1, 0);
    return r == (ssize_t)n ? KERN_SUCCESS : KERN_INVALID_ARGUMENT;
}

// Thread introspection surface. Linux has no task/thread port API; these
// helpers report failure so callers fall back to their portable paths. The
// guest scheduler itself does not depend on them.
#include <pthread.h>
typedef uint32_t mach_msg_type_number_t;
typedef int thread_act_t;
typedef thread_act_t *thread_act_array_t;
typedef struct {
    struct { int32_t seconds; int32_t microseconds; } user_time, system_time;
    int run_state;
} thread_basic_info_data_t;
#define THREAD_BASIC_INFO 0
#define THREAD_BASIC_INFO_COUNT (sizeof(thread_basic_info_data_t) / sizeof(int))
typedef uint32_t thread_state_t;
typedef int *thread_info_t;
typedef struct {
    uint64_t __x[29];
    uint64_t __fp, __lr, __sp, __pc;
    uint32_t __cpsr;
    uint32_t __pad;
} arm_thread_state64_t;
#define ARM_THREAD_STATE64 0
#define ARM_THREAD_STATE64_COUNT (sizeof(arm_thread_state64_t) / sizeof(uint32_t))
#define __darwin_arm_thread_state64_get_pc(st) ((st).__pc)
#define __darwin_arm_thread_state64_get_fp(st) ((st).__fp)
#define __darwin_arm_thread_state64_get_lr(st) ((st).__lr)
#define __darwin_arm_thread_state64_get_sp(st) ((st).__sp)
static inline kern_return_t thread_info(thread_act_t t, int flavor, thread_info_t info, mach_msg_type_number_t *c) { (void)t; (void)flavor; (void)info; (void)c; return KERN_FAILURE; }
static inline kern_return_t thread_suspend(thread_act_t t) { (void)t; return KERN_FAILURE; }
static inline kern_return_t thread_resume(thread_act_t t) { (void)t; return KERN_FAILURE; }
static inline kern_return_t thread_get_state(thread_act_t t, int flavor, thread_state_t st, mach_msg_type_number_t *c) { (void)t; (void)flavor; (void)st; (void)c; return KERN_FAILURE; }
static inline kern_return_t task_threads(mach_port_t task, thread_act_array_t *list, mach_msg_type_number_t *n) { (void)task; *list = NULL; *n = 0; return KERN_FAILURE; }
static inline kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t port) { (void)task; (void)port; return KERN_SUCCESS; }
static inline kern_return_t vm_deallocate(mach_port_t task, vm_address_t addr, vm_size_t size) { (void)task; (void)addr; (void)size; return KERN_SUCCESS; }
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { (void)t; return 0; }
#define vm_page_size ((vm_size_t)getpagesize())

// ---- Diagnostics-only Mach surface ----
typedef int *task_info_t;
typedef struct { uint64_t phys_footprint, internal, compressed, external; } task_vm_info_data_t;
#define TASK_VM_INFO 0
#define TASK_VM_INFO_COUNT (sizeof(task_vm_info_data_t) / sizeof(int))
typedef struct { uint64_t total_user, total_system, task_interrupt_wakeups, task_platform_idle_wakeups, task_gpu_utilisation; } task_energy_data_t;
typedef struct { task_energy_data_t cpu_energy, gpu_energy; uint64_t task_ptime; } task_power_info_v2_data_t;
#define TASK_POWER_INFO_V2 0
#define TASK_POWER_INFO_V2_COUNT (sizeof(task_power_info_v2_data_t) / sizeof(int))
typedef struct { int pth_curpri, pth_priority, pth_policy; } thread_extended_info_data_t;
#define THREAD_EXTENDED_INFO 0
#define THREAD_EXTENDED_INFO_COUNT (sizeof(thread_extended_info_data_t) / sizeof(int))
typedef struct { uint64_t thread_id; } thread_identifier_info_data_t;
#define THREAD_IDENTIFIER_INFO 0
#define THREAD_IDENTIFIER_INFO_COUNT (sizeof(thread_identifier_info_data_t) / sizeof(int))
typedef struct { uint32_t period, computation, constraint, preemptible; } thread_time_constraint_policy_data_t;
#define THREAD_TIME_CONSTRAINT_POLICY 0
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT (sizeof(thread_time_constraint_policy_data_t) / sizeof(int))
typedef struct { int unused; } thread_standard_policy_data_t;
#define THREAD_STANDARD_POLICY 0
#define THREAD_STANDARD_POLICY_COUNT (sizeof(thread_standard_policy_data_t) / sizeof(int))
typedef void *thread_policy_t;
static inline kern_return_t task_info(mach_port_t task, int flavor, task_info_t info, mach_msg_type_number_t *c) { (void)task; (void)flavor; (void)info; (void)c; return KERN_FAILURE; }
static inline kern_return_t thread_policy_set(thread_act_t t, int flavor, thread_policy_t p, mach_msg_type_number_t c) { (void)t; (void)flavor; (void)p; (void)c; return KERN_FAILURE; }
static inline int pthread_threadid_np(const void *t, uint64_t *id) { (void)t; if (id) *id = (uint64_t)getpid(); return 0; }
static inline int sysctlbyname(const char *name, void *old, size_t *oldlen, void *newp, size_t newlen) { (void)name; (void)old; (void)oldlen; (void)newp; (void)newlen; errno = ENOTSUP; return -1; }
static inline int mach_wait_until(uint64_t deadline) {
    struct timespec ts = {(time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull)};
    return clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}
#define VM_FLAGS_FIXED 0
#define VM_FLAGS_OVERWRITE 0
#define VM_INHERIT_NONE 0
#define FALSE 0
// Linux guest memory is anonymous, so a true alias is not available without a
// memfd-backed guest address space. This compatibility maps a private copy and
// logs the limitation; virtual-pool games need the memfd conversion.
static inline kern_return_t vm_remap(mach_port_t task, vm_address_t *to, vm_size_t size, vm_size_t mask, int flags,
                                     mach_port_t src_task, vm_address_t from, boolean_t copy, vm_prot_t *cur,
                                     vm_prot_t *max, vm_inherit_t inherit) {
    (void)task; (void)mask; (void)flags; (void)src_task; (void)copy; (void)inherit;
    void *p = mmap((void *)(uintptr_t)*to, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return KERN_FAILURE;
    memcpy(p, (const void *)(uintptr_t)from, size);
    if (cur) *cur = VM_PROT_READ | VM_PROT_WRITE;
    if (max) *max = VM_PROT_ALL;
    *to = (vm_address_t)(uintptr_t)p;
    return KERN_SUCCESS;
}
