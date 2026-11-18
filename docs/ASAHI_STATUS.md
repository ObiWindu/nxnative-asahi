# Asahi port status and engineering report

This is the current, tested state of the native Asahi Linux/AArch64 port. Every
claim below names the test or the code path that proves it. Nothing here is
"complete emulator" language: no retail or homebrew game was available in the
development environment, so the first real game bring-up is still open.

## Working now

### Platform and execution

- Native CMake/Ninja build for AArch64 Linux; no Apple SDK, Objective-C, Metal
  or Xcode dependency. The shader translator, NSP extractor and SPIRV-Cross are
  native AArch64 Linux binaries built from the in-repo sources.
- Native execution of the game's own AArch64 code: ELF module loading,
  relocations, forced HLE take-over through the `swforce`/`swhle` ELF tables,
  native AArch64 `nnMain` entry and clean return.
- Guest TLS discovery for glibc: the loader finds the 16-byte TSD slots at run
  time and writes byte offsets into the key page.
- 16 KiB host pages with 4 KiB guest pages: permission grouping, dirty spans,
  memfd aliases, W^X and generated-code execution.
- Linux save transactions using `FICLONE`/`copy_file_range` and
  `renameat2(RENAME_EXCHANGE)`, with a documented fallback.
- SDL3 window (Wayland or X11), keyboard and gamepad input into the HID service.
- SDL3/PipeWire/ALSA PCM output for `nn::audio::AudioOut`, with a timed silent
  fallback that keeps the game's buffer pacing when no device is available.
- The full `AudioOut` and SDK 21 `OutputStream` HLE surface (open/start/stop/
  append/release/volume/statistics, virtual output device, input stubs).
- Game preparation from an NSP folder, a Switch 2 `.nup` container or a dump
  folder: extraction, module rewrite, shader split, SPIR-V build, per-profile
  runtime, and a launcher-ready game folder. Detected direct-Vulkan games with
  packaged SPIR-V skip the NVN shader chain and pass their modules to the guest
  Vulkan path. Runtime-generated Vulkan SPIR-V can be captured on Linux and
  validated into a descriptor, specialization and pipeline-layout inventory
  for the game package. NVN titles that compile GLSL through GLSLC can capture
  outputs on Linux in a first `--skip-shaders` run, then validate and consume
  those files on re-import.
  Pipeline-state precompilation from those captures is not implemented. NUP and
  dump builds need no keys.
- Graphical launcher: game library scan, keyboard navigation, per-game settings
  (frame rate, render scale, mute), local save backup/restore/delete, and launch with
  the prepared game folder and environment.

### Graphics

- Real Vulkan through the installed Mesa driver (Honeykrisp on the reference
  machine); capabilities are queried, not hardcoded.
- Guest-facing Vulkan interception for Vulkan titles (instance, device, memory,
  images, swapchain, `VK_NN_vi_surface`, properties/features, pipeline cache).
- NVN interface layer compiled as Linux code: 743 entry points with ELF thunks,
  519 implemented (509 semantic + 10 empty), the rest reported as unimplemented.
- NVN recording/replay split preserved: the game's command memory is recorded in
  NVN's format and replayed by the host.
