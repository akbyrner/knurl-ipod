#!/usr/bin/env python3
"""Build a raw disk image like an iPod's: an MBR with one FAT32 partition, holding a folder.

    make_test_image.py SOURCE_DIR IMAGE [SIZE_MB] [--units4]

--units4 writes the partition start in 2048-byte units, like an iTunes-formatted iPod,
to exercise the firmware's fallback. Needs mkfs.vfat and mtools (in the toolchain image).
"""
import os
import struct
import subprocess
import sys

src, img = sys.argv[1], sys.argv[2]
size_mb = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else 64
units4 = "--units4" in sys.argv
start = 2048                                   # in 512-byte sectors (1 MiB)
total = size_mb * 2048

with open(img, "wb") as f:
    f.truncate(total * 512)
    mbr = bytearray(512)
    s = start // 4 if units4 else start
    n = (total - start) // 4 if units4 else total - start
    mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\0\0\0", 0x0C, b"\0\0\0", s, n)
    mbr[510:512] = b"\x55\xaa"
    f.seek(0)
    f.write(mbr)

subprocess.run(["mkfs.vfat", "-F", "32", "-n", "TESTPOD", "--offset", str(start), img, str((total - start) // 2)],
               check=True, stdout=subprocess.DEVNULL)
target = f"{img}@@{start * 512}"
env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
for name in sorted(os.listdir(src)):
    subprocess.run(["mcopy", "-s", "-Q", "-i", target, os.path.join(src, name), "::"], check=True, env=env)
print(f"{img}: {size_mb} MB, FAT32 at sector {start}{' (table in 2048-byte units)' if units4 else ''}, from {src}")
