# Graphics architecture on Asahi Linux

## One real Vulkan device

`src/graphics/vulkan/` owns the instance, physical-device selection, logical
device, queues, command pools, memory-type selection, capability query and
pipeline cache. Nothing assumes a device name, a Vulkan version or an extension.

At startup the layer queries:

- API version, GPU name, vendor/device IDs, driver name/version/info;
- extensions;
- core features;
- queue families and flags;
- memory heaps and types;
- format features for the formats the emulator needs;
- limits and compute capabilities.

`nxnative-asahi vk-info` prints the matrix. A system with no accelerated Vulkan
device is reported as unsupported; llvmpipe is only available with
`NX_VK_ALLOW_CPU=1` for diagnostics.

## Path A: guest titles that ship Vulkan

Hades, Super Mario 3D All-Stars (64/Sunshine/Galaxy) and the Galaxy sequels
already issue standard Vulkan calls and ship SPIR-V. On macOS nxnative had to
implement Vulkan on Metal. On Linux the guest-facing Vulkan API is a **real
Vulkan interception layer**:

- `src/graphics/vkapi/vk_api.c` implements the handful of entry points that need
  translation: instance/device creation, physical-device properties/features,
  `VK_NN_vi_surface`, surface capabilities, swapchain creation, presentation,
  shader/pipeline diagnostics and the persistent pipeline cache.
- `tools/gen_vk_entries.py` generates the remaining ~130 entry points as
  AArch64 assembly forwarding stubs. Each stub loads the real Mesa function
  pointer from a slot and branches to it, preserving every argument and return
  register and the stack layout exactly.
- The guest's `vk*` imports are bound to this table by `dm/host/hle.c`; the
  guest's Vulkan loader and NVIDIA driver modules are not loaded.
- The guest's `VkInstance`, `VkPhysicalDevice`, `VkDevice`, `VkQueue`,
  `VkBuffer`, `VkImage`, `VkCommandBuffer`, descriptors, semaphores and fences
  are the real Mesa handles. Memory is real Mesa memory; Honeykrisp exposes both
  memory types as host-visible, so mapping works without shadow allocations.
- `vkCreateViSurfaceNN` creates a real Wayland/X11 Vulkan surface from the SDL3
  window. The game sees the Switch surface contract; the driver sees a normal
  Linux surface.
- Surface capabilities add `VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR` (Super Mario
  Galaxy requires it) and `vkCreateSwapchainKHR` maps `INHERIT` to `IDENTITY`.
- The game's swapchain extent resizes the window when possible; the real FIFO
  present mode provides pacing.
- The guest's NVIDIA binary pipeline cache is ignored; a real, versioned
  `VkPipelineCache` is persisted under `$XDG_CACHE_HOME/nxnative-asahi/` and
  keyed by the device UUID and driver version.

**Shaders:** the guest's SPIR-V is passed directly to
`vkCreateShaderModule`. There is no MSL, metallib, SPIRV-Cross or Metal step for
these titles. The original `vk_shaders.py` fixups existed for Metal; the native
Vulkan path does not need them.

## Path B: NVN titles

The NVN interface layer (`dm/gfx/nvn/hle_nvn.c`) records NVN calls into the
game's command memory, preserving NVN's object layouts, compact-record
expansion and texture layout/deswizzle math. The Asahi replay
(`src/graphics/nvn/nvn_vk.c` and `nvn_vk_draw.inc`) turns those records into
Vulkan work:

- device, queues, memory pools, command buffers, submission, fences and events;
- render targets (colour and depth), clears and barriers;
- buffer→texture, texture→texture and texture→buffer copies;
- buffer textures (`samplerBuffer`, `UNIFORM_TEXEL_BUFFER`);
- 2D array, cube and 3D textures with every layer/slice of every level uploaded;
- multisample render targets and resolve through `vkCmdResolveImage`;
- textures, samplers, texture/sampler ID pools, views and CPU/GPU ownership;
- guest-memory upload (linear rows or a CPU deswizzle) and GPU readback;
- SPIR-V shader modules from `<key>_b.spv` and the reflection JSON
  `<key>_b.json`, with an automatic descriptor set layout;
- vertex/fragment pipelines: vertex attributes/streams/buffers, cull, winding,
  blend, channel masks, depth test/write/compare, dynamic viewport/scissor;
