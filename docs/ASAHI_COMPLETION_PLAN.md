# Asahi completion plan

This audit records the checked-in implementation on `main` and the baseline
run in the current workspace. The repository's Linux status and compatibility
documents are the source for prior claims; this audit marks a capability
verified only when the checked-in test or a current local run exercises it.

## Baseline

- Branch: `main`. The worktree was already dirty at first inspection. Existing
  tracked edits included `docs/ASAHI_STATUS.md`, `docs/BUILDING.md`,
  `docs/COMPATIBILITY.md`, `scripts/build_asahi_from_nsp.py`,
  `src/graphics/nvn/nvn_vk_draw.inc` and `tests/t_shader_cache.c`; untracked
  files included `dm/tools/tests/test_asahi_import.py`. These were preserved.
- Host: AArch64 Fedora Asahi, Linux 7.2.12, 16 KiB pages; Apple M2 Pro
  (G14S B1), Honeykrisp Mesa 26.2.6, Vulkan 1.4.354. `vk-info` reports one
  queue and a 7.6 GiB device-local/host-visible heap. The desktop session is
  not available to this process.
- `GAME=010015100B514000 scripts/build_asahi.sh` could not reuse the existing
  ignored `build-asahi` directory because its CMake cache points to a different
  checkout. Reconfiguring and building in `/tmp/nxasahi-audit-build` avoided
  touching that cache; the build succeeded and all 38 CTest tests passed. The
  fixed descriptor-array draw was added afterward and its `nvn_texture` test
  also passed on the local Vulkan device.
- `python3 -m pytest dm/tools/tests -q`: 1,412 passed, 57 skipped, 1,232
  subtests passed. Seven upstream/deprecation warnings were reported.
- Follow-up: runtime Vulkan capture now records descriptor layout bindings,
  layout flags, immutable-sampler presence, push-constant ranges and a stable
  pipeline-layout reference per stage. `vkapi_smoke` passed with Khronos
  validation enabled; offline validation/package of the emitted capture passed.
  Full current run: CTest 38/38; Python tools 1,416 passed, 57 skipped, 1,232
  subtests; docs 6/6; `git diff --check` clean.
- Follow-up: NVN direct texture/sampler arrays now use reflected-size descriptor
  scratch buffers and per-binding Vulkan descriptor-limit checks. The
  `nvn_texture` Honeykrisp test dynamically samples element 16 of a 17-element
  array; all 38 CTests passed with Khronos validation enabled, and the full
  Python/docs suites passed again.
- Follow-up: indirect compute dispatch now reads the guest's three group counts
  at replay time and uses them for `vkCmdDispatch`, after prior synchronous work
  completes. `nvn_compute` verifies one GPU dispatch writes the argument record
  consumed by the next dispatch; full validation-enabled CTest and Python/docs
  runs passed.
- Follow-up: direct fixed storage-image arrays now resolve every NVN image slot
  and track GPU ownership through compute and graphics submissions. Image layout
  transitions now cover every mip and array layer, matching the texture-wide
  layout tracker. The compute test writes a dynamically indexed pair of
  mipmapped 2D array images at layer 1, reads those writes in a second compute
  dispatch and verifies SSBO results; the draw test also writes and reads a
  fragment-stage storage image. Validation passes; graphics storage-image
  arrays, compute-to-graphics visibility and real-game coverage remain.
- Follow-up: replay no longer drops `OP_BARRIER`. All defined NVN primitive,
  fragment, tiled-fragment, indirect-data and invalidate bits produce a
  conservative Vulkan all-commands memory dependency; unknown bits are reported
  and still receive the conservative dependency. The compute-image test places
  all defined bits between its storage-image write and read dispatches and
  verifies both output visibility and one recorded barrier. Full validation
  suite passed on M2 Pro Honeykrisp.
- Follow-up: buffer↔texture copies now honor selected view mip/layer ranges,
  3D z offsets/depth, and recorded row/image strides; partial buffer uploads
  first preserve untouched texels. Exact same-format texture↔texture copy and
  resolve paths use the selected source/destination subresources and preserve
  untouched destination data. Vulkan blits now handle scaled regions and
  X/Y/3D-Z mirroring when the format advertises the required blit features;
  array-layer Z mirroring is rejected. `nvn_copy` verifies padded two-layer
  copies, mip-1/layer-1 views, a 3D slice range, nearest 2× scaling and X
  mirroring on M2 Pro Honeykrisp. Compatible packed-float color conversion is
  implemented through Vulkan blit and verified from R11G11B10F to RGBA16F.
  Compressed/depth formats, linear-filter and Y/Z mirror coverage, self-overlap
  and real-game copy traces remain open.
