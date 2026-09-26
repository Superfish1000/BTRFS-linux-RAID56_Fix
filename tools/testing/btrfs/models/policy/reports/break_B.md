# break_B

NOTE: the harness refused to let this subagent write attack_B/REPORT.md ("Subagents should return findings as text"). I did not work around it with the shell. This output is the report. Everything else is in <scratch>/attack_B/. The shared policy_model.py is untouched (md5 3ed8fa4f503c6f4495373e0725a5ca41, same as policy_model.md5).

## Bottom line for policy B (per-stripe degrade)

**1. B never returned a silently wrong read without a mutation.**
- Scope: 91 exhaustive runs, 46 of them under B or CB, at depth 6-12 with up to 0.9 M states each; plus 23 runs of a new same-schedule driver, 38,731,070 schedules in total.
- Fresh-value re-runs, in which a stale copy can never alias a new value, also give 0.
- B's silence rests on two read-side refusals:
  - the torn-read refusal (raid56.c:3337, btrfs_wib_unrecovered at raid56-wib.c:4418);
  - the §3.5 suspect verdict (scrub_raid56_mark_suspect, scrub.c:5122).
- Switch off either one and exactly B's losing schedules become silent:
  - schedules (G1 k=5): 3,428 of 3,428;
  - exhaustive states: 27,956 with torn reads trusted, 27,670 without §3.5.
- A in the same single-fault family: 0 and 0.
- B also becomes silent as soon as it cannot keep its record:
  - B built on today's eviction rules (B+CE) is silent in 896 RAID5 schedules where today's kernel (CUR) is not;
  - a B that also "never refuses" when the log is full (B+EN) is silent in 150,228 of 544,942 cap-1 schedules, with every disk present.

**2. B loses acknowledged data from every disk with ONE failing disk and ONE crash, and nothing else.** The loss is loud (EIO).
- Mechanism:
  - a write to the failing disk fails; the value is acknowledged and lives only in parity;
  - a later write to another column of the same stripe goes ahead under B (degrade) and rewrites that parity;
  - a crash between that write's data and parity writes destroys the only copy.
- B treats a present, failing disk as missing for that stripe. The classic degraded write hole needs crash + disk loss at the same moment; B turns it into crash alone.
- Same-schedule counts, RAID5, one failing disk, at most one crash, all disks at the mount:
  - k=7: B loses (GONE) in 212,862 of 3,717,550 schedules (5.7%; 8.0% of crash outcomes);
  - k=8: 1,540,844 of 24,992,686 (6.2%);
  - A: 0 and 0. A+NI: 0 losses of any kind.
- The same 212,862 schedules for checksummed data.
- Metadata (k=6): B loses tree blocks in 28,858 of 544,942 schedules. A goes read-only in 103,018 of them and loses nothing.
- Exhaustive E1 (one failing disk, 2 crashes, d12): B 10,004 GONE states, every one carrying the degrade history bit; A 0.

**3. C (md-style: fail the disk at its first write error) and CB lose in exactly the same schedules.**
- In G1, B = B+NI = C = C+NI = CB = 212,862 GONE.
- C also has more UNREACH (436,962 vs 212,862), and more GONE when the failing disk dies at the crash (G2x: C 44,772, B 27,990, A 0).
- Running degraded on a failed disk IS B's hole.
- What closes it:
  - refusal (A, or B+SD, which equals A);
  - C+SD+NI: loss-free in G1 but refuses 165,520 vs A+NI's 103,018 schedules (k=6; +61%).
- NI alone does not help: B+NI loses in as many schedules as B.

**4. Other B classes (all loud):**
- B-3, crash inside the tree-log replay: a new model switch (--crash-in-replay; the shared model never crashed a replay).
  - A: the mount fails and the data is kept.
  - B: the mount fails AND the degraded column's acknowledged value is gone. E8: B 7,492 GONE states, A 0.
- B-4, RAID6, failing disk + crash + one more device lost (within the two-device tolerance):
  - G3 k=5: B loses where A does not in 5,378 schedules (k=6: 33,190);
  - E3: B 50,212 GONE states at d8, of which 43,818 carry the degrade bit.
- B-5, RAID6 with two failing disks + one crash: B 5,820 schedules, A 0 (G5).
- B-6, no crash at all:
  - a failed flush on another disk during a degrade write makes that write fail with EIO, and it destroys the degraded column's earlier acknowledged value;
  - exhaustive, no crash, d7: B 1,304 GONE, A 0, C 1,580.
