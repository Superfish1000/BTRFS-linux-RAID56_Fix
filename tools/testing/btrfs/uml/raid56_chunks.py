#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Where is every chunk of a filesystem?
#
#   raid56_chunks.py <device>
#
# Runs in the guest, with the filesystem's devices scanned (mounted or not).
# Prints one line per chunk, in logical order:
#
#   <logical start> <length> <type> <nr stripes> <devid>:<path>:<physical> ...
#
# one <devid>:<path>:<physical> triple per chunk stripe, in stripe order: the
# device holding it and where that stripe starts on it.  For RAID5/6, data
# column c of full stripe n is on chunk stripe (n + c) % nr_stripes, P after
# the data (raid56_layout.py).
import re
import subprocess
import sys

dev = sys.argv[1]
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

for c in sorted(chunks, key=lambda c: c['start']):
    if 'len' not in c:
        continue
    print(c['start'], c['len'], c['type'], len(c['stripes']),
          ' '.join(f'{d}:{devpath.get(d, "?")}:{o}' for d, o in c['stripes']))