- NVN→Vulkan replay backend:
  - devices, queues, memory pools, command buffers, submission, fences and
    events;
  - render targets (colour and depth), clears (including NVN's colour-channel
    and stencil write masks and scissor rectangles) and barriers, including
    buffer→texture, texture→texture and texture→buffer copies, including
    block-compressed textures; CPU texture reads (3D slices, array layers,
    blocks and resolved multisampled colour targets);
  - `nvnCommandBufferDrawTexture`: a sampled source texel region into a
    destination pixel rectangle of each colour target, with the scissor and the
    channel masks;
  - NVN textures and samplers, texture-ID and sampler-ID pools, views
    (including channel swizzles), CPU and
    GPU ownership tracking, guest-memory upload and GPU readback;
  - shader cache loading of the translated SPIR-V modules (`<key>_b.spv`)
    with the replay reflection JSON (`<key>_b.json`), automatic descriptor set
    layout reflection, one disjoint binding range per stage;
  - graphics pipelines with NVN vertex attributes/streams/buffers, cull/front
    face, blend factors/equations and the fixed-function Porter-Duff advanced
    modes, alpha test, logic operations, channel masks, depth test/write/compare,
    viewport/scissor, indexed and instanced draws, geometry shaders (one
    `<key>_bg<n>` SPIR-V variant per input topology), tessellation
    (hull/domain stages) and NVN's
    near=-w depth convention (an injected `gl_Position.z` rewrite, before each
    `EmitVertex` in a geometry stage);
  - texture/sampler/UBO/SSBO/storage-image descriptor binding, semantic NVN
    slots, combined texture handles and bindless handles read from the game's
    own uniform buffers (`tex_ubo`, `samp_ubo`, `img_ubo`, `tbuf_ubo`);
  - direct and indirect compute dispatches from translated SPIR-V with
    SSBO/image/UBO bindings, `imageStore` and storage-buffer copy-back to guest
    memory; indirect counts are bounds-checked against device limits;
  - presentation through a Vulkan swapchain created from the SDL3 window
    (letterboxed blit), plus a headless path that reports "no surface" instead
    of pretending to present.

The NVN replay is deliberately fail-closed: unsupported stage combinations,
bindless pool bindings and unknown reflection kinds produce a `[RED]` line and
drop that draw/dispatch rather than drawing a wrong picture.
Those are the next blocks, listed below.

## Test evidence

`ctest --test-dir build-asahi --output-on-failure` on the reference machine
(Apple M2 Pro, Fedora Asahi Remix 44, kernel 7.2.12, Mesa 26.2.6 Honeykrisp,
Vulkan 1.4.354): **38 tests, 38 passed (two skip when the driver has no presenting surface)**.