- Failing disk leaves and returns (G4, E6): no new mechanism. Every B-only loss involving detach/return is B-1 again (B loses where A does not in 6,939 G4 schedules).
- Mixed csum/nodatasum columns (E11): B 12,440 GONE, A 0.

**5. Kernel mapping and decision.**
- B is not in the kernel. The write is refused at raid56.c:4743 in rmw_repair_first(), called at 4977, after one retry.
- B IS the failed-disk design's §2.1, "Phase A never refuses":
  - it applies before the trigger (CB);
  - §1.2 lists phase-A failures as "not triggers ... degrade the write instead of refusing it";
  - its knob raid56_wf_phase_a_refuses=1 restores refusal.
- Under the user's rule, stage 1 should ship with raid56_wf_phase_a_refuses=1 as the default.
- The cost is the design's own measured control (S1-01): RAID5/6 metadata goes read-only 0.01-27 s after a disk starts failing writes, 8/8 arms.
- The debug knob raid56_rmw_single_phase (raid56.c:4624, "restores a known defect") reproduces B's class on today's kernel: the write-back goes out with phase B.
- UML rmw_torn.sh's control arm already shows the resulting loss. A B-specific arm is proposed below but was not run.

**6. Side finding (not B).**
- E7 (a failing disk plus a failing flush on another disk, RAID5) gave C 12,834 silent states and CB 10,361.
- Cause, in the shared policy_model.py: when T-flush fails one device, the model stops naming EVERY flush-lost column of that commit. That includes one on a second device the design would refuse to admit (btrfs_check_rw_degradable), and would then name via HEAD's readd.
- With my --fix-cflush switch, C has 0 silent states (E7c_C_d6: 3,542, down to 0).
- It needs two flush-failing devices. No family in the builder's matrix has that, so its C numbers stand; the builder should apply the fix.

**7. A, CUR and A2 in these runs.**
- No new class. A's losses are the builder's A-1 (UNREACH, removed by NI) and A-2 (the degraded write hole, GONE).
- CUR's silent states under cap 1 are the builder's CUR-1/2/5. B+CE adds silent RAID5 schedules on top of CUR-2: B tears the stripe, today's rules then evict the verdict.
- A2 was never silent in the cap-1 grammars.

## Method

