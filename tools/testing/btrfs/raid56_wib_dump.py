#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Dump the RAID56 write-intent log blocks of btrfs device images or block
devices (see fs/btrfs/raid56-wib.h for the on-disk layout).

Usage: raid56_wib_dump.py <device-or-image>...

The checksum is verified with the checksum type of the primary superblock
(crc32c, xxhash64 if the xxhash module is available, sha256, blake2b).
"""

import hashlib
import struct
import sys

OFFSET = 512 * 1024
SLOT_SIZE = 4096
NR_SLOTS = 2
MAGIC = 0x4c49575f36354952
HEADER = struct.Struct("<32s16sQQIIQ6Q")   # csum, fsid, magic, seq, nr, shift, flags, reserved
# A block carries one of two entry layouts and says which in its flags.  The
# narrow one is what a block with nothing stale in it uses; the wide one adds
# the three fields that say WHICH SIDE of a stripe is wrong, which is the whole
# point of the record.  Parsing a wide block as narrow does not fail -- it
# silently reads every field from the wrong offset, which is worse than
# refusing, and is what this tool used to do to every block written since the
# format gained those fields.
ENTRY_NARROW = struct.Struct("<QQQ")          # bytenr, bitmap, error
ENTRY_WIDE = struct.Struct("<QQQQQQ")         # + stale, stale_par, gen
FLAG_STALE = 1 << 0                           # BTRFS_WIB_FLAG_STALE: wide entries
FLAGS_SUPPORTED = FLAG_STALE                  # BTRFS_WIB_FLAGS_SUPPORTED
# The last 8 bytes of a slot (BTRFS_WIB_TRAILER_OFFSET): BTRFS_WIB_TORN_MARKING
# when the writer marks the records that may hide a torn write.  Without it,
# the kernel reads every error record of the block as possibly torn.
TRAILER_OFFSET = SLOT_SIZE - 8
TORN_MARKING = 0x4b52414d4e524f54             # "TORNMARK"
SUPER_OFFSET = 64 * 1024
SUPER_MAGIC = b"_BHRfS_M"
CSUM_NAMES = {0: "crc32c", 1: "xxhash64", 2: "sha256", 3: "blake2b"}


def crc32c(data):
    # btrfs csum type 0 (crc32c) with the ~0 seed / final inversion, as in the kernel.
    try:
        import crc32c as _c
        return _c.crc32c(data).to_bytes(4, "little")
    except ImportError:
        pass
    poly = 0x82F63B78
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (poly & -(crc & 1))
    return (crc ^ 0xFFFFFFFF).to_bytes(4, "little")


def checksum(csum_type, data):
    """Return the checksum bytes, or None if the type cannot be computed here."""
    if csum_type == 0:
        return crc32c(data)
    if csum_type == 1:
        try:
            import xxhash
            return xxhash.xxh64(data).intdigest().to_bytes(8, "little")
        except ImportError:
            return None
    if csum_type == 2:
        return hashlib.sha256(data).digest()
    if csum_type == 3:
        return hashlib.blake2b(data, digest_size=32).digest()
    return None


def read_csum_type(f):
    f.seek(SUPER_OFFSET)
    sb = f.read(4096)
    if sb[0x40:0x48] != SUPER_MAGIC:
        return None
    return struct.unpack_from("<H", sb, 0xc4)[0]


def dump(path):
    with open(path, "rb") as f:
        csum_type = read_csum_type(f)
        if csum_type is None:
            print(f"{path}: no btrfs superblock, assuming crc32c")
            csum_type = 0
        for slot in range(NR_SLOTS):
            f.seek(OFFSET + slot * SLOT_SIZE)
            blk = f.read(SLOT_SIZE)
            csum, fsid, magic, seq, nr, shift, flags = HEADER.unpack_from(blk)[:7]
            if magic != MAGIC:
                print(f"{path} slot {slot}: no log block (magic 0x{magic:x})")
                continue
            calc = checksum(csum_type, blk[32:])
            if calc is None:
                state = f"unverified ({CSUM_NAMES.get(csum_type, csum_type)})"
            else:
                state = "OK" if csum[:len(calc)] == calc else "BAD"
            wide = bool(flags & FLAG_STALE)
            ent = ENTRY_WIDE if wide else ENTRY_NARROW
            marks = struct.unpack_from("<Q", blk, TRAILER_OFFSET)[0] == TORN_MARKING
            print(f"{path} slot {slot}: seq {seq} entries {nr} block_shift {shift} "
                  f"flags 0x{flags:x} {'wide' if wide else 'narrow'} "
                  f"fsid {fsid.hex()} csum {state} "
                  f"{'marks-torn' if marks else 'unmarked (error records read as possibly torn)'}")
            # Refuse rather than guess, which is what the kernel does with a
            # flag it does not know (BTRFS_WIB_FLAGS_SUPPORTED).  A bit we have
            # never seen may well move the fields we are about to read, and a
            # confident dump of the wrong offsets is the failure mode this tool
            # exists to avoid -- it is the only one that works when the
            # filesystem will not mount.
            if flags & ~FLAGS_SUPPORTED:
                print(f"    UNSUPPORTED flag bits 0x{flags & ~FLAGS_SUPPORTED:x}: "
                      f"refusing to decode entries, the layout may have changed")
                continue
            for i in range(min(nr, (SLOT_SIZE - HEADER.size) // ent.size)):
                vals = ent.unpack_from(blk, HEADER.size + ent.size * i)
                bytenr, bitmap, error = vals[0], vals[1], vals[2]
                stale, stale_par, gen = (vals[3], vals[4], vals[5]) if wide else (0, 0, 0)
                blocks = [bytenr + (b << shift) for b in range(64) if (bitmap | error) & (1 << b)]
                print(f"    region {bytenr} inflight 0x{bitmap:016x} error 0x{error:016x} "
                      f"-> {len(blocks)} dirty 64K blocks"
                      + (f" starting at {blocks[0]}" if blocks else ""))
                if wide:
                    # stale names the data column whose write did not land;
                    # stale_par says the parity does not describe the data.
                    # Those need OPPOSITE repairs, so a dump that cannot tell
                    # them apart cannot be used to decide anything.
                    print(f"      stale 0x{stale:016x} stale_par 0x{stale_par:016x} gen {gen}")
                    # raid56-wib.h states @stale is a strict subset of @error.
                    # Checking it here does double duty: it catches a record
                    # the kernel should never have written, and it catches THIS
                    # TOOL reading at the wrong offsets -- a misparse would
                    # almost certainly break it, so a clean run is evidence the
                    # layout above is the one on the disk.
                    if stale & ~error:
                        print(f"      INCONSISTENT: stale has bits outside error "
                              f"(0x{stale & ~error:016x}); the record is corrupt "
                              f"or this tool is decoding the wrong layout")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        dump(p)