- Neither a retail game nor a homebrew game image was available to the audit.
  Synthetic guest and renderer success does not establish game compatibility.

## Gap matrix

### Completed and verified

| Area | Relevant source | Current behavior | Required behavior / root cause | Next work | Completion test |
|---|---|---|---|---|---|
| 4 KiB guest pages on 16 KiB Linux pages | `src/memory/nx_mem.c`, `dm/host/hle_svc.c`, `tests/t_memory_16k.c` | Guest permissions, dirty tracking, W^X, memfd aliases and generated AArch64 code are covered; local `memory_16k` passed. | Preserve sub-page semantics on Asahi's larger host page. | Keep as a regression gate; real game allocation patterns remain unknown. | Existing CTest plus real game memory trace. |
| Vulkan device and basic rendering | `src/graphics/vulkan/nx_vk.c`, `tests/t_vulkan_smoke.c` | Queries the installed device and renders/readbacks an offscreen triangle; local `vulkan_smoke` passed. | Use real Honeykrisp capabilities and keep unsupported features visible. | Validate the startup report and desktop surface on the user's actual session. | CTest plus a real SDL desktop presentation run. |
| Vulkan pipeline cache persistence | `src/graphics/vulkan/nx_vk.c`, `src/graphics/nvn/nvn_vk.c`, `src/graphics/nvn/nvn_vk_draw.inc`, `tests/t_vulkan_smoke.c` | Runtime graphics/compute pipelines use a persisted Vulkan cache; loader checks header size/version, vendor/device IDs and the full pipeline-cache UUID before loading. Smoke test covers stale UUID, malformed/foreign headers, truncated-file recovery, and save/reload of driver-produced cache data; validated local test passed. | This cache is not a full pipeline-state precompile workflow, and device/driver behavior still needs a real Honeykrisp game session. | Add progress/resume-aware precompilation from captured pipeline state, then measure first-run and warm-run shader stalls on a game. | Cache roundtrip and malformed-file smoke test plus real-game cold/warm frame-time comparison. |
| NVN compute, barriers and storage-image writes | `src/graphics/nvn/nvn_vk.c`, `src/graphics/nvn/nvn_vk_draw.inc`, `tests/t_nvn_compute.c`, `tests/t_nvn_compute_image.c`, `tests/t_nvn_draw.c` | Direct and host-assisted indirect group-count dispatch, SSBO copyback, scalar and fixed-array `imageStore` pass synthetic tests with validation; explicit `OP_BARRIER` maps all defined bits to a Vulkan memory dependency, and a second compute dispatch reads prior image writes. Graphics scalar storage-image writes are read back after draw completion; image layout barriers cover every mip and layer, and sampled/storage images receive same-layout shader dependencies. | Maintain NVN ordering/visibility for real mixed graphics and compute command streams; device writes that do not participate in current copyback paths still need verification. | Add graphics storage-image array and compute-to-graphics visibility tests plus atomic coverage; investigate GPU-native indirect dispatch to avoid host synchronization; obtain a real game workload. | Synthetic direct/indirect dispatch, mipmapped compute array write/read with explicit NVN barrier, graphics storage-image readback with validation, then real game. |
| NVN copies, texel buffers and texture dimensions | `src/graphics/nvn/nvn_vk_draw.inc`, `dm/gfx/nvn/hle_nvn.c`, `tests/t_nvn_copy.c`, `tests/t_nvn_tbuf.c`, `tests/t_nvn_array.c`, `tests/t_nvn_cube.c`, `tests/t_nvn_3d.c` | Buffer↔texture copies honor view-selected mip/layer ranges, array and 3D depth regions, row/image strides, and preserve untouched texels; exact compatible texture copies and resolves use selected subresources. Vulkan blits implement scaled regions, compatible numeric color-format conversion and mirroring on supported axes/formats. Pixel tests cover padded layers, mip/layer views, 3D slices, nearest scaling, X mirror, and R11G11B10F→RGBA16F conversion with validation. | Cover compressed and depth/stencil formats, linear filtering, remaining mirror axes, same-image overlap/aliasing semantics and real command ordering. | Add GPU conversion paths where required; reject unsupported formats/flags explicitly; validate array-layer mirror-Z semantics and gather real game traces. | Pixel-exact copy/blit tests for formats, strides, views, aliasing and flags, then real-game image comparisons. |
| Memory aliasing | `src/memory/nx_mem.c`, `tests/t_memory_16k.c` | Alias visibility and dirty spans are tested locally. | Keep aliases coherent when GPU work and decommit/reuse overlap. | Extend to multi-alias teardown and in-flight GPU lifetime cases. | Alias race/reuse tests and real game map/unmap traces. |