**b_model.py** is a copy of policy_model.py plus:
- --crash-in-replay;
- --hist history bits (B degrade / RMW over an absent column / write while FAILED), with loss metrics split by them;
- --bad-devs / --detach-devs for targeted fault schedules (depth 11-12);
- --mut cur_evict (today's eviction rules on any policy);
- --fix-cflush.

Regression: 6 builder runs re-run with no new switch give identical counts.

**sched.py** (new) is the same-schedule driver.
- Why: state counts depend on each policy's state-space growth, so they cannot be compared across policies.
- It fixes the adversary's policy-independent choices:
  - fault injection;
  - writes and repairs;
  - a crash during a given write, plus the device set at the next mount;
  - device loss/return, scrub, replace.
- It enumerates every crash point, every landed subset of phase-B writes and every eviction choice.
- A disk may die whatever the policy: beyond the tolerance, the mount is ro,degraded.
- C and CB fail a disk at its first write error, as md does.
- Grammars:
  - G1: RAID5, one failing disk, at most one crash;
  - G2x: the failing disk itself dies at the crash;
  - G2o: another disk dies at the crash (a RAID5 double fault);
  - G3: RAID6, failing disk + one device lost;
  - G4: failing disk leaves and returns, persistent or healed;
  - G5: RAID6, two failing disks;
  - c1: cap-1 variants with CUR, A2, B+CE, B+EN;
  - K: controls.
- k = 5-8 operations after the fault.

**walk.py** prints any schedule step by step for several policies, for readable traces.

All runs used nice -n 10, at most 25-28 min each, with a 2.5 GB memory guard (T = stopped by it).

## Table

### Same schedules (schedules whose worst outcome is in the class; nodatasum unless stated)

GONE = the acknowledged value is on no disk. UNR = on disk but unreachable. REF = a write got EIO. RO = read-only or the mount failed. SILENT = 0 in every cell of this table.

| grammar (schedules) | A | A+NI | B | B+NI | C (md trigger) | C+NI | CB |
|---|---|---|---|---|---|---|---|
| G1 RAID5, 1 failing disk, <=1 crash, k=7 (3,717,550) | GONE 0 / UNR 425,724 / REF 635,986 | 0 loss / REF 898,794 | **GONE 212,862** (8.0% of crash outcomes) / UNR 212,862 / REF 21,624 | GONE 212,862 / UNR 0 | GONE 212,862 / UNR 436,962 | GONE 212,862 / UNR 112,050 | = C |
| G1 k=8 (24,992,686) | GONE 0 / UNR 3,081,688 | 0 loss / REF 7,243,754 | **GONE 1,540,844** | - | GONE 1,540,844 / UNR 2,964,588 | - | - |
| G1 csum k=7 (3,717,550) | 0 loss / REF 898,794 | 0 loss | GONE 212,862 | 212,862 | 212,862 | 212,862 | = C |
| G1 metadata k=6 (544,942) | **0 loss / RO 103,018** | 0 loss / RO 103,018 | **GONE 28,858** / RO 0 | 28,858 | 28,858 | 28,858 | = C |
| G2x failing disk dies at the crash k=6 (544,942) | GONE 0 / UNR 55,980 | 0 loss | GONE 27,990 | 27,990 | GONE 44,772 | 44,772 | = C |
| G2o another disk dies at the crash (RAID5 double fault) k=6 (977,910) | GONE 141,320 | 70,660 | 141,320 | 141,320 | GONE 230,864 / RO 321,248 | 115,432 | = C |
| G3 RAID6, failing disk + 1 device lost k=5 (127,130) | GONE 2,538 / UNR 12,006 | GONE 2,262 | GONE 7,916 / UNR 9,440 | 5,590 | GONE 7,916 / UNR 19,816 | 7,640 | = C |
| G3 k=6 (675,994) | GONE 16,614 | 14,602 | GONE 49,804 | - | GONE 49,804 | - | - |
| G3 csum k=5 | GONE 522 | 522 | 1,248 | 1,142 | 1,248 | 1,142 | = C |
| G4 failing disk leaves/returns k=6 (241,203; transient identical) | GONE 5,882 (A-2) / UNR 21,620 | 5,882 | GONE 12,821 | 12,821 | 17,943 | 17,943 | = C |
| G5 RAID6, 2 failing disks k=5 (117,573) | GONE 0 / UNR 12,624 | 0 loss | GONE 5,820 | 3,428 | 5,820 | 5,820 | = C |

**Pairwise: B loses the value from every disk and the other policy does not**

| grammar | vs A | vs A+NI |
|---|---|---|
| G1 k7 | 212,862 | 212,862 |
| G1 k8 | 1,540,844 | 1,540,844 |
| G2x | 27,990 | 27,990 |
| G3 k5 | 5,378 | 5,654 |
| G3 k6 | 33,190 | 35,202 |
| G4 | 6,939 | 6,939 |
| G5 | 5,820 | 5,820 |

The reverse (A loses, B does not) is 0 in G1, G2x, G4 and G5.

**Strict variants**

| grammar | A+NI | B | B+SD | C | C+SD+NI | CTM+SD+NI |
|---|---|---|---|---|---|---|
| G1 k6: GONE / UNR / REF | 0 / 0 / 103,018 | 27,990 / 27,990 / 1,736 | 0 / 55,980 / 74,298 | 27,990 / 61,554 / 3,224 | **0 / 0 / 165,520** | 0 / 0 / 165,520 |
| G2x k6: GONE / REF | 0 / 76,206 | 27,990 / 46,508 | - | 44,772 / 38,276 | 0 / 202,232 | 28,140 / 182,828 |
| G3 k5: GONE / UNR / REF | 2,262 / 1,308 / 25,524 | 7,916 / 9,440 / 8,656 | - | 7,916 / 19,816 / 7,488 | 1,184 / 3,492 / 59,432 | 2,584 / 12,276 / 53,648 |

**Cap-1 log: SILENT schedules**

| grammar | A | CUR | A2 | B | B+CE (B on today's eviction rules) | B+EN (B that never refuses) | C | CTM |
|---|---|---|---|---|---|---|---|---|
| G1c1 k6 (544,942) | 0 | 0 | 0 | 0 | 0 | **150,228** | 0 | 0 |
| G2c1 k5 (197,822) | 0 | 3,080 | 0 | 0 | **3,976** (896 where CUR is not) | **51,828** | 0 | 0 |
| G3c1 k5 (127,130) | 0 | 17,124 | 0 | 0 | 17,124 (same schedules as CUR) | **30,488** | 0 | 0 |

### Exhaustive runs (distinct states; SIL / GONE / UNREACH; T = memory-stopped at that depth)

GONE_DEG = GONE states whose history contains a B degrade.

| family | A | B | C | CB |
|---|---|---|---|---|
| E1 1 failing d0, 2 crashes, repair/replace/scrub, d12 | 0 / 0 / 23,100 (44,080 st) | 0 / 10,004 (GONE_DEG 10,004) / 47,202 (95,266 st) | 0 / 10,852 / 162,962 | 0 / 65,832 / 320,399 |
| E1 csum d12 | 0/0/0 | 0 / 8,660 / 0 | 0 / 8,600 / 0 | 0 / 31,948 / 0 |
| E1 B+NI / A+NI d12 | A+NI 0/0/0 | B+NI 0 / 10,268 / 9,920 | - | - |
| E9 metadata d11 | 0 / 0 / 0, RO 3,380 | 0 / 1,056 / 0, RO 528 | 0 / 1,052 / 0 | 0 / 3,924 / 0 |
| E8 crash inside replay d10 | 0 / 0 / 12,540, DOWN 4,608 | 0 / 7,492 (all DEG) / 25,150 | 0 / 7,716 / 87,748 | 0 / 42,296 / 167,176 |
| E6 failing disk leaves/returns, persistent d11 | 0 / 7,024 (A-2) / 45,058 | 0 / 18,588 (DEG-only 5,292) / 80,772 | 0 / 16,776 / 116,066 | 0 / 44,368 / 206,980 |
| E6 transient d11 | 0 / 8,592 / 75,468 | 0 / 25,804 / 138,810 | 0 / 24,212 / 188,092 | 0 / 66,344 / 335,110 |
| E7 failing disk + failing flush elsewhere, d7T | 0 / 0 / 144,125 | 0 / 12,478 (all DEG) / 122,110 | **12,834** (model artifact, 0 with --fix-cflush) / 22,600 | 10,361 (same artifact) / 27,491 |
| E7 no crash, d7 (with --fix-cflush) | 0 / 0 / 11,340 | 0 / **1,304** / 11,948 | 0 / 1,580 / 16,335 | - |
| E3 RAID6 failing + missing | d9: 0 / 8,188 / 114,860 | d8T: 0 / 50,212 (DEG 43,818) / 192,130 | d8T: 0 / 28,420 / 266,704 | d7T: 0 / 46,478 / 216,391 |
| E5 RAID6 two failing, nodatasum | d8T: 0 / 0 / 51,176 | d7T: 0 / 9,010 (all DEG) / 69,584 | d6T: 0 / 2,028 / 87,313 | d6T: 0 / 5,210 |
| E5 csum | d9: 0 / 0 / 0 | d8T: 0 / 15,016 / 0 | d7T: 0 / 3,336 | d7T: 0 / 13,276 |
| E11 mixed csum/nodatasum d11 | 0 / 0 / 15,388 | 0 / 12,440 (all DEG) / 31,442 | 0 / 12,924 / 106,180 | 0 / 63,620 / 207,711 |
| E14 persistent, cap 1, d11 | 0 / 0 / 704 (CUR same) | 0 / 224 / 1,408; B+CE same; **B+EN SIL 43,733** | 0 / 528 / 5,872 | - |
| E13 RAID6 failing + missing, cap 1, d7-8 | 0 / 1,708; **CUR SIL 283,986** | 0 / 9,576; **B+CE SIL 243,746; B+EN SIL 263,302** | 0 / 31,496 | - |
| E12 RAID5 3 stripes, cap 2, failing + missing + return, d6-7T | 0 / 107,841; **CUR SIL 42,728** | 0 / 120,005; **B+CE SIL 33,321; B+EN SIL 78,035** | SIL 27,699 (C-3 / ERRATA 2) | - |
| F fresh values (persist d11 / RAID6 d7-8 / return d10) | SIL 0 / 0 / 0 | SIL 0 / 0 / 0; GONE 9,936 / 47,386 / 22,780 | SIL 0 | SIL 0 |

## Counterexamples

### B: LOST_ACKED (GONE: on no disk; loud EIO) (loud loss of acknowledged data from a single disk fault plus a crash. 212,862 of 3,717,550 schedules at k=7; 1,540,844 of 24,992,686 at k=8. A and A+NI: 0. Exhaustive E1 d12: 10,004 states, all with the degrade bit.)

Scenario: B-1: RAID5, one disk failing writes, one crash, all disks present at the mount (G1, E1, E6, E7, E11)

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed): P:=(2,1) lands, the record names d0, d0 still holds 1, so 2 lives only in P || write s0.d1=2: phase A must write d0's rebuilt 2 back and d0 refuses. Under B, 'phaseA {d0} refused->degrade', and phase B writes d1=2 and P:=(2,2) ; phaseB landed {d1} (or {P}) ; CRASH ; mount -> disk d0=1, d1=2, P=(2,1) (or d1=1, P=(2,2)); record T[0]; recovery: torn + named -> undecidable, reads of s0.d0 EIO; 2 is on no disk. Under A, the second write is REFUSED(phaseA {d0} write-back failed) and P keeps 2. A+NI loses nothing on any schedule of G1.

Kernel: Not in today's kernel: rmw_repair_first() refuses at raid56.c:4743 (called from rmw_rbio at 4977, after one retry). B is exactly the failed-disk design's §2.1 'Phase A never refuses' (CB); its knob raid56_wf_phase_a_refuses=1 restores the refusal. The debug knob raid56_rmw_single_phase=1 (raid56.c:4624) reproduces the class today; UML rmw_torn.sh's control arm shows the loss.

### B: LOST_ACKED (GONE) of checksummed metadata (metadata loss (filesystem damage). B: 28,858 of 544,942 schedules (E9: 1,056 states). A: 0 loss, read-only in 103,018 schedules.)

Scenario: B-2: metadata RAID5, one failing disk, one crash (G1 --meta, E9)

Trace: The B-1 trace with metadata writes. Under B, the tree block's acknowledged value is gone: detected by its checksum, but lost. Under A: write s0.d1=2 REFUSED(phaseA {d0} write-back failed) -> the filesystem goes read-only, nothing is lost.

Kernel: Today the kernel refuses (raid56.c:4743) and metadata goes read-only. The design's S1-01 control measured this as read-only 0.01-27 s after the fault (8/8 arms). §2.1 would turn it into B's loss.

### B: LOST_ACKED (GONE) + mount fails (worse than A's mount failure: the mount fails AND data is gone. E8 d10: B 7,492 GONE states (all degrade-attributed), A 0.)

Scenario: B-3: crash inside the tree-log replay's own RMW (new --crash-in-replay; E8, walk.py)

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || fsync s0.d1=2 (tree log) ; CRASH ; mount ; replay s0.d1=2 phaseA {d0} refused->degrade ; phaseB landed {d1} ; CRASH ; mount ; replay s0.d1=2 REFUSED(undecidable-torn) -> mount-failed, disk s0=[1,2], P=(2,1): d0's acknowledged 2 is gone. Under A: replay REFUSED(phaseA {d0} write-back failed): the mount fails but P still holds 2, and pulling the disk (A2) or a replace gets past it.

Kernel: The replay writes go through the same RMW (btrfs_recover_log_trees, BTRFS_FS_LOG_RECOVERING tree-log.c:7755); today it is refused at raid56.c:4743.

### B: LOST_ACKED (GONE) (B loses where A does not in 5,378 of 127,130 schedules (k=5) and 33,190 of 675,994 (k=6). E3 d8: 50,212 GONE states, 43,818 with the degrade bit.)

Scenario: B-4: RAID6, failing disk + crash + one more device lost (within the two-device tolerance) (G3, E3)

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed): P=Q=(2,1), d0 holds 1 || write s0.d1=2 phaseA {d0} refused->degrade ; phaseB landed {d1} ; CRASH ; mount without P -> Q=(2,1) no longer matches d1=2: d0's 2 is gone. Under A the second write is refused; with P lost, Q and d1=1 rebuild 2.

