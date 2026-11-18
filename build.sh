#!/bin/sh
# Build the firmware in the toolchain container.  ./build.sh [make args]   e.g. ./build.sh clean all
set -e
cd "$(dirname "$0")"
docker image inspect ipod-fw-toolchain >/dev/null 2>&1 || docker build -q -t ipod-fw-toolchain toolchain/
exec docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/work -w /work ipod-fw-toolchain make -C firmware "$@"
