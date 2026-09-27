#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# What 'btrfs rescue zero-log' does, for a filesystem the btrfs-progs of the
# rig refuse to write: they predate the raid56_write_intent feature
# (BTRFS_FEATURE_COMPAT_RO_RAID56_WRITE_INTENT), and open no filesystem that
# has it for writing ("couldn't open RDWR because of unsupported option
# features").  One that knows it clears the tree log the same way.
#
#   zero_log.py <device>...
#
# Every copy of the superblock on every device given: log_root and
# log_root_level set to 0, the crc32c checksum recomputed.  Prints the log
# root it cleared.
import os
import struct
import sys

SUPER = [65536, 64 << 20, 256 << 30]
SIZE = 4096
MAGIC = b'_BHRfS_M'


def crc32c_table():
    t = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        t.append(c)
    return t


TABLE = crc32c_table()


def crc32c(data):
    c = 0xFFFFFFFF
    for b in data:
        c = TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


cleared = 0
for dev in sys.argv[1:]:
    with open(dev, 'r+b') as f:
        f.seek(0, 2)
        size = f.tell()
        for off in SUPER:
            if off + SIZE > size:
                continue
            f.seek(off)
            sb = bytearray(f.read(SIZE))
            if sb[64:72] != MAGIC:
                continue
            if struct.unpack_from('<H', sb, 196)[0] != 0:
                sys.exit('%s: not a crc32c checksum' % dev)
            if struct.unpack_from('<I', sb, 0)[0] != crc32c(sb[32:]):
                sys.exit('%s: bad superblock checksum at %d' % (dev, off))
            root = struct.unpack_from('<Q', sb, 96)[0]
            struct.pack_into('<Q', sb, 96, 0)
            sb[200] = 0
            sb[0:32] = bytes(32)
            struct.pack_into('<I', sb, 0, crc32c(sb[32:]))
            f.seek(off)
            f.write(sb)
            cleared += 1
            print('%s: superblock at %d, log_root %d cleared' % (dev, off, root))
        f.flush()
        os.fsync(f.fileno())
sys.exit(0 if cleared else 'no superblock found')
