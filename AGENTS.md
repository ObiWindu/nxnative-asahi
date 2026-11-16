# Notes for agents

- **nxnative-asahi** is an independent Asahi Linux fork of nxnative. It runs
  Nintendo Switch software natively on Apple Silicon under Fedora Asahi Remix:
  the game's own AArch64 code runs, rewritten only where the console kernel
  requires it (`dm/tools/patch_nso.py`), on a Linux host that answers Horizon's
  SVCs and services (`dm/host`, `src/horizon`) and implements NVN and Vulkan on
  real Vulkan through Mesa's Honeykrisp driver (`dm/gfx/nvn`, `src/`). SDL3
  provides the window, input and audio; the launcher is a Linux desktop app.
  This is not the macOS/iOS project: macOS and iOS code is being phased out as
  the Linux port advances. Do not add new Apple SDK, Xcode, Metal,
  Objective-C, Cocoa, CoreAudio or GameController dependencies, and do not keep
  an Apple code path alive merely for compatibility.
- [README.md](README.md) is the player-facing page: project scope, installation,
  game preparation and running. [docs/BUILDING.md](docs/BUILDING.md) has the
  detailed build and dependency instructions.
- Where to start:

  | Task | Read first |
  |---|---|
  | Build, install, prepare a game | [docs/BUILDING.md](docs/BUILDING.md), then [README.md](README.md) |
  | A crash, abort, hang, `[RED]` line or wrong picture | [docs/agents/symptoms.md](docs/agents/symptoms.md) (search the log text), then [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) and the [subsystem references](docs/reference/README.md) |
  | A game that does not reach its frame loop | [docs/agents/03-bringup.md](docs/agents/03-bringup.md), then the symptom index |
  | Rendering or shaders | [docs/GRAPHICS.md](docs/GRAPHICS.md), [docs/agents/04-graphics.md](docs/agents/04-graphics.md), [rendering](docs/reference/rendering.md), [shaders](docs/reference/shaders.md) and [NVN objects](docs/reference/nvn-objects.md) |
  | One game, or a change to its profile | its note, `dm/games/<id>/README.md`, then [dm/games/README.md](dm/games/README.md) for the keys and the games table |
  | Another Switch title | [scripts/build_asahi_from_nsp.py](scripts/build_asahi_from_nsp.py), then [docs/agents](docs/agents/README.md) |
  | Checking a change before a pull request | [docs/agents/06-dev-loop.md](docs/agents/06-dev-loop.md), plus the test commands below |