| Test | What it proves |
|---|---|
| `memory_16k` | Host page 16384, guest page 4096, permission groups, PROT_NONE fault, memfd aliases, dirty spans, generated AArch64 code returns 42 |
| `vulkan_smoke` | Real Honeykrisp device, real shaders, offscreen triangle, centre and corner readback |
| `vkapi_smoke` | Guest-facing Vulkan interception: instance, `VK_NN_vi_surface`, Tegra-shaped device, real image/memory/clear/readback, SPIR-V capture and pipeline-stage metadata |
| `synth_guest` | Synthetic NSO: load, relocate, TSD discovery, `DT_INIT`, native entry, `nnMain returned` |
| `synth_vk_guest` | Synthetic NSO importing `vkCreateInstance` through a JUMP_SLOT, resolved to a real Mesa instance |
| `shader_cache` | SPIR-V + reflection JSON loading, descriptor type reflection (UBO, combined image sampler), `c1`, vertex inputs |
| `nvn_vk` | NVN device/queue/program/command buffer, clear through a real render pass, guest readback |
| `nvn_draw` | General NVN draw: SPIR-V pipeline, vertex state, primitive expansion and indirect calls; fragment `imageStore` survives completion and is verified by guest texture readback; headless present returns a ticket |
| `nvn_texture` | NVN texture/sampler pools, fragment UBO and a 17-element combined texture/sampler array; shader dynamically samples element 16 and the readback matches the expected blend |
| `nvn_compute` | Compute dispatch writes the group-count record for the following indirect dispatch; the second dispatch reads the GPU-produced counts and writes all 64 expected SSBO values |
| `nvn_tex_ubo` | A texture handle written into the game's own uniform buffer (`tex_ubo`) is read at draw time and sampled |
| `audio_out` | `AudioOut` HLE: open, format queries, start, four appends, four release-queue returns through the PCM sink, stop/close |
| `audio_pcm` | PCM sink buffer lifecycle, real device or timed silent sink |
| `nvn_geometry` | Three-stage program with a geometry shader from `<key>_bg3.spv`; the geometry stage emits the centre triangle |
| `nvn_geometry_topo` | One program with `<key>_bg0` (points in, red) and `<key>_bg3` (triangles in, green): a point draw gets variant 0 and a triangle draw variant 3 |
| `nvn_tess` | Vertex + hull + domain + pixel stages; the domain shader emits the centre triangle |
| `nvn_tbuf` | NVN BUFFER texture bound as `samplerBuffer`, `texelFetch` readback |
| `nvn_array` | 2D array texture, two layers uploaded, `sampler2DArray` layer 1 readback |
| `nvn_cube` | Cubemap faces uploaded, `samplerCube` +Z readback |
| `nvn_3d` | 3D texture slices uploaded, `sampler3D` slice 1 readback |
| `nvn_msaa` | 4× MSAA draw, `nvnCommandBufferDownsample` resolve, readback |
| `nvn_indexed` | Indexed NVN draw from a guest index buffer, GPU readback |
| `nvn_copy` | Buffer↔texture copies with row/image strides; 2D array layers, selected mip/layer views, exact copies, 3D slices, nearest scaling, X mirroring, and R11G11B10F→RGBA16F conversion verify on readback |
| `nvn_compute` | NVN compute program, storage buffer binding, DispatchCompute, results copied back to guest memory |
| `nvn_compute_image` | Dynamically indexed two-element storage-image array writes distinct values to mipmapped 2D array textures at layer 1; an explicit all-bits NVN barrier precedes a second dispatch that reads both images and copies verified values to an SSBO |
| `nvn_depth` | NVN near=-w depth rewrite; `gl_FragCoord.z` is 0.25 for clip z=-0.5 |
| `nvn_depth_geometry` | A geometry shader emitting clip z=-0.5 is rewritten before `EmitVertex`; the centre reads z=0.25 instead of being clipped |
| `nvn_depth_zero` | NVN near=0 mode leaves clip z=0.5 unchanged |
| `nvn_alpha` | Alpha test with one program and four functions: LESS/GREATER keep the matching half, ALWAYS/NEVER keep both/none; also proves the reference and function are in the pipeline key |
| `nvn_logicop` | Logic operations over a blue clear: COPY, AND, OR, XOR, INVERT and COPY_INVERTED give their Vulkan results, and a logic op wins over an enabled blend |
| `nvn_advblend` | Porter-Duff advanced modes (ZERO/SRC/DST/SRC_OVER both premultiplied and not/PLUS/SRC_IN) give the fixed-function results |
| `vk_present` | Swapchain blit/present path on a `VK_EXT_headless_surface`; skips when the driver cannot present to one |
| `vk_window` | SDL offscreen window, Vulkan surface and device; skips presentation when the surface cannot present |
| `synth_guest_window` | The synthetic guest under a real SDL offscreen window; frontend exits when `nnMain` returns |
| `synth_vk_guest_window` | The Vulkan-importing synthetic guest under the offscreen window |
| `launcher_list` | The launcher discovers the repository profiles and their runtime path |
| `launcher_ui` | Launcher window, renderer and two UI frames under `SDL_VIDEODRIVER=dummy` |
| `launcher_saves` | Save backup and delete confined to the game folder |
| `launcher_settings` | Per-game settings become the runtime environment (fps, render scale, mute) |

The launcher UI is exercised headlessly through SDL's dummy video driver. A real
desktop window and Vulkan presentation were not exercised from the development
shell: the graphical session belongs to another user. The headless tests use the
same Vulkan device, render passes, pipelines and command buffers; the
window-specific part is swapchain creation, image acquisition and presentation.

## Not done / open

1. **Real game bring-up.** No legal game or homebrew NRO/NSP was available. The
   loader, SVC surface, filesystem, HID, audio and NVN paths have only synthetic
   tests so far. This is the single largest unknown.
   Direct-Vulkan import, runtime capture, and captured-inventory packaging are
   still not exercised with a real title.
2. **Presentation validation.** Swapchain creation/present is implemented but
   not exercised in a real desktop session.
