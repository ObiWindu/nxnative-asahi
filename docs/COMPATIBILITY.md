# Compatibility report

This is a status report for the Asahi Linux fork, not a list of supported
games. The original nxnative games table is not a claim about this fork.

## Tested environment

- Apple M2 Pro, Fedora Asahi Remix 44, kernel `7.2.12-402.asahi.fc44.aarch64+16k`
  (16 KiB host pages).
- Mesa 26.2.6 Honeykrisp, Vulkan 1.4.354.
- No game files were available in the development environment. The tests below
  use synthetic data and real GPU/audio workloads.

## Status vocabulary

- **Implemented** — the code path is present and exercised by a test.
- **Compiles, not functionally verified** — the subsystem builds but no test or
  game has exercised it end to end.
- **Incomplete** — a working subset exists, with clearly listed gaps.
- **Not implemented** — the path is absent; the guest receives an unimplemented
  import stub, which logs and returns 0, or a clear failure. It is never a
  silent success.

## Matrix

| Subsystem | Status | Notes |
|---|---|---|
| CMake/AArch64 Linux build | Implemented | Native ELF; no Apple SDK, Objective-C, Metal or Xcode in the Asahi targets |
| Game preparation | Implemented | `scripts/build_asahi_from_nsp.py`: NSP folders, Switch 2 `.nup` containers and dump folders; NUP/dump need no keys; builds shaders and the per-profile runtime |
| System/page-size detection | Implemented | `sysconf`, `/proc`, `/etc/os-release`, runtime Vulkan report |
| Guest memory model | Implemented | 4 KiB guest / 16 KiB host, permission grouping, memfd aliases, dirty spans, W^X |
| Guest loader / relocations | Implemented | Synthetic NSO: MOD0, dynamic section, symbols, `DT_INIT`, relocations |
| pthread TSD compatibility | Implemented | Runtime discovery of glibc's 16-byte TSD slots; the key page holds byte offsets |
| Native AArch64 execution | Implemented | Synthetic guest enters `nnMain` and exits 0 |
| Gates/SVCs | Incomplete | ELF gate path compiles; a real rewritten game has not been run |
| Horizon threads/events/sync | Compiles, not functionally verified | Portable Linux fallbacks; no game workload |
| Horizon memory SVCs | Compiles, not functionally verified | 16 KiB host handling; real game allocation patterns not tested |
| FS / transactional saves | Compiles, not functionally verified | `FICLONE`/`copy_file_range` + `renameat2(RENAME_EXCHANGE)`; launcher backup/delete tested only as commands |
| HID service | Incomplete | SDL3 keyboard and one gamepad fed into HID; multiple controllers, motion and rumble remain |
| Audio (`AudioOut`, `OutputStream`) | Implemented | SDL3/PipeWire/ALSA sink with timed silent fallback; open/start/stop/append/release/volume tested |
| Audio input | Not implemented | No microphone; opens fail cleanly |
| Video/movies | Not implemented | VideoToolbox/VideoDecoder paths are not linked; dav1d/libvpx staged |
| Web applet | Not implemented | No WebKit replacement; the applet reports unavailable |
| Software keyboard / controller applet | Not implemented | Return unavailable/failure |
| Cheats | Not implemented | Host cheat service is not linked |
| NVN interface layer | Implemented | 743 ELF entry points; 519 implemented; records preserved |
| NVN→Vulkan core replay | Implemented | Pools, textures, samplers, render targets, clears, copies, fences, events, submission |
| NVN shader path | Implemented (cache path) | Loads translated SPIR-V + reflection JSON from `b-<chain>`; translator, extractor and SPIRV-Cross are native AArch64 Linux binaries |
| NVN draw path | Implemented (vertex/fragment) | Vertex attributes/streams/buffers, UBO/SSBO/texture/sampler/image descriptors, bindless handles read from the game's uniform buffers (`tex_ubo`/`samp_ubo`/`img_ubo`/`tbuf_ubo`), state, indexed/instanced draws, depth/stencil state, alpha test, logic operations, near=-w and near=0 depth modes; tested to guest readback |
| NVN alpha test | Implemented | A discard test injected before each fragment return, with the alpha function and reference as pipeline specialization constants; tested with LESS/GREATER/ALWAYS/NEVER |
| NVN advanced blend modes | Implemented (Porter-Duff) | ZERO/SRC/DST/SRC_OVER/DST_OVER/SRC_IN/DST_IN/SRC_OUT/DST_OUT/SRC_ATOP/DST_ATOP/XOR/PLUS use fixed-function factors, with the premultiplied-source and overlap rules; multiply/screen/overlay/HSL and the other shader-dependent modes fall back to the state's blend functions with a red line |
| NVN logic operations | Implemented | Vulkan logic-op state, which takes the place of blending; tested with COPY/AND/OR/XOR/INVERT/COPY_INVERTED and with blending enabled |
| NVN compute | Implemented | Compute pipeline, SSBO/image/UBO bindings, imageStore, DispatchCompute, storage-buffer copy-back; tested synthetically |
| NVN geometry | Implemented | One `<key>_bg<n>.spv` per input topology, selected by the draw primitive (points/lines/adjacency/triangles; after tessellation the triangle variant is used); tested synthetically with a point and a triangle draw |
| NVN tessellation | Implemented | Hull/domain stages and patch-list draws; tested synthetically |
| NVN2 mesh/task stages | Not implemented | Rejected with a red line |
| NVN buffer textures | Implemented | `samplerBuffer`/`UNIFORM_TEXEL_BUFFER`, tested synthetically |
| NVN 2D array textures | Implemented | Every layer uploaded, `sampler2DArray` view, tested synthetically |
| NVN cubemaps | Implemented | Six faces uploaded, `samplerCube` view, tested synthetically |
| NVN 3D textures | Implemented | All slices of every level uploaded, `sampler3D` view, tested synthetically |
| NVN bindless pools | Not implemented | Rejected with a red line |
| Multisampled render targets | Implemented | 2×/4×/8×/16× targets, multisample render pass and `vkCmdResolveImage` downsample, tested synthetically |
| NVN presentation | Implemented, not exercised | Vulkan swapchain blit from the SDL3 window; headless path reports no surface |
| Vulkan guest API | Implemented | Real-Vulkan interception; synthetic client tested |
| Vulkan device/WSI | Implemented | Real instance/device/queues/memory, SDL3 surface, swapchain, FIFO present |
| Shader path for Vulkan titles | Implemented | Guest SPIR-V goes directly to Mesa |
| Pipeline cache | Implemented | Persistent, GPU UUID/driver-keyed `VkPipelineCache` |
| Window/frontend | Implemented | Launcher/library, settings, saves, SDL3 window, keyboard, one gamepad |
| Multi-game runtime | Incomplete | One binary per profile; launcher picks `nxnative-<title id>` or the generic runtime |
| Packaging | Incomplete | CMake install, desktop entry and Fedora spec draft; no release artifact |

