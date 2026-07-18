#!/usr/bin/env bash
#
# Package the macOS SDK already installed on this Mac (via Xcode or the Command
# Line Tools) into docker/cross/macos/sdk/ so the osxcross container can build a
# macOS target with no manual steps. Prints the matching DARWIN_VER on stdout so
# the caller can pass it to `docker build --build-arg DARWIN_VER=...`.
#
# This only packages *your own* locally-installed SDK — Apple licenses it for use
# on Apple hardware, which is exactly where this runs. Nothing is fetched from a
# network. The resulting tarball is gitignored and never committed.
#
# No-op (re-uses the existing tarball, still prints DARWIN_VER) if an SDK is
# already present in sdk/.
#
# SPDX-License-Identifier: curl
set -euo pipefail

cd "$(dirname "$0")"          # docker/cross/macos
SDK_DIR="sdk"
mkdir -p "$SDK_DIR"

log() { echo ">> $*" >&2; }
die() { echo "error: $*" >&2; exit 1; }

# osxcross names its toolchain darwin<major+9>.<minor> for a macOS SDK (e.g.
# 15.5 -> darwin24.5), so we must emit the full major.minor to match the wrapper
# names exactly (a bare "24" yields "x86_64-apple-darwin24-clang: not found").
darwin_ver() {
  local major="${1%%.*}" rest minor
  rest="${1#*.}"
  if [ "$rest" = "$1" ]; then minor=0; else minor="${rest%%.*}"; fi
  if [ "$major" -ge 11 ]; then
    echo "$(( major + 9 )).${minor}"
  else
    echo "$(( minor + 4 )).0"   # 10.x era
  fi
}

# Respect an SDK the user dropped in manually.
existing="$(ls "$SDK_DIR"/MacOSX*.sdk.tar.* 2>/dev/null | head -1 || true)"

if [ -n "$existing" ]; then
  log "using existing SDK tarball: $existing"
  ver="$(basename "$existing")"; ver="${ver#MacOSX}"; ver="${ver%%.sdk*}"
  darwin_ver "$ver"
  exit 0
fi

[ "$(uname -s)" = "Darwin" ] || die "not running on macOS and no SDK tarball in $SDK_DIR/ (supply one — see ../README.md)"
command -v xcrun >/dev/null || die "'xcrun' not found — install Xcode or the Command Line Tools (xcode-select --install)"

sdk_path="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null)" || die "could not locate the macOS SDK (try: sudo xcode-select --switch /Applications/Xcode.app)"
[ -d "$sdk_path" ] || die "SDK path '$sdk_path' does not exist"
sdk_ver="$(xcrun --sdk macosx --show-sdk-version 2>/dev/null)" || die "could not read the macOS SDK version"

# Resolve to the real directory and archive it by its real name (osxcross globs
# MacOSX*.sdk inside the tarball). Do NOT dereference internal symlinks.
real="$(cd "$sdk_path" && pwd -P)"
parent="$(dirname "$real")"
base="$(basename "$real")"

# Prefer xz (smaller) when available, else gzip (always present on macOS).
if command -v xz >/dev/null; then
  out="$SDK_DIR/MacOSX${sdk_ver}.sdk.tar.xz"
  log "packaging macOS SDK ${sdk_ver} from ${real} -> ${out} (xz, this takes a minute)"
  tar -C "$parent" -cf - "$base" | xz -T0 -c > "$out"
else
  out="$SDK_DIR/MacOSX${sdk_ver}.sdk.tar.gz"
  log "packaging macOS SDK ${sdk_ver} from ${real} -> ${out} (gzip, this takes a minute)"
  tar -C "$parent" -czf "$out" "$base"
fi

log "wrote $out ($(du -h "$out" | cut -f1))"
darwin_ver "$sdk_ver"