3. **NVN2 mesh/task stages**, bindless pools, runtime and UBO/c2 descriptor
   arrays, transform feedback, sparse pools and DLSS equivalents remain
   unsupported and are refused with a red line. Fixed direct texture/sampler
   arrays are replayed element-by-element. Compute storage-image atomics have
   synthetic Honeykrisp coverage for R32UI imageAtomicAdd and R32I
   imageAtomicAdd/imageAtomicOr; this does not cover every format or
   operation. NVN advanced blend equations map to the Vulkan advanced blend
   extension when queried capabilities and state permit it, with synthetic
   coverage for Porter-Duff plus multiply, screen, overlay and difference.
   NVN's free-range normalized-destination setting still falls back. Alpha
   test, logic operations and the geometry input-topology variants are tested
   synthetically.
4. **Multi-game single binary.** The host is compiled for one game profile;
   the launcher selects a per-profile runtime binary (`nxnative-<title id>`) or
   the generic `nxnative-asahi`. Building one binary that loads any profile at
   run time is a larger refactor of the generated profile constants.
5. **NVN performance.** Runtime graphics and compute pipelines use a persisted
   Vulkan pipeline cache with device/driver UUID validation. Full captured-state
   precompilation is not implemented. Descriptor reuse, per-draw host buffer
   allocation, per-frame CPU deswizzle for CPU-written display textures, and a
   verbose logging default remain performance work.
6. **Movie decode, web applet, controller applet, software keyboard, camera**
   remain stubs or partial.
7. **Packaging.** `packaging/build_rpm.sh` builds a local Fedora RPM
   (platform+launcher, or one profile's host); no signed repository or public
   release artifact was produced.

## How to run

Build the platform, graphics and tests:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNXNATIVE_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Build the host for a profile (the generated constants are profile-specific):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNXNATIVE_BUILD_HOST=ON -DNXNATIVE_GAME=<base-title-id>
cmake --build build -j"$(nproc)"
```

Run a prepared folder:

```sh
build/nxnative-asahi run <prepared-game-folder> main
```

Or use the launcher, which discovers profiles under `dm/games` (or
`NXNATIVE_GAMES_DIR`), writes per-game settings under
`$XDG_CONFIG_HOME/nxnative/launcher`, and starts the matching runtime:

```sh
build/nxnative-launcher
build/nxnative-launcher --list
build/nxnative-launcher --dry-run <title-id>
```

For NVN shaders, point the runtime at a shader cache directory built by
`dm/gfx/tools/dmsh_build.py` (which now contains `<key>_b.spv` and
`<key>_b.json`):

```sh
NX_NVN_SHADER_CACHE=/path/to/cache/b-<chain> build/nxnative-asahi run <folder> main
```

The launcher uses `<game-folder>/shaders` automatically if it exists. The chain
version is in `dm/gfx/nvn/dmsh_chain.h`; the directory must be
`b-<that version>`.

## Dependencies

| Dependency | Purpose | Required |
|---|---|---|
| Vulkan loader and headers | device, WSI, guest Vulkan | yes |
| Mesa Vulkan driver (Honeykrisp on Asahi) | GPU | yes |
| SDL3 runtime | window, Vulkan surface, input, audio | yes (runtime, `dlopen`) |
| pthread, libdl, libm | host platform | yes |
| CMake, Ninja, C compiler | build | build-only |
| Python 3 | generated profile/header and tests | build-only |
| dav1d, libvpx | movie decode | staged, vendored |
| Shader cache from dmsh_build | NVN graphics | per game |

No Nintendo firmware, keys or game files are included.

## Honest conclusion

The port now has a real native execution core, a correct 16 KiB-page memory
model, real Vulkan rendering, a working NVN→Vulkan replay (verified down to
guest texture readback with textured and bindless-UBO draws), a Linux-native
SPIR-V toolchain, a game-preparation script, working Linux audio and a usable
launcher. It is not yet proven on a real game; that bring-up, NVN2 mesh/task and
advanced resource support, and desktop presentation validation are the main
engineering gaps.
