# Building on Asahi Linux

## Reference environment

The port is developed and tested on:

| Item | Value |
|---|---|
| Hardware | Apple M2 Pro (12-core CPU, 16-core GPU, 16 GiB unified memory) |
| OS | Fedora Asahi Remix 44 (KDE Plasma) |
| Kernel | `7.2.12-402.asahi.fc44.aarch64+16k` (16 KiB pages) |
| GPU driver | Mesa 26.2.6, Honeykrisp |
| Vulkan | 1.4.354 |
| Compiler | GCC 16.2.1; Clang 22.1.8 also builds and passes the tests |
| Build | CMake 3.31 + Ninja |

The build is native AArch64 Linux. There is no Xcode, Objective-C, Apple SDK or Metal dependency in the Asahi targets.

## Dependencies

System packages:

- `cmake`, `ninja`, `gcc` or `clang`
- `vulkan-loader`, `vulkan-headers`
- `SDL3` runtime (`sdl3` on Fedora). Development headers are not required; the
  front end declares the small ABI subset it uses and loads `libSDL3.so.0` at
  run time.
- `python3` for generating the per-game profile header and the synthetic test
  module.
- Optional: `vulkan-validation-layers` for `NX_VK_VALIDATION=1`.
- Optional: the `shaderc` runtime library for the NVN draw tests; they report
  SKIP when `libshaderc_shared.so.1` is absent.
- `pipewire`/`pipewire-alsa` or ALSA for audio (SDL3 opens the default device; without one the sink runs silently at the correct rate).

No proprietary firmware, keys, games or Nintendo SDK files are shipped or
required to build the tests.

## Configure and build

A game profile is required for a host build; there is deliberately no default.

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNXNATIVE_GAME=010015100B514000 \
  -DNXNATIVE_BUILD_HOST=ON

