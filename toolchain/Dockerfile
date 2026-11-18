# Build environment for the hand-rolled iPod Video firmware.
#   docker build -t ipod-fw-toolchain toolchain/
#   (gcc-arm-linux-gnueabi + qemu-user: run the ARM decoder code on the build machine, tools/mp3check.sh)
#   docker run --rm -u 1000:1000 -v "$PWD":/work -w /work ipod-fw-toolchain make -C firmware
FROM ubuntu:24.04
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi \
      make python3 python3-pil dosfstools mtools gcc libc6-dev \
      gcc-arm-linux-gnueabi libc6-dev-armel-cross qemu-user \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /work
