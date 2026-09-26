#!/usr/bin/env python3
"""
tp_model.py -- where must A2's taint live so that no mount can lose both the
dropped record and the taint?  An abstract persistence model with per-device
copies of the write-intent log and of the superblock.

Setting (RAID5 on 3 devices by default, --raid6 for 4): device X (d0) is
missing.  A record R naming X's column exists and is durable in the log
copies of the present devices (the kernel's durability rule).  A2 drops R
(log full) and taints X.  Afterwards any mix of: transient write errors on
the present devices, log block writes (each later mark writes a new block
built from the in-memory set, which no longer holds R), superblock writes
(transaction commits), crashes, and mounts with any device set the
filesystem accepts (X back or not, another device missing).

Kernel rules modelled (raid56-wib.c at stage0-wip 2a1e2379e4):
  log write   a block goes to every present device (FUA); it counts as
              written iff nr - nr_errors >= 1 and (nr - nr_errors >= t + 1 or
              nr_errors + missing <= t) (wib_write_all_devices(), 1902-1956);
              otherwise the mark fails, but the in-memory set that dropped R
              stays the base of the next block (wib_write_block_locked(),
              "the next union is built on it")
  log load    per device only its newest valid block; stale marks come only
              from the devices holding the newest sequence among those
              present (btrfs_wib_load(), 6924-6945)
  superblock  write_all_supers() succeeds if at least one device took it
              (max_errors = num_devices - 1), the mount uses the present
              device with the highest generation (latest_dev)

Placements of the taint (--place):
  log-header   in the header of every log block written while X is tainted:
               atomic with the block that omits R
  sb-lazy      in the superblock; the drop reaches the log at the next mark,
               the taint the superblock at the next commit
  sb-first     in the superblock, written (write_all_supers) before the block
               that omits R; the drop waits for it (>= 1 copy, upstream rule)
  sb-first-all as sb-first, but durable only when EVERY present superblock
               took it (the stage-1 design's "zero superblock errors" rule);
               otherwise R stays
A VIOLATION: a mount with X present at which neither R (with its stale mark)
nor the taint is known -- X's stale column would be trusted.
"""
import argparse
from collections import deque


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--place", choices=("log-header", "log-once", "sb-lazy", "sb-first", "sb-first-all"),
                    required=True, help="log-once: negative control, the taint only in the first block after the drop")
    ap.add_argument("--raid6", action="store_true")
    ap.add_argument("--depth", type=int, default=14)
    ap.add_argument("--faults", type=int, default=2, help="transient write-error episodes")
    a = ap.parse_args()
    N = 4 if a.raid6 else 3
    T = 2 if a.raid6 else 1
    X = 0
    # device tuple: (present, bad, log_seq, log_has_R, log_taint, sb_gen, sb_taint)
    # global: (mem_R, mem_taint, seq, gen, drop_state, faults_used, crashed)
    #   drop_state: 0 R not yet dropped; 1 dropped in memory (log not yet
    #   written); 2 dropped and a block without R was attempted
    # initial: X missing; R durable on every present device's newest block
    devs0 = []
    for c in range(N):
        if c == X:
            devs0.append((0, 0, 0, 0, 0, 1, 0))
        else:
            devs0.append((1, 0, 1, 1, 0, 1, 0))
    init = (tuple(devs0), (1, 0, 1, 1, 0, 0, 0))

    def present(devs):
        return [c for c in range(N) if devs[c][0]]

    def log_write(devs, g, has_R, taint):
        """write a new log block to every present device; returns (devs, ok)"""
        mem_R, mem_t, seq, gen, ds, fu, cr = g
        seq2 = seq + 1
        nr = 0
        err = 0
        nd = list(devs)
        for c in range(N):
            p, b, ls, lr, lt, sg, st = devs[c]
            if not p:
                continue
            nr += 1
            if b:
                err += 1
                continue
            nd[c] = (p, b, seq2, has_R, taint, sg, st)
        missing = N - nr
        ok = (nr - err >= 1) and (nr - err >= T + 1 or err + missing <= T)
        return tuple(nd), (mem_R, mem_t, seq2, gen, ds, fu, cr), ok

    def sb_write(devs, g, taint, need_all):
        mem_R, mem_t, seq, gen, ds, fu, cr = g
        gen2 = gen + 1
        nd = list(devs)
        okn = 0
        errs = 0
        for c in range(N):
            p, b, ls, lr, lt, sg, st = devs[c]
            if not p:
                continue
            if b:
                errs += 1
                continue
            nd[c] = (p, b, ls, lr, lt, gen2, taint)
            okn += 1
        ok = okn >= 1 and (errs == 0 or not need_all)
        return tuple(nd), (mem_R, mem_t, seq, gen2, ds, fu, cr), ok

    def mount_view(devs):
        """what a mount with this device set knows: (R known, taint known)"""
        pres = present(devs)
        newest = max(devs[c][2] for c in pres)
        r_known = any(devs[c][3] for c in pres if devs[c][2] == newest)
        if a.place in ("log-header", "log-once"):
            t_known = any(devs[c][4] for c in pres if devs[c][2] == newest)
        else:
            gmax = max(devs[c][5] for c in pres)
            t_known = any(devs[c][6] for c in pres if devs[c][5] == gmax)
        return r_known, t_known

    def succ(state):
        devs, g = state
        mem_R, mem_t, seq, gen, ds, fu, cr = g
        out = []
        pres = present(devs)
        # --- the drop (once)
        if ds == 0 and not cr:
            if a.place in ("log-header", "log-once", "sb-lazy"):
                g2 = (0, 1, seq, gen, 1, fu, cr)
                out.append(("drop R, taint X in memory", (devs, g2)))
            else:
                need_all = a.place == "sb-first-all"
                d2, g2, ok = sb_write(devs, (mem_R, 1, seq, gen, ds, fu, cr), 1, need_all)
                if ok:
                    g3 = (0, 1, g2[2], g2[3], 1, fu, cr)
                    out.append(("taint X in the superblocks (write_all_supers ok), then drop R in memory", (d2, g3)))
                else:
                    # not durable: R is kept; the taint may sit on some superblocks
                    g3 = (1, 0, g2[2], g2[3], 0, fu, cr)
                    out.append(("taint superblock write not durable -> R kept", (d2, g3)))
        # --- a mark writes a log block from the in-memory set
        if not cr and pres:
            has_R = mem_R
            taint = mem_t if a.place == "log-header" else 0
            if a.place == "log-once":
                taint = mem_t if ds == 1 else 0
            d2, g2, ok = log_write(devs, g, has_R, taint)
            if ds == 1 and not mem_R:
                g2 = g2[:4] + (2,) + g2[5:]
            out.append(("log block %d (%s)%s" % (g2[2], "with R" if has_R else "without R",
                                                 "" if ok else " -- not enough copies: that write fails"), (d2, g2)))
        # --- a transaction commit writes the superblocks
        if not cr and pres:
            taint = mem_t if a.place not in ("log-header", "log-once") else 0
            d2, g2, ok = sb_write(devs, g, taint, False)
            out.append(("commit: superblocks gen %d%s" % (g2[3], "" if ok else " (all failed)"), (d2, g2)))
        # --- transient write errors on present devices other than X
        if not cr:
            for c in range(N):
                p, b, ls, lr, lt, sg, st = devs[c]
                if c == X or not p:
                    continue
                if not b and fu < a.faults:
                    nd = list(devs)
                    nd[c] = (p, 1, ls, lr, lt, sg, st)
                    out.append(("d%d starts failing writes" % c, (tuple(nd), g[:5] + (fu + 1, cr))))
                if b:
                    nd = list(devs)
                    nd[c] = (p, 0, ls, lr, lt, sg, st)
                    out.append(("d%d heals" % c, (tuple(nd), g)))
        # --- crash: memory is lost
        if not cr:
            out.append(("CRASH", (devs, (mem_R, mem_t, seq, gen, ds, fu, 1))))
            # a clean unmount: a last commit (superblocks), then memory is gone
            taint = mem_t if a.place not in ("log-header", "log-once") else 0
            d2, g2, ok = sb_write(devs, g, taint, False)
            if True:
                d3, g3, ok3 = log_write(d2, g2, mem_R, mem_t if a.place == "log-header" else 0)
                out.append(("unmount (commit: log block %d, superblocks gen %d)" % (g3[2], g3[3]),
                            (d3, g3[:6] + (1,))))
        # --- mount (after a crash or an unmount): any tolerated device set
        if cr:
            base = devs
            for mask in range(1 << N):
                pres2 = [c for c in range(N) if mask >> c & 1]
                if len(pres2) < N - T:
                    continue
                nd = []
                for c in range(N):
                    p, b, ls, lr, lt, sg, st = base[c]
                    nd.append((1 if c in pres2 else 0, 0, ls, lr, lt, sg, st))
                nd = tuple(nd)
                r_known, t_known = mount_view(nd)
                lab = "mount with {%s}" % ",".join("d%d" % c for c in pres2)
                out.append((lab, ("MOUNT", nd, r_known, t_known, cr, ds)))
        return out

    parent = {init: None}
    q = deque([(init, 0)])
    viol = []
    nstates = 0
    while q:
        st, d = q.popleft()
        nstates += 1
        if d >= a.depth:
            continue
        for lab, s2 in succ(st):
            if s2[0] == "MOUNT":
                _, nd, r_known, t_known, cr, ds = s2
                if nd[X][0] and ds > 0 and not r_known and not t_known:
                    tr = [lab]
                    s = st
                    while parent[s] is not None:
                        s, l = parent[s]
                        tr.append(l)
                    viol.append(" || ".join(reversed(tr)))
                continue
            if s2 in parent:
                continue
            parent[s2] = (st, lab)
            q.append((s2, d + 1))
    viol.sort(key=len)
    print("RESULT place=%s raid=%d states=%d depth=%d VIOLATIONS=%d" % (a.place, 6 if a.raid6 else 5,
                                                                      nstates, a.depth, len(viol)))
    for v in viol[:3]:
        print("  VIOLATION: " + v)


if __name__ == "__main__":
    main()
