#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
sched.py -- compare policies on the SAME adversarial schedules.

The exhaustive runs of b_model.py count distinct states, which depend on how
far each policy's state space grows, so their magnitudes are not comparable
across policies.  Here the adversary's choices are fixed and policy-independent:
a schedule is a sequence of operations (fault injection, writes, repairs,
crashes with a given device set at the following mount, device loss/return,
scrub, replace).  Each policy runs the same schedule; the model's own
non-determinism inside an operation (every crash point inside the write,
every subset of phase-B device writes that landed, every eviction choice) is
enumerated.  Each (schedule, policy) then has a set of end states ("leaves"),
each classified by b_model's own metrics:
  SILENT      a read returns a value other than the acknowledged one, no error
  GONE        LOST_ACKED and the acknowledged value is on no disk any more
  UNREACH     LOST_ACKED, the value is still on disk but no admin action
              (heal/return/pull/remount/scrub/replace/fail, <= 3) reads it
  STUCK       data intact but no admin sequence restores full write service
  RO          the filesystem went read-only or a mount failed on the path
  REFUSED     some write on the path failed with EIO
  OK          none of the above
C and CB fail a device the way md does: on its first write error (the first
time a record names the failing device), if the tolerance allows.

Every prefix of a schedule is itself a schedule, so the enumeration is a tree
walk that shares prefixes.  Output: per grammar, per policy, the number of
schedules whose worst leaf is in each class, the fraction of leaves per class,
pairwise differences, and the shortest example schedule per class.

