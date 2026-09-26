#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Which members hold the row of each of a file's overwritten blocks?
#
#   raid56_rows.py <file> <device> <stride> <count> [phys]
#
# Runs in the guest, with every device present.  For block i * stride of the
# file (4 KiB blocks, i < count) prints one line:
#
#   <i> <file block> <its data column> <parity device> <b>:<d> <b>:<d> ...
#
# then one <file block>:<device> pair per data column of the same full stripe,
# in column order: the sector of that column in the same vertical stripe (the
# row a sub-stripe write of the block rewrites the parity of), and the
# /dev/mapper/dN index of the device it is on.  A sector outside the file is
# -1.  With "phys", each pair is <b>:<d>:<physical offset of that sector on its
# device>, and the parity device is followed by P's physical offset in the
# same row.  The geometry is btrfs_map_block()'s, as in raid56_layout.py: data
# column c of full stripe n on chunk stripe (n + c) % num_stripes, P after the
# data, Q after P.  With "region" instead, the parity device is followed by
# <r>:<s>: the write-intent log region (4 MiB of logical address space) the
# full stripe starts in, and 1 if it runs into the next one, else 0.
import re
import subprocess
import sys

path, dev, stride, count = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
phys = len(sys.argv) > 5 and sys.argv[5] == 'phys'
region = len(sys.argv) > 5 and sys.argv[5] == 'region'
RS = 4 << 20
SL = 65536
BS = 4096

ext = []
for line in subprocess.run(['filefrag', '-v', '-b%d' % BS, path],
                           capture_output=True, text=True).stdout.splitlines():
    m = re.match(r'\s*\d+:\s+(\d+)\.\.\s*(\d+):\s+(\d+)\.\.\s*(\d+):', line)
    if m:
        f0, f1, p0, _ = (int(x) for x in m.groups())
        ext.append((f0, f1, p0))


def logical_of(fblk):
    for f0, f1, p0 in ext:
        if f0 <= fblk <= f1:
            return (p0 + fblk - f0) * BS
    return None


def fblk_of(logical):
    blk = logical // BS
    for f0, f1, p0 in ext:
        if p0 <= blk <= p0 + f1 - f0:
            return f0 + blk - p0
    return -1


out = subprocess.run(['btrfs', 'inspect-internal', 'dump-tree', '-t', 'chunk', dev],
                     capture_output=True, text=True).stdout
chunks, cur = [], None
for line in out.splitlines():
    m = re.search(r'CHUNK_ITEM (\d+)\)', line)
    if m:
        cur = {'start': int(m.group(1)), 'stripes': []}
        chunks.append(cur)
        continue
    if cur is None:
        continue
    m = re.search(r'length (\d+) owner \d+ stripe_len (\d+) type (\S+)', line)
    if m:
        cur['len'], cur['type'] = int(m.group(1)), m.group(3)
    m = re.search(r'stripe (\d+) devid (\d+) offset (\d+)', line)
    if m:
        cur['stripes'].append((int(m.group(2)), int(m.group(3))))

devidx = {}
show = subprocess.run(['btrfs', 'filesystem', 'show', dev], capture_output=True,
                      text=True).stdout
for m in re.finditer(r'devid\s+(\d+)\s+size.*?path\s+(\S+)', show):
    d = re.search(r'(\d+)$', m.group(2))
    devidx[int(m.group(1))] = int(d.group(1)) if d else -1

for i in range(count):
    fblk = i * stride
    L = logical_of(fblk)
    if L is None:
        print(f'{i} {fblk} -1 -1')
        continue
    for c in chunks:
        if 'len' in c and c['start'] <= L < c['start'] + c['len'] and \
           ('RAID5' in c['type'] or 'RAID6' in c['type']):
            break
    else:
        print(f'{i} {fblk} -1 -1')
        continue
    npar = 2 if 'RAID6' in c['type'] else 1
    num = len(c['stripes'])
    nd = num - npar
    fsl = nd * SL
    n = (L - c['start']) // fsl
    fs = c['start'] + n * fsl
    col = (L - fs) // SL
    off = (L - fs) % SL

    def idx(k):
        return devidx.get(c['stripes'][k][0], -1)

    def at(k):
        return f':{c["stripes"][k][1] + n * SL + off}' if phys else ''
    mates = ' '.join(f'{fblk_of(fs + d * SL + off)}:{idx((n + d) % num)}{at((n + d) % num)}'
                     for d in range(nd))
    reg = f' {fs // RS}:{int((fs + fsl - 1) // RS != fs // RS)}' if region else ''
    print(f'{i} {fblk} {col} {idx((n + nd) % num)}{at((n + nd) % num)}{reg} {mates}')
