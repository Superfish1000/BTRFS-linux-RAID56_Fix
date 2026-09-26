#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
window.py -- does CUR's alert give an administrator a chance to prevent the
loss it announces, and does the alert's own advice do it?

Runs the cur_model.py search for one scenario, then for every state with a
silent wrong read finds the transition that dropped the record behind it (a
full-log eviction, or a flush loss acknowledged without a name), and measures:

  window   benign steps between the drop and the first wrong read on that
           path (0: the drop itself makes the read wrong)
  alert    whether a record_dropped/log_flush_unnamed alert is visible in the
           mount where the wrong read happens (the kernel's latch is in
           memory only: pass --alert-volatile to model that)

and, from the state right after the drop (what the administrator sees when the
alert fires), applies each response and explores a continuation to
--cont-depth: application writes, unmount/mount with any device that can come
back doing so, and --cont-detach (default 1) further device losses within the
parity tolerance -- the event a redundancy record exists for; no write or
flush faults, no crash:

  none          do nothing
  scrub         'run scrub' (wib_evict_sticky()'s warning)
  fix+scrub     'fix the failing device' (heal it / let the missing one come
                back) then scrub (EV_DROPPED's text, raid56-wib.c:6138)
  replace       replace every missing, failing or tainted member (and the
                outsider) -- no scrub
  replace+scrub the same, then scrub

A response is SAFE when no continuation (application writes, unmount/mount,
returns of missing devices, and --cont-detach more device losses within the
tolerance) reaches a silent wrong read and no
continuation loses the acknowledged value from the disks.

Usage: window.py [--cont-depth K] [--max-drops N] <cur_model.py args>
"""

import argparse
import sys
import time
from collections import Counter, defaultdict

import cur_model as cm


def drop_edge(label):
    return (" evict s" in label) or ("log cannot name it" in label) or ("log has no room" in label)


def kind_of(label):
    if "log cannot name it" in label and " evict s" not in label:
        return "unnamed-flush-acked"
    if "log has no room" in label and " evict s" not in label:
        return "unrecorded-flush-acked"
    k = label[label.index(" evict s"):]
    if "<" in k:
        return k[k.index("<") + 1:k.index(">")]
    return "?"


def cont_model(a, detach):
    b = argparse.Namespace(**vars(a))
    b.bad = "none"
    b.flush = False
    b.unnamed_flush = False
    b.lose_flush = False
    b.maxfault = 0
    b.detach = detach
    b.crash = 0
    b.ure = 0
    b.replay = False
    b.repair = False
    b.scrub = False
    b.replace = False
    b.ret = True
    b.remount = 2
    return cm.Model(b)


def reset_cnt(t):
    w = cm.W.of(t)
    w.cnt = [0] * len(w.cnt)
    return w


def continuation(mc, w0, depth):
    """BFS of benign events; returns (min depth of a silent read or None,
    min depth of a lost value or None)"""
    t0 = w0.tup()
    seen = {t0}
    frontier = [t0]
    silent_d = lost_d = None
    for d in range(depth + 1):
        nxt = []
        for t in frontier:
            w = cm.W.of(t)
            s, _, lost = mc.read_report(w)
            if s and silent_d is None:
                silent_d = d
            if lost and lost_d is None:
                lost_d = d
            if silent_d is not None and lost_d is not None:
                return silent_d, lost_d
            if d == depth:
                continue
            for lab, t2, ev in mc.successors(t):
                if lab.startswith("scrub") or lab.startswith("replace") or lab.startswith("trigger"):
                    continue        # the administrator's own actions are the response
                if t2 not in seen:
                    seen.add(t2)
                    nxt.append(t2)
        frontier = nxt
    return silent_d, lost_d


def responses(m, t):
    """[(name, [W...])] the states each response leads to"""
    w = cm.W.of(t)
    out = []
    out.append(("none", [w]))
    if w.mode == cm.RW:
        out.append(("scrub", [m.scrub(w)]))
    # fix: heal failing devices, let missing (not gone) ones come back
    wf = w.copy()
    for c in range(m.ND):
        d = wf.devs[c]
        if d[cm.PR] and (d[cm.BAD] or d[cm.FB]):
            d[cm.BAD] = 0
            d[cm.FB] = 0
    back = [c for c in range(m.ND) if not wf.devs[c][cm.PR] and not wf.devs[c][cm.GONE]]
    fixed = []
    if back:
        for c in back:
            wf.devs[c][cm.PR] = 1
        for lab, wm, ev in m.mount(wf):
            fixed.append(wm)
    else:
        fixed.append(wf)
    out.append(("fix+scrub", [m.scrub(x) if x.mode == cm.RW else x for x in fixed]))
    # replace every missing / failing / tainted member
    wr = w.copy()
    ok = wr.mode == cm.RW
    for c in range(m.N):
        d = wr.devs[c]
        if not ok:
            break
        if not d[cm.PR] or d[cm.BAD] or d[cm.FB] or d[cm.TN]:
            r = m.replace(wr, c)
            if r is None:
                ok = False
            else:
                wr = r
    if m.ND > m.N and not wr.devs[m.N][cm.PR]:
        wr.devs[m.N] = [1, 0, 0, 0, 0, 0]
    if ok:
        out.append(("replace", [wr]))
        out.append(("replace+scrub", [m.scrub(wr)]))
    else:
        out.append(("replace", []))
        out.append(("replace+scrub", []))
    return out


def main():
    wp = argparse.ArgumentParser(add_help=False)
    wp.add_argument("--cont-depth", type=int, default=3)
    wp.add_argument("--max-drops", type=int, default=400)
    wp.add_argument("--cont-detach", type=int, default=1,
                    help="devices that may still go missing (within the tolerance) in the continuation")
    wa, rest = wp.parse_known_args()
    a = cm.build_parser().parse_args(rest)
    cm.W.SYMM = not a.nosymm
    cm.FRESH = a.fresh_values
    m = cm.Model(a)
    cm.run.rss_limit_mb = a.max_rss_mb
    cm.run.why = ""
    t0 = time.time()
    parent, stats, edges, first, d, trunc, el = cm.run(m, a.depth, a.admin_depth, a.time, want_admin=False)
    print("SEARCH states=%d depth=%d%s time=%.1fs SILENT_WRONG=%d SILENT_NOALERT=%d" % (
        len(parent), d, " TRUNCATED" if trunc else "", el, stats["SILENT_WRONG"], stats["SILENT_NOALERT"]))
    print("CONFIG", " ".join(rest))

    windows = Counter()
    alert_vis = Counter()
    drops = {}
    ex = {}
    nsilent = 0
    for t in parent:
        w = cm.W.of(t)
        silent, _, _ = m.read_report(w)
        if not silent:
            continue
        nsilent += 1
        # the path back to the root
        path = []
        x = t
        while parent.get(x) is not None:
            px, lab = parent[x]
            path.append((px, lab, x))
            x = px
        path.reverse()
        k_s = None
        for k, (px, lab, cx) in enumerate(path):
            if m.read_report(cm.W.of(cx))[0]:
                k_s = k
                break
        idx = None
        for k in range(k_s + 1):
            if drop_edge(path[k][1]):
                idx = k
        if idx is None:
            windows[("no-drop", -1)] += 1
            continue
        first_silent = k_s
        kind = kind_of(path[idx][1])
        win = (first_silent - idx) if first_silent is not None else -1
        windows[(kind, win)] += 1
        alert_vis["alert visible" if w.alert & (cm.AL_DROP | cm.AL_UNFL) else "NO alert in this mount"] += 1
        td = path[idx][2]
        drops.setdefault(td, (kind, win))
        key = (kind, win)
        tr = [lab for _, lab, _ in path]
        if key not in ex or len(tr) < len(ex[key][0]):
            ex[key] = (tr, idx, t)
    print("SILENT states %d; by (drop kind, window steps):" % nsilent)
    for (k, win), n in sorted(windows.items(), key=lambda kv: (-kv[1])):
        print("  %-40s window=%-3d states=%d" % (k, win, n))
    print("alert visibility at the wrong read:", dict(alert_vis))
    for key, (tr, idx, t) in sorted(ex.items()):
        print("EXAMPLE %s window=%d: %s" % (key[0], key[1],
                                            " || ".join(tr[:idx]) + " ||>> " + tr[idx] + " <<|| " + " || ".join(tr[idx + 1:])))
        print("   state: %s" % cm.describe(m, t))

    mc = cont_model(a, wa.cont_detach)
    tally = defaultdict(Counter)
    ex_resp = {}
    # up to --max-drops drop states per drop kind, in a fixed order
    per = defaultdict(list)
    for td, kw in sorted(drops.items(), key=lambda kv: repr(kv[0])):
        per[kw[0]].append((td, kw))
    items = [x for k in sorted(per) for x in per[k][:wa.max_drops]]
    for td, (kind, win) in items:
        for name, states in responses(m, td):
            if not states:
                tally[(kind, name)]["replace failed"] += 1
                continue
            worst = "SAFE"
            for ws in states:
                w1 = reset_cnt(ws.tup())
                sd, ld = continuation(mc, w1, wa.cont_depth)
                if sd is not None and ld is not None:
                    r = "silent+gone"
                elif sd is not None:
                    r = "silent"
                elif ld is not None:
                    r = "gone(loud)"
                else:
                    r = "SAFE"
                if r != "SAFE":
                    worst = r
            tally[(kind, name)][worst] += 1
            ex_resp.setdefault((kind, name, worst), td)
    print("RESPONSES at the drop (%d distinct drop states%s, at most %d per kind), continuation depth %d "
          "(writes, remounts, returns, up to %d more device loss within the tolerance):" % (
              len(items), " of %d" % len(drops) if len(drops) > len(items) else "", wa.max_drops, wa.cont_depth,
              wa.cont_detach))
    for (kind, name), c in sorted(tally.items()):
        tot = sum(c.values())
        print("  %-34s %-14s %s" % (kind, name, "  ".join("%s=%d(%.0f%%)" % (k, v, 100.0 * v / tot)
                                                        for k, v in sorted(c.items()))))
    for (kind, name, worst), td in sorted(ex_resp.items()):
        if name in ("replace", "replace+scrub") and worst == "SAFE":
            continue
        print("RESP-EXAMPLE %s / %s -> %s: drop state %s" % (kind, name, worst, cm.describe(m, td)))
    print("TIME %.1fs" % (time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
