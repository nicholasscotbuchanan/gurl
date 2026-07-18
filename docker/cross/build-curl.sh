#!/usr/bin/env bash
#
# Entrypoint: cross-build a static curl (with NFS + --parallel-chunks) against
# the dependency stack that build-deps.sh installed into $PREFIX at image-build
# time. The curl source tree is bind-mounted read-only at /src; the finished
# binary lands in /dist as curl-$TARGET_NAME[.exe].
#
# SPDX-License-Identifier: curl
set -euo pipefail

: "${TRIPLE:?TRIPLE not set}"
: "${TARGET_NAME:?TARGET_NAME not set}"
: "${CC:?CC not set}"

PREFIX="${PREFIX:-/opt/prefix}"
SRC="${SRC:-/src}"
OUT="${OUT:-/dist}"
JOBS="${JOBS:-$(nproc)}"
EXE="${EXE:-}"
EXTRA_LDFLAGS="${EXTRA_LDFLAGS:-}"
EXTRA_LIBS="${EXTRA_LIBS:-}"
ZLIB_MODE="${ZLIB_MODE:-configure}"

export CC
export AR="${AR:-ar}"
export RANLIB="${RANLIB:-ranlib}"
# Point pkg-config exclusively at our cross prefix so it never picks up host
# libs. OpenSSL 3.x installs to lib64 on some targets (e.g. linux x86_64) while
# zlib/libnfs use lib, so include both.
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig"

# Work from a clean copy so the bind-mounted (read-only) source stays untouched
# and stale host build artifacts don't leak into the cross build.
work=/tmp/curl-build
rm -rf "$work"
mkdir -p "$work"
echo "=== copying source tree ==="
tar -C "$SRC" --exclude=.git --exclude=dist -cf - . | tar -C "$work" -xf -

# Purge host-build residue from the copy. Leftover autotools artifacts poison
# the out-of-tree (VPATH) build: make finds a stale libcurl.la/*.lo via VPATH in
# the source dir, decides the target is up-to-date, and never builds it under
# _build/lib — so the final link fails with "cannot find ../lib/libcurl.la".
# These file types are never committed in curl, so deleting them is safe.
#  - config.status also makes autoconf refuse an out-of-tree configure;
#  - a stray curl_config.h shadows the freshly generated one via ""-include.
rm -f "$work"/config.status "$work"/config.cache "$work"/config.log
find "$work" -type d \( -name .libs -o -name .deps \) -exec rm -rf {} + 2>/dev/null || true
find "$work" -type f \( \
     -name '*.o'  -o -name '*.lo'  -o -name '*.la'  -o -name '*.a'   \
  -o -name '*.Plo' -o -name '*.Po' -o -name '*.Tpo' -o -name 'curl_config.h' \
  \) -delete 2>/dev/null || true

cd "$work"
echo "=== autoreconf ==="
autoreconf -fi

# curl finds zlib via the SDK on macOS (ZLIB_MODE=skip); elsewhere use our build.
zlib_arg=(--with-zlib="$PREFIX")
if [ "$ZLIB_MODE" = "skip" ]; then
  zlib_arg=(--with-zlib)
fi

mkdir -p _build && cd _build
echo "=== configure (host=$TRIPLE) ==="
../configure \
  --host="$TRIPLE" \
  --disable-shared --enable-static \
  --with-openssl="$PREFIX" \
  --with-libnfs="$PREFIX" \
  --with-libsmb2="$PREFIX" \
  "${zlib_arg[@]}" \
  --without-libpsl \
  --without-brotli --without-zstd \
  --disable-ldap --disable-ldaps \
  LIBS="$EXTRA_LIBS"

echo "=== configure summary ==="
sed -n '/Configured to build curl/,/^$/p' config.log 2>/dev/null || true

# EXTRA_LDFLAGS (e.g. libtool's -all-static) is applied at *make* time, not
# configure time: -all-static is a libtool flag that plain-gcc configure link
# tests would reject. Passing it here makes libtool link the curl tool fully
# statically. (Empty for macOS, where a fully static binary is not possible.)
echo "=== make ==="
# Overriding LDFLAGS on the make line replaces the value configure baked in, so
# re-add the dependency search paths that --with-openssl/libnfs/zlib had put
# there; the -lnfs/-lssl/-lcrypto/-lz names themselves come from LIBS. Include
# lib64 because OpenSSL 3.x installs there on some targets.
make -j"$JOBS" LDFLAGS="-L$PREFIX/lib -L$PREFIX/lib64 $EXTRA_LDFLAGS"

install -d "$OUT"
artifact="$OUT/curl-${TARGET_NAME}${EXE}"
cp "src/curl${EXE}" "$artifact"
"${STRIP:-strip}" "$artifact" 2>/dev/null || true

echo "=== built $artifact ==="
file "$artifact" 2>/dev/null || true
# Only Linux targets can (sometimes) run inside this Linux container; a Windows,
# FreeBSD or macOS binary would just fault, so don't even try to exec those.
if [ "${PLATFORM:-}" = "linux" ] && "$artifact" --version >/dev/null 2>&1; then
  "$artifact" --version
  echo "--- protocols ---"
  "$artifact" --version | grep -qi nfs && echo "nfs: ENABLED" || echo "nfs: MISSING"
else
  echo "(cross binary for ${TARGET_NAME} — not run on this build host; verify on the target)"
fi
