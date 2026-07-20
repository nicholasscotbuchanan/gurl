# Cross-compile curl for Linux (x86_64 or aarch64) as a fully static musl binary.
#
#   ARCH=x86_64 | aarch64  selects the musl cross toolchain and OpenSSL target.
#   Build context must be docker/cross/ (so build-*.sh are visible).
#
# SPDX-License-Identifier: curl
FROM debian:bookworm-slim

ARG ARCH=x86_64
ARG ZLIB_VERSION=1.3.1
ARG OPENSSL_VERSION=3.5.1
ARG LIBNFS_REF=libnfs-6.0.2
ARG LIBSMB2_REF=5264e394d1f3046ac56b2f0bcdc375703320730e

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y --no-install-recommends \
      build-essential curl ca-certificates git perl make \
      autoconf automake libtool pkg-config xz-utils \
 && rm -rf /var/lib/apt/lists/*

# Prebuilt musl cross toolchain (static-friendly, no glibc/NSS baggage).
# If musl.cc is unreachable, swap the URL for a mirror such as https://more.musl.cc/10/x86_64-linux-musl/
RUN curl -fsSL "https://musl.cc/${ARCH}-linux-musl-cross.tgz" -o /tmp/tc.tgz \
 && tar -C /opt -xf /tmp/tc.tgz \
 && rm /tmp/tc.tgz
ENV PATH="/opt/${ARCH}-linux-musl-cross/bin:${PATH}"

ENV PLATFORM=linux \
    TARGET_NAME=linux-${ARCH} \
    TRIPLE=${ARCH}-linux-musl \
    OPENSSL_TARGET=linux-${ARCH} \
    PREFIX=/opt/prefix \
    CC=${ARCH}-linux-musl-gcc \
    CXX=${ARCH}-linux-musl-g++ \
    AR=${ARCH}-linux-musl-ar \
    RANLIB=${ARCH}-linux-musl-ranlib \
    STRIP=${ARCH}-linux-musl-strip \
    CROSS_PREFIX=${ARCH}-linux-musl- \
    EXE= \
    ZLIB_MODE=configure \
    EXTRA_LDFLAGS=-all-static \
    EXTRA_LIBS="-lpthread -ldl" \
    ZLIB_VERSION=${ZLIB_VERSION} \
    OPENSSL_VERSION=${OPENSSL_VERSION} \
    LIBNFS_REF=${LIBNFS_REF} \
    LIBSMB2_REF=${LIBSMB2_REF}

# deps first so editing build-curl.sh below does not bust the dependency layer
COPY build-deps.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/build-deps.sh && /usr/local/bin/build-deps.sh

COPY build-curl.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/build-curl.sh

WORKDIR /src
ENTRYPOINT ["/usr/local/bin/build-curl.sh"]
