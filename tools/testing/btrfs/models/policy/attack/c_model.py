#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
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

ATTACK-C EXTENSIONS (c_model.py; every switch defaults off, so the base
model's results reproduce unchanged):
  --sb           per-device superblocks: generation (relative), the FAILED
                 list it carries, and the tree-log root it carries.  Every
                 write's commit is a transaction commit (TXN: generation+1,
                 all mirrors) or an fsync log commit (LOG: same generation,
                 primary only, log root); a crash may land the superblock
                 on any subset of the devices it is written to.  A mount
                 picks a present device with the highest generation; ties
                 are adversarial (volumes.c: strict '>' in list order).  A
                 superblock without the newest log root loses the writes
                 that only a log commit acknowledged: SILENT_LOGROOT.
                 Extra actions: 'txn commit' (periodic / devstate worker)
                 and 'CRASH (idle)'.  Clean unmounts commit first.
  --pending      C-family: the trigger makes the device FAILED-PENDING
                 (WRITE_FAILED in memory: writes skip it, no new names, the
                 existing names kept).  It becomes FAILED (durable, names
                 forgotten) only at a TXN commit whose superblock listed it
                 with zero superblock errors on the present non-failed
                 devices.  The gate while pending: no record is dropped or
                 evicted; a finished write's in-flight mark stays on disk
                 (residue, loaded as possibly torn); persist_now fails, so a
                 phase-A write-back refuses.  A mount applies the chosen
                 superblock's list (listed = FAILED durable, else healthy).
  --tfua         C-family: a log-block or superblock FUA that fails on a
                 present device admits it inline (T-fua, §1.2).
  --f1 R         the log-commit superblock rule (ERRATA 6 / model F1):
                 none (kernel), errata (C only: force a TXN commit when a
                 present device's FUA failed or it was admitted during that
                 write_all_supers), full (C only: also a present device
                 skipped as already WRITE_FAILED whose last superblock has
                 the current generation), any (every policy: any present
                 device the log commit's superblock did not reach).
  --e1all        C-family, ERRATA 1 as written: section 3.5 applies to
                 every record loaded at mount (not only possibly torn ones),
                 and forget keeps the entry as a vague sticky record.
  --nosrc        C-family, stage 1 as specified: F's content is never a
                 rebuild or RMW source, even when its checksum verifies
                 (direct verified reads of F's own sectors stay allowed).
  --recov-crash  a crash may hit mount recovery after any prefix of the
                 stripes it recovers (then another mount).
"""

import argparse
import itertools
import sys
import time
from collections import Counter

EIO = "EIO"
EMPTY = frozenset()
PR, BAD, FB, FL, TN, GONE, DUR, SG, SL, LR, ORPH = range(11)
NDF = 11
SG_MIN = -3
RES = "R"           # a finished write's in-flight mark kept on disk by the gate
# stored as (RES, cols): cols = the data columns the in-flight mark covers
# (narrow_inflight), EMPTY = the whole stripe
RW, RO, DOWN = 0, 1, 2
MODEN = {RW: "rw", RO: "ro", DOWN: "mount-failed"}
AL_DROP = 1
AL_UNFL = 2
AL_LOGLOST = 4      # a mount lost log-committed (fsync'd) writes: terminal
AL_SPLIT = 8        # a mount picked an abandoned (orphan) superblock lineage: terminal
TXN, LOG = "TXN", "LOG"


def newdev(present=1):
    d = [0] * NDF
    d[PR] = present
    return d


def is_res(torn):
    return isinstance(torn, tuple) and torn[0] == RES


def rt(torn):
    """the residue mark reads as possibly torn (narrow with narrow_inflight)"""
    if is_res(torn):
        return torn[1] if torn[1] else True
    return torn
ZERO = -9          # zeros a replace wrote where it could neither copy nor rebuild
VALS = (1, 2, 3, 4, 5)
POLICIES = ("OLD", "A", "CUR", "A2", "B", "C", "CB", "CTM")
MUTS = ("strict_degraded", "narrow_inflight", "evict_naming", "a2_no_taint", "c_trust_unverified", "no_suspect",
        "read_trust_torn", "absent_par_unnamed", "ack_unnamed", "b_no_record", "c_no_gate", "sd_csum", "no_tombstone")


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
    __slots__ = ("devs", "disk", "par", "acc", "rec", "pnd", "lg", "mode", "cnt", "alert", "gh")

    @staticmethod
    def of(t):
        w = W.__new__(W)
        devs, stripes, w.mode, cnt, w.alert, w.gh = t
        w.devs = [list(d) for d in devs]
        w.disk = [list(x[0]) for x in stripes]
        w.par = [list(x[1]) for x in stripes]
        w.acc = [list(x[2]) for x in stripes]
        w.rec = [x[3] for x in stripes]
        w.pnd = [x[4] for x in stripes]
        w.lg = [list(x[5]) for x in stripes]
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
        n.lg = [list(x) for x in self.lg]
        n.mode = self.mode
        n.cnt = list(self.cnt)
        n.alert = self.alert
        n.gh = self.gh
        return n

    SYMM = True

    def tup(self):
        if not W.SYMM:
            return (tuple(tuple(d) for d in self.devs),
                    tuple((tuple(self.disk[s]), tuple(self.par[s]), tuple(self.acc[s]), self.rec[s], self.pnd[s],
                           tuple(self.lg[s]))
                          for s in range(len(self.disk))), self.mode, tuple(self.cnt), self.alert, self.gh)
        stripes = tuple(sorted(
            ((tuple(self.disk[s]), tuple(self.par[s]), tuple(self.acc[s]), self.rec[s], self.pnd[s],
              tuple(self.lg[s]))
             for s in range(len(self.disk))), key=repr))
        return (tuple(tuple(d) for d in self.devs), stripes, self.mode, tuple(self.cnt), self.alert, self.gh)


def fmt_rec(r):
    if r is None:
        return "-"
    torn, names, verdict = r
    s = ",".join(str(c) for c in sorted(names))
    t = "" if not torn else ("T" if torn is True else ("Rsd" if is_res(torn) else
                                                       "T{%s}" % ",".join(str(c) for c in sorted(torn))))
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
        self.cfam = a.policy in ("C", "CB", "CTM")
        self.sb = a.sb
        self.pending = a.pending and self.cfam
        self.tfua = a.tfua and self.cfam
        self.f1 = a.f1
        self.e1all = a.e1all and self.cfam
        self.nosrc = a.nosrc and self.cfam
        self.recov_crash = a.recov_crash
        if self.pending and not self.sb:
            raise SystemExit("--pending needs --sb")

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

    def src_ok(self, w, c, csum_cell=False):
        """may the content of column c be used as a source without verification"""
        d = w.devs[c]
        if not d[PR]:
            return False
        if d[TN]:
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
        torn, names, verdict = w.rec[s]
        return rt(torn), names, verdict

    def gated(self, w):
        """a device is FAILED-PENDING: the gate is closed (no drops)"""
        return self.pending and "c_no_gate" not in self.muts and any(d[FL] and not d[DUR] for d in w.devs)

    def srcable(self, w, c):
        """--nosrc: may F's (verified) content feed a rebuild of another column"""
        return not (self.nosrc and self.is_failed(w, c))

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
        if self.nosrc:
            base_holes = sorted(set(base_holes) | {j for j in range(self.nd)
                                                   if j != i and not self.srcable(w, j)})
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
        if not self.src_ok(w, j, cs):
            return None
        if not self.srcable(w, j):
            return None
        if self.log and j in names and not cs:
            return None
        v = w.disk[s][j]
        if cs and not self.verified(w, s, j, v):
            return None
        return v

    def usable_pars(self, w, s, names):
        return [p for p in range(self.npar)
                if self.src_ok(w, self.nd + p) and not (self.log and (self.nd + p) in names)]

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
            if d[TN] and not cs:
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
        """list of (stripe, taint_devices) the policy may drop, lowest pass first"""
        if not self.log:
            return []
        if self.gated(w):
            return []           # I3: no log drop anywhere while a failure is pending
        missing = {c for c in range(self.N) if not w.devs[c][PR]}
        recs = [(s, r) for s, r in enumerate(w.rec) if r is not None and s != busy]
        vague = [(s, ()) for s, r in recs if not r[0] and not r[1] and not r[2]]
        if vague:
            return vague
        P = self.P
        if P == "CUR" or "evict_naming" in self.muts:
            may = bool(missing) or replay or "evict_naming" in self.muts
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
            c = [(s, tuple(sorted(r[1]))) for s, r in recs
                 if r[1] and not r[0] and not r[2] and r[1] <= missing]
            if P == "A2" and "a2_no_taint" not in self.muts:
                return c
            return [(s, ()) for s, _ in c]
        return []

    def do_evict(self, w, e, taint):
        w.rec[e] = None
        w.alert |= AL_DROP
        for c in taint:
            w.devs[c][TN] = 1

    # --------------------------------------------------------- C: failing --
    def fail_device(self, w, c):
        """C/CTM admission.  Atomic model: FAILED durably, names forgotten.
        --pending: FAILED-PENDING (WRITE_FAILED in memory) only."""
        w.devs[c][FL] = 1
        if self.pending:
            return
        self.make_durable(w, c)

    def make_durable(self, w, c):
        """WF_DURABLE: forget the names on its column (sticky kept vague
        with --e1all, as spec 2.4 writes it); once no failure is pending,
        the gate opens and the residue marks go"""
        w.devs[c][DUR] = 1
        for s in range(self.S):
            r = w.rec[s]
            if r is None:
                continue
            torn, names, verdict = r
            names = names - {c}
            if not names and not torn and not verdict:
                w.rec[s] = (False, EMPTY, False) if (self.e1all and r[1]) else None
            else:
                w.rec[s] = (torn, names, verdict)
        # the residue marks stay on disk until the next log write after the
        # gate opened (sb_commit): a crash in between still loads them

    def open_gate(self, w):
        for s in range(self.S):
            r = w.rec[s]
            if r is not None and is_res(r[0]):
                w.rec[s] = (False, r[1], r[2]) if (r[1] or r[2]) else None

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
                if not self.src_ok(w, j, j in self.csum) and w.acc[s][j] is not None:
                    return [refused("degraded-rmw", w)]
                if "sd_csum" in self.muts and w.acc[s][j] is not None and \
                        (self.is_failed(w, j) or w.devs[j][TN]):
                    # a checksummed column on a failed/tainted device whose
                    # content does not verify must be rebuilt too
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
        tfua_tag = ""
        if self.log and self.tfua:
            # the in-flight mark's log block is written to every device: a
            # device whose writes fail fails it, and T-fua admits it inline;
            # the rbio re-samples after the mark (spec 2.1, point 1)
            for c in range(self.N):
                dv = w.devs[c]
                if dv[PR] and dv[BAD] and not dv[FL] and self.absent_budget_ok(w, 1):
                    self.fail_device(w, c)
                    tfua_tag += " T-fua(log) fails %s" % self.cn(c)
            if tfua_tag:
                tag += tfua_tag
                torn, names, verdict = self.recinfo(w, s)
        if self.log and self.pending and not self.gated(w):
            self.open_gate(w)       # this log write may drop what finished
        if self.log:
            if not self.log_can_write(w):
                return [refused("log-write", w)]
            if w.rec[s] is None:
                tlog = ""
                if self.nrec(w) >= self.cap and self.P in ("C", "CB", "CTM"):
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
                if w.rec[s] is None and self.nrec(w) >= self.cap:
                    cands = self.evict_candidates(w, s, replay_mode)
                    if not cands:
                        return [refused("log-full", w)]
                    if probe:
                        cands = cands[:1]
                    branches = []
                    for e, taint in cands:
                        we = w.copy()
                        self.do_evict(we, e, taint)
                        lab = " evict s%d%s%s" % (e, fmt_rec(w.rec[e]),
                                                  (" taint" + self.cset(taint)) if taint else "")
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
            torn = rt(torn)
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
                     and not w.devs[j][TN]]
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
                    if not self.log_can_write(w) or self.gated(w):
                        # (gated: btrfs_wib_persist_now returns -EIO while a
                        # failure is pending, spec 2.4)
                        wr = w.copy()
                        wr.rec[s] = (torn, names, verdict) if (torn or names or verdict) else None
                        lab, wr2, ev = refused("persist" + ("-gated" if self.gated(w) else ""), wr)
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
            # quiet columns (WRITE_FAILED): neither marked nor cleared; a
            # pending device's existing names stay until it is durable
            nn = {c for c in nn if not self.is_failed(w, c)} | \
                {c for c in names if self.is_failed(w, c) and not w.devs[c][DUR]}
            if ok:
                # the parities that landed describe the stripe again
                ntorn = bool(unnamed)
                nverdict = False
                if not ntorn and self.gated(w):
                    # the gate: this commit's log block may not omit the
                    # write's in-flight mark (flushed &= !gate)
                    ntorn = (RES, frozenset([i]) if (i is not None and "narrow_inflight" in self.muts)
                             else EMPTY)
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
        if self.sb and i is not None and ctx != "replay":
            # the commit's superblocks: TXN or LOG, crash at any landing
            lab0 = tag
            if failedB:
                lab0 += " (write to %s failed)" % self.cset(failedB)
            if flushlost:
                lab0 += " (flush lost %s)" % self.cset(flushlost)
            if tflush_failed:
                ev.add("FAILDEV")
            kinds = (TXN,) if (probe or self.a.sb_txn_only) else (TXN, LOG)
            for kind in kinds:
                for sfx, wk, evk, acked in self.sb_commit(w, kind, can_crash, tflush_failed,
                                                          cell=(s, i, v)):
                    if not acked:
                        out.append((lab0 + sfx, wk, frozenset(ev) | evk))
                        continue
                    out.append((lab0 + sfx + " ok", wk, frozenset(ev) | evk))
            return out
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
        out.append((lab, w, frozenset(ev)))
        return out

    # ------------------------------------------------------- superblocks --
    def sl_now(self, w):
        return sum(1 << c for c in range(self.N) if w.devs[c][FL])

    def sb_commit(self, w0, kind, can_crash, tflush=(), cell=None):
        """write_all_supers of a commit of `kind` after the op's log block.
        Returns [(label suffix, W, events, acked)]: crash outcomes (already
        mounted) and, last, the committed state (acked=True; with cell
        (s, i, v) the write is acknowledged in it)."""
        res = []
        w = w0.copy()
        N = self.N
        if self.pending and not self.gated(w):
            self.open_gate(w)       # btrfs_wib_commit drops what finished
        sl = self.sl_now(w)
        targets = [c for c in range(N) if w.devs[c][PR] and not w.devs[c][FL]]
        fua_failed = [c for c in targets if w.devs[c][BAD]]
        landed = [c for c in targets if not w.devs[c][BAD]]
        errors = bool(fua_failed)
        lab = " ; %s commit" % kind
        tfua = []
        if self.tfua:
            for c in fua_failed:
                if self.absent_budget_ok(w, 1):
                    self.fail_device(w, c)
                    tfua.append(c)
            if tfua:
                lab += " T-fua(sb) fails %s" % self.cset(tfua)
        lseq = w.cnt[4] + 1 if kind == LOG else 0

        def unacked(wc):
            if cell is not None:
                s, i, v = cell
                a = wc.acc[s][i]
                if a is not None:
                    a2 = a | {v}
                    wc.acc[s][i] = a2 if len(a2) <= 3 else None

        def land(wc, X, final):
            for c in X:
                d = wc.devs[c]
                d[SL] = sl
                d[ORPH] = 0
                if kind == TXN:
                    d[SG] = 1
                    d[LR] = 0
                else:
                    d[SG] = 0
                    d[LR] = lseq
            if kind == TXN and final:
                for c in range(N):
                    d = wc.devs[c]
                    d[SG] = max(d[SG] - 1, SG_MIN)
                for s2 in range(self.S):
                    wc.lg[s2] = [None] * self.nd
                wc.cnt[4] = 0

        if can_crash:
            for k in range(len(landed)):
                for X in itertools.combinations(landed, k):
                    wc = w.copy()
                    unacked(wc)
                    land(wc, X, False)
                    for l2, wm, ev in self.crash_outcomes(wc, lab + " sb landed %s ; CRASH" % self.cset(X), ()):
                        res.append((l2, wm, ev, False))
        land(w, landed, True)
        if kind == LOG:
            w.cnt[4] = lseq
        if kind == TXN and not errors:
            for c in range(N):
                if w.devs[c][FL] and not w.devs[c][DUR] and (sl >> c) & 1:
                    self.make_durable(w, c)
                    lab += " (%s durable)" % self.cn(c)
        if not landed:
            # every superblock failed: write_all_supers aborts
            w.mode = RO
            res.append((lab + " no superblock landed -> ABORT RO", w, frozenset({"RO"}), False))
            return res
        # ERRATA 6 / model F1: a log commit whose superblock missed a device
        # that still holds this generation forces a transaction commit
        if kind == LOG and self.f1 != "none":
            force = []
            for c in range(N):
                d = w.devs[c]
                if c in landed:
                    continue
                if not d[PR]:
                    # a missing device keeps its last superblock: same
                    # generation, no log root, no list entry
                    if self.a.f1_missing and d[SG] == 0 and self.f1 != "none":
                        force.append(c)
                    continue
                cur_gen = c in fua_failed or d[SG] == 0
                if self.f1 == "any":
                    if cur_gen:
                        force.append(c)
                elif self.cfam:
                    if c in fua_failed or (c in tflush and d[SG] == 0):
                        force.append(c)
                    elif self.f1 == "full" and d[FL] and d[SG] == 0:
                        force.append(c)
            if force:
                for sfx, wk, evk, acked in self.sb_commit(w, TXN, can_crash, (), cell):
                    res.append((lab + " (missed %s: FORCE_COMMIT)" % self.cset(force) + sfx, wk, evk, acked))
                return res
        if cell is not None:
            s, i, v = cell
            w.acc[s][i] = frozenset([v])
            for c in range(N):
                if w.devs[c][ORPH] == 1:
                    w.devs[c][ORPH] = 2     # the abandoned lineage now misses an acknowledged write
            if kind == LOG:
                w.lg[s][i] = (0, lseq)
            else:
                w.lg[s][i] = None
        ev = {"FAILDEV"} if tfua else set()
        res.append((lab, w, frozenset(ev), True))
        return res

    def clean_commit(self, w):
        """a commit with nothing in flight and no crash (unmount, mount end)"""
        if w.mode != RW:
            return w
        r = self.sb_commit(w, TXN, False)
        return r[-1][1]

    def sb_choices(self, w):
        """the superblocks a mount may pick: highest generation among the
        present devices; ties adversarial.  [(label, (SG, SL, LR))]"""
        cands = [c for c in range(self.N) if w.devs[c][PR]]
        if not cands:
            return []
        g = max(w.devs[c][SG] for c in cands)
        seen = {}
        for c in cands:
            d = w.devs[c]
            if d[SG] == g:
                key = (d[SG], d[SL], d[LR], d[ORPH])
                seen.setdefault(key, []).append(c)
        if len(seen) == 1:
            return [("", k) for k in seen]
        return [(" (tie: sb of %s)" % self.cset(cs), k) for k, cs in sorted(seen.items())]

    # -------------------------------------------------------------- crash --
    def crash_outcomes(self, wc, tag, ev0):
        """crash image -> every mount that may follow"""
        res = []
        wc.cnt[0] += 1
        for lab, wm, ev in self.mount_variants(wc, crashed=True):
            res.append((tag + " ; " + lab, wm, frozenset(ev0) | ev))
        return res

    def mount_any(self, w):
        """mount with every superblock the mount may pick (--sb)"""
        if not self.sb:
            return self.mount(w)
        res = []
        for lab, ch in self.sb_choices(w):
            res.extend(self.mount(w, (lab, ch)))
        return res

    def mount_variants(self, w, crashed):
        """choices of the device set at the mount, then the mount itself"""
        res = []
        if self.sb and not crashed:
            w = self.clean_commit(w)        # the unmount's final commit
        opts = [(w, "mount")]
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
        # the replaced disk shows up at a mount where T is missing
        if self.a.ghost and w.gh is not None and w.cnt[1] < self.a.detach:
            gd, gcol, gsg = w.gh
            if w.devs[gd][PR]:
                wg = w.copy()
                wg.cnt[1] += 1
                wg.gh = None
                if self.cfam and "no_tombstone" not in self.muts:
                    # spec 1.4: a device claiming a tombstoned devid with a
                    # superblock generation <= sb_bound is treated as absent
                    wg.devs[gd][PR] = 0
                    wg.devs[gd][GONE] = 1
                    lab = "mount without T(%s), old %s refused by its tombstone" % (self.cn(gd), self.cn(gd))
                else:
                    nd_ = newdev(1)
                    nd_[SG] = gsg
                    wg.devs[gd] = nd_
                    for s in range(self.S):
                        if gd < self.nd:
                            wg.disk[s][gd] = gcol[s]
                        else:
                            wg.par[s][gd - self.nd] = gcol[s]
                    lab = "mount without T(%s), the old %s accepted as devid %s" % (self.cn(gd), self.cn(gd),
                                                                                 self.cn(gd))
                if self.nmissing(wg) + self.nfailed(wg) <= self.npar:
                    opts.append((wg, lab))
        # returning devices
        if self.a.ret:
            back = [c for c in range(self.N) if not w.devs[c][PR] and not w.devs[c][GONE]]
            if back:
                wb = w.copy()
                for c in back:
                    wb.devs[c][PR] = 1
                opts.append((wb, "mount with %s back" % self.cset(back)))
        for wo, lab in opts:
            for l2, wm, ev in self.mount_any(wo):
                res.append((lab + l2, wm, ev))
        return res

    def mount(self, w, choice=None):
        """mount recovery (and tree-log replay); returns [(label, W, events)]"""
        w = w.copy()
        w.mode = RW
        P = self.P
        lab = ""
        replayed_log = False
        if self.sb and choice is not None:
            clab, (sgc, slc, lrc, orph) = choice
            lab += clab
            if orph == 2:
                # the superblock of a commit that crashed after landing only
                # on this device, which was missing when the filesystem went
                # on (degraded) from the older generation and has since
                # written the same generation again: the mount takes an
                # abandoned history.  Everything the degraded session
                # committed is gone (and its tree blocks may be overwritten
                # with blocks of the same generation), with no error.
                w.alert |= AL_SPLIT
                return [(lab + " ORPHAN SUPERBLOCK: the degraded session's commits are silently gone", w,
                         frozenset({"SPLIT"}))]
            if orph == 1:
                for c in range(self.N):
                    w.devs[c][ORPH] = 0     # nothing acknowledged since: the orphan's history is adopted
            lost = []
            for s in range(self.S):
                for i in range(self.nd):
                    e = w.lg[s][i]
                    if e is None:
                        continue
                    ag, aq = e
                    if not (sgc > ag or (sgc == ag and lrc >= aq)):
                        lost.append((s, i))
            if lost:
                # the picked superblock lacks the tree-log root that
                # acknowledged these fsyncs: the files come back without
                # them, and no error says so
                w.alert |= AL_LOGLOST
                return [(lab + " LOG ROOT MISSING: fsync'd %s silently lost" % lost, w,
                         frozenset({"LOGLOST"}))]
            replayed_log = any(e is not None for x in w.lg for e in x)
            for c in range(self.N):
                d = w.devs[c]
                if not d[PR] and d[SG] > sgc and not d[ORPH]:
                    d[ORPH] = 1
                d[SG] = max(d[SG] - sgc, SG_MIN)
            for s in range(self.S):
                w.lg[s] = [None] * self.nd
            w.cnt[4] = 0
            # the FAILED list of the picked superblock (spec 1.6): listed
            # devices are FAILED (durable) and forgotten before recovery; a
            # failure that no picked superblock carries is gone
            listed = [c for c in range(self.N) if (slc >> c) & 1]
            for c in range(self.N):
                d = w.devs[c]
                if c in listed:
                    d[FL] = 1
                elif d[FL]:
                    d[FL] = 0
                    d[DUR] = 0
                    lab += " (%s not listed: trusted)" % self.cn(c)
            ro_mount = self.nmissing(w) + self.nfailed(w) > self.npar
            # a residue on disk is an in-flight mark: loaded as possibly torn
            # (--residue-smart: its 'finished' flag lets a mount that lists
            # the pending device drop it)
            for s in range(self.S):
                r = w.rec[s]
                if r is not None and is_res(r[0]):
                    if self.a.residue_smart and listed and not ro_mount:
                        w.rec[s] = (False, r[1], r[2]) if (r[1] or r[2]) else None
                    else:
                        w.rec[s] = (rt(r[0]), r[1], r[2])
            if self.a.mount_commit and not ro_mount:
                # proposed fix: make the picked superblock (its list, and
                # T-missing's entries) durable on every present device before
                # anything is forgotten or recovered on its strength
                # (skipping a generation when degraded, with --gen-skip)
                if P == "CTM":
                    for c in range(self.N):
                        if not w.devs[c][PR] and not w.devs[c][FL]:
                            self.fail_device(w, c)
                            lab += " T-missing fails %s" % self.cn(c)
                if self.a.gen_skip and self.nmissing(w) > 0:
                    for c in range(self.N):
                        w.devs[c][SG] = max(w.devs[c][SG] - 1, SG_MIN)
                for c in listed:
                    w.devs[c][DUR] = 0
                w = self.clean_commit(w)
                lab += " (mount commit)"
            else:
                for c in listed:
                    if ro_mount:
                        w.devs[c][DUR] = 1      # in memory only: no persist_now on a ro mount
                    else:
                        self.make_durable(w, c)
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
        extra = []
        if self.log:
            for s in range(self.S):
                r = w.rec[s]
                if r is None:
                    continue
                if replay and (r[1] or r[2]):
                    continue        # error records wait for the replay
                self.recover_stripe(w, s)
                if self.recov_crash and w.cnt[0] < self.a.crash and w.rec[s] != r:
                    # a crash right after this stripe's recovery landed
                    wc = w.copy()
                    extra.extend(self.crash_outcomes(wc, lab + " ; recovered s%d ; CRASH during recovery" % s, ()))
        if self.sb and self.a.gen_skip and not self.a.mount_commit and w.mode == RW and self.nmissing(w) > 0:
            for c in range(self.N):
                w.devs[c][SG] = max(w.devs[c][SG] - 1, SG_MIN)
            w = self.clean_commit(w)
            lab += " (degraded: gen-skip commit)"
        elif self.sb and replayed_log and w.mode == RW:
            w = self.clean_commit(w)        # the log replay ends with a commit
        if not replay:
            return [(lab, w, frozenset())] + extra
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
        torn = rt(torn)
        nd, npar = self.nd, self.npar
        absent_data = [j for j in range(nd) if not self.src_ok(w, j, j in self.csum)
                       and not (j in self.csum and w.devs[j][PR] and self.is_failed(w, j))]
        missing_par = [nd + p for p in range(npar) if not w.devs[nd + p][PR]]
        names = set(names)
        if torn and "absent_par_unnamed" not in self.muts:
            names |= set(missing_par)
        if not absent_data:
            vals = list(w.disk[s])
            holes = [j for j in range(nd) if (j in names) or
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
        if (torn or self.e1all) and not verdict:
            # spec 3.5 (--e1all: ERRATA 1, every loaded record)
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
        torn, names, verdict = self.recinfo(w, s)
        names = set(names)
        anymissing = any(not w.devs[c][PR] for c in range(self.N))
        vals = list(w.disk[s])
        holes = []
        for j in range(nd):
            cs = j in self.csum
            if not w.devs[j][PR] or w.devs[j][TN] or (self.is_failed(w, j) and not cs and
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
            elif not self.is_failed(w, j):
                newnames.add(j)
        clean = True
        if not anymissing:
            vec = tuple(vals)
            for p in range(npar):
                c = nd + p
                if self.writable(w, c) and not w.devs[c][BAD]:
                    w.par[s][p] = vec
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

    def scrub(self, w):
        w = w.copy()
        allok = True
        gated = self.gated(w)
        keep = list(w.rec)
        for s in range(self.S):
            if not self.scrub_stripe(w, s):
                allok = False
        if gated:
            w.rec = keep            # the gate: the scrub drops no record
        if allok:
            for c in range(self.N):
                if w.devs[c][TN] and w.devs[c][PR] and not w.devs[c][BAD]:
                    w.devs[c][TN] = 0
        return w

    # ------------------------------------------------------------ replace --
    def replace(self, w0, d):
        """replace device d by a new one T; None if the replace fails.  Per
        full stripe, T's column is a verified/trusted copy of d, else a
        rebuild without d, else zeros recorded stale (log policies)."""
        w = w0.copy()
        nd, npar = self.nd, self.npar
        src = list(w.devs[d])
        if self.a.ghost:
            # the replaced disk keeps a valid superblock (devid N, T's
            # uuid) unless the finishing scratch reached it: it cannot if
            # it is absent or its writes fail (dev-replace.c:1058-1059
            # scratches only a WRITEABLE source; the design scratches
            # best effort, errors ignored)
            if not src[PR] or src[BAD] or src[FB]:
                col = tuple((w.disk[s][d] if d < nd else w.par[s][d - nd]) for s in range(self.S))
                w.gh = (d, col, src[SG])
            else:
                w.gh = None
        for s in range(self.S):
            torn, names, verdict = self.recinfo(w, s)
            val = None
            if d < nd:
                cs = d in self.csum
                if src[PR] and not src[TN]:
                    trusted = not (self.log and d in names and not cs)
                    if self.is_failed(w, d) and not cs and "c_trust_unverified" not in self.muts:
                        trusted = False
                    if trusted:
                        vv = w.disk[s][d]
                        if not cs or self.verified(w, s, d, vv):
                            val = vv
                if val is None:
                    w.devs[d] = newdev(0)      # never a source
                    val = self.pread(w, s, d) if w.acc[s][d] is not None else w.disk[s][d]
                    w.devs[d] = list(src)
            else:
                w.devs[d] = newdev(0)
                vec = [self.pread(w, s, j) if w.acc[s][j] is not None else w.disk[s][j]
                       for j in range(nd)]
                w.devs[d] = list(src)
                val = EIO if any(x == EIO for x in vec) else tuple(vec)
            if val == EIO:
                if self.log:
                    if w.rec[s] is None and self.nrec(w) >= self.cap:
                        cands = self.evict_candidates(w, s, False)
                        if not cands:
                            return None         # the replace fails: nowhere to record the zeros
                        self.do_evict(w, cands[0][0], cands[0][1])
                    t0, n0, v0 = w.rec[s] if w.rec[s] is not None else (False, EMPTY, False)
                    w.rec[s] = (t0, n0 | {d}, v0)
                val = ZERO if d < nd else (ZERO,) * nd
            elif self.log and w.rec[s] is not None:
                t0, n0, v0 = w.rec[s]
                n0 = n0 - {d}
                w.rec[s] = (t0, n0, v0) if (t0 or n0 or v0) else None
            if d < nd:
                w.disk[s][d] = val
            else:
                w.par[s][d - nd] = val
        w.devs[d] = newdev(1)
        if self.sb:
            w = self.clean_commit(w)        # the finishing commit
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
            if self.sb:
                wb = self.clean_commit(wb)
            for c in back:
                wb.devs[c][PR] = 1
            for lab, wm, ev in self.mount_any(wb):
                res.append(("admin return %s ; %s" % (self.cset(back), lab), wm.tup()))
        if not main:
            for c in bad:
                if self.P in ("C", "CB", "CTM") and w.devs[c][FL]:
                    continue
                wp = w.copy()
                if self.sb:
                    wp = self.clean_commit(wp)
                wp.devs[c][PR] = 0
                wp.devs[c][GONE] = 1
                if self.nmissing(wp) + self.nfailed(wp) <= self.npar:
                    for lab, wm, ev in self.mount_any(wp):
                        res.append(("admin pull %s ; remount%s" % (self.cn(c), lab), wm.tup()))
            if w.mode != RW or any(r is not None for r in w.rec) or self.gated(w):
                wm0 = self.clean_commit(w) if self.sb else w
                for lab, wm, ev in self.mount_any(wm0):
                    res.append(("admin remount%s" % lab, wm.tup()))
            if self.sb and w.mode == RW:
                wc = self.clean_commit(w)
                res.append(("admin sync (txn commit)", wc.tup()))
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
            if not main or a.scrub:
                res.append(("scrub", self.scrub(w).tup()))
            if (not main or a.replace) and not (a.no_spare and not main):
                for c in range(N):
                    dv = w.devs[c]
                    if not dv[PR] or dv[BAD] or dv[FB] or dv[TN] or self.is_failed(w, c):
                        if not dv[PR] and self.nmissing(w) > self.npar:
                            continue
                        wr = self.replace(w, c)
                        if wr is not None:
                            res.append(("replace %s" % self.cn(c), wr.tup()))
            if self.P in ("C", "CB", "CTM"):
                cands = bad
                if main and self.a.admin_fail_any:
                    # T-admin: the admin retires any present disk
                    cands = [c for c in range(N) if w.devs[c][PR]]
                for c in cands:
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

    def admin_eval(self, t, depth):
        key = (t, depth)
        r = self.memo_admin.get(key)
        if r is not None:
            return r
        d_ok = self.data_ok(t)
        f_ok = d_ok and self.full_avail(t)
        if not f_ok and depth > 0:
            for lab, t2 in self.admin_actions(t):
                if t2 == t:
                    continue
                d2, f2 = self.admin_eval(t2, depth - 1)
                d_ok = d_ok or d2
                f_ok = f_ok or f2
                if f_ok:
                    break
        r = (d_ok, f_ok)
        self.memo_admin[key] = r
        return r

    # --------------------------------------------------------- main edges --
    def init(self):
        w = W.__new__(W)
        w.devs = [newdev(1) for _ in range(self.N)]
        w.disk = [[1] * self.nd for _ in range(self.S)]
        w.par = [[tuple([1] * self.nd)] * self.npar for _ in range(self.S)]
        w.acc = [[frozenset([1])] * self.nd for _ in range(self.S)]
        w.rec = [None] * self.S
        w.pnd = [None] * self.S
        w.lg = [[None] * self.nd for _ in range(self.S)]
        w.mode = RW
        w.cnt = [0, 0, 0, 0, 0]
        w.alert = 0
        w.gh = None
        return w.tup()

    def successors(self, t):
        a = self.a
        w = W.of(t)
        res = []
        N = self.N
        if w.alert & (AL_LOGLOST | AL_SPLIT):
            return []           # counted; the fs no longer has the acknowledged data
        if self.sb:
            if w.mode == RW:
                # periodic transaction commit / the devstate worker's commit
                for sfx, wk, evk, acked in self.sb_commit(w, TXN, w.cnt[0] < a.crash):
                    if acked and wk.tup() == t:
                        continue
                    res.append(("txn commit" + sfx, wk, evk))
            if w.cnt[0] < a.crash and w.mode != DOWN:
                wc = w.copy()
                res.extend(self.crash_outcomes(wc, "CRASH (idle)", ()))
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
        if w.alert & AL_LOGLOST:
            note("SILENT_WRONG", t, "fsync'd writes lost with the log root")
            note("SILENT_LOGROOT", t, "")
            return
        if w.alert & AL_SPLIT:
            note("SILENT_WRONG", t, "an orphan superblock lineage was mounted")
            note("SILENT_SPLIT", t, "")
            return
        silent, needless, lost = m.read_report(w)
        if silent:
            note("SILENT_WRONG", t, "cells %s" % silent)
            if not w.alert:
                note("SILENT_NOALERT", t, "cells %s" % silent)
        if needless:
            note("REFUSED_READ", t, "cells %s" % needless)
        if lost:
            note("LOST_INFO", t, "cells %s" % lost)
        if want_admin:
            dok, fok = m.admin_eval(t, admin_depth)
            if not dok:
                note("LOST_ACKED", t)
                if lost:
                    note("LOST_ACKED_GONE", t, "cells %s" % lost)
                else:
                    note("LOST_ACKED_UNREACHABLE", t)
            elif not fok:
                note("STUCK", t)
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
            flags.append("FAILED" if (d[DUR] or not m.pending) else "FAILED-PENDING")
        if d[TN]:
            flags.append("tainted")
        if m.sb:
            flags.append("sb(g%d%s,list%s%s)" % (d[SG], ",ORPHAN" if d[ORPH] else "",
                                                 m.cset([x for x in range(m.N) if (d[SL] >> x) & 1]),
                                              (",log%d" % d[LR]) if d[LR] else ""))
        parts.append("%s:%s" % (m.cn(c), "/".join(flags) or "ok"))
    st = []
    for s in range(m.S):
        acc = ["*" if a is None else "|".join(str(x) for x in sorted(a)) for a in w.acc[s]]
        st.append("s%d disk=%s par=%s acc=%s rec=%s%s" % (
            s, list(w.disk[s]), [list(p) for p in w.par[s]], acc, fmt_rec(w.rec[s]),
            (" replay-pending=%s" % (w.pnd[s],)) if w.pnd[s] else "") +
            ((" logacked=%s" % [e for e in w.lg[s]]) if any(e is not None for e in w.lg[s]) else ""))
    al = "+".join(n for b, n in ((AL_DROP, "record_dropped"), (AL_UNFL, "log_unflushed"),
                                 (AL_LOGLOST, "LOG_ROOT_LOST"), (AL_SPLIT, "ORPHAN_MOUNTED")) if w.alert & b) or "-"
    return "devs[%s] mode=%s alert=%s | %s" % (" ".join(parts), MODEN[w.mode], al, " ; ".join(st))


METRICS = ("SILENT_WRONG", "SILENT_NOALERT", "SILENT_LOGROOT", "SILENT_SPLIT", "LOST_ACKED", "LOST_ACKED_GONE",
           "LOST_ACKED_UNREACHABLE", "LOST_INFO", "REFUSED_READ", "STUCK")
EDGE_METRICS = ("REFUSED_WRITE", "RO", "DROPPED", "FAILDEV", "REPAIR_FAILED", "LOGLOST", "SPLIT")


def main():
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
    ap.add_argument("--sb", action="store_true", help="per-device superblocks, TXN/LOG commits, adversarial ties")
    ap.add_argument("--sb-txn-only", action="store_true", help="--sb with transaction commits only (no fsync log)")
    ap.add_argument("--pending", action="store_true", help="C: FAILED-PENDING window with the gate (needs --sb)")
    ap.add_argument("--tfua", action="store_true", help="C: a failed log/superblock FUA admits the device inline")
    ap.add_argument("--f1", choices=("none", "errata", "full", "any"), default="none",
                    help="log-commit superblock rule (ERRATA 6 / model F1)")
    ap.add_argument("--e1all", action="store_true", help="C: ERRATA 1 as written (3.5 on every loaded record)")
    ap.add_argument("--nosrc", action="store_true", help="C: F never a rebuild/RMW source (stage 1 as specified)")
    ap.add_argument("--recov-crash", action="store_true", help="a crash may interrupt mount recovery")
    ap.add_argument("--ghost", action="store_true",
                    help="a replaced disk whose superblock survived may show up instead of T at a mount")
    ap.add_argument("--f1-missing", action="store_true",
                    help="the --f1 rule also forces a commit for a MISSING device holding the current generation")
    ap.add_argument("--mount-commit", action="store_true",
                    help="a rw mount commits the picked superblock (list) to every present device before recovery")
    ap.add_argument("--gen-skip", action="store_true",
                    help="a degraded rw mount commits at once, skipping one generation (orphan-superblock fence)")
    ap.add_argument("--admin-fail-any", action="store_true",
                    help="C: the main search may admit any present device (T-admin on a healthy disk)")
    ap.add_argument("--residue-smart", action="store_true",
                    help="C --pending, proposed fix: gated finished entries are dropped at a mount that lists F")
    a = ap.parse_args()
    W.SYMM = not a.nosymm
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
        " STATES_RO=%d STATES_DOWN=%d" % (stats["STATES_RO"], stats["STATES_DOWN"])))
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