Kernel: Not in today's kernel (raid56.c:4743 refuses). Design §2.5/§3.4 plus §2.1 would allow it.

### B: LOST_ACKED (GONE) (G5: B 5,820 of 117,573 schedules, A 0. E5: B 9,010 (nodatasum) and 15,016 (csum) states, all degrade-attributed; A 0.)

Scenario: B-5: RAID6, two failing disks + one crash (G5, E5)

Trace: d0 and P fail writes || write s0.d0=2 ok: d0 and P failed, only Q=(2,1) holds 2 || write s0.d1=2 phaseA {d0} refused->degrade ; phaseB landed {Q} ; CRASH ; mount -> Q=(2,2), d1=1: 2 is gone. A refuses the second write.

Kernel: Not in today's kernel (raid56.c:4743).

### B: LOST_ACKED (GONE) with no crash (a write that fails with EIO destroys a DIFFERENT, earlier acknowledged value. d7: B 1,304 states, A 0, C 1,580.)

Scenario: B-6: RAID5, failing disk + a failed flush on another disk (two faults), no crash (E7nc)

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed): P=(2,1) || d1 starts failing flushes || write s0.d1=2: B degrades d0, phase B lands d1 and P=(2,2); the commit's flush loses d1's write -> REFUSED(tolerance {d0,d1}) (EIO to the caller), but now P=(2,2) and d1=1: d0's acknowledged 2 is gone. Variant: P's flush is lost, so P=(2,1) with d1=2.

