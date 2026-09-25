#!/usr/bin/env python3
# Where does a file's first whole RAID5/6 full stripe live, member by member?
#
#   r6deg_layout.py <file> <device>
#
# Runs in the guest.  Derived from ../../raid56_layout.py; prints, as shell
# assignments, the full stripe's logical start (FULL), its data column count
# (NDATA), the file offset of data column 0 (FO0), and the /dev/mapper/dN index
# of every member: D0..D<n-1> for the data columns, P and Q.  Geometry as in
# btrfs_map_block(): data column c of full stripe n on stripe (n + c) % num,
# P after the data, Q after P.
import re
import subprocess
import sys

path, dev = sys.argv[1], sys.argv[2]
SL = 65536

ext = []
for line in subprocess.run(['filefrag', '-v', '-b4096', path],
                           capture_output=True, text=True).stdout.splitlines():
    m = re.match(r'\s*\d+:\s+(\d+)\.\.\s*(\d+):\s+(\d+)\.\.\s*(\d+):', line)
    if m:
        ext.append(tuple(int(x) * 4096 for x in m.groups()))
f0, f1, p0, _ = ext[0]
f1 += 4096

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
    if 'len' not in c or not (c['start'] <= p0 < c['start'] + c['len']):
        continue
    npar = 2 if 'RAID6' in c['type'] else 1
    num = len(c['stripes'])
    nd = num - npar
    fsl = nd * SL
    fss = c['start'] + -(-(p0 - c['start']) // fsl) * fsl
    if fss + fsl > p0 + (f1 - f0):
        sys.exit('no whole full stripe inside the first extent')
    row = (fss - c['start']) // fsl

    def idx(k):
        d = devpath[c['stripes'][k][0]]
        return re.search(r'(\d+)$', d).group(1)
    parts = [f'FULL={fss}', f'NDATA={nd}', f'FO0={f0 + fss - p0}', f'ROW={row}']
    for col in range(nd):
        parts.append(f'D{col}={idx((row + col) % num)}')
    parts.append(f'P={idx((row + nd) % num)}')
    if npar == 2:
        parts.append(f'Q={idx((row + nd + 1) % num)}')
    # Every further whole full stripe inside the first extent, as
    # S<k>=<file offset of column 0>:<data devices...>:<P>[:<Q>]
    nstripes = 0
    for k in range(int(sys.argv[3]) if len(sys.argv) > 3 else 0):
        fo = f0 + fss - p0 + k * fsl
        if fo + fsl > f1 - f0 + f0:
            break
        rk = row + k
        mem = [idx((rk + col) % num) for col in range(nd)] + \
              [idx((rk + nd + p) % num) for p in range(npar)]
        parts.append(f'S{k}={fo}:' + ':'.join(mem))
        nstripes += 1
    parts.append(f'NSTRIPES={nstripes}')
    print(' '.join(parts))
    break
else:
    sys.exit('file not in any chunk')
