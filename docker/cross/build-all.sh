#!/usr/bin/env bash
#
# Build curl cross-compile images and run them to produce static binaries in
# dist/cross/ at the repo root. Works with docker or podman.
#
# Usage:
#   ./build-all.sh                       # build every target (macos requires an SDK)
#   ./build-all.sh linux-x86_64 windows  # build only the named targets
#   ENGINE=podman ./build-all.sh linux   # force a container engine
#
# Target names: linux-x86_64  linux-aarch64  windows  freebsd  macos
# (aliases: "linux" = both Linux arches, "all" = everything)
#
# SPDX-License-Identifier: curl
set -euo pipefail

cd "$(dirname "$0")"
CROSS_DIR="$PWD"
REPO_ROOT="$(cd ../.. && pwd)"
DIST="$REPO_ROOT/dist/cross"

ENGINE="${ENGINE:-}"
if [ -z "$ENGINE" ]; then
  ENGINE="$(command -v docker || command -v podman || true)"
fi
if [ -z "$ENGINE" ]; then
  echo "error: neither docker nor podman found on PATH" >&2
  exit 1
fi
echo ">> using container engine: $ENGINE"
mkdir -p "$DIST"

# build_image <image-tag> <dockerfile-rel> [--build-arg ...]
build_image() {
  local tag="$1" dockerfile="$2"; shift 2
  echo ">> building image $tag ($dockerfile)"
  "$ENGINE" build -t "$tag" -f "$dockerfile" "$@" "$CROSS_DIR"
}

# run_image <image-tag>  -> writes dist/cross/curl-<target>[.exe]
run_image() {
  local tag="$1"
  echo ">> running $tag"
  "$ENGINE" run --rm \
    -v "$REPO_ROOT:/src:ro" \
    -v "$DIST:/dist" \
    "$tag"
}

target_linux_x86_64() { build_image curl-cross-linux-x86_64 linux/Dockerfile --build-arg ARCH=x86_64; run_image curl-cross-linux-x86_64; }
target_linux_aarch64() { build_image curl-cross-linux-aarch64 linux/Dockerfile --build-arg ARCH=aarch64; run_image curl-cross-linux-aarch64; }
target_windows() { build_image curl-cross-windows windows/Dockerfile; run_image curl-cross-windows; }
target_freebsd() { build_image curl-cross-freebsd freebsd/Dockerfile; run_image curl-cross-freebsd; }
target_macos() {
  # On a Mac this packages the locally-installed SDK and prints the matching
  # DARWIN_VER; on a non-Mac it reuses a tarball you dropped in macos/sdk/ (or
  # exits with instructions if none is present).
  local dv
  dv="$("$CROSS_DIR/macos/prepare-macos-sdk.sh")" || return 1
  build_image curl-cross-macos macos/Dockerfile --build-arg "DARWIN_VER=$dv"
  run_image curl-cross-macos
}

# Expand requested targets into a deduped, order-preserving list of functions.
# Kept array-free so it also runs under macOS's stock bash 3.2 with `set -u`.
[ "$#" -eq 0 ] && set -- all

TODO=""
add_target() { case " $TODO " in *" $1 "*) ;; *) TODO="$TODO $1" ;; esac; }
for t in "$@"; do
  case "$t" in
    all)           add_target linux_x86_64; add_target linux_aarch64; add_target windows; add_target freebsd; add_target macos ;;
    linux)         add_target linux_x86_64; add_target linux_aarch64 ;;
    linux-x86_64)  add_target linux_x86_64 ;;
    linux-aarch64) add_target linux_aarch64 ;;
    windows)       add_target windows ;;
    freebsd)       add_target freebsd ;;
    macos)         add_target macos ;;
    *) echo "error: unknown target '$t'" >&2; exit 1 ;;
  esac
done

for fn in $TODO; do
  echo "==================================================================="
  echo "== target: $fn"
  echo "==================================================================="
  "target_${fn}"
done

echo ">> done. Artifacts in $DIST:"
ls -la "$DIST"
