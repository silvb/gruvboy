# Cross-compile environment for the Miyoo Mini.
# Uses the prebuilt toolchain from shauninman/miyoomini-toolchain-buildroot-aarch64,
# which matches the device's glibc and includes SDL 1.2 in its sysroot.
FROM debian/eol:buster-slim

ARG TOOLCHAIN_URL=https://github.com/shauninman/miyoomini-toolchain-buildroot-aarch64/releases/download/v0.0.3/miyoomini-toolchain.tar.xz
ARG TOOLCHAIN_SHA256=ad35e1e19e4741c08efe878909e3cd65b39b736c447b2b089cd9fe74d7bc8f5c

RUN apt-get -y update \
 && apt-get -y install --no-install-recommends ca-certificates curl make xz-utils \
 && rm -rf /var/lib/apt/lists/*

RUN test "$(uname -m)" = "aarch64" \
 && curl --fail --location --retry 3 --output /tmp/toolchain.tar.xz "$TOOLCHAIN_URL" \
 && echo "$TOOLCHAIN_SHA256  /tmp/toolchain.tar.xz" | sha256sum --check \
 && tar -xf /tmp/toolchain.tar.xz -C /opt \
 && rm /tmp/toolchain.tar.xz

ENV CROSS_COMPILE=/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-
ENV SYSROOT=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot
WORKDIR /work
