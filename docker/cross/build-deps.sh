#!/usr/bin/env bash
#
# Cross-build the static dependency stack (zlib, OpenSSL, libnfs) for the target
# described by the environment. This runs at *image build* time so the expensive
# dependency builds are baked into an image layer and cached; build-curl.sh then
# links curl against $PREFIX at *run* time.
#
# The per-target Dockerfiles set the environment this script reads:
#   PLATFORM        linux | windows | freebsd | macos   (informational)
#   TARGET_NAME     e.g. linux-x86_64                    (artifact suffix)
#   TRIPLE          autotools --host triple, e.g. x86_64-linux-musl
#   OPENSSL_TARGET  OpenSSL Configure target, e.g. linux-x86_64 / mingw64 / darwin64-x86_64
#   CC/CXX/AR/RANLIB/STRIP   toolchain commands (CC may carry --target/--sysroot for clang)
#   CROSS_PREFIX    tool prefix (e.g. x86_64-w64-mingw32-), used by zlib's win32 makefile
#   ZLIB_MODE       configure | win32gcc | skip
#   PREFIX          install prefix for the dep stack (default /opt/prefix)
#   ZLIB_VERSION / OPENSSL_VERSION / LIBNFS_REF / LIBSMB2_REF   source versions
#
# SPDX-License-Identifier: curl
set -euo pipefail

: "${PLATFORM:?PLATFORM not set}"
: "${TRIPLE:?TRIPLE not set}"
: "${OPENSSL_TARGET:?OPENSSL_TARGET not set}"
: "${CC:?CC not set}"

PREFIX="${PREFIX:-/opt/prefix}"
JOBS="${JOBS:-$(nproc)}"
ZLIB_MODE="${ZLIB_MODE:-configure}"
CROSS_PREFIX="${CROSS_PREFIX:-}"
export CC
export CXX="${CXX:-}"
export AR="${AR:-ar}"
export RANLIB="${RANLIB:-ranlib}"

mkdir -p "$PREFIX" /build
cd /build

fetch() { # url [url2 ...] -> downloads to basename of first url
  local out; out="$(basename "$1")"
  local url
  for url in "$@"; do
    echo "--- fetching $url"
    if curl -fsSL "$url" -o "$out"; then return 0; fi
  done
  echo "!!! could not download $out from any mirror" >&2
  return 1
}

########################################################################
# zlib
########################################################################
build_zlib() {
  if [ "$ZLIB_MODE" = "skip" ]; then
    echo "=== zlib: skipped (using target SDK's zlib) ==="
    return 0
  fi
  echo "=== zlib ${ZLIB_VERSION} (${ZLIB_MODE}) ==="
  fetch "https://zlib.net/fossils/zlib-${ZLIB_VERSION}.tar.gz" \
        "https://www.zlib.net/zlib-${ZLIB_VERSION}.tar.gz"
  tar xf "zlib-${ZLIB_VERSION}.tar.gz"
  cd "zlib-${ZLIB_VERSION}"
  if [ "$ZLIB_MODE" = "win32gcc" ]; then
    # zlib's unix ./configure does not produce a usable Windows static lib;
    # its win32 makefile does. PREFIX here is the *tool* prefix.
    make -f win32/Makefile.gcc -j"$JOBS" libz.a PREFIX="$CROSS_PREFIX"
    install -d "$PREFIX/include" "$PREFIX/lib"
    install -m644 zlib.h zconf.h "$PREFIX/include/"
    install -m644 libz.a "$PREFIX/lib/"
  else
    # honours CC/AR/RANLIB from the environment; static archive only.
    CHOST="$TRIPLE" ./configure --static --prefix="$PREFIX"
    make -j"$JOBS"
    make install
  fi
  cd /build
}

########################################################################
# OpenSSL (static libssl/libcrypto, no shared, no tests)
########################################################################
build_openssl() {
  echo "=== openssl ${OPENSSL_VERSION} (${OPENSSL_TARGET}) ==="
  fetch "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz" \
        "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
  tar xf "openssl-${OPENSSL_VERSION}.tar.gz"
  cd "openssl-${OPENSSL_VERSION}"
  if [ -n "${OPENSSL_CROSS_PREFIX:-}" ]; then
    # Some targets (mingw) need OpenSSL to derive *all* tools from a single
    # prefix — notably windres for the provider .rc files, which the env-CC
    # path leaves as a bare `windres` that can't find the mingw headers.
    # Unset the tool env vars so --cross-compile-prefix fully governs them.
    ( unset CC CXX AR RANLIB
      ./Configure "$OPENSSL_TARGET" \
        no-shared no-tests \
        --cross-compile-prefix="$OPENSSL_CROSS_PREFIX" \
        --prefix="$PREFIX" --openssldir="$PREFIX/ssl" ${OPENSSL_EXTRA:-} )
  else
    # OpenSSL honours $CC/$AR/$RANLIB from the environment for cross builds.
    ./Configure "$OPENSSL_TARGET" \
      no-shared no-tests \
      --prefix="$PREFIX" --openssldir="$PREFIX/ssl" ${OPENSSL_EXTRA:-}
  fi
  make -j"$JOBS"
  make install_sw
  cd /build
}

