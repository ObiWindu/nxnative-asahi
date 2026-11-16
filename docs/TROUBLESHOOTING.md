# Troubleshooting

## First: collect a system report

```sh
build/nxnative-asahi sysinfo
build/nxnative-asahi vk-info
```

Include both outputs in a report. They cover the OS, kernel, page size, CPU,
memory, Vulkan API, GPU, driver, extensions, memory types, queue family,
formats and relevant limits.

## The build cannot find Vulkan

Install the loader and headers:

```sh
sudo dnf install vulkan-loader vulkan-loader-devel vulkan-headers
```

`cmake` reports `missing components: glslc glslangValidator` as a warning; the
Asahi build does not use those tools (the guest's Vulkan shaders are already
SPIR-V, and the NVN shader chain uses its own tools).

## No window / SDL3 errors

The front end loads `libSDL3.so.0` at run time. Install the runtime:

```sh
sudo dnf install sdl3
```

If you are over SSH or in a session without access to the compositor, force the
headless path:

```sh
SDL_VIDEODRIVER=dummy build/nxnative-asahi run <folder> main
```

A headless Vulkan title can create its device and render offscreen, but it has
no VI surface and will not present.

## `MESA: error: Opening /dev/dri/cardN failed: Permission denied`

The Vulkan device may still work through the render node
(`/dev/dri/renderD128`). `vk-info` and the offscreen test succeed on the
reference machine despite these messages. Do not disable SELinux or add broad
permissions; check that your user is in the `video`/`render` groups if the
render node is also inaccessible.

## `no Vulkan physical devices`

- Check `vulkaninfo --summary`.
- Install the Mesa Asahi drivers (`mesa-vulkan-drivers`).
- Do not set `NX_VK_DEVICE` to a name that does not match the installed GPU.
- `NX_VK_ALLOW_CPU=1` exists for diagnostics only.

## Vulkan validation

```sh
NX_VK_VALIDATION=1 build/nxnative-asahi vk-info
```

The layer must be installed (`vulkan-validation-layers`). If it is missing, the
program logs a warning and continues without it.

## `FATAL open .../main.fixups.tsv`

The `run` directory is not a prepared module folder. It must contain the
rewritten `<module>.patched.bin` and `<module>.fixups.tsv` files produced by the
NSP script / `dm/tools/patch_nso.py`.

## `missing symbol ...`

The guest module does not define a symbol the host's bootstrap calls, or the
module list is wrong. Use the profile's module order; the host's synthetic test
(`tests/make_synth_module.py`) lists the minimum bootstrap symbols.

## NVN shader is not in the cache

A line like `shader <key> (...) is missing in <dir>` means the translated shader
cache does not hold that key. `dm/gfx/tools/dmsh_build.py` must have built it
and the directory must be `b-<DMSH_CHAIN>`:

```sh
# the preparation script builds the cache and the runtime together:
python3 scripts/build_asahi_from_nsp.py <NSP folder> -k prod.keys
# or build one shader cache directly:
DM_GAME=<id> python3 dm/gfx/tools/dmsh_build.py <Switch package dir> <cache dir>
NX_NVN_SHADER_CACHE=/path/to/b-<chain> build/nxnative-asahi run <folder> main
```

The key is the 16 hex digits from `shader_map.tsv`; `<key>_b.spv` and
`<key>_b.json` must both be present. The chain version is in
`dm/gfx/nvn/dmsh_chain.h`. The translator and SPIRV-Cross are native AArch64
Linux binaries; rebuild a game's cache with `scripts/build_asahi_from_nsp.py`.

## NVN draw or dispatch is dropped with `[RED]`

The replay refuses what it cannot draw correctly. Common lines:

- `stage geometry` with a missing `<key>_bg<n>.spv`: the geometry shader was not
  built for the draw's input topology (points, lines, adjacency or triangles).
- A tessellation program needs both hull and domain stages; one alone is
  rejected.
- `binding kind 'tex_pool' / 'tex_htab' / ...`: bindless pool bindings are not
  implemented.
- A multisampled texture used for anything but a render target or a resolve
  reports a red line.
- `NVN draw without a bound program`: the command stream drew without
  `nvnCommandBufferBindProgram`.
- `texture ... is not registered`: the game bound a texture ID that was never
  registered in the active texture pool.

The draw is dropped, not approximated. Include the line and the preceding
`[nvn]` state in a report.

## Audio is silent

The Linux audio path uses SDL3's default output device (PipeWire or ALSA). Check
that a device exists:

```sh
pactl info        # or: aplay -l
```

If SDL3 cannot open a device, the sink runs silently at the correct rate; the
game still progresses. `DM_AUDIO_MUTE=1` mutes on purpose; the launcher's Mute
setting sets it. `DM_AUDIO_DUMP=<file>` writes the PCM blocks the game appends.

## The launcher finds no games

The launcher scans `NXNATIVE_GAMES_DIR` (colon-separated; default `dm/games` in
the build tree) and `$XDG_DATA_HOME/nxnative/games`. Each game folder needs
`game.tsv` directly, or a sibling `<title-id>_build` folder. A profile with no
`romfs`/`*.patched.bin` is shown as `game files not found`. The runtime is
looked up as `NXNATIVE_RUNTIME`, `NXNATIVE_RUNTIME_DIR/nxnative-<id>`, a
sibling `nxnative-<id>`, then `nxnative-asahi` beside the launcher or on `PATH`.

Use `--list` and `--dry-run <id>` to see what it found and the exact command and
environment it would start. The saves screen backs up (`B`), restores the newest
backup (`R`) and deletes (`D`, twice) only the selected game's `save` and
`save_device` folders.

## Gamepads are not detected

The current front end supports one SDL3 gamepad and the keyboard. Check:

```sh
ls /dev/input
```

SDL3 uses udev/evdev; the user must have permission to the input devices. The
window must be focused for keyboard input. Multiple controllers, motion and
rumble are not implemented.

## Logs

- Console: `[dm ...]` lines, controlled by `NX_LOG=0..3`.
- `NX_VK_DEBUG=1` prints the NVN pipeline, vertex state, texture/UBO bindings
  and draw arguments used by the replay.
- The launcher keeps settings in `$XDG_CONFIG_HOME/nxnative/launcher/`.
- The platform paths are created on first use.

## Crashes

The host installs a signal handler that prints the fault address, PC/LR, general
registers, a frame-pointer walk, the stack words below SP and the guest thread
table. The output is intentionally verbose; attach it to a report.

On Linux the ESR register is not exposed by `ucontext_t`, so the read/write
classification of a fault is reported as unknown in that line. The address and
PC are still exact.

## Performance

The first measurement to make is always `vk-info` plus a GPU timing run. The
emulator does not assume unified memory makes synchronization free; barriers and
fences are required. The NVN backend's performance work starts only after its
correctness path exists.
