#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
a2_model.py -- attack copy of policy_model.py (md5 3ed8fa4f) for policy A2.
Everything below the next paragraph is the original description; the A2
attack extensions are listed in A2_ATTACK_DOC at the end of this docstring.

policy_model.py -- one exhaustive model that compares what btrfs RAID5/6 does
when a device misbehaves, under six policies, and counts every way each one
can lose or silently corrupt acknowledged data, refuse work, or wedge.

Policies (--policy):
  OLD  upstream btrfs, no write-intent log: nothing records a torn parity or a
       stale member; no mount recovery; scrub trusts nodatasum data.
  A    strict refusal: the log keeps every record that protects acknowledged
       data; a full log with nothing droppable fails the write (EIO, or RO for
       metadata); a phase-A repair that cannot write back refuses the write;
       reads the record cannot vouch for fail with EIO.
  CUR  the stage-0 kernel (raid56-wib.c at BTRFS-s0 stage0-wip): A, except a
       full log evicts records (wib_evict_sticky): pass 0 vague records;
       pass 1 records naming members while a device is missing or during
       tree-log replay (wib_may_evict_naming), else torn-only records
       (wib_entry_torn_only); pass 3 recovery verdicts (wib_entry_verdict),
       with the record_dropped alert.
  A2   A, plus the one allowed drop: a record whose names are ALL on devices
       that are missing now (not torn, not a verdict); the drop persistently
       taints those devices, whose content is then never trusted (treated as
       missing for reads, rebuild sources and copies) until a scrub rewrote
       it all, or a replace.
  B    A, but a phase-A write-back that the stripe's own disk refuses does not
       refuse the write: that column is treated as missing for the stripe
       (its record keeps naming it) and phase B goes on.
  C    stage 1 of the failed-disk design: A until the trigger; the trigger
       (repeated/persistent write errors, failed flush, log pressure, admin)
       marks the device FAILED durably; it is then skipped by every write
       quietly (no records), its names are forgotten, its checksummed sectors
       are served only when they verify, its nodatasum sectors and parity are
       never used; replace restores it.  Log full with a device MISSING:
       ERRATA 2 of the design keeps HEAD's exception for records naming only
       missing devices (dropped with the alert, no taint).
  CB   C as the design's section 2.1 writes it: phase A never refuses (B's
       degrade) before the trigger, then C.
  CTM  C plus the design's optional T-missing: a missing device becomes FAILED
       at once (reason MISSING), so records never need to name it and it is
       distrusted when it returns.

Knobs (--mut).  Two refinements, measured as variants:
  strict_degraded    refuse an RMW that must rebuild an absent (missing,
                     failed, tainted) data column holding committed data:
                     no write while degraded can open the degraded write hole
  narrow_inflight    the in-flight mark is written with phase A's persist
                     (just before phase B) and names only the data column
                     phase B writes; recovery trusts a one-parity rebuild whose
                     holes contain every such column (old or new value)
Mutations (negative controls; each must produce its failure class):
  evict_naming       A/A2/B/C: a full log evicts records naming members
  a2_no_taint        A2: drop without tainting the device
  c_trust_unverified C/CTM: nodatasum reads and replace copies of a FAILED
                     device are served from it unverified
  no_suspect         mount recovery of a torn stripe with an absent nodatasum
                     column trusts the rebuild (no verdict, spec 3.5 off)
  read_trust_torn    reads return an unchecked rebuild of a torn stripe
  absent_par_unnamed recovery of a torn stripe does not name a missing parity
  ack_unnamed        a strict policy acknowledges a flush loss the log cannot
                     name (as CUR does) instead of aborting the commit
  b_no_record        B: the degraded column is not kept named

Model (values, not fault counts): nd=2 data columns, npar=1 (RAID5) or 2
(RAID6) parity columns, one device per column, --stripes full stripes with
one sector per column.  A data cell holds an int; a parity holds the SNAPSHOT
of the data vector it was computed from, so a rebuild is right iff the other
cells still equal that snapshot, else a tagged garbage value.  acc[s][i] is
the set of values a read may return (the last acknowledged value; {old,new}
while a write is unacknowledged or after a crash interrupted it).  A write is
an RMW followed by its commit (fsync): mark -> phase A (write back what the
record names stale, FUA, persist) -> phase B (new data + every parity, each a
separate device write) -> completion (tolerance check, record update) ->
commit (barrier flush; a failed flush loses that device's writes of the op;
the log block; the acknowledgement).  A crash may hit after the mark, after
phase A, or after ANY subset of phase B's device writes landed; the mount
that follows may find a device missing or a missing one returned, and may
have a tree-log replay write to do (--replay).  Log records are per full
stripe: (torn, names, verdict); names are column indices (data = stale data,
parity = stale parity).  --cap is the number of records the log holds.

Metrics (every reachable state is quiescent; counts are distinct states or
distinct transitions):
  SILENT_WRONG  a read returns a value outside acc without an error
  SILENT_NOALERT  ... and no record_dropped alert was ever raised
  LOST_ACKED    no sequence of admin actions (heal, return, pull, remount,
                scrub, replace, fail) of length <= --admin-depth reaches a
                state where every acknowledged cell reads back correctly
  LOST_INFO     the acknowledged value is gone from the disks altogether (no
                direct copy, no parity combination yields it): an oracle bound
  REFUSED_WRITE transitions where a write/replay failed with EIO
  RO            transitions to read-only (metadata write refused) or to a
                mount that fails (replay refused)
  REFUSED_READ  states with a read that fails although the oracle can
                recover the value (needless EIO)
  STUCK         not LOST_ACKED, but no admin sequence restores full write
                availability (rw, no failing device left in service, a write
                to every cell in turn acknowledged) with the data intact
  DROPPED       transitions that dropped a record (alert raised)

Usage: python3 policy_model.py --policy A --bad persistent --cap 1 ...
       see run_all.sh for the matrix.

