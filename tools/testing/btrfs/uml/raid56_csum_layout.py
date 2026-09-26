#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Where does a checksummed block share a RAID5/6 full stripe with nodatacow
# files?
#
#   raid56_csum_layout.py <csum-file> <b-file> <c-file> <device>
#
# Runs in the guest.  Takes the full stripe holding the first block of
# <csum-file> (column B, row S_ROW of it) and prints shell assignments: the
# file offsets in <b-file> of the other rows of column B that it holds (B_ROWS,
# space separated), the file offset in <c-file> of the block of another data
# column in the same row as the checksummed block (FO_C, column C), and the
# index of the /dev/mapper/dN device holding column B (IDX_B), column C
# (IDX_C), P (IDX_P) and, on RAID6, Q (IDX_Q), with the physical offset of Q's
# sector in the checksummed block's row (PHYS_Q).  The geometry is
# raid56_layout.py's.
import re
import subprocess
import sys

csum_path, b_path, c_path, dev = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
SL = 65536
BS = 4096


def extents(path):
    ext = []
    out = subprocess.run(['filefrag', '-v', '-b4096', path],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r'\s*\d+:\s+(\d+)\.\.\s*(\d+):\s+(\d+)\.\.\s*(\d+):', line)
        if m:
            f0, f1, p0, _ = (int(x) * BS for x in m.groups())
            ext.append((f0, f1 + BS, p0))
    return ext


def file_offset(ext, logical):
    for f0, f1, p0 in ext:
        if p0 <= logical < p0 + (f1 - f0):
            return f0 + logical - p0
    return None


cext, bext, cxt = extents(csum_path), extents(b_path), extents(c_path)
if not cext or not bext or not cxt:
    sys.exit('no extents')
ls = cext[0][2]

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

devpath = {}
show = subprocess.run(['btrfs', 'filesystem', 'show', dev], capture_output=True,
                      text=True).stdout
for m in re.finditer(r'devid\s+(\d+)\s+size.*?path\s+(\S+)', show):
    devpath[int(m.group(1))] = m.group(2)

for c in chunks:
    if 'len' not in c or not (c['start'] <= ls < c['start'] + c['len']):
        continue
    npar = 2 if 'RAID6' in c['type'] else 1
    num = len(c['stripes'])
    nd = num - npar
    fsl = nd * SL
    n = (ls - c['start']) // fsl
    fss = c['start'] + n * fsl
    col = (ls - fss) // SL
    row = (ls - fss) % SL // BS

    def idx(k):
        d = devpath[c['stripes'][k % num][0]]
        return re.search(r'(\d+)$', d).group(1)

    fo_c = col_c = None
    for cc in range(nd):
        if cc == col:
            continue
        fo = file_offset(cxt, fss + cc * SL + row * BS)
        if fo is not None:
            fo_c, col_c = fo, cc
            break
    rows = [file_offset(bext, fss + col * SL + r * BS) for r in range(SL // BS) if r != row]
    rows = [str(r) for r in rows if r is not None]
    if fo_c is None or not rows:
        sys.exit(f'full stripe {fss}: {c_path} holds no block of another column in '
                 f'row {row}, or {b_path} no other row of column {col} (checksummed '
                 f'block at {ls}, extents {bext[:2]} {cxt[:2]})')
    print(f'FULL={fss} NDATA={nd} S_ROW={row} FO_C={fo_c} B_ROWS="{" ".join(rows)}" '
          f'IDX_B={idx(n + col)} IDX_C={idx(n + col_c)} IDX_P={idx(n + nd)}'
          + (f' IDX_Q={idx(n + nd + 1)} '
             f'PHYS_Q={c["stripes"][(n + nd + 1) % num][1] + n * SL + row * BS}'
             if npar == 2 else ''))
    break
else:
    sys.exit('file not in any chunk')
