#!/usr/bin/env python3
# Where does a file's first whole full stripe live?
#
#   raid56_layout.py <file> <device>
#
# Runs in the guest.  Prints shell assignments for the first RAID5/6 full
# stripe lying entirely inside the file's first extent: the file offset of
# its data column 0 (FO_B) and column 1 (FO_C), the index of the /dev/mapper/dN
# device holding column 0 (IDX_B) and the parity P (IDX_P), and the physical
# offset of column 0's first sector on its device (PHYS_B).  The geometry is
# btrfs_map_block()'s: data column c of full stripe n on stripe (n + c) %
# num_stripes, P after the data.  IDX_C is the device of column 1, DEV_C its
# path and PHYS_C the physical offset of its first sector (the same row).
# DEV_P and PHYS_P are the same for the parity P.  On RAID6 also IDX_Q, DEV_Q
# and PHYS_Q: the same for Q, the stripe after P.  With three data columns or
# more also FO_D, IDX_D, DEV_D and PHYS_D: the same for column 2.
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
    fss = c['start'] + -(-(p0 - c['start']) // fsl) * fsl   # first full stripe at or after p0
    if fss + fsl > p0 + (f1 - f0):
        sys.exit('no whole full stripe inside the first extent')
    row = (fss - c['start']) // fsl
    def idx(k):
        d = devpath[c['stripes'][k][0]]
        m = re.search(r'(\d+)$', d)
        return m.group(1)
    ib, ic, ip = (row + 0) % num, (row + 1) % num, (row + nd) % num
    iq = (row + nd + 1) % num
    print(f'FULL={fss} NDATA={nd} FO_B={f0 + fss - p0} FO_C={f0 + fss - p0 + SL} '
          f'IDX_B={idx(ib)} IDX_C={idx(ic)} IDX_P={idx(ip)} DEV_B={devpath[c["stripes"][ib][0]]} '
          f'PHYS_B={c["stripes"][ib][1] + row * SL} '
          f'DEV_C={devpath[c["stripes"][ic][0]]} PHYS_C={c["stripes"][ic][1] + row * SL} '
          f'DEV_P={devpath[c["stripes"][ip][0]]} PHYS_P={c["stripes"][ip][1] + row * SL}'
          + (f' IDX_Q={idx(iq)} DEV_Q={devpath[c["stripes"][iq][0]]} '
             f'PHYS_Q={c["stripes"][iq][1] + row * SL}' if npar == 2 else '')
          + (f' FO_D={f0 + fss - p0 + 2 * SL} IDX_D={idx((row + 2) % num)} '
             f'DEV_D={devpath[c["stripes"][(row + 2) % num][0]]} '
             f'PHYS_D={c["stripes"][(row + 2) % num][1] + row * SL}' if nd >= 3 else ''))
    break
else:
    sys.exit('file not in any chunk')
