#!/usr/bin/env python3
"""Aggregate out/*.txt RESULT lines into markdown tables (summary.md)."""
import glob
import os
import re
import sys

os.chdir(os.path.dirname(os.path.abspath(__file__)))
COLS = ["states", "SILENT_WRONG", "SILENT_NOALERT", "LOST_ACKED_GONE", "LOST_ACKED_UNREACHABLE",
        "REFUSED_READ", "STUCK", "REFUSED_WRITE", "RO", "DROPPED"]
SHORT = {"states": "states", "SILENT_WRONG": "SILENT", "SILENT_NOALERT": "silent-no-alert",
         "LOST_ACKED_GONE": "LOST(gone)", "LOST_ACKED_UNREACHABLE": "LOST(unreach)",
         "REFUSED_READ": "REF_READ", "STUCK": "STUCK", "REFUSED_WRITE": "REF_WRITE",
         "RO": "RO/down", "DROPPED": "dropped"}
POL_ORDER = ["OLD", "A", "CUR", "A2", "B", "C", "CB", "CTM"]
PCT = {"SILENT_WRONG", "LOST_ACKED_GONE", "LOST_ACKED_UNREACHABLE", "STUCK"}


def parse(path):
    r = {}
    for line in open(path):
        if line.startswith("RESULT"):
            for k, v in re.findall(r"(\w+)=([\w.+-]+)", line):
                r[k] = v
            r["truncated"] = "TRUNCATED" in line
            m = re.search(r" depth=(\d+)", line)
            r["depth"] = m.group(1) if m else "?"
    return r


def main():
    rows = {}
    for p in sorted(glob.glob("out/*.txt")):
        name = os.path.basename(p)[:-4]
        r = parse(p)
        if not r:
            continue
        rows[name] = r
    fams = {}
    for name, r in rows.items():
        if not name.startswith("M_"):
            continue
        body = name[2:]
        fam, pol, data = body.rsplit("_", 2)
        fams.setdefault(fam, {})[(pol, data)] = r
    print("# policy_model.py matrix\n")
    print("Counts are distinct reachable states (SILENT .. STUCK; with the share of all states "
          "of that run) or distinct transitions (REF_WRITE, RO/down, dropped). 0 is shown as '.'. "
          "A depth ending in T means the run stopped early (time or memory) at that depth.\n")
    for fam in fams:
        print("## %s\n" % fam)
        print("| policy | data | depth | " + " | ".join(SHORT[c] for c in COLS) + " |")
        print("|---|---|---|" + "---|" * len(COLS))
        for pol in POL_ORDER:
            for data in ("nodatasum", "csum"):
                r = fams[fam].get((pol, data))
                if not r:
                    continue
                vals = []
                st = float(r.get("states", "1") or 1)
                for c in COLS:
                    v = r.get(c, "?")
                    if v == "0":
                        vals.append(".")
                    elif c in PCT and v != "?":
                        vals.append("%s (%.1f%%)" % (v, 100.0 * int(v) / st))
                    else:
                        vals.append(v)
                print("| %s | %s | %s%s | %s |" % (pol, data, r["depth"], "T" if r["truncated"] else "",
                                                  " | ".join(vals)))
        print()
    print("## variants (SD = strict_degraded, NI = narrow_inflight)\n")
    print("| run | depth | " + " | ".join(SHORT[c] for c in COLS) + " |")
    print("|---|---|" + "---|" * len(COLS))
    for name, r in sorted(rows.items()):
        if not name.startswith("V_"):
            continue
        vals = ["." if r.get(c, "?") == "0" else r.get(c, "?") for c in COLS]
        print("| %s | %s%s | %s |" % (name[2:], r["depth"], "T" if r["truncated"] else "", " | ".join(vals)))
    print()
    print("## negative controls\n")
    want = {}
    if os.path.exists("controls.want"):
        for line in open("controls.want"):
            k, v = line.split()
            want[k] = v
    print("| control | expects | observed | verdict |")
    print("|---|---|---|---|")
    for name, r in sorted(rows.items()):
        if not name.startswith("K_"):
            continue
        w = want.get(name, "?")
        got = r.get(w, "?")
        print("| %s | %s>0 | %s | %s |" % (name, w, got, "BITES" if got not in ("0", "?") else "DOES NOT BITE"))


if __name__ == "__main__":
    main()
