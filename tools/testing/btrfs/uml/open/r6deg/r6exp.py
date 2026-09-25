#!/usr/bin/env python3
# Expected content of a file's 4K blocks after a series of in-place overwrites.
#
#   r6exp.py record <db> <offset> <content-file> <acked 0|1>
#   r6exp.py check  <db> <file> <original-copy>
#
# An acknowledged overwrite must read back exactly; a refused one promised
# nothing, so the block may hold what it held before it (anything since the
# last acknowledged value) or the refused content -- nothing else.  check
# prints "ok=N eio=N silent=N" and the first few bad blocks.
import json
import os
import sys

BS = 4096
cmd, db = sys.argv[1], sys.argv[2]
hist = json.load(open(db)) if os.path.exists(db) else {}
if cmd == 'record':
    off, path, acked = int(sys.argv[3]), sys.argv[4], sys.argv[5] == '1'
    data = open(path, 'rb').read(BS)
    hist.setdefault(str(off), []).append([data.hex(), acked])
    json.dump(hist, open(db, 'w'))
    sys.exit(0)
path, orig_path = sys.argv[3], sys.argv[4]
orig = open(orig_path, 'rb').read()
fd = os.open(path, os.O_RDONLY)
ok, eio, silent = 0, [], []
for b in range(len(orig) // BS):
    off = b * BS
    allowed = [orig[off:off + BS]]
    for data, acked in hist.get(str(off), []):
        d = bytes.fromhex(data)
        allowed = [d] if acked else allowed + [d]
    try:
        got = os.pread(fd, BS, off)
    except OSError:
        eio.append(b)
        continue
    if got in allowed:
        ok += 1
    else:
        silent.append(b)
print(f"ok={ok} eio={len(eio)} silent={len(silent)} eio_blocks={eio[:10]} silent_blocks={silent[:10]}")
