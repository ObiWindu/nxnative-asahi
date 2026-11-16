# Asahi Linux architecture

This document describes the Asahi Linux fork of nxnative: what is reused, what is
replaced, and why. It is grounded in the source tree at the time of the fork, not
in the macOS application's published architecture.

## 1. What the existing codebase actually is

nxnative is not a conventional CPU-emulating Switch emulator. It runs the game's
own AArch64 code natively on Apple Silicon. The pieces that make this possible
are:

| Layer | Existing implementation | Reuse on Asahi |
|---|---|---|
| Guest code loading | `dm/host/loader.c`: maps rewritten NSOs, relocates ELF dynamic symbols, applies forced HLE takeovers | **Direct reuse** after replacing Mach-O section lookup with ELF linker sections and file loading |
| Code rewriting | `dm/tools/patch_nso.py`: rewrites `x18`, `tpidr_el0` / `tpidrro_el0`, SVCs and counter reads into host gates | **Reuse**, with one target change: Linux reads the pthread TSD through `tpidr_el0`, not `tpidrro_el0` |
| Kernel | `dm/host/hle_svc.c`, `hle_thread.c`, `thread.c`, `hle_os.c`: Horizon SVCs, threads, events, memory | **Reuse with a Linux memory/thread back end** |
| Services | `dm/host/hle_*.c`: fs, hid, audio, vi, nvdrv, account, friends, swkbd, etc. | **Reuse** the guest-visible service semantics; replace Apple sinks |
| Graphics | `dm/gfx/nvn`: NVN record layer + Metal replay; `dm/gfx/vk`: Vulkan-on-Metal; `dm/gfx/gl`: GL-on-Metal | **Replace only the back ends.** The NVN command record format, object layouts, texture layout math and shader toolchain stay |
| Shaders | Maxwell machine code → SPIR-V (Ryujinx translator) → SPIRV-Cross → MSL → metallib | The first half is the valuable part. On Asahi, **keep the SPIR-V** and feed Vulkan directly for Vulkan titles; build a Vulkan NVN shader ABI for NVN titles |
| Frontend | `front_mac.m` (AppKit/Metal), iOS app | **Replace** with SDL3 + native Vulkan |
| Audio | CoreAudio AudioUnit | **Replace** with PipeWire/ALSA |
| Input | GameController.framework, CoreHaptics, CoreMotion | **Replace** with SDL3 gamepad/keyboard and Linux evdev |
| Video | VideoToolbox | **Replace** with dav1d/libvpx (already vendored) where movies are required |
| Build | Xcode/clang + frameworks, `dm/host/Makefile` | **Replace** with CMake and a normal AArch64 Linux toolchain |

The key property to preserve is native execution: guest code runs at full speed on
the host CPU. There is no interpreter or JIT for ordinary game code. The rewriter
changes only instructions whose semantics Apple's kernel (now Linux's kernel)
does not provide directly.

## 2. Decisions

### 2.1 One host, native AArch64 Linux

Guest and host are both AArch64. The fork keeps direct execution. Linux memory
protection, signal delivery, TLS and executable memory are handled by a small
platform layer; no CPU translator is introduced.

The existing gate mechanism (`dm/host/entry.S`, `dm/host/thread.c`) is kept. It
runs SVC handlers on a per-thread gate stack, preserving the guest stack exactly
as Horizon does. ELF assembly labels and `.bss`/`.text` directives replace the
Mach-O ones.

The guest TLS rewrite is adjusted for Linux:

- macOS: patched guest reads use `mrs tpidrro_el0` because `x18` is reserved.
- Linux: the host TSD base is at `tpidr_el0`; patched reads use `mrs tpidr_el0`.
  `x18` is a normal register on Linux, but the existing per-thread slot is kept:
  it is already proven, and it keeps the rewriter's behaviour uniform.

The rewriter therefore grows a `--target linux` mode. The runtime key numbers are
still written into the NSO key page by the loader, so no key value is baked into
the binary.

### 2.2 The host page size is not the guest page size

