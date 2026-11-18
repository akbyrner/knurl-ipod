#!/usr/bin/env python3
"""Wrap a raw firmware image in the header the Rockbox bootloader expects for rockbox.ipod:
4-byte big-endian checksum, then the model tag "ipvd", then the image. The checksum is the
iPod Video's model number (5) plus the sum of every image byte, modulo 2^32. (Verified
against the real rockbox.ipod on the iPod.)"""
import struct
import sys

MODEL_NUMBER, MODEL_TAG = 5, b"ipvd"


def wrap(image: bytes) -> bytes:
    checksum = (MODEL_NUMBER + sum(image)) & 0xFFFFFFFF
    return struct.pack(">I", checksum) + MODEL_TAG + image


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    image = open(src, "rb").read()
    data = wrap(image)
    open(dst, "wb").write(data)
    print(f"{dst}: {len(image):,} byte image, checksum 0x{struct.unpack('>I', data[:4])[0]:08X}")
