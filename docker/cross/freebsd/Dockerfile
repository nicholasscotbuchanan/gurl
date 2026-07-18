# Cross-compile curl for FreeBSD x86_64 as a static binary using clang + lld and
# a sysroot extracted from the FreeBSD base distribution.
# Build context must be docker/cross/.
#
# SPDX-License-Identifier: curl
FROM debian:bookworm-slim

# download.freebsd.org keeps only the current point release; bump this when it
# rotates (a 404 on base.txz is the symptom).
ARG FREEBSD_VERSION=14.3
ARG FREEBSD_ARCH=amd64
ARG FREEBSD_ABI=14           # the "freebsdNN" part of the target triple
ARG ZLIB_VERSION=1.3.1
ARG OPENSSL_VERSION=3.5.1
ARG LIBNFS_REF=libnfs-6.0.2

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y --no-install-recommends \
      clang lld llvm binutils curl ca-certificates git perl make \
      autoconf automake libtool pkg-config xz-utils \
 && rm -rf /var/lib/apt/lists/*

# Pull just the pieces of the FreeBSD base system we need to link against.
ENV SYSROOT=/opt/freebsd-sysroot
RUN mkdir -p "$SYSROOT" \
 && curl -fsSL "https://download.freebsd.org/ftp/releases/${FREEBSD_ARCH}/${FREEBSD_VERSION}-RELEASE/base.txz" -o /tmp/base.txz \
 && tar -C "$SYSROOT" -xJf /tmp/base.txz ./lib ./usr/lib ./usr/include \
 && rm /tmp/base.txz

ENV PLATFORM=freebsd \
    TARGET_NAME=freebsd-x86_64 \
    TRIPLE=x86_64-unknown-freebsd${FREEBSD_ABI} \
    OPENSSL_TARGET=BSD-x86_64 \
    PREFIX=/opt/prefix \
    CC="clang --target=x86_64-unknown-freebsd${FREEBSD_ABI} --sysroot=/opt/freebsd-sysroot -fuse-ld=lld" \
    CXX="clang++ --target=x86_64-unknown-freebsd${FREEBSD_ABI} --sysroot=/opt/freebsd-sysroot -fuse-ld=lld" \
    AR=llvm-ar \
    RANLIB=llvm-ranlib \
    STRIP=llvm-strip \
    CROSS_PREFIX= \
    EXE= \
    ZLIB_MODE=configure \
    ZLIB_VERSION=${ZLIB_VERSION} \
    OPENSSL_VERSION=${OPENSSL_VERSION} \
    LIBNFS_REF=${LIBNFS_REF}

# deps first so editing build-curl.sh (or the link flags below) does not bust
# the dependency layer
COPY build-deps.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/build-deps.sh && /usr/local/bin/build-deps.sh

# curl-only link settings, kept after the deps build so tweaking them is cheap.
# -z muldefs: libnfs bundles its own RPC/ZDR symbols (e.g. _null_auth) that also
# live in FreeBSD's libc.a (Sun RPC is in libc there, unlike glibc/musl); let lld
# keep the first definition instead of erroring on the duplicates.
ENV EXTRA_LDFLAGS="-all-static -Wl,-z,muldefs" \
    EXTRA_LIBS="-lpthread"

COPY build-curl.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/build-curl.sh

WORKDIR /src
ENTRYPOINT ["/usr/local/bin/build-curl.sh"]