Asahi uses 16 KiB host pages. Horizon exposes 4 KiB guest pages. The fork has one
memory module (`src/memory/`) that owns the distinction:

- `guest_page_size` is always 4096, `host_page_size` is queried with
  `sysconf(_SC_PAGESIZE)` and validated at startup.
- Guest mappings are reserved at host-page granularity with `mmap(PROT_NONE)`.
- A table records the state of every 4 KiB guest page: unmapped, read, write,
  execute, shared, dirty.
- Host `mprotect` calls are derived from the guest table, one host page at a
  time. A host page whose guest pages disagree gets the union of their
  permissions. Hardware cannot enforce sub-host-page protection, so mixed pages
  are reported; code and data are separated by the loader at 16 KiB boundaries,
  which makes mixed W^X pages avoidable in practice.
- Dirty tracking protects host pages. A write fault marks all guest pages in the
  host page dirty. This over-approximates, never under-approximates: a texture
  upload may copy more than necessary, but never misses a write.
- Aliases and GPU-visible memory are explicit objects. `memfd_create` plus
  `mmap(MAP_SHARED)` at several addresses replaces `vm_remap`.

The loader already aligns module segments to 16 KiB (`0x3FFF` rounding), so the
normal NSO layout does not mix code and writable data in one host page. The
memory tests exercise the remaining mixed cases and fail loudly rather than
silently dropping a permission.

### 2.3 Vulkan, not Metal

Two independent graphics paths are built on one Vulkan device layer:

1. **Vulkan titles** (Hades, Super Mario 3D All-Stars / 64 / Sunshine, Super Mario
   Galaxy 1 and 2). These games already ship a Vulkan loader and SPIR-V shaders.
   On macOS the host had to implement Vulkan on Metal. On Linux this becomes a
   thin, real-Vulkan interception layer: the game's objects, memory, command
   buffers, semaphores, fences and pipelines are real Vulkan objects. Only
   `VK_NN_vi_surface`, surface capabilities, swapchain creation and presentation
   need translation. The shader step disappears entirely; the game's SPIR-V goes
   straight to Honeykrisp.
2. **NVN titles** (the majority). The existing record/replay split stays: NVN API
   calls are recorded into the game's command memory, and a new C backend replays
   the records as Vulkan commands. The first backend is scoped to cover the core
   path (memory pools, textures, samplers, render targets, depth/stencil, shaders,
   graphics/compute pipelines, draws, dispatches, clears, copies, fences,
   events, presentation). Advanced NVN features (sparse/virtual pools, DLSS,
   transform feedback, NVN2 mesh/task stages, multi-draw optimizations)
   are staged and reported honestly.

The common device layer owns instance/device selection, queue families, memory
type selection, a versioned pipeline cache, swapchain management, and capability
reporting. It queries everything at runtime; no GPU name or extension is assumed.

### 2.4 Shaders

For Vulkan titles: no translation. The game's SPIR-V is used as-is. The host
validates the module and logs any feature the driver rejects.

For NVN titles: the existing offline chain already produces the intermediate
SPIR-V before SPIRV-Cross, and `dmsh_build.py` now writes it as `<key>_b.spv`
beside the Metal library and the reflection JSON. The NVN replay loads both,
builds descriptor sets from the reflection (one disjoint binding range per
stage) and fills the driver's `c0`/`c1`/`c2` tables as the Metal backend does.
The translator, NSP extractor and SPIRV-Cross are native AArch64 Linux binaries
built from the in-repo sources and used by the Asahi host.

### 2.5 Frontend, audio, input

- SDL3 (runtime library; a small ABI shim avoids a development-header
  dependency) provides Wayland/X11 windows, Vulkan surfaces, keyboard, mouse,
  gamepads, hotplug and audio device selection.
- Audio output uses PipeWire through SDL3's audio API, with ALSA as the fallback.
  The Horizon audio services keep their existing guest-visible queue and timing
  semantics; only the sink changes.
- Gamepads are mapped to Switch semantics by the existing `hle_hid.c` layer. SDL3
  supplies standard-layout controllers; raw evdev is the fallback for devices SDL
  does not recognise.
