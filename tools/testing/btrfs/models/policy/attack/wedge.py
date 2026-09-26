#!/usr/bin/env python3
"""
wedge.py -- how many writes does each policy take on a degraded array
before it refuses one?  Deterministic schedules on a2_model.Model (the
exhaustive model's own write/mount code), 8 full stripes, each write going
to a new region (the worst case for a log of --cap records).

Scenarios:
  degraded        d0 missing; 24 writes to d1 then d0 columns, round robin
  degraded+crash  as above, but the first write crashes with only P landed;
                  the degraded mount's recovery keeps a verdict (not droppable)
  degraded+2crash two such crashes (two verdicts)
  raid6 miss+bad  RAID6: d0 missing and d1 failing every write; 24 writes,
                  d1's column first (records name the present d1), then d0's
  returned        d0 missing, 8 writes, then d0 back (remount), 16 writes
  scanned         d0 missing, 4 writes, d0 re-appears to a device scan
                  (no I/O until a mount: volumes.c:959-962 clears MISSING),
                  20 writes
Each cell: acknowledged/attempted, and the index of the first refused write.
"""
import re
import sys

_ARGV = sys.argv[1:]
sys.argv = sys.argv[:1]
import a2_model as M  # noqa: E402

NS = 8


def mk(policy, cap, raid=5, muts=(), extra=()):
    args = ["--policy", policy, "--stripes", str(NS), "--cap", str(cap), "--raid", str(raid),
            "--crash", "3", "--detach", "3", "--ret", "--remount", "3"] + list(extra)
    for mu in muts:
        args += ["--mut", mu]
    a = M.make_parser().parse_args(args)
    M.W.SYMM = False
    return M.Model(a)


def pick(outs, pat):
    for lab, w, ev in outs:
        if re.search(pat, lab):
            return lab, w, ev
    raise AssertionError("no outcome matching %r in %s" % (pat, [o[0] for o in outs][:12]))


class Sim:
    def __init__(self, m):
        self.m = m
        self.w = M.W.of(m.init())
        self.att = 0
        self.ack = 0
        self.first = None
        self.log = []

    def detach(self, c):
        self.w.devs[c][M.PR] = 0
        self.w.devs[c][M.GONE] = 0

    def bad(self, c):
        self.w.devs[c][M.BAD] = 1

    def scan(self, c):
        self.w.devs[c][M.SCN] = 1

    def remount(self, pat=r"^mount"):
        lab, w, ev = pick(self.m.mount_variants(self.w, crashed=False), pat)
        self.w = w
        self.log.append("remount: " + lab)

    def write(self, s, i):
        self.att += 1
        outs = self.m.rmw(self.w, s, i, "probe")
        lab, w, ev = outs[-1]
        if "REFUSED_WRITE" in ev:
            if self.first is None:
                self.first = self.att
            self.log.append("#%d %s" % (self.att, lab))
            if w.mode == M.RW:
                self.w = w
            return False
        self.ack += 1
        self.w = w
        return True

    def crash_write(self, s, i, landed="{P}"):
        self.att += 1
        outs = self.m.rmw(self.w, s, i, "write")
        if len(outs) == 1 and "REFUSED_WRITE" in outs[0][2]:
            if self.first is None:
                self.first = self.att
            self.log.append("#%d %s (so no crash)" % (self.att, outs[0][0]))
            return
        lab, w, ev = pick(outs, r"phaseB landed %s ; CRASH ; mount( T-missing fails d0)?$" % re.escape(landed))
        self.w = w
        self.log.append("#%d %s" % (self.att, lab))

    def res(self):
        return "%d/%d%s" % (self.ack, self.att, (" first@%d" % self.first) if self.first else "")


def rr(sim, n, avoid=(), first_col=0):
    """n writes, each to the next stripe not in avoid, column first_col for
    a full round of stripes, then the other column, and so on"""
    stripes = [s for s in range(NS) if s not in avoid]
    for k in range(n):
        s = stripes[k % len(stripes)]
        i = first_col if (k // len(stripes)) % 2 == 0 else 1 - first_col
        sim.write(s, i)


def scen_degraded(m):
    x = Sim(m)
    x.detach(0)
    rr(x, 24)
    return x


def scen_degraded_crash(m, ncrash=1):
    """the crashes leave verdicts on s0 (and s1); the writes avoid those
    stripes, so the verdicts stay"""
    x = Sim(m)
    x.detach(0)
    for k in range(ncrash):
        x.crash_write(k, 1)
    rr(x, 24, avoid=range(ncrash))
    return x


def scen_raid6(m):
    x = Sim(m)
    x.detach(0)
    x.bad(1)
    rr(x, 24, first_col=1)      # d1's column first: each write fails on d1 (named, present)
    return x


def scen_returned(m):
    """8 writes into d0's column while d0 is missing; d0 comes back; then 16
    writes into d1's column (each needs d0's content: named or tainted)"""
    x = Sim(m)
    x.detach(0)
    rr(x, 8, first_col=0)
    x.att_before = x.att
    x.remount(r"^mount with \{d0\} back$")
    rr(x, 16, first_col=1)
    return x


def scen_scanned(m):
    x = Sim(m)
    x.detach(0)
    rr(x, 2)
    x.scan(0)
    rr(x, 22)
    return x


POLS = [
    ("A", "A", ()),
    ("CUR", "CUR", ()),
    ("A2", "A2", ()),
    ("A2 taintfirst", "A2", ("--a2-persist", "taintfirst")),
    ("A2 +repair-onto-tainted", "A2", ("MUT:a2_phaseA_tainted",)),
    ("A2P (per-stripe, column clear)", "A2", ("--taint-region", "1", "--taint-clear", "column")),
    ("A2 keyed on MISSING bit", "A2", ("MUT:a2_missing_bit",)),
    ("CTM", "CTM", ()),
]


def build(pol, extra, cap, raid=5):
    muts = [e[4:] for e in extra if e.startswith("MUT:")]
    ex = [e for e in extra if not e.startswith("MUT:")]
    return mk(pol, cap, raid, muts, ex)


def main():
    scen = [
        ("degraded", 5, scen_degraded),
        ("degraded+crash", 5, lambda m: scen_degraded_crash(m, 1)),
        ("degraded+2crash", 5, lambda m: scen_degraded_crash(m, 2)),
        ("raid6 miss+bad", 6, scen_raid6),
        ("returned", 5, scen_returned),
        ("scanned", 5, scen_scanned),
    ]
    caps = [1, 2, 4]
    show = "-v" in _ARGV
    print("| scenario | cap | " + " | ".join(p[0] for p in POLS) + " |")
    print("|---|---|" + "---|" * len(POLS))
    for name, raid, fn in scen:
        for cap in caps:
            cells = []
            for pname, pol, extra in POLS:
                m = build(pol, extra, cap, raid)
                try:
                    x = fn(m)
                    cells.append(x.res())
                    if show:
                        print("   [%s cap %d %s] recs at end: %s" % (name, cap, pname,
                              [M.fmt_rec(r) for r in x.w.rec]), file=sys.stderr)
                    if show:
                        print("   [%s cap %d %s] %s" % (name, cap, pname, " / ".join(x.log[:6])), file=sys.stderr)
                except AssertionError as e:
                    cells.append("n/a")
                    if show:
                        print("   [%s cap %d %s] %s" % (name, cap, pname, e), file=sys.stderr)
            print("| %s | %d | %s |" % (name, cap, " | ".join(cells)))


if __name__ == "__main__":
    main()