Kernel: Not in today's kernel: A refuses before touching P (raid56.c:4743).

### B+CE (B on today's eviction rules): SILENT_WRONG (silent wrong read (alert raised earlier). 896 schedules where CUR is not silent. In RAID6 cap 1 (G3c1), B+CE and CUR are silent in exactly the same 17,124 schedules.)

Scenario: B-7: RAID5 cap 1, failing d0 dies at the crash, then a write into a new region (G2c1)

Trace: d0 fails writes ; write s0.d0 (fails; 2 only in P) ; write s0.d1 + CRASH with d0 dying at the crash (mount without d0): B's degrade tore s0, so recovery gives it a verdict ; write s1.d0: the log is full and a device is missing, so today's rules evict s0's verdict (record_dropped alert) -> read of s0.d0 rebuilds from P=(2,2) and d1=1: garbage, no error. CUR refuses 'write s0.d1', nothing is torn, and CUR is OK on this schedule.

Kernel: The eviction is today's: wib_may_evict_naming() (raid56-wib.c:716-726, true while a device is missing, 724), wib_evict_sticky() pass 3 (847), reached from wib_find_or_alloc_entry() (982). This is the builder's CUR-2; B feeds it more torn stripes.

### B+EN (B that also never refuses on a full log): SILENT_WRONG (silent: 150,228 of 544,942 schedules. E14 d11: 43,733 states.)