- the NVN alpha test, injected into the fragment SPIR-V as a discard before each
  return, with the reference and comparison mask as specialization constants
  (the pipeline cache key includes both), Vulkan logic-op state, which takes
  the place of blending, and the fixed-function Porter-Duff advanced blend
  modes (multiply/screen/overlay/HSL fall back with a red line);
- descriptor binding for uniforms, storage buffers, textures, samplers and
  storage images, using the semantic NVN slots;
- indexed, instanced and base-vertex draws;
- the NVN near=-w depth rewrite (`gl_Position.z = z/2 + w/2`) injected into
  the final vertex-processing stage at its return, or around each `EmitVertex`
  in a geometry stage, with near=0 using the base SPIR-V;
- compute pipelines and dispatches with SSBO/image/UBO bindings, including
  storage-buffer copy-back to guest memory;
- geometry shaders from the `<key>_bg<n>.spv` variant matching the draw
  topology (points, lines, adjacency or triangles), and tessellation
  (hull/domain stages, patch-list draws);
- presentation through the SDL3 window's Vulkan swapchain.

**Shader ABI.** All resources are in descriptor set 0. Each translated shader
numbers its bindings from 1; the replay moves vertex-stage bindings to
`binding + 0`, fragment to `binding + 1000`, geometry to `+2000`, hull to
`+3000`, domain to `+4000`, compute to `+5000`, so the stages never collide in the linked pipeline. The JSON names each
binding's NVN kind and slot (`cbuf`, `c1`, `drvcb`, `tex`, `samp`, `img`,
`sbuf`, ...). The driver's `c0`/`c1`/`c2` tables are filled from the current NVN
bindings and `dmsh_abi.h`.

`dm/gfx/tools/dmsh_build.py` writes `<key>_b.spv` and the reflection JSON;
geometry shaders write their topology variants as `<key>_bg0.spv` ...
`<key>_bg4.spv` (the replay selects one by draw topology). The runtime reads a
directory named `b-<chain version>`; set `NX_NVN_SHADER_CACHE` or put it in
`<game-folder>/shaders`. The translator, extractor and SPIRV-Cross are native
AArch64 Linux binaries; `scripts/build_asahi_from_nsp.py` builds a game's cache
on the machine.

The replay is fail-closed. Unsupported descriptor kinds, bindless pools,
descriptor arrays and unknown reflection kinds log a `[RED]` line and drop that
draw/dispatch rather than drawing a wrong picture.

Sparse/virtual pools, DLSS, transform feedback, multi-draw optimizations and the
Metal pass-merging heuristics are later work.

## Path C: OpenGL titles

The macOS GL layer (`dm/gfx/gl/hle_gl.m`) translates NVIDIA desktop GL to Metal.
On Linux the equivalent is a native EGL/OpenGL context over Mesa's Asahi GL
driver. This is lower priority than Vulkan and is not in the current build.

## Presentation and pacing

- Vulkan titles use the real swapchain's FIFO present. `vkAcquireNextImageKHR`
  and `vkQueuePresentKHR` are the real Mesa calls.
- The front end presents through SDL3's Vulkan surface.
- The NVN path uses the same device and a Vulkan swapchain created from the
  window; the final display texture is blitted with letterboxing. Its pacing
  hooks are present but the replay does not yet merge passes or pace ahead of
  the FIFO present.
- `dm_guest_frame_tick` is called on presentation when the host graphics layer
  registers the hook.

## Diagnostics

- `NX_VK_VALIDATION=1` enables the Khronos validation layer when installed.
- `nxnative-asahi vk-info` prints the capability matrix.
- Shader module failures log the SPIR-V size and driver result.
- Pipeline creation failures log the failing pipeline count.
- The front end logs the selected window backend (`wayland`, `x11`, `dummy`).
- Device-lost handling and swapchain recreation are reported, not hidden.

## What is not implemented

- NVN2 mesh/task stages.
- Buffer textures, bindless pools and transform feedback.
- The Proton/guest OpenGL path.
- GPU timing/overlay integration.
- Explicit memory aliasing for NVN pools (the design is in progress).
- A full Asahi-specific performance profile; the code queries capabilities but
  has not been tuned on M1/M2 beyond the reference device.
