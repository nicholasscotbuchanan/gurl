# Cross-compile curl for macOS (x86_64 or arm64, via --build-arg MACOS_ARCH)
# using osxcross.
#
# SDK: on a Mac, build-all.sh runs macos/prepare-macos-sdk.sh to package your
# locally-installed SDK into macos/sdk/ and pass the matching DARWIN_VER
# automatically. On a non-Mac host, drop a MacOSX<ver>.sdk.tar.xz into
# macos/sdk/ yourself and pass --build-arg DARWIN_VER=<macOS major + 9>.<minor>
# (osxcross names its wrappers x86_64-apple-darwin<that>-clang, e.g. SDK 15.5 ->
# 24.5). See ../README.md and https://github.com/tpoechtrager/osxcross.
#
# Build context must be docker/cross/.
#
# SPDX-License-Identifier: curl
#
# Ubuntu 24.04 for a recent host clang (18): osxcross and the target compile
# against the SDK's libc++, and an older clang (e.g. Debian bookworm's 14) fails
# on newer SDK headers ("only virtual member functions can be marked override").
FROM ubuntu:24.04

ARG DARWIN_VER=24.5
ARG OSXCROSS_REF=master
ARG ZLIB_VERSION=1.3.1
ARG OPENSSL_VERSION=3.5.1
ARG LIBNFS_REF=libnfs-6.0.2
ARG LIBSMB2_REF=5264e394d1f3046ac56b2f0bcdc375703320730e

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y --no-install-recommends \
      clang lld llvm cmake patch python3 git make curl ca-certificates \
      autoconf automake libtool pkg-config cpio bzip2 xz-utils \
      libssl-dev liblzma-dev libxml2-dev uuid-dev libbz2-dev zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*

# osxcross builds the Apple cross toolchain (clang wrappers + cctools) from the SDK.
RUN git clone --depth 1 --branch "${OSXCROSS_REF}" https://github.com/tpoechtrager/osxcross /opt/osxcross
# The whole sdk/ dir is copied so the build fails clearly if no SDK was supplied.
COPY macos/sdk/ /opt/osxcross/tarballs/
RUN cd /opt/osxcross \
 && ls tarballs/*.sdk.tar.* >/dev/null 2>&1 || { \
      echo 'ERROR: no macOS SDK found in docker/cross/macos/sdk/ — see this Dockerfile header'; exit 1; } \
 && UNATTENDED=1 ./build.sh
ENV PATH="/opt/osxcross/target/bin:${PATH}"

# Target macOS arch: x86_64 or arm64. osxcross builds both wrapper sets from the
# one SDK, and this ARG is only used below (after the osxcross layer), so the two
# arches share the cached toolchain image. OpenSSL's target is darwin64-<arch>.
ARG MACOS_ARCH=x86_64

ENV PLATFORM=macos \
    TARGET_NAME=macos-${MACOS_ARCH} \
    TRIPLE=${MACOS_ARCH}-apple-darwin${DARWIN_VER} \
    OPENSSL_TARGET=darwin64-${MACOS_ARCH} \
    PREFIX=/opt/prefix \
    CC=${MACOS_ARCH}-apple-darwin${DARWIN_VER}-clang \
    CXX=${MACOS_ARCH}-apple-darwin${DARWIN_VER}-clang++ \
    AR=${MACOS_ARCH}-apple-darwin${DARWIN_VER}-ar \
    RANLIB=${MACOS_ARCH}-apple-darwin${DARWIN_VER}-ranlib \
    STRIP=${MACOS_ARCH}-apple-darwin${DARWIN_VER}-strip \
    CROSS_PREFIX=${MACOS_ARCH}-apple-darwin${DARWIN_VER}- \
    EXE= \
    ZLIB_MODE=skip \
    ZLIB_VERSION=${ZLIB_VERSION} \
    OPENSSL_VERSION=${OPENSSL_VERSION} \
    LIBNFS_REF=${LIBNFS_REF} \
    LIBSMB2_REF=${LIBSMB2_REF}

# deps first so editing build-curl.sh (or the link flags below) does not bust
# the dependency layer
COPY build-deps.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/build-deps.sh && /usr/local/bin/build-deps.sh

# curl-only link settings, kept after the deps build so tweaking them is cheap.
# curl's lib/macos.c (Curl_macos_init) reads the system proxy configuration via
# CoreFoundation + SystemConfiguration, so the tool link needs those frameworks.
# No -static on macOS: Apple ships no static libc, so libSystem stays dynamic
# (the deps in $PREFIX are still linked statically as only .a exist there).
ENV EXTRA_LDFLAGS= \
    EXTRA_LIBS="-framework CoreFoundation -framework SystemConfiguration"

COPY build-curl.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/build-curl.sh

WORKDIR /src
ENTRYPOINT ["/usr/local/bin/build-curl.sh"]