### Implemented but incompletely tested

| Area | Relevant source | Current behavior | Required behavior / root cause | Proposed work | Test required |
|---|---|---|---|---|---|
| Game import and package assembly | `scripts/build_asahi_from_nsp.py`, `scripts/build_ipa_from_nsp.py`, `dm/games/profile_guess.py`, `dm/gfx/tools/vk_capture.py`, `dm/tools/tests/test_asahi_import.py` | NSP-folder, NUP and dump preparation paths exist. NVN uses the translated shader cache; detected direct-Vulkan games with packaged or runtime-generated SPIR-V use the guest Vulkan path. `NX_VK_SHADER_CAPTURE_DIR` captures modules, specialization values, descriptor layouts and push-constant ranges; import validates reflection and packages the capture inventory. No user game files were present. | Correct extraction, update/DLC selection, build identity, shader collection and standalone package for varied real inputs; pipeline-state precompilation and a real direct-Vulkan game run remain unverified. | Add full pipeline-state precompilation, then exercise direct Vulkan with a legally obtained title through import and launch. | Workflow-selection, deterministic module/stage/layout capture, offline reflection/package validation; then at least one legally obtained title through import/build/launch. |
| Guest Vulkan interception | `src/graphics/vkapi/vk_api.c`, `src/graphics/vulkan/nx_vk.c`, `tests/t_vkapi_smoke.c`, `tests/t_synth_vk_guest.c` | Synthetic guest creates real Vulkan instance/device and image operations; importer now accepts detected direct-Vulkan titles with packaged SPIR-V. No retail Vulkan game has been run. | Implement and advertise exactly the guest Vulkan surface required by a real Switch Vulkan title. | Trace the first Vulkan game and add missing entry points/semantics by observed use. | Real Vulkan title from launch through gameplay and clean shutdown. |
| Linux Horizon services, filesystem and saves | `dm/host/hle_svc.c`, `dm/host/hle_fs.c`, `dm/host/save_sync.c`, `src/horizon/` | Linux paths compile; save transactions and synthetic memory behavior have isolated tests. | Match guest lifecycle, FS and synchronization under actual SDK/game behavior. | Bring up a legal game, capture unhandled SVC/service results, implement root causes. | Service-specific synthetic tests plus repeated real-game save/reload. |
| Runtime shader cache compatibility | `dm/gfx/tools/dmsh_build.py`, `src/graphics/nvn/nvn_spv.c`, `src/graphics/nvn/nvn_vk_draw.inc`, `tests/t_shader_cache.c` | SPIR-V reflection verifies descriptor-array counts, entry points and metadata on synthetic shaders; parser rejects malformed instruction lengths and IDs outside the module bound. | Consume real shader metadata and bind resources exactly as their NVN code expects. | Add captured real-game fixtures without committing proprietary assets; record safe metadata-only regressions. | Validated real shader cache and deterministic rendered output. |
| Launcher and window presentation | `src/launcher/main.c`, `src/platform/linux/nx_window_sdl.c`, `src/graphics/vulkan/nx_vk.c` | Headless UI and offscreen/window tests passed; desktop session is not attached to this run. | User-visible library launch and swapchain behavior on Wayland/X11. | Run from a logged-in session and validate resize, focus, presentation and shutdown. | Manual desktop run plus automated surface tests. |
| c0/c2 driver-bank values | `src/graphics/nvn/nvn_vk_draw.inc`, `dm/gfx/tools/dmsh_abi.py`, `dm/gfx/nvn/dmsh_abi.h` | Replay fills c0 storage-buffer/compute state and c2 texture/image/sampler tables from current bindings. `NX_VK_DEBUG=1` now emits shader-keyed, stage/register diagnostics for nonzero bank values. No real-game capture has validated every bank offset or shader assumption on this Vulkan path. | Confirm translator ABI, stage selection, offsets and live guest values against shaders that read c0/c2 dynamically; diagnose stale or missing values from traces. | Capture representative shader bank reads and compare trace entries with the guest command state; add synthetic value tests for each supported bank/stage. | c0/c2 shader fixtures read the expected address/size/handle words on Honeykrisp, followed by a real game shader trace. |
| Audio and input | `dm/host/hle_audio_linux.c`, `dm/host/hle_hid.c`, `src/platform/linux/nx_audio_sdl.c`, `tests/t_audio_out.c` | PCM/AudioOut and keyboard/one gamepad code paths exist; synthetic audio tests passed. | Match game timing and support broader controller/lifecycle behavior. | Validate on a real title; then implement multi-controller, rumble and motion from observed API calls. | Real play session with audio/input and clean exit. |

