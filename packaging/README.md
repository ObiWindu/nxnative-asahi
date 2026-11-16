# Packaging

`fedora-asahi/nxnative-asahi.spec` builds on Fedora Asahi Remix 44. It is not a
signed release artifact yet, but a local RPM builds and installs (verified on
the reference machine). `packaging/build_rpm.sh` automates the source tarball
and `rpmbuild` run; pass a profile title ID to package a host for it.

A release needs a decision the source tree does not make for the packager: the
host is currently built per game profile (`NXNATIVE_GAME=<base title ID>`) and
the profile's constants are compiled in. A package can either:

- build one host per title and install launchers per game, or
- gain the runtime-profile refactor described in
  [docs/ASAHI_ARCHITECTURE.md](../docs/ASAHI_ARCHITECTURE.md) before shipping a
  single multi-game binary.

The draft spec builds the platform/graphics/tests without a profile, because a
spec cannot know the user's title ID. Add the profile at build time or wait for
the runtime-profile work.

The launcher and desktop entry are installed by CMake:

```sh
cmake --install build --prefix /usr/local
```

`scripts/build_asahi_games.sh` builds one `nxnative-<title id>` runtime per
profile into `build-asahi/runtimes`; the launcher selects the matching binary
(`NXNATIVE_RUNTIME_DIR`). The generic `nxnative-asahi` remains the CLI and the
fallback runtime.

The spec builds the platform and launcher by default. To package a host for one
profile:

```sh
packaging/build_rpm.sh 010015100B514000
```

The resulting `nxnative-asahi` embeds that profile; the launcher's runtime
probe accepts it for that title. The spec disables LTO, which otherwise drops
the local static HLE thunk targets on Fedora's `-flto` flags.
