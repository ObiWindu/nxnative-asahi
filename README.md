# nxnative-asahi

**nxnative-asahi** is a native Nintendo Switch runtime for **Asahi Linux on Apple
Silicon**. It ports the features and capabilities of the
[nxnative](https://github.com/seedds/nxnative) project to Linux, using Linux
facilities instead of Apple frameworks: AArch64 guest code execution, Horizon
OS services, a native Vulkan renderer through Mesa's Honeykrisp driver, SDL3
windowing/input/audio and a Linux desktop launcher.

The game's own ARM64 code runs directly on the Apple chip. The rewriter changes
only instructions that depend on the console kernel, and the host answers
Horizon's supervisor calls and services. It is not an emulator in the
instruction-interpretation sense, and nothing from a game is included in this
repository.

> **Port status:** the Linux foundation, Vulkan device, 16 KiB-page memory
> model, NVN→Vulkan replay, audio, input, launcher and Linux-native game
> preparation are implemented and tested with synthetic workloads and real GPU
> draws. No retail game has been run in the development environment yet; the
> first real-game bring-up is the current milestone. See
> [docs/ASAHI_STATUS.md](docs/ASAHI_STATUS.md) for the tested state and
> [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) for the precise matrix.

## Relationship to nxnative

nxnative was built around macOS and iOS, Apple's graphics and audio frameworks,
and Apple's SDKs. nxnative-asahi is an independent Linux fork that keeps the
useful emulator functionality — the AArch64 loader and execution, memory model,
Horizon services, shader translation, NVN command model and game profiles — and
replaces the platform layer.

The macOS implementation is being **phased out as the port advances**. The iOS
application and IPA packaging have been removed. Metal compilation and the iOS
shader targets have been removed from the Asahi shader build. Legacy macOS files
and tools still present in the tree are not part of the Asahi target and are
removed or replaced as their Linux equivalents become complete. This fork makes
no promise of continued macOS or iOS compatibility.

## Features

Implemented and exercised by the Linux build:

- native AArch64 execution of the guest, ELF/NSO loading, relocations, `DT_INIT`
  and `nnMain`;
- 4 KiB guest memory on 16 KiB Asahi pages, aliases, dirty spans and W^X;
- a real Vulkan device and queue through Mesa Honeykrisp, with runtime feature
  and format queries rather than hard-coded device assumptions;
- an NVN→Vulkan replay: pools, textures, samplers, render targets, clears,
  copies, fences, events, vertex/fragment pipelines, vertex attributes,
  uniforms, storage buffers, storage images, compute dispatches, alpha tests,
  logic operations, Porter-Duff advanced blending, geometry shaders (one
  variant per input topology), tessellation, 2D-array/cube/3D textures, MSAA
  resolve, guest texture readback and swapchain presentation;
- guest-facing Vulkan interception for titles that ship a Vulkan loader;
- SDL3 audio through PipeWire/ALSA for `AudioOut` and `OutputStream`, with a
  timed silent fallback;
- SDL3 keyboard and gamepad input;
- a graphical launcher with game discovery, frame-rate/render-scale/mute
  settings, save backup/delete and per-game runtime selection;
- a Linux-native preparation path for NSP folders, Switch 2 `.nup` containers
  and dump folders, including DLC and update-only containers.

Known gaps are listed in [docs/ASAHI_STATUS.md](docs/ASAHI_STATUS.md). Bindless
pool arrays, descriptor arrays, NVN2 mesh/task replay, movies, rumble, motion,
multiple controllers and applets are incomplete. Unsupported draws are refused
with a `[RED]` line rather than rendered incorrectly.

## Hardware and software requirements

- **OS:** Fedora Asahi Remix 44, with a 16 KiB-page Asahi kernel. Other
  AArch64 Linux distributions may work if they provide the same kernel, Mesa
  and Vulkan capabilities.
- **Hardware:** Apple Silicon M1 or M2 initially. M3/M4 and later are
  capability-dependent: support depends on the installed Asahi kernel,
  firmware, Mesa Vulkan driver and the features it exposes.
- **GPU:** Mesa's Apple Vulkan implementation (Honeykrisp). The host queries
  the device; it does not assume a GPU name or a fixed extension set.
- **Display:** Wayland preferred; X11 is supported where SDL3 can create a
  surface.
- **Build tools:** CMake, Ninja, GCC or Clang, Python 3.12+.
- **Runtime libraries:** the Vulkan loader and headers, SDL3, and
  PipeWire/ALSA for audio.
- **Python modules for game preparation:** `lz4`, `capstone` 5.x,
  `fonttools`, `zbic` and `zstandard`.

No Nintendo firmware, keys, games or SDK files are shipped or required to build
the tests.

## Installation

### 1. Install the build dependencies

On Fedora Asahi Remix:

```sh
sudo dnf install cmake ninja-build gcc gcc-c++ python3-pip \
  vulkan-loader vulkan-headers SDL3 pipewire pipewire-alsa
```

Package names vary by distribution. `SDL3` is loaded at run time; the host does
not require the SDL3 development headers for its own build. Install the
`vulkan-validation-layers` package if you want to run with
`NX_VK_VALIDATION=1`.

Install the Python game-preparation modules:

```sh
python3 -m pip install --user lz4 'capstone>=5,<6' fonttools zbic zstandard
```

The Linux shader translator, NSP extractor and SPIRV-Cross are committed as
native AArch64 Linux binaries in the repository. Building them from source
needs .NET 10 only for the two C# tools; ordinary builds and game preparation
need no .NET, Xcode or Apple SDK.

### 2. Build the platform and tests

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNXNATIVE_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

### 3. Build a runtime for a game profile

A host build is profile-specific; there is deliberately no default game:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNXNATIVE_BUILD_HOST=ON \
  -DNXNATIVE_GAME=<base-game-title-id>
cmake --build build -j"$(nproc)"
```

The title ID is the base game's 16-hex-digit ID; the profile is read from
`dm/games/<id>/game.tsv`. Profiles outside the repository can be selected with
`DM_GAMES_DIR`.

To build several per-profile runtimes for the launcher:

```sh
scripts/build_asahi_games.sh <base-title-id> [more...]
NXNATIVE_RUNTIME_DIR=$PWD/build-asahi/runtimes build/nxnative-launcher
```

`scripts/build_asahi.sh` is a convenience wrapper for the platform, graphics
and tests; set `GAME=<id>` to build a host too.

### 4. Prepare a game

The preparation script accepts an NSP folder, a Switch 2 `.nup` container (or
the folder holding it), or a dump folder with `base/`, `updates/` and `dlc/`:

```sh
# NSP folder: prod.keys is required
python3 scripts/build_asahi_from_nsp.py "<NSP folder>" -k prod.keys

# Switch 2 .nup or dump folder: no keys
python3 scripts/build_asahi_from_nsp.py "<game.nup>"
python3 scripts/build_asahi_from_nsp.py "<dump folder>"
```

The script extracts the game files, rewrites the code modules, splits and
translates the shaders to SPIR-V, builds the profile runtime and assembles a
launcher-ready game folder. It prints a direct launch command and a launcher
command. Work files stay in `<source>/<id>_build/`; delete that folder to start
over.

### 5. Run

```sh
build/nxnative-asahi sysinfo
build/nxnative-asahi vk-info
build/nxnative-asahi profile
build/nxnative-asahi run <prepared-game-folder> main
```

Or start the graphical launcher:

```sh
NXNATIVE_GAMES_DIR=<folder of prepared games> build/nxnative-launcher
```

The launcher writes per-game settings under `$XDG_CONFIG_HOME/nxnative/launcher`
and launches with `DM_ROMFS`, `DM_SAVE`, `DM_TEMP`, the game's shader directory
and any per-game `nxnative.env` overrides.

For a headless smoke test of the host:

```sh
SDL_VIDEODRIVER=dummy build/nxnative-asahi run build/synth-game main
```

## How to play

The supported workflow is:

1. install the dependencies above;
2. build the platform and a runtime for your game's profile;
3. prepare the game with `scripts/build_asahi_from_nsp.py`;
4. launch it from the printed command or from `nxnative-launcher`.

See [docs/BUILDING.md](docs/BUILDING.md) for the detailed build and preparation
instructions, [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) for common
failures and [docs/ASAHI_STATUS.md](docs/ASAHI_STATUS.md) for the current
limitations.

## Tests

The Linux test suite needs no game files:

```sh
ctest --test-dir build --output-on-failure
```

It covers the 16 KiB memory model, synthetic NSO loading and execution, real
Vulkan rendering and readback, NVN replay (draws, textures, compute, geometry,
tessellation, MSAA, alpha, logic operations, advanced blending, bindless
handles in uniform buffers), presentation, audio, launcher settings/saves and
the Linux tools.

The Python tool tests cover extraction, rewriting, shader splitting, NUP/dump
handling, profiles and documentation:

```sh
python3 -m pytest dm/tools/tests -q
```

GPU and desktop tests are meaningful only on real Asahi hardware; run them
locally. The development environment used for the current status is an Apple
M2 Pro, Fedora Asahi Remix 44, kernel 7.2.12 and Mesa 26.2.6 Honeykrisp.

## Contributing

Contributions are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) before
opening a pull request.

In short:

- this is an **Asahi Linux-first** project; do not add new macOS, iOS, Xcode,
  Metal, Objective-C or Apple SDK dependencies, and do not preserve an Apple
  code path just for compatibility;
- keep the useful emulator functionality: fix the root cause rather than
  hiding a symptom;
- never commit game files, keys, title keys, firmware or extracted game data;
- run the CMake and Python tests relevant to the change, and say what was
  tested on real hardware;
- after changing a shader-chain tool, regenerate
  `dm/gfx/nvn/dmsh_chain.h` with
  `python3 dm/gfx/tools/dmsh_chain.py --header`;
- document game-specific findings in the game's profile note and general
  mechanisms in `docs/reference/`;
- contributions are accepted under the project's MIT license.

Large changes, new device classes, new rendering paths and new platform
baselines should be discussed in an issue before implementation.

## Documentation

- [Architecture](docs/ASAHI_ARCHITECTURE.md)
- [Building and installation](docs/BUILDING.md)
- [Memory model](docs/MEMORY_MODEL.md)
- [Graphics](docs/GRAPHICS.md)
- [Compatibility](docs/COMPATIBILITY.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Status and limitations](docs/ASAHI_STATUS.md)
- [Agent-oriented development notes](docs/agents/README.md)

## License

nxnative-asahi is free software under the **MIT License**. See [LICENSE](LICENSE)
for the full text. By contributing, you agree that your contribution is
licensed under the same terms.

## Legal and attribution

This project does not include Nintendo firmware, `prod.keys`, title keys, games
or other copyrighted material. Users must supply their own legally obtained game
files and keys. Development and tests use synthetic data and real GPU workloads.

nxnative-asahi is an independent fork started from the nxnative codebase. The
original nxnative copyright notice and MIT license are retained in
[LICENSE](LICENSE). Nintendo Switch is a trademark of Nintendo; this project is
not affiliated with or endorsed by Nintendo.