### Partially implemented

| Area | Relevant source | Current behavior | Required behavior / root cause | Proposed work | Test required |
|---|---|---|---|---|---|
| NVN descriptor arrays and pool arrays | `src/graphics/nvn/nvn_spv.c`, `src/graphics/nvn/nvn_vk_draw.inc`, `dm/gfx/nvn/hle_nvn.c`, `src/graphics/vulkan/nx_vk.c`, `tests/t_shader_cache.c`, `tests/t_nvn_texture.c` | Reflection reports fixed counts and runtime arrays distinctly; layout preserves fixed counts, rejects runtime arrays before layout creation and caps bindings at 128. Replay maps direct texture/sampler arrays element-by-element to NVN slot `reg + element`, validates each binding against queried Vulkan descriptor limits and sizes write storage from reflected totals. A Honeykrisp draw samples element 0 and dynamically indexes element 16 of a 17-element texture array with validation enabled. Uniform-buffer/c2 arrays, pool arrays and runtime arrays still fail closed; descriptor-indexing runtime features are queried but not enabled. | Support remaining array kinds from their own index sources and enable runtime descriptor indexing only when the complete required feature set is available. | Keep runtime-array rejection until variable-count layouts and NVN pool mapping are implemented; add per-kind tests. | 17-element dynamic texture-array draw passes on Honeykrisp; add runtime-array refusal coverage and validate other array kinds plus a real game. |
| NVN primitive lowering | `src/graphics/nvn/nvn_vk_draw.inc`, `tests/t_nvn_draw.c`, `tests/t_nvn_indexed.c` | Quads, quad strips, polygons and line loops expand into triangle/line index lists. Expansion handles indexed and array input and splits indexed restart segments; synthetic Honeykrisp readbacks pass for all four modes, including an indexed quad separated by a restart marker, with validation enabled. | Verify winding, base-vertex behavior and degenerate segments against NVN semantics. | Add base-vertex and varied index-width tests. | Reference readback tests for all four modes, indexed and non-indexed draws, restart, base vertex and index widths. |
| NVN indirect draws | `src/graphics/nvn/nvn_vk_draw.inc`, `src/graphics/nvn/nvn_vk.c`, `tests/t_nvn_draw.c`, `tests/t_nvn_indexed.c` | Arrays and indexed indirect arguments are bounds-checked against mapped guest memory and CPU-expanded into existing direct draw calls. Indirect-count paths read the count after prior synchronous Vulkan work, skip records at or above count, and support both inline batches and large `OP_MULTI_DRAW`; synthetic array/indexed readbacks pass on Honeykrisp with validation enabled. | Preserve semantics while removing CPU readback and per-draw queue-idle overhead; large batches remain CPU-expanded, and mesh/task indirect dispatch is not covered. | Add GPU-native indirect execution when capability and buffer-address translation allow; benchmark a captured GPU-driven game. | Synthetic tests for ordinary, indexed, count-limited and large batches; then test actual GPU-written count/argument records in a game workload. |
| NVN2 mesh/task shaders | `src/graphics/nvn/nvn_vk_draw.inc`, `dm/gfx/nvn/hle_nvn.c`, `dm/gfx/tools/` | Stage detection exists, but replay rejects mesh/task shaders. | Translate NVN2 workgroup payload/mesh output and dispatch semantics to available Vulkan mesh-shader functionality or a semantics-preserving fallback; host features are not yet mapped to that ABI. | Inspect actual NVN2 shader metadata and Honeykrisp mesh-shader capabilities before selecting an execution path. | Synthetic payload/topology tests on supported hardware plus a real NVN2 title. |
| Storage-image atomics, transform feedback and sparse pools | `src/graphics/nvn/nvn_vk_draw.inc`, `src/graphics/nvn/nvn_vk.c`, `src/graphics/vulkan/nx_vk.c` | Some image writes and buffers work, but these mechanisms are explicitly listed as absent. | Implement NVN-visible atomic, capture and residency semantics; current replay lacks those state/data paths. | Add only after a shader/command trace establishes required operations and device feature availability. | Per-operation synthetic readback tests and real workload validation. |
| Advanced blend modes | `src/graphics/nvn/nvn_vk_draw.inc`, `dm/gfx/nvn/hle_nvn.c` | Porter-Duff modes map to fixed-function Vulkan blending; multiply/screen/overlay/HSL and other shader-dependent modes fall back to base blend functions. | Match each NVN advanced equation and overlap/premultiplication semantics; fixed-function factors cannot express all modes. | Implement a tested shader-based path or supported Vulkan advanced-blend extension, selected by capability. | CPU reference image comparisons across overlap, masks and formats. |
| Game/version compatibility tiers | `scripts/build_asahi_from_nsp.py`, `dm/games/`, `docs/COMPATIBILITY.md` | Profiles and build-ID knowledge are extensive, but actual Linux retail gameplay is not established for this workspace. | Separate buildable/bootable/renderable/playable/verified evidence by build. | Record one game bring-up end-to-end before expanding status claims. | Import, ELF validation, boot, render, input/audio/save/progression/shutdown. |
| Multi-game runtime and packaging | `CMakeLists.txt`, `scripts/build_asahi_games.sh`, `src/launcher/main.c`, `packaging/` | Profile-specific binaries and local RPM build paths exist; one universal profile binary and released repository/package are not complete. | Installable application must resolve profile/runtime and XDG storage independently of checkout. | Test installed RPM in a clean user environment and quantify whether runtime-profile refactor is needed. | Install/remove/reinstall and launch prepared game outside source tree. |

