#!/usr/bin/env python3
# Is each acknowledged block of the nocow file on the platter where the
# filesystem says it lives?
#
#   nocow_platter.py <file> <blocks> <stride> <device>
#
# Runs in the guest against a read-only mount.  The file's extents come from
# filefrag, each block's physical location from btrfs-map-logical, and the bytes
# straight off that device -- not through the filesystem, whose read path
# consults the write-intent record and reconstructs a recorded block.  That is
# right for a reader and useless for asking whether a column was rewritten.
#
# Prints one line per block that is not 'B' on its platter, then
# "PLATTER_MISSING <n> of <blocks>".  A block that cannot be located is
# counted missing: a probe that cannot look is not a probe that found nothing.
import re
import subprocess
import sys

path, blocks, stride, dev = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
BS = 4096
# Optional: the overwrites that were acknowledged.  Only those have to be 'B';
# a refused one may be the old 'A' or the new 'B', nothing else.
acked = None
if len(sys.argv) > 5:
    try:
        acked = {int(x) for x in open(sys.argv[5]).read().split()}
    except OSError:
        acked = None

ext = []
for line in subprocess.run(['filefrag', '-v', '-b%d' % BS, path],
                           capture_output=True, text=True).stdout.splitlines():
    m = re.match(r'\s*\d+:\s+(\d+)\.\.\s*(\d+):\s+(\d+)\.\.\s*(\d+):', line)
    if m:
        ext.append(tuple(int(x) for x in m.groups()))

# btrfs-map-logical only answers for a range that starts an extent item, so
# ask for each whole extent and pick the piece a block falls in.  It prints one
# "mirror 1" line per contiguous piece, each within one data stripe.
pieces = {}


def extent_pieces(p0, nblk):
    if p0 not in pieces:
        r = subprocess.run(['btrfs-map-logical', '-l', str(p0 * BS), '-b',
                            str(nblk * BS), dev], capture_output=True, text=True)
        pieces[p0] = sorted(
            (int(a), int(b), c) for a, b, c in
            re.findall(r'mirror 1 logical (\d+) physical (\d+) device (\S+)',
                       r.stdout))
    return pieces[p0]


missing = 0
for i in range(blocks):
    fblk = i * stride
    hit = None
    for f0, f1, p0, _ in ext:
        if f0 <= fblk <= f1:
            hit = (p0, f1 - f0 + 1, (p0 + fblk - f0) * BS)
            break
    if hit is None:
        print('UNMAPPED block %d' % i)
        missing += 1
        continue
    p0, nblk, loc = hit
    cand = [pc for pc in extent_pieces(p0, nblk) if pc[0] <= loc]
    if not cand or loc - cand[-1][0] >= 65536:
        print('NOMAP block %d logical %d' % (i, loc))
        missing += 1
        continue
    logical, phys, pdev = cand[-1]
    phys += loc - logical
    with open(pdev, 'rb') as f:
        f.seek(phys)
        data = f.read(BS)
    if data != b'B' * BS and not (acked is not None and i not in acked and
                                  data == b'A' * BS):
        print('STALE block %d logical %d on %s at %d (%d of %d bytes are B)'
              % (i, loc, pdev, phys, data.count(b'B'), BS))
        missing += 1
print('PLATTER_MISSING %d of %d' % (missing, blocks))
