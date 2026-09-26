#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
sched.py -- availability of A and C under the SAME fault schedules, using
the transition functions of c_model.py (deterministic runs, no search).

A schedule is a device fault plan plus a workload (a round-robin of writes
over every cell of --stripes full stripes, with the kernel's queued repair
retried every few writes).  For each policy variant it reports:
  writes     writes attempted / refused (EIO) / read-only transitions
  crashpts   every crash point inside every write of the run (after the
             mark, after phase A, after each subset of phase B, in the
             commit), each followed by the mount: how many of them leave
             a SILENT read, how many LOST acknowledged data (no admin
             sequence of <= 3 actions reads it back), how many leave a
             needless EIO
  end        the state after the workload: redundancy (degraded or not),
             reads that fail
The C variants differ in their trigger: tfua (the first failed log-block
FUA, spec 1.2), tlog (the log full of the device's names), admin (after N
writes).  Every variant runs the same workload on the same schedule.
"""

import argparse
import sys

import c_model as cm


def ns(**kw):
    d = dict(policy="A", mut=None, raid=5, stripes=4, cap=0, data="nodatasum", meta=False, bad="persistent",
             flush=False, unnamed_flush=False, maxfault=1, detach=0, ret=False, overfault=False, crash=1,
             remount=0, replay=False, replace=True, no_spare=False, scrub=False, repair=True, depth=0,
             admin_depth=3, no_admin=False, time=0, max_rss_mb=0, name="", traces=False, classes="",
             maxclasses=0, fresh_values=False, nosymm=True, sb=False, sb_txn_only=True, pending=False,
             tfua=False, f1="none", e1all=False, nosrc=False, recov_crash=False, admin_fail_any=False,
             residue_smart=False, ghost=False, f1_missing=False, gen_skip=False, mount_commit=False)
    d.update(kw)
    return argparse.Namespace(**d)


def pick_ack(outs):
    """the branch without a crash: the last one (the model appends it last)"""
    for lab, w, ev in reversed(outs):
        if "CRASH" not in lab:
            return lab, w, ev
    return outs[-1]


def run(variant, sched, a, verbose=False):
    m = cm.Model(a)
    cm.W.SYMM = False
    w = cm.W.of(m.init())
    st = dict(writes=0, refused=0, ro=0, crashpts=0, silent=0, lost=0, lost_gone=0, needless=0, trig=None,
              repairs_failed=0)
    cells = [(s, i) for s in range(a.stripes) for i in range(m.nd)]
    step = 0
    log = []
    for ev in sched["events"]:
        kind = ev[0]
        if kind == "bad":
            w.devs[ev[1]][cm.BAD] = 1
            log.append("t%d %s starts failing writes" % (step, m.cn(ev[1])))
        elif kind == "flushbad":
            w.devs[ev[1]][cm.FB] = 1
        elif kind == "heal":
            w.devs[ev[1]][cm.BAD] = 0
            w.devs[ev[1]][cm.FB] = 0
            log.append("t%d %s heals" % (step, m.cn(ev[1])))
        elif kind == "detach":
            w.devs[ev[1]][cm.PR] = 0
            w.devs[ev[1]][cm.GONE] = 1
            if m.nmissing(w) + m.nfailed(w) > m.npar:
                w.mode = cm.RO
                st["ro"] += 1
            log.append("t%d %s lost" % (step, m.cn(ev[1])))
        elif kind == "replace":
            c = ev[1]
            if w.mode == cm.RW:
                w2 = m.replace(w, c)
                if w2 is not None:
                    w = w2
                    log.append("t%d replace %s" % (step, m.cn(c)))
                else:
                    log.append("t%d replace %s FAILED" % (step, m.cn(c)))
        elif kind == "work":
            n, admin_at = ev[1], ev[2] if len(ev) > 2 else None
            for k in range(n):
                step += 1
                if w.mode != cm.RW:
                    st["writes"] += 1
                    st["refused"] += 1
                    continue
                if variant.get("admin") is not None and step == variant["admin"] and m.cfam:
                    for c in range(m.N):
                        dv = w.devs[c]
                        if dv[cm.PR] and dv[cm.BAD] and not dv[cm.FL] and m.absent_budget_ok(w, 1):
                            m.fail_device(w, c)
                            if m.pending:
                                w = m.clean_commit(w)
                            st["trig"] = st["trig"] or step
                            log.append("t%d admin fails %s" % (step, m.cn(c)))
                s, i = cells[(step - 1) % len(cells)]
                # every crash point of this write, each mounted
                a.crash = 1
                w.cnt[0] = 0
                outs = m.rmw(w, s, i, "write")
                for lab, wc, evs in outs:
                    if "CRASH" not in lab:
                        continue
                    st["crashpts"] += 1
                    silent, needless, lost = m.read_report(wc)
                    if silent:
                        st["silent"] += 1
                    if needless:
                        st["needless"] += 1
                    dok, fok = m.admin_eval(wc.tup(), a.admin_depth)
                    if not dok:
                        st["lost"] += 1
                        if lost:
                            st["lost_gone"] += 1
                lab, w2, evs = pick_ack(outs)
                st["writes"] += 1
                if "REFUSED_WRITE" in evs:
                    st["refused"] += 1
                if "RO" in evs:
                    st["ro"] += 1
                if "FAILDEV" in evs or " T-" in lab:
                    if st["trig"] is None and any(d[cm.FL] for d in w2.devs):
                        st["trig"] = step
                w = w2
                w.cnt[0] = 0
                if verbose:
                    log.append("t%d %s" % (step, lab))
                # the kernel's queued repair of recorded stripes, retried
                if a.repair and step % sched.get("repair_every", 3) == 0 and w.mode == cm.RW:
                    for s2 in range(a.stripes):
                        torn, names, verdict = m.recinfo(w, s2)
                        if any(m.writable(w, c) for c in names) and not torn and not verdict:
                            outs = m.rmw(w, s2, None, "probe")
                            lab, w3, evs = pick_ack(outs)
                            if "REPAIR_FAILED" in evs:
                                st["repairs_failed"] += 1
                            else:
                                w = w3
                            w.cnt[0] = 0
                    # T-repair: the repair gave up (about 6 attempts)
                    if m.cfam and variant.get("trepair") and st["repairs_failed"] >= variant["trepair"]:
                        for c in range(m.N):
                            dv = w.devs[c]
                            if dv[cm.PR] and dv[cm.BAD] and not dv[cm.FL] and m.absent_budget_ok(w, 1):
                                m.fail_device(w, c)
                                if m.pending:
                                    w = m.clean_commit(w)
                                st["trig"] = st["trig"] or step
                                log.append("t%d T-repair fails %s" % (step, m.cn(c)))
    silent, needless, lost = m.read_report(w)
    st["end_eio"] = sum(1 for s in range(a.stripes) for i in range(m.nd)
                        if w.acc[s][i] is not None and m.pread(w, s, i) == cm.EIO)
    st["end_silent"] = len(silent)
    st["end_degraded"] = int(any(d[cm.FL] or not d[cm.PR] for d in w.devs))
    st["end_mode"] = cm.MODEN[w.mode]
    return st, log


SCHEDS = {
    # d0's writes fail from t=0 for good; 24 writes; replace at the end
    "persist_d0": dict(events=[("bad", 0), ("work", 24)]),
    "persist_d0_replace": dict(events=[("bad", 0), ("work", 12), ("replace", 0), ("work", 12)]),
    # parity device failing
    "persist_P": dict(events=[("bad", 2), ("work", 24)]),
    # a 6-write glitch on d0, then healthy
    "transient_d0": dict(events=[("bad", 0), ("work", 6), ("heal", 0), ("work", 18)]),
    # d0 failing, then d1 lost at t=12 (second failure)
    "persist_d0_then_d1_lost": dict(events=[("bad", 0), ("work", 12), ("detach", 1), ("work", 12)]),
    # RAID6: a second disk starts failing its writes while the first is failed
    "persist_d0_then_d1_bad": dict(events=[("bad", 0), ("work", 12), ("bad", 1), ("work", 12)]),
}

VARIANTS = [
    ("A", dict(policy="A"), {}),
    ("A+NI", dict(policy="A", mut=["narrow_inflight"]), {}),
    ("C T-log", dict(policy="C"), {}),
    ("C T-repair(2)", dict(policy="C"), dict(trepair=2)),
    ("C admin@6", dict(policy="C"), dict(admin=6)),
    ("C T-fua", dict(policy="C", sb=True, pending=True, tfua=True, f1="full"), {}),
    ("C T-fua+SD+NI", dict(policy="C", sb=True, pending=True, tfua=True, f1="full",
                           mut=["strict_degraded", "sd_csum", "narrow_inflight"]), {}),
    ("C T-fua+nosrc", dict(policy="C", sb=True, pending=True, tfua=True, f1="full", nosrc=True), {}),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stripes", type=int, default=4)
    ap.add_argument("--cap", type=int, default=2)
    ap.add_argument("--data", default="nodatasum")
    ap.add_argument("--raid", type=int, default=5)
    ap.add_argument("--sched", action="append")
    ap.add_argument("--maxfault", type=int, default=1)
    ap.add_argument("-v", action="store_true")
    o = ap.parse_args()
    names = o.sched or list(SCHEDS)
    print("stripes=%d cap=%d data=%s raid=%d; writes round-robin over every cell, queued repair every 3 writes"
          % (o.stripes, o.cap, o.data, o.raid))
    hdr = "%-26s %-16s %6s %7s %4s %5s | %6s %6s %6s %6s %8s | %4s %4s %3s %s" % (
        "schedule", "variant", "writes", "refused", "RO", "trig", "crashp", "SILENT", "LOST", "gone",
        "needEIO", "eio", "sil", "deg", "mode")
    print(hdr)
    for sn in names:
        sc = SCHEDS[sn]
        for vn, kw, var in VARIANTS:
            a = ns(stripes=o.stripes, cap=o.cap, data=o.data, raid=o.raid, maxfault=o.maxfault, **kw)
            st, log = run(var, sc, a, o.v)
            print("%-26s %-16s %6d %7d %4d %5s | %6d %6d %6d %6d %8d | %4d %4d %3d %s" % (
                sn, vn, st["writes"], st["refused"], st["ro"], st["trig"] if st["trig"] else "-",
                st["crashpts"], st["silent"], st["lost"], st["lost_gone"], st["needless"],
                st["end_eio"], st["end_silent"], st["end_degraded"], st["end_mode"]))
            if o.v:
                for l in log:
                    print("      ", l)
            sys.stdout.flush()


if __name__ == "__main__":
    main()
