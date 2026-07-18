# Cross-compiling curl in containers

Reproducible, containerized cross-compilation of this branch's curl — including
`nfs://` support (static **libnfs**) and `--parallel-chunks` — as **static**
binaries linked against **OpenSSL**, for:

| Target         | Toolchain (in container)                 | Output                     |
| -------------- | ---------------------------------------- | -------------------------- |
| Linux x86_64   | musl cross-gcc                           | `curl-linux-x86_64`        |
| Linux aarch64  | musl cross-gcc                           | `curl-linux-aarch64`       |
| Windows x86_64 | mingw-w64                                | `curl-windows-x86_64.exe`  |
| FreeBSD x86_64 | clang + lld + FreeBSD base sysroot       | `curl-freebsd-x86_64`      |
| macOS x86_64   | osxcross (needs an Apple SDK you supply) | `curl-macos-x86_64`        |

Each container builds the dependency stack once at image-build time (cached in a
layer), then links curl at run time. Every target statically links zlib*,
OpenSSL and libnfs. Linux/Windows/FreeBSD binaries are fully static; macOS links
those deps statically but `libSystem` dynamically (Apple ships no static libc).

\* macOS uses the SDK's zlib instead of building its own.

## Requirements

- `docker` **or** `podman` on `PATH`. On macOS both run a Linux VM — start it first:
  - Docker Desktop: just launch the app.
  - Podman: `podman machine init && podman machine start` (give it disk/CPU:
    `podman machine init --cpus 4 --disk-size 60`).
- ~15 GB free disk for images + dependency builds.
- Network access (toolchains and dependency sources are fetched during the build).

## Usage

```sh
cd docker/cross

./build-all.sh                          # all targets (macOS needs an SDK, see below)
./build-all.sh linux                    # both Linux arches
./build-all.sh linux-x86_64 windows     # a specific subset
ENGINE=podman ./build-all.sh freebsd    # force an engine
```

Artifacts land in `dist/cross/` at the repo root. The Linux-x86_64 run prints
`curl --version` (it can execute in the container); the other targets print
`(cross binary — not runnable on this build host)` and must be checked on the
real target OS.

Build a single target by hand:

```sh
podman build -t curl-cross-linux-x86_64 -f linux/Dockerfile --build-arg ARCH=x86_64 .
podman run --rm -v "$(git -C . rev-parse --show-toplevel):/src:ro" \
                -v "$(git -C . rev-parse --show-toplevel)/dist/cross:/dist" \
                curl-cross-linux-x86_64
```

## macOS: the SDK

Apple licenses the macOS SDK for use on Apple hardware only, so it is **not**
shipped here. But you don't have to package it by hand:

**On a Mac** (with Xcode or the Command Line Tools installed), `build-all.sh`
handles it automatically. When you build the `macos` target, it runs
[`macos/prepare-macos-sdk.sh`](macos/prepare-macos-sdk.sh), which packages the
SDK already on your machine (found via `xcrun`) into `macos/sdk/` and passes the
matching `DARWIN_VER` to the build. Nothing is downloaded; the tarball is
gitignored. Just run:

```sh
./build-all.sh macos      # or ./build-all.sh  to do every target
```

Re-runs reuse the packaged tarball. To force a refresh, delete
`macos/sdk/MacOSX*.sdk.tar.*` and build again.