Usage: python3 sched.py --grammar G1 [--k 4] [--policies A,B,C,CB] [--data nodatasum]
"""

import argparse
import sys
import time
from collections import Counter, defaultdict

import b_model as bm

CLASSES = ("SILENT", "GONE", "UNREACH", "STUCK", "RO", "REFUSED", "OK")
RANK = {c: n for n, c in enumerate(CLASSES)}


def make_args(policy, raid, data, meta=False, muts=(), stripes=2, cap=0, bad="persistent"):
    a = argparse.Namespace()
    a.policy = policy
    a.mut = list(muts)
    a.raid = raid
    a.stripes = stripes
    a.cap = cap
    a.data = data
    a.meta = meta
    a.bad = bad
    a.flush = False
    a.unnamed_flush = False
    a.maxfault = 9
    a.detach = 9
    a.ret = True
    a.overfault = True      # a disk may die whatever the policy: beyond the tolerance the
                            # filesystem mounts ro,degraded (b_model mount())
    a.crash = 9
    a.remount = 9
    a.replay = False
    a.crash_in_replay = False
    a.replace = True
    a.no_spare = False
    a.scrub = True
    a.repair = True
    a.admin_depth = 3
    a.hist = False
    a.fix_cflush = False
    a.bad_devs = None
    a.detach_devs = None
    return a


def mount_kind(seg):
    if seg.startswith("mount without "):
        return "without " + seg.split()[2]
    if seg.startswith("mount with "):
        return "with " + seg[len("mount with "):].split(" back")[0] + " back"
    return "plain"


class Runner:
    def __init__(self, policy, raid, data, meta=False, muts=(), cap=0, stripes=2, bad="persistent"):
        self.policy = policy
        self.a = make_args(policy.split("+")[0], raid, data, meta, muts, stripes, cap, bad)
        self.m = bm.Model(self.a)
        self.memo_step = {}
        self.memo_leaf = {}

    def cidx(self, name):
        return self.m.cname.index(name)

    # ---- md-style trigger for C/CB: fail the device at its first write error
    def md_trigger(self, t):
        m = self.m
        if m.P not in ("C", "CB", "CTM"):
            return t
        w = bm.W.of(t)
        changed = False
        for c in range(m.N):
            d = w.devs[c]
            if d[bm.PR] and (d[bm.BAD] or d[bm.FB]) and not d[bm.FL] and m.absent_budget_ok(w, 1):
                if any(r is not None and c in r[1] for r in w.rec):
                    m.fail_device(w, c)
                    changed = True
        return w.tup() if changed else t

    def step(self, t, op):
        """-> list of (state, flags) ; flags: frozenset of path events"""
        key = (t, op)
        r = self.memo_step.get(key)
        if r is not None:
            return r
        m = self.m
        w = bm.W.of(t)
        kind = op[0]
        res = []
        if kind == "bad":
            c = self.cidx(op[1])
            if w.devs[c][bm.PR] and not w.devs[c][bm.FL]:
                w.devs[c][bm.BAD] = 1
            res.append((w.tup(), frozenset()))
        elif kind == "heal":
            c = self.cidx(op[1])
            w.devs[c][bm.BAD] = 0
            w.devs[c][bm.FB] = 0
            res.append((w.tup(), frozenset()))
        elif kind == "detach":
            c = self.cidx(op[1])
            if w.devs[c][bm.PR] and w.mode != bm.DOWN:
                w.devs[c][bm.PR] = 0
                w.devs[c][bm.GONE] = 0
                ev = set()
                if m.P == "CTM" and not w.devs[c][bm.FL] and m.absent_budget_ok(bm.W.of(t), 1):
                    m.fail_device(w, c)
                if m.nmissing(w) + m.nfailed(w) > m.npar and w.mode == bm.RW:
                    w.mode = bm.RO
                    ev.add("RO")
                res.append((w.tup(), frozenset(ev)))
            else:
                res.append((t, frozenset()))
        elif kind in ("W", "WC", "R"):
            if w.mode != bm.RW:
                res.append((t, frozenset(["REFUSED"])))
            else:
                s = op[1]
                i = op[2] if kind != "R" else None
                if kind == "R":
                    torn, names, verdict = m.recinfo(w, s)
                    if not (any(m.writable(w, c) for c in names) and not torn and not verdict):
                        res.append((t, frozenset()))
                        self.memo_step[key] = res
                        return res
                outs = m.rmw(w, s, i, "repair" if kind == "R" else "write")
                for lab, wn, ev in outs:
                    crashed = "CRASH" in lab
                    if kind == "WC":
                        if not crashed:
                            continue
                        ml = lab.split(" CRASH")[-1]
                        seg = ml.split(" ; ")[1] if " ; " in ml else ml
                        if mount_kind(seg) != op[3]:
                            continue
                    elif crashed:
                        continue
                    fl = set()
                    if "REFUSED_WRITE" in ev:
                        fl.add("REFUSED")
                    if "RO" in ev:
                        fl.add("RO")
                    if "DROPPED" in ev:
                        fl.add("DROPPED")
                    if "phaseA" in lab and "degrade" in lab:
                        fl.add("DEGRADE")
                    tn = wn.tup() if isinstance(wn, bm.W) else wn
                    res.append((tn, frozenset(fl)))
                if kind == "WC" and not res:
                    # the write was refused before any crash point (before
                    # its mark): the machine still crashes, after the refusal,
                    # and the mount sees the device set the schedule asks for
                    for lab, wn, ev in outs:
                        if "CRASH" in lab:
                            continue
                        wc = (wn if isinstance(wn, bm.W) else bm.W.of(wn)).copy()
                        wc.cnt[0] += 1
                        for ml, wm, ev2 in m.mount_variants(wc, crashed=True):
                            if mount_kind(ml) != op[3]:
                                continue
                            fl = {"REFUSED"} if "REFUSED_WRITE" in ev else set()
                            if "RO" in ev or "RO" in ev2 or wm.mode != bm.RW:
                                fl.add("RO")
                            res.append((wm.tup(), frozenset(fl)))
                    if not res:
                        raise AssertionError("no leaf for %s under %s" % (op, self.policy))
        elif kind == "remount":
            spec = op[1]
            wr = w.copy()
            for lab, wm, ev in m.mount_variants(wr, crashed=False):
                if mount_kind(lab) != spec:
                    continue
                fl = set()
                if "RO" in ev or wm.mode != bm.RW:
                    fl.add("RO")
                res.append((wm.tup(), frozenset(fl)))
        elif kind == "scrub":
            if w.mode == bm.RW:
                res.append((m.scrub(w).tup(), frozenset()))
            else:
                res.append((t, frozenset()))
        elif kind == "replace":
            c = self.cidx(op[1])
            wr = m.replace(w, c) if w.mode == bm.RW else None
            res.append(((wr.tup() if wr is not None else t), frozenset()))
        else:
            raise ValueError(op)
        res = [(self.md_trigger(tn), fl) for tn, fl in res]
        self.memo_step[key] = res
        return res

    def leaf(self, t):
        r = self.memo_leaf.get(t)
        if r is not None:
            return r
        m = self.m
        w = bm.W.of(t)
        silent, needless, lost = m.read_report(w)
        dok, fok = m.admin_eval(t, self.a.admin_depth)
        if silent:
            c = "SILENT"
        elif not dok:
            c = "GONE" if lost else "UNREACH"
        elif not fok:
            c = "STUCK"
        else:
            c = None
        r = (c, w.mode != bm.RW)
        self.memo_leaf[t] = r
        return r

    def classify(self, t, fl):
        c, ro = self.leaf(t)
        if c:
            return c
        if ro or "RO" in fl:
            return "RO"
        if "REFUSED" in fl:
            return "REFUSED"
        return "OK"


def fmt_op(op):
    k = op[0]
    if k == "W":
        return "write s%d.d%d" % (op[1], op[2])
    if k == "WC":
        return "write s%d.d%d+CRASH%s" % (op[1], op[2], "" if op[3] == "plain" else "(mount " + op[3] + ")")
    if k == "R":
        return "repair s%d" % op[1]
    if k == "remount":
        return "remount" + ("" if op[1] == "plain" else " " + op[1])
    return " ".join(str(x) for x in op)


def grammar(name, raid):
    """-> (prefixes, alphabet, K, limits(op_seq, op) -> bool)"""
    Wops = [("W", s, i) for s in range(2) for i in range(2)]
    Rops = [("R", s) for s in range(2)]
    if name == "G1":
        # RAID5, one failing disk X, at most one crash (mount with every disk)
        pre = [[("bad", "d0")], [("bad", "P")]]
        alpha = Wops + Rops + [("WC", s, i, "plain") for s in range(2) for i in range(2)]

        def ok(seq, op):
            return not (op[0] == "WC" and any(o[0] == "WC" for o in seq))
        return pre, alpha, ok
    if name == "G2":
        # RAID5: crash plus a disk lost at the same moment (the failing disk X
        # itself, or another one: a double fault)
        pre = [[("bad", "d0")], [("bad", "P")]]
        alpha = Wops + Rops + [("WC", s, i, m) for s in range(2) for i in range(2)
                               for m in ("without d0", "without d1", "without P")]

        def ok(seq, op):
            return not (op[0] == "WC" and any(o[0] == "WC" for o in seq))
        return pre, alpha, ok
    if name in ("G2x", "G2o"):
        # G2 split: the disk lost at the crash is the failing disk X itself
        # (G2x: the classic degraded write hole, one disk involved) or another
        # disk (G2o: a double fault on RAID5)
        pre = [[("bad", "d0")], [("bad", "P")]]
        alpha = Wops + Rops + [("WC", s, i, m) for s in range(2) for i in range(2)
                               for m in ("without d0", "without d1", "without P")]

        def ok(seq, op):
            if op[0] == "WC" and any(o[0] == "WC" for o in seq):
                return False
            if op[0] == "WC":
                x = seq[0][1]
                same = op[3] == "without " + x
                return same if name == "G2x" else not same
            return True
        return pre, alpha, ok
    if name == "G3":
        # RAID6: failing disk X, a second device Y lost (at runtime, or at the
        # mount after the crash), at most one crash, at most one loss
        pre = [[("bad", "d0")], [("bad", "P")]]
        others = ["d0", "d1", "P", "Q"]
        alpha = Wops + [("WC", s, i, m) for s in range(2) for i in range(2)
                        for m in ["plain"] + ["without " + y for y in others]] + \
            [("detach", y) for y in others]

        def ok(seq, op):
            if op[0] == "WC" and any(o[0] == "WC" for o in seq):
                return False
            lost = sum(1 for o in seq if o[0] == "detach" or (o[0] == "WC" and o[3] != "plain"))
            if (op[0] == "detach" or (op[0] == "WC" and op[3] != "plain")) and lost >= 1:
                return False
            return True
        return pre, alpha, ok
    if name == "G4":
        # RAID5: the failing disk X goes missing and returns (healed or not),
        # with writes before, between and after, at most one crash
        pre = [[("bad", "d0")]]
        alpha = [("W", 0, 0), ("W", 0, 1), ("W", 1, 0), ("WC", 0, 1, "plain"), ("WC", 0, 0, "plain"),
                 ("detach", "d0"), ("remount", "with {d0} back"), ("heal", "d0"), ("scrub",), ("replace", "d0")]

        def ok(seq, op):
            if op[0] == "WC" and any(o[0] == "WC" for o in seq):
                return False
            if op[0] in ("detach", "remount", "heal", "replace") and any(o == op for o in seq):
                return False
            if op[0] == "remount" and ("detach", "d0") not in seq:
                return False
            return True
        return pre, alpha, ok
    if name == "G5":
        # RAID6: two failing disks (X, then Y) and at most one crash
        pre = [[("bad", "d0"), ("bad", "d1")], [("bad", "d0"), ("bad", "P")], [("bad", "P"), ("bad", "Q")]]
        alpha = Wops + Rops + [("WC", s, i, m) for s in range(2) for i in range(2) for m in ("plain",)]

        def ok(seq, op):
            return not (op[0] == "WC" and any(o[0] == "WC" for o in seq))
        return pre, alpha, ok
    raise ValueError(name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--grammar", required=True)
    ap.add_argument("--k", type=int, default=4)
    ap.add_argument("--policies", default="A,B,C,CB")
    ap.add_argument("--data", default="nodatasum")
    ap.add_argument("--meta", action="store_true")
    ap.add_argument("--cap", type=int, default=0)
    ap.add_argument("--bad", default="persistent")
    ap.add_argument("--examples", type=int, default=3)
    ap.add_argument("--time", type=float, default=1700)
    ap.add_argument("--diff", default="", help="P,Q,CLASS: print example schedules where P's worst leaf is "
                    "CLASS and Q's is less severe")
    a = ap.parse_args()
    bm.W.SYMM = False       # stripe indices in a schedule are absolute
    raid = 6 if a.grammar in ("G3", "G5") else 5
    pols = a.policies.split(",")
    runners = {}
    for p in pols:
        base, *muts = p.split("+")
        mm = {"NI": "narrow_inflight", "SD": "strict_degraded", "CE": "cur_evict", "EN": "evict_naming",
              "BNR": "b_no_record", "RTT": "read_trust_torn", "NS": "no_suspect"}
        runners[p] = Runner(p, raid, a.data, a.meta, [mm[x] for x in muts], a.cap, bad=a.bad)
    pre, alpha, ok = grammar(a.grammar, raid)
    worst = {p: Counter() for p in pols}
    leafc = {p: Counter() for p in pols}
    crash_sched = {p: Counter() for p in pols}
    crash_leaf = {p: Counter() for p in pols}
    examples = defaultdict(list)
    pair = Counter()
    diffspec = a.diff.split(",") if a.diff else None
    diffc = [0]
    diffex = []
    pairg = Counter()
    paira = Counter()
    nsched = 0
    t0 = time.time()
    trunc = False

    def visit(seq, sets):
        nonlocal nsched, trunc
        if time.time() - t0 > a.time:
            trunc = True
            return
        nsched += 1
        wc = {}
        hascrash = any(o[0] == "WC" for o in seq)
        for p in pols:
            cl = Counter(runners[p].classify(t, fl) for t, fl in sets[p])
            if not cl:
                cl = Counter(["OK"])
            wst = min(cl, key=lambda c: RANK[c])
            wc[p] = wst
            worst[p][wst] += 1
            leafc[p].update(cl)
            if hascrash:
                crash_sched[p][wst] += 1
                crash_leaf[p].update(cl)
            key = (p, wst)
            ex = examples[key]
            if wst != "OK" and (len(ex) < 40 or len(seq) < max(len(e[0]) for e in ex)):
                # a leaf in that class; keep the shortest schedules
                for t, fl in sets[p]:
                    if runners[p].classify(t, fl) == wst:
                        ex.append((list(seq), t))
                        if len(ex) > 40:
                            ex.sort(key=lambda e: len(e[0]))
                            ex.pop()
                        break
        if diffspec:
            dp, dq, dc = diffspec[:3]
            need = diffspec[3] if len(diffspec) > 3 else ""
            if wc[dp] == dc and RANK[wc[dq]] > RANK[dc] and \
                    (not need or need in " ; ".join(fmt_op(o) for o in seq)):
                diffc[0] += 1
                if len(diffex) < 40 or len(seq) < max(len(e[0]) for e in diffex):
                    for t, fl in sets[dp]:
                        if runners[dp].classify(t, fl) == dc:
                            diffex.append((list(seq), t, wc[dq]))
                            diffex.sort(key=lambda e: len(e[0]))
                            del diffex[40:]
                            break
        for p in pols:
            for q in pols:
                if p != q and RANK[wc[p]] <= RANK["UNREACH"] and RANK[wc[p]] < RANK[wc[q]]:
                    pair[(p, q)] += 1
                if p != q and wc[p] == "GONE" and RANK[wc[q]] > RANK["GONE"]:
                    pairg[(p, q)] += 1
                if p != q and wc[p] in ("RO", "REFUSED", "STUCK") and wc[q] == "OK":
                    paira[(p, q)] += 1
        if len(seq) >= a.k + len(seq_prefix_len[0]):
            return
        for op in alpha:
            if not ok(seq, op):
                continue
            nsets = {}
            for p in pols:
                ns = set()
                for t, fl in sets[p]:
                    for t2, fl2 in runners[p].step(t, op):
                        ns.add((t2, fl | fl2))
                nsets[p] = ns
            visit(seq + [op], nsets)

    seq_prefix_len = [None, 0]
    for pfx in pre:
        seq_prefix_len[0] = pfx
        seq_prefix_len[1] = len(pfx)
        sets = {}
        for p in pols:
            r = runners[p]
            cur = {(r.m.init(), frozenset())}
            for op in pfx:
                ns = set()
                for t, fl in cur:
                    for t2, fl2 in r.step(t, op):
                        ns.add((t2, fl | fl2))
                cur = ns
            sets[p] = cur
        visit(list(pfx), sets)

    print("SCHED grammar=%s k=%d data=%s%s policies=%s schedules=%d time=%.0fs%s" % (
        a.grammar, a.k, a.data, " meta" if a.meta else "", a.policies, nsched, time.time() - t0,
        " TRUNCATED" if trunc else ""))
    print()
    print("Schedules by worst leaf (all schedules):")
    print("| policy | " + " | ".join(CLASSES) + " |")
    print("|---|" + "---|" * len(CLASSES))
    for p in pols:
        print("| %s | %s |" % (p, " | ".join(str(worst[p][c]) for c in CLASSES)))
    print()
    print("Schedules with a crash, by worst leaf:")
    print("| policy | " + " | ".join(CLASSES) + " |")
    print("|---|" + "---|" * len(CLASSES))
    for p in pols:
        print("| %s | %s |" % (p, " | ".join(str(crash_sched[p][c]) for c in CLASSES)))
    print()
    print("Leaves (crash points x landed subsets x eviction choices) in crash schedules, by class:")
    print("| policy | " + " | ".join(CLASSES) + " | loss fraction |")
    print("|---|" + "---|" * len(CLASSES) + "---|")
    for p in pols:
        tot = sum(crash_leaf[p].values()) or 1
        loss = crash_leaf[p]["SILENT"] + crash_leaf[p]["GONE"] + crash_leaf[p]["UNREACH"]
        print("| %s | %s | %.3f |" % (p, " | ".join(str(crash_leaf[p][c]) for c in CLASSES), loss / tot))
    print()
    for title, pr in (("Pairwise: schedules where the row policy's worst leaf is a data loss (SILENT/GONE/UNREACH) "
                       "strictly more severe than the column policy's worst leaf:", pair),
                      ("Pairwise: schedules where the row policy loses the value from every disk (GONE or worse) "
                       "and the column policy does not:", pairg),
                      ("Pairwise (availability): schedules where the row policy refuses/goes RO/wedges and the "
                       "column policy is OK:", paira)):
        print(title)
        print("| row \\ column | " + " | ".join(pols) + " |")
        print("|---|" + "---|" * len(pols))
        for p in pols:
            print("| %s | %s |" % (p, " | ".join("-" if p == q else str(pr[(p, q)]) for q in pols)))
        print()
    print()
    if diffspec:
        print("DIFF %s is %s where %s is less severe: %d schedules" % (diffspec[0], diffspec[2], diffspec[1], diffc[0]))
        for seq, t, qc in diffex[:a.examples]:
            print("DIFFEX (%s: %s): %s" % (diffspec[1], qc, " ; ".join(fmt_op(o) for o in seq)))
            print("    leaf: %s" % bm.describe(runners[diffspec[0]].m, t))
        print()
    for p in pols:
        for c in CLASSES[:-1]:
            ex = examples.get((p, c))
            if not ex:
                continue
            ex = sorted(ex, key=lambda e: len(e[0]))
            for seq, t in ex[:a.examples]:
                print("EXAMPLE %s %s: %s" % (p, c, " ; ".join(fmt_op(o) for o in seq)))
                print("    leaf: %s" % bm.describe(runners[p].m, t))
    return 0


if __name__ == "__main__":
    sys.exit(main())