- Where a new finding goes, once: a fact about one game (its passes, shader
  keys, measurements, why its profile needs a key or a cut, its status) in that
  game's note, in the section it belongs to and marked current or resolved; a
  general mechanism (a host or tool change that holds for other games) as an
  entry in `docs/reference/<subsystem>.md` (Symptom, Cause, Fix, Found on with
  the game's note linked), then `python3 dm/tools/docs_symptoms.py` to update
  the symptom index, and a line under the game note's "Found on this game".
  Link to a finding instead of repeating it. Adding a game takes its note and
  its row in [the games table](dm/games/README.md#games). Keep testing
  milestones, platform and remaining validation in the game note.
  `dm/tools/tests/test_docs.py` checks links, anchors, notes/table rows, the
  symptom index and the length of this file (1,500 words) and of each guide in
  `docs/agents` (6,000).
- Each game's constants and shader tables form a profile in `dm/games/<id>/`,
  where `<id>` is the base game's title ID: `game.tsv` plus the tables (and
  `patches.py` when the rewriter has patches for its builds). Mario's profile
  is `010015100B514000`. Tools pick the profile with `DM_GAME`; CMake and
  `scripts/build_asahi.sh` pick it with `GAME` / `NXNATIVE_GAME`; the launcher
  selects a per-profile runtime. There is no default game: set it every time
  (the tools stop and list the profiles otherwise). Put per-game values in the
  profile, not in code ([dm/games/README.md](dm/games/README.md)).
- Prefer general optimizations and fixes to game-specific rules. Performance
  work (frame pacing, power, caches, load times) counts here as much as
  correctness. First look for a change that holds for every game: Horizon /
  NVN / Vulkan semantics, or what the binary, its SDK version, its imports or
  its files show. Use a game-specific rule (a profile key, a rewriter cut, a
  build-ID gate, a per-game branch in the host) only when it is absolutely
  necessary, that is, when no general rule can do it.
- Find a bug's root cause and fix it there; do not just work around it. Before
  changing code, find why it happens: the faulting instruction and its
  decompilation, the value that went wrong and who wrote it, the Horizon / NVN
  / Vulkan semantics the host or translator gets wrong. A change that only
  hides the symptom (a stub, a skipped call, a clamp, a retry, a sleep or a
  longer timeout, a cache flush, a profile key that turns a feature off) is a
  workaround: use one only when the cause is known and lies where it cannot be
  fixed (the game's own code, which a rewriter cut changes deliberately, or a
  platform limit), or when the user asks for a stopgap. Then say so in the code
  comment and in the game note or reference entry: the cause, or what is still
  unknown, marked open.
- Per-build knowledge is keyed by the NSO build ID: rewriter cuts, observation
  gates and frame-step stubs in the profile's `patches.py` (a game's own
  modules) or `dm/tools/sdk_patches.py` (Nintendo's `sdk` modules), which
  `dm/tools/patch_nso.py` (the general rewriter) loads; the profile's `build`
  rows (the builds the game was brought up on, and their display versions);
  and host behaviour that uses fixed module addresses checks
  `module_t.build_id` (from `fixups.tsv`; `dm_main_build()`). When a new game
  build appears, carry addresses over with `dm/tools/nso_fnmap.py` and check
  them by disassembly; never let fixed addresses run on a build they were not
  found in. For automatic cuts across updates, opt individual patches into
  `MATCH_CUTS` only after verifying their function fingerprints and
  assumptions; follow
  [Function-matched cuts](docs/reference/rewriter.md#function-matched-cuts-for-unregistered-builds).
  The shared matcher derives addresses from code; fixed addresses remain
  build-gated.
- To analyse or port **another** Switch title, run
  `python3 scripts/build_asahi_from_nsp.py` on its NSP folder, `.nup` file or
  dump folder. With no matching profile it writes a survey to
  `<source>/<title ID>_survey/survey.md`, then a provisional profile guessed
  from the game files (`dm/games/profile_guess.py`; found through
  `DM_GAMES_DIR`). The Asahi path builds SPIR-V shaders and the per-profile
  runtime on Linux; it does not build an IPA or use the Mac host for heap checks.
  Then follow [docs/agents](docs/agents/README.md) (extraction, binary survey,
  host bring-up, graphics, debugging; the [symptom index](docs/agents/symptoms.md)
  for each failure) and add a profile with its note. `dm/ios` and the IPA
  workflow no longer exist.
- Scripts the user runs live in `scripts/` at the repository root; each runs
  from a terminal and exits non-zero on error. The Asahi entry points are
  `build_asahi_from_nsp.py` (prepare a game), `build_asahi.sh` (build/test) and
  `build_asahi_games.sh` (per-profile runtimes). `build_ipa_from_nsp.py` remains
  the shared extraction/rewrite/shader-split core used by the Asahi script; it
  is not the Asahi packaging path. `tools/kateai_graph.py` regenerates the Kate
  AI project graph at `.kateai/project_graph.json` (generated and git-ignored).
  Tools that only agents or other scripts call stay under `dm/`.
- Game files, keys and anything extracted from them live in the game's own
  folder, never in this repository and never in the home folder (it is nearly
  full): the work folder `<source>/<id>_build/` for extraction, runs and
  analysis. Do not print key values. Never commit ROMs, NSP/NUP/NCA files,
  shader caches, saves or extracted assets.
- Language: everything is in English. Red log lines (something unimplemented or
  wrong) start with `[RED] `. Text that must match a game's own Chinese data is
  written as escapes (`"\xE8\xB5\xB7"`), never as characters.
- The shader translation chain has one version for every game, `DMSH_CHAIN` in
  `dm/gfx/nvn/dmsh_chain.h`, which the host is compiled with and loads only
  libraries of. It covers the Asahi chain's tools (`dm/gfx/tools/dmsh_*`,
  `Program.cs`, `dm/gfx/nvn/dmsh_abi.h` and `dmsh_chain.lock`). A profile's own
  inputs (`shader_map.tsv`, `shader_facts.tsv`, `shader_pairs.tsv`,
  `bf_want.tsv`, its `shader_*` / `nvn_*` keys) are not part of it: they are in
  each shader's cache key, and their digest stamps the libraries
  (`b-<V>/inputs.tsv`). The procedure:
  1. After changing a chain tool, run `dmsh_chain.py --header` and commit
     `dm/gfx/nvn/dmsh_chain.h` with the change
     (`dm/tools/tests/test_nsp_chain.py` fails on a stale one).
  2. After that, or after changing a profile's inputs, rebuild that profile's
     shaders with `dmsh_build.py` or `build_asahi_from_nsp.py`; the full build
     writes `c1_full.tsv` and `cb_extent.tsv` from its Linux outputs. Give each
     game its own output directory.
  3. Rebuild the host.
  The translator, extractor and SPIRV-Cross are native AArch64 Linux binaries;
  a translator rebuild must update `tool_lock.py --update-lock` and
  `dmsh_chain.py --header`. The Asahi shader build is SPIR-V-only: no Metal
  compile, `IOSOUT` or iPhone libraries.
- Test continuously:
  `scripts/build_asahi.sh` runs the CMake/Ninja build and CTest;
  `python3 -m pytest dm/tools/tests -q` runs the Linux tool tests. A change to
  memory, Vulkan, GPU synchronization, audio, input or threading also needs a
  real game/Asahi run. `scripts/regress.py` is the legacy macOS regression
  runner and is not usable on Linux; do not cite it for Asahi.
- Commit only when asked, with
  `git -c user.name=seedds -c user.email=seedds@gmail.com commit`; the message
  opens with `area/component: summary`
  ([Commit messages](docs/agents/06-dev-loop.md#8-commit-messages)), and a
  change a front end must act on takes a `Front-ends:` line.
- The project is released under the MIT License ([LICENSE](LICENSE)). By
  contributing, you agree that your contribution is licensed under the same
  terms. The original nxnative copyright and MIT notice are retained.