########################################################################
# libnfs (static, NFSv3) — this is what enables curl's nfs:// support
########################################################################
build_libnfs() {
  echo "=== libnfs ${LIBNFS_REF} ==="
  git clone --depth 1 --branch "${LIBNFS_REF}" https://github.com/sahlberg/libnfs.git
  cd libnfs
  ./bootstrap
  # --without-libkrb5: libnfs 6.x enables gssapi_krb5 by default and hard-fails
  #   without gssapi dev files; curl's NFSv3 path does not need Kerberos.
  # examples/utils are host tools we do not need and that can fail to cross-link.
  # --disable-werror: libnfs' win32 compat shim trips -Werror under mingw.
  ./configure --host="$TRIPLE" --prefix="$PREFIX" \
    --disable-shared --enable-static \
    --without-libkrb5 --disable-werror \
    --disable-examples --disable-utils
  make -j"$JOBS"
  make install
  cd /build
}

########################################################################
# libsmb2 (static, SMB2/SMB3) — this is what enables curl's smb:// support
########################################################################
build_libsmb2() {
  echo "=== libsmb2 ${LIBSMB2_REF} ==="
  # LIBSMB2_REF may be a tag or a raw commit SHA, so fetch it explicitly
  # rather than using --branch (which only accepts refs).
  mkdir libsmb2
  cd libsmb2
  git init -q
  git remote add origin https://github.com/sahlberg/libsmb2.git
  git fetch -q --depth 1 origin "${LIBSMB2_REF}"
  git checkout -q FETCH_HEAD
  # libsmb2's configure unconditionally hard-fails if libdl is absent, which
  # breaks the mingw (Windows) cross build where there is no libdl. Make the
  # check optional; curl's SMB path does not use libsmb2's dlopen feature.
  sed -i 's/AC_MSG_ERROR(\[dlsym not found, libdl is required\])//' configure.ac
  # libsmb2.h detects Windows only via _WINDOWS/_XBOX, but mingw-w64 (which its
  # own lib/compat.h keys off __MINGW32__) does not define _WINDOWS. That makes
  # the public header typedef t_socket as `int` on mingw while compat.h uses
  # `SOCKET`, which both breaks libsmb2's own build and would give curl an
  # int/SOCKET ABI mismatch. Teach the header to recognise mingw too.
  sed -i 's/#if defined(_WINDOWS)$/#if defined(_WINDOWS) || defined(__MINGW32__)/' include/smb2/libsmb2.h
  sed -i 's/#if defined(_WINDOWS) || defined(_XBOX)/#if defined(_WINDOWS) || defined(__MINGW32__) || defined(_XBOX)/' include/smb2/libsmb2.h
  # libsmb2 ships a fallback asprintf/vasprintf guarded only by `#ifndef
  # vasprintf`. mingw-w64 provides both as functions (not macros), so the guard
  # does not trip and the fallback redefines them. Skip the fallback on mingw.
  sed -i 's/^#ifndef vasprintf$/#if !defined(vasprintf) \&\& !defined(__MINGW32__)/' include/asprintf.h
  sed -i 's/^#ifndef asprintf$/#if !defined(asprintf) \&\& !defined(__MINGW32__)/' include/asprintf.h
  # invoke via sh: bootstrap may not have its exec bit after a raw fetch/checkout
  sh ./bootstrap
  # libsmb2's autotools build omits several Windows defines that its own CMake
  # build sets, so the mingw compile hits problems the autotools path never
  # covers. Reproduce the CMake Windows define set for the mingw target:
  #   WIN32_LEAN_AND_MEAN  - keep <windows.h> from clobbering the dcerpc struct
  #                          field named "interface" (#define interface struct)
  #   HAVE_LINGER          - mingw's winsock already defines struct linger
  #   NEED_GETLOGIN_R/GETPID/RANDOM/SRANDOM - provide the POSIX shims mingw lacks
  smb2_defs=""
  if [ "$PLATFORM" = "windows" ]; then
    smb2_defs="-DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -DHAVE_LINGER \
-DNEED_GETLOGIN_R -DNEED_GETPID -DNEED_RANDOM -DNEED_SRANDOM"
  fi
  # --without-libkrb5: use libsmb2's built-in NTLMSSP; curl's SMB path does
  #   not need Kerberos and krb5 dev files are not in the cross sysroots.
  # --disable-werror: libsmb2's win32 compat shim trips -Werror under mingw.
  ./configure --host="$TRIPLE" --prefix="$PREFIX" \
    --disable-shared --enable-static \
    --without-libkrb5 --disable-werror \
    CPPFLAGS="${CPPFLAGS:-} $smb2_defs"
  # Build only the library and headers: the utils/ subdir builds -Werror host
  # programs (smb2-cp, smb2-ls) that are not needed and can fail to cross-link.
  make -C include install
  make -C lib -j"$JOBS"
  make -C lib install
  install -d "$PREFIX/lib/pkgconfig"
  install -m644 libsmb2.pc "$PREFIX/lib/pkgconfig/"
  cd /build
}

build_zlib
build_openssl
build_libnfs
build_libsmb2

echo "=== dependency stack installed under $PREFIX ==="
ls -la "$PREFIX/lib" || true