**On a non-Mac host**, supply the SDK yourself: extract it from a Mac's Xcode
(e.g. with osxcross's `tools/gen_sdk_package*.sh`), drop the resulting
`MacOSX<ver>.sdk.tar.xz` into `macos/sdk/`, and pass the matching darwin version.
osxcross names its wrappers `x86_64-apple-darwin<DARWIN_VER>-clang`, where
`DARWIN_VER = <macOS major + 9>.<minor>` — e.g. SDK 15.5 → `24.5`, 14.4 → `23.4`:

```sh
podman build -t curl-cross-macos -f macos/Dockerfile --build-arg DARWIN_VER=24.5 .
podman run --rm -v "$PWD/../..:/src:ro" -v "$PWD/../../dist/cross:/dist" curl-cross-macos
```

## Configuration knobs

Dependency versions are `ARG`s — override per target with `--build-arg`:

- `OPENSSL_VERSION` (default `3.5.1`)
- `ZLIB_VERSION` (default `1.3.1`)
- `LIBNFS_REF` (default `libnfs-6.0.2`; may be any tag or branch, e.g. `master`)
- FreeBSD: `FREEBSD_VERSION` (`14.3`; the mirror keeps only the current point
  release, so bump this if `base.txz` 404s), `FREEBSD_ABI` (`14`)
- macOS: `DARWIN_VER`, `OSXCROSS_REF`

If a default version 404s (releases move), bump the `ARG` — the URLs in
`build-deps.sh` derive from these.

## How it fits together

```text
build-all.sh          orchestrates: build image -> run image -> collect artifact
<target>/Dockerfile   installs the toolchain, sets the per-target env, runs build-deps.sh
build-deps.sh         (image build) static zlib + OpenSSL + libnfs -> /opt/prefix
build-curl.sh         (entrypoint)  autoreconf + out-of-tree ./configure --host + make
```

curl is configured with `--host=<triple> --disable-shared --enable-static
--with-openssl=/opt/prefix --with-libnfs=/opt/prefix`. Because
[`lib/curl_setup.h`](../../lib/curl_setup.h) maps `USE_LIBNFS` → `USE_NFS`,
linking libnfs is what enables the `nfs://` scheme; `--version` on the target
should list **nfs** among the protocols. `--parallel-chunks` is tool-side and
always present.

The source tree is bind-mounted **read-only** and copied into the container
before building, so your working tree is never modified and host build artifacts
never leak into the cross build.

## Verification status

All five targets were built end-to-end in podman on an arm64 macOS host, each a
static, NFS-enabled curl (`nfs` in the protocol list, libnfs linked):

| Target        | Result                                             | NFS check                     |
| ------------- | -------------------------------------------------- | ----------------------------- |
| linux-x86_64  | `static-pie` ELF x86-64, OpenSSL 3.5.1, zlib 1.3.1 | ran in-container: `nfs: ENABLED` |
| linux-aarch64 | `static-pie` ELF ARM64                             | ran in-container: `nfs: ENABLED` |
| windows       | `PE32+` static `.exe`                              | configure summary + `nfs` symbols |
| freebsd       | static ELF, FreeBSD 14.3                           | configure summary + `libnfs` symbols |
| macos         | `Mach-O` x86_64 (PIE)                              | configure summary + `nfs://`/`libnfs` symbols |

Non-Linux binaries can't run on the Linux build host, so their NFS support is
confirmed from curl's configure summary (`Protocols: … nfs …`) plus libnfs
symbols in the binary rather than by executing `--version`.

## External dependencies that can drift

The build pulls a few things from the network; if a target breaks, it's almost
always one of these (all overridable — see the knobs above):

- **musl.cc** — the Linux toolchain comes from `musl.cc`, which occasionally goes
  down. Swap the URL in `linux/Dockerfile` for a mirror (`https://more.musl.cc/...`).
- **FreeBSD point releases rotate** — `download.freebsd.org` serves only the
  current one; bump `FREEBSD_VERSION` (a `base.txz` 404 is the tell).
- **Dependency versions** — `OPENSSL_VERSION` / `ZLIB_VERSION` / `LIBNFS_REF` are
  `ARG`s; bump if a release URL moves.
- **macOS SDK / DARWIN_VER** — osxcross wrapper names embed the full darwin
  version (`x86_64-apple-darwin<major+9>.<minor>-clang`); on a Mac this is derived
  automatically, but a hand-supplied SDK needs a matching `DARWIN_VER`.