cmake --build build -j"$(nproc)"
```

The profile ID is the base game title ID; the profile is read from
`dm/games/<id>/game.tsv`. A profile outside the repository can be selected with
`DM_GAMES_DIR` (the NSP script's provisional profile uses this).

Without a game profile the platform, Vulkan device and Vulkan interception
libraries and tests still build:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

## Prepare a game

The preparation script reads an NSP folder, a Switch 2 `.nup` container, or a
dump folder (`base/`, `updates/`, `dlc/`), rewrites the code modules, builds the
shader SPIR-V for NVN games and compiles the per-profile runtime. Detected
direct-Vulkan games with packaged SPIR-V skip the NVN shader chain and pass
their shader modules to the guest Vulkan path. Runtime-generated Vulkan shader
modules can be captured during a Linux game run; OpenGL game preparation is
not supported yet:

```sh
python3 scripts/build_asahi_from_nsp.py <NSP folder> -k prod.keys
python3 scripts/build_asahi_from_nsp.py <game.nup>          # no keys
python3 scripts/build_asahi_from_nsp.py <dump folder>       # no keys
```

Python 3.12+ with `lz4`, `capstone`, `fonttools`, `zbic` and `zstandard`
(`python3 -m pip install --user lz4 'capstone>=5,<6' fonttools zbic zstandard`).
The script prints a direct launch command and a launcher command. The extracted
files, rewritten modules, shader cache and prepared game folder stay in
`<source>/<id>_build/`; delete that folder to start over. A NUP or dump needs
no keys.

## Run

```sh
build/nxnative-asahi sysinfo
build/nxnative-asahi vk-info
build/nxnative-asahi run <prepared-game-folder> [modules...] [-- guest args...]
build/nxnative-asahi profile      # the compiled profile's title ID and name
```

`<prepared-game-folder>` is the output of the preparation script's rewrite
step: it contains `<module>.patched.bin` and `<module>.fixups.tsv` for the
profile's modules. The guest filesystem and save services need `DM_ROMFS` and
`DM_SAVE` (and usually `DM_TEMP`) in the environment, as the preparation script
sets them; the launcher fills them in automatically. Selecting a game and building its host is a profile-time decision, so
the launcher looks for `nxnative-<title id>` beside itself or on `PATH` and
falls back to `nxnative-asahi`:

For a title whose Vulkan shaders are generated at runtime, capture them while
playing by setting `NX_VK_SHADER_CAPTURE_DIR` on the host process:

```sh
NX_VK_SHADER_CAPTURE_DIR="$PWD/vk-capture" build/nxnative-asahi run <prepared-game-folder> main
```

The directory receives content-addressed SPIR-V modules, stage/specialization
records, descriptor set layouts and push-constant ranges. The driver pipeline cache remains the mechanism that avoids later
pipeline compilation. Re-run preparation with `--vk-capture-dir` to verify
SPIR-V digests, reflect resource bindings/constants and package the capture
inventory beside the prepared game's `game.tsv`:

```sh
python3 scripts/build_asahi_from_nsp.py <same source> --vk-capture-dir "$PWD/vk-capture"
```

Captured modules are inventoried for inspection and future precompilation;
repeat any original import options such as `--keys` or `--package` on this
command. Pipeline-state precompilation from the inventory is still incomplete.

```sh
build/nxnative-launcher              # game library, settings, saves
build/nxnative-launcher --list
build/nxnative-launcher --dry-run <title-id>
```

To give the launcher a runtime for several profiles, build them in one go; each
binary is compiled with its own profile constants and named by title ID:

```sh
scripts/build_asahi_games.sh 010015100B514000 0100A6301214E000
NXNATIVE_RUNTIME_DIR=$PWD/build-asahi/runtimes build/nxnative-launcher
```

The launcher scans `NXNATIVE_GAMES_DIR` (default `dm/games` in the build tree)
and `$XDG_DATA_HOME/nxnative/games`. A game folder may contain `game.tsv`
directly or the profile may sit beside a `<title-id>_build` folder. It writes
settings under `$XDG_CONFIG_HOME/nxnative/launcher`, and launches with
`DM_ROMFS`, `DM_SAVE`, `DM_TEMP`, an optional `NX_NVN_SHADER_CACHE` from
`<folder>/shaders`, and any per-game `nxnative.env` overrides.

NVN shader caches: `dm/gfx/tools/dmsh_build.py` writes `<key>_b.spv` and
`<key>_b.json` (geometry shaders also get `<key>_bg0.spv` ... `_bg4.spv`, one
per input topology). On Asahi it writes SPIR-V only; there is no Metal compile.
`scripts/build_asahi_from_nsp.py` runs the whole build, or point
`NX_NVN_SHADER_CACHE` at a `b-<chain>` directory (or place it in
`<game-folder>/shaders/b-<chain>`; the launcher picks the first `b-*` there).
The chain version is in `dm/gfx/nvn/dmsh_chain.h`. The translator, extractor
and SPIRV-Cross are native AArch64 Linux binaries built from the in-repo
sources (`dm/tools/native_tools.sh`, `dm/gfx/tools/spirv_cross_bin/build.sh`).

The Vulkan path expects a graphical session. On a headless SSH session, force
the headless front end with:

```sh
SDL_VIDEODRIVER=dummy build/nxnative-asahi run <folder> main
```

In a desktop session, SDL3 creates a Wayland (or X11) window and a Vulkan
surface. Keyboard and gamepad input are read each poll.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

A profile build runs thirty-two tests. A platform-only build (no
`NXNATIVE_GAME`) runs the ten that do not need the guest host: the memory,
Vulkan, Vulkan-interception, shader-cache, PCM and launcher tests. The NVN
replay tests and `AudioOut` need a profile.

Thirty-eight tests (profile build):

| Test | What it proves |
|---|---|
| `memory_16k` | 4 KiB guest pages on a 16 KiB host page: grouping, faulting, aliases, dirty spans, generated-code execution |
| `vulkan_smoke` | Real Honeykrisp device, real shaders, offscreen triangle and readback |
| `vkapi_smoke` | The guest-facing Vulkan interception layer: instance, device, memory, image clear, queue submit and readback |
| `synth_guest` | Loader, MOD0/dynamic parsing, relocation, pthread TSD slots, `DT_INIT`, native AArch64 entry into `nnMain`, clean exit |
| `synth_vk_guest` | The same, plus a JUMP_SLOT import of `vkCreateInstance` resolved through the host table and a real Mesa instance created from the synthetic guest |
| `shader_cache` | SPIR-V + reflection JSON loading and descriptor/type/vertex-input reflection |
| `nvn_vk` | NVN device/queue/program/command buffer, clear and guest readback |
| `nvn_draw` | General NVN draw with SPIR-V pipeline, vertex attributes, GPU readback and headless-present ticket |
| `nvn_texture` | NVN texture/sampler pools, fragment UBO and combined texture+sampler descriptors |
| `nvn_tex_ubo` | A texture handle stored in the game's uniform buffer is read at draw time |
| `nvn_geometry` | Geometry shader from `<key>_bg3.spv` in a three-stage program |
| `nvn_geometry_topo` | Point and triangle draws select `<key>_bg0` and `<key>_bg3` from one program |
| `nvn_tess` | Hull and domain stages with a tessellated centre triangle |
| `nvn_tbuf` | Buffer texture (`samplerBuffer`) texel fetch |
| `nvn_array` | 2D array texture and `sampler2DArray` layer readback |
| `nvn_cube` | Cubemap faces and `samplerCube` readback |
| `nvn_3d` | 3D texture slices and `sampler3D` readback |
| `nvn_msaa` | 4× MSAA draw and downsample resolve |
| `nvn_indexed` | Indexed draw through a guest index buffer |
| `nvn_copy` | Buffer/texture and texture/texture copies with byte-exact round trip |
| `nvn_compute` | Compute program, SSBO binding, dispatch and guest-memory copy-back |
| `nvn_compute_image` | Compute storage-image write and readback |
| `nvn_depth` | NVN near=-w depth rewrite and fragment-depth readback |
| `nvn_depth_geometry` | The depth rewrite is applied before a geometry stage's `EmitVertex` |
| `nvn_depth_zero` | NVN near=0 depth mode leaves clip z unchanged |
| `nvn_alpha` | Alpha test across LESS/GREATER/ALWAYS/NEVER with one program, proving the pipeline key carries the alpha state |
| `nvn_logicop` | Logic operations COPY/AND/OR/XOR/INVERT/COPY_INVERTED over a clear |
| `nvn_advblend` | Porter-Duff advanced blend modes through fixed-function factors |
| `vk_present` | Swapchain create/blit/present on a headless surface when the driver supports it |
| `vk_window` | SDL offscreen window and Vulkan surface/device; presentation skips when unsupported |
| `synth_guest_window` | Synthetic guest under an SDL offscreen window and clean frontend exit |
| `synth_vk_guest_window` | Vulkan-importing synthetic guest under an SDL offscreen window |
| `audio_out` | `AudioOut` open/start/append/release/stop/close through the PCM sink |
| `audio_pcm` | PCM sink buffer lifecycle, real device or timed silent sink |
| `launcher_list` | Launcher profile discovery and runtime resolution |
| `launcher_ui` | Launcher window, renderer and two UI frames under the dummy video driver |
| `launcher_saves` | Save backup and delete confined to the game folder |
| `launcher_settings` | Per-game settings become the runtime environment (fps, render scale, mute) |

The NVN draw tests use shaders compiled by the system's shaderc library at test
time; no game shader data is needed. `libshaderc_shared.so.1` is optional: the
shader-cache test reports SKIP if it is absent.

`synth_guest` uses `tests/make_synth_module.py`, a synthetic NSO-format module
with no Nintendo code or data.

## Install

```sh
cmake --install build --prefix /usr/local
```

The install target installs both executables, the desktop entry and the
documentation. A Fedora Asahi Remix RPM builds with:

```sh
packaging/build_rpm.sh                 # platform + launcher
packaging/build_rpm.sh <title-id>      # a host for that profile too
```

It is a local package, not a signed release repository.

## Environment switches

| Variable | Meaning |
|---|---|
| `NX_LOG=0..3` | Log level (error, warn, info, debug) |
| `NX_VK_DEVICE=<text>` | Select a Vulkan device by name substring |
| `NX_VK_VALIDATION=1` | Enable `VK_LAYER_KHRONOS_validation` when installed |
| `NX_VK_ALLOW_CPU=1` | Allow llvmpipe for diagnostics (not a supported game path) |
| `SDL_VIDEODRIVER=dummy` | Headless front end |
| `DM_GAME` / `GAME` | Profile selection for the legacy tools |
| `DM_NVN_*`, `DM_*` | Existing host diagnostic switches, where the corresponding service is linked |
