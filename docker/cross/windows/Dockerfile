# Cross-compile curl for Windows x86_64 as a static .exe using mingw-w64.
# Build context must be docker/cross/.
#
# SPDX-License-Identifier: curl
FROM debian:bookworm-slim

ARG ZLIB_VERSION=1.3.1
ARG OPENSSL_VERSION=3.5.1
ARG LIBNFS_REF=libnfs-6.0.2
ARG LIBSMB2_REF=5264e394d1f3046ac56b2f0bcdc375703320730e

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y --no-install-recommends \
      build-essential curl ca-certificates git perl make \
      autoconf automake libtool pkg-config xz-utils \
      gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 mingw-w64-tools \
 && rm -rf /var/lib/apt/lists/*

ENV PLATFORM=windows \
    TARGET_NAME=windows-x86_64 \
    TRIPLE=x86_64-w64-mingw32 \
    OPENSSL_TARGET=mingw64 \
    PREFIX=/opt/prefix \
    CC=x86_64-w64-mingw32-gcc \
    CXX=x86_64-w64-mingw32-g++ \
    AR=x86_64-w64-mingw32-ar \
    RANLIB=x86_64-w64-mingw32-ranlib \
    STRIP=x86_64-w64-mingw32-strip \
    CROSS_PREFIX=x86_64-w64-mingw32- \
    OPENSSL_CROSS_PREFIX=x86_64-w64-mingw32- \
    EXE=.exe \
    ZLIB_MODE=win32gcc \
    EXTRA_LDFLAGS=-all-static \
    EXTRA_LIBS="-lws2_32 -lcrypt32 -lbcrypt -liphlpapi -ladvapi32" \
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