A2_ATTACK_DOC (extensions; every one defaults off and then the model is the
original, which run_regress.sh checks against the shared out/ files):
  --taint-region K   the taint is kept per region of K full stripes (bitmask
                     per device) instead of per device (K=0, A2 as proposed)
  --a2-persist M     atomic (default: drop and taint in one durable write,
                     e.g. the taint in the log block header that omits the
                     record); dropfirst (the log block that omits the record
                     is written at the mark, the taint reaches disk only with
                     the op's commit / the next commit; a crash in between
                     loses it); taintfirst (the taint must be durable on every
                     present superblock -- zero errors -- before the drop: no
                     drop while a present member fails writes)
  --entry-stripes K  a log entry covers K consecutive full stripes (the
                     kernel's region entry, 64 blocks); capacity counts
                     entries and eviction spends a whole entry
  --scrub-mode M     atomic (default); cursor (scrub is a sequence of
                     per-stripe steps that interleave with writes, detaches,
                     crashes and remounts -- btrfs scrub resume -- and the
                     taint of a region is cleared at the end of a pass only if
                     no drop tainted it during the pass); cursor-noepoch (the
                     pass clears whatever is tainted at its end: mutation)
  --ghost            replacing a device that is missing or failing writes
                     (its superblock cannot be scratched) leaves the old disk
                     around; a later mount may find it in place of its
                     replacement (same devid and uuid: dev-replace.c:1017-1019
                     swaps them, device_list_add() accepts it)
  --tombstone M      none (default); tainted (a replace of a tainted device
                     keeps a tombstone: the old disk is recognised by its
                     superblock generation and treated as missing); all
                     (every replace of a missing/failing device does)
  --die K            K devices may die for good (GONE) at runtime or at a
                     mount, in addition to --detach (which may return)
  --main-pull        the admin pull of a failing device (the A-4 unwedge) is
                     a main-search edge; with --ret the pulled disk may be
                     plugged back later
  --scan             a missing device may re-appear at runtime (device scan):
                     still no I/O until the next mount, but not counted as
                     missing by fs_devices->missing_devices (volumes.c:961)
Mutations added: a2_drop_torn, a2_drop_verdict, a2_entry_partial,
  a2_missing_bit (the drop tests the MISSING bit / missing_devices, which a
  device scan clears, not bdev == NULL), a2_phaseA_tainted (variant: phase A
  may write back a rebuilt column onto a tainted device),
  taint_skip_read, taint_skip_rmw, taint_skip_recover, taint_skip_scrub,
  taint_skip_replace (one taint enforcement site forgotten each).
"""

import argparse
import itertools
import sys
import time
from collections import Counter

EIO = "EIO"
EMPTY = frozenset()
PR, BAD, FB, FL, TN, GONE, PEND, SCN = range(8)
DEV_OK = (1, 0, 0, 0, 0, 0, 0, 0)
DEV_NONE = (0, 0, 0, 0, 0, 0, 0, 0)
RW, RO, DOWN = 0, 1, 2
MODEN = {RW: "rw", RO: "ro", DOWN: "mount-failed"}
AL_DROP = 1
AL_UNFL = 2
ZERO = -9          # zeros a replace wrote where it could neither copy nor rebuild
VALS = (1, 2, 3, 4, 5)
POLICIES = ("OLD", "A", "CUR", "A2", "B", "C", "CB", "CTM")
MUTS = ("strict_degraded", "narrow_inflight", "evict_naming", "a2_no_taint", "c_trust_unverified", "no_suspect",
        "read_trust_torn", "absent_par_unnamed", "ack_unnamed", "b_no_record",
        "a2_drop_torn", "a2_drop_verdict", "a2_entry_partial", "a2_missing_bit", "a2_phaseA_tainted",
        "taint_skip_read", "taint_skip_rmw", "taint_skip_recover", "taint_skip_scrub", "taint_skip_replace")


def garbage(c, pars=()):
    """a wrong rebuild of column c; rebuilds from different parity sets
    disagree (real P and Q arithmetic differs), so a RAID6 cross-check of
    two wrong rebuilds fails as it does in recover_verify_q()"""
    return -(1 + c + 16 * sum(1 << p for p in pars))


FRESH = False


def newval(acc, diskv, parvals=()):
    """the value a new write stores: never one the cell may legally read
    back already, nor (with --fresh-values) one the column still holds on
    disk or in any parity snapshot, so a stale copy can never alias it"""
    bad = set(acc or ()) | {diskv}
    vals = VALS
    if FRESH:
        bad |= set(parvals)
        vals = VALS + (6, 7, 8, 9)
    for x in vals:
        if x not in bad:
            return x
    raise AssertionError("value domain exhausted")


class W:
    """Mutable working copy of a state."""
    __slots__ = ("devs", "disk", "par", "acc", "rec", "pnd", "mode", "cnt", "alert", "x")

    @staticmethod
    def of(t):
        w = W.__new__(W)
        devs, stripes, w.mode, cnt, w.alert, w.x = t
        w.devs = [list(d) for d in devs]
        w.disk = [list(x[0]) for x in stripes]
        w.par = [list(x[1]) for x in stripes]
        w.acc = [list(x[2]) for x in stripes]
        w.rec = [x[3] for x in stripes]
        w.pnd = [x[4] for x in stripes]
        w.cnt = list(cnt)
        return w

    def copy(self):
        n = W.__new__(W)
        n.devs = [list(d) for d in self.devs]
        n.disk = [list(x) for x in self.disk]
        n.par = [list(x) for x in self.par]
        n.acc = [list(x) for x in self.acc]
        n.rec = list(self.rec)
        n.pnd = list(self.pnd)
        n.mode = self.mode
        n.cnt = list(self.cnt)
        n.alert = self.alert
        n.x = self.x
        return n

    SYMM = True

    def tup(self):
        if not W.SYMM:
            return (tuple(tuple(d) for d in self.devs),
                    tuple((tuple(self.disk[s]), tuple(self.par[s]), tuple(self.acc[s]), self.rec[s], self.pnd[s])
                          for s in range(len(self.disk))), self.mode, tuple(self.cnt), self.alert, self.x)
        stripes = tuple(sorted(
            ((tuple(self.disk[s]), tuple(self.par[s]), tuple(self.acc[s]), self.rec[s], self.pnd[s])
             for s in range(len(self.disk))), key=repr))
        return (tuple(tuple(d) for d in self.devs), stripes, self.mode, tuple(self.cnt), self.alert, self.x)


def fmt_rec(r):
    if r is None:
        return "-"
    torn, names, verdict = r
    s = ",".join(str(c) for c in sorted(names))
    t = "" if not torn else ("T" if torn is True else "T{%s}" % ",".join(str(c) for c in sorted(torn)))
    return "%s%s%s" % (t, "V" if verdict else "", "[" + s + "]")


class Model:
    def __init__(self, a):
        self.a = a
        self.raid = a.raid
        self.nd = 2
        self.npar = 1 if a.raid == 5 else 2
        self.N = self.nd + self.npar
        self.S = a.stripes
        self.cap = a.cap if a.cap else a.stripes
        self.P = a.policy
        self.log = a.policy != "OLD"
        self.muts = set(a.mut or ())
        self.meta = a.meta
        if a.meta or a.data == "csum":
            self.csum = frozenset(range(self.nd))
        elif a.data == "mixed":
            self.csum = frozenset([0])
        else:
            self.csum = EMPTY
        self.cname = ["d%d" % j for j in range(self.nd)] + (["P"] if self.npar == 1 else ["P", "Q"])
        self.memo_admin = {}
        self.memo_avail = {}
        # A2 attack extensions
        self.rsz = a.taint_region
        self.nreg = 1 if not self.rsz else (self.S + self.rsz - 1) // self.rsz
        self.allreg = (1 << self.nreg) - 1
        self.ek = max(1, a.entry_stripes)

    # ------------------------------------------------------------ helpers --
    def cn(self, c):
        return self.cname[c]

    def cset(self, cs):
        return "{" + ",".join(self.cn(c) for c in sorted(cs)) + "}"

    def nmissing(self, w):
        return sum(1 for d in w.devs if not d[PR])

    def nfailed(self, w):
        return sum(1 for d in w.devs if d[FL] and d[PR])

    def absent_budget_ok(self, w, extra=0):
        """missing + failed (present) devices within the parity budget"""
        return self.nmissing(w) + self.nfailed(w) + extra <= self.npar

    def is_failed(self, w, c):
        return self.P in ("C", "CB", "CTM") and bool(w.devs[c][FL])

    def writable(self, w, c):
        """a write is issued to the device (it may still fail if BAD)"""
        d = w.devs[c]
        return bool(d[PR]) and not self.is_failed(w, c)

    # ------------------------------------------------------- A2 taint --
    def rbit(self, s):
        """the taint bit of the region holding full stripe s"""
        return 1 if not self.rsz else 1 << (s // self.rsz)

    def tn(self, w, c, s, site=None):
        """is device c tainted (durably or in memory) for stripe s?  site
        names the enforcement point; a taint_skip_<site> mutation forgets it"""
        if site is not None and ("taint_skip_" + site) in self.muts:
            return False
        d = w.devs[c]
        return bool((d[TN] | d[PEND]) & self.rbit(s))

    def tn_any(self, w, c):
        d = w.devs[c]
        return bool(d[TN] | d[PEND])

    def persist_taint(self, w):
        """a commit: the in-memory taint (dropfirst) reaches the superblock"""
        for d in w.devs:
            if d[PEND]:
                d[TN] |= d[PEND]
                d[PEND] = 0

    def lose_pending_taint(self, w):
        for d in w.devs:
            d[PEND] = 0

    def ent(self, s):
        return s // self.ek

    def need_entry(self, w, s):
        """does a new record for stripe s need a new log entry?"""
        g = self.ent(s)
        return not any(w.rec[t] is not None for t in range(self.S) if self.ent(t) == g)

    def nent(self, w):
        return len({self.ent(t) for t in range(self.S) if w.rec[t] is not None})

    def log_full_for(self, w, s):
        return w.rec[s] is None and self.need_entry(w, s) and self.nent(w) >= self.cap

    def src_ok(self, w, c, csum_cell=False, s=None, site=None):
        """may the content of column c be used as a source without verification"""
        d = w.devs[c]
        if not d[PR]:
            return False
        if (self.tn(w, c, s, site) if s is not None else self.tn_any(w, c)):
            # A2 taint: distrusted like a stale member; a checksummed data
            # sector may still be served once it verifies
            return bool(csum_cell and c < self.nd)
        if self.is_failed(w, c):
            if csum_cell:
                return True        # verified by the caller
            if c < self.nd and "c_trust_unverified" in self.muts:
                return True
            return False
        return True

    def recinfo(self, w, s):
        if not self.log or w.rec[s] is None:
            return False, EMPTY, False
        return w.rec[s]

    def nrec(self, w):
        return sum(1 for r in w.rec if r is not None)

    def log_can_write(self, w):
        t = self.npar
        copies = sum(1 for c in range(self.N)
                     if w.devs[c][PR] and not w.devs[c][BAD] and not self.is_failed(w, c))
        bad = sum(1 for c in range(self.N) if w.devs[c][PR] and w.devs[c][BAD] and not self.is_failed(w, c))
        return copies >= t + 1 or (bad + self.nmissing(w) + self.nfailed(w)) <= t

    # ------------------------------------------------------------ rebuild --
    def solve(self, w, s, holes, pars):
        """values of the hole data columns from parity subset pars (len(pars)
        == len(holes)), using the present content of the other data columns"""
        disk = w.disk[s]
        snaps = [w.par[s][p] for p in pars]
        known = [j for j in range(self.nd) if j not in holes]
        ok = all(disk[j] == sn[j] for sn in snaps for j in known)
        if len(pars) == 2:
            ok = ok and all(snaps[0][h] == snaps[1][h] for h in holes)
        if ok:
            return {h: snaps[0][h] for h in holes}
        return {h: garbage(h, pars) for h in holes}

    def verified(self, w, s, j, v):
        a = w.acc[s][j]
        return a is None or v in a

    def rebuild_csum(self, w, s, i, base_holes, any_parity=True):
        """csum target i: try every hole set containing i and base_holes and
        every parity subset; the first verified value, else EIO"""
        others = [j for j in range(self.nd) if j != i and j not in base_holes]
        pars = [p for p in range(self.npar)
                if w.devs[self.nd + p][PR] and not self.is_failed(w, self.nd + p)]
        for k in range(len(others) + 1):
            for extra in itertools.combinations(others, k):
                holes = sorted(set(base_holes) | {i} | set(extra))
                if len(holes) > len(pars):
                    continue
                for ps in itertools.combinations(pars, len(holes)):
                    v = self.solve(w, s, holes, ps)[i]
                    if self.verified(w, s, i, v):
                        return v
        return EIO

    def data_known(self, w, s, j, names):
        """content of data column j usable as-is for a rebuild/RMW, or None"""
        cs = j in self.csum
        if not self.src_ok(w, j, cs, s, "rmw"):
            return None
        if self.log and j in names and not cs:
            return None
        v = w.disk[s][j]
        if cs and not self.verified(w, s, j, v):
            return None
        return v

    def usable_pars(self, w, s, names):
        return [p for p in range(self.npar)
                if self.src_ok(w, self.nd + p, False, s, "rmw") and not (self.log and (self.nd + p) in names)]

    def torn_blocks(self, torn, verdict, holes, npars):
        """may an unchecked rebuild of `holes` from `npars` parities be
        trusted in a stripe marked (torn, verdict)?  torn is True (a write
        into the whole stripe may have been torn) or, with narrow_inflight,
        the set of data columns the interrupted write supplied: a rebuild
        with one parity whose holes contain every such column returns the
        old or the new value of each, both acceptable."""
        if verdict:
            return True
        if not torn:
            return False
        if torn is True:
            return True
        return not (torn <= set(holes) and npars == 1)

    def rebuild_nocsum(self, w, s, holes, target, torn):
        """nodatasum rebuild of `target` with the given hole set; returns value
        or EIO following the policy's read rules"""
        names = self.recinfo(w, s)[1]
        pars = self.usable_pars(w, s, names)
        if len(holes) > len(pars):
            return EIO
        results = set()
        for ps in itertools.combinations(pars, len(holes)):
            results.add(self.solve(w, s, holes, ps)[target])
            if not self.log:
                break           # upstream: first rebuild, unverified
        if not self.log:
            return results.pop()
        if len(pars) > len(holes):
            # spare parity: cross-check (recover_verify_q / EV_READ_PARITY)
            return results.pop() if len(results) == 1 else EIO
        if torn and "read_trust_torn" not in self.muts:
            return EIO          # btrfs_wib_unrecovered / stripe_torn refusal
        return results.pop()

    # --------------------------------------------------------------- read --
    def pread(self, w, s, i):
        """policy read of data cell (s, i): a value or EIO"""
        torn, names, verdict = self.recinfo(w, s)
        cs = i in self.csum
        d = w.devs[i]
        if d[PR]:
            trust = True
            if self.log and i in names and not cs:
                trust = False
            if self.tn(w, i, s, "read") and not cs:
                trust = False
            if self.is_failed(w, i) and not cs and "c_trust_unverified" not in self.muts:
                trust = False
            if trust:
                v = w.disk[s][i]
                if not cs or self.verified(w, s, i, v):
                    return v
        if cs:
            base = [j for j in range(self.nd) if j != i and not w.devs[j][PR]]
            return self.rebuild_csum(w, s, i, base)
        holes = [i] + [j for j in range(self.nd) if j != i and self.data_known(w, s, j, names) is None]
        holes = sorted(holes)
        return self.rebuild_nocsum(w, s, holes, i, self.torn_blocks(torn, verdict, holes, len(holes)))

    def oracle_ok(self, w, s, i):
        a = w.acc[s][i]
        if a is None:
            return True
        if w.pnd[s] is not None and w.pnd[s][0] == i:
            return True         # the value is in the tree log
        if w.devs[i][PR] and w.disk[s][i] in a:
            return True
        others = [j for j in range(self.nd) if j != i]
        pars = [p for p in range(self.npar) if w.devs[self.nd + p][PR]]
        for k in range(len(others) + 1):
            for extra in itertools.combinations(others, k):
                holes = sorted({i} | set(extra) | {j for j in others if not w.devs[j][PR]})
                if len(holes) > len(pars):
                    continue
                for ps in itertools.combinations(pars, len(holes)):
                    if self.solve(w, s, holes, ps)[i] in a:
                        return True
        return False

    def read_report(self, w):
        """(silent cells, needless-EIO cells, info-lost cells)"""
        silent, needless, lost = [], [], []
        if w.mode == DOWN:
            for s in range(self.S):
                for i in range(self.nd):
                    if w.acc[s][i] is not None and not self.oracle_ok(w, s, i):
                        lost.append((s, i))
            return silent, needless, lost
        for s in range(self.S):
            for i in range(self.nd):
                a = w.acc[s][i]
                if a is None:
                    continue
                v = self.pread(w, s, i)
                ok = self.oracle_ok(w, s, i)
                if not ok:
                    lost.append((s, i))
                if v == EIO:
                    if ok:
                        needless.append((s, i))
                elif v not in a:
                    silent.append((s, i, v))
        return silent, needless, lost

    # ----------------------------------------------------------- eviction --
    def evict_candidates(self, w, busy, replay):
        """list of (stripe, taint_devices) the policy may drop, lowest pass
        first.  With --entry-stripes K > 1 a candidate stands for its whole
        entry (do_evict spends every record of it)."""
        if not self.log:
            return []
        if self.ek > 1:
            return self.evict_entries(w, busy, replay)
        missing = self.missing_set(w)
        recs = [(s, r) for s, r in enumerate(w.rec) if r is not None and s != busy]
        vague = [(s, ()) for s, r in recs if not r[0] and not r[1] and not r[2]]
        if vague:
            return vague
        P = self.P
        if P == "CUR" or "evict_naming" in self.muts:
            may = bool(self.missing_count_set(w)) or replay or "evict_naming" in self.muts
            if may:
                p1 = [(s, ()) for s, r in recs if not r[2]]
            else:
                p1 = [(s, ()) for s, r in recs if r[0] and not r[1] and not r[2]]
            if p1:
                return p1
            if may:
                return [(s, ()) for s, r in recs if r[2]]
            return []
        if P in ("A2", "C", "CB", "CTM"):
            c = [(s, tuple(sorted(r[1]))) for s, r in recs if self.a2_droppable(r, missing)]
            if P == "A2" and "a2_no_taint" not in self.muts:
                if not self.a2_may_drop_now(w):
                    return []
                return c
            return [(s, ()) for s, _ in c]
        return []

    def missing_set(self, w):
        """devices A2 may drop records for: no I/O goes to them.  With the
        a2_missing_bit mutation, a device a scan re-registered (SCN) no longer
        counts (the kernel's BTRFS_DEV_STATE_MISSING / missing_devices,
        cleared by device_list_add() at volumes.c:959-962 while bdev stays NULL)"""
        if "a2_missing_bit" in self.muts:
            return {c for c in range(self.N) if not w.devs[c][PR] and not w.devs[c][SCN]}
        return {c for c in range(self.N) if not w.devs[c][PR]}

    def missing_count_set(self, w):
        """what CUR's wib_may_evict_naming() sees: fs_devices->missing_devices,
        which a device scan decrements (volumes.c:959-962)"""
        return {c for c in range(self.N) if not w.devs[c][PR] and not w.devs[c][SCN]}

    def a2_droppable(self, r, missing):
        torn, names, verdict = r
        if not names or not names <= missing:
            return False
        if torn and "a2_drop_torn" not in self.muts:
            return False
        if verdict and "a2_drop_verdict" not in self.muts:
            return False
        return True

    def a2_may_drop_now(self, w):
        """taintfirst: the taint must reach every present superblock (zero
        errors, the design's durability rule) before the record may go; a
        present member failing writes blocks it"""
        if self.a.a2_persist != "taintfirst":
            return True
        return not any(d[PR] and (d[BAD] or d[FB]) and not d[FL] for d in w.devs)

    def evict_entries(self, w, busy, replay):
        """entry-granular eviction: an entry is the K stripes t with t//K == g"""
        missing = self.missing_set(w)
        bg = self.ent(busy) if busy is not None else None
        groups = {}
        for t, r in enumerate(w.rec):
            if r is not None and self.ent(t) != bg:
                groups.setdefault(self.ent(t), []).append((t, r))
        P = self.P
        def vague(r):
            return not r[0] and not r[1] and not r[2]
        vg = [(g, ()) for g, rs in sorted(groups.items()) if all(vague(r) for _, r in rs)]
        if vg:
            return vg
        if P == "CUR" or "evict_naming" in self.muts:
            may = bool(self.missing_count_set(w)) or replay or "evict_naming" in self.muts
            if may:
                p1 = [(g, ()) for g, rs in sorted(groups.items()) if not any(r[2] for _, r in rs)]
            else:
                p1 = [(g, ()) for g, rs in sorted(groups.items())
                      if all((r[0] and not r[1] and not r[2]) or vague(r) for _, r in rs)]
            if p1:
                return p1
            if may:
                return [(g, ()) for g in sorted(groups)]
            return []
        if P in ("A2", "C", "CB", "CTM"):
            out = []
            for g, rs in sorted(groups.items()):
                chk = rs[:1] if "a2_entry_partial" in self.muts else rs
                if all(vague(r) or self.a2_droppable(r, missing) for _, r in chk) and \
                        any(self.a2_droppable(r, missing) for _, r in chk):
                    taint = sorted(set().union(*[set(r[1]) for _, r in chk]))
                    out.append((g, tuple(taint)))
            if P == "A2" and "a2_no_taint" not in self.muts:
                if not self.a2_may_drop_now(w):
                    return []
                return out
            return [(g, ()) for g, _ in out]
        return []

    def do_evict(self, w, e, taint):
        """spend the record of stripe e (entry e with --entry-stripes > 1),
        tainting the devices it names for its region(s)"""
        if self.ek > 1:
            stripes = [t for t in range(self.S) if self.ent(t) == e]
        else:
            stripes = [e]
        bits = 0
        for t in stripes:
            if w.rec[t] is not None:
                bits |= self.rbit(t)
            w.rec[t] = None
        w.alert |= AL_DROP
        if not bits:
            bits = self.rbit(stripes[0])
        sc, sok, gh = w.x
        for c in taint:
            if self.a.a2_persist == "dropfirst":
                w.devs[c][PEND] |= bits
            else:
                w.devs[c][TN] |= bits
            # a scrub pass in progress cannot clear what was tainted after it began
            if self.a.scrub_mode == "cursor":
                sok &= ~bits
        w.x = (sc, sok, gh)

    def evict_label(self, w, e):
        if self.ek > 1:
            return "e%d{%s}" % (e, ",".join("s%d%s" % (t, fmt_rec(w.rec[t])) for t in range(self.S)
                                           if self.ent(t) == e and w.rec[t] is not None))
        return "s%d%s" % (e, fmt_rec(w.rec[e]))

    # --------------------------------------------------------- C: failing --
    def fail_device(self, w, c):
        """C/CTM admission: FAILED durably; forget the names on its column"""
        w.devs[c][FL] = 1
        for s in range(self.S):
            r = w.rec[s]
            if r is None:
                continue
            torn, names, verdict = r
            names = names - {c}
            w.rec[s] = None if (not names and not torn and not verdict) else (torn, names, verdict)

    # ---------------------------------------------------------------- RMW --
    def rmw(self, w0, s, i, ctx, replay_mode=False):
        """A read-modify-write of data cell i (None: a repair) of stripe s,
        with its commit.  ctx: 'write', 'repair', 'replay', 'probe' (the
        availability probe: no crash, first eviction candidate).
        Returns a list of (label, W, events)."""
        out = []
        nd, npar = self.nd, self.npar
        meta = self.meta or ctx == "replay"
        probe = ctx == "probe"
        w = w0.copy()
        torn, names, verdict = self.recinfo(w, s)
        if ctx == "replay":
            i, v = w.pnd[s]
        elif i is not None:
            v = newval(w.acc[s][i], w.disk[s][i], [sn[i] for sn in w.par[s]])
        else:
            v = None
        tag = ("repair s%d" % s) if i is None else ("%s s%d.%s=%d" % (
            "replay" if ctx == "replay" else "write", s, self.cn(i), v))

        def refused(why, wr):
            ev = {"REFUSED_WRITE"} if i is not None else {"REPAIR_FAILED"}
            if i is not None and ctx == "replay":
                wr.mode = DOWN
                ev |= {"RO"}
            elif i is not None and meta:
                wr.mode = RO
                ev |= {"RO"}
            return ("%s REFUSED(%s)" % (tag, why), wr, frozenset(ev))

        # ---- the RMW read ----
        known = [None] * nd
        for j in range(nd):
            known[j] = self.data_known(w, s, j, names)
        holes = [j for j in range(nd) if known[j] is None]
        needed = [j for j in holes if j != i]
        cur = list(known)
        if "strict_degraded" in self.muts:
            for j in needed:
                if not self.src_ok(w, j, j in self.csum, s) and w.acc[s][j] is not None:
                    return [refused("degraded-rmw", w)]
        if needed:
            pars = self.usable_pars(w, s, names)
            if len(holes) > len(pars) and self.log:
                # every csum column may still be rebuilt and verified
                if all(j in self.csum for j in needed):
                    for j in needed:
                        cur[j] = self.rebuild_csum(w, s, j, [h for h in holes if h != j and not w.devs[h][PR]])
                    if any(cur[j] == EIO for j in needed):
                        return [refused("undecidable", w)]
                else:
                    return [refused("undecidable", w)]
            elif len(holes) > len(pars):
                return [refused("beyond-tolerance", w)]
            else:
                sols = [self.solve(w, s, holes, ps) for ps in itertools.combinations(pars, len(holes))]
                for j in needed:
                    if j in self.csum:
                        vv = [so[j] for so in sols if self.verified(w, s, j, so[j])]
                        if not vv:
                            r = self.rebuild_csum(w, s, j, [h for h in holes if h != j and not w.devs[h][PR]])
                            if r == EIO:
                                return [refused("unreadable", w)]
                            vv = [r]
                        cur[j] = vv[0]
                    else:
                        vals = {so[j] for so in sols}
                        if self.log:
                            if len(pars) > len(holes) and len(vals) > 1:
                                return [refused("parities-disagree", w)]
                            if len(pars) == len(holes) and self.torn_blocks(torn, verdict, holes, len(holes)):
                                return [refused("undecidable-torn", w)]
                        cur[j] = sols[0][j]
        if i is not None:
            cur[i] = v
        vec = tuple(cur)

        # ---- mark ----
        branches = [(w, "")]
        if self.log:
            if not self.log_can_write(w):
                return [refused("log-write", w)]
            if w.rec[s] is None:
                tlog = ""
                if self.log_full_for(w, s) and self.P in ("C", "CB", "CTM"):
                    # T-log(b): the log is full of records naming a failing
                    # member: admit it (its names are then forgotten)
                    for c in range(self.N):
                        dv = w.devs[c]
                        if (dv[PR] and (dv[BAD] or dv[FB]) and not dv[FL] and self.absent_budget_ok(w, 1)
                                and any(r is not None and c in r[1] for r in w.rec)):
                            self.fail_device(w, c)
                            tlog += " T-log fails %s" % self.cn(c)
                    if tlog:
                        tag += tlog
                        if w.rec[s] is not None:
                            torn, names, verdict = self.recinfo(w, s)
                if self.log_full_for(w, s):
                    cands = self.evict_candidates(w, s, replay_mode)
                    if not cands:
                        return [refused("log-full", w)]
                    if probe:
                        cands = cands[:1]
                    branches = []
                    for e, taint in cands:
                        we = w.copy()
                        self.do_evict(we, e, taint)
                        lab = " evict %s%s" % (self.evict_label(w, e),
                                               ((" taint%s" % self.cset(taint)) +
                                                ("(not durable)" if self.a.a2_persist == "dropfirst" else ""))
                                               if taint else "")
                        branches.append((we, lab))
                for we, _ in branches:
                    if we.rec[s] is None:
                        we.rec[s] = (False, EMPTY, False)
        for w1, elab in branches:
            ev0 = {"DROPPED"} if elab else set()
            if " T-log fails" in tag:
                ev0.add("FAILDEV")
            out.extend(self.rmw_body(w1, s, i, v, vec, ctx, tag + elab, ev0, meta, probe, refused))
        return out

    def crash_image(self, w, s, i, v, phase_b=True):
        """the op in progress is lost: its record is on disk as in flight,
        its target cell unacknowledged.  With narrow_inflight the in-flight
        mark is written with phase A's persist, just before phase B, and
        names only the data column phase B writes."""
        wc = w.copy()
        if self.log and "narrow_inflight" in self.muts and not phase_b:
            r = wc.rec[s]
            if r is not None and not (r[0] or r[1] or r[2]):
                wc.rec[s] = None
        elif self.log:
            torn, names, verdict = wc.rec[s] if wc.rec[s] else (False, EMPTY, False)
            if "narrow_inflight" in self.muts and torn is not True:
                t2 = (torn or EMPTY) | (frozenset([i]) if i is not None else EMPTY)
                wc.rec[s] = (t2 if t2 else False, names, verdict) if (t2 or names or verdict) else None
            else:
                wc.rec[s] = (True, names, verdict)
        if i is not None:
            a = wc.acc[s][i]
            if a is not None:
                a2 = a | {v}
                wc.acc[s][i] = a2 if len(a2) <= 3 else None
        return wc

    def rmw_body(self, w, s, i, v, vec, ctx, tag, ev0, meta, probe, refused):
        out = []
        nd, npar, N = self.nd, self.npar, self.N
        P = self.P
        can_crash = not probe and ctx != "replay" and w.cnt[0] < self.a.crash
        torn, names, verdict = self.recinfo(w, s)
        if can_crash:
            out.extend(self.crash_outcomes(self.crash_image(w, s, i, v, False), tag + " ; CRASH after mark", ev0))
        # ---- phase A ----
        degraded = set()
        if self.log:
            colsA = [j for j in range(nd) if j != i and j in names and self.writable(w, j)
                     and (not self.tn(w, j, s) or "a2_phaseA_tainted" in self.muts)]
            if colsA:
                okA = [j for j in colsA if not w.devs[j][BAD]]
                failA = [j for j in colsA if w.devs[j][BAD]]
                for j in okA:
                    w.disk[s][j] = vec[j]
                if can_crash and okA:
                    out.extend(self.crash_outcomes(self.crash_image(w, s, i, v, False),
                                                   tag + " ; phaseA wrote %s ; CRASH before persist" % self.cset(okA), ev0))
                if failA:
                    if P in ("B", "CB") and i is not None and "strict_degraded" not in self.muts:
                        degraded = set(failA)
                        tag += " phaseA %s refused->degrade" % self.cset(failA)
                    else:
                        # refused: the record goes back to what it was (sticky, names kept)
                        wr = w.copy()
                        wr.rec[s] = (torn, names, verdict) if (torn or names or verdict) else None
                        lab, wr2, ev = refused("phaseA %s write-back failed" % self.cset(failA), wr)
                        out.append((lab, wr2, ev | ev0))
                        return out
                if okA:
                    if not self.log_can_write(w):
                        wr = w.copy()
                        wr.rec[s] = (torn, names, verdict) if (torn or names or verdict) else None
                        lab, wr2, ev = refused("persist", wr)
                        out.append((lab, wr2, ev | ev0))
                        return out
                    names = names - set(okA)
                    w.rec[s] = (torn, names, verdict)
        # ---- phase B ----
        targets = ([i] if i is not None else []) + [nd + p for p in range(npar)]
        issued = [c for c in targets if self.writable(w, c)]
        skipped_missing = [c for c in targets if not w.devs[c][PR]]
        landable = [c for c in issued if not w.devs[c][BAD]]
        failedB = [c for c in issued if w.devs[c][BAD]]
        newval_of = {}
        for c in targets:
            newval_of[c] = v if c < nd else vec
        if can_crash:
            for k in range(len(landable) + 1):
                for sub in itertools.combinations(landable, k):
                    wc = w.copy()
                    for c in sub:
                        if c < nd:
                            wc.disk[s][c] = v
                        else:
                            wc.par[s][c - nd] = vec
                    out.extend(self.crash_outcomes(self.crash_image(wc, s, i, v),
                                                   tag + " ; phaseB landed %s ; CRASH" % self.cset(sub), ev0))
        pre = {c: (w.disk[s][c] if c < nd else w.par[s][c - nd]) for c in targets}
        for c in landable:
            if c < nd:
                w.disk[s][c] = v
            else:
                w.par[s][c - nd] = vec
        if self.a.taint_clear == "column" and self.rsz == 1:
            # a per-stripe taint goes with a write that rewrote the whole
            # column (one sector per column in this model), as a name does;
            # only once its flush is known good (below: flushlost undoes it)
            self._tclear = [c for c in landable if self.tn(w, c, s)]
        else:
            self._tclear = []
        # ---- commit: barrier flush ----
        flushlost = []
        flushfail_devs = [c for c in range(N) if w.devs[c][PR] and w.devs[c][FB] and not self.is_failed(w, c)]
        for c in landable:
            if c in flushfail_devs:
                flushlost.append(c)
                if c < nd:
                    w.disk[s][c] = pre[c]
                else:
                    w.par[s][c - nd] = pre[c]
        for c in self._tclear:
            if c not in flushlost:
                w.devs[c][TN] &= ~self.rbit(s)
                w.devs[c][PEND] &= ~self.rbit(s)
        tflush_failed = []
        if P in ("C", "CB", "CTM"):
            for c in flushfail_devs:
                if self.absent_budget_ok(w, 1):
                    self.fail_device(w, c)
                    tflush_failed.append(c)
            if tflush_failed:
                tag += " T-flush fails %s" % self.cset(tflush_failed)
                torn, names, verdict = self.recinfo(w, s)
        faults = set(failedB) | {c for c in range(N) if not w.devs[c][PR]} | \
            {c for c in range(N) if self.is_failed(w, c)} | degraded | set(flushlost)
        ok = len(faults) <= npar
        # ---- record ----
        if self.log:
            nn = set(names)
            for c in landable:
                if c not in flushlost:
                    nn.discard(c)
            nn |= set(failedB) | set(skipped_missing)
            unnamed = []
            if P not in ("C", "CB", "CTM") or not tflush_failed:
                fl = [c for c in flushlost if not self.is_failed(w, c)]
                if self.a.unnamed_flush and fl:
                    unnamed = fl
                else:
                    nn |= set(fl)
            if degraded and "b_no_record" in self.muts:
                nn -= degraded
            nn = {c for c in nn if not self.is_failed(w, c)}
            if ok:
                # the parities that landed describe the stripe again
                ntorn = bool(unnamed)
                nverdict = False
            else:
                ntorn, nverdict = (torn or bool(unnamed)), verdict
            w.rec[s] = (ntorn, frozenset(nn), nverdict) if (nn or ntorn or nverdict) else None
            if unnamed and ok:
                if P == "CUR" or "ack_unnamed" in self.muts:
                    # wib_readd_dropped nr_unnamed: marked possibly torn,
                    # EV_LOG_UNFLUSHED latched, the commit goes on
                    w.alert |= AL_UNFL
                    tag += " (flush lost %s, log cannot name it: torn-only + alert)" % self.cset(unnamed)
                else:
                    # strict: the commit that would acknowledge it aborts
                    wr = w.copy()
                    if i is not None:
                        a = wr.acc[s][i]
                        if a is not None:
                            a2 = a | {v}
                            wr.acc[s][i] = a2 if len(a2) <= 3 else None
                    wr.mode = RO
                    out.append(("%s (flush lost %s, cannot name it) COMMIT ABORTS -> RO" % (tag, self.cset(unnamed)),
                                wr, frozenset(ev0 | {"RO", "REFUSED_WRITE"} if i is not None else ev0 | {"RO"})))
                    return out
        if not ok:
            # the caller was told the write failed: its range may read back
            # old or new (a landed parity describes the new value)
            if i is not None and issued:
                a = w.acc[s][i]
                if a is not None:
                    a2 = a | {v}
                    w.acc[s][i] = a2 if len(a2) <= 3 else None
            lab, wr, ev = refused("tolerance %s" % self.cset(faults), w)
            out.append((lab, wr, ev | ev0))
            return out
        ev = set(ev0)
        if i is not None:
            w.acc[s][i] = frozenset([v])
            if ctx == "replay":
                w.pnd[s] = None
        lab = tag + " ok"
        if failedB:
            lab += " (write to %s failed)" % self.cset(failedB)
        if flushlost:
            lab += " (flush lost %s)" % self.cset(flushlost)
        if tflush_failed:
            ev.add("FAILDEV")
        self.persist_taint(w)       # the op's commit carries the taint
        out.append((lab, w, frozenset(ev)))
        return out

    # -------------------------------------------------------------- crash --
    def crash_outcomes(self, wc, tag, ev0):
        """crash image -> every mount that may follow"""
        res = []
        wc.cnt[0] += 1
        self.lose_pending_taint(wc)     # dropfirst: the taint was only in memory
        for lab, wm, ev in self.mount_variants(wc, crashed=True):
            res.append((tag + " ; " + lab, wm, frozenset(ev0) | ev))
        return res

    def mount_variants(self, w, crashed):
        """choices of the device set at the mount, then the mount itself"""
        res = []
        if not crashed:
            w = w.copy()
            self.persist_taint(w)       # the unmount's commit
        scn = [c for c in range(self.N) if w.devs[c][SCN]]
        if scn:
            # a device a scan registered is opened by the next mount
            w = w.copy()
            for c in scn:
                w.devs[c][PR] = 1
                w.devs[c][SCN] = 0
        opts = [(w, "mount" + (" (%s scanned back)" % self.cset(scn) if scn else ""))]
        # a device dying for good at the mount
        if w.cnt[4] < self.a.die:
            for c in range(self.N):
                if w.devs[c][PR]:
                    wd = w.copy()
                    wd.devs[c][PR] = 0
                    wd.devs[c][GONE] = 1
                    wd.cnt[4] += 1
                    if self.nmissing(wd) + self.nfailed(wd) <= self.npar or \
                            (self.a.overfault and self.nmissing(wd) < self.N - 1):
                        opts.append((wd, "mount without %s (dead)" % self.cn(c)))
        # the old disk of a replaced device, in place of its replacement
        gh = w.x[2]
        if gh is not None and w.cnt[5] < self.a.ghost_events:
            d, vals, tomb = gh
            wg = w.copy()
            wg.cnt[5] += 1
            wg.x = (wg.x[0], wg.x[1], None)
            if tomb:
                wg.devs[d] = list(DEV_NONE)
                wg.devs[d][GONE] = 1
                glab = "mount with the old %s instead of its replacement (tombstone: old disk refused)" % self.cn(d)
            else:
                wg.devs[d] = list(DEV_OK)
                for t in range(self.S):
                    if d < self.nd:
                        wg.disk[t][d] = vals[t]
                    else:
                        wg.par[t][d - self.nd] = vals[t]
                glab = "mount with the old %s instead of its replacement" % self.cn(d)
            if self.nmissing(wg) + self.nfailed(wg) <= self.npar:
                opts.append((wg, glab))
        # a device missing at the mount
        if w.cnt[1] < self.a.detach:
            for c in range(self.N):
                if w.devs[c][PR]:
                    wd = w.copy()
                    wd.devs[c][PR] = 0
                    wd.devs[c][GONE] = 0 if self.a.ret else 1
                    wd.cnt[1] += 1
                    if self.nmissing(wd) + self.nfailed(wd) <= self.npar or \
                            (self.a.overfault and self.nmissing(wd) < self.N - 1):
                        opts.append((wd, "mount without %s" % self.cn(c)))
        # returning devices
        if self.a.ret:
            back = [c for c in range(self.N) if not w.devs[c][PR] and not w.devs[c][GONE]]
            if back:
                wb = w.copy()
                for c in back:
                    wb.devs[c][PR] = 1
                opts.append((wb, "mount with %s back" % self.cset(back)))
        for wo, lab in opts:
            for l2, wm, ev in self.mount(wo):
                res.append((lab + l2, wm, ev))
        return res

    def mount(self, w):
        """mount recovery (and tree-log replay); returns [(label, W, events)]"""
        w = w.copy()
        w.mode = RW
        P = self.P
        lab = ""
        if self.nmissing(w) + self.nfailed(w) > self.npar:
            # beyond the tolerance: only 'mount -o degraded,ro' works; a
            # read-only mount runs no recovery and replays no tree log
            w.mode = RO if not any(p is not None for p in w.pnd) else DOWN
            return [(" (beyond tolerance: ro,degraded)", w, frozenset())]
        if P == "CTM":
            for c in range(self.N):
                if not w.devs[c][PR] and not w.devs[c][FL]:
                    self.fail_device(w, c)
                    lab += " T-missing fails %s" % self.cn(c)
        replay = [s for s in range(self.S) if w.pnd[s] is not None]
        if self.log:
            for s in range(self.S):
                r = w.rec[s]
                if r is None:
                    continue
                if replay and (r[1] or r[2]):
                    continue        # error records wait for the replay
                self.recover_stripe(w, s)
        if self.a.taint_resync and not replay:
            self.resync_tainted(w)
        if not replay:
            return [(lab, w, frozenset())]
        res = []
        s = replay[0]
        for l2, wr, ev in self.rmw(w, s, None, "replay", replay_mode=True):
            if wr.mode == RW and self.log:
                for s2 in range(self.S):
                    if wr.rec[s2] is not None:
                        self.recover_stripe(wr, s2)
            res.append((lab + " ; " + l2, wr, ev))
        return res

    def recover_stripe(self, w, s):
        """btrfs_wib_recover for one loaded record"""
        torn, names, verdict = w.rec[s]
        nd, npar = self.nd, self.npar
        absent_data = [j for j in range(nd) if not self.src_ok(w, j, j in self.csum, s, "recover")
                       and not (j in self.csum and w.devs[j][PR] and self.is_failed(w, j))]
        # a2_phaseA_tainted: a tainted column that is present and writable is
        # untrusted for reading but may take a rebuilt value, like a named one
        tholes = []
        if "a2_phaseA_tainted" in self.muts:
            tholes = [j for j in absent_data if w.devs[j][PR] and self.writable(w, j)
                      and not w.devs[j][BAD] and self.tn(w, j, s)]
            absent_data = [j for j in absent_data if j not in tholes]
        missing_par = [nd + p for p in range(npar) if not w.devs[nd + p][PR]]
        names = set(names)
        if torn and "absent_par_unnamed" not in self.muts:
            names |= set(missing_par)
        if not absent_data:
            vals = list(w.disk[s])
            holes = [j for j in range(nd) if (j in names) or (j in tholes) or
                     (j in self.csum and not self.verified(w, s, j, w.disk[s][j]))]
            for j in holes:
                if j in self.csum:
                    r = self.rebuild_csum(w, s, j, [])
                elif self.torn_blocks(torn, verdict, holes, len(holes)) and "no_suspect" not in self.muts:
                    r = EIO
                else:
                    r = self.rebuild_nocsum(w, s, sorted(holes), j, False)
                if r == EIO:
                    w.rec[s] = (torn, frozenset(names), verdict)
                    return
                vals[j] = r
            newnames = set()
            for j in holes:
                if self.writable(w, j) and not w.devs[j][BAD]:
                    w.disk[s][j] = vals[j]
                    if j in tholes and self.a.recovery_clears and self.rsz == 1:
                        # the column now holds a trusted rebuild: this
                        # stripe's taint goes, as with a scrub (column rule)
                        w.devs[j][TN] &= ~self.rbit(s)
                        w.devs[j][PEND] &= ~self.rbit(s)
                elif not self.is_failed(w, j):
                    newnames.add(j)
            vec = tuple(vals)
            for p in range(npar):
                c = nd + p
                if self.writable(w, c) and not w.devs[c][BAD]:
                    w.par[s][p] = vec
                elif not self.is_failed(w, c):
                    if w.devs[c][PR] or "absent_par_unnamed" not in self.muts:
                        newnames.add(c)
            w.rec[s] = (False, frozenset(newnames), False) if newnames else None
            return
        # an absent data column (missing / failed / tainted): spec 3.5
        if torn and not verdict and torn is not True and torn <= set(absent_data) and len(absent_data) == 1:
            # narrow_inflight: the interrupted write supplied only the absent
            # column; each parity describes its old or its new value.  Make
            # the parities agree (P wins) and call the stripe consistent.
            pars = self.usable_pars(w, s, names)
            if 0 in pars:
                for p in pars:
                    if p != 0:
                        c = nd + p
                        if self.writable(w, c) and not w.devs[c][BAD]:
                            w.par[s][p] = w.par[s][0]
                        else:
                            names.add(c)
                torn = False
        if torn and not verdict:
            ok = True
            for x in absent_data:
                if w.acc[s][x] is None:
                    continue
                if x in self.csum:
                    if self.rebuild_csum(w, s, x, [h for h in absent_data if h != x]) == EIO:
                        ok = False
                    continue
                pars = self.usable_pars(w, s, names)
                if "no_suspect" in self.muts:
                    continue
                if npar == 2 and len(absent_data) == 1 and len(pars) == 2:
                    dP = self.solve(w, s, [x], [0])[x]
                    dQ = self.solve(w, s, [x], [1])[x]
                    if dP == dQ and dP >= 0:
                        continue
                ok = False
            if ok:
                torn = False
            else:
                verdict = True
                names |= {nd + p for p in range(npar) if w.devs[nd + p][PR] and not self.is_failed(w, nd + p)}
        names = {c for c in names if not self.is_failed(w, c)}
        w.rec[s] = (torn, frozenset(names), verdict) if (torn or names or verdict) else None

    # -------------------------------------------------------------- scrub --
    def scrub_stripe(self, w, s):
        nd, npar = self.nd, self.npar
        self._rewritten = set()
        torn, names, verdict = self.recinfo(w, s)
        names = set(names)
        anymissing = any(not w.devs[c][PR] for c in range(self.N))
        vals = list(w.disk[s])
        holes = []
        for j in range(nd):
            cs = j in self.csum
            if not w.devs[j][PR] or self.tn(w, j, s, "scrub") or (self.is_failed(w, j) and not cs and
                                                       "c_trust_unverified" not in self.muts):
                holes.append(j)
            elif self.log and j in names:
                holes.append(j)
            elif cs and not self.verified(w, s, j, w.disk[s][j]):
                holes.append(j)
        undecided = False
        for j in holes:
            if w.acc[s][j] is None and not w.devs[j][PR]:
                continue
            if j in self.csum:
                r = self.rebuild_csum(w, s, j, [h for h in holes if h != j and not w.devs[h][PR]])
            elif self.log and self.torn_blocks(torn, verdict, holes, len(holes)):
                r = EIO
            else:
                r = self.rebuild_nocsum(w, s, sorted(holes), j, False)
            if r == EIO:
                undecided = True
                break
            vals[j] = r
        if undecided:
            return False
        newnames = {c for c in names if not w.devs[c][PR]}
        for j in holes:
            if not w.devs[j][PR]:
                continue
            if self.writable(w, j) and not w.devs[j][BAD]:
                w.disk[s][j] = vals[j]
                self._rewritten.add(j)
            elif not self.is_failed(w, j):
                newnames.add(j)
        clean = True
        if not anymissing:
            vec = tuple(vals)
            for p in range(npar):
                c = nd + p
                if self.writable(w, c) and not w.devs[c][BAD]:
                    w.par[s][p] = vec
                    self._rewritten.add(c)
                elif not self.is_failed(w, c):
                    newnames.add(c)
                    clean = False
            if self.log:
                w.rec[s] = (False, frozenset(newnames), False) if newnames else None
        else:
            clean = False
            if self.log and w.rec[s] is not None:
                w.rec[s] = (torn, frozenset(newnames | {c for c in names if c >= nd}), verdict)
        return clean and not any(c in newnames for c in range(nd))

    def clear_taint(self, w, okreg):
        for c in range(self.N):
            d = w.devs[c]
            if (d[TN] | d[PEND]) and d[PR] and not d[BAD]:
                d[TN] &= ~okreg
                d[PEND] &= ~okreg

    def scrub(self, w):
        """a complete scrub pass, atomic with respect to everything else;
        clears the taint of every region all of whose stripes it could
        rewrite (the whole device with --taint-region 0)"""
        w = w.copy()
        okreg = self.allreg
        colok = [self.allreg] * self.N
        for s in range(self.S):
            if not self.scrub_stripe(w, s):
                okreg &= ~self.rbit(s)
            for c in range(self.N):
                if c not in self._rewritten:
                    colok[c] &= ~self.rbit(s)
        if self.a.taint_clear == "column":
            self.clear_taint_cols(w, colok)
        else:
            self.clear_taint(w, okreg)
        w.x = (None, 0, w.x[2])
        return w

    def resync_tainted(self, w):
        """--taint-resync: at mount, rewrite the stripes of every tainted
        region of a present writable device (md's bitmap resync at re-add),
        clearing a region once each of its stripes' columns was rewritten"""
        todo = 0
        for c in range(self.N):
            d = w.devs[c]
            if d[PR] and not d[BAD] and self.writable(w, c):
                todo |= d[TN] | d[PEND]
        if not todo:
            return
        colok = [self.allreg] * self.N
        for s in range(self.S):
            if not (todo & self.rbit(s)):
                for c in range(self.N):
                    colok[c] &= ~self.rbit(s)
                continue
            self.scrub_stripe(w, s)
            for c in range(self.N):
                if c not in self._rewritten:
                    colok[c] &= ~self.rbit(s)
        self.clear_taint_cols(w, colok)

    def clear_taint_cols(self, w, colok):
        """--taint-clear column: a region of device c is untainted once the
        scrub rewrote c's column in every stripe of it from a trusted
        rebuild, whether or not another member then failed its write"""
        for c in range(self.N):
            d = w.devs[c]
            if (d[TN] | d[PEND]) and d[PR] and not d[BAD]:
                d[TN] &= ~colok[c]
                d[PEND] &= ~colok[c]

    def scrub_step(self, w):
        """--scrub-mode cursor: one stripe of a pass that may be interrupted
        and resumed (btrfs scrub cancel/resume keeps the position)"""
        w = w.copy()
        sc, sok, gh = w.x
        lab = "scrub step"
        if sc is None:
            sc, sok = 0, self.allreg
            lab = "scrub start"
        if not self.scrub_stripe(w, sc):
            sok &= ~self.rbit(sc)
        lab += " s%d" % sc
        sc += 1
        if sc >= self.S:
            self.clear_taint(w, sok)
            lab += " (pass ends, clears taint of regions %s)" % bin(sok)
            sc, sok = None, 0
        w.x = (sc, sok, gh)
        return lab, w

    # ------------------------------------------------------------ replace --
    def replace(self, w0, d):
        """replace device d by a new one T; None if the replace fails.  Per
        full stripe, T's column is a verified/trusted copy of d, else a
        rebuild without d, else zeros recorded stale (log policies)."""
        w = w0.copy()
        nd, npar = self.nd, self.npar
        src = list(w.devs[d])
        ghost = None
        if self.a.ghost and (not src[PR] or src[BAD] or src[FB]):
            # the old disk keeps its superblock: missing, or failing the
            # scratch write (dev-replace.c:1059 btrfs_scratch_superblocks)
            old = tuple(w0.disk[t][d] if d < nd else w0.par[t][d - nd] for t in range(self.S))
            tomb = self.a.tombstone == "all" or (self.a.tombstone == "tainted" and self.tn_any(w0, d))
            ghost = (d, old, tomb)
        own = set()         # stripes whose record carries this replace's zeros
        for s in range(self.S):
            torn, names, verdict = self.recinfo(w, s)
            val = None
            if d < nd:
                cs = d in self.csum
                if src[PR] and not self.tn(w0, d, s, "replace"):
                    trusted = not (self.log and d in names and not cs)
                    if self.is_failed(w, d) and not cs and "c_trust_unverified" not in self.muts:
                        trusted = False
                    if trusted:
                        vv = w.disk[s][d]
                        if not cs or self.verified(w, s, d, vv):
                            val = vv
                if val is None:
                    w.devs[d] = list(DEV_NONE)      # never a source
                    val = self.pread(w, s, d) if w.acc[s][d] is not None else w.disk[s][d]
                    w.devs[d] = list(src)
            else:
                w.devs[d] = list(DEV_NONE)
                vec = [self.pread(w, s, j) if w.acc[s][j] is not None else w.disk[s][j]
                       for j in range(nd)]
                w.devs[d] = list(src)
                val = EIO if any(x == EIO for x in vec) else tuple(vec)
            if val == EIO:
                if self.log:
                    if self.log_full_for(w, s):
                        cands = self.evict_candidates(w, s, False)
                        if not cands:
                            return None         # the replace fails: nowhere to record the zeros
                        if self.a.replace_own == "fail":
                            # the kernel spends the replace's own marks last and
                            # then fails the replace (wib_entry_replace_owned(),
                            # replace_marks_lost, btrfs_wib_replace_end())
                            ownk = {self.ent(t) if self.ek > 1 else t for t in own}
                            other = [c for c in cands if c[0] not in ownk]
                            if not other:
                                return None
                            cands = other
                        self.do_evict(w, cands[0][0], cands[0][1])
                    t0, n0, v0 = self.recinfo(w, s)
                    w.rec[s] = (t0, n0 | {d}, v0)
                    own.add(s)
                val = ZERO if d < nd else (ZERO,) * nd
            elif self.log and w.rec[s] is not None:
                t0, n0, v0 = w.rec[s]
                n0 = n0 - {d}
                w.rec[s] = (t0, n0, v0) if (t0 or n0 or v0) else None
            if d < nd:
                w.disk[s][d] = val
            else:
                w.par[s][d - nd] = val
        w.devs[d] = list(DEV_OK)
        self.persist_taint(w)
        if ghost is not None:
            w.x = (w.x[0], w.x[1], ghost)
        return w

    # ------------------------------------------------------ admin actions --
    def admin_actions(self, t, main=False):
        """deterministic admin macro-actions: [(label, t2)]"""
        w = W.of(t)
        res = []
        a = self.a
        N = self.N
        bad = [c for c in range(N) if w.devs[c][PR] and (w.devs[c][BAD] or w.devs[c][FB])]
        if bad and a.bad != "persistent" and not main:
            wh = w.copy()
            for c in bad:
                wh.devs[c][BAD] = 0
                wh.devs[c][FB] = 0
            res.append(("admin heal %s" % self.cset(bad), wh.tup()))
        back = [c for c in range(N) if not w.devs[c][PR] and not w.devs[c][GONE]]
        if back and a.ret and not main:
            wb = w.copy()
            self.persist_taint(wb)
            for c in back:
                wb.devs[c][PR] = 1
            for lab, wm, ev in self.mount(wb):
                res.append(("admin return %s ; %s" % (self.cset(back), lab), wm.tup()))
        if not main:
            for c in bad:
                if self.P in ("C", "CB", "CTM") and w.devs[c][FL]:
                    continue
                wp = w.copy()
                self.persist_taint(wp)
                wp.devs[c][PR] = 0
                wp.devs[c][GONE] = 1
                if self.nmissing(wp) + self.nfailed(wp) <= self.npar:
                    for lab, wm, ev in self.mount(wp):
                        res.append(("admin pull %s ; remount%s" % (self.cn(c), lab), wm.tup()))
            if w.mode != RW or any(r is not None for r in w.rec):
                wu = w.copy()
                self.persist_taint(wu)
                for lab, wm, ev in self.mount(wu):
                    res.append(("admin remount%s" % lab, wm.tup()))
        if w.mode == RW and not main:
            # the application rewrites (or deletes) cells whose content is
            # undefined (no acknowledged value to lose)
            lostc = [(s, i) for s in range(self.S) for i in range(self.nd)
                     if w.acc[s][i] is None]
            if lostc:
                wl = w
                okl = True
                for s, i in lostc:
                    outs = self.rmw(wl, s, i, "probe")
                    lab, wn, ev = outs[-1]
                    if "REFUSED_WRITE" in ev:
                        okl = False
                        break
                    wl = wn
                if okl:
                    res.append(("admin rewrite lost cells %s" % lostc, wl.tup()))
        if w.mode == RW:
            if main and a.scrub and a.scrub_mode != "atomic":
                lab, ws = self.scrub_step(w)
                res.append((lab, ws.tup()))
            elif not main or a.scrub:
                res.append(("scrub", self.scrub(w).tup()))
            if (not main or a.replace) and not (a.no_spare and not main):
                for c in range(N):
                    dv = w.devs[c]
                    if not dv[PR] or dv[BAD] or dv[FB] or self.tn_any(w, c) or self.is_failed(w, c):
                        if not dv[PR] and self.nmissing(w) > self.npar:
                            continue
                        wr = self.replace(w, c)
                        if wr is not None:
                            res.append(("replace %s" % self.cn(c), wr.tup()))
            if self.P in ("C", "CB", "CTM"):
                for c in bad:
                    if not w.devs[c][FL] and self.absent_budget_ok(w, 1):
                        wf = w.copy()
                        self.fail_device(wf, c)
                        res.append(("%s fail %s" % ("trigger" if main else "admin", self.cn(c)), wf.tup()))
        return res

    def data_ok(self, t):
        w = W.of(t)
        if w.mode == DOWN:
            return False
        if any(p is not None for p in w.pnd):
            return False
        for s in range(self.S):
            for i in range(self.nd):
                a = w.acc[s][i]
                if a is None:
                    continue
                v = self.pread(w, s, i)
                if v == EIO or v not in a:
                    return False
        return True

    def full_avail(self, t):
        r = self.memo_avail.get(t)
        if r is not None:
            return r
        w = W.of(t)
        ok = w.mode == RW
        if ok:
            for c in range(self.N):
                d = w.devs[c]
                if d[PR] and (d[BAD] or d[FB]) and not self.is_failed(w, c) and self.P != "OLD":
                    ok = False
        if ok:
            cur = w
            for s in range(self.S):
                for i in range(self.nd):
                    outs = self.rmw(cur, s, i, "probe")
                    lab, wn, ev = outs[-1]
                    if "REFUSED_WRITE" in ev:
                        ok = False
                        break
                    cur = wn
                if not ok:
                    break
        self.memo_avail[t] = ok
        return ok

    def untainted(self, t):
        return not any(d[TN] or d[PEND] for d in t[0])

    def strip_taint(self, t):
        devs = tuple(tuple(v if k not in (TN, PEND) else 0 for k, v in enumerate(d)) for d in t[0])
        return (devs,) + tuple(t[1:])

    def admin_eval(self, t, depth):
        """(data readable, full write availability, and in addition no device
        left tainted) reachable by <= depth admin actions"""
        key = (t, depth)
        r = self.memo_admin.get(key)
        if r is not None:
            return r
        d_ok = self.data_ok(t)
        f_ok = d_ok and self.full_avail(t)
        u_ok = f_ok and self.untainted(t)
        if not u_ok and depth > 0:
            for lab, t2 in self.admin_actions(t):
                if t2 == t:
                    continue
                d2, f2, u2 = self.admin_eval(t2, depth - 1)
                d_ok = d_ok or d2
                f_ok = f_ok or f2
                u_ok = u_ok or u2
                if u_ok:
                    break
        r = (d_ok, f_ok, u_ok)
        self.memo_admin[key] = r
        return r

    # --------------------------------------------------------- main edges --
    def init(self):
        w = W.__new__(W)
        w.devs = [list(DEV_OK) for _ in range(self.N)]
        w.x = (None, 0, None)
        w.disk = [[1] * self.nd for _ in range(self.S)]
        w.par = [[tuple([1] * self.nd)] * self.npar for _ in range(self.S)]
        w.acc = [[frozenset([1])] * self.nd for _ in range(self.S)]
        w.rec = [None] * self.S
        w.pnd = [None] * self.S
        w.mode = RW
        w.cnt = [0, 0, 0, 0, 0, 0]
        w.alert = 0
        return w.tup()

    def successors(self, t):
        a = self.a
        w = W.of(t)
        res = []
        N = self.N
        if w.mode == RW:
            for s in range(self.S):
                for i in range(self.nd):
                    res.extend(self.rmw(w, s, i, "write"))
                if a.repair and self.log:
                    torn, names, verdict = self.recinfo(w, s)
                    if any(self.writable(w, c) for c in names) and not torn and not verdict:
                        res.extend(self.rmw(w, s, None, "repair"))
            if a.replay and w.cnt[0] < a.crash:
                # an fsync'd write whose data is only in the tree log, then a crash
                for s in range(self.S):
                    for i in range(self.nd):
                        wc = w.copy()
                        v = newval(wc.acc[s][i], wc.disk[s][i], [sn[i] for sn in wc.par[s]])
                        wc.acc[s][i] = frozenset([v])
                        wc.pnd[s] = (i, v)
                        wc.cnt[0] += 1
                        for lab, wm, ev in self.mount_variants(wc, crashed=True):
                            res.append(("fsync s%d.%s=%d (tree log) ; CRASH ; %s" % (s, self.cn(i), v, lab), wm, ev))
        # faults
        nfault = sum(1 for d in w.devs if d[BAD] or d[FB])
        if w.cnt[3] < a.maxfault and nfault < a.maxfault:
            for c in range(N):
                d = w.devs[c]
                if not d[PR] or d[BAD] or d[FB] or d[FL]:
                    continue
                if a.bad != "none":
                    wb = w.copy()
                    wb.devs[c][BAD] = 1
                    wb.cnt[3] += 1
                    res.append(("%s starts failing writes" % self.cn(c), wb, frozenset()))
                if a.flush:
                    wb = w.copy()
                    wb.devs[c][FB] = 1
                    wb.cnt[3] += 1
                    res.append(("%s starts failing flushes" % self.cn(c), wb, frozenset()))
        if a.bad == "transient" or (a.flush and a.bad != "persistent"):
            for c in range(N):
                d = w.devs[c]
                if d[PR] and (d[BAD] or d[FB]):
                    wh = w.copy()
                    wh.devs[c][BAD] = 0
                    wh.devs[c][FB] = 0
                    res.append(("%s heals" % self.cn(c), wh, frozenset()))
        # runtime detach
        if w.cnt[1] < a.detach and w.mode != DOWN:
            for c in range(N):
                if w.devs[c][PR] and (self.nmissing(w) + self.nfailed(w) + 1 <= self.npar or
                                      (a.overfault and self.nmissing(w) + 1 < N)):
                    wd = w.copy()
                    wd.devs[c][PR] = 0
                    wd.devs[c][GONE] = 0 if a.ret else 1
                    wd.cnt[1] += 1
                    lab = "%s detaches" % self.cn(c)
                    if self.P == "CTM" and not wd.devs[c][FL] and self.absent_budget_ok(w, 1):
                        self.fail_device(wd, c)
                        lab += " (T-missing: failed)"
                    if self.nmissing(wd) + self.nfailed(wd) > self.npar and wd.mode == RW:
                        wd.mode = RO
                        lab += " (beyond tolerance: read-only)"
                    res.append((lab, wd, frozenset()))
        # a device dies for good at runtime
        if w.cnt[4] < a.die and w.mode != DOWN:
            for c in range(N):
                if w.devs[c][PR] and (self.nmissing(w) + self.nfailed(w) + 1 <= self.npar or
                                      (a.overfault and self.nmissing(w) + 1 < N)):
                    wd = w.copy()
                    wd.devs[c][PR] = 0
                    wd.devs[c][GONE] = 1
                    wd.cnt[4] += 1
                    lab = "%s dies" % self.cn(c)
                    if self.nmissing(wd) + self.nfailed(wd) > self.npar and wd.mode == RW:
                        wd.mode = RO
                        lab += " (beyond tolerance: read-only)"
                    res.append((lab, wd, frozenset()))
        # a missing device re-appears to a device scan (not opened until a mount)
        if a.scan:
            for c in range(N):
                d = w.devs[c]
                if not d[PR] and not d[GONE] and not d[SCN]:
                    ws = w.copy()
                    ws.devs[c][SCN] = 1
                    res.append(("%s re-appears (device scan; no I/O until a mount)" % self.cn(c), ws, frozenset()))
        # the admin pulls a failing disk and remounts degraded (A-4's way out)
        if a.main_pull and w.cnt[1] < a.detach and w.mode != DOWN:
            for c in range(N):
                d = w.devs[c]
                if d[PR] and (d[BAD] or d[FB]) and not (self.P in ("C", "CB", "CTM") and d[FL]):
                    wp = w.copy()
                    self.persist_taint(wp)
                    wp.devs[c][PR] = 0
                    wp.devs[c][GONE] = 0 if a.ret else 1
                    wp.devs[c][BAD] = 0     # replugged later, it works again
                    wp.devs[c][FB] = 0
                    wp.cnt[1] += 1
                    if self.nmissing(wp) + self.nfailed(wp) <= self.npar:
                        for lab, wm, ev in self.mount(wp):
                            res.append(("admin pulls %s ; remount%s" % (self.cn(c), lab), wm, ev))
        # clean remount (device set may change)
        if w.cnt[2] < a.remount:
            wr = w.copy()
            wr.cnt[2] += 1
            for lab, wm, ev in self.mount_variants(wr, crashed=False):
                res.append(("unmount ; " + lab, wm, ev))
        # admin actions in the main search
        for lab, t2 in self.admin_actions(t, main=True):
            res.append((lab, W.of(t2), frozenset()))
        out = []
        for lab, wn, ev in res:
            out.append((lab, wn.tup() if isinstance(wn, W) else wn, ev))
        return out


# ------------------------------------------------------------------ search --

def run(m, depth, admin_depth, time_limit, want_admin=True):
    init = m.init()
    parent = {init: None}
    frontier = [init]
    stats = Counter()
    first = {}
    examples = {}
    run.examples = examples
    edges = Counter()
    t0 = time.time()
    d = 0
    truncated = False

    def note(metric, key, extra=""):
        stats[metric] += 1
        if metric not in first:
            first[metric] = (key, extra)
        lst = examples.setdefault(metric, [])
        if len(lst) < 400:
            lst.append((key, extra))

    def measure(t):
        w = W.of(t)
        silent, needless, lost = m.read_report(w)
        if silent:
            note("SILENT_WRONG", t, "cells %s" % silent)
            if not w.alert:
                note("SILENT_NOALERT", t, "cells %s" % silent)
        if needless:
            note("REFUSED_READ", t, "cells %s" % needless)
        if lost:
            note("LOST_INFO", t, "cells %s" % lost)
        if any(d[TN] or d[PEND] for d in w.devs):
            stats["STATES_TAINTED"] += 1
        if want_admin:
            dok, fok, uok = m.admin_eval(t, admin_depth)
            if not dok and not m.untainted(t) and m.data_ok(m.strip_taint(t)):
                # every acknowledged cell would read back right if the taint
                # were not there: the loss is the taint's coarseness alone
                note("LOST_TAINT_ONLY", t)
            if not dok:
                note("LOST_ACKED", t)
                if lost:
                    note("LOST_ACKED_GONE", t, "cells %s" % lost)
                else:
                    note("LOST_ACKED_UNREACHABLE", t)
            elif not fok:
                note("STUCK", t)
            elif not uok:
                note("TAINT_STUCK", t)
        if w.mode == RO:
            stats["STATES_RO"] += 1
        if w.mode == DOWN:
            stats["STATES_DOWN"] += 1

    import resource
    rss_limit = run.rss_limit_mb * 1024      # ru_maxrss is in KiB on Linux
    measure(init)
    count = 0
    while frontier and d < depth:
        nxt = []
        for t in frontier:
            count += 1
            if count % 2000 == 0:
                if resource.getrusage(resource.RUSAGE_SELF).ru_maxrss > rss_limit:
                    truncated = True
                    run.why = "memory"
                    break
            for lab, t2, ev in m.successors(t):
                for e in ev:
                    edges[e] += 1
                    if ("E_" + e) not in first:
                        first["E_" + e] = ((t, lab), "")
                if t2 in parent:
                    continue
                parent[t2] = (t, lab)
                nxt.append(t2)
                measure(t2)
            if time.time() - t0 > time_limit:
                truncated = True
                run.why = "time"
                break
        if truncated:
            break
        frontier = nxt
        d += 1
    return parent, stats, edges, first, d, truncated, time.time() - t0


def trace(parent, t):
    labs = []
    while parent.get(t) is not None:
        t, lab = parent[t]
        labs.append(lab)
    return list(reversed(labs))


def describe(m, t):
    w = W.of(t)
    parts = []
    for c in range(m.N):
        d = w.devs[c]
        flags = []
        if not d[PR]:
            flags.append("missing" + ("(gone)" if d[GONE] else ""))
        if d[BAD]:
            flags.append("bad")
        if d[FB]:
            flags.append("flushbad")
        if d[FL]:
            flags.append("FAILED")
        if d[TN]:
            flags.append("tainted" + ("" if d[TN] == 1 and not m.rsz else "(%s)" % bin(d[TN])))
        if d[PEND]:
            flags.append("taint-in-memory(%s)" % bin(d[PEND]))
        if d[SCN]:
            flags.append("scanned")
        parts.append("%s:%s" % (m.cn(c), "/".join(flags) or "ok"))
    st = []
    for s in range(m.S):
        acc = ["*" if a is None else "|".join(str(x) for x in sorted(a)) for a in w.acc[s]]
        st.append("s%d disk=%s par=%s acc=%s rec=%s%s" % (
            s, list(w.disk[s]), [list(p) for p in w.par[s]], acc, fmt_rec(w.rec[s]),
            (" replay-pending=%s" % (w.pnd[s],)) if w.pnd[s] else ""))
    al = "+".join(n for b, n in ((AL_DROP, "record_dropped"), (AL_UNFL, "log_unflushed")) if w.alert & b) or "-"
    sc, sok, gh = w.x
    xs = ""
    if sc is not None:
        xs += " scrub-cursor=s%d ok-regions=%s" % (sc, bin(sok))
    if gh is not None:
        xs += " old-%s-disk=%s%s" % (m.cn(gh[0]), list(gh[1]), " (tombstone)" if gh[2] else "")
    return "devs[%s] mode=%s alert=%s%s | %s" % (" ".join(parts), MODEN[w.mode], al, xs, " ; ".join(st))


METRICS = ("SILENT_WRONG", "SILENT_NOALERT", "LOST_ACKED", "LOST_ACKED_GONE", "LOST_ACKED_UNREACHABLE",
           "LOST_INFO", "REFUSED_READ", "STUCK", "TAINT_STUCK", "LOST_TAINT_ONLY")
EDGE_METRICS = ("REFUSED_WRITE", "RO", "DROPPED", "FAILDEV", "REPAIR_FAILED")


def make_parser():
    ap = argparse.ArgumentParser(description="RAID5/6 failure-policy comparison model")
    ap.add_argument("--policy", choices=POLICIES, required=True)
    ap.add_argument("--mut", action="append", choices=MUTS)
    ap.add_argument("--raid", type=int, choices=(5, 6), default=5)
    ap.add_argument("--stripes", type=int, default=2)
    ap.add_argument("--cap", type=int, default=0, help="log capacity in records (0: = stripes, never full)")
    ap.add_argument("--data", choices=("nodatasum", "csum", "mixed"), default="nodatasum")
    ap.add_argument("--meta", action="store_true", help="writes are metadata: checksummed, refusal -> RO")
    ap.add_argument("--bad", choices=("none", "transient", "persistent"), default="none")
    ap.add_argument("--flush", action="store_true", help="a device's flushes may fail")
    ap.add_argument("--unnamed-flush", action="store_true",
                    help="the log cannot name what a failed flush lost (wib_readd_dropped nr_unnamed)")
    ap.add_argument("--maxfault", type=int, default=1)
    ap.add_argument("--detach", type=int, default=0, help="devices that may go missing (runtime or at mount)")
    ap.add_argument("--ret", action="store_true", help="missing devices may return (at a mount)")
    ap.add_argument("--overfault", action="store_true",
                    help="a device may be lost beyond the tolerance (the filesystem is then ro,degraded)")
    ap.add_argument("--crash", type=int, default=1)
    ap.add_argument("--remount", type=int, default=1)
    ap.add_argument("--replay", action="store_true", help="a crash may leave a tree-log replay write")
    ap.add_argument("--replace", action="store_true")
    ap.add_argument("--no-spare", action="store_true",
                    help="the admin closure has no spare disk: no replace (LOST/STUCK judged without one)")
    ap.add_argument("--scrub", action="store_true")
    ap.add_argument("--repair", action="store_true")
    ap.add_argument("--depth", type=int, default=6)
    ap.add_argument("--admin-depth", type=int, default=3)
    ap.add_argument("--no-admin", action="store_true")
    ap.add_argument("--time", type=float, default=1500)
    ap.add_argument("--max-rss-mb", type=int, default=2500,
                    help="stop the search (TRUNCATED) once the process uses this much memory")
    ap.add_argument("--name", default="")
    ap.add_argument("--traces", action="store_true")
    ap.add_argument("--classes", default="", help="comma list of metrics: print distinct example traces")
    ap.add_argument("--maxclasses", type=int, default=6)
    ap.add_argument("--fresh-values", action="store_true",
                    help="a new value never equals one the column holds anywhere (no aliasing)")
    ap.add_argument("--nosymm", action="store_true", help="no stripe-permutation reduction (readable traces)")
    ap.add_argument("--taint-region", type=int, default=0)
    ap.add_argument("--recovery-clears", action="store_true",
                    help="with --taint-region 1 and a2_phaseA_tainted: mount recovery that writes a rebuilt value onto a "
                         "tainted column clears that stripe's taint")
    ap.add_argument("--taint-resync", action="store_true",
                    help="a mount rewrites the tainted regions of present devices at once (md re-add resync)")
    ap.add_argument("--replace-own", choices=("drop", "fail"), default="drop",
                    help="drop: a replace's own zeros records are ordinary records (policy_model); "
                         "fail: spent last, and spending one fails the replace (the kernel's replace_marks_lost)")
    ap.add_argument("--taint-clear", choices=("clean", "column"), default="clean",
                    help="clean: a region is untainted by a scrub pass that left every stripe of it clean "
                         "(A2 as proposed); column: once its column was rewritten from a trusted rebuild")
    ap.add_argument("--a2-persist", choices=("atomic", "dropfirst", "taintfirst"), default="atomic")
    ap.add_argument("--entry-stripes", type=int, default=1)
    ap.add_argument("--scrub-mode", choices=("atomic", "cursor", "cursor-noepoch"), default="atomic")
    ap.add_argument("--ghost", action="store_true")
    ap.add_argument("--ghost-events", type=int, default=1)
    ap.add_argument("--tombstone", choices=("none", "tainted", "all"), default="none")
    ap.add_argument("--die", type=int, default=0)
    ap.add_argument("--main-pull", action="store_true")
    ap.add_argument("--scan", action="store_true")
    return ap


def main():
    a = make_parser().parse_args()
    W.SYMM = not a.nosymm
    if a.taint_region or a.entry_stripes > 1 or a.scrub_mode != "atomic" or a.ghost:
        W.SYMM = False          # region/entry/cursor/ghost state is per stripe
    global FRESH
    FRESH = a.fresh_values
    m = Model(a)
    run.rss_limit_mb = a.max_rss_mb
    run.why = ""
    parent, stats, edges, first, d, trunc, el = run(m, a.depth, a.admin_depth, a.time, not a.no_admin)
    cfg = " ".join(x for x in sys.argv[1:] if x != "--traces")
    print("RESULT name=%s policy=%s states=%d depth=%d%s time=%.1fs %s" % (
        a.name or "-", a.policy, len(parent), d, (" TRUNCATED(%s)" % run.why) if trunc else "", el,
        " ".join("%s=%d" % (k, stats[k]) for k in METRICS) + " " +
        " ".join("%s=%d" % (k, edges[k]) for k in EDGE_METRICS) +
        " STATES_RO=%d STATES_DOWN=%d STATES_TAINTED=%d" % (stats["STATES_RO"], stats["STATES_DOWN"],
                                                            stats["STATES_TAINTED"])))
    print("CONFIG", cfg)
    if a.classes:
        import re
        for k in a.classes.split(","):
            seen = {}
            for key, extra in run.examples.get(k, []):
                tr = trace(parent, key)
                sig = tuple(re.sub(r"[0-9]", "#", re.sub(r"=\d+", "", x)).split(" ; ")[0][:40] for x in tr)
                if sig in seen:
                    continue
                seen[sig] = (tr, key, extra)
            for n, (sig, (tr, key, extra)) in enumerate(sorted(seen.items(), key=lambda kv: len(kv[1][0]))):
                if n >= a.maxclasses:
                    break
                print("CLASS %s #%d (%s): %s" % (k, n, extra, " || ".join(tr)))
                print("   state: %s" % describe(m, key))
    if a.traces:
        for k in METRICS + tuple("E_" + e for e in EDGE_METRICS):
            if k not in first:
                continue
            key, extra = first[k]
            if k.startswith("E_"):
                t, lab = key
                tr = trace(parent, t) + [lab]
                print("TRACE %s: %s" % (k, " || ".join(tr)))
                print("   from: %s" % describe(m, t))
            else:
                tr = trace(parent, key)
                print("TRACE %s (%s): %s" % (k, extra, " || ".join(tr)))
                print("   state: %s" % describe(m, key))
    return 0


if __name__ == "__main__":
    sys.exit(main())