### Not implemented

| Area | Relevant source | Current behavior | Required behavior / root cause | Proposed work | Test required |
|---|---|---|---|---|---|
| Movie decode, camera and applets | `dm/host/hle_movie*.c`, `dm/host/hle_swkbd.c`, `dm/host/hle_web.c`, `dm/host/hle_haptics.c` | Several guest surfaces report unavailable or are not linked to Linux equivalents; decode dependencies are staged only. | Provide Linux-native decode/input/UI behavior for titles that require these services. | Wait for an actual title/service trace, then implement a redistributable Linux path or report precise incompatibility. | Service-specific synthetic tests and a title that exercises the surface. |
| Full direct Switch Vulkan game support | `src/graphics/vkapi/`, `src/graphics/vulkan/`, `tests/t_synth_vk_guest.c` | Instance/device/memory/image/WSI interception has a synthetic client test; a commercial direct-Vulkan game has not been run. | Cover the guest Vulkan functions/extensions and capability shape used by a real title. | Build a call inventory from the first target title and implement missing translation points. | Real direct-Vulkan title through gameplay and validation. |
| Linux OpenGL title path | `src/graphics/`, `CMakeLists.txt`, `docs/GRAPHICS.md` | The graphics source tree contains NVN, Vulkan and guest Vulkan interception only. `docs/GRAPHICS.md` explicitly says the Linux EGL/Mesa OpenGL path is not in the build. | Provide a native EGL/OpenGL context and guest GL translation/interception with Asahi Mesa semantics. | Audit the upstream guest GL HLE surface and implement Linux context/resource/shader/presentation path as a separate graphics backend after NVN priorities. | Synthetic GL import/context/draw tests and one real OpenGL title. |
| Runtime shader capture and unsupported packed shader workflows | `src/graphics/vkapi/vk_api.c`, `scripts/build_asahi_from_nsp.py`, `dm/gfx/tools/vk_capture.py`, `dm/gfx/tools/` | Direct Vulkan SPIR-V modules are captured atomically by content hash; sidecars record full digest, size, title/main-build identity, pipeline stage, entry point and specialization values. The inventory tool verifies capture hashes, reflects descriptors/constants with pinned SPIRV-Cross, and Asahi import can package the verified inventory. Pipeline-state precompilation is not implemented. NVN GLSLC capture, OpenGL capture, Unreal/Oodle and legacy packed-shader decode remain unwired in the Asahi preparation workflow. | Capture and inventory every needed shader on Linux, associate actual pipeline layouts and runtime coverage, then decode supported packed sources without Apple host dependencies. | Add pipeline-layout/coverage capture and precompile reusable Vulkan pipeline inputs where semantics permit; wire NVN GLSLC capture separately; report unsupported proprietary formats precisely. | Synthetic deterministic module/stage capture and reflection tests pass; add pipeline-layout fixtures, supported-container fixtures and real game shader coverage. |

### Known hardware or driver limitations

