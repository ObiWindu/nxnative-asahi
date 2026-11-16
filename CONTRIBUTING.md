# Contributing to nxnative-asahi

Issues and pull requests are welcome. This page explains how to report a
problem, how to prepare a change and what a pull request should contain.

nxnative-asahi is an independent **Asahi Linux** fork of nxnative. The project
runs the game's own AArch64 code natively on Apple Silicon and replaces the
original Apple platform layer with Linux, SDL3 and real Vulkan. The macOS and
iOS implementations are being phased out as the port advances.

## Reporting a problem

Open an issue with as much of this as you can:

- the nxnative-asahi commit (`git rev-parse --short HEAD`);
- the game's title ID and version, and its profile under `dm/games/<id>/`;
- the exact preparation command and the run command;
- the contents of `nxnative-asahi sysinfo` and `nxnative-asahi vk-info`;
- the host distribution, kernel, Mesa and Vulkan versions, and whether the
  session is Wayland or X11;
- the relevant log from `<source>/<id>_build/logs/` and the host's run log;
- whether the failure is a crash, hang, `[RED]` line or wrong picture, and the
  last lines before it.

`python3 scripts/bug_report.py <title ID | prepared game folder>` collects the
usual host and build information into one archive. Inspect the archive before
attaching it. It must not contain game files, keys or title keys.

Never attach or commit:

- NSP, NUP, XCI, NCA or NRO files;
- `prod.keys`, title keys or anything that can decrypt a game;
- RomFS contents, code modules, shader dumps or save files;
- extracted game data of any kind.

Title IDs, build IDs, offsets, short logs and screenshots of the emulator are
fine. Game files stay in the game's own folder outside this repository.

## Before you start

- Read [README.md](README.md) for the project scope and current status.
- Read [docs/ASAHI_STATUS.md](docs/ASAHI_STATUS.md) and
  [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) so a known gap is not reported
  as a new bug.
- For a crash, hang or wrong picture, search
  [docs/agents/symptoms.md](docs/agents/symptoms.md) for the text in the log.
- For a new title, read [docs/agents/03-bringup.md](docs/agents/03-bringup.md)
  and the [survey guide](docs/agents/02-survey.md).
- For graphics work, read [docs/GRAPHICS.md](docs/GRAPHICS.md) and
  [docs/reference/rendering.md](docs/reference/rendering.md).

## Development rules

- **Asahi Linux is the target.** Do not add new macOS, iOS, Xcode, Metal,
  Objective-C, Cocoa, CoreAudio or Apple SDK dependencies. Do not keep an Apple
  code path working merely for compatibility.
- **Prefer general fixes.** Fix the Horizon, NVN, Vulkan or memory mechanism
  rather than adding a per-game branch. A profile key or rewriter cut is for
  facts that belong to one game/build.
- **Fix causes, not symptoms.** A stub, skipped call, clamp, retry, sleep,
  longer timeout or cache flush that only hides the failure must say what the
  cause is and why it cannot be fixed there.
- **Do not assume 4 KiB host pages.** All host memory operations must work with
  the runtime `sysconf(_SC_PAGESIZE)`, including 16 KiB Asahi pages and 4 KiB
  guest pages.
- **Do not assume Vulkan capabilities.** Query the device and driver at run
  time. A missing feature must produce a clear incompatibility, not a wrong
  picture.
- **Fail closed in the graphics replay.** An unsupported draw or resource logs
  a `[RED]` line and is dropped rather than rendered incorrectly.
- **Keep game data out of the tree.** Profiles contain constants and tables,
  not game files.
- **Document findings once.** A fact about one game goes in
  `dm/games/<id>/README.md`. A general mechanism goes in the matching
  `docs/reference/` page and in `docs/agents/symptoms.md` through
  `python3 dm/tools/docs_symptoms.py`.

## Building and testing

A profile is required for a host build; there is deliberately no default game.

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNXNATIVE_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Build one runtime for a profile:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNXNATIVE_BUILD_HOST=ON \
  -DNXNATIVE_GAME=<base-title-id>
cmake --build build -j"$(nproc)"
```

Run the Python tool tests:

```sh
python3 -m pytest dm/tools/tests -q
```

Test on real Asahi hardware when the change touches memory, Vulkan, GPU
synchronization, audio, input, threading or the launcher. Synthetic tests are
necessary but are not a substitute for a game workload. Say exactly what was
tested, on which machine and with which kernel/Mesa versions.

After changing a shader-chain tool, regenerate the chain header and commit it
with the change:

```sh
python3 dm/gfx/tools/dmsh_chain.py --header
```

The chain version is shared by every game, so a chain change invalidates shader
caches. Rebuild the shaders for a representative game or explain why that was
not possible.

## Pull requests

- **One change per pull request, based on current `main`.** Rebase rather than
  merging `main` into the branch.
- **Include tests.** Add or update a test for every behaviour change. A fix
  without a test is accepted only when the failure cannot be reproduced
  synthetically; say why.
- **Update documentation.** Keep the README, `docs/BUILDING.md`,
  `docs/COMPATIBILITY.md`, `docs/ASAHI_STATUS.md`, the profile note and the
  reference page in step with the change.
- **Commit messages** start with `area/component: summary` and explain the
  symptom, the cause, the fix and what was tested. A change a front end must
  act on takes a `Front-ends:` line.
- **No generated game data.** Do not commit a shader cache, a prepared game
  folder, logs containing keys, or anything extracted from a game.
- **Be explicit about limits.** If a feature is partial, say which paths work
  and which are refused.

Large changes — a new platform baseline, a new rendering backend, a second
implementation of an existing path, or a change over roughly 1,000 lines —
should start as an issue so scope and approach can be agreed before review.

## License

nxnative-asahi is released under the **MIT License**; see [LICENSE](LICENSE).
By submitting a contribution, you agree that it is licensed under the same
terms. Do not contribute code that cannot be redistributed under MIT.

## Attribution

nxnative-asahi started from the nxnative codebase. The original copyright notice
and MIT license are retained in [LICENSE](LICENSE). Nintendo Switch is a
trademark of Nintendo; this project is not affiliated with or endorsed by
Nintendo.