- The launcher is a native desktop application: library scan, cover display,
  launch, pause/stop, settings, shader cache management, logs. A CLI
  (`nxnative-asahi run <game-dir>`) remains available for development.

### 2.6 Build and packaging

CMake is used because the project needs generated sources (profile headers,
Vulkan entry stubs, shader reflection) and sanitizer/validation variants. The
old Makefiles stay as historical reference and are not part of the Asahi build.

Targets:

- `nxnative_core` — loader, HLE, memory, threads, services.
- `nxnative_gfx` — Vulkan device, NVN replay, Vulkan API interception.
- `nxnative_platform` — Linux/SDL3 window, input, audio, paths, diagnostics.
- `nxnative-asahi` — the application.
- `tests/` — page-size, memory alias, Vulkan device, offscreen render, NVN
  record, and end-to-end synthetic workloads.

Dependencies are system libraries only: Vulkan loader/headers, libdl, pthread,
SDL3 (runtime), PipeWire/ALSA (runtime), dav1d/libvpx (vendored for movie
decode). No Apple SDK, no Objective-C runtime, no Xcode.

## 3. Component status and order of work

| Component | State at fork | Asahi plan |
|---|---|---|
| Guest loader / relocations | Proven | Port Mach-O section iteration to ELF; keep file-based `map_image` |
| SVCs / threads / events | Proven | Port Mach APIs to Linux (`process_vm_readv`, `/proc`, `ptrace`-free dumps) |
| HLE services | Proven | Keep semantics; replace Apple sinks one by one |
| NVN interface/record layer | Proven | Keep; compile against the Vulkan replay interface |
| NVN replay | Metal, ~1.7 MB | Rewrite as `nvn_vk.c`, core first |
| Vulkan-on-Metal | Proven, 154 KB | Replace with real-Vulkan interception; shader translation removed |
| GL-on-Metal | Proven | Native EGL/GL later; lower priority than Vulkan |
| Shader chain | Proven | Add Vulkan output; keep MSL path only as reference |
| Frontend | AppKit/iOS | SDL3 native desktop |
| Audio | CoreAudio | PipeWire/ALSA sink |
| Input | GameController | SDL3 + evdev |
| Video | VideoToolbox | dav1d/libvpx; movie services staged |

### Roadmap

1. **Foundation — done.** CMake, Linux platform, 16 KiB memory module, system
   report, headless Vulkan device, offscreen GPU test.
2. **Vulkan titles — done (headless).** Real-Vulkan interception, SDL3 surface/
   swapchain, synthetic Vulkan client test; real-title smoke runs still open.
3. **NVN core — done (synthetic).** Pools, textures, samplers, programs, render
   targets, vertex/fragment draws, compute, geometry, tessellation, clear/readback
   and events; NVN2 mesh/task and the advanced resource kinds remain.
4. **Host integration — done (synthetic).** Loader, TSD, SVC thread/service
   port, synthetic guest execution; first homebrew run still open.
5. **Audio/input/frontend — done.** SDL3 audio and gamepad, launcher, settings,
   saves.
6. **Stability and performance — open.** Pipeline-cache warming, frame pacing,
   profiling, long-running tests, packaging for Fedora Asahi Remix.

## 4. Risks and limits

- **NVN replay is the long pole.** The Metal backend encodes years of
  game-specific behaviour. The Vulkan backend starts with the core and will not
  be feature-equivalent on day one. The fork must not claim otherwise.
- **16 KiB pages make 4 KiB permissions approximate.** Writes are tracked
  conservatively; mixed-permission host pages are reported. Code/data separation
  at module level keeps the common case exact.
- **The Asahi Vulkan driver is the compatibility target.** Capabilities are
  queried, never assumed. A missing feature produces an actionable report, not a
  silent fallback.
- **No proprietary material is shipped.** Users supply their own games and keys.
  Development uses homebrew and synthetic data.
- **Testing needs real hardware.** The build machine in this environment is an
  M2 Pro running Fedora Asahi Remix with Mesa Honeykrisp 26.2.6 and Vulkan
  1.4.354; that is the reference configuration.
