#!/bin/sh
# Run the decoder as ARM code (including the hand-written synth_arm.S) under qemu, inside the
# toolchain container, and check it's bit-identical to the portable C build that
# tools/mp3check.sh compared against ffmpeg. Run mp3check.sh first.
#   docker run --rm -v "$PWD":/work -w /work ipod-fw-toolchain tools/mp3check_arm.sh
set -e
B=firmware/build/mp3check
arm-linux-gnueabi-gcc -O2 -march=armv4t -marm -static -std=c11 -Ifirmware/src tools/mp3dec_test.c \
    firmware/src/mp3/mp3dec.c firmware/src/mp3/synth_arm.S $B/pow43.c -o $B/mp3dec_test.arm
for f in testdata/mp3/*.mp3; do
    b=$B/$(basename "$f" .mp3)
    qemu-arm $B/mp3dec_test.arm "$f" "$b.arm.raw" > /dev/null
    if cmp -s "$b.arm.raw" "$b.ours.raw"; then r=identical; else r=DIFFERENT; fi
    printf '%-22s ARM build vs C build: %s\n' "$(basename "$f")" "$r"
done