Scenario: B-8: cap 1, every disk present (G1c1, E14)

Trace: d0 fails writes ; write s0.d0 (fails; record names d0) ; write s1.d0: the log is full, and instead of refusing, the record naming d0 is evicted -> read of s0.d0 returns d0's stale 1 with no error.

Kernel: Hypothetical: B's 'keep writing' cannot go past a full log without this. The same eviction exists today only while degraded or during replay (CUR-1/3).

### A: LOST_ACKED (UNREACHABLE; data still in P) (loud; 425,724 of 3,717,550 G1 schedules; A+NI: 0.)

Scenario: A-1 (builder's): crash after the whole-stripe mark of a write that phase A then refuses (G1)

Trace: bad d0 ; write s0.d0 (fails; 2 in P) ; write s0.d1 ; CRASH after mark ; mount -> record T[0] makes s0 undecidable, reads of s0.d0 get EIO forever, although P=(2,1) still describes 2.

Kernel: Applies: btrfs_wib_mark() at raid56.c:4939 runs before rmw_repair_first() at 4977. Recovery plans SCRUB_WIB_TORN (scrub.c:3523), scrub_raid56_mark_suspect (5122), and the read refusal at raid56.c:3337. NI would reorder this; it is not implemented.

### A (all log policies): LOST_ACKED (GONE) (loud; A 2,538 (G3) / 522 (G3 csum) / 5,882 (G4) schedules; every policy has it.)

Scenario: A-2 (builder's degraded write hole) as it appeared in G3, G3 csum and G4

Trace: (i) G4: bad d0 ; write s0.d0 ; detach d0 ; write s0.d1+CRASH -> d0's 2 lived only in P, now torn. (ii) G3 RAID6: bad d0 ; write s0.d0 ; write s0.d0+CRASH (mount without d1) -> P=(3,1), Q=(2,1): d1's untouched value cannot be rebuilt, because d0's column is stale. (iii) G3 csum: bad P ; detach d0 ; write s0.d0 ; write s0.d1+CRASH -> two absent columns on RAID6 and a torn Q.

Kernel: Applies: degraded RMWs and writes into a failing column are allowed. Recovery keeps and classifies the stripe (scrub.c:5767, 5385, 5122), so reads get EIO, not a guess. This is the documented [LIMITS] of raid56-wib.c. SD closes (i) and (iii), not (ii) (a crash and a disk loss at the same moment).

### C / CB: LOST_ACKED (GONE): the same hole as B (exactly B's schedules in G1 (212,862); more when the failing disk dies at the crash (G2x: 44,772 vs B 27,990). C+SD+NI: 0 loss, at 61% more refusals than A+NI.)

Scenario: G1, G2x, G5, E1 (C fails d0 at its first write error, as md does)

Trace: bad d0 || write s0.d0=2 (fails -> trigger fail d0) || write s0.d1=2 (degraded: rebuild d0 from P, write d1 and P) ; phaseB landed {d1} ; CRASH ; mount -> record TV[2], 2 is gone.

Kernel: Design stage 1 (not built). This is the builder's C-1.

### C / CB (model artifact): SILENT_WRONG (12,834 states (C), 10,361 (CB); 0 with --fix-cflush.)

Scenario: E7: two flush-failing devices on RAID5

Trace: d0 starts failing flushes || d1 starts failing flushes || write s0.d1=2 -> T-flush fails d0; d1's lost write is NOT named, because the shared model skips all flush-lost naming once a T-flush fired; REFUSED(tolerance) -> read of s0.d0 = rebuild from P=(1,2) and d1=1: garbage.

Kernel: Not a design hole. The design refuses admission beyond the tolerance (btrfs_check_rw_degradable, design §1.3), so HEAD's readd names d1. The fix belongs in policy_model.py (the 'if P not in (C,CB,CTM) or not tflush_failed' branch in rmw_body). The builder's matrix has no family with two flush-failing devices, so its numbers are unaffected.

## Sensitivity

**Regression.** b_model.py with no new switch reproduces 6 builder runs exactly (out_regress/): M_persistent_full_B, M_meta_persistent_full_B, M_replay_full_B, M_missing_return_full_CUR, M_cap2_missing_return_C, M_flush_unnamed_CUR. The only difference is the added LOST_GONE_NOHIST column.

**Schedule-driver controls.** Every class the driver must detect shows up (K_G1 k=5 / K_G2 k=4):

| control | SILENT schedules |
|---|---|
| OLD | 31,454 / 14,010 |
| B without its record (b_no_record) | 8,234 / 684 |
| B with torn reads trusted (read_trust_torn) | 3,428 = exactly B's 3,428 GONE schedules |
| B without §3.5 (no_suspect) | 3,428 / 5,524 |
| A without §3.5 | 5,148 (G2) |
| A with torn reads trusted | 0 (G1) |
| B that never refuses on a full log (evict_naming, G1c1) | 150,228 |
| B on today's eviction rules (cur_evict, G2c1) | 3,976 |

**Exhaustive dependency controls** (persistent family, any failing device, d10):

| control | SILENT states |
|---|---|
| B + read_trust_torn | 27,956 |
| A + read_trust_torn | 0 |
| B + no_suspect | 27,670 |
| A + no_suspect | 0 |
| B + b_no_record | 169,546 |
| no_suspect with the failing disk leaving and returning | A 15,096, B 32,610 |

So B's non-silence depends on the torn-read refusal (raid56.c:3337) and on §3.5 (scrub.c:5122). A's does not, in single-fault schedules.

**History attribution.** In E1, E5, E8, E9 and E11, every B GONE state carries the B-degrade history bit (GONE_DEG = GONE). A's GONE states never do; they carry the degraded-RMW-over-a-missing-device bit (A-2).

**New switches visibly change results:**
- --crash-in-replay produces B-3. Without it no replay can tear, as in the builder's replay_full.
- --fix-cflush removes C's E7 silent states (E7c_C_d6: 3,542, down to 0). This shows the class is visible and that the switch is what removes it.

**Fresh values** (--fresh-values, no aliasing): B SILENT 0 in the persist d11, RAID6 bad+missing d7-8 and failing-disk-returns d10 families. Its GONE counts are of the same size as without fresh values (9,936 vs 10,004 at d11/d12).

**Driver fairness.** A first pass counted C as OK where its model forbade the adversary's disk loss (a FAILED disk counts toward the tolerance). That pass is kept in sched_out_v1/ and is not used. The fixed driver lets a disk die whatever the policy (ro,degraded beyond the tolerance). It also applies the crash after a write refused before its mark. G1 was unchanged by the fix.

## Caveats

- **REPORT.md was not written.** The harness blocks report files from subagents, and I did not work around it through the shell. This structured output is the report.
- **Model scope** is the builder's:
  - one sector per column, 2 data columns, 2-3 full stripes;
  - atomic scrub and replace;
  - a failed flush loses that device's writes of the operation;
  - RAID6 Q is a snapshot with parity-set-dependent garbage;
  - the admin closure is at most 3 actions.
- **B is the builder's B.** It degrades only a phase-A write-back that the stripe's own disk refuses. Repairs and a full log still refuse. B+EN is the literal "never refuse" reading; B+CE is B built on today's eviction rules.
- **The C trigger in the schedule driver is md-style** (fail at the first write error). The design triggers later (T-log at about 2 s, T-repair at about 63 s) and runs §2.1, i.e. B, until then. So CB is the design as written; in G1, CB = C = B.
- **Schedule counts are not field probabilities.** They weight every operation sequence equally and serve to compare policies on the same inputs. What matters is zero vs non-zero and the pairwise columns. B-1 needs a failing disk, a later write into the same full stripe, and a crash during that write's device writes.
- **Search bounds and truncation:**
  - several exhaustive runs stopped on the 2.5 GB memory guard (marked T; counts are exact up to that depth): E3 B/C/CB, E5 A/B/C/CB (nodatasum), E7 all, E12/E13 most;
  - I cancelled the planned deeper E3 run of B (d9, 5.5 GB) to keep the total time bounded; RAID6 is covered by E3 d8, F_fresh RAID6 d7 and G3 up to k=6;
  - the machine was shared (load up to 6), and every run used nice 10.
- **No kernel reproduction.** Nothing was run on a UML kernel; the kernel under /home/user was only read. Proposed B-specific arm (not run):
  - take rmw_repair.sh's prep (a device fails writes under nodatacow overwrites of 'B');
  - with the device still failing, write 'C' into those full stripes with raid56_rmw_single_phase=1 and raid56_crash_point=1 (P/Q never reach the disk, raid56.c:1817; panic at 5013), then mount rw;
  - expected with the knob: 'B' EIO and on no platter;
  - expected without it (today's kernel): the 'C' write gets EIO and 'B' stays intact in P.
- **Kernel line numbers** are at BTRFS-s0 stage0-wip 2a1e2379e4 (kcite.sh). The design's line numbers are stale per its own ERRATA 7; I cite the design by section.
- **G4 persistent and transient gave identical tables.** With replace always available in the admin closure, heal adds nothing.
- **The C/CB silent states in E7 are a shared-model artifact**, explained above (--fix-cflush). The shared policy_model.py should get the same correction if anyone extends its matrix to two flush-failing devices.

## Model files

- <scratch>/attack_B/b_model.py
- <scratch>/attack_B/policy_model.orig.py
- <scratch>/attack_B/sched.py
- <scratch>/attack_B/walk.py
- <scratch>/attack_B/run_jobs.sh
- <scratch>/attack_B/run_sched.sh
- <scratch>/attack_B/summ.py
- <scratch>/attack_B/stable.py
- <scratch>/attack_B/jobs1.txt
- <scratch>/attack_B/jobs2.txt
- <scratch>/attack_B/jobs3s.txt
- <scratch>/attack_B/jobsD.txt
- <scratch>/attack_B/sjobs2.txt
- <scratch>/attack_B/sjobs3.txt
- <scratch>/attack_B/sjobs4.txt
- <scratch>/attack_B/out/
- <scratch>/attack_B/out_regress/
- <scratch>/attack_B/sched_out/
- <scratch>/attack_B/sched_out_v1/
