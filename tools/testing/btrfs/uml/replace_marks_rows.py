#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Pick the rows of replace_marks_kept's stale and resume arms.
#
#   replace_marks_rows.py <pick> <regions> <cap> <nr-late> <score> <fill> <late>
#
# <pick> has one line per row with data on device 3, as init-final3.sh writes
# it ("<row> <file block> <sibling device> <its offset> <device 3 offset>");
# <regions> is raid56_rows.py's output with "region" for the same rows, whose
# fifth field says which write-intent log regions (4 MiB of logical address
# space) the row's full stripe covers.  Writes to <score> the two rows lowest
# on device 3, which the replace reaches first; to <late> the <nr-late> rows
# highest on device 3, which it reaches last; and to <fill> rows whose regions
# all lie after the first two's, in logical order, until those rows and the
# first two cover exactly <cap> regions -- a full log once each is written
# into, with the first two's records first in the table (slots are taken in
# the order of the writes, and in logical order when the log is reloaded).
# Prints what it chose; exits 1 if the rows do not cover <cap> regions, or if
# fewer than two late rows need a region of their own.
import sys

pick_f, regs_f, cap, nr_late, score_f, fill_f, late_f = sys.argv[1:8]
cap, nr_late = int(cap), int(nr_late)

pick, order = {}, []
for line in open(pick_f):
    f = line.split()
    if len(f) < 5:
        continue
    pick[int(f[0])] = line
    order.append((int(f[4]), int(f[0])))
regs = {}
for line in open(regs_f):
    f = line.split()
    if len(f) < 5 or ':' not in f[4]:
        continue
    r, s = (int(x) for x in f[4].split(':'))
    regs[int(f[0])] = {r, r + 1} if s else {r}

order.sort()
first = [i for _, i in order[:2]]
late = [i for _, i in reversed(order) if i not in first][:nr_late]
if len(first) < 2 or any(i not in regs for i in first + late):
    print('rows or regions missing')
    sys.exit(1)
have = set().union(*(regs[i] for i in first))
top = max(have)
fill = []
cands = sorted((i for i in pick if i in regs and i not in first and i not in late and
                min(regs[i]) > top), key=lambda i: min(regs[i]))
for i in cands:
    new = regs[i] - have
    if len(have) + len(new) > cap:
        continue
    have |= new
    fill.append(i)
    if len(have) == cap:
        break
own = [i for i in late if not regs[i] <= have]
with open(score_f, 'w') as out:
    out.writelines(pick[i] for i in first)
with open(fill_f, 'w') as out:
    out.writelines(pick[i] for i in fill)
with open(late_f, 'w') as out:
    out.writelines(pick[i] for i in late)
print(f'first rows {first} (regions {sorted(set().union(*(regs[i] for i in first)))}),'
      f' {len(fill)} fill rows, {len(have)} of {cap} regions, late rows {late}'
      f' ({len(own)} needing a region of their own)')
sys.exit(0 if len(have) == cap and len(own) >= 2 else 1)
