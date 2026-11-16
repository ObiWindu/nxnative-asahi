# Memory model: 4 KiB guest pages on 16 KiB Asahi pages

## The invariant

```text
guest_page_size = 4096
host_page_size  = sysconf(_SC_PAGESIZE)     // 16384 on Asahi
host_page_size >= guest_page_size
host_page_size % guest_page_size == 0
```

No code may assume `host_page_size == 4096`, and no code may treat a guest
address range as if it were host-page aligned. The platform checks the geometry
at startup and fails if it is not representable.

## Identity mapping

Guest memory is mapped at the address the guest sees. NVN and Vulkan titles
pass guest addresses to the GPU, and the existing host uses those addresses as
buffer addresses. `NxMemRange` therefore keeps `base == guest base` and records:

- `size`, rounded to a host page;
- one permission/dirty byte per 4 KiB guest page;
- one cached `PROT_*` value per host page;
- an optional `memfd` for aliases.

## Protection

`nx_mem_protect(range, addr, len, prot)`:

1. updates the 4 KiB guest entries in `[addr, addr+len)`;
2. finds every host page touching that range;
3. computes the union of the guest permissions in each host page;
4. calls `mprotect` once per host page with the union.

A host page whose four guest pages disagree cannot be enforced exactly by the
hardware. It receives the union (over-permissive, never under-permissive) and is
counted by `nx_mem_approximate_pages()`; the operation logs at debug level. The
loader keeps module text and writable data on separate 16 KiB boundaries, so the
normal guest layout does not mix them.

If a host page needs both write and execute, the code requests `RWX`. If the
kernel or SELinux refuses it, the error is propagated; execute or write is never
silently dropped.

## Dirty tracking

`nx_mem_dirty_track(range, addr, len)` sets the covering host pages to
`PROT_READ`. A write fault is handled by the graphics dirty tracker; it calls
`nx_mem_dirty_commit(range, fault_addr, ...)`, which:

- makes the whole host page writable again;
- marks **all four** 4 KiB guest pages in that host page dirty;
- returns the first dirty guest address and span.

This over-approximates an upload range, but never misses a CPU write. The
existing `nvn_dirty.c` uses this model; the 4 KiB state is per host page, so a
fault on one 4 KiB guest page dirties its three neighbours in the same host
page.

## Aliases and shared memory

Horizon aliases and virtual memory pools map the same physical memory at more
than one address. `NxMemAlias` uses `memfd_create` plus `mmap(MAP_SHARED)` at each
address. Writes through one alias are visible through the other; the memory has
one physical backing, as on the Switch.

The original macOS host used `vm_remap` for this. Linux `memfd` is the direct
equivalent for anonymous shared memory and does not require a filesystem.

## Generated and rewritten code

- The loader copies module text into anonymous pages, then seals
  `[0, text_end)` and the patch area `PROT_READ|PROT_EXEC`.
- The rewriter's patch area is executable and is never writable after the seal.
- `nx_mem_make_executable` performs the final `mprotect(RX)` and
  `__builtin___clear_cache` for generated code.
- The memory test writes two AArch64 instructions (`mov w0, #42; ret`), switches
  the page to RX, calls it and checks the return value.

## JIT

The Linux JIT path uses ordinary `mmap`/`mprotect` transitions rather than
`MAP_JIT` and `pthread_jit_write_protect_np`. The code page is writable only
while code is being produced; it is RX while executing. Fedora's SELinux policy
allows the transition in the tested environment; a refusal is reported as an
error.

## Tests

`tests/t_memory_16k.c` runs on the actual host page size and checks:

- host/guest geometry;
- a 64 KiB range and its permission table;
- a mixed 16 KiB page is reported as approximate (on a 4 KiB host page the same protection is exact);
- a `PROT_NONE` host page faults, its neighbour still writes;
- two memfd aliases share writes;
- a dirty-tracked host page faults, unprotects, and marks all four guest pages;
- the dirty span can be drained;
- generated AArch64 code runs.

## Limitations

- 4 KiB protection inside a 16 KiB page is approximate. Exact enforcement would
  require either a software page table with fault emulation or dual mappings;
  neither is needed for the tested NSO layout, where code and data are separated
  at 16 KiB.
- Growing or shrinking a guest mapping at 4 KiB granularity changes the whole
  host page. The host memory code rounds outward and keeps the union.
- The current guest execution test does not exercise a real game's allocation
  pattern. The core SVC memory code comes from nxnative's penultimate macOS 16 KiB
  implementation, but it still needs a game/homebrew run here.