| Area | Relevant source | Current behavior | Required behavior / root cause | Proposed work | Test required |
|---|---|---|---|---|---|
| Vulkan feature availability varies by GPU/driver | `src/graphics/vulkan/nx_vk.c`, `src/frontend/main.c` | Device/extension/format queries and `vk-info` report the current device; current local CTest used this host's device. | Select correct fallbacks and avoid claiming unsupported NVN operations. | Record the exact GPU/driver and per-feature path in test reports; query feature bits before enabling optional operations. | M1/M2 Honeykrisp matrix; validation with unsupported-feature cases. |
| Desktop presentation requires an accessible compositor/session | `src/platform/linux/nx_window_sdl.c`, `src/graphics/vulkan/nx_vk.c` | Offscreen/headless tests pass; this audit did not connect to a user desktop session. | Verify real Wayland/X11 swapchain lifecycle. | Run under the actual launcher session and test resize, focus loss and suspend/resume. | Desktop manual and automated surface coverage. |
| Later Apple GPUs and other AArch64 distributions | `src/graphics/vulkan/nx_vk.c`, `docs/BUILDING.md` | Capability-dependent; not proven by this M2-class run. | Depend on queried capabilities, kernel and driver support rather than CPU family alone. | Publish only measured device/driver combinations. | Actual hardware/driver runs per claimed target. |
| Vertex-stage storage writes on the reference Honeykrisp device | `src/graphics/vulkan/nx_vk.c`, `src/graphics/nvn/nvn_vk_draw.inc`, shader translation tools | The actual M2 Pro report has `vertexStores 0`; the graphics pipeline cannot legally enable the missing Vulkan vertex-stage storage feature. | Preserve shader storage side effects, invocation identity and visibility while using an execution path supported by the GPU. The hardware feature bit is absent; deleting or skipping the writes would change guest behavior. | Inspect representative translated shaders and resource dependencies first; design a general transformation/prepass only when ordering and outputs can be proven equivalent. No per-game disable switch. | Synthetic semantic checks for values/order/visibility, SPIR-V validation, then Wonder and another game on Honeykrisp. |

### Performance limitations

| Area | Relevant source | Current behavior | Required behavior / root cause | Proposed work | Test required |
|---|---|---|---|---|---|
| NVN submission/resource churn | `src/graphics/nvn/nvn_vk_draw.inc`, `src/graphics/nvn/nvn_vk.c` | Per-draw host buffers/descriptors and conservative submissions; status document records no NVN pipeline warm-up or descriptor reuse. | Reduce CPU allocation/driver overhead without changing guest ordering; cause is short-lived Vulkan objects and no workload data yet. | Profile after real-game bring-up, then cache/reuse only measured stable state and track resource lifetime against GPU completion. | Frame-time/CPU/GPU samples and regression image tests on Wonder-class workload. |
| CPU texture deswizzle/upload | `dm/gfx/nvn/hle_nvn.c`, `src/graphics/nvn/nvn_vk_draw.inc` | CPU-written textures can be deswizzled each frame. | Avoid repeated full-level work and upload stalls while preserving dirty/coherency behavior. | Add per-level dirty tracking and asynchronous preparation only after tracing actual writes. | Texture upload timing plus pixel-exact update/readback tests. |
| Shader prewarming | `dm/gfx/tools/dmsh_build.py`, `src/graphics/nvn/nvn_vk_draw.inc` | Offline SPIR-V build exists, but Vulkan pipeline warm-up is not done. | Avoid large first-use pipeline stalls and remain cancellable/resumable. | Measure pipeline creation, then add bounded profile cache warming tied to GPU/driver identity. | Cold/warm timing on a real shader collection. |

## Order of work

1. Harden descriptor reflection/layout handling, validate fixed direct NVN
   texture/sampler arrays, and test the new quad/loop primitive lowerings.
   Implement other array kinds from their own index sources; keep runtime arrays
   and bindless pools fail-closed until their indexing semantics are complete.
2. Bring up at least one legally obtained NVN game on Honeykrisp and turn its
   observed failures into general fixes and metadata-only regression tests.
3. Expand missing NVN2, storage-image atomic, transform-feedback and sparse
   behavior only where a real title requires it and device features permit it.
4. Verify Linux Vulkan title support, applets, media and controller behavior
   from actual guest traces.
5. Profile real workloads before optimizing resource churn, deswizzle/upload or
   pipeline prewarming.

This is a live matrix: promote an item only when its stated test has passed,
and keep synthetic test evidence distinct from real-game and Honeykrisp results.