## What has been functionally tested

1. **16 KiB page behavior** — 16384-byte host page, 4096-byte guest model,
   mixed-page reporting, PROT_NONE faults, memfd aliases, dirty marking and
   generated AArch64 code execution.
2. **Native Vulkan rendering** — offscreen 256×256 render pass with hand-written
   SPIR-V, real pipeline, draw, copy and readback.
3. **Guest-facing Vulkan interception** — instance, `VK_NN_vi_surface`, a
   Tegra-shaped physical device, real device/memory/image/command buffer/clear/
   readback through the generated AArch64 stubs; a synthetic guest imports
   `vkCreateInstance` and creates a real Mesa instance.
4. **Guest execution** — synthetic NSO loaded, relocated, `DT_INIT` run, host
   bootstrap calls `nnMain`, process exits 0.
5. **NVN shader cache** — SPIR-V modules compiled at test time in the exact
   `<key>_b.spv` + `<key>_b.json` shape, loaded and reflected (UBO and combined
   image sampler types, `c1`, vertex inputs).
6. **NVN clear and draw** — NVN device/queue/program/command memory, clear and a
   full vertex/fragment draw through a real Vulkan pipeline; guest texture
   readback matches the clear and the drawn triangle.
7. **NVN texture and UBO binding** — texture pool and sampler pool, registered
   texture, fragment-stage uniform buffer, combined texture handle; the readback
   matches `texture * 0.5 + tint * 0.5`.
8. **NVN compute** — a compute program from the shader cache, an SSBO bound
   through the NVN slot, a 16×1×1 dispatch; all 64 words copied back to guest
   memory with the expected values.
9. **AudioOut** — four appended PCM blocks returned through the release queue
   in 45 ms on the real SDL3 sink.

## What is explicitly not verified

- No retail or homebrew Switch game has been run in this environment.
- The NVN replay has not seen a real game's shader cache, command stream or
  texture formats.
- Bindless pools, transform feedback and sparse textures are absent.
- Presentation through a real desktop surface was not exercised; the development
  shell cannot access the user's graphical session.
- Movie decode, rumble, motion, multi-controller input, software keyboard,
  web/controller applets and cheats are not complete.
- The Asahi kernel/Mesa combination is the only tested one. M3/M4 and other
  distributions are capability-dependent and untested.

## Hardware/driver gating

The Vulkan layer queries the real device and never assumes a GPU name. The
capability report (`nxnative-asahi vk-info`) lists the API version, GPU, driver,
extensions, memory types and limits. A system with no accelerated Vulkan device
is reported as unsupported rather than silently falling back to software.

## Known limitations

- Host pages are 16 KiB; 4 KiB guest permissions inside one host page are
  grouped and mixed pages are reported.
- The runtime is compiled for one game profile (`NXNATIVE_GAME`). The launcher
  selects among per-profile runtime binaries; one binary that loads any profile
  at run time is not done.
- The shader build tools are native AArch64 Linux binaries built from the
  in-repo sources; they have not been exercised on a full retail shader package
  in this environment.
- The NVN replay allocates host buffers per draw and deswizzles CPU-written
  textures per frame; it is correct, not fast.
- The Vulkan interception layer returns a Switch-shaped property set while the
  real Honeykrisp device is more capable. This is deliberate for guest engine
  sizing; unsupported real features are not advertised.
